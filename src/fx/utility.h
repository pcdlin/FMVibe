#pragma once

#include "common.h"

namespace fmvibe {

// The utility's gain on each side, from its Gain knob (0..1, see utilityGain()) and its Pan (-1..1): a balance, which
// turns the side it pans away from down and leaves the other as it is.
struct UtilityGains {
    double left, right;
};
UtilityGains utilityGains(double gainNorm, double pan);

namespace fx {

// The utility: the sides through a high-pass for its Mono Bass, then scaled by Width, and each side's gain (Gain
// and the balance), ramped.
struct Utility {
    Biquad highPass;
    double gainL = 1.0, gainR = 1.0, width = 1.0;
    double mono = 0.0; // how much of the high-passed sides is heard, 0 (Mono Bass off) .. 1, faded
    double hz = 0.0;   // the high-pass's corner: Mono Bass's, the last one set while it fades out

    void start(const Context &ctx);
    void run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
