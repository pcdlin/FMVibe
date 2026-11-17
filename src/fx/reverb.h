#pragma once

#include "common.h"

#include <limits>

namespace fmvibe {

// The reverb's decay (seconds to fall 60 dB) and pre-delay (seconds) from their params.
double reverbDecay(double norm);
double reverbPreDelay(double norm);
// How each reverb algorithm sets up the network: its lines' length against the hall's, how much its damping
// lets through against the knob's, how many all-passes diffuse the input on each side, how loud its early
// reflections are, how far its lines' lengths waver against the hall's, and whether it feeds its tail back
// pitch-shifted (a shimmer).
struct ReverbStyle {
    double length;
    double brightness;
    int diffusers;
    double early;
    double wander;
    bool shimmer;
};
ReverbStyle reverbStyle(ReverbType type);
ReverbType reverbType(double value);
// The lines' length against the hall's longest, from the Size knob and the algorithm.
double reverbSpace(double sizeNorm, ReverbType type);
// A room's early reflections (seconds at full size, after the pre-delay) and their levels.
inline constexpr double kRoomEchoSeconds[] = {0.0071, 0.0113, 0.0167, 0.0229, 0.0311, 0.0409};
inline constexpr double kRoomEchoLevels[] = {0.55, 0.45, 0.38, 0.3, 0.24, 0.18};
// How much faster the tail decays above the damping's corner than the Decay knob says, the same for every line.
inline constexpr double kReverbHighDecay = 0.2;
// The lines' longest length at full size (seconds).
inline constexpr double kReverbLongest = 0.0889;

namespace fx {

// A feedback delay network: kReverbLines lines of different lengths, mixed through a Hadamard matrix
// (which keeps their energy and spreads each line's evenly over all of them), each with the gain that makes
// it fall 60 dB in the decay time and a high shelf that takes it down faster above the damping's corner, by as
// much for every line. The lines' lengths waver slowly, each at its own pace, so the tail's resonances
// smear rather than ring; each line is read between samples through a first-order all-pass, which passes every
// frequency at its level, where a straight line between two samples would dull the top a little more each time
// round. Each side of the input goes through the pre-delay and a few all-passes that diffuse
// it, then into half the lines; the lines' outputs, signed differently, make the two sides. A shimmer
// band-passes each side of its tail, pitch-shifts it and feeds it back into the lines (past the all-passes), so
// what goes round climbs (or falls) by the interval each time.
constexpr int kReverbLines = 16;
constexpr int kDiffusers = 6; // per side: a plate runs the input through all of them, the others through four

// What every reverb at a rate shares, worked out once for it (see Effects::prepare()).
struct ReverbTables {
    std::array<double, kReverbLines> lengths{}; // at full size, in samples
    double meanLength = 0.0; // how long the lines are on average, for their rounds per second (harmonic mean)
    std::array<std::array<int, kDiffusers>, 2> diffuserLengths{};
    std::array<std::array<double, 2>, kReverbLines> wanderTurn{}; // each line's wavering: cos and sin of its step

    void prepare(double sampleRate);
};

struct Reverb {
    std::array<ZeroBuffer, kReverbLines> lines; // all as long as the longest, with room for the wavering
    std::array<std::array<ZeroBuffer, kDiffusers>, 2> diffusers; // per side
    std::array<ZeroBuffer, 2> preDelay;
    std::array<ZeroBuffer, 2> shifter; // a shimmer's band-passed tail, per side, its shifters read
    int lineWrite = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    int diffuserWrite = 0;
    int preWrite = 0;
    int shiftWrite = 0;
    // The diffusers the algorithm ran last. Those it left out stood still, holding the sound of then, and are
    // cleared when one taken up again (see run()), as the shifters are when a shimmer starts again.
    int diffusersUsed = kDiffusers;
    std::array<double, kReverbLines> damp{}; // the shelves' low-pass states
    std::array<double, kReverbLines> lineRead{}; // each line's all-pass read, its last output (see run())
    // The lines' wavering: a sine each, as a turning phasor.
    std::array<double, kReverbLines> wanderCos{}, wanderSin{};
    // Shimmer: the shifters' read heads sweep the window (see kShimmerWindow) at a phase both sides share, the
    // right side's a quarter of a cycle on. What they read was band-passed on its way in: a low-pass, and what
    // a one-pole lets through below the high-pass corner, taken away.
    double shimmerPhase = 0.0;
    std::array<std::array<Biquad, 2>, 2> shimmerLow{}; // two in a row a side
    std::array<double, 2> shimmerBass{};
    double size = 1.0;      // the lines' length scale, gliding toward the knob
    double preLength = 0.0; // samples, gliding
    double reach = 0.0;     // samples before what goes in has all come out of the pre-delay (see run())
    double dry = 1.0;       // ramped
    double wet = 0.0;
    double width = 1.0;
    double shimmer = 0.0;   // the gain the shifted tail goes back in at, ramped
    // Each line's gain and its shelf's (see run()), and the size, share of the tail fed back shifted and
    // decay they were worked out for: NaN until the first chunk.
    std::array<double, kReverbLines> gains{}, highs{};
    std::array<double, 3> gainsFor{std::numeric_limits<double>::quiet_NaN()};

    // Allocates the lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so a tail keeps it running.
    double run(const Context &ctx, const ReverbTables &tables, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
