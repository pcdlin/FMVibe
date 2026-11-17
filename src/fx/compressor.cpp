#include "compressor.h"

#include <limits>

namespace fmvibe::fx {

namespace {

// A gain (dB) closer than this to where it is going is there: a release would otherwise only arrive as it underflows,
// a minute and more on, working its gain out every sample until then.
constexpr double kGainSnapDb = 1e-4;

// One step of a gain (dB) toward `target`, by the attack's coefficient while it has to come down, else the
// release's; there once within kGainSnapDb.
double followGain(double gain, double target, double attack, double release) {
    const double next = target + (gain - target) * (target < gain ? attack : release);
    return std::abs(next - target) < kGainSnapDb ? target : next;
}

// Moves a band's gain reduction (dB) toward what the curve asks for at detector
// level `peak`: at the attack pace while it has to come down, else at the release pace.
// `quiet`: the level (1 is full scale) below which the curve leaves it alone, under the knee, where the reduction
// heads for 0 without the logarithm worked out.
// Returns whether it is still on its way there.
bool followReduction(double &reduction, double peak, double thresholdDb, double ratio, double kneeDb, double attack,
                     double release, double quiet) {
    double target = 0.0;
    if (peak >= quiet) {
        const double inDb = 20.0 * std::log10(std::max(peak, 1e-9));
        target = compressorCurve(inDb, thresholdDb, ratio, kneeDb) - inDb;
    }
    reduction = followGain(reduction, target, attack, release);
    return reduction != target;
}

} // namespace

void Compressor::start(const Context &ctx) {
    *this = {0.0, dbToGain(ctx.at(FxParam::CompMakeup)), ctx.at(FxParam::CompMix)};
}

bool Compressor::run(const Context &ctx, float *const *io, uint32_t n, const Meters &meters) {
    const double threshold = ctx.at(FxParam::CompThreshold);
    const double ratio = ctx.at(FxParam::CompRatio);
    const double knee = ctx.at(FxParam::CompKnee);
    const double attack = smoothing(timeFromNorm(ctx.at(FxParam::CompAttack)), ctx.sampleRate);
    const double release = smoothing(timeFromNorm(ctx.at(FxParam::CompRelease)), ctx.sampleRate);
    const double makeupStep = rampStep(makeup, dbToGain(ctx.at(FxParam::CompMakeup)), n, ctx.rampShortest);
    const double mixStep = rampStep(mix, ctx.at(FxParam::CompMix), n, ctx.rampShortest);
    // Below the knee the curve leaves the level as it is (see compressorCurve()): compared as a level, it takes no
    // logarithm.
    const double quiet = dbToGain(threshold - 0.5 * std::max(knee, 0.0));
    double most = 0.0;
    // The reduction as a gain, worked out again only when it moves: below the threshold it rests at 0 dB.
    double gainOf = std::numeric_limits<double>::quiet_NaN(), reductionGain = 1.0;
    bool moving = false;
    for (uint32_t s = 0; s < n; ++s) {
        makeup += makeupStep;
        mix += mixStep;
        // Both sides follow the louder one, so the stereo image stays put.
        const double peak = std::max(std::abs(io[0][s]), std::abs(io[1][s]));
        moving = followReduction(reduction, peak, threshold, ratio, knee, attack, release, quiet);
        const double now = reduction;
        most = std::min(most, now);
        if (now != gainOf) {
            gainOf = now;
            reductionGain = dbToGain(now);
        }
        const double gain = reductionGain * makeup;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            io[ch][s] = static_cast<float>(x + (x * gain - x) * mix);
        }
    }
    meters.reduction = {static_cast<float>(-most), 0.0f, 0.0f};
    return moving;
}

} // namespace fmvibe::fx
