#include "presetdiscovery.h"

#include "factory.h"
#include "plugininfo.h"
#include "state.h"
#include "uistrings.h"

#include <clap/plugin-features.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <system_error>

namespace fmvibe {

namespace {

// The sounds the factory presets' prefixes stand for (presets/factory.txt), as preset features.
struct Prefix {
    char code[3];
    const char *feature;
};
constexpr Prefix kPrefixes[] = {
    {"BA", "bass"}, {"DR", CLAP_PLUGIN_FEATURE_DRUM}, {"FX", "fx"},       {"KY", "keys"},
    {"LD", "lead"}, {"PD", "pad"},                    {"PL", "pluck"},    {"SQ", "sequence"},
};

std::string utf8Of(const std::filesystem::path &path) {
    try {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception &) {
        return {};
    }
}

// Whether `a` and `b` are the same folder: lexically, or as the file system has them (a case or a link apart).
bool sameFolder(const std::filesystem::path &a, const std::filesystem::path &b) {
    if (a.empty() || b.empty())
        return false;
    if (a.lexically_normal() == b.lexically_normal())
        return true;
    std::error_code error;
    return std::filesystem::equivalent(a, b, error) && !error;
}

const clap_universal_plugin_id_t kPluginUniversalId = {"clap", kPluginId};

// What every preset says besides its name: the plugin it is for, and the sounds its name's prefix stands for.
void describe(const clap_preset_discovery_metadata_receiver_t *receiver, std::string_view name) {
    receiver->add_plugin_id(receiver, &kPluginUniversalId);
    receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    receiver->add_feature(receiver, CLAP_PLUGIN_FEATURE_SYNTHESIZER);
    for (const char *feature : presetFeatures(name))
        receiver->add_feature(receiver, feature);
}

struct Provider {
    clap_preset_discovery_provider_t clap;
    const clap_preset_discovery_indexer_t *indexer;
    std::filesystem::path userFolder;
    std::string userFolderText; // in UTF-8, for its location; empty for none

    static Provider &self(const clap_preset_discovery_provider_t *p) {
        return *static_cast<Provider *>(p->provider_data);
    }

    bool init() {
        const clap_preset_discovery_filetype_t filetype = {tr(Str::DiscoveryFileType), tr(Str::DiscoveryFileTypeHint),
                                                           kPresetExtension};
        if (!indexer->declare_filetype(indexer, &filetype))
            return false;
        const clap_preset_discovery_location_t factory = {CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT,
                                                          tr(Str::DiscoveryFactory),
                                                          CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
        indexer->declare_location(indexer, &factory);
        // The preset folder, whether or not the plugin has made it yet: the host keeps what it was told.
        if (!userFolderText.empty()) {
            const clap_preset_discovery_location_t user = {CLAP_PRESET_DISCOVERY_IS_USER_CONTENT,
                                                           tr(Str::DiscoveryUser), CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                                           userFolderText.c_str()};
            indexer->declare_location(indexer, &user);
        }
        return true;
    }

    bool metadata(uint32_t kind, const char *location, const clap_preset_discovery_metadata_receiver_t *receiver) {
        if (!receiver)
            return false;
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            const std::vector<FactoryPreset> &presets = factoryPresets();
            for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
                if (!receiver->begin_preset(receiver, presets[i].name.c_str(), factoryPresetKey(i)))
                    break;
                describe(receiver, presets[i].name);
                receiver->set_flags(receiver, CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT);
                receiver->add_creator(receiver, kPluginVendor);
            }
            return true;
        }
        const std::filesystem::path file =
            kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location ? pathFromUtf8(location) : std::filesystem::path();
        if (file.empty()) {
            receiver->on_error(receiver, 0, tr(Str::DiscoveryNoLocation));
            return false;
        }
        PresetFileInfo info;
        int32_t osError = 0;
        switch (readPresetFileInfo(file, userFolder, info, osError)) {
        case PresetFileStatus::Ok:
            break;
        case PresetFileStatus::Unreadable:
            receiver->on_error(receiver, osError, tr(Str::DiscoveryUnreadable));
            return false;
        case PresetFileStatus::NotPreset:
            receiver->on_error(receiver, 0, tr(Str::DiscoveryNotPreset));
            return false;
        }
        if (info.factoryCopy >= 0)
            return true; // the plugin location lists it: the file holds no preset of its own
        if (receiver->begin_preset(receiver, info.name.c_str(), nullptr))
            describe(receiver, info.name);
        return true;
    }
};

const clap_preset_discovery_provider_descriptor_t kProviderDescriptor = {
    CLAP_VERSION_INIT,
    kPresetProviderId,
    nullptr, // named by presetProviderDescriptor(), from the wordings
    kPluginVendor,
};

} // namespace

std::filesystem::path pathFromUtf8(std::string_view text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception &) {
        return {};
    }
}

std::vector<const char *> presetFeatures(std::string_view name) {
    if (name.size() < 4 || name[2] != ' ')
        return {};
    for (const Prefix &prefix : kPrefixes)
        if (name.compare(0, 2, prefix.code) == 0)
            return {prefix.feature};
    return {};
}

int factoryPresetByKey(const char *key) {
    if (!key)
        return -1;
    const std::vector<FactoryPreset> &presets = factoryPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].name == key)
            return static_cast<int>(i);
    return -1;
}

const char *factoryPresetKey(int index) {
    const std::vector<FactoryPreset> &presets = factoryPresets();
    return index >= 0 && index < static_cast<int>(presets.size()) ? presets[index].name.c_str() : nullptr;
}

int factoryCopyOf(const std::filesystem::path &file, const std::filesystem::path &userFolder) {
    if (userFolder.empty() || !sameFolder(file.parent_path(), userFolder / "Factory"))
        return -1;
    const int index = factoryPresetByKey(utf8Of(file.stem()).c_str());
    if (index < 0)
        return -1;
    std::error_code error;
    const std::span<const unsigned char> data = factoryPresets()[index].data;
    if (std::filesystem::file_size(file, error) != data.size() || error)
        return -1;
    std::ifstream in(file, std::ios::binary);
    std::vector<char> bytes(data.size());
    if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        return -1;
    return std::memcmp(bytes.data(), data.data(), data.size()) == 0 ? index : -1;
}

PresetFileStatus readPresetFileInfo(const std::filesystem::path &file, const std::filesystem::path &userFolder,
                                    PresetFileInfo &info, int32_t &osError) {
    osError = 0;
    info = {};
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error) || error) {
        osError = error.value();
        return PresetFileStatus::Unreadable;
    }
    const uintmax_t size = std::filesystem::file_size(file, error);
    if (error) {
        osError = error.value();
        return PresetFileStatus::Unreadable;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return PresetFileStatus::Unreadable;
    unsigned char header[12];
    if (!in.read(reinterpret_cast<char *>(header), sizeof(header)) || !patchHeaderValid(header, size))
        return PresetFileStatus::NotPreset;
    info.name = utf8Of(file.stem());
    if (info.name.empty()) // a name with no UTF-8 form can't be shown
        return PresetFileStatus::NotPreset;
    info.factoryCopy = factoryCopyOf(file, userFolder);
    return PresetFileStatus::Ok;
}

const clap_preset_discovery_provider_descriptor_t *presetProviderDescriptor() {
    static const clap_preset_discovery_provider_descriptor_t descriptor = [] {
        clap_preset_discovery_provider_descriptor_t named = kProviderDescriptor;
        named.name = tr(Str::DiscoveryProvider);
        return named;
    }();
    return &descriptor;
}

const clap_preset_discovery_provider_t *createPresetProvider(const clap_preset_discovery_indexer_t *indexer,
                                                             const std::filesystem::path &userFolder) {
    if (!indexer || !clap_version_is_compatible(indexer->clap_version))
        return nullptr;
    auto *provider = new Provider{};
    provider->indexer = indexer;
    provider->userFolder = userFolder;
    provider->userFolderText = utf8Of(userFolder);
    provider->clap = {
        presetProviderDescriptor(),
        provider,
        [](const clap_preset_discovery_provider_t *p) { return Provider::self(p).init(); },
        [](const clap_preset_discovery_provider_t *p) { delete &Provider::self(p); },
        [](const clap_preset_discovery_provider_t *p, uint32_t kind, const char *location,
           const clap_preset_discovery_metadata_receiver_t *receiver) {
            return Provider::self(p).metadata(kind, location, receiver);
        },
        [](const clap_preset_discovery_provider_t *, const char *) -> const void * { return nullptr; },
    };
    return &provider->clap;
}

} // namespace fmvibe
