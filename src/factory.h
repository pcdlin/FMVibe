#pragma once

#include "params.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
}

// A factory preset, built into the plugin: its name and its preset file's bytes, a sparse patch
// (see writePatch()). The files are presets/Factory/<name>.fmv, which the build compiles in (cmake/embed_presets.cmake)
// in the order presets/factory.txt names them.
struct FactoryPreset {
    std::string name;
    std::span<const unsigned char> data;
};

// Every example, in the order they came.
const std::vector<FactoryPreset> &factoryPresets();
// The factory preset named `name`, whatever its case, or nullptr. A name from up to 0.11, its category's two letters
// before it ("LD Screamer"), names the preset without them, as a project saved then does.
const FactoryPreset *factoryPresetNamed(std::string_view name);
// An example's params and editor layout, as loading its file gives them: false, leaving them as they were, for bytes
// that aren't a preset.
bool loadFactoryPreset(const FactoryPreset &preset, ParamValues &values, gui::UiLayout &layout);

} // namespace fmvibe
