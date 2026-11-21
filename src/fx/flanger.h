#pragma once

#include "common.h"

namespace fmvibe {

// The flanger's sweep: its delay (seconds) with its LFO at `lfo` (-1..1), from its params. It sweeps from
// (1 - 0.95 Depth) times Delay to (1 + 0.95 Depth) times it, evenly in octaves (the notches, a delay's inverse apart,
// moving as evenly in pitch): Delay times `middle` (the two ends' geometric mean) times e^(`reach` lfo).
struct FlangerSweep {
    double middle, reach;
};
FlangerSweep flangerSweep(double depth);
double flangerDelay(double delayNorm, double depth, double lfo);
// Its magnitude response, for drawing, with its delay at `delaySeconds`, at `polarity` (see sweepPolarity()). At 50 %
// mix its notches go deepest.
double flangerResponse(double hz, double delaySeconds, double feedback, double mix, double polarity);

namespace fx {

// The flanger: a short line a side, its delay swept by the LFO (the right side's a quarter cycle ahead when stereo),
// fed back. Through zero (TZF), the dry sound waits kThroughZeroWait (see fxlag.h) in a line of its own, and the swept
// copy, read as much later less the sweep's middle, passes through it twice a cycle, cancelling every frequency for a
// moment: the jet-plane sweep of two tapes. Its feedback still goes round the swept delay alone, as without. The output
// then comes out kThroughZeroWait late whatever the knobs say, which fxlag.h tells the host and the effects graph's
// other paths. Switched, the sound fades from one to the other over kThroughZeroFadeSeconds. Delay and Depth moving
// slowly, its reads follow them; jumping (as one a note's own random value sets does, when the effects take up the new
// note), they crossfade from where they read to where they now put them.
constexpr double kThroughZeroFadeSeconds = 0.01;
// Where Delay and Depth have it read: the sweep's middle in samples and its reach (see FlangerSweep).
struct FlangerReads {
    double middle = 0.0, reach = 0.0;
    bool operator==(const FlangerReads &) const = default;
};
struct Flanger {
    std::array<ZeroBuffer, 2> lines;
    std::array<ZeroBuffer, 2> dryLines; // the dry sound, for through zero: as long as it waits
    int dryWrite = 0;
    double throughZero = 0.0;           // 0 .. 1, how far the sound has gone over to the one through zero
    int write = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    std::array<double, 2> last{}; // the delayed sound, fed back
    double phase = 0.0;
    double mix = 0.5;
    double polarity = 1.0; // as the phaser's
    // The delay per side in samples, worked out every kSweepStep samples like the phaser's coefficient, and where the
    // swept copy is read through zero: as much later as the dry sound waits, less the sweep's middle.
    std::array<double, 2> delay{}, zeroDelay{};
    bool delayKnown = false;
    FlangerReads reads; // where it reads
    // While a jump crossfades: where it read before it and its delays there, and the samples the crossfade has left of
    // its fadeLength.
    FlangerReads fadeReads;
    std::array<double, 2> fadeDelay{}, fadeZeroDelay{};
    uint32_t fadeLeft = 0, fadeLength = 0;

    // Allocates the lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its feedback keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
    // Its sweep going round `cycles` while the effects go by idle, worked out again where it is then.
    void idle(double cycles);
};

} // namespace fx
} // namespace fmvibe
