#pragma once

// A small radix-2 complex FFT, in place, for building tables on the main thread (see opdata.h): a forward transform
// of a frame, then inverse ones of its spectrum cut down per level. Not for the audio thread (it allocates its
// twiddles once per size).

#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

namespace fmvibe {

// Transforms `x` (a power of two long) in place: forward, X[k] = sum x[n] e^(-2 pi i k n / N); inverse, the same with
// e^(+...) and no scaling. False for a length that isn't a power of two.
inline bool fft(std::vector<std::complex<double>> &x, bool inverse) {
    const size_t n = x.size();
    if (n == 0 || (n & (n - 1)) != 0)
        return false;
    for (size_t i = 1, j = 0; i < n; ++i) { // bit-reversed order
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(x[i], x[j]);
    }
    for (size_t size = 2; size <= n; size <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * std::numbers::pi / static_cast<double>(size);
        const size_t half = size / 2;
        // The twiddles of this stage, each worked out rather than stepped, so rounding doesn't build up.
        std::vector<std::complex<double>> w(half);
        for (size_t k = 0; k < half; ++k)
            w[k] = std::polar(1.0, angle * static_cast<double>(k));
        for (size_t start = 0; start < n; start += size)
            for (size_t k = 0; k < half; ++k) {
                const std::complex<double> odd = x[start + k + half] * w[k];
                x[start + k + half] = x[start + k] - odd;
                x[start + k] += odd;
            }
    }
    return true;
}

// The harmonics 1..`count` of one cycle of `samples` (a power of two long), as complex amplitudes c[n] such that the
// cycle is sum over n of Re(c[n] e^(2 pi i n t)) plus its DC; c[0] is the DC. Harmonics past what the length holds are 0.
inline std::vector<std::complex<double>> harmonicsOf(const float *samples, size_t length, int count) {
    std::vector<std::complex<double>> x(length);
    for (size_t i = 0; i < length; ++i)
        x[i] = samples[i];
    std::vector<std::complex<double>> out(static_cast<size_t>(count) + 1);
    if (!fft(x, false))
        return out;
    const double scale = 2.0 / static_cast<double>(length);
    out[0] = x[0] / static_cast<double>(length);
    for (int k = 1; k <= count && static_cast<size_t>(k) < length / 2; ++k)
        out[k] = x[k] * scale;
    return out;
}

// One cycle of `size` samples (a power of two) from complex amplitudes as harmonicsOf() gives them, harmonics 1..`top`
// (the DC left out), into `out`.
inline void cycleOf(const std::vector<std::complex<double>> &harmonics, int top, size_t size, float *out) {
    std::vector<std::complex<double>> x(size);
    for (int k = 1; k <= top && static_cast<size_t>(k) < harmonics.size() && static_cast<size_t>(k) < size / 2; ++k) {
        x[k] = harmonics[k] * 0.5;
        x[size - k] = std::conj(harmonics[k]) * 0.5;
    }
    fft(x, true);
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<float>(x[i].real());
}

} // namespace fmvibe
