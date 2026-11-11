#pragma once

#include <string>

namespace fmvibe {

// The editor's look is the user's rather than the project's: FMVibe's settings file in the roaming app
// data, read again whenever an editor opens, so a look picked in another process shows too.
constexpr int kMaxHandleSize = 2;

struct UserSettings {
    std::string theme;
    double glow = 0.5;
    double hue = -1.0; // the theme's main color as a hue, 0 to 1, or -1 for the theme's own
    int handleSize = 0; // the depth handles' and pills' size, 0 to kMaxHandleSize
    double links = 0.75; // how strongly the dashed lines from a source to its targets are drawn
    double zoom = 0.0; // the editor size new instances open at, relative to the default; 0 if never resized
    int examples = -1; // how many examples the preset folder has been given, or -1 if not known
    std::string examplesVersion; // the version of FMVibe that last wrote them out, or empty if none has
};
extern UserSettings userSettings; // as the settings file has them; editors only run on the main thread

void readUserSettings();
// Writes what this process changed since it last read them into the file as it is now, where another process may
// have changed the others (and the examples count and their version only go up); userSettings then holds what was
// written.
void writeUserSettings();

// Puts the file written at part in target's place, waiting out another process that reads target meanwhile (another
// instance's process, a virus scanner, the search indexer), which keeps it from being replaced for a moment. If it
// can't, part is removed and target is left as it was.
bool replaceFile(const std::wstring &part, const std::wstring &target);

// Whether version `a` ("1.12", say) comes before `b`, number by number; an empty one before any other.
bool versionBefore(const std::string &a, const std::string &b);

} // namespace fmvibe
