#pragma once

#include "canvas.h"
#include "controller.h"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace fmvibe::gui {

struct Modifiers {
    bool shift = false; // fine adjustment
    bool ctrl = false;
    bool alt = false;
};

enum class CursorKind { Arrow, Hand, Vertical, Horizontal, Move, Text };

struct MenuItem {
    int command = 0; // 0 is a separator
    std::string label;
    bool checked = false; // shown with a check mark: the choice in force
};

// Points that depend only on what their key holds (the params a curve is drawn from, where it goes), kept from
// frame to frame and worked out again only when any of that changes.
class PointCache {
public:
    // The points for `key` (its first value telling the curves apart), from make() unless they are kept already.
    template <class Make>
    std::vector<Point> get(const std::vector<double> &key, Make &&make);

private:
    struct Entry {
        uint64_t hash = 0;
        std::vector<double> key;
        std::vector<Point> points;
        uint64_t used = 0;
    };
    std::vector<Entry> entries_;
    uint64_t clock_ = 0;
};

// The whole editor: layout, drawing and mouse handling in logical coordinates
// (kWidth x kHeight). Only depends on Direct2D, so it can render into a window
// or an offscreen bitmap alike.
class View {
public:
    static constexpr float kWidth = 1200.0f;
    // The height depends on whether the modulation panel is collapsed.
    static float heightFor(const UiLayout &layout);
    float height() const { return heightFor(controller_.layout()); }

    explicit View(Controller &controller);
    ~View(); // ends a drag still going on, so no note hangs and no gesture stays open

    // The color themes, by the names Controller::theme() takes; the first is the default.
    static int themeCount();
    static const char *themeName(int index);

    void paint(Canvas &canvas);
    // Picks up param changes (automation, state loads) and telemetry; true if a repaint is due.
    bool poll();
    // Takes the latest telemetry, so a frame painted between polls animates from
    // where the audio is now rather than from the last poll.
    void refreshTelemetry() { controller_.telemetry(telemetry_); }

    // True if what the editor shows changed: what is under the mouse, or what a drag moves. A move that changes
    // neither needs no paint.
    bool mouseMove(Point p, Modifiers mods);
    void mouseDown(Point p, Modifiers mods);
    void mouseUp(Point p, Modifiers mods);
    void mouseLeave();
    void doubleClick(Point p, Modifiers mods);
    void wheel(Point p, float steps, Modifiers mods);
    std::vector<MenuItem> contextMenu(Point p);
    void menuCommand(int command);
    CursorKind cursor() const;
    // Undo and redo the patch's last edits; not while a drag or typing goes on.
    void undo();
    void redo();

    // Typing a value, after a double-click on the number a knob or slot shows, or a search while the preset browser
    // is open. Meanwhile the window should send the keyboard here: typeChar() takes printable ASCII. Up and Down only
    // move through the browser's list.
    enum class Key { Enter, Escape, Backspace, Delete, Left, Right, Home, End, SelectAll, Up, Down };
    bool typing() const { return typing_.has_value() || browserOpen_; }
    void typeChar(char32_t c);
    // Shift with Left, Right, Home and End extends the browser search's selection.
    void typeKey(Key key, bool shift = false);
    // The clipboard, which the window keeps: what Ctrl+C (or with `cut`, Ctrl+X) takes from what is typed, the
    // search's selection or a typed value's while all of it is selected, and what Ctrl+V puts in its place (line
    // breaks and tabs as spaces, other characters past printable ASCII left out).
    std::string copyText(bool cut);
    void pasteText(std::string_view text);
    // Commits the value typed, if it reads as one, or drops it.
    void endTyping(bool commit);

private:
    enum class Kind {
        Knob,         // param
        Choice,       // param, b = the value it selects (segmented buttons)
        Toggle,       // param: flips between 0 and 1
        ViewTab,      // a = RoutingView: picks what the panels under the tabs show
        OpTab,        // a = InspectorTab: picks what the lower half of an operator's page shows
        ScopeCycles,  // b = how many cycles the scope shows

        NodeTab,      // a = node: selects it; the selected one switches it on and off
        MatrixLabel,  // a = node: a matrix row's or column's name; a click does what the node's tab does
        MacroName,    // a = macro: the name under its knob; double-click renames it
        NoteDisplay,  // a = kNoteSource or kVelocitySource: its display in the keyboard strip; a click opens its curve
        FxFold,       // a = effect slot: folds its card to a strip, or unfolds it
        SourceFold,   // a = source place: the same for its card
        MacroHandle,  // a = kMaxSources + macro, kNoteSource, kWheelSource or kVelocitySource: drag onto a target, as a
                      // source's handle
        WaveButton,   // a = op, b = wave: one of the common waves
        WaveMore,     // a = op: "..." opens the list of the other waves
        WaveMenu,     // a = 1 on the open list, 0 around it (a click there closes it)
        WaveMenuItem, // a = op, b = wave: an entry of the list
        TypeButton,   // a = filter, b = type
        Node,         // a = node (operators, then filters)
        NodeGhost,    // a = node that is off: a dashed button in the graph's top left that switches it on
        Port,         // a = node: its edge, a band along its outline; drag from it to connect
        WireLabel,    // param = route amount, a = from node, b = to node
        OutLabel,     // param = output level, a = node
        WireEnd,      // param = the route, a = from node, b = to node (kNodes: the output): its arrowhead, to drag elsewhere
        Wire,         // param = the route, a = from node, b = to node: along its curve; drag to bend it
        GridCell,     // param, a = row node, b = column
        SourceHandle, // a = source: drag onto a param to modulate it
        SourceGrip,   // a = source, b = its position among the sources: drag to reorder
        SourceAdd,    // a = SourceType: fills a free place with a new source of that type
        SourceRemove, // a = source
        SourcesToggle, // collapses the modulation panel to its title row, or opens it again
        KeysToggle,    // collapses the keyboard panel to its top row, or opens it again
        SlotBar,      // param = slot amount, a = source, b = slot
        SlotRemove,   // a = source, b = slot
        SlotBypass,   // param = the slot's bypass, a = source, b = slot: the dot at the row's start
        SlotPolarity, // a = source, b = slot: switches its modulation between unipolar and bipolar
        SlotScroll,   // a = source: the bar beside a long slot list; drag the thumb, or click to jump there
        ValueText,    // param, a = source, b = slot: a slot's depth in numbers; double-click types one
        EnvPoint,     // a = envelope (ops, then kOps + source), b = point
        EnvCurve,     // param = curve, a = envelope, b = segment (attack, decay, release): bends it
        FilterPoint,  // a = filter: cutoff and resonance handle
        ModDot,       // param = slot amount, a = source, b = slot: depth handle on a knob
        FxGrip,       // a = effect slot, b = its position in the chain: drag to reorder
        FxPower,      // param = its switch, a = effect slot, b = position: the icon; a click switches it, a drag moves it
        FxMenuButton, // ADD EFFECT: opens the list of effect types
        FxShadow,     // the dashed card after the effects: opens the list of effect types over itself
        FxInsert,     // a = place in the chain: the arrow between two cards; opens the list to add an effect there
        SourceShadow, // a = SourceType: the dashed cards after the sources, LFO above ENV: adds one
        FxMenu,       // a = 1 on the open list, 0 around it (a click there closes it)
        FxAdd,        // a = FxType: an entry of the list, puts that effect at the end of the chain
        FxRemove,     // a = effect slot
        Scrollbar,    // a = ScrollArea: drag the thumb, or click the track to jump there
        McBand,       // a = effect slot, b = band: which band of a three-band compressor its card shows
        PeqPoint,     // param = the band's Q (for the wheel), a = effect slot, b = band: frequency and gain handle
        LfoDisplay,   // a = lfo: click opens the shape editor of a freeform LFO
        ClipDisplay,  // a = effect slot: the EDIT button on a drawn curve's display, which opens the big editor
        ListButton,   // param = a choice (an LFO's shape, the shaper's curve, the reverb's algorithm): opens its list
        ListMenu,     // a = 1 on the open list, 0 around it (a click there closes it)
        ListItem,     // param, b = the value it picks: an entry of the list
        PeqTypeButton, // param = the band's type, a = effect slot, b = band: opens the list of band shapes
        PeqTypeMenu,  // a = 1 on the open list, 0 around it (a click there closes it)
        PeqTypeItem,  // param = the band's type, b = PeqType: an entry of the list
        ShapeArea,    // a = shape (see shapeEditor_): double-click adds a point
        ShapePoint,   // a = shape, b = point
        ShapeCurve,   // param = curve, a = shape, b = point: bends the segment leaving the point
        PartialArea,  // a = operator, b = 0 for the levels, 1 for the phases: drag across to draw the partials
        PartialReset, // a = operator: back to a plain sine
        ShapeDone,    // closes the shape editor
        ShapeReset,   // a = shape: back to the shape it starts with
        ShapeGrid,    // a = axis (0 across, 1 up and down), b = -1 or 1 steps the grid, 0 is its count (wheel)
        UndoButton,   // a = 0 undoes, 1 redoes
        Key,          // a = key of the keyboard (0 = the lowest): drag across keys to play them
        Octave,       // a = -1 or 1: shifts the keyboard down or up an octave
        PresetButton, // a = PresetAction
        PresetStep,   // a = -1 or 1: the previous or next preset in the library
        PresetNameBox, // the current preset's name: opens the preset browser
        BrowserBackdrop, // a = 1 on the browser itself, 0 around it (a click there closes it)
        BrowserRow,   // a = preset index: loads it
        BrowserClose,
        BrowserFolder, // opens the preset folder
        BrowserSearch, // the search field: click to put the caret there, drag to select, double-click for a word
        BrowserClear, // the x in the search field: empties it
        SettingsButton, // the cog next to the preset buttons: opens the settings dialog
        ConfirmBackdrop, // a = 1 on the dialog asking before Init or Load, 0 around it (a click there cancels)
        ConfirmButton,  // a = 1 goes ahead, 0 cancels
        SettingsBackdrop, // a = 1 on the dialog itself, 0 around it (a click there closes it)
        SettingsOption, // b = the oversampling factor it picks
        SettingsTheme,  // b = the theme it picks
        SettingsGlow,   // the slider for how strongly the graphs' curves glow
        SettingsClose,
        ModWheel,     // param = mod wheel: drag or scroll
        BendWheel,    // drag; springs back to center on release
    };

    enum class PresetAction { Init, Load, Save };

    struct Widget {
        Kind kind = Kind::Knob;
        Rect rect;
        int param = -1;
        int a = -1;
        int b = -1;
        const char *label = nullptr;
        bool compact = false; // header knob with its label beside it
        bool disabled = false; // dimmed, and takes no input (a fixed operator's Pitch and Detune, their depth handles)
        Rect clip{};          // on a card that may be scrolled partly out: drawn and hit only within this

        bool same(const Widget &o) const { return kind == o.kind && param == o.param && a == o.a && b == o.b; }
    };

    enum class DragKind {
        None,
        Value,
        Node,
        Connect,
        Reroute,
        ModSource,
        EnvPoint,
        EnvCurve,
        FilterPoint,
        PeqPoint,
        FxMove,
        SourceMove,
        Scroll,
        ShapePoint,
        ShapeCurve,
        Partials,
        Key,
        ModWheel,
        BendWheel,
        SlotScroll,
        Glow,
        TextSelect, // across the browser's search
        Bend,       // a wire, sideways: how far it bows
    };

    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start;
        Point now;
        double startValue = 0.0;
        bool moved = false;
        bool fine = false;
        bool snap = false; // shape points snap to a grid
        float envScale = 0.0f; // an envelope's zoom, held while one of its handles is dragged
        bool step = false; // an undo step is open until the drag ends (moving nodes or cards)
        float scrollStart = 0.0f; // moving a card: where its row was scrolled to when it was grabbed
        int lastPartial = -1; // drawing partials: the one the last move was over, and its value in startValue
    };

    struct Typing {
        Widget widget;
        std::string text;
        size_t caret = 0;
        bool selected = true; // all of it, as it starts: typing replaces it
    };

    struct Assignment {
        int source;
        int slot;
        int target;
        double amount;
        bool bypassed; // kept, but modulating nothing for now
        bool bipolar;  // swings its target both ways (see slotBipolar())
    };

    // While a paint or a hit test goes on, nothing they read changes (but for what the host may change meanwhile, which
    // the next poll() brings), so what is derived from the params, the layout and the editor's state is worked out
    // once and kept until the outermost scope ends: the effects chain and the sources as they show, how far their
    // rows are scrolled, each source's slots, where the cards' shapes are, the modulation by target and the widgets.
    // Outside a scope, each call works it out afresh.
    class DerivedScope {
    public:
        explicit DerivedScope(const View &view);
        ~DerivedScope();
        DerivedScope(const DerivedScope &) = delete;
        DerivedScope &operator=(const DerivedScope &) = delete;

    private:
        const View &view_;
    };
    // assignments(), and in it each target's, in the same order.
    struct ModTable {
        std::vector<Assignment> all;
        std::vector<int> byTarget; // indices into `all`, by target and then in their order there
    };
    struct Derived {
        std::optional<std::vector<int>> chain, chainShown, sources, sourcesShown;
        std::optional<float> fxLength, sourcesLength;
        std::array<std::optional<std::vector<int>>, kAllSources> slots{};
        std::array<std::optional<float>, kMaxSources> cards{};
        std::array<std::optional<Rect>, kMaxSources + kShaperUnits + 3> shapeAreas{}; // by shape + 1, to kVelocityCurve
        std::optional<ModTable> mods;
        std::optional<std::vector<Widget>> widgets;
    };
    ModTable modTable() const;
    // Calls f(m) for each assignment of `target`, in assignments() order.
    template <class F>
    void forEachMod(int target, F &&f) const;
    // The widgets, built once while a scope goes on: paint() and hitTest() ask, each in a scope of its own.
    const std::vector<Widget> &scopedWidgets() const;

    // Model access.
    double value(int index) const;
    // Noise has no pitch or unison copies: just a slope and a stereo spread.
    bool isNoise(int op) const;
    // Plays at its Freq x Mult rather than following the note (see OpParam::Fixed).
    bool isFixed(int op) const;
    std::string fixedHzText(int op, bool compact) const;
    double norm(int index) const;
    void edit(int index, double value);
    std::vector<Assignment> assignments() const;
    std::vector<int> usedSlots(int source) const;
    // A source's card lists its used slots, a few rows at a time: the first row it shows,
    // and scrolling so that `slot` shows.
    // How many rows of a source's list are scrolled past as shown (fractions while it glides), the most
    // that can be, and scrolling it: gliding there over a few polls, or at once.
    float slotFirst(int source) const;
    float slotEnd(int source) const;
    void scrollSlots(int source, float first, bool glide);
    bool glideSlots(); // moves the gliding lists on; true if any moved
    void revealSlot(int source, int slot);
    // The thumb of a long slot list's scrollbar, on the card at screen place `card`.
    Rect slotThumb(int source, float card) const;
    void assign(int source, int target);
    void removeSlot(int source, int slot);
    bool slotBypassed(int source, int slot) const;
    void toggleBypass(int source, int slot);
    // Whether a slot swings its target both ways (see fmvibe::slotBipolar()): as set for it, or as its source does.
    bool slotBipolar(int source, int slot) const;
    // Sets it for that slot alone; for a source, sets the source's own polarity and puts all of its slots back to
    // following it. Each is one undo step.
    void setSlotPolarity(int source, int slot, bool bipolar);
    void setSourcePolarity(int source, bool bipolar);
    // How far a route's wire bows to its left, as a share of the distance it spans: as dragged, or kDefaultWireBend.
    float wireBend(int param) const;
    void setWireBend(int param, float bend); // kDefaultWireBend forgets it
    // "Bypass LFO 1 modulation", or "Resume ..." for one that is bypassed.
    std::string bypassLabel(int source, int slot) const;
    void connect(int from, int to);
    // Moves the route from `from` into `to` over to `into` (kNodes: the output), with its amount and modulation.
    void reroute(int from, int to, int into);
    // The route whose arrowhead is being dragged elsewhere, which the graph then draws following the mouse; else -1.
    int rerouting() const;
    // Whether a source's handle let go of on `w` modulates what it shows: a knob, a route's label or matrix cell, or
    // a depth bar, as the drag's overlay rings them; never the source's own output (the mod wheel's knob).
    bool droppable(const Widget &w, int source) const;
    // The color of the effect in `slot`: each type of effect has its own.
    Color fxColor(int slot) const;
    // A list's color (its effect's, the voice shapers' or the filters'), and whether it can pick `item`: a band-pass's
    // or notch's slope list can't take 6 or 18 dB.
    Color listColor(int param) const;
    bool listItemFits(int param, int item) const;
    // The entry a list shows as picked: its value, or for a slope the one that plays.
    int listShown(int param) const;
    bool nodeOn(int node) const;
    void switchOnRoute(int from, int to);
    std::vector<int> enabledNodes() const;
    // Whether sound gets into a filter or shaper: from an operator that is on, or a filter or shaper that has some.
    bool boxHasInput(int node) const;
    // Whether `node` is heard in the end: it goes out, or feeds or modulates a node that is.
    bool reachesOut(int node) const;
    // Whether a filter's or shaper's own sound gets to the output: it goes out, or into a filter or shaper whose sound
    // does.
    bool boxGoesOut(int node) const;
    // Effects. The FX view takes the place of the routing panel and the inspector.
    bool fxWide() const;
    FxType fxSlotType(int slot) const;
    // The slots that hold an effect, in chain order; shown, with the card being
    // dragged already moved to where it would land.
    std::vector<int> fxChain() const;
    std::vector<int> fxChainShown() const;
    std::vector<int> fxChainMoved() const; // fxChainShown() worked out
    // Fills an empty slot with a fresh effect of `type`, or a copy of slot `copyOf`, and puts it in the chain.
    // A new effect, or a copy of the one in slot `copyOf`, at place `at` in the chain, or at its end.
    void addEffect(FxType type, int copyOf = -1, int at = -1);
    void removeEffect(int slot);
    // The chain order with `chain` first, then the empty slots.
    double fxOrderWith(const std::vector<int> &chain) const;
    // Measured in card places (the parametric EQ's card takes more than one): how far the row is scrolled,
    // where the card at `index` of `chain` starts in the row, and so its screen place, 0..6 on screen and
    // fractions while it scrolls partly out.
    float fxScroll() const;
    // Cards folded to a strip, which take fewer places: kFoldedFx or kFoldedSource of one.
    bool fxFolded(int slot) const;
    bool sourceFolded(int source) const;
    float fxCardSpan(int slot) const;
    float sourceSpan(int source) const;
    float sourceOffset(const std::vector<int> &shown, size_t index) const;
    void setFolded(bool fx, int index, bool folded);
    void paintDownward(Canvas &c, Rect r, std::string_view name, Color color);
    float fxOffset(const std::vector<int> &chain, size_t index) const;
    float fxScreen(const std::vector<int> &chain, size_t index) const;
    void revealFx(int position);
    // The card rows that scroll: the effects chain and the modulation sources. How many
    // card places each has, how many fit, and its thumb.
    enum class ScrollArea { Fx, Sources };
    float scrollLength(ScrollArea area) const;
    // While there is room for more, a dashed card after the last effect (a card place wide) and two after the last
    // source (LFO above ENV, square) add one.
    bool fxShadow() const;
    bool sourceShadow() const;
    float scrollFirst(ScrollArea area) const;
    void setScroll(ScrollArea area, float first);
    Rect scrollThumb(ScrollArea area) const;
    Rect peqArea(int slot) const;
    // Several params changed as one gesture.
    void editMany(const std::vector<std::pair<int, double>> &changes);
    // Drawn shapes: a freeform LFO's (shape = the source), a shaper's curve (shape = kMaxSources + the shaper's
    // unit: an effect slot, or kFxSlots + a shaper), the note source's curve (shape = kNoteCurve) or velocity's
    // (kVelocityCurve). The big editor shows over the routing panel and the inspector while the LFO's shape is
    // Custom, over the routing panel while the note source's or velocity's curve or a voice shaper's is open, or over
    // the effects for a shaper effect's Custom curve;
    // a shaper effect's card, or a voice shaper's page in the inspector, lets you edit its curve too. A curve's
    // points run from the left edge to the right one; an LFO's wrap around.
    int shapeEditing() const;
    // Its geometry: the panel the big editor fills, its buttons, and where a shape is edited (the big
    // editor's plot, or a shaper curve's in its card while the big editor is closed) and where a value
    // sits there; without a shape, the big editor's.
    Rect shapePanel() const;
    Rect shapeArea(int shape = -1) const;
    Rect shapeAreaNow(int shape) const; // shapeArea() worked out
    Rect shapeDone() const;
    Rect shapeReset() const;
    Rect shapeGridButton(int axis, int part) const;
    Point shapeAt(double x, double y) const { return shapeAt(-1, x, y); }
    Point shapeAt(int shape, double x, double y) const;
    double shapeXAt(int shape, Point p) const;
    double shapeYAt(int shape, Point p) const;
    // The FX view, the shaper's curve editor that opens from it, or an LFO's shape editor takes the inspector's
    // place.
    bool inspectorCovered() const;
    // Shaper units (see shaperParams()): the shaper effects, by slot, then the voices' shapers, kFxSlots + the shaper.
    static constexpr int kNoteCurve = kMaxSources + kShaperUnits;
    static constexpr int kVelocityCurve = kNoteCurve + 1;
    static bool isCurve(int shape) { return shape >= kMaxSources; } // a shaper's, the note's or velocity's: no wrap
    static bool isClipCurve(int shape) { return shape >= kMaxSources && shape < kMaxSources + kFxSlots; } // an effect's
    static bool isShaperCurve(int shape) { return shape >= kMaxSources + kFxSlots && shape < kNoteCurve; }
    static bool isNoteCurve(int shape) { return shape == kNoteCurve; }
    static bool isVelocityCurve(int shape) { return shape == kVelocityCurve; }
    // The curves of the sources in the keyboard strip, which open from their displays there.
    static bool isStripCurve(int shape) { return isNoteCurve(shape) || isVelocityCurve(shape); }
    // The curve of the note source or velocity, and back.
    static int curveOf(int source) { return isVelocity(source) ? kVelocityCurve : kNoteCurve; }
    static int sourceOfCurve(int shape) { return isVelocityCurve(shape) ? kVelocitySource : kNoteSource; }
    static int shaperCurve(int shaper) { return kMaxSources + kFxSlots + shaper; }
    // The color of a shaper unit: its effect's, or the voice shapers'.
    Color clipColor(int unit) const;
    Color shapeColor(int shape) const;
    static int shapeBase(int shape);
    // How many points it can have, and how many params it takes from shapeBase() on.
    static int shapeMaxPoints(int shape) { return isStripCurve(shape) ? kMaxNotePoints : kMaxShapePoints; }
    static int shapeParamCount(int shape) { return isStripCurve(shape) ? kNoteCurveParamCount : kShapeParamCount; }
    int shapeIndex(int shape, int point, PointField field) const;
    int shapePointCount(int shape) const;
    // Onto a PointCache key: a drawn shape's point count (its param `base`) and its points, as many as it has.
    void keyShape(std::vector<double> &key, int base) const;
    double shapePointX(int shape, int point) const;
    Point shapePointPos(int shape, int point) const;
    Point shapeCurvePos(int shape, int point) const;
    void addShapePoint(int shape, Point p);
    // The changes that put a new point at (x, y) among the others, the later ones moving up a place.
    std::vector<std::pair<int, double>> insertShapePoint(int shape, double x, double y) const;
    // A symmetric shaper's curve (see ClipCurve): only its right half is edited, the left drawn as its mirror. Its
    // points left of the middle are hidden, and so are the bends that start there; a click on that half counts as one
    // on its mirror image. Switched on in the editor, the curve gets a point in the middle, which then stays there.
    bool curveSymmetric(int shape) const;
    bool pointShown(int shape, int point) const;
    bool middlePoint(int shape, int point) const;
    Point unmirrored(int shape, Point p) const;
    void switchSymmetric(int param);
    void switchFixed(int op);
    void addShapeStep(int shape, Point p, bool snap);
    bool drawsStep(const Widget &w, Modifiers mods) const;
    void removeShapePoint(int shape, int point);
    void resetShape(int shape);
    // The next grid the shape editor offers along `axis`, `steps` away.
    void stepShapeGrid(int axis, int steps);
    // While an LFO's or envelope's handle, or a card of the row, is dragged near either end of a row of cards, the
    // row scrolls that way.
    bool edgeScroll();
    void flash(std::string message);
    // The MIDI note under the keyboard's first key.
    int keyboardBase() const;
    // Modulation sources. The places that hold one, in the order they show; shown,
    // with the card being dragged already moved to where it would land.
    std::vector<int> sources() const;
    std::vector<int> sourcesShown() const;
    std::vector<int> sourcesMoved() const; // sourcesShown() worked out
    SourceType sourceTypeOf(int source) const;
    std::pair<double, double> modReach(const Assignment &m) const;
    // Whether a source swings its targets both ways (see fmvibe::sourceBipolar()).
    bool sourceBipolar(int source) const;
    // What a double-click or Reset sets a param to: its default, or a bipolar macro's middle.
    double resetValue(int param) const;
    std::string sourceName(int source) const;
    // A param's name as the editor shows it: a source's fields go by the source's name, not their place's.
    std::string paramName(int index) const;
    bool sourceOn(int source) const;
    // Whether `m`'s depth handle is under the mouse or being dragged: its range is drawn wider, over the others.
    bool modFocused(const Assignment &m) const;
    // Whether it does anything, as its handle shows: an LFO or envelope while it is on, a macro or the note source
    // once it modulates something.
    bool sourceLive(int source) const;
    // How many sources are scrolled past, as a fraction, and the screen place (0..3,
    // fractions while scrolling) of a source's card, or kOffScreen.
    static constexpr float kOffScreen = -100.0f;
    float sourceScroll() const;
    float cardOf(int source) const;
    void revealSource(int source);
    // Fills a free place with a fresh source of `type`, or a copy of `copyOf`, shown after the others or the original.
    void addSource(SourceType type, int copyOf = -1);
    // Takes it out with its modulation, and whatever modulates its knobs.
    void removeSource(int source);
    // Shows the sources in the order `shown`, the free places after them.
    void setSourceOrder(const std::vector<int> &shown);
    // Where an LFO of the newest voice is now, and its output there.
    double lfoPhaseNow(int lfo) const;
    double lfoValueNow(int lfo) const;
    // Whether notes start an LFO at its Phase: not while it is free (and not a one-shot), nor Random.
    bool lfoPhaseUsed(int lfo) const;

    // Geometry.
    std::vector<Widget> widgets() const;
    std::optional<Widget> hitTest(Point p) const;
    Point nodeCenter(int node) const;
    int nodeAt(Point p) const;
    Rect envRect(int env) const;
    static Point knobCenter(const Widget &w);
    static float knobRadius(const Widget &w);
    // Whether `p` is on the number widget `w` shows, where a double-click starts typing.
    static bool onValue(const Widget &w, Point p);
    void startTyping(const Widget &w);
    // Where the value being typed shows, over the number it replaces.
    Rect typingField() const;
    // Pixels per unit of normalized time: what fills the display, or while a drag of its handles goes on, what it
    // was when the drag began, as long as the envelope still fits.
    float envScale(int env) const;
    void applyDrag(Point p);
    // Ends the drag going on: its gestures and undo step, a key it plays, the bend wheel it holds. When the button
    // goes up, after what letting go does; or when its button-up never came, at the next press.
    void finishDrag();
    // Whole notches of the wheel on `target` (a param, or another of the wheel's targets), with `steps` added: a
    // trackpad's smaller steps add up toward them while the wheel stays on that target.
    int wheelNotches(int target, float steps);

    // Painting.
    void paintPanels(Canvas &c);
    void paintHeader(Canvas &c);
    void paintKeyboard(Canvas &c);
    void paintKey(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintWheel(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintRouting(Canvas &c);
    void paintMatrixLabels(Canvas &c);
    void paintFx(Canvas &c);
    void paintShapeEditor(Canvas &c, int shape);
    void paintClipCurveEditor(Canvas &c, int shape);
    void paintNoteCurveEditor(Canvas &c);
    void paintVelocityCurveEditor(Canvas &c);
    // The editor's grid; `zero` is where the zero line goes, and a centered grid stresses its middle line.
    void paintShapeGrid(Canvas &c, double zero, bool centered);
    void paintShapeGridControls(Canvas &c);
    // The partial editor: an operator's Custom wave, over the routing panel while the shape editor isn't.
    int partialEditing() const;
    int partialsShown(int op) const; // how many partials its Custom wave plays, and the editor shows
    void paintPartialEditor(Canvas &c, int op);
    // A shaper's transfer curve, or its drawn curve to edit: an effect slot's or (unit kFxSlots + shaper) a voice shaper's.
    void paintShaperCurve(Canvas &c, Rect r, int unit);
    void paintDisperser(Canvas &c, Rect r, int slot);
    void paintWidener(Canvas &c, Rect r, int slot);
    // The note source in the keyboard strip, right of the macros: the note the newest voice plays, and its name; and
    // velocity, right of it: the velocity the newest voice came with.
    void paintNoteSource(Canvas &c, float top);
    void paintVelocitySource(Canvas &c, float top);
    void paintClipCurveCard(Canvas &c, Rect r, int unit);
    void paintMirroredPoints(Canvas &c, int shape, float radius);
    void paintDelay(Canvas &c, Rect r, int slot);
    void paintEq(Canvas &c, Rect r, int slot);
    void paintParamEq(Canvas &c, Rect r, int slot);
    void paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum);
    // Where a parametric EQ band's handle sits: at its gain, or for a cut, at its resonance's peak.
    Point peqHandle(Rect area, int slot, int band) const;
    void paintCompressor(Canvas &c, Rect r, int slot);
    void paintMultiComp(Canvas &c, Rect r, int slot);
    void paintReverb(Canvas &c, Rect r, int slot);
    // The phaser's and flanger's responses, with the LFO at the middle and at both ends.
    void paintSweep(Canvas &c, Rect r, int slot, FxType type);
    void paintShifter(Canvas &c, Rect r, int slot);
    void paintInspector(Canvas &c);
    // A shaper's page in the inspector: its summary in `summary`, its transfer curve, and the label over its inputs.
    void paintShaperPage(Canvas &c, int shaper, Rect summary);
    std::string boxRoutes(int node) const;
    void paintSources(Canvas &c);
    void paintSource(Canvas &c, float card, int source);
    void paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live);
    // The values of the envelope handle `w` (hovered or dragged), at its display's top right over everything else.
    void paintEnvReadout(Canvas &c, const Widget &w);
    // A filter's response, from its params; its key tracking (`keyTrackParam`) follows `pitch` (semitones, 69 is
    // A4). `spectrum`: what the filter puts out, measured, to show behind the response, if any.
    void paintFilterResponse(Canvas &c, Rect r, int typeParam, int slopeParam, int cutoffParam, int resonanceParam,
                             int keyTrackParam, double pitch, Color color, const Spectrum *spectrum = nullptr);
    void paintUnison(Canvas &c, Rect r, int op);
    void paintScope(Canvas &c, Rect r, int op);
    // The tab an operator's page shows: the one picked, but a noise operator's envelope for the scope it hasn't.
    InspectorTab opTabShown(int op) const;
    bool opTabModulated(int op, InspectorTab tab) const;
    void paintNoiseSlope(Canvas &c, Rect r, int op);
    void paintLfo(Canvas &c, Rect r, int lfo);
    void paintWidget(Canvas &c, const Widget &w);
    void paintNode(Canvas &c, const Widget &w);
    void paintKnob(Canvas &c, const Widget &w);
    std::string modRangeText(int source, int slot) const;
    void paintModOverlay(Canvas &c, const std::vector<Widget> &list);
    // A route's amount where modulation takes the newest voice right now (0..1), or as set while nothing plays.
    double liveAmount(int param) const;
    // The color of the first source that modulates `param` (switched on, not bypassed), or none.
    std::optional<Color> modColor(int param) const;
    // Under a route's label or matrix cell: each modulation's range, as on a knob's ring, and where it is now.
    void paintModRange(Canvas &c, Rect bar, int param);
    // Lines from a hovered source handle to whatever it modulates that is on screen.
    void paintSourceLinks(Canvas &c, const std::vector<Widget> &list, int source);
    void paintTyping(Canvas &c);
    std::string undoText(bool redo) const;
    void paintTooltip(Canvas &c);
    // The settings dialog: while it is open it takes every click.
    std::vector<Widget> settingsWidgets() const;
    std::vector<Widget> confirmWidgets() const;
    std::string confirmWhat() const;
    // The preset browser: while it is open it takes every click, and the confirm dialog goes over it.
    std::vector<Widget> browserWidgets() const;
    void paintBrowser(Canvas &c);
    // Its rows: each preset's index and name, with a header (-1 and the subfolder) before each subfolder's. While
    // something is typed in its search, only the presets whose subfolder and name hold every word of it.
    std::vector<std::pair<int, std::string>> browserRows() const;
    // The browser takes the keyboard while it is open: what is typed goes into its search, Up and Down pick a
    // preset from the list, Enter loads it, and Escape empties the search, or then closes the browser.
    void browserChar(char32_t c);
    void browserKey(Key key, bool shift);
    void searchChanged();
    // Editing the search: its selection (from the anchor to the caret, either way round), typing or pasting over it,
    // and where a click at `x` puts the caret, by where each character starts as last painted.
    std::pair<size_t, size_t> searchSelection() const;
    void insertSearch(std::string_view text);
    void eraseSearch(size_t from, size_t to);
    void clearSearch();
    size_t searchStopAt(float x) const;
    // Scrolls the list so the preset's row shows: about in the middle, or just inside the list.
    void revealBrowserRow(int preset, bool middle);
    // Loads a preset of the library, asking first if the patch changed since the last.
    void openPreset(int index);
    void stepPreset(int direction);
    // Whether a route shows its wire: it is on, or it was and has only been turned down to 0 %.
    bool wired(int param) const;
    // Whether the graph shows `node`'s line to OUT: it is wired, and a filter has something to put out.
    bool goesOut(int node) const;
    // Whether `node`'s own sound reaches the output, as the dot on its tab shows: its Out is up, an operator is on, and
    // a filter or shaper has something going into it.
    bool soundsOut(int node) const;
    void keepRoutes();
    void disconnect(int param); // to 0 %, and its wire goes
    void runPreset(PresetAction action);
    void paintSettings(Canvas &c);
    void paintConfirm(Canvas &c);
    void paintHoverTags(Canvas &c, const std::vector<Widget> &list);

    bool isHovered(const Widget &w) const;
    bool isActive(const Widget &w) const;

    Controller &controller_;
    const ParamTable &table_;
    ParamValues seen_{};
    Telemetry telemetry_{};
    std::optional<Widget> hover_;
    Point hoverPoint_; // where the mouse is
    Drag drag_;
    // What the last press hit, as it went down, and where: a double-click after it goes with it.
    std::optional<Widget> pressed_;
    Point pressPoint_{-1000.0f, -1000.0f};
    int wheelTarget_ = -1; // what the wheel turned last (see wheelNotches()), and its steps short of a notch
    float wheelRest_ = 0.0f;
    // The open context menu's targets (below) still are what it opened on: no undo, redo or wheel since. The web
    // demo's menu stays open while those reach the editor.
    bool menuLive_ = false;
    int menuParam_ = -1;
    int menuFx_ = -1;                        // the effect slot of the open context menu
    int menuSource_ = -1;                    // the source of the open context menu
    std::optional<Widget> menuValue_;        // what the open context menu's "Edit value" types into
    std::pair<int, int> menuPoint_{-1, -1}; // lfo and shape point of the open context menu
    int shapeEditor_ = -1;                   // the LFO whose shape the routing panel shows, or -1
    int partialEditor_ = -1;                 // the operator whose Custom wave's partials it shows, or -1
    float sourceScroll_ = 0.0f;              // how many source cards are scrolled past, before clamping
    float fxScroll_ = 0.0f;                  // how many effect cards are scrolled past, before clamping
    bool fxMenuOpen_ = false;                // the ADD EFFECT list
    Point fxMenuAt_{-1.0f, -1.0f};           // its top left: over the shadow card it opened from, or (x < 0) under ADD EFFECT
    int fxInsertAt_ = -1;                    // where the list puts the effect it adds: a place in the chain, or (-1) its end
    int listMenu_ = -1;                      // the param whose list (ListButton) is open, or -1
    int peqMenu_ = -1;                       // the band type param whose list of shapes is open, or -1
    int waveMenu_ = -1;                      // the operator whose list of more waves is open, or -1
    std::array<int, kFxSlots> fxBand_{};     // the band each three-band compressor's card shows
    std::array<float, kMaxSources> slotScroll_{}; // rows each source's slot list is scrolled to, before clamping
    std::array<float, kMaxSources> slotGlide_{};  // and where it shows, gliding after it
    int octave_ = 0;                         // the keyboard's shift, -2..2 octaves from C1
    bool settingsOpen_ = false;
    int theme_ = 0;                          // the theme painted last
    // A running random LFO's last cycles, for its display: each one's glide (from, to) at its place in a
    // round of kRandomCycles, and how many have come since it started running (the newest is the last). The
    // phase the newest was last seen at, and whether it has ended without a snapshot bringing the next glide yet
    // (its phase went back: one that wraps before the glide changes, half written).
    static constexpr int kRandomCycles = 4;
    struct RandomTrace {
        std::array<std::pair<float, float>, kRandomCycles> glides{};
        int count = 0;
        float phase = 0.0f;
        bool ended = false;
    };
    std::array<RandomTrace, kMaxSources> randomTraces_{};
    void traceRandomLfos();         // takes in the newest telemetry's glides
    bool randomSweep(int lfo) const; // whether its display shows what it ran, rather than a made-up run
    // Where a running random LFO is in its newest traced cycle: like lfoPhaseNow(), but it stays at the end of
    // the cycle until a snapshot brings the next one, rather than wrap round to the start of the same quarter.
    double randomPhaseNow(int lfo) const;
    // The macros' names as their knobs' labels, in place for as long as the widgets that point at them.
    mutable std::array<std::array<char, 32>, kMacros> macroLabels_{};
    // What the macros and the mod wheel add to each param, worked out from their settings at each paint, so it shows
    // with no note playing too, while the host may have let the plugin sleep.
    std::vector<float> macroOffsets_ = std::vector<float>(kModulatedCount, 0.0f);
    // Where modulation takes param `index` for the newest voice, against its set value, or with no note playing
    // where the macros take it; nothing for the params that can't be modulated, such as the macros' own knobs.
    float liveOffset(int index) const {
        if (index < 0 || index >= kModulatedCount)
            return 0.0f;
        return telemetry_.voices > 0 ? telemetry_.modOffset[index] : macroOffsets_[index];
    }
    bool metering_ = false;                  // the output meter showed something when last polled
    float glow_ = 0.0f;                      // how strongly the curves glowed when painted last
    float glowDragged_ = -1.0f;              // the glow slider's amount while it is dragged, else -1
    float shownGlow() const {
        return glowDragged_ >= 0.0f ? glowDragged_ : static_cast<float>(controller_.glow());
    }
    int confirm_ = -1;                       // the PresetAction the confirm dialog asks about, or -1
    int pendingPreset_ = -1;                 // the library preset a confirmed Load loads, or -1 for a file
    bool browserOpen_ = false;
    int browserScroll_ = 0;                  // rows scrolled past, before clamping
    std::string browserSearch_;              // what is typed in its search
    size_t searchCaret_ = 0;                 // where the caret is in it
    size_t searchAnchor_ = 0;                // where its selection starts: at the caret, nothing is selected
    std::vector<float> searchStops_;         // where each place between its characters was painted, across
    float searchShift_ = 0.0f;               // how far its text is scrolled left, so the caret shows
    int browserPick_ = -1;                   // the preset Up and Down picked, which Enter loads, or -1
    double bend_ = 0.0;                      // the bend wheel's position, -1..1
    std::string flash_;
    int flashTicks_ = 0;
    std::optional<Typing> typing_;
    int caretTicks_ = 0; // polls since the last key, for the caret's blink
    mutable Derived derived_; // see DerivedScope
    mutable int derivedDepth_ = 0;
    mutable PointCache curves_; // the graphs' curves, worked out again only when what they are drawn from changes
};

} // namespace fmvibe::gui
