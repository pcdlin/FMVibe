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

inline Point loopCenter(Point node) {
    return {node.x, node.y - kNodeRadius - 9.0f};
}

// The param that sets how much `from` feeds `to`, or -1 if they can't connect. Every node modulates operators and
// feeds filters and shapers, but a filter only feeds later filters, and a shaper no shaper.
inline int routeParam(int from, int to) {
    if (from < kOps)
        return to < kOps            ? matrixIndex(from, to)
               : isFilterNode(to) ? sendIndex(from, to - kOps)
                                  : shaperSendIndex(from, shaperOfNode(to));
    if (isShaperNode(from))
        return to < kOps            ? shaperModIndex(shaperOfNode(from), to)
               : isFilterNode(to) ? shaperFilterIndex(shaperOfNode(from), to - kOps)
                                  : -1;
    return to < kOps            ? filterModIndex(from - kOps, to)
           : isFilterNode(to) ? filterLinkIndex(from - kOps, to - kOps) // only into later filters
                              : filterShaperIndex(from - kOps, shaperOfNode(to));
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

// Why `from` can't go into a node: only a filter into itself or an earlier filter, or a shaper into a shaper, can't.
inline const char *routeRefusal(int from) {
    return tr(isShaperNode(from) ? Str::MsgRefuseShaper : Str::MsgRefuseFilter);
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

// Matrix cell: rows are nodes, columns the operators, the filters, then the output.
inline int gridParam(int row, int col) {
    if (col == kGridCols - 1)
        return outParam(row);
    return row == col ? (row < kOps ? matrixIndex(row, row) : -1) : routeParam(row, col);
}

// A wire bows to one side, so A -> B and B -> A don't overlap.
struct Wire {
    Point start;
    Point control;
    Point end; // base of the arrowhead
    Point label;
    Point tangent;
};

inline Wire wireBetween(int fromNode, Point a, int toNode, Point b) {
    const Point d{b.x - a.x, b.y - a.y};
    const Point u = normalize(d);
    const float bend = std::hypot(d.x, d.y) * 0.18f;
    Wire w;
    w.control = {(a.x + b.x) * 0.5f - u.y * bend, (a.y + b.y) * 0.5f + u.x * bend};
    const Point fromA = normalize({w.control.x - a.x, w.control.y - a.y});
    const Point fromB = normalize({w.control.x - b.x, w.control.y - b.y});
    const float startOffset = nodeExtent(fromNode, fromA);
    const float endOffset = nodeExtent(toNode, fromB) + 8.0f;
    w.start = {a.x + fromA.x * startOffset, a.y + fromA.y * startOffset};
    w.end = {b.x + fromB.x * endOffset, b.y + fromB.y * endOffset};
    w.label = {0.25f * w.start.x + 0.5f * w.control.x + 0.25f * w.end.x,
               0.25f * w.start.y + 0.5f * w.control.y + 0.25f * w.end.y};
    w.tangent = normalize({w.end.x - w.control.x, w.end.y - w.control.y});
    return w;
}

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
