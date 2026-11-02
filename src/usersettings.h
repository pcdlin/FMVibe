#pragma once

#include <string>

namespace fmvibe {

// The editor's look is the user's rather than the project's: FMVibe's settings file in the roaming app
// data, read again whenever an editor opens, so a look picked in another process shows too.
struct UserSettings {
    std::string theme;
    double glow = 0.5;
    int examples = -1; // how many examples the preset folder has been given, or -1 if not known
};
extern UserSettings userSettings; // as the settings file has them; editors only run on the main thread

void readUserSettings();
void writeUserSettings();

} // namespace fmvibe
