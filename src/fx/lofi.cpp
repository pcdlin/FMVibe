#include "lofi.h"

namespace fmvibe {

LoFiSettings loFiSettings(double bits, double rateNorm, double sampleRate) {
    const double resolution = std::clamp(std::round(bits), 1.0, static_cast<double>(kMaxLoFiBits));
    const double rateHz = std::min(cutoffFromNorm(rateNorm), sampleRate);
    return {std::exp2(1.0 - resolution), rateHz, std::min(0.45 * rateHz, 0.45 * sampleRate)};
}

namespace fx {

void LoFi::start(const Context &ctx) {
    held = {};
    quiet = 0;
    phase = 1.0;
    smooth = {};
    smoothCoefs = BiquadRamp{};
    smoothMix = ctx.at(FxParam::LoFiSmooth);
    mix = ctx.at(FxParam::LoFiMix);
}

void LoFi::run(const Context &ctx, float *const *io, uint32_t n) {
    const LoFiSettings set =
        loFiSettings(ctx.at(FxParam::LoFiBits), ctx.at(FxParam::LoFiRate), ctx.sampleRate);
    const double advance = set.rateHz / ctx.sampleRate; // of a sample's time, each host sample
    smoothCoefs.begin(biquadCoefs(biquad(BiquadType::LowPass, set.smoothHz, ctx.sampleRate)), n, ctx.rampShortest);
    const double smoothStep = rampStep(smoothMix, ctx.at(FxParam::LoFiSmooth), n, ctx.rampShortest);
    const double mixStep = rampStep(mix, ctx.at(FxParam::LoFiMix), n, ctx.rampShortest);
    const int quietAfter = static_cast<int>(kLoFiQuietSeconds * ctx.sampleRate);
    for (uint32_t s = 0; s < n; ++s) {
        smoothMix += smoothStep;
        mix += mixStep;
        smoothCoefs.next();
        phase += advance;
        const bool take = phase >= 1.0; // a new sample, held until the next
        if (take)
            phase -= std::floor(phase);
        quiet = std::abs(io[0][s]) < 1e-6f && std::abs(io[1][s]) < 1e-6f ? std::min(quiet + 1, quietAfter) : 0;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            if (take)
                held[ch] = quiet >= quietAfter ? 0.0 : loFiLevel(x, set.step);
            const double smoothed = smooth[ch].process(held[ch], smoothCoefs.now);
            const double wet = held[ch] + (smoothed - held[ch]) * smoothMix;
            io[ch][s] = static_cast<float>(x + (wet - x) * mix);
        }
    }
    smoothCoefs.end();
}

} // namespace fx
} // namespace fmvibe
