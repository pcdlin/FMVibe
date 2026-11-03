#include "view.h"

#include "layout.h"
#include "routing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

void View::connect(int from, int to) {
    const UndoScope step(controller_); // the output a filter or shaper takes over goes in the same step
    const int param = routeParam(from, to);
    if (param < 0) {
        flash(routeRefusal(from));
        return;
    }
    if (value(param) > 0.0)
        return;
    if (isBoxNode(to)) {
        // A node that already goes to the output is moved through the filter or shaper, which goes out in its place
        // unless its sound gets there already.
        const int out = outParam(from);
        const double level = value(out);
        edit(param, level > 0.0 ? level : 1.0);
        if (level > 0.0) {
            // The output's modulation goes along with it, as with a route moved by its arrowhead (see reroute()),
            // unless its source modulates the send already.
            const std::vector<Assignment> mods = assignments();
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
        edit(param, from == to ? 0.25 : 0.3); // modulation starts lower
    }
    keepRoutes();
}

void View::reroute(int from, int to, int into) {
    const int param = routeTo(from, to), next = routeTo(from, into);
    if (next == param)
        return;
    if (next < 0) {
        flash(routeRefusal(from));
        return;
    }
    if (wired(next)) {
        flash(trf(Str::MsgRouteExists, {nodeTitle(from), into == kNodes ? tr(Str::BusOut) : nodeTitle(into)}));
        return;
    }
    const UndoScope step(controller_); // with the routes it keeps
    const ParamDef &def = table_[next];
    std::vector<std::pair<int, double>> changes{{next, def.min + norm(param) * (def.max - def.min)}, {param, 0.0}};
    for (const Assignment &m : assignments())
        if (m.target == param)
            changes.push_back({slotTargetIndex(m.source, m.slot), slotTargetValue(next)});
    // A node's output moved into a filter or shaper goes out through it: the box goes out unless its sound gets
    // there already.
    const bool takesOver = to == kNodes && into < kNodes && isBoxNode(into) && !boxGoesOut(into);
    if (takesOver)
        changes.push_back({outParam(into), 1.0});
    editMany(changes);
    std::vector<int> &kept = controller_.layout().keptRoutes;
    std::erase(kept, param);
    std::erase_if(controller_.layout().wireBends, [&](const auto &bend) { return bend.first == param; }); // a new wire
    kept.push_back(next); // shown even at 0 %, as the route was
    if (takesOver && std::find(kept.begin(), kept.end(), outParam(into)) == kept.end())
        kept.push_back(outParam(into));
    controller_.layoutChanged();
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool View::boxGoesOut(int node) const {
    std::array<bool, kNodes> seen{};
    auto out = [&](auto &self, int box) -> bool {
        if (seen[box])
            return false;
        seen[box] = true;
        if (value(outParam(box)) > 0.0)
            return true;
        for (int to = kOps; to < kNodes; ++to) {
            const int param = to == box ? -1 : routeParam(box, to);
            if (param >= 0 && value(param) > 0.0 && nodeOn(to) && self(self, to))
                return true;
        }
        return false;
    };
    return out(out, node);
}

int View::rerouting() const {
    return drag_.kind == DragKind::Reroute && drag_.moved ? drag_.widget.param : -1;
}

bool View::soundsOut(int node) const {
    return value(outParam(node)) > 0.0 && (isBoxNode(node) ? boxHasInput(node) : nodeOn(node));
}

// Operators, filters and shapers alike, whether anything goes into them or not.
bool View::goesOut(int node) const {
    const int param = outParam(node);
    return param >= 0 && wired(param);
}

bool View::wired(int param) const {
    const std::vector<int> &kept = controller_.layout().keptRoutes;
    return value(param) > 0.0 || std::find(kept.begin(), kept.end(), param) != kept.end();
}

void View::keepRoutes() {
    std::vector<int> &kept = controller_.layout().keptRoutes;
    auto keep = [&](int param) {
        if (param >= 0 && value(param) > 0.0 && std::find(kept.begin(), kept.end(), param) == kept.end())
            kept.push_back(param);
    };
    for (int from = 0; from < kNodes; ++from) {
        keep(outParam(from));
        for (int to = 0; to < kNodes; ++to)
            keep(routeParam(from, to));
    }
}

// Its modulation goes with it, or the route would still open and close unseen.
void View::disconnect(int param) {
    const UndoScope step(controller_);
    for (const Assignment &m : assignments())
        if (m.target == param)
            removeSlot(m.source, m.slot);
    edit(param, 0.0);
    std::erase(controller_.layout().keptRoutes, param);
    std::erase_if(controller_.layout().wireBends, [&](const auto &bend) { return bend.first == param; });
}

bool View::nodeOn(int node) const {
    return value(enabledParam(node)) >= 0.5;
}

// What a matrix route joins switches on: the row's node, and the column's unless it is the output.
void View::switchOnRoute(int from, int to) {
    for (int node : {from, to})
        if (node < kNodes && !nodeOn(node))
            edit(enabledParam(node), 1.0);
}

std::vector<int> View::enabledNodes() const {
    std::vector<int> nodes;
    for (int node = 0; node < kNodes; ++node)
        if (nodeOn(node))
            nodes.push_back(node);
    return nodes;
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool View::boxHasInput(int node) const {
    std::array<bool, kNodes> seen{};
    auto fed = [&](auto &self, int box) -> bool {
        if (seen[box] || !nodeOn(box))
            return false;
        seen[box] = true;
        for (int from = 0; from < kNodes; ++from) {
            const int param = from == box ? -1 : routeParam(from, box);
            if (param >= 0 && value(param) > 0.0 && nodeOn(from) && (from < kOps || self(self, from)))
                return true;
        }
        return false;
    };
    return fed(fed, node);
}

bool View::reachesOut(int node) const {
    // A search along the routes that are on, from `node` through switched-on nodes to one that goes out.
    std::array<bool, kNodes> visited{};
    auto reaches = [&](auto &self, int from) -> bool {
        if (visited[from] || !nodeOn(from))
            return false;
        visited[from] = true;
        if (const int out = outParam(from); out >= 0 && value(out) > 0.0)
            return true;
        for (int to = 0; to < kNodes; ++to) {
            const int param = to == from ? -1 : routeParam(from, to);
            if (param >= 0 && value(param) > 0.0 && self(self, to))
                return true;
        }
        return false;
    };
    return reaches(reaches, node);
}

Point View::nodeCenter(int node) const {
    const UiLayout &layout = controller_.layout();
    if (isBoxNode(node))
        return {kGraph.x + layout.boxX[node - kOps] * kGraph.w, kGraph.y + layout.boxY[node - kOps] * kGraph.h};
    return {kGraph.x + layout.nodeX[node] * kGraph.w, kGraph.y + layout.nodeY[node] * kGraph.h};
}

int View::nodeAt(Point p) const {
    for (int node = kNodes - 1; node >= 0; --node) {
        if (!nodeOn(node))
            continue;
        const Point c = nodeCenter(node);
        const bool inside = isBoxNode(node)
                                ? std::abs(p.x - c.x) <= kFilterHalfW + 4.0f && std::abs(p.y - c.y) <= kFilterHalfH + 4.0f
                                : std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius + 4.0f;
        if (inside)
            return node;
    }
    return -1;
}

} // namespace fmvibe::gui
