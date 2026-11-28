#pragma once

// The routing panel (see routingpanel.cpp): the routing (see routingmodel.h) as the panels' row shows it beside the
// inspector, in either of two views of the same params: the graph (ROUTING), its operators circles and its filters and
// shapers boxes, joined by wires bowing between them and lines into OUT, each with its depth on a label; and the
// matrix (MATRIX), a cell a route. With either, ALGORITHM at the tabs' row's right opens the panel of algorithms over
// the graph. The View lists its widgets there while ROUTING or MATRIX is picked and no full-size editor is over the
// row, and its popups (the panel of algorithms, a label's list of modes) with the others'.
//
// The panel lays the graph out (its labels clear of the nodes and of each other, the switched-off nodes' ghosts under
// OUT) and paints it, the glow behind OUT following the output's level and the nodes' halos and wires' swell following
// what they let through; it moves the nodes (onto its grid's points with Ctrl held), connects them by their edges,
// moves the routes by their ends, bends the wires, opens a label's list of modes and the panel of algorithms, and
// offers what is the routing's in their context menus. A route's label and a matrix cell show their route's depth as a
// knob shows a param: the View drags, types and turns it and puts the modulation's depth handles on it as it does a
// knob's, the panel adding what is the routing's (a cell dragged switching on what it joins, a double-click connecting
// or disconnecting, the modes and Disconnect in its menu), wherever one shows (on the operator page's I/O tab too).
// The matrix's names do what the inspector's tabs do, and are the inspector's with them (see InspectorPanel). The glow
// is the effects graph's backdrop too (see paintFxBackdrop()), which the View paints for the FX view.

#include "panel.h"
#include "routingmodel.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class RoutingPanel : public Panel {
public:
    explicit RoutingPanel(PanelHost &host);

    // The routing graph shows (ROUTING picked, no editor over the panels), with its glow behind it; and either view
    // shows, with ALGORITHM at the tabs' row's right.
    bool graphShown() const;
    bool shown() const;
    // The panel of algorithms closes, as an open list does as something scrolls under the middle button.
    void closeAlgorithms() { algorithmMenuOpen_ = false; }
    // The glow behind OUT follows the output's level in `now`, the newest telemetry (see follow()): whether how it shows
    // changed. Then, once the View has taken `now` in, the nodes' halos and the wires' swell follow what they let
    // through: whether any moved visibly. (The View repaints what shows them: the graph, or the effects graph's
    // backdrop.)
    bool followOutput(const Telemetry &now);
    bool followRoutes();
    // The effects graph's backdrop, under its nodes and wires in `area`: the routing graph's glow and orbit rings,
    // spreading from its OUT at `out` (see PanelHost::paintBackdrop()).
    void paintFxBackdrop(Canvas &c, Rect area, Point out);

    // Its context menus' commands, apart from the View's and the other panels' (see View::menuCommand()): Disconnect,
    // a node's Switch Off (a node shown dim's Switch On and Hide), Reset Bend, and from kRouteModeCommand on each of
    // a route's modes.
    static constexpr int kMenuDisconnect = 3;
    static constexpr int kMenuSwitchOff = 4;
    static constexpr int kMenuResetBend = 14;
    static constexpr int kRouteModeCommand = 16;
    static constexpr int kMenuSwitchOn = 24;
    static constexpr int kMenuHide = 25;

    // The routing graph's glow behind it (see paintBack()): its shape at this zoom, over the part of the panel it
    // reaches, and its pixels for the strength and accent it was filled with last.
    struct RadarGlow {
        Rect area;
        Rect panel;   // what it is drawn in, the corners rounded
        Point center; // where it spreads from
        float scale = 0.0f;
        std::vector<float> distance; // each pixel's from OUT
        std::vector<float> cover;    // how much of it is in the panel, at its rounded corners
        Image image;
        float strength = -1.0f;
        float reach = -1.0f;
        Color accent{};
    };

    void widgets(std::vector<Widget> &list) const override;
    void popups(std::vector<Widget> &list) const override; // the graph's labels, a label's list of modes, the panel of algorithms
    void paintBack(Canvas &c) override;
    void paintDrag(Canvas &c) override; // a route being connected or moved, following the mouse
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    std::optional<bool> hits(const Widget &w, Point p) const override;
    bool hoverMoves(Point before, Point now) const override;
    bool step() override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doubleClick(const Widget &w, Point p, Modifiers mods) override;
    void drag(const DragState &d) override;
    void drop(const DragState &d) override;
    void release() override;
    void draggingValue(const Widget &w) override;
    bool dragsOnTouch(const Widget &w) const override;
    bool wheelOn(const Widget &w, Point p, float steps, Modifiers mods) override;
    bool blocksPan(const Widget &w) const override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    void paramMenu(const Widget &w, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;
    bool dialog() const override { return algorithmMenuOpen_; }
    std::optional<bool> dialogKey(Key key) override;

private:
    using Kind = WidgetKind;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return routing_.value(index); }
    void edit(int index, double v) { routing_.edit(index, v); }

    // The drags of its own: a node moved, a route connected from a node's edge, a route moved by its arrowhead onto
    // another node to go into or by its start onto another node to come from, and a wire bent sideways. What each
    // holds, where the mouse is now and whether it has moved past a click's jitter yet (see DragState).
    enum class DragKind { None, Node, Connect, Reroute, RerouteFrom, Bend };
    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start, now;
        bool moved = false;
        bool dropped = false;  // let go of, rather than called off (see release())
        bool leftNode = false; // a route being dragged has been well clear of the node it comes from
        double startValue = 0.0;
    };
    void startDrag(DragKind kind, const Widget &w, Point p, bool step);

    // Noise has no ratio: its node shows its color instead.
    bool isNoise(int op) const;
    // A node's center, or OUT's (kNodes).
    Point routePoint(int node) const;
    // The route whose arrowhead or start is being dragged elsewhere, which the graph then draws following the mouse;
    // else -1.
    int rerouting() const;
    // The node whose circle or box is at `p`, -1 for none.
    int nodeAt(Point p) const;
    // A route's amount where modulation takes the newest voice right now (0..1), or as set while nothing plays.
    double liveAmount(int param) const;
    // Whether `node` is heard in the end: it goes out, or feeds or modulates a node that is, by routes open where
    // modulation takes them now (see liveAmount()).
    bool reachesOut(int node) const;
    // How far `node` is let through to the output now: the largest live amount of its routes that lead there (0..1).
    double outLevel(int node) const;
    // Where a label `width` wide goes on a wire, a quadratic curve from `start` by `control` to `end` whose middle is
    // `middle`: there, unless that puts it over a node or one of the labels `taken`; then slid along the wire, either
    // way, as little as clears every node shown and those labels, staying inside the panel; where nowhere along it
    // does (a node across a short wire), just beside it, either side, as near its middle as clears them. Where nowhere
    // does, the same, as little as clears the nodes alone; where nowhere does that, at the middle.
    Point labelAlong(Point start, Point control, Point end, Point middle, float width,
                     const std::vector<Rect> &taken = {}) const;
    // Where the graph's labels rest, by route (and OUT line): the feedback loops' first, then each wire's and each OUT
    // line's along it, clear of the labels before it as labelAlong() has it. Each is the label as labelPill() takes
    // it, before a mode's glyph widens it. The route being dragged has none.
    std::vector<std::pair<int, Rect>> routeLabels() const;
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
    // The routing graph's nodes under the mouse, or whose power switch is (see Kind::NodePower), -1 for none; and where
    // its power switch goes, at its upper right.
    int hoveredNode() const;
    static Point nodePowerAt(int node, Point center);
    // Ctrl+drag in the routing graph: where a node dragged to `want` goes on its grid, the nearest point it fits at
    // clear of the other nodes; and whether the grid's points show now.
    Point gridPointFor(int node, Point want) const;
    bool gridShown() const;

    // What the graph shows of each node's halo and each wire's amount (by route), following them (see followRoutes()),
    // and how bright a node's halo is to be, 0 to 1: an operator's as its envelope and the routes to the output let it
    // through, a filter's or shaper's as loud as what goes through it (see boxLevel()).
    float nodeGlowTarget(int node) const;
    float boxLevel(int node) const;
    float shownAmount(int param) const;
    // How the glow behind OUT shows, as loud as the output has risen to (see radarLevel_).
    struct RadarLook {
        float strength; // its alpha on OUT
        float reach;    // how far out it fades to nothing
    };
    RadarLook radarLook() const;
    static float sentOutLevel(const Telemetry &t);

    // Painting: the graph (its glow, OUT, the operators' halos and copies and the wires; its nodes and labels are
    // widgets), the matrix's names and its legend, and the nodes.
    void paintRouting(Canvas &c);
    void paintMatrixLabels(Canvas &c);
    void paintNode(Canvas &c, const Widget &w, bool hovered);
    void paintOpBacking(Canvas &c, int op, Point center);
    bool opHeard(int op) const;

    PanelHost &host_;
    // The patch's routing, its model telling the editor why it refused an edit (see PanelHost::flash()).
    RoutingModel &routing_;
    const ParamTable &table_;
    Drag drag_;
    RadarGlow radarGlow_;
    RadarGlow fxRadarGlow_;                          // the same behind the effects graph (see paintFxBackdrop())
    std::array<float, kNodes> nodeGlow_{};           // what the routing graph shows of each node's halo, following it
    std::vector<std::pair<int, float>> wireAmounts_; // and of each wire's amount, by route
    float radarLevel_ = 0.0f; // the output's level the glow has risen or fallen to, 0 to 1 (see followOutput())
    bool algorithmMenuOpen_ = false; // the panel of algorithms, under ALGORITHM
    int algorithmOps_ = 6;           // the set it shows: the algorithms of this many operators (see kAlgorithmSets)
    // What its open context menu was opened on: a route (Disconnect, its modes, Reset Bend), or a node (Switch Off).
    int menuRoute_ = -1;
    int menuNode_ = -1;
};

} // namespace fmvibe::gui
