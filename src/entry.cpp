// DLL entry point: the host looks up the exported `clap_entry` symbol, asks it
// for the plugin factory and creates plugin instances through that, and for the
// preset discovery factory, whose provider lists FMVibe's presets in the host's
// browser without an instance (src/presetdiscovery.h).

#include "plugin.h"
#include "presetdiscovery.h"
#include "presets.h"

#include <cstring>
#include <filesystem>

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

// One provider, for the factory presets and the user's preset folder. Thread-safe, as CLAP asks.
uint32_t CLAP_ABI discoveryCount(const clap_preset_discovery_factory_t *) {
    return 1;
}

const clap_preset_discovery_provider_descriptor_t *CLAP_ABI
discoveryDescriptor(const clap_preset_discovery_factory_t *, uint32_t index) {
    return index == 0 ? fmvibe::presetProviderDescriptor() : nullptr;
}

const clap_preset_discovery_provider_t *CLAP_ABI discoveryCreate(const clap_preset_discovery_factory_t *,
                                                                 const clap_preset_discovery_indexer_t *indexer,
                                                                 const char *providerId) {
    if (!providerId || std::strcmp(providerId, fmvibe::kPresetProviderId) != 0)
        return nullptr;
    // The folder the preset library uses (Documents\FMVibe\presets), or none when Windows doesn't say.
    return fmvibe::createPresetProvider(indexer, std::filesystem::path(fmvibe::PresetLibrary::folderPath()));
}

const clap_preset_discovery_factory_t kDiscoveryFactory = {
    discoveryCount,
    discoveryDescriptor,
    discoveryCreate,
};

bool CLAP_ABI entryInit(const char *) {
    return true;
}

void CLAP_ABI entryDeinit() {}

const void *CLAP_ABI entryGetFactory(const char *factoryId) {
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &kFactory;
    if (std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID) == 0 ||
        std::strcmp(factoryId, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == 0)
        return &kDiscoveryFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
