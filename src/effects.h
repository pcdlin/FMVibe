#pragma once

#include "analyzer.h"
#include "clipcurve.h"
#include "filter.h"
#include "params.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

namespace fmvibe {

// Crossovers of the DJ EQ.
constexpr double kEqLowHz = 250.0;
constexpr double kEqHighHz = 2500.0;

// Delay mix: the dry signal stays at full level up to 50 %, the echoes reach full level there.
inline double delayDryLevel(double mix) {
    return std::min(1.0, 2.0 * (1.0 - mix));
}
inline double delayWetLevel(double mix) {
    return std::min(1.0, 2.0 * mix);
}

// Magnitude response of the DJ EQ at `hz`, for band params (-1..1) low, mid and high.
double eqResponse(double hz, double low, double mid, double high);
// Magnitude response of one parametric EQ band at `hz`, from its params (frequency and Q normalized, gain in dB).
double peqResponse(double hz, PeqType type, double freq, double gainDb, double q);
// The phaser's and flanger's sweeps: the phaser's all-pass corner (Hz) and the flanger's delay
// (seconds) with their LFO at `lfo` (-1..1), from their params.
double phaserCorner(double centerNorm, double depth, double lfo);
double flangerDelay(double delayNorm, double depth, double lfo);
// How much the phaser's and flanger's feedback (-1..1 as set) sends round, and the gain that keeps
// their broadband level where it was without feedback.
double sweepFeedback(double feedback);
double sweepGain(double feedback);
// Magnitude responses, for drawing: the phaser with its corner at `cornerHz`, the flanger with its
// delay at `delaySeconds`. At 50 % mix their notches go deepest.
double phaserResponse(double hz, double cornerHz, int stages, double feedback, double mix);
double flangerResponse(double hz, double delaySeconds, double feedback, double mix);
// The phase disperser's all-pass (b0, b1, b2, a1, a2) at Freq (0..1 on the cutoff curve), Pinch setting its Q
// from 0.3 to 6, and how long the sound at `hz` comes out after the rest through `stages` of them (seconds).
std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate);
double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate);
// The phaser's and flanger's Mix knobs run to 100 %, which is half wet, where their notches go deepest: past that
// they would only get shallower again.
inline double sweepWet(double mix) {
    return 0.5 * mix;
}
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
// A shimmer's pitch shifters: two read heads each, sweeping a window this long (seconds) and crossfading as they
// jump back, in the lines they shift.
inline constexpr double kShimmerWindow = 0.08;
// How much faster the tail decays above the damping's corner than the Decay knob says, the same for every line.
inline constexpr double kReverbHighDecay = 0.2;
// The lines' longest length at full size (seconds): how long a shimmer's tail takes, at most, to go round once and
// climb its interval, shifted on the way.
inline constexpr double kReverbLongest = 0.0889;

// The widener's taps: how long each waits at full size (seconds) and how fast it is swept (Hz).
// The first and third go to the left side, the others to the right.
constexpr int kWidenerTaps = 4;
inline constexpr double kWidenerDelays[kWidenerTaps] = {0.0097, 0.0133, 0.0179, 0.0231};
inline constexpr double kWidenerRates[kWidenerTaps] = {0.29, 0.37, 0.43, 0.53};
// Tap `tap`'s delay (seconds) with its sweep at `lfo` (-1..1), from the Size knob (0..1): from a fifth of its
// length, a tight doubling, to all of it, swept 12 % either way.
double widenerTap(int tap, double size, double lfo);
// How much of the sound at `hz` the widener widens, from its High Pass knob (0..1 on the cutoff curve): the
// response of its high-pass, 12 dB per octave.
double widenerSpread(double hz, double highPassNorm);

// The effects chain on the sum of the voices: kFxSlots slots, each holding a
// shaper, delay, DJ EQ, parametric EQ, filter, compressor (single or three
// bands), reverb, phaser, flanger, frequency shifter, phase disperser or widener, or nothing, run in the order
// the FxOrder param picks. An empty or switched-off slot costs nothing;
// switching one fades it in or out over a few milliseconds. Audio thread
// only, apart from construction and prepare().
class Effects {
public:
    // Allocates the delay lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void reset();

    // In place. Reads the slot params and FxOrder from `p`; synced delays and sweeps use `tempo` (BPM), and a
    // filter's key tracking follows `pitch`, the newest voice's (semitones, 69 is A4).
    void process(float *left, float *right, uint32_t frames, const ParamValues &p, double tempo, double pitch);
    // Something is still to be heard after the input went silent: delay echoes, a reverb's tail.
    bool ringing() const { return tail_ > 0; }
    // How far a compressor in `slot` turns band `band` down right now, in dB (0 or more); a
    // single-band one uses band 0. The most of the latest block, for the editor's meters.
    float reduction(int slot, int band) const { return reduction_[slot][band]; }
    // The parametric EQs measure their output only while the editor can show it.
    void setAnalyzing(bool on) { analyzing_ = on; }
    // The level of what a parametric EQ or filter in `slot` puts out in spectrum band `band`, where a full-scale
    // sine reads 1: the most of the latest measurements, falling away about 55 dB a second.
    float spectrum(int slot, int band) const { return slots_[slot].analyzed.bands[band]; }
    // Where a phaser's, flanger's or widener's sweep is, in cycles (the widener's first tap's); 0 for other effects.
    float sweepPhase(int slot) const;

private:
    static constexpr uint32_t kChunk = 64; // samples per parameter ramp

    struct BiquadCoefs {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct Biquad { // transposed direct form II
        double z1 = 0.0, z2 = 0.0;

        double process(double x, const BiquadCoefs &c) {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    struct Shaper {
        std::array<ClipState, 2> sides{};
        double drive = 1.0; // gains, ramped
        double output = 1.0;
        double mix = 1.0;
    };

    struct Delay {
        std::array<std::vector<float>, 2> lines;
        int write = 0;
        std::array<double, 2> length{};   // samples, glide toward the set times
        std::array<double, 2> lowPass{};  // tone filter states
        std::array<double, 2> highPass{};
        double feedback = 0.0;
        double dry = 1.0;
        double wet = 0.0;
        double width = 1.0;
    };

    // Linkwitz-Riley crossovers: the low band gets an allpass so the three bands
    // sum back to a flat response.
    struct EqChannel {
        Biquad lowA, lowB, lowPhase, restA, restB, midA, midB, highA, highB;
    };

    struct Eq {
        std::array<EqChannel, 2> channels{};
        std::array<double, 3> gains{1.0, 1.0, 1.0};
    };

    struct ParamEq {
        std::array<std::array<Biquad, 2>, kPeqBands> bands{}; // per band, per channel
    };

    struct FilterFx {
        std::array<Filter, 2> channels{};
        std::array<std::vector<float>, 2> combs; // comb delay lines, combLength() long
        double mix = 1.0;
    };

    // The gain reduction (dB, 0 or below) follows the static curve at the attack
    // or release pace.
    struct Compressor {
        double reduction = 0.0;
        double makeup = 1.0; // gains, ramped
        double mix = 1.0;
    };

    // Split like the DJ EQ, at crossovers set per chunk.
    struct MultiComp {
        std::array<EqChannel, 2> channels{};
        std::array<double, kMcBands> reduction{};
        std::array<double, kMcBands> gains{1.0, 1.0, 1.0}; // band gains after compression, ramped
        double mix = 1.0;
        double output = 1.0; // gain after the mix, ramped
    };

    // A feedback delay network: kReverbLines lines of different lengths, mixed through a Hadamard matrix
    // (which keeps their energy and spreads each line's evenly over all of them), each with the gain that makes
    // it fall 60 dB in the decay time and a high shelf that takes it down faster above the damping's corner, by as
    // much for every line. The lines' lengths waver slowly, each at its own pace, so the tail's resonances
    // smear rather than ring. Each side of the input goes through the pre-delay and a few all-passes that diffuse
    // it, then into half the lines; the lines' outputs, signed differently, make the two sides. A shimmer reads
    // a few of its lines through pitch shifters as well, so what goes round climbs (or falls) each time.
    static constexpr int kReverbLines = 16;
    static constexpr int kDiffusers = 6; // per side: a plate runs the input through all of them, the others through four
    static constexpr int kShimmerLines = 4;
    struct Reverb {
        std::array<std::vector<float>, kReverbLines> lines; // all as long as the longest, with room for the wavering
        std::array<std::array<std::vector<float>, kDiffusers>, 2> diffusers; // per side
        std::array<std::vector<float>, 2> preDelay;
        int lineWrite = 0;
        int diffuserWrite = 0;
        int preWrite = 0;
        std::array<double, kReverbLines> damp{}; // the shelves' low-pass states
        // The lines' wavering: a sine each, as a turning phasor.
        std::array<double, kReverbLines> wanderCos{}, wanderSin{};
        // Shimmer: each shifted line's read heads sweep a window behind its length (see kShimmerWindow), a
        // phase all of them share, each a little apart; what they read is band-passed: a low-pass, and what it
        // lets through below the high-pass corner, taken away.
        double shimmerPhase = 0.0;
        std::array<double, kShimmerLines> shimmerLow{}, shimmerBass{};
        double size = 1.0;      // the lines' length scale, gliding toward the knob
        double preLength = 0.0; // samples, gliding
        double reach = 0.0;     // samples before what goes in has all come out of the pre-delay (see runReverb())
        double dry = 1.0;       // ramped
        double wet = 0.0;
        double width = 1.0;
        double shimmer = 0.0;   // how much of the shifted lines is shifted, ramped
        // Each line's gain and its shelf's (see runReverb()), and the size, Shimmer and decay they were worked out
        // for: NaN until the first chunk.
        std::array<double, kReverbLines> gains{}, highs{};
        std::array<double, 3> gainsFor{std::numeric_limits<double>::quiet_NaN()};
    };

    // First-order all-passes in a row, per side, their corners swept by the LFO; the right side's
    // LFO runs a quarter cycle ahead. Those past Stages wait cleared, so more start clean.
    struct Phaser {
        std::array<std::array<double, kMaxPhaserStages>, 2> stages{};
        std::array<double, 2> last{}; // the last stage's output, fed back to the first
        double phase = 0.0;           // the LFO, in cycles
        double mix = 0.5;
    };

    struct Flanger {
        std::array<std::vector<float>, 2> lines;
        int write = 0;
        std::array<double, 2> last{}; // the delayed sound, fed back
        double phase = 0.0;
        double mix = 0.5;
    };

    // Two all-pass chains whose outputs stay 90 degrees apart across the audio band (Olli
    // Niemitalo's design): a signal and its Hilbert transform.
    struct Hilbert {
        std::array<std::array<double, 4>, 4> a{}; // per section: x[n-1], x[n-2], y[n-1], y[n-2]
        std::array<std::array<double, 4>, 4> b{};
        double delayed = 0.0; // the first chain's output, one sample late

        // The in-phase and quadrature parts of `x`.
        std::array<double, 2> process(double x);
    };

    // The phase disperser's all-passes, per side; those past Stages wait cleared, so more start clean.
    struct Disperser {
        std::array<std::array<Biquad, kMaxDisperserStages>, 2> stages{};
        double mix = 1.0;
    };

    // The sound above the high-pass, mono, in a short line that kWidenerTaps swept taps read.
    struct Widener {
        std::vector<float> line;
        int write = 0;
        Biquad highPass;
        std::array<double, kWidenerTaps> phases{}; // the taps' sweeps, in cycles
        double size = 0.5; // gliding toward the knob
        double width = 1.0; // ramped
        double mix = 0.5;
    };

    struct Shifter {
        std::array<Hilbert, 2> hilbert{};
        std::array<double, 2> phase{}; // the oscillators, in cycles
        std::array<double, 2> last{};  // the shifted sound, fed back
        double mix = 1.0;
    };

    // One place in the chain. `running` is the type it processes as; when the
    // slot gets another type, the old one fades out before the new one starts.
    struct Slot {
        FxType running = FxType::None;
        double fade = 0.0; // 0 = off .. 1 = fully in
        Shaper shaper;
        Delay delay;
        Eq eq;
        ParamEq peq;
        FilterFx filter;
        Compressor comp;
        MultiComp multi;
        Reverb reverb;
        Phaser phaser;
        Flanger flanger;
        Shifter shifter;
        Disperser disperser;
        Widener widener;
        // The parametric EQ's, filter's or phase disperser's output, for its analyzer.
        Analyzed analyzed;
        // The shaper's drawn curve as a table (see runShaper()).
        ClipTable curve;
    };

    void start(int slot, const ParamValues &p, double tempo);
    double delayTarget(int slot, int side, const ParamValues &p, double tempo) const;
    void runShaper(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runDelay(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    void runEq(int slot, float *const *io, uint32_t n, const ParamValues &p);
    void runParamEq(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runFilter(int slot, float *const *io, uint32_t n, const ParamValues &p, double pitch);
    void runCompressor(int slot, float *const *io, uint32_t n, const ParamValues &p);
    void runMultiComp(int slot, float *const *io, uint32_t n, const ParamValues &p);
    // These return the loudest sample they put out or took in, so a tail keeps them running.
    double runReverb(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runPhaser(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    double runFlanger(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    double runShifter(int slot, float *const *io, uint32_t n, const ParamValues &p);

    double sampleRate_ = 48000.0;
    std::array<double, kReverbLines> reverbLengths_{}; // at full size, in samples
    std::array<std::array<int, kDiffusers>, 2> diffuserLengths_{};
    std::array<std::array<double, 2>, kReverbLines> wanderTurn_{}; // each line's wavering: cos and sin of its step
    std::array<Slot, kFxSlots> slots_;
    BiquadCoefs lowPass1_, highPass1_, lowPass2_, highPass2_, allPass2_;
    int64_t tail_ = 0; // samples left to keep processing after the input went silent
    std::array<std::array<float, kMcBands>, kFxSlots> reduction_{};
    void feedAnalyzer(int slot, float *const *io, uint32_t n);
    double runDisperser(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runWidener(int slot, float *const *io, uint32_t n, const ParamValues &p);
    bool analyzing_ = false;
    Analyzer analyzer_;
};

} // namespace fmvibe
