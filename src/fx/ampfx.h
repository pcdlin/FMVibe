#pragma once

#include "common.h"

#include "../amp.h"

namespace fmvibe::fx {

// The amp effect (its sound is src/amp.h's): the one running at its setting (model, cabinet, mic, oversampling, ...)
// and, for 10 ms after the setting changes, the one it left, fading out as the new one fades in. Large, so a slot
// holds it on the heap, from prepare().
struct AmpFx {
    AmpCore now, was;
    double wasFade = 0.0;           // 1 .. 0
    double output = 1.0, mix = 1.0; // their gains at the end of the last chunk

    void prepare(double sampleRate);
    void start(const Context &ctx);
    // `dry`: what went in, as late as the amp's oversampling makes its wet sound, for its Mix. How far its stages
    // saturate goes to the meters' band 0.
    void run(const Context &ctx, float *const *io, uint32_t n, const float (*dry)[kChunk], const Meters &meters);
};

} // namespace fmvibe::fx
