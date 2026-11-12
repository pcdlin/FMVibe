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

// Writes the examples from number `from` on into `folder`, each its file's bytes as built in.
void writeExamples(const std::filesystem::path &folder, size_t from) {
    const std::vector<FactoryPreset> &presets = factoryPresets();
    for (size_t n = from; n < presets.size(); ++n) {
        const FactoryPreset &preset = presets[n];
        FILE *file = nullptr;
        const std::wstring name = widePath(preset.name); // UTF-8, as presets/factory.txt has it
        if (name.empty())
            continue;
        const std::filesystem::path path = folder / (name + L".fmv");
        std::error_code error;
        if (std::filesystem::exists(path, error) || _wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
            continue; // never over a preset of the same name
        std::fwrite(preset.data.data(), 1, preset.data.size(), file);
        std::fclose(file);
    }
}

// Writes the first `given` examples as built in over their files in `folder` where these differ, as a newer FMVibe
// brings them tuned or laid out anew. Those taken out of the folder stay out.
void updateExamples(const std::filesystem::path &folder, size_t given) {
    const std::vector<FactoryPreset> &presets = factoryPresets();
    for (size_t n = 0; n < std::min(given, presets.size()); ++n) {
        const FactoryPreset &preset = presets[n];
        const std::wstring name = widePath(preset.name);
        if (name.empty())
            continue;
        const std::filesystem::path path = folder / (name + L".fmv");
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error))
            continue;
        FILE *file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            continue;
        std::vector<unsigned char> now(preset.data.size() + 1);
        const size_t read = std::fread(now.data(), 1, now.size(), file);
        std::fclose(file);
        if (read == preset.data.size() && std::equal(preset.data.begin(), preset.data.end(), now.begin()))
            continue; // as built in already
        // Written beside it, then put in its place, so an instance loading it meanwhile reads the old or the new.
        std::filesystem::path part = path;
        part += L"." + std::to_wstring(GetCurrentProcessId()) + L".part";
        if (_wfopen_s(&file, part.c_str(), L"wb") != 0 || !file)
            continue;
        const bool written = std::fwrite(preset.data.data(), 1, preset.data.size(), file) == preset.data.size();
        if (std::fclose(file) == 0 && written)
            replaceFile(part.wstring(), path.wstring());
        else
            std::filesystem::remove(part, error);
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
    // The tests and the validators name a folder of their own (see FMVIBE_SETTINGS_DIR in usersettings.cpp), so
    // they leave the user's presets alone.
    wchar_t override_[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"FMVIBE_PRESETS_DIR", override_, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::wstring(override_, length);
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
    const std::wstring folderName = folderPath();
    if (folderName.empty())
        return;
    const fs::path folder(folderName);
    std::error_code error;
    const bool fresh = !fs::exists(folder, error) && !error;
    readUserSettings();
    const size_t given = fresh                        ? 0
                         : userSettings.examples >= 0 ? static_cast<size_t>(userSettings.examples)
                                                      : kFirstExamples;
    const size_t examples = factoryPresets().size();
    // A newer FMVibe than the one that last wrote the examples out writes those still there again, as it has them.
    const bool newer = versionBefore(userSettings.examplesVersion, FMVIBE_VERSION);
    if (!error && (given < examples || newer)) {
        fs::create_directories(folder / L"Factory", error);
        if (!error) {
            if (newer && !fresh)
                updateExamples(folder / L"Factory", given);
            writeExamples(folder / L"Factory", given);
            userSettings.examples = static_cast<int>(std::max(examples, given));
            userSettings.examplesVersion = FMVIBE_VERSION;
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
