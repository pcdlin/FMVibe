#pragma once

#include "params.h"

#include <string>
#include <utility>
#include <vector>

namespace fmvibe {

namespace gui {
struct UiLayout;
}

// An example preset the plugin writes into a new preset folder: its name, the params it sets on top of
// Init (see paramTable()), and where the routing graph puts operators A..F, then filters 1 and 2 and shapers 1 and
// 2 (0..1 in the graph area), when its stacks read better than in the default places; empty for those, and those
// left out keep their default places. Then the names of the macros it uses, from the first.
struct FactoryPreset {
    std::string name;
    std::vector<std::pair<int, double>> changes;
    std::vector<std::pair<float, float>> nodes;
    std::vector<std::string> macros;
};

std::vector<FactoryPreset> factoryPresets();
// The editor layout an example is saved with: the default one, with its nodes where it puts them.
gui::UiLayout exampleLayout(const FactoryPreset &preset);

// The examples the preset library started with; later ones go after them, and a preset folder made before
// they came gets them once (see Plugin::rescanPresets()).
constexpr size_t kFirstExamples = 6;

} // namespace fmvibe
