#pragma once

// The SEQ view (see seqpanel.cpp): the sequencer's settings on a card in the inspector's place, and the roll of the
// pattern picked beside it, with its tools, its ruler and loop, its keys and a lane of one value a note (or the
// pattern's pitch bend and mod wheel curves). It shows in the panels' row while the SEQ tab is picked and no
// full-size editor is over the row; the View lists its widgets there, and its scale's and snap's lists over the
// others. The card's knobs and switches are params the View edits as it edits every knob, Choice and Toggle; the rest
// (the roll, its ruler, keys, lane, tools and field) is the panel's. A note is a slot of the pattern with a length; the
// roll picks notes by their slots.

#include "controller.h"
#include "panel.h"
#include "patchedits.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

namespace detail {
struct SeqRoll; // see layout.h
}

class SeqPanel : public Panel {
public:
    explicit SeqPanel(PanelHost &host);
    ~SeqPanel() override { seqBlipEnd(); } // a note an arrow's move put on let go as the editor closes

    // The confirm dialog's question this panel asks (see View::confirm_, whose other kinds are the View's and the
    // preset browser's): every note of the pattern going.
    static constexpr int kConfirmClear = 7;
    static constexpr int kConfirmOverwrite = 10; // a pattern pasted or copied over one that holds notes
    // The roll's right-click menu's commands, numbered from kMenuCommands (as many as kMenuCount; see
    // View::menuCommand(), which numbers the others).
    static constexpr int kMenuCommands = 50, kMenuCount = 12;

    // The SEQ view shows, with no shape editor over it.
    bool wide() const;
    // At its full size (the user's setting, EditorSettings::seqFull): as tall as the panels' row and the modulation
    // panel, the patterns' overview under the card; else the panels' row alone. The roll's places at its size.
    bool full() const;
    const detail::SeqRoll &roll() const;
    void toggleSize(); // the full size or the half, kept in the user's settings
    bool seqOn() const; // the sequencer's power switch
    // While it is off, where the View draws the widgets dimmed: all of the view but the card's title row, whose power
    // switch and size button stay as they are (what it paints there it dims itself, see paintBack()).
    std::vector<Rect> offAreas() const;

    void widgets(std::vector<Widget> &list) const override;
    void popups(std::vector<Widget> &list) const override; // the scale's and the snap's lists, over everything
    void paintBack(Canvas &c) override;
    void paintDrag(Canvas &c) override; // a pattern's tile carried onto another
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    bool doublePress(const Widget &w) const override { return w.kind == Kind::SeqSize; } // a quick second click too
    // A source's handle over PATTERN: its buttons take it, as one, ringed together, modulating the pattern picked.
    bool takesSource(const Widget &w, int source) const override;
    void paintSourceRing(Canvas &c, const Widget &w, bool over, Color color) override;
    bool dropSource(const Widget &w, int source) override;
    std::string tooltip(const Widget &w) const override;
    bool tipAtMouse(const Widget &w) const override;
    // The cursor over the roll (a pen while drawing, moving and stretching over a note), none elsewhere.
    std::optional<CursorKind> cursor(const Widget &w) const override;
    // With Bend, what the roll shows follows the mouse across it: the faint point on a bend's line, the point or
    // segment's middle lit, and the bend lit of the note under the mouse.
    bool hoverMoves(Point before, Point now) const override;
    void poll(const Telemetry &now, std::vector<Rect> &repaint) override;
    bool step() override;
    void pressing(const Widget *hit) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doubleClick(const Widget &w, Point p, Modifiers mods) override;
    void drag(const DragState &d) override;
    void drop(const DragState &d) override;
    void release() override;
    bool wheelOn(const Widget &w, Point p, float steps, Modifiers mods) override;
    bool wheelAcross(Point p, float steps) override;
    std::optional<std::pair<int, float>> panAt(Point p) const override;
    void panBegin(int part) override;
    std::optional<float> pan(int part, float first, float across, float down) override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;
    void closePopups() override;
    // The SEQ view's keys, while it shows (see View::seqKey()).
    bool key(Key key, bool shift) override;
    void stepApplied(const UndoStep &step, bool forward) override;
    ConfirmWords confirmWords(int kind) const override;
    void confirmed(int kind, int button) override;

private:
    using Kind = WidgetKind;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return edits_.value(index); }
    void edit(int index, double value) { edits_.edit(index, value); }
    void editMany(const std::vector<std::pair<int, double>> &changes) { edits_.editMany(changes); }

    struct SeqNote {
        int slot;
        double start, length;
        int pitch;
        double velocity, chance;
        bool slide, mute;
    };
    int seqPattern() const;
    std::vector<SeqNote> seqNotes() const; // the pattern's, in slot order
    std::vector<SeqNote> seqNotesOf(int pattern) const; // another's
    int seqStepsOf(int pattern) const;                  // and how long it is
    int seqSteps() const;                  // how long it is, in steps
    // The pitches the roll's rows show, bottom first: every one from layout's seqLow up, or only the scale's, as many
    // as fit.
    std::vector<int> seqRows() const;
    float seqStepX(double step) const;
    double seqStepAt(float x) const;
    float seqRowY(int pitch) const; // its row's top; below the grid for a pitch not shown
    int seqPitchAt(float y) const;  // the row's pitch at `y`, the nearest shown
    float seqRowH() const;
    double seqSnapped(double step, bool free) const; // onto the snap's grid, unless it is off or `free`
    Rect seqNoteRect(const SeqNote &note) const;
    // The note under `p` (its slot), the last drawn first, -1 for none; and whether `p` is at its right end, or its
    // left one (its start), either of which stretches it.
    int seqNoteAt(Point p, bool *atEnd = nullptr, bool *atStart = nullptr) const;
    int seqFreeSlot() const;
    bool seqSelected(int slot) const;
    // The notes the field and the tools act on: those picked, still in the pattern.
    std::vector<int> seqPicked() const;
    // A press, its drag holding `held` (for a double-click's second press, nothing: see doubleClick()).
    bool pressAs(const Widget &w, Point p, Modifiers mods, const Widget &held);
    // A list of `count` entries under `button`, `width` wide: above it where it would reach past the editor's bottom.
    Rect seqList(Rect button, int count, float width) const;
    void seqDrag(Point p);
    // The roll's zoom and scroll, the editor's alone (not saved): across, how many times the whole pattern it is
    // zoomed in and the step it shows from; up and down, how many times the rows are as high. And what they come to:
    // the steps it shows, the step it shows from (held to the pattern), every pitch its rows can show (only the
    // scale's while folded), and the index in them of its lowest row.
    float seqZoomX_ = 1.0f, seqScrollX_ = 0.0f, seqZoomY_ = 1.0f;
    Point seqPanStart_; // a pan's start: the step shown from, and the lowest row's index
    float seqShownSteps() const;
    float seqScroll() const;
    std::vector<int> seqPitchList() const;
    int seqFirstRow() const;
    void seqShowFromRow(float row); // the rows from that index up, held to the list
    // The rows moved just as far as shows every note of `slots` (`up`: the highest first, where they don't all fit),
    // and not at all while they show.
    void seqReveal(const std::vector<int> &slots, bool up);
    // Zoomed by `factor` across (the step at `at.x` staying where it is) or up and down (the row at `at.y`).
    void zoomSeq(bool across, float factor, Point at);
    // Where a note's dot shows in the lane (at its start, as high as its velocity or chance), and the note whose dot or
    // stem is under `p` (its slot, the last drawn first), -1 for none.
    Point seqLaneDot(const SeqNote &note) const;
    int seqLaneNoteAt(Point p) const; // a picked one first, where notes start together
    // The notes whose stems stand at `x` in the lane: of those starting together there, only the picked ones while
    // any of them is.
    std::vector<SeqNote> seqLaneNotesAt(float x) const;
    // The lane on a curve (BEND or MOD WHEEL): which, its points as stored, and those written back (the points past
    // them back to their defaults, so a preset saved sparse leaves them out), in the undo step open.
    bool seqCurveLane() const;
    SeqCurve seqLaneCurve() const;
    std::vector<SeqCurvePoint> seqCurvePoints(SeqCurve curve) const;
    void setSeqCurve(SeqCurve curve, const std::vector<SeqCurvePoint> &points);
    // Where a curve's value sits in the lane, up and down (the bend's 0 in the middle, the mod wheel's at the bottom),
    // and the value at `y`, in the curve's range.
    float seqCurveY(SeqCurve curve, double v) const;
    double seqCurveValueAt(SeqCurve curve, float y) const;
    // A point put where the mouse is: at its step and value, or with `snap` (Ctrl held) on the grid, across on the
    // snap's lines and up and down on the lane's (quarters of the curve's range), held to the pattern.
    SeqCurvePoint seqCurvePointFor(SeqCurve curve, Point p, bool snap) const;
    // Puts it in among the curve's points (after those at its step); its index, -1 where the curve is full.
    int addSeqCurvePoint(SeqCurve curve, SeqCurvePoint point);
    // The point of the lane's curve under `p`, and the segment whose middle (its bend handle) is, -1 for none.
    int seqCurvePointAt(Point p) const;
    int seqCurveBendAt(Point p) const;
    Point seqCurveBendPos(const std::vector<SeqCurvePoint> &points, SeqCurve curve, int segment) const;
    // The curve's segments: as many as its points within the pattern, the last going round the pattern's end to the
    // first point (where it ends, counted on past the end), and whether one has a bend handle (see seqCurveBendAt()).
    int seqCurveSegments(const std::vector<SeqCurvePoint> &points) const;
    SeqCurvePoint seqCurveSegmentEnd(const std::vector<SeqCurvePoint> &points, int segment) const;
    bool seqCurveBends(const std::vector<SeqCurvePoint> &points, int segment) const;
    // Painting the curve, from `from` (at `fromValue`) on to `step` (at `v`): a point on every line of the grid
    // (`unit` steps apart) past `from` up to `step`, the values between in a straight line, in place of the points
    // there.
    void paintSeqCurve(double from, double fromValue, double step, double v, double unit);
    int seqCurveJustAdded_ = -1; // the point the last press put down, which a double-click leaves
    // The points picked on the lane's curve, as a shape editor's are (see ShapePick): of the pattern and curve shown,
    // while the curve has as many points as when they were picked; let go at a click anywhere off the lane.
    struct SeqCurvePick {
        int pattern = -1;
        SeqCurve curve = SeqCurve::Bend;
        int count = 0;
        std::vector<int> points; // in order
    };
    SeqCurvePick seqCurvePick_;
    std::vector<int> seqCurvePicked() const;
    void setSeqCurvePick(std::vector<int> points);
    // The points picked, moved together after the one grabbed (seqDrag_.anchor), now where `to` is.
    void moveSeqCurvePoints(SeqCurvePoint to);
    // --- The notes' own bends (see SeqNoteBendParam), drawn over the rows (a row a semitone) and edited with the Bend
    // tool.
    bool seqDrawing() const; // Draw picked (not Select or Bend)
    bool seqBendTool() const;
    // A note's points as stored, and written back (the points past them back to their defaults, so a preset saved
    // sparse leaves them out), in the undo step open.
    std::vector<SeqCurvePoint> seqBendPoints(int slot) const;
    void setSeqBend(int slot, const std::vector<SeqCurvePoint> &points);
    // The farthest a note bends from its pitch, signed (0 without points).
    double seqBendExtreme(int slot) const;
    // Where a pitch (fractions between) sits on the roll, up and down: between the middles of the rows it falls
    // between, in proportion (folded to the scale too), a row a semitone past the pitches the roll can show; and the
    // pitch at `y`.
    float seqPitchY(double pitch) const;
    double seqPitchAtY(float y) const;
    // Where a note's bend of `semitones` sits up and down on the roll, and the bend at `y` there.
    float seqBendY(const SeqNote &n, double semitones) const;
    double seqBendAtY(const SeqNote &n, float y) const;
    // With the Bend tool, every bent note's points and segments' handles take the mouse.
    struct BendHit {
        int slot = -1;
        int index = -1; // its point, or its segment
        bool operator==(const BendHit &) const = default;
    };
    BendHit seqBendPointAt(Point p) const;
    BendHit seqBendSegmentAt(Point p) const; // a segment's middle: its bend handle
    // With the Bend tool, the bend's line under `p` (within the note, clear of its points and handles): the note, the
    // point on the line there (from the note's start, as it runs) and where it shows.
    struct BendLineHit {
        int slot = -1;
        SeqCurvePoint point{};
        Point at{};
    };
    std::optional<BendLineHit> seqBendLineAt(Point p) const;
    // With the Bend tool, the note whose bend is lit at `p`: the one whose point, segment's middle or line is there,
    // else the note there; -1 for none.
    int seqBendLitNote(Point p) const;
    // With the Bend tool, the note under `p` whose bend runs over its bar there (one without a bend always does), which
    // a press bends; -1 for none. Where the bend has left the bar, the bar takes nothing.
    int seqBendNoteAt(Point p) const;
    // Whether a segment takes a bend (it rises or falls and has room for its handle), and where its handle is.
    bool seqBendSegmentBends(const SeqNote &n, const std::vector<SeqCurvePoint> &points, int segment) const;
    Point seqBendSegmentPos(const SeqNote &n, const std::vector<SeqCurvePoint> &points, int segment) const;
    // A point put where the mouse is on note `n`: within the note, on the snap's lines and up and down on the rows (only
    // the scale's while folded), or with `free` anywhere, to the cent.
    SeqCurvePoint seqBendPointFor(const SeqNote &n, Point p, bool free) const;
    // Puts it in among the note's points (after those at its time), a point at the note's start at 0 first where the
    // note has none and it goes later; its index, -1 (flashing why) where the note bends through as many as it can.
    int addSeqBendPoint(int slot, SeqCurvePoint point);
    // Shift+drag with Bend: the note's bend drawn from `from` (at `fromValue`) on to `step` (at `v`), a point on every
    // line of the grid (`unit` steps apart) past `from`, in place of the points there.
    void paintSeqBend(int slot, double from, double fromValue, double step, double v, double unit);
    std::optional<SeqNote> seqNoteOf(int slot) const;
    BendHit seqBendJustAdded_; // the point the last press put down, which a double-click leaves
    // The bend points picked (a box from empty space with Bend, Shift or Ctrl+click, Ctrl+A), as the lane's curve's are:
    // of the pattern shown, each while its note has as many points as when it was picked; moved together, and Delete
    // removing them.
    struct BendPick {
        int pattern = -1;
        std::vector<BendHit> points;
        std::vector<int> counts; // each one's note's points as it was picked
    };
    BendPick seqBendPick_;
    std::vector<BendHit> seqBendPicked() const;
    void setSeqBendPick(std::vector<BendHit> points);
    // The points picked, moved together after the one grabbed (seqDrag_.anchor, bendIndex), now where `p` is.
    void moveSeqBendPoints(Point p);
    void removeSeqBendPoints(const std::vector<BendHit> &points); // one undo step
    void doubleClickBend(Point p);
    // The notes' bends on the roll.
    void paintSeqBends(Canvas &c, const std::vector<SeqNote> &notes);
    // Every param of a note (its fields, then its bend's), as it is copied, cleared and pasted.
    static std::vector<int> seqSlotParams(int pattern, int slot);
    static constexpr int kSeqSlotParamCount = kSeqNoteParamCount + kSeqNoteBendParamCount;
    Rect seqScrollTrack() const;
    Rect seqScrollThumb() const;
    Rect seqScrollYThumb() const; // the rows' bar's thumb: high up for high rows
    // The roll fitted to the pattern's notes, after a patch loaded: across the whole pattern, the rows about the notes
    // (lower, down to the smallest, where they don't fit).
    void seqFit();
    uint64_t seqLoadsSeen_ = 0; // the patch loads the roll was fitted to (see Controller::patchLoads())
    // Notes glowing as the sequencer plays them: a note starts glowing as the playhead (in the telemetry `t`) reaches
    // its first step, and fades over as long as it lasts at the tempo. Whether any glows, or just stopped, to paint.
    bool seqTrackGlows(const Telemetry &t);
    float seqGlow(const SeqNote &note) const; // 0..1
    std::array<double, kSeqNotes> seqGlowFrom_ = [] {
        std::array<double, kSeqNotes> a{};
        a.fill(-1.0);
        return a;
    }(); // when each slot's note started glowing, in seconds (see seqNow()), -1 for never
    std::array<bool, kSeqNotes> seqInFirstStep_{}; // whether the playhead was in each note's first step at the last poll
    bool seqGlowed_ = false;
    // The roll's keys lit as the sequencer plays their notes (or one is heard while held): each eases up to lit and
    // back down a step every poll, rather than switching. Whether any moved, to paint.
    bool seqFollowKeys(const Telemetry &t);
    std::array<float, 128> seqKeyLight_{}; // 0..1, each key's light as shown
    static double seqNow(); // seconds on a steady clock
    // A note, a box or a paint stroke dragged to the roll's edge (or past it) scrolls it that way, a step of it each
    // poll: whether it did.
    bool seqEdgeScroll();
    float seqEdgeRows_ = 0.0f; // the rows scrolled so far by part, until a whole one
    std::string seqFieldText(int field) const;
    // One undo step each: a note put down (its slot, -1 when the pattern is full), notes removed, the picked notes'
    // field `field` moved by `steps` of it, Slide or Mute switched, the picked notes (or every one) stretched.
    int addSeqNote(double start, int pitch, double length);
    void removeSeqNotes(const std::vector<int> &slots);
    // No two notes start together on one pitch: the notes that do with one of `kept` (just put down or moved) go, in
    // the undo step open.
    void seqDropCovered(const std::vector<int> &kept);
    bool seqPatternEmpty(int pattern) const; // no notes, and no points on its curves
    void stepSeqField(int field, double steps);
    void flipSeqFlag(int flag);
    void stretchSeq(double factor);
    void clearSeqPattern(int pattern);    // its notes and its curves' points, one undo step
    void askClearSeqPattern(int pattern); // asking first (kConfirmClear), unless it is empty
    int seqClearing_ = 0;                 // the pattern kConfirmClear asks about
    void copySeqPattern(int from, int to); // every note, curve, length and loop, one undo step
    // The overview's tile under `p`, -1 for none; and one tile painted, `carried` as a drag carries it.
    int seqPatternModulated() const;
    int seqTileAt(Point p) const;
    void paintSeqTile(Canvas &c, int pattern, Rect tile, bool hovered, bool carried);
    struct TileScale {
        Rect notes;         // the tile's notes area
        int steps = 1;      // the pattern's
        float stepW = 0.0f; // a step across, the longest pattern's steps filling the area
        float from = 0.0f;  // the pitch whose row's bottom is the area's bottom
        float rowH = 0.0f;  // a pitch's row, 0 while the pattern has no notes
    };
    TileScale seqTileScale(int pattern) const;
    std::optional<Rect> seqViewRect(int pattern) const;
    bool seqViewGrabbed(int pattern, Point p) const;
    // A pattern's params in one order (its length and loop, its notes, its curves), the values the pattern clipboard
    // holds; a pattern's values written over another's, one undo step, and the message saying so flashed; or, while
    // the other holds notes or curve points and would change, asked about first (kConfirmOverwrite).
    std::vector<int> seqPatternParams(int pattern) const;
    std::vector<double> seqPatternValues(int pattern) const;
    void writeSeqPattern(const std::vector<double> &values, int to, const std::string &done);
    void offerSeqPattern(std::vector<double> values, int to, std::string done);
    void copySeqPatternToClip(int pattern); // into the pattern clipboard, which the editor keeps for any patch
    void pasteSeqPattern(int to);           // from it, asking first over notes
    std::vector<double> seqPatternClip_; // the pattern copied (Copy Pattern, Ctrl+C over a tile), empty for none
    struct {
        std::vector<double> values;
        int to = -1;
        std::string done;
    } seqOverwrite_;       // the paste or copy kConfirmOverwrite asks about
    int seqMenuTile_ = -1; // the tile the right-click menu opened on
    // The notes copied (Ctrl+C, Ctrl+X, the roll's menu), kept by the editor for any pattern: each note's fields, its
    // start from the first one's; and how far they reach, from that start to the last one's end on the grid.
    struct SeqClip {
        std::vector<std::array<double, kSeqSlotParamCount>> notes; // its fields and its bend (see seqSlotParams())
        double from = 0.0; // where the first one started
        double span = 0.0;
    };
    SeqClip seqClip_;
    double seqClipNext_ = 0.0; // where a paste goes without the mouse on the roll: after what was copied or pasted last
    SeqClip seqCopyPicked() const; // the notes picked, as the clipboard keeps them
    // The notes of `clip` put down from `at` (as one undo step, picked, those past the pattern's end left out); false if
    // none went in.
    bool seqPaste(const SeqClip &clip, double at);
    double seqMenuAt_ = 0.0; // the step the roll's menu was opened at, where its Paste goes
    // A drag on the roll, its ruler, its lane or the field (the panel's own, see PanelHost::startDrag()): what it moves,
    // and what it started from.
    struct SeqDrag {
        enum class Mode { None, Move, Resize, ResizeStart, Box, Paint, Velocity, Lane, LaneGroup, Loop, LoopStart,
                          LoopEnd, LoopMove, Field, Audition, Scroll, ScrollY, CurvePoint, CurveBend, CurvePaint,
                          CurveBox, LaneBox, Tile, ViewRect, BendPoint, BendCurve, BendPaint, BendGroup, BendBox };
        Mode mode = Mode::None;
        Point start, now;                // where the mouse went down, and where it is now
        int anchor = -1;                 // the note grabbed (Move, Resize), the field (Field), the key (Audition)
        double grabStep = 0.0;           // where it was grabbed: in steps,
        int grabPitch = 0;               // and its row
        std::vector<SeqNote> origin;     // the notes moved, as they were
        bool free = false;               // Shift or Alt held: off the snap's grid
        bool extend = false;             // Velocity: a note put down by a double-click, which across also sets the length
        // extend: whether the mouse went across (else up and down) on its last move of a few pixels, which the cursor
        // shows, and where that move ended.
        bool across = false;
        Point turnedAt;
        bool unpick = false;             // Shift-pressed on a note picked: let go unmoved, it is no longer picked
        std::vector<int> painted;        // the notes painted so far (Paint)
        std::vector<int> boxBase;        // the notes picked before the box, which Shift adds to
        double loopFrom = 0.0;           // Loop: the step it started at; CurvePaint: the step it painted last
        double startCurve = 0.0;         // CurveBend: the segment's bend as it was grabbed; CurvePaint: the value
                                         // painted last
        int sounding = -1;               // the note heard while it is held (see Controller::auditionNote()), -1 for none
        // The lane's curve: the points picked, moved together (CurvePoint) and where they were as they were grabbed;
        // a point of several picked, pressed without Ctrl or Shift, picked alone if let go unmoved (`narrow`); and the
        // points picked before a box (CurveBox), which Ctrl or Shift adds it to.
        std::vector<int> curvePoints;
        std::vector<SeqCurvePoint> curveOrigin;
        bool narrow = false;
        bool curveUnpick = false; // a point picked, pressed with Ctrl or Shift: let go of if let go unmoved
        std::vector<int> curveBase;
        bool carried = false; // Tile: moved off where it was pressed, so carried onto another
        // Box: where it started on the roll, in rows up from the first of seqPitchList()'s (and across in grabStep's
        // steps), so that corner stays on the notes it started by as the roll scrolls under a drag held at its edge.
        float boxRow = 0.0f;
        // BendPoint, BendCurve, BendPaint: the note's point or segment (with the note in `anchor`), and whether a stroke
        // ran out of points (flashed once).
        int bendIndex = -1;
        bool bendFull = false;
        // BendGroup: the points picked, moved together, and where they were as they were grabbed; BendBox: those picked
        // before the box, which Shift or Ctrl adds it to.
        std::vector<BendHit> bendPoints;
        std::vector<SeqCurvePoint> bendOrigin;
    };
    Point seqBoxStart() const; // the Box's starting corner, where the roll shows it now
    void seqHear(int pitch); // that note heard from now on in the drag, the one before let go; -1 lets it go
    // The notes picked moved a row or an octave by the arrows, heard for a moment at their new pitches (each pitch
    // once), those heard before let go; poll() lets them go once kSeqBlipSeconds have passed.
    void seqBlip(const std::vector<int> &slots);
    void seqBlipEnd();
    static constexpr double kSeqBlipSeconds = 0.3;
    std::vector<int> seqBlipPitches_; // the pitches heard
    double seqBlipUntil_ = 0.0;       // until when, on the editor's clock (see PanelHost::now())
    SeqDrag seqDrag_;
    bool dragging() const { return seqDrag_.mode != SeqDrag::Mode::None; } // a drag of its own goes on
    // The pattern's loop, in steps, as its params have it (the end, 0 for the pattern's, as the pattern's end), and
    // what a press on the ruler at `x` drags: an end of it, the whole of it, or a new one across the bars.
    std::pair<int, int> seqLoop() const;
    std::pair<int, int> seqLoopOf(int pattern) const;
    SeqDrag::Mode seqRulerMode(float x) const;
    std::vector<int> seqSelection_; // the slots picked (of the pattern shown)
    int seqSelectionPattern_ = 0;   // and which pattern that was: another one's picks are forgotten
    bool seqSnapMenu_ = false;
    // The macros' list, open over its tab; the macro the tab names while the lane shows something else; and the macro
    // the tab names now.
    bool seqMacroMenu_ = false;
    int seqLastMacro_ = 0;
    int seqTabMacro() const;
    void showSeqMacro(int macro);

    PanelHost &host_;
    PatchEdits &edits_; // the params, edited as the editor edits them, with why an edit was refused flashed up
    const ParamTable &table_;
};

} // namespace fmvibe::gui
