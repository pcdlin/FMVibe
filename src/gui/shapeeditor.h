#pragma once

// The drawn shapes' editors (see shapeeditor.cpp): a Curve source's shape, a shaper's drawn curve (a shaper effect's
// or a voice shaper's), the note source's curve and velocity's, each edited in place (a Curve source's in its widened
// card, a shaper effect's in its card's display, a voice shaper's on its page in the inspector) or in the full-size
// editor over the panels' row; and an operator's Custom wave's partials, in their full-size editor there. The
// ShapeEditor keeps which are open, the points picked, its drags and the widened card, lays out, paints and edits the
// full-size editors as a panel of the editor's (their widgets listed where the routing panel's would be), and owns
// every shape editor's widgets wherever they are listed: what lays out a card or page that holds a shape lists them
// through drawnWidgets() and cardWidgets() and paints them through paintShaperCurve() and paintLfoCard(), and says
// where they show through PanelHost::shapeHome(). What a full-size editor covers the others ask of it (shapeEditing(),
// partialEditing(), shapePanel()), as the panels do through the host.

#include "fxgraphmodel.h"
#include "modulationmodel.h"
#include "panel.h"

#include <array>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

// Drawn shapes, by number: a freeform LFO's (shape = the source), a shaper's curve (shape = kMaxSources + the shaper's
// unit: an effect slot, or kFxSlots + a shaper), the note source's curve (shape = kNoteCurve) or velocity's
// (kVelocityCurve). Shaper units (see shaperParams()): the shaper effects, by slot, then the voices' shapers, kFxSlots +
// the shaper.
constexpr int kNoteCurve = kMaxSources + kShaperUnits;
constexpr int kVelocityCurve = kNoteCurve + 1;
inline bool isCurve(int shape) { return shape >= kMaxSources; } // a shaper's, the note's or velocity's: no wrap
inline bool isClipCurve(int shape) { return shape >= kMaxSources && shape < kMaxSources + kFxSlots; } // an effect's
inline bool isShaperCurve(int shape) { return shape >= kMaxSources + kFxSlots && shape < kNoteCurve; }
inline bool isNoteCurve(int shape) { return shape == kNoteCurve; }
inline bool isVelocityCurve(int shape) { return shape == kVelocityCurve; }
// The curves of the sources in the keyboard strip, which open from their displays there.
inline bool isStripCurve(int shape) { return isNoteCurve(shape) || isVelocityCurve(shape); }
// The curve of the note source or velocity, and back.
inline int curveOf(int source) { return isVelocity(source) ? kVelocityCurve : kNoteCurve; }
inline int sourceOfCurve(int shape) { return isVelocityCurve(shape) ? kVelocitySource : kNoteSource; }
inline int shaperCurve(int shaper) { return kMaxSources + kFxSlots + shaper; }
int shapeBase(int shape);
// How loud what goes into shaper `unit` is as of now, the snapshot `t` fallen by its age: where the dot on its curve
// stands. And whether that dot has moved between two snapshots, or come or gone.
float shaperLevelNow(const Telemetry &t, int unit);
bool shaperDotMoved(const Telemetry &now, const Telemetry &was, int unit);
// How many points it can have, and how many params it takes from shapeBase() on.
inline int shapeMaxPoints(int shape) { return isStripCurve(shape) ? kMaxNotePoints : kMaxShapePoints; }
inline int shapeParamCount(int shape) { return isStripCurve(shape) ? kNoteCurveParamCount : kShapeParamCount; }

class ShapeEditor : public Panel {
public:
    explicit ShapeEditor(PanelHost &host);

    // What a widened LFO card's tools do to its whole shape, each one undo step: flip it upside down, play it back to
    // front, move it a grid column earlier or later (round the cycle), and copy it to paste into another LFO.
    enum class ShapeTool { Invert, Reverse, ShiftLeft, ShiftRight, Copy, Paste };
    // Its context menu's commands (see contextMenu()): a bend's Straighten and a point's Delete Point, as the View
    // numbered them; taken only while its own menu is open, so they may share their numbers with the View's.
    static constexpr int kMenuStraighten = 1;
    static constexpr int kMenuDeletePoint = 5;

    // The big editor shows over the routing panel and the inspector while the LFO's shape is Custom, over the routing
    // panel while the note source's or velocity's curve or a voice shaper's is open, or over the effects for a shaper
    // effect's Custom curve; a shaper effect's card, or a voice shaper's page in the inspector, lets you edit its curve
    // too. A curve's points run from the left edge to the right one; an LFO's wrap around. The shape it shows, or -1.
    int shapeEditing() const;
    // A Custom LFO's card widened to edit its shape in it (see kWideSource), or -1: a click on its display widens it,
    // and it stays so while it is Custom and not folded, also while the full-size editor has its shape.
    int wideLfo() const;
    // The partial editor: an operator's Custom wave, over the routing panel while the shape editor isn't; or -1.
    int partialEditing() const;
    // The panel the big editor fills.
    Rect shapePanel() const;
    // Opening and closing them. A shape opened (an effect's or a voice shaper's edit button, the note source's display),
    // or toggled: open, or closed if it is the one asked for already. Both full-size editors closed (another view
    // shown), the partials of `op` opened in their place, or closed unless they are `op`'s (another node shown).
    void openShape(int shape) { shapeEditor_ = shape; }
    void toggleShape(int shape) { shapeEditor_ = shapeEditor_ == shape ? -1 : shape; }
    void close();
    void openPartials(int op);
    void closePartialsBut(int op);
    // An LFO's card widened (an LFO's full-size editor giving its shape back to it), and back to its size (its shape no
    // longer Custom, or its card folded).
    void widen(int lfo);
    void narrow(int lfo);
    // A drawn shape whose place is taken by something else, or emptied (a source's place, an effect's slot): the
    // full-size editor showing it, if it does, closes, and so does its widened card.
    void shapeGone(int shape);

    // Lets go of the points picked, as what a panel picks takes over from them.
    void clearShapePick() { shapePick_ = ShapePick{}; }
    // Flips a shaper's Symmetric switch (`param`: an effect's or a voice shaper's), as a click on it does.
    void switchSymmetric(int param);
    // The next grid the shape editor offers along `axis`, `steps` away.
    void stepShapeGrid(int axis, int steps);
    // Whether a click on `w` draws a step rather than working with points (Shift doing the other).
    bool drawsStep(const Widget &w, Modifiers mods) const;
    // Onto a PointCache key: a drawn shape's point count (its param `base`) and its points, as many as it has.
    void keyShape(std::vector<double> &key, int base) const;

    // What shows a shape in place lists and paints it through these. A shaper's drawn curve in `display` (an effect's
    // card's, or a voice shaper's page's): its plot, its bends and its points. A widened LFO card at screen place `card`:
    // its title row's buttons, its tools and modes, its grid's steppers and its plot with its points.
    void drawnWidgets(std::vector<Widget> &list, int unit, Rect display) const;
    void cardWidgets(std::vector<Widget> &list, int lfo, float card) const;
    // A shaper's transfer curve, or its drawn curve to edit: an effect slot's or (unit kFxSlots + shaper) a voice
    // shaper's.
    void paintShaperCurve(Canvas &c, Rect r, int unit);
    // A widened LFO card's shape, at screen place `card`.
    void paintLfoCard(Canvas &c, float card, int lfo);

    void forgetScoped() override;
    void widgets(std::vector<Widget> &list) const override; // a full-size editor's, while one is open
    void paintBack(Canvas &c) override;                      // a full-size editor, while one is open
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    void pressing(const Widget *hit) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doubleClick(const Widget &w, Point p, Modifiers mods) override;
    bool doublePress(const Widget &w) const override;
    void drag(const DragState &d) override;
    void drop(const DragState &d) override;
    void release() override;
    bool dragsOnTouch(const Widget &w) const override;
    bool key(Key key, bool shift) override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;

private:
    using Kind = WidgetKind;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return mods_.value(index); }
    void editMany(const std::vector<std::pair<int, double>> &changes) { mods_.editMany(changes); }
    // The full-size editors: whether either shows now.
    bool open() const { return shapeEditing() >= 0 || partialEditing() >= 0; }
    // How many partials an operator's Custom wave plays, and the editor shows; whether its wave is drawn from partials.
    int partialsShown(int op) const;
    bool isCustomWave(int op) const;
    // Whether a click on a shape draws a step rather than working with points (Shift doing the other), and whether a
    // dragged point snaps to the grid (Ctrl doing the other; the switch only where the grid shows: the big editor and
    // a widened LFO card).
    bool shapeSteps(int shape) const;
    bool shapeSnaps(int shape, bool ctrl) const;
    // The big editor's Points | Steps and Snap: left of its grid's controls under the plot, or in a shaper effect's
    // narrower editor in its title row.
    Rect shapeModeButton(int mode) const;
    // A widened card's tools: what they make of the shape, and the shape copied, which Paste puts into another LFO.
    void applyShapeTool(int lfo, ShapeTool tool);
    struct DrawnPoint {
        double x, y, curve; // the curve bends the segment leaving it
    };
    std::vector<DrawnPoint> drawnPoints(int shape) const;
    // The edits that make the shape these points (the first at the start), or nothing when they don't fit in it.
    std::optional<std::vector<std::pair<int, double>>> drawnChanges(int shape,
                                                                    const std::vector<DrawnPoint> &points) const;
    // How to edit a shape in the big editor or a widened LFO card, as Points | Steps and Snap have it.
    std::string shapeHint() const;
    // Whether a widened card's Copy has kept a shape for Paste.
    static bool shapeCopied();
    // Its geometry: its buttons, and where a shape is edited (the big editor's plot, or where it shows in place while
    // the big editor doesn't have it: see PanelHost::shapeHome()) and where a value sits there; without a shape, the big
    // editor's.
    Rect shapeArea(int shape = -1) const;
    Rect shapeAreaNow(int shape) const; // shapeArea() worked out
    Rect shapeDone() const;
    Rect shapeReset() const;
    Rect partialRandom() const; // the partial editor's dice, left of its reset button
    Rect shapeGridButton(int axis, int part) const;
    Point shapeAt(double x, double y) const { return shapeAt(-1, x, y); }
    Point shapeAt(int shape, double x, double y) const;
    double shapeXAt(int shape, Point p) const;
    double shapeYAt(int shape, Point p) const;
    // The color of a shaper unit: its effect's, or the voice shapers'.
    Color clipColor(int unit) const;
    Color shapeColor(int shape) const;
    int shapeIndex(int shape, int point, PointField field) const;
    int shapePointCount(int shape) const;
    double shapePointX(int shape, int point) const;
    Point shapePointPos(int shape, int point) const;
    Point shapeCurvePos(int shape, int point) const;
    void addShapePoint(int shape, Point p);
    // The changes that put a new point at (x, y) among the others, the later ones moving up a place.
    std::vector<std::pair<int, double>> insertShapePoint(int shape, double x, double y) const;
    // A symmetric shaper's curve (see ClipCurve): only its right half is edited, the left drawn as its mirror. Its
    // points left of the middle are hidden, and so are the bends that start there; a click on that half does nothing.
    // Switched on in the editor, the curve gets a point in the middle, which then stays there.
    bool curveSymmetric(int shape) const;
    bool pointShown(int shape, int point) const;
    bool middlePoint(int shape, int point) const;
    bool inMirroredHalf(int shape, Point p) const;
    // The big editor's symmetric switch, for a shaper's Custom curve: left of its Points | Steps.
    Rect shapeSymButton() const;
    // Draws a step into the shape as `before` holds it (its params from shapeBase() on), or as it is when that is
    // empty. False when there's no room for it.
    bool addShapeStep(int shape, Point p, bool snap, const std::vector<double> &before = {});
    void removeShapePoint(int shape, int point);
    void resetShape(int shape);
    // The points picked in a shape editor, which a drag on one of them moves together and Delete removes: picked by a
    // box dragged across the shape from empty space, Shift+click adding a point or taking it away, Ctrl+A picking every
    // one. Not saved; the editor's alone. A click anywhere else lets them go, and so does any change of how many
    // points the shape has.
    struct ShapePick {
        int shape = -1;
        int count = 0; // how many points the shape had, which the points picked are of
        std::vector<int> points; // in order
    };
    ShapePick shapePick_;
    // The points picked of `shape` that it still has and shows (a symmetric curve's mirrored half shows none).
    std::vector<int> shapePicked(int shape) const;
    bool shapePointPicked(int shape, int point) const;
    void setShapePick(int shape, std::vector<int> points);
    // Whether `shape` is shown where it can be edited: the big editor, a widened card, a shaper's card or page.
    bool shapeEditable(int shape) const;
    // Whether a point stays where it is across: the first, a curve's last, a symmetric curve's middle.
    bool shapePointPinned(int shape, int point) const;
    // The points picked, moved together after the one grabbed, now at `x`, `y` (see drag()).
    void moveShapePoints(int shape, int grabbed, double x, double y);
    // Removes those of `points` that can go, as many as leave the shape two, as one undo step.
    void removeShapePoints(int shape, const std::vector<int> &points);

    // Painting the full-size editors and what goes with them.
    void paintShapeEditor(Canvas &c, int shape);
    void paintClipCurveEditor(Canvas &c, int shape);
    void paintNoteCurveEditor(Canvas &c);
    void paintVelocityCurveEditor(Canvas &c);
    // The editor's grid; `zero` is where the zero line goes, and a centered grid stresses its middle line.
    void paintShapeGrid(Canvas &c, Rect area, double zero, bool centered);
    void paintShapeGridControls(Canvas &c);
    void paintPartialEditor(Canvas &c, int op);
    void paintClipCurveCard(Canvas &c, Rect r, int unit);
    void paintMirroredPoints(Canvas &c, int shape, float radius);

    // A drag of its own (see PanelHost::startDrag()): what it holds, the widget pressed, where the mouse is now, and
    // what each kind keeps.
    enum class DragKind {
        None,
        ShapePoint,
        ShapeCurve,
        ShapeStep, // a step just drawn, following the mouse (shapeBefore: the shape before it)
        ShapeBox,  // a box across a shape from where it was pressed to where the mouse is, picking the points it reaches
        Partials,
    };
    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start, now;
        double startValue = 0.0;
        int lastPartial = -1; // drawing partials: the one the last move was over, and its value in startValue
        std::vector<double> shapeBefore; // drawing a step: the shape's params before it
        // Points moved together (DragKind::ShapePoint with more than one picked): which, and where each was as they
        // were grabbed (x, y). Pressed with Shift on one picked already, `unpick` lets it go if it is let go unmoved;
        // pressed without on one of several picked, `narrow` picks it alone then. A box (DragKind::ShapeBox): the
        // points picked before it, which Shift adds it to.
        std::vector<int> points;
        std::vector<std::pair<double, double>> origin;
        bool unpick = false;
        bool narrow = false;
        std::vector<int> boxBase;
    };
    // The widget a readout names: the point (or partials) being dragged, else the one under the mouse.
    const Widget *focused(DragKind dragged) const;
    // Starts a drag of `kind` on `w`, pressed at `p` (with `step`, its undo step open); and what it does with the mouse
    // at `p`, Ctrl held or not.
    void startDrag(DragKind kind, const Widget &w, Point p, bool step);
    void moveTo(Point p, bool ctrl);
    // A value set as a drag sets it: within its range, and only when it changes.
    void set(int index, double v);

    PanelHost &host_;
    // The patch's modulation and effects, their models telling the editor why they refused an edit (see
    // PanelHost::flash()).
    ModulationModel &mods_;
    FxGraphModel &fx_;
    const ParamTable &table_;
    Drag drag_;
    int shapeEditor_ = -1;                   // the shape the full-size editor shows, or -1
    int wideLfo_ = -1;                       // the LFO whose card is widened (see wideLfo()), or -1
    int partialEditor_ = -1;                 // the operator whose Custom wave's partials it shows, or -1
    std::mt19937 random_{std::random_device{}()}; // the partial editor's dice
    std::pair<int, int> menuPoint_{-1, -1}; // shape and point of the open context menu's Delete Point
    int menuCurve_ = -1;                     // the bend the open context menu straightens, or -1
    // Where each shape is edited, kept while the host's scope goes on: by shape + 1, to kVelocityCurve.
    std::array<ScopeCache<Rect>, kMaxSources + kShaperUnits + 3> shapeAreas_{};
};

} // namespace fmvibe::gui
