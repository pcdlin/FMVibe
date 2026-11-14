#include "tables.h"

#include "fft.h"
#include "opdata.h"
#include "uistrings.h"
#include "wavetable.h"

#include <cmath>
#include <complex>
#include <numbers>

namespace fmvibe {

namespace {

using Spectrum = std::vector<std::complex<double>>; // harmonics 0..kHarmonics, as harmonicsOf() gives them
constexpr int kHarmonics = FrameTable::kHarmonics;
constexpr double kPi = std::numbers::pi;

// A wave's series as complex amplitudes (see harmonicsOf()): b sin(n t) + a cos(n t) is Re((a - i b) e^(i n t)).
Spectrum waveSpectrum(OpWave wave) {
    Spectrum s(kHarmonics + 1);
    for (int n = 1; n <= kHarmonics; ++n) {
        double a = 0.0, b = 0.0;
        waveHarmonic(wave, n, a, b);
        s[n] = {a, -b};
    }
    return s;
}

// The peak of a spectrum's cycle, found at 4096 points.
double peakOf(const Spectrum &s) {
    std::vector<float> cycle(4096);
    cycleOf(s, kHarmonics, cycle.size(), cycle.data());
    double peak = 0.0;
    for (float x : cycle)
        peak = std::max(peak, static_cast<double>(std::abs(x)));
    return peak;
}

Spectrum scaled(Spectrum s, double by) {
    for (auto &c : s)
        c *= by;
    return s;
}

// One cycle worked out at 16384 points, as a spectrum: for shapes made in time (folding, sync, FM), whose harmonics
// past the 512th then fold back far below hearing.
template <class Shape>
Spectrum timeSpectrum(const Shape &shape) {
    constexpr int kPoints = 16384;
    std::vector<float> cycle(kPoints);
    for (int i = 0; i < kPoints; ++i)
        cycle[i] = static_cast<float>(shape(static_cast<double>(i) / kPoints));
    return harmonicsOf(cycle.data(), cycle.size(), kHarmonics);
}

// A sawtooth's series, falling from 1 at its fundamental.
double sawLevel(int n) { return 1.0 / n; }

Spectrum frameSpectrum(FactoryTable table, double x) { // x: 0 at the first frame, 1 at the last
    Spectrum s(kHarmonics + 1);
    switch (table) {
    case FactoryTable::BasicShapes: {
        // Sine, triangle, saw, square, then a 25 % pulse, each scaled to peak at 1, crossfaded in turn.
        static const std::array<Spectrum, 5> shapes = [] {
            std::array<Spectrum, 5> out;
            const OpWave waves[] = {OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Pulse};
            for (int k = 0; k < 5; ++k) {
                out[k] = waveSpectrum(waves[k]);
                out[k] = scaled(out[k], 1.0 / peakOf(out[k]));
            }
            return out;
        }();
        const double at = x * 4.0;
        const int from = std::min(static_cast<int>(at), 3);
        const double t = at - from;
        for (int n = 0; n <= kHarmonics; ++n)
            s[n] = shapes[from][n] * (1.0 - t) + shapes[from + 1][n] * t;
        break;
    }
    case FactoryTable::Pwm: { // a pulse from half the cycle down to 3 % of it
        const double width = 0.5 + (0.03 - 0.5) * x;
        for (int n = 1; n <= kHarmonics; ++n) {
            const double c = 2.0 * kPi * n * width;
            const double a = 2.0 / (kPi * n) * std::sin(c), b = 2.0 / (kPi * n) * (1.0 - std::cos(c));
            s[n] = {a, -b};
        }
        break;
    }
    case FactoryTable::HarmonicBuild: { // a saw's partials, one more a frame, from 1 to 64
        const int count = 1 + static_cast<int>(std::lround(x * 63.0));
        for (int n = 1; n <= count; ++n)
            s[n] = {0.0, -sawLevel(n)};
        break;
    }
    case FactoryTable::FmSweep: { // a sine phase-modulated by itself (1:1), its index from 0 to 8
        const double index = 8.0 * x;
        s = timeSpectrum([index](double t) { return std::sin(2.0 * kPi * t + index * std::sin(2.0 * kPi * t)); });
        break;
    }
    case FactoryTable::SyncSweep: { // a saw hard-synced at 1 to 8 times its cycle
        const double ratio = 1.0 + 7.0 * x;
        s = timeSpectrum([ratio](double t) {
            const double p = ratio * t;
            return 1.0 - 2.0 * (p - std::floor(p));
        });
        break;
    }
    case FactoryTable::ResonantSweep: { // a saw through a resonant low-pass, its cutoff from harmonic 64 down to 2
        const double cutoff = 64.0 * std::pow(2.0 / 64.0, x), q = 4.0;
        for (int n = 1; n <= kHarmonics; ++n) {
            const double r = n / cutoff;
            const double gain = 1.0 / std::sqrt((1.0 - r * r) * (1.0 - r * r) + (r / q) * (r / q));
            s[n] = {0.0, -sawLevel(n) * gain};
        }
        break;
    }
    case FactoryTable::Vowels: { // a buzz through the formants of a, e, i, o and u in turn, as the Vowel wave's "ah"
        // Each vowel's three formants, as harmonics of a buzz at about 110 Hz.
        constexpr double kFormants[5][3] = {
            {6.6, 9.9, 22.2}, {4.8, 16.7, 22.5}, {2.5, 20.8, 27.4}, {5.2, 7.6, 21.9}, {2.7, 7.9, 20.4}};
        const double at = x * 4.0;
        const int from = std::min(static_cast<int>(at), 3);
        const double t = at - from;
        double f[3];
        for (int k = 0; k < 3; ++k)
            f[k] = kFormants[from][k] + (kFormants[from + 1][k] - kFormants[from][k]) * t;
        auto peak = [](double n, double at, double width) { return 1.0 / (1.0 + (n - at) * (n - at) / (width * width)); };
        for (int n = 1; n <= kHarmonics; ++n)
            s[n] = {0.0, -(0.15 + peak(n, f[0], 1.2) + 0.7 * peak(n, f[1], 1.5) + 0.3 * peak(n, f[2], 2.5)) / n};
        break;
    }
    case FactoryTable::Fold: { // a sine driven 1 to 8 times over the fold's edge, folded back each time it passes it
        const double drive = 1.0 + 7.0 * x;
        s = timeSpectrum([drive](double t) {
            const double v = drive * std::sin(2.0 * kPi * t);
            return std::asin(std::sin(0.5 * kPi * v)) * 2.0 / kPi; // a triangle fold: 1 at the edge, back down past it
        });
        break;
    }
    case FactoryTable::OddEven: { // a saw's odd harmonics fading out as its even ones fade in, the fundamental kept
        for (int n = 1; n <= kHarmonics; ++n) {
            const double level = n == 1 ? 1.0 : n % 2 == 1 ? 1.0 - x : x;
            s[n] = {0.0, -sawLevel(n) * level};
        }
        break;
    }
    case FactoryTable::Count:
        break;
    }
    return s;
}

} // namespace

std::vector<float> factoryTableFrames(int table) {
    if (table < 0 || table >= static_cast<int>(FactoryTable::Count))
        return {};
    std::vector<float> frames(static_cast<size_t>(kFactoryFrames) * FrameTable::kFrameSize);
    for (int f = 0; f < kFactoryFrames; ++f) {
        const Spectrum s = frameSpectrum(static_cast<FactoryTable>(table), static_cast<double>(f) / (kFactoryFrames - 1));
        cycleOf(s, kHarmonics, FrameTable::kFrameSize, &frames[static_cast<size_t>(f) * FrameTable::kFrameSize]);
    }
    return frames;
}

std::shared_ptr<const OpData> makeFactoryTable(int table) {
    std::vector<float> frames = factoryTableFrames(table);
    if (frames.empty())
        return nullptr;
    auto data = std::make_shared<OpData>();
    data->kind = DataKind::Table;
    data->key = static_cast<uint32_t>(table);
    data->name = factoryTableName(table);
    data->frames = kFactoryFrames;
    data->pcm = std::move(frames);
    buildOpData(*data);
    return data;
}

} // namespace fmvibe
