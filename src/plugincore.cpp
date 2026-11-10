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
    // A new instance opens at the size the user last gave an editor; a state loaded after it brings its own.
    readUserSettings();
    if (userSettings.zoom >= 0.5)
        layout_.zoom = static_cast<float>(userSettings.zoom);
}

PluginCore::~PluginCore() = default;

void PluginCore::layoutChanged() {
    sectionsChanged(); // the sources' order
    editorFitChanged();
    hostMarkDirty();
}

void PluginCore::performSetValue(int index, double value) {
    storeValue(index, value);
    if (hostListed(index))
        hostEdit(index, value);
    else if (applyingStep_)
        dirtyAfterStep_ = true; // once, at the step's end
    else
        hostMarkDirty(); // the host hears of no edit to mark the project by
    if (shapesSections(index))
        sectionsChanged();
    if (index == paramIndex(GlobalParam::Voices)) {
        voiceLimitChanged();
        hostVoicesChanged();
    }
}

// Voices are allocated here, on the main thread, for the current voice limit.
void PluginCore::activate(double sampleRate) {
    sampleRate_ = sampleRate;
    synth_.prepare(sampleRate, voiceLimit(), oversampling_.load());
    restartAskedFor_.store(0);
}

int PluginCore::voiceLimit() const {
    return std::clamp(static_cast<int>(std::lround(values_[paramIndex(GlobalParam::Voices)].load())), 1, kMaxVoices);
}

// Audio thread, or main thread while not processing.
void PluginCore::applyPlayMode() {
    auto on = [this](GlobalParam p) { return values_[paramIndex(p)].load(std::memory_order_relaxed) >= 0.5; };
    synth_.setPlayMode(on(GlobalParam::Mono), on(GlobalParam::Legato), on(GlobalParam::GlideAlways));
    synth_.setMpe(mpe_.load(std::memory_order_relaxed), mpeBendRange_.load(std::memory_order_relaxed));
}

void PluginCore::performStepBegin() {
    applyingStep_ = true;
    dirtyAfterStep_ = false;
}

void PluginCore::performStepEnd() {
    applyingStep_ = false;
    if (dirtyAfterStep_)
        hostMarkDirty();
    dirtyAfterStep_ = false;
}

// Thread-safe: each format's hostRequestRestart() may be called from any thread.
void PluginCore::askRestart() {
    const int capacity = synth_.capacity(), voices = voiceLimit(), factor = oversampling_.load();
    if (capacity <= 0 || (voices <= capacity && factor == synth_.oversampling())) {
        restartAskedFor_.store(0); // so the same need, should it come back, asks again
        return;
    }
    const int need = std::max(voices, capacity) * 16 + factor;
    if (restartAskedFor_.exchange(need) != need)
        hostRequestRestart();
}

void PluginCore::voiceLimitChanged() {
    askRestart();
}

// Main thread.
void PluginCore::restartIfNeeded() {
    askRestart();
}

// Main thread, from the settings dialog.
void PluginCore::setOversampling(int factor) {
    factor = validOversampling(factor);
    if (oversampling_.exchange(factor) == factor)
        return;
    restartIfNeeded();
    hostMarkDirty();
}

// Main thread, from the settings dialog: the audio thread takes them up at its next block.
void PluginCore::setMpe(bool on) {
    if (mpe_.exchange(on) != on)
        hostMarkDirty();
}

void PluginCore::setMpeBendRange(int semitones) {
    semitones = std::clamp(semitones, 1, kMaxMpeBendRange);
    if (mpeBendRange_.exchange(semitones) != semitones)
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
    // what follows hears it, and an edit in the editor meanwhile is heard from the next block on. The copy is taken
    // again only when something was stored since the last one.
    if (const uint64_t version = valuesVersion_.load(std::memory_order_acquire); version != snapshotVersion_) {
        snapshotVersion_ = version;
        for (int i = 0; i < kParamCount; ++i)
            snapshot_[i] = values_[i].load(std::memory_order_relaxed);
        snapshotChanged_ = true;
    }
}

void PluginCore::render(float *left, float *right, uint32_t frames) {
    if (frames == 0)
        return;
    synth_.setVoiceLimit(static_cast<int>(std::lround(snapshot_[paramIndex(GlobalParam::Voices)])));
    applyPlayMode();
    synth_.render(left, right, frames, snapshot_, !snapshotChanged_);
    snapshotChanged_ = false;
    telemetryFrames_ += frames;
}

void PluginCore::endBlock() {
    synth_.clearEndedNotes();
    // The editor paints about 60 times a second: a snapshot every kTelemetrySeconds of audio is plenty, however short
    // the host's blocks, and the first one as soon as the editor opens.
    const bool open = editorOpen_.load(std::memory_order_relaxed);
    if (open && (!telemetryOpen_ || telemetryFrames_ >= kTelemetrySeconds * sampleRate_)) {
        telemetryFrames_ = 0.0;
        Telemetry &t = *telemetryScratch_;
        synth_.telemetry(t);
        telemetry_.store(t);
    }
    telemetryOpen_ = open;
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
    if (inBlock) {
        // The rest of the block being processed hears it (see beginBlock()); a value it has already, as a host's
        // automation often sends, leaves what the synth works out from the params as it is.
        if (snapshot_[index] != value) {
            snapshot_[index] = value;
            snapshotChanged_ = true;
        }
    } else {
        valuesChanged(); // the next block's snapshot takes it
    }
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
    valuesInto(values);
    return values;
}

void PluginCore::valuesInto(ParamValues &values) const {
    for (int i = 0; i < kParamCount; ++i)
        values[i] = values_[i].load(std::memory_order_relaxed);
}

void PluginCore::sectionsChanged() {
    std::array<int64_t, kOps + 2 * kMaxSources> now{};
    for (int op = 0; op < kOps; ++op)
        now[op] = values_[paramIndex(op, OpParam::Fixed)].load() >= 0.5;
    for (int source = 0; source < kMaxSources; ++source) {
        now[kOps + source] = std::lround(values_[paramIndex(source, SourceParam::Type)].load());
        now[kOps + kMaxSources + source] = layout_.sourceOrder[source];
    }
    if (now == shownSections_)
        return;
    shownSections_ = now;
    hostSectionsChanged();
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
    const auto values = std::make_unique<ParamValues>();
    valuesInto(*values);
    if (!writePatch(stream, *values, layout_))
        return false;
    if (!settings)
        return true;
    if (!writeSettings(stream, {oversampling_.load(), mpe_.load(), mpeBendRange_.load()}))
        return false;
    // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
    return writePresetChunk(stream, patchEdited(), utf8(std::filesystem::path(presetFile())));
}

bool PluginCore::loadState(const clap_istream_t *stream, bool settings) {
    const auto read = std::make_unique<ParamValues>();
    ParamValues &loaded = *read;
    if (!readParams(stream, loaded))
        return false;
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    valuesChanged();
    // Hosts re-read the values only when asked; the remote pages follow in sectionsChanged().
    hostValuesChanged();
    // The state's voice limit: more voices than are allocated take a restart (a lower limit the audio thread takes
    // at its next block), and hosts read the voice info again.
    voiceLimitChanged();
    hostVoicesChanged();

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
        const Settings stored = readSettings(stream);
        oversampling_.store(stored.oversampling);
        mpe_.store(stored.mpe);
        mpeBendRange_.store(stored.mpeBendRange);
        restartIfNeeded();

        // Which preset it was. Without that, it shows Init, changed unless it is Init.
        std::string preset;
        bool edited = false;
        if (readPresetChunk(stream, preset, edited)) {
            // Save writes the patch over the preset named here, so a project names only a preset file: any other
            // path (a project made by hand, say) shows as an edited patch with no preset.
            std::wstring file = widePath(preset);
            std::wstring extension = std::filesystem::path(file).extension().wstring();
            for (wchar_t &c : extension)
                c = c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            if (!file.empty() && extension != L".fmv") {
                file.clear();
                edited = true;
            }
            setPreset(file, edited);
        } else {
            const ParamTable &table = paramTable();
            bool atInit = true;
            for (int i = 0; i < kParamCount && atInit; ++i)
                atInit = loaded[i] == table[i].def;
            setPreset(L"", !atInit);
        }
    }
    editorFitChanged();
    if (editor_)
        editor_->repaint(); // a layout alone may have changed, which the editor's poll doesn't see
    hostRequestCallback(); // sectionsChanged(), once the host is done loading
    return true;
}

// Main thread. Every param may have changed at once: tell the host about each one that did, since `before`. All
// of them, three events each, would be more than some hosts take in one go.
void PluginCore::notifyParams(const ParamValues &before) {
    for (int i = 0; i < kParamCount; ++i) {
        const double value = values_[i].load();
        if (value == before[i] || !hostListed(i))
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
    layout_.shapeSteps = from.shapeSteps;
    layout_.shapeSnap = from.shapeSnap;
    layout_.opTab = from.opTab;
    layout_.scopeCycles = from.scopeCycles;
}

// Init and loading a preset start the undo history over: undo never brings back the patch before.
// Afterwards the patch is the preset, unchanged.
void PluginCore::resetPreset() {
    const auto told = std::make_unique<ParamValues>();
    valuesInto(*told);
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(table[i].def);
    valuesChanged();
    const gui::UiLayout before = layout_;
    layout_ = gui::UiLayout{};
    keepEditorLayout(before);
    notifyParams(*told);
    layoutChanged();
    setPreset(L"");
    clearHistory();
}

// A preset file holds the same bytes as the host's state.
bool PluginCore::loadPreset(const std::wstring &path) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    const clap_istream_t stream = fileIn(file);
    const auto told = std::make_unique<ParamValues>();
    valuesInto(*told);
    const bool loaded = loadState(&stream, false);
    if (loaded) {
        notifyParams(*told);
        layoutChanged();
        setPreset(path);
        clearHistory();
    }
    std::fclose(file);
    return loaded;
}

bool PluginCore::savePreset(const std::wstring &path) {
    // Written beside the file first and put in its place once whole, so a write that fails (a full disk, a file
    // another program holds) leaves the preset it would replace as it was. The scan lists only .fmv files.
    namespace fs = std::filesystem;
    const fs::path target(path), part(path + L".part");
    FILE *file = nullptr;
    if (_wfopen_s(&file, part.c_str(), L"wb") != 0 || !file)
        return false;
    const clap_ostream_t stream = fileOut(file);
    const bool saved = saveState(&stream, false);
    const bool closed = std::fclose(file) == 0;
    if (!saved || !closed) {
        std::error_code ignored;
        fs::remove(part, ignored);
        return false;
    }
    if (!replaceFile(part.wstring(), target.wstring()))
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
