#include "controller.h"

#include <algorithm>

namespace fmvibe::gui {

namespace {

// The parts of the layout that belong to the patch, which undo and redo bring back with the params.
bool samePatchLayout(const UiLayout &a, const UiLayout &b) {
    return a.nodeX == b.nodeX && a.nodeY == b.nodeY && a.boxX == b.boxX && a.boxY == b.boxY &&
           a.sourceOrder == b.sourceOrder && a.keptRoutes == b.keptRoutes && a.macroNames == b.macroNames &&
           a.wireBends == b.wireBends && a.emptyMods == b.emptyMods;
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
}

} // namespace

void Controller::beginEdit(int index) {
    if (!applying_) {
        beginStep();
        record(index);
    }
    performBeginEdit(index);
}

void Controller::setValue(int index, double value) {
    if (applying_) {
        performSetValue(index, value);
        return;
    }
    // An edit outside a gesture is a step of its own.
    beginStep();
    record(index);
    performSetValue(index, value);
    endStep();
}

void Controller::endEdit(int index) {
    performEndEdit(index);
    if (!applying_)
        endStep();
}

void Controller::beginStep(bool merges) {
    if (applying_ || depth_++ > 0)
        return;
    open_ = UndoStep{};
    open_.before = layout();
    open_.merges = merges;
    open_.presetBefore = preset_;
    open_.editedBefore = edited_;
    presetSet_ = false;
    recorded_.assign(kParamCount, false);
}

void Controller::setPreset(const std::wstring &file, bool edited) {
    preset_ = file;
    edited_ = edited;
    presetSet_ = depth_ > 0;
}

void Controller::endStep() {
    if (applying_ || depth_ == 0 || --depth_ > 0)
        return;
    UndoStep step = std::move(open_);
    open_ = UndoStep{};
    for (UndoStep::Change &change : step.changes)
        change.after = value(change.index);
    std::erase_if(step.changes, [](const UndoStep::Change &change) { return change.after == change.before; });
    step.after = layout();
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

    // A wheel turn on the param the last one turned joins it, unless something was undone since.
    if (step.merges && redo_.empty() && !undo_.empty()) {
        UndoStep &last = undo_.back();
        if (last.merges && !last.layoutChanged && !step.layoutChanged && last.changes.size() == 1 &&
            step.changes.size() == 1 && last.changes[0].index == step.changes[0].index) {
            last.changes[0].after = step.changes[0].after;
            last.editedAfter = step.editedAfter;
            if (last.changes[0].after == last.changes[0].before) { // turned back to where it started
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

void Controller::record(int index) {
    // The mod wheel is played, not part of the patch.
    if (depth_ == 0 || index < 0 || index >= kParamCount || recorded_[index] ||
        index == paramIndex(GlobalParam::ModWheel))
        return;
    recorded_[index] = true;
    open_.changes.push_back({index, value(index), 0.0});
}

void Controller::recordAll() {
    for (int index = 0; index < kParamCount; ++index)
        record(index);
}

void Controller::undo() {
    if (depth_ > 0 || undo_.empty())
        return;
    UndoStep step = std::move(undo_.back());
    undo_.pop_back();
    apply(step, false);
    redo_.push_back(std::move(step));
}

void Controller::redo() {
    if (depth_ > 0 || redo_.empty())
        return;
    UndoStep step = std::move(redo_.back());
    redo_.pop_back();
    apply(step, true);
    undo_.push_back(std::move(step));
}

// As one gesture, in the order the step's params were first edited, so an effect's or source's
// type is set after its fields, as when it was added.
void Controller::apply(const UndoStep &step, bool forward) {
    applying_ = true;
    preset_ = forward ? step.presetAfter : step.presetBefore;
    edited_ = forward ? step.editedAfter : step.editedBefore;
    for (const UndoStep::Change &change : step.changes)
        performBeginEdit(change.index);
    for (const UndoStep::Change &change : step.changes)
        performSetValue(change.index, forward ? change.after : change.before);
    for (auto it = step.changes.rbegin(); it != step.changes.rend(); ++it)
        performEndEdit(it->index);
    if (step.layoutChanged) {
        restorePatchLayout(layout(), forward ? step.after : step.before);
        layoutChanged();
    }
    applying_ = false;
}

void Controller::clearHistory() {
    undo_.clear();
    redo_.clear();
    if (depth_ > 0) { // a gesture still open starts over from here
        open_.changes.clear();
        open_.before = layout();
        open_.presetBefore = preset_;
        open_.editedBefore = edited_;
        presetSet_ = false;
        recorded_.assign(kParamCount, false);
    }
}

} // namespace fmvibe::gui
