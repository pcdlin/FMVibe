#include "controller.h"

#include <algorithm>
#include <cmath>

namespace fmvibe::gui {

namespace {

// The parts of the layout that belong to the patch, which undo and redo bring back with the params.
bool samePatchLayout(const UiLayout &a, const UiLayout &b) {
    return a.nodeX == b.nodeX && a.nodeY == b.nodeY && a.boxX == b.boxX && a.boxY == b.boxY &&
           a.sourceOrder == b.sourceOrder && a.keptRoutes == b.keptRoutes && a.macroNames == b.macroNames &&
           a.wireBends == b.wireBends && a.emptyMods == b.emptyMods && a.plainWave == b.plainWave;
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
    to.plainWave = from.plainWave;
}

// Whether a change to param `index` can change which data the patch names or plays.
bool touchesData(int index) {
    if (isDataKeyParam(index) || waveParamOp(index) >= 0)
        return true;
    return index >= kOpBase && index < kOpBase + kOps * kOpParamCount &&
           (index - kOpBase) % kOpParamCount == static_cast<int>(OpParam::Enabled);
}

} // namespace

void Controller::beforeSet(int index, double value) {
    if (isDataKeyParam(index)) {
        if (const uint32_t key = dataKey(value);
            key != 0 || (index - kOpBase) % kOpParamCount == static_cast<int>(OpParam::Table))
            data_.ensure(key);
        return;
    }
    int op = waveParamOp(index);
    OpWave wave = op >= 0 ? opWave(value) : OpWave::Sine;
    if (op < 0 && touchesData(index) && value >= 0.5) { // an operator switched on
        op = (index - kOpBase) / kOpParamCount;
        wave = opWave(this->value(paramIndex(op, OpParam::Wave)));
    }
    if (op < 0)
        return;
    if (wave == OpWave::Wavetable)
        data_.ensure(dataKey(this->value(paramIndex(op, OpParam::Table))));
    else if (wave == OpWave::Sample)
        if (const uint32_t key = dataKey(this->value(paramIndex(op, OpParam::Sample))); key != 0)
            data_.ensure(key);
}

void Controller::refreshData() {
    std::vector<uint32_t> used, held;
    for (int op = 0; op < kOps; ++op) {
        const uint32_t table = dataKey(value(paramIndex(op, OpParam::Table)));
        const uint32_t sample = dataKey(value(paramIndex(op, OpParam::Sample)));
        held.push_back(table);
        if (sample != 0)
            held.push_back(sample);
        if (value(paramIndex(op, OpParam::Enabled)) < 0.5)
            continue;
        const OpWave wave = opWave(value(paramIndex(op, OpParam::Wave)));
        if (wave == OpWave::Wavetable)
            used.push_back(table);
        else if (wave == OpWave::Sample && sample != 0)
            used.push_back(sample);
    }
    for (const std::vector<UndoStep> *steps : {&undo_, &redo_})
        for (const UndoStep &step : *steps)
            for (const UndoStep::Change &change : step.changes)
                if (isDataKeyParam(change.index)) {
                    held.push_back(dataKey(change.before));
                    held.push_back(dataKey(change.after));
                }
    data_.keep(used, held, dataBlocks(), dataIdle());
}

std::string Controller::importOperatorData(int op, DataKind kind, const std::vector<uint8_t> &bytes,
                                           const std::string &name, const std::string &origin, int frameSize) {
    if (op < 0 || op >= kOps)
        return {};
    TableFile file = readTableFile(bytes.data(), bytes.size(), name,
                                   kind == DataKind::Table ? ReadAs::Table : ReadAs::Sample, frameSize);
    if (file.error != ReadError::None)
        return trf(Str::MsgImportFailed, {name, tr(readErrorText(file.error))});
    auto data = std::make_shared<OpData>();
    data->kind = file.sample ? DataKind::Sample : DataKind::Table;
    const size_t slash = name.find_last_of("/\\");
    data->name = slash == std::string::npos ? name : name.substr(slash + 1);
    data->origin = origin;
    data->format = file.format;
    data->rate = file.sample ? file.rate : 0;
    data->frames = file.frames;
    data->root = file.root;
    data->loopStart = file.loopStart;
    data->loopEnd = file.loopEnd;
    data->int16 = file.sixteenBit;
    data->pcm = std::move(file.data);
    if (data->int16) // as the patch will store it, so the key it hashes to stays as it is loaded back
        for (float &x : data->pcm)
            x = static_cast<float>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f)) / 32767.0f;
    // What the patch holds would go past its limits: refused.
    const size_t bytesNeeded = data->pcm.size() * (data->int16 ? 2 : 4);
    size_t total = bytesNeeded;
    std::vector<uint32_t> counted;
    for (int other = 0; other < kOps; ++other)
        for (const OpParam p : {OpParam::Table, OpParam::Sample})
            if (const uint32_t key = dataKey(value(paramIndex(other, p)));
                key >= kUserDataBase && std::find(counted.begin(), counted.end(), key) == counted.end()) {
                counted.push_back(key);
                if (const OpData *d = data_.find(key))
                    total += d->pcm.size() * (d->int16 ? 2 : 4);
            }
    if (bytesNeeded > kMaxDataEntryBytes || total > kMaxDataChunkBytes)
        return trf(Str::MsgImportFailed, {data->name, tr(Str::MsgImportTooBig)});
    buildOpData(*data);
    const bool sample = data->kind == DataKind::Sample;
    struct {
        float root;
        uint32_t loopStart, loopEnd;
        int frames;
    } const stored{data->root, data->loopStart, data->loopEnd, data->frames};
    const uint32_t key = data_.add(std::move(data));
    UndoScope scope(*this);
    const int wave = paramIndex(op, OpParam::Wave);
    if (const OpWave was = opWave(value(wave)); !isDataWave(was) && layout().plainWave[op] != static_cast<int>(was)) {
        layout().plainWave[op] = static_cast<int>(was);
        layoutChanged();
    }
    setValue(paramIndex(op, sample ? OpParam::Sample : OpParam::Table), static_cast<double>(key));
    setValue(wave, static_cast<double>(sample ? OpWave::Sample : OpWave::Wavetable));
    if (sample) {
        // Its root, and its loop if the file has one (else none), from the start.
        setValue(paramIndex(op, OpParam::SampleRoot), stored.root);
        setValue(paramIndex(op, OpParam::SampleStart), 0.0);
        const bool loop = stored.loopEnd > stored.loopStart && stored.frames > 1;
        const double last = std::max(stored.frames - 1, 1);
        setValue(paramIndex(op, OpParam::LoopMode), static_cast<double>(loop ? SampleLoop::Forward : SampleLoop::Off));
        setValue(paramIndex(op, OpParam::LoopStart), loop ? std::clamp(stored.loopStart / last, 0.0, 1.0) : 0.0);
        setValue(paramIndex(op, OpParam::LoopEnd), loop ? std::clamp(stored.loopEnd / last, 0.0, 1.0) : 1.0);
    }
    if (layout().opTab != InspectorTab::Source) {
        layout().opTab = InspectorTab::Source;
        layoutChanged();
    }
    return {};
}

void Controller::pickTable(int op, uint32_t key) {
    if (op < 0 || op >= kOps || !data_.get(key))
        return;
    UndoScope scope(*this);
    const int wave = paramIndex(op, OpParam::Wave);
    if (const OpWave was = opWave(value(wave)); !isDataWave(was) && layout().plainWave[op] != static_cast<int>(was)) {
        layout().plainWave[op] = static_cast<int>(was);
        layoutChanged();
    }
    setValue(paramIndex(op, OpParam::Table), static_cast<double>(key));
    setValue(wave, static_cast<double>(OpWave::Wavetable));
}

std::array<int, kMcBands> Controller::linkedBands(int index) const {
    std::array<int, kMcBands> fields{index, -1, -1};
    const int slot = fxSlotOf(index);
    if (slot < 0 || fxType(value(paramIndex(slot, FxParam::Type))) != FxType::MultiDyn ||
        value(paramIndex(slot, FxParam::MdLink)) < 0.5)
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

int Controller::crossoverPartner(int index) const {
    const int split = splitOf(index);
    if (split < 0 || std::lround(value(paramIndex(split, SplitParam::Bands))) < kFxBands)
        return -1;
    if (index == paramIndex(split, SplitParam::Low))
        return paramIndex(split, SplitParam::High);
    if (index == paramIndex(split, SplitParam::High))
        return paramIndex(split, SplitParam::Low);
    return -1;
}

double Controller::pushedCrossover(int index, double v) const {
    const int partner = crossoverPartner(index);
    const double third = std::pow(2.0, 1.0 / 3.0);
    const double hz = cutoffFromNorm(std::clamp(v, 0.0, 1.0));
    if (partner > index) // the low crossover moved: the high one at least a third of an octave above it
        return std::max(value(partner), std::min(normFromCutoff(std::min(hz * third, kMaxCutoff)), 1.0));
    return std::min(value(partner), std::max(normFromCutoff(hz / third), 0.0)); // the high one: the low one below
}

void Controller::beginEdit(int index) {
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
            performBeginEdit(field);
    if (partner >= 0)
        performBeginEdit(partner);
}

void Controller::setValue(int index, double value) {
    if (touchesData(index))
        beforeSet(index, value);
    if (applying_) {
        performSetValue(index, value);
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
    for (int field : fields)
        if (field >= 0)
            performSetValue(field, value);
    if (partner >= 0)
        if (const double pushed = pushedCrossover(index, value); pushed != this->value(partner))
            performSetValue(partner, pushed);
    endStep();
}

void Controller::endEdit(int index) {
    const auto fields = applying_ ? std::array<int, kMcBands>{index, -1, -1} : linkedBands(index);
    const int partner = applying_ ? -1 : crossoverPartner(index);
    for (int field : fields)
        if (field >= 0)
            performEndEdit(field);
    if (partner >= 0)
        performEndEdit(partner);
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
    const bool dataStep = std::any_of(step.changes.begin(), step.changes.end(),
                                      [](const UndoStep::Change &change) { return touchesData(change.index); });
    struct Refresh { // once the step is in the history, which then holds what it names
        Controller &c;
        bool on;
        ~Refresh() {
            if (on)
                c.refreshData();
        }
    } refresh{*this, dataStep};
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

void Controller::record(int index) {
    // The mod wheel and the pitch bend are played, not part of the patch.
    if (depth_ == 0 || index < 0 || index >= kParamCount || recorded_[index] || isPlayed(index))
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
    performStepBegin();
    preset_ = forward ? step.presetAfter : step.presetBefore;
    edited_ = forward ? step.editedAfter : step.editedBefore;
    for (const UndoStep::Change &change : step.changes)
        performBeginEdit(change.index);
    for (const UndoStep::Change &change : step.changes) {
        if (touchesData(change.index))
            beforeSet(change.index, forward ? change.after : change.before);
        performSetValue(change.index, forward ? change.after : change.before);
    }
    for (auto it = step.changes.rbegin(); it != step.changes.rend(); ++it)
        performEndEdit(it->index);
    if (step.layoutChanged) {
        restorePatchLayout(layout(), forward ? step.after : step.before);
        layoutChanged();
    }
    performStepEnd();
    applying_ = false;
    refreshData();
}

void Controller::clearHistory() {
    undo_.clear();
    redo_.clear();
    refreshData();
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
