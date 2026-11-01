// Minimal headless CLAP host: loads the built plugin like a DAW would, checks
// its params and state, plays notes through it and checks what comes out.
// Usage: fmvibe-smoke <path\to\FMVibe.clap>

#include <clap/clap.h>

#include "params.h" // param ids only; everything else goes through CLAP

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace fmvibe;

namespace {

int failures = 0;
int restartRequests = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

const clap_host_t kHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "fmvibe-smoke",
    "",
    "",
    "0",
    [](const clap_host_t *, const char *) -> const void * { return nullptr; },
    [](const clap_host_t *) { ++restartRequests; },
    [](const clap_host_t *) {}, // request_process
    [](const clap_host_t *) {}, // request_callback
};

struct InputEvents {
    std::vector<const clap_event_header_t *> events;
    clap_input_events_t list{this, &size, &get};

    static uint32_t CLAP_ABI size(const clap_input_events_t *l) {
        return static_cast<uint32_t>(static_cast<const InputEvents *>(l->ctx)->events.size());
    }
    static const clap_event_header_t *CLAP_ABI get(const clap_input_events_t *l, uint32_t index) {
        return static_cast<const InputEvents *>(l->ctx)->events[index];
    }
};

struct OutputEvents {
    std::vector<clap_event_note_t> noteEnds;
    std::vector<uint16_t> paramEventTypes; // gesture begin/end and values coming from the editor
    clap_output_events_t list{this, &tryPush};

    static bool CLAP_ABI tryPush(const clap_output_events_t *l, const clap_event_header_t *ev) {
        auto *self = static_cast<OutputEvents *>(l->ctx);
        if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return true;
        if (ev->type == CLAP_EVENT_NOTE_END)
            self->noteEnds.push_back(*reinterpret_cast<const clap_event_note_t *>(ev));
        else if (ev->type == CLAP_EVENT_PARAM_VALUE || ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ||
                 ev->type == CLAP_EVENT_PARAM_GESTURE_END)
            self->paramEventTypes.push_back(ev->type);
        return true;
    }
};

void pumpMessages(DWORD milliseconds) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

// Hands out at most 5 bytes per call, so the plugin's partial read/write handling gets exercised.
struct MemoryStream {
    std::vector<uint8_t> data;
    size_t readPos = 0;
    clap_ostream_t out{this, &write};
    clap_istream_t in{this, &read};

    static int64_t CLAP_ABI write(const clap_ostream_t *s, const void *buffer, uint64_t size) {
        auto *self = static_cast<MemoryStream *>(s->ctx);
        const auto *bytes = static_cast<const uint8_t *>(buffer);
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, 5));
        self->data.insert(self->data.end(), bytes, bytes + n);
        return static_cast<int64_t>(n);
    }
    static int64_t CLAP_ABI read(const clap_istream_t *s, void *buffer, uint64_t size) {
        auto *self = static_cast<MemoryStream *>(s->ctx);
        const size_t n = static_cast<size_t>(std::min<uint64_t>({size, 5, self->data.size() - self->readPos}));
        std::memcpy(buffer, self->data.data() + self->readPos, n);
        self->readPos += n;
        return static_cast<int64_t>(n);
    }
};

clap_event_note_t noteEvent(uint16_t type, int32_t noteId, int16_t key) {
    clap_event_note_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    ev.note_id = noteId;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = key;
    ev.velocity = 1.0;
    return ev;
}

// MIDI pitch bend, -1..1.
clap_event_midi_t bendEvent(double amount) {
    clap_event_midi_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    const int value = std::clamp(static_cast<int>(std::lround(8192.0 + amount * 8192.0)), 0, 16383);
    ev.data[0] = 0xE0;
    ev.data[1] = static_cast<uint8_t>(value & 0x7F);
    ev.data[2] = static_cast<uint8_t>(value >> 7);
    return ev;
}

clap_event_param_value_t paramEvent(int index, double value) {
    clap_event_param_value_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    ev.param_id = idOfIndex(index);
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    return ev;
}

clap_event_param_mod_t paramModEvent(int index, int32_t noteId, double amount) {
    clap_event_param_mod_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0};
    ev.param_id = idOfIndex(index);
    ev.note_id = noteId;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.amount = amount;
    return ev;
}

bool paramInfosAreSane(const clap_plugin_t *plugin, const clap_plugin_params_t *params) {
    std::set<clap_id> seen;
    for (uint32_t i = 0; i < params->count(plugin); ++i) {
        clap_param_info_t info;
        if (!params->get_info(plugin, i, &info) || info.name[0] == '\0' || !seen.insert(info.id).second ||
            !(info.min_value <= info.default_value && info.default_value <= info.max_value)) {
            std::printf("      bad param #%u (id %u)\n", i, info.id);
            return false;
        }
    }
    return true;
}

// True if `text` is null-terminated within `capacity` bytes and is whole UTF-8:
// hosts such as Bitwig reject a string that ends inside a multi-byte character.
bool isWholeUtf8(const char *text, size_t capacity) {
    const size_t length = strnlen(text, capacity);
    if (length == capacity)
        return false;
    for (size_t i = 0; i < length;) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const size_t size = lead < 0x80              ? 1
                            : (lead >> 5) == 0x6  ? 2
                            : (lead >> 4) == 0xE  ? 3
                            : (lead >> 3) == 0x1E ? 4
                                                  : 0;
        if (size == 0 || i + size > length)
            return false;
        for (size_t k = 1; k < size; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                return false;
        i += size;
    }
    return true;
}

// Names and value texts are whole UTF-8 strings, even when the host's buffer is small.
bool paramTextsAreWholeUtf8(const clap_plugin_t *plugin, const clap_plugin_params_t *params) {
    bool ok = true;
    for (uint32_t i = 0; i < params->count(plugin); ++i) {
        clap_param_info_t info;
        if (!params->get_info(plugin, i, &info))
            return false;
        if (!isWholeUtf8(info.name, sizeof(info.name)) || !isWholeUtf8(info.module, sizeof(info.module))) {
            std::printf("      param #%u: name or module is not whole UTF-8\n", i);
            ok = false;
        }
        // Hosts list every entry of a stepped param (Bitwig does, for its menus).
        std::vector<double> values = {info.min_value, info.default_value, info.max_value};
        if ((info.flags & CLAP_PARAM_IS_STEPPED) && info.max_value - info.min_value <= 64.0)
            for (double v = info.min_value + 1.0; v < info.max_value; v += 1.0)
                values.push_back(v);
        for (size_t capacity = 1; capacity <= 256; capacity = capacity < 48 ? capacity + 1 : capacity * 2) {
            for (double value : values) {
                char text[256];
                std::memset(text, 'x', sizeof(text));
                if (params->value_to_text(plugin, info.id, value, text, static_cast<uint32_t>(capacity)) &&
                    !isWholeUtf8(text, capacity)) {
                    std::printf("      %s at %g, %zu-byte buffer: \"%.*s\" is cut inside a character\n", info.name,
                                value, capacity, static_cast<int>(strnlen(text, capacity)), text);
                    ok = false;
                }
            }
        }
    }
    return ok;
}

bool paramTextRoundTrips(const clap_plugin_t *plugin, const clap_plugin_params_t *params) {
    bool ok = true;
    for (uint32_t i = 0; i < params->count(plugin); ++i) {
        clap_param_info_t info;
        if (!params->get_info(plugin, i, &info))
            return false;
        const double mid = (info.min_value + info.max_value) / 2.0;
        for (double value : {info.min_value, info.default_value, mid, info.max_value}) {
            char text[64] = {}, again[64] = {};
            double parsed = 0.0;
            const bool roundTrip = params->value_to_text(plugin, info.id, value, text, sizeof(text)) &&
                                   params->text_to_value(plugin, info.id, text, &parsed) &&
                                   params->value_to_text(plugin, info.id, parsed, again, sizeof(again)) &&
                                   std::strcmp(text, again) == 0;
            if (!roundTrip) {
                std::printf("      %s: %g -> \"%s\" -> %g -> \"%s\"\n", info.name, value, text, parsed, again);
                ok = false;
            }
        }
    }
    return ok;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fmvibe-smoke <plugin.clap>\n");
        return 2;
    }

    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    const auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(lib, "clap_entry"));
    if (!entry) {
        std::fprintf(stderr, "%s does not export clap_entry\n", argv[1]);
        return 1;
    }
    check(clap_version_is_compatible(entry->clap_version), "entry CLAP version is compatible");
    check(entry->init(argv[1]), "entry init");

    const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || factory->get_plugin_count(factory) != 1) {
        check(false, "factory exposes one plugin");
        return 1;
    }
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("      %s %s (%s)\n", desc->name, desc->version, desc->id);

    const clap_plugin_t *plugin = factory->create_plugin(factory, &kHost, desc->id);
    if (!plugin || !plugin->init(plugin)) {
        check(false, "create and init plugin");
        return 1;
    }
    const auto *params = static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto *state = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    const auto *remote =
        static_cast<const clap_plugin_remote_controls_t *>(plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS));
    check(params && state && remote && plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS) &&
              plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS) && plugin->get_extension(plugin, CLAP_EXT_VOICE_INFO),
          "audio-ports, note-ports, params, state, remote-controls and voice-info extensions");
    if (!params || !state || !remote)
        return 1;

    check(params->count(plugin) == static_cast<uint32_t>(kParamCount) && paramInfosAreSane(plugin, params),
          "param infos: unique ids, names, defaults within range");
    check(paramTextRoundTrips(plugin, params), "param text conversions read back what they write");
    check(paramTextsAreWholeUtf8(plugin, params), "param names and texts are whole UTF-8, even in small buffers");
    {
        bool rangeOk = true;
        for (int index : {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)}) {
            clap_param_info_t info;
            char low[32] = {}, high[32] = {};
            rangeOk = rangeOk && params->get_info(plugin, static_cast<uint32_t>(index), &info) &&
                      params->value_to_text(plugin, info.id, info.min_value, low, sizeof(low)) &&
                      params->value_to_text(plugin, info.id, info.max_value, high, sizeof(high)) &&
                      std::strcmp(low, "C-1") == 0 && std::strcmp(high, "G9") == 0 &&
                      info.default_value == (index == paramIndex(NoteParam::Low) ? info.min_value : info.max_value);
        }
        check(rangeOk, "the note source's Low and High run over every MIDI note, C-1 to G9, and start at its ends");
    }
    // What a new instance starts with: no filter follows the note, a shaper's output is linked to its drive, and a
    // frequency shifter's Shift takes a share of 100 Hz.
    {
        auto infoOf = [&](int index) {
            clap_param_info_t info{};
            params->get_info(plugin, static_cast<uint32_t>(index), &info);
            return info;
        };
        bool defaults = infoOf(paramIndex(0, FxParam::ShaperLink)).default_value == 1.0 &&
                        infoOf(paramIndex(0, FxParam::FilterKeyTrack)).default_value == 0.0;
        for (int f = 0; f < kFilters; ++f)
            defaults = defaults && infoOf(paramIndex(f, FilterParam::KeyTrack)).default_value == 0.0;
        const clap_param_info_t range = infoOf(paramIndex(0, FxParam::ShiftRange));
        char text[32] = {};
        defaults = defaults && params->value_to_text(plugin, range.id, range.default_value, text, sizeof(text)) &&
                   std::strcmp(text, "100 Hz") == 0;
        check(defaults, "filters start without key tracking, a shaper linked and a shifter's range at 100 Hz");
    }
    {
        bool opsOk = true;
        for (int op = 0; op < kOps; ++op) {
            double on = 0.0, out = 0.0;
            params->get_value(plugin, idOfIndex(paramIndex(op, OpParam::Enabled)), &on);
            params->get_value(plugin, idOfIndex(paramIndex(op, OpParam::Out)), &out);
            opsOk = opsOk && on == (op == 0 ? 1.0 : 0.0) && out == (op == 0 ? 1.0 : 0.0);
            for (int to = 0; to < kOps; ++to) {
                double amount = 1.0;
                params->get_value(plugin, idOfIndex(matrixIndex(op, to)), &amount);
                opsOk = opsOk && amount == 0.0;
            }
        }
        double f1On = 1.0;
        params->get_value(plugin, idOfIndex(paramIndex(0, FilterParam::Enabled)), &f1On);
        check(opsOk && f1On == 0.0,
              "a new instance has only operator A switched on and heard, with no modulation");
    }

    bool pagesOk = remote->count(plugin) > 0;
    for (uint32_t i = 0; i < remote->count(plugin); ++i) {
        clap_remote_controls_page_t page;
        pagesOk = pagesOk && remote->get(plugin, i, &page);
        for (clap_id id : page.param_ids)
            pagesOk = pagesOk && (id == CLAP_INVALID_ID || indexOfId(id) >= 0);
    }
    check(pagesOk, "remote control pages point at existing params");

    // --- Audio.
    constexpr double kSampleRate = 48000.0;
    constexpr uint32_t kBlock = 256;
    check(plugin->activate(plugin, kSampleRate, 1, kBlock) && plugin->start_processing(plugin),
          "activate and start processing");

    std::vector<float> left(kBlock), right(kBlock);
    float *channels[] = {left.data(), right.data()};
    clap_audio_buffer_t output{};
    output.data32 = channels;
    output.channel_count = 2;
    InputEvents in;
    OutputEvents out;
    clap_process_t proc{};
    proc.steady_time = -1;
    proc.frames_count = kBlock;
    proc.audio_outputs = &output;
    proc.audio_outputs_count = 1;
    proc.in_events = &in.list;
    proc.out_events = &out.list;

    // Runs one block with the given events (all at time 0); returns its peak level, NaN on garbage output.
    clap_process_status status = CLAP_PROCESS_CONTINUE; // of the latest block
    auto runBlock = [&](std::vector<const clap_event_header_t *> events = {}) {
        in.events = std::move(events);
        status = plugin->process(plugin, &proc);
        if (status == CLAP_PROCESS_ERROR)
            return NAN;
        float peak = 0.0f;
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
                return NAN;
            peak = std::max({peak, std::fabs(left[i]), std::fabs(right[i])});
        }
        return peak;
    };
    auto runBlocks = [&](uint32_t count) {
        float peak = 0.0f;
        for (uint32_t b = 0; b < count; ++b)
            peak = std::max(peak, runBlock());
        return peak;
    };
    const uint32_t blocksPerSecond = static_cast<uint32_t>(kSampleRate / kBlock);
    const auto chokeAll = [] {
        clap_event_note_t ev = noteEvent(CLAP_EVENT_NOTE_CHOKE, -1, -1);
        ev.port_index = -1;
        ev.channel = -1;
        return ev;
    }();

    // Default patch: one held note.
    const clap_event_note_t on1 = noteEvent(CLAP_EVENT_NOTE_ON, 1, 60);
    const clap_event_note_t off1 = noteEvent(CLAP_EVENT_NOTE_OFF, 1, 60);
    float heldPeak = runBlock({&on1.header});
    heldPeak = std::max(heldPeak, runBlocks(blocksPerSecond / 2));
    check(heldPeak > 0.05f && heldPeak < 1.0f, "held note produces sound at a sane level");
    check(out.noteEnds.empty(), "no NOTE_END while the note is held");

    runBlock({&off1.header});
    for (uint32_t b = 0; b < blocksPerSecond && out.noteEnds.empty(); ++b)
        runBlock();
    check(out.noteEnds.size() == 1 && out.noteEnds[0].note_id == 1 && out.noteEnds[0].key == 60,
          "NOTE_END reported after the release");
    check(runBlock() == 0.0f, "silence after the note ended");

    // Voice stealing and choke.
    out.noteEnds.clear();
    std::vector<clap_event_note_t> burst;
    for (int16_t i = 0; i < 20; ++i)
        burst.push_back(noteEvent(CLAP_EVENT_NOTE_ON, 100 + i, static_cast<int16_t>(48 + i)));
    std::vector<const clap_event_header_t *> burstEvents;
    for (const clap_event_note_t &n : burst)
        burstEvents.push_back(&n.header);
    runBlock(burstEvents);
    check(out.noteEnds.size() == 4 && out.noteEnds[0].note_id == 100 && out.noteEnds[3].note_id == 103,
          "20 notes on 16 voices steal the 4 oldest");

    out.noteEnds.clear();
    check(runBlock({&chokeAll.header}) == 0.0f && out.noteEnds.size() == 16,
          "choke with wildcards silences every voice");

    // Per-voice host modulation (what Bitwig's modulators send): pull one note's carrier all the way down.
    const int carrierOut = paramIndex(0, OpParam::Out);
    const clap_event_note_t on7 = noteEvent(CLAP_EVENT_NOTE_ON, 7, 64);
    const clap_event_note_t on8 = noteEvent(CLAP_EVENT_NOTE_ON, 8, 67);
    float note7Peak = runBlock({&on7.header});
    note7Peak = std::max(note7Peak, runBlocks(8));
    const clap_event_param_mod_t duck7 = paramModEvent(carrierOut, 7, -1.0);
    runBlock({&duck7.header});
    check(runBlocks(4) < note7Peak / 100.0f, "per-voice param modulation reaches its voice");
    runBlock({&on8.header});
    check(runBlocks(4) > note7Peak / 2.0f, "a new voice starts without the other voice's modulation");
    runBlock({&chokeAll.header});

    // Mod slots can target any continuous param: an envelope -> Op A Out at -100 %
    // with full sustain silences the only carrier after the attack. Places start
    // empty; a new source starts switched on.
    const int env1 = 4;
    const clap_event_param_value_t envType =
        paramEvent(paramIndex(env1, SourceParam::Type), static_cast<double>(SourceType::Env));
    const clap_event_param_value_t envTarget =
        paramEvent(slotTargetIndex(env1, 0), slotTargetValue(paramIndex(0, OpParam::Out)));
    const clap_event_param_value_t envAmount = paramEvent(slotAmountIndex(env1, 0), -1.0);
    const clap_event_param_value_t envSustain = paramEvent(paramIndex(env1, SourceParam::Sustain), 1.0);
    const clap_event_note_t on9 = noteEvent(CLAP_EVENT_NOTE_ON, 9, 60);
    runBlock({&envType.header, &envTarget.header, &envAmount.header, &envSustain.header, &on9.header});
    runBlocks(2);
    check(runBlocks(8) < 1e-4f, "mod envelope slot drives its target param");
    const clap_event_param_value_t envBypass = paramEvent(slotBypassIndex(env1, 0), 1.0);
    runBlock({&envBypass.header});
    const bool bypassed = runBlocks(4) > 0.05f;
    const clap_event_param_value_t envActive = paramEvent(slotBypassIndex(env1, 0), 0.0);
    runBlock({&envActive.header});
    runBlocks(2);
    check(bypassed && runBlocks(4) < 1e-4f, "a bypassed slot stops modulating, and modulates again once let go");
    // Bipolar, the envelope swings its target about its middle level: at full sustain, -100 % takes A's output
    // down by half rather than all the way.
    const clap_event_param_value_t envBipolar = paramEvent(paramIndex(env1, SourceParam::EnvBipolar), 1.0);
    runBlock({&envBipolar.header});
    runBlocks(2);
    const float bipolarLevel = runBlocks(4);
    const clap_event_param_value_t envUnipolar = paramEvent(paramIndex(env1, SourceParam::EnvBipolar), 0.0);
    const clap_event_param_value_t envAmountOff = paramEvent(slotAmountIndex(env1, 0), 0.0);
    runBlock({&envUnipolar.header, &envAmountOff.header});
    runBlocks(2);
    const float full = runBlocks(4);
    std::printf("      envelope on A's output at -100 %%, full sustain: peak %.3f bipolar, %.3f without it\n",
                bipolarLevel, full);
    check(full > 0.05f, "clearing the slot amount brings the note back");
    check(bipolarLevel > 0.4f * full && bipolarLevel < 0.6f * full,
          "a bipolar envelope swings its target about its middle");
    const clap_event_param_value_t envEmpty = paramEvent(paramIndex(env1, SourceParam::Type), 0.0);
    runBlock({&envEmpty.header, &chokeAll.header});

    // Let go of, a voice runs on while a mod envelope that modulates something releases, after its operators have
    // faded (in 15 ms), so what the envelope moves follows it down: it reports NOTE_END half a second on, once the
    // envelope's release is over, and at once while the envelope modulates nothing.
    {
        auto endsAfter = [&](double amount) { // seconds from the note-off to NOTE_END
            const clap_event_param_value_t settings[] = {
                paramEvent(paramIndex(env1, SourceParam::Type), static_cast<double>(SourceType::Env)),
                paramEvent(slotTargetIndex(env1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount))),
                paramEvent(slotAmountIndex(env1, 0), amount),
                paramEvent(paramIndex(env1, SourceParam::Release), normFromTime(0.5))};
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 11, 62);
            const clap_event_note_t off = noteEvent(CLAP_EVENT_NOTE_OFF, 11, 62);
            runBlock({&settings[0].header, &settings[1].header, &settings[2].header, &settings[3].header, &on.header});
            runBlocks(4);
            out.noteEnds.clear();
            runBlock({&off.header});
            uint32_t blocks = 1;
            for (; out.noteEnds.empty() && blocks < 2 * blocksPerSecond; ++blocks)
                runBlock();
            return blocks * static_cast<double>(kBlock) / kSampleRate;
        };
        const double held = endsAfter(0.3), unheld = endsAfter(0.0);
        const clap_event_param_value_t clear = paramEvent(slotTargetIndex(env1, 0), 0.0);
        runBlock({&clear.header, &envEmpty.header});
        std::printf("      NOTE_END %.3f s after the note-off with a mod envelope releasing over 0.5 s, %.3f s without "
                    "a target\n",
                    held, unheld);
        check(held > 0.4 && held < 0.7 && unheld < 0.1,
              "a released voice runs on while a mod envelope that modulates something releases");
    }

    // An empty place modulates nothing, even with its slots set; once it holds an
    // envelope it does, and switched off it stops again.
    {
        const int place = 7;
        const clap_event_param_value_t target =
            paramEvent(slotTargetIndex(place, 0), slotTargetValue(paramIndex(0, OpParam::Out)));
        const clap_event_param_value_t amount = paramEvent(slotAmountIndex(place, 0), -1.0);
        const clap_event_param_value_t sustain = paramEvent(paramIndex(place, SourceParam::Sustain), 1.0);
        const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 10, 60);
        runBlock({&target.header, &amount.header, &sustain.header, &on.header});
        runBlocks(2);
        const bool ignoredWhileEmpty = runBlocks(4) > 0.05f;
        const clap_event_param_value_t type =
            paramEvent(paramIndex(place, SourceParam::Type), static_cast<double>(SourceType::Env));
        runBlock({&type.header});
        runBlocks(2);
        const bool heardOnceFilled = runBlocks(8) < 1e-4f;
        const clap_event_param_value_t switchOff = paramEvent(sourceEnabledIndex(place), 0.0);
        runBlock({&switchOff.header});
        runBlocks(2);
        check(ignoredWhileEmpty && heardOnceFilled && runBlocks(4) > 0.05f,
              "only a source that holds a type and is switched on modulates");
        const clap_event_param_value_t clear = paramEvent(slotAmountIndex(place, 0), 0.0);
        const clap_event_param_value_t switchOn = paramEvent(sourceEnabledIndex(place), 1.0);
        const clap_event_param_value_t empty = paramEvent(paramIndex(place, SourceParam::Type), 0.0);
        runBlock({&clear.header, &switchOn.header, &empty.header, &chokeAll.header});
    }

    // Spectrum checks on an A4 (440 Hz). A patch is described by the params that
    // matter here; everything is set before the note so earlier checks don't leak in.
    struct Patch {
        double bToA = 0.0;
        OpWave wave = OpWave::Sine;
        bool throughFilter = false;
        FilterType type = FilterType::LowPass;
        int slope = kDefaultSlope; // 6, 12, 18 or 24 dB per octave
        double cutoffHz = 1000.0;
        double resonance = 0.0;
        double f1ToA = 0.0; // B goes into filter 1 (which isn't heard), and filter 1 modulates A
        int filter = 0;     // the filter A goes through
        bool intoLast = false; // that filter is heard only through the last one, filter 2, a 300 Hz low-pass
    };
    // Plays the patch for ~0.1 s; returns the left channel after a short settle.
    auto play = [&](const Patch &patch) {
        const bool f1Mods = patch.f1ToA > 0.0;
        const int last = kFilters - 1;
        std::vector<clap_event_param_value_t> settings = {
            paramEvent(paramIndex(1, OpParam::Enabled), 1.0),
            paramEvent(sendIndex(1, 0), f1Mods ? 1.0 : 0.0),
            paramEvent(filterModIndex(0, 0), patch.f1ToA),
            paramEvent(matrixIndex(1, 0), patch.bToA),
            paramEvent(paramIndex(0, OpParam::Wave), static_cast<double>(patch.wave)),
            paramEvent(paramIndex(0, OpParam::Out), patch.throughFilter ? 0.0 : 1.0),
            paramEvent(paramIndex(patch.filter, FilterParam::Type), static_cast<double>(patch.type)),
            paramEvent(paramIndex(patch.filter, FilterParam::Slope), patch.slope),
            paramEvent(paramIndex(patch.filter, FilterParam::Cutoff), normFromCutoff(patch.cutoffHz)),
            paramEvent(paramIndex(patch.filter, FilterParam::Resonance), patch.resonance),
            paramEvent(paramIndex(patch.filter, FilterParam::KeyTrack), 0.0),
            paramEvent(filterLinkIndex(std::min(patch.filter, last - 1), last), patch.intoLast ? 1.0 : 0.0),
        };
        if (patch.intoLast)
            settings.insert(settings.end(),
                            {paramEvent(paramIndex(last, FilterParam::Type), static_cast<double>(FilterType::LowPass)),
                             paramEvent(paramIndex(last, FilterParam::Slope), kDefaultSlope),
                             paramEvent(paramIndex(last, FilterParam::Cutoff), normFromCutoff(300.0)),
                             paramEvent(paramIndex(last, FilterParam::Resonance), 0.0),
                             paramEvent(paramIndex(last, FilterParam::KeyTrack), 0.0)});
        for (int f = 0; f < kFilters; ++f) {
            const bool used = patch.throughFilter && f == patch.filter;
            settings.push_back(paramEvent(paramIndex(f, FilterParam::Enabled),
                                          used || (f == 0 && f1Mods) || (f == last && patch.intoLast) ? 1.0 : 0.0));
            settings.push_back(paramEvent(sendIndex(0, f), used ? 1.0 : 0.0));
            settings.push_back(paramEvent(paramIndex(f, FilterParam::Out),
                                          (f == 0 && f1Mods) || (used && patch.intoLast) ? 0.0 : 1.0));
        }
        const clap_event_note_t a4 = noteEvent(CLAP_EVENT_NOTE_ON, 20, 69);
        std::vector<const clap_event_header_t *> events;
        for (const auto &s : settings)
            events.push_back(&s.header);
        events.push_back(&a4.header);
        runBlock(events);
        runBlocks(4);
        std::vector<float> signal;
        for (int b = 0; b < 16; ++b) {
            if (!std::isfinite(runBlock()))
                return std::vector<float>{};
            signal.insert(signal.end(), left.begin(), left.end());
        }
        runBlock({&chokeAll.header});
        return signal;
    };
    auto magnitude = [&](const std::vector<float> &signal, double hz) { // Hann-windowed DFT at one frequency
        double re = 0.0, im = 0.0;
        const double n = static_cast<double>(signal.size());
        for (size_t i = 0; i < signal.size(); ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / n);
            const double phase = 2.0 * 3.141592653589793 * hz * i / kSampleRate;
            re += signal[i] * w * std::cos(phase);
            im -= signal[i] * w * std::sin(phase);
        }
        return std::hypot(re, im);
    };
    auto secondHarmonicRatio = [&](const Patch &patch) {
        const std::vector<float> signal = play(patch);
        return signal.empty() ? NAN : magnitude(signal, 880.0) / magnitude(signal, 440.0);
    };
    auto peakOf = [](const std::vector<float> &signal) {
        float peak = signal.empty() ? NAN : 0.0f;
        for (float s : signal)
            peak = std::max(peak, std::fabs(s));
        return peak;
    };

    // The matrix does FM: B -> A puts the 2nd harmonic onto a pure-sine carrier.
    const double pure = secondHarmonicRatio({});
    const double fm = secondHarmonicRatio({.bToA = 0.4});
    std::printf("      880/440 Hz magnitude: %.4f without B -> A, %.3f with B -> A at 40 %%\n", pure, fm);
    check(pure < 0.01 && fm > 0.1, "matrix routing adds FM sidebands");

    // A filter's output modulates an operator the same way: B through an open
    // low-pass FMs A about as much as B directly; a low cutoff takes most of it away.
    const double filteredFm = secondHarmonicRatio({.cutoffHz = 8000.0, .f1ToA = 0.4});
    const double darkFm = secondHarmonicRatio({.cutoffHz = 60.0, .f1ToA = 0.4});
    std::printf("      880/440 Hz magnitude with F1 -> A at 40 %%: %.3f through an 8 kHz low-pass, %.3f through 60 Hz\n",
                filteredFm, darkFm);
    check(filteredFm > 0.5 * fm && darkFm < 0.3 * filteredFm, "a filter's output modulates an operator");

    // Filter 2: a saw through filter 2 alone, then through filter 1 on into filter 2 (a 300 Hz low-pass).
    check(kFilters == 2 && kShapers == 2, "there are two filters and two shapers");
    const double throughF2 = secondHarmonicRatio({.wave = OpWave::Saw, .throughFilter = true, .cutoffHz = 8000.0,
                                                  .filter = 1});
    const double intoF2 = secondHarmonicRatio({.wave = OpWave::Saw, .throughFilter = true, .cutoffHz = 8000.0,
                                               .filter = 0, .intoLast = true});
    std::printf("      saw 880/440 Hz: %.2f through F2 at 8 kHz, %.2f with F1 into F2 at 300 Hz\n", throughF2, intoF2);
    check(throughF2 > 0.4 && intoF2 < throughF2 / 3.0, "filter 2 works, and filter 1 feeds it");

    // Operator waves.
    bool wavesSound = true;
    for (int w = 0; w < static_cast<int>(OpWave::Count); ++w) {
        const float peak = peakOf(play({.wave = static_cast<OpWave>(w)}));
        if (!(peak > 0.05f && peak < 1.0f)) {
            std::printf("      wave %d: peak %g\n", w, peak);
            wavesSound = false;
        }
    }
    check(wavesSound, "every operator wave sounds at a sane level");
    const double saw = secondHarmonicRatio({.wave = OpWave::Saw});
    check(saw > 0.4 && saw < 0.6, "a saw has its 2nd harmonic at about half the fundamental");

    // Filters, with A's saw routed through filter 1.
    const double lowPassed = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::LowPass, .cutoffHz = 300.0});
    const double highPassed = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::HighPass, .cutoffHz = 2000.0});
    std::printf("      saw 880/440 Hz: %.2f dry, %.2f low-passed at 300 Hz, %.2f high-passed at 2 kHz\n", saw,
                lowPassed, highPassed);
    check(lowPassed < saw / 2.0 && highPassed > saw * 2.0, "low-pass and high-pass reshape the spectrum");
    // Each slope from 6 to 24 dB per octave cuts the same filters harder than the one before: both harmonics lie past
    // the low-pass's cutoff and before the high-pass's, so the 2nd falls (or rises) against the 1st by another
    // pole's worth each time.
    std::array<double, kFilterSlopes> lowBySlope{}, highBySlope{};
    for (int slope = 0; slope < kFilterSlopes; ++slope) {
        lowBySlope[slope] = secondHarmonicRatio(
            {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::LowPass, .slope = slope, .cutoffHz = 300.0});
        highBySlope[slope] = secondHarmonicRatio(
            {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::HighPass, .slope = slope, .cutoffHz = 2000.0});
    }
    std::printf("      6 to 24 dB: low-passed %.3f %.3f %.3f %.3f, high-passed %.2f %.2f %.2f %.2f\n", lowBySlope[0],
                lowBySlope[1], lowBySlope[2], lowBySlope[3], highBySlope[0], highBySlope[1], highBySlope[2], highBySlope[3]);
    bool steeper = true;
    for (int slope = 1; slope < kFilterSlopes; ++slope)
        steeper = steeper && lowBySlope[slope] < lowBySlope[slope - 1] / 1.4 && highBySlope[slope] > highBySlope[slope - 1] * 1.4;
    check(steeper, "each slope from 6 to 24 dB per octave cuts harder than the one before");
    // A 6 dB low-pass is a plain one-pole without resonance: an octave and a half past its cutoff, its 2nd harmonic
    // comes out at the one-pole's level against the 1st.
    const double onePole = 0.5 * std::sqrt((1.0 + std::pow(440.0 / 300.0, 2.0)) / (1.0 + std::pow(880.0 / 300.0, 2.0)));
    check(std::abs(lowBySlope[0] / onePole - 1.0) < 0.1, "the 6 dB low-pass is a one-pole");
    // A band-pass or notch comes in 12 and 24 dB only: set to 6 or 18, it plays the slope above.
    const double band6 = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::BandPass, .slope = 0, .cutoffHz = 600.0});
    const double band12 = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::BandPass, .slope = 1, .cutoffHz = 600.0});
    const double band18 = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::BandPass, .slope = 2, .cutoffHz = 600.0});
    const double band24 = secondHarmonicRatio(
        {.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::BandPass, .slope = 3, .cutoffHz = 600.0});
    check(std::abs(band6 - band12) < 1e-6 * band12 && std::abs(band18 - band24) < 1e-6 * band24 &&
              std::abs(band12 - band24) > 0.01 * band12,
          "a band-pass set to 6 or 18 dB plays 12 or 24");
    bool filtersSound = true;
    for (int t = 0; t < static_cast<int>(FilterType::Count); ++t) {
        for (int slope = 0; slope < kFilterSlopes; ++slope) {
            const float peak = peakOf(play({.wave = OpWave::Saw,
                                            .throughFilter = true,
                                            .type = static_cast<FilterType>(t),
                                            .slope = slope,
                                            .cutoffHz = 1200.0,
                                            .resonance = 0.9}));
            if (!(peak > 0.005f && peak < 4.0f)) {
                std::printf("      filter type %d at %d dB: peak %g\n", t, slopeDb(slope), peak);
                filtersSound = false;
            }
        }
    }
    check(filtersSound, "every filter type passes sound and stays finite at high resonance, at every slope");

    // Switched off, an operator is silent, and a filter passes nothing sent into it.
    {
        const clap_event_param_value_t aOff = paramEvent(paramIndex(0, OpParam::Enabled), 0.0);
        const clap_event_param_value_t aOn = paramEvent(paramIndex(0, OpParam::Enabled), 1.0);
        const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 45, 60);
        runBlock({&aOff.header, &on.header});
        const float operatorOff = runBlocks(4);
        runBlock({&chokeAll.header, &aOn.header});
        check(operatorOff == 0.0f, "a switched-off operator is silent");

        // A only through filter 1, with the filter switched off.
        const clap_event_param_value_t route[] = {
            paramEvent(paramIndex(0, OpParam::Out), 0.0),
            paramEvent(sendIndex(0, 0), 1.0),
            paramEvent(paramIndex(0, FilterParam::Enabled), 0.0),
        };
        const clap_event_note_t on2 = noteEvent(CLAP_EVENT_NOTE_ON, 46, 60);
        runBlock({&route[0].header, &route[1].header, &route[2].header, &on2.header});
        const float filterOff = runBlocks(4);
        runBlock({&chokeAll.header});
        check(filterOff == 0.0f, "a switched-off filter passes nothing");
        const clap_event_param_value_t restore[] = {paramEvent(paramIndex(0, OpParam::Out), 1.0),
                                                    paramEvent(sendIndex(0, 0), 0.0)};
        runBlock({&restore[0].header, &restore[1].header});
    }

    // Shapers. B's sine through shaper 1 onto A modulates A, as B itself would; switched off, the shaper passes
    // nothing on. A's sine through shaper 1, a hard clip, into filter 1 (open) comes out squared off, with odd
    // harmonics, where a soft curve without drive leaves a quieter one a sine. A shaper routed nowhere isn't heard;
    // with its To Out up, it goes straight out, as an operator does.
    {
        auto playShaped = [&](const std::vector<std::pair<int, double>> &changes) {
            std::vector<clap_event_param_value_t> evs;
            for (const auto &[index, value] : changes)
                evs.push_back(paramEvent(index, value));
            const clap_event_note_t a4 = noteEvent(CLAP_EVENT_NOTE_ON, 21, 69);
            std::vector<const clap_event_header_t *> events;
            for (const clap_event_param_value_t &ev : evs)
                events.push_back(&ev.header);
            events.push_back(&a4.header);
            runBlock(events);
            runBlocks(4);
            std::vector<float> signal;
            for (int b = 0; b < 16; ++b) {
                runBlock();
                signal.insert(signal.end(), left.begin(), left.end());
            }
            runBlock({&chokeAll.header});
            return signal;
        };
        const int shaperOn = paramIndex(0, ShaperParam::Enabled);
        const std::vector<float> modulated = playShaped({{paramIndex(0, OpParam::Wave), 0.0}, // A a sine again
                                                         {paramIndex(1, OpParam::Enabled), 1.0},
                                                         {paramIndex(1, OpParam::Out), 0.0},
                                                         {matrixIndex(1, 0), 0.0},
                                                         {shaperOn, 1.0},
                                                         {shaperSendIndex(1, 0), 1.0},
                                                         {shaperModIndex(0, 0), 0.4}});
        const std::vector<float> unmodulated = playShaped({{shaperOn, 0.0}});
        const double shapedFm = magnitude(modulated, 880.0) / magnitude(modulated, 440.0);
        const double offFm = magnitude(unmodulated, 880.0) / magnitude(unmodulated, 440.0);
        const auto mode = static_cast<double>(ClipMode::Hard);
        const std::vector<float> clipped = playShaped({{shaperOn, 1.0},
                                                       {shaperSendIndex(1, 0), 0.0},
                                                       {shaperModIndex(0, 0), 0.0},
                                                       {paramIndex(0, OpParam::Out), 0.0},
                                                       {shaperSendIndex(0, 0), 1.0},
                                                       {shaperFilterIndex(0, 0), 1.0},
                                                       {paramIndex(0, FilterParam::Enabled), 1.0},
                                                       {paramIndex(0, FilterParam::Type), 0.0},
                                                       {paramIndex(0, FilterParam::Cutoff), 1.0},
                                                       {paramIndex(0, FilterParam::Resonance), 0.0},
                                                       {paramIndex(0, ShaperParam::Mode), mode},
                                                       {paramIndex(0, ShaperParam::Drive), 24.0},
                                                       {paramIndex(0, ShaperParam::Link), 0.0},
                                                       {paramIndex(0, ShaperParam::Output), -6.0}});
        const std::vector<float> clean = playShaped({{shaperSendIndex(0, 0), 0.25}, // where tanh is still nearly straight
                                                     {paramIndex(0, ShaperParam::Mode), static_cast<double>(ClipMode::Soft)},
                                                     {paramIndex(0, ShaperParam::Drive), 0.0},
                                                     {paramIndex(0, ShaperParam::Output), 0.0}});
        const float alone = peakOf(playShaped({{shaperFilterIndex(0, 0), 0.0}}));
        const float direct = peakOf(playShaped({{paramIndex(0, ShaperParam::Out), 1.0}}));
        const double clippedThird = magnitude(clipped, 1320.0) / magnitude(clipped, 440.0);
        const double cleanThird = magnitude(clean, 1320.0) / magnitude(clean, 440.0);
        std::printf("      A by B through shaper 1, 880/440 Hz: %.3f (%.4f off); A through a hard shaper into F1, "
                    "1320/440 Hz: %.3f (%.4f soft, undriven); a shaper alone: peak %.4f, straight out %.4f\n",
                    shapedFm, offFm, clippedThird, cleanThird, alone, direct);
        check(shapedFm > 0.1 && offFm < 0.01, "a shaper's output modulates an operator, and one that is off doesn't");
        check(clippedThird > 0.15 && cleanThird < 0.05, "a shaper clips what goes through it into a filter");
        check(alone == 0.0f && direct > 0.05f,
              "a shaper routed nowhere isn't heard, and goes straight out by its To Out");
        const clap_event_param_value_t restore[] = {
            paramEvent(paramIndex(0, OpParam::Out), 1.0),        paramEvent(shaperSendIndex(0, 0), 0.0),
            paramEvent(shaperOn, 0.0),                           paramEvent(paramIndex(0, FilterParam::Enabled), 0.0),
            paramEvent(paramIndex(0, ShaperParam::Link), 1.0),   paramEvent(paramIndex(0, ShaperParam::Drive), 6.0),
            paramEvent(paramIndex(0, ShaperParam::Out), 0.0)};
        std::vector<const clap_event_header_t *> events;
        for (const clap_event_param_value_t &ev : restore)
            events.push_back(&ev.header);
        runBlock(events);
    }

    // A voice ringing through a resonant comb still ends after its release.
    {
        play({.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::Comb, .resonance = 1.0});
        const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 30, 57);
        const clap_event_note_t off = noteEvent(CLAP_EVENT_NOTE_OFF, 30, 57);
        out.noteEnds.clear();
        runBlock({&on.header});
        runBlocks(8);
        runBlock({&off.header});
        for (uint32_t b = 0; b < 2 * blocksPerSecond && out.noteEnds.empty(); ++b)
            runBlock();
        check(out.noteEnds.size() == 1 && runBlock() == 0.0f, "filter tails end and report NOTE_END");
        // Back to the default routing for the CPU check.
        const clap_event_param_value_t outOn = paramEvent(paramIndex(0, OpParam::Out), 1.0);
        const clap_event_param_value_t sendOff = paramEvent(sendIndex(0, 0), 0.0);
        const clap_event_param_value_t sine = paramEvent(paramIndex(0, OpParam::Wave), 0.0);
        runBlock({&outOn.header, &sendOff.header, &sine.header});
    }

    // Unison: four copies spread across the stereo field; without spread both sides match.
    auto unisonStereo = [&](double spread) {
        const clap_event_param_value_t settings[] = {
            paramEvent(paramIndex(0, OpParam::Unison), 4.0),
            paramEvent(paramIndex(0, OpParam::UnisonDetune), 30.0),
            paramEvent(paramIndex(0, OpParam::UnisonSpread), spread),
        };
        const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 40, 60);
        runBlock({&settings[0].header, &settings[1].header, &settings[2].header, &on.header});
        float peak = 0.0f, difference = 0.0f;
        for (int b = 0; b < 8; ++b) {
            peak = std::max(peak, runBlock());
            for (uint32_t i = 0; i < kBlock; ++i)
                difference = std::max(difference, std::fabs(left[i] - right[i]));
        }
        runBlock({&chokeAll.header});
        return std::pair(peak, difference);
    };
    const auto [widePeak, wide] = unisonStereo(1.0);
    const auto [centeredPeak, centered] = unisonStereo(0.0);
    std::printf("      unison x4: peak %.3f, L/R difference %.3f spread, %.1g centered\n", widePeak, wide, centered);
    check(widePeak > 0.05f && widePeak < 1.0f && centeredPeak > 0.05f && wide > 0.01f && centered < 1e-6f,
          "unison copies spread across the stereo field");
    const clap_event_param_value_t unisonOff = paramEvent(paramIndex(0, OpParam::Unison), 1.0);
    runBlock({&unisonOff.header});

    // Phase randomization: at 0 % every note starts the waveform at the same
    // point, so two notes begin identically; at 100 % they don't.
    auto firstBlocksMatch = [&](double amount) {
        const clap_event_param_value_t phase = paramEvent(paramIndex(0, OpParam::PhaseRandom), amount);
        runBlock({&phase.header});
        std::vector<float> takes[2];
        for (std::vector<float> &take : takes) {
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 45, 69);
            runBlock({&on.header});
            take = left;
            runBlock({&chokeAll.header});
        }
        return takes[0] == takes[1];
    };
    const bool samePhase = firstBlocksMatch(0.0);
    const bool randomPhase = !firstBlocksMatch(1.0);
    check(samePhase && randomPhase, "phase randomization: notes start alike at 0 %, differently at 100 %");
    const clap_event_param_value_t phaseOff = paramEvent(paramIndex(0, OpParam::PhaseRandom), 0.0);
    runBlock({&phaseOff.header});

    // Sets params in one block, before the next check.
    auto setParams = [&](std::initializer_list<std::pair<int, double>> values) {
        std::vector<clap_event_param_value_t> evs;
        for (const auto &[index, value] : values)
            evs.push_back(paramEvent(index, value));
        std::vector<const clap_event_header_t *> events;
        for (const clap_event_param_value_t &ev : evs)
            events.push_back(&ev.header);
        runBlock(events);
    };

    // Noise slope on A: white noise jumps from sample to sample, pink much less and
    // brown hardly at all, all about as loud. Spread decorrelates left and right.
    {
        struct Measure {
            double rms, roughness, correlation;
        };
        auto measure = [&](double slope, double spread) {
            setParams({{paramIndex(0, OpParam::Wave), static_cast<double>(OpWave::Noise)},
                       {paramIndex(0, OpParam::NoiseSlope), slope},
                       {paramIndex(0, OpParam::UnisonSpread), spread}});
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 70, 60);
            runBlock({&on.header});
            runBlocks(blocksPerSecond / 10); // the color filters settle
            double ll = 0.0, rr = 0.0, lr = 0.0, steps = 0.0;
            float previous = left[kBlock - 1];
            for (uint32_t b = 0; b < blocksPerSecond; ++b) {
                runBlock();
                for (uint32_t i = 0; i < kBlock; ++i) {
                    ll += left[i] * left[i];
                    rr += right[i] * right[i];
                    lr += left[i] * right[i];
                    steps += (left[i] - previous) * (left[i] - previous);
                    previous = left[i];
                }
            }
            runBlock({&chokeAll.header});
            return Measure{std::sqrt(ll / (blocksPerSecond * kBlock)), steps / ll, lr / std::sqrt(ll * rr)};
        };
        const Measure white = measure(0.0, 0.0), pink = measure(0.5, 0.0), brown = measure(1.0, 0.0);
        const Measure spread = measure(0.5, 1.0);
        std::printf("      noise roughness white %.3f, pink %.3f, brown %.4f; rms %.3f / %.3f / %.3f\n",
                    white.roughness, pink.roughness, brown.roughness, white.rms, pink.rms, brown.rms);
        std::printf("      left/right correlation %.3f narrow, %.3f at full spread\n", pink.correlation,
                    spread.correlation);
        auto similar = [&](double rms) { return rms > white.rms * 0.7 && rms < white.rms * 1.41; };
        check(white.roughness > 1.5 && pink.roughness > 0.2 && pink.roughness < 1.2 && brown.roughness < 0.05,
              "the noise slope moves from white through pink to brown");
        check(similar(pink.rms) && similar(brown.rms), "noise keeps its loudness across slopes");
        check(pink.correlation > 0.999 && std::abs(spread.correlation) < 0.1,
              "noise spread decorrelates left and right");
        setParams({{paramIndex(0, OpParam::Wave), 0.0},
                   {paramIndex(0, OpParam::NoiseSlope), 0.0},
                   {paramIndex(0, OpParam::UnisonSpread), 0.5}});
    }

    // LFO sync: a square LFO 1 at -100 % on A's output, set to 0.5, swings it half the depth either way: muted
    // in the first half of each cycle, full in the second. At 1/4 a cycle takes 0.5 s at 120 bpm and 1 s at
    // 60 bpm, so 0.35 s into the note it is sounding at 120 bpm and still muted at 60.
    {
        clap_event_transport_t transport{};
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
        proc.transport = &transport;
        setParams({{paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo)},
                   {paramIndex(0, SourceParam::Shape), static_cast<double>(LfoShape::Square)},
                   {paramIndex(0, SourceParam::Sync), 1.0},
                   {paramIndex(0, SourceParam::Division), 13.0}, // 1/4
                   {paramIndex(0, OpParam::Out), 0.5},
                   {slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(0, 0), -1.0}});
        auto levelAt = [&](double bpm, double seconds) {
            transport.tempo = bpm;
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 60, 69);
            runBlock({&on.header});
            runBlocks(static_cast<uint32_t>(seconds * kSampleRate / kBlock) - 1);
            const float peak = runBlock();
            runBlock({&chokeAll.header});
            return peak;
        };
        const float early = levelAt(120.0, 0.1), fast = levelAt(120.0, 0.35), slow = levelAt(60.0, 0.35);
        std::printf("      synced square LFO at 0.35 s: %.3f at 120 bpm, %.3f at 60 bpm\n", fast, slow);
        check(early == 0.0f && fast > 0.1f && slow == 0.0f, "a synced LFO follows the host tempo");
        proc.transport = nullptr;
        setParams({{paramIndex(0, SourceParam::Sync), 0.0}});

        // A freeform shape drawn as the same square, at the free 2 Hz rate: up for
        // the first half of the cycle (muted), down for the second.
        setParams({{paramIndex(0, SourceParam::Shape), static_cast<double>(LfoShape::Custom)},
                   {paramIndex(0, SourceParam::Rate), normFromRate(2.0)},
                   {shapeCountIndex(0), 4.0},
                   {shapePointIndex(0, 0, PointField::Y), 1.0},
                   {shapePointIndex(0, 1, PointField::X), 0.499},
                   {shapePointIndex(0, 1, PointField::Y), 1.0},
                   {shapePointIndex(0, 2, PointField::X), 0.5},
                   {shapePointIndex(0, 2, PointField::Y), -1.0},
                   {shapePointIndex(0, 3, PointField::X), 0.999},
                   {shapePointIndex(0, 3, PointField::Y), -1.0}});
        const float drawnEarly = levelAt(120.0, 0.1), drawnLate = levelAt(120.0, 0.35);
        std::printf("      drawn square LFO: %.3f at 0.1 s, %.3f at 0.35 s\n", drawnEarly, drawnLate);
        check(drawnEarly == 0.0f && drawnLate > 0.1f, "a freeform LFO follows its points");

        // Free: notes don't restart the LFO. After a reset, a note's 0.1 s falls in
        // the muted first half of the shared clock; a later note started so that its
        // 0.1 s lands at 0.35 s on the clock hears the second half. Retriggered, both
        // are muted.
        auto twoNotes = [&](double free) {
            setParams({{paramIndex(0, SourceParam::Free), free}});
            plugin->reset(plugin); // the clock starts over
            const float first = levelAt(120.0, 0.1); // 20 blocks, measured in the 19th
            runBlocks(static_cast<uint32_t>(0.35 * kSampleRate / kBlock) - 20 - 18);
            return std::pair(first, levelAt(120.0, 0.1));
        };
        const auto [freeFirst, freeLater] = twoNotes(1.0);
        const auto [retrigFirst, retrigLater] = twoNotes(0.0);
        std::printf("      free square LFO 0.1 s into each note: %.3f then %.3f (retriggered %.3f then %.3f)\n",
                    freeFirst, freeLater, retrigFirst, retrigLater);
        check(freeFirst == 0.0f && freeLater > 0.1f && retrigFirst == 0.0f && retrigLater == 0.0f,
              "a free LFO keeps running across notes; a retriggered one restarts");

        // One-shot: the drawn square runs once and then stays at its end, where the
        // cycle started (muted); looping, 0.85 s in is back in the sounding half.
        const float looping = levelAt(120.0, 0.85);
        setParams({{paramIndex(0, SourceParam::OneShot), 1.0}});
        const float oneShot = levelAt(120.0, 0.85), oneShotEarly = levelAt(120.0, 0.35);
        std::printf("      drawn square at 0.85 s: %.3f looping, %.3f one-shot (%.3f at 0.35 s)\n", looping, oneShot,
                    oneShotEarly);
        check(looping > 0.1f && oneShot == 0.0f && oneShotEarly > 0.1f, "a one-shot LFO runs one cycle, then stays");
        setParams({{paramIndex(0, SourceParam::OneShot), 0.0}});

        // Unipolar: in the low half the square is 0 instead of -1, so it leaves A at 0.5 where the bipolar one
        // pushes it up half the depth, to 1; in the high half it takes the whole depth off.
        const float bipolarLow = levelAt(120.0, 0.35);
        setParams({{paramIndex(0, SourceParam::Unipolar), 1.0}});
        const float unipolarLow = levelAt(120.0, 0.35), unipolarHigh = levelAt(120.0, 0.1);
        std::printf("      square on A's output in its low half: %.3f bipolar, %.3f unipolar\n", bipolarLow,
                    unipolarLow);
        check(std::abs(unipolarLow / bipolarLow - 0.5f) < 0.02f && unipolarHigh == 0.0f,
              "a unipolar LFO swings from 0 instead of both ways");
        setParams({{paramIndex(0, SourceParam::Shape), 0.0}, {paramIndex(0, SourceParam::Type), 0.0},
                   {paramIndex(0, SourceParam::Unipolar), 0.0}, {paramIndex(0, OpParam::Out), 1.0},
                   {slotTargetIndex(0, 0), 0.0}, {slotAmountIndex(0, 0), 0.0}});
    }

    // Hold: with a short decay to silence, A still sounds 0.2 s in while a 0.3 s hold lasts.
    {
        auto levelAt = [&](double hold, double seconds) {
            setParams({{paramIndex(0, OpParam::Hold), hold}});
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 71, 69);
            runBlock({&on.header});
            runBlocks(static_cast<uint32_t>(seconds * kSampleRate / kBlock) - 1);
            const float peak = runBlock();
            runBlock({&chokeAll.header});
            return peak;
        };
        setParams({{paramIndex(0, OpParam::Decay), normFromTime(0.01)}, {paramIndex(0, OpParam::Sustain), 0.0}});
        const float held = levelAt(normFromTime(0.3), 0.2), after = levelAt(normFromTime(0.3), 0.4),
                    none = levelAt(0.0, 0.2);
        std::printf("      0.2 s into a 0.3 s hold: %.3f (%.4f at 0.4 s, %.4f without the hold)\n", held, after, none);
        check(held > 0.2f && after < 1e-3f && none < 1e-3f, "the hold keeps the envelope at full level");
        setParams({{paramIndex(0, OpParam::Hold), 0.0},
                   {paramIndex(0, OpParam::Decay), normFromTime(1.5)},
                   {paramIndex(0, OpParam::Sustain), 1.0}});
    }

    // Pitch: an envelope at full sustain, at +12.5 % of A's ±48 st Pitch range, moves the A4 up an octave.
    {
        const int place = 5;
        setParams({{paramIndex(place, SourceParam::Type), static_cast<double>(SourceType::Env)},
                   {paramIndex(place, SourceParam::Sustain), 1.0},
                   {slotTargetIndex(place, 0), slotTargetValue(paramIndex(0, OpParam::Transpose))},
                   {slotAmountIndex(place, 0), 0.125}});
        const std::vector<float> lifted = play({});
        setParams({{paramIndex(place, SourceParam::Type), 0.0}, {slotTargetIndex(place, 0), 0.0},
                   {slotAmountIndex(place, 0), 0.0}});
        const std::vector<float> plain = play({});
        std::printf("      enveloped pitch: 880/440 Hz %.2f, plain %.3f\n", magnitude(lifted, 880.0) / magnitude(lifted, 440.0),
                    magnitude(plain, 880.0) / magnitude(plain, 440.0));
        check(magnitude(lifted, 880.0) > 10.0 * magnitude(lifted, 440.0) &&
                  magnitude(plain, 440.0) > 10.0 * magnitude(plain, 880.0),
              "an envelope on an operator's pitch transposes it");
    }

    // Envelope curves on A: halfway through a 0.4 s segment, a curve of 1 has nearly
    // arrived, -1 has hardly left, and a straight segment is about halfway there.
    {
        const double segment = normFromTime(0.4);
        auto halfway = [&](int curveParam, double curve, bool release) {
            setParams({{curveParam, curve}});
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 70, 69);
            const clap_event_note_t off = noteEvent(CLAP_EVENT_NOTE_OFF, 70, 69);
            runBlock({&on.header});
            if (release) {
                runBlocks(blocksPerSecond / 10);
                runBlock({&off.header});
            }
            runBlocks(static_cast<uint32_t>(0.2 * kSampleRate / kBlock) - 1);
            const float peak = runBlock();
            runBlock({&chokeAll.header});
            return peak;
        };
        auto shapes = [&](int curveParam, bool release) {
            return std::array<float, 3>{halfway(curveParam, -1.0, release), halfway(curveParam, 0.0, release),
                                        halfway(curveParam, 1.0, release)};
        };
        setParams({{paramIndex(0, OpParam::Attack), segment}, {paramIndex(0, OpParam::Sustain), 1.0}});
        const auto attack = shapes(paramIndex(0, OpParam::AttackCurve), false);
        setParams({{paramIndex(0, OpParam::Attack), 0.0},
                   {paramIndex(0, OpParam::AttackCurve), 0.0},
                   {paramIndex(0, OpParam::Decay), segment},
                   {paramIndex(0, OpParam::Sustain), 0.0}});
        const auto decay = shapes(paramIndex(0, OpParam::DecayCurve), false);
        setParams({{paramIndex(0, OpParam::DecayCurve), kExponentialCurve},
                   {paramIndex(0, OpParam::Sustain), 1.0},
                   {paramIndex(0, OpParam::Release), segment}});
        const auto release = shapes(paramIndex(0, OpParam::ReleaseCurve), true);
        std::printf("      halfway at curve -1 / 0 / 1: attack %.3f %.3f %.3f, decay %.3f %.3f %.3f, release %.3f %.3f "
                    "%.3f\n",
                    attack[0], attack[1], attack[2], decay[0], decay[1], decay[2], release[0], release[1], release[2]);
        check(attack[2] > 1.6f * attack[1] && attack[0] < 0.1f * attack[1], "an attack curve bends the rise");
        check(decay[0] > 1.6f * decay[1] && decay[2] < 0.1f * decay[1], "a decay curve bends the fall");
        check(release[0] > 1.6f * release[1] && release[2] < 0.1f * release[1], "a release curve bends the release");
        setParams({{paramIndex(0, OpParam::Decay), normFromTime(1.5)},
                   {paramIndex(0, OpParam::Release), normFromTime(0.015)},
                   {paramIndex(0, OpParam::ReleaseCurve), kExponentialCurve}});
    }

    // A Custom wave plays the partials it is drawn from: the second alone sounds an octave up.
    setParams({{partialLevelIndex(0, 0), 0.0}, {partialLevelIndex(0, 1), 1.0}});
    const double octave = secondHarmonicRatio({.wave = OpWave::Custom});
    setParams({{partialLevelIndex(0, 0), 1.0}, {partialLevelIndex(0, 1), 0.0}});
    const double fundamental = secondHarmonicRatio({.wave = OpWave::Custom});
    std::printf("      Custom wave 880/440 Hz: %.1f with the second partial alone, %.4f with the first\n", octave,
                fundamental);
    check(octave > 20.0 && fundamental < 0.01, "a Custom wave plays the partials it is drawn from");

    // A macro moves what it is mapped to, for every voice: mapped to A's output at -100 % and turned up, it
    // silences A; turned down, A sounds again.
    {
        setParams({{slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(kMaxSources, 0), -1.0},
                   {paramIndex(0, MacroParam::Value), 1.0}});
        const float muted = peakOf(play({}));
        setParams({{paramIndex(0, MacroParam::Value), 0.0}});
        const float sounding = peakOf(play({}));
        setParams({{slotTargetIndex(kMaxSources, 0), 0.0}, {slotAmountIndex(kMaxSources, 0), 0.0}});
        std::printf("      macro on A's output: peak %.3f turned up, %.3f turned down\n", muted, sounding);
        check(muted < 0.01f && sounding > 0.1f, "a macro moves its target for every voice, as far as it is turned");
        // Bipolar, it moves nothing at the knob's middle and half its depth at the ends: A's output stays full at the
        // middle, comes down by half turned up, and turned down would go past full, where it stays.
        setParams({{slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(kMaxSources, 0), -1.0},
                   {paramIndex(0, MacroParam::Bipolar), 1.0},
                   {paramIndex(0, MacroParam::Value), 0.5}});
        const float middle = peakOf(play({}));
        setParams({{paramIndex(0, MacroParam::Value), 1.0}});
        const float up = peakOf(play({}));
        setParams({{paramIndex(0, MacroParam::Value), 0.0}});
        const float down = peakOf(play({}));
        setParams({{slotTargetIndex(kMaxSources, 0), 0.0},
                   {slotAmountIndex(kMaxSources, 0), 0.0},
                   {paramIndex(0, MacroParam::Bipolar), 0.0}});
        std::printf("      bipolar macro on A's output at -100 %%: peak %.3f at the middle, %.3f up, %.3f down (%.3f "
                    "without it)\n",
                    middle, up, down, sounding);
        check(std::abs(middle - sounding) < 1e-3f && up > 0.4f * sounding && up < 0.6f * sounding &&
                  std::abs(down - sounding) < 1e-3f,
              "a bipolar macro moves nothing at its middle and half its depth either way");
    }

    // The mod wheel moves nothing until it is assigned; then it is a macro of its own: mapped to A's output at
    // -100 % and turned up, it silences A.
    {
        const int wheel = paramIndex(GlobalParam::ModWheel);
        const float resting = peakOf(play({}));
        setParams({{wheel, 1.0}});
        const float unassigned = peakOf(play({}));
        setParams({{slotTargetIndex(kWheelSource, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(kWheelSource, 0), -1.0}});
        const float muted = peakOf(play({}));
        setParams({{wheel, 0.0}, {slotTargetIndex(kWheelSource, 0), 0.0}, {slotAmountIndex(kWheelSource, 0), 0.0}});
        std::printf("      mod wheel up: peak %.3f unassigned (%.3f down), %.3f on A's output at -100 %%\n", unassigned,
                    resting, muted);
        check(std::abs(unassigned - resting) < 1e-4f && muted < 0.01f,
              "the mod wheel moves only what it is assigned to, like a macro");
        // Bipolar, like a macro: down, it takes A's output up from where it is (already full), and all the way up
        // half of the depth down.
        setParams({{slotTargetIndex(kWheelSource, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(kWheelSource, 0), -1.0},
                   {paramIndex(WheelParam::Bipolar), 1.0}});
        const float bipolarDown = peakOf(play({}));
        setParams({{wheel, 1.0}});
        const float bipolarUp = peakOf(play({}));
        setParams({{wheel, 0.0},
                   {slotTargetIndex(kWheelSource, 0), 0.0},
                   {slotAmountIndex(kWheelSource, 0), 0.0},
                   {paramIndex(WheelParam::Bipolar), 0.0}});
        std::printf("      bipolar mod wheel on A's output at -100 %%: peak %.3f down, %.3f up\n", bipolarDown, bipolarUp);
        check(std::abs(bipolarDown - resting) < 1e-3f && bipolarUp > 0.4f * resting && bipolarUp < 0.6f * resting,
              "a bipolar mod wheel swings its target both ways");
    }

    // With its curve drawn along the cutoff knobs' (nothing up to the pitch at 20 Hz, then straight up to where G9
    // sits on them), the note source puts a cutoff right on the note: a narrow notch at 0, modulated 100 % by it,
    // takes an A4 saw's fundamental away. Bent two semitones up (the default range), the notch goes along to 494 Hz.
    {
        const int curve = paramIndex(NoteParam::Curve);
        const Patch notch{.wave = OpWave::Saw, .throughFilter = true, .type = FilterType::Notch, .cutoffHz = 20.0,
                          .resonance = 0.9};
        const double open = magnitude(play({.wave = OpWave::Saw}), 440.0);
        setParams({{slotTargetIndex(kNoteSource, 0), slotTargetValue(paramIndex(0, FilterParam::Cutoff))},
                   {slotAmountIndex(kNoteSource, 0), 1.0},
                   {curve, 3.0},
                   {shapeFieldIndex(curve, 1, PointField::X), pitchOfHz(kMinCutoff) / kMaxNotePitch},
                   {shapeFieldIndex(curve, 1, PointField::Y), -1.0},
                   {shapeFieldIndex(curve, 2, PointField::X), 1.0},
                   {shapeFieldIndex(curve, 2, PointField::Y), 2.0 * cutoffNormOfPitch(kMaxNotePitch) - 1.0}});
        const double onNote = magnitude(play(notch), 440.0) / open;
        const clap_event_midi_t bendUp = bendEvent(1.0), center = bendEvent(0.0);
        runBlock({&bendUp.header});
        const std::vector<float> bent = play(notch);
        runBlock({&center.header});
        const double bentOpen = magnitude(play({.wave = OpWave::Saw}), 440.0 * std::exp2(2.0 / 12.0));
        runBlock({&bendUp.header});
        const double onBent = magnitude(bent, 440.0 * std::exp2(2.0 / 12.0)) / bentOpen;
        runBlock({&center.header});
        // Without it, the notch sits at 20 Hz and the fundamental comes through.
        setParams({{slotTargetIndex(kNoteSource, 0), 0.0}, {slotAmountIndex(kNoteSource, 0), 0.0}});
        const double offNote = magnitude(play(notch), 440.0) / open;
        std::printf("      A4 saw through a notch the note source tunes: fundamental x%.4f, bent x%.4f, untuned x%.3f\n",
                    onNote, onBent, offNote);
        check(onNote < 0.03 && onBent < 0.03 && offNote > 0.8,
              "the note source puts a cutoff on the note's pitch, pitch bend included");
    }

    // Its range and curve: across A3 to A5 in a straight line, it takes A's output (at 0, modulated by 100 %)
    // to half at A4 and all of it at A5, none at A3; with the curve drawn the other way round, an A3 brings A
    // out in full and an A5 not at all. As it starts, it rises in a straight line over every MIDI note.
    {
        const int curve = paramIndex(NoteParam::Curve);
        play({}); // A straight out again, through no filter
        setParams({{paramIndex(0, OpParam::Out), 0.0},
                   {slotTargetIndex(kNoteSource, 0), slotTargetValue(paramIndex(0, OpParam::Out))},
                   {slotAmountIndex(kNoteSource, 0), 1.0},
                   {paramIndex(NoteParam::Low), 57.0},
                   {paramIndex(NoteParam::High), 81.0},
                   {curve, 2.0},
                   {shapeFieldIndex(curve, 1, PointField::X), 1.0},
                   {shapeFieldIndex(curve, 1, PointField::Y), 1.0}});
        auto peakAt = [&](int16_t key) {
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 90, key);
            runBlock({&on.header});
            runBlocks(4);
            const float peak = runBlocks(8);
            runBlock({&chokeAll.header});
            return peak;
        };
        const float a3 = peakAt(57), a4 = peakAt(69), a5 = peakAt(81);
        // Bipolar, it swings about the middle of what it puts out: nothing at A4, half at A5.
        setParams({{paramIndex(NoteParam::Bipolar), 1.0}});
        const float bipolarA4 = peakAt(69), bipolarA5 = peakAt(81);
        setParams({{paramIndex(NoteParam::Bipolar), 0.0}});
        std::printf("      bipolar: peak %.3f at A4, %.3f at A5\n", bipolarA4, bipolarA5);
        check(bipolarA4 < 0.005f && std::abs(bipolarA5 / a5 - 0.5f) < 0.05f,
              "a bipolar note source swings about the middle of what it puts out");
        setParams({{shapeFieldIndex(curve, 0, PointField::Y), 1.0}, {shapeFieldIndex(curve, 1, PointField::Y), -1.0}});
        const float flippedA3 = peakAt(57), flippedA5 = peakAt(81);
        // Back to the range and curve it starts with, as the plugin lists them.
        std::vector<clap_event_param_value_t> back;
        for (int i = paramIndex(NoteParam::Low); i < paramIndex(NoteParam::Count); ++i) {
            clap_param_info_t info;
            if (params->get_info(plugin, static_cast<uint32_t>(i), &info))
                back.push_back(paramEvent(i, info.default_value));
        }
        std::vector<const clap_event_header_t *> events;
        for (const clap_event_param_value_t &ev : back)
            events.push_back(&ev.header);
        runBlock(events);
        const float lowKey = peakAt(32), highKey = peakAt(95);
        setParams({{paramIndex(0, OpParam::Out), 1.0},
                   {slotTargetIndex(kNoteSource, 0), 0.0},
                   {slotAmountIndex(kNoteSource, 0), 0.0}});
        std::printf("      A's output by the note over A3..A5: peak %.3f / %.3f / %.3f, curve flipped %.3f at A3, %.3f at A5\n",
                    a3, a4, a5, flippedA3, flippedA5);
        std::printf("      as it starts: peak %.3f at G#1, %.3f at B6 (x%.3f and x%.3f of A5's)\n", lowKey, highKey,
                    lowKey / a5, highKey / a5);
        check(a3 < 0.005f && a5 > 0.2f && std::abs(a4 / a5 - 0.5f) < 0.05f && flippedA3 > 0.2f && flippedA5 < 0.005f,
              "the note source spans its range through its curve");
        check(std::abs(lowKey / a5 - 32.0f / 127.0f) < 0.03f && std::abs(highKey / a5 - 95.0f / 127.0f) < 0.04f,
              "as it starts, the note source rises in a straight line from C-1 to G9");
    }

    // Effects, on the plain sine. Slot 0 holds a shaper, slot 1 a DJ EQ, slot 2
    // a delay, slot 3 a filter, slots 4 and 5 parametric EQs; each starts on.
    {
        const int order = paramIndex(GlobalParam::FxOrder);
        auto type = [](int slot) { return paramIndex(slot, FxParam::Type); };
        auto fx = [](int slot, FxParam field) { return paramIndex(slot, field); };
        // A hard shaper at full drive, unlinked, turns the sine into a square at its output level.
        setParams({{type(0), static_cast<double>(FxType::Shaper)},
                   {fx(0, FxParam::ShaperLink), 0.0},
                   {fx(0, FxParam::ShaperMode), static_cast<double>(ClipMode::Hard)},
                   {fx(0, FxParam::ShaperDrive), 36.0},
                   {fx(0, FxParam::ShaperOutput), -6.0}});
        const float clipped = peakOf(play({}));
        std::printf("      hard shaper at -6 dB output: peak %.3f\n", clipped);
        check(clipped > 0.45f && clipped < 0.52f, "the hard shaper holds the level at its ceiling");
        bool modesSound = true;
        for (int m = 0; m < static_cast<int>(ClipMode::Count); ++m) {
            setParams({{fx(0, FxParam::ShaperMode), static_cast<double>(m)}});
            const float peak = peakOf(play({}));
            std::printf("      shaper mode %d: peak %.3f\n", m, peak);
            modesSound = modesSound && peak > 0.2f && peak < 1.0f;
        }
        check(modesSound, "every shaper mode sounds at a sane level");
        // Linked, the output comes down by as much as keeps a -6 dBFS sine as loud as it went in: at 36 dB of drive,
        // where the hard shaper puts out a full-scale square, about 9 dB rather than all 36.
        setParams({{fx(0, FxParam::ShaperMode), static_cast<double>(ClipMode::Hard)},
                   {fx(0, FxParam::ShaperOutput), 0.0},
                   {fx(0, FxParam::ShaperLink), 1.0}});
        const float linkedPeak = peakOf(play({}));
        setParams({{fx(0, FxParam::ShaperLink), 0.0}});
        const double linkedDb = clipOutputDb(0.0, 36.0, true);
        std::printf("      hard shaper at 36 dB of drive, linked: peak %.4f (%.1f dB)\n", linkedPeak, linkedDb);
        check(std::abs(linkedPeak - std::pow(10.0, linkedDb / 20.0)) < 0.01 && linkedDb < -8.0 && linkedDb > -10.0,
              "linked, the shaper's output comes down as its drive goes up, keeping the level about even");

        // EQ mid +6 dB around a shaper with 18 dB of drive: boosting after the
        // shaper gets louder than clipping the boost.
        setParams({{fx(0, FxParam::ShaperMode), static_cast<double>(ClipMode::Hard)},
                   {fx(0, FxParam::ShaperDrive), 18.0},
                   {fx(0, FxParam::ShaperOutput), 0.0},
                   {type(1), static_cast<double>(FxType::Eq)},
                   {fx(1, FxParam::EqMid), 1.0},
                   {order, fxOrderStarting({0, 1, 2, 3, 4, 5})}});
        const float clipThenEq = peakOf(play({}));
        setParams({{order, fxOrderStarting({1, 0, 2, 3, 4, 5})}});
        const float eqThenClip = peakOf(play({}));
        std::printf("      peak %.2f shaper then EQ, %.2f EQ then shaper\n", clipThenEq, eqThenClip);
        check(clipThenEq > 1.5f * eqThenClip, "the FX order changes the result");

        // DJ EQ: unity is transparent, killing the mid band removes the 440 Hz sine.
        // An empty slot passes the sound untouched.
        setParams({{type(0), static_cast<double>(FxType::None)}, {fx(1, FxParam::EqMid), 0.0}, {order, 0.0}});
        const std::vector<float> flat = play({});
        setParams({{fx(1, FxParam::EqMid), -1.0}});
        const std::vector<float> killed = play({});
        setParams({{fx(1, FxParam::Enabled), 0.0}});
        const std::vector<float> dry = play({});
        const double flatRatio = magnitude(flat, 440.0) / magnitude(dry, 440.0);
        const double killRatio = magnitude(killed, 440.0) / magnitude(dry, 440.0);
        std::printf("      EQ at 440 Hz: %.3f flat, %.3f with the mid killed\n", flatRatio, killRatio);
        check(std::abs(flatRatio - 1.0) < 0.02 && killRatio < 0.15, "the EQ is flat at unity and kills its bands");
        setParams({{type(1), static_cast<double>(FxType::None)}});

        // Compressor: the sine peaks 18 dB over a -30 dB threshold; at 4:1 it comes out
        // 13.5 dB down (x0.21). Makeup brings it back up; at 0 % mix it is untouched.
        setParams({{type(6), static_cast<double>(FxType::Compressor)},
                   {fx(6, FxParam::CompThreshold), -30.0},
                   {fx(6, FxParam::CompRatio), 4.0},
                   {fx(6, FxParam::CompKnee), 0.0},
                   {fx(6, FxParam::CompAttack), 0.0},
                   {fx(6, FxParam::CompRelease), normFromTime(0.1)}});
        const double squeezed = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{fx(6, FxParam::CompMakeup), 12.0}});
        const double madeUp = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{fx(6, FxParam::CompMakeup), 0.0}, {fx(6, FxParam::CompMix), 0.0}});
        const double parallel = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      compressor at 440 Hz: x%.3f at 4:1, x%.3f with +12 dB makeup, x%.3f at 0 %% mix\n", squeezed,
                    madeUp, parallel);
        check(squeezed > 0.17 && squeezed < 0.27 && std::abs(madeUp / squeezed - 3.98) < 0.15 &&
                  std::abs(parallel - 1.0) < 0.02,
              "the compressor turns the sine down by its ratio");

        // Three bands: 440 Hz sits in the mid band. Only the mid band's settings reach it.
        setParams({{type(6), static_cast<double>(FxType::MultiComp)},
                   {mcThresholdIndex(6, 0), -60.0},
                   {mcThresholdIndex(6, 1), 0.0},
                   {mcThresholdIndex(6, 2), -60.0},
                   {fx(6, FxParam::McAttack), 0.0}});
        const double untouched = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{mcThresholdIndex(6, 1), -30.0}, {mcRatioIndex(6, 1), 4.0}});
        const double midSqueezed = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{mcGainIndex(6, 1), 6.0}});
        const double midLifted = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      3-band compressor at 440 Hz: x%.3f with the mid band idle, x%.3f at 4:1, x%.3f with +6 dB\n",
                    untouched, midSqueezed, midLifted);
        check(std::abs(untouched - 1.0) < 0.05 && midSqueezed > 0.15 && midSqueezed < 0.3 &&
                  std::abs(midLifted / midSqueezed - 2.0) < 0.1,
              "the three-band compressor works on each band on its own");
        setParams({{type(6), static_cast<double>(FxType::None)}});

        // Parametric EQ: a +12 dB bell on 440 Hz, then a second instance of the same on top.
        setParams({{type(4), static_cast<double>(FxType::ParamEq)},
                   {peqFreqIndex(4, 0), normFromCutoff(440.0)},
                   {peqGainIndex(4, 0), 12.0},
                   {peqGainIndex(4, 1), 0.0}});
        const double oneBell = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{type(5), static_cast<double>(FxType::ParamEq)},
                   {peqFreqIndex(5, 0), normFromCutoff(440.0)},
                   {peqGainIndex(5, 0), 12.0}});
        const double twoBells = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      +12 dB bell at 440 Hz: x%.2f, two of them x%.2f\n", oneBell, twoBells);
        check(std::abs(oneBell - 3.98) < 0.3, "a parametric EQ band boosts at its frequency");
        check(std::abs(twoBells - oneBell * oneBell) < 1.0, "two instances of an effect both work");
        // A low cut at 2 kHz takes most of the 440 Hz sine away; a +12 dB low shelf at 4 kHz lifts it whole.
        setParams({{type(5), static_cast<double>(FxType::None)},
                   {peqTypeIndex(4, 0), static_cast<double>(PeqType::LowCut)},
                   {peqFreqIndex(4, 0), normFromCutoff(2000.0)},
                   {peqQIndex(4, 0), normFromQ(0.7071)}});
        const double lowCut = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{peqTypeIndex(4, 0), static_cast<double>(PeqType::LowShelf)}, {peqFreqIndex(4, 0), normFromCutoff(4000.0)}});
        const double lowShelf = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      440 Hz through a low cut at 2 kHz: x%.3f, a +12 dB low shelf at 4 kHz: x%.2f\n", lowCut, lowShelf);
        check(lowCut < 0.1 && std::abs(lowShelf - 3.98) < 0.4, "the EQ's bands also cut and shelve");
        setParams({{peqTypeIndex(4, 0), static_cast<double>(PeqType::Peak)}});
        setParams({{type(4), static_cast<double>(FxType::None)}, {type(5), static_cast<double>(FxType::None)}});

        // Delay: a short note's echoes, recorded from the note on, once the note
        // itself has faded (its release is 15 ms). Returns when each side's first
        // echo came (seconds after the note), how loud it got and its energy.
        struct Echo {
            double seconds[2] = {-1.0, -1.0};
            float peak[2] = {0.0f, 0.0f};
            double energy[2] = {0.0, 0.0};
            bool ringing = false;
        };
        auto echo = [&]() {
            Echo e;
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 70, 69);
            const clap_event_note_t off = noteEvent(CLAP_EVENT_NOTE_OFF, 70, 69);
            runBlock({&on.header});
            runBlock();
            runBlock({&off.header});
            for (uint32_t b = 3; b < blocksPerSecond * 3 / 10; ++b) {
                runBlock();
                if (b == 6)
                    e.ringing = status == CLAP_PROCESS_CONTINUE; // the voice is done by now
                if (b < 7)
                    continue;
                const float *sides[2] = {left.data(), right.data()};
                for (int c = 0; c < 2; ++c) {
                    for (uint32_t i = 0; i < kBlock; ++i) {
                        const float level = std::fabs(sides[c][i]);
                        if (e.seconds[c] < 0.0 && level > 0.02f)
                            e.seconds[c] = (b * kBlock + i) / kSampleRate;
                        e.peak[c] = std::max(e.peak[c], level);
                        e.energy[c] += static_cast<double>(level) * level;
                    }
                }
            }
            runBlocks(blocksPerSecond);
            return e;
        };
        // Ping-pong: the first echo comes on the left only. Processing goes on
        // after the voice ended, until the echoes are gone.
        setParams({{type(2), static_cast<double>(FxType::Delay)},
                   {fx(2, FxParam::DelaySync), 0.0},
                   {fx(2, FxParam::DelayTimeL), normFromTime(0.1)},
                   {fx(2, FxParam::DelayTimeR), normFromTime(0.1)},
                   {fx(2, FxParam::DelayFeedback), 0.0},
                   {fx(2, FxParam::DelayPingPong), 1.0},
                   {fx(2, FxParam::DelayMix), 0.5}});
        const Echo pingPong = echo();
        std::printf("      ping-pong echo: left %.3f, right %.3f\n", pingPong.peak[0], pingPong.peak[1]);
        check(pingPong.ringing && pingPong.peak[0] > 0.05f && pingPong.peak[1] < 1e-4f,
              "the delay echoes after the note, ping-pong starts left");
        check(status == CLAP_PROCESS_SLEEP && runBlock() == 0.0f, "processing sleeps once the echoes died away");
        // At zero width the same echo comes out on both sides.
        setParams({{fx(2, FxParam::DelayWidth), 0.0}});
        const Echo mono = echo();
        check(mono.peak[1] > 0.02f && std::abs(mono.peak[0] - mono.peak[1]) < 1e-4f,
              "the delay's width narrows the echoes to mono");
        // Separate sides: 100 ms on the left, 200 ms on the right.
        setParams({{fx(2, FxParam::DelayWidth), 1.0},
                   {fx(2, FxParam::DelayPingPong), 0.0},
                   {fx(2, FxParam::DelayTimeR), normFromTime(0.2)}});
        const Echo sides = echo();
        std::printf("      first echoes: left after %.0f ms, right after %.0f ms\n", sides.seconds[0] * 1000.0,
                    sides.seconds[1] * 1000.0);
        check(std::abs(sides.seconds[0] - 0.1) < 0.006 && std::abs(sides.seconds[1] - 0.2) < 0.006,
              "the delay's left and right times are separate");
        // Linked, the right side waits the left side's 100 ms too.
        setParams({{fx(2, FxParam::DelayLink), 1.0}});
        const Echo linked = echo();
        setParams({{fx(2, FxParam::DelayLink), 0.0}});
        check(std::abs(linked.seconds[0] - 0.1) < 0.006 && std::abs(linked.seconds[1] - 0.1) < 0.006,
              "linked, the delay's right side takes the left side's time");
        // The tone's high-pass end thins a 440 Hz echo out; its low-pass end darkens it less.
        setParams({{fx(2, FxParam::DelayTone), 1.0}});
        const Echo thin = echo();
        setParams({{fx(2, FxParam::DelayTone), 0.0}});
        const double thinRatio = std::sqrt(thin.energy[0] / sides.energy[0]);
        std::printf("      echo through the tone at HP 4 kHz: %.3f of open\n", thinRatio);
        check(thinRatio < 0.3, "the delay's tone knob high-passes above the middle");
        setParams({{type(2), static_cast<double>(FxType::None)}});

        // The filter effect low-passes the mix like a voice filter does; at 0 % mix it does nothing.
        setParams({{type(3), static_cast<double>(FxType::Filter)},
                   {fx(3, FxParam::FilterType), static_cast<double>(FilterType::LowPass)},
                   {fx(3, FxParam::FilterSlope), 3.0},
                   {fx(3, FxParam::FilterCutoff), normFromCutoff(300.0)},
                   {fx(3, FxParam::FilterResonance), 0.0}});
        const double fxLowPassed = secondHarmonicRatio({.wave = OpWave::Saw});
        setParams({{fx(3, FxParam::FilterMix), 0.0}});
        const double fxDry = secondHarmonicRatio({.wave = OpWave::Saw});
        std::printf("      saw 880/440 Hz: %.2f through the filter effect at 300 Hz, %.2f at 0 %% mix\n", fxLowPassed,
                    fxDry);
        check(fxLowPassed < saw / 4.0 && std::abs(fxDry - saw) < 0.01, "the filter effect filters the mix");
        // Key tracking moves the cutoff with the newest voice's pitch from C4 on: at 100 %, a cutoff at C4 sits on
        // an A4's 440 Hz, as one set there does.
        setParams({{fx(3, FxParam::FilterMix), 1.0},
                   {fx(3, FxParam::FilterCutoff), normFromCutoff(hzOfPitch(60.0))},
                   {fx(3, FxParam::FilterKeyTrack), 1.0}});
        const double fxTracked = secondHarmonicRatio({.wave = OpWave::Saw});
        setParams({{fx(3, FxParam::FilterKeyTrack), 0.0}});
        const double fxAtC4 = secondHarmonicRatio({.wave = OpWave::Saw});
        setParams({{fx(3, FxParam::FilterCutoff), normFromCutoff(440.0)}});
        const double fxAtA4 = secondHarmonicRatio({.wave = OpWave::Saw});
        std::printf("      saw 880/440 Hz, filter effect at C4: %.3f, key tracked on an A4 %.3f, at 440 Hz %.3f\n",
                    fxAtC4, fxTracked, fxAtA4);
        check(std::abs(fxTracked / fxAtA4 - 1.0) < 0.02 && fxAtC4 < 0.8 * fxTracked,
              "the filter effect's key tracking follows the note");
        bool fxFiltersSound = true;
        setParams({{fx(3, FxParam::FilterResonance), 0.9}});
        for (int t = 0; t < static_cast<int>(FilterType::Count); ++t) {
            setParams({{fx(3, FxParam::FilterType), static_cast<double>(t)},
                       {fx(3, FxParam::FilterCutoff), normFromCutoff(1200.0)}});
            const float peak = peakOf(play({.wave = OpWave::Saw}));
            fxFiltersSound = fxFiltersSound && peak > 0.005f && peak < 4.0f;
        }
        check(fxFiltersSound, "every filter effect type passes sound and stays finite");
        setParams({{type(3), static_cast<double>(FxType::None)}});
        runBlocks(blocksPerSecond); // let any comb ring out

        // The phaser at 100 % (half wet) and the flanger at 50 %, with their sweeps held still, cancel 440 Hz
        // where their notch sits: two all-passes cornered there, or a delay of half its period. At 0 % mix
        // they leave it alone.
        setParams({{type(7), static_cast<double>(FxType::Phaser)},
                   {fx(7, FxParam::PhaserDepth), 0.0},
                   {fx(7, FxParam::PhaserCenter), normFromCutoff(440.0)},
                   {fx(7, FxParam::PhaserStages), 2.0},
                   {fx(7, FxParam::PhaserFeedback), 0.0},
                   {fx(7, FxParam::PhaserMix), 1.0}});
        const double phased = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{fx(7, FxParam::PhaserMix), 0.0}});
        const double unphased = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        setParams({{type(7), static_cast<double>(FxType::Flanger)},
                   {fx(7, FxParam::FlangerDepth), 0.0},
                   {fx(7, FxParam::FlangerDelay), normFromTime(1.0 / 880.0)},
                   {fx(7, FxParam::FlangerFeedback), 0.0},
                   {fx(7, FxParam::FlangerMix), 0.5}});
        const double flanged = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      440 Hz through a notch on it: x%.3f phaser (x%.3f at 0 %% mix), x%.3f flanger\n", phased,
                    unphased, flanged);
        check(phased < 0.1 && std::abs(unphased - 1.0) < 0.02 && flanged < 0.1,
              "the phaser and flanger notch where their settings put it");

        // The frequency shifter moves the sine by its Shift's share of its Range: +50 % of 200 Hz takes it to 540 Hz,
        // -50 % to 340 Hz.
        // The widener leaves the mono sum alone at full width: what it adds goes to the sides, one against the other.
        // At 0 % width it adds a chorus in the middle instead, and its high-pass keeps the lows out of it.
        setParams({{type(7), static_cast<double>(FxType::Widener)},
                   {fx(7, FxParam::WidenerWidth), 1.0},
                   {fx(7, FxParam::WidenerMix), 1.0},
                   {fx(7, FxParam::WidenerHighPass), normFromCutoff(100.0)}});
        const auto sumAndSides = [&](int blocks) {
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 80, 69);
            runBlock({&on.header});
            runBlocks(8);
            double sum = 0.0, sides = 0.0;
            for (int b = 0; b < blocks; ++b) {
                runBlock();
                for (uint32_t i = 0; i < kBlock; ++i) {
                    sum += std::pow(0.5 * (left[i] + right[i]), 2.0);
                    sides += std::pow(0.5 * (left[i] - right[i]), 2.0);
                }
            }
            runBlock({&chokeAll.header});
            runBlocks(4);
            return std::pair{std::sqrt(sum), std::sqrt(sides)};
        };
        const auto [wideSum, wideSides] = sumAndSides(16);
        setParams({{fx(7, FxParam::WidenerMix), 0.0}});
        const auto [drySum, drySides] = sumAndSides(16);
        setParams({{fx(7, FxParam::WidenerMix), 1.0}, {fx(7, FxParam::WidenerWidth), 0.0}});
        const auto [chorusSum, chorusSides] = sumAndSides(16);
        setParams({{fx(7, FxParam::WidenerWidth), 1.0}, {fx(7, FxParam::WidenerHighPass), normFromCutoff(2000.0)}});
        const auto [highSum, highSides] = sumAndSides(16);
        std::printf("      widener on a 440 Hz sine: sides %.3f of the sum at full width (mono sum x%.3f), %.3f at 0 %% width "
                    "(sum x%.2f), %.3f with its high-pass at 2 kHz\n",
                    wideSides / wideSum, wideSum / drySum, chorusSides / chorusSum, chorusSum / drySum, highSides / highSum);
        check(drySides / drySum < 1e-4 && wideSides / wideSum > 0.15 && std::abs(wideSum / drySum - 1.0) < 0.01 &&
                  chorusSides / chorusSum < 1e-4 && std::abs(chorusSum / drySum - 1.0) > 0.05 &&
                  highSides / highSum < 0.3 * wideSides / wideSum,
              "the widener widens above its high-pass and keeps the mono sum dry at full width");

        setParams({{type(7), static_cast<double>(FxType::FreqShifter)},
                   {fx(7, FxParam::ShiftRange), normFromShiftRange(200.0)},
                   {fx(7, FxParam::ShiftAmount), 0.5},
                   {fx(7, FxParam::ShiftSpread), 0.0},
                   {fx(7, FxParam::ShiftFeedback), 0.0},
                   {fx(7, FxParam::ShiftMix), 1.0}});
        const std::vector<float> up = play({});
        setParams({{fx(7, FxParam::ShiftAmount), -0.5}});
        const std::vector<float> down = play({});
        const double upRatio = magnitude(up, 540.0) / std::max(magnitude(up, 340.0), magnitude(up, 440.0));
        const double downRatio = magnitude(down, 340.0) / std::max(magnitude(down, 540.0), magnitude(down, 440.0));
        std::printf("      shifted sine against what is left elsewhere: x%.0f up, x%.0f down\n", upRatio, downRatio);
        check(upRatio > 20.0 && downRatio > 20.0, "the frequency shifter moves a sine by its shift, either way");

        // A reverb keeps sounding after the note, and the plugin keeps processing for its tail.
        setParams({{type(7), static_cast<double>(FxType::Reverb)}, {fx(7, FxParam::ReverbMix), 0.5}});
        const Echo tail = echo();
        std::printf("      reverb tail after the note: peak %.3f left, %.3f right\n", tail.peak[0], tail.peak[1]);
        check(tail.ringing && tail.peak[0] > 0.01f && tail.peak[0] < 2.0f && tail.peak[1] > 0.01f &&
                  tail.energy[0] != tail.energy[1],
              "the reverb rings on after the note, in stereo");
        // So does each of its algorithms, at a sane level.
        bool algorithmsRing = true;
        for (int t = 0; t < static_cast<int>(ReverbType::Count); ++t) {
            setParams({{fx(7, FxParam::ReverbType), static_cast<double>(t)}});
            const Echo ring = echo();
            std::printf("      reverb algorithm %d: tail peak %.3f\n", t, ring.peak[0]);
            algorithmsRing = algorithmsRing && ring.ringing && ring.peak[0] > 0.01f && ring.peak[0] < 2.0f;
        }
        check(algorithmsRing, "every reverb algorithm rings on after the note, at a sane level");
        // The shimmer is a hall with octaves in its tail: after an A4 sine, a hall's tail has nothing around 880 Hz,
        // a shimmer's has the octave, and the one above it too. Its pitch shifter's grains don't line up in phase,
        // so an octave's energy lies a few Hz either side of it: each is measured over 60 Hz around it.
        auto band = [&](const std::vector<float> &signal, double hz) {
            double energy = 0.0;
            for (double f = hz - 30.0; f <= hz + 30.0; f += 1.0)
                energy += std::pow(magnitude(signal, f), 2.0);
            return std::sqrt(energy);
        };
        std::vector<float> lastTail;
        auto tailOctaves = [&](ReverbType t) {
            setParams({{fx(7, FxParam::ReverbType), static_cast<double>(t)}});
            const clap_event_note_t on = noteEvent(CLAP_EVENT_NOTE_ON, 71, 69);
            const clap_event_note_t off = noteEvent(CLAP_EVENT_NOTE_OFF, 71, 69);
            runBlock({&on.header});
            runBlocks(blocksPerSecond / 4);
            runBlock({&off.header});
            runBlocks(blocksPerSecond / 10);
            std::vector<float> tail;
            for (uint32_t b = 0; b < blocksPerSecond; ++b) {
                runBlock();
                tail.insert(tail.end(), left.begin(), left.end());
            }
            runBlocks(blocksPerSecond * 3); // the rest of it dies away
            const double fundamental = band(tail, 440.0);
            lastTail = tail;
            return std::pair{band(tail, 880.0) / fundamental, band(tail, 1760.0) / fundamental};
        };
        // Any other band of the last tail, against its 440 Hz.
        auto shimmerBand = [&](double hz) { return band(lastTail, hz) / band(lastTail, 440.0); };
        const auto [hallOctave, hallTwo] = tailOctaves(ReverbType::Hall);
        const auto [shimmerOctave, shimmerTwo] = tailOctaves(ReverbType::Shimmer);
        std::printf("      reverb tail after an A4 sine, 880 and 1760 Hz against 440: hall x%.4f x%.4f, shimmer x%.3f x%.3f\n",
                    hallOctave, hallTwo, shimmerOctave, shimmerTwo);
        check(hallOctave < 0.02 && shimmerOctave > 0.2 && shimmerTwo > 0.05,
              "the shimmer's tail rises in octaves, where a hall's keeps to the note");
        // Its Pitch sets the interval: a fifth up brings in 659 Hz and then 988, not the octave; an octave down,
        // 220 Hz. With the Shimmer knob at 0 it shifts nothing, like a hall.
        const int shimmerPitch = fx(7, FxParam::ReverbShimmerPitch);
        setParams({{shimmerPitch, 7.0}});
        const auto [fifthOctave, fifthTwo] = tailOctaves(ReverbType::Shimmer);
        const double fifth = shimmerBand(659.26), fifthNinth = shimmerBand(987.77);
        setParams({{shimmerPitch, -12.0}});
        tailOctaves(ReverbType::Shimmer);
        const double below = shimmerBand(220.0);
        setParams({{shimmerPitch, 12.0}, {fx(7, FxParam::ReverbShimmer), 0.0}});
        const auto [unshiftedOctave, unshiftedTwo] = tailOctaves(ReverbType::Shimmer);
        setParams({{fx(7, FxParam::ReverbShimmer), 0.5}});
        std::printf("      shimmer a fifth up: 659 Hz x%.3f, 988 Hz x%.3f, 880 Hz x%.3f; an octave down: 220 Hz x%.3f; "
                    "unshifted: 880 Hz x%.4f\n",
                    fifth, fifthNinth, fifthOctave, below, unshiftedOctave);
        check(fifth > 0.2 && fifthNinth > 0.05 && fifthOctave < 0.5 * fifth && below > 0.2 && unshiftedOctave < 0.02,
              "the shimmer's Pitch sets the interval its tail climbs or falls by, and its Shimmer how much shifts");
        (void)fifthTwo;
        (void)unshiftedTwo;
        setParams({{fx(7, FxParam::ReverbType), static_cast<double>(ReverbType::Hall)}});

        // The disperser's all-passes leave a steady tone's level alone at full mix: only its phase turns.
        // (Centered on the tone itself it would hold it back past the note's end.)
        setParams({{type(7), static_cast<double>(FxType::Disperser)},
                   {fx(7, FxParam::DisperserFreq), normFromCutoff(3000.0)},
                   {fx(7, FxParam::DisperserStages), 16.0},
                   {fx(7, FxParam::DisperserPinch), 0.8},
                   {fx(7, FxParam::DisperserMix), 1.0}});
        const double dispersed = magnitude(play({}), 440.0) / magnitude(dry, 440.0);
        std::printf("      disperser on a 440 Hz sine: level x%.3f\n", dispersed);
        check(dispersed > 0.93 && dispersed < 1.07, "the disperser keeps a steady tone at its level");

        // Pushed to their limits, every new effect stays finite and sane.
        bool sane = true;
        const std::pair<FxType, std::vector<std::pair<int, double>>> extremes[] = {
            {FxType::Reverb,
             {{fx(7, FxParam::ReverbSize), 1.0}, {fx(7, FxParam::ReverbDecay), 1.0}, {fx(7, FxParam::ReverbMix), 1.0}}},
            {FxType::Reverb, // its tail pitched up and fed back for the longest decay
             {{fx(7, FxParam::ReverbType), static_cast<double>(ReverbType::Shimmer)},
              {fx(7, FxParam::ReverbSize), 1.0},
              {fx(7, FxParam::ReverbDecay), 1.0},
              {fx(7, FxParam::ReverbDamping), 0.0},
              {fx(7, FxParam::ReverbMix), 1.0}}},
            {FxType::Phaser,
             {{fx(7, FxParam::PhaserFeedback), 1.0}, {fx(7, FxParam::PhaserStages), 12.0}, {fx(7, FxParam::PhaserDepth), 1.0},
              {fx(7, FxParam::PhaserRate), normFromRate(20.0)}}},
            {FxType::Flanger,
             {{fx(7, FxParam::FlangerFeedback), -1.0}, {fx(7, FxParam::FlangerDepth), 1.0},
              {fx(7, FxParam::FlangerRate), normFromRate(20.0)}}},
            {FxType::Disperser, {{fx(7, FxParam::DisperserStages), 32.0}, {fx(7, FxParam::DisperserPinch), 1.0},
                                 {fx(7, FxParam::DisperserFreq), 1.0}}},
            {FxType::FreqShifter, {{fx(7, FxParam::ShiftRange), 1.0}, {fx(7, FxParam::ShiftAmount), 1.0},
                                   {fx(7, FxParam::ShiftFeedback), 0.9},
                                   {fx(7, FxParam::ShiftSpread), 1.0}}},
            {FxType::Widener, {{fx(7, FxParam::WidenerSize), 1.0}, {fx(7, FxParam::WidenerWidth), 0.0},
                                {fx(7, FxParam::WidenerMix), 1.0}, {fx(7, FxParam::WidenerHighPass), 0.0}}},
        };
        for (const auto &[effect, settings] : extremes) {
            std::vector<clap_event_param_value_t> evs{paramEvent(type(7), static_cast<double>(effect))};
            for (const auto &[index, value] : settings)
                evs.push_back(paramEvent(index, value));
            std::vector<const clap_event_header_t *> events;
            for (const clap_event_param_value_t &ev : evs)
                events.push_back(&ev.header);
            runBlock(events);
            const float peak = peakOf(play({.wave = OpWave::Saw}));
            std::printf("      effect type %d at its limits: peak %.3f\n", static_cast<int>(effect), peak);
            sane = sane && peak > 0.005f && peak < 4.0f;
        }
        check(sane, "the reverb (a shimmer too), phaser, flanger, disperser, shifter and widener stay finite at their "
                    "limits");
        setParams({{type(7), static_cast<double>(FxType::None)}, {fx(7, FxParam::ReverbType), 0.0}});
        runBlocks(blocksPerSecond * 3); // let the tail die away
    }

    // Mono, legato and glide, on a pure sine (B -> A off) so pitches are easy to tell apart.
    {
        constexpr double c4 = 261.63, e4 = 329.63;
        auto note = [&](uint16_t type, int32_t id, int16_t key) {
            const clap_event_note_t ev = noteEvent(type, id, key);
            return runBlock({&ev.header});
        };
        auto capture = [&](int blocks) {
            std::vector<float> signal;
            for (int b = 0; b < blocks; ++b) {
                runBlock();
                signal.insert(signal.end(), left.begin(), left.end());
            }
            return signal;
        };
        auto pitchOf = [&](const std::vector<float> &s) { // zero crossings of a sine
            int crossings = 0;
            for (size_t i = 1; i < s.size(); ++i)
                crossings += (s[i - 1] < 0.0f) != (s[i] < 0.0f);
            return crossings * kSampleRate / (2.0 * s.size());
        };
        const int mono = paramIndex(GlobalParam::Mono), legato = paramIndex(GlobalParam::Legato);
        const int glide = paramIndex(GlobalParam::Glide), always = paramIndex(GlobalParam::GlideAlways);
        const int decay = paramIndex(0, OpParam::Decay), sustain = paramIndex(0, OpParam::Sustain);
        setParams({{matrixIndex(1, 0), 0.0}, {mono, 1.0}, {legato, 1.0}, {glide, 0.0}, {always, 0.0}});

        // A new key takes the voice over; letting it go returns to the key still held.
        out.noteEnds.clear();
        note(CLAP_EVENT_NOTE_ON, 50, 60);
        runBlocks(4);
        note(CLAP_EVENT_NOTE_ON, 51, 64);
        runBlocks(2);
        const std::vector<float> both = capture(8);
        check(magnitude(both, e4) > 10.0 * magnitude(both, c4) && out.noteEnds.empty(),
              "mono: the newer key takes the voice over");
        note(CLAP_EVENT_NOTE_OFF, 51, 64);
        const bool e4Ended = out.noteEnds.size() == 1 && out.noteEnds[0].note_id == 51;
        runBlocks(2);
        const std::vector<float> back = capture(8);
        check(e4Ended && magnitude(back, c4) > 10.0 * magnitude(back, e4),
              "mono: letting go returns to the held key and ends the released note");
        note(CLAP_EVENT_NOTE_OFF, 50, 60);
        for (uint32_t b = 0; b < blocksPerSecond && out.noteEnds.size() < 2; ++b)
            runBlock();
        check(out.noteEnds.size() == 2 && out.noteEnds[1].note_id == 50, "mono: each note ends exactly once");

        // Legato keeps the envelope at its sustain level; without it a new note restarts the attack.
        auto overlapPeak = [&](double legatoOn) {
            setParams({{legato, legatoOn}, {decay, normFromTime(0.05)}, {sustain, 0.2}});
            note(CLAP_EVENT_NOTE_ON, 60, 60);
            runBlocks(blocksPerSecond / 4);
            const float onset = note(CLAP_EVENT_NOTE_ON, 61, 64); // a retriggered attack peaks right away
            const float peak = std::max(onset, runBlocks(4));
            runBlock({&chokeAll.header});
            return peak;
        };
        const float legatoPeak = overlapPeak(1.0);
        const float retriggerPeak = overlapPeak(0.0);
        std::printf("      overlapping note peak: %.3f legato, %.3f retriggered\n", legatoPeak, retriggerPeak);
        check(retriggerPeak > 3.0f * legatoPeak, "legato keeps envelopes running, retrigger restarts them");

        // Glide (200 ms) slides between overlapping notes, and only between those unless Always is on.
        setParams({{legato, 1.0}, {decay, normFromTime(1.5)}, {sustain, 1.0}, {glide, normFromTime(0.2)}});
        note(CLAP_EVENT_NOTE_ON, 70, 60);
        runBlocks(blocksPerSecond / 10);
        note(CLAP_EVENT_NOTE_ON, 71, 64);
        const double gliding = pitchOf(capture(8));
        runBlocks(blocksPerSecond / 2);
        const double arrived = pitchOf(capture(8));
        runBlock({&chokeAll.header});
        note(CLAP_EVENT_NOTE_ON, 72, 60);
        runBlocks(4);
        note(CLAP_EVENT_NOTE_OFF, 72, 60);
        note(CLAP_EVENT_NOTE_ON, 73, 64); // not overlapping: no glide
        const double jumped = pitchOf(capture(8));
        runBlock({&chokeAll.header});
        setParams({{always, 1.0}});
        note(CLAP_EVENT_NOTE_ON, 74, 60); // glides down from the last E4 first: let it arrive
        runBlocks(blocksPerSecond / 2);
        note(CLAP_EVENT_NOTE_OFF, 74, 60);
        note(CLAP_EVENT_NOTE_ON, 75, 64);
        const double alwaysGliding = pitchOf(capture(8));
        runBlock({&chokeAll.header});
        std::printf("      glide C4 -> E4: %.0f Hz after 40 ms, %.0f Hz later; detached %.0f Hz, with Always %.0f Hz\n",
                    gliding, arrived, jumped, alwaysGliding);
        check(gliding < 310.0 && std::abs(arrived - e4) < 0.02 * e4, "glide slides to the new note");
        check(std::abs(jumped - e4) < 0.03 * e4 && alwaysGliding < 310.0,
              "glide skips detached notes unless Always is on");

        // Back to the defaults for the rest of the tests.
        setParams({{mono, 0.0}, {glide, 0.0}, {always, 0.0}, {sustain, 0.6}});
    }

    // Voice limit: lowering it applies at once.
    {
        out.noteEnds.clear();
        const clap_event_param_value_t four = paramEvent(paramIndex(GlobalParam::Voices), 4.0);
        std::vector<clap_event_note_t> notes;
        for (int16_t i = 0; i < 6; ++i)
            notes.push_back(noteEvent(CLAP_EVENT_NOTE_ON, 200 + i, static_cast<int16_t>(50 + i)));
        std::vector<const clap_event_header_t *> events{&four.header};
        for (const clap_event_note_t &n : notes)
            events.push_back(&n.header);
        runBlock(events);
        check(out.noteEnds.size() == 2 && out.noteEnds[0].note_id == 200,
              "a lower voice limit applies immediately (6 notes on 4 voices steal 2)");
        runBlock({&chokeAll.header});
    }

    // Raising it past the allocated voices asks the host for a restart; after
    // reactivating, the voices are there.
    {
        restartRequests = 0;
        const clap_event_param_value_t forty = paramEvent(paramIndex(GlobalParam::Voices), 40.0);
        runBlock({&forty.header});
        check(restartRequests == 1, "raising the voice limit requests a restart");
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->activate(plugin, kSampleRate, 1, kBlock);
        plugin->start_processing(plugin);
        const auto *voiceInfo =
            static_cast<const clap_plugin_voice_info_t *>(plugin->get_extension(plugin, CLAP_EXT_VOICE_INFO));
        clap_voice_info_t info{};
        check(voiceInfo && voiceInfo->get(plugin, &info) && info.voice_count == 40 && info.voice_capacity == 40,
              "voice-info reports the new limit after reactivation");
        out.noteEnds.clear();
        std::vector<clap_event_note_t> notes;
        for (int16_t i = 0; i < 50; ++i)
            notes.push_back(noteEvent(CLAP_EVENT_NOTE_ON, 300 + i, static_cast<int16_t>(30 + i)));
        std::vector<const clap_event_header_t *> events;
        for (const clap_event_note_t &n : notes)
            events.push_back(&n.header);
        runBlock(events);
        check(out.noteEnds.size() == 10 && out.noteEnds[0].note_id == 300,
              "50 notes on 40 voices steal the 10 oldest");
        out.noteEnds.clear();
        check(runBlock({&chokeAll.header}) == 0.0f && out.noteEnds.size() == 40, "choke ends all 40 voices");
    }

    // CPU: 16 voices with all six operators routed into one another.
    std::vector<clap_event_param_value_t> routing;
    for (int from = 0; from < kOps; ++from) {
        routing.push_back(paramEvent(paramIndex(from, OpParam::Enabled), 1.0));
        routing.push_back(paramEvent(paramIndex(from, OpParam::Out), 0.3));
        if (from + 1 < kOps)
            routing.push_back(paramEvent(matrixIndex(from + 1, from), 0.3));
    }
    std::vector<const clap_event_header_t *> loadEvents;
    for (const clap_event_param_value_t &ev : routing)
        loadEvents.push_back(&ev.header);
    for (int i = 0; i < 16; ++i)
        loadEvents.push_back(&burst[i].header);
    runBlock(loadEvents);
    const auto start = std::chrono::steady_clock::now();
    const float loadPeak = runBlocks(blocksPerSecond);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("      16 voices x 6 operators: 1 s of audio in %.1f ms (%.1f %% of one core)\n", ms, ms / 10.0);
    check(std::isfinite(loadPeak), "full polyphony stays finite");
    runBlock({&chokeAll.header});

    // The same 16 voices with every operator at 8 unison copies.
    std::vector<clap_event_param_value_t> unison8;
    for (int op = 0; op < kOps; ++op) {
        unison8.push_back(paramEvent(paramIndex(op, OpParam::Unison), 8.0));
        unison8.push_back(paramEvent(paramIndex(op, OpParam::UnisonSpread), 1.0));
    }
    std::vector<const clap_event_header_t *> unisonEvents;
    for (const clap_event_param_value_t &ev : unison8)
        unisonEvents.push_back(&ev.header);
    for (int i = 0; i < 16; ++i)
        unisonEvents.push_back(&burst[i].header);
    runBlock(unisonEvents);
    const auto unisonStart = std::chrono::steady_clock::now();
    const float unisonPeak = runBlocks(blocksPerSecond);
    const double unisonMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - unisonStart).count();
    std::printf("      16 voices x 6 operators x 8 unison: 1 s of audio in %.1f ms (%.1f %% of one core)\n", unisonMs,
                unisonMs / 10.0);
    check(std::isfinite(unisonPeak), "full unison stays finite");
    runBlock({&chokeAll.header});

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);

    // --- Editor: embed it in a (hidden) host window, paint, drag the Volume knob, close.
    const int volume = paramIndex(GlobalParam::Volume);
    const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(gui && gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, false) &&
              !gui->is_api_supported(plugin, CLAP_WINDOW_API_WIN32, true),
          "gui extension offers an embedded Win32 editor");
    if (gui) {
        HWND parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr, nullptr,
                                      nullptr, nullptr);
        uint32_t width = 0, height = 0;
        bool opened = gui->create(plugin, CLAP_WINDOW_API_WIN32, false) && gui->get_size(plugin, &width, &height) &&
                      width > 0 && height > 0;
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = parent;
        opened = opened && gui->set_parent(plugin, &window) && gui->show(plugin);
        check(opened, "editor opens inside a host window");

        uint32_t adjustedWidth = 900, adjustedHeight = 100;
        gui->adjust_size(plugin, &adjustedWidth, &adjustedHeight);
        check(adjustedWidth == 900 && adjustedHeight == 774 && gui->set_size(plugin, adjustedWidth, adjustedHeight),
              "resizing keeps the aspect ratio");

        // At 900 px wide the view is drawn at 0.75 scale; the Volume knob sits at (204, 32) in view units.
        HWND editor = GetWindow(parent, GW_CHILD);
        double volumeBefore = 0.0, volumeAfter = 0.0;
        params->get_value(plugin, idOfIndex(volume), &volumeBefore);
        out.paramEventTypes.clear();
        if (editor) {
            SendMessageW(editor, WM_PAINT, 0, 0);
            pumpMessages(100);
            SendMessageW(editor, WM_MOUSEMOVE, 0, MAKELPARAM(153, 24));
            SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(153, 24));
            SendMessageW(editor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(153, 4));
            SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(153, 4));
            SendMessageW(editor, WM_PAINT, 0, 0);
            pumpMessages(100);
        }
        params->get_value(plugin, idOfIndex(volume), &volumeAfter);
        in.events.clear();
        params->flush(plugin, &in.list, &out.list); // the host collects the editor's edits
        const std::vector<uint16_t> expected = {CLAP_EVENT_PARAM_GESTURE_BEGIN, CLAP_EVENT_PARAM_VALUE,
                                                CLAP_EVENT_PARAM_GESTURE_END};
        check(editor && volumeAfter > volumeBefore + 5.0 && out.paramEventTypes == expected,
              "dragging a knob changes the param and tells the host (begin, value, end)");

        gui->hide(plugin);
        gui->destroy(plugin);
        DestroyWindow(parent);
        check(!IsWindow(editor), "closing the editor destroys its window");
    }

    // --- State: save, change a param through flush(), load, expect the saved value back.
    MemoryStream saved;
    check(state->save(plugin, &saved.out), "save state");

    const clap_id volumeId = idOfIndex(volume);
    double before = 0.0, changed = 0.0, restored = 0.0;
    params->get_value(plugin, volumeId, &before);
    const clap_event_param_value_t loud = paramEvent(volume, 6.0);
    in.events = {&loud.header};
    params->flush(plugin, &in.list, &out.list);
    params->get_value(plugin, volumeId, &changed);
    check(changed == 6.0 && changed != before, "flush() applies a param change");

    check(state->load(plugin, &saved.in), "load state");
    params->get_value(plugin, volumeId, &restored);
    check(restored == before, "load restores the saved value");

    MemoryStream garbage;
    garbage.data = {1, 2, 3, 4, 5, 6, 7};
    check(!state->load(plugin, &garbage.in), "load rejects a foreign blob");

    // A patch with a delay and a shaper in the first two slots, as a host hands it
    // back: (id, value) pairs, in any order and not necessarily all of them.
    {
        MemoryStream patch;
        auto put = [&](const void *data, size_t size) {
            const auto *bytes = static_cast<const uint8_t *>(data);
            patch.data.insert(patch.data.end(), bytes, bytes + size);
        };
        const std::pair<int, double> fields[] = {
            {paramIndex(1, FxParam::ShaperDrive), 24.0},
            {paramIndex(1, FxParam::Type), static_cast<double>(FxType::Shaper)},
            {paramIndex(0, FxParam::Type), static_cast<double>(FxType::Delay)},
            {paramIndex(0, FxParam::DelayTimeR), normFromTime(0.25)},
            {paramIndex(9, SourceParam::Type), static_cast<double>(SourceType::Env)},
            {paramIndex(2, SourceParam::Type), static_cast<double>(SourceType::Lfo)},
            {paramIndex(2, SourceParam::Rate), normFromRate(5.0)},
        };
        const uint32_t header[] = {0x38454C43, 1, static_cast<uint32_t>(std::size(fields))};
        put(header, sizeof(header));
        for (const auto &[index, value] : fields) {
            const uint32_t id = idOfIndex(index);
            put(&id, sizeof(id));
            put(&value, sizeof(value));
        }
        auto get = [&](int index) {
            double value = 0.0;
            params->get_value(plugin, idOfIndex(index), &value);
            return value;
        };
        const bool loaded = state->load(plugin, &patch.in);
        plugin->on_main_thread(plugin); // the host would, after the plugin asked for it
        check(loaded && get(paramIndex(0, FxParam::Type)) == static_cast<double>(FxType::Delay) &&
                  std::abs(get(paramIndex(0, FxParam::DelayTimeR)) - normFromTime(0.25)) < 1e-9 &&
                  get(paramIndex(1, FxParam::Type)) == static_cast<double>(FxType::Shaper) &&
                  get(paramIndex(1, FxParam::ShaperDrive)) == 24.0 &&
                  get(paramIndex(2, FxParam::Type)) == static_cast<double>(FxType::None) &&
                  get(paramIndex(2, SourceParam::Type)) == static_cast<double>(SourceType::Lfo) &&
                  std::abs(get(paramIndex(2, SourceParam::Rate)) - normFromRate(5.0)) < 1e-9 &&
                  get(paramIndex(9, SourceParam::Type)) == static_cast<double>(SourceType::Env) &&
                  get(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::None),
              "a patch loads its effect slots and sources; params it leaves out keep their defaults");

        MemoryStream newer;
        newer.data = patch.data;
        newer.data[4] = 2; // a state version from the future
        check(!state->load(plugin, &newer.in), "load rejects a newer state version");

        // Hosts see a slot's fields for the effect it holds, and not the others.
        auto hidden = [&](int index) {
            clap_param_info_t info{};
            params->get_info(plugin, static_cast<uint32_t>(index), &info);
            return (info.flags & CLAP_PARAM_IS_HIDDEN) != 0;
        };
        check(!hidden(paramIndex(0, FxParam::DelayFeedback)) && hidden(paramIndex(0, FxParam::ShaperDrive)) &&
                  !hidden(paramIndex(2, FxParam::Type)) && hidden(paramIndex(2, FxParam::Enabled)),
              "an effect slot's unused fields are hidden from hosts");
        uint32_t fxPages = 0;
        for (uint32_t i = 0; i < remote->count(plugin); ++i) {
            clap_remote_controls_page_t page;
            remote->get(plugin, i, &page);
            fxPages += std::strcmp(page.section_name, "FX") == 0;
        }
        check(fxPages == 2, "each effect in the chain gets a remote control page");

        // Likewise a source place's fields for the type it holds, and a page per source,
        // named by type in the order they show.
        check(!hidden(paramIndex(2, SourceParam::Rate)) && hidden(paramIndex(2, SourceParam::Attack)) &&
                  !hidden(paramIndex(9, SourceParam::Attack)) && hidden(paramIndex(9, SourceParam::Rate)) &&
                  !hidden(slotAmountIndex(9, 0)) && !hidden(paramIndex(0, SourceParam::Type)) &&
                  hidden(paramIndex(0, SourceParam::Enabled)) && hidden(slotAmountIndex(0, 0)),
              "a source place's unused fields are hidden from hosts");
        std::vector<std::string> sourcePages;
        for (uint32_t i = 0; i < remote->count(plugin); ++i) {
            clap_remote_controls_page_t page;
            remote->get(plugin, i, &page);
            if (std::strcmp(page.section_name, "Modulation") == 0)
                sourcePages.push_back(page.page_name);
        }
        check(sourcePages == std::vector<std::string>{"LFO 1", "Env 1"}, "each source gets a remote control page");
    }

    // Every remote control page has an id of its own, the macros' too, also with a source in place 0, whose page
    // goes by that place.
    {
        MemoryStream first;
        const uint32_t header[] = {0x38454C43, 1, 1};
        const uint32_t id = idOfIndex(paramIndex(0, SourceParam::Type));
        const double lfo = static_cast<double>(SourceType::Lfo);
        const auto *parts = reinterpret_cast<const uint8_t *>(header);
        first.data.assign(parts, parts + sizeof(header));
        first.data.insert(first.data.end(), reinterpret_cast<const uint8_t *>(&id),
                          reinterpret_cast<const uint8_t *>(&id) + sizeof(id));
        first.data.insert(first.data.end(), reinterpret_cast<const uint8_t *>(&lfo),
                          reinterpret_cast<const uint8_t *>(&lfo) + sizeof(lfo));
        const bool loaded = state->load(plugin, &first.in);
        plugin->on_main_thread(plugin);
        std::set<clap_id> pageIds;
        bool unique = true, sourcePage = false;
        for (uint32_t i = 0; i < remote->count(plugin); ++i) {
            clap_remote_controls_page_t page;
            remote->get(plugin, i, &page);
            unique = unique && pageIds.insert(page.page_id).second;
            sourcePage = sourcePage || std::strcmp(page.section_name, "Modulation") == 0;
        }
        check(loaded && sourcePage && unique, "every remote control page has an id of its own");
    }

    // A layout as large as one gets (every route kept at 0 %, every macro's name at full length: past 256 floats)
    // loads whole, and what follows it too: saved again, it comes back as it was, with the settings chunk's
    // oversampling after it.
    {
        std::vector<float> layout;
        for (int node = 0; node < 2 * kOps + 2 * (kFilters + kShapers); ++node) // the node places
            layout.push_back(0.05f * static_cast<float>(node + 1));
        layout.insert(layout.end(), {2.0f, 1.0f, 1.0f, 0.0f}); // selected node, zoom, the matrix, panel open
        for (int source = kMaxSources; source-- > 0;) // the sources the other way round
            layout.push_back(static_cast<float>(source));
        std::vector<int> routes;
        for (int node = 0; node < kOps; ++node)
            routes.push_back(paramIndex(node, OpParam::Out));
        for (int filter = 0; filter < kFilters; ++filter)
            routes.push_back(paramIndex(filter, FilterParam::Out));
        for (int from = 0; from < kOps; ++from) {
            for (int to = 0; to < kOps; ++to)
                routes.push_back(matrixIndex(from, to));
            for (int filter = 0; filter < kFilters; ++filter)
                routes.push_back(sendIndex(from, filter));
        }
        for (int filter = 0; filter < kFilters; ++filter) {
            for (int to = 0; to < kOps; ++to)
                routes.push_back(filterModIndex(filter, to));
            for (int to = filter + 1; to < kFilters; ++to)
                routes.push_back(filterLinkIndex(filter, to));
        }
        for (int shaper = 0; shaper < kShapers; ++shaper) {
            for (int op = 0; op < kOps; ++op)
                routes.push_back(shaperSendIndex(op, shaper));
            for (int to = 0; to < kOps; ++to)
                routes.push_back(shaperModIndex(shaper, to));
            for (int filter = 0; filter < kFilters; ++filter)
                routes.push_back(shaperFilterIndex(shaper, filter));
        }
        layout.push_back(static_cast<float>(routes.size()));
        for (int route : routes)
            layout.push_back(static_cast<float>(idOfIndex(route)));
        layout.insert(layout.end(), {24.0f, 6.0f}); // the shape editor's grid
        for (int macro = 0; macro < kMacros; ++macro) {
            layout.push_back(24.0f);
            for (int i = 0; i < 24; ++i)
                layout.push_back(static_cast<float>('a' + (macro + i) % 26));
        }
        layout.insert(layout.end(), {5.0f, 2.0f}); // folded cards

        MemoryStream big;
        auto put = [&](const void *data, size_t size) {
            const auto *bytes = static_cast<const uint8_t *>(data);
            big.data.insert(big.data.end(), bytes, bytes + size);
        };
        const uint32_t header[] = {0x38454C43, 1, 0};
        const uint32_t layoutChunk[] = {0x5459414C, static_cast<uint32_t>(layout.size())};
        const uint32_t settingsChunk[] = {0x47544553, 1};
        const float oversampling = 4.0f;
        put(header, sizeof(header));
        put(layoutChunk, sizeof(layoutChunk));
        put(layout.data(), layout.size() * sizeof(float));
        put(settingsChunk, sizeof(settingsChunk));
        put(&oversampling, sizeof(oversampling));
        const bool loaded = state->load(plugin, &big.in);
        plugin->on_main_thread(plugin);
        MemoryStream again;
        const bool savedAgain = state->save(plugin, &again.out);
        // Past the params: the layout chunk, then the settings chunk.
        size_t at = 12 + static_cast<size_t>(kParamCount) * 12;
        auto take = [&](void *out, size_t size) {
            const bool ok = at + size <= again.data.size();
            if (ok)
                std::memcpy(out, again.data.data() + at, size);
            at += size;
            return ok;
        };
        uint32_t chunk[2] = {};
        std::vector<float> back;
        bool whole = take(chunk, sizeof(chunk)) && chunk[0] == layoutChunk[0];
        if (whole) {
            back.resize(chunk[1]);
            whole = take(back.data(), back.size() * sizeof(float));
        }
        float factor = 0.0f;
        const bool settings = take(chunk, sizeof(chunk)) && chunk[0] == settingsChunk[0] && chunk[1] >= 1 &&
                              take(&factor, sizeof(factor));
        std::printf("      a layout chunk of %zu floats: %zu back, oversampling %.0f after it\n", layout.size(),
                    back.size(), factor);
        check(layout.size() > 256 && loaded && savedAgain && whole && back == layout && settings && factor == 4.0f,
              "a layout past 256 floats loads whole, and the settings after it too");

        // A chunk with more than this version knows (as a later one could write) keeps what it knows and skips
        // the rest, so the settings after it still load.
        MemoryStream longer;
        longer.data = big.data;
        const size_t end = 12 + 8 + layout.size() * sizeof(float); // where the settings chunk starts
        const uint32_t grown = static_cast<uint32_t>(layout.size()) + 5000;
        std::memcpy(longer.data.data() + 16, &grown, sizeof(grown));
        longer.data.insert(longer.data.begin() + static_cast<std::ptrdiff_t>(end), 5000 * sizeof(float), 0);
        const float twice = 2.0f; // a different oversampling, to tell it was read
        std::memcpy(longer.data.data() + end + 5000 * sizeof(float) + 8, &twice, sizeof(twice));
        const bool loadedLonger = state->load(plugin, &longer.in);
        MemoryStream third;
        state->save(plugin, &third.out);
        float thirdFactor = 0.0f;
        const size_t settingsAt = 12 + static_cast<size_t>(kParamCount) * 12 + 8 + layout.size() * sizeof(float) + 8;
        if (settingsAt + sizeof(float) <= third.data.size())
            std::memcpy(&thirdFactor, third.data.data() + settingsAt, sizeof(thirdFactor));
        check(loadedLonger && thirdFactor == 2.0f, "a layout chunk longer than this version knows is skipped past");
    }

    plugin->destroy(plugin);
    entry->deinit();
    FreeLibrary(lib);

    std::printf("%s\n", failures ? "SMOKE TEST FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
