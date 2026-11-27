// The routing panel (RoutingPanel, see routingpanel.h): the routing graph and the matrix as the panels' row shows them,
// with ALGORITHM and its panel: their widgets, painting (the graph's glow, which the effects graph's backdrop shares,
// its nodes, wires and labels, the matrix's cells, names and legend), the drags that move the nodes and the routes and
// bend the wires, a label's list of modes, the routing's context menus and the panel of algorithms. What edits the
// routes is RoutingModel's (routingmodel.cpp); the geometry they are drawn with is routing.h's.

#include "routingpanel.h"
#include "lists.h"

#include "algorithms.h"
#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "routing.h"
#include "theme.h"
#include "../uistrings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

namespace {

// A route's depth as its label shows it.
std::string percent(double value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
    return text;
}

// "LP", or "LP 24" at a slope other than 12 dB per octave, as it plays.
std::string filterLabel(FilterType type, double slope) {
    const int played = playedSlope(type, filterSlope(slope));
    return std::string(filterTypeShortName(type)) +
           (hasSlope(type) && played != kDefaultSlope ? " " + std::to_string(slopeDb(played)) : "");
}

// --- The classic algorithms (algorithms.h): ALGORITHM and its panel's thumbnails.

// An algorithm drawn small in `r`, laid out as the routing graph lays it out (algorithmPlaces()): the carriers in a
// column at the right, their lines running into OUT, a dot at the right end, what modulates them to their left, each
// operator a ring of `radius` in its color filled with `ground`, its wires in its color; its feedback a small loop on
// its top or, back up a stack, a dashed curve under the stack.
void paintAlgorithm(Canvas &c, Rect r, const Algorithm &a, float radius, Color ground) {
    const AlgorithmPlaces places = algorithmPlaces(a);
    constexpr float kCarrierCol = 6.0f;
    float lowest = kCarrierCol;
    for (int op = 0; op < a.ops; ++op)
        lowest = std::min(lowest, places.col[op]);
    const Point out{r.right() - 4.0f, r.center().y};
    const float carrierX = r.right() - 18.0f;
    const float step =
        lowest < kCarrierCol ? std::min(8.5f, (carrierX - r.x - radius - 1.0f) / (kCarrierCol - lowest)) : 0.0f;
    auto at = [&](int op) {
        return Point{carrierX - (kCarrierCol - places.col[op]) * step,
                     r.y + radius + 3.0f + places.y[op] * (r.h - 2.0f * radius - 6.0f)};
    };
    for (int op = 0; op < a.ops; ++op)
        if (a.carriers >> op & 1)
            c.line(at(op), out, nodeColor(op).alpha(0.45f), 1.0f);
    for (int to = 0; to < a.ops; ++to)
        for (int from = 0; from < a.ops; ++from)
            if (a.mods[to] >> from & 1)
                c.line(at(from), at(to), nodeColor(from), 1.3f);
    if (a.feedbackFrom != a.feedbackTo) {
        const Point p = at(a.feedbackFrom), q = at(a.feedbackTo);
        c.curve(p, {0.5f * (p.x + q.x), std::max(p.y, q.y) + 3.0f * radius}, q, nodeColor(a.feedbackFrom), 1.0f, true);
    }
    c.fillCircle(out, 3.0f, kAccent);
    for (int op = 0; op < a.ops; ++op) {
        c.fillCircle(at(op), radius, ground);
        c.strokeCircle(at(op), radius, nodeColor(op), 1.4f);
    }
    if (a.feedbackFrom == a.feedbackTo) {
        const Point p = at(a.feedbackFrom);
        c.strokeCircle({p.x, p.y - radius - 2.4f}, 2.4f, nodeColor(a.feedbackFrom), 1.0f);
    }
}

// What the panel's foot says of an algorithm: its number, how many carriers it has and where its feedback is.
std::string algorithmText(const Algorithm &a) {
    const int carriers = std::popcount(a.carriers);
    const char *const from = kNodeNames[a.feedbackFrom], *const to = kNodeNames[a.feedbackTo];
    return trf(Str::AlgorithmName, {std::to_string(a.number)}) + " \xC2\xB7 " +
           (carriers == 1 ? tr(Str::AlgorithmCarrierOne) : trf(Str::AlgorithmCarriers, {std::to_string(carriers)})) +
           " \xC2\xB7 " +
           (a.feedbackFrom == a.feedbackTo ? trf(Str::AlgorithmFeedback, {from})
                                           : trf(Str::AlgorithmFeedbackLoop, {from, to}));
}

// ALGORITHM, drawn as + SOURCE is (paintMenuButton()): a small algorithm's glyph in place of the plus (two operators
// feeding one that goes out), the name, the algorithm the operators form on a tag in the accent (`badge`, empty for
// none), then a chevron; lit under the mouse and while its panel is open.
void paintAlgorithmButton(Canvas &c, Rect r, const std::string &badge, bool hovered, bool open) {
    const bool lit = hovered || open;
    const bool pill = look.flat;
    const Color ink = lit || pill ? kText : kDim;
    if (pill) {
        paintPill(c, r, lit);
    } else {
        c.fillRect(r, lit ? kAccent.alpha(0.18f) : kRaised, kRadiusControl);
        c.strokeRect(r, lit ? kAccent.alpha(0.9f) : kBorder, 1.0f, kRadiusControl);
    }
    const float x = r.x + 11.0f, y = r.center().y;
    const Point top{x, y - 4.0f}, bottom{x, y + 4.0f}, carrier{x + 9.0f, y};
    c.line(top, carrier, ink, 1.0f);
    c.line(bottom, carrier, ink, 1.0f);
    c.line(carrier, {carrier.x + 6.0f, y}, ink, 1.0f);
    const Color fill = pill ? (lit ? mix(kTrack, kAccent, 0.24f) : kTrack) : (lit ? mix(kRaised, kAccent, 0.18f) : kRaised);
    for (const auto &[p, radius] : {std::pair{top, 2.0f}, std::pair{bottom, 2.0f}, std::pair{carrier, 2.6f}}) {
        c.fillCircle(p, radius, fill);
        c.strokeCircle(p, radius, ink, 1.1f);
    }
    const float nameX = x + 22.0f;
    c.text({nameX, r.y - kAddNameLift, 80.0f, r.h}, tr(Str::ButtonAlgorithm), kTextLabel, ink, Align::Left, true);
    if (!badge.empty()) {
        const float w = 10.0f + 6.0f * static_cast<float>(badge.size());
        const Rect tag{r.right() - 26.0f - w, r.y + 5.0f, w, r.h - 10.0f};
        c.fillRect(tag, kAccent.alpha(0.24f), kRadiusControl);
        c.text(tag, badge, kTextSmall, lightTheme() ? kText : kAccent, Align::Center, true);
    }
    paintChevron(c, {r.right() - 13.0f, y}, pill && !lit ? kDim : ink);
}

// The glow behind the routing graph: a bloom of the accent from OUT, strongest on the core and fading smoothly out to
// as far as it reaches, its corners rounded as the panel's are. Where each of the target's own pixels lies from OUT
// (`scale` a logical pixel, held to 3, past which it is drawn stretched) is worked out once, over the part of the panel
// the glow can reach; then its pixels are filled for how strong it is and how far it reaches (see
// RoutingPanel::radarLook()), rounded up or down by an 8x8 ordered dither: the glow is so faint that it spans a few
// levels of each channel, which would otherwise show as rings.
constexpr float kRadarReach = 520.0f; // at the loudest, the outermost ring's
// The same behind the effects graph, from its OUT, over a box less tall: its rings closer together.
constexpr float kFxRadarShare = 0.6f;
void shapeRadarGlow(RoutingPanel::RadarGlow &glow, float scale, Rect panel = kFxPanel, Point center = kOutCore,
                    float corner = kPanelRadius, float reach = kRadarReach) {
    scale = std::clamp(scale, 0.25f, 3.0f);
    glow.panel = panel;
    glow.center = center;
    const float left = std::max(panel.x, center.x - reach), top = std::max(panel.y, center.y - reach);
    const float right = std::max(left + 1.0f, std::min(panel.right(), center.x + reach)),
                bottom = std::max(top + 1.0f, std::min(panel.bottom(), center.y + reach));
    glow.area = {left, top, right - left, bottom - top};
    glow.scale = scale;
    const int w = static_cast<int>(std::ceil(glow.area.w * scale)), h = static_cast<int>(std::ceil(glow.area.h * scale));
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    glow.image.width = w;
    glow.image.height = h;
    glow.image.pixels.assign(count, 0u);
    glow.distance.assign(count, 0.0f);
    glow.cover.assign(count, 1.0f);
    glow.strength = -1.0f; // filled again
    const float sx = glow.area.w / static_cast<float>(w), sy = glow.area.h / static_cast<float>(h); // logical a pixel
    for (int y = 0; y < h; ++y) {
        const float ly = glow.area.y + (static_cast<float>(y) + 0.5f) * sy;
        const float dy = std::max({panel.y + corner - ly, ly - (panel.bottom() - corner), 0.0f});
        for (int x = 0; x < w; ++x) {
            const float lx = glow.area.x + (static_cast<float>(x) + 0.5f) * sx;
            const float dx = std::max({panel.x + corner - lx, lx - (panel.right() - corner), 0.0f});
            const size_t at = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            glow.distance[at] = std::hypot(lx - center.x, ly - center.y);
            if (dx > 0.0f && dy > 0.0f)
                glow.cover[at] = std::clamp((corner - std::hypot(dx, dy)) / sx + 0.5f, 0.0f, 1.0f);
        }
    }
}

// Its pixels for `strength` (its alpha on the core) reaching `reach` out, in `accent`: as light from a point falls off,
// steeply by the core and ever more gently further out, 1 / (1 + (u / 0.28)^2) of it, u the way out, tapered to
// nothing at its reach by (1 - u^2)^2.
void fillRadarGlow(RoutingPanel::RadarGlow &glow, float strength, float reach, Color accent) {
    constexpr int kBayer[8][8] = {{0, 32, 8, 40, 2, 34, 10, 42},   {48, 16, 56, 24, 50, 18, 58, 26},
                                  {12, 44, 4, 36, 14, 46, 6, 38},  {60, 28, 52, 20, 62, 30, 54, 22},
                                  {3, 35, 11, 43, 1, 33, 9, 41},   {51, 19, 59, 27, 49, 17, 57, 25},
                                  {15, 47, 7, 39, 13, 45, 5, 37},  {63, 31, 55, 23, 61, 29, 53, 21}};
    const int w = glow.image.width, h = glow.image.height;
    const float scaled = strength * 255.0f, across = 1.0f / (reach * reach);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t at = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
            const float u2 = glow.distance[at] * glow.distance[at] * across;
            if (u2 >= 1.0f) {
                glow.image.pixels[at] = 0u;
                continue;
            }
            const float rest = 1.0f - u2, light = 1.0f / (1.0f + u2 * (1.0f / (0.28f * 0.28f)));
            const float a = light * rest * rest * glow.cover[at] * scaled;
            const float t = (static_cast<float>(kBayer[y & 7][x & 7]) + 0.5f) / 64.0f;
            auto level = [&](float v) { return static_cast<uint32_t>(std::clamp(std::floor(v + t), 0.0f, 255.0f)); };
            glow.image.pixels[at] =
                level(a) << 24 | level(accent.r * a) << 16 | level(accent.g * a) << 8 | level(accent.b * a);
        }
    static uint64_t serials = 0;
    glow.image.serial = ++serials;
    glow.strength = strength;
    glow.reach = reach;
    glow.accent = accent;
}

// The graph's backdrop, under the whole panel, the inspector's lens too: the glow spreading from OUT, the orbit rings
// round it and two faint rays from it to the lens's far corners, the panel's corner left clear for the tab shown.
void paintRadar(Canvas &c, const RoutingPanel::RadarGlow &glow) {
    c.pushClip(kFxPanel);
    if (glow.strength > 0.0f)
        c.image(glow.image, glow.area);
    // (Ending at the middles of the lens's corners' arcs, under the lens, so they don't show past its rounded corners.)
    const float in = kRadiusPanel;
    c.line(kOutCore, {kInspector.x + in, kInspector.y + in}, mix(kPanel, kBorder, 0.45f), 1.0f);
    c.line(kOutCore, {kInspector.x + in, kInspector.bottom() - in}, mix(kPanel, kBorder, 0.45f), 1.0f);
    c.strokeCircle(kOutCore, 180.0f, kBorder, 1.0f);
    c.strokeCircle(kOutCore, 340.0f, mix(kPanel, kBorder, 0.7f), 1.0f);
    c.strokeCircle(kOutCore, 500.0f, mix(kPanel, kBorder, 0.45f), 1.0f);
    c.popClip();
}

// OUT: a disc shading from the accent in its middle to a darker accent at its edge, in fine steps, in a faint halo,
// ringed in the accent while a route held over it would go there.
void paintOutCore(Canvas &c, bool target) {
    c.fillCircle(kOutCore, kOutCoreRadius + 14.0f, kAccent.alpha(0.08f));
    const Color deep = mix(kAccent, kBackground, 0.3f);
    c.fillCircle(kOutCore, kOutCoreRadius, deep);
    constexpr int kSteps = 20;
    for (int k = 1; k < kSteps; ++k) {
        const float t = static_cast<float>(k) / (kSteps - 1);
        c.fillCircle(kOutCore, kOutCoreRadius * (1.0f - 0.85f * t), mix(deep, kAccent, t));
    }
    if (target)
        c.strokeCircle(kOutCore, kOutCoreRadius + 5.0f, kAccent, 2.0f);
    c.text({kOutCore.x - kOutCoreRadius, kOutCore.y - 9.0f, 2.0f * kOutCoreRadius, 18.0f}, tr(Str::BusOut), kTextLabel,
           kPanel, Align::Center, true);
}


// A routing node's fill: the panel's color, tinted with its own under the mouse; the node the inspector shows solid
// in its color, a little lighter under the mouse.
Color nodeFace(Color color, bool selected, bool hovered) {
    if (selected)
        return hovered ? mix(color, kText, 0.15f) : color;
    return hovered ? mix(kPanel, color, 0.12f) : kPanel;
}

// Whether a label's chip `r` comes within 2 px of any of the labels `taken`.
bool meetsLabel(Rect r, const std::vector<Rect> &taken) {
    constexpr float kGap = 2.0f;
    for (const Rect &other : taken)
        if (r.x < other.right() + kGap && other.x < r.right() + kGap && r.y < other.bottom() + kGap &&
            other.y < r.bottom() + kGap)
            return true;
    return false;
}

// The grid's points nearest `want` first; a point a node fits at, clear of the others, takes it.
template <class Fits>
Point nearestGridPoint(Rect area, int cols, int rows, Point want, Fits fits, Point otherwise) {
    std::vector<Point> points;
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col)
            points.push_back(gridPoint(area, cols, rows, col, row));
    std::sort(points.begin(), points.end(), [&](Point a, Point b) {
        return std::hypot(a.x - want.x, a.y - want.y) < std::hypot(b.x - want.x, b.y - want.y);
    });
    for (const Point p : points)
        if (fits(p))
            return p;
    return otherwise;
}

} // namespace

void detail::paintGridMarks(Canvas &c, Rect area, int cols, int rows, const Point *target) {
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col) {
            const Point p = gridPoint(area, cols, rows, col, row);
            if (target && std::hypot(p.x - target->x, p.y - target->y) < 0.5f)
                continue; // drawn lit below
            c.line({p.x - 3.5f, p.y}, {p.x + 3.5f, p.y}, kFaint.alpha(0.45f), 1.0f);
            c.line({p.x, p.y - 3.5f}, {p.x, p.y + 3.5f}, kFaint.alpha(0.45f), 1.0f);
        }
    if (target) {
        c.line({target->x - 7.0f, target->y}, {target->x + 7.0f, target->y}, kAccent, 1.6f);
        c.line({target->x, target->y - 7.0f}, {target->x, target->y + 7.0f}, kAccent, 1.6f);
    }
}

RoutingPanel::RoutingPanel(PanelHost &host)
    : host_(host), routing_(host.routingModel()), table_(paramTable()) {}

bool RoutingPanel::graphShown() const {
    return controller().layout().view == RoutingView::Graph && !host_.shapeEditorOpen() && !host_.partialEditorOpen();
}

bool RoutingPanel::shown() const {
    const RoutingView view = controller().layout().view;
    return (view == RoutingView::Graph || view == RoutingView::Matrix) && !host_.shapeEditorOpen() &&
           !host_.partialEditorOpen();
}

// --- Geometry.

bool RoutingPanel::isNoise(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Noise);
}

Point RoutingPanel::routePoint(int node) const {
    return node == kNodes ? kOutCore : routing_.nodeCenter(node);
}

int RoutingPanel::rerouting() const {
    return (drag_.kind == DragKind::Reroute || drag_.kind == DragKind::RerouteFrom) && drag_.moved ? drag_.widget.param
                                                                                                    : -1;
}

int RoutingPanel::nodeAt(Point p) const {
    for (int node = kNodes - 1; node >= 0; --node) {
        if (!routing_.nodeOn(node))
            continue;
        const Point c = routing_.nodeCenter(node);
        const bool inside = isBoxNode(node)
                                ? std::abs(p.x - c.x) <= kFilterHalfW + 4.0f && std::abs(p.y - c.y) <= kFilterHalfH + 4.0f
                                : std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius + 4.0f;
        if (inside)
            return node;
    }
    return -1;
}

// A route's amount as modulation takes it now (see PanelHost::liveValue()), as a share of its range.
double RoutingPanel::liveAmount(int param) const {
    const ParamDef &def = table_[param];
    return (host_.liveValue(param) - def.min) / (def.max - def.min);
}

bool RoutingPanel::reachesOut(int node) const {
    // A search along the routes that are open, from `node` through switched-on nodes to one that goes out.
    std::array<bool, kNodes> visited{};
    auto reaches = [&](auto &self, int from) -> bool {
        if (visited[from] || !routing_.nodeOn(from))
            return false;
        visited[from] = true;
        if (liveAmount(outParam(from)) > 0.0)
            return true;
        for (int to = 0; to < kNodes; ++to)
            if (to != from && liveAmount(routeParam(from, to)) > 0.0 && self(self, to))
                return true;
        return false;
    };
    return reaches(reaches, node);
}

double RoutingPanel::outLevel(int node) const {
    if (!routing_.nodeOn(node))
        return 0.0;
    double level = liveAmount(outParam(node));
    for (int to = 0; to < kNodes; ++to)
        if (to != node && liveAmount(routeParam(node, to)) > level && reachesOut(to))
            level = liveAmount(routeParam(node, to));
    return level;
}

Point RoutingPanel::labelAlong(Point start, Point control, Point end, Point middle, float width,
                       const std::vector<Rect> &taken) const {
    constexpr float kClear = kSelectedRing + 1.0f; // clear of a selected node's ring too
    constexpr float kStep = 0.035f;                // along the curve, either way from its middle
    constexpr int kSteps = 8;                      // and no further than a fifth of it from either end
    const std::vector<int> nodes = routing_.enabledNodes();
    std::vector<Point> centers;
    centers.reserve(nodes.size());
    for (int node : nodes)
        centers.push_back(routing_.nodeCenter(node));
    auto clear = [&](Point p, bool ofLabels) {
        const Rect r = chip(p, width);
        for (size_t i = 0; i < nodes.size(); ++i)
            if (overNode(nodes[i], centers[i], r, kClear))
                return false;
        return !ofLabels || !meetsLabel(r, taken);
    };
    const Rect inside{kRouting.x + 0.5f * width + 4.0f, kRouting.y + 12.0f, kRouting.w - width - 8.0f,
                      kRouting.h - 24.0f}; // where its center keeps the chip in the panel, as wireInside() has it
    for (const bool ofLabels : {true, false}) {
        if (clear(middle, ofLabels))
            return middle;
        for (int k = 1; k <= kSteps; ++k)
            for (const float side : {-1.0f, 1.0f}) {
                const float t = 0.5f + side * kStep * static_cast<float>(k), s = 1.0f - t;
                const Point p{s * s * start.x + 2.0f * s * t * control.x + t * t * end.x,
                              s * s * start.y + 2.0f * s * t * control.y + t * t * end.y};
                if (inside.contains(p) && clear(p, ofLabels))
                    return p;
            }
    }
    return middle;
}

std::vector<std::pair<int, Rect>> RoutingPanel::routeLabels() const {
    const std::vector<int> on = routing_.enabledNodes();
    const int moving = rerouting();
    std::vector<std::pair<int, Rect>> labels;
    std::vector<Rect> taken; // each at rest, a mode's glyph widening it about its middle (see labelPill())
    struct Between {
        int param;
        Wire wire;
        float width;
    };
    std::vector<Between> wires;
    for (int from : on)
        for (int to : on) {
            const int param = routeParam(from, to);
            if (from == to || !routing_.wired(param) || param == moving)
                continue;
            const int mode = routeModeParam(from, to);
            const bool glyph = mode >= 0 && routeMode(value(mode)) != RouteMode::Phase;
            wires.push_back(
                {param,
                 wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param)),
                 glyph ? kWireLabelW + kLabelGlyphW : kWireLabelW});
        }
    // A loop's label goes beside it on the right, or else on its left, above it or at an upper corner: the first
    // of these clear of the other nodes, the loops' labels before it and where the wires' labels would rest with
    // no other label about, inside the panel. Where none is, the first clear of the loops' labels alone; where none
    // is, on the right.
    std::vector<Rect> wished;
    for (const Between &b : wires)
        wished.push_back(chip(labelAlong(b.wire.start, b.wire.control, b.wire.end, b.wire.label, b.width), b.width));
    const Rect inside{kRouting.x + 4.0f, kRouting.y + 3.0f, kRouting.w - 8.0f, kRouting.h - 6.0f};
    for (int node : on)
        if (const int param = routeParam(node, node); routing_.wired(param) && param != moving) {
            const Point loop = loopCenter(node, routing_.nodeCenter(node));
            constexpr float kAbove = 12.0f + 0.5f * kWireLabelH; // its foot 12 px over the loop's center
            const Point offsets[] = {{kLoopLabelX, 0.0f},
                                     {-kLoopLabelX, 0.0f},
                                     {0.0f, -kAbove},
                                     {kLoopLabelX, 1.0f - kAbove},
                                     {-kLoopLabelX, 1.0f - kAbove}};
            auto clearOf = [&](Rect r, bool all) {
                if (!inside.contains({r.x, r.y}) || !inside.contains({r.right(), r.bottom()}) || meetsLabel(r, taken))
                    return false;
                if (!all)
                    return true;
                for (int other : on)
                    if (other != node && overNode(other, routing_.nodeCenter(other), r, 1.0f))
                        return false;
                return !meetsLabel(r, wished);
            };
            Rect idle = chip({loop.x + offsets[0].x, loop.y + offsets[0].y}, kLoopLabelW);
            bool found = false;
            for (const bool all : {true, false})
                for (const Point o : offsets)
                    if (const Rect r = chip({loop.x + o.x, loop.y + o.y}, kLoopLabelW); !found && clearOf(r, all)) {
                        idle = r;
                        found = true;
                    }
            labels.emplace_back(param, idle);
            taken.push_back(idle);
        }
    for (const Between &b : wires) {
        const Point at = labelAlong(b.wire.start, b.wire.control, b.wire.end, b.wire.label, b.width, taken);
        labels.emplace_back(b.param, chip(at, kWireLabelW));
        taken.push_back(chip(at, b.width));
    }
    for (int node : on)
        if (routing_.goesOut(node) && outParam(node) != moving) {
            const OutWire w = outWireOf(node, routing_.nodeCenter(node), routing_.wireBend(outParam(node)));
            // Beside the line where it is too short for the label to fit along it; else along it, clear of the nodes
            // and the labels before it.
            const Point at = w.beside ? w.label : labelAlong(w.start, w.control, w.end, w.label, kOutLabelW, taken);
            labels.emplace_back(outParam(node), chip(at, kOutLabelW));
            taken.push_back(chip(at, kOutLabelW));
        }
    return labels;
}

RoutingPanel::LabelPill RoutingPanel::labelPill(int param, int mode, Rect idle) const {
    auto mine = [&](const Widget &w) {
        return ((w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::LabelRemove) &&
                w.param == param) ||
               (w.kind == Kind::LabelMode && mode >= 0 && w.param == mode);
    };
    LabelPill pill;
    const std::optional<Widget> &hover = host_.hovered();
    const std::optional<Widget> held = host_.dragged();
    pill.open = (hover && mine(*hover)) || (mode >= 0 && host_.openList() == mode) || (held && mine(*held));
    // A mode other than Phase shows inside it at rest, at its left: the label widens by kLabelGlyphW, still centered on
    // its wire, and its depth's text sits that much right of the middle, half of kLabelGlyphW.
    const bool glyph = mode >= 0 && routeMode(value(mode)) != RouteMode::Phase;
    const float shift = glyph ? 0.5f * kLabelGlyphW : 0.0f;
    if (!pill.open) {
        pill.glyph = glyph;
        pill.depth = glyph ? Rect{idle.x - shift, idle.y, idle.w + kLabelGlyphW, idle.h} : idle;
        pill.whole = pill.depth;
        return pill;
    }
    // The depth's text stays where it rests, so it doesn't move as the pill opens and the mouse on it stays on it; the
    // mode's part goes on its left, over where a glyph shows at rest, and the x on its right.
    idle.x += shift;
    pill.depth = idle;
    if (mode >= 0)
        pill.mode = {idle.x - kLabelModeW, idle.y, kLabelModeW, idle.h};
    pill.remove = {idle.right(), idle.y, kLabelRemoveW, idle.h};
    const float left = mode >= 0 ? pill.mode.x : idle.x;
    pill.whole = {left, idle.y, pill.remove.right() - left, idle.h};
    return pill;
}

int RoutingPanel::hoveredNode() const {
    const std::optional<Widget> &hover = host_.hovered();
    if (host_.dragging() || !hover) // not while anything is dragged
        return -1;
    const Kind k = hover->kind;
    return k == Kind::Node || k == Kind::Port || k == Kind::NodeDelete ? hover->a : -1;
}

Point RoutingPanel::nodeDeleteAt(int node, Point center) {
    if (!isBoxNode(node)) // on the circle, up and to the right
        return {center.x + 0.72f * kNodeRadius, center.y - 0.72f * kNodeRadius};
    return {center.x + kFilterHalfW - 2.0f, center.y - kFilterHalfH + 2.0f}; // at a box's corner
}

// Whether operator `op` is heard: going out, or into a filter or shaper that is on.
// --- The routing graph's grid (Ctrl+drag)

bool RoutingPanel::gridShown() const {
    return host_.ctrlHeld() && graphShown() &&
           (drag_.kind == DragKind::Node || (!host_.dragging() && kRouting.contains(host_.mouse())));
}

Point RoutingPanel::gridPointFor(int node, Point want) const {
    // Where RoutingModel::nodePlace() would leave it untouched, nothing is in its way.
    auto fits = [&](Point p) {
        bool found = false;
        const Point at = routing_.nodePlace(node, p, &found);
        return found && std::hypot(at.x - p.x, at.y - p.y) < 0.5f;
    };
    return nearestGridPoint(kGraph, kRoutingGridCols, kRoutingGridRows, want, fits, routing_.nodePlace(node, want));
}

// Ctrl going down or up, or the mouse going in or out of the routing graph with it held, shows or hides its grid.
bool RoutingPanel::hoverMoves(Point before, Point now) const {
    return host_.ctrlHeld() && kRouting.contains(before) != kRouting.contains(now);
}

// --- What the graph follows of the telemetry.

// The routing graph's glow follows the output's level.
bool RoutingPanel::followOutput(const Telemetry &now) {
    const RadarLook before = radarLook();
    radarLevel_ = follow(radarLevel_, sentOutLevel(now));
    const RadarLook after = radarLook();
    return after.strength != before.strength || after.reach != before.reach;
}

// The routing graph's nodes' halos and its wires' swell follow what they show as the glow behind OUT does (see
// follow()). (The effects graph's nodes' and wires' glow follow theirs in FxPanel::poll().)
bool RoutingPanel::followRoutes() {
    bool moved = false;
    auto track = [&](float &shown, float target) {
        const float next = follow(shown, target);
        moved = moved || std::abs(next - shown) > 0.002f;
        shown = next;
    };
    for (int node = 0; node < kNodes; ++node)
        track(nodeGlow_[node], nodeGlowTarget(node));
    std::vector<int> wired;
    for (int from : routing_.enabledNodes()) {
        for (int to = 0; to < kNodes; ++to)
            if (routing_.nodeOn(to) && routing_.wired(routeParam(from, to)))
                wired.push_back(routeParam(from, to));
        if (routing_.goesOut(from))
            wired.push_back(outParam(from));
    }
    std::erase_if(wireAmounts_, [&](const auto &w) { return std::find(wired.begin(), wired.end(), w.first) == wired.end(); });
    for (int param : wired) {
        const float target = static_cast<float>(liveAmount(param));
        const auto it = std::find_if(wireAmounts_.begin(), wireAmounts_.end(), [&](const auto &w) { return w.first == param; });
        if (it == wireAmounts_.end())
            wireAmounts_.push_back({param, target});
        else
            track(it->second, target);
    }
    return moved;
}

// The panel of algorithms closes once ALGORITHM goes: another view, an editor over the panels.
bool RoutingPanel::step() {
    if (algorithmMenuOpen_ && !shown()) {
        algorithmMenuOpen_ = false;
        return true;
    }
    return false;
}

// How strongly a routing node's halo lights up, 0 to 1, which nodeGlow_ follows: an operator's as brightly as its
// envelope and the routes that take it to the output let it through, where modulation takes them now, so a route an LFO
// opens from 0 % lights it up; a filter's or shaper's alike, as loud as what goes through it (see boxLevel()).
float RoutingPanel::nodeGlowTarget(int node) const {
    if (!routing_.nodeOn(node))
        return 0.0f;
    const float level = node < kOps ? std::min(host_.telemetry().opLevels[node], 1.0f) : boxLevel(node);
    return level * static_cast<float>(std::sqrt(outLevel(node)));
}

// How loud what goes through a filter or shaper is, 0 to 1, as an operator's envelope says how loud it is: the
// loudest of what feeds it, each as loud as its source (an operator by its envelope, a filter or shaper by this in
// turn) as far as the route lets it through, where modulation takes it now. Filters and shapers can feed each other
// round in a loop, so each is looked at once.
float RoutingPanel::boxLevel(int node) const {
    std::array<bool, kNodes> seen{};
    auto level = [&](auto &self, int box) -> float {
        if (seen[box] || !routing_.nodeOn(box))
            return 0.0f;
        seen[box] = true;
        float most = 0.0f;
        for (int from = 0; from < kNodes; ++from) {
            if (from == box || !routing_.nodeOn(from))
                continue;
            const float through = static_cast<float>(liveAmount(routeParam(from, box)));
            if (through <= 0.0f)
                continue;
            const float source = from < kOps ? std::min(host_.telemetry().opLevels[from], 1.0f) : self(self, from);
            most = std::max(most, source * std::min(through, 1.0f));
        }
        return most;
    };
    return host_.telemetry().voices > 0 ? level(level, node) : 0.0f;
}

// A wire's amount as its swell shows it, following its modulation (see follow()); a wire not followed yet, as it is.
float RoutingPanel::shownAmount(int param) const {
    const auto it = std::find_if(wireAmounts_.begin(), wireAmounts_.end(), [&](const auto &w) { return w.first == param; });
    return it != wireAmounts_.end() ? it->second : static_cast<float>(liveAmount(param));
}

// How loud what the routing graph's nodes send to OUT is, 0 to 1, for its glow: the level of the effects graph's IN,
// the voices' sum before any effect (a peak falling away about 90 dB a second, as the effects graph's nodes glow), the
// 12 dB the voices go into the effects with given back, from -48 dB up to full scale.
float RoutingPanel::sentOutLevel(const Telemetry &t) {
    const float level = t.age < 0.5f ? t.fxPortLevels[kFxPortIn] : 0.0f;
    if (!(level > 0.0f))
        return 0.0f;
    return std::clamp((20.0f * std::log10(level) + 12.0f + 48.0f) / 48.0f, 0.0f, 1.0f);
}

// How the routing graph's glow shows, as loud as the output has risen to (see followOutput()): its alpha on OUT from
// 30 % of its strength in silence to all of it at the loudest, and its reach from half the outermost ring's to all of
// it, so it spreads out as the sound swells; and that as strongly as the glow setting says (20 % on the core at the
// loudest with it at 75 %, none with it Off). In 64 steps of loudness, so it is filled and painted again only as it
// shows.
RoutingPanel::RadarLook RoutingPanel::radarLook() const {
    const float setting = host_.shownGlow() / 0.75f;
    if (setting <= 0.0f)
        return {0.0f, kRadarReach};
    const float loud = std::round(radarLevel_ * 64.0f) / 64.0f;
    return {0.2f * setting * (0.3f + 0.7f * loud), kRadarReach * (0.5f + 0.5f * loud)};
}

// --- Widgets.

void RoutingPanel::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    // A wire's or OUT label in the graph: the label, then, while it is a pill (see labelPill()), its mode's part and its
    // x, all three drawn as one.
    auto labelWidgets = [&](Kind kind, int param, int mode, Rect idle, int from, int to) {
        const LabelPill pill = labelPill(param, mode, idle);
        add(kind, pill.depth, param, from, kind == Kind::OutLabel ? -1 : to);
        list.back().modeGlyph = pill.glyph;
        if (!pill.open)
            return;
        list.back().group = pill.whole;
        if (mode >= 0) {
            add(Kind::LabelMode, pill.mode, mode, from, to);
            list.back().group = pill.whole;
        }
        add(Kind::LabelRemove, pill.remove, param, from, to);
        list.back().group = pill.whole;
    };
    if (!shown())
        return;
    add(Kind::AlgorithmButton, kAlgorithmButton);
    const RoutingView view = controller().layout().view;
    // Switched-off nodes are left out of the graph, and greyed out in the matrix; their tabs switch them back on.
    const std::vector<int> on = routing_.enabledNodes();
    if (view == RoutingView::Matrix) {
        // Rows feed columns; the last column is the output. The rows' and columns' names work like the
        // nodes' tabs.
        for (int row = 0; row < kNodes; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                add(Kind::GridCell, gridCell(row, col), gridParam(row, col), row, col);
            }
        }
        for (int node = 0; node < kNodes; ++node) {
            const Rect row = gridCell(node, 0), col = gridCell(0, node);
            add(Kind::MatrixLabel, {row.x - 32.0f, row.y, 28.0f, row.h}, -1, node);
            add(Kind::MatrixLabel, {col.x - 2.0f, col.y - kMatrixNamesH, col.w + 4.0f, 18.0f}, -1, node);
        }
        return;
    }
    // The wires between the nodes, under everything else: dragged sideways, they bend. Then the nodes, each with
    // its edge, then depth labels and arrowheads on top of them (popups() lifts the labels over all of it).
    const int rerouted = rerouting();
    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (from == to || !routing_.wired(param) || param == rerouted)
                continue;
            const Wire w =
                wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param));
            const float left = std::min({w.start.x, w.control.x, w.end.x});
            const float top = std::min({w.start.y, w.control.y, w.end.y});
            const float right = std::max({w.start.x, w.control.x, w.end.x});
            const float bottom = std::max({w.start.y, w.control.y, w.end.y});
            add(Kind::Wire, Rect{left, top, right - left, bottom - top}.inset(-kWireGrab), param, from, to);
        }
        // Its line to OUT bends as they do.
        if (routing_.goesOut(from) && outParam(from) != rerouted) {
            const OutWire w = outWireOf(from, routing_.nodeCenter(from), routing_.wireBend(outParam(from)));
            const float left = std::min({w.start.x, w.control.x, w.end.x});
            const float top = std::min({w.start.y, w.control.y, w.end.y});
            const float right = std::max({w.start.x, w.control.x, w.end.x});
            const float bottom = std::max({w.start.y, w.control.y, w.end.y});
            add(Kind::Wire, Rect{left, top, right - left, bottom - top}.inset(-kWireGrab), outParam(from), from,
                kNodes);
        }
    }
    for (int node : on) {
        const Point c = routing_.nodeCenter(node);
        const Rect r = isBoxNode(node)
                           ? Rect{c.x - kFilterHalfW, c.y - kFilterHalfH, 2.0f * kFilterHalfW, 2.0f * kFilterHalfH}
                           : Rect{c.x - kNodeRadius, c.y - kNodeRadius, 2.0f * kNodeRadius, 2.0f * kNodeRadius};
        add(Kind::Node, r, -1, node);
        add(Kind::Port, r.inset(-kEdgeOut), -1, node);
    }
    const int moving = rerouting(); // follows the mouse, without its label
    // The labels, clear of the nodes and of each other (see routeLabels()), in the order the loops above go.
    const std::vector<std::pair<int, Rect>> labels = routeLabels();
    auto labelOf = [&](int param) {
        const auto it = std::find_if(labels.begin(), labels.end(), [&](const auto &l) { return l.first == param; });
        return it != labels.end() ? it->second : Rect{};
    };
    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (!routing_.wired(param) || param == moving)
                continue;
            labelWidgets(Kind::WireLabel, param, routeModeParam(from, to), labelOf(param), from, to);
        }
    }
    for (int node : on)
        if (routing_.goesOut(node) && outParam(node) != moving)
            labelWidgets(Kind::OutLabel, outParam(node), -1, labelOf(outParam(node)), node, kNodes);
    // The arrowheads: dragged onto another node or the bus, they move their routes there.
    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (from == to || !routing_.wired(param) || param == moving)
                continue;
            const Wire w =
                wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param));
            add(Kind::WireEnd, wireEndGrip(w.end, w.tangent), param, from, to);
        }
        if (routing_.goesOut(from) && outParam(from) != moving) {
            const OutWire w = outWireOf(from, routing_.nodeCenter(from), routing_.wireBend(outParam(from)));
            add(Kind::WireEnd, wireEndGrip(w.end, w.dir), outParam(from), from, kNodes);
        }
    }
    // Where the wires leave their nodes, over the nodes' edges: dragged onto another node, they come from there.
    auto startGrip = [](Point start, Point toward) {
        return wireStartGrip(start, normalize({toward.x - start.x, toward.y - start.y}));
    };
    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (from == to || !routing_.wired(param) || param == moving)
                continue;
            const Wire w =
                wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param));
            add(Kind::WireStart, startGrip(w.start, w.control), param, from, to);
        }
        if (routing_.goesOut(from) && outParam(from) != moving) {
            const OutWire w = outWireOf(from, routing_.nodeCenter(from), routing_.wireBend(outParam(from)));
            add(Kind::WireStart, startGrip(w.start, w.control), outParam(from), from, kNodes);
        }
    }
    // The node under the mouse: an x in a circle at its upper right, over its edge, which switches it off.
    for (int node : on)
        if (node == hoveredNode()) {
            const Point x = nodeDeleteAt(node, routing_.nodeCenter(node));
            add(Kind::NodeDelete, {x.x - 8.0f, x.y - 8.0f, 16.0f, 16.0f}, -1, node);
        }
    // The nodes that are off, as dashed ghosts docked under OUT, in the graph's bottom right, over any node moved
    // there.
    float x = kRouting.right() - 16.0f + 6.0f;
    for (int node = 0; node < kNodes; ++node)
        if (!routing_.nodeOn(node))
            x -= (isBoxNode(node) ? 34.0f : 24.0f) + 6.0f;
    for (int node = 0; node < kNodes; ++node) {
        if (routing_.nodeOn(node))
            continue;
        const float w = isBoxNode(node) ? 34.0f : 24.0f;
        add(Kind::NodeGhost, {x, kRouting.bottom() - 34.0f, w, 24.0f}, -1, node);
        x += w + 6.0f;
    }
}

void RoutingPanel::popups(std::vector<Widget> &list) const {
    // The graph's labels go over every widget listed so far, an open pill over the labels at rest, and all of them under
    // the lists that follow: they take the mouse over a node, a wire's end or a depth handle. (The I/O tab's depths,
    // labels too, stay on the inspector's page.)
    if (graphShown()) {
        auto isLabel = [](const Widget &w) {
            return w.kind == Kind::LabelMode || w.kind == Kind::LabelRemove ||
                   ((w.kind == Kind::WireLabel || w.kind == Kind::OutLabel) && kRouting.contains(w.rect.center()));
        };
        const auto labels = std::stable_partition(list.begin(), list.end(), [&](const Widget &w) { return !isLabel(w); });
        std::stable_partition(labels, list.end(), [](const Widget &w) { return w.group.w <= 0.0f; });
    }
    const WidgetAdder add{list};
    // The panel of algorithms, under ALGORITHM: the set of its switch, by how many operators they use.
    if (algorithmMenuOpen_ && shown()) {
        static_assert(std::size(kAlgorithmSets) == kAlgorithmSetCount);
        const AlgorithmRange shownSet = algorithmSetRange(algorithmOps_);
        add(Kind::AlgorithmMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        const size_t panel = list.size();
        add(Kind::AlgorithmMenu, algorithmPanel(shownSet.count), -1, 1);
        const size_t sets = list.size();
        for (int set = 0; set < kAlgorithmSetCount; ++set)
            add(Kind::AlgorithmSet, algorithmSetButton(shownSet.count, set), -1, -1, kAlgorithmSets[set]);
        mergeButtons(list, sets);
        for (int i = 0; i < shownSet.count; ++i)
            add(Kind::AlgorithmItem, algorithmCell(shownSet.count, i), -1, shownSet.first + i);
        for (size_t i = panel; i < list.size(); ++i)
            list[i].popup = true; // over everything else, as an open list
    }
    // A route's list of modes, under the mode's part of its label's pill, while the graph shows that route: a list the
    // View picks from as it picks every list's (see PanelHost::openList()).
    const int open = host_.openList();
    if (const int from = routeModeFrom(open), to = routeModeTo(open);
        from >= 0 && graphShown() && routing_.nodeOn(from) && routing_.nodeOn(to) &&
        routing_.wired(routeParam(from, to)) && rerouting() != routeParam(from, to)) {
        Rect idle;
        for (const auto &[param, rect] : routeLabels())
            if (param == routeParam(from, to))
                idle = rect;
        const Rect part = labelPill(routeParam(from, to), open, idle).mode;
        const Rect button{part.x, part.y, 112.0f, part.h};
        const int items = static_cast<int>(RouteMode::Count);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::ListMenu, listMenu(button, items), -1, 1);
        for (int item = 0; item < items; ++item)
            add(Kind::ListItem, listMenuItem(button, items, item), open, -1, item);
    }
}

// --- Painting.

void RoutingPanel::paintBack(Canvas &c) {
    if (controller().layout().view == RoutingView::Matrix)
        paintMatrixLabels(c);
    else
        paintRouting(c);
}

void RoutingPanel::paintRouting(Canvas &c) {
    // The glow's distances are worked out again only as the editor is zoomed, and its pixels filled again as its strength,
    // its reach or the theme's accent changes.
    const float scale = std::clamp(c.deviceScale(), 0.25f, 3.0f);
    if (std::abs(scale - radarGlow_.scale) > 1e-3f)
        shapeRadarGlow(radarGlow_, scale);
    const RadarLook glow = radarLook();
    if (glow.strength > 0.0f && (glow.strength != radarGlow_.strength || glow.reach != radarGlow_.reach ||
                                 kAccent.r != radarGlow_.accent.r || kAccent.g != radarGlow_.accent.g ||
                                 kAccent.b != radarGlow_.accent.b))
        fillRadarGlow(radarGlow_, glow.strength, glow.reach, kAccent);
    else if (glow.strength <= 0.0f)
        radarGlow_.strength = 0.0f;
    paintRadar(c, radarGlow_);
    c.text({kHintX, kRouting.y + 10.0f, kRouting.right() - 16.0f - kHintX, 22.0f},
           tr(Str::HintRouting), kTextLabel, kFaint, Align::Right);
    if (gridShown()) { // Ctrl held: the grid's points, under everything, the one a node dragged lands on lit
        const Point target = drag_.kind == DragKind::Node ? routing_.nodeCenter(drag_.widget.a) : Point{};
        paintGridMarks(c, kGraph, kRoutingGridCols, kRoutingGridRows, drag_.kind == DragKind::Node ? &target : nullptr);
    }
    // The lens's leader: from the node the inspector shows, while it is in the graph, level across to the lens.
    if (const std::optional<Point> leader = lensLeaderStart(routing_, controller().layout().selectedNode, graphShown())) {
        const Color color = nodeColor(controller().layout().selectedNode);
        c.line(*leader, {kInspector.right(), leader->y}, color.alpha(0.18f), 1.5f, true);
        c.fillCircle(*leader, 2.5f, color.alpha(0.4f));
    }

    // OUT, lit while a new or moving route would go there.
    const int moving = rerouting(); // drawn following the mouse instead
    const int busParam = drag_.kind == DragKind::Connect || moving >= 0 ? outParam(drag_.widget.a) : -1;
    const bool busTarget = busParam >= 0 && onOutCore(drag_.now) &&
                           (drag_.kind == DragKind::Connect || busParam == moving || !routing_.wired(busParam));
    paintOutCore(c, busTarget);

    // The operators' halos and unison copies, under the wires, so an arrowhead coming in at a node's side shows over
    // the copies peeking out there.
    const std::vector<int> on = routing_.enabledNodes();
    for (int node : on)
        if (node < kOps)
            paintOpBacking(c, node, routing_.nodeCenter(node));

    // Modulated routes swell and thin with their modulation while notes play. Under the mouse, or while its depth is
    // dragged (on its label, or a knob's) or it is bent, a route is drawn in the text's color: a route's own ends,
    // dragged, hold it without that.
    const std::optional<Widget> &hovered = host_.hovered();
    const std::optional<Widget> held = host_.dragged();
    auto focused = [&](int param) {
        return (hovered && hovered->param == param && !host_.dragging()) ||
               (held && held->param == param && held->kind != Kind::WireEnd && held->kind != Kind::WireStart);
    };
    for (int node : on) {
        const int param = outParam(node);
        if (!routing_.goesOut(node) || param == moving)
            continue;
        const float out = shownAmount(param);
        const OutWire w = outWireOf(node, routing_.nodeCenter(node), routing_.wireBend(param));
        const Color color = focused(param) ? kText : nodeColor(node).alpha(0.35f + 0.65f * out);
        c.curve(w.start, w.control, w.end, color, 1.2f + 2.4f * out);
        arrowHead(c, w.end, w.dir, color);
    }

    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (!routing_.wired(param) || param == moving)
                continue;
            const float amount = shownAmount(param);
            const Color base = routeColor(from, to);
            const Color color = focused(param) ? kText : base.alpha(0.35f + 0.65f * amount);
            const float width = 1.2f + 2.4f * amount;
            if (from == to) {
                c.strokeCircle(loopCenter(from, routing_.nodeCenter(from)), 9.0f, color, width);
                continue;
            }
            const Wire w =
                wireBetween(from, routing_.nodeCenter(from), to, routing_.nodeCenter(to), routing_.wireBend(param));
            c.curve(w.start, w.control, w.end, color, width);
            arrowHead(c, w.end, w.tangent, color);
        }
    }
}

void RoutingPanel::paintMatrixLabels(Canvas &c) {
    // A node that is off has its name greyed out, like its row and column of cells; the name under the mouse
    // lights up. The node the inspector shows has its names on a plate, like its tab: a click on them switches
    // it off or on again, as on the tab, and a click on another name selects that node (switching it on).
    const int selected = controller().layout().selectedNode;
    auto nameColor = [&](int node) {
        if (const std::optional<Widget> &hover = host_.hovered();
            hover && hover->kind == Kind::MatrixLabel && hover->a == node)
            return kText;
        // In a flat theme an operator's name takes its own color too, as its cells, its tab and its plate do.
        const Color color = isBoxNode(node) || look.flat ? nodeColor(node) : (node == selected ? kText : kDim);
        return routing_.nodeOn(node) ? color : (isBoxNode(node) ? color.alpha(0.35f) : kFaint);
    };
    const Color plate = nodeColor(selected).alpha(routing_.nodeOn(selected) ? 0.22f : 0.1f);
    const Rect first = gridCell(selected, 0), top = gridCell(0, selected); // the first cells of its row and column
    c.fillRect({first.x - 31.0f, first.y + 3.0f, 26.0f, first.h - 6.0f}, plate, kRadiusControl);
    c.fillRect({top.x - 1.0f, top.y - kMatrixNamesH, top.w + 2.0f, 18.0f}, plate, kRadiusControl);
    for (int col = 0; col < kGridCols; ++col) {
        const Rect cell = gridCell(0, col);
        const bool out = col == kGridCols - 1;
        c.text({cell.x - 6.0f, cell.y - kMatrixNamesH, cell.w + 12.0f, 18.0f},
               out ? tr(Str::BusOut) : kNodeNames[col], kTextValue, out ? kText : nameColor(col), Align::Center, true);
    }
    for (int row = 0; row < kNodes; ++row) {
        const Rect cell = gridCell(row, 0);
        c.text({cell.x - 32.0f, cell.y, 28.0f, cell.h}, kNodeNames[row], kTextValue, nameColor(row), Align::Center, true);
    }

    // Legend beside the grid, its lines broken at their spaces to fit the width the cells leave it.
    const float width = kLegendW;
    constexpr float kLineH = 15.0f;
    auto wrapped = [&](float x, float top, float room, std::string_view text, float size, Color color) {
        int lines = 0;
        while (!text.empty()) {
            size_t end = text.size();
            while (c.textWidth(text.substr(0, end), size) > room) {
                const size_t space = text.substr(0, end).rfind(' ');
                if (space == std::string_view::npos || space == 0)
                    break; // a single word wider than the room stays whole
                end = space;
            }
            c.text({x, top + static_cast<float>(lines) * kLineH, room, 20.0f}, text.substr(0, end), size, color, Align::Left);
            text.remove_prefix(end);
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);
            ++lines;
        }
        return lines;
    };
    float y = kRouting.y + 36.0f;
    c.text({kLegendX, y - 22.0f, width, 16.0f}, tr(Str::HeadingHowToRead), kTextLabel, kFaint, Align::Left, true);
    struct Entry {
        Color color;
        bool outline;
        const char *text;
        bool operators = false; // in a flat theme, in the operators' colors, a strip each, as their cells are
    };
    const Entry entries[] = {
        {kAccent, false, tr(Str::LegendFm), true},
        {kAccent, true, tr(Str::LegendFeedback), true},
        {kFilterColor, false, tr(Str::LegendFilter)},
        {kShaperColor, false, tr(Str::LegendShaper)},
        {kText, false, tr(Str::LegendOut)},
    };
    for (const Entry &e : entries) {
        const Rect swatch{kLegendX, y + 3.0f, 14.0f, 14.0f};
        const int strips = e.operators && look.flat ? kOps : 1;
        for (int k = 0; k < strips; ++k) {
            const Color color = strips > 1 ? opColor(k) : e.color;
            const float x0 = swatch.x + swatch.w * static_cast<float>(k) / static_cast<float>(strips);
            const float x1 = swatch.x + swatch.w * static_cast<float>(k + 1) / static_cast<float>(strips);
            if (strips > 1)
                c.pushClip({x0, swatch.y - 1.0f, x1 - x0, swatch.h + 2.0f});
            if (e.outline)
                c.strokeRect(swatch, color.alpha(0.6f), 1.0f, kRadiusMark);
            else
                c.fillRect(swatch, color.alpha(0.45f), kRadiusMark);
            if (strips > 1)
                c.popClip();
        }
        const int lines = wrapped(kLegendX + 24.0f, y, width - 24.0f, e.text, kTextBody, kDim);
        y += 26.0f + static_cast<float>(lines - 1) * kLineH;
    }
    y += 8.0f;
    for (const Str note : {Str::MatrixClick, Str::MatrixClickThen})
        y += static_cast<float>(wrapped(kLegendX, y, width, tr(note), kTextBody, kFaint)) * kLineH + 4.0f;
    y += 8.0f;
    for (const Str hint : {Str::HintMatrixRows, Str::HintMatrixDrag, Str::HintMatrixToggle})
        y += static_cast<float>(wrapped(kLegendX, y, width, tr(hint), kTextLabel, kFaint)) * kLineH;
}

bool RoutingPanel::opHeard(int op) const {
    bool heard = value(paramIndex(op, OpParam::Out)) > 0.0;
    for (int filter = 0; filter < kFilters; ++filter)
        heard = heard || (routing_.nodeOn(kOps + filter) && value(sendIndex(op, filter)) > 0.0);
    for (int shaper = 0; shaper < kShapers; ++shaper)
        heard = heard || (routing_.nodeOn(kOps + kFilters + shaper) && value(shaperSendIndex(op, shaper)) > 0.0);
    return heard;
}

// What an operator's node has behind it, drawn under the wires: its halo, which lights up while playing only if it
// ends up in the output, as brightly as its envelope and the routes that take it there let it through (where
// modulation takes them now: one an LFO opens from 0 % lights up too), and with unison a stack of copies peeking out
// on both sides, past the selection ring, outlined as the node is.
void RoutingPanel::paintOpBacking(Canvas &c, int op, Point center) {
    const Color color = nodeColor(op);
    if (const float level = nodeGlow_[op]; level > 0.01f) {
        if (glowing > 0.0f) // feathered, as strongly as the glow is set
            featherCircle(c, center, kNodeRadius, 16.0f, color.alpha(0.3f * level));
        else // with the glow off, a plain ring of light, so a sounding operator still shows
            c.fillCircle(center, kNodeRadius + 7.0f, color.alpha(0.22f * level));
    }
    if (std::lround(value(paramIndex(op, OpParam::Unison))) > 1 && !isNoise(op)) {
        const Color outline = opHeard(op) ? color : color.alpha(0.45f);
        for (const float dx : {-9.0f, 9.0f}) {
            const Point copy{center.x + dx, center.y};
            c.fillCircle(copy, kNodeRadius, kPanel);
            c.strokeCircle(copy, kNodeRadius, outline.alpha(0.6f), 1.5f);
        }
    }
}

void RoutingPanel::paintNode(Canvas &c, const Widget &w, bool hover) {
    const bool hovered = hover || host_.isActive(w);
    const bool selected = controller().layout().selectedNode == w.a;
    const Point center = w.rect.center();

    if (isBoxNode(w.a)) { // a filter's box with its type, or a shaper's, squarer, with its curve's name
        const bool shaper = isShaperNode(w.a);
        const float corner = shaper ? kRadiusMark : kRadiusControl;
        const Color color = nodeColor(w.a);
        const bool live = routing_.boxHasInput(w.a);
        // Its halo, as an operator's: as bright as what goes through it and on to the output (see nodeGlowTarget()),
        // feathered as the glow setting says, a plain ring of light with it off.
        if (const float level = nodeGlow_[w.a]; level > 0.01f) {
            if (glowing > 0.0f)
                featherRect(c, w.rect, corner, 16.0f, color.alpha(0.3f * level));
            else
                c.fillRect(w.rect.inset(-7.0f), color.alpha(0.22f * level), corner + 7.0f);
        }
        // Filled with the panel's color and outlined in its own, faintly while nothing goes into it; the node the
        // inspector shows filled solid with its color, its name and type in the panel's, and ringed in it.
        c.fillRect(w.rect, nodeFace(color, selected, hovered), corner);
        if (!selected)
            c.strokeRect(w.rect, live ? color : color.alpha(0.45f), 1.5f, corner);
        else
            c.strokeRect(w.rect.inset(-kSelectedRing), color.alpha(0.8f), 1.5f, corner + kSelectedRing);
        c.text({w.rect.x, w.rect.y + 2.0f, w.rect.w, 16.0f}, kNodeNames[w.a], kTextValue, selected ? kPanel : color,
               Align::Center, true);
        const int box = w.a - kOps; // the filter, or past them the shaper
        const std::string kind =
            shaper ? clipModeName(clipMode(value(paramIndex(shaperOfNode(w.a), ShaperParam::Mode))))
                   : filterLabel(static_cast<FilterType>(std::lround(value(paramIndex(box, FilterParam::Type)))),
                                 value(paramIndex(box, FilterParam::Slope)));
        c.text({w.rect.x, w.rect.y + 17.0f, w.rect.w, 14.0f}, kind, kTextLabel, selected ? kPanel.alpha(0.7f) : kDim);
        return;
    }

    const int op = w.a;
    const Color color = nodeColor(op); // the accent, or in a theme that colors each operator its own
    // A silent one's outline: its color, faintly. (Its halo and unison copies are under the wires: see
    // paintOpBacking().)
    const Color outline = opHeard(op) ? color : color.alpha(0.45f);
    // Filled with the panel's color and outlined in its own; the node the inspector shows filled solid with its color,
    // its wave, letter and ratio in the panel's, and ringed in it.
    c.fillCircle(center, kNodeRadius, nodeFace(color, selected, hovered));
    if (!selected)
        c.strokeCircle(center, kNodeRadius, outline, 1.5f);
    else
        c.strokeCircle(center, kNodeRadius + kSelectedRing, color.alpha(0.8f), 1.5f);
    const Color ink = selected ? kPanel : color;
    const Color caption = selected ? kPanel.alpha(0.7f) : kDim; // its ratio
    // Wave glyph above the letter, ratio below it.
    const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))));
    const std::vector<Point> glyph = waveIcon(wave, {center.x - 6.0f, center.y - 16.0f, 12.0f, 5.0f});
    c.polyline(glyph.data(), glyph.size(), ink, 1.2f);
    c.text({center.x - 20.0f, center.y - 11.0f, 40.0f, 20.0f}, kNodeNames[op], kTextHeading, ink, Align::Center, true);
    char ratio[16];
    const double r = ratioHundredths(value(paramIndex(op, OpParam::Ratio))) / 100.0;
    std::snprintf(ratio, sizeof(ratio), "\xC3\x97%.2g", r);
    if (value(paramIndex(op, OpParam::Fixed)) >= 0.5)
        std::snprintf(ratio, sizeof(ratio), "%s",
                      fixedHzText(value(paramIndex(op, OpParam::FixedFreq)), value(paramIndex(op, OpParam::FixedMult)),
                                  true)
                          .c_str());
    if (isNoise(op)) {
        // No ratio: its color, or its slope between the named ones.
        const double db = std::round(value(paramIndex(op, OpParam::NoiseSlope)) * 60.0) / 10.0;
        const char *const name = db == 0.0   ? tr(Str::NoiseWhiteCaption)
                                 : db == 3.0 ? tr(Str::NoisePinkCaption)
                                 : db == 6.0 ? tr(Str::NoiseBrownCaption)
                                             : nullptr;
        if (name)
            std::snprintf(ratio, sizeof(ratio), "%s", name);
        else
            std::snprintf(ratio, sizeof(ratio), "-%.1fdB", db);
    }
    c.text({center.x - 22.0f, center.y + 5.0f, 44.0f, 14.0f}, ratio, kTextLabel, caption); // inside the ring
}

// The effects graph's backdrop: the routing graph's, its glow as strong and as loud, spreading from the graph's OUT over
// its box (whose corners it keeps round), with its rings, closer together as the box is less tall. Its distances
// are worked out again as OUT moves (the graph scrolled, or gliding), the box changes or the editor is zoomed.
void RoutingPanel::paintFxBackdrop(Canvas &c, Rect area, Point out) {
    const float scale = std::clamp(c.deviceScale(), 0.25f, 3.0f);
    out = {std::round(out.x), std::round(out.y)};
    RadarLook glow = radarLook();
    glow.strength *= 1.5f; // from a smaller OUT, so it shows as much
    const float reach = glow.reach;
    auto same = [](Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
    if (glow.strength > 0.0f) {
        if (std::abs(scale - fxRadarGlow_.scale) > 1e-3f || !same(area, fxRadarGlow_.panel) || out.x != fxRadarGlow_.center.x ||
            out.y != fxRadarGlow_.center.y)
            shapeRadarGlow(fxRadarGlow_, scale, area, out, kRadiusCard);
        if (glow.strength != fxRadarGlow_.strength || reach != fxRadarGlow_.reach || kAccent.r != fxRadarGlow_.accent.r ||
            kAccent.g != fxRadarGlow_.accent.g || kAccent.b != fxRadarGlow_.accent.b)
            fillRadarGlow(fxRadarGlow_, glow.strength, reach, kAccent);
        c.image(fxRadarGlow_.image, fxRadarGlow_.area);
    }
    // On the box's darker ground, fainter than the routing graph's, to stand out from it as much.
    c.strokeCircle(out, 180.0f * kFxRadarShare, mix(kDisplayColor, kBorder, 0.6f), 1.0f);
    c.strokeCircle(out, 340.0f * kFxRadarShare, mix(kDisplayColor, kBorder, 0.42f), 1.0f);
    c.strokeCircle(out, 500.0f * kFxRadarShare, mix(kDisplayColor, kBorder, 0.27f), 1.0f);
}

void RoutingPanel::paintDrag(Canvas &c) {
    if (drag_.kind == DragKind::RerouteFrom && drag_.moved) {
        const int to = drag_.widget.b;
        // From the mouse to where it goes; away from the nodes it is faint: let go of there, it goes.
        const int source = nodeAt(drag_.now);
        const Color color = source < 0 ? kFaint : nodeColor(source);
        Point end = to == kNodes ? kOutCore : outlineToward(to, routing_.nodeCenter(to), drag_.now);
        if (to == kNodes) { // onto the core's edge, toward the mouse
            const Point out = normalize({drag_.now.x - kOutCore.x, drag_.now.y - kOutCore.y});
            end = {kOutCore.x + out.x * (kOutCoreRadius + 2.0f), kOutCore.y + out.y * (kOutCoreRadius + 2.0f)};
        }
        const Point dir = normalize({end.x - drag_.now.x, end.y - drag_.now.y});
        const Point base{end.x - dir.x * 8.0f, end.y - dir.y * 8.0f};
        c.line(drag_.now, base, color, 2.0f, true);
        arrowHead(c, base, dir, color);
        c.fillCircle(drag_.now, 3.5f, color);
        if (source >= 0) { // a route that is there already from that node rings faintly
            const int param = routeTo(source, to);
            const Color ring = param == drag_.widget.param || !routing_.wired(param) ? color : kFaint;
            const Point center = routing_.nodeCenter(source);
            if (isBoxNode(source))
                c.strokeRect({center.x - kFilterHalfW - 5.0f, center.y - kFilterHalfH - 5.0f,
                              2.0f * kFilterHalfW + 10.0f, 2.0f * kFilterHalfH + 10.0f},
                             ring, 2.0f, kRadiusControl + 5.0f);
            else
                c.strokeCircle(center, kNodeRadius + 5.0f, ring, 2.0f);
        }
    } else if (drag_.kind == DragKind::Connect || rerouting() >= 0) {
        const int from = drag_.widget.a;
        // A moving route away from the nodes and the bus is faint: let go of there, it goes.
        const bool removing = rerouting() >= 0 && nodeAt(drag_.now) < 0 && !onOutCore(drag_.now);
        const Color color = removing ? kFaint : nodeColor(from);
        // The route leaves its node toward the mouse; a moving one has its arrowhead under it.
        const Point origin = routing_.nodeCenter(from);
        const Point start = outlineToward(from, origin, drag_.now);
        if (nodeAt(drag_.now) == from && drag_.leftNode) {
            // Held back over its own node, once it has been away from it, it loops back into it: let go of there,
            // the node feeds itself.
            const LoopArc loop = loopbackArc(from, origin); // on the ring that rings the node it is held over
            const float end = loop.to - 8.0f / loop.radius; // an arrowhead's length short of the outline
            std::array<Point, 33> points;
            for (size_t i = 0; i < points.size(); ++i)
                points[i] = loop.at(loop.from + (end - loop.from) * static_cast<float>(i) /
                                                    static_cast<float>(points.size() - 1));
            c.polyline(points.data(), points.size(), color, 2.0f, true);
            const Point tip = loop.at(loop.to);
            arrowHead(c, points.back(), normalize({tip.x - points.back().x, tip.y - points.back().y}), color);
        } else if (drag_.kind == DragKind::Connect) {
            if (drag_.moved)
                c.line(start, drag_.now, color, 2.0f, true);
        } else {
            const Point dir = normalize({drag_.now.x - origin.x, drag_.now.y - origin.y});
            const Point base{drag_.now.x - dir.x * 4.0f, drag_.now.y - dir.y * 4.0f};
            c.line(start, base, color, 2.0f, true);
            arrowHead(c, base, dir, color);
        }
        const int target = nodeAt(drag_.now);
        if (target >= 0) {
            const Point center = routing_.nodeCenter(target);
            // A route a moving one would find there already rings faintly.
            const int param = routeParam(from, target);
            const bool fits = drag_.kind == DragKind::Connect || param == drag_.widget.param || !routing_.wired(param);
            const Color ring = fits ? color : kFaint;
            if (isBoxNode(target))
                c.strokeRect({center.x - kFilterHalfW - 5.0f, center.y - kFilterHalfH - 5.0f,
                              2.0f * kFilterHalfW + 10.0f, 2.0f * kFilterHalfH + 10.0f},
                             ring, 2.0f, kRadiusControl + 5.0f);
            else
                c.strokeCircle(center, kNodeRadius + 5.0f, ring, 2.0f);
        }
    }
}

// The graph's nodes, ghosts, edges, arrowheads, wires' starts and labels, the matrix's cells and ALGORITHM with its
// panel, wherever they are listed (a route's label on the operator page's I/O tab too).
bool RoutingPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::Node:
        paintNode(c, w, hovered);
        break;
    case Kind::NodeDelete: { // as on the effects graph's nodes (see FxPanel::paintFxGraph())
        const Point x = w.rect.center();
        const Color ink = hovered ? kText : kDim;
        c.fillCircle(x, 7.5f, nodeDiscFill(hovered));
        c.strokeCircle(x, 7.5f, ink, 1.2f);
        c.line({x.x - 3.0f, x.y - 3.0f}, {x.x + 3.0f, x.y + 3.0f}, ink, 1.4f);
        c.line({x.x + 3.0f, x.y - 3.0f}, {x.x - 3.0f, x.y + 3.0f}, ink, 1.4f);
        break;
    }
    case Kind::NodeGhost: {
        // Dashed, like the cards that add things, and in the node's color while hovered; the one the inspector shows
        // filled with its color, as a node shown in the graph is.
        const bool selected = controller().layout().selectedNode == w.a;
        const Color color = hovered || selected ? nodeColor(w.a) : kFaint;
        const float corner = isShaperNode(w.a) ? kRadiusMark : isFilterNode(w.a) ? kRadiusControl : w.rect.h * 0.5f;
        if (selected)
            c.fillRect(w.rect, nodeColor(w.a).alpha(kSelectedFill), corner);
        c.strokeRect(w.rect, color, 1.0f, corner, true);
        c.text(w.rect, kNodeNames[w.a], kTextLabel, selected ? kText : color, Align::Center, true);
        break;
    }
    case Kind::Port: { // under the mouse, or dragged from, the node's outline glows in its color
        if (!hovered && !(drag_.kind == DragKind::Connect && drag_.widget.a == w.a))
            break;
        const Color color = nodeColor(w.a);
        const Rect node = w.rect.inset(kEdgeOut);
        if (isBoxNode(w.a)) {
            const float radius = isShaperNode(w.a) ? kRadiusMark : kRadiusControl;
            c.strokeRect(node.inset(-2.0f), color.alpha(0.35f), 5.0f, radius + 2.0f);
            c.strokeRect(node, color, 2.5f, radius);
        } else {
            c.strokeCircle(node.center(), kNodeRadius + 2.0f, color.alpha(0.35f), 5.0f);
            c.strokeCircle(node.center(), kNodeRadius, color, 2.5f);
        }
        break;
    }
    case Kind::WireEnd: // drawn with its wire, which lights up under the mouse, a circle around its arrowhead
        if (hovered) {
            if (w.b == kNodes) {
                const OutWire out = outWireOf(w.a, routing_.nodeCenter(w.a), routing_.wireBend(w.param));
                wireEndHighlight(c, out.end, out.dir, nodeColor(w.a));
            } else {
                const Wire wire = wireBetween(w.a, routing_.nodeCenter(w.a), w.b, routing_.nodeCenter(w.b),
                                              routing_.wireBend(w.param));
                wireEndHighlight(c, wire.end, wire.tangent, nodeColor(w.a));
            }
        }
        break;
    case Kind::WireLabel:
    case Kind::OutLabel: {
        const double amount = value(w.param);
        const bool focused = hovered || active;
        // Filled solid with its route's color (a line to OUT's with its node's), its depth in the panel's; a little
        // lighter while the mouse is on its depth or drags it.
        const Color color = w.kind == Kind::OutLabel ? nodeColor(w.a) : routeColor(w.a, w.b);
        // As a pill, it draws all of it, its parts' lines and its own lit part; its parts draw what is on them.
        const bool pill = w.group.w > 0.0f;
        const Rect body = pill ? w.group : w.rect;
        c.fillRect(body, color, body.h * 0.5f);
        if (focused) {
            c.pushClip(w.rect);
            c.fillRect(body, kText.alpha(0.18f), body.h * 0.5f);
            c.popClip();
        }
        if (pill) {
            if (w.group.x < w.rect.x)
                c.line({w.rect.x, w.rect.y + 3.0f}, {w.rect.x, w.rect.bottom() - 3.0f}, kPanel.alpha(0.3f), 1.0f);
            c.line({w.rect.right(), w.rect.y + 3.0f}, {w.rect.right(), w.rect.bottom() - 3.0f}, kPanel.alpha(0.3f),
                   1.0f);
        }
        std::string text = percent(amount);
        if (w.kind == Kind::WireLabel && w.a == w.b)
            text = trf(Str::CaptionFeedback, {text});
        // In the graph larger than on the operator page's I/O tab (see kWireLabelH), its glyph, text and ranges too.
        const bool graph = kRouting.contains(w.rect.center());
        const float glyphW = graph ? kLabelGlyphW : 18.0f;
        // Its ranges and handles are over and under it, and its text stays in the middle, its digits centered.
        constexpr float lift = -1.0f;
        Rect textRect{w.rect.x, w.rect.y + lift, w.rect.w, w.rect.h};
        if (w.modeGlyph) { // its mode, other than Phase, at its left
            const auto mode = routeMode(value(routeModeParam(w.a, w.b)));
            routeModeIcon(c, {w.rect.x + (graph ? 15.0f : 13.0f), w.rect.center().y}, mode, kPanel, graph ? 1.0f : 0.85f);
            textRect = {w.rect.x + glyphW, w.rect.y + lift, w.rect.w - glyphW, w.rect.h}; // where it sits when open
        }
        c.text(textRect, text, graph ? kTextBody : kTextLabel, kPanel, Align::Center, true);
        // Just above it, clear of its solid fill (the hovered one's, twice as thick, too); the handles go below.
        const Rect ranges = graph ? Rect{w.rect.x + 9.0f, w.rect.y - 5.0f, w.rect.w - 18.0f, 2.0f}
                                  : Rect{w.rect.x + 7.0f, w.rect.y - 4.0f, w.rect.w - 14.0f, 1.5f};
        host_.paintModRange(c, ranges, w.param);
        break;
    }
    case Kind::LabelMode: {
        // Its mode's glyph and a chevron, pointing up while its list is open; lit under the mouse and while open.
        // On the pill's solid fill, in the panel's color; a little lighter under the mouse and while open.
        const bool open = host_.openList() == w.param;
        if (hovered || open) {
            c.pushClip(w.rect);
            c.fillRect(w.group, kText.alpha(open ? 0.26f : 0.18f), w.group.h * 0.5f);
            c.popClip();
        }
        const Point m{w.rect.x + 13.0f, w.rect.center().y};
        routeModeIcon(c, m, routeMode(value(w.param)), kPanel, 1.0f);
        const Point v{w.rect.right() - 8.5f, w.rect.center().y};
        const float dir = open ? -1.0f : 1.0f;
        const Point chevron[] = {{v.x - 3.0f, v.y - 1.4f * dir}, {v.x, v.y + 1.8f * dir}, {v.x + 3.0f, v.y - 1.4f * dir}};
        c.polyline(chevron, 3, kPanel, 1.6f);
        break;
    }
    case Kind::LabelRemove: {
        if (hovered) {
            c.pushClip(w.rect);
            c.fillRect(w.group, kText.alpha(0.18f), w.group.h * 0.5f);
            c.popClip();
        }
        const Point m = w.rect.center();
        const Color ink = kPanel;
        c.line({m.x - 3.5f, m.y - 3.5f}, {m.x + 3.5f, m.y + 3.5f}, ink, 1.5f);
        c.line({m.x - 3.5f, m.y + 3.5f}, {m.x + 3.5f, m.y - 3.5f}, ink, 1.5f);
        break;
    }
    case Kind::GridCell: {
        const double amount = value(w.param);
        const bool out = w.b == kGridCols - 1;
        const float a = static_cast<float>(liveAmount(w.param)); // the fill follows modulation (the macros' with no note)
        const bool boxes = isBoxNode(w.a) || (!out && isBoxNode(w.b));
        const Color fill = out ? kText.alpha(0.1f + 0.3f * a)
                               : routeColor(w.a, out ? w.a : w.b).alpha(0.12f + (boxes ? 0.5f : 0.55f) * a);
        // The row and column of a node that is off are greyed out; their routes keep their amounts.
        const bool off = !routing_.nodeOn(w.a) || (!out && !routing_.nodeOn(w.b));
        const float dim = off ? 0.35f : 1.0f;
        c.fillRect(w.rect, (amount > 0.0 ? fill : kRaised).alpha(dim), kRadiusControl);
        if (w.a == w.b)
            c.strokeRect(w.rect, nodeColor(w.a).alpha(0.35f * dim), 1.0f, kRadiusControl); // feedback diagonal
        if (hovered || active)
            c.strokeRect(w.rect, kText, 1.5f, kRadiusControl);
        char text[8];
        std::snprintf(text, sizeof(text), "%.0f", amount * 100.0);
        const bool shown = amount > 0.0 || routing_.wired(w.param); // a route kept at 0 % shows its 0, dimmed
        c.text(w.rect, shown ? text : "\xC2\xB7", kTextValue,
               off ? kFaint : (amount > 0.0 ? kText : (shown ? kDim : kFaint)), Align::Center, amount > 0.0 && !off);
        // A route that isn't Phase shows its mode's glyph in its top left corner.
        float rangeLeft = 4.0f;
        if (const int mode = out ? -1 : routeModeParam(w.a, w.b); mode >= 0 && shown) {
            const auto m = routeMode(value(mode));
            if (m != RouteMode::Phase) {
                routeModeIcon(c, {w.rect.x + 6.5f, w.rect.y + 6.5f}, m,
                              (amount > 0.0 ? kText : kDim).alpha(off ? 0.35f : 0.85f), 0.6f);
                rangeLeft = 12.0f;
            }
        }
        // Its ranges along its top (right of the glyph), its handles centered along its foot.
        host_.paintModRange(c, {w.rect.x + rangeLeft, w.rect.y + 3.5f, w.rect.w - rangeLeft - 4.0f, 1.5f}, w.param);
        break;
    }
    case Kind::WireStart: // a circle with a dot where it leaves its node
        if (hovered) {
            const Point start = w.b == kNodes
                                    ? outWireOf(w.a, routing_.nodeCenter(w.a), routing_.wireBend(w.param)).start
                                    : wireBetween(w.a, routing_.nodeCenter(w.a), w.b, routing_.nodeCenter(w.b),
                                                  routing_.wireBend(w.param))
                                          .start;
            wireStartHighlight(c, start, nodeColor(w.a));
        }
        break;
    case Kind::AlgorithmButton: {
        const int now = routing_.currentAlgorithm();
        std::string badge;
        if (now >= 0)
            badge = kAlgorithms[now].ops < kOps ? trf(Str::AlgorithmBadgeOps, {std::to_string(kAlgorithms[now].ops),
                                                                                std::to_string(kAlgorithms[now].number)})
                                                : std::to_string(kAlgorithms[now].number);
        paintAlgorithmButton(c, w.rect, badge, hovered, algorithmMenuOpen_);
        break;
    }
    case Kind::AlgorithmMenu:
        if (w.a == 1) { // the panel: its title, the switch's label, and its foot naming the thumbnail under the mouse
            paintMenuPanel(c, w.rect);
            const float x = w.rect.x + kAlgorithmPad, right = w.rect.right() - kAlgorithmPad;
            c.text({x, w.rect.y + kAlgorithmPad, 200.0f, 22.0f}, tr(Str::ButtonAlgorithm), kTextLabel, kDim, Align::Left,
                   true);
            const Rect sets = algorithmSetButton(algorithmSetRange(algorithmOps_).count, 0);
            c.text({sets.x - 128.0f, sets.y, 120.0f, sets.h}, tr(Str::AlgorithmOps), kTextLabel, kDim, Align::Right);
            const float foot = w.rect.bottom() - kAlgorithmFootH + 10.0f;
            c.line({x, foot}, {right, foot}, kBorder, 1.0f);
            const std::optional<Widget> &hover = host_.hovered();
            const bool over = hover && hover->kind == Kind::AlgorithmItem;
            const Rect line{x, foot, right - x - 90.0f, w.rect.bottom() - foot};
            c.text(line, over ? algorithmText(kAlgorithms[hover->a]) : tr(Str::AlgorithmHint), kTextLabel,
                   over ? kText : kDim, Align::Left);
            c.text({right - 90.0f, foot, 90.0f, line.h}, tr(Str::AlgorithmEscape), kTextSmall, kFaint, Align::Right);
        }
        break;
    case Kind::AlgorithmSet: {
        const bool on = algorithmOps_ == w.b;
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, std::to_string(w.b), kTextLabel, on ? litInk(kText) : kDim, Align::Center, on);
        break;
    }
    case Kind::AlgorithmItem: {
        // A cell a little darker than the panel, the algorithm the operators form lit in the accent; its number at
        // the top right, above where the carriers' lines run into OUT, which no operator reaches (at the top left
        // the operators furthest from OUT, and their feedback loops, would run into it).
        const Algorithm &a = kAlgorithms[w.a];
        const bool current = routing_.currentAlgorithm() == w.a;
        const Color dark{0.0f, 0.0f, 0.0f, 1.0f};
        const Color well = lightTheme() ? mix(popupFill(), dark, hovered ? 0.1f : 0.05f)
                                        : (hovered ? look.track : look.display);
        const Color ground = current ? mix(well, kAccent, 0.16f) : well;
        c.fillRect(w.rect, ground, kRadiusControl);
        if (current)
            c.strokeRect(w.rect.inset(0.75f), kAccent, 1.5f, kRadiusControl);
        c.text({w.rect.right() - 36.0f, w.rect.y + 3.0f, 30.0f, 12.0f}, std::to_string(a.number), kTextSmall,
               current ? kAccent : hovered ? kDim : kFaint, Align::Right, true);
        paintAlgorithm(c, {w.rect.x + 3.0f, w.rect.y + 9.0f, w.rect.w - 6.0f, w.rect.h - 12.0f}, a, 4.0f, ground);
        break;
    }
    case Kind::Wire:
        break; // drawn with the graph
    default:
        return false;
    }
    return true;
}

// What the graph's handles and ALGORITHM do, under them; nothing for a node, a label or a cell, nor for ALGORITHM or a
// label's mode while what they open is open.
std::string RoutingPanel::tooltip(const Widget &w) const {
    switch (w.kind) {
    case Kind::AlgorithmButton:
        return algorithmMenuOpen_ ? std::string() : std::string(tr(Str::TipAlgorithm));
    case Kind::WireStart:
        return tr(Str::TipWireStart);
    case Kind::NodeDelete:
        return trf(Str::MenuSwitchOff, {nodeTitle(w.a)});
    case Kind::WireEnd:
        return tr(Str::TipWireEnd);
    case Kind::Port:
        return tr(Str::TipPort);
    case Kind::LabelMode:
        return host_.openList() == w.param ? std::string()
                                           : trf(Str::TipRouteMode, {routeModeName(routeMode(value(w.param)))});
    case Kind::LabelRemove:
        return tr(Str::TipRemoveRoute);
    case Kind::Wire:
        return tr(Str::TipWire);
    case Kind::NodeGhost:
        return trf(Str::TipNodeGhost, {nodeTitle(w.a)});
    default:
        return {};
    }
}

std::optional<CursorKind> RoutingPanel::cursor(const Widget &w) const {
    switch (w.kind) {
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
        return CursorKind::Vertical;
    case Kind::AlgorithmMenu: // the panel itself and what is around it, where a click picks nothing
        return CursorKind::Arrow;
    case Kind::Node:
    case Kind::Wire:
        return CursorKind::Move;
    case Kind::Port:
    case Kind::NodeGhost:
    case Kind::NodeDelete:
    case Kind::WireEnd:
    case Kind::WireStart:
    case Kind::LabelMode:
    case Kind::LabelRemove:
    case Kind::AlgorithmButton:
    case Kind::AlgorithmSet:
    case Kind::AlgorithmItem:
        return CursorKind::Hand;
    default:
        return std::nullopt;
    }
}

// An operator within its circle, a node's edge along its outline, a wire along its curve (within its rect).
std::optional<bool> RoutingPanel::hits(const Widget &w, Point p) const {
    switch (w.kind) {
    case Kind::Node:
        if (isBoxNode(w.a))
            return std::nullopt;
        return std::hypot(p.x - w.rect.center().x, p.y - w.rect.center().y) <= kNodeRadius;
    case Kind::Port:
        return onNodeEdge(w.a, w.rect.center(), p);
    case Kind::Wire: {
        const Wire wire =
            wireBetween(w.a, routing_.nodeCenter(w.a), w.b, routePoint(w.b), routing_.wireBend(w.param));
        return w.rect.contains(p) && distanceToWire(wire, p) <= kWireGrab;
    }
    default:
        return std::nullopt;
    }
}

// --- Input.

void RoutingPanel::startDrag(DragKind kind, const Widget &w, Point p, bool step) {
    drag_ = Drag{};
    drag_.kind = kind;
    drag_.widget = w;
    drag_.start = drag_.now = p;
    host_.startDrag(*this, w, p, step);
}

bool RoutingPanel::press(const Widget &w, Point p, Modifiers mods) {
    UiLayout &layout = controller().layout();
    switch (w.kind) {
    case Kind::WireLabel: // its depth, dragged up and down as a knob's value
    case Kind::OutLabel:
    case Kind::GridCell:
        host_.dragValue(w, p, mods);
        break;
    case Kind::Node:
        layout.selectedNode = w.a;
        controller().beginStep(); // moving it is an undo step
        startDrag(DragKind::Node, w, p, true);
        break;
    case Kind::Port:
        startDrag(DragKind::Connect, w, p, false);
        break;
    case Kind::NodeDelete:
        edit(enabledParam(w.a), 0.0); // switch it off, as its menu does
        host_.resetHover();
        break;
    case Kind::WireEnd:
        startDrag(DragKind::Reroute, w, p, false);
        break;
    case Kind::WireStart:
        startDrag(DragKind::RerouteFrom, w, p, false);
        break;
    case Kind::Wire: {
        const double bend = // as drawn
            fitBend(w.a, routing_.nodeCenter(w.a), w.b, routePoint(w.b), routing_.wireBend(w.param));
        controller().beginStep(); // bending it is an undo step
        startDrag(DragKind::Bend, w, p, true);
        drag_.startValue = bend;
        break;
    }
    case Kind::AlgorithmButton: // open on the set of the algorithm the operators form, the six operators' with none
        if (const int now = routing_.currentAlgorithm(); !algorithmMenuOpen_)
            algorithmOps_ = now >= 0 ? kAlgorithms[now].ops : kOps;
        algorithmMenuOpen_ = !algorithmMenuOpen_;
        break;
    case Kind::AlgorithmMenu:
        if (w.a == 0) // a click anywhere else closes it
            algorithmMenuOpen_ = false;
        break;
    case Kind::AlgorithmSet:
        algorithmOps_ = w.b;
        host_.resetHover();
        break;
    case Kind::AlgorithmItem:
        algorithmMenuOpen_ = false;
        host_.resetHover();
        routing_.applyAlgorithm(w.a);
        break;
    case Kind::NodeGhost: // into the graph where it was, and into the inspector (the graph shows no partial editor)
        edit(enabledParam(w.a), 1.0);
        layout.selectedNode = w.a;
        break;
    case Kind::LabelMode: // its list of modes, or closed again
        host_.showList(host_.openList() == w.param ? -1 : w.param);
        break;
    case Kind::LabelRemove:
        host_.showList(-1);
        routing_.disconnect(w.param);
        host_.resetHover();
        break;
    default:
        return false;
    }
    return true;
}

bool RoutingPanel::doubleClick(const Widget &w, Point, Modifiers) {
    switch (w.kind) {
    case Kind::Wire: { // back to how it bows to begin with (a line to OUT straight)
        const UndoScope step(controller());
        routing_.setWireBend(w.param, RoutingModel::defaultWireBend(w.param));
        return true;
    }
    case Kind::OutLabel: // like a wire label: shown, so wired, so off
        routing_.disconnect(w.param);
        return true;
    case Kind::GridCell:
    case Kind::WireLabel: {
        // Toggle: off, or on at a typical amount for that kind of route.
        const int to = w.b;
        const bool full = to >= kOps && w.a != to; // into a filter, a shaper or the output; modulation starts lower
        const double on = full ? 1.0 : (w.a == to ? 0.25 : 0.3);
        if (routing_.wired(w.param)) {
            routing_.disconnect(w.param);
        } else {
            const UndoScope step(controller());
            if (w.kind == Kind::GridCell) // a route in the matrix switches on what it joins
                routing_.switchOnRoute(w.a, to);
            edit(w.param, on);
        }
        return true;
    }
    default:
        return false;
    }
}

void RoutingPanel::drag(const DragState &d) {
    drag_.now = d.now;
    drag_.moved = d.moved;
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::Connect:
    case DragKind::Reroute:
        if (!drag_.leftNode)
            drag_.leftNode = clearOfNode(w.a, routing_.nodeCenter(w.a), d.now);
        break;
    case DragKind::Node: {
        UiLayout &layout = controller().layout();
        // Kept off the other nodes; Ctrl held, onto the grid's nearest point it fits at.
        const Point at = d.mods.ctrl ? gridPointFor(w.a, d.now) : routing_.nodePlace(w.a, d.now);
        const float x = std::clamp((at.x - kGraph.x) / kGraph.w, 0.0f, 1.0f);
        const float y = std::clamp((at.y - kGraph.y) / kGraph.h, 0.0f, 1.0f);
        if (isBoxNode(w.a)) {
            layout.boxX[w.a - kOps] = x;
            layout.boxY[w.a - kOps] = y;
        } else {
            layout.nodeX[w.a] = x;
            layout.nodeY[w.a] = y;
        }
        break;
    }
    case DragKind::Bend: {
        // The wire's middle follows the mouse sideways, from where it was grabbed: it lies half as far out as the
        // curve's control point.
        const Point a = routing_.nodeCenter(w.a), b = routePoint(w.b);
        const float span = std::max(std::hypot(b.x - a.x, b.y - a.y), 1.0f);
        const Point left = leftOf(a, b);
        const float moved = (d.now.x - drag_.start.x) * left.x + (d.now.y - drag_.start.y) * left.y;
        // It stops where it would leave the panel.
        const float bend = fitBend(w.a, a, w.b, b,
                                   std::clamp(static_cast<float>(drag_.startValue) + 2.0f * moved / span,
                                              -kMaxWireBend, kMaxWireBend));
        if (bend != routing_.wireBend(w.param))
            routing_.setWireBend(w.param, bend);
        break;
    }
    case DragKind::RerouteFrom:
    case DragKind::None:
        break;
    }
}

void RoutingPanel::drop(const DragState &d) {
    drag_.now = d.now;
    drag_.moved = d.moved;
    drag_.dropped = true;
    const Widget &w = drag_.widget;
    const Point p = d.now;
    switch (drag_.kind) {
    case DragKind::Node:
        if (d.moved)
            controller().layoutChanged();
        break;
    case DragKind::Connect: {
        if (!d.moved) { // a click on the edge picks the node, like one further inside
            controller().layout().selectedNode = w.a;
            break;
        }
        const int to = nodeAt(p);
        if (to >= 0) {
            routing_.connect(w.a, to);
        } else if (onOutCore(p) && value(outParam(w.a)) <= 0.0) {
            edit(outParam(w.a), 1.0);
        }
        break;
    }
    case DragKind::RerouteFrom: {
        if (!d.moved) // a click leaves it be
            break;
        if (const int source = nodeAt(p); source >= 0)
            routing_.rerouteFrom(w.a, w.b, source);
        else // let go of off the nodes, the route goes, with its modulation
            routing_.disconnect(w.param);
        break;
    }
    case DragKind::Reroute: {
        if (!d.moved) // a click leaves it be
            break;
        const int into = nodeAt(p) >= 0 ? nodeAt(p) : (onOutCore(p) ? kNodes : -1);
        if (into >= 0)
            routing_.reroute(w.a, w.b, into);
        else // let go of off the nodes and the bus, the route goes, with its modulation
            routing_.disconnect(w.param);
        break;
    }
    case DragKind::Bend: // each move set it already
    case DragKind::None:
        break;
    }
}

// Called off, a node moved stays where it was taken, as an undo step; a route dragged goes nowhere.
void RoutingPanel::release() {
    if (!drag_.dropped && drag_.kind == DragKind::Node && drag_.moved)
        controller().layoutChanged();
    drag_ = Drag{};
}

// A matrix cell dragged switches on what it joins, in the same step.
void RoutingPanel::draggingValue(const Widget &w) {
    if (w.kind == Kind::GridCell)
        routing_.switchOnRoute(w.a, w.b);
}

bool RoutingPanel::dragsOnTouch(const Widget &w) const {
    switch (w.kind) {
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
    case Kind::Node:
    case Kind::Port:
    case Kind::WireEnd:
    case Kind::WireStart:
    case Kind::Wire:
        return true;
    default:
        return false;
    }
}

// A label's mode: to the next mode along, as the wheel turns, a run of notches one undo step.
bool RoutingPanel::wheelOn(const Widget &w, Point, float steps, Modifiers) {
    if (w.kind != Kind::LabelMode)
        return false;
    const int whole = host_.wheelNotches(w.param, steps);
    const int to =
        std::clamp(static_cast<int>(routeMode(value(w.param))) + whole, 0, static_cast<int>(RouteMode::Count) - 1);
    if (whole != 0 && to != static_cast<int>(routeMode(value(w.param)))) {
        const UndoScope notches(controller(), true);
        edit(w.param, to);
    }
    return true;
}

bool RoutingPanel::blocksPan(const Widget &w) const {
    return w.kind == Kind::AlgorithmMenu && w.a != 0;
}

namespace {
// A route into an operator lists its modes in a menu, the one in force checked.
void addRouteModes(std::vector<MenuItem> &items, int route, double mode) {
    if (routeModeOfRoute(route) < 0)
        return;
    const auto now = routeMode(mode);
    for (int m = 0; m < static_cast<int>(RouteMode::Count); ++m)
        items.push_back({RoutingPanel::kRouteModeCommand + m, routeModeName(static_cast<RouteMode>(m)),
                         static_cast<int>(now) == m});
    items.push_back({0, ""});
}

} // namespace

// A node's (or its edge's): Switch Off. A label's mode or x, an arrowhead or a wire: the route's modes, Reset Bend for
// a wire bent, and Disconnect.
bool RoutingPanel::contextMenu(const std::optional<Widget> &hit, Point, std::vector<MenuItem> &items) {
    menuRoute_ = menuNode_ = -1;
    if (!hit)
        return false;
    auto modeOf = [&](int route) {
        const int mode = routeModeOfRoute(route);
        return mode >= 0 ? value(mode) : 0.0;
    };
    switch (hit->kind) {
    case Kind::Node: // the node, or its edge
    case Kind::Port:
        menuNode_ = hit->a;
        items.push_back({kMenuSwitchOff, trf(Str::MenuSwitchOff, {nodeTitle(hit->a)})});
        return true;
    case Kind::LabelMode:
    case Kind::LabelRemove:
        menuRoute_ = hit->kind == Kind::LabelMode ? routeParam(hit->a, hit->b) : hit->param;
        addRouteModes(items, menuRoute_, modeOf(menuRoute_));
        items.push_back({kMenuDisconnect, tr(Str::MenuDisconnect)});
        return true;
    case Kind::WireEnd:
    case Kind::Wire:
        menuRoute_ = hit->param;
        addRouteModes(items, hit->param, modeOf(hit->param));
        if (hit->kind == Kind::Wire && routing_.wireBend(hit->param) != RoutingModel::defaultWireBend(hit->param))
            items.push_back({kMenuResetBend, tr(Str::MenuResetBend)});
        items.push_back({kMenuDisconnect, tr(Str::MenuDisconnect)});
        return true;
    default:
        return false;
    }
}

// A route's label or matrix cell: after Reset to Default, the route's modes and Disconnect, while it is wired.
void RoutingPanel::paramMenu(const Widget &w, std::vector<MenuItem> &items) {
    if (w.kind != Kind::WireLabel && w.kind != Kind::OutLabel && w.kind != Kind::GridCell)
        return;
    menuRoute_ = w.param;
    if (!routing_.wired(w.param))
        return;
    if (w.kind != Kind::OutLabel && routeModeOfRoute(w.param) >= 0) {
        items.push_back({0, ""});
        addRouteModes(items, w.param, value(routeModeOfRoute(w.param)));
        items.pop_back(); // Disconnect follows the modes without a line between
    }
    items.push_back({kMenuDisconnect, tr(Str::MenuDisconnect)});
}

bool RoutingPanel::menuCommand(int command) {
    if (command == kMenuDisconnect && menuRoute_ >= 0) {
        routing_.disconnect(menuRoute_);
    } else if (command == kMenuSwitchOff && menuNode_ >= 0) {
        edit(enabledParam(menuNode_), 0.0); // switch a node off
    } else if (command >= kRouteModeCommand && command < kRouteModeCommand + static_cast<int>(RouteMode::Count) &&
               menuRoute_ >= 0 && routeModeOfRoute(menuRoute_) >= 0) {
        edit(routeModeOfRoute(menuRoute_), command - kRouteModeCommand);
    } else if (command == kMenuResetBend && menuRoute_ >= 0) {
        const UndoScope step(controller());
        routing_.setWireBend(menuRoute_, RoutingModel::defaultWireBend(menuRoute_)); // a line to OUT straight
    } else {
        return false;
    }
    return true;
}

// The panel of algorithms: Escape closes it, Enter goes on to the host.
std::optional<bool> RoutingPanel::dialogKey(Key key) {
    if (!algorithmMenuOpen_)
        return std::nullopt;
    if (key != Key::Escape)
        return false;
    algorithmMenuOpen_ = false;
    host_.resetHover();
    return true;
}

} // namespace fmvibe::gui
