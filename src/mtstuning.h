#pragma once

// MTS-ESP's tuning for the synth, through ODDSound's client library (third_party/mts-esp). Each plugin instance holds
// one client, registered as the instance is made and deregistered as it goes, so a master knows what it tunes. With an
// MTS-ESP master connected the client plays the master's tuning; without one, its own table, which MTS SysEx retunes;
// with neither, 12-TET.

#include "gui/controller.h"
#include "tuning.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct MTSClient;

namespace fmvibe {

class MtsTuning : public Tuning {
public:
    MtsTuning();
    ~MtsTuning() override;
    MtsTuning(const MtsTuning &) = delete;
    MtsTuning &operator=(const MtsTuning &) = delete;

    // Audio thread only: the client caches its answers in itself, without locks.
    double retune(int key, int channel) override;
    bool unmapped(int key, int channel) override;
    // A master connected: its tuning plays over the patch's own.
    bool overrides() override;
    // Audio thread: a MIDI SysEx message from the host, F0 to F7 (a message without its F0 taken as if it had one).
    // MTS SysEx retunes the client's own table, and the name of the scale it gives is kept for the editor.
    void sysEx(const uint8_t *data, size_t size);

    // Main thread: where the tuning comes from now, and its scale's name. A master's name the master gives; MTS
    // SysEx's, the audio thread copied as it came.
    gui::TuningStatus status() const;

private:
    MTSClient *client_ = nullptr;
    // Set once MTS SysEx has retuned the client's table, and the scale's name it gave: the audio thread writes the
    // name with nameSeq_ odd meanwhile, and the main thread reads it again until it read it with nameSeq_ even and
    // unchanged (a sequence lock, so neither waits for the other).
    std::atomic<bool> sysExTuned_{false};
    std::atomic<uint32_t> nameSeq_{0};
    static constexpr size_t kNameSize = 64;
    std::array<std::atomic<char>, kNameSize> name_{};
};

} // namespace fmvibe
