#pragma once

// Direct2D and DirectWrite on Windows; in the browser build, an HTML canvas (canvas_web.cpp).
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#endif

#include <cstddef>
#include <cstdint>
#include <map>
#include <string_view>
#include <tuple>

namespace fmvibe::gui {

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Point center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2.0f * d, h - 2.0f * d}; }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static constexpr Color hex(uint32_t rgb, float alpha = 1.0f) {
        return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
    }
    constexpr Color alpha(float a2) const { return {r, g, b, a * a2}; }
};

Color mix(Color a, Color b, float t);

enum class Align { Left, Center, Right };

// Device-independent resources that outlive a frame: factories, text formats, stroke styles.
// The browser's canvas keeps its own, so there it holds nothing.
class Resources {
public:
#ifdef _WIN32
    Resources();

    ID2D1Factory *factory() const { return factory_.Get(); }
    ID2D1StrokeStyle *roundStroke() const { return round_.Get(); }
    ID2D1StrokeStyle *dashedStroke() const { return dashed_.Get(); }
    IDWriteFactory *writeFactory() const { return write_.Get(); }
    IDWriteTextFormat *textFormat(float size, bool bold, Align align);

private:
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<ID2D1StrokeStyle> dashed_;
    std::map<std::tuple<int, bool, Align>, ComPtr<IDWriteTextFormat>> formats_;
#endif
};

// Drawing helper over any Direct2D render target (window or offscreen bitmap), or over the page's canvas in the
// browser, in the target's current transform. Angles are radians, clockwise from 12 o'clock.
class Canvas {
public:
#ifdef _WIN32
    Canvas(ID2D1RenderTarget *target, Resources &resources);
#else
    explicit Canvas(Resources &resources);
#endif

    void clear(Color c);
    void fillRect(Rect r, Color c, float radius = 0.0f);
    void strokeRect(Rect r, Color c, float width = 1.0f, float radius = 0.0f, bool dashed = false);
    void fillGradient(Rect r, Color top, Color bottom, float radius = 0.0f);
    void fillCircle(Point center, float radius, Color c);
    void strokeCircle(Point center, float radius, Color c, float width = 1.0f);
    void line(Point a, Point b, Color c, float width = 1.0f, bool dashed = false);
    void arc(Point center, float radius, float from, float to, Color c, float width);
    void curve(Point a, Point control, Point b, Color c, float width, bool dashed = false); // quadratic
    void polyline(const Point *points, size_t count, Color c, float width, bool dashed = false);
    void fillPolygon(const Point *points, size_t count, Color c);
    void text(Rect r, std::string_view utf8, float size, Color c, Align align = Align::Center, bool bold = false);
    // How wide text() draws `utf8` at this size, untrimmed.
    float textWidth(std::string_view utf8, float size, bool bold = false);
    // Everything drawn until the matching popClip() is cut to `r`.
    void pushClip(Rect r);
    void popClip();

    static Point onCircle(Point center, float radius, float angle);

private:
#ifdef _WIN32
    ID2D1SolidColorBrush *brush(Color c);

    ID2D1RenderTarget *target_;
    Resources &resources_;
    ComPtr<ID2D1SolidColorBrush> brush_;
#endif
};

} // namespace fmvibe::gui
