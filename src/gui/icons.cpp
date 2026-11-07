#include "icons.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace fmvibe::gui::detail {

namespace {

// The icons' points, which depend on nothing but which icon and where: one cache for every editor, which all paint
// on the main thread (as `look` in theme.h assumes).
PointCache icons;

} // namespace

// The power symbol: a ring open at the top, with a stroke down into the gap.
void powerIcon(Canvas &c, Point center, Color color) {
    constexpr float kRadius = 5.0f, kGap = 0.7f; // radians either side of 12 o'clock
    c.arc(center, kRadius, kGap, 2.0f * std::numbers::pi_v<float> - kGap, color, 1.6f);
    c.line({center.x, center.y - kRadius - 1.5f}, {center.x, center.y - 0.5f}, color, 1.6f);
}

// A few piano keys, crossed out for an operator that doesn't follow them (its Fixed switch).
void keyboardIcon(Canvas &c, Point center, Color color, bool crossed) {
    const Rect keys{center.x - 8.0f, center.y - 5.0f, 16.0f, 10.0f};
    c.strokeRect(keys, color, 1.2f, 1.5f);
    for (float x : {-4.0f, 4.0f}) { // a black key over each of these white keys' edges, the edge on below it
        c.fillRect({center.x + x - 1.3f, keys.y, 2.6f, 5.5f}, color);
        c.line({center.x + x, keys.y + 5.5f}, {center.x + x, keys.bottom()}, color, 1.0f);
    }
    c.line({center.x, keys.y}, {center.x, keys.bottom()}, color, 1.0f);
    if (crossed)
        c.line({center.x - 9.0f, center.y + 7.0f}, {center.x + 9.0f, center.y - 7.0f}, color, 1.6f);
}

// A circular arrow, running clockwise from the upper right round to the top.
void resetIcon(Canvas &c, Point center, Color color) {
    constexpr float kFrom = 1.0f, kTo = 6.0f, kRadius = 6.0f;
    c.arc(center, kRadius, kFrom, kTo, color, 1.5f);
    const Point end = Canvas::onCircle(center, kRadius, kTo);
    const Point along{std::cos(kTo), std::sin(kTo)}, out{std::sin(kTo), -std::cos(kTo)};
    const Point head[] = {{end.x + along.x * 3.5f, end.y + along.y * 3.5f},
                          {end.x + out.x * 3.0f, end.y + out.y * 3.0f},
                          {end.x - out.x * 3.0f, end.y - out.y * 3.0f}};
    c.fillPolygon(head, std::size(head), color);
}

// A check mark.
void doneIcon(Canvas &c, Point center, Color color) {
    const Point check[] = {{center.x - 6.0f, center.y + 0.5f}, {center.x - 2.0f, center.y + 4.5f},
                           {center.x + 6.0f, center.y - 4.5f}};
    c.polyline(check, std::size(check), color, 1.8f);
}

void editIcon(Canvas &c, Point center, Color color) {
    // Along the pencil from its tip (lower left) to its end (upper right), and across it.
    const Point along{0.7071f, -0.7071f}, across{0.7071f, 0.7071f};
    auto at = [&](float s, float t) {
        return Point{center.x + along.x * s + across.x * t, center.y + along.y * s + across.y * t};
    };
    constexpr float kTip = -6.0f, kWood = -2.5f, kBand = 3.5f, kEnd = 6.0f, kHalf = 2.0f;
    const Point outline[] = {at(kTip, 0.0f), at(kWood, kHalf),  at(kEnd, kHalf),
                             at(kEnd, -kHalf), at(kWood, -kHalf), at(kTip, 0.0f)};
    c.polyline(outline, std::size(outline), color, 1.2f);
    c.line(at(kWood, kHalf), at(kWood, -kHalf), color, 1.0f);
    c.line(at(kBand, kHalf), at(kBand, -kHalf), color, 1.0f);
    const Point lead[] = {at(kTip, 0.0f), at(kTip + 1.6f, 0.9f), at(kTip + 1.6f, -0.9f)};
    c.fillPolygon(lead, std::size(lead), color);
}

namespace {
// An arrowhead at `tip`, pointing along (dx, dy) (a unit vector), `size` long.
void arrowTip(Canvas &c, Point tip, float dx, float dy, float size, Color color, float width) {
    const Point back{tip.x - dx * size, tip.y - dy * size};
    c.line({back.x - dy * size, back.y + dx * size}, tip, color, width);
    c.line({back.x + dy * size, back.y - dx * size}, tip, color, width);
}
} // namespace

void collapseIcon(Canvas &c, Point m, Color color) {
    c.line({m.x, m.y - 5.5f}, {m.x, m.y + 5.5f}, color, 1.4f);
    for (float side : {-1.0f, 1.0f}) {
        const Point tip{m.x + side * 2.5f, m.y};
        c.line({m.x + side * 8.0f, m.y}, tip, color, 1.4f);
        arrowTip(c, tip, -side, 0.0f, 3.0f, color, 1.4f);
    }
}

void expandIcon(Canvas &c, Point m, Color color) {
    constexpr float k = 0.7071f;
    for (float side : {-1.0f, 1.0f}) {
        const Point tip{m.x + side * 5.5f, m.y - side * 5.5f};
        c.line({m.x + side * 1.0f, m.y - side * 1.0f}, tip, color, 1.4f);
        arrowTip(c, tip, side * k, -side * k, 3.0f, color, 1.4f);
    }
}

void shapeToolIcon(Canvas &c, Point m, int tool, Color color) {
    constexpr float kW = 1.4f;
    switch (tool) {
    case 0: // invert: up and down arrows across a middle line
        c.line({m.x, m.y - 6.5f}, {m.x, m.y + 6.5f}, color, kW);
        arrowTip(c, {m.x, m.y - 6.5f}, 0.0f, -1.0f, 3.0f, color, kW);
        arrowTip(c, {m.x, m.y + 6.5f}, 0.0f, 1.0f, 3.0f, color, kW);
        c.line({m.x - 8.0f, m.y}, {m.x - 3.0f, m.y}, color, kW);
        c.line({m.x + 3.0f, m.y}, {m.x + 8.0f, m.y}, color, kW);
        break;
    case 1: // reverse: left and right arrows across an upright line
        c.line({m.x - 6.5f, m.y}, {m.x + 6.5f, m.y}, color, kW);
        arrowTip(c, {m.x - 6.5f, m.y}, -1.0f, 0.0f, 3.0f, color, kW);
        arrowTip(c, {m.x + 6.5f, m.y}, 1.0f, 0.0f, 3.0f, color, kW);
        c.line({m.x, m.y - 7.0f}, {m.x, m.y - 3.0f}, color, kW);
        c.line({m.x, m.y + 3.0f}, {m.x, m.y + 7.0f}, color, kW);
        break;
    case 2: // shift left: an arrow to a line at the left
    case 3: { // shift right: to one at the right
        const float way = tool == 2 ? -1.0f : 1.0f;
        const Point tip{m.x + way * 4.5f, m.y};
        c.line({m.x - way * 5.5f, m.y}, tip, color, kW);
        arrowTip(c, tip, way, 0.0f, 3.5f, color, kW);
        c.line({m.x + way * 7.0f, m.y - 6.0f}, {m.x + way * 7.0f, m.y + 6.0f}, color, kW);
        break;
    }
    case 4: // copy: two sheets, one over the other
        c.strokeRect({m.x - 6.0f, m.y - 2.5f, 8.5f, 9.0f}, color, 1.2f, 1.5f);
        c.line({m.x - 2.0f, m.y - 4.5f}, {m.x - 2.0f, m.y - 6.5f}, color, 1.2f);
        c.line({m.x - 2.0f, m.y - 6.5f}, {m.x + 6.0f, m.y - 6.5f}, color, 1.2f);
        c.line({m.x + 6.0f, m.y - 6.5f}, {m.x + 6.0f, m.y + 2.5f}, color, 1.2f);
        c.line({m.x + 6.0f, m.y + 2.5f}, {m.x + 4.5f, m.y + 2.5f}, color, 1.2f);
        break;
    default: // paste: a clipboard
        c.strokeRect({m.x - 5.5f, m.y - 5.0f, 11.0f, 12.5f}, color, 1.2f, 1.5f);
        c.fillRect({m.x - 3.0f, m.y - 7.0f, 6.0f, 3.5f}, color, 1.0f);
        c.line({m.x - 3.0f, m.y + 0.5f}, {m.x + 3.0f, m.y + 0.5f}, color, 1.2f);
        c.line({m.x - 3.0f, m.y + 3.5f}, {m.x + 2.0f, m.y + 3.5f}, color, 1.2f);
        break;
    }
}

void pointsIcon(Canvas &c, Point m, Color color) {
    c.line({m.x - 6.0f, m.y + 4.0f}, {m.x - 1.5f, m.y - 1.0f}, color, 1.2f);
    c.line({m.x + 1.5f, m.y - 1.0f}, {m.x + 6.0f, m.y + 3.0f}, color, 1.2f);
    c.strokeCircle({m.x, m.y - 2.0f}, 2.2f, color, 1.3f);
}

void stepsIcon(Canvas &c, Point m, Color color) {
    const Point stair[] = {{m.x - 7.0f, m.y + 4.0f}, {m.x - 3.0f, m.y + 4.0f}, {m.x - 3.0f, m.y - 4.0f},
                           {m.x + 3.0f, m.y - 4.0f}, {m.x + 3.0f, m.y + 1.0f}, {m.x + 7.0f, m.y + 1.0f}};
    c.polyline(stair, std::size(stair), color, 1.3f);
}

void gridIcon(Canvas &c, Point m, Color color) {
    for (int i = -1; i <= 1; ++i) {
        c.line({m.x + i * 4.0f, m.y - 6.0f}, {m.x + i * 4.0f, m.y + 6.0f}, color, 1.0f);
        c.line({m.x - 6.0f, m.y + i * 4.0f}, {m.x + 6.0f, m.y + i * 4.0f}, color, 1.0f);
    }
}

void axisIcon(Canvas &c, Point m, int axis, Color color) {
    const float dx = axis == 0 ? 1.0f : 0.0f, dy = axis == 0 ? 0.0f : 1.0f;
    const Point a{m.x - dx * 5.5f, m.y - dy * 5.5f}, b{m.x + dx * 5.5f, m.y + dy * 5.5f};
    c.line(a, b, color, 1.2f);
    arrowTip(c, a, -dx, -dy, 2.5f, color, 1.2f);
    arrowTip(c, b, dx, dy, 2.5f, color, 1.2f);
}

// A filter type's response in a few strokes, for the type buttons: frequency across, level up.
void filterIcon(Canvas &c, Rect r, FilterType type, Color color) {
    std::vector<double> key = curveKey(Curve::FilterIcon, r);
    key.push_back(static_cast<double>(type));
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        auto peak = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
        for (int i = 0; i <= 24; ++i) {
            const float x = i / 24.0f;
            float y = 0.0f;
            switch (type) {
            case FilterType::LowPass: // flat, a little resonance at the corner, then down
                y = x < 0.55f ? 0.35f + 0.35f * peak(x, 0.55f, 80.0f) : 0.7f - 3.4f * (x - 0.55f);
                break;
            case FilterType::HighPass:
                y = x > 0.45f ? 0.35f + 0.35f * peak(x, 0.45f, 80.0f) : 0.7f - 3.4f * (0.45f - x);
                break;
            case FilterType::BandPass:
                y = -0.9f + 1.7f * peak(x, 0.5f, 30.0f);
                break;
            case FilterType::Notch:
                y = 0.5f - 1.4f * peak(x, 0.5f, 120.0f);
                break;
            case FilterType::Comb: // evenly spaced teeth
                y = -0.8f + 1.5f * std::pow(std::abs(std::cos(3.0f * kPi * x)), 6.0f);
                break;
            case FilterType::Count:
                break;
            }
            points.push_back({r.x + x * r.w, r.center().y - std::clamp(y, -1.0f, 1.0f) * r.h * 0.5f});
        }
        return points;
    });
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// An effect's tempo sync switch: the delay's, the phaser's or the flanger's.
bool isFxSync(FxParam field) {
    return field == FxParam::DelaySync || field == FxParam::PhaserSync || field == FxParam::FlangerSync;
}

// Effect switches drawn as icons, named in their tooltips: a metronome for any effect's SYNC, two chain links for
// the delay's, the shaper's and the three-band dynamics' LINK, a ball bouncing between two paddles for the
// delay's PING-PONG, and for the shaper's SYM an odd curve, its halves turned about the middle.
bool isFxIconSwitch(FxParam field) {
    return isFxSync(field) || field == FxParam::DelayLink || field == FxParam::ShaperLink ||
           field == FxParam::MdLink || field == FxParam::DelayPingPong || field == FxParam::ShaperSymmetric;
}
void fxSwitchIcon(Canvas &c, Point m, FxParam field, Color color) {
    if (isFxSync(field)) {
        const Point body[] = {{m.x - 5.0f, m.y + 5.5f}, {m.x - 2.0f, m.y - 5.5f}, {m.x + 2.0f, m.y - 5.5f},
                              {m.x + 5.0f, m.y + 5.5f}, {m.x - 5.0f, m.y + 5.5f}};
        c.polyline(body, std::size(body), color, 1.3f);
        c.line({m.x, m.y + 3.5f}, {m.x + 5.5f, m.y - 6.0f}, color, 1.3f);
        c.fillCircle({m.x + 2.8f, m.y - 1.4f}, 1.6f, color);
    } else if (field == FxParam::DelayLink || field == FxParam::ShaperLink || field == FxParam::MdLink) {
        c.strokeRect({m.x - 9.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
        c.strokeRect({m.x - 2.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
    } else if (field == FxParam::ShaperSymmetric) {
        c.line({m.x, m.y - 6.0f}, {m.x, m.y + 6.0f}, color.alpha(0.45f), 1.0f);
        std::vector<Point> points;
        for (int i = 0; i <= 24; ++i) { // a curve rising to level off, its left half the right one turned over
            const float t = i / 12.0f - 1.0f;
            points.push_back({m.x + 9.0f * t, m.y - 5.0f * std::tanh(2.5f * t)});
        }
        c.polyline(points.data(), points.size(), color, 1.4f);
        c.fillCircle(m, 1.6f, color);
    } else {
        c.line({m.x - 8.5f, m.y - 5.5f}, {m.x - 8.5f, m.y + 5.5f}, color, 1.6f);
        c.line({m.x + 8.5f, m.y - 5.5f}, {m.x + 8.5f, m.y + 5.5f}, color, 1.6f);
        const Point path[] = {{m.x - 6.0f, m.y - 4.0f}, {m.x + 6.0f, m.y}, {m.x - 6.0f, m.y + 4.0f}};
        c.polyline(path, std::size(path), color.alpha(0.6f), 1.0f);
        c.fillCircle({m.x - 4.5f, m.y + 3.0f}, 1.8f, color);
    }
}

// An LFO's clock modes, named in their tooltips: RETRIGGER a rise that starts again at each dot (a note), FREE an
// infinity sign, ANCHOR an anchor, ONE-SHOT a single cycle that then stays where it ends.
void lfoClockIcon(Canvas &c, Point m, LfoClock clock, Color color) {
    const std::vector<double> key{static_cast<double>(Curve::ModeIcon), m.x, m.y, static_cast<double>(clock)};
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        if (clock == LfoClock::Retrigger) {
            for (int half = 0; half < 2; ++half)
                for (int i = 0; i <= 12; ++i)
                    points.push_back({m.x - 10.0f + half * 10.0f + i * (10.0f / 12.0f),
                                      m.y + 4.0f - 8.0f * std::sin(i / 12.0f * 0.5f * kPi)});
        } else if (clock == LfoClock::Free) {
            for (int i = 0; i <= 48; ++i) {
                const float t = i / 48.0f * 2.0f * kPi, d = 1.0f + std::sin(t) * std::sin(t);
                points.push_back({m.x + 10.0f * std::cos(t) / d, m.y + 11.0f * std::sin(t) * std::cos(t) / d});
            }
        } else if (clock == LfoClock::Anchor) { // its arms, the rest drawn below
            for (int i = 0; i <= 24; ++i) {
                const float t = (0.1f + 0.8f * i / 24.0f) * kPi;
                points.push_back({m.x - 5.0f * std::cos(t), m.y + 0.2f + 5.0f * std::sin(t)});
            }
        } else {
            for (int i = 0; i <= 24; ++i)
                points.push_back({m.x - 10.0f + i * 0.5f, m.y - 4.5f * std::sin(i / 24.0f * 2.0f * kPi)});
            points.push_back({m.x + 10.0f, m.y});
        }
        return points;
    });
    if (clock == LfoClock::Retrigger) { // the notes that start it
        c.fillCircle({m.x - 10.0f, m.y + 4.0f}, 1.6f, color);
        c.fillCircle({m.x, m.y + 4.0f}, 1.6f, color);
    } else if (clock == LfoClock::Anchor) { // its ring, shank and stock, within 6 px of the middle, as the buttons hold
        c.strokeCircle({m.x, m.y - 4.0f}, 1.4f, color, 1.1f);
        c.line({m.x, m.y - 2.6f}, {m.x, m.y + 5.2f}, color, 1.4f);
        c.line({m.x - 2.8f, m.y - 0.8f}, {m.x + 2.8f, m.y - 0.8f}, color, 1.4f);
    } else if (clock == LfoClock::OneShot)
        c.fillCircle(points.back(), 1.7f, color);
    c.polyline(points.data(), points.size(), color, 1.4f);
}

void polarityIcon(Canvas &c, Point m, bool bipolar, Color color) {
    const std::vector<double> key{static_cast<double>(Curve::PolarityIcon), m.x, m.y, bipolar ? 1.0 : 0.0};
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 16; ++i) {
            const float t = i / 16.0f;
            points.push_back({m.x - 5.0f + 10.0f * t, bipolar ? m.y - 3.5f * std::sin(2.0f * kPi * t)
                                                             : m.y + 3.5f - 3.5f * (1.0f - std::cos(2.0f * kPi * t))});
        }
        return points;
    });
    const float base = bipolar ? m.y : m.y + 3.5f;
    c.line({m.x - 5.5f, base}, {m.x + 5.5f, base}, color.alpha(0.5f), 1.0f);
    c.polyline(points.data(), points.size(), color, 1.3f);
}

void routeModeIcon(Canvas &c, Point m, RouteMode mode, Color color, float scale) {
    auto at = [&](float x, float y) { return Point{m.x + x * scale, m.y + y * scale}; };
    const float width = scale < 0.9f ? 1.1f : 1.4f;
    switch (mode) {
    case RouteMode::LinearFm:
        c.line(at(-4.5f, 4.0f), at(4.5f, -4.0f), color, width);
        c.line(at(-5.0f, 4.5f), at(5.0f, 4.5f), color.alpha(0.5f), 1.0f);
        break;
    case RouteMode::ExpFm: {
        const std::vector<double> key{static_cast<double>(Curve::RouteIcon), m.x, m.y, scale};
        const std::vector<Point> points = icons.get(key, [&] {
            std::vector<Point> points;
            for (int i = 0; i <= 12; ++i) {
                const float t = i / 12.0f;
                points.push_back(at(-4.5f + 9.0f * t, 4.0f - 8.0f * (std::exp2(4.0f * t) - 1.0f) / 15.0f));
            }
            return points;
        });
        c.line(at(-5.0f, 4.5f), at(5.0f, 4.5f), color.alpha(0.5f), 1.0f);
        c.polyline(points.data(), points.size(), color, width);
        break;
    }
    case RouteMode::Ring: {
        c.strokeCircle(m, 4.5f * scale, color, width - 0.1f);
        const float d = 4.5f * 0.7071f;
        c.line(at(-d, -d), at(d, d), color, width - 0.2f);
        c.line(at(-d, d), at(d, -d), color, width - 0.2f);
        break;
    }
    default: // Phase
        c.strokeCircle(m, 3.2f * scale, color, width - 0.1f);
        c.line(at(0.0f, -5.5f), at(0.0f, 5.5f), color, width - 0.1f);
        break;
    }
}

// A small picture of each effect, for its card's title and the ADD EFFECT list.
void splitIcon(Canvas &c, Rect r, Color color) {
    const Point from{r.x, r.center().y}, fork{r.x + r.w * 0.4f, r.center().y};
    c.line(from, fork, color, 1.4f);
    for (float y : {-0.45f, 0.0f, 0.45f})
        c.line(fork, {r.right(), r.center().y + y * r.h}, color, 1.4f);
}

void sumIcon(Canvas &c, Rect r, Color color) {
    const float h = std::min(r.h, 14.0f), w = h * 0.75f;
    const float x = r.center().x - 0.5f * w, y = r.center().y - 0.5f * h;
    const Point sigma[] = {{x + w, y}, {x, y}, {x + 0.55f * w, y + 0.5f * h}, {x, y + h}, {x + w, y + h}};
    c.polyline(sigma, 5, color, 1.5f);
}

void fxModeIcon(Canvas &c, Point m, bool graph, Color color) {
    if (!graph) { // three cards in a row
        for (int i = -1; i <= 1; ++i)
            c.strokeRect({m.x + i * 6.0f - 2.5f, m.y - 5.0f, 5.0f, 10.0f}, color, 1.2f, 1.0f);
        return;
    }
    // A node forking into two, which meet again
    const Point a{m.x - 8.0f, m.y}, b{m.x + 8.0f, m.y}, up{m.x, m.y - 5.0f}, down{m.x, m.y + 5.0f};
    c.line(a, up, color, 1.2f);
    c.line(a, down, color, 1.2f);
    c.line(up, b, color, 1.2f);
    c.line(down, b, color, 1.2f);
    for (const Point p : {a, b, up, down})
        c.fillCircle(p, 1.8f, color);
}

void fxIcon(Canvas &c, Rect r, FxType type, Color color) {
    std::vector<Point> points;
    auto at = [&](float x, float y) { points.push_back({r.x + x * r.w, r.center().y - y * r.h * 0.5f}); };
    auto stroke = [&](Color tint) {
        c.polyline(points.data(), points.size(), tint, 1.4f);
        points.clear();
    };
    auto curve = [&](auto &&y) { // y(x) for x across, -1..1 up
        for (int i = 0; i <= 24; ++i)
            at(i / 24.0f, y(i / 24.0f));
        stroke(color);
    };
    auto bar = [&](float x, float height, Color tint) {
        c.line({r.x + x * r.w, r.bottom()}, {r.x + x * r.w, r.bottom() - height * r.h}, tint, 1.6f);
    };
    auto notch = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
    switch (type) {
    case FxType::Shaper: // a sine with its top and bottom cut flat
        curve([](float x) { return std::clamp(1.7f * std::sin(2.0f * kPi * x), -1.0f, 1.0f) * 0.8f; });
        break;
    case FxType::Delay: // the sound, then echoes, each softer
        bar(0.1f, 1.0f, color);
        bar(0.5f, 0.6f, color);
        bar(0.9f, 0.32f, color);
        break;
    case FxType::Eq: // three faders
        for (int k = 0; k < 3; ++k) {
            const float x = r.x + (0.17f + 0.33f * k) * r.w;
            const float y = r.y + (k == 1 ? 0.7f : (k == 0 ? 0.35f : 0.5f)) * r.h;
            c.line({x, r.y}, {x, r.bottom()}, color.alpha(0.45f), 1.0f);
            c.fillRect({x - 2.5f, y - 1.5f, 5.0f, 3.0f}, color, 1.0f);
        }
        break;
    case FxType::ParamEq: // a bell
        curve([&](float x) { return -0.6f + 1.4f * notch(x, 0.5f, 25.0f); });
        break;
    case FxType::Filter: // a low-pass, with a little resonance at its corner
        curve([&](float x) { return x < 0.5f ? 0.3f + 0.4f * notch(x, 0.5f, 60.0f) : 0.7f - 3.2f * (x - 0.5f); });
        break;
    case FxType::Compressor: // output against input, bending over at the threshold
        curve([](float x) { return x < 0.55f ? -1.0f + 2.4f * x : 0.32f + 0.5f * (x - 0.55f); });
        break;
    case FxType::MultiDyn: { // over three bands, levels pressed toward the middle: down from above, up from below
        for (float x : {1.0f / 3.0f, 2.0f / 3.0f})
            c.line({r.x + x * r.w, r.y}, {r.x + x * r.w, r.bottom()}, color.alpha(0.4f), 1.0f);
        const float mid = r.center().y;
        c.line({r.x, mid}, {r.right(), mid}, color, 1.4f);
        for (float side : {-1.0f, 1.0f}) { // the arrow down on the left, up on the right
            const float x = r.x + (side < 0.0f ? 0.25f : 0.75f) * r.w;
            const float from = mid + side * 0.5f * r.h, to = mid + side * 0.14f * r.h;
            c.line({x, from}, {x, to + side * 1.5f}, color, 1.4f);
            const Point head[] = {{x - 2.8f, to + side * 2.8f}, {x + 2.8f, to + side * 2.8f}, {x, to}};
            c.fillPolygon(head, 3, color);
        }
        break;
    }
    case FxType::Reverb: // a dense tail dying away
        for (int k = 0; k < 8; ++k)
            bar(0.06f + 0.125f * k, 1.0f - 0.115f * k, color.alpha(k == 0 ? 1.0f : 0.75f));
        break;
    case FxType::Phaser: // a few wide notches
        curve([&](float x) { return 0.6f - 1.4f * (notch(x, 0.28f, 90.0f) + notch(x, 0.72f, 90.0f)); });
        break;
    case FxType::Flanger: // a comb of narrow notches
        curve([&](float x) {
            float y = 0.6f;
            for (int k = 0; k < 4; ++k)
                y -= 1.5f * notch(x, 0.125f + 0.25f * k, 500.0f);
            return y;
        });
        break;
    case FxType::Disperser: { // a chirp: the sound smeared, its cycles drawn out
        std::vector<Point> chirp;
        for (int i = 0; i <= 40; ++i) {
            const float x = i / 40.0f;
            chirp.push_back({r.x + x * r.w, r.y + r.h * (0.5f - 0.42f * std::sin(2.0f * kPi * (1.0f + 3.0f * x) * x))});
        }
        c.polyline(chirp.data(), chirp.size(), color, 1.4f);
        break;
    }
    case FxType::FreqShifter: { // a partial, moved along
        bar(0.12f, 0.9f, color.alpha(0.4f));
        bar(0.88f, 0.9f, color);
        const float y = r.y + 0.35f * r.h;
        c.line({r.x + 0.28f * r.w, y}, {r.x + 0.7f * r.w, y}, color, 1.4f);
        const Point head[] = {{r.x + 0.76f * r.w, y}, {r.x + 0.64f * r.w, y - 3.0f}, {r.x + 0.64f * r.w, y + 3.0f}};
        c.fillPolygon(head, 3, color);
        break;
    }
    case FxType::Widener: { // the middle, and the sound spreading out of it to both sides
        const float cx = r.center().x;
        c.line({cx, r.y}, {cx, r.bottom()}, color.alpha(0.5f), 1.4f);
        for (float side : {-1.0f, 1.0f}) {
            const Point wing[] = {{cx + side * 0.2f * r.w, r.y + 0.1f * r.h},
                                  {cx + side * 0.48f * r.w, r.center().y},
                                  {cx + side * 0.2f * r.w, r.bottom() - 0.1f * r.h}};
            c.polyline(wing, 3, color, 1.4f);
        }
        break;
    }
    case FxType::MultiComp: // retired, never offered
    case FxType::None:
    case FxType::Count:
        break;
    }
}

// A band shape's response in a few strokes: frequency across, level up. Cuts show a little resonance
// at their corner, which tells them from the shelves.
void peqIcon(Canvas &c, Rect r, PeqType type, Color color) {
    std::vector<double> key = curveKey(Curve::PeqIcon, r);
    key.push_back(static_cast<double>(type));
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        const double q = normFromQ(isCut(type) ? 2.2 : 0.7);
        for (int i = 0; i <= 20; ++i) {
            const double x = i / 20.0;
            const double db = 20.0 * std::log10(peqResponse(cutoffFromNorm(0.15 + 0.7 * x), type, 0.5, 9.0, q));
            points.push_back({r.x + static_cast<float>(x) * r.w,
                              r.center().y - static_cast<float>(std::clamp(db, -15.0, 15.0) / 15.0) * r.h * 0.5f});
        }
        // Centered by the height the curve takes, not by its 0 dB line: a bell rises above it, a cut falls below.
        const auto [low, high] = std::minmax_element(points.begin(), points.end(),
                                                     [](Point a, Point b) { return a.y < b.y; });
        const float shift = r.center().y - 0.5f * (low->y + high->y);
        for (Point &point : points)
            point.y += shift;
        return points;
    });
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// --- Wave icons, one cycle across r.

std::vector<Point> lfoPoints(LfoShape shape, Rect r) {
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (shape) {
    case LfoShape::Sine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 48.0f));
        break;
    case LfoShape::Triangle:
        at(0.0f, 0.0f);
        at(0.25f, 1.0f);
        at(0.75f, -1.0f);
        at(1.0f, 0.0f);
        break;
    case LfoShape::Saw:
        at(0.0f, -1.0f);
        at(1.0f, 1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Square:
        at(0.0f, 1.0f);
        at(0.5f, 1.0f);
        at(0.5f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Random: {
        const float steps[] = {0.35f, -0.7f, 0.85f, -0.25f};
        for (int i = 0; i < 4; ++i) {
            at(i / 4.0f, steps[i]);
            at((i + 1) / 4.0f, steps[i]);
        }
        break;
    }
    case LfoShape::Custom: // straight lines between points you can drag; see customShapeDots()
        at(0.0f, -0.7f);
        at(0.35f, 0.8f);
        at(0.7f, -0.1f);
        at(1.0f, 0.6f);
        break;
    case LfoShape::Count:
        break;
    }
    return points;
}

// The Custom icon's line has a dot on each point, like the shape editor's handles.
void customShapeDots(Canvas &c, Rect r, Color color) {
    for (const Point &point : lfoPoints(LfoShape::Custom, r)) {
        c.fillCircle(point, 2.6f, kRaised);
        c.strokeCircle(point, 2.0f, color, 1.3f);
    }
}

std::vector<Point> wavePoints(OpWave wave, Rect r) {
    switch (wave) {
    case OpWave::Sine:
        return lfoPoints(LfoShape::Sine, r);
    case OpWave::Triangle:
        return lfoPoints(LfoShape::Triangle, r);
    case OpWave::Saw:
        return lfoPoints(LfoShape::Saw, r);
    case OpWave::Square:
        return lfoPoints(LfoShape::Square, r);
    default:
        break;
    }
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (wave) {
    case OpWave::Pulse:
        at(0.0f, 1.0f);
        at(0.25f, 1.0f);
        at(0.25f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::HalfSine:
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, 2.0f * std::sin(kPi * i / 24.0f) - 1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::AbsSine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, 2.0f * std::abs(std::sin(2.0f * kPi * i / 48.0f)) - 1.0f);
        break;
    case OpWave::AltSine: // a whole sine in the first half, then flat
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 24.0f));
        at(1.0f, 0.0f);
        break;
    case OpWave::Organ: // its drawbars added up
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.44f * (std::sin(t) + std::sin(2.0f * t) +
                                   0.5f * (std::sin(3.0f * t) + std::sin(4.0f * t)) +
                                   0.25f * (std::sin(6.0f * t) + std::sin(8.0f * t))));
        }
        break;
    case OpWave::Vowel: // a ripple at the formant, dying away through each cycle
        for (int i = 0; i <= 48; ++i) {
            const float t = i / 48.0f;
            at(t, std::exp(-3.5f * t) * std::sin(2.0f * kPi * 4.0f * t + 0.6f));
        }
        break;
    case OpWave::Noise: {
        const float values[] = {0.1f, 0.8f, -0.5f, 0.3f, -0.9f, 0.6f, -0.2f, 0.9f, -0.7f, 0.2f, -0.4f, 0.7f, -0.8f};
        for (int i = 0; i < 13; ++i)
            at(i / 12.0f, values[i]);
        break;
    }
    case OpWave::Custom: // a few partials added up, standing for whatever was drawn
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.65f * std::sin(t) + 0.35f * std::sin(3.0f * t + 0.6f) + 0.2f * std::sin(5.0f * t));
        }
        break;
    default:
        break;
    }
    return points;
}

// An LFO shape's or a wave's icon, as lfoPoints() and wavePoints() draw it, kept with the other icons.
std::vector<Point> shapeIcon(LfoShape shape, Rect r) {
    std::vector<double> key = curveKey(Curve::LfoIcon, r);
    key.push_back(static_cast<double>(shape));
    return icons.get(key, [&] { return lfoPoints(shape, r); });
}
std::vector<Point> waveIcon(OpWave wave, Rect r) {
    std::vector<double> key = curveKey(Curve::WaveIcon, r);
    key.push_back(static_cast<double>(wave));
    return icons.get(key, [&] { return wavePoints(wave, r); });
}

} // namespace fmvibe::gui::detail
