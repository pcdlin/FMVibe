#pragma once

// What the editor is made of for input: its widgets (each a kind and where it is, with a param and two ints whose
// meaning the kind gives), the mouse's modifiers and cursors, the keys the window sends, the entries of a context menu
// and the text fields' editing. Shared by the View and its panels (see panel.h).

#include "canvas.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmvibe::gui {

struct Modifiers {
    bool shift = false; // fine adjustment
    bool ctrl = false;
    bool alt = false;
};

enum class CursorKind { Arrow, Hand, Vertical, Horizontal, Move, Text, Pen }; // Pen: drawing notes in the SEQ view's roll

struct MenuItem {
    int command = 0; // 0 is a separator
    std::string label;
    bool checked = false; // shown with a check mark: the choice in force
};

// The keys the window sends while typing goes on or a dialog is open (see View::typing() and View::dialogKey()).
enum class Key { Enter, Escape, Backspace, Delete, Left, Right, Home, End, SelectAll, Up, Down, Copy, Cut, Paste, Tab, BackTab, Duplicate };

// What a widget is, which says what its param, a and b mean.
enum class WidgetKind {
    Knob,         // param
    Choice,       // param, b = the value it selects (segmented buttons)
    Toggle,       // param: flips between 0 and 1
    ViewTab,      // a = RoutingView: picks what the panels under the tabs show
    AlgorithmButton, // ALGORITHM, at the tabs' row's right, with ROUTING and MATRIX: opens the panel of algorithms
    AlgorithmMenu, // a = 1 on the open panel of algorithms, 0 around it (a click there closes it)
    AlgorithmSet, // b = 6, 4, 3 or 2: the panel's switch between the sets, by how many operators they use
    AlgorithmItem, // a = index in kAlgorithms: a thumbnail of the panel; a click sets the operators to it
    OpTab,        // a = InspectorTab: picks what the lower half of an operator's page shows
    ScopeCycles,  // b = how many cycles the scope shows

    NodeTab,      // a = node: selects it; the selected one switches it on and off
    MatrixLabel,  // a = node: a matrix row's or column's name; a click does what the node's tab does
    MacroName,    // a = macro: the name under its knob; double-click renames it
    NoteDisplay,  // a = kNoteSource or kVelocitySource: its display in the keyboard strip; a click opens its curve
    FxFold,       // a = effect slot: folds its card to a strip, or unfolds it
    SourceFold,   // a = source place: the same for its card
    MacroHandle,  // a = kMaxSources + macro, kNoteSource, kWheelSource, kVelocitySource, kBendSource or kOpSource + op (an operator's letter on its page): drag onto a target, as a
                  // source's handle
    WaveButton,   // a = op, b = wave: one of the common waves
    WaveMore,     // a = op: "..." opens the list of the other waves
    WaveMenu,     // a = 1 on the open list, 0 around it (a click there closes it)
    WaveMenuItem, // a = op, b = wave: an entry of the list
    Node,         // a = node (operators, then filters)
    NodeGhost,    // a = node that is off: a dashed button in the graph's top left that switches it on
    NodeDelete,   // a = node: the x in a circle at a node's upper right, while the mouse is over it: switches it off
    Port,         // a = node: its edge, a band along its outline; drag from it to connect
    WireLabel,    // param = route amount, a = from node, b = to node
    OutLabel,     // param = output level, a = node
    WireEnd,      // param = the route, a = from node, b = to node (kNodes: the output): its arrowhead, to drag elsewhere
    WireStart,    // param = the route, a = from node, b = to node (kNodes: the output): where it leaves its node, to
                  // drag onto another node, which it then comes from
    Wire,         // param = the route, a = from node, b = to node: along its curve; drag to bend it
    LabelMode,    // param = the route's mode, a = from node, b = to operator: the left part of a wire label's pill
                  // (see RoutingPanel::labelPill()), its mode's glyph; a click opens the list of modes, the wheel steps them
    LabelRemove,  // param = the route, a = from node, b = to node (kNodes: the output): the x at the right of a
                  // wire or OUT label's pill; a click removes the route with its modulation
    GridCell,     // param, a = row node, b = column
    SourceHandle, // a = source: drag onto a param to modulate it
    SourceGrip,   // a = source, b = its position among the sources: drag to reorder
    SourceMenuButton, // + SOURCE: opens the list of source types under it
    SourceRemove, // a = source
    SourcesToggle, // collapses the modulation panel to its title row, or opens it again
    KeysToggle,    // collapses the keyboard panel to its top row, or opens it again
    SlotBar,      // param = slot amount, a = source, b = slot
    SlotRemove,   // a = source, b = slot
    SlotBypass,   // param = the slot's bypass, a = source, b = slot: the dot at the row's start
    SourcePolarity, // a = source: switches it between unipolar and bipolar, for every modulation it has (the
                    // switch after a MODS row's source)
    ModsSource,   // a = source, b = slot: a MODS row's source; a click opens the menu that moves it to another
                  // (an empty modulation's: a = its source or -1, b = emptyModSlot() of it)
    ModsTarget,   // a = source, b = slot: a MODS row's target; a click opens the menu of targets
    ModsAdd,      // the row after the MODS view's last: adds an empty modulation
    ModsScale,    // param = the slot's Amount, a = source, b = slot: a MODS row's Amount bar, dragged across
    ModsCurve,    // param = the slot's curve, a = source, b = slot: a MODS row's curve, dragged up or down
    ModsAux,      // a = source, b = slot: a MODS row's aux, or + Aux; a click opens the menu of auxes
    ModsAuxClear, // a = source, b = slot: the x on a MODS row's aux
    ModsAuxDepth, // a = source, b = slot: the aux depth on an aux's chip, dragged up and down
    ModsScroll,   // the MODS view's scrollbar: drag the thumb, or click the track to jump there
    InputsScroll, // the operator page's INPUTS tab's scrollbar, as the MODS view's
    ModListScroll, // a pill's open list's scrollbar, as the MODS view's
    CascadeMenu, // a = 1 on a level of the open cascading menu (b = the level), 0 around it (a click closes it)
    CascadeItem,  // a = level, b = entry: an entry of the open cascading menu
    SourceMods,   // a = source: the button on its card that shows its modulations in the MODS view
    ValueText,    // param, a = source, b = slot: a slot's depth in numbers; double-click types one
    EnvPoint,     // a = envelope (ops, then kOps + source), b = point
    EnvCurve,     // param = curve, a = envelope, b = segment (attack, decay, release): bends it
    FilterPoint,  // a = filter (b = 1: the filter effect in slot a): cutoff and resonance handle
    ModDot,       // param = slot amount, a = source, b = slot: depth handle on a knob
    ModPill,      // a = the target param, b = how many depth handles show before it: "+n" in place of the handles
                  // that don't fit; a click opens the list of the target's modulations
    ModList,      // a = the target param: the open list of every modulation of the target, under its pill
    ModListRow,   // a = source, b = slot: a row of the list; a source's handle dropped on it sets the row's aux
    FxGrip,       // a = effect slot, b = its position in the chain: drag to reorder
    FxPower,      // param = its switch, a = effect slot, b = position: the icon; a click switches it, a drag moves it
    FxMenuButton, // ADD EFFECT: opens the list of effect types
    FxShadow,     // the dashed card after the effects: opens the list of effect types over itself
    FxInsert,     // a = lane, b = item·kFxGapStride + place (see FxSlotAt): an arrow before a card in the row (the
                  // first too, and along a container's band), which opens the list to add an effect there
    SourceShadow, // the dashed card after the sources: opens the list of source types over itself
    SourceMenu,   // a = 1 on the open list of source types, 0 around it (a click there closes it)
    SourceMenuItem, // a = SourceType: an entry of the list, adds a source of that type
    FxMenu,       // a = 1 on the open list, 0 around it (a click there closes it)
    FxAdd,        // a = FxType: an entry of the list, puts that effect at the end of the chain
    FxRemove,     // a = effect slot
    Scrollbar,    // a = ScrollArea: drag the thumb, or click the track to jump there
    DynBand,      // a = effect slot, b = band: a three-band dynamics' band's row; a click picks the band its
                  // knobs set
    DynZone,      // param = the ratio, a = effect slot, b = 2 x band + 0 for its downward zone (right of the Above
                  // line), 1 for its upward one (left of the Below line): drag up and down for the ratio. Linked,
                  // every band's zones set the low band's
    DynGrip,      // param = the threshold's ratio (for Alt and the wheel), a = effect slot, b = 2 x band + 0 for
                  // its Above threshold's line, 1 for its Below one's: drag across for the threshold. Linked,
                  // every band's lines set the low band's
    PeqPoint,     // param = the band's Q (for the wheel), a = effect slot, b = band: frequency and gain handle;
                  // a press picks the band too, and switches it on if it is off
    PeqBand,      // a = effect slot, b = band: a parametric EQ band's chip; a click picks the band its knobs set
    PeqBandOn,    // param = the band's On switch, a = effect slot, b = band: its chip's power symbol and number,
                  // which switch it on and off, and pick it
    LfoDisplay,   // a = lfo: click opens the shape editor of a freeform LFO
    ClipDisplay,  // a = effect slot: the edit button on a drawn curve's display, which opens the big editor
    ListButton,   // param = a choice (an LFO's shape, the shaper's curve, the reverb's algorithm): opens its list
    ListMenu,     // a = 1 on the open list, 0 around it (a click there closes it)
    ListItem,     // param, b = the value it picks: an entry of the list
    ListStep,     // param = a list's choice (a filter's type, a shaper's curve, an amp's model or cabinet), b = -1
                  // or 1: an arrow in its button that steps to the entry before or after, round the list's ends
    PeqTypeButton, // param = the band's type, a = effect slot, b = band: the shape on its chip, which opens the
                   // list of band shapes
    PeqTypeMenu,  // a = 1 on the open list, 0 around it (a click there closes it)
    PeqTypeItem,  // param = the band's type, b = PeqType: an entry of the list
    AmpMic,       // param = the mic's Position, a = effect slot: the mic on an amp's cabinet display, which drags
                  // across the cone (Position) and away from it (Distance)
    UtilityHandle, // param = the knob it sets, a = effect slot, b = which (see kUtilityHandleParams): a handle on
                   // the utility's display
    ShapeArea,    // a = shape (see shapeEditor_): double-click adds a point
    ShapePoint,   // a = shape, b = point
    ShapeCurve,   // param = curve, a = shape, b = point: bends the segment leaving the point
    PartialArea,  // a = operator, b = 0 for the levels, 1 for the phases: drag across to draw the partials
    PartialReset, // a = operator: back to a plain sine
    PartialRandom, // a = operator: random levels on a pink slope, and random phases
    PartialEdit,  // a = operator: the edit button left of a Custom wave's keyboard switch; opens its partial
                  // editor, or closes it
    ShapeDone,    // closes the shape editor
    ShapeReset,   // a = shape: back to the shape it starts with
    ShapeGrid,    // a = axis (0 across, 1 up and down), b = -1 or 1 steps the grid, 0 is its count (wheel)
    ShapeCollapse, // a = lfo: its widened card back to a normal one
    ShapeExpand,  // a = lfo: opens its shape in the full-size editor, over the panels
    ShapeWiden,   // a = lfo: a Custom LFO's normal card widened to have its shape, as a click on its display does
    ShapeTool,    // a = lfo, b = ShapeTool: a button in its widened card that changes the whole shape
    ShapeMode,    // a = lfo, b = 0 Points, 1 Steps (what a click on its shape does), 2 Snap (to the grid)
    UndoButton,   // a = 0 undoes, 1 redoes
    Key,          // a = key of the keyboard (0 = the lowest): drag across keys to play them
    Octave,       // a = -1 or 1: shifts the keyboard down or up an octave
    PresetButton, // a = PresetAction
    PresetStep,   // a = -1 or 1: the previous or next preset in the library
    PresetNameBox, // the current preset's name: opens the preset browser
    TempoBox,     // the host's tempo (see Controller::hasTempo()): drag up and down, scroll, or double-click to type
    PresetInfoButton, // the (i) in the preset bar: opens the preset's info card
    InfoCard,     // a = 1 on the open info card, 0 around it (a click there closes it)
    InfoAction,   // a = 0 the star (a favorite), 1 Show in Browser, 2 Save As or Edit Info (see infoCardWidgets())
    BrowserBackdrop, // a = 1 on the browser itself, 0 around it (a click there closes it)
    BrowserRow,   // a = preset index: picks it, a double-click loads it
    BrowserFav,   // a = preset index: its star, a favorite or not
    BrowserSubfolders, // the switch: a folder lists its subfolders' presets too
    BrowserLibrary, // a = BrowserScope: All, Favorites or Recent, atop the sidebar
    BrowserTree,  // a = row of browserTree(): shows that folder
    BrowserTreeToggle, // a = row of browserTree(): opens or closes it in the tree
    BrowserCategory, // a = row of browserCategoryList(): filters by it, or not
    BrowserCategoryRemove, // a = row of browserCategoryList(), one of the user's own: removes it, asking first
    BrowserCatClear, // takes the category filters off
    BrowserNew,   // the + by FOLDERS: opens the New Folder form
    BrowserCrumb, // a = how many of the folder's parts: shows that folder
    BrowserChip,  // a = row of browserCategoryList(): a filter by it, whose click takes it off
    BrowserClose, // closes the browser
    BrowserSideScroll, // the sidebar's scrollbar, at its right edge: drag the thumb, or click the track
    BrowserShowFile, // shows the file of the preset picked in Explorer
    BrowserSaveHere, // while a folder of the user's is shown: the Save dialog, on that folder
    SaveBackdrop, // a = 1 on the Save dialog itself, 0 around it (a click there closes it)
    SaveCategory, // a = row of saveCategoryList(), or -1 for None: the preset's category
    SaveNewCategory, // + New: opens the New Category form
    SaveFolderRow, // a = row of the folders (0 the preset folder, then presetFolders() + 1): where it goes
    SaveNewFolder, // New Folder: opens its form
    SaveToFile,   // Save to File...: the file dialog, which puts it anywhere
    SaveButton,   // a = 1 saves (or replaces), 0 cancels
    SaveClose,
    NameMenu,     // a = 1 on the New Category or New Folder form, 0 around it (a click there closes it)
    NameColor,    // a = color: the new category's
    NameButton,   // a = 1 adds the category or makes the folder, 0 cancels
    SearchField,  // a = Field: a text field (the browser's search, the MODS view's filter, the target menu's
                  // search): click to put the caret there, drag to select, double-click for a word
    SearchClear,  // a = Field: the x in a text field: empties it
    SettingsButton, // the cog next to the preset buttons: opens the settings dialog
    ConfirmBackdrop, // a = 1 on the dialog asking before Init, Load or Save, 0 around it (a click there cancels)
    ConfirmButton,  // a = 1 goes ahead, 0 cancels
    SettingsBackdrop, // a = 1 on the dialog itself, 0 around it (a click there closes it)
    SettingsOption, // b = the oversampling factor it picks
    SettingsMpe,    // b = 1 switches MPE on, 0 off
    SettingsMpeRange, // b = the MPE bend range it picks, in semitones
    SettingsTheme,  // b = the theme it picks
    SettingsHandles, // b = the depth handles' size it picks
    SettingsGlow,   // the slider for how strongly the graphs' curves glow
    SettingsHue,    // the slider for the theme's main color
    SettingsLinks,  // the slider for how strongly the sources' dashed lines are drawn
    SettingsGrid,   // the slider for how strongly the SEQ view's roll draws its grid
    SettingsClose,
    ModWheel,     // param = mod wheel: drag or scroll
    BendWheel,    // drag; springs back to center on release
    // The effects graph (see fxpanel.cpp).
    FxMode,       // b = 0 the chain, 1 the graph: the switch at the FX panel's top right
    FxNode,       // a = node: a click shows it in the inspector
    FxSlot,       // a = lane (0 the main line, 1 the high band, 2 the low band, 3 the middle band): a +, which opens
                  // the list of effects
    FxGap,        // a = lane, b = item·kFxGapStride + place (see FxSlotAt): a wire's middle, which opens it too
    FxAddNode,    // b = its bands, 2 or 3: the list's entries after the effects, a split with its join, opened from
                  // the main line
    FxNodeRemove, // a = node: the x on a split's controls, its container's in the row or its card in the graph
    FxNodeDelete, // a = node: the x in a circle at a node's upper right, while the mouse is over the node
    FxNodePower,  // a = node, param = its Enabled: the power symbol in a circle at an effect's node's upper left,
                  // while the mouse is over the node
    FxWire,       // a = lane, b = item·kFxGapStride + place: along a wire that takes a node at its middle, its +
    FxClear,      // the button at the FX panel's top right that removes every effect, asking first
    SplitPoint,   // param = the crossover, a = split, b = 0 the low one, 1 the high one: its handle on the display
    // A split's controls (see splitControlWidgets()), in its container in the row and on its card in the graph.
    SplitBand,    // a = split, b = band (0 low, 1 high, 2 middle; see FxGraphModel::splitLane()): its tab, which
                  // shows that band
    SplitBands,   // a = split, b = 2 or 3: the BANDS switch's half that splits it in that many bands
    SplitArea,    // a = split, b = band (as SplitBand's): the display between its band's crossovers, which shows
                  // that band
    SplitSolo,    // param = a join's solo, a = split, b = lane: the S under its band's level, each band's on its own
    FxBoxGrip,    // a = split's node: its container's title row in the row, which drags it along the line
    FxBoxFold,    // a = split: folds its container to a strip, or unfolds it
    FxBoxAdd,     // a = lane (1 the high band, 2 the low band, 3 the middle band), b = item: a container's dashed
                  // card, for the list
    // The SEQ view (see seqpanel.cpp).
    SeqTool,      // b = 0 select, 1 draw: what a click on the roll does
    SeqSnap,      // the snap's button: opens its list
    SeqSnapMenu,  // a = 1 on the open list, 0 around it (a click there closes it)
    SeqSnapItem,  // b = the snap it picks (see kSeqSnaps)
    SeqMacroTab,  // the lane's tab of the macros' curves: shows the macro last shown and opens their list
    SeqMacroMenu, // a = 1 on the open list, 0 around it (a click there closes it)
    SeqMacroItem, // b = the macro whose curve it shows
    SeqStretch,   // b = 2 twice as long, -2 half as long: the notes picked, or the whole pattern
    SeqField,     // b = 0 pitch, 1 velocity, 2 chance, 3 length, 4 offset: a value of the notes picked, dragged
    SeqFlag,      // b = 0 Slide, 1 Mute: switches it for the notes picked
    SeqClear,     // removes every note of the pattern, asking first
    SeqCopy,      // copies the pattern into the next empty one
    SeqRoot,      // param = Root: dragged up and down, or scrolled, a semitone a step
    SeqFold,      // shows only the scale's rows, or every row
    SeqRuler,     // the bars over the roll: drag the loop's ends, or across it for a new loop
    SeqKeys,      // the piano keys at the roll's left: a click plays the note
    SeqGrid,      // the roll itself: the notes on it are picked, moved, stretched, drawn and deleted here
    SeqLane,      // the lane under the roll: drag across the notes to set its value
    SeqLaneTab,   // b = SeqLane: picks what the lane shows
    SeqZoom,      // b = 0 zooms out across, 1 in across, 2 out up and down, 3 in up and down
    SeqScroll,    // the bar under the roll while it is zoomed in across: drag the thumb, or click the track there
    SeqScrollY,   // the bar at the roll's right, for its rows: drag the thumb, or click the track there
    // A Wavetable or Sample operator's, on the inspector's page (see inspectorpanel.cpp's source page).
    OpSourceList, // a = op: the list button on the page's title row that picks its source
    OpSourceMenu, // the open source list: a = 0 around it (a click closes it), 1 its panel
    OpSourceItem, // a = op, b = 0 a wave, 1 a wavetable, 2 a sample: an entry of the open source list
    TableStep,    // a = op, b = -1 or 1: steps to the table before or after the one played, in its source
    TablePicker,  // a = op: the table's name; opens the table browser
    SamplePicker, // a = op: the sample's name; opens the table browser, picking a sample
    DataImport,   // a = op, b = DataKind: the import button, and the dashed card of a Sample operator with none
    SampleHandle, // param, a = op, b = which (0 Start, 1 Loop Start, 2 Loop End, 3 Loop Fade): dragged across
    SampleRoot,   // param = its Root, a = op: the ROOT readout, dragged up and down or typed
    // The table browser (see tablebrowser.cpp), over the routing panel.
    TbSource,     // a = source (-2 Factory, -1 In this patch, else the library's): picks it; right-click its menu
    TbAddFolder,  // + Add folder...
    TbGroup,      // a = its row: a subfolder's header, which folds it
    TbRow,        // a = its row in the list: loads it into the operator
    TbScroll,     // the list's area, which the wheel scrolls
    TbScrollBar,  // the list's scrollbar along its right: its thumb dragged, or the track clicked to jump there
    TbDone,       // closes the browser, keeping what was picked
    TbImport,     // Import file...: the file dialog
};

struct Widget {
    WidgetKind kind = WidgetKind::Knob;
    Rect rect;
    int param = -1;
    int a = -1;
    int b = -1;
    const char *label = nullptr;
    bool compact = false; // header knob with its label and number beside it
    bool shrunk = false;  // a compact knob drawn smaller (the bend range, in the keyboard strip's top row)
    bool disabled = false; // dimmed, and takes no input (a fixed operator's Pitch and Detune, their depth handles)
    bool faint = false;    // dimmed, but takes input still (an amp's cabinet controls while the cabinet is off)
    Rect clip{};          // on a card that may be scrolled partly out: drawn and hit only within this
    bool popup = false;   // in the open list of a pill (its row's bypass, depth and remove): over everything else
    // One of a row of buttons drawn merged into one pill, as Poly | Mono: the pill, and from where to where this
    // one's part of it reaches (halfway across the gaps to its neighbors). group.w is 0 for a button on its own.
    Rect group{};
    float partLeft = 0.0f, partRight = 0.0f;
    bool modeGlyph = false; // a wire label in the graph showing its route's mode (not Phase) inside it
    // A stepper's field (see isStepper()): drawn in place of its knob, and hit as its rect is. It may reach past the
    // rect's sides, as far as the widgets beside it leave room.
    Rect well{};

    bool same(const Widget &o) const { return kind == o.kind && param == o.param && a == o.a && b == o.b; }
};

// Merges the buttons added to `list` since `from` into one pill, as Poly | Mono, each its part of it (see
// Widget::group).
inline void mergeButtons(std::vector<Widget> &list, size_t from) {
    if (list.size() < from + 2)
        return;
    Rect group = list[from].rect;
    group.w = list.back().rect.right() - group.x;
    for (size_t i = from; i < list.size(); ++i) {
        Widget &w = list[i];
        w.group = group;
        w.partLeft = i == from ? group.x : (list[i - 1].rect.right() + w.rect.x) * 0.5f;
        w.partRight = i + 1 == list.size() ? group.right() : (w.rect.right() + list[i + 1].rect.x) * 0.5f;
    }
}

// A line of text being edited in a field: what is typed, the caret in it and where its selection starts (at the
// caret, nothing is selected), where each place between its characters was painted (across), and how far its text
// is scrolled left, so the caret shows. Typing adds only printable ASCII, but a field may be given UTF-8 from a file
// (a preset's name, author or line): the caret, the selection, deleting and `most` go by whole characters, and the
// caret and anchor are always at a character's first byte.
struct TextField {
    std::string text;
    size_t caret = 0;
    size_t anchor = 0;
    std::vector<float> stops;
    float shift = 0.0f;
    size_t most = 40; // the most characters it holds

    // Its selection, from the anchor to the caret, either way round.
    std::pair<size_t, size_t> selection() const { return std::minmax(caret, anchor); }
    // `value` as its text, cut to `most` characters, the caret at its end and nothing selected.
    void set(std::string value);
    // Whether byte `at` begins a character (or is the text's end).
    bool startsCharacter(size_t at) const;
    // The place between characters before `at` and after it.
    size_t before(size_t at) const;
    size_t after(size_t at) const;
    // How many characters it holds.
    size_t characters() const;
    void erase(size_t from, size_t to);
    // Over the selection; line breaks and tabs as spaces, the rest past printable ASCII left out, as much as
    // fits. Whether the text changed.
    bool insert(std::string_view typed);
    void clear();
    // The place between characters nearest `x`, by where they were painted; before they were, its end.
    size_t stopAt(float x) const;
    // Selects the word at `x`, up to the spaces either side.
    void selectWord(float x);
    // Backspace, Delete, the arrows, Home, End and Select All, Shift growing the selection: whether the text
    // changed. Enter, Escape, Up and Down are the field owner's.
    bool key(Key key, bool extend);
    // Its words, in lower case, and whether `text` holds every one of them, whatever its case.
    std::vector<std::string> words() const;
    static bool holds(std::string_view text, const std::vector<std::string> &words);
};
// The text fields, each by its owner: the preset browser's search, the MODS view's filter, the search atop a MODS
// row's target menu, the Save dialog's name, author and description, the New Category or New Folder form's name, and
// the table browser's search and a folder's name as it is renamed there.
enum class Field { Browser, ModsFilter, CascadeSearch, SaveName, SaveAuthor, SaveDescription, NewName, Tables, TableRename };

} // namespace fmvibe::gui
