#include "usersettings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "presets.h"

namespace fmvibe {

namespace {

// How long a file that another program holds for a moment (a virus scanner, the search indexer) is waited out.
constexpr int kBusyAttempts = 20;
constexpr int kBusyWaitMs = 10;
// Longer than another process can hold the lock while it waits out such a program, reading and then replacing.
constexpr int kLockWaitMs = 1000;

std::filesystem::path userSettingsFile() {
#ifdef _WIN32
    // The tests name a folder of their own, so they leave the user's settings alone.
    wchar_t override_[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"FMVIBE_SETTINGS_DIR", override_, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::filesystem::path(override_) / L"settings.txt";
    PWSTR appData = nullptr;
    std::filesystem::path file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        file = std::filesystem::path(appData) / L"FMVibe" / L"settings.txt";
    CoTaskMemFree(appData);
    return file;
#else
    // The tests name a folder of their own, so they leave the user's settings alone.
    if (const char *dir = std::getenv("FMVIBE_SETTINGS_DIR"); dir && *dir)
        return std::filesystem::path(dir) / "settings.txt";
    if (const char *config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return std::filesystem::path(config) / "FMVibe" / "settings.txt";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "FMVibe" / "settings.txt";
    return {};
#endif
}

#ifdef _WIN32
// Held from reading the file until the new one is in its place, so that processes (Bitwig runs instances in several)
// take turns. Where Windows gives no mutex, each goes ahead alone.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path &file) {
        const std::wstring name =
            L"Local\\FMVibeSettings-" + std::to_wstring(std::hash<std::wstring>{}(file.wstring()));
        mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (!mutex_)
            return;
        // Abandoned, by a process that ended holding it: the file is still whole, as it is only ever replaced.
        const DWORD waited = WaitForSingleObject(mutex_, kLockWaitMs);
        owned_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    ~FileLock() {
        if (owned_)
            ReleaseMutex(mutex_);
        if (mutex_)
            CloseHandle(mutex_);
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    bool held() const { return owned_ || !mutex_; }

private:
    HANDLE mutex_ = nullptr;
    bool owned_ = false;
};
#else
// The same with flock() on a lock file beside the settings: processes take turns, and a process that ended holding it
// leaves the file whole, as it is only ever replaced.
class FileLock {
public:
    explicit FileLock(const std::filesystem::path &file) {
        std::filesystem::path lock = file;
        lock += ".lock";
        std::error_code error;
        std::filesystem::create_directories(lock.parent_path(), error);
        fd_ = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0)
            return;
        for (int attempt = 0;; ++attempt) {
            if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
                owned_ = true;
                return;
            }
            if (attempt >= kLockWaitMs / kBusyWaitMs) {
                ::close(fd_);
                fd_ = -1;
                return;
            }
            ::usleep(kBusyWaitMs * 1000);
        }
    }
    ~FileLock() {
        if (fd_ >= 0) {
            if (owned_)
                ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
    }
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    bool held() const { return owned_; }

private:
    int fd_ = -1;
    bool owned_ = false;
};
#endif

UserSettings parsed(const std::string &text) {
    UserSettings settings;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("theme="))
            settings.theme = line.substr(6);
        else if (line.starts_with("glowLevel=")) // a plain "glow=" came before the 50 % default: left alone
            settings.glow = std::clamp(std::atof(line.c_str() + 10), 0.0, 1.0);
        else if (line.starts_with("hue="))
            settings.hue = std::min(std::atof(line.c_str() + 4), 1.0);
        else if (line.starts_with("handleSize="))
            settings.handleSize = std::clamp(std::atoi(line.c_str() + 11), 0, kMaxHandleSize);
        else if (line.starts_with("linkIntensity="))
            settings.links = std::clamp(std::atof(line.c_str() + 14), 0.0, 1.0);
        else if (line.starts_with("gridIntensity="))
            settings.grid = std::clamp(std::atof(line.c_str() + 14), 0.0, 1.0);
        else if (line.starts_with("tintedCards="))
            settings.tintedCards = std::atoi(line.c_str() + 12) != 0;
        else if (line.starts_with("seqFull="))
            settings.seqFull = std::atoi(line.c_str() + 8) != 0;
        else if (line.starts_with("zoom="))
            settings.zoom = std::clamp(std::atof(line.c_str() + 5), 0.0, 3.0);
        else if (line.starts_with("keysCollapsed="))
            settings.keysCollapsed = std::atoi(line.c_str() + 14) != 0;
        else if (line.starts_with("favorite=") && line.size() > 9)
            settings.favorites.push_back(line.substr(9));
        else if (line.starts_with("recent=") && line.size() > 7 && settings.recent.size() < kMaxRecentPresets)
            settings.recent.push_back(line.substr(7));
        else if (line.starts_with("category=")) { // its name, then | and its color
            const size_t bar = line.rfind('|');
            if (bar != std::string::npos && bar > 9)
                settings.categories.push_back({line.substr(9, bar - 9), std::max(std::atoi(line.c_str() + bar + 1), 0)});
        } else if (line.starts_with("browser="))
            settings.browserPlace = line.substr(8);
        else if (line.starts_with("author="))
            settings.author = line.substr(7);
    }
    return settings;
}

// The file's settings, the defaults if there is no file yet. False for a file that is there but can't be read, which
// says nothing of what it holds: taken for the defaults, they would be written over it.
bool readFile(const std::filesystem::path &file, UserSettings &settings) {
#ifdef _WIN32
    HANDLE in = INVALID_HANDLE_VALUE;
    for (int attempt = 0;; ++attempt) {
        in = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (in != INVALID_HANDLE_VALUE)
            break;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            settings = {};
            return true;
        }
        if (attempt >= kBusyAttempts || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED))
            return false;
        Sleep(kBusyWaitMs);
    }
    std::string text;
    char buffer[4096];
    DWORD got = 0;
    bool whole = false;
    while (ReadFile(in, buffer, sizeof(buffer), &got, nullptr)) {
        whole = got == 0; // read to its end
        if (whole)
            break;
        text.append(buffer, got);
    }
    CloseHandle(in);
    if (whole)
        settings = parsed(text);
    return whole;
#else
    int in = -1;
    for (int attempt = 0;; ++attempt) {
        in = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
        if (in >= 0)
            break;
        if (errno == ENOENT || errno == ENOTDIR) {
            settings = {};
            return true;
        }
        if (attempt >= kBusyAttempts || (errno != EBUSY && errno != EACCES && errno != ETXTBSY))
            return false;
        ::usleep(kBusyWaitMs * 1000);
    }
    std::string text;
    char buffer[4096];
    bool whole = false;
    for (;;) {
        const ssize_t got = ::read(in, buffer, sizeof buffer);
        if (got > 0) {
            text.append(buffer, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue; // a signal interrupted the read: it goes on, not taken for the file's end
        whole = got == 0;
        break;
    }
    ::close(in);
    if (whole)
        settings = parsed(text);
    return whole;
#endif
}

// Written beside the file under this process's own name, then put in its place: another process reading it meanwhile
// finds the old file or the new one, never one cut short.
bool writeFile(const std::filesystem::path &file, const UserSettings &settings) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    error.clear(); // a folder that couldn't be made shows as the write failing
    std::filesystem::path part = file;
#ifdef _WIN32
    part += L"." + std::to_wstring(GetCurrentProcessId()) + L".part";
#else
    part += "." + std::to_string(::getpid()) + ".part";
#endif
    {
        std::ofstream out(part);
        out << "theme=" << settings.theme << "\nglowLevel=" << settings.glow << "\nhandleSize=" << settings.handleSize
            << "\nlinkIntensity=" << settings.links << "\ngridIntensity=" << settings.grid
            << "\nseqFull=" << (settings.seqFull ? 1 : 0) << "\ntintedCards=" << (settings.tintedCards ? 1 : 0)
            << '\n';
        if (settings.hue >= 0.0)
            out << "hue=" << settings.hue << '\n';
        if (settings.zoom > 0.0)
            out << "zoom=" << settings.zoom << '\n';
        if (settings.keysCollapsed)
            out << "keysCollapsed=1\n";
        if (!settings.browserPlace.empty())
            out << "browser=" << settings.browserPlace << '\n';
        if (!settings.author.empty())
            out << "author=" << settings.author << '\n';
        for (const auto &[name, color] : settings.categories)
            out << "category=" << name << '|' << color << '\n';
        for (const std::string &favorite : settings.favorites)
            out << "favorite=" << favorite << '\n';
        for (const std::string &recent : settings.recent)
            out << "recent=" << recent << '\n';
        out.close();
        if (!out)
            error = std::make_error_code(std::errc::io_error);
#ifndef _WIN32
        if (!error) { // the bytes on the disk before the rename, as MoveFileEx's WRITE_THROUGH does on Windows
            const int fd = ::open(part.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ::fsync(fd);
                ::close(fd);
            }
        }
#endif
    }
    if (!error)
#ifdef _WIN32
        // The path's own wide form, as wideOf() is on Windows: the smoke test builds this file without presets.cpp.
        return replaceFile(part.wstring(), file.wstring());
#else
        return replaceFile(wideOf(part), wideOf(file));
#endif
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    return false;
}

// `theirs` with what `mine` added to `base` and took out of it, an entry being known by its key.
template <class T, class Key>
std::vector<T> mergedList(const std::vector<T> &base, const std::vector<T> &mine, const std::vector<T> &theirs, Key key) {
    auto has = [&](const std::vector<T> &list, const auto &wanted) {
        return std::any_of(list.begin(), list.end(), [&](const T &entry) { return key(entry) == wanted; });
    };
    std::vector<T> merged = theirs;
    std::erase_if(merged, [&](const T &entry) { return has(base, key(entry)) && !has(mine, key(entry)); });
    for (const T &entry : mine) {
        if (has(base, key(entry)))
            continue;
        const auto same = std::find_if(merged.begin(), merged.end(), [&](const T &e) { return key(e) == key(entry); });
        if (same != merged.end())
            *same = entry;
        else
            merged.push_back(entry);
    }
    return merged;
}

// The recent presets, the last first: those `mine` put ahead of what it kept in order from `base`, then `theirs`.
std::vector<std::string> mergedRecent(const std::vector<std::string> &base, const std::vector<std::string> &mine,
                                      const std::vector<std::string> &theirs) {
    size_t ahead = mine.size();
    for (size_t at = base.size(); ahead > 0; --ahead) {
        size_t found = at;
        while (found > 0 && base[found - 1] != mine[ahead - 1])
            --found;
        if (found == 0)
            break;
        at = found - 1;
    }
    std::vector<std::string> merged(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(ahead));
    auto has = [](const std::vector<std::string> &list, const std::string &entry) {
        return std::find(list.begin(), list.end(), entry) != list.end();
    };
    for (const std::string &entry : theirs)
        if (!has(merged, entry) && (has(mine, entry) || !has(base, entry)))
            merged.push_back(entry);
    if (merged.size() > kMaxRecentPresets)
        merged.resize(kMaxRecentPresets);
    return merged;
}

// `theirs`, the file as it is now, with what this process changed from `base` to `mine`. Another process's changes to
// the same file stay, down to each entry of the lists.
UserSettings merged(const UserSettings &base, const UserSettings &mine, const UserSettings &theirs) {
    UserSettings out = theirs;
    if (mine.theme != base.theme)
        out.theme = mine.theme;
    if (mine.glow != base.glow)
        out.glow = mine.glow;
    if (mine.hue != base.hue)
        out.hue = mine.hue;
    if (mine.handleSize != base.handleSize)
        out.handleSize = mine.handleSize;
    if (mine.links != base.links)
        out.links = mine.links;
    if (mine.grid != base.grid)
        out.grid = mine.grid;
    if (mine.seqFull != base.seqFull)
        out.seqFull = mine.seqFull;
    if (mine.tintedCards != base.tintedCards)
        out.tintedCards = mine.tintedCards;
    if (mine.zoom != base.zoom)
        out.zoom = mine.zoom;
    if (mine.keysCollapsed != base.keysCollapsed)
        out.keysCollapsed = mine.keysCollapsed;
    if (mine.browserPlace != base.browserPlace)
        out.browserPlace = mine.browserPlace;
    if (mine.author != base.author)
        out.author = mine.author;
    out.favorites = mergedList(base.favorites, mine.favorites, theirs.favorites, [](const std::string &e) { return e; });
    out.categories = mergedList(base.categories, mine.categories, theirs.categories,
                                [](const std::pair<std::string, int> &e) { return e.first; });
    if (mine.recent != base.recent)
        out.recent = mergedRecent(base.recent, mine.recent, theirs.recent);
    return out;
}

// The file as this process last read or wrote it: what userSettings differs from it by, this process changed and has
// yet to write.
UserSettings asRead;

} // namespace

UserSettings userSettings;

void readUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    const FileLock lock(file);
    UserSettings theirs;
    if (!readFile(file, theirs))
        return;
    userSettings = merged(asRead, userSettings, theirs);
    asRead = std::move(theirs);
}

void writeUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    const FileLock lock(file);
    UserSettings theirs;
    if (!lock.held() || !readFile(file, theirs))
        return; // what this process changed stays to write, with its next read or write
    userSettings = merged(asRead, userSettings, theirs);
    asRead = writeFile(file, userSettings) ? userSettings : std::move(theirs);
}

UserSettings savedUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    UserSettings settings;
    return !file.empty() && readFile(file, settings) ? settings : UserSettings{};
}

bool replaceFile(const std::wstring &part, const std::wstring &target) {
#ifdef _WIN32
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD error = GetLastError();
        if (attempt >= kBusyAttempts || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED))
            break;
        Sleep(kBusyWaitMs);
    }
    DeleteFileW(part.c_str());
    return false;
#else
    // rename() replaces the target in one step, as MoveFileEx does, and is the file's own name on POSIX.
    const std::string from = utf8Text(part), to = utf8Text(target);
    for (int attempt = 0;; ++attempt) {
        if (::rename(from.c_str(), to.c_str()) == 0)
            return true;
        if (attempt >= kBusyAttempts || (errno != EBUSY && errno != EACCES))
            break;
        ::usleep(kBusyWaitMs * 1000);
    }
    ::unlink(from.c_str());
    return false;
#endif
}

} // namespace fmvibe
