#pragma once

#include <array>
#include <complex>
#include <vector>

namespace fmvibe {

// The analyzers show what they measure in this many bands, spread evenly over the log frequency axis from 20 Hz
// to 20 kHz (as the cutoff knobs are).
constexpr int kSpectrumBands = 96;
using Spectrum = std::array<float, kSpectrumBands>;

// A signal an analyzer measures: its latest Analyzer::kSize samples, mono, how many came in since the last
// measurement, and the levels that came to.
struct Analyzed {
    std::vector<float> samples;
    int write = 0;
    int since = 0;
    int start = 0; // where `since` starts (see Analyzer::prepare())
    Spectrum bands{};

    void clear();
};

// A Hann-windowed FFT of a signal's latest kSize samples every half of that. Each band takes the loudest bin in it
// (a full-scale sine reads 1); where the sound got quieter, it falls away by about 1.2 dB a measurement, so 55 dB a
// second at 48 kHz. One analyzer serves any number of signals at its rate.
class Analyzer {
public:
    static constexpr int kSize = 2048;
    static_assert((kSize & (kSize - 1)) == 0); // a power of two, for the radix-2 transform and the masks

    // Allocates, also each signal's samples (prepare(signal)). Main thread.
    void prepare(double sampleRate);
    // `stagger` of `of`: signals measured together start their counts that far apart, so their transforms fall in
    // different blocks rather than all in one.
    static void prepare(Analyzed &signal, int stagger = 0, int of = 1);

    void push(Analyzed &signal, float x) {
        signal.samples[signal.write] = x;
        signal.write = (signal.write + 1) % kSize;
        if (++signal.since >= kSize / 2) {
            signal.since = 0;
            measure(signal);
        }
    }

private:
    void measure(Analyzed &signal);

    double sampleRate_ = 48000.0;
    std::vector<float> window_;
    std::vector<std::complex<float>> twiddles_; // e^(-2 pi i k / N)
    std::vector<std::complex<float>> fft_;      // the transform being worked on
    std::vector<int> reversed_;                 // each index's bits in reverse order, for the transform's input
};

} // namespace fmvibe
