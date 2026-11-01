#include "plugin.h"

#include "factory.h"
#include "gui/controller.h"
#include "gui/editor.h"
#include "params.h"
#include "synth.h"
#include "uistrings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <shellapi.h>
#include <shlobj.h>
#include <xmmintrin.h>

namespace fmvibe {

namespace {

const char *const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

// State blob: magic, version, param count, (id, value) pairs, then the editor
// layout chunk, then (in the host's state, not in preset files) the settings chunk.
constexpr uint32_t kStateMagic = 0x38454C43;    // "CLE8"
constexpr uint32_t kLayoutMagic = 0x5459414C;   // "LAYT"
constexpr uint32_t kSettingsMagic = 0x47544553; // "SETG"
constexpr uint32_t kPresetMagic = 0x54535250;   // "PRST"
constexpr int kDefaultOversampling = 2;
constexpr size_t kMaxMacroName = 24; // bytes of a macro's name that are saved
// The most of a layout chunk the loader reads, any more is skipped: plenty of room past the most this version
// writes (see writePatch()), which is its fixed fields, every route kept (a node's output and its route into
// each node, at most) and every macro's name at full length.
constexpr uint32_t kMaxLayoutFloats = 4096;
static_assert(2 * kOps + 2 * (kFilters + kShapers) + 4 + kMaxSources + 1 + gui::kNodes * (gui::kNodes + 1) + 2 +
                  kMacros * (1 + kMaxMacroName) + 2 <=
              kMaxLayoutFloats);
constexpr uint32_t kMaxSettingsFloats = 64; // likewise for the settings chunk

int validOversampling(long factor) {
    return factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
}

// The editor's look is the user's rather than the project's: FMVibe's settings file in the roaming app
// data, read again whenever an editor opens, so a look picked in another process shows too.
std::filesystem::path userSettingsFile() {
    PWSTR appData = nullptr;
    std::filesystem::path file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        file = std::filesystem::path(appData) / L"FMVibe" / L"settings.txt";
    CoTaskMemFree(appData);
    return file;
}

struct UserSettings {
    std::string theme;
    double glow = 0.5;
    int examples = -1; // how many examples the preset folder has been given, or -1 if not known
};
UserSettings userSettings; // as the settings file has them; editors only run on the main thread

void readUserSettings() {
    userSettings = {};
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("theme="))
            userSettings.theme = line.substr(6);
        else if (line.starts_with("glowLevel=")) // a plain "glow=" came before the 50 % default: left alone
            userSettings.glow = std::clamp(std::atof(line.c_str() + 10), 0.0, 1.0);
        else if (line.starts_with("examples="))
            userSettings.examples = std::max(std::atoi(line.c_str() + 9), 0);
    }
}

void writeUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file);
    out << "theme=" << userSettings.theme << "\nglowLevel=" << userSettings.glow << '\n';
    if (userSettings.examples >= 0)
        out << "examples=" << userSettings.examples << '\n';
}
constexpr uint32_t kStateVersion = 1;

bool writeAll(const clap_ostream_t *stream, const void *data, size_t size) {
    auto *bytes = static_cast<const uint8_t *>(data);
    while (size > 0) {
        const int64_t written = stream->write(stream, bytes, size);
        if (written <= 0)
            return false;
        bytes += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

// Reads until `size` bytes have come or the stream ends, and says how many came.
size_t readUpTo(const clap_istream_t *stream, void *data, size_t size) {
    auto *bytes = static_cast<uint8_t *>(data);
    size_t done = 0;
    while (done < size) {
        const int64_t read = stream->read(stream, bytes + done, size - done);
        if (read <= 0)
            break;
        done += static_cast<size_t>(read);
    }
    return done;
}

bool readAll(const clap_istream_t *stream, void *data, size_t size) {
    return readUpTo(stream, data, size) == size;
}

// Reads a chunk of floats that starts with `magic` and its count into `out`: up to `most` of them, skipping
// any past those, so whatever follows is read from its start. Chunks only grow at their end, so what is kept
// still means what it did. False, with `out` empty, when the chunk is something else; if the stream ends
// partway, `out` keeps what came whole.
bool readChunk(const clap_istream_t *stream, uint32_t magic, uint32_t most, std::vector<float> &out) {
    out.clear();
    uint32_t chunk[2];
    if (!readAll(stream, chunk, sizeof(chunk)) || chunk[0] != magic)
        return false;
    out.resize(std::min(chunk[1], most));
    const size_t came = readUpTo(stream, out.data(), out.size() * sizeof(float));
    if (came < out.size() * sizeof(float)) {
        out.resize(came / sizeof(float));
        return true;
    }
    float skipped[64];
    for (uint32_t left = chunk[1] - static_cast<uint32_t>(out.size()); left > 0;) {
        const uint32_t n = std::min<uint32_t>(left, static_cast<uint32_t>(std::size(skipped)));
        if (!readAll(stream, skipped, n * sizeof(float)))
            break;
        left -= n;
    }
    return true;
}

// Streams over a file, for presets.
clap_istream_t fileIn(FILE *file) {
    return {file, [](const clap_istream_t *s, void *buffer, uint64_t size) -> int64_t {
                return static_cast<int64_t>(std::fread(buffer, 1, size, static_cast<FILE *>(s->ctx)));
            }};
}

clap_ostream_t fileOut(FILE *file) {
    return {file, [](const clap_ostream_t *s, const void *buffer, uint64_t size) -> int64_t {
                return static_cast<int64_t>(std::fwrite(buffer, 1, size, static_cast<FILE *>(s->ctx)));
            }};
}

// A path's text in UTF-8, or empty for one that has no UTF-8 form (a lone surrogate in a file name): the
// exception std::filesystem throws for it mustn't reach the host or the window procedure.
std::string utf8(const std::filesystem::path &path, bool generic = false) {
    try {
        const std::u8string text = generic ? path.generic_u8string() : path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception &) {
        return {};
    }
}

// The path a UTF-8 text names, or an empty one for text that isn't UTF-8 (a damaged state), for the same reason.
std::wstring widePath(std::string_view text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    } catch (const std::exception &) {
        return {};
    }
}

// Whether `a` sorts before `b`, ignoring case.
bool lessNoCase(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// The state's first part, which a preset file holds too: magic, version, param count, (id, value)
// pairs, then the editor layout chunk.
bool writePatch(const clap_ostream_t *stream, const ParamValues &values, const gui::UiLayout &ui) {
    const uint32_t header[] = {kStateMagic, kStateVersion, static_cast<uint32_t>(kParamCount)};
    if (!writeAll(stream, header, sizeof(header)))
        return false;
    for (int i = 0; i < kParamCount; ++i) {
        const uint32_t id = idOfIndex(i);
        if (!writeAll(stream, &id, sizeof(id)) || !writeAll(stream, &values[i], sizeof(values[i])))
            return false;
    }

    // Layout chunk fields, in order; later versions only append.
    std::vector<float> layout(ui.nodeX.begin(), ui.nodeX.end());
    layout.insert(layout.end(), ui.nodeY.begin(), ui.nodeY.end());
    layout.insert(layout.end(), ui.boxX.begin(), ui.boxX.end());
    layout.insert(layout.end(), ui.boxY.begin(), ui.boxY.end());
    layout.push_back(static_cast<float>(ui.selectedNode));
    layout.push_back(ui.zoom);
    layout.push_back(static_cast<float>(ui.view));
    layout.push_back(ui.sourcesCollapsed ? 1.0f : 0.0f);
    for (int source : ui.sourceOrder)
        layout.push_back(static_cast<float>(source));
    // The routes kept at 0 %, by param id.
    layout.push_back(static_cast<float>(ui.keptRoutes.size()));
    for (int index : ui.keptRoutes)
        layout.push_back(static_cast<float>(idOfIndex(index)));
    layout.push_back(static_cast<float>(ui.shapeGridX));
    layout.push_back(static_cast<float>(ui.shapeGridY));
    // The macros' names, each as its length and then its bytes.
    for (const std::string &name : ui.macroNames) {
        layout.push_back(static_cast<float>(std::min<size_t>(name.size(), kMaxMacroName)));
        for (size_t i = 0; i < name.size() && i < kMaxMacroName; ++i)
            layout.push_back(static_cast<float>(static_cast<unsigned char>(name[i])));
    }
    layout.push_back(static_cast<float>(ui.fxFolded)); // 16 bits each: exact as floats
    layout.push_back(static_cast<float>(ui.sourcesFolded));
    const uint32_t chunk[] = {kLayoutMagic, static_cast<uint32_t>(layout.size())};
    return writeAll(stream, chunk, sizeof(chunk)) && writeAll(stream, layout.data(), layout.size() * sizeof(float));
}

// Note, param-mod and note-expression events all address voices the same way.
template <class Event>
NoteRef noteRef(const Event &ev) {
    return {ev.note_id, ev.port_index, ev.channel, ev.key};
}

// The knobs of an effect slot's page, for the effect it holds (and for a reverb, its algorithm).
std::vector<int> fxPageParams(int slot, FxType type, const ParamValues &values) {
    auto at = [slot](std::initializer_list<FxParam> fields) {
        std::vector<int> indices;
        for (FxParam f : fields)
            indices.push_back(paramIndex(slot, f));
        return indices;
    };
    switch (type) {
    case FxType::Shaper:
        return at({FxParam::Enabled, FxParam::ShaperMode, FxParam::ShaperDrive, FxParam::ShaperOutput, FxParam::ShaperMix,
                   FxParam::ShaperLink, FxParam::ShaperSymmetric});
    case FxType::Delay:
        return at({FxParam::Enabled, FxParam::DelayTimeL, FxParam::DelayTimeR, FxParam::DelayFeedback,
                   FxParam::DelayTone, FxParam::DelayWidth, FxParam::DelayPingPong, FxParam::DelayMix});
    case FxType::Eq:
        return at({FxParam::Enabled, FxParam::EqLow, FxParam::EqMid, FxParam::EqHigh});
    case FxType::ParamEq:
        return at({FxParam::Enabled, FxParam::PeqFreq1, FxParam::PeqGain1, FxParam::PeqQ1, FxParam::PeqFreq2,
                   FxParam::PeqGain2, FxParam::PeqQ2});
    case FxType::Filter:
        return at({FxParam::Enabled, FxParam::FilterType, FxParam::FilterSlope, FxParam::FilterCutoff,
                   FxParam::FilterResonance, FxParam::FilterKeyTrack, FxParam::FilterDrive, FxParam::FilterMix});
    case FxType::Compressor:
        return at({FxParam::Enabled, FxParam::CompThreshold, FxParam::CompRatio, FxParam::CompKnee,
                   FxParam::CompAttack, FxParam::CompRelease, FxParam::CompMakeup, FxParam::CompMix});
    case FxType::MultiComp: // the band ratios and gains are left to the editor
        return {paramIndex(slot, FxParam::McLowFreq),  paramIndex(slot, FxParam::McHighFreq),
                mcThresholdIndex(slot, 0),             mcThresholdIndex(slot, 1),
                mcThresholdIndex(slot, 2),             paramIndex(slot, FxParam::McAttack),
                paramIndex(slot, FxParam::McRelease),  paramIndex(slot, FxParam::McMix)};
    case FxType::Reverb: // a shimmer's interval and amount in place of the pre-delay and width
        if (reverbType(values[paramIndex(slot, FxParam::ReverbType)]) == ReverbType::Shimmer)
            return at({FxParam::Enabled, FxParam::ReverbType, FxParam::ReverbSize, FxParam::ReverbDecay,
                       FxParam::ReverbDamping, FxParam::ReverbShimmerPitch, FxParam::ReverbShimmer, FxParam::ReverbMix});
        return at({FxParam::Enabled, FxParam::ReverbType, FxParam::ReverbSize, FxParam::ReverbDecay,
                   FxParam::ReverbDamping, FxParam::ReverbPreDelay, FxParam::ReverbWidth, FxParam::ReverbMix});
    case FxType::Phaser:
        return at({FxParam::Enabled, FxParam::PhaserRate, FxParam::PhaserDepth, FxParam::PhaserCenter,
                   FxParam::PhaserFeedback, FxParam::PhaserStages, FxParam::PhaserMix, FxParam::PhaserStereo});
    case FxType::Flanger:
        return at({FxParam::Enabled, FxParam::FlangerRate, FxParam::FlangerDepth, FxParam::FlangerDelay,
                   FxParam::FlangerFeedback, FxParam::FlangerMix, FxParam::FlangerStereo});
    case FxType::FreqShifter:
        return at({FxParam::Enabled, FxParam::ShiftRange, FxParam::ShiftAmount, FxParam::ShiftSpread,
                   FxParam::ShiftFeedback, FxParam::ShiftMix});
    case FxType::Disperser:
        return at({FxParam::Enabled, FxParam::DisperserFreq, FxParam::DisperserStages, FxParam::DisperserPinch,
                   FxParam::DisperserMix});
    case FxType::Widener:
        return at({FxParam::Enabled, FxParam::WidenerSize, FxParam::WidenerWidth, FxParam::WidenerHighPass,
                   FxParam::WidenerMix});
    case FxType::None:
    case FxType::Count:
        break;
    }
    return {};
}

// Pages of 8 knobs that hosts like Bitwig show on the device. Each effect in
// the chain gets a page, in chain order, for whatever it is; so does each
// source, in the order the editor shows them.
std::vector<clap_remote_controls_page_t> buildRemotePages(const ParamValues &values, const gui::UiLayout &layout) {
    std::vector<clap_remote_controls_page_t> pages;
    clap_id nextId = 0;
    auto add = [&](const char *section, const std::string &name, const std::vector<int> &indices,
                   clap_id id = CLAP_INVALID_ID) {
        clap_remote_controls_page_t page{};
        std::snprintf(page.section_name, sizeof(page.section_name), "%s", section);
        page.page_id = id != CLAP_INVALID_ID ? id : nextId++;
        std::snprintf(page.page_name, sizeof(page.page_name), "%s", name.c_str());
        std::fill(std::begin(page.param_ids), std::end(page.param_ids), CLAP_INVALID_ID);
        for (size_t i = 0; i < indices.size() && i < CLAP_REMOTE_CONTROLS_COUNT; ++i)
            page.param_ids[i] = idOfIndex(indices[i]);
        pages.push_back(page);
    };
    auto letter = [](int op) { return std::string(1, static_cast<char>('A' + op)); };

    // The macros first, as the page a host shows to begin with. Its id is its own, past the effects' (1000 on)
    // and the sources' (2000 on).
    std::vector<int> macros;
    for (int macro = 0; macro < kMacros; ++macro)
        macros.push_back(paramIndex(macro, MacroParam::Value));
    const char *const mix = tr(Str::PageMix);
    add(mix, tr(Str::PageMacros), macros, 3000);
    add(mix, tr(Str::PageGlobal),
        {paramIndex(GlobalParam::Volume), paramIndex(GlobalParam::FmAmount), paramIndex(GlobalParam::Pitch),
         paramIndex(GlobalParam::Pan)});
    add(mix, tr(Str::PagePlay),
        {paramIndex(GlobalParam::Mono), paramIndex(GlobalParam::Legato), paramIndex(GlobalParam::Glide),
         paramIndex(GlobalParam::GlideAlways), paramIndex(GlobalParam::BendRange), paramIndex(GlobalParam::Voices)});
    std::vector<int> outputs;
    for (int op = 0; op < kOps; ++op)
        outputs.push_back(paramIndex(op, OpParam::Out));
    for (int shaper = 0; shaper < kShapers; ++shaper)
        outputs.push_back(paramIndex(shaper, ShaperParam::Out));
    add(mix, tr(Str::PageOutputs), outputs);
    // Effect pages keep their slot's id whatever else changes.
    for (int slot : fxOrder(values[paramIndex(GlobalParam::FxOrder)])) {
        const FxType type = fxType(values[paramIndex(slot, FxParam::Type)]);
        if (type != FxType::None)
            add(tr(Str::ModuleFx), std::to_string(slot + 1) + " " + fxName(type), fxPageParams(slot, type, values),
                static_cast<clap_id>(1000 + slot));
    }

    for (int op = 0; op < kOps; ++op) {
        add(tr(Str::ModuleOperators), trf(Str::ParamOp, {letter(op)}),
            {paramIndex(op, OpParam::Wave), paramIndex(op, OpParam::Coarse), paramIndex(op, OpParam::Fine),
             paramIndex(op, OpParam::Detune), paramIndex(op, OpParam::Attack), paramIndex(op, OpParam::Decay),
             paramIndex(op, OpParam::Sustain), paramIndex(op, OpParam::Release)});
    }

    for (int filter = 0; filter < kFilters; ++filter) { // its own fields; what it feeds goes on the Matrix pages
        std::vector<int> fields;
        for (int f = 0; f < static_cast<int>(FilterParam::Shapers); ++f)
            fields.push_back(paramIndex(filter, FilterParam::Type) + f);
        add(tr(Str::ModuleFilters), trf(Str::ParamFilter, {std::to_string(filter + 1)}), fields);
    }
    std::vector<int> links;
    for (int from = 0; from < kFilters; ++from)
        for (int to = from + 1; to < kFilters; ++to)
            links.push_back(filterLinkIndex(from, to));
    add(tr(Str::ModuleFilters), tr(Str::PageFilterLinks), links);
    // A shaper's page: its switch, curve and knobs, then what it feeds into the filters.
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        std::vector<int> fields;
        for (int f = 0; f < static_cast<int>(ShaperParam::Mods); ++f)
            fields.push_back(paramIndex(shaper, ShaperParam::Enabled) + f);
        add(tr(Str::ModuleShapers), trf(Str::ParamShaper, {std::to_string(shaper + 1)}), fields);
    }

    // What each operator modulates, what goes into each filter and shaper, and what each filter and shaper
    // modulates.
    for (int from = 0; from < kOps; ++from) {
        std::vector<int> row;
        for (int to = 0; to < kOps; ++to)
            row.push_back(matrixIndex(from, to));
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {letter(from)}), row);
    }
    for (int filter = 0; filter < kFilters; ++filter) {
        std::vector<int> row;
        for (int op = 0; op < kOps; ++op)
            row.push_back(sendIndex(op, filter));
        add(tr(Str::ModuleMatrix), trf(Str::PageInto, {"F" + std::to_string(filter + 1)}), row);
    }
    for (int filter = 0; filter < kFilters; ++filter) {
        std::vector<int> row;
        for (int to = 0; to < kOps; ++to)
            row.push_back(filterModIndex(filter, to));
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {"F" + std::to_string(filter + 1)}), row);
    }
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        std::vector<int> into, mods;
        for (int op = 0; op < kOps; ++op) {
            into.push_back(shaperSendIndex(op, shaper));
            mods.push_back(shaperModIndex(shaper, op));
        }
        for (int filter = 0; filter < kFilters; ++filter) // and from the filters
            into.push_back(filterShaperIndex(filter, shaper));
        add(tr(Str::ModuleMatrix), trf(Str::PageInto, {"S" + std::to_string(shaper + 1)}), into);
        add(tr(Str::ModuleMatrix), trf(Str::PageModulates, {"S" + std::to_string(shaper + 1)}), mods);
    }

    // Sources with their slot depths; the targets themselves are set in the editor.
    // Pages keep their place's id, like the effects'.
    auto get = [&values](int i) { return values[i]; };
    for (int source : gui::shownSources(layout, get)) {
        auto at = [source](SourceParam f) { return paramIndex(source, f); };
        std::vector<int> fields =
            sourceType(values[at(SourceParam::Type)]) == SourceType::Lfo
                ? std::vector<int>{at(SourceParam::Shape), at(SourceParam::Rate), at(SourceParam::Sync),
                                   at(SourceParam::Division)}
                : std::vector<int>{at(SourceParam::Attack), at(SourceParam::Decay), at(SourceParam::Sustain),
                                   at(SourceParam::Release)};
        for (int slot = 0; slot < kSlots && fields.size() < CLAP_REMOTE_CONTROLS_COUNT; ++slot)
            fields.push_back(slotAmountIndex(source, slot));
        add(tr(Str::ModuleModulation), gui::sourceName(layout, get, source), fields,
            static_cast<clap_id>(2000 + source));
    }
    return pages;
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

// Telemetry handed from the audio thread to the editor. Values may come from
// different blocks, which is harmless for drawing.
struct SharedTelemetry {
    std::array<std::atomic<float>, kModulatedCount> modOffset{};
    std::array<std::atomic<float>, kMaxSources> sources{};
    std::array<std::atomic<float>, kMaxSources> lfoPhases{};
    std::array<std::atomic<float>, kMaxSources> lfoRates{};
    std::array<std::atomic<float>, kMaxSources> lfoFrom{};
    std::array<std::atomic<float>, kMaxSources> lfoTo{};
    std::array<std::atomic<float>, kOps> opLevels{};
    std::atomic<float> notePitch{0.0f};
    std::atomic<float> fxPitch{60.0f};
    std::array<std::atomic<float>, kFxSlots * kMcBands> fxReduction{};
    std::array<std::atomic<float>, kFxSlots * kSpectrumBands> fxSpectrum{};
    std::array<std::atomic<float>, kFilters * kSpectrumBands> filterSpectrum{};
    std::array<std::atomic<float>, kFxSlots> fxPhases{};
    std::array<std::atomic<float>, 2> outPeak{};
    std::atomic<float> clipHold{0.0f};
    std::atomic<int64_t> stamp{0}; // steady_clock ticks when stored, for Telemetry::age
    std::atomic<int> voices{0};
    std::atomic<float> tempo{120.0f};
    std::array<std::atomic<uint32_t>, 4> keys{};

    void store(const Telemetry &t) {
        for (size_t i = 0; i < keys.size(); ++i)
            keys[i].store(t.keys[i], std::memory_order_relaxed);
        for (int i = 0; i < kModulatedCount; ++i)
            modOffset[i].store(t.modOffset[i], std::memory_order_relaxed);
        for (int i = 0; i < kMaxSources; ++i) {
            sources[i].store(t.sources[i], std::memory_order_relaxed);
            lfoPhases[i].store(t.lfoPhases[i], std::memory_order_relaxed);
            lfoRates[i].store(t.lfoRates[i], std::memory_order_relaxed);
            lfoFrom[i].store(t.lfoFrom[i], std::memory_order_relaxed);
            lfoTo[i].store(t.lfoTo[i], std::memory_order_relaxed);
        }
        for (int i = 0; i < kOps; ++i)
            opLevels[i].store(t.opLevels[i], std::memory_order_relaxed);
        notePitch.store(t.notePitch, std::memory_order_relaxed);
        fxPitch.store(t.fxPitch, std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kMcBands; ++i)
            fxReduction[i].store(t.fxReduction[i / kMcBands][i % kMcBands], std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kSpectrumBands; ++i)
            fxSpectrum[i].store(t.fxSpectrum[i / kSpectrumBands][i % kSpectrumBands], std::memory_order_relaxed);
        for (int i = 0; i < kFilters * kSpectrumBands; ++i)
            filterSpectrum[i].store(t.filterSpectrum[i / kSpectrumBands][i % kSpectrumBands], std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots; ++i)
            fxPhases[i].store(t.fxPhases[i], std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
            outPeak[c].store(t.outPeak[c], std::memory_order_relaxed);
        clipHold.store(t.clipHold, std::memory_order_relaxed);
        voices.store(t.voices, std::memory_order_relaxed);
        tempo.store(t.tempo, std::memory_order_relaxed);
        stamp.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }

    void load(Telemetry &t) const {
        for (size_t i = 0; i < keys.size(); ++i)
            t.keys[i] = keys[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kModulatedCount; ++i)
            t.modOffset[i] = modOffset[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kMaxSources; ++i) {
            t.sources[i] = sources[i].load(std::memory_order_relaxed);
            t.lfoPhases[i] = lfoPhases[i].load(std::memory_order_relaxed);
            t.lfoRates[i] = lfoRates[i].load(std::memory_order_relaxed);
            t.lfoFrom[i] = lfoFrom[i].load(std::memory_order_relaxed);
            t.lfoTo[i] = lfoTo[i].load(std::memory_order_relaxed);
        }
        for (int i = 0; i < kOps; ++i)
            t.opLevels[i] = opLevels[i].load(std::memory_order_relaxed);
        t.notePitch = notePitch.load(std::memory_order_relaxed);
        t.fxPitch = fxPitch.load(std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kMcBands; ++i)
            t.fxReduction[i / kMcBands][i % kMcBands] = fxReduction[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots * kSpectrumBands; ++i)
            t.fxSpectrum[i / kSpectrumBands][i % kSpectrumBands] = fxSpectrum[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFilters * kSpectrumBands; ++i)
            t.filterSpectrum[i / kSpectrumBands][i % kSpectrumBands] = filterSpectrum[i].load(std::memory_order_relaxed);
        for (int i = 0; i < kFxSlots; ++i)
            t.fxPhases[i] = fxPhases[i].load(std::memory_order_relaxed);
        for (int c = 0; c < 2; ++c)
            t.outPeak[c] = outPeak[c].load(std::memory_order_relaxed);
        t.clipHold = clipHold.load(std::memory_order_relaxed);
        t.voices = voices.load(std::memory_order_relaxed);
        t.tempo = tempo.load(std::memory_order_relaxed);
        const auto elapsed = std::chrono::steady_clock::duration(
            std::chrono::steady_clock::now().time_since_epoch().count() - stamp.load(std::memory_order_relaxed));
        t.age = std::chrono::duration<float>(elapsed).count();
    }
};

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
    void rescanPresets() override;
    const std::vector<gui::PresetEntry> &presets() const override { return presetList_; }
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
    // Writes the example presets into a new preset folder.
    // Writes the examples from number `from` on into `folder`.
    void writeExamples(const std::filesystem::path &folder, size_t from) const;
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
    // per-segment copy.
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
    std::vector<gui::PresetEntry> presetList_;
    std::vector<std::wstring> presetFiles_;
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

    // Render up to each event's timestamp, then apply it: sample-accurate notes and automation.
    const clap_input_events_t *in = process->in_events;
    const uint32_t frames = process->frames_count;
    const uint32_t eventCount = in->size(in);
    uint32_t cursor = 0;
    for (uint32_t i = 0; i <= eventCount; ++i) {
        const clap_event_header_t *ev = i < eventCount ? in->get(in, i) : nullptr;
        const uint32_t until = ev ? std::min(ev->time, frames) : frames;
        if (until > cursor) {
            const ParamValues &params = currentParams();
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
    values_[i].store(std::clamp(ev.value, paramTable()[i].min, paramTable()[i].max));
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
    // The host keeps one voice per port/channel/key; if that key was struck
    // again later in this block, its voice is still alive.
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

    // Settings chunk fields, in order; later versions only append.
    const float stored[] = {static_cast<float>(oversampling_.load())};
    const uint32_t settingsChunk[] = {kSettingsMagic, static_cast<uint32_t>(std::size(stored))};
    if (!writeAll(stream, settingsChunk, sizeof(settingsChunk)) || !writeAll(stream, stored, sizeof(stored)))
        return false;

    // The preset the patch came from: whether it changed since, then its file's path in UTF-8.
    const std::string preset = (patchEdited() ? '\1' : '\0') + utf8(std::filesystem::path(presetFile()));
    const uint32_t presetChunk[] = {kPresetMagic, static_cast<uint32_t>(preset.size())};
    return writeAll(stream, presetChunk, sizeof(presetChunk)) && writeAll(stream, preset.data(), preset.size());
}

bool Plugin::loadState(const clap_istream_t *stream, bool settings) {
    uint32_t header[3];
    if (!readAll(stream, header, sizeof(header)) || header[0] != kStateMagic || header[1] < 1 ||
        header[1] > kStateVersion)
        return false;

    // Params missing from the state keep their defaults; unknown ids are skipped.
    const ParamTable &table = paramTable();
    ParamValues loaded;
    for (int i = 0; i < kParamCount; ++i)
        loaded[i] = table[i].def;
    for (uint32_t n = 0; n < header[2]; ++n) {
        uint32_t id;
        double value;
        if (!readAll(stream, &id, sizeof(id)) || !readAll(stream, &value, sizeof(value)))
            return false;
        if (!std::isfinite(value))
            continue;
        const int i = indexOfId(id);
        if (i >= 0)
            loaded[i] = std::clamp(value, table[i].min, table[i].max);
    }
    for (int i = 0; i < kParamCount; ++i)
        values_[i].store(loaded[i]);
    // Hosts re-read the values only when asked; any hidden flags follow in sectionsChanged().
    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES);

    // The editor layout is optional; whatever is missing keeps its default.
    gui::UiLayout layout;
    std::vector<float> stored;
    readChunk(stream, kLayoutMagic, kMaxLayoutFloats, stored);
    size_t field = 0;
    // A value that isn't a number (a damaged file) keeps its default too: clamped, it would stay NaN, and turned
    // into an index, reach far outside the arrays it indexes.
    auto next = [&](float fallback, float min, float max) {
        if (field >= stored.size())
            return fallback;
        const float value = stored[field++];
        return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
    };
    for (float &x : layout.nodeX)
        x = next(x, 0.0f, 1.0f);
    for (float &y : layout.nodeY)
        y = next(y, 0.0f, 1.0f);
    for (float &x : layout.boxX)
        x = next(x, 0.0f, 1.0f);
    for (float &y : layout.boxY)
        y = next(y, 0.0f, 1.0f);
    layout.selectedNode = static_cast<int>(next(0.0f, 0.0f, gui::kNodes - 1.0f));
    layout.zoom = next(layout.zoom, 0.5f, 3.0f);
    layout.view = static_cast<gui::RoutingView>(std::lround(next(0.0f, 0.0f, 2.0f)));
    layout.sourcesCollapsed = next(0.0f, 0.0f, 1.0f) >= 0.5f;
    // The source order lists every place once; anything else falls back to the default.
    std::array<bool, kMaxSources> listed{};
    bool valid = true;
    for (int &source : layout.sourceOrder) {
        source = static_cast<int>(std::lround(next(static_cast<float>(source), 0.0f, kMaxSources - 1.0f)));
        valid = valid && !listed[source];
        listed[source] = true;
    }
    if (!valid)
        layout.sourceOrder = gui::identitySourceOrder();
    const int kept = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kParamCount))));
    for (int k = 0; k < kept && field < stored.size(); ++k) {
        const int index = indexOfId(static_cast<clap_id>(std::lround(next(0.0f, 0.0f, 1.0e6f))));
        if (index >= 0 && std::find(layout.keptRoutes.begin(), layout.keptRoutes.end(), index) == layout.keptRoutes.end())
            layout.keptRoutes.push_back(index);
    }
    // A grid the editor doesn't offer falls back to the default.
    auto grid = [&](int fallback, const auto &offered) {
        const int lines = static_cast<int>(std::lround(next(static_cast<float>(fallback), 0.0f, 64.0f)));
        return std::find(std::begin(offered), std::end(offered), lines) != std::end(offered) ? lines : fallback;
    };
    layout.shapeGridX = grid(layout.shapeGridX, gui::kShapeGridsX);
    layout.shapeGridY = grid(layout.shapeGridY, gui::kShapeGridsY);
    for (std::string &name : layout.macroNames) { // saved before the macros came, they keep their numbers
        name.clear();
        const int length = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kMaxMacroName))));
        for (int i = 0; i < length; ++i)
            name.push_back(static_cast<char>(std::lround(next(32.0f, 32.0f, 255.0f))));
    }
    layout.fxFolded = static_cast<uint32_t>(std::lround(next(0.0f, 0.0f, 65535.0f)));
    layout.sourcesFolded = static_cast<uint32_t>(std::lround(next(0.0f, 0.0f, 65535.0f)));
    // A preset leaves the editor as it is (its size, the modulation panel, the shape grid) from the start,
    // so its window isn't resized to the preset's and back.
    const gui::UiLayout before = layout_;
    layout_ = layout;
    if (!settings)
        keepEditorLayout(before);

    // The settings are optional too: a project saved before them gets the defaults.
    if (settings) {
        std::vector<float> values;
        readChunk(stream, kSettingsMagic, kMaxSettingsFloats, values);
        const float factor = values.empty() || !std::isfinite(values[0]) ? kDefaultOversampling : values[0];
        oversampling_.store(validOversampling(std::lround(factor)));
        restartIfNeeded();

        // Which preset it was. Without that, it shows Init, changed unless it is Init.
        std::string preset;
        uint32_t presetChunk[2];
        if (readAll(stream, presetChunk, sizeof(presetChunk)) && presetChunk[0] == kPresetMagic &&
            presetChunk[1] >= 1 && presetChunk[1] <= 4096) {
            preset.resize(presetChunk[1]);
            if (!readAll(stream, preset.data(), preset.size()))
                preset.clear();
        }
        if (!preset.empty()) {
            setPreset(widePath(std::string_view(preset).substr(1)), preset[0] != '\0');
        } else {
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
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        folder = std::wstring(documents) + L"\\FMVibe\\presets";
    CoTaskMemFree(documents);
    return folder;
}

// The first scan makes the folder, with the examples in a subfolder of their own; a folder made before
// later examples came gets those once. The settings file counts the examples given, so deleted ones
// stay gone.
void Plugin::rescanPresets() {
    namespace fs = std::filesystem;
    presetList_.clear();
    presetFiles_.clear();
    const std::wstring name = presetFolder();
    if (name.empty())
        return;
    const fs::path folder(name);
    std::error_code error;
    const bool fresh = !fs::exists(folder, error) && !error;
    readUserSettings();
    const size_t given = fresh                        ? 0
                         : userSettings.examples >= 0 ? static_cast<size_t>(userSettings.examples)
                                                      : kFirstExamples;
    const size_t examples = factoryPresets().size();
    if (!error && given < examples) {
        fs::create_directories(folder / L"Factory", error);
        if (!error) {
            writeExamples(folder / L"Factory", given);
            userSettings.examples = static_cast<int>(examples);
            writeUserSettings();
        }
    }
    struct Found {
        gui::PresetEntry entry;
        fs::path path;
    };
    std::vector<Found> found;
    error.clear();
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError) || _wcsicmp(it->path().extension().c_str(), L".fmv") != 0)
            continue;
        const fs::path parent = it->path().parent_path().lexically_relative(folder);
        const std::string category = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
        std::string name = utf8(it->path().stem());
        if (name.empty()) // a name with no UTF-8 form can't be shown
            continue;
        found.push_back({{std::move(name), category}, it->path().lexically_normal()});
    }
    // The top folder's presets first, then each subfolder's, each by name.
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.category != b.entry.category)
            return a.entry.category.empty() || (!b.entry.category.empty() && lessNoCase(a.entry.category, b.entry.category));
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (const Found &f : found) {
        presetList_.push_back(f.entry);
        presetFiles_.push_back(f.path.wstring());
    }
}

int Plugin::presetIndex() const {
    if (presetFile().empty())
        return -1;
    const std::wstring current = std::filesystem::path(presetFile()).lexically_normal().wstring();
    for (size_t i = 0; i < presetFiles_.size(); ++i)
        if (_wcsicmp(presetFiles_[i].c_str(), current.c_str()) == 0)
            return static_cast<int>(i);
    return -1;
}

std::string Plugin::presetName() const {
    return presetFile().empty() ? tr(Str::PresetInit) : utf8(std::filesystem::path(presetFile()).stem());
}

bool Plugin::loadPresetAt(int index) {
    return index >= 0 && index < static_cast<int>(presetFiles_.size()) && loadPreset(presetFiles_[index]);
}

void Plugin::openPresetFolder() {
    rescanPresets(); // makes it if it isn't there yet
    const std::wstring folder = presetFolder();
    if (!folder.empty())
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Plugin::writeExamples(const std::filesystem::path &folder, size_t from) const {
    const ParamTable &table = paramTable();
    const std::vector<FactoryPreset> presets = factoryPresets();
    for (size_t n = from; n < presets.size(); ++n) {
        const FactoryPreset &preset = presets[n];
        ParamValues values;
        for (int i = 0; i < kParamCount; ++i)
            values[i] = table[i].def;
        for (const auto &[index, value] : preset.changes)
            values[index] = value;
        FILE *file = nullptr;
        const std::wstring name(preset.name.begin(), preset.name.end()); // the examples' names are ASCII
        const std::filesystem::path path = folder / (name + L".fmv");
        std::error_code error;
        if (std::filesystem::exists(path, error) || _wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            continue; // never over a preset of the same name
        const clap_ostream_t stream = fileOut(file);
        writePatch(&stream, values, exampleLayout(preset));
        std::fclose(file);
    }
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
