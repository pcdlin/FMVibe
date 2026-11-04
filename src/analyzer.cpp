#include "analyzer.h"

#include "params.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace fmvibe {

void Analyzed::clear() {
    std::fill(samples.begin(), samples.end(), 0.0f);
    write = since = 0;
    bands = {};
}

void Analyzer::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
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

void Analyzer::prepare(Analyzed &signal) {
    signal.samples.assign(kSize, 0.0f);
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
    const double binHz = sampleRate_ / kSize;
    const float scale = 4.0f / kSize;
    auto bin = [&](double k) {
        const int i = std::clamp(static_cast<int>(k), 0, kSize / 2 - 1);
        const double t = std::clamp(k - i, 0.0, 1.0);
        return static_cast<float>(std::abs(fft_[i]) * (1.0 - t) + std::abs(fft_[i + 1]) * t);
    };
    constexpr float kFall = 0.87f; // per measurement, about 1.2 dB
    for (int band = 0; band < kSpectrumBands; ++band) {
        const double lo = cutoffFromNorm(static_cast<double>(band) / kSpectrumBands) / binHz;
        const double hi = cutoffFromNorm(static_cast<double>(band + 1) / kSpectrumBands) / binHz;
        float level = bin(0.5 * (lo + hi));
        for (int k = static_cast<int>(std::ceil(lo)); k <= hi && k < kSize / 2; ++k)
            level = std::max(level, std::abs(fft_[k]));
        level *= scale;
        signal.bands[band] = std::max(level, signal.bands[band] * kFall);
    }
}

} // namespace fmvibe
