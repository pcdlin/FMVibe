#include "theme.h"

#include <algorithm>
#include <iterator>

namespace fmvibe::gui::detail {

int themeIndex(const std::string &name) {
    for (int i = 0; i < kThemeCount; ++i)
        if (name == kThemes[i].name)
            return i;
    return 0;
}

Palette look = kThemes[0];

Color fxTypeColor(FxType type) {
    return look.fxTypes[std::clamp(static_cast<int>(type), 0, static_cast<int>(FxType::Count) - 1)];
}
float glowing = 0.0f;

// A graph's curve. Glowing, it is drawn thinner, over a soft halo of its own color that fades outward; the
// halo grows stronger and wider with the glow (at half, three strokes 6, 3.5 and 2 times the curve's width). The
// halo and the curve are strokes of one path.
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

namespace {
constexpr Color kBlack{0.0f, 0.0f, 0.0f, 1.0f}, kWhite{1.0f, 1.0f, 1.0f, 1.0f};
// The Light and Paper themes cast lighter shadows and catch brighter highlights.
bool lightTheme() {
    return kBackground.r + kBackground.g + kBackground.b > 1.5f;
}
} // namespace

void buttonFace(Canvas &c, Rect r, Color fill, float radius) {
    const bool light = lightTheme();
    if (fill.a < 0.999f) { // pressed in: its tint over a well, darker along its top
        c.fillRect(r, mix(kPanel, kBlack, light ? 0.07f : 0.3f), radius);
        c.fillRect(r, fill, radius);
        const float shade = std::min(5.0f, r.h * 0.5f);
        c.fillGradient({r.x, r.y, r.w, shade}, kBlack.alpha(light ? 0.12f : 0.32f), kBlack.alpha(0.0f),
                       std::min(radius, shade * 0.5f));
        return;
    }
    // Raised: the edge it stands on below it, its face lighter at the top, and a highlight along its top edge.
    c.fillRect({r.x, r.y + 2.0f, r.w, r.h}, mix(kBackground, kBlack, light ? 0.3f : 0.6f).alpha(light ? 0.6f : 0.95f),
               radius);
    c.fillGradient(r, mix(fill, kWhite, light ? 0.6f : 0.08f), mix(fill, kBlack, light ? 0.05f : 0.18f), radius);
    const float inset = std::max(radius, 2.0f);
    if (r.w > 2.0f * inset)
        c.line({r.x + inset, r.y + 0.75f}, {r.right() - inset, r.y + 0.75f}, kWhite.alpha(light ? 0.9f : 0.13f),
               1.0f);
}

void knobCap(Canvas &c, Point center, float radius) {
    const bool light = lightTheme();
    c.fillCircle({center.x, center.y + 2.0f}, radius + 0.5f, kBlack.alpha(light ? 0.16f : 0.5f)); // its shadow
    c.fillCircleGradient(center, radius, {center.x - radius * 0.3f, center.y - radius * 0.55f}, radius * 1.6f,
                         mix(kKnobBody, kWhite, light ? 0.0f : 0.22f), mix(kKnobBody, kBlack, light ? 0.14f : 0.4f));
    c.strokeCircle(center, radius, kBorder, 1.0f);
    c.arc(center, radius - 1.2f, -1.0f, 1.0f, kWhite.alpha(light ? 0.9f : 0.16f), 1.0f); // light along its top
}

void cardShadow(Canvas &c, Rect r, float radius) {
    const float strength = lightTheme() ? 0.5f : 1.0f;
    for (int i = 1; i <= 3; ++i) {
        const float spread = 1.0f * i;
        c.fillRect({r.x - spread, r.y + 1.0f + spread, r.w + 2.0f * spread, r.h}, kBlack.alpha(0.07f * strength),
                   radius + spread);
    }
}

} // namespace fmvibe::gui::detail
