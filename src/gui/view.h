#pragma once

#include "canvas.h"
#include "controller.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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

    void paint(Canvas &canvas) { paint(canvas, wholeArea()); }
    // Paints only what shows in `area` (the editor's coordinates), leaving the rest as the canvas holds it from the
    // frame before: what lies outside it is skipped, and what reaches into it clipped to it. Within it, the pixels
    // come out as a whole paint would make them.
    void paint(Canvas &canvas, Rect area);
    Rect wholeArea() const { return {0.0f, 0.0f, kWidth, height()}; }
    // Picks up param changes (automation, state loads) and telemetry; true if a repaint is due.
    bool poll();
    // Where what the last poll() found changed shows: the whole editor, or the places of what moved when only what
    // keeps to its place did (the output meter and scope and the playing count in the header, the keys, the
    // operator's scope), which paint() can repaint alone, each apart.
    const std::vector<Rect> &changedAreas() const { return changedAreas_; }
    // Takes the latest telemetry, so a frame painted between polls animates from
    // where the audio is now rather than from the last poll.
    void refreshTelemetry() { controller_.telemetry(telemetry_); }

    // True if what the editor shows changed: what is under the mouse, or what a drag moves. A move that changes
    // neither needs no paint.
    bool mouseMove(Point p, Modifiers mods);
    void mouseDown(Point p, Modifiers mods);
    void mouseUp(Point p, Modifiers mods);
    // The mouse taken away mid-drag (the window losing its capture, the page its focus): what the drag holds ends,
    // its gestures and undo step closed, but nothing is dropped where the mouse is: no route moved or removed, no
    // source assigned, no card moved.
    void cancelDrag();
    void mouseLeave();
    void doubleClick(Point p, Modifiers mods);
    void wheel(Point p, float steps, Modifiers mods);
    // The middle button held over a panel that scrolls (the effects, the source cards, the MODS view's rows, the
    // preset browser's list) grabs what it shows: until it goes up, mouseMove() drags that along with the mouse.
    // Elsewhere, or while the left button drags something, it does nothing.
    void middleDown(Point p);
    void middleUp(Point p);
    std::vector<MenuItem> contextMenu(Point p);
    void menuCommand(int command);
    CursorKind cursor() const;
    // Undo and redo the patch's last edits; not while a drag or typing goes on.
    void undo();
    void redo();

    // Typing a value, after a double-click on the number a knob or slot shows, or into a text field: the preset
    // browser's search while it is open, the MODS view's filter once clicked, and the search atop a MODS row's target
    // menu while it is open. Meanwhile the window should send the keyboard here: typeChar() takes printable ASCII. Up
    // and Down only move through the browser's list and the target menu's matches.
    enum class Key { Enter, Escape, Backspace, Delete, Left, Right, Home, End, SelectAll, Up, Down };
    bool typing() const { return typing_.has_value() || fieldTyping().has_value(); }
    void typeChar(char32_t c);
    // Shift with Left, Right, Home and End extends a text field's selection.
    void typeKey(Key key, bool shift = false);
    // The clipboard, which the window keeps: what Ctrl+C (or with `cut`, Ctrl+X) takes from what is typed, a text
    // field's selection or a typed value's while all of it is selected, and what Ctrl+V puts in its place (line
    // breaks and tabs as spaces, other characters past printable ASCII left out).
    std::string copyText(bool cut);
    void pasteText(std::string_view text);
    // Commits the value typed, if it reads as one, or drops it.
    void endTyping(bool commit);
    // Ctrl going down or up, from the keyboard while the window has it (mouse messages bring it too): while it is held
    // over a graph, its grid's points show, and a node dragged snaps to them. True if a repaint is due.
    bool setCtrl(bool held);

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
        NodeDelete,   // a = node: the x in a circle at a node's upper right, while the mouse is over it: switches it off
        Port,         // a = node: its edge, a band along its outline; drag from it to connect
        WireLabel,    // param = route amount, a = from node, b = to node
        OutLabel,     // param = output level, a = node
        WireEnd,      // param = the route, a = from node, b = to node (kNodes: the output): its arrowhead, to drag elsewhere
        WireStart,    // param = the route, a = from node, b = to node (kNodes: the output): where it leaves its node, to
                      // drag onto another node, which it then comes from
        Wire,         // param = the route, a = from node, b = to node: along its curve; drag to bend it
        LabelMode,    // param = the route's mode, a = from node, b = to operator: the left part of a wire label's pill
                      // (see labelPill()), its mode's glyph; a click opens the list of modes, the wheel steps them
        LabelRemove,  // param = the route, a = from node, b = to node (kNodes: the output): the x at the right of a
                      // wire or OUT label's pill; a click removes the route with its modulation
        GridCell,     // param, a = row node, b = column
        SourceHandle, // a = source: drag onto a param to modulate it
        SourceGrip,   // a = source, b = its position among the sources: drag to reorder
        SourceMenuButton, // + SOURCE: opens the list of source types under it
        SourceRemove, // a = source
        SourcesToggle, // collapses the modulation panel to its title row, or opens it again
        KeysToggle,    // collapses the keyboard panel to its top row, or opens it again
        SlotBar,      // param = slot amount, a = source, b = slot
        SlotRemove,   // a = source, b = slot
        SlotBypass,   // param = the slot's bypass, a = source, b = slot: the dot at the row's start
        SourcePolarity, // a = source: switches it between unipolar and bipolar, for every modulation it has (the
                        // switch after a MODS row's source)
        ModsSource,   // a = source, b = slot: a MODS row's source; a click opens the menu that moves it to another
                      // (an empty modulation's: a = its source or -1, b = emptyModSlot() of it)
        ModsTarget,   // a = source, b = slot: a MODS row's target; a click opens the menu of targets
        ModsAdd,      // the row after the MODS view's last: adds an empty modulation
        ModsScale,    // param = the slot's Amount, a = source, b = slot: a MODS row's Amount bar, dragged across
        ModsCurve,    // param = the slot's curve, a = source, b = slot: a MODS row's curve, dragged up or down
        ModsAux,      // a = source, b = slot: a MODS row's aux, or + Aux; a click opens the menu of auxes
        ModsAuxClear, // a = source, b = slot: the x on a MODS row's aux
        ModsScroll,   // the MODS view's scrollbar: drag the thumb, or click the track to jump there
        CascadeMenu,  // a = 1 on a level of the open cascading menu (b = the level), 0 around it (a click closes it)
        CascadeItem,  // a = level, b = entry: an entry of the open cascading menu
        SourceMods,   // a = source: the button on its card that shows its modulations in the MODS view
        ValueText,    // param, a = source, b = slot: a slot's depth in numbers; double-click types one
        EnvPoint,     // a = envelope (ops, then kOps + source), b = point
        EnvCurve,     // param = curve, a = envelope, b = segment (attack, decay, release): bends it
        FilterPoint,  // a = filter (b = 1: the filter effect in slot a): cutoff and resonance handle
        ModDot,       // param = slot amount, a = source, b = slot: depth handle on a knob
        ModPill,      // a = the target param, b = how many depth handles show before it: "+n" in place of the handles
                      // that don't fit; a click opens the list of the target's modulations
        ModList,      // a = the target param: the open list of every modulation of the target, under its pill
        ModListRow,   // a = source, b = slot: a row of the list; a source's handle dropped on it sets the row's aux
        FxGrip,       // a = effect slot, b = its position in the chain: drag to reorder
        FxPower,      // param = its switch, a = effect slot, b = position: the icon; a click switches it, a drag moves it
        FxMenuButton, // ADD EFFECT: opens the list of effect types
        FxShadow,     // the dashed card after the effects: opens the list of effect types over itself
        FxInsert,     // a = lane, b = item·kFxGapStride + place (see FxSlotAt): an arrow before a card in the row (the
                      // first too, and along a container's band), which opens the list to add an effect there
        SourceShadow, // the dashed card after the sources: opens the list of source types over itself
        SourceMenu,   // a = 1 on the open list of source types, 0 around it (a click there closes it)
        SourceMenuItem, // a = SourceType: an entry of the list, adds a source of that type
        FxMenu,       // a = 1 on the open list, 0 around it (a click there closes it)
        FxAdd,        // a = FxType: an entry of the list, puts that effect at the end of the chain
        FxRemove,     // a = effect slot
        Scrollbar,    // a = ScrollArea: drag the thumb, or click the track to jump there
        DynBand,      // a = effect slot, b = band: a three-band dynamics' band's row; a click picks the band its
                      // knobs set
        DynZone,      // param = the ratio, a = effect slot, b = 2 x band + 0 for its downward zone (right of the Above
                      // line), 1 for its upward one (left of the Below line): drag up and down for the ratio. Linked,
                      // every band's zones set the low band's
        DynGrip,      // param = the threshold's ratio (for Alt and the wheel), a = effect slot, b = 2 x band + 0 for
                      // its Above threshold's line, 1 for its Below one's: drag across for the threshold. Linked,
                      // every band's lines set the low band's
        PeqPoint,     // param = the band's Q (for the wheel), a = effect slot, b = band: frequency and gain handle;
                      // a press picks the band too, and switches it on if it is off
        PeqBand,      // a = effect slot, b = band: a parametric EQ band's chip; a click picks the band its knobs set
        PeqBandOn,    // param = the band's On switch, a = effect slot, b = band: its chip's power symbol and number,
                      // which switch it on and off, and pick it
        LfoDisplay,   // a = lfo: click opens the shape editor of a freeform LFO
        ClipDisplay,  // a = effect slot: the edit button on a drawn curve's display, which opens the big editor
        ListButton,   // param = a choice (an LFO's shape, the shaper's curve, the reverb's algorithm): opens its list
        ListMenu,     // a = 1 on the open list, 0 around it (a click there closes it)
        ListItem,     // param, b = the value it picks: an entry of the list
        PeqTypeButton, // param = the band's type, a = effect slot, b = band: the shape on its chip, which opens the
                       // list of band shapes
        PeqTypeMenu,  // a = 1 on the open list, 0 around it (a click there closes it)
        PeqTypeItem,  // param = the band's type, b = PeqType: an entry of the list
        AmpMic,       // param = the mic's Position, a = effect slot: the mic on an amp's cabinet display, which drags
                      // across the cone (Position) and away from it (Distance)
        ShapeArea,    // a = shape (see shapeEditor_): double-click adds a point
        ShapePoint,   // a = shape, b = point
        ShapeCurve,   // param = curve, a = shape, b = point: bends the segment leaving the point
        PartialArea,  // a = operator, b = 0 for the levels, 1 for the phases: drag across to draw the partials
        PartialReset, // a = operator: back to a plain sine
        PartialEdit,  // a = operator: the edit button left of a Custom wave's keyboard switch; opens its partial
                      // editor, or closes it
        ShapeDone,    // closes the shape editor
        ShapeReset,   // a = shape: back to the shape it starts with
        ShapeGrid,    // a = axis (0 across, 1 up and down), b = -1 or 1 steps the grid, 0 is its count (wheel)
        ShapeCollapse, // a = lfo: its widened card back to a normal one
        ShapeExpand,  // a = lfo: opens its shape in the full-size editor, over the panels
        ShapeWiden,   // a = lfo: a Custom LFO's normal card widened to have its shape, as a click on its display does
        ShapeTool,    // a = lfo, b = ShapeTool: a button in its widened card that changes the whole shape
        ShapeMode,    // a = lfo, b = 0 Points, 1 Steps (what a click on its shape does), 2 Snap (to the grid)
        UndoButton,   // a = 0 undoes, 1 redoes
        Key,          // a = key of the keyboard (0 = the lowest): drag across keys to play them
        Octave,       // a = -1 or 1: shifts the keyboard down or up an octave
        PresetButton, // a = PresetAction
        PresetStep,   // a = -1 or 1: the previous or next preset in the library
        PresetNameBox, // the current preset's name: opens the preset browser
        TempoBox,     // the host's tempo (see Controller::hasTempo()): drag up and down, scroll, or double-click to type
        BrowserBackdrop, // a = 1 on the browser itself, 0 around it (a click there closes it)
        BrowserRow,   // a = preset index: loads it
        BrowserClose,
        BrowserFolder, // opens the preset folder
        SearchField,  // a = Field: a text field (the browser's search, the MODS view's filter, the target menu's
                      // search): click to put the caret there, drag to select, double-click for a word
        SearchClear,  // a = Field: the x in a text field: empties it
        SettingsButton, // the cog next to the preset buttons: opens the settings dialog
        ConfirmBackdrop, // a = 1 on the dialog asking before Init, Load or Save, 0 around it (a click there cancels)
        ConfirmButton,  // a = 1 goes ahead, 0 cancels
        SettingsBackdrop, // a = 1 on the dialog itself, 0 around it (a click there closes it)
        SettingsOption, // b = the oversampling factor it picks
        SettingsMpe,    // b = 1 switches MPE on, 0 off
        SettingsMpeRange, // b = the MPE bend range it picks, in semitones
        SettingsTheme,  // b = the theme it picks
        SettingsHandles, // b = the depth handles' size it picks
        SettingsGlow,   // the slider for how strongly the graphs' curves glow
        SettingsLinks,  // the slider for how strongly the sources' dashed lines are drawn
        SettingsClose,
        ModWheel,     // param = mod wheel: drag or scroll
        BendWheel,    // drag; springs back to center on release
        // The effects graph (see fxgraph.cpp).
        FxMode,       // b = 0 the chain, 1 the graph: the switch at the FX panel's top right
        FxNode,       // a = node: a click shows it in the inspector
        FxSlot,       // a = lane (0 the main line, 1 the high band, 2 the low band, 3 the middle band): a +, which opens
                      // the list of effects
        FxGap,        // a = lane, b = item·kFxGapStride + place (see FxSlotAt): a wire's middle, which opens it too
        FxAddNode,    // b = its bands, 2 or 3: the list's entries after the effects, a split with its join, opened from
                      // the main line
        FxNodeRemove, // a = node: the x on a split's controls, its container's in the row or its card in the graph
        FxNodeDelete, // a = node: the x in a circle at a node's upper right, while the mouse is over the node
        FxWire,       // a = lane, b = item·kFxGapStride + place: along a wire that takes a node at its middle, its +
        FxClear,      // the button at the FX panel's top right that removes every effect, asking first
        SplitPoint,   // param = the crossover, a = split, b = 0 the low one, 1 the high one: its handle on the display
        // A split's controls (see splitControlWidgets()), in its container in the row and on its card in the graph.
        SplitBand,    // a = split, b = band (0 low, 1 high, 2 middle; see View::splitLane()): its tab, which shows that
                      // band
        SplitBands,   // a = split, b = 2 or 3: the BANDS switch's half that splits it in that many bands
        SplitSolo,    // param = a join's solo, a = split, b = lane: the S under its band's level, each band's on its own
        FxBoxGrip,    // a = split's node: its container's title row in the row, which drags it along the line
        FxBoxFold,    // a = split: folds its container to a strip, or unfolds it
        FxBoxAdd,     // a = lane (1 the high band, 2 the low band, 3 the middle band), b = item: a container's dashed
                      // card, for the list
    };

    // Save writes the patch over its preset's file (asking first), and is only on while a preset with a file has
    // changed; Save As picks a file in the dialog, and is off for Init until it changes.
    enum class PresetAction { Init, Load, Save, SaveAs };

    // What a widened LFO card's tools do to its whole shape, each one undo step: flip it upside down, play it back to
    // front, move it a grid column earlier or later (round the cycle), and copy it to paste into another LFO.
    enum class ShapeTool { Invert, Reverse, ShiftLeft, ShiftRight, Copy, Paste };

    struct Widget {
        Kind kind = Kind::Knob;
        Rect rect;
        int param = -1;
        int a = -1;
        int b = -1;
        const char *label = nullptr;
        bool compact = false; // header knob with its label and number beside it
        bool shrunk = false;  // a compact knob drawn smaller (the bend range, in the keyboard strip's top row)
        bool disabled = false; // dimmed, and takes no input (a fixed operator's Pitch and Detune, their depth handles)
        bool faint = false;    // dimmed, but takes input still (an amp's cabinet controls while the cabinet is off)
        Rect clip{};          // on a card that may be scrolled partly out: drawn and hit only within this
        bool popup = false;   // in the open list of a pill (its row's bypass, depth and remove): over everything else
        // One of a row of buttons drawn merged into one pill, as Poly | Mono: the pill, and from where to where this
        // one's part of it reaches (halfway across the gaps to its neighbors). group.w is 0 for a button on its own.
        Rect group{};
        float partLeft = 0.0f, partRight = 0.0f;
        bool modeGlyph = false; // a wire label in the graph showing its route's mode (not Phase) inside it
        bool graph = false;     // a wire's or OUT label in the graph: its ranges above it, its depth handles below

        bool same(const Widget &o) const { return kind == o.kind && param == o.param && a == o.a && b == o.b; }
    };

    enum class DragKind {
        None,
        Value,
        Node,
        Connect,
        Reroute,
        RerouteFrom, // a route by its start, onto another node to come from
        ModSource,
        EnvPoint,
        EnvCurve,
        FilterPoint,
        PeqPoint,
        DynGrip,
        FxMove,
        SourceMove,
        Scroll,
        ShapePoint,
        ShapeCurve,
        ShapeStep, // a step just drawn, following the mouse (drag_.shapeBefore: the shape before it)
        Partials,
        Key,
        ModWheel,
        BendWheel,
        Glow,
        TextSelect, // across a text field (drag_.widget.a: its Field)
        Bend,       // a wire, sideways: how far it bows
        ModsScroll, // the MODS view's thumb
        Pan,        // the middle button: what a panel that scrolls shows, along with the mouse (a = PanArea)
        SplitPoint, // a split's crossover handle, across
        FxNodeMove, // a node of the effects graph (a split or a join with what it holds), onto another place
        Tempo,      // the title bar's tempo, up and down (drag_.startValue: the tempo it started at)
        AmpMic,     // an amp's mic, across and away from its cabinet's cone
    };

    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start;
        Point now;
        double startValue = 0.0;
        bool moved = false;
        bool leftNode = false; // a route being dragged has been well clear of the node it comes from
        bool fine = false;
        bool snap = false; // shape points snap to a grid
        float envScale = 0.0f; // an envelope's zoom, held while one of its handles is dragged
        bool step = false; // an undo step is open until the drag ends (moving nodes or cards)
        float scrollStart = 0.0f; // moving a card: where its row was scrolled to when it was grabbed
        int lastPartial = -1; // drawing partials: the one the last move was over, and its value in startValue
        std::vector<double> shapeBefore; // drawing a step: the shape's params before it
    };

    struct Typing {
        Widget widget;
        std::string text;
        size_t caret = 0;
        bool selected = true; // all of it, as it starts: typing replaces it
    };

    // A line of text being edited in a field: what is typed, the caret in it and where its selection starts (at the
    // caret, nothing is selected), where each place between its characters was painted (across), and how far its text
    // is scrolled left, so the caret shows.
    struct TextField {
        std::string text;
        size_t caret = 0;
        size_t anchor = 0;
        std::vector<float> stops;
        float shift = 0.0f;

        // Its selection, from the anchor to the caret, either way round.
        std::pair<size_t, size_t> selection() const { return std::minmax(caret, anchor); }
        void erase(size_t from, size_t to);
        // Over the selection; line breaks and tabs as spaces, the rest past printable ASCII left out, as much as
        // fits. Whether the text changed.
        bool insert(std::string_view typed);
        void clear();
        // The place between characters nearest `x`, by where they were painted; before they were, its end.
        size_t stopAt(float x) const;
        // Selects the word at `x`, up to the spaces either side.
        void selectWord(float x);
        // Backspace, Delete, the arrows, Home, End and Select All, Shift growing the selection: whether the text
        // changed. Enter, Escape, Up and Down are the field owner's.
        bool key(Key key, bool extend);
        // Its words, in lower case, and whether `text` holds every one of them, whatever its case.
        std::vector<std::string> words() const;
        static bool holds(std::string_view text, const std::vector<std::string> &words);
    };
    enum class Field { Browser, ModsFilter, CascadeSearch };

    struct Assignment {
        int source;
        int slot;
        int target;
        double amount;
        bool bypassed; // kept, but modulating nothing for now
        bool bipolar;  // swings its target both ways, as its source does (see sourceBipolar())
        double scale;  // its Amount, which scales the depth (see SlotAuxParam)
        int aux;       // the source that scales it too, or -1
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
    struct MenuNode;
    struct Derived {
        std::optional<std::vector<int>> chain, sources, sourcesShown;
        std::optional<std::vector<Assignment>> modsRows;
        std::shared_ptr<const MenuNode> cascade; // the open menu's entries (see cascadeTree())
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
    bool isCustomWave(int op) const; // its wave is drawn from partials
    std::string fixedHzText(int op, bool compact) const;
    double norm(int index) const;
    void edit(int index, double value);
    std::vector<Assignment> assignments() const;
    std::vector<int> usedSlots(int source) const;
    // Whether a source does something: it modulates a target, or scales another source's modulation as its aux.
    bool sourceUsed(int source) const;
    void assign(int source, int target);
    void removeSlot(int source, int slot);
    bool slotBypassed(int source, int slot) const;
    void toggleBypass(int source, int slot);
    // Makes a source swing every one of its targets both ways, or one way.
    void setSourcePolarity(int source, bool bipolar);
    // How far a route's wire bows to its left, as a share of the distance it spans: as dragged, or kDefaultWireBend.
    float wireBend(int param) const;
    void setWireBend(int param, float bend); // kDefaultWireBend forgets it
    // The MODS view: every modulation as a row, the sources in the order of assignments(), then the empty ones
    // (UiLayout::emptyMods; one from a source that has rows goes after them instead) and a row that adds one, over the routing panel and the inspector. Its rows scroll: how
    // many are scrolled past as shown (gliding), the most that can be, and scrolling them, gliding or at once; the
    // thumb of its bar.
    bool modsWide() const;
    std::vector<Assignment> modsRows() const;
    std::vector<Assignment> modsRowsNow() const; // modsRows() worked out
    // An empty modulation's row stands in for a slot as emptyModSlot(its place in the list), always below -1;
    // emptyModOf() takes it back, -1 for a slot of a source.
    static constexpr int emptyModSlot(int row) { return -2 - row; }
    static constexpr int emptyModOf(int slot) { return slot <= -2 ? -2 - slot : -1; }
    // An empty modulation's source and target, -1 for none: a source since removed, or a target no longer there,
    // reads as none.
    std::pair<int, int> emptyMod(int row) const;
    // What a row modulates: its slot's target, or an empty modulation's.
    int modsRowTarget(int source, int slot) const;
    float modsFirst() const;
    float modsEnd() const;
    void scrollMods(float first, bool glide);
    // Scrolls the rows at once to `slot`'s row of `source` (an empty modulation's too), with as many of its source's
    // rows above it as fit, and says where it is shown.
    Rect revealModsRow(int source, int slot);
    bool glideMods();
    Rect modsThumb() const;
    void paintMods(Canvas &c);
    // Changing a modulation from its row. Moving it to another source takes a free slot there, keeping its target,
    // depth, polarity (as it is now), Amount, aux and bypass, as one undo step; refused when that source modulates the
    // target already or has no slot free. Changing its target keeps the rest; refused when the source modulates that
    // target already. An aux of -1 takes it away.
    void moveModulation(int source, int slot, int to);
    void retarget(int source, int slot, int target);
    void setAux(int source, int slot, int aux);
    // Adds an empty modulation from `source` (-1 for none yet), at the end of the list or after its source's rows,
    // scrolls to it and opens the menu of what it lacks; refused past kMaxEmptyMods.
    void addEmptyMod(int source = -1);
    // Shows a routing view (the tabs' click), taking the panels back from the shape and partial editors.
    void showView(RoutingView view);
    // The MODS view, its rows scrolled to the source's first; with none there yet, a new empty modulation from it
    // (see a card's SourceMods button).
    void showSourceMods(int source);
    // Picks an empty modulation's source, or its target (-1 leaves it as it is). With both, it becomes a modulation of
    // that source as a handle dropped on the target makes one, and leaves the list; refused, as assign() refuses,
    // the row stays as it was. One undo step. Either way the rows scroll to where it is now, among its source's.
    void fillEmptyMod(int row, int source, int target);
    void removeEmptyMod(int row);
    // The cascading menus of a MODS row: its sources by category (LFOs, envelopes, macros, the keyboard's), the same
    // with None for its aux, and every target by category, only the effects in the chain among them. Each entry is a
    // category (with entries of its own), a heading, or a pick: a source (-1 for None) or a target param.
    enum class CascadeKind { Source, Target, Aux };
    struct MenuNode {
        std::string label;
        std::optional<Color> color; // a dot before the label
        bool heading = false;
        bool enabled = true;
        bool checked = false;  // what the row has now
        bool holds = false;    // a category that has what the row has now somewhere in it
        int pick = -2;         // a source or target; -2 for a category or heading
        std::string note;      // after the label, faint: why it can't be picked
        std::string path;      // before the label, dim: the categories a search found it in
        bool newColumn = false; // starts a column of its own level beside the one before (an operator's ENVELOPE)
        std::vector<MenuNode> children;
    };
    struct Cascade {
        CascadeKind kind = CascadeKind::Source;
        int source = -1, slot = -1; // the row
        Point at;                   // the first level's top left
        std::vector<int> open;      // the entry opened at each level so far
        int picked = 0;             // while its search finds targets: the match Up and Down picked, which Enter takes
    };
    // A target menu has a search atop its first level: while something is typed in it, that level lists the targets
    // whose path (categories, then name) holds every word of it, flat, each with its path, as many as
    // kCascadeMatches, then how many more there are.
    bool cascadeSearches() const { return cascade_ && cascade_->kind == CascadeKind::Target; }
    // Where the search goes in the first level, and so where that level's entries start.
    Rect cascadeSearchRect(Rect level) const;
    MenuNode cascadeTree() const;
    MenuNode cascadeTreeNow() const; // cascadeTree() worked out
    MenuNode cascadeCategories() const; // cascadeTree() before its search narrows it
    // The open menu's levels, from the first: their entries, and where they show.
    std::vector<const MenuNode *> cascadeLevels(const MenuNode &root) const;
    std::vector<Rect> cascadeRects(const MenuNode &root) const;
    // An entry of a level (`depth` of them from the first), in the level's place `at`: down its column, the level's
    // columns side by side, each as wide.
    Rect cascadeItemRect(const MenuNode &level, Rect at, int index, size_t depth) const;
    static int cascadeColumns(const MenuNode &level);
    void openCascade(CascadeKind kind, int source, int slot, Rect from);
    void pickCascade(int level, int index);
    // The keyboard in the target menu's search: Up and Down pick a match, Enter takes it, Escape empties the search,
    // or then closes the menu.
    void cascadeKey(Key key);
    // The MODS view's filter: in its title row, at the right, a magnifier until it is clicked or holds something.
    // While it holds something, only the rows whose source and target names hold every word of it show (and the
    // empty modulations, which have yet to get theirs). Escape empties it, or then lets the keyboard go, as Enter and
    // a click elsewhere do.
    Rect modsFilterRect() const;
    bool modsFilterOpen() const { return modsFiltering_ || !modsFilter_.text.empty(); }
    std::string modsFilterCount() const; // "3 of 13": the modulations it shows, of all
    void modsFilterKey(Key key);
    void paintCascade(Canvas &c);
    // "Bypass LFO 1 modulation", or "Resume ..." for one that is bypassed.
    std::string bypassLabel(int source, int slot) const;
    void connect(int from, int to);
    // Moves the route from `from` into `to` over to `into` (kNodes: the output), with its amount and modulation.
    void reroute(int from, int to, int into);
    // Moves the route from `from` into `to` (kNodes: the output) over to come from `source`, with its amount, mode and
    // modulation.
    void rerouteFrom(int from, int to, int source);
    // The route whose arrowhead or start is being dragged elsewhere, which the graph then draws following the mouse;
    // else -1.
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
    // Whether `node` is heard in the end: it goes out, or feeds or modulates a node that is, by routes open where
    // modulation takes them now (see liveAmount()).
    bool reachesOut(int node) const;
    // How far `node` is let through to the output now: the largest live amount of its routes that lead there (0..1).
    double outLevel(int node) const;
    // Whether a filter's or shaper's own sound gets to the output: it goes out, or into a filter or shaper whose sound
    // does.
    bool boxGoesOut(int node) const;
    // Effects. The FX view takes the place of the routing panel and the inspector.
    bool fxWide() const;
    FxType fxSlotType(int slot) const;
    // The slots that hold an effect, in the line's order (see fxLine()): the main line's, and each split's high band's
    // then its low band's.
    std::vector<int> fxChain() const;
    // Fills an empty slot with a fresh effect of `type`, or a copy of slot `copyOf`, and puts it in the line: a copy
    // right after its original, a new one where the list was opened (see fxSlot_).
    void addEffect(FxType type, int copyOf = -1);
    void removeEffect(int slot);
    // The effects (see fxgraph.cpp): a main line of effects and splits from IN to OUT, each split with a high and a low
    // band of effects (and, split in three, a middle one between), closed by a join (the sum of its number) or, the line's last item, left open into OUT. The FX
    // view shows it as a row of cards, each split a container holding its bands (fxRow()), or as a graph on a fixed
    // grid: two views of one line, which the editor reads from the wires (fxLine()) and writes the wires from
    // (writeFxLine()), so the engine runs what either shows. Nodes are numbered as the graph's params number them
    // (a slot, kFxSlots + split, the output, then the sums); wires come from ports (fxSlotPort(), fxBandPort(), ...).
    struct FxWireRef {
        int node;  // the node it goes into
        int input; // which of that node's inputs
        int port;  // where it comes from
    };
    // A split's bands go by lane: 1 the high one, 2 the low one, 3 the middle one, which only a split in three has.
    struct FxLineItem {
        int node;            // an effect's slot, or a split's node
        std::vector<int> hi; // a split's bands: the effects' slots along each, in order
        std::vector<int> lo;
        bool joined = false; // closed by its join
        std::vector<int> mid;
        bool three = false; // split in three bands, `mid` between `hi` and `lo`
        std::vector<int> &band(int lane) { return lane == 1 ? hi : lane == 3 ? mid : lo; }
        const std::vector<int> &band(int lane) const { return lane == 1 ? hi : lane == 3 ? mid : lo; }
        // Its bands' lanes, top to bottom as the graph has them: the high band, the middle one, the low one.
        std::span<const int> lanes() const {
            static constexpr int kThreeLanes[] = {1, 3, 2}, kTwoLanes[] = {1, 2};
            return three ? std::span<const int>(kThreeLanes) : std::span<const int>(kTwoLanes);
        }
    };
    using FxLine = std::vector<FxLineItem>;
    // Where the grid puts things: each node (row -1 the high band, 0 the main line and a middle band, 1 the low band),
    // each + (lane 0 the main line, 1 the high band, 2 the low band, 3 the middle band) and OUT's column.
    struct FxGridLayout {
        struct Place {
            int node, col, row;
        };
        struct Slot {
            int lane, col, row;
        };
        std::vector<Place> nodes;
        std::vector<Slot> slots;
        int outCol = 1;
    };
    // Where the list of effects puts what it adds when it was opened in the graph: at the end of a lane (a +, `item`
    // -1), or where a wire's middle was clicked: on the main line before item `item`, or on that item's band (lane 1
    // the high one, 2 the low one, 3 the middle one) at `place`. Lane -1: neither, the list opened elsewhere.
    struct FxSlotAt {
        int lane = -1;
        int item = -1;
        int place = 0;
    };
    static constexpr int kFxGapStride = 64; // a band's places, more than it can hold
    // The wires the grid draws: level from `from` to `to` (an arrowhead's base where `arrow`), bending S-shaped from one
    // row to another within a gap between nodes, in `color`, and
    // where a node clicked in at their middle goes (`at`, lane -1 for none).
    struct FxGraphWire {
        Point from, to;
        Color color;
        bool arrow = true;
        FxSlotAt at;
        FxSlotAt drop;             // where a node dropped anywhere on it goes: `at`, or on to a +, a join or OUT its end
        std::optional<Point> plus; // where its + shows, if not at its middle: an empty band's, on the band's row
        int port = -1;             // what it carries: the port it comes from, which lights it as loud as that is
        bool faint = false;        // along a band its join doesn't hear, the other one soloed
        bool bendAtStart = false;  // bending to its row as it leaves (out of a split's band), else as it arrives
        std::vector<Point> path;   // its points, from `from` to `to` (see wirePath()), worked out once
        Point middle;              // where its + shows: `plus`, or on its path halfway across
    };
    // Whether the join of a split on the line lets a band of it through (`lane` 1 the high one, 2 the low one, 3 the
    // middle one): not while another band is soloed and it isn't.
    bool fxBandSoloedOut(const FxLineItem &item, int lane) const;
    // How brightly a port of the effects graph lights what it goes out of and along, 0..1: by its level in the
    // telemetry, in dB from kFxGlowFloorDb up to full scale; 0 while the audio has stopped.
    static constexpr float kFxGlowFloorDb = -48.0f;
    float fxGlow(int port) const;
    // Where the graph's nodes (by node), +s (by lane), IN and OUT show on screen: on their cells (fxPlacesOf()), or,
    // while a node is dragged and until they settle after, gliding there (fxPlacesShown(), see fxGlide_).
    struct FxPlaces {
        std::array<Point, kFxNodes> node{};
        std::array<Point, 4> slot{};
        Point in, out;
    };
    FxPlaces fxPlacesOf(const FxGridLayout &grid, float scroll) const;
    FxPlaces fxPlacesShown(const FxGridLayout &grid, float scroll) const;
    std::vector<FxGraphWire> fxGraphWires(const FxLine &line, const FxPlaces &places) const;
    bool fxWireHit(const Widget &w, Point p) const; // whether `p` is on the wire an FxWire widget stands for
    // The line and the wires hit tests look along, kept while a DerivedScope goes on, as derived_ is: widgets(),
    // hitTest() and paint() ask for them many times over.
    struct FxDerived {
        std::optional<FxLine> line;
        std::optional<std::vector<FxGraphWire>> hitWires;
    };
    mutable FxDerived fxDerived_;
    // Dragging a node: what is dragged (an effect, or a split with its bands and join, by its split or its join),
    // the line without it, where it was taken from and where it would land now, and where its nodes were on screen.
    struct FxDrag {
        FxLineItem item;
        FxLine rest;
        FxSlotAt was, at;
        std::vector<int> nodes;
        std::array<Point, kFxNodes> from{};
        float startPlace = 0.0f, span = 1.0f; // in the row: where its card or container was, and how wide
        bool byMouse = false; // in the row: the mouse has been over a container, and where it lands goes by the mouse
    };
    // The nodes' places as shown, in grid cells (fractions while they glide).
    struct FxGlide {
        bool on = false;
        std::array<Point, kFxNodes> node{};
        std::array<Point, 4> slot{};
        float outCol = 1.0f;
    };
    FxLine fxLineShown() const; // the line, or while a node, card or container is dragged, with it where it would land
    void startFxDrag(int node);
    void retargetFxDrag(Point mouse); // where it would land, for the mouse at `mouse`
    void dropFxNode(bool take);       // let go of: moved there if `take`, in its placeholder's place at once
    bool glideFxGraph();              // a step of the glide, in poll(): whether anything moved
    static FxLine fxLineWith(FxLine rest, const FxLineItem &item, FxSlotAt at);
    // Every effect (and every split and join) gone, one undo step, the modulation of their knobs with them; whether
    // there is any.
    void clearFx();
    bool fxAnyToClear() const;
    bool fxGraph() const;
    bool fxNodeShown(int node) const;
    std::vector<FxWireRef> fxWires() const;
    // The line the wires make, with any effect they leave out put at its end (on the high band while a split is open).
    FxLine fxLine() const;
    FxLine fxLineOfWires() const; // fxLine() worked out
    // Wires the graph as `line` says, the splits and joins it holds in use and the others not, the join's band
    // levels kept, in the undo step the caller holds.
    void writeFxLine(const FxLine &line);
    static FxGridLayout fxGridLayout(const FxLine &line);
    static int fxJoinNode(int split) { return fxSumNode(split); } // a split's join: the sum of the same number
    float fxGraphScroll(const FxGridLayout &grid) const;     // how far the graph is scrolled, held to its width
    float fxGraphScrollMost(const FxGridLayout &grid) const; // how far it scrolls at most
    void revealFxNode(int node);                         // scrolls the graph to show that node
    // The card the effects graph's inspector shows the node in.
    Rect fxInspector(int node) const;
    // How many card places the inspector's card takes: the node picked's (a wide effect's 1.5), else one. The graph's
    // box and its scrollbar take what is left of them (they hide detail::fxGraphArea() and fxGraphScrollTrack() here).
    float fxInspectorSpan() const;
    Rect fxGraphArea() const;
    Rect fxGraphScrollTrack() const;
    // The card of the effect in `slot` where it shows now: in the row, or in the graph's inspector; none while it
    // doesn't show.
    std::optional<Rect> fxCardOf(int slot) const;
    int fxSelectedNode() const; // the node the inspector shows, -1 for none
    // An effect just put in `slot`: where the list was opened (see fxSlot_), or, a copy, after the effect it copies.
    void fxPlaceNew(int slot, int copyOf);
    // A split with its join right after it, its bands empty: where the list was opened on the main line (an arrow in
    // the row, a wire's middle in the graph; see fxSlot_), else at the line's end.
    void addSplit(int bands = 2);
    // A split in two bands or three (see SplitParam::Bands), one undo step. Three: an empty middle band opens between
    // the two, shown, its crossover two octaves above the low one, and its join takes it in at 0 dB. Two: the middle
    // band goes, its effects onto the low band's end (`midTo` 2), the high band's start (1) or removed (0); with
    // effects on it and `midTo` -1, the confirm dialog asks which first.
    void setSplitBands(int split, int bands, int midTo = -1);
    bool splitThree(int split) const; // split in three bands
    // An effect, or a split with its join (a join's x removes its split). A split's bands' effects stay on the main
    // line in its place, high band first (`effects` 0), or go with it, with their modulation (1); with any on its
    // bands, -1 asks which first (kConfirmSplitRemove). One undo step.
    void removeFxNode(int node, int effects = -1);
    // The other bands of the split whose band holds effect `slot`, low to high by lane (none off a band); and the
    // effect moved to the end of one of them (`lane` as FxLineItem's), one undo step.
    std::vector<int> fxOtherBands(int slot) const;
    void moveFxToBand(int slot, int lane);
    // The FX view showing the effects as a graph, or as the row: only the view changes.
    void setFxGraph(bool on);
    // The node of the graph under the mouse, or whose x is (see Kind::FxNodeDelete), -1 for none; and where its x
    // goes, at its upper right.
    int fxHoveredNode() const;
    // The same for the routing graph's nodes, whose x switches the node off (see Kind::NodeDelete).
    int routingHoveredNode() const;
    static Point nodeDeleteAt(int node, Point center);
    static Point fxDeleteAt(int node, Point center);
    // The graph's widgets: its nodes and +s, and the inspector's split or join card.
    void fxGraphWidgets(std::vector<Widget> &list) const;
    // A split's controls on a card's place (`card`): in its container in the row (`row`, where its title drags it and
    // it folds), or as the card of its split or join node picked in the graph. Its title, the band tabs, the
    // crossover's display, the slope, the bands' solos, and the knobs for the low band's level, the crossover and the
    // high band's.
    void splitControlWidgets(std::vector<Widget> &list, int split, Rect card, bool row) const;
    void paintSplitControls(Canvas &c, int split, Rect card, bool row);
    std::optional<Rect> splitCardOf(int split) const; // where a split's controls show now, if they do
    // The band it shows: 1 its high band, 2 its low one, 3 its middle one (FxSlotAt), as its tab says (UiLayout's
    // splitBand: 0 the low band, 1 the high one, 2 the middle one, the low one while it is split in two).
    int splitLane(int split) const;
    void showSplitBand(int split, int band); // 0 its low band, 1 its high one, 2 its middle one
    bool splitFolded(int split) const;
    void setSplitFolded(int split, bool folded);
    // The row (the chain view): the line's items left to right, each an effect's card or a split's container (its
    // controls a card place wide, the cards of the band it shows, then a narrow dashed card adding to that band), in
    // card places from the row's start, as fxCard() takes them less the scroll.
    struct FxRowCard {
        int slot;
        float place, span;
        FxSlotAt at;         // where it is in the line: the main line's item (lane 0), or a place on a band
        bool faint = false; // carried, where it would land (see fxRowShown())
    };
    struct FxRowBox {
        int split, item;
        int lane;          // the band it shows (see splitLane())
        float place, span; // the whole container, or its strip folded
        bool folded;
        float add;         // its dashed card's place
        bool faint = false;
    };
    struct FxRow {
        std::vector<FxRowCard> cards;
        std::vector<FxRowBox> boxes;
        std::vector<float> items; // each main-line item's place, then where the line ends
    };
    // Where a container's controls start: past the room it keeps before it (see kFxBoxEdge), a folded strip at once.
    static float boxColumn(const FxRowBox &box);
    FxRow fxRow(const FxLine &line) const;
    // With a card or container being dragged where it would land, and the cards and containers gliding there (see
    // fxRowGlide_). The one carried goes where it would land at once, and shows faint there until it is let go of.
    FxRow fxRowShown() const;
    FxRow fxRowShownOf(const FxLine &line) const; // the same for `line`, worked out
    // fxRowShown(), kept while a DerivedScope goes on, as fxDerived_ is: every card's display asks for it.
    mutable std::optional<FxRow> fxRowDerived_;
    // The widgets of the effect card being carried in the row, laid out at the place it was picked up from, only for
    // its faint image under the mouse (painted, never hit): it may land on a band its container doesn't show.
    mutable std::vector<Widget> carriedWidgets_;
    // The row's cards (by slot) and containers (by split node) as shown, in card places, and where the line ends: once a
    // card or container is dragged, each glides a step of the way to where it goes every poll(), until they settle after
    // it is let go. A node not shown at the last step shows where it goes at once.
    struct FxRowGlide {
        bool on = false;
        std::array<float, kFxNodes> place{};
        std::array<bool, kFxNodes> known{};
        float end = 0.0f;
        int carried = -1; // the node carried: an effect's slot, or a split's node with what it holds
    };
    void startFxRowGlide(); // from where everything shows now
    bool glideFxRow();      // a step of the glide, in poll(): whether anything moved
    float fxBoxSpan(const FxLineItem &item) const;
    void revealFxRow(int node); // scrolls the row to show the card that holds `node`, else its container
    // Dragging a card or container along the row (DragKind::FxMove): taken out of the line, then where it would land
    // for the mouse at `mouse` (an effect onto the band a container shows too, or another band by its tab), then let
    // go.
    void startRowDrag(int node);
    void retargetRowDrag(Point mouse);
    void dropRowDrag();
    // The mouse on the graph's widgets: a press (true if it was one of them), and a drag going on.
    bool fxGraphPress(const Widget &w, Point p);
    void fxGraphDrag(Point p);
    void paintFxGraph(Canvas &c);
    void paintFxCard(Canvas &c, int slot, Rect card, bool dragged);
    // A split's display on its controls' card (shorter in three bands, to leave room for the crossovers' knobs under
    // it), and a crossover's handle there (`which` 0 the low one, 1 the high one, which only three bands have).
    static Rect splitDisplay(Rect card, bool three);
    Point splitHandle(int split, Rect display, int which = 0) const;
    // How far a band's level at its join lowers its curve: its knob's position, 0 silent .. 1 at 0 dB (by its port's
    // band: 0 low, 1 middle, 2 high).
    double splitBandGain(int split, int band) const;
    // Ctrl+drag in the routing graph: where a node dragged to `want` goes on its grid, the nearest point it fits at
    // clear of the other nodes; and whether the grid's points show now.
    Point routingGridPoint(int node, Point want) const;
    bool routingGridShown() const;
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
    // What scrolls sideways with a scrollbar: the effects chain and the modulation sources, by card places, and the
    // effects graph, by pixels. How long each is, how much of it shows, where it is scrolled to and its thumb.
    enum class ScrollArea { Fx, Sources, FxGraph };
    // What the middle button drags along: those rows, the MODS view's rows or the preset browser's list.
    enum class PanArea { Fx, Sources, Mods, Browser };
    void applyPan(Point p);
    float scrollLength(ScrollArea area) const;
    // While there is room for more, a dashed card after the last effect (a card place wide) and two after the last
    // source (LFO above ENV, square) add one.
    bool fxShadow() const;
    bool sourceShadow() const;
    // That dashed card while it is on screen: the list of source types it opens shuts once the card isn't on screen
    // (see poll()).
    std::optional<Rect> sourceShadowCard() const;
    // Whether the list of source types still has what opened it (+ SOURCE with a place free, or that dashed card on
    // screen), and its top left there.
    bool sourceMenuShown() const;
    Point sourceMenuTopLeft() const;
    // The top left of a list as wide and high as `menu` that opens at the mouse, moved back inside the editor where
    // it would reach past an edge.
    Point menuAtMouse(Point mouse, Rect menu) const;
    float scrollShown(ScrollArea area) const;
    float scrollFirst(ScrollArea area) const;
    void setScroll(ScrollArea area, float first);
    Rect scrollThumb(ScrollArea area) const;
    Rect peqArea(int slot) const;
    Rect fxFilterArea(int slot) const;
    Rect mdDisplay(int slot) const; // a three-band dynamics' display, where its card is now
    // The threshold a three-band dynamics' line `line` (2 x band + 0 for Above, 1 for Below) sets: its band's, or,
    // linked, the low band's.
    int mdThreshold(int slot, int line) const;
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
    // A Custom LFO's card widened to edit its shape in it (see kWideSource), or -1: a click on its display widens it,
    // and it stays so while it is Custom and not folded, also while the full-size editor has its shape. Its card's
    // span, and whether the source's card is the widened one.
    int wideLfo() const;
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
    void paintLfoCard(Canvas &c, float card, int lfo);
    // How to edit a shape in the big editor or a widened LFO card, as Points | Steps and Snap have it.
    std::string shapeHint() const;
    // Whether a widened card's Copy has kept a shape for Paste.
    static bool shapeCopied();
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
    void flipSwitch(int param); // as a click on its switch flips it
    // Draws a step into the shape as `before` holds it (its params from shapeBase() on), or as it is when that is
    // empty. False when there's no room for it.
    bool addShapeStep(int shape, Point p, bool snap, const std::vector<double> &before = {});
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
    // Whether it does anything, as its handle shows: a source once it is used (see sourceUsed()), and an LFO or
    // envelope only while it is on too.
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
    // An LFO's clock mode, as its Free, Anchor and One-Shot make it, and setting them for one.
    LfoClock lfoClockOf(int lfo) const;
    void setLfoClock(int lfo, LfoClock clock);
    // Whether a widget is one of an LFO's clock modes (a Choice on its Free, a = the LFO, b = the mode).
    static bool isLfoClockChoice(const Widget &w) {
        return w.kind == Kind::Choice && sourceFieldOf(w.param) == SourceParam::Free;
    }

    // Geometry.
    std::vector<Widget> widgets() const;
    std::optional<Widget> hitTest(Point p) const;
    Point nodeCenter(int node) const;
    Point nodePlace(int node, Point want, bool *found = nullptr) const;
    void placeSwitchedOn(int node); // moves a node just switched on off the others shown, as part of the edit
    int nodeAt(Point p) const;
    Rect envRect(int env) const;
    static Point knobCenter(const Widget &w);
    // An operator's Ratio, a knob drawn as a 7-segment readout (see kRatioBox).
    static bool isRatioDisplay(const Widget &w);
    // An operator's Ratio, whose depth handles drag on its log curve, an octave every 60 px.
    static bool isRatioTarget(int param);
    static float compactTextX(const Widget &w);
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
    // The FX panel; with `only`, just the row's cards, containers and arrows across from it (a carried container's
    // image), without the panel's header.
    void paintFx(Canvas &c, std::optional<Rect> only = std::nullopt);
    void paintShapeEditor(Canvas &c, int shape);
    void paintClipCurveEditor(Canvas &c, int shape);
    void paintNoteCurveEditor(Canvas &c);
    void paintVelocityCurveEditor(Canvas &c);
    // The editor's grid; `zero` is where the zero line goes, and a centered grid stresses its middle line.
    void paintShapeGrid(Canvas &c, Rect area, double zero, bool centered);
    // Where (x, y) of a shape plotted in `area` goes: x 0..1 across, y -1..1 up, a little short of its edges.
    static Point areaAt(Rect area, double x, double y);
    void paintShapeGridControls(Canvas &c);
    // The partial editor: an operator's Custom wave, over the routing panel while the shape editor isn't.
    int partialEditing() const;
    int partialsShown(int op) const; // how many partials its Custom wave plays, and the editor shows
    void paintPartialEditor(Canvas &c, int op);
    // A shaper's transfer curve, or its drawn curve to edit: an effect slot's or (unit kFxSlots + shaper) a voice shaper's.
    void paintShaperCurve(Canvas &c, Rect r, int unit);
    void paintDisperser(Canvas &c, Rect r, int slot);
    void paintWidener(Canvas &c, Rect r, int slot);
    void paintAmp(Canvas &c, Rect r, int slot);
    void paintAmpCab(Canvas &c, Rect r, int slot);
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
    void paintMultiDyn(Canvas &c, Rect r, int slot);
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
    // A route's label in the graph, from its place as it rests (`idle`): while it or its parts are under the mouse,
    // its list of modes is open or it is dragged, a pill, the mode's part on its left (`mode`, a route into an
    // operator only, its feedback aside) and the x on its right; otherwise the label alone, widened for its mode's
    // glyph when that isn't Phase. `whole` is all of it.
    struct LabelPill {
        Rect depth, mode, remove, whole;
        bool open = false;
        bool glyph = false;
    };
    LabelPill labelPill(int param, int mode, Rect idle) const;
    // The nodes whose routes into operator `op` its INPUTS tab lists, in the graph's order, and that tab's picture.
    std::vector<int> inputsOf(int op) const;
    void paintInputs(Canvas &c, int op);
    bool opTabModulated(int op, InspectorTab tab) const;
    void paintNoiseSlope(Canvas &c, Rect r, int op);
    void paintLfo(Canvas &c, Rect r, int lfo);
    void paintWidget(Canvas &c, const Widget &w);
    // Merges the buttons added to `list` since `from` into one pill, as Poly | Mono, each its part of it.
    static void mergeButtons(std::vector<Widget> &list, size_t from);
    // A button's face: on its own a rounded box, outlined in `color` while lit; merged into a row's pill, its part of
    // the pill (see Widget::group), with a line between it and the next part unless one of them is lit.
    void paintButtonFace(Canvas &c, const Widget &w, Color fill, bool lit, Color color, float radius);
    bool lastPartLit_ = false; // whether the part of a pill painted last was lit, for the line after it
    void paintNode(Canvas &c, const Widget &w);
    void paintKnob(Canvas &c, const Widget &w);
    void paintRatio(Canvas &c, const Widget &w);
    std::string modRangeText(int source, int slot) const;
    void paintModOverlay(Canvas &c, const std::vector<Widget> &list);
    // A route's amount where modulation takes the newest voice right now (0..1), or as set while nothing plays.
    double liveAmount(int param) const;
    // A param's value where modulation takes the newest voice right now, in its range (the macros' with no note), as
    // the effects play it: what their displays draw.
    double liveValue(int param) const;
    // Under a route's label or matrix cell: each modulation's range, as on a knob's ring, and where it is now.
    void paintModRange(Canvas &c, Rect bar, int param);
    // A pill's list: every modulation of its target, a row each, under the pill (or over it, short of room), kept
    // inside the editor. It opens from a click on the pill, or under a source's handle dragged over the pill (then it
    // closes again unless the handle is let go of on a row); a click anywhere else but on a source's handle closes it,
    // and so does the pill going away.
    struct ModListOpen {
        int target = -1;
        Rect anchor; // the pill it opened from
        bool sprung = false; // opened under a dragged handle
    };
    bool modListOn(const Widget &pill) const;
    Rect modListPanel(Rect anchor, int rows, bool aux) const;
    // Whether the pill the open list belongs to still shows; if not, the list closes.
    bool modListPillShown() const;
    void openModList(const Widget &pill, bool sprung);
    void closeGoneLists();
    void paintModPill(Canvas &c, const Widget &w, bool hovered);
    void paintModList(Canvas &c, const Widget &w);
    void paintModListRow(Canvas &c, const Widget &w);
    // Lines from a hovered source handle to whatever it modulates that is on screen; with a `slot`, from the handle
    // (when it is on screen) to that modulation's alone, as when its depth handle is hovered.
    void paintSourceLinks(Canvas &c, const std::vector<Widget> &list, int source, int slot = -1);
    // Dotted lines from a source's handle to the depth handles it is the aux of (on screen), each ringed with dots;
    // with `ofSource` and `ofSlot`, only to that modulation's (as when it is hovered, from its aux's handle).
    void paintAuxLinks(Canvas &c, const std::vector<Widget> &list, int aux, int ofSource = -1, int ofSlot = -1);
    void paintTyping(Canvas &c);
    std::string undoText(bool redo) const;
    void paintTooltip(Canvas &c);
    // The settings dialog: while it is open it takes every click.
    std::vector<Widget> settingsWidgets() const;
    std::vector<Widget> confirmWidgets() const;
    std::string confirmWhat() const;
    bool confirmShorter() const;
    // The preset browser: while it is open it takes every click, and the confirm dialog goes over it.
    std::vector<Widget> browserWidgets() const;
    void paintBrowser(Canvas &c);
    // Its rows: each preset's index and name, with a header (-1 and the subfolder) before each subfolder's. While
    // something is typed in its search, only the presets whose subfolder and name hold every word of it.
    std::vector<std::pair<int, std::string>> browserRows() const;
    // The browser takes the keyboard while it is open: what is typed goes into its search, Up and Down pick a
    // preset from the list, Enter loads it, and Escape empties the search, or then closes the browser.
    void browserKey(Key key);
    // The text fields: the one the keyboard goes to now, if any; each by its Field; what follows a change to what is
    // typed in one; emptying one; and painting one into `box`: a magnifier, then in `typed` the text, its selection
    // and, while it has the keyboard, the caret, or `hint` while it is empty, and `after` faint at its right end.
    std::optional<Field> fieldTyping() const;
    TextField &field(Field f);
    void fieldChanged(Field f);
    void clearField(Field f);
    void paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint, std::string_view after = {});
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
    bool presetActionOn(PresetAction action) const;
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
    std::optional<ModListOpen> modList_;     // a pill's open list
    int shapeEditor_ = -1;                   // the LFO whose shape the routing panel shows, or -1
    int wideLfo_ = -1;                       // the LFO whose card is widened (see wideLfo()), or -1
    int partialEditor_ = -1;                 // the operator whose Custom wave's partials it shows, or -1
    float sourceScroll_ = 0.0f;              // how many source cards are scrolled past, before clamping
    float fxScroll_ = 0.0f;                  // how many effect cards are scrolled past, before clamping
    bool fxMenuOpen_ = false;                // the ADD EFFECT list
    Point fxMenuAt_{-1.0f, -1.0f};           // its top left: at the mouse, or (x < 0) under ADD EFFECT
    FxSlotAt fxSlot_;                        // where in the line the list was opened, if there
    std::optional<FxDrag> fxDrag_;           // a node of the graph, or a card or container of the row, being dragged
    FxGlide fxGlide_;
    FxRowGlide fxRowGlide_;
    float fxGraphScroll_ = 0.0f;             // how far the effects graph is scrolled sideways, before clamping
    int listMenu_ = -1;                      // the param whose list (ListButton) is open, or -1
    int peqMenu_ = -1;                       // the band type param whose list of shapes is open, or -1
    int waveMenu_ = -1;                      // the operator whose list of more waves is open, or -1
    bool sourceMenuOpen_ = false;            // the list of source types, opened from the dashed card after the sources
    Point sourceMenuAt_{0.0f, 0.0f};         // its top left: where the mouse clicked that card; x < 0 under + SOURCE
    std::optional<Cascade> cascade_;         // a MODS row's cascading menu, while it is open
    float modsScroll_ = 0.0f;                // rows the MODS view is scrolled to, before clamping
    float modsGlide_ = 0.0f;                 // and where it shows, gliding after it
    std::array<int, kFxSlots> fxBand_{};     // the band each three-band dynamics' or parametric EQ's card shows
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
    std::array<float, kAllSources * kSlots> macroSlotOffsets_{}; // their slots', as a share of each target's range
    std::optional<double> liveSlotOffset(int source, int slot) const;
    // Where modulation takes param `index` for the newest voice, against its set value, or with no note playing
    // where the macros take it; nothing for the params that can't be modulated, such as the macros' own knobs.
    float liveOffset(int index) const {
        if (index < 0 || index >= kModulatedCount)
            return 0.0f;
        return telemetry_.voices > 0 ? telemetry_.modOffset[index] : macroOffsets_[index];
    }
    bool metering_ = false;                  // the output meter showed something when last polled
    std::vector<Rect> changedAreas_;         // see changedAreas()
    // The scopes' levels, following their spans' peaks (see followScopeLevel()): the output scope's, which its trace is
    // drawn to, and the operator scope's, which its dB readout shows.
    float outScopeLevel_ = 0.0f;
    float opScopeLevel_ = 0.0f;
    float glow_ = 0.0f;                      // how strongly the curves glowed when painted last
    float glowDragged_ = -1.0f;              // the glow slider's amount while it is dragged, else -1
    float shownGlow() const {
        return glowDragged_ >= 0.0f ? glowDragged_ : static_cast<float>(controller_.glow());
    }
    float links_ = 0.0f;                     // how strongly the source's dashed lines were drawn when painted last
    int handles_ = 0;                        // and the depth handles' size
    float linksDragged_ = -1.0f;             // the LINES slider's amount while it is dragged, else -1
    float shownLinks() const {
        return linksDragged_ >= 0.0f ? linksDragged_ : static_cast<float>(controller_.linkIntensity());
    }
    // The amount of the settings slider being dragged (DragKind::Glow): the glow's, or the lines'.
    float &sliderDragged() { return drag_.widget.kind == Kind::SettingsLinks ? linksDragged_ : glowDragged_; }
    bool ctrlHeld_ = false; // see setCtrl()
    int confirm_ = -1; // the PresetAction the confirm dialog asks about, kConfirmFxClear, or -1
    static constexpr int kConfirmFxClear = 4; // asking before every effect goes
    static constexpr int kConfirmMidBand = 5; // asking where a split's middle band's effects go, back in two bands
    static constexpr int kConfirmSplitRemove = 6; // asking whether a split's bands' effects stay, as it is removed
    int confirmSplit_ = -1; // the split kConfirmMidBand or kConfirmSplitRemove asks about
    size_t confirmEffects_ = 0; // how many effects it asks about, counted as it opens
    int pendingPreset_ = -1;                 // the library preset a confirmed Load loads, or -1 for a file
    bool browserOpen_ = false;
    int browserScroll_ = 0;                  // rows scrolled past, before clamping
    TextField browserSearch_;                // what is typed in its search
    TextField modsFilter_;                   // the MODS view's filter
    bool modsFiltering_ = false;             // and whether it has the keyboard
    TextField cascadeSearch_;                // the search atop the open target menu
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
