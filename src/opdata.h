#pragma once

// The data a Wavetable or Sample operator plays (see OpWave): a table of frames, or a recorded sample. The patch keeps
// what it imported in its DATA chunk (see state.h), by key (see OpParam::Table); FMVibe's own tables are generated
// (see tables.h). Built on the main thread, into a form the audio thread reads without allocating, and handed to it
// through a fixed registry of pointers (DataRegistry); the main thread keeps each one alive (DataStore) until the audio
// thread can no longer be reading it.

#include "params.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fmvibe {

// A table of frames as the audio thread reads it: every frame at kLevels levels, each holding half the harmonics of
// the one before (512 at the first), each cycle kept with its first sample again after it, as WaveTables' are. Read
// between its samples in a straight line, a level needs many samples a cycle of its highest harmonic, or what the
// line adds folds back as aliasing: 32 from the fourth level on (2048 samples there, and half as many a level after),
// 16, 8 and 4 at the first three, which only low notes read, their highest harmonics far past hearing. Level by level,
// so a level's frames sit next to one another, and one gain for the whole table, from its loudest frame.
struct FrameTable {
    static constexpr int kFrameSize = 2048; // a frame's samples as stored and imported (Serum's frame size)
    static constexpr int kHarmonics = 512;  // at the first level
    static constexpr int kLevels = 10;      // 512 harmonics down to 1
    static constexpr int kMaxFrames = 512;
    static constexpr int levelSize(int level) { return std::min(kFrameSize, 32 * (kHarmonics >> level)); }
    static constexpr int levelHarmonics(int level) { return kHarmonics >> level; }
    // The level whose harmonics stay below Nyquist at a phase increment (cycles a sample).
    static int level(double increment) {
        return increment <= 0.0 ? 0
                                : std::clamp(static_cast<int>(std::ceil(std::log2(increment * 2.0 * kHarmonics))), 0,
                                             kLevels - 1);
    }

    int frames = 0;
    std::vector<float> data;
    std::array<size_t, kLevels> offsets{};
    // Frame `f` at `level`: levelSize(level) + 1 samples.
    const float *frame(int level, int f) const {
        return &data[offsets[level] + static_cast<size_t>(f) * (levelSize(level) + 1)];
    }
};

// Builds a table from `count` frames of kFrameSize samples each (frame after frame), DC taken out of each and the
// harmonics past kHarmonics left out, all scaled by one gain so the loudest frame peaks at 1. Main thread.
FrameTable buildFrameTable(const float *frames, int count);

// A sample as the audio thread reads it: level 0 is the sample itself, each level after it the one before low-passed
// and halved, so a voice pitching it up reads the level where it steps between half a sample and one a sample (see
// level()). Each level has kPad samples of silence before it and after it, for the reads around its ends.
struct SampleMips {
    static constexpr int kMaxLevels = 7; // the sample and 6 halvings
    static constexpr int kPad = 4;
    int levels = 0;
    int64_t length = 0; // level 0's samples
    std::array<std::vector<float>, kMaxLevels> data;
    // The level for a step of `step` level-0 samples a sample: the first where it steps `most` of its samples or fewer.
    // Stepping at most one, nothing it holds lies past Nyquist; oversampled, at most 0.78, what reading between its
    // samples adds lands past hearing (and past the decimator) too, as the voices' rate is twice the host's or more.
    int level(double step, double most = 1.0) const {
        if (!(step > most))
            return 0;
        return std::clamp(static_cast<int>(std::ceil(std::log2(step / most))), 0, levels - 1);
    }
    // Level `level`'s first sample (with kPad before it to read).
    const float *at(int level) const { return data[level].data() + kPad; }
    int64_t levelLength(int level) const { return (length + (int64_t{1} << level) - 1) >> level; }
};

// Builds the levels of a sample of `length` samples. Main thread.
SampleMips buildSampleMips(const float *samples, int64_t length);

// The 4-point Hermite read of `x` (kPad around it) at `pos` samples, between its samples.
inline float hermite(const float *x, double pos) {
    const double fl = std::floor(pos);
    const auto i = static_cast<int64_t>(fl);
    const float t = static_cast<float>(pos - fl);
    const float xm1 = x[i - 1], x0 = x[i], x1 = x[i + 1], x2 = x[i + 2];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

enum class DataKind : uint32_t { Table = 1, Sample = 2 };

// The most a sample holds (87 s at 48 kHz) and the most a table holds, in samples; and what a DATA chunk takes (see
// state.h): an entry, and all of them.
constexpr int64_t kMaxSampleLength = int64_t{1} << 22;
constexpr size_t kMaxDataEntryBytes = size_t{16} << 20;
constexpr size_t kMaxDataChunkBytes = size_t{64} << 20;

// What a table or sample holds, as the patch stores it (see state.h), and the form it is played from.
struct OpData {
    DataKind kind = DataKind::Table;
    uint32_t key = 0;
    std::string name;   // the file it came from, without its folder, or a factory table's name: only to show it
    // Where it was picked from in the table browser, "" for a file opened on its own: the source's name and the path
    // within it, "/" between folders. Only to show it, and to step to its neighbours there.
    std::string origin;
    std::string format; // the format tag it was read with ("clm", "srge", "wt", "flac", "vital", "wav"), "" if none
    // The stored form. A table: `frames` frames of FrameTable::kFrameSize samples, normalized. A sample: `frames`
    // samples (mono, peak 1) at `rate`, with the root and loop its file named (loopEnd 0: none).
    uint32_t rate = 0;
    int frames = 0;
    float root = 60.0f;
    uint32_t loopStart = 0, loopEnd = 0;
    bool int16 = false; // stored as 16 bits a sample: its file had no more
    std::vector<float> pcm;
    // The form played.
    FrameTable table;
    SampleMips sample;
    // For the editor's waveform: the sample's lowest and highest value in each of kPeakColumns columns of its length.
    static constexpr int kPeakColumns = 1024;
    std::vector<float> peakMin, peakMax;
    // About how much memory it holds, in bytes.
    size_t bytes() const;
};

// Builds the played form from the stored one (and the peaks). Main thread.
void buildOpData(OpData &data);
// Only a sample's peaks, for showing it (the table browser's preview), not for playing.
void buildPeaks(OpData &data);
// The key a stored form hashes to, from kUserDataBase up, below 2^31.
uint32_t contentKey(const OpData &data);
// A factory table (see FactoryTable), generated and built; null for a number that isn't one. Main thread.
std::shared_ptr<const OpData> makeFactoryTable(int table);

// The operators' data as the audio thread finds it: a fixed list of keys and pointers, which the main thread fills
// (see DataStore) and the audio thread looks through at each render(). A key the audio thread doesn't find leaves its
// operator silent and is noted for the main thread, which builds and registers it (see takeMissing()). The audio thread
// counts its renders, so the main thread can tell when one it took a pointer away from has ended (see blocks()).
class DataRegistry {
public:
    static constexpr int kEntries = 32;
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    DataRegistry() {
        for (auto &k : keys_)
            k.store(kNone);
        for (auto &d : data_)
            d.store(nullptr);
        for (auto &m : missing_)
            m.store(kNone);
        for (auto &k : ignored_)
            k.store(kNone);
    }
    // Main thread. The entry `slot` takes `data` for `key`.
    void set(int slot, uint32_t key, const OpData *data) {
        data_[slot].store(data);
        keys_[slot].store(key);
        for (auto &k : ignored_)
            if (k.load() == key)
                k.store(kNone);
    }
    // Main thread: `key` can't be had (a host set a param to a key the patch doesn't hold): the audio thread stops
    // asking for it, until it is registered.
    void ignore(uint32_t key) {
        for (auto &k : ignored_) {
            uint32_t none = kNone;
            if (k.load() == key || k.compare_exchange_strong(none, key))
                return;
        }
    }
    void clear(int slot) {
        keys_[slot].store(kNone);
        data_[slot].store(nullptr);
    }
    // Audio thread: the data registered for `key`, or null.
    const OpData *find(uint32_t key) const {
        for (int i = 0; i < kEntries; ++i)
            if (keys_[i].load(std::memory_order_acquire) == key) {
                const OpData *d = data_[i].load(std::memory_order_acquire);
                if (d && keys_[i].load(std::memory_order_acquire) == key)
                    return d;
            }
        return nullptr;
    }
    // Audio thread: `key` wasn't there.
    void noteMissing(uint32_t key) {
        for (const auto &k : ignored_)
            if (k.load(std::memory_order_relaxed) == key)
                return;
        for (auto &m : missing_) {
            uint32_t none = kNone;
            if (m.load(std::memory_order_relaxed) == key || m.compare_exchange_strong(none, key))
                break;
        }
        anyMissing_.store(true, std::memory_order_release);
    }
    // Main thread: the keys noted missing since the last call.
    std::vector<uint32_t> takeMissing() {
        std::vector<uint32_t> out;
        if (!anyMissing_.exchange(false))
            return out;
        for (auto &m : missing_)
            if (const uint32_t key = m.exchange(kNone); key != kNone)
                out.push_back(key);
        return out;
    }
    bool anyMissing() const { return anyMissing_.load(std::memory_order_acquire); }
    // Audio thread, at the end of each render(); and how many there have been, from any thread.
    void renderDone() { blocks_.fetch_add(1); }
    uint64_t blocks() const { return blocks_.load(); }

private:
    std::array<std::atomic<uint32_t>, kEntries> keys_;
    std::array<std::atomic<const OpData *>, kEntries> data_;
    std::array<std::atomic<uint32_t>, 2 * kOps> missing_;
    std::array<std::atomic<uint32_t>, 2 * kOps> ignored_;
    std::atomic<bool> anyMissing_{false};
    std::atomic<uint64_t> blocks_{0};
};

// The data the patch, its undo history and the audio thread use, by key, on the main thread. Factory tables are made
// as they are first asked for. The keys the params name are registered (see DataRegistry) before a param is set to
// them (see ensure()); the keys neither the params nor the history name are let go of, and freed once the audio thread
// can no longer be reading them (see keep()).
class DataStore {
public:
    // Where registered data goes: the plugin's synth's registry, or (the browser demo) a function that sends it to the
    // audio side, with null for data let go of.
    void attach(DataRegistry *registry) { registry_ = registry; }
    using Sink = std::function<void(uint32_t key, const std::shared_ptr<const OpData> &data)>;
    void setSink(Sink sink) { sink_ = std::move(sink); }

    // The data for `key`, made first if it is a factory table; null if the store doesn't have it.
    std::shared_ptr<const OpData> get(uint32_t key);
    // As get(), without making anything.
    const OpData *find(uint32_t key) const;
    // Takes in built data, keyed by its content (contentKey()); data the store has already keeps its key. The key.
    uint32_t add(std::shared_ptr<OpData> data);
    // Takes in built data under the key a state stored it with (from kUserDataBase up), in place of what the store
    // held under it.
    void put(std::shared_ptr<OpData> data);
    // The keys of the patch's own data that `values` name, each once, with their data: what a state stores.
    std::vector<const OpData *> named(const std::vector<uint32_t> &keys) const;
    // Registers `key` for the audio thread, made first if it is a factory table: before a param is set to it. False if
    // the store doesn't have it or the registry is full of keys the params name.
    bool ensure(uint32_t key);
    // The keys the params name (`used`, registered) and those the undo history names (`held`, kept): the rest are let
    // go of. `blocks` is the audio thread's render count (see DataRegistry::blocks()) and `idle` whether it renders
    // nothing now; data let go of is freed once it has moved on, or at once while idle.
    void keep(const std::vector<uint32_t> &used, const std::vector<uint32_t> &held, uint64_t blocks, bool idle);
    // Every key the store holds data for (factory tables made so far among them).
    std::vector<uint32_t> keys() const;
    // Forgets everything (the browser demo, whose patches aren't saved; and tests).
    void clear();

private:
    void registerKey(uint32_t key, const std::shared_ptr<const OpData> &data);
    void unregister(uint32_t key, uint64_t blocks);
    std::map<uint32_t, std::shared_ptr<const OpData>> data_;
    DataRegistry *registry_ = nullptr;
    Sink sink_;
    // What each registry entry holds, and the entries let go of that the audio thread may still read: freed (and the
    // entry free again) once it has rendered past `blocks`.
    struct Slot {
        uint32_t key = DataRegistry::kNone;
        std::shared_ptr<const OpData> data;
        bool cooling = false;
        uint64_t blocks = 0;
    };
    std::array<Slot, DataRegistry::kEntries> slots_{};
    std::vector<uint32_t> sunk_; // the keys sent to the sink
};

} // namespace fmvibe
