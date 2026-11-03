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

struct Palette {
    const char *name; // as the settings keep it
    Str label;        // as the settings dialog shows it
    Color background, panel, raised, border, track, knobBody;
    Color text, dim, faint;
    Color accent;    // operators and FM routing
    Color filter;    // filters and what feeds them
    Color fx;        // effects
    Color sourceAdd; // the buttons that add LFOs and envelopes
    // One per source place, whichever type it holds, per macro, the note source's, the wheel's and velocity's.
    Color sources[kAllSources];
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
inline constexpr Palette kThemes[] = {
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
      Color::hex(0xE2E8F0), Color::hex(0xB8C4FF), Color::hex(0xFF9E7A)},
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
      Color::hex(0xE5E7EB), Color::hex(0x99F6E4), Color::hex(0xFFB59A)},
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
      Color::hex(0x3F4A5C), Color::hex(0x4F52E8), Color::hex(0xC2410C)},
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
      Color::hex(0x5E554A), Color::hex(0x6A3DB8), Color::hex(0xA0522D)},
     Color::hex(0xFFFCF6), Color::hex(0xEFE8DA), Color::hex(0xFCF8F0), Color::hex(0xF1EBDF),
     Color::hex(0xFFFDF8), Color::hex(0xEDE4D3), Color::hex(0x2E2722), Color::hex(0x4C433B), Color::hex(0xA19582),
     {Color::hex(0xC0410F), Color::hex(0xB8361B), Color::hex(0x2D55B0), Color::hex(0xB15A0A), Color::hex(0xC0410F),
      Color::hex(0x8E2A9E), Color::hex(0x177A52), Color::hex(0x0E6E68), Color::hex(0x4A3FB8), Color::hex(0xB3284F),
      Color::hex(0x5A7A12), Color::hex(0x1A7287), Color::hex(0x5E554A), Color::hex(0x9B2AA0)}},
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
inline const Color &kSourceAddColor = look.sourceAdd;
inline const Color (&kSourceColors)[kAllSources] = look.sources;
inline const Color &kCardColor = look.card;
inline const Color &kDisplayColor = look.display;
Color fxTypeColor(FxType type);
extern float glowing; // how strongly graphs' curves glow, 0..1, which View::paint() sets from the controller too

// A graph's curve, glowing as `glowing` says (see theme.cpp).
void plotLine(Canvas &c, const Point *points, size_t count, Color color, float width);

} // namespace fmvibe::gui::detail
