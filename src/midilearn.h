#pragma once

// MIDI learn: MIDI controllers (CCs) mapped to params, from a knob's right-click menu. Independent of Windows and of
// the plugin formats: the plugin core keeps one, the audio thread plays it and the state saves its mappings in the
// settings chunk (see Settings::midiMap), so it builds and is tested anywhere.
//
// A CC drives one param, on any MIDI channel; a param takes one CC (learning another moves it there). CCs 120 to 127
// are channel mode messages (All Sound Off, All Notes Off and so on), never learned.

#include "params.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace fmvibe {

// The CCs that can be learned: 0 to 119.
constexpr int kLearnableCcs = 120;

// A CC and the param (by index) it drives.
struct MidiMapping {
    int cc = -1;
    int index = -1;
    bool operator==(const MidiMapping &) const = default;
};

// Whether param `index` can be learned: one the editor turns with a knob. Not Voices (which may take a restart, and
// isn't automatable either), nor the retired params, which do nothing.
bool midiLearnable(int index);
// The value a CC at `cc` (0..1, its 0..127 over 127) sets param `index` to: across its range, a stepped one's on a
// step.
double midiLearnValue(int index, double cc);

// The mappings, and the param waiting for its CC. The main thread (the editor, a state loaded) and the audio thread
// (a CC coming in) both read and write them, lock-free, in atomics: nothing here allocates but mappings() and set().
class MidiLearn {
public:
    MidiLearn() { clear(); }

    // Any thread: the param `cc` drives, or -1; the CC that drives param `index`, or -1; the param waiting for a CC.
    int paramOf(int cc) const {
        return cc >= 0 && cc < kLearnableCcs ? params_[cc].load(std::memory_order_relaxed) : -1;
    }
    int ccOf(int index) const;
    int armed() const { return armed_.load(std::memory_order_relaxed); }
    // Any thread: a number that moves on whenever the mappings or the param armed change.
    uint64_t stamp() const { return stamp_.load(std::memory_order_acquire); }

    // Main thread: param `index` waits for the next CC (any other waiting stops); -1, or a param that can't be
    // learned, stops waiting.
    void arm(int index);
    // Main thread: param `index` is driven by no CC any more.
    void forget(int index);
    // Main thread: every mapping, by CC, for the state; and the state's, in place of these (those with a CC or a param
    // out of range, or a param that can't be learned, left out; a CC or a param named twice keeping its last).
    std::vector<MidiMapping> mappings() const;
    void set(const std::vector<MidiMapping> &mappings);
    void clear();

    // Audio thread: CC `cc` came in. Were a param waiting, it takes this CC (one the CC drove before, or another CC
    // that drove it, giving it up), and it stops waiting: true then.
    bool learn(int cc);

private:
    void moved() { stamp_.fetch_add(1, std::memory_order_release); }

    std::array<std::atomic<int>, kLearnableCcs> params_;
    std::atomic<int> armed_{-1};
    std::atomic<uint64_t> stamp_{1};
};

} // namespace fmvibe
