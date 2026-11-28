#pragma once

// The inspector (see inspectorpanel.cpp): the lens floating over the routing panel's left end, which shows the node
// picked (an operator, a filter or a shaper) on a page of its own: the nodes' tabs along its top, its title (the node's
// letter, what it is and what it is routed to) beside its on/off switch, and its page. An operator's: its waves (the
// common ones as buttons, the rest in a list), its Ratio readout or, fixed, its Freq and Mult, its knobs, and its tabs:
// UNISON (where the copies sit; a noise operator's spectrum), ENVELOPE, WARP (its warps over its scope), I/O (a row
// per route into and out of it) and KEYS (its keyboard level scaling, its level across the keys with handles on it).
// A filter's: its type's and slope's lists, its response with its handle, and its knobs. A shaper's: its curve's list
// and switches, its curve (drawn in place, see ShapeEditor), and its knobs. A filter's and a shaper's page end with
// knobs for what feeds them. The View lists its widgets in the panels' row while it shows (see covered()), and its
// popups (the list of more waves, its pages' lists) with the others'.
//
// Its knobs, switches, lists, a route's depth on an I/O row and the envelope on the ENVELOPE tab are the View's to
// paint and edit, as they are wherever they show (the envelope's handles and zoom are shared with the mod envelopes'
// cards, see PanelHost::envelopeWidgets()); the panel adds what is its own: the tabs, the waves and their list, the
// scope's cycles, the I/O rows' scrolling, the filter's handle, the Fixed switch's Freq and Mult, and a node's tab
// or its name wherever it shows (the matrix's, an I/O row's), whose click shows that node.

#include "modulationmodel.h"
#include "panel.h"
#include "rateshape.h"
#include "routingmodel.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class ShapeEditor;

class InspectorPanel : public Panel {
public:
    // A voice shaper's drawn curve shows on its page through `shapes`, which also says what covers the panels' row.
    InspectorPanel(PanelHost &host, ShapeEditor &shapes);

    // Whether something else takes its place: the FX, MODS and SEQ views, or a full-size shape editor (but the note
    // source's, velocity's and a voice shaper's, which leave it beside them). The View lists, paints and hands it
    // nothing of its own meanwhile.
    bool covered() const;
    // The operator whose scope shows (its WARP tab, while it shows), whose output the audio thread captures; or -1.
    int scopeShown() const;
    // The list of more waves closes, as an open list does as something scrolls under the middle button.
    void closeWaveMenu() { waveMenu_ = -1; }

    void widgets(std::vector<Widget> &list) const override;
    void popups(std::vector<Widget> &list) const override; // the list of more waves, and its pages' lists
    void paintBack(Canvas &c) override;                     // the lens and the page shown
    void paintOverWidgets(Canvas &c) override;              // a wave button's name, under it while the mouse is on it
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    void forgetScoped() override;
    void poll(const Telemetry &now, std::vector<Rect> &repaint) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doubleClick(const Widget &w, Point p, Modifiers mods) override;
    void drag(const DragState &d) override;
    void release() override;
    bool dragsOnTouch(const Widget &w) const override;
    bool wheelOn(const Widget &w, Point p, float steps, Modifiers mods) override;
    bool wheelBehind(Point p, float steps) override;
    bool blocksPan(const Widget &w) const override;
    std::optional<std::pair<int, float>> panAt(Point p) const override;
    std::optional<float> pan(int part, float first, float across, float down) override;

private:
    using Kind = WidgetKind;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return mods_.value(index); }
    void edit(int index, double v) { mods_.edit(index, v); }

    // Its drags: the filter's handle (its cutoff across, its resonance up and down, one gesture), a keyboard scaling
    // handle on the KEYS tab, a Rate/Level envelope's point, and the I/O tab's scrollbar's thumb. What each holds and
    // where the thumb's rows were when it was grabbed.
    enum class DragKind { None, FilterPoint, KeyPoint, InputsScroll, RatePoint };
    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        float startValue = 0.0f;
        float startLevel = 0.0f; // a Rate/Level breakpoint's level, startValue its rate
        double startDepth = 0.0; // a keyboard scaling depth's
    };

    // Noise has no pitch or unison copies: just a slope and a stereo spread.
    bool isNoise(int op) const;
    // Plays at its Freq x Mult rather than following the note (see OpParam::Fixed).
    bool isFixed(int op) const;
    bool isCustomWave(int op) const; // its wave is drawn from partials
    std::string fixedHzText(int op, bool compact) const;
    // Switched to fixed, the operator plays where its ratio put it at A4 (see the Fixed switch).
    void switchFixed(int op);
    // A node's tab, or its name, clicked: the node shown switches on and off; another shows, switched on if it was off,
    // and the partial editor of the operator shown before closes.
    void showNode(int node);
    // Every modulation, as the View has them (ModulationModel::assignments()), kept while the host's scope goes on.
    std::vector<Assignment> assignments() const;
    // The tab an operator's page shows: the one picked, but a noise operator's envelope for the scope it hasn't.
    InspectorTab opTabShown(int op) const;
    bool opEnvDrawn(int op) const; // its ENVELOPE tab shows its drawn envelope (Curve), not its AHDSR
    bool opEnvRates(int op) const; // or its Rate/Level envelope
    // Whether a source modulates one of the knobs on an operator's tab, which shows a ring on the tab.
    bool opTabModulated(int op, InspectorTab tab) const;
    OpWarp opWarp(int op) const;
    bool opSynced(int op) const;
    std::vector<Point> opWavePoints(int op, Rect r, int cycles, bool plain) const;
    // A row of a node's I/O list (an operator's on its I/O tab, a filter's or a shaper's under its display): a section's
    // heading (INPUTS or OUTPUTS), its line saying it has no route, a route in from `node`, its feedback, a route out to
    // `node` or its line to OUT; `at` its top, in px down the list.
    struct IoRow {
        enum class Kind { Heading, None, In, Feedback, Out, Bus };
        Kind kind;
        int node;
        bool outputs; // in the OUTPUTS section
        float at;
    };
    // Its rows, the nodes that are on in the graph's order: the inputs, the feedback, then the outputs and OUT. How
    // long they are, how far they scroll at most and where they are scrolled to, in rows of kInputsStep; where a row
    // is drawn, and the scrollbar's thumb.
    std::vector<IoRow> ioRows(int node) const;
    static float ioLength(const std::vector<IoRow> &rows);
    static float ioTop(int node); // where its list starts (see kInputsTop, kBoxIoTop)
    bool ioShown(int node) const; // its list shows: a filter's and a shaper's always, an operator's on its I/O tab
    float ioEnd(int node) const;
    float ioFirst(int node) const;
    float ioY(int node, float at) const;
    Rect ioThumb(int node) const;
    // Whether the rows of the node shown's I/O list show at `p`, scrolling (more than fit).
    bool ioScrollAt(Point p) const;
    // What a filter's display reads (see FilterReads): the values, where modulation takes them now and how far.
    std::pair<double, double> reach(int param, double set) const;

    // Painting: a page's title, a filter's or shaper's routes for its summary, and the pages' displays.
    void paintPageTitle(Canvas &c, int node, Rect r, const std::string &head, const std::string &rest);
    std::string boxRoutes(int node) const;
    // A shaper's page in the inspector: its transfer curve, and the label over its inputs.
    void paintShaperPage(Canvas &c, int shaper);
    void paintUnison(Canvas &c, Rect r, int op);
    void paintScope(Canvas &c, Rect r, int op);
    void paintNoiseSlope(Canvas &c, Rect r, int op);
    void paintIo(Canvas &c, int node);
    // The KEYS tab: where a handle sits (0 the breakpoint, 1 and 2 the left and right depth, on the curve near its
    // ends), the depth a depth's handle dragged to `moved` dB on the display from `start` sets, the operator's level
    // across the keys, and a handle's readout over the display while it is under the mouse or held.
    Point keyPoint(int op, int handle) const;
    double keyDepthDragged(int op, int handle, double start, double moved) const;
    void paintKeyScale(Canvas &c, int op);
    void paintRateEnvelope(Canvas &c, int op);
    void paintRateReadout(Canvas &c, const Widget &w);
    // Its Rate/Level envelope as the display draws it (see rateShapeOf()), kept while its settings stay.
    const RateShape &rateShape(int op) const;
    void paintKeyReadout(Canvas &c, const Widget &w);

    PanelHost &host_;
    ShapeEditor &shapes_;
    // The patch's modulation and routing, their models telling the editor why they refused an edit (see
    // PanelHost::flash()).
    ModulationModel &mods_;
    RoutingModel &routing_;
    const ParamTable &table_;
    Drag drag_;
    int waveMenu_ = -1;          // the operator whose list of more waves is open, or -1
    float ioScroll_ = 0.0f;      // rows the I/O list is scrolled to, before clamping
    float opScopeLevel_ = 0.0f;  // the operator scope's level, following its spans' peaks, which its dB readout shows
    ScopeCache<std::vector<Assignment>> assignments_;
    mutable std::vector<double> rateKey_; // what rateShape_ was traced from
    mutable RateShape rateShape_;
    int rateSegment_ = 0; // the segment of a Rate/Level envelope its knobs set (0-3)
};

} // namespace fmvibe::gui
