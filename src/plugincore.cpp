#include "plugincore.h"

#include "factory.h"
#include "fxlag.h"
#include "gui/dx7.h"
#include "oversampler.h"
#include "params.h"
#include "uistrings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <random>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define FMVIBE_SSE 1
#endif

namespace fmvibe {

namespace {

// How long after the plugin tells a host a param's value it takes none for that param from the host.
constexpr int64_t kHostEchoMs = 500;
// How soon after the deactivate the activate of a restart the plugin asked for comes, for the notes to play on
// through it. A host that leaves the plugin inactive longer (switched off for a while, with a restart asked for still
// to come) finds the notes ended, as after any other activate: a key let go of meanwhile reached no voice.
constexpr int64_t kKeepVoicesMs = 1000;

#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

} // namespace

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

PluginCore::PluginCore() {
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(table[i].def);
    // A new instance opens at the size the user last gave an editor, its keyboard panel collapsed or open as the user
    // left one last; a state loaded after it brings its own.
    const UserSettings saved = savedUserSettings();
    if (saved.zoom >= 0.5)
        layout_.zoom = static_cast<float>(saved.zoom);
    layout_.keysCollapsed = saved.keysCollapsed;
    // Randomness of its own, so that instances layered don't play the same noise. The tests and fmvibe-bench, which
    // compare what plays sample for sample, ask for the same every time. On Windows the process's own environment,
    // which the tests change as they run: the statically linked CRT's getenv() reads a copy taken as the DLL loaded.
#ifdef _WIN32
    if (GetEnvironmentVariableW(L"FMVIBE_FIXED_SEED", nullptr, 0) == 0)
        synth_.seedNotes(std::random_device{}());
#else
    // An empty variable counts as none.
    const char *fixedSeed = std::getenv("FMVIBE_FIXED_SEED");
    if (fixedSeed == nullptr || *fixedSeed == '\0')
        synth_.seedNotes(std::random_device{}());
#endif
}

PluginCore::~PluginCore() = default;

void PluginCore::layoutChanged() {
    sectionsChanged(); // the sources' order
    editorFitChanged();
    hostMarkDirty();
}

// A gesture on Voices holds a restart for more voices off until it ends (see onTimer()).
void PluginCore::performBeginEdit(int index) {
    if (index == paramIndex(GlobalParam::Voices))
        ++voicesGestures_;
    if (hostListed(index))
        hostBeginEdit(index);
}

void PluginCore::performEndEdit(int index) {
    if (hostListed(index))
        hostEndEdit(index);
    // The wait starts again as the gesture ends, so that steps clicked or scrolled one after another ask once.
    if (index == paramIndex(GlobalParam::Voices) && voicesGestures_ > 0 && --voicesGestures_ == 0)
        if (int64_t at = voicesSettleAt_.load(); at != 0)
            voicesSettleAt_.compare_exchange_strong(at, steadyMs() + kVoicesSettleMs);
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
    } else if (fxLagParam(index) && !latencyEdited_) { // an effect or wire that may make the output later or sooner
        // Looked at from the main-thread callback, once the edit is whole: an effect moved along the line leaves it for
        // a moment as its wires change one by one, and a restart asked for then would cut the notes off for nothing.
        latencyEdited_ = true;
        hostRequestCallback();
    }
}

// Voices are allocated here, on the main thread, for the current voice limit: unless a restart the plugin asked for
// finds them fit to play on (see activate() in plugincore.h).
void PluginCore::beginDenormals() {
#ifdef FMVIBE_SSE
    csr_ = _mm_getcsr();
    _mm_setcsr(csr_ | 0x8040); // flush-to-zero and denormals-are-zero
#elif defined(__aarch64__)
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr_));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(csr_ | (1u << 24) | (1u << 19))); // FZ, FZ16
#else
    csr_ = 0;
#endif
}

void PluginCore::endDenormals() {
#ifdef FMVIBE_SSE
    _mm_setcsr(csr_);
#elif defined(__aarch64__)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(csr_));
#endif
}

bool PluginCore::activate(double sampleRate) {
    const bool keep = restartPending() && !outOfMemory() && !keysHeld_.load() && deactivatedAt_ >= 0 &&
                      steadyMs() - deactivatedAt_ < kKeepVoicesMs &&
                      synth_.fits(sampleRate, voiceLimit(), oversampling_.load());
    sampleRate_ = sampleRate;
    if (!keep) {
        try {
            synth_.prepare(sampleRate, voiceLimit(), oversampling_.load());
        } catch (const std::bad_alloc &) { // an exception must not reach the host, which would end with it
            outOfMemory_.store(true);
            return false;
        }
        keysHeld_.store(false); // their notes ended
    }
    latencyWaiting_.store(false); // the latency is taken now
    latencyDue_.store(false);
    outOfMemory_.store(false);
    pathDelays_ = fxLagDelays(sampleRate);
    voicesDelay_ = decimatorDelay(sampleRate, synth_.oversampling());
    latency_.store(latencyNow());
    restartAskedFor_.store(0);
    return true;
}

void PluginCore::onMainThread() {
    sectionsChanged();
    // A voice limit settling (see voiceLimitChanged()): the timer asks for the restart once it has.
    if (voicesSettleAt_.load() != 0 && !timerOn_) {
        timerOn_ = hostTimer(true);
        if (!timerOn_) { // a host without timers: asked for now
            voicesSettleAt_.store(0);
            askRestart();
        }
    }
    if (latencyEdited_ || latencyDue_.exchange(false)) { // an edit, or the keys a latency waited for are up
        latencyEdited_ = false;
        askRestart();
    }
}

// Asks for the restart a voice limit raised past the voices allocated takes, once it has stayed where it is for
// kVoicesSettleMs and no gesture on it is open, and then stops.
void PluginCore::onTimer() {
    int64_t at = voicesSettleAt_.load();
    if (at == 0) {
        if (timerOn_) {
            timerOn_ = false;
            hostTimer(false);
        }
        return;
    }
    if (voicesGestures_ > 0 || steadyMs() < at)
        return;
    if (voicesSettleAt_.compare_exchange_strong(at, 0)) // else it moved again meanwhile, and waits again
        askRestart();
}

int PluginCore::latencyAt(double sampleRate) const {
    const std::array<double, FxLag::kKinds> paths = fxLagDelays(sampleRate);
    const FxLag lag = fxOutputLag([this](int i) { return values_[i].load(std::memory_order_relaxed); });
    return static_cast<int>(std::lround(decimatorDelay(sampleRate, oversampling_.load()) + fxLagSamples(lag, paths)));
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
    synth_.setOwnTempo(tempoSync() ? 0.0 : ownTempo_.load(std::memory_order_relaxed));
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
    // A voice limit still settling holds every restart off: onTimer() asks for one once it has, for whatever needs
    // it by then, so that an edit meanwhile (the latency, the oversampling) doesn't make two in a row.
    if (voicesSettleAt_.load() != 0)
        return;
    const int capacity = synth_.capacity(), factor = oversampling_.load(), voices = voiceLimit();
    const int latency = capacity > 0 ? latencyNow() : 0;
    const bool voicesFit = voices <= capacity && factor == synth_.oversampling();
    if (capacity <= 0 || (voicesFit && latency == latency_.load())) {
        latencyWaiting_.store(false);
        restartAskedFor_.store(0); // so the same need, should it come back, asks again
        return;
    }
    // The latency alone, while a key or a pedal holds a note: once they are all up (see endBlock()).
    if (voicesFit && keysHeld_.load()) {
        latencyWaiting_.store(true);
        restartAskedFor_.store(0);
        return;
    }
    latencyWaiting_.store(false);
    const int need = (std::max(voices, capacity) * 16 + factor) * 4096 + std::clamp(latency, 0, 4095);
    if (restartAskedFor_.exchange(need) != need)
        hostRequestRestart();
}

void PluginCore::voiceLimitChanged(bool settled) {
    const int capacity = synth_.capacity();
    if (!settled && capacity > 0 && voiceLimit() > capacity) {
        voicesSettleAt_.store(steadyMs() + kVoicesSettleMs);
        hostRequestCallback(); // this may be the audio thread: onMainThread() starts the timer
        return;
    }
    voicesSettleAt_.store(0); // lowered within the voices allocated, which takes effect at once, or settled
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

// Main thread, from the title bar's H and tempo: likewise.
void PluginCore::setTempoSync(bool on) {
    if (tempoSync_.exchange(on) != on) {
        hostMarkDirty();
        hostRequestProcess(); // so the editor hears the tempo played, should the host be asleep
    }
}

// A host that lets the editor set its tempo plays it too, so its transport's beats keep in step.
void PluginCore::setOwnTempo(double bpm) {
    bpm = gui::clampTempo(bpm);
    if (ownTempo_.exchange(bpm) != bpm) {
        hostMarkDirty();
        hostRequestProcess();
    }
    if (hasTempo() && tempo() != bpm)
        setTempo(bpm);
}

// Main thread, as the plugin starts in a host that lets the editor set its tempo: the synth's own starts at it.
void PluginCore::startAtHostTempo() {
    if (hasTempo())
        ownTempo_.store(gui::clampTempo(tempo()));
}

void PluginCore::openManual() {
    openExternal(widePath(gui::kManualUrl)); // plain ASCII
}

void PluginCore::beginBlock() {
    // Filter and delay tails decay into denormals, which are slow; flush them to zero.
    beginDenormals();

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
    // the host's blocks, and the first one as soon as the editor opens. One too as the sequencer's curves stop: the host
    // may let the plugin sleep right after, which would leave the wheels showing where the curves last took them.
    const bool open = editorOpen_.load(std::memory_order_relaxed);
    const bool curves = synth_.seqCurvesApplied(), curvesStopped = telemetryCurves_ && !curves;
    telemetryCurves_ = curves;
    if (open && (!telemetryOpen_ || curvesStopped || telemetryFrames_ >= kTelemetrySeconds * sampleRate_)) {
        telemetryFrames_ = 0.0;
        Telemetry &t = *telemetryScratch_;
        synth_.telemetry(t);
        telemetry_.store(t);
    }
    telemetryOpen_ = open;
    playedTempo_.store(synth_.tempo(), std::memory_order_relaxed);
    // A restart for the latency waits while keys hold notes (see askRestart()): asked for once they are all up.
    const bool held = synth_.holdsKeys();
    keysHeld_.store(held);
    if (!held && latencyWaiting_.exchange(false)) {
        latencyDue_.store(true);
        hostRequestCallback();
    }
    endDenormals();
}

// Main thread: the virtual keyboard. The host may be asleep, so ask it to run us.
void PluginCore::playNote(int note, bool on, int velocity) {
    keys_.push({note, on, false, velocity});
    hostRequestProcess();
}

void PluginCore::auditionNote(int note, bool on) {
    keys_.push({note, on, true});
    hostRequestProcess();
}

void PluginCore::auditionBend(double semitones) {
    keys_.push({0, false, true, 0, true, semitones});
    hostRequestProcess();
}

// In the queue with the keys, so a key pressed after the button is played as it says.
void PluginCore::playSeqPreview(bool on, double from) {
    KeyEvent ev{0, on};
    ev.preview = true;
    ev.from = from;
    keys_.push(ev);
    hostRequestProcess();
}

// Audio thread: notes from the virtual keyboard play like MIDI notes without a note id, at their velocity; those the SEQ view plays to be
// heard on a port of their own, which the sequencer leaves be.
void PluginCore::playKeys() {
    KeyEvent ev;
    while (keys_.pop(ev)) {
        if (ev.bend) { // every note heard on its own, bent
            synth_.noteExpression({-1, kAuditionPort, -1, -1}, CLAP_NOTE_EXPRESSION_TUNING, ev.semitones);
            continue;
        }
        if (ev.preview) {
            synth_.setSeqPreview(ev.on, ev.from);
            continue;
        }
        const NoteRef note{-1, ev.audition ? kAuditionPort : int16_t{0}, 0, static_cast<int16_t>(ev.note)};
        if (ev.on)
            synth_.noteOn(note, std::clamp(ev.velocity, 0, 127) / 127.0);
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
    if (editor_)
        editor_->fitLayout(); // a host that won't resize leaves the editor drawn smaller, centered
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

bool PluginCore::saveState(const clap_ostream_t *stream, bool settings) const {
    const auto values = std::make_unique<ParamValues>();
    valuesInto(*values);
    if (!writePatch(stream, *values, layout_))
        return false;
    if (!settings)
        return true;
    if (!writeSettings(stream,
                       {oversampling_.load(), mpe_.load(), mpeBendRange_.load(), tempoSync_.load(), ownTempo_.load()}))
        return false;
    // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
    return writePresetChunk(stream, patchEdited(), utf8(pathOf(presetFile())));
}

bool PluginCore::loadState(const clap_istream_t *stream, bool settings) {
    const auto read = std::make_unique<ParamValues>();
    ParamValues &loaded = *read;
    if (!readParams(stream, loaded))
        return false;
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    valuesChanged();
    // Hosts re-read the values only when asked (a preset's load asks in notifyParams()); the remote pages follow in
    // sectionsChanged().
    if (settings)
        hostValuesChanged();
    // The state's voice limit: more voices than are allocated take a restart (a lower limit the audio thread takes
    // at its next block), and hosts read the voice info again.
    voiceLimitChanged(true);
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
        tempoSync_.store(stored.tempoSync);
        ownTempo_.store(stored.ownTempo);
        if (hasTempo() && tempo() != stored.ownTempo)
            setTempo(stored.ownTempo);
        restartIfNeeded();

        // Which preset it was. Without that, it shows Init, changed unless it is Init.
        std::string preset;
        bool edited = false;
        if (readPresetChunk(stream, preset, edited)) {
            // Save writes the patch over the preset named here, so a project names only a preset file or a factory
            // preset: any other path (a project made by hand, say) shows as an edited patch with no preset. A copy
            // of a factory preset the plugin once wrote into the preset folder is that factory preset.
            std::wstring file = PresetLibrary::resolve(widePath(preset));
            const bool factory = PresetLibrary::factoryPreset(file) != nullptr;
            std::wstring extension = factory ? L".fmv" : wideOf(pathOf(file).extension());
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
    voiceLimitChanged(true);
    hostVoicesChanged();
    hostMarkDirty();
}

// The files in the folder `from` are in `to` now, or gone for an empty `to`: the preset the patch is, and the favorite
// and recent presets, follow them. A patch whose file went stays as it sounds, an edited patch with no preset.
void PluginCore::presetFolderMoved(const std::wstring &from, const std::wstring &to) {
    const std::wstring prefix = from + kSep;
    const std::wstring &file = presetFile();
    if (file.size() > prefix.size() && samePath(file.substr(0, prefix.size()), prefix)) {
        if (to.empty())
            setPreset(L"", true);
        else
            setPreset(to + file.substr(from.size()), patchEdited());
    }
    const std::string from8 = utf8(pathOf(from)) + static_cast<char>(kSep),
                      to8 = utf8(pathOf(to)) + static_cast<char>(kSep);
    auto inFolder = [&](const std::string &kept) {
        return kept.size() > from8.size() &&
               (samePath(widePath(kept.substr(0, from8.size())), widePath(from8)));
    };
    readUserSettings(); // those another instance marked in it follow too
    bool moved = false;
    for (std::vector<std::string> *list : {&userSettings.favorites, &userSettings.recent}) {
        if (to.empty()) {
            moved = std::erase_if(*list, inFolder) > 0 || moved;
            continue;
        }
        for (std::string &kept : *list)
            if (inFolder(kept)) {
                kept = to8 + kept.substr(from8.size());
                moved = true;
            }
    }
    if (moved)
        writeUserSettings();
}

bool PluginCore::renamePresetFolder(const std::string &from, const std::string &to) {
    const std::wstring a = PresetLibrary::subfolderPath(from), b = PresetLibrary::subfolderPath(to);
    if (!presets_.renameFolder(from, to))
        return false;
    presetFolderMoved(a, b);
    return true;
}

bool PluginCore::deletePresetFolder(const std::string &folder) {
    const std::wstring path = PresetLibrary::subfolderPath(folder);
    if (!presets_.deleteFolder(folder))
        return false;
    presetFolderMoved(path, L"");
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
    FILE *file = openPath(path, "rb");
    if (!file)
        return false;
    const clap_istream_t stream = fileIn(file);
    const bool loaded = loadPresetFrom(&stream, path);
    std::fclose(file);
    return loaded;
}

bool PluginCore::importDx7(const std::wstring &path) {
    namespace fs = std::filesystem;
    FILE *file = openPath(path, "rb");
    if (!file)
        return false;
    std::vector<unsigned char> bytes;
    unsigned char chunk[4096];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), file)) > 0 && bytes.size() < (16u << 20);)
        bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(file);
    // The folder its presets go into, named as the file, what a folder's name can't hold replaced.
    std::string bank = utf8(pathOf(path).stem());
    for (char &c : bank)
        if (static_cast<unsigned char>(c) < 0x20 || std::strchr("\\/:*?\"<>|", c))
            c = '-';
    while (!bank.empty() && (bank.back() == '.' || bank.back() == ' '))
        bank.pop_back();
    while (!bank.empty() && bank.front() == ' ')
        bank.erase(bank.begin());
    if (!PresetLibrary::validName(bank))
        bank = "Import";
    const std::vector<gui::dx7::Preset> presets = gui::dx7::presetsOf(bytes.data(), bytes.size(), bank);
    const std::string folder = std::string(tr(Str::PresetsDx7Folder)) + "/" + bank;
    std::wstring first;
    for (const gui::dx7::Preset &preset : presets) {
        const std::wstring target = PresetLibrary::presetFile(folder, preset.name);
        if (target.empty())
            continue;
        std::error_code error;
        fs::create_directories(pathOf(target).parent_path(), error);
        // Written beside its file first and put in its place once whole, as Save writes a preset.
        const std::wstring part = target + L".part";
        FILE *out = openPath(part, "wb");
        if (!out)
            continue;
        const bool written = std::fwrite(preset.bytes.data(), 1, preset.bytes.size(), out) == preset.bytes.size();
        if (std::fclose(out) != 0 || !written || !replaceFile(part, target)) {
            fs::remove(pathOf(part), error);
            continue;
        }
        if (first.empty())
            first = target;
    }
    if (first.empty())
        return false;
    rescanPresets(); // so the library lists them
    return loadPreset(first);
}

bool PluginCore::loadPresetFrom(const clap_istream_t *stream, const std::wstring &path) {
    const auto told = std::make_unique<ParamValues>();
    valuesInto(*told);
    const bool loaded = loadState(stream, false);
    if (loaded) {
        // The preset plays at the tempo it was saved at: the synth's own tempo takes it (heard while the H is off,
        // and always in a host that lets the editor set its tempo).
        if (layout_.savedTempo > 0.0f)
            setOwnTempo(layout_.savedTempo);
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
    // A factory preset is built in: Save As saves it anew. A file in the old Factory subfolder named as one would never
    // be listed, and would load as the built-in one (see PresetLibrary::resolve()).
    if (path.starts_with(PresetLibrary::kFactoryPrefix) || PresetLibrary::factoryPreset(PresetLibrary::resolve(path)))
        return false;
    const fs::path target(pathOf(path)), part(pathOf(path + L".part"));
    FILE *file = openPath(wideOf(part), "wb");
    if (!file)
        return false;
    // The tempo it plays at goes with it: its own while it doesn't follow the host, else the host's, as the last
    // block played it. The patch keeps it only once the file is written.
    const float tempoBefore = layout_.savedTempo;
    const double bpm = !tempoSync() ? ownTempo() : playedTempo_.load(std::memory_order_relaxed);
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
    if (!replaceFile(wideOf(part), wideOf(target))) {
        layout_.savedTempo = tempoBefore;
        return false;
    }
    setPreset(path);
    rescanPresets(); // so the library lists it
    return true;
}

void PluginCore::choosePreset(gui::PresetDialog dialog) {
    PresetLibrary::makeFolder(); // where the dialogs start
    if (editor_)
        editor_->choosePreset(dialog);
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
    return utf8(pathOf(presetFile()).stem());
}

bool PluginCore::loadPresetAt(int index) {
    return index >= 0 && index < static_cast<int>(presets_.files().size()) && loadPreset(presets_.files()[index]);
}

bool PluginCore::savePresetIn(const std::string &folder, const std::string &name, const gui::PresetInfo &info) {
    const std::wstring path = PresetLibrary::presetFile(folder, name);
    if (path.empty())
        return false;
    std::error_code ignored;
    std::filesystem::create_directories(pathOf(path).parent_path(), ignored);
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
    return !path.empty() && std::filesystem::exists(pathOf(path), ignored);
}

// The file read whole (its params and its layout), its info replaced, and written back as a preset saved is: beside
// it first, then in its place, so a write that fails leaves it as it was.
bool PluginCore::setPresetInfo(int index, const gui::PresetInfo &info) {
    namespace fs = std::filesystem;
    if (index < 0 || index >= static_cast<int>(presets_.files().size()) || presets_.entries()[index].factory)
        return false;
    const std::wstring path = presets_.files()[index];
    const auto values = std::make_unique<ParamValues>();
    gui::UiLayout layout;
    {
        FILE *file = openPath(path, "rb");
        if (!file)
            return false;
        const clap_istream_t stream = fileIn(file);
        const bool read = readParams(&stream, *values);
        if (read)
            layout = readLayout(&stream);
        std::fclose(file);
        if (!read)
            return false;
    }
    layout.presetInfo = info;
    const fs::path part(pathOf(path + L".part"));
    FILE *file = openPath(wideOf(part), "wb");
    if (!file)
        return false;
    const clap_ostream_t stream = fileOut(file);
    const bool written = writePatch(&stream, *values, layout);
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::error_code ignored;
        fs::remove(part, ignored);
        return false;
    }
    if (!replaceFile(wideOf(part), path))
        return false;
    if (presets_.indexOf(presetFile()) == index) { // the patch is that preset: its info is the file's now
        layout_.presetInfo = info;
        layoutChanged();
    }
    rescanPresets();
    return true;
}

// The file moved, one of its name there replaced; what named it follows it: the patch, if it is that preset, and the
// favorites and recent presets (another instance's too).
bool PluginCore::movePreset(int index, const std::string &folder) {
    namespace fs = std::filesystem;
    if (index < 0 || index >= static_cast<int>(presets_.files().size()) || presets_.entries()[index].factory)
        return false;
    const std::wstring from = presets_.files()[index];
    const std::wstring to = PresetLibrary::presetFile(folder, presets_.entries()[index].name);
    if (to.empty())
        return false;
    if (samePath(from, to))
        return true;
    std::error_code ignored;
    fs::create_directories(pathOf(to).parent_path(), ignored);
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
        return false;
#else
    fs::rename(pathOf(from), pathOf(to), ignored);
    if (ignored)
        return false;
#endif
    if (samePath(presetFile(), from))
        setPreset(to, patchEdited());
    const std::string from8 = utf8(pathOf(from)), to8 = utf8(pathOf(to));
    readUserSettings();
    bool moved = false;
    for (std::vector<std::string> *list : {&userSettings.favorites, &userSettings.recent}) {
        if (std::find(list->begin(), list->end(), from8) == list->end())
            continue;
        std::erase(*list, to8); // the preset it replaced, whose file is this one's now
        std::replace(list->begin(), list->end(), from8, to8);
        moved = true;
    }
    if (moved)
        writeUserSettings();
    rescanPresets();
    return true;
}

bool PluginCore::presetFavorite(int index) const {
    if (index < 0 || index >= static_cast<int>(presets_.files().size()))
        return false;
    const std::string file = utf8(pathOf(presets_.files()[index]));
    return std::find(userSettings.favorites.begin(), userSettings.favorites.end(), file) != userSettings.favorites.end();
}

void PluginCore::setPresetFavorite(int index, bool on) {
    if (index < 0 || index >= static_cast<int>(presets_.files().size()) || presetFavorite(index) == on)
        return;
    const std::string file = utf8(pathOf(presets_.files()[index]));
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
    const std::string file = utf8(pathOf(path));
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

std::string PluginCore::presetAuthor() const {
    return userSettings.author;
}

void PluginCore::setPresetAuthor(const std::string &author) {
    if (userSettings.author == author)
        return;
    userSettings.author = author;
    writeUserSettings();
}

} // namespace fmvibe
