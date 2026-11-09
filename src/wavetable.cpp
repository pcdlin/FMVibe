#include "wavetable.h"

#include <cstdint>
#include <numbers>

namespace fmvibe {

namespace {

constexpr int kWaves = static_cast<int>(OpWave::Count);
constexpr int kMask = WaveTables::kSize - 1;

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
    case OpWave::AltSine: // sin(2t) for the first half of the cycle, then nothing
        if (n == 2)
            return {0.0, 0.5};
        return {odd ? 4.0 / (pi * (4.0 - n * n)) : 0.0, 0.0};
    case OpWave::Organ: { // drawbars 8' 4' 2 2/3' 2' 1 1/3' 1' at 8 8 6 6 4 4, 3 dB a step
        constexpr double kDrawbars[] = {1.0, 1.0, 0.5, 0.5, 0.0, 0.25, 0.0, 0.25};
        return {0.0, n <= 8 ? kDrawbars[n - 1] : 0.0};
    }
    case OpWave::Vowel: { // a buzz falling 6 dB an octave, through formant peaks at harmonics 7, 11 and 24: an "ah"
        auto peak = [n](double at, double width) { return 1.0 / (1.0 + (n - at) * (n - at) / (width * width)); };
        return {0.0, (0.15 + peak(7.0, 1.2) + 0.7 * peak(11.0, 1.5) + 0.3 * peak(24.0, 2.5)) / n};
    }
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
                // kSize is a power of two: masking with it takes the indices round as % does.
                for (int i = 0, k = 0; i < kSize; ++i, k = (k + n) & kMask)
                    sum[i] += h.sine * sine[k] + h.cosine * sine[(k + kSize / 4) & kMask];
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
namespace {
// The levels from 0 up, as WaveTables has them: how many partials each has room for.
constexpr int customTop(int level) { return std::min(WaveTables::kHarmonics >> level, kPartials); }
} // namespace

void CustomWave::addPartial(int n, double amount, double phase) {
    if (amount == 0.0)
        return;
    constexpr int kSize = WaveTables::kSize;
    const std::vector<double> &sine = sineTable();
    const int shift = static_cast<int>(std::lround(phase * kSize)) % kSize;
    for (int t = 0; t < kTables && customTop(t + kFull) >= n; ++t) {
        double *sum = &sums_[static_cast<size_t>(t) * kSize];
        // From `shift`, n steps a sample round the table, which a mask with its size wraps as % would.
        for (int i = 0, k = shift & kMask; i < kSize; ++i, k = (k + n) & kMask)
            sum[i] += amount * sine[k];
    }
}

// From scratch, as WaveTables builds its: from the thinnest level up, each the one before with the partials it has
// room for added (in place, so nothing is allocated on the audio thread).
void CustomWave::buildAll() {
    constexpr int kSize = WaveTables::kSize;
    const std::vector<double> &sine = sineTable();
    int done = 0;
    for (int t = kTables - 1; t >= 0; --t) {
        double *sum = &sums_[static_cast<size_t>(t) * kSize];
        if (t == kTables - 1)
            std::fill_n(sum, kSize, 0.0);
        else
            std::copy_n(sum + kSize, kSize, sum); // the thinner level's, just built
        const int top = customTop(t + kFull);
        for (int n = done + 1; n <= top; ++n) {
            const double amount = built_[2 * (n - 1)];
            if (amount <= 0.0)
                continue;
            const int shift = static_cast<int>(std::lround(built_[2 * (n - 1) + 1] * kSize)) % kSize;
            for (int i = 0, k = shift & kMask; i < kSize; ++i, k = (k + n) & kMask)
                sum[i] += amount * sine[k];
        }
        done = top;
    }
}

void CustomWave::build() {
    if (!stale_)
        return;
    stale_ = false;
    // Partials changed, and what taking them out and putting them in again would cost against adding up every one
    // that sounds: a table's worth for each level each is in.
    int changed = 0, cost = 0, sounding = 0;
    for (int n = 1; n <= kPartials; ++n) {
        const double was = std::max(built_[2 * (n - 1)], 0.0), now = std::max(wanted_[2 * (n - 1)], 0.0);
        sounding += now > 0.0 ? 1 : 0;
        if (was == now && (was == 0.0 || built_[2 * n - 1] == wanted_[2 * n - 1]))
            continue;
        ++changed;
        int levels = 0;
        for (int t = 0; t < kTables && customTop(t + kFull) >= n; ++t)
            ++levels;
        cost += levels * ((was > 0.0) + (now > 0.0));
    }
    // With nothing left sounding, from scratch: what rounding left of the partials taken out would else be scaled
    // up to full level.
    const bool fromScratch = !everBuilt_ || sounding == 0 || cost >= sounding || ++sinceAll_ > 64;
    if (fromScratch) {
        built_ = wanted_;
        sinceAll_ = 0;
        buildAll();
    } else if (changed > 0) {
        for (int n = 1; n <= kPartials; ++n) {
            const double wasLevel = built_[2 * (n - 1)], wasPhase = built_[2 * n - 1];
            const double nowLevel = wanted_[2 * (n - 1)], nowPhase = wanted_[2 * n - 1];
            if (wasLevel == nowLevel && wasPhase == nowPhase)
                continue;
            addPartial(n, -std::max(wasLevel, 0.0), wasPhase);
            addPartial(n, std::max(nowLevel, 0.0), nowPhase);
        }
        built_ = wanted_;
    }
    everBuilt_ = true;
    // Each table as floats, scaled together so the fullest peaks at 1; all silent with every partial at 0.
    constexpr int kSize = WaveTables::kSize;
    double peak = 0.0;
    for (int i = 0; i < kSize; ++i)
        peak = std::max(peak, std::abs(sums_[i]));
    const double scale = peak > 0.0 ? 1.0 / peak : 0.0;
    for (int t = 0; t < kTables; ++t) {
        float *table = &data_[static_cast<size_t>(t) * (kSize + 1)];
        const double *sum = &sums_[static_cast<size_t>(t) * kSize];
        for (int i = 0; i < kSize; ++i)
            table[i] = static_cast<float>(sum[i] * scale);
        table[kSize] = table[0];
    }
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
