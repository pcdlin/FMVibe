#include "analyzer.h"

#include "params.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace fmvibe {

void Analyzed::clear() {
    std::fill(samples.begin(), samples.end(), 0.0f);
    write = 0;
    since = start;
    bands = {};
}

void Analyzer::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    // 0.87 (about 1.2 dB) a measurement at 48 kHz, as many measurements a second as there are at this rate.
    fall_ = static_cast<float>(std::pow(0.87, 48000.0 / sampleRate));
    for (int band = 0; band <= kSpectrumBands; ++band)
        edges_[band] = cutoffFromNorm(static_cast<double>(band) / kSpectrumBands) / (sampleRate / kSize);
    window_.resize(kSize);
    twiddles_.resize(kSize / 2);
    fft_.resize(kSize);
    for (int i = 0; i < kSize; ++i)
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kSize));
    for (int k = 0; k < kSize / 2; ++k)
        twiddles_[k] = std::polar(1.0f, static_cast<float>(-2.0 * std::numbers::pi * k / kSize));
    constexpr int kBits = std::countr_zero(static_cast<unsigned>(kSize));
    reversed_.resize(kSize);
    for (int i = 0; i < kSize; ++i) {
        unsigned reversed = 0;
        for (int b = 0; b < kBits; ++b)
            reversed |= ((static_cast<unsigned>(i) >> b) & 1u) << (kBits - 1 - b);
        reversed_[i] = static_cast<int>(reversed);
    }
}

void Analyzer::prepare(Analyzed &signal, int stagger, int of) {
    signal.samples.assign(kSize, 0.0f);
    signal.start = of > 0 ? stagger % of * (kSize / 2) / of : 0;
    signal.clear();
}

void Analyzer::measure(Analyzed &signal) {
    // Windowed, oldest sample first, in bit-reversed order for the radix-2 transform.
    for (int i = 0; i < kSize; ++i)
        fft_[reversed_[i]] = signal.samples[(signal.write + i) & (kSize - 1)] * window_[i];
    for (int size = 2; size <= kSize; size *= 2) {
        const int half = size / 2, stride = kSize / size;
        for (int start = 0; start < kSize; start += size)
            for (int k = 0; k < half; ++k) {
                const std::complex<float> odd = fft_[start + k + half] * twiddles_[k * stride];
                fft_[start + k + half] = fft_[start + k] - odd;
                fft_[start + k] += odd;
            }
    }
    // Each band takes the loudest bin in it; a band narrower than a bin reads between the two nearest.
    // A full-scale sine's bin comes to N/4 through the Hann window.
    const float scale = 4.0f / kSize;
    auto bin = [&](double k) {
        const int i = std::clamp(static_cast<int>(k), 0, kSize / 2 - 1);
        const double t = std::clamp(k - i, 0.0, 1.0);
        return static_cast<float>(std::abs(fft_[i]) * (1.0 - t) + std::abs(fft_[i + 1]) * t);
    };
    for (int band = 0; band < kSpectrumBands; ++band) {
        const double lo = edges_[band], hi = edges_[band + 1];
        float loudest = 0.0f; // compared as powers, one square root a band
        for (int k = static_cast<int>(std::ceil(lo)); k <= hi && k < kSize / 2; ++k)
            loudest = std::max(loudest, std::norm(fft_[k]));
        const float level = std::max(bin(0.5 * (lo + hi)), std::sqrt(loudest)) * scale;
        signal.bands[band] = std::max(level, signal.bands[band] * fall_);
    }
}

} // namespace fmvibe
