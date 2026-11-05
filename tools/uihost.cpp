// fmvibe-uihost: a small CLAP host for trying the editor by hand, without a DAW. It loads the built plugin, opens
// its editor in a window of its own and plays it through the default output device (WASAPI, shared mode), with
// notes from the editor's keyboard, the computer's (as in the browser demo) and every MIDI input it can open. Without a device (or with --silent) it
// still runs the plugin in real time into silence, so the meters, the scope and the analyzers move.
// Usage: fmvibe-uihost [path\to\FMVibe.clap] [--silent] [--no-midi] [--tempo <bpm>]

#include <clap/clap.h>

#include <windows.h>

#include <mmreg.h>
#include <mmsystem.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kMaxBlock = 1024;      // the most frames one process() call gets
constexpr uint32_t kSilentBlock = 256;    // frames per block without a device
constexpr double kFallbackRate = 48000.0; // without a device
constexpr REFERENCE_TIME kBufferTime = 200000; // the device buffer asked for: 20 ms, in 100 ns units

constexpr UINT kMsgCallback = WM_APP;    // the plugin asked for on_main_thread()
constexpr UINT kMsgRestart = WM_APP + 1; // the plugin asked to be deactivated and activated again
constexpr UINT kMsgResize = WM_APP + 2;  // the editor asked for another size (wParam x lParam)

constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;

std::string utf8(const wchar_t *text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 1 ? static_cast<size_t>(n - 1) : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// --- The audio thread: runs the plugin's process() into the device, or into silence on a clock.

struct OutputEvents {
    clap_output_events_t list{this, [](const clap_output_events_t *, const clap_event_header_t *) { return true; }};
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

class Audio {
public:
    Audio(const clap_plugin_t *plugin, bool silent, double tempo) : plugin_(plugin), silent_(silent) {
        transport_.header = {sizeof(transport_), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transport_.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        transport_.tempo = tempo;
        transport_.tsig_num = 4;
        transport_.tsig_denom = 4;
        left_.resize(kMaxBlock);
        right_.resize(kMaxBlock);
        midi_.reserve(256);
        pending_.reserve(256);
        events_.events.reserve(256);
    }

    // Opens the device and returns its sample rate, which the plugin is then activated at (see setActive()).
    double start() {
        std::future<double> opened = ratePromise_.get_future();
        thread_ = std::thread([this] { run(ratePromise_); });
        return opened.get();
    }

    void stop() {
        quit_ = true;
        if (thread_.joinable())
            thread_.join();
    }

    void setActive(bool active) { active_ = active; }

    // Main thread: the audio thread stops processing and stays out of the plugin until resume().
    void pause() {
        stopped_ = false;
        stopRequest_ = true;
        while (!stopped_ && alive_)
            Sleep(1);
    }
    void resume() { stopRequest_ = false; }

    // Any thread (the MIDI inputs' callbacks): a channel message for the next block.
    void pushMidi(uint8_t status, uint8_t data1, uint8_t data2) {
        clap_event_midi_t ev{};
        ev.header = {sizeof(ev), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
        ev.port_index = 0;
        ev.data[0] = status;
        ev.data[1] = data1;
        ev.data[2] = data2;
        std::lock_guard<std::mutex> lock(midiLock_);
        if (midi_.size() < 4096)
            midi_.push_back(ev);
    }

private:
    void run(std::promise<double> &rate) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        bool announced = false;
        if (!silent_)
            playToDevice(rate, announced);
        if (!announced) {
            rate.set_value(kFallbackRate);
            announced = true;
            rate_ = kFallbackRate;
        }
        if (!quit_)
            playToSilence();
        if (processing_) {
            plugin_->stop_processing(plugin_);
            processing_ = false;
        }
        if (task)
            AvRevertMmThreadCharacteristics(task);
        CoUninitialize();
        alive_ = false;
    }

    // Returns once quit or once the device fails; `announced` says whether the rate went out.
    void playToDevice(std::promise<double> &rate, bool &announced) {
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        WAVEFORMATEX *mix = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(devices.GetAddressOf()))) ||
            FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(client.GetAddressOf()))) ||
            FAILED(client->GetMixFormat(&mix))) {
            std::printf("No audio output device: running without sound.\n");
            return;
        }
        const DWORD deviceRate = mix->nSamplesPerSec;
        CoTaskMemFree(mix);

        // Stereo float at the device's own rate; Windows converts to its mix format where that differs.
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = deviceRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<IAudioRenderClient> renderer;
        UINT32 bufferFrames = 0;
        BYTE *data = nullptr;
        if (!ready ||
            FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      kBufferTime, 0, &format, nullptr)) ||
            FAILED(client->SetEventHandle(ready)) || FAILED(client->GetBufferSize(&bufferFrames)) ||
            FAILED(client->GetService(IID_PPV_ARGS(renderer.GetAddressOf()))) ||
            FAILED(renderer->GetBuffer(bufferFrames, &data)) ||
            FAILED(renderer->ReleaseBuffer(bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT)) || FAILED(client->Start())) {
            std::printf("The audio output device can't be opened: running without sound.\n");
            if (ready)
                CloseHandle(ready);
            return;
        }
        rate_ = deviceRate;
        rate.set_value(rate_);
        announced = true;
        std::printf("Playing at %lu Hz, %.1f ms buffer.\n", deviceRate, 1000.0 * bufferFrames / deviceRate);

        while (!quit_) {
            WaitForSingleObject(ready, 200);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding)))
                break;
            const UINT32 frames = bufferFrames - padding;
            if (frames == 0)
                continue;
            if (FAILED(renderer->GetBuffer(frames, &data)))
                break;
            render(reinterpret_cast<float *>(data), frames);
            if (FAILED(renderer->ReleaseBuffer(frames, 0)))
                break;
        }
        if (!quit_)
            std::printf("The audio output device went away: running without sound.\n");
        client->Stop();
        CloseHandle(ready);
    }

    // The plugin in real time, its output thrown away.
    void playToSilence() {
        timeBeginPeriod(1);
        std::vector<float> scratch(2 * kSilentBlock);
        const auto block = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(kSilentBlock / rate_));
        auto next = std::chrono::steady_clock::now();
        while (!quit_) {
            render(scratch.data(), kSilentBlock);
            next += block;
            const auto now = std::chrono::steady_clock::now();
            if (now - next > std::chrono::milliseconds(100))
                next = now; // fell behind (a debugger, say): don't race to catch up
            std::this_thread::sleep_until(next);
        }
        timeEndPeriod(1);
    }

    // Audio thread: `frames` stereo frames, interleaved.
    void render(float *out, uint32_t frames) {
        if (stopRequest_) {
            if (processing_) {
                plugin_->stop_processing(plugin_);
                processing_ = false;
            }
            stopped_ = true;
        } else if (active_ && !processing_) {
            processing_ = plugin_->start_processing(plugin_);
        }
        if (stopRequest_ || !processing_) {
            std::fill(out, out + 2 * frames, 0.0f);
            return;
        }

        pending_.clear();
        if (midiLock_.try_lock()) {
            pending_.swap(midi_);
            midiLock_.unlock();
        }
        for (uint32_t done = 0; done < frames;) {
            const uint32_t n = std::min(frames - done, kMaxBlock);
            events_.events.clear();
            if (done == 0)
                for (const clap_event_midi_t &ev : pending_)
                    events_.events.push_back(&ev.header);
            float *channels[2] = {left_.data(), right_.data()};
            clap_audio_buffer_t output{};
            output.data32 = channels;
            output.channel_count = 2;
            clap_process_t process{};
            process.steady_time = steadyTime_;
            process.frames_count = n;
            process.transport = &transport_;
            process.audio_outputs = &output;
            process.audio_outputs_count = 1;
            process.in_events = &events_.list;
            process.out_events = &outEvents_.list;
            if (plugin_->process(plugin_, &process) == CLAP_PROCESS_ERROR) {
                std::fill(left_.begin(), left_.begin() + n, 0.0f);
                std::fill(right_.begin(), right_.begin() + n, 0.0f);
            }
            for (uint32_t i = 0; i < n; ++i) { // kept in range, for the ears
                out[2 * (done + i)] = std::clamp(left_[i], -1.0f, 1.0f);
                out[2 * (done + i) + 1] = std::clamp(right_[i], -1.0f, 1.0f);
            }
            steadyTime_ += n;
            done += n;
        }
    }

    const clap_plugin_t *plugin_;
    const bool silent_;
    std::promise<double> ratePromise_;
    std::thread thread_;
    double rate_ = kFallbackRate;
    std::atomic<bool> quit_{false}, alive_{true}, active_{false}, stopRequest_{false}, stopped_{false};

    // Audio thread only.
    bool processing_ = false;
    int64_t steadyTime_ = 0;
    clap_event_transport_t transport_{};
    std::vector<float> left_, right_;
    std::vector<clap_event_midi_t> pending_;
    InputEvents events_;
    OutputEvents outEvents_;

    std::mutex midiLock_;
    std::vector<clap_event_midi_t> midi_;
};

// --- MIDI inputs: every one that opens, its channel messages into the audio thread.

void CALLBACK midiInput(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR packed, DWORD_PTR) {
    if (message != MIM_DATA)
        return;
    const auto status = static_cast<uint8_t>(packed & 0xFF);
    if (status < 0x80 || status >= 0xF0) // channel messages only
        return;
    reinterpret_cast<Audio *>(instance)->pushMidi(status, static_cast<uint8_t>((packed >> 8) & 0x7F),
                                                  static_cast<uint8_t>((packed >> 16) & 0x7F));
}

std::vector<HMIDIIN> openMidiInputs(Audio &audio) {
    std::vector<HMIDIIN> inputs;
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        midiInGetDevCapsW(i, &caps, sizeof(caps));
        const std::string name = utf8(caps.szPname);
        HMIDIIN input = nullptr;
        if (midiInOpen(&input, i, reinterpret_cast<DWORD_PTR>(&midiInput), reinterpret_cast<DWORD_PTR>(&audio),
                       CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            std::printf("MIDI input %s can't be opened (another program may have it).\n", name.c_str());
            continue;
        }
        midiInStart(input);
        inputs.push_back(input);
        std::printf("MIDI input: %s\n", name.c_str());
    }
    return inputs;
}

// --- The host: what the plugin may ask of it, and the window around the editor.

struct App {
    const clap_plugin_t *plugin = nullptr;
    const clap_plugin_gui_t *gui = nullptr;
    Audio *audio = nullptr;
    double sampleRate = kFallbackRate;
    HWND window = nullptr;
    std::string title;          // the window's, before the computer keyboard's octave and velocity
    int octave = 0;             // the computer keyboard's, from middle C
    int velocity = 5;           // the computer keyboard's, in kKeyVelocities
    std::array<int, 256> held;  // by scan code: the note the key plays, or -1
    bool active = false;        // activated, and not deactivated since
    bool editorCreated = false; // gui->create() went through, and gui->destroy() hasn't followed
    bool editorOpen = false;    // and it is in the window
    std::atomic<bool> callbackPosted{false}, restartPosted{false};
};

App app;

const clap_host_gui_t kHostGui = {
    [](const clap_host_t *) {}, // resize_hints_changed: asked for again at each resize
    [](const clap_host_t *, uint32_t width, uint32_t height) {
        return app.window && PostMessageW(app.window, kMsgResize, width, height) != 0;
    },
    [](const clap_host_t *) { return false; }, // request_show
    [](const clap_host_t *) { return false; }, // request_hide
    [](const clap_host_t *, bool) {},          // closed: not floating
};

const clap_host_t kHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "fmvibe-uihost",
    "",
    "",
    "0",
    [](const clap_host_t *, const char *id) -> const void * {
        return std::strcmp(id, CLAP_EXT_GUI) == 0 ? &kHostGui : nullptr;
    },
    [](const clap_host_t *) { // request_restart
        if (app.window && !app.restartPosted.exchange(true))
            PostMessageW(app.window, kMsgRestart, 0, 0);
    },
    [](const clap_host_t *) {}, // request_process: the audio thread processes all the time
    [](const clap_host_t *) {   // request_callback
        if (app.window && !app.callbackPosted.exchange(true))
            PostMessageW(app.window, kMsgCallback, 0, 0);
    },
};

// The window's frame around a client area of no size.
SIZE frameSize(HWND window) {
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, 0, GetDpiForWindow(window));
    return {frame.right - frame.left, frame.bottom - frame.top};
}

// A client area the editor fills: its width for the size given, or less where that would be taller.
void fitEditor(uint32_t &width, uint32_t &height) {
    const uint32_t room = height;
    app.gui->adjust_size(app.plugin, &width, &height);
    clap_gui_resize_hints_t hints{};
    if (height > room && app.gui->get_resize_hints(app.plugin, &hints) && hints.aspect_ratio_height > 0) {
        width = static_cast<uint32_t>(static_cast<uint64_t>(room) * hints.aspect_ratio_width /
                                      hints.aspect_ratio_height);
        app.gui->adjust_size(app.plugin, &width, &height);
    }
}

void resizeClient(uint32_t width, uint32_t height) {
    const SIZE frame = frameSize(app.window);
    SetWindowPos(app.window, nullptr, 0, 0, static_cast<int>(width) + frame.cx, static_cast<int>(height) + frame.cy,
                 SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
}

void closeEditor() {
    if (app.editorOpen)
        app.gui->hide(app.plugin);
    if (app.editorCreated)
        app.gui->destroy(app.plugin);
    app.editorOpen = app.editorCreated = false;
}

void restartPlugin() {
    app.restartPosted = false;
    app.audio->pause();
    app.audio->setActive(false);
    if (app.active)
        app.plugin->deactivate(app.plugin);
    app.active = app.plugin->activate(app.plugin, app.sampleRate, 1, kMaxBlock);
    app.audio->setActive(app.active);
    app.audio->resume();
    std::printf("%s\n", app.active ? "Restarted the plugin, as it asked." : "The plugin didn't activate again.");
}

// --- The computer keyboard's piano, as the browser demo has it: A is middle C, W the C# above it, and so on to
// the semicolon; Z and X shift the octave, C and V step the velocity. By scan code, so it plays by key position on
// any layout.

constexpr std::array<int, 8> kKeyVelocities = {1, 20, 40, 60, 80, 100, 120, 127}; // 100 as the plugin's own keyboard

int pianoKey(uint32_t scanCode) {
    switch (scanCode) {
    case 0x1E: return 0;  // A
    case 0x11: return 1;  // W
    case 0x1F: return 2;  // S
    case 0x12: return 3;  // E
    case 0x20: return 4;  // D
    case 0x21: return 5;  // F
    case 0x14: return 6;  // T
    case 0x22: return 7;  // G
    case 0x15: return 8;  // Y
    case 0x23: return 9;  // H
    case 0x16: return 10; // U
    case 0x24: return 11; // J
    case 0x25: return 12; // K
    case 0x18: return 13; // O
    case 0x26: return 14; // L
    case 0x19: return 15; // P
    case 0x27: return 16; // semicolon
    default: return -1;
    }
}
constexpr uint32_t kOctaveDown = 0x2C; // Z
constexpr uint32_t kOctaveUp = 0x2D;   // X
constexpr uint32_t kVelocityDown = 0x2E; // C
constexpr uint32_t kVelocityUp = 0x2F;   // V

void showKeys() {
    const std::string title = app.title + " - keys from C" + std::to_string(4 + app.octave) + ", velocity " +
                              std::to_string(kKeyVelocities[app.velocity]);
    SetWindowTextA(app.window, title.c_str());
}

void releaseKeys() {
    for (int &note : app.held)
        if (note >= 0) {
            app.audio->pushMidi(0x80, static_cast<uint8_t>(note), 0);
            note = -1;
        }
}

// The host's loop, before a key reaches the window that has the keyboard: true when the piano took it. Keys the
// editor takes while a value is typed never come here (its hook empties them), nor Ctrl+Z over the editor.
bool playKey(const MSG &msg) {
    if ((msg.message != WM_KEYDOWN && msg.message != WM_KEYUP) || !msg.hwnd ||
        GetAncestor(msg.hwnd, GA_ROOT) != app.window)
        return false;
    const auto flags = static_cast<uint32_t>(msg.lParam);
    if (flags & (1u << 24)) // an extended key: the arrows, the keypad's Enter and the like
        return false;
    const uint32_t scanCode = (flags >> 16) & 0xFF;
    if (msg.message == WM_KEYUP) { // let go, even with Ctrl down since
        const int note = app.held[scanCode];
        if (note < 0)
            return false;
        app.held[scanCode] = -1;
        app.audio->pushMidi(0x80, static_cast<uint8_t>(note), 0);
        return true;
    }
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
        return false;
    const bool repeat = (flags & (1u << 30)) != 0;
    if (const int key = pianoKey(scanCode); key >= 0) {
        if (repeat || app.held[scanCode] >= 0)
            return true;
        const int note = 60 + 12 * app.octave + key;
        if (note > 127)
            return true;
        app.held[scanCode] = note;
        app.audio->pushMidi(0x90, static_cast<uint8_t>(note), static_cast<uint8_t>(kKeyVelocities[app.velocity]));
        return true;
    }
    if (scanCode == kOctaveDown || scanCode == kOctaveUp) {
        if (!repeat) {
            app.octave = std::clamp(app.octave + (scanCode == kOctaveUp ? 1 : -1), -4, 4);
            showKeys();
        }
        return true;
    }
    if (scanCode == kVelocityDown || scanCode == kVelocityUp) {
        if (!repeat) {
            app.velocity = std::clamp(app.velocity + (scanCode == kVelocityUp ? 1 : -1), 0,
                                      static_cast<int>(kKeyVelocities.size()) - 1);
            showKeys();
        }
        return true;
    }
    return false;
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ACTIVATE: // no key may hang once the window loses the keyboard
        if (LOWORD(wParam) == WA_INACTIVE)
            releaseKeys();
        break;
    case kMsgCallback:
        app.callbackPosted = false;
        app.plugin->on_main_thread(app.plugin);
        return 0;
    case kMsgRestart:
        restartPlugin();
        return 0;
    case kMsgResize:
        if (app.editorOpen)
            resizeClient(static_cast<uint32_t>(wParam), static_cast<uint32_t>(lParam));
        return 0;
    case WM_SIZING: { // keep the editor's aspect ratio while the frame is dragged
        if (!app.editorOpen)
            break;
        RECT &rect = *reinterpret_cast<RECT *>(lParam);
        const SIZE frame = frameSize(window);
        uint32_t width = static_cast<uint32_t>(std::max<LONG>(0, rect.right - rect.left - frame.cx));
        uint32_t height = static_cast<uint32_t>(std::max<LONG>(0, rect.bottom - rect.top - frame.cy));
        clap_gui_resize_hints_t hints{};
        if ((wParam == WMSZ_TOP || wParam == WMSZ_BOTTOM) && app.gui->get_resize_hints(app.plugin, &hints) &&
            hints.aspect_ratio_height > 0) // the height dragged: the width follows it
            width = static_cast<uint32_t>(static_cast<uint64_t>(height) * hints.aspect_ratio_width /
                                          hints.aspect_ratio_height);
        app.gui->adjust_size(app.plugin, &width, &height);
        const LONG outerWidth = static_cast<LONG>(width) + frame.cx;
        const LONG outerHeight = static_cast<LONG>(height) + frame.cy;
        if (wParam == WMSZ_LEFT || wParam == WMSZ_TOPLEFT || wParam == WMSZ_BOTTOMLEFT)
            rect.left = rect.right - outerWidth;
        else
            rect.right = rect.left + outerWidth;
        if (wParam == WMSZ_TOP || wParam == WMSZ_TOPLEFT || wParam == WMSZ_TOPRIGHT)
            rect.top = rect.bottom - outerHeight;
        else
            rect.bottom = rect.top + outerHeight;
        return TRUE;
    }
    case WM_SIZE:
        if (app.editorOpen && wParam != SIZE_MINIMIZED) {
            uint32_t width = LOWORD(lParam), height = HIWORD(lParam);
            fitEditor(width, height);
            app.gui->set_size(app.plugin, width, height);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT &suggested = *reinterpret_cast<const RECT *>(lParam);
        SetWindowPos(window, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                     suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        closeEditor(); // the editor goes first, while its parent is still there
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void usage() {
    std::printf("Usage: fmvibe-uihost [path\\to\\FMVibe.clap] [--silent] [--no-midi] [--tempo <bpm>]\n"
                "  Opens FMVibe's editor in a window and plays it through the default output device.\n"
                "  The plugin defaults to FMVibe.clap next to this program.\n"
                "  --silent   run the plugin without an audio device (the meters and scope still move)\n"
                "  --no-midi  leave the MIDI inputs alone\n"
                "  --tempo    the tempo synced LFOs and effects follow (default 120 BPM)\n"
                "  The computer keyboard plays notes: A W S E D ... from middle C, Z and X shift the octave,\n"
                "  C and V step the velocity (1, 20, 40 ... 127).\n");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    std::wstring path;
    bool silent = false, midi = true;
    double tempo = 120.0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--silent") {
            silent = true;
        } else if (arg == L"--no-midi") {
            midi = false;
        } else if (arg == L"--tempo" && i + 1 < argc) {
            tempo = std::clamp(std::wcstod(argv[++i], nullptr), 20.0, 999.0);
        } else if (arg == L"-h" || arg == L"--help" || arg == L"/?" || !path.empty() || arg.rfind(L"--", 0) == 0) {
            usage();
            return arg == L"-h" || arg == L"--help" || arg == L"/?" ? 0 : 2;
        } else {
            path = arg;
        }
    }
    if (path.empty()) { // FMVibe.clap next to this program, where the build puts both
        wchar_t self[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
        path.assign(self, n);
        path.resize(path.find_last_of(L"\\/") + 1);
        path += L"FMVibe.clap";
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // for the file dialogs

    // The window comes first, so whatever the plugin asks for has somewhere to go; it shows once the editor is in.
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOWTEXT); // dark, around an editor that doesn't fill it
    windowClass.lpszClassName = L"FMVibeUiHost";
    RegisterClassExW(&windowClass);
    app.window = CreateWindowExW(0, windowClass.lpszClassName, L"FMVibe", kWindowStyle, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 1200, 900, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!app.window) {
        std::fprintf(stderr, "Can't create a window.\n");
        return 1;
    }

    const std::string pathUtf8 = utf8(path.c_str());
    HMODULE library = LoadLibraryW(path.c_str());
    if (!library) {
        std::fprintf(stderr, "Can't load %s (error %lu).\n", pathUtf8.c_str(), GetLastError());
        return 1;
    }
    const auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(GetProcAddress(library, "clap_entry"));
    if (!entry || !entry->init(pathUtf8.c_str())) {
        std::fprintf(stderr, "%s isn't a CLAP plugin.\n", pathUtf8.c_str());
        return 1;
    }
    const auto *factory =
        static_cast<const clap_plugin_factory_t *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_descriptor_t *descriptor =
        factory && factory->get_plugin_count(factory) > 0 ? factory->get_plugin_descriptor(factory, 0) : nullptr;
    app.plugin = descriptor ? factory->create_plugin(factory, &kHost, descriptor->id) : nullptr;
    if (!app.plugin || !app.plugin->init(app.plugin)) {
        std::fprintf(stderr, "%s has no plugin to create.\n", pathUtf8.c_str());
        return 1;
    }
    std::printf("%s %s, from %s\n", descriptor->name, descriptor->version, pathUtf8.c_str());
    app.title = std::string(descriptor->name) + " " + descriptor->version + " (fmvibe-uihost)";
    app.held.fill(-1);
    showKeys();

    Audio audio(app.plugin, silent, tempo);
    app.audio = &audio;
    app.sampleRate = audio.start();
    app.active = app.plugin->activate(app.plugin, app.sampleRate, 1, kMaxBlock);
    audio.setActive(app.active);
    std::vector<HMIDIIN> midiInputs;
    if (midi)
        midiInputs = openMidiInputs(audio);

    app.gui = static_cast<const clap_plugin_gui_t *>(app.plugin->get_extension(app.plugin, CLAP_EXT_GUI));
    clap_window_t parent{};
    parent.api = CLAP_WINDOW_API_WIN32;
    parent.win32 = app.window;
    uint32_t width = 0, height = 0;
    app.editorCreated = app.gui && app.gui->create(app.plugin, CLAP_WINDOW_API_WIN32, false);
    app.editorOpen = app.editorCreated && app.gui->get_size(app.plugin, &width, &height) &&
                     app.gui->set_parent(app.plugin, &parent) && app.gui->show(app.plugin);
    const bool opened = app.editorOpen;
    if (!opened) {
        std::fprintf(stderr, "The plugin's editor doesn't open.\n");
    } else {
        resizeClient(width, height);
        ShowWindow(app.window, SW_SHOWNORMAL);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (playKey(message))
                continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    for (HMIDIIN input : midiInputs) {
        midiInStop(input);
        midiInClose(input);
    }
    audio.stop();
    closeEditor(); // if the loop ended without WM_CLOSE, or the editor didn't open
    if (IsWindow(app.window))
        DestroyWindow(app.window);
    app.window = nullptr;
    if (app.active)
        app.plugin->deactivate(app.plugin);
    app.plugin->destroy(app.plugin);
    entry->deinit();
    FreeLibrary(library);
    CoUninitialize();
    return opened ? 0 : 1;
}
