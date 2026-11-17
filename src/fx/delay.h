#pragma once

#include "common.h"

namespace fmvibe {

// Delay mix: the dry signal stays at full level up to 50 %, the echoes reach full level there.
inline double delayDryLevel(double mix) {
    return std::min(1.0, 2.0 * (1.0 - mix));
}
inline double delayWetLevel(double mix) {
    return std::min(1.0, 2.0 * mix);
}

namespace fx {

// The delay: a line a side, its echoes through tone filters, fed back.
struct Delay {
    std::array<ZeroBuffer, 2> lines;
    int write = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    std::array<double, 2> length{};   // samples, glide toward the set times
    std::array<double, 2> lowPass{};  // tone filter states
    std::array<double, 2> highPass{};
    double feedback = 0.0;
    double dry = 1.0;
    double wet = 0.0;
    double width = 1.0;

    // Allocates the lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so a tail keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
    // How long side `side` waits as set, in samples.
    double target(const Context &ctx, int side) const;
};

} // namespace fx
} // namespace fmvibe
