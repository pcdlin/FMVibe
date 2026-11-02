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

namespace fmvibe {

namespace gui {
struct UiLayout;
}

constexpr int kDefaultOversampling = 2;
// The oversampling factor nearest below `factor`: 1, 2, 4 or 8.
int validOversampling(long factor);

// Streams over a file, for presets.
clap_istream_t fileIn(FILE *file);
clap_ostream_t fileOut(FILE *file);

// The state's first part, which a preset file holds too: magic, version, param count, (id, value)
// pairs, then the editor layout chunk.
bool writePatch(const clap_ostream_t *stream, const ParamValues &values, const gui::UiLayout &ui);
// The patch's header and params: false for a stream that isn't FMVibe's state, is of a newer version or ends among
// the params. Params missing from the state keep their defaults; unknown ids are skipped.
bool readParams(const clap_istream_t *stream, ParamValues &values);
// The editor layout chunk that follows the params. It is optional; whatever is missing keeps its default.
gui::UiLayout readLayout(const clap_istream_t *stream);

// The settings chunk, in the host's state only: this instance's settings, which presets leave alone.
bool writeSettings(const clap_ostream_t *stream, int oversampling);
// The oversampling factor it holds; a project saved before the settings came gets the default.
int readSettings(const clap_istream_t *stream);

// The preset chunk, after the settings: the preset the patch came from, whether it changed since, then its file's
// path in UTF-8 ("" for Init).
bool writePresetChunk(const clap_ostream_t *stream, bool edited, const std::string &file);
// False when the state doesn't say which preset it was.
bool readPresetChunk(const clap_istream_t *stream, std::string &file, bool &edited);

} // namespace fmvibe
