#pragma once

// Laying a patch's routing graph out: where its nodes go and how its wires bow, so that no node sits on another and
// no wire, line to OUT or label runs over a node it doesn't join. fmvibe-presettool --arrange lays the factory presets
// out with it, and the UI checks keep them clear by its count of clashes. In namespace detail, as routing.h, whose
// geometry (the wires' curves, the lines to OUT, the loops) it measures with.

#include "routing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fmvibe::gui::detail {

// The routes of a patch's graph between nodes that are on (kNodes standing for OUT), its loops among them.
struct ArrangeGraph {
    struct Route {
        int from, to, param;
    };
    std::vector<int> nodes;
    std::vector<Route> routes;
};

inline ArrangeGraph arrangeGraph(const ParamValues &values, const UiLayout &layout) {
    ArrangeGraph graph;
    auto on = [&](int node) { return values[enabledParam(node)] >= 0.5; };
    auto wired = [&](int param) {
        return values[param] > 0.0 ||
               std::find(layout.keptRoutes.begin(), layout.keptRoutes.end(), param) != layout.keptRoutes.end();
    };
    for (int node = 0; node < kNodes; ++node)
        if (on(node))
            graph.nodes.push_back(node);
    for (int from : graph.nodes) {
        for (int to : graph.nodes)
            if (wired(routeParam(from, to)))
                graph.routes.push_back({from, to, routeParam(from, to)});
        if (wired(outParam(from)))
            graph.routes.push_back({from, kNodes, outParam(from)});
    }
    return graph;
}

inline Point arrangeCenter(const UiLayout &layout, int node) {
    if (node == kNodes)
        return kOutCore;
    if (isBoxNode(node))
        return {kGraph.x + layout.boxX[node - kOps] * kGraph.w, kGraph.y + layout.boxY[node - kOps] * kGraph.h};
    return {kGraph.x + layout.nodeX[node] * kGraph.w, kGraph.y + layout.nodeY[node] * kGraph.h};
}

inline void arrangePlace(UiLayout &layout, int node, float x, float y) {
    if (isBoxNode(node)) {
        layout.boxX[node - kOps] = x;
        layout.boxY[node - kOps] = y;
    } else {
        layout.nodeX[node] = x;
        layout.nodeY[node] = y;
    }
}

inline float arrangeBend(const UiLayout &layout, const ArrangeGraph::Route &route) {
    for (const auto &[param, bend] : layout.wireBends)
        if (param == route.param)
            return bend;
    return route.to == kNodes ? 0.0f : kDefaultWireBend;
}

inline void arrangeSetBend(UiLayout &layout, const ArrangeGraph::Route &route, float bend) {
    std::erase_if(layout.wireBends, [&](const auto &b) { return b.first == route.param; });
    if (bend != (route.to == kNodes ? 0.0f : kDefaultWireBend))
        layout.wireBends.push_back({route.param, bend});
}

// How far `p` is outside a node's outline (below 0 inside it).
inline float arrangeOutside(int node, Point center, Point p) {
    if (!isBoxNode(node))
        return std::hypot(p.x - center.x, p.y - center.y) - kNodeRadius;
    const float dx = std::abs(p.x - center.x) - kFilterHalfW, dy = std::abs(p.y - center.y) - kFilterHalfH;
    if (dx <= 0.0f && dy <= 0.0f)
        return std::max(dx, dy);
    return std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
}

// How far apart two nodes' outlines are (below 0 where they overlap).
inline float arrangeGap(int a, Point ca, int b, Point cb) {
    if (!isBoxNode(a) && !isBoxNode(b))
        return std::hypot(ca.x - cb.x, ca.y - cb.y) - 2.0f * kNodeRadius;
    if (!isBoxNode(a))
        return arrangeOutside(b, cb, ca) - kNodeRadius;
    if (!isBoxNode(b))
        return arrangeOutside(a, ca, cb) - kNodeRadius;
    const float dx = std::abs(ca.x - cb.x) - 2.0f * kFilterHalfW, dy = std::abs(ca.y - cb.y) - 2.0f * kFilterHalfH;
    if (dx <= 0.0f && dy <= 0.0f)
        return std::max(dx, dy);
    return std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
}

// How a layout fares: its clashes (nodes on each other, wires or lines to OUT through a node they don't join), and a
// cost that also weighs wires crossing, running backward and their lengths, by which layouts are compared.
struct ArrangeScore {
    int clashes = 0;
    float cost = 0.0f;
};

inline ArrangeScore arrangeScore(const ArrangeGraph &graph, const UiLayout &layout) {
    ArrangeScore score;
    constexpr float kNodeRoom = 26.0f; // between two nodes' outlines, for their labels and wires
    constexpr float kWireRoom = 10.0f; // between a wire and a node it doesn't join
    constexpr int kSamples = 16;
    std::array<Point, kNodes + 1> centers{};
    for (int node : graph.nodes)
        centers[node] = arrangeCenter(layout, node);
    centers[kNodes] = kOutCore;
    std::array<bool, kNodes> looped{};
    for (const auto &r : graph.routes)
        if (r.from == r.to)
            looped[r.from] = true;
    // The ghosts of the nodes that are off, docked in the graph's bottom right, under OUT (see View's widgets).
    float dockW = 0.0f;
    for (int node = 0; node < kNodes; ++node)
        if (std::find(graph.nodes.begin(), graph.nodes.end(), node) == graph.nodes.end())
            dockW += (isBoxNode(node) ? 34.0f : 24.0f) + 6.0f;
    const Rect dock{kRouting.right() - 16.0f - dockW - 6.0f, kRouting.bottom() - 40.0f, dockW + 6.0f, 32.0f};
    auto overDock = [&](int node, Point c) {
        const float half = isBoxNode(node) ? kFilterHalfH : kNodeRadius;
        const float halfW = isBoxNode(node) ? kFilterHalfW : kNodeRadius;
        return c.y + half > dock.y && c.x + halfW > dock.x;
    };
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const int a = graph.nodes[i];
        if (overDock(a, centers[a]))
            score.cost += 400.0f;
        const float core = std::hypot(centers[a].x - kOutCore.x, centers[a].y - kOutCore.y) -
                           (isBoxNode(a) ? kFilterHalfW : kNodeRadius) - kOutCoreRadius;
        if (core < kNodeRoom)
            score.cost += 1000.0f * (kNodeRoom - core) / kNodeRoom;
        for (size_t j = i + 1; j < graph.nodes.size(); ++j) {
            const int b = graph.nodes[j];
            float gap = arrangeGap(a, centers[a], b, centers[b]);
            // A loop over a node stands above it, and its label beside it.
            if (looped[a] || looped[b])
                gap = std::min(gap, std::abs(centers[a].x - centers[b].x) < 60.0f && centers[b].y < centers[a].y
                                        ? gap - 22.0f
                                        : gap);
            if (gap < kNodeRoom)
                score.cost += 1000.0f * (kNodeRoom - gap) / kNodeRoom;
            if (gap < 2.0f * kNodeRoom) // and a little more, where there is room for it
                score.cost += 8.0f * (2.0f * kNodeRoom - gap);
            if (gap < 10.0f)
                ++score.clashes;
        }
    }
    struct Path {
        int from, to;
        std::array<Point, kSamples + 1> points;
        Rect label;
    };
    std::vector<Path> paths;
    for (const auto &r : graph.routes) {
        if (r.from == r.to)
            continue;
        Point start, control, end, label;
        if (r.to == kNodes) {
            const OutWire w = outWireOf(r.from, centers[r.from], arrangeBend(layout, r));
            start = w.start, control = w.control, end = w.end, label = w.label;
        } else {
            const Wire w = wireBetween(r.from, centers[r.from], r.to, centers[r.to], arrangeBend(layout, r));
            start = w.start, control = w.control, end = w.end, label = w.label;
        }
        Path path{r.from, r.to, {}, {label.x - 19.0f, label.y - 9.0f, 38.0f, 18.0f}};
        float length = 0.0f;
        for (int k = 0; k <= kSamples; ++k) {
            const float t = static_cast<float>(k) / kSamples, s = 1.0f - t;
            path.points[k] = {s * s * start.x + 2.0f * s * t * control.x + t * t * end.x,
                              s * s * start.y + 2.0f * s * t * control.y + t * t * end.y};
            if (k > 0)
                length += std::hypot(path.points[k].x - path.points[k - 1].x, path.points[k].y - path.points[k - 1].y);
        }
        // Long enough for its label and arrowhead, with room either side; and no longer than it needs.
        constexpr float kShortest = 96.0f;
        if (length < kShortest)
            score.cost += 14.0f * (kShortest - length);
        score.cost += 0.02f * length;
        if (r.to != kNodes && centers[r.to].x < centers[r.from].x - 10.0f) // running backward, right to left
            score.cost += 0.4f * (centers[r.from].x - centers[r.to].x);
        for (int node : {r.from, r.to}) // the loop on a node it joins, which a wire coming in from above runs into
            if (node != kNodes && looped[node]) {
                const Point loop = loopCenter(node, centers[node]);
                float closest = 1e9f;
                for (const Point &p : path.points)
                    closest = std::min(closest, std::hypot(p.x - loop.x, p.y - loop.y) - 9.0f);
                if (closest < 4.0f)
                    score.cost += 1500.0f;
                if (closest < 1.0f)
                    ++score.clashes;
            }
        for (int node : graph.nodes) {
            if (node == r.from || node == r.to)
                continue;
            float closest = 1e9f;
            for (const Point &p : path.points)
                closest = std::min(closest, arrangeOutside(node, centers[node], p));
            if (looped[node]) { // and its loop, a circle on its top
                const Point loop = loopCenter(node, centers[node]);
                for (const Point &p : path.points)
                    closest = std::min(closest, std::hypot(p.x - loop.x, p.y - loop.y) - 11.0f);
            }
            if (closest < kWireRoom)
                score.cost += 2000.0f * (kWireRoom - closest) / kWireRoom;
            if (closest < 6.0f)
                ++score.clashes;
            if (overNode(node, centers[node], path.label, 6.0f))
                score.cost += 600.0f;
            if (overNode(node, centers[node], path.label, 1.0f))
                ++score.clashes;
        }
        paths.push_back(path);
    }
    // Wires crossing, and labels on each other.
    auto crosses = [](Point a, Point b, Point c, Point d) {
        auto side = [](Point p, Point q, Point r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        return side(a, b, c) * side(a, b, d) < 0.0f && side(c, d, a) * side(c, d, b) < 0.0f;
    };
    // Whether a wire runs through a label, looked at every few pixels along it.
    auto through = [](const Path &wire, const Rect &label) {
        const Rect r = label.inset(-3.0f);
        for (int k = 0; k < kSamples; ++k)
            for (int m = 0; m < 4; ++m) {
                const float t = m / 4.0f;
                const Point a = wire.points[k], b = wire.points[k + 1];
                const Point p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (r.contains(p))
                    return true;
            }
        return false;
    };
    // The loops' labels, beside them on their right where they can (see View::routeLabels()).
    for (int node : graph.nodes) {
        if (!looped[node])
            continue;
        const Point loop = loopCenter(node, centers[node]);
        const Rect label{loop.x + 32.0f - 22.0f, loop.y - 9.0f, 44.0f, 18.0f};
        for (const Path &p : paths) {
            if (through(p, label))
                score.cost += 150.0f;
            const Rect &b = p.label;
            if (label.x < b.right() && b.x < label.right() && label.y < b.bottom() && b.y < label.bottom())
                score.cost += 150.0f;
        }
        for (int other : graph.nodes)
            if (other != node && overNode(other, centers[other], label, 2.0f))
                score.cost += 150.0f;
    }
    for (size_t i = 0; i < paths.size(); ++i)
        for (size_t j = i + 1; j < paths.size(); ++j) {
            const Path &p = paths[i], &q = paths[j];
            const Rect &a = p.label, &b = q.label;
            if (a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom()) {
                score.cost += 400.0f;
                ++score.clashes;
            }
            if (through(q, a))
                score.cost += 250.0f;
            if (through(p, b))
                score.cost += 250.0f;
            if (p.from == q.from || p.from == q.to || p.to == q.from || p.to == q.to)
                continue; // wires meeting at a node
            int crossings = 0;
            for (int k = 0; k < kSamples && crossings == 0; ++k)
                for (int m = 0; m < kSamples; ++m)
                    if (crosses(p.points[k], p.points[k + 1], q.points[m], q.points[m + 1])) {
                        ++crossings;
                        break;
                    }
            score.cost += 120.0f * static_cast<float>(crossings);
        }
    return score;
}

inline ArrangeScore arrangeScore(const ParamValues &values, const UiLayout &layout) {
    return arrangeScore(arrangeGraph(values, layout), layout);
}

// Lays the graph out again: from where its nodes are, and from columns by how far each is from OUT (those going out
// nearest it, what modulates them to their left, and so on), each node moved to the free place on a grid over the
// graph's area, and each wire given the bow, that lowers the cost most, until none does; the better of the two is
// kept, and the layout left as it was where neither betters it.
inline void arrangeRouting(const ParamValues &values, UiLayout &layout) {
    const ArrangeGraph graph = arrangeGraph(values, layout);
    if (graph.nodes.empty())
        return;
    constexpr int kCols = 12, kRows = 7;
    // A layout's cost, and a little for each node moved from where it was, so a graph that needs one node moved keeps
    // the others where they were.
    const UiLayout origin = layout;
    auto costOf = [&](const UiLayout &l) {
        ArrangeScore s = arrangeScore(graph, l);
        for (int node : graph.nodes) {
            const Point a = arrangeCenter(l, node), b = arrangeCenter(origin, node);
            s.cost += 0.15f * std::hypot(a.x - b.x, a.y - b.y);
        }
        return s;
    };
    auto climb = [&](UiLayout &l) {
        ArrangeScore now = costOf(l);
        for (int round = 0; round < 40; ++round) {
            bool better = false;
            for (int node : graph.nodes) {
                const Point was = arrangeCenter(l, node);
                const float wx = (was.x - kGraph.x) / kGraph.w, wy = (was.y - kGraph.y) / kGraph.h;
                float bx = wx, by = wy;
                for (int col = 0; col < kCols; ++col)
                    for (int row = 0; row < kRows; ++row) {
                        const float x = static_cast<float>(col) / (kCols - 1), y = static_cast<float>(row) / (kRows - 1);
                        arrangePlace(l, node, x, y);
                        if (const ArrangeScore s = costOf(l); s.cost < now.cost - 0.01f) {
                            now = s;
                            bx = x, by = y;
                            better = true;
                        }
                    }
                arrangePlace(l, node, bx, by);
            }
            for (size_t i = 0; i < graph.nodes.size(); ++i) // two nodes swapping places
                for (size_t j = i + 1; j < graph.nodes.size(); ++j) {
                    const int a = graph.nodes[i], b = graph.nodes[j];
                    const Point ca = arrangeCenter(l, a), cb = arrangeCenter(l, b);
                    auto norm = [](Point c) { return Point{(c.x - kGraph.x) / kGraph.w, (c.y - kGraph.y) / kGraph.h}; };
                    arrangePlace(l, a, norm(cb).x, norm(cb).y);
                    arrangePlace(l, b, norm(ca).x, norm(ca).y);
                    if (const ArrangeScore s = costOf(l); s.cost < now.cost - 0.01f) {
                        now = s;
                        better = true;
                    } else {
                        arrangePlace(l, a, norm(ca).x, norm(ca).y);
                        arrangePlace(l, b, norm(cb).x, norm(cb).y);
                    }
                }
            for (const auto &r : graph.routes) { // each wire's bow
                if (r.from == r.to)
                    continue;
                const float was = arrangeBend(l, r);
                float best = was;
                const float bows[] = {kDefaultWireBend, -kDefaultWireBend, 0.0f, 0.12f, -0.12f};
                for (float bow : bows) {
                    arrangeSetBend(l, r, bow);
                    if (const ArrangeScore s = costOf(l); s.cost < now.cost - 0.01f) {
                        now = s;
                        best = bow;
                        better = true;
                    }
                }
                arrangeSetBend(l, r, best);
            }
            if (!better)
                break;
        }
        return now;
    };
    // From where the nodes are.
    UiLayout kept = layout;
    const ArrangeScore keptScore = climb(kept);
    // From columns by how far each node is from OUT, the wires bowing as they start.
    UiLayout layered = layout;
    for (const auto &r : graph.routes)
        arrangeSetBend(layered, r, r.to == kNodes ? 0.0f : kDefaultWireBend);
    std::array<int, kNodes> depth{};
    depth.fill(1000);
    for (const auto &r : graph.routes)
        if (r.to == kNodes)
            depth[r.from] = 1;
    for (int pass = 0; pass < kNodes; ++pass)
        for (const auto &r : graph.routes)
            if (r.to != kNodes && r.from != r.to && depth[r.to] < 1000)
                depth[r.from] = std::min(depth[r.from], depth[r.to] + 1);
    int deepest = 1;
    for (int node : graph.nodes)
        if (depth[node] < 1000)
            deepest = std::max(deepest, depth[node]);
    for (int node : graph.nodes)
        if (depth[node] >= 1000)
            depth[node] = deepest + 1;
    for (int node : graph.nodes)
        deepest = std::max(deepest, depth[node]);
    for (int d = 1; d <= deepest; ++d) {
        std::vector<int> column;
        for (int node : graph.nodes)
            if (depth[node] == d)
                column.push_back(node);
        const float x = deepest > 1 ? 0.95f - 0.9f * static_cast<float>(d - 1) / (deepest - 1) : 0.75f;
        for (size_t i = 0; i < column.size(); ++i)
            arrangePlace(layered, column[i], std::round(x * (kCols - 1)) / (kCols - 1),
                         std::round((static_cast<float>(i) + 1.0f) / (column.size() + 1.0f) * (kRows - 1)) / (kRows - 1));
    }
    const ArrangeScore layeredScore = climb(layered);
    const ArrangeScore before = costOf(layout);
    const UiLayout &best = layeredScore.cost < keptScore.cost ? layered : kept;
    if (std::min(layeredScore.cost, keptScore.cost) < before.cost)
        layout = best;
}

} // namespace fmvibe::gui::detail
