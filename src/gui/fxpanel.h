#pragma once

// The FX view (see fxpanel.cpp): the effects (see fxgraphmodel.h), a line the view shows as a row of cards, each split
// a container holding its bands, or as a graph on a fixed grid with an inspector beside it: two views of one line, which
// the editor reads from the wires and FxGraphModel writes the wires from, so the engine runs what either shows. It
// shows in the panels' row while the FX tab is picked and no shape editor is over the row, in place of the routing
// panel and the inspector; the View lists its widgets there, and its list of effects with the others' popups. The panel
// lays the row and the graph out, paints them (the containers, the folded strips, the arrows, the graph's nodes and
// wires, a split's controls), moves the cards and nodes (copies with Ctrl), lets them glide aside, scrolls them, opens
// the list of effects, and asks before the bin, a split's removal and its middle band's going take effects away. What
// an effect's card holds below its title row (its knobs, switches, display and the handles on it) is still the View's,
// which it lays out and paints for the panel (PanelHost::cardWidgets(), paintCard()), as are the row's and the graph's
// scrollbars, which the View drags through the scrolls below.

#include "fxgraphmodel.h"
#include "panel.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class FxPanel : public Panel {
public:
    explicit FxPanel(PanelHost &host);

    // The FX view shows, with no shape editor over it: it takes the place of the routing panel and the inspector.
    bool wide() const;
    // The FX view showing the effects as a graph, rather than the row.
    bool graph() const;
    // The card of the effect in `slot` where it shows now: in the row, or in the graph's inspector; none while it
    // doesn't show.
    std::optional<Rect> cardOf(int slot) const;
    // Where a split's controls show now, if they do; and a split's display on its controls' card (shorter in three
    // bands, to leave room for the crossovers' knobs under it).
    std::optional<Rect> splitCardOf(int split) const;
    static Rect splitDisplay(Rect card, bool three);
    // The band each three-band dynamics' or parametric EQ's card shows, and showing another.
    int band(int slot) const { return fxBand_[slot]; }
    void showBand(int slot, int band) { fxBand_[slot] = band; }
    // The switched-off effects' cards as they show, below which they are dimmed: each slot, its card and what it is cut
    // off at.
    std::vector<std::tuple<int, Rect, Rect>> offCards() const;
    // What scrolls sideways with a scrollbar: the row, by card places, and the graph, by pixels. How long each is, how
    // much of the graph shows, where each is scrolled to (held to its end), and scrolling it there.
    float rowLength() const;
    float rowScroll() const { return fxScroll(); }
    void setRowScroll(float first) { fxScroll_ = std::max(first, 0.0f); } // rowScroll() holds the top end
    float graphLength() const;
    float graphShown() const { return fxGraphArea().w; }
    float graphScroll() const;
    void setGraphScroll(float first) { fxGraphScroll_ = std::max(first, 0.0f); }
    Rect graphScrollTrack() const { return fxGraphScrollTrack(); }
    // A split in two bands or three, as FxGraphModel::setSplitBands() has it: with effects on its middle band and
    // `midTo` -1, the confirm dialog asks first where they go (0 removed, 1 the high band's start, 2 the low band's
    // end).
    void setSplitBands(int split, int bands, int midTo = -1);

    // Its questions (see confirmWords()), numbered apart from the others' (see View::confirm_).
    static constexpr int kConfirmFxClear = 4;     // asking before every effect goes
    static constexpr int kConfirmMidBand = 5;     // asking where a split's middle band's effects go, back in two bands
    static constexpr int kConfirmSplitRemove = 6; // asking whether a split's bands' effects stay, as it is removed
    // Its context menu's commands (see contextMenu()), apart from the View's and the SEQ view's: Duplicate, Remove
    // and Reset Settings, then the OTT settings, an amp's oversampling and moving an effect to another band.
    static constexpr int kMenuDuplicate = 6;
    static constexpr int kMenuRemove = 7;
    static constexpr int kMenuReset = 8;
    static constexpr int kOttCommand = 15;
    static constexpr int kAmpOversamplingCommand = 30;
    static constexpr int kMoveBandCommand = 40;

    void forgetScoped() override;
    void widgets(std::vector<Widget> &list) const override;
    void popups(std::vector<Widget> &list) const override; // the list of effects, while it is open
    void paintBack(Canvas &c) override;
    void paintOverWidgets(Canvas &c) override;
    void paintDrag(Canvas &c) override;
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::optional<bool> hits(const Widget &w, Point p) const override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    void poll(const Telemetry &t, std::vector<Rect> &repaint) override;
    bool step() override;
    bool scrollToward(Point p) override;
    void settle() override;
    void closePopups() override;
    void undoing() override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doublePress(const Widget &w) const override;
    void drag(const DragState &d) override;
    void drop(const DragState &d) override;
    void release() override;
    bool dragsOnTouch(const Widget &w) const override;
    bool wheelAcross(Point p, float steps) override;
    bool wheelBehind(Point p, float steps) override;
    std::optional<std::pair<int, float>> panAt(Point p) const override;
    std::optional<float> pan(int part, float first, float across, float down) override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;
    ConfirmWords confirmWords(int kind) const override;
    void confirmed(int kind, int button) override;

private:
    using Kind = WidgetKind;
    using FxWireRef = gui::FxWireRef;
    using FxLineItem = gui::FxLineItem;
    using FxLine = gui::FxLine;
    using FxSlotAt = gui::FxSlotAt;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return fx_.value(index); }
    void edit(int index, double v) { fx_.edit(index, v); }
    // What is under the mouse, as the View found it last (it stays what the press found while a drag goes on).
    const std::optional<Widget> &hover() const { return host_.hovered(); }

    // The drags of its own: a card or container moved along the row, a node of the graph onto another place, a split's
    // crossover handle across. Where the mouse went down and is now, whether it has moved past a click's jitter yet
    // (see DragState), and what each keeps.
    enum class DragKind { None, FxMove, FxNodeMove, SplitPoint };
    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start, now;
        bool moved = false;
        bool copied = false;      // its first move has been seen, which a Ctrl held makes a copy (see drag())
        bool dropped = false;     // let go of, rather than called off (see release())
        double startValue = 0.0;
        float scrollStart = 0.0f; // moving a card: where its row was scrolled to when it was grabbed
    };
    void startDrag(DragKind kind, const Widget &w, Point p);

    // The slots that hold an effect, in the line's order (see FxGraphModel::chain()).
    std::vector<int> fxChain() const;
    // FxGraphModel's edits, with what the editor's own state needs around them: an effect added where the list was
    // opened (see fxSlot_), shown in either view; a slot filled or emptied (see effectFilled()).
    void addEffect(FxType type, int copyOf = -1);
    void removeEffect(int slot);
    // What the editor keeps of an effect slot that another effect now fills (as `copyOf`'s copy, or fresh), or that
    // empties.
    void effectFilled(int slot, int copyOf);
    void effectGone(int slot);
    // Cards folded to a strip, which take fewer places: kFoldedFx of one.
    bool fxFolded(int slot) const { return fx_.folded(true, slot); }
    float fxCardSpan(int slot) const;
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
    // How brightly a port of the effects graph lights what it goes out of and along, 0..1: by its level in the
    // telemetry, in dB from kFxGlowFloorDb up to full scale; 0 while the audio has stopped.
    static constexpr float kFxGlowFloorDb = -48.0f;
    float fxGlow(int port) const;
    static float fxGlowTarget(const Telemetry &t, int port);
    std::array<float, kFxPorts> fxGlow_{}; // what the effects graph shows of each port's glow, following it
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
        bool copy = false;    // a copy carried, the original left in `rest`: let go of anywhere, it lands
    };
    // Ctrl held as a card or node starts to move: what is carried becomes a copy of it, the original staying where it
    // is, made as the drag's undo step opens (an effect into a free slot, a split with its join and its bands'
    // effects). Too few free, it stays a move, with a message.
    void copyDragged();
    // A copy of `item` (an effect, or a split with its join and the effects on its bands): its settings, not its
    // modulation, in free slots and a free split, shown folded as the original; `copies` maps each node to its copy.
    // None, with a message, where there aren't enough.
    std::optional<FxLineItem> copyFxItem(const FxLineItem &item, std::map<int, int> &copies);
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
    bool glideFxGraph();              // a step of the glide, in step(): whether anything moved
    // Every effect (and every split and join) gone, as FxGraphModel::clear() has it, the row and the graph scrolled
    // back to their start.
    void clearFx();
    bool fxGraph() const { return graph(); }
    // The line the wires make (see FxGraphModel::line()), kept while the host's scope goes on.
    FxLine fxLine() const;
    static FxGridLayout fxGridLayout(const FxLine &line);
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
    std::optional<Rect> fxCardOf(int slot) const { return cardOf(slot); }
    int fxSelectedNode() const; // the node the inspector shows, -1 for none
    // A split with its join right after it, its bands empty: where the list was opened on the main line (an arrow in
    // the row, a wire's middle in the graph; see fxSlot_), else at the line's end; shown.
    void addSplit(int bands = 2);
    // An effect, or a split with its join (a join's x removes its split), as FxGraphModel::removeNode() has it: a
    // split's bands' effects stay on the main line (`effects` 0) or go with it (1); with any on its bands, -1 asks
    // which first (kConfirmSplitRemove).
    void removeFxNode(int node, int effects = -1);
    // The FX view showing the effects as a graph, or as the row: only the view changes.
    void setFxGraph(bool on);
    // The node of the graph under the mouse, or whose x or power switch is (see Kind::FxNodeDelete and FxNodePower),
    // -1 for none; and where its x goes, at its upper right, and an effect's power switch, at its upper left.
    int fxHoveredNode() const;
    static Point fxDeleteAt(int node, Point center);
    static Point fxPowerAt(int node, Point center);
    // The graph's widgets: its nodes and +s, and the inspector's split or join card.
    void fxGraphWidgets(std::vector<Widget> &list) const;
    // A card's widgets, where it shows: in the row, or in the effects graph's inspector (`graph`), where it neither folds
    // nor moves. Its title row is the panel's; what is below it, the View's (PanelHost::cardWidgets()).
    void cardWidgets(std::vector<Widget> &list, int slot, size_t pos, Rect card, bool graph) const;
    // A split's controls on a card's place (`card`): in its container in the row (`row`, where its title drags it and
    // it folds), or as the card of its split or join node picked in the graph. Its title, the band tabs, the
    // crossover's display, the slope, the bands' solos, and the knobs for the low band's level, the crossover and the
    // high band's.
    void splitControlWidgets(std::vector<Widget> &list, int split, Rect card, bool row) const;
    void paintSplitControls(Canvas &c, int split, Rect card, bool row);
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
        int lane;          // the band it shows (see FxGraphModel::splitLane())
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
    // Kept while the host's scope goes on: every card's display asks for it.
    FxRow fxRowShown() const;
    FxRow fxRowShownOf(const FxLine &line) const; // the same for `line`, worked out
    // The widgets of the effect card being carried in the row, laid out at the place it was picked up from, only for
    // its faint image under the mouse (painted, never hit): it may land on a band its container doesn't show.
    mutable std::vector<Widget> carriedWidgets_;
    // The row's cards (by slot) and containers (by split node) as shown, in card places, and where the line ends: once a
    // card or container is dragged, each glides a step of the way to where it goes every step(), until they settle after
    // it is let go. A node not shown at the last step shows where it goes at once.
    struct FxRowGlide {
        bool on = false;
        std::array<float, kFxNodes> place{};
        std::array<bool, kFxNodes> known{};
        float end = 0.0f;
        int carried = -1; // the node carried: an effect's slot, or a split's node with what it holds
    };
    void startFxRowGlide(); // from where everything shows now
    bool glideFxRow();      // a step of the glide, in step(): whether anything moved
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
    // The FX panel; with `only`, just the row's cards, containers and arrows across from it (a carried container's
    // image), without the panel's header.
    void paintFx(Canvas &c, std::optional<Rect> only = std::nullopt);
    void paintFxGraph(Canvas &c);
    Point splitHandle(int split, Rect display, int which = 0) const;
    // How far a band's level at its join lowers its curve: its knob's position, 0 silent .. 1 at 0 dB (by its port's
    // band: 0 low, 1 middle, 2 high).
    double splitBandGain(int split, int band) const;
    // Measured in card places (the parametric EQ's card takes more than one): how far the row is scrolled, held to its
    // length.
    float fxScroll() const;
    // While there is room for more, a dashed card after the last effect (a card place wide) adds one.
    bool fxShadow() const;
    // The color of the effect in `slot`: each type of effect has its own.
    Color fxColor(int slot) const;

    PanelHost &host_;
    // The patch's effects, its model telling the editor why it refused an edit (see PanelHost::flash()).
    FxGraphModel &fx_;
    const ParamTable &table_;
    Drag drag_;
    float fxScroll_ = 0.0f;              // how many effect cards are scrolled past, before clamping
    bool fxMenuOpen_ = false;            // the ADD EFFECT list
    Point fxMenuAt_{-1.0f, -1.0f};       // its top left: at the mouse, or (x < 0) under ADD EFFECT
    FxSlotAt fxSlot_;                    // where in the line the list was opened, if there
    std::optional<FxDrag> fxDrag_;       // a node of the graph, or a card or container of the row, being dragged
    FxGlide fxGlide_;
    FxRowGlide fxRowGlide_;
    float fxGraphScroll_ = 0.0f;         // how far the effects graph is scrolled sideways, before clamping
    std::array<int, kFxSlots> fxBand_{}; // the band each three-band dynamics' or parametric EQ's card shows
    int confirmSplit_ = -1;              // the split kConfirmMidBand or kConfirmSplitRemove asks about
    size_t confirmEffects_ = 0;          // how many effects it asks about, counted as it opens
    int menuFx_ = -1;                    // the node its open context menu was opened on, or -1
    // The line, the wires hit tests look along, the row as shown and its length, kept while the host's scope goes on:
    // widgets(), hit tests and paint() ask for them many times over.
    ScopeCache<FxLine> line_;
    ScopeCache<std::vector<FxGraphWire>> hitWires_;
    ScopeCache<FxRow> rowShown_;
    ScopeCache<float> rowLength_;
};

} // namespace fmvibe::gui
