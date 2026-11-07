#include "state.h"

#include "gui/controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

namespace fmvibe {

namespace {

// State blob: magic, version, param count, (id, value) pairs, then the editor
// layout chunk, then (in the host's state, not in preset files) the settings chunk.
constexpr uint32_t kStateMagic = 0x38454C43;    // "CLE8"
constexpr uint32_t kLayoutMagic = 0x5459414C;   // "LAYT"
constexpr uint32_t kSettingsMagic = 0x47544553; // "SETG"
constexpr uint32_t kPresetMagic = 0x54535250;   // "PRST"
constexpr size_t kMaxMacroName = 24; // bytes of a macro's name that are saved
// The most of a layout chunk the loader reads, any more is skipped: plenty of room past the most this version
// writes (see layoutFields()), which is its fixed fields, every route kept (a node's output and its route into
// each node, at most), every macro's name at full length, every route's wire bent and every empty modulation.
constexpr uint32_t kMaxLayoutFloats = 4096;
constexpr int kMaxRoutes = gui::kNodes * (gui::kNodes + 1);
static_assert(2 * kOps + 2 * (kFilters + kShapers) + 4 + kMaxSources + 1 + kMaxRoutes + 2 +
                  kMacros * (1 + kMaxMacroName) + 2 + 3 + 1 + 2 * kMaxRoutes + 1 + 2 * gui::kMaxEmptyMods + 2 + 1 <=
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
constexpr uint32_t kStateVersion = 8;

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
    // Param indices, as their count and then their ids.
    void paramIds(const std::vector<int> &indices) {
        out.push_back(static_cast<float>(indices.size()));
        for (int index : indices)
            out.push_back(static_cast<float>(idOfIndex(index)));
    }
    // Text, as its length (up to `most` bytes) and then its bytes.
    void text(const std::string &text, size_t most) {
        out.push_back(static_cast<float>(std::min<size_t>(text.size(), most)));
        for (size_t i = 0; i < text.size() && i < most; ++i)
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
    // Empty modulations, as their count and then each one's source + 1 and its target's id + 1 (0 for none).
    void emptyMods(const std::vector<std::pair<int, int>> &mods) {
        out.push_back(static_cast<float>(mods.size()));
        for (const auto &[source, target] : mods) {
            out.push_back(static_cast<float>(source + 1));
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
            const int source = static_cast<int>(std::lround(next(0.0f, 0.0f, static_cast<float>(kAllSources)))) - 1;
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
    io.number(ui.view, 0.0f, 3.0f, rounded<gui::RoutingView>);
    io.number(ui.sourcesCollapsed, 0.0f, 1.0f, [](float value) { return value >= 0.5f; });
    // The source order lists every place once; anything else falls back to the default.
    io.numbers(ui.sourceOrder, 0.0f, kMaxSources - 1.0f, rounded<int>, everyPlaceOnce);
    // The routes kept at 0 %, by param id.
    io.paramIds(ui.keptRoutes);
    // A grid the editor doesn't offer falls back to the default.
    io.number(ui.shapeGridX, 0.0f, 64.0f, rounded<int>, oneOf(gui::kShapeGridsX));
    io.number(ui.shapeGridY, 0.0f, 64.0f, rounded<int>, oneOf(gui::kShapeGridsY));
    // The macros' names, each as its length and then its bytes. Saved before the macros came, they keep their
    // numbers.
    for (auto &name : ui.macroNames)
        io.text(name, kMaxMacroName);
    io.number(ui.fxFolded, 0.0f, 65535.0f, rounded<uint32_t>); // 16 bits each: exact as floats
    io.number(ui.sourcesFolded, 0.0f, 65535.0f, rounded<uint32_t>);
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
    // The effects graph's node places, and the node its inspector shows.
    io.numbers(ui.fxNodeX, 0.0f, 1.0f);
    io.numbers(ui.fxNodeY, 0.0f, 1.0f);
    io.number(ui.fxSelected, -1.0f, kFxNodes - 1.0f, rounded<int>);
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
    for (int i = 0; i < kParamCount; ++i) {
        if (!written(i))
            continue;
        const uint32_t id = idOfIndex(i);
        if (!writeAll(stream, &id, sizeof(id)) || !writeAll(stream, &values[i], sizeof(values[i])))
            return false;
    }

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
    const int volume = paramIndex(GlobalParam::Volume);
    for (uint32_t n = 0; n < header[2]; ++n) {
        uint32_t id;
        double value;
        if (!readAll(stream, &id, sizeof(id)) || !readAll(stream, &value, sizeof(value)))
            return false;
        if (!std::isfinite(value))
            continue;
        const int i = indexOfId(id);
        if (i == volume && header[1] < 2)
            value -= 12.0; // as loud as it was
        if (i >= 0)
            loaded[i] = std::clamp(value, table[i].min, table[i].max);
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
    if (header[1] < 8) { // the chain's order onto its places
        const int retired = paramIndex(GlobalParam::RetiredFxOrder);
        for (const auto &[index, value] : fxOrderChanges(fxOrderFromRetired(loaded[retired])))
            loaded[index] = value;
        loaded[retired] = table[retired].def;
    }
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
                            static_cast<float>(settings.mpeBendRange)};
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
