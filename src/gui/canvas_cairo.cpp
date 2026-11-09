// The Canvas over a Cairo context, for the Linux development build (FMVIBE_CAIRO): the quality test and the UI
// snapshots run there, so cloud sessions on Linux can build, test and look at the editor. It follows canvas.cpp's
// Direct2D drawing as canvas2d.js does: round caps and joins on lines and curves, a dash of twice the width, dots,
// text centered in its box, cut with an ellipsis and clipped to it. The fonts differ (Segoe UI is Windows'), so text
// widths come out a little unlike the plugin's; the text uses FMVIBE_FONT when it is set, else Inter, else
// whatever sans-serif font fontconfig finds.

#include "canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

namespace {

// Lifts the small label sizes toward the body size so they read more easily, keeping their order (as canvas.cpp).
float liftedSize(float size) {
    constexpr float kLiftBelow = 11.5f;
    return size < kLiftBelow ? size + (kLiftBelow - size) * 0.4f : size;
}

const char *fontFamily() {
    static const std::string family = [] {
        const char *chosen = std::getenv("FMVIBE_FONT");
        return std::string(chosen && *chosen ? chosen : "Inter");
    }();
    return family.c_str();
}

void setFont(cairo_t *cr, float size, bool bold) {
    cairo_select_font_face(cr, fontFamily(), CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

// `text` with each byte that doesn't start a whole UTF-8 sequence (a macro name a state held, say) as U+FFFD, as
// Direct2D shows it: Cairo would refuse the whole string, and every drawing after it on the surface.
std::string validUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    auto byte = [&](size_t i) { return i < text.size() ? static_cast<unsigned char>(text[i]) : 0u; };
    auto follows = [&](size_t i) { return (byte(i) & 0xC0u) == 0x80u; };
    for (size_t i = 0; i < text.size();) {
        const unsigned b = byte(i);
        size_t length = 0;
        if (b < 0x80)
            length = 1;
        else if (b >= 0xC2 && b <= 0xDF && follows(i + 1))
            length = 2;
        else if (b >= 0xE0 && b <= 0xEF && follows(i + 1) && follows(i + 2) && (b != 0xE0 || byte(i + 1) >= 0xA0) &&
                 (b != 0xED || byte(i + 1) < 0xA0))
            length = 3; // not overlong, nor a surrogate
        else if (b >= 0xF0 && b <= 0xF4 && follows(i + 1) && follows(i + 2) && follows(i + 3) &&
                 (b != 0xF0 || byte(i + 1) >= 0x90) && (b != 0xF4 || byte(i + 1) < 0x90))
            length = 4; // not overlong, nor past U+10FFFF
        if (length == 0) {
            out += "\xEF\xBF\xBD";
            ++i;
        } else {
            out.append(text.substr(i, length));
            i += length;
        }
    }
    return out;
}

double advance(cairo_t *cr, const std::string &utf8) {
    cairo_text_extents_t extents;
    cairo_text_extents(cr, utf8.c_str(), &extents);
    return extents.x_advance;
}

// Direct2D clamps radii that don't fit.
void roundRectPath(cairo_t *cr, Rect r, float radius) {
    cairo_new_path(cr);
    const float most = std::min(std::abs(r.w), std::abs(r.h)) * 0.5f;
    radius = std::clamp(radius, 0.0f, most);
    if (radius <= 0.0f) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        return;
    }
    constexpr double kPi = std::numbers::pi;
    cairo_arc(cr, r.right() - radius, r.y + radius, radius, -kPi / 2, 0);
    cairo_arc(cr, r.right() - radius, r.bottom() - radius, radius, 0, kPi / 2);
    cairo_arc(cr, r.x + radius, r.bottom() - radius, radius, kPi / 2, kPi);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

void pointsPath(cairo_t *cr, const Point *points, size_t count) {
    cairo_new_path(cr);
    cairo_move_to(cr, points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i)
        cairo_line_to(cr, points[i].x, points[i].y);
}

// A quadratic curve, as the cubic Cairo draws.
void curvePath(cairo_t *cr, Point a, Point control, Point b) {
    cairo_new_path(cr);
    cairo_move_to(cr, a.x, a.y);
    cairo_curve_to(cr, a.x + (control.x - a.x) * 2.0 / 3.0, a.y + (control.y - a.y) * 2.0 / 3.0,
                   b.x + (control.x - b.x) * 2.0 / 3.0, b.y + (control.y - b.y) * 2.0 / 3.0, b.x, b.y);
}

void circlePath(cairo_t *cr, Point center, float radius) {
    cairo_new_path(cr);
    cairo_arc(cr, center.x, center.y, std::max(0.0f, radius), 0, 2 * std::numbers::pi);
    cairo_close_path(cr);
}

} // namespace

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Canvas::Canvas(cairo_t *target, Resources &) : target_(target) {}

Point Canvas::onCircle(Point center, float radius, float angle) {
    return {center.x + radius * std::sin(angle), center.y - radius * std::cos(angle)};
}

void Canvas::setColor(Color c) {
    cairo_set_source_rgba(target_, c.r, c.g, c.b, c.a);
}

void Canvas::stroke(Color c, float width, Stroke style) {
    setColor(c);
    cairo_set_line_width(target_, width);
    const bool square = style == Stroke::Square;
    cairo_set_line_cap(target_, square ? CAIRO_LINE_CAP_BUTT : CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(target_, square ? CAIRO_LINE_JOIN_MITER : CAIRO_LINE_JOIN_ROUND);
    if (style == Stroke::Dashed) {
        const double dashes[] = {2.0 * width, 2.0 * width};
        cairo_set_dash(target_, dashes, 2, 0);
    } else if (style == Stroke::Dotted) {
        const double dots[] = {0.01 * width, 2.5 * width};
        cairo_set_dash(target_, dots, 2, 0);
    } else {
        cairo_set_dash(target_, nullptr, 0, 0);
    }
    cairo_stroke(target_);
}

void Canvas::clear(Color c) {
    cairo_save(target_);
    cairo_identity_matrix(target_);
    cairo_set_operator(target_, CAIRO_OPERATOR_SOURCE);
    setColor(c);
    cairo_paint(target_);
    cairo_restore(target_);
}

void Canvas::fillRect(Rect r, Color c, float radius) {
    roundRectPath(target_, r, radius);
    setColor(c);
    cairo_fill(target_);
}

void Canvas::strokeRect(Rect r, Color c, float width, float radius, bool dashed) {
    roundRectPath(target_, r.inset(width * 0.5f), radius);
    stroke(c, width, dashed ? Stroke::Dashed : Stroke::Square);
}

void Canvas::fillGradient(Rect r, Color top, Color bottom, float radius) {
    cairo_pattern_t *gradient = cairo_pattern_create_linear(r.x, r.y, r.x, r.bottom());
    cairo_pattern_add_color_stop_rgba(gradient, 0, top.r, top.g, top.b, top.a);
    cairo_pattern_add_color_stop_rgba(gradient, 1, bottom.r, bottom.g, bottom.b, bottom.a);
    roundRectPath(target_, r, radius);
    cairo_set_source(target_, gradient);
    cairo_fill(target_);
    cairo_pattern_destroy(gradient);
}

void Canvas::fillCircle(Point center, float radius, Color c) {
    circlePath(target_, center, radius);
    setColor(c);
    cairo_fill(target_);
}

void Canvas::strokeCircle(Point center, float radius, Color c, float width) {
    circlePath(target_, center, radius);
    stroke(c, width, Stroke::Square);
}

void Canvas::strokeCircleDotted(Point center, float radius, Color c, float width) {
    circlePath(target_, center, radius);
    stroke(c, width, Stroke::Dotted);
}

void Canvas::curveDotted(Point a, Point control, Point b, Color c, float width) {
    curvePath(target_, a, control, b);
    stroke(c, width, Stroke::Dotted);
}

void Canvas::line(Point a, Point b, Color c, float width, bool dashed) {
    cairo_new_path(target_);
    cairo_move_to(target_, a.x, a.y);
    cairo_line_to(target_, b.x, b.y);
    stroke(c, width, dashed ? Stroke::Dashed : Stroke::Round);
}

// The editor's angles run clockwise from 12 o'clock, Cairo's (with y down) clockwise from 3 o'clock.
void Canvas::arc(Point center, float radius, float from, float to, Color c, float width) {
    if (to < from)
        std::swap(from, to);
    if (to - from < 1e-3f)
        return;
    if (to - from >= 2.0f * std::numbers::pi_v<float> - 1e-3f) {
        strokeCircle(center, radius, c, width);
        return;
    }
    cairo_new_path(target_);
    cairo_arc(target_, center.x, center.y, std::max(0.0f, radius), from - std::numbers::pi / 2,
              to - std::numbers::pi / 2);
    stroke(c, width, Stroke::Round);
}

void Canvas::curve(Point a, Point control, Point b, Color c, float width, bool dashed) {
    curvePath(target_, a, control, b);
    stroke(c, width, dashed ? Stroke::Dashed : Stroke::Round);
}

void Canvas::polyline(const Point *points, size_t count, Color c, float width, bool dashed) {
    polylineStrokes(points, count, &c, &width, 1, dashed);
}

void Canvas::polylineStrokes(const Point *points, size_t count, const Color *colors, const float *widths,
                             size_t strokes, bool dashed) {
    if (count < 2 || strokes == 0)
        return;
    pointsPath(target_, points, count);
    cairo_path_t *path = strokes > 1 ? cairo_copy_path(target_) : nullptr;
    for (size_t i = 0; i < strokes; ++i) {
        if (i > 0) {
            cairo_new_path(target_);
            cairo_append_path(target_, path);
        }
        stroke(colors[i], widths[i], dashed ? Stroke::Dashed : Stroke::Round);
    }
    if (path)
        cairo_path_destroy(path);
}

void Canvas::fillPolygon(const Point *points, size_t count, Color c) {
    if (count < 3)
        return;
    pointsPath(target_, points, count);
    cairo_close_path(target_);
    setColor(c);
    cairo_fill(target_);
}

// One line, centered up and down in its box like DirectWrite's paragraph centering, and cut with an ellipsis when it
// is wider than the box (DirectWrite's character trimming). Nothing shows in an empty box, or one too narrow for the
// ellipsis itself, and nothing outside the box.
void Canvas::text(Rect r, std::string_view utf8, float size, Color c, Align align, bool bold) {
    if (utf8.empty() || !(r.w > 0.0f && r.h > 0.0f))
        return;
    setFont(target_, liftedSize(size), bold);
    std::string shown = validUtf8(utf8);
    double width = advance(target_, shown);
    if (width > r.w) {
        static const std::string kEllipsis = "\xE2\x80\xA6";
        if (advance(target_, kEllipsis) > r.w)
            return;
        // The character boundaries: the byte each UTF-8 sequence starts at.
        std::vector<size_t> starts;
        for (size_t i = 0; i < shown.size(); ++i)
            if ((static_cast<unsigned char>(shown[i]) & 0xC0) != 0x80)
                starts.push_back(i); // the first among them: valid UTF-8 starts with one
        size_t lo = 0, hi = starts.size() - 1; // the most characters that fit before the ellipsis
        while (lo < hi) {
            const size_t mid = (lo + hi + 1) / 2;
            if (advance(target_, shown.substr(0, starts[mid]) + kEllipsis) <= r.w)
                lo = mid;
            else
                hi = mid - 1;
        }
        shown = shown.substr(0, lo < starts.size() ? starts[lo] : shown.size()) + kEllipsis;
        width = advance(target_, shown);
    }
    cairo_font_extents_t font;
    cairo_font_extents(target_, &font);
    const double x = align == Align::Left ? r.x : align == Align::Right ? r.right() - width : r.x + (r.w - width) / 2;
    const double y = r.y + (r.h - font.ascent - font.descent) / 2 + font.ascent;
    cairo_save(target_);
    cairo_new_path(target_);
    cairo_rectangle(target_, r.x, r.y, r.w, r.h);
    cairo_clip(target_);
    setColor(c);
    cairo_move_to(target_, x, y);
    cairo_show_text(target_, shown.c_str());
    cairo_restore(target_);
}

float Canvas::textWidth(std::string_view utf8, float size, bool bold) {
    if (utf8.empty())
        return 0.0f;
    setFont(target_, liftedSize(size), bold);
    return static_cast<float>(advance(target_, validUtf8(utf8)));
}

void Canvas::pushClip(Rect r) {
    cairo_save(target_);
    cairo_new_path(target_);
    cairo_rectangle(target_, r.x, r.y, r.w, r.h);
    cairo_clip(target_);
}

void Canvas::popClip() {
    cairo_restore(target_);
}

void Canvas::pushLayer(Point offset, float opacity, Rect bounds) {
    cairo_save(target_);
    cairo_translate(target_, offset.x, offset.y);
    cairo_new_path(target_);
    cairo_rectangle(target_, bounds.x, bounds.y, bounds.w, bounds.h);
    cairo_clip(target_); // the group's surface as small as that
    cairo_push_group(target_);
    opacities_.push_back(opacity);
}

void Canvas::popLayer() {
    cairo_pop_group_to_source(target_);
    cairo_paint_with_alpha(target_, opacities_.back());
    opacities_.pop_back();
    cairo_restore(target_);
}

} // namespace fmvibe::gui
