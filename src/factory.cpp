#include "factory.h"

#include "gui/controller.h"
#include "state.h"

namespace fmvibe {

// factoryPresets() is generated from the preset files (cmake/embed_presets.cmake).

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
