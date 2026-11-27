#pragma once

// DX7 voices: Yamaha DX7 sysex read (32-voice bank dumps, single voices) and each voice turned into a patch that plays
// it as the DX7 does, as near as FMVibe's engine takes it (see toPatch() and "DX7 import" in docs/features.md). The
// conversion follows the DX7's own level, rate and sensitivity curves, as the open-source emulations of it measured
// them, worked out here rather than tabled. Window-free: the plugin's Load and fmvibe-presettool both import through it.

#include "controller.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fmvibe::gui::dx7 {

// A voice unpacked, as a single-voice dump (VCED) has it: six operators of kOpBytes bytes, OP6 first, then the voice's
// own fields from kVoiceFields on, its name last.
constexpr int kOpBytes = 21;
constexpr int kVoiceFields = 6 * kOpBytes; // 126
constexpr int kVoiceBytes = 155;
constexpr int kNameBytes = 10;
constexpr int kPackedBytes = 128; // a voice packed into a bank (VMEM)
constexpr int kBankVoices = 32;

// Within an operator's bytes.
enum OpField {
    Rate1 = 0,   // the envelope's four rates (0-99) ...
    Level1 = 4,  // ... and four levels (0-99)
    BreakPoint = 8,
    LeftDepth,
    RightDepth,
    LeftCurve,
    RightCurve,
    RateScaling, // 0-7
    AmpModSens,  // 0-3
    VelocitySens, // 0-7
    OutputLevel, // 0-99
    OscMode,     // 0 ratio, 1 fixed
    Coarse,      // 0-31
    Fine,        // 0-99
    Detune,      // 0-14, 7 in tune
};
// Within the voice's own fields, from kVoiceFields.
enum VoiceField {
    PitchRate1 = 0,  // the pitch envelope's four rates ...
    PitchLevel1 = 4, // ... and levels, 50 in tune
    AlgorithmNumber = 8, // 0-31
    Feedback,        // 0-7
    OscKeySync,      // 1: every note starts the operators' waves at 0
    LfoSpeed,
    LfoDelay,
    LfoPitchDepth,
    LfoAmpDepth,
    LfoKeySync,
    LfoWave,         // triangle, saw down, saw up, square, sine, sample and hold
    PitchModSens,    // 0-7
    Transpose,       // 0-48, 24 at C3 (MIDI's C4)
    Name,
};

struct Voice {
    std::array<uint8_t, kVoiceBytes> data{};
    int op(int dxOp, OpField field) const { return data[(6 - dxOp) * kOpBytes + field]; } // dxOp 1-6, as the DX7's
    int voice(VoiceField field) const { return data[kVoiceFields + field]; }
    // Its ten characters, trailing spaces taken off, anything that isn't printable ASCII a space.
    std::string name() const;
};

// The DX7's INIT VOICE: one sine from OP1, a gate envelope.
Voice initVoice();
// Every byte held to its field's range (as a voice read from a file is).
Voice clamped(const Voice &voice);

// The voices in a sysex file, in order: each 32-voice bank dump and single-voice dump it holds (checksums aren't
// required: many banks in circulation have wrong ones), or a bank's 4096 bytes without their header. Empty for a file
// that holds none.
std::vector<Voice> parseSysex(const uint8_t *bytes, size_t size);
Voice unpackVoice(const uint8_t *packed);
std::array<uint8_t, kPackedBytes> packVoice(const Voice &voice);
// A bank dump of up to 32 voices (INIT VOICE after them), and a single-voice dump, as a DX7 sends them.
std::vector<uint8_t> bankSysex(const std::vector<Voice> &voices);
std::vector<uint8_t> voiceSysex(const Voice &voice);

// The patch that plays `voice`: from Init (`values` and `layout` set to it first), its algorithm laid out as the
// ALGORITHM panel lays it out, each operator on a Rate/Level envelope with the DX7's rates, levels and rate scaling and
// its keyboard level scaling, and the LFO, the pitch envelope and velocity as sources.
void toPatch(const Voice &voice, ParamValues &values, UiLayout &layout);

// A preset file's name (without .fmv) and bytes, sparse as the factory presets are stored.
struct Preset {
    std::string name;
    std::vector<unsigned char> bytes;
};
// A preset for each voice in a sysex file, named by its place in the file and its name ("07 E.PIANO 1"), its info
// saying where it came from (`bank`, the file's name); empty for a file without DX7 voices.
std::vector<Preset> presetsOf(const uint8_t *bytes, size_t size, const std::string &bank);

} // namespace fmvibe::gui::dx7
