#pragma once

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace fmvibe {

// Band-limited single-cycle tables for the operator waves, one per octave: each
// level holds half the harmonics of the one before, so an operator reads the
// richest table that stays below Nyquist at its frequency. Noise has no table.
// Each table has at least four samples per cycle of its highest harmonic, so
// reading between samples stays clean.
class WaveTables {
public:
    static constexpr int kSize = 4096;
    static_assert((kSize & (kSize - 1)) == 0); // read() masks its index with it
    static constexpr int kHarmonics = kSize / 4; // in the richest table
    static constexpr int kLevels = 11;           // 1024 harmonics down to 1

    WaveTables();

    // kSize + 1 samples (the last repeats the first) for a phase increment in cycles per sample.
    const float *table(OpWave wave, double increment) const;
    // The level whose harmonics stay below Nyquist at a phase increment.
    static int level(double increment);

    // Phase in cycles, any range. A phase that isn't a number gives one back, without reading outside the table: kSize
    // is a power of two, so the mask leaves every other index as it is.
    static float read(const float *table, double phase) {
        const double x = (phase - floorOf(phase)) * kSize;
        const int i = std::min(static_cast<int>(x), kSize - 1) & (kSize - 1);
        return table[i] + (table[i + 1] - table[i]) * static_cast<float>(x - i);
    }

    // std::floor(x), exactly, without the library call or the long instruction sequence a compiler targeting plain
    // SSE2 makes of it: read() takes one for every unison copy of every operator, every sample. Past 2^52 every
    // double is a whole number already, and std::floor takes those (and infinities and NaN) as before.
    static double floorOf(double x) {
        if (!(std::abs(x) < 4503599627370496.0)) // 2^52
            return std::floor(x);
        // Toward zero, keeping the sign, so -0 stays -0 as std::floor gives it.
        const double whole = std::copysign(static_cast<double>(static_cast<int64_t>(x)), x);
        return whole > x ? whole - 1.0 : whole;
    }

private:
    static size_t offset(int wave, int level) { return (static_cast<size_t>(wave) * kLevels + level) * (kSize + 1); }

    std::vector<float> data_;
};

// Built once, on first use (the plugin calls it on the main thread).
const WaveTables &waveTables();

// An operator's Custom wave, drawn from its partials, with WaveTables' levels and size. Built again
// whenever the partials change; the synth does that on the audio thread, into memory it holds.
class CustomWave {
public:
    CustomWave() : data_(static_cast<size_t>(WaveTables::kLevels) * (WaveTables::kSize + 1), 0.0f) {}
    // From the levels and phases of partials 1 to kPartials, alternating (a level of 0 leaves a partial out). True
    // if they changed.
    bool update(const double *partials);
    // The same from the first `count` partials' `level(k)` and `phase(k)`, the rest silent: looked over first, and
    // only copied out when they changed, since the synth asks for it every time it renders.
    template <class Level, class Phase>
    bool update(int count, const Level &level, const Phase &phase) {
        bool same = true;
        for (int k = 0; k < kPartials && same; ++k)
            same = partials_[2 * k] == (k < count ? level(k) : 0.0) &&
                   partials_[2 * k + 1] == (k < count ? phase(k) : 0.0);
        if (same)
            return false;
        std::array<double, 2 * kPartials> partials{};
        for (int k = 0; k < count; ++k) {
            partials[2 * k] = level(k);
            partials[2 * k + 1] = phase(k);
        }
        return update(partials.data());
    }
    const float *table(double increment) const {
        return &data_[static_cast<size_t>(WaveTables::level(increment)) * (WaveTables::kSize + 1)];
    }

private:
    std::vector<float> data_;
    std::vector<double> partials_ = std::vector<double>(2 * kPartials, -1.0); // what data_ was built from
    std::vector<double> sum_ = std::vector<double>(WaveTables::kSize); // the sum being built, kept so update() doesn't allocate
};

// The first kPartials partials of `wave` as a Custom wave's, in (level, phase) pairs: levels 0..1 with the
// loudest at 1, phases 0..1 of a cycle, so the Custom wave starts out as that wave. False for a wave without
// any (Noise, or Custom itself), leaving `out` alone.
bool wavePartials(OpWave wave, std::array<double, 2 * kPartials> &out);

} // namespace fmvibe
