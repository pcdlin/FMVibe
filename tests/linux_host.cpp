// A small headless host for the Linux plugins: loads the built .clap and the .vst3 bundle's object, plays a note
// through each (MTS SysEx retuning it), saves and loads a state, and (where a display is there) opens the CLAP editor in a window of its own
// and pumps it a few times. Not a validator: a check that what is built loads and plays.

#include "mtstuning.h"

#include <clap/clap.h>

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

// The host's connection to the X server, open for the whole run as a host's is (null without a display). An X server
// with no client left resets, refusing connections for a moment: Xvfb's does, and the editor opens and closes one of
// its own as it is made, so a connection the host opened only then could find the server resetting.
Display *hostDisplay = nullptr;

int ignoreXError(Display *, XErrorEvent *) { return 0; }

// What a window shows, up to `width` × `height` from its top left, once it shows. Under a window manager (a desktop's,
// WSLg's) the host's window is shown a moment after it is mapped, and may sit partly off the screen: XGetImage on a
// window not yet viewable, or past the screen's edge, answers BadMatch, which Xlib's own handler ends the process on.
// So this waits for it to be viewable (up to 2 s, `pump` running the editor meanwhile, and a few frames more for it to
// paint what it was exposed for), reads only what is on the screen, and lets an error there pass. Null if it never
// shows; the image may be smaller than asked.
XImage *grabShown(Display *display, Window window, int width, int height, const std::function<void()> &pump) {
    XWindowAttributes attributes{};
    for (int wait = 0; wait < 200; ++wait) {
        if (XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable)
            break;
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (attributes.map_state != IsViewable)
        return nullptr;
    for (int frame = 0; frame < 8; ++frame) {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    XGetWindowAttributes(display, window, &attributes);
    int x = 0, y = 0;
    Window ignored = None;
    XTranslateCoordinates(display, window, DefaultRootWindow(display), 0, 0, &x, &y, &ignored);
    const int screenWidth = DisplayWidth(display, DefaultScreen(display));
    const int screenHeight = DisplayHeight(display, DefaultScreen(display));
    const int left = std::max(0, -x), top = std::max(0, -y);
    const int right = std::min({width, attributes.width, screenWidth - x});
    const int bottom = std::min({height, attributes.height, screenHeight - y});
    if (right - left < 8 || bottom - top < 8)
        return nullptr;
    XSync(display, False);
    int (*previous)(Display *, XErrorEvent *) = XSetErrorHandler(ignoreXError);
    XImage *image = XGetImage(display, window, left, top, static_cast<unsigned>(right - left),
                              static_cast<unsigned>(bottom - top), AllPlanes, ZPixmap);
    XSync(display, False);
    XSetErrorHandler(previous);
    return image;
}

// Whether an image holds more than one color: the editor's own drawing shows.
bool notFlat(XImage *image) {
    const unsigned long first = XGetPixel(image, 0, 0);
    for (int y = 0; y < image->height; y += 4)
        for (int x = 0; x < image->width; x += 4)
            if (XGetPixel(image, x, y) != first)
                return true;
    return false;
}

// --- CLAP ----------------------------------------------------------------------------------------------------------

struct ClapHost {
    clap_host_t host{};
    clap_host_timer_support_t timer{};
    const clap_plugin_t *plugin = nullptr;
    clap_plugin_timer_support_t *pluginTimer = nullptr;
    std::atomic<bool> callback{false};
    clap_id timerId = CLAP_INVALID_ID;

    ClapHost() {
        host.clap_version = CLAP_VERSION_INIT;
        host.host_data = this;
        host.name = "fmvibe-linux-host";
        host.vendor = "FMVibe";
        host.url = "";
        host.version = "1";
        host.get_extension = [](const clap_host_t *h, const char *id) -> const void * {
            auto *self = static_cast<ClapHost *>(h->host_data);
            if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
                return &self->timer;
            return nullptr;
        };
        host.request_restart = [](const clap_host_t *) {};
        host.request_process = [](const clap_host_t *) {};
        host.request_callback = [](const clap_host_t *h) {
            static_cast<ClapHost *>(h->host_data)->callback.store(true);
        };
        timer.register_timer = [](const clap_host_t *h, uint32_t, clap_id *id) {
            auto *self = static_cast<ClapHost *>(h->host_data);
            self->timerId = 1;
            *id = 1;
            return true;
        };
        timer.unregister_timer = [](const clap_host_t *h, clap_id) {
            static_cast<ClapHost *>(h->host_data)->timerId = CLAP_INVALID_ID;
            return true;
        };
    }
};

struct Events {
    std::vector<clap_event_note_t> notes;
    clap_input_events_t in{};
    uint32_t at = 0;

    Events() {
        in.ctx = this;
        in.size = [](const clap_input_events_t *list) {
            return static_cast<uint32_t>(static_cast<Events *>(list->ctx)->notes.size());
        };
        in.get = [](const clap_input_events_t *list, uint32_t index) -> const clap_event_header_t * {
            auto *self = static_cast<Events *>(list->ctx);
            return index < self->notes.size() ? &self->notes[index].header : nullptr;
        };
    }

    void note(bool on, int key, double velocity) {
        clap_event_note_t event{};
        event.header.size = sizeof event;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        event.header.time = 0;
        event.note_id = -1;
        event.port_index = 0;
        event.channel = 0;
        event.key = static_cast<int16_t>(key);
        event.velocity = velocity;
        notes.push_back(event);
    }
};

struct State {
    std::vector<uint8_t> bytes;
    clap_ostream_t out{};
    clap_istream_t in{};
    size_t at = 0;

    State() {
        out.ctx = this;
        out.write = [](const clap_ostream_t *stream, const void *buffer, uint64_t size) -> int64_t {
            auto *self = static_cast<State *>(stream->ctx);
            const auto *bytes = static_cast<const uint8_t *>(buffer);
            self->bytes.insert(self->bytes.end(), bytes, bytes + size);
            return static_cast<int64_t>(size);
        };
        in.ctx = this;
        in.read = [](const clap_istream_t *stream, void *buffer, uint64_t size) -> int64_t {
            auto *self = static_cast<State *>(stream->ctx);
            const size_t left = self->bytes.size() - std::min(self->at, self->bytes.size());
            const size_t take = static_cast<size_t>(std::min<uint64_t>(size, left));
            std::memcpy(buffer, self->bytes.data() + self->at, take);
            self->at += take;
            return static_cast<int64_t>(take);
        };
    }
};

double play(const clap_plugin_t *plugin, int key = 69, int blocks = 8) {
    Events events;
    events.note(true, key, 1.0);
    std::vector<float> left(512), right(512);
    float peak = 0.0f;
    for (int block = 0; block < blocks; ++block) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        float *channels[2] = {left.data(), right.data()};
        clap_audio_buffer_t audio{};
        audio.data32 = channels;
        audio.channel_count = 2;
        clap_process_t process{};
        process.frames_count = 512;
        process.steady_time = block * 512;
        process.audio_outputs = &audio;
        process.audio_outputs_count = 1;
        process.in_events = &events.in;
        process.out_events = nullptr;
        if (block == 1)
            events.note(false, key, 0.0);
        plugin->process(plugin, &process);
        for (float sample : left)
            peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

// MTS SysEx (MIDI Tuning Standard), a single note tuning change in real time to every device: A4 to 69 and 0x2000 of
// 0x4000 of a semitone, a quarter tone up.
const std::vector<uint8_t> kQuarterToneA4 = {0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00, 0x01, 0x45, 0x45, 0x40, 0x00, 0xF7};
// The same putting A4 back where it was.
const std::vector<uint8_t> kQuarterToneA4Back = {0xF0, 0x7F, 0x7F, 0x08, 0x02, 0x00, 0x01, 0x45, 0x45, 0x00, 0x00, 0xF7};

// The pitch of what came out, as a key (69 at 440 Hz), from its rising zero crossings.
double keyOf(const std::vector<float> &x, double rate) {
    std::vector<double> at;
    for (size_t i = 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
    if (at.size() < 2)
        return 0.0;
    return 69.0 + 12.0 * std::log2((at.size() - 1) * rate / (at.back() - at.front()) / 440.0);
}

// The pitch `key` plays through a CLAP plugin (as keyOf()), `sysex` sent before it in its first block; let go of and
// rung out after.
double clapPitch(const clap_plugin_t *plugin, int key, const std::vector<uint8_t> &sysex = {}) {
    struct List {
        std::vector<const clap_event_header_t *> events;
        clap_input_events_t in{};
    } list;
    list.in.ctx = &list;
    list.in.size = [](const clap_input_events_t *in) {
        return static_cast<uint32_t>(static_cast<List *>(in->ctx)->events.size());
    };
    list.in.get = [](const clap_input_events_t *in, uint32_t index) {
        const auto &events = static_cast<List *>(in->ctx)->events;
        return index < events.size() ? events[index] : nullptr;
    };
    clap_event_midi_sysex_t message{};
    message.header = {sizeof message, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
    message.buffer = sysex.data();
    message.size = static_cast<uint32_t>(sysex.size());
    clap_event_note_t on{}, off{};
    on.header = {sizeof on, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0};
    on.note_id = -1;
    on.key = static_cast<int16_t>(key);
    on.velocity = 1.0;
    off = on;
    off.header.type = CLAP_EVENT_NOTE_OFF;
    clap_output_events_t out{}; // the notes' ends, which go nowhere
    out.try_push = [](const clap_output_events_t *, const clap_event_header_t *) { return true; };
    std::vector<float> left(512), right(512), heard;
    constexpr int kHeld = 60, kBlocks = 160;
    for (int block = 0; block < kBlocks; ++block) {
        list.events.clear();
        if (block == 0) {
            if (!sysex.empty())
                list.events.push_back(&message.header);
            list.events.push_back(&on.header);
        } else if (block == kHeld) {
            list.events.push_back(&off.header);
        }
        float *channels[2] = {left.data(), right.data()};
        clap_audio_buffer_t audio{};
        audio.data32 = channels;
        audio.channel_count = 2;
        clap_process_t process{};
        process.frames_count = 512;
        process.steady_time = -1;
        process.audio_outputs = &audio;
        process.audio_outputs_count = 1;
        process.in_events = &list.in;
        process.out_events = &out;
        plugin->process(plugin, &process);
        if (block >= 8 && block < kHeld)
            heard.insert(heard.end(), left.begin(), left.end());
    }
    return keyOf(heard, 48000.0);
}

// The settings chunk's field `field` (see writeSettings() in src/state.cpp) in a state's bytes: where it is, or 0.
size_t settingAt(const std::vector<uint8_t> &bytes, size_t field) {
    const uint8_t magic[] = {'S', 'E', 'T', 'G'}; // kSettingsMagic, little-endian
    const auto at = std::search(bytes.begin(), bytes.end(), std::begin(magic), std::end(magic));
    const size_t count = static_cast<size_t>(at - bytes.begin()) + 4, first = count + 4;
    uint32_t fields = 0;
    if (at == bytes.end() || first > bytes.size())
        return 0;
    std::memcpy(&fields, bytes.data() + count, 4);
    return field < fields && first + 4 * (field + 1) <= bytes.size() ? first + 4 * field : 0;
}

// The field set to `value`.
bool setSetting(std::vector<uint8_t> &bytes, size_t field, float value) {
    const size_t at = settingAt(bytes, field);
    if (at == 0)
        return false;
    std::memcpy(bytes.data() + at, &value, 4);
    return true;
}

// MTS-ESP's client as the plugins hold it: with no master (none runs here), MTS SysEx retunes its own table, and the
// editor is told it does, and the scale's name; a message without its F0 the same; other SysEx nothing.
void testMtsTuning() {
    std::printf("== MTS-ESP client\n");
    fmvibe::MtsTuning none;
    check(none.status().source == fmvibe::gui::TuningSource::Standard && none.retune(69, 0) == 0.0,
          "with no master and no SysEx, the client plays 12-TET and says nothing tunes it");
    // A bulk tuning dump: the name, then each key's semitone and fraction of one; A4 a quarter tone up.
    std::vector<uint8_t> dump = {0xF0, 0x7E, 0x7F, 0x08, 0x01, 0x00};
    const char name[17] = "Quarter A4      ";
    dump.insert(dump.end(), name, name + 16);
    for (int key = 0; key < 128; ++key) {
        dump.push_back(static_cast<uint8_t>(key));
        dump.push_back(key == 69 ? 0x40 : 0x00);
        dump.push_back(0x00);
    }
    dump.push_back(0x00); // its checksum, which the client doesn't check
    dump.push_back(0xF7);
    for (bool framed : {true, false}) {
        fmvibe::MtsTuning tuning;
        tuning.sysEx(framed ? dump.data() : dump.data() + 1, framed ? dump.size() : dump.size() - 1);
        const fmvibe::gui::TuningStatus status = tuning.status();
        std::printf("       %s: '%s', A4 %.4f semitones, C4 %.4f\n", framed ? "a bulk dump" : "one without its F0",
                    status.scale.c_str(), tuning.retune(69, 0), tuning.retune(60, 0));
        check(status.source == fmvibe::gui::TuningSource::SysEx && status.scale == "Quarter A4" &&
                  std::abs(tuning.retune(69, 0) - 0.5) < 0.001 && std::abs(tuning.retune(60, 0)) < 0.001,
              framed ? "MTS SysEx retunes the client, the editor told so and the scale's name"
                     : "MTS SysEx without its F0 retunes the client the same");
    }
    fmvibe::MtsTuning other;
    const std::vector<uint8_t> identity = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}; // an identity request
    other.sysEx(identity.data(), identity.size());
    check(other.status().source == fmvibe::gui::TuningSource::Standard, "SysEx that isn't MTS tunes nothing");
}

// The user's settings file has a '.' before a fraction, whatever the locale: read in a host that has set one writing
// a ',' (as GTK and Qt apps set the desktop's), saved zooms of 0.6 and 0.75 still open the editor as wide as they say
// (both within the screen, which holds a larger one back), where atof() would have read 0, no zoom saved, for both.
// Left out where no such locale is installed.
void testSettingsLocale(const clap_plugin_factory_t *factory, const char *id) {
    char dir[] = "/tmp/fmvibe-settings-XXXXXX";
    if (!mkdtemp(dir)) {
        check(false, "a settings folder is made");
        return;
    }
    const std::string localeWas = std::setlocale(LC_ALL, nullptr);
    auto commaLocale = []() -> const char * {
        for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"})
            if (std::setlocale(LC_ALL, name) && std::localeconv()->decimal_point[0] == ',')
                return name;
        return nullptr;
    };
    const char *comma = commaLocale();
    const char *locpathWas = std::getenv("LOCPATH");
    const std::string keepLocpath = locpathWas ? locpathWas : "";
    if (!comma) { // none installed: one compiled into the folder, where glibc's localedef and its sources are
        const std::string made = std::string("localedef -i de_DE -f UTF-8 ") + dir + "/de_DE.UTF-8 >/dev/null 2>&1";
        if (std::system(made.c_str()) == 0) {
            setenv("LOCPATH", dir, 1);
            comma = commaLocale();
        }
    }
    auto restore = [&] {
        std::setlocale(LC_ALL, localeWas.c_str());
        if (locpathWas)
            setenv("LOCPATH", keepLocpath.c_str(), 1);
        else
            unsetenv("LOCPATH");
    };
    if (!comma) {
        restore();
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
        std::printf("       (no locale with a decimal comma to be had: the settings file's numbers aren't checked)\n");
        return;
    }
    const char *dirWas = std::getenv("FMVIBE_SETTINGS_DIR");
    const std::string keepDir = dirWas ? dirWas : "";
    setenv("FMVIBE_SETTINGS_DIR", dir, 1);
    auto width = [&](const char *zoom) {
        std::ofstream(std::filesystem::path(dir) / "settings.txt") << "zoom=" << zoom << "\n";
        ClapHost host;
        const clap_plugin_t *plugin = factory->create_plugin(factory, &host.host, id);
        uint32_t w = 0, h = 0;
        if (plugin && plugin->init(plugin)) {
            host.plugin = plugin;
            const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
            if (gui && gui->create(plugin, CLAP_WINDOW_API_X11, false)) {
                gui->get_size(plugin, &w, &h);
                gui->destroy(plugin);
            }
        }
        if (plugin)
            plugin->destroy(plugin);
        return w;
    };
    const uint32_t smaller = width("0.6"), larger = width("0.75");
    restore();
    if (dirWas)
        setenv("FMVIBE_SETTINGS_DIR", keepDir.c_str(), 1);
    else
        unsetenv("FMVIBE_SETTINGS_DIR");
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
    std::printf("       in %s, a saved zoom of 0.6 opens %u px wide, of 0.75 %u\n", comma, smaller, larger);
    check(larger > 0 && std::abs(static_cast<double>(smaller) - 0.8 * larger) <= 2.0,
          "a host's decimal comma leaves the settings file's numbers as they are written");
}

void testClap(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        check(false, "the .clap opens");
        std::printf("       %s\n", dlerror());
        return;
    }
    check(true, "the .clap opens");
    const auto *entry = static_cast<const clap_plugin_entry_t *>(dlsym(library, "clap_entry"));
    if (!entry || !entry->init("")) {
        check(false, "its entry takes");
        return;
    }
    const auto *factory = static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    check(factory && factory->get_plugin_count(factory) == 1, "one plugin in its factory");
    if (!factory)
        return;
    ClapHost host;
    const clap_plugin_descriptor_t *descriptor = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t *plugin = factory->create_plugin(factory, &host.host, descriptor->id);
    check(plugin != nullptr, "the plugin is made");
    if (!plugin)
        return;
    host.plugin = plugin;
    check(plugin->init(plugin), "it initializes");
    check(plugin->activate(plugin, 48000.0, 1, 512), "it activates");
    check(plugin->start_processing(plugin), "it starts processing");

    const auto *params = static_cast<const clap_plugin_params_t *>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    check(params && params->count(plugin) > 1000, "hosts are given its params");
    const auto *gui = static_cast<const clap_plugin_gui_t *>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    check(gui != nullptr, "it has an editor");

    const double peak = play(plugin);
    check(peak > 0.001, "a note sounds");
    std::printf("       peak %.3f\n", peak);
    {
        // MTS-ESP, off to begin with, ignores MTS SysEx; switched on in the project's settings, MTS SysEx retunes it;
        // off again, it plays 12-TET, and on again, the SysEx's tuning.
        const auto *tuneState = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
        auto settingTo = [&](float on) {
            State saved;
            State loaded;
            if (!tuneState || !tuneState->save(plugin, &saved.out))
                return false;
            loaded.bytes = saved.bytes;
            return setSetting(loaded.bytes, 5, on) && tuneState->load(plugin, &loaded.in);
        };
        const double ignored = clapPitch(plugin, 69, kQuarterToneA4);
        const bool firstOn = settingTo(1.0f);
        const double plain = clapPitch(plugin, 69, kQuarterToneA4Back), tuned = clapPitch(plugin, 69, kQuarterToneA4);
        const bool offLoaded = settingTo(0.0f);
        const double off = clapPitch(plugin, 69);
        State kept;
        float keptValue = 1.0f; // what a project saved now holds
        const bool keptOff = tuneState && tuneState->save(plugin, &kept.out) && settingAt(kept.bytes, 5) != 0 &&
                             (std::memcpy(&keptValue, kept.bytes.data() + settingAt(kept.bytes, 5), 4), keptValue == 0.0f);
        const bool onLoaded = settingTo(1.0f);
        const double on = clapPitch(plugin, 69);
        std::printf("       A4 as key %.3f, after MTS SysEx %.3f, MTS-ESP off %.3f, on again %.3f\n", plain, tuned, off,
                    on);
        check(std::abs(ignored - 69.0) < 0.01, "with MTS-ESP off, as to begin with, MTS SysEx tunes nothing");
        check(firstOn && std::abs(plain - 69.0) < 0.01 && std::abs(tuned - 69.5) < 0.01,
              "with MTS-ESP on, MTS SysEx retunes the CLAP plugin");
        check(offLoaded && keptOff && onLoaded && std::abs(off - 69.0) < 0.01 && std::abs(on - 69.5) < 0.01,
              "MTS-ESP switched off in the project's settings plays 12-TET, and on again the tuning");
    }

    State saved;
    const auto *state = static_cast<const clap_plugin_state_t *>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    check(state && state->save(plugin, &saved.out) && saved.bytes.size() > 1000, "its state saves");
    State loaded;
    loaded.bytes = saved.bytes;
    check(state && state->load(plugin, &loaded.in), "its state loads back");

    if (gui && hostDisplay) {
        check(gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false), "its editor takes an X11 window");
        check(gui->create(plugin, CLAP_WINDOW_API_X11, false), "its editor is made");
        uint32_t width = 0, height = 0;
        check(gui->get_size(plugin, &width, &height) && width > 100 && height > 100, "it has a size");
        Display *display = hostDisplay;
        const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1300, 1080, 0, 0, 0);
        XStoreName(display, parent, "FMVibe editor host");
        XMapWindow(display, parent);
        XSync(display, False);
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_X11;
        window.x11 = parent;
        check(gui->set_parent(plugin, &window), "it goes into a host's window");
        check(gui->show(plugin), "it shows");
        host.pluginTimer = static_cast<clap_plugin_timer_support_t *>(
            const_cast<void *>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT)));
        for (int frame = 0; frame < 8; ++frame) {
            if (host.pluginTimer)
                host.pluginTimer->on_timer(plugin, 1);
            if (host.callback.exchange(false))
                plugin->on_main_thread(plugin);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        check(host.timerId != CLAP_INVALID_ID, "its editor took the host's timer");
        // The editor's child window has painted: its pixels aren't one flat color (the view's own drawing shows).
        Window root = None, above = None, child = None, *children = nullptr;
        unsigned count = 0;
        if (XQueryTree(display, parent, &root, &above, &children, &count) && children) {
            if (count > 0)
                child = children[count - 1]; // the topmost child: the editor's window
            XFree(children);
        }
        bool painted = false;
        if (child) {
            const auto pumpClap = [&] {
                if (host.pluginTimer)
                    host.pluginTimer->on_timer(plugin, 1);
                if (host.callback.exchange(false))
                    plugin->on_main_thread(plugin);
            };
            if (XImage *image = grabShown(display, child, 200, 200, pumpClap)) {
                painted = notFlat(image);
                XDestroyImage(image);
            }
            // FMVIBE_SHOT=file.ppm saves the whole editor: a look at what the host's window shows.
            if (const char *shot = std::getenv("FMVIBE_SHOT")) {
                if (XImage *image = grabShown(display, child, 900, 700, pumpClap)) {
                    const int shotWidth = image->width, shotHeight = image->height;
                    if (FILE *file = std::fopen(shot, "wb")) {
                        std::fprintf(file, "P6\n%d %d\n255\n", shotWidth, shotHeight);
                        for (int y = 0; y < shotHeight; ++y)
                            for (int x = 0; x < shotWidth; ++x) {
                                const unsigned long pixel = XGetPixel(image, x, y);
                                const unsigned char rgb[3] = {static_cast<unsigned char>((pixel >> 16) & 0xFF),
                                                              static_cast<unsigned char>((pixel >> 8) & 0xFF),
                                                              static_cast<unsigned char>(pixel & 0xFF)};
                                std::fwrite(rgb, 1, 3, file);
                            }
                        std::fclose(file);
                    }
                    XDestroyImage(image);
                }
            }
        }
        check(painted, "its editor paints");
        gui->hide(plugin);
        gui->destroy(plugin);
        XDestroyWindow(display, parent);
        XSync(display, False);

        // Again, as a host may: a scale given before the window (CLAP's order, set_scale then get_size), and the
        // host's window destroyed before the editor. Xlib's own error handler, which ends the process, is the host's:
        // the editor's errors on its window gone (BadWindow) must not reach it.
        XSetErrorHandler(nullptr);
        check(gui->create(plugin, CLAP_WINDOW_API_X11, false), "its editor is made again");
        uint32_t scaledWidth = 0, scaledHeight = 0;
        check(gui->set_scale(plugin, 0.5) && gui->get_size(plugin, &scaledWidth, &scaledHeight) &&
                  scaledWidth < width && scaledHeight < height,
              "a scale given before its window sizes the editor");
        const Window doomed = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1300, 1080, 0, 0, 0);
        XMapWindow(display, doomed);
        XSync(display, False);
        window.x11 = doomed;
        check(gui->set_parent(plugin, &window) && gui->show(plugin), "it goes into a host's window again");
        for (int frame = 0; frame < 3; ++frame) {
            if (host.pluginTimer)
                host.pluginTimer->on_timer(plugin, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        Window scaledChild = None;
        if (XQueryTree(display, doomed, &root, &above, &children, &count) && children) {
            if (count > 0)
                scaledChild = children[count - 1];
            XFree(children);
        }
        XWindowAttributes scaled{};
        check(scaledChild && XGetWindowAttributes(display, scaledChild, &scaled) &&
                  static_cast<uint32_t>(scaled.width) == scaledWidth,
              "its window opens at the size the scale gave, not the desktop's");
        XDestroyWindow(display, doomed); // the host's window goes first, the editor's child with it
        XSync(display, False);
        if (host.pluginTimer)
            host.pluginTimer->on_timer(plugin, 1); // the editor's events, on a window that is gone
        gui->destroy(plugin);
        check(true, "an editor whose host window went first closes, the host still running");
    }

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    if (hostDisplay)
        testSettingsLocale(factory, descriptor->id);
    entry->deinit();
    check(true, "it closes");
    dlclose(library);
}

// --- VST3 ----------------------------------------------------------------------------------------------------------

using namespace Steinberg;
using namespace Steinberg::Vst;

// What a Linux host gives a VST3 editor: a run loop with the display's socket and a timer, which here is fired by
// hand. The editor is pumped by that timer, so this checks the path Bitwig takes.
class TestRunLoop final : public Steinberg::Linux::IRunLoop {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, Steinberg::Linux::IRunLoop)
        QUERY_INTERFACE(iid, obj, Steinberg::Linux::IRunLoop::iid, Steinberg::Linux::IRunLoop)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override { return --refs_; } // one reference, for the whole test
    tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                            Steinberg::Linux::FileDescriptor fd) override {
        event_ = handler;
        fd_ = fd;
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler) override {
        if (event_ == handler)
            event_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler, uint64_t) override {
        timer_ = handler;
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler) override {
        if (timer_ == handler)
            timer_ = nullptr;
        return kResultOk;
    }
    bool tick() {
        if (!timer_)
            return false;
        timer_->onTimer();
        return true;
    }
    // The file descriptor it watches has something to read, as the host's run loop finds.
    bool fire() {
        if (!event_)
            return false;
        event_->onFDIsSet(fd_);
        return true;
    }
    bool watching() const { return event_ != nullptr; }

private:
    std::atomic<uint32> refs_{1};
    int fd_ = -1;
    Steinberg::Linux::IEventHandler *event_ = nullptr;
    Steinberg::Linux::ITimerHandler *timer_ = nullptr;
};

class TestFrame final : public IPlugFrame {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IPlugFrame)
        QUERY_INTERFACE(iid, obj, IPlugFrame::iid, IPlugFrame)
        if (FUnknownPrivate::iidEqual(iid, Steinberg::Linux::IRunLoop::iid)) {
            runLoop.addRef();
            *obj = &runLoop;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override { return --refs_; }
    tresult PLUGIN_API resizeView(IPlugView *, ViewRect *) override { return kResultOk; }

    TestRunLoop runLoop;

private:
    std::atomic<uint32> refs_{1};
};

// A host application that offers its run loop as the context it initializes a plugin with, as some Linux hosts do.
class TestHostContext final : public FUnknown {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, FUnknown)
        if (FUnknownPrivate::iidEqual(iid, Steinberg::Linux::IRunLoop::iid)) {
            runLoop.addRef();
            *obj = &runLoop;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    TestRunLoop runLoop;
};

// Records the restarts a plugin asks for, and on which thread.
class TestHandler final : public IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(iid, obj, IComponentHandler::iid, IComponentHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restarts.fetch_or(flags);
        if (std::this_thread::get_id() != mainThread)
            offMain.store(true);
        return kResultOk;
    }

    std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<int32> restarts{0};
    std::atomic<bool> offMain{false};
};

// A state's bytes in memory.
// A block's events, for the processor.
class TestEvents final : public IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IEventList)
        QUERY_INTERFACE(iid, obj, IEventList::iid, IEventList)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Event &e) override {
        if (index < 0 || index >= getEventCount())
            return kInvalidArgument;
        e = events[static_cast<size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Event &e) override {
        events.push_back(e);
        return kResultOk;
    }

    std::vector<Event> events;
};

class MemoryStream final : public IBStream {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
        QUERY_INTERFACE(iid, obj, FUnknown::iid, IBStream)
        QUERY_INTERFACE(iid, obj, IBStream::iid, IBStream)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API read(void *buffer, int32 bytes, int32 *done) override {
        const int32 n = std::max<int32>(0, std::min<int32>(bytes, static_cast<int32>(data.size() - at)));
        std::memcpy(buffer, data.data() + at, static_cast<size_t>(n));
        at += static_cast<size_t>(n);
        if (done)
            *done = n;
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 bytes, int32 *done) override {
        const auto *from = static_cast<const uint8_t *>(buffer);
        data.insert(data.begin() + static_cast<std::ptrdiff_t>(at), from, from + bytes);
        at += static_cast<size_t>(bytes);
        if (done)
            *done = bytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64 *result) override {
        const int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? static_cast<int64>(at) : static_cast<int64>(data.size());
        at = static_cast<size_t>(std::clamp<int64>(base + pos, 0, static_cast<int64>(data.size())));
        if (result)
            *result = static_cast<int64>(at);
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *pos) override {
        if (pos)
            *pos = static_cast<int64>(at);
        return kResultOk;
    }

    std::vector<uint8_t> data;
    size_t at = 0;
};

// A state handed over with no view open asks the host to read the values again (kParamValuesChanged) on its main
// thread: through the host application's run loop where it offers one, only once that run loop finds the wake; and
// without one, through the plugin's own thread, after setState() has returned.
void testVst3Wake(IPluginFactory *factory, const TUID cid) {
    for (const bool hostLoop : {true, false}) {
        FUnknown *unknown = nullptr;
        if (factory->createInstance(cid, IComponent::iid, reinterpret_cast<void **>(&unknown)) != kResultOk) {
            check(false, "a second component is made");
            return;
        }
        auto *component = static_cast<IComponent *>(unknown);
        TestHostContext context;
        component->initialize(hostLoop ? static_cast<FUnknown *>(&context) : nullptr);
        IEditController *edit = nullptr;
        component->queryInterface(IEditController::iid, reinterpret_cast<void **>(&edit));
        TestHandler handler;
        if (edit)
            edit->setComponentHandler(&handler);
        MemoryStream state;
        component->getState(&state);
        state.at = 0;
        const bool loaded = component->setState(&state) == kResultOk;
        if (hostLoop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250)); // no thread of its own to do it meanwhile
            const bool waited = handler.restarts.load() == 0;
            const bool fired = context.runLoop.fire();
            check(loaded && context.runLoop.watching() && waited && fired &&
                      (handler.restarts.load() & kParamValuesChanged) && !handler.offMain.load(),
                  "with no view open, a state's restart comes through the host application's run loop, on its main "
                  "thread");
        } else {
            for (int k = 0; k < 100 && !(handler.restarts.load() & kParamValuesChanged); ++k)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(loaded && (handler.restarts.load() & kParamValuesChanged),
                  "with no run loop, a state's restart comes through the plugin's own thread");
        }
        if (edit) {
            edit->setComponentHandler(nullptr);
            edit->release();
        }
        component->terminate();
        check(!context.runLoop.watching(), "terminated, it leaves the host's run loop");
        component->release();
    }
}

void testVst3(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        check(false, "the VST3 object opens");
        std::printf("       %s\n", dlerror());
        return;
    }
    check(true, "the VST3 object opens");
    using GetFactory = IPluginFactory *(*)();
    const auto getFactory = reinterpret_cast<GetFactory>(dlsym(library, "GetPluginFactory"));
    IPluginFactory *factory = getFactory ? getFactory() : nullptr;
    check(factory != nullptr, "its factory is there");
    if (!factory) {
        dlclose(library);
        return;
    }
    PClassInfo info{};
    check(factory->getClassInfo(0, &info) == kResultOk && factory->countClasses() == 1, "one class in its factory");
    // What a strict scanner reads before it loads anything: the class' name, category and ids, in both forms.
    PClassInfo2 info2{};
    PClassInfoW infoW{};
    IPluginFactory2 *factory2 = nullptr;
    IPluginFactory3 *factory3 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void **>(&factory2));
    factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void **>(&factory3));
    const bool forms = factory2 && factory3 && factory2->getClassInfo2(0, &info2) == kResultOk &&
                       factory3->getClassInfoUnicode(0, &infoW) == kResultOk;
    check(forms, "its class reads in both forms");
    if (factory2)
        factory2->release();
    if (factory3)
        factory3->release();
    check(std::strcmp(info.category, kVstAudioEffectClass) == 0 && info.name[0] != 0 && info2.vendor[0] != 0 &&
              infoW.name[0] != 0 && std::memcmp(info.cid, info2.cid, sizeof(TUID)) == 0,
          "its class says what it is");
    FUnknown *unknown = nullptr;
    check(factory->createInstance(info.cid, IComponent::iid, reinterpret_cast<void **>(&unknown)) == kResultOk,
          "the component is made");
    auto *component = static_cast<IComponent *>(unknown);
    if (!component) {
        dlclose(library);
        return;
    }
    check(component->initialize(nullptr) == kResultOk, "it initializes");
    {
        // MTS-ESP, off to begin with, switched on through the state's settings chunk, for the MTS SysEx below.
        MemoryStream state;
        const bool saved = component->getState(&state) == kResultOk && setSetting(state.data, 5, 1.0f);
        state.at = 0;
        check(saved && component->setState(&state) == kResultOk, "a state's settings chunk switches MTS-ESP on");
    }
    IAudioProcessor *processor = nullptr;
    check(component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void **>(&processor)) == kResultOk,
          "it is a processor");
    if (processor) {
        ProcessSetup setup{};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 512;
        setup.sampleRate = 48000.0;
        check(processor->setupProcessing(setup) == kResultOk, "it sets up");
        check(component->setActive(true) == kResultOk, "it activates");
        check(processor->setProcessing(true) == kResultOk, "it processes");
        std::vector<float> left(512), right(512);
        float *channels[2] = {left.data(), right.data()};
        AudioBusBuffers output{};
        output.numChannels = 2;
        output.channelBuffers32 = channels;
        ProcessData data{};
        data.symbolicSampleSize = kSample32;
        data.numSamples = 512;
        data.numOutputs = 1;
        data.outputs = &output;
        data.processContext = nullptr;
        float peak = 0.0f;
        for (int block = 0; block < 4; ++block) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            processor->process(data);
            for (float sample : left)
                peak = std::max(peak, std::fabs(sample));
        }
        check(peak >= 0.0f, "a block goes through"); // silence without a note: the check is that it doesn't crash
        // MTS SysEx in a data event retunes it, as it does the CLAP plugin.
        {
            TestEvents events;
            data.inputEvents = &events;
            Event message{};
            message.type = Event::kDataEvent;
            message.data.type = DataEvent::kMidiSysEx;
            message.data.bytes = kQuarterToneA4.data();
            message.data.size = static_cast<uint32>(kQuarterToneA4.size());
            Event on{};
            on.type = Event::kNoteOnEvent;
            on.noteOn.pitch = 69;
            on.noteOn.velocity = 1.0f;
            on.noteOn.noteId = -1;
            Event off{};
            off.type = Event::kNoteOffEvent;
            off.noteOff.pitch = 69;
            off.noteOff.noteId = -1;
            std::vector<float> heard;
            for (int block = 0; block < 160; ++block) {
                events.events.clear();
                if (block == 0) {
                    events.events.push_back(message);
                    events.events.push_back(on);
                } else if (block == 60) {
                    events.events.push_back(off);
                }
                processor->process(data);
                if (block >= 8 && block < 60)
                    heard.insert(heard.end(), left.begin(), left.end());
            }
            data.inputEvents = nullptr;
            const double tuned = keyOf(heard, 48000.0);
            std::printf("       A4 after MTS SysEx in a data event: key %.3f\n", tuned);
            check(std::abs(tuned - 69.5) < 0.01, "MTS SysEx in a VST3 data event retunes the plugin");
        }
        processor->setProcessing(false);

        // The editor, as Bitwig opens it: a view in an X11 host window, pumped by the run loop's timer.
        IEditController *edit = nullptr;
        component->queryInterface(IEditController::iid, reinterpret_cast<void **>(&edit));
        IPlugView *view = edit ? edit->createView(ViewType::kEditor) : nullptr;
        check(view != nullptr, "its VST3 editor view is made");
        if (view && hostDisplay) {
            TestFrame frame;
            check(view->setFrame(&frame) == kResultOk, "the host's frame takes");
            check(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue,
                  "the view takes an X11 window");
            Display *display = hostDisplay;
            const Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1300, 1080, 0, 0, 0);
            XMapWindow(display, parent);
            XSync(display, False);
            check(view->attached(reinterpret_cast<void *>(static_cast<uintptr_t>(parent)),
                                 kPlatformTypeX11EmbedWindowID) == kResultOk,
                  "the view goes into the host's window");
            bool pumped = false;
            for (int tick = 0; tick < 8; ++tick) {
                pumped = frame.runLoop.tick() || pumped;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            check(pumped, "the host's run loop pumps the editor");
            Window root = None, above = None, child = None, *children = nullptr;
            unsigned count = 0;
            if (XQueryTree(display, parent, &root, &above, &children, &count) && children) {
                if (count > 0)
                    child = children[count - 1];
                XFree(children);
            }
            bool painted = false;
            if (child) {
                if (XImage *image = grabShown(display, child, 200, 200, [&] { frame.runLoop.tick(); })) {
                    painted = notFlat(image);
                    XDestroyImage(image);
                }
            }
            check(painted, "the VST3 editor paints");
            view->removed();
            view->setFrame(nullptr); // while the frame and its run loop are alive, as a host does
            XDestroyWindow(display, parent);
            XSync(display, False);
        }
        if (view) {
            view->setFrame(nullptr);
            view->release();
        }
        if (edit)
            edit->release();
        component->setActive(false);
        processor->release();
    }
    check(component->terminate() == kResultOk, "it terminates");
    component->release();
    testVst3Wake(factory, info.cid);
    dlclose(library);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <FMVibe.clap> <FMVibe.so>\n", argv[0]);
        return 2;
    }
    testMtsTuning();
    if (std::getenv("DISPLAY")) {
        hostDisplay = XOpenDisplay(nullptr);
        check(hostDisplay != nullptr, "the host connects to the display");
    }
    std::printf("== CLAP\n");
    testClap(argv[1]);
    std::printf("== VST3\n");
    testVst3(argv[2]);
    if (hostDisplay)
        XCloseDisplay(hostDisplay);
    std::printf(failures == 0 ? "all checks passed\n" : "%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
