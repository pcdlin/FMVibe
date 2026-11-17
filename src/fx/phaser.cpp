#include "phaser.h"

#include <complex>

namespace fmvibe {

double phaserCorner(double centerNorm, double depth, double lfo) {
    return cutoffFromNorm(centerNorm) * std::exp2(2.5 * depth * lfo);
}

double phaserResponse(double hz, double cornerHz, int stages, double feedback, double mix, double polarity) {
    using Complex = std::complex<double>;
    const Complex s(0.0, hz / cornerHz);
    const Complex stage = (1.0 - s) / (1.0 + s); // the analog first-order all-pass
    Complex chain = 1.0;
    for (int k = 0; k < stages; ++k)
        chain *= stage;
    const double g = polarity * sweepFeedback(feedback);
    return std::abs(1.0 - mix + polarity * mix * sweepGain(feedback) * chain / (1.0 - g * chain));
}

namespace fx {

void Phaser::start(const Context &ctx) {
    *this = Phaser{};
    mix = sweepWet(ctx.at(FxParam::PhaserMix));
    polarity = sweepPolarity(ctx.at(FxParam::PhaserNegative));
}

void Phaser::idle(double cycles) {
    phase += cycles;
    phase -= std::floor(phase);
    coefKnown = false; // worked out again where the sweep is now
}

double Phaser::run(const Context &ctx, float *const *io, uint32_t n) {
    const ParamValues &p = ctx.p;
    const double sampleRate = ctx.sampleRate;
    const double step = sweepHz([&p](int i) { return p[i]; }, ctx.slot, FxType::Phaser, ctx.tempo) / sampleRate;
    const double centerHz = cutoffFromNorm(ctx.at(FxParam::PhaserCenter)); // phaserCorner()'s, the same all chunk
    const double depth = ctx.at(FxParam::PhaserDepth);
    const double feedback = sweepFeedback(ctx.at(FxParam::PhaserFeedback));
    const double gain = sweepGain(ctx.at(FxParam::PhaserFeedback));
    const int count = std::clamp(static_cast<int>(std::lround(ctx.at(FxParam::PhaserStages))), 1, kMaxPhaserStages);
    for (int ch = 0; ch < 2; ++ch)
        for (int k = count; k < kMaxPhaserStages; ++k)
            stages[ch][k] = 0.0;
    const double mixStep = rampStep(mix, sweepWet(ctx.at(FxParam::PhaserMix)), n, ctx.rampShortest);
    const double polarityTo = sweepPolarity(ctx.at(FxParam::PhaserNegative));
    const double polarityStep = 2.0 / (kPolaritySeconds * sampleRate); // the whole way, -1 to 1
    const double spread = ctx.at(FxParam::PhaserStereo) >= 0.5 ? 0.25 : 0.0; // the right side's head start
    // The all-passes' coefficient where the sweep is at `phase`, on each side.
    auto coefAt = [&](double at, int ch) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (at + spread * ch));
        const double corner = centerHz * std::exp2(2.5 * depth * lfo); // phaserCorner()
        const double t = std::tan(std::numbers::pi * std::clamp(corner, 20.0, 0.45 * sampleRate) / sampleRate);
        return (t - 1.0) / (t + 1.0);
    };
    if (!coefKnown) {
        coef = {coefAt(phase, 0), coefAt(phase, 1)};
        coefKnown = true;
    }
    double peak = 0.0;
    // Every kSweepStep samples the coefficient is worked out where the sweep will be at the step's last sample, and
    // moves there in a straight line from where it was.
    std::array<double, 2> coefTo{}, coefStep{};
    uint32_t stepLeft = 0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            double ahead = phase;
            for (uint32_t k = 0; k < stepLeft; ++k) { // as the samples move it, below
                ahead += step;
                ahead -= std::floor(ahead);
            }
            for (int ch = 0; ch < 2; ++ch) {
                coefTo[ch] = coefAt(ahead, ch);
                coefStep[ch] = (coefTo[ch] - coef[ch]) / stepLeft;
            }
        }
        --stepLeft;
        mix += mixStep;
        polarity = moveToward(polarity, polarityTo, polarityStep);
        phase += step;
        phase -= std::floor(phase);
        for (int ch = 0; ch < 2; ++ch) {
            coef[ch] = stepLeft == 0 ? coefTo[ch] : coef[ch] + coefStep[ch]; // exactly there at the step's end
            const double a = coef[ch];
            const double x = io[ch][s];
            double y = x + polarity * feedback * last[ch];
            for (int k = 0; k < count; ++k) { // y = a x + z; z = x - a y
                double &z = stages[ch][k];
                const double out = a * y + z;
                z = y - a * out;
                y = out;
            }
            last[ch] = y;
            io[ch][s] = static_cast<float>(x + (polarity * y * gain - x) * mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
