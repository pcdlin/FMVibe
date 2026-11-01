#include "wavetable.h"

#include <cstdint>
#include <numbers>

namespace fmvibe {

namespace {

constexpr int kWaves = static_cast<int>(OpWave::Count);

struct Harmonic {
    double cosine;
    double sine;
};

// Fourier series of each wave, without DC. Sine-phase so every wave starts near 0.
Harmonic harmonic(OpWave wave, int n) {
    constexpr double pi = std::numbers::pi;
    const bool odd = n % 2 == 1;
    switch (wave) {
    case OpWave::Sine:
        return {0.0, n == 1 ? 1.0 : 0.0};
    case OpWave::Triangle:
        return {0.0, odd ? 8.0 / (pi * pi * n * n) * ((n / 2) % 2 ? -1.0 : 1.0) : 0.0};
    case OpWave::Saw:
        return {0.0, 2.0 / (pi * n) * (odd ? 1.0 : -1.0)};
    case OpWave::Square:
        return {0.0, odd ? 4.0 / (pi * n) : 0.0};
    case OpWave::Pulse: { // 25 % duty
        const double c = 2.0 * pi * n * 0.25;
        return {2.0 / (pi * n) * std::sin(c), 2.0 / (pi * n) * (1.0 - std::cos(c))};
    }
    case OpWave::HalfSine: // max(sin, 0)
        if (n == 1)
            return {0.0, 0.5};
        return {odd ? 0.0 : -2.0 / (pi * (n * n - 1.0)), 0.0};
    case OpWave::AbsSine: // |sin|: only even harmonics, so it sounds an octave up
        return {odd ? 0.0 : -4.0 / (pi * (n * n - 1.0)), 0.0};
    case OpWave::Noise:
    case OpWave::Custom: // drawn from partials: see CustomWave
    case OpWave::Count:
        break;
    }
    return {0.0, 0.0};
}

// A sine table to read harmonics from, shared by WaveTables and CustomWave.
const std::vector<double> &sineTable() {
    static const std::vector<double> sine = [] {
        std::vector<double> table(WaveTables::kSize);
        for (int i = 0; i < WaveTables::kSize; ++i)
            table[i] = std::sin(2.0 * std::numbers::pi * i / WaveTables::kSize);
        return table;
    }();
    return sine;
}

} // namespace

WaveTables::WaveTables() : data_(static_cast<size_t>(kWaves) * kLevels * (kSize + 1), 0.0f) {
    const std::vector<double> &sine = sineTable();

    // Build from the thinnest table up, adding the harmonics each richer level allows.
    std::vector<double> sum(kSize);
    for (int w = 0; w < kWaves; ++w) {
        const auto wave = static_cast<OpWave>(w);
        if (wave == OpWave::Noise || wave == OpWave::Custom)
            continue;
        std::fill(sum.begin(), sum.end(), 0.0);
        int done = 0;
        for (int level = kLevels - 1; level >= 0; --level) {
            const int top = kHarmonics >> level;
            for (int n = done + 1; n <= top; ++n) {
                const Harmonic h = harmonic(wave, n);
                if (h.cosine == 0.0 && h.sine == 0.0)
                    continue;
                for (int i = 0; i < kSize; ++i) {
                    const int k = (n * i) % kSize;
                    sum[i] += h.sine * sine[k] + h.cosine * sine[(k + kSize / 4) % kSize];
                }
            }
            done = top;
            float *table = &data_[offset(w, level)];
            for (int i = 0; i < kSize; ++i)
                table[i] = static_cast<float>(sum[i]);
            table[kSize] = table[0];
        }

        // One gain per wave, taken from the full-band table, so octaves match in level.
        const float *full = &data_[offset(w, 0)];
        float peak = 0.0f;
        for (int i = 0; i < kSize; ++i)
            peak = std::max(peak, std::abs(full[i]));
        if (peak > 0.0f)
            for (int level = 0; level < kLevels; ++level)
                for (int i = 0; i <= kSize; ++i)
                    data_[offset(w, level) + i] /= peak;
    }
}

int WaveTables::level(double increment) {
    return increment <= 0.0 ? 0
                            : std::clamp(static_cast<int>(std::ceil(std::log2(increment * 2 * kHarmonics))), 0,
                                         kLevels - 1);
}

const float *WaveTables::table(OpWave wave, double increment) const {
    return &data_[offset(static_cast<int>(wave), level(increment))];
}

const WaveTables &waveTables() {
    static const WaveTables tables;
    return tables;
}

// Like WaveTables: from the thinnest level up, each adding the partials it has room for, then one gain
// from the fullest, so the octaves match. With every partial at 0, it is silent.
bool CustomWave::update(const double *partials) {
    if (std::equal(partials_.begin(), partials_.end(), partials))
        return false;
    std::copy_n(partials, partials_.size(), partials_.begin());
    constexpr int kSize = WaveTables::kSize;
    const std::vector<double> &sine = sineTable();
    std::vector<double> &sum = sum_;
    std::fill(sum.begin(), sum.end(), 0.0);
    int done = 0;
    for (int level = WaveTables::kLevels - 1; level >= 0; --level) {
        const int top = std::min(WaveTables::kHarmonics >> level, kPartials);
        for (int n = done + 1; n <= top; ++n) {
            const double amount = partials[2 * (n - 1)];
            if (amount <= 0.0)
                continue;
            const int shift = static_cast<int>(std::lround(partials[2 * (n - 1) + 1] * kSize)) % kSize;
            for (int i = 0; i < kSize; ++i)
                sum[i] += amount * sine[(n * i + shift) % kSize];
        }
        done = top;
        float *table = &data_[static_cast<size_t>(level) * (kSize + 1)];
        for (int i = 0; i < kSize; ++i)
            table[i] = static_cast<float>(sum[i]);
        table[kSize] = table[0];
    }
    float peak = 0.0f;
    for (int i = 0; i < kSize; ++i)
        peak = std::max(peak, std::abs(data_[i]));
    if (peak > 0.0f)
        for (float &x : data_)
            x /= peak;
    return true;
}

bool wavePartials(OpWave wave, std::array<double, 2 * kPartials> &out) {
    if (wave == OpWave::Noise || wave == OpWave::Custom || wave == OpWave::Count)
        return false;
    // A partial's a sin(n t) + b cos(n t) is its level times sin(n t + 2 pi phase).
    double loudest = 0.0;
    for (int n = 1; n <= kPartials; ++n) {
        const Harmonic h = harmonic(wave, n);
        loudest = std::max(loudest, std::hypot(h.sine, h.cosine));
    }
    if (loudest <= 0.0)
        return false;
    for (int n = 1; n <= kPartials; ++n) {
        const Harmonic h = harmonic(wave, n);
        const double level = std::hypot(h.sine, h.cosine) / loudest;
        const double phase = level > 1e-9 ? std::atan2(h.cosine, h.sine) / (2.0 * std::numbers::pi) : 0.0;
        out[2 * (n - 1)] = level < 1e-9 ? 0.0 : level;
        out[2 * (n - 1) + 1] = phase < 0.0 ? phase + 1.0 : phase;
    }
    return true;
}

} // namespace fmvibe
