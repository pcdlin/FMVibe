#pragma once

#include "gui/controller.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe {

// A path's text in UTF-8, or empty for one that has no UTF-8 form (a lone surrogate in a file name): the
// exception std::filesystem throws for it mustn't reach the host or the window procedure.
std::string utf8(const std::filesystem::path &path, bool generic = false);

// The path a UTF-8 text names, or an empty one for text that isn't UTF-8 (a damaged state), for the same reason.
std::wstring widePath(std::string_view text);

// The preset library: the .fmv files in the preset folder (Documents\FMVibe\presets) and its subfolders, as last
// scanned, each entry with its file. Main thread.
class PresetLibrary {
public:
    // The preset folder, or "" when Windows doesn't say where the documents are. The environment variable
    // FMVIBE_PRESETS_DIR names another, for the tests and the validators.
    static std::wstring folderPath();
    // The first scan makes the folder, with the examples in a subfolder of their own; a folder made before
    // later examples came gets those once. The settings file counts the examples given, so deleted ones
    // stay gone.
    void rescan();
    // Shows the folder, which a scan makes first if it isn't there yet.
    void openFolder();
    // The top folder's presets first, then each subfolder's, each by name.
    const std::vector<gui::PresetEntry> &entries() const { return entries_; }
    const std::vector<std::wstring> &files() const { return files_; }
    // The entry whose file `file` is, or -1 for "" or a file from elsewhere.
    int indexOf(const std::wstring &file) const;

private:
    std::vector<gui::PresetEntry> entries_;
    std::vector<std::wstring> files_;
};

} // namespace fmvibe
