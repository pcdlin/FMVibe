// Minimal headless VST3 host: loads the built FMVibe.vst3 like a DAW would, checks its factory, buses and params,
// plays notes through it (with MIDI controllers and note expressions), saves and loads its state and drives its
// editor. The engine is the CLAP plugin's, which tests/smoke_host.cpp and tests/quality.cpp check at length; this
// checks what the VST3 side adds.
// Usage: fmvibe-vst3-smoke <path\to\FMVibe.vst3\Contents\x86_64-win\FMVibe.vst3>

#include "gui/controller.h" // UiLayout, to write a state with the modulation panel collapsed
#include "params.h"
#include "state.h"
#include "uistrings.h"
#include "vst3plugin.h"

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
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

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace fmvibe;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

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

std::string narrow(const char16 *text) {
    std::string out;
    for (; text && *text; ++text)
        out += *text < 128 ? static_cast<char>(*text) : '?';
    return out;
}

// The host's objects live on the stack: their counts don't matter.
#define HOST_OBJECT(Interface)                                                                                         \
    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override {                                     \
        QUERY_INTERFACE(queried, obj, FUnknown::iid, Interface)                                                       \
        QUERY_INTERFACE(queried, obj, Interface::iid, Interface)                                                      \
        *obj = nullptr;                                                                                               \
        return kNoInterface;                                                                                          \
    }                                                                                                                 \
    uint32 PLUGIN_API addRef() override { return 1; }                                                                 \
    uint32 PLUGIN_API release() override { return 1; }

// Hands out at most 5 bytes per call, so the plugin's partial read/write handling gets exercised.
struct MemoryStream final : IBStream {
    HOST_OBJECT(IBStream)
    std::vector<uint8_t> data;
    size_t pos = 0;

    tresult PLUGIN_API read(void *buffer, int32 numBytes, int32 *numBytesRead) override {
        const size_t n = std::min<size_t>({static_cast<size_t>(std::max(numBytes, 0)), 5, data.size() - pos});
        std::memcpy(buffer, data.data() + pos, n);
        pos += n;
        if (numBytesRead)
            *numBytesRead = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 numBytes, int32 *numBytesWritten) override {
        const size_t n = std::min<size_t>(static_cast<size_t>(std::max(numBytes, 0)), 5);
        const auto *bytes = static_cast<const uint8_t *>(buffer);
        data.insert(data.begin() + static_cast<ptrdiff_t>(pos), bytes, bytes + n);
        pos += n;
        if (numBytesWritten)
            *numBytesWritten = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 offset, int32 mode, int64 *result) override {
        const int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? static_cast<int64>(pos)
                                                                        : static_cast<int64>(data.size());
        pos = static_cast<size_t>(std::clamp<int64>(base + offset, 0, static_cast<int64>(data.size())));
        if (result)
            *result = static_cast<int64>(pos);
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *result) override {
        if (result)
            *result = static_cast<int64>(pos);
        return kResultOk;
    }
};

// What the plugin tells the host: its edits, restarts and changes to save.
struct Handler final : IComponentHandler, IComponentHandler2 {
    tresult PLUGIN_API queryInterface(const TUID queried, void **obj) override {
        QUERY_INTERFACE(queried, obj, FUnknown::iid, IComponentHandler)
        QUERY_INTERFACE(queried, obj, IComponentHandler::iid, IComponentHandler)
        QUERY_INTERFACE(queried, obj, IComponentHandler2::iid, IComponentHandler2)
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    struct Edit {
        char what; // b(egin), v(alue), e(nd)
        ParamID id;
        double value;
    };
    std::vector<Edit> edits;
    int32 restarts = 0; // every flag asked for
    bool dirty = false;

    tresult PLUGIN_API beginEdit(ParamID id) override {
        edits.push_back({'b', id, 0.0});
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override {
        edits.push_back({'v', id, value});
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        edits.push_back({'e', id, 0.0});
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override {
        restarts |= flags;
        return kResultOk;
    }
    tresult PLUGIN_API setDirty(TBool state) override {
        dirty = dirty || state;
        return kResultOk;
    }
    tresult PLUGIN_API requestOpenEditor(FIDString) override { return kResultFalse; }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }
};

struct Frame final : IPlugFrame {
    HOST_OBJECT(IPlugFrame)
    std::vector<std::pair<int32, int32>> resizes;
    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *size) override {
        resizes.push_back({size->getWidth(), size->getHeight()});
        return view->onSize(size);
    }
};

struct Events final : IEventList {
    HOST_OBJECT(IEventList)
    std::vector<Event> events;
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
};

struct Queue final : IParamValueQueue {
    HOST_OBJECT(IParamValueQueue)
    ParamID id = 0;
    std::vector<std::pair<int32, double>> points;
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32 &offset, ParamValue &value) override {
        if (index < 0 || index >= getPointCount())
            return kInvalidArgument;
        offset = points[static_cast<size_t>(index)].first;
        value = points[static_cast<size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32 &index) override {
        index = getPointCount();
        points.push_back({offset, value});
        return kResultOk;
    }
};

struct Changes final : IParameterChanges {
    HOST_OBJECT(IParameterChanges)
    std::vector<Queue> queues;
    void add(ParamID id, int32 offset, double value) {
        for (Queue &q : queues)
            if (q.id == id) {
                q.points.push_back({offset, value});
                return;
            }
        queues.emplace_back();
        queues.back().id = id;
        queues.back().points.push_back({offset, value});
    }
    int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues.size()); }
    IParamValueQueue *PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < getParameterCount() ? &queues[static_cast<size_t>(index)] : nullptr;
    }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override {
        index = getParameterCount();
        queues.emplace_back();
        queues.back().id = id;
        return &queues.back();
    }
};

Event noteOn(int32 offset, int16 pitch, int32 noteId, float velocity = 1.0f) {
    Event e{};
    e.sampleOffset = offset;
    e.type = Event::kNoteOnEvent;
    e.noteOn = {0, pitch, 0.0f, velocity, 0, noteId};
    return e;
}

Event noteOff(int32 offset, int16 pitch, int32 noteId) {
    Event e{};
    e.sampleOffset = offset;
    e.type = Event::kNoteOffEvent;
    e.noteOff = {0, pitch, 0.0f, noteId, 0.0f};
    return e;
}

Event expression(int32 offset, NoteExpressionTypeID type, int32 noteId, double value) {
    Event e{};
    e.sampleOffset = offset;
    e.type = Event::kNoteExpressionValueEvent;
    e.noteExpressionValue = {type, noteId, value};
    return e;
}

constexpr double kRate = 48000.0;
constexpr int32 kBlock = 480;

// The plugin, as a host holds it.
struct Instance {
    IComponent *component = nullptr;
    IAudioProcessor *processor = nullptr;
    IEditController *controller = nullptr;
    std::vector<float> left = std::vector<float>(kBlock), right = std::vector<float>(kBlock);
    // The transport each block hands over, at 120 BPM: stopped, or playing at a position it stays at.
    bool playing = false;
    double beats = 0.0;

    // One block: the events and param changes given, the audio into left and right.
    void process(Events &events, Changes &changes, int32 frames = kBlock) {
        float *channels[2] = {left.data(), right.data()};
        AudioBusBuffers out;
        out.numChannels = 2;
        out.channelBuffers32 = channels;
        ProcessContext context{};
        context.state = ProcessContext::kTempoValid;
        context.tempo = 120.0;
        context.sampleRate = kRate;
        if (playing) {
            context.state |= ProcessContext::kPlaying | ProcessContext::kProjectTimeMusicValid;
            context.projectTimeMusic = beats;
            context.projectTimeSamples = std::llround(beats * 0.5 * kRate);
        }
        ProcessData data;
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = frames;
        data.numOutputs = 1;
        data.outputs = &out;
        data.inputParameterChanges = &changes;
        data.inputEvents = &events;
        data.processContext = &context;
        processor->process(data);
    }
    void process() {
        Events events;
        Changes changes;
        process(events, changes);
    }
    // Renders `seconds` with nothing new coming in; returns the left channel.
    std::vector<float> render(double seconds) {
        std::vector<float> all;
        const int blocks = static_cast<int>(std::ceil(seconds * kRate / kBlock));
        for (int b = 0; b < blocks; ++b) {
            process();
            all.insert(all.end(), left.begin(), left.end());
        }
        return all;
    }
    double normalized(int index) { return controller->getParamNormalized(idOfIndex(index)); }
};

double peak(const std::vector<float> &samples, size_t from = 0, size_t to = SIZE_MAX) {
    double p = 0.0;
    for (size_t i = from; i < std::min(to, samples.size()); ++i)
        p = std::max(p, std::abs(static_cast<double>(samples[i])));
    return p;
}

double rms(const float *samples, size_t count) {
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i)
        sum += static_cast<double>(samples[i]) * samples[i];
    return std::sqrt(sum / std::max<size_t>(count, 1));
}

// A sine's frequency, from how often it rises through zero.
double frequency(const std::vector<float> &samples) {
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = 1; i < samples.size(); ++i)
        if (samples[i - 1] < 0.0f && samples[i] >= 0.0f) {
            if (crossings == 0)
                first = i;
            last = i;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * kRate / static_cast<double>(last - first) : 0.0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: fmvibe-vst3-smoke <FMVibe.vst3>\n");
        return 2;
    }
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }
    using InitDll = bool (*)();
    using GetFactory = IPluginFactory *(PLUGIN_API *)();
    const auto initDll = reinterpret_cast<InitDll>(GetProcAddress(lib, "InitDll"));
    const auto exitDll = reinterpret_cast<InitDll>(GetProcAddress(lib, "ExitDll"));
    const auto getFactory = reinterpret_cast<GetFactory>(GetProcAddress(lib, "GetPluginFactory"));
    check(initDll && exitDll && getFactory && initDll(), "module exports InitDll, ExitDll and GetPluginFactory");
    if (!getFactory)
        return 1;

    // --- The factory: one instrument class.
    IPluginFactory *factory = getFactory();
    IPluginFactory3 *factory3 = nullptr;
    check(factory && factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void **>(&factory3)) == kResultOk,
          "factory is an IPluginFactory3");
    PFactoryInfo factoryInfo;
    PClassInfo2 info2;
    PClassInfoW infoW;
    check(factory->getFactoryInfo(&factoryInfo) == kResultOk && std::strcmp(factoryInfo.vendor, "pcdlin") == 0 &&
              (factoryInfo.flags & PFactoryInfo::kUnicode),
          "factory info names the vendor, in Unicode");
    check(factory->countClasses() == 1 && factory3->getClassInfo2(0, &info2) == kResultOk &&
              std::strcmp(info2.category, kVstAudioEffectClass) == 0 && std::strcmp(info2.name, "FMVibe") == 0 &&
              std::strcmp(info2.subCategories, PlugType::kInstrumentSynth) == 0 &&
              std::strcmp(info2.version, FMVIBE_VERSION) == 0 && std::memcmp(info2.cid, vst3::kClassId, 16) == 0,
          "one class: FMVibe, an Instrument|Synth audio module at this version");
    check(factory3->getClassInfoUnicode(0, &infoW) == kResultOk && narrow(infoW.name) == "FMVibe" &&
              narrow(infoW.vendor) == "pcdlin",
          "the class's Unicode info");
    std::printf("      %s %s (%s)\n", info2.name, info2.version, info2.sdkVersion);

    // --- The component, which is its own controller.
    Instance plugin;
    check(factory->createInstance(info2.cid, IComponent::iid, reinterpret_cast<void **>(&plugin.component)) ==
                  kResultOk &&
              plugin.component,
          "create the component");
    if (!plugin.component)
        return 1;
    void *unknown = nullptr;
    check(factory->createInstance(info2.cid, IPlugView::iid, &unknown) != kResultOk && !unknown,
          "the class offers no interface it doesn't have");
    check(plugin.component->initialize(nullptr) == kResultOk, "initialize");
    TUID controllerId{};
    check(plugin.component->getControllerClassId(controllerId) != kResultOk,
          "no separate controller class: the component is its controller");
    plugin.component->queryInterface(IAudioProcessor::iid, reinterpret_cast<void **>(&plugin.processor));
    plugin.component->queryInterface(IEditController::iid, reinterpret_cast<void **>(&plugin.controller));
    IMidiMapping *midiMapping = nullptr;
    INoteExpressionController *expressions = nullptr;
    INoteExpressionPhysicalUIMapping *physical = nullptr;
    IUnitInfo *unitInfo = nullptr;
    IProcessContextRequirements *contextNeeds = nullptr;
    plugin.component->queryInterface(IMidiMapping::iid, reinterpret_cast<void **>(&midiMapping));
    plugin.component->queryInterface(INoteExpressionController::iid, reinterpret_cast<void **>(&expressions));
    plugin.component->queryInterface(INoteExpressionPhysicalUIMapping::iid, reinterpret_cast<void **>(&physical));
    plugin.component->queryInterface(IUnitInfo::iid, reinterpret_cast<void **>(&unitInfo));
    plugin.component->queryInterface(IProcessContextRequirements::iid, reinterpret_cast<void **>(&contextNeeds));
    check(plugin.processor && plugin.controller && midiMapping && expressions && physical && unitInfo &&
              contextNeeds,
          "IAudioProcessor, IEditController, IMidiMapping, note expressions, units and context requirements");
    if (!plugin.processor || !plugin.controller || !midiMapping || !expressions || !physical || !unitInfo)
        return 1;
    check((contextNeeds->getProcessContextRequirements() & IProcessContextRequirements::kNeedTempo) != 0,
          "it asks for the tempo");
    check((contextNeeds->getProcessContextRequirements() & IProcessContextRequirements::kNeedTransportState) != 0 &&
              (contextNeeds->getProcessContextRequirements() & IProcessContextRequirements::kNeedProjectTimeMusic) != 0,
          "and for the transport's state and position, for anchored LFOs");
    Handler handler;
    plugin.controller->setComponentHandler(&handler);

    // --- Buses: a stereo output and a 16-channel note input.
    BusInfo bus{};
    check(plugin.component->getBusCount(kAudio, kOutput) == 1 && plugin.component->getBusCount(kAudio, kInput) == 0 &&
              plugin.component->getBusCount(kEvent, kInput) == 1 &&
              plugin.component->getBusCount(kEvent, kOutput) == 0,
          "one audio output and one event input");
    check(plugin.component->getBusInfo(kAudio, kOutput, 0, bus) == kResultOk && bus.channelCount == 2 &&
              bus.busType == kMain && (bus.flags & BusInfo::kDefaultActive),
          "the output is a stereo main bus, active to begin with");
    check(plugin.component->getBusInfo(kEvent, kInput, 0, bus) == kResultOk && bus.channelCount == 16,
          "the note input takes 16 channels");
    SpeakerArrangement stereo = SpeakerArr::kStereo, mono = SpeakerArr::kMono;
    check(plugin.processor->setBusArrangements(nullptr, 0, &stereo, 1) == kResultTrue &&
              plugin.processor->setBusArrangements(nullptr, 0, &mono, 1) == kResultFalse,
          "stereo out is taken, mono refused");
    check(plugin.processor->canProcessSampleSize(kSample32) == kResultTrue &&
              plugin.processor->canProcessSampleSize(kSample64) != kResultTrue,
          "32-bit processing only");

    // --- Params: FMVibe's, with the CLAP plugin's ids, then the MIDI controllers'.
    const int32 paramCount = plugin.controller->getParameterCount();
    check(paramCount == kParamCount + vst3::kMidiParamCount, "every param and every MIDI controller's");
    std::set<ParamID> ids;
    bool sameIds = true, unitsKnown = true, roundTrips = true, defaultsRight = true;
    std::set<UnitID> unitIds;
    for (int32 u = 0; u < unitInfo->getUnitCount(); ++u) {
        UnitInfo unit{};
        if (unitInfo->getUnitInfo(u, unit) == kResultOk)
            unitIds.insert(unit.id);
    }
    UnitInfo root{};
    check(unitInfo->getUnitInfo(0, root) == kResultOk && root.id == kRootUnitId &&
              root.parentUnitId == kNoParentUnitId && unitIds.size() > 10,
          "params grouped in units under the root, as their modules are");
    for (int32 i = 0; i < paramCount; ++i) {
        ParameterInfo p{};
        if (plugin.controller->getParameterInfo(i, p) != kResultOk) {
            sameIds = false;
            continue;
        }
        ids.insert(p.id);
        unitsKnown = unitsKnown && unitIds.count(p.unitId) == 1;
        if (i >= kParamCount)
            continue;
        sameIds = sameIds && p.id == idOfIndex(i);
        const ParamDef &def = paramTable()[i];
        // Plain to normalized and back gives the value again: exactly, for a stepped param.
        for (double v : {def.min, def.def, def.max}) {
            const double back = plugin.controller->normalizedParamToPlain(
                p.id, plugin.controller->plainParamToNormalized(p.id, v));
            roundTrips = roundTrips && (p.stepCount > 0 ? back == v : std::abs(back - v) <= 1e-9 * (def.max - def.min) + 1e-12);
        }
        defaultsRight = defaultsRight && std::abs(plugin.controller->normalizedParamToPlain(p.id, p.defaultNormalizedValue) -
                                                  def.def) <= 1e-9 * (def.max - def.min) + 1e-12;
    }
    check(sameIds && ids.size() == static_cast<size_t>(paramCount), "param ids are the CLAP plugin's, all different");
    check(unitsKnown, "every param's unit is one the plugin lists");
    check(roundTrips, "values go to normalized and back unchanged, stepped ones exactly");
    check(defaultsRight, "default normalized values are the defaults");

    const int volume = paramIndex(GlobalParam::Volume);
    const ParamID volumeId = idOfIndex(volume);
    ParameterInfo volumeInfo{};
    plugin.controller->getParameterInfo(volume, volumeInfo);
    check(narrow(volumeInfo.title) == tr(Str::ParamVolume) && (volumeInfo.flags & ParameterInfo::kCanAutomate) &&
              !(volumeInfo.flags & ParameterInfo::kIsHidden),
          "Volume is named and automatable");
    String128 text{};
    ParamValue parsed = -1.0;
    const double minus6 = plugin.controller->plainParamToNormalized(volumeId, -6.0);
    check(plugin.controller->getParamStringByValue(volumeId, minus6, text) == kResultOk &&
              narrow(text) == "-6.0 dB" && plugin.controller->getParamValueByString(volumeId, text, parsed) == kResultOk &&
              std::abs(parsed - minus6) < 1e-9,
          "a value's text, and back");
    ParameterInfo voicesInfo{};
    plugin.controller->getParameterInfo(paramIndex(GlobalParam::Voices), voicesInfo);
    check(voicesInfo.stepCount == kMaxVoices - 1 && !(voicesInfo.flags & ParameterInfo::kCanAutomate),
          "Voices is stepped and not automatable");
    ParameterInfo midiInfo{};
    plugin.controller->getParameterInfo(kParamCount, midiInfo);
    check(midiInfo.id == vst3::kMidiParamBase && (midiInfo.flags & ParameterInfo::kIsHidden),
          "MIDI controllers' params are hidden");

    // A field of an effect the slot doesn't hold is hidden; once it holds that effect, the host hears to look again.
    const int delayTime = paramIndex(0, FxParam::DelayTimeR);
    const int slotType = paramIndex(0, FxParam::Type);
    ParameterInfo fieldInfo{};
    plugin.controller->getParameterInfo(delayTime, fieldInfo);
    const bool hiddenBefore = (fieldInfo.flags & ParameterInfo::kIsHidden) != 0;
    plugin.controller->setParamNormalized(idOfIndex(slotType),
                                          plugin.controller->plainParamToNormalized(
                                              idOfIndex(slotType), static_cast<double>(FxType::Delay)));
    pumpMessages(50);
    plugin.controller->getParameterInfo(delayTime, fieldInfo);
    check(hiddenBefore && !(fieldInfo.flags & ParameterInfo::kIsHidden) && (handler.restarts & kParamTitlesChanged),
          "an effect's fields show once its slot holds it, and the host is told");
    plugin.controller->setParamNormalized(idOfIndex(slotType), 0.0);
    pumpMessages(50);

    // MIDI controllers, channel pressure and pitch bend map onto params, per channel.
    ParamID bendId = 0, ccId = 0, other = 0;
    check(midiMapping->getMidiControllerAssignment(0, 0, kPitchBend, bendId) == kResultTrue &&
              midiMapping->getMidiControllerAssignment(0, 3, kCtrlFilterResonance, ccId) == kResultTrue &&
              bendId != ccId && ids.count(bendId) && ids.count(ccId) &&
              midiMapping->getMidiControllerAssignment(1, 0, kPitchBend, other) == kResultFalse &&
              midiMapping->getMidiControllerAssignment(0, 0, kCtrlProgramChange, other) == kResultFalse,
          "MIDI controllers map to params of their own, on the note input only");

    // Note expressions, and what touch surfaces and MPE should send.
    NoteExpressionTypeInfo exprInfo{};
    std::set<NoteExpressionTypeID> types;
    for (int32 i = 0; i < expressions->getNoteExpressionCount(0, 0); ++i)
        if (expressions->getNoteExpressionInfo(0, 0, i, exprInfo) == kResultOk)
            types.insert(exprInfo.typeId);
    check(types == std::set<NoteExpressionTypeID>{kVolumeTypeID, kPanTypeID, kTuningTypeID, kBrightnessTypeID,
                                                  vst3::kPressureTypeId},
          "note expressions: volume, pan, tuning, brightness and pressure");
    PhysicalUIMap maps[3] = {{kPUIXMovement, kInvalidTypeID}, {kPUIYMovement, kInvalidTypeID},
                             {kPUIPressure, kInvalidTypeID}};
    PhysicalUIMapList mapList{3, maps};
    check(physical->getPhysicalUIMapping(0, 0, mapList) == kResultTrue && maps[0].noteExpressionTypeID == kTuningTypeID &&
              maps[1].noteExpressionTypeID == kBrightnessTypeID &&
              maps[2].noteExpressionTypeID == vst3::kPressureTypeId,
          "X movement tunes a note, Y brightens it, pressure presses it");

    // --- Playing.
    ProcessSetup setup{kRealtime, kSample32, kBlock, kRate};
    check(plugin.processor->setupProcessing(setup) == kResultOk && plugin.component->setActive(true) == kResultOk &&
              plugin.processor->setProcessing(true) == kResultOk,
          "set up, activate and start processing");

    {
        Events events;
        Changes changes;
        events.events = {noteOn(0, 69, 1)};
        plugin.process(events, changes);
    }
    std::vector<float> held = plugin.render(0.5);
    const double a4 = frequency(std::vector<float>(held.begin() + 4800, held.end()));
    check(peak(held) > 0.05 && std::abs(a4 - 440.0) < 2.0, "a note sounds, an A4 at 440 Hz");

    // The MIDI-mapped pitch bend param bends it two semitones (the default Bend Range).
    {
        Events events;
        Changes changes;
        changes.add(bendId, 0, 1.0);
        plugin.process(events, changes);
    }
    const double bent = frequency(plugin.render(0.3));
    check(std::abs(bent - 440.0 * std::pow(2.0, 2.0 / 12.0)) < 3.0, "pitch bend through the MIDI mapping");
    {
        Events events;
        Changes changes;
        changes.add(bendId, 0, 8192.0 / 16383.0);
        plugin.process(events, changes);
    }

    // A tuning note expression an octave up, on the note's id.
    {
        Events events;
        Changes changes;
        events.events = {expression(0, kTuningTypeID, 1, 0.5 + 12.0 / 240.0)};
        plugin.process(events, changes);
    }
    const double tuned = frequency(plugin.render(0.3));
    check(std::abs(tuned - 880.0) < 4.0, "a tuning note expression on the note's id moves it an octave");
    {
        Events events;
        Changes changes;
        events.events = {expression(0, kTuningTypeID, 2, 0.5 - 12.0 / 240.0)};
        plugin.process(events, changes);
    }
    check(std::abs(frequency(plugin.render(0.3)) - 880.0) < 4.0, "an expression on another note's id doesn't");

    // A volume note expression at 0.125 (-6 dB) halves the level.
    const double before6 = peak(plugin.render(0.2));
    {
        Events events;
        Changes changes;
        events.events = {expression(0, kVolumeTypeID, 1, 0.125)};
        plugin.process(events, changes);
    }
    const std::vector<float> lower = plugin.render(0.2);
    const double after6 = peak(lower, lower.size() / 2);
    check(std::abs(after6 / before6 - 0.5) < 0.05, "a volume note expression sets the note's level");

    // Automation lands where in the block it is: Volume all the way down half way through.
    {
        Events events;
        Changes changes;
        changes.add(volumeId, kBlock / 2, 0.0);
        plugin.process(events, changes);
        // Over a cycle of the A4 (109 samples) at a time: Volume glides down over a few ms from its place on.
        const size_t cycle = 109;
        const double first = rms(plugin.left.data(), 2 * cycle);
        const double justBefore = rms(plugin.left.data() + kBlock / 2 - cycle, cycle);
        const double end = rms(plugin.left.data() + kBlock - cycle, cycle);
        check(first > 0.01 && std::abs(justBefore / first - 1.0) < 0.05 && end < first * 0.6,
              "a param change applies at its place in the block");
        check(std::abs(plugin.normalized(volume)) < 1e-12, "and the controller has the value");
        changes.queues.clear();
        changes.add(volumeId, 0, plugin.controller->plainParamToNormalized(volumeId, paramTable()[volume].def));
        plugin.process(events, changes);
    }

    // A param change and a note at the same place: the note hears the value (here, Mono, so the new note takes the
    // voice over).
    {
        Events events;
        Changes changes;
        changes.add(idOfIndex(paramIndex(GlobalParam::Mono)), 100, 1.0);
        events.events = {noteOn(100, 76, 3)};
        plugin.process(events, changes);
        plugin.render(0.3);
        const double e5 = frequency(plugin.render(0.3));
        check(std::abs(e5 - 659.26) < 4.0, "a param change comes before a note at the same place");
        events.events = {noteOff(0, 76, 3), noteOff(0, 69, 1)};
        changes.queues.clear();
        changes.add(idOfIndex(paramIndex(GlobalParam::Mono)), 0, 0.0);
        plugin.process(events, changes);
    }
    const std::vector<float> released = plugin.render(4.0);
    check(peak(released, released.size() - 4800) < 1e-4, "notes let go fall silent");

    // Without MIDI note ids (-1), a note off finds its note by its key.
    {
        Events events;
        Changes changes;
        events.events = {noteOn(0, 60, -1)};
        plugin.process(events, changes);
        const double on = peak(plugin.render(0.2));
        events.events = {noteOff(0, 60, -1)};
        plugin.process(events, changes);
        const std::vector<float> off = plugin.render(4.0);
        check(on > 0.05 && peak(off, off.size() - 4800) < 1e-4, "notes without ids end by their key");
    }

    // An anchored square LFO 1 at -100 % on A's output, set to 0.5, synced to quarter notes: muted in the first half
    // of each beat, full in the second, where the playing transport stands (the same position each block), not
    // from the note's start.
    {
        Events events;
        Changes changes;
        auto set = [&](int index, double plain) {
            changes.add(idOfIndex(index), 0, plugin.controller->plainParamToNormalized(idOfIndex(index), plain));
        };
        const int lfoParams[] = {paramIndex(0, SourceParam::Type), paramIndex(0, SourceParam::Shape),
                                 paramIndex(0, SourceParam::Sync), paramIndex(0, SourceParam::Division),
                                 paramIndex(0, SourceParam::Anchor), paramIndex(0, OpParam::Out),
                                 slotTargetIndex(0, 0), slotAmountIndex(0, 0)};
        set(lfoParams[0], static_cast<double>(SourceType::Lfo));
        set(lfoParams[1], static_cast<double>(LfoShape::Square));
        set(lfoParams[2], 1.0);
        set(lfoParams[3], 13.0); // 1/4
        set(lfoParams[4], 1.0);
        set(lfoParams[5], 0.5);
        set(lfoParams[6], slotTargetValue(paramIndex(0, OpParam::Out)));
        set(lfoParams[7], -1.0);
        plugin.process(events, changes);
        changes.queues.clear();
        plugin.playing = true;
        auto levelAt = [&](double beats) {
            plugin.beats = beats;
            events.events = {noteOn(0, 69, 8)};
            plugin.process(events, changes);
            events.events.clear();
            plugin.process();
            const double level = peak(plugin.render(0.01));
            events.events = {noteOff(0, 69, 8)};
            plugin.process(events, changes);
            events.events.clear();
            plugin.render(4.0);
            return level;
        };
        const double secondHalf = levelAt(2.6), firstHalf = levelAt(3.1);
        plugin.playing = false;
        for (int index : lfoParams)
            set(index, paramTable()[index].def);
        plugin.process(events, changes);
        check(secondHalf > 0.05 && firstHalf == 0.0,
              "an anchored LFO stands where the process context's position puts it");
    }

    // More param changes and events than a block sorts by their place (4096): those past them apply after them, in
    // the host's order, so a note's end past them still follows its start, and a param's last point wins.
    {
        Events events;
        Changes changes;
        const double volumeDefault = plugin.controller->plainParamToNormalized(volumeId, paramTable()[volume].def);
        const double volumeLow = plugin.controller->plainParamToNormalized(volumeId, -20.0);
        for (int32 p = 0; p < 4095; ++p)
            changes.add(volumeId, p * kBlock / 4096, volumeDefault);
        events.events = {noteOn(0, 60, 7), noteOff(kBlock / 2, 60, 7)}; // the note on is the 4096th, its end after
        plugin.process(events, changes);
        const std::vector<float> after = plugin.render(4.0);
        check(peak(after, after.size() - 4800) < 1e-4, "a note's end past the changes a block sorts still ends it");
        changes.queues.clear();
        events.events.clear();
        for (int32 p = 0; p < 4100; ++p)
            changes.add(volumeId, p * kBlock / 4100, p + 1 < 4100 ? volumeDefault : volumeLow);
        plugin.process(events, changes);
        const bool lastWins = std::abs(plugin.normalized(volume) - volumeLow) < 1e-12;
        changes.queues.clear();
        changes.add(volumeId, 0, volumeDefault);
        plugin.process(events, changes);
        check(lastWins, "and the last of a param's points past them wins");
    }

    // A block with no audio to render (a host flushing params) still takes the changes.
    {
        Events events;
        Changes changes;
        changes.add(volumeId, 0, plugin.controller->plainParamToNormalized(volumeId, -20.0));
        ProcessData data;
        data.numSamples = 0;
        data.inputParameterChanges = &changes;
        data.inputEvents = &events;
        plugin.processor->process(data);
        check(std::abs(plugin.controller->normalizedParamToPlain(volumeId, plugin.normalized(volume)) + 20.0) < 1e-9,
              "a param flush without audio is taken");
    }

    // While processing, the controller's copy from the host doesn't overrule what process() was given.
    const double volumeNow = plugin.normalized(volume);
    plugin.controller->setParamNormalized(volumeId, 0.9);
    check(plugin.normalized(volume) == volumeNow, "while processing, values come through process()");

    // More voices than are allocated: the host is asked to deactivate and activate the plugin.
    handler.restarts = 0;
    {
        Events events;
        Changes changes;
        const int voices = paramIndex(GlobalParam::Voices);
        changes.add(idOfIndex(voices), 0, plugin.controller->plainParamToNormalized(idOfIndex(voices), 64.0));
        plugin.process(events, changes);
    }
    pumpMessages(50);
    check((handler.restarts & kLatencyChanged) != 0, "raising the voice limit asks the host for a restart");
    plugin.processor->setProcessing(false);
    plugin.component->setActive(false);
    plugin.component->setActive(true);
    plugin.processor->setProcessing(true);

    // --- State: the same bytes the CLAP plugin saves, through a stream that moves 5 bytes at a time.
    plugin.processor->setProcessing(false);
    MemoryStream saved;
    check(plugin.component->getState(&saved) == kResultOk && saved.data.size() > 12 &&
              std::memcmp(saved.data.data(), "CLE8", 4) == 0,
          "save state, in the CLAP plugin's format");
    MemoryStream controllerState;
    check(plugin.controller->getState(&controllerState) == kResultOk && controllerState.data.empty(),
          "the controller keeps no state of its own");
    const double savedVolume = plugin.normalized(volume);
    plugin.controller->setParamNormalized(volumeId, 0.1);
    check(std::abs(plugin.normalized(volume) - 0.1) < 1e-12, "while not processing, the host's value is taken");
    saved.pos = 0;
    handler.restarts = 0;
    check(plugin.component->setState(&saved) == kResultOk && plugin.normalized(volume) == savedVolume,
          "load state restores the saved value");
    pumpMessages(50);
    check((handler.restarts & kParamValuesChanged) != 0, "and has the host read the values again");
    MemoryStream garbage;
    garbage.data = {1, 2, 3, 4, 5, 6, 7};
    check(plugin.component->setState(&garbage) != kResultOk, "load rejects a foreign blob");
    plugin.processor->setProcessing(true);

    // --- Editor: embed it in a (hidden) host window, resize it, drag the Volume knob, close.
    IPlugView *view = plugin.controller->createView(ViewType::kEditor);
    check(view && view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue &&
              view->isPlatformTypeSupported(kPlatformTypeNSView) != kResultTrue,
          "an editor view for HWND");
    check(!plugin.controller->createView(ViewType::kEditor), "one editor at a time");
    if (view) {
        Frame frame;
        view->setFrame(&frame);
        HWND parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 700, nullptr, nullptr,
                                      nullptr, nullptr);
        ViewRect size;
        check(view->getSize(&size) == kResultOk && size.getWidth() > 0 && size.getHeight() > 0 &&
                  view->attached(parent, kPlatformTypeHWND) == kResultOk,
              "the editor opens inside a host window");
        ViewRect wanted(0, 0, 900, 100);
        check(view->canResize() == kResultTrue && view->checkSizeConstraint(&wanted) == kResultTrue &&
                  wanted.getWidth() == 900 && wanted.getHeight() == 752 && view->onSize(&wanted) == kResultOk,
              "resizing keeps the aspect ratio");

        // At 900 px wide the view is drawn at 0.75 scale; the Volume knob sits at (201, 64) in view units.
        HWND editor = GetWindow(parent, GW_CHILD);
        const double volumeBefore = plugin.normalized(volume);
        handler.edits.clear();
        if (editor) {
            SendMessageW(editor, WM_PAINT, 0, 0);
            pumpMessages(100);
            SendMessageW(editor, WM_MOUSEMOVE, 0, MAKELPARAM(151, 48));
            SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(151, 48));
            SendMessageW(editor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(151, 28));
            SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(151, 28));
            SendMessageW(editor, WM_PAINT, 0, 0);
            pumpMessages(100);
        }
        const double volumeAfter = plugin.normalized(volume);
        const bool gesture = handler.edits.size() >= 3 && handler.edits.front().what == 'b' &&
                             handler.edits.back().what == 'e' &&
                             std::all_of(handler.edits.begin(), handler.edits.end(),
                                         [&](const Handler::Edit &e) { return e.id == volumeId; }) &&
                             handler.edits[handler.edits.size() - 2].what == 'v' &&
                             std::abs(handler.edits[handler.edits.size() - 2].value - volumeAfter) < 1e-12;
        check(editor && volumeAfter > volumeBefore && gesture,
              "dragging a knob changes the param and tells the host (begin, value, end)");

        // The host hands the processor the old value a little later: it is not taken.
        {
            Events events;
            Changes changes;
            changes.add(volumeId, 0, volumeBefore);
            plugin.process(events, changes);
        }
        check(plugin.normalized(volume) == volumeAfter, "a value from the host just after the plugin told it one is not taken");

        // A state whose modulation panel is collapsed makes the editor shorter: the host is asked to resize it.
        {
            plugin.processor->setProcessing(false);
            ParamValues values;
            for (int i = 0; i < kParamCount; ++i)
                values[i] = paramTable()[i].def;
            gui::UiLayout layout;
            layout.sourcesCollapsed = true;
            MemoryStream collapsed;
            const clap_ostream_t out{&collapsed, [](const clap_ostream_t *s, const void *buffer, uint64_t n) -> int64_t {
                                         auto *stream = static_cast<MemoryStream *>(s->ctx);
                                         const auto *bytes = static_cast<const uint8_t *>(buffer);
                                         stream->data.insert(stream->data.end(), bytes, bytes + n);
                                         return static_cast<int64_t>(n);
                                     }};
            writePatch(&out, values, layout);
            frame.resizes.clear();
            check(plugin.component->setState(&collapsed) == kResultOk && frame.resizes.size() == 1 &&
                      frame.resizes[0].first == 900 && frame.resizes[0].second < 752,
                  "a shorter editor asks the host for a smaller window");
            plugin.processor->setProcessing(true);
        }

        check(view->removed() == kResultOk && !IsWindow(editor), "closing the editor destroys its window");
        view->release();
        DestroyWindow(parent);
        IPlugView *again = plugin.controller->createView(ViewType::kEditor);
        check(again != nullptr, "the editor opens again after its view is gone");
        if (again)
            again->release();
    }

    plugin.processor->setProcessing(false);
    plugin.component->setActive(false);
    plugin.controller->setComponentHandler(nullptr);
    plugin.component->terminate();
    for (FUnknown *interface : std::initializer_list<FUnknown *>{plugin.processor, plugin.controller, midiMapping,
                                                                 expressions, physical, unitInfo, contextNeeds})
        if (interface)
            interface->release();
    check(plugin.component->release() == 0, "the last reference frees the plugin");
    factory3->release();
    check(exitDll(), "ExitDll");
    FreeLibrary(lib);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
