#include "canvas.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>

namespace fmvibe::gui {

namespace {

D2D1_POINT_2F pt(Point p) {
    return D2D1::Point2F(p.x, p.y);
}

D2D1_RECT_F rc(Rect r) {
    return D2D1::RectF(r.x, r.y, r.right(), r.bottom());
}

D2D1_COLOR_F col(Color c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

} // namespace

Color mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Resources::Resources() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(write_.GetAddressOf()));

    D2D1_STROKE_STYLE_PROPERTIES round = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(round, nullptr, 0, round_.GetAddressOf());
    D2D1_STROKE_STYLE_PROPERTIES dashed = round;
    dashed.dashStyle = D2D1_DASH_STYLE_DASH;
    factory_->CreateStrokeStyle(dashed, nullptr, 0, dashed_.GetAddressOf());
    // Dots: a dash a hair long, which the round caps make a dot, then a gap (both in widths of the stroke).
    D2D1_STROKE_STYLE_PROPERTIES dotted = round;
    dotted.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    const float dots[] = {0.01f, 2.5f};
    factory_->CreateStrokeStyle(dotted, dots, 2, dotted_.GetAddressOf());
}

IDWriteTextFormat *Resources::textFormat(float size, bool bold, Align align) {
    const auto key = std::tuple(static_cast<int>(size * 10.0f), bold, align);
    auto found = formats_.find(key);
    if (found != formats_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    write_->CreateTextFormat(L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                             format.GetAddressOf());
    format->SetTextAlignment(align == Align::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                     : DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // Clip long names with an ellipsis instead of overflowing.
    ComPtr<IDWriteInlineObject> ellipsis;
    write_->CreateEllipsisTrimmingSign(format.Get(), ellipsis.GetAddressOf());
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    format->SetTrimming(&trimming, ellipsis.Get());

    return formats_.emplace(key, format).first->second.Get();
}

void Canvas::pushClip(Rect r) {
    target_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void Canvas::popClip() {
    target_->PopAxisAlignedClip();
}

Canvas::Canvas(ID2D1RenderTarget *target, Resources &resources) : target_(target), resources_(resources) {
    target_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), brush_.GetAddressOf());
}

ID2D1SolidColorBrush *Canvas::brush(Color c) {
    brush_->SetColor(col(c));
    return brush_.Get();
}

Point Canvas::onCircle(Point center, float radius, float angle) {
    return {center.x + radius * std::sin(angle), center.y - radius * std::cos(angle)};
}

void Canvas::clear(Color c) {
    target_->Clear(col(c));
}

void Canvas::fillRect(Rect r, Color c, float radius) {
    if (radius > 0.0f)
        target_->FillRoundedRectangle(D2D1::RoundedRect(rc(r), radius, radius), brush(c));
    else
        target_->FillRectangle(rc(r), brush(c));
}

void Canvas::strokeRect(Rect r, Color c, float width, float radius, bool dashed) {
    const Rect half = r.inset(width * 0.5f);
    ID2D1StrokeStyle *style = dashed ? resources_.dashedStroke() : nullptr;
    if (radius > 0.0f)
        target_->DrawRoundedRectangle(D2D1::RoundedRect(rc(half), radius, radius), brush(c), width, style);
    else
        target_->DrawRectangle(rc(half), brush(c), width, style);
}

void Canvas::fillGradient(Rect r, Color top, Color bottom, float radius) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, col(top)}, {1.0f, col(bottom)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    target_->CreateGradientStopCollection(stops, 2, collection.GetAddressOf());
    ComPtr<ID2D1LinearGradientBrush> gradient;
    target_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.bottom())),
        collection.Get(), gradient.GetAddressOf());
    target_->FillRoundedRectangle(D2D1::RoundedRect(rc(r), radius, radius), gradient.Get());
}

void Canvas::fillCircle(Point center, float radius, Color c) {
    target_->FillEllipse(D2D1::Ellipse(pt(center), radius, radius), brush(c));
}

void Canvas::strokeCircle(Point center, float radius, Color c, float width) {
    target_->DrawEllipse(D2D1::Ellipse(pt(center), radius, radius), brush(c), width);
}

void Canvas::strokeCircleDotted(Point center, float radius, Color c, float width) {
    target_->DrawEllipse(D2D1::Ellipse(pt(center), radius, radius), brush(c), width, resources_.dottedStroke());
}

// A quadratic curve's path, kept with the resources (see Resources::path()).
static ID2D1PathGeometry *curvePath(Resources &resources, Point a, Point control, Point b) {
    return resources.path({1, {a.x, a.y, control.x, control.y, b.x, b.y}}, [&](ID2D1GeometrySink *sink) {
        sink->BeginFigure(pt(a), D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pt(control), pt(b)));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
    });
}

void Canvas::curveDotted(Point a, Point control, Point b, Color c, float width) {
    target_->DrawGeometry(curvePath(resources_, a, control, b), brush(c), width, resources_.dottedStroke());
}

void Canvas::line(Point a, Point b, Color c, float width, bool dashed) {
    target_->DrawLine(pt(a), pt(b), brush(c), width,
                      dashed ? resources_.dashedStroke() : resources_.roundStroke());
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
    // Knobs at rest draw the same arcs frame after frame: their paths are kept (see Resources::path()).
    ID2D1PathGeometry *path =
        resources_.path({0, {center.x, center.y, radius, from, to}}, [&](ID2D1GeometrySink *sink) {
            sink->BeginFigure(pt(onCircle(center, radius, from)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(pt(onCircle(center, radius, to)), D2D1::SizeF(radius, radius), 0.0f,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                          to - from > std::numbers::pi_v<float> ? D2D1_ARC_SIZE_LARGE
                                                                                : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        });
    target_->DrawGeometry(path, brush(c), width, resources_.roundStroke());
}

void Canvas::curve(Point a, Point control, Point b, Color c, float width, bool dashed) {
    target_->DrawGeometry(curvePath(resources_, a, control, b), brush(c), width,
                          dashed ? resources_.dashedStroke() : resources_.roundStroke());
}

void Canvas::polyline(const Point *points, size_t count, Color c, float width, bool dashed) {
    polylineStrokes(points, count, &c, &width, 1, dashed);
}

void Canvas::polylineStrokes(const Point *points, size_t count, const Color *colors, const float *widths,
                             size_t strokes, bool dashed) {
    if (count < 2 || strokes == 0)
        return;
    ComPtr<ID2D1PathGeometry> path;
    resources_.factory()->CreatePathGeometry(path.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    sink->BeginFigure(pt(points[0]), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < count; ++i)
        sink->AddLine(pt(points[i]));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ID2D1StrokeStyle *style = dashed ? resources_.dashedStroke() : resources_.roundStroke();
    for (size_t i = 0; i < strokes; ++i)
        target_->DrawGeometry(path.Get(), brush(colors[i]), widths[i], style);
}

void Canvas::fillPolygon(const Point *points, size_t count, Color c) {
    if (count < 3)
        return;
    ComPtr<ID2D1PathGeometry> path;
    resources_.factory()->CreatePathGeometry(path.GetAddressOf());
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(sink.GetAddressOf());
    sink->BeginFigure(pt(points[0]), D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < count; ++i)
        sink->AddLine(pt(points[i]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    target_->FillGeometry(path.Get(), brush(c));
}

namespace {
// Lifts the small label sizes toward the body size so they read more easily, keeping their order.
float liftedSize(float size) {
    constexpr float kLiftBelow = 11.5f;
    return size < kLiftBelow ? size + (kLiftBelow - size) * 0.4f : size;
}
// The text in UTF-16: in `buffer` when it fits, as all but very long text does (UTF-16 never takes more units than
// UTF-8 takes bytes), else in `longer`, rather than failing and leaving the text out.
std::wstring_view widen(std::string_view utf8, wchar_t (&buffer)[256], std::wstring &longer) {
    const int size = static_cast<int>(utf8.size());
    if (utf8.size() <= std::size(buffer)) {
        const int length =
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, buffer, static_cast<int>(std::size(buffer)));
        return {buffer, static_cast<size_t>(std::max(length, 0))};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    if (length <= 0)
        return {};
    longer.resize(static_cast<size_t>(length));
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, longer.data(), length);
    return longer;
}
} // namespace

void Canvas::text(Rect r, std::string_view utf8, float size, Color c, Align align, bool bold) {
    wchar_t buffer[256];
    std::wstring longer;
    const std::wstring_view wide = widen(utf8, buffer, longer);
    if (wide.empty())
        return;
    target_->DrawText(wide.data(), static_cast<UINT32>(wide.size()),
                      resources_.textFormat(liftedSize(size), bold, align), rc(r), brush(c),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// A text is measured once in each format (textFormat() tells them apart by a tenth of their size, and boldness): the
// width found is kept with the resources, a few thousand of them at most, as the browser's canvas2d.js keeps them.
float Canvas::textWidth(std::string_view utf8, float size, bool bold) {
    if (utf8.empty())
        return 0.0f;
    const float lifted = liftedSize(size);
    const int format = static_cast<int>(lifted * 10.0f) * 2 + (bold ? 1 : 0);
    std::string key(sizeof(format) + utf8.size(), '\0');
    std::memcpy(key.data(), &format, sizeof(format));
    std::memcpy(key.data() + sizeof(format), utf8.data(), utf8.size());
    std::unordered_map<std::string, float> &widths = resources_.textWidths();
    if (const auto known = widths.find(key); known != widths.end())
        return known->second;
    wchar_t buffer[256];
    std::wstring longer;
    const std::wstring_view wide = widen(utf8, buffer, longer);
    if (wide.empty())
        return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(resources_.writeFactory()->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()),
                                                           resources_.textFormat(lifted, bold, Align::Left), 1.0e4f,
                                                           100.0f, layout.GetAddressOf())))
        return 0.0f;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    if (widths.size() >= 4096)
        widths.clear();
    widths.emplace(std::move(key), metrics.widthIncludingTrailingWhitespace);
    return metrics.widthIncludingTrailingWhitespace;
}

} // namespace fmvibe::gui
