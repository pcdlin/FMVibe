#include "multidyn.h"

#include <limits>

namespace fmvibe::fx {

namespace {

// As a share of a level, a gain closer than 1e-4 dB to where it is going is there (as the compressor's, see
// compressor.cpp), and under this level (-180 dB) a band's curve no longer tells levels apart, for the three-band
// dynamics' envelopes.
constexpr double kLevelSnap = 1.1513e-5;
constexpr double kLevelFloor = 1e-9;

} // namespace

void MultiDyn::start(const Context &ctx) {
    *this = MultiDyn{};
    for (int band = 0; band < kMcBands; ++band)
        gains[band] = dbToGain(ctx.p[mdGainIndex(ctx.slot, band)]);
    mix = ctx.at(FxParam::MdMix);
    output = dbToGain(ctx.at(FxParam::MdOutput));
}

bool MultiDyn::run(const Context &ctx, float *const *io, uint32_t n, const Meters &meters) {
    const ParamValues &p = ctx.p;
    const int slot = ctx.slot;
    const double sampleRate = ctx.sampleRate;
    const CrossoverCoefs crossovers = crossoverCoefs(cutoffFromNorm(ctx.at(FxParam::MdLowFreq)),
                                                     cutoffFromNorm(ctx.at(FxParam::MdHighFreq)), sampleRate);
    const double attack = smoothing(timeFromNorm(ctx.at(FxParam::MdAttack)), sampleRate);
    const double release = smoothing(timeFromNorm(ctx.at(FxParam::MdRelease)), sampleRate);
    const double knee = ctx.at(FxParam::MdKnee);
    const double range = ctx.at(FxParam::MdRange);
    const bool rms = ctx.at(FxParam::MdRms) >= 0.5;
    const double average = 1.0 - smoothing(kMdRmsSeconds, sampleRate);
    const double peakFall = smoothing(kMdPeakSeconds, sampleRate);
    const double fall = smoothing(0.33, sampleRate); // the meters fall about 26 dB a second
    std::array<double, kMcBands> above{}, down{}, below{}, up{}, gainStep{}, flatFrom{}, flatTo{};
    for (int b = 0; b < kMcBands; ++b) { // each band its own; Link is the editor's, which edits them together
        above[b] = p[mdAboveIndex(slot, b)];
        down[b] = dynRatio(p[mdDownIndex(slot, b)]);
        below[b] = p[mdBelowIndex(slot, b)];
        up[b] = dynRatio(p[mdUpIndex(slot, b)]);
        gainStep[b] = rampStep(gains[b], dbToGain(p[mdGainIndex(slot, b)]), n, ctx.rampShortest);
        // Between the Below threshold's knee and the Above one's, the curve leaves the level as it is (see
        // dynamicsGain()): compared as a level, it takes no logarithm. Empty when the knees overlap.
        flatFrom[b] = dbToGain(std::min(below[b], above[b]) + 0.5 * std::max(knee, 0.0));
        flatTo[b] = dbToGain(above[b] - 0.5 * std::max(knee, 0.0));
    }
    const double mixStep = rampStep(mix, ctx.at(FxParam::MdMix), n, ctx.rampShortest);
    const double outputStep = rampStep(output, dbToGain(ctx.at(FxParam::MdOutput)), n, ctx.rampShortest);
    std::array<double, kMcBands> most{}, loudest{}, loudestOut{};
    // Each band's gain as a factor, worked out again only when it moves: between the thresholds it rests at 0 dB.
    std::array<double, kMcBands> gainOf, bandFactor{};
    gainOf.fill(std::numeric_limits<double>::quiet_NaN());
    bool moving = false; // whether a band's gain is still on its way to its curve's, in the chunk's last sample
    for (uint32_t s = 0; s < n; ++s) {
        moving = false;
        mix += mixStep;
        output += outputStep;
        double bands[2][kMcBands];
        for (int ch = 0; ch < 2; ++ch)
            splitThree(channels[ch], io[ch][s], crossovers, bands[ch]);
        double bandGain[kMcBands];
        for (int b = 0; b < kMcBands; ++b) {
            gains[b] += gainStep[b];
            // Both sides follow the louder one, so the stereo image stays put.
            const double loud = std::max(std::abs(bands[0][b]), std::abs(bands[1][b]));
            power[b] += (loud * loud - power[b]) * average;
            peak[b] = std::max(loud, peak[b] * peakFall);
            const double level = rms ? std::sqrt(power[b]) : peak[b];
            meter[b] = std::max(level, meter[b] * fall);
            loudest[b] = std::max(loudest[b], meter[b]);
            // The envelope is there once within kGainSnapDb of the level, or once both are under the curve's floor:
            // a release would otherwise only arrive as it underflows, working the curve out every sample until then.
            const double next = level + (envelope[b] - level) * (level > envelope[b] ? attack : release);
            envelope[b] = std::abs(next - level) <= std::max(level * kLevelSnap, kLevelFloor) ? level : next;
            const double followed = envelope[b];
            if (followed > flatFrom[b] && followed < flatTo[b]) {
                gain[b] = 0.0;
            } else {
                // The lift stops short of taking the band's peaks past kMdLiftHeadroom over Below (see there).
                const double ceiling = std::min(below[b], above[b]) + kMdLiftHeadroom -
                                       20.0 * std::log10(std::max(peak[b], kLevelFloor));
                gain[b] = dynamicsGain(20.0 * std::log10(std::max(followed, kLevelFloor)), above[b], down[b],
                                       below[b], up[b], knee, std::clamp(ceiling, 0.0, range));
            }
            // Still on its way: the envelope following the level, or the peaks the lift's ceiling reads falling.
            moving = moving || followed != level || peak[b] > kLevelFloor;
            most[b] = std::min(most[b], gain[b]);
            if (gain[b] != gainOf[b]) {
                gainOf[b] = gain[b];
                bandFactor[b] = dbToGain(gain[b]);
            }
            bandGain[b] = bandFactor[b] * gains[b];
            outMeter[b] = std::max(level * bandFactor[b], outMeter[b] * fall);
            loudestOut[b] = std::max(loudestOut[b], outMeter[b]);
        }
        // The dry side is the bands' plain sum, so a partial mix doesn't comb with the crossovers' phase shift.
        for (int ch = 0; ch < 2; ++ch) {
            const double dry = bands[ch][0] + bands[ch][1] + bands[ch][2];
            const double wet = bands[ch][0] * bandGain[0] + bands[ch][1] * bandGain[1] + bands[ch][2] * bandGain[2];
            io[ch][s] = static_cast<float>((dry + (wet - dry) * mix) * output);
        }
    }
    for (int b = 0; b < kMcBands; ++b) {
        meters.reduction[b] = static_cast<float>(-most[b]);
        meters.bandLevel[b] = static_cast<float>(loudest[b]);
        // What the band does as its meters read it: their two ends held and falling alike (see MultiDyn).
        meters.bandGain[b] = static_cast<float>(loudest[b] > kLevelFloor && loudestOut[b] > 0.0
                                                    ? 20.0 * std::log10(loudestOut[b] / loudest[b])
                                                    : gain[b]);
    }
    return moving;
}

} // namespace fmvibe::fx
