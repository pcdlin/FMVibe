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
// The editor's band the panels' row keeps to, under the header: the routing panel or what takes its place, and the
// inspector, or the full-size editors over them (see View::poll(), which repaints what moves there).
constexpr Rect kPanelsBand{0.0f, kHeaderH, View::kWidth, kRouting.y + kRouting.h - kHeaderH};
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
// the panel's top, then the rows, the last one 14 px above its bottom. Its cells are kCellAspect times as wide as
// they are tall, and the legend takes what is left of the panel at their right.
constexpr float kMatrixMargin = 14.0f;
constexpr float kMatrixNamesH = 22.0f; // the column names' row, over the first row of cells
constexpr float kCellGap = 4.0f;
constexpr float kCellAspect = 1.5f; // a cell's width over its height
constexpr float kCellStepY = (kRouting.h - 2.0f * kMatrixMargin - kMatrixNamesH + kCellGap) / kNodes;
constexpr float kCellH = kCellStepY - kCellGap;
constexpr float kCellW = kCellH * kCellAspect;
constexpr float kCellStepX = kCellW + kCellGap;
constexpr Point kGridOrigin{kRouting.x + 56.0f, kRouting.y + kMatrixMargin + kMatrixNamesH};
constexpr float kLegendX = kGridOrigin.x + kGridCols * kCellStepX - kCellGap + 28.0f;
constexpr float kLegendW = kRouting.x + kRouting.w - 16.0f - kLegendX;
static_assert(kLegendW >= 150.0f, "the matrix's legend needs this much of the panel beside the cells");
// The inspector's tabs, one per node along its top: the operators', the filters' and the shapers' in three groups, a
// hairline between two groups (kNodeTabDivider()), on a band of their own across the lens's top, parted from the page
// under it by a hairline.
constexpr float kNodeTabStep = 24.0f, kNodeTabGroupGap = 13.0f;
// The band's height, the tabs in its middle; the title row's switches (kTitleRow) stand clear of its hairline.
constexpr float kNodeTabBandH = 32.0f;
inline Rect nodeTabRect(int node) {
    const int group = node < kOps ? 0 : node < kOps + kFilters ? 1 : 2;
    return {kInspector.x + 16.0f + node * kNodeTabStep + group * kNodeTabGroupGap,
            kInspector.y + 0.5f * (kNodeTabBandH - 22.0f), kNodeTabStep - 1.0f, 22.0f};
}
// The dot in a node's tab's upper right while the node sounds out (kTabDotRadius).
inline Point nodeTabDot(int node) {
    const Rect r = nodeTabRect(node);
    return {r.x + r.w - 4.0f, r.y + 4.0f};
}
// The hairline before group 1 (the filters) or 2 (the shapers), in the middle of the gap.
inline float nodeTabDivider(int group) {
    const int first = group == 1 ? kOps : kOps + kFilters;
    return nodeTabRect(first).x - 0.5f * (kNodeTabGroupGap + 1.0f);
}
// The page below the tabs, title and switch, dimmed while its node is off.
constexpr Rect kInspectorPage{kInspector.x + 1.0f, kInspector.y + 60.0f, kInspector.w - 2.0f, kInspector.h - 61.0f};
// The page's title row under the tabs: the node's letter, then its summary; the switches at its right end.
constexpr Rect kTitleRow{kInspector.x + 16.0f, kInspector.y + 37.0f, kInspector.w - 32.0f, 20.0f};
constexpr float kTitleLetterW = 16.0f;
// An operator's letter there is its output's handle as a modulation source: dragged onto what it should modulate.
constexpr Rect kOpNameHandle{kTitleRow.x - 4.0f, kTitleRow.y, kTitleLetterW + 8.0f, kTitleRow.h};
// An operator's Fixed switch, a keyboard on the summary line left of the power switch.
constexpr Rect kFixedSwitch{kInspector.x + kInspector.w - 16.0f - 26.0f - 30.0f, kInspector.y + 37.0f, 26.0f, 19.0f};
// An operator's sound in one block under the title: at its left, its pitch (the Ratio readout, Freq and Mult when
// fixed, or a noise operator's Slope and Spread), as tall as two rows; at its right, the waves and under them the
// Pitch, Detune, Phase and Pan knobs, their labels on one line (the Ratio readout's under its box, kRatioLabelY).
constexpr float kOpHeroY = kInspector.y + 66.0f; // between the title row and the tabs
constexpr float kOpPitchW = 88.0f; // the left column
constexpr float kOpRightX = kInspector.x + 16.0f + kOpPitchW + 12.0f;
constexpr float kOpRightW = kInspector.x + kInspector.w - 16.0f - kOpRightX;
constexpr float kOpWavesH = 20.0f; // the waves' row, over the knobs
// The knobs under the waves, their modulation rings (out to 5 px past a knob's edge while one's depth handle is under
// the mouse, see View::paintKnob()) 3 px clear of the row.
constexpr float kOpKnobD = 28.0f;
constexpr float kOpKnobY = kOpHeroY + kOpWavesH + 3.0f + 5.0f + 0.5f * kOpKnobD;
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
// Its top a little under the waves' row's, and its label as far under it as a knob's is under the knob.
constexpr Rect kRatioBox{kInspector.x + 16.0f, kOpHeroY + 4.0f, kOpPitchW, 46.0f};
constexpr float kOpLabelY = kOpKnobY + 0.5f * kOpKnobD + 2.0f; // the top of the knobs' labels' line
constexpr float kRatioLabelY = kRatioBox.y + kRatioBox.h + 2.0f; // the top of the Ratio readout's label
constexpr int kRatioDigits = 4; // tens, ones, tenths, hundredths
constexpr long kRatioPlaces[kRatioDigits] = {1000, 100, 10, 1}; // in hundredths
constexpr float kRatioDigitW = 15.0f, kRatioDigitH = 21.0f, kRatioDigitGap = 2.5f, kRatioPointW = 6.5f;
// Digit k's place in the box (the decimal point after the second).
inline Rect ratioDigitRect(int k) {
    const float total = kRatioDigits * kRatioDigitW + (kRatioDigits - 1) * kRatioDigitGap + kRatioPointW;
    const float left = kRatioBox.x + (kRatioBox.w - total) * 0.5f;
    const float x = left + k * (kRatioDigitW + kRatioDigitGap) + (k >= 2 ? kRatioPointW : 0.0f);
    return {x, kRatioBox.y + 12.0f, kRatioDigitW, kRatioDigitH};
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
// The edit button in a Custom LFO's display, a pencil in its top right corner.
constexpr float kEditButtonW = 24.0f, kEditButtonH = 16.0f;
inline Rect lfoEditButton(Rect display) {
    return {display.right() - 4.0f - kEditButtonW, display.y + 4.0f, kEditButtonW, kEditButtonH};
}
// An operator's page: under its block, a line across, then a well whose top edge holds the tabs (one per
// InspectorTab) and whose display under them shows what the tab picks; the tab's controls in a row under the well.
constexpr float kOpRuleY = kInspector.y + 144.0f;
constexpr float kOpTabsY = kInspector.y + 150.0f;
constexpr float kOpTabH = 22.0f;
// An even height keeps the envelope's levels (from 12 px under its top to 10 px over its bottom) on whole pixels at 0.5.
constexpr Rect kOpDisplay{kInspector.x + 16.0f, kOpTabsY + kOpTabH, kInspector.w - 32.0f, 116.0f};
// The well, the tabs and the display; INPUTS, with no controls under it, takes it down to the lens's bottom.
constexpr Rect kOpWell{kOpDisplay.x, kOpTabsY, kOpDisplay.w, kOpDisplay.y + kOpDisplay.h - kOpTabsY};
constexpr Rect kOpWellTall{kOpDisplay.x, kOpTabsY, kOpDisplay.w, kInspector.y + kInspector.h - 10.0f - kOpTabsY};
constexpr float kOpControlsY = kOpDisplay.y + kOpDisplay.h + 25.0f; // the controls' knobs' centers
inline Rect opTab(int tab, bool noise = false) {
    // UNISON, ENVELOPE, SCOPE, INPUTS, WARP, KEYS by InspectorTab; along the line ENVELOPE, UNISON, WARP, INPUTS, KEYS,
    // a fifth of the well each. SCOPE is no longer a tab of its own (the WARP tab shows the scope; a layout saved on it
    // opens WARP): it sits where WARP does. A noise operator's are ENVELOPE, SPECTRUM (in UNISON's place), INPUTS and
    // KEYS, a quarter each.
    constexpr int kPlace[] = {1, 0, 2, 3, 2, 4}, kNoisePlace[] = {1, 0, 2, 2, 2, 3};
    const float w = (kOpWell.w - 8.0f) / (noise ? 4.0f : 5.0f);
    return {kOpWell.x + 4.0f + (noise ? kNoisePlace : kPlace)[tab] * w, kOpTabsY, w, kOpTabH};
}
// The ring in an operator page tab's upper right while one of its controls is modulated (kTabRingRadius).
inline Point opTabRing(int tab, bool noise = false) {
    const Rect r = opTab(tab, noise);
    return {r.x + r.w - 4.0f, r.y + 4.0f}; // above the name's caps, which a wide name (ENVELOPE) runs out under

}
// Wave: its display (the scope; with no note, the operator's cycles as its warp reads them, or the note's when
// synced, the cycle buttons in its top right corner), then the warps' buttons, the amount's knob and the NOTE
// (note sync) switch along the row under it.
constexpr Rect kScopeRect = kOpDisplay;
inline Rect warpButton(int warp) {
    return {kInspector.x + 16.0f + warp * 26.0f, kOpControlsY - 11.0f, 26.0f, 22.0f};
}
constexpr Point kWarpAmountKnob{kInspector.x + 210.0f, kOpControlsY};
constexpr Rect kSyncButton{kInspector.x + kInspector.w - 16.0f - 32.0f, kOpControlsY - 11.0f, 32.0f, 22.0f};
// Keys: keyboard level scaling's display, the keys across (MIDI 0 to 127) and the level up and down (kKeyScaleTopDb at
// its top, kKeyScaleBottomDb at its bottom), each side's curve switch in its top corners; the breakpoint's and the
// depths' knobs in the row under the well.
constexpr Rect kKeyScaleRect = kOpDisplay;
constexpr float kKeyScaleTopDb = 24.0f, kKeyScaleBottomDb = -48.0f;
inline Rect keyScalePlot() {
    return {kKeyScaleRect.x + 10.0f, kKeyScaleRect.y + 25.0f, kKeyScaleRect.w - 20.0f, kKeyScaleRect.h - 37.0f};
}
inline float keyScaleX(double key) {
    const Rect r = keyScalePlot();
    return r.x + static_cast<float>(std::clamp(key, 0.0, 127.0) / 127.0) * r.w;
}
inline double keyScaleKey(float x) { // the key at x, held to the keys
    const Rect r = keyScalePlot();
    return std::clamp(static_cast<double>((x - r.x) / r.w) * 127.0, 0.0, 127.0);
}
inline float keyScaleY(double db) {
    const Rect r = keyScalePlot();
    const double held = std::clamp(db, static_cast<double>(kKeyScaleBottomDb), static_cast<double>(kKeyScaleTopDb));
    return r.y + static_cast<float>((kKeyScaleTopDb - held) / (kKeyScaleTopDb - kKeyScaleBottomDb)) * r.h;
}
inline double keyScaleDbAt(float y) {
    const Rect r = keyScalePlot();
    return kKeyScaleTopDb - static_cast<double>((y - r.y) / r.h) * (kKeyScaleTopDb - kKeyScaleBottomDb);
}
// A side's curve switch (0 the left side's, 1 the right's): EXP, lit while it is exponential.
inline Rect keyCurveButton(int side) {
    return {side == 0 ? kKeyScaleRect.x + 6.0f : kKeyScaleRect.right() - 6.0f - 34.0f, kKeyScaleRect.y + 5.0f, 34.0f,
            16.0f};
}
// Break, L Depth, R Depth and Rate Scaling (the Rate/Level envelope's, keyboard scaling on a DX7), a quarter each.
inline Point keyScaleKnob(int knob) {
    return {kOpDisplay.x + kOpDisplay.w * (knob + 0.5f) / 4.0f, kOpControlsY};
}
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
// Envelope: the curve on the whole display; in the row under the well, at its left, ADSR over Curve over Rate/Level
// (their icons, one pill), then the AHDSR's five knobs, or for a drawn envelope One-Shot | Sustain, its sync switch,
// its Length and, at the right, the pencil that opens its full-size editor, with its Slew between them, or for a
// Rate/Level envelope 1 | 2 | 3 | 4, which picks the segment, then that segment's rate and level (its Rate Scaling
// is on the KEYS tab, as keyboard scaling).
constexpr Rect kOpEnvRect = kOpDisplay;
inline Rect opEnvModeButton(int mode) { // 0 ADSR, over 1 Curve, over 2 Rate/Level, in the same place for each
    return {kOpDisplay.x, kOpDisplay.y + kOpDisplay.h + 4.0f + mode * 18.0f, 26.0f, 18.0f};
}
inline Rect rateSegmentButton(int segment) { // one pill, right of the modes
    return {kOpDisplay.x + 36.0f + static_cast<float>(segment) * 22.0f, kOpControlsY - 11.0f, 22.0f, 22.0f};
}
inline Point opRateKnob(int knob) { // 0 the segment's rate, 1 its level, right of the segments
    constexpr float left = kOpDisplay.x + 36.0f + 4.0f * 22.0f + 4.0f, step = (kOpDisplay.x + kOpDisplay.w - left) / 2.0f;
    return {left + step * (static_cast<float>(knob) + 0.5f), kOpControlsY};
}
// A Rate/Level envelope's plot in the display, as the AHDSR's (see rateShapeOf()).
constexpr Rect kOpRatePlot{kOpDisplay.x + 10.0f, kOpDisplay.y + 14.0f, kOpDisplay.w - 20.0f, kOpDisplay.h - 24.0f};
inline Point opEnvKnob(int knob) { // the AHDSR's: Attack, Hold, Decay, Sustain, Release, right of ADSR | Curve
    constexpr float left = kOpDisplay.x + 32.0f, step = (kOpDisplay.x + kOpDisplay.w - left) / 5.0f;
    return {left + step * (static_cast<float>(knob) + 0.5f), kOpControlsY};
}
inline Rect opEnvSustainButton(int sustain) { // 0 One-Shot, 1 Sustain, as the Curve's clock modes' icons
    return {kInspector.x + 70.0f + sustain * 26.0f, kOpControlsY - 11.0f, 26.0f, 22.0f};
}
inline Rect opEnvSyncButton() { return {kInspector.x + 148.0f, kOpControlsY - 11.0f, 24.0f, 22.0f}; }
constexpr Point kOpEnvLengthKnob{kInspector.x + 196.0f, kOpControlsY};
constexpr Point kOpEnvSlewKnob{kInspector.x + 236.0f, kOpControlsY};
inline Rect opEnvEditButton() { return {kOpDisplay.right() - 26.0f, kOpControlsY - 11.0f, 26.0f, 22.0f}; }
// A drawn envelope's plot in the display: room above it for its loop's strip, and under it for the ruler's times.
constexpr Rect kOpEnvPlot{kOpDisplay.x + 10.0f, kOpDisplay.y + 16.0f, kOpDisplay.w - 20.0f, 80.0f};
// Unison: where the copies sit, over a row of buttons along the well's bottom (how they spread in pitch, then, past a
// rule, where they sit across), its knobs under the well.
constexpr float kUnisonKnobsY = kOpControlsY;
constexpr float kUnisonButtonsH = 28.0f; // the row, at the display's bottom
constexpr Rect kUnisonRect{kOpDisplay.x, kOpDisplay.y, kOpDisplay.w, kOpDisplay.h - kUnisonButtonsH};
inline Rect uniCurveButton(int curve) {
    return {kOpDisplay.x + 6.0f + curve * 26.0f, kOpDisplay.bottom() - kUnisonButtonsH + 2.0f, 26.0f, 22.0f};
}
constexpr float kUniRuleX = kOpDisplay.x + 6.0f + 4 * 26.0f + 8.0f; // between the curves and the pans
inline Rect uniPanButton(int pan) {
    return {kUniRuleX + 8.0f + pan * 26.0f, kOpDisplay.bottom() - kUnisonButtonsH + 2.0f, 26.0f, 22.0f};
}
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
// A Morph filter's slope's list and its notch switch (through the notch rather than the band-pass, the notch's icon),
// side by side in its place, centered together.
inline Rect morphSlopeButton(Rect display) {
    return {display.center().x - 57.0f, display.bottom() + 4.0f, 72.0f, 18.0f};
}
inline Rect morphNotchButton(Rect display) {
    return {display.center().x + 21.0f, display.bottom() + 4.0f, 36.0f, 18.0f};
}
// A filter's Model list (for a family of more than one model, see gui/lists.h) and its slope's beside it, in the
// slope's place under its display, centered together: on a filter's page the model's button with its arrows and icon,
// on the filter effect's card (`plain`) with neither, its name short. Its list opens wide enough for the models' names.
inline Rect filterModelButton(Rect display, bool plain) {
    const float w = plain ? 64.0f : 136.0f;
    return {display.center().x - 0.5f * (w + 6.0f + 72.0f), display.bottom() + 4.0f, w, 18.0f};
}
inline Rect filterModelSlopeButton(Rect display, bool plain) {
    const Rect model = filterModelButton(display, plain);
    return {model.right() + 6.0f, model.y, 72.0f, 18.0f};
}
inline Rect filterModelMenuButton(Rect button) {
    return {button.x, button.y, std::max(button.w, 136.0f), button.h};
}
// A filter's page: its type's list across the row under the summary (where a shaper's curve's is), its response with
// the slope's list under it.
constexpr Rect kFilterTypeButton{kInspector.x + 16.0f, kInspector.y + 62.0f, kInspector.w - 32.0f, 22.0f};
// A Formant or Morph filter's six knobs (its Vowel or Morph after Resonance), a little smaller and closer together
// than the five the others have.
constexpr float kFilterSixKnobD = 30.0f;
inline Point filterSixKnob(int i) {
    return {kInspector.x + 29.0f + i * 48.0f, kInspector.y + 110.0f};
}
// A filter type's icon before its name, on a type list's button or one of its rows.
inline Rect filterTypeIconRect(Rect row) {
    return {row.x + 9.0f, row.center().y - 5.0f, 22.0f, 10.0f};
}
constexpr Rect kFilterDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 90.0f};
inline const Rect kFilterSlopeButton = slopeButton(kFilterDisplay);
// A shaper's page: its curve's list and the link switch in the row under the summary, with a Custom curve's edit
// button (a pencil, opening its big editor) and symmetric switch between them, or the Stack curve's Stack stepper,
// then its transfer curve where a filter's response shows.
constexpr Rect kShaperLinkButton{kInspector.x + kInspector.w - 16.0f - 44.0f, kInspector.y + 62.0f, 44.0f, 22.0f};
constexpr Rect kShaperSymButton{kShaperLinkButton.x - 50.0f, kShaperLinkButton.y, 44.0f, 22.0f};
constexpr float kClipEditW = 28.0f; // a Custom curve's edit button beside its list, on a shaper's page and card
constexpr Rect kShaperEditButton{kShaperSymButton.x - 6.0f - kClipEditW, kShaperLinkButton.y, kClipEditW, 22.0f};
// The Stack stepper's place: a knob this size, whose field (see View::widgets()) comes out level with LINK and as high
// (its foot 5.5 px above the knob's), 56 px wide, 6 px clear of LINK's left edge, without a label.
constexpr float kShaperStackKnob = 32.0f;
constexpr Point kShaperStackCenter{kShaperLinkButton.x - 6.0f - 28.0f,
                                   kShaperLinkButton.y + kShaperLinkButton.h + 5.5f - 0.5f * kShaperStackKnob};
inline Rect shaperModeButton(bool custom, bool stacked = false) {
    return {kInspector.x + 16.0f, kInspector.y + 62.0f,
            kInspector.w - 32.0f - (custom    ? kShaperLinkButton.right() - kShaperEditButton.x + 6.0f
                                    : stacked ? 44.0f + 6.0f + 56.0f + 6.0f
                                              : 50.0f),
            22.0f};
}
constexpr Rect kShaperDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 112.0f};
// An operator's wave buttons: the common waves, then "..." with a list of the others.
constexpr OpWave kCommonWaves[] = {OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Noise};
constexpr OpWave kMoreWaves[] = {OpWave::Pulse,   OpWave::HalfSine, OpWave::AbsSine, OpWave::AltSine,
                                 OpWave::Organ,   OpWave::Vowel,    OpWave::Custom};
static_assert(std::size(kCommonWaves) + std::size(kMoreWaves) == static_cast<size_t>(OpWave::Count));
constexpr int kWaveButtons = static_cast<int>(std::size(kCommonWaves)) + 1;
// One segmented row at the top of the block's right column.
inline Rect waveButton(int button) {
    const float step = kOpRightW / kWaveButtons;
    return {kOpRightX + button * step, kOpHeroY, step, kOpWavesH};
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
// The row between the header and the panels: the view tabs at its left.
constexpr float kTopRowY = kHeaderH + 8.0f;
constexpr float kTopRowH = 24.0f; // the preset bar's and the toolbar's height, in the title bar
// Tabs of the routing panel, one per RoutingView: folder tabs from the row's top down onto the panels' top edge,
// the first one's left side over the panel's left edge, which it runs on into while it is shown.
constexpr float kViewTabH = kPanelsTop - kTopRowY;
// By RoutingView: SEQ's sits between FX's and MODS's.
constexpr Rect kViewTabs[] = {{16.0f, kTopRowY, 96.0f, kViewTabH},
                               {116.0f, kTopRowY, 88.0f, kViewTabH},
                               {208.0f, kTopRowY, 64.0f, kViewTabH},
                               {344.0f, kTopRowY, 76.0f, kViewTabH},
                               {276.0f, kTopRowY, 64.0f, kViewTabH}};
constexpr float kViewTabRadius = kRadiusCard; // its top corners
// The dot in a view tab's upper right while what it shows is at work (an effect switched on, the sequencer on;
// kTabDotRadius).
inline Point viewTabDot(int view) {
    return {kViewTabs[view].x + kViewTabs[view].w - 9.0f, kViewTabs[view].y + kViewTabs[view].h * 0.5f - 4.0f};
}
constexpr float kViewTabFlare = 6.0f;  // the shown one's curves into the panel's edge
// ALGORITHM, at the row's right end over the routing panel's, with ROUTING and MATRIX: it opens the panel of the
// classic algorithms (kAlgorithms) under it, right-aligned with the routing panel and over the graph or matrix, the
// inspector left in view. The panel has a title row with a switch between the sets at its right (6 | 4 | 3 | 2, how
// many operators they use, "Operators" before it), a thumbnail a cell, kAlgorithmCols to a row, and a foot under a
// line, which names the thumbnail under the mouse. Six operators' four rows of them end clear of the ghosts the graph
// docks at its bottom right (see RoutingPanel::widgets()).
constexpr Rect kAlgorithmButton{kRouting.x + kRouting.w - 190.0f, kTopRowY + 4.0f, 190.0f, 24.0f}; // "4-OP 8" fits on it
constexpr int kAlgorithmCols = 8;
constexpr float kAlgorithmCellW = 72.0f, kAlgorithmCellH = 58.0f, kAlgorithmGap = 6.0f, kAlgorithmPad = 14.0f;
constexpr float kAlgorithmTitleH = 32.0f, kAlgorithmFootH = 34.0f;
constexpr float kAlgorithmPanelW = 2.0f * kAlgorithmPad + kAlgorithmCols * kAlgorithmCellW + (kAlgorithmCols - 1) * kAlgorithmGap;
inline Rect algorithmPanel(int count) {
    const int rows = (count + kAlgorithmCols - 1) / kAlgorithmCols;
    return {kRouting.x + kRouting.w - kAlgorithmPanelW, kPanelsTop + 4.0f, kAlgorithmPanelW,
            kAlgorithmPad + kAlgorithmTitleH + rows * kAlgorithmCellH + (rows - 1) * kAlgorithmGap + kAlgorithmFootH};
}
inline Rect algorithmCell(int count, int i) {
    const Rect p = algorithmPanel(count);
    return {p.x + kAlgorithmPad + (i % kAlgorithmCols) * (kAlgorithmCellW + kAlgorithmGap),
            p.y + kAlgorithmPad + kAlgorithmTitleH + (i / kAlgorithmCols) * (kAlgorithmCellH + kAlgorithmGap),
            kAlgorithmCellW, kAlgorithmCellH};
}
// The switch's button for the set at `set` in kAlgorithmSets (6 first), in the title row at its right; its label
// goes before the first.
constexpr int kAlgorithmSetCount = 4;
inline Rect algorithmSetButton(int count, int set) {
    constexpr float kW = 32.0f;
    const Rect p = algorithmPanel(count);
    return {p.right() - kAlgorithmPad - (kAlgorithmSetCount - set) * kW, p.y + kAlgorithmPad, kW, 22.0f};
}
// The FX view covers the routing panel and the inspector: one card per effect,
// left to right in processing order, then a card to add one; kVisibleFx at a time.
constexpr Rect kFxPanel{16.0f, kPanelsTop, kRouting.x + kRouting.w - 16.0f, kRouting.h};
// The full-size shape editors (a Curve's, an operator's envelope's, the note source's and velocity's curves): as wide as
// the routing panel, in the middle of the panels' row, over the inspector too.
constexpr Rect kShapeEditorPanel{kFxPanel.x + 0.5f * (kFxPanel.w - kRouting.w), kPanelsTop, kRouting.w, kRouting.h};
constexpr int kVisibleFx = 6;
constexpr float kFxGap = 22.0f; // room for the plus that adds an effect between two cards
// The row's first card starts a gap in from the clip it scrolls in (see fxCardClip()), for the arrow before it.
constexpr float kFxRowLeft = 8.0f + kFxGap;
constexpr float kFxCardW = (kFxPanel.w - kFxRowLeft - 16.0f - (kVisibleFx - 1) * kFxGap) / kVisibleFx;
constexpr float kFxTop = kRouting.y + 48.0f;
// The SEQ view covers them too, at its full size the modulation panel under them as well (kModPanel, whose bottom it
// shares), so it is as tall as both and the gap between; at its half size (EditorSettings::seqFull off) only them, as
// the FX view does: the sequencer's settings on a card where the inspector floats, and the roll of the pattern picked
// in the rest, under a row of tools: the piano keys at its left, the bars over it, a lane of one value a note under it
// and the lane's tabs under that. Its rows are kSeqRowH high, as many as fit.
constexpr Rect kSeqPanel{kFxPanel.x, kPanelsTop, kFxPanel.w, kRouting.h + 16.0f + 258.0f};
// The editor's band the SEQ view keeps to, as kPanelsBand is the panels' row's (see View::panelsBand()).
constexpr Rect kSeqBand{0.0f, kHeaderH, View::kWidth, kSeqPanel.y + kSeqPanel.h - kHeaderH};
constexpr Rect kSeqCard = kInspector;
constexpr float kSeqCardX = kSeqCard.x + 12.0f;       // the card's labels
constexpr float kSeqCardControls = kSeqCard.x + 76.0f; // and the controls after them
constexpr Rect kSeqTools{kRouting.x + 4.0f, kPanelsTop + 10.0f, 1172.0f - kRouting.x - 4.0f, 24.0f};
constexpr float kSeqKeysW = 44.0f, kSeqRulerH = 20.0f, kSeqLaneH = 52.0f, kSeqLaneTabsH = 22.0f, kSeqRowH = 10.0f;
constexpr float kSeqScrollW = 12.0f;     // the rows' scroll bar, at the grid's right
constexpr float kSeqLaneTabStep = 76.0f; // a lane's tab and the gap after it
constexpr float kSeqMacroRowH = 22.0f;
// The tools over the roll, left to right: Select | Draw | Bend, SNAP and its list's button, x2 and /2; then from the
// right the bin, MUTE and SLIDE, and left of them the field of the notes picked (NOTE, VEL, CHANCE, LEN, OFFSET, BEND).
constexpr float kSeqToolW = 28.0f;
constexpr int kSeqToolCount = 3;
constexpr Rect seqToolButton(int b) { return {kSeqTools.x + static_cast<float>(b) * kSeqToolW, kSeqTools.y, kSeqToolW, kSeqTools.h}; }
constexpr Rect seqSnapButton() { return {kSeqTools.x + 126.0f, kSeqTools.y, 76.0f, kSeqTools.h}; }
// SNAP's label, between the tools and its list's button, against the button (right-aligned, 6 px before it).
constexpr Rect kSeqSnapLabel{kSeqTools.x + kSeqToolCount * kSeqToolW + 4.0f, kSeqTools.y + 1.0f,
                             seqSnapButton().x - 6.0f - (kSeqTools.x + kSeqToolCount * kSeqToolW + 4.0f), 22.0f};
constexpr Rect seqStretchButton(int b) { return {kSeqTools.x + 208.0f + (b > 0 ? 0.0f : 30.0f), kSeqTools.y, 28.0f, kSeqTools.h}; }
constexpr Rect seqClearButton() { return {kSeqTools.x + kSeqTools.w - 26.0f, kSeqTools.y, 26.0f, kSeqTools.h}; }
constexpr Rect seqFlagButton(int flag) { // Slide, then Mute, left of the bin
    return {seqClearButton().x - 8.0f - (flag == 0 ? 104.0f : 50.0f), kSeqTools.y, flag == 0 ? 52.0f : 50.0f, kSeqTools.h};
}
constexpr int kSeqFieldCount = 6;
constexpr float kSeqFieldWidths[kSeqFieldCount] = {66.0f, 58.0f, 86.0f, 58.0f, 80.0f, 66.0f};
constexpr Rect seqFieldRect(int field) { // left of Slide, the note first
    float x = seqFlagButton(0).x - 8.0f;
    for (int f = kSeqFieldCount - 1; f >= field; --f)
        x -= kSeqFieldWidths[f];
    return {x, kSeqTools.y, kSeqFieldWidths[field], kSeqTools.h};
}
static_assert(seqFieldRect(0).x >= seqStretchButton(-2).x + seqStretchButton(-2).w + 8.0f, "the field reaches the tools");
// The lane's tabs: Velocity, Chance, Bend, Mod Wheel, then the macros' (see SeqRoll::macroTab).
constexpr int kSeqLaneTabCount = 4;
// The roll's places, down to the panel's bottom less 10 px (`bottom`): its box, the keys, the grid and the rows' scroll
// bar, the ruler over them, the lane and its tabs under them, and after the lane's tabs the macros' curves: one more
// tab, wider for its macro's number and its arrow, and over it the list of macros it opens.
struct SeqRoll {
    Rect box, keys, grid, scrollY, ruler, laneArea, laneTabs, macroTab, macroList;
};
constexpr SeqRoll seqRollTo(float bottom) {
    SeqRoll r{};
    const float top = kSeqTools.y + kSeqTools.h + 8.0f;
    r.box = {kSeqTools.x, top, kSeqTools.w, bottom - 10.0f - top};
    r.keys = {r.box.x, r.box.y + kSeqRulerH, kSeqKeysW, r.box.h - kSeqRulerH - kSeqLaneH - kSeqLaneTabsH};
    r.grid = {r.keys.x + r.keys.w, r.keys.y, r.box.x + r.box.w - r.keys.x - r.keys.w - kSeqScrollW, r.keys.h};
    r.scrollY = {r.grid.x + r.grid.w + 3.0f, r.grid.y + 2.0f, 6.0f, r.grid.h - 4.0f};
    r.ruler = {r.grid.x, r.box.y, r.grid.w, kSeqRulerH};
    r.laneArea = {r.grid.x, r.grid.y + r.grid.h, r.grid.w, kSeqLaneH};
    r.laneTabs = {r.box.x, r.laneArea.y + r.laneArea.h, r.box.w, kSeqLaneTabsH};
    r.macroTab = {r.grid.x + kSeqLaneTabCount * kSeqLaneTabStep, r.laneTabs.y + 3.0f, 92.0f, r.laneTabs.h - 6.0f};
    r.macroList = {r.macroTab.x, r.macroTab.y - 4.0f - (8.0f + 8.0f * kSeqMacroRowH), 180.0f, 8.0f + 8.0f * kSeqMacroRowH};
    return r;
}
constexpr SeqRoll kSeqFullRoll = seqRollTo(kSeqPanel.y + kSeqPanel.h);
constexpr SeqRoll kSeqHalfRoll = seqRollTo(kFxPanel.y + kFxPanel.h);
constexpr const SeqRoll &seqRoll(bool full) { return full ? kSeqFullRoll : kSeqHalfRoll; }
// The full size's, by name.
constexpr Rect kSeqBox = kSeqFullRoll.box;
constexpr Rect kSeqKeys = kSeqFullRoll.keys;
constexpr Rect kSeqGrid = kSeqFullRoll.grid;
constexpr Rect kSeqScrollYTrack = kSeqFullRoll.scrollY;
constexpr Rect kSeqRuler = kSeqFullRoll.ruler;
constexpr Rect kSeqLaneArea = kSeqFullRoll.laneArea;
constexpr Rect kSeqLaneTabs = kSeqFullRoll.laneTabs;
constexpr Rect kSeqMacroTab = kSeqFullRoll.macroTab;
constexpr Rect kSeqMacroList = kSeqFullRoll.macroList;
// The macros' tab in two parts: its label, the macro it shows, and at its right end the arrow that opens their list.
constexpr float kSeqMacroArrowW = 20.0f;
constexpr Rect seqMacroLabel(Rect tab) { return {tab.x, tab.y, tab.w - kSeqMacroArrowW, tab.h}; }
constexpr Rect seqMacroArrow(Rect tab) { return {tab.x + tab.w - kSeqMacroArrowW, tab.y, kSeqMacroArrowW, tab.h}; }
constexpr Rect seqMacroRow(Rect list, int macro) {
    return {list.x + 4.0f, list.y + 4.0f + static_cast<float>(macro) * kSeqMacroRowH, list.w - 8.0f, kSeqMacroRowH};
}
// The card's rows, from its top: its title, PATTERN, PLAY, two rows of knobs, ORDER and ROOT, SCALE and FOLD.
constexpr float kSeqTitleY = kSeqCard.y + 12.0f;
constexpr float kSeqPatternY = kSeqCard.y + 46.0f;
constexpr float kSeqPlayY = kSeqCard.y + 88.0f; // clear of PATTERN's modulation under it
constexpr float kSeqKnobD = 42.0f;                 // the knobs' size
constexpr float kSeqKnobsY = kSeqCard.y + 142.0f; // the first row's knobs' centers
constexpr float kSeqKnobStep = 78.0f;              // to the second row's
constexpr float kSeqOrderY = kSeqCard.y + 290.0f;
constexpr float kSeqScaleY = kSeqCard.y + 322.0f;
// PATTERN's four buttons as one row, and under it, while it is modulated, its modulation's reach from A's middle to
// D's (seqPatternReach()) and its depth handles under that, centered on the row, as a knob's are under it.
inline Rect seqPatternRow() { return {kSeqCardControls, kSeqPatternY, 3.0f * 30.0f + 28.0f, 22.0f}; }
inline Rect seqPatternReach() {
    const Rect row = seqPatternRow();
    return {row.x + 14.0f, row.bottom() + 3.0f, 90.0f, 1.5f};
}
constexpr float kSeqPatternHandlesY = kSeqPatternY + 22.0f + 11.0f; // the depth handles' centers
// The Play button, at the Pattern row's right end, clear of the buttons and their modulation's reach.
constexpr Rect kSeqPreviewButton{kSeqCard.x + kSeqCard.w - 12.0f - 64.0f, kSeqPatternY, 64.0f, 22.0f};
// The card's switch between the view's full and half size, left of its power switch: the Curve card's expand and
// collapse button.
constexpr Rect kSeqSizeButton{kSeqCard.x + kSeqCard.w - 12.0f - 26.0f - 4.0f - 26.0f, kSeqTitleY, 26.0f, 22.0f};
inline Rect seqKnobCenterRect(int i) { // the place of knob `i` of the eight, as knobRect() takes it
    return {kSeqCard.x + 16.0f + 268.0f / 4.0f * (static_cast<float>(i % 4) + 0.5f), kSeqKnobsY + (i / 4) * kSeqKnobStep,
            0.0f, 0.0f};
}
// At the full size, under the card, down to the panel's bottom (as far in as the card from its top), the patterns' overview: a card
// like it, its title, then a tile a pattern, A to D, one under another: its letter at its left and its notes drawn
// small after it, across the longest pattern's length (see SeqPanel::paintSeqTile()). A click on a tile shows its
// pattern; a tile dragged onto another copies its pattern there.
constexpr Rect kSeqOverview{kSeqCard.x, kSeqCard.y + kSeqCard.h + 8.0f, kSeqCard.w,
                            kSeqPanel.y + kSeqPanel.h - 6.0f - (kSeqCard.y + kSeqCard.h + 8.0f)};
constexpr float kSeqOverviewTitleY = kSeqOverview.y + 12.0f;
constexpr float kSeqTileGap = 6.0f;
constexpr float kSeqTileTop = kSeqOverview.y + 42.0f;
constexpr float kSeqTileH =
    (kSeqOverview.y + kSeqOverview.h - 12.0f - kSeqTileTop - (kSeqPatterns - 1) * kSeqTileGap) / kSeqPatterns;
constexpr float kSeqTileLetterW = 26.0f; // the letter's place at the tile's left; the notes take the rest
inline Rect seqTile(int pattern) {
    return {kSeqOverview.x + 12.0f, kSeqTileTop + static_cast<float>(pattern) * (kSeqTileH + kSeqTileGap),
            kSeqOverview.w - 24.0f, kSeqTileH};
}
inline Rect seqTileNotes(Rect tile) {
    return {tile.x + kSeqTileLetterW, tile.y + 6.0f, tile.w - kSeqTileLetterW - 8.0f, tile.h - 12.0f};
}
static_assert(kSeqTileH >= 40.0f);

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
// A depth bar's track (a MODS row's, a pill's list's): in from its ends by its handle's radius, so the handle at
// either end stays clear of the depth after it.
inline Rect slotBarTrack(Rect bar) { return {bar.x + 5.0f, bar.y, bar.w - 10.0f, bar.h}; }
// A text field's x at its right end, centered on its height, which empties it, and where its text goes: after the
// magnifier, short of the x.
inline Rect fieldClear(Rect field) {
    return {field.right() - 22.0f, field.y + (field.h - 18.0f) * 0.5f, 18.0f, 18.0f};
}
inline Rect fieldTyped(Rect field) { return {field.x + 26.0f, field.y, field.w - 26.0f - 24.0f, field.h}; }
// Where a form's field (the Save dialog's, the name form's) shows its text: inside its sides, its whole height, so
// what hangs below the line (an underscore, a g) isn't clipped away.
inline Rect formFieldTyped(Rect field) { return {field.x + 10.0f, field.y, field.w - 20.0f, field.h}; }
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
constexpr int kCascadeColumnRows = 16; // the most sources a list of them has in a column, past which it goes on beside it
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
// Tall enough for a card's button under its display to end 12 px above its bottom. The SEQ view covers it.
constexpr Rect kModPanel{16.0f, kPanelsTop + 372.0f + 16.0f, 1168.0f, 258.0f};
static_assert(kModPanel.y + kModPanel.h == kSeqPanel.y + kSeqPanel.h);
constexpr int kVisibleCards = 4;
constexpr float kCardGap = 12.0f;
constexpr float kCardW = (kModPanel.w - 24.0f - (kVisibleCards - 1) * kCardGap) / kVisibleCards;
constexpr float kCardTop = kModPanel.y + 44.0f;
constexpr float kCardH = kModPanel.y + kModPanel.h - 18.0f - kCardTop; // the scrollbar goes under them
constexpr float kAddSourceX = kModPanel.x + 112.0f;

// The panel's title, at its top left; the keyboard strip's chevron and title sit as far into it (keysToggle()).
inline Rect modTitle() {
    return {kModPanel.x + 8.0f, kModPanel.y + 8.0f, 100.0f, 26.0f};
}

// Every card's title row, an effect's, a source's or a split's: its mark (the effect's icon or the source's handle) and
// its name at the left; at the right, from the corner in, the cross, the power switch (a split has none), the buttons
// beside it (`tools` of them: a Curve card's), all always shown, then the fold button, which shows while the mouse is
// over the card: what always shows packs against the corner and never moves. Its name stops short of the fold button
// (cardTitleRight()). An effect card in the effects graph's inspector has no fold or cross (its node's cross removes
// it): its power switch takes the cross's corner (`cross` false), and its name goes up to it.
// An effect card's (or a split container's) title: its box, 20 px tall, this far down the card, which centers its
// name on the title row's icon and buttons (cardFold(), cardPower(), cardClose()).
constexpr float kCardTitleY = 7.5f;
constexpr float kCardToolW = 28.0f, kCardToolStep = 34.0f; // a title row's always-shown buttons, and their spacing
inline Rect cardClose(Rect card) {
    return {card.right() - 22.0f, card.y + 8.0f, 14.0f, 21.0f};
}
// The power switch: 6 px clear of the cross beside it, or in the cross's corner where there is none.
inline Rect cardPower(Rect card, bool cross = true) {
    return {card.right() - (cross ? 48.0f : 28.0f), card.y + 8.5f, 20.0f, 19.0f};
}
// The always-shown button `i` places left of the power switch, from the right.
inline Rect cardTool(Rect card, int i) {
    return {cardPower(card).x - 4.0f - kCardToolW - static_cast<float>(i) * kCardToolStep, card.y + 7.0f, kCardToolW,
            22.0f};
}
// The fold button: 6 px clear of what is right of it (as the power switch is of the cross), the name stopping as far
// short of it on its left.
constexpr float kCardFoldGap = 6.0f;
inline Rect cardFold(Rect card, bool power = true, int tools = 0) {
    const float edge = !power     ? cardClose(card).x
                       : tools > 0 ? cardTool(card, tools - 1).x - 2.0f
                                   : cardPower(card).x;
    return {edge - kCardFoldGap - 14.0f, card.y + 9.0f, 14.0f, 19.0f};
}
inline float cardTitleRight(Rect card, bool power = true, int tools = 0) {
    return cardFold(card, power, tools).x - kCardFoldGap;
}

// A card by its screen place: 0..3, fractions while it scrolls.
// `span`: the card places it takes, less than one folded.
inline Rect sourceCard(float card, float span = 1.0f) {
    return {kModPanel.x + 12.0f + card * (kCardW + kCardGap), kCardTop, span * (kCardW + kCardGap) - kCardGap, kCardH};
}
// A folded card is a narrow strip: its handle, a grip, its switch and cross, then its name down the rest, which
// unfolds it. An effect card folds much the same way: its icon, which switches it, then its cross and its name.
// That strip is as wide as the icon's (or the handle's) place at the title's left, so it stays put as the card folds:
// a source's as narrow as an effect's, its handle as far in from the card's edge as an effect's icon.
constexpr float kSourceHandleX = 9.0f; // the handle's left edge, from the card's
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
// Under the plot, along its left: Points | Steps and Snap (see lfoModeButton()), then the grid's steppers, across
// (axis 0) then up and down: each an arrow, then a step down (part 0), its count (1) and a step up (2).
inline Rect lfoCardGridButton(float card, int axis, int part) {
    constexpr float kX[] = {14.0f, 32.0f, 54.0f}, kW[] = {18.0f, 22.0f, 18.0f};
    const Rect plot = lfoCardPlot(card);
    return {plot.x + 90.0f + axis * 88.0f + kX[part], plot.bottom() + 18.0f, kW[part], 18.0f};
}
// Its title row's buttons, by what they do: back to a normal card (0) and the full-size editor (2), beside the power
// switch (cardTool()), left to right. The shape's reset (1) is the first of its tools (see lfoToolButton()).
constexpr int kWideSourceTools = 2;
inline Rect lfoCardButton(float card, int button) {
    return cardTool(sourceCard(card, kWideSource), button == 0 ? 1 : 0);
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

inline Rect sourceDisplay(float card) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 98.0f, p.w - 32.0f, 52.0f};
}

// A row of a pill's list: the target's name, between the bypass dot and the depth bar.
constexpr float kSlotNameW = 96.0f;

// The "+n" pill that takes the place of the depth handles a knob, label or cell has no room for: under a knob, and
// smaller in a label's or cell's corner; wider from +10 on. Its list: a title, then a row per modulation: its bypass
// dot, the source's name, its aux (a column only while one of the rows has one), its depth bar with its value and its
// remove.
// Both come in three sizes, a setting of the user's (EditorSettings::handleSize), the smallest first; they stay as far
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
constexpr float kModListAuxW = 126.0f;   // the aux column: its chip and a gap

// A row's aux chip, after its source's name, its x at its right end.
inline Rect modListAuxChip(Rect row) {
    return {row.x + 116.0f, row.y + 1.0f, kModListAuxW - 4.0f, row.h - 2.0f};
}
// The width of the aux depth on an aux's chip, before its x (in the MODS view and a pill's list alike).
constexpr float kAuxDepthW = 34.0f;
// The aux depth's place on a pill's list row's chip, before its x.
inline Rect modListAuxDepth(Rect row) {
    const Rect chip = modListAuxChip(row);
    return {chip.right() - 20.0f - kAuxDepthW, chip.y, kAuxDepthW, chip.h};
}

// Under a source's display, at the card's foot, the button that shows its modulations in the MODS view.
inline Rect sourceModsButton(float card) { // as wide as its words, about
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 160.0f, 110.0f, 24.0f};
}

// A widened LFO card's column of controls has the shape's reset and tools (see ShapeTool) in a row where a normal
// card's display is, level with the plot's foot; Points | Steps and Snap (see ShapeMode) go under the plot, as icons,
// with the grid's steppers.
constexpr int kShapeTools = 6;
inline Rect lfoToolSlot(float card, int slot) { // 0 the reset, then the tools
    const Rect d = sourceDisplay(card);
    const float w = (d.w - kShapeTools * 4.0f) / (kShapeTools + 1);
    return {d.x + slot * (w + 4.0f), lfoCardPlot(card).bottom() - 22.0f, w, 22.0f};
}
inline Rect lfoToolButton(float card, int tool) { return lfoToolSlot(card, tool + 1); }
inline Rect lfoResetButton(float card) { return lfoToolSlot(card, 0); }
inline Rect lfoModeButton(float card, int mode) { // Points and Steps in one pill, then Snap, from the plot's frame's edge
    const Rect plot = lfoCardPlot(card);
    return {plot.x - 8.0f + mode * 26.0f + (mode == 2 ? 6.0f : 0.0f), plot.bottom() + 16.0f, 26.0f, 22.0f};
}

// A source card's title row, as every card's is (cardClose()): handle and name (the row drags the card), then the fold
// button, any always-shown buttons, the power switch and the remove cross at the right; of a card `span` places wide.
inline Rect sourceRemove(float card, float span = 1.0f) {
    return cardClose(sourceCard(card, span));
}

inline Rect sourcePower(float card, float span = 1.0f) {
    return cardPower(sourceCard(card, span));
}
// A Custom LFO's normal card: beside its power switch, the button that widens it (its one always-shown button).
inline Rect lfoWidenButton(float card) {
    return cardTool(sourceCard(card), 0);
}

// An LFO's shape list (a button with the shape's icon and name, and the list it opens under it) over its clock modes'
// list (see lfoClockButton()): retrigger, free, anchor and one-shot, a random one's only the first two. A Curve's
// Sustain, after them, isn't among an LFO's: a Curve's five are its list where an LFO's shape list is (see
// detail::isCurveClock()), and its full-size editor's title row has them as buttons.
constexpr int kLfoModes = static_cast<int>(LfoClock::OneShot) + 1;
static_assert(static_cast<int>(LfoClock::Sustain) == kLfoModes, "an LFO card's clock modes are those before Sustain");
constexpr int kCurveModes = static_cast<int>(LfoClock::Count);
constexpr float kLfoListW = 122.0f; // the shape list's and the clock list's width

inline Rect lfoShapeButton(float card) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 42.0f, kLfoListW, 22.0f};
}

// The shapes an LFO's list offers, in its order: every one but the retired Random and Custom, which the Random and
// Curve sources took over.
constexpr LfoShape kLfoShapesOffered[] = {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Square};
constexpr int kLfoShapeItems = static_cast<int>(std::size(kLfoShapesOffered));
// The curves a shaper's list offers, in its order: Stack, which came after Custom (they are stored by number), before
// it.
constexpr ClipMode kClipModesOffered[] = {ClipMode::Hard,   ClipMode::Soft,     ClipMode::Asymmetric,
                                          ClipMode::Cubic,  ClipMode::Gentle,   ClipMode::SineFold,
                                          ClipMode::Fold,   ClipMode::Stack,    ClipMode::Custom};
static_assert(std::size(kClipModesOffered) == static_cast<size_t>(ClipMode::Count), "the list offers every curve");

// Right of those two rows, the Rate (0) and Phase (1) knobs with their labels.
inline Point lfoKnob(float card, int knob) {
    const Rect p = sourceCard(card);
    return {p.right() - 91.0f + knob * 50.0f, lfoShapeButton(card).y + 14.0f}; // the label takes 18 px under the knob
}

// An LFO's clock modes, a list under the shape's, as wide; a Curve's and a Random source's are their list where the
// shape's is (see detail::isCurveClock()).
inline Rect lfoClockButton(float card) {
    const Rect list = lfoShapeButton(card);
    return {list.x, list.bottom() + 6.0f, list.w, list.h};
}

// An LFO's Unipolar switch, a small square between its shape list and the Rate knob, level with the list, and its sync
// switch under it, level with the clock list.
inline Rect lfoUnipolarSwitch(float card) {
    const Rect list = lfoShapeButton(card);
    const float x = (list.right() + lfoKnob(card, 0).x - 15.0f) * 0.5f;
    return {x - 10.0f, list.center().y - 10.0f, 20.0f, 20.0f};
}
inline Rect lfoSyncButton(float card) {
    const Rect unipolar = lfoUnipolarSwitch(card);
    return {unipolar.x, lfoClockButton(card).center().y - unipolar.h * 0.5f, unipolar.w, unipolar.h};
}

// A Curve card, and a Random card laid out as one: under its clock list, at its right end before the Rate knob, its
// Unipolar and sync switches; right of the list its Rate (or Length), Phase and Slew knobs (0 to 2), a Random's Rate,
// Smooth and Chaos, alike and side by side, their labels under them.
inline Rect curveSyncButton(float card) {
    const Rect clocks = lfoClockButton(card);
    return {clocks.right() - 20.0f, clocks.center().y - 10.0f, 20.0f, 20.0f};
}
inline Rect curveUnipolarSwitch(float card) {
    const Rect sync = curveSyncButton(card);
    return {sync.x - 6.0f - sync.w, sync.y, sync.w, sync.h};
}
inline Point curveKnob(float card, int knob) {
    const Rect p = sourceCard(card);
    return {p.x + 163.0f + knob * 40.0f, lfoKnob(card, 0).y};
}

// ADD EFFECT at the top left, and the list it opens under it: one row per type,
// then how many places are left.
inline Rect fxMenuButton() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + 9.0f, 116.0f, 22.0f};
}

// The effects the list offers, in its order: every type but None and the retired three-band compressor, whose place
// the three-band dynamics takes. They go by kind in six groups (kFxMenuGroups), each under its name.
constexpr FxType kFxOffered[] = {
    FxType::Shaper,     FxType::Amp,       FxType::LoFi,         // drive
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
constexpr int kFxMenuGroups[] = {3, 3, 2, 2, 4, 3};
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

// A shaper card's curve picker: a button with the curve's name across the card, and the list it opens under it (for a
// Custom curve, the curve's edit button at its right, a pencil opening its big editor); under its display LINK, which
// ties the Output knob to the drive, centered on the card; for a Custom curve SYM left of it, and for the Stack curve
// its Stack stepper right of it, the two centered together.
// The reverb's row: its algorithm's button.
inline Rect clipModeButton(Rect card, bool custom = false) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f - (custom ? kClipEditW + 6.0f : 0.0f), 22.0f};
}
inline Rect clipEditButton(Rect card) {
    return {card.right() - 14.0f - kClipEditW, card.y + 44.0f, kClipEditW, 22.0f};
}
inline Rect clipSymButton(Rect card) {
    return {card.center().x - 47.0f, card.y + 199.0f, 44.0f, 20.0f};
}
inline Rect clipLinkButton(Rect card, bool custom, bool stacked = false) {
    return {card.center().x + (custom ? 3.0f : stacked ? -53.0f : -22.0f), card.y + 199.0f, 44.0f, 20.0f};
}
// The Stack stepper's place: a knob this size, whose field (see View::widgets()) comes out level with LINK and as high
// (its foot 5.5 px above the knob's), 56 px wide, 6 px clear of LINK's right edge, without a label.
constexpr float kClipStackKnob = 30.0f;
inline Point clipStackCenter(Rect card) {
    return {card.center().x + 25.0f, card.y + 199.0f + 20.0f + 5.5f - 0.5f * kClipStackKnob};
}
// The reverb's three rows of knobs, down its card from its top.
constexpr float kReverbKnobsY[] = {158.0f, 212.0f, 266.0f};
// A card's two rows of knobs under its display (the delay's, compressor's, phaser's and on), and the shaper's one row
// under its LINK: down the card from its top, the lower row where it always was, the others moved down toward it as
// far as the smaller knobs of a row of four let them, the display taking the room.
constexpr float kFxKnobsY1 = 211.0f, kFxKnobsY2 = 266.0f, kShaperKnobsY = 245.0f;
// The flanger's and frequency shifter's lower row, Feedback and Mix with TZF or nothing between: this share of the card
// in from either side, further in than a row of three's ends, so Feedback's label stays clear of the card's edge.
constexpr float kFxPairX = 0.21f;
inline Rect reverbTypeButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}
// The filter effect's type, in the same place: a list, as a voice filter's is.
inline Rect filterTypeListButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}

// A list button's arrows, which step its choice along the list (see View::stepList()): `dir` -1 the one before, at its
// left end, and 1 the one after, at its right end, where other lists' chevron is. Its name goes between them.
constexpr float kListStepW = 18.0f;
inline Rect listStepButton(Rect button, int dir) {
    return {dir > 0 ? button.right() - 1.0f - kListStepW : button.x + 1.0f, button.y + 1.0f, kListStepW,
            button.h - 2.0f};
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

// The effect's icon, at the left of its card's title row, folded or not: centered with the title (kCardTitleY) and the
// buttons, as a source card's handle is.
inline Rect fxIconRect(Rect card) {
    return {card.x + kFxIconX, card.y + 11.0f, kFxIconW, 14.0f};
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
                         type == FxType::Utility || type == FxType::Disperser;
    // The filter's slope switch, the delay's SYNC, LINK and PING-PONG, the phaser's and flanger's SYNC, STEREO and
    // polarity and the chorus's voices go under their displays.
    const bool switchesUnder = type == FxType::Phaser || type == FxType::Flanger || type == FxType::Chorus ||
                               type == FxType::Filter || type == FxType::Delay;
    // The shaper's LINK goes between its display and its knobs; the reverb's three rows of knobs (kReverbKnobsY)
    // take more of it.
    const float bottom = type == FxType::Reverb      ? 130.0f
                         : switchesUnder             ? 163.0f
                         : type == FxType::ParamEq   ? 180.0f // its chips under it (kPeqChipY)
                         : twoRows                   ? 185.0f
                         : type == FxType::Shaper    ? 195.0f
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
    return {ampLeft(card).center().x - 22.0f, card.y + 154.0f, 44.0f, 18.0f}; // centered under the amp's display
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
inline float keyboardTop() {
    return kModPanel.bottom() + 16.0f;
}
inline Rect keyboardPanel(float top, bool collapsed) {
    return {16.0f, top, 1168.0f, collapsed ? kKeyboardBarH : kKeyboardH};
}
// The chevron and the title, as far from the strip's top as the modulation panel's from its.
inline Rect keysToggle(float top) {
    const Rect sources = modTitle();
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
// The preset bar, in the middle of the title bar, as tall as the row's buttons: back, the preset's category, name and
// author on one line, forward.
constexpr Rect kPresetName{410.0f, kTitleRowY, 380.0f, kTopRowH};
inline Rect presetStep(int direction) {
    return {direction < 0 ? kPresetName.x - 30.0f : kPresetName.right() + 4.0f, kPresetName.y, 26.0f, kPresetName.h};
}
// The tempo: a field midway between the name and the preset bar's back arrow, and at its left the H that has the synth
// follow the host's tempo.
constexpr Rect kTempoSync{215.0f, kTitleRowY, 24.0f, kTopRowH};
constexpr Rect kTempoBox{kTempoSync.x + kTempoSync.w + 4.0f, kTitleRowY, 76.0f, kTopRowH};
// At the tempo's right, while the sequencer is on: the SEQ card's Play button again, its icon alone, whatever view
// shows.
constexpr Rect kTitleSeqPreview{kTempoBox.x + kTempoBox.w + 4.0f, kTitleRowY, 26.0f, kTopRowH};
static_assert(kTitleSeqPreview.x + kTitleSeqPreview.w + 4.0f <= kPresetName.x - 30.0f); // clear of the back arrow
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
// How far a key's light moves each poll as it eases up to lit and back down, the keyboard strip's and the SEQ view's
// roll's: about 35 ms up and 80 ms down at 60 polls a second.
constexpr float kKeyLightRise = 0.5f, kKeyLightFall = 0.22f;

// Position in the grid: rows are nodes, columns nodes and then the output.
inline Rect gridCell(int row, int col) {
    return {kGridOrigin.x + col * kCellStepX, kGridOrigin.y + row * kCellStepY, kCellW, kCellH};
}

inline Rect knobRect(Point center, float diameter) {
    return {center.x - diameter * 0.5f, center.y - diameter * 0.5f, diameter, diameter + 18.0f};
}

// A depth label in the routing graph, a chip centered on its wire: kWireLabelH tall, as wide as a wire's (a line to
// OUT's a little wider, a loop's wider still, for its "fb"), its depth in kTextBody. A mode other than Phase widens it
// by kLabelGlyphW. Open into a pill, its mode's part goes on its left and its x on its right. Its depth handles go
// under it in a knob's size, as far apart as under a knob (see View::widgets()).
constexpr float kWireLabelH = 19.0f;
constexpr float kWireLabelW = 46.0f, kOutLabelW = 48.0f, kLoopLabelW = 54.0f;
constexpr float kLabelGlyphW = 20.0f;
constexpr float kLabelModeW = 32.0f, kLabelRemoveW = 22.0f;
// A loop's label beside it on its right, its left edge 10 px right of the loop's center.
constexpr float kLoopLabelX = 10.0f + 0.5f * kLoopLabelW;
inline Rect chip(Point center, float width = kWireLabelW) {
    return {center.x - width * 0.5f, center.y - 0.5f * kWireLabelH, width, kWireLabelH};
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
constexpr float kSettingsH = 642.0f;
constexpr float kSettingsMpeY = 184.0f;  // the divider above MPE
constexpr float kSettingsLookY = 294.0f; // and above LOOK
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
// The sliders' and HANDLES' names, before them: wide enough for HANDLES in any of the fonts the editor draws in.
constexpr float kSettingsLabelW = 64.0f;
constexpr float kSettingsTrackX = 24.0f + kSettingsLabelW; // from the panel's left
constexpr float kSettingsTrackW = 276.0f - kSettingsTrackX; // ending where they always have
// Under the themes, the hue slider's track, with room above and below to grab it: the theme's main color. HUE before
// it, its hue after.
inline Rect settingsHue(Rect panel) {
    return {panel.x + kSettingsTrackX, panel.y + kSettingsLookY + 72.0f, kSettingsTrackW, 26.0f};
}
// The glow slider, like it; GLOW before it, its amount after.
inline Rect settingsGlow(Rect panel) {
    return {panel.x + kSettingsTrackX, panel.y + kSettingsLookY + 104.0f, kSettingsTrackW, 26.0f};
}
// Under it, the LINES slider, like it: how strongly a source's dashed lines are drawn.
inline Rect settingsLinks(Rect panel) {
    return {panel.x + kSettingsTrackX, panel.y + kSettingsLookY + 136.0f, kSettingsTrackW, 26.0f};
}
// Under it, the GRID slider, like it: how strongly the SEQ view's roll draws its grid.
inline Rect settingsGrid(Rect panel) {
    return {panel.x + kSettingsTrackX, panel.y + kSettingsLookY + 168.0f, kSettingsTrackW, 26.0f};
}
// Under that, HANDLES and the depth handles' sizes: small, medium and large.
inline Rect settingsHandles(Rect panel, int size) {
    constexpr float kStep = (kSettingsTrackW + 4.0f) / 3.0f;
    return {panel.x + kSettingsTrackX + size * kStep, panel.y + kSettingsLookY + 202.0f, kStep - 4.0f, 28.0f};
}
// Under them, CARDS and its Plain (0) and Tinted (1), as wide as two of the handles' sizes.
inline Rect settingsCards(Rect panel, int tinted) {
    constexpr float kStep = (kSettingsTrackW + 4.0f) / 3.0f;
    return {panel.x + kSettingsTrackX + tinted * kStep, panel.y + kSettingsLookY + 236.0f, kStep - 4.0f, 28.0f};
}
inline Rect settingsClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}
// Before it, the button that opens the user manual.
inline Rect settingsManual(Rect panel) {
    return {panel.right() - 70.0f, panel.y + 14.0f, 24.0f, 24.0f};
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

// On screen, at least in part, for a card `span` places wide, of a row that shows `shown` at a time.
inline bool onScreen(float place, int shown, float span = 1.0f) {
    return place > -span && place < static_cast<float>(shown);
}

// The top left of a list as wide and high as `menu` that opens at the mouse, moved back inside the editor (as high as
// `height`) where it would reach past an edge.
inline Point menuAt(Point mouse, Rect menu, float height) {
    constexpr float kMargin = 4.0f;
    return {std::clamp(mouse.x, kMargin, std::max(View::kWidth - kMargin - menu.w, kMargin)),
            std::clamp(mouse.y, kMargin, std::max(height - kMargin - menu.h, kMargin))};
}

} // namespace fmvibe::gui::detail
