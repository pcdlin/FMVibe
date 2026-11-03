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

// The freeform shape editor: an LFO's fills the routing panel and the inspector, which it hides; a shaper's curve
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
        const std::vector<int> chain = fxChainShown();
        const auto at = std::find(chain.begin(), chain.end(), shape - kMaxSources);
        return fxDisplay(fxScreen(chain, static_cast<size_t>(at - chain.begin())), FxType::Shaper).inset(8.0f);
    }
    if (shape >= 0 && isShaperCurve(shape) && shapeEditing() != shape) // on its page in the inspector
        return kShaperDisplay.inset(8.0f);
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

// A little room above and below the full swing, so its points can be grabbed: 12 px, less in a card.
static float shapeReach(Rect area) {
    return area.h * 0.5f - std::min(12.0f, area.h * 0.06f);
}

Point View::shapeAt(int shape, double x, double y) const {
    const Rect area = shapeArea(shape);
    return {area.x + static_cast<float>(x) * area.w, area.center().y - static_cast<float>(y) * shapeReach(area)};
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
void View::addShapeStep(int shape, Point p, bool snap) {
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
    const int count = shapePointCount(shape);
    auto at = [&](int point) {
        return Pt{shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)),
                  value(shapeIndex(shape, point, PointField::Curve))};
    };
    std::vector<Pt> before, left, right, after; // left and right: on the step's lines; the ones between go
    double middle = -1.0; // but a symmetric curve's middle point stays (see removeShapePoint()), at the step's level
    for (int point = 0; point < count; ++point) {
        const Pt pt = at(point);
        if (pt.x < x0 - kOnLine)
            before.push_back(pt);
        else if (pt.x < x0 + kOnLine)
            left.push_back(pt);
        else if (pt.x > x1 + kOnLine)
            after.push_back(pt);
        else if (pt.x > x1 - kOnLine)
            right.push_back(pt);
        else if (middlePoint(shape, point))
            middle = pt.x;
    }
    // Of the points on a line, the one facing the step goes and the others stay: on the left line the last, on the
    // right line the first. A point alone there stays only for a step beside this one: a level line a column long.
    // The first point is always at the start, and a curve's last one at the end: a step there takes its place.
    auto stepBetween = [&](const Pt &a, const Pt &b) {
        return std::abs(b.x - a.x - (x1 - x0)) < kOnLine && std::abs(b.y - a.y) < 1e-6;
    };
    const bool leftStep = left.size() == 1 && !before.empty() && stepBetween(before.back(), left[0]);
    if (x0 <= 0.0 || (left.size() == 1 && !leftStep))
        left.clear();
    else if (left.size() > 1)
        left.pop_back();
    const bool rightStep = right.size() == 1 && !after.empty() && stepBetween(right[0], after.front());
    if (x1 >= 1.0 || (right.size() == 1 && !rightStep))
        right.clear();
    else if (right.size() > 1)
        right.erase(right.begin());
    std::vector<Pt> points = before;
    points.insert(points.end(), left.begin(), left.end());
    points.push_back({x0, y, 0.0});
    if (middle >= 0.0)
        points.push_back({middle, y, 0.0});
    points.push_back({x1, y, 0.0});
    points.insert(points.end(), right.begin(), right.end());
    points.insert(points.end(), after.begin(), after.end());
    if (static_cast<int>(points.size()) > shapeMaxPoints(shape)) {
        flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
        return;
    }
    std::vector<std::pair<int, double>> changes;
    auto change = [&](int index, double v) {
        if (v != value(index))
            changes.push_back({index, v});
    };
    for (size_t point = 0; point < points.size(); ++point) {
        if (point > 0) // the first point's place is the start, whatever its X says
            change(shapeIndex(shape, static_cast<int>(point), PointField::X), points[point].x);
        change(shapeIndex(shape, static_cast<int>(point), PointField::Y), points[point].y);
        change(shapeIndex(shape, static_cast<int>(point), PointField::Curve), points[point].curve);
    }
    change(shapeBase(shape), static_cast<double>(points.size()));
    editMany(changes);
}

// Shift+click anywhere in the big editor, over its points and bends too, draws a step.
bool View::drawsStep(const Widget &w, Modifiers mods) const {
    return mods.shift && (w.kind == Kind::ShapeArea || w.kind == Kind::ShapePoint || w.kind == Kind::ShapeCurve) &&
           w.a == shapeEditing();
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

} // namespace fmvibe::gui
