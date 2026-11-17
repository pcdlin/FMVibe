#include "eq.h"

#include <complex>

namespace fmvibe {

double eqResponse(double hz, double low, double mid, double high) {
    using Complex = std::complex<double>;
    const Complex s1(0.0, hz / kEqLowHz);
    const Complex s2(0.0, hz / kEqHighHz);
    const Complex lp1 = 1.0 / (s1 * s1 + std::numbers::sqrt2 * s1 + 1.0);
    const Complex hp1 = s1 * s1 * lp1;
    const Complex lp2 = 1.0 / (s2 * s2 + std::numbers::sqrt2 * s2 + 1.0);
    const Complex hp2 = s2 * s2 * lp2;
    const Complex ap2 = (s2 * s2 - std::numbers::sqrt2 * s2 + 1.0) * lp2;
    return std::abs(eqGain(low) * ap2 * lp1 * lp1 + eqGain(mid) * hp1 * hp1 * lp2 * lp2 +
                    eqGain(high) * hp1 * hp1 * hp2 * hp2);
}

namespace fx {

CrossoverCoefs Eq::crossovers(double sampleRate) {
    auto set = [&](BiquadCoefs &c, BiquadType type, double hz) {
        const Coefs b = biquad(type, hz, sampleRate);
        c = {b.b0, b.b1, b.b2, b.a1, b.a2};
    };
    CrossoverCoefs out;
    set(out.lp1, BiquadType::LowPass, kEqLowHz);
    set(out.hp1, BiquadType::HighPass, kEqLowHz);
    set(out.lp2, BiquadType::LowPass, kEqHighHz);
    set(out.hp2, BiquadType::HighPass, kEqHighHz);
    set(out.ap2, BiquadType::AllPass, kEqHighHz);
    return out;
}

void Eq::start(const Context &ctx) {
    channels = {};
    gains = {eqGain(ctx.at(FxParam::EqLow)), eqGain(ctx.at(FxParam::EqMid)), eqGain(ctx.at(FxParam::EqHigh))};
}

void Eq::run(const Context &ctx, const CrossoverCoefs &bands, float *const *io, uint32_t n) {
    const double targets[3] = {eqGain(ctx.at(FxParam::EqLow)), eqGain(ctx.at(FxParam::EqMid)),
                               eqGain(ctx.at(FxParam::EqHigh))};
    double steps[3];
    for (int b = 0; b < 3; ++b)
        steps[b] = rampStep(gains[b], targets[b], n, ctx.rampShortest);
    for (uint32_t s = 0; s < n; ++s) {
        for (int b = 0; b < 3; ++b)
            gains[b] += steps[b];
        for (int ch = 0; ch < 2; ++ch) {
            EqChannel &e = channels[ch];
            const double x = io[ch][s];
            const double low = e.lowPhase.process(e.lowB.process(e.lowA.process(x, bands.lp1), bands.lp1), bands.ap2);
            const double rest = e.restB.process(e.restA.process(x, bands.hp1), bands.hp1);
            const double mid = e.midB.process(e.midA.process(rest, bands.lp2), bands.lp2);
            const double high = e.highB.process(e.highA.process(rest, bands.hp2), bands.hp2);
            io[ch][s] = static_cast<float>(gains[0] * low + gains[1] * mid + gains[2] * high);
        }
    }
}

} // namespace fx
} // namespace fmvibe
