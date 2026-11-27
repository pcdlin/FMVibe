#include "mtstuning.h"

#include "../third_party/mts-esp/libMTSClient.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace fmvibe {

namespace {
// A scale's name as the editor shows it: without the spaces MTS SysEx pads it to 16 characters with.
std::string trimmed(std::string name) {
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0'))
        name.pop_back();
    return name;
}
} // namespace

MtsTuning::MtsTuning() : client_(MTS_RegisterClient()) {}

MtsTuning::~MtsTuning() {
    MTS_DeregisterClient(client_);
}

double MtsTuning::retune(int key, int channel) {
    return MTS_RetuningInSemitones(client_, static_cast<char>(key), static_cast<signed char>(channel));
}

bool MtsTuning::overrides() {
    return MTS_HasMaster(client_);
}

bool MtsTuning::unmapped(int key, int channel) {
    return MTS_ShouldFilterNote(client_, static_cast<char>(key), static_cast<signed char>(channel));
}

void MtsTuning::sysEx(const uint8_t *data, size_t size) {
    if (!data || size == 0)
        return;
    if (data[0] == 0xF0) {
        MTS_ParseMIDIDataU(client_, data, static_cast<int>(std::min<size_t>(size, 1 << 20)));
    } else { // without its F0: as long as a bulk dump and more, on the stack
        std::array<uint8_t, 1024> framed;
        framed[0] = 0xF0;
        const size_t n = std::min(size, framed.size() - 1);
        std::memcpy(framed.data() + 1, data, n);
        MTS_ParseMIDIDataU(client_, framed.data(), static_cast<int>(n + 1));
    }
    if (!MTS_HasReceivedMTSSysEx(client_))
        return;
    // The client's own scale name, read on this thread, which alone writes it (MTS_GetScaleName() gives a master's
    // while one is connected, which isn't the SysEx's).
    const char *name = MTS_HasMaster(client_) ? nullptr : MTS_GetScaleName(client_);
    if (name) {
        nameSeq_.fetch_add(1, std::memory_order_relaxed); // odd: being written
        std::atomic_thread_fence(std::memory_order_release);
        size_t i = 0;
        for (; i + 1 < kNameSize && name[i]; ++i)
            name_[i].store(name[i], std::memory_order_relaxed);
        name_[i].store('\0', std::memory_order_relaxed);
        nameSeq_.fetch_add(1, std::memory_order_release); // even again
    }
    sysExTuned_.store(true, std::memory_order_release);
}

gui::TuningStatus MtsTuning::status() const {
    gui::TuningStatus status;
    if (MTS_HasMaster(client_)) {
        status.source = gui::TuningSource::Master;
        // A master's name, from libMTS. (Should the master leave just now, the client's own is read, which the audio
        // thread writes only as MTS SysEx comes: held to its size here all the same.)
        if (const char *name = MTS_GetScaleName(client_))
            status.scale = trimmed(std::string(name, strnlen(name, kNameSize)));
        return status;
    }
    if (!sysExTuned_.load(std::memory_order_acquire))
        return status;
    status.source = gui::TuningSource::SysEx;
    for (;;) {
        const uint32_t before = nameSeq_.load(std::memory_order_acquire);
        if (before & 1u)
            continue; // being written: a moment
        std::string name;
        for (size_t i = 0; i < kNameSize; ++i) {
            const char c = name_[i].load(std::memory_order_relaxed);
            if (!c)
                break;
            name += c;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (nameSeq_.load(std::memory_order_relaxed) == before) {
            status.scale = trimmed(std::move(name));
            return status;
        }
    }
}

} // namespace fmvibe
