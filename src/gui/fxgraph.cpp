// The effects as a line of effects and splits from IN to OUT, which the editor reads from the wires and writes back to
// them, and the View's methods that edit it; its two views, the row of cards (each split a container holding its
// bands) and the graph on its slot grid, with their widgets and painting; and a split's controls, which both show.
// The engine runs the wires as they are (see Effects::processGraph()); the editor only ever wires a line.

#include "view.h"

#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "routing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace fmvibe::gui {

using namespace detail;

namespace {

// The bands of a split the grid uses: the high one along the row above the main line, the low one along the row below.
constexpr int kHighBand = kFxBands - 1;
constexpr int kLowBand = 0;

Rect boxAround(Point c, float halfW, float halfH) {
    return {c.x - halfW, c.y - halfH, 2.0f * halfW, 2.0f * halfH};
}

Rect nodeRect(int node, Point c) {
    if (node == kFxOutNode)
        return boxAround(c, kFxEndHalfW, kFxEndHalfH);
    if (fxNodeSum(node) >= 0)
        return boxAround(c, kFxJoinHalfW, kFxJoinHalfH);
    if (fxNodeSplit(node))
        return boxAround(c, kFxSplitHalfW, kFxSplitHalfH);
    return boxAround(c, kFxNodeR, kFxNodeR);
}

Rect slotRect(Point c) {
    return boxAround(c, kFxSlotHalf, kFxSlotHalf);
}

float distanceToSegment(Point p, Point a, Point b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = dx * dx + dy * dy;
    const float t = length > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0f, 1.0f) : 0.0f;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Where a wire's + shows: at its middle, which an S-shaped wire passes through halfway between its ends, or where it
// says (an empty band's).
template <class Wire>
Point wireMiddle(const Wire &w) {
    return w.plus ? *w.plus : Point{(w.from.x + w.to.x) * 0.5f, (w.from.y + w.to.y) * 0.5f};
}

// An S-shaped wire from `a` to `b`, leaving and arriving level: a cubic, sampled.
std::vector<Point> sWire(Point a, Point b) {
    const float dx = std::max(16.0f, (b.x - a.x) * 0.5f);
    const Point c1{a.x + dx, a.y}, c2{b.x - dx, b.y};
    constexpr int kSteps = 24;
    std::vector<Point> points;
    points.reserve(kSteps + 1);
    for (int k = 0; k <= kSteps; ++k) {
        const float t = static_cast<float>(k) / kSteps, u = 1.0f - t;
        const float wa = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, wb = t * t * t;
        points.push_back({wa * a.x + w1 * c1.x + w2 * c2.x + wb * b.x, wa * a.y + w1 * c1.y + w2 * c2.y + wb * b.y});
    }
    return points;
}

// Where an item of the line sits: on the main line, or along one of a split's bands.
struct LinePlace {
    int item = -1; // the main line's item it is, or holds it
    int lane = 0;  // 0 the main line, 1 the high band, 2 the low band
    int at = 0;    // its place along the band
};

template <class Line>
LinePlace placeOf(const Line &line, int node) {
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        if (line[i].node == node)
            return {i, 0, 0};
        for (int lane = 1; lane <= 2; ++lane) {
            const std::vector<int> &band = lane == 1 ? line[i].hi : line[i].lo;
            if (const auto it = std::find(band.begin(), band.end(), node); it != band.end())
                return {i, lane, static_cast<int>(it - band.begin())};
        }
        if (line[i].joined && fxSumNode(line[i].node - kFxSlots) == node) // a split's join
            return {i, 0, 0};
    }
    return {};
}

template <class Line>
bool openSplit(const Line &line) {
    return !line.empty() && fxNodeSplit(line.back().node) && !line.back().joined;
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

// --- The routing graph's grid (Ctrl+drag)

bool View::setCtrl(bool held) {
    if (held == ctrlHeld_)
        return false;
    ctrlHeld_ = held;
    return true; // the grid's points show or go, and a node being dragged snaps or goes free
}

bool View::routingGridShown() const {
    return ctrlHeld_ && controller_.layout().view == RoutingView::Graph && shapeEditing() < 0 &&
           partialEditing() < 0 && (drag_.kind == DragKind::Node || (drag_.kind == DragKind::None && kRouting.contains(hoverPoint_)));
}

// The grid's points nearest `want` first; a point a node fits at, clear of the others, takes it.
namespace {
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

Point View::routingGridPoint(int node, Point want) const {
    // Where nodePlace() would leave it untouched, nothing is in its way.
    auto fits = [&](Point p) {
        bool found = false;
        const Point at = nodePlace(node, p, &found);
        return found && std::hypot(at.x - p.x, at.y - p.y) < 0.5f;
    };
    return nearestGridPoint(kGraph, kRoutingGridCols, kRoutingGridRows, want, fits, nodePlace(node, want));
}

// --- Model

bool View::fxGraph() const {
    return controller_.layout().fxGraphView;
}

bool View::fxNodeShown(int node) const {
    if (node == kFxOutNode)
        return true;
    if (node < 0 || node >= kFxNodes)
        return false;
    if (node < kFxSlots)
        return fxSlotType(node) != FxType::None;
    if (const int sum = fxNodeSum(node); sum >= 0)
        return value(paramIndex(sum, SumParam::Used)) >= 0.5;
    return value(paramIndex(node - kFxSlots, SplitParam::Used)) >= 0.5;
}

std::vector<View::FxWireRef> View::fxWires() const {
    std::vector<FxWireRef> wires;
    for (int node = 0; node < kFxNodes; ++node) {
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

View::FxLine View::fxLine() const {
    if (fxDerived_.line) // only kept while a scope goes on
        return *fxDerived_.line;
    FxLine line = fxLineOfWires();
    if (derivedDepth_ > 0)
        fxDerived_.line = line;
    return line;
}

View::FxLine View::fxLineOfWires() const {
    const std::vector<FxWireRef> wires = fxWires();
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
        const int high = band(fxBandPort(split, kHighBand), item.hi);
        const int low = band(fxBandPort(split, kLowBand), item.lo);
        item.joined = high >= 0 && high == low && fxNodeSum(high) >= 0 && !taken[high];
        line.push_back(item);
        if (!item.joined) // open: the line ends here
            break;
        taken[high] = true;
        port = fxSumPort(fxNodeSum(high));
    }
    // The effects the wires leave out go at its end, by number, on the high band while a split is open.
    const bool open = openSplit(line);
    for (int slot = 0; slot < kFxSlots; ++slot)
        if (!taken[slot] && fxSlotType(slot) != FxType::None) {
            if (open)
                line.back().hi.push_back(slot);
            else
                line.push_back({slot, {}, {}, false});
        }
    return line;
}

void View::writeFxLine(const FxLine &line) {
    std::map<int, double> want;
    std::array<bool, kFxSplits> used{}, joined{};
    for (const FxLineItem &item : line)
        if (fxNodeSplit(item.node)) {
            used[item.node - kFxSlots] = true;
            joined[item.node - kFxSlots] = item.joined;
        }
    // Every wire goes, at full level, but for a join's two bands, which keep theirs. A level reset loses its
    // modulation: the grid shows no other wire's level.
    std::vector<int> reset;
    for (int node = 0; node < kFxNodes; ++node)
        for (int input = 0; input < fxInputCount(node); ++input) {
            want[fxInputIndex(node, input, FxInputField::From)] = 0.0;
            const int sum = fxNodeSum(node);
            if (sum >= 0 && joined[sum] && input < 2)
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
        int high = fxBandPort(split, kHighBand), low = fxBandPort(split, kLowBand);
        for (int slot : item.hi) {
            wire(slot, 0, high);
            high = fxSlotPort(slot);
        }
        for (int slot : item.lo) {
            wire(slot, 0, low);
            low = fxSlotPort(slot);
        }
        if (item.joined) {
            wire(fxJoinNode(split), 0, high);
            wire(fxJoinNode(split), 1, low);
            port = fxSumPort(split);
        } else { // open, the line's last: both bands go out
            wire(kFxOutNode, 0, high);
            wire(kFxOutNode, 1, low);
            port = -1;
        }
    }
    if (port >= 0)
        wire(kFxOutNode, 0, port);
    for (int split = 0; split < kFxSplits; ++split) {
        want[paramIndex(split, SplitParam::Used)] = used[split] ? 1.0 : 0.0;
        want[paramIndex(split, SplitParam::Bands)] = 2.0;
        want[paramIndex(split, SumParam::Used)] = joined[split] ? 1.0 : 0.0;
        if (!joined[split]) // a join taken out starts again unsoloed
            for (const SumParam f : {SumParam::SoloHigh, SumParam::SoloLow})
                want[paramIndex(split, f)] = 0.0;
    }
    // What modulated a level reset, or the crossover of a split no longer in use, goes.
    for (const Assignment &m : assignments()) {
        const int split = splitOf(m.target);
        const bool crossover = split >= 0 && !used[split] &&
                               (m.target == paramIndex(split, SplitParam::Low) ||
                                m.target == paramIndex(split, SplitParam::High));
        if (crossover || std::find(reset.begin(), reset.end(), m.target) != reset.end())
            removeSlot(m.source, m.slot);
    }
    std::vector<std::pair<int, double>> changes;
    for (const auto &[index, v] : want)
        if (value(index) != v)
            changes.push_back({index, v});
    editMany(changes);
}

View::FxGridLayout View::fxGridLayout(const FxLine &line) {
    FxGridLayout grid;
    int col = 1; // IN takes the first column
    bool open = false;
    for (const FxLineItem &item : line) {
        grid.nodes.push_back({item.node, col, 0});
        if (item.node < kFxSlots) {
            ++col;
            continue;
        }
        // A split's bands start in the column after it, the high band above the main line, the low one below.
        const int start = col + 1;
        for (size_t k = 0; k < item.hi.size(); ++k)
            grid.nodes.push_back({item.hi[k], start + static_cast<int>(k), -1});
        for (size_t k = 0; k < item.lo.size(); ++k)
            grid.nodes.push_back({item.lo[k], start + static_cast<int>(k), 1});
        const int longest = static_cast<int>(std::max(item.hi.size(), item.lo.size()));
        if (item.joined) { // the join in the column after the longer band, a column on from empty ones
            grid.nodes.push_back({fxJoinNode(item.node - kFxSlots), start + std::max(longest, 1), 0});
        } else { // a + at the end of each band
            grid.slots.push_back({1, start + static_cast<int>(item.hi.size()), -1});
            grid.slots.push_back({2, start + static_cast<int>(item.lo.size()), 1});
            open = true;
        }
        col = start + (item.joined ? std::max(longest, 1) : longest) + 1;
    }
    if (!open) // the main line's +, then OUT
        grid.slots.push_back({0, col++, 0});
    grid.outCol = col;
    return grid;
}

float View::fxGraphScrollMost(const FxGridLayout &grid) const {
    const float width = kFxGridLeft + static_cast<float>(grid.outCol) * kFxGridCol + kFxEndHalfW + 16.0f;
    return std::max(width - fxGraphArea().w, 0.0f);
}

float View::fxGraphScroll(const FxGridLayout &grid) const {
    return std::clamp(fxGraphScroll_, 0.0f, fxGraphScrollMost(grid));
}

void View::revealFxNode(int node) {
    const FxGridLayout grid = fxGridLayout(fxLine());
    const float scroll = fxGraphScroll(grid);
    const Rect area = fxGraphArea();
    for (const auto &place : grid.nodes)
        if (place.node == node) {
            // The node and the column after it (its + or what follows), else as near as fits.
            const float x = kFxGridLeft + static_cast<float>(place.col) * kFxGridCol;
            const float left = x - 0.5f * kFxGridCol, right = x + 1.5f * kFxGridCol;
            fxGraphScroll_ = left < scroll ? left : right > scroll + area.w ? right - area.w : scroll;
            fxGraphScroll_ = fxGraphScroll(grid);
        }
}

int View::fxHoveredNode() const {
    if (drag_.kind == DragKind::FxNodeMove) // not while one is held
        return -1;
    return hover_ && (hover_->kind == Kind::FxNode || hover_->kind == Kind::FxNodeDelete) ? hover_->a : -1;
}

Point View::fxDeleteAt(int node, Point center) {
    if (node < kFxSlots) // on the circle, up and to the right
        return {center.x + 0.72f * kFxNodeR, center.y - 0.72f * kFxNodeR};
    const Rect r = nodeRect(node, center); // at a box's corner
    return {r.right() - 2.0f, r.y + 2.0f};
}

Rect View::fxInspector(int node) const {
    return fxInspectorCard(node >= 0 && node < kFxSlots ? fxSpan(fxSlotType(node)) : 1.0f);
}

int View::fxSelectedNode() const {
    const int node = controller_.layout().fxSelected;
    if (node >= 0 && node < kFxNodes && node != kFxOutNode && fxNodeShown(node))
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
    for (const FxRowCard &card : fxRowShown().cards)
        if (card.slot == slot) {
            const float place = card.place - fxScroll();
            if (place <= -card.span || place >= static_cast<float>(kVisibleFx))
                return std::nullopt;
            return fxCard(place, card.span);
        }
    return std::nullopt; // on a band its container doesn't show
}

// --- Editing

void View::fxPlaceNew(int slot, int copyOf) {
    FxLine line = fxLine();
    // The line lists it at its end already, as an effect the wires leave out: it goes where it was asked for instead.
    for (FxLineItem &item : line) {
        item.hi.erase(std::remove(item.hi.begin(), item.hi.end(), slot), item.hi.end());
        item.lo.erase(std::remove(item.lo.begin(), item.lo.end(), slot), item.lo.end());
    }
    line.erase(std::remove_if(line.begin(), line.end(), [slot](const FxLineItem &i) { return i.node == slot; }),
               line.end());
    // Into the band at `lane` of item `i`, at `at`, or on the main line at `i`.
    auto put = [&](int i, int lane, int at) {
        if (lane == 0) {
            line.insert(line.begin() + std::clamp(i, 0, static_cast<int>(line.size())), FxLineItem{slot, {}, {}, false});
            return;
        }
        std::vector<int> &band = lane == 1 ? line[i].hi : line[i].lo;
        band.insert(band.begin() + std::clamp(at, 0, static_cast<int>(band.size())), slot);
    };
    const bool open = openSplit(line);
    const int last = static_cast<int>(line.size()) - 1;
    if (const LinePlace was = copyOf >= 0 ? placeOf(line, copyOf) : LinePlace{}; was.item >= 0 && fxGraph()) {
        put(was.lane == 0 ? was.item + 1 : was.item, was.lane, was.at + 1); // a copy right after its original
    } else if (was.item >= 0) { // in the row, at the line's end, or its band's
        if (was.lane == 0)
            put(open ? last : last + 1, 0, 0);
        else
            put(was.item, was.lane, static_cast<int>((was.lane == 1 ? line[was.item].hi : line[was.item].lo).size()));
    } else if (fxSlot_.lane >= 0 && fxSlot_.item >= 0) { // a wire's middle
        put(fxSlot_.item, fxSlot_.lane, fxSlot_.place);
    } else if (fxSlot_.lane == 0 || (fxSlot_.lane > 0 && !open)) { // the main line's +
        put(open ? last : last + 1, 0, 0);
    } else if (fxSlot_.lane > 0) { // a band's +
        put(last, fxSlot_.lane, static_cast<int>((fxSlot_.lane == 1 ? line[last].hi : line[last].lo).size()));
    } else if (const int selected = controller_.layout().fxSelected;
               selected >= 0 && selected != slot && placeOf(line, selected).item >= 0) {
        // After the node picked: an effect in its band or on the main line, a join on the main line, a split at the
        // end of its shorter band (the high one if they are as long).
        const LinePlace at = placeOf(line, selected);
        if (fxNodeSplit(selected)) {
            const FxLineItem &item = line[at.item];
            const int lane = item.lo.size() < item.hi.size() ? 2 : 1;
            put(at.item, lane, static_cast<int>((lane == 1 ? item.hi : item.lo).size()));
        } else {
            put(at.lane == 0 ? at.item + 1 : at.item, at.lane, at.at + 1);
        }
    } else if (open) { // none picked: at the end, on the high band while a split is open
        put(last, 1, static_cast<int>(line[last].hi.size()));
    } else {
        put(last + 1, 0, 0);
    }
    writeFxLine(line);
    controller_.layout().fxSelected = slot;
    controller_.layoutChanged();
    revealFxNode(slot);
    revealFxRow(slot);
}

void View::addSplit() {
    FxLine line = fxLine();
    // With its join right after it, its bands empty: where a wire's middle on the main line was clicked, else at
    // the line's end.
    const bool between = fxSlot_.lane == 0 && fxSlot_.item >= 0;
    if (!between && openSplit(line))
        return; // the main line has no + while a split is open (only an earlier patch leaves one so)
    int split = -1;
    for (int s = 0; s < kFxSplits && split < 0; ++s)
        if (placeOf(line, fxSplitNode(s)).item < 0)
            split = s;
    if (split < 0) {
        flash(trf(Str::MsgSplitsFull, {std::to_string(kFxSplits)}));
        return;
    }
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes; // a fresh split's crossover
    for (const SplitParam f : {SplitParam::Low, SplitParam::High})
        changes.push_back({paramIndex(split, f), table_[paramIndex(split, f)].def});
    for (int input = 0; input < 2; ++input) // and its join's band levels
        changes.push_back({fxInputIndex(fxJoinNode(split), input, FxInputField::Level), 0.0}); // 0 dB
    editMany(changes);
    line.insert(line.begin() + (between ? std::clamp(fxSlot_.item, 0, static_cast<int>(line.size()))
                                        : static_cast<int>(line.size())),
                FxLineItem{fxSplitNode(split), {}, {}, true});
    writeFxLine(line);
    controller_.layout().fxSelected = fxSplitNode(split);
    controller_.layoutChanged();
    revealFxNode(fxSplitNode(split));
    revealFxRow(fxSplitNode(split));
}

void View::removeFxNode(int node) {
    if (node >= 0 && node < kFxSlots) {
        removeEffect(node);
        return;
    }
    FxLine line = fxLine();
    const LinePlace at = placeOf(line, node); // a join's is its split's: it goes with it
    if (at.item < 0)
        return;
    // A split, or its join: its bands' effects go on the main line in its place, the high band's first.
    const UndoScope step(controller_);
    const FxLineItem split = line[at.item];
    line.erase(line.begin() + at.item);
    std::vector<FxLineItem> effects;
    for (int slot : split.hi)
        effects.push_back({slot, {}, {}, false});
    for (int slot : split.lo)
        effects.push_back({slot, {}, {}, false});
    line.insert(line.begin() + at.item, effects.begin(), effects.end());
    writeFxLine(line);
}

void View::setFxGraph(bool on) {
    // Two views of the same line: only what shows changes.
    if (on == fxGraph())
        return;
    controller_.layout().fxGraphView = on;
    controller_.layoutChanged();
    fxMenuOpen_ = false;
    fxSlot_ = {};
    hover_.reset();
}

// --- Places

View::FxPlaces View::fxPlacesOf(const FxGridLayout &grid, float scroll) const {
    FxPlaces at;
    for (const auto &place : grid.nodes)
        at.node[place.node] = fxGridCell(place.col, place.row, scroll);
    for (const auto &slot : grid.slots)
        at.slot[slot.lane] = fxGridCell(slot.col, slot.row, scroll);
    at.in = fxGridCell(0, 0, scroll);
    at.out = fxGridCell(grid.outCol, 0, scroll);
    return at;
}

View::FxPlaces View::fxPlacesShown(const FxGridLayout &grid, float scroll) const {
    if (!fxGlide_.on)
        return fxPlacesOf(grid, scroll);
    FxPlaces at;
    for (const auto &place : grid.nodes)
        at.node[place.node] = fxGridAt(fxGlide_.node[place.node].x, fxGlide_.node[place.node].y, scroll);
    for (const auto &slot : grid.slots)
        at.slot[slot.lane] = fxGridAt(fxGlide_.slot[slot.lane].x, fxGlide_.slot[slot.lane].y, scroll);
    at.in = fxGridCell(0, 0, scroll);
    at.out = fxGridAt(fxGlide_.outCol, 0.0f, scroll);
    return at;
}

View::FxLine View::fxLineShown() const {
    if ((drag_.kind == DragKind::FxNodeMove || drag_.kind == DragKind::FxMove) && drag_.moved && fxDrag_)
        return fxLineWith(fxDrag_->rest, fxDrag_->item, fxDrag_->at);
    return fxLine();
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
        w.clip = fxGraphArea(); // the graph scrolls inside its area
        list.push_back(w);
    };
    const FxLine line = fxLine();
    const FxGridLayout grid = fxGridLayout(line);
    const float scroll = fxGraphScroll(grid);
    const FxPlaces at = fxPlacesOf(grid, scroll);
    const std::vector<FxGraphWire> wires = fxGraphWires(line, at);
    // Along the wires that take a node at their middle, under the rest: the mouse anywhere on one shows its +.
    for (const FxGraphWire &w : wires)
        if (w.at.lane >= 0) {
            const float left = std::min(w.from.x, w.to.x), top = std::min(w.from.y, w.to.y);
            add(Kind::FxWire,
                Rect{left, top, std::max(w.from.x, w.to.x) - left, std::max(w.from.y, w.to.y) - top}.inset(-6.0f), -1,
                w.at.lane, w.at.item * kFxGapStride + w.at.place);
        }
    for (const auto &place : grid.nodes)
        add(Kind::FxNode, nodeRect(place.node, at.node[place.node]).inset(-4.0f), -1, place.node);
    for (const auto &place : grid.nodes)
        if (place.node == fxHoveredNode()) {
            const Point x = fxDeleteAt(place.node, at.node[place.node]);
            add(Kind::FxNodeDelete, {x.x - 8.0f, x.y - 8.0f, 16.0f, 16.0f}, -1, place.node);
        }
    for (const auto &slot : grid.slots)
        add(Kind::FxSlot, slotRect(at.slot[slot.lane]).inset(-4.0f), -1, slot.lane);
    for (const FxGraphWire &w : wires) // the +s at their middles, which take the click
        if (w.at.lane >= 0) {
            const Point m = wireMiddle(w);
            add(Kind::FxGap, {m.x - 10.0f, m.y - 10.0f, 20.0f, 20.0f}, -1, w.at.lane,
                w.at.item * kFxGapStride + w.at.place);
        }
}

bool View::fxWireHit(const Widget &w, Point p) const {
    // The wires where they sit on their cells, worked out once while a scope goes on (each FxWire widget the mouse
    // is over asks).
    std::optional<std::vector<FxGraphWire>> fresh;
    std::optional<std::vector<FxGraphWire>> &kept = derivedDepth_ > 0 ? fxDerived_.hitWires : fresh;
    if (!kept) {
        const FxLine line = fxLine();
        const FxGridLayout grid = fxGridLayout(line);
        kept = fxGraphWires(line, fxPlacesOf(grid, fxGraphScroll(grid)));
    }
    for (const FxGraphWire &wire : *kept)
        if (wire.at.lane == w.a && wire.at.item * kFxGapStride + wire.at.place == w.b) {
            const std::vector<Point> points = sWire(wire.from, wire.to);
            for (size_t k = 1; k < points.size(); ++k)
                if (distanceToSegment(p, points[k - 1], points[k]) <= 6.0f)
                    return true;
            return false;
        }
    return false;
}

std::vector<View::FxGraphWire> View::fxGraphWires(const FxLine &line, const FxPlaces &places) const {
    auto effectColor = [&](int slot) {
        return value(paramIndex(slot, FxParam::Enabled)) >= 0.5 ? fxTypeColor(fxSlotType(slot)) : kFaint;
    };
    struct Tail {
        Point at;
        Color color;
        int port; // what goes along from there
    };
    std::vector<FxGraphWire> wires;
    // From each node's right to the next one's left (`to`, an arrowhead's tip where `arrow`); a node clicked in at its
    // middle goes `where`.
    auto wire = [&](Tail from, Point to, bool arrow, FxSlotAt where = {}) {
        wires.push_back({from.at, arrow ? Point{to.x - 8.0f, to.y} : to, from.color, arrow, where, std::nullopt,
                         from.port});
    };
    const auto &at = places.node;
    const Point in = places.in, out = places.out;
    Tail tail{{in.x + kFxEndHalfW, in.y}, kDim, kFxPortIn};
    bool open = false;
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        const FxLineItem &item = line[i];
        const Point p = at[item.node];
        if (item.node < kFxSlots) {
            wire(tail, {p.x - kFxNodeR, p.y}, true, {0, i, 0});
            tail = {{p.x + kFxNodeR, p.y}, effectColor(item.node), fxSlotPort(item.node)};
            continue;
        }
        wire(tail, {p.x - kFxSplitHalfW, p.y}, true, {0, i, 0});
        // Out of the bands' ports in the split's grey.
        const int split = item.node - kFxSlots;
        Tail bands[] = {{fxBandPortAt(p, kHighBand), splitColor(), fxBandPort(split, kHighBand)},
                        {fxBandPortAt(p, kLowBand), splitColor(), fxBandPort(split, kLowBand)}};
        for (int lane = 0; lane < 2; ++lane) {
            const std::vector<int> &band = lane == 0 ? item.hi : item.lo;
            for (size_t k = 0; k < band.size(); ++k) {
                const Point e = at[band[k]];
                wire(bands[lane], {e.x - kFxNodeR, e.y}, true, {lane + 1, i, static_cast<int>(k)});
                bands[lane] = {{e.x + kFxNodeR, e.y}, effectColor(band[k]), fxSlotPort(band[k])};
            }
        }
        if (item.joined) { // into the join, a node clicked in there going at its band's end
            const Point j = at[fxJoinNode(item.node - kFxSlots)];
            const float input = fxBandPortAt(j, kHighBand).y - j.y; // level with the split's ports
            for (int lane = 0; lane < 2; ++lane) {
                const std::vector<int> &band = lane == 0 ? item.hi : item.lo;
                const Point into{j.x - kFxJoinHalfW, lane == 0 ? j.y + input : j.y - input};
                const FxSlotAt end{lane + 1, i, static_cast<int>(band.size())};
                if (!band.empty()) {
                    wire(bands[lane], into, true, end);
                    continue;
                }
                // Empty: out to its +, on its row a column on from the split, and from there into the join.
                const Point plus{p.x + kFxGridCol, p.y + (lane == 0 ? -kFxGridRow : kFxGridRow)};
                wire(bands[lane], {plus.x - kFxSlotHalf, plus.y}, false, end);
                wires.back().plus = plus;
                wire({{plus.x + kFxSlotHalf, plus.y}, bands[lane].color, bands[lane].port}, into, true);
            }
            tail = {{j.x + kFxJoinHalfW, j.y}, sumColor(), fxSumPort(item.node - kFxSlots)};
            continue;
        }
        open = true; // each band on to its +, and from there out
        for (int lane = 0; lane < 2; ++lane) {
            const Point plus = places.slot[lane + 1];
            wire(bands[lane], {plus.x - kFxSlotHalf, plus.y}, false);
            wire({{plus.x + kFxSlotHalf, plus.y}, bands[lane].color, bands[lane].port},
                 {out.x - kFxEndHalfW, out.y + (lane == 0 ? -12.0f : 12.0f)}, true);
        }
    }
    if (!open) {
        const Point plus = places.slot[0];
        wire(tail, {plus.x - kFxSlotHalf, plus.y}, false);
        wire({{plus.x + kFxSlotHalf, plus.y}, tail.color, tail.port}, {out.x - kFxEndHalfW, out.y}, true);
    }
    return wires;
}

float View::fxGlow(int port) const {
    if (port < 0 || port >= kFxPorts || !(telemetry_.age < 0.5f))
        return 0.0f;
    const float level = telemetry_.fxPortLevels[port];
    if (!(level > 0.0f))
        return 0.0f;
    return std::clamp((20.0f * std::log10(level) - kFxGlowFloorDb) / -kFxGlowFloorDb, 0.0f, 1.0f);
}

// --- A split's controls

Rect View::splitDisplay(Rect card) {
    return {card.x + 14.0f, card.y + 66.0f, card.w - 28.0f, 104.0f};
}

namespace {
// The bands' responses on a split's display: how much of each band (0..1) goes through at `hz`, at its slope (a
// one-pole and the rest, or a Linkwitz-Riley of second or fourth order), and where on the display that shows.
double bandResponse(int band, double hz, double crossover, int slope) {
    const double x = band == kLowBand ? hz / crossover : crossover / hz;
    return slope == 6 ? 1.0 / std::sqrt(1.0 + x * x) : slope == 12 ? 1.0 / (1.0 + x * x) : 1.0 / (1.0 + x * x * x * x);
}
float responseY(Rect display, double level) {
    const float top = display.y + 12.0f, bottom = display.bottom() - 22.0f;
    return bottom - static_cast<float>(level) * (bottom - top);
}
} // namespace

Point View::splitHandle(int split, Rect display) const {
    const double hz =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)))[0];
    const int slope = slopeDb(filterSlope(value(paramIndex(split, SplitParam::Slope))));
    const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
    return {std::clamp(x, display.x, display.right()), responseY(display, bandResponse(kLowBand, hz, hz, slope))};
}

int View::splitLane(int split) const {
    return controller_.layout().splitBand[split] == 1 ? 1 : 2;
}

void View::showSplitBand(int split, int band) {
    if (controller_.layout().splitBand[split] == band)
        return;
    controller_.layout().splitBand[split] = band;
    controller_.layoutChanged();
}

bool View::splitFolded(int split) const {
    return (controller_.layout().splitsFolded >> split) & 1u;
}

void View::setSplitFolded(int split, bool folded) {
    uint32_t &bits = controller_.layout().splitsFolded;
    const uint32_t before = bits;
    bits = folded ? bits | (1u << split) : bits & ~(1u << split);
    if (bits != before)
        controller_.layoutChanged();
}

std::optional<Rect> View::splitCardOf(int split) const {
    if (fxGraph()) {
        const int node = fxSelectedNode();
        if (node == fxSplitNode(split) || node == fxJoinNode(split))
            return fxInspector(node);
        return std::nullopt;
    }
    for (const FxRowBox &box : fxRowShown().boxes)
        if (box.split == split && !box.folded) {
            const float place = box.place - fxScroll();
            if (place > -1.0f && place < static_cast<float>(kVisibleFx))
                return fxCard(place, 1.0f);
        }
    return std::nullopt;
}

void View::splitControlWidgets(std::vector<Widget> &list, int split, Rect card, bool row) const {
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    // The title row: in the row, it drags the container along the line, and folds it; the x removes the split.
    if (row) {
        add(Kind::FxBoxGrip, {card.x, card.y, card.w - 52.0f, 34.0f}, -1, fxSplitNode(split));
        add(Kind::FxBoxFold, {card.right() - 26.0f - 22.0f, card.y + 9.0f, 18.0f, 19.0f}, -1, split);
    }
    add(Kind::FxNodeRemove, {card.right() - 26.0f, card.y + 8.0f, 18.0f, 21.0f}, -1, fxSplitNode(split));
    for (int band = 0; band < 2; ++band)
        add(Kind::SplitBand, splitTab(card, band), -1, split, band);
    // The crossover's handle on the display.
    const Point h = splitHandle(split, splitDisplay(card));
    add(Kind::SplitPoint, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f}, paramIndex(split, SplitParam::Low), split);
    // Its slope under the display, 6 | 12 | 24 dB (18, which a filter has, it doesn't).
    const size_t first = list.size();
    const int slopes[] = {0, 1, kFilterSlopes - 1};
    for (int k = 0; k < 3; ++k)
        add(Kind::Choice, splitSlopeButton(card, k), paramIndex(split, SplitParam::Slope), -1, slopes[k]);
    mergeButtons(list, first);
    // The bands' solos, over their levels, and the knobs: the low band's level, the crossover, the high band's level
    // (the high band's wire takes the join's first input, the low band's its second).
    for (int band = 0; band < 2; ++band)
        add(Kind::SplitSolo, splitSoloButton(card, band),
            paramIndex(split, band == 0 ? SumParam::SoloLow : SumParam::SoloHigh), split, band);
    const int params[] = {fxInputIndex(fxJoinNode(split), 1, FxInputField::Level), paramIndex(split, SplitParam::Low),
                          fxInputIndex(fxJoinNode(split), 0, FxInputField::Level)};
    const Str labels[] = {Str::BandLow, Str::LabelCrossover, Str::BandHigh};
    for (int k = 0; k < 3; ++k) {
        Widget knob;
        knob.kind = Kind::Knob;
        knob.rect = knobRect(splitKnob(card, k), 30.0f);
        knob.param = params[k];
        knob.label = tr(labels[k]);
        list.push_back(knob);
    }
}

// --- The row

float View::fxBoxSpan(const FxLineItem &item) const {
    const int split = item.node - kFxSlots;
    if (splitFolded(split))
        return kFoldedFx;
    float span = 1.0f + kFxBoxAddSpan;
    for (int slot : splitLane(split) == 1 ? item.hi : item.lo)
        span += fxCardSpan(slot);
    return span;
}

View::FxRow View::fxRow(const FxLine &line) const {
    FxRow row;
    float place = 0.0f;
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        const FxLineItem &item = line[i];
        row.items.push_back(place);
        if (item.node < kFxSlots) {
            const float span = fxCardSpan(item.node);
            row.cards.push_back({item.node, place, span, {0, i, 0}});
            place += span;
            continue;
        }
        const int split = item.node - kFxSlots;
        FxRowBox box{split, i, splitLane(split), place, fxBoxSpan(item), splitFolded(split), 0.0f};
        if (!box.folded) { // the band it shows, after its controls
            float at = place + 1.0f;
            const std::vector<int> &band = box.lane == 1 ? item.hi : item.lo;
            for (int k = 0; k < static_cast<int>(band.size()); ++k) {
                const float span = fxCardSpan(band[k]);
                row.cards.push_back({band[k], at, span, {box.lane, i, k}});
                at += span;
            }
            box.add = at;
        }
        row.boxes.push_back(box);
        place += box.span;
    }
    row.items.push_back(place);
    return row;
}

View::FxRow View::fxRowShown() const {
    return fxRow(fxLineShown());
}

void View::revealFxRow(int node) {
    const FxLine line = fxLine();
    const FxRow row = fxRow(line);
    float place = -1.0f, span = 1.0f;
    for (const FxRowCard &card : row.cards)
        if (card.slot == node) {
            place = card.place;
            span = card.span;
        }
    const int item = placeOf(line, node).item; // else the container that holds it
    for (const FxRowBox &box : row.boxes)
        if (place < 0.0f && box.item == item) {
            place = box.place;
            span = box.span;
        }
    if (place < 0.0f)
        return;
    const float first = fxScroll();
    fxScroll_ = place < first ? place
                : place + span > first + kVisibleFx ? std::min(place, place + span - static_cast<float>(kVisibleFx))
                                                    : first;
}

void View::startRowDrag(int node) {
    fxDrag_.reset();
    FxLine line = fxLine();
    const FxRow row = fxRow(line);
    const LinePlace place = placeOf(line, node);
    if (place.item < 0)
        return;
    FxDrag d;
    for (const FxRowCard &card : row.cards)
        if (card.slot == node) {
            d.startPlace = card.place;
            d.span = card.span;
        }
    for (const FxRowBox &box : row.boxes)
        if (fxSplitNode(box.split) == node) {
            d.startPlace = box.place;
            d.span = box.span;
        }
    if (place.lane == 0) { // off the main line: an effect, or a split with what it holds
        d.item = line[place.item];
        d.was = {0, place.item, 0};
        line.erase(line.begin() + place.item);
    } else { // off a band
        std::vector<int> &band = place.lane == 1 ? line[place.item].hi : line[place.item].lo;
        d.item = {node, {}, {}, false};
        d.was = {place.lane, place.item, place.at};
        band.erase(band.begin() + place.at);
    }
    d.rest = std::move(line);
    d.at = d.was;
    fxDrag_ = std::move(d);
}

void View::retargetRowDrag(Point mouse) {
    if (!fxDrag_)
        return;
    FxDrag &d = *fxDrag_;
    const bool effect = d.item.node < kFxSlots;
    // Carrying an effect over a container's tab shows that band, to drop it on.
    if (effect)
        for (const FxRowBox &box : fxRow(fxLineWith(d.rest, d.item, d.at)).boxes) {
            if (box.folded)
                continue;
            const Rect column = fxCard(box.place - fxScroll(), 1.0f);
            for (int band = 0; band < 2; ++band)
                if (splitTab(column, band).contains(mouse))
                    showSplitBand(box.split, band);
        }
    // Where the mouse is along the row, in card places, held to the cards on screen while the row scrolls under it. It
    // lands after the main line's items whose middles are left of that; an effect, over the band a container shows,
    // after that band's cards whose middles are. (A wide card or container would cross its neighbor's middle at once.)
    const float middle = std::clamp((mouse.x - fxCard(0.0f).x) / (kFxCardW + kFxGap) + fxScroll(), fxScroll(),
                                    fxScroll() + kVisibleFx);
    const FxRow row = fxRow(d.rest);
    const int n = static_cast<int>(d.rest.size());
    int to = 0;
    for (int i = 0; i < n; ++i)
        to += 0.5f * (row.items[i] + row.items[i + 1]) < middle;
    if (openSplit(d.rest) || (fxNodeSplit(d.item.node) && !d.item.joined)) // a split left open ends the line
        to = fxNodeSplit(d.item.node) && !d.item.joined ? n : std::min(to, n - 1);
    FxSlotAt at{0, to, 0};
    for (const FxRowBox &box : row.boxes) {
        if (!effect || box.folded || middle < box.place + 0.75f || middle > box.place + box.span)
            continue;
        int k = 0;
        for (const FxRowCard &card : row.cards)
            k += card.at.lane == box.lane && card.at.item == box.item && card.place + 0.5f * card.span < middle;
        at = {box.lane, box.item, k};
    }
    d.at = at;
}

void View::dropRowDrag() {
    if (!fxDrag_)
        return;
    const FxDrag d = std::move(*fxDrag_);
    fxDrag_.reset();
    const bool stays = d.at.lane == d.was.lane && d.at.item == d.was.item && d.at.place == d.was.place;
    if (!drag_.moved || stays)
        return;
    const UndoScope step(controller_);
    writeFxLine(fxLineWith(d.rest, d.item, d.at));
}

// --- Dragging nodes

View::FxLine View::fxLineWith(FxLine line, const FxLineItem &item, FxSlotAt at) {
    if (at.lane == 0) {
        line.insert(line.begin() + std::clamp(at.item, 0, static_cast<int>(line.size())), item);
    } else if (at.item >= 0 && at.item < static_cast<int>(line.size())) {
        std::vector<int> &band = at.lane == 1 ? line[at.item].hi : line[at.item].lo;
        band.insert(band.begin() + std::clamp(at.place, 0, static_cast<int>(band.size())), item.node);
    }
    return line;
}

void View::startFxDrag(int node) {
    fxDrag_.reset();
    FxLine line = fxLine();
    const int taken = fxNodeSum(node) >= 0 ? fxSplitNode(fxNodeSum(node)) : node; // a join is carried by its split
    const LinePlace place = placeOf(line, taken);
    if (place.item < 0)
        return;
    FxDrag d;
    if (place.lane == 0) { // off the main line: an effect, or a split with what it holds
        d.item = line[place.item];
        d.was = {0, place.item, 0};
        line.erase(line.begin() + place.item);
    } else { // off a band
        std::vector<int> &band = place.lane == 1 ? line[place.item].hi : line[place.item].lo;
        d.item = {taken, {}, {}, false};
        d.was = {place.lane, place.item, place.at};
        band.erase(band.begin() + place.at);
    }
    d.rest = std::move(line);
    d.at = d.was;
    d.nodes = {d.item.node};
    d.nodes.insert(d.nodes.end(), d.item.hi.begin(), d.item.hi.end());
    d.nodes.insert(d.nodes.end(), d.item.lo.begin(), d.item.lo.end());
    if (d.item.joined)
        d.nodes.push_back(fxJoinNode(d.item.node - kFxSlots));
    const FxGridLayout grid = fxGridLayout(fxLine());
    const FxPlaces shown = fxPlacesShown(grid, fxGraphScroll(grid));
    for (int n : d.nodes)
        d.from[n] = shown.node[n];
    if (!fxGlide_.on) { // the glide starts from where everything is
        for (const auto &p : grid.nodes)
            fxGlide_.node[p.node] = {static_cast<float>(p.col), static_cast<float>(p.row)};
        for (const auto &s : grid.slots)
            fxGlide_.slot[s.lane] = {static_cast<float>(s.col), static_cast<float>(s.row)};
        fxGlide_.outCol = static_cast<float>(grid.outCol);
    }
    fxDrag_ = std::move(d);
}

void View::retargetFxDrag(Point mouse) {
    if (!fxDrag_)
        return;
    FxDrag &d = *fxDrag_;
    // Where the node grabbed is carried, in cells of the graph as it shows: with the place it would land in opened.
    const int grabbed = drag_.widget.a;
    const Point carried{d.from[grabbed].x + mouse.x - drag_.start.x, d.from[grabbed].y + mouse.y - drag_.start.y};
    const FxGridLayout shown = fxGridLayout(fxLineShown());
    const Rect area = fxGraphArea();
    const Point cell{(carried.x - area.x - kFxGridLeft + fxGraphScroll(shown)) / kFxGridCol,
                     (carried.y - area.center().y) / kFxGridRow};
    auto colOf = [&](int node) {
        for (const auto &p : shown.nodes)
            if (p.node == node)
                return static_cast<float>(p.col);
        return 0.0f;
    };
    auto slotCol = [&](int lane) {
        for (const auto &s : shown.slots)
            if (s.lane == lane)
                return static_cast<float>(s.col);
        return static_cast<float>(shown.outCol);
    };
    // The places it can land in the line without it, each the gap it shows as: halfway between the nodes it would go
    // between, as they show. Only over one of those does it move there; over a node, or away from them, the place it
    // has stays open, so the others don't shuffle about under it.
    constexpr float kOverCols = 0.35f, kOverRows = 0.45f;
    float best = 1e30f;
    FxSlotAt over{};
    auto consider = [&](FxSlotAt at, float prev, float next, float row) {
        const float col = 0.5f * (prev + next);
        if (std::abs(col - cell.x) > kOverCols || std::abs(row - cell.y) > kOverRows)
            return;
        const float dx = (col - cell.x) * kFxGridCol, dy = (row - cell.y) * kFxGridRow;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            over = at;
        }
    };
    // Past a line's or a band's + too, along its wire on to OUT (from `row` at the + to `outRow` there): its end.
    auto considerOut = [&](FxSlotAt at, float plus, float row, float outRow) {
        const float out = static_cast<float>(shown.outCol);
        if (cell.x < plus || cell.x > out || out <= plus)
            return;
        const float along = row + (outRow - row) * (cell.x - plus) / (out - plus);
        if (std::abs(cell.y - along) > kOverRows)
            return;
        const float dy = (cell.y - along) * kFxGridRow;
        if (dy * dy < best) {
            best = dy * dy;
            over = at;
        }
    };
    const bool group = fxNodeSplit(d.item.node);
    const bool open = openSplit(d.rest);
    const int n = static_cast<int>(d.rest.size());
    auto rightOf = [&](const FxLineItem &item) { // a joined split ends at its join
        return colOf(item.joined ? fxJoinNode(item.node - kFxSlots) : item.node);
    };
    for (int i = 0; i <= n; ++i) {
        if (i == n && open)
            break; // nothing goes after a split left open, which ends the line
        if (group && !d.item.joined && i < n)
            continue; // a split left open stays at the end
        consider({0, i, 0}, i > 0 ? rightOf(d.rest[i - 1]) : 0.0f, i < n ? colOf(d.rest[i].node) : slotCol(0), 0.0f);
        if (i == n)
            considerOut({0, i, 0}, slotCol(0), 0.0f, 0.0f);
    }
    for (int i = 0; i < n && !group; ++i) { // an effect onto a band too (a split stays on the main line)
        if (!fxNodeSplit(d.rest[i].node))
            continue;
        for (int lane = 1; lane <= 2; ++lane) {
            const std::vector<int> &band = lane == 1 ? d.rest[i].hi : d.rest[i].lo;
            const int size = static_cast<int>(band.size());
            for (int k = 0; k <= size; ++k) {
                const float prev = k > 0 ? colOf(band[k - 1]) : colOf(d.rest[i].node);
                const float next = k < size              ? colOf(band[k])
                                   : d.rest[i].joined ? colOf(fxJoinNode(d.rest[i].node - kFxSlots))
                                                      : slotCol(lane);
                consider({lane, i, k}, prev, next, lane == 1 ? -1.0f : 1.0f);
                if (k == size && !d.rest[i].joined) // an open band's wire bends from its + into OUT
                    considerOut({lane, i, k}, slotCol(lane), lane == 1 ? -1.0f : 1.0f, lane == 1 ? -0.17f : 0.17f);
            }
        }
    }
    if (over.lane >= 0)
        d.at = over;
}

void View::dropFxNode(bool take) {
    if (!fxDrag_)
        return;
    const FxDrag d = std::move(*fxDrag_);
    fxDrag_.reset();
    if (!drag_.moved)
        return; // a click picks it only
    // Its nodes glide from where they were let go of into their places.
    const FxGridLayout grid = fxGridLayout(fxLine());
    const float scroll = fxGraphScroll(grid);
    const Rect area = fxGraphArea();
    for (int n : d.nodes) {
        const Point p{d.from[n].x + drag_.now.x - drag_.start.x, d.from[n].y + drag_.now.y - drag_.start.y};
        fxGlide_.node[n] = {(p.x - area.x - kFxGridLeft + scroll) / kFxGridCol, (p.y - area.center().y) / kFxGridRow};
    }
    fxGlide_.on = true;
    const bool stays = d.at.lane == d.was.lane && d.at.item == d.was.item && d.at.place == d.was.place;
    if (!take || stays)
        return;
    const UndoScope step(controller_);
    writeFxLine(fxLineWith(d.rest, d.item, d.at));
}

bool View::glideFxGraph() {
    if (!fxGlide_.on)
        return false;
    const bool dragging = drag_.kind == DragKind::FxNodeMove && drag_.moved && fxDrag_.has_value();
    if (dragging) { // held at the graph's left or right edge, it scrolls that way, and where it lands follows
        const Rect area = fxGraphArea();
        constexpr float kZone = 28.0f, kStep = 12.0f;
        const float x = drag_.now.x;
        const float pull = x < area.x + kZone           ? -std::min((area.x + kZone - x) / kZone, 1.0f)
                           : x > area.right() - kZone ? std::min((x - area.right() + kZone) / kZone, 1.0f)
                                                      : 0.0f;
        if (pull != 0.0f) {
            const FxGridLayout grid = fxGridLayout(fxLineShown());
            fxGraphScroll_ = std::clamp(fxGraphScroll(grid) + pull * kStep, 0.0f, fxGraphScrollMost(grid));
        }
        retargetFxDrag(drag_.now);
    }
    // A step of the way to where each goes, a third of what is left.
    const FxGridLayout grid = fxGridLayout(fxLineShown());
    bool moving = false;
    auto ease = [&](Point &at, Point to) {
        if (std::abs(to.x - at.x) + std::abs(to.y - at.y) < 0.004f) {
            at = to;
            return;
        }
        at.x += (to.x - at.x) * 0.3f;
        at.y += (to.y - at.y) * 0.3f;
        moving = true;
    };
    for (const auto &p : grid.nodes)
        ease(fxGlide_.node[p.node], {static_cast<float>(p.col), static_cast<float>(p.row)});
    for (const auto &s : grid.slots)
        ease(fxGlide_.slot[s.lane], {static_cast<float>(s.col), static_cast<float>(s.row)});
    Point out{fxGlide_.outCol, 0.0f};
    ease(out, {static_cast<float>(grid.outCol), 0.0f});
    fxGlide_.outCol = out.x;
    if (!dragging && !moving)
        fxGlide_.on = false;
    return true;
}

// --- Mouse

bool View::fxGraphPress(const Widget &w, Point p) {
    switch (w.kind) {
    case Kind::FxMode:
        setFxGraph(w.b == 1);
        return true;
    case Kind::FxNode: // picked; dragged, onto another place
        controller_.layout().fxSelected = w.a;
        controller_.layoutChanged();
        drag_.kind = DragKind::FxNodeMove;
        startFxDrag(w.a);
        return true;
    case Kind::FxWire: // only its + takes a click
        return true;
    case Kind::SplitSolo: { // a join's solos, one at a time: one switched on switches the other off, one undo step
        const int other = paramIndex(w.a, w.b == 0 ? SumParam::SoloHigh : SumParam::SoloLow);
        const UndoScope step(controller_);
        if (value(w.param) >= 0.5)
            editMany({{w.param, 0.0}});
        else
            editMany({{w.param, 1.0}, {other, 0.0}});
        return true;
    }
    case Kind::SplitBand:
        showSplitBand(w.a, w.b);
        return true;
    case Kind::FxBoxFold:
        setSplitFolded(w.a, !splitFolded(w.a));
        hover_.reset();
        return true;
    case Kind::FxBoxGrip: // dragged along the row with its bands; a click does nothing else
        drag_.kind = DragKind::FxMove;
        drag_.scrollStart = fxScroll();
        startRowDrag(w.a);
        return true;
    case Kind::FxBoxAdd: { // the list at the mouse, the effect picked going at the end of the band
        const FxLine line = fxLine();
        const int size = w.b < static_cast<int>(line.size())
                             ? static_cast<int>((w.a == 1 ? line[w.b].hi : line[w.b].lo).size())
                             : 0;
        fxSlot_ = {w.a, w.b, size};
        fxMenuAt_ = menuAtMouse(p, fxMenu({0.0f, 0.0f}));
        fxMenuOpen_ = true;
        hover_.reset();
        return true;
    }
    case Kind::FxSlot: // the list of effects beside it, with a split or a join after them
        fxSlot_ = {w.a, -1, 0};
        fxMenuAt_ = menuAtMouse({w.rect.center().x + 26.0f, w.rect.center().y - 44.0f}, fxMenu({0.0f, 0.0f}, true));
        fxMenuOpen_ = true;
        hover_.reset();
        return true;
    case Kind::FxGap: // the same beside it, a split after the effects only on the main line
        fxSlot_ = {w.a, w.b / kFxGapStride, w.b % kFxGapStride};
        fxMenuAt_ = menuAtMouse({w.rect.center().x + 26.0f, w.rect.center().y - 44.0f}, fxMenu({0.0f, 0.0f}, w.a == 0));
        fxMenuOpen_ = true;
        hover_.reset();
        return true;
    case Kind::FxAddNode: // a split, with its join
        fxMenuOpen_ = false;
        addSplit();
        fxSlot_ = {};
        return true;
    case Kind::FxNodeRemove:
    case Kind::FxNodeDelete:
        removeFxNode(w.a);
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
    if (drag_.kind == DragKind::FxNodeMove) {
        if (drag_.moved && fxDrag_) {
            fxGlide_.on = true; // the others make room for it, gliding
            retargetFxDrag(p);
        }
        return;
    }
    if (drag_.kind != DragKind::SplitPoint)
        return;
    const std::optional<Rect> card = splitCardOf(drag_.widget.a);
    if (!card)
        return;
    const Rect display = splitDisplay(*card);
    controller_.setValue(drag_.widget.param, std::clamp((p.x - display.x) / display.w, 0.0f, 1.0f));
}

// --- Painting

void View::paintFxGraph(Canvas &c) {
    const Rect area = fxGraphArea();
    c.fillRect(area, kDisplayColor, 8.0f);
    const FxLine line = fxLineShown();
    const FxGridLayout grid = fxGridLayout(line);
    const float scroll = fxGraphScroll(grid);
    const FxPlaces at = fxPlacesShown(grid, scroll);
    const int selected = fxSelectedNode();
    const bool dragging = drag_.kind == DragKind::FxNodeMove && drag_.moved && fxDrag_;
    auto carried = [&](int node) {
        return dragging && std::find(fxDrag_->nodes.begin(), fxDrag_->nodes.end(), node) != fxDrag_->nodes.end();
    };
    auto effectColor = [&](int slot) {
        return value(paramIndex(slot, FxParam::Enabled)) >= 0.5 ? fxTypeColor(fxSlotType(slot)) : kFaint;
    };
    c.pushClip(area);
    // The grid's places, faintly: a dot where a node can sit.
    for (int col = 1; col < grid.outCol; ++col)
        for (int row = -1; row <= 1; ++row)
            c.fillCircle(fxGridCell(col, row, scroll), 1.3f, kText.alpha(0.08f));

    // The wires, S-shaped, each in the color of what it comes from with an arrowhead where it goes into a node; the
    // middle of one under the mouse anywhere along it, or whose list is open, or of an empty band, a + in a circle.
    // As loud as what goes along it is, a wire glows in its color under it, and an arrowhead into OUT lights OUT.
    const std::vector<FxGraphWire> wires = fxGraphWires(line, at);
    float outGlow = 0.0f;
    for (const FxGraphWire &w : wires) {
        const std::vector<Point> points = sWire(w.from, w.to);
        const float glow = fxGlow(w.port);
        if (glow > 0.01f) {
            featherLine(c, points.data(), points.size(), 1.6f, 6.0f, w.color.alpha(0.3f * glow));
        }
        if (w.arrow && std::abs(w.to.x + 8.0f - (at.out.x - kFxEndHalfW)) < 0.5f)
            outGlow = std::max(outGlow, glow);
        c.polyline(points.data(), points.size(), w.color.alpha(0.8f), 1.6f);
        if (w.arrow)
            arrowHead(c, w.to, {1.0f, 0.0f}, w.color);
    }
    for (const FxGraphWire &w : wires) {
        if (w.at.lane < 0 || dragging)
            continue;
        const int b = w.at.item * kFxGapStride + w.at.place;
        const bool listed = fxMenuOpen_ && fxSlot_.lane == w.at.lane && fxSlot_.item == w.at.item &&
                            fxSlot_.place == w.at.place;
        const bool hot = hover_ && (hover_->kind == Kind::FxGap || hover_->kind == Kind::FxWire) &&
                         hover_->a == w.at.lane && hover_->b == b;
        // An empty band between a split and its join shows it always.
        const bool empty = w.at.lane > 0 && w.at.item < static_cast<int>(line.size()) &&
                           (w.at.lane == 1 ? line[w.at.item].hi : line[w.at.item].lo).empty();
        if (!listed && !hot && !empty)
            continue;
        const bool onPlus = hover_ && hover_->kind == Kind::FxGap && hover_->a == w.at.lane && hover_->b == b;
        const Point m = wireMiddle(w);
        // A band's in the accent, as the line's + at its end; the main line's in its wire's color.
        const Color color = w.at.lane > 0 ? kAccent : w.color;
        c.fillCircle(m, 8.5f, kDisplayColor);
        c.fillCircle(m, 8.5f, color.alpha(listed ? 0.35f : onPlus ? 0.28f : 0.16f));
        c.strokeCircle(m, 8.5f, color, 1.2f);
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText, 1.5f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText, 1.5f);
    }
    // The bands' names beside each split.
    for (const FxLineItem &item : line)
        if (fxNodeSplit(item.node)) {
            const float x = at.node[item.node].x + 22.0f;
            c.text({x, fxGridCell(0, -1, scroll).y - kFxNodeR - 22.0f, 80.0f, 14.0f}, tr(Str::BandHigh), 9.5f,
                   bandColor(kHighBand), Align::Left, true);
            c.text({x, fxGridCell(0, 1, scroll).y + kFxNodeR + 22.0f, 80.0f, 14.0f}, tr(Str::BandLow), 9.5f,
                   bandColor(kLowBand), Align::Left, true);
        }

    // IN and OUT.
    for (const auto &[p, name] : {std::pair{at.in, Str::NodeFxIn}, std::pair{at.out, Str::NodeFxOut}}) {
        const Rect r = boxAround(p, kFxEndHalfW, kFxEndHalfH);
        if (const float glow = name == Str::NodeFxIn ? fxGlow(kFxPortIn) : outGlow; glow > 0.01f)
            featherRect(c, r, 6.0f, 12.0f, kText.alpha(0.18f * glow));
        c.fillRect(r, kCardColor, 6.0f);
        c.strokeRect(r, kBorder, 1.0f, 6.0f);
        c.text(r, tr(name), 10.0f, kDim, Align::Center, true);
    }

    // A node: an effect a circle with its icon, a split a box with its bands' ports, a join a box as large, each its
    // name under it; or, where a node being dragged would land, its outline dotted.
    // `alpha` draws it fainter: one being dragged.
    auto paintNode = [&](int node, Point center, bool ghost, float alpha = 1.0f) {
        const bool isSelected = node == selected;
        const bool hot = !dragging && hover_ && hover_->kind == Kind::FxNode && hover_->a == node;
        const float width = hot ? 2.4f : 1.6f;
        if (ghost) {
            if (node < kFxSlots)
                c.strokeCircleDotted(center, kFxNodeR, kFaint, 1.3f);
            else
                c.strokeRect(nodeRect(node, center), kFaint, 1.3f, 6.0f, true);
            return;
        }
        auto name = [&](float below, const std::string &text) {
            c.text({center.x - 60.0f, center.y + below + 3.0f, 120.0f, 14.0f}, text, 9.5f, (isSelected ? kText : kDim).alpha(alpha),
                   Align::Center, true);
        };
        // As loud as what comes out of it is, a node glows in its color around it, as an operator does.
        if (node < kFxSlots) {
            const Color color = effectColor(node).alpha(alpha);
            if (const float glow = alpha < 1.0f ? 0.0f : fxGlow(fxSlotPort(node)); glow > 0.01f)
                featherCircle(c, center, kFxNodeR, 16.0f, color.alpha(0.3f * glow));
            if (isSelected)
                c.strokeCircle(center, kFxNodeR + 5.0f, color.alpha(0.5f), 1.5f);
            c.fillCircle(center, kFxNodeR, isSelected ? color.alpha(kSelectedFill) : kCardColor.alpha(alpha));
            c.strokeCircle(center, kFxNodeR, color, width);
            fxIcon(c, {center.x - 10.0f, center.y - 7.0f, 20.0f, 14.0f}, fxSlotType(node), color);
            name(kFxNodeR, fxTitle(fxSlotType(node)));
            return;
        }
        const bool join = fxNodeSum(node) >= 0;
        const Color color = (join ? sumColor() : splitColor()).alpha(alpha);
        const Rect r = nodeRect(node, center);
        const float glow = alpha < 1.0f ? 0.0f
                           : join ? fxGlow(fxSumPort(fxNodeSum(node)))
                                  : std::max(fxGlow(fxBandPort(node - kFxSlots, kHighBand)),
                                             fxGlow(fxBandPort(node - kFxSlots, kLowBand)));
        if (glow > 0.01f)
            featherRect(c, r, 6.0f, 14.0f, kText.alpha(0.18f * glow));
        if (isSelected)
            c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, 9.0f);
        c.fillRect(r, isSelected ? color.alpha(kSelectedFill) : kCardColor.alpha(alpha), 6.0f);
        c.strokeRect(r, color, width, 6.0f);
        if (join) {
            joinIcon(c, {center.x - 8.0f, center.y - 7.0f, 15.0f, 14.0f}, color);
            name(kFxJoinHalfH, trf(Str::NodeJoin, {std::to_string(fxNodeSum(node) + 1)}));
            // A band soloed: an S in a circle at its top left, in the soloed band's color (the accent for both).
            const int sum = fxNodeSum(node);
            const bool high = value(paramIndex(sum, SumParam::SoloHigh)) >= 0.5;
            const bool low = value(paramIndex(sum, SumParam::SoloLow)) >= 0.5;
            if (high || low) {
                const Color mark = (high && low ? kAccent : bandColor(high ? kHighBand : kLowBand)).alpha(alpha);
                const Point m{r.x + 1.0f, r.y + 1.0f};
                c.fillCircle(m, 7.0f, kCardColor.alpha(alpha));
                c.strokeCircle(m, 7.0f, mark, 1.3f);
                c.text({m.x - 7.0f, m.y - 7.5f, 14.0f, 14.0f}, tr(Str::SoloMark), 8.5f, mark, Align::Center, true);
            }
        } else {
            splitIcon(c, {center.x - 8.0f, center.y - 7.0f, 14.0f, 14.0f}, color);
            name(kFxSplitHalfH, trf(Str::NodeSplit, {std::to_string(node - kFxSlots + 1)}));
        }
    };
    for (const auto &place : grid.nodes)
        paintNode(place.node, at.node[place.node], carried(place.node));

    // The node under the mouse: an x in a circle at its upper right, which removes it; faint on a join that something
    // comes after, which can't go yet.
    for (const auto &place : grid.nodes) {
        if (place.node != fxHoveredNode())
            continue;
        const Point x = fxDeleteAt(place.node, at.node[place.node]);
        const bool hot = hover_->kind == Kind::FxNodeDelete;
        const Color ink = hot ? kText : kDim;
        c.fillCircle(x, 7.5f, hot ? kRaised : kCardColor);
        c.strokeCircle(x, 7.5f, ink, 1.2f);
        c.line({x.x - 3.0f, x.y - 3.0f}, {x.x + 3.0f, x.y + 3.0f}, ink, 1.4f);
        c.line({x.x + 3.0f, x.y - 3.0f}, {x.x - 3.0f, x.y + 3.0f}, ink, 1.4f);
    }

    // The +s where a node can go next: a + in a circle, as on a wire's middle, in its lane's color.
    for (const auto &slot : grid.slots) {
        const Point m = at.slot[slot.lane];
        const Color color = kAccent; // a band's as the line's
        const bool listed = fxMenuOpen_ && fxSlot_.lane == slot.lane && fxSlot_.item < 0;
        const bool hot = !dragging && hover_ && hover_->kind == Kind::FxSlot && hover_->a == slot.lane;
        c.fillCircle(m, kFxSlotHalf, kDisplayColor);
        c.fillCircle(m, kFxSlotHalf, color.alpha(listed ? 0.35f : hot ? 0.28f : 0.16f));
        c.strokeCircle(m, kFxSlotHalf, color, 1.2f);
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText, 1.5f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText, 1.5f);
    }

    // What is being dragged, over everything, with the mouse, faint.
    if (dragging)
        for (int n : fxDrag_->nodes)
            paintNode(n,
                      {fxDrag_->from[n].x + drag_.now.x - drag_.start.x, fxDrag_->from[n].y + drag_.now.y - drag_.start.y},
                      false, 0.5f);
    c.popClip();

    // The node it shows, in the inspector.
    if (selected >= 0 && selected < kFxSlots)
        paintFxCard(c, selected, fxInspector(selected), false);
    else if (fxNodeSplit(selected)) // a split and its join share their controls' card
        paintSplitControls(c, selected - kFxSlots, fxInspector(selected), false);
    else if (fxNodeSum(selected) >= 0)
        paintSplitControls(c, fxNodeSum(selected), fxInspector(selected), false);
    else {
        const Rect card = fxInspectorCard();
        c.strokeRect(card, kBorder, 1.0f, 8.0f, true);
        c.text(card, tr(Str::HintFxInspector), 10.5f, kFaint);
    }
}

void View::paintSplitControls(Canvas &c, int split, Rect card, bool row) {
    // On grey: in the row, its container paints it; in the graph, its card.
    if (!row) {
        c.fillRect(card, splitFill(), 8.0f);
        c.strokeRect(card, splitLine(), 1.3f, 8.0f);
    }
    const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.kind == Kind::FxBoxGrip &&
                          drag_.widget.a == fxSplitNode(split)) ||
                         (hover_ && hover_->kind == Kind::FxBoxGrip && hover_->a == fxSplitNode(split) &&
                          drag_.kind == DragKind::None);
    splitIcon(c, fxIconRect(card), grabbed ? kText : kDim);
    c.text({card.x + 36.0f, card.y + 6.0f, card.w - 84.0f, 20.0f}, trf(Str::NodeSplit, {std::to_string(split + 1)}),
           11.0f, kText, Align::Left, true);
    // The bands' responses on the cutoff grid, each in its color, the one shown filled, the crossover's handle where
    // they cut.
    const Rect display = splitDisplay(card);
    c.fillRect(display, kDisplayColor, 6.0f);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
        c.line({x, display.y + 4.0f}, {x, display.bottom() - 4.0f}, kBorder, 1.0f, true);
    }
    const double hz =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)))[0];
    const int slope = slopeDb(filterSlope(value(paramIndex(split, SplitParam::Slope))));
    const int shown = splitLane(split) == 1 ? kHighBand : kLowBand;
    for (const int band : {shown == kLowBand ? kHighBand : kLowBand, shown}) {
        std::vector<Point> points;
        for (int k = 0; k <= 120; ++k) {
            const double norm = k / 120.0;
            points.push_back({display.x + static_cast<float>(norm) * display.w,
                              responseY(display, bandResponse(band, cutoffFromNorm(norm), hz, slope))});
        }
        if (band == shown) {
            std::vector<Point> area = points;
            area.push_back({display.right(), responseY(display, 0.0)});
            area.push_back({display.x, responseY(display, 0.0)});
            c.fillPolygon(area.data(), area.size(), bandColor(band).alpha(0.13f));
            c.polyline(points.data(), points.size(), bandColor(band), 1.8f);
        } else {
            c.polyline(points.data(), points.size(), bandColor(band).alpha(0.5f), 1.4f);
        }
    }
    const Point h = splitHandle(split, display);
    c.line({h.x, display.y + 2.0f}, {h.x, display.bottom() - 2.0f}, kText.alpha(0.3f), 1.0f);
    const bool hot = (hover_ && hover_->kind == Kind::SplitPoint && hover_->a == split) ||
                     (drag_.kind == DragKind::SplitPoint && drag_.widget.a == split);
    c.fillCircle(h, hot ? 7.0f : 6.0f, kText);
    c.strokeCircle(h, hot ? 7.0f : 6.0f, kDisplayColor, 1.5f);
    char text[24];
    std::snprintf(text, sizeof(text), hz >= 1000.0 ? "%.1f kHz" : "%.0f Hz", hz >= 1000.0 ? hz / 1000.0 : hz);
    c.text({h.x - 40.0f, display.bottom() - 18.0f, 80.0f, 14.0f}, text, 9.0f, kText);
}

} // namespace fmvibe::gui
