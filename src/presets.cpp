#include "presets.h"

#include "factory.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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

bool sameNoCase(const std::wstring &a, const std::wstring &b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// The subfolder versions up to 0.11 wrote the factory presets into, which the library leaves out.
std::filesystem::path oldFactoryFolder(const std::wstring &folder) {
    return (std::filesystem::path(folder) / L"Factory").lexically_normal();
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

void PresetLibrary::makeFolder() {
    const std::wstring folder = folderPath();
    std::error_code error;
    if (!folder.empty())
        std::filesystem::create_directories(folder, error);
}

std::wstring PresetLibrary::factoryFile(const std::string &name) {
    return std::wstring(kFactoryPrefix) + widePath(name); // UTF-8, as presets/factory.txt has it
}

const FactoryPreset *PresetLibrary::factoryPreset(const std::wstring &file) {
    if (!file.starts_with(kFactoryPrefix))
        return nullptr;
    for (const FactoryPreset &preset : factoryPresets())
        if (factoryFile(preset.name) == file)
            return &preset;
    return nullptr;
}

std::wstring PresetLibrary::resolve(const std::wstring &file) {
    namespace fs = std::filesystem;
    const std::wstring folder = folderPath();
    if (file.empty() || folder.empty() || file.starts_with(kFactoryPrefix))
        return file;
    try {
        const fs::path path = fs::path(file).lexically_normal();
        if (!sameNoCase(path.parent_path().wstring(), oldFactoryFolder(folder).wstring()) ||
            !sameNoCase(path.extension().wstring(), L".fmv"))
            return file;
        const std::wstring stem = path.stem().wstring();
        for (const FactoryPreset &preset : factoryPresets())
            if (sameNoCase(widePath(preset.name), stem))
                return factoryFile(preset.name);
    } catch (const std::exception &) {
    }
    return file;
}

void PresetLibrary::rescan() {
    namespace fs = std::filesystem;
    entries_.clear();
    files_.clear();
    struct Found {
        gui::PresetEntry entry;
        std::wstring file;
    };
    std::vector<Found> found;
    for (const FactoryPreset &preset : factoryPresets())
        found.push_back({{preset.name, kFactoryCategory}, factoryFile(preset.name)});
    const std::wstring folderName = folderPath();
    if (!folderName.empty()) {
        const fs::path folder(folderName);
        const std::wstring oldFactory = oldFactoryFolder(folderName).wstring();
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code fileError;
            // The factory presets an earlier version wrote out, whatever has become of them: the list has them.
            if (it.depth() == 0 && it->is_directory(fileError) &&
                sameNoCase(it->path().lexically_normal().wstring(), oldFactory)) {
                it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(fileError) || _wcsicmp(it->path().extension().c_str(), L".fmv") != 0)
                continue;
            const std::wstring file = it->path().lexically_normal().wstring();
            const fs::path parent = it->path().parent_path().lexically_relative(folder);
            const std::string category = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
            std::string name = utf8(it->path().stem());
            if (name.empty()) // a name with no UTF-8 form can't be shown
                continue;
            found.push_back({{std::move(name), category}, file});
        }
    }
    // The top folder's presets first, then each subfolder's, each by name.
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.category != b.entry.category)
            return a.entry.category.empty() || (!b.entry.category.empty() && lessNoCase(a.entry.category, b.entry.category));
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (Found &f : found) {
        entries_.push_back(std::move(f.entry));
        files_.push_back(std::move(f.file));
    }
}

int PresetLibrary::indexOf(const std::wstring &file) const {
    if (file.empty())
        return -1;
    const std::wstring current =
        file.starts_with(kFactoryPrefix) ? file : std::filesystem::path(file).lexically_normal().wstring();
    for (size_t i = 0; i < files_.size(); ++i)
        if (_wcsicmp(files_[i].c_str(), current.c_str()) == 0)
            return static_cast<int>(i);
    return -1;
}

void PresetLibrary::openFolder() {
    makeFolder();
    rescan();
    const std::wstring folder = folderPath();
    if (!folder.empty())
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace fmvibe
