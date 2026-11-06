#pragma once

// Direct2D and DirectWrite on Windows; in the browser build, an HTML canvas (canvas_web.cpp).
#ifdef _WIN32
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

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
    ID2D1StrokeStyle *dottedStroke() const { return dotted_.Get(); }
    IDWriteFactory *writeFactory() const { return write_.Get(); }
    IDWriteTextFormat *textFormat(float size, bool bold, Align align);
    // The widths Canvas::textWidth() measured, by text format and text (see there): measuring one lays the text out.
    std::unordered_map<std::string, float> &textWidths() { return widths_; }

    // A path geometry made from a few numbers (an arc's, a curve's), kept for the next frame, which mostly draws the
    // same ones again (knobs at rest, wires): `build` fills the sink of a path made for a key not kept yet.
    struct PathKey {
        uint32_t kind = 0;
        std::array<float, 7> values{};
    };
    template <class Build>
    ID2D1PathGeometry *path(const PathKey &key, const Build &build) {
        if (const auto found = paths_.find(key); found != paths_.end())
            return found->second.Get();
        ComPtr<ID2D1PathGeometry> path;
        factory_->CreatePathGeometry(path.GetAddressOf());
        ComPtr<ID2D1GeometrySink> sink;
        path->Open(sink.GetAddressOf());
        build(sink.Get());
        sink->Close();
        if (paths_.size() >= 4096) // a few thousand at most, as the text widths
            paths_.clear();
        return paths_.emplace(key, path).first->second.Get();
    }

private:
    // Keys alike byte for byte, so a key's hash and equality agree (-0 and 0, NaN).
    struct PathKeyOps {
        size_t operator()(const PathKey &key) const noexcept {
            const auto *bytes = reinterpret_cast<const unsigned char *>(&key);
            size_t hash = 14695981039346656037ull;
            for (size_t i = 0; i < sizeof key; ++i)
                hash = (hash ^ bytes[i]) * 1099511628211ull;
            return hash;
        }
        bool operator()(const PathKey &a, const PathKey &b) const noexcept {
            return std::memcmp(&a, &b, sizeof a) == 0;
        }
    };
    std::unordered_map<PathKey, ComPtr<ID2D1PathGeometry>, PathKeyOps, PathKeyOps> paths_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<ID2D1StrokeStyle> dashed_;
    ComPtr<ID2D1StrokeStyle> dotted_;
    std::map<std::tuple<int, bool, Align>, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<std::string, float> widths_;
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
    // Dotted rather than dashed (round dots a few widths apart): a quadratic curve, and a circle.
    void curveDotted(Point a, Point control, Point b, Color c, float width);
    void strokeCircleDotted(Point center, float radius, Color c, float width);
    void polyline(const Point *points, size_t count, Color c, float width, bool dashed = false);
    // The same polyline stroked `strokes` times over, in colors[i] at widths[i] in turn, as that many polyline()
    // calls would draw it; its path is built once.
    void polylineStrokes(const Point *points, size_t count, const Color *colors, const float *widths, size_t strokes,
                         bool dashed = false);
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
