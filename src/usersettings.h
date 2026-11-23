#pragma once

#include <string>
#include <utility>
#include <vector>

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
    double grid = 0.5;   // how strongly the SEQ view's roll draws its grid lines
    bool tintedCards = false; // a flat theme's cards tinted with their own color, or plain
    bool seqFull = true; // the SEQ view at its full size, over the modulation panel too, or at its half
    double zoom = 0.0; // the editor size new instances open at, relative to the default; 0 if never resized
    bool keysCollapsed = false; // whether new instances open with the keyboard panel collapsed, as it was left last
    // The preset library's: the favorite presets' files and the ones loaded last (the last first), in UTF-8 (a factory
    // preset's as its "file", see PresetLibrary::kFactoryPrefix), the user's own categories (each its name and its
    // color), where the browser opens (see gui::PresetStore::presetBrowserPlace()) and the author the user typed last
    // (see gui::PresetStore::presetAuthor()).
    std::vector<std::string> favorites;
    std::vector<std::string> recent;
    std::vector<std::pair<std::string, int>> categories;
    std::string browserPlace;
    std::string author;
};
constexpr size_t kMaxRecentPresets = 12;
extern UserSettings userSettings; // as the settings file has them. Main thread only

// Takes up the file as it is now, keeping what this process has changed and not written yet. A file that can't be read
// leaves userSettings as they are.
void readUserSettings();
// Writes what this process changed since it last read them into the file as it is now, where other processes may have
// changed the rest, a favorite or a category of theirs among them; userSettings then holds what was written. If the
// file can't be read or replaced just now, the change stays in userSettings, to go with the next read or write.
void writeUserSettings();

// What the file says new instances open with: the editor's size and whether the keyboard panel is collapsed (see
// UserSettings::zoom and UserSettings::keysCollapsed). Any thread: hosts may create instances on any, and it leaves
// userSettings alone.
UserSettings savedUserSettings();

// Puts the file written at part in target's place, waiting out another process that reads target meanwhile (another
// instance's process, a virus scanner, the search indexer), which keeps it from being replaced for a moment. If it
// can't, part is removed and target is left as it was.
bool replaceFile(const std::wstring &part, const std::wstring &target);

} // namespace fmvibe
