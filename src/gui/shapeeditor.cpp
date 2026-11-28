#include "shapeeditor.h"
#include "lists.h"
#include "shapeclip.h"

#include "icons.h"
#include "layout.h"
#include "routing.h"
#include "theme.h"
#include "../uistrings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

namespace {

// `value` as `format` prints it, for the numbers a wording takes (as view.cpp's).
std::string number(const char *format, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int note) {
    return std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 1);
}

} // namespace

ShapeEditor::ShapeEditor(PanelHost &host)
    : host_(host), mods_(host.modulationModel()), fx_(host.fxGraphModel()), table_(paramTable()) {}

// The freeform shape editor: an LFO's in its widened card, or in full size over the routing panel and the inspector,
// which it hides; a shaper's curve
// takes a narrower panel in the middle of the effects' panel it opens from, so its plot is closer to square, the
// inspector still covered; the note source's curve and velocity's fill the routing panel.
constexpr float kClipEditorW = 520.0f;
constexpr float kShapeRulerRoom = 12.0f; // what an LFO's plot gives up for its loop's strip
constexpr double kLoopApart = 1.0 / 256.0; // how near a loop's ends come, dragged off the grid's columns
Rect ShapeEditor::shapePanel() const {
    const int shape = shapeEditing();
    if (shape >= 0 && isClipCurve(shape))
        return {kFxPanel.x + std::round((kFxPanel.w - kClipEditorW) * 0.5f), kFxPanel.y, kClipEditorW, kFxPanel.h};
    // A Curve's, an envelope's, the note source's and velocity's alike; a voice shaper's beside the inspector.
    return shape >= 0 && (!isCurve(shape) || isOpEnvCurve(shape) || isStripCurve(shape)) ? kShapeEditorPanel : kRouting;
}

Rect ShapeEditor::shapeArea(int shape) const {
    if (shape < -1 || shape >= kOpEnvCurve + kOps)
        return shapeAreaNow(shape);
    return shapeAreas_[shape + 1].get(host_, [&] { return shapeAreaNow(shape); }); // only kept while a scope goes on
}

void ShapeEditor::forgetScoped() {
    for (const ScopeCache<Rect> &area : shapeAreas_)
        area.forget();
}

Rect ShapeEditor::shapeAreaNow(int shape) const {
    // In its card, on its page in the inspector, or an LFO's in its widened card, where what shows it puts it.
    if (shape >= 0 && (isClipCurve(shape) || isShaperCurve(shape) || isOpEnvCurve(shape) || !isCurve(shape)) &&
        shapeEditing() != shape)
        return host_.shapeHome(shape);
    // An LFO's (a Curve's) plot leaves room above it for its loop's strip and ruler, under the title row.
    const Rect panel = shapePanel();
    const int editing = shape >= 0 ? shape : shapeEditing();
    const float ruler = editing >= 0 && (!isCurve(editing) || isOpEnvCurve(editing)) ? kShapeRulerRoom : 0.0f;
    return {panel.x + 40.0f, panel.y + 60.0f + ruler, panel.w - 80.0f, panel.h - 96.0f - ruler};
}

Rect ShapeEditor::shapeDone() const {
    const Rect panel = shapePanel();
    return {panel.right() - 16.0f - 28.0f, panel.y + 9.0f, 28.0f, 22.0f};
}

Rect ShapeEditor::shapeReset() const {
    const Rect done = shapeDone();
    return {done.x - 6.0f - 28.0f, done.y, 28.0f, 22.0f};
}

// A Curve's (an LFO's) big editor's title row and an operator's envelope's, from the left: the name, the mode list, a
// Curve's Unipolar switch, the sync switch and the knobs, an envelope's Slew where a Curve's Phase is.
Rect ShapeEditor::shapeTitle() const { // as wide as the longest title ("VELOCITY CURVE") takes
    const Rect panel = shapePanel();
    return {panel.x + 16.0f, panel.y + 9.0f, 136.0f, 22.0f};
}

Rect ShapeEditor::shapeClockList() const {
    const Rect title = shapeTitle();
    return {title.right() + 12.0f, title.y, kLfoListW, title.h};
}

Rect ShapeEditor::noteRangeKnob(int knob) const {
    return {shapeClockList().x + knob * 96.0f, shapePanel().y, 92.0f, 40.0f};
}

Rect ShapeEditor::shapeUnipolarSwitch() const {
    const Rect list = shapeClockList();
    return {list.right() + 8.0f, list.y, 22.0f, 22.0f};
}

Rect ShapeEditor::shapeSyncButton(int shape) const {
    if (isOpEnvCurve(shape)) // no Unipolar switch: right after the list
        return shapeUnipolarSwitch();
    const Rect unipolar = shapeUnipolarSwitch();
    return {unipolar.right() + 6.0f, unipolar.y, 22.0f, 22.0f};
}

Rect ShapeEditor::shapeTitleKnob(int shape, int knob) const {
    const int place = isOpEnvCurve(shape) ? std::min(knob, 1) : knob; // an envelope has no Phase
    return {shapeSyncButton(shape).right() + 8.0f + place * 96.0f, shapePanel().y, 92.0f, 40.0f};
}

Rect ShapeEditor::partialRandom() const {
    const Rect reset = shapeReset();
    return {reset.x - 6.0f - reset.w, reset.y, reset.w, reset.h};
}

// The grid's controls, under the editor's right end: per axis a step down (part 0), the count and a step up.
Rect ShapeEditor::shapeGridButton(int axis, int part) const {
    constexpr float kParts[] = {18.0f, 28.0f, 18.0f};
    const Rect area = shapeArea();
    const float group = area.right() - (axis == 0 ? 148.0f : 64.0f);
    const float x = group + (part > 0 ? kParts[0] : 0.0f) + (part > 1 ? kParts[1] : 0.0f);
    return {x, area.bottom() + 9.0f, kParts[part], 18.0f};
}

// The big editor's Points | Steps and Snap, left of the grid's controls and their label; in a shaper effect's
// narrower editor, in its title row left of the reset button.
Rect ShapeEditor::shapeModeButton(int mode) const {
    const bool title = isClipCurve(shapeEditing());
    const float right = title ? shapeReset().x - 8.0f : shapeGridButton(0, 0).x - 60.0f;
    const Rect snap{right - 64.0f, title ? shapeReset().y + 2.0f : shapeGridButton(0, 0).y, 64.0f, 18.0f};
    return mode == 2 ? snap : Rect{snap.x - 8.0f - (2 - mode) * 68.0f, snap.y, 68.0f, snap.h};
}

Rect ShapeEditor::shapeBelow() const {
    const Rect area = shapeArea();
    return {area.x, area.bottom() + 10.0f, shapeModeButton(0).x - 12.0f - area.x, 16.0f};
}

Rect ShapeEditor::shapeSymButton() const {
    const Rect points = shapeModeButton(0);
    return {points.x - 8.0f - 44.0f, points.y, 44.0f, points.h};
}

// How to edit, under a narrow editor's plot in `box`, left: on one line where it fits, else in smaller type over two,
// broken between its parts (at the dots between them).
static void paintHintBelow(Canvas &c, Rect box, const std::string &hint) {
    if (c.textWidth(hint, kTextLabel) <= box.w) {
        c.text(box, hint, kTextLabel, kFaint, Align::Left);
        return;
    }
    constexpr std::string_view kDot = "  \xC2\xB7  ";
    std::string lines[2];
    for (size_t from = 0, line = 0; from <= hint.size();) {
        const size_t at = std::min(hint.find(kDot, from), hint.size());
        const std::string part = hint.substr(from, at - from);
        const std::string joined = lines[line].empty() ? part : lines[line] + std::string(kDot) + part;
        if (line == 0 && !lines[0].empty() && c.textWidth(joined, kTextSmall) > box.w)
            lines[line = 1] = part;
        else
            lines[line] = joined;
        from = at + kDot.size();
    }
    constexpr float kLine = 11.0f;
    for (int line = 0; line < 2; ++line)
        c.text({box.x, box.center().y - kLine + line * kLine, box.w, kLine}, lines[line], kTextSmall, kFaint,
               Align::Left);
}

// A little room above and below the full swing, so its points can be grabbed: 12 px, less in a card.
static float shapeReach(Rect area) {
    return area.h * 0.5f - std::min(12.0f, area.h * 0.06f);
}

// Where (x, y) of a shape plotted in `area` goes: x 0..1 across, y -1..1 up, a little short of its edges.
static Point areaAt(Rect area, double x, double y) {
    return {area.x + static_cast<float>(x) * area.w, area.center().y - static_cast<float>(y) * shapeReach(area)};
}

Point ShapeEditor::shapeAt(int shape, double x, double y) const {
    return areaAt(shapeArea(shape), x, y);
}

double ShapeEditor::shapeXAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return (p.x - area.x) / area.w;
}

double ShapeEditor::shapeYAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return std::clamp((area.center().y - p.y) / shapeReach(area), -1.0f, 1.0f);
}

int ShapeEditor::shapeEditing() const {
    if (shapeEditor_ < 0)
        return -1;
    if (isStripCurve(shapeEditor_)) // open until it is closed
        return shapeEditor_;
    if (isOpEnvCurve(shapeEditor_)) // while its operator plays its drawn envelope
        return opEnvMode(value(paramIndex(opOfEnvCurve(shapeEditor_), OpParam::EnvMode))) == OpEnvMode::Curve
                   ? shapeEditor_
                   : -1;
    if (isShaperCurve(shapeEditor_)) {
        const int shaper = shapeEditor_ - kMaxSources - kFxSlots;
        return clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom ? shapeEditor_ : -1;
    }
    if (isClipCurve(shapeEditor_)) {
        const int slot = shapeEditor_ - kMaxSources;
        return fx_.slotType(slot) == FxType::Shaper &&
                       std::lround(value(paramIndex(slot, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Custom)
                   ? shapeEditor_
                   : -1;
    }
    // A Curve source's.
    if (!runsLfo(mods_.sourceTypeOf(shapeEditor_)) || mods_.lfoShapeOf(shapeEditor_) != LfoShape::Custom)
        return -1;
    return shapeEditor_;
}

int ShapeEditor::wideLfo() const {
    if (wideLfo_ < 0 || !runsLfo(mods_.sourceTypeOf(wideLfo_)) ||
        (!isStripSource(wideLfo_) && mods_.folded(false, wideLfo_)) || // folded
        mods_.lfoShapeOf(wideLfo_) != LfoShape::Custom)
        return -1;
    return wideLfo_;
}

bool ShapeEditor::shapeSteps(int) const {
    return controller().layout().shapeSteps;
}

// Snap is for the editors that show the grid: the big one and a widened LFO card; a shaper's curve in its card or on
// its page snaps with Ctrl alone.
bool ShapeEditor::shapeSnaps(int shape, bool ctrl) const {
    return ctrl != ((shape == shapeEditing() || shape == wideLfo()) && controller().layout().shapeSnap);
}

int shapeBase(int shape) {
    return isOpEnvCurve(shape)      ? opEnvShapeIndex(opOfEnvCurve(shape))
           : isNoteCurve(shape)     ? paramIndex(NoteParam::Curve)
           : isVelocityCurve(shape) ? paramIndex(VelocityParam::Curve)
           : isCurve(shape)         ? shaperParams(shape - kMaxSources).curve // an effect's or a voice shaper's
                                    : shapeCountIndex(shape);
}

Color ShapeEditor::shapeColor(int shape) const {
    return isOpEnvCurve(shape)                        ? kAccent
           : isStripCurve(shape)                      ? kSourceColors[sourceOfCurve(shape)]
           : isClipCurve(shape) || isShaperCurve(shape) ? clipColor(shape - kMaxSources)
                                                        : kSourceColors[shape];
}

bool ShapeEditor::curveShape(int shape) const {
    return shape >= 0 && shape < kMaxSources && mods_.sourceTypeOf(shape) == SourceType::Curve;
}

bool ShapeEditor::loopedShape(int shape) const {
    return curveShape(shape) || isOpEnvCurve(shape);
}

bool ShapeEditor::loopShown(int shape) const {
    if (isOpEnvCurve(shape)) // its loop is Sustain's alone
        return value(paramIndex(opOfEnvCurve(shape), OpParam::EnvSustain)) >= 0.5;
    return curveShape(shape) && lfoClock([this](int index) { return value(index); }, shape) != LfoClock::OneShot;
}

std::pair<int, int> ShapeEditor::loopParams(int shape) const {
    if (isOpEnvCurve(shape))
        return {paramIndex(opOfEnvCurve(shape), OpParam::EnvLoopStart), paramIndex(opOfEnvCurve(shape), OpParam::EnvLoopEnd)};
    return {paramIndex(shape, SourceParam::LoopStart), paramIndex(shape, SourceParam::LoopEnd)};
}

LfoLoop ShapeEditor::loopOf(int shape) const {
    if (!isOpEnvCurve(shape))
        return lfoLoop([this](int index) { return value(index); }, shape);
    const auto [startParam, endParam] = loopParams(shape);
    double a = value(startParam), b = value(endParam);
    a = std::isfinite(a) ? std::clamp(a, 0.0, 1.0) : 0.0;
    b = std::isfinite(b) ? std::clamp(b, 0.0, 1.0) : 1.0;
    return a <= b ? LfoLoop{a, b} : LfoLoop{b, a};
}

ShapeEditor::ShapeClock ShapeEditor::shapeClock(int shape) const {
    if (isOpEnvCurve(shape)) {
        const int op = opOfEnvCurve(shape);
        if (value(paramIndex(op, OpParam::EnvSync)) >= 0.5)
            return {ShapeTime::Beats,
                    divisionBeats(static_cast<int>(std::lround(value(paramIndex(op, OpParam::EnvDivision)))))};
        return {ShapeTime::Seconds, timeFromNorm(value(paramIndex(op, OpParam::EnvLength)))};
    }
    const ShapeTime time = mods_.curveTime(shape);
    const int division = static_cast<int>(std::lround(value(paramIndex(shape, SourceParam::Division))));
    return {time, time == ShapeTime::Seconds ? mods_.curveLength(shape)
                  : time == ShapeTime::Beats ? divisionBeats(division)
                                             : 1.0};
}

std::string ShapeEditor::placeText(int shape, double x, bool brief) const {
    const ShapeClock clock = shapeClock(shape);
    return shapePlaceText(clock.time, clock.length, x, brief);
}

// Over the big editor's plot frame, under the title row; along the top of a widened card's plot, in its frame.
Rect ShapeEditor::loopStrip(int shape) const {
    const Rect r = shapeArea(shape);
    if (shapeEditing() == shape)
        return {r.x - 8.0f, r.y - 8.0f - 24.0f, r.w + 16.0f, 20.0f};
    return {r.x - 8.0f, r.y - 8.0f, r.w + 16.0f, 10.0f};
}

ShapeEditor::LoopGrab ShapeEditor::loopGrabAt(int shape, float x) const {
    const LfoLoop loop = loopOf(shape);
    const Rect r = shapeArea(shape);
    const float x0 = r.x + static_cast<float>(loop.start) * r.w, x1 = r.x + static_cast<float>(loop.end) * r.w;
    if (std::abs(x - x1) <= 6.0f)
        return LoopGrab::End;
    if (std::abs(x - x0) <= 6.0f)
        return LoopGrab::Start;
    return x > x0 && x < x1 ? LoopGrab::Move : LoopGrab::New;
}

double ShapeEditor::loopColumn(int shape, Point p) const {
    const double columns = controller().layout().shapeGridX;
    return std::clamp(std::round(shapeXAt(shape, p) * columns) / columns, 0.0, 1.0);
}

std::vector<std::pair<double, std::string>> ShapeEditor::rulerMarks(int shape, float width, float gap) const {
    // How far along the curve a unit of the ruler is, and the units it steps by: a share of the cycle in fives, tens,
    // quarters and halves; a time in ones, twos and fives of a power of ten; beats in sixteenths, eighths and on up.
    double unit = 0.01;
    std::vector<double> steps{5.0, 10.0, 25.0, 50.0, 100.0};
    const ShapeClock clock = shapeClock(shape);
    const ShapeTime time = clock.time;
    if (time == ShapeTime::Seconds) {
        unit = 1.0 / clock.length;
        steps.clear();
        for (double decade = 0.001; decade < 100.0; decade *= 10.0)
            for (const double times : {1.0, 2.0, 5.0})
                steps.push_back(times * decade);
    } else if (time == ShapeTime::Beats) {
        unit = 1.0 / (4.0 * clock.length);
        steps = {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0};
    }
    double step = steps.back();
    for (const double s : steps)
        if (s * unit * width >= gap) {
            step = s;
            break;
        }
    std::vector<std::pair<double, std::string>> marks;
    for (int k = 0; k * step * unit <= 1.0 + 1e-9; ++k) {
        const double x = std::min(k * step * unit, 1.0);
        const bool start = k == 0 && time != ShapeTime::Beats; // "0", where beats count from 1
        marks.push_back({x, start ? "0" : placeText(shape, x, true)});
    }
    return marks;
}

void ShapeEditor::paintPhase(Canvas &c, int lfo, Rect r) const {
    const std::optional<Widget> held = host_.dragged();
    const int phase = paramIndex(lfo, SourceParam::Phase);
    auto get = [this](int index) { return value(index); };
    if (lfoShared(get, lfo) && !lfoAnchored(get, lfo) && !lfoGlobal(get, lfo))
        return; // free, not anchored: it starts nowhere in particular
    const double at = std::clamp(value(phase), 0.0, 1.0);
    const float x = r.x + static_cast<float>(at) * r.w;
    if (held && held->param == phase)
        c.line({x, r.y}, {x, r.bottom()}, shapeColor(lfo).alpha(0.8f), 1.5f);
    else if (at > 0.0 && at < 1.0) // at either end it is the plot's edge
        c.line({x, r.y}, {x, r.bottom()}, shapeColor(lfo).alpha(0.3f), 1.0f, true);
}

void ShapeEditor::paintLoop(Canvas &c, int shape, Rect r, bool card) {
    const Color color = shapeColor(shape);
    // The ruler's labels on round places (see rulerMarks()): at the foot of a card's plot, in the strip under the bar
    // in the big editor, its end right of its place.
    const float gap = card ? 60.0f : 80.0f;
    const Rect strip = loopStrip(shape);
    for (const auto &[x, text] : rulerMarks(shape, r.w, gap)) {
        const float at = r.x + static_cast<float>(x) * r.w;
        if (card && at > r.right() - 30.0f)
            continue; // no room for it
        if (card && isOpEnvCurve(shape)) // an ENVELOPE tab's, under its plot
            c.text({at + (x == 0.0 ? 0.0f : -gap * 0.5f), r.bottom() + 4.0f, gap, 12.0f}, text,
                   kTextSmall, kFaint, x == 0.0 ? Align::Left : Align::Center);
        else if (card)
            c.text({at + 3.0f, r.bottom() - 13.0f, gap - 6.0f, 12.0f}, text, kTextSmall, kFaint, Align::Left);
        else if (at > r.right() - 30.0f)
            c.text({at - 80.0f, strip.y + 8.0f, 80.0f, 12.0f}, text, kTextSmall, kFaint, Align::Right);
        else
            c.text({at + (x == 0.0 ? 0.0f : 3.0f), strip.y + 8.0f, gap - 6.0f, 12.0f}, text, kTextSmall, kFaint,
                   Align::Left);
    }
    if (!loopShown(shape))
        return;
    // What the clock mode doesn't play dimmed, as the SEQ view's roll is outside its loop: past the loop retriggered
    // (each note plays on into it), either side of it free or anchored; none in Sustain, which plays on past it once let
    // go. Its ends dashed across the plot, and its bar on the strip.
    const LfoLoop loop = loopOf(shape);
    const float x0 = r.x + static_cast<float>(loop.start) * r.w, x1 = r.x + static_cast<float>(loop.end) * r.w;
    const LfoClock clock = isOpEnvCurve(shape) ? LfoClock::Sustain // the only one it loops in
                                               : lfoClock([this](int index) { return value(index); }, shape);
    if (clock == LfoClock::Free || clock == LfoClock::Anchor) // Global plays from its Phase into it, as Retrigger
        c.fillRect({r.x, r.y, x0 - r.x, r.h}, kBackground.alpha(0.45f));
    if (clock != LfoClock::Sustain)
        c.fillRect({x1, r.y, r.right() - x1, r.h}, kBackground.alpha(0.45f));
    for (const float x : {x0, x1})
        if (x > r.x + 0.5f && x < r.right() - 0.5f) // not along the plot's edges
            c.line({x, r.y}, {x, r.bottom()}, color.alpha(0.6f), 1.0f, true);
    const Color bar = color.alpha(isOpEnvCurve(shape) || mods_.sourceOn(shape) ? 0.85f : 0.4f);
    if (card) {
        c.fillRect({x0, strip.y + 2.0f, std::max(x1 - x0, 1.0f), 4.0f}, bar, kRadiusMark);
    } else {
        c.fillRect({x0, strip.y + 3.0f, std::max(x1 - x0, 1.0f), 5.0f}, bar, kRadiusMark);
        for (const float x : {x0, x1}) // its ends, which drag
            c.fillCircle({x, strip.y + 5.5f}, 3.5f, bar);
    }
}

int ShapeEditor::shapeIndex(int shape, int point, PointField field) const {
    return shapeFieldIndex(shapeBase(shape), point, field);
}

int ShapeEditor::shapePointCount(int shape) const {
    return fmvibe::shapePointCount(value(shapeBase(shape)), shapeMaxPoints(shape));
}

void ShapeEditor::keyShape(std::vector<double> &key, int base) const {
    const int count = fmvibe::shapePointCount(value(base)); // as many as shapeValue() and clipCurveValue() read
    for (int i = 0; i < 1 + 3 * count; ++i)
        key.push_back(value(base + i));
}

// The first point sits at the start; a curve's last one at the end.
double ShapeEditor::shapePointX(int shape, int point) const {
    if (point == 0)
        return 0.0;
    if (isCurve(shape) && point + 1 == shapePointCount(shape))
        return 1.0;
    return value(shapeIndex(shape, point, PointField::X));
}

Point ShapeEditor::shapePointPos(int shape, int point) const {
    return shapeAt(shape, shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)));
}

// Halfway along the segment, on the curve.
Point ShapeEditor::shapeCurvePos(int shape, int point) const {
    const Point a = shapePointPos(shape, point);
    const bool wraps = point + 1 == shapePointCount(shape);
    const Point b =
        wraps ? Point{shapeArea(shape).right(), shapePointPos(shape, 0).y} : shapePointPos(shape, point + 1);
    const float t = static_cast<float>(shapeBend(0.5, value(shapeIndex(shape, point, PointField::Curve))));
    return {(a.x + b.x) * 0.5f, a.y + (b.y - a.y) * t};
}

void ShapeEditor::addShapePoint(int shape, Point p) {
    if (shapePointCount(shape) >= shapeMaxPoints(shape)) {
        host_.flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
        return;
    }
    editMany(insertShapePoint(shape, std::clamp(shapeXAt(shape, p), 0.001, 0.999), shapeYAt(shape, p)));
}

std::vector<std::pair<int, double>> ShapeEditor::insertShapePoint(int shape, double x, double y) const {
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

bool ShapeEditor::curveSymmetric(int shape) const {
    if (!isClipCurve(shape) && !isShaperCurve(shape))
        return false;
    const ShaperParams clip = shaperParams(shape - kMaxSources);
    return clipMode(value(clip.mode)) == ClipMode::Custom && value(clip.symmetric) >= 0.5;
}

constexpr double kOnMiddle = 1e-4; // how near the middle a point counts as on it

bool ShapeEditor::pointShown(int shape, int point) const {
    return !curveSymmetric(shape) || shapePointX(shape, point) > 0.5 - kOnMiddle;
}

bool ShapeEditor::middlePoint(int shape, int point) const {
    return curveSymmetric(shape) && std::abs(shapePointX(shape, point) - 0.5) < kOnMiddle;
}

bool ShapeEditor::inMirroredHalf(int shape, Point p) const {
    return curveSymmetric(shape) && shapeXAt(shape, p) < 0.5;
}

// Flips a shaper's Symmetric; switched on, its curve gets a point in the middle, where the curve is, if it has none
// there and has room for one. One undo step either way.
void ShapeEditor::switchSymmetric(int param) {
    clearShapePick();
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
bool ShapeEditor::addShapeStep(int shape, Point p, bool snap, const std::vector<double> &before) {
    const int base = shapeBase(shape);
    std::vector<double> target = before; // every param of the shape, as it is to be
    if (target.empty())
        for (int f = 0; f < shapeParamCount(shape); ++f)
            target.push_back(value(base + f));
    auto was = [&](int index) { return target[static_cast<size_t>(index - base)]; };
    const UiLayout &layout = controller().layout();
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

// Alt+click anywhere in the big editor or a widened LFO card, over its points and bends too, draws a step; or
// with Steps picked, a plain click.
bool ShapeEditor::drawsStep(const Widget &w, Modifiers mods) const {
    return mods.alt != shapeSteps(w.a) &&
           (w.kind == Kind::ShapeArea || w.kind == Kind::ShapePoint || w.kind == Kind::ShapeCurve) &&
           (w.a == shapeEditing() || w.a == wideLfo());
}

void ShapeEditor::removeShapePoint(int shape, int point) {
    if (middlePoint(shape, point)) {
        host_.flash(tr(Str::MsgCurveMiddle));
        return;
    }
    const int count = shapePointCount(shape);
    const bool end = isCurve(shape) && point + 1 == count;
    if (point == 0 || end || count <= 2) {
        host_.flash(tr(isStripCurve(shape) && (point == 0 || end) ? Str::MsgNoteCurveEnds
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

void ShapeEditor::resetShape(int shape) {
    clearShapePick();
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < shapeParamCount(shape); ++f)
        changes.push_back({shapeBase(shape) + f, table_[shapeBase(shape) + f].def});
    if (isNoteCurve(shape)) // and its range, every MIDI note
        for (int index : {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)})
            changes.push_back({index, table_[index].def});
    editMany(changes);
}

void ShapeEditor::stepShapeGrid(int axis, int steps) {
    UiLayout &layout = controller().layout();
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
    controller().layoutChanged();
}

std::vector<ShapeEditor::DrawnPoint> ShapeEditor::drawnPoints(int shape) const {
    std::vector<DrawnPoint> points;
    for (int point = 0; point < shapePointCount(shape); ++point)
        points.push_back({shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)),
                          value(shapeIndex(shape, point, PointField::Curve))});
    return points;
}

std::optional<std::vector<std::pair<int, double>>> ShapeEditor::drawnChanges(int shape,
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
    // The points it had past those back at their defaults, so a preset saved sparse leaves them out.
    for (int point = static_cast<int>(points.size()); point < shapePointCount(shape); ++point)
        for (const PointField field : {PointField::X, PointField::Y, PointField::Curve})
            put(shapeIndex(shape, point, field), table_[shapeIndex(shape, point, field)].def);
    put(shapeBase(shape), static_cast<double>(points.size()));
    return changes;
}

bool ShapeEditor::shapeCopied() {
    return !shapeClipboard().empty();
}

std::string ShapeEditor::shapeName(int shape) const {
    if (isOpEnvCurve(shape))
        return trf(Str::NameOpEnvelope, {std::string(1, static_cast<char>('A' + opOfEnvCurve(shape)))});
    if (isNoteCurve(shape))
        return tr(Str::LabelNote);
    if (isVelocityCurve(shape))
        return tr(Str::LabelVelocitySource);
    if (isShaperCurve(shape))
        return trf(Str::NameShaper, {std::to_string(shape - kMaxSources - kFxSlots + 1)});
    if (isClipCurve(shape))
        return tr(Str::NameShaperEffect);
    return mods_.sourceName(shape);
}

// As it sounds: a symmetric curve's left half is its right half turned round its middle, not the points it keeps there.
// Its right half starts at the middle, where it stands there (a point put there on its segment, where it has none).
void ShapeEditor::copyShape(int shape) {
    ShapeClip clip;
    const std::vector<DrawnPoint> points = drawnPoints(shape);
    if (curveSymmetric(shape)) {
        std::vector<ShapeClip::Point> all;
        for (const DrawnPoint &p : points)
            all.push_back({p.x, p.y, p.curve});
        const size_t middle = splitClipAt(all, 0.5);
        std::vector<ShapeClip::Point> right(all.begin() + static_cast<std::ptrdiff_t>(middle), all.end());
        right.front().x = 0.5;
        for (size_t i = right.size() - 1; i >= 1; --i) // a segment run backward bends the other way
            clip.points.push_back({1.0 - right[i].x, -right[i].y, -right[i - 1].curve});
        if (std::abs(right.front().y) > 1e-9) // a curve that doesn't pass through the middle jumps there
            clip.points.push_back({0.5, -right.front().y, 0.0});
        clip.points.insert(clip.points.end(), right.begin(), right.end());
    } else {
        for (const DrawnPoint &p : points)
            clip.points.push_back({p.x, p.y, p.curve});
    }
    clip.wraps = !isCurve(shape);
    if (loopedShape(shape)) {
        const LfoLoop loop = loopOf(shape);
        clip.looped = true;
        clip.loopStart = loop.start;
        clip.loopEnd = loop.end;
    }
    shapeClipboard() = clip;
    host_.flash(trf(Str::MsgShapeCopied, {shapeName(shape)}));
}

void ShapeEditor::pasteShape(int shape) {
    const ShapeClip &clip = shapeClipboard();
    if (clip.empty()) {
        host_.flash(tr(Str::MsgNothingCopied));
        return;
    }
    clearShapePick();
    std::vector<ShapeClip::Point> points = isCurve(shape) ? clipToEnds(clip) : clip.points;
    points.front().x = 0.0;
    const size_t had = points.size();
    const size_t most = static_cast<size_t>(shapeMaxPoints(shape));
    const bool symmetric = curveSymmetric(shape);
    points = thinClip(points, symmetric ? most - 1 : most); // a symmetric one's middle point, with room for it
    const size_t kept = points.size();                       // of the clip's own
    if (symmetric)
        splitClipAt(points, 0.5);
    std::vector<DrawnPoint> drawn;
    for (const ShapeClip::Point &p : points)
        drawn.push_back({std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, -1.0, 1.0), std::clamp(p.curve, -1.0, 1.0)});
    auto changes = drawnChanges(shape, drawn);
    if (!changes)
        return;
    if (clip.looped && loopedShape(shape)) {
        const auto [start, end] = loopParams(shape);
        for (const auto &[index, v] : {std::pair{start, clip.loopStart}, std::pair{end, clip.loopEnd}})
            if (value(index) != v)
                changes->push_back({index, v});
    }
    if (!changes->empty())
        editMany(*changes);
    if (kept < had)
        host_.flash(trf(Str::MsgShapeThinned, {std::to_string(kept), std::to_string(had)}));
    else
        host_.flash(trf(Str::MsgShapePasted, {shapeName(shape)}));
}

// Inverting flips each level; a bend keeps its value, which bends the flipped segment the mirrored way. Reversed,
// the shape runs from the end back to the start: the first point stays first, the others come in the other order,
// each at 1 - its time, and a segment run backwards bends the other way (shapeBend(): 1 - bend(1 - t, c) is
// bend(t, -c)). Shifted, the shape moves a column of the grid later (right) or earlier (left), round the cycle: the
// point where the new start falls, or one put there on the curve (its segment split in two, each part bent as much as
// keeps it on the same curve), becomes the first.
void ShapeEditor::applyShapeTool(int lfo, ShapeTool tool) {
    if (tool != ShapeTool::Copy) // the points move along the shape, or round it
        clearShapePick();
    if (tool == ShapeTool::Copy) {
        copyShape(lfo);
        return;
    }
    if (tool == ShapeTool::Paste) {
        pasteShape(lfo);
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
        const double column = 1.0 / std::max(controller().layout().shapeGridX, 1);
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
        host_.flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(lfo))}));
        return;
    }
    if (!changes->empty())
        editMany(*changes);
}

// --- Picking points to move or remove together.

std::vector<int> ShapeEditor::shapePicked(int shape) const {
    std::vector<int> points;
    if (shape < 0 || shapePick_.shape != shape)
        return points;
    const int count = shapePointCount(shape);
    if (shapePick_.count != count)
        return points; // points came or went since: what was picked isn't those points any more
    for (int point : shapePick_.points)
        if (point >= 0 && point < count && pointShown(shape, point))
            points.push_back(point);
    return points;
}

bool ShapeEditor::shapePointPicked(int shape, int point) const {
    const std::vector<int> points = shapePicked(shape);
    return std::find(points.begin(), points.end(), point) != points.end();
}

void ShapeEditor::setShapePick(int shape, std::vector<int> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    shapePick_ = {shape, shape >= 0 ? shapePointCount(shape) : 0, std::move(points)};
}

bool ShapeEditor::shapeEditable(int shape) const {
    if (shape < 0)
        return false;
    for (const Widget &w : host_.listedWidgets())
        if (w.kind == Kind::ShapeArea && w.a == shape)
            return true;
    return false;
}

bool ShapeEditor::shapePointPinned(int shape, int point) const {
    return point == 0 || (isCurve(shape) && point + 1 == shapePointCount(shape)) || middlePoint(shape, point);
}

void ShapeEditor::moveShapePoints(int shape, int grabbed, double x, double y) {
    // Every point moves as far as the one grabbed, as far as each can: up and down within the shape, and across
    // between its nearest neighbors that stay (those not picked, and those that stay where they are across: the first,
    // a curve's last, a symmetric curve's middle), so the points keep their order and their spacing.
    const std::vector<int> &points = drag_.points;
    const auto &origin = drag_.origin;
    const auto at = std::find(points.begin(), points.end(), grabbed);
    if (at == points.end() || origin.size() != points.size())
        return;
    const size_t g = static_cast<size_t>(at - points.begin());
    const int count = shapePointCount(shape);
    const double apart = isCurve(shape) ? 0.002 : 0.0;
    auto moving = [&](int point) {
        return std::find(points.begin(), points.end(), point) != points.end() && !shapePointPinned(shape, point);
    };
    double dx = x - origin[g].first, dy = y - origin[g].second;
    for (size_t i = 0; i < points.size(); ++i) {
        dy = std::clamp(dy, std::min(-1.0 - origin[i].second, 0.0), std::max(1.0 - origin[i].second, 0.0));
        const int point = points[i];
        if (!moving(point))
            continue;
        int below = point - 1, above = point + 1;
        while (below >= 0 && moving(below))
            --below;
        while (above < count && moving(above))
            ++above;
        const double low = std::max(below >= 0 ? shapePointX(shape, below) : 0.0, curveSymmetric(shape) ? 0.5 : 0.0);
        const double high = above < count ? shapePointX(shape, above) : 1.0;
        dx = std::clamp(dx, std::min(low + apart - origin[i].first, 0.0), std::max(high - apart - origin[i].first, 0.0));
    }
    for (size_t i = 0; i < points.size(); ++i) {
        if (moving(points[i]))
            set(shapeIndex(shape, points[i], PointField::X), origin[i].first + dx);
        set(shapeIndex(shape, points[i], PointField::Y), origin[i].second + dy);
    }
}

void ShapeEditor::removeShapePoints(int shape, const std::vector<int> &points) {
    const int count = shapePointCount(shape);
    std::vector<bool> gone(static_cast<size_t>(count), false);
    int left = count;
    for (int point : points)
        if (point >= 0 && point < count && !shapePointPinned(shape, point) && !gone[static_cast<size_t>(point)] &&
            left > 2) {
            gone[static_cast<size_t>(point)] = true;
            --left;
        }
    if (left == count) { // none of them can go: say why, as for one
        if (!points.empty())
            removeShapePoint(shape, points.front());
        return;
    }
    // Those that stay close up, in order; the places past them are left as they were, unused.
    std::vector<std::pair<int, double>> changes;
    int to = 0;
    for (int point = 0; point < count; ++point) {
        if (gone[static_cast<size_t>(point)])
            continue;
        if (to != point) {
            changes.push_back({shapeIndex(shape, to, PointField::X), shapePointX(shape, point)});
            for (PointField field : {PointField::Y, PointField::Curve})
                changes.push_back({shapeIndex(shape, to, field), value(shapeIndex(shape, point, field))});
        }
        ++to;
    }
    changes.push_back({shapeBase(shape), static_cast<double>(left)});
    editMany(changes);
    clearShapePick();
}

// The View hands them on with nothing typed, no dialog open and no drag going on, before the other panels (see
// View::seqKey()).
bool ShapeEditor::key(Key key, bool) {
    if (key == Key::Copy || key == Key::Paste) { // over a shape: it copied, or the shape copied pasted into it
        const std::optional<Widget> &hover = host_.hovered();
        if (!hover || (hover->kind != Kind::ShapeArea && hover->kind != Kind::ShapePoint &&
                       hover->kind != Kind::ShapeCurve && hover->kind != Kind::ShapeLoop) ||
            !shapeEditable(hover->a))
            return false;
        if (key == Key::Copy)
            copyShape(hover->a);
        else
            pasteShape(hover->a);
        return true;
    }
    if (key == Key::Delete) {
        const int shape = shapePick_.shape;
        const std::vector<int> points = shapePicked(shape);
        if (points.empty() || !shapeEditable(shape))
            return false;
        removeShapePoints(shape, points);
        return true;
    }
    if (key == Key::SelectAll) {
        // The shape under the mouse, else the big editor's, else a widened card's, unless the SEQ view shows beside
        // it: Ctrl+A there picks the roll's notes.
        int shape = -1;
        const std::optional<Widget> &hover = host_.hovered();
        if (hover && (hover->kind == Kind::ShapeArea || hover->kind == Kind::ShapePoint ||
                      hover->kind == Kind::ShapeCurve))
            shape = hover->a;
        else if (shapeEditing() >= 0)
            shape = shapeEditing();
        else if (wideLfo() >= 0 && !(controller().layout().view == RoutingView::Seq && partialEditing() < 0))
            shape = wideLfo(); // (the SEQ view shows while no full-size editor is over it)
        if (shape < 0 || !shapeEditable(shape))
            return false;
        std::vector<int> points;
        for (int point = 0; point < shapePointCount(shape); ++point)
            if (pointShown(shape, point))
                points.push_back(point);
        setShapePick(shape, points);
        return true;
    }
    return false;
}


// --- What is open.

void ShapeEditor::close() {
    shapeEditor_ = -1;
    partialEditor_ = -1;
}

void ShapeEditor::openPartials(int op) {
    partialEditor_ = op;
    shapeEditor_ = -1;
}

void ShapeEditor::closePartialsBut(int op) {
    if (partialEditor_ != op)
        partialEditor_ = -1;
}

// Widened, an LFO's card has its shape, and the full-size editor not.
void ShapeEditor::widen(int lfo) {
    wideLfo_ = lfo;
    if (shapeEditing() >= 0 && !isCurve(shapeEditing()))
        shapeEditor_ = -1;
}

void ShapeEditor::narrow(int lfo) {
    if (wideLfo_ == lfo)
        wideLfo_ = -1;
}

void ShapeEditor::shapeGone(int shape) {
    if (shapeEditor_ == shape)
        shapeEditor_ = -1;
    if (wideLfo_ == shape)
        wideLfo_ = -1;
}

Color ShapeEditor::clipColor(int unit) const {
    return unit < kFxSlots ? fxTypeColor(fx_.slotType(unit)) : kShaperColor;
}

bool ShapeEditor::isCustomWave(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Custom);
}

// --- The full-size editors and the shapes in place.

void ShapeEditor::paintShapeEditor(Canvas &c, int shape) {
    if (isNoteCurve(shape)) {
        paintNoteCurveEditor(c);
        return;
    }
    if (isVelocityCurve(shape)) {
        paintVelocityCurveEditor(c);
        return;
    }
    if (isClipCurve(shape) || isShaperCurve(shape)) {
        paintClipCurveEditor(c, shape);
        return;
    }
    if (isOpEnvCurve(shape)) {
        paintOpEnvEditor(c, shape);
        return;
    }
    const int lfo = shape;
    const Color color = kSourceColors[lfo];
    const Rect title = shapeTitle();
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, trf(Str::TitleShape, {mods_.sourceName(lfo)}), kTextBody, color, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    if (curveShape(lfo)) // the loop's strip and the ruler, over the plot's frame
        c.fillRect(loopStrip(lfo), kDisplayColor, kRadiusControl);
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const double zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? -1.0 : 0.0;
    paintShapeGrid(c, shapeArea(), zero, false);
    paintShapeGridControls(c);

    // The curve, filled toward the zero line.
    auto get = [this](int index) { return value(index); };
    std::vector<double> key = curveKey(Curve::LfoEditor, r);
    keyShape(key, shapeCountIndex(lfo));
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        shapeOutline(get, lfo, 400, [&](double x, double y) { points.push_back(shapeAt(x, y)); });
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, zero));
    area.push_back(shapeAt(0.0, zero));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (curveShape(lfo))
        paintLoop(c, lfo, r, false);
    paintPhase(c, lfo, r);

    if (const auto now = host_.lfoNow(lfo)) {
        if (curveShape(lfo)) { // a line across at the level it puts out, which a release can take off its curve
            const float y = shapeAt(0.0, std::clamp(host_.lfoLevelNow(lfo), -1.0, 1.0)).y;
            c.line({r.x, y}, {r.right(), y}, color.alpha(0.35f), 1.0f, true);
        }
        const Point dot = shapeAt(now->first, now->second);
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse, in numbers: where it is along the cycle (a Curve's in time or in beats, where its
    // places read so) and what the LFO puts out there, a unipolar one mapping -1..1 onto 0..1; else how to edit.
    const Widget *focus = focused(DragKind::ShapePoint);
    const Rect below = shapeBelow();
    if (focus && focus->kind == Kind::ShapePoint && focus->a == lfo) {
        const double x = shapePointX(lfo, focus->b);
        const double y = value(shapePointIndex(lfo, focus->b, PointField::Y));
        const std::string level = zero < 0.0 ? number("%.2f", 0.5 * (y + 1.0)) : number("%+.2f", y);
        const std::string point = std::to_string(focus->b + 1);
        c.text(below,
               mods_.curveTime(lfo) == ModulationModel::CurveTime::Share
                   ? trf(Str::CaptionShapePoint, {point, number("%.1f", x * 100.0), level})
                   : trf(Str::CaptionShapePointAt, {point, mods_.curvePlace(lfo, x, false), level}),
               kTextLabel, kDim, Align::Left);
    } else {
        paintHintBelow(c, below, shapeHint());
    }
}

// An operator's drawn envelope: its level up from the bottom, its time across, filled from the bottom as a unipolar
// shape is; a line across at the newest voice's level and a dot on the shape where it is, as an envelope's.
void ShapeEditor::paintOpEnvCurve(Canvas &c, int shape, Rect r) {
    const Color color = shapeColor(shape);
    auto at = [&](double x, double y) { return areaAt(r, x, y); };
    std::vector<double> key = curveKey(Curve::LfoEditor, r);
    key.push_back(static_cast<double>(shape));
    keyShape(key, shapeBase(shape));
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> out;
        const int count = shapePointCount(shape);
        for (int point = 0; point + 1 < count; ++point) {
            const double x0 = shapePointX(shape, point), x1 = shapePointX(shape, point + 1);
            const double y0 = value(shapeIndex(shape, point, PointField::Y));
            const double y1 = value(shapeIndex(shape, point + 1, PointField::Y));
            const double curve = value(shapeIndex(shape, point, PointField::Curve));
            out.push_back(at(x0, y0));
            const int n = x1 - x0 < kShapeSameTime ? 0 : std::max(1, static_cast<int>(std::ceil((x1 - x0) * 200.0)));
            for (int i = 1; i < n; ++i) {
                const double t = static_cast<double>(i) / n;
                out.push_back(at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * shapeBend(t, curve)));
            }
        }
        out.push_back(at(1.0, value(shapeIndex(shape, count - 1, PointField::Y))));
        return out;
    });
    std::vector<Point> area = points;
    area.push_back(at(1.0, -1.0));
    area.push_back(at(0.0, -1.0));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    paintLoop(c, shape, r, shapeEditing() != shape);
    const int op = opOfEnvCurve(shape);
    const Telemetry &t = host_.telemetry();
    if (t.voices > 0 && t.opEnvPhase[op] >= 0.0f) {
        const double level = std::clamp(static_cast<double>(t.opEnvLevels[op]), 0.0, 1.0);
        const float y = at(0.0, 2.0 * level - 1.0).y;
        c.line({r.x, y}, {r.right(), y}, color.alpha(0.35f), 1.0f, true);
        // The dot on the shape where it is (a release carries it off the shape for a while: the line shows that).
        const double x = std::clamp(static_cast<double>(t.opEnvPhase[op]), 0.0, 1.0);
        const int count = shapePointCount(shape);
        int from = 0;
        while (from + 2 < count && shapePointX(shape, from + 1) <= x)
            ++from;
        const double x0 = shapePointX(shape, from), x1 = shapePointX(shape, from + 1);
        const double tt = x1 - x0 > kShapeSameTime ? std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) : 1.0;
        const double y0 = value(shapeIndex(shape, from, PointField::Y)), y1 = value(shapeIndex(shape, from + 1, PointField::Y));
        const Point dot = at(x, y0 + (y1 - y0) * shapeBend(tt, value(shapeIndex(shape, from, PointField::Curve))));
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }
}

void ShapeEditor::paintOpEnvEditor(Canvas &c, int shape) {
    const Color color = shapeColor(shape);
    const int op = opOfEnvCurve(shape);
    const Rect title = shapeTitle();
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, trf(Str::TitleOpEnvelope, {std::string(1, static_cast<char>('A' + op))}), kTextBody, color,
           Align::Center, true);
    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    c.fillRect(loopStrip(shape), kDisplayColor, kRadiusControl);
    paintShapeGrid(c, r, -1.0, false);
    paintShapeGridControls(c);
    paintOpEnvCurve(c, shape, r);
    // The point under the mouse, in numbers: its time (or place in beats) and its level; else how to edit.
    const Widget *focus = focused(DragKind::ShapePoint);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const double y = value(shapeIndex(shape, focus->b, PointField::Y));
        c.text(shapeBelow(),
               trf(Str::CaptionShapePointAt, {std::to_string(focus->b + 1),
                                               placeText(shape, shapePointX(shape, focus->b), false),
                                               number("%.2f", 0.5 * (y + 1.0))}),
               kTextLabel, kDim, Align::Left);
    } else {
        paintHintBelow(c, shapeBelow(), shapeHint());
    }
}

void ShapeEditor::paintOpEnvelope(Canvas &c, int op) {
    const int shape = opEnvCurve(op);
    paintShapeGrid(c, kOpEnvPlot, -1.0, false);
    paintOpEnvCurve(c, shape, kOpEnvPlot);
}

std::string ShapeEditor::shapeHint() const {
    const UiLayout &layout = controller().layout();
    return trf(Str::HintShape, {tr(layout.shapeSteps ? Str::HintShapeSteps : Str::HintShapePoints),
                                tr(layout.shapeSnap ? Str::HintCtrlFree : Str::HintCtrlSnap)});
}

// A shaper's drawn curve: input across, output up, the plain line (no change) for reference.
void ShapeEditor::paintClipCurveEditor(Canvas &c, int shape) {
    const int unit = shape - kMaxSources;
    const Color fx = clipColor(unit);
    const Rect panel = shapePanel();
    const Rect title{panel.x + 16.0f, panel.y + 9.0f, 132.0f, 22.0f};
    c.fillRect(title, fx.alpha(0.18f), kRadiusControl);
    c.text(title,
           unit < kFxSlots ? trf(Str::TitleFxShaperCurve, {std::to_string(unit + 1)})
                           : trf(Str::TitleShaperCurve, {std::to_string(unit - kFxSlots + 1)}),
           kTextBody, fx, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), 0.0, true);
    paintShapeGridControls(c);
    c.line(shapeAt(0.0, -1.0), shapeAt(1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    c.text({r.right() - 120.0f, r.bottom() - 18.0f, 116.0f, 14.0f}, tr(Str::AxisInput), kTextLabel, kFaint, Align::Right);
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOutput), kTextLabel, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    const bool symmetric = curveSymmetric(shape);
    std::vector<double> key = curveKey(Curve::ClipEditor, r);
    key.push_back(symmetric ? 1.0 : 0.0);
    keyShape(key, base);
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 400; ++i) {
            const double x = i / 400.0;
            points.push_back(shapeAt(x, shaperCurveValue(get, ClipMode::Custom, base, symmetric, 2.0 * x - 1.0)));
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, 0.0));
    area.push_back(shapeAt(0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    if (symmetric) // the mirrored half, which follows the other, behind a veil
        c.fillRect({r.x, r.y, r.w * 0.5f, r.h}, kDisplayColor.alpha(0.35f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    paintMirroredPoints(c, shape, 4.0f);

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = focused(DragKind::ShapePoint);
    // (A voice shaper's editor has its switches there too, left of the grid's controls and their label.)
    const float end = isClipCurve(shape) ? shapeGridButton(0, 0).x - 48.0f : shapeSymButton().x;
    const Rect below{r.x, r.bottom() + 10.0f, end - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const std::string in = number("%+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0);
        const std::string out = number("%+.2f", value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text(below, trf(Str::CaptionCurvePoint, {std::to_string(focus->b + 1), in, out}), kTextLabel, kDim, Align::Left);
    } else {
        paintHintBelow(c, below, shapeHint());
    }
}

// The note source's curve: the notes from Low to High across (the Cs between them named under the plot), what
// the source puts out up, from nothing at the bottom to all of it at the top, and for reference, dashed, what would
// take a cutoff at 0 by 100 % onto the note (the line it starts as); while notes play, a dot where the newest
// voice is.
void ShapeEditor::paintSourceCurve(Canvas &c, int shape, detail::Curve curve, Rect r, Color color, double dot) {
    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    std::vector<double> key = curveKey(curve, r);
    keyShape(key, base);
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 400; ++i) {
            const double x = i / 400.0;
            points.push_back(shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0)));
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, -1.0));
    area.push_back(shapeAt(0.0, -1.0));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (dot >= 0.0) {
        const Point at = shapeAt(dot, clipCurveValue(get, base, 2.0 * dot - 1.0));
        c.fillCircle(at, 8.0f, color.alpha(0.25f));
        c.fillCircle(at, 3.5f, kText);
    }
}

void ShapeEditor::paintNoteCurveEditor(Canvas &c) {
    const int shape = kNoteCurve;
    const Color color = shapeColor(shape);
    const Rect title = shapeTitle();
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, tr(Str::TitleNoteCurve), kTextBody, color, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), -1.0, false);
    paintShapeGridControls(c);
    const double low = value(paramIndex(NoteParam::Low)), high = value(paramIndex(NoteParam::High));
    std::vector<double> key = curveKey(Curve::NoteExact, r);
    key.insert(key.end(), {low, high});
    const std::vector<Point> exact = curveCache().get(key, [&] { // what would put a cutoff at 0 by 100 % on the note
        std::vector<Point> exact;
        for (int i = 0; i <= 120; ++i) {
            const double x = i / 120.0;
            exact.push_back(shapeAt(x, 2.0 * cutoffNormOfPitch(low + x * (high - low)) - 1.0));
        }
        return exact;
    });
    c.polyline(exact.data(), exact.size(), kFaint.alpha(0.6f), 1.0f, true);
    for (int octave = 0; octave * 12.0 <= kMaxNotePitch; ++octave) {
        const double x = high == low ? -1.0 : (octave * 12.0 - low) / (high - low);
        if (x > 0.03 && x < 0.97)
            c.text({shapeAt(x, -1.0).x - 20.0f, r.bottom() - 7.0f, 40.0f, 14.0f}, noteName(octave * 12), kTextLabel, kFaint);
    }
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOut), kTextLabel, kFaint, Align::Left);

    const double pitch = host_.telemetry().notePitch;
    const double across = high == low ? (pitch >= low ? 1.0 : 0.0) : std::clamp((pitch - low) / (high - low), 0.0, 1.0);
    paintSourceCurve(c, shape, Curve::NoteEditor, r, color, host_.telemetry().voices > 0 ? across : -1.0);

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = focused(DragKind::ShapePoint);
    const Rect below = shapeBelow();
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char note[32];
        formatParam(paramIndex(NoteParam::Low), low + shapePointX(shape, focus->b) * (high - low), note, sizeof(note));
        const std::string out = number("%.0f", 50.0 * (value(shapeIndex(shape, focus->b, PointField::Y)) + 1.0));
        c.text(below, trf(Str::CaptionNotePoint, {std::to_string(focus->b + 1), note, out}), kTextLabel, kDim, Align::Left);
    } else {
        paintHintBelow(c, below, shapeHint());
    }
}

// Velocity's curve: the velocities across (0 to 127, marked every 32), what the source puts out up, from nothing at
// the bottom to all of it at the top, and for reference, dashed, the straight line it starts as; while notes play, a
// dot where the newest voice's velocity is.
void ShapeEditor::paintVelocityCurveEditor(Canvas &c) {
    const int shape = kVelocityCurve;
    const Color color = shapeColor(shape);
    const Rect title = shapeTitle();
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, tr(Str::TitleVelocityCurve), kTextBody, color, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), -1.0, false);
    paintShapeGridControls(c);
    c.line(shapeAt(0.0, -1.0), shapeAt(1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    for (int velocity = 32; velocity < 127; velocity += 32)
        c.text({shapeAt(velocity / 127.0, -1.0).x - 20.0f, r.bottom() - 7.0f, 40.0f, 14.0f}, std::to_string(velocity),
               kTextLabel, kFaint);
    c.text({r.right() - 120.0f, r.bottom() - 9.0f, 116.0f, 14.0f}, tr(Str::AxisVelocity), kTextLabel, kFaint, Align::Right);
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOut), kTextLabel, kFaint, Align::Left);

    paintSourceCurve(c, shape, Curve::VelocityEditor, r, color,
                     host_.telemetry().voices > 0 ? std::clamp(static_cast<double>(host_.telemetry().noteVelocity), 0.0, 1.0)
                                                  : -1.0);

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = focused(DragKind::ShapePoint);
    const Rect below = shapeBelow();
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const std::string in = number("%.0f", 127.0 * shapePointX(shape, focus->b));
        const std::string out = number("%.0f", 50.0 * (value(shapeIndex(shape, focus->b, PointField::Y)) + 1.0));
        c.text(below, trf(Str::CaptionVelocityPoint, {std::to_string(focus->b + 1), in, out}), kTextLabel, kDim,
               Align::Left);
    } else {
        paintHintBelow(c, below, shapeHint());
    }
}

int ShapeEditor::partialEditing() const {
    if (partialEditor_ < 0 || shapeEditing() >= 0)
        return -1;
    return isCustomWave(partialEditor_) ? partialEditor_ : -1;
}

int ShapeEditor::partialsShown(int op) const {
    return partialCount(value(partialCountIndex(op)));
}

void ShapeEditor::paintPartialEditor(Canvas &c, int op) {
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 120.0f, 22.0f};
    c.fillRect(title, kAccent.alpha(0.18f), kRadiusControl);
    c.text(title, trf(Str::TitlePartials, {kNodeNames[op]}), kTextBody, kAccent, Align::Center, true);
    // Only the partials the wave plays show; the ones past them keep their values for when there are more again.
    const int count = partialsShown(op);
    // The partial under the mouse in numbers, or how to draw.
    const Widget *focus = focused(DragKind::Partials);
    std::string hint = tr(Str::HintPartials);
    if (focus && focus->kind == Kind::PartialArea) {
        const Point at = drag_.kind == DragKind::Partials ? drag_.now : host_.mouse();
        const int k = partialAt(focus->b == 0 ? kPartialLevels : kPartialPhases, at.x, count);
        const double level = value(partialLevelIndex(op, k));
        const std::string partial = std::to_string(k + 1), phase = number("%.0f", value(partialPhaseIndex(op, k)) * 360.0);
        hint = level > 0.0 ? trf(Str::CaptionPartial, {partial, number("%.1f", 20.0 * std::log10(level)), phase})
                           : trf(Str::CaptionPartialSilent, {partial, phase});
    }
    const float hintX = partialCountButton(static_cast<int>(std::size(kPartialCounts)) - 1).right() + 12.0f;
    c.text({hintX, kRouting.y + 10.0f, partialRandom().x - hintX - 12.0f, 22.0f}, hint, kTextLabel, kFaint, Align::Right);

    // The wave they make, one cycle, at its loudest from top to bottom, as the operator plays it.
    const Rect wave = kPartialWave;
    c.fillRect(wave.inset(-6.0f), kDisplayColor, kRadiusCard);
    c.line({wave.x, wave.center().y}, {wave.right(), wave.center().y}, kBorder, 1.0f);
    std::vector<double> key = curveKey(Curve::Partials, wave);
    for (int k = 0; k < count; ++k)
        key.insert(key.end(), {value(partialLevelIndex(op, k)), value(partialPhaseIndex(op, k))});
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<double> samples(241, 0.0);
        double peak = 0.0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const double x = static_cast<double>(i) / (samples.size() - 1);
            for (int k = 0; k < count; ++k)
                if (const double level = value(partialLevelIndex(op, k)); level > 0.0)
                    samples[i] +=
                        level * std::sin(2.0 * std::numbers::pi * ((k + 1) * x + value(partialPhaseIndex(op, k))));
            peak = std::max(peak, std::abs(samples[i]));
        }
        std::vector<Point> points;
        for (size_t i = 0; i < samples.size(); ++i) {
            const float y = static_cast<float>(peak > 0.0 ? samples[i] / peak : 0.0);
            points.push_back({wave.x + wave.w * i / (samples.size() - 1), wave.center().y - y * wave.h * 0.5f});
        }
        return points;
    });
    plotLine(c, points.data(), points.size(), kAccent, 2.0f);

    // Levels (in dB, a faint line every 12) and phases (a full cycle at the top) alike as bars from the bottom,
    // in different colors; a phase is faint while its partial is silent.
    for (int row = 0; row < 2; ++row) {
        const Rect area = row == 0 ? kPartialLevels : kPartialPhases;
        c.fillRect(area.inset(-6.0f), kDisplayColor, kRadiusCard);
        c.text({area.x - 2.0f, area.y - 22.0f, 80.0f, 12.0f}, tr(row == 0 ? Str::HeadingLevel : Str::HeadingPhase), kTextLabel,
               kFaint, Align::Left, true);
        for (int k = 0; k < count; ++k) {
            const Rect bar = partialBar(area, k, count);
            const double v = row == 0 ? partialBarFromLevel(value(partialLevelIndex(op, k)))
                                      : value(partialPhaseIndex(op, k));
            const float h = static_cast<float>(v) * bar.h;
            c.fillRect({bar.x, area.y, bar.w, area.h}, kRaised.alpha(0.5f), kRadiusMark);
            if (h > 0.5f) {
                const bool silent = row == 1 && value(partialLevelIndex(op, k)) <= 0.0;
                const float alpha = (0.35f + 0.5f * static_cast<float>(v)) * (silent ? 0.4f : 1.0f);
                c.fillRect({bar.x, bar.bottom() - h, bar.w, h}, (row == 0 ? kAccent : kFxColor).alpha(alpha), kRadiusMark);
            }
        }
        if (row == 0)
            for (int db = 0; db > kPartialFloorDb; db -= 12) {
                const double bar = partialBarFromLevel(std::pow(10.0, db / 20.0));
                const float y = area.bottom() - static_cast<float>(bar) * area.h;
                if (db < 0)
                    c.line({area.x, y}, {area.right(), y}, kBorder.alpha(0.5f), 1.0f);
                c.text({area.x - 38.0f, y - 6.0f, 30.0f, 12.0f}, std::to_string(db), kTextSmall, kFaint, Align::Right);
            }
    }
    // Each partial's number under it; of 64, the first and every fourth, where there is room for them.
    for (int k = 0; k < count; ++k) {
        if (count > kFirstPartials && k > 0 && (k + 1) % 4 != 0)
            continue;
        const Rect bar = partialBar(kPartialPhases, k, count);
        c.text({bar.center().x - 12.0f, kPartialPhases.bottom() + 8.0f, 24.0f, 12.0f}, std::to_string(k + 1), kTextLabel,
               kFaint);
    }
}

void ShapeEditor::paintShapeGrid(Canvas &c, Rect r, double zero, bool centered) {
    const int across = controller().layout().shapeGridX;
    const int upDown = controller().layout().shapeGridY;
    // Lines across stand out every quarter, or every third on a grid of thirds; a centered grid's middle more.
    const int major = across % 4 == 0 && across > 4 ? across / 4 : (across % 3 == 0 && across > 3 ? across / 3 : 0);
    for (int i = 1; i < across; ++i) {
        const float x = r.x + r.w * i / across;
        const float alpha = centered && 2 * i == across ? 1.0f : (major > 0 && i % major == 0 ? 0.9f : 0.35f);
        c.line({x, r.y}, {x, r.bottom()}, kBorder.alpha(alpha), 1.0f);
    }
    // Up and down: solid at zero, dashed elsewhere, fainter between the halves.
    for (int i = 0; i <= upDown; ++i) {
        const double y = -1.0 + 2.0 * i / upDown;
        const bool atZero = std::abs(y - zero) < 1e-9;
        const bool half = 4 * i % upDown == 0;
        c.line(areaAt(r, 0.0, y), areaAt(r, 1.0, y), kBorder.alpha(atZero ? 1.0f : (half ? 0.5f : 0.3f)), 1.0f,
               !atZero);
    }
}

// "GRID - 16 + x - 8 +" under the editor's right end; the buttons and counts are widgets.
void ShapeEditor::paintShapeGridControls(Canvas &c) {
    const Rect first = shapeGridButton(0, 0);
    c.text({first.x - 48.0f, first.y, 42.0f, first.h}, tr(Str::HeadingGrid), kTextLabel, kFaint, Align::Right, true);
    const Rect across = shapeGridButton(0, 2), upDown = shapeGridButton(1, 0);
    c.text({across.right(), across.y, upDown.x - across.right(), across.h}, "\xC3\x97", kTextBody, kFaint);
}

// Transfer curve: input level across, output up, with the drive, output and mix applied (the Stack curve as many
// times in a row as it is stacked, each driven and brought back down between, without its filters).
void ShapeEditor::paintShaperCurve(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int curve = kMaxSources + unit;
    const ClipMode mode = clipMode(value(clip.mode));
    if (mode == ClipMode::Custom && shapeEditing() != curve) { // with its points, unless its big editor has them
        paintClipCurveCard(c, r, unit);
        return;
    }
    const Rect inner = r.inset(8.0f);
    constexpr double kRange = 1.3;
    auto at = [&](double x, double y) {
        return Point{inner.center().x + static_cast<float>(x / kRange) * inner.w * 0.5f,
                     inner.center().y - static_cast<float>(std::clamp(y, -kRange, kRange) / kRange) * inner.h * 0.5f};
    };
    c.line(at(-kRange, 0.0), at(kRange, 0.0), kBorder, 1.0f);
    c.line(at(0.0, -kRange), at(0.0, kRange), kBorder, 1.0f);
    c.line(at(-kRange, 1.0), at(kRange, 1.0), kBorder.alpha(0.7f), 1.0f, true); // full scale
    c.line(at(-kRange, -1.0), at(kRange, -1.0), kBorder.alpha(0.7f), 1.0f, true);
    c.line(at(-kRange, -kRange), at(kRange, kRange), kFaint.alpha(0.6f), 1.0f);

    const double drive = std::pow(10.0, host_.liveValue(clip.drive) / 20.0); // as modulation takes them now
    const double output =
        std::pow(10.0, clipOutputDb(host_.liveValue(clip.output), host_.liveValue(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = host_.liveValue(clip.mix);
    auto get = [this](int index) { return value(index); };
    const bool symmetric = curveSymmetric(curve);
    const int stack = mode == ClipMode::Stack ? shaperStack(host_.liveValue(clip.stack)) : 1;
    const double between = shaperBetween(host_.liveValue(clip.drive));
    auto shape = [&](double x) {
        double y = shaperCurveValue(get, mode, shapeBase(curve), symmetric, x);
        for (int k = 1; k < stack; ++k)
            y = clipShape(mode, y * between * drive);
        return y;
    };
    std::vector<double> key = curveKey(Curve::Shaper, inner);
    key.insert(key.end(), {static_cast<double>(mode), symmetric ? 1.0 : 0.0, drive, output, mix,
                           static_cast<double>(stack)});
    if (mode == ClipMode::Custom) // a drawn curve whose big editor has its points
        keyShape(key, shapeBase(curve));
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 240; ++i) {
            const double x = kRange * (2.0 * i / 240.0 - 1.0);
            points.push_back(at(x, x + (shape(x * drive) * output - x) * mix));
        }
        return points;
    });
    plotLine(c, points.data(), points.size(), fx, 2.0f);
}

// A symmetric curve's points, mirrored onto its left half as faint rings: they move with the ones they mirror.
void ShapeEditor::paintMirroredPoints(Canvas &c, int shape, float radius) {
    if (!curveSymmetric(shape))
        return;
    const Color color = shapeColor(shape).alpha(0.45f);
    for (int point = 0; point < shapePointCount(shape); ++point)
        if (pointShown(shape, point) && !middlePoint(shape, point))
            c.strokeCircle(shapeAt(shape, 1.0 - shapePointX(shape, point), -value(shapeIndex(shape, point, PointField::Y))),
                           radius, color, 1.0f);
}

// A drawn curve in its card, as in its editor (input across, output up, to full scale), with its points and
// bends to drag (widgets); faintly behind it, what the shaper makes of it with its drive, output and mix.
void ShapeEditor::paintClipCurveCard(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int shape = kMaxSources + unit;
    c.line(shapeAt(shape, 0.0, 0.0), shapeAt(shape, 1.0, 0.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.5, -1.0), shapeAt(shape, 0.5, 1.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.0, -1.0), shapeAt(shape, 1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    const double drive = std::pow(10.0, host_.liveValue(clip.drive) / 20.0); // as modulation takes them now
    const double output =
        std::pow(10.0, clipOutputDb(host_.liveValue(clip.output), host_.liveValue(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = host_.liveValue(clip.mix);
    const bool symmetric = curveSymmetric(shape);
    auto curve = [&](double in) { return shaperCurveValue(get, ClipMode::Custom, base, symmetric, in); };
    std::vector<double> key = curveKey(Curve::Drawn, shapeArea(shape));
    key.push_back(symmetric ? 1.0 : 0.0);
    keyShape(key, base);
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0, in = 2.0 * x - 1.0;
            points.push_back(shapeAt(shape, x, curve(in)));
        }
        return points;
    });
    key[0] = static_cast<double>(Curve::Driven);
    key.insert(key.end(), {drive, output, mix});
    const std::vector<Point> driven = curveCache().get(key, [&] {
        std::vector<Point> driven;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0, in = 2.0 * x - 1.0;
            const double out = in + (curve(in * drive) * output - in) * mix;
            driven.push_back(shapeAt(shape, x, std::clamp(out, -1.1, 1.1)));
        }
        return driven;
    });
    c.polyline(driven.data(), driven.size(), fx.alpha(0.35f), 1.0f);
    std::vector<Point> area = points;
    area.push_back(shapeAt(shape, 1.0, 0.0));
    area.push_back(shapeAt(shape, 0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    paintMirroredPoints(c, shape, 3.0f);

    // The point under the mouse, in numbers (input to output), in the top left corner, which a curve
    // leaves empty.
    const Widget *focus = focused(DragKind::ShapePoint);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char text[48];
        std::snprintf(text, sizeof(text), "%+.2f \xE2\x86\x92 %+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0,
                      value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text({r.x + 6.0f, r.y + 4.0f, r.w - 12.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
    }
}

// A widened LFO card's shape: the grid and the shape in the plot's frame, filled toward its zero line, and where
// the newest voice is; its points and bends are widgets, but for while the full-size editor has the shape, which the
// plot then only shows. Under it an arrow by each of the grid's steppers.
void ShapeEditor::paintLfoCard(Canvas &c, float card, int lfo) {
    const Color color = kSourceColors[lfo];
    const Rect r = lfoCardPlot(card);
    const bool elsewhere = shapeEditing() == lfo; // the big editor has it: dimmed, as its card's controls are
    if (elsewhere)
        c.pushLayer({0.0f, 0.0f}, kDimOpacity, r.inset(-8.0f));
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const double zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? -1.0 : 0.0;
    paintShapeGrid(c, r, zero, false);
    auto get = [this](int index) { return value(index); };
    std::vector<double> key = curveKey(Curve::LfoEditor, r);
    keyShape(key, shapeCountIndex(lfo));
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        shapeOutline(get, lfo, 200, [&](double x, double y) { points.push_back(areaAt(r, x, y)); });
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(areaAt(r, 1.0, zero));
    area.push_back(areaAt(r, 0.0, zero));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (!elsewhere) { // the big editor has it, and shows them
        if (curveShape(lfo))
            paintLoop(c, lfo, r, true);
        paintPhase(c, lfo, r);
    }
    if (const auto now = host_.lfoNow(lfo)) {
        if (curveShape(lfo)) { // and a line across at the level it puts out, as in the big editor
            const float y = areaAt(r, 0.0, std::clamp(host_.lfoLevelNow(lfo), -1.0, 1.0)).y;
            c.line({r.x, y}, {r.right(), y}, color.alpha(0.35f), 1.0f, true);
        }
        const Point dot = areaAt(r, now->first, now->second);
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }
    if (elsewhere)
        c.popLayer();

    for (int axis = 0; axis < 2; ++axis) {
        const Rect down = lfoCardGridButton(card, axis, 0);
        axisIcon(c, {down.x - 8.0f, down.center().y}, axis, elsewhere ? kFaint.alpha(kDimOpacity) : kFaint);
    }
}

// --- Its widgets.

void ShapeEditor::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    const int editing = shapeEditing();
    const int partials = partialEditing();
    if (editing >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::ShapeReset, shapeReset(), -1, editing);
        for (int axis = 0; axis < 2; ++axis)
            for (int part = 0; part < 3; ++part)
                add(Kind::ShapeGrid, shapeGridButton(axis, part), -1, axis, part - 1);
        add(Kind::ShapeArea, shapeArea().inset(-8.0f), -1, editing);
        if (curveShape(editing) || isOpEnvCurve(editing)) {
            // The title row, a Curve's and an envelope's alike: the mode list, opening under this button (see popups();
            // a Curve's clock list sets its modes as its card's does, see View::setLfoClock()), a Curve's Unipolar
            // switch, the sync switch, and compact knobs: Rate (an envelope's Length), or the note value it is synced
            // to, a Curve's Phase, and Slew.
            const bool env = isOpEnvCurve(editing);
            const int op = env ? opOfEnvCurve(editing) : -1;
            auto knob = [&](int place, int param, Str label) {
                add(Kind::Knob, shapeTitleKnob(editing, place), param);
                list.back().label = tr(label);
                list.back().compact = true;
            };
            add(Kind::ListButton, shapeClockList(),
                env ? paramIndex(op, OpParam::EnvSustain) : paramIndex(editing, SourceParam::Free), kEditorList);
            if (!env)
                add(Kind::Toggle, shapeUnipolarSwitch(), paramIndex(editing, SourceParam::Unipolar));
            const int sync = env ? paramIndex(op, OpParam::EnvSync) : paramIndex(editing, SourceParam::Sync);
            add(Kind::Toggle, shapeSyncButton(editing), sync);
            const bool synced = value(sync) >= 0.5;
            if (env) {
                knob(0, paramIndex(op, synced ? OpParam::EnvDivision : OpParam::EnvLength), Str::LabelLength);
                knob(2, paramIndex(op, OpParam::EnvSlew), Str::LabelSlew);
            } else {
                const bool length = mods_.curveRateAsLength(editing);
                knob(0, paramIndex(editing, synced ? SourceParam::Division : SourceParam::Rate),
                     length ? Str::LabelLength : Str::LabelRate);
                knob(1, paramIndex(editing, SourceParam::Phase), Str::LabelPhase);
                knob(2, paramIndex(editing, SourceParam::CurveSlew), Str::LabelSlew);
            }
            if (loopShown(editing)) // over the plot, under the points
                add(Kind::ShapeLoop, loopStrip(editing), -1, editing);
        }
        for (int mode = 0; mode < 3; ++mode) { // Points | Steps in one pill, and Snap, as in a widened LFO card
            add(Kind::ShapeMode, shapeModeButton(mode), -1, editing, mode);
            if (mode == 1)
                merge(list.size() - 2);
        }
        if (isClipCurve(editing) || isShaperCurve(editing)) // a Custom curve's symmetric switch, as on its card or page
            add(Kind::Toggle, shapeSymButton(), shaperParams(editing - kMaxSources).symmetric);
        if (isNoteCurve(editing)) { // the range across, in the title row, after the title, close together
            const char *const labels[] = {tr(Str::LabelLow), tr(Str::LabelHigh)};
            const int range[] = {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)};
            for (int i = 0; i < 2; ++i) {
                add(Kind::Knob, noteRangeKnob(i), range[i]);
                list.back().label = labels[i];
                list.back().compact = true;
            }
        }
        const int count = shapePointCount(editing);
        for (int point = 0; point < count - (isCurve(editing) ? 1 : 0); ++point) { // a curve doesn't wrap
            const double next = point + 1 < count ? shapePointX(editing, point + 1) : 1.0;
            if (next - shapePointX(editing, point) < 1e-6 || !pointShown(editing, point))
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(editing, point);
            add(Kind::ShapeCurve, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, shapeIndex(editing, point, PointField::Curve),
                editing, point);
        }
        for (int point = 0; point < count; ++point) {
            if (!pointShown(editing, point))
                continue;
            const Point c = shapePointPos(editing, point);
            add(Kind::ShapePoint, {c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f}, -1, editing, point);
        }
    } else if (partials >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::PartialReset, shapeReset(), -1, partials);
        add(Kind::PartialRandom, partialRandom(), -1, partials);
        const size_t counts = list.size();
        for (int choice = 0; choice < static_cast<int>(std::size(kPartialCounts)); ++choice)
            add(Kind::Choice, partialCountButton(choice), partialCountIndex(partials), -1, kPartialCounts[choice]);
        merge(counts);
        add(Kind::PartialArea, kPartialLevels, -1, partials, 0);
        add(Kind::PartialArea, kPartialPhases, -1, partials, 1);
    }
}

void ShapeEditor::drawnWidgets(std::vector<Widget> &list, int unit, Rect display) const {
    const WidgetAdder add{list};
    const int shape = kMaxSources + unit;
    add(Kind::ShapeArea, display, -1, shape);
    const int count = shapePointCount(shape);
    for (int point = 0; point + 1 < count; ++point) {
        if (shapePointX(shape, point + 1) - shapePointX(shape, point) < 1e-6 || !pointShown(shape, point))
            continue; // a jump straight up or down has nothing to bend
        const Point c = shapeCurvePos(shape, point);
        add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(shape, point, PointField::Curve),
            shape, point);
        list.back().compact = true;
    }
    for (int point = 0; point < count; ++point) {
        if (!pointShown(shape, point))
            continue;
        const Point c = shapePointPos(shape, point);
        add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, shape, point);
        list.back().compact = true;
    }
}

void ShapeEditor::drawEnvelope(int op) {
    std::vector<std::pair<int, double>> changes{{paramIndex(op, OpParam::EnvMode), 1.0}};
    const int shape = opEnvCurve(op);
    const int base = shapeBase(shape);
    const ParamTable &table = paramTable();
    bool untouched = true;
    for (int i = 0; i < kShapeParamCount && untouched; ++i)
        untouched = value(base + i) == table[base + i].def;
    if (untouched) {
        auto time = [&](OpParam field) { return envTimeFromNorm(value(paramIndex(op, field))); };
        // An envelope segment's curve as a shape's bend: envCurve() bends the other way, and 8 to shapeBend()'s 1.
        auto bend = [&](OpParam field) {
            return std::clamp(-value(paramIndex(op, field)) * kCurveSteepness / 8.0, -1.0, 1.0);
        };
        // A hold of 0 is none (Off), as the AHDSR has it (see envRates()), where an envelope time's 0 is 1 ms.
        const double holdNorm = value(paramIndex(op, OpParam::Hold));
        const double attack = time(OpParam::Attack), hold = holdNorm > 0.0 ? timeFromNorm(holdNorm) : 0.0;
        const double decay = time(OpParam::Decay);
        const double release = time(OpParam::Release);
        const double sustain = std::clamp(value(paramIndex(op, OpParam::Sustain)), 0.0, 1.0);
        const double column = std::max(0.25 * (attack + hold + decay + release), 0.05); // the loop: some room to see
        const double scale = 1.0 / (attack + hold + decay + column + release);
        struct Point {
            double x, y, curve;
        };
        std::vector<Point> points{{0.0, -1.0, bend(OpParam::AttackCurve)}};
        double at = attack;
        if (hold > 0.0) {
            points.push_back({at * scale, 1.0, 0.0});
            at += hold;
        }
        points.push_back({at * scale, 1.0, bend(OpParam::DecayCurve)});
        at += decay;
        const double level = 2.0 * sustain - 1.0;
        const double loopStart = at * scale;
        points.push_back({loopStart, level, 0.0});
        at += column;
        const double loopEnd = at * scale;
        points.push_back({loopEnd, level, bend(OpParam::ReleaseCurve)});
        points.push_back({1.0, -1.0, 0.0});
        changes.push_back({base, static_cast<double>(points.size())});
        for (int i = 0; i < static_cast<int>(points.size()); ++i) {
            changes.push_back({shapeFieldIndex(base, i, PointField::X), points[i].x});
            changes.push_back({shapeFieldIndex(base, i, PointField::Y), points[i].y});
            changes.push_back({shapeFieldIndex(base, i, PointField::Curve), points[i].curve});
        }
        changes.push_back({paramIndex(op, OpParam::EnvSustain), 1.0});
        changes.push_back({paramIndex(op, OpParam::EnvLoopStart), loopStart});
        changes.push_back({paramIndex(op, OpParam::EnvLoopEnd), loopEnd});
        changes.push_back({paramIndex(op, OpParam::EnvLength), normFromTime(1.0)}); // its proportions, a second long
    }
    editMany(changes);
}

void ShapeEditor::opEnvWidgets(std::vector<Widget> &list, int op) const {
    const WidgetAdder add{list};
    const int shape = opEnvCurve(op);
    add(Kind::OpEnvEdit, opEnvEditButton(), -1, op);
    add(Kind::ShapeArea, kOpEnvPlot.inset(-6.0f), -1, shape);
    if (loopShown(shape))
        add(Kind::ShapeLoop, loopStrip(shape), -1, shape);
    const int count = shapePointCount(shape);
    for (int point = 0; point + 1 < count; ++point) {
        if (shapePointX(shape, point + 1) - shapePointX(shape, point) < 1e-6)
            continue; // a jump straight up or down has nothing to bend
        const Point c = shapeCurvePos(shape, point);
        add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(shape, point, PointField::Curve),
            shape, point);
        list.back().compact = true;
    }
    for (int point = 0; point < count; ++point) {
        const Point c = shapePointPos(shape, point);
        add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, shape, point);
        list.back().compact = true;
    }
}

void ShapeEditor::popups(std::vector<Widget> &list) const {
    // The mode list, opened from its title row's button (a Curve's clock modes, an envelope's One-Shot | Sustain): under
    // it, a click anywhere else closing it, its entries picked as a card's are (see View::pickListValue()).
    const int editing = shapeEditing();
    const bool env = editing >= 0 && isOpEnvCurve(editing);
    if (editing < 0 || !(curveShape(editing) || env) || !host_.openListInEditor())
        return;
    const int param = env ? paramIndex(opOfEnvCurve(editing), OpParam::EnvSustain)
                          : paramIndex(editing, SourceParam::Free);
    if (host_.openList() != param)
        return;
    const WidgetAdder add{list};
    const Rect button = shapeClockList();
    const int items = env ? 2 : kCurveModes;
    add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
    add(Kind::ListMenu, listMenu(button, items), -1, 1);
    for (int item = 0; item < items; ++item)
        add(Kind::ListItem, listMenuItem(button, items, item), param, -1, item);
}

void ShapeEditor::cardWidgets(std::vector<Widget> &list, int lfo, float card) const {
    const WidgetAdder add{list};
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    const int s = lfo;
    // Its shape in the card: the title row's buttons, the reset and tools where a normal card's display is, Points |
    // Steps, Snap and the grid's steppers under the plot, and the plot with its points; greyed out, taking no input and
    // without its points, while the full-size editor has the shape, but for a click on the plot, which closes it.
    const bool elsewhere = shapeEditing() == s;
    const size_t editorStart = list.size();
    add(Kind::ShapeCollapse, lfoCardButton(card, 0), -1, s);
    add(Kind::ShapeExpand, lfoCardButton(card, 2), -1, s);
    add(Kind::ShapeReset, lfoResetButton(card), -1, s);
    for (int tool = 0; tool < kShapeTools; ++tool)
        add(Kind::ShapeTool, lfoToolButton(card, tool), -1, s, tool);
    for (int mode = 0; mode < 3; ++mode) {
        add(Kind::ShapeMode, lfoModeButton(card, mode), -1, s, mode);
        list.back().compact = true; // their icons alone, named in their tooltips
        if (mode == 1) // Points | Steps in one pill
            merge(list.size() - 2);
    }
    for (int axis = 0; axis < 2; ++axis)
        for (int part = 0; part < 3; ++part)
            add(Kind::ShapeGrid, lfoCardGridButton(card, axis, part), -1, axis, part - 1);
    add(Kind::ShapeArea, lfoCardPlot(card).inset(-8.0f), -1, s);
    if (loopShown(s)) // along the plot's top, under the points
        add(Kind::ShapeLoop, loopStrip(s), -1, s);
    for (size_t i = editorStart; i < list.size(); ++i)
        list[i].disabled = elsewhere;
    if (elsewhere)
        add(Kind::ShapeBack, lfoCardPlot(card).inset(-8.0f), -1, s);
    if (!elsewhere) {
        const int count = shapePointCount(s);
        for (int point = 0; point < count; ++point) {
            const double next = point + 1 < count ? shapePointX(s, point + 1) : 1.0;
            if (next - shapePointX(s, point) < 1e-6)
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(s, point);
            add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(s, point, PointField::Curve), s,
                point);
            list.back().compact = true;
        }
        for (int point = 0; point < count; ++point) {
            const Point c = shapePointPos(s, point);
            add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, s, point);
            list.back().compact = true;
        }
    }
}

// --- Painting.

void ShapeEditor::paintBack(Canvas &c) {
    if (shapeEditing() >= 0)
        paintShapeEditor(c, shapeEditing());
    else if (partialEditing() >= 0)
        paintPartialEditor(c, partialEditing());
}

const Widget *ShapeEditor::focused(DragKind dragged) const {
    const std::optional<Widget> &hover = host_.hovered();
    return drag_.kind == dragged ? &drag_.widget : (hover ? &*hover : nullptr);
}

namespace {
// An edit button, the same pencil wherever it opens an editor: an action's face at rest, lit in `color` under the
// mouse, and outlined in it while what it opens is open.
void editButton(Canvas &c, Rect r, Color color, bool hovered, bool open) {
    if (!open && !hovered)
        paintActionFace(c, r, false);
    else
        c.fillRect(r, open ? color.alpha(0.22f) : color.alpha(0.3f), kRadiusControl);
    if (open)
        c.strokeRect(r, color, 1.0f, kRadiusControl);
    editIcon(c, r.center(), open || hovered ? kText : kDim);
}
} // namespace

bool ShapeEditor::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::ShapeReset:
    case Kind::PartialReset:
        resetIcon(c, w.rect.center(), paintActionFace(c, w.rect, hovered));
        break;
    case Kind::PartialRandom:
        diceIcon(c, w.rect.center(), paintActionFace(c, w.rect, hovered));
        break;
    case Kind::ShapeGrid: {
        const UiLayout &layout = controller().layout();
        if (w.b == 0) { // the count
            c.text(w.rect, std::to_string(w.a == 0 ? layout.shapeGridX : layout.shapeGridY), kTextBody,
                   hovered ? kText : kDim, Align::Center, true);
            break;
        }
        const Color color = paintActionFace(c, w.rect, hovered);
        const Point m = w.rect.center();
        c.line({m.x - 3.5f, m.y}, {m.x + 3.5f, m.y}, color, 1.5f);
        if (w.b > 0)
            c.line({m.x, m.y - 3.5f}, {m.x, m.y + 3.5f}, color, 1.5f);
        break;
    }
    case Kind::ShapeArea: // the box dragged across it, picking points; the rest is drawn with its editor or card
        if (drag_.kind == DragKind::ShapeBox && drag_.widget.a == w.a) {
            const Rect box{std::min(drag_.start.x, drag_.now.x), std::min(drag_.start.y, drag_.now.y),
                           std::abs(drag_.now.x - drag_.start.x), std::abs(drag_.now.y - drag_.start.y)};
            c.pushClip(w.rect);
            c.fillRect(box, shapeColor(w.a).alpha(0.12f));
            c.strokeRect(box, shapeColor(w.a).alpha(0.7f), 1.0f);
            c.popClip();
        }
        break;
    case Kind::LfoDisplay:
        // A freeform LFO's display opens its editor.
        if (mods_.lfoShapeOf(w.a) == LfoShape::Custom)
            editButton(c, lfoEditButton(w.rect), kSourceColors[w.a], hovered, false);
        break;
    case Kind::PartialEdit: // a Custom wave's, lit while its partial editor is open
        editButton(c, w.rect, kAccent, hovered, partialEditing() == w.a);
        break;
    case Kind::ClipDisplay: // a drawn curve's edit button, beside its list; lit while its big editor is open
        editButton(c, w.rect, clipColor(w.a), hovered, shapeEditing() == kMaxSources + w.a);
        break;
    case Kind::OpEnvEdit: // an operator's drawn envelope's (its full-size editor, open, covers it)
        editButton(c, w.rect, kAccent, hovered, false);
        break;
    case Kind::ShapePoint: { // compact: smaller, in a shaper's card; picked: filled, and ringed
        const bool focused = hovered || active;
        const bool picked = shapePointPicked(w.a, w.b);
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 5.0f : 3.8f) : (focused ? 6.5f : 5.0f);
        c.fillCircle(w.rect.center(), radius, focused || picked ? color : kPanel);
        c.strokeCircle(w.rect.center(), radius, color, w.compact ? 1.5f : 2.0f);
        if (picked)
            c.strokeCircle(w.rect.center(), radius + (w.compact ? 2.5f : 3.0f), color.alpha(0.6f), 1.0f);
        break;
    }
    case Kind::ShapeCurve: {
        const bool focused = hovered || active;
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 3.8f : 2.4f) : (focused ? 4.5f : 3.0f);
        c.fillCircle(w.rect.center(), radius, focused ? kText : color.alpha(0.6f));
        break;
    }
    case Kind::PartialArea:
    case Kind::ShapeLoop:
    case Kind::ShapeBack:
        break; // drawn with its editor or card
    case Kind::ShapeDone:
        paintActionFace(c, w.rect, hovered);
        doneIcon(c, w.rect.center(), kText);
        break;
    case Kind::ShapeCollapse:
    case Kind::ShapeExpand:
    case Kind::ShapeWiden:
        if (const Color ink = paintActionFace(c, w.rect, hovered); w.kind == Kind::ShapeCollapse)
            collapseIcon(c, w.rect.center(), ink);
        else
            expandIcon(c, w.rect.center(), ink);
        break;
    case Kind::ShapeTool: { // Paste faint while nothing is copied
        const bool can = w.b != static_cast<int>(ShapeTool::Paste) || shapeCopied();
        shapeToolIcon(c, w.rect.center(), w.b, paintActionFace(c, w.rect, hovered, can));
        break;
    }
    case Kind::ShapeMode: { // Points | Steps, the one picked lit, and Snap lit while it is on
        const UiLayout &layout = controller().layout();
        const bool on = w.b == 2 ? layout.shapeSnap : layout.shapeSteps == (w.b == 1);
        const Color color = shapeColor(w.a);
        host_.paintButtonFace(c, w, on, hovered, color, 0.22f, kRadiusControl);
        const Color ink = on ? litInk(kText) : (hovered ? kText : kDim);
        const Str names[] = {Str::SwitchPoints, Str::SwitchSteps, Str::SwitchSnap};
        const char *name = tr(names[w.b]);
        const float textW = c.textWidth(name, kTextLabel, on);
        const float left = w.rect.center().x - (16.0f + textW) * 0.5f;
        // A widened card's are their icons alone, in their middles.
        const Point icon = w.compact ? w.rect.center() : Point{left + 6.0f, w.rect.center().y};
        if (w.b == 0)
            pointsIcon(c, icon, on ? litInk(color) : ink);
        else if (w.b == 1)
            stepsIcon(c, icon, on ? litInk(color) : ink);
        else
            gridIcon(c, icon, on ? litInk(color) : ink);
        if (!w.compact)
            c.text({left + 16.0f, w.rect.y, textW + 2.0f, w.rect.h}, name, kTextLabel, ink, Align::Left, on);
        break;
    }
    default:
        return false;
    }
    return true;
}

// Its buttons' and switches' tips, under them: the grid's steppers' (not its counts'), and none for its plots.
std::string ShapeEditor::tooltip(const Widget &w) const {
    switch (w.kind) {
    case Kind::ShapeReset:
        return tr(isNoteCurve(w.a)                         ? Str::TipResetNoteCurve
                  : isVelocityCurve(w.a)                   ? Str::TipResetVelocityCurve
                  : isClipCurve(w.a) || isShaperCurve(w.a) ? Str::TipResetCurve
                                                           : Str::TipResetShape);
    case Kind::PartialReset:
        return tr(Str::TipResetPartials);
    case Kind::PartialRandom:
        return tr(Str::TipRandomPartials);
    case Kind::PartialEdit:
        return tr(partialEditing() == w.a ? Str::TipClosePartials : Str::TipEditPartials);
    case Kind::ClipDisplay:
    case Kind::OpEnvEdit:
        return tr(Str::TipEditCurve);
    case Kind::ShapeDone:
        return tr(Str::TipDone);
    case Kind::ShapeCollapse:
        return tr(Str::TipShapeCollapse);
    case Kind::ShapeExpand:
        return tr(Str::TipShapeExpand);
    case Kind::ShapeWiden:
        return tr(Str::TipShapeWiden);
    case Kind::ShapeTool: {
        const Str tips[] = {Str::TipShapeInvert, Str::TipShapeReverse, Str::TipShapeShiftLeft,
                            Str::TipShapeShiftRight, Str::TipShapeCopy, Str::TipShapePaste};
        return tr(tips[std::clamp(w.b, 0, kShapeTools - 1)]);
    }
    case Kind::ShapeMode:
        return tr(w.b == 0 ? Str::TipShapePoints : w.b == 1 ? Str::TipShapeSteps : Str::TipShapeSnap);
    case Kind::ShapeGrid:
        return w.b != 0 ? tr(w.a == 0 ? Str::TipGridAcross : Str::TipGridUpDown) : std::string();
    default:
        return {};
    }
}

std::optional<CursorKind> ShapeEditor::cursor(const Widget &w) const {
    switch (w.kind) {
    case Kind::ShapeCurve:
        return CursorKind::Vertical;
    case Kind::ShapeArea:
        return CursorKind::Arrow;
    case Kind::ShapeLoop: { // moving over the loop, left and right over an end of it or a new one, as the SEQ view's
        const LoopGrab grab = drag_.kind == DragKind::Loop ? drag_.loopGrab : loopGrabAt(w.a, host_.mouse().x);
        return grab == LoopGrab::Move ? CursorKind::Move : CursorKind::Horizontal;
    }
    case Kind::ShapePoint:
        return CursorKind::Move;
    case Kind::PartialArea:
    case Kind::PartialReset:
    case Kind::PartialRandom:
    case Kind::PartialEdit:
    case Kind::ShapeDone:
    case Kind::ShapeReset:
    case Kind::ShapeGrid:
    case Kind::ShapeCollapse:
    case Kind::ShapeExpand:
    case Kind::ShapeBack:
    case Kind::ShapeWiden:
    case Kind::ShapeTool:
    case Kind::ShapeMode:
    case Kind::LfoDisplay:
    case Kind::ClipDisplay:
    case Kind::OpEnvEdit:
        return CursorKind::Hand;
    default:
        return std::nullopt;
    }
}

// --- Input.

// The points picked in a shape editor are let go of at a click anywhere but on that shape.
void ShapeEditor::pressing(const Widget *hit) {
    if (shapePick_.shape >= 0 && !(hit && (hit->kind == Kind::ShapeArea || hit->kind == Kind::ShapePoint ||
                                           hit->kind == Kind::ShapeCurve) &&
                                   hit->a == shapePick_.shape))
        clearShapePick();
}

void ShapeEditor::startDrag(DragKind kind, const Widget &w, Point p, bool step) {
    drag_ = Drag{};
    drag_.kind = kind;
    drag_.widget = w;
    drag_.start = drag_.now = p;
    host_.startDrag(*this, w, p, step);
}

bool ShapeEditor::press(const Widget &w, Point p, Modifiers mods) {
    // A symmetric curve's left half only mirrors its right one: a click there does nothing.
    if (w.kind == Kind::ShapeArea && inMirroredHalf(w.a, p))
        return true;
    if (drawsStep(w, mods)) {
        // Held, the step follows the mouse, up and down and from column to column, as one undo step.
        const int shape = w.a;
        clearShapePick(); // the points it takes the place of may have been picked
        std::vector<double> before;
        for (int f = 0; f < shapeParamCount(shape); ++f)
            before.push_back(value(shapeBase(shape) + f));
        controller().beginStep();
        const bool snap = shapeSnaps(shape, mods.ctrl);
        if (!addShapeStep(shape, p, snap, before)) {
            controller().endStep();
            host_.flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
            return true;
        }
        startDrag(DragKind::ShapeStep, w, p, true);
        drag_.shapeBefore = std::move(before);
        return true;
    }
    UiLayout &layout = controller().layout();
    switch (w.kind) {
    case Kind::ShapeWiden:
    case Kind::LfoDisplay: // a Custom LFO's widens its card, which then has the shape (and the full-size editor not)
        if (mods_.lfoShapeOf(w.a) == LfoShape::Custom) {
            widen(w.a);
            host_.revealCard(w.a);
            host_.resetHover();
        }
        return true;
    case Kind::ShapeDone: // an LFO's full-size editor goes back into its widened card
        if (const int shape = shapeEditing(); shape >= 0 && !isCurve(shape)) {
            wideLfo_ = shape;
            host_.revealCard(shape);
        }
        shapeEditor_ = -1;
        partialEditor_ = -1;
        return true;
    case Kind::ShapeCollapse:
        wideLfo_ = -1;
        host_.resetHover();
        return true;
    case Kind::ShapeExpand:
        shapeEditor_ = w.a;
        partialEditor_ = -1;
        host_.resetHover();
        return true;
    case Kind::ShapeBack: // the card's plot, dimmed: the shape back into the card, as the editor's done button does
        shapeEditor_ = -1;
        host_.resetHover();
        return true;
    case Kind::ShapeTool:
        applyShapeTool(w.a, static_cast<ShapeTool>(w.b));
        return true;
    case Kind::ShapeMode:
        if (w.b == 2)
            layout.shapeSnap = !layout.shapeSnap;
        else
            layout.shapeSteps = w.b == 1;
        controller().layoutChanged();
        return true;
    case Kind::ClipDisplay:
        shapeEditor_ = kMaxSources + w.a;
        return true;
    case Kind::OpEnvEdit: // over the routing panel and the inspector, as a Curve's, the pencil under it
        shapeEditor_ = opEnvCurve(w.a);
        partialEditor_ = -1;
        host_.resetHover();
        return true;
    case Kind::PartialEdit: // a second click closes it
        partialEditor_ = partialEditing() == w.a ? -1 : w.a;
        shapeEditor_ = -1;
        return true;
    case Kind::ShapeArea: { // a box across it picks the points it reaches, with Shift as well as those picked already
        std::vector<int> base = mods.shift ? shapePicked(w.a) : std::vector<int>{};
        startDrag(DragKind::ShapeBox, w, p, false);
        drag_.boxBase = std::move(base);
        setShapePick(w.a, drag_.boxBase);
        return true;
    }
    case Kind::ShapePoint: {
        // A point not picked is picked alone, or with Shift along with the others; one picked already moves with the
        // others, and let go of unmoved is picked alone, or with Shift let go.
        std::vector<int> picked = shapePicked(w.a);
        const bool was = std::find(picked.begin(), picked.end(), w.b) != picked.end();
        bool unpick = false, narrow = false;
        if (mods.shift && was)
            unpick = true;
        else if (mods.shift)
            picked.push_back(w.b);
        else if (!was)
            picked = {w.b};
        else
            narrow = picked.size() > 1;
        setShapePick(w.a, picked);
        picked = shapePicked(w.a);
        if (picked.size() > 1) { // together, one undo step
            controller().beginStep();
            std::vector<std::pair<double, double>> origin;
            for (int point : picked) {
                for (PointField field : {PointField::X, PointField::Y})
                    controller().beginEdit(shapeIndex(w.a, point, field));
                origin.push_back({shapePointX(w.a, point), value(shapeIndex(w.a, point, PointField::Y))});
            }
            startDrag(DragKind::ShapePoint, w, p, true);
            drag_.origin = std::move(origin);
            drag_.points = std::move(picked);
        } else {
            for (PointField field : {PointField::X, PointField::Y})
                controller().beginEdit(shapeIndex(w.a, w.b, field));
            startDrag(DragKind::ShapePoint, w, p, false);
        }
        drag_.unpick = unpick;
        drag_.narrow = narrow;
        return true;
    }
    case Kind::ShapeCurve:
        startDrag(DragKind::ShapeCurve, w, p, false);
        drag_.startValue = value(w.param);
        controller().beginEdit(w.param);
        return true;
    case Kind::ShapeLoop: // an end of the loop, the whole of it, or a new loop across the columns outside it
        controller().beginStep(); // one undo step, however far it goes
        startDrag(DragKind::Loop, w, p, true);
        drag_.loopGrab = loopGrabAt(w.a, p.x);
        drag_.loopFrom = loopColumn(w.a, p);
        drag_.loopWas = loopOf(w.a);
        return true;
    case Kind::PartialArea:
        controller().beginStep(); // one undo step for the whole stroke
        startDrag(DragKind::Partials, w, p, true);
        moveTo(p, mods.ctrl);
        return true;
    case Kind::PartialReset: {
        std::vector<std::pair<int, double>> changes;
        for (int k = 0; k < kPartials; ++k) {
            changes.push_back({partialLevelIndex(w.a, k), table_[partialLevelIndex(w.a, k)].def});
            changes.push_back({partialPhaseIndex(w.a, k), table_[partialPhaseIndex(w.a, k)].def});
        }
        editMany(changes);
        return true;
    }
    case Kind::PartialRandom: {
        // Every partial, the hidden ones too: a random level under a pink slope (-3 dB an octave, the level of
        // partial k at most 1 / sqrt(k)) and a random phase. One undo step.
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<std::pair<int, double>> changes;
        for (int k = 0; k < kPartials; ++k) {
            const double level = unit(random_) / std::sqrt(static_cast<double>(k + 1));
            changes.push_back({partialLevelIndex(w.a, k), level});
            changes.push_back({partialPhaseIndex(w.a, k), unit(random_)});
        }
        editMany(changes);
        return true;
    }
    case Kind::ShapeReset:
        resetShape(w.a);
        return true;
    case Kind::ShapeGrid:
        if (w.b != 0)
            stepShapeGrid(w.a, w.b);
        return true;
    default:
        return false;
    }
}

bool ShapeEditor::doubleClick(const Widget &w, Point p, Modifiers mods) {
    if (drawsStep(w, mods)) // a press again (see View::doubleClick())
        return false;
    switch (w.kind) {
    case Kind::PartialArea: { // that bar to nothing
        const int k = partialAt(w.b == 0 ? kPartialLevels : kPartialPhases, p.x, partialsShown(w.a));
        mods_.edit(w.b == 0 ? partialLevelIndex(w.a, k) : partialPhaseIndex(w.a, k), 0.0);
        return true;
    }
    case Kind::ShapeArea:
        if (!inMirroredHalf(w.a, p)) // the mirrored half takes no points
            addShapePoint(w.a, p);
        return true;
    case Kind::ShapePoint:
        removeShapePoint(w.a, w.b);
        return true;
    case Kind::ShapeCurve:
        mods_.edit(w.param, 0.0); // straight again: 0 %, whatever the segment started as
        return true;
    case Kind::ShapeLoop: { // the loop over the whole curve again
        const auto [start, end] = loopParams(w.a);
        editMany({{start, 0.0}, {end, 1.0}});
        return true;
    }
    default:
        return false;
    }
}

// The grid's steppers and the tools, clicked quickly again, step and act again.
bool ShapeEditor::doublePress(const Widget &w) const {
    return (w.kind == Kind::ShapeGrid && w.b != 0) || w.kind == Kind::ShapeTool;
}

void ShapeEditor::set(int index, double v) { // within its range, as the View sets a value dragged
    v = std::clamp(v, table_[index].min, table_[index].max);
    if (v != value(index))
        controller().setValue(index, v);
}

void ShapeEditor::drag(const DragState &d) {
    drag_.now = d.now;
    drag_.loopFree = d.mods.shift || d.mods.ctrl; // a loop's drag, as the mouse moves (see moveTo())
    moveTo(d.now, d.mods.ctrl);
}

void ShapeEditor::moveTo(Point p, bool ctrl) {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::ShapePoint: {
        // Points keep their order: each stays between its neighbors. The first one stays at the
        // start, and a curve's last one at the end. An LFO's may reach a neighbor's time, a line straight up or
        // down (see shapeValue()); a curve's keep a little apart.
        const int shape = w.a;
        const int point = w.b;
        double x = shapeXAt(shape, p);
        double y = shapeYAt(shape, p);
        if (shapeSnaps(shape, ctrl)) { // to the grid's lines
            const UiLayout &layout = controller().layout();
            x = std::round(x * layout.shapeGridX) / layout.shapeGridX;
            const double half = 0.5 * layout.shapeGridY; // lines per unit of height
            y = std::round((y + 1.0) * half) / half - 1.0;
        }
        if (!drag_.points.empty()) { // the points picked, after it
            moveShapePoints(shape, point, x, y);
            break;
        }
        const int count = shapePointCount(shape);
        // Symmetric, the middle point stays in the middle, and the others right of it.
        if (point > 0 && !(isCurve(shape) && point + 1 == count) && !middlePoint(shape, point)) {
            const double low = std::max(shapePointX(shape, point - 1), curveSymmetric(shape) ? 0.5 : 0.0);
            const double high = point + 1 < count ? shapePointX(shape, point + 1) : 1.0;
            const double apart = isCurve(shape) ? 0.002 : 0.0;
            set(shapeIndex(shape, point, PointField::X), std::max(low + apart, std::min(x, high - apart)));
        }
        set(shapeIndex(shape, point, PointField::Y), y);
        break;
    }
    case DragKind::ShapeStep: // drawn again into the shape as it was, where the mouse is; where it doesn't fit, it stays
        if (!inMirroredHalf(w.a, p)) // held over the mirrored half, the step stays where it was
            addShapeStep(w.a, p, shapeSnaps(w.a, ctrl), drag_.shapeBefore);
        break;
    case DragKind::ShapeBox: { // the points shown inside it, and those picked before it
        const Rect box{std::min(drag_.start.x, p.x), std::min(drag_.start.y, p.y), std::abs(p.x - drag_.start.x),
                       std::abs(p.y - drag_.start.y)};
        std::vector<int> picked = drag_.boxBase;
        for (int point = 0; point < shapePointCount(w.a); ++point)
            if (pointShown(w.a, point) && box.contains(shapePointPos(w.a, point)))
                picked.push_back(point);
        setShapePick(w.a, std::move(picked));
        break;
    }
    case DragKind::Loop: {
        // As the SEQ view's loop, on the grid's columns: a new one at least a column long, moved as long as it was and
        // within the curve, or an end, a column at least from the other. With Shift or Ctrl held, off them, where the
        // mouse is, its ends still kLoopApart apart. One undo step (see press()).
        const int shape = w.a;
        const bool free = drag_.loopFree;
        const double column = 1.0 / controller().layout().shapeGridX;
        const double apart = free ? kLoopApart : column;
        auto along = [&](Point at) { return std::clamp(shapeXAt(shape, at), 0.0, 1.0); };
        const double at = free ? along(p) : loopColumn(shape, p);
        const double from = free ? along(drag_.start) : drag_.loopFrom;
        LfoLoop loop = drag_.loopWas;
        switch (drag_.loopGrab) {
        case LoopGrab::New:
            loop = {std::min(from, at), std::max(from, at)};
            if (loop.end - loop.start < apart - 1e-9)
                return;
            break;
        case LoopGrab::Move: {
            const double length = drag_.loopWas.end - drag_.loopWas.start;
            const double moved = shapeXAt(shape, p) - shapeXAt(shape, drag_.start);
            const double by = free ? moved : std::round(moved / column) * column;
            loop.start = std::clamp(drag_.loopWas.start + by, 0.0, 1.0 - length);
            loop.end = loop.start + length;
            break;
        }
        case LoopGrab::Start:
            loop.start = std::max(std::min(at, loop.end - apart), 0.0);
            break;
        case LoopGrab::End:
            loop.end = std::min(std::max(at, loop.start + apart), 1.0);
            break;
        }
        const auto [start, end] = loopParams(shape);
        if (loop.start != value(start) || loop.end != value(end))
            editMany({{start, loop.start}, {end, loop.end}});
        break;
    }
    case DragKind::Partials: {
        // A quick sweep sets every bar it passed, along a line (in bar heights) from where the last move was.
        const int op = w.a;
        const Rect area = w.b == 0 ? kPartialLevels : kPartialPhases;
        const int k = partialAt(area, p.x, partialsShown(op));
        const double v = std::clamp((area.bottom() - p.y) / area.h, 0.0f, 1.0f);
        const int from = drag_.lastPartial < 0 ? k : drag_.lastPartial;
        const double fromValue = drag_.lastPartial < 0 ? v : drag_.startValue;
        for (int j = std::min(from, k); j <= std::max(from, k); ++j) {
            const double t = from == k ? 1.0 : static_cast<double>(j - from) / (k - from);
            const double bar = fromValue + (v - fromValue) * t;
            if (w.b == 0)
                set(partialLevelIndex(op, j), partialLevelFromBar(bar));
            else
                set(partialPhaseIndex(op, j), bar);
        }
        drag_.lastPartial = k;
        drag_.startValue = v;
        break;
    }
    case DragKind::ShapeCurve: {
        // Dragging up lifts the middle of the segment, whichever way it runs.
        const int shape = w.a;
        const int next = w.b + 1 == shapePointCount(shape) ? 0 : w.b + 1;
        const double rise = value(shapeIndex(shape, next, PointField::Y)) - value(shapeIndex(shape, w.b, PointField::Y));
        set(w.param, drag_.startValue - (rise < 0.0 ? -1.0 : 1.0) * (drag_.start.y - p.y) / 100.0);
        break;
    }
    case DragKind::None:
        break;
    }
}

void ShapeEditor::drop(const DragState &d) {
    const Widget &w = drag_.widget;
    // A point clicked, not moved: with Shift, one picked already let go; without, one of several picked picked alone.
    if (drag_.kind == DragKind::ShapePoint && !d.moved && (drag_.unpick || drag_.narrow)) {
        std::vector<int> picked = shapePicked(w.a);
        if (drag_.unpick)
            std::erase(picked, w.b);
        else
            picked = {w.b};
        setShapePick(w.a, std::move(picked));
    }
}

// The drag's gestures end; its undo step, if it opened one, closes after (see PanelHost::startDrag()).
void ShapeEditor::release() {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::ShapeCurve:
        controller().endEdit(w.param);
        break;
    case DragKind::ShapePoint:
        if (drag_.points.empty())
            drag_.points = {w.b};
        for (int point : drag_.points)
            for (PointField field : {PointField::Y, PointField::X})
                controller().endEdit(shapeIndex(w.a, point, field));
        break;
    case DragKind::Partials:
    case DragKind::ShapeStep:
    case DragKind::ShapeBox:
    case DragKind::Loop:
    case DragKind::None:
        break;
    }
    drag_ = Drag{};
}

bool ShapeEditor::dragsOnTouch(const Widget &w) const {
    return w.kind == Kind::ShapeArea || w.kind == Kind::ShapePoint || w.kind == Kind::ShapeCurve ||
           w.kind == Kind::PartialArea || w.kind == Kind::ShapeLoop || drawsStep(w, {});
}

// A Curve's point's Set as Loop Start and Set as Loop End, where that end would stay on its side of the other; a
// point's Delete Point, while it can go; and a bend's Straighten.
bool ShapeEditor::contextMenu(const std::optional<Widget> &hit, Point, std::vector<MenuItem> &items) {
    menuPoint_ = {-1, -1};
    menuLoop_ = {-1, 0.0};
    menuCurve_ = -1;
    menuShape_ = -1;
    // Copy Shape, and Paste Shape while one is copied, after what the point or bend offers.
    auto shapeItems = [&] {
        menuShape_ = hit->a;
        if (!items.empty())
            items.push_back({0, {}});
        items.push_back({kMenuCopyShape, tr(Str::MenuCopyShape)});
        if (shapeCopied())
            items.push_back({kMenuPasteShape, tr(Str::MenuPasteShape)});
        return true;
    };
    if (hit && (hit->kind == Kind::ShapeArea || hit->kind == Kind::ShapeLoop))
        return shapeItems();
    if (hit && hit->kind == Kind::ShapePoint) {
        if (loopedShape(hit->a)) {
            const double x = shapePointX(hit->a, hit->b);
            const LfoLoop loop = loopOf(hit->a);
            menuLoop_ = {hit->a, x};
            if (x <= loop.end - kLoopApart) // the ends kept as far apart as a drag keeps them
                items.push_back({kMenuSetLoopStart, tr(Str::MenuSetLoopStart)});
            if (x >= loop.start + kLoopApart)
                items.push_back({kMenuSetLoopEnd, tr(Str::MenuSetLoopEnd)});
        }
        const bool end = isCurve(hit->a) && hit->b + 1 == shapePointCount(hit->a);
        if (hit->b > 0 && !end && shapePointCount(hit->a) > 2) {
            menuPoint_ = {hit->a, hit->b};
            if (!items.empty())
                items.push_back({0, {}});
            items.push_back({kMenuDeletePoint, tr(Str::MenuDeletePoint)});
        }
        return shapeItems();
    }
    if (hit && hit->kind == Kind::ShapeCurve) {
        menuCurve_ = hit->param;
        items.push_back({kMenuStraighten, tr(Str::MenuStraighten)});
        return shapeItems();
    }
    return false;
}

bool ShapeEditor::menuCommand(int command) {
    if ((command == kMenuCopyShape || command == kMenuPasteShape) && menuShape_ >= 0) {
        if (command == kMenuCopyShape)
            copyShape(menuShape_);
        else
            pasteShape(menuShape_);
        return true;
    }
    if ((command == kMenuSetLoopStart || command == kMenuSetLoopEnd) && menuLoop_.first >= 0) { // exactly at the point
        // Both ends written, in order: a state may hold them the other way round, which loopOf() reads in order.
        LfoLoop loop = loopOf(menuLoop_.first);
        (command == kMenuSetLoopStart ? loop.start : loop.end) = menuLoop_.second;
        if (loop.end - loop.start < kLoopApart) // the point moved since the menu opened
            return true;
        const auto [start, end] = loopParams(menuLoop_.first);
        editMany({{start, loop.start}, {end, loop.end}});
        return true;
    }
    if (command == kMenuDeletePoint && menuPoint_.first >= 0) {
        removeShapePoint(menuPoint_.first, menuPoint_.second);
        return true;
    }
    if (command == kMenuStraighten && menuCurve_ >= 0) {
        mods_.edit(menuCurve_, mods_.resetValue(menuCurve_));
        return true;
    }
    return false;
}

} // namespace fmvibe::gui
