#pragma once

// What the effects (src/fx/*.h, each an effect type a slot of Effects runs) share: the chunk they run in, what a slot
// hands its effect (Context, Meters), biquads and the crossovers built from them, ramps and delay line reads.

#include "../params.h"
#include "../zerobuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace fmvibe {

// How much the phaser's and flanger's feedback (-1..1 as set) sends round, and the gain that keeps
// their broadband level where it was without feedback.
double sweepFeedback(double feedback);
double sweepGain(double feedback);
// The phaser's and flanger's polarity from their Negative switch: 1, or -1 to turn their wet sound and their
// feedback over. Their response then is the positive one's with the wet sound's phase turned half a cycle, every
// notch moved to where a peak was.
inline double sweepPolarity(double negative) {
    return negative >= 0.5 ? -1.0 : 1.0;
}
// The phaser's and flanger's Mix knobs run to 100 %, which is half wet, where their notches go deepest: past that
// they would only get shallower again.
inline double sweepWet(double mix) {
    return 0.5 * mix;
}

namespace fx {

constexpr uint32_t kChunk = 64; // samples per parameter ramp
// How often the phaser and flanger work out where their sweep is, in samples, moving in a straight line between:
// a sweep (100 Hz at most) bends too little over these few samples to tell, and each step costs a sine, an
// exponential and a tangent per side.
constexpr uint32_t kSweepStep = 8;

// What an effect runs with, from the slot it is in (see Effects::runSlot()): the params (its own read by `slot`), the
// host rate and the fewest samples a gain's or mix's ramp is spread over at it, the host's tempo (BPM) for synced
// delays and sweeps, and the pitch a filter's key tracking follows, the newest voice's (semitones, 69 is A4).
struct Context {
    const ParamValues &p;
    int slot;
    double sampleRate;
    uint32_t rampShortest;
    double tempo;
    double pitch;

    // The slot's field `f`.
    double at(FxParam f) const { return p[paramIndex(slot, f)]; }
};

// Where an effect puts what the editor's meters show for its slot (see Effects::reduction(), bandLevel() and
// bandGain()).
struct Meters {
    std::array<float, kMcBands> &reduction;
    std::array<float, kMcBands> &bandLevel;
    std::array<float, kMcBands> &bandGain;
};

struct BiquadCoefs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

struct Biquad { // transposed direct form II
    double z1 = 0.0, z2 = 0.0;

    double process(double x, const BiquadCoefs &c) {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

enum class BiquadType { LowPass, HighPass, AllPass };

struct Coefs {
    double b0, b1, b2, a1, a2;
};

// Butterworth-Q biquads (RBJ cookbook): two in a row make a Linkwitz-Riley crossover.
Coefs biquad(BiquadType type, double hz, double sampleRate);

// Linkwitz-Riley crossovers: the low band gets an allpass so the three bands
// sum back to a flat response.
struct EqChannel {
    Biquad lowA, lowB, lowPhase, restA, restB, midA, midB, highA, highB;
};

// Linkwitz-Riley crossovers at `lowHz` and `highHz` (kept apart and below where the biquads hold up), and three
// bands of `x` through them, the low one through an all-pass at the high crossover so the bands sum back flat.
struct CrossoverCoefs {
    BiquadCoefs lp1, hp1, lp2, hp2, ap2;
};
CrossoverCoefs crossoverCoefs(double lowHz, double highHz, double sampleRate);
inline void splitThree(EqChannel &e, double x, const CrossoverCoefs &c, double *bands) {
    bands[0] = e.lowPhase.process(e.lowB.process(e.lowA.process(x, c.lp1), c.lp1), c.ap2);
    const double rest = e.restB.process(e.restA.process(x, c.hp1), c.hp1);
    bands[1] = e.midB.process(e.midA.process(rest, c.lp2), c.lp2);
    bands[2] = e.highB.process(e.highA.process(rest, c.hp2), c.hp2);
}

inline double dbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

// Per-sample smoothing factor for a time constant of `seconds`.
inline double smoothing(double seconds, double sampleRate) {
    return std::exp(-1.0 / (seconds * sampleRate));
}

// One-pole smoothing coefficient for a corner at `hz`.
inline double onePole(double hz, double sampleRate) {
    return 1.0 - std::exp(-2.0 * std::numbers::pi * std::min(hz, 0.45 * sampleRate) / sampleRate);
}

// The halfband filters an effect running at twice the rate goes up and down through: flat up to 20 kHz (or as near
// the host rate's half as it is), as the voices' decimator is.
inline double halfBandTransition(double sampleRate) {
    return std::clamp(0.25 - 20000.0 / (2.0 * sampleRate), 0.01, 0.24);
}

// The fewest samples a ramp of a gain or mix is spread over: the 32 of a chunk the synth hands the effects up to
// 48 kHz, and as long a time at higher rates, where it takes more than one chunk.
inline uint32_t rampShortest(double sampleRate) {
    return std::max(32u, static_cast<uint32_t>(std::lround(32.0 * sampleRate / 48000.0)));
}

// Whether a chunk of `n` samples takes a gain or mix ramping from `from` to `to` all the way there: one as long as the
// ramp's shortest does, and a shorter one once what it would leave is too little to hear (-140 dB of the value), so
// a ramp going a share of the way each chunk still ends.
inline bool rampEnds(double from, double to, uint32_t n, uint32_t shortest) {
    return n >= shortest || std::abs(to - from) * (1.0 - static_cast<double>(n) / shortest) <
                                1e-7 * std::max(1.0, std::abs(to));
}

// The step that ramps a gain or mix from `from` to `to` over a chunk of `n` samples, the ramp `shortest` samples long
// at the least. A shorter chunk (a chunk the host's events cut short, or any at a rate above 48 kHz) goes only its
// share of the way, as the voices' levels do (see Synth::renderBlock()), so a jump ramps as smoothly however the
// block is split, and over as long a time at any rate.
inline double rampStep(double from, double to, uint32_t n, uint32_t shortest) {
    return n > 0 && rampEnds(from, to, n, shortest) ? (to - from) / n : (to - from) / std::max(n, shortest);
}

// The phaser's and flanger's polarity turns over in this long, its wet sound and feedback fading through nothing
// rather than jumping, however the host splits the block.
constexpr double kPolaritySeconds = 0.01;

inline double moveToward(double from, double to, double step) {
    return from < to ? std::min(from + step, to) : std::max(from - step, to);
}

// Reads a delay line `delay` samples behind `write` (3 or more, so the four samples it takes were all written before
// this one), between samples on a cubic (Catmull-Rom) through the two either side: a sweep moving the delay doesn't
// dull the highs and wobble them as a straight line between two samples does.
inline double readCubic(const ZeroBuffer &line, int write, double delay) {
    const int size = static_cast<int>(line.size());
    double pos = write - delay;
    while (pos < 0.0)
        pos += size;
    const int i1 = std::min(static_cast<int>(pos), size - 1);
    const double t = pos - i1;
    const int i0 = i1 == 0 ? size - 1 : i1 - 1;
    const int i2 = i1 + 1 == size ? 0 : i1 + 1;
    const int i3 = i2 + 1 == size ? 0 : i2 + 1;
    const double y0 = line[i0], y1 = line[i1], y2 = line[i2], y3 = line[i3];
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

// Where a chunk of `n` samples reads a line `size` long behind its start, written from `write` on, with a delay
// from `shortest` to `longest` samples: the stretch to clear of what it held before it started (see
// ZeroBuffer::clearStale()), a sample further either way for the interpolation and the delay's fraction.
inline std::pair<size_t, size_t> readsBehind(int size, int write, uint32_t n, double shortest, double longest) {
    if (longest + 2.0 >= size) // round the line and more: anywhere along it
        return {0, static_cast<size_t>(size)};
    const double from = size + write - longest - 2.0, to = size + write + n - shortest + 2.0;
    return {static_cast<size_t>(std::clamp(from, 0.0, static_cast<double>(size))),
            static_cast<size_t>(std::clamp(to, 0.0, static_cast<double>(size)))};
}

} // namespace fx
} // namespace fmvibe
