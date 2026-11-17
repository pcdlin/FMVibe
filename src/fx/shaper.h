#pragma once

#include "common.h"

#include "../clipcurve.h"
#include "../oversampler.h"

namespace fmvibe::fx {

// The shaper effect: the voices' shapers' curves on the whole mix, at twice the rate (see run()).
struct Shaper {
    std::array<ClipState, 2> sides{};
    double drive = 1.0; // gains, ramped
    double output = 1.0;
    double mix = 1.0;
    double between = 1.0; // after each Stack stage but the last
    // A shaper's curve as a chunk ran it: what its path (see run()) is built from.
    struct Path {
        ClipMode mode = ClipMode::Soft;
        bool symmetric = false;
        int stack = 1;
    };
    // The drawn curve as a table (see run()).
    ClipTable curve;
    // Up to twice the rate and back down, a side each, designed in prepare(); and whether the last chunk ran a Stack
    // curve.
    std::array<HalfBand, 2> up, down;
    bool stacked = false;
    // Whether the last chunk ran the shaper (start() clears it), and its curve then.
    bool ran = false;
    Path last;
    // For a moment after its curve goes to Stack or leaves it, the path it left, fading out as the new one fades
    // in, with its curve and its sides as they were. Both run on the same samples at twice the rate.
    Path was;
    std::array<ClipState, 2> wasSides{};
    double wasFade = 0.0; // 1 .. 0

    void prepare(double sampleRate);
    // Clears it as it is about to fade in, and jumps its ramps to the current settings.
    void start(const Context &ctx);
    void run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fmvibe::fx
