// The Canvas over the page's <canvas> in the browser build: each call goes to Module.fmv2d (web/site/canvas2d.js),
// which draws with the 2D context. Same results as canvas.cpp's Direct2D, as far as the 2D context allows.

#include "canvas.h"

#include <emscripten/em_js.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace fmvibe::gui {

namespace {

// Colors cross as one 0xRRGGBBAA number; the JS side caches the style string for each. Each channel is rounded as
// std::lround() would, halves up, without its call: in double, adding the half to a float is exact.
uint32_t rgba(Color c) {
    auto byte = [](float v) {
        const float scaled = std::clamp(v, 0.0f, 1.0f) * 255.0f;
        return scaled > 0.0f ? static_cast<uint32_t>(static_cast<double>(scaled) + 0.5) : 0u;
    };
    return byte(c.r) << 24 | byte(c.g) << 16 | byte(c.b) << 8 | byte(c.a);
}

// Lifts the small label sizes toward the body size so they read more easily, keeping their order (as canvas.cpp).
float liftedSize(float size) {
    constexpr float kLiftBelow = 11.5f;
    return size < kLiftBelow ? size + (kLiftBelow - size) * 0.4f : size;
}

} // namespace

EM_JS_DEPS(fmv_canvas, "$UTF8ToString");
EM_JS(void, js_clear, (uint32_t color), { Module.fmv2d.clear(color); });
EM_JS(void, js_fill_rect, (float x, float y, float w, float h, uint32_t color, float radius),
      { Module.fmv2d.fillRect(x, y, w, h, color, radius); });
EM_JS(void, js_stroke_rect, (float x, float y, float w, float h, uint32_t color, float width, float radius, int dashed),
      { Module.fmv2d.strokeRect(x, y, w, h, color, width, radius, dashed != 0); });
EM_JS(void, js_fill_gradient, (float x, float y, float w, float h, uint32_t top, uint32_t bottom, float radius),
      { Module.fmv2d.fillGradient(x, y, w, h, top, bottom, radius); });
EM_JS(void, js_fill_circle, (float x, float y, float radius, uint32_t color),
      { Module.fmv2d.fillCircle(x, y, radius, color); });
EM_JS(void, js_stroke_circle, (float x, float y, float radius, uint32_t color, float width),
      { Module.fmv2d.strokeCircle(x, y, radius, color, width); });
EM_JS(void, js_stroke_circle_dotted, (float x, float y, float radius, uint32_t color, float width),
      { Module.fmv2d.strokeCircleDotted(x, y, radius, color, width); });
EM_JS(void, js_curve_dotted, (float x1, float y1, float cx, float cy, float x2, float y2, uint32_t color, float width),
      { Module.fmv2d.curveDotted(x1, y1, cx, cy, x2, y2, color, width); });
EM_JS(void, js_line,(float x1, float y1, float x2, float y2, uint32_t color, float width, int dashed),
      { Module.fmv2d.line(x1, y1, x2, y2, color, width, dashed != 0); });
EM_JS(void, js_arc, (float x, float y, float radius, float from, float to, uint32_t color, float width),
      { Module.fmv2d.arc(x, y, radius, from, to, color, width); });
EM_JS(void, js_curve, (float x1, float y1, float cx, float cy, float x2, float y2, uint32_t color, float width, int dashed),
      { Module.fmv2d.curve(x1, y1, cx, cy, x2, y2, color, width, dashed != 0); });
// Points are (x, y) float pairs in the module's memory.
EM_JS(void, js_polyline, (const float *points, int count, uint32_t color, float width, int dashed),
      { Module.fmv2d.polyline(HEAPF32.subarray(points >> 2, (points >> 2) + 2 * count), color, width, dashed != 0); });
// The same points stroked up to four times over (`strokes` of the colors and widths, in turn), in one crossing.
EM_JS(void, js_polyline_strokes,
      (const float *points, int count, int strokes, uint32_t color0, float width0, uint32_t color1, float width1,
       uint32_t color2, float width2, uint32_t color3, float width3, int dashed),
      {
          Module.fmv2d.polylineStrokes(HEAPF32.subarray(points >> 2, (points >> 2) + 2 * count), strokes, color0,
                                       width0, color1, width1, color2, width2, color3, width3, dashed != 0);
      });
EM_JS(void, js_fill_polygon, (const float *points, int count, uint32_t color),
      { Module.fmv2d.fillPolygon(HEAPF32.subarray(points >> 2, (points >> 2) + 2 * count), color); });
EM_JS(void, js_text,
      (float x, float y, float w, float h, const char *utf8, int length, float size, uint32_t color, int align, int bold),
      { Module.fmv2d.text(x, y, w, h, UTF8ToString(utf8, length), size, color, align, bold != 0); });
EM_JS(float, js_text_width, (const char *utf8, int length, float size, int bold),
      { return Module.fmv2d.textWidth(UTF8ToString(utf8, length), size, bold != 0); });
EM_JS(void, js_push_clip, (float x, float y, float w, float h), { Module.fmv2d.pushClip(x, y, w, h); });
EM_JS(void, js_pop_clip, (), { Module.fmv2d.popClip(); });
EM_JS(void, js_push_layer, (float x, float y, float opacity), { Module.fmv2d.pushLayer(x, y, opacity); });
EM_JS(void, js_pop_layer, (), { Module.fmv2d.popLayer(); });

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Canvas::Canvas(Resources &) {}

Point Canvas::onCircle(Point center, float radius, float angle) {
    return {center.x + radius * std::sin(angle), center.y - radius * std::cos(angle)};
}

void Canvas::clear(Color c) {
    js_clear(rgba(c));
}

void Canvas::fillRect(Rect r, Color c, float radius) {
    js_fill_rect(r.x, r.y, r.w, r.h, rgba(c), radius);
}

void Canvas::strokeRect(Rect r, Color c, float width, float radius, bool dashed) {
    const Rect half = r.inset(width * 0.5f);
    js_stroke_rect(half.x, half.y, half.w, half.h, rgba(c), width, radius, dashed);
}

void Canvas::fillGradient(Rect r, Color top, Color bottom, float radius) {
    js_fill_gradient(r.x, r.y, r.w, r.h, rgba(top), rgba(bottom), radius);
}

void Canvas::fillCircle(Point center, float radius, Color c) {
    js_fill_circle(center.x, center.y, radius, rgba(c));
}

void Canvas::strokeCircle(Point center, float radius, Color c, float width) {
    js_stroke_circle(center.x, center.y, radius, rgba(c), width);
}

void Canvas::strokeCircleDotted(Point center, float radius, Color c, float width) {
    js_stroke_circle_dotted(center.x, center.y, radius, rgba(c), width);
}

void Canvas::curveDotted(Point a, Point control, Point b, Color c, float width) {
    js_curve_dotted(a.x, a.y, control.x, control.y, b.x, b.y, rgba(c), width);
}

void Canvas::line(Point a, Point b, Color c, float width, bool dashed) {
    js_line(a.x, a.y, b.x, b.y, rgba(c), width, dashed);
}

void Canvas::arc(Point center, float radius, float from, float to, Color c, float width) {
    if (to < from)
        std::swap(from, to);
    if (to - from < 1e-3f)
        return;
    if (to - from >= 2.0f * std::numbers::pi_v<float> - 1e-3f) {
        strokeCircle(center, radius, c, width);
        return;
    }
    js_arc(center.x, center.y, radius, from, to, rgba(c), width);
}

void Canvas::curve(Point a, Point control, Point b, Color c, float width, bool dashed) {
    js_curve(a.x, a.y, control.x, control.y, b.x, b.y, rgba(c), width, dashed);
}

void Canvas::polyline(const Point *points, size_t count, Color c, float width, bool dashed) {
    static_assert(sizeof(Point) == 2 * sizeof(float));
    if (count >= 2)
        js_polyline(&points[0].x, static_cast<int>(count), rgba(c), width, dashed);
}

void Canvas::polylineStrokes(const Point *points, size_t count, const Color *colors, const float *widths,
                             size_t strokes, bool dashed) {
    if (count < 2)
        return;
    for (size_t first = 0; first < strokes; first += 4) { // four strokes a crossing
        const size_t n = std::min<size_t>(strokes - first, 4);
        auto color = [&](size_t i) { return i < n ? rgba(colors[first + i]) : 0u; };
        auto width = [&](size_t i) { return i < n ? widths[first + i] : 0.0f; };
        js_polyline_strokes(&points[0].x, static_cast<int>(count), static_cast<int>(n), color(0), width(0), color(1),
                            width(1), color(2), width(2), color(3), width(3), dashed);
    }
}

void Canvas::fillPolygon(const Point *points, size_t count, Color c) {
    if (count >= 3)
        js_fill_polygon(&points[0].x, static_cast<int>(count), rgba(c));
}

void Canvas::text(Rect r, std::string_view utf8, float size, Color c, Align align, bool bold) {
    if (!utf8.empty())
        js_text(r.x, r.y, r.w, r.h, utf8.data(), static_cast<int>(utf8.size()), liftedSize(size), rgba(c),
                static_cast<int>(align), bold);
}

float Canvas::textWidth(std::string_view utf8, float size, bool bold) {
    return utf8.empty() ? 0.0f : js_text_width(utf8.data(), static_cast<int>(utf8.size()), liftedSize(size), bold);
}

void Canvas::pushClip(Rect r) {
    js_push_clip(r.x, r.y, r.w, r.h);
}

void Canvas::popClip() {
    js_pop_clip();
}

void Canvas::pushLayer(Point offset, float opacity, Rect bounds) {
    js_push_layer(offset.x, offset.y, opacity);
    js_push_clip(bounds.x, bounds.y, bounds.w, bounds.h); // only what is inside shows, as in the other backends
}

void Canvas::popLayer() {
    js_pop_clip();
    js_pop_layer();
}

} // namespace fmvibe::gui
