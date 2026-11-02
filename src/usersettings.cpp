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
    PWSTR appData = nullptr;
    std::filesystem::path file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        file = std::filesystem::path(appData) / L"FMVibe" / L"settings.txt";
    CoTaskMemFree(appData);
    return file;
}

} // namespace

UserSettings userSettings;

void readUserSettings() {
    userSettings = {};
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("theme="))
            userSettings.theme = line.substr(6);
        else if (line.starts_with("glowLevel=")) // a plain "glow=" came before the 50 % default: left alone
            userSettings.glow = std::clamp(std::atof(line.c_str() + 10), 0.0, 1.0);
        else if (line.starts_with("examples="))
            userSettings.examples = std::max(std::atoi(line.c_str() + 9), 0);
    }
}

void writeUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file);
    out << "theme=" << userSettings.theme << "\nglowLevel=" << userSettings.glow << '\n';
    if (userSettings.examples >= 0)
        out << "examples=" << userSettings.examples << '\n';
}

} // namespace fmvibe
