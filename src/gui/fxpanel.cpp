// The FX view (FxPanel, see fxpanel.h): the effects' line of effects and splits from IN to OUT (which FxGraphModel,
// fxgraphmodel.h, reads from the wires, writes back to them and edits) as it shows it: its two views, the row of cards
// (each split a container holding its bands) and the graph on its slot grid, with their widgets, painting, moves,
// glides and scrolling; a split's controls, which both show; the list of effects, its context menu and its questions.

#include "fxpanel.h"
#include "lists.h"

#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "routing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>

namespace fmvibe::gui {

using namespace detail;

namespace {

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

} // namespace

FxPanel::FxPanel(PanelHost &host)
    : host_(host), fx_(host.fxGraphModel()), table_(paramTable()) {}

bool FxPanel::wide() const {
    return controller().layout().view == RoutingView::Fx && !host_.shapeEditorOpen();
}

bool FxPanel::graph() const {
    return controller().layout().fxGraphView;
}

void FxPanel::forgetScoped() {
    line_.forget();
    hitWires_.forget();
    rowShown_.forget();
    rowLength_.forget();
}

// --- The line, and the grid it is laid out on

FxPanel::FxLine FxPanel::fxLine() const {
    return line_.get(host_, [&] { return fx_.line(); });
}

std::vector<int> FxPanel::fxChain() const {
    return FxGraphModel::chain(fxLine());
}

Color FxPanel::fxColor(int slot) const {
    return fxTypeColor(fx_.slotType(slot));
}

float FxPanel::fxCardSpan(int slot) const {
    return fxFolded(slot) ? kFoldedFx : fxSpan(fx_.slotType(slot));
}

bool FxPanel::fxShadow() const {
    return fxChain().size() < static_cast<size_t>(kFxSlots);
}

float FxPanel::rowLength() const {
    return rowLength_.get(host_, [&] { return fxRow(fxLine()).items.back() + (fxShadow() ? 1.0f : 0.0f); });
}

float FxPanel::fxScroll() const {
    return std::clamp(fxScroll_, 0.0f, std::max(rowLength() - kVisibleFx, 0.0f));
}

float FxPanel::graphLength() const {
    return fxGraphScrollMost(fxGridLayout(fxLine())) + fxGraphArea().w;
}

float FxPanel::graphScroll() const {
    return fxGraphScroll(fxGridLayout(fxLine()));
}

FxPanel::FxGridLayout FxPanel::fxGridLayout(const FxLine &line) {
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

float FxPanel::fxGraphScrollMost(const FxGridLayout &grid) const {
    // As far as its narrowest box (beside the widest card) needs: picking a node, which may widen the box, never moves
    // the graph under the mouse, a box widened leaving a little room after OUT at the scroll's end instead.
    const float width = kFxGridLeft + static_cast<float>(grid.outCol) * kFxGridCol + kFxEndHalfW + 16.0f;
    return std::max(width - detail::fxGraphArea(1.5f).w, 0.0f);
}

float FxPanel::fxGraphScroll(const FxGridLayout &grid) const {
    return std::clamp(fxGraphScroll_, 0.0f, fxGraphScrollMost(grid));
}

void FxPanel::revealFxNode(int node) {
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

int FxPanel::fxHoveredNode() const {
    if (drag_.kind == DragKind::FxNodeMove) // not while one is held
        return -1;
    return hover() && (hover()->kind == Kind::FxNode || hover()->kind == Kind::FxNodeDelete ||
                      hover()->kind == Kind::FxNodePower)
               ? hover()->a
               : -1;
}

Point FxPanel::fxDeleteAt(int node, Point center) {
    const Rect r = nodeRect(node, center); // at its box's upper right corner
    return {r.right() - 2.0f, r.y + 2.0f};
}

Point FxPanel::fxPowerAt(int node, Point center) {
    const Rect r = nodeRect(node, center); // at its box's upper left corner, mirroring the x
    return {r.x + 2.0f, r.y + 2.0f};
}

Rect FxPanel::fxInspector(int node) const {
    return fxInspectorCard(node >= 0 && node < kFxSlots ? fxSpan(fx_.slotType(node)) : 1.0f);
}

float FxPanel::fxInspectorSpan() const {
    const int node = fxSelectedNode();
    return node >= 0 && node < kFxSlots ? fxSpan(fx_.slotType(node)) : 1.0f;
}

Rect FxPanel::fxGraphArea() const {
    return detail::fxGraphArea(fxInspectorSpan());
}

Rect FxPanel::fxGraphScrollTrack() const {
    return detail::fxGraphScrollTrack(fxInspectorSpan());
}

int FxPanel::fxSelectedNode() const {
    const int node = controller().layout().fxSelected;
    if (node >= 0 && node < kFxNodes && node != kFxOutNode && fx_.nodeShown(node))
        return node;
    const std::vector<int> chain = fxChain(); // none picked: the first effect, else the first split
    if (!chain.empty())
        return chain.front();
    for (int split = 0; split < kFxSplits; ++split)
        if (fx_.nodeShown(fxSplitNode(split)))
            return fxSplitNode(split);
    return -1;
}

std::optional<Rect> FxPanel::cardOf(int slot) const {
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

void FxPanel::addEffect(FxType type, int copyOf) {
    // Where the list was opened, after the node the graph shows, or a copy after its original; either view scrolls to
    // show it.
    const int slot = fx_.addEffect(type, copyOf, fxSlot_);
    if (slot < 0)
        return;
    effectFilled(slot, copyOf);
    revealFxNode(slot);
    revealFxRow(slot);
}

// What the editor keeps of an effect slot that another effect now fills, or that empties: a shape editor left open on
// its curve closes, and its card shows the band its original's does, or the first.
void FxPanel::effectFilled(int slot, int copyOf) {
    effectGone(slot);
    fxBand_[slot] = copyOf >= 0 ? fxBand_[copyOf] : 0;
}

void FxPanel::effectGone(int slot) {
    host_.shapeGone(kMaxSources + slot);
}

void FxPanel::removeEffect(int slot) {
    fx_.removeEffect(slot);
    effectGone(slot);
}

void FxPanel::clearFx() {
    for (int slot : fx_.clear())
        effectGone(slot);
    fxScroll_ = 0.0f;
    fxGraphScroll_ = 0.0f;
}

void FxPanel::addSplit(int bands) {
    const int node = fx_.addSplit(bands, fxSlot_);
    if (node < 0)
        return;
    revealFxNode(node);
    revealFxRow(node);
}

void FxPanel::removeFxNode(int node, int effects) {
    if (node >= 0 && node < kFxSlots) {
        removeEffect(node);
        return;
    }
    const std::optional<FxLineItem> split = fx_.itemOf(node); // a join's is its split's: it goes with it
    if (!split)
        return;
    size_t held = 0; // its bands' effects
    for (int lane : split->lanes())
        held += split->band(lane).size();
    if (held > 0 && effects < 0) { // whether they stay is asked first
        confirmSplit_ = split->node - kFxSlots;
        confirmEffects_ = held;
        host_.askConfirm(*this, kConfirmSplitRemove);
        return;
    }
    for (int slot : fx_.removeNode(node, effects == 1))
        effectGone(slot);
}

void FxPanel::setSplitBands(int split, int bands, int midTo) {
    if (bands < 3 && midTo < 0)
        if (const std::vector<int> mid = fx_.middleEffects(split); !mid.empty()) { // where they go is asked first
            confirmSplit_ = split;
            confirmEffects_ = mid.size();
            host_.askConfirm(*this, kConfirmMidBand);
            return;
        }
    for (int slot : fx_.setSplitBands(split, bands, midTo))
        effectGone(slot);
}

void FxPanel::setFxGraph(bool on) {
    // Two views of the same line: only what shows changes.
    if (on == fxGraph())
        return;
    controller().layout().fxGraphView = on;
    controller().layoutChanged();
    fxMenuOpen_ = false;
    fxSlot_ = {};
    host_.resetHover();
}

// --- Places

FxPanel::FxPlaces FxPanel::fxPlacesOf(const FxGridLayout &grid, float scroll) const {
    FxPlaces at;
    for (const auto &place : grid.nodes)
        at.node[place.node] = fxGridCell(place.col, place.row, scroll);
    for (const auto &slot : grid.slots)
        at.slot[slot.lane] = fxGridCell(slot.col, slot.row, scroll);
    at.in = fxGridCell(0, 0, scroll);
    at.out = fxGridCell(grid.outCol, 0, scroll);
    return at;
}

FxPanel::FxPlaces FxPanel::fxPlacesShown(const FxGridLayout &grid, float scroll) const {
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

FxPanel::FxLine FxPanel::fxLineShown() const {
    if ((drag_.kind == DragKind::FxNodeMove || drag_.kind == DragKind::FxMove) && drag_.moved && fxDrag_)
        return FxGraphModel::lineWith(fxDrag_->rest, fxDrag_->item, fxDrag_->at);
    return fxLine();
}

// --- Widgets

void FxPanel::fxGraphWidgets(std::vector<Widget> &list) const {
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
            if (place.node < kFxSlots) { // an effect's, not a split's or a join's
                const Point on = fxPowerAt(place.node, at.node[place.node]);
                add(Kind::FxNodePower, {on.x - 8.0f, on.y - 8.0f, 16.0f, 16.0f},
                    paramIndex(place.node, FxParam::Enabled), place.node);
            }
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

std::optional<bool> FxPanel::hits(const Widget &w, Point p) const {
    if (w.kind != Kind::FxWire)
        return std::nullopt;
    return w.rect.contains(p) && fxWireHit(w, p);
}

bool FxPanel::fxWireHit(const Widget &w, Point p) const {
    // The wires where they sit on their cells, worked out once while a scope goes on (each FxWire widget the mouse
    // is over asks).
    const std::vector<FxGraphWire> kept = hitWires_.get(host_, [&] {
        const FxLine line = fxLine();
        const FxGridLayout grid = fxGridLayout(line);
        return fxGraphWires(line, fxPlacesOf(grid, fxGraphScroll(grid)));
    });
    for (const FxGraphWire &wire : kept)
        if (wire.at.lane == w.a && wire.at.item * kFxGapStride + wire.at.place == w.b) {
            const std::vector<Point> &points = wire.path;
            for (size_t k = 1; k < points.size(); ++k)
                if (distanceToSegment(p, points[k - 1], points[k]) <= 6.0f)
                    return true;
            return false;
        }
    return false;
}

std::vector<FxPanel::FxGraphWire> FxPanel::fxGraphWires(const FxLine &line, const FxPlaces &places) const {
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
                wires.back().faint = fx_.bandSoloedOut(item, lane);
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
                const bool faint = fx_.bandSoloedOut(item, lane);
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

float FxPanel::fxGlow(int port) const {
    return port >= 0 && port < kFxPorts ? fxGlow_[port] : 0.0f;
}

// How strongly a port of the effects graph glows as of snapshot `t`, which fxGlow_ follows (see follow()).
float FxPanel::fxGlowTarget(const Telemetry &t, int port) {
    if (port < 0 || port >= kFxPorts || !(t.age < 0.5f))
        return 0.0f;
    const float level = t.fxPortLevels[port];
    if (!(level > 0.0f))
        return 0.0f;
    return std::clamp((20.0f * std::log10(level) - kFxGlowFloorDb) / -kFxGlowFloorDb, 0.0f, 1.0f);
}

// --- A split's controls

Rect FxPanel::splitDisplay(Rect card, bool three) {
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

double FxPanel::splitBandGain(int split, int band) const {
    // As far along as its knob is, from silence to 0 dB: as a gain, half the knob would leave a curve at 3 % of its
    // height.
    const double db = value(fxInputIndex(fxJoinNode(split), laneJoinInput(bandLane(band)), FxInputField::Level));
    return std::clamp((db - kSumFloorDb) / -kSumFloorDb, 0.0, 1.0);
}

Point FxPanel::splitHandle(int split, Rect display, int which) const {
    const bool three = fx_.splitThree(split);
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

std::optional<Rect> FxPanel::splitCardOf(int split) const {
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

void FxPanel::splitControlWidgets(std::vector<Widget> &list, int split, Rect card, bool row) const {
    const WidgetAdder add{list};
    // The title row: in the row, it drags the container along the line, and folds it, and the x removes the split (in
    // the graph, its node's x does).
    if (row) {
        add(Kind::FxBoxGrip, {card.x, card.y, cardTitleRight(card, false) - card.x, 34.0f}, -1, fxSplitNode(split));
        add(Kind::FxBoxFold, cardFold(card, false), -1, split);
        add(Kind::FxNodeRemove, cardClose(card), -1, fxSplitNode(split));
    }
    const bool three = fx_.splitThree(split);
    const std::vector<int> tabs = three ? std::vector<int>{0, 2, 1} : std::vector<int>{0, 1};
    for (int tab : tabs)
        add(Kind::SplitBand, splitTab(card, tabPosition(tab, three), static_cast<int>(tabs.size())), -1, split, tab);
    // The display between two crossovers (or a crossover and its edge) shows that band, under the handles.
    const Rect display = splitDisplay(card, three);
    {
        std::vector<float> cuts = {display.x};
        for (int which = 0; which < (three ? 2 : 1); ++which)
            cuts.push_back(splitHandle(split, display, which).x);
        cuts.push_back(display.right());
        for (size_t k = 0; k < tabs.size(); ++k) {
            // Left to right by place: the low band's, the middle one's, the high one's.
            const int tab = *std::find_if(tabs.begin(), tabs.end(), [&](int t) {
                return tabPosition(t, three) == static_cast<int>(k);
            });
            const float left = cuts[k], right = std::max(cuts[k], cuts[k + 1]);
            add(Kind::SplitArea, {left, display.y, right - left, display.h}, -1, split, tab);
        }
    }
    // The crossovers' handles on the display.
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

float FxPanel::fxBoxSpan(const FxLineItem &item) const {
    const int split = item.node - kFxSlots;
    if (fx_.splitFolded(split))
        return kFoldedFx;
    float span = 2.0f * kFxBoxEdge + 1.0f + kFxBoxAddSpan;
    for (int slot : item.band(fx_.splitLane(split)))
        span += fxCardSpan(slot);
    return span;
}

float FxPanel::boxColumn(const FxRowBox &box) {
    return box.place + (box.folded ? 0.0f : kFxBoxEdge);
}

FxPanel::FxRow FxPanel::fxRow(const FxLine &line) const {
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
        FxRowBox box{split, i, fx_.splitLane(split), place, fxBoxSpan(item), fx_.splitFolded(split), 0.0f};
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

FxPanel::FxRow FxPanel::fxRowShown() const {
    return rowShown_.get(host_, [&] { return fxRowShownOf(fxLineShown()); });
}

FxPanel::FxRow FxPanel::fxRowShownOf(const FxLine &line) const {
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

void FxPanel::startFxRowGlide() {
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

bool FxPanel::glideFxRow() {
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

void FxPanel::revealFxRow(int node) {
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

void FxPanel::startRowDrag(int node) {
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

void FxPanel::retargetRowDrag(Point mouse) {
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
            const bool three = fx_.splitThree(box.split);
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
    // Where the card (or container) carried is along the row, in card places: its left edge where it was picked up,
    // moved with the mouse, held to the cards on screen while the row scrolls under it; and the mouse.
    const float pitch = kFxCardW + kFxGap;
    const float carried =
        std::clamp(d.startPlace - drag_.scrollStart + (mouse.x - drag_.start.x) / pitch + fxScroll(), fxScroll(),
                   std::max(fxScroll() + static_cast<float>(kVisibleFx) - d.span, fxScroll()));
    // The mouse in places, as far as it goes (`over`, beyond the row's ends too: a container at either end of the row
    // has the panel's edge beyond it), and held to the cards on screen while the row scrolls under it.
    const float over = (mouse.x - fxCard(0.0f).x) / pitch + fxScroll();
    const float middle = std::clamp(over, fxScroll(), fxScroll() + kVisibleFx);
    // Along a run of `count` items (the main line's, or a band's cards) with the card's left edge at `at`, it lands
    // past each card it covers a fifth of (kRowGive), coming onto it from either side, so the cards make way as soon
    // as it moves onto them, and past a folded container (`wide`) once its left edge is beyond the container's middle.
    // `was` is the place along them it came from, if any: before it, it is still where it was; from elsewhere, past
    // each item's middle.
    constexpr float kRowGive = 0.2f;
    auto landing = [&](int count, int was, float at, auto placeOf, auto spanOf, auto wide) {
        int to = 0;
        for (int i = 0; i < count; ++i) {
            const float give = wide(i) || was < 0 ? 0.5f : i < was ? 1.0f - kRowGive : kRowGive;
            to += placeOf(i) + give * spanOf(i) < at;
        }
        return to;
    };
    const FxRow row = fxRow(d.rest);
    const int n = static_cast<int>(d.rest.size());
    auto spanAt = [&](int i) { return row.items[i + 1] - row.items[i]; };
    const bool openCarried = fxNodeSplit(d.item.node) && !d.item.joined; // a split left open, which ends the line
    // The containers as the row shows them once it has settled, the card where it lands now (wider by it while it is
    // on a band, after it while it lands before them): the mouse over one, from its controls to its end, it goes by
    // that, as drawn. Left of its controls is before it, the room it keeps there too.
    const FxRow landed = fxRow(fxLineShown());
    for (const FxRowBox &box : landed.boxes) {
        if (box.folded || fxSplitNode(box.split) == d.item.node || over < boxColumn(box) || over > box.place + box.span)
            continue;
        d.byMouse = true; // and from now on, the main line goes by the mouse too (see below)
        // Across from its controls (on the way to its tabs), or a container carried over it, where it would land
        // stays as it is.
        const Rect column = fxCard(boxColumn(box) - fxScroll(), 1.0f);
        if (!effect || mouse.x < column.right())
            return;
        // Over the band it shows: along that band, among its cards as they sit in the container.
        const FxRowBox *restBox = nullptr;
        for (const FxRowBox &b : row.boxes)
            if (b.split == box.split)
                restBox = &b;
        if (!restBox)
            return;
        std::vector<const FxRowCard *> band; // the band, without the card carried
        for (const FxRowCard &card : row.cards)
            if (card.at.lane == restBox->lane && card.at.item == restBox->item)
                band.push_back(&card);
        const float shift = boxColumn(*restBox) - boxColumn(box); // from where the container shows to the line without it
        const bool from = d.was.lane == restBox->lane && d.was.item == restBox->item;
        d.at = {restBox->lane, restBox->item,
                landing(static_cast<int>(band.size()), from ? d.was.place : -1, carried + shift,
                        [&](int i) { return band[i]->place; }, [&](int i) { return band[i]->span; },
                        [](int) { return false; })};
        return;
    }
    int to = 0;
    if (openCarried) {
        to = n;
    } else if (d.byMouse) {
        // Once the mouse has been over a container, by the mouse, as the row shows: out of a container's side it lands
        // right beside it, and past a card once the mouse is beyond that card's middle where it shows (the items before
        // where it lands as they are, those after it a card on).
        to = std::clamp(d.at.item, 0, n);
        if (d.at.lane != 0 && d.at.item < n) // out of a container's band: on the side the mouse left it
            for (const FxRowBox &box : landed.boxes)
                if (fxSplitNode(box.split) == d.rest[d.at.item].node && over > box.place + box.span)
                    to = d.at.item + 1;
        while (to > 0 && middle < row.items[to - 1] + 0.5f * spanAt(to - 1))
            --to;
        while (to < n && middle > row.items[to] + d.span + 0.5f * spanAt(to))
            ++to;
    } else {
        to = landing(n, d.was.lane == 0 ? d.was.item : -1, carried, [&](int i) { return row.items[i]; }, spanAt,
                     [&](int i) { return fxNodeSplit(d.rest[i].node); });
    }
    if (openSplit(d.rest)) // nothing goes after a split left open, which ends the line
        to = std::min(to, n - 1);
    d.at = {0, to, 0};
}

void FxPanel::dropRowDrag() {
    if (!fxDrag_)
        return;
    const FxDrag d = std::move(*fxDrag_);
    fxDrag_.reset();
    const bool stays = d.at.lane == d.was.lane && d.at.item == d.was.item && d.at.place == d.was.place;
    if (!drag_.moved || (stays && !d.copy)) // a copy lands even beside its original
        return;
    const UndoScope step(controller());
    fx_.writeLine(FxGraphModel::lineWith(d.rest, d.item, d.at));
}

std::optional<FxPanel::FxLineItem> FxPanel::copyFxItem(const FxLineItem &item, std::map<int, int> &copies) {
    const std::optional<FxLineItem> copy = fx_.copyItem(item, copies);
    for (const auto &[slot, to] : copies)
        if (slot < kFxSlots)
            effectFilled(to, slot);
    return copy;
}

// --- Dragging nodes

void FxPanel::startFxDrag(int node) {
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

void FxPanel::retargetFxDrag(Point mouse) {
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

void FxPanel::dropFxNode(bool take) {
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
    if (take && (!stays || d.copy)) { // a copy lands even beside its original
        const UndoScope step(controller());
        fx_.writeLine(FxGraphModel::lineWith(d.rest, d.item, d.at));
    }
    for (const auto &p : fxGridLayout(fxLine()).nodes)
        if (std::find(d.nodes.begin(), d.nodes.end(), p.node) != d.nodes.end())
            fxGlide_.node[p.node] = {static_cast<float>(p.col), static_cast<float>(p.row)};
}

bool FxPanel::glideFxGraph() {
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

bool FxPanel::fxGraphPress(const Widget &w, Point p) {
    switch (w.kind) {
    case Kind::FxMode:
        setFxGraph(w.b == 1);
        return true;
    case Kind::FxNode: // picked; dragged, onto another place
        controller().layout().fxSelected = w.a;
        controller().layoutChanged();
        startDrag(DragKind::FxNodeMove, w, p);
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
    case Kind::SplitArea:
        fx_.showSplitBand(w.a, w.b);
        return true;
    case Kind::FxBoxFold:
        fx_.setSplitFolded(w.a, !fx_.splitFolded(w.a));
        host_.resetHover();
        return true;
    case Kind::FxBoxGrip: // dragged along the row with its bands; a click does nothing else
        startDrag(DragKind::FxMove, w, p);
        drag_.scrollStart = fxScroll();
        startRowDrag(w.a);
        return true;
    case Kind::FxBoxAdd: { // the list at the mouse, the effect picked going at the end of the band
        const FxLine line = fxLine();
        const int size = w.b < static_cast<int>(line.size())
                             ? static_cast<int>(line[w.b].band(w.a).size())
                             : 0;
        fxSlot_ = {w.a, w.b, size};
        fxMenuAt_ = menuAt(p, fxMenu({0.0f, 0.0f}), host_.height());
        fxMenuOpen_ = true;
        host_.resetHover();
        return true;
    }
    case Kind::FxSlot: // the list of effects beside it, with a split or a join after them
        fxSlot_ = {w.a, -1, 0};
        fxMenuAt_ = menuAt({w.rect.center().x + 26.0f, w.rect.center().y - 44.0f}, fxMenu({0.0f, 0.0f}, true),
                           host_.height());
        fxMenuOpen_ = true;
        host_.resetHover();
        return true;
    case Kind::FxGap: // the same beside it, a split after the effects only on the main line
        fxSlot_ = {w.a, w.b / kFxGapStride, w.b % kFxGapStride};
        fxMenuAt_ = menuAt({w.rect.center().x + 26.0f, w.rect.center().y - 44.0f}, fxMenu({0.0f, 0.0f}, w.a == 0),
                           host_.height());
        fxMenuOpen_ = true;
        host_.resetHover();
        return true;
    case Kind::FxAddNode: // a split, with its join
        fxMenuOpen_ = false;
        addSplit(w.b);
        fxSlot_ = {};
        return true;
    case Kind::FxNodeRemove:
    case Kind::FxNodeDelete:
        removeFxNode(w.a);
        host_.resetHover();
        return true;
    case Kind::FxNodePower: // the effect on or off; the mouse stays over its node
        edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
        return true;
    case Kind::SplitPoint:
        startDrag(DragKind::SplitPoint, w, p);
        drag_.startValue = value(w.param);
        controller().beginEdit(w.param);
        fxGraphDrag(p);
        return true;
    default:
        return false;
    }
}

void FxPanel::fxGraphDrag(Point p) {
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
    const Rect display = splitDisplay(*card, fx_.splitThree(drag_.widget.a));
    controller().setValue(drag_.widget.param, std::clamp((p.x - display.x) / display.w, 0.0f, 1.0f));
}

// --- Painting

void FxPanel::paintFxGraph(Canvas &c) {
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
        return value(paramIndex(slot, FxParam::Enabled)) >= 0.5 ? fxTypeColor(fx_.slotType(slot)) : kFaint;
    };
    c.pushClip(area);
    host_.paintBackdrop(c, area, at.out);
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
            // A grey wire's glow would darken a light theme's ground like a shadow: there it glows in the color of the
            // effect it comes out of, or in the accent.
            Color halo = w.color;
            if (lightTheme() && std::max({w.color.r, w.color.g, w.color.b}) - std::min({w.color.r, w.color.g, w.color.b}) < 0.1f)
                halo = w.port >= fxSlotPort(0) && w.port < fxSlotPort(kFxSlots) ? effectColor(w.port - fxSlotPort(0))
                                                                                  : kAccent;
            featherLine(c, points.data(), points.size(), 1.6f, 6.0f, halo.alpha(0.3f * glow));
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
        const bool hot = hover() && (hover()->kind == Kind::FxGap || hover()->kind == Kind::FxWire) &&
                         hover()->a == w.at.lane && hover()->b == b;
        // After a split's band's last node (or on an empty band) it shows always, faint until the mouse is on it; on
        // the other wires only while the mouse is on its wire.
        const bool bandEnd = w.at.lane > 0 && w.at.item < static_cast<int>(line.size()) &&
                             w.at.place == static_cast<int>(line[w.at.item].band(w.at.lane).size());
        if (!listed && !hot && !bandEnd)
            continue;
        const bool onPlus = hover() && hover()->kind == Kind::FxGap && hover()->a == w.at.lane && hover()->b == b;
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
                const float heard = fx_.bandSoloedOut(item, lane) ? kSoloedOutAlpha : 1.0f;
                for (int slot : item.band(lane))
                    soloedOut[slot] = heard < 1.0f;
                // The high band's name just over its wire and the low band's just under it, where they level out on
                // their rows; the middle band's over its wire, past where the high band's has climbed away from it,
                // short of the node or + on it.
                const float row = static_cast<float>(laneRow(lane)) * kFxGridRow;
                const float y = lane == 1 ? split.y + row - 18.0f : lane == 2 ? split.y + row + 4.0f : split.y - 17.0f;
                const Str name = lane == 1 ? Str::BandHigh : lane == 2 ? Str::BandLow : Str::BandMid;
                c.text({split.x + (lane == 3 ? 40.0f : 34.0f), y, 80.0f, 16.0f}, tr(name), kTextLabel,
                       laneColor(lane).alpha(heard), Align::Left, true);
            }
        }

    // How strongly the nodes glow as loud as they are: a quarter less in a flat theme, on its tinted nodes.
    const float nodeGlow = look.flat ? 0.75f : 1.0f;

    // IN and OUT.
    for (const auto &[p, name] : {std::pair{at.in, Str::NodeFxIn}, std::pair{at.out, Str::NodeFxOut}}) {
        const Rect r = boxAround(p, kFxEndHalfW, kFxEndHalfH);
        if (const float glow = name == Str::NodeFxIn ? fxGlow(kFxPortIn) : outGlow; glow > 0.01f)
            featherRect(c, r, kRadiusControl, 14.0f, kText.alpha(0.18f * glow * nodeGlow));
        paintNodeShadow(c, r, kRadiusControl);
        c.fillRect(r, look.flat ? kTrack : kCardColor, kRadiusControl);
        if (!look.flat)
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
        const bool hot = !dragging && hover() && hover()->kind == Kind::FxNode && hover()->a == node;
        const float width = hot ? 2.4f : 1.6f;
        if (ghost) {
            c.strokeRect(nodeRect(node, center), kFaint, 1.3f, kRadiusControl, true);
            return;
        }
        auto name = [&](float below, const std::string &text) {
            c.text({center.x - 60.0f, center.y + below + 3.0f, 120.0f, 16.0f}, text, kTextLabel, (isSelected ? kText : kDim).alpha(alpha),
                   Align::Center, true);
        };
        // As loud as what comes out of it is, a node glows in its color around it, as an operator does.
        if (node < kFxSlots) {
            const Color color = effectColor(node).alpha(alpha);
            const Rect r = nodeRect(node, center);
            if (const float glow = alpha < 1.0f ? 0.0f : fxGlow(fxSlotPort(node)); glow > 0.01f)
                featherRect(c, r, kRadiusControl, 18.0f, color.alpha(0.3f * glow * nodeGlow));
            if (isSelected)
                c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, kRadiusControl + 5.0f);
            if (look.flat) { // filled with a tint of its color, stronger under the mouse and picked, no outline
                if (alpha >= 1.0f) // on a soft shadow, as the routing graph's nodes (not while it is carried)
                    paintNodeShadow(c, r, kRadiusControl);
                c.fillRect(r, kCardColor.alpha(fill), kRadiusControl);
                c.fillRect(r, color.alpha(isSelected ? 0.4f : hot ? 0.3f : 0.18f), kRadiusControl);
            } else {
                c.fillRect(r, isSelected ? color.alpha(kSelectedFill) : kCardColor.alpha(fill), kRadiusControl);
                c.strokeRect(r, color, width, kRadiusControl);
            }
            fxIcon(c, {center.x - 12.0f, center.y - 8.5f, 24.0f, 17.0f}, fx_.slotType(node), color);
            name(kFxNodeHalfH, fxTitle(fx_.slotType(node)));
            return;
        }
        const bool join = fxNodeSum(node) >= 0;
        const int sum = join ? fxNodeSum(node) : node - kFxSlots; // a split's join is the sum of its number
        const bool three = fx_.splitThree(sum);
        const Color color = (join ? sumColor() : splitColor()).alpha(alpha);
        const Rect r = nodeRect(node, center);
        const float glow = alpha < 1.0f ? 0.0f
                           : join ? fxGlow(fxSumPort(sum))
                                  : std::max({fxGlow(fxBandPort(sum, kHighBand)), fxGlow(fxBandPort(sum, kLowBand)),
                                              three ? fxGlow(fxBandPort(sum, 1)) : 0.0f});
        if (glow > 0.01f)
            featherRect(c, r, kRadiusControl, 16.0f, kText.alpha(0.18f * glow * nodeGlow));
        if (isSelected) // as an effect's: its ring 5 px out, its corners as much rounder
            c.strokeRect(r.inset(-5.0f), color.alpha(0.5f), 1.5f, kRadiusControl + 5.0f);
        // Its upper half in the high band's color, its lower half in the low band's, parted by a slight slant (rising
        // across a split, falling across a join, its mirror); split in three, its thirds, the middle band's between. A split's colors come in from grey at its left, where the
        // main line comes in; a join's go back out to grey at its right, where the main line goes on. In a flat theme,
        // as the effects' nodes: filled with its tints more strongly, under the mouse and picked more still, without an
        // outline.
        if (alpha >= 1.0f)
            paintNodeShadow(c, r, kRadiusControl);
        c.fillRect(r, kCardColor.alpha(alpha), kRadiusControl);
        const Color neutral = join ? sumColor() : splitColor();
        const std::vector<Point> outline = roundedOutline(r, kRadiusControl);
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
        const float tint = look.flat ? (isSelected ? 0.4f : hot ? 0.3f : 0.18f) : isSelected ? kSelectedFill : 0.1f;
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
            if (look.flat)
                continue;
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
        const bool hot = hover()->kind == Kind::FxNodeDelete;
        const Color ink = hot ? kText : kDim;
        c.fillCircle(x, 7.5f, nodeDiscFill(hot));
        c.strokeCircle(x, 7.5f, ink, 1.2f);
        c.line({x.x - 3.0f, x.y - 3.0f}, {x.x + 3.0f, x.y + 3.0f}, ink, 1.4f);
        c.line({x.x + 3.0f, x.y - 3.0f}, {x.x - 3.0f, x.y + 3.0f}, ink, 1.4f);
        // An effect's power symbol in a circle at its upper left, as the x: in its color while it is on.
        if (place.node >= kFxSlots)
            continue;
        const Point o = fxPowerAt(place.node, at.node[place.node]);
        const bool hotPower = hover()->kind == Kind::FxNodePower;
        const bool on = value(paramIndex(place.node, FxParam::Enabled)) >= 0.5;
        const Color mark = on ? fxTypeColor(fx_.slotType(place.node)) : hotPower ? kText : kDim;
        c.fillCircle(o, 7.5f, nodeDiscFill(hotPower));
        c.strokeCircle(o, 7.5f, hotPower ? kText : kDim, 1.2f);
        constexpr float kRadius = 3.2f, kGap = 0.75f; // the power symbol, small enough for the circle
        c.arc(o, kRadius, kGap, 2.0f * std::numbers::pi_v<float> - kGap, mark, 1.3f);
        c.line({o.x, o.y - kRadius - 1.0f}, {o.x, o.y - 0.3f}, mark, 1.3f);
    }

    // The +s where a node can go next: a + in a circle, as on a wire's middle, in the color of the wire it is on (grey
    // at the main line's end, a band's color at the end of a split left open), faint until the mouse is on it (or its
    // list is open). None while a node is dragged.
    for (const auto &slot : grid.slots) {
        if (dragging)
            break;
        const Point m = at.slot[slot.lane];
        const bool listed = fxMenuOpen_ && fxSlot_.lane == slot.lane && fxSlot_.item < 0;
        const bool hot = hover() && hover()->kind == Kind::FxSlot && hover()->a == slot.lane;
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
        host_.paintCard(c, kMaxSources + selected, fxInspector(selected), false);
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

void FxPanel::paintSplitControls(Canvas &c, int split, Rect card, bool row) {
    // On grey: in the row, its container paints it; in the graph, its card.
    if (!row) { // on a shadow and outlined as an effect's card is: in a flat theme only while the mouse is over it
        paintCardShadow(c, card);
        c.fillRect(card, splitFill(), kRadiusCard);
        if (!look.flat || (!host_.dragging() && card.contains(host_.mouse())))
            c.strokeRect(card, splitLine(), 1.3f, kRadiusCard);
    }
    const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.kind == Kind::FxBoxGrip &&
                          drag_.widget.a == fxSplitNode(split)) ||
                         (hover() && hover()->kind == Kind::FxBoxGrip && hover()->a == fxSplitNode(split) &&
                          !host_.dragging());
    splitIcon(c, fxIconRect(card), grabbed ? kText : kDim, fx_.splitThree(split) ? 3 : 2);
    c.text({card.x + 36.0f, card.y + kCardTitleY, kSplitTitleW - 4.0f, 20.0f}, trf(Str::NodeSplit, {std::to_string(split + 1)}),
           kTextBody, kText, Align::Left, true);
    // The bands' responses on the cutoff grid, each in its color and lowered by its level, the one shown filled, a
    // handle on each crossover where they cut.
    const bool three = fx_.splitThree(split);
    const Rect display = splitDisplay(card, three);
    c.fillRect(display, kDisplayColor, kRadiusCard);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = display.x + static_cast<float>(normFromCutoff(hz)) * display.w;
        c.line({x, display.y + 4.0f}, {x, display.bottom() - 4.0f}, kBorder, 1.0f, true);
    }
    const std::array<double, 2> crossovers =
        splitCrossovers(value(paramIndex(split, SplitParam::Low)), value(paramIndex(split, SplitParam::High)));
    const int slope = slopeDb(filterSlope(value(paramIndex(split, SplitParam::Slope))));
    const int shown = laneBand(fx_.splitLane(split));
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
        const bool hot = (hover() && hover()->kind == Kind::SplitPoint && hover()->a == split && hover()->b == which) ||
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

// --- The view's widgets

// A card's widgets, where it shows: in the row, or in the effects graph's inspector (`graph`), where it neither folds
// nor moves.
void FxPanel::cardWidgets(std::vector<Widget> &list, int slot, size_t pos, Rect card, bool graph) const {
    const WidgetAdder add{list};
    auto at = [slot](FxParam f) { return paramIndex(slot, f); };
    // The title row, as every card's: the icon at the title's left (which also switches the effect on and
    // off, and drags the card like the title does), then the fold button (shown under the mouse), the power switch
    // and the cross. In the graph, only the power switch, in the cross's corner: the node's own cross removes the
    // effect.
    const Rect icon = fxIconRect(card).inset(-4.0f);
    if (!graph && fxFolded(slot)) { // a strip: the icon, a grip, the switch and cross, then the rest unfolds it
        add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
        add(Kind::FxGrip, {card.x, card.y + 30.0f, card.w, 16.0f}, -1, slot, static_cast<int>(pos));
        add(Kind::Toggle, {card.center().x - 10.0f, card.y + 50.0f, 20.0f, 19.0f}, at(FxParam::Enabled));
        add(Kind::FxRemove, {card.center().x - 7.0f, card.y + 73.0f, 14.0f, 21.0f}, -1, slot);
        add(Kind::FxFold, {card.x, card.y + 98.0f, card.w, card.h - 98.0f}, -1, slot);
        return;
    }
    if (!graph) // in the graph, the card stays in the inspector
        add(Kind::FxGrip, {card.x, card.y, cardTitleRight(card) - card.x, 34.0f}, -1, slot,
            static_cast<int>(pos));
    add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
    if (!graph)
        add(Kind::FxFold, cardFold(card), -1, slot);
    add(Kind::Toggle, cardPower(card, !graph), at(FxParam::Enabled));
    if (!graph)
        add(Kind::FxRemove, cardClose(card), -1, slot);
    host_.cardWidgets(list, kMaxSources + slot, card); // what is below its title row
}

void FxPanel::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    add(Kind::FxMenuButton, fxMenuButton());
    const size_t cardsStart = list.size();
    if (fxGraph()) {
        fxGraphWidgets(list);
        if (const int node = fxSelectedNode(); node >= 0 && node < kFxSlots)
            cardWidgets(list, node, 0, fxInspector(node), true);
        else if (fxNodeSplit(node)) // a split and its join share their controls' card
            splitControlWidgets(list, node - kFxSlots, fxInspector(node), false);
        else if (fxNodeSum(node) >= 0)
            splitControlWidgets(list, fxNodeSum(node), fxInspector(node), false);
    } else {
        // The row: the effects' cards, and each split's container, its controls, its band's cards and its dashed
        // card that adds to that band.
        const FxRow row = fxRowShown();
        const float scroll = fxScroll();
        for (const FxRowCard &card : row.cards)
            if (const float place = card.place - scroll; onScreen(place, kVisibleFx, card.span))
                cardWidgets(list, card.slot, 0, fxCard(place, card.span), false);
        carriedWidgets_.clear();
        if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && fxDrag_->item.node < kFxSlots &&
            !fxFolded(fxDrag_->item.node)) {
            const size_t mark = list.size();
            cardWidgets(list, fxDrag_->item.node, 0, fxCard(fxDrag_->startPlace - drag_.scrollStart, fxDrag_->span),
                        false);
            carriedWidgets_.assign(list.begin() + static_cast<std::ptrdiff_t>(mark), list.end());
            list.resize(mark);
        }
        for (const FxRowBox &box : row.boxes) {
            const float place = box.place - scroll;
            if (box.folded) { // a strip: the icon and a grip drag it, the cross, then the rest unfolds it
                if (!onScreen(place, kVisibleFx, box.span))
                    continue;
                const Rect strip = fxCard(place, box.span);
                add(Kind::FxBoxGrip, {strip.x, strip.y, strip.w, 46.0f}, -1, fxSplitNode(box.split));
                add(Kind::FxNodeRemove, {strip.center().x - 9.0f, strip.y + 50.0f, 18.0f, 21.0f}, -1,
                    fxSplitNode(box.split));
                add(Kind::FxBoxFold, {strip.x, strip.y + 76.0f, strip.w, strip.h - 76.0f}, -1, box.split);
                continue;
            }
            if (const float column = boxColumn(box) - scroll; onScreen(column, kVisibleFx))
                splitControlWidgets(list, box.split, fxCard(column, 1.0f), true);
            if (const float at = box.add - scroll; fxShadow() && onScreen(at, kVisibleFx, kFxBoxAddSpan)) {
                const Rect card = fxCard(at);
                add(Kind::FxBoxAdd, fxBoxAddCard(card), -1, box.lane, box.item);
            }
        }
        if (const float place = row.items.back() - scroll; fxShadow() && onScreen(place, kVisibleFx))
            add(Kind::FxShadow, fxCard(place));
        // The arrows between the cards, while there is room for another effect: a click adds one there. On the
        // main line before each item, the first too; in a container, before each of its band's cards (the first
        // after its controls). None goes before a dashed card, which adds there itself.
        auto insert = [&](float place, int lane, int item, int at) {
            if (place - scroll >= 0.0f && place - scroll < kVisibleFx) {
                const Rect card = fxCard(place - scroll);
                add(Kind::FxInsert, {card.x - kFxGap * 0.5f - 10.0f, card.center().y - 14.0f, 20.0f, 28.0f}, -1,
                    lane, item * kFxGapStride + at);
            }
        };
        for (size_t i = 0; i + 1 < row.items.size() && fxShadow(); ++i)
            insert(row.items[i], 0, static_cast<int>(i), 0);
        for (const FxRowBox &box : row.boxes) {
            if (box.folded || !fxShadow())
                continue;
            int k = 0;
            for (const FxRowCard &card : row.cards)
                if (card.at.lane == box.lane && card.at.item == box.item)
                    insert(card.place, box.lane, box.item, k++);
        }
    }
    for (size_t i = cardsStart; i < list.size() && !fxGraph(); ++i)
        list[i].clip = fxCardClip();
    for (int graph = 0; graph < 2; ++graph) // the switch between the chain and the graph
        add(Kind::FxMode, fxModeButton(graph), -1, -1, graph);
    mergeButtons(list, list.size() - 2);
    add(Kind::FxClear, fxClearButton());
}

void FxPanel::popups(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    if (fxMenuOpen_ && wide()) {
        // Opened for the main line (from a + or a wire of the graph, from the row's arrows, its dashed card or ADD
        // EFFECT), it offers a split (with its join) after the effects; for a band, effects only.
        const bool withNode = fxSlot_.lane == 0;
        add(Kind::FxMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::FxMenu, fxMenu(fxMenuAt_, withNode), -1, 1);
        for (FxType type : kFxOffered)
            add(Kind::FxAdd, fxMenuItem(fxMenuAt_, type), -1, static_cast<int>(type));
        if (withNode)
            for (int bands : {2, 3})
                add(Kind::FxAddNode, fxMenuNodeItem(fxMenuAt_, bands), -1, -1, bands);
    }
}

// --- Painting the view

void FxPanel::paintBack(Canvas &c) {
    paintFx(c);
}

void FxPanel::paintFx(Canvas &c, std::optional<Rect> only) {
    // Left out with `only`: what isn't across from it.
    auto away = [&](Rect r) { return only && (r.right() <= only->x || r.x >= only->right()); };
    if (!only) {
        const std::vector<int> chain = fxChain();
        // How many of the effects' places are taken, beside ADD EFFECT.
        const Rect count{fxMenuButton().right() + 10.0f, fxMenuButton().y, 44.0f, fxMenuButton().h};
        const bool full = chain.size() >= static_cast<size_t>(kFxSlots);
        c.text(count, std::to_string(chain.size()) + "/" + std::to_string(kFxSlots), kTextBody, full ? kText : kDim,
               Align::Left, full);
        const float hintX = count.right() + 8.0f;
        if (!chain.empty()) // how to work the cards or the graph's nodes, while there are some
            c.text({hintX, kFxPanel.y + 10.0f, fxClearButton().x - 12.0f - hintX, 22.0f},
                   tr(fxGraph() ? Str::HintFxGraph : Str::HintFx), kTextLabel, kFaint, Align::Right);
    }
    if (fxGraph()) {
        paintFxGraph(c);
        return;
    }
    const FxRow row = fxRowShown();
    const FxLine shownLine = fxLineShown();
    const float scroll = fxScroll();
    // In the gap before the card at `place`, an arrow: the signal runs left to right. Under the mouse, or with its
    // list open, it is a plus that adds an effect there (in the line, `at` on `lane` of item `item`).
    auto chevron = [&](float place, int lane, int item, int at, Color color) {
        const Rect next = fxCard(place - scroll);
        const Point m{next.x - kFxGap * 0.5f, next.center().y};
        if (away({m.x - 8.0f, m.y - 8.0f, 16.0f, 16.0f}))
            return;
        const int b = item * kFxGapStride + at;
        const bool lit = (hover() && hover()->kind == Kind::FxInsert && hover()->a == lane && hover()->b == b) ||
                         (fxMenuOpen_ && fxSlot_.lane == lane && fxSlot_.item == item && fxSlot_.place == at);
        if (lit) {
            c.fillCircle(m, 8.0f, kAccent.alpha(0.25f));
            c.strokeCircle(m, 8.0f, kAccent, 1.2f);
            c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText, 1.5f);
            c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText, 1.5f);
            return;
        }
        const Point arrow[] = {{m.x - 3.0f, m.y - 6.0f}, {m.x + 3.0f, m.y}, {m.x - 3.0f, m.y + 6.0f}};
        c.polyline(arrow, 3, color, 1.5f);
    };
    // The grip's dots on a folded strip, lit while it is hovered or held.
    auto gripDots = [&](Rect strip, bool lit) {
        for (int d = 0; d < 6; ++d)
            c.fillCircle({strip.center().x - 5.0f + (d / 2) * 5.0f, strip.y + 35.0f + (d % 2) * 5.0f}, 1.4f,
                         lit ? kText : kFaint);
    };
    c.pushClip(fxCardClip());
    for (size_t i = 0; i + 1 < row.items.size(); ++i) // before each item on the main line, the first too
        chevron(row.items[i], 0, static_cast<int>(i), 0, kFaint);
    // Each split's container under its cards: its outline round them a little out, its controls on grey at its left
    // (out to the outline above, below and before them, and only to their card place after them),
    // then the cards of the band it shows (painted below) and its dashed card (a widget).
    for (const FxRowBox &box : row.boxes) {
        const float place = box.place - scroll;
        if (!onScreen(place, kVisibleFx, box.span))
            continue;
        // Unfolded, inside the room it keeps before and after it.
        const Rect whole = box.folded ? fxCard(place, box.span)
                                      : fxCard(boxColumn(box) - scroll, box.span - 2.0f * kFxBoxEdge);
        if (away(whole.inset(-kFxBoxPad)))
            continue;
        const bool held = drag_.kind == DragKind::FxMove && drag_.widget.kind == Kind::FxBoxGrip &&
                          drag_.widget.a == fxSplitNode(box.split);
        const bool hot = held || (hover() && hover()->kind == Kind::FxBoxGrip &&
                                  hover()->a == fxSplitNode(box.split) && !host_.dragging());
        const Color outline = held ? kText.alpha(0.7f) : splitLine();
        if (box.folded) { // a strip: its icon, a grip, its cross, then its name down the rest
            // On a shadow and outlined as a folded card is: always, or in a flat theme while the mouse is over it.
            paintCardShadow(c, whole);
            c.fillRect(whole, splitFill(), kRadiusCard);
            if (!look.flat || hot || (!host_.dragging() && whole.contains(host_.mouse())))
                c.strokeRect(whole, outline, held ? 1.6f : 1.3f, kRadiusCard);
            splitIcon(c, fxIconRect(whole), hot ? kText : kDim, fx_.splitThree(box.split) ? 3 : 2);
            gripDots(whole, hot);
            paintDownward(c, {whole.x, whole.y + 82.0f, whole.w, whole.h - 90.0f},
                          trf(Str::NodeSplit, {std::to_string(box.split + 1)}), kText);
            continue;
        }
        const Rect outer = whole.inset(-kFxBoxPad);
        const Rect column = fxCard(boxColumn(box) - scroll, 1.0f);
        // The grey ends at the controls' card place, so the arrow into the band is as far from it as from a card.
        const float bandX = column.right();
        paintCardShadow(c, outer, kFxBoxRadius); // as the cards beside it
        c.fillRect(outer, splitFill(), kFxBoxRadius);
        c.fillRect({bandX, outer.y, outer.right() - bandX, outer.h}, kPanel, kFxBoxRadius);
        c.fillRect({bandX, outer.y, kFxBoxRadius, outer.h}, kPanel);
        if (look.flat) { // without its outline, the band shown faintly behind its cards, in the band's color
            c.pushClip({bandX, outer.y, outer.right() - bandX, outer.h});
            c.fillRect(outer, laneColor(box.lane).alpha(look.cardTint * 0.7f), kFxBoxRadius);
            c.popClip();
        }
        // Outlined as a card is: always, or in a theme whose cards go without one, while the mouse is over it.
        if (!look.flat || hot || (!host_.dragging() && outer.contains(host_.mouse())))
            c.strokeRect(outer, outline, held ? 1.6f : 1.3f, kFxBoxRadius);
        host_.noteCard(kMaxSources + kFxSlots + box.split, outer, kFxPanel);
        paintSplitControls(c, box.split, column, true);
        // The arrows along its band, into each of its cards, in the band's color; none into its dashed card.
        // Faint along a band its join doesn't let through, the other band soloed.
        const float heard = box.item < static_cast<int>(shownLine.size()) &&
                                    fx_.bandSoloedOut(shownLine[box.item], box.lane)
                                ? kSoloedOutAlpha
                                : 1.0f;
        const Color band = laneColor(box.lane).alpha(heard);
        int k = 0;
        for (const FxRowCard &card : row.cards)
            if (card.at.lane == box.lane && card.at.item == box.item) {
                chevron(card.place, box.lane, box.item, k, band);
                ++k;
            }
    }
    for (const FxRowCard &card : row.cards) {
        const int slot = card.slot;
        const FxType type = fx_.slotType(slot);
        const float pos = card.place - scroll;
        if (!onScreen(pos, kVisibleFx, card.span))
            continue;
        const Rect r = fxCard(pos, card.span);
        if (away(r))
            continue;
        const bool on = value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
        // Lit while grabbed by its title; by its icon only once it moves, as a click there only switches it.
        const bool dragged = drag_.kind == DragKind::FxMove && drag_.widget.kind != Kind::FxBoxGrip &&
                             drag_.widget.a == slot && (drag_.moved || drag_.widget.kind != Kind::FxPower);
        if (fxFolded(slot)) {
            const Color fx = fxTypeColor(type);
            host_.paintCardFace(c, r, fx, on, kMaxSources + slot, dragged, 0.45f, kFxPanel);
            // its icon and cross (widgets), the grip's dots between, then its name down the strip
            gripDots(r, (hover() && hover()->kind == Kind::FxGrip && hover()->a == slot && !host_.dragging()) ||
                            dragged);
            paintDownward(c, {r.x, r.y + 104.0f, r.w, r.h - 110.0f}, upperCase(fxTitle(type)), on ? kText : kDim);
            continue;
        }
        host_.paintCard(c, kMaxSources + slot, r, dragged);
    }
    c.popClip();

}

void FxPanel::paintOverWidgets(Canvas &c) {
    if (wide() && !fxGraph()) {
        c.pushClip(fxCardClip());
        const FxRow row = fxRowShown();
        // The card or container carried, faint where it would land until it is let go of; and the cards of a band a
        // container shows that its join doesn't let through, the other band soloed.
        const FxLine line = fxLineShown();
        for (const FxRowCard &card : row.cards) {
            const float pos = card.place - fxScroll();
            if (!onScreen(pos, kVisibleFx, card.span))
                continue;
            if (card.faint)
                c.fillRect(fxCard(pos, card.span).inset(-1.0f), kPanel.alpha(0.65f), kRadiusCard + 1.0f);
            else if (card.at.lane != 0 && card.at.item < static_cast<int>(line.size()) &&
                     fx_.bandSoloedOut(line[card.at.item], card.at.lane))
                c.fillRect(fxCard(pos, card.span).inset(-1.0f), kPanel.alpha(1.0f - kSoloedOutAlpha), kRadiusCard + 1.0f);
        }
        for (const FxRowBox &box : row.boxes) {
            if (!box.faint || !onScreen(box.place - fxScroll(), kVisibleFx, box.span))
                continue;
            if (box.folded)
                c.fillRect(fxCard(box.place - fxScroll(), box.span).inset(-1.0f), kPanel.alpha(0.65f), kRadiusCard + 1.0f);
            else
                c.fillRect(fxCard(boxColumn(box) - fxScroll(), box.span - 2.0f * kFxBoxEdge).inset(-kFxBoxPad - 1.0f),
                           kPanel.alpha(0.65f), kFxBoxRadius + 1.0f);
        }
        c.popClip();
    }
}

void FxPanel::paintDrag(Canvas &c) {
    // The card being dragged: where it would land only its outline, and a faint image of it under the mouse wherever
    // it goes (over every panel, held to none), before it snaps into place: an effect card or a split's container. The
    // image is the card painted at its landing place, moved, with the widgets on it (not the row's dashed card, which
    // may glide through there).
    if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && !fxGraph()) {
        const int node = fxDrag_->item.node;
        const FxRow row = fxRowShown();
        std::optional<Rect> landing;
        float radius = kRadiusCard;
        for (const FxRowCard &card : row.cards)
            if (card.slot == node)
                landing = fxCard(card.place - fxScroll(), card.span);
        for (const FxRowBox &box : row.boxes)
            if (fxSplitNode(box.split) == node) {
                landing = box.folded ? fxCard(box.place - fxScroll(), box.span)
                                     : fxCard(boxColumn(box) - fxScroll(), box.span - 2.0f * kFxBoxEdge)
                                           .inset(-kFxBoxPad);
                radius = box.folded ? kRadiusCard : kFxBoxRadius;
            }
        Rect start = fxCard(fxDrag_->startPlace - drag_.scrollStart, fxDrag_->span);
        if (node < kFxSlots && !fxFolded(node)) {
            // An effect card: its outline where it lands, if that shows (not on a band its container hides), and its
            // image painted afresh where it was picked up from, moved under the mouse.
            if (landing)
                host_.paintCardLanding(c, *landing, fxCardClip(), radius, fxColor(node));
            // Its glow and the knobs' labels reach a little past it.
            c.pushLayer({drag_.now.x - drag_.start.x, drag_.now.y - drag_.start.y}, 0.5f, start.inset(-24.0f));
            host_.paintCard(c, kMaxSources + node, start, true);
            for (const Widget &w : carriedWidgets_)
                host_.paintWidget(c, w);
            c.popLayer();
        } else if (landing) { // a container, or a folded card: painted at its landing place, moved
            if (node >= kFxSlots && landing->w > start.w) // a container's outline stands out round it
                start = start.inset(-kFxBoxPad);
            std::vector<Widget> list;
            widgets(list);
            host_.paintCarriedCard(c, *landing, start, fxCardClip(), radius, node < kFxSlots ? fxColor(node) : splitLine(),
                                   [&] { paintFx(c, *landing); }, list);
        }
    }
}

bool FxPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    switch (w.kind) {
    case Kind::FxGrip:
        break; // drawn with its card
    case Kind::FxPower: {
        // The effect's icon: in its color while the effect is on, grey while it is off, as its card is, whatever
        // the mouse does. An effect that is on lights it up while the card is grabbed, by its title or the icon,
        // and under the mouse it sits on a button, as a click switches it.
        const FxType type = fx_.slotType(w.a);
        const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.a == w.a) ||
                             (hover() && hover()->kind == Kind::FxGrip && hover()->a == w.a && !host_.dragging());
        if (hovered)
            c.fillRect(w.rect, kTrack, kRadiusControl);
        const bool on = value(w.param) >= 0.5;
        fxIcon(c, w.rect.inset(4.0f), type, !on ? kDim : (hovered || grabbed ? kText : fxTypeColor(type)));
        break;
    }
    case Kind::FxBoxFold: { // as an effect card's
        if (fx_.splitFolded(w.a)) {
            if (hovered)
                c.fillRect(w.rect.inset(3.0f), kText.alpha(0.06f), kRadiusControl);
            break;
        }
        if (!hovered && !host_.cardFoldShown(kMaxSources + fxSplitNode(w.a)))
            break;
        paintTitleActionFace(c, w.rect, hovered);
        paintChevron(c, w.rect.center(), hovered ? kText : kDim, ChevronWay::Left);
        break;
    }
    case Kind::FxBoxGrip: // the title row: drawn with the split's controls
        break;
    case Kind::SplitBand: {
        // The band's name on the tabs' line after a dot in its color, the one shown underlined in it, as an operator
        // page's tabs are in the accent. A band soloed has an S in its color in its dot's place; a hidden one, how many
        // effects it holds after its name.
        const int lane = laneOfTab(w.b);
        const bool shown = fx_.splitLane(w.a) == lane;
        const Color band = laneColor(lane);
        const char *name = tr(lane == 1 ? Str::TabBandHigh : lane == 3 ? Str::TabBandMid : Str::TabBandLow);
        const float width = c.textWidth(name, kTextLabel, true);
        size_t effects = 0;
        for (const FxLineItem &item : fxLine())
            if (item.node == fxSplitNode(w.a))
                effects = item.band(lane).size();
        const bool soloed = value(paramIndex(w.a, laneSolo(lane))) >= 0.5;
        // The dot (an S in a circle while soloed), the name and its count centered together, so three tabs keep to
        // their own.
        const bool counted = !shown && effects > 0;
        const float lead = soloed ? 4.0f : 0.0f; // the S's circle wider than the dot
        const float x = w.rect.center().x - 0.5f * (width + (counted ? 12.0f : 0.0f) - lead) + 4.0f,
                    y = w.rect.center().y;
        const float base = w.rect.bottom();
        // Lit in its band's color while an effect carried over it would land on that band.
        if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && fxDrag_->item.node < kFxSlots &&
            w.rect.contains(drag_.now)) {
            c.fillRect(w.rect, band.alpha(0.18f), kRadiusControl);
            c.strokeRect(w.rect, band.alpha(0.8f), 1.2f, kRadiusControl);
        }
        c.line({w.rect.x, base - 0.5f}, {w.rect.right(), base - 0.5f}, mix(kCardColor, kDim, 0.25f), 1.0f);
        if (shown)
            c.line({w.rect.x + 8.0f, base - 1.0f}, {w.rect.right() - 8.0f, base - 1.0f}, band, 2.0f);
        if (soloed) {
            const Point badge{x - 11.0f, y};
            c.fillCircle(badge, 6.0f, band);
            c.text({badge.x - 6.0f, badge.y - 6.5f, 12.0f, 13.0f}, tr(Str::SoloMark), kTextSmall, kDisplayColor,
                   Align::Center, true);
        } else {
            c.fillCircle({x - 9.0f, y}, 3.0f, band.alpha(shown ? 1.0f : 0.45f));
        }
        c.text({x, w.rect.y, width + 2.0f, w.rect.h}, name, kTextLabel, shown || hovered ? kText : kDim, Align::Left, true);
        if (counted)
            c.text({x + width + 1.0f, w.rect.y, 16.0f, w.rect.h}, std::to_string(effects), kTextSmall, kDim.alpha(0.8f));
        break;
    }
    case Kind::SplitSolo: { // a plain S, filled in its band's color while soloed
        const bool on = value(w.param) >= 0.5;
        host_.paintButtonFace(c, w, on, hovered, laneColor(w.b), 0.22f, kRadiusControl); // as the other switches
        c.text(w.rect, tr(Str::SoloMark), kTextLabel, on ? litInk(kText) : hovered ? kText : kDim, Align::Center, true);
        break;
    }
    case Kind::FxBoxAdd: {
        // As the dashed card after the effects, narrower: a plus, then which band it adds to.
        const float arm = 9.0f;
        const Point m{w.rect.center().x, w.rect.center().y - 14.0f};
        const Color ink = paintAddCard(c, w.rect, m, arm, hovered);
        // As Add Effect and Add Source are.
        c.text({w.rect.x, m.y + arm + 6.0f, w.rect.w, 14.0f}, tr(Str::ButtonAddToBand), kTextLabel, ink, Align::Center,
               true);
        c.text({w.rect.x, m.y + arm + 20.0f, w.rect.w, 14.0f},
               tr(w.a == 1 ? Str::TabBandHigh : w.a == 3 ? Str::TabBandMid : Str::TabBandLow), kTextLabel, ink,
               Align::Center, true);
        break;
    }
    case Kind::FxMenuButton: // its list, not the dashed card's
        paintMenuButton(c, w.rect, tr(Str::ButtonEffect), hovered, fxMenuOpen_ && fxMenuAt_.x < 0.0f,
                        fxChain().size() >= static_cast<size_t>(kFxSlots));
        break;
    case Kind::FxInsert: // drawn with the arrows between the cards
        break;
    case Kind::FxMode: {
        const bool on = fxGraph() == (w.b == 1);
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        fxModeIcon(c, w.rect.center(), w.b == 1, on ? litInk(kAccent) : (hovered ? kText : kDim));
        break;
    }
    case Kind::FxClear: { // a bin, faint while there is nothing to remove
        binIcon(c, w.rect.center(), paintActionFace(c, w.rect, hovered, fx_.anyToClear()));
        break;
    }
    case Kind::FxNode: // drawn with the effects graph (see paintFxGraph())
    case Kind::FxWire:
    case Kind::FxSlot:
    case Kind::FxGap:
    case Kind::FxNodeDelete:
    case Kind::FxNodePower:
    case Kind::SplitPoint:
    case Kind::SplitArea: // drawn with the split's display
        break;
    case Kind::FxMenu:
        if (w.a == 1) { // the list's panel, with each group's name on a strip over its effects (and from the main line
                        // the splits')
            static constexpr Str kGroups[] = {Str::MenuFxDrive, Str::MenuFxEqFilter,   Str::MenuFxDynamics,
                                              Str::MenuFxSpace, Str::MenuFxModulation, Str::MenuFxStereo,
                                              Str::MenuFxSplit};
            static_assert(std::size(kGroups) == kFxMenuSplitGroup + 1, "each group of the list of effects has its name");
            paintMenuPanel(c, w.rect);
            const int groups = fxSlot_.lane == 0 ? kFxMenuSplitGroup + 1 : kFxMenuGroupCount;
            for (int g = 0; g < groups; ++g) {
                const Rect head = fxMenuHead(fxMenuAt_, g);
                const Rect strip{head.x, head.y, head.w, head.h - 2.0f};
                c.fillRect(strip, kPanel, kRadiusControl);
                c.text({strip.x + 8.0f, strip.y, strip.w - 16.0f, strip.h}, tr(kGroups[g]), kTextSmall, kText,
                       Align::Left, true);
            }
        }
        break;
    case Kind::FxAdd:
        paintMenuRow(c, w.rect, hovered, false);
        fxIcon(c, {w.rect.x + 9.0f, w.rect.center().y - 7.0f, 18.0f, 14.0f}, static_cast<FxType>(w.a),
               hovered ? fxTypeColor(static_cast<FxType>(w.a)) : kDim);
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, fxName(static_cast<FxType>(w.a)), kTextBody,
               hovered ? kText : kDim, Align::Left);
        break;
    case Kind::FxAddNode: { // in the splits' group, under the left column: a split in two bands or three
        const Color color = splitColor();
        const bool three = w.b == 3;
        paintMenuRow(c, w.rect, hovered, false);
        const Rect icon{w.rect.x + 9.0f, w.rect.center().y - 7.0f, 18.0f, 14.0f};
        splitIcon(c, icon, hovered ? color : kDim, three ? 3 : 2);
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, tr(three ? Str::MenuSplitThree : Str::MenuSplitBands),
               kTextBody, hovered ? kText : kDim, Align::Left);
        break;
    }
    case Kind::SplitBands: { // the BANDS switch: 2 | 3, the one it is lit in the accent
        const bool on = (w.b == 3) == fx_.splitThree(w.a);
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl); // as the S beside it
        c.text(w.rect, std::to_string(w.b), kTextLabel, on ? litInk(kText) : hovered ? kText : kDim, Align::Center, true);
        break;
    }
    default:
        return false;
    }
    return true;
}

// --- The mouse on the view

void FxPanel::startDrag(DragKind kind, const Widget &w, Point p) {
    drag_ = Drag{};
    drag_.kind = kind;
    drag_.widget = w;
    drag_.start = drag_.now = p;
    host_.startDrag(*this, w, p, false);
}

void FxPanel::settle() {
    fxRowGlide_.on = false; // the row's cards still settling snap into place, where a click means them
}

void FxPanel::closePopups() {
    fxMenuOpen_ = false;
}

void FxPanel::undoing() {
    fxMenuOpen_ = false; // so is the place in the line the list of effects was opened at
    fxSlot_ = {};
}

bool FxPanel::press(const Widget &w, Point p, Modifiers) {
    if (fxGraphPress(w, p)) // the effects graph's own
        return true;
    switch (w.kind) {
    case Kind::FxMenuButton:
        if (fxChain().size() >= static_cast<size_t>(kFxSlots)) {
            host_.flash(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        } else {
            fxMenuOpen_ = !fxMenuOpen_ || fxMenuAt_.x >= 0.0f; // from another list, to the button's
            fxMenuAt_ = {-1.0f, -1.0f};
            // In the row, at the main line's end; in the graph, after the node picked.
            fxSlot_ = fxGraph() ? FxSlotAt{} : FxSlotAt{0, static_cast<int>(fxLine().size()), 0};
        }
        break;
    case Kind::FxShadow: // the list at the mouse, kept inside the editor, for the main line's end
        fxMenuAt_ = menuAt(p, fxMenu({0.0f, 0.0f}, true), host_.height());
        fxMenuOpen_ = true;
        fxSlot_ = {0, static_cast<int>(fxLine().size()), 0};
        host_.resetHover();
        break;
    case Kind::FxInsert: { // the list over the cards on either side, its middle at the arrow, kept inside the panel
        const Rect menu = fxMenu({0.0f, 0.0f}, w.a == 0); // the main line's with a split after the effects
        fxMenuAt_ = {std::clamp(w.rect.center().x - 0.5f * menu.w, kFxPanel.x + 8.0f, kFxPanel.right() - 8.0f - menu.w),
                     std::min(kFxTop + 8.0f, kFxPanel.bottom() - 4.0f - menu.h)};
        fxMenuOpen_ = true;
        fxSlot_ = {w.a, w.b / kFxGapStride, w.b % kFxGapStride};
        host_.resetHover();
        break;
    }
    case Kind::FxMenu:
        if (w.a == 0) // a click anywhere else closes the list
            fxMenuOpen_ = false;
        break;
    case Kind::FxAdd:
        fxMenuOpen_ = false;
        addEffect(static_cast<FxType>(w.a));
        fxSlot_ = {};
        break;
    case Kind::FxRemove:
        removeEffect(w.a);
        break;
    case Kind::FxClear: // asking first, while there is anything to remove
        if (fx_.anyToClear()) {
            host_.askConfirm(*this, kConfirmFxClear);
        }
        break;
    case Kind::FxGrip:
    case Kind::FxPower: // the icon: let go without moving, it switches the effect
        if (fxGraph()) { // in the graph's inspector, where the card doesn't move
            if (w.kind == Kind::FxPower)
                edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
            break;
        }
        startDrag(DragKind::FxMove, w, p);
        drag_.scrollStart = fxScroll();
        startRowDrag(w.a);
        break;
    case Kind::FxFold:
        fx_.setFolded(true, w.a, !fxFolded(w.a));
        host_.resetHover();
        break;
    default:
        return false;
    }
    return true;
}

// A quick second click on a card's icon, a node's power switch, the chain | graph switch, a + or a node comes as a
// double-click: it plays as a press.
bool FxPanel::doublePress(const Widget &w) const {
    switch (w.kind) {
    case Kind::FxPower:
    case Kind::FxNodePower:
    case Kind::FxMode:
    case Kind::FxSlot:
    case Kind::FxGap:
    case Kind::FxNode:
        return true;
    default:
        return false;
    }
}

void FxPanel::drag(const DragState &d) {
    drag_.start = d.start;
    drag_.now = d.now;
    drag_.moved = d.moved;
    if (d.moved && !drag_.copied) { // it starts to move: with Ctrl held, a copy of it is carried
        drag_.copied = true;
        if (d.mods.ctrl)
            copyDragged();
    }
    switch (drag_.kind) {
    case DragKind::FxMove: // where it would land follows the mouse, the others gliding aside
        if (drag_.moved) {
            startFxRowGlide();
            retargetRowDrag(d.now);
        }
        break;
    case DragKind::FxNodeMove:
    case DragKind::SplitPoint:
        fxGraphDrag(d.now);
        break;
    case DragKind::None:
        break;
    }
}

void FxPanel::drop(const DragState &d) {
    drag_.now = d.now;
    drag_.moved = d.moved;
    drag_.dropped = true;
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::FxMove:
        if (!drag_.moved) { // a click: the icon switches the effect, and the card stays where it is
            fxDrag_.reset();
            if (w.kind == Kind::FxPower)
                edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
            break;
        }
        retargetRowDrag(d.now);
        dropRowDrag(); // where it would land, along the main line or onto a container's band
        break;
    case DragKind::FxNodeMove: // where it is let go of, if it moved
        dropFxNode(true);
        break;
    case DragKind::SplitPoint: // each move set it already
    case DragKind::None:
        break;
    }
}

void FxPanel::release() {
    if (!drag_.dropped) { // called off
        if (drag_.kind == DragKind::FxNodeMove) // back where it was taken from
            dropFxNode(false);
        if (drag_.kind == DragKind::FxMove) // a card or container stays where it was
            fxDrag_.reset();
    }
    if (drag_.kind == DragKind::SplitPoint)
        controller().endEdit(drag_.widget.param);
    drag_ = Drag{};
}

void FxPanel::copyDragged() {
    if ((drag_.kind != DragKind::FxMove && drag_.kind != DragKind::FxNodeMove) || !fxDrag_)
        return;
    host_.beginCopyDrag();
    std::map<int, int> copies;
    const std::optional<FxLineItem> item = copyFxItem(fxDrag_->item, copies);
    if (!item) {
        host_.cancelCopyDrag();
        return;
    }
    // The original back where it was, and the copy carried from right after it.
    FxDrag &d = *fxDrag_;
    d.rest = FxGraphModel::lineWith(d.rest, d.item, d.was);
    d.was = d.was.lane == 0 ? FxSlotAt{0, d.was.item + 1, 0} : FxSlotAt{d.was.lane, d.was.item, d.was.place + 1};
    d.at = d.was;
    d.item = *item;
    for (int &node : d.nodes) {
        d.from[copies[node]] = d.from[node];
        node = copies[node];
    }
    d.copy = true;
    if (const auto grabbed = copies.find(drag_.widget.a); grabbed != copies.end())
        drag_.widget.a = grabbed->second;
    if (drag_.kind == DragKind::FxNodeMove) { // the copy's card in the inspector
        controller().layout().fxSelected = drag_.widget.a;
        controller().layoutChanged();
    }
}

bool FxPanel::dragsOnTouch(const Widget &w) const {
    switch (w.kind) {
    case Kind::FxGrip:
    case Kind::FxPower:
    case Kind::FxNode:
    case Kind::FxBoxGrip:
    case Kind::SplitPoint:
        return true;
    default:
        return false;
    }
}

std::optional<CursorKind> FxPanel::cursor(const Widget &w) const {
    if (drag_.kind == DragKind::FxNodeMove && drag_.moved)
        return CursorKind::Move;
    switch (w.kind) {
    case Kind::SplitPoint:
        return CursorKind::Horizontal;
    case Kind::FxMenu: // an open list's own panel and what is around it, where a click picks nothing
        return CursorKind::Arrow;
    case Kind::FxGrip:
    case Kind::FxBoxGrip:
    case Kind::FxNode:
        return CursorKind::Move;
    case Kind::FxWire: // only its + takes a click
        return CursorKind::Arrow;
    default:
        return std::nullopt;
    }
}

std::string FxPanel::tooltip(const Widget &w) const {
    switch (w.kind) {
    case Kind::FxMode:
        return tr(w.b == 1 ? Str::TipFxGraph : Str::TipFxChain);
    case Kind::FxNode:
        return tr(Str::TipFxNode);
    case Kind::FxSlot:
        return tr(w.a == 0 ? Str::TipFxSlot : Str::TipFxSlotBand);
    case Kind::FxClear:
        return tr(Str::TipFxClear);
    case Kind::FxGap:
        return tr(w.a == 0 ? Str::TipFxGap : Str::TipFxGapBand);
    case Kind::FxNodeRemove:
    case Kind::FxNodeDelete:
        return tr(w.a < kFxSlots           ? Str::TipFxRemove
                  : fxNodeSum(w.a) >= 0   ? Str::TipRemoveJoin
                                          : Str::TipRemoveSplit);
    case Kind::FxNodePower: // only switches, unlike the card's icon, which also drags
        return trf(value(w.param) >= 0.5 ? Str::TipSwitchNodeOff : Str::TipSwitchNodeOn, {fxName(fx_.slotType(w.a))});
    case Kind::FxInsert: // on the main line, a split too; in a container, on its band
        return tr(w.a == 0 ? Str::TipFxInsert : Str::TipFxGapBand);
    case Kind::FxRemove:
        return tr(Str::TipFxRemove);
    case Kind::FxPower: // the icon on its card
        return tr(value(w.param) >= 0.5 ? Str::TipFxOn : Str::TipFxOff);
    case Kind::FxBoxAdd:
        return tr(Str::TipFxSlotBand);
    case Kind::FxBoxGrip:
        return tr(Str::TipSplitGrip);
    case Kind::SplitBand:
        return tr(Str::TipSplitBand);
    case Kind::SplitSolo:
        return tr(Str::TipSplitSolo);
    case Kind::SplitBands:
        return tr(w.b == 3 ? Str::TipSplitThree : Str::TipSplitTwo);
    default:
        return {};
    }
}

// --- What moves by itself

void FxPanel::poll(const Telemetry &t, std::vector<Rect> &repaint) {
    // The effects graph's nodes' and wires' glow follow what they show as the glow behind OUT does (see follow()); the
    // panel is painted again while any moves visibly.
    bool moved = false;
    for (int port = 0; port < kFxPorts; ++port) {
        const float next = follow(fxGlow_[port], fxGlowTarget(t, port));
        moved = moved || std::abs(next - fxGlow_[port]) > 0.002f;
        fxGlow_[port] = next;
    }
    if (moved && wide() && fxGraph() && host_.shownGlow() > 0.0f)
        repaint.push_back(kPanelsBand);
}

bool FxPanel::step() {
    bool moved = false;
    // A card held near either end of the row scrolls it that way, where it lands following.
    if (drag_.kind == DragKind::FxMove && drag_.moved && wide())
        if (const float speed = edgePull(kFxPanel, drag_.now, drag_.start.x); speed != 0.0f) {
            const float before = fxScroll();
            setRowScroll(before + edgeScrollStep(speed));
            if (fxScroll() != before) {
                moved = true;
                retargetRowDrag(drag_.now);
            }
        }
    if (glideFxGraph()) // the effects graph's nodes making room for one dragged, or settling after
        moved = true;
    if (glideFxRow()) // the row's cards and containers likewise
        moved = true;
    return moved;
}

bool FxPanel::scrollToward(Point p) {
    if (!wide())
        return false;
    const float speed = edgePull(kFxPanel, p, std::nullopt);
    if (speed == 0.0f)
        return false;
    const float before = fxScroll();
    setRowScroll(before + edgeScrollStep(speed));
    return fxScroll() != before;
}

// --- Scrolling

bool FxPanel::wheelAcross(Point p, float steps) {
    // As the wheel scrolls them, the other way round: a notch to the right moves on as a notch down does.
    if (!wide())
        return false;
    const std::optional<Widget> hit = host_.widgetAt(p);
    if (fxGraph() && (fxGraphArea().contains(p) || (hit && hit->kind == Kind::Scrollbar))) {
        const FxGridLayout grid = fxGridLayout(fxLine());
        fxGraphScroll_ = std::clamp(fxGraphScroll(grid) + steps * 0.5f * kFxGridCol, 0.0f, fxGraphScrollMost(grid));
        if (fxSlot_.lane >= 0)
            fxMenuOpen_ = false;
        return true;
    }
    if (!fxGraph() && kFxPanel.contains(p)) {
        setRowScroll(fxScroll() + steps * 0.4f);
        host_.closeGoneLists();
        return true;
    }
    return false;
}

bool FxPanel::wheelBehind(Point p, float steps) {
    if (!wide() || !kFxPanel.contains(p) || steps == 0.0f)
        return false;
    if (fxGraph()) {
        const std::optional<Widget> hit = host_.widgetAt(p);
        if (fxGraphArea().contains(p) || (hit && hit->kind == Kind::Scrollbar)) {
            // The graph sideways, half a column a notch, closing a +'s list.
            const FxGridLayout grid = fxGridLayout(fxLine());
            fxGraphScroll_ = std::clamp(fxGraphScroll(grid) - steps * 0.5f * kFxGridCol, 0.0f, fxGraphScrollMost(grid));
            if (fxSlot_.lane >= 0)
                fxMenuOpen_ = false;
        }
        return true;
    }
    setRowScroll(fxScroll() - steps * 0.4f);
    host_.closeGoneLists();
    return true;
}

// Its parts the middle button pans: the row (0), by card places, and the graph (1), by pixels.
std::optional<std::pair<int, float>> FxPanel::panAt(Point p) const {
    if (wide() && fxGraph() && fxGraphArea().contains(p))
        return std::pair{1, graphScroll()};
    if (wide() && !fxGraph() && kFxPanel.contains(p))
        return std::pair{0, fxScroll()};
    return std::nullopt;
}

std::optional<float> FxPanel::pan(int part, float first, float across, float) {
    // What was under the mouse as the button went down stays under it. Past either end, the row stops there.
    const bool graphPart = part == 1;
    const float wanted = graphPart ? first - across : first - across / (kFxCardW + kFxGap);
    const float most = graphPart ? fxGraphScrollMost(fxGridLayout(fxLine())) : rowLength() - kVisibleFx;
    const float to = std::clamp(wanted, 0.0f, std::max(most, 0.0f));
    if (graphPart)
        fxGraphScroll_ = to;
    else
        setRowScroll(to);
    return to != wanted ? std::optional(to) : std::nullopt;
}

// --- Its context menu and questions

bool FxPanel::contextMenu(const std::optional<Widget> &hit, Point, std::vector<MenuItem> &items) {
    menuFx_ = -1;
    if (!hit)
        return false;
    if ((hit->kind == Kind::FxNode && hit->a >= kFxSlots) || hit->kind == Kind::FxBoxGrip) { // a split or a join
        menuFx_ = hit->a;
        items.push_back({kMenuRemove, tr(Str::MenuRemove)});
        return true;
    }
    if (hit->kind == Kind::FxGrip || hit->kind == Kind::FxPower || hit->kind == Kind::FxNode ||
        hit->kind == Kind::FxNodePower) {
        menuFx_ = hit->a;
        if (fxChain().size() < static_cast<size_t>(kFxSlots))
            items.push_back({kMenuDuplicate, tr(Str::MenuDuplicate)});
        items.push_back({kMenuReset, tr(Str::MenuResetSettings)});
        if (fx_.slotType(menuFx_) == FxType::MultiDyn)
            items.push_back({kOttCommand, tr(Str::MenuOtt)});
        // An amp's: how many times the host rate its stages run at, the one in force checked; a shaper's likewise,
        // from 2x (here only, not on its card).
        if (fx_.slotType(menuFx_) == FxType::Amp || fx_.slotType(menuFx_) == FxType::Shaper) {
            items.push_back({0, ""});
            const bool amp = fx_.slotType(menuFx_) == FxType::Amp;
            const long now =
                std::lround(value(paramIndex(menuFx_, amp ? FxParam::AmpOversampling : FxParam::ShaperOversampling)));
            for (int i = amp ? 0 : 1; i < kAmpOversamplings; ++i)
                items.push_back({kAmpOversamplingCommand + i, trf(Str::MenuOversampling, {std::to_string(1 << i) + "x"}),
                                 now == i});
        }
        // On a split's band, in the row: the split's other bands it can move to.
        if (!fxGraph()) {
            const std::vector<int> bands = fx_.otherBands(menuFx_);
            if (!bands.empty())
                items.push_back({0, ""});
            for (int lane : bands)
                items.push_back({kMoveBandCommand + lane,
                                 trf(Str::MenuMoveToBand, {tr(lane == 1   ? Str::BandHigh
                                                              : lane == 3 ? Str::BandMid
                                                                          : Str::BandLow)})});
        }
        items.push_back({0, ""});
        items.push_back({kMenuRemove, tr(Str::MenuRemove)});
        return true;
    }
    return false;
}

bool FxPanel::menuCommand(int command) {
    if (command == kMenuRemove && menuFx_ >= kFxSlots) {
        removeFxNode(menuFx_);
    } else if (menuFx_ >= kFxSlots) {
        // a split or a join offers nothing else, and the View nothing for it
    } else if (command == kMenuDuplicate && menuFx_ >= 0) {
        addEffect(fx_.slotType(menuFx_), menuFx_);
    } else if (command == kMenuRemove && menuFx_ >= 0) {
        removeEffect(menuFx_);
    } else if (command == kMenuReset && menuFx_ >= 0) {
        // Every field back to its default, apart from which effect it is and whether it is on.
        std::vector<std::pair<int, double>> changes;
        for (int f = static_cast<int>(FxParam::Enabled) + 1; f < kFxParamCount; ++f) {
            const int index = paramIndex(menuFx_, static_cast<FxParam>(f));
            changes.push_back({index, table_[index].def});
        }
        for (int f = 0; f < kShapeParamCount; ++f)
            changes.push_back({clipCurveCountIndex(menuFx_) + f, table_[clipCurveCountIndex(menuFx_) + f].def});
        fx_.editMany(changes);
    } else if (command > kMoveBandCommand && command <= kMoveBandCommand + 3 && menuFx_ >= 0) {
        fx_.moveToBand(menuFx_, command - kMoveBandCommand);
    } else if (command == kOttCommand && menuFx_ >= 0 && fx_.slotType(menuFx_) == FxType::MultiDyn) {
        fx_.editMany(mdOttSettings(menuFx_));
    } else if (command >= kAmpOversamplingCommand && command < kAmpOversamplingCommand + kAmpOversamplings &&
               menuFx_ >= 0 && fx_.slotType(menuFx_) == FxType::Amp) {
        edit(paramIndex(menuFx_, FxParam::AmpOversampling), command - kAmpOversamplingCommand);
    } else if (command > kAmpOversamplingCommand && command < kAmpOversamplingCommand + kAmpOversamplings &&
               menuFx_ >= 0 && fx_.slotType(menuFx_) == FxType::Shaper) {
        edit(paramIndex(menuFx_, FxParam::ShaperOversampling), command - kAmpOversamplingCommand);
    } else {
        return false;
    }
    return true;
}

Panel::ConfirmWords FxPanel::confirmWords(int kind) const {
    ConfirmWords words;
    if (kind == kConfirmMidBand) { // where a middle band's effects go: none is more likely than the others
        words.title = tr(Str::ConfirmMidBandTitle);
        words.what = confirmEffects_ == 1 ? std::string(tr(Str::ConfirmMidBandOne))
                                          : trf(Str::ConfirmMidBand, {std::to_string(confirmEffects_)});
        words.ok = tr(Str::ButtonMidRemove);
        words.lit = false;
        // Onto the low band's end (2) or the high one's start (3), each in that band's color.
        words.choices = {{tr(Str::ButtonMidToLow), laneColor(2)}, {tr(Str::ButtonMidToHigh), laneColor(1)}};
    } else if (kind == kConfirmSplitRemove) { // the split with its bands' effects (4), or without them
        words.title = tr(Str::ConfirmSplitRemoveTitle);
        words.what = confirmEffects_ == 1 ? std::string(tr(Str::ConfirmSplitRemoveOne))
                                          : trf(Str::ConfirmSplitRemove, {std::to_string(confirmEffects_)});
        words.ok = tr(Str::ButtonSplitKeep);
        words.shorter = true;
        words.third = tr(Str::ButtonSplitDelete);
    } else { // every effect, which undo brings back
        words.title = tr(Str::ConfirmFxClearTitle);
        words.what = tr(Str::ConfirmFxClear);
        words.ok = tr(Str::ButtonClearFx);
    }
    return words;
}

void FxPanel::confirmed(int kind, int button) {
    if (kind == kConfirmSplitRemove) { // the split removed, its bands' effects kept on the main line (1) or with it (4)
        if (button == 1 || button == 4)
            removeFxNode(fxSplitNode(confirmSplit_), button == 4 ? 1 : 0);
        confirmSplit_ = -1;
    } else if (kind == kConfirmMidBand) { // back in two bands: the middle band's effects removed (1), onto the low
                                          // band (2), the high one (3)
        if (button > 0)
            setSplitBands(confirmSplit_, 2, button == 1 ? 0 : button == 2 ? 2 : 1);
        confirmSplit_ = -1;
    } else if (button == 1) {
        clearFx();
    }
}

std::vector<std::tuple<int, Rect, Rect>> FxPanel::offCards() const {
    // A switched-off effect's card, in the effects graph's inspector or along the row.
    std::vector<std::tuple<int, Rect, Rect>> cards;
    if (!wide())
        return cards;
    if (fxGraph()) {
        if (const int node = fxSelectedNode();
            node >= 0 && node < kFxSlots && value(paramIndex(node, FxParam::Enabled)) < 0.5)
            cards.emplace_back(node, fxInspector(node), fxInspector(node));
        return cards;
    }
    for (const FxRowCard &card : fxRowShown().cards) {
        const float pos = card.place - fxScroll();
        if (onScreen(pos, kVisibleFx, card.span) && value(paramIndex(card.slot, FxParam::Enabled)) < 0.5 &&
            !fxFolded(card.slot))
            cards.emplace_back(card.slot, fxCard(pos, card.span), fxCardClip());
    }
    return cards;
}

} // namespace fmvibe::gui
