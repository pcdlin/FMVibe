#pragma once

#include "gui/controller.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe {

struct FactoryPreset;

// A path's text in UTF-8, or empty for one that has no UTF-8 form (a lone surrogate in a file name): the
// exception std::filesystem throws for it mustn't reach the host or the window procedure.
std::string utf8(const std::filesystem::path &path, bool generic = false);

// The path a UTF-8 text names, or an empty one for text that isn't UTF-8 (a damaged state), for the same reason.
std::wstring widePath(std::string_view text);

// The preset library: the factory presets, built into the plugin, and the .fmv files in the preset folder
// (Documents\FMVibe\presets) and its subfolders, as last scanned, each entry with its file. A factory preset's
// "file" is kFactoryPrefix and its name, which no file has. Main thread.
class PresetLibrary {
public:
    static constexpr std::wstring_view kFactoryPrefix = L"factory:";
    // The category the factory presets show under, as the subfolder the plugin once wrote them into.
    static constexpr const char *kFactoryCategory = "Factory";

    // The preset folder, or "" when Windows doesn't say where the documents are.
    static std::wstring folderPath();
    // Makes the preset folder if it isn't there yet (empty: the factory presets aren't written into it).
    static void makeFolder();
    // The "file" of the factory preset named `name`.
    static std::wstring factoryFile(const std::string &name);
    // The factory preset `file` names, or nullptr for a file on disk (or a name no factory preset has).
    static const FactoryPreset *factoryPreset(const std::wstring &file);
    // `file`, or the factory preset's "file" for a file named as one in the folder's Factory subfolder, where
    // versions up to 0.11 wrote them out: projects saved then name it.
    static std::wstring resolve(const std::wstring &file);

    // Lists the factory presets and the folder's presets, leaving out its Factory subfolder (see resolve()). The
    // folder isn't made for it.
    void rescan();
    // Shows the folder, made first if it isn't there yet.
    void openFolder();
    // The top folder's presets first, then each subfolder's (the factory presets as the Factory subfolder's), each
    // by name.
    const std::vector<gui::PresetEntry> &entries() const { return entries_; }
    const std::vector<std::wstring> &files() const { return files_; }
    // The entry whose file `file` is, or -1 for "" or a file from elsewhere.
    int indexOf(const std::wstring &file) const;

private:
    std::vector<gui::PresetEntry> entries_;
    std::vector<std::wstring> files_;
};

} // namespace fmvibe
