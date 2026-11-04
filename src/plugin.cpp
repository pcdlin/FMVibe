#include "plugin.h"

#include "params.h"
#include "plugincore.h"
#include "remotepages.h"
#include "uistrings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

// An edit made in the editor, on its way to the host as a param event.
struct EditEvent {
    enum class Type : uint8_t { Begin, Value, End };
    Type type;
    clap_id id;
    double value;
};

} // namespace

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "io.github.pcdlin.fmvibe",
    kPluginName,
    kPluginVendor,
    "", // url
    "", // manual_url
    "", // support_url
    FMVIBE_VERSION,
    kPluginDescription,
    kFeatures,
};

// The CLAP plugin: the core, with CLAP's process loop, events and extensions.
class Plugin final : public PluginCore {
public:
    explicit Plugin(const clap_host_t *host);

    const clap_plugin_t *clapPlugin() const { return &plugin_; }

protected:
    void hostBeginEdit(int index) override { queueEdit(index, {EditEvent::Type::Begin, idOfIndex(index), 0.0}); }
    void hostEdit(int index, double value) override {
        queueEdit(index, {EditEvent::Type::Value, idOfIndex(index), value});
    }
    void hostEndEdit(int index) override { queueEdit(index, {EditEvent::Type::End, idOfIndex(index), 0.0}); }
    void hostValuesChanged() override {
        if (hostParams_)
            hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
    }
    void hostSectionsChanged(bool fieldsShown) override {
        remotePages_ = buildRemotePages(valuesNow(), uiLayout());
        if (fieldsShown && hostParams_)
            hostParams_->rescan(host_, CLAP_PARAM_RESCAN_INFO);
        if (hostRemoteControls_)
            hostRemoteControls_->changed(host_);
    }
    void hostMarkDirty() override {
        if (hostState_)
            hostState_->mark_dirty(host_);
    }
    void hostVoicesChanged() override {
        if (hostVoiceInfo_)
            hostVoiceInfo_->changed(host_);
    }
    void hostResizeEditor(uint32_t width, uint32_t height) override {
        if (!hostGui_)
            return;
        hostGui_->resize_hints_changed(host_);
        hostGui_->request_resize(host_, width, height);
    }
    void hostRequestRestart() override { host_->request_restart(host_); } // thread-safe
    void hostRequestCallback() override { host_->request_callback(host_); }
    void hostRequestProcess() override { host_->request_process(host_); }

private:
    static Plugin &self(const clap_plugin_t *plugin) { return *static_cast<Plugin *>(plugin->plugin_data); }
    static const void *extension(const char *id);

    bool init();
    clap_process_status process(const clap_process_t *process);
    void handleEvent(const clap_event_header_t *ev);
    void applyTransport(const clap_event_transport_t &transport);
    void setParam(const clap_event_param_value_t &ev) { setHostValue(indexOfId(ev.param_id), ev.value, true); }
    void queueEdit(int index, const EditEvent &ev);
    // What the host's main-thread callback does: the values rescan queueEdit() put off, then onMainThread().
    void mainThread();
    void sendEdits(const clap_output_events_t *out);
    void reportEndedNotes(const clap_output_events_t *out, uint32_t frames);

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
    // Main thread: the remote pages, as the sections were when the host was last told (see sectionsChanged()).
    std::vector<clap_remote_controls_page_t> remotePages_;
    // Presets set every param at once, about three events each.
    SpscQueue<EditEvent, 3 * kParamCount + 1024> edits_;
    // Main thread: how many of each param's gestures went out begun, whose ends the queue keeps room for, and how
    // many in all (see queueEdit()).
    std::array<uint16_t, kParamCount> begun_{};
    size_t openGestures_ = 0;
    // Main thread: an edit didn't fit in the queue, and the host is to read every value again.
    bool rescanValues_ = false;
};

Plugin::Plugin(const clap_host_t *host) : host_(host) {
    remotePages_ = buildRemotePages(valuesNow(), uiLayout());

    plugin_ = {
        &kDescriptor,
        this,
        [](const clap_plugin_t *p) { return self(p).init(); },
        [](const clap_plugin_t *p) {
            // The editor goes first, while this is still a Plugin: closing it may end a gesture (hostEndEdit()).
            self(p).closeEditor();
            delete &self(p);
        },
        [](const clap_plugin_t *p, double sampleRate, uint32_t, uint32_t) {
            self(p).activate(sampleRate);
            return true;
        },
        [](const clap_plugin_t *p) { self(p).deactivate(); },
        [](const clap_plugin_t *) { return true; },              // start_processing
        [](const clap_plugin_t *p) { self(p).deactivate(); },    // stop_processing: the editor's telemetry stops
        [](const clap_plugin_t *p) { self(p).resetVoices(); },
        [](const clap_plugin_t *p, const clap_process_t *process) { return self(p).process(process); },
        [](const clap_plugin_t *, const char *id) { return extension(id); },
        [](const clap_plugin_t *p) { self(p).mainThread(); }, // see setHostValue(), loadState(), queueEdit()
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

    sendEdits(process->out_events);
    if (process->transport)
        applyTransport(*process->transport);
    beginBlock();

    clap_audio_buffer_t &output = process->audio_outputs[0];
    output.constant_mask = 0;
    float *left = output.data32[0];
    float *right = output.data32[1];

    // Render up to each event's timestamp, then apply it: sample-accurate notes and automation.
    const clap_input_events_t *in = process->in_events;
    const uint32_t frames = process->frames_count;
    const uint32_t eventCount = in->size(in);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i <= eventCount; ++i) {
        const clap_event_header_t *ev = i < eventCount ? in->get(in, i) : nullptr;
        const uint32_t until = ev ? std::min(ev->time, frames) : frames;
        if (until > cursor) {
            render(left + cursor, right + cursor, until - cursor);
            cursor = until;
        }
        if (ev)
            handleEvent(ev);
    }

    reportEndedNotes(process->out_events, frames);
    endBlock();
    return synth().activeVoiceCount() > 0 || synth().effectsRinging() ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
}

void Plugin::applyTransport(const clap_event_transport_t &transport) {
    if (transport.flags & CLAP_TRANSPORT_HAS_TEMPO)
        synth().setTempo(transport.tempo);
}

void Plugin::handleEvent(const clap_event_header_t *ev) {
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;

    Synth &synth = this->synth();
    switch (ev->type) {
    case CLAP_EVENT_NOTE_ON: {
        const auto &note = *reinterpret_cast<const clap_event_note_t *>(ev);
        synth.noteOn(noteRef(note), note.velocity);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
        synth.noteOff(noteRef(*reinterpret_cast<const clap_event_note_t *>(ev)));
        break;
    case CLAP_EVENT_NOTE_CHOKE:
        synth.choke(noteRef(*reinterpret_cast<const clap_event_note_t *>(ev)));
        break;
    case CLAP_EVENT_NOTE_EXPRESSION: {
        const auto &expr = *reinterpret_cast<const clap_event_note_expression_t *>(ev);
        synth.noteExpression(noteRef(expr), expr.expression_id, expr.value);
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
        synth.paramMod(indexOfId(mod.param_id), noteRef(mod), mod.amount);
        break;
    }
    case CLAP_EVENT_MIDI: {
        const auto &midi = *reinterpret_cast<const clap_event_midi_t *>(ev);
        const uint8_t status = midi.data[0] & 0xF0;
        const NoteRef note{-1, static_cast<int16_t>(midi.port_index), static_cast<int16_t>(midi.data[0] & 0x0F),
                           static_cast<int16_t>(midi.data[1])};
        if (status == 0x90 && midi.data[2] > 0)
            synth.noteOn(note, midi.data[2] / 127.0);
        else if (status == 0x80 || status == 0x90)
            synth.noteOff(note);
        else if (status == 0xE0)
            synth.pitchBend(note.channel, ((midi.data[2] << 7 | midi.data[1]) - 8192) / 8192.0);
        else if (status == 0xB0)
            synth.controlChange(note.channel, midi.data[1] & 0x7F, (midi.data[2] & 0x7F) / 127.0);
        else if (status == 0xD0)
            synth.channelPressure(note.channel, (midi.data[1] & 0x7F) / 127.0);
        else if (status == 0xA0)
            synth.polyPressure(note, (midi.data[2] & 0x7F) / 127.0);
        break;
    }
    }
}

// Main thread.
void Plugin::queueEdit(int index, const EditEvent &ev) {
    // The value itself is already stored; the event tells the host about it. The queue keeps room for the end of
    // each gesture begun, so the host is never left inside one, and a gesture whose begin didn't fit goes without
    // its end. An edit that doesn't fit has the host read every value again instead, once for all of them, when it
    // calls back (a large undo may not fit by thousands).
    bool queued = false;
    switch (ev.type) {
    case EditEvent::Type::Begin: // room for itself, and for its end with the others'
        queued = begun_[index] < UINT16_MAX && edits_.room() >= openGestures_ + 2 && edits_.push(ev);
        if (queued) {
            ++begun_[index];
            ++openGestures_;
        }
        break;
    case EditEvent::Type::Value:
        queued = edits_.room() >= openGestures_ + 1 && edits_.push(ev);
        break;
    case EditEvent::Type::End:
        if (begun_[index] == 0)
            return; // its begin didn't go out
        --begun_[index];
        --openGestures_;
        queued = edits_.push(ev); // the room kept for it
        break;
    }
    if (!queued) {
        if (!rescanValues_) {
            rescanValues_ = true;
            hostRequestCallback();
        }
        return;
    }
    if (hostParams_)
        hostParams_->request_flush(host_);
}

void Plugin::mainThread() {
    if (rescanValues_) {
        rescanValues_ = false;
        if (hostParams_)
            hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);
    }
    onMainThread();
}

// Audio thread, or flush() which never runs alongside it.
void Plugin::sendEdits(const clap_output_events_t *out) {
    EditEvent ev;
    int64_t now = -1;
    while (edits_.pop(ev)) {
        if (ev.type == EditEvent::Type::Value) {
            if (now < 0)
                now = steadyMs();
            told(indexOfId(ev.id), now);
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
    for (const NoteRef &note : synth().endedNotes()) {
        if (synth().isActive(note))
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
    // endBlock() clears them
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
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
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
        auto get = [&plugin](int i) { return plugin.value(i); };
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
        *value = self(p).value(i);
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
        // The editor's edits first, as in process(): the host's values that follow are then checked against them.
        plugin.sendEdits(out);
        const uint32_t count = in->size(in);
        for (uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t *ev = in->get(in, i);
            if (ev->space_id == CLAP_CORE_EVENT_SPACE_ID && ev->type == CLAP_EVENT_PARAM_VALUE)
                plugin.setParam(*reinterpret_cast<const clap_event_param_value_t *>(ev));
        }
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
        const int capacity = plugin.synth().capacity() > 0 ? plugin.synth().capacity() : plugin.voiceLimit();
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
        return !isFloating && std::strcmp(api, CLAP_WINDOW_API_WIN32) == 0 && self(p).openEditor();
    },
    [](const clap_plugin_t *p) { self(p).closeEditor(); },
    [](const clap_plugin_t *, double) { return false; }, // set_scale: the editor asks Windows for the DPI
    [](const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
        if (!self(p).editor())
            return false;
        self(p).editor()->size(*width, *height);
        return true;
    },
    [](const clap_plugin_t *) { return true; }, // can_resize
    [](const clap_plugin_t *p, clap_gui_resize_hints_t *hints) {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = true;
        hints->aspect_ratio_width = static_cast<uint32_t>(gui::View::kWidth);
        hints->aspect_ratio_height = static_cast<uint32_t>(gui::View::heightFor(self(p).uiLayout()));
        return true;
    },
    [](const clap_plugin_t *p, uint32_t *width, uint32_t *height) {
        if (self(p).editor())
            self(p).editor()->fit(*width, *height);
        else
            gui::Editor::constrain(*width, *height, gui::View::heightFor(self(p).uiLayout()));
        return true;
    },
    [](const clap_plugin_t *p, uint32_t width, uint32_t height) {
        return self(p).editor() && self(p).editor()->setSize(width, height);
    },
    [](const clap_plugin_t *p, const clap_window_t *window) {
        return self(p).editor() && std::strcmp(window->api, CLAP_WINDOW_API_WIN32) == 0 &&
               self(p).editor()->attach(static_cast<HWND>(window->win32));
    },
    [](const clap_plugin_t *, const clap_window_t *) { return false; }, // set_transient: not floating
    [](const clap_plugin_t *, const char *) {},                         // suggest_title
    [](const clap_plugin_t *p) { return self(p).editor() && self(p).editor()->show(true); },
    [](const clap_plugin_t *p) { return self(p).editor() && self(p).editor()->show(false); },
};

const clap_plugin_t *createPlugin(const clap_host_t *host) {
    return (new Plugin(host))->clapPlugin();
}

} // namespace fmvibe
