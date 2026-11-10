#pragma once

// The editor's look: the color themes the settings pick from, the colors of the one being painted, and the glow
// of the graphs' curves. Like layout.h, icons.h and routing.h, it is shared by the editor's own files only (see
// view.cpp), in namespace detail.

#include "view.h"

#include <cstddef>
#include <iterator>
#include <string>

namespace fmvibe::gui::detail {

// --- Look.

// The scales every corner and every piece of text keep to. A corner is one of four radii, by what it rounds; the
// rings and outlines drawn around something take its radius plus how far out they are, so their corners stay concentric.
constexpr float kRadiusMark = 3.0f;    // small marks: tags, chips in a cell, meters, tracks and thin bars
constexpr float kRadiusControl = 5.0f; // buttons, switches, lists' buttons, fields, menu rows, graph nodes
constexpr float kRadiusCard = 8.0f;    // cards, displays, menus, tooltips and the view tabs
constexpr float kRadiusPanel = 12.0f;  // the panels and the dialogs
// Text comes in five sizes (the name in the title bar apart). A name fitted to its room may be drawn smaller.
constexpr float kTextSmall = 9.0f;     // axis ticks, units, readouts on displays, graph nodes' names
constexpr float kTextLabel = 10.5f;    // knob labels, switches, buttons, hints
constexpr float kTextBody = 11.0f;     // card titles (bold capitals), list and menu rows, fields
constexpr float kTextValue = 13.0f;    // the header's values, the matrix's and the routing graph's names, messages
constexpr float kTextHeading = 15.0f;  // dialog titles, an operator's letter, big numbers
constexpr float kTextLogo = 22.0f;     // "FMVibe" in the title bar

struct Palette {
    const char *name; // as the settings keep it
    Str label;        // as the settings dialog shows it
    Color background, panel, raised, border, track, knobBody;
    Color text, dim, faint;
    Color accent;    // operators and FM routing
    Color filter;    // filters and what feeds them
    Color fx;        // effects
    // One per source place, whichever type it holds, per macro, the note source's, the wheel's and velocity's.
    Color sources[kAllSources];
    Color card;      // effect and source cards
    Color display;   // the wells graphs are drawn in
    Color headerTop, headerBottom;
    Color whiteKey, whiteKeyHover, blackKey, blackKeyHover, keyLabel;
    // Each type of effect in a color of its own, by FxType (None: the effects' color): shaper red, delay
    // blue, DJ EQ amber, parametric EQ orange, filter purple, compressor green, three-band teal, reverb indigo,
    // phaser pink, flanger lime, frequency shifter cyan, phase disperser slate, widener fuchsia, amp tweed gold.
    Color fxTypes[static_cast<int>(FxType::Count)];
    // A split's bands, low, middle and high: from cool and darker to warm and lighter, so they part by lightness too.
    Color bands[3];
    // How it draws beyond its colors. Left out, a theme draws as Midnight and Light do: outlined cards and controls,
    // the accent for every knob and operator, switches lit with a tint.
    // `flat` is Spectrum's and Porcelain's look: cards tinted with their own color and outlined only under the mouse,
    // a knob's arc (and its track, faintly) in the color of the card it is on (a modulated one's in the text's color,
    // apart from its modulation rings) and its pointer a dot; switches filled
    // solid with their color when lit, set in a well; list buttons, menus and the other controls without outlines;
    // + SOURCE and the other add buttons as filled pills, the cards that add one filled faintly; each operator in a
    // color of its own (ops); routing nodes on a soft shadow (nodeTop down to nodeBottom), the effects graph's
    // filled with a tint of their color.
    bool flat = false;
    Color ops[kOps]{};         // A to F: its node, its tab and the routes it sends
    float cardTint = 0.0f;     // how strongly a card takes its color
    Color nodeTop{}, nodeBottom{};
    float capShadow = 0.0f;    // a shadow under each knob cap, this dark
    float panelShadow = 0.0f;  // a soft shadow under the panels and the tab shown, this dark, which then go unoutlined
    float cardShadow = 0.0f;   // a soft shadow under the effect and source cards, this dark
};
static_assert(static_cast<int>(FxType::Count) == 16, "a color for each effect type in every theme");

// The themes the settings pick from, by name; the first is the default.
inline constexpr Palette kThemes[] = {
    {"Midnight", Str::ThemeMidnight,
     Color::hex(0x0D0F13), Color::hex(0x161920), Color::hex(0x1F232C), Color::hex(0x272C36), Color::hex(0x2D323D),
     Color::hex(0x252A35),
     Color::hex(0xE8EBF2), Color::hex(0x8D94A5), Color::hex(0x4A5162),
     Color::hex(0x8EA2FF), Color::hex(0xC084FC), Color::hex(0xFB923C),
     {Color::hex(0x38BDF8), Color::hex(0xF472B6), Color::hex(0xA3E635), Color::hex(0x818CF8),
      Color::hex(0xFBBF24), Color::hex(0x34D399), Color::hex(0xF87171), Color::hex(0xE879F9),
      Color::hex(0x2DD4BF), Color::hex(0xFDE047), Color::hex(0xA78BFA), Color::hex(0xFDA4AF),
      Color::hex(0x67E8F9), Color::hex(0x86EFAC), Color::hex(0x60A5FA), Color::hex(0xFDBA74),
      Color::hex(0xFDA4AF), Color::hex(0xFDBA74), Color::hex(0xFDE68A), Color::hex(0xBEF264),
      Color::hex(0x6EE7B7), Color::hex(0x67E8F9), Color::hex(0xA5B4FC), Color::hex(0xF0ABFC),
      Color::hex(0xE2E8F0), Color::hex(0xB8C4FF), Color::hex(0xFF9E7A)},
     Color::hex(0x1A1D25), Color::hex(0x101217), Color::hex(0x1A1E26), Color::hex(0x12151B),
     Color::hex(0xE4E7EE), Color::hex(0xCDD3DF), Color::hex(0x07090D), Color::hex(0x262C38), Color::hex(0x4A5162),
     {Color::hex(0xFB923C), Color::hex(0xF87171), Color::hex(0x60A5FA), Color::hex(0xFBBF24), Color::hex(0xFB923C),
      Color::hex(0xC084FC), Color::hex(0x34D399), Color::hex(0x2DD4BF), Color::hex(0x818CF8), Color::hex(0xF472B6),
      Color::hex(0xA3E635), Color::hex(0x22D3EE), Color::hex(0xCBD5E1), Color::hex(0xE879F9),
      Color::hex(0x2DD4BF), Color::hex(0xE0B878)},
     {Color::hex(0x6AA3F0), Color::hex(0x4CC9B0), Color::hex(0xEFB46F)}},
    {"Light", Str::ThemeLight, // crisp: white cards and knob caps on a cool gray, with deep, saturated colors
     Color::hex(0xD5DAE3), Color::hex(0xF3F5F8), Color::hex(0xE4E8EF), Color::hex(0xC3CAD6), Color::hex(0xCDD3DD),
     Color::hex(0xFFFFFF),
     Color::hex(0x0F131B), Color::hex(0x444C5D), Color::hex(0x878FA0),
     Color::hex(0x3346D3), Color::hex(0x8A2BE2), Color::hex(0xDD4F07),
     {Color::hex(0x0274B8), Color::hex(0xD0216D), Color::hex(0x4F8A0B), Color::hex(0x4338CA),
      Color::hex(0xC26505), Color::hex(0x04805C), Color::hex(0xD11F1F), Color::hex(0xB01ECA),
      Color::hex(0x0B857A), Color::hex(0xA87103), Color::hex(0x6D28D9), Color::hex(0xD0163F),
      Color::hex(0x0780A0), Color::hex(0x138A3F), Color::hex(0x1F5BE0), Color::hex(0xDD4F07),
      Color::hex(0xD0163F), Color::hex(0xDD4F07), Color::hex(0xA87103), Color::hex(0x4F8A0B),
      Color::hex(0x04805C), Color::hex(0x0780A0), Color::hex(0x4338CA), Color::hex(0xB01ECA),
      Color::hex(0x3F4A5C), Color::hex(0x4F52E8), Color::hex(0xC2410C)},
     Color::hex(0xFFFFFF), Color::hex(0xE8ECF2), Color::hex(0xFFFFFF), Color::hex(0xEDF0F5),
     Color::hex(0xFFFFFF), Color::hex(0xDDE3EC), Color::hex(0x1A1F29), Color::hex(0x3A4252), Color::hex(0x878FA0),
     {Color::hex(0xDD4F07), Color::hex(0xD11F1F), Color::hex(0x1F5BE0), Color::hex(0xC26505), Color::hex(0xDD4F07),
      Color::hex(0x8A2BE2), Color::hex(0x04805C), Color::hex(0x0B857A), Color::hex(0x4338CA), Color::hex(0xD0216D),
      Color::hex(0x4F8A0B), Color::hex(0x0780A0), Color::hex(0x475569), Color::hex(0xB01ECA),
      Color::hex(0x0B857A), Color::hex(0x9A6A1C)},
     {Color::hex(0x2F67D0), Color::hex(0x0B8A75), Color::hex(0xB86E0E)}},
    {"Spectrum", Str::ThemeSpectrum, // flat on deep indigo, each operator in a color of its own
     Color::hex(0x0D0C14), Color::hex(0x15131F), Color::hex(0x1D1A2A), Color::hex(0x262238), Color::hex(0x2A2640),
     Color::hex(0x221F30),
     Color::hex(0xEBE8F5), Color::hex(0x9590AB), Color::hex(0x5A5570),
     Color::hex(0x7C8CFF), Color::hex(0xC9A8FF), Color::hex(0xFFB86B),
     {Color::hex(0x4FC3F7), Color::hex(0xFF7A8A), Color::hex(0xC6E85A), Color::hex(0xB49CFF),
      Color::hex(0xFFC857), Color::hex(0x4FE0A8), Color::hex(0xFF6B6B), Color::hex(0xF08CE8),
      Color::hex(0x3ED6C1), Color::hex(0xF5E05A), Color::hex(0x9C8CFF), Color::hex(0xFFA3B5),
      Color::hex(0x6EE0F0), Color::hex(0x9EF0A0), Color::hex(0x6EA8FF), Color::hex(0xFFB070),
      Color::hex(0xFF8A5C), Color::hex(0xF5C04A), Color::hex(0xA6E05A), Color::hex(0x3ED6C1),
      Color::hex(0x4FC3F7), Color::hex(0x7C8CFF), Color::hex(0xB49CFF), Color::hex(0xFF5C9A),
      Color::hex(0xE0DCF0), Color::hex(0xB8C0FF), Color::hex(0xFF8A5C)},
     Color::hex(0x1A1726), Color::hex(0x0D0C14), Color::hex(0x15131F), Color::hex(0x100E18),
     Color::hex(0xD9D5E8), Color::hex(0xC4BFD8), Color::hex(0x1D1A2A), Color::hex(0x34304A), Color::hex(0x5A5570),
     {Color::hex(0xFFB86B), Color::hex(0xFF6B6B), Color::hex(0x4FC3F7), Color::hex(0xF5C04A), Color::hex(0xFF8A5C),
      Color::hex(0xC9A8FF), Color::hex(0x4FE0A8), Color::hex(0x3ED6C1), Color::hex(0x7C8CFF), Color::hex(0xFF5C9A),
      Color::hex(0xA6E05A), Color::hex(0x6EE0F0), Color::hex(0xB8B4CC), Color::hex(0xF08CE8),
      Color::hex(0x3ED6C1), Color::hex(0xE8C27A)},
     {Color::hex(0x6E9BFF), Color::hex(0x3ED6C1), Color::hex(0xFFB86B)},
     true,
     {Color::hex(0x7C8CFF), Color::hex(0xFF8A5C), Color::hex(0x3ED6C1), Color::hex(0xF5C04A), Color::hex(0xFF5C9A),
      Color::hex(0xA6E05A)},
     0.10f, Color::hex(0x2A263D), Color::hex(0x211E30)},
    {"Porcelain", Str::ThemePorcelain, // Spectrum in light: white panels lifted off a cool gray by soft shadows
     Color::hex(0xECEEF2), Color::hex(0xFFFFFF), Color::hex(0xF3F5F8), Color::hex(0xE3E6EB), Color::hex(0xE7EAEF),
     Color::hex(0xFFFFFF),
     Color::hex(0x1A1D24), Color::hex(0x5D6472), Color::hex(0x9AA1AD),
     Color::hex(0x2F5BFF), Color::hex(0x7A4DFF), Color::hex(0xE2546A),
     {Color::hex(0x0E9BC4), Color::hex(0xE2546A), Color::hex(0x7FA313), Color::hex(0x7A4DFF),
      Color::hex(0xD98A0B), Color::hex(0x0E8F6E), Color::hex(0xD64545), Color::hex(0xB03FC4),
      Color::hex(0x0F8F85), Color::hex(0xB08A0B), Color::hex(0x6A45E0), Color::hex(0xD6406A),
      Color::hex(0x0A8FAA), Color::hex(0x3E8E3E), Color::hex(0x2F5BFF), Color::hex(0xE07B39),
      Color::hex(0xC26A55), Color::hex(0xB5822F), Color::hex(0x9A9420), Color::hex(0x5E9A4E),
      Color::hex(0x3E9A90), Color::hex(0x4F7FC2), Color::hex(0x6E68C2), Color::hex(0xA066C2),
      Color::hex(0x3A404C), Color::hex(0x6A85FF), Color::hex(0xE07B39)},
     Color::hex(0xFFFFFF), Color::hex(0xF3F5F8), Color::hex(0xFFFFFF), Color::hex(0xFAFBFC),
     Color::hex(0xFFFFFF), Color::hex(0xEDF0F5), Color::hex(0x1A1D24), Color::hex(0x3A3F4B), Color::hex(0x8A91A0),
     {Color::hex(0xE2546A), Color::hex(0xD64545), Color::hex(0x2F5BFF), Color::hex(0xD98A0B), Color::hex(0xE07B39),
      Color::hex(0x7A4DFF), Color::hex(0x0E8F6E), Color::hex(0x0F8F85), Color::hex(0x4F52E8), Color::hex(0xD6406A),
      Color::hex(0x7FA313), Color::hex(0x0A8FAA), Color::hex(0x5D6472), Color::hex(0xB03FC4),
      Color::hex(0x0F8F85), Color::hex(0xA07020)},
     {Color::hex(0x2F67D0), Color::hex(0x0B8A75), Color::hex(0xC27A1E)},
     true,
     {Color::hex(0x2F5BFF), Color::hex(0xD9541A), Color::hex(0x0A9488), Color::hex(0xB07A00), Color::hex(0xD0236E),
      Color::hex(0x4E8A0E)},
     0.07f, Color::hex(0xFFFFFF), Color::hex(0xF2F4F7), 0.22f, 0.06f, 0.07f},
};
constexpr int kThemeCount = static_cast<int>(std::size(kThemes));
// A source left out of a theme's list would be drawn black.
constexpr bool everySourceColored() {
    for (const Palette &theme : kThemes)
        for (const Color &color : theme.sources)
            if (color.r == 0.0f && color.g == 0.0f && color.b == 0.0f)
                return false;
    return true;
}
static_assert(everySourceColored(), "a color for each source in every theme");

int themeIndex(const std::string &name);

// The theme being painted: View::paint() sets it from the controller first, so the names below always
// stand for the colors of the editor painting.
extern Palette look;
inline const Color &kBackground = look.background;
inline const Color &kPanel = look.panel;
inline const Color &kRaised = look.raised;
inline const Color &kBorder = look.border;
inline const Color &kTrack = look.track;
inline const Color &kKnobBody = look.knobBody;
inline const Color &kText = look.text;
inline const Color &kDim = look.dim;
inline const Color &kFaint = look.faint;
inline const Color &kAccent = look.accent;
inline const Color &kFilterColor = look.filter;
inline const Color &kShaperColor = look.fxTypes[static_cast<int>(FxType::Shaper)]; // the voices' shapers, as the effect
inline const Color &kFxColor = look.fx;
inline const Color (&kSourceColors)[kAllSources] = look.sources;
inline const Color &kCardColor = look.card;
inline const Color &kDisplayColor = look.display;
Color fxTypeColor(FxType type);
// An operator's own color in a flat theme, else the accent.
inline Color opColor(int op) {
    return look.flat && op >= 0 && op < kOps ? look.ops[op] : look.accent;
}
// What is drawn on a lit switch: in its color, or on a flat theme's, filled solid with it, in the panels' (dark in a
// dark theme, white in a light one).
inline Color litInk(Color color) {
    return look.flat ? look.panel : color;
}
// Whether the theme is a light one, by its background: shadows are drawn fainter on it.
inline bool lightTheme() {
    return look.background.r + look.background.g + look.background.b > 1.5f;
}
// The well a flat theme's switches and list buttons are set in, and one under the mouse: the displays' color in a dark theme, a
// faint shade of whatever is under it in a light one (where the displays' color would vanish on a tinted card).
inline Color wellColor() {
    return lightTheme() ? Color{0.0f, 0.0f, 0.0f, 0.06f} : look.display;
}
inline Color wellHover() {
    return lightTheme() ? Color{0.0f, 0.0f, 0.0f, 0.11f} : look.track;
}
// Whether controls get their 1 px outline: not in a flat theme.
inline bool outlined() {
    return !look.flat;
}
// How large the switches' drawn icons are: a little smaller in a flat theme, whose lit switches are filled to the edges.
inline float switchIconScale() {
    return look.flat ? 0.8f : 1.0f;
}
extern float glowing; // how strongly graphs' curves glow, 0..1, which View::paint() sets from the controller too

// A graph's curve, glowing as `glowing` says (see theme.cpp).
void plotLine(Canvas &c, const Point *points, size_t count, Color color, float width);

// A soft shadow round `r` (its corners `radius`): `steps` rings, each `spread` further out and as faint, the whole
// `drop` lower than `r` and `alpha` dark where they all overlap.
void softShadow(Canvas &c, Rect r, float radius, int steps, float spread, float drop, float alpha);

// A node's shadow in a flat theme, a little lower than the node; none in the others. A box (`r`, its corners
// `radius`): the routing graph's filters and shapers and the effects graph's nodes; or a circle: the operators.
void paintNodeShadow(Canvas &c, Rect r, float radius);
void paintNodeShadow(Canvas &c, Point center, float radius);

// A card's shadow, in a theme that gives its cards one (see Palette::cardShadow); none in the others.
void paintCardShadow(Canvas &c, Rect r);

// An open list's or menu's panel: outlined, or in a flat theme lifted by a soft shadow instead.
void paintMenuPanel(Canvas &c, Rect r);

// A glow feathering out, as strong as `color`'s alpha where it starts and fading to nothing `spread` further out: round a
// circle of radius `inner`, round a rounded box, and along a line `width` wide. As the glow setting (`glowing`) is at
// 75 %; stronger and wider above it, fainter and narrower below, and none when it is off.
void featherCircle(Canvas &c, Point center, float inner, float spread, Color color);
void featherRect(Canvas &c, Rect r, float radius, float spread, Color color);
void featherLine(Canvas &c, const Point *points, size_t count, float width, float spread, Color color);

} // namespace fmvibe::gui::detail
