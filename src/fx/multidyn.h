#pragma once

#include "common.h"

namespace fmvibe::fx {

// The three-band dynamics. Split like the DJ EQ, at crossovers set per chunk; each band's detector follows its peaks
// (`peak`, falling back slowly between them), or its RMS level (`power`, a mean square). Its `envelope` follows that
// level at the attack pace while it rises, else at the release pace, and its gain (dB) is dynamicsGain() of the
// envelope. Following the level rather than the gain, a note out of silence takes the lift away within a few
// milliseconds, as the level climbs tens of dB in its first moments, where a gain following at the attack pace would
// take tens of milliseconds to come down from the whole Range. `meter` is the detector level the editor shows, falling
// slowly, and `outMeter` that level through the band's gain, held and falling alike: the editor draws what the band does
// from one to the other, so as a band goes quiet, the gain lifting what is left, both ends fall together rather
// than the held level going in taking the lift of the quiet that follows it.
struct MultiDyn {
    std::array<EqChannel, 2> channels{};
    std::array<double, kMcBands> peak{}, power{}, envelope{};
    std::array<double, kMcBands> gain{};
    std::array<double, kMcBands> meter{}, outMeter{};
    std::array<double, kMcBands> gains{1.0, 1.0, 1.0}; // band gains after the dynamics, ramped
    double mix = 1.0;
    double output = 1.0; // gain after the mix, ramped

    void start(const Context &ctx);
    // Returns whether a band's gain is still on its way to where its curve puts it (a release running out). Each
    // band's gain and levels go to the meters.
    bool run(const Context &ctx, float *const *io, uint32_t n, const Meters &meters);
};

} // namespace fmvibe::fx
