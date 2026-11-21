#include "flanger.h"

#include "../fxlag.h"

#include <complex>

namespace fmvibe {

FlangerSweep flangerSweep(double depth) {
    const double reach = 0.95 * std::clamp(depth, 0.0, 1.0);
    return {std::sqrt((1.0 - reach) * (1.0 + reach)), 0.5 * std::log((1.0 + reach) / (1.0 - reach))};
}

double flangerDelay(double delayNorm, double depth, double lfo) {
    const FlangerSweep sweep = flangerSweep(depth);
    return timeFromNorm(delayNorm) * sweep.middle * std::exp(sweep.reach * lfo);
}

double flangerResponse(double hz, double delaySeconds, double feedback, double mix, double polarity) {
    using Complex = std::complex<double>;
    const Complex delay = std::polar(1.0, -2.0 * std::numbers::pi * hz * delaySeconds);
    const double g = polarity * sweepFeedback(feedback);
    return std::abs(1.0 - mix + polarity * mix * sweepGain(feedback) * delay / (1.0 - g * delay));
}

namespace fx {

namespace {

// The longest a line's read reaches: the sweep's longest end (1.95 times the longest Delay, 15 ms), or through zero
// the dry sound's wait on top of the most the sweep runs past its middle (1.64 times it), with room.
constexpr double kMaxFlangerDelay = 0.034;
// The fastest Delay and Depth move where it reads that it follows, in samples of delay a sample (its pitch bent by as
// much, 1 % here, as the chorus's): moving faster, as they jump, it crossfades to where it now reads over
// kFlangerJumpFadeSeconds instead, where following them would move where it reads faster than the sound goes by, a
// click.
constexpr double kFlangerSlew = 0.01;
constexpr double kFlangerJumpFadeSeconds = 0.01;

} // namespace

void Flanger::prepare(double sampleRate) {
    for (ZeroBuffer &line : lines)
        line.assign(static_cast<size_t>(std::ceil(kMaxFlangerDelay * sampleRate)) + 4, 0.0f);
    for (ZeroBuffer &line : dryLines)
        line.assign(static_cast<size_t>(throughZeroWaitSamples(sampleRate)), 0.0f);
}

void Flanger::start(const Context &ctx) {
    for (ZeroBuffer &line : lines)
        line.zero(ran);
    for (ZeroBuffer &line : dryLines)
        line.zero(ran);
    ran = 0;
    write = dryWrite = 0;
    last = {};
    throughZero = ctx.at(FxParam::FlangerThroughZero) >= 0.5 ? 1.0 : 0.0;
    phase = 0.0;
    delayKnown = false;
    mix = sweepWet(ctx.at(FxParam::FlangerMix));
    polarity = sweepPolarity(ctx.at(FxParam::FlangerNegative));
}

void Flanger::idle(double cycles) {
    phase += cycles;
    phase -= std::floor(phase);
    delayKnown = false; // worked out again where the sweep is now, at Delay and Depth as they are then
}

double Flanger::run(const Context &ctx, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const ParamValues &p = ctx.p;
    const double sampleRate = ctx.sampleRate;
    const double step = sweepHz([&p](int i) { return p[i]; }, ctx.slot, FxType::Flanger, ctx.tempo) / sampleRate;
    const FlangerSweep sweep = flangerSweep(ctx.at(FxParam::FlangerDepth)); // flangerDelay()'s, the same all chunk
    const FlangerReads want{timeFromNorm(ctx.at(FxParam::FlangerDelay)) * sweep.middle * sampleRate, sweep.reach};
    const double feedback = sweepFeedback(ctx.at(FxParam::FlangerFeedback));
    const double gain = sweepGain(ctx.at(FxParam::FlangerFeedback));
    const double mixStep = rampStep(mix, sweepWet(ctx.at(FxParam::FlangerMix)), n, ctx.rampShortest);
    const double polarityTo = sweepPolarity(ctx.at(FxParam::FlangerNegative));
    const double polarityStep = 2.0 / (kPolaritySeconds * sampleRate);
    const double spread = ctx.at(FxParam::FlangerStereo) >= 0.5 ? 0.25 : 0.0; // the right side's head start
    const int size = static_cast<int>(lines[0].size());
    const int wait = static_cast<int>(dryLines[0].size()); // read before it is written over, `wait` samples on
    const double zeroTo = ctx.at(FxParam::FlangerThroughZero) >= 0.5 ? 1.0 : 0.0;
    const double zeroStep = 1.0 / (kThroughZeroFadeSeconds * sampleRate);
    // The delay in samples as `r` has it read where the sweep is at `phase`, on each side, and through zero where the
    // swept copy is read (the dry sound's wait, then as far off the sweep's middle as the sweep is): worked out every
    // kSweepStep samples, as the phaser's coefficient is (see Phaser::run()). 3 samples at the least, for its cubic.
    auto sweepAt = [&](const FlangerReads &r, double at, int ch, double &zeroAt) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (at + spread * ch));
        const double swept = r.middle * std::exp(r.reach * lfo); // flangerDelay()
        zeroAt = std::clamp(wait + swept - r.middle, 3.0, size - 3.0);
        return std::clamp(swept, 3.0, size - 3.0);
    };
    // Delay and Depth moving where it reads (the swept copy, and through zero) no faster than kFlangerSlew, it follows
    // them; jumping, it crossfades from where it reads to where they now put it. Another jump during a crossfade waits
    // for it to end.
    bool jumped = false;
    if (!delayKnown) {
        reads = want;
        fadeLeft = 0;
    } else if (want != reads) {
        double moved = 0.0;
        for (int ch = 0; ch < 2; ++ch) {
            double zeroWant = 0.0, zeroNow = 0.0;
            moved = std::max(moved, std::abs(sweepAt(want, phase, ch, zeroWant) - sweepAt(reads, phase, ch, zeroNow)));
            moved = std::max(moved, std::abs(zeroWant - zeroNow));
        }
        if (moved <= kFlangerSlew * n) {
            reads = want;
        } else if (fadeLeft == 0) {
            fadeReads = reads;
            fadeDelay = delay;
            fadeZeroDelay = zeroDelay;
            reads = want;
            fadeLength = fadeLeft = std::max(1u, static_cast<uint32_t>(kFlangerJumpFadeSeconds * sampleRate));
            jumped = true;
        }
    }
    if (!delayKnown || jumped) { // where it reads at once (fading in, after a jump)
        for (int ch = 0; ch < 2; ++ch)
            delay[ch] = sweepAt(reads, phase, ch, zeroDelay[ch]);
        delayKnown = true;
    }
    std::array<double, 2> delayTo{}, delayStep{}, zeroDelayTo{}, zeroDelayStep{};
    std::array<double, 2> fadeTo{}, fadeStep{}, fadeZeroTo{}, fadeZeroStep{};
    uint32_t stepLeft = 0;
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            double ahead = phase;
            for (uint32_t k = 0; k < stepLeft; ++k) {
                ahead += step;
                ahead -= std::floor(ahead);
            }
            for (int ch = 0; ch < 2; ++ch) {
                delayTo[ch] = sweepAt(reads, ahead, ch, zeroDelayTo[ch]);
                delayStep[ch] = (delayTo[ch] - delay[ch]) / stepLeft;
                zeroDelayStep[ch] = (zeroDelayTo[ch] - zeroDelay[ch]) / stepLeft;
                if (fadeLeft > 0) {
                    fadeTo[ch] = sweepAt(fadeReads, ahead, ch, fadeZeroTo[ch]);
                    fadeStep[ch] = (fadeTo[ch] - fadeDelay[ch]) / stepLeft;
                    fadeZeroStep[ch] = (fadeZeroTo[ch] - fadeZeroDelay[ch]) / stepLeft;
                }
            }
        }
        --stepLeft;
        // How far the crossfade has come, on a raised cosine: what it reads where it reads now.
        const bool fading = fadeLeft > 0;
        double now = 1.0;
        if (fading) {
            --fadeLeft;
            now = 0.5 - 0.5 * std::cos(std::numbers::pi * (1.0 - static_cast<double>(fadeLeft) / fadeLength));
        }
        mix += mixStep;
        polarity = moveToward(polarity, polarityTo, polarityStep);
        phase += step;
        phase -= std::floor(phase);
        throughZero = moveToward(throughZero, zeroTo, zeroStep);
        // The share of the sound through zero as it goes over to it, along a raised cosine.
        const double late = throughZero >= 1.0 ? 1.0 : 0.5 - 0.5 * std::cos(std::numbers::pi * throughZero);
        for (int ch = 0; ch < 2; ++ch) {
            delay[ch] = stepLeft == 0 ? delayTo[ch] : delay[ch] + delayStep[ch];
            zeroDelay[ch] = stepLeft == 0 ? zeroDelayTo[ch] : zeroDelay[ch] + zeroDelayStep[ch];
            if (fading) {
                fadeDelay[ch] = stepLeft == 0 ? fadeTo[ch] : fadeDelay[ch] + fadeStep[ch];
                fadeZeroDelay[ch] = stepLeft == 0 ? fadeZeroTo[ch] : fadeZeroDelay[ch] + fadeZeroStep[ch];
            }
            // On a cubic: through a straight line between two samples, what goes round would lose its top and come
            // back grainy as the sweep moves.
            double delayed = readCubic(lines[ch], write, delay[ch]);
            if (fading)
                delayed = now * delayed + (1.0 - now) * readCubic(lines[ch], write, fadeDelay[ch]);
            const double in = io[ch][s];
            // The dry sound always goes through its line, so through zero starts with what came before.
            const double waited = dryLines[ch][dryWrite];
            dryLines[ch][dryWrite] = static_cast<float>(in);
            double x = in, wet = delayed;
            if (late > 0.0) {
                double zeroed = readCubic(lines[ch], write, zeroDelay[ch]);
                if (fading)
                    zeroed = now * zeroed + (1.0 - now) * readCubic(lines[ch], write, fadeZeroDelay[ch]);
                x = in + (waited - in) * late;
                wet = delayed + (zeroed - delayed) * late;
            }
            lines[ch][write] = static_cast<float>(in + polarity * feedback * delayed);
            last[ch] = delayed;
            io[ch][s] = static_cast<float>(x + (polarity * wet * gain - x) * mix);
            peak = std::max({peak, std::abs(in), std::abs(delayed), std::abs(waited)});
        }
        write = write + 1 == size ? 0 : write + 1;
        dryWrite = dryWrite + 1 == wait ? 0 : dryWrite + 1;
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
