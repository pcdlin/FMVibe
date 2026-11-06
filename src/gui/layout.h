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
constexpr float kPanelRadius = 10.0f; // the panels' corners
constexpr Rect kRouting{16.0f, kPanelsTop, 852.0f, 372.0f};
constexpr Rect kInspector{884.0f, kPanelsTop, 300.0f, 372.0f};
constexpr Rect kGraph{kRouting.x + 40.0f, kRouting.y + 70.0f, kRouting.w - 80.0f, kRouting.h - 146.0f}; // node centers
constexpr float kBusY = kRouting.y + kRouting.h - 30.0f;
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
// An operator's Fixed switch, a keyboard on the summary line left of the power switch.
constexpr Rect kFixedSwitch{kInspector.x + kInspector.w - 16.0f - 26.0f - 30.0f, kInspector.y + 37.0f, 26.0f, 19.0f};
// An operator's Ratio: a 7-segment readout, 0.50 to 64.00, over the first two places of its knob row, each digit dragged
// on its own; its widget runs on under the box to the label, as a knob's does (see ratioDigit()). The track runs along
// its top, clear of the depth handles along its bottom edge.
constexpr Rect kRatioBox{kInspector.x + 20.0f, kInspector.y + 90.0f, (kInspector.w - 32.0f) / 3.0f - 8.0f, 36.0f};
constexpr int kRatioDigits = 4; // tens, ones, tenths, hundredths
constexpr long kRatioPlaces[kRatioDigits] = {1000, 100, 10, 1}; // in hundredths
constexpr float kRatioDigitW = 14.0f, kRatioDigitH = 20.0f, kRatioDigitGap = 2.5f, kRatioPointW = 6.0f;
// Digit k's place in the box (the decimal point after the second).
inline Rect ratioDigitRect(int k) {
    const float total = kRatioDigits * kRatioDigitW + (kRatioDigits - 1) * kRatioDigitGap + kRatioPointW;
    const float left = kRatioBox.x + (kRatioBox.w - total) * 0.5f;
    const float x = left + k * (kRatioDigitW + kRatioDigitGap) + (k >= 2 ? kRatioPointW : 0.0f);
    return {x, kRatioBox.y + 10.0f, kRatioDigitW, kRatioDigitH};
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
// An operator's page: under its knob row, tabs (one per InspectorTab) pick what the lower half shows.
constexpr float kOpTabsY = kInspector.y + 156.0f;
constexpr float kOpTabH = 20.0f;
inline Rect opTab(int tab) {
    // UNISON, ENVELOPE, SCOPE, INPUTS by InspectorTab; along the line ENVELOPE, UNISON, INPUTS, SCOPE
    constexpr float kX[] = {78.0f, 0.0f, 200.0f, 144.0f}, kW[] = {62.0f, 74.0f, 54.0f, 52.0f};
    return {kInspector.x + 16.0f + kX[tab], kOpTabsY, kW[tab], kOpTabH};
}
// Inputs: a row per route into the operator, its source's name, its mode's buttons, the mode's name and its depth,
// the rows closer together when there are many.
constexpr float kInputsTop = kInspector.y + 186.0f;
constexpr float kInputsBottom = kInspector.y + kInspector.h - 10.0f;
inline float inputsStep(int rows) {
    return std::min(22.0f, (kInputsBottom - kInputsTop) / static_cast<float>(std::max(rows, 1)));
}
inline Rect inputsName(float y) { return {kInspector.x + 16.0f, y, 26.0f, 18.0f}; }
inline Rect inputsMode(float y, int mode) { return {kInspector.x + 48.0f + mode * 26.0f, y, 26.0f, 18.0f}; }
inline Rect inputsModeName(float y) { return {kInspector.x + 158.0f, y, 74.0f, 18.0f}; }
inline Rect inputsDepth(float y) { return {kInspector.x + kInspector.w - 16.0f - 48.0f, y, 48.0f, 18.0f}; }
// Envelope: the curve, its knobs under it at y + 322.
constexpr Rect kOpEnvRect{kInspector.x + 16.0f, kInspector.y + 184.0f, kInspector.w - 32.0f, 110.0f};
// Unison: the knobs at y + 206, where the copies sit under them.
constexpr float kUnisonKnobsY = kInspector.y + 206.0f;
constexpr Rect kUnisonRect{kInspector.x + 16.0f, kInspector.y + 248.0f, kInspector.w - 32.0f, 110.0f};
// A noise operator's spectrum, in Unison's place.
constexpr Rect kNoiseRect{kInspector.x + 16.0f, kInspector.y + 184.0f, kInspector.w - 32.0f, 110.0f};
// Scope: the operator's output, the cycle count's buttons in its top right corner.
constexpr Rect kScopeRect{kInspector.x + 16.0f, kInspector.y + 184.0f, kInspector.w - 32.0f, 174.0f};
inline Rect scopeCycleButton(int choice) {
    return {kScopeRect.right() - 6.0f - (3 - choice) * 20.0f, kScopeRect.y + 6.0f, 20.0f, 16.0f};
}
constexpr Rect kFilterDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 112.0f};
// A filter's slope list, at the right end of its row of type buttons.
constexpr Rect kFilterSlopeButton{kInspector.x + kInspector.w - 16.0f - 62.0f, kInspector.y + 62.0f, 62.0f, 22.0f};
// A shaper's page: its curve's list and the link switch in the row under the summary, with the symmetric switch
// between them for a Custom curve, then its transfer curve where a filter's response shows.
constexpr Rect kShaperLinkButton{kInspector.x + kInspector.w - 16.0f - 44.0f, kInspector.y + 62.0f, 44.0f, 22.0f};
constexpr Rect kShaperSymButton{kShaperLinkButton.x - 50.0f, kShaperLinkButton.y, 44.0f, 22.0f};
inline Rect shaperModeButton(bool custom) {
    return {kInspector.x + 16.0f, kInspector.y + 62.0f, kInspector.w - 32.0f - (custom ? 100.0f : 50.0f), 22.0f};
}
constexpr Rect kShaperDisplay = kFilterDisplay;
// An operator's wave buttons: the common waves, then "..." with a list of the others.
constexpr OpWave kCommonWaves[] = {OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Noise};
constexpr OpWave kMoreWaves[] = {OpWave::Pulse,   OpWave::HalfSine, OpWave::AbsSine, OpWave::AltSine,
                                 OpWave::Organ,   OpWave::Vowel,    OpWave::Custom};
static_assert(std::size(kCommonWaves) + std::size(kMoreWaves) == static_cast<size_t>(OpWave::Count));
constexpr int kWaveButtons = static_cast<int>(std::size(kCommonWaves)) + 1;
inline Rect waveButton(int button) {
    constexpr float kStep = (kInspector.w - 32.0f + 4.0f) / kWaveButtons;
    return {kInspector.x + 16.0f + button * kStep, kInspector.y + 62.0f, kStep - 4.0f, 22.0f};
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
constexpr Rect kViewTabs[] = {{16.0f, kTopRowY, 96.0f, kViewTabH},
                               {116.0f, kTopRowY, 88.0f, kViewTabH},
                               {208.0f, kTopRowY, 64.0f, kViewTabH},
                               {276.0f, kTopRowY, 76.0f, kViewTabH}};
constexpr float kViewTabRadius = 8.0f; // its top corners
constexpr float kViewTabFlare = 6.0f;  // the shown one's curves into the panel's edge
// The FX view covers the routing panel and the inspector: one card per effect,
// left to right in processing order, then a card to add one; kVisibleFx at a time.
constexpr Rect kFxPanel{kRouting.x, kRouting.y, kInspector.x + kInspector.w - kRouting.x, kRouting.h};
constexpr int kVisibleFx = 6;
constexpr float kFxGap = 22.0f; // room for the plus that adds an effect between two cards
constexpr float kFxCardW = (kFxPanel.w - 32.0f - (kVisibleFx - 1) * kFxGap) / kVisibleFx;
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
// The first divider, before the play cluster.
constexpr float kDivider0 = kHeaderStart + 535.0f;
// The global knobs, 120 px apart, centered between the meter and the first divider: from the first one's arc (2 px
// into its place) to the Pan knob's widest value ("100 L", 78 px into its place), 43 px from each.
constexpr float kHeaderKnobW = 120.0f;
constexpr float kHeaderKnobX = kHeaderStart + 55.0f;
static_assert(kHeaderKnobX + 2.0f - (kHeaderStart + 14.0f) == 43.0f &&
              kDivider0 - (kHeaderKnobX + 3.0f * kHeaderKnobW + 78.0f) == 42.0f);
constexpr float kPlayX = kDivider0 + 44.0f; // Poly | Mono over Legato, 44 px past the first divider
// The Glide knob 24 px past them, its Always switch (on its label's row) ending 32 px before the second divider.
constexpr float kGlideX = kPlayX + 112.0f;
constexpr float kGlideW = 126.0f;
constexpr float kDividers[] = {kDivider0, kGlideX + kGlideW + 10.0f + 32.0f};
// Voices, 53 px past the second divider, right beside PLAYING (whose word starts at about 1134 px): its own place
// narrower than the global knobs', so it stops short of the count.
constexpr float kVoicesX = kDividers[1] + 53.0f;
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

// A card by its screen place: 0..3, fractions while it scrolls.
// `span`: the card places it takes, less than one folded.
inline Rect sourceCard(float card, float span = 1.0f) {
    return {kModPanel.x + 12.0f + card * (kCardW + kCardGap), kCardTop, span * (kCardW + kCardGap) - kCardGap, kCardH};
}
// A folded card is a narrow strip: its handle, a grip, its switch and cross, then its name down the rest, which
// unfolds it. An effect card folds much the same way: its icon, which switches it, then its cross and its name.
// That strip is as wide as the icon's place at the title's left, so the icon stays put as the card folds.
constexpr float kFoldedSource = 0.2f;
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
// That list, its top left given; an entry a type.
inline Rect sourceMenu(Point at) { return {at.x, at.y, 150.0f, 8.0f + kSourceTypes * 26.0f}; }
inline Rect sourceMenuItem(Point at, SourceType type) {
    const Rect menu = sourceMenu(at);
    return {menu.x + 4.0f, menu.y + 4.0f + (static_cast<int>(type) - 1) * 26.0f, menu.w - 8.0f, 24.0f};
}
// A MIDI card's input (CC, Pressure or Slide), a list button under its title at the left, where an LFO's shape is;
// its CC and Slew knobs at the right.
inline Rect midiInputButton(Rect card) { return {card.x + 16.0f, card.y + 42.0f, 122.0f, 22.0f}; }
constexpr float kSourceShadowSpan = (kSourceShadowW + kCardGap) / (kCardW + kCardGap);
constexpr float kFxIconX = 10.0f; // the icon's left edge, from the card's
constexpr float kFxIconW = 18.0f;
constexpr float kFoldedFx = (2.0f * kFxIconX + kFxIconW + kFxGap) / (kFxCardW + kFxGap);

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

// + LFO, + ENV, + RANDOM and + MIDI, after the toggle; shaped like ADD EFFECT.
inline Rect sourceAddButton(SourceType type) {
    const int k = static_cast<int>(type) - static_cast<int>(SourceType::Lfo);
    return {kAddSourceX + 88.0f * static_cast<float>(k), kModPanel.y + 9.0f, 82.0f, 22.0f};
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

// Title row: handle and name (the row drags the card), then (LFOs) SYNC, the power switch and the remove cross at the
// right; of a card `span` places wide.
inline Rect sourceRemove(float card, float span = 1.0f) {
    const Rect p = sourceCard(card, span);
    return {p.right() - 26.0f, p.y + 7.0f, 18.0f, 21.0f};
}

inline Rect sourcePower(float card, float span = 1.0f) {
    const Rect p = sourceCard(card, span);
    return {p.right() - 30.0f - 26.0f, p.y + 8.0f, 26.0f, 19.0f};
}
// A Custom LFO's normal card: left of the fold button (which is left of the power switch), the button that widens it.
inline Rect lfoWidenButton(float card) {
    const float fold = sourcePower(card).x - 4.0f - 18.0f;
    return {fold - 4.0f - 28.0f, sourceCard(card).y + 7.0f, 28.0f, 22.0f};
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
// the three-band dynamics takes.
constexpr FxType kFxOffered[] = {FxType::Shaper,     FxType::Delay,   FxType::Eq,          FxType::ParamEq,
                                 FxType::Filter,     FxType::Compressor, FxType::MultiDyn, FxType::Reverb,
                                 FxType::Phaser,     FxType::Flanger, FxType::FreqShifter, FxType::Disperser,
                                 FxType::Widener};
constexpr int kFxTypes = static_cast<int>(std::size(kFxOffered));
static_assert(kFxTypes == static_cast<int>(FxType::Count) - 2, "a new effect type goes in the list of effects");
constexpr int fxOfferedPlace(FxType type) {
    for (int i = 0; i < kFxTypes; ++i)
        if (kFxOffered[i] == type)
            return i;
    return 0;
}
// The list's top left is under the button, or (from the shadow card after the effects) given. Its entries are close
// enough together that it fits in the panel under the button.
constexpr float kFxMenuPitch = 23.0f;
inline Rect fxMenu(Point at) {
    const Rect button = fxMenuButton();
    return {at.x < 0.0f ? button.x : at.x, at.x < 0.0f ? button.bottom() + 4.0f : at.y, 180.0f,
            8.0f + kFxTypes * kFxMenuPitch};
}
static_assert(9.0f + 22.0f + 4.0f + 8.0f + kFxTypes * kFxMenuPitch <= kFxPanel.h, // from under the button
              "the list of effects fits in the panel");

inline Rect fxMenuItem(Point at, FxType type) {
    const Rect menu = fxMenu(at);
    return {menu.x + 4.0f, menu.y + 4.0f + static_cast<float>(fxOfferedPlace(type)) * kFxMenuPitch, menu.w - 8.0f,
            kFxMenuPitch - 2.0f};
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

// The filter effect's slope list, under its display in the middle.
inline Rect fxSlopeButton(Rect card, Rect display) {
    return {card.center().x - 36.0f, display.bottom() + 4.0f, 72.0f, 18.0f};
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
inline Rect fxCardClip() {
    return {kFxPanel.x + 8.0f, kFxTop - 4.0f, kFxPanel.w - 16.0f, kFxPanel.bottom() - 20.0f - kFxTop + 8.0f};
}

// How many of the usual card places a card takes: the parametric EQ's and the three-band dynamics' are wider, for
// their displays and knobs.
inline float fxSpan(FxType type) {
    return type == FxType::ParamEq || type == FxType::MultiDyn ? 1.5f : 1.0f;
}

// A card's rect by its screen place, in card places from the row's left end (0..kVisibleFx, fractions while
// it scrolls), and how many places it takes.
inline Rect fxCard(float position, float span = 1.0f) {
    return {kFxPanel.x + 16.0f + position * (kFxCardW + kFxGap), kFxTop, span * (kFxCardW + kFxGap) - kFxGap,
            kFxPanel.bottom() - 20.0f - kFxTop};
}

// The effect's icon, at the left of its card's title row, folded or not.
inline Rect fxIconRect(Rect card) {
    return {card.x + kFxIconX, card.y + 10.0f, kFxIconW, 14.0f};
}

// Displays sit under a card's buttons, if it has any, and above its knobs: one
// row of them, or two for the delay, the parametric EQ and the effects with six knobs or so.
inline Rect fxDisplay(float position, FxType type) {
    const Rect card = fxCard(position, fxSpan(type));
    if (type == FxType::MultiDyn) // its switches in the title row, its three rows of knobs under it
        return {card.x + 14.0f, card.y + 42.0f, card.w - 28.0f, 72.0f};
    const bool buttons = type == FxType::Shaper || type == FxType::Reverb;
    const bool buttonRow = buttons || type == FxType::Filter;
    const float top = buttonRow ? 74.0f : 44.0f;
    const bool twoRows = type == FxType::Delay || type == FxType::ParamEq || type == FxType::Compressor ||
                         type == FxType::Reverb || type == FxType::Phaser || type == FxType::Flanger ||
                         type == FxType::Filter || type == FxType::FreqShifter;
    // The filter's slope switch, the delay's SYNC, LINK and PING-PONG and the phaser's and flanger's SYNC and STEREO
    // go under their displays.
    const bool switchesUnder = type == FxType::Phaser || type == FxType::Flanger || type == FxType::Filter ||
                               type == FxType::Delay;
    // The shaper's LINK goes between its display and its knobs.
    const float bottom = switchesUnder               ? 158.0f
                         : twoRows                   ? 180.0f
                         : type == FxType::Shaper    ? 190.0f
                                                     : 198.0f;
    return {card.x + 14.0f, card.y + top, card.w - 28.0f, bottom - top};
}

// The three-band dynamics' display: a level bar a band, the high band on top, each its name on the left and its gain
// on the right, with a scale in dB under them. Levels run from -kMdRangeDb at the left to 0 dB at the right.
constexpr float kMdRangeDb = 60.0f;
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
inline Rect mdRmsButton(Rect card) { return {card.right() - 122.0f, card.y + 8.5f, 34.0f, 19.0f}; }
inline Rect mdLinkButton(Rect card) { return {card.right() - 84.0f, card.y + 8.5f, 30.0f, 19.0f}; }
// Its rows of knobs: the band's own (Above, Down, Below, Up, Gain), a line, then the shared ones in two rows.
constexpr float kMdBandKnobsY = 142.0f, kMdLineY = 186.5f, kMdSharedY1 = 214.0f, kMdSharedY2 = 266.0f;

// The parametric EQ's plot: frequency across, gain up and down.
constexpr float kPeqRangeDb = 20.0f;
inline Point peqAt(Rect area, double freq, double db) {
    return {area.x + static_cast<float>(freq) * area.w,
            area.center().y - static_cast<float>(db) / kPeqRangeDb * area.h * 0.5f};
}

// The bands' chips, in a row under the display: each its power switch, its number and its shape. A click picks the
// band the row of knobs under them sets.
constexpr float kPeqChipY = 188.0f, kPeqChipH = 26.0f, kPeqKnobsY = 252.0f;
inline Rect peqChip(Rect card, int band) {
    const float w = (card.w - 28.0f - 4.0f * (kPeqBands - 1)) / kPeqBands;
    return {card.x + 14.0f + band * (w + 4.0f), card.y + kPeqChipY, w, kPeqChipH};
}
inline Rect peqChipPower(Rect chip) { return {chip.x, chip.y, 26.0f, chip.h}; }
inline Rect peqChipShape(Rect chip) { return {chip.right() - 34.0f, chip.y, 34.0f, chip.h}; }

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
// title, the macros, the note source and velocity, and under them the wheels on the left and six octaves of keys,
// the octave shift under their right end, the mod wheel's handle in the top row. Collapsed, only the top row shows.
constexpr float kKeyboardH = 192.0f;
constexpr float kKeyboardBarH = 72.0f; // the top row, the names under it included: all that shows while collapsed
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
// The bend range's knob, header-style (the knob, its label and value beside it), over the bend wheel, below the
// strip's title: within its top row, drawn smaller than the header knobs (see Widget::shrunk). Its knob's center, 16 px
// into the rect, lies over the bend wheel's middle.
inline Rect bendKnob(float top) { return {31.0f, top + 36.0f, 82.0f, 34.0f}; }
inline Rect bendWheel(float top) {
    return {34.0f, top + kKeys.y, 26.0f, 90.0f};
}
inline Rect modWheel(float top) {
    return {74.0f, top + kKeys.y, 26.0f, 90.0f};
}
// The mod wheel's handle, to drag onto what it moves, as a macro's: in the top row between the macros and the note
// source, its name under it, whether the keys show or not.
inline Rect wheelHandle(float top) {
    return {918.0f, top + kStripRow - 10.0f, 20.0f, 20.0f};
}
constexpr int kKeyCount = 73; // C to C, six octaves
constexpr int kWhiteKeys = 43;
constexpr float kWhiteW = kKeys.w / kWhiteKeys;
constexpr float kBlackW = kWhiteW * 0.6f;
constexpr float kBlackH = kKeys.h * 0.6f;
constexpr int kBaseNote = 24; // C1, where the keyboard starts before any octave shift
// Init, Load, Save and the settings cog: icons at the right end of the title bar.
inline Rect toolbarButton(int i) {
    constexpr float kW = 28.0f, kGap = 6.0f;
    return {kInspector.right() - (4 - i) * (kW + kGap) + kGap, kTitleRowY, kW, kTopRowH};
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
constexpr float kSettingsH = 562.0f;
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
// The glow slider's track, with room above and below to grab it; GLOW before it, its amount after.
inline Rect settingsGlow(Rect panel) {
    return {panel.x + 76.0f, panel.y + kSettingsLookY + 72.0f, 200.0f, 26.0f};
}
// Under it, the LINES slider, like it: how strongly a source's dashed lines are drawn.
inline Rect settingsLinks(Rect panel) {
    return {panel.x + 76.0f, panel.y + kSettingsLookY + 104.0f, 200.0f, 26.0f};
}
// Under that, HANDLES and the depth handles' sizes: small, medium and large.
inline Rect settingsHandles(Rect panel, int size) {
    return {panel.x + 76.0f + size * 68.0f, panel.y + kSettingsLookY + 138.0f, 64.0f, 28.0f};
}
inline Rect settingsClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}

// The dialog asking before Init or Load replaces a changed patch, centered like the settings; a line shorter when
// it doesn't say what replaces the patch.
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
