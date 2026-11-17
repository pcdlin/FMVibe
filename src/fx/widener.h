#pragma once

#include "common.h"

namespace fmvibe {

// The widener's taps: how long each waits at full size (seconds) and how fast it is swept (Hz).
// The first and third go to the left side, the others to the right.
constexpr int kWidenerTaps = 4;
inline constexpr double kWidenerDelays[kWidenerTaps] = {0.0097, 0.0133, 0.0179, 0.0231};
inline constexpr double kWidenerRates[kWidenerTaps] = {0.29, 0.37, 0.43, 0.53};
// Tap `tap`'s delay (seconds) with its sweep at `lfo` (-1..1), from the Size knob (0..1): from a fifth of its
// length, a tight doubling, to all of it, swept 12 % either way.
double widenerTap(int tap, double size, double lfo);
// How much of the sound at `hz` the widener widens, from its Cutoff knob (0..1 on the cutoff curve): the
// response of its high-pass, 12 dB per octave.
double widenerSpread(double hz, double highPassNorm);

namespace fx {

// The widener: the sound above the high-pass, mono, in a short line that kWidenerTaps swept taps read.
struct Widener {
    ZeroBuffer line;
    int write = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    Biquad highPass;
    std::array<double, kWidenerTaps> phases{}; // the taps' sweeps, in cycles
    double size = 0.5; // gliding toward the knob
    double width = 1.0; // ramped
    double mix = 0.5;

    // Allocates the line. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its line keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
};

} // namespace fx
} // namespace fmvibe
