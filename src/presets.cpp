#include "presets.h"

#include "factory.h"
#include "params.h"
#include "state.h"
#include "usersettings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <system_error>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace fmvibe {

namespace {

// Whether `a` sorts before `b`, ignoring case.
bool lessNoCase(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// Writes the example presets into a new preset folder.
// Writes the examples from number `from` on into `folder`.
void writeExamples(const std::filesystem::path &folder, size_t from) {
    const ParamTable &table = paramTable();
    const std::vector<FactoryPreset> presets = factoryPresets();
    for (size_t n = from; n < presets.size(); ++n) {
        const FactoryPreset &preset = presets[n];
        ParamValues values;
        for (int i = 0; i < kParamCount; ++i)
            values[i] = table[i].def;
        for (const auto &[index, value] : preset.changes)
            values[index] = value;
        FILE *file = nullptr;
        const std::wstring name(preset.name.begin(), preset.name.end()); // the examples' names are ASCII
        const std::filesystem::path path = folder / (name + L".fmv");
        std::error_code error;
        if (std::filesystem::exists(path, error) || _wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            continue; // never over a preset of the same name
        const clap_ostream_t stream = fileOut(file);
        writePatch(&stream, values, exampleLayout(preset));
        std::fclose(file);
    }
}

} // namespace

std::string utf8(const std::filesystem::path &path, bool generic) {
    try {
        const std::u8string text = generic ? path.generic_u8string() : path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception &) {
        return {};
    }
}

std::wstring widePath(std::string_view text) {
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    } catch (const std::exception &) {
        return {};
    }
}

std::wstring PresetLibrary::folderPath() {
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        folder = std::wstring(documents) + L"\\FMVibe\\presets";
    CoTaskMemFree(documents);
    return folder;
}

void PresetLibrary::rescan() {
    namespace fs = std::filesystem;
    entries_.clear();
    files_.clear();
    const std::wstring name = folderPath();
    if (name.empty())
        return;
    const fs::path folder(name);
    std::error_code error;
    const bool fresh = !fs::exists(folder, error) && !error;
    readUserSettings();
    const size_t given = fresh                        ? 0
                         : userSettings.examples >= 0 ? static_cast<size_t>(userSettings.examples)
                                                      : kFirstExamples;
    const size_t examples = factoryPresets().size();
    if (!error && given < examples) {
        fs::create_directories(folder / L"Factory", error);
        if (!error) {
            writeExamples(folder / L"Factory", given);
            userSettings.examples = static_cast<int>(examples);
            writeUserSettings();
        }
    }
    struct Found {
        gui::PresetEntry entry;
        fs::path path;
    };
    std::vector<Found> found;
    error.clear();
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError) || _wcsicmp(it->path().extension().c_str(), L".fmv") != 0)
            continue;
        const fs::path parent = it->path().parent_path().lexically_relative(folder);
        const std::string category = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
        std::string name = utf8(it->path().stem());
        if (name.empty()) // a name with no UTF-8 form can't be shown
            continue;
        found.push_back({{std::move(name), category}, it->path().lexically_normal()});
    }
    // The top folder's presets first, then each subfolder's, each by name.
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.category != b.entry.category)
            return a.entry.category.empty() || (!b.entry.category.empty() && lessNoCase(a.entry.category, b.entry.category));
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (const Found &f : found) {
        entries_.push_back(f.entry);
        files_.push_back(f.path.wstring());
    }
}

int PresetLibrary::indexOf(const std::wstring &file) const {
    if (file.empty())
        return -1;
    const std::wstring current = std::filesystem::path(file).lexically_normal().wstring();
    for (size_t i = 0; i < files_.size(); ++i)
        if (_wcsicmp(files_[i].c_str(), current.c_str()) == 0)
            return static_cast<int>(i);
    return -1;
}

void PresetLibrary::openFolder() {
    rescan(); // makes it if it isn't there yet
    const std::wstring folder = folderPath();
    if (!folder.empty())
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace fmvibe
