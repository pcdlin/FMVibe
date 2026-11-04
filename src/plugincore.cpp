#include "plugincore.h"

#include "params.h"
#include "uistrings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <xmmintrin.h>

namespace fmvibe {

namespace {

// How long after the plugin tells the host a param's value it takes none for that param from the host.
constexpr int64_t kHostEchoMs = 500;

// Velocity of notes played from the virtual keyboard.
constexpr double kKeyVelocity = 100.0 / 127.0;

} // namespace

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

PluginCore::PluginCore() {
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(table[i].def);
}

PluginCore::~PluginCore() = default;

void PluginCore::layoutChanged() {
    sectionsChanged(); // the sources' order
    editorFitChanged();
    hostMarkDirty();
}

void PluginCore::performSetValue(int index, double value) {
    values_[index].store(value);
    hostEdit(index, value);
    if (shapesSections(index))
        sectionsChanged();
    if (index == paramIndex(GlobalParam::Voices)) {
        voiceLimitChanged();
        hostVoicesChanged();
    }
}

// Voices are allocated here, on the main thread, for the current voice limit.
void PluginCore::activate(double sampleRate) {
    synth_.prepare(sampleRate, voiceLimit(), oversampling_.load());
    restartRequested_.store(false);
}

int PluginCore::voiceLimit() const {
    return std::clamp(static_cast<int>(std::lround(values_[paramIndex(GlobalParam::Voices)].load())), 1, kMaxVoices);
}

// Audio thread, or main thread while not processing.
void PluginCore::applyPlayMode() {
    auto on = [this](GlobalParam p) { return values_[paramIndex(p)].load(std::memory_order_relaxed) >= 0.5; };
    synth_.setPlayMode(on(GlobalParam::Mono), on(GlobalParam::Legato), on(GlobalParam::GlideAlways));
    synth_.setMpe(on(GlobalParam::Mpe),
                  values_[paramIndex(GlobalParam::MpeBendRange)].load(std::memory_order_relaxed));
}

// Thread-safe: each format's hostRequestRestart() may be called from any thread.
void PluginCore::voiceLimitChanged() {
    if (synth_.capacity() > 0 && voiceLimit() > synth_.capacity() && !restartRequested_.exchange(true))
        hostRequestRestart();
}

// Main thread.
void PluginCore::restartIfNeeded() {
    if (synth_.capacity() > 0 && oversampling_.load() != synth_.oversampling() && !restartRequested_.exchange(true))
        hostRequestRestart();
}

// Main thread, from the settings dialog.
void PluginCore::setOversampling(int factor) {
    factor = validOversampling(factor);
    if (oversampling_.exchange(factor) == factor)
        return;
    restartIfNeeded();
    hostMarkDirty();
}

void PluginCore::beginBlock() {
    // Filter and delay tails decay into denormals, which are slow; flush them to zero.
    csr_ = _mm_getcsr();
    _mm_setcsr(csr_ | 0x8040); // flush-to-zero and denormals-are-zero

    playKeys();
    synth_.setWheelBend(wheelBend_.load());
    synth_.setAnalyzing(editorOpen_.load(std::memory_order_relaxed));
    synth_.setScope(editorOpen_.load(std::memory_order_relaxed) ? scopeOp_.load(std::memory_order_relaxed) : -1,
                    scopeCycles_.load(std::memory_order_relaxed));
    // Editor changes to these land between blocks; apply them before this block's notes.
    synth_.setVoiceLimit(voiceLimit());
    applyPlayMode();

    // The params are read once for the block; a param event in it updates the copy as well (see setHostValue()), so
    // what follows hears it, and an edit in the editor meanwhile is heard from the next block on.
    for (int i = 0; i < kParamCount; ++i)
        snapshot_[i] = values_[i].load(std::memory_order_relaxed);
}

void PluginCore::render(float *left, float *right, uint32_t frames) {
    if (frames == 0)
        return;
    synth_.setVoiceLimit(static_cast<int>(std::lround(snapshot_[paramIndex(GlobalParam::Voices)])));
    applyPlayMode();
    synth_.render(left, right, frames, snapshot_);
}

void PluginCore::endBlock() {
    synth_.clearEndedNotes();
    if (editorOpen_.load(std::memory_order_relaxed)) {
        Telemetry t;
        synth_.telemetry(t);
        telemetry_.store(t);
    }
    _mm_setcsr(csr_);
}

// Main thread: the virtual keyboard. The host may be asleep, so ask it to run us.
void PluginCore::playNote(int note, bool on) {
    keys_.push({note, on});
    hostRequestProcess();
}

// Audio thread: notes from the virtual keyboard play like MIDI notes without a note id.
void PluginCore::playKeys() {
    KeyEvent ev;
    while (keys_.pop(ev)) {
        const NoteRef note{-1, 0, 0, static_cast<int16_t>(ev.note)};
        if (ev.on)
            synth_.noteOn(note, kKeyVelocity);
        else
            synth_.noteOff(note);
    }
}

void PluginCore::setHostValue(int index, double value, bool inBlock) {
    if (index < 0 || index >= kParamCount || !std::isfinite(value))
        return;
    // For a moment after the plugin tells the host a value, what the host sends may still be an older one it is
    // handing back (Bitwig does): taken, it would flip a preset just loaded back to the one before.
    if (steadyMs() - toldAt_[index].load(std::memory_order_relaxed) < kHostEchoMs)
        return;
    value = std::clamp(value, paramTable()[index].min, paramTable()[index].max);
    values_[index].store(value);
    if (inBlock)
        snapshot_[index] = value; // the rest of the block being processed hears it (see beginBlock())
    // Voice settings apply before any note-ons that follow in the same block.
    if (index == paramIndex(GlobalParam::Voices)) {
        if (inBlock)
            synth_.setVoiceLimit(voiceLimit());
        voiceLimitChanged();
    } else if (index == paramIndex(GlobalParam::Mono) || index == paramIndex(GlobalParam::Legato) ||
               index == paramIndex(GlobalParam::GlideAlways)) {
        if (inBlock)
            applyPlayMode();
    } else if (shapesSections(index)) {
        hostRequestCallback(); // this may be the audio thread; tell the host on the main thread
    }
}

ParamValues PluginCore::valuesNow() const {
    ParamValues values;
    for (int i = 0; i < kParamCount; ++i)
        values[i] = values_[i].load(std::memory_order_relaxed);
    return values;
}

void PluginCore::sectionsChanged() {
    std::array<int64_t, kFxSlots + 1 + 2 * kMaxSources + kFxSlots> now{};
    for (int slot = 0; slot < kFxSlots; ++slot) {
        now[slot] = std::lround(values_[paramIndex(slot, FxParam::Type)].load());
        if (fxType(static_cast<double>(now[slot])) == FxType::Reverb) // a shimmer's page differs
            now[kFxSlots + 1 + 2 * kMaxSources + slot] =
                static_cast<int64_t>(reverbType(values_[paramIndex(slot, FxParam::ReverbType)].load()));
    }
    now[kFxSlots] = std::llround(values_[paramIndex(GlobalParam::FxOrder)].load()); // up to 16!, past a long
    for (int source = 0; source < kMaxSources; ++source) {
        now[kFxSlots + 1 + source] = std::lround(values_[paramIndex(source, SourceParam::Type)].load());
        now[kFxSlots + 1 + kMaxSources + source] = layout_.sourceOrder[source];
    }
    if (now == shownSections_)
        return;
    auto types = [](const auto &a) { // which effect or source each slot and place holds
        std::array<int64_t, kFxSlots + kMaxSources> t{};
        std::copy_n(a.begin(), kFxSlots, t.begin());
        std::copy_n(a.begin() + kFxSlots + 1, kMaxSources, t.begin() + kFxSlots);
        return t;
    };
    const bool typesChanged = types(now) != types(shownSections_);
    shownSections_ = now;
    hostSectionsChanged(typesChanged);
}

void PluginCore::editorFitChanged() {
    if (!editor_)
        return;
    uint32_t width = 0, height = 0, fitted = 0;
    editor_->size(width, height);
    editor_->fit(width, fitted);
    if (fitted != height)
        hostResizeEditor(width, fitted); // a host that won't resize leaves the editor drawn smaller, centered
}

bool PluginCore::openEditor() {
    if (editor_)
        return false;
    readUserSettings();
    editor_ = std::make_unique<gui::Editor>(*this);
    editorOpen_.store(true);
    return true;
}

void PluginCore::closeEditor() {
    editorOpen_.store(false);
    editor_.reset();
}

bool PluginCore::saveState(const clap_ostream_t *stream, bool settings) const {
    if (!writePatch(stream, valuesNow(), layout_))
        return false;
    if (!settings)
        return true;
    if (!writeSettings(stream, oversampling_.load()))
        return false;
    // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
    return writePresetChunk(stream, patchEdited(), utf8(std::filesystem::path(presetFile())));
}

bool PluginCore::loadState(const clap_istream_t *stream, bool settings) {
    ParamValues loaded;
    if (!readParams(stream, loaded))
        return false;
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    // Hosts re-read the values only when asked; any hidden flags follow in sectionsChanged().
    hostValuesChanged();

    // The editor layout is optional; whatever is missing keeps its default.
    const gui::UiLayout layout = readLayout(stream);
    // A preset leaves the editor as it is (its size, the modulation panel, the shape grid) from the start,
    // so its window isn't resized to the preset's and back.
    const gui::UiLayout before = layout_;
    layout_ = layout;
    if (!settings)
        keepEditorLayout(before);

    // The settings are optional too: a project saved before them gets the defaults.
    if (settings) {
        oversampling_.store(readSettings(stream));
        restartIfNeeded();

        // Which preset it was. Without that, it shows Init, changed unless it is Init.
        std::string preset;
        bool edited = false;
        if (readPresetChunk(stream, preset, edited)) {
            setPreset(widePath(preset), edited);
        } else {
            const ParamTable &table = paramTable();
            bool atInit = true;
            for (int i = 0; i < kParamCount && atInit; ++i)
                atInit = loaded[i] == table[i].def;
            setPreset(L"", !atInit);
        }
    }
    editorFitChanged();
    hostRequestCallback(); // sectionsChanged(), once the host is done loading
    return true;
}

// Main thread. Every param may have changed at once: tell the host about each one that did, since `before`. All
// of them, three events each, would be more than some hosts take in one go.
void PluginCore::notifyParams(const ParamValues &before) {
    for (int i = 0; i < kParamCount; ++i) {
        const double value = values_[i].load();
        if (value == before[i])
            continue;
        hostBeginEdit(i);
        hostEdit(i, value);
        hostEndEdit(i);
    }
    sectionsChanged();
    voiceLimitChanged();
    hostVoicesChanged();
    hostMarkDirty();
}

void PluginCore::keepEditorLayout(const gui::UiLayout &from) {
    layout_.zoom = from.zoom;
    layout_.sourcesCollapsed = from.sourcesCollapsed;
    layout_.keysCollapsed = from.keysCollapsed;
    layout_.shapeGridX = from.shapeGridX;
    layout_.shapeGridY = from.shapeGridY;
    layout_.opTab = from.opTab;
    layout_.scopeCycles = from.scopeCycles;
}

// Init and loading a preset are undo steps like any edit in the editor; afterwards the patch is
// the preset, unchanged.
void PluginCore::resetPreset() {
    {
        gui::UndoScope step(*this);
        recordAll();
        const auto told = std::make_unique<ParamValues>(valuesNow());
        const ParamTable &table = paramTable();
        for (int i = 0; i < kParamCount; ++i)
            values_[i].store(table[i].def);
        const gui::UiLayout before = layout_;
        layout_ = gui::UiLayout{};
        keepEditorLayout(before);
        notifyParams(*told);
        layoutChanged();
        setPreset(L"");
    }
}

// A preset file holds the same bytes as the host's state.
bool PluginCore::loadPreset(const std::wstring &path) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    const clap_istream_t stream = fileIn(file);
    bool loaded = false;
    {
        gui::UndoScope step(*this);
        recordAll();
        const auto told = std::make_unique<ParamValues>(valuesNow());
        loaded = loadState(&stream, false);
        if (loaded) {
            notifyParams(*told);
            layoutChanged();
            setPreset(path); // in the step, so undoing it brings back the preset before
        }
    }
    std::fclose(file);
    return loaded;
}

bool PluginCore::savePreset(const std::wstring &path) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
        return false;
    const clap_ostream_t stream = fileOut(file);
    const bool saved = saveState(&stream, false);
    const bool closed = std::fclose(file) == 0;
    if (!saved || !closed)
        return false;
    setPreset(path);
    rescanPresets(); // so the library lists it
    return true;
}

void PluginCore::choosePreset(bool save) {
    if (editor_)
        editor_->choosePreset(save);
}

std::wstring PluginCore::presetFolder() const {
    return PresetLibrary::folderPath();
}

int PluginCore::presetIndex() const {
    return presets_.indexOf(presetFile());
}

std::string PluginCore::presetName() const {
    return presetFile().empty() ? tr(Str::PresetInit) : utf8(std::filesystem::path(presetFile()).stem());
}

bool PluginCore::loadPresetAt(int index) {
    return index >= 0 && index < static_cast<int>(presets_.files().size()) && loadPreset(presets_.files()[index]);
}

void PluginCore::openPresetFolder() {
    presets_.openFolder();
}

} // namespace fmvibe
