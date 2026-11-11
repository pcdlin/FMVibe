#include "theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>

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

} // namespace fmvibe::gui::detail
