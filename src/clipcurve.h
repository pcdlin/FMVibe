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

// The Stack curve: a soft one (tanh) biased off its middle, like a tube stage, scaled to pass what is quiet at its
// level and moved to pass silence as silence. It saturates gradually, its lower half reaching further than its upper
// half (to -1.25 and 0.83): even harmonics, and an offset that grows as it is driven harder, which the stage after it
// (see StackStage) pumps against.
constexpr double kStackBias = 0.2;
inline const double kStackTanh = std::tanh(kStackBias);
inline const double kStackScale = 1.0 / (1.0 - kStackTanh * kStackTanh); // its slope at 0, 1

inline double stackCurve(double x) {
    return (std::tanh(x + kStackBias) - kStackTanh) * kStackScale;
}

// Its antiderivative, 0 at 0.
inline double stackIntegral(double x) {
    static const double atZero = logCosh(kStackBias);
    return (logCosh(x + kStackBias) - atZero - kStackTanh * x) * kStackScale;
}

// A Stack stage's high-pass before its gain, like the coupling capacitor between two gain stages. The offset the
// curve puts out as it is driven harder, the next stage's high-pass takes out over some milliseconds, which that stage
// drives: a stack's low end pumps with the sound, and what the high-passes still hold as a note ends, the stages after
// them drive on, a low thump past the note.
constexpr double kStackHighPassHz = 20.0;
// The anti-aliasing averages each stage's curve over the last sample step, which for what the curve leaves straight is
// the mean of two samples: a top falling away toward the rate's half, which a stack multiplies. Each stage lifts its
// top back by this much of the step it took, y + e (y - y'): at 96 kHz (where the shaper effect runs them from a
// 48 kHz host, see Effects::runShaper(), and the voices at their default oversampling) eight stages stay within half
// a dB up to 15 kHz, 3 dB down at 20 kHz; one stage within 0.4 dB to 20 kHz.
constexpr double kStackLift = 0.25;
// What a stage's high-pass puts out comes this much closer to 0 before the stage drives it: far below hearing, but a
// stack of eight at full drive would otherwise bring up what is all but silence (a reverb's tail far down, rounding)
// by nearly 300 dB, and its ringing would never end.
constexpr double kStackFloor = 1e-9;

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
    case ClipMode::Stack: // one stage of it, without its filters and gains (see StackStage)
        return clip::stackCurve(x);
    case ClipMode::Custom: // needs the drawn points; see clipCurveValue()
    case ClipMode::Count:
        break;
    }
    return std::clamp(x, -1.0, 1.0);
}

// Curves that aren't symmetric shift the signal's average, which a DC blocker after them takes out. A drawn curve
// gets one even when it is made symmetric: one that jumps at the middle shifts the average of a sound that spends
// longer above 0 than below. Stack has one after its last stage (the high-pass each stage starts with takes out
// what the one before it put out).
constexpr bool clipNeedsBlocker(ClipMode mode) {
    return mode == ClipMode::Asymmetric || mode == ClipMode::Custom || mode == ClipMode::Stack;
}

// A shaper curve: one of the formulas, or (Custom) the drawn one through its table. Symmetric, the input below 0
// takes the curve's upper half turned over, -f(-x), so the curve is odd: a drawn curve its right half (one that
// doesn't pass through 0 jumps there). Only a drawn curve is ever run symmetric (see shaperSettings()).
struct ClipCurve {
    ClipMode mode;
    const double *table;
    bool symmetric = false;
    uint32_t version = 0; // the table's (see ClipTable): it is filled again in place when its points change
    // While a drawn curve glides to new points (see ClipTable), the table it glides from this block, and how far
    // along toward `table` this sample is: the curve is the two blended, as is its integral.
    const double *from = nullptr;
    double fade = 1.0;
    // A drawn curve's level at the middle, in `from` and in `table`, which run() takes out of what comes out (made
    // symmetric, the jump it makes there, as far as it shifts the average): a point moved shifts it as slowly as a
    // hand drags, which the DC blocker lets through.
    double restFrom = 0.0;
    double restTo = 0.0;
    // The Stack curve's stages, run one after another (see StackStage), and their high-passes' pole at the
    // rate they run at; 0 stages for every other curve, which runs once, as it is.
    int stages = 0;
    double highPass = 0.0;

    // A Stack curve run `count` times in a row at `rate` (see ShaperSettings::stack); other curves are left as
    // they are.
    void stack(int count, double rate) {
        if (mode != ClipMode::Stack)
            return;
        stages = std::clamp(count, 1, kMaxShaperStack);
        highPass = std::exp(-2.0 * std::numbers::pi * clip::kStackHighPassHz / rate);
    }

    double rest() const { return from ? restFrom + (restTo - restFrom) * fade : restTo; }

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
               (mode != ClipMode::Custom || (table == other.table && version == other.version &&
                                             from == other.from && fade == other.fade));
    }

    // The antiderivative, for first-order antiderivative anti-aliasing. Symmetric, it is even: F(|x|) - F(0).
    double integral(double x) const {
        return symmetric ? plainIntegral(std::abs(x)) - plainIntegral(0.0) : plainIntegral(x);
    }

private:
    double plainShape(double x) const {
        if (mode != ClipMode::Custom)
            return clipShape(mode, x);
        const double to = clipTableValue(table, x);
        return from ? clipTableValue(from, x) + (to - clipTableValue(from, x)) * fade : to;
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
        case ClipMode::Stack:
            return clip::stackIntegral(x);
        case ClipMode::Custom: {
            const double to = clipTableIntegral(table, x);
            return from ? clipTableIntegral(from, x) + (to - clipTableIntegral(from, x)) * fade : to;
        }
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
// clipOutputDb()) and mix as gains; and how many times the Stack curve runs in a row (1 for every other curve),
// each stage but the last followed by the level that keeps it about as loud as it went in, as a linked shaper is
// (`between`, see shaperBetween()).
struct ShaperSettings {
    ClipMode mode;
    double drive;
    double output;
    double mix;
    bool symmetric;
    int stack = 1;
    double between = 1.0;
};

// The gain after each Stack stage but the last: the linked output for `driveDb`, so a stack plays as as many
// linked shapers in a row, the last with the shaper's own Output.
inline double shaperBetween(double driveDb) {
    return std::pow(10.0, clipOutputDb(0.0, driveDb, true) / 20.0);
}
// A Stack param's value as a count of stages.
inline int shaperStack(double value) {
    return value == value ? std::clamp(static_cast<int>(std::lround(value)), 1, kMaxShaperStack) : 1;
}

template <class Get>
ShaperSettings shaperSettings(const Get &get, const ShaperParams &p) {
    const double driveDb = get(p.drive);
    const ClipMode mode = clipMode(get(p.mode));
    return {mode, std::pow(10.0, driveDb / 20.0),
            std::pow(10.0, clipOutputDb(get(p.output), driveDb, get(p.link) >= 0.5) / 20.0), get(p.mix),
            mode == ClipMode::Custom && get(p.symmetric) >= 0.5, // the switch shows for a drawn curve only
            mode == ClipMode::Stack ? shaperStack(get(p.stack)) : 1, // and the Stack knob for Stack
            shaperBetween(driveDb)};
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

// How long a drawn curve takes to glide to new points. Swapped in at once, a point dragged would step what comes out
// every block, which is heard as crackle: most of all the middle point, which sets the level of all that is quiet.
constexpr double kClipGlideSeconds = 0.02;

// A drawn curve's table (see buildClipTable()), built again whenever the points it was built from change, and glided
// to from the curve as it was then, block by block (see advance()). Its tables are allocated when it is made, so the
// audio thread only fills them.
struct ClipTable {
    std::vector<double> values = std::vector<double>(kClipTableSize, 0.0);   // the curve at this block's end
    std::vector<double> previous = std::vector<double>(kClipTableSize, 0.0); // and at its start, while gliding
    std::vector<double> target = std::vector<double>(kClipTableSize, 0.0);   // built from the points
    std::vector<double> start = std::vector<double>(kClipTableSize, 0.0);    // where the glide began
    std::array<double, kShapeParamCount> points; // NaN until the first build
    uint32_t version = 0;                        // how many times `values` changed
    double progress = 1.0;                       // along the glide from `start` to `target`
    bool gliding = false;                        // whether this block glides from `previous` to `values`
    bool built = false;

    ClipTable() { points.fill(std::numeric_limits<double>::quiet_NaN()); }

    // `base`: the drawn curve's point count. The first curve is taken at once; a later one glides in from the curve
    // as it is (see advance()).
    template <class Get>
    void update(const Get &get, int base) {
        bool same = true;
        for (int i = 0; i < kShapeParamCount && same; ++i)
            same = points[i] == get(base + i);
        if (same)
            return;
        for (int i = 0; i < kShapeParamCount; ++i)
            points[i] = get(base + i);
        buildClipTable(get, base, target.data());
        if (!built) {
            built = true;
            values = target;
            gliding = false;
            progress = 1.0;
            ++version;
            return;
        }
        start = values;
        progress = 0.0;
    }

    // Moves the glide on by a block `seconds` long, before the block runs: through it the curve goes from
    // `previous` to `values` (see curve()).
    void advance(double seconds) {
        if (progress >= 1.0) {
            if (gliding) { // the last block's glide ended at `values`, which this one keeps
                gliding = false;
                ++version;
            }
            return;
        }
        previous.swap(values);
        progress = std::min(1.0, progress + seconds / kClipGlideSeconds);
        if (progress >= 1.0)
            values = target;
        else
            for (int i = 0; i < kClipTableSize; ++i)
                values[i] = start[i] + (target[i] - start[i]) * progress;
        gliding = true;
        ++version;
    }

    // The curve to run this block; while it glides, its `fade` goes from 0 at the block's start to 1 at its end.
    ClipCurve curve(ClipMode mode, bool symmetric) const {
        const bool resting = mode == ClipMode::Custom;
        return {mode,
                values.data(),
                symmetric,
                version,
                gliding ? previous.data() : nullptr,
                1.0,
                resting && gliding ? clipTableValue(previous.data(), 0.0) : 0.0,
                resting ? clipTableValue(values.data(), 0.0) : 0.0};
    }
};

// The DC blocker after the uneven curves (see clipNeedsBlocker()): a 5 Hz high-pass, by its pole at `rate`. What a
// drawn curve puts out at the middle is taken out before it (see ClipCurve::rest()).
inline double clipBlockerPole(double rate) {
    return 1.0 - 2.0 * std::numbers::pi * 5.0 / rate;
}

// One stage of the Stack curve as it runs: its high-pass, then its drive into the curve (anti-aliased as
// ClipState::run() runs the others, its top lifted back, see kStackLift). A stack runs them one after another, each
// driven alike, as if
// as many linked shapers stood in a row (see ShaperSettings::between), but for their DC blockers: the high-pass each
// stage starts with does that job, and a slower one between them would draw a stack's ringing out for seconds. The DC
// blocker, the shaper's own output level and its mix come once, after the last.
struct StackStage {
    double highIn = 0.0;
    double highOut = 0.0;
    double last = 0.0;         // its last driven input
    double lastIntegral = 0.0; // and the curve's integral there
    double lastOut = 0.0;      // the curve's, before the lift

    double run(double x, double drive, const ClipCurve &curve) {
        const double high = curve.highPass * (highOut + x - highIn);
        highIn = x;
        highOut = high;
        const double floored = high > clip::kStackFloor    ? high - clip::kStackFloor
                               : high < -clip::kStackFloor ? high + clip::kStackFloor
                                                               : 0.0;
        const double driven = floored * drive;
        const double dx = driven - last;
        const double integral = clip::stackIntegral(driven);
        const double y = dx > -1e-6 && dx < 1e-6 ? clip::stackCurve(0.5 * (driven + last))
                                                 : (integral - lastIntegral) / dx;
        last = driven;
        lastIntegral = integral;
        const double lifted = y + clip::kStackLift * (y - lastOut);
        lastOut = y;
        return lifted;
    }
};

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
    // A DC blocker on the sign of what goes into a symmetric curve, whose jump at the middle is that sign times the
    // curve's level there (see ClipCurve::rest()).
    double signIn = 0.0;
    double signOut = 0.0;
    // The Stack curve's stages (see ClipCurve::stages), and how many of them ran in the step before: one the
    // stack takes on starts from silence.
    std::array<StackStage, kMaxShaperStack> stages{};
    int stagesRan = 0;

    // `x` driven through the curve (blocked if `block`), then at `output`, and `mix` of that against x. The curve is
    // averaged over the last sample step (first-order antiderivative anti-aliasing), which keeps the harmonics that
    // hard and steep curves add from aliasing as badly. Stack runs its stages, `between` after each but the last.
    double run(const ClipCurve &curve, double x, double drive, double output, double mix, bool block, double pole,
               double between = 1.0) {
        const double driven = x * drive;
        if (curve.stages > 0)
            return runStages(curve, x, driven, drive, between, output, mix, pole);
        stagesRan = 0;
        const double dx = driven - last;
        double y;
        double sign; // of the input, averaged over the step as y is: what the jump of a symmetric curve scales
        if (dx > -1e-6 && dx < 1e-6) { // as |dx| < 1e-6, which MSVC can't see guards the division below
            const double middle = 0.5 * (driven + last);
            y = curve.shape(middle);
            sign = middle > 0.0 ? 1.0 : middle < 0.0 ? -1.0 : 0.0;
            integralKnown = false;
        } else {
            sign = (std::abs(driven) - std::abs(last)) / dx;
            const double from = integralKnown && curve.same(integralOf) ? lastIntegral : curve.integral(last);
            lastIntegral = curve.integral(driven);
            integralOf = curve;
            integralKnown = true;
            y = (lastIntegral - from) / dx;
        }
        // A drawn curve's level at the middle comes out as an offset, and made symmetric, as a jump at every crossing of
        // 0, whose average follows how much longer the input stays above 0 than below; either moves with the curve's
        // points. Both are taken out here as they move, the jump's average through a blocker of its own.
        if (curve.symmetric) {
            const double blockedSign = sign - signIn + pole * signOut;
            signIn = sign;
            signOut = blockedSign;
            y -= curve.rest() * (sign - blockedSign);
        } else {
            y -= curve.rest();
        }
        last = driven;
        const double blocked = y - dcIn + pole * dcOut;
        dcIn = y;
        dcOut = blocked;
        if (block)
            y = blocked;
        return x + (y * output - x) * mix;
    }

private:
    // The Stack curve's stages one after another. The anti-aliasing and the DC blocker the other curves run with
    // follow along, so a curve picked after it starts from where the sound is. Kept out of run(), which the voices'
    // loop inlines, so that loop doesn't grow by one it seldom runs.
#ifdef _MSC_VER
    __declspec(noinline)
#else
    __attribute__((noinline))
#endif
    double runStages(const ClipCurve &curve, double x, double driven, double drive, double between, double output,
                     double mix, double pole) {
        for (int k = stagesRan; k < curve.stages; ++k)
            stages[k] = {};
        stagesRan = curve.stages;
        double y = stages[0].run(x, drive, curve);
        for (int k = 1; k < curve.stages; ++k)
            y = stages[k].run(y * between, drive, curve);
        last = driven;
        integralKnown = false;
        const double blocked = y - dcIn + pole * dcOut;
        dcIn = y;
        dcOut = blocked;
        return x + (blocked * output - x) * mix;
    }
};

} // namespace fmvibe
