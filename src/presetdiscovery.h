#pragma once

// CLAP's preset discovery: what lets a host list FMVibe's presets in its own browser, without a plugin instance,
// and load one through the plugin's preset-load extension (src/plugin.cpp). One provider declares the .fmv file
// type, the factory presets built into the plugin (a plugin location, each preset by its load key) and the user's
// preset folder (a file location the host crawls), and reads a preset's metadata: its name, the plugin it is for,
// whether it is factory content and the sounds its name's prefix stands for. Independent of Windows and of the
// plugin, so it builds and is tested anywhere; src/entry.cpp hands the factory out with the user's folder.

#include <clap/factory/preset-discovery.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe {

// The provider's id, and the preset files' extension (without its dot).
inline constexpr char kPresetProviderId[] = "io.github.pcdlin.fmvibe.presets";
inline constexpr char kPresetExtension[] = "fmv";

// The path a UTF-8 text names, or an empty one for text that isn't UTF-8 (hosts hand locations over in UTF-8).
std::filesystem::path pathFromUtf8(std::string_view text);

// The sounds a preset's name says it is by its two-letter prefix ("BA FM Bass": bass), as features a host maps to
// its own tags: none for a name without a known prefix.
std::vector<const char *> presetFeatures(std::string_view name);

// The factory preset a load key names, or -1. A factory preset's load key is its name, which presets/factory.txt
// keeps unique and never changes once released (hosts store the key with their projects and favorites).
int factoryPresetByKey(const char *key);
const char *factoryPresetKey(int index);

// The factory preset whose unchanged copy `file` is (the preset library writes them into the user's folder's
// Factory subfolder), or -1: the host lists the built-in one, by its key, in its place.
int factoryCopyOf(const std::filesystem::path &file, const std::filesystem::path &userFolder);

// What reading a preset file's metadata found.
enum class PresetFileStatus {
    Ok,
    Unreadable, // the file can't be opened or its size read (the OS's error in `osError`)
    NotPreset,  // not FMVibe's state, of a newer version than this one reads, or cut short
};
struct PresetFileInfo {
    std::string name;     // the file's name without its extension, in UTF-8
    int factoryCopy = -1; // see factoryCopyOf()
};
// Reads only what the metadata needs: the file's size and its patch header (see patchHeaderValid()), and the whole
// file only when it may be a factory preset's copy. Cheap, and safe on any file.
PresetFileStatus readPresetFileInfo(const std::filesystem::path &file, const std::filesystem::path &userFolder,
                                    PresetFileInfo &info, int32_t &osError);

// The one provider's descriptor, and a new provider for `indexer` (destroyed through its destroy()), whose file
// location is `userFolder` (the preset library's folder; empty for none). Thread-safe.
const clap_preset_discovery_provider_descriptor_t *presetProviderDescriptor();
const clap_preset_discovery_provider_t *createPresetProvider(const clap_preset_discovery_indexer_t *indexer,
                                                             const std::filesystem::path &userFolder);

} // namespace fmvibe
