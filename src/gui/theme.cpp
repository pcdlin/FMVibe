#include "theme.h"

#include <algorithm>
#include <iterator>

namespace fmvibe::gui::detail {

int themeIndex(const std::string &name) {
    // A theme no longer offered, kept in a user's settings, shows as the one nearest it.
    static constexpr struct {
        const char *retired, *now;
    } kRetired[] = {{"Graphite", "Midnight"}, {"Paper", "Light"}};
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

Color fxTypeColor(FxType type) {
    return look.fxTypes[std::clamp(static_cast<int>(type), 0, static_cast<int>(FxType::Count) - 1)];
}
float glowing = 0.0f;

// A graph's curve. Glowing, it is drawn thinner, over a soft halo of its own color that fades outward; the
// halo grows stronger and wider with the glow (at half, three strokes 6, 3.5 and 2 times the curve's width). The
// halo and the curve are strokes of one path.
namespace {
// Rings a glow is drawn in, from the outermost in: each as faint, so where they overlap it builds up toward the middle.
constexpr int kFeatherSteps = 10;
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
    const Color step = color.alpha(std::min(1.6f * featherStrength() / kFeatherSteps, 1.0f));
    for (int k = 0; k < kFeatherSteps; ++k)
        c.fillCircle(center, inner + spread * (1.0f - static_cast<float>(k) / kFeatherSteps), step);
}

void featherRect(Canvas &c, Rect r, float radius, float spread, Color color) {
    if (glowing <= 0.0f)
        return;
    spread = featherSpread(spread);
    const Color step = color.alpha(std::min(1.6f * featherStrength() / kFeatherSteps, 1.0f));
    for (int k = 0; k < kFeatherSteps; ++k) {
        const float out = spread * (1.0f - static_cast<float>(k) / kFeatherSteps);
        c.fillRect(r.inset(-out), step, radius + out);
    }
}

void featherLine(Canvas &c, const Point *points, size_t count, float width, float spread, Color color) {
    if (glowing <= 0.0f)
        return;
    spread = featherSpread(spread);
    Color colors[kFeatherSteps];
    float widths[kFeatherSteps];
    for (int k = 0; k < kFeatherSteps; ++k) {
        colors[k] = color.alpha(std::min(1.6f * featherStrength() / kFeatherSteps, 1.0f));
        widths[k] = width + 2.0f * spread * (1.0f - static_cast<float>(k) / kFeatherSteps);
    }
    c.polylineStrokes(points, count, colors, widths, kFeatherSteps);
}

void paintMenuPanel(Canvas &c, Rect r) {
    if (look.flatMenus) {
        constexpr int kSteps = 4; // faint and close: each ring adds a little, about a fifth dark under the panel
        for (int k = kSteps; k > 0; --k) {
            const float out = 1.25f * k;
            c.fillRect({r.x - out, r.y - out + 2.0f, r.w + 2.0f * out, r.h + 2.0f * out}, Color{0.0f, 0.0f, 0.0f, 0.05f},
                       kRadiusCard + out);
        }
    }
    c.fillRect(r, kRaised, kRadiusCard);
    if (!look.flatMenus)
        c.strokeRect(r, kBorder, 1.0f, kRadiusCard);
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
