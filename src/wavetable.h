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
    // It runs for every unison copy of every operator, every sample: std::floor here measured fastest in the plugins'
    // MSVC build. A floor made by hand with std::copysign, which GCC inlines, became a library call there, costing a
    // tenth more CPU with one copy and a third more with eight.
    static float read(const float *table, double phase) {
        const double x = (phase - std::floor(phase)) * kSize;
        const int i = std::min(static_cast<int>(x), kSize - 1) & (kSize - 1);
        return table[i] + (table[i + 1] - table[i]) * static_cast<float>(x - i);
    }

    // As read(), for a phase already within 0..1 (a warped one): without the floor, which a warped operator would
    // otherwise take twice a copy and a sample.
    static float readCycle(const float *table, double phase) {
        const double x = phase * kSize;
        const int i = std::clamp(static_cast<int>(x), 0, kSize - 1);
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
    // The levels at and below kFull hold every partial alike (their room for harmonics is kPartials or more): one
    // table stands for them all, so it is built once.
    static constexpr int kFull = [] {
        int level = 0;
        while (level + 1 < WaveTables::kLevels && (WaveTables::kHarmonics >> (level + 1)) >= kPartials)
            ++level;
        return level;
    }();
    static constexpr int kTables = WaveTables::kLevels - kFull;

    CustomWave()
        : data_(static_cast<size_t>(kTables) * (WaveTables::kSize + 1), 0.0f),
          sums_(static_cast<size_t>(kTables) * WaveTables::kSize, 0.0) {}
    // Takes the first `count` partials' `level(k)` and `phase(k)` (a level of 0 leaves a partial out), the rest
    // silent; true if they changed. Cheap, as the synth asks every time its params change: the tables follow only
    // at build(), once an operator plays the wave.
    template <class Level, class Phase>
    bool update(int count, const Level &level, const Phase &phase) {
        bool same = true;
        for (int k = 0; k < kPartials && same; ++k)
            same = wanted_[2 * k] == (k < count ? level(k) : 0.0) &&
                   wanted_[2 * k + 1] == (k < count ? phase(k) : 0.0);
        if (same)
            return false;
        for (int k = 0; k < kPartials; ++k) {
            wanted_[2 * k] = k < count ? level(k) : 0.0;
            wanted_[2 * k + 1] = k < count ? phase(k) : 0.0;
        }
        stale_ = true;
        return true;
    }
    // The tables for the partials update() took last, built if they changed since: only the partials that changed
    // are taken out and put in again, while they are few (as a drag in the partial editor moves one).
    void build();
    // kSize + 1 samples, as WaveTables::table(); build() first.
    const float *table(double increment) const {
        const int level = std::max(WaveTables::level(increment), kFull) - kFull;
        return &data_[static_cast<size_t>(level) * (WaveTables::kSize + 1)];
    }

private:
    void buildAll();
    // Adds `amount` of partial `n` (from 1) at `phase` into every table that has room for it.
    void addPartial(int n, double amount, double phase);

    std::vector<float> data_;  // kTables tables, the fullest first, each scaled so the fullest peaks at 1
    std::vector<double> sums_; // the same unscaled, each level's partials added up, kept for build() to change
    std::array<double, 2 * kPartials> wanted_{};                    // what update() took: levels and phases
    std::array<double, 2 * kPartials> built_{};                     // what the tables hold
    bool stale_ = true;
    bool everBuilt_ = false;
    int sinceAll_ = 0; // builds by changes since the last from scratch, which rounding over many would drift from
};

// The first kPartials partials of `wave` as a Custom wave's, in (level, phase) pairs: levels 0..1 with the
// loudest at 1, phases 0..1 of a cycle, so the Custom wave starts out as that wave. False for a wave without
// any (Noise, or Custom itself), leaving `out` alone.
bool wavePartials(OpWave wave, std::array<double, 2 * kPartials> &out);
// Harmonic `n` (from 1) of a wave with a table of its own, in sine phase: `sine` sin(n t) + `cosine` cos(n t), before
// the wave is scaled to peak at 1. Both 0 for a wave without a series (Noise, Custom, Wavetable, Sample).
void waveHarmonic(OpWave wave, int n, double &cosine, double &sine);
// Whether a wave has a table in WaveTables: all but Noise, Custom and the data waves.
bool hasTable(OpWave wave);

} // namespace fmvibe
