#pragma once

#include "common.h"

namespace fmvibe::fx {

// Two all-pass chains whose outputs stay 90 degrees apart across the audio band, from kHilbertLowHz up: a signal
// and its Hilbert transform. Their coefficients are worked out for the rate (see hilbertCoefs()), with more
// sections the higher it is, since the band then spans more octaves; at 44.1 and 48 kHz, six a chain (Olli
// Niemitalo's design has four), which keep the mirror image about 70 dB down.
constexpr int kMaxHilbertSections = 9;
struct HilbertCoefs {
    int sections = 0; // per chain
    std::array<double, kMaxHilbertSections> a{}; // the chain heard a sample late
    std::array<double, kMaxHilbertSections> b{};
};
struct Hilbert {
    std::array<std::array<double, 4>, kMaxHilbertSections> a{}; // per section: x[n-1], x[n-2], y[n-1], y[n-2]
    std::array<std::array<double, 4>, kMaxHilbertSections> b{};
    double delayed = 0.0; // the first chain's output, one sample late

    // The in-phase and quadrature parts of `x`.
    std::array<double, 2> process(double x, const HilbertCoefs &coefs);
};
// The same for every frequency shifter at a rate (Effects works them out once, in prepare()).
HilbertCoefs hilbertCoefs(double sampleRate);

// The frequency shifter: each side's analytic signal turned by an oscillator, fed back.
struct Shifter {
    std::array<Hilbert, 2> hilbert{};
    std::array<double, 2> phase{}; // the oscillators, in cycles
    std::array<double, 2> last{};  // the shifted sound, fed back
    double mix = 1.0;

    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its feedback keeps it running.
    double run(const Context &ctx, const HilbertCoefs &coefs, float *const *io, uint32_t n);
};

} // namespace fmvibe::fx
