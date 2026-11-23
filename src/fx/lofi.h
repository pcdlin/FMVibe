#pragma once

#include "common.h"

namespace fmvibe {

// The Lo-Fi's settings from its params: how far apart its levels are (full scale is 1, so 2^(1 - Bits)), how often it
// takes a sample (Hz, Rate held under the host rate) and the corner of its smoothing (Hz, a little under half Rate).
struct LoFiSettings {
    double step, rateHz, smoothHz;
};
LoFiSettings loFiSettings(double bits, double rateNorm, double sampleRate);
// What a sample becomes at that resolution: the nearest of 2^Bits levels, each in the middle of its share of full scale
// (half a step off 0, as a converter's codes sit), so 1 bit is two levels, ±0.5, and nothing goes past full scale.
inline double loFiLevel(double x, double step) {
    const double top = 1.0 - 0.5 * step;
    return std::clamp(step * (std::floor(x / step) + 0.5), -top, top);
}

namespace fx {

constexpr double kLoFiQuietSeconds = 0.01; // silence going in for this long comes out as silence (see LoFi::quiet)

// Lo-Fi, as an early digital converter: each side held between samples taken Rate times a second (the steps between
// fold the top back down, as they did), each sample at Bits of resolution (the nearest of 2^Bits levels across full
// scale, see loFiLevel()), and Smooth blending in what a 12 dB low-pass just under half Rate makes of it, as a converter's output
// filter does. Mix blends it with the dry sound.
struct LoFi {
    std::array<double, 2> held{}; // the sample each side holds
    double phase = 1.0;           // how far to the next sample, 0 .. 1 (1 to begin with: the first sample takes one)
    std::array<Biquad, 2> smooth{};
    BiquadRamp smoothCoefs{};
    double smoothMix = 0.0; // Smooth, ramped
    double mix = 1.0;       // Mix, ramped
    // How long both sides have been silent (under -120 dB, where the effects count a sound as rung out), in samples:
    // past kLoFiQuietSeconds it holds 0, where a level half a step off 0 would put silence out as DC, which nothing
    // after the effects takes away and which would keep them running for good. A sound crossing 0 isn't silent.
    int quiet = 0;

    void start(const Context &ctx);
    void run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
