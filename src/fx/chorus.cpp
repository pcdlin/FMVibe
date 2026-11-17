#include "chorus.h"

namespace fmvibe {

ChorusGains chorusGains(int voice, int voices, double spread) {
    const double pan = voices <= 1 ? 0.0 : std::clamp(spread, 0.0, 1.0) * (-1.0 + 2.0 * voice / (voices - 1.0));
    const double angle = (1.0 + pan) * 0.25 * std::numbers::pi;
    const double scale = std::numbers::sqrt2 / std::sqrt(static_cast<double>(std::max(voices, 1)));
    return {scale * std::cos(angle), scale * std::sin(angle)};
}

namespace fx {

namespace {

// The fastest the chorus's voices' sweeps move apart or together as Voices changes, in cycles a second, and their
// places along the delays, in Delays a second: a voice's pitch then wavers a little more for a moment, rather than
// jumping.
constexpr double kChorusGlide = 1.0;
constexpr double kChorusStaggerGlide = 0.2;
// How long a chorus voice takes to fade in or out as Voices changes, or to move between the sides as Spread does.
constexpr double kChorusFadeSeconds = 0.01;

int chorusVoices(const Context &ctx) {
    return static_cast<int>(std::clamp(std::lround(ctx.at(FxParam::ChorusVoices)),
                                       static_cast<long>(kMinChorusVoices), static_cast<long>(kMaxChorusVoices)));
}

} // namespace

void Chorus::prepare(double sampleRate) {
    const double longest = kMaxChorusDelay * (1.0 + kChorusStagger + kChorusSweep + kChorusDrift);
    for (ZeroBuffer &line : lines)
        line.assign(static_cast<size_t>(std::ceil(longest * sampleRate)) + 4, 0.0f);
}

void Chorus::start(const Context &ctx) {
    for (ZeroBuffer &line : lines)
        line.zero(ran);
    ran = 0;
    write = 0;
    bass = {};
    phase = 0.0;
    delayKnown = false;
    mix = ctx.at(FxParam::ChorusMix);
    // Its voices where Voices and Spread put them at once; those it doesn't play silent, their sweeps and places
    // where they would be with one voice more, so one coming in glides little. Their drifts start apart.
    const int voices = chorusVoices(ctx);
    for (int k = 0; k < kMaxChorusVoices; ++k) {
        offsets[k] = chorusOffset(k, std::max(voices, k + 1));
        stagger[k] = chorusStagger(k, std::max(voices, k + 1));
        drift[k] = 0.37 * k - std::floor(0.37 * k);
        const ChorusGains g = k < voices ? chorusGains(k, voices, ctx.at(FxParam::ChorusSpread)) : ChorusGains{};
        gainL[k] = g.left;
        gainR[k] = g.right;
    }
}

void Chorus::idle(double cycles) {
    phase += cycles;
    phase -= std::floor(phase);
    delayKnown = false; // worked out again where the sweep is now
}

double Chorus::run(const Context &ctx, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const ParamValues &p = ctx.p;
    const double sampleRate = ctx.sampleRate;
    const double step = sweepHz([&p](int i) { return p[i]; }, ctx.slot, FxType::Chorus, ctx.tempo) / sampleRate;
    const double delaySamples = timeFromNorm(ctx.at(FxParam::ChorusDelay)) * sampleRate; // the same all chunk
    const double depth = std::clamp(ctx.at(FxParam::ChorusDepth), 0.0, 1.0);
    const double swing = kChorusSweep * depth, wander = kChorusDrift * depth;
    const double feedback = sweepFeedback(ctx.at(FxParam::ChorusFeedback));
    const double gain = sweepGain(ctx.at(FxParam::ChorusFeedback)); // as the flanger's, so feedback doesn't ring louder
    const double mixStep = rampStep(mix, ctx.at(FxParam::ChorusMix), n, ctx.rampShortest);
    const int voices = chorusVoices(ctx);
    const double spread = ctx.at(FxParam::ChorusSpread);
    const double bassCoef = onePole(kChorusBassHz, sampleRate);
    const int size = static_cast<int>(lines[0].size());
    // The voices' sweeps and places glide toward where Voices puts them, and their gains ramp to where Voices and
    // Spread put them; a voice silent and staying so is left out (snapped to silence once its ramp has all but ended,
    // which its last steps' rounding may not quite reach).
    const uint32_t fade = std::max(ctx.rampShortest, static_cast<uint32_t>(kChorusFadeSeconds * sampleRate));
    std::array<double, kMaxChorusVoices> stepL{}, stepR{}, driftStep{};
    std::array<bool, kMaxChorusVoices> heard{};
    for (int k = 0; k < kMaxChorusVoices; ++k) {
        if (k >= voices && std::abs(gainL[k]) < 1e-9 && std::abs(gainR[k]) < 1e-9)
            gainL[k] = gainR[k] = 0.0;
        if (k < voices) {
            offsets[k] = moveToward(offsets[k], chorusOffset(k, voices), kChorusGlide * n / sampleRate);
            stagger[k] = moveToward(stagger[k], chorusStagger(k, voices), kChorusStaggerGlide * n / sampleRate);
        }
        const ChorusGains g = k < voices ? chorusGains(k, voices, spread) : ChorusGains{};
        stepL[k] = rampStep(gainL[k], g.left, n, fade);
        stepR[k] = rampStep(gainR[k], g.right, n, fade);
        heard[k] = k < voices || gainL[k] != 0.0 || gainR[k] != 0.0;
        driftStep[k] = kChorusDriftHz[k] / sampleRate;
    }
    // Each voice's delay in samples with the sweep at `at` and its drift at `drifted`: worked out every kSweepStep
    // samples, as the flanger's is (see Flanger::run()). 3 samples at the least, for its cubic.
    auto delayAt = [&](double at, double drifted, int k) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (at - offsets[k]));
        const double share = 1.0 + stagger[k] + swing * lfo + wander * std::sin(2.0 * std::numbers::pi * drifted);
        return std::clamp(delaySamples * share, 3.0, size - 3.0);
    };
    if (!delayKnown) {
        for (int k = 0; k < kMaxChorusVoices; ++k)
            delay[k] = delayAt(phase, drift[k], k);
        delayKnown = true;
    }
    auto wrap = [](double cycles) { return cycles - std::floor(cycles); };
    std::array<double, kMaxChorusVoices> delayTo{}, delayStep{};
    uint32_t stepLeft = 0;
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            const double ahead = wrap(phase + step * stepLeft);
            for (int k = 0; k < kMaxChorusVoices; ++k) {
                delayTo[k] = delayAt(ahead, wrap(drift[k] + driftStep[k] * stepLeft), k);
                delayStep[k] = (delayTo[k] - delay[k]) / stepLeft;
            }
        }
        --stepLeft;
        mix += mixStep;
        phase = wrap(phase + step);
        const double x[2] = {io[0][s], io[1][s]};
        double high[2];
        for (int c = 0; c < 2; ++c) { // the bass goes past the voices
            bass[c] += bassCoef * (x[c] - bass[c]);
            high[c] = x[c] - bass[c];
        }
        double wet[2] = {0.0, 0.0}, back[2] = {0.0, 0.0};
        for (int k = 0; k < kMaxChorusVoices; ++k) {
            drift[k] = wrap(drift[k] + driftStep[k]);
            delay[k] = stepLeft == 0 ? delayTo[k] : delay[k] + delayStep[k];
            if (!heard[k])
                continue;
            gainL[k] += stepL[k];
            gainR[k] += stepR[k];
            const double left = readCubic(lines[0], write, delay[k]);
            const double right = readCubic(lines[1], write, delay[k]);
            wet[0] += gainL[k] * left;
            wet[1] += gainR[k] * right;
            // Its share of what goes round: 1 / voices once its gains are where they go (whose squares add up to
            // 2 / voices at any Spread), fading in or out with them. The shares add up to 1 at most, also while
            // voices come and go, so Feedback under 1 keeps what goes round from growing.
            const double share = 0.5 * (gainL[k] * gainL[k] + gainR[k] * gainR[k]);
            back[0] += share * left;
            back[1] += share * right;
        }
        for (int c = 0; c < 2; ++c) {
            lines[c][write] = static_cast<float>(high[c] + feedback * back[c]);
            io[c][s] = static_cast<float>(x[c] + (gain * wet[c] - high[c]) * mix);
            peak = std::max({peak, std::abs(x[c]), std::abs(wet[c])});
        }
        write = write + 1 == size ? 0 : write + 1;
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
