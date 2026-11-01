// DLL entry point: the host looks up the exported `clap_entry` symbol, asks it
// for the plugin factory and creates plugin instances through that.

#include "plugin.h"

#include <cstring>

namespace {

uint32_t CLAP_ABI factoryGetPluginCount(const clap_plugin_factory_t *) {
    return 1;
}

const clap_plugin_descriptor_t *CLAP_ABI factoryGetPluginDescriptor(const clap_plugin_factory_t *,
                                                                    uint32_t index) {
    return index == 0 ? &fmvibe::kDescriptor : nullptr;
}

const clap_plugin_t *CLAP_ABI factoryCreatePlugin(const clap_plugin_factory_t *,
                                                  const clap_host_t *host,
                                                  const char *pluginId) {
    if (!clap_version_is_compatible(host->clap_version) ||
        std::strcmp(pluginId, fmvibe::kDescriptor.id) != 0)
        return nullptr;
    return fmvibe::createPlugin(host);
}

const clap_plugin_factory_t kFactory = {
    factoryGetPluginCount,
    factoryGetPluginDescriptor,
    factoryCreatePlugin,
};

bool CLAP_ABI entryInit(const char *) {
    return true;
}

void CLAP_ABI entryDeinit() {}

const void *CLAP_ABI entryGetFactory(const char *factoryId) {
    return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
