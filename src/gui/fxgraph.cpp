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
// The grid's row a band's nodes sit on, by lane: the high band's above the main line, the low band's below, a middle
// band's on it.
int laneRow(int lane) {
    return lane == 1 ? -1 : lane == 2 ? 1 : 0;
}

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
    return boxAround(c, kFxNodeR, kFxNodeHalfH);
}

Rect slotRect(Point c) {
    return boxAround(c, kFxSlotHalf, kFxSlotHalf);
}

// A split's and a join's nodes show the bands in their halves, parted by a line slanting this far either side of the
// middle.
constexpr float kSplitSlant = 4.0f;

// A rounded box's outline, clockwise on screen from its upper left corner, a few points along each corner.
std::vector<Point> roundedOutline(Rect r, float radius) {
    constexpr int kSteps = 6;
    const Point corners[] = {{r.x + radius, r.y + radius},
                             {r.right() - radius, r.y + radius},
                             {r.right() - radius, r.bottom() - radius},
                             {r.x + radius, r.bottom() - radius}};
    std::vector<Point> points;
    for (int k = 0; k < 4; ++k)
        for (int step = 0; step <= kSteps; ++step) { // from the left (180°) round through up (270°), and so on
            const float angle = std::numbers::pi_v<float> * (1.0f + 0.5f * static_cast<float>(k) +
                                                             0.5f * static_cast<float>(step) / kSteps);
            points.push_back({corners[k].x + radius * std::cos(angle), corners[k].y + radius * std::sin(angle)});
        }
    return points;
}

// The part of a closed outline on one side of the line through `a` and `b` (`above`: the side up the screen from a
// line running left to right), as a path starting and ending on the line, the cut along it left out: filled, it
// closes along the cut.
std::vector<Point> outlineSide(const std::vector<Point> &outline, Point a, Point b, bool above) {
    auto side = [&](Point p) {
        const float s = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); // below 0 above the line
        return above ? -s : s;
    };
    struct Vertex {
        Point p;
        bool onCut;
    };
    std::vector<Vertex> kept;
    const size_t n = outline.size();
    for (size_t i = 0; i < n; ++i) {
        const Point p = outline[i], q = outline[(i + 1) % n];
        const float sp = side(p), sq = side(q);
        if (sp >= 0.0f)
            kept.push_back({p, false});
        if ((sp >= 0.0f) != (sq >= 0.0f)) {
            const float t = sp / (sp - sq);
            kept.push_back({{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t}, true});
        }
    }
    // The cut runs from where the outline leaves this side to where it comes back: start there, end at the leaving.
    size_t start = 0;
    for (size_t i = 0; i < kept.size(); ++i)
        if (kept[i].onCut && kept[(i + 1) % kept.size()].onCut)
            start = (i + 1) % kept.size();
    std::vector<Point> path;
    for (size_t k = 0; k < kept.size(); ++k)
        path.push_back(kept[(start + k) % kept.size()].p);
    return path;
}

float distanceToSegment(Point p, Point a, Point b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = dx * dx + dy * dy;
    const float t = length > 0.0f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / length, 0.0f, 1.0f) : 0.0f;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
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

// A graph wire's path: level for as long as it can be, bending from one row to another within a gap between two grid
// columns' nodes (kFxWireBend), at its start where it leaves a split's band port (`bendAtStart`), else at its end,
// so it never crosses a node on its way.
template <class Wire>
std::vector<Point> wirePath(const Wire &w) {
    const Point a = w.from, b = w.to;
    if (b.x - a.x <= kFxWireBend || std::abs(b.y - a.y) < 0.5f)
        return sWire(a, b);
    if (w.bendAtStart) {
        std::vector<Point> points = sWire(a, {a.x + kFxWireBend, b.y});
        points.push_back(b);
        return points;
    }
    std::vector<Point> points{a};
    const std::vector<Point> bend = sWire({b.x - kFxWireBend, a.y}, b);
    points.insert(points.end(), bend.begin(), bend.end());
    return points;
}

// Where a wire's + shows: on its path (`points`) halfway across, or where it says (an empty band's).
template <class Wire>
Point wireMiddle(const Wire &w, const std::vector<Point> &points) {
    if (w.plus)
        return *w.plus;
    const float x = (w.from.x + w.to.x) * 0.5f;
    for (size_t k = 1; k < points.size(); ++k)
        if (points[k].x >= x) {
            const Point p = points[k - 1], q = points[k];
            const float t = q.x > p.x ? (x - p.x) / (q.x - p.x) : 0.0f;
            return {x, p.y + t * (q.y - p.y)};
        }
    return {x, (w.from.y + w.to.y) * 0.5f};
}

// Where an item of the line sits: on the main line, or along one of a split's bands.
struct LinePlace {
    int item = -1; // the main line's item it is, or holds it
    int lane = 0;  // 0 the main line, 1 the high band, 2 the low band, 3 the middle band
    int at = 0;    // its place along the band
};

template <class Line>
LinePlace placeOf(const Line &line, int node) {
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        if (line[i].node == node)
            return {i, 0, 0};
        for (int lane : line[i].lanes()) {
            const std::vector<int> &band = line[i].band(lane);
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
        // A split's bands start in the column after it, the high band above the main line, the low one below, a
        // middle one along the main line between the split and its join.
        const int start = col + 1;
        int longest = 0;
        for (int lane : item.lanes()) {
            const std::vector<int> &band = item.band(lane);
            for (size_t k = 0; k < band.size(); ++k)
                grid.nodes.push_back({band[k], start + static_cast<int>(k), laneRow(lane)});
            longest = std::max(longest, static_cast<int>(band.size()));
        }
        // Each band's + in the column after its last node, the longest band's too, and the join (or OUT) after that.
        if (item.joined) {
            grid.nodes.push_back({fxJoinNode(item.node - kFxSlots), start + longest + 1, 0});
        } else {
            for (int lane : item.lanes())
                grid.slots.push_back({lane, start + static_cast<int>(item.band(lane).size()), laneRow(lane)});
            open = true;
        }
        col = start + longest + 1 + (item.joined ? 1 : 0);
    }
    if (!open) // the main line's +, then OUT
        grid.slots.push_back({0, col++, 0});
    grid.outCol = col;
    return grid;
}

float View::fxGraphScrollMost(const FxGridLayout &grid) const {
    // As far as its narrowest box (beside the widest card) needs: picking a node, which may widen the box, never moves
    // the graph under the mouse, a box widened leaving a little room after OUT at the scroll's end instead.
    const float width = kFxGridLeft + static_cast<float>(grid.outCol) * kFxGridCol + kFxEndHalfW + 16.0f;
    return std::max(width - detail::fxGraphArea(1.5f).w, 0.0f);
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
    const Rect r = nodeRect(node, center); // at its box's upper right corner
    return {r.right() - 2.0f, r.y + 2.0f};
}

Rect View::fxInspector(int node) const {
    return fxInspectorCard(node >= 0 && node < kFxSlots ? fxSpan(fxSlotType(node)) : 1.0f);
}

float View::fxInspectorSpan() const {
    const int node = fxSelectedNode();
    return node >= 0 && node < kFxSlots ? fxSpan(fxSlotType(node)) : 1.0f;
}

Rect View::fxGraphArea() const {
    return detail::fxGraphArea(fxInspectorSpan());
}

Rect View::fxGraphScrollTrack() const {
    return detail::fxGraphScrollTrack(fxInspectorSpan());
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
    for (FxLineItem &item : line)
        for (int lane : {1, 2, 3}) {
            std::vector<int> &band = item.band(lane);
            band.erase(std::remove(band.begin(), band.end(), slot), band.end());
        }
    line.erase(std::remove_if(line.begin(), line.end(), [slot](const FxLineItem &i) { return i.node == slot; }),
               line.end());
    // Into the band at `lane` of item `i`, at `at`, or on the main line at `i`.
    auto put = [&](int i, int lane, int at) {
        if (lane == 0) {
            line.insert(line.begin() + std::clamp(i, 0, static_cast<int>(line.size())), FxLineItem{slot, {}, {}, false});
            return;
        }
        std::vector<int> &band = line[i].band(lane);
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
            put(was.item, was.lane, static_cast<int>(line[was.item].band(was.lane).size()));
    } else if (fxSlot_.lane >= 0 && fxSlot_.item >= 0) { // a wire's middle
        put(fxSlot_.item, fxSlot_.lane, fxSlot_.place);
    } else if (fxSlot_.lane == 0 || (fxSlot_.lane > 0 && !open)) { // the main line's +
        put(open ? last : last + 1, 0, 0);
    } else if (fxSlot_.lane > 0) { // a band's +
        put(last, fxSlot_.lane, static_cast<int>(line[last].band(fxSlot_.lane).size()));
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
    writeFxLine(line);
    controller_.layout().fxSelected = slot;
    controller_.layoutChanged();
    revealFxNode(slot);
    revealFxRow(slot);
}

void View::addSplit(int bands) {
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
    line.insert(line.begin() + (between ? std::clamp(fxSlot_.item, 0, static_cast<int>(line.size()))
                                        : static_cast<int>(line.size())),
                item);
    writeFxLine(line);
    controller_.layout().fxSelected = fxSplitNode(split);
    controller_.layoutChanged();
    revealFxNode(fxSplitNode(split));
    revealFxRow(fxSplitNode(split));
}

void View::removeFxNode(int node, int effects) {
    if (node >= 0 && node < kFxSlots) {
        removeEffect(node);
        return;
    }
    FxLine line = fxLine();
    const LinePlace at = placeOf(line, node); // a join's is its split's: it goes with it
    if (at.item < 0)
        return;
    const FxLineItem split = line[at.item];
    std::vector<int> held; // its bands' effects: the high band's first (then a middle band's)
    for (int lane : split.lanes())
        for (int slot : split.band(lane))
            held.push_back(slot);
    if (!held.empty() && effects < 0) { // whether they stay is asked first
        confirm_ = kConfirmSplitRemove;
        confirmSplit_ = split.node - kFxSlots;
        confirmEffects_ = held.size();
        hover_.reset();
        return;
    }
    // A split, or its join: its bands' effects go on the main line in its place, or with it.
    const UndoScope step(controller_);
    line.erase(line.begin() + at.item);
    if (effects != 1) {
        std::vector<FxLineItem> kept;
        for (int slot : held)
            kept.push_back({slot, {}, {}, false});
        line.insert(line.begin() + at.item, kept.begin(), kept.end());
    }
    writeFxLine(line);
    if (effects == 1)
        for (int slot : held) // removed, with their modulation
            removeEffect(slot);
}

std::vector<int> View::fxOtherBands(int slot) const {
    const FxLine line = fxLine();
    const LinePlace at = placeOf(line, slot);
    std::vector<int> lanes;
    if (at.item < 0 || at.lane == 0)
        return lanes;
    for (int lane : {2, 3, 1}) // low, middle, high
        if (lane != at.lane && (lane != 3 || line[at.item].three))
            lanes.push_back(lane);
    return lanes;
}

void View::moveFxToBand(int slot, int lane) {
    FxLine line = fxLine();
    const LinePlace at = placeOf(line, slot);
    if (at.item < 0 || at.lane == 0 || at.lane == lane || (lane == 3 && !line[at.item].three))
        return;
    const UndoScope step(controller_);
    std::vector<int> &from = line[at.item].band(at.lane);
    from.erase(from.begin() + at.at);
    line[at.item].band(lane).push_back(slot);
    writeFxLine(line);
}

bool View::splitThree(int split) const {
    return split >= 0 && split < kFxSplits && std::lround(value(paramIndex(split, SplitParam::Bands))) >= 3;
}

void View::setSplitBands(int split, int bands, int midTo) {
    FxLine line = fxLine();
    const LinePlace at = placeOf(line, fxSplitNode(split));
    if (at.item < 0)
        return;
    FxLineItem &item = line[at.item];
    const bool three = bands >= 3;
    if (three == item.three)
        return;
    if (!three && !item.mid.empty() && midTo < 0) { // where its effects go is asked first
        confirm_ = kConfirmMidBand;
        confirmSplit_ = split;
        confirmEffects_ = item.mid.size();
        hover_.reset();
        return;
    }
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
        writeFxLine(line);
        showSplitBand(split, tabOfLane(3));
        return;
    }
    const std::vector<int> mid = std::move(item.mid);
    item.mid.clear();
    if (midTo == 2)
        item.lo.insert(item.lo.end(), mid.begin(), mid.end());
    else if (midTo == 1)
        item.hi.insert(item.hi.begin(), mid.begin(), mid.end());
    writeFxLine(line);
    if (midTo != 1 && midTo != 2)
        for (int slot : mid) // removed, with their modulation
            removeEffect(slot);
    if (splitLane(split) == 2 && controller_.layout().splitBand[split] == tabOfLane(3)) // the middle one was shown
        showSplitBand(split, midTo == 1 ? tabOfLane(1) : tabOfLane(2));
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
    // What is being dragged shows its placeholder where it would land at once, as a node there would be: the wires
    // run to it there, never to it on its way, while the others glide aside.
    const bool dragging = drag_.kind == DragKind::FxNodeMove && drag_.moved && fxDrag_.has_value();
    auto carried = [&](int node) {
        return dragging && std::find(fxDrag_->nodes.begin(), fxDrag_->nodes.end(), node) != fxDrag_->nodes.end();
    };
    FxPlaces at;
    for (const auto &place : grid.nodes)
        at.node[place.node] = carried(place.node)
                                  ? fxGridCell(place.col, place.row, scroll)
                                  : fxGridAt(fxGlide_.node[place.node].x, fxGlide_.node[place.node].y, scroll);
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
    const Rect area = fxGraphArea(); // the graph scrolls inside its area
    auto add = [&list, area](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        w.clip = area;
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
            const Point m = w.middle;
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
            const std::vector<Point> &points = wire.path;
            for (size_t k = 1; k < points.size(); ++k)
                if (distanceToSegment(p, points[k - 1], points[k]) <= 6.0f)
                    return true;
            return false;
        }
    return false;
}

bool View::fxBandSoloedOut(const FxLineItem &item, int lane) const {
    if (!fxNodeSplit(item.node) || !item.joined)
        return false;
    const int sum = item.node - kFxSlots; // a split's join is the sum of its number
    bool any = false;
    for (int l : item.lanes())
        any = any || value(paramIndex(sum, laneSolo(l))) >= 0.5;
    return any && value(paramIndex(sum, laneSolo(lane))) < 0.5;
}

std::vector<View::FxGraphWire> View::fxGraphWires(const FxLine &line, const FxPlaces &places) const {
    // The main line's wires grey, a split's bands' in their colors all the way into the join (or out, left open).
    const Color mainLine = kDim;
    struct Tail {
        Point at;
        Color color;
        int port;           // what goes along from there
        bool split = false; // right at a split's band port: a wire from there bends to its row as it leaves
    };
    std::vector<FxGraphWire> wires;
    // From each node's right to the next one's left (`to`, an arrowhead's tip where `arrow`); a node clicked in at its
    // middle goes `where`, and so does one dragged onto it anywhere, unless `drop` says otherwise.
    auto wire = [&](Tail from, Point to, bool arrow, FxSlotAt where = {}, std::optional<FxSlotAt> drop = {}) {
        wires.push_back({from.at, arrow ? Point{to.x - 8.0f, to.y} : to, from.color, arrow, where, drop.value_or(where),
                         std::nullopt, from.port});
        wires.back().bendAtStart = from.split; // out of a split's band, next to the main line
    };
    const auto &at = places.node;
    const Point in = places.in, out = places.out;
    // A wire runs on to a + and on from it, leaving room for it; while a node is dragged no + shows, and the wire
    // runs straight through where it would be.
    const bool dragging = drag_.kind == DragKind::FxNodeMove && drag_.moved && fxDrag_.has_value();
    const float plusHalf = dragging ? 0.0f : kFxSlotHalf;
    Tail tail{{in.x + kFxEndHalfW, in.y}, mainLine, kFxPortIn};
    bool open = false;
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        const FxLineItem &item = line[i];
        const Point p = at[item.node];
        if (item.node < kFxSlots) {
            wire(tail, {p.x - kFxNodeR, p.y}, true, {0, i, 0});
            tail = {{p.x + kFxNodeR, p.y}, mainLine, fxSlotPort(item.node)};
            continue;
        }
        wire(tail, {p.x - kFxSplitHalfW, p.y}, true, {0, i, 0});
        // Out of the bands' ports in their colors, along their effects.
        const int split = item.node - kFxSlots;
        std::array<Tail, 4> bands{}; // by lane
        for (int lane : item.lanes()) {
            const int band = laneBand(lane);
            bands[lane] = {fxBandPortAt(p, band), laneColor(lane), fxBandPort(split, band), true};
            const std::vector<int> &effects = item.band(lane);
            for (size_t k = 0; k < effects.size(); ++k) {
                const Point e = at[effects[k]];
                wire(bands[lane], {e.x - kFxNodeR, e.y}, true, {lane, i, static_cast<int>(k)});
                wires.back().faint = fxBandSoloedOut(item, lane);
                bands[lane] = {{e.x + kFxNodeR, e.y}, laneColor(lane), fxSlotPort(effects[k])};
            }
        }
        if (item.joined) { // into the join, a node clicked in there going at its band's end
            const Point j = at[fxJoinNode(item.node - kFxSlots)];
            const float input = fxBandPortAt(j, kHighBand).y - j.y; // level with the split's ports
            for (int lane : item.lanes()) {
                const std::vector<int> &effects = item.band(lane);
                const float row = static_cast<float>(laneRow(lane));
                const Point into{j.x - kFxJoinHalfW, j.y - row * input};
                const FxSlotAt end{lane, i, static_cast<int>(effects.size())};
                const bool faint = fxBandSoloedOut(item, lane);
                if (!effects.empty()) {
                    wire(bands[lane], into, true, end);
                    wires.back().faint = faint;
                    // Its + in the band's next place, a column on from its last node: the join is a column further on.
                    const Point last = at[effects.back()];
                    wires.back().plus = Point{last.x + kFxGridCol, last.y};
                    continue;
                }
                // Empty: out to its +, on its row a column on from the split, and from there into the join.
                const Point plus{p.x + kFxGridCol, p.y + row * kFxGridRow};
                wire(bands[lane], {plus.x - plusHalf, plus.y}, false, end);
                wires.back().plus = plus;
                wires.back().faint = faint;
                wire({{plus.x + plusHalf, plus.y}, bands[lane].color, bands[lane].port}, into, true, {}, end);
                wires.back().faint = faint;
            }
            tail = {{j.x + kFxJoinHalfW, j.y}, mainLine, fxSumPort(item.node - kFxSlots)};
            continue;
        }
        open = true; // each band on to its +, and from there out
        for (int lane : item.lanes()) {
            const Point plus = places.slot[lane];
            const FxSlotAt end{lane, i, static_cast<int>(item.band(lane).size())};
            wire(bands[lane], {plus.x - plusHalf, plus.y}, false, {}, end);
            wire({{plus.x + plusHalf, plus.y}, bands[lane].color, bands[lane].port},
                 {out.x - kFxEndHalfW, out.y + 12.0f * static_cast<float>(laneRow(lane))}, true, {}, end);
        }
    }
    if (!open) {
        const Point plus = places.slot[0];
        const FxSlotAt end{0, static_cast<int>(line.size()), 0};
        wire(tail, {plus.x - plusHalf, plus.y}, false, {}, end);
        wire({{plus.x + plusHalf, plus.y}, tail.color, tail.port}, {out.x - kFxEndHalfW, out.y}, true, {}, end);
    }
    for (FxGraphWire &w : wires) { // each one's path and its + once, for the widgets, the hit tests and the paint
        w.path = wirePath(w);
        w.middle = wireMiddle(w, w.path);
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

Rect View::splitDisplay(Rect card, bool three) {
    return {card.x + 14.0f, card.y + 66.0f, card.w - 28.0f, three ? 70.0f : 104.0f};
}

namespace {
// The bands' responses on a split's display: how much of each band (0..1) goes through at `hz` (by its port's band),
// at its slope (a one-pole and the rest, or a Linkwitz-Riley of second or fourth order; three bands at 12 or 24 dB,
// 6 playing as 12), and where on the display that shows.
double passed(double x, int slope) {
    return slope == 6 ? 1.0 / std::sqrt(1.0 + x * x) : slope == 12 ? 1.0 / (1.0 + x * x) : 1.0 / (1.0 + x * x * x * x);
}
double bandResponse(int band, double hz, std::array<double, 2> crossovers, int slope, bool three) {
    if (!three)
        return band == kLowBand ? passed(hz / crossovers[0], slope) : passed(crossovers[0] / hz, slope);
    const int s = slope > 12 ? 24 : 12;
    if (band == kLowBand)
        return passed(hz / crossovers[0], s);
    if (band == kHighBand)
        return passed(crossovers[1] / hz, s);
    return passed(crossovers[0] / hz, s) * passed(hz / crossovers[1], s);
}
float responseY(Rect display, double level) {
    const float top = display.y + 12.0f, bottom = display.bottom() - 22.0f;
    return bottom - static_cast<float>(level) * (bottom - top);
}
// A band's lane by its port's band (0 low, 1 middle, 2 high).
int bandLane(int band) {
    return band == kHighBand ? 1 : band == 1 ? 3 : 2;
}
} // namespace

double View::splitBandGain(int split, int band) const {
    // As far along as its knob is, from silence to 0 dB: as a gain, half the knob would leave a curve at 3 % of its
    // height.
    const double db = value(fxInputIndex(fxJoinNode(split), laneJoinInput(bandLane(band)), FxInputField::Level));
    return std::clamp((db - kSumFloorDb) / -kSumFloorDb, 0.0, 1.0);
}

Point View::splitHandle(int split, Rect display, int which) const {
    const bool three = splitThree(split);
    const std::array<double, 2> crossovers =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)));
    const double hz = crossovers[which == 1 && three ? 1 : 0];
    const int slope = slopeDb(filterSlope(value(paramIndex(split, SplitParam::Slope))));
    const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
    // On the louder curve of the two bands it parts, where they cross.
    const int below = three ? (which == 1 ? 1 : kLowBand) : kLowBand;
    const int above = three ? (which == 1 ? kHighBand : 1) : kHighBand;
    double at = 0.0;
    for (const int band : {below, above})
        at = std::max(at, bandResponse(band, hz, crossovers, slope, three) * splitBandGain(split, band));
    return {std::clamp(x, display.x, display.right()), responseY(display, at)};
}

int View::splitLane(int split) const {
    const int tab = controller_.layout().splitBand[split];
    return tab == 2 && !splitThree(split) ? 2 : laneOfTab(tab);
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
            const float place = boxColumn(box) - fxScroll();
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
    // The title row: in the row, it drags the container along the line, and folds it, and the x removes the split (in
    // the graph, its node's x does).
    if (row) {
        add(Kind::FxBoxGrip, {card.x, card.y, cardTitleRight(card, false) - card.x, 34.0f}, -1, fxSplitNode(split));
        add(Kind::FxBoxFold, cardFold(card, false), -1, split);
        add(Kind::FxNodeRemove, cardClose(card), -1, fxSplitNode(split));
    }
    const bool three = splitThree(split);
    const std::vector<int> tabs = three ? std::vector<int>{0, 2, 1} : std::vector<int>{0, 1};
    for (int tab : tabs)
        add(Kind::SplitBand, splitTab(card, tabPosition(tab, three), static_cast<int>(tabs.size())), -1, split, tab);
    // The crossovers' handles on the display.
    const Rect display = splitDisplay(card, three);
    for (int which = 0; which < (three ? 2 : 1); ++which) {
        const Point h = splitHandle(split, display, which);
        add(Kind::SplitPoint, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f},
            paramIndex(split, which == 1 ? SplitParam::High : SplitParam::Low), split, which);
    }
    // In the title row after its name, the BANDS switch (over the row's grip). Under the display, the slope's list as a
    // filter's is: 6, 12 or 24 dB (18, which a filter has, it doesn't); three bands 12 or 24 dB.
    for (int bands : {2, 3})
        add(Kind::SplitBands, splitBandsButton(card, bands), -1, split, bands);
    add(Kind::ListButton, splitSlopeButton(display), paramIndex(split, SplitParam::Slope));
    // The knobs: the bands' levels (the high band's wire takes the join's first input, the low band's its second, a
    // middle band's its third), with the crossover between them in two bands, and in three the two crossovers' over
    // them; then the bands' solos, under their levels.
    auto knob = [&](Point at, float size, int param, Str label) {
        Widget w;
        w.kind = Kind::Knob;
        w.rect = knobRect(at, size);
        w.param = param;
        w.label = tr(label);
        list.push_back(w);
    };
    auto level = [&](int lane) { return fxInputIndex(fxJoinNode(split), laneJoinInput(lane), FxInputField::Level); };
    if (three) {
        knob(splitCrossKnob(card, 0), 24.0f, paramIndex(split, SplitParam::Low), Str::LabelCrossLow);
        knob(splitCrossKnob(card, 1), 24.0f, paramIndex(split, SplitParam::High), Str::LabelCrossHigh);
        knob(splitKnob(card, 0), 30.0f, level(2), Str::BandLow);
        knob(splitKnob(card, 1), 30.0f, level(3), Str::BandMid);
        knob(splitKnob(card, 2), 30.0f, level(1), Str::BandHigh);
    } else {
        knob(splitKnob(card, 0), 30.0f, level(2), Str::BandLow);
        knob(splitKnob(card, 1), 30.0f, paramIndex(split, SplitParam::Low), Str::LabelCrossover);
        knob(splitKnob(card, 2), 30.0f, level(1), Str::BandHigh);
    }
    const std::pair<int, int> solos[] = {{0, 2}, {1, 3}, {2, 1}}; // knob's place, lane
    for (const auto &[k, lane] : solos)
        if (three || lane != 3)
            add(Kind::SplitSolo, splitSoloButton(card, k), paramIndex(split, laneSolo(lane)), split, lane);
}

// --- The row

float View::fxBoxSpan(const FxLineItem &item) const {
    const int split = item.node - kFxSlots;
    if (splitFolded(split))
        return kFoldedFx;
    float span = 2.0f * kFxBoxEdge + 1.0f + kFxBoxAddSpan;
    for (int slot : item.band(splitLane(split)))
        span += fxCardSpan(slot);
    return span;
}

float View::boxColumn(const FxRowBox &box) {
    return box.place + (box.folded ? 0.0f : kFxBoxEdge);
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
            float at = boxColumn(box) + 1.0f;
            const std::vector<int> &band = item.band(box.lane);
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
    if (fxRowDerived_) // only kept while a scope goes on
        return *fxRowDerived_;
    FxRow row = fxRowShownOf(fxLineShown());
    if (derivedDepth_ > 0)
        fxRowDerived_ = row;
    return row;
}

View::FxRow View::fxRowShownOf(const FxLine &line) const {
    FxRow row = fxRow(line);
    const FxRowGlide &g = fxRowGlide_;
    if (!g.on)
        return row;
    // The carried one, and what a carried container holds, where it lands; the others where they glide.
    int carriedItem = -1;
    for (const FxRowBox &box : row.boxes)
        if (fxSplitNode(box.split) == g.carried)
            carriedItem = box.item;
    auto carried = [&](const FxRowCard &card) {
        return card.slot == g.carried || (carriedItem >= 0 && card.at.lane != 0 && card.at.item == carriedItem);
    };
    for (FxRowCard &card : row.cards)
        if (g.known[card.slot] && !carried(card))
            card.place = g.place[card.slot];
    for (FxRowBox &box : row.boxes)
        if (const int node = fxSplitNode(box.split); g.known[node] && node != g.carried) { // its dashed card with it
            box.add += g.place[node] - box.place;
            box.place = g.place[node];
        }
    for (size_t i = 0; i < line.size(); ++i)
        if (g.known[line[i].node] && line[i].node != g.carried)
            row.items[i] = g.place[line[i].node];
    row.items.back() = g.end;
    // Faint while it is carried (a container with what it holds).
    if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_) {
        for (FxRowCard &card : row.cards)
            card.faint = card.slot == g.carried;
        for (FxRowBox &box : row.boxes)
            box.faint = box.item == carriedItem;
    }
    return row;
}

void View::startFxRowGlide() {
    if (fxRowGlide_.on)
        return;
    const FxRow row = fxRow(fxLineShown());
    FxRowGlide g;
    g.on = true;
    for (const FxRowCard &card : row.cards) {
        g.place[card.slot] = card.place;
        g.known[card.slot] = true;
    }
    for (const FxRowBox &box : row.boxes) {
        g.place[fxSplitNode(box.split)] = box.place;
        g.known[fxSplitNode(box.split)] = true;
    }
    g.end = row.items.back();
    g.carried = fxDrag_ ? fxDrag_->item.node : -1;
    fxRowGlide_ = g;
}

bool View::glideFxRow() {
    FxRowGlide &g = fxRowGlide_;
    if (!g.on)
        return false;
    if (fxGraph()) {
        g.on = false;
        return true;
    }
    // A step of the way to where each goes, a third of what is left.
    const FxRow row = fxRow(fxLineShown());
    std::array<bool, kFxNodes> shown{};
    bool moving = false;
    auto ease = [&](float &at, float to, bool known) {
        if (!known || std::abs(to - at) < 0.002f) {
            at = to;
            return;
        }
        at += (to - at) * 0.3f;
        moving = true;
    };
    // The carried one, and what a carried container holds, goes where it lands at once.
    int carriedItem = -1;
    for (const FxRowBox &box : row.boxes)
        if (fxSplitNode(box.split) == g.carried)
            carriedItem = box.item;
    for (const FxRowCard &card : row.cards) {
        const bool carried =
            card.slot == g.carried || (carriedItem >= 0 && card.at.lane != 0 && card.at.item == carriedItem);
        ease(g.place[card.slot], card.place, g.known[card.slot] && !carried);
        shown[card.slot] = true;
    }
    for (const FxRowBox &box : row.boxes) {
        const int node = fxSplitNode(box.split);
        ease(g.place[node], box.place, g.known[node] && node != g.carried);
        shown[node] = true;
    }
    ease(g.end, row.items.back(), true);
    g.known = shown;
    if (!moving && !(drag_.kind == DragKind::FxMove && fxDrag_))
        g.on = false;
    return moving;
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
        if (fxSplitNode(box.split) == node) { // its outline, inside the room it keeps
            d.startPlace = boxColumn(box);
            d.span = box.span - (box.folded ? 0.0f : 2.0f * kFxBoxEdge);
        }
    if (place.lane == 0) { // off the main line: an effect, or a split with what it holds
        d.item = line[place.item];
        d.was = {0, place.item, 0};
        line.erase(line.begin() + place.item);
    } else { // off a band
        std::vector<int> &band = line[place.item].band(place.lane);
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
    // An effect carried over a container's tab goes to the end of that band, let go of there; the container keeps
    // showing the band it shows.
    if (effect) // the container where it shows, gliding too
        for (const FxRowBox &box : fxRowShown().boxes) {
            if (box.folded)
                continue;
            const Rect column = fxCard(boxColumn(box) - fxScroll(), 1.0f);
            const bool three = splitThree(box.split);
            for (int tab = 0; tab < (three ? 3 : 2); ++tab) {
                if (!splitTab(column, tabPosition(tab, three), three ? 3 : 2).contains(mouse))
                    continue;
                for (int i = 0; i < static_cast<int>(d.rest.size()); ++i)
                    if (d.rest[i].node == fxSplitNode(box.split)) {
                        const int lane = laneOfTab(tab);
                        d.at = {lane, i, static_cast<int>(d.rest[i].band(lane).size())};
                        return;
                    }
            }
        }
    // While the mouse is still across from the card's own place (above or below it too: the card moves freely), it
    // stays where it was: nothing moves until the mouse is clear of it. Across from a container's controls (on the way
    // to its tabs), where it would land stays as it is too.
    const Rect own = fxCard(d.startPlace - fxScroll(), d.span);
    if (mouse.x >= own.x && mouse.x < own.right()) {
        d.at = d.was;
        return;
    }
    for (const FxRowBox &box : fxRowShown().boxes)
        if (!box.folded && fxSplitNode(box.split) != d.item.node) {
            const Rect column = fxCard(boxColumn(box) - fxScroll(), 1.0f);
            if (mouse.x >= column.x && mouse.x < column.right())
                return;
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
        if (!effect || box.folded || middle < boxColumn(box) + 0.75f || middle > box.place + box.span)
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
        std::vector<int> &band = line[at.item].band(at.lane);
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
        std::vector<int> &band = line[place.item].band(place.lane);
        d.item = {taken, {}, {}, false};
        d.was = {place.lane, place.item, place.at};
        band.erase(band.begin() + place.at);
    }
    d.rest = std::move(line);
    d.at = d.was;
    d.nodes = {d.item.node};
    d.nodes.insert(d.nodes.end(), d.item.hi.begin(), d.item.hi.end());
    d.nodes.insert(d.nodes.end(), d.item.lo.begin(), d.item.lo.end());
    d.nodes.insert(d.nodes.end(), d.item.mid.begin(), d.item.mid.end());
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
    std::vector<FxSlotAt> places; // every place it can land in
    auto consider = [&](FxSlotAt at, float prev, float next, float row) {
        places.push_back(at);
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
        for (int lane : d.rest[i].lanes()) {
            const std::vector<int> &band = d.rest[i].band(lane);
            const int size = static_cast<int>(band.size());
            for (int k = 0; k <= size; ++k) {
                const float prev = k > 0 ? colOf(band[k - 1]) : colOf(d.rest[i].node);
                const float next = k < size              ? colOf(band[k])
                                   : d.rest[i].joined ? colOf(fxJoinNode(d.rest[i].node - kFxSlots))
                                                      : slotCol(lane);
                const float row = static_cast<float>(laneRow(lane));
                consider({lane, i, k}, prev, next, row);
                if (k == size && !d.rest[i].joined) // an open band's wire bends from its + into OUT
                    considerOut({lane, i, k}, slotCol(lane), row, 0.17f * row);
            }
        }
    }
    // The mouse anywhere on a wire, its arrowhead too, lands it where that wire goes: read off the line as it will
    // stand once the others have glided aside, so a wire sweeping by under the mouse doesn't take it somewhere else.
    // A wire into or out of its own placeholder keeps the place it has.
    {
        const FxLine shownLine = fxLineShown();
        const FxGridLayout grid = fxGridLayout(shownLine);
        auto restAt = [&](FxSlotAt s) -> std::optional<FxSlotAt> { // a place on the line shown, on the line without it
            auto itemOf = [&](int node) {
                for (int k = 0; k < n; ++k)
                    if (d.rest[k].node == node)
                        return k;
                return -1;
            };
            if (s.item < 0 || s.item > static_cast<int>(shownLine.size()))
                return std::nullopt;
            if (s.lane == 0) {
                if (s.item == static_cast<int>(shownLine.size()))
                    return FxSlotAt{0, n, 0};
                const int node = shownLine[s.item].node;
                if (node == d.item.node)
                    return d.at;
                const int k = itemOf(node);
                return k >= 0 ? std::optional(FxSlotAt{0, k, 0}) : std::nullopt;
            }
            if (s.item == static_cast<int>(shownLine.size()))
                return std::nullopt;
            const int split = shownLine[s.item].node;
            if (split == d.item.node) // inside the split it carries
                return d.at;
            const int k = itemOf(split);
            if (k < 0)
                return std::nullopt;
            const std::vector<int> &band = shownLine[s.item].band(s.lane);
            const std::vector<int> &restBand = d.rest[k].band(s.lane);
            if (s.place >= static_cast<int>(band.size()))
                return FxSlotAt{s.lane, k, static_cast<int>(restBand.size())};
            if (band[s.place] == d.item.node)
                return d.at;
            const auto it = std::find(restBand.begin(), restBand.end(), band[s.place]);
            return it != restBand.end() ? std::optional(FxSlotAt{s.lane, k, static_cast<int>(it - restBand.begin())})
                                        : std::nullopt;
        };
        auto same = [](FxSlotAt a, FxSlotAt b) { return a.lane == b.lane && a.item == b.item && a.place == b.place; };
        float nearest = kFxDropReach;
        for (const FxGraphWire &w : fxGraphWires(shownLine, fxPlacesOf(grid, fxGraphScroll(grid)))) {
            if (w.drop.lane < 0)
                continue;
            float dist = w.arrow ? distanceToSegment(mouse, w.to, {w.to.x + 8.0f, w.to.y}) : 1e30f; // its arrowhead
            for (size_t k = 1; k < w.path.size(); ++k)
                dist = std::min(dist, distanceToSegment(mouse, w.path[k - 1], w.path[k]));
            if (dist > nearest)
                continue;
            const std::optional<FxSlotAt> at = restAt(w.drop);
            if (!at || (!same(*at, d.at) && std::none_of(places.begin(), places.end(),
                                                          [&](FxSlotAt p) { return same(p, *at); })))
                continue;
            nearest = dist;
            over = *at;
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
    // Its nodes take their placeholders' places at once (the others glide on into theirs): where it lands, or, let go
    // of anywhere else, where it was.
    fxGlide_.on = true;
    const bool stays = d.at.lane == d.was.lane && d.at.item == d.was.item && d.at.place == d.was.place;
    if (take && !stays) {
        const UndoScope step(controller_);
        writeFxLine(fxLineWith(d.rest, d.item, d.at));
    }
    for (const auto &p : fxGridLayout(fxLine()).nodes)
        if (std::find(d.nodes.begin(), d.nodes.end(), p.node) != d.nodes.end())
            fxGlide_.node[p.node] = {static_cast<float>(p.col), static_cast<float>(p.row)};
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
    for (const auto &p : grid.nodes) {
        const Point to{static_cast<float>(p.col), static_cast<float>(p.row)};
        if (dragging && std::find(fxDrag_->nodes.begin(), fxDrag_->nodes.end(), p.node) != fxDrag_->nodes.end())
            fxGlide_.node[p.node] = to; // its placeholder, where it would land at once (see fxPlacesShown())
        else
            ease(fxGlide_.node[p.node], to);
    }
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
    case Kind::SplitSolo: // a join's solo, on or off, each band's on its own: the join lets every band soloed through
        edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
        return true;
    case Kind::SplitBands:
        setSplitBands(w.a, w.b);
        return true;
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
                             ? static_cast<int>(line[w.b].band(w.a).size())
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
        addSplit(w.b);
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
    const Rect display = splitDisplay(*card, splitThree(drag_.widget.a));
    controller_.setValue(drag_.widget.param, std::clamp((p.x - display.x) / display.w, 0.0f, 1.0f));
}

// --- Painting

void View::paintFxGraph(Canvas &c) {
    const Rect area = fxGraphArea();
    c.fillRect(area, kDisplayColor, kRadiusCard);
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
    // As loud as what goes along it is, a wire glows in its color under it, and an arrowhead into OUT lights OUT. Along
    // a band its join doesn't hear, the other one soloed, it is faint and doesn't glow.
    const std::vector<FxGraphWire> wires = fxGraphWires(line, at);
    float outGlow = 0.0f;
    for (const FxGraphWire &w : wires) {
        const std::vector<Point> &points = w.path;
        const float glow = w.faint ? 0.0f : fxGlow(w.port);
        if (glow > 0.01f) {
            featherLine(c, points.data(), points.size(), 1.6f, 6.0f, w.color.alpha(0.3f * glow));
        }
        if (w.arrow && std::abs(w.to.x + 8.0f - (at.out.x - kFxEndHalfW)) < 0.5f)
            outGlow = std::max(outGlow, glow);
        const float alpha = w.faint ? kSoloedOutAlpha : 1.0f;
        c.polyline(points.data(), points.size(), w.color.alpha(0.8f * alpha), 1.6f);
        if (w.arrow)
            arrowHead(c, w.to, {1.0f, 0.0f}, w.color.alpha(alpha));
    }
    for (const FxGraphWire &w : wires) {
        if (w.at.lane < 0 || dragging)
            continue;
        const int b = w.at.item * kFxGapStride + w.at.place;
        const bool listed = fxMenuOpen_ && fxSlot_.lane == w.at.lane && fxSlot_.item == w.at.item &&
                            fxSlot_.place == w.at.place;
        const bool hot = hover_ && (hover_->kind == Kind::FxGap || hover_->kind == Kind::FxWire) &&
                         hover_->a == w.at.lane && hover_->b == b;
        // After a split's band's last node (or on an empty band) it shows always, faint until the mouse is on it; on
        // the other wires only while the mouse is on its wire.
        const bool bandEnd = w.at.lane > 0 && w.at.item < static_cast<int>(line.size()) &&
                             w.at.place == static_cast<int>(line[w.at.item].band(w.at.lane).size());
        if (!listed && !hot && !bandEnd)
            continue;
        const bool onPlus = hover_ && hover_->kind == Kind::FxGap && hover_->a == w.at.lane && hover_->b == b;
        const Point m = w.middle;
        // In its wire's color: grey on the main line, a band's color on a band. Faint until the mouse is on it (or its
        // list is open).
        const float shown = (w.faint ? kSoloedOutAlpha : 1.0f) * (listed || onPlus ? 1.0f : kFxPlusIdleAlpha);
        const Color color = w.color.alpha(shown);
        c.fillCircle(m, 8.5f, kDisplayColor);
        c.fillCircle(m, 8.5f, color.alpha(listed ? 0.35f : onPlus ? 0.28f : 0.16f));
        c.strokeCircle(m, 8.5f, color, 1.2f);
        const Color ink = kText.alpha(shown);
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, ink, 1.5f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, ink, 1.5f);
    }
    // The bands' names beside each split, a band its join doesn't hear faint; and the effects on such a band.
    std::array<bool, kFxNodes> soloedOut{};
    for (const FxLineItem &item : line)
        if (fxNodeSplit(item.node)) {
            const Point split = at.node[item.node];
            for (int lane : item.lanes()) {
                const float heard = fxBandSoloedOut(item, lane) ? kSoloedOutAlpha : 1.0f;
                for (int slot : item.band(lane))
                    soloedOut[slot] = heard < 1.0f;
                // The high band's name just over its wire and the low band's just under it, where they level out on
                // their rows; the middle band's over its wire, past where the high band's has climbed away from it,
                // short of the node or + on it.
                const float row = static_cast<float>(laneRow(lane)) * kFxGridRow;
                const float y = lane == 1 ? split.y + row - 18.0f : lane == 2 ? split.y + row + 4.0f : split.y - 17.0f;
                const Str name = lane == 1 ? Str::BandHigh : lane == 2 ? Str::BandLow : Str::BandMid;
                c.text({split.x + (lane == 3 ? 36.0f : 30.0f), y, 80.0f, 14.0f}, tr(name), kTextSmall,
                       laneColor(lane).alpha(heard), Align::Left, true);
            }
        }

    // IN and OUT.
    for (const auto &[p, name] : {std::pair{at.in, Str::NodeFxIn}, std::pair{at.out, Str::NodeFxOut}}) {
        const Rect r = boxAround(p, kFxEndHalfW, kFxEndHalfH);
        if (const float glow = name == Str::NodeFxIn ? fxGlow(kFxPortIn) : outGlow; glow > 0.01f)
            featherRect(c, r, kRadiusControl, 12.0f, kText.alpha(0.18f * glow));
        c.fillRect(r, kCardColor, kRadiusControl);
        c.strokeRect(r, kBorder, 1.0f, kRadiusControl);
        c.text(r, tr(name), kTextLabel, kDim, Align::Center, true);
    }

    // A node: an effect a rounded box with its icon, a split a box with its bands' ports, a join a box as large, each
    // its name under it; or, where a node being dragged would land, its outline dotted.
    // `alpha` draws it fainter: one being dragged, its fill as faint (`fill` below 0), or one on a band its join
    // doesn't hear, filled as the others.
    auto paintNode = [&](int node, Point center, bool ghost, float alpha = 1.0f, float fill = -1.0f) {
        if (fill < 0.0f)
            fill = alpha;
        const bool isSelected = node == selected;
        const bool hot = !dragging && hover_ && hover_->kind == Kind::FxNode && hover_->a == node;
        const float width = hot ? 2.4f : 1.6f;
        if (ghost) {
            c.strokeRect(nodeRect(node, center), kFaint, 1.3f, node < kFxSlots ? kRadiusControl : 6.0f, true);
            return;
        }
        auto name = [&](float below, const std::string &text) {
            c.text({center.x - 60.0f, center.y + below + 3.0f, 120.0f, 14.0f}, text, kTextSmall, (isSelected ? kText : kDim).alpha(alpha),
                   Align::Center, true);
        };
        // As loud as what comes out of it is, a node glows in its color around it, as an operator does.
        if (node < kFxSlots) {
            const Color color = effectColor(node).alpha(alpha);
            const Rect r = nodeRect(node, center);
            if (const float glow = alpha < 1.0f ? 0.0f : fxGlow(fxSlotPort(node)); glow > 0.01f)
                featherRect(c, r, kRadiusControl, 16.0f, color.alpha(0.3f * glow));
            if (isSelected)
                c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, kRadiusControl + 5.0f);
            c.fillRect(r, isSelected ? color.alpha(kSelectedFill) : kCardColor.alpha(fill), kRadiusControl);
            c.strokeRect(r, color, width, kRadiusControl);
            fxIcon(c, {center.x - 10.0f, center.y - 7.0f, 20.0f, 14.0f}, fxSlotType(node), color);
            name(kFxNodeHalfH, fxTitle(fxSlotType(node)));
            return;
        }
        const bool join = fxNodeSum(node) >= 0;
        const int sum = join ? fxNodeSum(node) : node - kFxSlots; // a split's join is the sum of its number
        const bool three = splitThree(sum);
        const Color color = (join ? sumColor() : splitColor()).alpha(alpha);
        const Rect r = nodeRect(node, center);
        const float glow = alpha < 1.0f ? 0.0f
                           : join ? fxGlow(fxSumPort(sum))
                                  : std::max({fxGlow(fxBandPort(sum, kHighBand)), fxGlow(fxBandPort(sum, kLowBand)),
                                              three ? fxGlow(fxBandPort(sum, 1)) : 0.0f});
        if (glow > 0.01f)
            featherRect(c, r, 6.0f, 14.0f, kText.alpha(0.18f * glow));
        if (isSelected)
            c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, 9.0f);
        // Its upper half in the high band's color, its lower half in the low band's, parted by a slight slant (rising
        // across a split, falling across a join, its mirror); split in three, its thirds, the middle band's between. A split's colors come in from grey at its left, where the
        // main line comes in; a join's go back out to grey at its right, where the main line goes on.
        c.fillRect(r, kCardColor.alpha(alpha), 6.0f);
        const Color neutral = join ? sumColor() : splitColor();
        const std::vector<Point> outline = roundedOutline(r, 6.0f);
        const float slant = join ? -kSplitSlant : kSplitSlant;
        const float third = three ? r.h / 6.0f : 0.0f; // the cuts' offsets from the middle
        const Point a{r.x, center.y - third + slant}, b{r.right(), center.y - third - slant};
        const Point a2{r.x, center.y + third + slant}, b2{r.right(), center.y + third - slant};
        auto onCut = [&](Point p, Point q) { // a piece along a cut, where two bands meet: no outline there
            auto on = [](Point x, Point from, Point to) {
                const float dx = to.x - from.x, dy = to.y - from.y;
                return std::abs(dx * (x.y - from.y) - dy * (x.x - from.x)) / std::hypot(dx, dy) < 0.05f;
            };
            return (on(p, a, b) && on(q, a, b)) || (three && on(p, a2, b2) && on(q, a2, b2));
        };
        auto across = [&](float x) { // how far the band's color has come in, 0 grey .. 1
            const float t = std::clamp((x - r.x) / r.w, 0.0f, 1.0f);
            return join ? 1.0f - t : t;
        };
        const float tint = isSelected ? kSelectedFill : 0.1f;
        for (const int band : {kHighBand, 1, kLowBand}) {
            if (band == 1 && !three)
                continue;
            const Color ink = bandColor(band);
            const std::vector<Point> half = band == kHighBand ? outlineSide(outline, a, b, true)
                                            : band == kLowBand ? outlineSide(outline, a2, b2, false)
                                                               : outlineSide(outlineSide(outline, a, b, false), a2, b2, true);
            // Its tint in slices across, each as far from grey as the color has come in there.
            constexpr int kSlices = 7;
            for (int k = 0; k < kSlices; ++k) {
                const float x0 = r.x + r.w * static_cast<float>(k) / kSlices;
                const float x1 = r.x + r.w * static_cast<float>(k + 1) / kSlices;
                const std::vector<Point> slice =
                    outlineSide(outlineSide(half, {x0, 0.0f}, {x0, 1.0f}, true), {x1, 0.0f}, {x1, 1.0f}, false);
                if (slice.size() >= 3)
                    c.fillPolygon(slice.data(), slice.size(),
                                  mix(neutral, ink, across(0.5f * (x0 + x1))).alpha(alpha * tint));
            }
            // Its outline in runs of one color each: cut where it goes 2 px across (straight up and down it stays one
            // piece), each piece's color a mix of grey and the band's for where it is across, in kSteps steps, and
            // the pieces of a step drawn as one polyline, the next run starting where it ends.
            constexpr int kSteps = 12;
            std::vector<Point> run;
            int step = -1;
            auto flush = [&] {
                if (run.size() >= 2)
                    c.polyline(run.data(), run.size(),
                               mix(neutral, ink, static_cast<float>(step) / kSteps).alpha(alpha), width);
            };
            for (size_t i = 0; i + 1 < half.size(); ++i) {
                const Point p = half[i], q = half[i + 1];
                if (onCut(p, q)) {
                    flush();
                    run.clear();
                    step = -1;
                    continue;
                }
                const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(q.x - p.x) / 2.0f)));
                for (int k = 0; k < pieces; ++k) {
                    const Point from{p.x + (q.x - p.x) * k / pieces, p.y + (q.y - p.y) * k / pieces};
                    const Point to{p.x + (q.x - p.x) * (k + 1) / pieces, p.y + (q.y - p.y) * (k + 1) / pieces};
                    const int at = static_cast<int>(std::lround(across(0.5f * (from.x + to.x)) * kSteps));
                    if (at != step) {
                        flush();
                        run = {from};
                        step = at;
                    }
                    run.push_back(to);
                }
            }
            flush();
        }
        if (join) {
            joinIcon(c, {center.x - 8.0f, center.y - 7.0f, 15.0f, 14.0f}, color, three ? 3 : 2);
            name(kFxJoinHalfH, trf(Str::NodeJoin, {std::to_string(fxNodeSum(node) + 1)}));
        } else {
            splitIcon(c, {center.x - 8.0f, center.y - 7.0f, 14.0f, 14.0f}, color, three ? 3 : 2);
            name(kFxSplitHalfH, trf(Str::NodeSplit, {std::to_string(node - kFxSlots + 1)}));
        }
        // A band soloed: an S in a circle at the split's and the join's top left, in the soloed band's color (the
        // accent for more than one). A split left open, with no join, has no solo.
        const bool joined = join || value(paramIndex(sum, SumParam::Used)) >= 0.5;
        const bool high = value(paramIndex(sum, SumParam::SoloHigh)) >= 0.5;
        const bool low = value(paramIndex(sum, SumParam::SoloLow)) >= 0.5;
        const bool mid = three && value(paramIndex(sum, SumParam::SoloMid)) >= 0.5;
        if (joined && (high || low || mid)) {
            const Color mark = (high + low + mid > 1 ? kAccent : bandColor(high ? kHighBand : mid ? 1 : kLowBand))
                                   .alpha(alpha);
            const Point m{r.x + 1.0f, r.y + 1.0f};
            c.fillCircle(m, 7.0f, kCardColor.alpha(alpha));
            c.strokeCircle(m, 7.0f, mark, 1.3f);
            c.text({m.x - 7.0f, m.y - 7.5f, 14.0f, 14.0f}, tr(Str::SoloMark), kTextSmall, mark, Align::Center, true);
        }
    };
    for (const auto &place : grid.nodes)
        paintNode(place.node, at.node[place.node], carried(place.node),
                  place.node < kFxSlots && soloedOut[place.node] ? kSoloedOutAlpha : 1.0f, 1.0f);

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

    // The +s where a node can go next: a + in a circle, as on a wire's middle, in the color of the wire it is on (grey
    // at the main line's end, a band's color at the end of a split left open), faint until the mouse is on it (or its
    // list is open). None while a node is dragged.
    for (const auto &slot : grid.slots) {
        if (dragging)
            break;
        const Point m = at.slot[slot.lane];
        const bool listed = fxMenuOpen_ && fxSlot_.lane == slot.lane && fxSlot_.item < 0;
        const bool hot = hover_ && hover_->kind == Kind::FxSlot && hover_->a == slot.lane;
        const float shown = listed || hot ? 1.0f : kFxPlusIdleAlpha;
        const Color color = (slot.lane > 0 ? laneColor(slot.lane) : kDim).alpha(shown);
        c.fillCircle(m, kFxSlotHalf, kDisplayColor);
        c.fillCircle(m, kFxSlotHalf, color.alpha(listed ? 0.35f : hot ? 0.28f : 0.16f));
        c.strokeCircle(m, kFxSlotHalf, color, 1.2f);
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText.alpha(shown), 1.5f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText.alpha(shown), 1.5f);
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
        c.strokeRect(card, kBorder, 1.0f, kRadiusCard, true);
        c.text(card, tr(Str::HintFxInspector), kTextLabel, kFaint);
    }
}

void View::paintSplitControls(Canvas &c, int split, Rect card, bool row) {
    // On grey: in the row, its container paints it; in the graph, its card.
    if (!row) {
        c.fillRect(card, splitFill(), kRadiusCard);
        c.strokeRect(card, splitLine(), 1.3f, kRadiusCard);
    }
    const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.kind == Kind::FxBoxGrip &&
                          drag_.widget.a == fxSplitNode(split)) ||
                         (hover_ && hover_->kind == Kind::FxBoxGrip && hover_->a == fxSplitNode(split) &&
                          drag_.kind == DragKind::None);
    splitIcon(c, fxIconRect(card), grabbed ? kText : kDim, splitThree(split) ? 3 : 2);
    c.text({card.x + 36.0f, card.y + 6.0f, kSplitTitleW - 4.0f, 20.0f}, trf(Str::NodeSplit, {std::to_string(split + 1)}),
           kTextBody, kText, Align::Left, true);
    // The bands' responses on the cutoff grid, each in its color and lowered by its level, the one shown filled, a
    // handle on each crossover where they cut.
    const bool three = splitThree(split);
    const Rect display = splitDisplay(card, three);
    c.fillRect(display, kDisplayColor, kRadiusCard);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
        c.line({x, display.y + 4.0f}, {x, display.bottom() - 4.0f}, kBorder, 1.0f, true);
    }
    const std::array<double, 2> crossovers =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)));
    const int slope = slopeDb(filterSlope(value(paramIndex(split, SplitParam::Slope))));
    const int shown = laneBand(splitLane(split));
    std::vector<int> bands; // the one shown last, over the others
    for (const int band : {kLowBand, 1, kHighBand})
        if (band != shown && (three || band != 1))
            bands.push_back(band);
    bands.push_back(shown);
    for (const int band : bands) {
        const double gain = splitBandGain(split, band);
        std::vector<Point> points;
        for (int k = 0; k <= 120; ++k) {
            const double norm = k / 120.0;
            points.push_back({display.x + static_cast<float>(norm) * display.w,
                              responseY(display, gain * bandResponse(band, cutoffFromNorm(norm), crossovers, slope, three))});
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
    for (int which = 0; which < (three ? 2 : 1); ++which) {
        const Point h = splitHandle(split, display, which);
        c.line({h.x, display.y + 2.0f}, {h.x, display.bottom() - 2.0f}, kText.alpha(0.3f), 1.0f);
        const bool hot = (hover_ && hover_->kind == Kind::SplitPoint && hover_->a == split && hover_->b == which) ||
                         (drag_.kind == DragKind::SplitPoint && drag_.widget.a == split && drag_.widget.b == which);
        c.fillCircle(h, hot ? 7.0f : 6.0f, kText);
        c.strokeCircle(h, hot ? 7.0f : 6.0f, kDisplayColor, 1.5f);
        const double hz = crossovers[which];
        char text[24];
        std::snprintf(text, sizeof(text), hz >= 1000.0 ? "%.1f kHz" : "%.0f Hz", hz >= 1000.0 ? hz / 1000.0 : hz);
        // Two readouts kept apart, each on its side of the other.
        float x = h.x - 40.0f;
        if (three) {
            const float other = splitHandle(split, display, 1 - which).x;
            const float gap = 48.0f - std::abs(other - h.x);
            if (gap > 0.0f)
                x += (which == 0 ? -0.5f : 0.5f) * gap;
        }
        x = std::clamp(x, display.x - 18.0f, display.right() - 62.0f);
        c.text({x, display.bottom() - 18.0f, 80.0f, 14.0f}, text, kTextSmall, kText);
    }
}

} // namespace fmvibe::gui
