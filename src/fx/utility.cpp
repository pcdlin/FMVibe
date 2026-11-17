#include "utility.h"

namespace fmvibe {

UtilityGains utilityGains(double gainNorm, double pan) {
    const double gain = utilityGain(gainNorm);
    const double p = std::clamp(pan, -1.0, 1.0);
    return {gain * std::min(1.0 - p, 1.0), gain * std::min(1.0 + p, 1.0)};
}

namespace fx {

void Utility::start(const Context &ctx) {
    highPass = Biquad{};
    const UtilityGains g = utilityGains(ctx.at(FxParam::UtilityGain), ctx.at(FxParam::UtilityPan));
    gainL = g.left;
    gainR = g.right;
    width = ctx.at(FxParam::UtilityWidth);
    hz = utilityMonoBassHz(ctx.at(FxParam::UtilityMonoBass));
    mono = hz > 0.0 ? 1.0 : 0.0;
}

void Utility::run(const Context &ctx, float *const *io, uint32_t n) {
    const double sampleRate = ctx.sampleRate;
    // Switched off, the high-pass keeps the corner it had while it fades out; switched on from silence, it starts
    // cleared, at the corner asked for.
    const double asked = utilityMonoBassHz(ctx.at(FxParam::UtilityMonoBass));
    if (asked > 0.0) {
        if (mono <= 0.0)
            highPass = Biquad{};
        hz = asked;
    }
    const Coefs hp = biquad(BiquadType::HighPass, std::clamp(hz, kMinCutoff, 0.45 * sampleRate), sampleRate);
    const BiquadCoefs highPassCoefs{hp.b0, hp.b1, hp.b2, hp.a1, hp.a2};
    const UtilityGains g = utilityGains(ctx.at(FxParam::UtilityGain), ctx.at(FxParam::UtilityPan));
    const double leftStep = rampStep(gainL, g.left, n, ctx.rampShortest),
                 rightStep = rampStep(gainR, g.right, n, ctx.rampShortest);
    const double widthStep = rampStep(width, ctx.at(FxParam::UtilityWidth), n, ctx.rampShortest);
    // Mono Bass switching on or off fades between the sides as they are and high-passed, as the polarity turns over.
    const double monoTarget = asked > 0.0 ? 1.0 : 0.0, monoStep = 1.0 / (kPolaritySeconds * sampleRate);
    for (uint32_t s = 0; s < n; ++s) {
        gainL += leftStep;
        gainR += rightStep;
        width += widthStep;
        mono = moveToward(mono, monoTarget, monoStep);
        const double mid = 0.5 * (io[0][s] + io[1][s]);
        double side = 0.5 * (io[0][s] - io[1][s]);
        const double high = mono > 0.0 ? highPass.process(side, highPassCoefs) : side; // off, it needn't run
        side = (side + (high - side) * mono) * width;
        io[0][s] = static_cast<float>((mid + side) * gainL);
        io[1][s] = static_cast<float>((mid - side) * gainR);
    }
}

} // namespace fx
} // namespace fmvibe
