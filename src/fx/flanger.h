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
// fed back.
struct Flanger {
    std::array<ZeroBuffer, 2> lines;
    int write = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    std::array<double, 2> last{}; // the delayed sound, fed back
    double phase = 0.0;
    double mix = 0.5;
    double polarity = 1.0; // as the phaser's
    // The delay per side in samples, worked out every kSweepStep samples like the phaser's coefficient.
    std::array<double, 2> delay{};
    bool delayKnown = false;

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
