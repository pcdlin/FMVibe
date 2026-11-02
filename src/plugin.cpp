#include "plugin.h"

#include "gui/controller.h"
#include "gui/editor.h"
#include "params.h"
#include "presets.h"
#include "remotepages.h"
#include "sharedtelemetry.h"
#include "state.h"
#include "synth.h"
#include "uistrings.h"
#include "usersettings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <xmmintrin.h>

namespace fmvibe {

namespace {

const char *const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

// Note, param-mod and note-expression events all address voices the same way.
template <class Event>
NoteRef noteRef(const Event &ev) {
    return {ev.note_id, ev.port_index, ev.channel, ev.key};
}

// Single-producer single-consumer ring buffer: editor (main thread) to audio thread.
template <class T, size_t N>
class SpscQueue {
public:
    bool push(const T &item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        items_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Producer only: how many more items fit for certain (the consumer may make more room meanwhile).
    size_t room() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        return (tail_.load(std::memory_order_acquire) + N - head - 1) % N;
    }

    bool pop(T &item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        item = items_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> items_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// An edit made in the editor, on its way to the host as a param event.
struct EditEvent {
    enum class Type : uint8_t { Begin, Value, End };
    Type type;
    clap_id id;
    double value;
};

// How much of the edit queue only gestures' ends may take.
constexpr size_t kEndRoom = 256;

// How long after the plugin tells the host a param's value it takes none for that param from the host.
constexpr int64_t kHostEchoMs = 500;

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A key of the editor's virtual keyboard, on its way to the audio thread.
struct KeyEvent {
    int note;
    bool on;
};

// Velocity of notes played from the virtual keyboard.
constexpr double kKeyVelocity = 100.0 / 127.0;

} // namespace

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "io.github.pcdlin.fmvibe",
    "FMVibe",
    "pcdlin",
    "", // url
    "", // manual_url
    "", // support_url
    FMVIBE_VERSION,
    "Six-operator FM synthesizer",
    kFeatures,
};

class Plugin final : public gui::Controller {
public:
    explicit Plugin(const clap_host_t *host);

    const clap_plugin_t *clapPlugin() const { return &plugin_; }

    // gui::Controller, main thread.
    double value(int index) const override { return values_[index].load(); }
    void telemetry(Telemetry &out) const override { telemetry_.load(out); }
    gui::UiLayout &layout() override { return layout_; }
    void layoutChanged() override {
        sectionsChanged(); // the sources' order
        editorFitChanged();
        if (hostState_)
            hostState_->mark_dirty(host_);
    }
    void playNote(int note, bool on) override;
    void pitchWheel(double amount) override { wheelBend_.store(amount); }
    void resetPreset() override;
    bool loadPreset(const std::wstring &path) override;
    bool savePreset(const std::wstring &path) override;
    void choosePreset(bool save) override;
    void rescanPresets() override { presets_.rescan(); }
    const std::vector<gui::PresetEntry> &presets() const override { return presets_.entries(); }
    int presetIndex() const override;
    std::string presetName() const override;
    bool loadPresetAt(int index) override;
    std::wstring presetFolder() const override;
    void openPresetFolder() override;
    int oversampling() const override { return oversampling_.load(); }
    void setOversampling(int factor) override;
    std::string theme() const override { return userSettings.theme; }
    void setTheme(const std::string &name) override {
        userSettings.theme = name;
        writeUserSettings();
    }
    double glow() const override { return userSettings.glow; }
    void setGlow(double amount) override {
        userSettings.glow = std::clamp(amount, 0.0, 1.0);
        writeUserSettings();
    }

protected:
    void performBeginEdit(int index) override { queueEdit({EditEvent::Type::Begin, idOfIndex(index), 0.0}); }
    void performSetValue(int index, double value) override {
        values_[index].store(value);
        queueEdit({EditEvent::Type::Value, idOfIndex(index), value});
        if (shapesSections(index))
            sectionsChanged();
        if (index == paramIndex(GlobalParam::Voices)) {
            voiceLimitChanged();
            if (hostVoiceInfo_)
                hostVoiceInfo_->changed(host_);
        }
    }
    void performEndEdit(int index) override { queueEdit({EditEvent::Type::End, idOfIndex(index), 0.0}); }

private:
    static Plugin &self(const clap_plugin_t *plugin) { return *static_cast<Plugin *>(plugin->plugin_data); }
    static const void *extension(const char *id);

    bool init();
    bool activate(double sampleRate);
    int voiceLimit() const;
    void voiceLimitChanged();
    void applyPlayMode();
    clap_process_status process(const clap_process_t *process);
    void playKeys();
    void handleEvent(const clap_event_header_t *ev);
    void applyTransport(const clap_event_transport_t &transport);
    void setParam(const clap_event_param_value_t &ev);
    void queueEdit(const EditEvent &ev);
    void sendEdits(const clap_output_events_t *out);
    void notifyParams(const ParamValues &before);
    // Which effect each slot holds, the chain order and which source each place
    // holds: what hosts see of the effects and sources.
    static bool shapesSections(int index) {
        return fxFieldOf(index) == FxParam::Type || sourceFieldOf(index) == SourceParam::Type ||
               index == paramIndex(GlobalParam::FxOrder) || fxFieldOf(index) == FxParam::ReverbType;
    }
    void sectionsChanged();
    // The editor's height follows the modulation panel; asks the host to resize the window.
    void editorFitChanged();
    ParamValues valuesNow() const;
    void reportEndedNotes(const clap_output_events_t *out, uint32_t frames);
    const ParamValues &currentParams();

    // Preset files leave out the settings, and loading one keeps them as they are.
    bool saveState(const clap_ostream_t *stream, bool settings = true) const;
    bool loadState(const clap_istream_t *stream, bool settings = true);
    // What a preset leaves as it is: how big the editor is, whether the modulation panel is
    // collapsed, the shape editor's grid.
    void keepEditorLayout(const gui::UiLayout &from);
    // A new oversampling factor takes a deactivate/activate, like more voices.
    void restartIfNeeded();

    static const clap_plugin_audio_ports_t kAudioPorts;
    static const clap_plugin_note_ports_t kNotePorts;
    static const clap_plugin_params_t kParamsExt;
    static const clap_plugin_state_t kState;
    static const clap_plugin_remote_controls_t kRemoteControls;
    static const clap_plugin_voice_info_t kVoiceInfo;
    static const clap_plugin_gui_t kGui;

    clap_plugin_t plugin_;
    const clap_host_t *host_;
    const clap_host_params_t *hostParams_ = nullptr;
    const clap_host_state_t *hostState_ = nullptr;
    const clap_host_voice_info_t *hostVoiceInfo_ = nullptr;
    const clap_host_remote_controls_t *hostRemoteControls_ = nullptr;
    const clap_host_gui_t *hostGui_ = nullptr;
    // Main thread: the remote pages, and what they were built for: the effect
    // types, then the chain order, the source types, the sources' order and each reverb's algorithm.
    std::vector<clap_remote_controls_page_t> remotePages_;
    std::array<int64_t, kFxSlots + 1 + 2 * kMaxSources + kFxSlots> shownSections_{};
    Synth synth_;
    // Raising the voice limit past the allocated voices, or a new oversampling
    // factor, needs a deactivate/activate.
    std::atomic<bool> restartRequested_{false};
    std::atomic<int> oversampling_{kDefaultOversampling};
    // Written by the audio thread (param events) and the main thread (editor,
    // state load), read by both, hence atomics. The audio thread works from a
    // copy taken at the start of each block, which the block's param events update.
    std::array<std::atomic<double>, kParamCount> values_;
    ParamValues snapshot_{};
    // Presets set every param at once, about three events each.
    SpscQueue<EditEvent, 3 * kParamCount + 1024> edits_;
    // When each param's value last went out to the host (steady clock, ms); see setParam(). Audio thread, or
    // flush() which never runs alongside it.
    std::array<int64_t, kParamCount> toldAt_{};
    SpscQueue<KeyEvent, 256> keys_;
    std::atomic<double> wheelBend_{0.0};
    SharedTelemetry telemetry_;
    std::atomic<bool> editorOpen_{false};
    gui::UiLayout layout_;
    std::unique_ptr<gui::Editor> editor_;
    // The preset library as last scanned, with each entry's file (the patch's own is the Controller's).
    // Main thread.
    PresetLibrary presets_;
};

Plugin::Plugin(const clap_host_t *host) : host_(host) {
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(table[i].def);
    remotePages_ = buildRemotePages(valuesNow(), layout_);

    plugin_ = {
        &kDescriptor,
        this,
        [](const clap_plugin_t *p) { return self(p).init(); },
        [](const clap_plugin_t *p) { delete &self(p); },
        [](const clap_plugin_t *p, double sampleRate, uint32_t, uint32_t) { return self(p).activate(sampleRate); },
        [](const clap_plugin_t *p) { self(p).telemetry_.store(Telemetry{}); }, // deactivate
        [](const clap_plugin_t *) { return true; },                           // start_processing
        [](const clap_plugin_t *p) { self(p).telemetry_.store(Telemetry{}); }, // stop_processing
        [](const clap_plugin_t *p) { self(p).synth_.reset(); },
        [](const clap_plugin_t *p, const clap_process_t *process) { return self(p).process(process); },
        [](const clap_plugin_t *, const char *id) { return extension(id); },
        [](const clap_plugin_t *p) { self(p).sectionsChanged(); }, // on_main_thread: see setParam(), loadState()
    };
}

bool Plugin::init() {
    hostParams_ = static_cast<const clap_host_params_t *>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state_t *>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostVoiceInfo_ = static_cast<const clap_host_voice_info_t *>(host_->get_extension(host_, CLAP_EXT_VOICE_INFO));
    hostGui_ = static_cast<const clap_host_gui_t *>(host_->get_extension(host_, CLAP_EXT_GUI));
    hostRemoteControls_ =
        static_cast<const clap_host_remote_controls_t *>(host_->get_extension(host_, CLAP_EXT_REMOTE_CONTROLS));
    if (!hostRemoteControls_)
        hostRemoteControls_ = static_cast<const clap_host_remote_controls_t *>(
            host_->get_extension(host_, CLAP_EXT_REMOTE_CONTROLS_COMPAT));
    return true;
}

// Voices are allocated here, on the main thread, for the current voice limit.
bool Plugin::activate(double sampleRate) {
    synth_.prepare(sampleRate, voiceLimit(), oversampling_.load());
    restartRequested_.store(false);
    return true;
}

int Plugin::voiceLimit() const {
    return std::clamp(static_cast<int>(std::lround(values_[paramIndex(GlobalParam::Voices)].load())), 1, kMaxVoices);
}

// Audio thread, or main thread while not processing.
void Plugin::applyPlayMode() {
    auto on = [this](GlobalParam p) { return values_[paramIndex(p)].load(std::memory_order_relaxed) >= 0.5; };
    synth_.setPlayMode(on(GlobalParam::Mono), on(GlobalParam::Legato), on(GlobalParam::GlideAlways));
}

// Thread-safe: request_restart may be called from any thread.
void Plugin::voiceLimitChanged() {
    if (synth_.capacity() > 0 && voiceLimit() > synth_.capacity() && !restartRequested_.exchange(true))
        host_->request_restart(host_);
}

// Main thread.
void Plugin::restartIfNeeded() {
    if (synth_.capacity() > 0 && oversampling_.load() != synth_.oversampling() && !restartRequested_.exchange(true))
        host_->request_restart(host_);
}

// Main thread, from the settings dialog.
void Plugin::setOversampling(int factor) {
    factor = validOversampling(factor);
    if (oversampling_.exchange(factor) == factor)
        return;
    restartIfNeeded();
    if (hostState_)
        hostState_->mark_dirty(host_);
}

const void *Plugin::extension(const char *id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &kNotePorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kParamsExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &kState;
    if (std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS) == 0 || std::strcmp(id, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == 0)
        return &kRemoteControls;
    if (std::strcmp(id, CLAP_EXT_VOICE_INFO) == 0)
        return &kVoiceInfo;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &kGui;
    return nullptr;
}

clap_process_status Plugin::process(const clap_process_t *process) {
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 ||
        !process->audio_outputs[0].data32)
        return CLAP_PROCESS_ERROR;

    // Filter and delay tails decay into denormals, which are slow; flush them to zero.
    const unsigned int csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040); // flush-to-zero and denormals-are-zero

    sendEdits(process->out_events);
    if (process->transport)
        applyTransport(*process->transport);
    playKeys();
    synth_.setWheelBend(wheelBend_.load());
    synth_.setAnalyzing(editorOpen_.load(std::memory_order_relaxed));
    // Editor changes to these land between blocks; apply them before this block's notes.
    synth_.setVoiceLimit(voiceLimit());
    applyPlayMode();

    clap_audio_buffer_t &output = process->audio_outputs[0];
    output.constant_mask = 0;
    float *left = output.data32[0];
    float *right = output.data32[1];

    // Render up to each event's timestamp, then apply it: sample-accurate notes and automation. The params are read
    // once for the block; a param event in it updates the copy as well (see setParam()), so what follows hears it,
    // and an edit in the editor meanwhile is heard from the next block on.
    const clap_input_events_t *in = process->in_events;
    const uint32_t frames = process->frames_count;
    const uint32_t eventCount = in->size(in);
    const ParamValues &params = currentParams();
    uint32_t cursor = 0;
    for (uint32_t i = 0; i <= eventCount; ++i) {
        const clap_event_header_t *ev = i < eventCount ? in->get(in, i) : nullptr;
        const uint32_t until = ev ? std::min(ev->time, frames) : frames;
        if (until > cursor) {
            synth_.setVoiceLimit(static_cast<int>(std::lround(params[paramIndex(GlobalParam::Voices)])));
            applyPlayMode();
            synth_.render(left + cursor, right + cursor, until - cursor, params);
            cursor = until;
        }
        if (ev)
            handleEvent(ev);
    }

    reportEndedNotes(process->out_events, frames);
    if (editorOpen_.load(std::memory_order_relaxed)) {
        Telemetry t;
        synth_.telemetry(t);
        telemetry_.store(t);
    }
    _mm_setcsr(csr);
    return synth_.activeVoiceCount() > 0 || synth_.effectsRinging() ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
}

// Main thread: the virtual keyboard. The host may be asleep, so ask it to run us.
void Plugin::playNote(int note, bool on) {
    keys_.push({note, on});
    host_->request_process(host_);
}

// Audio thread: notes from the virtual keyboard play like MIDI notes without a note id.
void Plugin::playKeys() {
    KeyEvent ev;
    while (keys_.pop(ev)) {
        const NoteRef note{-1, 0, 0, static_cast<int16_t>(ev.note)};
        if (ev.on)
            synth_.noteOn(note, kKeyVelocity);
        else
            synth_.noteOff(note);
    }
}

void Plugin::applyTransport(const clap_event_transport_t &transport) {
    if (transport.flags & CLAP_TRANSPORT_HAS_TEMPO)
        synth_.setTempo(transport.tempo);
}

void Plugin::handleEvent(const clap_event_header_t *ev) {
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;

    switch (ev->type) {
    case CLAP_EVENT_NOTE_ON: {
        const auto &note = *reinterpret_cast<const clap_event_note_t *>(ev);
        synth_.noteOn(noteRef(note), note.velocity);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
        synth_.noteOff(noteRef(*reinterpret_cast<const clap_event_note_t *>(ev)));
        break;
    case CLAP_EVENT_NOTE_CHOKE:
        synth_.choke(noteRef(*reinterpret_cast<const clap_event_note_t *>(ev)));
        break;
    case CLAP_EVENT_NOTE_EXPRESSION: {
        const auto &expr = *reinterpret_cast<const clap_event_note_expression_t *>(ev);
        synth_.noteExpression(noteRef(expr), expr.expression_id, expr.value);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE:
        setParam(*reinterpret_cast<const clap_event_param_value_t *>(ev));
        break;
    case CLAP_EVENT_TRANSPORT:
        applyTransport(*reinterpret_cast<const clap_event_transport_t *>(ev));
        break;
    case CLAP_EVENT_PARAM_MOD: {
        const auto &mod = *reinterpret_cast<const clap_event_param_mod_t *>(ev);
        synth_.paramMod(indexOfId(mod.param_id), noteRef(mod), mod.amount);
        break;
    }
    case CLAP_EVENT_MIDI: {
        const auto &midi = *reinterpret_cast<const clap_event_midi_t *>(ev);
        const uint8_t status = midi.data[0] & 0xF0;
        const NoteRef note{-1, static_cast<int16_t>(midi.port_index), static_cast<int16_t>(midi.data[0] & 0x0F),
                           static_cast<int16_t>(midi.data[1])};
        if (status == 0x90 && midi.data[2] > 0)
            synth_.noteOn(note, midi.data[2] / 127.0);
        else if (status == 0x80 || status == 0x90)
            synth_.noteOff(note);
        else if (status == 0xE0)
            synth_.pitchBend(((midi.data[2] << 7 | midi.data[1]) - 8192) / 8192.0);
        break;
    }
    }
}

void Plugin::setParam(const clap_event_param_value_t &ev) {
    const int i = indexOfId(ev.param_id);
    if (i < 0 || !std::isfinite(ev.value))
        return;
    // For a moment after the plugin tells the host a value, what the host sends may still be an older one it is
    // handing back (Bitwig does): taken, it would flip a preset just loaded back to the one before.
    if (steadyMs() - toldAt_[i] < kHostEchoMs)
        return;
    const double value = std::clamp(ev.value, paramTable()[i].min, paramTable()[i].max);
    values_[i].store(value);
    snapshot_[i] = value; // the rest of the block being processed hears it (see process())
    // Voice settings apply before any note-ons that follow in the same block.
    if (i == paramIndex(GlobalParam::Voices)) {
        synth_.setVoiceLimit(voiceLimit());
        voiceLimitChanged();
    } else if (i == paramIndex(GlobalParam::Mono) || i == paramIndex(GlobalParam::Legato) ||
               i == paramIndex(GlobalParam::GlideAlways)) {
        applyPlayMode();
    } else if (shapesSections(i)) {
        host_->request_callback(host_); // this may be the audio thread; tell the host on the main thread
    }
}

void Plugin::queueEdit(const EditEvent &ev) {
    // The value itself is already stored; the event tells the host about it. The last of the queue is kept for
    // the ends of gestures already begun, so the host is never left inside one; an edit that doesn't fit has the
    // host read every value again instead.
    const bool queued =
        ev.type == EditEvent::Type::End ? edits_.push(ev) : edits_.room() > kEndRoom && edits_.push(ev);
    if (!hostParams_)
        return;
    if (!queued)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
    hostParams_->request_flush(host_);
}

void Plugin::sendEdits(const clap_output_events_t *out) {
    EditEvent ev;
    int64_t now = -1;
    while (edits_.pop(ev)) {
        if (ev.type == EditEvent::Type::Value) {
            if (now < 0)
                now = steadyMs();
            toldAt_[indexOfId(ev.id)] = now;
            clap_event_param_value_t value{};
            value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
            value.param_id = ev.id;
            value.note_id = -1;
            value.port_index = -1;
            value.channel = -1;
            value.key = -1;
            value.value = ev.value;
            out->try_push(out, &value.header);
        } else {
            clap_event_param_gesture_t gesture{};
            gesture.header = {sizeof(gesture), 0, CLAP_CORE_EVENT_SPACE_ID,
                              static_cast<uint16_t>(ev.type == EditEvent::Type::Begin
                                                        ? CLAP_EVENT_PARAM_GESTURE_BEGIN
                                                        : CLAP_EVENT_PARAM_GESTURE_END),
                              0};
            gesture.param_id = ev.id;
            out->try_push(out, &gesture.header);
        }
    }
}

void Plugin::reportEndedNotes(const clap_output_events_t *out, uint32_t frames) {
    // A host without note ids keeps one voice per port/channel/key: if that key was struck again later in this
    // block, its voice is still alive (see Synth::isActive()).
    for (const NoteRef &note : synth_.endedNotes()) {
        if (synth_.isActive(note))
            continue;
        clap_event_note_t ev{};
        ev.header.size = sizeof(ev);
        ev.header.time = frames > 0 ? frames - 1 : 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_NOTE_END;
        ev.note_id = note.noteId;
        ev.port_index = note.port;
        ev.channel = note.channel;
        ev.key = note.key;
        out->try_push(out, &ev.header);
    }
    synth_.clearEndedNotes();
}

const ParamValues &Plugin::currentParams() {
    for (int i = 0; i < kParamCount; ++i)
        snapshot_[i] = values_[i].load(std::memory_order_relaxed);
    return snapshot_;
}

ParamValues Plugin::valuesNow() const {
    ParamValues values;
    for (int i = 0; i < kParamCount; ++i)
        values[i] = values_[i].load(std::memory_order_relaxed);
    return values;
}

// Main thread. A slot's or place's fields only show in hosts while it holds
// their type, and each effect and source gets a remote page.
void Plugin::sectionsChanged() {
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
    remotePages_ = buildRemotePages(valuesNow(), layout_);
    if (typesChanged && hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO);
    if (hostRemoteControls_)
        hostRemoteControls_->changed(host_);
}

void Plugin::editorFitChanged() {
    if (!editor_)
        return;
    uint32_t width = 0, height = 0, fitted = 0;
    editor_->size(width, height);
    editor_->fit(width, fitted);
    if (fitted == height || !hostGui_)
        return;
    // A host that won't resize leaves the editor drawn smaller, centered in its window.
    hostGui_->resize_hints_changed(host_);
    hostGui_->request_resize(host_, width, fitted);
}

bool Plugin::saveState(const clap_ostream_t *stream, bool settings) const {
    if (!writePatch(stream, valuesNow(), layout_))
        return false;
    if (!settings)
        return true;
    if (!writeSettings(stream, oversampling_.load()))
        return false;
    // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
    return writePresetChunk(stream, patchEdited(), utf8(std::filesystem::path(presetFile())));
}

bool Plugin::loadState(const clap_istream_t *stream, bool settings) {
    ParamValues loaded;
    if (!readParams(stream, loaded))
        return false;
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    // Hosts re-read the values only when asked; any hidden flags follow in sectionsChanged().
    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);

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
    host_->request_callback(host_); // sectionsChanged(), once the host is done loading
    return true;
}

// Main thread. Every param may have changed at once: tell the host about each one that did, since `before`. All
// of them, three events each, would be more than the edit queue holds once two loads come before the host takes
// them, and more than some hosts take in one go.
void Plugin::notifyParams(const ParamValues &before) {
    for (int i = 0; i < kParamCount; ++i) {
        const double value = values_[i].load();
        if (value == before[i])
            continue;
        queueEdit({EditEvent::Type::Begin, idOfIndex(i), 0.0});
        queueEdit({EditEvent::Type::Value, idOfIndex(i), value});
        queueEdit({EditEvent::Type::End, idOfIndex(i), 0.0});
    }
    sectionsChanged();
    voiceLimitChanged();
    if (hostVoiceInfo_)
        hostVoiceInfo_->changed(host_);
    if (hostState_)
        hostState_->mark_dirty(host_);
}

void Plugin::keepEditorLayout(const gui::UiLayout &from) {
    layout_.zoom = from.zoom;
    layout_.sourcesCollapsed = from.sourcesCollapsed;
    layout_.keysCollapsed = from.keysCollapsed;
    layout_.shapeGridX = from.shapeGridX;
    layout_.shapeGridY = from.shapeGridY;
}

// Init and loading a preset are undo steps like any edit in the editor; afterwards the patch is
// the preset, unchanged.
void Plugin::resetPreset() {
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
bool Plugin::loadPreset(const std::wstring &path) {
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

bool Plugin::savePreset(const std::wstring &path) {
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

void Plugin::choosePreset(bool save) {
    if (editor_)
        editor_->choosePreset(save);
}

std::wstring Plugin::presetFolder() const {
    return PresetLibrary::folderPath();
}

int Plugin::presetIndex() const {
    return presets_.indexOf(presetFile());
}

std::string Plugin::presetName() const {
    return presetFile().empty() ? tr(Str::PresetInit) : utf8(std::filesystem::path(presetFile()).stem());
}

bool Plugin::loadPresetAt(int index) {
    return index >= 0 && index < static_cast<int>(presets_.files().size()) && loadPreset(presets_.files()[index]);
}

void Plugin::openPresetFolder() {
    presets_.openFolder();
}

const clap_plugin_audio_ports_t Plugin::kAudioPorts = {
    [](const clap_plugin_t *, bool isInput) -> uint32_t { return isInput ? 0 : 1; },
    [](const clap_plugin_t *, uint32_t index, bool isInput, clap_audio_port_info_t *info) {
        if (isInput || index != 0)
            return false;
        *info = {};
        info->id = 0;
        std::snprintf(info->name, sizeof(info->name), "%s", tr(Str::PortOutput));
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const clap_plugin_note_ports_t Plugin::kNotePorts = {
    [](const clap_plugin_t *, bool isInput) -> uint32_t { return isInput ? 1 : 0; },
    [](const clap_plugin_t *, uint32_t index, bool isInput, clap_note_port_info_t *info) {
        if (!isInput || index != 0)
            return false;
        *info = {};
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s", tr(Str::PortNotes));
        return true;
    },
};

const clap_plugin_params_t Plugin::kParamsExt = {
    [](const clap_plugin_t *) { return static_cast<uint32_t>(kParamCount); },
    [](const clap_plugin_t *p, uint32_t index, clap_param_info_t *info) {
        if (index >= static_cast<uint32_t>(kParamCount))
            return false;
        const ParamDef &def = paramTable()[index];
        const Plugin &plugin = self(p);
        auto get = [&plugin](int i) { return plugin.values_[i].load(); };
        *info = {};
        info->id = def.id;
        // Fields of effect slots and source places for other types than they hold stay out of sight.
        info->flags = def.flags | (fieldInUse(get, static_cast<int>(index)) ? 0u : CLAP_PARAM_IS_HIDDEN);
        std::snprintf(info->name, sizeof(info->name), "%s", def.name.c_str());
        std::snprintf(info->module, sizeof(info->module), "%s", def.module.c_str());
        info->min_value = def.min;
        info->max_value = def.max;
        info->default_value = def.def;
        return true;
    },
    [](const clap_plugin_t *p, clap_id id, double *value) {
        const int i = indexOfId(id);
        if (i < 0)
            return false;
        *value = self(p).values_[i].load();
        return true;
    },
    [](const clap_plugin_t *, clap_id id, double value, char *out, uint32_t capacity) {
        return formatParam(indexOfId(id), value, out, capacity);
    },
    [](const clap_plugin_t *, clap_id id, const char *text, double *value) {
        return parseParam(indexOfId(id), text, value);
    },
    // Param changes while the plugin is not processing (or between blocks), and
    // the editor's edits on their way out to the host.
    [](const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) {
        Plugin &plugin = self(p);
        const uint32_t count = in->size(in);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t *ev = in->get(in, i);
            if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE)
                plugin.setParam(*reinterpret_cast<const clap_event_param_value_t *>(ev));
        }
        plugin.sendEdits(out);
    },
};

const clap_plugin_state_t Plugin::kState = {
    [](const clap_plugin_t *p, const clap_ostream_t *stream) { return self(p).saveState(stream); },
    [](const clap_plugin_t *p, const clap_istream_t *stream) {
        const bool loaded = self(p).loadState(stream);
        if (loaded)
            self(p).clearHistory();
        return loaded;
    },
};

const clap_plugin_remote_controls_t Plugin::kRemoteControls = {
    [](const clap_plugin_t *p) { return static_cast<uint32_t>(self(p).remotePages_.size()); },
    [](const clap_plugin_t *p, uint32_t index, clap_remote_controls_page_t *page) {
        if (index >= self(p).remotePages_.size())
            return false;
        *page = self(p).remotePages_[index];
        return true;
    },
};

const clap_plugin_voice_info_t Plugin::kVoiceInfo = {
    [](const clap_plugin_t *p, clap_voice_info_t *info) {
        const Plugin &plugin = self(p);
        const int capacity = plugin.synth_.capacity() > 0 ? plugin.synth_.capacity() : plugin.voiceLimit();
        info->voice_count = static_cast<uint32_t>(std::min(plugin.voiceLimit(), capacity));
        info->voice_capacity = static_cast<uint32_t>(capacity);
        info->flags = CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES;
        return true;
    },
};

// Embedded Win32 editor only; no floating windows.
const clap_plugin_gui_t Plugin::kGui = {
    [](const clap_plugin_t *, const char *api, bool isFloating) {
        return !isFloating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0;
    },
    [](const clap_plugin_t *, const char **api, bool *isFloating) {
        *api = CLAP_WINDOW_API_WIN32;
        *isFloating = false;
        return true;
    },
    [](const clap_plugin_t *p, const char *api, bool isFloating) {
        Plugin &plugin = self(p);
        if (isFloating || std::strcmp(api, CLAP_WINDOW_API_WIN32) != 0 || plugin.editor_)
            return false;
        readUserSettings();
        plugin.editor_ = std::make_unique<gui::Editor>(plugin);
        plugin.editorOpen_.store(true);
        return true;
    },
    [](const clap_plugin_t *p) {
        Plugin &plugin = self(p);
        plugin.editorOpen_.store(false);
        plugin.editor_.reset();
    },
    [](const clap_plugin_t *, double) { return false; }, // set_scale: the editor asks Windows for the DPI
    [](const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
        if (!self(p).editor_)
            return false;
        self(p).editor_->size(*width, *height);
        return true;
    },
    [](const clap_plugin_t *) { return true; }, // can_resize
    [](const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = true;
        hints->aspect_ratio_width = static_cast<uint32_t>(gui::View::kWidth);
        hints->aspect_ratio_height = static_cast<uint32_t>(gui::View::heightFor(self(p).layout_));
        return true;
    },
    [](const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
        if (self(p).editor_)
            self(p).editor_->fit(*width, *height);
        else
            gui::Editor::constrain(*width, *height, gui::View::heightFor(self(p).layout_));
        return true;
    },
    [](const clap_plugin_t *p, uint32_t width, uint32_t height) {
        return self(p).editor_ && self(p).editor_->setSize(width, height);
    },
    [](const clap_plugin_t *p, const clap_window_t *window) {
        return self(p).editor_ && std::strcmp(window->api, CLAP_WINDOW_API_WIN32) == 0 &&
               self(p).editor_->attach(static_cast<HWND>(window->win32));
    },
    [](const clap_plugin_t *, const clap_window_t *) { return false; }, // set_transient: not floating
    [](const clap_plugin_t *, const char *) {},                         // suggest_title
    [](const clap_plugin_t *p) { return self(p).editor_ && self(p).editor_->show(true); },
    [](const clap_plugin_t *p) { return self(p).editor_ && self(p).editor_->show(false); },
};

const clap_plugin_t *createPlugin(const clap_host_t *host) {
    return (new Plugin(host))->clapPlugin();
}

} // namespace fmvibe
