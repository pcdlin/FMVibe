#include "routingmodel.h"

#include "algorithms.h"
#include "arrange.h"
#include "layout.h"
#include "routing.h"

#include <bit>
#include <limits>
#include <memory>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

void RoutingModel::connect(int from, int to) {
    const UndoScope step(controller_); // the output a filter or shaper takes over goes in the same step
    const int param = routeParam(from, to);
    if (value(param) > 0.0)
        return;
    if (isBoxNode(to) && from != to) {
        // A node that already goes to the output is moved through the filter or shaper, which goes out in its place
        // unless its sound gets there already.
        const int out = outParam(from);
        const double level = value(out);
        edit(param, level > 0.0 ? level : 1.0);
        if (level > 0.0) {
            // The output's modulation goes along with it, as with a route moved by its arrowhead (see reroute()),
            // unless its source modulates the send already.
            const std::vector<Assignment> mods = modulation().assignments();
            std::vector<std::pair<int, double>> moved;
            for (const Assignment &m : mods)
                if (m.target == out && std::none_of(mods.begin(), mods.end(), [&](const Assignment &o) {
                        return o.source == m.source && o.target == param;
                    }))
                    moved.push_back({slotTargetIndex(m.source, m.slot), slotTargetValue(param)});
            editMany(moved);
            disconnect(out);
            if (!boxGoesOut(to))
                edit(outParam(to), 1.0);
        }
    } else {
        edit(param, from == to ? 0.25 : 0.3); // modulation and feedback start lower
    }
    keepRoutes();
}

void RoutingModel::reroute(int from, int to, int into) {
    const int param = routeTo(from, to), next = routeTo(from, into);
    if (next == param)
        return;
    if (wired(next)) {
        say(trf(Str::MsgRouteExists, {nodeTitle(from), into == kNodes ? tr(Str::BusOut) : nodeTitle(into)}));
        return;
    }
    // A node's output moved into a filter or shaper goes out through it: the box goes out unless its sound gets
    // there already.
    moveRoute(param, next, to == kNodes && into < kNodes && isBoxNode(into) && !boxGoesOut(into) ? into : -1);
}

void RoutingModel::rerouteFrom(int from, int to, int source) {
    const int param = routeTo(from, to), next = routeTo(source, to);
    if (next == param)
        return;
    if (wired(next)) {
        say(trf(Str::MsgRouteExists, {nodeTitle(source), to == kNodes ? tr(Str::BusOut) : nodeTitle(to)}));
        return;
    }
    moveRoute(param, next);
}

void RoutingModel::moveRoute(int param, int next, int goesOut) {
    const UndoScope step(controller_); // with the routes it keeps
    const std::vector<int> shown = shownNodes();
    const ParamDef &def = table_[next];
    std::vector<std::pair<int, double>> changes{{next, def.min + norm(param) * (def.max - def.min)}, {param, 0.0}};
    // Its mode goes along while it still goes into an operator; elsewhere a route has none, and the one it leaves is
    // Phase again.
    const int mode = routeModeOfRoute(param), nextMode = routeModeOfRoute(next);
    if (mode >= 0 && value(mode) != 0.0) {
        if (nextMode >= 0)
            changes.push_back({nextMode, value(mode)});
        changes.push_back({mode, 0.0});
    }
    for (const Assignment &m : modulation().assignments())
        if (m.target == param)
            changes.push_back({slotTargetIndex(m.source, m.slot), slotTargetValue(next)});
    if (goesOut >= 0)
        changes.push_back({outParam(goesOut), 1.0});
    editMany(changes);
    std::vector<int> &kept = controller_.layout().keptRoutes;
    std::erase(kept, param);
    std::erase_if(controller_.layout().wireBends, [&](const auto &bend) { return bend.first == param; }); // a new wire
    kept.push_back(next); // shown even at 0 %, as the route was
    if (goesOut >= 0 && std::find(kept.begin(), kept.end(), outParam(goesOut)) == kept.end())
        kept.push_back(outParam(goesOut));
    hideUnwired(shown);
    controller_.layoutChanged();
}

// A node shown dim that an edit left without a connection is hidden, as its menu's Hide hides it: a route coming
// back without switching it on (a host's automation) leaves it docked.
void RoutingModel::hideUnwired(const std::vector<int> &before) {
    const std::vector<int> now = shownNodes();
    for (int node : before)
        if (!nodeOn(node) && std::find(now.begin(), now.end(), node) == now.end())
            controller_.layout().hiddenNodes |= 1u << node;
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool RoutingModel::boxGoesOut(int node) const {
    std::array<bool, kNodes> seen{};
    auto out = [&](auto &self, int box) -> bool {
        if (seen[box])
            return false;
        seen[box] = true;
        if (value(outParam(box)) > 0.0)
            return true;
        for (int to = kOps; to < kNodes; ++to)
            if (to != box && value(routeParam(box, to)) > 0.0 && nodeOn(to) && self(self, to))
                return true;
        return false;
    };
    return out(out, node);
}

bool RoutingModel::soundsOut(int node) const {
    return value(outParam(node)) > 0.0 && (isBoxNode(node) ? boxHasInput(node) : nodeOn(node));
}

// Operators, filters and shapers alike, whether anything goes into them or not.
bool RoutingModel::goesOut(int node) const {
    return wired(outParam(node));
}

bool RoutingModel::wired(int param) const {
    const std::vector<int> &kept = controller_.layout().keptRoutes;
    return value(param) > 0.0 || std::find(kept.begin(), kept.end(), param) != kept.end();
}

void RoutingModel::keepRoutes() {
    std::vector<int> &kept = controller_.layout().keptRoutes;
    auto keep = [&](int param) {
        if (value(param) > 0.0 && std::find(kept.begin(), kept.end(), param) == kept.end())
            kept.push_back(param);
    };
    for (int from = 0; from < kNodes; ++from) {
        keep(outParam(from));
        for (int to = 0; to < kNodes; ++to)
            keep(routeParam(from, to));
    }
}

// Its modulation goes with it, or the route would still open and close unseen.
void RoutingModel::disconnect(int param) {
    const UndoScope step(controller_);
    const std::vector<int> shown = shownNodes();
    for (const Assignment &m : modulation().assignments())
        if (m.target == param)
            modulation().removeSlot(m.source, m.slot);
    edit(param, 0.0);
    if (const int mode = routeModeOfRoute(param); mode >= 0 && value(mode) != 0.0) // a new route starts as Phase
        edit(mode, 0.0);
    std::erase(controller_.layout().keptRoutes, param);
    std::erase_if(controller_.layout().wireBends, [&](const auto &bend) { return bend.first == param; });
    hideUnwired(shown);
}

bool RoutingModel::nodeOn(int node) const {
    return value(enabledParam(node)) >= 0.5;
}

// What a matrix route joins switches on: the row's node, and the column's unless it is the output.
void RoutingModel::switchOnRoute(int from, int to) {
    for (int node : {from, to})
        if (node < kNodes && !nodeOn(node))
            edit(enabledParam(node), 1.0);
}

std::vector<int> RoutingModel::enabledNodes() const {
    std::vector<int> nodes;
    for (int node = 0; node < kNodes; ++node)
        if (nodeOn(node))
            nodes.push_back(node);
    return nodes;
}

std::vector<int> RoutingModel::shownNodes() const {
    const std::array<bool, kNodes> shown = graphNodes([&](int node) { return nodeOn(node); },
                                                      [&](int param) { return wired(param); },
                                                      controller_.layout().hiddenNodes);
    std::vector<int> nodes;
    for (int node = 0; node < kNodes; ++node)
        if (shown[node])
            nodes.push_back(node);
    return nodes;
}

bool RoutingModel::nodeShown(int node) const {
    if (nodeOn(node))
        return true;
    const std::vector<int> nodes = shownNodes();
    return std::find(nodes.begin(), nodes.end(), node) != nodes.end();
}

void RoutingModel::hideNode(int node) {
    if (!nodeDim(node))
        return;
    const UndoScope step(controller_);
    controller_.layout().hiddenNodes |= 1u << node;
    controller_.layoutChanged();
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool RoutingModel::boxHasInput(int node) const {
    std::array<bool, kNodes> seen{};
    auto fed = [&](auto &self, int box) -> bool {
        if (seen[box] || !nodeOn(box))
            return false;
        seen[box] = true;
        for (int from = 0; from < kNodes; ++from)
            if (from != box && value(routeParam(from, box)) > 0.0 && nodeOn(from) && (from < kOps || self(self, from)))
                return true;
        return false;
    };
    return fed(fed, node);
}

Point RoutingModel::nodeCenter(int node) const {
    const UiLayout &layout = controller_.layout();
    if (isBoxNode(node))
        return {kGraph.x + layout.boxX[node - kOps] * kGraph.w, kGraph.y + layout.boxY[node - kOps] * kGraph.h};
    return {kGraph.x + layout.nodeX[node] * kGraph.w, kGraph.y + layout.nodeY[node] * kGraph.h};
}

// Where a node dragged to `want` goes: there, unless it would cover another node shown, when it is pushed out of
// that one's way to kNodeGap past its outline, and kept inside the graph. A node and the one it is pushed from each
// take a rounded box, their outlines' sum: a circle's is a point grown by its radius, a filter's or a shaper's its box.
// Pushed into a third one with no room left, it stays where it was (and `found`, when given, is false).
Point RoutingModel::nodePlace(int node, Point want, bool *found) const {
    if (found)
        *found = true;
    constexpr float kNodeGap = 10.0f; // the edges' bands, kEdgeOut each, just meet
    auto half = [](int n) { return isBoxNode(n) ? Point{kFilterHalfW, kFilterHalfH} : Point{0.0f, 0.0f}; };
    auto radius = [](int n) { return isBoxNode(n) ? 0.0f : kNodeRadius; };
    // How far, and which way, `at` has to move to clear `other`.
    auto push = [&](Point at, int other) -> Point {
        const Point c = nodeCenter(other);
        const Point h{half(node).x + half(other).x, half(node).y + half(other).y};
        const float r = radius(node) + radius(other) + kNodeGap;
        const Point d{at.x - c.x, at.y - c.y};
        const Point e{d.x - std::clamp(d.x, -h.x, h.x), d.y - std::clamp(d.y, -h.y, h.y)};
        const float out = std::hypot(e.x, e.y);
        if (out > 1e-4f)
            return out >= r - 0.01f ? Point{} : Point{e.x / out * (r - out), e.y / out * (r - out)}; // rounding aside
        const float x = h.x + r - std::abs(d.x), y = h.y + r - std::abs(d.y); // inside the box: out the nearer side
        return x <= y ? Point{d.x < 0.0f ? -x : x, 0.0f} : Point{0.0f, d.y < 0.0f ? -y : y};
    };
    auto inGraph = [](Point at) {
        return Point{std::clamp(at.x, kGraph.x, kGraph.right()), std::clamp(at.y, kGraph.y, kGraph.bottom())};
    };
    const std::vector<int> on = shownNodes(); // clear of the nodes shown dim too
    Point at = inGraph(want);
    for (int pass = 0; pass < 8; ++pass) {
        bool clear = true;
        for (int other : on) {
            if (other == node)
                continue;
            const Point by = push(at, other);
            if (by.x != 0.0f || by.y != 0.0f) {
                at = inGraph({at.x + by.x, at.y + by.y});
                clear = false;
            }
        }
        if (clear)
            return at;
    }
    if (found)
        *found = false;
    return nodeCenter(node);
}

// A node just switched on goes where it was, unless that covers a node shown: then out of its way, or, with no room
// there, to the free place nearest it on a grid over the graph. With none free anywhere, it stays.
void RoutingModel::placeSwitchedOn(int node) {
    const Point was = nodeCenter(node);
    bool found = false;
    Point at = nodePlace(node, was, &found);
    if (found && at.x == was.x && at.y == was.y)
        return; // clear where it is: left exactly so
    if (!found) {
        constexpr int kSteps = 16;
        std::vector<Point> spots;
        for (int i = 0; i <= kSteps; ++i)
            for (int j = 0; j <= kSteps; ++j)
                spots.push_back({kGraph.x + kGraph.w * i / kSteps, kGraph.y + kGraph.h * j / kSteps});
        auto dist = [&](Point p) { return std::hypot(p.x - was.x, p.y - was.y); };
        std::stable_sort(spots.begin(), spots.end(), [&](Point a, Point b) { return dist(a) < dist(b); });
        for (Point spot : spots) {
            at = nodePlace(node, spot, &found);
            if (found)
                break;
        }
        if (!found)
            return;
    }
    UiLayout &layout = controller_.layout();
    const float x = std::clamp((at.x - kGraph.x) / kGraph.w, 0.0f, 1.0f);
    const float y = std::clamp((at.y - kGraph.y) / kGraph.h, 0.0f, 1.0f);
    if (isBoxNode(node)) {
        layout.boxX[node - kOps] = x;
        layout.boxY[node - kOps] = y;
    } else {
        layout.nodeX[node] = x;
        layout.nodeY[node] = y;
    }
}

bool RoutingModel::outThroughBox(int op) const {
    for (int box = kOps; box < kNodes; ++box)
        if (nodeOn(box) && value(routeParam(op, box)) > 0.0 && boxGoesOut(box))
            return true;
    return false;
}

float RoutingModel::wireBend(int param) const {
    for (const auto &[route, bend] : controller_.layout().wireBends)
        if (route == param)
            return bend;
    return defaultWireBend(param);
}

// A wire between nodes bows to begin with; a node's line to OUT runs straight.
float RoutingModel::defaultWireBend(int param) {
    for (int node = 0; node < kNodes; ++node)
        if (outParam(node) == param)
            return 0.0f;
    return kDefaultWireBend;
}

void RoutingModel::setWireBend(int param, float bend) {
    std::vector<std::pair<int, float>> &bends = controller_.layout().wireBends;
    std::erase_if(bends, [&](const auto &b) { return b.first == param; });
    bend = std::clamp(bend, -kMaxWireBend, kMaxWireBend);
    if (bend != defaultWireBend(param))
        bends.push_back({param, bend});
    controller_.layoutChanged();
}

void RoutingModel::applyAlgorithm(int index) {
    if (index < 0 || index >= static_cast<int>(kAlgorithms.size()))
        return;
    const Algorithm &a = kAlgorithms[index];
    const UndoScope step(controller_);
    // The operators it uses on, the others off, their settings and routes kept for when they come back.
    for (int op = 0; op < kOps; ++op)
        if (const double on = op < a.ops ? 1.0 : 0.0; value(enabledParam(op)) != on)
            edit(enabledParam(op), on);
    for (int to = 0; to < a.ops; ++to)
        for (int from = 0; from < a.ops; ++from) {
            const int param = routeParam(from, to);
            const bool feedback = from == a.feedbackFrom && to == a.feedbackTo;
            if ((a.mods[to] >> from & 1) || feedback) {
                if (value(param) <= 0.0)
                    edit(param, feedback ? kAlgorithmFeedback : kAlgorithmDepth);
            } else if (wired(param)) {
                disconnect(param);
            }
        }
    const double level = 1.0 / std::sqrt(static_cast<double>(std::popcount(a.carriers)));
    for (int op = 0; op < a.ops; ++op) {
        const int out = outParam(op);
        if (a.carriers >> op & 1) {
            if (value(out) <= 0.0 && !outThroughBox(op))
                edit(out, level);
        } else {
            // A modulator isn't heard: neither straight out nor through a filter or shaper that goes out. What it
            // sends into one that only modulates stays.
            if (wired(out))
                disconnect(out);
            for (int box = kOps; box < kNodes; ++box)
                if (nodeOn(box) && wired(routeParam(op, box)) && boxGoesOut(box))
                    disconnect(routeParam(op, box));
        }
    }
    keepRoutes();
    // Laid out afresh: in its places, the wires among its operators and their lines to OUT bowing as new ones do.
    UiLayout &layout = controller_.layout();
    std::erase_if(layout.wireBends, [&](const auto &bend) {
        for (int from = 0; from < a.ops; ++from) {
            if (bend.first == outParam(from))
                return true;
            for (int to = 0; to < a.ops; ++to)
                if (bend.first == routeParam(from, to))
                    return true;
        }
        return false;
    });
    const AlgorithmPlaces places = algorithmPlaces(a);
    for (int op = 0; op < a.ops; ++op) {
        layout.nodeX[op] = (places.col[op] + 0.5f) / static_cast<float>(kRoutingGridCols);
        layout.nodeY[op] = std::clamp(places.y[op], 0.0f, 1.0f);
    }
    for (int op = 0; op < a.ops; ++op) // clear of a filter or shaper shown where it lands
        placeSwitchedOn(op);
    const auto values = std::make_unique<ParamValues>();
    for (int i = 0; i < kParamCount; ++i)
        (*values)[i] = value(i);
    auto bendTo = [&](int param, float bend, float rest) {
        std::erase_if(layout.wireBends, [&](const auto &b) { return b.first == param; });
        if (bend != rest)
            layout.wireBends.push_back({param, bend});
    };
    auto better = [](const ArrangeScore &s, const ArrangeScore &than) {
        return s.clashes < than.clashes || (s.clashes == than.clashes && s.cost < than.cost - 0.01f);
    };
    // A wire given the bow, the one it has or among `bends` (the first its own), at which the layout's clashes and
    // cost are least.
    auto bowBest = [&](int param, std::initializer_list<float> bends) {
        const float rest = *bends.begin();
        float best = rest;
        for (const auto &[p, bend] : layout.wireBends)
            if (p == param)
                best = bend;
        ArrangeScore least = arrangeScore(*values, layout);
        for (float bend : bends) {
            bendTo(param, bend, rest);
            if (const ArrangeScore s = arrangeScore(*values, layout); better(s, least)) {
                least = s;
                best = bend;
            }
        }
        bendTo(param, best, rest);
    };
    // A loop back up a stack would run straight through the operators between: it bows round them, as far and which
    // way the layout's clashes and cost are least.
    if (a.feedbackFrom != a.feedbackTo)
        bowBest(routeParam(a.feedbackFrom, a.feedbackTo),
                {kDefaultWireBend, -kDefaultWireBend, 0.3f, -0.3f, 0.45f, -0.45f, 0.6f, -0.6f});
    // Many carriers' lines would come into OUT together, their arrowheads on each other: they fan out, bowing the
    // further the further from the middle of them, as far as the clashes and cost are least, and then each as far and
    // which way they are least.
    std::vector<int> outs;
    for (int op = 0; op < a.ops; ++op)
        if ((a.carriers >> op & 1) && wired(outParam(op)))
            outs.push_back(op);
    if (outs.size() > 2) {
        std::sort(outs.begin(), outs.end(), [&](int x, int y) { return layout.nodeY[x] < layout.nodeY[y]; });
        auto fan = [&](float reach) {
            for (size_t i = 0; i < outs.size(); ++i)
                bendTo(outParam(outs[i]), reach * (2.0f * i / (outs.size() - 1) - 1.0f), 0.0f);
        };
        float best = 0.0f;
        ArrangeScore least = arrangeScore(*values, layout);
        for (float reach : {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f, 0.5f, -0.5f}) {
            fan(reach);
            if (const ArrangeScore s = arrangeScore(*values, layout); better(s, least)) {
                least = s;
                best = reach;
            }
        }
        fan(best);
        for (int pass = 0; pass < 2; ++pass)
            for (int op : outs)
                bowBest(outParam(op), {0.0f, 0.12f, -0.12f, 0.24f, -0.24f, 0.36f, -0.36f, 0.48f, -0.48f});
    }
    controller_.layoutChanged();
}

int RoutingModel::currentAlgorithm() const {
    OpShape now;
    for (int op = 0; op < kOps; ++op)
        if (nodeOn(op))
            now.on |= static_cast<uint8_t>(1 << op);
    for (int from = 0; from < kOps; ++from) {
        if (!(now.on >> from & 1))
            continue;
        if (wired(outParam(from)) || outThroughBox(from))
            now.out |= static_cast<uint8_t>(1 << from);
        for (int to = 0; to < kOps; ++to)
            if ((now.on >> to & 1) && wired(routeParam(from, to)))
                now.routes |= routeBit(from, to);
    }
    for (size_t i = 0; i < kAlgorithms.size(); ++i)
        if (shapeOf(kAlgorithms[i]) == now)
            return static_cast<int>(i);
    return -1;
}

} // namespace fmvibe::gui
