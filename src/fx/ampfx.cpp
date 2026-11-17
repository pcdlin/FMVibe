#include "ampfx.h"

namespace fmvibe::fx {

void AmpFx::prepare(double sampleRate) {
    now.prepare(sampleRate);
    was.prepare(sampleRate);
}

void AmpFx::start(const Context &ctx) {
    now.start(ampConfig(ctx.p, ctx.slot), ampKnobs(ctx.p, ctx.slot));
    wasFade = 0.0;
    output = std::pow(10.0, ctx.at(FxParam::AmpOutput) / 20.0);
    mix = ctx.at(FxParam::AmpMix);
}

void AmpFx::run(const Context &ctx, float *const *io, uint32_t n, const float (*dry)[kChunk], const Meters &meters) {
    const AmpConfig config = ampConfig(ctx.p, ctx.slot);
    const AmpKnobs knobs = ampKnobs(ctx.p, ctx.slot);
    // A new setting: the amp at the old one fades out as one at the new one, cleared, fades in. One that changes
    // again meanwhile waits for that to end.
    if (!(config == now.config()) && wasFade <= 0.0) {
        was = now;
        now.start(config, knobs);
        wasFade = 1.0;
    }
    float old[2][kChunk]; // what went in, for the amp it leaves
    if (wasFade > 0.0) {
        std::copy_n(io[0], n, old[0]);
        std::copy_n(io[1], n, old[1]);
    }
    double drive = now.process(io, n, knobs);
    if (wasFade > 0.0) {
        float *const oldIo[2] = {old[0], old[1]};
        drive = std::max(drive, was.process(oldIo, n, knobs));
        const double fadeStep = 1.0 / (0.01 * ctx.sampleRate);
        for (uint32_t s = 0; s < n; ++s) {
            wasFade = std::max(wasFade - fadeStep, 0.0);
            const float w = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * wasFade));
            for (int c = 0; c < 2; ++c)
                io[c][s] = io[c][s] * (1.0f - w) + old[c][s] * w;
        }
    }
    // Output and Mix glide sample by sample, the same however the host splits the block.
    const double outputTo = std::pow(10.0, ctx.at(FxParam::AmpOutput) / 20.0), mixTo = ctx.at(FxParam::AmpMix);
    const double glide = 1.0 - std::exp(-1.0 / (0.005 * ctx.sampleRate));
    for (uint32_t s = 0; s < n; ++s) {
        output += (outputTo - output) * glide;
        mix += (mixTo - mix) * glide;
        for (int c = 0; c < 2; ++c)
            io[c][s] = static_cast<float>(dry[c][s] * (1.0 - mix) + io[c][s] * output * mix);
    }
    meters.reduction[0] = static_cast<float>(drive);
}

} // namespace fmvibe::fx
