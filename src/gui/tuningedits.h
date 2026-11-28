#pragma once

// The patch's own tuning as the editor edits it (see TuningParam): a tuning file's text loaded into it, and its scale or
// keyboard mapping taken out again, each one undo step that the host hears of as any edit. Window-free: the plugin, the
// browser demo and the tests share it.

#include "../tuningfile.h"
#include "controller.h"

#include <optional>
#include <string>
#include <string_view>

namespace fmvibe::gui {

// A tuning file's text loaded into the patch, `file` saying what it holds (by its extension): a scale (.scl, .ascl,
// .tun) as its scale, leaving its keyboard mapping, or a keyboard mapping (.kbm) over its scale. `name` (the file's
// name, without its extension) is what the overflow menu shows of it. Nothing on success, else why it didn't load,
// the patch left as it was.
std::optional<Str> loadTuningText(Controller &c, std::string_view text, TuningFile file, const std::string &name);
// The patch's scale (`mapping` false) or keyboard mapping taken out: back to 12-TET, or to no mapping.
void resetTuning(Controller &c, bool mapping);
// Whether the patch has a scale, and a keyboard mapping, of its own.
bool patchHasScale(const Controller &c);
bool patchHasMapping(const Controller &c);
// Where the tuning comes from, `live` being MTS-ESP's (a master, MTS SysEx or nothing) and `ui` the patch's layout,
// with the patch's own tuning taken in: playing over SysEx, and under a master.
TuningStatus withPatchTuning(const Controller &c, const UiLayout &ui, TuningStatus live);

} // namespace fmvibe::gui
