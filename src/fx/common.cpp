#include "common.h"

namespace fmvibe {

double sweepFeedback(double feedback) {
    return 0.9 * std::clamp(feedback, -1.0, 1.0);
}

double sweepGain(double feedback) {
    const double g = sweepFeedback(feedback);
    return std::sqrt(1.0 - g * g);
}

namespace fx {

Coefs biquad(BiquadType type, double hz, double sampleRate) {
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosW = std::cos(w);
    const double alpha = std::sin(w) / std::numbers::sqrt2; // sin(w) / (2Q), Q = 1/sqrt(2)
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    switch (type) {
    case BiquadType::LowPass:
        b0 = b2 = (1.0 - cosW) * 0.5;
        b1 = 1.0 - cosW;
        break;
    case BiquadType::HighPass:
        b0 = b2 = (1.0 + cosW) * 0.5;
        b1 = -(1.0 + cosW);
        break;
    case BiquadType::AllPass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cosW;
        b2 = 1.0 + alpha;
        break;
    }
    const double a0 = 1.0 + alpha;
    return {b0 / a0, b1 / a0, b2 / a0, -2.0 * cosW / a0, (1.0 - alpha) / a0};
}

CrossoverCoefs crossoverCoefs(double lowHz, double highHz, double sampleRate) {
    // Kept below where the biquads hold up, the high crossover at least a third of an octave above the low one.
    lowHz = std::min(lowHz, 0.3 * sampleRate);
    highHz = std::clamp(highHz, lowHz * 1.26, 0.45 * sampleRate);
    auto coefs = [&](BiquadType type, double hz) {
        const Coefs b = biquad(type, hz, sampleRate);
        return BiquadCoefs{b.b0, b.b1, b.b2, b.a1, b.a2};
    };
    return {coefs(BiquadType::LowPass, lowHz), coefs(BiquadType::HighPass, lowHz), coefs(BiquadType::LowPass, highHz),
            coefs(BiquadType::HighPass, highHz), coefs(BiquadType::AllPass, highHz)};
}

} // namespace fx
} // namespace fmvibe
