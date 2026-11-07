// The effects graph (GlobalParam::FxGraph): its model, the View's methods that wire it, and its widgets and painting.
// Its nodes are the effect slots and the splits, which the engine runs in the order the wires make (see
// Effects::processGraph()), the input (the voices) and the output.

#include "view.h"

#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "routing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

namespace fmvibe::gui {

using namespace detail;

namespace {

// How far a node's outline lies from its center in direction `dir`.
float boxExtent(Point dir, float halfW, float halfH) {
    const float tx = std::abs(dir.x) > 1e-4f ? halfW / std::abs(dir.x) : 1e9f;
    const float ty = std::abs(dir.y) > 1e-4f ? halfH / std::abs(dir.y) : 1e9f;
    return std::min(tx, ty);
}

float nodeExtentOf(int node, Point dir) {
    if (node == kFxOutNode)
        return boxExtent(dir, kFxEndHalfW, kFxEndHalfH);
    if (node >= kFxSlots)
        return boxExtent(dir, kFxSplitHalfW, kFxSplitHalfH);
    return kFxNodeR;
}

Rect nodeRect(int node, Point center) {
    if (node == kFxOutNode)
        return {center.x - kFxEndHalfW, center.y - kFxEndHalfH, 2.0f * kFxEndHalfW, 2.0f * kFxEndHalfH};
    if (node >= kFxSlots)
        return {center.x - kFxSplitHalfW, center.y - kFxSplitHalfH, 2.0f * kFxSplitHalfW, 2.0f * kFxSplitHalfH};
    return {center.x - kFxNodeR, center.y - kFxNodeR, 2.0f * kFxNodeR, 2.0f * kFxNodeR};
}

Rect inRect() {
    const Point c = fxInCenter();
    return {c.x - kFxEndHalfW, c.y - kFxEndHalfH, 2.0f * kFxEndHalfW, 2.0f * kFxEndHalfH};
}

// A wire's level in numbers, the label at its middle.
Rect labelRect(Point at) {
    return {at.x - 19.0f, at.y - 8.0f, 38.0f, 16.0f};
}

// A wire from `a` into `node` at `to`: from an effect's center (`fromSlot`, leaving its circle) or from a port on an
// outline already (the input's, a band's).
Wire fxWireShape(Point a, bool fromSlot, int node, Point to, float bend) {
    return bentWireBy(a, to, bend, [fromSlot](Point) { return fromSlot ? kFxNodeR : 0.0f; },
                      [node](Point dir) { return nodeExtentOf(node, dir); });
}

std::string percent(double v) {
    char text[16];
    std::snprintf(text, sizeof(text), "%d %%", static_cast<int>(std::lround(v * 100.0)));
    return text;
}

} // namespace

// --- Model

bool View::fxGraph() const {
    return value(paramIndex(GlobalParam::FxGraph)) >= 0.5;
}

bool View::fxNodeShown(int node) const {
    if (node == kFxOutNode)
        return true;
    if (node < 0 || node > kFxOutNode)
        return false;
    if (node < kFxSlots)
        return fxSlotType(node) != FxType::None;
    return value(paramIndex(node - kFxSlots, SplitParam::Used)) >= 0.5;
}

int View::fxPortNode(int port) {
    if (port == kFxPortIn)
        return -1;
    return fxPortSlot(port) >= 0 ? fxPortSlot(port) : fxSplitNode(fxPortSplit(port));
}

std::vector<View::FxWireRef> View::fxWires() const {
    std::vector<FxWireRef> wires;
    for (int node = 0; node <= kFxOutNode; ++node) {
        if (!fxNodeShown(node))
            continue;
        for (int input = 0; input < fxInputCount(node); ++input) {
            const int port = fxPortOf(value(fxInputIndex(node, input, FxInputField::From)));
            if (port < 0)
                continue;
            const int from = fxPortNode(port);
            if (from >= 0 && !fxNodeShown(from))
                continue;
            // A split of two bands has no middle one.
            if (fxPortBand(port) == 1 && std::lround(value(paramIndex(fxPortSplit(port), SplitParam::Bands))) < 3)
                continue;
            wires.push_back({node, input, port});
        }
    }
    return wires;
}

bool View::fxReaches(int from, int to) const {
    if (from == to)
        return true;
    const std::vector<FxWireRef> wires = fxWires();
    std::vector<bool> seen(kFxOutNode + 1, false);
    std::deque<int> next{from};
    seen[from] = true;
    while (!next.empty()) {
        const int node = next.front();
        next.pop_front();
        for (const FxWireRef &w : wires)
            if (fxPortNode(w.port) == node && !seen[w.node]) {
                if (w.node == to)
                    return true;
                seen[w.node] = true;
                next.push_back(w.node);
            }
    }
    return false;
}

Point View::fxNodeCenter(int node) const {
    if (node == kFxOutNode)
        return fxOutCenter();
    if (node < 0)
        return fxInCenter();
    const UiLayout &layout = controller_.layout();
    return fxNodeAt(layout.fxNodeX[node], layout.fxNodeY[node]);
}

Point View::fxPortPoint(int port, Point toward) const {
    if (port == kFxPortIn) {
        const Point c = fxInCenter();
        return {c.x + kFxEndHalfW, c.y};
    }
    if (const int band = fxPortBand(port); band >= 0)
        return fxBandPortAt(fxNodeCenter(fxSplitNode(fxPortSplit(port))), band);
    const Point c = fxNodeCenter(fxPortSlot(port));
    const Point dir = normalize({toward.x - c.x, toward.y - c.y});
    return {c.x + dir.x * kFxNodeR, c.y + dir.y * kFxNodeR};
}

Rect View::fxInspector(int node) const {
    return fxInspectorCard(node >= 0 && node < kFxSlots ? fxSpan(fxSlotType(node)) : 1.0f);
}

int View::fxSelectedNode() const {
    const int node = controller_.layout().fxSelected;
    if (node >= 0 && node < kFxInputNodes && fxNodeShown(node))
        return node;
    const std::vector<int> chain = fxChain(); // none picked: the first effect, else the first split
    if (!chain.empty())
        return chain.front();
    for (int split = 0; split < kFxSplits; ++split)
        if (fxNodeShown(fxSplitNode(split)))
            return fxSplitNode(split);
    return -1;
}

std::optional<Rect> View::fxCardOf(int slot) const {
    if (fxGraph())
        return fxSelectedNode() == slot ? std::optional(fxInspector(slot)) : std::nullopt;
    const std::vector<int> chain = fxChainShown();
    const auto at = std::find(chain.begin(), chain.end(), slot);
    if (at == chain.end())
        return std::nullopt;
    const float place = fxScreen(chain, static_cast<size_t>(at - chain.begin()));
    const float span = fxCardSpan(slot);
    if (place <= -span || place >= static_cast<float>(kVisibleFx))
        return std::nullopt;
    return fxCard(place, span);
}

bool View::fxHit(const Widget &w, Point p) const {
    if (!w.rect.contains(p))
        return false;
    if (w.kind == Kind::FxPort && w.b >= 0 && w.b < kFxSlots) { // an effect's edge: a band along its circle
        const Point c = fxNodeCenter(w.b);
        const float d = std::hypot(p.x - c.x, p.y - c.y);
        return d >= kFxNodeR - kEdgeIn && d <= kFxNodeR + kEdgeOut;
    }
    if (w.kind != Kind::FxWire)
        return true;
    const int port = fxPortOf(value(fxInputIndex(w.a, w.b, FxInputField::From)));
    if (port < 0)
        return false;
    const Point to = fxNodeCenter(w.a);
    const int from = fxPortNode(port);
    const bool slot = from >= 0 && from < kFxSlots;
    const Wire wire = fxWireShape(slot ? fxNodeCenter(from) : fxPortPoint(port, to), slot, w.a, to, wireBend(w.param));
    return distanceToWire(wire, p) <= kWireGrab;
}

int View::fxNodeAtPoint(Point p) const {
    if (nodeRect(kFxOutNode, fxOutCenter()).inset(-6.0f).contains(p))
        return kFxOutNode;
    for (int node = kFxInputNodes - 1; node >= 0; --node) // the ones drawn last first
        if (fxNodeShown(node) && nodeRect(node, fxNodeCenter(node)).inset(-6.0f).contains(p))
            return node;
    return -1;
}

// --- Editing

bool View::fxConnect(int port, int node) {
    if (node < 0 || node > kFxOutNode || !fxNodeShown(node))
        return false;
    const int from = fxPortNode(port);
    if (from == node || (!kAllowFxLoops && from >= 0 && fxReaches(node, from))) {
        flash(tr(Str::MsgFxLoop));
        return false;
    }
    int free = -1;
    for (int input = fxInputCount(node) - 1; input >= 0; --input) {
        const int was = fxPortOf(value(fxInputIndex(node, input, FxInputField::From)));
        if (was == port) {
            flash(tr(Str::MsgFxWireExists));
            return false;
        }
        if (was < 0)
            free = input;
    }
    if (free < 0) {
        flash(trf(Str::MsgFxInputsFull, {std::to_string(fxInputCount(node))}));
        return false;
    }
    const UndoScope step(controller_); // with the chain's places it moves
    const int level = fxInputIndex(node, free, FxInputField::Level);
    setWireBend(level, kDefaultWireBend);
    editMany({{fxInputIndex(node, free, FxInputField::From), fxPortValue(port)}, {level, 1.0}});
    fxSyncOrder();
    return true;
}

void View::fxRemoveWire(int node, int input, std::vector<std::pair<int, double>> &changes) {
    const int level = fxInputIndex(node, input, FxInputField::Level);
    for (const Assignment &m : assignments()) // its modulation goes with it
        if (m.target == level)
            removeSlot(m.source, m.slot);
    setWireBend(level, kDefaultWireBend);
    changes.push_back({fxInputIndex(node, input, FxInputField::From), 0.0});
    changes.push_back({level, 1.0});
}

void View::fxRewire(int node, int input, int into) {
    if (into == node)
        return;
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes;
    const int port = fxPortOf(value(fxInputIndex(node, input, FxInputField::From)));
    const int level = fxInputIndex(node, input, FxInputField::Level);
    if (into < 0 || port < 0) {
        fxRemoveWire(node, input, changes);
        editMany(changes);
        fxSyncOrder();
        return;
    }
    const int from = fxPortNode(port);
    if (from == into || (!kAllowFxLoops && from >= 0 && fxReaches(into, from))) {
        flash(tr(Str::MsgFxLoop));
        return;
    }
    int free = -1;
    for (int k = fxInputCount(into) - 1; k >= 0; --k) {
        const int was = fxPortOf(value(fxInputIndex(into, k, FxInputField::From)));
        if (was == port) {
            flash(tr(Str::MsgFxWireExists));
            return;
        }
        if (was < 0)
            free = k;
    }
    if (free < 0) {
        flash(trf(Str::MsgFxInputsFull, {std::to_string(fxInputCount(into))}));
        return;
    }
    // Its level and its modulation go along; the bend stays behind, as a new wire's.
    const int next = fxInputIndex(into, free, FxInputField::Level);
    changes = {{fxInputIndex(into, free, FxInputField::From), fxPortValue(port)},
               {next, value(level)},
               {fxInputIndex(node, input, FxInputField::From), 0.0},
               {level, 1.0}};
    for (const Assignment &m : assignments())
        if (m.target == level)
            changes.push_back({slotTargetIndex(m.source, m.slot), slotTargetValue(next)});
    setWireBend(level, kDefaultWireBend);
    setWireBend(next, kDefaultWireBend);
    editMany(changes);
    fxSyncOrder();
}

void View::fxInsertAfterSelected(int node, std::vector<std::pair<int, double>> &changes) {
    const int selected = fxSelectedNode();
    const bool afterSlot = selected >= 0 && selected < kFxSlots && selected != node;
    const int source = afterSlot ? fxSlotPort(selected) : kFxPortIn;
    // What the node puts out: an effect its output, a split its low and high bands.
    std::vector<int> outs;
    if (node < kFxSlots)
        outs.push_back(fxSlotPort(node));
    else
        outs = {fxBandPort(node - kFxSlots, 0), fxBandPort(node - kFxSlots, kFxBands - 1)};
    // A split's high band bows up toward where it goes, its low band down (as wires bow to begin with).
    auto bowUp = [&](int level) { setWireBend(level, -kDefaultWireBend); };
    // The wires out of what it goes after now come out of it.
    bool taken = false;
    for (const FxWireRef &w : fxWires()) {
        if (w.port != source || w.node == node)
            continue;
        taken = true;
        changes.push_back({fxInputIndex(w.node, w.input, FxInputField::From), fxPortValue(outs[0])});
        for (size_t k = 1; k < outs.size(); ++k) // a split's other bands into free inputs beside it
            for (int input = 0; input < fxInputCount(w.node); ++input) {
                const int index = fxInputIndex(w.node, input, FxInputField::From);
                const bool changed = std::any_of(changes.begin(), changes.end(),
                                                 [&](const auto &c) { return c.first == index; });
                if (!changed && fxPortOf(value(index)) < 0) {
                    changes.push_back({index, fxPortValue(outs[k])});
                    changes.push_back({fxInputIndex(w.node, input, FxInputField::Level),
                                       value(fxInputIndex(w.node, w.input, FxInputField::Level))});
                    bowUp(fxInputIndex(w.node, input, FxInputField::Level));
                    break;
                }
            }
    }
    // Nothing went on from there: the node goes out.
    if (!taken)
        for (size_t k = 0, input = 0; k < outs.size() && input < kFxOutInputs; ++input) {
            const int index = fxInputIndex(kFxOutNode, static_cast<int>(input), FxInputField::From);
            if (fxPortOf(value(index)) < 0) {
                const int level = fxInputIndex(kFxOutNode, static_cast<int>(input), FxInputField::Level);
                if (k > 0)
                    bowUp(level);
                else
                    setWireBend(level, kDefaultWireBend);
                changes.push_back({index, fxPortValue(outs[k++])});
                changes.push_back({level, 1.0});
            }
        }
    for (int input = 0; input < fxInputCount(node); ++input) { // it takes what it goes after, and nothing else
        changes.push_back({fxInputIndex(node, input, FxInputField::From), input == 0 ? fxPortValue(source) : 0.0});
        changes.push_back({fxInputIndex(node, input, FxInputField::Level), 1.0});
    }
    // It sits right of what it goes after, clear of the other nodes.
    UiLayout &layout = controller_.layout();
    float x = afterSlot ? std::min(layout.fxNodeX[selected] + 0.2f, 1.0f) : 0.1f;
    float y = afterSlot ? layout.fxNodeY[selected] : 0.5f;
    for (int tries = 0; tries < 8; ++tries) {
        bool clear = true;
        for (int other = 0; other < kFxInputNodes && clear; ++other)
            clear = other == node || !fxNodeShown(other) ||
                    std::hypot(layout.fxNodeX[other] - x, layout.fxNodeY[other] - y) > 0.12f;
        if (clear)
            break;
        y = y + 0.3f > 1.0f ? y - 0.6f + 0.15f : y + 0.3f;
        y = std::clamp(y, 0.0f, 1.0f);
    }
    layout.fxNodeX[node] = x;
    layout.fxNodeY[node] = y;
    layout.fxSelected = node;
    controller_.layoutChanged();
}

void View::fxSyncOrder() {
    // Kahn's algorithm over the wires between the slots and splits; what a loop holds goes after, by number.
    const std::vector<FxWireRef> wires = fxWires();
    std::array<int, kFxInputNodes> waiting{};
    for (const FxWireRef &w : wires)
        if (w.node < kFxInputNodes && fxPortNode(w.port) >= 0)
            ++waiting[w.node];
    std::vector<int> order;
    std::array<bool, kFxInputNodes> done{};
    for (bool moved = true; moved;) {
        moved = false;
        for (int node = 0; node < kFxInputNodes; ++node) {
            if (done[node] || !fxNodeShown(node) || waiting[node] > 0)
                continue;
            done[node] = moved = true;
            order.push_back(node);
            for (const FxWireRef &w : wires)
                if (w.node < kFxInputNodes && fxPortNode(w.port) == node)
                    --waiting[w.node];
        }
    }
    for (int node = 0; node < kFxInputNodes; ++node)
        if (!done[node] && fxNodeShown(node))
            order.push_back(node);
    std::vector<int> chain;
    for (int node : order)
        if (node < kFxSlots)
            chain.push_back(node);
    if (chain == fxChain())
        return;
    std::vector<std::pair<int, double>> changes;
    for (const auto &[index, v] : ::fmvibe::fxOrderChanges(fxOrderWith(chain)))
        if (value(index) != v)
            changes.push_back({index, v});
    editMany(changes);
}

void View::addSplit() {
    int split = -1;
    for (int s = 0; s < kFxSplits && split < 0; ++s)
        if (!fxNodeShown(fxSplitNode(s)))
            split = s;
    if (split < 0) {
        flash(trf(Str::MsgSplitsFull, {std::to_string(kFxSplits)}));
        return;
    }
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes;
    for (const SplitParam f : {SplitParam::Bands, SplitParam::Low, SplitParam::High})
        changes.push_back({paramIndex(split, f), table_[paramIndex(split, f)].def});
    changes.push_back({paramIndex(split, SplitParam::Used), 1.0});
    editMany(changes);
    changes.clear();
    fxInsertAfterSelected(fxSplitNode(split), changes);
    editMany(changes);
    fxSyncOrder();
}

void View::removeSplit(int split) {
    const UndoScope step(controller_);
    const int node = fxSplitNode(split);
    std::vector<std::pair<int, double>> changes;
    // What went into it goes on where its bands went, if it came from one place; its own wires go.
    int into = -2;
    for (const FxWireRef &w : fxWires())
        if (w.node == node)
            into = into == -2 ? w.port : -1;
    for (const FxWireRef &w : fxWires()) {
        if (fxPortSplit(w.port) != split)
            continue;
        bool there = false; // the band's target takes it already, from another band or itself
        for (int input = 0; input < fxInputCount(w.node) && into >= 0; ++input)
            there = there || fxPortOf(value(fxInputIndex(w.node, input, FxInputField::From))) == into;
        for (const auto &c : changes)
            there = there || (c.second == fxPortValue(into) && into >= 0 &&
                              c.first >= fxInputIndex(w.node, 0, FxInputField::From) &&
                              c.first <= fxInputIndex(w.node, fxInputCount(w.node) - 1, FxInputField::From));
        if (into >= 0 && !there)
            changes.push_back({fxInputIndex(w.node, w.input, FxInputField::From), fxPortValue(into)});
        else
            fxRemoveWire(w.node, w.input, changes);
    }
    for (int input = 0; input < kFxInputs; ++input)
        fxRemoveWire(node, input, changes);
    for (const Assignment &m : assignments()) // and the modulation of its crossovers
        if (m.target == paramIndex(split, SplitParam::Low) || m.target == paramIndex(split, SplitParam::High))
            removeSlot(m.source, m.slot);
    changes.push_back({paramIndex(split, SplitParam::Used), 0.0});
    editMany(changes);
    fxSyncOrder();
}

void View::setSplitBands(int split, int bands) {
    const int param = paramIndex(split, SplitParam::Bands);
    if (std::lround(value(param)) == bands)
        return;
    const UndoScope step(controller_); // with the middle band's wires
    std::vector<std::pair<int, double>> changes{{param, static_cast<double>(bands)}};
    const int mid = fxBandPort(split, 1), high = fxBandPort(split, kFxBands - 1);
    // Wires from the middle band kept while it had two bands come back as they were.
    bool midWired = false;
    for (int node = 0; node <= kFxOutNode && !midWired; ++node)
        for (int input = 0; input < fxInputCount(node) && !midWired; ++input)
            midWired = fxNodeShown(node) && fxPortOf(value(fxInputIndex(node, input, FxInputField::From))) == mid;
    if (bands >= kFxBands && !midWired)
        for (const FxWireRef &w : fxWires()) {
            if (w.port != high)
                continue;
            for (int input = 0; input < fxInputCount(w.node); ++input) {
                const int from = fxInputIndex(w.node, input, FxInputField::From);
                const bool taken = std::any_of(changes.begin(), changes.end(),
                                               [&](const auto &c) { return c.first == from; });
                if (!taken && fxPortOf(value(from)) < 0) {
                    const int level = fxInputIndex(w.node, input, FxInputField::Level);
                    changes.push_back({from, fxPortValue(mid)});
                    changes.push_back({level, value(fxInputIndex(w.node, w.input, FxInputField::Level))});
                    setWireBend(level, 0.0f); // straight, between the high band's bowing up and the low one's down
                    break;
                }
            }
        }
    editMany(changes);
    fxSyncOrder();
}

bool View::fxGraphIsChain() const {
    for (int split = 0; split < kFxSplits; ++split)
        if (fxNodeShown(fxSplitNode(split)))
            return false;
    // A single path from the input to the output through every effect, each wire at full level.
    const std::vector<FxWireRef> wires = fxWires();
    const std::vector<int> chain = fxChain();
    if (wires.size() != chain.size() + 1)
        return false;
    int port = kFxPortIn;
    for (size_t k = 0; k <= chain.size(); ++k) {
        const int node = k < chain.size() ? chain[k] : kFxOutNode;
        const auto w = std::find_if(wires.begin(), wires.end(), [&](const FxWireRef &r) { return r.node == node; });
        if (w == wires.end() || w->port != port || value(fxInputIndex(w->node, w->input, FxInputField::Level)) != 1.0)
            return false;
        port = k < chain.size() ? fxSlotPort(chain[k]) : -1;
    }
    return true;
}

void View::setFxGraph(bool on, bool asked) {
    if (on == fxGraph())
        return;
    if (!on && !asked && !fxGraphIsChain()) {
        confirm_ = kConfirmFxChain;
        hover_.reset();
        return;
    }
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes;
    for (const FxWireRef &w : fxWires()) // the wires there were go, with their modulation
        fxRemoveWire(w.node, w.input, changes);
    for (int node = 0; node <= kFxOutNode; ++node)
        for (int input = 0; input < fxInputCount(node); ++input)
            if (value(fxInputIndex(node, input, FxInputField::From)) != 0.0)
                changes.push_back({fxInputIndex(node, input, FxInputField::From), 0.0});
    for (int split = 0; split < kFxSplits; ++split)
        if (fxNodeShown(fxSplitNode(split))) {
            for (const Assignment &m : assignments())
                if (m.target == paramIndex(split, SplitParam::Low) || m.target == paramIndex(split, SplitParam::High))
                    removeSlot(m.source, m.slot);
            changes.push_back({paramIndex(split, SplitParam::Used), 0.0});
        }
    changes.push_back({paramIndex(GlobalParam::FxGraph), on ? 1.0 : 0.0});
    if (on) { // wired as the chain ran, its nodes in a row (two past six)
        const std::vector<int> chain = fxChain();
        int port = kFxPortIn;
        UiLayout &layout = controller_.layout();
        const size_t n = chain.size(), perRow = n > 6 ? (n + 1) / 2 : n;
        for (size_t k = 0; k < n; ++k) {
            changes.push_back({fxInputIndex(chain[k], 0, FxInputField::From), fxPortValue(port)});
            changes.push_back({fxInputIndex(chain[k], 0, FxInputField::Level), 1.0});
            port = fxSlotPort(chain[k]);
            const size_t row = k / perRow, col = k % perRow;
            layout.fxNodeX[chain[k]] = (static_cast<float>(col) + 0.5f) / static_cast<float>(perRow);
            layout.fxNodeY[chain[k]] = n > 6 ? (row == 0 ? 0.2f : 0.8f) : 0.5f;
        }
        changes.push_back({fxInputIndex(kFxOutNode, 0, FxInputField::From), fxPortValue(port)});
        changes.push_back({fxInputIndex(kFxOutNode, 0, FxInputField::Level), 1.0});
        layout.fxSelected = chain.empty() ? -1 : chain.front();
        controller_.layoutChanged();
    }
    editMany(changes); // the chain keeps its order, which the graph's follows
}

// --- Widgets

void View::fxGraphWidgets(std::vector<Widget> &list) const {
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    const std::vector<FxWireRef> wires = fxWires();
    // The wires first, under the nodes: along their curves to bend them.
    for (const FxWireRef &r : wires) {
        const int level = fxInputIndex(r.node, r.input, FxInputField::Level);
        const Point to = fxNodeCenter(r.node);
        const int from = fxPortNode(r.port);
        const bool slot = from >= 0 && from < kFxSlots;
        const Wire w = fxWireShape(slot ? fxNodeCenter(from) : fxPortPoint(r.port, to), slot, r.node, to,
                                   wireBend(level));
        const float left = std::min({w.start.x, w.control.x, w.end.x}), right = std::max({w.start.x, w.control.x, w.end.x});
        const float top = std::min({w.start.y, w.control.y, w.end.y}), bottom = std::max({w.start.y, w.control.y, w.end.y});
        add(Kind::FxWire, Rect{left, top, right - left, bottom - top}.inset(-kWireGrab), level, r.node, r.input);
    }
    // The nodes, each with its edge to wire it from (a split its bands' ports), and the input's port.
    add(Kind::FxPort, inRect(), -1, kFxPortIn, -1);
    for (int node = 0; node < kFxInputNodes; ++node) {
        if (!fxNodeShown(node))
            continue;
        const Rect r = nodeRect(node, fxNodeCenter(node));
        if (node < kFxSlots) {
            add(Kind::FxPort, r.inset(-kEdgeOut), -1, fxSlotPort(node), node);
            add(Kind::FxNode, r.inset(kEdgeIn), -1, node);
        } else {
            add(Kind::FxNode, r, -1, node);
            const int split = node - kFxSlots;
            const bool three = std::lround(value(paramIndex(split, SplitParam::Bands))) >= 3;
            for (int band = 0; band < kFxBands; ++band)
                if (band != 1 || three) {
                    const Point p = fxBandPortAt(fxNodeCenter(node), band);
                    add(Kind::FxPort, {p.x - 8.0f, p.y - 8.0f, 16.0f, 16.0f}, -1, fxBandPort(split, band), node);
                }
        }
    }
    // Then the labels and arrowheads on top.
    for (const FxWireRef &r : wires) {
        const int level = fxInputIndex(r.node, r.input, FxInputField::Level);
        const Point to = fxNodeCenter(r.node);
        const int from = fxPortNode(r.port);
        const bool slot = from >= 0 && from < kFxSlots;
        const Wire w = fxWireShape(slot ? fxNodeCenter(from) : fxPortPoint(r.port, to), slot, r.node, to,
                                   wireBend(level));
        const Rect label = labelRect(w.label);
        add(Kind::FxLabel, label, level, r.node, r.input);
        const bool open = (hover_ && (hover_->kind == Kind::FxLabel || hover_->kind == Kind::FxLabelRemove) &&
                           hover_->a == r.node && hover_->b == r.input) ||
                          (drag_.kind == DragKind::Value && drag_.widget.kind == Kind::FxLabel &&
                           drag_.widget.param == level);
        if (open)
            add(Kind::FxLabelRemove, {label.right() + 2.0f, label.y, 16.0f, 16.0f}, level, r.node, r.input);
        add(Kind::FxWireEnd, arrowGrip(w.end, w.tangent), level, r.node, r.input);
    }
}

Rect View::splitDisplay(Rect card) {
    return {card.x + 14.0f, card.y + 74.0f, card.w - 28.0f, 110.0f};
}

Point View::splitHandle(int split, int which, Rect display) const {
    const std::array<double, 2> hz =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)));
    const float x = display.x + static_cast<float>(normFromCutoff(hz[which])) * display.w;
    return {std::clamp(x, display.x, display.right()), display.y + display.h * 0.35f};
}

void View::splitCardWidgets(std::vector<Widget> &list, int split, Rect card) const {
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    add(Kind::FxNodeRemove, {card.right() - 26.0f, card.y + 8.0f, 18.0f, 21.0f}, -1, fxSplitNode(split));
    // BANDS 2 | 3 under the title, the crossovers' handles on the display, then the knobs.
    const int bands = paramIndex(split, SplitParam::Bands);
    const size_t first = list.size();
    for (int count = 2; count <= kFxBands; ++count)
        add(Kind::Choice, {card.x + 14.0f + (count - 2) * 40.0f + 56.0f, card.y + 44.0f, 40.0f, 20.0f}, bands, -1,
            count);
    mergeButtons(list, first);
    const Rect display = splitDisplay(card);
    const bool three = std::lround(value(bands)) >= 3;
    for (int which = 0; which < (three ? 2 : 1); ++which) {
        const Point h = splitHandle(split, which, display);
        add(Kind::SplitPoint, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f},
            paramIndex(split, which == 0 ? SplitParam::Low : SplitParam::High), split, which);
    }
    const int params[] = {paramIndex(split, SplitParam::Low), paramIndex(split, SplitParam::High)};
    const Str labels[] = {Str::LabelLowX, Str::LabelHighX};
    for (int i = 0; i < (three ? 2 : 1); ++i) {
        Widget w;
        w.kind = Kind::Knob;
        w.rect = knobRect({card.x + card.w * (three ? 0.25f + 0.5f * i : 0.5f), card.y + 240.0f}, 36.0f);
        w.param = params[i];
        w.label = tr(labels[i]);
        list.push_back(w);
    }
}

// --- Mouse

bool View::fxGraphPress(const Widget &w, Point p) {
    UiLayout &layout = controller_.layout();
    switch (w.kind) {
    case Kind::FxMode:
        setFxGraph(w.b == 1);
        return true;
    case Kind::FxSplitAdd:
        addSplit();
        return true;
    case Kind::Choice: // a split's BANDS 2 | 3
        if (const int split = splitOf(w.param); split >= 0) {
            setSplitBands(split, w.b);
            return true;
        }
        return false;
    case Kind::FxNode:
        layout.fxSelected = w.a;
        drag_.kind = DragKind::FxNode;
        drag_.step = true; // moving it is an undo step
        controller_.beginStep();
        return true;
    case Kind::FxPort:
        drag_.kind = DragKind::FxConnect;
        return true;
    case Kind::FxWireEnd:
        drag_.kind = DragKind::FxRewire;
        return true;
    case Kind::FxWire:
        drag_.kind = DragKind::FxBend;
        drag_.startValue = wireBend(w.param);
        drag_.step = true;
        controller_.beginStep();
        return true;
    case Kind::FxLabelRemove: {
        const UndoScope step(controller_);
        std::vector<std::pair<int, double>> changes;
        fxRemoveWire(w.a, w.b, changes);
        editMany(changes);
        fxSyncOrder();
        hover_.reset();
        return true;
    }
    case Kind::FxNodeRemove:
        removeSplit(w.a - kFxSlots);
        hover_.reset();
        return true;
    case Kind::SplitPoint:
        drag_.kind = DragKind::SplitPoint;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        fxGraphDrag(p);
        return true;
    default:
        return false;
    }
}

void View::fxGraphDrag(Point p) {
    switch (drag_.kind) {
    case DragKind::FxNode: {
        const Point start = fxNodeCenter(drag_.widget.a);
        (void)start;
        const Point norm = fxNodeNorm({p.x, p.y});
        UiLayout &layout = controller_.layout();
        layout.fxNodeX[drag_.widget.a] = norm.x;
        layout.fxNodeY[drag_.widget.a] = norm.y;
        controller_.layoutChanged();
        break;
    }
    case DragKind::FxBend: {
        // As far to the wire's left as the mouse is from the line between its ends, as a share of their distance.
        const int node = drag_.widget.a;
        const int port = fxPortOf(value(fxInputIndex(node, drag_.widget.b, FxInputField::From)));
        if (port < 0)
            break;
        const Point to = fxNodeCenter(node);
        const int from = fxPortNode(port);
        const Point a = from >= 0 && from < kFxSlots ? fxNodeCenter(from) : fxPortPoint(port, to);
        const Point left = leftOf(a, to);
        const float span = std::max(std::hypot(to.x - a.x, to.y - a.y), 1.0f);
        const Point mid{(a.x + to.x) * 0.5f, (a.y + to.y) * 0.5f};
        // A quadratic curve's middle is halfway to its control point.
        const float reach = 2.0f * ((p.x - mid.x) * left.x + (p.y - mid.y) * left.y);
        setWireBend(drag_.widget.param, std::clamp(reach / span, -kMaxWireBend, kMaxWireBend));
        break;
    }
    case DragKind::SplitPoint: {
        const int split = drag_.widget.a;
        const auto card = fxInspector(fxSplitNode(split));
        const Rect display = splitDisplay(card);
        const double norm = std::clamp((p.x - display.x) / display.w, 0.0f, 1.0f);
        controller_.setValue(drag_.widget.param, norm);
        break;
    }
    default:
        break;
    }
}

void View::fxGraphRelease(Point p) {
    switch (drag_.kind) {
    case DragKind::FxConnect:
        if (drag_.moved)
            fxConnect(drag_.widget.a, fxNodeAtPoint(p));
        break;
    case DragKind::FxRewire:
        if (drag_.moved) {
            const int into = fxNodeAtPoint(p);
            if (into != drag_.widget.a)
                fxRewire(drag_.widget.a, drag_.widget.b, into);
        }
        break;
    default:
        break;
    }
}

// --- Painting

void View::paintFxGraph(Canvas &c) {
    const Rect area = fxGraphArea();
    c.fillRect(area, kDisplayColor, 8.0f);
    const std::vector<FxWireRef> wires = fxWires();
    const int selected = fxSelectedNode();
    const bool rewiring = drag_.kind == DragKind::FxRewire && drag_.moved;
    if (wires.empty() && fxChain().empty())
        c.text({area.x + 60.0f, area.center().y - 40.0f, area.w - 120.0f, 20.0f}, tr(Str::HintFxGraph), 10.5f, kFaint);

    auto portColor = [&](int port) {
        if (port == kFxPortIn)
            return kDim;
        if (const int band = fxPortBand(port); band >= 0)
            return bandColor(band);
        return fxColor(fxPortSlot(port));
    };
    // The wires, each in the color of what it comes from, with its arrowhead and its level.
    for (const FxWireRef &r : wires) {
        const int level = fxInputIndex(r.node, r.input, FxInputField::Level);
        const bool moving = rewiring && drag_.widget.a == r.node && drag_.widget.b == r.input;
        const Point to = moving ? drag_.now : fxNodeCenter(r.node);
        const int from = fxPortNode(r.port);
        const bool slot = from >= 0 && from < kFxSlots;
        const Wire w = bentWireBy(slot ? fxNodeCenter(from) : fxPortPoint(r.port, to), to,
                                  moving ? 0.0f : wireBend(level), [slot](Point) { return slot ? kFxNodeR : 0.0f; },
                                  [&](Point dir) { return moving ? 0.0f : nodeExtentOf(r.node, dir); });
        const double amount = value(level);
        const bool lit = (hover_ && (hover_->kind == Kind::FxWire || hover_->kind == Kind::FxLabel ||
                                     hover_->kind == Kind::FxWireEnd) &&
                          hover_->a == r.node && hover_->b == r.input) ||
                         (drag_.kind == DragKind::FxBend && drag_.widget.a == r.node && drag_.widget.b == r.input);
        const Color color = portColor(r.port);
        c.curve(w.start, w.control, w.end, color.alpha(amount > 0.0 ? 0.45f + 0.5f * static_cast<float>(amount) : 0.4f),
                lit ? 2.4f : 1.6f, amount <= 0.0);
        arrowHead(c, w.end, w.tangent, color);
        if (moving)
            continue;
        const Rect label = labelRect(w.label);
        const bool hot = hover_ && (hover_->kind == Kind::FxLabel || hover_->kind == Kind::FxLabelRemove) &&
                         hover_->a == r.node && hover_->b == r.input;
        c.fillRect(label, hot ? kRaised : kPanel, 8.0f);
        c.strokeRect(label, color.alpha(hot ? 0.9f : 0.5f), 1.0f, 8.0f);
        c.text(label, percent(amount), 9.0f, kText);
        if (hot || (drag_.kind == DragKind::Value && drag_.widget.param == level)) {
            const Rect x{label.right() + 2.0f, label.y, 16.0f, 16.0f};
            const bool overX = hover_ && hover_->kind == Kind::FxLabelRemove;
            c.fillRect(x, overX ? kTrack : kPanel, 8.0f);
            const Point m = x.center();
            c.line({m.x - 3.5f, m.y - 3.5f}, {m.x + 3.5f, m.y + 3.5f}, overX ? kText : kDim, 1.4f);
            c.line({m.x + 3.5f, m.y - 3.5f}, {m.x - 3.5f, m.y + 3.5f}, overX ? kText : kDim, 1.4f);
        }
    }
    // A wire being drawn from a port, the node under the mouse ringed if it can take it.
    if (drag_.kind == DragKind::FxConnect && drag_.moved) {
        const int port = drag_.widget.a;
        const Point start = fxPortPoint(port, drag_.now);
        c.line(start, drag_.now, portColor(port), 1.6f, true);
    }
    if ((drag_.kind == DragKind::FxConnect || rewiring) && drag_.moved) {
        const int target = fxNodeAtPoint(drag_.now);
        if (target >= 0 && target != drag_.widget.a)
            c.strokeRect(nodeRect(target, fxNodeCenter(target)).inset(-5.0f), kAccent, 1.5f,
                         target < kFxSlots ? kFxNodeR + 5.0f : 8.0f);
    }

    // The input and the output.
    auto endBox = [&](Rect r, Str name, bool hot) {
        c.fillRect(r, kCardColor, 6.0f);
        c.strokeRect(r, hot ? kText : kBorder, hot ? 1.5f : 1.0f, 6.0f);
        c.text(r, tr(name), 10.0f, kDim, Align::Center, true);
    };
    endBox(inRect(), Str::NodeFxIn,
           (hover_ && hover_->kind == Kind::FxPort && hover_->a == kFxPortIn) ||
               (drag_.kind == DragKind::FxConnect && drag_.widget.a == kFxPortIn));
    endBox(nodeRect(kFxOutNode, fxOutCenter()), Str::NodeFxOut, false);

    // The nodes: an effect a circle with its icon and its name under it, a split a box with its bands' ports.
    for (int node = 0; node < kFxInputNodes; ++node) {
        if (!fxNodeShown(node))
            continue;
        const Point center = fxNodeCenter(node);
        const bool isSelected = node == selected;
        const bool edge = (hover_ && hover_->kind == Kind::FxPort && hover_->b == node) ||
                          (drag_.kind == DragKind::FxConnect && drag_.widget.b == node);
        if (node < kFxSlots) {
            const FxType type = fxSlotType(node);
            const bool on = value(paramIndex(node, FxParam::Enabled)) >= 0.5;
            const Color color = on ? fxTypeColor(type) : kFaint;
            if (isSelected)
                c.strokeCircle(center, kFxNodeR + 5.0f, color.alpha(0.5f), 1.5f);
            c.fillCircle(center, kFxNodeR, isSelected ? color.alpha(kSelectedFill) : kCardColor);
            c.strokeCircle(center, kFxNodeR, edge ? kText : color, edge ? 2.2f : 1.6f);
            fxIcon(c, {center.x - 10.0f, center.y - 7.0f, 20.0f, 14.0f}, type, color);
            c.text({center.x - 60.0f, center.y + kFxNodeR + 3.0f, 120.0f, 14.0f}, fxTitle(type), 9.5f,
                   isSelected ? kText : kDim, Align::Center, true);
        } else {
            const int split = node - kFxSlots;
            const Rect r = nodeRect(node, center);
            const Color color = splitColor();
            if (isSelected)
                c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, 9.0f);
            c.fillRect(r, isSelected ? color.alpha(kSelectedFill) : kCardColor, 6.0f);
            c.strokeRect(r, color, 1.6f, 6.0f);
            splitIcon(c, {r.x + 8.0f, center.y - 7.0f, 20.0f, 14.0f}, color);
            c.text({center.x - 60.0f, r.bottom() + 3.0f, 120.0f, 14.0f},
                   trf(Str::NodeSplit, {std::to_string(split + 1)}), 9.5f, isSelected ? kText : kDim, Align::Center,
                   true);
            const bool three = std::lround(value(paramIndex(split, SplitParam::Bands))) >= 3;
            for (int band = 0; band < kFxBands; ++band) {
                if (band == 1 && !three)
                    continue;
                const Point port = fxBandPortAt(center, band);
                const bool hot = (hover_ && hover_->kind == Kind::FxPort && hover_->a == fxBandPort(split, band)) ||
                                 (drag_.kind == DragKind::FxConnect && drag_.widget.a == fxBandPort(split, band));
                c.fillCircle(port, kFxPortR + (hot ? 1.5f : 0.0f), bandColor(band));
                c.strokeCircle(port, kFxPortR + (hot ? 1.5f : 0.0f), kCardColor, 1.2f);
            }
        }
    }

    // The node it shows, in the inspector.
    if (selected >= 0 && selected < kFxSlots)
        paintFxCard(c, selected, fxInspector(selected), false);
    else if (selected >= kFxSlots)
        paintSplitCard(c, selected - kFxSlots, fxInspector(selected));
    else {
        const Rect card = fxInspectorCard();
        c.strokeRect(card, kBorder, 1.0f, 8.0f, true);
        c.text(card, tr(Str::HintFxInspector), 10.5f, kFaint);
    }
}

void View::paintSplitCard(Canvas &c, int split, Rect card) {
    const Color color = splitColor();
    c.fillRect(card, kCardColor, 8.0f);
    c.strokeRect(card, color.alpha(0.45f), 1.0f, 8.0f);
    splitIcon(c, fxIconRect(card), color);
    c.text({card.x + 36.0f, card.y + 6.0f, card.w - 84.0f, 20.0f}, trf(Str::NodeSplit, {std::to_string(split + 1)}),
           11.0f, kText, Align::Left, true);
    c.text({card.x + 14.0f, card.y + 44.0f, 52.0f, 20.0f}, tr(Str::LabelBands), 9.5f, kDim, Align::Left, true);
    // The bands' responses on the cutoff grid, each in its color, the crossovers' handles where they cut.
    const Rect display = splitDisplay(card);
    c.fillRect(display, kDisplayColor, 6.0f);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
        c.line({x, display.y + 4.0f}, {x, display.bottom() - 4.0f}, kBorder, 1.0f, true);
    }
    const std::array<double, 2> hz =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)));
    const bool three = std::lround(value(paramIndex(split, SplitParam::Bands))) >= 3;
    auto lowPass = [](double f, double corner) { return 1.0 / std::sqrt(1.0 + std::pow(f / corner, 8.0)); };
    auto highPass = [](double f, double corner) { return 1.0 / std::sqrt(1.0 + std::pow(corner / f, 8.0)); };
    const float top = display.y + display.h * 0.35f, span = display.h * 0.55f;
    for (int band = 0; band < kFxBands; ++band) {
        if (band == 1 && !three)
            continue;
        std::vector<Point> points;
        for (int k = 0; k <= 120; ++k) {
            const double norm = k / 120.0, f = cutoffFromNorm(norm);
            double m = 1.0;
            if (band == 0)
                m = lowPass(f, hz[0]);
            else if (band == 1)
                m = highPass(f, hz[0]) * lowPass(f, hz[1]);
            else
                m = highPass(f, three ? hz[1] : hz[0]);
            points.push_back({display.x + static_cast<float>(norm) * display.w,
                              top + static_cast<float>(1.0 - m) * span - (1.0f - 0.707f) * span});
        }
        c.polyline(points.data(), points.size(), bandColor(band), 1.8f);
    }
    for (int which = 0; which < (three ? 2 : 1); ++which) {
        const Point h = splitHandle(split, which, display);
        c.line({h.x, display.y + 2.0f}, {h.x, display.bottom() - 2.0f}, kText.alpha(0.3f), 1.0f);
        const bool hot = (hover_ && hover_->kind == Kind::SplitPoint && hover_->b == which) ||
                         (drag_.kind == DragKind::SplitPoint && drag_.widget.b == which);
        c.fillCircle(h, hot ? 7.0f : 6.0f, kDisplayColor);
        c.strokeCircle(h, hot ? 7.0f : 6.0f, kText, 1.6f);
        char text[24];
        const double f = hz[which];
        std::snprintf(text, sizeof(text), f >= 1000.0 ? "%.1f kHz" : "%.0f Hz", f >= 1000.0 ? f / 1000.0 : f);
        c.text({h.x - 40.0f, display.bottom() - 18.0f, 80.0f, 14.0f}, text, 9.0f, kText);
    }
}

} // namespace fmvibe::gui
