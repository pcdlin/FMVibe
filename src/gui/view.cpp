#include "view.h"

#include "../effects.h"
#include "../filter.h"
#include "../uistrings.h"
#include "../wavetable.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>
#include <string_view>

namespace fmvibe::gui {

namespace {

// --- Look.

struct Palette {
    const char *name; // as the settings keep it
    Str label;        // as the settings dialog shows it
    Color background, panel, raised, border, track, knobBody;
    Color text, dim, faint;
    Color accent;    // operators and FM routing
    Color filter;    // filters and what feeds them
    Color fx;        // effects
    Color sourceAdd; // the buttons that add LFOs and envelopes
    Color sources[kAllSources]; // one per source place, whichever type it holds, per macro, the note source's, the wheel's
    Color card;      // effect and source cards
    Color display;   // the wells graphs are drawn in
    Color headerTop, headerBottom;
    Color whiteKey, whiteKeyHover, blackKey, blackKeyHover, keyLabel;
    // Each type of effect in a color of its own, by FxType (None: the effects' color): shaper red, delay
    // blue, DJ EQ amber, parametric EQ orange, filter purple, compressor green, three-band teal, reverb indigo,
    // phaser pink, flanger lime, frequency shifter cyan, phase disperser slate, widener fuchsia.
    Color fxTypes[static_cast<int>(FxType::Count)];
};
static_assert(static_cast<int>(FxType::Count) == 14, "a color for each effect type in every theme");

// The themes the settings pick from, by name; the first is the default.
constexpr Palette kThemes[] = {
    {"Midnight", Str::ThemeMidnight,
     Color::hex(0x0D0F13), Color::hex(0x161920), Color::hex(0x1F232C), Color::hex(0x272C36), Color::hex(0x2D323D),
     Color::hex(0x252A35),
     Color::hex(0xE8EBF2), Color::hex(0x8D94A5), Color::hex(0x4A5162),
     Color::hex(0x8EA2FF), Color::hex(0xC084FC), Color::hex(0xFB923C), Color::hex(0x60A5FA),
     {Color::hex(0x38BDF8), Color::hex(0xF472B6), Color::hex(0xA3E635), Color::hex(0x818CF8),
      Color::hex(0xFBBF24), Color::hex(0x34D399), Color::hex(0xF87171), Color::hex(0xE879F9),
      Color::hex(0x2DD4BF), Color::hex(0xFDE047), Color::hex(0xA78BFA), Color::hex(0xFDA4AF),
      Color::hex(0x67E8F9), Color::hex(0x86EFAC), Color::hex(0x60A5FA), Color::hex(0xFDBA74),
      Color::hex(0xFDA4AF), Color::hex(0xFDBA74), Color::hex(0xFDE68A), Color::hex(0xBEF264),
      Color::hex(0x6EE7B7), Color::hex(0x67E8F9), Color::hex(0xA5B4FC), Color::hex(0xF0ABFC),
      Color::hex(0xE2E8F0), Color::hex(0xB8C4FF)},
     Color::hex(0x1A1D25), Color::hex(0x101217), Color::hex(0x1A1E26), Color::hex(0x12151B),
     Color::hex(0xE4E7EE), Color::hex(0xCDD3DF), Color::hex(0x07090D), Color::hex(0x262C38), Color::hex(0x4A5162),
     {Color::hex(0xFB923C), Color::hex(0xF87171), Color::hex(0x60A5FA), Color::hex(0xFBBF24), Color::hex(0xFB923C),
      Color::hex(0xC084FC), Color::hex(0x34D399), Color::hex(0x2DD4BF), Color::hex(0x818CF8), Color::hex(0xF472B6),
      Color::hex(0xA3E635), Color::hex(0x22D3EE), Color::hex(0xCBD5E1), Color::hex(0xE879F9)}},
    {"Graphite", Str::ThemeGraphite, // neutral grays, without the blue
     Color::hex(0x111111), Color::hex(0x1A1A1A), Color::hex(0x252525), Color::hex(0x2F2F2F), Color::hex(0x363636),
     Color::hex(0x2C2C2C),
     Color::hex(0xEDEDED), Color::hex(0x9C9C9C), Color::hex(0x575757),
     Color::hex(0x5EEAD4), Color::hex(0xF0ABFC), Color::hex(0xFACC15), Color::hex(0x7DD3FC),
     {Color::hex(0x7DD3FC), Color::hex(0xF9A8D4), Color::hex(0xBEF264), Color::hex(0xA5B4FC),
      Color::hex(0xFCD34D), Color::hex(0x6EE7B7), Color::hex(0xFCA5A5), Color::hex(0xF0ABFC),
      Color::hex(0x5EEAD4), Color::hex(0xFEF08A), Color::hex(0xC4B5FD), Color::hex(0xFECDD3),
      Color::hex(0xA5F3FC), Color::hex(0xBBF7D0), Color::hex(0x93C5FD), Color::hex(0xFED7AA),
      Color::hex(0xFECDD3), Color::hex(0xFED7AA), Color::hex(0xFEF3C7), Color::hex(0xD9F99D),
      Color::hex(0xA7F3D0), Color::hex(0xA5F3FC), Color::hex(0xC7D2FE), Color::hex(0xF5D0FE),
      Color::hex(0xE5E7EB), Color::hex(0x99F6E4)},
     Color::hex(0x1E1E1E), Color::hex(0x141414), Color::hex(0x202020), Color::hex(0x171717),
     Color::hex(0xE6E6E6), Color::hex(0xCFCFCF), Color::hex(0x0A0A0A), Color::hex(0x2C2C2C), Color::hex(0x575757),
     {Color::hex(0xFACC15), Color::hex(0xFCA5A5), Color::hex(0x93C5FD), Color::hex(0xFCD34D), Color::hex(0xFDBA74),
      Color::hex(0xD8B4FE), Color::hex(0x6EE7B7), Color::hex(0x5EEAD4), Color::hex(0xA5B4FC), Color::hex(0xF9A8D4),
      Color::hex(0xBEF264), Color::hex(0x67E8F9), Color::hex(0xE2E8F0), Color::hex(0xF0ABFC)}},
    {"Light", Str::ThemeLight, // crisp: white cards and knob caps on a cool gray, with deep, saturated colors
     Color::hex(0xD5DAE3), Color::hex(0xF3F5F8), Color::hex(0xE4E8EF), Color::hex(0xC3CAD6), Color::hex(0xCDD3DD),
     Color::hex(0xFFFFFF),
     Color::hex(0x0F131B), Color::hex(0x444C5D), Color::hex(0x878FA0),
     Color::hex(0x3346D3), Color::hex(0x8A2BE2), Color::hex(0xDD4F07), Color::hex(0x1F5BE0),
     {Color::hex(0x0274B8), Color::hex(0xD0216D), Color::hex(0x4F8A0B), Color::hex(0x4338CA),
      Color::hex(0xC26505), Color::hex(0x04805C), Color::hex(0xD11F1F), Color::hex(0xB01ECA),
      Color::hex(0x0B857A), Color::hex(0xA87103), Color::hex(0x6D28D9), Color::hex(0xD0163F),
      Color::hex(0x0780A0), Color::hex(0x138A3F), Color::hex(0x1F5BE0), Color::hex(0xDD4F07),
      Color::hex(0xD0163F), Color::hex(0xDD4F07), Color::hex(0xA87103), Color::hex(0x4F8A0B),
      Color::hex(0x04805C), Color::hex(0x0780A0), Color::hex(0x4338CA), Color::hex(0xB01ECA),
      Color::hex(0x3F4A5C), Color::hex(0x4F52E8)},
     Color::hex(0xFFFFFF), Color::hex(0xE8ECF2), Color::hex(0xFFFFFF), Color::hex(0xEDF0F5),
     Color::hex(0xFFFFFF), Color::hex(0xDDE3EC), Color::hex(0x1A1F29), Color::hex(0x3A4252), Color::hex(0x878FA0),
     {Color::hex(0xDD4F07), Color::hex(0xD11F1F), Color::hex(0x1F5BE0), Color::hex(0xC26505), Color::hex(0xDD4F07),
      Color::hex(0x8A2BE2), Color::hex(0x04805C), Color::hex(0x0B857A), Color::hex(0x4338CA), Color::hex(0xD0216D),
      Color::hex(0x4F8A0B), Color::hex(0x0780A0), Color::hex(0x475569), Color::hex(0xB01ECA)}},
    {"Paper", Str::ThemePaper, // warm: cream cards on sand, ink-brown text and earthy colors
     Color::hex(0xE2DACB), Color::hex(0xF7F2E8), Color::hex(0xEDE6D8), Color::hex(0xD2C7B3), Color::hex(0xDDD3C0),
     Color::hex(0xFFFCF5),
     Color::hex(0x2A231C), Color::hex(0x695D4E), Color::hex(0xA19582),
     Color::hex(0x0E6E68), Color::hex(0x8E2A9E), Color::hex(0xC0410F), Color::hex(0x0E6E68),
     {Color::hex(0x0E6E68), Color::hex(0xB3284F), Color::hex(0x5A7A12), Color::hex(0x4A3FB8),
      Color::hex(0xB15A0A), Color::hex(0x177A52), Color::hex(0xB8361B), Color::hex(0x9B2AA0),
      Color::hex(0x1A7287), Color::hex(0x9A7410), Color::hex(0x6A3DB8), Color::hex(0xB82848),
      Color::hex(0x235F7A), Color::hex(0x3E7D2C), Color::hex(0x2D55B0), Color::hex(0x9C4A1A),
      Color::hex(0xB82848), Color::hex(0xC0410F), Color::hex(0x9A7410), Color::hex(0x5A7A12),
      Color::hex(0x177A52), Color::hex(0x1A7287), Color::hex(0x4A3FB8), Color::hex(0x9B2AA0),
      Color::hex(0x5E554A), Color::hex(0x6A3DB8)},
     Color::hex(0xFFFCF6), Color::hex(0xEFE8DA), Color::hex(0xFCF8F0), Color::hex(0xF1EBDF),
     Color::hex(0xFFFDF8), Color::hex(0xEDE4D3), Color::hex(0x2E2722), Color::hex(0x4C433B), Color::hex(0xA19582),
     {Color::hex(0xC0410F), Color::hex(0xB8361B), Color::hex(0x2D55B0), Color::hex(0xB15A0A), Color::hex(0xC0410F),
      Color::hex(0x8E2A9E), Color::hex(0x177A52), Color::hex(0x0E6E68), Color::hex(0x4A3FB8), Color::hex(0xB3284F),
      Color::hex(0x5A7A12), Color::hex(0x1A7287), Color::hex(0x5E554A), Color::hex(0x9B2AA0)}},
};
constexpr int kThemeCount = static_cast<int>(std::size(kThemes));

int themeIndex(const std::string &name) {
    for (int i = 0; i < kThemeCount; ++i)
        if (name == kThemes[i].name)
            return i;
    return 0;
}

// The theme being painted: View::paint() sets it from the controller first, so the names below always
// stand for the colors of the editor painting.
Palette look = kThemes[0];
const Color &kBackground = look.background;
const Color &kPanel = look.panel;
const Color &kRaised = look.raised;
const Color &kBorder = look.border;
const Color &kTrack = look.track;
const Color &kKnobBody = look.knobBody;
const Color &kText = look.text;
const Color &kDim = look.dim;
const Color &kFaint = look.faint;
const Color &kAccent = look.accent;
const Color &kFilterColor = look.filter;
const Color &kShaperColor = look.fxTypes[static_cast<int>(FxType::Shaper)]; // the voices' shapers, as the effect
const Color &kFxColor = look.fx;
const Color &kSourceAddColor = look.sourceAdd;
const Color (&kSourceColors)[kAllSources] = look.sources;
const Color &kCardColor = look.card;
const Color &kDisplayColor = look.display;
Color fxTypeColor(FxType type) {
    return look.fxTypes[std::clamp(static_cast<int>(type), 0, static_cast<int>(FxType::Count) - 1)];
}
float glowing = 0.0f; // how strongly graphs' curves glow, 0..1, which View::paint() sets from the controller too

// A graph's curve. Glowing, it is drawn thinner, over a soft halo of its own color that fades outward; the
// halo grows stronger and wider with the glow (at half, three strokes 6, 3.5 and 2 times the curve's width).
void plotLine(Canvas &c, const Point *points, size_t count, Color color, float width) {
    if (glowing > 0.0f) {
        const float strength = 2.0f * glowing, radius = 0.5f + glowing;
        c.polyline(points, count, color.alpha(0.05f * strength), width * (1.0f + 5.0f * radius));
        c.polyline(points, count, color.alpha(0.08f * strength), width * (1.0f + 2.5f * radius));
        c.polyline(points, count, color.alpha(0.16f * strength), width * (1.0f + radius));
        width *= 1.0f - 0.3f * std::min(strength, 1.0f);
    }
    c.polyline(points, count, color, width);
}
const char *const kNodeNames[kNodes] = {"A", "B", "C", "D", "E", "F", "F1", "F2", "S1", "S2"};
// The effect cards' titles, by FxType from the shaper on.
constexpr Str kFxTitles[] = {Str::FxTitleShaper, Str::FxTitleDelay, Str::FxTitleDjEq, Str::FxTitleParamEq,
                             Str::FxTitleFilter, Str::FxTitleCompressor, Str::FxTitleMultiComp, Str::FxTitleReverb,
                             Str::FxTitlePhaser, Str::FxTitleFlanger, Str::FxTitleFreqShifter, Str::FxTitleDisperser,
                             Str::FxTitleWidener};
static_assert(std::size(kFxTitles) == static_cast<size_t>(FxType::Count) - 1);
const char *fxTitle(FxType type) {
    return type == FxType::None ? "" : tr(kFxTitles[static_cast<int>(type) - 1]);
}
constexpr Str kMcBandNames[kMcBands] = {Str::BandButtonLow, Str::BandButtonMid, Str::BandButtonHigh};
constexpr Str kViewNames[] = {Str::ViewRouting, Str::ViewMatrix, Str::ViewFx};

// `value` as `format` prints it, for the numbers a wording takes.
std::string number(const char *format, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

// --- Layout, in logical pixels.

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kKnobStart = -0.75f * kPi; // knobs sweep 270 degrees
constexpr float kKnobSweep = 1.5f * kPi;

// The routing panel shows either the graph or the matrix; both edit the same params.
// Tabs above the panels pick what they show: the routing graph or matrix next
// to the inspector, or the effects across both.
constexpr float kPanelsTop = 104.0f;
constexpr Rect kRouting{16.0f, kPanelsTop, 852.0f, 372.0f};
constexpr Rect kInspector{884.0f, kPanelsTop, 300.0f, 372.0f};
constexpr Rect kGraph{kRouting.x + 40.0f, kRouting.y + 70.0f, kRouting.w - 80.0f, kRouting.h - 146.0f}; // node centers
constexpr float kBusY = kRouting.y + kRouting.h - 30.0f;
constexpr float kNodeRadius = 24.0f;
constexpr float kFilterHalfW = 33.0f;
constexpr float kFilterHalfH = 17.0f;
constexpr int kGridCols = kNodes + 1; // operators, filters, shapers, output
// The matrix lists every operator, filter and shaper, and fills the panel's height: the column names above the
// first row, the last row 14 px above the panel's bottom.
constexpr float kCell = 27.0f;
constexpr float kCellStep = 31.0f;
constexpr Point kGridOrigin{kRouting.x + 56.0f, kRouting.y + kRouting.h - 14.0f - (kNodes - 1) * kCellStep - kCell};
constexpr float kLegendX = kGridOrigin.x + kGridCols * kCellStep + 40.0f;
constexpr Rect kOpEnvRect{kInspector.x + 16.0f, kInspector.y + 214.0f, kInspector.w - 32.0f, 78.0f};
constexpr Rect kUnisonRect{kInspector.x + 226.0f, kInspector.y + 150.0f, 58.0f, 40.0f};
// Where a noise operator's unison row would be: its spectrum.
constexpr Rect kNoiseRect{kInspector.x + 16.0f, kInspector.y + 148.0f, kInspector.w - 32.0f, 48.0f};
constexpr Rect kFilterDisplay{kInspector.x + 16.0f, kInspector.y + 150.0f, kInspector.w - 32.0f, 112.0f};
// A filter's slope list, at the right end of its row of type buttons.
constexpr Rect kFilterSlopeButton{kInspector.x + kInspector.w - 16.0f - 62.0f, kInspector.y + 62.0f, 62.0f, 22.0f};
// A shaper's page: its curve's list and the link switch in the row under the summary, then its transfer curve where
// a filter's response shows.
constexpr Rect kShaperModeButton{kInspector.x + 16.0f, kInspector.y + 62.0f, kInspector.w - 32.0f - 50.0f, 22.0f};
constexpr Rect kShaperLinkButton{kInspector.x + kInspector.w - 16.0f - 44.0f, kInspector.y + 62.0f, 44.0f, 22.0f};
constexpr Rect kShaperDisplay = kFilterDisplay;
// An operator's wave buttons: the common waves, then "..." with a list of the others.
constexpr OpWave kCommonWaves[] = {OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Noise};
constexpr OpWave kMoreWaves[] = {OpWave::Pulse,   OpWave::HalfSine, OpWave::AbsSine, OpWave::AltSine,
                                 OpWave::Organ,   OpWave::Vowel,    OpWave::Custom};
static_assert(std::size(kCommonWaves) + std::size(kMoreWaves) == static_cast<size_t>(OpWave::Count));
constexpr int kWaveButtons = static_cast<int>(std::size(kCommonWaves)) + 1;
Rect waveButton(int button) {
    constexpr float kStep = (kInspector.w - 32.0f + 4.0f) / kWaveButtons;
    return {kInspector.x + 16.0f + button * kStep, kInspector.y + 62.0f, kStep - 4.0f, 22.0f};
}
// The list "..." opens, right-aligned under it.
Rect waveMenu() {
    const Rect more = waveButton(kWaveButtons - 1);
    return {more.right() - 132.0f, more.bottom() + 4.0f, 132.0f, 8.0f + std::size(kMoreWaves) * 24.0f};
}
Rect waveMenuItem(int item) {
    const Rect menu = waveMenu();
    return {menu.x + 4.0f, menu.y + 4.0f + item * 24.0f, menu.w - 8.0f, 24.0f};
}
bool isMoreWave(OpWave wave) {
    return std::find(std::begin(kMoreWaves), std::end(kMoreWaves), wave) != std::end(kMoreWaves);
}
// Tabs of the routing panel, one per RoutingView.
constexpr Rect kViewTabs[] = {{16.0f, 72.0f, 84.0f, 24.0f}, {104.0f, 72.0f, 74.0f, 24.0f}, {182.0f, 72.0f, 48.0f, 24.0f}};
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

// The output meter's level on `side` (1 is full scale) and its clip light, as of now: the snapshot's, fallen on for
// as long as the snapshot has aged, as the audio thread would have let them fall had it gone on.
float meterPeak(const Telemetry &t, int side) {
    return t.outPeak[side] * static_cast<float>(std::pow(10.0, -kMeterFallDb * t.age / 20.0));
}
bool meterClipped(const Telemetry &t) {
    return t.clipHold > t.age;
}

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

Rect modPanel(bool collapsed) {
    return {kModPanel.x, kModPanel.y, kModPanel.w, collapsed ? kModBarH : kModPanel.h};
}

Rect sourcesToggle() {
    return {kModPanel.x + 8.0f, kModPanel.y + 8.0f, 100.0f, 26.0f};
}

// A card by its screen place: 0..3, fractions while it scrolls.
// `span`: the card places it takes, less than one folded.
Rect sourceCard(float card, float span = 1.0f) {
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
Rect sourceCardClip() {
    return {kModPanel.x + 6.0f, kCardTop - 4.0f, kModPanel.w - 12.0f, kCardH + 8.0f};
}

// ADD LFO and ADD ENV, after the toggle; shaped like ADD EFFECT.
Rect sourceAddButton(SourceType type) {
    return {kAddSourceX + (type == SourceType::Env ? 88.0f : 0.0f), kModPanel.y + 9.0f, 82.0f, 22.0f};
}

// A random LFO's display makes room at its right for the Smooth and Chaos knobs.
constexpr float kRandomKnobsW = 100.0f;
Rect sourceDisplay(float card, bool narrow = false) {
    const Rect p = sourceCard(card);
    return {p.x + 16.0f, p.y + 98.0f, p.w - 32.0f - (narrow ? kRandomKnobsW : 0.0f), 52.0f};
}

// 0 is Smooth, 1 Chaos.
Point randomKnob(float card, int knob) {
    const Rect display = sourceDisplay(card, true);
    return {display.right() + 25.0f + knob * 50.0f, display.y + 15.0f};
}

// The used slots, kVisibleSlots rows at a time; `row` counts from the top of the list, fractions while it
// scrolls and the rows at its ends are cut off.
constexpr int kVisibleSlots = 4;
constexpr float kSlotNameW = 96.0f; // the target's name, between the bypass dot and the depth bar
Rect slotRow(float card, float row) {
    const Rect p = sourceCard(card);
    return {p.x + 12.0f, p.y + 160.0f + row * 24.0f, p.w - 24.0f, 20.0f};
}

// Where the rows go, and the scrollbar beside them when there are more rows than fit.
Rect slotList(float card) {
    const Rect first = slotRow(card, 0);
    return {first.x, first.y, first.w + 10.0f, slotRow(card, kVisibleSlots - 1).bottom() - first.y};
}

Rect slotScrollTrack(float card) {
    const Rect list = slotList(card);
    return {list.right() - 6.0f, list.y + 2.0f, 3.0f, list.h - 4.0f};
}

// Where the rows of the list show: within the list and within the row of cards.
Rect slotClip(float card, Rect cards) {
    const Rect list = slotList(card);
    const float x = std::max(list.x - 2.0f, cards.x), y = std::max(list.y, cards.y);
    const float right = std::min(list.right(), cards.right()), bottom = std::min(list.bottom(), cards.bottom());
    return {x, y, std::max(right - x, 0.0f), std::max(bottom - y, 0.0f)};
}

// Title row: grip, handle and name, then (LFOs) SYNC, the power switch and the remove cross at the right.
Rect sourceRemove(float card) {
    const Rect p = sourceCard(card);
    return {p.right() - 26.0f, p.y + 7.0f, 18.0f, 21.0f};
}

Rect sourcePower(float card) {
    const Rect p = sourceCard(card);
    return {p.right() - 30.0f - 26.0f, p.y + 8.0f, 26.0f, 19.0f};
}

// An LFO's sync switch, under its Rate knob (and the knob's label), in the row of its other switches.
Rect sourceSync(float card) {
    const Rect p = sourceCard(card);
    return {p.right() - 44.0f - 22.0f, p.y + 79.0f, 44.0f, 16.0f};
}

bool isSourcePower(int param) {
    return sourceFieldOf(param) == SourceParam::Enabled;
}

// ADD EFFECT at the top left, and the list it opens under it: one row per type,
// then how many places are left.
Rect fxMenuButton() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + 9.0f, 116.0f, 22.0f};
}

constexpr int kFxTypes = static_cast<int>(FxType::Count) - 1; // without None
// The list's top left is under the button, or (from the shadow card after the effects) given.
Rect fxMenu(Point at) {
    const Rect button = fxMenuButton();
    return {at.x < 0.0f ? button.x : at.x, at.x < 0.0f ? button.bottom() + 4.0f : at.y, 180.0f, 8.0f + kFxTypes * 26.0f};
}

Rect fxMenuItem(Point at, FxType type) {
    const Rect menu = fxMenu(at);
    return {menu.x + 4.0f, menu.y + 4.0f + (static_cast<int>(type) - 1) * 26.0f, menu.w - 8.0f, 24.0f};
}

// A shaper card's curve picker: a button with the curve's name across the card, and the list it opens under it;
// its LINK sits over the Output knob, which it ties to the drive. The reverb's row: its algorithm's button.
Rect clipModeButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}
Rect clipLinkButton(Rect card) {
    return {card.center().x - 22.0f, card.y + 194.0f, 44.0f, 20.0f};
}
Rect reverbTypeButton(Rect card) {
    return {card.x + 14.0f, card.y + 44.0f, card.w - 28.0f, 22.0f};
}

// The filter effect's slope list, under its display in the middle.
Rect fxSlopeButton(Rect card, Rect display) {
    return {card.center().x - 36.0f, display.bottom() + 4.0f, 72.0f, 18.0f};
}

// The type param that goes with a filter slope param, or -1 for other params.
int slopeTypeParam(int param) {
    if (fxFieldOf(param) == FxParam::FilterSlope)
        return paramIndex(fxSlotOf(param), FxParam::FilterType);
    for (int filter = 0; filter < kFilters; ++filter)
        if (paramIndex(filter, FilterParam::Slope) == param)
            return paramIndex(filter, FilterParam::Type);
    return -1;
}

bool isFilterSlope(int param) {
    return slopeTypeParam(param) >= 0;
}

// A choice that opens a list under its button (a shaper's curve, the reverb's algorithm, a filter's slope): which
// values it has, and their names.
int listCount(int param) {
    return isFilterSlope(param)                       ? kFilterSlopes
           : fxFieldOf(param) == FxParam::ReverbType ? static_cast<int>(ReverbType::Count)
                                                      : static_cast<int>(ClipMode::Count);
}
std::string listItemName(int param, int value) {
    if (isFilterSlope(param))
        return std::to_string(slopeDb(value)) + " dB";
    return fxFieldOf(param) == FxParam::ReverbType ? reverbTypeName(static_cast<ReverbType>(value))
                                                    : clipModeName(static_cast<ClipMode>(value));
}

Rect listMenu(Rect button, int count) {
    return {button.x, button.bottom() + 4.0f, button.w, 8.0f + count * 22.0f};
}

Rect listMenuItem(Rect button, int count, int item) {
    const Rect menu = listMenu(button, count);
    return {menu.x + 4.0f, menu.y + 4.0f + item * 22.0f, menu.w - 8.0f, 22.0f};
}

// The scrollbars under the card rows: thin tracks along the panels' bottom edges.
Rect fxScrollTrack() {
    return {kFxPanel.x + 16.0f, kFxPanel.y + kFxPanel.h - 14.0f, kFxPanel.w - 32.0f, 6.0f};
}

Rect sourceScrollTrack() {
    return {kModPanel.x + 12.0f, kModPanel.y + kModPanel.h - 12.0f, kModPanel.w - 24.0f, 6.0f};
}

// The effect cards are cut off at the row's ends while they scroll.
Rect fxCardClip() {
    return {kFxPanel.x + 8.0f, kFxTop - 4.0f, kFxPanel.w - 16.0f, kFxPanel.bottom() - 20.0f - kFxTop + 8.0f};
}

// How many of the usual card places a card takes: the parametric EQ's is wider, for its spectrum and knobs.
float fxSpan(FxType type) {
    return type == FxType::ParamEq ? 1.5f : 1.0f;
}

// A card's rect by its screen place, in card places from the row's left end (0..kVisibleFx, fractions while
// it scrolls), and how many places it takes.
Rect fxCard(float position, float span = 1.0f) {
    return {kFxPanel.x + 16.0f + position * (kFxCardW + kFxGap), kFxTop, span * (kFxCardW + kFxGap) - kFxGap,
            kFxPanel.bottom() - 20.0f - kFxTop};
}

// The effect's icon, at the left of its card's title row, folded or not.
Rect fxIconRect(Rect card) {
    return {card.x + kFxIconX, card.y + 10.0f, kFxIconW, 14.0f};
}

// Displays sit under a card's buttons, if it has any, and above its knobs: one
// row of them, or two for the delay, the parametric EQ and the effects with six knobs or so.
Rect fxDisplay(float position, FxType type) {
    const Rect card = fxCard(position, fxSpan(type));
    const bool buttons = type == FxType::Shaper || type == FxType::Delay || type == FxType::MultiComp ||
                         type == FxType::Reverb;
    const bool buttonRow = buttons || type == FxType::Filter;
    const float top = buttonRow ? 74.0f : 44.0f;
    const bool twoRows = type == FxType::Delay || type == FxType::ParamEq || type == FxType::Compressor ||
                         type == FxType::Reverb || type == FxType::Phaser || type == FxType::Flanger ||
                         type == FxType::Filter || type == FxType::FreqShifter;
    // The filter's slope switch and the phaser's and flanger's SYNC and STEREO go under their displays.
    const bool switchesUnder = type == FxType::Phaser || type == FxType::Flanger || type == FxType::Filter;
    // The shaper's LINK goes between its display and its knobs.
    const float bottom = switchesUnder               ? 158.0f
                         : twoRows                   ? 180.0f
                         : type == FxType::MultiComp ? 140.0f
                         : type == FxType::Shaper    ? 190.0f
                                                     : 198.0f;
    return {card.x + 14.0f, card.y + top, card.w - 28.0f, bottom - top};
}

// The power symbol: a ring open at the top, with a stroke down into the gap.
void powerIcon(Canvas &c, Point center, Color color) {
    constexpr float kRadius = 5.0f, kGap = 0.7f; // radians either side of 12 o'clock
    c.arc(center, kRadius, kGap, 2.0f * std::numbers::pi_v<float> - kGap, color, 1.6f);
    c.line({center.x, center.y - kRadius - 1.5f}, {center.x, center.y - 0.5f}, color, 1.6f);
}

// A filter type's response in a few strokes, for the type buttons: frequency across, level up.
void filterIcon(Canvas &c, Rect r, FilterType type, Color color) {
    std::vector<Point> points;
    auto peak = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
    for (int i = 0; i <= 24; ++i) {
        const float x = i / 24.0f;
        float y = 0.0f;
        switch (type) {
        case FilterType::LowPass: // flat, a little resonance at the corner, then down
            y = x < 0.55f ? 0.35f + 0.35f * peak(x, 0.55f, 80.0f) : 0.7f - 3.4f * (x - 0.55f);
            break;
        case FilterType::HighPass:
            y = x > 0.45f ? 0.35f + 0.35f * peak(x, 0.45f, 80.0f) : 0.7f - 3.4f * (0.45f - x);
            break;
        case FilterType::BandPass:
            y = -0.9f + 1.7f * peak(x, 0.5f, 30.0f);
            break;
        case FilterType::Notch:
            y = 0.5f - 1.4f * peak(x, 0.5f, 120.0f);
            break;
        case FilterType::Comb: // evenly spaced teeth
            y = -0.8f + 1.5f * std::pow(std::abs(std::cos(3.0f * kPi * x)), 6.0f);
            break;
        case FilterType::Count:
            break;
        }
        points.push_back({r.x + x * r.w, r.center().y - std::clamp(y, -1.0f, 1.0f) * r.h * 0.5f});
    }
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// An effect's tempo sync switch: the delay's, the phaser's or the flanger's.
bool isFxSync(FxParam field) {
    return field == FxParam::DelaySync || field == FxParam::PhaserSync || field == FxParam::FlangerSync;
}

// Effect switches drawn as icons, named in their tooltips: a metronome for any effect's SYNC, two chain links for
// the delay's, the shaper's and the three-band compressor's LINK, and a ball bouncing between two paddles for the
// delay's PING-PONG.
bool isFxIconSwitch(FxParam field) {
    return isFxSync(field) || field == FxParam::DelayLink || field == FxParam::ShaperLink ||
           field == FxParam::McLink || field == FxParam::DelayPingPong;
}
void fxSwitchIcon(Canvas &c, Point m, FxParam field, Color color) {
    if (isFxSync(field)) {
        const Point body[] = {{m.x - 5.0f, m.y + 5.5f}, {m.x - 2.0f, m.y - 5.5f}, {m.x + 2.0f, m.y - 5.5f},
                              {m.x + 5.0f, m.y + 5.5f}, {m.x - 5.0f, m.y + 5.5f}};
        c.polyline(body, std::size(body), color, 1.3f);
        c.line({m.x, m.y + 3.5f}, {m.x + 5.5f, m.y - 6.0f}, color, 1.3f);
        c.fillCircle({m.x + 2.8f, m.y - 1.4f}, 1.6f, color);
    } else if (field == FxParam::DelayLink || field == FxParam::ShaperLink || field == FxParam::McLink) {
        c.strokeRect({m.x - 9.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
        c.strokeRect({m.x - 2.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
    } else {
        c.line({m.x - 8.5f, m.y - 5.5f}, {m.x - 8.5f, m.y + 5.5f}, color, 1.6f);
        c.line({m.x + 8.5f, m.y - 5.5f}, {m.x + 8.5f, m.y + 5.5f}, color, 1.6f);
        const Point path[] = {{m.x - 6.0f, m.y - 4.0f}, {m.x + 6.0f, m.y}, {m.x - 6.0f, m.y + 4.0f}};
        c.polyline(path, std::size(path), color.alpha(0.6f), 1.0f);
        c.fillCircle({m.x - 4.5f, m.y + 3.0f}, 1.8f, color);
    }
}

// A three-band compressor's band, for its button: its slice of the spectrum (frequency across) raised over a faint
// baseline, the low band falling away to the right, the mid one a plateau, the high one rising from the left.
void mcBandIcon(Canvas &c, Rect r, int band, Color color) {
    c.line({r.x, r.bottom()}, {r.right(), r.bottom()}, color.alpha(0.35f), 1.0f);
    auto rise = [](float x, float at) { // 0 well below `at`, 1 well above, smoothly across a crossover
        const float t = std::clamp((x - at) / 0.22f + 0.5f, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    std::vector<Point> points;
    for (int i = 0; i <= 24; ++i) {
        const float x = i / 24.0f;
        const float level = band == 0 ? 1.0f - rise(x, 0.45f)
                            : band == 1 ? rise(x, 0.25f) * (1.0f - rise(x, 0.75f))
                                        : rise(x, 0.55f);
        points.push_back({r.x + x * r.w, r.bottom() - level * r.h});
    }
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// An LFO's mode switches, named in their tooltips: FREE an infinity sign, ONE-SHOT a single cycle that then stays
// where it ends, UNIPOLAR a wave that only rises from its baseline.
void lfoModeIcon(Canvas &c, Point m, SourceParam mode, Color color) {
    std::vector<Point> points;
    if (mode == SourceParam::Free) {
        for (int i = 0; i <= 48; ++i) {
            const float t = i / 48.0f * 2.0f * kPi, d = 1.0f + std::sin(t) * std::sin(t);
            points.push_back({m.x + 10.0f * std::cos(t) / d, m.y + 11.0f * std::sin(t) * std::cos(t) / d});
        }
    } else if (mode == SourceParam::OneShot) {
        for (int i = 0; i <= 24; ++i)
            points.push_back({m.x - 10.0f + i * 0.5f, m.y - 4.5f * std::sin(i / 24.0f * 2.0f * kPi)});
        points.push_back({m.x + 10.0f, m.y});
        c.fillCircle(points.back(), 1.7f, color);
    } else {
        c.line({m.x - 11.0f, m.y + 5.0f}, {m.x + 11.0f, m.y + 5.0f}, color.alpha(0.5f), 1.0f);
        for (int i = 0; i <= 32; ++i)
            points.push_back({m.x - 10.0f + i * 0.625f, m.y + 5.0f - 4.5f * (1.0f - std::cos(i / 32.0f * 4.0f * kPi))});
    }
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// A small picture of each effect, for its card's title and the ADD EFFECT list.
void fxIcon(Canvas &c, Rect r, FxType type, Color color) {
    std::vector<Point> points;
    auto at = [&](float x, float y) { points.push_back({r.x + x * r.w, r.center().y - y * r.h * 0.5f}); };
    auto stroke = [&](Color tint) {
        c.polyline(points.data(), points.size(), tint, 1.4f);
        points.clear();
    };
    auto curve = [&](auto &&y) { // y(x) for x across, -1..1 up
        for (int i = 0; i <= 24; ++i)
            at(i / 24.0f, y(i / 24.0f));
        stroke(color);
    };
    auto bar = [&](float x, float height, Color tint) {
        c.line({r.x + x * r.w, r.bottom()}, {r.x + x * r.w, r.bottom() - height * r.h}, tint, 1.6f);
    };
    auto notch = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
    switch (type) {
    case FxType::Shaper: // a sine with its top and bottom cut flat
        curve([](float x) { return std::clamp(1.7f * std::sin(2.0f * kPi * x), -1.0f, 1.0f) * 0.8f; });
        break;
    case FxType::Delay: // the sound, then echoes, each softer
        bar(0.1f, 1.0f, color);
        bar(0.5f, 0.6f, color);
        bar(0.9f, 0.32f, color);
        break;
    case FxType::Eq: // three faders
        for (int k = 0; k < 3; ++k) {
            const float x = r.x + (0.17f + 0.33f * k) * r.w;
            const float y = r.y + (k == 1 ? 0.7f : (k == 0 ? 0.35f : 0.5f)) * r.h;
            c.line({x, r.y}, {x, r.bottom()}, color.alpha(0.45f), 1.0f);
            c.fillRect({x - 2.5f, y - 1.5f, 5.0f, 3.0f}, color, 1.0f);
        }
        break;
    case FxType::ParamEq: // a bell
        curve([&](float x) { return -0.6f + 1.4f * notch(x, 0.5f, 25.0f); });
        break;
    case FxType::Filter: // a low-pass, with a little resonance at its corner
        curve([&](float x) { return x < 0.5f ? 0.3f + 0.4f * notch(x, 0.5f, 60.0f) : 0.7f - 3.2f * (x - 0.5f); });
        break;
    case FxType::Compressor: // output against input, bending over at the threshold
    case FxType::MultiComp:  // the same, over three bands
        if (type == FxType::MultiComp)
            for (float x : {1.0f / 3.0f, 2.0f / 3.0f})
                c.line({r.x + x * r.w, r.y}, {r.x + x * r.w, r.bottom()}, color.alpha(0.4f), 1.0f);
        curve([](float x) { return x < 0.55f ? -1.0f + 2.4f * x : 0.32f + 0.5f * (x - 0.55f); });
        break;
    case FxType::Reverb: // a dense tail dying away
        for (int k = 0; k < 8; ++k)
            bar(0.06f + 0.125f * k, 1.0f - 0.115f * k, color.alpha(k == 0 ? 1.0f : 0.75f));
        break;
    case FxType::Phaser: // a few wide notches
        curve([&](float x) { return 0.6f - 1.4f * (notch(x, 0.28f, 90.0f) + notch(x, 0.72f, 90.0f)); });
        break;
    case FxType::Flanger: // a comb of narrow notches
        curve([&](float x) {
            float y = 0.6f;
            for (int k = 0; k < 4; ++k)
                y -= 1.5f * notch(x, 0.125f + 0.25f * k, 500.0f);
            return y;
        });
        break;
    case FxType::Disperser: { // a chirp: the sound smeared, its cycles drawn out
        std::vector<Point> chirp;
        for (int i = 0; i <= 40; ++i) {
            const float x = i / 40.0f;
            chirp.push_back({r.x + x * r.w, r.y + r.h * (0.5f - 0.42f * std::sin(2.0f * kPi * (1.0f + 3.0f * x) * x))});
        }
        c.polyline(chirp.data(), chirp.size(), color, 1.4f);
        break;
    }
    case FxType::FreqShifter: { // a partial, moved along
        bar(0.12f, 0.9f, color.alpha(0.4f));
        bar(0.88f, 0.9f, color);
        const float y = r.y + 0.35f * r.h;
        c.line({r.x + 0.28f * r.w, y}, {r.x + 0.7f * r.w, y}, color, 1.4f);
        const Point head[] = {{r.x + 0.76f * r.w, y}, {r.x + 0.64f * r.w, y - 3.0f}, {r.x + 0.64f * r.w, y + 3.0f}};
        c.fillPolygon(head, 3, color);
        break;
    }
    case FxType::Widener: { // the middle, and the sound spreading out of it to both sides
        const float cx = r.center().x;
        c.line({cx, r.y}, {cx, r.bottom()}, color.alpha(0.5f), 1.4f);
        for (float side : {-1.0f, 1.0f}) {
            const Point wing[] = {{cx + side * 0.2f * r.w, r.y + 0.1f * r.h},
                                  {cx + side * 0.48f * r.w, r.center().y},
                                  {cx + side * 0.2f * r.w, r.bottom() - 0.1f * r.h}};
            c.polyline(wing, 3, color, 1.4f);
        }
        break;
    }
    case FxType::None:
    case FxType::Count:
        break;
    }
}

// The parametric EQ's plot: frequency across, gain up and down.
constexpr float kPeqRangeDb = 20.0f;
Point peqAt(Rect area, double freq, double db) {
    return {area.x + static_cast<float>(freq) * area.w,
            area.center().y - static_cast<float>(db) / kPeqRangeDb * area.h * 0.5f};
}

// A band shape's response in a few strokes: frequency across, level up. Cuts show a little resonance
// at their corner, which tells them from the shelves.
void peqIcon(Canvas &c, Rect r, PeqType type, Color color) {
    std::vector<Point> points;
    const double q = normFromQ(isCut(type) ? 2.2 : 0.7);
    for (int i = 0; i <= 20; ++i) {
        const double x = i / 20.0;
        const double db = 20.0 * std::log10(peqResponse(cutoffFromNorm(0.15 + 0.7 * x), type, 0.5, 9.0, q));
        points.push_back({r.x + static_cast<float>(x) * r.w,
                          r.center().y - static_cast<float>(std::clamp(db, -15.0, 15.0) / 15.0) * r.h * 0.5f});
    }
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// The list of band shapes, which opens upward from its button over the card's display.
constexpr int kPeqTypes = static_cast<int>(PeqType::Count);
Rect peqMenu(Rect button) {
    constexpr float kH = 8.0f + kPeqTypes * 22.0f;
    return {button.x, button.y - 4.0f - kH, 124.0f, kH};
}

Rect peqMenuItem(Rect button, int type) {
    const Rect menu = peqMenu(button);
    return {menu.x + 4.0f, menu.y + 4.0f + type * 22.0f, menu.w - 8.0f, 22.0f};
}

// A band's type button, in the first place of its row of knobs.
Rect peqTypeButton(Rect card, float y) {
    return {card.x + card.w * 0.125f - 17.0f, card.y + y - 14.0f, 34.0f, 28.0f};
}



// The keyboard strip under the modulation panel, `top` down: along its top the note source, the macros and the
// octave shift, and under them the wheels on the left and six octaves of keys.
constexpr float kKeyboardH = 192.0f;
constexpr Rect kKeys{130.0f, 76.0f, 1046.0f, 98.0f}; // relative to the strip's top
constexpr float kStripRow = 29.0f;                   // the middle of the row above the keys, below the strip's top
float keyboardTop(bool collapsed) {
    return modPanel(collapsed).bottom() + 16.0f;
}
Rect keyboardPanel(float top) {
    return {16.0f, top, 1168.0f, kKeyboardH};
}
// The macros in a row above the keys, as far as the octave shift: a knob each, its name under it (double-click
// to rename), and left of it a handle to drag onto what it moves, as with a source's.
Point macroKnob(float keyboardTop, int macro) {
    const float cell = (1032.0f - kKeys.x) / kMacros;
    return {kKeys.x + (macro + 0.5f) * cell + 10.0f, keyboardTop + kStripRow};
}
// The note source, above the wheels: its handle, and beside it the note it follows, its name under that.
Rect noteHandle(float keyboardTop) {
    return {26.0f, keyboardTop + kStripRow - 10.0f, 20.0f, 20.0f};
}
Rect noteDisplay(float keyboardTop) {
    return {52.0f, keyboardTop + kStripRow - 12.0f, 68.0f, 24.0f};
}
Rect bendWheel(float top) {
    return {34.0f, top + kKeys.y, 26.0f, 90.0f};
}
Rect modWheel(float top) {
    return {74.0f, top + kKeys.y, 26.0f, 90.0f};
}
// The mod wheel's handle, right of it, to drag onto what it moves, as a macro's.
Rect wheelHandle(float top) {
    return {104.0f, top + kKeys.y + 35.0f, 20.0f, 20.0f};
}
constexpr int kKeyCount = 73; // C to C, six octaves
constexpr int kWhiteKeys = 43;
constexpr float kWhiteW = kKeys.w / kWhiteKeys;
constexpr float kBlackW = kWhiteW * 0.6f;
constexpr float kBlackH = kKeys.h * 0.6f;
constexpr int kBaseNote = 24; // C1, where the keyboard starts before any octave shift
float buttonY(float top) { // level with the macros' knobs
    return top + kStripRow - 13.0f;
}
// Init, Load, Save and the settings cog: icons at the right end of the view tabs' row.
Rect toolbarButton(int i) {
    constexpr float kW = 28.0f, kGap = 6.0f;
    return {kInspector.right() - (4 - i) * (kW + kGap) + kGap, kViewTabs[0].y, kW, kViewTabs[0].h};
}
// Undo and redo, a little apart to their left.
Rect undoButton(int redo) {
    const Rect init = toolbarButton(0);
    return {init.x - 14.0f - (2 - redo) * 34.0f, init.y, 28.0f, init.h};
}
// The preset bar, in the middle of the view tabs' row: back, the preset's name, forward.
constexpr Rect kPresetName{430.0f, 72.0f, 340.0f, 24.0f};
Rect presetStep(int direction) {
    return {direction < 0 ? kPresetName.x - 30.0f : kPresetName.right() + 4.0f, kPresetName.y, 26.0f, kPresetName.h};
}
Rect octaveButton(int dir, float top) {
    return {dir < 0 ? 1040.0f : 1148.0f, buttonY(top), 28.0f, 26.0f};
}

// Within an octave, the white keys (C D E F G A B) and the black keys (C# D# F# G# A#).
constexpr int kWhiteSlot[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
constexpr int kBlackAfter[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};
constexpr bool isBlackKey(int key) {
    return kBlackAfter[key % 12] >= 0;
}
const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int note) {
    return std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 1);
}


// Position in the grid: rows are nodes, columns nodes and then the output.
Rect gridCell(int row, int col) {
    return {kGridOrigin.x + col * kCellStep, kGridOrigin.y + row * kCellStep, kCell, kCell};
}

Rect knobRect(Point center, float diameter) {
    return {center.x - diameter * 0.5f, center.y - diameter * 0.5f, diameter, diameter + 18.0f};
}

Rect chip(Point center, float width = 38.0f) {
    return {center.x - width * 0.5f, center.y - 9.0f, width, 18.0f};
}

Point normalize(Point v) {
    const float length = std::hypot(v.x, v.y);
    return length > 1e-4f ? Point{v.x / length, v.y / length} : Point{0.0f, 1.0f};
}

float knobAngle(double norm) {
    return kKnobStart + static_cast<float>(std::clamp(norm, 0.0, 1.0)) * kKnobSweep;
}

std::string percent(double value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
    return text;
}

// --- Routing graph: operators are circles, filters and shapers boxes.

bool isBoxNode(int node) {
    return node >= kOps;
}

bool isFilterNode(int node) {
    return node >= kOps && node < kOps + kFilters;
}

bool isShaperNode(int node) {
    return node >= kOps + kFilters;
}

int shaperOfNode(int node) {
    return node - kOps - kFilters;
}

// Operators in the accent color, filters and shapers each in their own.
Color nodeColor(int node) {
    return isShaperNode(node) ? kShaperColor : isFilterNode(node) ? kFilterColor : kAccent;
}

// A route takes the color of the shaper or filter it joins, if any.
Color routeColor(int from, int to) {
    return isShaperNode(from) || isShaperNode(to)   ? kShaperColor
           : isFilterNode(from) || isFilterNode(to) ? kFilterColor
                                                    : kAccent;
}

// Distance from a node's center to its outline in direction `dir`.
float nodeExtent(int node, Point dir) {
    if (!isBoxNode(node))
        return kNodeRadius;
    const float tx = std::abs(dir.x) > 1e-4f ? kFilterHalfW / std::abs(dir.x) : 1e9f;
    const float ty = std::abs(dir.y) > 1e-4f ? kFilterHalfH / std::abs(dir.y) : 1e9f;
    return std::min(tx, ty);
}

// A node's edge, a band along its outline from kEdgeIn inside it to kEdgeOut outside: a drag from there connects the
// node to another, while one from further inside moves it.
constexpr float kEdgeIn = 6.0f, kEdgeOut = 5.0f;
bool onNodeEdge(int node, Point center, Point p) {
    if (isBoxNode(node)) {
        const float dx = std::abs(p.x - center.x), dy = std::abs(p.y - center.y);
        return dx <= kFilterHalfW + kEdgeOut && dy <= kFilterHalfH + kEdgeOut &&
               (dx >= kFilterHalfW - kEdgeIn || dy >= kFilterHalfH - kEdgeIn);
    }
    const float d = std::hypot(p.x - center.x, p.y - center.y);
    return d >= kNodeRadius - kEdgeIn && d <= kNodeRadius + kEdgeOut;
}

// Where a route leaving `node` toward `to` crosses its outline.
Point outlineToward(int node, Point center, Point to) {
    const Point dir = normalize({to.x - center.x, to.y - center.y});
    const float extent = nodeExtent(node, dir);
    return {center.x + dir.x * extent, center.y + dir.y * extent};
}

Point loopCenter(Point node) {
    return {node.x, node.y - kNodeRadius - 9.0f};
}

// The param that sets how much `from` feeds `to`, or -1 if they can't connect. Every node modulates operators and
// feeds filters and shapers, but a filter only feeds later filters, and a shaper no shaper.
int routeParam(int from, int to) {
    if (from < kOps)
        return to < kOps            ? matrixIndex(from, to)
               : isFilterNode(to) ? sendIndex(from, to - kOps)
                                  : shaperSendIndex(from, shaperOfNode(to));
    if (isShaperNode(from))
        return to < kOps            ? shaperModIndex(shaperOfNode(from), to)
               : isFilterNode(to) ? shaperFilterIndex(shaperOfNode(from), to - kOps)
                                  : -1;
    return to < kOps            ? filterModIndex(from - kOps, to)
           : isFilterNode(to) ? filterLinkIndex(from - kOps, to - kOps) // only into later filters
                              : filterShaperIndex(from - kOps, shaperOfNode(to));
}

// What a node puts straight out to the output.
int outParam(int node) {
    return isShaperNode(node)   ? paramIndex(shaperOfNode(node), ShaperParam::Out)
           : isFilterNode(node) ? paramIndex(node - kOps, FilterParam::Out)
                                : paramIndex(node, OpParam::Out);
}

// The route from `from` into `to`, kNodes standing for the output.
int routeTo(int from, int to) {
    return to == kNodes ? outParam(from) : routeParam(from, to);
}

// Why `from` can't go into a node: only a filter into itself or an earlier filter, or a shaper into a shaper, can't.
const char *routeRefusal(int from) {
    return tr(isShaperNode(from) ? Str::MsgRefuseShaper : Str::MsgRefuseFilter);
}

// Whether a port or arrowhead let go at `p` lands on the OUT bus.
bool onBus(Point p) {
    return std::abs(p.y - kBusY) < 16.0f && p.x > kRouting.x && p.x < kRouting.right();
}

// Whether `param` is a route of the graph: one node into another, or a node into the output.
bool isRouteParam(int param) {
    for (int from = 0; from < kNodes; ++from) {
        if (outParam(from) == param)
            return true;
        for (int to = 0; to < kNodes; ++to)
            if (routeParam(from, to) == param)
                return true;
    }
    return false;
}

int enabledParam(int node) {
    return isShaperNode(node)   ? paramIndex(shaperOfNode(node), ShaperParam::Enabled)
           : isFilterNode(node) ? paramIndex(node - kOps, FilterParam::Enabled)
                                : paramIndex(node, OpParam::Enabled);
}

bool isPowerParam(int param) {
    for (int node = 0; node < kNodes; ++node)
        if (enabledParam(node) == param)
            return true;
    return false;
}

bool isLfoSync(int param) {
    return sourceFieldOf(param) == SourceParam::Sync;
}

// "LP", or "LP 24" at a slope other than 12 dB per octave, as it plays.
std::string filterLabel(FilterType type, double slope) {
    const int played = playedSlope(type, filterSlope(slope));
    return std::string(filterTypeShortName(type)) +
           (hasSlope(type) && played != kDefaultSlope ? " " + std::to_string(slopeDb(played)) : "");
}

bool isLfoFree(int param) {
    return sourceFieldOf(param) == SourceParam::Free;
}

bool isPartialCount(int param) {
    return param >= partialCountIndex(0) && param <= partialCountIndex(kOps - 1);
}

std::string nodeTitle(int node) {
    return isShaperNode(node)   ? trf(Str::NodeShaper, {std::to_string(shaperOfNode(node) + 1)})
           : isFilterNode(node) ? trf(Str::NodeFilter, {std::to_string(node - kOps + 1)})
                                : trf(Str::NodeOperator, {kNodeNames[node]});
}

constexpr float kHintX = kRouting.x + 330.0f; // the routing graph's hint, in its title row

// Matrix cell: rows are nodes, columns the operators, the filters, then the output.
int gridParam(int row, int col) {
    if (col == kGridCols - 1)
        return outParam(row);
    return row == col ? (row < kOps ? matrixIndex(row, row) : -1) : routeParam(row, col);
}

// A wire bows to one side, so A -> B and B -> A don't overlap.
struct Wire {
    Point start;
    Point control;
    Point end; // base of the arrowhead
    Point label;
    Point tangent;
};

Wire wireBetween(int fromNode, Point a, int toNode, Point b) {
    const Point d{b.x - a.x, b.y - a.y};
    const Point u = normalize(d);
    const float bend = std::hypot(d.x, d.y) * 0.18f;
    Wire w;
    w.control = {(a.x + b.x) * 0.5f - u.y * bend, (a.y + b.y) * 0.5f + u.x * bend};
    const Point fromA = normalize({w.control.x - a.x, w.control.y - a.y});
    const Point fromB = normalize({w.control.x - b.x, w.control.y - b.y});
    const float startOffset = nodeExtent(fromNode, fromA);
    const float endOffset = nodeExtent(toNode, fromB) + 8.0f;
    w.start = {a.x + fromA.x * startOffset, a.y + fromA.y * startOffset};
    w.end = {b.x + fromB.x * endOffset, b.y + fromB.y * endOffset};
    w.label = {0.25f * w.start.x + 0.5f * w.control.x + 0.25f * w.end.x,
               0.25f * w.start.y + 0.5f * w.control.y + 0.25f * w.end.y};
    w.tangent = normalize({w.end.x - w.control.x, w.end.y - w.control.y});
    return w;
}

void arrowHead(Canvas &c, Point base, Point dir, Color color) {
    const Point side{-dir.y * 4.5f, dir.x * 4.5f};
    const Point points[] = {{base.x + dir.x * 8.0f, base.y + dir.y * 8.0f},
                            {base.x + side.x, base.y + side.y},
                            {base.x - side.x, base.y - side.y}};
    c.fillPolygon(points, 3, color);
}

// What can be grabbed of an arrowhead with its base at `base`, pointing along `dir`.
Rect arrowGrip(Point base, Point dir) {
    const Point middle{base.x + dir.x * 4.0f, base.y + dir.y * 4.0f};
    return {middle.x - 9.0f, middle.y - 9.0f, 18.0f, 18.0f};
}

// A node's line down to the OUT bus: from its outline to the base of the arrowhead, whose tip touches the bus, with
// its label halfway along. A node too close to the bus for the label to fit in between has it beside the line, on
// the left.
struct OutWire {
    Point start;
    Point end; // base of the arrowhead
    Point label;
};

OutWire outWireOf(int node, Point center) {
    const float top = center.y + (isBoxNode(node) ? kFilterHalfH : kNodeRadius);
    const float end = kBusY - 10.0f;
    OutWire w{{center.x, top}, {center.x, end}, {center.x, 0.5f * (top + end)}};
    if (end - top < 18.0f)
        w.label.x -= 28.0f;
    return w;
}

// --- Envelopes, for operators (0..kOps-1) and mod envelopes (kOps + source).

struct EnvIndices {
    int attack;
    int hold;
    int decay;
    int sustain;
    int release;
    int curves; // attack, decay and release curves follow each other from here

    int curve(int segment) const { return curves + segment; }
};

EnvIndices envIndices(int env) {
    if (env < kOps)
        return {paramIndex(env, OpParam::Attack), paramIndex(env, OpParam::Hold), paramIndex(env, OpParam::Decay),
                paramIndex(env, OpParam::Sustain), paramIndex(env, OpParam::Release),
                paramIndex(env, OpParam::AttackCurve)};
    const int s = env - kOps;
    return {paramIndex(s, SourceParam::Attack), paramIndex(s, SourceParam::Hold), paramIndex(s, SourceParam::Decay),
            paramIndex(s, SourceParam::Sustain), paramIndex(s, SourceParam::Release),
            paramIndex(s, SourceParam::AttackCurve)};
}
static_assert(paramIndex(0, OpParam::ReleaseCurve) == paramIndex(0, OpParam::AttackCurve) + 2);
static_assert(paramIndex(0, SourceParam::ReleaseCurve) == paramIndex(0, SourceParam::AttackCurve) + 2);

constexpr Str kEnvSegmentNames[] = {Str::LabelAttack, Str::LabelDecay, Str::LabelRelease};

// Attack, hold, decay and release each get kEnvMinSegment pixels, so short
// ones keep their handles apart, plus `scale` pixels per unit of their
// (log-scaled) time; the sustain hold has a fixed share. The scale is picked to
// fill the display (see envFitScale()), so short envelopes are zoomed in.
constexpr float kEnvMinSegment = 10.0f;
constexpr float kEnvSustainShare = 0.16f;

struct EnvShape {
    Point p0, p1, ph, p2, p3, p4; // start, peak, end of the hold, sustain level, end of the sustain hold, end
    float scale;
    float top;
    float bottom;
};

template <class Get>
EnvShape envShape(Rect r, const EnvIndices &e, const Get &get, float scale) {
    const float left = r.x + 10.0f;
    const float width = r.w - 20.0f;
    auto span = [&](int param) { return kEnvMinSegment + static_cast<float>(get(param)) * scale; };
    EnvShape s;
    s.scale = scale;
    s.top = r.y + 12.0f;
    s.bottom = r.bottom() - 10.0f;
    s.p0 = {left, s.bottom};
    s.p1 = {left + span(e.attack), s.top};
    s.ph = {s.p1.x + span(e.hold), s.top};
    s.p2 = {s.ph.x + span(e.decay), s.bottom - static_cast<float>(get(e.sustain)) * (s.bottom - s.top)};
    s.p3 = {s.p2.x + width * kEnvSustainShare, s.p2.y};
    s.p4 = {s.p3.x + span(e.release), s.bottom};
    return s;
}

// The scale at which an envelope fills a display `r` wide. Below a combined
// 0.15 of normalized time (all four under about 4 ms), it stops zooming.
template <class Get>
float envFitScale(Rect r, const EnvIndices &e, const Get &get) {
    const float room = (r.w - 20.0f) * (1.0f - kEnvSustainShare) - 4.0f * kEnvMinSegment;
    const double total = get(e.attack) + get(e.hold) + get(e.decay) + get(e.release);
    return room / static_cast<float>(std::max(total, 0.15));
}

// A point on segment 0 (attack, p0 to p1), 1 (decay, ph to p2) or 2 (release,
// p3 to p4), at time t (0..1) along it, bent by its curve as the DSP bends it.
Point envSegmentPoint(const EnvShape &s, int segment, double t, double curve, double sustain) {
    const double f = envCurve(t, curve);
    const double level = segment == 0 ? f : segment == 1 ? sustain + (1.0 - sustain) * (1.0 - f) : sustain * (1.0 - f);
    const Point from = segment == 0 ? s.p0 : segment == 1 ? s.ph : s.p3;
    const Point to = segment == 0 ? s.p1 : segment == 1 ? s.p2 : s.p4;
    return {from.x + static_cast<float>(t) * (to.x - from.x), s.bottom - static_cast<float>(level) * (s.bottom - s.top)};
}

// --- Filter response display: x is the cutoff knob's log scale, y is dB.

constexpr float kResponseMinDb = -36.0f;
constexpr float kResponseMaxDb = 18.0f;

Point filterHandle(Rect r, double cutoff, double resonance) {
    return {r.x + 10.0f + static_cast<float>(cutoff) * (r.w - 20.0f),
            r.bottom() - 10.0f - static_cast<float>(resonance) * (r.h - 20.0f)};
}

// --- Wave icons, one cycle across r.

std::vector<Point> lfoPoints(LfoShape shape, Rect r) {
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (shape) {
    case LfoShape::Sine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 48.0f));
        break;
    case LfoShape::Triangle:
        at(0.0f, 0.0f);
        at(0.25f, 1.0f);
        at(0.75f, -1.0f);
        at(1.0f, 0.0f);
        break;
    case LfoShape::Saw:
        at(0.0f, -1.0f);
        at(1.0f, 1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Square:
        at(0.0f, 1.0f);
        at(0.5f, 1.0f);
        at(0.5f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Random: {
        const float steps[] = {0.35f, -0.7f, 0.85f, -0.25f};
        for (int i = 0; i < 4; ++i) {
            at(i / 4.0f, steps[i]);
            at((i + 1) / 4.0f, steps[i]);
        }
        break;
    }
    case LfoShape::Custom: // straight lines between points you can drag; see customShapeDots()
        at(0.0f, -0.7f);
        at(0.35f, 0.8f);
        at(0.7f, -0.1f);
        at(1.0f, 0.6f);
        break;
    case LfoShape::Count:
        break;
    }
    return points;
}

// The Custom icon's line has a dot on each point, like the shape editor's handles.
void customShapeDots(Canvas &c, Rect r, Color color) {
    for (const Point &point : lfoPoints(LfoShape::Custom, r)) {
        c.fillCircle(point, 2.6f, kRaised);
        c.strokeCircle(point, 2.0f, color, 1.3f);
    }
}

std::vector<Point> wavePoints(OpWave wave, Rect r) {
    switch (wave) {
    case OpWave::Sine:
        return lfoPoints(LfoShape::Sine, r);
    case OpWave::Triangle:
        return lfoPoints(LfoShape::Triangle, r);
    case OpWave::Saw:
        return lfoPoints(LfoShape::Saw, r);
    case OpWave::Square:
        return lfoPoints(LfoShape::Square, r);
    default:
        break;
    }
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (wave) {
    case OpWave::Pulse:
        at(0.0f, 1.0f);
        at(0.25f, 1.0f);
        at(0.25f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::HalfSine:
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, 2.0f * std::sin(kPi * i / 24.0f) - 1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::AbsSine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, 2.0f * std::abs(std::sin(2.0f * kPi * i / 48.0f)) - 1.0f);
        break;
    case OpWave::AltSine: // a whole sine in the first half, then flat
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 24.0f));
        at(1.0f, 0.0f);
        break;
    case OpWave::Organ: // its drawbars added up
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.44f * (std::sin(t) + std::sin(2.0f * t) +
                                   0.5f * (std::sin(3.0f * t) + std::sin(4.0f * t)) +
                                   0.25f * (std::sin(6.0f * t) + std::sin(8.0f * t))));
        }
        break;
    case OpWave::Vowel: // a ripple at the formant, dying away through each cycle
        for (int i = 0; i <= 48; ++i) {
            const float t = i / 48.0f;
            at(t, std::exp(-3.5f * t) * std::sin(2.0f * kPi * 4.0f * t + 0.6f));
        }
        break;
    case OpWave::Noise: {
        const float values[] = {0.1f, 0.8f, -0.5f, 0.3f, -0.9f, 0.6f, -0.2f, 0.9f, -0.7f, 0.2f, -0.4f, 0.7f, -0.8f};
        for (int i = 0; i < 13; ++i)
            at(i / 12.0f, values[i]);
        break;
    }
    case OpWave::Custom: // a few partials added up, standing for whatever was drawn
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.65f * std::sin(t) + 0.35f * std::sin(3.0f * t + 0.6f) + 0.2f * std::sin(5.0f * t));
        }
        break;
    default:
        break;
    }
    return points;
}

// The partial editor, over the routing panel: the wave the partials make, their levels, and their phases.
// Each row's label sits between its box and the one above, 32 apart.
constexpr Rect kPartialWave{kRouting.x + 40.0f, kRouting.y + 44.0f, kRouting.w - 80.0f, 106.0f};
constexpr Rect kPartialLevels{kRouting.x + 40.0f, kRouting.y + 182.0f, kRouting.w - 80.0f, 84.0f};
constexpr Rect kPartialPhases{kRouting.x + 40.0f, kRouting.y + 298.0f, kRouting.w - 80.0f, 48.0f};
// The bars share the width out among the `count` partials the wave plays, closer together the more there are.
Rect partialBar(Rect area, int partial, int count) {
    const float step = area.w / count;
    const float gap = std::min(3.0f, step * 0.125f);
    return {area.x + partial * step + gap, area.y, step - 2.0f * gap, area.h};
}
int partialAt(Rect area, float x, int count) {
    return std::clamp(static_cast<int>((x - area.x) / (area.w / count)), 0, count - 1);
}
// The resolution switches in the title row, after the title: 16, 32 and 64 partials.
Rect partialCountButton(int choice) {
    return {kRouting.x + 144.0f + choice * 36.0f, kRouting.y + 9.0f, 34.0f, 22.0f};
}
// The level bars run in dB, from 0 dB at the top down to the floor just above the bottom, which is none. The
// levels themselves stay amplitudes.
constexpr double kPartialFloorDb = -60.0;
double partialLevelFromBar(double bar) {
    return bar <= 0.0 ? 0.0 : std::pow(10.0, (1.0 - std::min(bar, 1.0)) * kPartialFloorDb / 20.0);
}
double partialBarFromLevel(double level) {
    return level <= 0.0 ? 0.0 : std::clamp(1.0 - 20.0 * std::log10(level) / kPartialFloorDb, 0.0, 1.0);
}

} // namespace

float View::heightFor(const UiLayout &layout) {
    return keyboardPanel(keyboardTop(layout.sourcesCollapsed)).bottom() + 16.0f;
}

View::View(Controller &controller) : controller_(controller), table_(paramTable()) {
    poll();
}

int View::themeCount() {
    return kThemeCount;
}

const char *View::themeName(int index) {
    return kThemes[std::clamp(index, 0, kThemeCount - 1)].name;
}

View::~View() {
    if (drag_.kind == DragKind::Key)
        controller_.playNote(keyboardBase() + drag_.widget.a, false);
    if (drag_.kind == DragKind::BendWheel)
        controller_.pitchWheel(0.0);
    if (drag_.step)
        controller_.endStep();
}

// --- Model access.

double View::value(int index) const {
    return controller_.value(index);
}

bool View::isNoise(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Noise);
}

double View::norm(int index) const {
    const ParamDef &def = table_[index];
    if (def.kind == ParamKind::VoiceCount) // 1..max on a log scale
        return std::log(value(index)) / std::log(def.max);
    return (value(index) - def.min) / (def.max - def.min);
}

void View::edit(int index, double v) {
    const ParamDef &def = table_[index];
    controller_.beginEdit(index);
    controller_.setValue(index, std::clamp(v, def.min, def.max));
    controller_.endEdit(index);
}

std::vector<View::Assignment> View::assignments() const {
    std::vector<Assignment> list;
    std::vector<int> all = sources();
    for (int macro = 0; macro < kMacros; ++macro) // the macros count as sources after the places, then the note's
        all.push_back(kMaxSources + macro);         // and the mod wheel's
    all.push_back(kNoteSource);
    all.push_back(kWheelSource);
    for (int s : all) {
        for (int slot = 0; slot < kSlots; ++slot) {
            const int target = slotTarget(value(slotTargetIndex(s, slot)));
            if (target >= 0)
                list.push_back({s, slot, target, value(slotAmountIndex(s, slot)), slotBypassed(s, slot)});
        }
    }
    return list;
}

std::vector<int> View::usedSlots(int source) const {
    std::vector<int> slots;
    for (int slot = 0; slot < kSlots; ++slot)
        if (slotTarget(value(slotTargetIndex(source, slot))) >= 0)
            slots.push_back(slot);
    return slots;
}

float View::slotEnd(int source) const {
    return static_cast<float>(std::max(static_cast<int>(usedSlots(source).size()) - kVisibleSlots, 0));
}

float View::slotFirst(int source) const {
    return std::clamp(slotGlide_[source], 0.0f, slotEnd(source));
}

void View::scrollSlots(int source, float first, bool glide) {
    slotScroll_[source] = std::clamp(first, 0.0f, slotEnd(source));
    if (!glide)
        slotGlide_[source] = slotScroll_[source];
}

void View::revealSlot(int source, int slot) {
    if (isStripSource(source)) // a macro or the note source has no list of its slots to scroll
        return;
    const std::vector<int> slots = usedSlots(source);
    const float row = static_cast<float>(std::find(slots.begin(), slots.end(), slot) - slots.begin());
    const float first = slotFirst(source);
    scrollSlots(source, row < first ? row : (row + 1.0f > first + kVisibleSlots ? row + 1.0f - kVisibleSlots : first),
                false);
}

bool View::glideSlots() {
    // The shown place closes most of the way to where the wheel sent the list each poll, then settles.
    bool moved = false;
    for (int s = 0; s < kMaxSources; ++s) {
        const float to = std::clamp(slotScroll_[s], 0.0f, slotEnd(s));
        const float from = slotFirst(s);
        if (std::abs(to - from) < 0.005f) {
            moved = moved || slotGlide_[s] != to;
            slotGlide_[s] = to;
            continue;
        }
        slotGlide_[s] = from + (to - from) * 0.35f;
        moved = true;
    }
    return moved;
}

Rect View::slotThumb(int source, float card) const {
    const Rect track = slotScrollTrack(card);
    const int rows = static_cast<int>(usedSlots(source).size());
    const float h = track.h * kVisibleSlots / std::max(rows, kVisibleSlots);
    return {track.x, track.y + (track.h - h) * slotFirst(source) / std::max(slotEnd(source), 1.0f), track.w, h};
}

void View::assign(int source, int target) {
    const UndoScope step(controller_); // with the route it may keep
    int free = -1;
    for (int slot = kSlots - 1; slot >= 0; --slot) {
        const int current = slotTarget(value(slotTargetIndex(source, slot)));
        if (current == target) {
            flash(trf(Str::MsgAlreadyModulates, {sourceName(source), paramName(target)}));
            return;
        }
        if (current < 0)
            free = slot;
    }
    if (free < 0) {
        flash(trf(Str::MsgSlotsFull, {sourceName(source), std::to_string(kSlots)}));
        return;
    }
    const double amount = 0.3; // a moderate depth, always upward to begin with
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, free), slotTargetValue(target)},
                                                {slotAmountIndex(source, free), amount}};
    if (!isStripSource(source))
        changes.push_back({slotBypassIndex(source, free), 0.0});
    editMany(changes);
    revealSlot(source, free);
    // A modulated route opens and closes with the modulation, so its wire shows, even at 0 %.
    std::vector<int> &kept = controller_.layout().keptRoutes;
    if (isRouteParam(target) && std::find(kept.begin(), kept.end(), target) == kept.end()) {
        kept.push_back(target);
        controller_.layoutChanged();
    }
}

void View::removeSlot(int source, int slot) {
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, slot), 0.0},
                                                {slotAmountIndex(source, slot), 0.0}};
    if (!isStripSource(source))
        changes.push_back({slotBypassIndex(source, slot), 0.0});
    editMany(changes);
}

// A macro's or the note source's slots can't be bypassed.
bool View::slotBypassed(int source, int slot) const {
    return !isStripSource(source) && value(slotBypassIndex(source, slot)) >= 0.5;
}

void View::toggleBypass(int source, int slot) {
    if (!isStripSource(source))
        edit(slotBypassIndex(source, slot), slotBypassed(source, slot) ? 0.0 : 1.0);
}

void View::connect(int from, int to) {
    const UndoScope step(controller_); // the output a filter or shaper takes over goes in the same step
    const int param = routeParam(from, to);
    if (param < 0) {
        flash(routeRefusal(from));
        return;
    }
    if (value(param) > 0.0)
        return;
    if (isBoxNode(to)) {
        // A node that already goes to the output is moved through the filter or shaper, which goes out in its place
        // unless its sound gets there already.
        const int out = outParam(from);
        const double level = value(out);
        edit(param, level > 0.0 ? level : 1.0);
        if (level > 0.0) {
            disconnect(out);
            if (!boxGoesOut(to))
                edit(outParam(to), 1.0);
        }
    } else {
        edit(param, from == to ? 0.25 : 0.3); // modulation starts lower
    }
    keepRoutes();
}

void View::reroute(int from, int to, int into) {
    const int param = routeTo(from, to), next = routeTo(from, into);
    if (next == param)
        return;
    if (next < 0) {
        flash(routeRefusal(from));
        return;
    }
    if (wired(next)) {
        flash(trf(Str::MsgRouteExists, {nodeTitle(from), into == kNodes ? tr(Str::BusOut) : nodeTitle(into)}));
        return;
    }
    const UndoScope step(controller_); // with the routes it keeps
    const ParamDef &def = table_[next];
    std::vector<std::pair<int, double>> changes{{next, def.min + norm(param) * (def.max - def.min)}, {param, 0.0}};
    for (const Assignment &m : assignments())
        if (m.target == param)
            changes.push_back({slotTargetIndex(m.source, m.slot), slotTargetValue(next)});
    // A node's output moved into a filter or shaper goes out through it: the box goes out unless its sound gets
    // there already.
    const bool takesOver = to == kNodes && into < kNodes && isBoxNode(into) && !boxGoesOut(into);
    if (takesOver)
        changes.push_back({outParam(into), 1.0});
    editMany(changes);
    std::vector<int> &kept = controller_.layout().keptRoutes;
    std::erase(kept, param);
    kept.push_back(next); // shown even at 0 %, as the route was
    if (takesOver && std::find(kept.begin(), kept.end(), outParam(into)) == kept.end())
        kept.push_back(outParam(into));
    controller_.layoutChanged();
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool View::boxGoesOut(int node) const {
    std::array<bool, kNodes> seen{};
    auto out = [&](auto &self, int box) -> bool {
        if (seen[box])
            return false;
        seen[box] = true;
        if (value(outParam(box)) > 0.0)
            return true;
        for (int to = kOps; to < kNodes; ++to) {
            const int param = to == box ? -1 : routeParam(box, to);
            if (param >= 0 && value(param) > 0.0 && nodeOn(to) && self(self, to))
                return true;
        }
        return false;
    };
    return out(out, node);
}

int View::rerouting() const {
    return drag_.kind == DragKind::Reroute && drag_.moved ? drag_.widget.param : -1;
}

bool View::soundsOut(int node) const {
    return value(outParam(node)) > 0.0 && (!isBoxNode(node) || boxHasInput(node));
}

// Operators, filters and shapers alike, whether anything goes into them or not.
bool View::goesOut(int node) const {
    const int param = outParam(node);
    return param >= 0 && wired(param);
}

bool View::wired(int param) const {
    const std::vector<int> &kept = controller_.layout().keptRoutes;
    return value(param) > 0.0 || std::find(kept.begin(), kept.end(), param) != kept.end();
}

void View::keepRoutes() {
    std::vector<int> &kept = controller_.layout().keptRoutes;
    auto keep = [&](int param) {
        if (param >= 0 && value(param) > 0.0 && std::find(kept.begin(), kept.end(), param) == kept.end())
            kept.push_back(param);
    };
    for (int from = 0; from < kNodes; ++from) {
        keep(outParam(from));
        for (int to = 0; to < kNodes; ++to)
            keep(routeParam(from, to));
    }
}

// Its modulation goes with it, or the route would still open and close unseen.
void View::disconnect(int param) {
    const UndoScope step(controller_);
    for (const Assignment &m : assignments())
        if (m.target == param)
            removeSlot(m.source, m.slot);
    edit(param, 0.0);
    std::erase(controller_.layout().keptRoutes, param);
}

Color View::fxColor(int slot) const {
    return fxTypeColor(fxSlotType(slot));
}

Color View::listColor(int param) const {
    return shaperOf(param) >= 0 ? kShaperColor : fxSlotOf(param) >= 0 ? fxColor(fxSlotOf(param)) : kFilterColor;
}

int View::listShown(int param) const {
    if (isFilterSlope(param))
        return playedSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), filterSlope(value(param)));
    return static_cast<int>(std::lround(value(param)));
}

bool View::listItemFits(int param, int item) const {
    return !isFilterSlope(param) || slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), item);
}

Color View::clipColor(int unit) const {
    return unit < kFxSlots ? fxColor(unit) : kShaperColor;
}

bool View::nodeOn(int node) const {
    return value(enabledParam(node)) >= 0.5;
}

// What a matrix route joins switches on: the row's node, and the column's unless it is the output.
void View::switchOnRoute(int from, int to) {
    for (int node : {from, to})
        if (node < kNodes && !nodeOn(node))
            edit(enabledParam(node), 1.0);
}

std::vector<int> View::enabledNodes() const {
    std::vector<int> nodes;
    for (int node = 0; node < kNodes; ++node)
        if (nodeOn(node))
            nodes.push_back(node);
    return nodes;
}

// Filters and shapers can feed each other round in a loop, so each is looked at once.
bool View::boxHasInput(int node) const {
    std::array<bool, kNodes> seen{};
    auto fed = [&](auto &self, int box) -> bool {
        if (seen[box] || !nodeOn(box))
            return false;
        seen[box] = true;
        for (int from = 0; from < kNodes; ++from) {
            const int param = from == box ? -1 : routeParam(from, box);
            if (param >= 0 && value(param) > 0.0 && nodeOn(from) && (from < kOps || self(self, from)))
                return true;
        }
        return false;
    };
    return fed(fed, node);
}

bool View::reachesOut(int node) const {
    // A search along the routes that are on, from `node` through switched-on nodes to one that goes out.
    std::array<bool, kNodes> visited{};
    auto reaches = [&](auto &self, int from) -> bool {
        if (visited[from] || !nodeOn(from))
            return false;
        visited[from] = true;
        if (const int out = outParam(from); out >= 0 && value(out) > 0.0)
            return true;
        for (int to = 0; to < kNodes; ++to) {
            const int param = to == from ? -1 : routeParam(from, to);
            if (param >= 0 && value(param) > 0.0 && self(self, to))
                return true;
        }
        return false;
    };
    return reaches(reaches, node);
}

// The freeform shape editor: an LFO's fills the routing panel and the inspector, which it hides; a shaper's curve
// takes a narrower panel in the middle of the effects' panel it opens from, so its plot is closer to square, the
// inspector still covered; the note source's curve fills the routing panel.
constexpr float kClipEditorW = 520.0f;
Rect View::shapePanel() const {
    const int shape = shapeEditing();
    if (shape >= 0 && isClipCurve(shape))
        return {kFxPanel.x + std::round((kFxPanel.w - kClipEditorW) * 0.5f), kFxPanel.y, kClipEditorW, kFxPanel.h};
    return shape >= 0 && !isCurve(shape) ? kFxPanel : kRouting;
}

Rect View::shapeArea(int shape) const {
    if (shape >= 0 && isClipCurve(shape) && shapeEditing() != shape) { // in its card
        const std::vector<int> chain = fxChainShown();
        const auto at = std::find(chain.begin(), chain.end(), shape - kMaxSources);
        return fxDisplay(fxScreen(chain, static_cast<size_t>(at - chain.begin())), FxType::Shaper).inset(8.0f);
    }
    if (shape >= 0 && isShaperCurve(shape) && shapeEditing() != shape) // on its page in the inspector
        return kShaperDisplay.inset(8.0f);
    const Rect panel = shapePanel();
    return {panel.x + 40.0f, panel.y + 60.0f, panel.w - 80.0f, panel.h - 96.0f};
}

Rect View::shapeDone() const {
    const Rect panel = shapePanel();
    return {panel.right() - 16.0f - 60.0f, panel.y + 9.0f, 60.0f, 22.0f};
}

Rect View::shapeReset() const {
    const Rect done = shapeDone();
    return {done.x - 8.0f - 60.0f, done.y, 60.0f, 22.0f};
}

// The grid's controls, under the editor's right end: per axis a step down (part 0), the count and a step up.
Rect View::shapeGridButton(int axis, int part) const {
    constexpr float kParts[] = {18.0f, 28.0f, 18.0f};
    const Rect area = shapeArea();
    const float group = area.right() - (axis == 0 ? 148.0f : 64.0f);
    const float x = group + (part > 0 ? kParts[0] : 0.0f) + (part > 1 ? kParts[1] : 0.0f);
    return {x, area.bottom() + 9.0f, kParts[part], 18.0f};
}

// A little room above and below the full swing, so its points can be grabbed: 12 px, less in a card.
static float shapeReach(Rect area) {
    return area.h * 0.5f - std::min(12.0f, area.h * 0.06f);
}

Point View::shapeAt(int shape, double x, double y) const {
    const Rect area = shapeArea(shape);
    return {area.x + static_cast<float>(x) * area.w, area.center().y - static_cast<float>(y) * shapeReach(area)};
}

double View::shapeXAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return (p.x - area.x) / area.w;
}

double View::shapeYAt(int shape, Point p) const {
    const Rect area = shapeArea(shape);
    return std::clamp((area.center().y - p.y) / shapeReach(area), -1.0f, 1.0f);
}

bool View::inspectorCovered() const {
    const int shape = shapeEditing();
    return fxWide() || (shape >= 0 && !isNoteCurve(shape) && !isShaperCurve(shape));
}

bool View::fxWide() const {
    return controller_.layout().view == RoutingView::Fx && shapeEditing() < 0;
}

FxType View::fxSlotType(int slot) const {
    return fxType(value(paramIndex(slot, FxParam::Type)));
}

std::vector<int> View::fxChain() const {
    std::vector<int> chain;
    for (int slot : fxOrder(value(paramIndex(GlobalParam::FxOrder))))
        if (fxSlotType(slot) != FxType::None)
            chain.push_back(slot);
    return chain;
}

std::vector<int> View::fxChainShown() const {
    std::vector<int> chain = fxChain();
    if (drag_.kind != DragKind::FxMove)
        return chain;
    // The dragged card lands after the other cards whose middles are left of its own, among those on screen. It
    // stays under the mouse while the row scrolls under it.
    const int slot = drag_.widget.a;
    const float moved = (drag_.now.x - drag_.start.x) / (kFxCardW + kFxGap) + fxScroll() - drag_.scrollStart;
    const float middle = fxOffset(chain, drag_.widget.b) + 0.5f * fxCardSpan(slot) + moved;
    int first = 0, last = 0; // the cards that start on screen
    for (size_t i = 0; i < chain.size(); ++i) {
        const float place = fxOffset(chain, i) - fxScroll();
        if (place < -0.01f)
            first = static_cast<int>(i) + 1;
        if (place + fxCardSpan(chain[i]) < kVisibleFx + 0.01f)
            last = static_cast<int>(i);
    }
    chain.erase(std::remove(chain.begin(), chain.end(), slot), chain.end());
    int to = 0;
    float offset = 0.0f;
    for (int other : chain) {
        const float span = fxCardSpan(other);
        to += offset + 0.5f * span < middle;
        offset += span;
    }
    to = std::clamp(to, first, std::max(last, first));
    chain.insert(chain.begin() + std::min(to, static_cast<int>(chain.size())), slot);
    return chain;
}

float View::fxOffset(const std::vector<int> &chain, size_t index) const {
    float offset = 0.0f;
    for (size_t i = 0; i < index && i < chain.size(); ++i)
        offset += fxCardSpan(chain[i]);
    return offset;
}

bool View::fxFolded(int slot) const {
    return (controller_.layout().fxFolded >> slot) & 1u;
}

bool View::sourceFolded(int source) const {
    return !isStripSource(source) && ((controller_.layout().sourcesFolded >> source) & 1u);
}

float View::fxCardSpan(int slot) const {
    return fxFolded(slot) ? kFoldedFx : fxSpan(fxSlotType(slot));
}

float View::sourceSpan(int source) const {
    return sourceFolded(source) ? kFoldedSource : 1.0f;
}

float View::sourceOffset(const std::vector<int> &shown, size_t index) const {
    float offset = 0.0f;
    for (size_t i = 0; i < index && i < shown.size(); ++i)
        offset += sourceSpan(shown[i]);
    return offset;
}

// Folding a card or unfolding it; a new effect or source starts unfolded.
void View::setFolded(bool fx, int index, bool folded) {
    uint32_t &bits = fx ? controller_.layout().fxFolded : controller_.layout().sourcesFolded;
    const uint32_t before = bits;
    bits = folded ? bits | (1u << index) : bits & ~(1u << index);
    if (bits != before)
        controller_.layoutChanged();
}

double View::fxOrderWith(const std::vector<int> &chain) const {
    FxOrder order{};
    size_t n = 0;
    for (int slot : chain)
        order[n++] = slot;
    for (int slot : fxOrder(value(paramIndex(GlobalParam::FxOrder))))
        if (std::find(chain.begin(), chain.end(), slot) == chain.end())
            order[n++] = slot;
    return fxOrderValue(order);
}

float View::fxScroll() const {
    return std::clamp(fxScroll_, 0.0f, std::max(scrollLength(ScrollArea::Fx) - kVisibleFx, 0.0f));
}

float View::fxScreen(const std::vector<int> &chain, size_t index) const {
    return fxOffset(chain, index) - fxScroll();
}

// On screen, at least in part, for a card `span` places wide.
static bool onScreen(float place, int shown, float span = 1.0f) {
    return place > -span && place < static_cast<float>(shown);
}

float View::scrollLength(ScrollArea area) const {
    if (area == ScrollArea::Sources) {
        const std::vector<int> shown = sources();
        return sourceOffset(shown, shown.size()) + (sourceShadow() ? kSourceShadowSpan : 0.0f);
    }
    const std::vector<int> chain = fxChain();
    return fxOffset(chain, chain.size()) + (fxShadow() ? 1.0f : 0.0f);
}

bool View::fxShadow() const {
    return fxChain().size() < static_cast<size_t>(kFxSlots);
}

bool View::sourceShadow() const {
    return sources().size() < static_cast<size_t>(kMaxSources);
}

float View::scrollFirst(ScrollArea area) const {
    return area == ScrollArea::Fx ? fxScroll() : sourceScroll();
}

void View::setScroll(ScrollArea area, float first) {
    (area == ScrollArea::Fx ? fxScroll_ : sourceScroll_) = std::max(first, 0.0f); // the getters clamp the top end
}

Rect View::scrollThumb(ScrollArea area) const {
    const Rect track = area == ScrollArea::Fx ? fxScrollTrack() : sourceScrollTrack();
    const float cards = std::max(scrollLength(area), 1.0f);
    const float shown = static_cast<float>(area == ScrollArea::Fx ? kVisibleFx : kVisibleCards);
    const float width = std::max(track.w * std::min(shown, cards) / cards, 24.0f);
    const float hidden = std::max(cards - shown, 1.0f);
    return {track.x + (track.w - width) * scrollFirst(area) / hidden, track.y, width, track.h};
}

void View::revealFx(int position) {
    const std::vector<int> chain = fxChain();
    const float first = fxScroll();
    const float place = fxOffset(chain, position);
    const float span = position < static_cast<int>(chain.size()) ? fxCardSpan(chain[position]) : 1.0f;
    fxScroll_ = place < first ? place : (place + span > first + kVisibleFx ? place + span - kVisibleFx : first);
}

void View::addEffect(FxType type, int copyOf, int at) {
    std::vector<int> chain = fxChain();
    int slot = -1;
    for (int s = 0; s < kFxSlots && slot < 0; ++s)
        if (fxSlotType(s) == FxType::None)
            slot = s;
    if (slot < 0) {
        flash(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        return;
    }
    // The slot gets its settings, the original's for a copy, then its type last: that makes the host rescan
    // the slot's fields, which by then hold what they should.
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < kFxParamCount; ++f) {
        const int index = paramIndex(slot, static_cast<FxParam>(f));
        if (static_cast<FxParam>(f) != FxParam::Type)
            changes.push_back(
                {index, copyOf >= 0 ? value(paramIndex(copyOf, static_cast<FxParam>(f))) : table_[index].def});
    }
    for (int f = 0; f < kShapeParamCount; ++f) { // the shaper's drawn curve
        const int index = clipCurveCountIndex(slot) + f;
        changes.push_back({index, copyOf >= 0 ? value(clipCurveCountIndex(copyOf) + f) : table_[index].def});
    }
    changes.push_back({paramIndex(slot, FxParam::Type), static_cast<double>(type)});
    // A new effect, a copy too, goes where it was asked for, or at the end of the chain, which scrolls to show it.
    const int position = at >= 0 && at < static_cast<int>(chain.size()) ? at : static_cast<int>(chain.size());
    chain.insert(chain.begin() + position, slot);
    changes.push_back({paramIndex(GlobalParam::FxOrder), fxOrderWith(chain)});
    setFolded(true, slot, false);
    editMany(changes);
    fxBand_[slot] = copyOf >= 0 ? fxBand_[copyOf] : 0;
    revealFx(position);
}

void View::removeEffect(int slot) {
    const UndoScope step(controller_);
    setFolded(true, slot, false);
    // Modulation of its knobs goes with it.
    for (const Assignment &m : assignments())
        if (fxSlotOf(m.target) == slot)
            removeSlot(m.source, m.slot);
    edit(paramIndex(slot, FxParam::Type), static_cast<double>(FxType::None));
}

Rect View::peqArea(int slot) const {
    const std::vector<int> chain = fxChainShown();
    const auto pos = std::find(chain.begin(), chain.end(), slot) - chain.begin();
    return fxDisplay(fxScreen(chain, static_cast<size_t>(pos)), FxType::ParamEq).inset(8.0f);
}

void View::editMany(const std::vector<std::pair<int, double>> &changes) {
    for (const auto &[index, v] : changes)
        controller_.beginEdit(index);
    for (const auto &[index, v] : changes)
        controller_.setValue(index, std::clamp(v, table_[index].min, table_[index].max));
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        controller_.endEdit(it->first);
}

int View::shapeEditing() const {
    if (shapeEditor_ < 0)
        return -1;
    if (isNoteCurve(shapeEditor_)) // open until it is closed
        return shapeEditor_;
    if (isShaperCurve(shapeEditor_)) {
        const int shaper = shapeEditor_ - kMaxSources - kFxSlots;
        return clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom ? shapeEditor_ : -1;
    }
    if (isClipCurve(shapeEditor_)) {
        const int slot = shapeEditor_ - kMaxSources;
        return fxSlotType(slot) == FxType::Shaper &&
                       std::lround(value(paramIndex(slot, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Custom)
                   ? shapeEditor_
                   : -1;
    }
    if (sourceTypeOf(shapeEditor_) != SourceType::Lfo ||
        std::lround(value(paramIndex(shapeEditor_, SourceParam::Shape))) != static_cast<long>(LfoShape::Custom))
        return -1;
    return shapeEditor_;
}

int View::shapeBase(int shape) {
    return isNoteCurve(shape) ? paramIndex(NoteParam::Curve)
           : isCurve(shape)   ? shaperParams(shape - kMaxSources).curve // an effect's or a voice shaper's
                              : shapeCountIndex(shape);
}

Color View::shapeColor(int shape) const {
    return isNoteCurve(shape)                         ? kSourceColors[kNoteSource]
           : isClipCurve(shape) || isShaperCurve(shape) ? clipColor(shape - kMaxSources)
                                                        : kSourceColors[shape];
}

int View::shapeIndex(int shape, int point, PointField field) const {
    return shapeFieldIndex(shapeBase(shape), point, field);
}

int View::shapePointCount(int shape) const {
    return fmvibe::shapePointCount(value(shapeBase(shape)), shapeMaxPoints(shape));
}

// The first point sits at the start; a curve's last one at the end.
double View::shapePointX(int shape, int point) const {
    if (point == 0)
        return 0.0;
    if (isCurve(shape) && point + 1 == shapePointCount(shape))
        return 1.0;
    return value(shapeIndex(shape, point, PointField::X));
}

Point View::shapePointPos(int shape, int point) const {
    return shapeAt(shape, shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)));
}

// Halfway along the segment, on the curve.
Point View::shapeCurvePos(int shape, int point) const {
    const Point a = shapePointPos(shape, point);
    const bool wraps = point + 1 == shapePointCount(shape);
    const Point b =
        wraps ? Point{shapeArea(shape).right(), shapePointPos(shape, 0).y} : shapePointPos(shape, point + 1);
    const float t = static_cast<float>(shapeBend(0.5, value(shapeIndex(shape, point, PointField::Curve))));
    return {(a.x + b.x) * 0.5f, a.y + (b.y - a.y) * t};
}

void View::addShapePoint(int shape, Point p) {
    const int count = shapePointCount(shape);
    if (count >= shapeMaxPoints(shape)) {
        flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
        return;
    }
    const double x = std::clamp(shapeXAt(shape, p), 0.001, 0.999);
    int at = 1;
    while (at < count && shapePointX(shape, at) < x)
        ++at;
    // Later points move up one place to make room; a curve's last point stays at the end.
    std::vector<std::pair<int, double>> changes;
    for (int point = count - 1; point >= at; --point) {
        changes.push_back({shapeIndex(shape, point + 1, PointField::X), shapePointX(shape, point)});
        for (PointField field : {PointField::Y, PointField::Curve})
            changes.push_back({shapeIndex(shape, point + 1, field), value(shapeIndex(shape, point, field))});
    }
    changes.push_back({shapeIndex(shape, at, PointField::X), x});
    changes.push_back({shapeIndex(shape, at, PointField::Y), shapeYAt(shape, p)});
    changes.push_back({shapeIndex(shape, at, PointField::Curve), 0.0});
    changes.push_back({shapeBase(shape), count + 1.0});
    editMany(changes);
}

// A step, as in a step sequencer: a level line from the grid line left of p to the one right of it, at p's height,
// with a point at each end. The points between the two lines go; a point already on a line stays, for the step
// next to this one, the new one beside it at the same place so the shape jumps straight up or down there.
void View::addShapeStep(int shape, Point p, bool snap) {
    const UiLayout &layout = controller_.layout();
    const int lines = layout.shapeGridX;
    const int column = std::clamp(static_cast<int>(std::floor(shapeXAt(shape, p) * lines)), 0, lines - 1);
    const double x0 = static_cast<double>(column) / lines, x1 = static_cast<double>(column + 1) / lines;
    double y = shapeYAt(shape, p);
    if (snap) {
        const double half = 0.5 * layout.shapeGridY; // lines per unit of height
        y = std::round((y + 1.0) * half) / half - 1.0;
    }
    constexpr double kOnLine = 1e-4;
    struct Pt {
        double x, y, curve;
    };
    const int count = shapePointCount(shape);
    auto at = [&](int point) {
        return Pt{shapePointX(shape, point), value(shapeIndex(shape, point, PointField::Y)),
                  value(shapeIndex(shape, point, PointField::Curve))};
    };
    std::vector<Pt> before, left, right, after; // left and right: on the step's lines; the ones between go
    for (int point = 0; point < count; ++point) {
        const Pt pt = at(point);
        if (pt.x < x0 - kOnLine)
            before.push_back(pt);
        else if (pt.x < x0 + kOnLine)
            left.push_back(pt);
        else if (pt.x > x1 + kOnLine)
            after.push_back(pt);
        else if (pt.x > x1 - kOnLine)
            right.push_back(pt);
    }
    // Of the points on a line, the one facing the step goes and the others stay: on the left line the last, on the
    // right line the first. A point alone there stays only for a step beside this one: a level line a column long.
    // The first point is always at the start, and a curve's last one at the end: a step there takes its place.
    auto stepBetween = [&](const Pt &a, const Pt &b) {
        return std::abs(b.x - a.x - (x1 - x0)) < kOnLine && std::abs(b.y - a.y) < 1e-6;
    };
    const bool leftStep = left.size() == 1 && !before.empty() && stepBetween(before.back(), left[0]);
    if (x0 <= 0.0 || (left.size() == 1 && !leftStep))
        left.clear();
    else if (left.size() > 1)
        left.pop_back();
    const bool rightStep = right.size() == 1 && !after.empty() && stepBetween(right[0], after.front());
    if (x1 >= 1.0 || (right.size() == 1 && !rightStep))
        right.clear();
    else if (right.size() > 1)
        right.erase(right.begin());
    std::vector<Pt> points = before;
    points.insert(points.end(), left.begin(), left.end());
    points.push_back({x0, y, 0.0});
    points.push_back({x1, y, 0.0});
    points.insert(points.end(), right.begin(), right.end());
    points.insert(points.end(), after.begin(), after.end());
    if (static_cast<int>(points.size()) > shapeMaxPoints(shape)) {
        flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
        return;
    }
    std::vector<std::pair<int, double>> changes;
    auto change = [&](int index, double v) {
        if (v != value(index))
            changes.push_back({index, v});
    };
    for (size_t point = 0; point < points.size(); ++point) {
        if (point > 0) // the first point's place is the start, whatever its X says
            change(shapeIndex(shape, static_cast<int>(point), PointField::X), points[point].x);
        change(shapeIndex(shape, static_cast<int>(point), PointField::Y), points[point].y);
        change(shapeIndex(shape, static_cast<int>(point), PointField::Curve), points[point].curve);
    }
    change(shapeBase(shape), static_cast<double>(points.size()));
    editMany(changes);
}

// Shift+click anywhere in the big editor, over its points and bends too, draws a step.
bool View::drawsStep(const Widget &w, Modifiers mods) const {
    return mods.shift && (w.kind == Kind::ShapeArea || w.kind == Kind::ShapePoint || w.kind == Kind::ShapeCurve) &&
           w.a == shapeEditing();
}

void View::removeShapePoint(int shape, int point) {
    const int count = shapePointCount(shape);
    const bool end = isCurve(shape) && point + 1 == count;
    if (point == 0 || end || count <= 2) {
        flash(tr(isNoteCurve(shape) && (point == 0 || end) ? Str::MsgNoteCurveEnds
                 : isCurve(shape) && (point == 0 || end)   ? Str::MsgCurveEnds
                 : point == 0                              ? Str::MsgFirstPoint
                                                           : Str::MsgTwoPoints));
        return;
    }
    std::vector<std::pair<int, double>> changes;
    for (int p = point; p + 1 < count; ++p) {
        changes.push_back({shapeIndex(shape, p, PointField::X), shapePointX(shape, p + 1)});
        for (PointField field : {PointField::Y, PointField::Curve})
            changes.push_back({shapeIndex(shape, p, field), value(shapeIndex(shape, p + 1, field))});
    }
    changes.push_back({shapeBase(shape), count - 1.0});
    editMany(changes);
}

void View::resetShape(int shape) {
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < shapeParamCount(shape); ++f)
        changes.push_back({shapeBase(shape) + f, table_[shapeBase(shape) + f].def});
    if (isNoteCurve(shape)) // and its range, every MIDI note
        for (int index : {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)})
            changes.push_back({index, table_[index].def});
    editMany(changes);
}

void View::stepShapeGrid(int axis, int steps) {
    UiLayout &layout = controller_.layout();
    int &lines = axis == 0 ? layout.shapeGridX : layout.shapeGridY;
    auto step = [&](const auto &offered) {
        const int count = static_cast<int>(std::size(offered));
        int at = 0;
        while (at + 1 < count && offered[at] < lines)
            ++at;
        lines = offered[std::clamp(at + steps, 0, count - 1)];
    };
    if (axis == 0)
        step(kShapeGridsX);
    else
        step(kShapeGridsY);
    controller_.layoutChanged();
}

bool View::edgeScroll() {
    // A handle being dragged onto a target scrolls whichever row it is over; a card being moved, its own row.
    const bool handle = drag_.kind == DragKind::ModSource;
    const bool card = (drag_.kind == DragKind::FxMove || drag_.kind == DragKind::SourceMove) && drag_.moved;
    if (!handle && !card)
        return false;
    // Within kZone of a panel's left or right edge, or past it, the row moves up to kStep cards a poll. A card
    // grabbed that close to an edge only scrolls once it is taken further toward it than where it was grabbed.
    constexpr float kZone = 28.0f, kStep = 0.06f;
    const Point p = drag_.now;
    auto pull = [&](Rect panel) {
        if (handle && (p.y < panel.y || p.y >= panel.bottom()))
            return 0.0f;
        const float leftEdge = card ? std::min(panel.x + kZone, drag_.start.x - 4.0f) : panel.x + kZone;
        const float rightEdge = card ? std::max(panel.right() - kZone, drag_.start.x + 4.0f) : panel.right() - kZone;
        const float left = (leftEdge - p.x) / kZone;
        const float right = (p.x - rightEdge) / kZone;
        return left > 0.0f ? -std::min(left, 1.0f) : (right > 0.0f ? std::min(right, 1.0f) : 0.0f);
    };
    for (ScrollArea area : {ScrollArea::Sources, ScrollArea::Fx}) {
        const bool sources = area == ScrollArea::Sources;
        if (sources ? controller_.layout().sourcesCollapsed : !fxWide())
            continue;
        if (card && sources != (drag_.kind == DragKind::SourceMove))
            continue;
        const float speed = pull(sources ? modPanel(false) : kFxPanel);
        if (speed == 0.0f)
            continue;
        const float before = scrollFirst(area);
        setScroll(area, before + kStep * (speed < 0.0f ? -0.35f + 0.65f * speed : 0.35f + 0.65f * speed));
        return scrollFirst(area) != before;
    }
    return false;
}

void View::flash(std::string message) {
    flash_ = std::move(message);
    flashTicks_ = 160; // ~2.5 s of polls
}

int View::keyboardBase() const {
    return kBaseNote + 12 * octave_;
}

std::vector<int> View::sources() const {
    return shownSources(controller_.layout(), [this](int index) { return value(index); });
}

std::vector<int> View::sourcesShown() const {
    std::vector<int> shown = sources();
    if (drag_.kind != DragKind::SourceMove)
        return shown;
    // The dragged card lands after the other cards whose middles are left of its own, among those on screen. It
    // stays under the mouse while the row scrolls under it.
    const int source = drag_.widget.a;
    const float moved = (drag_.now.x - drag_.start.x) / (kCardW + kCardGap) + sourceScroll() - drag_.scrollStart;
    const float middle = sourceOffset(shown, drag_.widget.b) + 0.5f * sourceSpan(source) + moved;
    int first = 0, last = 0; // the cards that start on screen
    for (size_t i = 0; i < shown.size(); ++i) {
        const float place = sourceOffset(shown, i) - sourceScroll();
        if (place < -0.01f)
            first = static_cast<int>(i) + 1;
        if (place + sourceSpan(shown[i]) < kVisibleCards + 0.01f)
            last = static_cast<int>(i);
    }
    shown.erase(std::remove(shown.begin(), shown.end(), source), shown.end());
    int to = 0;
    float offset = 0.0f;
    for (int other : shown) {
        to += offset + 0.5f * sourceSpan(other) < middle;
        offset += sourceSpan(other);
    }
    to = std::clamp(to, first, std::max(last, first));
    shown.insert(shown.begin() + std::min(to, static_cast<int>(shown.size())), source);
    return shown;
}

SourceType View::sourceTypeOf(int source) const {
    return isStripSource(source) ? SourceType::None : sourceType(value(paramIndex(source, SourceParam::Type)));
}

std::string View::sourceName(int source) const {
    return fmvibe::gui::sourceName(controller_.layout(), [this](int index) { return value(index); }, source);
}

std::string View::paramName(int index) const {
    if (index < 0 || index >= kParamCount)
        return std::string();
    if (const int owner = bypassSourceOf(index); owner >= 0) {
        const int target = slotTarget(value(slotTargetIndex(owner, (index - kSlotBypassBase) % kSlots)));
        return target >= 0 ? trf(Str::ParamNameBypassOn, {sourceName(owner), paramName(target)})
                           : trf(Str::ParamNameBypass, {sourceName(owner)});
    }
    const int source = sourceOf(index);
    const std::string &name = table_[index].name;
    if (source < 0)
        return name;
    const size_t place = trf(Str::ParamMod, {std::to_string(source + 1)}).size() + 1; // see buildTable()
    return sourceName(source) + " " + name.substr(place);
}

bool View::sourceOn(int source) const {
    return isStripSource(source) || value(sourceEnabledIndex(source)) >= 0.5;
}

bool View::modFocused(const Assignment &m) const {
    const Widget *w = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    return w && w->kind == Kind::ModDot && w->a == m.source && w->b == m.slot;
}

bool View::sourceLive(int source) const {
    return isStripSource(source) ? !usedSlots(source).empty() : sourceOn(source);
}

float View::sourceScroll() const {
    return std::clamp(sourceScroll_, 0.0f, std::max(scrollLength(ScrollArea::Sources) - kVisibleCards, 0.0f));
}

float View::cardOf(int source) const {
    if (controller_.layout().sourcesCollapsed)
        return kOffScreen;
    const std::vector<int> shown = sourcesShown();
    const auto at = std::find(shown.begin(), shown.end(), source);
    const float card = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
    return at != shown.end() && onScreen(card, kVisibleCards, sourceSpan(source)) ? card : kOffScreen;
}

void View::revealSource(int source) {
    const std::vector<int> shown = sources();
    const float place = sourceOffset(shown, static_cast<size_t>(std::find(shown.begin(), shown.end(), source) - shown.begin()));
    const float span = sourceSpan(source);
    const float first = sourceScroll();
    sourceScroll_ = place < first ? place : (place + span > first + kVisibleCards ? place + span - kVisibleCards : first);
}

void View::setSourceOrder(const std::vector<int> &shown) {
    UiLayout &layout = controller_.layout();
    std::array<int, kMaxSources> order{};
    size_t n = 0;
    for (int source : shown)
        order[n++] = source;
    for (int source : layout.sourceOrder)
        if (std::find(shown.begin(), shown.end(), source) == shown.end())
            order[n++] = source;
    if (order == layout.sourceOrder)
        return;
    layout.sourceOrder = order;
    controller_.layoutChanged();
}

void View::addSource(SourceType type, int copyOf) {
    const UndoScope step(controller_); // with where it shows
    std::vector<int> shown = sources();
    int source = -1;
    for (int s = 0; s < kMaxSources && source < 0; ++s)
        if (sourceTypeOf(s) == SourceType::None)
            source = s;
    if (source < 0) {
        flash(trf(Str::MsgSourcesFull, {std::to_string(kMaxSources)}));
        return;
    }
    // Every field, its slots and its freeform shape: fresh, or as in the original.
    // The type comes last, as in addEffect().
    std::vector<std::pair<int, double>> changes;
    auto take = [&](int index, int from) {
        changes.push_back({index, copyOf >= 0 ? value(from) : table_[index].def});
    };
    for (int f = 0; f < kSourceParamCount; ++f)
        if (static_cast<SourceParam>(f) != SourceParam::Type)
            take(paramIndex(source, static_cast<SourceParam>(f)),
                 paramIndex(std::max(copyOf, 0), static_cast<SourceParam>(f)));
    for (int f = 0; f < kShapeParamCount; ++f)
        take(shapeCountIndex(source) + f, shapeCountIndex(std::max(copyOf, 0)) + f);
    for (int slot = 0; slot < kSlots; ++slot)
        take(slotBypassIndex(source, slot), slotBypassIndex(std::max(copyOf, 0), slot));
    changes.push_back({paramIndex(source, SourceParam::Type), static_cast<double>(type)});
    const auto at = copyOf >= 0 ? std::find(shown.begin(), shown.end(), copyOf) + 1 : shown.end();
    shown.insert(at, source);
    setSourceOrder(shown);
    setFolded(false, source, false);
    editMany(changes);
    slotScroll_[source] = slotGlide_[source] = copyOf >= 0 ? slotScroll_[copyOf] : 0.0f;
    if (controller_.layout().sourcesCollapsed) {
        controller_.layout().sourcesCollapsed = false;
        controller_.layoutChanged();
    }
    revealSource(source);
}

void View::removeSource(int source) {
    const UndoScope step(controller_);
    setFolded(false, source, false);
    // Its own slots, and every slot that modulates one of its knobs.
    for (const Assignment &m : assignments())
        if (m.source == source || sourceOf(m.target) == source)
            removeSlot(m.source, m.slot);
    if (shapeEditor_ == source)
        shapeEditor_ = -1;
    edit(paramIndex(source, SourceParam::Type), static_cast<double>(SourceType::None));
}

// Snapshots come once per audio block, which can be tens of milliseconds apart;
// in between, the phase moves on at the LFO's rate, so the dot glides. A host
// that stopped processing doesn't send it far. A one-shot stops at the end of its cycle.
double View::lfoPhaseNow(int lfo) const {
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    const double phase = telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age;
    if (value(paramIndex(lfo, SourceParam::OneShot)) >= 0.5)
        return std::min(phase, 1.0);
    return phase - std::floor(phase);
}

double View::lfoValueNow(int lfo) const {
    const auto shape = static_cast<LfoShape>(std::clamp(
        static_cast<int>(std::lround(value(paramIndex(lfo, SourceParam::Shape)))), 0, static_cast<int>(LfoShape::Count) - 1));
    if (shape == LfoShape::Random) { // can't be told ahead
        const double out = telemetry_.sources[lfo];
        return value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? out * 2.0 - 1.0 : out;
    }
    if (shape == LfoShape::Custom)
        return shapeValue([this](int index) { return value(index); }, lfo, lfoPhaseNow(lfo));
    return lfoValue(shape, lfoPhaseNow(lfo), 0.0);
}

void View::traceRandomLfos() {
    // A new cycle is a new glide; one that stops running, or isn't random any more, starts over.
    for (int s = 0; s < kMaxSources; ++s) {
        RandomTrace &trace = randomTraces_[s];
        const bool random = sourceTypeOf(s) == SourceType::Lfo &&
                            std::lround(value(paramIndex(s, SourceParam::Shape))) == static_cast<long>(LfoShape::Random);
        if (!random || telemetry_.voices == 0 || !sourceOn(s)) {
            trace.count = 0;
            continue;
        }
        const std::pair glide{telemetry_.lfoFrom[s], telemetry_.lfoTo[s]};
        const float phase = telemetry_.lfoPhases[s];
        if (trace.count > 0 && trace.glides[(trace.count - 1) % kRandomCycles] == glide) {
            trace.ended = trace.ended || phase < trace.phase;
            trace.phase = phase;
            continue;
        }
        trace.glides[trace.count % kRandomCycles] = glide;
        ++trace.count;
        trace.phase = phase;
        trace.ended = false;
    }
}

bool View::randomSweep(int lfo) const {
    return telemetry_.voices > 0 && sourceOn(lfo) && randomTraces_[lfo].count > 0;
}

double View::randomPhaseNow(int lfo) const {
    if (randomTraces_[lfo].ended)
        return 1.0;
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    return std::min(telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age, 1.0);
}

bool View::poll() {
    bool changed = false;
    for (int i = 0; i < kParamCount; ++i) {
        const double v = controller_.value(i);
        if (v != seen_[i]) {
            seen_[i] = v;
            changed = true;
        }
    }
    if (changed)
        keepRoutes(); // whatever turned a route on, the editor or the host
    Telemetry t;
    controller_.telemetry(t);
    if (t.voices > 0 || telemetry_.voices > 0 || t.keys != telemetry_.keys)
        changed = true; // animating
    // The output meter falls while it shows anything, also once the audio has stopped and the snapshot ages, and
    // is painted once more as it goes.
    const float bottom = static_cast<float>(std::pow(10.0, kMeterFloorDb / 20.0));
    const bool metering = std::max(meterPeak(t, 0), meterPeak(t, 1)) > bottom || meterClipped(t);
    if (metering || metering_)
        changed = true;
    metering_ = metering;
    // Phasers, flangers and wideners sweep while the audio runs.
    if (t.age < 0.5f)
        for (int slot : fxChain())
            if (fxSlotType(slot) == FxType::Phaser || fxSlotType(slot) == FxType::Flanger ||
                fxSlotType(slot) == FxType::Widener)
                changed = true;
    // An analyzer's spectrum goes on falling away after the audio stops, until it is gone; while notes play, the
    // filters' move along with them.
    auto showing = [](const Spectrum &spectrum) {
        return std::any_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; });
    };
    if (t.age < 1.5f)
        changed = changed || std::any_of(t.fxSpectrum.begin(), t.fxSpectrum.end(), showing) ||
                  std::any_of(t.filterSpectrum.begin(), t.filterSpectrum.end(), showing);
    telemetry_ = t;
    traceRandomLfos();
    if (flashTicks_ > 0 && --flashTicks_ == 0)
        changed = true;
    if (edgeScroll())
        changed = true;
    if (glideSlots())
        changed = true;
    if (themeIndex(controller_.theme()) != theme_ || shownGlow() != glow_) // picked in another editor
        changed = true;
    if (typing_ && ++caretTicks_ % 32 == 0)
        changed = true; // the caret blinks
    return changed;
}

// --- Geometry.

Point View::nodeCenter(int node) const {
    const UiLayout &layout = controller_.layout();
    if (isBoxNode(node))
        return {kGraph.x + layout.boxX[node - kOps] * kGraph.w, kGraph.y + layout.boxY[node - kOps] * kGraph.h};
    return {kGraph.x + layout.nodeX[node] * kGraph.w, kGraph.y + layout.nodeY[node] * kGraph.h};
}

int View::nodeAt(Point p) const {
    for (int node = kNodes - 1; node >= 0; --node) {
        if (!nodeOn(node))
            continue;
        const Point c = nodeCenter(node);
        const bool inside = isBoxNode(node)
                                ? std::abs(p.x - c.x) <= kFilterHalfW + 4.0f && std::abs(p.y - c.y) <= kFilterHalfH + 4.0f
                                : std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius + 4.0f;
        if (inside)
            return node;
    }
    return -1;
}

Point View::knobCenter(const Widget &w) {
    return w.compact ? Point{w.rect.x + 18.0f, w.rect.y + 20.0f} : Point{w.rect.center().x, w.rect.y + w.rect.w * 0.5f};
}

float View::knobRadius(const Widget &w) {
    return w.compact ? 17.0f : w.rect.w * 0.5f;
}

// A knob's number shows under it, or beside it in the header.
bool View::onValue(const Widget &w, Point p) {
    if (w.kind == Kind::ValueText)
        return true;
    if (w.kind != Kind::Knob)
        return false;
    return w.compact ? p.x >= w.rect.x + 38.0f : p.y >= w.rect.y + w.rect.w;
}

Rect View::envRect(int env) const {
    // Mod envelopes are only drawn while a card shows them; otherwise any card's display will do.
    const float card = env < kOps ? kOffScreen : cardOf(env - kOps);
    return env < kOps ? kOpEnvRect : sourceDisplay(card == kOffScreen ? 0.0f : card);
}

float View::envScale(int env) const {
    const float fit = envFitScale(envRect(env), envIndices(env), [this](int index) { return value(index); });
    // While one of its handles is dragged, the zoom holds still, so the point stays under the mouse, until the
    // envelope would run past the display's end: then it zooms out to keep all of it in.
    if ((drag_.kind == DragKind::EnvPoint || drag_.kind == DragKind::EnvCurve) && drag_.widget.a == env)
        return std::min(drag_.envScale, fit);
    return fit;
}

std::vector<View::Widget> View::widgets() const {
    std::vector<Widget> list;
    list.reserve(240);
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    auto knob = [&](int param, Point center, float diameter, const char *label) {
        add(Kind::Knob, knobRect(center, diameter), param);
        list.back().label = label;
    };
    // A drawn shaper curve is edited right in its display, as in the big editor its EDIT button opens.
    auto drawnCurve = [&](int unit, Rect display) {
        const int shape = kMaxSources + unit;
        add(Kind::ShapeArea, display, -1, shape);
        const int count = shapePointCount(shape);
        for (int point = 0; point + 1 < count; ++point) {
            if (shapePointX(shape, point + 1) - shapePointX(shape, point) < 1e-6)
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(shape, point);
            add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(shape, point, PointField::Curve),
                shape, point);
            list.back().compact = true;
        }
        for (int point = 0; point < count; ++point) {
            const Point c = shapePointPos(shape, point);
            add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, shape, point);
            list.back().compact = true;
        }
        // EDIT in the corner a curve leaves empty
        add(Kind::ClipDisplay, {display.right() - 40.0f, display.bottom() - 20.0f, 34.0f, 15.0f}, -1, unit);
    };

    // Header.
    auto headerKnob = [&](int param, float x, float width, const char *label) {
        add(Kind::Knob, {x, 12.0f, width, 40.0f}, param);
        list.back().label = label;
        list.back().compact = true;
    };
    const int globals[] = {paramIndex(GlobalParam::Volume), paramIndex(GlobalParam::FmAmount),
                           paramIndex(GlobalParam::Pitch), paramIndex(GlobalParam::Pan)};
    const Str globalLabels[] = {Str::LabelVolume, Str::LabelFmAmount, Str::LabelPitch, Str::LabelPan};
    for (int i = 0; i < 4; ++i)
        headerKnob(globals[i], kHeaderKnobX + i * kHeaderKnobW, kHeaderKnobW, tr(globalLabels[i]));
    // Play cluster: Poly | Mono over Legato, then Glide with its Always switch.
    const int mono = paramIndex(GlobalParam::Mono);
    add(Kind::Choice, {kPlayX, 12.0f, 44.0f, 19.0f}, mono, -1, 0);
    add(Kind::Choice, {kPlayX + 44.0f, 12.0f, 44.0f, 19.0f}, mono, -1, 1);
    add(Kind::Toggle, {kPlayX, 35.0f, 88.0f, 19.0f}, paramIndex(GlobalParam::Legato));
    headerKnob(paramIndex(GlobalParam::Glide), kPlayX + 98.0f, kHeaderKnobW, tr(Str::LabelGlide));
    add(Kind::Toggle, {kPlayX + 206.0f, 23.0f, 56.0f, 19.0f}, paramIndex(GlobalParam::GlideAlways));
    headerKnob(paramIndex(GlobalParam::BendRange), 906.0f, 96.0f, tr(Str::LabelBend));
    headerKnob(paramIndex(GlobalParam::Voices), 1006.0f, kHeaderKnobW, tr(Str::LabelVoices));

    // Routing panel, as a graph, as a matrix, or the effects chain; or the shape
    // editor of a freeform LFO, in place of all three.
    const int editing = shapeEditing();
    for (int v = 0; v < 3; ++v)
        add(Kind::ViewTab, kViewTabs[v], -1, v);
    const RoutingView view = controller_.layout().view;
    // Switched-off nodes are left out of the graph, and greyed out in the matrix; their tabs switch them back on.
    const int partials = partialEditing();
    const std::vector<int> on = enabledNodes();
    if (editing >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::ShapeReset, shapeReset(), -1, editing);
        for (int axis = 0; axis < 2; ++axis)
            for (int part = 0; part < 3; ++part)
                add(Kind::ShapeGrid, shapeGridButton(axis, part), -1, axis, part - 1);
        add(Kind::ShapeArea, shapeArea().inset(-8.0f), -1, editing);
        if (isNoteCurve(editing)) { // the range across, in the title row, after the title, close together
            const char *const labels[] = {tr(Str::LabelLow), tr(Str::LabelHigh)};
            const int range[] = {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)};
            for (int i = 0; i < 2; ++i) {
                add(Kind::Knob, {kRouting.x + 148.0f + i * 96.0f, kRouting.y, 92.0f, 40.0f}, range[i]);
                list.back().label = labels[i];
                list.back().compact = true;
            }
        }
        const int count = shapePointCount(editing);
        for (int point = 0; point < count - (isCurve(editing) ? 1 : 0); ++point) { // a curve doesn't wrap
            const double next = point + 1 < count ? shapePointX(editing, point + 1) : 1.0;
            if (next - shapePointX(editing, point) < 1e-6)
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(editing, point);
            add(Kind::ShapeCurve, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, shapeIndex(editing, point, PointField::Curve),
                editing, point);
        }
        for (int point = 0; point < count; ++point) {
            const Point c = shapePointPos(editing, point);
            add(Kind::ShapePoint, {c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f}, -1, editing, point);
        }
    } else if (partials >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::PartialReset, shapeReset(), -1, partials);
        for (int choice = 0; choice < static_cast<int>(std::size(kPartialCounts)); ++choice)
            add(Kind::Choice, partialCountButton(choice), partialCountIndex(partials), -1, kPartialCounts[choice]);
        add(Kind::PartialArea, kPartialLevels, -1, partials, 0);
        add(Kind::PartialArea, kPartialPhases, -1, partials, 1);
    } else if (view == RoutingView::Fx) {
        const std::vector<int> chain = fxChainShown();
        add(Kind::FxMenuButton, fxMenuButton());
        if (scrollLength(ScrollArea::Fx) > kVisibleFx + 0.01f)
            add(Kind::Scrollbar, fxScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::Fx));
        const size_t cardsStart = list.size();
        for (size_t pos = 0; pos < chain.size(); ++pos) {
            const int slot = chain[pos];
            const FxType type = fxSlotType(slot);
            const float screen = fxScreen(chain, pos);
            if (!onScreen(screen, kVisibleFx, fxCardSpan(slot)))
                continue;
            const Rect card = fxCard(screen, fxCardSpan(slot));
            auto at = [slot](FxParam f) { return paramIndex(slot, f); };
            // The icon at the title's left switches the effect on and off, and drags the card like the title does.
            const Rect icon = fxIconRect(card).inset(-4.0f);
            if (fxFolded(slot)) { // a strip: the icon, the cross, then the rest unfolds it
                add(Kind::FxGrip, {card.x, card.y, card.w, 30.0f}, -1, slot, static_cast<int>(pos));
                add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
                add(Kind::FxRemove, {card.center().x - 9.0f, card.y + 34.0f, 18.0f, 21.0f}, -1, slot);
                add(Kind::FxFold, {card.x, card.y + 60.0f, card.w, card.h - 60.0f}, -1, slot);
                continue;
            }
            add(Kind::FxGrip, {card.x, card.y, card.w - 52.0f, 34.0f}, -1, slot, static_cast<int>(pos));
            add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
            add(Kind::FxFold, {card.right() - 26.0f - 22.0f, card.y + 9.0f, 18.0f, 19.0f}, -1, slot);
            add(Kind::FxRemove, {card.right() - 26.0f, card.y + 8.0f, 18.0f, 21.0f}, -1, slot);
            const float row = card.y + 44.0f; // mode buttons and switches
            auto knobs = [&](std::initializer_list<int> params, std::initializer_list<const char *> labels,
                             float y = 240.0f) {
                const float step = card.w / static_cast<float>(params.size());
                const float diameter = params.size() > 3 ? 32.0f : (y == 240.0f ? 36.0f : 30.0f);
                for (size_t i = 0; i < params.size(); ++i)
                    if (params.begin()[i] >= 0) // -1 leaves its place empty
                        knob(params.begin()[i], {card.x + step * (static_cast<float>(i) + 0.5f), card.y + y}, diameter,
                             labels.begin()[i]);
            };
            switch (type) {
            case FxType::Shaper: {
                add(Kind::ListButton, clipModeButton(card), at(FxParam::ShaperMode));
                add(Kind::Toggle, clipLinkButton(card), at(FxParam::ShaperLink));
                if (std::lround(value(at(FxParam::ShaperMode))) == static_cast<long>(ClipMode::Custom))
                    drawnCurve(slot, fxDisplay(screen, type));
                knobs({at(FxParam::ShaperDrive), at(FxParam::ShaperOutput), at(FxParam::ShaperMix)},
                      {tr(Str::LabelDrive), tr(Str::LabelOutput), tr(Str::LabelMix)});
                break;
            }
            case FxType::Delay: {
                // Sync, link and ping-pong, as icons, across the card.
                const FxParam switches[] = {FxParam::DelaySync, FxParam::DelayLink, FxParam::DelayPingPong};
                const float w = (card.w - 28.0f + 4.0f) / 3.0f;
                for (int i = 0; i < 3; ++i)
                    add(Kind::Toggle, {card.x + 14.0f + i * w, row, w - 4.0f, 22.0f}, at(switches[i]));
                // Synced, the time knobs pick note values and show them as their labels. Linked, the
                // left one sets both sides.
                const bool synced = value(at(FxParam::DelaySync)) >= 0.5;
                const int timeL = synced ? at(FxParam::DelayDivisionL) : at(FxParam::DelayTimeL);
                const int timeR = synced ? at(FxParam::DelayDivisionR) : at(FxParam::DelayTimeR);
                const bool linked = value(at(FxParam::DelayLink)) >= 0.5;
                knobs({timeL, linked ? -1 : timeR, at(FxParam::DelayFeedback)},
                      {synced ? nullptr : tr(linked ? Str::LabelTime : Str::LabelTimeL),
                       synced ? nullptr : tr(Str::LabelTimeR), tr(Str::LabelFeedback)},
                      206.0f);
                knobs({at(FxParam::DelayTone), at(FxParam::DelayWidth), at(FxParam::DelayMix)},
                      {tr(Str::LabelTone), tr(Str::LabelWidth), tr(Str::LabelMix)}, 266.0f);
                break;
            }
            case FxType::Eq:
                knobs({at(FxParam::EqLow), at(FxParam::EqMid), at(FxParam::EqHigh)},
                      {tr(Str::LabelLow), tr(Str::LabelMid), tr(Str::LabelHigh)});
                break;
            case FxType::ParamEq: {
                const Rect area = fxDisplay(screen, type).inset(8.0f);
                for (int band = 0; band < kPeqBands; ++band) {
                    const Point h = peqHandle(area, slot, band);
                    add(Kind::PeqPoint, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f}, peqQIndex(slot, band), slot, band);
                }
                // Each band's row: its shape, then frequency, gain and Q.
                for (int band = 0; band < kPeqBands; ++band) {
                    const float y = band == 0 ? 206.0f : 266.0f;
                    add(Kind::PeqTypeButton, peqTypeButton(card, y), peqTypeIndex(slot, band), slot, band);
                    const Str labels[kPeqBands][3] = {{Str::LabelFreq1, Str::LabelGain1, Str::LabelQ1},
                                                      {Str::LabelFreq2, Str::LabelGain2, Str::LabelQ2}};
                    knobs({-1, peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band)},
                          {nullptr, tr(labels[band][0]), tr(labels[band][1]), tr(labels[band][2])}, y);
                }
                break;
            }
            case FxType::Filter: {
                // The filter types in a row of icons, and the slope's list under the response.
                constexpr int kTypes = static_cast<int>(FilterType::Count);
                const float w = (card.w - 28.0f + 4.0f) / kTypes;
                for (int t = 0; t < kTypes; ++t)
                    add(Kind::Choice, {card.x + 14.0f + t * w, row, w - 4.0f, 20.0f}, at(FxParam::FilterType), -1, t);
                add(Kind::ListButton, fxSlopeButton(card, fxDisplay(screen, type)), at(FxParam::FilterSlope));
                knobs({at(FxParam::FilterCutoff), at(FxParam::FilterResonance), at(FxParam::FilterKeyTrack)},
                      {tr(Str::LabelCutoff), tr(Str::LabelReso), tr(Str::LabelKeyTrack)}, 206.0f);
                knobs({at(FxParam::FilterDrive), -1, at(FxParam::FilterMix)},
                      {tr(Str::LabelDrive), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            }
            case FxType::Compressor:
                knobs({at(FxParam::CompThreshold), at(FxParam::CompRatio), at(FxParam::CompKnee)},
                      {tr(Str::LabelThreshold), tr(Str::LabelRatio), tr(Str::LabelKnee)}, 206.0f);
                knobs({at(FxParam::CompAttack), at(FxParam::CompRelease), at(FxParam::CompMakeup),
                       at(FxParam::CompMix)},
                      {tr(Str::LabelAttack), tr(Str::LabelRelease), tr(Str::LabelMakeup), tr(Str::LabelMix)}, 266.0f);
                break;
            case FxType::MultiComp: {
                // Band buttons pick which band's knobs show, then LINK; the crossovers and timing are shared.
                // Linked, the knobs set the low band, which every band follows.
                const float w = (card.w - 28.0f + 6.0f) / (kMcBands + 1);
                for (int band = 0; band < kMcBands; ++band)
                    add(Kind::McBand, {card.x + 14.0f + band * w, row, w - 6.0f, 22.0f}, -1, slot, band);
                add(Kind::Toggle, {card.x + 14.0f + kMcBands * w, row, w - 6.0f, 22.0f}, at(FxParam::McLink));
                const int band = value(at(FxParam::McLink)) >= 0.5 ? 0 : fxBand_[slot];
                knobs({mcThresholdIndex(slot, band), mcRatioIndex(slot, band), mcGainIndex(slot, band)},
                      {tr(Str::LabelThreshold), tr(Str::LabelRatio), tr(Str::LabelGain)}, 160.0f);
                knobs({at(FxParam::McLowFreq), at(FxParam::McHighFreq), at(FxParam::McMix)},
                      {tr(Str::LabelLowX), tr(Str::LabelHighX), tr(Str::LabelMix)}, 216.0f);
                knobs({at(FxParam::McAttack), at(FxParam::McRelease), at(FxParam::McOutput)},
                      {tr(Str::LabelAttack), tr(Str::LabelRelease), tr(Str::LabelOutput)}, 268.0f);
                break;
            }
            case FxType::Reverb:
                add(Kind::ListButton, reverbTypeButton(card), at(FxParam::ReverbType));
                if (reverbType(value(at(FxParam::ReverbType))) == ReverbType::Shimmer) {
                    // Its interval and how much of what goes round it shifts, among the others.
                    knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay),
                           at(FxParam::ReverbShimmerPitch)},
                          {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay), tr(Str::LabelPitch)},
                          206.0f);
                    knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbWidth), at(FxParam::ReverbShimmer),
                           at(FxParam::ReverbMix)},
                          {tr(Str::LabelDamp), tr(Str::LabelWidth), tr(Str::LabelShimmer), tr(Str::LabelMix)}, 266.0f);
                    break;
                }
                knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay)},
                      {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay)}, 206.0f);
                knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbWidth), at(FxParam::ReverbMix)},
                      {tr(Str::LabelDamp), tr(Str::LabelWidth), tr(Str::LabelMix)}, 266.0f);
                break;
            case FxType::Phaser:
            case FxType::Flanger: {
                // Under the display, SYNC (the delay's metronome) at the left, over the Rate knob, and STEREO in the
                // middle. Synced, the Rate knob picks a note value and shows it as its label.
                const bool phaser = type == FxType::Phaser;
                const int sync = at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync);
                const float under = fxDisplay(screen, type).bottom() + 4.0f;
                add(Kind::Toggle, {card.x + 14.0f, under, 34.0f, 18.0f}, sync);
                add(Kind::Toggle, {card.center().x - 36.0f, under, 72.0f, 18.0f},
                    at(phaser ? FxParam::PhaserStereo : FxParam::FlangerStereo));
                const bool synced = value(sync) >= 0.5;
                const int rate = synced ? at(phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision)
                                        : at(phaser ? FxParam::PhaserRate : FxParam::FlangerRate);
                if (phaser) {
                    knobs({rate, at(FxParam::PhaserDepth), at(FxParam::PhaserCenter)},
                          {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelCenter)}, 206.0f);
                    knobs({at(FxParam::PhaserFeedback), at(FxParam::PhaserStages), at(FxParam::PhaserMix)},
                          {tr(Str::LabelFeedback), tr(Str::LabelStages), tr(Str::LabelMix)}, 266.0f);
                    break;
                }
                knobs({rate, at(FxParam::FlangerDepth), at(FxParam::FlangerDelay)},
                      {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelDelay)}, 206.0f);
                knobs({at(FxParam::FlangerFeedback), -1, at(FxParam::FlangerMix)},
                      {tr(Str::LabelFeedback), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            }
            case FxType::FreqShifter:
                // The range, then the share of it the shift takes.
                knobs({at(FxParam::ShiftRange), at(FxParam::ShiftAmount), at(FxParam::ShiftSpread)},
                      {tr(Str::LabelRange), tr(Str::LabelShift), tr(Str::LabelSpread)}, 206.0f);
                knobs({at(FxParam::ShiftFeedback), -1, at(FxParam::ShiftMix)},
                      {tr(Str::LabelFeedback), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            case FxType::Disperser:
                knobs({at(FxParam::DisperserFreq), at(FxParam::DisperserStages), at(FxParam::DisperserPinch),
                       at(FxParam::DisperserMix)},
                      {tr(Str::LabelFreq), tr(Str::LabelStages), tr(Str::LabelPinch), tr(Str::LabelMix)});
                break;
            case FxType::Widener:
                knobs({at(FxParam::WidenerSize), at(FxParam::WidenerWidth), at(FxParam::WidenerHighPass),
                       at(FxParam::WidenerMix)},
                      {tr(Str::LabelSize), tr(Str::LabelWidth), tr(Str::LabelHighPass), tr(Str::LabelMix)});
                break;
            case FxType::None:
            case FxType::Count:
                break;
            }
        }
        if (const float place = fxOffset(chain, chain.size()) - fxScroll(); fxShadow() && onScreen(place, kVisibleFx))
            add(Kind::FxShadow, fxCard(place));
        // The arrows between the cards, while there is room for another effect: a click adds one there.
        for (size_t pos = 1; pos < chain.size() && fxShadow(); ++pos)
            if (const float place = fxScreen(chain, pos); place > 0.0f && place < kVisibleFx) {
                const Rect card = fxCard(place);
                add(Kind::FxInsert, {card.x - kFxGap * 0.5f - 10.0f, card.center().y - 14.0f, 20.0f, 28.0f}, -1,
                    static_cast<int>(pos));
            }
        for (size_t i = cardsStart; i < list.size(); ++i)
            list[i].clip = fxCardClip();
    } else if (view == RoutingView::Matrix) {
        // Rows feed columns; the last column is the output. The rows' and columns' names work like the
        // nodes' tabs.
        for (int row = 0; row < kNodes; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                const int param = gridParam(row, col);
                if (param >= 0)
                    add(Kind::GridCell, gridCell(row, col), param, row, col);
            }
        }
        for (int node = 0; node < kNodes; ++node) {
            const Rect row = gridCell(node, 0), col = gridCell(0, node);
            add(Kind::MatrixLabel, {row.x - 32.0f, row.y, 28.0f, row.h}, -1, node);
            add(Kind::MatrixLabel, {col.x - 2.0f, col.y - 22.0f, col.w + 4.0f, 18.0f}, -1, node);
        }
    } else {
        // Nodes, each with its edge, then depth labels and arrowheads on top of them.
        for (int node : on) {
            const Point c = nodeCenter(node);
            const Rect r = isBoxNode(node)
                               ? Rect{c.x - kFilterHalfW, c.y - kFilterHalfH, 2.0f * kFilterHalfW, 2.0f * kFilterHalfH}
                               : Rect{c.x - kNodeRadius, c.y - kNodeRadius, 2.0f * kNodeRadius, 2.0f * kNodeRadius};
            add(Kind::Node, r, -1, node);
            add(Kind::Port, r.inset(-kEdgeOut), -1, node);
        }
        const int moving = rerouting(); // follows the mouse, without its label
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (param < 0 || !wired(param) || param == moving)
                    continue;
                if (from == to) {
                    const Point loop = loopCenter(nodeCenter(from));
                    add(Kind::WireLabel, chip({loop.x + 32.0f, loop.y}, 44.0f), param, from, to);
                } else {
                    const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to));
                    add(Kind::WireLabel, chip(w.label), param, from, to);
                }
            }
        }
        for (int node : on)
            if (goesOut(node) && outParam(node) != moving)
                add(Kind::OutLabel, chip(outWireOf(node, nodeCenter(node)).label, 40.0f), outParam(node), node);
        // The arrowheads, over the labels: dragged onto another node or the bus, they move their routes there.
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (param < 0 || from == to || !wired(param) || param == moving)
                    continue;
                const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to));
                add(Kind::WireEnd, arrowGrip(w.end, w.tangent), param, from, to);
            }
            if (goesOut(from) && outParam(from) != moving)
                add(Kind::WireEnd, arrowGrip(outWireOf(from, nodeCenter(from)).end, {0.0f, 1.0f}), outParam(from), from,
                    kNodes);
        }
        // The nodes that are off, as dashed ghosts in the graph's top left, over any node moved there.
        float x = kRouting.x + 16.0f;
        for (int node = 0; node < kNodes; ++node) {
            if (nodeOn(node))
                continue;
            const float w = isBoxNode(node) ? 34.0f : 24.0f;
            add(Kind::NodeGhost, {x, kRouting.y + 10.0f, w, 24.0f}, -1, node);
            x += w + 6.0f;
        }
    }

    // Inspector: tabs, then the selected operator, filter or shaper. The FX view covers it.
    const size_t inspectorStart = list.size();
    const int node = controller_.layout().selectedNode;
    const float tabsX = kInspector.x + 16.0f;
    for (int i = 0; i < kNodes; ++i) // the operators', the filters' and the shapers', a little apart
        add(Kind::NodeTab,
            {tabsX + i * 25.0f + (isBoxNode(i) ? 6.0f : 0.0f) + (isShaperNode(i) ? 6.0f : 0.0f), kInspector.y + 9.0f,
             24.0f, 22.0f},
            -1, i);
    add(Kind::Toggle, {kInspector.right() - 16.0f - 26.0f, kInspector.y + 37.0f, 26.0f, 19.0f}, enabledParam(node));
    auto knobRow = [&](const int (&params)[5], const char *const (&labels)[5]) {
        for (int i = 0; i < 5; ++i)
            if (params[i] >= 0)
                knob(params[i], {kInspector.x + 34.0f + i * 58.0f, kInspector.y + 110.0f}, 34.0f, labels[i]);
    };
    auto opKnobRow = [&](const int (&params)[6], const char *const (&labels)[6]) {
        const float step = (kInspector.w - 32.0f) / 6.0f;
        for (int i = 0; i < 6; ++i)
            if (params[i] >= 0)
                knob(params[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 110.0f}, 32.0f, labels[i]);
    };
    const Str envLabels[] = {Str::LabelAttack, Str::LabelHold, Str::LabelDecay, Str::LabelSustain, Str::LabelRelease};
    // What feeds a filter or shaper, from the operators (and filter 1, or the shapers, into a filter) that are on.
    auto inputKnobs = [&](int into) {
        std::vector<int> sources;
        for (int from = 0; from < kNodes; ++from)
            if (from != into && nodeOn(from) && routeParam(from, into) >= 0)
                sources.push_back(from);
        const float spacing = (kInspector.w - 32.0f) / std::max<size_t>(sources.size(), 6);
        for (size_t i = 0; i < sources.size(); ++i)
            knob(routeParam(sources[i], into), {kInspector.x + 16.0f + spacing * (i + 0.5f), kInspector.y + 314.0f},
                 28.0f, kNodeNames[sources[i]]);
    };
    if (isShaperNode(node)) {
        // The curve's list and the link switch, then drive, output, mix and what goes straight out; a drawn curve
        // is edited in the display unless its big editor has it.
        const int shaper = shaperOfNode(node);
        add(Kind::ListButton, kShaperModeButton, paramIndex(shaper, ShaperParam::Mode));
        add(Kind::Toggle, kShaperLinkButton, paramIndex(shaper, ShaperParam::Link));
        const int params[] = {paramIndex(shaper, ShaperParam::Drive), paramIndex(shaper, ShaperParam::Output),
                              paramIndex(shaper, ShaperParam::Mix), paramIndex(shaper, ShaperParam::Out)};
        const Str labels[] = {Str::LabelDrive, Str::LabelOutput, Str::LabelMix, Str::LabelToOut};
        const float step = (kInspector.w - 32.0f) / 4.0f;
        for (int i = 0; i < 4; ++i)
            knob(params[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 110.0f}, 34.0f, tr(labels[i]));
        if (clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom &&
            shapeEditing() != shaperCurve(shaper))
            drawnCurve(kFxSlots + shaper, kShaperDisplay);
        inputKnobs(node);
    } else if (node < kOps) {
        const int op = node;
        for (int b = 0; b + 1 < kWaveButtons; ++b)
            add(Kind::WaveButton, waveButton(b), -1, op, static_cast<int>(kCommonWaves[b]));
        add(Kind::WaveMore, waveButton(kWaveButtons - 1), -1, op);
        if (isNoise(op)) {
            // Its slope and stereo spread take the place of the pitch knobs; the unison row shows the spectrum.
            const int params[] = {paramIndex(op, OpParam::NoiseSlope), paramIndex(op, OpParam::UnisonSpread), -1, -1,
                                  paramIndex(op, OpParam::Velocity), paramIndex(op, OpParam::Out)};
            const char *const labels[] = {tr(Str::LabelSlope), tr(Str::LabelSpread),   nullptr,
                                          nullptr,             tr(Str::LabelVelocity), tr(Str::LabelOut)};
            opKnobRow(params, labels);
        } else {
            const int params[] = {paramIndex(op, OpParam::Coarse), paramIndex(op, OpParam::Transpose),
                                  paramIndex(op, OpParam::Fine),   paramIndex(op, OpParam::Detune),
                                  paramIndex(op, OpParam::Velocity), paramIndex(op, OpParam::Out)};
            const char *const labels[] = {tr(Str::LabelRatio),  tr(Str::LabelPitch),    tr(Str::LabelFine),
                                          tr(Str::LabelDetune), tr(Str::LabelVelocity), tr(Str::LabelOut)};
            opKnobRow(params, labels);
            const int unison[] = {paramIndex(op, OpParam::Unison), paramIndex(op, OpParam::UnisonDetune),
                                  paramIndex(op, OpParam::UnisonSpread), paramIndex(op, OpParam::PhaseRandom)};
            const Str unisonLabels[] = {Str::LabelUnison, Str::LabelUniDetune, Str::LabelUniSpread, Str::LabelRndPhase};
            for (int i = 0; i < 4; ++i)
                knob(unison[i], {kInspector.x + 30.0f + i * 56.0f, kInspector.y + 168.0f}, 34.0f, tr(unisonLabels[i]));
        }
        const EnvIndices e = envIndices(op);
        const int ahdsr[] = {e.attack, e.hold, e.decay, e.sustain, e.release};
        const float step = (kInspector.w - 32.0f) / 5.0f;
        for (int i = 0; i < 5; ++i)
            knob(ahdsr[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 322.0f}, 30.0f, tr(envLabels[i]));
    } else {
        const int filter = node - kOps;
        for (int t = 0; t < static_cast<int>(FilterType::Count); ++t)
            add(Kind::TypeButton, {kInspector.x + 16.0f + t * 36.0f, kInspector.y + 62.0f, 32.0f, 22.0f}, -1, filter,
                t);
        add(Kind::ListButton, kFilterSlopeButton, paramIndex(filter, FilterParam::Slope));
        const int params[] = {paramIndex(filter, FilterParam::Cutoff), paramIndex(filter, FilterParam::Resonance),
                              paramIndex(filter, FilterParam::KeyTrack), paramIndex(filter, FilterParam::Drive),
                              paramIndex(filter, FilterParam::Out)};
        const char *const labels[] = {tr(Str::LabelCutoff), tr(Str::LabelResonance), tr(Str::LabelKeyTrack),
                                      tr(Str::LabelDrive), tr(Str::LabelOut)};
        knobRow(params, labels);
        const Point handle = filterHandle(kFilterDisplay, value(params[0]), value(params[1]));
        add(Kind::FilterPoint, {handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f}, -1, filter);
        inputKnobs(node);
    }

    const bool wide = inspectorCovered();
    if (wide)
        list.resize(inspectorStart);

    // Envelope handles, for the selected operator and the mod envelopes on screen.
    std::vector<int> envs;
    // Cards partly scrolled out are there too, cut off at the row's ends: (position, screen place).
    const std::vector<int> cardSources = sourcesShown();
    const bool collapsed = controller_.layout().sourcesCollapsed;
    std::vector<std::pair<int, float>> cards;
    for (int position = 0; position < static_cast<int>(cardSources.size()) && !collapsed; ++position)
        if (const float place = sourceOffset(cardSources, position) - sourceScroll();
            onScreen(place, kVisibleCards, sourceSpan(cardSources[position])))
            cards.push_back({position, place});
    for (const auto &[position, card] : cards) // a folded card shows no envelope
        if (sourceTypeOf(cardSources[position]) == SourceType::Env && !sourceFolded(cardSources[position]))
            envs.push_back(kOps + cardSources[position]);
    if (node < kOps && !wide)
        envs.push_back(node);
    for (int env : envs) {
        const EnvIndices e = envIndices(env);
        const EnvShape s = envShape(envRect(env), e, [this](int index) { return value(index); }, envScale(env));
        // Curve handles halfway along each segment, under the points so those win where they overlap.
        for (int b = 0; b < 3; ++b) {
            const Point c = envSegmentPoint(s, b, 0.5, value(e.curve(b)), value(e.sustain));
            add(Kind::EnvCurve, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, e.curve(b), env, b);
        }
        const Point points[] = {s.p1, s.ph, s.p2, s.p4};
        for (int b = 0; b < 4; ++b)
            add(Kind::EnvPoint, {points[b].x - 7.0f, points[b].y - 7.0f, 14.0f, 14.0f}, -1, env, b);
        if (env >= kOps)
            for (int k = 0; k < 7; ++k)
                list[list.size() - 1 - k].clip = sourceCardClip();
    }

    // Modulation panel: the collapse toggle, the add buttons, the scrollbar, then the cards.
    add(Kind::SourcesToggle, sourcesToggle());
    if (cardSources.size() < static_cast<size_t>(kMaxSources))
        for (SourceType type : {SourceType::Lfo, SourceType::Env})
            add(Kind::SourceAdd, sourceAddButton(type), -1, static_cast<int>(type));
    if (scrollLength(ScrollArea::Sources) > kVisibleCards + 0.01f && !collapsed)
        add(Kind::Scrollbar, sourceScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::Sources));
    const size_t sourceCardsStart = list.size();
    for (const auto &[position, card] : cards) {
        const int s = cardSources[position];
        if (sourceFolded(s)) { // a strip: the handle, a grip, the switch and cross, then the rest unfolds it
            const Rect p = sourceCard(card, kFoldedSource);
            add(Kind::SourceHandle, {p.center().x - 10.0f, p.y + 7.0f, 20.0f, 20.0f}, -1, s);
            add(Kind::SourceGrip, {p.x, p.y + 30.0f, p.w, 16.0f}, -1, s, position);
            add(Kind::Toggle, {p.center().x - 13.0f, p.y + 50.0f, 26.0f, 19.0f}, sourceEnabledIndex(s));
            add(Kind::SourceRemove, {p.center().x - 9.0f, p.y + 73.0f, 18.0f, 21.0f}, -1, s);
            add(Kind::SourceFold, {p.x, p.y + 98.0f, p.w, p.h - 98.0f}, -1, s);
            continue;
        }
        const Rect p = sourceCard(card);
        const bool lfo = sourceTypeOf(s) == SourceType::Lfo;
        const float fold = sourcePower(card).x - 4.0f - 18.0f;
        add(Kind::SourceGrip, {p.x, p.y, fold - 4.0f - p.x, 34.0f}, -1, s, position);
        add(Kind::SourceFold, {fold, p.y + 8.0f, 18.0f, 19.0f}, -1, s);
        add(Kind::SourceHandle, {p.x + 26.0f, p.y + 7.0f, 20.0f, 20.0f}, -1, s);
        add(Kind::Toggle, sourcePower(card), sourceEnabledIndex(s));
        add(Kind::SourceRemove, sourceRemove(card), -1, s);
        if (lfo) {
            for (int shape = 0; shape < static_cast<int>(LfoShape::Count); ++shape)
                add(Kind::ShapeButton, {p.x + 16.0f + shape * 31.0f, p.y + 40.0f, 28.0f, 26.0f}, -1, s, shape);
            const bool random =
                std::lround(value(paramIndex(s, SourceParam::Shape))) == static_cast<long>(LfoShape::Random);
            add(Kind::LfoDisplay, sourceDisplay(card, random), -1, s);
            if (random) {
                knob(paramIndex(s, SourceParam::RandomSmooth), randomKnob(card, 0), 30.0f, tr(Str::LabelSmooth));
                knob(paramIndex(s, SourceParam::RandomChaos), randomKnob(card, 1), 30.0f, tr(Str::LabelChaos));
            }
            // Synced, the knob picks a note value instead of a rate and shows it as its label.
            add(Kind::Toggle, sourceSync(card), paramIndex(s, SourceParam::Sync));
            // Free, one-shot and unipolar, as icons, each under two of the shapes, in a row with the sync switch.
            const SourceParam modes[] = {SourceParam::Free, SourceParam::OneShot, SourceParam::Unipolar};
            for (int i = 0; i < 3; ++i)
                add(Kind::Toggle, {p.x + 16.0f + i * 62.0f, p.y + 79.0f, 59.0f, 16.0f}, paramIndex(s, modes[i]));
            const bool synced = value(paramIndex(s, SourceParam::Sync)) >= 0.5;
            knob(synced ? paramIndex(s, SourceParam::Division) : paramIndex(s, SourceParam::Rate),
                 {p.right() - 44.0f, p.y + 47.0f}, 30.0f, synced ? nullptr : tr(Str::LabelRate));
        } else {
            const EnvIndices e = envIndices(kOps + s);
            const int params[] = {e.attack, e.hold, e.decay, e.sustain, e.release};
            const float step = (p.w - 24.0f) / 5.0f;
            for (int i = 0; i < 5; ++i)
                knob(params[i], {p.x + 12.0f + step * (i + 0.5f), p.y + 54.0f}, 32.0f, tr(envLabels[i]));
        }
        // The rows on screen, at least in part; cut off at the list's ends (their clip is set below).
        const std::vector<int> slots = usedSlots(s);
        const float first = slotFirst(s);
        const int top = static_cast<int>(std::floor(first));
        const size_t rowsStart = list.size();
        for (int row = top; row < std::min(top + kVisibleSlots + 1, static_cast<int>(slots.size())); ++row) {
            const Rect r = slotRow(card, static_cast<float>(row) - first);
            add(Kind::SlotBypass, {r.x + 1.0f, r.y, 16.0f, r.h}, slotBypassIndex(s, slots[row]), s, slots[row]);
            add(Kind::SlotBar, {r.x + 116.0f, r.y, 76.0f, r.h}, slotAmountIndex(s, slots[row]), s, slots[row]);
            add(Kind::ValueText, {r.x + 194.0f, r.y, 38.0f, r.h}, slotAmountIndex(s, slots[row]), s, slots[row]);
            add(Kind::SlotRemove, {r.right() - 20.0f, r.y, 20.0f, r.h}, -1, s, slots[row]);
        }
        for (size_t i = rowsStart; i < list.size(); ++i)
            list[i].clip = slotClip(card, sourceCardClip());
        if (slots.size() > static_cast<size_t>(kVisibleSlots)) {
            const Rect track = slotScrollTrack(card);
            add(Kind::SlotScroll, {track.x - 3.0f, track.y - 2.0f, track.w + 6.0f, track.h + 4.0f}, -1, s);
        }
    }
    if (const float place = sourceOffset(cardSources, cardSources.size()) - sourceScroll();
        sourceShadow() && !collapsed && onScreen(place, kVisibleCards, kSourceShadowSpan)) {
        const Rect p = sourceCard(place, kSourceShadowSpan);
        add(Kind::SourceShadow, {p.x, p.y, p.w, kSourceShadowW}, -1, static_cast<int>(SourceType::Lfo));
        add(Kind::SourceShadow, {p.x, p.bottom() - kSourceShadowW, p.w, kSourceShadowW}, -1,
            static_cast<int>(SourceType::Env));
    }
    for (size_t i = sourceCardsStart; i < list.size(); ++i)
        if (list[i].clip.w <= 0.0f) // the slot rows have theirs
            list[i].clip = sourceCardClip();

    // Depth handles in the gap under each modulated knob, and small ones in the top right corner of each
    // modulated wire label, matrix cell and source's depth bar (another source modulating its depth).
    const std::vector<Assignment> mods = assignments();
    const size_t count = list.size();
    for (size_t i = 0; i < count; ++i) {
        const Kind kind = list[i].kind;
        if (kind != Kind::Knob && kind != Kind::WireLabel && kind != Kind::OutLabel && kind != Kind::GridCell &&
            kind != Kind::SlotBar)
            continue;
        std::vector<const Assignment *> here;
        for (const Assignment &m : mods)
            if (m.target == list[i].param)
                here.push_back(&m);
        const Rect r = list[i].rect;
        if (kind != Kind::Knob) {
            const bool cell = kind == Kind::GridCell || kind == Kind::SlotBar;
            for (size_t k = 0; k < here.size(); ++k) {
                const Point c{r.right() - (cell ? 4.0f : 3.0f) - k * (cell ? 6.0f : 7.0f), r.y + (cell ? 4.0f : 2.0f)};
                add(Kind::ModDot, {c.x - 3.5f, c.y - 3.5f, 7.0f, 7.0f},
                    slotAmountIndex(here[k]->source, here[k]->slot), here[k]->source, here[k]->slot);
                list.back().compact = true;
                list.back().clip = list[i].clip; // a depth bar's is cut off with its card
            }
            continue;
        }
        const Point base = list[i].compact ? Point{r.x + 18.0f, r.y + 37.0f} : Point{r.center().x, r.y + r.w - 1.0f};
        for (size_t k = 0; k < here.size(); ++k) {
            const float x = base.x + (k - (here.size() - 1) * 0.5f) * 9.0f;
            add(Kind::ModDot, {x - 5.0f, base.y - 5.0f, 10.0f, 10.0f}, slotAmountIndex(here[k]->source, here[k]->slot),
                here[k]->source, here[k]->slot);
            list.back().clip = list[i].clip;
        }
    }

    // The macros: their knobs, the names under them and their handles.
    const float macros = keyboardTop(controller_.layout().sourcesCollapsed);
    for (int macro = 0; macro < kMacros; ++macro) {
        const Point center = macroKnob(macros, macro);
        std::snprintf(macroLabels_[macro].data(), macroLabels_[macro].size(), "%s",
                      macroName(controller_.layout(), macro).c_str());
        knob(paramIndex(macro, MacroParam::Value), center, 34.0f, macroLabels_[macro].data());
        add(Kind::MacroName, {center.x - 50.0f, center.y + 18.0f, 100.0f, 17.0f}, -1, macro);
        add(Kind::MacroHandle, {center.x - 52.0f, center.y - 10.0f, 20.0f, 20.0f}, -1, kMaxSources + macro);
    }
    // The note source's handle, left of them, works the same way; its display opens its curve.
    add(Kind::MacroHandle, noteHandle(macros), -1, kNoteSource);
    add(Kind::NoteDisplay, noteDisplay(macros));

    // Keyboard strip. The white keys come first, so the black keys on top win the hits.
    const float keys = keyboardTop(controller_.layout().sourcesCollapsed);
    add(Kind::BendWheel, bendWheel(keys));
    add(Kind::ModWheel, modWheel(keys), paramIndex(GlobalParam::ModWheel));
    add(Kind::MacroHandle, wheelHandle(keys), -1, kWheelSource);
    for (int key = 0; key < kKeyCount; ++key) {
        if (isBlackKey(key))
            continue;
        const float x = kKeys.x + (key / 12 * 7 + kWhiteSlot[key % 12]) * kWhiteW;
        add(Kind::Key, {x, keys + kKeys.y, kWhiteW, kKeys.h}, -1, key);
    }
    for (int key = 0; key < kKeyCount; ++key) {
        if (!isBlackKey(key))
            continue;
        const float x = kKeys.x + (key / 12 * 7 + kBlackAfter[key % 12] + 1) * kWhiteW - kBlackW * 0.5f;
        add(Kind::Key, {x, keys + kKeys.y, kBlackW, kBlackH}, -1, key);
    }
    for (int redo = 0; redo < 2; ++redo)
        add(Kind::UndoButton, undoButton(redo), -1, redo);
    add(Kind::PresetStep, presetStep(-1), -1, -1);
    add(Kind::PresetNameBox, kPresetName);
    add(Kind::PresetStep, presetStep(1), -1, 1);
    for (int action = 0; action < 3; ++action)
        add(Kind::PresetButton, toolbarButton(action), -1, action);
    add(Kind::SettingsButton, toolbarButton(3));
    add(Kind::Octave, octaveButton(-1, keys), -1, -1);
    add(Kind::Octave, octaveButton(1, keys), -1, 1);

    if (fxMenuOpen_ && fxWide()) {
        add(Kind::FxMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::FxMenu, fxMenu(fxMenuAt_), -1, 1);
        for (int t = 1; t <= kFxTypes; ++t)
            add(Kind::FxAdd, fxMenuItem(fxMenuAt_, static_cast<FxType>(t)), -1, t);
    }
    // The open list of an effect's choice (the shaper's curve, the reverb's algorithm), under its button,
    // while that card is on screen and still holds that effect.
    const FxParam listField = fxFieldOf(listMenu_);
    const int listSlot = fxSlotOf(listMenu_);
    if (listSlot >= 0 && fxWide() && fxTypeOfField(listField) == fxSlotType(listSlot) && !fxFolded(listSlot)) {
        const std::vector<int> chain = fxChainShown();
        const auto at = std::find(chain.begin(), chain.end(), listSlot);
        const float screen = fxScreen(chain, static_cast<size_t>(at - chain.begin()));
        if (at != chain.end() && onScreen(screen, kVisibleFx)) {
            const Rect card = fxCard(screen, fxCardSpan(listSlot));
            const Rect button = listField == FxParam::ReverbType    ? reverbTypeButton(card)
                                : listField == FxParam::FilterSlope ? fxSlopeButton(card, fxDisplay(screen, FxType::Filter))
                                                                    : clipModeButton(card);
            const int items = listCount(listMenu_);
            add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::ListMenu, listMenu(button, items), -1, 1);
            for (int item = 0; item < items; ++item)
                add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
        }
    }
    // A shaper's list of curves or a filter's slopes, under its button on its page.
    const int pageNode = controller_.layout().selectedNode;
    const bool shaperList = shaperOf(listMenu_) >= 0 && pageNode == kOps + kFilters + shaperOf(listMenu_);
    const bool slopeList =
        listMenu_ >= 0 && isFilterNode(pageNode) && listMenu_ == paramIndex(pageNode - kOps, FilterParam::Slope);
    if ((shaperList || slopeList) && !inspectorCovered()) {
        const Rect button = slopeList ? kFilterSlopeButton : kShaperModeButton;
        const int items = listCount(listMenu_);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::ListMenu, listMenu(button, items), -1, 1);
        for (int item = 0; item < items; ++item)
            add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
    }
    // The open list of a parametric EQ band's shapes, above its button, while that card is on screen.
    if (peqMenu_ >= 0 && fxWide() && fxSlotType(fxSlotOf(peqMenu_)) == FxType::ParamEq) {
        const int slot = fxSlotOf(peqMenu_);
        const int band = peqMenu_ - peqTypeIndex(slot, 0);
        const std::vector<int> chain = fxChainShown();
        const auto at = std::find(chain.begin(), chain.end(), slot);
        const float screen = fxScreen(chain, static_cast<size_t>(at - chain.begin()));
        if (at != chain.end() && !fxFolded(slot) && onScreen(screen, kVisibleFx, fxSpan(FxType::ParamEq))) {
            const Rect button = peqTypeButton(fxCard(screen, fxSpan(FxType::ParamEq)), band == 0 ? 206.0f : 266.0f);
            add(Kind::PeqTypeMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::PeqTypeMenu, peqMenu(button), -1, 1);
            for (int t = 0; t < kPeqTypes; ++t)
                add(Kind::PeqTypeItem, peqMenuItem(button, t), peqMenu_, -1, t);
        }
    }
    // The open list of more waves, under "...", while its operator shows in the inspector.
    if (waveMenu_ >= 0 && waveMenu_ == controller_.layout().selectedNode && !inspectorCovered()) {
        add(Kind::WaveMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::WaveMenu, waveMenu(), -1, 1);
        for (size_t i = 0; i < std::size(kMoreWaves); ++i)
            add(Kind::WaveMenuItem, waveMenuItem(static_cast<int>(i)), -1, waveMenu_, static_cast<int>(kMoreWaves[i]));
    }
    return list;
}

// The settings dialog, centered over the editor.
namespace {
constexpr float kSettingsW = 380.0f;
constexpr float kSettingsH = 364.0f;
constexpr int kOversamplingFactors[] = {1, 2, 4, 8};
Rect settingsPanel(float viewHeight) {
    return {(View::kWidth - kSettingsW) * 0.5f, std::round((viewHeight - kSettingsH) * 0.5f), kSettingsW, kSettingsH};
}
Rect settingsOption(Rect panel, int i) {
    return {panel.x + 24.0f + i * 64.0f, panel.y + 82.0f, 58.0f, 28.0f};
}
Rect settingsTheme(Rect panel, int i) {
    const float w = (panel.w - 48.0f + 6.0f) / kThemeCount;
    return {panel.x + 24.0f + i * w, panel.y + 214.0f, w - 6.0f, 30.0f};
}
// The glow slider's track, with room above and below to grab it; GLOW before it, its amount after.
Rect settingsGlow(Rect panel) {
    return {panel.x + 76.0f, panel.y + 256.0f, 200.0f, 26.0f};
}
Rect settingsClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}
} // namespace

std::vector<View::Widget> View::settingsWidgets() const {
    const Rect panel = settingsPanel(height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    add(Kind::SettingsBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::SettingsBackdrop, panel, 1);
    for (int i = 0; i < static_cast<int>(std::size(kOversamplingFactors)); ++i)
        add(Kind::SettingsOption, settingsOption(panel, i), -1, kOversamplingFactors[i]);
    for (int i = 0; i < kThemeCount; ++i)
        add(Kind::SettingsTheme, settingsTheme(panel, i), -1, i);
    add(Kind::SettingsGlow, settingsGlow(panel));
    add(Kind::SettingsClose, settingsClose(panel));
    return list;
}

void View::paintSettings(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = settingsPanel(height());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 200.0f, 24.0f}, tr(Str::SettingsTitle), 14.0f, kText, Align::Left, true);
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 60.0f, 200.0f, 16.0f}, tr(Str::SettingsOversampling), 10.5f, kFaint, Align::Left,
           true);
    c.text({panel.x + 24.0f, panel.y + 122.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhat), 10.0f, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 140.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhy), 10.0f, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 158.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingSaved), 10.5f, kFaint, Align::Left);
    c.line({panel.x + 24.0f, panel.y + 184.5f}, {panel.right() - 24.0f, panel.y + 184.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 192.0f, 200.0f, 16.0f}, tr(Str::SettingsLook), 10.5f, kFaint, Align::Left, true);
    const Rect glow = settingsGlow(panel);
    c.text({panel.x + 24.0f, glow.y, 48.0f, glow.h}, tr(Str::SettingsGlow), 10.5f, kDim, Align::Left, true);
    const float amount = shownGlow();
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f %%", amount * 100.0f);
    c.text({glow.right() + 14.0f, glow.y, panel.right() - 24.0f - glow.right() - 14.0f, glow.h},
           amount > 0.0f ? text : tr(Str::ValueOff), 10.5f, amount > 0.0f ? kText : kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 294.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsLookSaved), 10.5f, kFaint, Align::Left);
    // The version and the license, at the bottom.
    c.line({panel.x + 24.0f, panel.y + 320.5f}, {panel.right() - 24.0f, panel.y + 320.5f}, kBorder, 1.0f);
    const Rect about{panel.x + 24.0f, panel.y + 328.0f, panel.w - 48.0f, 16.0f};
    c.text(about, trf(Str::SettingsVersion, {FMVIBE_VERSION}), 10.5f, kFaint, Align::Left, true);
    c.text(about, tr(Str::SettingsLicense), 10.5f, kDim, Align::Right);
    for (const Widget &w : settingsWidgets())
        paintWidget(c, w, {});
}

// The dialog asking before Init or Load replaces a changed patch, centered like the settings; a line shorter when
// it doesn't say what replaces the patch.
namespace {
constexpr float kConfirmW = 360.0f;
constexpr float kConfirmH = 150.0f;
constexpr float kConfirmLine = 18.0f;
Rect confirmPanel(float viewHeight, bool shorter) {
    const float h = kConfirmH - (shorter ? kConfirmLine : 0.0f);
    return {(View::kWidth - kConfirmW) * 0.5f, std::round((viewHeight - h) * 0.5f), kConfirmW, h};
}
// 1 goes ahead, 0 cancels.
Rect confirmButton(Rect panel, int ok) {
    return {panel.right() - 24.0f - (ok ? 80.0f : 166.0f), panel.bottom() - 24.0f - 28.0f, 80.0f, 28.0f};
}
} // namespace

void View::runPreset(PresetAction action) {
    switch (action) {
    case PresetAction::Init:
        controller_.resetPreset();
        break;
    case PresetAction::Load: // a preset from the library (which closes the browser), or a file from the dialog
        if (pendingPreset_ >= 0) {
            if (controller_.loadPresetAt(pendingPreset_))
                browserOpen_ = false;
            else
                flash(tr(Str::MsgPresetUnreadable));
        } else {
            controller_.choosePreset(false);
        }
        break;
    case PresetAction::Save:
        controller_.choosePreset(true);
        break;
    }
}

// What replaces the patch: Init, or the preset picked from the library. A file still to be picked in the dialog goes
// unsaid.
std::string View::confirmWhat() const {
    if (confirm_ == static_cast<int>(PresetAction::Init))
        return tr(Str::ConfirmInit);
    const std::vector<PresetEntry> &presets = controller_.presets();
    return pendingPreset_ >= 0 && pendingPreset_ < static_cast<int>(presets.size())
               ? trf(Str::ConfirmLoadPreset, {presets[pendingPreset_].name})
               : std::string();
}

std::vector<View::Widget> View::confirmWidgets() const {
    const Rect panel = confirmPanel(height(), confirmWhat().empty());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::ConfirmBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::ConfirmBackdrop, panel, 1);
    add(Kind::ConfirmButton, confirmButton(panel, 0), 0);
    add(Kind::ConfirmButton, confirmButton(panel, 1), 1);
    return list;
}

// The preset browser, centered over the editor: a title row, the list, and the folder at the foot.
namespace {
constexpr float kBrowserW = 440.0f;
constexpr float kBrowserRowH = 24.0f;
Rect browserPanel(float viewHeight) {
    const float h = std::min(560.0f, viewHeight - 120.0f);
    return {(View::kWidth - kBrowserW) * 0.5f, std::round((viewHeight - h) * 0.5f), kBrowserW, h};
}
Rect browserList(Rect panel) {
    return {panel.x + 12.0f, panel.y + 58.0f, panel.w - 24.0f, panel.h - 58.0f - 54.0f};
}
int browserVisibleRows(Rect panel) {
    return static_cast<int>(browserList(panel).h / kBrowserRowH);
}
Rect browserClose(Rect panel) {
    return {panel.right() - 38.0f, panel.y + 14.0f, 24.0f, 24.0f};
}
Rect browserFolder(Rect panel) {
    return {panel.right() - 24.0f - 110.0f, panel.bottom() - 40.0f, 110.0f, 26.0f};
}
} // namespace

std::vector<std::pair<int, std::string>> View::browserRows() const {
    std::vector<std::pair<int, std::string>> rows;
    const std::vector<PresetEntry> &presets = controller_.presets();
    for (size_t i = 0; i < presets.size(); ++i) {
        if (!presets[i].category.empty() && (i == 0 || presets[i].category != presets[i - 1].category))
            rows.push_back({-1, presets[i].category});
        rows.push_back({static_cast<int>(i), presets[i].name});
    }
    return rows;
}

std::vector<View::Widget> View::browserWidgets() const {
    const Rect panel = browserPanel(height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::BrowserBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::BrowserBackdrop, panel, 1);
    const std::vector<std::pair<int, std::string>> rows = browserRows();
    const Rect area = browserList(panel);
    const int shown = browserVisibleRows(panel);
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row)
        if (rows[row].first >= 0)
            add(Kind::BrowserRow, {area.x, area.y + (row - first) * kBrowserRowH, area.w, kBrowserRowH - 2.0f},
                rows[row].first);
    add(Kind::BrowserClose, browserClose(panel));
    add(Kind::BrowserFolder, browserFolder(panel));
    return list;
}

void View::paintBrowser(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = browserPanel(height());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 120.0f, 24.0f}, tr(Str::PresetsTitle), 14.0f, kText, Align::Left, true);
    const std::vector<std::pair<int, std::string>> rows = browserRows();
    const size_t count = controller_.presets().size();
    c.text({panel.x + 104.0f, panel.y + 16.0f, 200.0f, 20.0f},
           count == 1 ? std::string(tr(Str::PresetsCountOne)) : trf(Str::PresetsCount, {std::to_string(count)}), 10.5f,
           kFaint, Align::Left);
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);

    const Rect area = browserList(panel);
    if (rows.empty()) {
        c.text({area.x, area.center().y - 20.0f, area.w, 18.0f}, tr(Str::PresetsNone), 12.0f, kDim);
        c.text({area.x, area.center().y, area.w, 18.0f}, tr(Str::PresetsNoneHint), 10.5f, kFaint);
    }
    // Subfolders' headers; the presets' rows are widgets.
    const int shown = browserVisibleRows(panel);
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row)
        if (rows[row].first < 0)
            c.text({area.x + 4.0f, area.y + (row - first) * kBrowserRowH + 4.0f, area.w - 8.0f, kBrowserRowH - 4.0f},
                   rows[row].second, 10.5f, kFaint, Align::Left, true);
    if (static_cast<int>(rows.size()) > shown) { // where the list is
        const Rect track{area.right() + 4.0f, area.y, 3.0f, area.h};
        const float thumb = track.h * shown / rows.size();
        c.fillRect(track, kBorder, 1.5f);
        c.fillRect({track.x, track.y + (track.h - thumb) * first / (rows.size() - shown), track.w, thumb}, kFaint, 1.5f);
    }

    // The folder, cut from the left to fit beside its button.
    std::string folder;
    for (wchar_t ch : controller_.presetFolder())
        folder += ch < 0x80 ? static_cast<char>(ch) : '?';
    const float room = browserFolder(panel).x - panel.x - 36.0f;
    std::string shownFolder = folder;
    while (shownFolder.size() > 4 && c.textWidth(shownFolder, 10.5f) > room)
        shownFolder = "\xE2\x80\xA6" + folder.substr(folder.size() - (shownFolder.size() - 4));
    c.text({panel.x + 24.0f, panel.bottom() - 40.0f, room, 26.0f}, shownFolder, 10.5f, kFaint, Align::Left);
    for (const Widget &w : browserWidgets())
        paintWidget(c, w, {});
}

void View::openPreset(int index) {
    if (controller_.patchEdited()) {
        confirm_ = static_cast<int>(PresetAction::Load);
        pendingPreset_ = index;
        hover_.reset();
    } else if (controller_.loadPresetAt(index)) {
        browserOpen_ = false; // loaded from the browser, it closes; cancelled or unreadable, it stays for another pick
        hover_.reset();
    } else {
        flash(tr(Str::MsgPresetUnreadable));
    }
}

// Steps through the library, round from the last to the first; from Init, to the first or the last.
void View::stepPreset(int direction) {
    controller_.rescanPresets();
    const int count = static_cast<int>(controller_.presets().size());
    if (count == 0) {
        flash(tr(Str::MsgNoPresets));
        return;
    }
    const int now = controller_.presetIndex();
    openPreset(now < 0 ? (direction > 0 ? 0 : count - 1) : (now + direction + count) % count);
}

void View::paintConfirm(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const std::string what = confirmWhat();
    const Rect panel = confirmPanel(height(), what.empty());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 18.0f, panel.w - 48.0f, 22.0f}, tr(Str::ConfirmTitle), 14.0f, kText,
           Align::Left, true);
    float y = panel.y + 48.0f;
    if (!what.empty()) {
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, what, 10.5f, kDim, Align::Left);
        y += kConfirmLine;
    }
    c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, tr(Str::ConfirmSaveFirst), 10.5f, kDim, Align::Left);
    for (const Widget &w : confirmWidgets())
        paintWidget(c, w, {});
}

std::optional<View::Widget> View::hitTest(Point p) const {
    const std::vector<Widget> list = settingsOpen_     ? settingsWidgets()
                                     : confirm_ >= 0   ? confirmWidgets()
                                     : browserOpen_    ? browserWidgets()
                                                       : widgets();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if (it->clip.w > 0.0f && !it->clip.contains(p))
            continue;
        if (it->kind == Kind::Node && !isBoxNode(it->a)) {
            const Point c = it->rect.center();
            if (std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius)
                return *it;
        } else if (it->kind == Kind::Port) {
            if (onNodeEdge(it->a, it->rect.center(), p))
                return *it;
        } else if (it->rect.contains(p)) {
            return *it;
        }
    }
    return std::nullopt;
}

bool View::isHovered(const Widget &w) const {
    return drag_.kind == DragKind::None && hover_ && hover_->same(w);
}

bool View::isActive(const Widget &w) const {
    return drag_.kind != DragKind::None && drag_.widget.same(w);
}

// --- Painting.

void View::paint(Canvas &c) {
    const std::vector<Widget> list = widgets();
    const std::vector<Assignment> mods = assignments();

    theme_ = themeIndex(controller_.theme());
    look = kThemes[theme_];
    glow_ = glowing = shownGlow();
    c.clear(kBackground);
    paintPanels(c);
    paintHeader(c);
    if (shapeEditing() >= 0)
        paintShapeEditor(c, shapeEditing());
    else if (partialEditing() >= 0)
        paintPartialEditor(c, partialEditing());
    else switch (controller_.layout().view) {
    case RoutingView::Graph:
        paintRouting(c);
        break;
    case RoutingView::Matrix:
        paintMatrixLabels(c);
        break;
    case RoutingView::Fx:
        paintFx(c);
        break;
    }
    if (!inspectorCovered())
        paintInspector(c);
    paintKeyboard(c);
    paintNoteSource(c, keyboardTop(controller_.layout().sourcesCollapsed));
    paintSources(c);
    // The open lists (of effects, curves, EQ shapes, waves) go over everything else, the dimmed cards included.
    auto paintWidgets = [&](bool lists) {
        for (const Widget &w : list) {
            const bool open = w.kind == Kind::FxMenu || w.kind == Kind::FxAdd || w.kind == Kind::ListMenu ||
                              w.kind == Kind::ListItem || w.kind == Kind::PeqTypeMenu ||
                              w.kind == Kind::PeqTypeItem || w.kind == Kind::WaveMenu ||
                              w.kind == Kind::WaveMenuItem;
            if (open != lists)
                continue;
            if (w.clip.w > 0.0f)
                c.pushClip(w.clip);
            paintWidget(c, w, mods);
            if (w.clip.w > 0.0f)
                c.popClip();
        }
    };
    paintWidgets(false);
    // An envelope handle's readout goes over the curve and the other handles.
    for (const Widget &w : list)
        if ((w.kind == Kind::EnvPoint || w.kind == Kind::EnvCurve) && (isHovered(w) || isActive(w))) {
            if (w.clip.w > 0.0f)
                c.pushClip(w.clip);
            paintEnvReadout(c, w);
            if (w.clip.w > 0.0f)
                c.popClip();
        }
    // A switched-off source's card is dimmed below its title row (a folded one's name is already dim).
    c.pushClip(sourceCardClip());
    for (int s : sources()) {
        const float card = cardOf(s);
        if (card == kOffScreen || sourceOn(s) || sourceFolded(s))
            continue;
        const Rect p = sourceCard(card);
        c.fillRect({p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}, kPanel.alpha(0.6f), 7.0f);
    }
    c.popClip();
    // So is a switched-off effect's.
    if (shapeEditing() < 0 && controller_.layout().view == RoutingView::Fx) {
        c.pushClip(fxCardClip());
        const std::vector<int> chain = fxChainShown();
        for (size_t i = 0; i < chain.size(); ++i) {
            const float pos = fxScreen(chain, i);
            const float span = fxCardSpan(chain[i]);
            if (!onScreen(pos, kVisibleFx, span) || value(paramIndex(chain[i], FxParam::Enabled)) >= 0.5 ||
                fxFolded(chain[i]))
                continue;
            const Rect p = fxCard(pos, span);
            c.fillRect({p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}, kCardColor.alpha(0.6f), 7.0f);
        }
        c.popClip();
    }
    // And the inspector's page while its operator, filter or shaper is off, below the tabs, summary and switch.
    if (!inspectorCovered() && !nodeOn(controller_.layout().selectedNode))
        c.fillRect({kInspector.x + 1.0f, kInspector.y + 60.0f, kInspector.w - 2.0f, kInspector.h - 61.0f},
                   kPanel.alpha(0.6f), 9.0f);
    paintWidgets(true);
    // The card under the mouse while dragging, before it snaps into place.
    if (drag_.kind == DragKind::SourceMove && drag_.moved) {
        Rect ghost = sourceCard(sourceOffset(sources(), drag_.widget.b) - drag_.scrollStart, sourceSpan(drag_.widget.a));
        ghost.x = std::clamp(ghost.x + drag_.now.x - drag_.start.x, kModPanel.x + 4.0f,
                             kModPanel.right() - 4.0f - ghost.w);
        c.strokeRect(ghost, kSourceColors[drag_.widget.a].alpha(0.6f), 1.0f, 8.0f);
    }

    if (drag_.kind == DragKind::Connect || rerouting() >= 0) {
        const int from = drag_.widget.a;
        // A moving route away from the nodes and the bus is faint: let go of there, it goes.
        const bool removing = rerouting() >= 0 && nodeAt(drag_.now) < 0 && !onBus(drag_.now);
        const Color color = removing ? kFaint : nodeColor(from);
        // The route leaves its node toward the mouse; a moving one has its arrowhead under it.
        const Point origin = nodeCenter(from);
        const Point start = outlineToward(from, origin, drag_.now);
        if (drag_.kind == DragKind::Connect) {
            if (drag_.moved)
                c.line(start, drag_.now, color, 2.0f, true);
        } else {
            const Point dir = normalize({drag_.now.x - origin.x, drag_.now.y - origin.y});
            const Point base{drag_.now.x - dir.x * 4.0f, drag_.now.y - dir.y * 4.0f};
            c.line(start, base, color, 2.0f, true);
            arrowHead(c, base, dir, color);
        }
        const int target = nodeAt(drag_.now);
        if (target >= 0) {
            const Point center = nodeCenter(target);
            // A route that can't be, or that a moving one would find there already, rings faintly.
            const int param = routeParam(from, target);
            const bool fits =
                param >= 0 && (drag_.kind == DragKind::Connect || param == drag_.widget.param || !wired(param));
            const Color ring = fits ? color : kFaint;
            if (isBoxNode(target))
                c.strokeRect({center.x - kFilterHalfW - 5.0f, center.y - kFilterHalfH - 5.0f,
                              2.0f * kFilterHalfW + 10.0f, 2.0f * kFilterHalfH + 10.0f},
                             ring, 2.0f, 10.0f);
            else
                c.strokeCircle(center, kNodeRadius + 5.0f, ring, 2.0f);
        }
    }
    if (drag_.kind == DragKind::ModSource)
        paintModOverlay(c, list);
    else if (hover_ && (hover_->kind == Kind::SourceHandle || hover_->kind == Kind::MacroHandle) &&
             drag_.kind == DragKind::None)
        paintSourceLinks(c, list, hover_->a);
    paintHoverTags(c, list);
    if (typing_)
        paintTyping(c);
    if (browserOpen_)
        paintBrowser(c);
    if (settingsOpen_)
        paintSettings(c);
    else if (confirm_ >= 0)
        paintConfirm(c);
    else
        paintTooltip(c);

    // Under the preset bar, over the panels' title rows.
    if (flashTicks_ > 0) {
        const Rect toast{View::kWidth * 0.5f - 220.0f, 102.0f, 440.0f, 28.0f};
        c.fillRect(toast, kRaised, 8.0f);
        c.strokeRect(toast, kBorder, 1.0f, 8.0f);
        c.text(toast, flash_, 12.0f, kText);
    }
}

void View::paintPanels(Canvas &c) {
    const bool collapsed = controller_.layout().sourcesCollapsed;
    std::vector<Rect> panels = {modPanel(collapsed), keyboardPanel(keyboardTop(collapsed))};
    if (shapeEditing() >= 0 && isClipCurve(shapeEditing()))
        panels.push_back(shapePanel()); // the shaper curve's editor, narrower than the effects it covers
    else if (inspectorCovered())
        panels.push_back(kFxPanel);
    else
        panels.insert(panels.end(), {kRouting, kInspector});
    for (const Rect &p : panels) {
        c.fillRect(p, kPanel, 10.0f);
        c.strokeRect(p, kBorder, 1.0f, 10.0f);
    }
}

void View::paintHeader(Canvas &c) {
    const Rect header{0.0f, 0.0f, View::kWidth, 64.0f};
    c.fillGradient(header, look.headerTop, look.headerBottom);
    c.line({0.0f, 63.5f}, {View::kWidth, 63.5f}, kBorder, 1.0f);
    // The name: "FM" in the operators' color, then "Vibe".
    c.text({24.0f, 12.0f, 40.0f, 40.0f}, "FM", 22.0f, kAccent, Align::Left, true);
    c.text({53.0f, 12.0f, 100.0f, 40.0f}, "Vibe", 22.0f, kText, Align::Left, true);

    // The output meter: each side's peak in the theme's color up to full scale, red past it, where the clip light
    // stays on for a while after the output went over.
    auto meterY = [](double db) {
        const double at = std::clamp((db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb), 0.0, 1.0);
        return kMeter.bottom() - kMeter.h * static_cast<float>(at);
    };
    const float zero = meterY(0.0);
    const bool clipped = meterClipped(telemetry_);
    for (int side = 0; side < 2; ++side) {
        const Rect bar{kMeter.x + side * 9.0f, kMeter.y, 5.0f, kMeter.h};
        c.fillRect({bar.x, zero, bar.w, bar.bottom() - zero}, kTrack, 1.5f);
        c.fillRect({bar.x, bar.y, bar.w, zero - 1.0f - bar.y}, kMeterRed.alpha(clipped ? 1.0f : 0.2f), 1.5f);
        const float peak = meterPeak(telemetry_, side);
        if (peak <= 0.0f)
            continue;
        const float top = meterY(20.0 * std::log10(peak));
        if (top < bar.bottom())
            c.fillRect({bar.x, std::max(top, zero), bar.w, bar.bottom() - std::max(top, zero)}, kAccent, 1.5f);
        if (top < zero - 1.0f)
            c.fillRect({bar.x, top, bar.w, zero - 1.0f - top}, kMeterRed, 1.5f);
    }

    for (float x : kDividers)
        c.line({x, 14.0f}, {x, 50.0f}, kBorder, 1.0f);

    char playing[16];
    std::snprintf(playing, sizeof(playing), "%d", telemetry_.voices);
    c.text({1110.0f, 13.0f, 74.0f, 14.0f}, tr(Str::HeadingPlaying), 10.5f, kDim, Align::Right, true);
    c.text({1110.0f, 28.0f, 74.0f, 22.0f}, playing, 15.0f, telemetry_.voices > 0 ? kText : kFaint, Align::Right,
           true);
}

void View::paintKeyboard(Canvas &c) {
    const float top = keyboardTop(controller_.layout().sourcesCollapsed);
    const Rect bend = bendWheel(top), mod = modWheel(top);
    c.text({bend.x - 16.0f, bend.bottom() + 4.0f, bend.w + 32.0f, 14.0f}, tr(Str::HeadingBend), 10.5f, kFaint,
           Align::Center, true);
    c.text({mod.x - 16.0f, mod.bottom() + 4.0f, mod.w + 32.0f, 14.0f}, tr(Str::HeadingMod), 10.5f, kFaint,
           Align::Center, true);
    const Rect down = octaveButton(-1, top);
    const Rect up = octaveButton(1, top);
    const std::string range = noteName(keyboardBase()) + " \xE2\x80\x93 " + noteName(keyboardBase() + kKeyCount - 1);
    c.text({down.right() + 4.0f, buttonY(top), up.x - down.right() - 8.0f, 26.0f}, range, 10.5f, kDim);
}

void View::paintKey(Canvas &c, const Widget &w, bool hovered, bool active) {
    const bool black = isBlackKey(w.a);
    Color color = black ? look.blackKey : look.whiteKey;
    if (hovered)
        color = black ? look.blackKeyHover : look.whiteKeyHover;
    // Lit while held: clicked here, or played by the host.
    if (active || telemetry_.keyDown(keyboardBase() + w.a))
        color = kAccent;
    c.fillRect(w.rect, color, 3.0f);
    c.strokeRect(w.rect, kBorder, 1.0f, 3.0f);
    if (!black && w.a % 12 == 0) // label the C keys
        c.text({w.rect.x, w.rect.bottom() - 24.0f, w.rect.w, 16.0f}, noteName(keyboardBase() + w.a), 10.5f,
               look.keyLabel);
}

void View::paintWheel(Canvas &c, const Widget &w, bool hovered, bool active) {
    const Rect r = w.rect;
    const float inset = 4.0f;
    const float top = r.y + inset;
    const float bottom = r.bottom() - inset;
    c.fillRect(r, kRaised, 8.0f);
    c.strokeRect(r, hovered || active ? kDim : kBorder, 1.0f, 8.0f);
    if (w.kind == Kind::ModWheel) {
        // Fills up from the bottom with the wheel's value, in the color of its handle; bipolar, from the middle, where
        // it moves nothing.
        const float y = bottom - (bottom - top) * static_cast<float>(value(w.param));
        const float from = sourceBipolar(kWheelSource) ? 0.5f * (top + bottom) : bottom;
        c.fillRect({r.x + inset, std::min(y, from), r.w - 2.0f * inset, std::abs(from - y)},
                   kSourceColors[kWheelSource].alpha(0.6f), 3.0f);
        if (from < bottom)
            c.line({r.x + 3.0f, from}, {r.right() - 3.0f, from}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    } else {
        // Springs back to the middle: fills up or down from the center.
        const float mid = r.y + r.h * 0.5f;
        const float y = mid - static_cast<float>(bend_) * (r.h * 0.5f - inset);
        c.fillRect({r.x + inset, std::min(mid, y), r.w - 2.0f * inset, std::abs(y - mid)}, kFilterColor.alpha(0.6f),
                   3.0f);
        c.line({r.x + 3.0f, mid}, {r.right() - 3.0f, mid}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    }
}

void View::paintRouting(Canvas &c) {
    c.text({kHintX, kRouting.y + 10.0f, kRouting.right() - 16.0f - kHintX, 22.0f},
           tr(Str::HintRouting), 10.5f, kFaint, Align::Right);

    // Output bus along the bottom, lit while a new or moving route would go there.
    const int moving = rerouting(); // drawn following the mouse instead
    const int busParam = drag_.kind == DragKind::Connect || moving >= 0 ? outParam(drag_.widget.a) : -1;
    const bool busTarget = busParam >= 0 && onBus(drag_.now) &&
                           (drag_.kind == DragKind::Connect || busParam == moving || !wired(busParam));
    c.line({kRouting.x + 48.0f, kBusY}, {kRouting.right() - 20.0f, kBusY}, busTarget ? kAccent : kTrack, 3.0f);
    c.text({kRouting.x + 14.0f, kBusY - 9.0f, 30.0f, 18.0f}, tr(Str::BusOut), 10.5f, kDim, Align::Left, true);

    // Modulated routes swell and thin with their modulation while notes play, and carry a thin core
    // in the color of the source that modulates them.
    const std::vector<int> on = enabledNodes();
    const std::vector<Assignment> mods = assignments();
    auto focused = [&](int param) {
        return (hover_ && hover_->param == param && drag_.kind == DragKind::None) ||
               (drag_.kind == DragKind::Value && drag_.widget.param == param);
    };
    for (int node : on) {
        const int param = outParam(node);
        if (!goesOut(node) || param == moving)
            continue;
        const float out = static_cast<float>(liveAmount(param));
        const OutWire w = outWireOf(node, nodeCenter(node));
        const Color base = isBoxNode(node) ? nodeColor(node) : kText;
        const Color color = focused(param) ? kText : base.alpha(0.25f + 0.5f * out);
        c.line(w.start, w.end, color, 1.5f + 2.0f * out);
        if (const auto core = modColor(param, mods))
            c.line(w.start, w.end, core->alpha(0.9f), 1.2f);
        arrowHead(c, w.end, {0.0f, 1.0f}, color);
    }

    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (param < 0 || !wired(param) || param == moving)
                continue;
            const float amount = static_cast<float>(liveAmount(param));
            const std::optional<Color> core = modColor(param, mods);
            const Color base = routeColor(from, to);
            const Color color = focused(param) ? kText : base.alpha(0.35f + 0.65f * amount);
            const float width = 1.2f + 3.0f * amount;
            if (from == to) {
                c.strokeCircle(loopCenter(nodeCenter(from)), 9.0f, color, width);
                if (core)
                    c.strokeCircle(loopCenter(nodeCenter(from)), 9.0f, core->alpha(0.9f), 1.2f);
                continue;
            }
            const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to));
            c.curve(w.start, w.control, w.end, color, width);
            if (core)
                c.curve(w.start, w.control, w.end, core->alpha(0.9f), 1.2f);
            arrowHead(c, w.end, w.tangent, color);
        }
    }
}

double View::liveAmount(int param) const {
    const ParamDef &def = table_[param];
    const double offset = telemetry_.voices > 0 ? liveOffset(param) : 0.0;
    return std::clamp((value(param) + offset - def.min) / (def.max - def.min), 0.0, 1.0);
}

std::optional<Color> View::modColor(int param, const std::vector<Assignment> &mods) const {
    for (const Assignment &m : mods)
        if (m.target == param && !m.bypassed && sourceOn(m.source))
            return kSourceColors[m.source];
    return std::nullopt;
}

// How far a modulation of `amount` takes its target from the set value, down and up: a bipolar source half of it
// either way, a unipolar one all of it one way.
std::pair<double, double> View::modReach(int source, double amount) const {
    if (sourceBipolar(source))
        return {-0.5 * std::abs(amount), 0.5 * std::abs(amount)};
    return {std::min(amount, 0.0), std::max(amount, 0.0)};
}

bool View::sourceBipolar(int source) const {
    return fmvibe::sourceBipolar([this](int index) { return value(index); }, source);
}

void View::paintModRange(Canvas &c, Rect bar, int param, const std::vector<Assignment> &mods) {
    const double n = norm(param);
    // The modulation whose depth handle is under the mouse goes last, twice as thick, over the others.
    auto range = [&](const Assignment &m, bool focused) {
        const auto [down, up] = modReach(m.source, m.amount);
        const double from = std::clamp(n + down, 0.0, 1.0);
        const double to = std::clamp(n + up, 0.0, 1.0);
        const float alpha = sourceOn(m.source) && !m.bypassed ? 1.0f : (focused ? 0.4f : 0.3f);
        const float h = focused ? 2.0f * bar.h : bar.h;
        c.fillRect({bar.x + static_cast<float>(from) * bar.w, bar.center().y - 0.5f * h,
                    std::max(static_cast<float>(to - from) * bar.w, 1.5f), h},
                   kSourceColors[m.source].alpha(alpha), 1.0f);
    };
    bool any = false;
    const Assignment *focus = nullptr;
    for (const Assignment &m : mods) {
        if (m.target != param)
            continue;
        any = true;
        if (modFocused(m))
            focus = &m;
        else
            range(m, false);
    }
    if (!any)
        return;
    if (focus)
        range(*focus, true);
    // Where the set amount is, and where the newest voice is now.
    c.fillRect({bar.x + static_cast<float>(n) * bar.w - 0.75f, bar.y - 1.0f, 1.5f, bar.h + 2.0f}, kDim, 0.5f);
    if (telemetry_.voices > 0 && liveOffset(param) != 0.0f)
        c.fillCircle({bar.x + static_cast<float>(liveAmount(param)) * bar.w, bar.center().y}, 2.2f, kText);
}

void View::paintMatrixLabels(Canvas &c) {
    // A node that is off has its name greyed out, like its row and column of cells; the name under the mouse
    // lights up. The node the inspector shows has its names on a plate, like its tab: a click on them switches
    // it off or on again, as on the tab, and a click on another name selects that node (switching it on).
    const int selected = controller_.layout().selectedNode;
    auto nameColor = [&](int node) {
        if (hover_ && hover_->kind == Kind::MatrixLabel && hover_->a == node)
            return kText;
        const Color color = isBoxNode(node) ? nodeColor(node) : (node == selected ? kText : kDim);
        return nodeOn(node) ? color : (isBoxNode(node) ? color.alpha(0.35f) : kFaint);
    };
    const Color plate = nodeColor(selected).alpha(nodeOn(selected) ? 0.22f : 0.1f);
    const Rect first = gridCell(selected, 0), top = gridCell(0, selected); // the first cells of its row and column
    c.fillRect({first.x - 31.0f, first.y + 3.0f, 26.0f, first.h - 6.0f}, plate, 5.0f);
    c.fillRect({top.x - 1.0f, top.y - 22.0f, top.w + 2.0f, 18.0f}, plate, 5.0f);
    for (int col = 0; col < kGridCols; ++col) {
        const Rect cell = gridCell(0, col);
        const bool out = col == kGridCols - 1;
        c.text({cell.x - 6.0f, cell.y - 22.0f, cell.w + 12.0f, 18.0f}, out ? tr(Str::BusOut) : kNodeNames[col], 12.0f,
               out ? kText : nameColor(col), Align::Center, true);
    }
    for (int row = 0; row < kNodes; ++row) {
        const Rect cell = gridCell(row, 0);
        c.text({cell.x - 32.0f, cell.y, 28.0f, cell.h}, kNodeNames[row], 12.5f, nameColor(row), Align::Center, true);
        for (int col = 0; col < kGridCols; ++col)
            if (gridParam(row, col) < 0)
                c.fillRect(gridCell(row, col), kRaised.alpha(0.35f), 5.0f); // no such route
    }
    c.text({kLegendX, kRouting.y + 10.0f, kRouting.right() - 16.0f - kLegendX, 22.0f},
           tr(Str::HintMatrix), 10.5f, kFaint, Align::Right);

    // Legend beside the grid.
    const float width = kRouting.right() - 16.0f - kLegendX;
    float y = kRouting.y + 72.0f;
    c.text({kLegendX, y - 22.0f, width, 16.0f}, tr(Str::HeadingHowToRead), 10.5f, kFaint, Align::Left, true);
    struct Entry {
        Color color;
        bool outline;
        const char *text;
    };
    const Entry entries[] = {
        {kAccent, false, tr(Str::LegendFm)},
        {kAccent, true, tr(Str::LegendFeedback)},
        {kFilterColor, false, tr(Str::LegendFilter)},
        {kShaperColor, false, tr(Str::LegendShaper)},
        {kText, false, tr(Str::LegendOut)},
    };
    for (const Entry &e : entries) {
        const Rect swatch{kLegendX, y + 3.0f, 14.0f, 14.0f};
        if (e.outline)
            c.strokeRect(swatch, e.color.alpha(0.6f), 1.0f, 3.0f);
        else
            c.fillRect(swatch, e.color.alpha(0.45f), 3.0f);
        c.text({kLegendX + 24.0f, y, width - 24.0f, 20.0f}, e.text, 11.0f, kDim, Align::Left);
        y += 26.0f;
    }
    y += 10.0f;
    const Str notes[] = {Str::MatrixClick, Str::MatrixClickThen};
    for (size_t i = 0; i < std::size(notes); ++i)
        c.text({kLegendX, y + static_cast<float>(i) * 20.0f, width, 18.0f}, tr(notes[i]), 11.0f, kFaint, Align::Left);
}

void View::paintFx(Canvas &c) {
    const std::vector<int> chain = fxChainShown();
    // How many of the chain's places are taken, beside ADD EFFECT.
    const Rect count{fxMenuButton().right() + 10.0f, fxMenuButton().y, 44.0f, fxMenuButton().h};
    const bool full = chain.size() >= static_cast<size_t>(kFxSlots);
    c.text(count, std::to_string(chain.size()) + "/" + std::to_string(kFxSlots), 11.0f, full ? kText : kDim,
           Align::Left, full);
    const float hintX = count.right() + 8.0f;
    if (!chain.empty()) // how to work the cards, while there are some
        c.text({hintX, kFxPanel.y + 10.0f, kFxPanel.right() - 16.0f - hintX, 22.0f}, tr(Str::HintFx), 10.5f, kFaint,
               Align::Right);
    // After a card, before the one at place `next`: the signal runs left to right. Under the mouse, or with its list
    // open, it is a plus that adds an effect there.
    auto chevron = [&](const Rect &card, size_t next) {
        const Point m{card.right() + kFxGap * 0.5f, card.center().y};
        const bool lit = (hover_ && hover_->kind == Kind::FxInsert && hover_->a == static_cast<int>(next)) ||
                         (fxMenuOpen_ && fxInsertAt_ == static_cast<int>(next));
        if (lit) {
            c.fillCircle(m, 8.0f, kFxColor.alpha(0.25f));
            c.strokeCircle(m, 8.0f, kFxColor, 1.2f);
            c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText, 1.5f);
            c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText, 1.5f);
            return;
        }
        const Point arrow[] = {{m.x - 3.0f, m.y - 6.0f}, {m.x + 3.0f, m.y}, {m.x - 3.0f, m.y + 6.0f}};
        c.polyline(arrow, 3, kFaint, 1.5f);
    };
    c.pushClip(fxCardClip());
    for (size_t i = 0; i < chain.size(); ++i) {
        const int slot = chain[i];
        const FxType type = fxSlotType(slot);
        const float pos = fxScreen(chain, i);
        if (!onScreen(pos, kVisibleFx, fxCardSpan(slot)))
            continue;
        const Rect card = fxCard(pos, fxCardSpan(slot));
        const bool on = value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
        const bool dragged = drag_.kind == DragKind::FxMove && drag_.widget.a == slot;
        const Color fx = fxTypeColor(type); // each type of effect in a color of its own
        c.fillRect(card, kCardColor, 8.0f);
        c.strokeRect(card, dragged ? fx : (on ? fx.alpha(0.45f) : kBorder), dragged ? 1.5f : 1.0f, 8.0f);
        if (fxFolded(slot)) { // its icon (a widget of its own) and cross, then its name down the strip
            paintDownward(c, {card.x, card.y + 66.0f, card.w, card.h - 74.0f}, fxTitle(type), on ? kText : kDim);
            if (i + 1 < chain.size())
                chevron(card, i + 1);
            continue;
        }

        // Title row: the effect's icon (a widget of its own, which switches it) and its name.
        const Rect titleRect{card.x + 34.0f, card.y + 8.0f, card.w - 82.0f, 20.0f};
        const char *title = fxTitle(type);
        float titleSize = 11.0f; // smaller for a long name, so it fits beside the icon
        while (titleSize > 9.0f && c.textWidth(title, titleSize, true) > titleRect.w)
            titleSize -= 0.5f;
        c.text(titleRect, title, titleSize, on ? kText : kDim, Align::Left, true);

        const Rect display = fxDisplay(pos, type);
        c.fillRect(display, kDisplayColor, 6.0f);
        switch (type) {
        case FxType::Shaper:
            paintShaperCurve(c, display, slot);
            break;
        case FxType::Delay:
            paintDelay(c, display, slot);
            break;
        case FxType::Eq:
            paintEq(c, display, slot);
            break;
        case FxType::ParamEq:
            paintParamEq(c, display, slot);
            break;
        case FxType::Filter:
            paintFilterResponse(c, display, paramIndex(slot, FxParam::FilterType),
                                paramIndex(slot, FxParam::FilterSlope), paramIndex(slot, FxParam::FilterCutoff),
                                paramIndex(slot, FxParam::FilterResonance), paramIndex(slot, FxParam::FilterKeyTrack),
                                telemetry_.fxPitch, fx, &telemetry_.fxSpectrum[slot]);
            break;
        case FxType::Compressor:
            paintCompressor(c, display, slot);
            break;
        case FxType::MultiComp:
            paintMultiComp(c, display, slot);
            break;
        case FxType::Reverb:
            paintReverb(c, display, slot);
            break;
        case FxType::Phaser:
        case FxType::Flanger:
            paintSweep(c, display, slot, type);
            break;
        case FxType::FreqShifter:
            paintShifter(c, display, slot);
            break;
        case FxType::Disperser:
            paintDisperser(c, display, slot);
            break;
        case FxType::Widener:
            paintWidener(c, display, slot);
            break;
        case FxType::None:
        case FxType::Count:
            break;
        }
        if (type == FxType::MultiComp) // the band's own knobs above, the shared ones below
            c.line({card.x + 14.0f, card.y + 197.5f}, {card.right() - 14.0f, card.y + 197.5f}, kBorder, 1.0f);
        if (!on)
            c.fillRect(display, kPanel.alpha(0.55f), 6.0f); // dimmed while switched off
        if (i + 1 < chain.size())
            chevron(card, i + 1);
    }
    c.popClip();

    // The card under the mouse while dragging, before it snaps into place.
    if (drag_.kind == DragKind::FxMove && drag_.moved) {
        Rect ghost = fxCard(fxOffset(fxChain(), drag_.widget.b) - drag_.scrollStart, fxCardSpan(drag_.widget.a));
        ghost.x = std::clamp(ghost.x + drag_.now.x - drag_.start.x, kFxPanel.x + 4.0f,
                             kFxPanel.right() - 4.0f - ghost.w);
        c.strokeRect(ghost, fxColor(drag_.widget.a).alpha(0.6f), 1.0f, 8.0f);
    }
}

void View::paintShapeEditor(Canvas &c, int shape) {
    if (isNoteCurve(shape)) {
        paintNoteCurveEditor(c);
        return;
    }
    if (isClipCurve(shape) || isShaperCurve(shape)) {
        paintClipCurveEditor(c, shape);
        return;
    }
    const int lfo = shape;
    const Color color = kSourceColors[lfo];
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 104.0f, 22.0f};
    c.fillRect(title, color.alpha(0.18f), 6.0f);
    c.text(title, trf(Str::TitleShape, {sourceName(lfo)}), 11.0f, color, Align::Center, true);
    c.text({title.right() + 12.0f, kRouting.y + 10.0f, shapeReset().x - title.right() - 24.0f, 22.0f},
           tr(Str::HintCurve), 10.5f, kFaint, Align::Right);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, 8.0f);
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const double zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? -1.0 : 0.0;
    paintShapeGrid(c, zero, false);
    paintShapeGridControls(c);

    // The curve, filled toward the zero line.
    auto get = [this](int index) { return value(index); };
    std::vector<Point> points;
    for (int i = 0; i <= 400; ++i) {
        const double x = i / 400.0;
        points.push_back(shapeAt(x, shapeValue(get, lfo, x)));
    }
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, zero));
    area.push_back(shapeAt(0.0, zero));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);

    if (telemetry_.voices > 0 && sourceOn(lfo)) {
        const Point dot = shapeAt(lfoPhaseNow(lfo), lfoValueNow(lfo));
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse, in numbers.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == lfo) {
        const double x = shapePointX(lfo, focus->b);
        const std::string cycle = number("%.1f", x * 100.0);
        // What the LFO puts out there: a unipolar one maps -1..1 onto 0..1.
        const double y = value(shapePointIndex(lfo, focus->b, PointField::Y));
        const std::string level = zero < 0.0 ? number("%.2f", 0.5 * (y + 1.0)) : number("%+.2f", y);
        c.text({r.x, r.bottom() + 10.0f, r.w - 220.0f, 16.0f},
               trf(Str::CaptionShapePoint, {std::to_string(focus->b + 1), cycle, level}), 10.5f, kDim, Align::Left);
    }
}

// A shaper's drawn curve: input across, output up, the plain line (no change) for reference.
void View::paintClipCurveEditor(Canvas &c, int shape) {
    const int unit = shape - kMaxSources;
    const Color fx = clipColor(unit);
    const Rect panel = shapePanel();
    const Rect title{panel.x + 16.0f, panel.y + 9.0f, 132.0f, 22.0f};
    c.fillRect(title, fx.alpha(0.18f), 6.0f);
    c.text(title,
           unit < kFxSlots ? trf(Str::TitleFxShaperCurve, {std::to_string(unit + 1)})
                           : trf(Str::TitleShaperCurve, {std::to_string(unit - kFxSlots + 1)}),
           11.0f, fx, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, 8.0f);
    paintShapeGrid(c, 0.0, true);
    paintShapeGridControls(c);
    c.line(shapeAt(0.0, -1.0), shapeAt(1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    c.text({r.right() - 120.0f, r.bottom() - 18.0f, 116.0f, 14.0f}, tr(Str::AxisInput), 10.5f, kFaint, Align::Right);
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOutput), 10.5f, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    std::vector<Point> points;
    for (int i = 0; i <= 400; ++i) {
        const double x = i / 400.0;
        points.push_back(shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0)));
    }
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, 0.0));
    area.push_back(shapeAt(0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    const Rect below{r.x, r.bottom() + 10.0f, shapeGridButton(0, 0).x - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const std::string in = number("%+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0);
        const std::string out = number("%+.2f", value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text(below, trf(Str::CaptionCurvePoint, {std::to_string(focus->b + 1), in, out}), 10.5f, kDim, Align::Left);
    } else {
        c.text(below, tr(Str::HintCurve), 10.0f, kFaint, Align::Left);
    }
}

// The note source's curve: the notes from Low to High across (the Cs between them named under the plot), what
// the source puts out up, from nothing at the bottom to all of it at the top, and for reference, dashed, what would
// take a cutoff at 0 by 100 % onto the note (the line it starts as); while notes play, a dot where the newest
// voice is.
void View::paintNoteCurveEditor(Canvas &c) {
    const int shape = kNoteCurve;
    const Color color = shapeColor(shape);
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 120.0f, 22.0f};
    c.fillRect(title, color.alpha(0.18f), 6.0f);
    c.text(title, tr(Str::TitleNoteCurve), 11.0f, color, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, 8.0f);
    paintShapeGrid(c, -1.0, false);
    paintShapeGridControls(c);
    const double low = value(paramIndex(NoteParam::Low)), high = value(paramIndex(NoteParam::High));
    std::vector<Point> exact; // what would put a cutoff at 0 by 100 % on the note, over this range
    for (int i = 0; i <= 120; ++i) {
        const double x = i / 120.0;
        exact.push_back(shapeAt(x, 2.0 * cutoffNormOfPitch(low + x * (high - low)) - 1.0));
    }
    c.polyline(exact.data(), exact.size(), kFaint.alpha(0.6f), 1.0f, true);
    for (int octave = 0; octave * 12.0 <= kMaxNotePitch; ++octave) {
        const double x = high == low ? -1.0 : (octave * 12.0 - low) / (high - low);
        if (x > 0.03 && x < 0.97)
            c.text({shapeAt(x, -1.0).x - 20.0f, r.bottom() - 16.0f, 40.0f, 14.0f}, noteName(octave * 12), 10.0f, kFaint);
    }
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOut), 10.5f, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    std::vector<Point> points;
    for (int i = 0; i <= 400; ++i) {
        const double x = i / 400.0;
        points.push_back(shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0)));
    }
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, -1.0));
    area.push_back(shapeAt(0.0, -1.0));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (telemetry_.voices > 0) {
        const double x = high == low ? (telemetry_.notePitch >= low ? 1.0 : 0.0)
                                     : std::clamp((telemetry_.notePitch - low) / (high - low), 0.0, 1.0);
        const Point dot = shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0));
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    const Rect below{r.x, r.bottom() + 10.0f, shapeGridButton(0, 0).x - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char note[32];
        formatParam(paramIndex(NoteParam::Low), low + shapePointX(shape, focus->b) * (high - low), note, sizeof(note));
        const std::string out = number("%.0f", 50.0 * (value(shapeIndex(shape, focus->b, PointField::Y)) + 1.0));
        c.text(below, trf(Str::CaptionNotePoint, {std::to_string(focus->b + 1), note, out}), 10.5f, kDim, Align::Left);
    } else {
        c.text(below, tr(Str::HintNoteCurve), 10.0f, kFaint, Align::Left);
    }
}

int View::partialEditing() const {
    if (partialEditor_ < 0 || shapeEditing() >= 0)
        return -1;
    return std::lround(value(paramIndex(partialEditor_, OpParam::Wave))) == static_cast<long>(OpWave::Custom)
               ? partialEditor_
               : -1;
}

int View::partialsShown(int op) const {
    return partialCount(value(partialCountIndex(op)));
}

void View::paintPartialEditor(Canvas &c, int op) {
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 120.0f, 22.0f};
    c.fillRect(title, kAccent.alpha(0.18f), 6.0f);
    c.text(title, trf(Str::TitlePartials, {kNodeNames[op]}), 11.0f, kAccent, Align::Center, true);
    // Only the partials the wave plays show; the ones past them keep their values for when there are more again.
    const int count = partialsShown(op);
    // The partial under the mouse in numbers, or how to draw.
    const Widget *focus = drag_.kind == DragKind::Partials ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    std::string hint = tr(Str::HintPartials);
    if (focus && focus->kind == Kind::PartialArea) {
        const Point at = drag_.kind == DragKind::Partials ? drag_.now : hoverPoint_;
        const int k = partialAt(focus->b == 0 ? kPartialLevels : kPartialPhases, at.x, count);
        const double level = value(partialLevelIndex(op, k));
        const std::string partial = std::to_string(k + 1), phase = number("%.0f", value(partialPhaseIndex(op, k)) * 360.0);
        hint = level > 0.0 ? trf(Str::CaptionPartial, {partial, number("%.1f", 20.0 * std::log10(level)), phase})
                           : trf(Str::CaptionPartialSilent, {partial, phase});
    }
    const float hintX = partialCountButton(static_cast<int>(std::size(kPartialCounts)) - 1).right() + 12.0f;
    c.text({hintX, kRouting.y + 10.0f, shapeReset().x - hintX - 12.0f, 22.0f}, hint, 10.5f, kFaint, Align::Right);

    // The wave they make, one cycle, at its loudest from top to bottom, as the operator plays it.
    const Rect wave = kPartialWave;
    c.fillRect(wave.inset(-6.0f), kDisplayColor, 6.0f);
    c.line({wave.x, wave.center().y}, {wave.right(), wave.center().y}, kBorder, 1.0f);
    std::vector<double> samples(241, 0.0);
    double peak = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double x = static_cast<double>(i) / (samples.size() - 1);
        for (int k = 0; k < count; ++k)
            if (const double level = value(partialLevelIndex(op, k)); level > 0.0)
                samples[i] += level * std::sin(2.0 * std::numbers::pi * ((k + 1) * x + value(partialPhaseIndex(op, k))));
        peak = std::max(peak, std::abs(samples[i]));
    }
    std::vector<Point> points;
    for (size_t i = 0; i < samples.size(); ++i)
        points.push_back({wave.x + wave.w * i / (samples.size() - 1),
                          wave.center().y - static_cast<float>(peak > 0.0 ? samples[i] / peak : 0.0) * wave.h * 0.5f});
    plotLine(c, points.data(), points.size(), kAccent, 2.0f);

    // Levels (in dB, a faint line every 12) and phases (a full cycle at the top) alike as bars from the bottom,
    // in different colors; a phase is faint while its partial is silent.
    for (int row = 0; row < 2; ++row) {
        const Rect area = row == 0 ? kPartialLevels : kPartialPhases;
        c.fillRect(area.inset(-6.0f), kDisplayColor, 6.0f);
        c.text({area.x - 2.0f, area.y - 22.0f, 80.0f, 12.0f}, tr(row == 0 ? Str::HeadingLevel : Str::HeadingPhase), 10.5f,
               kFaint, Align::Left, true);
        for (int k = 0; k < count; ++k) {
            const Rect bar = partialBar(area, k, count);
            const double v = row == 0 ? partialBarFromLevel(value(partialLevelIndex(op, k)))
                                      : value(partialPhaseIndex(op, k));
            const float h = static_cast<float>(v) * bar.h;
            c.fillRect({bar.x, area.y, bar.w, area.h}, kRaised.alpha(0.5f), 2.0f);
            if (h > 0.5f) {
                const bool silent = row == 1 && value(partialLevelIndex(op, k)) <= 0.0;
                const float alpha = (0.35f + 0.5f * static_cast<float>(v)) * (silent ? 0.4f : 1.0f);
                c.fillRect({bar.x, bar.bottom() - h, bar.w, h}, (row == 0 ? kAccent : kFxColor).alpha(alpha), 2.0f);
            }
        }
        if (row == 0)
            for (int db = 0; db > kPartialFloorDb; db -= 12) {
                const double bar = partialBarFromLevel(std::pow(10.0, db / 20.0));
                const float y = area.bottom() - static_cast<float>(bar) * area.h;
                if (db < 0)
                    c.line({area.x, y}, {area.right(), y}, kBorder.alpha(0.5f), 1.0f);
                c.text({area.x - 38.0f, y - 6.0f, 30.0f, 12.0f}, std::to_string(db), 9.0f, kFaint, Align::Right);
            }
    }
    // Each partial's number under it; of 64, the first and every fourth, where there is room for them.
    for (int k = 0; k < count; ++k) {
        if (count > kFirstPartials && k > 0 && (k + 1) % 4 != 0)
            continue;
        const Rect bar = partialBar(kPartialPhases, k, count);
        c.text({bar.center().x - 12.0f, kPartialPhases.bottom() + 8.0f, 24.0f, 12.0f}, std::to_string(k + 1), 10.0f,
               kFaint);
    }
}

void View::paintShapeGrid(Canvas &c, double zero, bool centered) {
    const Rect r = shapeArea();
    const int across = controller_.layout().shapeGridX;
    const int upDown = controller_.layout().shapeGridY;
    // Lines across stand out every quarter, or every third on a grid of thirds; a centered grid's middle more.
    const int major = across % 4 == 0 && across > 4 ? across / 4 : (across % 3 == 0 && across > 3 ? across / 3 : 0);
    for (int i = 1; i < across; ++i) {
        const float x = r.x + r.w * i / across;
        const float alpha = centered && 2 * i == across ? 1.0f : (major > 0 && i % major == 0 ? 0.9f : 0.35f);
        c.line({x, r.y}, {x, r.bottom()}, kBorder.alpha(alpha), 1.0f);
    }
    // Up and down: solid at zero, dashed elsewhere, fainter between the halves.
    for (int i = 0; i <= upDown; ++i) {
        const double y = -1.0 + 2.0 * i / upDown;
        const bool atZero = std::abs(y - zero) < 1e-9;
        const bool half = 4 * i % upDown == 0;
        c.line(shapeAt(0.0, y), shapeAt(1.0, y), kBorder.alpha(atZero ? 1.0f : (half ? 0.5f : 0.3f)), 1.0f, !atZero);
    }
}

// "GRID - 16 + x - 8 +" under the editor's right end; the buttons and counts are widgets.
void View::paintShapeGridControls(Canvas &c) {
    const Rect first = shapeGridButton(0, 0);
    c.text({first.x - 48.0f, first.y, 42.0f, first.h}, tr(Str::HeadingGrid), 10.5f, kFaint, Align::Right, true);
    const Rect across = shapeGridButton(0, 2), upDown = shapeGridButton(1, 0);
    c.text({across.right(), across.y, upDown.x - across.right(), across.h}, "\xC3\x97", 11.0f, kFaint);
}

// Transfer curve: input level across, output up, with the drive, output and mix applied.
void View::paintShaperCurve(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int curve = kMaxSources + unit;
    const ClipMode mode = clipMode(value(clip.mode));
    if (mode == ClipMode::Custom && shapeEditing() != curve) { // with its points, unless its big editor has them
        paintClipCurveCard(c, r, unit);
        return;
    }
    const Rect inner = r.inset(8.0f);
    constexpr double kRange = 1.3;
    auto at = [&](double x, double y) {
        return Point{inner.center().x + static_cast<float>(x / kRange) * inner.w * 0.5f,
                     inner.center().y - static_cast<float>(std::clamp(y, -kRange, kRange) / kRange) * inner.h * 0.5f};
    };
    c.line(at(-kRange, 0.0), at(kRange, 0.0), kBorder, 1.0f);
    c.line(at(0.0, -kRange), at(0.0, kRange), kBorder, 1.0f);
    c.line(at(-kRange, 1.0), at(kRange, 1.0), kBorder.alpha(0.7f), 1.0f, true); // full scale
    c.line(at(-kRange, -1.0), at(kRange, -1.0), kBorder.alpha(0.7f), 1.0f, true);
    c.line(at(-kRange, -kRange), at(kRange, kRange), kFaint.alpha(0.6f), 1.0f);

    const double drive = std::pow(10.0, value(clip.drive) / 20.0);
    const double output =
        std::pow(10.0, clipOutputDb(value(clip.output), value(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = value(clip.mix);
    auto get = [this](int index) { return value(index); };
    auto shape = [&](double x) {
        return mode == ClipMode::Custom ? clipCurveValue(get, shapeBase(curve), x) : clipShape(mode, x);
    };
    std::vector<Point> points;
    for (int i = 0; i <= 240; ++i) {
        const double x = kRange * (2.0 * i / 240.0 - 1.0);
        points.push_back(at(x, x + (shape(x * drive) * output - x) * mix));
    }
    plotLine(c, points.data(), points.size(), fx, 2.0f);
}

// A drawn curve in its card, as in its editor (input across, output up, to full scale), with its points and
// bends to drag (widgets); faintly behind it, what the shaper makes of it with its drive, output and mix.
void View::paintClipCurveCard(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int shape = kMaxSources + unit;
    c.line(shapeAt(shape, 0.0, 0.0), shapeAt(shape, 1.0, 0.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.5, -1.0), shapeAt(shape, 0.5, 1.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.0, -1.0), shapeAt(shape, 1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    const double drive = std::pow(10.0, value(clip.drive) / 20.0);
    const double output =
        std::pow(10.0, clipOutputDb(value(clip.output), value(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = value(clip.mix);
    std::vector<Point> driven, points;
    for (int i = 0; i <= 160; ++i) {
        const double x = i / 160.0, in = 2.0 * x - 1.0;
        const double out = in + (clipCurveValue(get, base, in * drive) * output - in) * mix;
        driven.push_back(shapeAt(shape, x, std::clamp(out, -1.1, 1.1)));
        points.push_back(shapeAt(shape, x, clipCurveValue(get, base, in)));
    }
    c.polyline(driven.data(), driven.size(), fx.alpha(0.35f), 1.0f);
    std::vector<Point> area = points;
    area.push_back(shapeAt(shape, 1.0, 0.0));
    area.push_back(shapeAt(shape, 0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);

    // The point under the mouse, in numbers (input to output), in the top left corner, which a curve
    // leaves empty.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char text[48];
        std::snprintf(text, sizeof(text), "%+.2f \xE2\x86\x92 %+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0,
                      value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text({r.x + 6.0f, r.y + 4.0f, r.w - 12.0f, 14.0f}, text, 10.0f, kDim, Align::Left);
    }
}

// The dry signal and the echoes on a timeline: the left side's above the line,
// the right side's below. Ping-pong echoes take turns, each after its side's time.
void View::paintDelay(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 16.0f, r.y + 20.0f, r.w - 22.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return value(paramIndex(slot, f)); };
    const bool synced = at(FxParam::DelaySync) >= 0.5;
    const bool linked = at(FxParam::DelayLink) >= 0.5;
    const double divisions[2] = {at(FxParam::DelayDivisionL), at(linked ? FxParam::DelayDivisionL : FxParam::DelayDivisionR)};
    const double times[2] = {at(FxParam::DelayTimeL), at(linked ? FxParam::DelayTimeL : FxParam::DelayTimeR)};
    double seconds[2];
    for (int side = 0; side < 2; ++side)
        seconds[side] = std::clamp(synced ? divisionSeconds(divisions[side], telemetry_.tempo) : timeFromNorm(times[side]),
                                   kMinTime, kMaxDelay);
    const double feedback = at(FxParam::DelayFeedback);
    const double mix = at(FxParam::DelayMix);
    const bool pingPong = at(FxParam::DelayPingPong) >= 0.5;
    const float width = static_cast<float>(at(FxParam::DelayWidth));
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    c.text({r.x + 4.0f, inner.y, 10.0f, 12.0f}, "L", 10.5f, kFaint);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 10.0f, 12.0f}, "R", 10.5f, kFaint);

    // Bars reach up for the left side and down for the right; narrower stereo
    // shows as each echo leaking onto the other side.
    auto bar = [&](double t, double level, int side, Color color) {
        const float x = inner.x + 2.0f + static_cast<float>(t) * (inner.w - 4.0f);
        const float h = static_cast<float>(std::min(level, 1.0)) * inner.h * 0.5f;
        const float own = h * (0.5f + 0.5f * width);
        const float other = h * (0.5f - 0.5f * width);
        const float up = side == 0 ? own : other;
        const float down = side == 0 ? other : own;
        c.fillRect({x - 1.5f, mid - up, 3.0f, up + down}, color, 1.5f);
    };
    bar(0.0, delayDryLevel(mix), 0, kDim);
    bar(0.0, delayDryLevel(mix), 1, kDim);
    const double span = 6.0 * std::max(seconds[0], seconds[1]);
    double level = delayWetLevel(mix);
    double pingPongTime = 0.0;
    for (int n = 1; n <= 16 && level > 0.01; ++n, level *= feedback) {
        auto echo = [&](int side, double t) {
            if (t <= span)
                bar(t / span, level, side, fx.alpha(0.35f + 0.65f * static_cast<float>(std::min(level, 1.0))));
        };
        if (pingPong) {
            const int side = (n - 1) % 2; // starts on the left, then crosses over
            pingPongTime += seconds[side];
            echo(side, pingPongTime);
        } else {
            echo(0, n * seconds[0]);
            echo(1, n * seconds[1]);
        }
    }

    // "1/8D | 1/4" or "375 ms | 500 ms"; linked, just "1/8D".
    char sides[2][32], text[80];
    for (int side = 0; side < 2; ++side) {
        if (synced)
            std::snprintf(sides[side], sizeof(sides[side]), "%s", divisionName(static_cast<int>(std::lround(divisions[side]))));
        else
            formatParam(paramIndex(slot, FxParam::DelayTimeL), normFromTime(seconds[side]), sides[side],
                        sizeof(sides[side]));
    }
    if (linked)
        std::snprintf(text, sizeof(text), "%s", sides[0]);
    else
        std::snprintf(text, sizeof(text), "%s  |  %s", sides[0], sides[1]);
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
    if (synced) {
        char bpm[16];
        std::snprintf(bpm, sizeof(bpm), "%.0f bpm", telemetry_.tempo);
        c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, bpm, 10.5f, kFaint, Align::Right);
    }
}

// Both bands' bells and what they add up to, on a log frequency axis.
Point View::peqHandle(Rect area, int slot, int band) const {
    const bool cut = isCut(peqType(value(peqTypeIndex(slot, band))));
    const double db = cut ? 20.0 * std::log10(qFromNorm(value(peqQIndex(slot, band)))) : value(peqGainIndex(slot, band));
    return peqAt(area, value(peqFreqIndex(slot, band)), db);
}

// What an effect or a voice filter puts out, measured, faintly behind its curves: 0 dB at the top down to -72 at
// the bottom. Once the audio stops (the snapshot gets old), it falls on at the analyzer's pace, 55 dB a second.
void View::paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum) {
    const float fall = 55.0f * std::max(telemetry_.age - 0.05f, 0.0f);
    if (fall >= 72.0f || std::none_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; }))
        return;
    std::vector<Point> points{{area.x, area.bottom()}};
    for (int band = 0; band < kSpectrumBands; ++band) {
        const float db = std::clamp(20.0f * std::log10(std::max(spectrum[band], 1e-6f)) - fall, -72.0f, 0.0f);
        points.push_back(
            {area.x + (band + 0.5f) / kSpectrumBands * area.w, area.bottom() - (db + 72.0f) / 72.0f * area.h});
    }
    points.push_back({area.right(), area.bottom()});
    c.fillPolygon(points.data(), points.size(), kText.alpha(0.1f));
    c.polyline(points.data() + 1, points.size() - 2, kDim.alpha(0.6f), 1.0f);
}

void View::paintParamEq(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect area = r.inset(8.0f);
    for (double db : {-12.0, 0.0, 12.0})
        c.line(peqAt(area, 0.0, db), peqAt(area, 1.0, db), kBorder.alpha(db == 0.0 ? 1.0f : 0.5f), 1.0f, db != 0.0);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = area.x + static_cast<float>(normFromCutoff(hz)) * area.w;
        c.line({x, area.y}, {x, area.bottom()}, kBorder.alpha(0.5f), 1.0f, true);
    }
    paintSpectrum(c, area, telemetry_.fxSpectrum[slot]);
    const Color bandColors[kPeqBands] = {kSourceColors[0], kSourceColors[1]};
    auto curve = [&](int only) { // only one band, or both (-1)
        std::vector<Point> points;
        for (int i = 0; i <= 120; ++i) {
            const double x = i / 120.0;
            double magnitude = 1.0;
            for (int band = 0; band < kPeqBands; ++band)
                if (only < 0 || only == band)
                    magnitude *= peqResponse(cutoffFromNorm(x), peqType(value(peqTypeIndex(slot, band))),
                                             value(peqFreqIndex(slot, band)), value(peqGainIndex(slot, band)),
                                             value(peqQIndex(slot, band)));
            points.push_back(peqAt(area, x, 20.0 * std::log10(std::max(magnitude, 1e-6))));
        }
        return points;
    };
    // A cut falls far below the display, so the curves are cut off at its edge.
    c.pushClip(r);
    for (int band = 0; band < kPeqBands; ++band) {
        const std::vector<Point> points = curve(band);
        c.polyline(points.data(), points.size(), bandColors[band].alpha(0.35f), 1.0f);
    }
    std::vector<Point> points = curve(-1);
    std::vector<Point> area0 = points;
    area0.push_back(peqAt(area, 1.0, 0.0));
    area0.push_back(peqAt(area, 0.0, 0.0));
    c.fillPolygon(area0.data(), area0.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    c.popClip();
}

// The EQ's response, with the crossovers marked.
// A name down a folded card's strip, a letter a line, from the top of `r`.
void View::paintDownward(Canvas &c, Rect r, std::string_view name, Color color) {
    for (size_t i = 0; i < name.size() && (i + 1) * 13.0f <= r.h; ++i)
        if (name[i] != ' ')
            c.text({r.x, r.y + i * 13.0f, r.w, 13.0f}, std::string(1, name[i]), 10.5f, color, Align::Center, true);
}

void View::paintEq(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 8.0f, r.w - 16.0f, r.h - 24.0f};
    constexpr float kMinDb = -30.0f;
    constexpr float kMaxDb = 9.0f;
    auto yOfDb = [&](float db) {
        return inner.bottom() - (std::clamp(db, kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb) * inner.h;
    };
    auto xOfHz = [&](double hz) { return inner.x + static_cast<float>(normFromCutoff(hz)) * inner.w; };
    c.line({inner.x, yOfDb(0.0f)}, {inner.right(), yOfDb(0.0f)}, kBorder, 1.0f);
    for (double hz : {kEqLowHz, kEqHighHz})
        c.line({xOfHz(hz), inner.y}, {xOfHz(hz), inner.bottom()}, kBorder.alpha(0.8f), 1.0f, true);

    const double low = value(paramIndex(slot, FxParam::EqLow));
    const double mid = value(paramIndex(slot, FxParam::EqMid));
    const double high = value(paramIndex(slot, FxParam::EqHigh));
    std::vector<Point> points;
    for (int i = 0; i <= 120; ++i) {
        const double x = i / 120.0;
        const double magnitude = eqResponse(cutoffFromNorm(x), low, mid, high);
        const float db = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-6)));
        points.push_back({inner.x + static_cast<float>(x) * inner.w, yOfDb(db)});
    }
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);

    const float labelY = inner.bottom() + 2.0f;
    const float edges[] = {inner.x, xOfHz(kEqLowHz), xOfHz(kEqHighHz), inner.right()};
    for (int b = 0; b < 3; ++b)
        c.text({edges[b], labelY, edges[b + 1] - edges[b], 12.0f}, tr(kMcBandNames[b]), 10.5f, kFaint, Align::Center,
               true);
}

// Static curves, input level across and output up, from -60 dB to 0, with the
// gain reduction as a meter on the right; the three-band one draws a curve per
// band, the one its card shows on top.
namespace {

constexpr float kCompRangeDb = 60.0f;
constexpr float kMeterRangeDb = 24.0f;

void compressorPlot(Canvas &c, Rect plot, double threshold, double ratio, double knee, Color color, float width) {
    std::vector<Point> points;
    for (int i = 0; i <= 60; ++i) {
        const double in = -kCompRangeDb + i * kCompRangeDb / 60.0;
        const double out = std::max(compressorCurve(in, threshold, ratio, knee), static_cast<double>(-kCompRangeDb));
        points.push_back({plot.x + static_cast<float>((in + kCompRangeDb) / kCompRangeDb) * plot.w,
                          plot.bottom() - static_cast<float>((out + kCompRangeDb) / kCompRangeDb) * plot.h});
    }
    if (width > 1.0f) // the curve shown, rather than a faint one behind it
        plotLine(c, points.data(), points.size(), color, width);
    else
        c.polyline(points.data(), points.size(), color, width);
}

// A downward bar for `db` of gain reduction.
void reductionMeter(Canvas &c, Rect meter, float db, Color color) {
    c.fillRect(meter, kCardColor, 2.0f);
    const float h = std::clamp(db / kMeterRangeDb, 0.0f, 1.0f) * meter.h;
    if (h > 0.5f)
        c.fillRect({meter.x, meter.y, meter.w, h}, color, 2.0f);
}

} // namespace

void View::paintCompressor(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect plot{r.x + 8.0f, r.y + 8.0f, r.w - 34.0f, r.h - 16.0f};
    c.line({plot.x, plot.bottom()}, {plot.right(), plot.y}, kBorder, 1.0f, true); // no compression
    const double threshold = value(paramIndex(slot, FxParam::CompThreshold));
    const float tx = plot.x + static_cast<float>((threshold + kCompRangeDb) / kCompRangeDb) * plot.w;
    c.line({tx, plot.y}, {tx, plot.bottom()}, fx.alpha(0.3f), 1.0f, true);
    compressorPlot(c, plot, threshold, value(paramIndex(slot, FxParam::CompRatio)),
                   value(paramIndex(slot, FxParam::CompKnee)), fx, 2.0f);
    const float reduction = telemetry_.fxReduction[slot][0];
    reductionMeter(c, {r.right() - 18.0f, plot.y, 8.0f, plot.h}, reduction, fx);
    c.text({plot.x + 4.0f, plot.y + 2.0f, plot.w, 14.0f}, trf(Str::CaptionReduction, {number("%.1f", reduction)}),
           10.5f, reduction > 0.05f ? kText : kFaint, Align::Left);
}

void View::paintMultiComp(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect plot{r.x + 8.0f, r.y + 6.0f, r.w - 50.0f, r.h - 12.0f};
    c.line({plot.x, plot.bottom()}, {plot.right(), plot.y}, kBorder, 1.0f, true);
    // Linked, the bands share one curve; the meters still show each band's reduction.
    const bool linked = value(paramIndex(slot, FxParam::McLink)) >= 0.5;
    const int shown = linked ? 0 : fxBand_[slot];
    for (int pass = 0; pass < 2; ++pass) { // the other bands first, faint, then the one shown
        for (int band = 0; band < kMcBands; ++band) {
            if ((band == shown) != (pass == 1) || (linked && band != shown))
                continue;
            compressorPlot(c, plot, value(mcThresholdIndex(slot, band)), value(mcRatioIndex(slot, band)), kMcKnee,
                           band == shown ? fx : fx.alpha(0.3f), band == shown ? 2.0f : 1.0f);
        }
    }
    float most = 0.0f;
    for (int band = 0; band < kMcBands; ++band) {
        const Rect meter{r.right() - 36.0f + band * 10.0f, plot.y, 7.0f, plot.h};
        const float reduction = telemetry_.fxReduction[slot][band];
        most = std::max(most, reduction);
        reductionMeter(c, meter, reduction, band == shown || linked ? fx : fx.alpha(0.45f));
    }
    const std::string text =
        linked ? trf(Str::CaptionLinkedReduction, {number("%.1f", most)})
               : trf(Str::CaptionBandReduction,
                     {tr(kMcBandNames[shown]), number("%.1f", telemetry_.fxReduction[slot][shown])});
    c.text({plot.x + 4.0f, plot.y + 1.0f, plot.w, 14.0f}, text, 10.5f, kDim, Align::Left);
}

// The tail's level in dB across the height, so it falls in a straight line to nothing at the decay
// time, after the pre-delay; the dry sound stands at the start. Their heights follow the mix.
// The reverb in time: the dry sound at the start, then after the pre-delay a build-up to the full tail, which
// dies away over the decay. A bigger space builds up more slowly (shown stretched, so Size reads), with its
// early reflections spread wider, and its tail is sparser. A room's early reflections are distinct, a plate
// is dense from the start with none, and a shimmer's tail sparkles above it.
void View::paintReverb(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return value(paramIndex(slot, f)); };
    const ReverbType type = reverbType(at(FxParam::ReverbType));
    const double space = reverbSpace(at(FxParam::ReverbSize), type); // the lines' length, 0.11 to 1
    const double decay = reverbDecay(at(FxParam::ReverbDecay));
    const double pre = reverbPreDelay(at(FxParam::ReverbPreDelay));
    const double build = decay * (type == ReverbType::Plate ? 0.02 : 0.03 + 0.2 * space);
    const double span = 1.15 * (pre + decay);
    const float wet = static_cast<float>(delayWetLevel(at(FxParam::ReverbMix)));
    const float full = wet * 0.5f * inner.h;
    auto x = [&](double seconds) { return inner.x + static_cast<float>(seconds / span) * inner.w; };
    auto level = [&](double t) { // 0..1: rising over the build-up, then falling to nothing at the decay
        const double since = t - pre;
        if (since <= 0.0)
            return 0.0f;
        if (since < build)
            return static_cast<float>(since / build);
        return static_cast<float>(std::max(0.0, 1.0 - (since - build) / std::max(decay - build, 1e-3)));
    };
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    // A made-up texture of reflections under the outline: sparser in a bigger space, densest in a plate, and
    // darker with damping.
    const float damping = static_cast<float>(at(FxParam::ReverbDamping));
    const int count = type == ReverbType::Plate ? 120 : static_cast<int>(std::lround(130.0 - 90.0 * space));
    uint32_t rng = 0x9E3779B9u;
    auto random = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    for (int i = 0; i < count; ++i) {
        const double t = pre + build * 0.4 + (decay - build * 0.4) * i / count;
        const float h = level(t) * full * (0.35f + 0.65f * random());
        c.line({x(t), mid - h}, {x(t), mid + h}, fx.alpha(0.55f - 0.3f * damping), 1.0f);
    }
    // The early reflections over the build-up, as far apart as the room's (the others' first echoes from
    // their lines, fainter).
    if (type != ReverbType::Plate) {
        const double last = kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1];
        for (size_t e = 0; e < std::size(kRoomEchoSeconds); ++e) {
            const double t = pre + build * 0.9 * kRoomEchoSeconds[e] / last;
            const float h = full * static_cast<float>(kRoomEchoLevels[e]) * (type == ReverbType::Room ? 1.5f : 0.8f);
            c.line({x(t), mid - h}, {x(t), mid + h}, fx.alpha(0.9f), 1.5f);
        }
    }
    // A shimmer's shifted tail in sparks off the outline, above it for a shift up and below it for one down: each
    // step of the interval later, further out and fainter, the more so the less it shifts. A quarter of the lines
    // shift, so a step takes about four rounds of them to come in.
    const double interval = at(FxParam::ReverbShimmerPitch);
    const float amount = static_cast<float>(at(FxParam::ReverbShimmer));
    if (type == ReverbType::Shimmer && std::abs(interval) >= 0.5 && amount > 0.0f) {
        const double round = 4.0 * (kReverbLongest * space + 0.5 * kShimmerWindow);
        const float side = interval > 0.0 ? -1.0f : 1.0f;
        for (int step = 1; step <= 3; ++step) {
            const double from = pre + step * round;
            const float strength = std::pow(amount, 0.5f * static_cast<float>(step));
            for (int k = 0; k < 14; ++k) {
                const double t = from + (pre + decay - from) * random();
                const float y = mid + side * (level(t) * full * (0.6f + 0.4f * strength) + 4.0f * step + 3.0f * random());
                c.fillCircle({x(t), std::clamp(y, inner.y, inner.bottom())}, 1.2f, fx.alpha(0.3f + 0.6f * strength));
            }
        }
    }
    const Point outline[] = {{x(pre), mid}, {x(pre + build), mid - full}, {x(pre + decay), mid},
                             {x(pre + build), mid + full}, {x(pre), mid}};
    plotLine(c, outline, 5, fx, 1.5f);
    const float dry = static_cast<float>(delayDryLevel(at(FxParam::ReverbMix))) * 0.5f * inner.h;
    c.fillRect({inner.x - 1.0f, mid - dry, 3.0f, 2.0f * dry}, kDim, 1.5f);

    char decayText[32], preText[32];
    formatParam(paramIndex(slot, FxParam::ReverbDecay), at(FxParam::ReverbDecay), decayText, sizeof(decayText));
    formatParam(paramIndex(slot, FxParam::ReverbPreDelay), at(FxParam::ReverbPreDelay), preText, sizeof(preText));
    std::string text = pre > 0.0 ? trf(Str::CaptionPreDelay, {decayText, preText}) : std::string(decayText);
    if (type == ReverbType::Shimmer) { // the interval it climbs by (or falls) in place of the pre-delay
        char pitchText[32];
        formatParam(paramIndex(slot, FxParam::ReverbShimmerPitch), interval, pitchText, sizeof(pitchText));
        text = trf(Str::CaptionShimmer, {decayText, pitchText});
    }
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
}

void View::paintSweep(Canvas &c, Rect r, int slot, FxType type) {
    const Color fx = fxColor(slot);
    const bool phaser = type == FxType::Phaser;
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 28.0f};
    auto at = [&](FxParam f) { return value(paramIndex(slot, f)); };
    const double feedback = at(phaser ? FxParam::PhaserFeedback : FxParam::FlangerFeedback);
    const double mix = sweepWet(at(phaser ? FxParam::PhaserMix : FxParam::FlangerMix));
    const double depth = at(phaser ? FxParam::PhaserDepth : FxParam::FlangerDepth);
    const int stages = static_cast<int>(std::lround(at(FxParam::PhaserStages)));
    constexpr float kMinDb = -30.0f, kMaxDb = 9.0f;
    auto yOfDb = [&](double db) {
        return inner.bottom() - (std::clamp(static_cast<float>(db), kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb) * inner.h;
    };
    c.line({inner.x, yOfDb(0.0)}, {inner.right(), yOfDb(0.0)}, kBorder, 1.0f);
    // The response at both ends of the sweep faintly, and bright where its LFO is now (moved on from the
    // last snapshot at its rate), or in the middle while no audio runs. A stereo sweep's right side, a
    // quarter cycle ahead, is the thinner of the two bright curves.
    const bool running = telemetry_.age < 0.5f;
    const double rateHz = sweepHz([this](int i) { return value(i); }, slot, phaser, telemetry_.tempo);
    const double phase = telemetry_.fxPhases[slot] + rateHz * std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    const bool stereo = at(phaser ? FxParam::PhaserStereo : FxParam::FlangerStereo) >= 0.5;
    struct Curve {
        double lfo;
        float alpha, width;
    };
    std::vector<Curve> curves = {{-1.0, 0.3f, 1.0f}, {1.0, 0.3f, 1.0f}};
    if (stereo && running)
        curves.push_back({std::sin(2.0 * std::numbers::pi * (phase + 0.25)), 0.7f, 1.2f});
    curves.push_back({running ? std::sin(2.0 * std::numbers::pi * phase) : 0.0, 1.0f, 1.8f});
    for (const Curve &curve : curves) {
        std::vector<Point> points;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0;
            const double hz = cutoffFromNorm(x);
            const double lfo = curve.lfo;
            const double magnitude =
                phaser ? phaserResponse(hz, phaserCorner(at(FxParam::PhaserCenter), depth, lfo), stages, feedback, mix)
                       : flangerResponse(hz, flangerDelay(at(FxParam::FlangerDelay), depth, lfo), feedback, mix);
            points.push_back({inner.x + static_cast<float>(x) * inner.w,
                              yOfDb(20.0 * std::log10(std::max(magnitude, 1e-6)))});
        }
        if (curve.width > 1.5f)
            plotLine(c, points.data(), points.size(), fx, curve.width);
        else
            c.polyline(points.data(), points.size(), fx.alpha(curve.alpha), curve.width);
    }
    char rate[32], shape[32], text[80];
    const bool synced = at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync) >= 0.5;
    const int rateParam = paramIndex(slot, synced ? (phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision)
                                                  : (phaser ? FxParam::PhaserRate : FxParam::FlangerRate));
    formatParam(rateParam, value(rateParam), rate, sizeof(rate));
    if (phaser)
        std::snprintf(shape, sizeof(shape), "%s", trf(Str::CaptionStages, {std::to_string(stages)}).c_str());
    else
        formatParam(paramIndex(slot, FxParam::FlangerDelay), at(FxParam::FlangerDelay), shape, sizeof(shape));
    std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", shape, rate);
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
}

// The partials of a 220 Hz tone, faint, and where the shifter moves them: the left side above the
// line, the right below, on a log axis from 50 Hz to 10 kHz.
// The disperser's delay across frequency: how much later than the rest each part of the sound comes out,
// rising to its peak at Freq, taller and narrower with more stages and more pinch. Behind it, on the same axis, what
// it puts out, measured.
void View::paintDisperser(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 28.0f};
    auto at = [&](FxParam f) { return value(paramIndex(slot, f)); };
    const int stages = static_cast<int>(std::lround(at(FxParam::DisperserStages)));
    constexpr double kRate = 48000.0;
    std::vector<double> delays;
    double most = 1e-4;
    for (int i = 0; i <= 160; ++i) {
        delays.push_back(disperserDelay(cutoffFromNorm(i / 160.0), at(FxParam::DisperserFreq),
                                        at(FxParam::DisperserPinch), stages, kRate));
        most = std::max(most, delays.back());
    }
    std::vector<Point> points;
    for (int i = 0; i <= 160; ++i)
        points.push_back({inner.x + inner.w * i / 160.0f,
                          inner.bottom() - static_cast<float>(delays[i] / (most * 1.1)) * inner.h});
    paintSpectrum(c, inner, telemetry_.fxSpectrum[slot]);
    c.line({inner.x, inner.bottom()}, {inner.right(), inner.bottom()}, kBorder, 1.0f);
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f * static_cast<float>(at(FxParam::DisperserMix)) + 0.04f));
    plotLine(c, points.data(), points.size(), fx, 1.8f);
    char freq[32], text[80];
    formatParam(paramIndex(slot, FxParam::DisperserFreq), at(FxParam::DisperserFreq), freq, sizeof(freq));
    std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionDispersion, {freq, number("%.1f", most * 1000.0)}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
}

// The widener's picture of the stereo field: the sound's place across, left to right, and its frequency up, from
// 20 Hz at the bottom to 20 kHz at the top. The dry sound stays in the middle; above the high-pass, the taps
// spread it out to the sides as far as Width and Mix take it, wavering with their sweep while the audio runs.
// What Width leaves in the middle, a chorus, thickens the middle line.
void View::paintWidener(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 18.0f, r.y + 20.0f, r.w - 36.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return value(paramIndex(slot, f)); };
    const double size = at(FxParam::WidenerSize), width = at(FxParam::WidenerWidth), mix = at(FxParam::WidenerMix);
    const double highPass = at(FxParam::WidenerHighPass);
    const float mid = inner.center().x;
    auto yOf = [&](double norm) { return inner.bottom() - static_cast<float>(norm) * inner.h; };
    const bool running = telemetry_.age < 0.5f;
    const double phase =
        telemetry_.fxPhases[slot] + kWidenerRates[0] * std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    std::vector<Point> left, right, chorus;
    for (int i = 0; i <= 64; ++i) {
        const double y = i / 64.0;
        const double spread = widenerSpread(cutoffFromNorm(y), highPass);
        const double waver = running ? 1.0 + 0.08 * (0.3 + size) * std::sin(2.0 * std::numbers::pi * (phase + 2.5 * y))
                                     : 1.0;
        // On a square-root curve of the mix, so half of it already shows wide.
        const float half = 0.5f * inner.w * static_cast<float>(std::min(spread * width * std::sqrt(mix) * waver, 1.0));
        left.push_back({mid - half, yOf(y)});
        right.push_back({mid + half, yOf(y)});
        chorus.push_back({mid + 0.5f + 5.0f * static_cast<float>(spread * (1.0 - width) * std::sqrt(mix)), yOf(y)});
    }
    std::vector<Point> area = left;
    area.insert(area.end(), right.rbegin(), right.rend());
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.16f));
    std::vector<Point> thick = chorus;
    for (auto it = chorus.rbegin(); it != chorus.rend(); ++it)
        thick.push_back({2.0f * mid - it->x, it->y});
    c.fillPolygon(thick.data(), thick.size(), fx.alpha(0.5f));
    c.line({mid, inner.y}, {mid, inner.bottom()}, kDim, 1.0f);
    plotLine(c, left.data(), left.size(), fx, 1.6f);
    plotLine(c, right.data(), right.size(), fx, 1.6f);
    // Where the high-pass is: below it the sound stays as it was, in the middle.
    const float corner = yOf(highPass);
    c.line({inner.x, corner}, {inner.right(), corner}, kFaint, 1.0f, true);

    char hp[32], text[80];
    formatParam(paramIndex(slot, FxParam::WidenerHighPass), highPass, hp, sizeof(hp));
    const double shortest = widenerTap(0, size, 0.0) * 1000.0, longest = widenerTap(kWidenerTaps - 1, size, 0.0) * 1000.0;
    const char *const digits = longest < 10.0 ? "%.1f" : "%.0f";
    std::snprintf(text, sizeof(text), "%s",
                  trf(Str::CaptionWidener, {hp, number(digits, shortest), number(digits, longest)}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 12.0f, 12.0f}, "L", 10.5f, kFaint);
    c.text({r.right() - 16.0f, inner.bottom() - 12.0f, 12.0f, 12.0f}, "R", 10.5f, kFaint);
}

void View::paintNoteSource(Canvas &c, float top) {
    // The note the newest voice plays, as the note source follows it (bend and glide included): its name, and
    // how many cents off it is; a dash while nothing plays.
    const Rect box = noteDisplay(top);
    const Color color = kSourceColors[kNoteSource];
    const bool lit = (hover_ && hover_->kind == Kind::NoteDisplay && drag_.kind == DragKind::None) ||
                     shapeEditing() == kNoteCurve; // it opens the curve, or has
    c.fillRect(box, lit ? kTrack : kDisplayColor, 5.0f);
    c.strokeRect(box, color.alpha(lit ? 0.9f : 0.45f), 1.0f, 5.0f);
    // Behind the note, faintly, its curve across the box.
    auto get = [this](int index) { return value(index); };
    std::vector<Point> curve;
    for (int i = 0; i <= 24; ++i) {
        const double x = i / 24.0;
        const double y = clipCurveValue(get, paramIndex(NoteParam::Curve), 2.0 * x - 1.0);
        curve.push_back({box.x + 4.0f + static_cast<float>(x) * (box.w - 8.0f),
                         box.bottom() - 4.0f - static_cast<float>(0.5 * (y + 1.0)) * (box.h - 8.0f)});
    }
    c.polyline(curve.data(), curve.size(), color.alpha(0.3f), 1.2f);
    std::string text = "\xE2\x80\x93";
    if (telemetry_.voices > 0) {
        const double pitch = telemetry_.notePitch;
        const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
        const long cents = std::lround((pitch - key) * 100.0);
        text = noteName(key);
        if (cents != 0)
            text += (cents > 0 ? " +" : " ") + std::to_string(cents);
    }
    c.text(box, text, 11.0f, telemetry_.voices > 0 ? kText : kFaint, Align::Center, telemetry_.voices > 0);
    c.text({box.x - 14.0f, top + kStripRow + 19.0f, box.w + 28.0f, 16.0f}, tr(Str::LabelNote), 10.5f, kDim);
    if (sourceBipolar(kNoteSource)) // swings both ways: a sign in its corner
        c.text({box.right() - 13.0f, box.y + 1.0f, 11.0f, 11.0f}, "\xC2\xB1", 10.0f, color, Align::Center, true);
}

void View::paintShifter(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 26.0f};
    const double hz =
        shiftHz(value(paramIndex(slot, FxParam::ShiftAmount)), value(paramIndex(slot, FxParam::ShiftRange)));
    const double spread = value(paramIndex(slot, FxParam::ShiftSpread));
    const double shifts[2] = {hz, hz * (1.0 - 2.0 * spread)};
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    auto x = [&](double f) { return inner.x + static_cast<float>(std::log(f / 50.0) / std::log(200.0)) * inner.w; };
    for (int k = 1; k <= 12; ++k) {
        const float h = (0.5f * inner.h - 2.0f) / std::sqrt(static_cast<float>(k));
        const double f = 220.0 * k;
        c.line({x(f), mid - h}, {x(f), mid + h}, kFaint.alpha(0.6f), 1.0f);
        for (int side = 0; side < 2; ++side) {
            const double moved = f + shifts[side];
            if (moved < 50.0 || moved > 10000.0)
                continue; // shifted below nothing, or off the end
            const float tip = side == 0 ? mid - h : mid + h;
            c.line({x(moved), mid}, {x(moved), tip}, fx, 1.6f);
        }
    }
    // The shift in Hz, and the range it is a share of.
    char shift[32], range[32], text[80];
    if (std::abs(hz) < 0.005)
        std::snprintf(shift, sizeof(shift), "0 Hz");
    else if (std::abs(hz) < 10.0)
        std::snprintf(shift, sizeof(shift), "%+.2f Hz", hz);
    else if (std::abs(hz) < 1000.0)
        std::snprintf(shift, sizeof(shift), "%+.1f Hz", hz);
    else
        std::snprintf(shift, sizeof(shift), "%+.2f kHz", hz / 1000.0);
    formatParam(paramIndex(slot, FxParam::ShiftRange), value(paramIndex(slot, FxParam::ShiftRange)), range,
                sizeof(range));
    std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionShiftOf, {shift, range}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, 10.5f, kDim, Align::Left);
    c.text({r.x + 4.0f, inner.y, 10.0f, 12.0f}, "L", 10.5f, kFaint);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 10.0f, 12.0f}, "R", 10.5f, kFaint);
}

void View::paintInspector(Canvas &c) {
    const int node = controller_.layout().selectedNode;
    // Leaves room for the on/off switch at the right.
    const Rect summaryRect{kInspector.x + 16.0f, kInspector.y + 38.0f, kInspector.w - 84.0f, 18.0f};
    const char *dot = "  \xC2\xB7  ";

    if (isShaperNode(node)) {
        paintShaperPage(c, shaperOfNode(node), summaryRect);
        return;
    }
    if (node < kOps) {
        const int op = node;
        const double ratio = coarseRatio(value(paramIndex(op, OpParam::Coarse))) *
                             (1.0 + value(paramIndex(op, OpParam::Fine)));
        std::string summary = std::string(kNodeNames[op]) + dot;
        if (isNoise(op)) {
            char slope[32];
            const int slopeParam = paramIndex(op, OpParam::NoiseSlope);
            formatParam(slopeParam, value(slopeParam), slope, sizeof(slope));
            summary += waveName(OpWave::Noise) + (dot + std::string(slope));
        } else {
            summary += waveName(static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))))) +
                       (dot + number("%.2f", ratio));
        }
        const long copies = std::lround(value(paramIndex(op, OpParam::Unison)));
        if (copies > 1 && !isNoise(op))
            summary += dot + trf(Str::SummaryUnison, {std::to_string(copies)});
        std::string feeds;
        if (value(paramIndex(op, OpParam::Out)) > 0.0)
            feeds = tr(Str::SummaryOut);
        for (int to = 0; to < kNodes; ++to)
            if (to != op && nodeOn(to) && value(routeParam(op, to)) > 0.0)
                feeds += (feeds.empty() ? "" : ", ") + std::string(kNodeNames[to]);
        if (!feeds.empty())
            summary += dot + trf(Str::SummaryTo, {feeds});
        c.text(summaryRect, summary, 11.0f, kDim, Align::Left);
        if (isNoise(op))
            paintNoiseSlope(c, kNoiseRect, op);
        else
            paintUnison(c, kUnisonRect, op);
        paintEnvelope(c, kOpEnvRect, op, kAccent, false);
        return;
    }

    const int filter = node - kOps;
    const auto type = static_cast<FilterType>(std::lround(value(paramIndex(filter, FilterParam::Type))));
    std::string summary = std::string(kNodeNames[node]) + dot + filterTypeName(type);
    if (hasSlope(type))
        summary += " " + std::to_string(slopeDb(playedSlope(type, filterSlope(value(paramIndex(filter, FilterParam::Slope)))))) +
                   " dB";
    c.text(summaryRect, summary + boxRoutes(node), 11.0f, kDim, Align::Left);
    // A voice filter tracks the newest voice's note; with none, the curve is back at the knob's C4.
    paintFilterResponse(c, kFilterDisplay, paramIndex(filter, FilterParam::Type),
                        paramIndex(filter, FilterParam::Slope), paramIndex(filter, FilterParam::Cutoff),
                        paramIndex(filter, FilterParam::Resonance), paramIndex(filter, FilterParam::KeyTrack),
                        telemetry_.voices > 0 ? telemetry_.notePitch : 60.0, kFilterColor,
                        &telemetry_.filterSpectrum[filter]);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), 10.5f, kFaint,
           Align::Left, true);
}

// A filter's or shaper's routes, for its page's summary: what goes into it, the filters and shapers it feeds and the
// output, and the operators it modulates.
std::string View::boxRoutes(int node) const {
    const std::string dot = "  \xC2\xB7  ";
    std::string from, to, mods;
    auto name = [](std::string &list, const char *node) { list += (list.empty() ? "" : ", ") + std::string(node); };
    for (int other = 0; other < kNodes; ++other) {
        if (other == node || !nodeOn(other))
            continue;
        if (const int in = routeParam(other, node); in >= 0 && value(in) > 0.0)
            name(from, kNodeNames[other]);
        if (const int out = routeParam(node, other); out >= 0 && value(out) > 0.0)
            name(other < kOps ? mods : to, kNodeNames[other]);
    }
    if (value(outParam(node)) > 0.0)
        name(to, tr(Str::SummaryOut));
    std::string text = dot + (from.empty() ? std::string(tr(Str::SummaryNoInput)) : trf(Str::SummaryFrom, {from}));
    if (!to.empty())
        text += dot + trf(Str::SummaryTo, {to});
    if (!mods.empty())
        text += dot + trf(Str::SummaryMods, {mods});
    return text;
}

void View::paintShaperPage(Canvas &c, int shaper, Rect summaryRect) {
    const std::string summary = std::string(kNodeNames[kOps + kFilters + shaper]) + "  \xC2\xB7  " +
                                clipModeName(clipMode(value(paramIndex(shaper, ShaperParam::Mode))));
    c.text(summaryRect, summary + boxRoutes(kOps + kFilters + shaper), 11.0f, kDim, Align::Left);
    c.fillRect(kShaperDisplay, kDisplayColor, 6.0f);
    paintShaperCurve(c, kShaperDisplay, kFxSlots + shaper);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), 10.5f, kFaint,
           Align::Left, true);
}

// Where the unison copies sit: left to right by stereo position, up and down by detune.
void View::paintUnison(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, 6.0f);
    c.line({r.center().x, r.y + 6.0f}, {r.center().x, r.bottom() - 6.0f}, kBorder, 1.0f);
    c.text({r.x + 4.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "L", 10.5f, kFaint);
    c.text({r.right() - 16.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "R", 10.5f, kFaint);
    const int copies = std::clamp(static_cast<int>(std::lround(value(paramIndex(op, OpParam::Unison)))), 1, kMaxUnison);
    const double detune = value(paramIndex(op, OpParam::UnisonDetune)) / 100.0;
    const double spread = value(paramIndex(op, OpParam::UnisonSpread));
    for (int k = 0; k < copies; ++k) {
        const double offset = copies > 1 ? 2.0 * k / (copies - 1) - 1.0 : 0.0;
        const Point dot{r.center().x + static_cast<float>(offset * spread) * (r.w * 0.5f - 10.0f),
                        r.center().y - static_cast<float>(offset * detune) * (r.h * 0.5f - 7.0f)};
        c.fillCircle(dot, 3.0f, copies > 1 ? kAccent : kDim);
    }
}

// The noise's spectrum from 20 Hz to 20 kHz, over faint white, pink and brown slopes.
void View::paintNoiseSlope(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, 6.0f);
    const Rect inner{r.x + 10.0f, r.y + 8.0f, r.w - 56.0f, r.h - 16.0f};
    // Every slope starts at the top left, at 20 Hz; brown falls the full height by 20 kHz.
    auto line = [&](double slope, Color color, float width) {
        c.line({inner.x, inner.y}, {inner.right(), inner.y + static_cast<float>(slope) * inner.h}, color, width);
    };
    const Str names[] = {Str::NoiseWhiteCaption, Str::NoisePinkCaption, Str::NoiseBrownCaption};
    for (int i = 0; i < 3; ++i) {
        line(i * 0.5, kBorder, 1.0f);
        const float y = inner.y + i * 0.5f * inner.h;
        c.text({inner.right() + 6.0f, y - 6.0f, 40.0f, 12.0f}, tr(names[i]), 10.5f, kFaint, Align::Left);
    }
    const float drop = static_cast<float>(value(paramIndex(op, OpParam::NoiseSlope))) * inner.h;
    const Point slope[] = {{inner.x, inner.y}, {inner.right(), inner.y + drop}};
    plotLine(c, slope, 2, kAccent, 2.0f);
}

void View::paintFilterResponse(Canvas &c, Rect r, int typeParam, int slopeParam, int cutoffParam,
                               int resonanceParam, int keyTrackParam, double pitch, Color color,
                               const Spectrum *spectrum) {
    c.fillRect(r, kDisplayColor, 6.0f);
    const Rect inner{r.x + 10.0f, r.y + 10.0f, r.w - 20.0f, r.h - 20.0f};
    if (spectrum) // what the filter puts out, behind its response
        paintSpectrum(c, inner, *spectrum);
    auto yOfDb = [&](float db) {
        const float t = (std::clamp(db, kResponseMinDb, kResponseMaxDb) - kResponseMinDb) /
                        (kResponseMaxDb - kResponseMinDb);
        return inner.bottom() - t * inner.h;
    };
    c.line({inner.x, yOfDb(0.0f)}, {inner.right(), yOfDb(0.0f)}, kBorder, 1.0f); // 0 dB

    const auto type = static_cast<FilterType>(std::lround(value(typeParam)));
    const double cutoff = value(cutoffParam);
    const double resonance = value(resonanceParam);
    const int steps = type == FilterType::Comb ? 240 : 96; // comb teeth need more points
    auto response = [&](double cutoffNorm, double res) {
        std::vector<Point> points;
        for (int i = 0; i <= steps; ++i) {
            const double x = static_cast<double>(i) / steps;
            const double magnitude =
                filterResponse(type, filterSlope(value(slopeParam)), cutoffFromNorm(cutoffNorm), res, cutoffFromNorm(x));
            const float db = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-6)));
            points.push_back({inner.x + static_cast<float>(x) * inner.w, yOfDb(db)});
        }
        return points;
    };
    // Modulated, the curve shows how far its modulation takes it either way, faintly (as a knob's ring
    // does), and while notes play it follows where the newest voice is. Key tracking moves all of it with
    // `pitch` from C4 on, an octave per octave at 100 %, as far as the filter's cutoff goes (down to 10 Hz).
    const double keyShift = value(keyTrackParam) * (pitch - 60.0) / 12.0 / std::log2(kMaxCutoff / kMinCutoff);
    auto tracked = [&](double cutoffNorm) { return std::clamp(cutoffNorm + keyShift, normFromCutoff(10.0), 1.0); };
    const std::vector<Assignment> mods = assignments();
    auto reach = [&](int param, double set) {
        double low = set, high = set;
        for (const Assignment &m : mods) {
            if (m.target != param || m.bypassed || !sourceOn(m.source))
                continue;
            const auto [down, up] = modReach(m.source, m.amount);
            low += down;
            high += up;
        }
        return std::pair{std::clamp(low, 0.0, 1.0), std::clamp(high, 0.0, 1.0)};
    };
    const auto [cutoffLow, cutoffHigh] = reach(cutoffParam, cutoff);
    const auto [resonanceLow, resonanceHigh] = reach(resonanceParam, resonance);
    if (cutoffLow != cutoffHigh || resonanceLow != resonanceHigh) {
        for (const auto &[at, res] : {std::pair{cutoffLow, resonanceLow}, std::pair{cutoffHigh, resonanceHigh}}) {
            const std::vector<Point> edge = response(tracked(at), res);
            c.polyline(edge.data(), edge.size(), color.alpha(0.3f), 1.0f);
        }
    }
    const std::vector<Point> points = response(tracked(liveAmount(cutoffParam)), liveAmount(resonanceParam));
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    const float cutoffX = inner.x + static_cast<float>(cutoff) * inner.w;
    c.line({cutoffX, inner.y}, {cutoffX, inner.bottom()}, color.alpha(0.3f), 1.0f, true);
}

void View::paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live) {
    c.fillRect(r, kDisplayColor, 6.0f);
    for (int i = 1; i < 4; ++i)
        c.line({r.x + 8.0f, r.y + r.h * i / 4.0f}, {r.right() - 8.0f, r.y + r.h * i / 4.0f}, kBorder.alpha(0.6f), 1.0f);

    const EnvIndices e = envIndices(env);
    const double sustain = value(e.sustain);
    const EnvShape s = envShape(r, e, [this](int index) { return value(index); }, envScale(env));
    const float height = s.bottom - s.top;
    std::vector<Point> points{s.p0};
    for (int segment = 0; segment < 3; ++segment) {
        if (segment == 1)
            points.push_back(s.ph); // the end of the hold
        if (segment == 2)
            points.push_back(s.p3); // the end of the sustain hold
        for (int i = 1; i <= 24; ++i)
            points.push_back(envSegmentPoint(s, segment, i / 24.0, value(e.curve(segment)), sustain));
    }

    std::vector<Point> area = points;
    area.push_back({s.p4.x, s.bottom});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);

    if (live && telemetry_.voices > 0 && sourceOn(env - kOps)) {
        const float level = telemetry_.sources[env - kOps];
        const float y = s.bottom - level * height;
        c.line({r.x + 8.0f, y}, {r.right() - 8.0f, y}, color.alpha(0.35f), 1.0f, true);
    }
    // A bipolar envelope swings its targets about its middle level: a line there, and a sign in the corner.
    if (env >= kOps && sourceBipolar(env - kOps)) {
        const float middle = s.bottom - 0.5f * height;
        c.line({r.x + 8.0f, middle}, {r.right() - 8.0f, middle}, color.alpha(0.45f), 1.0f, true);
        c.text({r.right() - 20.0f, r.y + 3.0f, 14.0f, 14.0f}, "\xC2\xB1", 11.0f, color, Align::Center, true);
    }
}

void View::paintEnvReadout(Canvas &c, const Widget &w) {
    const EnvIndices e = envIndices(w.a);
    char a[32], b[32];
    std::string text;
    if (w.kind == Kind::EnvCurve) {
        formatParam(w.param, value(w.param), a, sizeof(a));
        text = trf(Str::CaptionEnvCurve, {tr(kEnvSegmentNames[w.b]), a});
    } else if (w.b == 0) {
        formatParam(e.attack, value(e.attack), a, sizeof(a));
        text = a;
    } else if (w.b == 1) {
        formatParam(e.hold, value(e.hold), a, sizeof(a));
        text = trf(Str::CaptionHold, {a});
    } else if (w.b == 2) {
        formatParam(e.decay, value(e.decay), a, sizeof(a));
        formatParam(e.sustain, value(e.sustain), b, sizeof(b));
        text = std::string(a) + "  \xC2\xB7  " + b;
    } else {
        formatParam(e.release, value(e.release), a, sizeof(a));
        text = a;
    }
    // On a backdrop of the display's color, which the curve under it shows through faintly; left of a bipolar
    // envelope's sign.
    const Rect r = envRect(w.a);
    const float right = r.right() - (w.a >= kOps && sourceBipolar(w.a - kOps) ? 24.0f : 8.0f);
    const float width = c.textWidth(text, 10.5f);
    c.fillRect({right - width - 5.0f, r.y + 2.0f, width + 10.0f, 14.0f}, kDisplayColor.alpha(0.85f), 4.0f);
    c.text({r.x, r.y + 2.0f, right - r.x, 14.0f}, text, 10.5f, kText, Align::Right);
}

// How many cycles a random LFO's display shows.


void View::paintLfo(Canvas &c, Rect r, int lfo) {
    c.fillRect(r, kDisplayColor, 6.0f);
    const Rect inner{r.x + 10.0f, r.y + 8.0f, r.w - 20.0f, r.h - 16.0f};
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const float zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? inner.bottom() : inner.center().y;
    c.line({inner.x, zero}, {inner.right(), zero}, kBorder, 1.0f);
    const auto shape = static_cast<LfoShape>(std::clamp(
        static_cast<int>(std::lround(value(paramIndex(lfo, SourceParam::Shape)))), 0, static_cast<int>(LfoShape::Count) - 1));
    std::vector<Point> points;
    auto at = [&](double x, double y) {
        points.push_back({inner.x + static_cast<float>(x) * inner.w, inner.center().y - static_cast<float>(y) * inner.h * 0.5f});
    };
    if (shape == LfoShape::Custom) {
        auto get = [this](int index) { return value(index); };
        for (int i = 0; i <= 96; ++i)
            at(i / 96.0, shapeValue(get, lfo, i / 96.0));
    } else if (shape == LfoShape::Random && !randomSweep(lfo)) {
        // Four cycles of a made-up run with this LFO's chaos and smoothing, until it runs.
        const double smooth = value(paramIndex(lfo, SourceParam::RandomSmooth));
        const double chaos = value(paramIndex(lfo, SourceParam::RandomChaos));
        uint32_t rng = 0x2545F491u;
        double from = 0.3;
        for (int cycle = 0; cycle < kRandomCycles; ++cycle) {
            const double to = randomStep(from, chaos, rng);
            for (int i = 0; i <= 24; ++i)
                at((cycle + i / 24.0) / kRandomCycles, randomGlide(from, to, i / 24.0, smooth));
            from = to;
        }
    } else if (shape != LfoShape::Random) {
        points = lfoPoints(shape, inner);
    }
    const Color color = kSourceColors[lfo];
    plotLine(c, points.data(), points.size(), color, 2.0f);

    // Running, a random LFO's last cycles as they came, swept across: each new cycle takes the next quarter,
    // round from the right end to the left, the older ones fainter, and the live dot rides the newest.
    const RandomTrace &trace = randomTraces_[lfo];
    const int newest = std::max(trace.count - 1, 0) % kRandomCycles;
    if (shape == LfoShape::Random && randomSweep(lfo)) {
        const double smooth = value(paramIndex(lfo, SourceParam::RandomSmooth));
        for (int k = 0; k < std::min(trace.count, kRandomCycles); ++k) {
            const int age = (newest - k + kRandomCycles) % kRandomCycles; // 0 for the newest
            const auto [from, to] = trace.glides[k];
            points.clear();
            for (int i = 0; i <= 24; ++i)
                at((k + i / 24.0) / kRandomCycles, randomGlide(from, to, i / 24.0, smooth));
            plotLine(c, points.data(), points.size(), color.alpha(1.0f - 0.2f * age), 2.0f);
        }
    }

    if (telemetry_.voices > 0 && sourceOn(lfo)) {
        const double x = shape == LfoShape::Random ? (newest + randomPhaseNow(lfo)) / kRandomCycles : lfoPhaseNow(lfo);
        const Point dot{inner.x + static_cast<float>(x) * inner.w,
                        inner.center().y - static_cast<float>(lfoValueNow(lfo)) * inner.h * 0.5f};
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }
}

void View::paintSources(Canvas &c) {
    if (controller_.layout().sourcesCollapsed)
        return; // the chips and the toggle are widgets
    const std::vector<int> shown = sourcesShown();
    if (shown.empty()) {
        c.text({kModPanel.x, kCardTop + kCardH * 0.5f - 10.0f, kModPanel.w, 20.0f}, tr(Str::HintNoSources), 12.0f,
               kDim);
        return;
    }
    c.pushClip(sourceCardClip());
    for (int position = 0; position < static_cast<int>(shown.size()); ++position)
        if (const float card = sourceOffset(shown, position) - sourceScroll();
            onScreen(card, kVisibleCards, sourceSpan(shown[position])))
            paintSource(c, card, shown[position]);
    c.popClip();
}

void View::paintSource(Canvas &c, float card, int s) {
    const Rect p = sourceCard(card, sourceSpan(s));
    const Color color = kSourceColors[s];
    const bool lfo = sourceTypeOf(s) == SourceType::Lfo;
    const bool dragged = drag_.kind == DragKind::SourceMove && drag_.widget.a == s;
    c.fillRect(p, kCardColor, 8.0f);
    c.strokeRect(p, dragged ? color : (sourceOn(s) ? color.alpha(0.35f) : kBorder), dragged ? 1.5f : 1.0f, 8.0f);
    if (sourceFolded(s)) { // the grip's dots under the handle (widgets), then its name down the strip
        const bool grip =
            (hover_ && hover_->kind == Kind::SourceGrip && hover_->a == s && drag_.kind == DragKind::None) || dragged;
        for (int d = 0; d < 6; ++d)
            c.fillCircle({p.center().x - 5.0f + (d / 2) * 5.0f, p.y + 35.0f + (d % 2) * 5.0f}, 1.4f, grip ? kText : kFaint);
        std::string name = sourceName(s);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        paintDownward(c, {p.x, p.y + 104.0f, p.w, p.h - 110.0f}, name, sourceOn(s) ? color : kDim);
        return;
    }

    // Title row: grip, the handle (a widget) and the name.
    const bool grip =
        (hover_ && hover_->kind == Kind::SourceGrip && hover_->a == s && drag_.kind == DragKind::None) || dragged;
    for (int d = 0; d < 6; ++d)
        c.fillCircle({p.x + 11.0f + (d % 2) * 5.0f, p.y + 12.0f + (d / 2) * 5.0f}, 1.4f, grip ? kText : kFaint);
    c.text({p.x + 52.0f, p.y + 7.0f, 80.0f, 21.0f}, sourceName(s), 11.5f, kText, Align::Left, true);

    if (lfo)
        paintLfo(c, sourceDisplay(card, std::lround(value(paramIndex(s, SourceParam::Shape))) ==
                                             static_cast<long>(LfoShape::Random)),
                 s);
    else
        paintEnvelope(c, sourceDisplay(card), kOps + s, color, true);

    const std::vector<int> slots = usedSlots(s);
    if (slots.empty()) {
        c.text({p.x + 16.0f, p.y + 174.0f, p.w - 32.0f, 18.0f}, tr(Str::HintNoTargets), 11.0f, kFaint);
        c.text({p.x + 16.0f, p.y + 192.0f, p.w - 32.0f, 18.0f}, tr(Str::HintNoTargetsThen), 11.0f, kFaint);
        return;
    }
    const int rows = static_cast<int>(slots.size());
    const float first = slotFirst(s);
    const int top = static_cast<int>(std::floor(first));
    c.pushClip(slotClip(card, sourceCardClip()));
    for (int row = top; row < std::min(top + kVisibleSlots + 1, rows); ++row) {
        // The host can clear a target from the audio thread since usedSlots() looked: skip the row this frame.
        const int target = slotTarget(value(slotTargetIndex(s, slots[row])));
        if (target < 0)
            continue;
        const Rect r = slotRow(card, static_cast<float>(row) - first);
        c.fillRect(r, kRaised, 5.0f);
        // A bypassed row is dimmed; its dot (the bypass switch, a widget) is hollow. A long name is drawn smaller to
        // fit, down to 7.5, which the canvas draws at about 9 px; its width is a straight line between the two.
        const std::string name = paramName(target);
        constexpr float kFull = 11.0f, kSmallest = 7.5f;
        float size = kFull;
        if (const float full = c.textWidth(name, kFull); full > kSlotNameW) {
            const float smallest = c.textWidth(name, kSmallest);
            size = smallest >= kSlotNameW ? kSmallest
                                          : std::floor((kFull - (full - kSlotNameW) / (full - smallest) *
                                                                   (kFull - kSmallest)) * 4.0f) / 4.0f;
        }
        c.text({r.x + 18.0f, r.y, kSlotNameW, r.h}, name, size, slotBypassed(s, slots[row]) ? kFaint : kText,
               Align::Left);
    }
    c.popClip();
    // More rows than fit: the wheel over the list scrolls them, and so does the bar beside them (a widget).
}

void View::paintNode(Canvas &c, const Widget &w) {
    const bool hovered = isHovered(w) || isActive(w);
    const bool selected = controller_.layout().selectedNode == w.a;
    const Point center = w.rect.center();

    if (isBoxNode(w.a)) { // a filter's box with its type, or a shaper's, squarer, with its curve's name
        const bool shaper = isShaperNode(w.a);
        const float corner = shaper ? 3.0f : 8.0f;
        const Color color = nodeColor(w.a);
        const bool live = boxHasInput(w.a);
        if (live && telemetry_.voices > 0 && reachesOut(w.a))
            c.fillRect(w.rect.inset(-6.0f), color.alpha(0.12f), corner + 4.0f);
        c.fillRect(w.rect, hovered ? kTrack : kRaised, corner);
        c.strokeRect(w.rect, live ? color : kFaint, selected ? 2.5f : 1.5f, corner);
        if (selected)
            c.strokeRect(w.rect.inset(-4.5f), kText.alpha(0.5f), 1.0f, corner + 3.0f);
        c.text({w.rect.x, w.rect.y + 2.0f, w.rect.w, 16.0f}, kNodeNames[w.a], 12.0f, kText, Align::Center, true);
        const int box = w.a - kOps; // the filter, or past them the shaper
        const std::string kind =
            shaper ? clipModeName(clipMode(value(paramIndex(shaperOfNode(w.a), ShaperParam::Mode))))
                   : filterLabel(static_cast<FilterType>(std::lround(value(paramIndex(box, FilterParam::Type)))),
                                 value(paramIndex(box, FilterParam::Slope)));
        c.text({w.rect.x, w.rect.y + 17.0f, w.rect.w, 14.0f}, kind, 10.5f, kDim);
        return;
    }

    const int op = w.a;
    bool heard = value(paramIndex(op, OpParam::Out)) > 0.0;
    for (int filter = 0; filter < kFilters; ++filter)
        heard = heard || (nodeOn(kOps + filter) && value(sendIndex(op, filter)) > 0.0);
    for (int shaper = 0; shaper < kShapers; ++shaper)
        heard = heard || (nodeOn(kOps + kFilters + shaper) && value(shaperSendIndex(op, shaper)) > 0.0);
    // It lights up while playing only if it ends up in the output.
    const float level = std::min(telemetry_.opLevels[op], 1.0f);
    if (level > 0.01f && reachesOut(op))
        c.fillCircle(center, kNodeRadius + 7.0f, kAccent.alpha(0.22f * level));
    if (std::lround(value(paramIndex(op, OpParam::Unison))) > 1 && !isNoise(op)) {
        // Unison: a stack of rings peeking out on both sides.
        c.strokeCircle({center.x - 4.5f, center.y}, kNodeRadius, (heard ? kAccent : kFaint).alpha(0.45f), 1.2f);
        c.strokeCircle({center.x + 4.5f, center.y}, kNodeRadius, (heard ? kAccent : kFaint).alpha(0.45f), 1.2f);
    }
    c.fillCircle(center, kNodeRadius, hovered ? kTrack : kRaised);
    c.strokeCircle(center, kNodeRadius, heard ? kAccent : kFaint, selected ? 2.5f : 1.5f);
    if (selected)
        c.strokeCircle(center, kNodeRadius + 4.5f, kText.alpha(0.5f), 1.0f);
    // Wave glyph above the letter, ratio below it.
    const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))));
    const std::vector<Point> glyph = wavePoints(wave, {center.x - 6.0f, center.y - 16.0f, 12.0f, 5.0f});
    c.polyline(glyph.data(), glyph.size(), kDim, 1.0f);
    c.text({center.x - 20.0f, center.y - 11.0f, 40.0f, 20.0f}, kNodeNames[op], 15.0f, kText, Align::Center, true);
    char ratio[16];
    const double r = coarseRatio(value(paramIndex(op, OpParam::Coarse))) * (1.0 + value(paramIndex(op, OpParam::Fine)));
    std::snprintf(ratio, sizeof(ratio), "\xC3\x97%.2g", r);
    if (isNoise(op)) {
        // No ratio: its color, or its slope between the named ones.
        const double db = std::round(value(paramIndex(op, OpParam::NoiseSlope)) * 60.0) / 10.0;
        const char *const name = db == 0.0   ? tr(Str::NoiseWhiteCaption)
                                 : db == 3.0 ? tr(Str::NoisePinkCaption)
                                 : db == 6.0 ? tr(Str::NoiseBrownCaption)
                                             : nullptr;
        if (name)
            std::snprintf(ratio, sizeof(ratio), "%s", name);
        else
            std::snprintf(ratio, sizeof(ratio), "-%.1fdB", db);
    }
    c.text({center.x - 22.0f, center.y + 5.0f, 44.0f, 14.0f}, ratio, 10.0f, kDim); // inside the ring
}

void View::paintWidget(Canvas &c, const Widget &w, const std::vector<Assignment> &mods) {
    const bool hovered = isHovered(w);
    const bool active = isActive(w);
    switch (w.kind) {
    case Kind::Knob:
        paintKnob(c, w, mods);
        break;
    case Kind::Node:
        paintNode(c, w);
        break;
    case Kind::NodeGhost: { // dashed, like the cards that add things, and in the node's color while hovered
        const Color color = hovered ? nodeColor(w.a) : kFaint;
        c.strokeRect(w.rect, color, 1.0f, isShaperNode(w.a) ? 3.0f : isFilterNode(w.a) ? 6.0f : w.rect.h * 0.5f, true);
        c.text(w.rect, kNodeNames[w.a], 10.5f, color, Align::Center, true);
        break;
    }
    case Kind::Choice:
    case Kind::Toggle: {
        const bool on = w.kind == Kind::Choice ? std::lround(value(w.param)) == w.b : value(w.param) >= 0.5;
        // Legato only matters in mono: show it as inactive otherwise.
        bool relevant = w.param != paramIndex(GlobalParam::Legato) || value(paramIndex(GlobalParam::Mono)) >= 0.5;
        if (isLfoFree(w.param)) // a one-shot restarts with every note
            relevant = value(paramIndex(sourceOf(w.param), SourceParam::OneShot)) < 0.5;
        const Str clipModes[] = {Str::SwitchHard, Str::SwitchSoft, Str::SwitchAsym};
        const FxParam field = fxFieldOf(w.param);
        // Operators', filters', sources' and effects' on switches show the power symbol rather than a word.
        const bool power = isPowerParam(w.param) || isSourcePower(w.param) || field == FxParam::Enabled;
        const std::string count = std::to_string(w.b); // the partial editor's resolution
        const char *text = power ? ""
                           : isPartialCount(w.param) ? count.c_str()
                           : w.param == paramIndex(GlobalParam::Mono) ? tr(w.b == 0 ? Str::SwitchPoly : Str::SwitchMono)
                           : w.param == paramIndex(GlobalParam::Legato) ? tr(Str::SwitchLegato)
                           : field == FxParam::ShaperMode               ? tr(clipModes[std::clamp(w.b, 0, 2)])
                           : field == FxParam::FilterType ? filterTypeShortName(static_cast<FilterType>(w.b))
                           : field == FxParam::PhaserStereo || field == FxParam::FlangerStereo ? tr(Str::SwitchStereo)
                                                                                               : tr(Str::SwitchAlways);
        // LFO switches take their LFO's color, effect switches the effects' color.
        Color color = kAccent;
        if (sourceOf(w.param) >= 0)
            color = kSourceColors[sourceOf(w.param)];
        if (fxSlotOf(w.param) >= 0)
            color = fxColor(fxSlotOf(w.param));
        if (shaperOf(w.param) >= 0)
            color = kShaperColor;
        c.fillRect(w.rect, on ? color.alpha(relevant ? 0.22f : 0.08f) : (hovered ? kTrack : kRaised), 5.0f);
        if (on)
            c.strokeRect(w.rect, color.alpha(relevant ? 1.0f : 0.35f), 1.0f, 5.0f);
        if (field == FxParam::FilterType) // the filter types show as icons, named in their tooltips
            filterIcon(c, {w.rect.center().x - 10.0f, w.rect.y + 5.0f, 20.0f, w.rect.h - 10.0f},
                       static_cast<FilterType>(w.b), on ? color : kDim);
        else if (power)
            powerIcon(c, w.rect.center(), on ? color : (hovered ? kText : kDim));
        else if (shaperFieldOf(w.param) == ShaperParam::Link) // a voice shaper's link, as the shaper effect's
            fxSwitchIcon(c, w.rect.center(), FxParam::ShaperLink, on ? color : (hovered ? kText : kDim));
        else if (isFxIconSwitch(field))
            fxSwitchIcon(c, w.rect.center(), field, on ? color : (hovered ? kText : kDim));
        else if (isLfoSync(w.param)) // an LFO's SYNC, the effects' metronome
            fxSwitchIcon(c, w.rect.center(), FxParam::DelaySync, on ? color : (hovered ? kText : kDim));
        else if (const SourceParam mode = sourceFieldOf(w.param);
                 mode == SourceParam::Free || mode == SourceParam::OneShot || mode == SourceParam::Unipolar)
            lfoModeIcon(c, w.rect.center(), mode, !relevant ? kFaint : on ? color : (hovered ? kText : kDim));
        else
            c.text(w.rect, text, 10.5f, !relevant ? kFaint : (on ? kText : kDim), Align::Center, on);
        break;
    }
    case Kind::ViewTab: {
        // While an editor has the panels (shape, shaper curve or partials), none of the tabs is the one shown.
        const bool selected = controller_.layout().view == static_cast<RoutingView>(w.a) && shapeEditing() < 0 &&
                              partialEditing() < 0;
        if (selected)
            c.fillRect(w.rect, kRaised, 6.0f);
        else if (hovered)
            c.fillRect(w.rect, kRaised.alpha(0.5f), 6.0f);
        c.text(w.rect, tr(kViewNames[w.a]), 11.0f, selected ? kText : kDim, Align::Center, true);
        // A dot on the FX tab while any effect is on.
        bool anyFx = false;
        for (int slot : fxChain())
            anyFx = anyFx || value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
        if (w.a == static_cast<int>(RoutingView::Fx) && anyFx)
            c.fillCircle({w.rect.right() - 5.0f, w.rect.y + 5.0f}, 2.0f, kFxColor);
        break;
    }
    case Kind::NodeTab: {
        const bool selected = controller_.layout().selectedNode == w.a;
        const bool enabled = nodeOn(w.a);
        const Color color = nodeColor(w.a);
        c.fillRect(w.rect, selected ? color.alpha(enabled ? 1.0f : 0.35f) : (hovered ? kTrack : kRaised.alpha(enabled ? 1.0f : 0.4f)),
                   6.0f);
        c.text(w.rect, kNodeNames[w.a], 11.0f, selected ? kBackground : (enabled ? kDim : kFaint), Align::Center, true);
        if (soundsOut(w.a))
            c.fillCircle({w.rect.right() - 5.0f, w.rect.y + 5.0f}, 2.0f, selected ? kBackground : color);
        break;
    }
    case Kind::ShapeButton:
    case Kind::WaveButton: {
        const bool lfo = w.kind == Kind::ShapeButton;
        const Color color = lfo ? kSourceColors[w.a] : kAccent;
        const int param = lfo ? paramIndex(w.a, SourceParam::Shape) : paramIndex(w.a, OpParam::Wave);
        const bool selected = std::lround(value(param)) == w.b;
        c.fillRect(w.rect, selected ? color.alpha(0.18f) : (hovered ? kTrack : kRaised), 6.0f);
        if (selected)
            c.strokeRect(w.rect, color, 1.0f, 6.0f);
        const bool custom = lfo && static_cast<LfoShape>(w.b) == LfoShape::Custom;
        const Rect icon = w.rect.inset(custom ? 6.5f : (lfo ? 8.0f : 7.0f));
        const std::vector<Point> points =
            lfo ? lfoPoints(static_cast<LfoShape>(w.b), icon) : wavePoints(static_cast<OpWave>(w.b), icon);
        c.polyline(points.data(), points.size(), selected ? color : kDim, 1.5f);
        if (custom)
            customShapeDots(c, icon, selected ? color : kDim);
        break;
    }
    case Kind::TypeButton: {
        const bool selected = std::lround(value(paramIndex(w.a, FilterParam::Type))) == w.b;
        c.fillRect(w.rect, selected ? kFilterColor.alpha(0.18f) : (hovered ? kTrack : kRaised), 6.0f);
        if (selected)
            c.strokeRect(w.rect, kFilterColor, 1.0f, 6.0f);
        filterIcon(c, w.rect.inset(6.0f), static_cast<FilterType>(w.b), selected ? kFilterColor : kDim);
        break;
    }
    case Kind::Port: { // under the mouse, or dragged from, the node's outline glows in its color
        if (!hovered && !(drag_.kind == DragKind::Connect && drag_.widget.a == w.a))
            break;
        const Color color = nodeColor(w.a);
        const Rect node = w.rect.inset(kEdgeOut);
        if (isBoxNode(w.a)) {
            const float radius = isShaperNode(w.a) ? 3.0f : 8.0f;
            c.strokeRect(node.inset(-2.0f), color.alpha(0.35f), 5.0f, radius + 2.0f);
            c.strokeRect(node, color, 2.5f, radius);
        } else {
            c.strokeCircle(node.center(), kNodeRadius + 2.0f, color.alpha(0.35f), 5.0f);
            c.strokeCircle(node.center(), kNodeRadius, color, 2.5f);
        }
        break;
    }
    case Kind::WireEnd: // drawn with its wire, which lights up under the mouse
        break;
    case Kind::WireLabel:
    case Kind::OutLabel: {
        const double amount = value(w.param);
        const bool focused = hovered || active;
        const bool boxes = isBoxNode(w.a) || isBoxNode(w.b);
        const Color tint = w.kind == Kind::OutLabel ? kText.alpha(0.3f) : routeColor(w.a, w.b).alpha(boxes ? 0.6f : 0.5f);
        c.fillRect(w.rect, kRaised, 9.0f);
        c.strokeRect(w.rect, focused ? kText : tint, 1.0f, 9.0f);
        std::string text = percent(amount);
        if (w.kind == Kind::WireLabel && w.a == w.b)
            text = trf(Str::CaptionFeedback, {text});
        c.text({w.rect.x, w.rect.y - 2.0f, w.rect.w, w.rect.h}, text, 10.5f, focused ? kText : kDim);
        paintModRange(c, {w.rect.x + 7.0f, w.rect.bottom() - 4.0f, w.rect.w - 14.0f, 1.5f}, w.param, mods);
        break;
    }
    case Kind::GridCell: {
        const double amount = value(w.param);
        const bool out = w.b == kGridCols - 1;
        const float a = static_cast<float>(liveAmount(w.param)); // the fill follows modulation while notes play
        const bool boxes = isBoxNode(w.a) || (!out && isBoxNode(w.b));
        const Color fill = out ? kText.alpha(0.1f + 0.3f * a)
                               : routeColor(w.a, out ? w.a : w.b).alpha(0.12f + (boxes ? 0.5f : 0.55f) * a);
        // The row and column of a node that is off are greyed out; their routes keep their amounts.
        const bool off = !nodeOn(w.a) || (!out && !nodeOn(w.b));
        const float dim = off ? 0.35f : 1.0f;
        c.fillRect(w.rect, (amount > 0.0 ? fill : kRaised).alpha(dim), 5.0f);
        if (w.a == w.b)
            c.strokeRect(w.rect, kAccent.alpha(0.35f * dim), 1.0f, 5.0f); // feedback diagonal
        if (hovered || active)
            c.strokeRect(w.rect, kText, 1.5f, 5.0f);
        char text[8];
        std::snprintf(text, sizeof(text), "%.0f", amount * 100.0);
        const bool shown = amount > 0.0 || wired(w.param); // a route kept at 0 % shows its 0, dimmed
        c.text(w.rect, shown ? text : "\xC2\xB7", 12.0f,
               off ? kFaint : (amount > 0.0 ? kText : (shown ? kDim : kFaint)), Align::Center, amount > 0.0 && !off);
        paintModRange(c, {w.rect.x + 4.0f, w.rect.bottom() - 5.0f, w.rect.w - 8.0f, 1.5f}, w.param, mods);
        break;
    }
    case Kind::SourceAdd:
        // Like ADD EFFECT, without its chevron: these add at once.
        c.fillRect(w.rect, hovered ? kSourceAddColor.alpha(0.18f) : kRaised, 5.0f);
        c.strokeRect(w.rect, kSourceAddColor.alpha(hovered ? 0.9f : 0.45f), 1.0f, 5.0f);
        c.text(w.rect, tr(static_cast<SourceType>(w.a) == SourceType::Lfo ? Str::ButtonAddLfo : Str::ButtonAddEnv),
               10.5f, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::Scrollbar: {
        // The widget is the track with some room around it, so the thin bar is easy to hit.
        const auto area = static_cast<ScrollArea>(w.a);
        c.fillRect(w.rect.inset(4.0f), kRaised, 3.0f);
        c.fillRect(scrollThumb(area), hovered || active ? kDim : kFaint, 3.0f);
        break;
    }
    case Kind::SourceGrip:
        break; // drawn with its card
    case Kind::McBand: {
        // Linked, the band buttons stand aside: every band takes the low band's settings.
        const bool linked = value(paramIndex(w.a, FxParam::McLink)) >= 0.5;
        const bool selected = fxBand_[w.a] == w.b && !linked;
        c.fillRect(w.rect, selected ? fxColor(w.a).alpha(0.22f) : (hovered && !linked ? kTrack : kRaised), 5.0f);
        if (selected)
            c.strokeRect(w.rect, fxColor(w.a), 1.0f, 5.0f);
        mcBandIcon(c, {w.rect.center().x - 10.0f, w.rect.y + 6.0f, 20.0f, w.rect.h - 12.0f}, w.b,
                   linked ? kFaint : (selected ? fxColor(w.a) : (hovered ? kText : kDim)));
        break;
    }
    case Kind::SourcesToggle: {
        // A chevron pointing down while open, right while collapsed.
        const bool collapsed = controller_.layout().sourcesCollapsed;
        const Point m{w.rect.x + 9.0f, w.rect.center().y};
        const Point open[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        const Point shut[] = {{m.x - 2.0f, m.y - 4.0f}, {m.x + 2.5f, m.y}, {m.x - 2.0f, m.y + 4.0f}};
        c.polyline(collapsed ? shut : open, 3, hovered ? kText : kDim, 1.5f);
        c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, tr(Str::HeadingModulation), 11.0f,
               hovered ? kText : kDim, Align::Left, true);
        break;
    }
    case Kind::SourceHandle:
    case Kind::MacroHandle: {
        // A switched-off source's is faint, like its rings on knobs, and so is a macro's or the note source's while
        // it modulates nothing; either can still be dragged onto a target.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a].alpha(sourceLive(w.a) ? 1.0f : 0.3f);
        if (hovered || active)
            c.fillCircle(center, 13.0f, color.alpha(0.25f));
        c.fillCircle(center, 8.5f, color);
        c.fillCircle(center, 3.5f, kPanel);
        break;
    }
    case Kind::SlotBar: {
        const Color color = kSourceColors[w.a].alpha(slotBypassed(w.a, w.b) ? 0.4f : 1.0f);
        const double amount = value(w.param);
        const float cy = w.rect.center().y;
        const float mid = w.rect.center().x;
        const float x = mid + static_cast<float>(amount) * w.rect.w * 0.5f;
        c.fillRect({w.rect.x, cy - 2.0f, w.rect.w, 4.0f}, kTrack, 2.0f);
        c.fillRect({std::min(mid, x), cy - 2.0f, std::abs(x - mid), 4.0f}, color, 2.0f);
        c.line({mid, cy - 4.0f}, {mid, cy + 4.0f}, kFaint, 1.0f);
        c.fillCircle({x, cy}, hovered || active ? 5.0f : 4.0f, color);
        // Another source modulating this depth shows how far it takes it, under the bar.
        paintModRange(c, {w.rect.x, cy + 6.0f, w.rect.w, 1.5f}, w.param, mods);
        char text[16];
        std::snprintf(text, sizeof(text), "%+.0f%%", amount * 100.0);
        c.text({w.rect.right() + 2.0f, w.rect.y, 38.0f, w.rect.h}, text, 10.5f, kDim, Align::Right);
        break;
    }
    case Kind::SlotRemove: {
        const Point center = w.rect.center();
        const Color color = hovered ? kText : kFaint;
        c.line({center.x - 3.5f, center.y - 3.5f}, {center.x + 3.5f, center.y + 3.5f}, color, 1.5f);
        c.line({center.x - 3.5f, center.y + 3.5f}, {center.x + 3.5f, center.y - 3.5f}, color, 1.5f);
        break;
    }
    case Kind::SlotScroll: {
        const float card = cardOf(w.a);
        if (card == kOffScreen)
            break;
        c.fillRect(slotScrollTrack(card), kBorder, 1.5f);
        c.fillRect(slotThumb(w.a, card), kSourceColors[w.a].alpha(hovered || active ? 1.0f : 0.7f), 1.5f);
        break;
    }
    case Kind::ValueText:
        break; // drawn by its slot's bar
    case Kind::WaveMore: {
        // Three dots, or the glyph of the wave picked from the list, with a chevron.
        const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
        const bool selected = isMoreWave(wave);
        const bool lit = hovered || waveMenu_ == w.a;
        c.fillRect(w.rect, selected ? kAccent.alpha(0.18f) : (lit ? kTrack : kRaised), 6.0f);
        if (selected) {
            c.strokeRect(w.rect, kAccent, 1.0f, 6.0f);
            const Rect icon{w.rect.x + 7.0f, w.rect.y + 7.0f, w.rect.w - 22.0f, w.rect.h - 14.0f};
            const std::vector<Point> points = wavePoints(wave, icon);
            c.polyline(points.data(), points.size(), kAccent, 1.5f);
            const Point m{w.rect.right() - 8.0f, w.rect.center().y};
            const Point chevron[] = {{m.x - 3.0f, m.y - 1.5f}, {m.x, m.y + 1.5f}, {m.x + 3.0f, m.y - 1.5f}};
            c.polyline(chevron, 3, lit ? kText : kDim, 1.2f);
        } else {
            for (int d = -1; d <= 1; ++d)
                c.fillCircle({w.rect.center().x + d * 5.0f, w.rect.center().y}, 1.6f, lit ? kText : kDim);
        }
        break;
    }
    case Kind::WaveMenu:
        if (w.a == 1) {
            c.fillRect(w.rect, kRaised, 7.0f);
            c.strokeRect(w.rect, kBorder, 1.0f, 7.0f);
        }
        break;
    case Kind::WaveMenuItem: {
        const bool selected = std::lround(value(paramIndex(w.a, OpParam::Wave))) == w.b;
        if (hovered || selected)
            c.fillRect(w.rect, kAccent.alpha(hovered ? 0.18f : 0.1f), 5.0f);
        const Rect icon{w.rect.x + 8.0f, w.rect.y + 7.0f, 22.0f, w.rect.h - 14.0f};
        const std::vector<Point> points = wavePoints(static_cast<OpWave>(w.b), icon);
        c.polyline(points.data(), points.size(), selected ? kAccent : kDim, 1.5f);
        c.text({icon.right() + 10.0f, w.rect.y, w.rect.right() - icon.right() - 14.0f, w.rect.h},
               waveName(static_cast<OpWave>(w.b)), 11.0f, hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::ShapeReset:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text(w.rect, tr(Str::ButtonReset), 10.5f, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::ShapeGrid: {
        const UiLayout &layout = controller_.layout();
        if (w.b == 0) { // the count
            c.text(w.rect, std::to_string(w.a == 0 ? layout.shapeGridX : layout.shapeGridY), 11.0f,
                   hovered ? kText : kDim, Align::Center, true);
            break;
        }
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 5.0f);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.5f, m.y}, {m.x + 3.5f, m.y}, color, 1.5f);
        if (w.b > 0)
            c.line({m.x, m.y - 3.5f}, {m.x, m.y + 3.5f}, color, 1.5f);
        break;
    }
    case Kind::UndoButton: {
        const bool redo = w.a == 1;
        const bool can = (redo ? controller_.nextRedo() : controller_.nextUndo()) != nullptr;
        c.fillRect(w.rect, hovered && can ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        // An arrow curving over the top, its head at the left end (undo) or the right (redo), pointing down.
        const Point m{w.rect.center().x, w.rect.center().y + 2.0f};
        const Color color = !can ? kFaint : (hovered ? kText : kDim);
        constexpr float kEnd = 1.9f, kRadius = 5.5f;
        c.arc(m, kRadius, -kEnd, kEnd, color, 1.5f);
        const float angle = redo ? kEnd : -kEnd;
        const Point end = Canvas::onCircle(m, kRadius, angle);
        // Clockwise along the circle is (cos, sin); undo runs the other way.
        const float way = redo ? 1.0f : -1.0f;
        const Point along{way * std::cos(angle), way * std::sin(angle)}, side{-along.y, along.x};
        const Point head[] = {{end.x + along.x * 3.5f, end.y + along.y * 3.5f},
                              {end.x + side.x * 3.0f, end.y + side.y * 3.0f},
                              {end.x - side.x * 3.0f, end.y - side.y * 3.0f}};
        c.fillPolygon(head, std::size(head), color);
        break;
    }
    case Kind::EnvPoint: {
        const Point center = w.rect.center();
        const Color color = w.a < kOps ? kAccent : kSourceColors[w.a - kOps];
        c.fillCircle(center, hovered || active ? 5.5f : 4.0f, hovered || active ? color : kPanel);
        c.strokeCircle(center, hovered || active ? 5.5f : 4.0f, color, 1.8f);
        break; // its readout goes over the other handles (paintEnvReadout())
    }
    case Kind::FilterPoint: {
        const Point center = w.rect.center();
        const bool focused = hovered || active;
        c.fillCircle(center, focused ? 7.0f : 5.5f, focused ? kFilterColor : kPanel);
        c.strokeCircle(center, focused ? 7.0f : 5.5f, kFilterColor, 2.0f);
        if (focused) {
            const int cutoff = paramIndex(w.a, FilterParam::Cutoff);
            const int resonance = paramIndex(w.a, FilterParam::Resonance);
            char a[32], b[32], text[80];
            formatParam(cutoff, value(cutoff), a, sizeof(a));
            formatParam(resonance, value(resonance), b, sizeof(b));
            std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionResonance, {a, b}).c_str());
            c.text({kFilterDisplay.x, kFilterDisplay.y + 2.0f, kFilterDisplay.w - 8.0f, 14.0f}, text, 10.5f, kText,
                   Align::Right);
        }
        break;
    }
    case Kind::FxGrip:
    case Kind::ShapeArea:
    case Kind::MatrixLabel:
    case Kind::NoteDisplay:
        break; // drawn with their card, editor, matrix or the keyboard strip
    case Kind::FxPower: {
        // The effect's icon: in its color while the effect is on, grey while it is off, as its card is, whatever
        // the mouse does. An effect that is on lights it up while the card is grabbed, by its title or the icon,
        // and under the mouse it sits on a button, as a click switches it.
        const FxType type = fxSlotType(w.a);
        const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.a == w.a) ||
                             (hover_ && hover_->kind == Kind::FxGrip && hover_->a == w.a && drag_.kind == DragKind::None);
        if (hovered)
            c.fillRect(w.rect, kTrack, 5.0f);
        const bool on = value(w.param) >= 0.5;
        fxIcon(c, w.rect.inset(4.0f), type, !on ? kDim : (hovered || grabbed ? kText : fxTypeColor(type)));
        break;
    }
    case Kind::FxFold:
    case Kind::SourceFold: {
        // Unfolded, a chevron in the title row that folds the card; folded, the strip below the cross, lit
        // while hovered, which unfolds it.
        const bool folded = w.kind == Kind::FxFold ? fxFolded(w.a) : sourceFolded(w.a);
        if (folded) {
            if (hovered)
                c.fillRect(w.rect.inset(3.0f), kText.alpha(0.06f), 6.0f);
            break;
        }
        const Color color = hovered ? kText : kDim;
        const Point m = w.rect.center();
        const Point chevron[] = {{m.x + 2.5f, m.y - 5.0f}, {m.x - 2.5f, m.y}, {m.x + 2.5f, m.y + 5.0f}};
        c.polyline(chevron, 3, color, 1.5f);
        break;
    }
    case Kind::MacroName: // the knob draws the name; hovered, a line under it says it can be changed
        if (hovered && !typing_)
            c.line({w.rect.x + 14.0f, w.rect.bottom() - 1.0f}, {w.rect.right() - 14.0f, w.rect.bottom() - 1.0f},
                   kFaint, 1.0f, true);
        break;
    case Kind::FxMenuButton: {
        // A button with a chevron; full, it is dimmed.
        const bool full = fxChain().size() >= static_cast<size_t>(kFxSlots);
        const bool lit = (hovered || (fxMenuOpen_ && fxMenuAt_.x < 0.0f)) && !full; // its list, not the dashed card's
        c.fillRect(w.rect, lit ? kFxColor.alpha(0.18f) : kRaised, 5.0f);
        c.strokeRect(w.rect, kFxColor.alpha(full ? 0.15f : (lit ? 0.9f : 0.45f)), 1.0f, 5.0f);
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 30.0f, w.rect.h}, tr(Str::ButtonAddEffect), 10.5f,
               full ? kFaint : (lit ? kText : kDim), Align::Left, true);
        const Point m{w.rect.right() - 13.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        c.polyline(chevron, 3, full ? kFaint : (lit ? kText : kDim), 1.5f);
        break;
    }
    case Kind::FxShadow:
    case Kind::SourceShadow: {
        // A dashed outline with a plus in it and what it adds under that, lit while hovered.
        const bool fx = w.kind == Kind::FxShadow;
        const Color color = fx ? kFxColor : kSourceAddColor;
        if (hovered)
            c.fillRect(w.rect, color.alpha(0.07f), 8.0f);
        c.strokeRect(w.rect, hovered ? color.alpha(0.8f) : kFaint, 1.2f, 8.0f, true);
        const float arm = fx ? 12.0f : 9.0f;
        const Point m{w.rect.center().x, w.rect.center().y - arm};
        const Color ink = hovered ? kText : kDim;
        c.line({m.x - arm, m.y}, {m.x + arm, m.y}, ink, 2.0f);
        c.line({m.x, m.y - arm}, {m.x, m.y + arm}, ink, 2.0f);
        const char *what = tr(fx ? Str::ButtonAddEffect
                              : static_cast<SourceType>(w.a) == SourceType::Lfo ? Str::ButtonAddLfo
                                                                               : Str::ButtonAddEnv);
        c.text({w.rect.x, m.y + arm + 6.0f, w.rect.w, 16.0f}, what, 10.5f, ink, Align::Center, true);
        break;
    }
    case Kind::FxInsert: // drawn with the arrows between the cards
        break;
    case Kind::FxMenu:
        if (w.a == 1) { // the list's panel
            c.fillRect(w.rect, kRaised, 7.0f);
            c.strokeRect(w.rect, kBorder, 1.0f, 7.0f);
        }
        break;
    case Kind::FxAdd:
        if (hovered)
            c.fillRect(w.rect, fxTypeColor(static_cast<FxType>(w.a)).alpha(0.18f), 5.0f);
        fxIcon(c, {w.rect.x + 9.0f, w.rect.y + 5.0f, 18.0f, 14.0f}, static_cast<FxType>(w.a),
               hovered ? fxTypeColor(static_cast<FxType>(w.a)) : kDim);
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, fxName(static_cast<FxType>(w.a)), 11.0f,
               hovered ? kText : kDim, Align::Left);
        break;
    case Kind::FxRemove:
    case Kind::SourceRemove: {
        const Point center = w.rect.center();
        const Color color = hovered ? kText : kFaint;
        c.line({center.x - 4.0f, center.y - 4.0f}, {center.x + 4.0f, center.y + 4.0f}, color, 1.5f);
        c.line({center.x - 4.0f, center.y + 4.0f}, {center.x + 4.0f, center.y - 4.0f}, color, 1.5f);
        break;
    }
    case Kind::PeqPoint: {
        const Point center = w.rect.center();
        const bool focused = hovered || active;
        const Color color = kSourceColors[w.b];
        c.fillCircle(center, focused ? 7.5f : 6.5f, focused ? color : kPanel);
        c.strokeCircle(center, focused ? 7.5f : 6.5f, color, 1.8f);
        c.text({center.x - 8.0f, center.y - 7.0f, 16.0f, 14.0f}, std::to_string(w.b + 1), 10.5f,
               focused ? kBackground : kText, Align::Center, true);
        if (focused) {
            char freq[32], gain[32], q[32], text[112];
            formatParam(peqFreqIndex(w.a, w.b), value(peqFreqIndex(w.a, w.b)), freq, sizeof(freq));
            formatParam(peqGainIndex(w.a, w.b), value(peqGainIndex(w.a, w.b)), gain, sizeof(gain));
            formatParam(peqQIndex(w.a, w.b), value(peqQIndex(w.a, w.b)), q, sizeof(q));
            if (isCut(peqType(value(peqTypeIndex(w.a, w.b))))) // a cut has no gain
                std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", freq, q);
            else
                std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s  \xC2\xB7  %s", freq, gain, q);
            const Rect area = peqArea(w.a);
            c.text({area.x, area.y - 6.0f, area.w, 14.0f}, text, 10.5f, kText, Align::Center);
        }
        break;
    }
    case Kind::LfoDisplay:
        // A freeform LFO's display opens its editor.
        if (std::lround(value(paramIndex(w.a, SourceParam::Shape))) == static_cast<long>(LfoShape::Custom)) {
            const Rect edit{w.rect.right() - 38.0f, w.rect.y + 4.0f, 34.0f, 15.0f};
            c.fillRect(edit, hovered ? kSourceColors[w.a].alpha(0.3f) : kRaised, 4.0f);
            c.text(edit, tr(Str::ButtonEdit), 10.5f, hovered ? kText : kDim, Align::Center, true);
        }
        break;
    case Kind::ClipDisplay: // the EDIT button on a drawn curve's display
        c.fillRect(w.rect, hovered ? clipColor(w.a).alpha(0.3f) : kRaised, 4.0f);
        c.text(w.rect, tr(Str::ButtonEdit), 10.5f, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::ListButton: {
        // A slope shows as it plays (a band-pass's 6 dB as 12), and faintly for a comb, which has none.
        const int shown = listShown(w.param);
        const bool relevant =
            !isFilterSlope(w.param) || hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param)))));
        const bool lit = relevant && (hovered || listMenu_ == w.param);
        const Color color = listColor(w.param);
        c.fillRect(w.rect, lit ? color.alpha(0.18f) : kRaised, 5.0f);
        c.strokeRect(w.rect, color.alpha(lit ? 0.9f : relevant ? 0.45f : 0.2f), 1.0f, 5.0f);
        const Color text = !relevant ? kFaint : lit ? kText : kDim;
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 30.0f, w.rect.h}, listItemName(w.param, shown), 11.0f, text,
               Align::Left, true);
        const Point m{w.rect.right() - 13.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        c.polyline(chevron, 3, text, 1.5f);
        break;
    }
    case Kind::ListMenu:
        if (w.a == 1) {
            c.fillRect(w.rect, kRaised, 7.0f);
            c.strokeRect(w.rect, kBorder, 1.0f, 7.0f);
        }
        break;
    case Kind::PeqTypeButton: {
        // The band's shape in its color, the shape's name under it like a knob's label.
        const bool lit = hovered || peqMenu_ == w.param;
        const auto type = peqType(value(w.param));
        c.fillRect(w.rect, lit ? fxColor(fxSlotOf(w.param)).alpha(0.18f) : kRaised, 6.0f);
        c.strokeRect(w.rect, fxColor(fxSlotOf(w.param)).alpha(lit ? 0.9f : 0.45f), 1.0f, 6.0f);
        peqIcon(c, w.rect.inset(7.0f), type, kSourceColors[w.b]);
        c.text({w.rect.x - 14.0f, w.rect.bottom() + 4.0f, w.rect.w + 28.0f, 16.0f}, peqTypeName(type), 10.5f,
               lit ? kText : kDim);
        break;
    }
    case Kind::PeqTypeMenu:
        if (w.a == 1) {
            c.fillRect(w.rect, kRaised, 7.0f);
            c.strokeRect(w.rect, kBorder, 1.0f, 7.0f);
        }
        break;
    case Kind::PeqTypeItem: {
        const bool selected = peqType(value(w.param)) == static_cast<PeqType>(w.b);
        if (hovered || selected)
            c.fillRect(w.rect, fxColor(fxSlotOf(w.param)).alpha(hovered ? 0.18f : 0.1f), 5.0f);
        const int band = w.param - peqTypeIndex(fxSlotOf(w.param), 0);
        peqIcon(c, {w.rect.x + 8.0f, w.rect.y + 5.0f, 22.0f, w.rect.h - 10.0f}, static_cast<PeqType>(w.b),
                selected ? kSourceColors[band] : kDim);
        c.text({w.rect.x + 38.0f, w.rect.y, w.rect.w - 44.0f, w.rect.h}, peqTypeName(static_cast<PeqType>(w.b)), 11.0f,
               hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::ListItem: {
        // A band-pass's or notch's list shows the 6 and 18 dB slopes it doesn't come in faintly, and can't pick them.
        const bool fits = listItemFits(w.param, w.b);
        const bool selected = listShown(w.param) == w.b;
        if ((hovered && fits) || selected)
            c.fillRect(w.rect, listColor(w.param).alpha(hovered && fits ? 0.18f : 0.1f), 5.0f);
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, listItemName(w.param, w.b), 11.0f,
               !fits ? kFaint : hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::ShapePoint: { // compact: smaller, in a shaper's card
        const bool focused = hovered || active;
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 5.0f : 3.8f) : (focused ? 6.5f : 5.0f);
        c.fillCircle(w.rect.center(), radius, focused ? color : kPanel);
        c.strokeCircle(w.rect.center(), radius, color, w.compact ? 1.5f : 2.0f);
        break;
    }
    case Kind::ShapeCurve: {
        const bool focused = hovered || active;
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 3.8f : 2.4f) : (focused ? 4.5f : 3.0f);
        c.fillCircle(w.rect.center(), radius, focused ? kText : color.alpha(0.6f));
        break;
    }
    case Kind::EnvCurve: {
        const bool focused = hovered || active;
        const Color color = w.a < kOps ? kAccent : kSourceColors[w.a - kOps];
        c.fillCircle(w.rect.center(), focused ? 4.5f : 3.0f, focused ? kText : color.alpha(0.6f));
        break; // its readout goes over the handles (paintEnvReadout())
    }
    case Kind::PartialArea:
        break; // drawn with its editor
    case Kind::PartialReset:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text(w.rect, tr(Str::ButtonReset), 10.5f, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::ShapeDone:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text(w.rect, tr(Str::ButtonDone), 10.5f, kText, Align::Center, true);
        break;
    case Kind::Key:
        paintKey(c, w, hovered, active);
        break;
    case Kind::BendWheel:
    case Kind::ModWheel:
        paintWheel(c, w, hovered, active);
        break;
    case Kind::PresetButton: {
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        switch (static_cast<PresetAction>(w.a)) {
        case PresetAction::Init: {
            // A circular arrow, running clockwise from the upper right round to the top.
            constexpr float kFrom = 1.0f, kTo = 6.0f, kRadius = 6.0f;
            c.arc(m, kRadius, kFrom, kTo, color, 1.5f);
            const Point end = Canvas::onCircle(m, kRadius, kTo);
            const Point along{std::cos(kTo), std::sin(kTo)}, out{std::sin(kTo), -std::cos(kTo)};
            const Point head[] = {{end.x + along.x * 3.5f, end.y + along.y * 3.5f},
                                  {end.x + out.x * 3.0f, end.y + out.y * 3.0f},
                                  {end.x - out.x * 3.0f, end.y - out.y * 3.0f}};
            c.fillPolygon(head, std::size(head), color);
            break;
        }
        case PresetAction::Load: {
            // An open folder: a tab on the back, the front tilted forward.
            const Point back[] = {{m.x - 7.0f, m.y + 5.0f}, {m.x - 7.0f, m.y - 5.0f}, {m.x - 2.5f, m.y - 5.0f},
                                  {m.x - 1.0f, m.y - 3.5f}, {m.x + 5.0f, m.y - 3.5f}, {m.x + 5.0f, m.y - 1.0f}};
            const Point front[] = {{m.x - 7.0f, m.y + 5.0f}, {m.x - 4.5f, m.y - 1.0f}, {m.x + 7.5f, m.y - 1.0f},
                                   {m.x + 5.0f, m.y + 5.0f}, {m.x - 7.0f, m.y + 5.0f}};
            c.polyline(back, std::size(back), color, 1.4f);
            c.polyline(front, std::size(front), color, 1.4f);
            break;
        }
        case PresetAction::Save: {
            // A floppy disk: a clipped corner, the shutter at the top and the label below.
            const Point body[] = {{m.x - 6.5f, m.y - 6.5f}, {m.x + 3.5f, m.y - 6.5f}, {m.x + 6.5f, m.y - 3.5f},
                                  {m.x + 6.5f, m.y + 6.5f}, {m.x - 6.5f, m.y + 6.5f}, {m.x - 6.5f, m.y - 6.5f}};
            c.polyline(body, std::size(body), color, 1.4f);
            c.fillRect({m.x - 3.5f, m.y - 6.5f, 6.0f, 3.5f}, color);
            c.strokeRect({m.x - 3.5f, m.y + 1.0f, 7.0f, 5.5f}, color, 1.2f);
            break;
        }
        }
        break;
    }
    case Kind::Octave:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text(w.rect, w.a < 0 ? "-" : "+", 13.0f, kText, Align::Center, true);
        break;
    case Kind::SettingsButton: {
        const bool open = settingsOpen_;
        c.fillRect(w.rect, hovered || open ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        // A cog: eight teeth around a ring.
        const Point center = w.rect.center();
        const Color color = hovered || open ? kText : kDim;
        constexpr float kTau = 6.2831853f;
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = tooth * kTau / 8.0f;
            const float half = 0.2f;
            const Point outline[] = {Canvas::onCircle(center, 5.5f, angle - half * 1.3f),
                                     Canvas::onCircle(center, 8.0f, angle - half),
                                     Canvas::onCircle(center, 8.0f, angle + half),
                                     Canvas::onCircle(center, 5.5f, angle + half * 1.3f)};
            c.fillPolygon(outline, std::size(outline), color);
        }
        c.fillCircle(center, 6.0f, color);
        c.fillCircle(center, 2.6f, hovered || open ? kTrack : kRaised);
        break;
    }
    case Kind::SettingsBackdrop:
        break;
    case Kind::SettingsOption: {
        const bool on = controller_.oversampling() == w.b;
        c.fillRect(w.rect, on ? kAccent.alpha(0.22f) : (hovered ? kTrack : kRaised), 5.0f);
        c.strokeRect(w.rect, on ? kAccent : kBorder, 1.0f, 5.0f);
        char text[8];
        std::snprintf(text, sizeof(text), "%dx", w.b);
        c.text(w.rect, text, 11.0f, on ? kText : kDim, Align::Center, on);
        break;
    }
    case Kind::SettingsGlow: {
        // A track filled up to the amount, and a knob to drag along it.
        const float x = w.rect.x + shownGlow() * w.rect.w, y = w.rect.center().y;
        c.fillRect({w.rect.x, y - 2.0f, w.rect.w, 4.0f}, kTrack, 2.0f);
        c.fillRect({w.rect.x, y - 2.0f, x - w.rect.x, 4.0f}, kAccent, 2.0f);
        c.fillCircle({x, y}, 7.0f, hovered || active ? kText : kDim);
        c.fillCircle({x, y}, 3.0f, kAccent);
        break;
    }
    case Kind::SettingsTheme: {
        // Each theme in its own colors: its panel, its name, and its operator, filter and effect colors.
        const Palette &theme = kThemes[w.b];
        const bool on = themeIndex(controller_.theme()) == w.b;
        c.fillRect(w.rect, hovered && !on ? theme.raised : theme.panel, 5.0f);
        c.strokeRect(w.rect, on ? kAccent : (hovered ? kDim : kBorder), on ? 1.5f : 1.0f, 5.0f);
        c.text({w.rect.x, w.rect.y, w.rect.w, w.rect.h - 4.0f}, tr(theme.label), 10.5f, theme.text, Align::Center, on);
        const float third = (w.rect.w - 16.0f) / 3.0f;
        const Color stripe[] = {theme.accent, theme.filter, theme.fx};
        for (int i = 0; i < 3; ++i)
            c.fillRect({w.rect.x + 8.0f + i * third, w.rect.bottom() - 7.0f, third, 3.0f}, stripe[i]);
        break;
    }
    case Kind::ConfirmBackdrop:
    case Kind::BrowserBackdrop:
        break;
    case Kind::PresetNameBox: {
        // The preset's name, a dot while it has changed since it was loaded, and a chevron for the browser.
        const bool lit = hovered || browserOpen_;
        c.fillRect(w.rect, lit ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text({w.rect.x + 28.0f, w.rect.y, w.rect.w - 56.0f, w.rect.h}, controller_.presetName(), 11.5f, kText,
               Align::Center, true);
        if (controller_.patchEdited())
            c.fillCircle({w.rect.right() - 30.0f, w.rect.center().y}, 2.5f, kAccent);
        const Point m{w.rect.right() - 14.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        c.polyline(chevron, 3, lit ? kText : kDim, 1.5f);
        break;
    }
    case Kind::PresetStep: {
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        const Point m = w.rect.center();
        const float d = w.a < 0 ? 1.5f : -1.5f; // the chevron points the way it steps
        const Point chevron[] = {{m.x + d, m.y - 4.0f}, {m.x - d, m.y}, {m.x + d, m.y + 4.0f}};
        c.polyline(chevron, 3, hovered ? kText : kDim, 1.5f);
        break;
    }
    case Kind::BrowserRow: {
        const bool current = controller_.presetIndex() == w.a;
        if (current)
            c.fillRect(w.rect, kAccent.alpha(0.18f), 5.0f);
        else if (hovered)
            c.fillRect(w.rect, kTrack, 5.0f);
        if (current)
            c.strokeRect(w.rect, kAccent.alpha(0.6f), 1.0f, 5.0f);
        const std::vector<PresetEntry> &presets = controller_.presets();
        if (w.a < static_cast<int>(presets.size()))
            c.text({w.rect.x + 14.0f, w.rect.y, w.rect.w - 28.0f, w.rect.h}, presets[w.a].name, 11.5f,
                   current || hovered ? kText : kDim, Align::Left, current);
        break;
    }
    case Kind::BrowserClose: {
        c.fillRect(w.rect, hovered ? kTrack : kPanel, 5.0f);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 5.0f, m.y - 5.0f}, {m.x + 5.0f, m.y + 5.0f}, color, 1.5f);
        c.line({m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y - 5.0f}, color, 1.5f);
        break;
    }
    case Kind::BrowserFolder:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, 6.0f);
        c.strokeRect(w.rect, kBorder, 1.0f, 6.0f);
        c.text(w.rect, tr(Str::ButtonOpenFolder), 10.5f, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::ConfirmButton: {
        const bool ok = w.a == 1;
        c.fillRect(w.rect, ok ? kAccent.alpha(hovered ? 0.35f : 0.22f) : (hovered ? kTrack : kRaised), 6.0f);
        c.strokeRect(w.rect, ok ? kAccent : kBorder, 1.0f, 6.0f);
        const char *label = tr(!ok                                                ? Str::ButtonCancel
                               : confirm_ == static_cast<int>(PresetAction::Init) ? Str::ButtonInit
                                                                                  : Str::ButtonLoad);
        c.text(w.rect, label, 11.0f, kText, Align::Center, ok);
        break;
    }
    case Kind::SettingsClose: {
        c.fillRect(w.rect, hovered ? kTrack : kPanel, 5.0f);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 5.0f, m.y - 5.0f}, {m.x + 5.0f, m.y + 5.0f}, color, 1.5f);
        c.line({m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y - 5.0f}, color, 1.5f);
        break;
    }
    case Kind::ModDot: {
        // Compact: on a wire label, matrix cell or depth bar.
        // A bypassed modulation's dot is hollow.
        const Point center = w.rect.center();
        const float radius = hovered || active ? (w.compact ? 4.0f : 5.0f) : (w.compact ? 2.75f : 3.5f);
        const Color color = kSourceColors[w.a].alpha(sourceOn(w.a) ? 1.0f : 0.35f);
        if (slotBypassed(w.a, w.b))
            c.strokeCircle(center, radius - 0.6f, color, 1.2f);
        else
            c.fillCircle(center, radius, color);
        break;
    }
    case Kind::SlotBypass: {
        // The row's dot in the source's color: filled while it modulates, hollow while bypassed.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a];
        if (hovered)
            c.fillCircle(center, 6.5f, color.alpha(0.2f));
        if (slotBypassed(w.a, w.b))
            c.strokeCircle(center, 3.2f, color, 1.3f);
        else
            c.fillCircle(center, 3.5f, color);
        break;
    }
    }
}

// The name tags of a hovered wave button and of a hovered or dragged mod dot, painted after
// every widget so the ones below don't cover them.
void View::paintHoverTags(Canvas &c, const std::vector<Widget> &list) {
    for (const Widget &w : list) {
        if ((((w.kind == Kind::WaveButton || w.kind == Kind::WaveMore) && waveMenu_ < 0) ||
             w.kind == Kind::TypeButton) &&
            isHovered(w)) {
            const auto picked = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
            const char *name = w.kind == Kind::TypeButton   ? filterTypeName(static_cast<FilterType>(w.b))
                               : w.kind == Kind::WaveButton ? waveName(static_cast<OpWave>(w.b))
                               : isMoreWave(picked)         ? waveName(picked)
                                                            : tr(Str::TipMoreWaves);
            const Rect tip{w.rect.center().x - 40.0f, w.rect.bottom() + 3.0f, 80.0f, 16.0f};
            c.fillRect(tip, kRaised, 5.0f);
            c.text(tip, name, 10.5f, kText);
        } else if (w.kind == Kind::ModDot && (isHovered(w) || isActive(w))) {
            const Point center = w.rect.center();
            const std::string text = modRangeText(w.a, w.b);
            const float width = std::ceil(c.textWidth(text, 10.5f)) + 16.0f;
            const float x = std::clamp(center.x - width * 0.5f, 4.0f, View::kWidth - 4.0f - width);
            const Rect tip{x, center.y + 8.0f, width, 18.0f};
            c.fillRect(tip, kRaised, 5.0f);
            c.text(tip, text, 10.5f, kText);
        }
    }
}

// A modulation in words: its source, and what it takes its target through in the target's own units, from
// half the depth below the set value to half above for a bipolar source, or from it one way for a unipolar one.
std::string View::modRangeText(int source, int slot) const {
    std::string text = sourceName(source);
    const int target = slotTarget(value(slotTargetIndex(source, slot)));
    if (target >= 0) {
        const ParamDef &def = table_[target];
        const double set = value(target);
        const double reach = value(slotAmountIndex(source, slot)) * (def.max - def.min);
        const bool both = sourceBipolar(source);
        const double start = both ? set - 0.5 * reach : set; // a negative depth runs the other way
        const double end = both ? set + 0.5 * reach : set + reach;
        char from[32], to[32];
        formatParam(target, std::clamp(start, def.min, def.max), from, sizeof(from));
        formatParam(target, std::clamp(end, def.min, def.max), to, sizeof(to));
        text += std::string("  ") + from + " \xE2\x86\x92 " + to;
    }
    if (slotBypassed(source, slot))
        text += std::string("  ") + tr(Str::CaptionBypassed);
    return text;
}

void View::paintKnob(Canvas &c, const Widget &w, const std::vector<Assignment> &mods) {
    const ParamDef &def = table_[w.param];
    const bool focused = isHovered(w) || isActive(w);
    const Point center = w.compact ? Point{w.rect.x + 18.0f, w.rect.y + 20.0f}
                                   : Point{w.rect.center().x, w.rect.y + w.rect.w * 0.5f};
    const float radius = w.compact ? 17.0f : w.rect.w * 0.5f;
    const double n = norm(w.param);
    const float angle = knobAngle(n);

    c.arc(center, radius - 2.0f, kKnobStart, kKnobStart + kKnobSweep, focused ? kFaint : kTrack, 3.5f);
    // A macro's knob in its handle's color, faint like the handle, and its name too, while it modulates nothing.
    const int macro = macroOf(w.param);
    const bool idle = macro >= 0 && !sourceLive(kMaxSources + macro);
    const Color arc = macro >= 0 ? kSourceColors[kMaxSources + macro].alpha(idle ? 0.3f : 1.0f) : kAccent;
    // A bipolar macro moves nothing at the knob's middle, so its arc starts there.
    const bool fromMiddle = isBipolar(def.kind) || (macro >= 0 && sourceBipolar(kMaxSources + macro));
    c.arc(center, radius - 2.0f, fromMiddle ? 0.0f : kKnobStart, angle, arc, 3.5f);

    // Modulation ranges just outside the track, one ring per source. Bipolar sources swing both ways.
    // A switched-off source's ring is faint: it is kept, but does nothing. The one whose depth handle is under the
    // mouse is wider, over the others.
    const Assignment *focus = nullptr;
    for (const Assignment &m : mods) {
        if (m.target != w.param)
            continue;
        if (modFocused(m)) {
            focus = &m;
            continue;
        }
        const bool both = sourceBipolar(m.source);
        const auto [down, up] = modReach(m.source, m.amount);
        const float alpha = (both ? 0.6f : 0.9f) * (sourceOn(m.source) && !m.bypassed ? 1.0f : 0.3f);
        c.arc(center, radius + 2.5f, knobAngle(n + down), knobAngle(n + up), kSourceColors[m.source].alpha(alpha), 2.5f);
    }
    if (focus) {
        const auto [down, up] = modReach(focus->source, focus->amount);
        const float alpha = sourceOn(focus->source) && !focus->bypassed ? 1.0f : 0.4f;
        c.arc(center, radius + 3.0f, knobAngle(n + down), knobAngle(n + up), kSourceColors[focus->source].alpha(alpha),
              4.0f);
    }

    c.fillCircle(center, radius - 7.0f, kKnobBody);
    c.strokeCircle(center, radius - 7.0f, kBorder, 1.0f);
    c.line(Canvas::onCircle(center, (radius - 7.0f) * 0.3f, angle), Canvas::onCircle(center, radius - 9.5f, angle),
           kText, 2.0f);

    // Where modulation currently takes the newest voice.
    const float offset = liveOffset(w.param);
    if (telemetry_.voices > 0 && offset != 0.0f)
        c.fillCircle(Canvas::onCircle(center, radius - 2.0f, knobAngle(n + offset / (def.max - def.min))), 2.5f,
                     kText);

    char text[64];
    formatParam(w.param, value(w.param), text, sizeof(text));
    std::string shown = text;
    if (def.kind == ParamKind::Ratio)
        shown = "\xC3\x97" + shown;
    if (w.compact) {
        const float textWidth = w.rect.w - 42.0f;
        c.text({w.rect.x + 42.0f, w.rect.y + 3.0f, textWidth, 16.0f}, w.label, 10.5f, kDim, Align::Left);
        c.text({w.rect.x + 42.0f, w.rect.y + 18.0f, textWidth, 18.0f}, shown, 13.0f, kText, Align::Left, true);
    } else {
        const Rect label{w.rect.x - 14.0f, w.rect.y + w.rect.w + 2.0f, w.rect.w + 28.0f, 16.0f};
        c.text(label, focused || !w.label ? shown : std::string(w.label), 10.5f,
               focused ? kText : (idle ? kFaint : kDim));
    }
}

// What the play switches do, below the one under the mouse.
void View::paintTooltip(Canvas &c) {
    if (!hover_ || drag_.kind != DragKind::None ||
        (hover_->kind != Kind::Choice && hover_->kind != Kind::Toggle && hover_->kind != Kind::NodeTab &&
         hover_->kind != Kind::FxRemove && hover_->kind != Kind::SourceRemove &&
         hover_->kind != Kind::SourceAdd && hover_->kind != Kind::PresetButton &&
         hover_->kind != Kind::PresetStep && hover_->kind != Kind::PresetNameBox &&
         hover_->kind != Kind::BrowserFolder &&
         hover_->kind != Kind::SettingsButton && hover_->kind != Kind::UndoButton &&
         hover_->kind != Kind::ShapeReset && hover_->kind != Kind::SlotBypass && hover_->kind != Kind::MacroName &&
         hover_->kind != Kind::MacroHandle && hover_->kind != Kind::MatrixLabel && hover_->kind != Kind::FxPower &&
         hover_->kind != Kind::NoteDisplay && hover_->kind != Kind::WireEnd && hover_->kind != Kind::Port &&
         hover_->kind != Kind::McBand && hover_->kind != Kind::FxInsert &&
         !(hover_->kind == Kind::ListButton && isFilterSlope(hover_->param)) &&
         !(hover_->kind == Kind::ShapeGrid && hover_->b != 0)))
        return;
    const int param = hover_->param;
    const FxParam field = fxFieldOf(param);
    std::string text;
    if (hover_->kind == Kind::MacroName) {
        text = tr(Str::TipRenameMacro);
    } else if (hover_->kind == Kind::NoteDisplay) {
        text = tr(Str::TipNoteDisplay);
    } else if (hover_->kind == Kind::WireEnd) {
        text = tr(Str::TipWireEnd);
    } else if (hover_->kind == Kind::Port) {
        text = tr(Str::TipPort);
    } else if (hover_->kind == Kind::FxInsert) {
        text = tr(Str::TipFxInsert);
    } else if (hover_->kind == Kind::McBand) {
        const Str bands[] = {Str::BandLow, Str::BandMid, Str::BandHigh};
        text = value(paramIndex(hover_->a, FxParam::McLink)) >= 0.5 ? std::string(tr(Str::TipMcBandLinked))
                                                                     : trf(Str::TipMcBand, {tr(bands[hover_->b])});
    } else if (hover_->kind == Kind::MacroHandle && isWheel(hover_->a)) {
        text = tr(Str::TipWheelHandle);
    } else if (hover_->kind == Kind::MacroHandle && isNote(hover_->a)) {
        text = tr(Str::TipNoteHandle);
    } else if (hover_->kind == Kind::MacroHandle) {
        text = trf(Str::TipMacroHandle, {macroName(controller_.layout(), hover_->a - kMaxSources)});
    } else if (hover_->kind == Kind::NodeTab || hover_->kind == Kind::MatrixLabel) {
        const bool on = nodeOn(hover_->a);
        const std::string title = nodeTitle(hover_->a);
        text = controller_.layout().selectedNode != hover_->a
                   ? trf(on ? Str::TipShowNode : Str::TipSwitchOnAndShow, {title})
                   : trf(on ? Str::TipSwitchNodeOff : Str::TipSwitchNodeOn, {title});
    } else if (hover_->kind == Kind::NodeGhost) {
        text = trf(Str::TipNodeGhost, {nodeTitle(hover_->a)});
    } else if (isPartialCount(param)) {
        text = trf(Str::TipPartialCount, {std::to_string(hover_->b)});
    }
    else if (hover_->kind == Kind::PresetStep)
        text = tr(hover_->a < 0 ? Str::TipPreviousPreset : Str::TipNextPreset);
    else if (hover_->kind == Kind::PresetNameBox)
        text = tr(controller_.patchEdited() ? Str::TipPresetChanged : Str::TipPresetName);
    else if (hover_->kind == Kind::BrowserFolder)
        text = tr(Str::TipOpenFolder);
    else if (hover_->kind == Kind::PresetButton)
        text = tr(hover_->a == 0 ? Str::TipInit : hover_->a == 1 ? Str::TipLoad : Str::TipSave);
    else if (hover_->kind == Kind::SettingsButton)
        text = tr(Str::TipSettings);
    else if (hover_->kind == Kind::SlotBypass)
        text = tr(slotBypassed(hover_->a, hover_->b) ? Str::TipBypassed : Str::TipBypass);
    else if (hover_->kind == Kind::UndoButton)
        text = undoText(hover_->a == 1);
    else if (hover_->kind == Kind::ShapeReset)
        text = tr(isNoteCurve(hover_->a)                                ? Str::TipResetNoteCurve
                  : isClipCurve(hover_->a) || isShaperCurve(hover_->a) ? Str::TipResetCurve
                                                                       : Str::TipResetShape);
    else if (hover_->kind == Kind::ShapeGrid)
        text = tr(hover_->a == 0 ? Str::TipGridAcross : Str::TipGridUpDown);
    else if (hover_->kind == Kind::FxRemove)
        text = tr(Str::TipFxRemove);
    else if (hover_->kind == Kind::SourceRemove)
        text = tr(Str::TipSourceRemove);
    else if (hover_->kind == Kind::SourceAdd)
        text = tr(static_cast<SourceType>(hover_->a) == SourceType::Lfo ? Str::TipAddLfo : Str::TipAddEnv);
    else if (isPowerParam(param))
        text = tr(Str::TipNodePower);
    else if (isSourcePower(param))
        text = tr(Str::TipSourcePower);
    else if (param == paramIndex(GlobalParam::Mono))
        text = tr(hover_->b == 0 ? Str::TipPoly : Str::TipMono);
    else if (param == paramIndex(GlobalParam::Legato))
        text = tr(Str::TipLegato);
    else if (field == FxParam::Enabled) // the icon on its card
        text = tr(value(param) >= 0.5 ? Str::TipFxOn : Str::TipFxOff);
    else if (isLfoSync(param))
        text = tr(Str::TipLfoSync);
    else if (hover_->kind == Kind::ListButton && isFilterSlope(param))
        text = tr(slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), 0) ? Str::TipSlope
                                                                                                  : Str::TipSlopeBand);
    else if (isLfoFree(param))
        text = tr(Str::TipFree);
    else if (sourceFieldOf(param) == SourceParam::OneShot)
        text = tr(Str::TipOneShot);
    else if (sourceFieldOf(param) == SourceParam::Unipolar)
        text = tr(Str::TipUnipolar);
    else if (field == FxParam::DelaySync)
        text = tr(Str::TipDelaySync);
    else if (field == FxParam::PhaserSync || field == FxParam::FlangerSync)
        text = tr(Str::TipSweepSync);
    else if (field == FxParam::DelayPingPong)
        text = tr(Str::TipPingPong);
    else if (field == FxParam::DelayLink)
        text = tr(Str::TipDelayLink);
    else if (field == FxParam::McLink)
        text = tr(Str::TipBandLink);
    else if (field == FxParam::PhaserStereo || field == FxParam::FlangerStereo)
        text = tr(Str::TipStereo);
    else if (field == FxParam::ShaperLink || shaperFieldOf(param) == ShaperParam::Link)
        text = tr(Str::TipShaperLink);
    else if (field == FxParam::FilterType)
        text = filterTypeName(static_cast<FilterType>(hover_->b));
    else if (field == FxParam::ShaperMode)
        text = tr(hover_->b == 0 ? Str::TipClipHard : hover_->b == 1 ? Str::TipClipSoft : Str::TipClipAsym);
    else
        text = tr(Str::TipGlideAlways);
    // As wide as its text, centered under the widget and kept inside the editor.
    const float width = std::min(std::ceil(c.textWidth(text, 11.0f)) + 24.0f, View::kWidth - 16.0f);
    const float x = std::clamp(hover_->rect.center().x - width * 0.5f, 8.0f, View::kWidth - 8.0f - width);
    const Rect tip{x, hover_->rect.bottom() + 6.0f, width, 22.0f};
    c.fillRect(tip, kRaised, 6.0f);
    c.strokeRect(tip, kBorder, 1.0f, 6.0f);
    c.text(tip, text, 11.0f, kText);
}

void View::paintModOverlay(Canvas &c, const std::vector<Widget> &list) {
    const int source = drag_.widget.a;
    const Color color = kSourceColors[source];
    const auto target = hitTest(drag_.now);
    for (const Widget &w : list) {
        const bool droppable = w.param >= 0 && isModTarget(w.param) &&
                               (w.kind == Kind::Knob || w.kind == Kind::WireLabel || w.kind == Kind::OutLabel ||
                                w.kind == Kind::GridCell || w.kind == Kind::SlotBar);
        if (!droppable)
            continue;
        const bool over = target && target->same(w);
        const Color ring = color.alpha(over ? 1.0f : 0.35f);
        if (w.kind == Kind::Knob) {
            c.strokeCircle(knobCenter(w), knobRadius(w) + 6.0f, ring, over ? 2.5f : 1.5f);
        } else {
            c.strokeRect(w.rect.inset(w.kind == Kind::GridCell ? -1.0f : -3.0f), ring, over ? 2.0f : 1.0f, 6.0f);
        }
    }
    c.line(drag_.start, drag_.now, color.alpha(0.6f), 1.5f, true);
    c.fillCircle(drag_.now, 7.0f, color.alpha(0.5f));
    c.fillCircle(drag_.now, 3.0f, color);
}

void View::paintSourceLinks(Canvas &c, const std::vector<Widget> &list, int source) {
    std::vector<int> targets, bypassed;
    for (const Assignment &m : assignments())
        if (m.source == source)
            (m.bypassed ? bypassed : targets).push_back(m.target);
    const auto handle = std::find_if(list.begin(), list.end(),
                                     [&](const Widget &w) {
                                         return (w.kind == Kind::SourceHandle || w.kind == Kind::MacroHandle) &&
                                                w.a == source;
                                     });
    if ((targets.empty() && bypassed.empty()) || handle == list.end())
        return;
    const Point from = handle->rect.center();
    for (const Widget &w : list) {
        const bool target = w.kind == Kind::Knob || w.kind == Kind::WireLabel || w.kind == Kind::OutLabel ||
                            w.kind == Kind::GridCell || w.kind == Kind::SlotBar; // the last: another source's depth
        const bool modulated = target && std::find(targets.begin(), targets.end(), w.param) != targets.end();
        const bool off = target && std::find(bypassed.begin(), bypassed.end(), w.param) != bypassed.end();
        if (!modulated && !off)
            continue;
        // The line ends at the source's own dot on the target: the one under a knob, or in the top right corner
        // of a label, cell or depth bar. A param can show twice (an output's knob and its label on the bus), and
        // each has its dots, so the dot is the one on this widget.
        const auto dot = std::find_if(list.begin(), list.end(), [&](const Widget &d) {
            return d.kind == Kind::ModDot && d.a == source && w.rect.inset(-2.0f).contains(d.rect.center()) &&
                   slotTarget(value(slotTargetIndex(source, d.b))) == w.param;
        });
        const Point to = dot != list.end() ? dot->rect.center() : (w.kind == Kind::Knob ? knobCenter(w) : w.rect.center());
        if (w.clip.w > 0.0f && !w.clip.contains(to))
            continue;
        const Color color = kSourceColors[source].alpha(sourceOn(source) && modulated ? 1.0f : 0.4f); // bypassed: faint
        // Bowed to one side, so lines to targets in a row fan out rather than run over each other.
        const Point d{to.x - from.x, to.y - from.y};
        const Point control{(from.x + to.x) * 0.5f + d.y * 0.15f, (from.y + to.y) * 0.5f - d.x * 0.15f};
        c.curve(from, control, to, color.alpha(0.75f), 1.5f, true);
        if (dot != list.end()) // a ring round the dot the line points at
            c.strokeCircle(to, dot->compact ? 5.5f : 7.5f, color, 1.5f);
        if (w.kind == Kind::Knob)
            c.strokeCircle(knobCenter(w), knobRadius(w) + 5.0f, color.alpha(0.5f), 1.0f);
        else
            c.strokeRect(w.rect.inset(w.kind == Kind::GridCell ? -1.0f : -3.0f), color.alpha(0.5f), 1.0f, 6.0f);
    }
}

Rect View::typingField() const {
    const Widget &w = typing_->widget;
    if (w.kind == Kind::Knob && w.compact)
        return {w.rect.x + 38.0f, w.rect.y + 17.0f, w.rect.w - 36.0f, 20.0f};
    if (w.kind == Kind::Knob)
        return {w.rect.center().x - 40.0f, w.rect.y + w.rect.w + 1.0f, 80.0f, 18.0f};
    if (w.kind == Kind::ModDot) // under the small dot
        return {w.rect.center().x - 36.0f, w.rect.bottom() + 3.0f, 72.0f, 18.0f};
    if (w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::GridCell) // over the label or cell
        return {w.rect.center().x - 36.0f, w.rect.center().y - 9.0f, 72.0f, 18.0f};
    return {w.rect.x - 4.0f, w.rect.y, w.rect.w + 8.0f, w.rect.h};
}

// The text field, all of it selected until the first key, with a blinking caret.
void View::paintTyping(Canvas &c) {
    const Typing &t = *typing_;
    const Rect field = typingField();
    c.fillRect(field, kBackground, 4.0f);
    c.strokeRect(field, kAccent, 1.0f, 4.0f);
    constexpr float kSize = 11.0f;
    const float width = c.textWidth(t.text, kSize);
    const float x = std::max(field.center().x - width * 0.5f, field.x + 4.0f);
    if (t.selected && !t.text.empty())
        c.fillRect({x - 1.0f, field.y + 3.0f, width + 2.0f, field.h - 6.0f}, kAccent.alpha(0.35f), 2.0f);
    c.text({x, field.y, field.right() - 2.0f - x, field.h}, t.text, kSize, kText, Align::Left);
    if (caretTicks_ / 32 % 2 == 0) {
        const float caret = x + c.textWidth(std::string_view(t.text).substr(0, t.caret), kSize);
        c.line({caret, field.y + 3.0f}, {caret, field.bottom() - 3.0f}, kText, 1.0f);
    }
}

// "Undo Op A Fine", or how many params the step changes.
std::string View::undoText(bool redo) const {
    const UndoStep *step = redo ? controller_.nextRedo() : controller_.nextUndo();
    if (!step)
        return tr(redo ? Str::RedoNothing : Str::UndoNothing);
    if (step->changes.size() == 1)
        return trf(redo ? Str::RedoParam : Str::UndoParam, {paramName(step->changes.front().index)});
    if (step->changes.empty())
        return tr(redo ? Str::RedoLayout : Str::UndoLayout);
    return trf(redo ? Str::RedoChanges : Str::UndoChanges, {std::to_string(step->changes.size())});
}

// --- Input.

namespace {

std::vector<int> envPointParams(int env, int point) {
    const EnvIndices e = envIndices(env);
    if (point == 0)
        return {e.attack};
    if (point == 1)
        return {e.hold};
    if (point == 2)
        return {e.decay, e.sustain};
    return {e.release};
}

std::vector<int> filterPointParams(int filter) {
    return {paramIndex(filter, FilterParam::Cutoff), paramIndex(filter, FilterParam::Resonance)};
}

// A cut's handle sets its Q instead of its gain; a drag may touch either.
std::vector<int> peqPointParams(int slot, int band) {
    return {peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band)};
}

} // namespace

void View::applyDrag(Point p) {
    const Widget &w = drag_.widget;
    auto set = [&](int index, double v) {
        v = std::clamp(v, table_[index].min, table_[index].max);
        if (v != value(index))
            controller_.setValue(index, v);
    };
    switch (drag_.kind) {
    case DragKind::Key: {
        // Sliding across keys moves the note; off the keys, the last one keeps sounding.
        const auto hit = hitTest(p);
        if (hit && hit->kind == Kind::Key && hit->a != w.a) {
            controller_.playNote(keyboardBase() + w.a, false);
            controller_.playNote(keyboardBase() + hit->a, true);
            drag_.widget.a = hit->a;
        }
        break;
    }
    // The sliders follow the mouse; with Shift, they move a tenth as fast from where they were.
    case DragKind::ModWheel:
        set(w.param, drag_.fine ? drag_.startValue + (drag_.start.y - p.y) / w.rect.h * 0.1
                                : (w.rect.bottom() - p.y) / w.rect.h);
        break;
    case DragKind::Glow:
        glowDragged_ = std::clamp(drag_.fine ? static_cast<float>(drag_.startValue) + (p.x - drag_.start.x) / w.rect.w * 0.1f
                                             : (p.x - w.rect.x) / w.rect.w,
                                  0.0f, 1.0f);
        break;
    case DragKind::BendWheel: {
        const double mid = w.rect.y + w.rect.h * 0.5;
        bend_ = std::clamp((mid - p.y) / (w.rect.h * 0.5), -1.0, 1.0);
        controller_.pitchWheel(bend_);
        break;
    }
    case DragKind::Value: {
        const ParamDef &def = table_[w.param];
        if (w.kind == Kind::GridCell) // dragging a matrix cell switches on what it joins, in the same step
            switchOnRoute(w.a, w.b);
        if (w.kind == Kind::SlotBar)
            set(w.param, drag_.fine ? drag_.startValue + (p.x - drag_.start.x) / (w.rect.w * 0.5f) * 0.1
                                    : (p.x - w.rect.center().x) / (w.rect.w * 0.5f));
        else if (def.kind == ParamKind::VoiceCount) // doubles every 40 px
            set(w.param, std::round(drag_.startValue * std::exp2((drag_.start.y - p.y) / 40.0)));
        else if (snapsToSteps(def.kind)) { // a step every 10 px; a note lands on a whole one
            const double stepped = drag_.startValue + std::round((drag_.start.y - p.y) / 10.0f);
            set(w.param, def.kind == ParamKind::NotePitch ? std::round(stepped) : stepped);
        }
        else
            set(w.param, drag_.startValue + (drag_.start.y - p.y) / (drag_.fine ? 1000.0 : 200.0) * (def.max - def.min));
        break;
    }
    case DragKind::Node: {
        UiLayout &layout = controller_.layout();
        const float x = std::clamp((p.x - kGraph.x) / kGraph.w, 0.0f, 1.0f);
        const float y = std::clamp((p.y - kGraph.y) / kGraph.h, 0.0f, 1.0f);
        if (isBoxNode(w.a)) {
            layout.boxX[w.a - kOps] = x;
            layout.boxY[w.a - kOps] = y;
        } else {
            layout.nodeX[w.a] = x;
            layout.nodeY[w.a] = y;
        }
        break;
    }
    case DragKind::EnvPoint: {
        // Times go by the zoom the drag started with, so the point stays under the mouse; dragged past the
        // display's end, it goes on lengthening its segment while the display zooms out (see envScale()).
        const EnvIndices e = envIndices(w.a);
        const EnvShape s = envShape(envRect(w.a), e, [this](int index) { return value(index); }, drag_.envScale);
        auto time = [&](float from) { return (p.x - from - kEnvMinSegment) / s.scale; };
        if (w.b == 0) {
            set(e.attack, time(s.p0.x));
        } else if (w.b == 1) {
            set(e.hold, time(s.p1.x)); // to the left of its shortest place: none
        } else if (w.b == 2) {
            set(e.decay, time(s.ph.x));
            set(e.sustain, (s.bottom - p.y) / (s.bottom - s.top));
        } else {
            set(e.release, time(s.p3.x));
        }
        break;
    }
    case DragKind::EnvCurve:
        // Dragging up lifts the middle of the segment: a faster attack start, a slower decay or release start.
        set(w.param, drag_.startValue + (w.b == 0 ? 1.0 : -1.0) * (drag_.start.y - p.y) / 100.0);
        break;
    case DragKind::FilterPoint: {
        const Rect r = kFilterDisplay;
        set(paramIndex(w.a, FilterParam::Cutoff), (p.x - r.x - 10.0f) / (r.w - 20.0f));
        set(paramIndex(w.a, FilterParam::Resonance), (r.bottom() - 10.0f - p.y) / (r.h - 20.0f));
        break;
    }
    case DragKind::PeqPoint: {
        const Rect area = peqArea(w.a);
        set(peqFreqIndex(w.a, w.b), (p.x - area.x) / area.w);
        const double db = (area.center().y - p.y) / (area.h * 0.5f) * kPeqRangeDb;
        if (isCut(peqType(value(peqTypeIndex(w.a, w.b))))) // up and down is the resonance's peak
            set(peqQIndex(w.a, w.b), normFromQ(std::clamp(std::pow(10.0, db / 20.0), kMinQ, kMaxQ)));
        else
            set(peqGainIndex(w.a, w.b), db);
        break;
    }
    case DragKind::ShapePoint: {
        // Points keep their order: each stays between its neighbors. The first one stays at the
        // start, and a curve's last one at the end.
        const int shape = w.a;
        const int point = w.b;
        double x = shapeXAt(shape, p);
        double y = shapeYAt(shape, p);
        if (drag_.snap) { // to the grid's lines
            const UiLayout &layout = controller_.layout();
            x = std::round(x * layout.shapeGridX) / layout.shapeGridX;
            const double half = 0.5 * layout.shapeGridY; // lines per unit of height
            y = std::round((y + 1.0) * half) / half - 1.0;
        }
        const int count = shapePointCount(shape);
        if (point > 0 && !(isCurve(shape) && point + 1 == count)) {
            const double low = shapePointX(shape, point - 1);
            const double high = point + 1 < count ? shapePointX(shape, point + 1) : 1.0;
            set(shapeIndex(shape, point, PointField::X), std::max(low + 0.002, std::min(x, high - 0.002)));
        }
        set(shapeIndex(shape, point, PointField::Y), y);
        break;
    }
    case DragKind::Partials: {
        // A quick sweep sets every bar it passed, along a line (in bar heights) from where the last move was.
        const int op = w.a;
        const Rect area = w.b == 0 ? kPartialLevels : kPartialPhases;
        const int k = partialAt(area, p.x, partialsShown(op));
        const double v = std::clamp((area.bottom() - p.y) / area.h, 0.0f, 1.0f);
        const int from = drag_.lastPartial < 0 ? k : drag_.lastPartial;
        const double fromValue = drag_.lastPartial < 0 ? v : drag_.startValue;
        for (int j = std::min(from, k); j <= std::max(from, k); ++j) {
            const double t = from == k ? 1.0 : static_cast<double>(j - from) / (k - from);
            const double bar = fromValue + (v - fromValue) * t;
            if (w.b == 0)
                set(partialLevelIndex(op, j), partialLevelFromBar(bar));
            else
                set(partialPhaseIndex(op, j), bar);
        }
        drag_.lastPartial = k;
        drag_.startValue = v;
        break;
    }
    case DragKind::ShapeCurve: {
        // Dragging up lifts the middle of the segment, whichever way it runs.
        const int shape = w.a;
        const int next = w.b + 1 == shapePointCount(shape) ? 0 : w.b + 1;
        const double rise = value(shapeIndex(shape, next, PointField::Y)) - value(shapeIndex(shape, w.b, PointField::Y));
        set(w.param, drag_.startValue - (rise < 0.0 ? -1.0 : 1.0) * (drag_.start.y - p.y) / 100.0);
        break;
    }
    case DragKind::SlotScroll: {
        // The rows follow the thumb, a whole row at a time.
        const int rows = static_cast<int>(usedSlots(w.a).size());
        const float card = cardOf(w.a);
        if (card == kOffScreen || rows <= kVisibleSlots)
            break;
        const float room = slotScrollTrack(card).h - slotThumb(w.a, card).h;
        scrollSlots(w.a, static_cast<float>(drag_.startValue) + (p.y - drag_.start.y) / room * (rows - kVisibleSlots),
                    false); // the rows follow the thumb
        break;
    }
    case DragKind::Scroll: {
        // The cards follow the thumb.
        const auto area = static_cast<ScrollArea>(w.a);
        const float cards = scrollLength(area);
        const float shown = static_cast<float>(area == ScrollArea::Fx ? kVisibleFx : kVisibleCards);
        const float room = w.rect.w - 8.0f - scrollThumb(area).w;
        if (room > 0.0f)
            setScroll(area, static_cast<float>(drag_.startValue) + (p.x - drag_.start.x) / room * (cards - shown));
        break;
    }
    case DragKind::None:
    case DragKind::Connect:
    case DragKind::Reroute:
    case DragKind::ModSource:
    case DragKind::FxMove:
    case DragKind::SourceMove:
        break;
    }
}

void View::mouseMove(Point p, Modifiers mods) {
    hoverPoint_ = p;
    if (drag_.kind == DragKind::None) {
        hover_ = hitTest(p);
        return;
    }
    if ((drag_.kind == DragKind::Value || drag_.kind == DragKind::ModWheel || drag_.kind == DragKind::Glow) &&
        mods.shift != drag_.fine) {
        // Switching to/from fine mode re-anchors the drag so the value doesn't jump.
        drag_.fine = mods.shift;
        drag_.start = p;
        drag_.startValue = drag_.kind == DragKind::Glow ? glowDragged_ : value(drag_.widget.param);
    }
    drag_.snap = mods.ctrl;
    drag_.now = p;
    if (std::hypot(p.x - drag_.start.x, p.y - drag_.start.y) > 3.0f)
        drag_.moved = true;
    applyDrag(p);
}

void View::mouseDown(Point p, Modifiers mods) {
    // A click in the field being typed in leaves it be; anywhere else, it takes the value typed.
    if (typing_ && typingField().contains(p))
        return;
    endTyping(true);
    if (drag_.step) // a drag that never saw its button go up
        controller_.endStep();
    drag_ = Drag{};
    // Before a drag can take a route down to 0 %: the poll timer waits while the mouse moves, and a
    // state load may have replaced the layout since it last ran.
    keepRoutes();
    const auto hit = hitTest(p);
    if (!hit)
        return;
    const Widget &w = *hit;
    // Ctrl+Click types a value into whatever shows one: a knob, a modulation's depth, a route.
    if (mods.ctrl && (w.kind == Kind::Knob || w.kind == Kind::ValueText || w.kind == Kind::WireLabel ||
                      w.kind == Kind::OutLabel || w.kind == Kind::GridCell)) {
        startTyping(w);
        return;
    }
    if (drawsStep(w, mods)) {
        addShapeStep(w.a, p, mods.ctrl);
        return;
    }
    drag_.widget = w;
    drag_.start = drag_.now = p;
    drag_.fine = mods.shift;
    UiLayout &layout = controller_.layout();

    switch (w.kind) {
    case Kind::Knob:
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
    case Kind::ModDot:
    case Kind::SlotBar:
        drag_.kind = DragKind::Value;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        if (w.kind == Kind::SlotBar && !drag_.fine) // to the mouse at once; with Shift, from where it is
            applyDrag(p);
        break;
    case Kind::Node:
        layout.selectedNode = w.a;
        drag_.kind = DragKind::Node;
        drag_.step = true; // moving it is an undo step
        controller_.beginStep();
        break;
    case Kind::Port:
        drag_.kind = DragKind::Connect;
        break;
    case Kind::WireEnd:
        drag_.kind = DragKind::Reroute;
        break;
    case Kind::SourceHandle:
    case Kind::MacroHandle:
        drag_.kind = DragKind::ModSource;
        break;
    case Kind::EnvPoint:
        drag_.envScale = envScale(w.a); // before the drag starts, while it still fits the display
        drag_.kind = DragKind::EnvPoint;
        for (int index : envPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::EnvCurve:
        drag_.envScale = envScale(w.a);
        drag_.kind = DragKind::EnvCurve;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::FilterPoint:
        drag_.kind = DragKind::FilterPoint;
        for (int index : filterPointParams(w.a))
            controller_.beginEdit(index);
        break;
    case Kind::PeqPoint:
        if (mods.alt) { // up and down for its Q, as on a knob; the handle stays where it is
            drag_.kind = DragKind::Value;
            drag_.startValue = value(w.param);
            controller_.beginEdit(w.param);
            break;
        }
        drag_.kind = DragKind::PeqPoint;
        for (int index : peqPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::FxMenuButton:
        if (fxChain().size() >= static_cast<size_t>(kFxSlots)) {
            flash(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        } else {
            fxMenuOpen_ = !fxMenuOpen_ || fxMenuAt_.x >= 0.0f; // from another list, to the button's
            fxMenuAt_ = {-1.0f, -1.0f};
            fxInsertAt_ = -1;
        }
        break;
    case Kind::FxShadow: // the list over the card, kept inside the panel
        fxMenuAt_ = {std::min(w.rect.x + 8.0f, kFxPanel.right() - 8.0f - fxMenu({0.0f, 0.0f}).w), w.rect.y + 8.0f};
        fxMenuOpen_ = true;
        fxInsertAt_ = -1;
        hover_.reset();
        break;
    case Kind::FxInsert: { // the list over the cards on either side, its middle at the arrow, kept inside the panel
        const float width = fxMenu({0.0f, 0.0f}).w;
        fxMenuAt_ = {std::clamp(w.rect.center().x - 0.5f * width, kFxPanel.x + 8.0f, kFxPanel.right() - 8.0f - width),
                     kFxTop + 8.0f};
        fxMenuOpen_ = true;
        fxInsertAt_ = w.a;
        hover_.reset();
        break;
    }
    case Kind::SourceShadow:
        addSource(static_cast<SourceType>(w.a));
        break;
    case Kind::FxMenu:
        if (w.a == 0) // a click anywhere else closes the list
            fxMenuOpen_ = false;
        break;
    case Kind::FxAdd:
        fxMenuOpen_ = false;
        addEffect(static_cast<FxType>(w.a), -1, fxInsertAt_);
        fxInsertAt_ = -1;
        break;
    case Kind::FxRemove:
        removeEffect(w.a);
        break;
    case Kind::Scrollbar: {
        // On the thumb, drag it; elsewhere on the track, the thumb jumps to center on the click and can be dragged on.
        const auto area = static_cast<ScrollArea>(w.a);
        const Rect thumb = scrollThumb(area);
        if (p.x < thumb.x || p.x >= thumb.right()) {
            const Rect track = w.rect.inset(4.0f);
            const float cards = scrollLength(area);
            const float shown = static_cast<float>(area == ScrollArea::Fx ? kVisibleFx : kVisibleCards);
            const float room = track.w - thumb.w;
            setScroll(area, room > 0.0f ? (p.x - thumb.w * 0.5f - track.x) / room * (cards - shown) : 0.0f);
        }
        drag_.kind = DragKind::Scroll;
        drag_.startValue = scrollFirst(area);
        break;
    }
    case Kind::McBand:
        fxBand_[w.a] = w.b;
        break;
    case Kind::SourcesToggle:
        layout.sourcesCollapsed = !layout.sourcesCollapsed;
        controller_.layoutChanged(); // asks the host for the new window size
        break;
    case Kind::NodeTab:
    case Kind::MatrixLabel:
        // The selected tab switches its operator or filter on and off; another tab selects that one,
        // switching it on if it was off. A name in the matrix does the same.
        if (layout.selectedNode == w.a)
            edit(enabledParam(w.a), nodeOn(w.a) ? 0.0 : 1.0);
        else if (!nodeOn(w.a))
            edit(enabledParam(w.a), 1.0);
        layout.selectedNode = w.a;
        break;
    case Kind::NodeGhost: // into the graph where it was, and into the inspector
        edit(enabledParam(w.a), 1.0);
        layout.selectedNode = w.a;
        break;
    case Kind::ViewTab:
        layout.view = static_cast<RoutingView>(w.a);
        shapeEditor_ = -1; // the tabs take the panels back from the shape and partial editors
        partialEditor_ = -1;
        fxMenuOpen_ = false;
        listMenu_ = -1;
        break;
    case Kind::SourceGrip:
        drag_.kind = DragKind::SourceMove;
        drag_.scrollStart = sourceScroll();
        drag_.step = true;
        controller_.beginStep();
        break;
    case Kind::SourceAdd:
        addSource(static_cast<SourceType>(w.a));
        break;
    case Kind::SourceRemove:
        removeSource(w.a);
        break;
    case Kind::FxGrip:
    case Kind::FxPower: // the icon: let go without moving, it switches the effect
        drag_.kind = DragKind::FxMove;
        drag_.scrollStart = fxScroll();
        break;
    case Kind::FxFold:
        setFolded(true, w.a, !fxFolded(w.a));
        hover_.reset();
        break;
    case Kind::SourceFold:
        setFolded(false, w.a, !sourceFolded(w.a));
        hover_.reset();
        break;
    case Kind::Choice:
        edit(w.param, w.b);
        break;
    case Kind::Toggle:
        edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
        break;

    case Kind::ShapeButton:
        edit(paramIndex(w.a, SourceParam::Shape), w.b);
        if (w.b == static_cast<int>(LfoShape::Custom))
            shapeEditor_ = w.a;
        break;
    case Kind::LfoDisplay:
        if (std::lround(value(paramIndex(w.a, SourceParam::Shape))) == static_cast<long>(LfoShape::Custom))
            shapeEditor_ = w.a;
        break;
    case Kind::ShapeDone:
        shapeEditor_ = -1;
        partialEditor_ = -1;
        break;
    case Kind::ClipDisplay:
        shapeEditor_ = kMaxSources + w.a;
        break;
    case Kind::NoteDisplay:
        shapeEditor_ = shapeEditor_ == kNoteCurve ? -1 : kNoteCurve; // a second click closes it
        hover_.reset();
        break;
    case Kind::ListButton:
        if (isFilterSlope(w.param) && !hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param)))))) {
            flash(tr(Str::MsgCombSlope));
            break;
        }
        listMenu_ = listMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::ListMenu:
        if (w.a == 0) // a click anywhere else closes the list
            listMenu_ = -1;
        break;
    case Kind::ListItem:
        if (!listItemFits(w.param, w.b))
            break; // the list stays open for another pick
        listMenu_ = -1;
        edit(w.param, w.b); // a drawn curve is edited in the card, or the big editor
        break;
    case Kind::PeqTypeButton:
        peqMenu_ = peqMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::PeqTypeMenu:
        if (w.a == 0) // a click anywhere else closes the list
            peqMenu_ = -1;
        break;
    case Kind::PeqTypeItem:
        peqMenu_ = -1;
        edit(w.param, w.b);
        break;
    case Kind::Key:
        drag_.kind = DragKind::Key;
        controller_.playNote(keyboardBase() + w.a, true);
        break;
    case Kind::Octave:
        octave_ = std::clamp(octave_ + w.a, -2, 2);
        break;
    case Kind::PresetButton:
        // Init and Load ask first when they would throw away changes.
        pendingPreset_ = -1;
        if (static_cast<PresetAction>(w.a) != PresetAction::Save && controller_.patchEdited()) {
            confirm_ = w.a;
            hover_.reset();
        } else {
            runPreset(static_cast<PresetAction>(w.a));
        }
        break;
    case Kind::PresetStep:
        stepPreset(w.a);
        break;
    case Kind::PresetNameBox: {
        controller_.rescanPresets();
        browserOpen_ = true;
        // Scrolled so the current preset shows, about in the middle.
        const std::vector<std::pair<int, std::string>> rows = browserRows();
        const int current = controller_.presetIndex();
        const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.first == current; });
        browserScroll_ = current >= 0 && at != rows.end()
                             ? std::max(static_cast<int>(at - rows.begin()) - browserVisibleRows(browserPanel(height())) / 2, 0)
                             : 0;
        hover_.reset();
        break;
    }
    case Kind::BrowserBackdrop:
        if (w.a == 0) // a click around the browser closes it
            browserOpen_ = false;
        break;
    case Kind::BrowserRow:
        openPreset(w.a);
        break;
    case Kind::BrowserClose:
        browserOpen_ = false;
        hover_.reset();
        break;
    case Kind::BrowserFolder:
        controller_.openPresetFolder();
        break;
    case Kind::ConfirmBackdrop:
        if (w.a == 0) { // a click around the dialog cancels it
            confirm_ = -1;
            pendingPreset_ = -1;
        }
        break;
    case Kind::ConfirmButton: {
        const auto action = static_cast<PresetAction>(confirm_);
        confirm_ = -1;
        hover_.reset();
        if (w.a == 1)
            runPreset(action);
        pendingPreset_ = -1;
        break;
    }
    case Kind::SettingsButton:
        settingsOpen_ = true;
        hover_.reset();
        break;
    case Kind::SettingsBackdrop:
        if (w.a == 0) // a click around the dialog closes it
            settingsOpen_ = false;
        break;
    case Kind::SettingsOption:
        controller_.setOversampling(w.b);
        break;
    case Kind::SettingsTheme:
        controller_.setTheme(kThemes[w.b].name);
        break;
    case Kind::SettingsGlow: // dragged; the amount is kept when it is let go
        drag_.kind = DragKind::Glow;
        drag_.startValue = glowDragged_ = shownGlow();
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::SettingsClose:
        settingsOpen_ = false;
        hover_.reset();
        break;
    case Kind::ModWheel:
        drag_.kind = DragKind::ModWheel;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::BendWheel:
        drag_.kind = DragKind::BendWheel;
        applyDrag(p);
        break;
    case Kind::ShapeArea:
        break;
    case Kind::ShapePoint:
        drag_.kind = DragKind::ShapePoint;
        for (PointField field : {PointField::X, PointField::Y})
            controller_.beginEdit(shapeIndex(w.a, w.b, field));
        break;
    case Kind::ShapeCurve:
        drag_.kind = DragKind::ShapeCurve;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::WaveButton:
        edit(paramIndex(w.a, OpParam::Wave), w.b);
        break;
    case Kind::TypeButton:
        edit(paramIndex(w.a, FilterParam::Type), w.b);
        break;
    case Kind::SlotRemove:
        removeSlot(w.a, w.b);
        break;
    case Kind::SlotBypass:
        toggleBypass(w.a, w.b);
        break;
    case Kind::SlotScroll: {
        // On the thumb, drag it; elsewhere on the track, the thumb jumps to center on the click and can be dragged on.
        const float card = cardOf(w.a);
        const int rows = static_cast<int>(usedSlots(w.a).size());
        if (card == kOffScreen || rows <= kVisibleSlots)
            break;
        const Rect track = slotScrollTrack(card);
        const Rect thumb = slotThumb(w.a, card);
        if (p.y < thumb.y || p.y >= thumb.bottom()) {
            scrollSlots(w.a, (p.y - thumb.h * 0.5f - track.y) / (track.h - thumb.h) * (rows - kVisibleSlots), false);
        }
        drag_.kind = DragKind::SlotScroll;
        drag_.startValue = slotFirst(w.a);
        break;
    }
    case Kind::ValueText:
        break; // double-click types a value
    case Kind::WaveMore:
        waveMenu_ = waveMenu_ == w.a ? -1 : w.a;
        break;
    case Kind::WaveMenu:
        if (w.a == 0) // a click anywhere else closes the list
            waveMenu_ = -1;
        break;
    case Kind::WaveMenuItem: {
        waveMenu_ = -1;
        const UndoScope step(controller_);
        const auto before = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
        edit(paramIndex(w.a, OpParam::Wave), w.b);
        if (w.b == static_cast<int>(OpWave::Custom)) { // a drawn wave opens its partials
            // The partials start as the wave it was, so it sounds the same until they are drawn; after
            // Noise, or Custom again, they stay as they were.
            std::array<double, 2 * kPartials> partials{};
            if (wavePartials(before, partials)) {
                std::vector<std::pair<int, double>> changes;
                for (int k = 0; k < kPartials; ++k) {
                    changes.push_back({partialLevelIndex(w.a, k), partials[2 * k]});
                    changes.push_back({partialPhaseIndex(w.a, k), partials[2 * k + 1]});
                }
                editMany(changes);
            }
            partialEditor_ = w.a;
            shapeEditor_ = -1;
        }
        break;
    }
    case Kind::PartialArea:
        drag_.kind = DragKind::Partials;
        drag_.step = true; // one undo step for the whole stroke
        controller_.beginStep();
        applyDrag(p);
        break;
    case Kind::PartialReset: {
        std::vector<std::pair<int, double>> changes;
        for (int k = 0; k < kPartials; ++k) {
            changes.push_back({partialLevelIndex(w.a, k), table_[partialLevelIndex(w.a, k)].def});
            changes.push_back({partialPhaseIndex(w.a, k), table_[partialPhaseIndex(w.a, k)].def});
        }
        editMany(changes);
        break;
    }
    case Kind::ShapeReset:
        resetShape(w.a);
        break;
    case Kind::ShapeGrid:
        if (w.b != 0)
            stepShapeGrid(w.a, w.b);
        break;
    case Kind::UndoButton:
        if (w.a == 1)
            redo();
        else
            undo();
        break;
    }
}

void View::mouseUp(Point p, Modifiers) {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::Value:
        controller_.endEdit(w.param);
        break;
    case DragKind::Node:
        if (drag_.moved)
            controller_.layoutChanged();
        break;
    case DragKind::Connect: {
        if (!drag_.moved) { // a click on the edge picks the node, like one further inside
            controller_.layout().selectedNode = w.a;
            break;
        }
        const int to = nodeAt(p);
        if (to >= 0) {
            connect(w.a, to);
        } else if (onBus(p) && value(outParam(w.a)) <= 0.0) {
            edit(outParam(w.a), 1.0);
        }
        break;
    }
    case DragKind::Reroute: {
        if (!drag_.moved) // a click leaves it be
            break;
        const int into = nodeAt(p) >= 0 ? nodeAt(p) : (onBus(p) ? kNodes : -1);
        if (into >= 0)
            reroute(w.a, w.b, into);
        else // let go of off the nodes and the bus, the route goes, with its modulation
            disconnect(w.param);
        break;
    }
    case DragKind::ModSource: {
        const auto target = hitTest(p);
        if (target && target->param >= 0 && isModTarget(target->param) && target->kind != Kind::ModDot &&
            target->kind != Kind::PeqPoint && target->kind != Kind::WireEnd)
            assign(w.a, target->param);
        break;
    }
    case DragKind::PeqPoint:
        for (int index : peqPointParams(w.a, w.b))
            controller_.endEdit(index);
        break;
    case DragKind::EnvPoint:
        for (int index : envPointParams(w.a, w.b))
            controller_.endEdit(index);
        break;
    case DragKind::EnvCurve:
        controller_.endEdit(w.param);
        break;
    case DragKind::FilterPoint:
        for (int index : filterPointParams(w.a))
            controller_.endEdit(index);
        break;
    case DragKind::ShapePoint:
        for (PointField field : {PointField::Y, PointField::X})
            controller_.endEdit(shapeIndex(w.a, w.b, field));
        break;
    case DragKind::ShapeCurve:
        controller_.endEdit(w.param);
        break;
    case DragKind::Partials:
        break; // its undo step closes below
    case DragKind::Key:
        controller_.playNote(keyboardBase() + w.a, false);
        break;
    case DragKind::ModWheel:
        controller_.endEdit(w.param);
        break;
    case DragKind::Glow:
        controller_.setGlow(glowDragged_);
        glowDragged_ = -1.0f;
        break;
    case DragKind::BendWheel:
        bend_ = 0.0;
        controller_.pitchWheel(0.0);
        break;
    case DragKind::FxMove: {
        drag_.now = p;
        if (!drag_.moved && w.kind == Kind::FxPower) {
            edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
            break;
        }
        const double order = fxOrderWith(fxChainShown());
        if (order != value(paramIndex(GlobalParam::FxOrder)))
            edit(paramIndex(GlobalParam::FxOrder), order);
        break;
    }
    case DragKind::SourceMove:
        drag_.now = p;
        setSourceOrder(sourcesShown());
        break;
    case DragKind::Scroll:
    case DragKind::SlotScroll:
        break;
    case DragKind::None:
        break;
    }
    if (drag_.step)
        controller_.endStep();
    drag_ = Drag{};
    hover_ = hitTest(p);
}

void View::mouseLeave() {
    if (drag_.kind == DragKind::None)
        hover_.reset();
}

void View::doubleClick(Point p, Modifiers mods) {
    const auto hit = hitTest(p);
    if (hit && (hit->kind == Kind::Key || hit->kind == Kind::Octave || hit->kind == Kind::UndoButton ||
                hit->kind == Kind::PresetStep || hit->kind == Kind::FxPower ||
                hit->kind == Kind::SlotBypass || (hit->kind == Kind::ShapeGrid && hit->b != 0) ||
                drawsStep(*hit, mods))) {
        // Windows turns a quick second click into a double-click instead of a press:
        // play it as a press so fast repeated notes, octave steps, undos and steps aren't dropped.
        mouseDown(p, mods);
        return;
    }
    if (hit && hit->param >= 0 && onValue(*hit, p)) {
        startTyping(*hit);
        return;
    }
    if (hit && hit->kind == Kind::MacroName) {
        startTyping(*hit);
        return;
    }
    if (hit && hit->kind == Kind::PartialArea) { // that bar to nothing
        const int k = partialAt(hit->b == 0 ? kPartialLevels : kPartialPhases, p.x, partialsShown(hit->a));
        edit(hit->b == 0 ? partialLevelIndex(hit->a, k) : partialPhaseIndex(hit->a, k), 0.0);
        return;
    }
    if (hit && hit->kind == Kind::ShapeArea) {
        addShapePoint(hit->a, p);
        return;
    }
    if (hit && hit->kind == Kind::ShapePoint) {
        removeShapePoint(hit->a, hit->b);
        return;
    }
    if (hit && (hit->kind == Kind::ShapeCurve || hit->kind == Kind::EnvCurve)) {
        edit(hit->param, 0.0); // straight again: 0 %, whatever the segment started as
        return;
    }
    if (hit && hit->kind == Kind::PeqPoint) {
        if (isCut(peqType(value(peqTypeIndex(hit->a, hit->b)))))
            edit(peqQIndex(hit->a, hit->b), normFromQ(std::numbers::sqrt2 * 0.5)); // no resonance
        else
            edit(peqGainIndex(hit->a, hit->b), 0.0); // flat again
        return;
    }
    if (!hit || hit->param < 0)
        return;
    switch (hit->kind) {
    case Kind::ModDot:
        removeSlot(hit->a, hit->b);
        break;
    case Kind::Knob:
    case Kind::SlotBar:
        edit(hit->param, table_[hit->param].def);
        break;
    case Kind::OutLabel: // like a wire label: shown, so wired, so off
        disconnect(hit->param);
        break;
    case Kind::GridCell:
    case Kind::WireLabel: {
        // Toggle: off, or on at a typical amount for that kind of route.
        const int to = hit->b;
        const bool full = to >= kOps; // into a filter or the output; modulation starts lower
        const double on = full ? 1.0 : (hit->a == to ? 0.25 : 0.3);
        if (wired(hit->param)) {
            disconnect(hit->param);
        } else {
            const UndoScope step(controller_);
            if (hit->kind == Kind::GridCell) // a route in the matrix switches on what it joins
                switchOnRoute(hit->a, to);
            edit(hit->param, on);
        }
        break;
    }
    default:
        break;
    }
}

void View::wheel(Point p, float steps, Modifiers mods) {
    endTyping(true);
    if (browserOpen_ && !settingsOpen_ && confirm_ < 0) { // three rows a notch
        const int rows = static_cast<int>(browserRows().size());
        const int most = std::max(rows - browserVisibleRows(browserPanel(height())), 0);
        browserScroll_ = std::clamp(std::clamp(browserScroll_, 0, most) - static_cast<int>(std::lround(steps * 3.0f)), 0, most);
        return;
    }
    keepRoutes(); // as in mouseDown(): scrolling can take a route down to 0 % too
    const auto hit = hitTest(p);
    if (hit && hit->kind == Kind::ShapeGrid) {
        if (steps != 0.0f)
            stepShapeGrid(hit->a, steps > 0.0f ? 1 : -1);
        return;
    }
    // Over a source's slot list (but not on a depth bar), a notch scrolls it a row, gliding there; a
    // trackpad's smaller steps move it by parts of a row.
    if ((!hit || hit->param < 0) && steps != 0.0f) {
        for (int s : sources()) {
            const float card = cardOf(s);
            if (card == kOffScreen || sourceFolded(s) || !slotList(card).contains(p) || !sourceCardClip().contains(p) ||
                usedSlots(s).size() <= static_cast<size_t>(kVisibleSlots))
                continue;
            // From where it is headed, clamped, so notches add up but scrolling past an end doesn't pile up.
            scrollSlots(s, std::clamp(slotScroll_[s], 0.0f, slotEnd(s)) - steps, true);
            return;
        }
    }
    if ((!hit || hit->param < 0) && modPanel(controller_.layout().sourcesCollapsed).contains(p) && steps != 0.0f) {
        setScroll(ScrollArea::Sources, sourceScroll() - steps * 0.4f);
        return;
    }
    if ((!hit || hit->param < 0) && fxWide() && kFxPanel.contains(p) && steps != 0.0f) {
        setScroll(ScrollArea::Fx, fxScroll() - steps * 0.4f);
        return;
    }
    if (!hit || hit->param < 0)
        return;
    const UndoScope notches(controller_, true); // a run of notches on one knob undoes at once
    const ParamDef &def = table_[hit->param];
    const double v = value(hit->param);
    if (def.kind == ParamKind::VoiceCount) { // about 20 % per notch, at least one voice
        const double scaled = std::round(v * std::exp2(steps * 0.25));
        edit(hit->param, steps > 0 ? std::max(v + 1.0, scaled) : std::min(v - 1.0, scaled));
        return;
    }
    if (def.kind == ParamKind::Transpose || def.kind == ParamKind::NotePitch) { // to the next whole semitone
        edit(hit->param, steps > 0 ? std::floor(v + 1.0) : std::ceil(v - 1.0));
        return;
    }
    const double step = isStepped(def.kind) ? 1.0 : (def.max - def.min) * (mods.shift ? 0.002 : 0.01);
    edit(hit->param, v + steps * step);
}

// Menu commands from here on clear all of a source's slots; those from 100 up to here remove one slot.
constexpr int kClearCommand = 100 + kAllSources * kSlots; // the removals first, the macros' and the note's too
// And from here on, each bypasses a slot or lets it go again.
constexpr int kBypassCommand = kClearCommand + kAllSources;
// A source's polarity, in the menus of its card, its handle, and a macro's knob and name.
constexpr int kUnipolarCommand = 12;
constexpr int kBipolarCommand = 13;

std::vector<MenuItem> View::contextMenu(Point p) {
    endTyping(true);
    std::vector<MenuItem> items;
    menuParam_ = -1;
    menuFx_ = -1;
    menuSource_ = -1;
    menuPoint_ = {-1, -1};
    menuValue_.reset();
    const auto hit = hitTest(p);
    if (!hit)
        return items;
    if (hit->kind == Kind::FxGrip || hit->kind == Kind::FxPower) {
        menuFx_ = hit->a;
        if (fxChain().size() < static_cast<size_t>(kFxSlots))
            items.push_back({6, tr(Str::MenuDuplicate)});
        items.push_back({8, tr(Str::MenuResetSettings)});
        items.push_back({0, ""});
        items.push_back({7, tr(Str::MenuRemove)});
        return items;
    }
    // Every source can swing its targets one way or both, from the menu of its card or handle (and a macro's knob,
    // the mod wheel and the note source's display).
    const int polaritySource = hit->kind == Kind::SourceGrip || hit->kind == Kind::SourceHandle ||
                                       hit->kind == Kind::MacroHandle
                                   ? hit->a
                               : hit->kind == Kind::MacroName              ? kMaxSources + hit->a
                               : hit->kind == Kind::NoteDisplay            ? kNoteSource
                               : hit->kind == Kind::ModWheel               ? kWheelSource
                               : hit->kind == Kind::Knob && macroOf(hit->param) >= 0 ? kMaxSources + macroOf(hit->param)
                                                                                     : -1;
    auto addPolarity = [&] {
        menuSource_ = polaritySource;
        const bool bipolar = sourceBipolar(polaritySource);
        items.push_back({kUnipolarCommand, tr(Str::MenuUnipolar), !bipolar});
        items.push_back({kBipolarCommand, tr(Str::MenuBipolar), bipolar});
    };
    if (hit->kind == Kind::SourceGrip || hit->kind == Kind::SourceHandle) {
        if (!usedSlots(hit->a).empty())
            items.push_back({kClearCommand + hit->a, trf(Str::MenuClearTargets, {sourceName(hit->a)})});
        if (sources().size() < static_cast<size_t>(kMaxSources))
            items.push_back({9, tr(Str::MenuDuplicate)});
        if (!items.empty())
            items.push_back({0, ""});
        addPolarity();
        items.push_back({0, ""});
        items.push_back({10, tr(Str::MenuRemove)});
        return items;
    }
    if (hit->kind == Kind::MacroHandle || hit->kind == Kind::MacroName || hit->kind == Kind::NoteDisplay) {
        if (!usedSlots(polaritySource).empty()) {
            items.push_back({kClearCommand + polaritySource, trf(Str::MenuClearTargets, {sourceName(polaritySource)})});
            items.push_back({0, ""});
        }
        addPolarity();
        return items;
    }
    if (hit->kind == Kind::SlotBar || hit->kind == Kind::SlotRemove || hit->kind == Kind::ModDot ||
        hit->kind == Kind::ValueText || hit->kind == Kind::SlotBypass) {
        // The depth types where it shows: beside its bar, or under a knob's dot.
        if (hit->kind != Kind::SlotRemove && hit->kind != Kind::SlotBypass) {
            menuValue_ = *hit;
            if (hit->kind == Kind::SlotBar) {
                menuValue_->kind = Kind::ValueText;
                menuValue_->rect = {hit->rect.right() + 2.0f, hit->rect.y, 38.0f, hit->rect.h};
            }
            items.push_back({11, tr(Str::MenuEditDepth)});
        }
        items.push_back({kBypassCommand + hit->a * kSlots + hit->b, bypassLabel(hit->a, hit->b)});
        items.push_back({100 + hit->a * kSlots + hit->b, trf(Str::MenuRemoveModulation, {sourceName(hit->a)})});
        return items;
    }
    if (hit->kind == Kind::ShapePoint) {
        const bool end = isCurve(hit->a) && hit->b + 1 == shapePointCount(hit->a);
        if (hit->b > 0 && !end && shapePointCount(hit->a) > 2) {
            menuPoint_ = {hit->a, hit->b};
            items.push_back({5, tr(Str::MenuDeletePoint)});
        }
        return items;
    }
    if (hit->kind == Kind::ShapeCurve) {
        menuParam_ = hit->param;
        items.push_back({1, tr(Str::MenuStraighten)});
        return items;
    }
    if (hit->kind == Kind::Node || hit->kind == Kind::Port) { // the node, or its edge
        menuParam_ = enabledParam(hit->a);
        items.push_back({4, trf(Str::MenuSwitchOff, {nodeTitle(hit->a)})});
        return items;
    }
    if (hit->kind == Kind::WireEnd) {
        menuParam_ = hit->param;
        items.push_back({3, tr(Str::MenuDisconnect)});
        return items;
    }
    if (hit->param < 0)
        return items;

    menuParam_ = hit->param;
    if (hit->kind == Kind::Knob || hit->kind == Kind::WireLabel || hit->kind == Kind::OutLabel ||
        hit->kind == Kind::GridCell) {
        menuValue_ = *hit;
        items.push_back({11, tr(Str::MenuEditValue)});
    }
    items.push_back({1, tr(Str::MenuResetDefault)});
    if ((hit->kind == Kind::WireLabel || hit->kind == Kind::OutLabel || hit->kind == Kind::GridCell) &&
        wired(hit->param))
        items.push_back({3, tr(Str::MenuDisconnect)});
    std::vector<MenuItem> removals, bypasses;
    for (const Assignment &m : assignments()) {
        if (m.target != hit->param)
            continue;
        if (!isStripSource(m.source))
            bypasses.push_back({kBypassCommand + m.source * kSlots + m.slot, bypassLabel(m.source, m.slot)});
        removals.push_back({100 + m.source * kSlots + m.slot, trf(Str::MenuRemoveModulation, {sourceName(m.source)})});
    }
    if (!removals.empty()) {
        items.push_back({0, ""});
        items.insert(items.end(), bypasses.begin(), bypasses.end());
        items.push_back({0, ""});
        items.insert(items.end(), removals.begin(), removals.end());
        if (removals.size() > 1)
            items.push_back({2, tr(Str::MenuRemoveAll)});
    }
    if (polaritySource >= 0) { // a macro's knob, or the mod wheel
        items.push_back({0, ""});
        addPolarity();
    }
    return items;
}

std::string View::bypassLabel(int source, int slot) const {
    return trf(slotBypassed(source, slot) ? Str::MenuResume : Str::MenuBypass, {sourceName(source)});
}

void View::menuCommand(int command) {
    if (command == 1 && menuParam_ >= 0) {
        edit(menuParam_, table_[menuParam_].def);
    } else if (command == 2 && menuParam_ >= 0) {
        const UndoScope step(controller_);
        for (const Assignment &m : assignments())
            if (m.target == menuParam_)
                removeSlot(m.source, m.slot);
    } else if (command == 5 && menuPoint_.first >= 0) {
        removeShapePoint(menuPoint_.first, menuPoint_.second);
    } else if (command == 6 && menuFx_ >= 0) {
        addEffect(fxSlotType(menuFx_), menuFx_);
    } else if (command == 7 && menuFx_ >= 0) {
        removeEffect(menuFx_);
    } else if (command == 8 && menuFx_ >= 0) {
        // Every field back to its default, apart from which effect it is and whether it is on.
        std::vector<std::pair<int, double>> changes;
        for (int f = static_cast<int>(FxParam::Enabled) + 1; f < kFxParamCount; ++f) {
            const int index = paramIndex(menuFx_, static_cast<FxParam>(f));
            changes.push_back({index, table_[index].def});
        }
        for (int f = 0; f < kShapeParamCount; ++f)
            changes.push_back({clipCurveCountIndex(menuFx_) + f, table_[clipCurveCountIndex(menuFx_) + f].def});
        editMany(changes);
    } else if (command == 3 && menuParam_ >= 0) {
        disconnect(menuParam_);
    } else if (command == 4 && menuParam_ >= 0) {
        edit(menuParam_, 0.0); // switch a node off
    } else if (command == 9 && menuSource_ >= 0) {
        addSource(sourceTypeOf(menuSource_), menuSource_);
    } else if (command == 10 && menuSource_ >= 0) {
        removeSource(menuSource_);
    } else if (command == 11 && menuValue_) {
        startTyping(*menuValue_);
    } else if ((command == kUnipolarCommand || command == kBipolarCommand) && menuSource_ >= 0) {
        const Polarity polarity = polarityOf(menuSource_, sourceTypeOf(menuSource_));
        edit(polarity.index, command == kBipolarCommand ? polarity.bipolar : 1.0 - polarity.bipolar);
    } else if (command >= 100 && command < kClearCommand) {
        removeSlot((command - 100) / kSlots, (command - 100) % kSlots);
    } else if (command >= kClearCommand && command < kClearCommand + kAllSources) {
        const UndoScope step(controller_);
        for (int slot : usedSlots(command - kClearCommand))
            removeSlot(command - kClearCommand, slot);
    } else if (command >= kBypassCommand && command < kBypassCommand + kMaxSources * kSlots) {
        toggleBypass((command - kBypassCommand) / kSlots, (command - kBypassCommand) % kSlots);
    }
}

void View::undo() {
    if (drag_.kind == DragKind::None && !typing_)
        controller_.undo();
}

void View::redo() {
    if (drag_.kind == DragKind::None && !typing_)
        controller_.redo();
}

void View::startTyping(const Widget &w) {
    if (w.kind == Kind::MacroName) { // its name, all selected
        const std::string name = macroName(controller_.layout(), w.a);
        typing_ = Typing{w, name, name.size(), true};
        caretTicks_ = 0;
        return;
    }
    char text[64];
    formatParam(w.param, value(w.param), text, sizeof(text));
    typing_ = Typing{w, text, std::string_view(text).size(), true};
    caretTicks_ = 0;
}

void View::typeChar(char32_t c) {
    if (!typing_ || c < 0x20 || c > 0x7E)
        return;
    Typing &t = *typing_;
    if (t.selected) {
        t.text.clear();
        t.caret = 0;
        t.selected = false;
    }
    if (t.text.size() < 40)
        t.text.insert(t.caret++, 1, static_cast<char>(c));
    caretTicks_ = 0;
}

void View::typeKey(Key key) {
    if (!typing_)
        return;
    Typing &t = *typing_;
    caretTicks_ = 0;
    switch (key) {
    case Key::Enter:
        endTyping(true);
        return;
    case Key::Escape:
        endTyping(false);
        return;
    case Key::SelectAll:
        t.selected = true;
        t.caret = t.text.size();
        return;
    case Key::Backspace:
    case Key::Delete:
        if (t.selected) {
            t.text.clear();
            t.caret = 0;
        } else if (key == Key::Backspace && t.caret > 0) {
            t.text.erase(--t.caret, 1);
        } else if (key == Key::Delete && t.caret < t.text.size()) {
            t.text.erase(t.caret, 1);
        }
        break;
    case Key::Left: // from a selection, to its start
        t.caret = t.selected || t.caret == 0 ? 0 : t.caret - 1;
        break;
    case Key::Right:
        t.caret = t.selected ? t.text.size() : std::min(t.caret + 1, t.text.size());
        break;
    case Key::Home:
        t.caret = 0;
        break;
    case Key::End:
        t.caret = t.text.size();
        break;
    }
    t.selected = false;
}

void View::endTyping(bool commit) {
    if (!typing_)
        return;
    const Typing t = std::move(*typing_);
    typing_.reset();
    if (t.widget.kind == Kind::MacroName) { // a new name, an undo step of its own; emptied, its number again
        const size_t start = t.text.find_first_not_of(' ');
        std::string name = start == std::string::npos ? std::string()
                                                      : t.text.substr(start, t.text.find_last_not_of(' ') - start + 1);
        name = name.substr(0, 24);
        if (name == trf(Str::MacroDefaultName, {std::to_string(t.widget.a + 1)}))
            name.clear();
        UiLayout &layout = controller_.layout();
        if (commit && name != layout.macroNames[t.widget.a]) {
            const UndoScope step(controller_);
            layout.macroNames[t.widget.a] = name;
            controller_.layoutChanged();
        }
        return;
    }
    // Spaces around it don't count. Left as it was shown, or emptied, the value stays as it is,
    // rather than being rounded to what the text says.
    const size_t first = t.text.find_first_not_of(' ');
    char shown[64];
    formatParam(t.widget.param, value(t.widget.param), shown, sizeof(shown));
    if (!commit || first == std::string::npos || t.text == shown)
        return;
    const std::string text = t.text.substr(first, t.text.find_last_not_of(' ') - first + 1);
    double v = 0.0;
    if (parseParam(t.widget.param, text.c_str(), &v))
        edit(t.widget.param, v);
    else
        flash(trf(Str::MsgBadValue, {text, paramName(t.widget.param)}));
}

CursorKind View::cursor() const {
    if (drag_.kind == DragKind::Connect || drag_.kind == DragKind::Reroute || drag_.kind == DragKind::ModSource)
        return CursorKind::Hand;
    if (drag_.kind == DragKind::Value && drag_.widget.kind == Kind::PeqPoint) // Alt-dragged for its Q
        return CursorKind::Vertical;
    const Widget *w = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (!w)
        return CursorKind::Arrow;
    switch (w->kind) {
    case Kind::Knob:
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
    case Kind::ModDot:
    case Kind::ShapeCurve:
    case Kind::EnvCurve:
        return CursorKind::Vertical;
    case Kind::ShapeArea:
    case Kind::SettingsBackdrop:
    case Kind::ConfirmBackdrop:
    case Kind::BrowserBackdrop:
        return CursorKind::Arrow;
    case Kind::SlotBar:
        return CursorKind::Horizontal;
    case Kind::Node:
    case Kind::EnvPoint:
    case Kind::FilterPoint:
    case Kind::PeqPoint:
    case Kind::FxGrip:
    case Kind::SourceGrip:
    case Kind::ShapePoint:
        return CursorKind::Move;
    default:
        return CursorKind::Hand;
    }
}

} // namespace fmvibe::gui
