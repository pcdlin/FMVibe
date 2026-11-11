// fmvibe-bench: what a build of the CLAP plugin costs, and what it plays, so that two builds can be compared (see
// tools/bench.ps1, which builds commits and compares them with it).
//
//   fmvibe-bench time <FMVibe.clap> [passes] [cpu]
//   fmvibe-bench render <FMVibe.clap> <presets folder>
//
// time: from a new instance at 48 kHz in blocks of 256, the CPU each load takes, in ms per second of audio, the best of
// `passes` seconds (5 to begin with), pinned to logical processor `cpu` if given. The loads: 16 notes held on the
// default patch (operator A alone: mostly what a voice costs a block apart from its samples); then the smoke test's
// load, all six operators on, each into the one before it at 30 % and out at 30 %, at 1 unison copy and at 8 spread
// ones; nothing playing; and the 1-copy load with every envelope decaying, then releasing (10 s each), the stages a
// plucked or let-go note spends its time in, which cost otherwise than the sustain.
//
// render: each factory preset in the folder (*.fmv) loaded into a new instance, and a chord of 8 notes over six
// octaves played into it for 3 s, let go half way: as saved, and again with every operator at 8 spread unison copies.
// Prints a line a preset: an FNV-1a hash of every sample of each render, the louder render's peak, and the preset's
// name; a build that plays the same gives the same hashes.
//
// It talks to the plugin through CLAP alone, with the param ids that host projects store (stable from release to
// release), so it measures older builds as well as this one.
#include "params.h"

#include <clap/clap.h>

#ifndef NOMINMAX // the build defines it, as for every target
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace fmvibe;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBlock = 256;

const void *hostExtension(const clap_host_t *, const char *) { return nullptr; }
void hostNothing(const clap_host_t *) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "fmvibe-bench", "", "", "1", hostExtension,
                           hostNothing,         hostNothing,           hostNothing};

struct EventList {
    std::vector<const clap_event_header_t *> events;
    static uint32_t size(const clap_input_events_t *list) {
        return static_cast<uint32_t>(static_cast<const EventList *>(list->ctx)->events.size());
    }
    static const clap_event_header_t *get(const clap_input_events_t *list, uint32_t i) {
        return static_cast<const EventList *>(list->ctx)->events[i];
    }
};
bool pushNothing(const clap_output_events_t *, const clap_event_header_t *) { return true; }

clap_event_note_t note(uint16_t type, int16_t key, int32_t id, uint32_t time = 0) {
    clap_event_note_t ev{};
    ev.header = {sizeof(ev), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
    ev.note_id = id;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = key;
    ev.velocity = 0.8;
    return ev;
}

clap_event_param_value_t param(int index, double value) {
    clap_event_param_value_t ev{};
    ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    ev.param_id = idOfIndex(index);
    ev.note_id = -1;
    ev.port_index = ev.channel = ev.key = -1;
    ev.value = value;
    return ev;
}

// The plugin's DLL and its factory.
struct Module {
    HMODULE dll = nullptr;
    const clap_plugin_entry_t *entry = nullptr;
    const clap_plugin_factory_t *factory = nullptr;

    explicit Module(const char *path) {
        dll = LoadLibraryA(path);
        entry = dll ? reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(dll, "clap_entry")) : nullptr;
        if (entry && entry->init(path))
            factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    }
    ~Module() {
        if (factory)
            entry->deinit();
        if (dll)
            FreeLibrary(dll);
    }
};

// An instance, active and processing, and its output buffers.
class Instance {
public:
    explicit Instance(const Module &module)
        : plugin_(module.factory->create_plugin(module.factory, &kHost, "io.github.pcdlin.fmvibe")) {
        if (!plugin_)
            return;
        plugin_->init(plugin_);
        plugin_->activate(plugin_, kRate, 1, kBlock);
        plugin_->start_processing(plugin_);
    }
    ~Instance() {
        if (!plugin_)
            return;
        plugin_->stop_processing(plugin_);
        plugin_->deactivate(plugin_);
        plugin_->destroy(plugin_);
    }
    Instance(const Instance &) = delete;
    Instance &operator=(const Instance &) = delete;

    explicit operator bool() const { return plugin_ != nullptr; }
    const clap_plugin_t *plugin() const { return plugin_; }
    const float *left() const { return left_.data(); }
    const float *right() const { return right_.data(); }

    // One block, with `events` (in time order) in it.
    void block(const std::vector<const clap_event_header_t *> &events = {}) {
        EventList list{events};
        const clap_input_events_t in{&list, EventList::size, EventList::get};
        const clap_output_events_t out{nullptr, pushNothing};
        float *channels[2] = {left_.data(), right_.data()};
        clap_audio_buffer_t buffer{};
        buffer.data32 = channels;
        buffer.channel_count = 2;
        clap_process_t process{};
        process.steady_time = -1;
        process.frames_count = kBlock;
        process.audio_outputs = &buffer;
        process.audio_outputs_count = 1;
        process.in_events = &in;
        process.out_events = &out;
        plugin_->process(plugin_, &process);
    }

    // The fewest ms of CPU any of `passes` seconds of audio took, each timed on its own.
    double best(int passes) {
        const int blocks = static_cast<int>(kRate / kBlock);
        double least = 1e300;
        for (int pass = 0; pass < passes; ++pass) {
            const auto start = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b)
                block();
            least = std::min(least, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return least;
    }

private:
    const clap_plugin_t *plugin_;
    std::vector<float> left_ = std::vector<float>(kBlock), right_ = std::vector<float>(kBlock);
};

// Every operator's unison copies, spread across the stereo field.
void addUnison(std::vector<clap_event_param_value_t> &params, int copies) {
    for (int op = 0; op < kOps; ++op) {
        params.push_back(param(paramIndex(op, OpParam::Unison), copies));
        params.push_back(param(paramIndex(op, OpParam::UnisonSpread), copies > 1 ? 1.0 : 0.0));
    }
}

int timeLoads(const Module &module, int passes) {
    Instance instance(module);
    if (!instance)
        return 3;
    std::vector<clap_event_note_t> notes;
    for (int16_t i = 0; i < 16; ++i)
        notes.push_back(note(CLAP_EVENT_NOTE_ON, static_cast<int16_t>(48 + i), 100 + i));
    clap_event_note_t choke = note(CLAP_EVENT_NOTE_CHOKE, -1, -1);
    choke.port_index = choke.channel = -1;
    // Every note ended, the params set and the notes struck again, then a quarter of a second on: the attacks over.
    auto play = [&](const std::vector<clap_event_param_value_t> &params) {
        std::vector<const clap_event_header_t *> events{&choke.header};
        for (const auto &p : params)
            events.push_back(&p.header);
        for (const auto &n : notes)
            events.push_back(&n.header);
        instance.block(events);
        for (int b = 0; b < 47; ++b)
            instance.block();
    };

    play({});
    // A second of it untimed first: a core that has been idle (as tools/bench.ps1 leaves it, sampling the machine's
    // load between runs) runs at a lower clock until it has been busy for a moment, and the first load timed would pay.
    instance.best(1);
    const double light = instance.best(passes);

    std::vector<clap_event_param_value_t> chain;
    for (int op = 0; op < kOps; ++op) {
        chain.push_back(param(paramIndex(op, OpParam::Enabled), 1.0));
        chain.push_back(param(paramIndex(op, OpParam::Out), 0.3));
        if (op + 1 < kOps)
            chain.push_back(param(matrixIndex(op + 1, op), 0.3));
    }
    play(chain);
    const double one = instance.best(passes);

    std::vector<clap_event_param_value_t> wide = chain;
    addUnison(wide, 8);
    play(wide);
    const double eight = instance.best(passes);

    instance.block({&choke.header});
    for (int b = 0; b < 400; ++b) // past any tail
        instance.block();
    const double idle = instance.best(passes);

    // Envelope times are stored 0..1 on their log curve: 0.95 is past their 10 s cap, 0.3 some 20 ms.
    std::vector<clap_event_param_value_t> decaying = chain;
    addUnison(decaying, 1);
    for (int op = 0; op < kOps; ++op) {
        decaying.push_back(param(paramIndex(op, OpParam::Decay), 0.95));
        decaying.push_back(param(paramIndex(op, OpParam::Sustain), 0.0));
    }
    play(decaying);
    const double decay = instance.best(passes);

    std::vector<clap_event_param_value_t> releasing = chain;
    addUnison(releasing, 1);
    for (int op = 0; op < kOps; ++op) {
        releasing.push_back(param(paramIndex(op, OpParam::Decay), 0.3));
        releasing.push_back(param(paramIndex(op, OpParam::Sustain), 0.7));
        releasing.push_back(param(paramIndex(op, OpParam::Release), 0.95));
    }
    play(releasing);
    std::vector<clap_event_note_t> offs;
    for (int16_t i = 0; i < 16; ++i)
        offs.push_back(note(CLAP_EVENT_NOTE_OFF, static_cast<int16_t>(48 + i), 100 + i));
    std::vector<const clap_event_header_t *> offEvents;
    for (const auto &n : offs)
        offEvents.push_back(&n.header);
    instance.block(offEvents);
    const double release = instance.best(passes);

    std::printf("light %.2f one %.2f eight %.2f idle %.3f decay %.2f release %.2f\n", light, one, eight, idle, decay,
                release);
    return 0;
}

// A preset file's bytes, read as the host's state.
struct Bytes {
    std::vector<unsigned char> data;
    size_t at = 0;
    static int64_t read(const clap_istream_t *stream, void *buffer, uint64_t size) {
        Bytes &b = *static_cast<Bytes *>(stream->ctx);
        const size_t n = static_cast<size_t>(std::min<uint64_t>(size, b.data.size() - b.at));
        std::memcpy(buffer, b.data.data() + b.at, n);
        b.at += n;
        return static_cast<int64_t>(n);
    }
};

struct Rendered {
    uint64_t hash = 14695981039346656037ull; // FNV-1a's offset basis
    float peak = 0.0f;
    bool loaded = false;
};

Rendered renderPreset(const Module &module, const std::vector<unsigned char> &preset, int copies) {
    Rendered out;
    Instance instance(module);
    if (!instance)
        return out;
    Bytes bytes{preset};
    const clap_istream_t stream{&bytes, Bytes::read};
    const auto *state = static_cast<const clap_plugin_state_t *>(instance.plugin()->get_extension(instance.plugin(), CLAP_EXT_STATE));
    out.loaded = state && state->load(instance.plugin(), &stream);
    std::vector<clap_event_param_value_t> params;
    if (copies > 1)
        addUnison(params, copies);
    const int16_t keys[] = {36, 48, 55, 60, 64, 67, 72, 84};
    std::vector<clap_event_note_t> ons, offs;
    for (int k = 0; k < 8; ++k) {
        ons.push_back(note(CLAP_EVENT_NOTE_ON, keys[k], k, static_cast<uint32_t>(k * 17)));
        offs.push_back(note(CLAP_EVENT_NOTE_OFF, keys[k], k, static_cast<uint32_t>(k * 13)));
    }
    const int blocks = static_cast<int>(3.0 * kRate / kBlock);
    for (int b = 0; b < blocks; ++b) {
        std::vector<const clap_event_header_t *> events;
        if (b == 0) {
            for (const auto &p : params)
                events.push_back(&p.header);
            for (const auto &n : ons)
                events.push_back(&n.header);
        } else if (b == blocks / 2) {
            for (const auto &n : offs)
                events.push_back(&n.header);
        }
        instance.block(events);
        for (uint32_t s = 0; s < kBlock; ++s)
            for (const float sample : {instance.left()[s], instance.right()[s]}) {
                uint32_t bits;
                std::memcpy(&bits, &sample, sizeof(bits));
                for (int byte = 0; byte < 4; ++byte)
                    out.hash = (out.hash ^ ((bits >> (8 * byte)) & 0xFFu)) * 1099511628211ull;
                out.peak = std::max(out.peak, std::abs(sample));
            }
    }
    return out;
}

int renderPresets(const Module &module, const char *folder) {
    std::vector<std::filesystem::path> presets;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".fmv")
            presets.push_back(entry.path());
    if (error || presets.empty())
        return 4;
    std::sort(presets.begin(), presets.end());
    for (const auto &path : presets) {
        std::vector<unsigned char> preset;
        if (FILE *file = nullptr; _wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            for (int c; (c = std::fgetc(file)) != EOF;)
                preset.push_back(static_cast<unsigned char>(c));
            std::fclose(file);
        }
        const Rendered saved = renderPreset(module, preset, 1);
        const Rendered wide = renderPreset(module, preset, 8);
        std::printf("%016llx %016llx %.4f %s%s\n", static_cast<unsigned long long>(saved.hash),
                    static_cast<unsigned long long>(wide.hash), std::max(saved.peak, wide.peak),
                    saved.loaded ? "" : "(not loaded) ", path.stem().string().c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    const bool timing = argc >= 3 && std::strcmp(argv[1], "time") == 0;
    const bool rendering = argc >= 4 && std::strcmp(argv[1], "render") == 0;
    if (!timing && !rendering) {
        std::fprintf(stderr, "usage: fmvibe-bench time <FMVibe.clap> [passes] [cpu]\n"
                             "       fmvibe-bench render <FMVibe.clap> <presets folder>\n");
        return 2;
    }
    if (timing) {
        if (argc >= 5) // pinned, so the scheduler moving it between cores (or onto an efficiency core) costs nothing
            SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR{1} << std::atoi(argv[4]));
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
    const Module module(argv[2]);
    if (!module.factory) {
        std::fprintf(stderr, "fmvibe-bench: can't load %s\n", argv[2]);
        return 3;
    }
    return timing ? timeLoads(module, argc >= 4 ? std::max(1, std::atoi(argv[3])) : 5) : renderPresets(module, argv[3]);
}
