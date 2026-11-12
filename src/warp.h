#pragma once

#include "params.h"

#include <algorithm>
#include <cmath>

namespace fmvibe {

// An operator's Warp: a map of where along its wave's cycle it reads, from where along the cycle it is (both 0..1).
// Every warp maps 0 to 0 and the cycle's end to its end, so the wave stays whole and continuous wherever the
// operator's own was, and at an amount of 0 every warp is the plain wave: a warp switched or swept from 0 starts from
// the wave as it was. Its steepest stretch reads the wave at most kMaxWarpSqueeze times as fast as the plain wave;
// the synth reads a table with that many times fewer harmonics, so the warped wave stays below Nyquist.
//  - Bend: a curve that rushes through the cycle's start and lingers at its end (negative: the other way round), as
//    Casio's phase distortion bends a sine toward a saw.
//  - Squeeze: the cycle's first half squeezed into less (or more) of it and its second half stretched over the rest:
//    a square's pulse width, from 50 % to a sixteenth either way.
//  - Pulse: the whole cycle squeezed into its start, then resting where it began (negative: resting first, the cycle
//    at the end); a sixteenth of the cycle at full.
//  - Steps: the cycle in 8 steps (negative: 4), each holding then rising to the next, over an eighth of the step at
//    full.
constexpr double kMaxWarpSqueeze = 8.0;

// What warpPhase() needs from an amount, worked out once a sample rather than once a copy.
struct WarpCoefs {
    OpWarp mode = OpWarp::Off;
    double a = 0.0, b = 0.0, c = 0.0;
};

inline WarpCoefs warpCoefs(OpWarp mode, double amount) {
    const double x = std::clamp(amount, -1.0, 1.0);
    if (x == 0.0) // every warp's plain wave, exactly
        mode = OpWarp::Off;
    const double share = 1.0 - (1.0 - 1.0 / kMaxWarpSqueeze) * std::abs(x); // of the cycle (or a step) the wave takes
    WarpCoefs w{mode};
    switch (mode) {
    case OpWarp::Bend: { // p / (p + k (1 - p)): steepest at 1 / k at the start, k at the end
        constexpr double c = (kMaxWarpSqueeze - 1.0) / (kMaxWarpSqueeze + 1.0);
        w.a = (1.0 - c * x) / (1.0 + c * x); // k
        w.b = 1.0 - w.a;
        break;
    }
    case OpWarp::Squeeze: { // the first half of the wave over `width` of the cycle
        const double width = 0.5 + 0.5 * (1.0 - 1.0 / kMaxWarpSqueeze) * x;
        w.a = width;
        w.b = 0.5 / width;
        w.c = 0.5 / (1.0 - width);
        break;
    }
    case OpWarp::Pulse:
        w.a = x >= 0.0 ? share : 1.0 - share; // where the wave starts (negative) or ends
        w.b = 1.0 / share;
        w.c = x >= 0.0 ? 1.0 : 0.0;
        break;
    case OpWarp::Steps:
        w.a = x >= 0.0 ? 8.0 : 4.0; // steps
        w.b = 1.0 - share;          // of each step held
        w.c = 1.0 / share;
        break;
    default:
        break;
    }
    return w;
}

// Where along the wave's cycle a warped operator reads at `p` (0..1) of its cycle.
inline double warpPhase(const WarpCoefs &w, double p) {
    switch (w.mode) {
    case OpWarp::Bend:
        return p / (p + w.a * (1.0 - p));
    case OpWarp::Squeeze:
        return p < w.a ? p * w.b : 0.5 + (p - w.a) * w.c;
    case OpWarp::Pulse:
        if (w.c > 0.0)
            return p < w.a ? p * w.b : 0.0;
        return p < w.a ? 0.0 : (p - w.a) * w.b;
    case OpWarp::Steps: {
        const double x = p * w.a;
        const double step = std::floor(x);
        const double rise = std::max(x - step - w.b, 0.0) * w.c;
        return (step + rise) / w.a;
    }
    default:
        return p;
    }
}

// Where a warp bends the cycle (its slope changes): `perCycle` times a cycle, at `offsets` (0..1) of each of those
// stretches, its slope `before` and `after` each. The synth smooths the bend in the wave's slope there over the
// samples either side (polyBLAMP), as a band-limited bend would be.
struct WarpKinks {
    double perCycle = 1.0;
    double offsets[2]{};
    double before[2]{}, after[2]{};
    int count = 0;
};

inline WarpKinks warpKinks(const WarpCoefs &w) {
    switch (w.mode) {
    case OpWarp::Bend: // p / (p + k (1 - p)) runs at k into the cycle's end and at 1 / k out of its start
        return {1.0, {0.0, 0.0}, {w.a, 0.0}, {1.0 / w.a, 0.0}, 1};
    case OpWarp::Squeeze:
        return {1.0, {0.0, w.a}, {w.c, w.b}, {w.b, w.c}, 2};
    case OpWarp::Pulse:
        if (w.c > 0.0) // the wave, then resting from w.a on
            return {1.0, {0.0, w.a}, {0.0, w.b}, {w.b, 0.0}, 2};
        return {1.0, {0.0, w.a}, {w.b, 0.0}, {0.0, w.b}, 2}; // resting, then the wave from w.a on
    case OpWarp::Steps: // each step holds, then rises from w.b of it on
        return {w.a, {0.0, w.b}, {w.c, 0.0}, {0.0, w.c}, 2};
    default:
        return {};
    }
}

// How many times as fast as the plain wave a warp reads it at its steepest: 1 for none, up to kMaxWarpSqueeze.
inline double warpSqueeze(OpWarp mode, double amount) {
    const double x = std::min(std::abs(amount), 1.0);
    switch (mode) {
    case OpWarp::Bend: {
        constexpr double c = (kMaxWarpSqueeze - 1.0) / (kMaxWarpSqueeze + 1.0);
        return (1.0 + c * x) / (1.0 - c * x);
    }
    case OpWarp::Squeeze:
        return 0.5 / (0.5 - 0.5 * (1.0 - 1.0 / kMaxWarpSqueeze) * x);
    case OpWarp::Pulse:
    case OpWarp::Steps:
        return 1.0 / (1.0 - (1.0 - 1.0 / kMaxWarpSqueeze) * x);
    default:
        return 1.0;
    }
}

} // namespace fmvibe
