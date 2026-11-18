#include "shaper.h"

#include <initializer_list>
#include <limits>

namespace fmvibe::fx {

namespace {

// How long a shaper effect's path, going to Stack or leaving it, takes to fade over to the other (see run()), as
// the amp's settings do: a stack's stages start from silence, and its level as it is driven is unlike the others'.
constexpr double kShaperPathFade = 0.01; // seconds

} // namespace

void Shaper::prepare(double sampleRate) {
    const double transition = halfBandTransition(sampleRate);
    for (auto *bands : {&up, &down})
        for (HalfBand &band : *bands)
            band.design(HalfBand::kMaxCoefs, transition);
}

void Shaper::start(const Context &ctx) {
    const ParamValues &p = ctx.p;
    const ShaperSettings settings = shaperSettings([&p](int index) { return p[index]; }, shaperParams(ctx.slot));
    sides = {};
    // Its drawn curve too, taken at once as it starts rather than gliding in from the one it last ran with (a
    // patch or two ago, say).
    curve.built = false;
    curve.points.fill(std::numeric_limits<double>::quiet_NaN());
    drive = settings.drive;
    output = settings.output;
    mix = settings.mix;
    between = settings.between;
    ran = false; // its path starts as it is, from silence, fading in from nothing it left
    wasFade = 0.0;
}

// The voices' shapers run the same way (see Synth::renderBlock()), on what goes into them, at their oversampling; the
// shaper effect runs at twice the host rate. The anti-aliasing averages the curve over each sample step, which for what
// the curve leaves straight is the mean of two samples: at the host rate, a top falling away toward its half, 5 dB at
// 15 kHz at 48 kHz for a quiet sound the curve leaves alone, and a stack's further, stage by stage (see kStackLift);
// at twice it, about a dB. Going to Stack or leaving it under sound, the path it leaves runs on for kShaperPathFade on
// the same samples, fading out as the new one fades in.
void Shaper::run(const Context &ctx, float *const *io, uint32_t n, Meters meters) {
    float peak = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
        for (uint32_t s = 0; s < n; ++s)
            peak = std::max(peak, std::abs(io[ch][s]));
    meters.bandLevel[0] = std::max(peak, static_cast<float>(meters.bandLevel[0] * shaperLevelFall(n / ctx.sampleRate)));
    const ParamValues &p = ctx.p;
    auto get = [&p](int index) { return p[index]; };
    const ShaperParams fields = shaperParams(ctx.slot);
    const ShaperSettings settings = shaperSettings(get, fields);
    if (settings.mode == ClipMode::Custom) { // a drawn curve glides to new points
        curve.update(get, fields.curve);
        curve.advance(n / ctx.sampleRate);
    }
    const bool isStack = settings.mode == ClipMode::Stack;
    if (ran && isStack != stacked) {
        // The path it leaves fades out from where it is (one already fading out, which this one replaces, goes).
        was = last;
        wasSides = sides;
        wasFade = 1.0;
    }
    if (!ran) // from silence, rather than from what they last held
        for (int ch = 0; ch < 2; ++ch) {
            up[ch].reset();
            down[ch].reset();
        }
    stacked = isStack;
    ran = true;
    last = {settings.mode, settings.symmetric, settings.stack};
    const double rate = 2.0 * ctx.sampleRate;
    ClipCurve now = curve.curve(settings.mode, settings.symmetric);
    now.stack(settings.stack, rate);
    const double fadeStep = 1.0 / n;
    const bool block = clipNeedsBlocker(settings.mode);
    const double pole = clipBlockerPole(rate);
    // The path it left.
    ClipCurve wasCurve = curve.curve(was.mode, was.symmetric);
    wasCurve.stack(was.stack, rate);
    const bool wasBlock = clipNeedsBlocker(was.mode);
    const double wasStep = 1.0 / (kShaperPathFade * ctx.sampleRate);
    const double driveStep = rampStep(drive, settings.drive, n, ctx.rampShortest);
    const double outputStep = rampStep(output, settings.output, n, ctx.rampShortest);
    const double mixStep = rampStep(mix, settings.mix, n, ctx.rampShortest);
    const double betweenStep = rampStep(between, settings.between, n, ctx.rampShortest);
    for (uint32_t s = 0; s < n; ++s) {
        drive += driveStep;
        output += outputStep;
        mix += mixStep;
        between += betweenStep;
        if (now.from)
            now.fade = (s + 1) * fadeStep;
        const bool fading = wasFade > 0.0;
        double w = 0.0; // the path it left's share, along a raised cosine
        if (fading) {
            if (wasCurve.from)
                wasCurve.fade = (s + 1) * fadeStep;
            wasFade = std::max(wasFade - wasStep, 0.0);
            w = 0.5 - 0.5 * std::cos(std::numbers::pi * wasFade);
        }
        for (int ch = 0; ch < 2; ++ch) {
            double twice[2]; // up to twice the rate, both through the curve (and the path it left), and back down
            up[ch].processUp(io[ch][s], twice[0], twice[1]);
            for (double &x : twice) {
                double y = sides[ch].run(now, x, drive, output, mix, block, pole, between);
                if (fading)
                    y = y * (1.0 - w) + w * wasSides[ch].run(wasCurve, x, drive, output, mix, wasBlock, pole, between);
                x = y;
            }
            io[ch][s] = static_cast<float>(down[ch].process(twice[0], twice[1]));
        }
    }
}

} // namespace fmvibe::fx
