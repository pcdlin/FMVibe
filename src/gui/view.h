#pragma once

#include "canvas.h"
#include "controller.h"
#include "filterdisplay.h"
#include "fxgraphmodel.h"
#include "fxpanel.h"
#include "inspectorpanel.h"
#include "modspanel.h"
#include "modulationmodel.h"
#include "panel.h"
#include "presetbrowser.h"
#include "routingmodel.h"
#include "routingpanel.h"
#include "seqpanel.h"
#include "shapeeditor.h"
#include "widget.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <string>
#include <vector>

namespace fmvibe::gui {

// The whole editor: layout, drawing and mouse handling in logical coordinates
// (kWidth x kHeight). Only depends on Direct2D, so it can render into a window
// or an offscreen bitmap alike. Its areas that are panels of their own (see panel.h) it holds and hands what is
// theirs, doing for them what it does for every area (PanelHost).
class View : private PanelHost {
public:
    static constexpr float kWidth = 1200.0f;
    // The height depends on whether the modulation panel is collapsed.
    static float heightFor(const UiLayout &layout);
    float height() const override { return heightFor(controller_.layout()); }

    explicit View(Controller &controller);
    ~View(); // ends a drag still going on, so no note hangs and no gesture stays open

    // The color themes, by the names EditorSettings::theme takes; the first is the default.
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
    // The wheel turned sideways (a tilting wheel, a touchpad swiped across): what scrolls sideways under the mouse (the
    // effects' row and graph, the source cards) scrolls `steps` notches on, to the right for more than 0. Nothing
    // turns.
    void wheelAcross(Point p, float steps);
    // The middle button held over a panel that scrolls (the effects, the source cards, the MODS view's rows, the
    // preset browser's list) grabs what it shows: until it goes up, mouseMove() drags that along with the mouse.
    // Elsewhere, or while the left button drags something, it does nothing.
    void middleDown(Point p);
    void middleUp(Point p);
    // Two fingers on a touchscreen or touchpad, at `p` between them: a pan of whatever the middle button would drag
    // there, along with them, until they go up. A drag the first finger started ends first, as cancelDrag() ends it.
    // Whether there was something there to pan; moves return whether a repaint is due.
    bool touchPanBegin(Point p);
    bool touchPanMove(Point p);
    void touchPanEnd(Point p);
    // One finger on a touchscreen (a phone in the browser demo). Landing where nothing scrolls, it is the left button
    // going down there. Over a panel that scrolls, it waits: lifted without moving, it is a click where it landed (with
    // `secondTap`, a double-click's second click); moved further than `slop` (a finger's jitter, in the editor's
    // units), it drags what it landed on if that drags (a knob, a card by
    // its title, a handle, a node), and else pans what scrolls there, as two fingers do. touchMove() returns whether
    // a repaint is due; touchCancel() ends it all as cancelDrag() does (the browser taking the touch for its menu, say).
    void touchDown(Point p, bool secondTap, float slop);
    bool touchMove(Point p);
    void touchUp(Point p);
    void touchCancel();
    std::vector<MenuItem> contextMenu(Point p);
    void menuCommand(int command);
    CursorKind cursor() const;
    // Undo and redo the patch's last edits; not while a drag or typing goes on.
    void undo();
    void redo();
    // The classic algorithms (kAlgorithms in algorithms.h), as the panel of algorithms sets them and lights the one in
    // force: see RoutingModel::applyAlgorithm() and currentAlgorithm().
    static constexpr double kAlgorithmDepth = RoutingModel::kAlgorithmDepth;
    static constexpr double kAlgorithmFeedback = RoutingModel::kAlgorithmFeedback;
    void applyAlgorithm(int index) { routing_.applyAlgorithm(index); }
    int currentAlgorithm() const { return routing_.currentAlgorithm(); }

    // Typing a value, after a double-click on the number a knob or slot shows, or into a text field: the preset
    // browser's search while it is open, the MODS view's filter once clicked, and the search atop a MODS row's target
    // menu while it is open. Meanwhile the window should send the keyboard here: typeChar() takes printable ASCII. Up
    // and Down only move through the browser's list and the target menu's matches.
    using Key = gui::Key;
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
    // A dialog over the editor (a confirm dialog, the settings) takes Enter, which presses its lit button, and Escape,
    // which cancels it or closes the settings; the panel of algorithms takes Escape, which closes it. While one is open
    // the window should send it those keys: true if one was taken. With no dialog open, an open list or menu (a list
    // button's, the list of effects or of source types, a pill's, a MODS row's cascading menu) takes Escape, which
    // closes it: while one is open and nothing is dragged or typed (popupTakesEscape()), the window should send it
    // Escape too.
    bool dialogOpen() const;
    bool popupOpen() const;
    bool popupTakesEscape() const;
    // What the preset browser, the info card and the Save dialog show: whether each is open, the presets the browser
    // lists (in its order) and the one picked (see PresetBrowser).
    bool browserShown() const { return presets_.browserShown(); }
    bool infoCardShown() const { return presets_.infoCardShown(); }
    bool confirmShown() const { return confirm_ >= 0; }
    bool settingsShown() const { return settingsOpen_; }
    bool moreMenuShown() const { return moreMenuOpen_; } // the overflow menu after Init and Save
    int settingsTabShown() const { return settingsTab_; } // detail::kSettingsVisualTab or kSettingsEngineTab
    bool saveShown() const { return presets_.saveShown(); }
    bool saveFolderListShown() const { return presets_.saveFolderListShown(); }
    bool nameMenuShown() const { return presets_.nameMenuShown(); }
    std::vector<int> browserListed() const { return presets_.browserListed(); }
    int browserPicked() const { return presets_.browserPicked(); }
    float browserSideScrolled() const { return presets_.browserSideScrolled(); } // how far its sidebar is scrolled, in px
    std::vector<int> browserCategoryCounted() const { // by its category list's rows
        return presets_.browserCategoryCounted();
    }
    bool dialogKey(Key key);
    // Ctrl going down or up, from the keyboard while the window has it (mouse messages bring it too): while it is held
    // over a graph, its grid's points show, and a node dragged snaps to them. True if a repaint is due.
    bool setCtrl(bool held);
    // The SEQ view's keys, while it shows (the window sends them while the mouse is over the editor): Select All picks
    // every note of the pattern; Left and Right move the notes picked a grid step (a step with the snap off), Up and
    // Down a row (a degree of the scale while folded), with Shift an octave; Delete removes them. Copy keeps the notes
    // picked, Cut keeps and removes them, and Paste puts what was kept down at the step under the mouse on the roll
    // (else where a cut took it from, or after what was copied or pasted last), its pitches as they were. Each edit is
    // an undo step. Whether it took the key: the arrows, Delete, Copy and Cut only while notes are picked, Paste only
    // with notes kept, so the host keeps them otherwise.
    bool seqKey(Key key, bool shift);
    // What the editor tells time by, in seconds on a steady clock (how long a source's handle is held over a tab or a
    // node): a test may hand it a clock of its own.
    using Clock = double (*)();
    void setClock(Clock clock) { clock_ = clock; }
    // How many cards the editor keeps where it last painted them, for the mouse to find (see noteCard()): a test checks
    // that partial paints don't pile them up.
    size_t cardsNoted() const { return cardRects_.size(); }

private:
    using Kind = WidgetKind;

    // Save writes the patch over its preset's file (asking first), and is only on while a preset with a file has
    // changed; Save As picks a file in the dialog, and is off for Init until it changes.
    enum class PresetAction { Init, Load, Save, SaveAs };
    // Init, Load, Save or Save As pressed, on the title bar or in the overflow menu: does nothing while it is greyed
    // out, and asks first where it would throw changes away or overwrite a preset's file.
    void pressPreset(PresetAction action);

    using Widget = gui::Widget;

    enum class DragKind {
        None,
        Value,
        ModSource,
        EnvPoint,
        EnvCurve,
        FilterPoint,
        PeqPoint,
        DynGrip,
        SourceMove,
        Scroll,
        Key,
        ModWheel,
        BendWheel,
        Glow,
        TextSelect, // across a text field (drag_.widget.a: its Field)
        Panel,      // a panel's own (drag_.panel): its moves go to it
        ModListScroll, // a pill's open list's thumb
        Pan,        // the middle button: what a panel that scrolls shows, along with the mouse (a = PanArea)
        Tempo,      // the title bar's tempo, up and down (drag_.startValue: the tempo it started at)
        AmpMic,     // an amp's mic, across and away from its cabinet's cone
        UtilityHandle, // a handle on the utility's display: Gain and Mono Bass up and down, Pan and Width across
    };

    // What every drag has (where it started and is now, whether it moved, the modifiers, its undo step: see DragState,
    // which a panel's drag is handed), and what the View's own keep.
    struct Drag : DragState {
        DragKind kind = DragKind::None;
        Widget widget;
        double startValue = 0.0;
        bool fine = false;
        float scrollStart = 0.0f; // moving a card: where its row was scrolled to when it was grabbed
        // A card or node Ctrl-dragged, which carries a copy (see beginCopyDrag()): every param and the layout before the
        // copy was made, which a drag called off goes back to.
        std::vector<double> copyBefore;
        std::optional<UiLayout> copyLayout;
        // The panel whose drag (DragKind::Panel) or pan (PanArea::Panel) it is.
        Panel *panel = nullptr;
    };

    struct Typing {
        Widget widget;
        std::string text;
        size_t caret = 0;
        bool selected = true; // all of it, as it starts: typing replaces it
    };

    using TextField = gui::TextField;
    using Field = gui::Field;

    using Assignment = gui::Assignment; // see modulationmodel.h

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
        std::optional<std::vector<int>> chain, sources, sourcesShown;
        std::optional<float> sourcesLength;
        std::array<std::optional<std::vector<int>>, kAllSources> slots{};
        std::array<std::optional<float>, kMaxSources> cards{};
        std::optional<ModTable> mods;
        std::optional<std::vector<Widget>> widgets;
    };
    ModTable modTable() const;
    // Calls f(m) for each assignment of `target`, in assignments() order.
    template <class F>
    void forEachMod(int target, F &&f) const;
    // The widgets, built once while a scope goes on: paint() and hitTest() ask, each in a scope of its own.
    const std::vector<Widget> &scopedWidgets() const;

    // Model access: the patch's params and layout go through the models (see patchedits.h): the modulation's
    // (mods_), the routing graph's (routing_) and the effects graph's (fx_). What is here reads them for the screen,
    // keeps what a paint or hit test derives from them (see DerivedScope), and does what the editor's own state needs
    // around their edits.
    double value(int index) const { return mods_.value(index); }
    double norm(int index) const { return mods_.norm(index); }
    void edit(int index, double value) { mods_.edit(index, value); }
    // Several params changed as one gesture.
    void editMany(const std::vector<std::pair<int, double>> &changes) { mods_.editMany(changes); }
    // ModulationModel's, from the sources as sources() keeps them; and a source's slots, kept while a scope goes on.
    std::vector<Assignment> assignments() const;
    std::vector<int> usedSlots(int source) const;
    // Whether a source does something: it modulates a target, or scales another source's modulation as its aux.
    bool sourceUsed(int source) const;
    // Shows a routing view (the tabs' click), taking the panels back from the shape and partial editors.
    void showView(RoutingView view);
    // The MODS view, its rows scrolled to the source's first; with none there yet, a new empty modulation from it
    // (see a card's SourceMods button, and ModsPanel::showSource()).
    void showSourceMods(int source);
    // "Bypass LFO 1 modulation", or "Resume ..." for one that is bypassed.
    std::string bypassLabel(int source, int slot) const;
    // Whether a source's handle let go of on `w` modulates what it shows: a knob, a route's label or matrix cell, or
    // a depth bar, as the drag's overlay rings them; never the source's own output (a macro's or the mod wheel's
    // knob), nor a macro's knob that would loop (see ModulationModel::canModulate()).
    bool droppable(const Widget &w, int source) const;
    // The color of the effect in `slot`: each type of effect has its own.
    Color fxColor(int slot) const;
    // A list's color (its effect's, the voice shapers' or the filters'), and whether it can pick `item`: a band-pass's
    // or notch's slope list can't take 6 or 18 dB.
    Color listColor(int param) const;
    bool listItemFits(int param, int item) const;
    // The entry a list shows as picked: its value, or for a slope the one that plays.
    int listShown(int param) const;
    // Steps a list's choice to the entry before (dir -1) or after (1) the one shown, round its ends (its button's
    // arrows).
    void stepList(int param, int dir, bool model = false);
    // Effects (see FxPanel). The FX view takes the place of the routing panel and the inspector.
    bool fxWide() const;
    // The slots that hold an effect, in the line's order (see FxGraphModel::chain()), kept while a scope goes on.
    std::vector<int> fxChain() const;
    // Whether the FX view shows the effects as a graph, and where an effect's card shows now, if it does (see FxPanel).
    bool fxGraph() const { return fxPanel_.graph(); }
    std::optional<Rect> fxCardOf(int slot) const { return fxPanel_.cardOf(slot); }
    // What an effect's card holds below its title row, and the card itself: its frame, title and display (see
    // PanelHost::cardWidgets() and paintCard()).
    void effectCardWidgets(std::vector<Widget> &list, int slot, Rect card) const;
    void paintFxCard(Canvas &c, int slot, Rect card, bool dragged);
    // Ctrl held as a source card starts to move: what is carried becomes a copy of it, the original staying where it
    // is, made as the drag's undo step opens (a source into a free place; a panel's cards make their own, see
    // PanelHost::beginCopyDrag()). Too few free, it stays a move, with a message.
    void copyDragged();
    // The keyboard strip's keys lit as they are held (see poll()): each eases up to lit and back down a step every
    // poll, rather than switching (see kKeyLightRise), by MIDI key, 0..1.
    std::array<float, 128> keyLight_{};
    // Cards folded to a strip, which take fewer places: kFoldedFx or kFoldedSource of one.
    bool fxFolded(int slot) const;
    bool sourceFolded(int source) const;
    float sourceSpan(int source) const;
    float sourceOffset(const std::vector<int> &shown, size_t index) const;
    void setFolded(bool fx, int index, bool folded); // see PatchEdits::setFolded(), and wideLfo()
    // What scrolls sideways with a scrollbar: the effects chain and the modulation sources, by card places, and the
    // effects graph, by pixels. How long each is, how much of it shows, where it is scrolled to and its thumb.
    enum class ScrollArea { Fx, Sources, FxGraph };
    // What the middle button drags along: the source cards, a pill's list's, or a panel's own part (drag_.panel,
    // drag_.widget.b its part): the preset browser's list or sidebar, the SEQ view's roll, the MODS view's rows, the FX
    // view's row and graph or the I/O tab's rows, say.
    enum class PanArea { Sources, Panel, ModList };
    void applyPan(Point p);
    // What the middle button (or a finger) would pan at `p`, and where it is scrolled to now; none over a dialog, an
    // open list or menu that doesn't scroll, or while a drag goes on.
    struct PanStart {
        PanArea area;
        float first;
        Panel *panel = nullptr; // PanArea::Panel: which, and its part
        int part = -1;
    };
    std::optional<PanStart> panAt(Point p) const;
    // A finger landed over a panel that scrolls, waiting to see whether it taps, drags or pans (see touchDown()).
    struct TouchPress {
        Point at;
        bool second = false;
        float slop = 0.0f;
    };
    std::optional<TouchPress> touchPress_;
    // What a finger moving on it drags rather than pans past: knobs, handles, cards' titles, nodes and the like.
    bool dragsOnTouch(const Widget &w) const;
    float scrollLength(ScrollArea area) const;
    // While there is room for more, two dashed cards after the last source (LFO above ENV, square) add one (and one
    // after the last effect, see FxPanel).
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
    // The drawn shapes' editors (see ShapeEditor, shapes_): the shape the full-size editor shows, the LFO whose card
    // is widened and the operator whose partials show, or -1.
    int shapeEditing() const { return shapes_.shapeEditing(); }
    int wideLfo() const { return shapes_.wideLfo(); }
    int partialEditing() const { return shapes_.partialEditing(); }
    // The FX view, the shaper's curve editor that opens from it, or an LFO's shape editor takes the inspector's
    // place (see InspectorPanel::covered()).
    bool inspectorCovered() const;
    // The SEQ view reaches down over the modulation panel too: while it shows, the source cards and the panel's title
    // row are hidden (a full-size editor over the panels' row leaves them in view).
    bool sourcesHidden() const;
    // The band the panels' row keeps to: kPanelsBand, or kSeqBand while the SEQ view shows.
    Rect panelsBand() const;
    void flipSwitch(int param); // as a click on its switch flips it
    // While an LFO's or envelope's handle, or a card of the row, is dragged near either end of a row of cards, the
    // row scrolls that way.
    bool edgeScroll();
    // A source's handle held over a view's tab, an operator page's tab, or over a node's tab in the inspector or the
    // node or its name in the routing graph or the matrix, for kDwellSeconds shows that view or tab, or that node in
    // the inspector: what dwellTarget() finds under it (a node's edge as its node), and once it has stayed there long
    // enough, dwellStep() shows it, once, whether the mouse moves or a poll comes. Whether it showed something.
    static constexpr double kDwellSeconds = 0.5;
    std::optional<Widget> dwellTarget(const std::optional<Widget> &at) const;
    bool dwellStep();
    void flash(std::string message) override;
    // The MIDI note under the keyboard's first key.
    int keyboardBase() const;
    // The MIDI velocity a key (its rect) struck at `p` plays at: 1 at its top, 127 at its bottom.
    static int keyVelocity(const Rect &key, Point p);
    // Modulation sources. The places that hold one, in the order they show (kept while a scope goes on); shown,
    // with the card being dragged already moved to where it would land.
    std::vector<int> sources() const;
    std::vector<int> sourcesShown() const;
    std::vector<int> sourcesMoved() const; // sourcesShown() worked out
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
    // ModulationModel's, scrolled to show the source added, its card widened for a new Curve source; and the shape
    // editor and widened card the place had let go.
    int addSource(SourceType type, int copyOf = -1); // the place it fills, or -1 with none free
    void removeSource(int source);
    // Where an LFO of the newest voice is now, and its shape there; with no voice, a free or anchored one's shared
    // clock. What it puts out is lfoLevelNow() (see PanelHost).
    double lfoPhaseNow(int lfo) const;
    double lfoValueNow(int lfo) const;
    // Whether an LFO's dot shows where it is: while a note plays, or, free or anchored, while none does too, and
    // while it modulates the effects as they ring out after the last note.
    bool lfoRunning(int lfo) const;
    static bool lfoInTail(const Telemetry &t, int lfo);
    // Whether an LFO runs on with no note playing and shows it: switched on, free or anchored.
    bool lfoRunsIdle(int lfo) const;
    // Whether notes start an LFO at its Phase: not while it is free (and not a one-shot), nor Random.
    bool lfoPhaseUsed(int lfo) const;
    // An LFO's clock mode, as its Free, Anchor and One-Shot make it, and setting them for one.
    LfoClock lfoClockOf(int lfo) const;
    // How many clock modes its card offers: a random one only Retrigger and Free.
    int lfoClockCount(int lfo) const;
    // How many entries a list offers: a clock list as many as its source's clock modes (see lfoClockCount()).
    int listItems(int param, int shown = 0, bool model = false) const;
    void setLfoClock(int lfo, LfoClock clock);
    // An operator's ADSR | Curve, picked by a click or the wheel: its Curve drawn from its AHDSR the first time (see
    // ShapeEditor::drawEnvelope()), its AHDSR shutting its drawn envelope's editor.
    void pickEnvMode(int op, bool curve);
    // What a list's entry (or its arrows, or the wheel) picks: its param's value, or a Curve's clock mode through
    // setLfoClock() (see detail::isCurveClock()).
    void pickListValue(int param, int to);
    // Whether a widget is one of an LFO's clock modes (a Choice on its Free, a = the LFO, b = the mode).
    static bool isLfoClockChoice(const Widget &w) {
        return w.kind == Kind::Choice && sourceFieldOf(w.param) == SourceParam::Free;
    }

    // Geometry.
    std::vector<Widget> widgets() const;
    // An envelope's handles (an operator's, kOps + a source's for a mod envelope), where its display shows it: the
    // operator's for its page's ENVELOPE tab, which the inspector lists, a mod envelope's on its card.
    void envelopeWidgets(std::vector<Widget> &list, int env) const override;
    std::optional<Widget> hitTest(Point p) const;
    Rect envRect(int env) const;
    static Point knobCenter(const Widget &w);
    // An operator's Ratio, a knob drawn as a 7-segment readout (see kRatioBox).
    static bool isRatioDisplay(const Widget &w);
    // An operator's Ratio, whose depth handles drag on its log curve, an octave every 60 px.
    static bool isRatioTarget(int param);
    // A knob whose value moves only in whole steps (a count, semitones, a note, a note value, a factor), drawn as a
    // stepper: its number in a field with - and + at its ends and its steps along its foot (see Widget::well).
    static bool isStepper(const Widget &w);
    static bool isStepperParam(int param);
    // The stepper's - (-1) or + (1) under `p`, or 0.
    static int stepperButton(const Widget &w, Point p);
    // The value a notch of the wheel takes `param` to, `whole` notches up (down when negative): a step, and for the
    // voice limit about a fifth more or less.
    double notchValue(int param, int whole) const;
    static float compactTextX(const Widget &w);
    static float knobRadius(const Widget &w);
    // Whether `p` is on the number widget `w` shows, where a double-click starts typing.
    static bool onValue(const Widget &w, Point p);
    void startTyping(const Widget &w);
    // A param's value as the editor shows it, formatParam()'s, but a Curve's Rate as its curve's length while its
    // places read as times (see ModulationModel::curveTime()); and what typing it takes back: a length there ("1.2 s",
    // "800 ms", or a rate given in Hz), else what parseParam() takes.
    std::string shownValue(int index, double v) const;
    bool parseShown(int index, const std::string &text, double *out) const;
    // Where the value being typed shows, over the number it replaces.
    Rect typingField() const;
    // Pixels per unit of normalized time: what fills the display, or while its zoom is held (see envZoom_), the held
    // zoom, as long as the envelope still fits.
    float envScale(int env) const;
    // What fills the display with the envelope's times as long as modulation can take them, so that its curve stays
    // inside as it moves, on a time axis that stands still.
    float envFit(int env) const;
    // The envelope whose attack, hold, decay or release param `index` is, or -1.
    static int envOfTime(int index);
    // A drag holding an envelope's zoom: one of its handles, or one of its times' knobs.
    int envDragged() const;
    // The held zoom goes back to filling the display (see envZoom_) once the mouse is at `p`, clear of it.
    void envZoomLeft(Point p);
    // Each poll: a dragged handle pushed past where the envelope's end meets the display's zooms it out, its segment
    // growing under the mouse; a zoom let go eases back to filling the display. Whether either moved it.
    bool envZoomStep();
    void applyDrag(Point p);
    // Ends the drag going on: its gestures and undo step, a key it plays, the bend wheel it holds. When the button
    // goes up, after what letting go does; or when its button-up never came, at the next press.
    void finishDrag();
    // Whole notches of the wheel on `target` (a param, or another of the wheel's targets), with `steps` added: a
    // trackpad's smaller steps add up toward them while the wheel stays on that target.
    int wheelNotches(int target, float steps) override;

    // Painting.
    void paintPanels(Canvas &c);
    void paintHeader(Canvas &c);
    void paintKeyboard(Canvas &c);
    void paintKey(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintWheel(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintDisperser(Canvas &c, Rect r, int slot);
    void paintWidener(Canvas &c, Rect r, int slot);
    void paintUtility(Canvas &c, Rect r, int slot);
    void paintLoFi(Canvas &c, Rect r, int slot);
    // Where the utility's handle `handle` sits on its display `r`: at the params' values, or (`live`) where modulation
    // has them now.
    Point utilityHandlePoint(Rect r, int handle, bool live, int slot) const;
    void paintAmp(Canvas &c, Rect r, int slot);
    void paintAmpCab(Canvas &c, Rect r, int slot);
    // The note source in the keyboard strip, right of the macros: the note the newest voice plays, and its name; and
    // velocity, right of it: the velocity the newest voice came with.
    void paintNoteSource(Canvas &c, float top);
    void paintVelocitySource(Canvas &c, float top);
    void paintDelay(Canvas &c, Rect r, int slot);
    void paintEq(Canvas &c, Rect r, int slot);
    void paintParamEq(Canvas &c, Rect r, int slot);
    // Where a parametric EQ band's handle sits: at its gain, or for a cut, at its resonance's peak.
    Point peqHandle(Rect area, int slot, int band) const;
    void paintCompressor(Canvas &c, Rect r, int slot);
    void paintMultiDyn(Canvas &c, Rect r, int slot);
    void paintReverb(Canvas &c, Rect r, int slot);
    // The phaser's and flanger's responses, with the LFO at the middle and at both ends.
    void paintSweep(Canvas &c, Rect r, int slot, FxType type);
    void paintChorus(Canvas &c, Rect r, int slot);
    void paintShifter(Canvas &c, Rect r, int slot);
    void paintSources(Canvas &c);
    void paintSource(Canvas &c, float card, int source);
    void paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live) override;
    // The values of the envelope handle `w` (hovered or dragged), at its display's top right over everything else.
    void paintEnvReadout(Canvas &c, const Widget &w);
    // What a filter's display reads (see filterdisplay.h): the params, where modulation takes them now and how far.
    detail::FilterReads filterReads() const;
    void paintLfo(Canvas &c, Rect r, int lfo);
    void paintWidget(Canvas &c, const Widget &w) override;
    // A button's face: on its own a rounded box, outlined in `color` while lit; merged into a row's pill, its part of
    // the pill (see Widget::group), with a line between it and the next part unless one of them is lit.
    void paintButtonFace(Canvas &c, const Widget &w, bool lit, bool hot, Color color, float tint, float radius) override;
    bool lastPartLit_ = false; // whether the part of a pill painted last was lit, for the line after it
    void paintKnob(Canvas &c, const Widget &w);
    void paintRatio(Canvas &c, const Widget &w);
    void paintStepper(Canvas &c, const Widget &w);
    Color knobColor(int param) const;
    std::string modRangeText(int source, int slot) const;
    void paintModOverlay(Canvas &c, const std::vector<Widget> &list);
    // A route's amount where modulation takes the newest voice right now (0..1), or as set while nothing plays.
    double liveAmount(int param) const;
    // A param's value where modulation takes the newest voice right now, in its range (the macros' with no note), as
    // the effects play it: what their displays draw.
    double liveValue(int param) const override;
    // Under a route's label or matrix cell: each modulation's range, as on a knob's ring, and where it is now.
    void paintModRange(Canvas &c, Rect bar, int param) override;
    float modulatedBy(int param) const override { return liveOffset(param); }
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
    // It shows kModListRows rows at most; past that, they scroll (the wheel, the middle button, the bar at its right),
    // and the panel is that much wider for the bar.
    Rect modListPanel(Rect anchor, int rows, bool aux) const;
    int modListRowCount() const;
    float modListEnd() const;
    float modListFirst() const;
    Rect modListScrollTrack(Rect panel) const;
    Rect modListThumb(Rect track) const;
    // Whether the pill the open list belongs to still shows; if not, the list closes.
    bool modListPillShown() const;
    void openModList(const Widget &pill, bool sprung);
    // Every open list and menu closing, as a click around it closes it (see popupOpen()).
    void closeMenus();
    void paintModPill(Canvas &c, const Widget &w, bool hovered);
    void paintModList(Canvas &c, const Widget &w);
    void paintModListRow(Canvas &c, const Widget &w);
    // An aux's ring round a depth handle, as much of it as `depth` (0..1) (the bar along an aux's chip's foot is
    // paintAuxDepthBar(), theme.h).
    void paintAuxRing(Canvas &c, Point center, float radius, int aux, double depth) const;
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
    // Whether the theme picked tints its cards with tinted cards on (Spectrum, Porcelain): the settings' CARDS is
    // disabled in the others.
    bool cardsTintable() const;
    std::vector<Widget> confirmWidgets() const;
    // The confirm dialog's button `button` pressed (0 Cancel, 1 the way ahead, 2 to 4 the bands' and the split's others).
    void answerConfirm(int button);
    std::string confirmWhat() const;
    bool confirmShorter() const;
    // The text fields: the one the keyboard goes to now, if any; each by its Field (the preset browser's its own, see
    // Panel::field()); what follows a change to what is typed in one; emptying one; and painting one into `box`: a
    // magnifier, then in `typed` the text, its selection and, while it has the keyboard, the caret, or `hint` while it
    // is empty, and `after` faint at its right end.
    std::optional<Field> fieldTyping() const;
    TextField &field(Field f);
    void fieldChanged(Field f);
    void clearField(Field f) override;
    void paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint, std::string_view after = {},
                    bool lens = true) override;
    // The preset bar's Init, Load, Save and Save As, and its arrows (its info and what opens from it are the preset
    // browser's: see presets_).
    void stepPreset(int direction);
    bool presetActionOn(PresetAction action) const;
    void runPreset(PresetAction action);
    void paintSettings(Canvas &c);
    void paintConfirm(Canvas &c);
    void paintHoverTags(Canvas &c, const std::vector<Widget> &list);

    bool isHovered(const Widget &w) const override;
    bool isActive(const Widget &w) const override;
    // What the View does for its panels, the rest of PanelHost being the View's own methods above.
    Controller &controller() const override { return controller_; }
    ModulationModel &modulationModel() override { return mods_; }
    RoutingModel &routingModel() override { return routing_; }
    FxGraphModel &fxGraphModel() override { return fx_; }
    void resetHover() override { hover_.reset(); }
    void askConfirm(Panel &from, int kind) override;
    void startDrag(Panel &from, const Widget &w, Point p, bool step) override;
    void dragValue(const Widget &w, Point p, Modifiers mods) override;
    std::optional<Widget> dragged() const override {
        return drag_.kind != DragKind::None ? std::optional(drag_.widget) : std::nullopt;
    }
    bool ctrlHeld() const override { return ctrlHeld_; }
    bool sendKey(Key key, bool shift) override { return seqKey(key, shift); }
    bool dragging() const override { return drag_.kind != DragKind::None; }
    void beginCopyDrag() override;
    void cancelCopyDrag() override;
    std::optional<Widget> widgetAt(Point p) const override { return hitTest(p); }
    void closeGoneLists() override;
    void shapeGone(int shape) override { shapes_.shapeGone(shape); }
    void noteCard(int id, Rect card, Rect shown) override;
    bool cardFoldShown(int card) const override;
    void paintCardFace(Canvas &c, Rect card, Color color, bool on, int id, bool dragged, float edge,
                       Rect shown) override;
    void cardWidgets(std::vector<Widget> &list, int card, Rect r) const override {
        effectCardWidgets(list, card - kMaxSources, r);
    }
    void paintCard(Canvas &c, int card, Rect r, bool dragged) override { paintFxCard(c, card - kMaxSources, r, dragged); }
    void paintCardLanding(Canvas &c, Rect landing, Rect row, float radius, Color color) override;
    void paintCarriedCard(Canvas &c, Rect landing, Rect start, Rect row, float radius, Color color,
                          const std::function<void()> &paintCard, const std::vector<Widget> &widgets) override;
    void paintBackdrop(Canvas &c, Rect area, Point out) override { routingPanel_.paintFxBackdrop(c, area, out); }
    bool scoped() const override { return derivedDepth_ > 0; }
    const Telemetry &telemetry() const override { return telemetry_; }
    Point mouse() const override { return hoverPoint_; }
    double now() const override { return clock_(); }
    const std::optional<Widget> &hovered() const override { return hover_; }
    bool shapeEditorOpen() const override { return shapeEditing() >= 0; }
    bool partialEditorOpen() const override { return partialEditing() >= 0; }
    void clearShapePick() override { shapes_.clearShapePick(); }
    Rect shapeHome(int shape) const override;
    void revealCard(int card) override { revealSource(card); }
    std::optional<std::pair<double, double>> lfoNow(int lfo) const override;
    double lfoLevelNow(int lfo) const override;
    std::vector<Widget> listedWidgets() const override { return widgets(); }
    int openList() const override { return listMenu_; }
    bool openListModel() const override { return listMenu_ >= 0 && listModel_; }
    bool openListInEditor() const override { return listMenu_ >= 0 && listEditor_; }
    void showList(int param) override {
        listMenu_ = param;
        listModel_ = false;
        listEditor_ = false;
    }
    // Whether `w` (a list's button or arrow) is of the list open: a filter's Model list or its type's, a Curve's clock
    // list on its card or in its full-size editor.
    bool listOpenFor(const Widget &w) const;

    Controller &controller_;
    // The preset library and the patch as a file (see Controller::presetStore()).
    PresetStore &presetStore() const { return controller_.presetStore(); }
    // The patch's models, each telling the editor why it refused an edit (see flash()); the panels edit through them
    // too (see PanelHost::modulationModel()), so they come first.
    ModulationModel mods_;
    RoutingModel routing_;
    FxGraphModel fx_;
    // The panels: the shape editors, the preset browser, the SEQ view, the MODS view, the FX view, the routing panel,
    // the inspector, and all of them, in the order they are handed what goes on (the shape editors first, for the keys
    // they take before the others).
    ShapeEditor shapes_;
    PresetBrowser presets_;
    SeqPanel seq_;
    ModsPanel modsPanel_;
    FxPanel fxPanel_;
    RoutingPanel routingPanel_;
    InspectorPanel inspector_;
    std::vector<Panel *> panels_;
    // The one showing over the editor (see Panel::modal()), if any.
    Panel *modalPanel() const;
    // The tooltip of a panel's own widget, if any (see Panel::tooltip()).
    std::string panelTip(const Widget &w) const;
    const ParamTable &table_;
    ParamValues seen_{};
    uint64_t seenStamp_ = 0; // the controller's valuesStamp() when seen_ was taken (0: not yet)
    Telemetry telemetry_{};
    // Where the synth's tuning comes from, as poll() last asked (none while MTS-ESP is off): the title bar's MTS mark
    // and the settings' TUNING show it.
    TuningStatus tuning_{};
    std::optional<Widget> hover_;
    Point hoverPoint_; // where the mouse is
    // Where the effect and source cards were last painted, as much of each as shows (a source by its number, an effect
    // by kMaxSources + its slot), in the order they were painted, each with the paint that noted it, and the one the
    // mouse is over (-1 for none): a theme whose cards go without an outline outlines that one alone. A card noted
    // again by a later paint takes the place of where an earlier one noted it, so partial paints don't pile them up.
    struct NotedCard {
        int id;
        Rect rect;
        uint32_t paint;
    };
    std::vector<NotedCard> cardRects_;
    uint32_t cardPaint_ = 0; // counts the paints, for cardRects_
    int hoverCard_ = -1;
    bool touched_ = false;   // a finger has touched the editor: the cards' folds show without a mouse over them
    bool mouseOver_ = false; // the mouse is over the editor (since it last moved there, until it left)
    int cardAt(Point p) const;
    bool cardOutlined(int id, bool dragged) const;
    // The switched-off cards' bodies below their title rows, and the inspector's page of a switched-off node: what is in
    // them is dimmed. A card's displays under an overlay of its own face (`face`, within `clip`), which leaves its
    // face as it is; the page under a layer (paintInspector(), no face here); the widgets in both under a layer.
    struct DimArea {
        Rect area;
        Color face;
        Rect clip;
    };
    std::vector<DimArea> dimmedAreas() const;
    bool knobIdle(const Widget &w) const;
    bool widgetDimmed(const Widget &w, const std::vector<DimArea> &areas) const;
    Drag drag_;
    // An envelope's zoom, held from when one of its handles or one of its times' knobs is grabbed: while the drag goes
    // on, and after it while the mouse stays over its display, so the handles stay where they were put. Once the mouse
    // is clear of it, the zoom eases back to filling the display (`easing`). Only ever as far in as fits the envelope
    // (see envScale()). env -1: none held.
    struct EnvZoom {
        int env = -1;
        float scale = 0.0f;
        bool easing = false;
    };
    EnvZoom envZoom_;
    // What the last press hit, as it went down, and where: a double-click after it goes with it.
    std::optional<Widget> pressed_;
    Point pressPoint_{-1000.0f, -1000.0f};
    int wheelTarget_ = -1; // what the wheel turned last (see wheelNotches()), and its steps short of a notch
    float wheelRest_ = 0.0f;
    // The open context menu's targets (below) still are what it opened on: no undo, redo or wheel since. The web
    // demo's menu stays open while those reach the editor.
    bool menuLive_ = false;
    int menuParam_ = -1;
    int menuSource_ = -1;                    // the source of the open context menu
    std::optional<Widget> menuAuxDepth_;     // what the open context menu's "Edit aux depth" types into
    std::optional<Widget> menuValue_;        // what the open context menu's "Edit value" types into
    std::optional<ModListOpen> modList_;     // a pill's open list
    float sourceScroll_ = 0.0f;              // how many source cards are scrolled past, before clamping
    int listMenu_ = -1;                      // the param whose list (ListButton) is open, or -1
    bool listModel_ = false;                 // and whether it is a filter's Model list (see kModelList)
    bool listEditor_ = false;                // or opened from a full-size shape editor's title row (see kEditorList)
    int peqMenu_ = -1;                       // the band type param whose list of shapes is open, or -1
    bool sourceMenuOpen_ = false;            // the list of source types, opened from the dashed card after the sources
    Point sourceMenuAt_{0.0f, 0.0f};         // its top left: where the mouse clicked that card; x < 0 under + SOURCE
    float modListScroll_ = 0.0f;             // rows a pill's open list is scrolled to, before clamping
    int octave_ = 0;                         // the keyboard's shift, -2..2 octaves from C1
    bool settingsOpen_ = false;
    bool moreMenuOpen_ = false;              // the overflow menu, under its button after Init and Save
    int settingsTab_ = 0;                    // the settings dialog's tab shown: 0 VISUAL, 1 ENGINE (see layout.h)
    int theme_ = 0;                          // the theme painted last
    // A running random LFO's last cycles, for its display: each one's glide (from, to) at its place in a
    // round of kRandomGlides, and how many have come since it started running (the newest is the last). The
    // phase the newest was last seen at, and whether it has ended without a snapshot bringing the next glide yet
    // (its phase went back: one that wraps before the glide changes, half written). The display is kRandomCycles
    // wide and ends part way through the newest, so it reaches into one cycle more.
    static constexpr int kRandomCycles = 4;
    static constexpr int kRandomGlides = kRandomCycles + 1;
    struct RandomTrace {
        std::array<std::pair<float, float>, kRandomGlides> glides{};
        int count = 0;
        float phase = 0.0f;
        bool ended = false;
    };
    std::array<RandomTrace, kMaxSources> randomTraces_{};
    void traceRandomLfos();         // takes in the newest telemetry's glides
    bool idleLfosShown_ = false;    // what the controller was last told (see Controller::setIdleLfosShown())
    bool randomSweep(int lfo) const; // whether its display shows what it ran, rather than a made-up run
    // Where a running random LFO is in its newest traced cycle: like lfoPhaseNow(), but it stays at the end of
    // the cycle until a snapshot brings the next one, rather than wrap round to the start of the same quarter.
    double randomPhaseNow(int lfo) const;
    // The macros' names as their knobs' labels, in place for as long as the widgets that point at them.
    mutable std::array<std::array<char, 32>, kMacros> macroLabels_{};
    // What the macros and the mod wheel add to each param, worked out from their settings at each paint, so it shows
    // with no note playing too, while the host may have let the plugin sleep.
    std::vector<float> macroOffsets_ = std::vector<float>(kParamCount, 0.0f); // the sequencer's past kModulatedCount too
    std::array<float, kAllSources * kSlots> macroSlotOffsets_{}; // their slots', as a share of each target's range
    // Where the macros' and the mod wheel's slots on the macros' knobs take those knobs, against their set values, and
    // the macros a source of each voice moves (see MacroGraph), worked out with them.
    std::array<float, kMacros> macroKnobOffsets_{};
    uint32_t voiceMovedMacros_ = 0;
    bool voiceMovedMacro(int source) const { return isMacro(source) && (voiceMovedMacros_ >> knobOf(source) & 1u); }
    std::optional<double> liveSlotOffset(int source, int slot) const;
    // Where modulation takes param `index` for the newest voice, against its set value, or with no note playing
    // where the macros take it, and the last voice's modulation of the effects as they ring out; nothing for the
    // params that can't be modulated. A macro's knob, as the newest voice has it, or with none, where the macros and
    // the mod wheel take it, each from where the sequencer's curve holds it.
    float liveOffset(int index) const {
        if (isMacroKnob(index))
            return telemetry_.seqMacro[macroOf(index)] +
                   (telemetry_.voices > 0 ? telemetry_.macroOffset[macroOf(index)] : macroKnobOffsets_[macroOf(index)]);
        if (const int k = seqFeelOf(index); k >= 0) // as the sequencer plays it, while the synth runs it
            return telemetry_.voices > 0 || telemetry_.fxTail || telemetry_.seqPosition >= 0.0f ? telemetry_.seqFeel[k]
                                                                                               : macroOffsets_[index];
        if (index < 0 || index >= kModulatedCount)
            return 0.0f;
        if (telemetry_.voices > 0)
            return telemetry_.modOffset[index];
        return macroOffsets_[index] + (telemetry_.fxTail ? telemetry_.modOffset[index] : 0.0f);
    }
    bool metering_ = false;                  // the output meter showed something when last polled
    std::vector<Rect> changedAreas_;         // see changedAreas()
    // The output scope's level, following its spans' peaks (see followScopeLevel()), which its trace is drawn to (the
    // operator scope's is the inspector's).
    float outScopeLevel_ = 0.0f;
    float glow_ = 0.0f;                      // how strongly the curves glowed when painted last
    float glowDragged_ = -1.0f;              // the glow slider's amount while it is dragged, else -1
    float shownGlow() const override {
        return glowDragged_ >= 0.0f ? glowDragged_ : static_cast<float>(controller_.editorSettings().glow);
    }
    float links_ = 0.0f;                     // how strongly the source's dashed lines were drawn when painted last
    int handles_ = 0;                        // and the depth handles' size
    float linksDragged_ = -1.0f;             // the LINES slider's amount while it is dragged, else -1
    float shownLinks() const {
        return linksDragged_ >= 0.0f ? linksDragged_ : static_cast<float>(controller_.editorSettings().linkIntensity);
    }
    float gridDragged_ = -1.0f;              // the GRID slider's amount while it is dragged, else -1
    float shownGrid() const override {
        return gridDragged_ >= 0.0f ? gridDragged_ : static_cast<float>(controller_.editorSettings().gridIntensity);
    }
    // The amount of the settings slider being dragged (DragKind::Glow): the glow's, or the lines'.
    float hueDragged_ = -1.0f;               // the hue slider's hue while it is dragged, else -1
    // The hue the theme's main color shows now, 0 to 1, or below 0 for the theme's own.
    double shownHue() const { return hueDragged_ >= 0.0f ? hueDragged_ : controller_.editorSettings().accentHue; }
    double hue_ = -1.0;                      // the hue painted last
    float &sliderDragged() {
        return drag_.widget.kind == Kind::SettingsLinks  ? linksDragged_
               : drag_.widget.kind == Kind::SettingsGrid ? gridDragged_
               : drag_.widget.kind == Kind::SettingsHue  ? hueDragged_
                                                         : glowDragged_;
    }
    bool ctrlHeld_ = false; // see setCtrl()
    // The PresetAction the confirm dialog asks about, a panel's kind, or -1. The panels' kinds are numbered apart from
    // the View's: the preset browser's 1, 8 and 9 (PresetBrowser::kConfirmLoad and on), the FX view's 4 to 6
    // (FxPanel::kConfirmFxClear and on), the SEQ view's 7 (SeqPanel::kConfirmClear).
    int confirm_ = -1;
    bool seqTabPressedShown_ = false; // the SEQ view showed as a view tab was last pressed (see doubleClick())
    Panel *confirmFrom_ = nullptr; // the panel that asked (see askConfirm()), or none for the View's own questions
    static_assert(FxPanel::kConfirmFxClear > static_cast<int>(PresetAction::SaveAs) &&
                  SeqPanel::kConfirmClear > FxPanel::kConfirmSplitRemove &&
                  SeqPanel::kConfirmClear < PresetBrowser::kConfirmCategoryRemove);
    std::string flash_;
    int flashTicks_ = 0;
    // What a source's handle is held over (see dwellTarget()), since when, and whether that showed already.
    struct Dwell {
        std::optional<Widget> over;
        double since = 0.0;
        bool done = false;
    };
    Dwell dwell_;
    Clock clock_ = steadySeconds;
    static double steadySeconds();
    std::optional<Typing> typing_;
    int caretTicks_ = 0; // polls since the last key, for the caret's blink
    mutable Derived derived_; // see DerivedScope
    mutable int derivedDepth_ = 0;
};

} // namespace fmvibe::gui
