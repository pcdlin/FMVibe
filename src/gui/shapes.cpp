#include "view.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

// The freeform shape editor: an LFO's in its widened card, or in full size over the routing panel and the inspector,
// which it hides; a shaper's curve
// takes a narrower panel in the middle of the effects' panel it opens from, so its plot is closer to square, the
// inspector still covered; the note source's curve and velocity's fill the routing panel.
constexpr float kClipEditorW = 520.0f;
Rect View::shapePanel() const {
    const int shape = shapeEditing();
    if (shape >= 0 && isClipCurve(shape))
        return {kFxPanel.x + std::round((kFxPanel.w - kClipEditorW) * 0.5f), kFxPanel.y, kClipEditorW, kFxPanel.h};
    return shape >= 0 && !isCurve(shape) ? kFxPanel : kRouting;
}

Rect View::shapeArea(int shape) const {
    if (shape < -1 || shape > kVelocityCurve)
        return shapeAreaNow(shape);
    std::optional<Rect> &kept = derived_.shapeAreas[shape + 1]; // only kept while a scope goes on
    if (!kept) {
        const Rect area = shapeAreaNow(shape);
        if (derivedDepth_ <= 0)
            return area;
        kept = area;
    }
    return *kept;
}

Rect View::shapeAreaNow(int shape) const {
    if (shape >= 0 && isClipCurve(shape) && shapeEditing() != shape) { // in its card
        return fxDisplayIn(fxCardOf(shape - kMaxSources).value_or(Rect{}), FxType::Shaper).inset(8.0f);
    }
    if (shape >= 0 && isShaperCurve(shape) && shapeEditing() != shape) // on its page in the inspector
        return kShaperDisplay.inset(8.0f);
    if (shape >= 0 && !isCurve(shape) && shapeEditing() != shape) // an LFO's, in its widened card
        return lfoCardPlot(cardOf(shape));
    const Rect panel = shapePanel();
    return {panel.x + 40.0f, panel.y + 60.0f, panel.w - 80.0f, panel.h - 96.0f};
}

Rect View::shapeDone() const {
    const Rect panel = shapePanel();
    return {panel.right() - 16.0f - 28.0f, panel.y + 9.0f, 28.0f, 22.0f};
}

Rect View::shapeReset() const {
    const Rect done = shapeDone();
    return {done.x - 6.0f - 28.0f, done.y, 28.0f, 22.0f};
}

// The grid's controls, under the editor's right end: per axis a step down (part 0), the count and a step up.
Rect View::shapeGridButton(int axis, int part) const {
    constexpr float kParts[] = {18.0f, 28.0f, 18.0f};
    const Rect area = shapeArea();
    const float group = area.right() - (axis == 0 ? 148.0f : 64.0f);
    const float x = group + (part > 0 ? kParts[0] : 0.0f) + (part > 1 ? kParts[1] : 0.0f);
    return {x, area.bottom() + 9.0f, kParts[part], 18.0f};
}

// The big editor's Points | Steps and Snap, left of the grid's controls and their label; in a shaper effect's
// narrower editor, in its title row left of the reset button.
Rect View::shapeModeButton(int mode) const {
    const bool title = isClipCurve(shapeEditing());
    const float right = title ? shapeReset().x - 8.0f : shapeGridButton(0, 0).x - 60.0f;
    const Rect snap{right - 64.0f, title ? shapeReset().y + 2.0f : shapeGridButton(0, 0).y, 64.0f, 18.0f};
    return mode == 2 ? snap : Rect{snap.x - 8.0f - (2 - mode) * 68.0f, snap.y, 68.0f, snap.h};
}

// A little room above and below the full swing, so its points can be grabbed: 12 px, less in a card.
static float shapeReach(Rect area) {
    return area.h * 0.5f - std::min(12.0f, area.h * 0.06f);
}

Point View::areaAt(Rect area, double x, double y) {
    return {area.x + static_cast<float>(x) * area.w, area.center().y - static_cast<float>(y) * shapeReach(area)};
}

Point View::shapeAt(int shape, double x, double y) const {
    return areaAt(shapeArea(shape), x, y);
}

double View::shapeXAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return (p.x - area.x) / area.w;
}

double View::shapeYAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return std::clamp((area.center().y - p.y) / shapeReach(area), -1.0f, 1.0f);
}

int View::shapeEditing() const {
    if (shapeEditor_ < 0)
        return -1;
    if (isStripCurve(shapeEditor_)) // open until it is closed
        return shapeEditor_;
    if (isShaperCurve(shapeEditor_)) {
        const int shaper = shapeEditor_ - kMaxSources - kFxSlots;
        return clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom ? shapeEditor_ : -1;
    }
    if (isClipCurve(shapeEditor_)) {
        const int slot = shapeEditor_ - kMaxSources;
        return fxSlotType(slot) == FxType::Shaper &&
                       std::lround(value(paramIndex(slot, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Custom)
                   ? shapeEditor_
                   : -1;
    }
    if (sourceTypeOf(shapeEditor_) != SourceType::Lfo ||
        std::lround(value(paramIndex(shapeEditor_, SourceParam::Shape))) != static_cast<long>(LfoShape::Custom))
        return -1;
    return shapeEditor_;
}

int View::wideLfo() const {
    if (wideLfo_ < 0 || sourceTypeOf(wideLfo_) != SourceType::Lfo || sourceFolded(wideLfo_) ||
        std::lround(value(paramIndex(wideLfo_, SourceParam::Shape))) != static_cast<long>(LfoShape::Custom))
        return -1;
    return wideLfo_;
}

bool View::shapeSteps(int) const {
    return controller_.layout().shapeSteps;
}

// Snap is for the editors that show the grid: the big one and a widened LFO card; a shaper's curve in its card or on
// its page snaps with Ctrl alone.
bool View::shapeSnaps(int shape, bool ctrl) const {
    return ctrl != ((shape == shapeEditing() || shape == wideLfo()) && controller_.layout().shapeSnap);
}

int View::shapeBase(int shape) {
    return isNoteCurve(shape)       ? paramIndex(NoteParam::Curve)
           : isVelocityCurve(shape) ? paramIndex(VelocityParam::Curve)
           : isCurve(shape)         ? shaperParams(shape - kMaxSources).curve // an effect's or a voice shaper's
                                    : shapeCountIndex(shape);
}

Color View::shapeColor(int shape) const {
    return isStripCurve(shape)                        ? kSourceColors[sourceOfCurve(shape)]
           : isClipCurve(shape) || isShaperCurve(shape) ? clipColor(shape - kMaxSources)
                                                        : kSourceColors[shape];
}

int View::shapeIndex(int shape, int point, PointField field) const {
    return shapeFieldIndex(shapeBase(shape), point, field);
}

int View::shapePointCount(int shape) const {
    return fmvibe::shapePointCount(value(shapeBase(shape)), shapeMaxPoints(shape));
}

void View::keyShape(std::vector<double> &key, int base) const {
    const int count = fmvibe::shapePointCount(value(base)); // as many as shapeValue() and clipCurveValue() read
    for (int i = 0; i < 1 + 3 * count; ++i)
        key.push_back(value(base + i));
}

// The first point sits at the start; a curve's last one at the end.
double View::shapePointX(int shape, int point) const {
    if (point == 0)
        return 0.0;
    if (isCurve(shape) && point + 1 == shapePointCount(shape))
        return 1.0;
    return value(shapeIndex(shape, point, PointField::X));
}

Point View::shapePointPos(int shape, int point) const {
    return shapeAt(shape, shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)));
}

// Halfway along the segment, on the curve.
Point View::shapeCurvePos(int shape, int point) const {
    const Point a = shapePointPos(shape, point);
    const bool wraps = point + 1 == shapePointCount(shape);
    const Point b =
        wraps ? Point{shapeArea(shape).right(), shapePointPos(shape, 0).y} : shapePointPos(shape, point + 1);
    const float t = static_cast<float>(shapeBend(0.5, value(shapeIndex(shape, point, PointField::Curve))));
    return {(a.x + b.x) * 0.5f, a.y + (b.y - a.y) * t};
}

void View::addShapePoint(int shape, Point p) {
    if (shapePointCount(shape) >= shapeMaxPoints(shape)) {
        flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
        return;
    }
    editMany(insertShapePoint(shape, std::clamp(shapeXAt(shape, p), 0.001, 0.999), shapeYAt(shape, p)));
}

std::vector<std::pair<int, double>> View::insertShapePoint(int shape, double x, double y) const {
    const int count = shapePointCount(shape);
    int at = 1;
    while (at < count && shapePointX(shape, at) < x)
        ++at;
    // Later points move up one place to make room; a curve's last point stays at the end.
    std::vector<std::pair<int, double>> changes;
    for (int point = count - 1; point >= at; --point) {
        changes.push_back({shapeIndex(shape, point + 1, PointField::X), shapePointX(shape, point)});
        for (PointField field : {PointField::Y, PointField::Curve})
            changes.push_back({shapeIndex(shape, point + 1, field), value(shapeIndex(shape, point, field))});
    }
    changes.push_back({shapeIndex(shape, at, PointField::X), x});
    changes.push_back({shapeIndex(shape, at, PointField::Y), y});
    changes.push_back({shapeIndex(shape, at, PointField::Curve), 0.0});
    changes.push_back({shapeBase(shape), count + 1.0});
    return changes;
}

bool View::curveSymmetric(int shape) const {
    if (!isClipCurve(shape) && !isShaperCurve(shape))
        return false;
    const ShaperParams clip = shaperParams(shape - kMaxSources);
    return clipMode(value(clip.mode)) == ClipMode::Custom && value(clip.symmetric) >= 0.5;
}

constexpr double kOnMiddle = 1e-4; // how near the middle a point counts as on it

bool View::pointShown(int shape, int point) const {
    return !curveSymmetric(shape) || shapePointX(shape, point) > 0.5 - kOnMiddle;
}

bool View::middlePoint(int shape, int point) const {
    return curveSymmetric(shape) && std::abs(shapePointX(shape, point) - 0.5) < kOnMiddle;
}

Point View::unmirrored(int shape, Point p) const {
    if (!curveSymmetric(shape) || shapeXAt(shape, p) >= 0.5)
        return p;
    const Point middle = shapeAt(shape, 0.5, 0.0);
    return {2.0f * middle.x - p.x, 2.0f * middle.y - p.y};
}

// Flips a shaper's Symmetric; switched on, its curve gets a point in the middle, where the curve is, if it has none
// there and has room for one. One undo step either way.
void View::switchSymmetric(int param) {
    const bool on = value(param) < 0.5;
    int unit = 0;
    while (unit + 1 < kShaperUnits && shaperParams(unit).symmetric != param)
        ++unit;
    const int shape = kMaxSources + unit;
    std::vector<std::pair<int, double>> changes;
    if (on && shapePointCount(shape) < shapeMaxPoints(shape)) {
        bool has = false;
        for (int point = 0; point < shapePointCount(shape) && !has; ++point)
            has = std::abs(shapePointX(shape, point) - 0.5) < kOnMiddle;
        auto get = [this](int index) { return value(index); };
        if (!has)
            changes = insertShapePoint(shape, 0.5, clipCurveValue(get, shapeBase(shape), 0.0));
    }
    changes.push_back({param, on ? 1.0 : 0.0});
    editMany(changes);
}

// A step, as in a step sequencer: a level line from the grid line left of p to the one right of it, at p's height,
// with a point at each end. The points between the two lines go; a point already on a line stays, for the step
// next to this one, the new one beside it at the same place so the shape jumps straight up or down there.
bool View::addShapeStep(int shape, Point p, bool snap, const std::vector<double> &before) {
    const int base = shapeBase(shape);
    std::vector<double> target = before; // every param of the shape, as it is to be
    if (target.empty())
        for (int f = 0; f < shapeParamCount(shape); ++f)
            target.push_back(value(base + f));
    auto was = [&](int index) { return target[static_cast<size_t>(index - base)]; };
    const UiLayout &layout = controller_.layout();
    const int lines = layout.shapeGridX;
    const int column = std::clamp(static_cast<int>(std::floor(shapeXAt(shape, p) * lines)), 0, lines - 1);
    const double x0 = static_cast<double>(column) / lines, x1 = static_cast<double>(column + 1) / lines;
    double y = shapeYAt(shape, p);
    if (snap) {
        const double half = 0.5 * layout.shapeGridY; // lines per unit of height
        y = std::round((y + 1.0) * half) / half - 1.0;
    }
    constexpr double kOnLine = 1e-4;
    struct Pt {
        double x, y, curve;
    };
    const int count = fmvibe::shapePointCount(was(base), shapeMaxPoints(shape));
    auto at = [&](int point) { // as shapePointX() places them
        const double x = point == 0                                        ? 0.0
                         : isCurve(shape) && point + 1 == count ? 1.0
                                                                           : was(shapeIndex(shape, point, PointField::X));
        return Pt{x, was(shapeIndex(shape, point, PointField::Y)), was(shapeIndex(shape, point, PointField::Curve))};
    };
    std::vector<Pt> earlier, left, right, after; // left and right: on the step's lines; the ones between go
    double middle = -1.0; // but a symmetric curve's middle point stays (see removeShapePoint()), at the step's level
    for (int point = 0; point < count; ++point) {
        const Pt pt = at(point);
        if (pt.x < x0 - kOnLine)
            earlier.push_back(pt);
        else if (pt.x < x0 + kOnLine)
            left.push_back(pt);
        else if (pt.x > x1 + kOnLine)
            after.push_back(pt);
        else if (pt.x > x1 - kOnLine)
            right.push_back(pt);
        else if (curveSymmetric(shape) && std::abs(pt.x - 0.5) < kOnMiddle) // middlePoint(), as it was
            middle = pt.x;
    }
    // Of the points on a line, the one facing the step goes and the others stay: on the left line the last, on the
    // right line the first. A point alone there stays only for a step beside this one: a level line a column long.
    // The first point is always at the start, and a curve's last one at the end: a step there takes its place.
    auto stepBetween = [&](const Pt &a, const Pt &b) {
        return std::abs(b.x - a.x - (x1 - x0)) < kOnLine && std::abs(b.y - a.y) < 1e-6;
    };
    const bool leftStep = left.size() == 1 && !earlier.empty() && stepBetween(earlier.back(), left[0]);
    if (x0 <= 0.0 || (left.size() == 1 && !leftStep))
        left.clear();
    else if (left.size() > 1)
        left.pop_back();
    const bool rightStep = right.size() == 1 && !after.empty() && stepBetween(right[0], after.front());
    if (x1 >= 1.0 || (right.size() == 1 && !rightStep))
        right.clear();
    else if (right.size() > 1)
        right.erase(right.begin());
    std::vector<Pt> points = earlier;
    points.insert(points.end(), left.begin(), left.end());
    points.push_back({x0, y, 0.0});
    if (middle >= 0.0)
        points.push_back({middle, y, 0.0});
    points.push_back({x1, y, 0.0});
    points.insert(points.end(), right.begin(), right.end());
    points.insert(points.end(), after.begin(), after.end());
    if (static_cast<int>(points.size()) > shapeMaxPoints(shape))
        return false;
    auto put = [&](int index, double v) { target[static_cast<size_t>(index - base)] = v; };
    for (size_t point = 0; point < points.size(); ++point) {
        if (point > 0) // the first point's place is the start, whatever its X says
            put(shapeIndex(shape, static_cast<int>(point), PointField::X), points[point].x);
        put(shapeIndex(shape, static_cast<int>(point), PointField::Y), points[point].y);
        put(shapeIndex(shape, static_cast<int>(point), PointField::Curve), points[point].curve);
    }
    put(base, static_cast<double>(points.size()));
    std::vector<std::pair<int, double>> changes;
    for (size_t f = 0; f < target.size(); ++f)
        if (target[f] != value(base + static_cast<int>(f)))
            changes.push_back({base + static_cast<int>(f), target[f]});
    if (!changes.empty())
        editMany(changes);
    return true;
}

// Shift+click anywhere in the big editor or a widened LFO card, over its points and bends too, draws a step; or
// with Steps picked, a plain click.
bool View::drawsStep(const Widget &w, Modifiers mods) const {
    return mods.shift != shapeSteps(w.a) &&
           (w.kind == Kind::ShapeArea || w.kind == Kind::ShapePoint || w.kind == Kind::ShapeCurve) &&
           (w.a == shapeEditing() || w.a == wideLfo());
}

void View::removeShapePoint(int shape, int point) {
    if (middlePoint(shape, point)) {
        flash(tr(Str::MsgCurveMiddle));
        return;
    }
    const int count = shapePointCount(shape);
    const bool end = isCurve(shape) && point + 1 == count;
    if (point == 0 || end || count <= 2) {
        flash(tr(isStripCurve(shape) && (point == 0 || end) ? Str::MsgNoteCurveEnds
                 : isCurve(shape) && (point == 0 || end)   ? Str::MsgCurveEnds
                 : point == 0                              ? Str::MsgFirstPoint
                                                           : Str::MsgTwoPoints));
        return;
    }
    std::vector<std::pair<int, double>> changes;
    for (int p = point; p + 1 < count; ++p) {
        changes.push_back({shapeIndex(shape, p, PointField::X), shapePointX(shape, p + 1)});
        for (PointField field : {PointField::Y, PointField::Curve})
            changes.push_back({shapeIndex(shape, p, field), value(shapeIndex(shape, p + 1, field))});
    }
    changes.push_back({shapeBase(shape), count - 1.0});
    editMany(changes);
}

void View::resetShape(int shape) {
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < shapeParamCount(shape); ++f)
        changes.push_back({shapeBase(shape) + f, table_[shapeBase(shape) + f].def});
    if (isNoteCurve(shape)) // and its range, every MIDI note
        for (int index : {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)})
            changes.push_back({index, table_[index].def});
    editMany(changes);
}

void View::stepShapeGrid(int axis, int steps) {
    UiLayout &layout = controller_.layout();
    int &lines = axis == 0 ? layout.shapeGridX : layout.shapeGridY;
    auto step = [&](const auto &offered) {
        const int count = static_cast<int>(std::size(offered));
        int at = 0;
        while (at + 1 < count && offered[at] < lines)
            ++at;
        lines = offered[std::clamp(at + steps, 0, count - 1)];
    };
    if (axis == 0)
        step(kShapeGridsX);
    else
        step(kShapeGridsY);
    controller_.layoutChanged();
}

std::vector<View::DrawnPoint> View::drawnPoints(int shape) const {
    std::vector<DrawnPoint> points;
    for (int point = 0; point < shapePointCount(shape); ++point)
        points.push_back({shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)),
                          value(shapeIndex(shape, point, PointField::Curve))});
    return points;
}

std::optional<std::vector<std::pair<int, double>>> View::drawnChanges(int shape,
                                                                      const std::vector<DrawnPoint> &points) const {
    if (points.size() < 2 || static_cast<int>(points.size()) > shapeMaxPoints(shape))
        return std::nullopt;
    std::vector<std::pair<int, double>> changes;
    auto put = [&](int index, double v) {
        const ParamDef &info = table_[index];
        v = std::clamp(v, info.min, info.max);
        if (v != value(index))
            changes.push_back({index, v});
    };
    for (size_t k = 0; k < points.size(); ++k) {
        const int point = static_cast<int>(k);
        if (point > 0) // the first point's place is the start, whatever its X says
            put(shapeIndex(shape, point, PointField::X), points[k].x);
        put(shapeIndex(shape, point, PointField::Y), points[k].y);
        put(shapeIndex(shape, point, PointField::Curve), points[k].curve);
    }
    put(shapeBase(shape), static_cast<double>(points.size()));
    return changes;
}

namespace {
// The shape copied by a widened card's Copy, for its Paste into another LFO: its params from shapeBase() on.
std::vector<double> copiedShape;
} // namespace

bool View::shapeCopied() {
    return copiedShape.size() == static_cast<size_t>(kShapeParamCount);
}

// Inverting flips each level; a bend keeps its value, which bends the flipped segment the mirrored way. Reversed,
// the shape runs from the end back to the start: the first point stays first, the others come in the other order,
// each at 1 - its time, and a segment run backwards bends the other way (shapeBend(): 1 - bend(1 - t, c) is
// bend(t, -c)). Shifted, the shape moves a column of the grid later (right) or earlier (left), round the cycle: the
// point where the new start falls, or one put there on the curve (its segment split in two, each part bent as much as
// keeps it on the same curve), becomes the first.
void View::applyShapeTool(int lfo, ShapeTool tool) {
    if (tool == ShapeTool::Copy) {
        copiedShape.clear();
        for (int f = 0; f < kShapeParamCount; ++f)
            copiedShape.push_back(value(shapeBase(lfo) + f));
        flash(trf(Str::MsgShapeCopied, {sourceName(lfo)}));
        return;
    }
    if (tool == ShapeTool::Paste) {
        if (copiedShape.size() != static_cast<size_t>(kShapeParamCount)) {
            flash(tr(Str::MsgNothingCopied));
            return;
        }
        std::vector<std::pair<int, double>> changes;
        for (int f = 0; f < kShapeParamCount; ++f)
            if (copiedShape[f] != value(shapeBase(lfo) + f))
                changes.push_back({shapeBase(lfo) + f, copiedShape[f]});
        if (!changes.empty())
            editMany(changes);
        return;
    }
    std::vector<DrawnPoint> points = drawnPoints(lfo), changed;
    const size_t n = points.size();
    if (tool == ShapeTool::Invert) {
        changed = points;
        for (DrawnPoint &point : changed)
            point.y = -point.y;
    } else if (tool == ShapeTool::Reverse) {
        changed.push_back({0.0, points[0].y, -points[n - 1].curve});
        for (size_t j = 1; j < n; ++j)
            changed.push_back({1.0 - points[n - j].x, points[n - j].y, -points[n - 1 - j].curve});
    } else {
        constexpr double kSameTime = 1e-6;
        const double column = 1.0 / std::max(controller_.layout().shapeGridX, 1);
        const double start = tool == ShapeTool::ShiftRight ? 1.0 - column : column; // what comes to the start
        size_t first = n;
        for (size_t k = 0; k < n && first == n; ++k)
            if (std::abs(points[k].x - start) < kSameTime)
                first = k;
        if (first == n) { // a point on the curve there, splitting the segment it falls in
            size_t k = 0;
            while (k + 1 < n && points[k + 1].x <= start)
                ++k;
            const DrawnPoint &from = points[k];
            const double x1 = k + 1 < n ? points[k + 1].x : 1.0, y1 = points[k + 1 < n ? k + 1 : 0].y;
            const double u = x1 - from.x < 1e-9 ? 0.0 : (start - from.x) / (x1 - from.x);
            const DrawnPoint split{start, from.y + (y1 - from.y) * shapeBend(u, from.curve), from.curve * (1.0 - u)};
            points[k].curve = from.curve * u;
            points.insert(points.begin() + static_cast<std::ptrdiff_t>(k + 1), split);
            first = k + 1;
        }
        for (size_t k = 0; k < points.size(); ++k) {
            DrawnPoint point = points[(first + k) % points.size()];
            point.x -= start;
            if (k == 0 || std::abs(point.x) < kSameTime)
                point.x = 0.0;
            else if (point.x < 0.0)
                point.x += 1.0;
            changed.push_back(point);
        }
    }
    const auto changes = drawnChanges(lfo, changed);
    if (!changes) {
        flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(lfo))}));
        return;
    }
    if (!changes->empty())
        editMany(*changes);
}

} // namespace fmvibe::gui
