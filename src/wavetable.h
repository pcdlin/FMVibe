#pragma once

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
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
    static constexpr int kHarmonics = kSize / 4; // in the richest table
    static constexpr int kLevels = 11;           // 1024 harmonics down to 1

    WaveTables();

    // kSize + 1 samples (the last repeats the first) for a phase increment in cycles per sample.
    const float *table(OpWave wave, double increment) const;
    // The level whose harmonics stay below Nyquist at a phase increment.
    static int level(double increment);

    // Phase in cycles, any range.
    static float read(const float *table, double phase) {
        const double x = (phase - std::floor(phase)) * kSize;
        const int i = std::min(static_cast<int>(x), kSize - 1);
        return table[i] + (table[i + 1] - table[i]) * static_cast<float>(x - i);
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
