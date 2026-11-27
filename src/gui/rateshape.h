#pragma once

// An operator's Rate/Level envelope as its ENVELOPE tab draws it (see OpEnvMode::Rates): traced a DX7 envelope tick at
// a time by the engine's own RateEnvelope, from a note at C4 up to L3 and then let go, and where its four breakpoints,
// the handles, fall on the display. Shared by the inspector and the UI checks.

#include "../rateenvelope.h"
#include "canvas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fmvibe::gui {

// Its levels go up the display on the DX7's own scale, its units (see RateEnvelope), from none at the bottom to full
// at the top: evenly in dB, as its levels step. rateUnits() is levelUnits() between its steps, rateLevelOf() back.
inline double rateUnits(double level) {
    return std::max(RateEnvelope::kFloorUnits, RateEnvelope::scaledLevel(level) * 32.0 - 192.0);
}
inline double rateLevelOf(double units) {
    const double s = (units + 192.0) / 32.0;
    return std::clamp(s >= 48.0 ? s - 28.0 : std::min(19.0 * std::pow(std::max(s, 0.0) / 46.0, 1.0 / 0.7), 20.0), 0.0,
                      99.0);
}
inline double rateUnitsOfGain(double gain) {
    return gain > 0.0 ? std::max(RateEnvelope::kFullUnits + 256.0 * std::log2(gain), 0.0) : 0.0;
}
inline float rateY(Rect plot, double units) {
    return plot.bottom() - static_cast<float>(std::clamp(units / RateEnvelope::kFullUnits, 0.0, 1.0)) * plot.h;
}

struct RateShape {
    std::vector<Point> rise;         // through L1 and L2 to L3
    std::vector<Point> release;      // from L3, where it held, to L4
    std::array<Point, 4> points{};   // L1-L4: the handles
    std::array<double, 4> seconds{}; // how long each segment takes
    // Along each segment's curve, how far along it RateEnvelope::progress() counts each point (by its level, or by its
    // time where it waits), so the dot for a voice lands on the curve.
    std::array<std::vector<std::pair<double, Point>>, 4> along;

    // Where the dot for RateEnvelope::progress() `p` goes, or false for none.
    bool dotAt(double p, Point &dot) const {
        if (p < 0.0 || rise.empty() || release.empty())
            return false;
        if (p >= 3.0 && p < 4.0) { // held at L3
            dot = rise.back();
            return true;
        }
        const int k = p >= 4.0 ? 3 : std::clamp(static_cast<int>(p), 0, 2);
        const double f = std::clamp(p - (p >= 4.0 ? 4.0 : static_cast<double>(k)), 0.0, 1.0);
        const auto &marks = along[k];
        if (marks.empty())
            return false;
        dot = marks.back().second;
        for (size_t i = 0; i < marks.size(); ++i)
            if (marks[i].first >= f) {
                if (i == 0) {
                    dot = marks[0].second;
                } else {
                    const auto &[p0, a] = marks[i - 1];
                    const auto &[p1, b] = marks[i];
                    const float t = p1 > p0 ? static_cast<float>((f - p0) / (p1 - p0)) : 1.0f;
                    dot = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                }
                break;
            }
        return true;
    }
};

// In `plot`: its rise over the left 70 %, each segment as wide as the square root of its time (but at least 14 px, so
// a segment at once keeps its points apart), the hold a gap of 8 %, and its release over the rest.
inline RateShape rateShapeOf(const RateEnvelope::Settings &s, Rect plot) {
    const double tickRate = 1.0 / RateEnvelope::kTickSeconds;
    constexpr int kMostTicks = static_cast<int>(360.0 / RateEnvelope::kTickSeconds); // six minutes a part
    RateEnvelope env;
    env.start();
    env.prepare(s, tickRate, 0.0);
    std::vector<double> rise{0.0}; // its level a tick, in units
    std::array<size_t, 3> ends{};
    int segment = 0;
    for (int t = 0; t < kMostTicks && !env.holding() && !env.idle(); ++t) {
        rise.push_back(rateUnitsOfGain(env.next()));
        while (segment < std::min(env.segment(), 3))
            ends[segment++] = rise.size() - 1;
    }
    while (segment < 3)
        ends[segment++] = rise.size() - 1;
    env.release();
    env.prepare(s, tickRate, env.level());
    std::vector<double> fall{rise.back()};
    for (int t = 0; t < kMostTicks && env.segment() <= 3 && !env.idle(); ++t) {
        const double gain = env.next();
        if (env.segment() > 3) // at L4: a fade there (L4 heard) isn't drawn
            break;
        fall.push_back(rateUnitsOfGain(gain));
    }

    RateShape shape;
    auto along = [](const std::vector<double> &levels, double at) {
        const size_t k = std::min(static_cast<size_t>(at), levels.size() - 1);
        const double next = levels[std::min(k + 1, levels.size() - 1)];
        return levels[k] + (next - levels[k]) * (at - static_cast<double>(k));
    };
    // How far along a segment from `start` to `end` units a level is, as RateEnvelope::progress() counts it; -1 where
    // the segment waits, which counts its time.
    auto counted = [](double start, double end, bool waits) {
        return [=](double units, double share) {
            if (waits)
                return share;
            const double from = end > start ? std::max(start, 1716.0) : start;
            return std::abs(end - from) < 1e-9 ? 1.0 : std::clamp((units - from) / (end - from), 0.0, 1.0);
        };
    };
    const float riseW = plot.w * 0.7f, gap = plot.w * 0.08f, fallW = plot.w - riseW - gap;
    constexpr float kLeast = 14.0f;
    double weights[3], sum = 0.0;
    for (int k = 0; k < 3; ++k) {
        const size_t from = k > 0 ? ends[k - 1] : 0;
        weights[k] = std::sqrt(static_cast<double>(ends[k] - from));
        sum += weights[k];
        shape.seconds[k] = static_cast<double>(ends[k] - from) * RateEnvelope::kTickSeconds;
    }
    float x = plot.x;
    shape.rise.push_back({x, rateY(plot, rise[0])});
    for (int k = 0; k < 3; ++k) {
        const float span = kLeast + (riseW - 3.0f * kLeast) * static_cast<float>(sum > 0.0 ? weights[k] / sum : 1.0 / 3.0);
        const size_t from = k > 0 ? ends[k - 1] : 0;
        const double ticks = static_cast<double>(ends[k] - from);
        const int steps = std::max(1, static_cast<int>(std::min(ticks, 48.0)));
        const double start = rise[from], end = rise[ends[k]];
        const auto progress = counted(start, end, std::abs(end - start) < 0.5 || (k == 0 && s.levels[0] < 0.5));
        shape.along[k].push_back({0.0, shape.rise.back()});
        for (int i = 1; i <= steps; ++i) {
            const double share = static_cast<double>(i) / steps;
            const double units = along(rise, static_cast<double>(from) + ticks * share);
            shape.rise.push_back({x + span * static_cast<float>(share), rateY(plot, units)});
            shape.along[k].push_back({progress(units, share), shape.rise.back()});
        }
        x += span;
        shape.points[k] = {x, rateY(plot, rateUnits(s.levels[k]))};
    }
    const float x0 = plot.x + riseW + gap;
    const double ticks = static_cast<double>(fall.size() - 1);
    const int steps = std::max(1, static_cast<int>(std::min(ticks, 64.0)));
    const auto progress = counted(fall.front(), fall.back(), std::abs(fall.back() - fall.front()) < 0.5);
    for (int i = 0; i <= steps; ++i) { // along the square root of its time, as a long tail would crowd the rest out
        const double share = static_cast<double>(i) / steps;
        const double units = along(fall, share * share * ticks);
        shape.release.push_back({x0 + fallW * static_cast<float>(share), rateY(plot, units)});
        shape.along[3].push_back({progress(units, share * share), shape.release.back()});
    }
    shape.points[3] = {plot.right(), rateY(plot, rateUnits(s.levels[3]))};
    shape.seconds[3] = ticks * RateEnvelope::kTickSeconds;
    return shape;
}

} // namespace fmvibe::gui
