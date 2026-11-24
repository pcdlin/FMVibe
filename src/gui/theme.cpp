#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iterator>
#include <string_view>

namespace fmvibe::gui::detail {

int themeIndex(const std::string &name) {
    // A theme no longer offered, kept in a user's settings, shows as the one nearest it.
    static constexpr struct {
        const char *retired, *now;
    } kRetired[] = {{"Graphite", "Midnight"}, {"Paper", "Light"}, {"Machined", "Midnight"}};
    std::string shown = name;
    for (const auto &r : kRetired)
        if (name == r.retired)
            shown = r.now;
    for (int i = 0; i < kThemeCount; ++i)
        if (shown == kThemes[i].name)
            return i;
    return 0;
}

Palette look = kThemes[0];

namespace {
// Its hue, its saturation and its lightness, 0 to 1 each.
struct Hsl {
    float h, s, l;
};
Hsl toHsl(Color c) {
    const float most = std::max({c.r, c.g, c.b}), least = std::min({c.r, c.g, c.b});
    const float l = 0.5f * (most + least), d = most - least;
    if (d <= 1e-6f)
        return {0.0f, 0.0f, l};
    const float s = l > 0.5f ? d / (2.0f - most - least) : d / (most + least);
    float h = most == c.r ? (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f) : most == c.g ? (c.b - c.r) / d + 2.0f
                                                                                      : (c.r - c.g) / d + 4.0f;
    return {h / 6.0f, s, l};
}
Color fromHsl(Hsl hsl, float alpha) {
    if (hsl.s <= 0.0f)
        return {hsl.l, hsl.l, hsl.l, alpha};
    const float q = hsl.l < 0.5f ? hsl.l * (1.0f + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s, p = 2.0f * hsl.l - q;
    auto channel = [&](float t) {
        t -= std::floor(t);
        if (t < 1.0f / 6.0f)
            return p + (q - p) * 6.0f * t;
        if (t < 0.5f)
            return q;
        if (t < 2.0f / 3.0f)
            return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    return {channel(hsl.h + 1.0f / 3.0f), channel(hsl.h), channel(hsl.h - 1.0f / 3.0f), alpha};
}
} // namespace

float hueOf(Color color) {
    return toHsl(color).h;
}

Color withHue(Color color, float hue) {
    Hsl hsl = toHsl(color);
    hsl.h = hue - std::floor(hue);
    return fromHsl(hsl, color.a);
}

Palette withAccentHue(const Palette &palette, double hue) {
    Palette turned = palette;
    if (hue < 0.0)
        return turned;
    const Color accent = withHue(palette.accent, static_cast<float>(hue));
    const Color &first = palette.ops[0];
    if (first.r == palette.accent.r && first.g == palette.accent.g && first.b == palette.accent.b)
        turned.ops[0] = accent;
    turned.accent = accent;
    return turned;
}

Color fxTypeColor(FxType type) {
    return look.fxTypes[std::clamp(static_cast<int>(type), 0, static_cast<int>(FxType::Count) - 1)];
}
float glowing = 0.0f;
bool tintedCards = false;

// A graph's curve. Glowing, it is drawn thinner, over a soft halo of its own color that fades outward; the
// halo grows stronger and wider with the glow (at half, three strokes 6, 3.5 and 2 times the curve's width). The
// halo and the curve are strokes of one path.
namespace {
// Rings a glow is drawn in, from the outermost in, about one a pixel of its spread, so their edges, each a soft pixel
// wide, blend together. Where they overlap it builds up toward the middle: the outer ones fainter and the inner ones
// stronger, so it rises as a smoothstep from nothing at its outer edge (no edge to see there, nor a kink where a
// straight ramp would start) to its full strength by what it glows round.
constexpr int kMaxFeatherSteps = 32;
int featherSteps(float spread) {
    return std::clamp(static_cast<int>(std::ceil(spread)), 6, kMaxFeatherSteps);
}
// Ring k's alpha (k = 0 the outermost) of `steps`, its share of `total`.
float featherRing(int k, int steps, float total) {
    auto rise = [](float x) { return x * x * (3.0f - 2.0f * x); };
    const float share = rise(static_cast<float>(k + 1) / steps) - rise(static_cast<float>(k) / steps);
    return std::min(total * share, 1.0f);
}
// The glow setting makes them stronger and wider: as drawn at 75 %, none when it is off.
float featherStrength() {
    return glowing / 0.75f;
}
float featherSpread(float spread) {
    return spread * (0.25f + glowing);
}
} // namespace

void featherCircle(Canvas &c, Point center, float inner, float spread, Color color) {
    if (glowing <= 0.0f)
        return;
    spread = featherSpread(spread);
    const int steps = featherSteps(spread);
    for (int k = 0; k < steps; ++k)
        c.fillCircle(center, inner + spread * (1.0f - static_cast<float>(k) / steps),
                     color.alpha(featherRing(k, steps, 1.6f * featherStrength())));
}

void featherRect(Canvas &c, Rect r, float radius, float spread, Color color) {
    if (glowing <= 0.0f)
        return;
    spread = featherSpread(spread);
    const int steps = featherSteps(spread);
    for (int k = 0; k < steps; ++k) {
        const float out = spread * (1.0f - static_cast<float>(k) / steps);
        c.fillRect(r.inset(-out), color.alpha(featherRing(k, steps, 1.6f * featherStrength())), radius + out);
    }
}

void featherLine(Canvas &c, const Point *points, size_t count, float width, float spread, Color color) {
    if (glowing <= 0.0f)
        return;
    spread = featherSpread(spread);
    const int steps = featherSteps(spread);
    Color colors[kMaxFeatherSteps];
    float widths[kMaxFeatherSteps];
    for (int k = 0; k < steps; ++k) {
        colors[k] = color.alpha(featherRing(k, steps, 1.6f * featherStrength()));
        widths[k] = width + 2.0f * spread * (1.0f - static_cast<float>(k) / steps);
    }
    c.polylineStrokes(points, count, colors, widths, static_cast<size_t>(steps));
}

void softShadow(Canvas &c, Rect r, float radius, int steps, float spread, float drop, float alpha) {
    for (int k = steps; k > 0; --k) {
        const float out = spread * k;
        c.fillRect({r.x - out, r.y - out + drop, r.w + 2.0f * out, r.h + 2.0f * out},
                   Color{0.0f, 0.0f, 0.0f, alpha / steps}, radius + out);
    }
}

namespace {
// How dark a node's shadow is: fainter on a light theme's background.
Color nodeShadow() {
    return Color{0.0f, 0.0f, 0.0f, lightTheme() ? 0.12f : 0.3f};
}
} // namespace

void paintNodeShadow(Canvas &c, Rect r, float radius) {
    if (look.flat)
        c.fillRect({r.x - 1.0f, r.y + 1.0f, r.w + 2.0f, r.h + 2.0f}, nodeShadow(), radius + 1.0f);
}

void paintNodeShadow(Canvas &c, Point center, float radius) {
    if (look.flat)
        c.fillCircle({center.x, center.y + 2.0f}, radius + 1.0f, nodeShadow());
}

void paintCardShadow(Canvas &c, Rect r, float radius) {
    if (look.cardShadow > 0.0f) // close and faint, a little lower than the card
        softShadow(c, r, radius, 3, 1.0f, 1.5f, look.cardShadow);
}

Color popupFill() {
    if (!look.flat)
        return kRaised;
    // A light theme's white, as its panels, lifted off them by the shadow; a dark one's a step lighter than the raised
    // rows it may sit on, which a shadow on a dark ground alone wouldn't part it from.
    return lightTheme() ? kPanel : mix(kRaised, kText, 0.05f);
}

void paintMenuPanel(Canvas &c, Rect r, float radius) {
    if (look.flat) {
        // Dropped a little below it and tight at the sides, so it reads as lifted rather than as a halo; darker on a
        // dark theme's ground, where a faint one wouldn't show.
        if (lightTheme())
            softShadow(c, r, radius, 4, 1.0f, 3.0f, 0.16f);
        else
            softShadow(c, r, radius, 4, 1.25f, 3.0f, 0.5f);
    }
    c.fillRect(r, popupFill(), radius);
    if (!look.flat)
        c.strokeRect(r, kBorder, 1.0f, radius);
}

void paintMenuRow(Canvas &c, Rect r, bool hovered, bool selected) {
    if (!hovered && !selected)
        return;
    if (look.flat) // a shade of the text over the popup's fill, which shows on its white or its lighter dark alike
        c.fillRect(r, kText.alpha(hovered ? 0.09f : 0.05f), kRadiusControl);
    else
        c.fillRect(r, hovered ? kTrack : kTrack.alpha(0.5f), kRadiusControl);
}

void paintPill(Canvas &c, Rect r, bool lit) {
    c.fillRect(r, lit ? kAccent.alpha(0.24f) : kTrack, std::min(kRadiusPanel, r.h * 0.5f));
}

Color paintActionFace(Canvas &c, Rect r, bool hot, bool can, float radius) {
    c.fillRect(r, hot && can ? kTrack : kRaised, radius);
    if (outlined())
        c.strokeRect(r, kBorder, 1.0f, radius);
    return !can ? kFaint : hot ? kText : kDim;
}

Color paintMainActionFace(Canvas &c, Rect r, bool hot, bool lit, float radius) {
    if (lit && look.flat) {
        c.fillRect(r, kAccent, radius);
        return litInk(kAccent);
    }
    c.fillRect(r, kAccent.alpha(lit ? 0.3f : hot ? 0.24f : 0.14f), radius);
    if (!look.flat)
        c.strokeRect(r.inset(0.5f), kAccent.alpha(lit || hot ? 1.0f : 0.7f), 1.0f, radius);
    return hot && !lit ? kText : kAccent;
}

// As tall as the power switch beside it (cardPower() in layout.h), about the same middle.
void paintTitleActionFace(Canvas &c, Rect r, bool hovered) {
    if (hovered)
        c.fillRect({r.x, r.center().y - 9.5f, r.w, 19.0f}, look.flat ? wellHover() : kTrack, kRadiusControl);
}

void paintChevron(Canvas &c, Point m, Color ink, ChevronWay way, bool inRow) {
    // Pointing down: its arms `half` either side, from `back` above the middle to its tip `tip` below.
    const float half = inRow ? 3.5f : 4.0f, back = inRow ? 1.5f : 2.0f, tip = inRow ? 2.0f : 2.5f;
    const float width = inRow ? 1.3f : 1.5f;
    Point points[3];
    switch (way) {
    case ChevronWay::Down:
        points[0] = {m.x - half, m.y - back}, points[1] = {m.x, m.y + tip}, points[2] = {m.x + half, m.y - back};
        break;
    case ChevronWay::Up:
        points[0] = {m.x - half, m.y + back}, points[1] = {m.x, m.y - tip}, points[2] = {m.x + half, m.y + back};
        break;
    case ChevronWay::Right:
        points[0] = {m.x - back, m.y - half}, points[1] = {m.x + tip, m.y}, points[2] = {m.x - back, m.y + half};
        break;
    case ChevronWay::Left:
        points[0] = {m.x + back, m.y - half}, points[1] = {m.x - tip, m.y}, points[2] = {m.x + back, m.y + half};
        break;
    }
    c.polyline(points, 3, ink, width);
}

// Along the chip's foot, inside its rounded ends: a faint track, filled in the aux's color as far as the aux depth.
void paintAuxDepthBar(Canvas &c, Rect chip, int aux, double depth) {
    const float inset = chip.h * 0.5f;
    const Rect track{chip.x + inset, chip.bottom() - 3.0f, chip.w - 2.0f * inset, 2.0f};
    c.fillRect(track, kSourceColors[aux].alpha(0.25f), kRadiusMark);
    c.fillRect({track.x, track.y, track.w * static_cast<float>(std::clamp(depth, 0.0, 1.0)), track.h},
               kSourceColors[aux], kRadiusMark);
}

void plotLine(Canvas &c, const Point *points, size_t count, Color color, float width) {
    if (glowing > 0.0f) {
        const float strength = 2.0f * glowing, radius = 0.5f + glowing;
        const Color colors[] = {color.alpha(0.05f * strength), color.alpha(0.08f * strength),
                                color.alpha(0.16f * strength), color};
        const float widths[] = {width * (1.0f + 5.0f * radius), width * (1.0f + 2.5f * radius),
                                width * (1.0f + radius), width * (1.0f - 0.3f * std::min(strength, 1.0f))};
        c.polylineStrokes(points, count, colors, widths, std::size(colors));
        return;
    }
    c.polyline(points, count, color, width);
}

// What the graphs show of a level that moves with the sound (the glows behind OUT and around the nodes, the effects
// graph's nodes and wires, the routing graph's wires swelling with their modulation) follows it rather than jumping
// from one snapshot to the next: rising to it with a time constant of 70 ms and falling back with one of 100 ms, at
// the sixty polls a second the editor makes.
float follow(float shown, float target) {
    constexpr float kPollSeconds = 1.0f / 60.0f, kAttackSeconds = 0.07f, kReleaseSeconds = 0.1f;
    static const float attack = 1.0f - std::exp(-kPollSeconds / kAttackSeconds);
    static const float release = 1.0f - std::exp(-kPollSeconds / kReleaseSeconds);
    shown += (target - shown) * (target > shown ? attack : release);
    return std::abs(target - shown) < 1e-3f ? target : shown;
}

// A name in capitals, as a folded card's strip has it.
std::string upperCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

// --- The plus before the name on + EFFECT and + SOURCE.

// The plus, its left end at `left`.
static void paintAddPlus(Canvas &c, Point left, Color color) {
    const Point m{left.x + kAddPlusArm, left.y};
    c.line({m.x - kAddPlusArm, m.y}, {m.x + kAddPlusArm, m.y}, color, 1.5f);
    c.line({m.x, m.y - kAddPlusArm}, {m.x, m.y + kAddPlusArm}, color, 1.5f);
}

// + EFFECT and + SOURCE: a plus and the name, then a chevron for the list it opens. Neutral at rest, in the theme's
// accent under the mouse and while its list is open; dimmed while every place is taken (`full`). In a theme with pill
// buttons, a filled pill without an outline, its plus and name in full text at rest and its chevron dim.
void paintMenuButton(Canvas &c, Rect r, const char *name, bool hovered, bool open, bool full) {
    const bool lit = (hovered || open) && !full;
    const bool pill = look.flat;
    const Color ink = full ? kFaint : (lit || pill ? kText : kDim);
    if (pill) {
        paintPill(c, r, lit);
    } else {
        c.fillRect(r, lit ? kAccent.alpha(0.18f) : kRaised, kRadiusControl);
        c.strokeRect(r, lit ? kAccent.alpha(0.9f) : kBorder, 1.0f, kRadiusControl);
    }
    paintAddPlus(c, {r.x + 10.0f, r.center().y}, ink);
    c.text({r.x + 10.0f + kAddPlusW, r.y - kAddNameLift, r.w - 30.0f - kAddPlusW, r.h}, name, kTextLabel, ink,
           Align::Left, true);
    paintChevron(c, {r.right() - 13.0f, r.center().y}, pill && !lit && !full ? kDim : ink);
}

// A card that adds one, its plus `arm` long each way about `m`: dashed and faint at rest, lit in the accent under the
// mouse; or in a flat theme filled faintly without an outline (a tint of the accent under the mouse), its plus on a
// disc as + SOURCE's pill is filled. Gives the ink for what it says under the plus.
Color paintAddCard(Canvas &c, Rect r, Point m, float arm, bool hovered) {
    if (look.flat) {
        c.fillRect(r, hovered ? kAccent.alpha(0.1f) : kTrack.alpha(0.3f), kRadiusCard);
        c.fillCircle(m, arm + 7.0f, hovered ? kAccent.alpha(0.3f) : kTrack);
        arm *= 0.6f;
    } else {
        if (hovered)
            c.fillRect(r, kAccent.alpha(0.07f), kRadiusCard);
        c.strokeRect(r, hovered ? kAccent.alpha(0.8f) : kFaint.alpha(0.5f), 1.2f, kRadiusCard, true);
    }
    const Color ink = hovered ? kText : look.flat ? kDim : kFaint.alpha(0.8f);
    c.line({m.x - arm, m.y}, {m.x + arm, m.y}, ink, 2.0f);
    c.line({m.x, m.y - arm}, {m.x, m.y + arm}, ink, 2.0f);
    return ink;
}

// A name down a folded card's strip, a character a line, a number after a space on one line of its own.
void paintDownward(Canvas &c, Rect r, std::string_view name, Color color) {
    size_t number = name.size();
    while (number > 0 && std::isdigit(static_cast<unsigned char>(name[number - 1])))
        --number;
    if (number == 0 || name[number - 1] != ' ')
        number = name.size(); // no number after a space: every character down
    size_t line = 0;
    for (size_t i = 0; i < name.size() && (line + 1) * 13.0f <= r.h; ++i, ++line) {
        const size_t length = i == number ? name.size() - i : 1;
        if (name[i] != ' ')
            c.text({r.x, r.y + line * 13.0f, r.w, 13.0f}, std::string(name.substr(i, length)), kTextLabel, color,
                   Align::Center, true);
        i += length - 1;
    }
}

} // namespace fmvibe::gui::detail
