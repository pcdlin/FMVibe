#pragma once

#include "../params.h"
#include "../synth.h"
#include "../uistrings.h"

#include <array>
#include <string>
#include <vector>

namespace fmvibe::gui {

// Nodes of the routing graph: the operators, then the filters, then the shapers.
constexpr int kNodes = kOps + kFilters + kShapers;

// What the routing panel shows: the graph, the matrix (both edit the same
// routing), the effects chain or every modulation (MODS). Saved by number: only append.
enum class RoutingView { Graph, Matrix, Fx, Mods };

// What the lower half of an operator's page shows, one for every operator. Saved by number, so the tabs' order on
// the page (Envelope, Unison, Inputs, Scope) is layout.h's opTab(). A noise operator shows
// its spectrum in Unison's place, and its envelope in Scope's.
// Stored in the layout by number: only append.
enum class InspectorTab { Unison, Envelope, Scope, Inputs, Count };

// How many of the operator's cycles the scope shows across.
constexpr int kScopeCycleChoices[] = {1, 2, 4};

constexpr std::array<int, kMaxSources> identitySourceOrder() {
    std::array<int, kMaxSources> order{};
    for (int i = 0; i < kMaxSources; ++i)
        order[i] = i;
    return order;
}

// Editor state that isn't a param but is saved with the patch.
struct UiLayout {
    // Node positions in the routing graph, 0..1 within the graph area: the operators' circles, and the boxes of the
    // filters and then the shapers. Default: A in the middle, B, C and D in a row above it, E and F beside A, the
    // filters below, a shaper at each end of them.
    std::array<float, kOps> nodeX{0.5f, 0.2f, 0.5f, 0.8f, 0.2f, 0.8f};
    std::array<float, kOps> nodeY{0.55f, 0.1f, 0.1f, 0.1f, 0.55f, 0.55f};
    std::array<float, kFilters + kShapers> boxX{0.35f, 0.65f, 0.08f, 0.92f};
    std::array<float, kFilters + kShapers> boxY{1.0f, 1.0f, 1.0f, 1.0f};
    int selectedNode = 0;    // shown in the inspector panel
    // The order the editor shows the sources in: every place once, empty ones included.
    std::array<int, kMaxSources> sourceOrder = identitySourceOrder();
    RoutingView view = RoutingView::Graph;
    float zoom = 1.0f;       // editor size relative to the default, before DPI scaling
    bool sourcesCollapsed = false; // the modulation panel shows only its title row; the editor is shorter
    bool keysCollapsed = false;    // the keyboard panel shows only its top row, without the wheels and keys
    // Routes (param indices) that keep their wire at 0 %, so modulation can still be dropped on
    // them. A route joins once it is on, and leaves only when it is disconnected.
    std::vector<int> keptRoutes;
    // How many lines the shape editor's grid has across and up and down; Ctrl snaps to them.
    // Like the zoom, it belongs to the editor, not the patch: presets leave it alone.
    int shapeGridX = 16;
    int shapeGridY = 8;
    // The macros' names, empty for "Macro 1" and so on: part of the patch, like the node places.
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
};

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
// numbered by type in the order the sources show, a macro's name, "Note" or "Mod Wheel".
template <class Get>
std::string sourceName(const UiLayout &layout, const Get &get, int source) {
    if (isNote(source))
        return tr(Str::SourceNameNote);
    if (isWheel(source))
        return tr(Str::SourceNameWheel);
    if (isVelocity(source))
        return tr(Str::SourceNameVelocity);
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
                                                    : Str::SourceNameLfo;
    return trf(name, {std::to_string(number)});
}

// A preset in the library: a .fmv file in the preset folder, or in one of its subfolders.
struct PresetEntry {
    std::string name;     // the file's name without .fmv
    std::string category; // the subfolder it is in, "" at the top
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
};

// What the editor needs from the plugin. Main thread only.
class Controller {
public:
    virtual ~Controller() = default;

    virtual double value(int index) const = 0;
    // Edits from the editor. A drag is wrapped in begin/end so the host records it as one gesture.
    // Each gesture is an undo step, unless a beginStep() around it groups it with others. While a three-band
    // dynamics is linked, an edit of one band's threshold, ratio or gain is made to every band's (see linkedBands()).
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
    // Not while a step is open.
    void undo();
    void redo();
    // When the host replaces the whole state, what the history would bring back is gone.
    void clearHistory();

    virtual void telemetry(Telemetry &out) const = 0;
    virtual UiLayout &layout() = 0;
    virtual void layoutChanged() = 0;

    // The virtual keyboard: a MIDI note number (60 is middle C), and the bend wheel, -1..1.
    virtual void playNote(int note, bool on) = 0;
    virtual void pitchWheel(double amount) = 0;
    // The operator whose output the scope shows (-1 while no scope is shown, so the audio thread captures nothing),
    // over how many of its cycles.
    virtual void setScope(int op, int cycles) = 0;

    // Presets: the whole patch, as a file. Init sets every param and the layout back to their defaults.
    virtual void resetPreset() = 0;
    virtual bool loadPreset(const std::wstring &path) = 0;
    virtual bool savePreset(const std::wstring &path) = 0;
    // Asks the host window for a file dialog; the result comes back through load/savePreset.
    virtual void choosePreset(bool save) = 0;
    // Whether presets are files that can be written (not in the browser demo, whose dialog only says so).
    virtual bool writesPresetFiles() const { return true; }
    // Whether the patch is a preset file (not Init), and writing the patch over that file, as Save does.
    bool presetHasFile() const { return !preset_.empty(); }
    bool overwritePreset() {
        const std::wstring file = preset_; // savePreset() sets preset_ again
        return !file.empty() && savePreset(file);
    }

    // The preset library: the presets in the preset folder (Documents\FMVibe\presets), by subfolder and
    // then by name, as the last scan found them. The first scan makes the folder, with a few examples.
    virtual void rescanPresets() = 0;
    virtual const std::vector<PresetEntry> &presets() const = 0;
    // The entry loaded or saved last, or -1 after Init or for a file from elsewhere.
    virtual int presetIndex() const = 0;
    // "- Init -", or the name of the preset loaded or saved last.
    virtual std::string presetName() const = 0;
    virtual bool loadPresetAt(int index) = 0;
    virtual std::wstring presetFolder() const = 0;
    virtual void openPresetFolder() = 0;
    // Whether the patch changed since a preset was loaded or saved, or since Init: replacing it asks first
    // then, and the preset's name shows it. A patch the host brought counts as changed unless it says otherwise.
    // Undo and redo bring this back, and which preset it was, with the params.
    bool patchEdited() const { return edited_; }

    // Settings of this instance, not of the patch: saved with the project, left
    // alone by presets. Oversampling is 1, 2, 4 or 8.
    virtual int oversampling() const = 0;
    virtual void setOversampling(int factor) = 0;
    // MPE over MIDI (see Synth::setMpe()), and how far its member channels bend, in semitones (1 to 96).
    virtual bool mpe() const = 0;
    virtual void setMpe(bool on) = 0;
    virtual int mpeBendRange() const = 0;
    virtual void setMpeBendRange(int semitones) = 0;
    // The editor's color theme, by name (see View::themeName(); any other means the default). Unlike the
    // settings above it is the user's: every FMVibe editor uses it, whichever project it is in.
    virtual std::string theme() const = 0;
    virtual void setTheme(const std::string &name) = 0;
    // How strongly the curves in the editor's graphs glow, 0 (not at all) to 1; the user's too.
    virtual double glow() const = 0;
    virtual void setGlow(double amount) = 0;
    // How big the depth handles and their "+n" pills are drawn: 0 (the smallest, to begin with), 1 or 2; the user's
    // too.
    virtual int handleSize() const = 0;
    virtual void setHandleSize(int size) = 0;
    // How strongly the dashed lines from a source's handle to what it modulates are drawn: 0 (not at all) to 1
    // (opaque), 0.75 to begin with; the user's too.
    virtual double linkIntensity() const = 0;
    virtual void setLinkIntensity(double amount) = 0;

protected:
    // What beginEdit(), setValue() and endEdit() do besides keeping the history: undo and redo
    // set values through these too.
    virtual void performBeginEdit(int index) = 0;
    virtual void performSetValue(int index, double value) = 0;
    virtual void performEndEdit(int index) = 0;
    // The preset the patch is: its file ("" for Init, or for a patch from elsewhere), and whether it changed
    // since. Set while an undo step is open (as loading a preset does), it is what that step lands on, so
    // undoing it brings back the preset before; set outside one (saving, the host's state), no step records it.
    const std::wstring &presetFile() const { return preset_; }
    void setPreset(const std::wstring &file, bool edited = false);

private:
    static constexpr size_t kMaxUndoSteps = 100;
    void record(int index);
    // `index`, and while it is a linked three-band dynamics band's threshold, ratio or gain, the other bands' same
    // field: the params an edit of it sets. -1 for none.
    std::array<int, kMcBands> linkedBands(int index) const;
    void apply(const UndoStep &step, bool forward);

    std::vector<UndoStep> undo_; // oldest first
    std::vector<UndoStep> redo_; // the step undone last at the back
    UndoStep open_;
    std::vector<bool> recorded_; // the params open_ holds already
    int depth_ = 0;              // nested beginStep() calls
    bool applying_ = false;      // undo or redo setting values, which records nothing
    bool edited_ = false;        // see patchEdited()
    std::wstring preset_;        // see presetFile()
    bool presetSet_ = false;     // setPreset() was called in the open step
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
