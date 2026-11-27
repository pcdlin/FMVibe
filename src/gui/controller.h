#pragma once

#include "../params.h"
#include "../synth.h"
#include "../uistrings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe::gui {

// Nodes of the routing graph: the operators, then the filters, then the shapers.
constexpr int kNodes = kOps + kFilters + kShapers;

// What the routing panel shows: the graph, the matrix (both edit the same
// routing), the effects chain, every modulation (MODS) or the sequencer (SEQ, whose tab comes before MODS). Saved by
// number: only append.
enum class RoutingView { Graph, Matrix, Fx, Mods, Seq };

// The grids the SEQ view's roll snaps notes to, in steps (0 off), as its snap list offers them.
constexpr double kSeqSnaps[] = {0.0, 4.0, 2.0, 1.0, 0.5, 0.25, 4.0 / 3.0, 2.0 / 3.0};
constexpr int kSeqSnapCount = static_cast<int>(std::size(kSeqSnaps));
constexpr const char *kSeqSnapNames[] = {nullptr, "1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T"};
static_assert(std::size(kSeqSnapNames) == std::size(kSeqSnaps));
// The values the SEQ view's lane shows for each note: its velocity, or its chance.
// What the lane under the roll shows: the notes' velocities or chances, or the pattern's pitch bend, mod wheel or a
// macro's curve (see SeqCurve). Stored by number: only append.
enum class SeqLane { Velocity, Chance, Bend, ModWheel, Macro1, Count = Macro1 + kMacros };
constexpr bool seqLaneMacro(SeqLane lane) { return lane >= SeqLane::Macro1; }

// What the lower half of an operator's page shows, one for every operator. Saved by number, so the tabs' order on
// the page (Envelope, Unison, Warp, Inputs, Keys) is layout.h's opTab(). Warp is the WARP tab (its scope, warps and note
// sync); Scope, which had a tab of its own, shows it too, so a layout saved on it opens WARP. A noise operator shows
// its spectrum in Unison's place, and its envelope in Warp's (noise has no cycle to show, warp or sync).
// Stored in the layout by number: only append.
// Keys is keyboard level scaling: the operator's level across the keys.
enum class InspectorTab { Unison, Envelope, Scope, Inputs, Warp, Keys, Count };

// How many of the operator's cycles the scope shows across.
constexpr int kScopeCycleChoices[] = {1, 2, 4};

constexpr std::array<int, kMaxSources> identitySourceOrder() {
    std::array<int, kMaxSources> order{};
    for (int i = 0; i < kMaxSources; ++i)
        order[i] = i;
    return order;
}

// What a preset says about itself: who made it, its category and a line about it. Saved with the patch, in the layout
// chunk (see UiLayout::presetInfo), so a preset file and the host's project carry it; a preset saved before it has
// none. The category is stored by name: a built-in one by its English name (see kPresetCategories), any other as typed.
struct PresetInfo {
    std::string author;
    std::string category;
    std::string description; // one line
    bool empty() const { return author.empty() && category.empty() && description.empty(); }
    bool operator==(const PresetInfo &) const = default;
};
// The most characters typed into each, and the most bytes saved (room for UTF-8 from a file).
constexpr size_t kMaxPresetAuthor = 32;
constexpr size_t kMaxPresetCategory = 6; // a new one's name, as long as the built-in ones' at most (Pluck)
constexpr size_t kMaxPresetDescription = 80;
constexpr size_t kMaxPresetName = 48;

// The categories FMVibe offers, by the name presets store; each its color (see categoryColor()), and the two letters
// a factory preset's name started with up to 0.11.
struct BuiltInCategory {
    const char *name;
    Str label; // as the editor shows it
    Str pill;  // in capitals, as its pill shows it
    const char *prefix;
};
inline constexpr BuiltInCategory kPresetCategories[] = {
    {"Bass", Str::CategoryBass, Str::CategoryBassPill, "BA"},
    {"Drum", Str::CategoryDrum, Str::CategoryDrumPill, "DR"},
    {"FX", Str::CategoryFx, Str::CategoryFxPill, "FX"},
    {"Keys", Str::CategoryKeys, Str::CategoryKeysPill, "KY"},
    {"Lead", Str::CategoryLead, Str::CategoryLeadPill, "LD"},
    {"Pad", Str::CategoryPad, Str::CategoryPadPill, "PD"},
    {"Pluck", Str::CategoryPluck, Str::CategoryPluckPill, "PL"},
    {"Seq", Str::CategorySeq, Str::CategorySeqPill, "SQ"},
};
constexpr int kBuiltInCategories = static_cast<int>(std::size(kPresetCategories));
// A category of the user's own: its name and its color, one of kCustomCategoryColors after the built-in ones'.
// Kept in the user's settings, so every FMVibe offers it.
constexpr int kCustomCategoryColors = 5;
struct PresetCategory {
    std::string name;
    int color = 0; // 0 to kBuiltInCategories + kCustomCategoryColors - 1
};
// Whether two names are the same, whatever the case of their ASCII letters.
inline bool sameName(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return (x >= 'A' && x <= 'Z' ? x - 'A' + 'a' : x) == (y >= 'A' && y <= 'Z' ? y - 'A' + 'a' : y);
           });
}
// The built-in category named `name` (whatever its case), or -1.
inline int builtInCategory(std::string_view name) {
    for (int i = 0; i < kBuiltInCategories; ++i)
        if (sameName(kPresetCategories[i].name, name))
            return i;
    return -1;
}
// Up to 0.11, a factory preset's name started with its category's two letters and a space ("LD Screamer"), as a
// project saved then names it: the category they name, or -1 (see factoryPresetNamed()).
inline int prefixCategory(std::string_view name) {
    if (name.size() < 4 || name[2] != ' ')
        return -1;
    for (int i = 0; i < kBuiltInCategories; ++i)
        if (sameName(name.substr(0, 2), kPresetCategories[i].prefix))
            return i;
    return -1;
}
// Editor state that isn't a param but is saved with the patch.
constexpr size_t kMaxMacroName = 24; // bytes of a macro's name, as the state saves it

struct UiLayout {
    // Node positions in the routing graph, 0..1 within the graph area: the operators' circles, and the boxes of the
    // filters and then the shapers. Default: on the points of the grid Ctrl+drag snaps to (the centers of its 12 x 5
    // cells, see kRoutingGridCols), mirrored about OUT's row: the operators on the left, F and E in the second column
    // at the top and bottom, C and B in the fourth a row in from them, D and A in the seventh at the top and bottom;
    // then the shapers in the ninth, S2 above OUT's row and S1 below it, and the filters in the eleventh, F2 at the
    // top and F1 at the bottom.
    std::array<float, kOps> nodeX{13.0f / 24.0f, 7.0f / 24.0f, 7.0f / 24.0f, 13.0f / 24.0f, 3.0f / 24.0f, 3.0f / 24.0f};
    std::array<float, kOps> nodeY{0.9f, 0.7f, 0.3f, 0.1f, 0.9f, 0.1f};
    std::array<float, kFilters + kShapers> boxX{21.0f / 24.0f, 21.0f / 24.0f, 17.0f / 24.0f, 17.0f / 24.0f};
    std::array<float, kFilters + kShapers> boxY{0.9f, 0.1f, 0.7f, 0.3f};
    int selectedNode = 0;    // shown in the inspector panel
    // The order the editor shows the sources in: every place once, empty ones included.
    std::array<int, kMaxSources> sourceOrder = identitySourceOrder();
    RoutingView view = RoutingView::Graph;
    float zoom = 1.0f;       // editor size relative to the default, before DPI scaling
    // Retired: the modulation panel collapsed to its title row once, and no longer does. Kept for its place in the
    // layout chunk, read and written and otherwise left alone.
    bool retiredSourcesCollapsed = false;
    bool keysCollapsed = false;    // the keyboard panel shows only its top row, without the wheels and keys
    // Routes (param indices) that keep their wire at 0 %, so modulation can still be dropped on
    // them. A route joins once it is on, and leaves only when it is disconnected.
    std::vector<int> keptRoutes;
    // How many lines the shape editor's grid has across and up and down; Ctrl snaps to them. 8 x 8 to begin with.
    // Like the zoom, it belongs to the editor, not the patch: presets leave it alone.
    int shapeGridX = 8;
    int shapeGridY = 8;
    // The macros' names, empty for "Macro 1" and so on: part of the patch, like the node places. UTF-8, at most
    // kMaxMacroName bytes (whole characters), as the state saves them.
    std::array<std::string, kMacros> macroNames{};
    // The effect slots' and source places' cards folded to a narrow strip, a bit each; saved with the patch,
    // but not undone.
    uint32_t fxFolded = 0;      // the first kFirstFxSlots slots
    uint32_t fxFoldedLater = 0; // the slots after them
    uint32_t sourcesFolded = 0;
    InspectorTab opTab = InspectorTab::Envelope;
    int scopeCycles = 2;
    // How far the graph's wires bow to their left, as a share of the distance each spans, by route (a param index):
    // those dragged away from kDefaultWireBend, which the others keep. Part of the patch, like the node places.
    std::vector<std::pair<int, float>> wireBends;
    // Modulations added in the MODS view that have no source or no target yet, in the order they were added: each its
    // source (-1 for none) and its target (a param index, -1 for none). They modulate nothing; once both are picked,
    // the modulation takes a slot of its source and leaves this list. Part of the patch, like the node places.
    std::vector<std::pair<int, int>> emptyMods;
    // How shapes and curves are edited, with the grid: whether a click on one draws a step (Shift then works with
    // points), and whether its points snap to the grid (Ctrl then lets them go). They belong to the editor, like the
    // grid.
    bool shapeSteps = false;
    bool shapeSnap = false;
    // The node the effects graph's inspector shows (a slot, a split or a join, numbered as the graph's params number
    // them), -1 for none. Where the nodes sit follows from the wires (see View::fxGridLayout()).
    int fxSelected = -1;
    // Whether the FX view shows the effects as their graph rather than as a row of cards: two views of the same
    // effects, which switching between changes nothing of. Saved with the patch, like the routing view, but a preset
    // loaded leaves it as it is (see keepEditorLayout()).
    bool fxGraphView = false;
    // The band each split shows, by split (0 its low band, 1 its high one, 2 its middle one): the band whose effects its container in
    // the row shows, and whose response its card fills. Part of the patch, like the node places.
    std::array<int, kFxSplits> splitBand{};
    // The splits' containers in the row folded to a narrow strip, a bit each; saved with the patch, but not undone.
    uint32_t splitsFolded = 0;
    // The SEQ view's roll: whether a click draws (rather than selects), the grid it snaps to (kSeqSnaps), the lane it
    // shows, the lowest pitch it shows, whether it shows only the scale's rows, and whether a click bends notes (the
    // Bend tool, over seqDraw). They belong to the editor, like the shape editor's grid.
    bool seqDraw = false;
    int seqSnap = 3; // 1/16
    SeqLane seqLane = SeqLane::Velocity;
    int seqLow = 48;
    bool seqFold = false;
    bool seqBend = false;
    // The tempo the patch was last saved as a preset at, in BPM (the one it played at, whatever host it was), 0 for
    // none (a preset saved before it was kept, Init). The synth's own tempo (see Controller::ownTempo()) and a host
    // that lets the editor set the tempo (see Controller::hasTempo()) take it up as the preset loads, so a sequencer
    // pattern plays at the tempo it was made at; a plugin host's tempo stays its own. Part of the patch, like the node
    // places, but not undone.
    float savedTempo = 0.0f;
    // What the preset says about itself (see PresetInfo). Part of the patch, but not undone: it changes as a preset is
    // saved, or its info edited.
    PresetInfo presetInfo;
    // The names of the files the patch's own tuning came from (see TuningParam): its scale's and its keyboard
    // mapping's, each the file's name without its extension, as the overflow menu shows them; empty for none. Part of
    // the patch, undone with the tuning. UTF-8, at most kMaxTuningName bytes (whole characters).
    std::string tuningName;
    std::string mappingName;
};
constexpr size_t kMaxTuningName = 64;

// What a preset or Init leaves as it is, taken from the layout before it: how big the editor is, whether the keyboard
// strip is collapsed, the shape editor's grid, the operator page's tab and the scope's cycles, and the view shown (the routing
// view's tab, and the FX view's row or graph). A host's project brings them all back.
inline void keepEditorLayout(UiLayout &to, const UiLayout &from) {
    to.zoom = from.zoom;
    to.keysCollapsed = from.keysCollapsed;
    to.shapeGridX = from.shapeGridX;
    to.shapeGridY = from.shapeGridY;
    to.shapeSteps = from.shapeSteps;
    to.shapeSnap = from.shapeSnap;
    to.opTab = from.opTab;
    to.scopeCycles = from.scopeCycles;
    to.view = from.view;
    to.fxGraphView = from.fxGraphView;
    to.seqDraw = from.seqDraw;
    to.seqSnap = from.seqSnap;
    to.seqLane = from.seqLane;
    to.seqLow = from.seqLow;
    to.seqFold = from.seqFold;
    to.seqBend = from.seqBend;
}

// How many empty modulations the MODS view keeps at most.
constexpr int kMaxEmptyMods = 32;

// How far a wire bows to its left, as a share of the distance it spans, until it is dragged: enough that two wires
// running opposite ways between two nodes don't overlap. Dragged, it goes from -kMaxWireBend to kMaxWireBend.
constexpr float kDefaultWireBend = 0.18f;
constexpr float kMaxWireBend = 1.0f;

// A macro's name as shown: its own, or "Macro 1" and so on.
inline std::string macroName(const UiLayout &layout, int macro) {
    return layout.macroNames[macro].empty() ? trf(Str::MacroDefaultName, {std::to_string(macro + 1)})
                                        : layout.macroNames[macro];
}

// The grids the shape editor offers, across (a cycle, or the shaper's input) and up and down.
constexpr int kShapeGridsX[] = {2, 3, 4, 6, 8, 12, 16, 24, 32};
constexpr int kShapeGridsY[] = {2, 4, 6, 8, 12, 16};

// The places that hold a source, in the order the editor shows them. `get(index)` returns a param value.
template <class Get>
std::vector<int> shownSources(const UiLayout &layout, const Get &get) {
    std::vector<int> shown;
    for (int source : layout.sourceOrder)
        if (sourceType(get(paramIndex(source, SourceParam::Type))) != SourceType::None)
            shown.push_back(source);
    return shown;
}

// What the editor and the host's remote pages call a source: "LFO 2", "Env 1", "Random 1" or "MIDI 1",
// numbered by type in the order the sources show, a macro's name, "Note", "Mod Wheel" or an operator's "Op A".
template <class Get>
std::string sourceName(const UiLayout &layout, const Get &get, int source) {
    if (isOpSource(source))
        return trf(Str::SourceNameOp, {std::string(1, static_cast<char>('A' + source - kOpSource))});
    if (isNote(source))
        return tr(Str::SourceNameNote);
    if (isWheel(source))
        return tr(Str::SourceNameWheel);
    if (isVelocity(source))
        return tr(Str::SourceNameVelocity);
    if (isBend(source))
        return tr(Str::SourceNameBend);
    if (isMacro(source))
        return macroName(layout, source - kMaxSources);
    const SourceType type = sourceType(get(paramIndex(source, SourceParam::Type)));
    int number = 1;
    for (int other : layout.sourceOrder) {
        if (other == source)
            break;
        number += sourceType(get(paramIndex(other, SourceParam::Type))) == type ? 1 : 0;
    }
    const Str name = type == SourceType::Env        ? Str::SourceNameEnv
                     : type == SourceType::Random   ? Str::SourceNameRandom
                     : type == SourceType::Midi     ? Str::SourceNameMidi
                     : type == SourceType::Curve    ? Str::SourceNameCurve
                                                    : Str::SourceNameLfo;
    return trf(name, {std::to_string(number)});
}

// The tempo a host that stands in for a transport offers the editor (see Controller::hasTempo()): its range, where it
// starts, and a tempo held to the range and rounded to hundredths of a BPM (one that isn't a number, to the default).
constexpr double kMinTempo = 20.0, kMaxTempo = 999.0, kDefaultTempo = 140.0;
inline double clampTempo(double bpm) {
    if (std::isnan(bpm))
        return kDefaultTempo;
    return std::round(std::clamp(bpm, kMinTempo, kMaxTempo) * 100.0) / 100.0;
}

// Where the user manual is online, the released version's (as the README links it).
inline constexpr const char *kManualUrl = "https://pcdlin.github.io/FMVibe/manual.html";

// A preset in the library: a factory preset, built in, or a .fmv file in the preset folder or one of its subfolders.
struct PresetEntry {
    std::string name;   // the file's name without .fmv, or the factory preset's
    std::string folder; // the subfolder it is in, its parts joined by '/', "" at the top (and for a factory preset)
    bool factory = false;
    PresetInfo info;    // as its file has it
};

// The file dialogs a host window opens for the presets: Load's and Save's (a preset's file), and the overflow menu's
// Import sysex (a DX7 sysex file, see PluginCore::importDx7()).
enum class PresetDialog { Load, Save, ImportDx7 };

// The preset library and the patch as a file: what the preset bar, the preset browser and the Save dialog need from
// the plugin, reached through Controller::presetStore(). Main thread only. Which preset the patch is, and whether it
// changed since, is not the store's but the undo history's (see UndoHistory::presetFile()): an edit changes it, and
// undo and redo bring it back with the params. The Controller shows it to the editor (Controller::presetHasFile(),
// Controller::patchEdited()), and the store sets it as it loads and saves.
class PresetStore {
public:
    virtual ~PresetStore() = default;

    // Presets: the whole patch, as a file. Init sets every param and the layout back to their defaults.
    virtual void resetPreset() = 0;
    virtual bool loadPreset(const std::wstring &path) = 0;
    virtual bool savePreset(const std::wstring &path) = 0;
    // Asks the host window for a file dialog; the result comes back through loadPreset(), savePreset() or importDx7().
    virtual void choosePreset(PresetDialog dialog) = 0;
    // Imports a DX7 sysex file: its voices saved as presets and the first loaded. False where none could be.
    virtual bool importDx7(const std::wstring &) { return false; }
    // Whether presets are files that can be written (not in the browser demo, whose dialog only says so).
    virtual bool writesPresetFiles() const { return true; }
    // Whether Save can write over the patch's preset file: there is one (not Init), and it isn't a factory preset's,
    // which is built into the plugin, so Save saves it as a new preset, as Save As does.
    virtual bool presetWritable() const = 0;

    // The preset library: the factory presets, built in, and the presets in the preset folder
    // (Documents\FMVibe\presets), by subfolder (the factory presets as Factory's) and then by name, as the last
    // scan found them.
    virtual void rescanPresets() = 0;
    virtual const std::vector<PresetEntry> &presets() const = 0;
    // The entry loaded or saved last, or -1 after Init or for a file from elsewhere.
    virtual int presetIndex() const = 0;
    // "- Init -", or the name of the preset loaded or saved last.
    virtual std::string presetName() const = 0;
    virtual bool loadPresetAt(int index) = 0;
    virtual std::wstring presetFolder() const = 0;
    // Shows the preset folder's subfolder `folder` (its parts joined by '/', "" the preset folder) in Explorer.
    virtual void openPresetFolder(const std::string &) {}
    // Shows the preset's file in Explorer, picked: not a factory preset's, which has none.
    virtual void showPresetFile(int) {}
    // The preset folder's subfolders, as the last scan found them, empty ones too: each its parts joined by '/', every
    // folder after the one it is in, by name.
    virtual std::vector<std::string> presetFolders() const { return {}; }
    // Makes the subfolder `folder` (its parts joined by '/') and scans again; false if it can't be made.
    virtual bool makePresetFolder(const std::string &) { return false; }
    // Renames the subfolder `from` to `to` (in the same folder), and deletes one into the Recycle Bin with all it
    // holds; each scans again. False if it can't. A preset's file in a folder renamed is kept track of.
    virtual bool renamePresetFolder(const std::string &, const std::string &) { return false; }
    virtual bool deletePresetFolder(const std::string &) { return false; }
    // Saves the patch as the preset `name` in the subfolder `folder`, with `info`: it is then the patch's preset, and
    // the library lists it. Whether a preset of that name is there already, which saving replaces.
    virtual bool savePresetIn(const std::string &, const std::string &, const PresetInfo &) { return false; }
    virtual bool presetExists(const std::string &, const std::string &) const { return false; }
    // Writes `info` into the file of the library's preset `index`, leaving its sound alone (and the patch, though it
    // is that preset: only its info then changes, as the file's). Not a factory preset's.
    virtual bool setPresetInfo(int, const PresetInfo &) { return false; }
    // Moves the file of the library's preset `index` into the subfolder `folder`, replacing a preset of its name there,
    // and scans again: the patch, a favorite and a recent preset that are that preset follow it. Not a factory
    // preset's. False if it can't.
    virtual bool movePreset(int, const std::string &) { return false; }
    // The author the user typed into the Save sheet last, kept in their settings: a preset without one starts with it.
    virtual std::string presetAuthor() const { return {}; }
    virtual void setPresetAuthor(const std::string &) {}
    // The user's favorite presets and the ones loaded last (the last first), by entry: kept in the user's settings.
    virtual bool presetFavorite(int) const { return false; }
    virtual void setPresetFavorite(int, bool) {}
    virtual std::vector<int> recentPresets() const { return {}; }
    // The user's own categories, kept in their settings after the built-in ones (see kPresetCategories).
    virtual std::vector<PresetCategory> userCategories() const { return {}; }
    virtual void addUserCategory(const PresetCategory &) {}
    // Takes one of them out of the list by its name; the presets in it keep it (and show it as a category the list
    // doesn't know).
    virtual void removeUserCategory(const std::string &) {}
    // Where the preset browser opens: the sidebar's entry picked last ("all", "favorites", "recent", "factory", or
    // "folder:" and the folder, "" the preset folder itself), kept in the user's settings; "" for none yet.
    virtual std::string presetBrowserPlace() const { return {}; }
    virtual void setPresetBrowserPlace(const std::string &) {}
};

// The preset store of a host without files: an empty library, nothing to load or save, and Init leaving the patch as
// it is. A Controller that keeps no presets offers this one (see Controller::presetStore()); the browser demo starts
// from it and adds its built-in examples.
class NoPresetFiles : public PresetStore {
public:
    void resetPreset() override {}
    bool loadPreset(const std::wstring &) override { return false; }
    bool savePreset(const std::wstring &) override { return false; }
    void choosePreset(PresetDialog) override {}
    bool writesPresetFiles() const override { return false; }
    bool presetWritable() const override { return false; }
    void rescanPresets() override {}
    const std::vector<PresetEntry> &presets() const override { return none_; }
    int presetIndex() const override { return -1; }
    std::string presetName() const override { return tr(Str::PresetInit); }
    bool loadPresetAt(int) override { return false; }
    std::wstring presetFolder() const override { return {}; }

private:
    std::vector<PresetEntry> none_;
};

// The editor's look: the user's rather than the patch's or the project's, so every FMVibe editor uses it, whichever
// project it is in, and presets leave it alone. Read and written as a whole (see Controller::editorSettings()).
struct EditorSettings {
    // Where the glow, the lines' strength and the grid's start, as a new user's settings have them (a double-click on
    // their sliders puts them back).
    static constexpr double kDefaultGlow = 0.5;
    static constexpr double kDefaultLinkIntensity = 0.75;
    static constexpr double kDefaultGridIntensity = 0.5;
    static constexpr int kMaxHandleSize = 2;

    // The color theme, by name (see View::themeName(); any other means the default).
    std::string theme;
    // The theme's main color, its accent, as a hue 0 to 1 round the color wheel from red, or below 0 for the theme's
    // own (as to begin with); kept as the theme changes.
    double accentHue = -1.0;
    // How strongly the curves in the editor's graphs glow, 0 (not at all) to 1.
    double glow = kDefaultGlow;
    // How big the depth handles and their "+n" pills are drawn: 0 (the smallest, to begin with) to kMaxHandleSize.
    int handleSize = 0;
    // How strongly the dashed lines from a source's handle to what it modulates are drawn: 0 (not at all) to 1
    // (opaque).
    double linkIntensity = kDefaultLinkIntensity;
    // How strongly the SEQ view's roll draws its grid lines, 0 (none) to 1; kDefaultGridIntensity to begin with, the
    // lines as they were before it could be set.
    double gridIntensity = kDefaultGridIntensity;
    // Whether the SEQ view shows at its full size, over the modulation panel too, or at its half size, over the panels'
    // row alone (see SeqPanel::full()); full to begin with.
    bool seqFull = true;
    // Whether a theme whose cards go without an outline (Spectrum, Porcelain) tints each effect's and source's card with
    // its own color, as it did before cards were plain; off (plain cards) to begin with.
    bool tintedCards = false;

    // Each held to its range, as Controller::setEditorSettings() stores them. A hue below 0 stays as it is: any such
    // is the theme's own (the settings dialog sets -1), and one the user's settings file holds is written back as read.
    EditorSettings clamped() const {
        EditorSettings held = *this;
        held.accentHue = accentHue < 0.0 ? accentHue : std::min(accentHue, 1.0);
        held.glow = std::clamp(glow, 0.0, 1.0);
        held.handleSize = std::clamp(handleSize, 0, kMaxHandleSize);
        held.linkIntensity = std::clamp(linkIntensity, 0.0, 1.0);
        held.gridIntensity = std::clamp(gridIntensity, 0.0, 1.0);
        return held;
    }
};

// One step of the editor's undo history: the params it changed, with their values before and
// after, and the patch's part of the layout (node places, source order, kept routes) if that changed.
struct UndoStep {
    struct Change {
        int index;
        double before;
        double after;
    };
    std::vector<Change> changes; // in the order they were first edited
    bool layoutChanged = false;
    UiLayout before; // only kept when the layout changed
    UiLayout after;
    bool merges = false; // a wheel turn: the next one on the same param joins it
    // The preset the patch came from, and whether it had changed since, before and after the step.
    std::wstring presetBefore, presetAfter;
    bool editedBefore = false, editedAfter = false;
    // For a step that brings the SEQ view's picked notes back (a duplicate): their pattern (-1 for any other step) and
    // the slots picked before and after it.
    int seqPattern = -1;
    std::vector<int> seqPickedBefore, seqPickedAfter;
};

// Where the synth's tuning comes from (see Controller::tuningStatus()): nowhere, standard tuning (12-TET), an MTS-ESP
// master plugin, MTS SysEx the plugin was sent while neither a master nor the patch tunes it, or the patch's own tuning
// (a tuning file loaded into it); and the scale's name, as the master, the SysEx or the file names it.
enum class TuningSource { Standard, Master, SysEx, Patch };
struct TuningStatus {
    TuningSource source = TuningSource::Standard;
    std::string scale;
    bool operator==(const TuningStatus &) const = default;
};

class Controller;

// The editor's undo history: the steps it undoes and redoes, the one being recorded, and the preset the patch is,
// which every step records and brings back. Each Controller has one, which its edits go through
// (Controller::beginEdit() and the rest hand them on); the history reads the Controller's values and layout, and sets
// them through its perform...() hooks. Main thread only.
class UndoHistory {
public:
    explicit UndoHistory(Controller &controller) : controller_(controller) {}
    UndoHistory(const UndoHistory &) = delete;
    UndoHistory &operator=(const UndoHistory &) = delete;

    // Edits from the editor. A drag is wrapped in begin/end so the host records it as one gesture.
    // Each gesture is an undo step, unless a beginStep() around it groups it with others. While a three-band
    // dynamics is linked, an edit of one band's threshold, ratio or gain is made to every band's (see linkedBands()).
    // An edit of a three-band split's crossover that takes it past the other pushes that one along, a third of an
    // octave away, in the same gesture (see crossoverPartner()).
    void beginEdit(int index);
    void setValue(int index, double value);
    void endEdit(int index);

    // Undo and redo. Whatever is edited between the outermost beginStep() and its endStep() is
    // one step, however many gestures it takes. A step that `merges` joins the last one if that
    // merges too and both change just the same param, so a run of wheel turns undoes at once.
    void beginStep(bool merges = false);
    void endStep();
    // Takes every param into the open step: for changes made without setValue(), such as a preset load.
    void recordAll();
    const UndoStep *nextUndo() const { return undo_.empty() ? nullptr : &undo_.back(); }
    const UndoStep *nextRedo() const { return redo_.empty() ? nullptr : &redo_.back(); }
    // The open step brings these notes picked in the SEQ view back as it is undone and redone.
    void keepSeqPicks(int pattern, std::vector<int> before, std::vector<int> after);
    // Not while a step is open. The step undone or redone, if there was one.
    const UndoStep *undo();
    const UndoStep *redo();
    // When the host replaces the whole state, what the history would bring back is gone.
    void clear();
    // How many times the whole patch was replaced (a preset loaded, Init, the host's state), as clear() counts
    // them: the editor fits the SEQ view's roll to the notes of each.
    uint64_t patchLoads() const { return patchLoads_; }

    // The preset the patch is: its file ("" for Init, or for a patch from elsewhere), and whether it changed
    // since. Set while an undo step is open (as loading a preset does), it is what that step lands on, so
    // undoing it brings back the preset before; set outside one (saving, the host's state), no step records it.
    const std::wstring &presetFile() const { return preset_; }
    void setPreset(const std::wstring &file, bool edited = false);
    // Whether the patch changed since a preset was loaded or saved, or since Init: replacing it asks first
    // then, and the preset's name shows it. A patch the host brought counts as changed unless it says otherwise.
    // Undo and redo bring this back, and which preset it was, with the params.
    bool patchEdited() const { return edited_; }

private:
    static constexpr size_t kMaxUndoSteps = 100;
    void record(int index);
    // `index`, and while it is a linked three-band dynamics band's threshold, ratio or gain, the other bands' same
    // field: the params an edit of it sets. -1 for none.
    std::array<int, kMcBands> linkedBands(int index) const;
    // The other crossover of the three-band split whose crossover `index` is, -1 for any other param.
    int crossoverPartner(int index) const;
    // Where the other crossover goes for `index` set to `value`: pushed a third of an octave past it, or as it is.
    double pushedCrossover(int index, double value) const;
    void apply(const UndoStep &step, bool forward);

    Controller &controller_;
    std::vector<UndoStep> undo_; // oldest first
    std::vector<UndoStep> redo_; // the step undone last at the back
    UndoStep open_;
    std::vector<bool> recorded_; // the params open_ holds already
    int depth_ = 0;              // nested beginStep() calls
    bool applying_ = false;      // undo or redo setting values, which records nothing
    bool edited_ = false;        // see patchEdited()
    uint64_t patchLoads_ = 0;    // see patchLoads()
    std::wstring preset_;        // see presetFile()
    bool presetSet_ = false;     // setPreset() was called in the open step
};

// What the editor needs from the plugin: the params and their edits, the telemetry, the layout, the keyboard and the
// scope, the instance's settings and the user's look. The undo history is its own (see UndoHistory), and the presets
// are a PresetStore's (see presetStore()). Main thread only.
class Controller {
public:
    Controller() = default;
    virtual ~Controller() = default;
    Controller(const Controller &) = delete; // its history edits this one
    Controller &operator=(const Controller &) = delete;

    virtual double value(int index) const = 0;
    // A number that moves on whenever a param's value may have changed since, so that the editor, which looks for
    // changes some 60 times a second, need not look over all of them while it stands still. 0, as to begin with: not
    // known, and the editor looks every time.
    virtual uint64_t valuesStamp() const { return 0; }
    // Edits from the editor, and undo and redo, as the undo history keeps them (see UndoHistory::beginEdit() and
    // UndoHistory::beginStep()).
    void beginEdit(int index) { history_.beginEdit(index); }
    void setValue(int index, double value) { history_.setValue(index, value); }
    void endEdit(int index) { history_.endEdit(index); }
    void beginStep(bool merges = false) { history_.beginStep(merges); }
    void endStep() { history_.endStep(); }
    void recordAll() { history_.recordAll(); }
    const UndoStep *nextUndo() const { return history_.nextUndo(); }
    const UndoStep *nextRedo() const { return history_.nextRedo(); }
    void keepSeqPicks(int pattern, std::vector<int> before, std::vector<int> after) {
        history_.keepSeqPicks(pattern, std::move(before), std::move(after));
    }
    const UndoStep *undo() { return history_.undo(); }
    const UndoStep *redo() { return history_.redo(); }
    void clearHistory() { history_.clear(); }
    uint64_t patchLoads() const { return history_.patchLoads(); }

    virtual void telemetry(Telemetry &out) const = 0;
    virtual UiLayout &layout() = 0;
    virtual void layoutChanged() = 0;
    // The editor's window would be this size (physical pixels): the plugin asks its host to resize it (see
    // Editor::followDpi()). A host that won't leaves it as it is.
    virtual void requestEditorSize(uint32_t, uint32_t) {}
    // The user collapsed or opened the keyboard panel (UiLayout::keysCollapsed, set already): the plugin keeps it in the
    // user's settings, so that a new instance opens with the panel as it was left last.
    virtual void keysCollapsedSet(bool) {}

    // The virtual keyboard: a MIDI note number (60 is middle C) at a MIDI velocity (0-127; a note off ignores it).
    // (Its bend wheel is the Pitch Bend param, edited.)
    virtual void playNote(int note, bool on, int velocity) = 0;
    // A note the SEQ view plays to be heard (a key or a note clicked in its roll): that note alone, never the
    // sequencer's pattern, as the keyboard's would in Key or Latch. At velocity 100.
    virtual void auditionNote(int note, bool on) { playNote(note, on, 100); }
    // The notes heard so, bent `semitones` from their pitch (a note bend's point dragged in the SEQ view), until the next
    // is heard. A Controller that can't bend them plays them unbent.
    virtual void auditionBend(double) {}
    // The SEQ view's Play button: the sequencer plays its pattern from the loop's start, or from the pattern's step
    // `from` (the first note picked in the roll; -1 for none), as written but for Octave and Transpose, whatever Play
    // says, until turned off (the keys playing notes meanwhile, as without it). The telemetry's seqPreview says whether
    // it plays: the sequencer switched off or the plugin reset stops it.
    virtual void playSeqPreview(bool, double /*from*/ = -1.0) {}
    // The operator whose output the scope shows (-1 while no scope is shown, so the audio thread captures nothing),
    // over how many of its cycles.
    virtual void setScope(int op, int cycles) = 0;
    // Whether the editor shows a free or anchored LFO running with no note playing, so the plugin keeps its clock
    // going (and the telemetry coming) while a host would otherwise let it sleep.
    virtual void setIdleLfosShown(bool) {}

    // The preset library and the patch as a file. A Controller without files offers one that has none.
    virtual PresetStore &presetStore();
    // Whether the patch is a preset file (not Init), and whether it changed since (see UndoHistory::patchEdited()).
    bool presetHasFile() const { return !history_.presetFile().empty(); }
    bool patchEdited() const { return history_.patchEdited(); }
    // Whether the current preset is a factory preset, built in (its info can't be changed, nor its file shown).
    bool presetFactory() { return presetHasFile() && !presetStore().presetWritable(); }
    // Writes the patch over its preset's file, as Save does.
    bool overwritePreset() {
        const std::wstring file = history_.presetFile(); // savePreset() sets the preset again
        return presetStore().presetWritable() && presetStore().savePreset(file);
    }

    // Settings of this instance, not of the patch: saved with the project, left
    // alone by presets. Oversampling is 1, 2, 4 or 8.
    virtual int oversampling() const = 0;
    virtual void setOversampling(int factor) = 0;
    // MPE over MIDI (see Synth::setMpe()), and how far its member channels bend, in semitones (1 to 96).
    virtual bool mpe() const = 0;
    virtual void setMpe(bool on) = 0;
    virtual int mpeBendRange() const = 0;
    virtual void setMpeBendRange(int semitones) = 0;
    // MTS-ESP microtuning: whether the synth plays in the tuning an MTS-ESP master gives it (or MTS SysEx, while no
    // master is connected), and whether a held note follows the tuning as it changes or keeps the one it started in.
    // Off, and continuous, to begin with.
    // A host without MTS-ESP (the browser demo) offers none: the settings' TUNING shows it, its buttons disabled.
    virtual bool offersTuning() const { return false; }
    virtual bool mtsOn() const { return false; }
    virtual void setMtsOn(bool) {}
    virtual bool mtsContinuous() const { return true; }
    virtual void setMtsContinuous(bool) {}
    // Where the synth's tuning comes from now, and its scale's name (see TuningStatus).
    virtual TuningStatus tuningStatus() const { return {}; }
    // A tuning of the patch's own, from a file, which presets keep (see tuningedits.h): opening the file dialog for a
    // scale or tuning (.scl, .ascl, .tun) or, `mapping`, a keyboard mapping (.kbm) over the patch's scale. A host
    // without files (the browser demo) tells the user so instead.
    virtual void chooseTuning(bool mapping) { (void)mapping; }
    // The file the dialog picked loaded into the patch, as its extension says what it holds (one undo step): nothing
    // on success, else what to tell the user.
    virtual std::optional<std::string> loadTuningFile(const std::wstring &path) {
        (void)path;
        return std::nullopt;
    }
    // The editor's look (see EditorSettings). Unlike the settings above it is the user's: every FMVibe editor uses it,
    // whichever project it is in. Set, it is held to its ranges first, then stored.
    virtual EditorSettings editorSettings() const = 0;
    void setEditorSettings(const EditorSettings &settings) { storeEditorSettings(settings.clamped()); }
    // The host's tempo, for a host that stands in for a transport and lets the plugin set it (the browser demo, and
    // fmvibe-uihost through FMVibe's own host extension, see src/hosttempo.h). There the synth always plays its own
    // tempo, which the host's follows, and the title bar shows no H. A plugin host's tempo is its own, which the title
    // bar shows as the synth last played it while the synth follows it. In BPM, kMinTempo to kMaxTempo, in hundredths
    // (see clampTempo()).
    virtual bool hasTempo() const { return false; }
    virtual double tempo() const { return kDefaultTempo; }
    virtual void setTempo(double) {}
    // Whether the synth plays at the host's tempo (the title bar's H, on to begin with; never in a host that lets the
    // editor set its tempo), and the tempo of its own it plays at while it doesn't, which the title bar's tempo drags,
    // scrolls and types. Settings of the instance, saved with the host's project, which an edit marks changed but
    // doesn't undo; a preset loaded sets the tempo of its own to the one it was saved at. In BPM, as above.
    virtual bool tempoSync() const { return true; }
    virtual void setTempoSync(bool) {}
    virtual double ownTempo() const { return kDefaultTempo; }
    virtual void setOwnTempo(double) {}
    // Opens the user manual, online at kManualUrl (the settings dialog's book button).
    virtual void openManual() {}

protected:
    // What beginEdit(), setValue() and endEdit() do besides keeping the history: undo and redo
    // set values through these too.
    virtual void performBeginEdit(int index) = 0;
    virtual void performSetValue(int index, double value) = 0;
    virtual void performEndEdit(int index) = 0;
    // Around an undo or redo step's edits, which may be thousands: what the plugin tells the host once for all of
    // them (that the project changed) it can tell at the end.
    virtual void performStepBegin() {}
    virtual void performStepEnd() {}
    // What setEditorSettings() keeps, held to its ranges already: the plugin writes it into the user's settings.
    virtual void storeEditorSettings(const EditorSettings &settings) = 0;
    // The preset the patch is, as the undo history keeps it (see UndoHistory::presetFile()).
    const std::wstring &presetFile() const { return history_.presetFile(); }
    void setPreset(const std::wstring &file, bool edited = false) { history_.setPreset(file, edited); }

private:
    friend class UndoHistory; // which edits through the perform...() hooks
    UndoHistory history_{*this};
};

// Edits made while one of these lives are one undo step.
class UndoScope {
public:
    explicit UndoScope(Controller &controller, bool merges = false) : controller_(controller) {
        controller_.beginStep(merges);
    }
    ~UndoScope() { controller_.endStep(); }
    UndoScope(const UndoScope &) = delete;
    UndoScope &operator=(const UndoScope &) = delete;

private:
    Controller &controller_;
};

} // namespace fmvibe::gui
