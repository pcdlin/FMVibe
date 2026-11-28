#pragma once

// The routing graph as the editor edits it (see patchedits.h): which nodes are on and where they sit, the routes
// between them and out (which show their wires, how far those bow), connecting, moving and disconnecting them with
// their modulation, and the classic algorithms set on the operators. The geometry the routes are drawn with is
// routing.h's.

#include "canvas.h"
#include "modulationmodel.h"
#include "patchedits.h"

#include <vector>

namespace fmvibe::gui {

class RoutingModel : public PatchEdits {
public:
    using PatchEdits::PatchEdits;

    bool nodeOn(int node) const;
    std::vector<int> enabledNodes() const;
    // The nodes the graph shows in their places (see graphNodes() in routing.h): those on, and a node off that is still
    // wired, which shows dim (nodeDim()) until it is hidden. hideNode() docks such a node as a ghost under
    // OUT, as an undo step, as disconnecting or moving its last route does; switching it on again (PatchEdits::edit())
    // shows it as before.
    std::vector<int> shownNodes() const;
    bool nodeShown(int node) const;
    bool nodeDim(int node) const { return nodeShown(node) && !nodeOn(node); }
    void hideNode(int node);
    // A node's center in the editor, from where the layout puts it in the graph.
    Point nodeCenter(int node) const;
    // Where a node dragged to `want` goes: there, unless it would cover another node shown (see routingmodel.cpp).
    Point nodePlace(int node, Point want, bool *found = nullptr) const;
    void placeSwitchedOn(int node); // moves a node just switched on off the others shown, as part of the edit

    // Whether a route shows its wire: it is on, or it was and has only been turned down to 0 %.
    bool wired(int param) const;
    // Whether the graph shows `node`'s line to OUT: it is wired, and a filter has something to put out.
    bool goesOut(int node) const;
    // Whether `node`'s own sound reaches the output, as the dot on its tab shows: its Out is up, an operator is on, and
    // a filter or shaper has something going into it.
    bool soundsOut(int node) const;
    // Every route that is on kept (UiLayout::keptRoutes), so it shows even once turned down to 0 %.
    void keepRoutes();
    void disconnect(int param); // to 0 %, and its wire goes
    void connect(int from, int to);
    // Moves the route from `from` into `to` over to `into` (kNodes: the output), with its amount and modulation.
    void reroute(int from, int to, int into);
    // Moves the route from `from` into `to` (kNodes: the output) over to come from `source`, with its amount, mode and
    // modulation.
    void rerouteFrom(int from, int to, int source);
    void switchOnRoute(int from, int to);
    // Whether sound gets into a filter or shaper: from an operator that is on, or a filter or shaper that has some.
    bool boxHasInput(int node) const;
    // Whether a filter's or shaper's own sound gets to the output: it goes out, or into a filter or shaper whose sound
    // does.
    bool boxGoesOut(int node) const;
    // Whether an operator's sound gets to the output through a filter or shaper that is on, which it is sent into.
    bool outThroughBox(int op) const;

    // How far a route's wire bows to its left, as a share of the distance it spans: as dragged, or kDefaultWireBend.
    float wireBend(int param) const;
    void setWireBend(int param, float bend); // its default forgets it
    static float defaultWireBend(int param); // kDefaultWireBend, or 0 for a node's line to OUT

    // The classic algorithms (kAlgorithms in algorithms.h). applyAlgorithm() sets the operators to the one at `index`,
    // as one undo step: those it uses on and the others off, its routes among them in (a route it has keeps its depth,
    // a new one starts at kAlgorithmDepth, its feedback at kAlgorithmFeedback) and theirs not in it disconnected with
    // their modulation, its carriers going out (one going out already, or through a filter or shaper that does, as it
    // was; a new one at 1 / sqrt(carriers)) and the others not (their sends into a filter or shaper that goes out
    // disconnected too), and the operators in its places (see algorithmPlaces()), their wires bowing as new ones do.
    // Filters, shapers, the carriers' sends into them and every operator's own sound stay as they are.
    // currentAlgorithm() is the one the operators that are on form now (their routes above 0 % or kept at 0 %,
    // whatever their depths; one going out through a filter or shaper counting as a carrier), -1 for none.
    static constexpr double kAlgorithmDepth = 0.5, kAlgorithmFeedback = 0.25;
    void applyAlgorithm(int index);
    int currentAlgorithm() const;

private:
    // The route `param` moved onto `next`, which isn't wired yet: its depth, mode and modulation go along, undoably, and
    // the filter or shaper `goesOut` (if there is one) goes out.
    void moveRoute(int param, int next, int goesOut = -1);
    // Hides (UiLayout::hiddenNodes) the nodes shown dim `before` an edit that no longer show: their last
    // connection went.
    void hideUnwired(const std::vector<int> &before);

    ModulationModel modulation() const { return ModulationModel(controller_, messages_); }
};

} // namespace fmvibe::gui
