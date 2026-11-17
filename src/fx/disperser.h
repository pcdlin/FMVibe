#pragma once

#include "common.h"

namespace fmvibe {

// The phase disperser's all-pass (b0, b1, b2, a1, a2) at Freq (0..1 on the cutoff curve), Pinch setting its Q
// from 0.3 to 6, and how long the sound at `hz` comes out after the rest through `stages` of them (seconds).
std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate);
double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate);

namespace fx {

// The phase disperser's all-passes, per side; those past Stages wait cleared, so more start clean.
struct Disperser {
    std::array<std::array<Biquad, kMaxDisperserStages>, 2> stages{};
    double mix = 1.0;

    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its ring keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
