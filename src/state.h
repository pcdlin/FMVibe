#pragma once

// The state FMVibe saves, in the host's project and in preset files. Its layout is stored, so it never changes but
// at its ends (see "Plugin facts" in AGENTS.md). Independent of Windows and of the plugin: it builds and can be
// tested anywhere.
//
// The host's state: the patch (magic, version, param count, (id, value) pairs, then the editor layout chunk), then
// the settings chunk and the preset chunk. A preset file holds the patch alone.

#include "params.h"

#include <clap/stream.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
struct PresetInfo;
}

constexpr int kDefaultOversampling = 2;
// The oversampling factor nearest below `factor`: 1, 2, 4 or 8.
int validOversampling(long factor);

// MPE's bend range for its member channels, in semitones: 48 to begin with, as the MPE spec has it.
constexpr int kDefaultMpeBendRange = 48;
constexpr int kMaxMpeBendRange = 96;

// The settings of an instance, which presets leave alone: saved with the host's project, in the settings chunk.
struct Settings {
    int oversampling = kDefaultOversampling; // 1, 2, 4 or 8
    bool mpe = false;                        // MPE over MIDI (see Synth::setMpe())
    int mpeBendRange = kDefaultMpeBendRange; // 1 to kMaxMpeBendRange semitones
    // Whether the synth plays at the host's tempo (the title bar's H), and the tempo it plays at while it doesn't, in
    // BPM (held to the range and rounded as gui::clampTempo() does; 140 to begin with, gui::kDefaultTempo).
    bool tempoSync = true;
    double ownTempo = 140.0;
    // MTS-ESP (see Synth::setTuning()): whether the synth plays the tuning a master or MTS SysEx gives it, and whether
    // held notes follow it as it changes (else each keeps the one it started in). MTS-ESP off to begin with, and for a
    // project from before it; retuning continuous.
    bool mts = false;
    bool mtsContinuous = true;
};

// Streams over a file, for presets.
clap_istream_t fileIn(FILE *file);
clap_ostream_t fileOut(FILE *file);
// Streams over memory: reading bytes held elsewhere (the factory presets built into the plugin), and writing into a
// vector, which grows as written. Each stream keeps a pointer to its reader or vector.
struct MemoryReader {
    const unsigned char *data = nullptr;
    size_t size = 0;
    size_t at = 0;
};
clap_istream_t memoryIn(MemoryReader &reader);
clap_ostream_t memoryOut(std::vector<unsigned char> &bytes);

// The state's first part, which a preset file holds too: magic, version, param count, (id, value)
// pairs, then the editor layout chunk. A sparse patch holds only the params whose values differ from their defaults,
// which is what loading one gives the others: the factory presets are stored so.
bool writePatch(const clap_ostream_t *stream, const ParamValues &values, const gui::UiLayout &ui, bool sparse = false);
// The patch's header and params: false for a stream that isn't FMVibe's state, is of a newer version or ends among
// the params. Params missing from the state keep their defaults; unknown ids are skipped.
bool readParams(const clap_istream_t *stream, ParamValues &values);
// The editor layout chunk that follows the params. It is optional; whatever is missing keeps its default.
gui::UiLayout readLayout(const clap_istream_t *stream);

// What a preset file (or a state) says about itself (see gui::PresetInfo), read past its params without taking them:
// false for a stream that isn't FMVibe's state, is of a newer version or ends among the params.
bool readPresetInfo(const clap_istream_t *stream, gui::PresetInfo &info);
// The same from a preset file open for reading, seeking past its params rather than reading them, as the library's
// scan does for every file in the preset folder.
bool readPresetInfo(FILE *file, gui::PresetInfo &info);

// The settings chunk, in the host's state only: this instance's settings, which presets leave alone.
bool writeSettings(const clap_ostream_t *stream, const Settings &settings);
// The settings it holds; those a project saved before them doesn't have, or that aren't numbers, keep their defaults.
Settings readSettings(const clap_istream_t *stream);

// The preset chunk, after the settings: the preset the patch came from, whether it changed since, then its file's
// path in UTF-8 ("" for Init).
bool writePresetChunk(const clap_ostream_t *stream, bool edited, const std::string &file);
// False when the state doesn't say which preset it was.
bool readPresetChunk(const clap_istream_t *stream, std::string &file, bool &edited);

} // namespace fmvibe
