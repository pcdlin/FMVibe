#include "vst3plugin.h"

#include "params.h"
#include "plugincore.h"
#include "uistrings.h"

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstphysicalui.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/vstspeaker.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace fmvibe::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// --- Text: VST3 speaks UTF-16, FMVibe's wordings are UTF-8.

void copyText(char16 *to, int capacity, const std::string &utf8) {
    if (capacity <= 0)
        return;
    int at = 0;
    for (size_t i = 0; i < utf8.size() && at < capacity - 1;) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        uint32_t c = b;
        size_t length = 1;
        if ((b & 0xE0) == 0xC0) {
            c = b & 0x1Fu;
            length = 2;
        } else if ((b & 0xF0) == 0xE0) {
            c = b & 0x0Fu;
            length = 3;
        } else if ((b & 0xF8) == 0xF0) {
            c = b & 0x07u;
            length = 4;
        } else if (b >= 0x80) {
            c = '?';
        }
        bool whole = i + length <= utf8.size();
        for (size_t k = 1; whole && k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(utf8[i + k]);
            if ((next & 0xC0) != 0x80) {
                whole = false;
                break;
            }
            c = (c << 6) | (next & 0x3Fu);
        }
        if (!whole)
            c = '?';
        i += whole ? length : 1;
        if (c >= 0x10000 && c <= 0x10FFFF) { // a surrogate pair, while the room lasts
            if (at + 1 >= capacity - 1)
                break;
            const uint32_t v = c - 0x10000;
            to[at++] = static_cast<char16>(0xD800 + (v >> 10));
            to[at++] = static_cast<char16>(0xDC00 + (v & 0x3FF));
        } else {
            to[at++] = static_cast<char16>(c > 0xFFFF ? '?' : c);
        }
    }
    to[at] = 0;
}

std::string utf8Text(const char16 *text) {
    if (!text)
        return {};
    std::string out;
    for (size_t i = 0; text[i]; ++i) {
        uint32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(text[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c <= 0x10FFFF) {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += '?';
        }
    }
    return out;
}

// --- Params as VST3 has them: 0..1 over each one's range, a stepped one in whole steps.

constexpr double kMaxStepCount = 0x7FFFFFFF;

int32 stepCount(int index) {
    const ParamDef &def = paramTable()[index];
    const double steps = def.max - def.min;
    // The chain order has more orders than a step count holds: hosts see it as continuous (it is hidden anyway).
    return isStepped(def.kind) && steps >= 1.0 && steps <= kMaxStepCount ? static_cast<int32>(std::lround(steps)) : 0;
}

double toNormalized(int index, double plain) {
    const ParamDef &def = paramTable()[index];
    return def.max > def.min ? std::clamp((plain - def.min) / (def.max - def.min), 0.0, 1.0) : 0.0;
}

double toPlain(int index, double normalized) {
    const ParamDef &def = paramTable()[index];
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (const int32 steps = stepCount(index); steps > 0) // as VST3 spreads steps: each an equal share of 0..1
        return def.min + std::min<double>(steps, std::floor(normalized * (steps + 1)));
    const double plain = def.min + normalized * (def.max - def.min);
    return isStepped(def.kind) ? std::round(plain) : plain;
}

bool isMidiParam(ParamID id) {
    return id >= kMidiParamBase && id < kMidiParamBase + static_cast<ParamID>(kMidiParamCount);
}

// Where a pitch bend param rests: the middle.
double midiParamDefault(ParamID id) {
    return (id - kMidiParamBase) % kMidiControllers == kPitchBend ? 0.5 : 0.0;
}

// --- Units: the params' modules ("Operators/Op A"), as hosts group them.

struct Unit {
    UnitID id;
    UnitID parent;
    std::string name;
};

struct Units {
    std::vector<Unit> units;      // the root first
    std::vector<UnitID> unitOf;   // each param's, by index
};

const Units &units() {
    static const Units built = [] {
        Units u;
        u.units.push_back({kRootUnitId, kNoParentUnitId, kPluginName});
        u.unitOf.assign(kParamCount, kRootUnitId);
        std::map<std::string, UnitID> byPath;
        for (int h = 0; h < hostParamCount(); ++h) { // units for the params hosts are given
            const int i = hostParamIndex(h);
            const std::string &module = paramTable()[i].module;
            UnitID parent = kRootUnitId;
            size_t start = 0;
            while (start < module.size()) {
                const size_t end = std::min(module.find('/', start), module.size());
                const std::string path = module.substr(0, end);
                auto found = byPath.find(path);
                if (found == byPath.end()) {
                    const auto id = static_cast<UnitID>(u.units.size());
                    u.units.push_back({id, parent, module.substr(start, end - start)});
                    found = byPath.emplace(path, id).first;
                }
                parent = found->second;
                start = end + 1;
            }
            u.unitOf[i] = parent;
        }
        return u;
    }();
    return built;
}

// --- The state goes through the same writer and reader as CLAP's, over the host's IBStream.

clap_istream_t streamIn(IBStream *stream) {
    return {stream, [](const clap_istream_t *s, void *buffer, uint64_t size) -> int64_t {
                int32 read = 0;
                const auto chunk = static_cast<int32>(std::min<uint64_t>(size, 1u << 30));
                if (static_cast<IBStream *>(s->ctx)->read(buffer, chunk, &read) != kResultOk)
                    return -1;
                return read;
            }};
}

clap_ostream_t streamOut(IBStream *stream) {
    return {stream, [](const clap_ostream_t *s, const void *buffer, uint64_t size) -> int64_t {
                int32 written = 0;
                const auto chunk = static_cast<int32>(std::min<uint64_t>(size, 1u << 30));
                if (static_cast<IBStream *>(s->ctx)->write(const_cast<void *>(buffer), chunk, &written) != kResultOk)
                    return -1;
                return written;
            }};
}

// --- The note expressions FMVibe takes, in CLAP's terms, which the synth speaks.

struct Expression {
    NoteExpressionTypeID type;
    Str title;
    double defaultValue;
    int32 flags;
};

const Expression kExpressions[] = {
    {kVolumeTypeID, Str::ExprVolume, 0.25, NoteExpressionTypeInfo::kIsAbsolute},
    {kPanTypeID, Str::ExprPan, 0.5, NoteExpressionTypeInfo::kIsBipolar | NoteExpressionTypeInfo::kIsAbsolute},
    {kTuningTypeID, Str::ExprTuning, 0.5, NoteExpressionTypeInfo::kIsBipolar},
    {kBrightnessTypeID, Str::ExprBrightness, 0.5, NoteExpressionTypeInfo::kIsAbsolute},
    {kPressureTypeId, Str::ExprPressure, 0.0, NoteExpressionTypeInfo::kIsAbsolute},
};

// What the synth takes for a VST3 note expression (see Synth::noteExpression()), or false for one it doesn't.
bool clapExpression(NoteExpressionTypeID type, double normalized, int32_t &id, double &value) {
    switch (type) {
    case kVolumeTypeID: // 0.25 is 0 dB: a gain of 4 at the top, as CLAP's
        id = CLAP_NOTE_EXPRESSION_VOLUME;
        value = 4.0 * normalized;
        return true;
    case kPanTypeID:
        id = CLAP_NOTE_EXPRESSION_PAN;
        value = normalized;
        return true;
    case kTuningTypeID: // ten octaves either way, as CLAP's
        id = CLAP_NOTE_EXPRESSION_TUNING;
        value = 240.0 * (normalized - 0.5);
        return true;
    case kBrightnessTypeID:
        id = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
        value = normalized;
        return true;
    case kPressureTypeId:
        id = CLAP_NOTE_EXPRESSION_PRESSURE;
        value = normalized;
        return true;
    default:
        return false;
    }
}

// How other threads wake the main thread to have it do what only may be done there (see Plugin::wake()): a message-only
// window on Windows, a pipe whose read end the host's Linux run loop watches (IRunLoop), or a thread of our own where
// the host offers no run loop (a plugin without an editor).
#ifdef _WIN32
constexpr wchar_t kWakeClass[] = L"FMVibeVst3Wake";
constexpr UINT kWakeMessage = WM_APP + 1;
constexpr UINT_PTR kWakeTimer = 1; // see hostTimer()
int liveWakeWindows = 0;

HINSTANCE moduleHandle() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleHandle), &module);
    return module;
}
#else
const FIDString kViewPlatform = kPlatformTypeX11EmbedWindowID;
#endif

#ifdef _WIN32
const FIDString kViewPlatform = kPlatformTypeHWND;
#endif

// How many param changes and events a block sorts by time; any beyond apply after them, as the host gave them.
constexpr size_t kMaxChanges = 4096;

} // namespace

class View;

// IComponent and IEditController both have getState() and setState(), alike: these tell them apart. The
// component's is the whole state; the controller's is nothing, since the component holds it all.
class ComponentStateNames : public IComponent {
public:
    tresult PLUGIN_API setState(IBStream *state) final { return setComponentBytes(state); }
    tresult PLUGIN_API getState(IBStream *state) final { return getComponentBytes(state); }

protected:
    virtual tresult setComponentBytes(IBStream *state) = 0;
    virtual tresult getComponentBytes(IBStream *state) = 0;
};

class ControllerStateNames : public IEditController {
public:
    tresult PLUGIN_API setState(IBStream *) final { return kResultOk; }
    tresult PLUGIN_API getState(IBStream *) final { return kResultOk; }
};

class WakeHandler;

// The processor and the edit controller in one object, on the core.
class Plugin final : public PluginCore,
                     public ComponentStateNames,
                     public IAudioProcessor,
                     public IProcessContextRequirements,
                     public ControllerStateNames,
                     public IMidiMapping,
                     public INoteExpressionController,
                     public INoteExpressionPhysicalUIMapping,
                     public IUnitInfo {
public:
    Plugin();
    ~Plugin() override;

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override;
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (left == 0)
            delete this;
        return left;
    }

    // The editor's clock: the host's run loop timer calls this (and a wake, where one came for another reason).
    // Windows keeps its window's own timer, so there is nothing to pump there.
    void pumpEditor() {
#ifndef _WIN32
        if (gui::Editor *window = editor())
            window->pump();
#endif
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown *context) override;
    tresult PLUGIN_API terminate() override;

    // Holds the main thread's work and the wake thread's apart (see mainLock_); nothing on Windows, whose wake
    // window is the main thread's.
    class MainLock {
    public:
#ifdef _WIN32
        explicit MainLock(Plugin &) {}
#else
        explicit MainLock(Plugin &plugin) : lock_(plugin.mainLock_) {}

    private:
        std::lock_guard<std::recursive_mutex> lock_;
#endif
    };

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; } // this is its controller
    tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override;
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo &bus) override;
    tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool) override {
        return getBusCount(type, dir) > index && index >= 0 ? kResultTrue : kInvalidArgument;
    }
    tresult PLUGIN_API setActive(TBool state) override;

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *inputs, int32 numIns, SpeakerArrangement *outputs,
                                          int32 numOuts) override;
    tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement &arr) override;
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    // As setActive() worked it out (see PluginCore::latency()); an edit that moves it asks for a restart. A host may
    // ask between its setActive(false) and setActive(true): what the next activation will make it, then.
    uint32 PLUGIN_API getLatencySamples() override {
        return static_cast<uint32>(active_ ? latency() : latencyAt(sampleRate_));
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override;
    tresult PLUGIN_API setProcessing(TBool state) override;
    tresult PLUGIN_API process(ProcessData &data) override;
    uint32 PLUGIN_API getTailSamples() override { return kInfiniteTail; } // the effects ring on

    // IProcessContextRequirements
    uint32 PLUGIN_API getProcessContextRequirements() override {
        return IProcessContextRequirements::kNeedTempo | IProcessContextRequirements::kNeedProjectTimeMusic |
               IProcessContextRequirements::kNeedTransportState; // where it plays, for anchored LFOs
    }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream *) override { return kResultOk; } // the component is this
    int32 PLUGIN_API getParameterCount() override { return hostParamCount() + kMidiParamCount; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override;
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue normalized, String128 string) override;
    tresult PLUGIN_API getParamValueByString(ParamID id, TChar *string, ParamValue &normalized) override;
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue normalized) override;
    ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plain) override;
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override;
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override;
    tresult PLUGIN_API setComponentHandler(IComponentHandler *handler) override;
    IPlugView *PLUGIN_API createView(FIDString name) override;

    // IMidiMapping
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel, CtrlNumber controller,
                                                   ParamID &id) override;

    // INoteExpressionController
    int32 PLUGIN_API getNoteExpressionCount(int32 busIndex, int16) override {
        return busIndex == 0 ? static_cast<int32>(std::size(kExpressions)) : 0;
    }
    tresult PLUGIN_API getNoteExpressionInfo(int32 busIndex, int16 channel, int32 index,
                                             NoteExpressionTypeInfo &info) override;
    tresult PLUGIN_API getNoteExpressionStringByValue(int32 busIndex, int16 channel, NoteExpressionTypeID id,
                                                      NoteExpressionValue normalized, String128 string) override;
    tresult PLUGIN_API getNoteExpressionValueByString(int32 busIndex, int16 channel, NoteExpressionTypeID id,
                                                      const TChar *string, NoteExpressionValue &normalized) override;

    // INoteExpressionPhysicalUIMapping
    tresult PLUGIN_API getPhysicalUIMapping(int32 busIndex, int16 channel, PhysicalUIMapList &list) override;

    // IUnitInfo
    int32 PLUGIN_API getUnitCount() override { return static_cast<int32>(units().units.size()); }
    tresult PLUGIN_API getUnitInfo(int32 index, UnitInfo &info) override;
    int32 PLUGIN_API getProgramListCount() override { return 0; }
    tresult PLUGIN_API getProgramListInfo(int32, ProgramListInfo &) override { return kResultFalse; }
    tresult PLUGIN_API getProgramName(ProgramListID, int32, String128) override { return kResultFalse; }
    tresult PLUGIN_API getProgramInfo(ProgramListID, int32, Vst::CString, String128) override { return kResultFalse; }
    tresult PLUGIN_API hasProgramPitchNames(ProgramListID, int32) override { return kResultFalse; }
    tresult PLUGIN_API getProgramPitchName(ProgramListID, int32, int16, String128) override { return kResultFalse; }
    UnitID PLUGIN_API getSelectedUnit() override { return selectedUnit_; }
    tresult PLUGIN_API selectUnit(UnitID unit) override {
        if (unit < 0 || unit >= getUnitCount())
            return kInvalidArgument;
        selectedUnit_ = unit;
        return kResultTrue;
    }
    tresult PLUGIN_API getUnitByBus(MediaType, BusDirection, int32, int32, UnitID &) override { return kResultFalse; }
    tresult PLUGIN_API setUnitProgramData(int32, int32, IBStream *) override { return kResultFalse; }

protected:
    tresult setComponentBytes(IBStream *state) override;
    tresult getComponentBytes(IBStream *state) override;

    void hostBeginEdit(int index) override {
        if (handler_)
            handler_->beginEdit(idOfIndex(index));
    }
    void hostEdit(int index, double value) override {
        told(index, steadyMs());
        if (handler_)
            handler_->performEdit(idOfIndex(index), toNormalized(index, value));
    }
    void hostEndEdit(int index) override {
        if (handler_)
            handler_->endEdit(idOfIndex(index));
    }
    // Asked for later, on the main thread: a host may be in the middle of handing over a state.
    void hostValuesChanged() override { restart(kParamValuesChanged); }
    void hostMarkDirty() override {
        if (handler2_)
            handler2_->setDirty(true);
    }
    void hostResizeEditor(uint32_t width, uint32_t height) override;
    // VST3 has no restart of its own: a latency change has the host deactivate and activate the plugin.
    void hostRequestRestart() override { restart(kLatencyChanged); }
    void hostRequestCallback() override {
        callbackPending_.store(true);
        wake();
    }
    // A timer on the wake window, or on the view's run loop where Linux has one.
    bool hostTimer(bool on) override {
#ifdef _WIN32
        if (!wakeWindow_)
            return false;
        if (on)
            return SetTimer(wakeWindow_, kWakeTimer, kTimerMs, nullptr) != 0;
        KillTimer(wakeWindow_, kWakeTimer);
        return true;
#else
        // The run loop's timer, which pumps the editor, ticks the voice limit's settle timer too.
        if (!on)
            return true;
        return runLoop_ != nullptr && wakeHandler_ != nullptr;
#endif
    }

private:
    friend class View;
#ifndef _WIN32
    friend class WakeHandler;
#endif

    // A param change or an event, at its place in a block (see process()).
    struct Change {
        int32 offset;
        int32 order;     // as the host gave them: param changes first, then events
        ParamID id;      // a param change's
        double value;    // a param change's normalized value, or an event's index
        bool event;
    };

    // Any thread: has the main thread call restartComponent() with `flags`, soon.
    void restart(int32 flags) {
        restartFlags_.fetch_or(flags);
        wake();
    }
    // Any thread: has the main thread wake, unless one is on its way already.
    void wake() {
        if (wakePosted_.exchange(true))
            return;
#ifdef _WIN32
        // A post that fails (its queue full) leaves none on its way: the next wake posts again.
        if (!wakeWindow_ || !PostMessageW(wakeWindow_, kWakeMessage, 0, 0))
            wakePosted_.store(false);
#else
        const char byte = 1;
        if (::write(wakePipe_[1], &byte, 1) < 0 && errno != EAGAIN)
            wakePosted_.store(false);
#endif
    }
    // Main thread (the host's run loop, or the thread where the host offers none).
    void woken();
    void timerTicked();
#ifdef _WIN32
    static LRESULT CALLBACK wakeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void frameChanged(IPlugFrame *) {} // the wake window's timer pumps the editor
#else
    // Main thread: the host's run loop came or went with the view's frame.
    void frameChanged(IPlugFrame *frame);
    // Main thread, without mainLock_ (it may stop the wake thread, which waits for it): wakes the main thread through
    // `runLoop`, whose reference it takes, or else (`orThread`) through a thread of its own.
    void useRunLoop(Linux::IRunLoop *runLoop, bool orThread);
#endif

    // Hands each of the block's param changes and events to `take`, in the host's order (see Change::order).
    template <class Take>
    void forEachChange(ProcessData &data, Take &&take);
    // Returns whether there were more than kMaxChanges, which applyOverflow() applies.
    bool collect(ProcessData &data);
    void applyOverflow(ProcessData &data);
    void apply(const Change &change, ProcessData &data);
    void applyMidi(ParamID id, double value, int32 offset, ProcessData &data);
    void applyEvent(const Event &ev);

    std::atomic<uint32> refs_{1};
    IComponentHandler *handler_ = nullptr;
    IComponentHandler2 *handler2_ = nullptr;
    View *view_ = nullptr; // the editor's view, while there is one
#ifdef _WIN32
    HWND wakeWindow_ = nullptr;
#else
    // The pipe other threads write to, the run loop (or the thread) reads: the main thread wakes there.
    int wakePipe_[2] = {-1, -1};
    Linux::IRunLoop *runLoop_ = nullptr;
    Linux::IRunLoop *hostRunLoop_ = nullptr; // the host application's, where it offers one without a view
    // With no run loop, the wake thread does the main thread's work: this keeps it from running beside the host's
    // calls on its main thread (a state handed over, the handler set, the editor opened), which take it too. Never
    // held across restartComponent(), which a host may answer from its main thread.
    std::recursive_mutex mainLock_;
    WakeHandler *wakeHandler_ = nullptr;
    std::thread wakeThread_;
    std::atomic<bool> wakeThreadRunning_{false};
    void startWakeThread();
    void stopWakeThread();
    void readWake();
#endif
    std::atomic<bool> wakePosted_{false};
    std::atomic<bool> callbackPending_{false};
    std::atomic<int32> restartFlags_{0};
    double sampleRate_ = 48000.0;
    bool active_ = false; // main thread: since a setActive(true) that went through
    std::atomic<bool> processing_{false};
    // Processing stopped with a restart the plugin asked for still to come, the voices kept for it: a setActive(true)
    // takes them up (see PluginCore::activate()), and processing started again without one ends their notes.
    std::atomic<bool> stoppedForRestart_{false};
    UnitID selectedUnit_ = kRootUnitId; // the host's, for its own use
    std::vector<Change> changes_; // reserved up front: the audio thread never allocates
};

// The editor, in the host's window. The plugin has one view at a time, which owns the editor.
class View final : public IPlugView
#ifndef _WIN32
    , public IPlugViewContentScaleSupport
#endif
{
public:
    explicit View(Plugin &plugin) : plugin_(plugin) { plugin_.addRef(); }
    ~View() {
        if (owns()) {
            const Plugin::MainLock lock(plugin_);
            plugin_.view_ = nullptr;
            plugin_.closeEditor();
        }
        plugin_.release();
    }

    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override {
        QUERY_INTERFACE(queried, obj, FUnknown::iid, IPlugView)
        QUERY_INTERFACE(queried, obj, IPlugView::iid, IPlugView)
#ifndef _WIN32
        QUERY_INTERFACE(queried, obj, IPlugViewContentScaleSupport::iid, IPlugViewContentScaleSupport)
#endif
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (left == 0)
            delete this;
        return left;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return type && std::strcmp(type, kViewPlatform) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void *parent, FIDString type) override {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent || !owns())
            return kResultFalse;
        const Plugin::MainLock lock(plugin_);
        if (!plugin_.editor()) // attached again after removed()
            plugin_.openEditor();
        gui::Editor *editor = plugin_.editor();
#ifdef _WIN32
        return editor && editor->attach(static_cast<HWND>(parent)) ? kResultOk : kResultFalse;
#else
        // An X11 host hands its window's id as the parent.
        const auto window = static_cast<gui::Editor::WindowId>(reinterpret_cast<uintptr_t>(parent));
        if (!editor || !editor->attach(window))
            return kResultFalse;
        plugin_.pumpEditor(); // the first frame now, before the run loop's timer comes
        return kResultOk;
#endif
    }
    tresult PLUGIN_API removed() override {
        const Plugin::MainLock lock(plugin_);
        if (owns())
            plugin_.closeEditor();
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    // The editor watches the keyboard itself (see Editor::shortcut() and Editor::typedKey()).
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect *size) override {
        if (!size)
            return kInvalidArgument;
        const Plugin::MainLock lock(plugin_);
        uint32_t width = 0, height = 0;
        if (gui::Editor *editor = owns() ? plugin_.editor() : nullptr) {
            editor->size(width, height);
        } else {
            // As an editor opens: at the system's DPI, which it then follows to its window's
            width = static_cast<uint32_t>(
                std::lround(gui::View::kWidth * plugin_.uiLayout().zoom * gui::Editor::systemScale()));
            gui::Editor::constrain(width, height, gui::View::heightFor(plugin_.uiLayout()));
        }
        *size = ViewRect(0, 0, static_cast<int32>(width), static_cast<int32>(height));
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect *size) override {
        if (!size)
            return kInvalidArgument;
        const Plugin::MainLock lock(plugin_);
        if (gui::Editor *editor = owns() ? plugin_.editor() : nullptr)
            editor->setSize(static_cast<uint32_t>(std::max(size->getWidth(), 1)),
                            static_cast<uint32_t>(std::max(size->getHeight(), 1)));
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    // The host's content scale: the editor keeps its zoom, the host is asked for the size that has it at the scale.
#ifndef _WIN32
    tresult PLUGIN_API setContentScaleFactor(float factor) override {
        const Plugin::MainLock lock(plugin_);
        if (gui::Editor *editor = owns() ? plugin_.editor() : nullptr)
            editor->setScale(factor);
        return kResultOk;
    }
#endif
    tresult PLUGIN_API setFrame(IPlugFrame *frame) override {
        {
            const Plugin::MainLock lock(plugin_);
            frame_ = frame; // the host's, alive while it is set
        }
        plugin_.frameChanged(frame); // without the lock, as it may stop the wake thread
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    // The view's proportions: the width the host offers with the height following it, or, for a window dragged by its
    // top or bottom edge, the height with the width following it (see Editor::constrain()).
    tresult PLUGIN_API checkSizeConstraint(ViewRect *rect) override {
        if (!rect)
            return kInvalidArgument;
        const Plugin::MainLock lock(plugin_);
        auto width = static_cast<uint32_t>(std::max(rect->getWidth(), 1));
        auto height = static_cast<uint32_t>(std::max(rect->getHeight(), 0));
        if (gui::Editor *editor = owns() ? plugin_.editor() : nullptr)
            editor->fit(width, height);
        else
            gui::Editor::constrain(width, height, gui::View::heightFor(plugin_.uiLayout()));
        rect->right = rect->left + static_cast<int32>(width);
        rect->bottom = rect->top + static_cast<int32>(height);
        return kResultTrue;
    }

    // Asks the host for a window this big; it then calls onSize().
    void requestResize(uint32_t width, uint32_t height) {
        if (!frame_)
            return;
        ViewRect rect(0, 0, static_cast<int32>(width), static_cast<int32>(height));
        frame_->resizeView(this, &rect);
    }

private:
    bool owns() const { return plugin_.view_ == this; }

    std::atomic<uint32> refs_{1};
    Plugin &plugin_;
    IPlugFrame *frame_ = nullptr;
};

Plugin::Plugin() {
    changes_.reserve(kMaxChanges);
#ifdef _WIN32
    // Plugins are made on the main thread, which then owns this window.
    if (liveWakeWindows++ == 0) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Plugin::wakeProc;
        wc.hInstance = moduleHandle();
        wc.lpszClassName = kWakeClass;
        RegisterClassExW(&wc);
    }
    wakeWindow_ = CreateWindowExW(0, kWakeClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, moduleHandle(), nullptr);
    if (wakeWindow_)
        SetWindowLongPtrW(wakeWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
#else
    if (::pipe2(wakePipe_, O_CLOEXEC | O_NONBLOCK) == 0) {
        // A plugin without an editor never sees a run loop: its own thread reads the pipe instead.
        startWakeThread();
    } else {
        wakePipe_[0] = wakePipe_[1] = -1;
    }
#endif
}

Plugin::~Plugin() {
#ifndef _WIN32
    // The wake thread first: it may be using the handler.
    useRunLoop(nullptr, false);
    stopWakeThread();
    if (hostRunLoop_)
        hostRunLoop_->release();
    hostRunLoop_ = nullptr;
#endif
    closeEditor();
    setComponentHandler(nullptr);
#ifdef _WIN32
    if (wakeWindow_) {
        SetWindowLongPtrW(wakeWindow_, GWLP_USERDATA, 0); // a message still on its way finds no plugin
        DestroyWindow(wakeWindow_);
    }
    if (--liveWakeWindows == 0)
        UnregisterClassW(kWakeClass, moduleHandle());
#else
    if (wakePipe_[0] >= 0)
        ::close(wakePipe_[0]);
    if (wakePipe_[1] >= 0)
        ::close(wakePipe_[1]);
    wakePipe_[0] = wakePipe_[1] = -1;
#endif
}

tresult PLUGIN_API Plugin::queryInterface(const TUID queried, void **obj) {
    QUERY_INTERFACE(queried, obj, FUnknown::iid, IComponent)
    QUERY_INTERFACE(queried, obj, IPluginBase::iid, IComponent)
    QUERY_INTERFACE(queried, obj, IComponent::iid, IComponent)
    QUERY_INTERFACE(queried, obj, IAudioProcessor::iid, IAudioProcessor)
    QUERY_INTERFACE(queried, obj, IProcessContextRequirements::iid, IProcessContextRequirements)
    QUERY_INTERFACE(queried, obj, IEditController::iid, IEditController)
    QUERY_INTERFACE(queried, obj, IMidiMapping::iid, IMidiMapping)
    QUERY_INTERFACE(queried, obj, INoteExpressionController::iid, INoteExpressionController)
    QUERY_INTERFACE(queried, obj, INoteExpressionPhysicalUIMapping::iid, INoteExpressionPhysicalUIMapping)
    QUERY_INTERFACE(queried, obj, IUnitInfo::iid, IUnitInfo)
    *obj = nullptr;
    return kNoInterface;
}

// The host application's Linux run loop, where it offers one: the main thread wakes there with no view open, rather
// than on a thread of the plugin's own.
tresult PLUGIN_API Plugin::initialize(FUnknown *context) {
#ifndef _WIN32
    void *runLoop = nullptr;
    if (context && !hostRunLoop_ && context->queryInterface(Linux::IRunLoop::iid, &runLoop) == kResultOk && runLoop) {
        hostRunLoop_ = static_cast<Linux::IRunLoop *>(runLoop);
        if (!runLoop_) {
            hostRunLoop_->addRef();
            useRunLoop(hostRunLoop_, true);
        }
    }
    if (!runLoop_)
        startWakeThread(); // initialized again after terminate(), which stopped it
#else
    (void)context;
#endif
    return kResultOk;
}

tresult PLUGIN_API Plugin::terminate() {
    {
        const MainLock lock(*this);
        closeEditor();
    }
#ifndef _WIN32
    // The wake thread before the handler goes: it may be using it.
    useRunLoop(nullptr, false);
    stopWakeThread();
    if (hostRunLoop_)
        hostRunLoop_->release();
    hostRunLoop_ = nullptr;
#endif
    setComponentHandler(nullptr);
    return kResultOk;
}

#ifdef _WIN32
LRESULT CALLBACK Plugin::wakeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kWakeMessage || (message == WM_TIMER && wParam == kWakeTimer))
        if (auto *plugin = reinterpret_cast<Plugin *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            if (message == kWakeMessage)
                plugin->woken();
            else
                plugin->timerTicked();
            return 0;
        }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
#endif

void Plugin::woken() {
    wakePosted_.store(false);
    IComponentHandler *handler = nullptr;
    int32 flags = 0;
    {
        const MainLock lock(*this);
#ifndef _WIN32
        pumpEditor();
#endif
        if (callbackPending_.exchange(false))
            onMainThread(); // may ask for a restart: that comes next
        flags = restartFlags_.exchange(0);
        if (flags && handler_) {
            handler = handler_;
            handler->addRef();
        }
    }
    if (handler) {
        handler->restartComponent(flags);
        handler->release();
    }
}

void Plugin::timerTicked() {
    IComponentHandler *handler = nullptr;
    int32 flags = 0;
    {
        const MainLock lock(*this);
        onTimer(); // may ask for a restart: that comes next
        flags = restartFlags_.exchange(0);
        if (flags && handler_) {
            handler = handler_;
            handler->addRef();
        }
    }
    if (handler) {
        handler->restartComponent(flags);
        handler->release();
    }
}

#ifndef _WIN32
// The host's run loop watches the wake pipe: its byte means the main thread has what only it may do.
class WakeHandler final : public Steinberg::Linux::IEventHandler, public Steinberg::Linux::ITimerHandler {
public:
    explicit WakeHandler(Plugin &plugin) : plugin_(&plugin) {}
    void detach() { plugin_ = nullptr; }
    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override {
        QUERY_INTERFACE(queried, obj, FUnknown::iid, Linux::IEventHandler)
        QUERY_INTERFACE(queried, obj, Linux::IEventHandler::iid, Linux::IEventHandler)
        QUERY_INTERFACE(queried, obj, Linux::ITimerHandler::iid, Linux::ITimerHandler)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --refs_;
        if (left == 0)
            delete this;
        return left;
    }
    void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor fd) override {
        char buffer[64];
        while (fd >= 0 && ::read(fd, buffer, sizeof buffer) > 0) {
        }
        if (plugin_)
            plugin_->woken();
    }
    void PLUGIN_API onTimer() override {
        if (plugin_) {
            plugin_->pumpEditor(); // the editor's events, its poll and its paint, as CLAP's timer does there
            plugin_->timerTicked(); // and the voice limit's settle timer, which needs no timer of its own here
        }
    }

private:
    std::atomic<uint32> refs_{1};
    Plugin *plugin_;
};

void Plugin::startWakeThread() {
    if (wakePipe_[0] < 0 || wakeThreadRunning_.exchange(true))
        return;
    wakeThread_ = std::thread([this] {
        while (wakeThreadRunning_.load(std::memory_order_relaxed)) {
            pollfd waiting{wakePipe_[0], POLLIN, 0};
            if (::poll(&waiting, 1, 100) <= 0)
                continue;
            char buffer[64];
            while (::read(wakePipe_[0], buffer, sizeof buffer) > 0) {
            }
            if (!wakeThreadRunning_.load(std::memory_order_relaxed))
                break;
            woken();
        }
    });
}

void Plugin::stopWakeThread() {
    if (wakePipe_[0] < 0 || !wakeThreadRunning_.exchange(false))
        return;
    const char byte = 1; // wakes the poll, so the thread sees that it is to end
    if (::write(wakePipe_[1], &byte, 1) < 0) {
    }
    if (wakeThread_.joinable())
        wakeThread_.join();
}

// The view's frame: the host's run loop comes with it, where it offers one (Bitwig does), else the host application's.
// The pipe's read end is what wakes the main thread there; without a run loop, a thread of our own reads it.
void Plugin::frameChanged(IPlugFrame *frame) {
    Linux::IRunLoop *runLoop = nullptr;
    if (frame && frame->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void **>(&runLoop)) != kResultOk)
        runLoop = nullptr;
    if (!runLoop && hostRunLoop_) {
        runLoop = hostRunLoop_;
        runLoop->addRef();
    }
    useRunLoop(runLoop, true);
}

void Plugin::useRunLoop(Linux::IRunLoop *runLoop, bool orThread) {
    if (runLoop && runLoop == runLoop_) { // the one it has: the reference handed over isn't needed
        runLoop->release();
        return;
    }
    if (runLoop_ && wakeHandler_) {
        runLoop_->unregisterTimer(wakeHandler_);
        runLoop_->unregisterEventHandler(wakeHandler_);
        wakeHandler_->detach();
        wakeHandler_->release();
        wakeHandler_ = nullptr;
    }
    if (runLoop_)
        runLoop_->release();
    runLoop_ = runLoop;
    if (runLoop_) {
        stopWakeThread();
        wakeHandler_ = new WakeHandler(*this);
        if (runLoop_->registerEventHandler(wakeHandler_, wakePipe_[0]) != kResultOk ||
            runLoop_->registerTimer(wakeHandler_, 16) != kResultOk) {
            runLoop_->unregisterEventHandler(wakeHandler_);
            wakeHandler_->detach();
            wakeHandler_->release();
            wakeHandler_ = nullptr;
            runLoop_->release();
            runLoop_ = nullptr;
            if (orThread)
                startWakeThread();
        }
    } else if (orThread) {
        startWakeThread();
    }
}
#endif

int32 PLUGIN_API Plugin::getBusCount(MediaType type, BusDirection dir) {
    if (type == kAudio)
        return dir == kOutput ? 1 : 0;
    if (type == kEvent)
        return dir == kInput ? 1 : 0;
    return 0;
}

tresult PLUGIN_API Plugin::getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo &bus) {
    if (index != 0 || getBusCount(type, dir) < 1)
        return kInvalidArgument;
    bus = {};
    bus.mediaType = type;
    bus.direction = dir;
    bus.busType = kMain;
    bus.flags = BusInfo::kDefaultActive;
    if (type == kAudio) {
        bus.channelCount = 2;
        copyText(bus.name, 128, tr(Str::PortOutput));
    } else {
        bus.channelCount = kMidiChannels;
        copyText(bus.name, 128, tr(Str::PortNotes));
    }
    return kResultOk;
}

tresult PLUGIN_API Plugin::setActive(TBool state) {
    const MainLock lock(*this);
    if (state) {
        stoppedForRestart_.store(false);
        active_ = activate(sampleRate_); // voices are allocated here, on the main thread
        if (!active_)
            return kOutOfMemory;
    } else {
        active_ = false;
        processing_.store(false);
        deactivate();
    }
    return kResultOk;
}

tresult Plugin::setComponentBytes(IBStream *state) {
    if (!state)
        return kInvalidArgument;
    const MainLock lock(*this);
    const clap_istream_t stream = streamIn(state);
    try {
        if (!loadState(&stream))
            return kResultFalse;
    } catch (const std::bad_alloc &) { // an exception must not reach the host, which would end with it
        return kOutOfMemory;
    }
    clearHistory();
    return kResultOk;
}

tresult Plugin::getComponentBytes(IBStream *state) {
    if (!state)
        return kInvalidArgument;
    const MainLock lock(*this);
    const clap_ostream_t stream = streamOut(state);
    try {
        return saveState(&stream) ? kResultOk : kResultFalse;
    } catch (const std::bad_alloc &) {
        return kOutOfMemory;
    }
}

// A stereo output and nothing else.
tresult PLUGIN_API Plugin::setBusArrangements(SpeakerArrangement *, int32 numIns, SpeakerArrangement *outputs,
                                              int32 numOuts) {
    return numIns == 0 && numOuts == 1 && outputs && outputs[0] == SpeakerArr::kStereo ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Plugin::getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement &arr) {
    if (dir != kOutput || index != 0)
        return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
}

tresult PLUGIN_API Plugin::setupProcessing(ProcessSetup &setup) {
    if (setup.symbolicSampleSize != kSample32 || !(setup.sampleRate > 0.0))
        return kResultFalse;
    sampleRate_ = setup.sampleRate;
    return kResultOk;
}

tresult PLUGIN_API Plugin::setProcessing(TBool state) {
    processing_.store(state != 0);
    if (!state) {
        stopProcessing(); // the editor's telemetry stops
        // The notes end here, unless this stops processing for the restart the plugin asked for (a host does so before
        // setActive(false)), which they play on through.
        if (restartPending())
            stoppedForRestart_.store(true);
        else
            resetVoices();
    } else if (stoppedForRestart_.exchange(false)) {
        resetVoices(); // no restart came
    }
    return kResultOk;
}

tresult PLUGIN_API Plugin::process(ProcessData &data) {
    if (outOfMemory()) { // a host running it though its setActive() failed
        if (data.numOutputs > 0 && data.outputs && data.symbolicSampleSize == kSample32 &&
            data.outputs->channelBuffers32) {
            for (int32 channel = 0; channel < data.outputs->numChannels; ++channel)
                std::fill_n(data.outputs->channelBuffers32[channel], std::max(data.numSamples, 0), 0.0f);
            data.outputs->silenceFlags = (uint64{1} << data.outputs->numChannels) - 1;
        }
        return kResultOk;
    }
    if (data.numSamples > 0) // for hosts that process without setProcessing() first
        processing_.store(true, std::memory_order_relaxed);
    if (data.processContext && (data.processContext->state & ProcessContext::kTempoValid))
        synth().setTempo(data.processContext->tempo);
    // Where it plays, for anchored LFOs: in quarter notes when the host gives them, else from its samples, which are
    // always there, at its tempo.
    if (const ProcessContext *context = data.processContext; context && context->sampleRate > 0.0) {
        const double seconds = static_cast<double>(context->projectTimeSamples) / context->sampleRate;
        const bool music = (context->state & ProcessContext::kProjectTimeMusicValid) != 0;
        synth().setTransport((context->state & ProcessContext::kPlaying) != 0,
                             music ? context->projectTimeMusic : seconds * synth().hostTempo() / 60.0, seconds);
    } else {
        synth().setTransport(false, 0.0, 0.0);
    }
    beginBlock();
    const bool overflow = collect(data);

    // Without a stereo output to render into (a host flushing params), the changes are only taken.
    const int32 frames = std::max(data.numSamples, 0);
    AudioBusBuffers *out = data.numOutputs > 0 ? data.outputs : nullptr;
    float *left = nullptr, *right = nullptr;
    if (out && out->numChannels >= 2 && data.symbolicSampleSize == kSample32 && out->channelBuffers32) {
        left = out->channelBuffers32[0];
        right = out->channelBuffers32[1];
    }
    const bool rendering = left && right && frames > 0;

    // Render up to each change's place, then apply it: sample-accurate notes and automation.
    int32 cursor = 0;
    for (const Change &change : changes_) {
        const int32 at = std::clamp(change.offset, 0, frames);
        if (rendering && at > cursor) {
            render(left + cursor, right + cursor, static_cast<uint32_t>(at - cursor));
            cursor = at;
        }
        apply(change, data);
    }
    if (overflow)
        applyOverflow(data);
    if (rendering && frames > cursor)
        render(left + cursor, right + cursor, static_cast<uint32_t>(frames - cursor));
    if (rendering)
        out->silenceFlags = 0;

    endBlock();
    return kResultOk;
}

template <class Take>
void Plugin::forEachChange(ProcessData &data, Take &&take) {
    int32 order = 0;
    if (IParameterChanges *params = data.inputParameterChanges) {
        const int32 count = params->getParameterCount();
        for (int32 q = 0; q < count; ++q) {
            IParamValueQueue *queue = params->getParameterData(q);
            if (!queue)
                continue;
            const ParamID id = queue->getParameterId();
            const int32 points = queue->getPointCount();
            for (int32 p = 0; p < points; ++p) {
                int32 offset = 0;
                ParamValue value = 0.0;
                if (queue->getPoint(p, offset, value) != kResultOk)
                    continue;
                take(Change{offset, order++, id, value, false});
            }
        }
    }
    if (IEventList *events = data.inputEvents) {
        const int32 count = events->getEventCount();
        for (int32 e = 0; e < count; ++e) {
            Event ev{};
            if (events->getEvent(e, ev) != kResultOk)
                continue;
            take(Change{ev.sampleOffset, order++, 0, static_cast<double>(e), true});
        }
    }
}

// The block's param changes and events, by their place in it, as many as were reserved for.
bool Plugin::collect(ProcessData &data) {
    changes_.clear();
    bool overflow = false;
    forEachChange(data, [&](const Change &change) {
        if (changes_.size() < kMaxChanges)
            changes_.push_back(change);
        else
            overflow = true;
    });
    // Param changes come before the notes at the same place, so a note hears the values it starts with.
    std::sort(changes_.begin(), changes_.end(), [](const Change &a, const Change &b) {
        return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
    });
    return overflow;
}

// Those past what was reserved, after the others and in the host's order: each param's later points after its
// earlier ones, and events after the ones before them in the list, so a note's end follows its start.
void Plugin::applyOverflow(ProcessData &data) {
    forEachChange(data, [&](const Change &change) {
        if (change.order >= static_cast<int32>(kMaxChanges))
            apply(change, data);
    });
}

void Plugin::apply(const Change &change, ProcessData &data) {
    if (change.event) {
        Event ev{};
        if (data.inputEvents && data.inputEvents->getEvent(static_cast<int32>(change.value), ev) == kResultOk)
            applyEvent(ev);
    } else if (isMidiParam(change.id)) {
        applyMidi(change.id, change.value, change.offset, data);
    } else if (const int index = indexOfId(change.id); index >= 0) {
        setHostValue(index, toPlain(index, change.value), true);
    }
}

// A MIDI controller, channel pressure or pitch bend, as the host's MIDI mapping hands it over, at `offset` in the block.
void Plugin::applyMidi(ParamID id, double value, int32 offset, ProcessData &data) {
    const int channel = static_cast<int>(id - kMidiParamBase) / kMidiControllers;
    const int controller = static_cast<int>(id - kMidiParamBase) % kMidiControllers;
    value = std::clamp(value, 0.0, 1.0);
    if (controller == kPitchBend) // 0..16383 as MIDI has it, 8192 in the middle
        synth().pitchBend(channel, std::clamp((value * 16383.0 - 8192.0) / 8192.0, -1.0, 1.0));
    else if (controller == kAfterTouch)
        synth().channelPressure(channel, value);
    else
        synth().controlChange(channel, controller, value);
    // A hardware mod wheel moves the Mod Wheel param too (see playModWheel()), which the host hears in its place.
    if (controller == 1 && playModWheel(value) && data.outputParameterChanges) {
        int32 queue = 0;
        const ParamID wheel = idOfIndex(paramIndex(GlobalParam::ModWheel));
        if (IParamValueQueue *points = data.outputParameterChanges->addParameterData(wheel, queue)) {
            int32 point = 0;
            points->addPoint(std::max(offset, 0), toNormalized(paramIndex(GlobalParam::ModWheel), value), point);
        }
    }
}

void Plugin::applyEvent(const Event &ev) {
    Synth &synth = this->synth();
    switch (ev.type) {
    case Event::kNoteOnEvent: {
        const NoteOnEvent &on = ev.noteOn;
        const NoteRef note{on.noteId, 0, on.channel, on.pitch};
        synth.noteOn(note, on.velocity);
        if (on.tuning != 0.0f) // in cents
            synth.noteExpression(note, CLAP_NOTE_EXPRESSION_TUNING, on.tuning / 100.0);
        break;
    }
    case Event::kNoteOffEvent: {
        const NoteOffEvent &off = ev.noteOff;
        synth.noteOff({off.noteId, 0, off.channel, off.pitch});
        break;
    }
    case Event::kPolyPressureEvent: {
        const PolyPressureEvent &pressure = ev.polyPressure;
        synth.polyPressure({pressure.noteId, 0, pressure.channel, pressure.pitch}, pressure.pressure);
        break;
    }
    case Event::kDataEvent: // MTS SysEx retunes the synth (see PluginCore::playSysEx())
        if (ev.data.type == DataEvent::kMidiSysEx)
            playSysEx(ev.data.bytes, ev.data.size);
        break;
    case Event::kNoteExpressionValueEvent: {
        const NoteExpressionValueEvent &expr = ev.noteExpressionValue;
        int32_t id = 0;
        double value = 0.0;
        if (expr.noteId >= 0 && clapExpression(expr.typeId, expr.value, id, value))
            synth.noteExpression({expr.noteId, -1, -1, -1}, id, value);
        break;
    }
    default:
        break;
    }
}

tresult PLUGIN_API Plugin::getParameterInfo(int32 index, ParameterInfo &info) {
    if (index < 0 || index >= getParameterCount())
        return kInvalidArgument;
    info = {};
    if (index >= hostParamCount()) { // a MIDI controller's
        const int midi = index - hostParamCount();
        const int channel = midi / kMidiControllers + 1;
        const int controller = midi % kMidiControllers;
        info.id = kMidiParamBase + static_cast<ParamID>(midi);
        copyText(info.title, 128,
                 controller == kPitchBend    ? trf(Str::MidiPitchBendParam, {std::to_string(channel)})
                 : controller == kAfterTouch ? trf(Str::MidiAftertouchParam, {std::to_string(channel)})
                                             : trf(Str::MidiCcParam, {std::to_string(controller),
                                                                      std::to_string(channel)}));
        info.defaultNormalizedValue = midiParamDefault(info.id);
        info.unitId = kRootUnitId;
        info.flags = ParameterInfo::kIsHidden; // not read-only: the host sets them, which is how controllers come in
        return kResultOk;
    }
    index = hostParamIndex(index); // the params hosts are given, then the MIDI controllers'
    const ParamDef &def = paramTable()[index];
    info.id = def.id;
    copyText(info.title, 128, def.name);
    info.stepCount = stepCount(index);
    info.defaultNormalizedValue = toNormalized(index, def.def);
    info.unitId = units().unitOf[index];
    if (def.flags & CLAP_PARAM_IS_AUTOMATABLE)
        info.flags |= ParameterInfo::kCanAutomate;
    if ((def.flags & CLAP_PARAM_IS_ENUM) && info.stepCount > 0)
        info.flags |= ParameterInfo::kIsList;
    return kResultOk;
}

tresult PLUGIN_API Plugin::getParamStringByValue(ParamID id, ParamValue normalized, String128 string) {
    char text[128] = {};
    if (isMidiParam(id)) {
        std::snprintf(text, sizeof(text), "%.0f %%", std::clamp(normalized, 0.0, 1.0) * 100.0);
    } else {
        const int index = indexOfId(id);
        if (index < 0 || !formatParam(index, toPlain(index, normalized), text, sizeof(text)))
            return kInvalidArgument;
    }
    copyText(string, 128, text);
    return kResultOk;
}

tresult PLUGIN_API Plugin::getParamValueByString(ParamID id, TChar *string, ParamValue &normalized) {
    const std::string text = utf8Text(string);
    if (isMidiParam(id)) {
        char *end = nullptr;
        const double percent = std::strtod(text.c_str(), &end);
        if (end == text.c_str())
            return kResultFalse;
        normalized = std::clamp(percent / 100.0, 0.0, 1.0);
        return kResultOk;
    }
    const int index = indexOfId(id);
    double plain = 0.0;
    if (index < 0 || !parseParam(index, text.c_str(), &plain))
        return kResultFalse;
    normalized = toNormalized(index, plain);
    return kResultOk;
}

ParamValue PLUGIN_API Plugin::normalizedParamToPlain(ParamID id, ParamValue normalized) {
    const int index = indexOfId(id);
    return index >= 0 ? toPlain(index, normalized) : normalized;
}

ParamValue PLUGIN_API Plugin::plainParamToNormalized(ParamID id, ParamValue plain) {
    const int index = indexOfId(id);
    return index >= 0 ? toNormalized(index, plain) : plain;
}

ParamValue PLUGIN_API Plugin::getParamNormalized(ParamID id) {
    if (isMidiParam(id))
        return midiParamDefault(id);
    const int index = indexOfId(id);
    return index >= 0 ? toNormalized(index, value(index)) : 0.0;
}

// The host keeps the controller's values in step with what it sends the processor. While processing, the processor
// takes them from process(), in their place in a block: the host's copy here may come a moment late.
tresult PLUGIN_API Plugin::setParamNormalized(ParamID id, ParamValue value) {
    const int index = indexOfId(id);
    if (index < 0)
        return isMidiParam(id) ? kResultOk : kInvalidArgument;
    if (!processing_.load()) {
        const MainLock lock(*this);
        setHostValue(index, toPlain(index, value), false);
    }
    return kResultOk;
}

tresult PLUGIN_API Plugin::setComponentHandler(IComponentHandler *handler) {
    const MainLock lock(*this);
    if (handler == handler_)
        return kResultOk;
    if (handler2_)
        handler2_->release();
    handler2_ = nullptr;
    if (handler_)
        handler_->release();
    handler_ = handler;
    if (handler_) {
        handler_->addRef();
        void *handler2 = nullptr;
        if (handler_->queryInterface(IComponentHandler2::iid, &handler2) == kResultOk)
            handler2_ = static_cast<IComponentHandler2 *>(handler2);
    }
    return kResultOk;
}

IPlugView *PLUGIN_API Plugin::createView(FIDString name) {
    const MainLock lock(*this);
    if (!name || std::strcmp(name, ViewType::kEditor) != 0 || !openEditor()) // one editor at a time
        return nullptr;
    auto *view = new View(*this);
    view_ = view;
    return view;
}

void Plugin::hostResizeEditor(uint32_t width, uint32_t height) {
    if (view_)
        view_->requestResize(width, height);
}

tresult PLUGIN_API Plugin::getMidiControllerAssignment(int32 busIndex, int16 channel, CtrlNumber controller,
                                                       ParamID &id) {
    if (busIndex != 0 || channel < 0 || channel >= kMidiChannels || controller < 0 || controller >= kMidiControllers)
        return kResultFalse;
    id = kMidiParamBase + static_cast<ParamID>(channel * kMidiControllers + controller);
    return kResultTrue;
}

tresult PLUGIN_API Plugin::getNoteExpressionInfo(int32 busIndex, int16, int32 index, NoteExpressionTypeInfo &info) {
    if (busIndex != 0 || index < 0 || index >= static_cast<int32>(std::size(kExpressions)))
        return kInvalidArgument;
    const Expression &expr = kExpressions[index];
    info = {};
    info.typeId = expr.type;
    copyText(info.title, 128, tr(expr.title));
    copyText(info.shortTitle, 128, tr(expr.title));
    info.unitId = -1;
    info.valueDesc = {expr.defaultValue, 0.0, 1.0, 0};
    info.associatedParameterId = kNoParamId;
    info.flags = expr.flags;
    return kResultOk;
}

tresult PLUGIN_API Plugin::getNoteExpressionStringByValue(int32, int16, NoteExpressionTypeID id,
                                                          NoteExpressionValue normalized, String128 string) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    char text[64] = {};
    switch (id) {
    case kVolumeTypeID:
        if (normalized <= 0.0)
            std::snprintf(text, sizeof(text), "-inf dB");
        else
            std::snprintf(text, sizeof(text), "%.1f dB", 20.0 * std::log10(4.0 * normalized));
        break;
    case kPanTypeID:
        std::snprintf(text, sizeof(text), "%+.0f %%", (normalized - 0.5) * 200.0);
        break;
    case kTuningTypeID:
        std::snprintf(text, sizeof(text), "%+.2f st", 240.0 * (normalized - 0.5));
        break;
    case kBrightnessTypeID:
    case kPressureTypeId:
        std::snprintf(text, sizeof(text), "%.0f %%", normalized * 100.0);
        break;
    default:
        return kInvalidArgument;
    }
    copyText(string, 128, text);
    return kResultOk;
}

tresult PLUGIN_API Plugin::getNoteExpressionValueByString(int32, int16, NoteExpressionTypeID id, const TChar *string,
                                                          NoteExpressionValue &normalized) {
    const std::string text = utf8Text(string);
    char *end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return kResultFalse;
    switch (id) {
    case kVolumeTypeID:
        normalized = std::pow(10.0, number / 20.0) / 4.0;
        break;
    case kPanTypeID:
        normalized = number / 200.0 + 0.5;
        break;
    case kTuningTypeID:
        normalized = number / 240.0 + 0.5;
        break;
    case kBrightnessTypeID:
    case kPressureTypeId:
        normalized = number / 100.0;
        break;
    default:
        return kInvalidArgument;
    }
    normalized = std::clamp(normalized, 0.0, 1.0);
    return kResultOk;
}

// What a host with MPE or a touch surface should send: across the key, the note's tuning; along it, its brightness
// (slide); pressing it, its pressure.
tresult PLUGIN_API Plugin::getPhysicalUIMapping(int32 busIndex, int16, PhysicalUIMapList &list) {
    if (busIndex != 0)
        return kResultFalse;
    for (uint32 i = 0; i < list.count && list.map; ++i) {
        PhysicalUIMap &map = list.map[i];
        map.noteExpressionTypeID = map.physicalUITypeID == kPUIXMovement   ? kTuningTypeID
                                   : map.physicalUITypeID == kPUIYMovement ? kBrightnessTypeID
                                   : map.physicalUITypeID == kPUIPressure  ? kPressureTypeId
                                                                           : kInvalidTypeID;
    }
    return kResultTrue;
}

tresult PLUGIN_API Plugin::getUnitInfo(int32 index, UnitInfo &info) {
    const std::vector<Unit> &all = units().units;
    if (index < 0 || index >= static_cast<int32>(all.size()))
        return kInvalidArgument;
    info = {};
    info.id = all[index].id;
    info.parentUnitId = all[index].parent;
    copyText(info.name, 128, all[index].name);
    info.programListId = kNoProgramListId;
    return kResultOk;
}

FUnknown *createPlugin() {
    return static_cast<IComponent *>(new Plugin);
}

} // namespace fmvibe::vst3
