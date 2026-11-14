#include "opdata.h"

#include "fft.h"
#include "tables.h"

#include <cstring>
#include <numbers>

namespace fmvibe {

FrameTable buildFrameTable(const float *frames, int count) {
    FrameTable t;
    t.frames = std::clamp(count, 1, FrameTable::kMaxFrames);
    size_t total = 0;
    for (int level = 0; level < FrameTable::kLevels; ++level) {
        t.offsets[level] = total;
        total += static_cast<size_t>(t.frames) * (FrameTable::levelSize(level) + 1);
    }
    t.data.assign(total, 0.0f);
    if (count <= 0)
        return t;
    for (int f = 0; f < t.frames; ++f) {
        const std::vector<std::complex<double>> harmonics =
            harmonicsOf(frames + static_cast<size_t>(f) * FrameTable::kFrameSize, FrameTable::kFrameSize,
                        FrameTable::kHarmonics);
        for (int level = 0; level < FrameTable::kLevels; ++level) {
            const int size = FrameTable::levelSize(level);
            float *out = &t.data[t.offsets[level] + static_cast<size_t>(f) * (size + 1)];
            cycleOf(harmonics, FrameTable::levelHarmonics(level), static_cast<size_t>(size), out);
            out[size] = out[0];
        }
    }
    // One gain for the table, from its loudest frame at the first level, so the frames keep their levels.
    float peak = 0.0f;
    const size_t first = static_cast<size_t>(t.frames) * (FrameTable::kFrameSize + 1);
    for (size_t i = 0; i < first; ++i)
        peak = std::max(peak, std::abs(t.data[i]));
    if (peak > 0.0f)
        for (float &x : t.data)
            x /= peak;
    return t;
}

namespace {

// A low-pass for halving a sample's rate: windowed sinc (Kaiser), passing up to about 0.22 of the rate it filters and
// stopping from 0.28, so what is left above the new Nyquist folds only into the top of the new band, past what a
// voice reading that level plays below its own Nyquist.
const std::vector<float> &halvingFilter() {
    static const std::vector<float> taps = [] {
        constexpr int kTaps = 63;
        constexpr double kCutoff = 0.25, kBeta = 8.0;
        auto bessel0 = [](double x) {
            double sum = 1.0, term = 1.0;
            for (int k = 1; k < 32; ++k) {
                term *= (x / (2.0 * k)) * (x / (2.0 * k));
                sum += term;
            }
            return sum;
        };
        std::vector<float> h(kTaps);
        double total = 0.0;
        for (int n = 0; n < kTaps; ++n) {
            const double m = n - (kTaps - 1) / 2.0;
            const double sinc = m == 0.0 ? 2.0 * kCutoff : std::sin(2.0 * std::numbers::pi * kCutoff * m) / (std::numbers::pi * m);
            const double r = 2.0 * n / (kTaps - 1) - 1.0;
            const double w = bessel0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel0(kBeta);
            h[n] = static_cast<float>(sinc * w);
            total += sinc * w;
        }
        for (float &x : h)
            x = static_cast<float>(x / total);
        return h;
    }();
    return taps;
}

} // namespace

SampleMips buildSampleMips(const float *samples, int64_t length) {
    SampleMips m;
    m.length = std::max<int64_t>(length, 0);
    std::vector<float> level(samples, samples + m.length);
    const std::vector<float> &h = halvingFilter();
    const int half = static_cast<int>(h.size()) / 2;
    for (int k = 0; k < SampleMips::kMaxLevels; ++k) {
        std::vector<float> &out = m.data[k];
        out.assign(level.size() + 2 * SampleMips::kPad, 0.0f);
        std::copy(level.begin(), level.end(), out.begin() + SampleMips::kPad);
        m.levels = k + 1;
        if (level.size() < 64 || k + 1 == SampleMips::kMaxLevels)
            break;
        std::vector<float> next((level.size() + 1) / 2);
        const auto n = static_cast<int64_t>(level.size());
        for (int64_t i = 0; i < static_cast<int64_t>(next.size()); ++i) {
            double sum = 0.0;
            for (int t = 0; t < static_cast<int>(h.size()); ++t) {
                const int64_t j = 2 * i + t - half;
                if (j >= 0 && j < n)
                    sum += h[t] * level[j];
            }
            next[i] = static_cast<float>(sum);
        }
        level = std::move(next);
    }
    return m;
}

size_t OpData::bytes() const {
    size_t total = pcm.size() * sizeof(float) + table.data.size() * sizeof(float);
    for (const std::vector<float> &level : sample.data)
        total += level.size() * sizeof(float);
    return total + (peakMin.size() + peakMax.size()) * sizeof(float);
}

void buildOpData(OpData &data) {
    if (data.kind == DataKind::Table) {
        data.frames = std::clamp(data.frames, 1, FrameTable::kMaxFrames);
        data.pcm.resize(static_cast<size_t>(data.frames) * FrameTable::kFrameSize, 0.0f);
        data.table = buildFrameTable(data.pcm.data(), data.frames);
        return;
    }
    data.frames = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(data.pcm.size()), kMaxSampleLength));
    data.pcm.resize(static_cast<size_t>(data.frames));
    data.sample = buildSampleMips(data.pcm.data(), data.frames);
    buildPeaks(data);
}

void buildPeaks(OpData &data) {
    data.frames = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(data.pcm.size()), kMaxSampleLength));
    data.peakMin.assign(OpData::kPeakColumns, 0.0f);
    data.peakMax.assign(OpData::kPeakColumns, 0.0f);
    for (int c = 0; c < OpData::kPeakColumns && data.frames > 0; ++c) {
        const int64_t from = static_cast<int64_t>(c) * data.frames / OpData::kPeakColumns;
        const int64_t to = std::max(from + 1, static_cast<int64_t>(c + 1) * data.frames / OpData::kPeakColumns);
        float lo = data.pcm[from], hi = data.pcm[from];
        for (int64_t i = from; i < to && i < data.frames; ++i) {
            lo = std::min(lo, data.pcm[i]);
            hi = std::max(hi, data.pcm[i]);
        }
        data.peakMin[c] = lo;
        data.peakMax[c] = hi;
    }
}

uint32_t contentKey(const OpData &data) {
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    auto mix = [&hash](const void *bytes, size_t size) {
        const auto *b = static_cast<const unsigned char *>(bytes);
        for (size_t i = 0; i < size; ++i) {
            hash ^= b[i];
            hash *= 1099511628211ull;
        }
    };
    const uint32_t fields[] = {static_cast<uint32_t>(data.kind), data.rate, static_cast<uint32_t>(data.frames)};
    mix(fields, sizeof(fields));
    mix(data.pcm.data(), data.pcm.size() * sizeof(float));
    const auto key = static_cast<uint32_t>((hash ^ (hash >> 31)) & 0x7FFFFFFFu);
    return key < kUserDataBase ? key + kUserDataBase : key;
}

std::shared_ptr<const OpData> DataStore::get(uint32_t key) {
    if (const auto it = data_.find(key); it != data_.end())
        return it->second;
    if (key >= kFactoryTables)
        return nullptr;
    std::shared_ptr<const OpData> made = makeFactoryTable(static_cast<int>(key));
    if (made)
        data_[key] = made;
    return made;
}

const OpData *DataStore::find(uint32_t key) const {
    const auto it = data_.find(key);
    return it == data_.end() ? nullptr : it->second.get();
}

uint32_t DataStore::add(std::shared_ptr<OpData> data) {
    if (!data)
        return 0;
    uint32_t key = contentKey(*data);
    // The same content again keeps its key; another that hashes to a key taken takes the next one free.
    for (;; key = key + 1 >= 0x80000000u ? kUserDataBase : key + 1) {
        const auto it = data_.find(key);
        if (it == data_.end())
            break;
        const OpData &have = *it->second;
        if (have.kind == data->kind && have.rate == data->rate && have.frames == data->frames && have.pcm == data->pcm)
            return key;
    }
    data->key = key;
    data_[key] = std::move(data);
    return key;
}

void DataStore::put(std::shared_ptr<OpData> data) {
    if (!data || data->key < kUserDataBase)
        return;
    const uint32_t key = data->key;
    // Registered under this key already, it is let go of first, so the audio thread never reads data freed under it.
    for (const Slot &slot : slots_)
        if (slot.key == key && !slot.cooling && slot.data.get() != data.get()) {
            unregister(key, registry_ ? registry_->blocks() : 0);
            break;
        }
    data_[key] = std::move(data);
}

std::vector<const OpData *> DataStore::named(const std::vector<uint32_t> &keys) const {
    std::vector<const OpData *> out;
    std::vector<uint32_t> seen;
    for (const uint32_t key : keys) {
        if (key < kUserDataBase || std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);
        if (const OpData *d = find(key))
            out.push_back(d);
    }
    return out;
}

void DataStore::registerKey(uint32_t key, const std::shared_ptr<const OpData> &data) {
    if (sink_ && std::find(sunk_.begin(), sunk_.end(), key) == sunk_.end()) {
        sunk_.push_back(key);
        sink_(key, data);
    }
    if (!registry_)
        return;
    for (Slot &slot : slots_)
        if (slot.key == key && !slot.cooling)
            return;
    for (int i = 0; i < DataRegistry::kEntries; ++i) {
        Slot &slot = slots_[i];
        if (slot.key == DataRegistry::kNone && !slot.cooling) {
            slot.key = key;
            slot.data = data;
            registry_->set(i, key, data.get());
            return;
        }
    }
}

bool DataStore::ensure(uint32_t key) {
    const std::shared_ptr<const OpData> data = get(key);
    if (!data)
        return false;
    registerKey(key, data);
    if (!registry_)
        return true;
    for (const Slot &slot : slots_)
        if (slot.key == key && !slot.cooling)
            return true;
    return false;
}

void DataStore::unregister(uint32_t key, uint64_t blocks) {
    if (sink_) {
        if (const auto it = std::find(sunk_.begin(), sunk_.end(), key); it != sunk_.end()) {
            sunk_.erase(it);
            sink_(key, nullptr);
        }
    }
    if (!registry_)
        return;
    for (int i = 0; i < DataRegistry::kEntries; ++i) {
        Slot &slot = slots_[i];
        if (slot.key == key && !slot.cooling) {
            registry_->clear(i);
            slot.cooling = true;
            slot.blocks = blocks;
        }
    }
}

void DataStore::keep(const std::vector<uint32_t> &used, const std::vector<uint32_t> &held, uint64_t blocks, bool idle) {
    auto listed = [](const std::vector<uint32_t> &keys, uint32_t key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    // Let go of what the params no longer name, then register what they do.
    for (const Slot &slot : slots_)
        if (slot.key != DataRegistry::kNone && !slot.cooling && !listed(used, slot.key))
            unregister(slot.key, blocks);
    for (const uint32_t key : std::vector<uint32_t>(sunk_))
        if (!listed(used, key))
            unregister(key, blocks);
    for (uint32_t key : used)
        if (key != DataRegistry::kNone)
            ensure(key);
    // Free what the audio thread has rendered past since it was let go of.
    for (Slot &slot : slots_)
        if (slot.cooling && (idle || blocks > slot.blocks)) {
            slot = Slot{};
        }
    // Data nothing names any more goes, but for the factory tables, which are cheap to keep and to make again.
    for (auto it = data_.begin(); it != data_.end();) {
        const uint32_t key = it->first;
        bool inSlot = false;
        for (const Slot &slot : slots_)
            inSlot = inSlot || slot.key == key;
        if (key >= kFactoryTables && !listed(used, key) && !listed(held, key) && !inSlot)
            it = data_.erase(it);
        else
            ++it;
    }
}

std::vector<uint32_t> DataStore::keys() const {
    std::vector<uint32_t> out;
    for (const auto &[key, data] : data_)
        out.push_back(key);
    return out;
}

void DataStore::clear() {
    data_.clear();
}

} // namespace fmvibe
