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
constexpr float kPanelsTop = 104.0f;
constexpr float kPanelRadius = 10.0f; // the panels' corners
constexpr Rect kRouting{16.0f, kPanelsTop, 852.0f, 372.0f};
constexpr Rect kInspector{884.0f, kPanelsTop, 300.0f, 372.0f};
constexpr Rect kGraph{kRouting.x + 40.0f, kRouting.y + 70.0f, kRouting.w - 80.0f, kRouting.h - 146.0f}; // node centers
constexpr float kBusY = kRouting.y + kRouting.h - 30.0f;
constexpr float kNodeRadius = 24.0f;
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
// An operator's page: under its knob row, tabs (one per InspectorTab) pick what the lower half shows.
constexpr float kOpTabsY = kInspector.y + 156.0f;
constexpr float kOpTabH = 20.0f;
inline Rect opTab(int tab) {
    constexpr float kX[] = {78.0f, 0.0f, 144.0f}, kW[] = {62.0f, 74.0f, 54.0f}; // ENVELOPE, UNISON, SCOPE
    return {kInspector.x + 16.0f + kX[tab], kOpTabsY, kW[tab], kOpTabH};
}
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
// The row between the header and the panels: the view tabs at its left, the preset bar in its middle, the toolbar
// at its right.
constexpr float kTopRowY = 72.0f;
constexpr float kTopRowH = 24.0f;
// Tabs of the routing panel, one per RoutingView: folder tabs from the row's top down onto the panels' top edge,
// the first one's left side over the panel's left edge, which it runs on into while it is shown.
constexpr float kViewTabH = kPanelsTop - kTopRowY;
constexpr Rect kViewTabs[] = {
    {16.0f, kTopRowY, 96.0f, kViewTabH}, {116.0f, kTopRowY, 88.0f, kViewTabH}, {208.0f, kTopRowY, 64.0f, kViewTabH}};
constexpr float kViewTabRadius = 8.0f; // its top corners
constexpr float kViewTabFlare = 6.0f;  // the shown one's curves into the panel's edge
// The FX view covers the routing panel and the inspector: one card per effect,
// left to right in processing order, then a card to add one; kVisibleFx at a time.
constexpr Rect kFxPanel{kRouting.x, kRouting.y, kInspector.x + kInspector.w - kRouting.x, kRouting.h};
constexpr int kVisibleFx = 6;
constexpr float kFxGap = 22.0f; // room for the plus that adds an effect between two cards
constexpr float kFxCardW = (kFxPanel.w - 32.0f - (kVisibleFx - 1) * kFxGap) / kVisibleFx;
constexpr float kFxTop = kRouting.y + 48.0f;
// Header: global knobs, the play cluster between two dividers, then the voice controls.
constexpr float kHeaderKnobX = 186.0f;
constexpr float kHeaderKnobW = 104.0f;
constexpr float kPlayX = 624.0f;
constexpr float kDividers[] = {612.0f, 896.0f};
// The output meter, left of the Volume knob: a bar for each side, from -48 dB at the bottom to +6 dB at the top, red
// past full scale.
constexpr Rect kMeter{158.0f, 13.0f, 14.0f, 38.0f};
constexpr double kMeterFloorDb = -48.0;
constexpr double kMeterTopDb = 6.0;
constexpr Color kMeterRed = Color::hex(0xEF4444);

// The modulation panel: a title row with a chip per source (drag one onto a
// param to modulate it, click it to bring its card into view) and the buttons
// that add sources, then the sources' cards, kVisibleCards at a time, in the
// order they show (UiLayout::sourceOrder).
// Open, tall enough for a card's four slot rows to end 8 px above its bottom.
constexpr Rect kModPanel{16.0f, kPanelsTop + 372.0f + 16.0f, 1168.0f, 322.0f};
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
// The dashed cards after the sources: an LFO's above an envelope's, a little apart, each of them square.
constexpr float kSourceShadowGap = 10.0f;
constexpr float kSourceShadowW = (kCardH - kSourceShadowGap) * 0.5f;
constexpr float kSourceShadowSpan = (kSourceShadowW + kCardGap) / (kCardW + kCardGap);
constexpr float kFxIconX = 10.0f; // the icon's left edge, from the card's
constexpr float kFxIconW = 18.0f;
constexpr float kFoldedFx = (2.0f * kFxIconX + kFxIconW + kFxGap) / (kFxCardW + kFxGap);

// The cards are cut off at the row's ends while they scroll.
inline Rect sourceCardClip() {
    return {kModPanel.x + 6.0f, kCardTop - 4.0f, kModPanel.w - 12.0f, kCardH + 8.0f};
}

// ADD LFO and ADD ENV, after the toggle; shaped like ADD EFFECT.
inline Rect sourceAddButton(SourceType type) {
    return {kAddSourceX + (type == SourceType::Env ? 88.0f : 0.0f), kModPanel.y + 9.0f, 82.0f, 22.0f};
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

// The used slots, kVisibleSlots rows at a time; `row` counts from the top of the list, fractions while it
// scrolls and the rows at its ends are cut off.
constexpr int kVisibleSlots = 4;
constexpr float kSlotNameW = 90.0f; // the target's name, between the bypass dot and the polarity switch
inline Rect slotRow(float card, float row) {
    const Rect p = sourceCard(card);
    return {p.x + 12.0f, p.y + 160.0f + row * 24.0f, p.w - 24.0f, 20.0f};
}

// Where the rows go, and the scrollbar beside them when there are more rows than fit.
inline Rect slotList(float card) {
    const Rect first = slotRow(card, 0);
    return {first.x, first.y, first.w + 10.0f, slotRow(card, kVisibleSlots - 1).bottom() - first.y};
}

inline Rect slotScrollTrack(float card) {
    const Rect list = slotList(card);
    return {list.right() - 6.0f, list.y + 2.0f, 3.0f, list.h - 4.0f};
}

// Where the rows of the list show: within the list and within the row of cards.
inline Rect slotClip(float card, Rect cards) {
    const Rect list = slotList(card);
    const float x = std::max(list.x - 2.0f, cards.x), y = std::max(list.y, cards.y);
    const float right = std::min(list.right(), cards.right()), bottom = std::min(list.bottom(), cards.bottom());
    return {x, y, std::max(right - x, 0.0f), std::max(bottom - y, 0.0f)};
}

// Title row: handle and name (the row drags the card), then (LFOs) SYNC, the power switch and the remove cross at the right.
inline Rect sourceRemove(float card) {
    const Rect p = sourceCard(card);
    return {p.right() - 26.0f, p.y + 7.0f, 18.0f, 21.0f};
}

inline Rect sourcePower(float card) {
    const Rect p = sourceCard(card);
    return {p.right() - 30.0f - 26.0f, p.y + 8.0f, 26.0f, 19.0f};
}

// An LFO's shape list (a button with the shape's icon and name, and the list it opens under it) over its free,
// one-shot and unipolar switches (0..2, as icons), the two rows as wide.
inline Rect lfoModeSwitch(float card, int mode) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f + mode * 42.0f, p.y + 72.0f, 38.0f, 16.0f};
}

inline Rect lfoShapeButton(float card) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 42.0f, lfoModeSwitch(card, 2).right() - p.x - 16.0f, 22.0f};
}

// Right of those two rows, the Rate (0) and Phase (1) knobs with their labels, centered on them, each over a random
// LFO's Smooth and Chaos; Rate's sync switch, a small square, sits between the rows and its knob.
inline Point lfoKnob(float card, int knob) {
    const Rect top = lfoShapeButton(card), bottom = lfoModeSwitch(card, 2);
    return {randomKnob(card, knob).x, (top.y + bottom.bottom()) * 0.5f - 9.0f}; // the label takes 18 px under the knob
}

inline Rect sourceSync(float card) {
    const Point rate = lfoKnob(card, 0);
    const float x = (lfoModeSwitch(card, 2).right() + rate.x - 15.0f) * 0.5f;
    return {x - 10.0f, rate.y - 10.0f, 20.0f, 20.0f};
}

// ADD EFFECT at the top left, and the list it opens under it: one row per type,
// then how many places are left.
inline Rect fxMenuButton() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + 9.0f, 116.0f, 22.0f};
}

constexpr int kFxTypes = static_cast<int>(FxType::Count) - 1; // without None
// The list's top left is under the button, or (from the shadow card after the effects) given.
inline Rect fxMenu(Point at) {
    const Rect button = fxMenuButton();
    return {at.x < 0.0f ? button.x : at.x, at.x < 0.0f ? button.bottom() + 4.0f : at.y, 180.0f, 8.0f + kFxTypes * 26.0f};
}

inline Rect fxMenuItem(Point at, FxType type) {
    const Rect menu = fxMenu(at);
    return {menu.x + 4.0f, menu.y + 4.0f + (static_cast<int>(type) - 1) * 26.0f, menu.w - 8.0f, 24.0f};
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

// How many of the usual card places a card takes: the parametric EQ's is wider, for its spectrum and knobs.
inline float fxSpan(FxType type) {
    return type == FxType::ParamEq ? 1.5f : 1.0f;
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
    const bool buttons = type == FxType::Shaper || type == FxType::MultiComp || type == FxType::Reverb;
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
                         : type == FxType::MultiComp ? 140.0f
                         : type == FxType::Shaper    ? 190.0f
                                                     : 198.0f;
    return {card.x + 14.0f, card.y + top, card.w - 28.0f, bottom - top};
}

// The parametric EQ's plot: frequency across, gain up and down.
constexpr float kPeqRangeDb = 20.0f;
inline Point peqAt(Rect area, double freq, double db) {
    return {area.x + static_cast<float>(freq) * area.w,
            area.center().y - static_cast<float>(db) / kPeqRangeDb * area.h * 0.5f};
}

// The list of band shapes, which opens upward from its button over the card's display.
constexpr int kPeqTypes = static_cast<int>(PeqType::Count);
inline Rect peqMenu(Rect button) {
    constexpr float kH = 8.0f + kPeqTypes * 22.0f;
    return {button.x, button.y - 4.0f - kH, 124.0f, kH};
}

inline Rect peqMenuItem(Rect button, int type) {
    const Rect menu = peqMenu(button);
    return {menu.x + 4.0f, menu.y + 4.0f + type * 22.0f, menu.w - 8.0f, 22.0f};
}

// A band's type button, in the first place of its row of knobs.
inline Rect peqTypeButton(Rect card, float y) {
    return {card.x + card.w * 0.125f - 17.0f, card.y + y - 14.0f, 34.0f, 28.0f};
}

// The keyboard strip under the modulation panel, `top` down: along its top the collapse toggle with the strip's
// title, the macros, the note source and velocity, and under them the wheels on the left and six octaves of keys,
// the octave shift under their right end. Collapsed, only the top row shows, the mod wheel's handle joining it.
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
inline Rect bendWheel(float top) {
    return {34.0f, top + kKeys.y, 26.0f, 90.0f};
}
inline Rect modWheel(float top) {
    return {74.0f, top + kKeys.y, 26.0f, 90.0f};
}
// The mod wheel's handle, right of it, to drag onto what it moves, as a macro's; collapsed, in the top row between
// the macros and the note source, its name under it.
inline Rect wheelHandle(float top, bool collapsed) {
    return collapsed ? Rect{918.0f, top + kStripRow - 10.0f, 20.0f, 20.0f}
                     : Rect{104.0f, top + kKeys.y + 35.0f, 20.0f, 20.0f};
}
constexpr int kKeyCount = 73; // C to C, six octaves
constexpr int kWhiteKeys = 43;
constexpr float kWhiteW = kKeys.w / kWhiteKeys;
constexpr float kBlackW = kWhiteW * 0.6f;
constexpr float kBlackH = kKeys.h * 0.6f;
constexpr int kBaseNote = 24; // C1, where the keyboard starts before any octave shift
// Init, Load, Save and the settings cog: icons at the right end of the view tabs' row.
inline Rect toolbarButton(int i) {
    constexpr float kW = 28.0f, kGap = 6.0f;
    return {kInspector.right() - (4 - i) * (kW + kGap) + kGap, kTopRowY, kW, kTopRowH};
}
// Undo and redo, a little apart to their left.
inline Rect undoButton(int redo) {
    const Rect init = toolbarButton(0);
    return {init.x - 14.0f - (2 - redo) * 34.0f, init.y, 28.0f, init.h};
}
// The preset bar, in the middle of the view tabs' row: back, the preset's name, forward.
constexpr Rect kPresetName{430.0f, kTopRowY, 340.0f, kTopRowH};
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

// The settings dialog, centered over the editor.
constexpr float kSettingsW = 380.0f;
constexpr float kSettingsH = 364.0f;
constexpr int kOversamplingFactors[] = {1, 2, 4, 8};
inline Rect settingsPanel(float viewHeight) {
    return {(View::kWidth - kSettingsW) * 0.5f, std::round((viewHeight - kSettingsH) * 0.5f), kSettingsW, kSettingsH};
}
inline Rect settingsOption(Rect panel, int i) {
    return {panel.x + 24.0f + i * 64.0f, panel.y + 82.0f, 58.0f, 28.0f};
}
inline Rect settingsTheme(Rect panel, int i) {
    const float w = (panel.w - 48.0f + 6.0f) / kThemeCount;
    return {panel.x + 24.0f + i * w, panel.y + 214.0f, w - 6.0f, 30.0f};
}
// The glow slider's track, with room above and below to grab it; GLOW before it, its amount after.
inline Rect settingsGlow(Rect panel) {
    return {panel.x + 76.0f, panel.y + 256.0f, 200.0f, 26.0f};
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
inline Rect browserClear(Rect panel) {
    const Rect field = browserSearch(panel);
    return {field.right() - 22.0f, field.y + 3.0f, 18.0f, 18.0f};
}
// Where its text goes: after the magnifier, short of the x.
inline Rect browserTyped(Rect panel) {
    const Rect field = browserSearch(panel);
    return {field.x + 26.0f, field.y, field.w - 26.0f - 24.0f, field.h};
}

} // namespace fmvibe::gui::detail
