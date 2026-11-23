// The effects as a line of effects and splits from IN to OUT, which the editor reads from the wires and writes back to
// them, and the edits that change it. The engine runs the wires as they are (see Effects::processGraph()); the editor
// only ever wires a line. Its two views, the row and the graph, are the FX view's (fxpanel.cpp).

#include "fxgraphmodel.h"

#include "fxgraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

namespace fmvibe::gui {

using namespace detail;

// --- Reading and writing the line

FxType FxGraphModel::slotType(int slot) const {
    return fxType(value(paramIndex(slot, FxParam::Type)));
}

bool FxGraphModel::nodeShown(int node) const {
    if (node == kFxOutNode)
        return true;
    if (node < 0 || node >= kFxNodes)
        return false;
    if (node < kFxSlots)
        return slotType(node) != FxType::None;
    if (const int sum = fxNodeSum(node); sum >= 0)
        return value(paramIndex(sum, SumParam::Used)) >= 0.5;
    return value(paramIndex(node - kFxSlots, SplitParam::Used)) >= 0.5;
}

std::vector<FxWireRef> FxGraphModel::wires() const {
    std::vector<FxWireRef> wires;
    for (int node = 0; node < kFxNodes; ++node) {
        if (!nodeShown(node))
            continue;
        for (int input = 0; input < fxInputCount(node); ++input) {
            const int port = fxPortOf(value(fxInputIndex(node, input, FxInputField::From)));
            if (port < 0)
                continue;
            const int from = fxPortNode(port);
            if (from >= 0 && !nodeShown(from))
                continue;
            // A split of two bands has no middle one.
            if (fxPortBand(port) == 1 && std::lround(value(paramIndex(fxPortSplit(port), SplitParam::Bands))) < 3)
                continue;
            wires.push_back({node, input, port});
        }
    }
    return wires;
}

FxLine FxGraphModel::line() const {
    const std::vector<FxWireRef> wires = this->wires();
    // Where a port goes: the first node that takes it other than the output, else the output, else nowhere.
    auto into = [&](int port) {
        int out = -1;
        for (const FxWireRef &w : wires)
            if (w.port == port) {
                if (w.node != kFxOutNode)
                    return w.node;
                out = kFxOutNode;
            }
        return out;
    };
    std::vector<bool> taken(kFxNodes, false);
    // A band's effects, from its port on as far as effects take it; where it goes after them.
    auto band = [&](int port, std::vector<int> &slots) {
        for (int node = into(port); node >= 0 && node < kFxSlots && !taken[node]; node = into(port)) {
            taken[node] = true;
            slots.push_back(node);
            port = fxSlotPort(node);
        }
        return into(port);
    };
    FxLine line;
    int port = kFxPortIn;
    for (int node = into(port); node >= 0 && node != kFxOutNode && !taken[node]; node = into(port)) {
        if (node < kFxSlots) {
            taken[node] = true;
            line.push_back({node, {}, {}, false});
            port = fxSlotPort(node);
            continue;
        }
        if (!fxNodeSplit(node)) // a sum where the line goes on: not a line the grid draws
            break;
        taken[node] = true;
        const int split = node - kFxSlots;
        FxLineItem item{node, {}, {}, false};
        item.three = splitThree(split);
        const int high = band(fxBandPort(split, kHighBand), item.hi);
        const int low = band(fxBandPort(split, kLowBand), item.lo);
        const int mid = item.three ? band(fxBandPort(split, 1), item.mid) : high;
        item.joined = high >= 0 && high == low && high == mid && fxNodeSum(high) >= 0 && !taken[high];
        line.push_back(item);
        if (!item.joined) // open: the line ends here
            break;
        taken[high] = true;
        port = fxSumPort(fxNodeSum(high));
    }
    // The effects the wires leave out go at its end, by number, on the high band while a split is open.
    const bool open = openSplit(line);
    for (int slot = 0; slot < kFxSlots; ++slot)
        if (!taken[slot] && slotType(slot) != FxType::None) {
            if (open)
                line.back().hi.push_back(slot);
            else
                line.push_back({slot, {}, {}, false});
        }
    return line;
}

void FxGraphModel::writeLine(const FxLine &line) {
    std::map<int, double> want;
    std::array<bool, kFxSplits> used{}, joined{}, three{};
    for (const FxLineItem &item : line)
        if (fxNodeSplit(item.node)) {
            used[item.node - kFxSlots] = true;
            joined[item.node - kFxSlots] = item.joined;
            three[item.node - kFxSlots] = item.three;
        }
    // Every wire goes, at full level, but for a join's bands, which keep theirs. A level reset loses its
    // modulation: the grid shows no other wire's level.
    std::vector<int> reset;
    for (int node = 0; node < kFxNodes; ++node)
        for (int input = 0; input < fxInputCount(node); ++input) {
            want[fxInputIndex(node, input, FxInputField::From)] = 0.0;
            const int sum = fxNodeSum(node);
            if (sum >= 0 && joined[sum] && input < (three[sum] ? 3 : 2))
                continue;
            want[fxInputIndex(node, input, FxInputField::Level)] = table_[fxInputIndex(node, input, FxInputField::Level)].def;
            reset.push_back(fxInputIndex(node, input, FxInputField::Level));
        }
    auto wire = [&](int node, int input, int port) {
        want[fxInputIndex(node, input, FxInputField::From)] = fxPortValue(port);
    };
    int port = kFxPortIn;
    for (const FxLineItem &item : line) {
        if (item.node < kFxSlots) {
            wire(item.node, 0, port);
            port = fxSlotPort(item.node);
            continue;
        }
        const int split = item.node - kFxSlots;
        wire(item.node, 0, port);
        for (int lane : item.lanes()) { // each band from its port through its effects into the join, or out
            int band = fxBandPort(split, laneBand(lane));
            for (int slot : item.band(lane)) {
                wire(slot, 0, band);
                band = fxSlotPort(slot);
            }
            wire(item.joined ? fxJoinNode(split) : kFxOutNode, laneJoinInput(lane), band);
        }
        port = item.joined ? fxSumPort(split) : -1; // open, the line's last: its bands go out
    }
    if (port >= 0)
        wire(kFxOutNode, 0, port);
    for (int split = 0; split < kFxSplits; ++split) {
        want[paramIndex(split, SplitParam::Used)] = used[split] ? 1.0 : 0.0;
        want[paramIndex(split, SplitParam::Bands)] = used[split] && three[split] ? 3.0 : 2.0;
        want[paramIndex(split, SumParam::Used)] = joined[split] ? 1.0 : 0.0;
        if (!joined[split]) // a join taken out starts again unsoloed
            for (const SumParam f : {SumParam::SoloHigh, SumParam::SoloLow, SumParam::SoloMid})
                want[paramIndex(split, f)] = 0.0;
        else if (!three[split]) // and a middle band gone takes its solo with it
            want[paramIndex(split, SumParam::SoloMid)] = 0.0;
    }
    // What modulated a level reset, or the crossover of a split no longer in use, goes (an empty modulation onto one
    // losing its target).
    auto gone = [&](int target) {
        const int split = splitOf(target);
        const bool crossover = split >= 0 && !used[split] &&
                               (target == paramIndex(split, SplitParam::Low) ||
                                target == paramIndex(split, SplitParam::High));
        return crossover || std::find(reset.begin(), reset.end(), target) != reset.end();
    };
    for (const Assignment &m : modulation().assignments())
        if (gone(m.target))
            modulation().removeSlot(m.source, m.slot);
    modulation().forgetEmptyTargets(gone);
    std::vector<std::pair<int, double>> changes;
    for (const auto &[index, v] : want)
        if (value(index) != v)
            changes.push_back({index, v});
    editMany(changes);
}

std::vector<int> FxGraphModel::chain(const FxLine &line) {
    std::vector<int> chain;
    for (const FxLineItem &item : line) {
        if (item.node < kFxSlots)
            chain.push_back(item.node);
        for (int lane : item.lanes())
            chain.insert(chain.end(), item.band(lane).begin(), item.band(lane).end());
    }
    return chain;
}

FxLine FxGraphModel::lineWith(FxLine line, const FxLineItem &item, FxSlotAt at) {
    if (at.lane == 0) {
        line.insert(line.begin() + std::clamp(at.item, 0, static_cast<int>(line.size())), item);
    } else if (at.item >= 0 && at.item < static_cast<int>(line.size())) {
        std::vector<int> &band = line[at.item].band(at.lane);
        band.insert(band.begin() + std::clamp(at.place, 0, static_cast<int>(band.size())), item.node);
    }
    return line;
}

bool FxGraphModel::bandSoloedOut(const FxLineItem &item, int lane) const {
    if (!fxNodeSplit(item.node) || !item.joined)
        return false;
    const int sum = item.node - kFxSlots; // a split's join is the sum of its number
    bool any = false;
    for (int l : item.lanes())
        any = any || value(paramIndex(sum, laneSolo(l))) >= 0.5;
    return any && value(paramIndex(sum, laneSolo(lane))) < 0.5;
}

// --- Effects

int FxGraphModel::freeSlot() const {
    for (int s = 0; s < kFxSlots; ++s)
        if (slotType(s) == FxType::None)
            return s;
    return -1;
}

void FxGraphModel::fillEffect(int slot, FxType type, int copyOf) {
    // The slot gets its settings, the original's for a copy, then its type last: that makes the host rescan
    // the slot's fields, which by then hold what they should.
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < kFxParamCount; ++f) {
        const int index = paramIndex(slot, static_cast<FxParam>(f));
        if (static_cast<FxParam>(f) != FxParam::Type)
            changes.push_back(
                {index, copyOf >= 0 ? value(paramIndex(copyOf, static_cast<FxParam>(f))) : table_[index].def});
    }
    for (int f = 0; f < kShapeParamCount; ++f) { // the shaper's drawn curve
        const int index = clipCurveCountIndex(slot) + f;
        changes.push_back({index, copyOf >= 0 ? value(clipCurveCountIndex(copyOf) + f) : table_[index].def});
    }
    changes.push_back({paramIndex(slot, FxParam::Type), static_cast<double>(type)});
    setFolded(true, slot, false);
    editMany(changes);
}

int FxGraphModel::addEffect(FxType type, int copyOf, FxSlotAt at) {
    const UndoScope step(controller_); // with its wires
    const int slot = freeSlot();
    if (slot < 0) {
        say(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        return -1;
    }
    fillEffect(slot, type, copyOf);
    // Where the list was opened, after the node the graph shows, or a copy after its original.
    placeNew(slot, copyOf, at);
    return slot;
}

void FxGraphModel::placeNew(int slot, int copyOf, FxSlotAt where) {
    FxLine line = this->line();
    // The line lists it at its end already, as an effect the wires leave out: it goes where it was asked for instead.
    for (FxLineItem &item : line)
        for (int lane : {1, 2, 3}) {
            std::vector<int> &band = item.band(lane);
            band.erase(std::remove(band.begin(), band.end(), slot), band.end());
        }
    line.erase(std::remove_if(line.begin(), line.end(), [slot](const FxLineItem &i) { return i.node == slot; }),
               line.end());
    // Into the band at `lane` of item `i`, at `at`, or on the main line at `i`.
    auto put = [&](int i, int lane, int at) {
        // A band asked for that the line no longer has (the list stayed open while an undo or a state load took
        // the split away): at the line's end instead.
        if (lane != 0 && (i < 0 || i >= static_cast<int>(line.size()) || !fxNodeSplit(line[i].node) ||
                          (lane == 3 && !line[i].three) || lane < 0 || lane > 3)) {
            lane = 0;
            i = static_cast<int>(line.size()) - (openSplit(line) ? 1 : 0);
        }
        if (lane == 0) {
            line.insert(line.begin() + std::clamp(i, 0, static_cast<int>(line.size())), FxLineItem{slot, {}, {}, false});
            return;
        }
        std::vector<int> &band = line[i].band(lane);
        band.insert(band.begin() + std::clamp(at, 0, static_cast<int>(band.size())), slot);
    };
    const bool open = openSplit(line);
    const int last = static_cast<int>(line.size()) - 1;
    const bool graph = controller_.layout().fxGraphView;
    if (const LinePlace was = copyOf >= 0 ? placeOf(line, copyOf) : LinePlace{}; was.item >= 0 && graph) {
        put(was.lane == 0 ? was.item + 1 : was.item, was.lane, was.at + 1); // a copy right after its original
    } else if (was.item >= 0) { // in the row, at the line's end, or its band's
        if (was.lane == 0)
            put(open ? last : last + 1, 0, 0);
        else
            put(was.item, was.lane, static_cast<int>(line[was.item].band(was.lane).size()));
    } else if (where.lane >= 0 && where.item >= 0) { // a wire's middle
        put(where.item, where.lane, where.place);
    } else if (where.lane == 0 || (where.lane > 0 && !open)) { // the main line's +
        put(open ? last : last + 1, 0, 0);
    } else if (where.lane > 0) { // a band's +
        put(last, where.lane, static_cast<int>(line[last].band(where.lane).size()));
    } else if (const int selected = controller_.layout().fxSelected;
               selected >= 0 && selected != slot && placeOf(line, selected).item >= 0) {
        // After the node picked: an effect in its band or on the main line, a join on the main line, a split at the
        // end of its shortest band (the highest of those as short).
        const LinePlace at = placeOf(line, selected);
        if (fxNodeSplit(selected)) {
            const FxLineItem &item = line[at.item];
            int lane = 1;
            for (int l : item.lanes())
                if (item.band(l).size() < item.band(lane).size())
                    lane = l;
            put(at.item, lane, static_cast<int>(item.band(lane).size()));
        } else {
            put(at.lane == 0 ? at.item + 1 : at.item, at.lane, at.at + 1);
        }
    } else if (open) { // none picked: at the end, on the high band while a split is open
        put(last, 1, static_cast<int>(line[last].hi.size()));
    } else {
        put(last + 1, 0, 0);
    }
    writeLine(line);
    controller_.layout().fxSelected = slot;
    controller_.layoutChanged();
}

void FxGraphModel::removeEffect(int slot) {
    const UndoScope step(controller_);
    setFolded(true, slot, false);
    // Modulation of its knobs goes with it, and the MODS view's empty modulations onto them lose their target.
    for (const Assignment &m : modulation().assignments())
        if (fxSlotOf(m.target) == slot)
            modulation().removeSlot(m.source, m.slot);
    modulation().forgetEmptyTargets([slot](int target) { return fxSlotOf(target) == slot; });
    // Out of the line, which closes up behind it.
    FxLine line = this->line();
    for (FxLineItem &item : line)
        for (int lane : {1, 2, 3}) {
            std::vector<int> &band = item.band(lane);
            band.erase(std::remove(band.begin(), band.end(), slot), band.end());
        }
    line.erase(std::remove_if(line.begin(), line.end(), [slot](const FxLineItem &i) { return i.node == slot; }),
               line.end());
    writeLine(line);
    edit(paramIndex(slot, FxParam::Type), static_cast<double>(FxType::None));
}

bool FxGraphModel::anyToClear() const {
    const FxLine line = this->line();
    return !line.empty();
}

std::vector<int> FxGraphModel::clear() {
    const UndoScope step(controller_);
    for (const Assignment &m : modulation().assignments()) // the modulation of their knobs goes with them
        if (fxSlotOf(m.target) >= 0)
            modulation().removeSlot(m.source, m.slot);
    modulation().forgetEmptyTargets([](int target) { return fxSlotOf(target) >= 0; });
    const std::vector<int> chain = this->chain();
    for (int slot : chain)
        setFolded(true, slot, false);
    writeLine({}); // IN straight out, every split and join out of use
    std::vector<std::pair<int, double>> changes;
    for (int slot : chain)
        changes.push_back({paramIndex(slot, FxParam::Type), static_cast<double>(FxType::None)});
    editMany(changes);
    controller_.layout().fxSelected = -1;
    controller_.layoutChanged();
    return chain;
}

// --- Splits

int FxGraphModel::addSplit(int bands, FxSlotAt at) {
    FxLine line = this->line();
    // With its join right after it, its bands empty: where a wire's middle on the main line was clicked, else at
    // the line's end.
    const bool between = at.lane == 0 && at.item >= 0;
    if (!between && openSplit(line))
        return -1; // the main line has no + while a split is open (only an earlier patch leaves one so)
    int split = -1;
    for (int s = 0; s < kFxSplits && split < 0; ++s)
        if (placeOf(line, fxSplitNode(s)).item < 0)
            split = s;
    if (split < 0) {
        say(trf(Str::MsgSplitsFull, {std::to_string(kFxSplits)}));
        return -1;
    }
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes; // a fresh split's crossovers
    for (const SplitParam f : {SplitParam::Low, SplitParam::High})
        changes.push_back({paramIndex(split, f), table_[paramIndex(split, f)].def});
    for (int input = 0; input < 3; ++input) // and its join's band levels
        changes.push_back({fxInputIndex(fxJoinNode(split), input, FxInputField::Level), 0.0}); // 0 dB
    if (bands >= 3 && filterSlope(value(paramIndex(split, SplitParam::Slope))) == 0) // three bands have no 6 dB
        changes.push_back({paramIndex(split, SplitParam::Slope), 1.0});
    editMany(changes);
    FxLineItem item{fxSplitNode(split), {}, {}, true};
    item.three = bands >= 3;
    line.insert(line.begin() + (between ? std::clamp(at.item, 0, static_cast<int>(line.size()))
                                        : static_cast<int>(line.size())),
                item);
    writeLine(line);
    controller_.layout().fxSelected = fxSplitNode(split);
    controller_.layoutChanged();
    return fxSplitNode(split);
}

std::optional<FxLineItem> FxGraphModel::itemOf(int node) const {
    const FxLine line = this->line();
    const LinePlace at = placeOf(line, node);
    if (at.item < 0)
        return std::nullopt;
    return line[at.item];
}

std::vector<int> FxGraphModel::removeNode(int node, bool withEffects) {
    if (node >= 0 && node < kFxSlots) {
        removeEffect(node);
        return {node};
    }
    FxLine line = this->line();
    const LinePlace at = placeOf(line, node); // a join's is its split's: it goes with it
    if (at.item < 0)
        return {};
    const FxLineItem split = line[at.item];
    std::vector<int> held; // its bands' effects: the high band's first (then a middle band's)
    for (int lane : split.lanes())
        for (int slot : split.band(lane))
            held.push_back(slot);
    // A split, or its join: its bands' effects go on the main line in its place, or with it.
    const UndoScope step(controller_);
    line.erase(line.begin() + at.item);
    if (!withEffects) {
        std::vector<FxLineItem> kept;
        for (int slot : held)
            kept.push_back({slot, {}, {}, false});
        line.insert(line.begin() + at.item, kept.begin(), kept.end());
    }
    writeLine(line);
    if (!withEffects)
        return {};
    for (int slot : held) // removed, with their modulation
        removeEffect(slot);
    return held;
}

std::vector<int> FxGraphModel::otherBands(int slot) const {
    const FxLine line = this->line();
    const LinePlace at = placeOf(line, slot);
    std::vector<int> lanes;
    if (at.item < 0 || at.lane == 0)
        return lanes;
    for (int lane : {2, 3, 1}) // low, middle, high
        if (lane != at.lane && (lane != 3 || line[at.item].three))
            lanes.push_back(lane);
    return lanes;
}

void FxGraphModel::moveToBand(int slot, int lane) {
    FxLine line = this->line();
    const LinePlace at = placeOf(line, slot);
    if (at.item < 0 || at.lane == 0 || at.lane == lane || (lane == 3 && !line[at.item].three))
        return;
    const UndoScope step(controller_);
    std::vector<int> &from = line[at.item].band(at.lane);
    from.erase(from.begin() + at.at);
    line[at.item].band(lane).push_back(slot);
    writeLine(line);
}

bool FxGraphModel::splitThree(int split) const {
    return split >= 0 && split < kFxSplits && std::lround(value(paramIndex(split, SplitParam::Bands))) >= 3;
}

std::vector<int> FxGraphModel::middleEffects(int split) const {
    const std::optional<FxLineItem> item = itemOf(fxSplitNode(split));
    return item ? item->mid : std::vector<int>{};
}

std::vector<int> FxGraphModel::setSplitBands(int split, int bands, int midTo) {
    FxLine line = this->line();
    const LinePlace at = placeOf(line, fxSplitNode(split));
    if (at.item < 0)
        return {};
    FxLineItem &item = line[at.item];
    const bool three = bands >= 3;
    if (three == item.three)
        return {};
    const UndoScope step(controller_);
    item.three = three;
    if (three) {
        // The high crossover at its 2500 Hz, or two octaves above the low one where that is higher (as high as it
        // goes), so the low crossover's own 250 Hz gives 250 and 2500; the middle band in at 0 dB, shown.
        const double low =
            splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)))[0];
        const double high =
            std::max(table_[paramIndex(split, SplitParam::High)].def, normFromCutoff(std::min(4.0 * low, kMaxCutoff)));
        std::vector<std::pair<int, double>> changes{
            {paramIndex(split, SplitParam::High), high},
            {fxInputIndex(fxJoinNode(split), laneJoinInput(3), FxInputField::Level), 0.0}};
        // Its slope kept, but 6 dB, which three bands don't have: 12 dB, the nearest they do.
        if (filterSlope(value(paramIndex(split, SplitParam::Slope))) == 0)
            changes.push_back({paramIndex(split, SplitParam::Slope), 1.0});
        editMany(changes);
        writeLine(line);
        showSplitBand(split, tabOfLane(3));
        return {};
    }
    const std::vector<int> mid = std::move(item.mid);
    item.mid.clear();
    if (midTo == 2)
        item.lo.insert(item.lo.end(), mid.begin(), mid.end());
    else if (midTo == 1)
        item.hi.insert(item.hi.begin(), mid.begin(), mid.end());
    writeLine(line);
    if (midTo != 1 && midTo != 2)
        for (int slot : mid) // removed, with their modulation
            removeEffect(slot);
    if (splitLane(split) == 2 && controller_.layout().splitBand[split] == tabOfLane(3)) // the middle one was shown
        showSplitBand(split, midTo == 1 ? tabOfLane(1) : tabOfLane(2));
    return midTo != 1 && midTo != 2 ? mid : std::vector<int>{};
}

int FxGraphModel::splitLane(int split) const {
    const int tab = controller_.layout().splitBand[split];
    return tab == 2 && !splitThree(split) ? 2 : laneOfTab(tab);
}

void FxGraphModel::showSplitBand(int split, int band) {
    if (controller_.layout().splitBand[split] == band)
        return;
    controller_.layout().splitBand[split] = band;
    controller_.layoutChanged();
}

bool FxGraphModel::splitFolded(int split) const {
    return (controller_.layout().splitsFolded >> split) & 1u;
}

void FxGraphModel::setSplitFolded(int split, bool folded) {
    uint32_t &bits = controller_.layout().splitsFolded;
    const uint32_t before = bits;
    bits = folded ? bits | (1u << split) : bits & ~(1u << split);
    if (bits != before)
        controller_.layoutChanged();
}

// --- Copies

std::optional<FxLineItem> FxGraphModel::copyItem(const FxLineItem &item, std::map<int, int> &copies) {
    const bool split = fxNodeSplit(item.node);
    if (split && !item.joined) // a split left open ends the line, which has room for one
        return std::nullopt;
    std::vector<int> effects; // what it holds, the high band's first
    if (!split)
        effects.push_back(item.node);
    for (int lane : split ? item.lanes() : std::span<const int>())
        effects.insert(effects.end(), item.band(lane).begin(), item.band(lane).end());
    size_t free = 0;
    for (int s = 0; s < kFxSlots; ++s)
        free += slotType(s) == FxType::None;
    if (free < effects.size()) {
        say(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        return std::nullopt;
    }
    int to = -1; // a split the line doesn't use
    if (split) {
        const FxLine line = this->line();
        for (int s = 0; s < kFxSplits && to < 0; ++s)
            if (placeOf(line, fxSplitNode(s)).item < 0)
                to = s;
        if (to < 0) {
            say(trf(Str::MsgSplitsFull, {std::to_string(kFxSplits)}));
            return std::nullopt;
        }
    }
    for (int slot : effects) {
        const int copy = freeSlot();
        fillEffect(copy, slotType(slot), slot);
        setFolded(true, copy, folded(true, slot));
        copies[slot] = copy;
    }
    if (!split)
        return FxLineItem{copies[item.node], {}, {}, false};
    // Its crossovers and slope, and its join's band levels and solos (which the line keeps for a joined split).
    const int from = item.node - kFxSlots;
    std::vector<std::pair<int, double>> changes;
    for (const SplitParam f : {SplitParam::Low, SplitParam::High, SplitParam::Slope})
        changes.push_back({paramIndex(to, f), value(paramIndex(from, f))});
    for (const SumParam f : {SumParam::SoloHigh, SumParam::SoloLow, SumParam::SoloMid})
        changes.push_back({paramIndex(to, f), value(paramIndex(from, f))});
    for (int input = 0; input < 3; ++input)
        changes.push_back({fxInputIndex(fxJoinNode(to), input, FxInputField::Level),
                           value(fxInputIndex(fxJoinNode(from), input, FxInputField::Level))});
    editMany(changes);
    controller_.layout().splitBand[to] = controller_.layout().splitBand[from];
    controller_.layoutChanged();
    setSplitFolded(to, splitFolded(from));
    copies[item.node] = fxSplitNode(to);
    copies[fxJoinNode(from)] = fxJoinNode(to);
    FxLineItem copy = item;
    copy.node = fxSplitNode(to);
    for (int lane : copy.lanes())
        for (int &slot : copy.band(lane))
            slot = copies[slot];
    return copy;
}

} // namespace fmvibe::gui
