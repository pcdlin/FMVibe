#include "plugincore.h"

#include "factory.h"
#include "fxlag.h"
#include "oversampler.h"
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
    // The Wavetable and Sample operators' data reaches the audio thread through the synth's registry.
    data().attach(&synth_.registry());
    refreshData(); // operator A's table (Basic Shapes), should it be switched to Wavetable
}

PluginCore::~PluginCore() {
    // The registry's pointers go with the data: nothing renders any more.
    data().attach(nullptr);
}

void PluginCore::onMainThread() {
    sectionsChanged();
    dataAsked_.store(false);
    for (const uint32_t key : synth_.registry().takeMissing())
        if (!data().ensure(key))
            synth_.registry().ignore(key); // a key the patch doesn't hold: its operator stays silent
    refreshData();
}

void PluginCore::chooseOperatorFile(int op, DataKind kind) {
    if (editor_)
        editor_->chooseOperatorFile(op, kind);
}

TableLibrary *PluginCore::tableLibrary() {
    if (!library_) {
        const std::string folder = userSettingsFolder();
        library_ = std::make_unique<TableLibrary>(folder.empty() ? std::string() : folder + "/tables-cache.bin");
        readUserSettings();
        library_->setFolders(userSettings.tableFolders);
    }
    return library_.get();
}

void PluginCore::chooseTableFolder() {
    if (editor_)
        editor_->chooseTableFolder();
}

void PluginCore::tableFoldersChanged() {
    if (!library_)
        return;
    readUserSettings();
    userSettings.tableFolders = library_->folders();
    writeUserSettings();
}

void PluginCore::showTableFolder(int source) {
    if (library_ && source >= 0 && source < static_cast<int>(library_->sources().size()))
        gui::showFolder(library_->sources()[source].folder.path);
}

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
    } else if (fxLagParam(index)) { // an effect or wire that makes the output later or sooner
        askRestart();
    }
}

// Voices are allocated here, on the main thread, for the current voice limit.
void PluginCore::activate(double sampleRate) {
    sampleRate_ = sampleRate;
    synth_.prepare(sampleRate, voiceLimit(), oversampling_.load());
    for (int kind = 0; kind < FxLag::kKinds; ++kind)
        pathDelays_[kind] = upDownDelay(sampleRate, FxLag::factorOf(kind));
    voicesDelay_ = decimatorDelay(sampleRate, synth_.oversampling());
    latency_.store(latencyNow());
    restartAskedFor_.store(0);
}

// The voices' decimator, then the effects' latest path (see fxlag.h), at the rate and oversampling activate() took.
int PluginCore::latencyNow() const {
    const FxLag lag = fxOutputLag([this](int i) { return values_[i].load(std::memory_order_relaxed); });
    return static_cast<int>(std::lround(voicesDelay_ + fxLagSamples(lag, pathDelays_)));
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
    const int latency = capacity > 0 ? latencyNow() : 0;
    if (capacity <= 0 || (voices <= capacity && factor == synth_.oversampling() && latency == latency_.load())) {
        restartAskedFor_.store(0); // so the same need, should it come back, asks again
        return;
    }
    const int need = (std::max(voices, capacity) * 16 + factor) * 4096 + std::clamp(latency, 0, 4095);
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
    inBlock_.store(true);
    // Filter and delay tails decay into denormals, which are slow; flush them to zero.
    csr_ = _mm_getcsr();
    _mm_setcsr(csr_ | 0x8040); // flush-to-zero and denormals-are-zero

    playKeys();
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
    playedTempo_.store(synth_.tempo(), std::memory_order_relaxed);
    _mm_setcsr(csr_);
    // A Wavetable or Sample operator's data wasn't there (a factory table not built yet, a host's value): the main
    // thread builds and registers it, if it can.
    if (synth_.registry().anyMissing() && !dataAsked_.exchange(true))
        hostRequestCallback();
    inBlock_.store(false);
}

// Main thread: the virtual keyboard. The host may be asleep, so ask it to run us.
void PluginCore::playNote(int note, bool on) {
    keys_.push({note, on});
    hostRequestProcess();
}

void PluginCore::auditionNote(int note, bool on) {
    keys_.push({note, on, true});
    hostRequestProcess();
}

// Audio thread: notes from the virtual keyboard play like MIDI notes without a note id; those the SEQ view plays to be
// heard on a port of their own, which the sequencer leaves be.
void PluginCore::playKeys() {
    KeyEvent ev;
    while (keys_.pop(ev)) {
        const NoteRef note{-1, ev.audition ? kAuditionPort : int16_t{0}, 0, static_cast<int16_t>(ev.note)};
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
    stamp(); // the editor sees it
    if (inBlock) {
        // The rest of the block being processed hears it (see beginBlock()); a value it has already, as a host's
        // automation often sends, leaves what the synth works out from the params as it is, and another has it work
        // out again what follows from that param (see Synth::paramChanged()).
        if (snapshot_[index] != value) {
            snapshot_[index] = value;
            synth_.paramChanged(index);
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

bool PluginCore::playModWheel(double value) {
    const int index = paramIndex(GlobalParam::ModWheel);
    if (!std::isfinite(value))
        return false;
    value = std::clamp(value, 0.0, 1.0);
    if (snapshot_[index] == value && values_[index].load(std::memory_order_relaxed) == value)
        return false;
    values_[index].store(value);
    stamp();
    snapshot_[index] = value;
    synth_.paramChanged(index);
    // The host hands the value back a little later, and by then the wheel may have moved on: not taken, as for an
    // edit in the editor (see setHostValue()).
    told(index, steadyMs());
    return true;
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
    idleLfosShown_.store(false);
    editor_.reset();
}

// Main thread. A host that let the plugin sleep (CLAP's, once nothing sounds) is asked to run it again, so the clock
// moves on and the editor sees it.
void PluginCore::setIdleLfosShown(bool shown) {
    if (!idleLfosShown_.exchange(shown) && shown)
        hostRequestProcess();
}

namespace {

// The data keys the params name, an operator's table and sample each.
std::vector<uint32_t> namedKeys(const ParamValues &values) {
    std::vector<uint32_t> keys;
    for (int op = 0; op < kOps; ++op)
        for (const OpParam p : {OpParam::Table, OpParam::Sample})
            keys.push_back(dataKey(values[paramIndex(op, p)]));
    return keys;
}

} // namespace

bool PluginCore::saveState(const clap_ostream_t *stream, bool settings) const {
    const auto values = std::make_unique<ParamValues>();
    valuesInto(*values);
    if (!writePatch(stream, *values, layout_))
        return false;
    if (settings) {
        if (!writeSettings(stream, {oversampling_.load(), mpe_.load(), mpeBendRange_.load()}))
            return false;
        // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
        if (!writePresetChunk(stream, patchEdited(), utf8(std::filesystem::path(presetFile()))))
            return false;
    }
    // Last, the tables and samples the params name that the patch holds itself (not the factory tables); a patch with
    // none has no DATA chunk, its bytes as they were before there was one.
    const std::vector<const OpData *> named = data().named(namedKeys(*values));
    return named.empty() || writeDataChunk(stream, named);
}

bool PluginCore::loadState(const clap_istream_t *stream, bool settings) {
    const auto read = std::make_unique<ParamValues>();
    ParamValues &loaded = *read;
    if (!readParams(stream, loaded))
        return false;
    // The editor layout is optional; whatever is missing keeps its default.
    const gui::UiLayout layout = readLayout(stream);
    // The settings and the preset are optional too: a project saved before them gets the defaults.
    Settings stored;
    std::string preset;
    bool edited = false, hasPreset = false;
    if (settings) {
        stored = readSettings(stream);
        hasPreset = readPresetChunk(stream, preset, edited);
    }
    // The tables and samples, built and registered before the params naming them are stored, so the audio thread's
    // first block with them finds them.
    std::vector<std::shared_ptr<OpData>> entries;
    readDataChunk(stream, entries);
    for (std::shared_ptr<OpData> &entry : entries) {
        buildOpData(*entry);
        data().put(std::move(entry));
    }
    for (int op = 0; op < kOps; ++op) {
        const OpWave wave = opWave(loaded[paramIndex(op, OpParam::Wave)]);
        if (wave == OpWave::Wavetable)
            data().ensure(dataKey(loaded[paramIndex(op, OpParam::Table)]));
        else if (wave == OpWave::Sample && dataKey(loaded[paramIndex(op, OpParam::Sample)]) != 0)
            data().ensure(dataKey(loaded[paramIndex(op, OpParam::Sample)]));
    }

    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    valuesChanged();
    // Hosts re-read the values only when asked (a preset's load asks in notifyParams()); the remote pages follow in
    // sectionsChanged().
    if (settings)
        hostValuesChanged();
    // The state's voice limit: more voices than are allocated take a restart (a lower limit the audio thread takes
    // at its next block), and hosts read the voice info again.
    voiceLimitChanged();
    hostVoicesChanged();

    // A preset leaves the editor as it is (its size, the modulation panel, the shape grid) from the start,
    // so its window isn't resized to the preset's and back.
    const gui::UiLayout before = layout_;
    layout_ = layout;
    if (!settings)
        keepEditorLayout(before);

    if (settings) {
        oversampling_.store(stored.oversampling);
        mpe_.store(stored.mpe);
        mpeBendRange_.store(stored.mpeBendRange);
        restartIfNeeded();

        // Which preset it was. Without that, it shows Init, changed unless it is Init.
        if (hasPreset) {
            // Save writes the patch over the preset named here, so a project names only a preset file or a factory
            // preset: any other path (a project made by hand, say) shows as an edited patch with no preset. A copy
            // of a factory preset the plugin once wrote into the preset folder is that factory preset.
            std::wstring file = PresetLibrary::resolve(widePath(preset));
            const bool factory = PresetLibrary::factoryPreset(file) != nullptr;
            std::wstring extension = factory ? L".fmv" : std::filesystem::path(file).extension().wstring();
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

// Main thread. Every param may have changed at once (a preset, Init, a preview put back): the host reads every value
// again (CLAP's values rescan, VST3's kParamValuesChanged) rather than hearing an edit of each, so it records none of
// them as automation or in its undo, and the project is marked changed. Values the host sends for the params that
// changed, for a moment after, are older ones still on their way and aren't taken (see setHostValue()).
void PluginCore::notifyParams(const ParamValues &before) {
    const int64_t now = steadyMs();
    for (int i = 0; i < kParamCount; ++i)
        if (values_[i].load() != before[i])
            told(i, now);
    hostValuesChanged();
    sectionsChanged();
    voiceLimitChanged();
    hostVoicesChanged();
    hostMarkDirty();
}

// The files in it are elsewhere now: the preset the patch is, and the favorite and recent presets, follow them.
bool PluginCore::renamePresetFolder(const std::string &from, const std::string &to) {
    const std::wstring a = PresetLibrary::subfolderPath(from), b = PresetLibrary::subfolderPath(to);
    if (!presets_.renameFolder(from, to))
        return false;
    const std::wstring prefix = a + L"\\";
    const std::wstring &file = presetFile();
    if (file.size() > prefix.size() && _wcsnicmp(file.c_str(), prefix.c_str(), prefix.size()) == 0)
        setPreset(b + file.substr(a.size()), patchEdited());
    const std::string from8 = utf8(std::filesystem::path(a)) + "\\", to8 = utf8(std::filesystem::path(b)) + "\\";
    bool moved = false;
    for (std::vector<std::string> *list : {&userSettings.favorites, &userSettings.recent})
        for (std::string &kept : *list)
            if (kept.size() > from8.size() && _strnicmp(kept.c_str(), from8.c_str(), from8.size()) == 0) {
                kept = to8 + kept.substr(from8.size());
                moved = true;
            }
    if (moved)
        writeUserSettings();
    return true;
}

void PluginCore::keepEditorLayout(const gui::UiLayout &from) {
    gui::keepEditorLayout(layout_, from);
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

// A preset file holds the same bytes as the host's state; a factory preset's are built in.
bool PluginCore::loadPreset(const std::wstring &chosen) {
    const std::wstring path = PresetLibrary::resolve(chosen);
    if (const FactoryPreset *preset = PresetLibrary::factoryPreset(path)) {
        MemoryReader reader{preset->data.data(), preset->data.size()};
        const clap_istream_t stream = memoryIn(reader);
        return loadPresetFrom(&stream, path);
    }
    if (path.starts_with(PresetLibrary::kFactoryPrefix))
        return false; // a factory preset no longer built in
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    const clap_istream_t stream = fileIn(file);
    const bool loaded = loadPresetFrom(&stream, path);
    std::fclose(file);
    return loaded;
}

bool PluginCore::loadPresetFrom(const clap_istream_t *stream, const std::wstring &path) {
    const auto told = std::make_unique<ParamValues>();
    valuesInto(*told);
    const bool loaded = loadState(stream, false);
    if (loaded) {
        // A host that lets the editor set the tempo plays the preset at the tempo it was saved at.
        if (hasTempo() && layout_.savedTempo > 0.0f)
            setTempo(gui::clampTempo(layout_.savedTempo));
        notifyParams(*told);
        layoutChanged();
        setPreset(path);
        clearHistory();
        rememberRecent(path);
    }
    return loaded;
}

bool PluginCore::savePreset(const std::wstring &path) {
    // Written beside the file first and put in its place once whole, so a write that fails (a full disk, a file
    // another program holds) leaves the preset it would replace as it was. The scan lists only .fmv files.
    namespace fs = std::filesystem;
    if (path.starts_with(PresetLibrary::kFactoryPrefix)) // a factory preset is built in: Save As saves it anew
        return false;
    const fs::path target(path), part(path + L".part");
    FILE *file = nullptr;
    if (_wfopen_s(&file, part.c_str(), L"wb") != 0 || !file)
        return false;
    // The tempo it plays at goes with it: the editor's where the host lets it set one, else the host's last block's.
    // The patch keeps it only once the file is written.
    const float tempoBefore = layout_.savedTempo;
    const double bpm = hasTempo() ? tempo() : playedTempo_.load(std::memory_order_relaxed);
    if (bpm > 0.0)
        layout_.savedTempo = static_cast<float>(gui::clampTempo(bpm));
    const clap_ostream_t stream = fileOut(file);
    const bool saved = saveState(&stream, false);
    const bool closed = std::fclose(file) == 0;
    if (!saved || !closed) {
        std::error_code ignored;
        fs::remove(part, ignored);
        layout_.savedTempo = tempoBefore;
        return false;
    }
    if (!replaceFile(part.wstring(), target.wstring())) {
        std::error_code ignored;
        fs::remove(part, ignored);
        layout_.savedTempo = tempoBefore;
        return false;
    }
    setPreset(path);
    rescanPresets(); // so the library lists it
    return true;
}

void PluginCore::choosePreset(bool save) {
    PresetLibrary::makeFolder(); // where the dialogs start
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
    if (presetFile().empty())
        return tr(Str::PresetInit);
    if (const FactoryPreset *preset = PresetLibrary::factoryPreset(presetFile()))
        return preset->name;
    return utf8(std::filesystem::path(presetFile()).stem());
}

bool PluginCore::loadPresetAt(int index) {
    return index >= 0 && index < static_cast<int>(presets_.files().size()) && loadPreset(presets_.files()[index]);
}

bool PluginCore::savePresetIn(const std::string &folder, const std::string &name, const gui::PresetInfo &info) {
    const std::wstring path = PresetLibrary::presetFile(folder, name);
    if (path.empty())
        return false;
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    const gui::PresetInfo before = layout_.presetInfo;
    layout_.presetInfo = info;
    if (savePreset(path)) {
        layoutChanged();
        return true;
    }
    layout_.presetInfo = before;
    return false;
}

bool PluginCore::presetExists(const std::string &folder, const std::string &name) const {
    const std::wstring path = PresetLibrary::presetFile(folder, name);
    std::error_code ignored;
    return !path.empty() && std::filesystem::exists(path, ignored);
}

// The file read whole (its params, its layout and the tables and samples it holds), its info replaced, and written back
// as a preset saved is: beside it first, then in its place, so a write that fails leaves it as it was.
bool PluginCore::setPresetInfo(int index, const gui::PresetInfo &info) {
    namespace fs = std::filesystem;
    if (index < 0 || index >= static_cast<int>(presets_.files().size()) || presets_.entries()[index].factory)
        return false;
    const std::wstring path = presets_.files()[index];
    const auto values = std::make_unique<ParamValues>();
    gui::UiLayout layout;
    std::vector<std::shared_ptr<OpData>> held;
    {
        FILE *file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return false;
        const clap_istream_t stream = fileIn(file);
        const bool read = readParams(&stream, *values);
        if (read) {
            layout = readLayout(&stream);
            readDataChunk(&stream, held); // its own tables and samples, as stored, which go back with it
        }
        std::fclose(file);
        if (!read)
            return false;
    }
    layout.presetInfo = info;
    const fs::path part(path + L".part");
    FILE *file = nullptr;
    if (_wfopen_s(&file, part.c_str(), L"wb") != 0 || !file)
        return false;
    const clap_ostream_t stream = fileOut(file);
    std::vector<const OpData *> data;
    for (const std::shared_ptr<OpData> &entry : held)
        data.push_back(entry.get());
    const bool written = writePatch(&stream, *values, layout) && (data.empty() || writeDataChunk(&stream, data));
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::error_code ignored;
        fs::remove(part, ignored);
        return false;
    }
    if (!replaceFile(part.wstring(), path))
        return false;
    if (presets_.indexOf(presetFile()) == index) { // the patch is that preset: its info is the file's now
        layout_.presetInfo = info;
        layoutChanged();
    }
    rescanPresets();
    return true;
}

bool PluginCore::presetFavorite(int index) const {
    if (index < 0 || index >= static_cast<int>(presets_.files().size()))
        return false;
    const std::string file = utf8(std::filesystem::path(presets_.files()[index]));
    return std::find(userSettings.favorites.begin(), userSettings.favorites.end(), file) != userSettings.favorites.end();
}

void PluginCore::setPresetFavorite(int index, bool on) {
    if (index < 0 || index >= static_cast<int>(presets_.files().size()) || presetFavorite(index) == on)
        return;
    const std::string file = utf8(std::filesystem::path(presets_.files()[index]));
    auto &favorites = userSettings.favorites;
    if (on)
        favorites.push_back(file);
    else
        favorites.erase(std::remove(favorites.begin(), favorites.end(), file), favorites.end());
    writeUserSettings();
}

std::vector<int> PluginCore::recentPresets() const {
    std::vector<int> recent;
    for (const std::string &file : userSettings.recent) {
        const int index = presets_.indexOf(widePath(file));
        if (index >= 0 && std::find(recent.begin(), recent.end(), index) == recent.end())
            recent.push_back(index);
    }
    return recent;
}

// A preset loaded goes first among the recent ones.
void PluginCore::rememberRecent(const std::wstring &path) {
    const std::string file = utf8(std::filesystem::path(path));
    if (file.empty())
        return;
    auto &recent = userSettings.recent;
    recent.erase(std::remove(recent.begin(), recent.end(), file), recent.end());
    recent.insert(recent.begin(), file);
    if (recent.size() > kMaxRecentPresets)
        recent.resize(kMaxRecentPresets);
    writeUserSettings();
}

std::vector<gui::PresetCategory> PluginCore::userCategories() const {
    std::vector<gui::PresetCategory> categories;
    for (const auto &[name, color] : userSettings.categories)
        categories.push_back({name, color});
    return categories;
}

void PluginCore::addUserCategory(const gui::PresetCategory &category) {
    for (const auto &[name, color] : userSettings.categories)
        if (name == category.name)
            return;
    userSettings.categories.push_back({category.name, category.color});
    writeUserSettings();
}

void PluginCore::removeUserCategory(const std::string &name) {
    if (std::erase_if(userSettings.categories, [&](const auto &category) { return category.first == name; }) > 0)
        writeUserSettings();
}

std::string PluginCore::presetBrowserPlace() const {
    return userSettings.browserPlace;
}

void PluginCore::setPresetBrowserPlace(const std::string &place) {
    if (userSettings.browserPlace == place)
        return;
    userSettings.browserPlace = place;
    writeUserSettings();
}

} // namespace fmvibe
