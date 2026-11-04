#pragma once

#include "params.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
}

// An example preset the plugin writes into a new preset folder: its name and its preset file's bytes, a sparse patch
// (see writePatch()). The files are presets/Factory/<name>.fmv, which the build compiles in (cmake/embed_presets.cmake)
// in the order presets/factory.txt names them.
struct FactoryPreset {
    std::string name;
    std::span<const unsigned char> data;
};

// Every example, in the order they came.
const std::vector<FactoryPreset> &factoryPresets();
// An example's params and editor layout, as loading its file gives them: false, leaving them as they were, for bytes
// that aren't a preset.
bool loadFactoryPreset(const FactoryPreset &preset, ParamValues &values, gui::UiLayout &layout);

// The examples the preset library started with; later ones go after them, and a preset folder made before
// they came gets them once (see PresetLibrary::rescan()).
constexpr size_t kFirstExamples = 6;

} // namespace fmvibe
