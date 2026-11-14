#include "usersettings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <windows.h>
#include <shlobj.h>

namespace fmvibe {

namespace {

std::filesystem::path userSettingsFile() {
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
}

UserSettings readFile(const std::filesystem::path &file) {
    UserSettings settings;
    std::ifstream in(file);
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
        else if (line.starts_with("zoom="))
            settings.zoom = std::clamp(std::atof(line.c_str() + 5), 0.0, 3.0);
        else if (line.starts_with("examples="))
            settings.examples = std::max(std::atoi(line.c_str() + 9), 0);
        else if (line.starts_with("examplesVersion="))
            settings.examplesVersion = line.substr(16);
        else if (line.starts_with("tableFolder=")) { // its frame size, its name, then its path, "|" between them
            const std::string rest = line.substr(12);
            const size_t a = rest.find('|'), b = a == std::string::npos ? a : rest.find('|', a + 1);
            if (b != std::string::npos && b + 1 < rest.size()) {
                TableFolder folder;
                const int size = std::atoi(rest.c_str());
                folder.frameSize = size == 256 || size == 512 || size == 1024 || size == 2048 ? size : 0;
                folder.name = rest.substr(a + 1, b - a - 1);
                folder.path = rest.substr(b + 1);
                settings.tableFolders.push_back(std::move(folder));
            }
        }
    }
    return settings;
}

// The settings as this process last read or wrote them, which tells what it changed since.
UserSettings asRead;

bool sameFolders(const std::vector<TableFolder> &a, const std::vector<TableFolder> &b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const TableFolder &x, const TableFolder &y) {
        return x.name == y.name && x.path == y.path && x.frameSize == y.frameSize;
    });
}

} // namespace

UserSettings userSettings;

std::string userSettingsFolder() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return {};
    const std::u8string folder = file.parent_path().u8string();
    return std::string(reinterpret_cast<const char *>(folder.data()), folder.size());
}

void readUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    userSettings = file.empty() ? UserSettings{} : readFile(file);
    asRead = userSettings;
}

void writeUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    // Another process may have written the file since this one read it: what this one didn't change since keeps
    // the file's value, and the examples given never count down, or deleted ones would come back.
    UserSettings merged = readFile(file);
    if (userSettings.theme != asRead.theme)
        merged.theme = userSettings.theme;
    if (userSettings.glow != asRead.glow)
        merged.glow = userSettings.glow;
    if (userSettings.hue != asRead.hue)
        merged.hue = userSettings.hue;
    if (userSettings.handleSize != asRead.handleSize)
        merged.handleSize = userSettings.handleSize;
    if (userSettings.links != asRead.links)
        merged.links = userSettings.links;
    if (userSettings.zoom != asRead.zoom)
        merged.zoom = userSettings.zoom;
    if (!sameFolders(userSettings.tableFolders, asRead.tableFolders))
        merged.tableFolders = userSettings.tableFolders;
    merged.examples = std::max(merged.examples, userSettings.examples);
    if (versionBefore(merged.examplesVersion, userSettings.examplesVersion))
        merged.examplesVersion = userSettings.examplesVersion;
    userSettings = asRead = merged;

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    error.clear(); // a folder that couldn't be made shows as the write failing
    // Written beside it under this process's own name, then put in its place: another process reading it meanwhile
    // (Bitwig runs instances in several) finds the old file or the new one, never one cut short, which would read
    // as no examples given and write them again.
    std::filesystem::path part = file;
    part += L"." + std::to_wstring(GetCurrentProcessId()) + L".part";
    {
        std::ofstream out(part);
        out << "theme=" << merged.theme << "\nglowLevel=" << merged.glow << "\nhandleSize=" << merged.handleSize
            << "\nlinkIntensity=" << merged.links << '\n';
        if (merged.hue >= 0.0)
            out << "hue=" << merged.hue << '\n';
        if (merged.zoom > 0.0)
            out << "zoom=" << merged.zoom << '\n';
        if (merged.examples >= 0)
            out << "examples=" << merged.examples << '\n';
        if (!merged.examplesVersion.empty())
            out << "examplesVersion=" << merged.examplesVersion << '\n';
        for (const TableFolder &folder : merged.tableFolders) {
            std::string name = folder.name; // a "|" in the name would end it early
            std::replace(name.begin(), name.end(), '|', '/');
            out << "tableFolder=" << folder.frameSize << '|' << name << '|' << folder.path << '\n';
        }
        out.close();
        if (!out)
            error = std::make_error_code(std::errc::io_error);
    }
    if (!error)
        replaceFile(part.wstring(), file.wstring());
    else {
        std::error_code ignored;
        std::filesystem::remove(part, ignored);
    }
}

bool versionBefore(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        auto number = [](const std::string &text, size_t &at) {
            long n = 0;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                n = std::min(n * 10 + (text[at++] - '0'), 1000000000L);
            while (at < text.size() && (text[at] < '0' || text[at] > '9')) // the dot, or whatever else, after it
                ++at;
            return n;
        };
        const bool aHas = i < a.size(), bHas = j < b.size();
        const long x = aHas ? number(a, i) : -1, y = bHas ? number(b, j) : -1;
        if (x != y)
            return x < y;
    }
    return false;
}

bool replaceFile(const std::wstring &part, const std::wstring &target) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD error = GetLastError();
        if (attempt >= 20 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED))
            break;
        Sleep(10);
    }
    DeleteFileW(part.c_str());
    return false;
}

} // namespace fmvibe
