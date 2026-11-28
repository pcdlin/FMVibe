#include "patchedits.h"

#include "routing.h"
#include "routingmodel.h"

#include <algorithm>
#include <cmath>

namespace fmvibe::gui {

using namespace detail;

PatchEdits::PatchEdits(Controller &controller, ModelMessages messages)
    : controller_(controller), table_(paramTable()), messages_(std::move(messages)) {}

void PatchEdits::say(std::string message) const {
    if (messages_)
        messages_(std::move(message));
}

double PatchEdits::norm(int index) const {
    const ParamDef &def = table_[index];
    if (def.kind == ParamKind::VoiceCount) // 1..max on a log scale
        return std::log(value(index)) / std::log(def.max);
    return (value(index) - def.min) / (def.max - def.min);
}

void PatchEdits::edit(int index, double v) {
    if (!std::isfinite(v)) // std::clamp would pass a NaN on
        return;
    const ParamDef &def = table_[index];
    controller_.beginEdit(index);
    // A node switched on comes back into the graph clear of the others, in the same undo step.
    int switchedOn = -1;
    if (v >= 0.5 && value(index) < 0.5)
        for (int node = 0; node < kNodes; ++node)
            if (enabledParam(node) == index)
                switchedOn = node;
    controller_.setValue(index, std::clamp(v, def.min, def.max));
    if (switchedOn >= 0) {
        RoutingModel(controller_, messages_).placeSwitchedOn(switchedOn);
        controller_.layout().hiddenNodes &= ~(1u << switchedOn); // switched off again, it shows dim
    }
    // A route switched on is kept in the same undo step, which takes it out again with its value; kept only by the
    // next poll, after the step, undo would leave its wire at 0 %.
    if (value(index) > 0.0 && isRouteParam(index))
        RoutingModel(controller_, messages_).keepRoutes();
    controller_.endEdit(index);
}

void PatchEdits::editMany(const std::vector<std::pair<int, double>> &changes) {
    for (const auto &[index, v] : changes)
        controller_.beginEdit(index);
    for (const auto &[index, v] : changes)
        controller_.setValue(index, std::clamp(v, table_[index].min, table_[index].max));
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        controller_.endEdit(it->first);
}

bool PatchEdits::folded(bool fx, int index) const {
    const UiLayout &layout = controller_.layout();
    if (!fx)
        return (layout.sourcesFolded >> index) & 1u;
    return index < kFirstFxSlots ? (layout.fxFolded >> index) & 1u : (layout.fxFoldedLater >> (index - kFirstFxSlots)) & 1u;
}

void PatchEdits::setFolded(bool fx, int index, bool folded) {
    UiLayout &layout = controller_.layout();
    const bool later = fx && index >= kFirstFxSlots; // the slots past the first 16 have bits of their own
    uint32_t &bits = later ? layout.fxFoldedLater : fx ? layout.fxFolded : layout.sourcesFolded;
    const int bit = later ? index - kFirstFxSlots : index;
    const uint32_t before = bits;
    bits = folded ? bits | (1u << bit) : bits & ~(1u << bit);
    if (bits != before)
        controller_.layoutChanged();
}

} // namespace fmvibe::gui
