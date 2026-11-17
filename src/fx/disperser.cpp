#include "disperser.h"

#include <complex>

namespace fmvibe {

std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate) {
    const double hz = std::min(cutoffFromNorm(freqNorm), 0.45 * sampleRate);
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    const double q = 0.3 * std::pow(20.0, std::clamp(pinch, 0.0, 1.0));
    const double alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    return {(1.0 - alpha) / a0, -2.0 * std::cos(w) / a0, 1.0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate) {
    // An all-pass's delay is how fast its phase turns with frequency.
    const std::array<double, 5> c = disperserCoefs(freqNorm, pinch, sampleRate);
    auto phase = [&](double w) {
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        return std::arg((c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2));
    };
    const double w = 2.0 * std::numbers::pi * std::min(hz, 0.45 * sampleRate) / sampleRate;
    const double dw = 1e-4 * w + 1e-7;
    double turn = phase(w + dw) - phase(w - dw);
    turn -= 2.0 * std::numbers::pi * std::round(turn / (2.0 * std::numbers::pi));
    return -turn / (2.0 * dw) * stages / sampleRate;
}

namespace fx {

void Disperser::start(const Context &ctx) {
    *this = Disperser{};
    mix = ctx.at(FxParam::DisperserMix);
}

double Disperser::run(const Context &ctx, float *const *io, uint32_t n) {
    const std::array<double, 5> b =
        disperserCoefs(ctx.at(FxParam::DisperserFreq), ctx.at(FxParam::DisperserPinch), ctx.sampleRate);
    const BiquadCoefs c{b[0], b[1], b[2], b[3], b[4]};
    const int count =
        std::clamp(static_cast<int>(std::lround(ctx.at(FxParam::DisperserStages))), 1, kMaxDisperserStages);
    for (int ch = 0; ch < 2; ++ch)
        for (int k = count; k < kMaxDisperserStages; ++k)
            stages[ch][k] = Biquad{};
    const double mixStep = rampStep(mix, ctx.at(FxParam::DisperserMix), n, ctx.rampShortest);
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        mix += mixStep;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            double y = x;
            for (int k = 0; k < count; ++k)
                y = stages[ch][k].process(y, c);
            io[ch][s] = static_cast<float>(x + (y - x) * mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
