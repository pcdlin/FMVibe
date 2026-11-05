#pragma once

#include <string>

namespace fmvibe {

// The editor's look is the user's rather than the project's: FMVibe's settings file in the roaming app
// data, read again whenever an editor opens, so a look picked in another process shows too.
constexpr int kMaxHandleSize = 2;

struct UserSettings {
    std::string theme;
    double glow = 0.5;
    int handleSize = 0; // the depth handles' and pills' size, 0 to kMaxHandleSize
    int examples = -1; // how many examples the preset folder has been given, or -1 if not known
};
extern UserSettings userSettings; // as the settings file has them; editors only run on the main thread

void readUserSettings();
// Writes what this process changed since it last read them into the file as it is now, where another process may
// have changed the others (and the examples count only goes up); userSettings then holds what was written.
void writeUserSettings();

} // namespace fmvibe
