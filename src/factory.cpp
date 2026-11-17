#include "factory.h"

#include "gui/controller.h"
#include "state.h"

namespace fmvibe {

// factoryPresets() is generated from the preset files (cmake/embed_presets.cmake).

const FactoryPreset *factoryPresetNamed(std::string_view name) {
    for (const FactoryPreset &preset : factoryPresets())
        if (gui::sameName(preset.name, name))
            return &preset;
    if (gui::prefixCategory(name) >= 0) // a name from before they lost their two letters
        for (const FactoryPreset &preset : factoryPresets())
            if (gui::sameName(preset.name, name.substr(3)))
                return &preset;
    return nullptr;
}

bool loadFactoryPreset(const FactoryPreset &preset, ParamValues &values, gui::UiLayout &layout) {
    MemoryReader reader{preset.data.data(), preset.data.size()};
    const clap_istream_t stream = memoryIn(reader);
    ParamValues loaded;
    if (!readParams(&stream, loaded))
        return false;
    layout = readLayout(&stream);
    values = loaded;
    return true;
}

} // namespace fmvibe
