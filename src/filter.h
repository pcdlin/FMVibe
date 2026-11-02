#pragma once

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace fmvibe {

// Topology-preserving state-variable filter (Zavalishin/Simper): stays stable
// when its cutoff moves from block to block.
class Svf {
public:
    struct Coefs {
        double a1 = 1.0;
        double a2 = 0.0;
        double a3 = 0.0;
        double k = 2.0; // damping, 1/Q
    };

    struct Outputs {
        double low;
        double band;
        double high;
    };

    static Coefs coefs(double cutoffHz, double k, double sampleRate) {
        const double g = std::tan(std::numbers::pi * std::clamp(cutoffHz, 10.0, 0.45 * sampleRate) / sampleRate);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        return {a1, g * a1, g * g * a1, k};
    }

    Outputs process(double in, const Coefs &c) {
        const double v3 = in - ic2_;
        const double v1 = c.a1 * ic1_ + c.a2 * v3;
        const double v2 = ic2_ + c.a2 * ic1_ + c.a3 * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        return {v2, v1, in - c.k * v1 - v2};
    }

private:
    double ic1_ = 0.0;
    double ic2_ = 0.0;
};

// Comb delay line length for a sample rate: long enough for the lowest cutoff.
inline int combLength(double sampleRate) {
    return static_cast<int>(std::ceil(sampleRate / kMinCutoff)) + 4;
}

// Resonance 0..1 to SVF damping: 0 is Butterworth (Q 0.71), 1 rings at Q of about 18.
inline double filterDamping(double resonance) {
    return std::numbers::sqrt2 * (1.0 - 0.96 * std::clamp(resonance, 0.0, 1.0));
}
// The same for a 6 dB stage, taken from the SVF as low + band (or high + band): at 0 that is a plain one-pole, and
// resonance adds a peak at the cutoff as high as a 12 dB stage's, the slope past it staying 6 dB per octave.
inline double onePoleDamping(double resonance) {
    return std::numbers::sqrt2 * filterDamping(resonance);
}

inline double combFeedback(double resonance) {
    return 0.97 * std::clamp(resonance, 0.0, 1.0);
}

// One filter's settings for a block, derived from its params.
struct FilterSettings {
    FilterType type = FilterType::LowPass;
    // The slope as it plays (see playedSlope()): 6 dB is the resonant stage alone in its one-pole form, 12 the
    // resonant stage, and 18 and 24 add a plain second stage after it, one-pole or Butterworth, so the resonance stays
    // a single peak.
    int slope = kDefaultSlope;
    double drive = 1.0; // input gain into the saturator; 1 = clean
    Svf::Coefs main;
    Svf::Coefs second;
    double combDelay = 2.0; // samples
    double combFeedback = 0.0;
    double combGain = 1.0;
};

// cutoffHz already includes key tracking. `slope` (0..3, as stored) only matters to the types with a slope.
inline FilterSettings filterSettings(FilterType type, int slope, double cutoffHz, double resonance, double drive,
                                     double sampleRate) {
    FilterSettings s;
    s.type = type;
    s.slope = playedSlope(type, slope);
    s.drive = 1.0 + 8.0 * std::clamp(drive, 0.0, 1.0);
    switch (type) {
    case FilterType::LowPass:
    case FilterType::HighPass:
    case FilterType::BandPass:
    case FilterType::Notch:
        s.main = Svf::coefs(cutoffHz, s.slope == 0 ? onePoleDamping(resonance) : filterDamping(resonance), sampleRate);
        if (s.slope >= 2) // a one-pole for 18 dB, a Butterworth for 24
            s.second = Svf::coefs(cutoffHz, s.slope == 2 ? 2.0 : std::numbers::sqrt2, sampleRate);
        break;
    case FilterType::Comb:
        s.combDelay = std::max(sampleRate / cutoffHz, 2.0);
        s.combFeedback = combFeedback(resonance);
        s.combGain = std::sqrt(1.0 - s.combFeedback * s.combFeedback); // keeps broadband level steady
        break;
    case FilterType::Count:
        break;
    }
    return s;
}

// One channel of one voice's filter. The comb's delay line (`combSize` floats,
// see combLength) is owned by the synth.
class Filter {
public:
    double process(double in, const FilterSettings &s, float *comb, int combSize) {
#ifdef __EMSCRIPTEN__
        // WebAssembly can't flush denormals to zero as the plugin's process() does, and a voice's filters ringing
        // out in silence decay into them, at many times the CPU. A signal far below hearing keeps them above.
        guard_ = -guard_;
        in += guard_;
#endif
        if (s.drive > 1.0) {
            // The saturator averaged over the last sample step (antiderivative
            // anti-aliasing), so the harmonics it adds alias far less.
            const double driven = in * s.drive;
            const double dx = driven - lastDriven_;
            double y;
            if (std::abs(dx) < 1e-6) {
                y = std::tanh(0.5 * (driven + lastDriven_));
                lastKnown_ = false;
            } else {
                const double from = lastKnown_ ? lastLogCosh_ : logCosh(lastDriven_);
                lastLogCosh_ = logCosh(driven);
                lastKnown_ = true;
                y = (lastLogCosh_ - from) / dx;
            }
            lastDriven_ = driven;
            in = y / std::sqrt(s.drive);
        } else {
            lastDriven_ = in;
            lastKnown_ = false;
        }
        switch (s.type) {
        case FilterType::LowPass:
        case FilterType::HighPass:
        case FilterType::BandPass:
        case FilterType::Notch: {
            const double y = stage(a_, in, s.type, s.main, s.slope == 0);
            return s.slope >= 2 ? stage(b_, y, s.type, s.second, s.slope == 2) : y;
        }
        case FilterType::Comb: {
            double read = combPos_ - std::min(s.combDelay, combSize - 2.0);
            if (read < 0.0)
                read += combSize;
            if (read >= combSize) // a hair below 0 rounds up to the line's end when wrapped: that is its start
                read -= combSize;
            const int i = static_cast<int>(read);
            const double delayed = comb[i] + (comb[(i + 1) % combSize] - comb[i]) * (read - i);
            const double y = in + s.combFeedback * delayed;
            comb[combPos_] = static_cast<float>(y);
            combPos_ = (combPos_ + 1) % combSize;
            return y * s.combGain;
        }
        case FilterType::Count:
            break;
        }
        return in;
    }

private:
    // log(cosh(x)), the antiderivative of tanh, without overflowing for large x.
    static double logCosh(double x) {
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
    }

    // One 12 dB stage of the types with a slope, or for a low- or high-pass a 6 dB one (`onePole`).
    static double stage(Svf &svf, double in, FilterType type, const Svf::Coefs &c, bool onePole) {
        const Svf::Outputs o = svf.process(in, c);
        switch (type) {
        case FilterType::LowPass:
            return onePole ? o.low + o.band : o.low;
        case FilterType::HighPass:
            return onePole ? o.high + o.band : o.high;
        case FilterType::BandPass:
            return c.k * o.band; // unity gain at the center
        case FilterType::Notch:
            return o.low + o.high;
        default:
            return o.low;
        }
    }

    Svf a_;
    Svf b_;
    int combPos_ = 0;
    double lastDriven_ = 0.0; // the saturator's previous input
    double lastLogCosh_ = 0.0; // and logCosh() of it, when worked out already (lastKnown_)
    bool lastKnown_ = false;
#ifdef __EMSCRIPTEN__
    double guard_ = 1e-20; // see process()
#endif
};

// Magnitude response at `hz`, from the analog prototypes, for drawing. `slope` is as stored (0..3).
inline double filterResponse(FilterType type, int slope, double cutoffHz, double resonance, double hz) {
    auto secondOrder = [](double w, double k) { return std::sqrt((1.0 - w * w) * (1.0 - w * w) + (k * w) * (k * w)); };
    const double w = hz / cutoffHz;
    slope = playedSlope(type, slope);
    auto stage = [&](double k, bool onePole) {
        switch (type) {
        case FilterType::LowPass: // (1 + s) over the second order for the one-pole form
            return (onePole ? std::sqrt(1.0 + w * w) : 1.0) / secondOrder(w, k);
        case FilterType::HighPass: // (s^2 + s)
            return (onePole ? w * std::sqrt(1.0 + w * w) : w * w) / secondOrder(w, k);
        case FilterType::BandPass:
            return k * w / secondOrder(w, k);
        case FilterType::Notch:
            return std::abs(1.0 - w * w) / secondOrder(w, k);
        default:
            return 1.0 / secondOrder(w, k);
        }
    };
    switch (type) {
    case FilterType::LowPass:
    case FilterType::HighPass:
    case FilterType::BandPass:
    case FilterType::Notch: {
        const double first = slope == 0 ? stage(onePoleDamping(resonance), true) : stage(filterDamping(resonance), false);
        return first * (slope == 2 ? stage(2.0, true) : slope == 3 ? stage(std::numbers::sqrt2, false) : 1.0);
    }
    case FilterType::Comb: {
        const double fb = combFeedback(resonance);
        return std::sqrt(1.0 - fb * fb) /
               std::sqrt(1.0 + fb * fb - 2.0 * fb * std::cos(2.0 * std::numbers::pi * hz / cutoffHz));
    }
    case FilterType::Count:
        break;
    }
    return 1.0;
}

} // namespace fmvibe
