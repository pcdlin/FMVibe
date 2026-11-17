#include "delay.h"

namespace fmvibe::fx {

void Delay::prepare(double sampleRate) {
    const size_t longest = static_cast<size_t>(std::ceil(kMaxDelay * sampleRate)) + 4;
    for (ZeroBuffer &line : lines)
        line.assign(longest, 0.0f);
}

double Delay::target(const Context &ctx, int side) const {
    if (ctx.at(FxParam::DelayLink) >= 0.5)
        side = 0; // linked, the right side waits as long as the left
    const double seconds =
        ctx.at(FxParam::DelaySync) >= 0.5
            ? divisionSeconds(ctx.at(side == 0 ? FxParam::DelayDivisionL : FxParam::DelayDivisionR), ctx.tempo)
            : timeFromNorm(ctx.at(side == 0 ? FxParam::DelayTimeL : FxParam::DelayTimeR));
    // In whole samples: once the time has glided there, each echo is the sound as it went in, where one read between
    // two samples would come back a little duller every time round.
    const double size = static_cast<double>(lines[0].size());
    return std::clamp(std::round(seconds * ctx.sampleRate), 3.0, size - 3.0);
}

void Delay::start(const Context &ctx) {
    for (ZeroBuffer &line : lines) // cleared as the echoes reach: lines 4 s long would take a while to clear
        line.restart(ran);
    ran = 0;
    write = 0;
    length = {target(ctx, 0), target(ctx, 1)};
    lowPass = {};
    highPass = {};
    feedback = ctx.at(FxParam::DelayFeedback);
    dry = delayDryLevel(ctx.at(FxParam::DelayMix));
    wet = delayWetLevel(ctx.at(FxParam::DelayMix));
    width = ctx.at(FxParam::DelayWidth);
}

double Delay::run(const Context &ctx, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const double sampleRate = ctx.sampleRate;
    const double targets[2] = {target(ctx, 0), target(ctx, 1)};
    const double glide = 1.0 - std::exp(-1.0 / (0.02 * sampleRate)); // time changes glide, like tape
    const double mix = ctx.at(FxParam::DelayMix);
    const double feedbackStep = rampStep(feedback, ctx.at(FxParam::DelayFeedback), n, ctx.rampShortest);
    const double dryStep = rampStep(dry, delayDryLevel(mix), n, ctx.rampShortest);
    const double wetStep = rampStep(wet, delayWetLevel(mix), n, ctx.rampShortest);
    const double widthStep = rampStep(width, ctx.at(FxParam::DelayWidth), n, ctx.rampShortest);
    // The low-pass fades in over the first tenth of its travel, so the tone knob has no step at the middle.
    const double toneValue = ctx.at(FxParam::DelayTone);
    const DelayTone tone = delayTone(toneValue);
    const double lowPassCoef = tone.lowPassHz > 0.0 ? onePole(tone.lowPassHz, sampleRate) : 1.0;
    const double lowPassAmount = std::clamp(-toneValue * 10.0, 0.0, 1.0);
    const double highPassCoef = onePole(tone.highPassHz, sampleRate);
    const bool pingPong = ctx.at(FxParam::DelayPingPong) >= 0.5;
    const int size = static_cast<int>(lines[0].size());
    // What the lines held before the delay started is cleared only where this chunk's echoes read it, behind its
    // start: each length is on its way to its target (see ZeroBuffer::restart()).
    for (int c = 0; c < 2; ++c) {
        const auto [from, to] = readsBehind(size, write, n, std::min(length[c], targets[c]),
                                            std::max(length[c], targets[c]));
        lines[c].clearStale(ran - n, n + 1, from, to);
    }

    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        feedback += feedbackStep;
        dry += dryStep;
        wet += wetStep;
        width += widthStep;
        double echo[2];
        for (int c = 0; c < 2; ++c) {
            length[c] += (targets[c] - length[c]) * glide;
            // On a cubic while the time glides, so the echoes going round keep their top.
            const double read = readCubic(lines[c], write, length[c]);
            // Each echo passes the tone filters again, so repeats get darker (or thinner).
            lowPass[c] += lowPassCoef * (read - lowPass[c]);
            const double toned = read + (lowPass[c] - read) * lowPassAmount;
            highPass[c] += highPassCoef * (toned - highPass[c]);
            echo[c] = toned - highPass[c];
        }
        const double x[2] = {io[0][s], io[1][s]};
        // Ping-pong: the input goes in on the left, and each echo crosses to the other side.
        lines[0][write] = static_cast<float>(pingPong ? 0.5 * (x[0] + x[1]) + feedback * echo[1]
                                                      : x[0] + feedback * echo[0]);
        lines[1][write] = static_cast<float>(pingPong ? feedback * echo[0] : x[1] + feedback * echo[1]);
        write = write + 1 == size ? 0 : write + 1;
        // Width narrows what comes out toward the middle; the echoes themselves keep their sides.
        const double middle = 0.5 * (echo[0] + echo[1]);
        const double side = 0.5 * (echo[0] - echo[1]) * width;
        io[0][s] = static_cast<float>(x[0] * dry + (middle + side) * wet);
        io[1][s] = static_cast<float>(x[1] * dry + (middle - side) * wet);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(echo[0]), std::abs(echo[1])});
    }
    return peak;
}

} // namespace fmvibe::fx
