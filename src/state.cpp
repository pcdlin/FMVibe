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
// each node, at most), every macro's name at full length and every route's wire bent.
constexpr uint32_t kMaxLayoutFloats = 4096;
constexpr int kMaxRoutes = gui::kNodes * (gui::kNodes + 1);
static_assert(2 * kOps + 2 * (kFilters + kShapers) + 4 + kMaxSources + 1 + kMaxRoutes + 2 +
                  kMacros * (1 + kMaxMacroName) + 2 + 3 + 1 + 2 * kMaxRoutes <=
              kMaxLayoutFloats);
constexpr uint32_t kMaxSettingsFloats = 64; // likewise for the settings chunk
// 2: Volume gives back the 12 dB the voices go into the effects with, so a version 1 Volume loads 12 dB lower.
constexpr uint32_t kStateVersion = 2;

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
    io.number(ui.view, 0.0f, 2.0f, rounded<gui::RoutingView>);
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
    io.number(ui.opTab, 0.0f, 2.0f, rounded<gui::InspectorTab>);
    io.number(ui.scopeCycles, 0.0f, 4.0f, rounded<int>, oneOf(gui::kScopeCycleChoices));
    // The wires dragged to bow other than they start, by route.
    io.paramValues(ui.wireBends, -gui::kMaxWireBend, gui::kMaxWireBend);
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

bool writePatch(const clap_ostream_t *stream, const ParamValues &values, const gui::UiLayout &ui) {
    const uint32_t header[] = {kStateMagic, kStateVersion, static_cast<uint32_t>(kParamCount)};
    if (!writeAll(stream, header, sizeof(header)))
        return false;
    for (int i = 0; i < kParamCount; ++i) {
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

bool writeSettings(const clap_ostream_t *stream, int oversampling) {
    // Settings chunk fields, in order; later versions only append.
    const float stored[] = {static_cast<float>(oversampling)};
    const uint32_t settingsChunk[] = {kSettingsMagic, static_cast<uint32_t>(std::size(stored))};
    return writeAll(stream, settingsChunk, sizeof(settingsChunk)) && writeAll(stream, stored, sizeof(stored));
}

int readSettings(const clap_istream_t *stream) {
    std::vector<float> values;
    readChunk(stream, kSettingsMagic, kMaxSettingsFloats, values);
    const float factor = values.empty() || !std::isfinite(values[0]) ? kDefaultOversampling : values[0];
    return validOversampling(std::lround(factor));
}

bool writePresetChunk(const clap_ostream_t *stream, bool edited, const std::string &file) {
    const std::string preset = (edited ? '\1' : '\0') + file;
    const uint32_t presetChunk[] = {kPresetMagic, static_cast<uint32_t>(preset.size())};
    return writeAll(stream, presetChunk, sizeof(presetChunk)) && writeAll(stream, preset.data(), preset.size());
}

bool readPresetChunk(const clap_istream_t *stream, std::string &file, bool &edited) {
    std::string preset;
    uint32_t presetChunk[2];
    if (readAll(stream, presetChunk, sizeof(presetChunk)) && presetChunk[0] == kPresetMagic &&
        presetChunk[1] >= 1 && presetChunk[1] <= 4096) {
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
