#pragma once

#include "common.h"

#include <limits>

namespace fmvibe {

// Magnitude response of one parametric EQ band at `hz`, from its params (frequency and Q normalized, gain in dB).
double peqResponse(double hz, PeqType type, double freq, double gainDb, double q);

namespace fx {

// The parametric EQ: kPeqBands biquads a side, each switched on and off by its own On.
struct ParamEq {
    std::array<std::array<Biquad, 2>, kPeqBands> bands{}; // per band, per channel
    std::array<BiquadRamp, kPeqBands> coefs{};             // each band's, ramping to its params' (see BiquadRamp)
    // How much of each band is heard, gliding to its On switch (see run()); NaN until the first chunk,
    // which takes the switch as it is.
    static constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();
    std::array<double, kPeqBands> on{kUnset, kUnset, kUnset};
    double amount = 1.0; // ramping to its Amount (see run())
    // Each band's coefficients as its params last put them, and those params (its type, frequency, gain and Q, and the
    // rate): worked out again only as they change, some ten exp, pow, cos and sqrt calls a band and a chunk otherwise.
    std::array<std::array<double, 5>, kPeqBands> madeFrom = [] {
        std::array<std::array<double, 5>, kPeqBands> none;
        for (auto &band : none)
            band.fill(kUnset);
        return none;
    }();
    std::array<BiquadCoefs, kPeqBands> made{};

    void start(const Context &ctx);
    void run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
