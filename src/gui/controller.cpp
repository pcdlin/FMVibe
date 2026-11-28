#include "controller.h"

#include <algorithm>
#include <cmath>

namespace fmvibe::gui {

namespace {

// The parts of the layout that belong to the patch, which undo and redo bring back with the params.
bool samePatchLayout(const UiLayout &a, const UiLayout &b) {
    return a.nodeX == b.nodeX && a.nodeY == b.nodeY && a.boxX == b.boxX && a.boxY == b.boxY &&
           a.sourceOrder == b.sourceOrder && a.keptRoutes == b.keptRoutes && a.macroNames == b.macroNames &&
           a.wireBends == b.wireBends && a.emptyMods == b.emptyMods && a.tuningName == b.tuningName &&
           a.mappingName == b.mappingName && a.hiddenNodes == b.hiddenNodes;
}

void restorePatchLayout(UiLayout &to, const UiLayout &from) {
    to.nodeX = from.nodeX;
    to.nodeY = from.nodeY;
    to.boxX = from.boxX;
    to.boxY = from.boxY;
    to.sourceOrder = from.sourceOrder;
    to.keptRoutes = from.keptRoutes;
    to.macroNames = from.macroNames;
    to.wireBends = from.wireBends;
    to.emptyMods = from.emptyMods;
    to.tuningName = from.tuningName;
    to.mappingName = from.mappingName;
    to.hiddenNodes = from.hiddenNodes;
}

} // namespace

std::array<int, kMcBands> UndoHistory::linkedBands(int index) const {
    std::array<int, kMcBands> fields{index, -1, -1};
    const int slot = fxSlotOf(index);
    if (slot < 0 || fxType(controller_.value(paramIndex(slot, FxParam::Type))) != FxType::MultiDyn ||
        controller_.value(paramIndex(slot, FxParam::MdLink)) < 0.5)
        return fields;
    const int offset = index - mdAboveIndex(slot, 0); // the bands' fields run Above, Down, Below, Up, Gain
    constexpr int kBandFields = mdAboveIndex(0, 1) - mdAboveIndex(0, 0);
    if (offset < 0 || offset >= kMcBands * kBandFields)
        return fields;
    int next = 1;
    for (int band = 0; band < kMcBands; ++band)
        if (band != offset / kBandFields)
            fields[next++] = mdAboveIndex(slot, band) + offset % kBandFields;
    return fields;
}

int UndoHistory::crossoverPartner(int index) const {
    const int split = splitOf(index);
    if (split < 0 || std::lround(controller_.value(paramIndex(split, SplitParam::Bands))) < kFxBands)
        return -1;
    if (index == paramIndex(split, SplitParam::Low))
        return paramIndex(split, SplitParam::High);
    if (index == paramIndex(split, SplitParam::High))
        return paramIndex(split, SplitParam::Low);
    return -1;
}

double UndoHistory::pushedCrossover(int index, double v) const {
    const int partner = crossoverPartner(index);
    const double third = std::pow(2.0, 1.0 / 3.0);
    const double hz = cutoffFromNorm(std::clamp(v, 0.0, 1.0));
    if (partner > index) // the low crossover moved: the high one at least a third of an octave above it
        return std::max(controller_.value(partner),
                        std::min(normFromCutoff(std::min(hz * third, kMaxCutoff)), 1.0));
    // The high one: the low one below.
    return std::min(controller_.value(partner), std::max(normFromCutoff(hz / third), 0.0));
}

std::vector<int> UndoHistory::bendFollowers(int index) const {
    std::vector<int> depths;
    if (index != paramIndex(GlobalParam::BendRange))
        return depths;
    const double range = controller_.value(index);
    for (int slot = 0; slot < kSlots; ++slot) {
        const int target = slotTarget(controller_.value(slotTargetIndex(kBendSource, slot)));
        int op = -1;
        for (int o = 0; o < kOps && target >= 0; ++o)
            if (target == paramIndex(o, OpParam::Transpose))
                op = o;
        if (op < 0 || controller_.value(paramIndex(op, OpParam::Fixed)) < 0.5)
            continue;
        const int depth = slotAmountIndex(kBendSource, slot);
        if (std::abs(std::abs(controller_.value(depth)) - bendDepth(1.0, range)) < 1e-9)
            depths.push_back(depth);
    }
    return depths;
}

double UndoHistory::bendDepth(double depth, double range) {
    // Bipolar, half its depth bent fully up, of Pitch's span: as far as Bend Range.
    const ParamDef &pitch = paramTable()[paramIndex(0, OpParam::Transpose)];
    return (depth < 0.0 ? -1.0 : 1.0) * 2.0 * range / (pitch.max - pitch.min);
}

void UndoHistory::beginEdit(int index) {
    const auto fields = applying_ ? std::array<int, kMcBands>{index, -1, -1} : linkedBands(index);
    const int partner = applying_ ? -1 : crossoverPartner(index);
    if (!applying_) {
        beginStep();
        for (int field : fields)
            record(field);
        if (partner >= 0)
            record(partner);
    }
    for (int field : fields)
        if (field >= 0)
            controller_.performBeginEdit(field);
    if (partner >= 0)
        controller_.performBeginEdit(partner);
    if (!applying_ && index == paramIndex(GlobalParam::BendRange)) {
        bendGesture_ = bendFollowers(index);
        for (int depth : bendGesture_) {
            record(depth);
            controller_.performBeginEdit(depth);
        }
    }
}

void UndoHistory::setValue(int index, double value) {
    if (applying_) {
        controller_.performSetValue(index, value);
        return;
    }
    // An edit outside a gesture is a step of its own.
    const auto fields = linkedBands(index);
    const int partner = crossoverPartner(index);
    beginStep();
    for (int field : fields)
        record(field);
    if (partner >= 0)
        record(partner);
    followBend(index, value);
    for (int field : fields)
        if (field >= 0)
            controller_.performSetValue(field, value);
    if (partner >= 0)
        if (const double pushed = pushedCrossover(index, value); pushed != controller_.value(partner))
            controller_.performSetValue(partner, pushed);
    endStep();
}

void UndoHistory::followBend(int index, double value) {
    for (int depth : bendFollowers(index)) { // picked before Bend Range moves
        record(depth);
        controller_.performSetValue(depth, bendDepth(controller_.value(depth), value));
    }
}

void UndoHistory::endEdit(int index) {
    const auto fields = applying_ ? std::array<int, kMcBands>{index, -1, -1} : linkedBands(index);
    const int partner = applying_ ? -1 : crossoverPartner(index);
    for (int field : fields)
        if (field >= 0)
            controller_.performEndEdit(field);
    if (partner >= 0)
        controller_.performEndEdit(partner);
    if (!applying_) {
        if (index == paramIndex(GlobalParam::BendRange)) {
            for (int depth : bendGesture_)
                controller_.performEndEdit(depth);
            bendGesture_.clear();
        }
        endStep();
    }
}

void UndoHistory::beginStep(bool merges) {
    if (applying_ || depth_++ > 0)
        return;
    open_ = UndoStep{};
    open_.before = controller_.layout();
    open_.merges = merges;
    open_.presetBefore = preset_;
    open_.editedBefore = edited_;
    presetSet_ = false;
    recorded_.assign(kParamCount, false);
}

void UndoHistory::setPreset(const std::wstring &file, bool edited) {
    preset_ = file;
    edited_ = edited;
    presetSet_ = depth_ > 0;
}

void UndoHistory::endStep() {
    if (applying_ || depth_ == 0 || --depth_ > 0)
        return;
    UndoStep step = std::move(open_);
    open_ = UndoStep{};
    for (UndoStep::Change &change : step.changes)
        change.after = controller_.value(change.index);
    std::erase_if(step.changes, [](const UndoStep::Change &change) { return change.after == change.before; });
    step.after = controller_.layout();
    step.layoutChanged = !samePatchLayout(step.before, step.after);
    if (!step.layoutChanged)
        step.before = step.after = UiLayout{};
    // An edit leaves the patch changed; a step that set the preset lands on what it set.
    const bool changed = !step.changes.empty() || step.layoutChanged;
    step.presetAfter = preset_;
    step.editedAfter = presetSet_ ? edited_ : changed || step.editedBefore;
    presetSet_ = false;
    if (!changed && step.presetAfter == step.presetBefore && step.editedAfter == step.editedBefore)
        return;
    edited_ = step.editedAfter;

    // A wheel turn on the param the last one turned joins it, unless something was undone since. The first turn may
    // have changed the layout (a route it switched on is kept); the turns after it join it as long as they don't. The
    // param turned is the first each records; what it moved along with it (linked bands, a crossover it pushed) joins
    // too, whichever turns moved it.
    if (step.merges && redo_.empty() && !undo_.empty()) {
        UndoStep &last = undo_.back();
        if (last.merges && !step.layoutChanged && !last.changes.empty() && !step.changes.empty() &&
            last.changes[0].index == step.changes[0].index) {
            for (const UndoStep::Change &change : step.changes) {
                const auto it = std::find_if(last.changes.begin(), last.changes.end(),
                                             [&](const UndoStep::Change &c) { return c.index == change.index; });
                if (it != last.changes.end())
                    it->after = change.after;
                else
                    last.changes.push_back(change);
            }
            last.editedAfter = step.editedAfter;
            // Turned back to where it started: nothing left to undo, unless the layout changed with it.
            if (!last.layoutChanged && std::all_of(last.changes.begin(), last.changes.end(), [](const UndoStep::Change &c) {
                    return c.after == c.before;
                })) {
                edited_ = last.editedBefore;
                undo_.pop_back();
            }
            return;
        }
    }
    redo_.clear();
    undo_.push_back(std::move(step));
    if (undo_.size() > kMaxUndoSteps)
        undo_.erase(undo_.begin());
}

void UndoHistory::record(int index) {
    // The mod wheel and the pitch bend are played, not part of the patch.
    if (depth_ == 0 || index < 0 || index >= kParamCount || recorded_[index] || isPlayed(index))
        return;
    recorded_[index] = true;
    open_.changes.push_back({index, controller_.value(index), 0.0});
}

void UndoHistory::recordAll() {
    for (int index = 0; index < kParamCount; ++index)
        record(index);
}

void UndoHistory::keepSeqPicks(int pattern, std::vector<int> before, std::vector<int> after) {
    if (applying_ || depth_ == 0)
        return;
    open_.seqPattern = pattern;
    open_.seqPickedBefore = std::move(before);
    open_.seqPickedAfter = std::move(after);
}

const UndoStep *UndoHistory::undo() {
    if (depth_ > 0 || undo_.empty())
        return nullptr;
    UndoStep step = std::move(undo_.back());
    undo_.pop_back();
    apply(step, false);
    redo_.push_back(std::move(step));
    return &redo_.back();
}

const UndoStep *UndoHistory::redo() {
    if (depth_ > 0 || redo_.empty())
        return nullptr;
    UndoStep step = std::move(redo_.back());
    redo_.pop_back();
    apply(step, true);
    undo_.push_back(std::move(step));
    return &undo_.back();
}

// As one gesture, in the order the step's params were first edited, so an effect's or source's
// type is set after its fields, as when it was added.
void UndoHistory::apply(const UndoStep &step, bool forward) {
    applying_ = true;
    controller_.performStepBegin();
    preset_ = forward ? step.presetAfter : step.presetBefore;
    edited_ = forward ? step.editedAfter : step.editedBefore;
    for (const UndoStep::Change &change : step.changes)
        controller_.performBeginEdit(change.index);
    for (const UndoStep::Change &change : step.changes)
        controller_.performSetValue(change.index, forward ? change.after : change.before);
    for (auto it = step.changes.rbegin(); it != step.changes.rend(); ++it)
        controller_.performEndEdit(it->index);
    if (step.layoutChanged) {
        restorePatchLayout(controller_.layout(), forward ? step.after : step.before);
        controller_.layoutChanged();
    }
    controller_.performStepEnd();
    applying_ = false;
}

void UndoHistory::clear() {
    ++patchLoads_;
    undo_.clear();
    redo_.clear();
    if (depth_ > 0) { // a gesture still open starts over from here
        open_.changes.clear();
        open_.before = controller_.layout();
        open_.presetBefore = preset_;
        open_.editedBefore = edited_;
        presetSet_ = false;
        recorded_.assign(kParamCount, false);
    }
}

// A Controller that keeps no presets: one store, without files, does for all of them.
PresetStore &Controller::presetStore() {
    static NoPresetFiles none;
    return none;
}

} // namespace fmvibe::gui
