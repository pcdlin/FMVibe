#pragma once

// The routing graph's model: which nodes there are, which param joins two of them or sends one out, and where
// their wires and arrowheads go. Shared by the editor's own files only (see view.cpp), in namespace detail;
// routing.cpp holds the View's methods that connect and reroute.

#include "layout.h"
#include "theme.h"
#include "view.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace fmvibe::gui::detail {

// --- Routing graph: operators are circles, filters and shapers boxes.

const char *const kNodeNames[kNodes] = {"A", "B", "C", "D", "E", "F", "F1", "F2", "S1", "S2"};

inline bool isBoxNode(int node) {
    return node >= kOps;
}

inline bool isFilterNode(int node) {
    return node >= kOps && node < kOps + kFilters;
}

inline bool isShaperNode(int node) {
    return node >= kOps + kFilters;
}

inline int shaperOfNode(int node) {
    return node - kOps - kFilters;
}

// Operators in the accent color, filters and shapers each in their own.
inline Color nodeColor(int node) {
    return isShaperNode(node) ? kShaperColor : isFilterNode(node) ? kFilterColor : kAccent;
}

// A route takes the color of the shaper or filter it joins, if any.
inline Color routeColor(int from, int to) {
    return isShaperNode(from) || isShaperNode(to)   ? kShaperColor
           : isFilterNode(from) || isFilterNode(to) ? kFilterColor
                                                    : kAccent;
}

// Distance from a node's center to its outline in direction `dir`.
inline float nodeExtent(int node, Point dir) {
    if (!isBoxNode(node))
        return kNodeRadius;
    const float tx = std::abs(dir.x) > 1e-4f ? kFilterHalfW / std::abs(dir.x) : 1e9f;
    const float ty = std::abs(dir.y) > 1e-4f ? kFilterHalfH / std::abs(dir.y) : 1e9f;
    return std::min(tx, ty);
}

// A node's edge, a band along its outline from kEdgeIn inside it to kEdgeOut outside: a drag from there connects the
// node to another, while one from further inside moves it.
constexpr float kEdgeIn = 6.0f, kEdgeOut = 5.0f;
inline bool onNodeEdge(int node, Point center, Point p) {
    if (isBoxNode(node)) {
        const float dx = std::abs(p.x - center.x), dy = std::abs(p.y - center.y);
        return dx <= kFilterHalfW + kEdgeOut && dy <= kFilterHalfH + kEdgeOut &&
               (dx >= kFilterHalfW - kEdgeIn || dy >= kFilterHalfH - kEdgeIn);
    }
    const float d = std::hypot(p.x - center.x, p.y - center.y);
    return d >= kNodeRadius - kEdgeIn && d <= kNodeRadius + kEdgeOut;
}

// Where a route leaving `node` toward `to` crosses its outline.
inline Point outlineToward(int node, Point center, Point to) {
    const Point dir = normalize({to.x - center.x, to.y - center.y});
    const float extent = nodeExtent(node, dir);
    return {center.x + dir.x * extent, center.y + dir.y * extent};
}

// Where a node's loop into itself is drawn, a circle on its top edge: an operator's feedback, a shaper's.
inline Point loopCenter(int node, Point center) {
    return {center.x, center.y - (isBoxNode(node) ? kFilterHalfH : kNodeRadius) - 9.0f};
}

// The param that sets how much `from` feeds `to`: every node modulates operators and feeds filters and shapers,
// itself too (feedback).
inline int routeParam(int from, int to) {
    if (from < kOps)
        return to < kOps            ? matrixIndex(from, to)
               : isFilterNode(to) ? sendIndex(from, to - kOps)
                                  : shaperSendIndex(from, shaperOfNode(to));
    if (isShaperNode(from))
        return to < kOps            ? shaperModIndex(shaperOfNode(from), to)
               : isFilterNode(to) ? shaperFilterIndex(shaperOfNode(from), to - kOps)
                                  : shaperShaperIndex(shaperOfNode(from), shaperOfNode(to));
    return to < kOps            ? filterModIndex(from - kOps, to)
           : isFilterNode(to) ? filterLinkIndex(from - kOps, to - kOps)
                              : filterShaperIndex(from - kOps, shaperOfNode(to));
}

// The param that sets how the route from `from` into `to` acts on it (see RouteMode): only routes into an operator
// have one, its feedback aside, which is always Phase. -1 for the others.
inline int routeModeParam(int from, int to) {
    return from >= 0 && from < kNodes && to >= 0 && to < kOps && from != to ? routeModeIndex(from, to) : -1;
}

// What a node puts straight out to the output.
inline int outParam(int node) {
    return isShaperNode(node)   ? paramIndex(shaperOfNode(node), ShaperParam::Out)
           : isFilterNode(node) ? paramIndex(node - kOps, FilterParam::Out)
                                : paramIndex(node, OpParam::Out);
}

// The route from `from` into `to`, kNodes standing for the output.
inline int routeTo(int from, int to) {
    return to == kNodes ? outParam(from) : routeParam(from, to);
}

// Whether a port or arrowhead let go at `p` lands on the OUT bus.
inline bool onBus(Point p) {
    return std::abs(p.y - kBusY) < 16.0f && p.x > kRouting.x && p.x < kRouting.right();
}

// Whether `param` is a route of the graph: one node into another, or a node into the output.
inline bool isRouteParam(int param) {
    for (int from = 0; from < kNodes; ++from) {
        if (outParam(from) == param)
            return true;
        for (int to = 0; to < kNodes; ++to)
            if (routeParam(from, to) == param)
                return true;
    }
    return false;
}

// The mode param of a route's amount param, or -1 for one without (see routeModeParam()).
inline int routeModeOfRoute(int param) {
    for (int from = 0; from < kNodes; ++from)
        for (int to = 0; to < kOps; ++to)
            if (routeParam(from, to) == param)
                return routeModeParam(from, to);
    return -1;
}

inline int enabledParam(int node) {
    return isShaperNode(node)   ? paramIndex(shaperOfNode(node), ShaperParam::Enabled)
           : isFilterNode(node) ? paramIndex(node - kOps, FilterParam::Enabled)
                                : paramIndex(node, OpParam::Enabled);
}

inline bool isPowerParam(int param) {
    for (int node = 0; node < kNodes; ++node)
        if (enabledParam(node) == param)
            return true;
    return false;
}

inline std::string nodeTitle(int node) {
    return isShaperNode(node)   ? trf(Str::NodeShaper, {std::to_string(shaperOfNode(node) + 1)})
           : isFilterNode(node) ? trf(Str::NodeFilter, {std::to_string(node - kOps + 1)})
                                : trf(Str::NodeOperator, {kNodeNames[node]});
}

constexpr float kHintX = kRouting.x + 330.0f; // the routing graph's hint, in its title row

// Matrix cell: rows are nodes, columns the operators, the filters, the shapers, then the output. The diagonal is
// each node's feedback.
inline int gridParam(int row, int col) {
    return col == kGridCols - 1 ? outParam(row) : routeParam(row, col);
}

// A wire bows to its left by `bend` of the distance it spans (kDefaultWireBend until it is dragged), so A -> B and
// B -> A don't overlap; dragged, it bows either way, or runs straight. Near the panel's edge it bows only as far as
// stays inside (see fitBend()).
struct Wire {
    Point start;
    Point control;
    Point end; // base of the arrowhead
    Point label;
    Point tangent;
};

// Which way is left of the line from `a` to `b`, as a unit vector.
inline Point leftOf(Point a, Point b) {
    const Point u = normalize({b.x - a.x, b.y - a.y});
    return {-u.y, u.x};
}

// A wire between nodes whose outlines lie `extentA(dir)` and `extentB(dir)` from their centers in direction `dir`:
// the routing graph's nodes (see bentWire()), or the effects graph's.
template <class ExtentA, class ExtentB>
inline Wire bentWireBy(Point a, Point b, float bend, ExtentA extentA, ExtentB extentB) {
    const Point d{b.x - a.x, b.y - a.y};
    const Point left = leftOf(a, b);
    const float reach = std::hypot(d.x, d.y) * bend;
    Wire w;
    w.control = {(a.x + b.x) * 0.5f + left.x * reach, (a.y + b.y) * 0.5f + left.y * reach};
    const Point fromA = normalize({w.control.x - a.x, w.control.y - a.y});
    const Point fromB = normalize({w.control.x - b.x, w.control.y - b.y});
    const float startOffset = extentA(fromA);
    const float endOffset = extentB(fromB) + 8.0f;
    w.start = {a.x + fromA.x * startOffset, a.y + fromA.y * startOffset};
    w.end = {b.x + fromB.x * endOffset, b.y + fromB.y * endOffset};
    w.label = {0.25f * w.start.x + 0.5f * w.control.x + 0.25f * w.end.x,
               0.25f * w.start.y + 0.5f * w.control.y + 0.25f * w.end.y};
    w.tangent = normalize({w.end.x - w.control.x, w.end.y - w.control.y});
    return w;
}

inline Wire bentWire(int fromNode, Point a, int toNode, Point b, float bend) {
    return bentWireBy(a, b, bend, [fromNode](Point dir) { return nodeExtent(fromNode, dir); },
                      [toNode](Point dir) { return nodeExtent(toNode, dir); });
}

// Whether a wire stays inside the routing panel, a little in from its edge, its label too. A quadratic curve goes
// furthest along each axis at its ends or where its slope along that axis is 0.
inline bool wireInside(const Wire &w) {
    const Rect frame = kRouting.inset(6.0f);
    if (w.label.x < kRouting.x + 26.0f || w.label.x > kRouting.right() - 26.0f || w.label.y < kRouting.y + 12.0f ||
        w.label.y > kRouting.bottom() - 12.0f) // half a label chip in from the edge
        return false;
    auto within = [](float p0, float p1, float p2, float lo, float hi) {
        float low = std::min(p0, p2), high = std::max(p0, p2);
        if (const float d = p0 - 2.0f * p1 + p2; std::abs(d) > 1e-6f) {
            const float t = (p0 - p1) / d;
            if (t > 0.0f && t < 1.0f) {
                const float at = (1.0f - t) * (1.0f - t) * p0 + 2.0f * (1.0f - t) * t * p1 + t * t * p2;
                low = std::min(low, at);
                high = std::max(high, at);
            }
        }
        return low >= lo && high <= hi;
    };
    return within(w.start.x, w.control.x, w.end.x, frame.x, frame.right()) &&
           within(w.start.y, w.control.y, w.end.y, frame.y, frame.bottom());
}

// The bend a wire is drawn with: `bend`, or as far that way as keeps it and its label inside the panel, so a wire
// along the panel's edge bows less rather than spill out of it.
inline float fitBend(int fromNode, Point a, int toNode, Point b, float bend) {
    if (wireInside(bentWire(fromNode, a, toNode, b, bend)))
        return bend;
    float in = 0.0f, out = bend;
    for (int i = 0; i < 12; ++i) {
        const float mid = 0.5f * (in + out);
        (wireInside(bentWire(fromNode, a, toNode, b, mid)) ? in : out) = mid;
    }
    return in;
}

inline Wire wireBetween(int fromNode, Point a, int toNode, Point b, float bend) {
    return bentWire(fromNode, a, toNode, b, fitBend(fromNode, a, toNode, b, bend));
}

// A route held over the node it comes from, while it is dragged: most of a small circle standing on the node's top,
// out of the ring around it on the left, round over the top and back into the ring on the right, so it reads as the
// loop letting go there makes (its feedback). The ring is where the node's selection ring goes, and where a route held
// over a node rings it. Its angles run clockwise on screen (y down) from `from` to `to`, where it meets the ring.
struct LoopArc {
    Point center;
    float radius, from, to;
    Point at(float angle) const { return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)}; }
};
inline LoopArc loopbackArc(int node, Point center) {
    constexpr float r = 14.0f, pi = 3.14159265f, ring = kSelectedRing;
    float d, below; // how far the loop's center is above the node's, and how far below its own it meets the outline
    if (isBoxNode(node)) {
        below = 0.6f * r;
        d = kFilterHalfH + ring + below; // across the box's straight top edge
    } else {
        const float outline = kNodeRadius + ring;
        d = outline + 0.5f * r;
        below = (d * d + r * r - outline * outline) / (2.0f * d); // where the two circles cross
    }
    const float beta = std::asin(std::clamp(below / r, -1.0f, 1.0f));
    return {{center.x, center.y - d}, r, pi - beta, 2.0f * pi + beta};
}

// Whether `p` is well clear of a node's outline: a route dragged from it shows its loop back into it only once the
// mouse has been out here, so it doesn't flash as the drag leaves its edge.
inline bool clearOfNode(int node, Point center, Point p) {
    const Point dir = normalize({p.x - center.x, p.y - center.y});
    return std::hypot(p.x - center.x, p.y - center.y) - nodeExtent(node, dir) > 16.0f;
}

// Whether `r`, a label's chip, comes within `gap` of a node's outline, the node centered at `center`.
inline bool overNode(int node, Point center, Rect r, float gap) {
    if (isBoxNode(node))
        return r.x < center.x + kFilterHalfW + gap && r.right() > center.x - kFilterHalfW - gap &&
               r.y < center.y + kFilterHalfH + gap && r.bottom() > center.y - kFilterHalfH - gap;
    const float x = std::clamp(center.x, r.x, r.right()), y = std::clamp(center.y, r.y, r.bottom());
    return std::hypot(center.x - x, center.y - y) < kNodeRadius + gap;
}

// How far `p` is from a wire, along its curve between the nodes (its arrowhead's base included).
inline float distanceToWire(const Wire &w, Point p) {
    constexpr int kSteps = 24;
    float best = 1e9f;
    Point last = w.start;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / kSteps, s = 1.0f - t;
        const Point now{s * s * w.start.x + 2.0f * s * t * w.control.x + t * t * w.end.x,
                        s * s * w.start.y + 2.0f * s * t * w.control.y + t * t * w.end.y};
        const Point seg{now.x - last.x, now.y - last.y};
        const float length2 = seg.x * seg.x + seg.y * seg.y;
        const float along =
            length2 > 1e-6f ? std::clamp(((p.x - last.x) * seg.x + (p.y - last.y) * seg.y) / length2, 0.0f, 1.0f)
                            : 0.0f;
        best = std::min(best, std::hypot(p.x - (last.x + along * seg.x), p.y - (last.y + along * seg.y)));
        last = now;
    }
    return best;
}

// How close to a wire the mouse grabs it, to bend it.
constexpr float kWireGrab = 5.0f;

inline void arrowHead(Canvas &c, Point base, Point dir, Color color) {
    const Point side{-dir.y * 4.5f, dir.x * 4.5f};
    const Point points[] = {{base.x + dir.x * 8.0f, base.y + dir.y * 8.0f},
                            {base.x + side.x, base.y + side.y},
                            {base.x - side.x, base.y - side.y}};
    c.fillPolygon(points, 3, color);
}

// What can be grabbed of an arrowhead with its base at `base`, pointing along `dir`.
inline Rect arrowGrip(Point base, Point dir) {
    const Point middle{base.x + dir.x * 4.0f, base.y + dir.y * 4.0f};
    return {middle.x - 9.0f, middle.y - 9.0f, 18.0f, 18.0f};
}

// The grips that drag a wire by its ends, reaching back along it from where it meets its nodes: its arrowhead (from
// the base at `base`, pointing along `dir`), and its start (at `start`, leaving along `dir`).
inline Rect wireEndGrip(Point base, Point dir) {
    const Point middle{base.x - dir.x * 2.0f, base.y - dir.y * 2.0f};
    return {middle.x - 13.0f, middle.y - 13.0f, 26.0f, 26.0f};
}
inline Rect wireStartGrip(Point start, Point dir) {
    const Point middle{start.x + dir.x * 9.0f, start.y + dir.y * 9.0f};
    return {middle.x - 12.0f, middle.y - 12.0f, 24.0f, 24.0f};
}

// A wire's end under the mouse, which drags: a faint circle around its arrowhead (from the base at `base`, pointing
// along `dir`), or around its start with a dot on it.
inline void wireEndHighlight(Canvas &c, Point base, Point dir, Color color) {
    const Point middle{base.x + dir.x * 4.0f, base.y + dir.y * 4.0f};
    c.fillCircle(middle, 9.0f, color.alpha(0.16f));
    c.strokeCircle(middle, 9.0f, color.alpha(0.5f), 1.2f);
}
inline void wireStartHighlight(Canvas &c, Point start, Color color) {
    c.fillCircle(start, 8.0f, color.alpha(0.16f));
    c.strokeCircle(start, 8.0f, color.alpha(0.5f), 1.2f);
    c.fillCircle(start, 3.0f, color);
}

// A node's line down to the OUT bus: from its outline to the base of the arrowhead, whose tip touches the bus, with
// its label halfway along. A node too close to the bus for the label to fit in between has it beside the line, on
// the left.
struct OutWire {
    Point start;
    Point end; // base of the arrowhead
    Point label;
};

inline OutWire outWireOf(int node, Point center) {
    const float top = center.y + (isBoxNode(node) ? kFilterHalfH : kNodeRadius);
    const float end = kBusY - 10.0f;
    OutWire w{{center.x, top}, {center.x, end}, {center.x, 0.5f * (top + end)}};
    if (end - top < 18.0f)
        w.label.x -= 28.0f;
    return w;
}

} // namespace fmvibe::gui::detail
