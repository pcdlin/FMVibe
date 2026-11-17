#pragma once

// The editor's areas as classes of their own: each a Panel, which the View holds and hands what goes on in its area
// (the widgets it owns, pressed, dragged, scrolled, typed into, painted), and which reaches what the View keeps for
// every area (the controller, what is under the mouse, the text fields, the confirm dialog, the messages) through a
// PanelHost. A panel knows nothing else of the View. So far the preset browser is one (presetbrowser.h), which shows
// over the editor (modal), and the SEQ view (seqpanel.h), the MODS view (modspanel.h), the FX view (fxpanel.h), the
// routing panel (routingpanel.h) and the inspector (inspectorpanel.h), which show in their places among the editor's
// areas; and so are the drawn shapes' editors (shapeeditor.h), whose full-size editors show over the panels' row and
// whose widgets show wherever a shape is edited in place; and so is the table browser (tablebrowser.h), which shows over
// the routing panel.

#include "widget.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmvibe {
struct Telemetry;
}

namespace fmvibe::gui {

class Controller;
class FxGraphModel;
class ModulationModel;
class Panel;
class RoutingModel;

// A drag going on, as the View keeps it for every drag (its own and a panel's): where the mouse went down and where it
// is now, whether it has moved past a click's jitter yet, the modifiers its last move came with, and whether an undo
// step is open until it ends (see PanelHost::startDrag()).
struct DragState {
    Point start, now;
    bool moved = false;
    Modifiers mods;
    bool step = false;
};

// What the View does for its panels.
class PanelHost {
public:
    virtual Controller &controller() const = 0;
    // The patch's models, the editor's one of each, which tell it why they refused an edit (see flash()).
    virtual ModulationModel &modulationModel() = 0;
    virtual RoutingModel &routingModel() = 0;
    virtual FxGraphModel &fxGraphModel() = 0;
    // The editor's height now (its width is View::kWidth): it depends on whether the modulation panel is collapsed.
    virtual float height() const = 0;
    // Whether `w` is under the mouse (and no drag goes on), or is what a drag going on holds; and forgetting what is
    // under the mouse, as what shows there changes, until it moves again.
    virtual bool isHovered(const Widget &w) const = 0;
    virtual bool isActive(const Widget &w) const = 0;
    virtual void resetHover() = 0;
    // Paints a widget as the editor paints it wherever it is listed: one of the panel's own comes back to its
    // paintWidget(), another (a text field's x, say) the View's own way.
    virtual void paintWidget(Canvas &c, const Widget &w) = 0;
    // The text fields, which the panel that owns one keeps (see Panel::field()): painting one into `box` (a magnifier,
    // then in `typed` the text, its selection and, while it has the keyboard, the caret, or `hint` while it is empty,
    // and `after` faint at its right end), and emptying one, which its owner hears of (Panel::fieldChanged()).
    virtual void paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint,
                            std::string_view after = {}, bool lens = true) = 0;
    virtual void clearField(Field f) = 0;
    // A message flashed up under the preset bar for a moment.
    virtual void flash(std::string message) = 0;
    // The confirm dialog, over everything, asking about `kind` (one of the View's kinds of question): what it says
    // comes from `from`'s confirmWords(), and the button pressed goes to its confirmed().
    virtual void askConfirm(Panel &from, int kind) = 0;
    // A drag of a widget of `from`'s, pressed at `p`: until the button goes up, the moves go to its drag(), the button
    // let go of to its drop(), and its end, let go of or called off, to its release(), each with the drag as it stands
    // (see DragState). With `step`, the undo step the panel opened (Controller::beginStep()) closes as it ends.
    virtual void startDrag(Panel &from, const Widget &w, Point p, bool step) = 0;
    // A drag of the param a widget of the panel's shows (`w`'s), pressed at `p`: up and down, as the View drags a
    // knob's value (finely with Shift), as one gesture (see Panel::draggingValue()).
    virtual void dragValue(const Widget &w, Point p, Modifiers mods) = 0;
    // What a drag going on holds (the widget pressed, the View's drag or a panel's), or nothing.
    virtual std::optional<Widget> dragged() const = 0;
    // Whether Ctrl is held, as the keyboard or the mouse's last message said (see View::setCtrl()).
    virtual bool ctrlHeld() const = 0;
    // Whole notches of the wheel on `target` (a param), with `steps` added: a trackpad's smaller steps add up toward
    // them while the wheel stays on that target (see View::wheelNotches()).
    virtual int wheelNotches(int target, float steps) = 0;
    // A key going through the editor as the window sends it (see View::seqKey()): what a menu command that does what
    // a key does sends. Whether something took it.
    virtual bool sendKey(Key key, bool shift) = 0;
    // Whether a drag goes on, the View's or a panel's (see startDrag()).
    virtual bool dragging() const = 0;
    // The drag going on carries a copy of what it holds, which the panel makes next: every param and the layout as they
    // are now are kept, which the drag called off goes back to (dropping the copy, its undo step left empty), and the
    // drag's undo step opens if it hasn't. Then, if no copy could be made after all, nothing is kept to go back to.
    virtual void beginCopyDrag() = 0;
    virtual void cancelCopyDrag() = 0;
    // The widget at `p`, as a press there would find it; and every widget the editor lists now, in its order.
    virtual std::optional<Widget> widgetAt(Point p) const = 0;
    virtual std::vector<Widget> listedWidgets() const = 0;
    // The open lists (ListButtons') whose buttons no longer show close: scrolled away with their card, say.
    virtual void closeGoneLists() = 0;
    // A drawn shape whose place is taken by something else, or emptied (an effect's slot): the full-size editor showing
    // it, if it does, closes (see ShapeEditor::shapeGone()).
    virtual void shapeGone(int shape) = 0;

    // Whether a paint or a hit test goes on, in which nothing a panel reads changes: what it works out from the patch,
    // the layout and its own state it may keep until then (see ScopeCache, and Panel::forgetScoped()).
    virtual bool scoped() const = 0;

    // The newest telemetry the View took in (see View::poll()).
    virtual const Telemetry &telemetry() const = 0;
    // Where the mouse is, and the widget under it as the last move found it.
    virtual Point mouse() const = 0;
    virtual const std::optional<Widget> &hovered() const = 0;
    // The full-size editors that show over the panels' row while they are open, hiding the views under them: a drawn
    // shape's, and an operator's Custom wave's partials.
    virtual bool shapeEditorOpen() const = 0;
    virtual bool partialEditorOpen() const = 0;
    // The table browser, which shows over the routing panel while an operator's TABLE or SAMPLE picker has it open
    // (see TableBrowser), hiding the routing graph and the matrix under it but not the inspector.
    virtual bool tableBrowserOpen() const = 0;
    // Lets go of the points picked in a shape editor, as what the panel picks takes over from them.
    virtual void clearShapePick() = 0;
    // Where a drawn shape (see shapeeditor.h) is edited in place, while no full-size editor has it: a Curve source's in
    // its widened card, a shaper effect's in its card's display (worked out from an empty card while the card doesn't
    // show), a voice shaper's on its page in the inspector. What lays those out says.
    virtual Rect shapeHome(int shape) const = 0;
    // The row of cards that holds `card` (a source's, numbered as noteCard() numbers them) scrolls to show all of it.
    virtual void revealCard(int card) = 0;
    // A param's value where modulation takes the newest voice right now, in its range (the macros' with no note), as
    // the effects play it: what their displays draw.
    virtual double liveValue(int param) const = 0;
    // Where an LFO is along its cycle now (0..1) and what it puts out there (-1..1), while its dot shows (it runs: a
    // note plays, or it runs free or anchored, or in the effects' tail); nothing while it doesn't.
    virtual std::optional<std::pair<double, double>> lfoNow(int lfo) const = 0;
    // The param whose list (a ListButton's) is open, or -1: a list of the panel's own, which it lists (see
    // Panel::popups()) and whose entries the View picks as it picks every list's.
    virtual int openList() const = 0;
    // Opens the list of `param` (one of the panel's own, as above), or with -1 closes the one open.
    virtual void showList(int param) = 0;
    // How strongly a grid's lines show: the GRID setting, or where its slider is dragged to (0..1, 0.5 as they were
    // drawn before it could be set).
    virtual float shownGrid() const = 0;
    // How strongly the graphs glow: the GLOW setting, or where its slider is dragged to (0..1).
    virtual float shownGlow() const = 0;
    // The cards of the rows the View shows (an effect's, a source's), by their number: a source's place, or kMaxSources
    // and an effect's slot. Noting where one shows (as much of it as `shown` shows), for the mouse to find and a theme
    // whose cards go without an outline to outline it under the mouse; a card's face, as every card's is painted
    // (its shadow, its fill, tinted with `color` in a flat theme, faintly while it is off, and its outline in `color` at
    // `edge` while it is on), noted; and what the View still lays out and paints of an effect's card: its widgets below
    // its title row, and the card itself (its face, its title and its display; its widgets are painted with the others).
    virtual void noteCard(int card, Rect r, Rect shown) = 0;
    virtual void paintCardFace(Canvas &c, Rect r, Color color, bool on, int card, bool dragged, float edge,
                               Rect shown) = 0;
    virtual void cardWidgets(std::vector<Widget> &list, int card, Rect r) const = 0;
    virtual void paintCard(Canvas &c, int card, Rect r, bool dragged) = 0;
    // A graph's backdrop, under its nodes and wires in `area`: the routing graph's glow and orbit rings, spreading from
    // its OUT at `out`.
    virtual void paintBackdrop(Canvas &c, Rect area, Point out) = 0;
    // An envelope (an operator's, or kOps + a source's for a mod envelope), as the View edits every one, on its cards
    // and pages alike: its handles, listed into `list` where its display shows it, which the View drags (holding the
    // display's zoom while one is dragged or the mouse stays over it); and the display painted into `r` in `color`,
    // with (`live`) a line at the newest voice's level and a dot where it is along the curve.
    virtual void envelopeWidgets(std::vector<Widget> &list, int env) const = 0;
    virtual void paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live) = 0;
    // Each modulation of `param`'s range along `bar` (a route's label, a matrix cell), as a knob's ring shows them, the
    // one whose depth handle is under the mouse over the others, and where the set value is and modulation takes it now.
    virtual void paintModRange(Canvas &c, Rect bar, int param) = 0;
    // A switch's face, as the View paints its own: lit, a tint of `color` (`tint` strong); hot (under the mouse, say);
    // its part of a row merged into a pill (see mergeButtons()).
    virtual void paintButtonFace(Canvas &c, const Widget &w, bool lit, bool hot, Color color, float tint,
                                 float radius) = 0;

protected:
    ~PanelHost() = default;
};

// How hard a drag held at `p` near the left or right edge of a row of cards in `panel` pulls the row that way, -1..1 (0
// clear of them): within kEdgeZone of an edge, or past it. A card grabbed that close to an edge (at `grabbedX`) only
// pulls once it is taken further toward it than where it was grabbed; a source's handle (no `grabbedX`) only within
// the panel's height. And the cards the row scrolls a poll for it.
constexpr float kEdgeZone = 28.0f;
inline float edgePull(Rect panel, Point p, std::optional<float> grabbedX) {
    if (!grabbedX && (p.y < panel.y || p.y >= panel.bottom()))
        return 0.0f;
    const float leftEdge = grabbedX ? std::min(panel.x + kEdgeZone, *grabbedX - 4.0f) : panel.x + kEdgeZone;
    const float rightEdge = grabbedX ? std::max(panel.right() - kEdgeZone, *grabbedX + 4.0f) : panel.right() - kEdgeZone;
    const float left = (leftEdge - p.x) / kEdgeZone;
    const float right = (p.x - rightEdge) / kEdgeZone;
    return left > 0.0f ? -std::min(left, 1.0f) : (right > 0.0f ? std::min(right, 1.0f) : 0.0f);
}
inline float edgeScrollStep(float pull) {
    constexpr float kStep = 0.06f;
    return kStep * (pull < 0.0f ? -0.35f + 0.65f * pull : 0.35f + 0.65f * pull);
}

// What a panel works out for a paint or a hit test (its rows, a menu's entries), kept while the host's scope goes on
// (see PanelHost::scoped()) and let go of as one begins and ends (Panel::forgetScoped()); outside one, worked out
// afresh each time.
template <class T>
class ScopeCache {
public:
    template <class Make>
    T get(const PanelHost &host, Make &&make) const {
        if (value_)
            return *value_;
        T made = make();
        if (host.scoped())
            value_ = made;
        return made;
    }
    void forget() const { value_.reset(); }

private:
    mutable std::optional<T> value_;
};

// An area of the editor. What it doesn't own, it leaves: each call says whether it took what it was handed (false, or
// nothing, for what is not its own), and the View hands it on to the next or deals with it itself.
class Panel {
public:
    virtual ~Panel() = default;

    // Whether it shows over the editor now, taking every click (a dialog or popup of its own; under the settings and
    // the confirm dialog), and then its widgets, which hit testing looks through alone. A panel that isn't modal shows
    // in its place among the editor's areas, where the View lists its widgets, in their order among the others' (for
    // hit testing and painting), while it shows there; and its popups (its lists and menus open) after the others,
    // with the View's own.
    virtual bool modal() const { return false; }
    virtual void widgets(std::vector<Widget> &) const {}
    virtual void popups(std::vector<Widget> &) const {}
    // Paints its area under its widgets, in its place among the editor's areas (a view in the panels' row, say).
    virtual void paintBack(Canvas &) {}
    // Paints over the widgets of its area, under the open lists: what veils some of them (the cards a card carried would
    // land among, say). Then over the open lists, what a drag of its own carries, under the View's drags' overlays.
    virtual void paintOverWidgets(Canvas &) {}
    virtual void paintDrag(Canvas &) {}
    // Paints what it shows over the editor's widgets, under the dialogs and the tooltip.
    virtual void paint(Canvas &) {}
    // Paints a widget of its own, wherever it is listed (`hovered` while the mouse is over it); false for another.
    virtual bool paintWidget(Canvas &, const Widget &, bool /*hovered*/) { return false; }
    // What shows under a widget of its own while the mouse rests on it; empty for none, or for another. And whether it
    // goes under the mouse, rather than under the widget (a tall or wide one).
    virtual std::string tooltip(const Widget &) const { return {}; }
    virtual bool tipAtMouse(const Widget &) const { return false; }
    // The cursor over a widget of its own, or holding one in a drag of its own; nothing to leave it to the View.
    virtual std::optional<CursorKind> cursor(const Widget &) const { return std::nullopt; }

    // Whether `p` hits a widget of its own whose shape isn't its rect (a wire, along its path; a circle, within it), or
    // nothing to leave it to the rect, as for every other widget.
    virtual std::optional<bool> hits(const Widget &, Point) const { return std::nullopt; }

    // What it keeps while the host's scope goes on (see ScopeCache), let go of as one begins and ends.
    virtual void forgetScoped() {}
    // At each poll of the View's: the newest telemetry taken in, before the View keeps it (PanelHost::telemetry() is
    // still the poll before's), and where what it shows of it moved, added to `repaint`: the band of the editor it shows
    // in (kPanelsBand, say), or the part of it that shows that alone, which the View can repaint alone. Then a step of
    // what moves by itself (a glide, a scroll while a drag is held at an edge, the patch loaded): whether anything
    // moved, which repaints the whole editor.
    virtual void poll(const Telemetry &, std::vector<Rect> & /*repaint*/) {}
    virtual bool step() { return false; }
    // A source's handle (a drag of the View's) held at `p`, near an edge of a row of its own: the row scrolls that way a
    // step (see edgePull()). Whether it moved.
    virtual bool scrollToward(Point) { return false; }
    // The mouse going down, before anything is hit: what glides into place settles there at once, where a click means it.
    virtual void settle() {}

    // The mouse moving onto something else (`hit`, or nothing) while no drag goes on: a menu of its own opening what
    // it is over, say.
    virtual void hoverChanged(const std::optional<Widget> & /*hit*/) {}
    // The mouse moving from `before` to `now` while no drag goes on: whether what it shows changes with it, beyond what
    // is under the mouse (a grid shown while Ctrl is held over its area, say).
    virtual bool hoverMoves(Point /*before*/, Point /*now*/) const { return false; }
    // Its popups (a menu or list open) closing: as another opens, or as what they were opened on may change or scroll
    // away (an undo, a pan, another view shown).
    virtual void closePopups() {}
    // An undo or redo about to change the patch: what it keeps of where a list was opened may not hold after.
    virtual void undoing() {}
    // A press anywhere, before it goes to what it hit (`hit`, or nothing): what lets go at a click elsewhere (a field
    // taking the keyboard, say) lets go here.
    virtual void pressing(const Widget * /*hit*/) {}
    // The press and double-click on what it owns. A press that starts a drag asks for it (see PanelHost::startDrag()),
    // whose moves come to drag() (the mouse at `now`, with the modifiers held), the button let go of to drop(), and
    // whose end, let go of or called off, to release().
    virtual bool press(const Widget &, Point, Modifiers) { return false; }
    virtual bool doubleClick(const Widget &, Point, Modifiers) { return false; }
    // Whether a double-click on a widget of its own comes as a second press, as a quick second click on a switch does
    // (see View::doubleClick()).
    virtual bool doublePress(const Widget &) const { return false; }
    virtual void drag(const DragState &) {}
    virtual void drop(const DragState &) {}
    virtual void release() {}
    // A widget of its own whose value the View drags (see PanelHost::dragValue()) about to take a move: what goes with
    // it, in the same gesture (a matrix cell switching on the nodes it joins).
    virtual void draggingValue(const Widget &) {}
    // What a finger moving on a widget of its own drags rather than pans past (see View::touchMove()).
    virtual bool dragsOnTouch(const Widget &) const { return false; }
    // A source's handle dragged over the editor (see View's ModSource drag): whether a widget of its own takes it
    // (beside the knobs, labels and depth handles the View's take), its ring there as the drag's overlay draws it
    // (`over` while the handle is on it, in `color`), and the handle let go of on it.
    virtual bool takesSource(const Widget &, int /*source*/) const { return false; }
    virtual void paintSourceRing(Canvas &, const Widget &, bool /*over*/, Color) {}
    virtual bool dropSource(const Widget &, int /*source*/) { return false; }
    // The wheel: first over what it takes whole, before anything is hit (its popups, a list); then over a widget of
    // its own that isn't a popup's, with the modifiers held; and turned sideways (see View::wheelAcross()).
    virtual bool wheel(Point, float /*steps*/) { return false; }
    virtual bool wheelOn(const Widget &, Point, float /*steps*/, Modifiers) { return false; }
    virtual bool wheelAcross(Point, float /*steps*/) { return false; }
    // Then over its area where what is under the mouse doesn't take the wheel (nothing, or what sets no param), once
    // the View's own have passed it by: its rows scrolling, say.
    virtual bool wheelBehind(Point, float /*steps*/) { return false; }
    // Whether a widget of its own is part of an open menu that doesn't scroll, where nothing pans (see below).
    virtual bool blocksPan(const Widget &) const { return false; }
    // What the middle button or two fingers would pan at a point: one of its parts (its own number) and where that is
    // scrolled to now, in its own units. A pan of it moves `across` and `down` pixels from where that part was at
    // `first`: it scrolls there, held at its ends, and says where it was held to when it went past one (from where the
    // pan then goes on), else nothing.
    // As a pan starts, before its moves: where that part is now, which the pan moves from.
    virtual std::optional<std::pair<int, float>> panAt(Point) const { return std::nullopt; }
    virtual void panBegin(int /*part*/) {}
    virtual std::optional<float> pan(int /*part*/, float /*first*/, float /*across*/, float /*down*/) {
        return std::nullopt;
    }
    // A context menu opened on `hit` (or nothing) at `p`: it forgets what an earlier one was opened on, and puts its
    // entries in `items` if it owns what is hit; a command picked from it.
    virtual bool contextMenu(const std::optional<Widget> &, Point, std::vector<MenuItem> &) { return false; }
    // A context menu the View fills for what a widget of its own shows as a knob shows a param (Edit Value, Reset to
    // Default, then the modulations'): its own entries, after Reset to Default. Its commands come to menuCommand() too.
    virtual void paramMenu(const Widget &, std::vector<MenuItem> &) {}
    virtual bool menuCommand(int) { return false; }

    // The keyboard. Whether it shows a dialog that takes Enter and Escape (see View::dialogKey()), and those keys, with
    // the settings and the confirm dialog shut: whether it took one, or nothing to leave it to the View.
    virtual bool dialog() const { return false; }
    virtual std::optional<bool> dialogKey(Key) { return std::nullopt; }
    // A key the window sends while the mouse is over the editor, with nothing typed, no dialog open and no drag going
    // on (see View::seqKey()): whether it took it, or else the host has it.
    virtual bool key(Key, bool /*shift*/) { return false; }
    // Its text fields: each by its Field, and which one has the keyboard (with nothing when none does), while it
    // decides that (false: the View does); what follows a change to what is typed in one; and the keys a field
    // doesn't take itself (Enter, Escape, Up and Down).
    virtual TextField *field(Field) { return nullptr; }
    virtual bool fieldFocus(std::optional<Field> &) const { return false; }
    virtual void fieldChanged(Field) {}
    virtual void fieldKey(Field, Key) {}

    // A confirm dialog it asked for (see PanelHost::askConfirm()): its title, what it says, its button that goes ahead
    // and whether it says to save the patch first (it replaces the patch; else what goes is no part of it, or undo
    // brings it back); whether it is a line shorter (it says one line), whether its way ahead is lit in the accent
    // (which Enter presses; else none is more likely than the others, and Enter leaves it open), a third button
    // beside them, if any, and choices of their own over them on a taller dialog, each in its color; and the button
    // pressed (0 cancels, 1 goes ahead, 2 and on the choices, 4 the third). Its kinds are numbered apart from the
    // others' (see View::confirm_).
    struct ConfirmWords {
        std::string title, what;
        std::string ok;
        bool saveFirst = false;
        bool shorter = false;
        bool lit = true;
        std::string third;
        std::vector<std::pair<std::string, Color>> choices;
    };
    virtual ConfirmWords confirmWords(int /*kind*/) const { return {}; }
    virtual void confirmed(int /*kind*/, int /*button*/) {}
};

} // namespace fmvibe::gui
