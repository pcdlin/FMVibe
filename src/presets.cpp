#include "presets.h"

#include "factory.h"
#include "state.h"

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
    if (file.starts_with(kFactoryPrefix)) { // a factory preset's name from up to 0.11 names it as it is now
        const FactoryPreset *preset =
            factoryPresetNamed(utf8(fs::path(file.substr(kFactoryPrefix.size()))));
        return preset ? factoryFile(preset->name) : file;
    }
    const std::wstring folder = folderPath();
    if (file.empty() || folder.empty())
        return file;
    try {
        const fs::path path = fs::path(file).lexically_normal();
        if (!sameNoCase(path.parent_path().wstring(), oldFactoryFolder(folder).wstring()) ||
            !sameNoCase(path.extension().wstring(), L".fmv"))
            return file;
        if (const FactoryPreset *preset = factoryPresetNamed(utf8(path.stem())))
            return factoryFile(preset->name);
    } catch (const std::exception &) {
    }
    return file;
}

std::string folderSortKey(const std::string &folder) {
    std::string key;
    for (char ch : folder)
        key += ch == '/' ? '\x01' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}

bool PresetLibrary::validName(const std::string &name) {
    if (name.empty() || name.size() > 120 || name.back() == '.' || name.back() == ' ' || name.front() == ' ')
        return false;
    for (char ch : name)
        if (static_cast<unsigned char>(ch) < 0x20 || std::strchr("\\/:*?\"<>|", ch))
            return false;
    std::string upper;
    for (char ch : name.substr(0, name.find('.')))
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    static const char *const kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                            "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                            "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::none_of(std::begin(kReserved), std::end(kReserved), [&](const char *r) { return upper == r; });
}

bool PresetLibrary::oldFactorySubfolder(const std::string &folder) {
    return _stricmp(folder.c_str(), "Factory") == 0;
}

std::wstring PresetLibrary::presetFile(const std::string &folder, const std::string &name) {
    const std::wstring top = folderPath();
    if (top.empty() || !validName(name))
        return {};
    std::filesystem::path path(top);
    for (size_t at = 0; at < folder.size();) {
        const size_t end = std::min(folder.find('/', at), folder.size());
        const std::string part = folder.substr(at, end - at);
        if (!validName(part))
            return {};
        path /= widePath(part);
        at = end + 1;
    }
    path /= widePath(name + ".fmv");
    return path.lexically_normal().wstring();
}

void PresetLibrary::rescan() {
    namespace fs = std::filesystem;
    entries_.clear();
    files_.clear();
    folders_.clear();
    struct Found {
        gui::PresetEntry entry;
        std::wstring file;
    };
    std::vector<Found> found;
    const std::vector<FactoryPreset> &factory = factoryPresets();
    if (factoryInfos_.size() != factory.size()) { // built in: read once
        factoryInfos_.assign(factory.size(), {});
        for (size_t n = 0; n < factory.size(); ++n) {
            MemoryReader reader{factory[n].data.data(), factory[n].data.size()};
            const clap_istream_t stream = memoryIn(reader);
            readPresetInfo(&stream, factoryInfos_[n]);
        }
    }
    for (size_t n = 0; n < factory.size(); ++n)
        found.push_back({{factory[n].name, {}, true, factoryInfos_[n]}, factoryFile(factory[n].name)});
    std::map<std::wstring, KnownInfo> known; // the files found this time, so those gone are forgotten
    const std::wstring folderName = folderPath();
    if (!folderName.empty()) {
        const fs::path folder(folderName);
        const std::wstring oldFactory = oldFactoryFolder(folderName).wstring();
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code fileError;
            if (it->is_directory(fileError)) {
                const std::string sub = utf8(it->path().lexically_relative(folder), true);
                if (!sub.empty())
                    folders_.push_back(sub);
                continue;
            }
            if (!it->is_regular_file(fileError) || _wcsicmp(it->path().extension().c_str(), L".fmv") != 0)
                continue;
            // A factory preset an earlier version wrote out, whatever has become of it: the list has it built in. The
            // user's own presets beside them are theirs, and listed.
            if (sameNoCase(it->path().parent_path().lexically_normal().wstring(), oldFactory) &&
                factoryPresetNamed(utf8(it->path().stem())))
                continue;
            const std::wstring file = it->path().lexically_normal().wstring();
            const fs::path parent = it->path().parent_path().lexically_relative(folder);
            const std::string sub = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
            std::string name = utf8(it->path().stem());
            if (name.empty()) // a name with no UTF-8 form can't be shown
                continue;
            Found f{{std::move(name), sub, false, {}}, file};
            // Its info as read before while the file is as it was, else read again.
            KnownInfo now{it->last_write_time(fileError), it->file_size(fileError), {}};
            const auto before = known_.find(file);
            if (!fileError && before != known_.end() && before->second.time == now.time &&
                before->second.size == now.size) {
                now.info = before->second.info;
            } else {
                FILE *in = nullptr;
                if (_wfopen_s(&in, file.c_str(), L"rb") == 0 && in) {
                    readPresetInfo(in, now.info);
                    std::fclose(in);
                }
            }
            f.entry.info = now.info;
            if (!fileError)
                known[file] = std::move(now);
            found.push_back(std::move(f));
        }
        // The old Factory subfolder only while it holds something of the user's: a preset listed, or a folder.
        const std::string oldSub = utf8(fs::path(oldFactory).lexically_relative(folder), true);
        auto under = [&](const std::string &sub) { // whatever the case it has on disk
            return sub.size() >= oldSub.size() && (sub.size() == oldSub.size() || sub[oldSub.size()] == '/') &&
                   _strnicmp(sub.c_str(), oldSub.c_str(), oldSub.size()) == 0;
        };
        const bool kept =
            std::any_of(found.begin(), found.end(), [&](const Found &f) { return under(f.entry.folder); }) ||
            std::any_of(folders_.begin(), folders_.end(),
                        [&](const std::string &sub) { return sub.size() > oldSub.size() && under(sub); });
        if (!kept)
            std::erase_if(folders_, [&](const std::string &sub) { return under(sub); });
    }
    known_ = std::move(known);
    // The factory presets first, then the top folder's presets, then each subfolder's, each by name.
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.factory != b.entry.factory)
            return a.entry.factory;
        if (a.entry.folder != b.entry.folder)
            return folderSortKey(a.entry.folder) < folderSortKey(b.entry.folder);
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (Found &f : found) {
        entries_.push_back(std::move(f.entry));
        files_.push_back(std::move(f.file));
    }
    std::sort(folders_.begin(), folders_.end(),
              [](const std::string &a, const std::string &b) { return folderSortKey(a) < folderSortKey(b); });
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

void PresetLibrary::openFolder(const std::string &sub) {
    PresetLibrary::makeFolder();
    const std::wstring top = folderPath();
    if (top.empty())
        return;
    std::filesystem::path folder(top);
    if (!sub.empty()) {
        const std::wstring file = presetFile(sub, "x"); // the subfolder's, as a file's would be in it
        if (!file.empty())
            folder = std::filesystem::path(file).parent_path();
    }
    ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PresetLibrary::showFile(int index) const {
    if (index < 0 || index >= static_cast<int>(files_.size()) || entries_[index].factory)
        return;
    // Explorer's own switch shows the folder with the file picked.
    const std::wstring arguments = L"/select,\"" + files_[index] + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

bool PresetLibrary::makeFolder(const std::string &sub) {
    const std::wstring file = presetFile(sub, "x");
    if (file.empty() || oldFactorySubfolder(sub))
        return false;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), error);
    rescan();
    return !error;
}

std::wstring PresetLibrary::subfolderPath(const std::string &sub) {
    if (sub.empty())
        return folderPath();
    const std::wstring file = presetFile(sub, "x"); // a file's in it, as its folder
    return file.empty() ? std::wstring() : std::filesystem::path(file).parent_path().wstring();
}

bool PresetLibrary::renameFolder(const std::string &from, const std::string &to) {
    const std::wstring a = subfolderPath(from), b = subfolderPath(to);
    if (from.empty() || to.empty() || a.empty() || b.empty() || (oldFactorySubfolder(to) && !oldFactorySubfolder(from)))
        return false;
    std::error_code error;
    if (_wcsicmp(a.c_str(), b.c_str()) != 0 && std::filesystem::exists(b, error)) // another of that name; not itself
        return false;
    std::filesystem::rename(a, b, error);
    rescan();
    return !error;
}

// Into the Recycle Bin, as Explorer's Delete does, so it can be brought back.
bool PresetLibrary::deleteFolder(const std::string &sub) {
    const std::wstring path = subfolderPath(sub);
    if (sub.empty() || path.empty())
        return false;
    std::wstring from = path;
    from.push_back(L'\0'); // SHFileOperation takes a list of paths that two nulls end
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const bool done = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    rescan();
    return done;
}

} // namespace fmvibe
