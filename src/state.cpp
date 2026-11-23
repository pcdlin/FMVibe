#include "state.h"

#include "gui/controller.h"
#include "warp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <cstring>
#include <vector>

namespace fmvibe {

namespace {

// State blob: magic, version, param count, (id, value) pairs, then the editor
// layout chunk, then (in the host's state, not in preset files) the settings chunk.
constexpr uint32_t kStateMagic = 0x38454C43;    // "CLE8"
constexpr uint32_t kLayoutMagic = 0x5459414C;   // "LAYT"
constexpr uint32_t kSettingsMagic = 0x47544553; // "SETG"
constexpr uint32_t kPresetMagic = 0x54535250;   // "PRST"
// Bytes of a preset's info that are saved: what can be typed, three times over for UTF-8 from a file.
constexpr size_t kMaxInfoAuthor = 3 * gui::kMaxPresetAuthor;
// A category's stays as it was saved since the info came in (20 characters), longer than a new one can be typed now
// (gui::kMaxPresetCategory), so presets and projects with a longer one keep it whole.
constexpr size_t kMaxInfoCategory = 3 * 20;
constexpr size_t kMaxInfoDescription = 3 * gui::kMaxPresetDescription;
// The most of a layout chunk the loader reads, any more is skipped: plenty of room past the most this version
// writes (see layoutFields()), which is its fixed fields, every route kept (a node's output and its route into
// each node, at most), every macro's name at full length, every route's wire bent and every empty modulation.
constexpr uint32_t kMaxLayoutFloats = 4096;
constexpr int kMaxRoutes = gui::kNodes * (gui::kNodes + 1);
static_assert(2 * kOps + 2 * (kFilters + kShapers) + 4 + kMaxSources + 1 + kMaxRoutes + 2 +
                  kMacros * (1 + gui::kMaxMacroName) + 2 + 3 + 1 + 2 * kMaxRoutes + 1 + 2 * gui::kMaxEmptyMods + 2 + 1 +
                  2 + kFxSplits + 1 + 5 + 1 + 3 + kMaxInfoAuthor + kMaxInfoCategory + kMaxInfoDescription + 1 <=
              kMaxLayoutFloats);
constexpr uint32_t kMaxSettingsFloats = 64; // likewise for the settings chunk
// The most bytes of a preset chunk read and written: its flag and a preset file's path in UTF-8, past the longest a
// Windows path's 32767 characters take for all but the rarest scripts.
constexpr size_t kMaxPresetChunk = 65536;
// 2: Volume gives back the 12 dB the voices go into the effects with, so a version 1 Volume loads 12 dB lower.
// 3: Legato acts in poly too (it was on to begin with, doing nothing there), so an earlier state in poly loads it off.
// 4: An envelope's attack, decay and release go on below 1 ms to 0 ms, their range reaching below 0 (see
//    kMinEnvNorm). Their values keep their meaning, but a modulation's depth is a share of its target's range, so an
//    earlier state's depths on them load smaller, to swing them as far as they did.
// 5: An operator's ratio is one param, Ratio, on a log curve; it was Coarse (stepped) times 1 + Fine. An earlier
//    state's ratio loads from those, and their modulations move onto Ratio (see ratioFromRetired()).
// 6: The three-band compressor is retired for the three-band dynamics, so an earlier state's slot holding one loads as
//    a three-band dynamics doing what it did (see loadRetiredMultiComp()). A later state holding one (a host can still
//    set the type) loads as it was saved, so a state loads back what was saved.
// 7: Glide stops at 6 s (kMaxGlideTime) rather than 20 s. An earlier state's longer glide loads as 6 s, and a depth on
//    Glide loads larger by as much as its range shrank, swinging it as far as before.
// 8: The chain has 32 slots, its order a param a place (see fxOrder()); it was one number, the rank of the first 16
//    slots' order (RetiredFxOrder), which an earlier state's order loads from.
// 9: The effects always run as the effects graph, whose wires alone put them in order: the chain is a line of it. An
//    earlier state that doesn't wire the output (one saved with the effects as a chain) loads its chain's order as the
//    line from the input to the output (see fxChainWires()), and the chain's places do nothing.
// 10: The three-band dynamics' Above and Below thresholds reach down to -80 dB (kMdThresholdMinDb) rather than -60.
//    Their values keep their meaning, but a modulation's depth is a share of its target's range, so an earlier state's
//    depths on them load smaller, to swing them as far as they did.
// 11: The Random source is a random LFO, with the LFO's Rate, Sync and clock (see SourceType), and the LFO's Random
//    shape is retired for it. An earlier state's Random source loads as one standing still (unsynced, at 0 Hz, on
//    its own per voice), taking a value for each note as it did, and its LFO with the Random shape as a Random source
//    running as the LFO ran (see loadRandomSources()).
// 12: The Curve source is an LFO of a drawn shape, and the LFO's Custom shape is retired for it. An earlier state's
//    LFO with the Custom shape loads as a Curve source running as the LFO ran, its points and polarity kept (see
//    loadCurveSources()).
// 13: An operator's Unison Detune reaches 200 cents rather than 100 (kMaxUnisonDetune), so an earlier state's
//    depths on it load half as deep, to swing it as far as they did. And the unison copies' Unison Pan, new with it,
//    is Alternate to begin with: an earlier state, which has none, loads Ramp, its copies from left to right as they
//    played. And a source's synced Division reaches two note values slower, 8/1T and 8/1 (-1 and -2, see
//    kSlowestDivision), so its stored values keep their note values, but an earlier state's depths on it load smaller
//    (by 26 steps over 28), to step it as far as they did.
// 14: A pattern's curves wrap around its end, the last point's segment going on to the first point; they held their
//    first and last points' values before and after them. An earlier state's bend and mod wheel curves load with points
//    at the pattern's start and end that hold those values, playing as they played (see holdSeqCurveEnds()).
// 15: An operator's Sync warp plays its wave up to 16 times a cycle rather than 8 (kMaxSyncWarp), so an earlier state's
//    Warp Amount on an operator with the Sync warp, and the depths on it, load 7/15 as large, to play as they did.
// 16: There are 32 source places rather than 16 (kMaxSources). Sources are stored by the numbers they had, the places
//    added later after the operators (see storedSource()), so an earlier state loads as it was saved; the version
//    rises so that an earlier build refuses a state whose later places it would leave out.
// 17: In Key and Latch the keys transpose the sequencer's pattern from C4 (kSeqTransposeFrom), the scale's Root setting
//    the scale alone; they transposed it from Root. An earlier state playing from a key with Root off C4 loads that
//    difference into Octave and Transpose, playing as it played (see transposeFromRoot()).
//    And a sequencer note may bend (see SeqNoteBendParam): an earlier state has no bends and loads as it was saved,
//    and the version's rise has an earlier build refuse a state whose bends it would leave out.
// One version a release at most: what changes between two tagged releases goes into a single step, so the version
// rises once from one release to the next (see AGENTS.md, Plugin facts).
constexpr uint32_t kStateVersion = 17;

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

// How a layout field takes a value read back: as it is, rounded, or cut to a whole number.
struct AsIs {
    float operator()(float value) const { return value; }
};

template <class T>
T rounded(float value) {
    return static_cast<T>(std::lround(value));
}

template <class T>
T truncated(float value) {
    return static_cast<T>(value);
}

// A check of a value read back, which a field without one doesn't need.
struct AnyValue {
    template <class T>
    bool operator()(const T &) const {
        return true;
    }
};

// A value the editor offers, one of `offered`.
template <size_t N>
auto oneOf(const int (&offered)[N]) {
    return [&offered](int value) { return std::find(std::begin(offered), std::end(offered), value) != std::end(offered); };
}

bool everyPlaceOnce(const std::array<int, kMaxSources> &order) {
    std::array<bool, kMaxSources> listed{};
    for (int source : order) {
        if (source < 0 || source >= kMaxSources || listed[source])
            return false;
        listed[source] = true;
    }
    return true;
}

// Puts each layout field into the chunk, as a float.
struct LayoutWriter {
    std::vector<float> &out;

    template <class T, class Take = AsIs, class Check = AnyValue>
    void number(const T &value, float, float, Take = {}, Check = {}) {
        out.push_back(static_cast<float>(value));
    }
    template <class T, size_t N, class Take = AsIs, class Check = AnyValue>
    void numbers(const std::array<T, N> &values, float min, float max, Take take = {}, Check = {}) {
        for (const T &value : values)
            number(value, min, max, take);
    }
    // Values `from` to `to` of an array whose parts are fields apart, which check() then checks as a whole.
    template <class T, size_t N, class Take = AsIs>
    void numbers(const std::array<T, N> &values, size_t from, size_t to, float min, float max, Take take = {}) {
        for (size_t i = from; i < to; ++i)
            number(values[i], min, max, take);
    }
    template <class T, class Check>
    void check(const T &, const T &, Check) {}
    // 16 bits of a mask from `shift` on, exact as a float.
    void bits(const uint32_t &mask, int shift) { out.push_back(static_cast<float>(mask >> shift & 0xFFFFu)); }
    // Param indices, as their count and then their ids.
    void paramIds(const std::vector<int> &indices) {
        out.push_back(static_cast<float>(indices.size()));
        for (int index : indices)
            out.push_back(static_cast<float>(idOfIndex(index)));
    }
    // Text, as its length (up to `most` bytes, cut between whole UTF-8 characters) and then its bytes.
    void text(const std::string &text, size_t most) {
        size_t length = std::min(text.size(), most);
        while (length > 0 && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            --length;
        out.push_back(static_cast<float>(length));
        for (size_t i = 0; i < length; ++i)
            out.push_back(static_cast<float>(static_cast<unsigned char>(text[i])));
    }
    // Values by param, as their count and then each param's id and its value.
    void paramValues(const std::vector<std::pair<int, float>> &values, float, float) {
        out.push_back(static_cast<float>(values.size()));
        for (const auto &[index, value] : values) {
            out.push_back(static_cast<float>(idOfIndex(index)));
            out.push_back(value);
        }
    }
    // Empty modulations, as their count and then each one's source + 1 (by its stored number, see storedSource()) and
    // its target's id + 1 (0 for none).
    void emptyMods(const std::vector<std::pair<int, int>> &mods) {
        out.push_back(static_cast<float>(mods.size()));
        for (const auto &[source, target] : mods) {
            out.push_back(source < 0 ? 0.0f : static_cast<float>(storedSource(source) + 1));
            out.push_back(target < 0 ? 0.0f : static_cast<float>(idOfIndex(target)) + 1.0f);
        }
    }
};

// Takes each layout field back from the chunk, into a layout that starts with the defaults: a field the chunk
// doesn't reach keeps its default.
struct LayoutReader {
    const std::vector<float> &stored;
    size_t field = 0;

    // A value that isn't a number (a damaged file) keeps its default too: clamped, it would stay NaN, and turned
    // into an index, reach far outside the arrays it indexes.
    float next(float fallback, float min, float max) {
        if (field >= stored.size())
            return fallback;
        const float value = stored[field++];
        return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
    }

    // Clamped to `min`..`max` and taken by `take`; a value `check` refuses keeps the default.
    template <class T, class Take = AsIs, class Check = AnyValue>
    void number(T &value, float min, float max, Take take = {}, Check check = {}) {
        const T fallback = value;
        value = take(next(static_cast<float>(value), min, max));
        if (!check(value))
            value = fallback;
    }
    // Each one as above; values `check` refuses together keep the defaults.
    template <class T, size_t N, class Take = AsIs, class Check = AnyValue>
    void numbers(std::array<T, N> &values, float min, float max, Take take = {}, Check check = {}) {
        const std::array<T, N> fallback = values;
        for (T &value : values)
            number(value, min, max, take);
        if (!check(values))
            values = fallback;
    }
    // Values `from` to `to` of an array whose parts are fields apart, which check() then checks as a whole.
    template <class T, size_t N, class Take = AsIs>
    void numbers(std::array<T, N> &values, size_t from, size_t to, float min, float max, Take take = {}) {
        for (size_t i = from; i < to; ++i)
            number(values[i], min, max, take);
    }
    // What `valid` refuses falls back to `fallback`.
    template <class T, class Check>
    void check(T &value, const T &fallback, Check valid) {
        if (!valid(value))
            value = fallback;
    }
    // 16 bits of a mask from `shift` on; the others are left as they are.
    void bits(uint32_t &mask, int shift) {
        const auto part = static_cast<uint32_t>(std::lround(next(static_cast<float>(mask >> shift & 0xFFFFu), 0.0f, 65535.0f)));
        mask = (mask & ~(0xFFFFu << shift)) | part << shift;
    }
    // Ids that name no param, and repeated ones, are left out.
    void paramIds(std::vector<int> &indices) {
        const int count = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kParamCount))));
        for (int k = 0; k < count && field < stored.size(); ++k) {
            const int index = indexOfId(static_cast<clap_id>(std::lround(next(0.0f, 0.0f, 1.0e6f))));
            if (index >= 0 && std::find(indices.begin(), indices.end(), index) == indices.end())
                indices.push_back(index);
        }
    }
    void text(std::string &text, size_t most) {
        text.clear();
        const int length = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(most))));
        for (int i = 0; i < length; ++i)
            text.push_back(static_cast<char>(std::lround(next(32.0f, 32.0f, 255.0f))));
    }
    // Each value clamped to `min`..`max`; one whose id names no param, or a param listed already, is left out, and
    // so is one that isn't a number.
    void paramValues(std::vector<std::pair<int, float>> &values, float min, float max) {
        values.clear();
        const int count = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kParamCount))));
        for (int k = 0; k < count && field < stored.size(); ++k) {
            const int index = indexOfId(static_cast<clap_id>(std::lround(next(0.0f, 0.0f, 1.0e6f))));
            const float value = next(std::numeric_limits<float>::quiet_NaN(), min, max);
            if (index >= 0 && std::isfinite(value) &&
                std::none_of(values.begin(), values.end(), [&](const auto &v) { return v.first == index; }))
                values.push_back({index, value});
        }
    }
    // A source that isn't one, or a target that isn't a param modulation can move, reads as none.
    void emptyMods(std::vector<std::pair<int, int>> &mods) {
        mods.clear();
        const int count = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(gui::kMaxEmptyMods))));
        for (int k = 0; k < count && field < stored.size(); ++k) {
            const int saved = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kAllSources)))) - 1;
            const int source = saved < 0 ? -1 : sourceOfStored(saved);
            const long id = std::lround(next(0.0f, 0.0f, 1.0e6f)) - 1;
            int target = id < 0 ? -1 : indexOfId(static_cast<clap_id>(id));
            for (int op = 0; op < kOps; ++op) // from before Ratio: a Coarse or Fine moves onto it, as the slots do
                if (target == paramIndex(op, OpParam::RetiredCoarse) || target == paramIndex(op, OpParam::RetiredFine))
                    target = paramIndex(op, OpParam::Ratio);
            mods.push_back({source, isModTarget(target) ? target : -1});
        }
    }
};

// The layout chunk's fields, in order; later versions only append. Writing (LayoutWriter) and reading
// (LayoutReader) both go by this list: each field with the range a value read back is clamped to, how the field
// takes it (as it is by default) and what it must pass, or the field keeps its default.
template <class Io, class Layout>
void layoutFields(Io &io, Layout &ui) {
    io.numbers(ui.nodeX, 0.0f, 1.0f);
    io.numbers(ui.nodeY, 0.0f, 1.0f);
    io.numbers(ui.boxX, 0.0f, 1.0f);
    io.numbers(ui.boxY, 0.0f, 1.0f);
    io.number(ui.selectedNode, 0.0f, gui::kNodes - 1.0f, truncated<int>);
    io.number(ui.zoom, 0.5f, 3.0f);
    io.number(ui.view, 0.0f, static_cast<float>(gui::RoutingView::Seq), rounded<gui::RoutingView>);
    io.number(ui.retiredSourcesCollapsed, 0.0f, 1.0f, [](float value) { return value >= 0.5f; }); // does nothing
    // The source order: where the first kFirstSources places show, the rest at the end (see below).
    io.numbers(ui.sourceOrder, 0, kFirstSources, 0.0f, kMaxSources - 1.0f, rounded<int>);
    // The routes kept at 0 %, by param id.
    io.paramIds(ui.keptRoutes);
    // A grid the editor doesn't offer falls back to the default.
    io.number(ui.shapeGridX, 0.0f, 64.0f, rounded<int>, oneOf(gui::kShapeGridsX));
    io.number(ui.shapeGridY, 0.0f, 64.0f, rounded<int>, oneOf(gui::kShapeGridsY));
    // The macros' names, each as its length and then its bytes. Saved before the macros came, they keep their
    // numbers.
    for (auto &name : ui.macroNames)
        io.text(name, gui::kMaxMacroName);
    io.number(ui.fxFolded, 0.0f, 65535.0f, rounded<uint32_t>); // 16 bits each: exact as floats
    io.bits(ui.sourcesFolded, 0); // the first kFirstSources places' (the rest at the end)
    io.number(ui.keysCollapsed, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    io.number(ui.opTab, 0.0f, static_cast<float>(gui::InspectorTab::Count) - 1.0f, rounded<gui::InspectorTab>);
    io.number(ui.scopeCycles, 0.0f, 4.0f, rounded<int>, oneOf(gui::kScopeCycleChoices));
    // The wires dragged to bow other than they start, by route.
    io.paramValues(ui.wireBends, -gui::kMaxWireBend, gui::kMaxWireBend);
    // The modulations added in the MODS view without a source or a target yet.
    io.emptyMods(ui.emptyMods);
    // How shapes are edited: steps or points, and snapping to the grid.
    io.number(ui.shapeSteps, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    io.number(ui.shapeSnap, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    // The effect cards folded past the first 16 slots, which the 16 bits above don't reach.
    io.number(ui.fxFoldedLater, 0.0f, 65535.0f, rounded<uint32_t>);
    // The node the effects graph's inspector shows.
    io.number(ui.fxSelected, -1.0f, kFxNodes - 1.0f, rounded<int>);
    // Whether the FX view shows the graph, the band each split shows, and the splits' containers folded.
    io.number(ui.fxGraphView, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    io.numbers(ui.splitBand, 0.0f, 2.0f, rounded<int>); // 0 low, 1 high, 2 middle
    io.number(ui.splitsFolded, 0.0f, 15.0f, rounded<uint32_t>);
    // The SEQ view's roll: drawing or selecting, its snap, its lane, its lowest row and its folding.
    io.number(ui.seqDraw, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    io.number(ui.seqSnap, 0.0f, gui::kSeqSnapCount - 1.0f, rounded<int>);
    io.number(ui.seqLane, 0.0f, static_cast<float>(gui::SeqLane::Count) - 1.0f, rounded<gui::SeqLane>);
    io.number(ui.seqLow, 0.0f, 127.0f, rounded<int>);
    io.number(ui.seqFold, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    // The tempo the patch was saved as a preset at: 0 for none, or a tempo the editor offers.
    io.number(ui.savedTempo, 0.0f, static_cast<float>(gui::kMaxTempo), AsIs{},
              [](float bpm) { return bpm == 0.0f || bpm >= static_cast<float>(gui::kMinTempo); });
    // What the preset says about itself: its author, its category and its line, each as its length and its bytes.
    io.text(ui.presetInfo.author, kMaxInfoAuthor);
    io.text(ui.presetInfo.category, kMaxInfoCategory);
    io.text(ui.presetInfo.description, kMaxInfoDescription);
    // The places past the first kFirstSources: where they show, which then lists every place once (anything else falls
    // back to the default), and which are folded. A layout from before them shows them after the first, unfolded.
    io.numbers(ui.sourceOrder, kFirstSources, kMaxSources, 0.0f, kMaxSources - 1.0f, rounded<int>);
    io.check(ui.sourceOrder, gui::identitySourceOrder(), everyPlaceOnce);
    io.bits(ui.sourcesFolded, kFirstSources);
    // Whether the SEQ view's roll bends notes (its Bend tool).
    io.number(ui.seqBend, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
}

} // namespace

int validOversampling(long factor) {
    return factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
}

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

clap_istream_t memoryIn(MemoryReader &reader) {
    return {&reader, [](const clap_istream_t *s, void *buffer, uint64_t size) -> int64_t {
                MemoryReader &r = *static_cast<MemoryReader *>(s->ctx);
                const size_t n = static_cast<size_t>(std::min<uint64_t>(size, r.size - r.at));
                std::copy_n(r.data + r.at, n, static_cast<unsigned char *>(buffer));
                r.at += n;
                return static_cast<int64_t>(n);
            }};
}

clap_ostream_t memoryOut(std::vector<unsigned char> &bytes) {
    return {&bytes, [](const clap_ostream_t *s, const void *buffer, uint64_t size) -> int64_t {
                auto &to = *static_cast<std::vector<unsigned char> *>(s->ctx);
                const auto *from = static_cast<const unsigned char *>(buffer);
                to.insert(to.end(), from, from + size);
                return static_cast<int64_t>(size);
            }};
}

bool writePatch(const clap_ostream_t *stream, const ParamValues &values, const gui::UiLayout &ui, bool sparse) {
    const ParamTable &table = paramTable();
    auto written = [&](int i) { return !sparse || values[i] != table[i].def; };
    uint32_t count = 0;
    for (int i = 0; i < kParamCount; ++i)
        count += written(i) ? 1 : 0;
    const uint32_t header[] = {kStateMagic, kStateVersion, count};
    if (!writeAll(stream, header, sizeof(header)))
        return false;
    // The pairs go to the stream in one write rather than two a param: a host's stream may cost a call each.
    constexpr size_t kPair = sizeof(uint32_t) + sizeof(double);
    std::vector<unsigned char> pairs(static_cast<size_t>(count) * kPair);
    unsigned char *at = pairs.data();
    for (int i = 0; i < kParamCount; ++i) {
        if (!written(i))
            continue;
        const uint32_t id = idOfIndex(i);
        // An aux names its source by the number it is stored by (see storedSource()).
        const double value = table[i].kind == ParamKind::SlotAux ? storedAuxValue(values[i]) : values[i];
        std::memcpy(at, &id, sizeof(id));
        std::memcpy(at + sizeof(id), &value, sizeof(value));
        at += kPair;
    }
    if (!writeAll(stream, pairs.data(), pairs.size()))
        return false;

    std::vector<float> layout;
    LayoutWriter writer{layout};
    layoutFields(writer, ui);
    const uint32_t chunk[] = {kLayoutMagic, static_cast<uint32_t>(layout.size())};
    return writeAll(stream, chunk, sizeof(chunk)) && writeAll(stream, layout.data(), layout.size() * sizeof(float));
}

// A state from before Ratio: each operator's ratio from its Coarse and Fine, as they played, and the modulations on
// them moved onto Ratio. A depth was a share of Coarse's range (0 to 32) or Fine's (0 to 1); it becomes the share of
// Ratio's log range that reaches the ratio the old depth reached at its full swing from where the ratio is set.
void ratioFromRetired(ParamValues &loaded) {
    const ParamTable &table = paramTable();
    for (int op = 0; op < kOps; ++op) {
        const double coarse = loaded[paramIndex(op, OpParam::RetiredCoarse)];
        const double fine = loaded[paramIndex(op, OpParam::RetiredFine)];
        const double ratio = coarseRatio(coarse) * (1.0 + fine);
        const double norm = normFromRatio(ratio);
        loaded[paramIndex(op, OpParam::Ratio)] = norm;
        const int coarseIndex = paramIndex(op, OpParam::RetiredCoarse), fineIndex = paramIndex(op, OpParam::RetiredFine);
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTargetIndex(source, slot);
                const long id = std::lround(loaded[target]) - 1;
                const int index = id < 0 ? -1 : indexOfId(static_cast<clap_id>(id));
                if (index != coarseIndex && index != fineIndex)
                    continue;
                const int amount = slotAmountIndex(source, slot);
                const double depth = loaded[amount];
                const double reached =
                    index == coarseIndex
                        ? coarseRatio(std::clamp(coarse + depth * 32.0, 0.0, 32.0)) * (1.0 + fine)
                        : coarseRatio(coarse) * (1.0 + std::clamp(fine + depth, 0.0, 1.0));
                loaded[target] = slotTargetValue(paramIndex(op, OpParam::Ratio));
                loaded[amount] = std::clamp(normFromRatio(reached) - norm, table[amount].min, table[amount].max);
            }
    }
}

namespace {

// The three-band compressor is retired: in a state from before version 5, a slot that held one loads as a three-band
// dynamics doing what it did. Its
// crossovers, thresholds (as Above), ratios (as Down), gains, timing, mix, output and link carry over; the upward side
// stays at 1:1, the knee at the compressor's 6 dB, and the detector on peaks. Modulation aimed at its fields moves to
// theirs, and the compressor's own fields go back to their defaults.
void loadRetiredMultiComp(ParamValues &values) {
    const ParamTable &table = paramTable();
    for (int slot = 0; slot < kFxSlots; ++slot) {
        if (fxType(values[paramIndex(slot, FxParam::Type)]) != FxType::MultiComp)
            continue;
        std::vector<std::pair<int, int>> moves = {
            {paramIndex(slot, FxParam::McLowFreq), paramIndex(slot, FxParam::MdLowFreq)},
            {paramIndex(slot, FxParam::McHighFreq), paramIndex(slot, FxParam::MdHighFreq)},
            {paramIndex(slot, FxParam::McAttack), paramIndex(slot, FxParam::MdAttack)},
            {paramIndex(slot, FxParam::McRelease), paramIndex(slot, FxParam::MdRelease)},
            {paramIndex(slot, FxParam::McMix), paramIndex(slot, FxParam::MdMix)},
            {paramIndex(slot, FxParam::McOutput), paramIndex(slot, FxParam::MdOutput)},
            {paramIndex(slot, FxParam::McLink), paramIndex(slot, FxParam::MdLink)},
        };
        for (int band = 0; band < kMcBands; ++band) {
            moves.push_back({mcThresholdIndex(slot, band), mdAboveIndex(slot, band)});
            moves.push_back({mcRatioIndex(slot, band), mdDownIndex(slot, band)});
            moves.push_back({mcGainIndex(slot, band), mdGainIndex(slot, band)});
        }
        for (const auto &[from, to] : moves) {
            // A ratio (1 to 20) goes onto the dynamics' scale (see dynRatio()); a modulation's depth on it stays the
            // share of its range it was.
            const bool ratio = table[from].kind == ParamKind::CompRatio;
            const double moved = ratio ? normFromDynRatio(values[from]) : values[from];
            values[to] = std::clamp(moved, table[to].min, table[to].max);
            values[from] = table[from].def;
            for (int source = 0; source < kAllSources; ++source)
                for (int s = 0; s < kSlots; ++s)
                    if (slotTarget(values[slotTargetIndex(source, s)]) == from)
                        values[slotTargetIndex(source, s)] = static_cast<double>(idOfIndex(to)) + 1.0;
        }
        // Linked, its bands all took the low band's threshold, ratio and gain; the dynamics' bands each play their
        // own, so they take the low band's values.
        if (values[paramIndex(slot, FxParam::MdLink)] >= 0.5)
            for (int band = 1; band < kMcBands; ++band) {
                values[mdAboveIndex(slot, band)] = values[mdAboveIndex(slot, 0)];
                values[mdDownIndex(slot, band)] = values[mdDownIndex(slot, 0)];
                values[mdGainIndex(slot, band)] = values[mdGainIndex(slot, 0)];
            }
        for (int band = 0; band < kMcBands; ++band)
            values[mdUpIndex(slot, band)] = normFromDynRatio(1.0);
        values[paramIndex(slot, FxParam::MdKnee)] = kMcKnee;
        values[paramIndex(slot, FxParam::MdRms)] = 0.0;
        values[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
    }
}

// Before version 11, a Random source took a value of its own as each note started and held it, and an LFO could have the
// Random shape. The source keeps doing what it did as a random LFO standing still, which takes a value for each note,
// anywhere, at once (the LFO's fields it now uses go back to their defaults, but for Rate at 0 Hz and Sync off); the
// LFO turns into a Random source running as it did, with the same fields, Bipolar standing for its Unipolar.
void loadRandomSources(ParamValues &values) {
    const ParamTable &table = paramTable();
    for (int s = 0; s < kMaxSources; ++s) {
        const SourceType type = sourceType(values[paramIndex(s, SourceParam::Type)]);
        // An LFO's Rate at 0 was its slowest, 0.02 Hz, where it now stands still: it loads just above, as it played.
        if (type == SourceType::Lfo && values[paramIndex(s, SourceParam::Rate)] < kLeastNorm)
            values[paramIndex(s, SourceParam::Rate)] = kLeastNorm;
        if (type == SourceType::Random) {
            for (const SourceParam f : {SourceParam::Division, SourceParam::Free, SourceParam::OneShot,
                                        SourceParam::Anchor, SourceParam::RandomSmooth, SourceParam::RandomChaos})
                values[paramIndex(s, f)] = table[paramIndex(s, f)].def;
            values[paramIndex(s, SourceParam::Rate)] = 0.0;
            values[paramIndex(s, SourceParam::Sync)] = 0.0;
        } else if (type == SourceType::Lfo &&
                   std::lround(values[paramIndex(s, SourceParam::Shape)]) == static_cast<long>(LfoShape::Random)) {
            values[paramIndex(s, SourceParam::Type)] = static_cast<double>(SourceType::Random);
            values[paramIndex(s, SourceParam::Bipolar)] = values[paramIndex(s, SourceParam::Unipolar)] >= 0.5 ? 0.0 : 1.0;
            for (const SourceParam f : {SourceParam::Shape, SourceParam::Unipolar, SourceParam::Phase})
                values[paramIndex(s, f)] = table[paramIndex(s, f)].def;
        }
    }
}

// Before version 12, an LFO could have the Custom shape, drawn from points. It turns into a Curve source running as
// it did: the same fields and points (its freeform shape is the place's), Unipolar its polarity as before; its Shape
// goes back to its default.
void loadCurveSources(ParamValues &values) {
    const int shapeIndex = paramIndex(0, SourceParam::Shape);
    for (int s = 0; s < kMaxSources; ++s)
        if (sourceType(values[paramIndex(s, SourceParam::Type)]) == SourceType::Lfo &&
            std::lround(values[paramIndex(s, SourceParam::Shape)]) == static_cast<long>(LfoShape::Custom)) {
            values[paramIndex(s, SourceParam::Type)] = static_cast<double>(SourceType::Curve);
            values[paramIndex(s, SourceParam::Shape)] = paramTable()[shapeIndex].def;
        }
}

// Before version 14, a pattern's curves held their first point's value before it and their last's after it, and a point
// past the pattern's end (a pattern made shorter leaves them) still shaped the segment leading to it. A point at the
// pattern's start and one at its end keep each curve playing as it did, where the curve has room for them.
void holdSeqCurveEnds(ParamValues &values) {
    auto get = [&values](int index) { return values[index]; };
    for (int pattern = 0; pattern < kSeqPatterns; ++pattern) {
        const double steps = kSeqStepsPerBar * std::clamp(std::lround(values[paramIndex(pattern, SeqPatternParam::Length)]),
                                                          1L, static_cast<long>(kSeqBars));
        for (const SeqCurve curve : {SeqCurve::Bend, SeqCurve::ModWheel}) {
            const int count = seqCurvePointCount(get, pattern, curve);
            if (count == 0)
                continue;
            std::vector<SeqCurvePoint> points;
            for (int i = 0; i < count; ++i)
                points.push_back({values[seqCurveIndex(pattern, curve, i, PointField::X)],
                                  values[seqCurveIndex(pattern, curve, i, PointField::Y)],
                                  values[seqCurveIndex(pattern, curve, i, PointField::Curve)]});
            if (points.front().x > 0.0 && count < kSeqCurvePoints)
                points.insert(points.begin(), {0.0, points.front().y, 0.0});
            const auto past = std::find_if(points.begin(), points.end(), [&](const SeqCurvePoint &q) { return q.x > steps; });
            const bool endless = past == points.begin(); // every point past the end: the first's value throughout
            if (static_cast<int>(points.size()) < kSeqCurvePoints && !endless && std::prev(past)->x < steps) {
                const SeqCurvePoint a = *std::prev(past);
                double y = a.y;
                if (past != points.end())
                    y += (past->y - a.y) * shapeBend((steps - a.x) / (past->x - a.x), a.curve);
                points.insert(past, {steps, y, 0.0});
            }
            values[seqCurveCountIndex(pattern, curve)] = static_cast<double>(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                values[seqCurveIndex(pattern, curve, static_cast<int>(i), PointField::X)] = points[i].x;
                values[seqCurveIndex(pattern, curve, static_cast<int>(i), PointField::Y)] = points[i].y;
                values[seqCurveIndex(pattern, curve, static_cast<int>(i), PointField::Curve)] = points[i].curve;
            }
        }
    }
}

// Before version 17, the keys transposed a pattern from Root rather than C4: Transpose takes the difference, and
// Octave what Transpose can't reach, as far as its range does, a pattern playing from a key sounding as it did. One
// with the host's transport, which Root never moved, is left as it is.
void transposeFromRoot(ParamValues &values) {
    if (std::lround(values[paramIndex(SeqParam::Play)]) == static_cast<long>(SeqPlay::Host))
        return;
    const int octaveIndex = paramIndex(SeqParam::Octave), transposeIndex = paramIndex(SeqParam::Transpose);
    const ParamTable &table = paramTable();
    const long most = std::lround(table[transposeIndex].max), mostOctaves = std::lround(table[octaveIndex].max);
    long octave = std::lround(values[octaveIndex]);
    long transpose = std::lround(values[transposeIndex]) + kSeqTransposeFrom - std::lround(values[paramIndex(SeqParam::Root)]);
    for (; transpose > most && octave < mostOctaves; transpose -= 12)
        ++octave;
    for (; transpose < -most && octave > -mostOctaves; transpose += 12)
        --octave;
    values[octaveIndex] = static_cast<double>(octave);
    values[transposeIndex] = static_cast<double>(std::clamp(transpose, -most, most));
}

} // namespace

bool readParams(const clap_istream_t *stream, ParamValues &loaded) {
    uint32_t header[3];
    if (!readAll(stream, header, sizeof(header)) || header[0] != kStateMagic || header[1] < 1 ||
        header[1] > kStateVersion)
        return false;

    // Params missing from the state keep their defaults; unknown ids are skipped.
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        loaded[i] = table[i].def;
    // Before version 13 there was no Unison Pan: the copies sat from left to right, which is what a state without one
    // loads (one that has it, as saved).
    if (header[1] < 13)
        for (int op = 0; op < kOps; ++op)
            loaded[paramIndex(op, OpParam::UnisonPan)] = static_cast<double>(UniPan::Ramp);
    const int volume = paramIndex(GlobalParam::Volume);
    // Whether the state wires the effects graph's output: before version 9, only one saved with them as a graph does.
    bool outWired = false;
    const int outFirst = fxInputIndex(kFxOutNode, 0, FxInputField::From);
    // The pairs come in batches (a host's stream may cost a call each), no more of them than the count says.
    constexpr size_t kPair = sizeof(uint32_t) + sizeof(double), kBatch = 4096;
    std::vector<unsigned char> batch(std::min<size_t>(header[2], kBatch) * kPair);
    size_t inBatch = 0, taken = 0;
    for (uint32_t n = 0; n < header[2]; ++n) {
        if (taken == inBatch) {
            inBatch = std::min<size_t>(header[2] - n, kBatch);
            taken = 0;
            if (!readAll(stream, batch.data(), inBatch * kPair))
                return false;
        }
        uint32_t id;
        double value;
        std::memcpy(&id, batch.data() + taken * kPair, sizeof(id));
        std::memcpy(&value, batch.data() + taken * kPair + sizeof(id), sizeof(value));
        ++taken;
        if (!std::isfinite(value))
            continue;
        const int i = indexOfId(id);
        if (i == volume && header[1] < 2)
            value -= 12.0; // as loud as it was
        if (i >= 0)
            loaded[i] = std::clamp(value, table[i].min, table[i].max);
        if (i >= 0 && table[i].kind == ParamKind::SlotAux) // by the number it is stored by (see storedSource())
            loaded[i] = auxValueOfStored(loaded[i]);
        if (i >= outFirst && i < outFirst + kFxOutInputs * kFxInputFields && (i - outFirst) % kFxInputFields == 0 &&
            fxPortOf(value) >= 0)
            outWired = true;
    }
    if (header[1] < 3 && loaded[paramIndex(GlobalParam::Mono)] < 0.5) // as it played
        loaded[paramIndex(GlobalParam::Legato)] = 0.0;
    if (header[1] < 4) // as far as they swung their envelope times: their range was 0 to kMaxEnvNorm then
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTarget(loaded[slotTargetIndex(source, slot)]);
                if (target >= 0 && table[target].kind == ParamKind::EnvTime)
                    loaded[slotAmountIndex(source, slot)] *= kMaxEnvNorm / (table[target].max - table[target].min);
            }
    // Before version 3 (released up to 0.3), the envelopes' times, holds too, reached 0 to 1 (20 s), not kMaxEnvNorm
    // (10 s): on top of the step above, a depth on one swings it as far as it did, and so does one on a hold, whose
    // range has stayed 0 to kMaxEnvNorm since.
    if (header[1] < 3)
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTarget(loaded[slotTargetIndex(source, slot)]);
                const bool hold = target >= 0 && table[target].kind == ParamKind::TimeOrOff &&
                                  table[target].max == kMaxEnvNorm; // an operator's or an envelope's Hold
                if (target >= 0 && (table[target].kind == ParamKind::EnvTime || hold))
                    loaded[slotAmountIndex(source, slot)] /= kMaxEnvNorm;
            }
    if (header[1] < 7) // as far as they swung Glide: its range was 0 to 1 then
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot)
                if (slotTarget(loaded[slotTargetIndex(source, slot)]) == paramIndex(GlobalParam::Glide)) {
                    const int depth = slotAmountIndex(source, slot);
                    loaded[depth] = std::clamp(loaded[depth] / kMaxGlideNorm, table[depth].min, table[depth].max);
                }
    if (header[1] < 5)
        ratioFromRetired(loaded);
    if (header[1] < 6)
        loadRetiredMultiComp(loaded);
    if (header[1] < 10) // as far as they swung the three-band dynamics' thresholds (the retired compressor's, moved
                        // onto them above, had the same range): their range was kMdRetiredThresholdMinDb to 0 dB then
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTarget(loaded[slotTargetIndex(source, slot)]);
                const int fx = fxSlotOf(target);
                bool threshold = false;
                for (int band = 0; fx >= 0 && band < kMcBands; ++band)
                    threshold = threshold || target == mdAboveIndex(fx, band) || target == mdBelowIndex(fx, band);
                if (threshold)
                    loaded[slotAmountIndex(source, slot)] *= kMdRetiredThresholdMinDb / kMdThresholdMinDb;
            }
    if (header[1] < 13) // as far as they swung an operator's Unison Detune: its range was 0 to kRetiredMaxUnisonDetune
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTarget(loaded[slotTargetIndex(source, slot)]);
                bool detune = false;
                for (int op = 0; op < kOps; ++op)
                    detune = detune || target == paramIndex(op, OpParam::UnisonDetune);
                if (detune)
                    loaded[slotAmountIndex(source, slot)] *= kRetiredMaxUnisonDetune / kMaxUnisonDetune;
            }
    if (header[1] < 13) // as far as they stepped a source's Division: it reached from 4/1D (0) up only
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot) {
                const int target = slotTarget(loaded[slotTargetIndex(source, slot)]);
                if (target >= 0 && modulatedInSteps(target))
                    loaded[slotAmountIndex(source, slot)] *= kRetiredSourceDivisionSteps / kSourceDivisionSteps;
            }
    if (header[1] < 15) { // as many times through a cycle as a Sync warp played: it reached kRetiredMaxSyncWarp
        constexpr double scale = (kRetiredMaxSyncWarp - 1.0) / (kMaxSyncWarp - 1.0);
        for (int op = 0; op < kOps; ++op) {
            if (std::lround(loaded[paramIndex(op, OpParam::Warp)]) != static_cast<long>(OpWarp::Sync))
                continue;
            const int amount = paramIndex(op, OpParam::WarpAmount);
            loaded[amount] *= scale;
            for (int source = 0; source < kAllSources; ++source)
                for (int slot = 0; slot < kSlots; ++slot)
                    if (slotTarget(loaded[slotTargetIndex(source, slot)]) == amount)
                        loaded[slotAmountIndex(source, slot)] *= scale;
        }
    }
    if (header[1] < 8) { // the chain's order onto its places
        const int retired = paramIndex(GlobalParam::RetiredFxOrder);
        for (const auto &[index, value] : fxOrderChanges(fxOrderFromRetired(loaded[retired])))
            loaded[index] = value;
        loaded[retired] = table[retired].def;
    }
    if (header[1] < 11)
        loadRandomSources(loaded);
    if (header[1] < 12)
        loadCurveSources(loaded);
    if (header[1] < 14)
        holdSeqCurveEnds(loaded);
    if (header[1] < 17)
        transposeFromRoot(loaded);
    if (header[1] < 9) { // the chain's effects in its order, as the graph's line
        if (!outWired) {
            std::vector<int> chain;
            for (int slot : fxOrder([&loaded](int index) { return loaded[index]; }))
                if (fxType(loaded[paramIndex(slot, FxParam::Type)]) != FxType::None)
                    chain.push_back(slot);
            for (const auto &[index, value] : fxChainWires(chain))
                loaded[index] = value;
        }
        for (int position = 0; position < kFxSlots; ++position)
            loaded[fxPositionIndex(position)] = table[fxPositionIndex(position)].def;
    }
    return true;
}

namespace {

// The bytes of a patch's params, from its header: none for a stream that isn't FMVibe's state or that readParams()
// refuses for its version.
std::optional<uint64_t> paramBytes(const clap_istream_t *stream) {
    uint32_t header[3];
    if (!readAll(stream, header, sizeof(header)) || header[0] != kStateMagic || header[1] < 1 ||
        header[1] > kStateVersion)
        return std::nullopt;
    return static_cast<uint64_t>(header[2]) * (sizeof(uint32_t) + sizeof(double));
}

int64_t fileTell(FILE *file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ftello(file);
#endif
}

bool fileSeek(FILE *file, int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(file, offset, origin) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), origin) == 0;
#endif
}

} // namespace

bool readPresetInfo(FILE *file, gui::PresetInfo &info) {
    const clap_istream_t stream = fileIn(file);
    const std::optional<uint64_t> skip = paramBytes(&stream);
    if (!skip)
        return false;
    // The params, sought past rather than read: a preset the plugin saves holds every one, some 300 KB, where its
    // layout is a few. One that ends among them is refused, as the stream's reading refuses it.
    const int64_t at = fileTell(file);
    if (at < 0 || !fileSeek(file, 0, SEEK_END))
        return false;
    const int64_t size = fileTell(file);
    if (size < at || static_cast<uint64_t>(size - at) < *skip || !fileSeek(file, at + static_cast<int64_t>(*skip), SEEK_SET))
        return false;
    info = readLayout(&stream).presetInfo;
    return true;
}

bool readPresetInfo(const clap_istream_t *stream, gui::PresetInfo &info) {
    const std::optional<uint64_t> skip = paramBytes(stream);
    if (!skip)
        return false;
    // The params, passed over: only the layout after them holds the info.
    unsigned char skipped[4096];
    for (uint64_t left = *skip; left > 0;) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(left, sizeof(skipped)));
        if (!readAll(stream, skipped, n))
            return false;
        left -= n;
    }
    info = readLayout(stream).presetInfo;
    return true;
}

gui::UiLayout readLayout(const clap_istream_t *stream) {
    std::vector<float> stored;
    readChunk(stream, kLayoutMagic, kMaxLayoutFloats, stored);
    gui::UiLayout layout;
    LayoutReader reader{stored};
    layoutFields(reader, layout);
    return layout;
}

bool writeSettings(const clap_ostream_t *stream, const Settings &settings) {
    // Settings chunk fields, in order; later versions only append.
    const float stored[] = {static_cast<float>(settings.oversampling), settings.mpe ? 1.0f : 0.0f,
                            static_cast<float>(settings.mpeBendRange), settings.tempoSync ? 1.0f : 0.0f,
                            static_cast<float>(gui::clampTempo(settings.ownTempo))};
    const uint32_t settingsChunk[] = {kSettingsMagic, static_cast<uint32_t>(std::size(stored))};
    return writeAll(stream, settingsChunk, sizeof(settingsChunk)) && writeAll(stream, stored, sizeof(stored));
}

Settings readSettings(const clap_istream_t *stream) {
    std::vector<float> values;
    readChunk(stream, kSettingsMagic, kMaxSettingsFloats, values);
    auto field = [&values](size_t i, float fallback) {
        return i < values.size() && std::isfinite(values[i]) ? values[i] : fallback;
    };
    Settings settings;
    settings.oversampling = validOversampling(std::lround(field(0, kDefaultOversampling)));
    settings.mpe = field(1, 0.0f) >= 0.5f;
    settings.mpeBendRange =
        std::clamp(static_cast<int>(std::lround(field(2, kDefaultMpeBendRange))), 1, kMaxMpeBendRange);
    static_assert(Settings{}.ownTempo == gui::kDefaultTempo);
    settings.tempoSync = field(3, 1.0f) >= 0.5f; // a project from before it follows the host, as it did
    settings.ownTempo = gui::clampTempo(field(4, static_cast<float>(gui::kDefaultTempo)));
    return settings;
}

bool writePresetChunk(const clap_ostream_t *stream, bool edited, const std::string &file) {
    // A path longer than a reader takes is left out rather than cut, which would name another file.
    const std::string preset = (edited ? '\1' : '\0') + (file.size() < kMaxPresetChunk ? file : std::string());
    const uint32_t presetChunk[] = {kPresetMagic, static_cast<uint32_t>(preset.size())};
    return writeAll(stream, presetChunk, sizeof(presetChunk)) && writeAll(stream, preset.data(), preset.size());
}

bool readPresetChunk(const clap_istream_t *stream, std::string &file, bool &edited) {
    std::string preset;
    uint32_t presetChunk[2];
    if (readAll(stream, presetChunk, sizeof(presetChunk)) && presetChunk[0] == kPresetMagic &&
        presetChunk[1] >= 1 && presetChunk[1] <= kMaxPresetChunk) {
        preset.resize(presetChunk[1]);
        if (!readAll(stream, preset.data(), preset.size()))
            preset.clear();
    }
    if (preset.empty())
        return false;
    file = preset.substr(1);
    edited = preset[0] != '\0';
    return true;
}

} // namespace fmvibe
