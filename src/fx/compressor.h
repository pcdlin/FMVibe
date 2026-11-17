#pragma once

#include "common.h"

namespace fmvibe::fx {

// The compressor. The gain reduction (dB, 0 or below) follows the static curve at the attack
// or release pace.
struct Compressor {
    double reduction = 0.0;
    double makeup = 1.0; // gains, ramped
    double mix = 1.0;

    void start(const Context &ctx);
    // Returns whether its gain is still on its way to where the curve puts it (a release running out). Its reduction
    // goes to the meters' band 0.
    bool run(const Context &ctx, float *const *io, uint32_t n, const Meters &meters);
};

} // namespace fmvibe::fx
