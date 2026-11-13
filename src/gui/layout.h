#pragma once

// The editor's layout, in logical pixels: where the panels, cards, buttons, lists and dialogs go. Shared by the
// editor's own files only (see view.cpp), in namespace detail.

#include "theme.h"
#include "view.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace fmvibe::gui::detail {

// --- Layout, in logical pixels.

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kKnobStart = -0.75f * kPi; // knobs sweep 270 degrees
constexpr float kKnobSweep = 1.5f * kPi;

// The routing panel shows either the graph or the matrix; both edit the same params.
// Tabs above the panels pick what they show: the routing graph or matrix next
// to the inspector, or the effects across both.
// Over them the header: a title bar (the name, the preset bar and the toolbar), a faint line under it, then the global
// strip (the output scope and meter, the global knobs, the play cluster and Voices), a line under the two.
constexpr float kTitleBarH = 36.0f;
constexpr float kTitleRowY = 6.0f; // the preset bar's and the toolbar's top
constexpr float kHeaderH = 92.0f;
constexpr float kStripY = 32.0f; // how far the global strip's content sits below where it stood alone (13 to 51 px)
constexpr float kPanelsTop = 132.0f;
constexpr float kPanelRadius = kRadiusPanel; // the panels' corners
// The routing graph and the matrix share one panel with the inspector, kFxPanel's place: the inspector floats as a
// lens over its left end, a card a little in from the panel's edges, and the graph or matrix takes the rest. The
// graph's OUT is a core at its right end, the orbit rings around it reaching back across the panel.
constexpr Rect kInspector{24.0f, kPanelsTop + 6.0f, 300.0f, 360.0f};
constexpr Rect kRouting{kInspector.x + kInspector.w + 8.0f, kPanelsTop, 852.0f, 372.0f};
constexpr Point kOutCore{kRouting.x + kRouting.w - 70.0f, kRouting.y + 0.5f * kRouting.h};
constexpr float kOutCoreRadius = 34.0f;
constexpr Rect kGraph{kRouting.x + 40.0f, kRouting.y + 56.0f, kRouting.w - 190.0f, kRouting.h - 112.0f}; // node centers
// Ctrl+drag puts a node of the routing graph on the nearest point of a coarse grid over the area its center may go
// in: the cells' centers, kRoutingGridCols x kRoutingGridRows.
constexpr int kRoutingGridCols = 12, kRoutingGridRows = 5;
inline Point gridPoint(Rect area, int cols, int rows, int col, int row) {
    return {area.x + (static_cast<float>(col) + 0.5f) / static_cast<float>(cols) * area.w,
            area.y + (static_cast<float>(row) + 0.5f) / static_cast<float>(rows) * area.h};
}
constexpr float kNodeRadius = 24.0f;
// The node the inspector shows: filled with its color at this strength, and ringed in it this far out.
constexpr float kSelectedFill = 0.25f;
constexpr float kSelectedRing = 5.0f;
constexpr float kFilterHalfW = 33.0f;
constexpr float kFilterHalfH = 17.0f;
constexpr int kGridCols = kNodes + 1; // operators, filters, shapers, output
// The matrix lists every operator, filter and shaper, and fills the panel's height: the column names 14 px below
// the panel's top, then the rows, the last one 14 px above its bottom.
constexpr float kMatrixMargin = 14.0f;
constexpr float kMatrixNamesH = 22.0f; // the column names' row, over the first row of cells
constexpr float kCellGap = 4.0f;
constexpr float kCellStep = (kRouting.h - 2.0f * kMatrixMargin - kMatrixNamesH + kCellGap) / kNodes;
constexpr float kCell = kCellStep - kCellGap;
constexpr Point kGridOrigin{kRouting.x + 56.0f, kRouting.y + kMatrixMargin + kMatrixNamesH};
constexpr float kLegendX = kGridOrigin.x + kGridCols * kCellStep + 40.0f;
// The inspector's tabs, one per node along its top: the operators', the filters' and the shapers' in three groups, a
// hairline between two groups (kNodeTabDivider()).
constexpr float kNodeTabStep = 24.0f, kNodeTabGroupGap = 13.0f;
inline Rect nodeTabRect(int node) {
    const int group = node < kOps ? 0 : node < kOps + kFilters ? 1 : 2;
    return {kInspector.x + 16.0f + node * kNodeTabStep + group * kNodeTabGroupGap, kInspector.y + 9.0f,
            kNodeTabStep - 1.0f, 22.0f};
}
// The hairline before group 1 (the filters) or 2 (the shapers), in the middle of the gap.
inline float nodeTabDivider(int group) {
    const int first = group == 1 ? kOps : kOps + kFilters;
    return nodeTabRect(first).x - 0.5f * (kNodeTabGroupGap + 1.0f);
}
// The page below the tabs, title and switch, dimmed while its node is off.
constexpr Rect kInspectorPage{kInspector.x + 1.0f, kInspector.y + 60.0f, kInspector.w - 2.0f, kInspector.h - 61.0f};
// The page's title row under the tabs: the node's letter, then its summary (an operator's source switch instead); the
// switches at its right end.
constexpr Rect kTitleRow{kInspector.x + 16.0f, kInspector.y + 37.0f, kInspector.w - 32.0f, 20.0f};
constexpr float kTitleLetterW = 16.0f;
// An operator's Fixed switch, a keyboard on the title row left of the power switch.
constexpr Rect kFixedSwitch{kInspector.x + kInspector.w - 16.0f - 26.0f - 30.0f, kInspector.y + 37.0f, 26.0f, 19.0f};
// An operator's sound in one block under the title row, whose place its source switch (WAVE | TABLE | SAMPLE) takes:
// at its left, its pitch (the Ratio readout, Freq and Mult when fixed, or a noise operator's Slope and Spread), as tall
// as two rows; at its right, the waves (or the table's or the sample's picker), and under them the Pitch, Detune, Phase
// and Pan knobs. Their labels, and the Ratio readout's, share one line.
constexpr float kOpHeroY = kInspector.y + 64.0f;
constexpr float kOpPitchW = 88.0f; // the left column
constexpr float kOpRightX = kInspector.x + 16.0f + kOpPitchW + 12.0f;
constexpr float kOpRightW = kInspector.x + kInspector.w - 16.0f - kOpRightX;
// The source switch, three equal segments along the title row, where a filter's or shaper's page has its summary; it
// stops short of a Custom wave's edit button, left of the keyboard switch.
constexpr Rect kOpSourceRow{kTitleRow.x, kFixedSwitch.y, kFixedSwitch.x - kFixedSwitch.w - 10.0f - kTitleRow.x,
                            kFixedSwitch.h};
inline Rect opSourceButton(int source) {
    const float w = kOpSourceRow.w / 3.0f;
    return {kOpSourceRow.x + source * w, kOpSourceRow.y, w, kOpSourceRow.h};
}
constexpr float kOpRowY = kOpHeroY;
constexpr float kOpWavesH = 20.0f; // the waves' row, over the knobs
constexpr float kOpKnobY = kOpHeroY + 38.0f, kOpKnobD = 28.0f;
// The TABLE row: a step back, the table's picker (its glyph, its name and a chevron), a step on. The SAMPLE row: the
// sample's picker, then the import button.
inline Rect tableStepButton(int dir) {
    return {dir < 0 ? kOpRightX : kOpRightX + kOpRightW - 18.0f, kOpRowY, 18.0f, kOpWavesH};
}
constexpr Rect kTablePicker{kOpRightX + 20.0f, kOpRowY, kOpRightW - 40.0f, kOpWavesH};
constexpr Rect kSamplePicker{kOpRightX, kOpRowY, 144.0f, kOpWavesH};
constexpr Rect kSampleImport{kOpRightX + kOpRightW - 20.0f, kOpRowY, 20.0f, kOpWavesH};
// Place 0-3 along the right column (Pitch, Detune, Phase, Pan); the left column's two (Freq and Mult, or Slope and
// Spread) are places -2 and -1.
inline Point opKnobCenter(int place) {
    if (place < 0)
        return {kInspector.x + 16.0f + kOpPitchW * (place + 2.5f) * 0.5f, kOpKnobY};
    return {kOpRightX + kOpRightW * (place + 0.5f) / 4.0f, kOpKnobY};
}
// An operator's Ratio: a 7-segment readout, 0.50 to 64.00, the left column of its block, each digit dragged on its
// own; its widget runs on under the box to the label, as a knob's does (see ratioDigit()). The track runs along its
// top, clear of the depth handles along its bottom edge.
// Its top level with the waves' row, and its label on the knobs' labels' line (kOpLabelY), as a knob's is under it.
constexpr Rect kRatioBox{kInspector.x + 16.0f, kOpRowY, kOpPitchW, 44.0f};
constexpr float kOpLabelY = kOpKnobY + 0.5f * kOpKnobD + 2.0f; // the top of the block's labels' line
constexpr int kRatioDigits = 4; // tens, ones, tenths, hundredths
constexpr long kRatioPlaces[kRatioDigits] = {1000, 100, 10, 1}; // in hundredths
constexpr float kRatioDigitW = 15.0f, kRatioDigitH = 21.0f, kRatioDigitGap = 2.5f, kRatioPointW = 6.5f;
// Digit k's place in the box (the decimal point after the second).
inline Rect ratioDigitRect(int k) {
    const float total = kRatioDigits * kRatioDigitW + (kRatioDigits - 1) * kRatioDigitGap + kRatioPointW;
    const float left = kRatioBox.x + (kRatioBox.w - total) * 0.5f;
    const float x = left + k * (kRatioDigitW + kRatioDigitGap) + (k >= 2 ? kRatioPointW : 0.0f);
    return {x, kRatioBox.y + 11.0f, kRatioDigitW, kRatioDigitH};
}
// The digit nearest along the box to x.
inline int ratioDigit(float x) {
    int nearest = 0;
    for (int k = 1; k < kRatioDigits; ++k)
        if (std::abs(ratioDigitRect(k).center().x - x) < std::abs(ratioDigitRect(nearest).center().x - x))
            nearest = k;
    return nearest;
}
// The track over the digits: 0.5 to 64 in a straight line, with the ratio and its modulations along it (an arrow
// after its end for a modulation that goes on past 64).
constexpr Rect kRatioTrack{kRatioBox.x + 6.0f, kRatioBox.y + 4.0f, kRatioBox.w - 12.0f, 2.0f};
// A Custom wave's edit button (a pencil, as big as the keyboard switch, left of it): opens its partial editor.
constexpr Rect kPartialEditButton{kFixedSwitch.x - 4.0f - kFixedSwitch.w, kFixedSwitch.y, kFixedSwitch.w,
                                  kFixedSwitch.h};
// The edit buttons in a display's corner, a pencil each: a Custom LFO's top right, a drawn shaper curve's bottom right.
constexpr float kEditButtonW = 24.0f, kEditButtonH = 16.0f;
inline Rect lfoEditButton(Rect display) {
    return {display.right() - 4.0f - kEditButtonW, display.y + 4.0f, kEditButtonW, kEditButtonH};
}
inline Rect clipEditButton(Rect display) {
    return {display.right() - 6.0f - kEditButtonW, display.bottom() - 5.0f - kEditButtonH, kEditButtonW, kEditButtonH};
}
// An operator's page: under its block, a line across, then a well whose top edge holds the tabs (one per
// InspectorTab) and whose display under them shows what the tab picks; the tab's controls in a row under the well.
constexpr float kOpRuleY = kInspector.y + 144.0f;
constexpr float kOpTabsY = kInspector.y + 150.0f;
constexpr float kOpTabH = 22.0f;
// An even height keeps the envelope's levels (from 12 px under its top to 10 px over its bottom) on whole pixels at 0.5.
constexpr Rect kOpDisplay{kInspector.x + 16.0f, kOpTabsY + kOpTabH, kInspector.w - 32.0f, 118.0f};
// The well, the tabs and the display; INPUTS, with no controls under it, takes it down to the lens's bottom.
constexpr Rect kOpWell{kOpDisplay.x, kOpTabsY, kOpDisplay.w, kOpDisplay.y + kOpDisplay.h - kOpTabsY};
constexpr Rect kOpWellTall{kOpDisplay.x, kOpTabsY, kOpDisplay.w, kInspector.y + kInspector.h - 10.0f - kOpTabsY};
constexpr float kOpControlsY = kOpDisplay.y + kOpDisplay.h + 25.0f; // the controls' knobs' centers
// Which tabs an operator has: a plain wave's, a noise operator's, a Wavetable operator's (its TABLE tab, which holds
// its warp, in WARP's place) or a Sample operator's (its SAMPLE tab in WARP's place).
enum class OpTabs { Plain, Noise, Table, Sample };
inline Rect opTab(int tab, OpTabs tabs = OpTabs::Plain) {
    // UNISON, ENVELOPE, SCOPE, INPUTS, WARP, SOURCE by InspectorTab; along the line ENVELOPE, UNISON, WARP, INPUTS, a
    // quarter of the well each. SCOPE is no longer a tab of its own (the WARP tab shows the scope; a layout saved on it
    // opens WARP): it sits where WARP does. A noise operator's are ENVELOPE, SPECTRUM (in UNISON's place) and INPUTS, a
    // third each. A Wavetable operator's are ENVELOPE, UNISON, TABLE and INPUTS, and a Sample operator's ENVELOPE,
    // UNISON, SAMPLE and INPUTS, a quarter each.
    constexpr int kPlace[] = {1, 0, 2, 3, 2, 2}, kNoisePlace[] = {1, 0, 2, 2, 2, 2};
    constexpr int kDataPlace[] = {1, 0, 2, 3, 2, 2};
    static_assert(std::size(kPlace) == static_cast<size_t>(InspectorTab::Count));
    const int *places = tabs == OpTabs::Noise                               ? kNoisePlace
                        : tabs == OpTabs::Table || tabs == OpTabs::Sample ? kDataPlace
                                                                            : kPlace;
    const float count = tabs == OpTabs::Noise ? 3.0f : 4.0f;
    const float w = (kOpWell.w - 8.0f) / count;
    return {kOpWell.x + 4.0f + places[tab] * w, kOpTabsY, w, kOpTabH};
}
// The TABLE tab: its display, which is Position's control, then along the row under it the warps' buttons, the warp's
// Amount knob and the NOTE (note sync) switch, where the WARP tab has them (warpButton(), kWarpAmountKnob,
// kSyncButton). The SAMPLE tab: its display with the ROOT readout in its top right corner, then LOOP and MODE, each a
// row of three, under it, named under them.
inline Rect sampleRootBox(Rect display) { return {display.right() - 6.0f - 56.0f, display.y + 4.0f, 56.0f, 16.0f}; }
inline Rect sampleLoopButton(int choice, float y) {
    return {kInspector.x + 16.0f + choice * 42.0f, y - 10.0f, 42.0f, 20.0f};
}
inline Rect sampleModeButton(int choice, float y) {
    return {kInspector.x + kInspector.w - 16.0f - (3 - choice) * 42.0f, y - 10.0f, 42.0f, 20.0f};
}
// Wave: its display (the scope; with no note, the operator's cycles as its warp reads them, or the note's when
// synced, the cycle buttons in its top right corner), then the warps' buttons, the amount's knob and the NOTE
// (note sync) switch along the row under it.
constexpr Rect kScopeRect = kOpDisplay;
inline Rect warpButton(int warp) {
    return {kInspector.x + 16.0f + warp * 26.0f, kOpControlsY - 11.0f, 26.0f, 22.0f};
}
constexpr Point kWarpAmountKnob{kInspector.x + 210.0f, kOpControlsY};
constexpr Rect kSyncButton{kInspector.x + kInspector.w - 16.0f - 46.0f, kOpControlsY - 11.0f, 46.0f, 22.0f};
// Inputs: a row per route into the operator, its source's name, its mode's buttons, the mode's name and its depth,
// with room under the depth for its modulations' handles, inside the tall well. Past the rows the tab holds, they
// scroll, with a bar at their right; drawn and hit within kInputsList, which leaves room for the ranges on the first
// row's depth's top.
constexpr float kInputsTop = kOpDisplay.y + 8.0f;
constexpr float kInputsBottom = kOpWellTall.y + kOpWellTall.h - 4.0f;
constexpr float kInputsStep = 30.0f;
constexpr float kInputsRowSpan = 28.0f; // a row and the handles under its depth
constexpr Rect kInputsList{kOpWell.x, kInputsTop - 3.0f, kOpWell.w, kInputsBottom - kInputsTop + 3.0f};
inline Rect inputsScrollTrack() { return {kOpWell.right() - 7.0f, kInputsTop + 1.0f, 4.0f, kInputsBottom - kInputsTop - 2.0f}; }
inline Rect inputsName(float y) { return {kOpWell.x + 8.0f, y, 26.0f, 18.0f}; }
inline Rect inputsMode(float y, int mode) { return {kOpWell.x + 36.0f + mode * 26.0f, y, 26.0f, 18.0f}; }
inline Rect inputsModeName(float y) { return {kOpWell.x + 146.0f, y, 66.0f, 18.0f}; }
inline Rect inputsDepth(float y) { return {kOpWell.right() - 12.0f - 48.0f, y, 48.0f, 18.0f}; }
// Envelope: the curve, its knobs under the well.
constexpr Rect kOpEnvRect = kOpDisplay;
// Unison: where the copies sit, its knobs under the well.
constexpr float kUnisonKnobsY = kOpControlsY;
constexpr Rect kUnisonRect = kOpDisplay;
// A noise operator's spectrum, in Unison's place.
constexpr Rect kNoiseRect = kOpDisplay;
inline Rect scopeCycleButton(int choice) {
    return {kScopeRect.right() - 6.0f - (3 - choice) * 20.0f, kScopeRect.y + 6.0f, 20.0f, 16.0f};
}
// Every filter slope's list (a voice filter's, the filter effect's and a split's): a button under the display it goes
// with, in the middle.
inline Rect slopeButton(Rect display) {
    return {display.center().x - 36.0f, display.bottom() + 4.0f, 72.0f, 18.0f};
}
// A filter's page: its type buttons across the row under the summary, its response with the slope's list under it.
inline Rect filterTypeButton(int type) {
    constexpr float kStep = (kInspector.w - 32.0f + 4.0f) / static_cast<float>(FilterType::Count);
    return {kInspector.x + 16.0f + type * kStep, kInspector.y + 62.0f, kStep - 4.0f, 22.0f};
}
constexpr Rect kFilterDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 90.0f};
inline const Rect kFilterSlopeButton = slopeButton(kFilterDisplay);
// A shaper's page: its curve's list and the link switch in the row under the summary, with the symmetric switch
// between them for a Custom curve, then its transfer curve where a filter's response shows.
constexpr Rect kShaperLinkButton{kInspector.x + kInspector.w - 16.0f - 44.0f, kInspector.y + 62.0f, 44.0f, 22.0f};
constexpr Rect kShaperSymButton{kShaperLinkButton.x - 50.0f, kShaperLinkButton.y, 44.0f, 22.0f};
inline Rect shaperModeButton(bool custom) {
    return {kInspector.x + 16.0f, kInspector.y + 62.0f, kInspector.w - 32.0f - (custom ? 100.0f : 50.0f), 22.0f};
}
constexpr Rect kShaperDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 112.0f};
// An operator's wave buttons: the common waves, then "..." with a list of the others.
constexpr OpWave kCommonWaves[] = {OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Noise};
constexpr OpWave kMoreWaves[] = {OpWave::Pulse,   OpWave::HalfSine, OpWave::AbsSine, OpWave::AltSine,
                                 OpWave::Organ,   OpWave::Vowel,    OpWave::Custom};
// Every wave but Wavetable and Sample, which the source switch on the title row picks (see kOpSourceRow).
static_assert(std::size(kCommonWaves) + std::size(kMoreWaves) + 2 == static_cast<size_t>(OpWave::Count));
constexpr int kWaveButtons = static_cast<int>(std::size(kCommonWaves)) + 1;
// One segmented row along the block's top, in its right column.
inline Rect waveButton(int button) {
    const float step = kOpRightW / kWaveButtons;
    return {kOpRightX + button * step, kOpRowY, step, kOpWavesH};
}
// The list "..." opens, right-aligned under it.
inline Rect waveMenu() {
    const Rect more = waveButton(kWaveButtons - 1);
    return {more.right() - 132.0f, more.bottom() + 4.0f, 132.0f, 8.0f + std::size(kMoreWaves) * 24.0f};
}
inline Rect waveMenuItem(int item) {
    const Rect menu = waveMenu();
    return {menu.x + 4.0f, menu.y + 4.0f + item * 24.0f, menu.w - 8.0f, 24.0f};
}
inline bool isMoreWave(OpWave wave) {
    return std::find(std::begin(kMoreWaves), std::end(kMoreWaves), wave) != std::end(kMoreWaves);
}
// The table browser, over the routing panel as the partial editor is: along its top its title, the search, Import
// file... and Done; under them the sources' column at its left, the list in the middle and the preview at its right;
// the footer along its bottom.
constexpr Rect kTableBrowser{kRouting.x + 14.0f, kRouting.y + 10.0f, kRouting.w - 28.0f, kRouting.h - 20.0f};
constexpr float kTbTop = kTableBrowser.y + 38.0f, kTbBottom = kTableBrowser.y + kTableBrowser.h - 24.0f;
constexpr Rect kTbSources{kTableBrowser.x, kTbTop, 176.0f, kTbBottom - kTbTop};
constexpr Rect kTbList{kTbSources.x + kTbSources.w + 12.0f, kTbTop, 404.0f, kTbBottom - kTbTop};
constexpr Rect kTbPreview{kTbList.x + kTbList.w + 12.0f, kTbTop,
                          kTableBrowser.x + kTableBrowser.w - (kTbList.x + kTbList.w) - 12.0f, kTbBottom - kTbTop};
constexpr float kTbRowH = 24.0f;
constexpr Rect kTbSearch{kTbList.x, kTableBrowser.y + 4.0f, kTbList.w, 26.0f};
constexpr Rect kTbDone{kTableBrowser.x + kTableBrowser.w - 64.0f, kTableBrowser.y + 5.0f, 64.0f, 24.0f};
constexpr Rect kTbImport{kTbDone.x - 8.0f - 104.0f, kTableBrowser.y + 5.0f, 104.0f, 24.0f};
inline Rect tbSourceRow(int row) { return {kTbSources.x, kTbSources.y + row * kTbRowH, kTbSources.w, kTbRowH - 2.0f}; }
inline Rect tbListRow(float row) { return {kTbList.x, kTbList.y + row * kTbRowH, kTbList.w - 10.0f, kTbRowH - 2.0f}; }

// The row between the header and the panels: the view tabs at its left.
constexpr float kTopRowY = kHeaderH + 8.0f;
constexpr float kTopRowH = 24.0f; // the preset bar's and the toolbar's height, in the title bar
// Tabs of the routing panel, one per RoutingView: folder tabs from the row's top down onto the panels' top edge,
// the first one's left side over the panel's left edge, which it runs on into while it is shown.
constexpr float kViewTabH = kPanelsTop - kTopRowY;
constexpr Rect kViewTabs[] = {{16.0f, kTopRowY, 96.0f, kViewTabH},
                               {116.0f, kTopRowY, 88.0f, kViewTabH},
                               {208.0f, kTopRowY, 64.0f, kViewTabH},
                               {276.0f, kTopRowY, 76.0f, kViewTabH}};
constexpr float kViewTabRadius = kRadiusCard; // its top corners
constexpr float kViewTabFlare = 6.0f;  // the shown one's curves into the panel's edge
// The FX view covers the routing panel and the inspector: one card per effect,
// left to right in processing order, then a card to add one; kVisibleFx at a time.
constexpr Rect kFxPanel{16.0f, kPanelsTop, kRouting.x + kRouting.w - 16.0f, kRouting.h};
constexpr int kVisibleFx = 6;
constexpr float kFxGap = 22.0f; // room for the plus that adds an effect between two cards
// The row's first card starts a gap in from the clip it scrolls in (see fxCardClip()), for the arrow before it.
constexpr float kFxRowLeft = 8.0f + kFxGap;
constexpr float kFxCardW = (kFxPanel.w - kFxRowLeft - 16.0f - (kVisibleFx - 1) * kFxGap) / kVisibleFx;
constexpr float kFxTop = kRouting.y + 48.0f;
// The MODS view covers them too: a row per modulation under the columns' names (the count at the right of their
// row), scrolling with a bar at the right. Within a row, from its left: the bypass dot, the source and its polarity
// switch, an arrow, the target, the depth bar with its value, the Amount bar with its value, the aux and the remove
// button. After the last row, the button that adds an empty one.
constexpr float kModsRowH = 28.0f;
constexpr float kModsRowStep = 30.0f;
constexpr Rect kModsList{kFxPanel.x + 12.0f, kFxPanel.y + 36.0f, kFxPanel.w - 24.0f - 14.0f, kFxPanel.h - 46.0f};
constexpr float kModsHeaderY = kFxPanel.y + 12.0f;
inline Rect modsRow(float row) { // row: counted from the list's top, fractions while it scrolls
    return {kModsList.x, kModsList.y + row * kModsRowStep, kModsList.w, kModsRowH};
}
inline Rect modsScrollTrack() {
    return {kModsList.right() + 6.0f, kModsList.y + 2.0f, 4.0f, kModsList.h - 4.0f};
}
// The columns, as x within a row and width.
struct ModsColumn {
    float x, w;
};
constexpr ModsColumn kModsBypass{4.0f, 18.0f}, kModsSource{26.0f, 130.0f}, kModsPolarity{158.0f, 24.0f},
    kModsArrow{186.0f, 16.0f}, kModsTarget{206.0f, 232.0f}, kModsDepth{446.0f, 210.0f}, kModsCurve{668.0f, 52.0f},
    kModsScale{736.0f, 100.0f},
    kModsAux{890.0f, 196.0f}, kModsRemove{1104.0f, 24.0f}, kModsAdd{4.0f, 178.0f};
inline Rect modsCell(Rect row, ModsColumn column) { return {row.x + column.x, row.y, column.w, row.h}; }
// A text field's x at its right end, which empties it, and where its text goes: after the magnifier, short of the x.
inline Rect fieldClear(Rect field) { return {field.right() - 22.0f, field.y + 3.0f, 18.0f, 18.0f}; }
inline Rect fieldTyped(Rect field) { return {field.x + 26.0f, field.y, field.w - 26.0f - 24.0f, field.h}; }
// The MODS view's filter, at the right end of the columns' names: a magnifier, or open, a field over the count.
constexpr float kModsFilterW = 200.0f;
inline Rect modsFilter(bool open) {
    const float w = open ? kModsFilterW : 24.0f;
    return {kFxPanel.right() - 16.0f - w, kModsHeaderY - 2.0f, w, 24.0f};
}
// The cascading menus a row's source, target or aux opens: each level a column of entries, the next level beside the
// entry it opens from.
constexpr float kCascadeItemH = 22.0f;
constexpr float kCascadeHeaderH = 20.0f;
constexpr float kCascadePad = 4.0f;
// The target menu's search, atop its first level, which is at least as wide as kCascadeSearchW; the most targets it
// lists.
constexpr float kCascadeSearchH = 30.0f;
constexpr float kCascadeSearchW = 240.0f;
constexpr int kCascadeMatches = 16;
// The global strip, under the title bar: the output scope and meter, the global knobs, the play cluster between two
// dividers, then Voices, with PLAYING at the right end: spread across the editor as the title bar is, from the scope
// under the name's left edge to PLAYING over the panels' right one. Laid out from kHeaderStart, the meter's left edge.
constexpr float kOutScopeW = 96.0f;
constexpr float kOutScopeGap = 8.0f; // between the scope and the meter
constexpr float kHeaderStart = 24.0f + kOutScopeW + kOutScopeGap;
// The output meter, left of the Volume knob: a bar for each side, from -48 dB at the bottom to +6 dB at the top, red
// past full scale.
constexpr Rect kMeter{kHeaderStart, kStripY + 13.0f, 14.0f, 38.0f};
// The output scope, left of the meter and as tall: two periods of the newest note at a time (see kOutScopeBins).
constexpr Rect kOutScope{kMeter.x - kOutScopeGap - kOutScopeW, kMeter.y, kOutScopeW, kMeter.h};
// A span whose peak is below this draws nothing, and spans that small changing paint nothing again: the browser demo's
// output never quite reaches 0 (see Effects::process()).
constexpr float kOutScopeFloor = 1e-5f;
inline float outScopePeak(const Telemetry &t) {
    float peak = 0.0f;
    for (float v : t.outScope)
        peak = std::max(peak, std::abs(v));
    return peak;
}
// A scope's level follows its spans' peaks, up at once and down by this much a poll (about 60 a second: down to a
// third in about 300 ms), so a level that moves from span to span doesn't jolt what the scope shows. Returns whether
// it moved by more than a hundredth.
constexpr float kScopeLevelFall = 0.946f;
inline bool followScopeLevel(float &level, float peak) {
    const float before = level;
    level = peak >= level ? peak : std::max(peak, level * kScopeLevelFall);
    return std::abs(level - before) > 0.01f * before;
}
// The global knobs, Volume, FM Amount, Pitch, Bend (the bend range) and Pan, each as far from the next as its widest
// value needs ("-48.00 st" for Pitch, "96 st" for Bend), the first one's arc (2 px into its place) 43 px past the
// meter.
constexpr int kHeaderKnobs = 5;
constexpr float kHeaderKnobPlaces[kHeaderKnobs] = {106.0f, 106.0f, 108.0f, 96.0f, 100.0f}; // to the next one's
constexpr float kHeaderKnobX = kHeaderStart + 55.0f;
constexpr float headerKnobX(int knob) {
    float x = kHeaderKnobX;
    for (int i = 0; i < knob; ++i)
        x += kHeaderKnobPlaces[i];
    return x;
}
static_assert(kHeaderKnobX + 2.0f - (kHeaderStart + 14.0f) == 43.0f);
// The first divider, before the play cluster, 30 px past the Pan knob's widest value ("100 L", 78 px into its place).
constexpr float kDivider0 = headerKnobX(kHeaderKnobs - 1) + 78.0f + 30.0f;
constexpr float kPlayX = kDivider0 + 30.0f; // Poly | Mono over Legato, 30 px past the first divider
// The Glide knob 16 px past them, its Always switch (on its label's row) ending 24 px before the second divider.
constexpr float kGlideX = kPlayX + 104.0f;
constexpr float kGlideW = 126.0f;
constexpr float kDividers[] = {kDivider0, kGlideX + kGlideW + 10.0f + 24.0f};
// Voices, 39 px past the second divider, right beside PLAYING (whose word starts at about 1134 px): its own place
// narrower than the global knobs', so it stops short of the count.
constexpr float kVoicesX = kDividers[1] + 39.0f;
constexpr float kVoicesW = 80.0f;
static_assert(kVoicesX == 1040.0f && kVoicesX + kVoicesW <= 1120.0f);
// Legato under Poly | Mono, as wide as the two of them, how the notes that come in are read.
constexpr Rect kLegatoSwitch{kPlayX, kStripY + 35.0f, 88.0f, 19.0f};
constexpr double kMeterFloorDb = -48.0;
constexpr double kMeterTopDb = 6.0;
constexpr Color kMeterRed = Color::hex(0xEF4444);

// The modulation panel: a title row with a chip per source (drag one onto a
// param to modulate it, click it to bring its card into view) and the buttons
// that add sources, then the sources' cards, kVisibleCards at a time, in the
// order they show (UiLayout::sourceOrder).
// Open, tall enough for a card's button under its display to end 12 px above its bottom.
constexpr Rect kModPanel{16.0f, kPanelsTop + 372.0f + 16.0f, 1168.0f, 258.0f};
constexpr float kModBarH = 42.0f; // the title row: all that shows while collapsed
constexpr int kVisibleCards = 4;
constexpr float kCardGap = 12.0f;
constexpr float kCardW = (kModPanel.w - 24.0f - (kVisibleCards - 1) * kCardGap) / kVisibleCards;
constexpr float kCardTop = kModPanel.y + 44.0f;
constexpr float kCardH = kModPanel.y + kModPanel.h - 18.0f - kCardTop; // the scrollbar goes under them
constexpr float kAddSourceX = kModPanel.x + 112.0f;

inline Rect modPanel(bool collapsed) {
    return {kModPanel.x, kModPanel.y, kModPanel.w, collapsed ? kModBarH : kModPanel.h};
}

inline Rect sourcesToggle() {
    return {kModPanel.x + 8.0f, kModPanel.y + 8.0f, 100.0f, 26.0f};
}

// Every card's title row, an effect's, a source's or a split's: its mark (the effect's icon or the source's handle) and
// its name at the left; at the right the fold button, the power switch (a split has none) and the cross. Its name
// stops short of the fold button (cardTitleRight()).
inline Rect cardClose(Rect card) {
    return {card.right() - 22.0f, card.y + 8.0f, 14.0f, 21.0f};
}
inline Rect cardPower(Rect card) {
    return {card.right() - 44.0f, card.y + 8.5f, 20.0f, 19.0f};
}
// An effect card in the effects graph's inspector has no cross (its node's removes it): its power switch takes the
// cross's corner, and its name goes up to it.
inline Rect cardPowerInGraph(Rect card) {
    return {card.right() - 28.0f, card.y + 8.5f, 20.0f, 19.0f};
}
inline Rect cardFold(Rect card, bool power = true) {
    return {(power ? cardPower(card).x : cardClose(card).x) - 16.0f, card.y + 9.0f, 14.0f, 19.0f};
}
inline float cardTitleRight(Rect card, bool power = true) {
    return cardFold(card, power).x - 3.0f;
}

// A card by its screen place: 0..3, fractions while it scrolls.
// `span`: the card places it takes, less than one folded.
inline Rect sourceCard(float card, float span = 1.0f) {
    return {kModPanel.x + 12.0f + card * (kCardW + kCardGap), kCardTop, span * (kCardW + kCardGap) - kCardGap, kCardH};
}
// A folded card is a narrow strip: its handle, a grip, its switch and cross, then its name down the rest, which
// unfolds it. An effect card folds much the same way: its icon, which switches it, then its cross and its name.
// That strip is as wide as the icon's (or the handle's) place at the title's left, so it stays put as the card folds.
constexpr float kSourceHandleX = 14.0f; // the handle's left edge, from the card's
constexpr float kSourceHandleY = 8.0f;  // and its top
constexpr float kSourceHandleW = 20.0f;
constexpr float kFoldedSource = (2.0f * kSourceHandleX + kSourceHandleW + kCardGap) / (kCardW + kCardGap);
inline Rect sourceHandleRect(Rect card) {
    return {card.x + kSourceHandleX, card.y + kSourceHandleY, kSourceHandleW, kSourceHandleW};
}
// A Custom LFO's card widened to edit its shape right there, two card places wide: its own controls stay where a
// normal card has them, and the shape's plot, 264 px across, goes beside them, from under the title row down to the
// row of the point readout and the grid's steppers, level with the MODS button.
constexpr float kWideSource = 2.0f;
inline Rect lfoCardPlot(float card) {
    const Rect p = sourceCard(card, kWideSource);
    return {p.x + 281.0f, p.y + 44.0f, 264.0f, 102.0f};
}
// Under the plot's frame, at its left: the point under the mouse in numbers.
inline Rect lfoCardReadout(float card) {
    const Rect plot = lfoCardPlot(card);
    return {plot.x - 8.0f, plot.bottom() + 16.0f, 120.0f, 22.0f};
}
// Right of it, the grid's steppers, across (axis 0) then up and down: each an arrow, then a step down (part 0), its
// count (1) and a step up (2).
inline Rect lfoCardGridButton(float card, int axis, int part) {
    constexpr float kX[] = {14.0f, 32.0f, 54.0f}, kW[] = {18.0f, 22.0f, 18.0f};
    const Rect readout = lfoCardReadout(card);
    return {readout.x + 128.0f + axis * 76.0f + kX[part], readout.y + 2.0f, kW[part], 18.0f};
}
// Its title row's buttons, by what they do: back to a normal card (0), the shape's reset (1) and the full-size editor
// (2). Left to right, set off from the fold button: the reset, back to a normal card, the full-size editor.
inline Rect lfoCardButton(float card, int button) {
    constexpr float kFromRight[] = {1.0f, 2.0f, 0.0f};
    const Rect p = sourceCard(card, kWideSource);
    return {p.right() - 120.0f - kFromRight[button] * 34.0f, p.y + 7.0f, 28.0f, 22.0f};
}
// The dashed card after the sources, as tall as a card and narrower: it opens the list of source types.
constexpr float kSourceShadowGap = 10.0f;
constexpr float kSourceShadowW = (kCardH - kSourceShadowGap) * 0.5f;
constexpr int kSourceTypes = static_cast<int>(SourceType::Count) - 1; // without None
// The types it lists, in its order.
constexpr SourceType kSourceMenuOrder[] = {SourceType::Env, SourceType::Lfo, SourceType::Curve, SourceType::Random,
                                           SourceType::Midi};
static_assert(std::size(kSourceMenuOrder) == static_cast<size_t>(kSourceTypes));
// That list, its top left given; an entry a type.
inline Rect sourceMenu(Point at) { return {at.x, at.y, 150.0f, 8.0f + kSourceTypes * 26.0f}; }
inline Rect sourceMenuItem(Point at, SourceType type) {
    const Rect menu = sourceMenu(at);
    const auto *order = std::begin(kSourceMenuOrder);
    const auto row = static_cast<float>(std::find(order, std::end(kSourceMenuOrder), type) - order);
    return {menu.x + 4.0f, menu.y + 4.0f + row * 26.0f, menu.w - 8.0f, 24.0f};
}
// A MIDI card's input (CC, Pressure or Slide), a list button under its title at the left, where an LFO's shape is;
// its CC and Slew knobs at the right.
inline Rect midiInputButton(Rect card) { return {card.x + 16.0f, card.y + 42.0f, 122.0f, 22.0f}; }
constexpr float kSourceShadowSpan = (kSourceShadowW + kCardGap) / (kCardW + kCardGap);
constexpr float kFxIconX = 10.0f; // the icon's left edge, from the card's
constexpr float kFxIconW = 18.0f;
constexpr float kFoldedFx = (2.0f * kFxIconX + kFxIconW + kFxGap) / (kFxCardW + kFxGap);
// A split's container in the row: its controls in a grey column a card place wide, the cards of the band it shows, then
// a narrow dashed card adding an effect to that band, kFxBoxAddW wide; its grey outline kFxBoxPad outside them all
// round, so the cards in it line up with the row's. Folded, it is a strip as wide as a folded card. Unfolded, it keeps
// kFxBoxMargin more room before and after it (kFxBoxEdge in card places), as much as its outline stands out, so the
// arrows on either side of it are as far from its outline as from any card.
constexpr float kFxBoxPad = 5.0f;
constexpr float kFxBoxMargin = kFxBoxPad;
constexpr float kFxBoxEdge = kFxBoxMargin / (kFxCardW + kFxGap);
constexpr float kFxBoxRadius = kRadiusCard + kFxBoxPad; // the outline's corners, round the cards' at its pad
constexpr float kFxBoxAddW = 64.0f;
constexpr float kFxBoxAddSpan = (kFxBoxAddW + kFxGap) / (kFxCardW + kFxGap);
// That dashed card, given the card place it starts (`at`): centered in the room between what comes before it (the last
// card, or the controls) and the container's outline, kFxGap before it and kFxBoxPad after it.
inline Rect fxBoxAddCard(Rect at) {
    return {at.x - 0.5f * (kFxGap - kFxBoxPad), at.y, kFxBoxAddW, at.h};
}
// A split's controls on a card's place (its container's column, or its card in the graph), top to bottom under the
// title row: the band tabs (left to right the low band's, a middle one's, the high one's; see tabPosition()), the
// crossover's display (see View::splitDisplay()), under it at the right the slope's list, in three bands a row of the
// two crossovers' knobs, then the bands' levels' knobs with their solos under them (the BANDS switch, 2 | 3, is in the
// title row: see splitBandsButton()). In two bands those knobs are the low band's level (0), the crossover (1) and the high band's level (2); in
// three, the low, the middle and the high band's levels, and the solos go under each level.
inline Rect splitTab(Rect card, int position, int count) {
    const float w = (card.w - 28.0f) / static_cast<float>(count);
    return {card.x + 14.0f + static_cast<float>(position) * w, card.y + 38.0f, w, 20.0f};
}
inline Point splitKnob(Rect card, int k) {
    return {card.x + card.w * static_cast<float>(1 + 2 * k) / 6.0f, card.y + 232.0f};
}
inline Point splitCrossKnob(Rect card, int which) {
    return {card.x + card.w * static_cast<float>(1 + 2 * which) / 4.0f, card.y + 180.0f};
}
inline Rect splitSoloButton(Rect card, int k) {
    return {splitKnob(card, k).x - 12.0f, card.y + 270.0f, 24.0f, 20.0f};
}
// The BANDS switch (2 | 3) in the title row, right after the split's name, which stops short of it
// (kSplitTitleW).
// (kSplitTitleW). It ends short of the fold button in the row's narrower column too.
constexpr float kSplitTitleW = 46.0f;
inline Rect splitBandsButton(Rect card, int bands) {
    return {card.x + 36.0f + kSplitTitleW + static_cast<float>(bands - 2) * 20.0f, card.y + 8.5f, 20.0f, 19.0f};
}
inline Rect splitSlopeButton(Rect display) {
    return {display.right() - 64.0f, display.bottom() + 4.0f, 64.0f, 18.0f};
}

// The cards are cut off at the row's ends while they scroll.
inline Rect sourceCardClip() {
    return {kModPanel.x + 6.0f, kCardTop - 4.0f, kModPanel.w - 12.0f, kCardH + 8.0f};
}
// A list a card's button opens is cut off at the row's ends like its card, but may reach past the card's bottom,
// down to the panel's.
inline Rect sourceListClip() {
    const Rect cards = sourceCardClip();
    return {cards.x, cards.y, cards.w, kModPanel.bottom() - cards.y};
}

// + SOURCE, after the toggle, as + EFFECT is in the FX panel (fxMenuButton()): it opens the list of source types under
// it. How many places are taken goes after it.
inline Rect sourceMenuButton() {
    return {kAddSourceX, kModPanel.y + 9.0f, 116.0f, 22.0f};
}

// A random LFO's display makes room at its right for the Smooth and Chaos knobs.
constexpr float kRandomKnobsW = 100.0f;
inline Rect sourceDisplay(float card, bool narrow = false) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 98.0f, p.w - 32.0f - (narrow ? kRandomKnobsW : 0.0f), 52.0f};
}

// 0 is Smooth, 1 Chaos.
inline Point randomKnob(float card, int knob) {
    const Rect display = sourceDisplay(card, true);
    return {display.right() + 25.0f + knob * 50.0f, display.y + 15.0f};
}

// A row of a pill's list: the target's name, between the bypass dot and the depth bar.
constexpr float kSlotNameW = 96.0f;

// The "+n" pill that takes the place of the depth handles a knob, label or cell has no room for: under a knob, and
// smaller in a label's or cell's corner; wider from +10 on. Its list: a title, then a row per modulation: its bypass
// dot, the source's name, its aux (a column only while one of the rows has one), its depth bar with its value and its
// remove.
// Both come in three sizes, a setting of the user's (Controller::handleSize()), the smallest first; they stay as far
// apart at each.
struct ModHandleSize {
    float dot, dotHover;                 // a depth handle's radius under a knob, and hovered or dragged
    float dotCompact, dotCompactHover;   // on a label, cell or depth bar
    float pill, pillWide, pillH, pillText; // under a knob
    float pillCompact, pillWideCompact, pillCompactH, pillCompactText;
};
constexpr ModHandleSize kModHandleSizes[] = {
    {3.5f, 5.0f, 2.75f, 4.0f, 12.0f, 15.0f, 9.0f, 7.5f, 11.0f, 14.0f, 8.0f, 6.5f},
    {4.0f, 5.5f, 3.25f, 4.5f, 13.0f, 16.0f, 10.0f, 8.0f, 12.0f, 15.0f, 9.0f, 7.0f},
    {4.5f, 6.0f, 3.75f, 5.0f, 14.0f, 17.0f, 11.0f, 8.5f, 13.0f, 16.0f, 10.0f, 7.5f},
};
constexpr int kModHandleSizeCount = static_cast<int>(std::size(kModHandleSizes));
inline const ModHandleSize &modHandleSize(int size) {
    return kModHandleSizes[std::clamp(size, 0, kModHandleSizeCount - 1)];
}
constexpr float kModListPad = 6.0f;
constexpr float kModListTitleH = 24.0f;
constexpr float kModListRowStep = 24.0f; // a row is 20 px high
constexpr int kModListRows = 6;          // shown at once; more scroll
constexpr float kModListBarW = 8.0f;     // the room its scrollbar takes at its right
constexpr float kModListAuxW = 96.0f;    // the aux column: its chip and a gap

// A row's aux chip, after its source's name, its x at its right end.
inline Rect modListAuxChip(Rect row) {
    return {row.x + 116.0f, row.y + 1.0f, kModListAuxW - 4.0f, row.h - 2.0f};
}

// Under a source's display, at the card's foot, the button that shows its modulations in the MODS view.
inline Rect sourceModsButton(float card) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 160.0f, p.w - 32.0f, 24.0f};
}

// A widened LFO card's column of controls has the shape tools where a normal card's display is (see ShapeTool), in
// a row, and under them Points | Steps and Snap (see ShapeMode).
constexpr int kShapeTools = 6;
inline Rect lfoToolButton(float card, int tool) {
    const Rect d = sourceDisplay(card);
    const float w = (d.w - (kShapeTools - 1) * 4.0f) / kShapeTools;
    return {d.x + tool * (w + 4.0f), d.y, w, 22.0f};
}
inline Rect lfoModeButton(float card, int mode) {
    const Rect d = sourceDisplay(card);
    return mode < 2 ? Rect{d.x + mode * 75.0f, d.y + 28.0f, 75.0f, 22.0f}
                    : Rect{d.x + 156.0f, d.y + 28.0f, d.w - 156.0f, 22.0f};
}

// A source card's title row, as every card's is (cardClose()): handle and name (the row drags the card), then the fold
// button, the power switch and the remove cross at the right; of a card `span` places wide.
inline Rect sourceRemove(float card, float span = 1.0f) {
    return cardClose(sourceCard(card, span));
}

inline Rect sourcePower(float card, float span = 1.0f) {
    return cardPower(sourceCard(card, span));
}
// A Custom LFO's normal card: left of the fold button (which is left of the power switch), the button that widens it.
inline Rect lfoWidenButton(float card) {
    return {cardFold(sourceCard(card)).x - 4.0f - 28.0f, sourceCard(card).y + 7.0f, 28.0f, 22.0f};
}

// An LFO's shape list (a button with the shape's icon and name, and the list it opens under it) over its clock modes
// (retrigger, free, anchor and one-shot, as icons), side by side as Poly | Mono are, the two rows as wide.
constexpr int kLfoModes = static_cast<int>(LfoClock::Count);
inline Rect lfoModeSwitch(float card, int mode) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f + mode * 30.5f, p.y + 72.0f, 30.5f, 16.0f};
}

inline Rect lfoShapeButton(float card) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 42.0f, lfoModeSwitch(card, kLfoModes - 1).right() - p.x - 16.0f, 22.0f};
}

// The shapes an LFO's list offers, in its order: every one but the retired Random and Custom, which the Random and
// Curve sources took over.
constexpr LfoShape kLfoShapesOffered[] = {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Square};
constexpr int kLfoShapeItems = static_cast<int>(std::size(kLfoShapesOffered));

// A Random or Curve card's polarity, Unipolar (0) | Bipolar (1), where an LFO's shape list is, each half of it, over
// its clock modes as an LFO's.
inline Rect randomPolaritySwitch(float card, int bipolar) {
    const Rect list = lfoShapeButton(card);
    return {list.x + bipolar * list.w * 0.5f, list.y, list.w * 0.5f, list.h};
}

// Right of those two rows, the Rate (0) and Phase (1) knobs with their labels, centered on them, each over a random
// LFO's Smooth and Chaos; Rate's sync switch, a small square, sits between the shape list and the knob, level with
// the list.
inline Point lfoKnob(float card, int knob) {
    const Rect top = lfoShapeButton(card), bottom = lfoModeSwitch(card, kLfoModes - 1);
    return {randomKnob(card, knob).x, (top.y + bottom.bottom()) * 0.5f - 9.0f}; // the label takes 18 px under the knob
}

inline Rect sourceSync(float card) {
    const Point rate = lfoKnob(card, 0);
    const float x = (lfoModeSwitch(card, kLfoModes - 1).right() + rate.x - 15.0f) * 0.5f;
    return {x - 10.0f, lfoShapeButton(card).center().y - 10.0f, 20.0f, 20.0f};
}

// Under it, level with the clock modes, the Unipolar switch.
inline Rect lfoUnipolarSwitch(float card) {
    const Rect sync = sourceSync(card);
    return {sync.x, lfoModeSwitch(card, 0).y, sync.w, lfoModeSwitch(card, 0).h};
}

// ADD EFFECT at the top left, and the list it opens under it: one row per type,
// then how many places are left.
inline Rect fxMenuButton() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + 9.0f, 116.0f, 22.0f};
}

// The effects the list offers, in its order: every type but None and the retired three-band compressor, whose place
// the three-band dynamics takes. They go by kind in six groups (kFxMenuGroups), each under its name.
constexpr FxType kFxOffered[] = {
    FxType::Shaper,     FxType::Amp,                             // drive
    FxType::Eq,         FxType::ParamEq,   FxType::Filter,       // EQ and filter
    FxType::Compressor, FxType::MultiDyn,                        // dynamics
    FxType::Delay,      FxType::Reverb,                          // space
    FxType::Phaser,     FxType::Flanger,   FxType::Chorus,       FxType::FreqShifter, // modulation
    FxType::Widener,    FxType::Disperser, FxType::Utility,      // stereo and phase
};
constexpr int kFxTypes = static_cast<int>(std::size(kFxOffered));
static_assert(kFxTypes == static_cast<int>(FxType::Count) - 2, "a new effect type goes in the list of effects");
constexpr int fxOfferedPlace(FxType type) {
    for (int i = 0; i < kFxTypes; ++i)
        if (kFxOffered[i] == type)
            return i;
    return 0;
}
// How many effects each group holds, in the list's order. The first kFxMenuLeftGroups go down its left column, the
// others down its right one.
constexpr int kFxMenuGroups[] = {2, 3, 2, 2, 4, 3};
constexpr int kFxMenuGroupCount = static_cast<int>(std::size(kFxMenuGroups));
constexpr int kFxMenuLeftGroups = 3;
constexpr int fxMenuGroupsTotal() {
    int n = 0;
    for (int g : kFxMenuGroups)
        n += g;
    return n;
}
static_assert(fxMenuGroupsTotal() == kFxTypes, "every effect the list offers is in one of its groups");
// The list's top left is under the button, or (from the shadow card after the effects) given. A row every 23 px, a
// group's name on a strip over its rows, the groups a little apart down two columns side by side.
constexpr float kFxMenuPitch = 23.0f;
constexpr float kFxMenuHead = 20.0f;
constexpr float kFxMenuGroupGap = 5.0f;
constexpr float kFxMenuColumn = 170.0f;
// From the main line, one more group under the left column's: the splits, in two bands over three, under a name too.
constexpr int kFxMenuSplitGroup = kFxMenuGroupCount;
constexpr int kFxMenuSplitRows = 2;
constexpr float fxMenuGroupH(int group) {
    const int rows = group == kFxMenuSplitGroup ? kFxMenuSplitRows : kFxMenuGroups[group];
    return kFxMenuHead + static_cast<float>(rows) * kFxMenuPitch;
}
// How far down its column group `group` starts (its name, then its rows): the left column's groups, then the splits',
// and the right column's.
constexpr float fxMenuGroupTop(int group) {
    const bool left = group < kFxMenuLeftGroups || group == kFxMenuSplitGroup;
    const int first = left ? 0 : kFxMenuLeftGroups, end = left ? std::min(group, kFxMenuLeftGroups) : group;
    float y = 0.0f;
    for (int g = first; g < end; ++g)
        y += fxMenuGroupH(g) + kFxMenuGroupGap;
    return y;
}
// How tall the columns are, the left one with the splits from the main line (`node`).
constexpr float fxMenuColumnsH(bool node) {
    const int lastLeft = node ? kFxMenuSplitGroup : kFxMenuLeftGroups - 1;
    const float left = fxMenuGroupTop(lastLeft) + fxMenuGroupH(lastLeft);
    const float right = fxMenuGroupTop(kFxMenuGroupCount - 1) + fxMenuGroupH(kFxMenuGroupCount - 1);
    return left > right ? left : right;
}
constexpr float fxMenuHeight(bool node) {
    return 8.0f + fxMenuColumnsH(node);
}
inline Rect fxMenu(Point at, bool node = false) {
    const Rect button = fxMenuButton();
    return {at.x < 0.0f ? button.x : at.x, at.x < 0.0f ? button.bottom() + 4.0f : at.y, 8.0f + 2.0f * kFxMenuColumn,
            fxMenuHeight(node)};
}
static_assert(9.0f + 22.0f + 4.0f + fxMenuHeight(true) <= kFxPanel.h, // from under the button
              "the list of effects fits in the panel");

// A group's name, over its rows (kFxMenuSplitGroup the splits'). Its strip is drawn 2 px short of the first row, as
// the rows are apart.
inline Rect fxMenuHead(Point at, int group) {
    const Rect menu = fxMenu(at);
    const bool right = group >= kFxMenuLeftGroups && group != kFxMenuSplitGroup;
    return {menu.x + 4.0f + (right ? kFxMenuColumn : 0.0f), menu.y + 4.0f + fxMenuGroupTop(group), kFxMenuColumn - 2.0f,
            kFxMenuHead};
}
inline Rect fxMenuItem(Point at, FxType type) {
    int group = 0, row = fxOfferedPlace(type);
    while (row >= kFxMenuGroups[group])
        row -= kFxMenuGroups[group++];
    const Rect head = fxMenuHead(at, group);
    return {head.x, head.bottom() + static_cast<float>(row) * kFxMenuPitch, head.w, kFxMenuPitch - 2.0f};
}
// Its entries in the splits' group: a split in two bands, and under it one in three.
inline Rect fxMenuNodeItem(Point at, int bands = 2) {
    const Rect head = fxMenuHead(at, kFxMenuSplitGroup);
    return {head.x, head.bottom() + (bands == 3 ? kFxMenuPitch : 0.0f), head.w, kFxMenuPitch - 2.0f};
}

// A shaper card's curve picker: a button with the curve's name across the card, and the list it opens under it;
// under its display LINK, which ties the Output knob to the drive, centered on the card; for a Custom curve SYM
// left of it, the two centered together. The reverb's row: its algorithm's button.
inline Rect clipModeButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}
inline Rect clipSymButton(Rect card) {
    return {card.center().x - 47.0f, card.y + 194.0f, 44.0f, 20.0f};
}
inline Rect clipLinkButton(Rect card, bool custom) {
    return {card.center().x + (custom ? 3.0f : -22.0f), card.y + 194.0f, 44.0f, 20.0f};
}
inline Rect reverbTypeButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}

inline Rect listMenu(Rect button, int count) {
    return {button.x, button.bottom() + 4.0f, button.w, 8.0f + count * 22.0f};
}

inline Rect listMenuItem(Rect button, int count, int item) {
    const Rect menu = listMenu(button, count);
    return {menu.x + 4.0f, menu.y + 4.0f + item * 22.0f, menu.w - 8.0f, 22.0f};
}

// The scrollbars under the card rows: thin tracks along the panels' bottom edges.
inline Rect fxScrollTrack() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + kFxPanel.h - 14.0f, kFxPanel.w - 32.0f, 6.0f};
}

inline Rect sourceScrollTrack() {
    return {kModPanel.x + 12.0f, kModPanel.y + kModPanel.h - 12.0f, kModPanel.w - 24.0f, 6.0f};
}

// The effect cards are cut off at the row's ends while they scroll.
// Room for a container's outline above and below them too.
inline Rect fxCardClip() {
    return {kFxPanel.x + 8.0f, kFxTop - kFxBoxPad - 2.0f, kFxPanel.w - 16.0f,
            kFxPanel.bottom() - 20.0f - kFxTop + 2.0f * kFxBoxPad + 4.0f};
}

// How many of the usual card places a card takes: the parametric EQ's and the three-band dynamics' are wider, for
// their displays and knobs, and the amp's wider still, its amp and its cabinet side by side.
inline float fxSpan(FxType type) {
    return type == FxType::Amp ? 1.85f : type == FxType::ParamEq || type == FxType::MultiDyn ? 1.5f : 1.0f;
}

// A card's rect by its screen place, in card places from the row's left end (0..kVisibleFx, fractions while
// it scrolls), and how many places it takes.
inline Rect fxCard(float position, float span = 1.0f) {
    return {kFxPanel.x + kFxRowLeft + position * (kFxCardW + kFxGap), kFxTop, span * (kFxCardW + kFxGap) - kFxGap,
            kFxPanel.bottom() - 20.0f - kFxTop};
}

// The effect's icon, at the left of its card's title row, folded or not.
inline Rect fxIconRect(Rect card) {
    return {card.x + kFxIconX, card.y + 10.0f, kFxIconW, 14.0f};
}

// Displays sit under a card's buttons, if it has any, and above its knobs: one
// row of them, or two for the delay, the parametric EQ and the effects with six knobs or so.
inline Rect ampToneDisplay(Rect card);
inline Rect fxDisplayIn(Rect card, FxType type) {
    if (type == FxType::Amp) // the amp's; the cabinet's beside it (see ampCabDisplay())
        return ampToneDisplay(card);
    if (type == FxType::MultiDyn) // its switches in the title row, its three rows of knobs under it
        return {card.x + 14.0f, card.y + 42.0f, card.w - 28.0f, 72.0f};
    const bool buttons = type == FxType::Shaper || type == FxType::Reverb;
    const bool buttonRow = buttons || type == FxType::Filter;
    const float top = buttonRow ? 74.0f : 44.0f;
    const bool twoRows = type == FxType::Delay || type == FxType::ParamEq || type == FxType::Compressor ||
                         type == FxType::Reverb || type == FxType::Phaser || type == FxType::Flanger ||
                         type == FxType::Chorus || type == FxType::Filter || type == FxType::FreqShifter ||
                         type == FxType::Utility;
    // The filter's slope switch, the delay's SYNC, LINK and PING-PONG, the phaser's and flanger's SYNC, STEREO and
    // polarity and the chorus's voices go under their displays.
    const bool switchesUnder = type == FxType::Phaser || type == FxType::Flanger || type == FxType::Chorus ||
                               type == FxType::Filter || type == FxType::Delay;
    // The shaper's LINK goes between its display and its knobs.
    const float bottom = switchesUnder               ? 158.0f
                         : twoRows                   ? 180.0f
                         : type == FxType::Shaper    ? 190.0f
                                                     : 198.0f;
    return {card.x + 14.0f, card.y + top, card.w - 28.0f, bottom - top};
}
// The chorus's voices under its display, in its middle: kChorusVoiceChoices side by side, `choice` the place of one.
inline Rect chorusVoicesButton(Rect display, int choice) {
    constexpr float kW = 22.0f;
    const float left = display.center().x - 0.5f * kW * static_cast<float>(std::size(kChorusVoiceChoices));
    return {left + kW * static_cast<float>(choice), display.bottom() + 4.0f, kW, 18.0f};
}
// Its display's lanes, under its readout, and the height of voice `voice` of `voices`'s lane a share `cycle` of the
// way through its sweep at Depth: up for a longer delay, the middle Delay, the edges as far as a voice's place and its
// sweep reach.
inline Rect chorusLanes(Rect display) {
    return {display.x + 8.0f, display.y + 22.0f, display.w - 16.0f, display.h - 30.0f};
}
inline float chorusLaneY(Rect lanes, int voice, int voices, double depth, double cycle) {
    const double lfo = std::sin(2.0 * std::numbers::pi * (cycle - chorusOffset(voice, voices)));
    const double off = chorusShare(voice, voices, depth, lfo) - 1.0;
    return lanes.center().y - static_cast<float>(off / (kChorusStagger + kChorusSweep)) * 0.5f * lanes.h;
}
// The amp's card: the amp on the left, the cabinet on the right, a faint line between. Each side: a list at the top
// (the model; the cabinet's power switch, then its list), a display, switches under it (STEREO; the mic's three), then
// two rows of knobs, three on the amp's side (Gain, Volume, Presence over Bass, Mid, Treble) and two on the cabinet's
// (Position, Distance over Output and Mix).
constexpr float kAmpGap = 12.0f;
inline Rect ampLeft(Rect card) {
    return {card.x + 14.0f, card.y, std::round((card.w - 28.0f - kAmpGap) * 0.53f), card.h};
}
inline Rect ampRight(Rect card) {
    const Rect left = ampLeft(card);
    return {left.right() + kAmpGap, card.y, card.right() - 14.0f - left.right() - kAmpGap, card.h};
}
inline float ampDivider(Rect card) {
    return ampLeft(card).right() + kAmpGap * 0.5f;
}
inline Rect ampModelButton(Rect card) {
    const Rect left = ampLeft(card);
    return {left.x, card.y + 42.0f, left.w, 22.0f};
}
inline Rect ampCabPower(Rect card) {
    const Rect right = ampRight(card);
    return {right.x, card.y + 42.0f, 22.0f, 22.0f};
}
inline Rect ampCabButton(Rect card) {
    const Rect right = ampRight(card);
    return {right.x + 26.0f, card.y + 42.0f, right.w - 26.0f, 22.0f};
}
inline Rect ampToneDisplay(Rect card) {
    const Rect left = ampLeft(card);
    return {left.x, card.y + 72.0f, left.w, 76.0f};
}
inline Rect ampCabDisplay(Rect card) {
    const Rect right = ampRight(card);
    return {right.x, card.y + 72.0f, right.w, 76.0f};
}
inline Rect ampStereoButton(Rect card) {
    return {ampLeft(card).x, card.y + 154.0f, 64.0f, 18.0f};
}
inline Rect ampMicChoice(Rect card, int mic) {
    const Rect right = ampRight(card);
    const float w = right.w / static_cast<float>(AmpMic::Count);
    return {right.x + mic * w, card.y + 154.0f, w, 18.0f};
}
constexpr float kAmpKnobD = 28.0f, kAmpKnobsY1 = 204.0f, kAmpKnobsY2 = 258.0f;
inline Point ampKnob(Rect card, bool cab, int column, int row) {
    const Rect side = cab ? ampRight(card) : ampLeft(card);
    const float columns = cab ? 2.0f : 3.0f;
    return {side.x + side.w * (column + 0.5f) / columns, card.y + (row == 0 ? kAmpKnobsY1 : kAmpKnobsY2)};
}
// The speaker on the cabinet's display, seen from the side at its left (the baffle, the cone behind it), and where the
// mic in front of it is: up from the cone's axis to its edge by Position, away to the right by Distance (0..1).
inline float ampBaffleX(Rect display) { return display.x + 18.0f; }
inline float ampConeAxis(Rect display) { return display.y + display.h * 0.5f + 5.0f; }
inline float ampConeRadius(Rect display) { return display.h * 0.36f; }
inline Point ampMicPoint(Rect display, double position, double distance) {
    // (Not `near` and `far`: Windows' headers define both as macros.)
    const float closest = ampBaffleX(display) + 10.0f, farthest = display.right() - 22.0f;
    return {closest + (farthest - closest) * static_cast<float>(std::clamp(distance, 0.0, 1.0)),
            ampConeAxis(display) - ampConeRadius(display) * static_cast<float>(std::clamp(position, 0.0, 1.0))};
}
inline Rect ampMicRect(Rect display, double position, double distance) {
    const Point m = ampMicPoint(display, position, distance);
    return {m.x - 9.0f, m.y - 8.0f, 22.0f, 16.0f};
}

// The utility's display: at its left the Gain strip, from silence at the bottom to 0 dB at the top, and beside it the
// stereo field, left to right, its frequency up from 20 Hz at the bottom to 20 kHz at the top. The sound's middle is
// across it by Pan, and it spreads either way a quarter of the field's width at 100 % Width.
constexpr int kUtilityHandles = 4;
constexpr FxParam kUtilityHandleParams[kUtilityHandles] = {FxParam::UtilityGain, FxParam::UtilityPan,
                                                           FxParam::UtilityWidth, FxParam::UtilityMonoBass};
inline Rect utilityStrip(Rect display) {
    return {display.x + 9.0f, display.y + 24.0f, 4.0f, display.h - 34.0f};
}
inline Rect utilityField(Rect display) {
    return {display.x + 26.0f, display.y + 24.0f, display.w - 36.0f, display.h - 34.0f};
}
inline float utilityMiddle(Rect field, double pan) {
    return field.center().x + static_cast<float>(std::clamp(pan, -1.0, 1.0)) * field.w * 0.5f;
}
inline float utilitySpread(Rect field, double width) { // how far the band reaches either way of the middle
    return field.w * 0.25f * static_cast<float>(std::max(width, 0.0));
}
// Width's handle sits on the band's edge on the side with more room, this far down the field, where the sound is wide.
constexpr float kUtilityWidthHandleY = 0.3f;

inline Rect fxDisplay(float position, FxType type) {
    return fxDisplayIn(fxCard(position, fxSpan(type)), type);
}

// The effects graph (UiLayout::fxGraphView) takes the card row's place: at the right the card of the node it shows (an
// effect's, a split's or a join's), `span` card places wide, and the graph's box left of it, as wide as the card
// leaves it, so a narrow card gives the graph more room and a wide one takes some. The switch between the chain and
// the graph sits at the panel's top right.
inline Rect fxInspectorCard(float span = 1.0f) {
    const float w = span * (kFxCardW + kFxGap) - kFxGap;
    return {std::round(kFxPanel.right() - 16.0f - w), kFxTop, w, kFxPanel.bottom() - 20.0f - kFxTop};
}
inline Rect fxGraphArea(float span = 1.0f) {
    const float right = fxInspectorCard(span).x - 16.0f;
    return {kFxPanel.x + 16.0f, kFxTop, right - kFxPanel.x - 16.0f, kFxPanel.bottom() - 20.0f - kFxTop};
}
// Under the graph's box, while it is wider than that: its scrollbar, along the box at the panel's bottom edge.
inline Rect fxGraphScrollTrack(float span = 1.0f) {
    const Rect area = fxGraphArea(span);
    return {area.x + 12.0f, kFxPanel.y + kFxPanel.h - 14.0f, area.w - 24.0f, 6.0f};
}
// Left of the switch, a button that removes every effect.
inline Rect fxClearButton() {
    return {kFxPanel.right() - 16.0f - 2.0f * 34.0f - 10.0f - 34.0f, kFxPanel.y + 9.0f, 34.0f, 22.0f};
}
inline Rect fxModeButton(int graph) {
    return {kFxPanel.right() - 16.0f - 2.0f * 34.0f + graph * 34.0f, kFxPanel.y + 9.0f, 34.0f, 22.0f};
}
// Its nodes sit on a fixed grid: IN in column 0, then a column a node, the main line in the middle row (0) and a
// split's high and low bands in the rows above and below it (-1, 1), OUT after the last column. The graph scrolls
// sideways when its columns reach past the area (`scroll`, in pixels). An effect is a rounded box, as a voice filter's
// or shaper's node is in the routing graph (its icon in it, its name under it), a split a box with its bands' ports on
// its right edge (the high band on top), a join a box as large, IN and OUT boxes, and the + where a node can go next a
// + in a circle.
constexpr float kFxGridCol = 100.0f, kFxGridRow = 80.0f, kFxGridLeft = 38.0f;
constexpr float kFxNodeR = 25.0f;     // an effect's box's half width, where its wires meet it
constexpr float kFxNodeHalfH = 18.0f; // and its half height
constexpr float kSoloedOutAlpha = 0.3f; // how strongly a band its join doesn't hear (the other soloed) is drawn
constexpr float kFxPlusIdleAlpha = 0.4f; // how strongly the graph's +s are drawn while the mouse isn't on them
constexpr float kFxSplitHalfW = 16.0f, kFxSplitHalfH = 28.0f;
// How far along a graph wire bends from one row to another: within the gap between two columns' nodes.
constexpr float kFxWireBend = kFxGridCol - 2.0f * kFxNodeR - 4.0f;
constexpr float kFxEndHalfW = 18.0f, kFxEndHalfH = 34.0f;
constexpr float kFxJoinHalfW = kFxSplitHalfW, kFxJoinHalfH = kFxSplitHalfH; // as large as a split
constexpr float kFxSlotHalf = 9.5f; // a + where a node can go next: a circle, as on a wire's middle
constexpr float kFxDropReach = 8.0f; // how near a wire (or its arrowhead) the mouse lands a dragged node where it goes
inline Point fxGridAt(float col, float row, float scroll) { // fractions of a cell too, as nodes glide
    const Rect area = fxGraphArea(); // its left edge and middle, whatever its width
    return {area.x + kFxGridLeft + col * kFxGridCol - scroll, area.center().y + row * kFxGridRow};
}
inline Point fxGridCell(int col, int row, float scroll) {
    return fxGridAt(static_cast<float>(col), static_cast<float>(row), scroll);
}
// A split's band port: on its right edge, the high band (2) at the top, the low one (0) at the bottom.
inline Point fxBandPortAt(Point split, int band) {
    return {split.x + kFxSplitHalfW, split.y + (1 - band) * (kFxSplitHalfH - 10.0f)};
}

// The three-band dynamics' display: a level bar a band, the high band on top, each its name on the left and its gain
// on the right, with a scale in dB under them. Levels run from -kMdRangeDb at the left to 0 dB at the right.
constexpr float kMdRangeDb = static_cast<float>(-kMdThresholdMinDb);
inline Rect mdBar(Rect display, int band) {
    const int row = kMcBands - 1 - band;
    return {display.x + 38.0f, display.y + 7.0f + 17.0f * static_cast<float>(row), display.w - 78.0f, 12.0f};
}
inline float mdBarX(Rect bar, double db) {
    return bar.x + static_cast<float>(std::clamp((db + kMdRangeDb) / kMdRangeDb, 0.0, 1.0)) * bar.w;
}
// A band's row, its name to its gain: a click picks the band its knobs set.
inline Rect mdBandRow(Rect display, int band) {
    const Rect bar = mdBar(display, band);
    return {display.x + 2.0f, bar.y - 2.5f, display.w - 4.0f, bar.h + 5.0f};
}
// Where a band's threshold lines take the mouse: 8 px around each, down its row, `below` true for the Below
// threshold's. Two lines closer than that share the room between them, the Below line's to the left of their middle
// and the Above line's to the right, so each can still be dragged its own way.
inline Rect mdLineGrip(Rect bar, double belowDb, double aboveDb, bool below) {
    const float xBelow = mdBarX(bar, belowDb), xAbove = mdBarX(bar, aboveDb), middle = 0.5f * (xBelow + xAbove);
    const float left = below ? xBelow - 4.0f : std::max(xAbove - 4.0f, middle);
    const float right = below ? std::min(xBelow + 4.0f, middle) : xAbove + 4.0f;
    return {left, bar.y - 2.5f, right - left, bar.h + 5.0f};
}
// A band's zones, where it acts: left of its Below threshold's line (`below` true), where it lifts, and right of its
// Above threshold's, where it pulls down; dragged up and down they set that side's ratio. They stop short of the
// lines' grips (see mdLineGrip()).
inline Rect mdZone(Rect bar, double belowDb, double aboveDb, bool below) {
    const float left = below ? bar.x : mdBarX(bar, aboveDb) + 4.0f;
    const float right = below ? mdBarX(bar, belowDb) - 4.0f : bar.right();
    return {left, bar.y - 2.5f, std::max(right - left, 0.0f), bar.h + 5.0f};
}
// The meter in a band's bar, half as tall, down its middle, and the most ratio lines a zone draws.
constexpr float kMdMeterH = 6.0f;
constexpr int kMdRatioLines = 7;

// Its switches in the title row, left of the fold and remove buttons: RMS, then LINK.
// Its rows of knobs: the band's own (Above, Down, Below, Up, Gain), a line, then the shared ones in two rows; RMS and
// LINK at the line's right end (the title row holding only what every card's does).
constexpr float kMdBandKnobsY = 142.0f, kMdLineY = 186.5f, kMdSharedY1 = 214.0f, kMdSharedY2 = 266.0f;
inline Rect mdRmsButton(Rect card) { return {card.right() - 14.0f - 30.0f - 34.0f, card.y + kMdLineY - 9.5f, 34.0f, 19.0f}; }
inline Rect mdLinkButton(Rect card) { return {card.right() - 14.0f - 30.0f, card.y + kMdLineY - 9.5f, 30.0f, 19.0f}; }

// The parametric EQ's plot: frequency across, gain up and down.
constexpr float kPeqRangeDb = 20.0f;
inline Point peqAt(Rect area, double freq, double db) {
    return {area.x + static_cast<float>(freq) * area.w,
            area.center().y - static_cast<float>(db) / kPeqRangeDb * area.h * 0.5f};
}

// The bands' chips, in a row under the display: each its power switch, its number and its shape. A click picks the
// band the row of knobs under them sets.
constexpr float kPeqChipY = 188.0f, kPeqChipH = 26.0f;
inline Rect peqChip(Rect card, int band) {
    const float w = (card.w - 28.0f - 4.0f * (kPeqBands - 1)) / kPeqBands;
    return {card.x + 14.0f + band * (w + 4.0f), card.y + kPeqChipY, w, kPeqChipH};
}
inline Rect peqChipPower(Rect chip) { return {chip.x, chip.y, 26.0f, chip.h}; } // where the power symbol is drawn
inline Rect peqChipShape(Rect chip) { return {chip.right() - 34.0f, chip.y, 34.0f, chip.h}; }
// What switches the band on and off: the power symbol and the number, everything left of the shape.
inline Rect peqChipSwitch(Rect chip) { return {chip.x, chip.y, peqChipShape(chip).x - chip.x, chip.h}; }
// Its row of knobs under the chips, as large as a one-row card's: the band picked's Freq, Gain and Q, then Amount.
constexpr float kPeqKnobsY = 256.0f, kPeqKnobD = 36.0f;
inline Point peqKnobCenter(Rect card, int i) {
    return {card.x + card.w * (static_cast<float>(i) + 0.5f) / 4.0f, card.y + kPeqKnobsY};
}

// The list of band shapes, which opens upward from its chip over the card's display, inside the card.
constexpr int kPeqTypes = static_cast<int>(PeqType::Count);
inline Rect peqMenu(Rect card, int band) {
    constexpr float kH = 8.0f + kPeqTypes * 22.0f, kW = 124.0f;
    const Rect chip = peqChip(card, band);
    const float x = std::clamp(chip.center().x - kW * 0.5f, card.x + 8.0f, card.right() - 8.0f - kW);
    return {x, chip.y - 4.0f - kH, kW, kH};
}

inline Rect peqMenuItem(Rect card, int band, int type) {
    const Rect menu = peqMenu(card, band);
    return {menu.x + 4.0f, menu.y + 4.0f + type * 22.0f, menu.w - 8.0f, 22.0f};
}

// The keyboard strip under the modulation panel, `top` down: along its top the collapse toggle with the strip's
// title, the macros, the note source and velocity, and under them the wheels on the left and six octaves of keys, the octave shift under their right end. The wheels' handles sit above them, under the title, within the top
// row. Collapsed, only the top row shows.
constexpr float kKeyboardH = 192.0f;
// The top row, the names under it included: all that shows while collapsed, the wheels' names under their handles
// (see kWheelHandleY) clear of the strip's lower edge.
constexpr float kKeyboardBarH = 80.0f;
constexpr Rect kKeys{130.0f, 76.0f, 1046.0f, 98.0f}; // relative to the strip's top
constexpr float kStripRow = 29.0f;                   // the middle of the row above the keys, below the strip's top
inline float keyboardTop(bool sourcesCollapsed) {
    return modPanel(sourcesCollapsed).bottom() + 16.0f;
}
inline Rect keyboardPanel(float top, bool collapsed) {
    return {16.0f, top, 1168.0f, collapsed ? kKeyboardBarH : kKeyboardH};
}
// The chevron and the title, as far from the strip's top as the modulation panel's from its.
inline Rect keysToggle(float top) {
    const Rect sources = sourcesToggle();
    return {sources.x, top + sources.y - kModPanel.y, sources.w, sources.h};
}
// The macros in a row above the keys, close together from the keys' left end: a knob each, its name under it
// (double-click to rename), and left of it a handle to drag onto what it moves, as with a source's.
constexpr float kMacroCell = 96.0f;
inline Point macroKnob(float keyboardTop, int macro) {
    return {kKeys.x + (macro + 0.5f) * kMacroCell + 10.0f, keyboardTop + kStripRow};
}
// The note source and velocity, after the macros, velocity at the row's right end: each a display of what it
// follows, its name under it, and left of it its handle, level with the macros'.
inline Rect noteDisplay(float keyboardTop) {
    return {992.0f, keyboardTop + kStripRow - 12.0f, 68.0f, 24.0f};
}
inline Rect noteHandle(float keyboardTop) {
    return {966.0f, keyboardTop + kStripRow - 10.0f, 20.0f, 20.0f};
}
inline Rect velocityDisplay(float keyboardTop) {
    return {1108.0f, keyboardTop + kStripRow - 12.0f, 68.0f, 24.0f};
}
inline Rect velocityHandle(float keyboardTop) {
    return {1082.0f, keyboardTop + kStripRow - 10.0f, 20.0f, 20.0f};
}
// The display of the note source or velocity, and its handle.
inline Rect stripDisplay(float keyboardTop, bool velocity) {
    return velocity ? velocityDisplay(keyboardTop) : noteDisplay(keyboardTop);
}
inline Rect bendWheel(float top) {
    return {34.0f, top + kKeys.y, 26.0f, 90.0f};
}
inline Rect modWheel(float top) {
    return {74.0f, top + kKeys.y, 26.0f, 90.0f};
}
// The wheels' handles, to drag onto what they move, as a macro's: each over its wheel, below the strip's title, within
// its top row, so they stay while the keys are folded (their names then under them).
constexpr float kWheelHandleY = 50.0f; // their middle, below the strip's top
inline Rect wheelHandle(float top) {
    const Rect wheel = modWheel(top);
    return {wheel.center().x - 10.0f, top + kWheelHandleY - 10.0f, 20.0f, 20.0f};
}
inline Rect bendHandle(float top) {
    const Rect wheel = bendWheel(top);
    return {wheel.center().x - 10.0f, top + kWheelHandleY - 10.0f, 20.0f, 20.0f};
}
constexpr int kKeyCount = 73; // C to C, six octaves
constexpr int kWhiteKeys = 43;
constexpr float kWhiteW = kKeys.w / kWhiteKeys;
constexpr float kBlackW = kWhiteW * 0.6f;
constexpr float kBlackH = kKeys.h * 0.6f;
constexpr int kBaseNote = 24; // C1, where the keyboard starts before any octave shift
// Init, Load, Save, Save As and the settings cog: icons at the right end of the title bar.
inline Rect toolbarButton(int i) {
    constexpr float kW = 28.0f, kGap = 6.0f;
    return {kFxPanel.right() - (5 - i) * (kW + kGap) + kGap, kTitleRowY, kW, kTopRowH};
}
// Undo and redo, a little apart to their left.
inline Rect undoButton(int redo) {
    const Rect init = toolbarButton(0);
    return {init.x - 14.0f - (2 - redo) * 34.0f, init.y, 28.0f, init.h};
}
// The preset bar, in the middle of the title bar: back, the preset's name, forward.
constexpr Rect kPresetName{430.0f, kTitleRowY, 340.0f, kTopRowH};
inline Rect presetStep(int direction) {
    return {direction < 0 ? kPresetName.x - 30.0f : kPresetName.right() + 4.0f, kPresetName.y, 26.0f, kPresetName.h};
}
// The tempo, for a host that lets the editor set it (see Controller::hasTempo()): a field midway between the name and
// the preset bar's back arrow, a metronome at its left.
constexpr Rect kTempoBox{215.0f, kTitleRowY, 96.0f, kTopRowH};
// The octave shift: two small buttons under the keys' right end, the keys' range between them.
inline Rect octaveButton(int dir, float top) {
    return {dir < 0 ? 1094.0f : 1162.0f, top + kKeys.y + kKeys.h + 2.0f, 14.0f, 12.0f};
}

// Within an octave, the white keys (C D E F G A B) and the black keys (C# D# F# G# A#).
constexpr int kWhiteSlot[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
constexpr int kBlackAfter[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};
constexpr bool isBlackKey(int key) {
    return kBlackAfter[key % 12] >= 0;
}

// Position in the grid: rows are nodes, columns nodes and then the output.
inline Rect gridCell(int row, int col) {
    return {kGridOrigin.x + col * kCellStep, kGridOrigin.y + row * kCellStep, kCell, kCell};
}

inline Rect knobRect(Point center, float diameter) {
    return {center.x - diameter * 0.5f, center.y - diameter * 0.5f, diameter, diameter + 18.0f};
}

inline Rect chip(Point center, float width = 38.0f) {
    return {center.x - width * 0.5f, center.y - 9.0f, width, 18.0f};
}

inline Point normalize(Point v) {
    const float length = std::hypot(v.x, v.y);
    return length > 1e-4f ? Point{v.x / length, v.y / length} : Point{0.0f, 1.0f};
}

inline float knobAngle(double norm) {
    return kKnobStart + static_cast<float>(std::clamp(norm, 0.0, 1.0)) * kKnobSweep;
}

// The partial editor, over the routing panel: the wave the partials make, their levels, and their phases.
// Each row's label sits between its box and the one above, 32 apart.
constexpr Rect kPartialWave{kRouting.x + 40.0f, kRouting.y + 44.0f, kRouting.w - 80.0f, 106.0f};
constexpr Rect kPartialLevels{kRouting.x + 40.0f, kRouting.y + 182.0f, kRouting.w - 80.0f, 84.0f};
constexpr Rect kPartialPhases{kRouting.x + 40.0f, kRouting.y + 298.0f, kRouting.w - 80.0f, 48.0f};
// The bars share the width out among the `count` partials the wave plays, closer together the more there are.
inline Rect partialBar(Rect area, int partial, int count) {
    const float step = area.w / count;
    const float gap = std::min(3.0f, step * 0.125f);
    return {area.x + partial * step + gap, area.y, step - 2.0f * gap, area.h};
}
inline int partialAt(Rect area, float x, int count) {
    return std::clamp(static_cast<int>((x - area.x) / (area.w / count)), 0, count - 1);
}
// The resolution switches in the title row, after the title: 16, 32 and 64 partials.
inline Rect partialCountButton(int choice) {
    return {kRouting.x + 144.0f + choice * 36.0f, kRouting.y + 9.0f, 34.0f, 22.0f};
}
// The level bars run in dB, from 0 dB at the top down to the floor just above the bottom, which is none. The
// levels themselves stay amplitudes.
constexpr double kPartialFloorDb = -60.0;
inline double partialLevelFromBar(double bar) {
    return bar <= 0.0 ? 0.0 : std::pow(10.0, (1.0 - std::min(bar, 1.0)) * kPartialFloorDb / 20.0);
}
inline double partialBarFromLevel(double level) {
    return level <= 0.0 ? 0.0 : std::clamp(1.0 - 20.0 * std::log10(level) / kPartialFloorDb, 0.0, 1.0);
}

// The settings dialog, centered over the editor: OVERSAMPLING, then MPE (kSettingsMpeY down), then LOOK
// (kSettingsLookY down), each with its heading, its buttons and its lines under them.
constexpr float kSettingsW = 380.0f;
constexpr float kSettingsH = 594.0f;
constexpr float kSettingsMpeY = 184.0f;  // the divider above MPE
constexpr float kSettingsLookY = 312.0f; // and above LOOK
constexpr int kOversamplingFactors[] = {1, 2, 4, 8};
constexpr int kMpeBendRanges[] = {12, 24, 48, 96}; // semitones
inline Rect settingsPanel(float viewHeight) {
    return {(View::kWidth - kSettingsW) * 0.5f, std::round((viewHeight - kSettingsH) * 0.5f), kSettingsW, kSettingsH};
}
inline Rect settingsOption(Rect panel, int i) {
    return {panel.x + 24.0f + i * 64.0f, panel.y + 82.0f, 58.0f, 28.0f};
}
// MPE's Off (0) and On (1), then BEND and its ranges on the same row.
inline Rect settingsMpe(Rect panel, int on) {
    return {panel.x + 24.0f + on * 56.0f, panel.y + kSettingsMpeY + 30.0f, 52.0f, 28.0f};
}
inline Rect settingsMpeBendLabel(Rect panel) {
    return {panel.x + 146.0f, panel.y + kSettingsMpeY + 30.0f, 44.0f, 28.0f};
}
inline Rect settingsMpeRange(Rect panel, int i) {
    return {panel.x + 190.0f + i * 42.0f, panel.y + kSettingsMpeY + 30.0f, 38.0f, 28.0f};
}
inline Rect settingsTheme(Rect panel, int i) {
    const float w = (panel.w - 48.0f + 6.0f) / kThemeCount;
    return {panel.x + 24.0f + i * w, panel.y + kSettingsLookY + 30.0f, w - 6.0f, 30.0f};
}
// Under the themes, the hue slider's track, with room above and below to grab it: the theme's main color. HUE before
// it, its hue after.
inline Rect settingsHue(Rect panel) {
    return {panel.x + 76.0f, panel.y + kSettingsLookY + 72.0f, 200.0f, 26.0f};
}
// The glow slider, like it; GLOW before it, its amount after.
inline Rect settingsGlow(Rect panel) {
    return {panel.x + 76.0f, panel.y + kSettingsLookY + 104.0f, 200.0f, 26.0f};
}
// Under it, the LINES slider, like it: how strongly a source's dashed lines are drawn.
inline Rect settingsLinks(Rect panel) {
    return {panel.x + 76.0f, panel.y + kSettingsLookY + 136.0f, 200.0f, 26.0f};
}
// Under that, HANDLES and the depth handles' sizes: small, medium and large.
inline Rect settingsHandles(Rect panel, int size) {
    return {panel.x + 76.0f + size * 68.0f, panel.y + kSettingsLookY + 170.0f, 64.0f, 28.0f};
}
inline Rect settingsClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}

// The dialog asking before Init or Load replaces a changed patch, or Save overwrites its preset, centered like the settings; a line shorter when
// it doesn't say what replaces the patch, and before Save overwrites a preset.
constexpr float kConfirmW = 360.0f;
constexpr float kConfirmH = 150.0f;
constexpr float kConfirmLine = 18.0f;
inline Rect confirmPanel(float viewHeight, bool shorter) {
    const float h = kConfirmH - (shorter ? kConfirmLine : 0.0f);
    return {(View::kWidth - kConfirmW) * 0.5f, std::round((viewHeight - h) * 0.5f), kConfirmW, h};
}
// 1 goes ahead, 0 cancels.
inline Rect confirmButton(Rect panel, int ok) {
    return {panel.right() - 24.0f - (ok ? 80.0f : 166.0f), panel.bottom() - 24.0f - 28.0f, 80.0f, 28.0f};
}
// A third choice, left of Cancel: a split removed with its bands' effects (Delete), where 1 keeps them (Keep).
inline Rect confirmThirdButton(Rect panel) {
    return {panel.right() - 24.0f - 252.0f, panel.bottom() - 24.0f - 28.0f, 80.0f, 28.0f};
}
// Asking where a split's middle band's effects go, back in two bands: a row more, of a button for each band they can go
// onto (2 the low band's end, 3 the high band's start) over Remove (confirmButton()'s 1) and Cancel.
inline Rect confirmBandsPanel(float viewHeight) {
    const float h = kConfirmH + 20.0f;
    return {(View::kWidth - kConfirmW) * 0.5f, std::round((viewHeight - h) * 0.5f), kConfirmW, h};
}
inline Rect confirmBandButton(Rect panel, int choice) {
    const float w = (panel.w - 48.0f - 8.0f) * 0.5f;
    return {panel.x + 24.0f + (choice == 3 ? w + 8.0f : 0.0f), panel.y + 74.0f, w, 28.0f};
}

// The preset browser, centered over the editor: a title row, the list, and the folder at the foot.
constexpr float kBrowserW = 440.0f;
constexpr float kBrowserRowH = 24.0f;
inline Rect browserPanel(float viewHeight) {
    const float h = std::min(560.0f, viewHeight - 120.0f);
    return {(View::kWidth - kBrowserW) * 0.5f, std::round((viewHeight - h) * 0.5f), kBrowserW, h};
}
inline Rect browserList(Rect panel) {
    return {panel.x + 12.0f, panel.y + 58.0f, panel.w - 24.0f, panel.h - 58.0f - 54.0f};
}
inline int browserVisibleRows(Rect panel) {
    return static_cast<int>(browserList(panel).h / kBrowserRowH);
}
inline Rect browserClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}
inline Rect browserFolder(Rect panel) {
    return {panel.right() - 24.0f - 110.0f, panel.bottom() - 40.0f, 110.0f, 26.0f};
}
// The search field, in the title row left of the close button, and the x at its right end that empties it.
inline Rect browserSearch(Rect panel) {
    return {panel.right() - 46.0f - 180.0f, panel.y + 14.0f, 180.0f, 24.0f};
}
inline Rect browserClear(Rect panel) { return fieldClear(browserSearch(panel)); }
inline Rect browserTyped(Rect panel) { return fieldTyped(browserSearch(panel)); }

} // namespace fmvibe::gui::detail
