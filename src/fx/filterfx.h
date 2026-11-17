#pragma once

#include "common.h"

#include "../filter.h"
#include "../oversampler.h"

#include <limits>

namespace fmvibe::fx {

// The filter effect: a voice filter a side. Run at twice the rate (see run()), as the voices' filters run
// oversampled: up through `up` and back down through `down`, a side each, designed in prepare().
struct FilterFx {
    std::array<Filter, 2> channels{};
    std::array<ZeroBuffer, 2> combs; // comb delay lines, combLength() long at twice the rate
    std::array<HalfBand, 2> up, down;
    double mix = 1.0;
    // Its settings, and the params (and pitch, while it tracks the key) they were worked out from: NaN until the
    // first chunk, so worked out again only when one moves (see run()).
    FilterSettings settings;
    std::array<double, 10> settingsFor{std::numeric_limits<double>::quiet_NaN()};

    // Allocates the comb lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample its filters put out, so a resonant comb keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fmvibe::fx
