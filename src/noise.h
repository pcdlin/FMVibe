#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace fmvibe {

// Colored noise for the Noise wave: white noise through a cascade of one-pole
// shelves whose poles sit 1.25 octaves apart from 20 Hz up. Each shelf's zero
// sits `slope` of the way to the next pole, so from 20 Hz to 20 kHz the
// response falls by 6 dB per octave times the slope: 0 is white, 0.5 pink and
// 1 brown (every zero then cancels the next pole). The output is scaled to
// white noise's RMS, so the slope changes the color, not the loudness. When
// it runs oversampled, only what lies below the host's Nyquist counts toward
// that loudness: the rest is filtered out on the way down.
class NoiseColor {
public:
    static constexpr int kStages = 8;
    using State = std::array<double, kStages>;

    struct Coefs {
        std::array<double, kStages> zeros{};
        double gain = 1.0;
    };

    // Main thread, while the plugin is inactive. `hostRate` is the rate the
    // noise is heard at, `sampleRate` (the same or a multiple) the one it runs at.
    void prepare(double sampleRate, double hostRate) {
        sampleRate_ = sampleRate;
        top_ = std::numbers::pi * std::min(1.0, hostRate / sampleRate);
        for (int i = 0; i < kStages; ++i)
            poles_[i] = pole(kLowestHz * std::exp2(kSpacing * i));
        // The frequencies meanPower() steps through, and their cosines, are the same for every slope: worked out once.
        Steps steps;
        const double lowest = 2.0 * std::numbers::pi * 0.1 / sampleRate_; // 0.1 Hz; flat below
        steps.w[0] = lowest;
        steps.cos[0] = std::cos(lowest);
        for (int k = 1; k <= kSteps; ++k) {
            steps.w[k] = lowest * std::pow(top_ / lowest, static_cast<double>(k) / kSteps);
            steps.cos[k] = std::cos(steps.w[k]);
        }
        for (int t = 0; t <= kTable; ++t)
            gains_[t] = 1.0 / std::sqrt(meanPower(zerosFor(static_cast<double>(t) / kTable), steps));
    }

    // Once per block; slope is 0..1.
    Coefs coefs(double slope) const {
        slope = std::clamp(slope, 0.0, 1.0);
        const double at = slope * kTable;
        const int t = std::min(static_cast<int>(at), kTable - 1);
        return {zerosFor(slope), gains_[t] + (gains_[t + 1] - gains_[t]) * (at - t)};
    }

    double process(double in, const Coefs &c, State &state) const {
        double x = in;
        for (int i = 0; i < kStages; ++i) { // transposed direct form II
            const double y = x + state[i];
            state[i] = poles_[i] * y - c.zeros[i] * x;
            x = y;
        }
        return x * c.gain;
    }

private:
    static constexpr double kLowestHz = 20.0;
    static constexpr double kSpacing = 1.25; // octaves between poles: the zeros reach 20 kHz
    static constexpr int kTable = 64;        // gains per slope, interpolated

    double pole(double hz) const {
        return std::exp(-2.0 * std::numbers::pi * std::min(hz, 0.49 * sampleRate_) / sampleRate_);
    }

    std::array<double, kStages> zerosFor(double slope) const {
        std::array<double, kStages> zeros;
        for (int i = 0; i < kStages; ++i)
            zeros[i] = pole(kLowestHz * std::exp2(kSpacing * (i + slope)));
        return zeros;
    }

    // meanPower()'s steps: log-spaced, as brown noise's power sits at the bottom, from 0.1 Hz to the host's Nyquist.
    static constexpr int kSteps = 2000;
    struct Steps {
        std::array<double, kSteps + 1> w, cos;
    };

    // Mean of |H|^2 from 0 to Nyquist, counting only what lies below the host's
    // Nyquist: the output power heard for white input of power 1.
    double meanPower(const std::array<double, kStages> &zeros, const Steps &steps) const {
        auto power = [&](double cos) {
            double h = 1.0;
            for (int i = 0; i < kStages; ++i)
                h *= (1.0 - 2.0 * zeros[i] * cos + zeros[i] * zeros[i]) /
                     (1.0 - 2.0 * poles_[i] * cos + poles_[i] * poles_[i]);
            return h;
        };
        double sum = power(1.0) * steps.w[0]; // flat below the lowest step, as at 0 Hz
        double h = power(steps.cos[0]);
        for (int k = 1; k <= kSteps; ++k) {
            const double hNext = power(steps.cos[k]);
            sum += 0.5 * (h + hNext) * (steps.w[k] - steps.w[k - 1]);
            h = hNext;
        }
        return sum / std::numbers::pi;
    }

    double sampleRate_ = 48000.0;
    double top_ = std::numbers::pi; // the host's Nyquist, in radians at sampleRate_
    std::array<double, kStages> poles_{};
    std::array<double, kTable + 1> gains_{};
};

} // namespace fmvibe
