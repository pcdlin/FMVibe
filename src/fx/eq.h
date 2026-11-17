#pragma once

#include "common.h"

namespace fmvibe {

// Crossovers of the DJ EQ.
constexpr double kEqLowHz = 250.0;
constexpr double kEqHighHz = 2500.0;

// Magnitude response of the DJ EQ at `hz`, for band params (-1..1) low, mid and high.
double eqResponse(double hz, double low, double mid, double high);

namespace fx {

// The DJ EQ: three bands split at kEqLowHz and kEqHighHz, each at its gain.
struct Eq {
    std::array<EqChannel, 2> channels{};
    std::array<double, 3> gains{1.0, 1.0, 1.0};

    // Its crossovers at `sampleRate`, the same for every slot (Effects works them out once, in prepare()).
    static CrossoverCoefs crossovers(double sampleRate);
    void start(const Context &ctx);
    void run(const Context &ctx, const CrossoverCoefs &bands, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
