#pragma once

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace fmvibe {

// The shapers' curves, which the shaper effect and the voices' shapers share: the formulas (see ClipMode), a drawn
// curve read through a table, the first-order antiderivative anti-aliasing both run them with, and the rest of what
// runs a shaper.

namespace clip {

// log(cosh(x)) without overflowing for large x.
inline double logCosh(double x) {
    const double a = std::abs(x);
    return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
}

// Where x sits in the Fold curve's period of 4, from 0 at -1.
inline double foldPhase(double x) {
    const double m = std::fmod(x + 1.0, 4.0);
    return m < 0.0 ? m + 4.0 : m;
}

} // namespace clip

// A drawn curve's table: kClipTableSteps + 1 values across -1..1, then their running integral. Doubles: the
// anti-aliasing divides the integral's change by the input's, which for a quiet, slow sound is tiny, so a float's
// rounding where the input crosses a step would come out as a spike.
constexpr int kClipTableSteps = 512;
constexpr double kClipTableStep = 2.0 / kClipTableSteps;
constexpr int kClipTableSize = 2 * (kClipTableSteps + 1);

// Fills `table` (kClipTableSize long) from the drawn curve whose point count is param `base`.
template <class Get>
void buildClipTable(const Get &get, int base, double *table) {
    double *integral = table + kClipTableSteps + 1;
    for (int i = 0; i <= kClipTableSteps; ++i)
        table[i] = clipCurveValue(get, base, -1.0 + i * kClipTableStep);
    integral[0] = 0.0;
    for (int i = 1; i <= kClipTableSteps; ++i)
        integral[i] = integral[i - 1] + 0.5 * kClipTableStep * (table[i - 1] + table[i]);
}

inline double clipTableValue(const double *table, double x) {
    if (!(x == x)) // not a number: it would index anywhere
        x = 0.0;
    const double pos = std::clamp((x + 1.0) / kClipTableStep, 0.0, static_cast<double>(kClipTableSteps));
    const int i = std::min(static_cast<int>(pos), kClipTableSteps - 1);
    return table[i] + (table[i + 1] - table[i]) * (pos - i);
}

// Exact for the straight lines between the table's values; level past the ends.
inline double clipTableIntegral(const double *table, double x) {
    const double *integral = table + kClipTableSteps + 1;
    if (!(x == x)) // not a number: it would index anywhere
        x = 0.0;
    if (x <= -1.0)
        return (x + 1.0) * table[0];
    if (x >= 1.0)
        return integral[kClipTableSteps] + (x - 1.0) * table[kClipTableSteps];
    const int i = std::min(static_cast<int>((x + 1.0) / kClipTableStep), kClipTableSteps - 1);
    const double from = -1.0 + i * kClipTableStep;
    return integral[i] + (x - from) * 0.5 * (table[i] + clipTableValue(table, x));
}

// A shaper's curve, for an input that is already driven (no anti-aliasing; for drawing).
// Custom needs the drawn points (clipCurveValue()); here it is a plain hard clip.
inline double clipShape(ClipMode mode, double x) {
    switch (mode) {
    case ClipMode::Hard:
        return std::clamp(x, -1.0, 1.0);
    case ClipMode::Soft:
        return std::tanh(x);
    case ClipMode::Asymmetric: // soft on top, twice as steep and half as high below: even harmonics
        return x >= 0.0 ? std::tanh(x) : 0.5 * std::tanh(2.0 * x);
    case ClipMode::Cubic:
        return std::abs(x) <= 1.0 ? 1.5 * x - 0.5 * x * x * x : (x > 0.0 ? 1.0 : -1.0);
    case ClipMode::Gentle:
        return x / (1.0 + std::abs(x));
    case ClipMode::SineFold:
        return std::sin(0.5 * std::numbers::pi * x);
    case ClipMode::Fold:
        return 1.0 - std::abs(clip::foldPhase(x) - 2.0);
    case ClipMode::Custom: // needs the drawn points; see clipCurveValue()
    case ClipMode::Count:
        break;
    }
    return std::clamp(x, -1.0, 1.0);
}

// Curves that aren't symmetric shift the signal's average, which a DC blocker after them takes out. A symmetric
// shaper's curve always is.
constexpr bool clipNeedsBlocker(ClipMode mode, bool symmetric = false) {
    return !symmetric && (mode == ClipMode::Asymmetric || mode == ClipMode::Custom);
}

// A shaper curve: one of the formulas, or (Custom) the drawn one through its table. Symmetric, the input below 0
// takes the curve's upper half turned over, -f(-x), so the curve is odd: a drawn curve its right half (one that
// doesn't pass through 0 jumps there). Only a drawn curve is ever run symmetric (see shaperSettings()).
struct ClipCurve {
    ClipMode mode;
    const double *table;
    bool symmetric = false;
    uint32_t version = 0; // the table's (see ClipTable): it is filled again in place when its points change

    // Symmetric, a curve that jumps at 0 is the middle of its jump there, 0, so silence stays silent rather than
    // coming out at the curve's level there (a voice would never end).
    double shape(double x) const {
        if (!symmetric || x > 0.0)
            return plainShape(x);
        return x < 0.0 ? -plainShape(-x) : 0.0;
    }

    // Whether this is the curve `other` is, its table as it was then: one whose integral() is the same everywhere.
    bool same(const ClipCurve &other) const {
        return mode == other.mode && symmetric == other.symmetric &&
               (mode != ClipMode::Custom || (table == other.table && version == other.version));
    }

    // The antiderivative, for first-order antiderivative anti-aliasing. Symmetric, it is even: F(|x|) - F(0).
    double integral(double x) const {
        return symmetric ? plainIntegral(std::abs(x)) - plainIntegral(0.0) : plainIntegral(x);
    }

private:
    double plainShape(double x) const {
        return mode == ClipMode::Custom ? clipTableValue(table, x) : clipShape(mode, x);
    }

    double plainIntegral(double x) const {
        switch (mode) {
        case ClipMode::Hard:
            return std::abs(x) <= 1.0 ? 0.5 * x * x : std::abs(x) - 0.5;
        case ClipMode::Soft:
            return clip::logCosh(x);
        case ClipMode::Asymmetric:
            return x >= 0.0 ? clip::logCosh(x) : 0.25 * clip::logCosh(2.0 * x);
        case ClipMode::Cubic:
            return std::abs(x) <= 1.0 ? 0.75 * x * x - 0.125 * x * x * x * x : std::abs(x) - 0.375;
        case ClipMode::Gentle:
            return std::abs(x) - std::log1p(std::abs(x));
        case ClipMode::SineFold:
            return -2.0 / std::numbers::pi * std::cos(0.5 * std::numbers::pi * x);
        case ClipMode::Fold: { // periodic, as the curve averages to 0 over a period
            const double m = clip::foldPhase(x);
            return m <= 2.0 ? 0.5 * m * m - m : -0.5 * m * m + 3.0 * m - 4.0;
        }
        case ClipMode::Custom:
            return clipTableIntegral(table, x);
        case ClipMode::Count:
            break;
        }
        return 0.0;
    }
};

inline ClipMode clipMode(double value) {
    return static_cast<ClipMode>(
        std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(ClipMode::Count) - 1));
}

// What follows runs a shaper, the effect's and the voices' alike (see shaperParams()).

// What a shaper's params ask of it: its curve, and its drive, output (linked to the drive or not, see
// clipOutputDb()) and mix as gains.
struct ShaperSettings {
    ClipMode mode;
    double drive;
    double output;
    double mix;
    bool symmetric;
};
template <class Get>
ShaperSettings shaperSettings(const Get &get, const ShaperParams &p) {
    const double driveDb = get(p.drive);
    const ClipMode mode = clipMode(get(p.mode));
    return {mode, std::pow(10.0, driveDb / 20.0),
            std::pow(10.0, clipOutputDb(get(p.output), driveDb, get(p.link) >= 0.5) / 20.0), get(p.mix),
            mode == ClipMode::Custom && get(p.symmetric) >= 0.5}; // the switch shows for a drawn curve only
}

// A shaper's curve for drawing (no drive, no anti-aliasing), a drawn one read straight from its points (`curveBase`,
// its point count) rather than through a table; symmetric as ClipCurve.
template <class Get>
double shaperCurveValue(const Get &get, ClipMode mode, int curveBase, bool symmetric, double x) {
    auto plain = [&](double in) {
        return mode == ClipMode::Custom ? clipCurveValue(get, curveBase, in) : clipShape(mode, in);
    };
    return symmetric && x < 0.0 ? -plain(-x) : plain(x);
}

// A drawn curve's table (see buildClipTable()), built again whenever the points it was built from change. Its
// values are allocated when it is made, so the audio thread only fills them.
struct ClipTable {
    std::vector<double> values = std::vector<double>(kClipTableSize, 0.0);
    std::array<double, kShapeParamCount> points; // NaN until the first build
    uint32_t version = 0;                        // how many times it was built

    ClipTable() { points.fill(std::numeric_limits<double>::quiet_NaN()); }

    // `base`: the drawn curve's point count.
    template <class Get>
    void update(const Get &get, int base) {
        bool same = true;
        for (int i = 0; i < kShapeParamCount && same; ++i)
            same = points[i] == get(base + i);
        if (same)
            return;
        for (int i = 0; i < kShapeParamCount; ++i)
            points[i] = get(base + i);
        buildClipTable(get, base, values.data());
        ++version;
    }
};

// The DC blocker after the uneven curves (see clipNeedsBlocker()): a 5 Hz high-pass, by its pole at `rate`.
inline double clipBlockerPole(double rate) {
    return 1.0 - 2.0 * std::numbers::pi * 5.0 / rate;
}

// One side of a shaper as it runs: its last driven input, for the anti-aliasing, and its DC blocker, which keeps
// running whether its curve needs it or not, so it is settled when one that does is picked.
struct ClipState {
    double last = 0.0;
    double dcIn = 0.0;
    double dcOut = 0.0;
    // The curve's integral at `last`, worked out in the step before, which this one needs again: kept with the curve
    // it was worked out on, and taken while that stays the same.
    double lastIntegral = 0.0;
    ClipCurve integralOf{ClipMode::Count, nullptr};
    bool integralKnown = false;

    // `x` driven through the curve (blocked if `block`), then at `output`, and `mix` of that against x. The curve is
    // averaged over the last sample step (first-order antiderivative anti-aliasing), which keeps the harmonics that
    // hard and steep curves add from aliasing as badly.
    double run(const ClipCurve &curve, double x, double drive, double output, double mix, bool block, double pole) {
        const double driven = x * drive;
        const double dx = driven - last;
        double y;
        if (std::abs(dx) < 1e-6) {
            y = curve.shape(0.5 * (driven + last));
            integralKnown = false;
        } else {
            const double from = integralKnown && curve.same(integralOf) ? lastIntegral : curve.integral(last);
            lastIntegral = curve.integral(driven);
            integralOf = curve;
            integralKnown = true;
            y = (lastIntegral - from) / dx;
        }
        last = driven;
        const double blocked = y - dcIn + pole * dcOut;
        dcIn = y;
        dcOut = blocked;
        if (block)
            y = blocked;
        return x + (y * output - x) * mix;
    }
};

} // namespace fmvibe
