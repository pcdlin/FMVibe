#pragma once

// The VST3 plugin, written straight against the VST3 interfaces (no wrapper): one object that is both the
// processor and the edit controller (a "single component" effect), on the same core as the CLAP plugin.

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <cstdint>

namespace fmvibe::vst3 {

// The class id hosts know FMVibe's VST3 by. Stored in projects: never change it.
inline constexpr Steinberg::TUID kClassId = INLINE_UID(0x9BCBBCB9, 0x166D49F6, 0xA9C2271D, 0x5009BDD5);

// A new plugin, its one reference the caller's.
Steinberg::FUnknown *createPlugin();

// The params MIDI controllers come in as: hosts map every controller, channel pressure and pitch bend on each of the
// 16 channels to one of these (IMidiMapping), and the plugin hands them on to the synth as MIDI. Their ids sit far
// past FMVibe's own; hosts don't show them.
constexpr Steinberg::Vst::ParamID kMidiParamBase = 0x40000000;
constexpr int kMidiChannels = 16;
constexpr int kMidiControllers = 130; // CC 0..127, then channel pressure (128) and pitch bend (129), as VST3 numbers them
constexpr int kMidiParamCount = kMidiChannels * kMidiControllers;

// The note expression FMVibe takes for a note's pressure, which VST3 has no id of its own for.
constexpr Steinberg::Vst::NoteExpressionTypeID kPressureTypeId = 100000; // Vst::kCustomStart

} // namespace fmvibe::vst3
