#pragma once

// What the editor's models of the patch stand on (the modulation's, modulationmodel.h; the routing graph's,
// routingmodel.h; the effects graph's, fxgraphmodel.h): the params, read and edited through the Controller as the
// editor edits them, the layout saved with them, and the message an edit refused leaves, which the editor flashes up.
// They are window-free and know nothing of the View: they hold no state of their own but the Controller and where
// their messages go, so one can be made wherever it is needed, and a tool can edit a patch through them as the editor
// does.

#include "controller.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

// Where a model's messages go: why it refused an edit ("LFO 1 modulates Cutoff already"). The editor flashes them up;
// left empty, they go nowhere.
using ModelMessages = std::function<void(std::string)>;

class PatchEdits {
public:
    explicit PatchEdits(Controller &controller, ModelMessages messages = {});

    double value(int index) const { return controller_.value(index); }
    // Where its value sits in its range, 0..1: the voice limit on its log scale, the others straight.
    double norm(int index) const;
    // One param set as a gesture of its own, held to its range. A node switched on comes back into the routing graph
    // clear of the others, and a route switched on is kept (see RoutingModel::keepRoutes()), in the same undo step.
    void edit(int index, double value);
    // Several params changed as one gesture.
    void editMany(const std::vector<std::pair<int, double>> &changes);
    // An effect's card (`fx`, by slot) or a source's (by place) folded to a strip, or not, in the layout; a new effect
    // or source starts unfolded.
    bool folded(bool fx, int index) const;
    void setFolded(bool fx, int index, bool folded);

protected:
    void say(std::string message) const;

    Controller &controller_;
    const ParamTable &table_;
    ModelMessages messages_;
};

} // namespace fmvibe::gui
