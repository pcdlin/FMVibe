// fmvibe-mtsmaster: a small MTS-ESP master, for trying FMVibe (or any MTS-ESP client) against a real master by hand,
// on Windows and Linux. It registers through ODDSound's master library (third_party/mts-esp), which needs libMTS where
// an MTS-ESP master installs it, and tunes every client on the machine to one of a few scenes, each a case FMVibe's
// MTS-ESP support has to handle: equal divisions other than 12, just and meantone tunings, a stretched octave, unmapped
// keys, a second MIDI channel tuned apart, a tuning that keeps moving, and a master that comes and goes.
//
//   fmvibe-mtsmaster [scene] [--for <seconds>] [--reinit] [--list]
//
// Then, one a line on its input: a scene's name, `off` (deregister, as a master closed), `on` (register again),
// `status`, `list` or `quit`. It quits at the end of its input, after --for's seconds, or on Ctrl+C, deregistering.
// Not a test: while it runs, it retunes every MTS-ESP client on the machine, a DAW's too.

#include "../third_party/mts-esp/libMTSMaster.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace {

using Table = std::array<double, 128>;

double tet(int key) {
    return 440.0 * std::exp2((key - 69) / 12.0);
}

// Each key `steps` of `cents` apart, A4 (key 69) at 440 Hz.
Table equal(double cents) {
    Table t{};
    for (int key = 0; key < 128; ++key)
        t[key] = 440.0 * std::exp2((key - 69) * cents / 1200.0);
    return t;
}

// An octave's twelve keys from C at `cents` above C (C4, key 60, at its 12-TET pitch), repeating each octave.
Table octaveOf(const double (&cents)[12]) {
    Table t{};
    for (int key = 0; key < 128; ++key) {
        const int octave = key / 12 - 5, degree = key % 12;
        t[key] = tet(60) * std::exp2(octave + cents[degree] / 1200.0);
    }
    return t;
}

struct Scene {
    const char *name;  // what to type
    const char *scale; // the scale's name, which clients show
    const char *what;
    // The table at `seconds` since the scene began (most stand still), the keys it leaves unmapped, and, for channel
    // 2 (index 1), a table of its own.
    Table (*table)(double seconds);
    bool (*unmapped)(int key) = nullptr;
    Table (*channel2)() = nullptr;
    bool moves = false;
};

const Scene kScenes[] = {
    {"12tet", "12-TET", "standard tuning, as without a master", [](double) { return equal(100.0); }},
    {"19edo", "19-EDO", "19 equal steps to the octave, A4 at 440 Hz (a major third 6 steps, a fifth 11)",
     [](double) { return equal(1200.0 / 19.0); }},
    {"24edo", "24-EDO", "quarter tones, A4 at 440 Hz (a neutral third 7 steps, a fifth 14)",
     [](double) { return equal(50.0); }},
    {"31edo", "31-EDO", "31 equal steps to the octave, A4 at 440 Hz (a major third 10 steps, a fifth 18)",
     [](double) { return equal(1200.0 / 31.0); }},
    {"just", "5-limit JI on C", "5-limit just intonation on C: C, E and G a pure major triad",
     [](double) {
         static const double ratios[12] = {1.0,       16.0 / 15, 9.0 / 8, 6.0 / 5, 5.0 / 4, 4.0 / 3,
                                           45.0 / 32, 3.0 / 2,   8.0 / 5, 5.0 / 3, 9.0 / 5, 15.0 / 8};
         double cents[12];
         for (int i = 0; i < 12; ++i)
             cents[i] = 1200.0 * std::log2(ratios[i]);
         return octaveOf(cents);
     }},
    {"meantone", "1/4-comma meantone on C", "quarter-comma meantone, Eb to G#: pure major thirds",
     [](double) {
         const double fifth = 1200.0 * std::log2(std::pow(5.0, 0.25)); // 696.58 cents
         double cents[12];
         for (int n = -3; n <= 8; ++n) { // the chain of fifths from Eb to G#
             const int degree = ((n * 7) % 12 + 12) % 12;
             cents[degree] = std::fmod(n * fifth + 12000.0, 1200.0);
         }
         return octaveOf(cents);
     }},
    {"stretched", "12-TET, 1210-cent octave", "twelve equal steps to an octave stretched 10 cents, A4 at 440 Hz",
     [](double) { return equal(1210.0 / 12.0); }},
    {"unmapped", "12-TET, black keys unmapped", "the white keys at 12-TET, the black keys unmapped",
     [](double) {
         // An unmapped key takes the pitch of the key below it, as ODDSound suggests.
         Table t = equal(100.0);
         static constexpr bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
         for (int key = 1; key < 128; ++key)
             if (black[key % 12])
                 t[key] = t[key - 1];
         return t;
     },
     [](int key) {
         static constexpr bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
         return black[key % 12];
     }},
    {"channels", "12-TET, channel 2 a quarter tone up",
     "12-TET, but notes on MIDI channel 2 a quarter tone up (for clients that ask by channel)",
     [](double) { return equal(100.0); }, nullptr,
     [] {
         Table t = equal(100.0);
         for (double &hz : t)
             hz *= std::exp2(50.0 / 1200.0);
         return t;
     }},
    {"sweep", "12-TET, sweeping +-50 cents", "12-TET swinging 50 cents up and down every 4 seconds, held notes too",
     [](double seconds) {
         Table t = equal(100.0);
         const double cents = 50.0 * std::sin(2.0 * std::numbers::pi * seconds / 4.0);
         for (double &hz : t)
             hz *= std::exp2(cents / 1200.0);
         return t;
     },
     nullptr, nullptr, true},
};

const Scene *sceneNamed(const std::string &name) {
    for (const Scene &scene : kScenes)
        if (name == scene.name)
            return &scene;
    return nullptr;
}

void listScenes() {
    std::printf("scenes:\n");
    for (const Scene &scene : kScenes)
        std::printf("  %-10s %s\n", scene.name, scene.what);
    std::printf("commands: a scene's name, off, on, status, list, quit\n");
}

// Where ODDSound's libraries look for libMTS, and whether it is there.
std::filesystem::path libMtsPath() {
#ifdef _WIN32
    PWSTR common = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &common)) && common)
        path = std::filesystem::path(common) / L"MTS-ESP" / L"LIBMTS.dll";
    CoTaskMemFree(common);
    return path;
#else
    return "/usr/local/lib/libMTS.so";
#endif
}

std::atomic<bool> interrupted{false};

extern "C" void onInterrupt(int) {
    interrupted.store(true);
}

// What the input asks of the master, which a thread of its own carries out: every call into the master library is
// made there, as is each step of a scene that moves.
struct Master {
    std::mutex lock;
    std::vector<std::string> commands;
    bool registered = false;
    const Scene *scene = nullptr;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

    void apply(const Scene &next) {
        const bool changed = scene != &next;
        scene = &next;
        since = std::chrono::steady_clock::now();
        if (!registered)
            return;
        MTS_ClearNoteFilter();
        for (int key = 0; key < 128; ++key)
            if (next.unmapped && next.unmapped(key))
                MTS_FilterNote(true, static_cast<char>(key), -1);
        // Channel 2's table, or none: the other channels use the general one.
        MTS_SetMultiChannel(next.channel2 != nullptr, 1);
        if (next.channel2) {
            const Table own = next.channel2();
            MTS_SetMultiChannelNoteTunings(own.data(), 1);
        }
        const Table table = next.table(0.0);
        MTS_SetNoteTunings(table.data());
        MTS_SetScaleName(next.scale);
        if (changed)
            std::printf("scene %s: %s (%s), %d client(s)\n", next.name, next.scale, next.what, MTS_GetNumClients());
    }

    void registerAgain() {
        if (registered) {
            std::printf("registered already\n");
            return;
        }
        if (!MTS_CanRegisterMaster()) {
            std::printf("another master is registered: close it first%s\n",
                        MTS_HasIPC() ? " (or, if one crashed, start this with --reinit)" : "");
            return;
        }
        MTS_RegisterMaster();
        registered = true;
        std::printf("registered as the MTS-ESP master%s\n", MTS_HasIPC() ? " (IPC)" : "");
        const Scene *was = scene;
        scene = nullptr;
        apply(*was);
    }

    void deregister() {
        if (!registered)
            return;
        MTS_DeregisterMaster();
        registered = false;
        std::printf("deregistered: clients go back to their own tuning\n");
    }

    void status() {
        std::printf("%s, scene %s (%s), %d client(s)\n", registered ? "registered" : "not registered", scene->name,
                    scene->scale, MTS_GetNumClients());
    }

    // Returns false once asked to quit.
    bool command(const std::string &line) {
        if (line.empty())
            return true;
        if (line == "quit" || line == "q" || line == "exit")
            return false;
        if (line == "off")
            deregister();
        else if (line == "on")
            registerAgain();
        else if (line == "status")
            status();
        else if (line == "list")
            listScenes();
        else if (const Scene *next = sceneNamed(line))
            apply(*next);
        else
            std::printf("no scene or command %s (list shows them)\n", line.c_str());
        std::fflush(stdout);
        return true;
    }
};

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // each line as it comes, for a program reading them
    const Scene *first = &kScenes[0];
    double seconds = -1.0;
    bool reinit = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--for" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--reinit") {
            reinit = true;
        } else if (arg == "--list" || arg == "--help" || arg == "-h") {
            std::printf("usage: fmvibe-mtsmaster [scene] [--for <seconds>] [--reinit] [--list]\n");
            listScenes();
            return 0;
        } else if (const Scene *scene = sceneNamed(arg)) {
            first = scene;
        } else {
            std::fprintf(stderr, "no scene %s (--list shows them)\n", arg.c_str());
            return 2;
        }
    }

    const std::filesystem::path lib = libMtsPath();
    std::error_code error;
    if (lib.empty() || !std::filesystem::exists(lib, error)) {
        std::fprintf(stderr,
                     "libMTS isn't installed (looked for %s): install an MTS-ESP master (MTS-ESP Mini), or libMTS "
                     "from https://github.com/ODDSound/MTS-ESP/tree/main/libMTS\n",
                     lib.string().c_str());
        return 1;
    }
    if (!MTS_CanRegisterMaster()) {
        if (!(reinit && MTS_HasIPC())) {
            std::fprintf(stderr, "another MTS-ESP master is registered: close it first%s\n",
                         MTS_HasIPC() ? " (or, if one crashed, start this with --reinit)" : "");
            return 1;
        }
        MTS_Reinitialize();
        std::printf("reinitialized MTS-ESP\n");
    }
    if (MTS_Master_ShouldUpdateLibrary())
        std::printf("libMTS is older than this master's library: some features may be missing\n");

    Master master;
    master.scene = first;
    master.registerAgain();
    std::signal(SIGINT, onInterrupt);
#ifdef SIGTERM
    std::signal(SIGTERM, onInterrupt);
#endif

    // The master's own thread: commands, the moving scenes' steps every 10 ms, and the end.
    std::atomic<bool> done{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&] {
        for (;;) {
            std::vector<std::string> commands;
            {
                std::lock_guard<std::mutex> guard(master.lock);
                commands.swap(master.commands);
            }
            bool quit = done.load() || interrupted.load();
            for (const std::string &line : commands)
                quit = !master.command(line) || quit;
            if (seconds >= 0.0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= seconds)
                quit = true;
            if (quit)
                break;
            if (master.registered && master.scene->moves) {
                const double at = std::chrono::duration<double>(std::chrono::steady_clock::now() - master.since).count();
                const Table table = master.scene->table(at);
                MTS_SetNoteTunings(table.data());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        master.deregister();
        std::fflush(stdout);
        // The input may still be waiting for a line: end here rather than wait for it.
        std::_Exit(0);
    });

    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        std::lock_guard<std::mutex> guard(master.lock);
        master.commands.push_back(line);
        if (line == "quit" || line == "q" || line == "exit")
            break;
    }
    // The input over: with --for, run on until then; else stop now.
    if (seconds < 0.0)
        done.store(true);
    worker.join();
    return 0;
}
