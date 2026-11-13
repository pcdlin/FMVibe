// Renders the editor view offscreen into PNGs, replaying a few mouse
// interactions on the way, so the look and the drag logic can be checked
// without a host. Usage: fmvibe-uishot <output-dir> [scale]

#include "factory.h"
#include "gui/arrange.h"
#include "gui/canvas.h"
#include "gui/layout.h"
#include "gui/routing.h"
#include "gui/theme.h"
#include "gui/view.h"
#include "uistrings.h"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <vector>

using namespace fmvibe;
using namespace fmvibe::gui;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

class FakeController : public Controller {
public:
    FakeController() {
        for (int i = 0; i < kParamCount; ++i)
            values[i] = paramTable()[i].def;
    }

    double value(int index) const override { return values[index]; }
    void telemetry(Telemetry &out) const override { out = live; }
    UiLayout &layout() override { return ui; }
    void layoutChanged() override {}
    void playNote(int note, bool on) override {
        if (on)
            lastNote = note;
    }
    int lastNote = -1; // the last key played
    void setScope(int op, int cycles) override {
        scopeOp = op;
        scopeCycles = cycles;
    }
    void setIdleLfosShown(bool shown) override { idleLfosShown = shown; }
    bool idleLfosShown = false; // what the view said last (see Controller::setIdleLfosShown())
    void resetPreset() override { ++resets; }
    bool loadPreset(const std::wstring &) override { return false; }
    // As the plugin does: a preset saved is the patch's preset, unchanged. Unless writing is to fail.
    bool savePreset(const std::wstring &path) override {
        ++writes;
        written = path;
        if (writeFails)
            return false;
        setPreset(path);
        return true;
    }
    void choosePreset(bool save) override { ++(save ? saves : loads); }
    int resets = 0, loads = 0, saves = 0, writes = 0;
    std::wstring written; // the file savePreset() was given last
    bool writeFails = false;
    // A small library: one preset at the top, two in a subfolder.
    void rescanPresets() override {}
    const std::vector<PresetEntry> &presets() const override { return library; }
    // As the plugin does: a preset's file is its name here, and loading one sets every param (each preset with its
    // own volume), lands on the preset, unchanged, and starts the undo history over.
    int presetIndex() const override {
        for (size_t i = 0; i < library.size(); ++i)
            if (presetFile() == std::wstring(library[i].name.begin(), library[i].name.end()))
                return static_cast<int>(i);
        return -1;
    }
    std::string presetName() const override {
        return presetIndex() < 0 ? tr(Str::PresetInit) : library[presetIndex()].name;
    }
    using Controller::presetFile;
    using Controller::setPreset;
    bool loadPresetAt(int index) override {
        values[paramIndex(GlobalParam::Volume)] = -20.0 + 2.0 * index;
        setPreset(std::wstring(library[index].name.begin(), library[index].name.end()));
        clearHistory();
        return true;
    }
    std::wstring presetFolder() const override { return L"C:\\Users\\Someone\\Documents\\FMVibe\\presets"; }
    void openPresetFolder() override { ++folderOpens; }
    std::vector<PresetEntry> library{{"Init Bass", ""}, {"Bell", "Factory"}, {"Pad", "Factory"}};
    int folderOpens = 0;
    int oversampling() const override { return oversamplingFactor; }
    void setOversampling(int factor) override { oversamplingFactor = factor; }
    bool mpe() const override { return mpeOn; }
    void setMpe(bool on) override { mpeOn = on; }
    int mpeBendRange() const override { return mpeRange; }
    void setMpeBendRange(int semitones) override { mpeRange = semitones; }
    std::string theme() const override { return themeName; }
    void setTheme(const std::string &name) override { themeName = name; }
    double accentHue() const override { return hue; }
    void setAccentHue(double h) override { hue = h; }
    double glow() const override { return glowLevel; }
    void setGlow(double amount) override { glowLevel = amount; }
    int handleSize() const override { return handles; }
    void setHandleSize(int size) override { handles = size; }
    double linkIntensity() const override { return links; }
    void setLinkIntensity(double amount) override { links = amount; }
    bool hasTempo() const override { return offersTempo; }
    double tempo() const override { return bpm; }
    void setTempo(double v) override {
        bpm = v;
        ++tempoSets;
    }

protected:
    void performBeginEdit(int index) override {
        ++openEdits;
        begun.push_back(index);
    }
    void performSetValue(int index, double v) override {
        values[index] = v;
        sets.push_back(index);
    }
    void performEndEdit(int) override { --openEdits; }

public:

    void set(int index, double v) { values[index] = v; }
    std::vector<int> begun; // the params whose gestures began, in order

    ParamValues values{};
    int oversamplingFactor = 2;
    bool mpeOn = false;
    int mpeRange = 48;
    int handles = 0;
    double links = 0.75;
    bool offersTempo = false; // a host that lets the editor set its tempo (the browser demo, fmvibe-uihost)
    double bpm = kDefaultTempo;
    int tempoSets = 0;
    // Midnight, the theme the pixel checks were written in (an editor given no theme, or one it doesn't know, shows
    // the default, Spectrum).
    std::string themeName = "Midnight";
    double glowLevel = 0.0;
    double hue = -1.0;
    Telemetry live{};
    int scopeOp = -1; // what the view last asked the scope to capture
    int scopeCycles = 0;
    UiLayout ui{};
    int openEdits = 0;
    std::vector<int> sets; // the params setValue() was given, in order
};

// Whether the `count` params from `copy` on hold what those from `original` on do.
bool sameFields(const FakeController &model, int copy, int original, int count) {
    for (int f = 0; f < count; ++f)
        if (model.value(copy + f) != model.value(original + f))
            return false;
    return true;
}

// Whether the last of the params from `first` (`count` of them) that the editor set was `last`.
bool setLast(const FakeController &model, int first, int count, int last) {
    for (auto it = model.sets.rbegin(); it != model.sets.rend(); ++it)
        if (*it >= first && *it < first + count)
            return *it == last;
    return false;
}

#ifdef _WIN32
class Renderer {
public:
    explicit Renderer(float scale) : scale_(scale) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    }

    // The whole editor, or only the part `crop` covers (in the view's coordinates).
    bool save(View &view, const std::filesystem::path &path, const Rect *crop = nullptr) {
        const UINT width = static_cast<UINT>(View::kWidth * scale_);
        const UINT height = static_cast<UINT>(view.height() * scale_);
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                      bitmap.GetAddressOf())))
            return false;

        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        ComPtr<ID2D1RenderTarget> target;
        if (FAILED(resources_.factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf())))
            return false;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
        {
            Canvas canvas(target.Get(), resources_);
            view.paint(canvas);
        }
        if (FAILED(target->EndDraw()))
            return false;
        last_ = bitmap;

        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        WICRect part{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
        if (crop)
            part = {static_cast<INT>(crop->x * scale_), static_cast<INT>(crop->y * scale_),
                    static_cast<INT>(crop->w * scale_), static_cast<INT>(crop->h * scale_)};
        return SUCCEEDED(wic_->CreateStream(stream.GetAddressOf())) &&
               SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) &&
               SUCCEEDED(frame->Initialize(nullptr)) &&
               SUCCEEDED(frame->SetSize(static_cast<UINT>(part.Width), static_cast<UINT>(part.Height))) &&
               SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), &part)) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    // Paints only `area` of the editor over what was saved last (see View::paint()): into a new bitmap that starts
    // as a copy of it, as a window's target keeps its frame (a WIC target needn't keep its bitmap's pixels).
    bool repaint(View &view, Rect area) {
        UINT width = 0, height = 0;
        ComPtr<IWICBitmap> bitmap;
        if (!last_ || FAILED(last_->GetSize(&width, &height)) ||
            FAILED(wic_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                      bitmap.GetAddressOf())))
            return false;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        ComPtr<ID2D1RenderTarget> target;
        ComPtr<ID2D1Bitmap> before;
        if (FAILED(resources_.factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf())) ||
            FAILED(target->CreateBitmapFromWicBitmap(last_.Get(), before.GetAddressOf())))
            return false;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0, 0, 0, 0));
        target->DrawBitmap(before.Get(), nullptr, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
        {
            Canvas canvas(target.Get(), resources_);
            view.paint(canvas, area);
        }
        if (FAILED(target->EndDraw()))
            return false;
        last_ = bitmap;
        return true;
    }

    // Every pixel of what was saved (or repainted) last, as 0xRRGGBB, row by row.
    std::vector<uint32_t> image() const {
        std::vector<uint32_t> out;
        UINT width = 0, height = 0;
        if (!last_ || FAILED(last_->GetSize(&width, &height)))
            return out;
        const WICRect rect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
        ComPtr<IWICBitmapLock> lock;
        UINT size = 0, stride = 0;
        BYTE *data = nullptr;
        if (FAILED(last_->Lock(&rect, WICBitmapLockRead, lock.GetAddressOf())) || FAILED(lock->GetStride(&stride)) ||
            FAILED(lock->GetDataPointer(&size, &data)))
            return out;
        out.reserve(static_cast<size_t>(width) * height);
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x) {
                const BYTE *px = data + y * stride + 4 * x;
                out.push_back(static_cast<uint32_t>(px[2]) << 16 | static_cast<uint32_t>(px[1]) << 8 | px[0]);
            }
        return out;
    }

    // The color at `p` (in the view's coordinates) in what was saved last, as 0xRRGGBB.
    uint32_t pixel(Point p) const {
        const WICRect rect{static_cast<INT>(p.x * scale_), static_cast<INT>(p.y * scale_), 1, 1};
        ComPtr<IWICBitmapLock> lock;
        UINT size = 0;
        BYTE *data = nullptr;
        if (!last_ || FAILED(last_->Lock(&rect, WICBitmapLockRead, lock.GetAddressOf())) ||
            FAILED(lock->GetDataPointer(&size, &data)) || size < 4)
            return 0;
        return static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[1]) << 8 | data[0];
    }

private:
    float scale_;
    Resources resources_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IWICBitmap> last_;
};
#else
// The Linux development build's: the same over a Cairo image surface (src/gui/canvas_cairo.cpp).
class Renderer {
public:
    explicit Renderer(float scale) : scale_(scale) {}
    ~Renderer() {
        if (last_)
            cairo_surface_destroy(last_);
    }
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // The whole editor, or only the part `crop` covers (in the view's coordinates).
    bool save(View &view, const std::filesystem::path &path, const Rect *crop = nullptr) {
        const int width = static_cast<int>(View::kWidth * scale_);
        const int height = static_cast<int>(view.height() * scale_);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(surface);
            return false;
        }
        cairo_t *cr = cairo_create(surface);
        cairo_scale(cr, scale_, scale_);
        {
            Canvas canvas(cr, resources_);
            view.paint(canvas);
        }
        cairo_destroy(cr);
        cairo_surface_flush(surface);
        if (last_)
            cairo_surface_destroy(last_);
        last_ = surface;
        if (!crop)
            return cairo_surface_write_to_png(surface, path.c_str()) == CAIRO_STATUS_SUCCESS;
        cairo_surface_t *part = cairo_surface_create_for_rectangle(surface, crop->x * scale_, crop->y * scale_,
                                                                   crop->w * scale_, crop->h * scale_);
        cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(crop->w * scale_),
                                                            static_cast<int>(crop->h * scale_));
        cairo_t *copy = cairo_create(image);
        cairo_set_source_surface(copy, part, 0.0, 0.0);
        cairo_paint(copy);
        cairo_destroy(copy);
        const bool written = cairo_surface_write_to_png(image, path.c_str()) == CAIRO_STATUS_SUCCESS;
        cairo_surface_destroy(image);
        cairo_surface_destroy(part);
        return written;
    }

    // Paints only `area` of the editor over what was saved last (see View::paint()).
    bool repaint(View &view, Rect area) {
        if (!last_)
            return false;
        cairo_t *cr = cairo_create(last_);
        cairo_scale(cr, scale_, scale_);
        {
            Canvas canvas(cr, resources_);
            view.paint(canvas, area);
        }
        cairo_destroy(cr);
        cairo_surface_flush(last_);
        return true;
    }

    // Every pixel of what was saved (or repainted) last, as 0xRRGGBB, row by row.
    std::vector<uint32_t> image() const {
        std::vector<uint32_t> out;
        if (!last_)
            return out;
        const int width = cairo_image_surface_get_width(last_), height = cairo_image_surface_get_height(last_);
        const int stride = cairo_image_surface_get_stride(last_);
        const unsigned char *data = cairo_image_surface_get_data(last_);
        out.reserve(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                uint32_t argb;
                std::memcpy(&argb, data + y * stride + 4 * x, sizeof argb);
                out.push_back(argb & 0xFFFFFF);
            }
        return out;
    }

    // The color at `p` (in the view's coordinates) in what was saved last, as 0xRRGGBB.
    uint32_t pixel(Point p) const {
        if (!last_)
            return 0;
        const int x = static_cast<int>(p.x * scale_), y = static_cast<int>(p.y * scale_);
        if (x < 0 || y < 0 || x >= cairo_image_surface_get_width(last_) || y >= cairo_image_surface_get_height(last_))
            return 0;
        const unsigned char *row = cairo_image_surface_get_data(last_) + y * cairo_image_surface_get_stride(last_);
        uint32_t argb;
        std::memcpy(&argb, row + 4 * x, sizeof argb);
        return argb & 0xFFFFFF;
    }

private:
    float scale_;
    Resources resources_;
    cairo_surface_t *last_ = nullptr;
};
#endif

// Screen positions of a few controls in the default layout (mirrors src/gui/layout.h). The inspector's are given as
// they were with it at the panels' right, its top left at (884, 132), and moved to where its lens now floats.
constexpr float kInsDx = detail::kInspector.x - 884.0f, kInsDy = detail::kInspector.y - 132.0f;
constexpr Point ins(float x, float y) {
    return {x + kInsDx, y + kInsDy};
}
constexpr Point kOpPhaseKnob = ins(1101.0f, 242.0f); // an operator's Phase, a plain knob
constexpr Point kMultKnob = ins(967.0f, 242.0f);     // a fixed operator's Mult, in the Ratio readout's right half
constexpr Point kRatioTens = ins(917.0f, 242.0f), kRatioOnes = ins(933.0f, 242.0f), kRatioTenths = ins(956.0f, 242.0f),
                kRatioHundredths = ins(972.0f, 242.0f); // the Ratio readout's digits
constexpr Point kRatioDot = ins(944.7f, 257.0f); // the first depth handle along the Ratio readout's bottom edge
constexpr Point kPanKnob = ins(1146.0f, 242.0f); // an operator's Pan, bipolar
constexpr Point kOutCore = detail::kOutCore; // the routing graph's OUT
// What covers the routing panel alone (the note curve's and the partial editor) moved with it, right of the lens.
constexpr float kRoutingDx = detail::kRouting.x - 16.0f;
constexpr Point kPitchKnob = ins(1012.0f, 242.0f);
constexpr Point kUnisonKnob = ins(934.0f, 338.0f); // on the UNISON tab
constexpr Point kPhaseKnob = ins(1134.0f, 338.0f);
constexpr Point kSlopeKnob = ins(922.0f, 242.0f);
constexpr Point kDecayKnob = ins(1034.0f, 454.0f);
// An operator's envelope display, on its ENVELOPE tab (kOpEnvRect): its top and bottom.
constexpr float kOpEnvTop = 316.0f + kInsDy;
constexpr float kOpEnvBottom = 426.0f + kInsDy;
// The global strip, under the title bar: from the meter's left edge (kHeaderStart) on, the output scope left of the
// meter (kOutScope).
constexpr float kHeaderStart = 128.0f;
constexpr Point kFmAmountKnob{307.0f, 64.0f};
constexpr Point kFmAmountNumber{349.0f, 67.0f}; // beside its knob
constexpr Point kVoicesKnob{1058.0f, 64.0f}; // past the second divider, beside the count
constexpr Point kMonoButton{803.0f, 53.0f};
constexpr Point kLegatoToggle{781.0f, 76.0f};
constexpr Point kGlideKnob{859.0f, 64.0f};
constexpr Point kAlwaysToggle{951.0f, 53.0f};
constexpr float kOutScopeX = kHeaderStart - 104.0f, kOutScopeY = 45.0f, kOutScopeW = 96.0f, kOutScopeH = 38.0f;
// The view tabs sit above the panels.
constexpr Point kRoutingTab{64.0f, 116.0f};
constexpr Point kMatrixTab{160.0f, 116.0f};
constexpr Point kFxTab{240.0f, 116.0f};
constexpr Point kF1Tab = ins(1068.0f, 152.0f);
// Init, Load, Save and the settings cog at the right end of the title bar.
constexpr Point kInitButton{1034.0f, 18.0f};
constexpr Point kLoadButton{1068.0f, 18.0f};
constexpr Point kSaveButton{1102.0f, 18.0f};
constexpr Point kSaveAsButton{1136.0f, 18.0f};
constexpr Point kSettingsButton{1170.0f, 18.0f};
constexpr Point kOpFTab = ins(1037.0f, 152.0f);
constexpr Point kSourcesToggle{60.0f, 541.0f};

// The modulation panel: chips and add buttons in its title row, then four cards, 277 wide (and after them the
// dashed cards that add sources, 93 wide), at screen places 0..3, fractions while scrolled.
constexpr float kCardW = 277.0f;
constexpr float kSourceShadowSpan = (93.0f + 12.0f) / (kCardW + 12.0f);
float cardX(float card) {
    return 28.0f + card * (kCardW + 12.0f);
}


Point sourceGrip(float card) {
    return {cardX(card) + 90.0f, 572.0f}; // over its name
}

Point sourceHandle(float card) {
    return {cardX(card) + 24.0f, 582.0f};
}

// An LFO's sync switch, a square left of its Rate knob.
Point sourceSync(float card) {
    return {cardX(card) + 155.0f, 620.0f};
}

Point sourcePower(float card) {
    return detail::sourcePower(card).center();
}

Point sourceRemove(float card) {
    return detail::sourceRemove(card).center();
}

// An LFO card's shape list: its button, and a shape in the list it opens.
Point shapeButton(float card) {
    return {cardX(card) + 80.0f, 617.0f};
}

// The list offers every shape but the retired Random (detail::kLfoShapesOffered), in its order.
Point shapeItem(float card, LfoShape shape) {
    const auto *offered = std::begin(detail::kLfoShapesOffered);
    const auto row = std::find(offered, std::end(detail::kLfoShapesOffered), shape) - offered;
    return {shapeButton(card).x, 647.0f + static_cast<float>(row) * 22.0f};
}

// An LFO card's clock modes under its shape list, and its Unipolar switch under the sync switch.
Point retriggerToggle(float card) {
    return {cardX(card) + 31.25f, 644.0f};
}

Point freeToggle(float card) {
    return {cardX(card) + 61.75f, 644.0f};
}

Point anchorToggle(float card) {
    return {cardX(card) + 92.25f, 644.0f};
}

Point oneShotToggle(float card) {
    return {cardX(card) + 122.75f, 644.0f};
}

Point unipolarToggle(float card) {
    return {cardX(card) + 155.0f, 644.0f};
}

// Its Rate and Phase knobs, over a random LFO's Smooth and Chaos.
Point rateKnob(float card) {
    return {cardX(card) + 186.0f, 620.0f};
}

Point phaseKnob(float card) {
    return {cardX(card) + 236.0f, 620.0f};
}

// The places holding a source, in the order they show, and what the editor calls one.
std::vector<int> shown(FakeController &model) {
    return shownSources(model.layout(), [&](int index) { return model.value(index); });
}

std::string nameOf(FakeController &model, int source) {
    return sourceName(model.layout(), [&](int index) { return model.value(index); }, source);
}

// The slots holding an effect, in the line's order as the effects graph's wires make it: the main line's, and each
// split's high band's then its low band's (as the editor counts them), then any the wires leave out, by number.
std::vector<int> fxChain(const FakeController &model) {
    auto inUse = [&](int node) {
        if (node < kFxSlots)
            return std::lround(model.value(paramIndex(node, FxParam::Type))) != 0;
        if (fxNodeSum(node) >= 0)
            return model.value(paramIndex(fxNodeSum(node), SumParam::Used)) >= 0.5;
        return node != kFxOutNode && model.value(paramIndex(node - kFxSlots, SplitParam::Used)) >= 0.5;
    };
    auto into = [&](int port) { // the node in use, other than the output, that takes it
        for (int node = 0; node < kFxNodes; ++node)
            for (int input = 0; input < fxInputCount(node) && node != kFxOutNode && inUse(node); ++input)
                if (fxPortOf(model.value(fxInputIndex(node, input, FxInputField::From))) == port)
                    return node;
        return -1;
    };
    std::vector<int> chain;
    auto taken = [&](int slot) { return std::find(chain.begin(), chain.end(), slot) != chain.end(); };
    for (int port = kFxPortIn, node = into(port); node >= 0; node = into(port)) {
        if (node < kFxSlots && !taken(node)) {
            chain.push_back(node);
            port = fxSlotPort(node);
        } else if (fxNodeSplit(node)) {
            const int split = node - kFxSlots;
            for (int band : {kFxBands - 1, 0})
                for (int p = fxBandPort(split, band), n = into(p); n >= 0 && n < kFxSlots && !taken(n); n = into(p)) {
                    chain.push_back(n);
                    p = fxSlotPort(n);
                }
            port = fxSumPort(split);
        } else {
            break;
        }
    }
    for (int slot = 0; slot < kFxSlots; ++slot)
        if (inUse(slot) && !taken(slot))
            chain.push_back(slot);
    return chain;
}

// The effects in `slots` one after another from the effects graph's input to its output: a chain, as the editor
// wires one.
void setChain(FakeController &model, std::initializer_list<int> slots) {
    for (const auto &[index, value] : fxChainWires(slots))
        model.values[index] = value;
}

// FX cards, by position in the chain: 22 apart and about 169 wide (spread over the panel's 1122 from x 46, a gap in
// for the arrow before the first), the parametric EQ's one and a half card places wide, a folded card a strip 38
// wide: the icon's place, 18 wide and 10 in from the card's left edge, and as much on its right.
constexpr float kFxCardW = (1122.0f - 5.0f * 22.0f) / 6.0f;
constexpr float kFxStep = kFxCardW + 22.0f;
constexpr float kFoldedSpan = (38.0f + kFxStep - kFxCardW) / kFxStep;
float fxSpan(const FakeController &model, int slot) {
    const bool folded = slot < kFirstFxSlots ? (model.ui.fxFolded >> slot) & 1u
                                             : (model.ui.fxFoldedLater >> (slot - kFirstFxSlots)) & 1u;
    if (folded)
        return kFoldedSpan;
    const long type = std::lround(model.value(paramIndex(slot, FxParam::Type)));
    return type == static_cast<long>(FxType::Amp)                                                        ? 1.85f
           : type == static_cast<long>(FxType::ParamEq) || type == static_cast<long>(FxType::MultiDyn) ? 1.5f
                                                                                                         : 1.0f;
}
// How far the test has left the chain scrolled, in card places: cards sit that much left of their places.
float fxScrolled = 0.0f;
float fxCardX(const FakeController &model, int position) {
    const std::vector<int> chain = fxChain(model);
    float place = 0.0f;
    for (int i = 0; i < position && i < static_cast<int>(chain.size()); ++i)
        place += fxSpan(model, chain[i]);
    return 46.0f + (place - fxScrolled) * kFxStep;
}
float fxCardW(const FakeController &model, int position) {
    const std::vector<int> chain = fxChain(model);
    const float span = position < static_cast<int>(chain.size()) ? fxSpan(model, chain[position]) : 1.0f;
    return span * kFxStep - (kFxStep - kFxCardW);
}

Point fxGrip(const FakeController &model, int position) {
    return {fxCardX(model, position) + 40.0f, 197.0f};
}

// The effect's icon at its title's left, which switches it, on a folded card too.
Point fxPower(const FakeController &model, int position) {
    return {fxCardX(model, position) + 19.0f, 197.0f};
}

Point fxRemove(const FakeController &model, int position) {
    return {fxCardX(model, position) + fxCardW(model, position) - 15.0f, 198.0f};
}

// Knob `index` of `count` across a card, in the row at `y` within it.
Point fxKnob(const FakeController &model, int position, int index, int count, float y = 240.0f) {
    return {fxCardX(model, position) + fxCardW(model, position) / count * (index + 0.5f), 180.0f + y};
}

// ADD EFFECT at the FX panel's top left, and the middle of the entry for `type` in the list it opens (its groups in
// two columns, as src/gui/layout.h places them).
constexpr Point kFxMenuButton{90.0f, 152.0f};
Point rectCenter(Rect r) {
    return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
}
Point fxMenuItem(FxType type) {
    return rectCenter(detail::fxMenuItem({-1.0f, -1.0f}, type));
}

// A list a dashed card opens: its top left where the card was clicked, moved back inside the editor (4 px in) where
// it would reach past an edge. The effects' is as large as src/gui/layout.h makes it (taller from the main line, with
// a split's entries); the sources' 150 px wide, a type every 26 px.
Point listAt(const View &view, Point click, float w, float h) {
    return {std::clamp(click.x, 4.0f, View::kWidth - 4.0f - w), std::clamp(click.y, 4.0f, view.height() - 4.0f - h)};
}
Point fxListAt(const View &view, Point click, bool node = false) {
    const Rect menu = detail::fxMenu({0.0f, 0.0f}, node);
    return listAt(view, click, menu.w, menu.h);
}
Point fxListItem(const View &view, Point click, FxType type, bool node = false) {
    return rectCenter(detail::fxMenuItem(fxListAt(view, click, node), type));
}
Point fxListSplit(const View &view, Point click, int bands = 2) {
    return rectCenter(detail::fxMenuNodeItem(fxListAt(view, click, true), bands));
}
// Whether a list of effects at `at` (x < 0: under + EFFECT) shows group `group`'s name on its strip, darker than the
// list just over it, read at the strip's right end, clear of the name, in what was saved last.
bool fxGroupStrip(const Renderer &renderer, Point at, int group) {
    const Rect head = detail::fxMenuHead(at, group);
    auto level = [&](Point p) {
        const uint32_t color = renderer.pixel(p);
        return static_cast<int>(color >> 16 & 255) + static_cast<int>(color >> 8 & 255) + static_cast<int>(color & 255);
    };
    return level({head.right() - 6.0f, head.y + 9.0f}) + 15 < level({head.right() - 6.0f, head.y - 2.0f});
}
// Where a list opened from the effects graph's + (or a wire's) goes: beside it, up a little.
Point besidePlus(Point plus) {
    return {plus.x + 26.0f, plus.y - 44.0f};
}
// The list of source types opened at `click`: an entry 26 px a type, in its order (detail::kSourceMenuOrder).
Point sourceListItem(const View &view, Point click, SourceType type) {
    const Point at = listAt(view, click, 150.0f, 8.0f + detail::kSourceTypes * 26.0f);
    const auto *order = std::begin(detail::kSourceMenuOrder);
    const auto row = std::find(order, std::end(detail::kSourceMenuOrder), type) - order;
    return {at.x + 70.0f, at.y + 16.0f + 26.0f * static_cast<float>(row)};
}


// The freeform shape editor: x 0..1 across, y -1..1 up. An LFO's spans the routing panel and the inspector, its
// plot 1088 wide; the note source's curve, like the partial editor, fills the routing panel only, 772 wide.
Point shapeAt(double x, double y, float width = 1088.0f) {
    return {56.0f + static_cast<float>(x) * width, 330.0f - static_cast<float>(y) * 126.0f};
}
Point noteCurveAt(double x, double y) { // over the routing panel, right of the lens
    return {detail::kRouting.x + 40.0f + static_cast<float>(x) * 772.0f, 330.0f - static_cast<float>(y) * 126.0f};
}
constexpr Point kShapeDone{1154.0f, 152.0f};
constexpr Point kRoutingEditorDone{detail::kRouting.x + detail::kRouting.w - 30.0f, 152.0f}; // the note curve's and the partial editor's
// A shaper's big curve editor is a 520 px panel in the middle of the effects' panel, its plot 440 wide.
Point clipAt(double x, double y) {
    return {380.0f + static_cast<float>(x) * 440.0f, 330.0f - static_cast<float>(y) * 126.0f};
}
constexpr Point kClipDone{830.0f, 152.0f};

// The common waves' buttons (Sine, Triangle, Saw, Square, Noise), then "..." with the others.
Point waveButton(int button) {
    constexpr float kStep = (300.0f - 32.0f + 4.0f) / 6.0f;
    return ins(900.0f + button * kStep + (kStep - 4.0f) * 0.5f, 205.0f);
}
const Point kNoiseButton = waveButton(4);
// An operator page's tabs, ENVELOPE, UNISON, INPUTS and SCOPE (by InspectorTab), and the scope's buttons for 1, 2
// and 4 cycles in its display's top right corner.
Point opTabPoint(InspectorTab tab) {
    return detail::opTab(static_cast<int>(tab)).center();
}
Point scopeCycleButton(int choice) {
    return ins(1112.0f + choice * 20.0f, 330.0f);
}
const Point kMoreWavesButton = waveButton(5);

Point typeButton(int type) {
    return detail::filterTypeButton(type).center();
}

// The slope's list button under the filter's response, and its entries, 6 to 24 dB, which open under it.
const Point kSlopeButton = detail::kFilterSlopeButton.center();
Point slopeItem(int slope) {
    return detail::listMenuItem(detail::kFilterSlopeButton, kFilterSlopes, slope).center();
}

// Every node has its row and column, A..F, F1 and F2, then S1 and S2; the output is the last column.
Point gridCellCenter(int row, int col) {
    return {detail::kGridOrigin.x + col * 32.6f + 14.3f, 168.0f + row * 32.6f + 14.3f};
}

Point nodeCenter(const UiLayout &layout, int node) {
    const Rect g = detail::kGraph;
    if (node >= kOps)
        return {g.x + layout.boxX[node - kOps] * g.w, g.y + layout.boxY[node - kOps] * g.h};
    return {g.x + layout.nodeX[node] * g.w, g.y + layout.nodeY[node] * g.h};
}

// A node's label on its line to OUT: halfway between its outline and the arrowhead on OUT's edge, or left of the
// line where the node sits too close to OUT for it (see outWireOf()). The arrowhead's middle.
Point outLabelOf(const UiLayout &layout, int node) {
    return detail::outWireOf(node, nodeCenter(layout, node)).label;
}
Point outArrowOf(const UiLayout &layout, int node) {
    const detail::OutWire w = detail::outWireOf(node, nodeCenter(layout, node));
    return {w.end.x + w.dir.x * 4.0f, w.end.y + w.dir.y * 4.0f};
}

// The middle of a wire's arrowhead, from operator center `a` to operator center `b`, bowed as in view.cpp: the head
// starts 8 px off the circle, toward the bow's control point, and points along the curve's end.
Point arrowOf(Point a, Point b) {
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    const Point u{(b.x - a.x) / length, (b.y - a.y) / length};
    const Point control{(a.x + b.x) * 0.5f - u.y * length * 0.18f, (a.y + b.y) * 0.5f + u.x * length * 0.18f};
    const float toControl = std::hypot(control.x - b.x, control.y - b.y);
    const Point end{b.x + (control.x - b.x) / toControl * 32.0f, b.y + (control.y - b.y) / toControl * 32.0f};
    const float along = std::hypot(end.x - control.x, end.y - control.y);
    return {end.x + (end.x - control.x) / along * 4.0f, end.y + (end.y - control.y) / along * 4.0f};
}

// The inspector's tabs: A..F, then a little gap and F1 and F2, another and S1 and S2.
Point nodeTab(int node) {
    return ins(912.0f + node * 25.0f + (node >= kOps ? 6.0f : 0.0f) + (node >= kOps + kFilters ? 6.0f : 0.0f), 152.0f);
}

// A shaper's page: its curve's list, the symmetric and link switches, the Drive, Output and Mix knobs (centered, as far
// apart as four would be), the list's entries, and its drawn curve in the display (x 0..1 across, y -1..1 up), with its
// edit button in the display's corner.
constexpr Point kShaperList = ins(1009.0f, 205.0f);
constexpr Point kShaperSym = ins(1096.0f, 205.0f);
constexpr Point kShaperLink = ins(1146.0f, 205.0f);
Point shaperKnob(int knob) {
    return ins(884.0f + 150.0f + 268.0f / 4.0f * (knob - 1.0f), 242.0f); // Drive, Output, Mix
}
Point shaperListItem(ClipMode mode) {
    return ins(1009.0f, 220.0f + 4.0f + static_cast<int>(mode) * 22.0f + 11.0f);
}
Point shaperCurveAt(double x, double y) {
    return ins(908.0f + static_cast<float>(x) * 252.0f, 338.0f - static_cast<float>(y) * 42.24f);
}
constexpr Point kShaperEdit = ins(1150.0f, 381.0f);

// A switched-off node's ghost, docked under OUT in the graph's bottom right, the ghosts in the nodes' order (mirrors
// the widgets in view.cpp).
Point ghostOf(const FakeController &model, int node) {
    auto off = [&](int n) { return model.value(detail::enabledParam(n)) < 0.5; };
    auto width = [](int n) { return n >= kOps ? 34.0f : 24.0f; };
    float x = detail::kRouting.right() - 16.0f + 6.0f;
    for (int n = 0; n < kNodes; ++n)
        if (off(n))
            x -= width(n) + 6.0f;
    for (int n = 0; n < node; ++n)
        if (off(n))
            x += width(n) + 6.0f;
    return {x + 0.5f * width(node), detail::kRouting.bottom() - 22.0f};
}

// A point on a node's edge, where a drag connects it: an operator's lower right, a filter's or shaper's right side.
Point portOf(const UiLayout &layout, int node) {
    const Point c = nodeCenter(layout, node);
    if (node >= kOps)
        return {c.x + 33.0f, c.y};
    return {c.x + 17.3f, c.y + 17.3f};
}

// How bright a handle's ring is, in what was saved last: the brightest channel of any pixel down `span` px from `from`,
// a little right of the handle's middle. On the dark theme a faint handle is much darker.
int ringLevel(const Renderer &renderer, Point from, float span) {
    int most = 0;
    for (float y = from.y; y <= from.y + span; y += 1.0f) {
        const uint32_t color = renderer.pixel({from.x, y});
        most = std::max({most, static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                         static_cast<int>(color & 255)});
    }
    return most;
}

// The brightest pixel from `from` to `to`, in what was saved last; and whether a color is amber (the high band's),
// blue (the low band's) or grey, by how its red and blue differ.
uint32_t brightestAlong(const Renderer &renderer, Point from, Point to) {
    uint32_t best = 0;
    int most = -1;
    const int steps = std::max(1, static_cast<int>(std::hypot(to.x - from.x, to.y - from.y)));
    for (int k = 0; k <= steps; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        const uint32_t color = renderer.pixel({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
        const int level = std::max({static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                                    static_cast<int>(color & 255)});
        if (level > most) {
            most = level;
            best = color;
        }
    }
    return best;
}
int redOverBlue(uint32_t color) {
    return static_cast<int>(color >> 16 & 255) - static_cast<int>(color & 255);
}
bool amber(uint32_t color) {
    return redOverBlue(color) > 50;
}
bool blue(uint32_t color) {
    return redOverBlue(color) < -50;
}
bool grey(uint32_t color) {
    return std::abs(redOverBlue(color)) < 30 && (color >> 16 & 255) > 60;
}

// The color of the dot in the top right corner of a node's tab in the inspector, in what was saved last.
uint32_t tabDot(const Renderer &renderer, int node) {
    return renderer.pixel({nodeTab(node).x + 7.0f, nodeTab(node).y - 6.0f});
}

void click(View &view, Point p, Modifiers mods = {}) {
    view.mouseDown(p, mods);
    view.mouseUp(p, mods);
}

// + SOURCE, after the modulation panel's toggle, then a type in the list it opens under it.
Point sourceMenuEntry(SourceType type) {
    const Rect button = detail::sourceMenuButton();
    return detail::sourceMenuItem({button.x, button.bottom() + 4.0f}, type).center();
}
void addSource(View &view, SourceType type) {
    click(view, detail::sourceMenuButton().center());
    click(view, sourceMenuEntry(type));
}

// Picks `type` from the ADD EFFECT list.
void addFx(View &view, FxType type) {
    click(view, kFxMenuButton);
    click(view, fxMenuItem(type));
}

void drag(View &view, Point from, Point to) {
    view.mouseDown(from, {});
    view.mouseMove({(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f}, {});
    view.mouseMove(to, {});
    view.mouseUp(to, {});
}

// Undo and redo at the left of the toolbar.
constexpr Point kUndoButton{952.0f, 18.0f};
constexpr Point kRedoButton{986.0f, 18.0f};
// The LFO shape editor's reset button (the note curve's further left), and its grid's buttons: per axis (0 across)
// a step down (part 0), the count, a step up.
constexpr Point kShapeReset{1120.0f, 152.0f};
constexpr Point kNoteCurveReset{804.0f + kRoutingDx, 152.0f};
Point shapeGrid(int axis, int part) {
    constexpr float kParts[] = {18.0f, 28.0f, 18.0f};
    const float x = 1144.0f - (axis == 0 ? 148.0f : 64.0f) + (part > 0 ? 18.0f : 0.0f) + (part > 1 ? 28.0f : 0.0f);
    return {x + kParts[part] * 0.5f, 486.0f};
}
// A Custom LFO's display, a click on which widens its card to two card places. The widened card's title row buttons:
// back to a normal card (0), the shape's reset (1) and the full-size editor (2), left to right the reset, back to a
// normal card and the full-size editor; its shape tools, by
// View::ShapeTool, where a normal card's display is, and under them Points (0), Steps (1) and Snap (2); its plot,
// 264 x 102 from 281 px into the card, 44 under its top; and the grid's steppers under it, per axis (0 across) a
// step down (part 0), the count and a step up.
Point lfoDisplay(float card) {
    return {cardX(card) + 138.0f, 688.0f};
}
Point wideButton(float card, int button) {
    constexpr float kFromRight[] = {1.0f, 2.0f, 0.0f};
    return {cardX(card) + 2.0f * kCardW + 12.0f - 106.0f - kFromRight[button] * 34.0f, 582.0f};
}
Point lfoTool(float card, int tool) {
    return {cardX(card) + 16.0f + tool * 41.5f + 18.75f, 673.0f};
}
Point lfoMode(float card, int mode) {
    return {cardX(card) + 16.0f + (mode < 2 ? mode * 75.0f + 37.5f : 156.0f + 44.5f), 701.0f};
}
Point cardShapeAt(float card, double x, double y) {
    return {cardX(card) + 281.0f + static_cast<float>(x) * 264.0f, 659.0f - static_cast<float>(y) * 44.88f};
}
// A random LFO's Smooth (0) and Chaos (1) knobs, right of its narrowed display.
Point randomKnob(float card, int knob) {
    return {cardX(card) + 186.0f + knob * 50.0f, 677.0f};
}

void doubleClick(View &view, Point p) {
    click(view, p); // Windows sends a press, then the double-click in place of the second one
    view.doubleClick(p, {});
    view.mouseUp(p, {});
}

void type(View &view, const char *text) {
    for (const char *c = text; *c; ++c)
        view.typeChar(static_cast<char32_t>(*c));
}

// The newer parts of the editor, on a fresh patch: undo and redo, typing values, the random LFO's
// settings, the wave list, the shape editor's grid and reset, and scrolling while dragging a source.
void runMore(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const int opPhase = paramIndex(0, OpParam::Phase);

    // A knob drag is one step: undo takes it back, redo brings it again.
    drag(view, kOpPhaseKnob, {kOpPhaseKnob.x, kOpPhaseKnob.y - 40.0f});
    check(std::abs(model.value(opPhase) - 0.2) < 1e-6 && model.nextUndo() != nullptr, "a knob drag is an undo step");
    click(view, kUndoButton);
    const bool undone = model.value(opPhase) == 0.0;
    click(view, kRedoButton);
    check(undone && std::abs(model.value(opPhase) - 0.2) < 1e-6 && model.openEdits == 0,
          "the undo and redo buttons take a drag back and bring it again");
    // A run of wheel notches on one knob undoes at once.
    for (int n = 0; n < 3; ++n)
        view.wheel(kOpPhaseKnob, 1.0f, {});
    view.undo();
    check(std::abs(model.value(opPhase) - 0.2) < 1e-6, "a run of wheel notches is one undo step");
    // Moving a node is a step too.
    const Point a = nodeCenter(model.layout(), 0);
    const float wasX = model.layout().nodeX[0];
    drag(view, a, {a.x + 100.0f, a.y - 50.0f});
    const float movedX = model.layout().nodeX[0];
    view.undo();
    check(movedX != wasX && model.layout().nodeX[0] == wasX, "undo puts a dragged node back");
    view.redo();
    check(model.layout().nodeX[0] == movedX, "redo moves it again");
    view.undo();

    // Typing values: a double-click on a knob's number opens a field, Enter sets what it reads as.
    doubleClick(view, {kOpPhaseKnob.x, 266.0f});
    const bool typingOn = view.typing();
    type(view, "50");
    view.mouseMove({kOpPhaseKnob.x, 266.0f}, {});
    check(typingOn && renderer.save(view, dir / "16-typing.png"), "render a value being typed");
    view.typeKey(View::Key::Enter);
    check(!view.typing() && std::abs(model.value(opPhase) - 0.5) < 1e-9, "a typed value sets the knob");
    view.undo();
    check(std::abs(model.value(opPhase) - 0.2) < 1e-6, "a typed value is an undo step");
    doubleClick(view, kFmAmountNumber); // FM Amount's number, beside it in the header
    type(view, "150 %");
    view.typeKey(View::Key::Enter);
    const int fmAmount = paramIndex(GlobalParam::FmAmount);
    check(std::abs(model.value(fmAmount) - 1.5) < 1e-9, "a header knob takes a typed value too");
    doubleClick(view, kFmAmountNumber);
    type(view, "70");
    view.typeKey(View::Key::Escape);
    doubleClick(view, kFmAmountNumber);
    type(view, "lots");
    view.typeKey(View::Key::Enter);
    doubleClick(view, kFmAmountNumber);
    view.typeKey(View::Key::Enter); // as shown: no change, no step
    check(std::abs(model.value(fmAmount) - 1.5) < 1e-9, "Escape, text that isn't a value, or no change leave it");
    doubleClick(view, kFmAmountNumber);
    view.typeKey(View::Key::Backspace);
    type(view, "0");
    click(view, {600.0f, 328.0f}); // a click elsewhere takes the value
    check(std::abs(model.value(fmAmount)) < 1e-9 && !view.typing(), "a click elsewhere takes the value typed");
    doubleClick(view, kOpPhaseKnob);
    check(!view.typing() && model.value(opPhase) == 0.0, "a double-click on the knob itself still resets it");
    view.undo();
    // The right-click menu's "Edit value" types into the knob too.
    auto menuEntry = [&](Point at, const std::string &label) {
        for (const MenuItem &item : view.contextMenu(at))
            if (item.label == label)
                return item.command;
        return -1;
    };
    view.menuCommand(menuEntry(kOpPhaseKnob, tr(Str::MenuEditValue)));
    const bool menuTyping = view.typing();
    type(view, "35");
    view.typeKey(View::Key::Enter);
    check(menuTyping && std::abs(model.value(opPhase) - 0.35) < 1e-9, "a knob's menu has Edit value, which types into it");
    // Ctrl+Click types into a route: A's label on its line to OUT in the graph, then its OUT cell in the matrix.
    const Point aOut = outLabelOf(model.layout(), 0);
    view.mouseDown(aOut, {false, true});
    type(view, "50");
    view.mouseMove({600.0f, 328.0f}, {});
    check(view.typing() && renderer.save(view, dir / "16b-typing-route.png"), "render a route's value being typed");
    view.typeKey(View::Key::Enter);
    const int aOutParam = paramIndex(0, OpParam::Out);
    const bool outTyped = std::abs(model.value(aOutParam) - 0.5) < 1e-9;
    click(view, kMatrixTab);
    view.mouseDown(gridCellCenter(0, kNodes), {false, true});
    type(view, "25");
    view.typeKey(View::Key::Enter);
    click(view, kRoutingTab);
    check(outTyped && std::abs(model.value(aOutParam) - 0.25) < 1e-9,
          "Ctrl+Click types a route's amount, on its label or its matrix cell");

    // A Random source, a random LFO, shows Smooth and Chaos beside a preview of what they do.
    check(rateFromNorm(1.0) == 100.0 && sweepRateHz(0.0) == 0.0, "LFOs run from 0 Hz up to 100 Hz");
    addSource(view, SourceType::Lfo);
    // Undone with its shape list open, the LFO takes the list along: added again in its place, its button opens the
    // list afresh (rather than the click closing one left open), which picks a shape.
    click(view, shapeButton(0));
    view.undo();
    const bool lfoUndone = model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::None);
    addSource(view, SourceType::Lfo);
    click(view, shapeButton(0));
    const bool shapeListShot = renderer.save(view, dir / "17-lfo-shape-list.png");
    // The list reaches past the card's bottom, and shows whole there: its bottom padding, under Custom, is drawn
    // like its top's (read off the rendered pixels).
    const Point listTop{shapeButton(0).x, shapeItem(0, LfoShape::Sine).y - 13.0f};
    const Point listBottom{listTop.x, shapeItem(0, LfoShape::Square).y + 12.0f};
    check(renderer.pixel(listTop) == renderer.pixel(listBottom),
          "an LFO's shape list shows whole, past its card's bottom");
    click(view, shapeItem(0, LfoShape::Square));
    check(lfoUndone && shapeListShot &&
              std::lround(model.value(paramIndex(0, SourceParam::Shape))) == static_cast<long>(LfoShape::Square),
          "an LFO's shape list picks its shape, and goes with the LFO when that is undone");
    // The wheel over it steps along the shapes the list offers, which end at Square (the retired Random and Custom
    // left out).
    view.wheel(shapeButton(0), -1.0f, {});
    const bool wheeledDown =
        std::lround(model.value(paramIndex(0, SourceParam::Shape))) == static_cast<long>(LfoShape::Saw);
    view.wheel(shapeButton(0), 2.0f, {});
    check(wheeledDown && std::lround(model.value(paramIndex(0, SourceParam::Shape))) ==
                             static_cast<long>(LfoShape::Square),
          "the wheel over an LFO's shape list steps along the shapes it offers, Square the last");
    // The Random source, in the next card: synced on quarter notes as it starts, as an LFO is.
    addSource(view, SourceType::Random);
    const bool randomSynced = model.value(paramIndex(1, SourceParam::Sync)) == 1.0 &&
                              model.value(paramIndex(1, SourceParam::Bipolar)) == 1.0;
    drag(view, randomKnob(1, 0), {randomKnob(1, 0).x, randomKnob(1, 0).y - 120.0f});
    drag(view, randomKnob(1, 1), {randomKnob(1, 1).x, randomKnob(1, 1).y + 140.0f});
    check(randomSynced && std::abs(model.value(paramIndex(1, SourceParam::RandomSmooth)) - 0.6) < 1e-6 &&
              std::abs(model.value(paramIndex(1, SourceParam::RandomChaos)) - 0.3) < 1e-6,
          "a Random source starts synced and bipolar, and its Smooth and Chaos knobs set them");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "17-random-lfo.png"), "render a smoothed Random source");
    // Running, it shows its own last cycles instead, as one line scrolling left, the live dot at its right end.
    model.live.voices = 1;
    const float glides[] = {0.3f, -0.4f, 0.5f, 0.1f, -0.6f, 0.2f, 0.7f};
    for (int cycle = 0; cycle < 6; ++cycle) {
        model.live.lfoFrom[1] = glides[cycle];
        model.live.lfoTo[1] = glides[cycle + 1];
        model.live.lfoPhases[1] = 0.7f;
        model.live.sources[1] = glides[cycle + 1];
        view.poll();
    }
    check(renderer.save(view, dir / "17a-random-sweep.png"), "render a running random LFO's own cycles, scrolling");
    // Four cycles wide and ending 0.7 into the newest, the line reaches back into the fifth newest cycle, and on to
    // the display's left edge (read off the rendered pixels: the line is bright, the display and zero line dark).
    auto lineIn = [&](float x) {
        for (float y = 666.0f; y <= 710.0f; y += 1.0f) {
            const uint32_t color = renderer.pixel({x, y});
            if (std::max({color >> 16 & 255, color >> 8 & 255, color & 255}) > 150)
                return true;
        }
        return false;
    };
    check(lineIn(cardX(1) + 27.0f) && lineIn(cardX(1) + 26.0f + 0.05f * 125.0f),
          "a running random LFO's line reaches the left edge of its display");
    // Between snapshots the line scrolls on at the LFO's rate; run past the end of the cycle before a snapshot brings
    // the next one, it waits there with the dot on the glide's end rather than jump back to the start of the same
    // cycle, and so it does when a snapshot's phase has wrapped round ahead of its glide.
    const Point held{cardX(1) + 26.0f + 125.0f, 688.0f - 0.7f * 18.0f}; // the right end, at the glide's end
    auto dotAt = [&](Point p) {
        const uint32_t color = renderer.pixel(p);
        return (color >> 16 & 255) > 190 && (color >> 8 & 255) > 190 && (color & 255) > 190;
    };
    model.live.lfoPhases[1] = 0.98f;
    model.live.lfoRates[1] = 2.0f;
    model.live.age = 0.05f; // a tenth of a cycle on
    view.poll();
    const bool waits = renderer.save(view, dir / "17a2-random-cycle-end.png") && dotAt(held);
    model.live.lfoPhases[1] = 0.02f;
    model.live.lfoRates[1] = 0.0f;
    model.live.age = 0.0f;
    view.poll();
    const bool stays = renderer.save(view, dir / "17a2-random-cycle-end.png") && dotAt(held);
    check(waits && stays, "a running random LFO's dot waits at the end of its cycle for the next one");
    // Unsynced at 0 Hz, it stands still, holding a value for each note: a line across its display at the value the
    // newest voice holds, the live dot at its right end (read off the rendered pixels), and no Phase knob on its card.
    model.set(paramIndex(1, SourceParam::Sync), 0.0);
    model.set(paramIndex(1, SourceParam::Rate), 0.0);
    model.live.sources[1] = -0.5f;
    model.live.lfoTo[1] = -0.5f;
    view.poll();
    const bool heldShot = renderer.save(view, dir / "17a3-random-held.png");
    const float heldY = 688.0f + 0.5f * 18.0f;
    check(heldShot && dotAt({cardX(1) + 26.0f + 125.0f, heldY}) && lineIn(cardX(1) + 30.0f) &&
              lineIn(cardX(1) + 90.0f),
          "a Random source at 0 Hz shows the value the newest voice holds, as a line across its display");
    model.live = Telemetry{};
    view.poll();
    click(view, sourceRemove(1)); // the LFO alone again, for what follows

    // A card lists no modulations: the button under its display counts them and shows them in the MODS view, where
    // LFO 1's first row is the top one (its middle at y 154). There a row's number takes a typed depth, its depth
    // bar's menu has Edit depth and its dot bypasses it; back in ROUTING, the modulated knob's menu resumes it.
    for (int k = 0; k < 6; ++k) {
        model.set(slotTargetIndex(0, k), slotTargetValue(paramIndex(k, OpParam::Phase)));
        model.set(slotAmountIndex(0, k), 0.1 * (k + 1));
    }
    view.poll();
    const Point modsButton{cardX(0) + 60.0f, 736.0f};
    view.mouseMove(modsButton, {});
    const bool buttonShot = renderer.save(view, dir / "17b-card-mods-button.png");
    click(view, modsButton);
    check(buttonShot && model.ui.view == RoutingView::Mods, "a source card's button shows its modulations in MODS");
    const Point topNumber{661.0f, 182.0f};
    doubleClick(view, topNumber);
    type(view, "-25");
    view.typeKey(View::Key::Enter);
    check(std::abs(model.value(slotAmountIndex(0, 0)) + 0.25) < 1e-9, "a slot's number takes a typed depth");
    view.menuCommand(menuEntry({576.0f, 182.0f}, tr(Str::MenuEditDepth))); // on the depth bar
    type(view, "45");
    view.typeKey(View::Key::Enter);
    check(std::abs(model.value(slotAmountIndex(0, 0)) - 0.45) < 1e-9, "a depth bar's menu has Edit depth");
    click(view, {41.0f, 182.0f});
    const bool slotBypassed = model.value(slotBypassIndex(0, 0)) == 1.0;
    view.mouseMove({400.0f, 468.0f}, {});
    check(slotBypassed && renderer.save(view, dir / "17c-bypassed-slot.png"), "render a bypassed modulation");
    click(view, kRoutingTab);
    view.menuCommand(menuEntry(kOpPhaseKnob, trf(Str::MenuResume, {trf(Str::SourceNameLfo, {"1"})})));
    check(model.value(slotBypassIndex(0, 0)) == 0.0, "a row's dot bypasses its modulation, and the knob's menu resumes it");

    // Until a source modulates something, hovering its handle shows a "Drag to assign" tooltip below it (read off
    // the rendered pixels, against the same view with the mouse away).
    auto belowHandle = [&](const char *name) {
        std::vector<uint32_t> pixels;
        if (renderer.save(view, dir / name))
            for (float x = sourceHandle(0).x - 40.0f; x <= sourceHandle(0).x + 40.0f; x += 2.0f)
                pixels.push_back(renderer.pixel({x, 600.0f}));
        return pixels;
    };
    std::vector<double> targetsBefore;
    for (int slot = 0; slot < kSlots; ++slot) {
        targetsBefore.push_back(model.value(slotTargetIndex(0, slot)));
        model.set(slotTargetIndex(0, slot), 0.0);
    }
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    const std::vector<uint32_t> away = belowHandle("18a-handle-away.png");
    view.mouseMove(sourceHandle(0), {});
    const bool tipped = belowHandle("18b-handle-tooltip.png") != away;
    for (int slot = 0; slot < kSlots; ++slot)
        model.set(slotTargetIndex(0, slot), targetsBefore[slot]);
    view.poll();

    // Hovering a source's handle draws lines to what it modulates, and no tooltip.
    drag(view, sourceHandle(0), kFmAmountKnob);
    view.mouseMove({600.0f, 328.0f}, {});
    const std::vector<uint32_t> linkedAway = belowHandle("18-source-links.png");
    view.mouseMove(sourceHandle(0), {});
    check(tipped && belowHandle("18-source-links.png") == linkedAway,
          "a source's handle shows its tooltip only until it modulates something");

    // The uncommon waves sit in a list behind "...".
    click(view, kMoreWavesButton);
    view.mouseMove(ins(1102.0f, 260.0f), {});
    check(renderer.save(view, dir / "19-more-waves.png"), "render the list of more waves");
    click(view, ins(1102.0f, 236.0f)); // its first entry, Pulse
    view.mouseMove({600.0f, 328.0f}, {});
    check(model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Pulse) &&
              renderer.save(view, dir / "19b-pulse.png"),
          "the list behind ... picks an uncommon wave");
    // Custom, at the end of that list, opens the partial editor: a stroke across the level bars draws them
    // (half way up the bars, which run from -60 dB to 0 dB, is -30 dB), and a double-click takes a bar to nothing.
    click(view, kMoreWavesButton);
    click(view, ins(1102.0f, 236.0f + 6.0f * 24.0f));
    const bool customPicked = model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Custom);
    // Picked after the 25 % pulse, the partials start as the pulse's: the first loudest, the fourth silent.
    const bool fromPulse = std::abs(model.value(partialLevelIndex(0, 0)) - 1.0) < 1e-9 &&
                           std::abs(model.value(partialLevelIndex(0, 1)) - std::sqrt(0.5)) < 0.01 &&
                           model.value(partialLevelIndex(0, 3)) == 0.0;
    check(fromPulse, "picking Custom starts its partials from the wave it was");
    auto barX = [](int partial, int count = kDefaultPartials) { // over the routing panel, right of the lens
        return detail::kPartialLevels.x + (partial + 0.5f) * 772.0f / count;
    };
    const float halfway = 314.0f + 84.0f * 0.5f;
    drag(view, {barX(1), halfway}, {barX(3), halfway});
    doubleClick(view, {barX(0), halfway});
    bool drawn = model.value(partialLevelIndex(0, 0)) == 0.0;
    for (int partial = 1; partial <= 3; ++partial)
        drawn = drawn && std::abs(20.0 * std::log10(model.value(partialLevelIndex(0, partial))) + 30.0) < 1.5;
    drag(view, {barX(2), 430.0f + 48.0f * 0.75f}, {barX(2), 430.0f + 48.0f * 0.75f});
    view.mouseMove({barX(2), halfway}, {});
    check(customPicked && drawn && std::abs(model.value(partialPhaseIndex(0, 2)) - 0.25) < 0.03 &&
              renderer.save(view, dir / "23-partials.png"),
          "picking Custom opens the partial editor, which draws levels and phases");
    // Its resolution, 16, 32 or 64 partials, in the title row: at 64, the 50th can be drawn; at 16, the bars widen
    // to the first 16 and the 50th, unheard, keeps its level, there again at 64.
    auto partialCount = [](int choice) { return Point{detail::kRouting.x + 161.0f + choice * 36.0f, 152.0f}; };
    const int countParam = partialCountIndex(0), fiftieth = partialLevelIndex(0, 49);
    click(view, partialCount(2));
    drag(view, {barX(49, 64), halfway}, {barX(49, 64), halfway});
    const bool at64 = model.value(countParam) == 64.0 &&
                      std::abs(20.0 * std::log10(model.value(fiftieth)) + 30.0) < 1.5;
    click(view, partialCount(0));
    doubleClick(view, {barX(14, 16), halfway}); // the 15th, next to last at 16
    view.mouseMove({barX(1, 16), halfway}, {});
    const bool at16 = model.value(countParam) == 16.0 && model.value(partialLevelIndex(0, 14)) == 0.0 &&
                      model.value(partialLevelIndex(0, 13)) > 0.0 && renderer.save(view, dir / "23b-partials-16.png");
    const double kept = model.value(fiftieth);
    click(view, partialCount(2));
    view.mouseMove({barX(49, 64), halfway}, {});
    check(at64 && at16 && kept > 0.0 && model.value(fiftieth) == kept && renderer.save(view, dir / "23c-partials-64.png"),
          "the partial editor shows 16, 32 or 64 partials, keeping those it hides");
    click(view, partialCount(1));
    // The wheel over them steps from one count offered to the next, as clicks on them do: 32 up to 64, then down to 32
    // and 16, and no further.
    view.wheel(partialCount(1), 1.0f, {});
    const bool wheeled64 = model.value(countParam) == 64.0;
    view.wheel(partialCount(1), -1.0f, {});
    const bool wheeled32 = model.value(countParam) == 32.0;
    view.wheel(partialCount(1), -1.0f, {});
    view.wheel(partialCount(1), -1.0f, {});
    check(wheeled64 && wheeled32 && model.value(countParam) == 16.0, "the wheel steps through the partial counts offered");
    click(view, partialCount(1));
    // The dice left of the reset button gives every partial a random level under a pink slope (partial k at most
    // 1 / sqrt(k)) and a random phase, in one undo step.
    {
        const Point kPartialRandom{detail::kRouting.x + detail::kRouting.w - 98.0f, 152.0f};
        std::vector<double> before;
        for (int k = 0; k < kPartials; ++k)
            before.insert(before.end(), {model.value(partialLevelIndex(0, k)), model.value(partialPhaseIndex(0, k))});
        click(view, kPartialRandom);
        view.mouseMove(kPartialRandom, {});
        bool sloped = true, changed = false;
        double low = 0.0, high = 0.0;
        for (int k = 0; k < kPartials; ++k) {
            const double level = model.value(partialLevelIndex(0, k)), phase = model.value(partialPhaseIndex(0, k));
            sloped = sloped && level >= 0.0 && level <= 1.0 / std::sqrt(k + 1.0) + 1e-9 && phase >= 0.0 && phase < 1.0;
            changed = changed || level != before[2 * k] || phase != before[2 * k + 1];
            if (k < 8)
                low += level / 8.0; // the first eight's mean, against the last 32's
            else if (k >= 32)
                high += level / 32.0;
        }
        const bool shot = renderer.save(view, dir / "23g-partials-random.png");
        view.undo();
        bool restored = true;
        for (int k = 0; k < kPartials; ++k)
            restored = restored && model.value(partialLevelIndex(0, k)) == before[2 * k] &&
                     model.value(partialPhaseIndex(0, k)) == before[2 * k + 1];
        check(shot && sloped && changed && high < low && restored,
              "the partial editor's dice randomizes levels on a pink slope and phases, in one undo step");
    }
    click(view, kRoutingEditorDone);
    // While the wave is Custom, a pencil on the summary line, left of the keyboard switch, opens the partial editor
    // again, lit while it is open, and a second click closes it (read off the rendered pixels: the editor's title).
    {
        const Point kPartialEdit = detail::kPartialEditButton.center();
        auto partialsTitle = [&] {
            std::vector<uint32_t> pixels;
            for (float y = 142.0f; y < 162.0f; y += 2.0f)
                for (float x = detail::kRouting.x + 18.0f; x < detail::kRouting.x + 134.0f; x += 3.0f)
                    pixels.push_back(renderer.pixel({x, y}));
            return pixels;
        };
        view.mouseMove({600.0f, 328.0f}, {});
        const bool closedShot = renderer.save(view, dir / "23d-partials-closed.png");
        const std::vector<uint32_t> closed = partialsTitle();
        click(view, kPartialEdit);
        view.mouseMove({barX(5), halfway}, {});
        const bool openShot = renderer.save(view, dir / "23e-partials-edit.png");
        const bool opened = partialsTitle() != closed;
        const double level = model.value(partialLevelIndex(0, 5));
        drag(view, {barX(5), halfway}, {barX(5), halfway});
        const bool draws = level != model.value(partialLevelIndex(0, 5)) &&
                           std::abs(20.0 * std::log10(model.value(partialLevelIndex(0, 5))) + 30.0) < 1.5;
        view.undo();
        click(view, kPartialEdit);
        view.mouseMove({600.0f, 328.0f}, {});
        const bool shut = renderer.save(view, dir / "23f-partials-shut.png") && partialsTitle() == closed;
        check(closedShot && openShot && opened && draws && shut &&
                  model.value(partialLevelIndex(0, 5)) == level,
              "a Custom wave's edit button opens its partial editor, and a second click closes it");
        // Another node picked in the inspector closes it: filter 1's tab (switching it on), then operator A's again
        // showing the routing panel, not the partials.
        click(view, kPartialEdit);
        view.mouseMove({600.0f, 328.0f}, {});
        const bool reopened = renderer.save(view, dir / "23h-partials-reopened.png") && partialsTitle() != closed;
        click(view, nodeTab(kOps));
        click(view, nodeTab(0));
        view.undo(); // filter 1 off again
        view.mouseMove({600.0f, 328.0f}, {});
        const bool shot = renderer.save(view, dir / "23i-partials-other-node.png");
        check(shot && reopened && partialsTitle() == closed,
              "picking another node in the inspector closes the partial editor");
    }
    // The list's newer waves: Alt Sine, Organ and Vowel, between Abs Sine and Custom.
    click(view, kMoreWavesButton);
    click(view, ins(1102.0f, 236.0f + 4.0f * 24.0f));
    check(model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Organ), "the list picks Organ");
    view.undo();

    // The full-size shape editor (from a Curve source's widened card) spans the inspector, which it hides: no
    // operator tab or knob where they were. Its grid: a step up across is 24 lines, and Ctrl snaps a point to them.
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Curve));
    view.poll();
    click(view, lfoDisplay(0));
    click(view, wideButton(0, 2));
    const double fineBefore = model.value(paramIndex(0, OpParam::Phase));
    drag(view, kOpPhaseKnob, {kOpPhaseKnob.x, kOpPhaseKnob.y - 30.0f});
    check(model.value(paramIndex(0, OpParam::Phase)) == fineBefore && model.value(shapeCountIndex(0)) == 2.0,
          "the LFO shape editor hides the inspector");
    click(view, shapeGrid(0, 2));
    click(view, shapeGrid(1, 0));
    check(model.layout().shapeGridX == 24 && model.layout().shapeGridY == 6, "the grid's buttons step its lines");
    view.mouseDown(shapeAt(0.5, 1.0), {});
    view.mouseMove(shapeAt(0.3, 0.6), {false, true});
    view.mouseUp(shapeAt(0.3, 0.6), {false, true});
    const int point = shapePointIndex(0, 1, PointField::X);
    check(std::abs(model.value(point) - 7.0 / 24.0) < 1e-9 &&
              std::abs(model.value(shapePointIndex(0, 1, PointField::Y)) - 2.0 / 3.0) < 1e-9,
          "Ctrl snaps a point to the grid");
    view.mouseMove(shapeAt(0.3, 0.6), {});
    check(renderer.save(view, dir / "20-shape-grid.png"), "render the shape editor with a finer grid");
    click(view, kShapeReset);
    const bool reset = model.value(point) == 0.5 && model.value(shapeCountIndex(0)) == 2.0;
    view.undo();
    check(reset && std::abs(model.value(point) - 7.0 / 24.0) < 1e-9, "the reset button brings the first shape back, and undoes");
    click(view, kShapeDone); // back into the widened card, and that back to its size
    click(view, wideButton(0, 0));

    // Taking an effect out is one step, its modulation included.
    click(view, kFxTab);
    addFx(view, FxType::Delay);
    drag(view, sourceHandle(0), fxKnob(model, 0, 2, 3, 266.0f));
    const int mix = paramIndex(0, FxParam::DelayMix);
    click(view, fxRemove(model, 0));
    const bool removed = model.value(paramIndex(0, FxParam::Type)) == 0.0;
    click(view, kUndoButton);
    bool modulated = false;
    for (int slot = 0; slot < kSlots; ++slot)
        modulated = modulated || slotTarget(model.value(slotTargetIndex(0, slot))) == mix;
    check(removed && model.value(paramIndex(0, FxParam::Type)) == static_cast<double>(FxType::Delay) && modulated,
          "undo brings a removed effect back with its modulation");

    // The reverb, phaser, flanger and frequency shifter, each with its icon in the list and on its card.
    click(view, kFxMenuButton);
    view.mouseMove(fxMenuItem(FxType::Phaser), {});
    check(renderer.save(view, dir / "21-fx-menu.png"), "render the ADD EFFECT list with its icons");
    bool strips = true;
    for (int group = 0; group <= detail::kFxMenuSplitGroup; ++group)
        strips = strips && fxGroupStrip(renderer, {-1.0f, -1.0f}, group);
    check(strips, "the ADD EFFECT list shows each group's name on a strip, the splits' too (read off the rendered pixels)");
    click(view, {600.0f, 428.0f});
    for (FxType type : {FxType::Reverb, FxType::Phaser, FxType::Flanger, FxType::FreqShifter})
        addFx(view, type);
    bool added = fxChain(model) == std::vector<int>{0, 1, 2, 3, 4};
    for (int slot = 1; slot < 5; ++slot)
        added = added && model.value(paramIndex(slot, FxParam::Type)) == static_cast<double>(FxType::Reverb) + slot - 1;
    // The frequency shifter's Shift, the second knob, after its Range: a share of it.
    model.set(paramIndex(4, FxParam::ShiftRange), normFromShiftRange(1000.0));
    const double shift = model.value(paramIndex(4, FxParam::ShiftAmount));
    const Point shiftKnob = fxKnob(model, 4, 1, 3, 206.0f);
    drag(view, shiftKnob, {shiftKnob.x, shiftKnob.y - 20.0f});
    check(added && model.value(paramIndex(4, FxParam::ShiftAmount)) > shift + 0.1,
          "the newer effects join the chain, and their knobs work");
    model.set(paramIndex(2, FxParam::PhaserFeedback), 0.8);
    model.set(paramIndex(3, FxParam::FlangerFeedback), -0.6);
    model.set(paramIndex(4, FxParam::ShiftSpread), 0.8);
    view.mouseMove({600.0f, 328.0f}, {});
    check(renderer.save(view, dir / "21b-new-fx.png"), "render the reverb, phaser, flanger and frequency shifter");
    // The reverb's algorithm comes from a list under its button; its display follows the Size and the algorithm.
    click(view, {fxCardX(model, 1) + 40.0f, 180.0f + 44.0f + 11.0f});
    view.mouseMove({fxCardX(model, 1) + 40.0f, 265.0f + 3.0f * 22.0f}, {});
    const bool listed = renderer.save(view, dir / "21c-reverb-list.png");
    click(view, {fxCardX(model, 1) + 40.0f, 265.0f + 22.0f}); // Room
    check(listed && model.value(paramIndex(1, FxParam::ReverbType)) == static_cast<double>(ReverbType::Room),
          "the reverb's list picks its algorithm");
    model.set(paramIndex(1, FxParam::ReverbSize), 0.1);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool smallRoom = renderer.save(view, dir / "21d-reverb-small-room.png");
    model.set(paramIndex(1, FxParam::ReverbType), static_cast<double>(ReverbType::Shimmer));
    model.set(paramIndex(1, FxParam::ReverbSize), 1.0);
    check(smallRoom && renderer.save(view, dir / "21e-reverb-big-shimmer.png"),
          "render a small room and a big shimmer");
    model.set(paramIndex(1, FxParam::ReverbType), static_cast<double>(ReverbType::Hall));
    model.set(paramIndex(1, FxParam::ReverbSize), paramTable()[paramIndex(1, FxParam::ReverbSize)].def);
    click(view, kRoutingTab);

    // A modulated route: its wire swells with its modulation, its label shows the range.
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    view.poll();
    const Point aNode = nodeCenter(model.layout(), 0), bNode = nodeCenter(model.layout(), 1);
    drag(view, portOf(model.layout(), 1), aNode);
    const float span = std::hypot(aNode.x - bNode.x, aNode.y - bNode.y);
    const Point along{(aNode.x - bNode.x) / span, (aNode.y - bNode.y) / span};
    const Point wireLabel{(aNode.x + bNode.x) * 0.5f - along.y * span * 0.09f,
                          (aNode.y + bNode.y) * 0.5f + along.x * span * 0.09f};
    drag(view, sourceHandle(0), wireLabel);
    bool routeModulated = false;
    for (int slot = 0; slot < kSlots; ++slot)
        routeModulated = routeModulated || slotTarget(model.value(slotTargetIndex(0, slot))) == matrixIndex(1, 0);
    model.live.voices = 1;
    model.live.modOffset[matrixIndex(1, 0)] = 0.2f;
    view.poll();
    view.mouseMove({600.0f, 228.0f}, {});
    check(routeModulated && renderer.save(view, dir / "18b-modulated-route.png"),
          "render a modulated route, its wire swelling and its label showing the range");
    // That route turned down to 0 %: B, sounding, lights up only while its modulation opens the route again, just
    // above it (read off the rendered pixels), with the glow setting at its middle, where it starts, its halo
    // feathering out past the ring; off, the halo is a plain ring of light (7 px out), nothing beyond it.
    const double routed = model.value(matrixIndex(1, 0));
    model.glowLevel = 0.5;
    model.set(matrixIndex(1, 0), 0.0);
    model.live.opLevels[1] = 1.0f;
    struct Above {
        uint32_t ring, beyond; // in the plain ring's reach, and past it
    };
    auto aboveB = [&](float offset) {
        model.live.modOffset[matrixIndex(1, 0)] = offset;
        view.poll();
        renderer.save(view, dir / "18b2-modulated-route-glow.png");
        return Above{renderer.pixel({bNode.x, bNode.y - 28.0f}), renderer.pixel({bNode.x, bNode.y - 33.0f})};
    };
    const Above shut = aboveB(0.0f), opened = aboveB(0.3f);
    model.glowLevel = 0.0;
    const Above glowOff = aboveB(0.3f);
    check(opened.ring != shut.ring && opened.beyond != shut.beyond && glowOff.ring != shut.ring &&
              glowOff.beyond == shut.beyond,
          "an operator lights up while modulation opens its route from 0 %, a plain ring with the glow setting off");
    model.set(matrixIndex(1, 0), routed);
    model.live = Telemetry{};
    view.poll();

    // Key tracking moves a filter's response with the newest voice's note: at 100 %, a C7 takes the low-pass three
    // octaves up, so 3.2 kHz, under its fall at C4, is in its passband, which is filled below the curve.
    click(view, kF1Tab);
    const int keyTrack = paramIndex(0, FilterParam::KeyTrack);
    model.set(keyTrack, 1.0);
    model.live.voices = 1;
    auto fillAt = [&](float pitch) {
        model.live.notePitch = pitch;
        view.poll();
        renderer.save(view, dir / "18e-filter-key-track.png");
        return static_cast<int>(renderer.pixel(ins(910.0f + 0.75f * 248.0f, 329.0f)) >> 16 & 255);
    };
    const int fillC4 = fillAt(60.0f), fillC7 = fillAt(96.0f);
    check(fillC7 > fillC4 + 10, "a filter's response moves with the note its key tracking follows");
    model.set(keyTrack, 0.0);
    model.live = Telemetry{};
    view.poll();

    // A modulated filter: its response shows how far the modulation reaches, and follows it while notes play.
    drag(view, sourceHandle(0), ins(918.0f, 242.0f)); // onto its Cutoff knob
    bool cutoffModulated = false;
    for (int slot = 0; slot < kSlots; ++slot)
        cutoffModulated = cutoffModulated || slotTarget(model.value(slotTargetIndex(0, slot))) == paramIndex(0, FilterParam::Cutoff);
    model.live.voices = 1;
    model.live.modOffset[paramIndex(0, FilterParam::Cutoff)] = 0.18f;
    view.poll();
    view.mouseMove({600.0f, 228.0f}, {});
    check(cutoffModulated && renderer.save(view, dir / "18c-modulated-filter.png"),
          "render a modulated filter's response, its reach and where it is now");
    model.live = Telemetry{};
    view.poll();
    click(view, ins(912.0f, 152.0f)); // back to operator A

    // Presets: the bar shows Init; the arrows step through the library, asking first while the patch has
    // changed since it was loaded; the browser lists them by subfolder.
    check(model.presetName() == tr(Str::PresetInit) && model.patchEdited(), "a changed patch shows Init, changed");
    const Point nextPreset{787.0f, 18.0f}, previousPreset{413.0f, 18.0f}, presetBox{600.0f, 18.0f};
    click(view, nextPreset);
    const Rect confirmPanel{420.0f, std::round((view.height() - 150.0f) * 0.5f), 360.0f, 150.0f};
    view.mouseMove({confirmPanel.right() - 64.0f, confirmPanel.bottom() - 38.0f}, {});
    check(model.presetIndex() == -1 && renderer.save(view, dir / "22-preset-confirm.png"),
          "the next-preset arrow asks first while the patch has changed");
    click(view, {confirmPanel.right() - 64.0f, confirmPanel.bottom() - 38.0f});
    click(view, nextPreset);
    const bool stepped = model.presetIndex() == 1;
    click(view, previousPreset);
    click(view, previousPreset);
    check(stepped && model.presetIndex() == 2, "the arrows step through the presets, round from the first to the last");
    // The browser: OPEN FOLDER shows the folder, and a preset picked loads and closes it. While it is open, its
    // backdrop dims the header around it (read off the rendered pixels).
    const Point headerGap{300.0f, 4.0f}; // in the title bar, clear of its buttons
    view.mouseMove(presetBox, {});
    renderer.save(view, dir / "22a-preset-stepped.png");
    const uint32_t clear = renderer.pixel(headerGap);
    click(view, presetBox);
    const float browserTop = std::round((view.height() - 560.0f) * 0.5f); // the browser, centered in the editor
    auto browserRow = [&](int row) { return Point{600.0f, browserTop + 56.0f + row * 24.0f + 12.0f}; };
    view.mouseMove(browserRow(2), {});
    check(renderer.save(view, dir / "22b-preset-browser.png"), "render the preset browser");
    const uint32_t dimmed = renderer.pixel(headerGap);
    click(view, {380.0f + 440.0f - 24.0f - 55.0f, browserTop + 560.0f - 27.0f}); // OPEN FOLDER
    click(view, browserRow(2)); // Bell, under the Factory header
    view.mouseMove(presetBox, {});
    check(renderer.save(view, dir / "22c-preset-bar.png"), "render the preset bar");
    const bool closed = renderer.pixel(headerGap) == clear && clear != dimmed;
    check(model.presetIndex() == 1 && model.folderOpens == 1 && closed,
          "the browser opens the folder, and loading a preset from it closes it");
    // A preset loaded starts the undo history over: undo doesn't bring back the preset before, or an edit made to it.
    drag(view, kFmAmountKnob, {kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    model.loadPresetAt(2);
    view.undo();
    check(model.presetName() == "Pad" && !model.patchEdited() &&
              model.value(paramIndex(GlobalParam::Volume)) == -16.0,
          "undo after a preset load leaves the preset loaded");
    drag(view, kFmAmountKnob, {kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    const bool changedPad = model.presetName() == "Pad" && model.patchEdited();
    view.undo();
    check(changedPad && model.presetName() == "Pad" && !model.patchEdited(),
          "undoing the edits since a preset loaded leaves it unchanged again");
    // While the patch has changed, a preset picked in the browser asks first, over it; going ahead loads it and
    // closes the browser.
    drag(view, kFmAmountKnob, {kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    click(view, presetBox);
    click(view, browserRow(2)); // Bell
    const bool asked = model.presetIndex() == 2;
    click(view, {confirmPanel.right() - 64.0f, confirmPanel.bottom() - 38.0f}); // go ahead
    view.mouseMove(presetBox, {});
    renderer.save(view, dir / "22d-preset-loaded.png");
    check(asked && model.presetIndex() == 1 && renderer.pixel(headerGap) == clear,
          "a preset picked in the browser while the patch has changed asks first, then loads and closes it");
    // The browser takes the keyboard for its search: the words typed narrow the list to the presets whose subfolder
    // and name hold them all, the first one picked, and Enter loads it. With nothing found Enter does nothing; the
    // search's x empties it, Up and Down pick from the list, from the preset loaded, and Escape empties the search,
    // then closes the browser.
    click(view, presetBox);
    const bool takesKeys = view.typing();
    for (char ch : std::string("fac p")) // Factory, Pad
        view.typeChar(static_cast<char32_t>(ch));
    check(renderer.save(view, dir / "22e-preset-search.png"), "render the preset browser's search");
    view.typeKey(View::Key::Enter);
    check(takesKeys && model.presetIndex() == 2 && !view.typing(),
          "typing in the preset browser searches it, and Enter loads the preset found");
    click(view, presetBox);
    view.typeChar(U'z');
    view.typeKey(View::Key::Enter);
    const bool nothingFound = view.typing() && model.presetIndex() == 2;
    click(view, {380.0f + 440.0f - 46.0f - 22.0f + 9.0f, browserTop + 26.0f}); // the search's x
    view.typeKey(View::Key::Up); // from Pad: Bell, then Init Bass
    view.typeKey(View::Key::Up);
    view.typeKey(View::Key::Down);
    view.mouseMove(presetBox, {});
    check(renderer.save(view, dir / "22f-preset-picked.png"), "render a preset picked from the keyboard");
    view.typeKey(View::Key::Enter);
    check(nothingFound && model.presetIndex() == 1, "Up and Down pick a preset from the browser's list");
    click(view, presetBox);
    view.typeChar(U'b');
    view.typeKey(View::Key::Escape);
    const bool stillOpen = view.typing();
    view.typeKey(View::Key::Escape);
    check(stillOpen && !view.typing() && model.presetIndex() == 1,
          "Escape empties the browser's search, then closes the browser");
    {
        // Editing the search: Shift+Left selects back from the caret, typing replaces the selection, Home and End
        // jump to the ends (with Shift, selecting on the way), Ctrl+X cuts the selection and Ctrl+V pastes over
        // it, a line break as a space.
        click(view, presetBox);
        type(view, "bell");
        view.typeKey(View::Key::Left, true);
        view.typeKey(View::Key::Left, true);
        const std::string selected = view.copyText(false);
        type(view, "x"); // bex
        view.typeKey(View::Key::Home);
        type(view, "a"); // abex
        view.typeKey(View::Key::End, true);
        const std::string cut = view.copyText(true); // leaves a
        view.typeKey(View::Key::Home, true);
        view.pasteText("Fac\npad");
        check(selected == "ll" && cut == "bex" && view.copyText(false).empty() &&
                  renderer.save(view, dir / "22g-preset-search-edited.png"),
              "the browser's search selects with Shift and the arrows, Home and End, and cuts and pastes");
        // The mouse, by where the characters were painted: a drag from the field's start to its end selects all of
        // it, a click puts the caret there, a Shift+click selects from the caret, and a double-click picks a word.
        const float fieldY = browserTop + 26.0f, start = 380.0f + 440.0f - 46.0f - 180.0f + 4.0f,
                    end = 740.0f; // past the text, short of the x
        drag(view, {start, fieldY}, {end, fieldY});
        const std::string dragged = view.copyText(false);
        const bool selectedShot = renderer.save(view, dir / "22h-preset-search-selected.png");
        click(view, {start, fieldY});
        const bool placed = view.copyText(false).empty();
        click(view, {end, fieldY}, {true, false, false});
        const std::string extended = view.copyText(false);
        doubleClick(view, {end, fieldY});
        const std::string word = view.copyText(false);
        view.typeKey(View::Key::Enter);
        check(dragged == "Fac pad" && selectedShot && placed && extended == "Fac pad" && word == "pad" && model.presetIndex() == 2,
              "the mouse puts the caret in the browser's search and selects in it");
    }

    // Dragging a source's handle to the panel's right end scrolls the cards along.
    for (int n = 0; n < 4; ++n)
        addSource(view, SourceType::Lfo);
    model.set(paramIndex(4, SourceParam::Sync), 0.0); // the fifth free, its knob a rate rather than a note value
    view.wheel({60.0f, 541.0f}, 10.0f, {}); // back to the first card
    view.mouseDown(sourceHandle(0), {});
    view.mouseMove({1180.0f, 678.0f}, {});
    for (int n = 0; n < 40; ++n)
        view.poll();
    // Scrolled to the end: the fifth card, the dashed ones after it.
    view.mouseMove(rateKnob(3.0f - kSourceShadowSpan), {});
    view.mouseUp(rateKnob(3.0f - kSourceShadowSpan), {});
    bool fifth = false;
    for (int slot = 0; slot < kSlots; ++slot)
        fifth = fifth || slotTarget(model.value(slotTargetIndex(0, slot))) == paramIndex(4, SourceParam::Rate);
    check(fifth, "holding a dragged handle at the panel's edge scrolls the cards to the fifth");
}

// Moving a card past either end of its row scrolls the row along under it: the first effect taken to the right end
// lands last, and so does the first LFO, which taken back to the left end lands first again.
void testCardEdgeScroll(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    for (FxType type : {FxType::Delay, FxType::Reverb, FxType::Phaser, FxType::Flanger, FxType::Compressor,
                        FxType::Eq, FxType::Widener, FxType::Disperser})
        addFx(view, type);
    view.wheel({600.0f, 153.0f}, 10.0f, {}); // back to the start
    fxScrolled = 0.0f;
    const int firstFx = fxChain(model).front();
    view.mouseDown(fxGrip(model, 0), {});
    view.mouseMove({fxGrip(model, 0).x + 300.0f, 197.0f}, {});
    view.mouseMove({1180.0f, 197.0f}, {}); // at the panel's right end
    for (int n = 0; n < 80; ++n)
        view.poll();
    const bool fxShot = renderer.save(view, dir / "8c-fx-edge-scroll.png");
    view.mouseUp({1180.0f, 197.0f}, {});
    check(fxShot && fxChain(model).size() == 8 && fxChain(model).back() == firstFx,
          "an effect card held at the row's right end scrolls the row along, and lands at the end");

    for (int n = 0; n < 5; ++n)
        addSource(view, SourceType::Lfo);
    view.wheel({60.0f, 541.0f}, 10.0f, {}); // back to the first card
    view.mouseDown(sourceGrip(0), {});
    view.mouseMove({sourceGrip(0).x + 200.0f, 572.0f}, {});
    view.mouseMove({1180.0f, 572.0f}, {});
    for (int n = 0; n < 80; ++n)
        view.poll();
    view.mouseUp({1180.0f, 572.0f}, {});
    const bool last = shown(model) == std::vector<int>{1, 2, 3, 4, 0};
    // Scrolled to the end, it shows in the fifth place, before the dashed cards.
    const Point grip = sourceGrip(3.0f - kSourceShadowSpan);
    view.mouseDown(grip, {});
    view.mouseMove({grip.x - 200.0f, 572.0f}, {});
    view.mouseMove({20.0f, 572.0f}, {}); // at the panel's left end
    for (int n = 0; n < 80; ++n)
        view.poll();
    const bool sourceShot = renderer.save(view, dir / "11f-source-edge-scroll.png");
    view.mouseUp({20.0f, 572.0f}, {});
    check(last && sourceShot && shown(model) == std::vector<int>{0, 1, 2, 3, 4},
          "an LFO card held at either end of the row scrolls the row along, and lands at that end");
}

// The editor's dialogs take Enter and Escape: the bin's, the preset arrows' and the browser's "changed, load anyway?"
// (over the browser, whose search lets the keyboard go meanwhile) and the settings. Escape cancels or closes, Enter
// goes ahead; other keys and no dialog take nothing.
void testDialogKeys(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const bool none = !view.dialogOpen() && !view.dialogKey(View::Key::Escape);
    // The bin's dialog: Escape keeps the effects, Enter removes them (one undo step).
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    const Point bin = rectCenter(detail::fxClearButton());
    click(view, bin);
    const bool asked = view.dialogOpen() && !view.typing() && !view.dialogKey(View::Key::Up);
    const bool escaped =
        view.dialogKey(View::Key::Escape) && !view.dialogOpen() && fxChain(model) == std::vector<int>{0, 1};
    click(view, bin);
    const bool entered = view.dialogKey(View::Key::Enter) && !view.dialogOpen() && fxChain(model).empty();
    view.undo();
    check(none && asked && escaped && entered && fxChain(model) == std::vector<int>{0, 1},
          "Escape cancels the bin's dialog and Enter goes ahead");

    // The settings dialog: Escape closes it, and Enter too, nothing in it waiting to be confirmed.
    click(view, kSettingsButton);
    const bool settingsOpen = view.dialogOpen() && renderer.save(view, dir / "14h-settings-keys.png");
    const bool settingsEscaped = view.dialogKey(View::Key::Escape) && !view.dialogOpen();
    click(view, kSettingsButton);
    const bool settingsEntered = view.dialogKey(View::Key::Enter) && !view.dialogOpen();
    check(settingsOpen && settingsEscaped && settingsEntered, "Escape and Enter close the settings dialog");

    // The next-preset arrow on a changed patch asks first: Escape stays on Init, Enter loads the first preset.
    drag(view, kFmAmountKnob, {kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    const Point nextPreset{787.0f, 18.0f}, presetBox{600.0f, 18.0f};
    click(view, nextPreset);
    const bool presetAsked = view.dialogOpen() && model.presetIndex() == -1;
    view.dialogKey(View::Key::Escape);
    const bool stayed = !view.dialogOpen() && model.presetIndex() == -1 && model.patchEdited();
    click(view, nextPreset);
    view.dialogKey(View::Key::Enter);
    const bool loaded = !view.dialogOpen() && model.presetIndex() == 0;
    // From the browser: Enter in its search loads the preset found, asking first over it, where the search lets the
    // keyboard go; Enter there loads it and closes the browser.
    drag(view, kFmAmountKnob, {kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    click(view, presetBox);
    type(view, "bell");
    view.typeKey(View::Key::Enter);
    const bool overBrowser = view.dialogOpen() && !view.typing() && model.presetIndex() == 0 &&
                             renderer.save(view, dir / "22j-preset-confirm-keys.png");
    view.dialogKey(View::Key::Enter);
    check(presetAsked && stayed && loaded && overBrowser && model.presetName() == "Bell" && !view.typing() &&
              !view.dialogOpen(),
          "Escape cancels the preset dialog and Enter loads the preset, over the browser too");
}

// Ctrl held as a card or node starts to move carries a copy of it, the original staying: an effect card in the row,
// an effect's node and a split's (with its join and its bands' effects) in the graph, and a source card. The copy
// lands where it is let go of, with the original's settings, one undo step; a drag called off leaves nothing.
void testCtrlCopy(const std::filesystem::path &dir, Renderer &renderer) {
    const Modifiers ctrl{false, true, false};
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
    model.values[paramIndex(0, FxParam::ShaperDrive)] = 0.8;
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    fxScrolled = 0.0f;
    auto copied = [&](int copy, int original) {
        return model.value(paramIndex(copy, FxParam::Type)) == model.value(paramIndex(original, FxParam::Type)) &&
               model.value(paramIndex(copy, FxParam::ShaperDrive)) ==
                   model.value(paramIndex(original, FxParam::ShaperDrive));
    };
    // The row: the shaper carried past the reverb, its copy landing after it.
    const Point grip = fxGrip(model, 0), past{grip.x + 1.5f * kFxStep, grip.y};
    view.mouseDown(grip, ctrl);
    view.mouseMove({grip.x + 8.0f, grip.y}, ctrl);
    view.mouseMove(past, ctrl);
    for (int n = 0; n < 20; ++n)
        view.poll();
    const bool rowShot = renderer.save(view, dir / "8e-fx-ctrl-copy.png");
    view.mouseUp(past, ctrl);
    const bool rowCopy = fxChain(model) == std::vector<int>{0, 1, 2} && copied(2, 0);
    view.undo();
    const bool rowUndone =
        fxChain(model) == std::vector<int>{0, 1} && model.value(paramIndex(2, FxParam::Type)) == 0.0;
    // Without Ctrl it moves; called off, the copy goes again and leaves no undo step.
    drag(view, grip, past);
    const bool moved = fxChain(model) == std::vector<int>{1, 0};
    view.undo();
    const UndoStep *last = model.nextUndo();
    view.mouseDown(grip, ctrl);
    view.mouseMove({grip.x + 8.0f, grip.y}, ctrl);
    view.mouseMove(past, ctrl);
    view.cancelDrag();
    const bool calledOff = fxChain(model) == std::vector<int>{0, 1} &&
                           model.value(paramIndex(2, FxParam::Type)) == 0.0 && model.nextUndo() == last;
    check(rowShot && rowCopy && rowUndone && moved && calledOff,
          "an effect card Ctrl-dragged along the row leaves a copy where it is let go of, one undo step");

    // The graph: the shaper's node, then a split holding a delay on its high band, each copied onto the line.
    click(view, rectCenter(detail::fxModeButton(1)));
    auto cell = [](int col, int row) { return detail::fxGridCell(col, row, 0.0f); };
    auto settle = [&] {
        for (int k = 0; k < 40; ++k)
            view.poll();
    };
    auto dragNode = [&](Point from, Point to, Modifiers mods) {
        view.mouseMove(from, {});
        view.mouseDown(from, mods);
        view.mouseMove({from.x + 8.0f, from.y + 4.0f}, mods);
        view.mouseMove(to, mods);
        settle();
        view.mouseUp(to, mods);
        settle();
    };
    // Its copy's place opens right after it as it starts to move, so past the reverb is a column further on.
    dragNode(cell(1, 0), detail::fxGridAt(3.5f, 0.0f, 0.0f), ctrl);
    const bool nodeCopy = fxChain(model) == std::vector<int>{0, 1, 2} && copied(2, 0) && model.ui.fxSelected == 2;
    view.undo();
    settle();
    const Point plus = cell(3, 0);
    click(view, plus);
    click(view, fxListSplit(view, besidePlus(plus)));
    const Point high = cell(4, -1);
    click(view, high);
    click(view, fxListItem(view, besidePlus(high), FxType::Delay, true));
    model.values[paramIndex(0, SplitParam::Low)] = 0.3;
    view.poll();
    settle();
    const bool split =
        fxChain(model) == std::vector<int>{0, 1, 2} && model.value(paramIndex(0, SplitParam::Used)) == 1.0;
    dragNode(cell(3, 0), detail::fxGridAt(0.5f, 0.0f, 0.0f), ctrl);
    view.mouseMove({600.0f, 428.0f}, {});
    const bool splitShot = renderer.save(view, dir / "40v-fx-graph-ctrl-copy.png");
    const bool splitCopy = fxChain(model) == std::vector<int>{3, 0, 1, 2} &&
                           model.value(paramIndex(1, SplitParam::Used)) == 1.0 &&
                           model.value(paramIndex(1, SumParam::Used)) == 1.0 &&
                           model.value(paramIndex(1, SplitParam::Low)) == 0.3 &&
                           model.value(paramIndex(3, FxParam::Type)) == static_cast<double>(FxType::Delay) &&
                           model.value(paramIndex(0, SplitParam::Used)) == 1.0;
    view.undo();
    settle();
    check(nodeCopy && split && splitShot && splitCopy && fxChain(model) == std::vector<int>{0, 1, 2} &&
              model.value(paramIndex(1, SplitParam::Used)) == 0.0 && model.value(paramIndex(3, FxParam::Type)) == 0.0,
          "a node Ctrl-dragged in the effects graph leaves a copy where it is let go of, a split with its join and "
          "its bands' effects, one undo step");

    // A source card: LFO 1 carried past LFO 2, its copy (rate and all, modulating nothing) landing after it.
    FakeController sources;
    View sourceView(sources);
    addSource(sourceView, SourceType::Lfo);
    addSource(sourceView, SourceType::Lfo);
    sources.values[paramIndex(0, SourceParam::Rate)] = 0.7;
    sources.values[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(GlobalParam::FmAmount));
    sources.values[slotAmountIndex(0, 0)] = 0.3;
    sourceView.poll();
    sourceView.wheel({60.0f, 541.0f}, 10.0f, {}); // the first card
    const Point sourceAt = sourceGrip(0), sourceTo{cardX(1.0f) + 250.0f, 572.0f};
    sourceView.mouseDown(sourceAt, ctrl);
    sourceView.mouseMove({sourceAt.x + 8.0f, sourceAt.y}, ctrl);
    sourceView.mouseMove(sourceTo, ctrl);
    const bool sourceShot = renderer.save(sourceView, dir / "11g-source-ctrl-copy.png");
    sourceView.mouseUp(sourceTo, ctrl);
    const bool sourceCopy = shown(sources) == std::vector<int>{0, 1, 2} &&
                            sources.value(paramIndex(2, SourceParam::Type)) == static_cast<double>(SourceType::Lfo) &&
                            sources.value(paramIndex(2, SourceParam::Rate)) == 0.7 &&
                            slotTarget(sources.value(slotTargetIndex(2, 0))) < 0 &&
                            slotTarget(sources.value(slotTargetIndex(0, 0))) == paramIndex(GlobalParam::FmAmount);
    sourceView.undo();
    check(sourceShot && sourceCopy && shown(sources) == std::vector<int>{0, 1} &&
              sources.value(paramIndex(2, SourceParam::Type)) == 0.0,
          "a source card Ctrl-dragged leaves a copy where it is let go of, one undo step");
}

// Two fingers pan what the middle button drags (the effects' row, the source cards, the preset browser's list),
// ending what the first finger pressed without dropping anything, and doing nothing over a dialog; the wheel turned
// sideways scrolls the row, the source cards and the effects graph. One finger turns a knob it lands on (a double tap
// setting it back), taps what it lands on, and elsewhere pans the row, the source cards and the browser's list
// without picking what it went down on.
void testTouchPan(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    for (FxType type : {FxType::Delay, FxType::Reverb, FxType::Phaser, FxType::Flanger, FxType::Compressor,
                        FxType::Widener, FxType::Disperser, FxType::FreqShifter})
        addFx(view, type);
    view.wheel({600.0f, 153.0f}, 10.0f, {}); // back to the start
    const std::vector<int> chain = fxChain(model);
    auto fxAt = [&](float scrolled, int position) { // switched off and on again by its icon there
        fxScrolled = scrolled;
        click(view, fxPower(model, position));
        const bool off = model.value(paramIndex(chain[position], FxParam::Enabled)) == 0.0;
        click(view, fxPower(model, position));
        fxScrolled = 0.0f;
        return off;
    };
    // The first finger on a knob, the second beside it: the knob stays as it was, and the row goes along.
    const ParamValues before = model.values;
    const Point grab{700.0f, 328.0f};
    view.mouseDown(grab, {});
    const bool began = view.touchPanBegin(grab);
    view.touchPanMove({grab.x - kFxStep, grab.y + 6.0f});
    view.touchPanMove({grab.x - 2.0f * kFxStep, grab.y + 6.0f});
    const bool fxShot = renderer.save(view, dir / "8f-fx-touch-pan.png");
    view.touchPanEnd({grab.x - 2.0f * kFxStep, grab.y + 6.0f});
    view.mouseUp({grab.x - 2.0f * kFxStep, grab.y + 6.0f}, {}); // the first finger's, after
    check(began && fxShot && model.values == before && fxAt(2.0f, 2),
          "two fingers pan the effects' row, leaving the knob the first one went down on");
    // Sideways, the wheel scrolls the row: back two places, then on two.
    view.wheelAcross(grab, -5.0f);
    const bool back = fxAt(0.0f, 2);
    view.wheelAcross(grab, 5.0f);
    check(back && fxAt(2.0f, 2), "the wheel turned sideways scrolls the effects' row");

    // One finger: on a card's knob it turns it (the row staying), and a double tap puts it back.
    constexpr float kSlop = 6.0f;
    auto tap = [&](View &on, Point p, bool second = false) {
        on.touchDown(p, second, kSlop);
        on.touchMove({p.x + 2.0f, p.y + 1.0f}); // a finger's jitter
        on.touchUp(p);
    };
    fxScrolled = 2.0f; // the phaser's Rate, first on the first card shown
    const Point rate = fxKnob(model, 2, 0, 3, 205.0f);
    fxScrolled = 0.0f;
    view.touchDown(rate, false, kSlop);
    view.touchMove({rate.x + 1.0f, rate.y - 15.0f});
    view.touchMove({rate.x + 1.0f, rate.y - 30.0f});
    view.touchUp({rate.x + 1.0f, rate.y - 30.0f});
    const bool turned = model.values != before && fxAt(2.0f, 2);
    tap(view, rate);
    tap(view, rate, true);
    const bool reset = model.values == before;
    // Off the knobs, near a card's foot, it pans the row (two places back to the start), turning nothing; a tap on an
    // effect's icon switches it.
    const Point foot{700.0f, 476.0f};
    view.touchDown(foot, false, kSlop);
    view.touchMove({foot.x + kFxStep, foot.y + 3.0f});
    view.touchMove({foot.x + 2.0f * kFxStep, foot.y + 3.0f});
    const bool oneShot = renderer.save(view, dir / "8g-fx-one-finger-pan.png");
    view.touchUp({foot.x + 2.0f * kFxStep, foot.y + 3.0f});
    const bool panned = model.values == before && fxAt(0.0f, 2);
    tap(view, fxPower(model, 2));
    const bool tappedOff = model.value(paramIndex(chain[2], FxParam::Enabled)) == 0.0;
    tap(view, fxPower(model, 2));
    check(turned && reset && oneShot && panned && tappedOff &&
              model.value(paramIndex(chain[2], FxParam::Enabled)) == 1.0,
          "one finger turns a knob it lands on, a double tap putting it back, pans the effects' row off the knobs and "
          "taps an effect's icon");
    // Where nothing scrolls, it is the mouse at once: the header's FM Amount turns.
    const double fmAmount = model.value(paramIndex(GlobalParam::FmAmount));
    view.touchDown(kFmAmountKnob, false, kSlop);
    view.touchMove({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    view.touchUp({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    check(model.value(paramIndex(GlobalParam::FmAmount)) != fmAmount, "one finger turns a knob where nothing scrolls");

    // The source cards, a card's width to the left by two fingers, then back by the wheel sideways.
    for (int n = 0; n < 5; ++n)
        addSource(view, SourceType::Lfo);
    view.wheel({60.0f, 541.0f}, 10.0f, {});
    const std::vector<int> sources = shown(model);
    auto firstShown = [&] { // which card shows first: switched off and on again by its power switch
        click(view, sourcePower(0));
        int off = -1;
        for (int s : sources)
            off = model.value(sourceEnabledIndex(s)) == 0.0 ? s : off;
        click(view, sourcePower(0));
        return off;
    };
    view.touchPanBegin({cardX(1) + 120.0f, 678.0f});
    view.touchPanMove({cardX(0) + 120.0f, 678.0f});
    view.touchPanEnd({cardX(0) + 120.0f, 678.0f});
    const bool sourcesPanned = sources.size() >= 5 && firstShown() == sources[1];
    view.wheelAcross({cardX(1) + 120.0f, 678.0f}, -2.5f);
    check(sourcesPanned && firstShown() == sources[0],
          "two fingers pan the source cards, and the wheel turned sideways scrolls them");
    // One finger on a card's foot, under its display, pans them too.
    view.touchDown({cardX(1) + 120.0f, 748.0f}, false, kSlop);
    view.touchMove({cardX(0) + 120.0f, 748.0f});
    view.touchUp({cardX(0) + 120.0f, 748.0f});
    const bool onePanned = firstShown() == sources[1];
    view.wheelAcross({cardX(1) + 120.0f, 678.0f}, -2.5f);
    check(onePanned && firstShown() == sources[0], "one finger pans the source cards");

    // Over a dialog, nothing pans.
    click(view, kSettingsButton);
    const bool refused = !view.touchPanBegin(grab);
    view.dialogKey(View::Key::Escape);
    check(refused && !view.dialogOpen(), "two fingers pan nothing over a dialog");

    // The preset browser's list of forty, five rows up.
    FakeController presets;
    View presetView(presets);
    presets.library.clear();
    for (int n = 0; n < 40; ++n)
        presets.library.push_back({"Preset " + std::to_string(n), ""});
    presets.loadPresetAt(0);
    click(presetView, {600.0f, 18.0f});
    const float browserTop = std::round((presetView.height() - 560.0f) * 0.5f);
    auto browserRow = [&](int row) { return Point{600.0f, browserTop + 58.0f + row * 24.0f + 11.0f}; };
    presetView.touchPanBegin(browserRow(10));
    presetView.touchPanMove({600.0f, browserRow(5).y});
    presetView.touchPanEnd({600.0f, browserRow(5).y});
    click(presetView, browserRow(0));
    check(presets.presetIndex() == 5, "two fingers pan the preset browser's list");
    // One finger: dragged up the list it loads nothing, and a tap then loads the preset tapped.
    FakeController browsed;
    View browsedView(browsed);
    browsed.library = presets.library;
    browsed.loadPresetAt(0);
    click(browsedView, {600.0f, 18.0f});
    browsedView.touchDown(browserRow(10), false, kSlop);
    browsedView.touchMove({600.0f, browserRow(8).y});
    browsedView.touchMove({600.0f, browserRow(5).y});
    browsedView.touchUp({600.0f, browserRow(5).y});
    const bool nothingLoaded = browsed.presetIndex() == 0 && browsedView.typing(); // still open, its search typing
    tap(browsedView, browserRow(0));
    check(nothingLoaded && browsed.presetIndex() == 5,
          "one finger pans the preset browser's list without loading what it went down on, and a tap loads one");

    // The effects graph, wider than its box: the wheel sideways to its end, where the Param EQ's node takes a click,
    // and two fingers back to its start.
    FakeController graph;
    View graphView(graph);
    for (int slot = 0; slot < 10; ++slot)
        graph.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Delay);
    graph.values[paramIndex(9, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
    setChain(graph, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    graphView.poll();
    click(graphView, kFxTab);
    click(graphView, rectCenter(detail::fxModeButton(1)));
    const Rect area = detail::fxGraphArea(1.5f);
    const float most = detail::kFxGridLeft + 12.0f * detail::kFxGridCol + detail::kFxEndHalfW + 16.0f - area.w;
    graphView.wheelAcross(area.center(), 100.0f);
    click(graphView, detail::fxGridCell(10, 0, most));
    const bool atEnd = graph.ui.fxSelected == 9;
    graphView.touchPanBegin(area.center());
    graphView.touchPanMove({area.center().x + 2000.0f, area.center().y});
    graphView.touchPanEnd({area.center().x + 2000.0f, area.center().y});
    click(graphView, detail::fxGridCell(1, 0, 0.0f));
    check(most > 0.0f && atEnd && graph.ui.fxSelected == 0,
          "the wheel turned sideways and two fingers scroll the effects graph");
}

// The middle button held and dragged over a panel that scrolls drags what it shows along with the mouse, without
// turning what it went down on: the effects and the source cards across, the MODS view's rows and the preset browser's
// list up and down. Past either end the row stops there, and dragged back it follows again at once. While the left
// button drags a knob, it does nothing.
void testMiddleDrag(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    for (FxType type : {FxType::Delay, FxType::Reverb, FxType::Phaser, FxType::Flanger, FxType::Compressor,
                        FxType::Widener, FxType::Disperser, FxType::FreqShifter})
        addFx(view, type);
    view.wheel({600.0f, 153.0f}, 10.0f, {}); // back to the start
    const std::vector<int> chain = fxChain(model);
    // A card's icon, as `scrolled` places along, switches the effect at chain position `position` off and on again.
    auto fxAt = [&](float scrolled, int position) {
        fxScrolled = scrolled;
        click(view, fxPower(model, position));
        const bool off = model.value(paramIndex(chain[position], FxParam::Enabled)) == 0.0;
        click(view, fxPower(model, position));
        fxScrolled = 0.0f;
        return off;
    };
    const ParamValues before = model.values;
    const Point grab{700.0f, 328.0f}; // on a card's knob
    view.middleDown(grab);
    view.mouseMove({grab.x - kFxStep, grab.y + 10.0f}, {});
    view.mouseMove({grab.x - 2.0f * kFxStep, grab.y + 10.0f}, {});
    const bool fxShot = renderer.save(view, dir / "8d-fx-middle-drag.png");
    const bool panCursor = view.cursor() == CursorKind::Move;
    view.middleUp({grab.x - 2.0f * kFxStep, grab.y + 10.0f});
    const bool untouched = model.values == before;
    check(fxShot && panCursor && untouched && chain.size() == 8 && fxAt(2.0f, 2),
          "the middle button drags the effects along with the mouse, leaving the knob it went down on");
    // Eight cards and the dashed one, six places on screen: the row stops three along, and comes back at once.
    view.middleDown(grab);
    view.mouseMove({grab.x - 20.0f * kFxStep, grab.y}, {});
    view.mouseMove({grab.x - 19.0f * kFxStep, grab.y}, {});
    view.middleUp({grab.x - 19.0f * kFxStep, grab.y});
    check(fxAt(2.0f, 2), "the middle button drags the effects no further than their end, and back from there at once");
    // While the left button drags a knob, the middle one leaves the row where it is.
    const double fm = model.value(paramIndex(GlobalParam::FmAmount));
    view.mouseDown(kFmAmountKnob, {});
    view.middleDown(grab);
    view.mouseMove({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f}, {});
    view.middleUp({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f});
    view.mouseUp({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f}, {});
    check(model.value(paramIndex(GlobalParam::FmAmount)) > fm && fxAt(2.0f, 2),
          "the middle button does nothing while the left button drags a knob");

    // The source cards, a card's width to the left: the second shown takes the first's place.
    for (int n = 0; n < 5; ++n)
        addSource(view, SourceType::Lfo);
    view.wheel({60.0f, 541.0f}, 10.0f, {}); // back to the first card
    const std::vector<int> sources = shown(model);
    view.middleDown({cardX(1.0f) + 120.0f, 678.0f});
    view.mouseMove({cardX(0.0f) + 120.0f, 678.0f}, {});
    view.middleUp({cardX(0.0f) + 120.0f, 678.0f});
    click(view, sourcePower(0));
    const bool cardOff = sources.size() >= 5 && model.value(sourceEnabledIndex(sources[1])) == 0.0;
    click(view, sourcePower(0));
    check(cardOff, "the middle button drags the source cards along with the mouse");

    // The MODS view's thirteen rows (and the add row), two rows up: the top row's x is LFO 1's third modulation's.
    // Dragged far down, back to the top, where the first row's x is LFO 1's first.
    FakeController mods;
    View modsView(mods);
    mods.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    mods.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Env));
    const OpParam targets[] = {OpParam::Ratio,    OpParam::Detune,  OpParam::Transpose, OpParam::Out,
                               OpParam::Phase,    OpParam::Attack, OpParam::Decay,     OpParam::Sustain,
                               OpParam::Release, OpParam::Hold,    OpParam::UnisonDetune};
    for (int k = 0; k < 11; ++k) {
        mods.set(slotTargetIndex(0, k), slotTargetValue(paramIndex(0, targets[k])));
        mods.set(slotAmountIndex(0, k), 0.2);
    }
    mods.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
    mods.set(slotAmountIndex(1, 0), 0.3);
    mods.set(slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, FilterParam::Resonance)));
    mods.set(slotAmountIndex(kMaxSources, 0), 0.35);
    modsView.poll();
    click(modsView, {314.0f, 116.0f}); // the MODS tab
    auto removedOnly = [&](int slot) {
        click(modsView, {1144.0f, 182.0f}); // the top row's x
        bool ok = true;
        for (int k = 0; k < 11; ++k)
            ok = ok && (slotTarget(mods.value(slotTargetIndex(0, k))) < 0) == (k == slot);
        modsView.undo();
        return ok && slotTarget(mods.value(slotTargetIndex(0, slot))) == paramIndex(0, targets[slot]);
    };
    modsView.middleDown({600.0f, 328.0f});
    modsView.mouseMove({610.0f, 328.0f - 60.0f}, {});
    const bool modsShot = renderer.save(modsView, dir / "35p-mods-middle-drag.png");
    modsView.middleUp({610.0f, 328.0f - 60.0f});
    const bool scrolled = removedOnly(2);
    modsView.middleDown({600.0f, 228.0f});
    modsView.mouseMove({600.0f, 228.0f + 400.0f}, {});
    modsView.middleUp({600.0f, 228.0f + 400.0f});
    check(modsShot && scrolled && removedOnly(0), "the middle button drags the MODS view's rows up and down");

    // The preset browser's list of forty, five rows up: the top row is the sixth preset's.
    FakeController presets;
    View presetView(presets);
    presets.library.clear();
    for (int n = 0; n < 40; ++n)
        presets.library.push_back({"Preset " + std::to_string(n), ""});
    presets.loadPresetAt(0); // unchanged since, so picking another doesn't ask first
    click(presetView, {600.0f, 18.0f}); // the preset's name opens the browser
    const float browserTop = std::round((presetView.height() - 560.0f) * 0.5f);
    auto browserRow = [&](int row) { return Point{600.0f, browserTop + 58.0f + row * 24.0f + 11.0f}; };
    presetView.middleDown(browserRow(10));
    presetView.mouseMove({600.0f, browserRow(5).y}, {});
    const bool browserShot = renderer.save(presetView, dir / "22i-preset-middle-drag.png");
    presetView.middleUp({600.0f, browserRow(5).y});
    click(presetView, browserRow(0));
    check(browserShot && presets.presetIndex() == 5, "the middle button drags the preset browser's list up and down");
}

// A modulation's depth handle under the mouse draws its range over the others, and wider: LFO 1's and Env 1's
// ranges on Op A's Phase (set to the middle) overlap above the knob, Env 1's on top, and hovering either handle
// changes what shows there (read off the rendered pixels).
void testModFocus(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    addSource(view, SourceType::Lfo);
    addSource(view, SourceType::Env);
    model.set(paramIndex(0, OpParam::Phase), 0.5);
    drag(view, sourceHandle(0), kOpPhaseKnob);
    drag(view, sourceHandle(1), kOpPhaseKnob);
    auto ring = [&] { // above the knob, where both ranges run
        std::vector<uint32_t> pixels;
        for (float y = 220.0f + kInsDy; y < 235.0f + kInsDy; y += 1.0f)
            for (float x = kOpPhaseKnob.x - 20.0f; x < kOpPhaseKnob.x + 21.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    view.mouseMove({600.0f, 328.0f}, {});
    const bool plainShot = renderer.save(view, dir / "17c-mod-ranges.png");
    const std::vector<uint32_t> plain = ring();
    view.mouseMove({kOpPhaseKnob.x - 5.0f, 257.0f + kInsDy}, {}); // LFO 1's handle, the first under the knob
    const bool lfoShot = renderer.save(view, dir / "17d-mod-range-hovered.png");
    const std::vector<uint32_t> lfo = ring();
    view.mouseMove({kOpPhaseKnob.x + 5.0f, 257.0f + kInsDy}, {}); // Env 1's, the second
    renderer.save(view, dir / "17e-mod-range-hovered.png");
    const std::vector<uint32_t> env = ring();
    check(model.value(slotTargetIndex(0, 0)) == slotTargetValue(paramIndex(0, OpParam::Phase)) &&
              model.value(slotTargetIndex(1, 0)) == slotTargetValue(paramIndex(0, OpParam::Phase)) && plainShot &&
              lfoShot && lfo != plain && env != plain && lfo != env,
          "a hovered depth handle draws its range wider, over the others");
}

// In a theme whose cards go without an outline (Spectrum, Porcelain), a source card outlined only while the mouse is
// over it, and Midnight's outlined at rest (read off the rendered pixels).
void testCardOutline(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    addSource(view, SourceType::Lfo);
    const Rect card = detail::sourceCard(0.0f);
    const Point edge{card.x, card.center().y}, inside{card.x + 4.0f, card.center().y}, away{600.0f, 328.0f};
    auto edgeAt = [&](Point mouse, const char *file) {
        view.mouseMove(mouse, {});
        return renderer.save(view, dir / file) ? renderer.pixel(edge) : 0u;
    };
    const uint32_t midnight = edgeAt(away, "17f-card-outline-midnight.png");
    model.themeName = "Spectrum";
    const uint32_t bare = edgeAt(away, "17g-card-outline-rest.png");
    const uint32_t outlined = edgeAt(inside, "17h-card-outline-hover.png");
    const uint32_t bareAgain = edgeAt(away, "17i-card-outline-left.png");
    // Its dropdowns: the LFO's shape list button filled without an outline, and its list open on a soft shadow.
    click(view, detail::lfoShapeButton(0.0f).center());
    view.mouseMove(away, {});
    const bool listShot = renderer.save(view, dir / "17j-spectrum-list-open.png");
    click(view, away);
    // A node playing unison copies: their outlines peek out beside it, left of its circle (read off the rendered pixels).
    click(view, kRoutingTab);
    const Point node = nodeCenter(model.layout(), 0);
    const Point beside{node.x - detail::kNodeRadius - 9.0f, node.y}; // where a copy's outline runs
    view.mouseMove(away, {});
    const uint32_t single = renderer.save(view, dir / "17k-flat-unison-off.png") ? renderer.pixel(beside) : 0u;
    model.set(paramIndex(0, OpParam::Unison), 4.0);
    view.poll();
    view.mouseMove(away, {});
    const uint32_t stacked = renderer.save(view, dir / "17l-flat-unison.png") ? renderer.pixel(beside) : 0u;
    model.set(paramIndex(0, OpParam::Unison), 1.0);
    view.poll();
    click(view, kFxTab);
    model.themeName = "Midnight";
    check(single != 0u && stacked != 0u && single != stacked, "a Spectrum node's unison copies show beside it");
    check(bare != outlined && bare == bareAgain && bare != 0u && midnight != 0u && listShot,
          "a Spectrum card is outlined only while the mouse is over it");
}

// A modulation ring whose reach runs past its knob's end, cut off there, ends in a line across it: at the knob's end
// angle something is drawn further out than the ring reaches inside its range, while a reach that stays inside draws
// nothing there past the knob's track (read off the rendered pixels, along rays out from Op A's Phase knob).
void testModEndMarks(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    addSource(view, SourceType::Lfo);
    model.set(paramIndex(0, OpParam::Phase), 0.5);
    view.poll();
    drag(view, sourceHandle(0), kOpPhaseKnob);
    const bool assigned = slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Phase);
    const Color panel = detail::kThemes[detail::themeIndex("Midnight")].panel;
    const auto bg = static_cast<uint32_t>(panel.r * 255.0f + 0.5f) << 16 |
                    static_cast<uint32_t>(panel.g * 255.0f + 0.5f) << 8 | static_cast<uint32_t>(panel.b * 255.0f + 0.5f);
    auto drawn = [&](uint32_t p) {
        for (int shift = 0; shift < 24; shift += 8)
            if (std::abs(static_cast<int>((p >> shift) & 0xFF) - static_cast<int>((bg >> shift) & 0xFF)) > 24)
                return true;
        return false;
    };
    // How far out from the knob's middle something is drawn along the ray at `angle`.
    auto farthest = [&](float angle) {
        float last = 0.0f;
        for (float d = 8.0f; d <= 26.0f; d += 0.5f)
            if (drawn(renderer.pixel(Canvas::onCircle(kOpPhaseKnob, d, angle))))
                last = d;
        return last;
    };
    auto at = [&](double phase, const char *file) {
        model.set(paramIndex(0, OpParam::Phase), phase);
        view.poll();
        view.mouseMove({600.0f, 328.0f}, {});
        return renderer.save(view, dir / file);
    };
    const float top = detail::knobAngle(1.0), inside = detail::knobAngle(0.9);
    const bool cutShot = at(0.97, "17q-mod-end-mark.png");
    const float cutEnd = farthest(top), cutInside = farthest(inside);
    const bool freeShot = at(0.5, "17r-mod-no-end-mark.png");
    const float freeEnd = farthest(top), freeRing = farthest(detail::knobAngle(0.6));
    std::printf("      end mark: cut off %.1f px out at the end, %.1f inside; within range %.1f at the end, ring %.1f\n",
                cutEnd, cutInside, freeEnd, freeRing);
    check(assigned && cutShot && freeShot && cutEnd > cutInside + 1.0f && freeEnd + 2.0f < freeRing,
          "a modulation ring cut off at its knob's end ends in a line across it, and one within range doesn't");
}

// In a theme that colors knobs by their card (Spectrum, Porcelain), a modulated knob's arc keeps its color, and a
// modulation ring whose source's color is like it is drawn lighter (on a dark theme) to stand apart: Spectrum's LFO 1,
// a blue near its accent, on an operator's knob, in the accent. Midnight draws the ring in the LFO's own color (read off
// the rendered pixels, the upper half of Op A's Phase knob, set to its middle, where its arc and the ring run).
void testFlatModulatedKnob(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    addSource(view, SourceType::Lfo);
    model.set(paramIndex(0, OpParam::Phase), 0.5);
    view.poll();
    const Point away{600.0f, 328.0f};
    // The pixels near each of two colors, in the knob's upper half.
    auto count = [&](const char *file, uint32_t accent, uint32_t text) {
        view.mouseMove(away, {});
        std::pair<int, int> counts{0, 0};
        if (!renderer.save(view, dir / file))
            return counts;
        auto alike = [](uint32_t a, uint32_t b) {
            for (int shift = 0; shift < 24; shift += 8)
                if (std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF)) > 40)
                    return false;
            return true;
        };
        for (float y = kOpPhaseKnob.y - 22.0f; y < kOpPhaseKnob.y - 8.0f; y += 1.0f)
            for (float x = kOpPhaseKnob.x - 22.0f; x < kOpPhaseKnob.x + 22.0f; x += 1.0f) {
                const uint32_t p = renderer.pixel({x, y});
                counts.first += alike(p, accent);
                counts.second += alike(p, text);
            }
        return counts;
    };
    auto rgb = [](const Color &c) {
        return static_cast<uint32_t>(c.r * 255.0f + 0.5f) << 16 | static_cast<uint32_t>(c.g * 255.0f + 0.5f) << 8 |
               static_cast<uint32_t>(c.b * 255.0f + 0.5f);
    };
    const detail::Palette &spectrum = detail::kThemes[detail::themeIndex("Spectrum")],
                          &midnight = detail::kThemes[detail::themeIndex("Midnight")];
    model.themeName = "Spectrum";
    // A bipolar LFO's ring is drawn at 60 %, over the inspector's panel.
    auto ringOn = [&](const detail::Palette &theme, Color color) { return rgb(mix(theme.panel, color, 0.6f)); };
    const Color lfo = spectrum.sources[0];
    const uint32_t raw = ringOn(spectrum, lfo);
    const uint32_t lifted = ringOn(spectrum, mix(lfo, Color{1.0f, 1.0f, 1.0f, 1.0f}, 0.5f)); // told from the accent
    const auto plain = count("17m-flat-knob.png", rgb(spectrum.accent), raw);
    drag(view, sourceHandle(0), kOpPhaseKnob);
    const bool assigned = slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Phase);
    const auto modulated = count("17n-flat-knob-modulated.png", rgb(spectrum.accent), raw);
    const auto ring = count("17n-flat-knob-modulated.png", lifted, rgb(spectrum.text));
    model.themeName = "Midnight";
    const auto midnightModulated =
        count("17o-midnight-knob-modulated.png", rgb(midnight.accent), ringOn(midnight, midnight.sources[0]));
    // Where a macro takes it with no note playing, its dot on the arc.
    model.themeName = "Spectrum";
    const float strip = view.height() - 179.0f;
    drag(view, {188.0f - 32.0f, strip}, kOpPhaseKnob); // Macro 1's handle
    model.set(paramIndex(0, MacroParam::Value), 1.0);
    view.poll();
    view.mouseMove(away, {});
    const bool macroShot = renderer.save(view, dir / "17p-flat-knob-macro.png");
    std::printf("      Phase's arc: Spectrum %d accent, modulated %d accent / %d LFO / %d lighter ring; Midnight "
                "modulated %d accent / %d LFO\n",
                plain.first, modulated.first, modulated.second, ring.first, midnightModulated.first,
                midnightModulated.second);
    check(assigned && plain.first > 30 && modulated.first > 30 && ring.first > 30 && 2 * modulated.second < ring.first &&
              midnightModulated.first > 30 && midnightModulated.second > 10 && macroShot,
          "in Spectrum a modulated knob keeps its arc's color and a ring of a like color is drawn lighter, and in "
          "Midnight the ring keeps its source's color");
}

// In the flat themes (Spectrum, Porcelain): no two sources in one color, and no effect in the dim text's color (which
// would make one that is on look switched off); and an aux that is set a chip tinted with its source's color, which
// stands out from its MODS row where + Aux is a filled pill (read off the rendered pixels).
void testFlatThemes(const std::filesystem::path &dir, Renderer &renderer) {
    auto same = [](const Color &a, const Color &b) { return a.r == b.r && a.g == b.g && a.b == b.b; };
    bool distinct = true, lively = true;
    for (const detail::Palette &theme : detail::kThemes) {
        if (!theme.flat)
            continue;
        for (int i = 0; i < kAllSources; ++i)
            for (int j = i + 1; j < kAllSources; ++j)
                if (same(theme.sources[i], theme.sources[j])) {
                    std::printf("      %s: sources %d and %d share a color\n", theme.name, i, j);
                    distinct = false;
                }
        for (int type = 0; type < static_cast<int>(FxType::Count); ++type)
            if (same(theme.fxTypes[type], theme.dim)) {
                std::printf("      %s: effect type %d is in the dim text's color\n", theme.name, type);
                lively = false;
            }
    }
    check(distinct && lively, "a flat theme gives every source a color of its own, and no effect the dim text's");

    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Ratio)));
    model.set(slotAmountIndex(0, 0), 0.4);
    model.set(slotAuxIndex(0, 0), slotAuxValue(kMaxSources + 1)); // Macro 2
    view.poll();
    click(view, {314.0f, 116.0f}); // the MODS tab
    const Rect row = detail::modsRow(0.0f), cell = detail::modsCell(row, detail::kModsAux);
    const float y = row.center().y + 6.0f; // under the name's letters
    // How much of the chip's first 120 px is the row's own fill, its most common color along it.
    auto plain = [&](const char *theme, const char *file) {
        model.themeName = theme;
        view.mouseMove({600.0f, 600.0f}, {});
        if (!renderer.save(view, dir / file))
            return 1.0f;
        std::map<uint32_t, int> seen;
        for (float x = row.x + 2.0f; x < row.right() - 2.0f; x += 1.0f)
            ++seen[renderer.pixel({x, y})];
        const uint32_t fill =
            std::max_element(seen.begin(), seen.end(), [](auto &a, auto &b) { return a.second < b.second; })->first;
        int matching = 0, all = 0;
        for (float x = cell.x + 4.0f; x < cell.x + 124.0f; x += 1.0f, ++all)
            matching += renderer.pixel({x, y}) == fill;
        return static_cast<float>(matching) / static_cast<float>(all);
    };
    const float spectrum = plain("Spectrum", "17q-flat-aux-chip.png");
    const float porcelain = plain("Porcelain", "17r-flat-aux-chip-light.png");
    model.themeName = "Midnight";
    std::printf("      an aux chip's share in its row's own fill: Spectrum %.2f, Porcelain %.2f\n", spectrum, porcelain);
    check(model.value(slotAuxIndex(0, 0)) == slotAuxValue(kMaxSources + 1) && spectrum < 0.2f && porcelain < 0.2f,
          "in a flat theme an aux that is set shows as a chip tinted with its source's color");
}

// An operator's Ratio, a 7-segment readout: each digit dragged on its own, a step of its place every 6 px, carrying
// into the next and stopping at 64.00; the wheel stepping the digit under the mouse; a double-click rounding to the
// digit; the hovered digit lit behind (read off the rendered pixels); a typed fraction; and a source's handle dropped
// on it modulating it, its reach drawn along the track under the digits (read off the rendered pixels).
// The title bar's tempo, for a host that lets the editor set it (the browser demo, fmvibe-uihost): none for a plugin
// host; a field between the name and the preset bar, dragged a BPM every 2 px (with Shift a tenth), scrolled a BPM a
// notch onto whole ones (with Shift a tenth), typed into after a double-click or Ctrl+Click, held to 20-999 BPM, and
// none of it touching the patch or its undo history.
void testTempo(const std::filesystem::path &dir, Renderer &renderer) {
    const Rect titleBar{0.0f, 0.0f, 820.0f, detail::kTitleBarH};
    const Point box = detail::kTempoBox.center();
    // Inside the field, clear of the metronome and the number (whatever the font), and clear of its border.
    const Point inside{detail::kTempoBox.right() - 5.0f, detail::kTempoBox.center().y};
    const Point away{600.0f, 328.0f};
    uint32_t bare = 0;
    {
        FakeController model; // a plugin host, whose tempo is its own
        View view(model);
        view.mouseMove(away, {});
        const bool shot = renderer.save(view, dir / "43-tempo-none.png", &titleBar);
        bare = renderer.pixel(inside);
        drag(view, box, {box.x, box.y - 40.0f});
        view.wheel(box, 1.0f, {});
        doubleClick(view, box);
        check(shot && model.tempoSets == 0 && !view.typing(), "a plugin host's editor shows no tempo");
    }
    FakeController model;
    model.offersTempo = true;
    View view(model);
    const ParamValues patch = model.values;
    view.mouseMove(away, {});
    const bool shown = renderer.save(view, dir / "43a-tempo.png", &titleBar) && renderer.pixel(inside) != bare;
    view.mouseMove(box, {});
    check(shown && renderer.save(view, dir / "43b-tempo-hover.png", &titleBar),
          "a host that lets the editor set its tempo gets a field for it in the title bar (read off the rendered pixels)");

    drag(view, box, {box.x, box.y - 20.0f});
    const bool dragged = model.bpm == 150.0;
    view.mouseDown(box, {true, false}); // with Shift, a tenth of a BPM every 2 px
    view.mouseMove({box.x, box.y - 10.0f}, {true, false});
    view.mouseUp({box.x, box.y - 10.0f}, {true, false});
    const bool fine = std::abs(model.bpm - 150.5) < 1e-9;
    click(view, box); // a click leaves it be
    const bool clicked = std::abs(model.bpm - 150.5) < 1e-9;
    check(dragged && fine && clicked, "the tempo is dragged a BPM every 2 px, a tenth of one with Shift");
    view.wheel(box, 1.0f, {});
    const bool up = model.bpm == 151.0; // onto whole BPM
    view.wheel(box, -2.0f, {});
    const bool down = model.bpm == 149.0;
    view.wheel(box, 1.0f, {true, false});
    check(up && down && std::abs(model.bpm - 149.1) < 1e-9, "the wheel steps the tempo a BPM a notch, a tenth with Shift");
    drag(view, box, {box.x, box.y - 4000.0f});
    const bool top = model.bpm == 999.0;
    drag(view, box, {box.x, box.y + 4000.0f});
    check(top && model.bpm == 20.0, "the tempo stops at 999 and 20 BPM");

    doubleClick(view, box);
    const bool typing = view.typing();
    type(view, "92.5");
    view.mouseMove(away, {});
    const bool typingShot = renderer.save(view, dir / "43c-tempo-typing.png", &titleBar);
    view.typeKey(View::Key::Enter);
    const bool typed = !view.typing() && model.bpm == 92.5;
    view.mouseMove(away, {});
    const bool typedShot = renderer.save(view, dir / "43d-tempo-typed.png", &titleBar);
    check(typing && typingShot && typed && typedShot, "a double-click on the tempo types one");
    click(view, box, {false, true}); // Ctrl+Click types too, a unit after the number allowed
    type(view, "160 BPM");
    view.typeKey(View::Key::Enter);
    const bool ctrlTyped = model.bpm == 160.0;
    doubleClick(view, box);
    type(view, "fast");
    view.typeKey(View::Key::Enter);
    doubleClick(view, box);
    type(view, "60");
    view.typeKey(View::Key::Escape);
    const bool kept = model.bpm == 160.0;
    doubleClick(view, box);
    type(view, "5000");
    view.typeKey(View::Key::Enter);
    const bool held = model.bpm == 999.0;
    doubleClick(view, box);
    type(view, "100");
    click(view, away); // a click elsewhere takes the value
    check(ctrlTyped && kept && held && model.bpm == 100.0 && !view.typing(),
          "a typed tempo takes a unit after it and is held to its range; Escape, or text that isn't one, leave it");
    check(model.values == patch && model.nextUndo() == nullptr && model.openEdits == 0,
          "the tempo leaves the patch and its undo history alone");
}

void testRatioDisplay(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const int ratio = paramIndex(0, OpParam::Ratio);
    auto shown = [&] { return ratioHundredths(model.value(ratio)); };
    const bool fresh = shown() == 100;
    drag(view, kRatioOnes, {kRatioOnes.x, kRatioOnes.y - 12.0f}); // two steps of 1
    const bool ones = shown() == 300 && model.nextUndo() != nullptr && model.openEdits == 0;
    drag(view, kRatioHundredths, {kRatioHundredths.x, kRatioHundredths.y - 30.0f}); // five of 0.01
    const bool hundredths = shown() == 305;
    drag(view, kRatioTenths, {kRatioTenths.x, kRatioTenths.y + 60.0f}); // ten of 0.1 down, through the ones
    const bool carried = shown() == 205;
    drag(view, kRatioTens, {kRatioTens.x, kRatioTens.y - 600.0f}); // far past the top
    const bool top = shown() == 6400;
    view.mouseMove({600.0f, 328.0f}, {});
    const bool topShot = renderer.save(view, dir / "3g-ratio-top.png");
    view.undo();
    std::printf("      Ratio dragged: 1.00 to %.2f, 3.05, %.2f (carried), 64.00 at the top; undone %.2f\n",
                ones ? 3.0 : -1.0, carried ? 2.05 : -1.0, shown() / 100.0);
    check(fresh && ones && hundredths && carried && top && topShot && shown() == 205,
          "each digit of the Ratio readout drags by its own place, carrying, held to 64.00, each drag an undo step");

    for (int n = 0; n < 3; ++n)
        view.wheel(kRatioTenths, 1.0f, {});
    view.wheel(kRatioHundredths, -1.0f, {});
    const bool wheeled = shown() == 234;
    doubleClick(view, kRatioOnes);
    const bool rounded = shown() == 100; // the ones and below back to 1.00's: 2.34 becomes 1.00
    doubleClick(view, kRatioTenths); // 1.00 already has its tenths and hundredths at default: no change
    check(wheeled && rounded && shown() == 100,
          "the wheel steps the Ratio digit under the mouse, and a double-click resets it and the digits below");

    // The digit under the mouse is lit behind: a pixel in its corner, clear of its segments.
    const Point corner{kRatioTenths.x + 5.5f, kRatioTenths.y - 9.0f};
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "3g-ratio.png");
    const uint32_t idle = renderer.pixel(corner);
    view.mouseMove(kRatioTenths, {});
    const bool hoverShot = renderer.save(view, dir / "3g-ratio-hover.png");
    const uint32_t lit = renderer.pixel(corner);
    view.mouseMove(kRatioOnes, {});
    renderer.save(view, dir / "3g-ratio-hover-ones.png");
    check(hoverShot && lit != idle && renderer.pixel(corner) == idle, "the Ratio digit under the mouse is lit behind");

    view.mouseDown(kRatioOnes, {false, true}); // Ctrl+Click types a value, a fraction too
    const bool typing = view.typing();
    type(view, "7/2");
    view.mouseMove({600.0f, 328.0f}, {});
    const bool typingShot = renderer.save(view, dir / "3g-ratio-typing.png");
    view.typeKey(View::Key::Enter);
    check(typing && typingShot && shown() == 350, "a fraction typed into the Ratio readout sets it");

    // A source's handle dropped on the readout modulates it, its reach drawn in its color along the track.
    addSource(view, SourceType::Lfo);
    const Point track = ins(911.0f, 227.0f); // on the track over the digits (from x 910), a little below 3.50 (at x 913)
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "3g-ratio-plain.png");
    const uint32_t bare = renderer.pixel(track);
    drag(view, sourceHandle(0), kRatioOnes);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool modShot = renderer.save(view, dir / "3g-ratio-modulated.png");
    check(slotTarget(model.value(slotTargetIndex(0, 0))) == ratio && model.value(slotAmountIndex(0, 0)) != 0.0 &&
              modShot && renderer.pixel(track) != bare,
          "a source's handle dropped on the Ratio readout modulates it, its reach drawn along the track");

    // The track is a straight line to 64: set there, the ratio's tick is at its right end (x 979), and the modulation
    // going on past 64 has an arrow after it (x 980 to 984) (read off the rendered pixels).
    const Point end = ins(978.5f, 225.0f), arrow = ins(981.5f, 227.0f);
    const uint32_t endBefore = renderer.pixel(end), arrowBefore = renderer.pixel(arrow);
    model.set(ratio, 1.0);
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    const bool topModShot = renderer.save(view, dir / "3g-ratio-past-64.png");
    check(topModShot && shown() == 6400 && renderer.pixel(end) != endBefore && renderer.pixel(arrow) != arrowBefore,
          "the Ratio track ends at 64, where its tick then sits, with an arrow for modulation going on past it");
    model.set(ratio, normFromRatio(3.5));
    view.poll();

    // Its depth handle drags on Ratio's log curve, an octave of depth (a seventh of its range) every 60 px.
    const double depthBefore = model.value(slotAmountIndex(0, 0));
    drag(view, kRatioDot, {kRatioDot.x, kRatioDot.y - 60.0f});
    const double octaves = (model.value(slotAmountIndex(0, 0)) - depthBefore) * 7.0;
    view.undo();
    std::printf("      the Ratio's depth handle dragged up 60 px: %.3f octaves deeper\n", octaves);
    check(std::abs(octaves - 1.0) < 1e-6 && model.value(slotAmountIndex(0, 0)) == depthBefore,
          "a depth handle on the Ratio readout drags an octave of depth every 60 px");

    // Its depth handles, 9 px apart along its bottom edge from x 913, hold eight before a pill: the eighth at x 976,
    // and with a ninth, six handles and the pill (x 966 to 978), which opens their list under it (read off the
    // rendered pixels, inside the envelope's display, clear of the tabs above it, whose width follows the font).
    const Point eighth = ins(976.0f, 257.0f), pill = ins(971.7f, 257.0f), under = ins(960.0f, 320.0f);
    const uint32_t noEighth = renderer.pixel(eighth);
    for (int place = 1; place < 9; ++place) {
        model.set(paramIndex(place, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        if (place < 8) {
            model.set(slotTargetIndex(place, 0), slotTargetValue(ratio));
            model.set(slotAmountIndex(place, 0), 0.05);
        }
    }
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    const bool eightShot = renderer.save(view, dir / "3g-ratio-eight.png");
    const bool eightFit = renderer.pixel(eighth) != noEighth;
    model.set(slotTargetIndex(8, 0), slotTargetValue(ratio));
    model.set(slotAmountIndex(8, 0), 0.05);
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    const bool nineShot = renderer.save(view, dir / "3g-ratio-nine.png");
    const uint32_t closedList = renderer.pixel(under);
    click(view, pill);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool listShot = renderer.save(view, dir / "3g-ratio-pill-list.png");
    const bool opened = renderer.pixel(under) != closedList;
    click(view, {600.0f, 328.0f});
    check(eightShot && nineShot && listShot && eightFit && opened,
          "the Ratio readout holds eight depth handles, and a ninth puts them in a pill");
}

// The examples that use the most of the synth, as the preset library writes them: Init, then their changes,
// with their own node places and their name in the preset bar. `prefix` names the images: its routing, matrix
// and effects.
// Every factory preset's routing view, in Spectrum, into presets/: none with a node on another, or a wire, a line to
// OUT or a label over a node it doesn't join (as fmvibe-presettool --arrange lays them out).
void renderFactoryPresets(const std::filesystem::path &dir, Renderer &renderer) {
    std::filesystem::create_directories(dir / "presets");
    int clear = 0;
    for (const FactoryPreset &preset : factoryPresets()) {
        FakeController model;
        model.themeName = "Spectrum";
        model.glowLevel = 0.5;
        View view(model);
        if (!loadFactoryPreset(preset, model.values, model.ui)) {
            check(false, ("load the factory preset " + preset.name).c_str());
            continue;
        }
        model.ui.view = RoutingView::Graph; // its graph, whichever view it was saved in
        view.poll();
        view.mouseMove({600.0f, 600.0f}, {});
        renderer.save(view, dir / "presets" / (preset.name + ".png"));
        const detail::ArrangeScore score = detail::arrangeScore(model.values, model.ui);
        if (score.clashes == 0)
            ++clear;
        else
            std::printf("  %s: %d clashes in its routing graph\n", preset.name.c_str(), score.clashes);
    }
    check(clear == static_cast<int>(factoryPresets().size()),
          "every factory preset's routing graph keeps its nodes, wires and labels clear of each other");
}

void renderExample(const std::filesystem::path &dir, Renderer &renderer, const std::string &name,
                   const std::string &prefix, size_t effects, const std::string &theme = "", bool playing = false) {
    FakeController model;
    if (!theme.empty())
        model.themeName = theme;
    model.glowLevel = 0.5; // as a new user's settings have it
    View view(model);
    const std::vector<FactoryPreset> &presets = factoryPresets();
    const auto example =
        std::find_if(presets.begin(), presets.end(), [&](const FactoryPreset &p) { return p.name == name; });
    if (example == presets.end() || !loadFactoryPreset(*example, model.values, model.ui)) {
        check(false, ("the examples include " + name).c_str());
        return;
    }
    model.library = {{name, "Factory"}}; // the preset bar shows its name
    model.setPreset(std::wstring(name.begin(), name.end()));
    view.poll();
    if (playing) { // an A3 held, its operators sounding and going into the effects, the glows risen to it
        model.live.voices = 1;
        model.live.notePitch = 57.0f;
        model.live.noteVelocity = 0.8f;
        model.live.keys[57 / 32] |= 1u << (57 % 32);
        model.live.opLevels.fill(0.9f);
        model.live.opEnvLevels.fill(0.9f);
        model.live.opEnvPos.fill(-1.0f); // no dot along the envelopes, as if between snapshots of them
        model.live.envPos.fill(-1.0f);
        model.live.lfoPhases.fill(0.3f);
        model.live.fxPortLevels[kFxPortIn] = 0.25f;
        model.live.outPeak = {0.5f, 0.5f};
        for (int i = 0; i < 120; ++i)
            view.poll();
    }
    view.mouseMove({600.0f, 328.0f}, {});
    bool shots = renderer.save(view, dir / (prefix + "-routing.png"));
    click(view, kMatrixTab);
    view.mouseMove({600.0f, 328.0f}, {});
    shots = shots && renderer.save(view, dir / (prefix + "-matrix.png"));
    click(view, kFxTab);
    view.mouseMove({600.0f, 328.0f}, {});
    shots = shots && renderer.save(view, dir / (prefix + "-fx.png"));
    // Its effects' panel alone, as a chain and then as a graph, or as a graph only when it comes as one (the
    // README's, see release.ps1).
    const Rect panel{detail::kFxPanel.x - 8.0f, detail::kFxPanel.y - 1.0f, detail::kFxPanel.w + 16.0f,
                     detail::kFxPanel.h + 9.0f}; // not the tabs above it
    if (!model.ui.fxGraphView) {
        shots = shots && renderer.save(view, dir / (prefix + "-fx-chain-panel.png"), &panel);
        const Rect graphSwitch = detail::fxModeButton(1);
        click(view, {graphSwitch.x + graphSwitch.w * 0.5f, graphSwitch.y + graphSwitch.h * 0.5f});
    }
    view.mouseMove({600.0f, 600.0f}, {});
    shots = shots && model.ui.fxGraphView &&
            renderer.save(view, dir / (prefix + "-fx-graph-panel.png"), &panel);
    if (!theme.empty()) { // in a theme other than the checks' Midnight, its effects as a row and its MODS view too
        click(view, detail::fxModeButton(0).center());
        view.mouseMove({600.0f, 600.0f}, {});
        shots = shots && renderer.save(view, dir / (prefix + "-fx-row.png"));
        click(view, {314.0f, 116.0f});
        view.mouseMove({600.0f, 600.0f}, {});
        shots = shots && renderer.save(view, dir / (prefix + "-mods.png"));
    }
    check(shots && fxChain(model).size() == effects,
          ("render " + name + "'s routing, matrix and " + std::to_string(effects) +
           " effects, and its effects' panel as a chain and as a graph")
              .c_str());
}

// The chorus: added from the list of effects, under MODULATION after the flanger, with three voices; its voices picked
// under its display (2, 3, 4 and 6 as one choice in its middle, a click on the one lit leaving it), each an undo step,
// its display drawing a lane a voice (read off the rendered pixels); and its Rate and Spread knobs turning.
void testChorus(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    addFx(view, FxType::Chorus);
    auto at = [&](FxParam f) { return paramIndex(0, f); };
    check(model.value(at(FxParam::Type)) == static_cast<double>(FxType::Chorus) &&
              detail::fxOfferedPlace(FxType::Chorus) == detail::fxOfferedPlace(FxType::Flanger) + 1 &&
              model.value(at(FxParam::ChorusVoices)) == 3.0,
          "the list of effects adds a chorus, after the flanger, with three voices");
    const Rect display = detail::fxDisplayIn(detail::fxCard(0.0f), FxType::Chorus);
    auto voiceButton = [&](int voices) {
        int choice = 0;
        while (kChorusVoiceChoices[choice] != voices)
            ++choice;
        return rectCenter(detail::chorusVoicesButton(display, choice));
    };
    const int voices = at(FxParam::ChorusVoices);
    click(view, voiceButton(2));
    const bool two = model.value(voices) == 2.0;
    click(view, voiceButton(2));
    const bool stays = model.value(voices) == 2.0;
    click(view, voiceButton(6));
    const bool six = model.value(voices) == 6.0;
    view.undo();
    const bool undone = model.value(voices) == 2.0;
    view.redo();
    check(two && stays && six && undone && model.value(voices) == 6.0,
          "the chorus's 2, 3, 4 and 6 under its display pick its voices, each an undo step");

    // A lane a voice, held still (no audio running, so no dots), at full Depth, read down a column where six voices'
    // lanes are furthest apart: six lanes there, and two voices' two, the six's that are clear of them gone.
    model.set(at(FxParam::ChorusDepth), 1.0);
    model.live.age = 1.0f;
    view.poll();
    const Point away{600.0f, 628.0f};
    view.mouseMove(away, {});
    const Rect lanes = detail::chorusLanes(display);
    auto laneYs = [&](int count, double cycle) {
        std::vector<float> ys;
        for (int k = 0; k < count; ++k)
            ys.push_back(detail::chorusLaneY(lanes, k, count, 1.0, cycle));
        return ys;
    };
    auto gapOf = [](std::vector<float> ys) {
        std::sort(ys.begin(), ys.end());
        float gap = 1e9f;
        for (size_t i = 1; i < ys.size(); ++i)
            gap = std::min(gap, ys[i] - ys[i - 1]);
        return gap;
    };
    double cycle = 0.0;
    for (int i = 1; i < 100; ++i)
        if (gapOf(laneYs(6, i / 100.0)) > gapOf(laneYs(6, cycle)))
            cycle = i / 100.0;
    const std::vector<float> sixYs = laneYs(6, cycle), twoYs = laneYs(2, cycle);
    const float x = lanes.x + static_cast<float>(cycle) * lanes.w;
    auto clearOf = [](float y, const std::vector<float> &ys) {
        float d = 1e9f;
        for (float lane : ys)
            d = std::min(d, std::abs(y - lane));
        return d;
    };
    float clearY = lanes.y + 2.0f; // down the column, the furthest from every lane and the middle line
    for (float y = lanes.y + 2.0f; y < lanes.bottom() - 2.0f; y += 0.5f) {
        auto room = [&](float at) {
            return std::min({clearOf(at, sixYs), clearOf(at, twoYs), std::abs(at - lanes.center().y)});
        };
        if (room(y) > room(clearY))
            clearY = y;
    }
    auto lit = [&](float y) { // a lane across `y`, within a pixel
        const uint32_t clear = renderer.pixel({x, clearY});
        for (float dy : {-1.0f, 0.0f, 1.0f})
            if (renderer.pixel({x, y + dy}) != clear)
                return true;
        return false;
    };
    const bool sixShot = renderer.save(view, dir / "27f-chorus-six.png");
    bool sixLanes = gapOf(sixYs) >= 4.0f;
    for (float y : sixYs)
        sixLanes = sixLanes && lit(y);
    click(view, voiceButton(2));
    view.mouseMove(away, {});
    const bool twoShot = renderer.save(view, dir / "27f-chorus-two.png");
    bool twoLanes = true;
    int gone = 0;
    for (float y : twoYs)
        twoLanes = twoLanes && lit(y);
    for (float y : sixYs)
        if (clearOf(y, twoYs) > 4.0f) {
            twoLanes = twoLanes && !lit(y);
            ++gone;
        }
    std::printf("      chorus lanes down the column %.2f of a cycle in: six %.1f px apart at the least, %d clear of two's\n",
                cycle, gapOf(sixYs), gone);
    check(sixShot && twoShot && sixLanes && twoLanes && gone >= 2,
          "the chorus's display draws a lane a voice, each at its place and a share of a cycle apart (read off the "
          "rendered pixels)");

    // Rate, the first knob of the first row, turned up; Spread, the second of the second row, wide to begin with,
    // turned down.
    const double rateBefore = model.value(at(FxParam::ChorusRate));
    const Point rateKnob = fxKnob(model, 0, 0, 3, 206.0f);
    drag(view, rateKnob, {rateKnob.x, rateKnob.y - 20.0f});
    const Point spreadKnob = fxKnob(model, 0, 1, 3, 266.0f);
    drag(view, spreadKnob, {spreadKnob.x, spreadKnob.y + 20.0f});
    check(model.value(at(FxParam::ChorusRate)) > rateBefore && model.value(at(FxParam::ChorusSpread)) < 1.0,
          "the chorus's Rate and Spread knobs turn");

    // Mid-sweep, a dot on each lane where it is now.
    model.set(at(FxParam::ChorusVoices), 3.0);
    model.live.age = 0.01f;
    model.live.fxPhases[0] = 0.3f;
    view.poll();
    view.mouseMove(away, {});
    const bool midnight = renderer.save(view, dir / "27f-chorus.png");
    model.themeName = "Spectrum"; // and in the theme it starts in
    view.mouseMove({601.0f, 628.0f}, {});
    check(midnight && renderer.save(view, dir / "27f-chorus-spectrum.png"),
          "render a chorus mid-sweep, three voices, in Midnight and Spectrum");
}

// A stereo phaser and a flanger mid-sweep (their LFOs where the audio left them), a phase disperser and a filter,
// each with its output's spectrum behind its curve, and a widener, half of it in the middle.
void renderNewEffects(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    setChain(model, {0, 1, 2, 3, 4});
    const FxType types[] = {FxType::Phaser, FxType::Flanger, FxType::Disperser, FxType::Filter, FxType::Widener};
    for (int slot = 0; slot < 5; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(types[slot]);
    model.values[paramIndex(0, FxParam::PhaserStereo)] = 1.0;
    model.values[paramIndex(1, FxParam::FlangerRate)] = 0.0; // held still
    model.values[paramIndex(4, FxParam::WidenerWidth)] = 0.5;
    model.live.age = 0.01f;
    model.live.fxPhases = {0.1f, 0.4f, 0.0f, 0.0f, 0.3f};
    for (int band = 0; band < kSpectrumBands; ++band) {
        model.live.fxSpectrum[2][band] = 0.2f * std::exp(-std::abs(band - 50) / 16.0f); // behind the disperser's delay
        model.live.fxSpectrum[3][band] = 0.3f * std::exp(-std::abs(band - 40) / 12.0f);
    }
    view.poll();
    click(view, kFxTab);
    // The widener's knobs: Size, Width, High Pass and Mix.
    const int highPass = paramIndex(4, FxParam::WidenerHighPass);
    const double before = model.value(highPass);
    drag(view, fxKnob(model, 4, 2, 4), {fxKnob(model, 4, 2, 4).x, fxKnob(model, 4, 2, 4).y - 20.0f});
    const bool turned = model.value(highPass) > before;
    // The phaser's SYNC, the delay's metronome at the left under its display, turns its Rate knob into a note
    // value's.
    click(view, {fxCardX(model, 0) + 31.0f, 180.0f + 171.0f});
    const int division = paramIndex(0, FxParam::PhaserDivision), rate = paramIndex(0, FxParam::PhaserRate);
    const double divisionBefore = model.value(division), rateBefore = model.value(rate);
    drag(view, fxKnob(model, 0, 0, 3, 206.0f), {fxKnob(model, 0, 0, 3, 206.0f).x, fxKnob(model, 0, 0, 3, 206.0f).y - 20.0f});
    check(model.value(paramIndex(0, FxParam::PhaserSync)) == 1.0 && model.value(division) != divisionBefore &&
              model.value(rate) == rateBefore,
          "the phaser's SYNC turns its Rate knob into a note value");
    // The flanger's STEREO, in the middle under its display like the phaser's, is on to begin with.
    const int flangerStereo = paramIndex(1, FxParam::FlangerStereo);
    const Point stereo{fxCardX(model, 1) + fxCardW(model, 1) * 0.5f, 180.0f + 171.0f};
    click(view, stereo);
    const bool flangerMono = model.value(flangerStereo) == 0.0;
    click(view, stereo);
    check(flangerMono && model.value(flangerStereo) == 1.0, "the flanger's STEREO switches its sides apart and together");
    // The polarity, + and - at the right under the display: - turns the flanger's wet sound and feedback over, + back,
    // and a click on the side lit leaves it. Its - is left on for the render, its curve's notches where the peaks were.
    const int flangerNegative = paramIndex(1, FxParam::FlangerNegative);
    const float polarityRight = fxCardX(model, 1) + fxCardW(model, 1) - 14.0f;
    const Point plus{polarityRight - 30.0f, 180.0f + 171.0f}, minus{polarityRight - 10.0f, 180.0f + 171.0f};
    const bool startsPositive = model.value(flangerNegative) == 0.0;
    click(view, minus);
    const bool negated = model.value(flangerNegative) == 1.0;
    click(view, minus);
    const bool stays = model.value(flangerNegative) == 1.0;
    click(view, plus);
    const bool back = model.value(flangerNegative) == 0.0;
    click(view, minus);
    check(startsPositive && negated && stays && back && model.value(flangerNegative) == 1.0 &&
              model.value(paramIndex(0, FxParam::PhaserNegative)) == 0.0 && model.value(flangerStereo) == 1.0,
          "the flanger's + and - pick its polarity");
    // The filter's Key Track, the third knob of its first row.
    const int keyTrack = paramIndex(3, FxParam::FilterKeyTrack);
    drag(view, fxKnob(model, 3, 2, 3, 206.0f), {fxKnob(model, 3, 2, 3, 206.0f).x, fxKnob(model, 3, 2, 3, 206.0f).y - 20.0f});
    check(model.value(keyTrack) > 0.0, "the filter effect's Key Track knob turns");
    view.mouseMove({600.0f, 328.0f}, {});
    check(turned && renderer.save(view, dir / "27-new-effects.png") && fxChain(model).size() == 5,
          "render a synced stereo phaser and a flanger mid-sweep, a phase disperser, a filter's spectrum and an widener");
    // Its response follows the note its key tracking does, like a voice filter's: at 100 %, a C7 takes a 2 kHz
    // low-pass three octaves up, so 7 kHz, 22 dB down at C4, is filled down from -12 dB.
    model.set(paramIndex(3, FxParam::FilterCutoff), normFromCutoff(2000.0));
    model.set(keyTrack, 1.0);
    auto fillAt = [&](float pitch) {
        model.live.fxPitch = pitch;
        view.poll();
        renderer.save(view, dir / "27a-fx-filter-key-track.png");
        const uint32_t px = renderer.pixel({fxCardX(model, 3) + 24.0f + 0.85f * (fxCardW(model, 3) - 48.0f), 300.0f});
        return static_cast<int>((px >> 16 & 255) + (px >> 8 & 255) + (px & 255));
    };
    const int fillC4 = fillAt(60.0f), fillC7 = fillAt(96.0f);
    check(fillC7 > fillC4 + 10, "the filter effect's response moves with the note its key tracking follows");
    // The other effects' displays follow their params as modulation takes them, as the filter's does: the
    // disperser's peak and the widener's spread move with an offset on its Freq and its Width (read off the rendered
    // pixels: how many pixels of the display change).
    {
        auto displayPixels = [&](int slot, FxType type) {
            view.poll();
            const Rect shown = detail::fxDisplay((fxCardX(model, slot) - 46.0f) / kFxStep, type);
            std::vector<uint32_t> pixels;
            for (float y = shown.y + 20.0f; y < shown.bottom() - 4.0f; y += 1.0f)
                for (float x = shown.x + 4.0f; x < shown.right() - 4.0f; x += 1.0f)
                    pixels.push_back(renderer.pixel({x, y}));
            return pixels;
        };
        auto changed = [](const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
            int n = 0;
            for (size_t i = 0; i < a.size(); ++i)
                n += a[i] != b[i];
            return n;
        };
        model.live.voices = 1;
        model.live.age = 1.0f; // the widener's taps not wavering
        renderer.save(view, dir / "27b-fx-unmodulated.png");
        const std::vector<uint32_t> disperser = displayPixels(2, FxType::Disperser);
        const std::vector<uint32_t> widener = displayPixels(4, FxType::Widener);
        const int disperserFreq = paramIndex(2, FxParam::DisperserFreq), widenerWidth = paramIndex(4, FxParam::WidenerWidth);
        model.live.modOffset[disperserFreq] = model.value(disperserFreq) > 0.5 ? -0.3f : 0.3f;
        model.live.modOffset[widenerWidth] = model.value(widenerWidth) > 0.5 ? -0.4f : 0.4f;
        view.poll();
        const bool shot = renderer.save(view, dir / "27c-fx-modulated.png");
        const int disperserMoved = changed(disperser, displayPixels(2, FxType::Disperser));
        const int widenerMoved = changed(widener, displayPixels(4, FxType::Widener));
        model.live.modOffset[disperserFreq] = 0.0f;
        model.live.modOffset[widenerWidth] = 0.0f;
        model.live.voices = 0;
        model.live.age = 0.0f;
        std::printf("      modulated: %d pixels of the disperser's display change, %d of the widener's\n",
                    disperserMoved, widenerMoved);
        check(shot && disperserMoved > 100 && widenerMoved > 100,
              "the effects' displays follow their params as modulation takes them");
    }
    // Its response has a handle for cutoff (sideways) and resonance (up and down), as a voice filter's.
    model.live.fxPitch = 60.0f;
    view.poll();
    const Rect shown = detail::fxDisplay((fxCardX(model, 3) - 46.0f) / kFxStep, FxType::Filter);
    const int fxCutoff = paramIndex(3, FxParam::FilterCutoff), fxReso = paramIndex(3, FxParam::FilterResonance);
    const double fxCutoffBefore = model.value(fxCutoff), fxResoBefore = model.value(fxReso);
    const Point fxHandle{shown.x + 10.0f + static_cast<float>(fxCutoffBefore) * (shown.w - 20.0f),
                         shown.bottom() - 10.0f - static_cast<float>(fxResoBefore) * (shown.h - 20.0f)};
    drag(view, fxHandle, {fxHandle.x - 30.0f, fxHandle.y - 15.0f});
    check(model.value(fxCutoff) < fxCutoffBefore - 0.05 && model.value(fxReso) > fxResoBefore + 0.05,
          "dragging the filter effect's response handle moves its cutoff and resonance");
    // Its Mix shows in the response as a voice filter's does: what goes in blended back in (read off the rendered
    // pixels).
    {
        const int fxMix = paramIndex(3, FxParam::FilterMix);
        auto responsePixels = [&] {
            std::vector<uint32_t> pixels;
            for (float y = shown.y + 10.0f; y < shown.bottom() - 10.0f; y += 1.0f)
                for (float x = shown.x + 10.0f; x < shown.right() - 10.0f; x += 1.0f)
                    pixels.push_back(renderer.pixel({x, y}));
            return pixels;
        };
        view.mouseMove({600.0f, 60.0f}, {});
        renderer.save(view, dir / "27d-fx-filter-full.png");
        const std::vector<uint32_t> full = responsePixels();
        const double mixBefore = model.value(fxMix);
        model.set(fxMix, 0.4);
        view.poll();
        renderer.save(view, dir / "27e-fx-filter-mix.png");
        const std::vector<uint32_t> mixed = responsePixels();
        model.set(fxMix, mixBefore);
        int moved = 0;
        for (size_t i = 0; i < full.size(); ++i)
            moved += full[i] != mixed[i];
        std::printf("      the filter effect's Mix at 40 %%: %d pixels of its response change\n", moved);
        check(mixBefore == 1.0 && moved > 300, "the filter effect's response follows its Mix");
    }
}

// A shaper's page: the curve list picks Hard, the link switch unlinks, Drive turns; picked from the list, Custom shows
// its points in the display to drag, and its pencil opens the big editor over the routing panel, the page staying.
void testShapers(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const int shaper = 0, node = kOps + kFilters;
    click(view, nodeTab(node));
    const auto at = [&](ShaperParam field) { return model.value(paramIndex(shaper, field)); };
    check(at(ShaperParam::Enabled) == 1.0 && model.layout().selectedNode == node, "a shaper's tab shows its page");
    click(view, kShaperList);
    click(view, shaperListItem(ClipMode::Hard));
    click(view, kShaperLink);
    const double drive = at(ShaperParam::Drive);
    drag(view, shaperKnob(0), {shaperKnob(0).x, shaperKnob(0).y - 20.0f});
    check(at(ShaperParam::Mode) == static_cast<double>(ClipMode::Hard) && at(ShaperParam::Link) == 0.0 &&
              at(ShaperParam::Drive) > drive,
          "a shaper's list picks its curve, its chain unlinks it and its Drive knob turns");
    // Three knobs, centered: Mix last, and nothing where a fourth would be (its To Out is its line to OUT's label).
    drag(view, shaperKnob(2), {shaperKnob(2).x, shaperKnob(2).y + 30.0f});
    const bool mixTurned = at(ShaperParam::Mix) < 1.0;
    view.undo();
    const double outBefore = at(ShaperParam::Out);
    drag(view, shaperKnob(3), {shaperKnob(3).x, shaperKnob(3).y - 30.0f});
    check(mixTurned && at(ShaperParam::Mix) == 1.0 && at(ShaperParam::Out) == outBefore,
          "a shaper's page has Drive, Output and Mix, centered, and no To Out knob");
    model.set(paramIndex(0, OpParam::Enabled), 1.0);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(shaperSendIndex(0, shaper), 0.8);
    model.set(shaperSendIndex(1, shaper), 0.5);
    model.set(shaperModIndex(shaper, 2), 0.4);
    model.set(paramIndex(2, OpParam::Enabled), 1.0);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool pageShot = renderer.save(view, dir / "30-shaper-page.png");
    click(view, kShaperList);
    click(view, shaperListItem(ClipMode::Custom));
    drag(view, shaperCurveAt(0.5, 0.0), shaperCurveAt(0.5, 0.5));
    const double middle = model.value(shapeFieldIndex(shaperCurveCountIndex(shaper), 1, PointField::Y));
    view.mouseMove(shaperCurveAt(0.5, 0.5), {});
    const bool customShot = renderer.save(view, dir / "30a-shaper-custom.png");
    check(pageShot && customShot && at(ShaperParam::Mode) == static_cast<double>(ClipMode::Custom) &&
              std::abs(middle - 0.5) < 0.03,
          "picked from the list, a shaper's Custom curve is drawn right on its page");
    click(view, kShaperSym);
    const bool symmetric = at(ShaperParam::Symmetric) == 1.0;
    click(view, kShaperSym);
    check(symmetric && at(ShaperParam::Symmetric) == 0.0, "a Custom curve's SYM switches on and off");
    click(view, kShaperEdit);
    view.doubleClick(noteCurveAt(0.25, 0.5), {});
    const bool added = model.value(shaperCurveCountIndex(shaper)) == 4.0;
    const double driveThen = at(ShaperParam::Drive);
    drag(view, shaperKnob(0), {shaperKnob(0).x, shaperKnob(0).y - 20.0f});
    const bool pageStays = at(ShaperParam::Drive) > driveThen;
    view.mouseMove(noteCurveAt(0.25, 0.5), {});
    const bool editorShot = renderer.save(view, dir / "30b-shaper-editor.png");
    click(view, kRoutingEditorDone);
    check(added && pageStays && editorShot && model.layout().view == RoutingView::Graph,
          "the pencil opens a shaper's curve over the routing panel, its page staying beside it");
}

// Folding cards: the chevron left of an effect card's cross folds it to a strip (38 px, its neighbours closing
// up), whose icon stays where it was and still switches it, and whose name unfolds it; an LFO card folds the
// same way (to 0.2 of a place) and its strip's grip still moves it.
// A Custom LFO's card widens to edit its shape in the modulation panel, the panels above staying in use: picking
// Custom, or a click on a Custom LFO's display or its widen button, widens it to two card places, the next card
// moving along. Its shape is drawn there as in the full-size editor; its tools invert, reverse, shift and copy it, Points | Steps and Snap
// pick how a click works and whether points snap; its full-size button opens the shape over the panels, greying the
// card's editor out until it closes, and its own collapse button, another shape or folding takes the card back to
// its size; collapsing the modulation panel leaves it widened.
void testWideLfoCard(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    for (int lfo = 0; lfo < 3; ++lfo)
        model.values[paramIndex(lfo, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    view.poll();
    auto pointValue = [&](int lfo, int point, PointField field) {
        return model.value(shapePointIndex(lfo, point, field));
    };
    auto roughly = [](double a, double b, double within = 0.01) { return std::abs(a - b) < within; };
    const int count = shapeCountIndex(0);
    // The power switch of the card at a place, and whose it is: LFO 2's card one place along while LFO 1's is wide.
    auto powerAt = [&](float place, float span) {
        return Point{cardX(place) + span * (kCardW + 12.0f) - 12.0f - 34.0f, 573.0f};
    };
    auto switches = [&](Point at, int lfo) {
        click(view, at);
        const bool off = model.value(sourceEnabledIndex(lfo)) == 0.0;
        click(view, at);
        return off && model.value(sourceEnabledIndex(lfo)) == 1.0;
    };
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Curve));
    view.poll();
    click(view, lfoDisplay(0));
    const bool widened = switches(powerAt(0, 2.0f), 0) && switches(sourcePower(2), 1);
    click(view, wideButton(0, 0));
    const bool normal = switches(sourcePower(1), 1);
    click(view, lfoDisplay(0));
    const bool again = switches(powerAt(0, 2.0f), 0) && switches(sourcePower(2), 1);
    // Its normal card's widen button, left of the fold button, widens it too.
    click(view, wideButton(0, 0));
    view.mouseMove({cardX(0) + kCardW - 78.0f, 574.0f}, {});
    const bool button = renderer.save(view, dir / "10d0-lfo-card-widen-button.png") && switches(sourcePower(1), 1);
    click(view, {cardX(0) + kCardW - 78.0f, 574.0f});
    const bool byButton = switches(powerAt(0, 2.0f), 0) && switches(sourcePower(2), 1);
    check(widened && normal && again && button && byButton,
          "a click on a Curve source's display widens its card, and so does its widen button");
    // The inspector stays: its Phase knob turns while the shape shows.
    const double fineBefore = model.value(paramIndex(0, OpParam::Phase));
    drag(view, kOpPhaseKnob, {kOpPhaseKnob.x, kOpPhaseKnob.y - 30.0f});
    check(model.value(paramIndex(0, OpParam::Phase)) != fineBefore && model.openEdits == 0,
          "the inspector's knobs turn while a widened card shows an LFO's shape");
    view.undo();

    // Points as in the full-size editor: a double-click adds one, a drag moves it, a double-click deletes it.
    view.doubleClick(cardShapeAt(0, 0.75, -0.4), {});
    const bool added = model.value(count) == 3.0 && roughly(pointValue(0, 2, PointField::X), 0.75) &&
                       roughly(pointValue(0, 2, PointField::Y), -0.4, 0.03);
    drag(view, cardShapeAt(0, 0.75, -0.4), cardShapeAt(0, 0.8, 0.0));
    const bool dragged = roughly(pointValue(0, 2, PointField::X), 0.8) && roughly(pointValue(0, 2, PointField::Y), 0.0, 0.03);
    view.undo();
    model.live.voices = 1;
    model.live.lfoPhases[0] = 0.3f;
    model.live.sources[0] = 0.2f;
    view.poll();
    view.mouseMove(cardShapeAt(0, 0.75, -0.4), {});
    check(added && dragged && model.openEdits == 0 && renderer.save(view, dir / "10d-lfo-card.png"),
          "a widened card's shape takes points and drags, as the full-size editor's");
    model.live.voices = 0;
    view.poll();

    // The tools: Invert flips each level; Reverse runs the shape back to front, the first point staying first and each
    // bend the other way; Shift moves it a grid column later or earlier round the cycle, a point put on the curve where
    // the new start falls; each one undo step.
    model.set(shapePointIndex(0, 1, PointField::Curve), 0.5);
    click(view, lfoTool(0, 0));
    const bool inverted = pointValue(0, 0, PointField::Y) == 1.0 && pointValue(0, 1, PointField::Y) == -1.0 &&
                          roughly(pointValue(0, 2, PointField::Y), 0.4, 0.03) &&
                          pointValue(0, 1, PointField::Curve) == 0.5;
    click(view, lfoTool(0, 1));
    const bool reversed = model.value(count) == 3.0 && pointValue(0, 0, PointField::Y) == 1.0 &&
                          roughly(pointValue(0, 1, PointField::X), 0.25) &&
                          roughly(pointValue(0, 1, PointField::Y), 0.4, 0.03) &&
                          pointValue(0, 1, PointField::Curve) == -0.5 && pointValue(0, 2, PointField::X) == 0.5 &&
                          pointValue(0, 2, PointField::Y) == -1.0;
    const double third = pointValue(0, 1, PointField::X); // the reversed shape's second point, a little off 0.25
    model.layout().shapeGridX = 4;
    model.set(shapePointIndex(0, 1, PointField::X), 0.25);
    click(view, lfoTool(0, 3)); // right: from (0, 1), (0.25, 0.4), (0.5, -1) to a start halfway back up to 1
    const bool right = model.value(count) == 4.0 && roughly(pointValue(0, 0, PointField::Y), 0.0, 1e-9) &&
                       pointValue(0, 1, PointField::X) == 0.25 && pointValue(0, 1, PointField::Y) == 1.0 &&
                       pointValue(0, 2, PointField::X) == 0.5 && pointValue(0, 3, PointField::X) == 0.75 &&
                       pointValue(0, 3, PointField::Y) == -1.0;
    click(view, lfoTool(0, 2)); // left again: the point at 0.25 comes first, the one put in stays at the end
    const bool left = model.value(count) == 4.0 && pointValue(0, 0, PointField::Y) == 1.0 &&
                      pointValue(0, 1, PointField::X) == 0.25 && pointValue(0, 2, PointField::X) == 0.5 &&
                      pointValue(0, 3, PointField::X) == 0.75 && roughly(pointValue(0, 3, PointField::Y), 0.0, 1e-9);
    view.undo();
    view.undo();
    const bool undone = model.value(count) == 3.0 && pointValue(0, 1, PointField::X) == 0.25;
    std::printf("      shape tools: inverted %d reversed %d (second point at %.4f) right %d left %d undone %d\n",
                inverted, reversed, third, right, left, undone);
    view.mouseMove(lfoTool(0, 3), {});
    check(inverted && reversed && right && left && undone && renderer.save(view, dir / "10e-lfo-card-tools.png"),
          "a widened card's tools invert, reverse and shift its shape, each one undo step");
    model.layout().shapeGridX = 16;

    // Steps: a plain click draws a step, Shift+click works with points; Snap snaps a dragged point to the grid, Ctrl
    // letting it go. Both are the editor's, in the layout.
    click(view, lfoMode(0, 1));
    const bool stepsOn = model.layout().shapeSteps;
    click(view, cardShapeAt(0, 2.5 / 16.0, 0.5));
    const bool stepped = model.value(count) == 5.0 && pointValue(0, 1, PointField::X) == 2.0 / 16.0 &&
                         pointValue(0, 2, PointField::X) == 3.0 / 16.0;
    view.mouseMove(cardShapeAt(0, 0.9, 0.0), {});
    const bool stepsShot = renderer.save(view, dir / "10f-lfo-card-steps.png");
    view.undo();
    click(view, lfoMode(0, 0));
    click(view, lfoMode(0, 2));
    const bool snapOn = model.layout().shapeSnap && !model.layout().shapeSteps;
    drag(view, cardShapeAt(0, 0.25, 0.4), cardShapeAt(0, 0.3, 0.3));
    const bool snapped = std::abs(pointValue(0, 1, PointField::X) - 5.0 / 16.0) < 1e-9 &&
                         std::abs(pointValue(0, 1, PointField::Y) - 0.25) < 1e-9;
    view.mouseDown(cardShapeAt(0, 5.0 / 16.0, 0.25), {});
    view.mouseMove(cardShapeAt(0, 0.3, 0.3), {false, true});
    view.mouseUp(cardShapeAt(0, 0.3, 0.3), {false, true});
    const bool free = roughly(pointValue(0, 1, PointField::X), 0.3) && std::abs(pointValue(0, 1, PointField::X) - 5.0 / 16.0) > 1e-3;
    click(view, lfoMode(0, 2));
    check(stepsOn && stepped && stepsShot && snapOn && snapped && free && !model.layout().shapeSnap,
          "Steps draws a step with a plain click, and Snap snaps a point to the grid unless Ctrl is held");

    // Copy and Paste: Curve 1's shape into the Curve source in place 2, whose card, widened, takes Curve 1's back to its
    // size.
    click(view, lfoTool(0, 4));
    model.set(paramIndex(2, SourceParam::Type), static_cast<double>(SourceType::Curve));
    view.poll();
    click(view, lfoDisplay(3));
    const float wideCard = 2.0f; // with LFO 1's card back to its size, LFO 3's is the third, widened
    click(view, lfoTool(wideCard, 5));
    bool same = model.value(shapeCountIndex(2)) == model.value(count);
    for (int point = 0; point < 3 && same; ++point)
        for (PointField field : {PointField::X, PointField::Y, PointField::Curve})
            same = same && pointValue(2, point, field) == pointValue(0, point, field);
    view.undo();
    const bool oneWide = switches(sourcePower(1), 1) && switches(powerAt(2, 2.0f), 2);
    check(same && oneWide, "Copy and Paste take a shape from one Curve source to another, one card widened at a time");

    // The full-size editor: the inspector hidden; its collapse button goes back into the card, still widened.
    click(view, wideButton(wideCard, 2));
    drag(view, kOpPhaseKnob, {kOpPhaseKnob.x, kOpPhaseKnob.y - 30.0f});
    const bool covered = model.value(paramIndex(0, OpParam::Phase)) == fineBefore;
    // Meanwhile the card's own editor is greyed out and takes nothing: its Invert leaves the shape be.
    const double firstLevel = pointValue(2, 0, PointField::Y);
    click(view, lfoTool(wideCard, 0));
    const bool greyed = pointValue(2, 0, PointField::Y) == firstLevel;
    view.mouseMove({400.0f, 728.0f}, {});
    const bool bigShot = renderer.save(view, dir / "10g-lfo-full-size.png");
    click(view, kShapeDone);
    const bool back = switches(powerAt(2, 2.0f), 2);
    // Closed by a view tab too, the card's editor is back in use.
    click(view, wideButton(wideCard, 2));
    click(view, kRoutingTab);
    click(view, lfoTool(wideCard, 0));
    const bool restored = pointValue(2, 0, PointField::Y) == -firstLevel;
    view.undo();
    // Collapsing the modulation panel and opening it again leaves the card widened.
    click(view, kSourcesToggle);
    click(view, kSourcesToggle);
    const bool kept = switches(powerAt(2, 2.0f), 2);
    check(covered && greyed && bigShot && back && restored && kept,
          "the full-size editor greys the widened card's editor out until it closes, by its done button or a tab, "
          "and the card stays widened while the panel collapses");
    // Back to its size by its collapse button; its display widens it again.
    click(view, wideButton(wideCard, 0));
    const bool collapsed = switches(sourcePower(2), 2);
    click(view, lfoDisplay(2));
    check(collapsed && switches(powerAt(2, 2.0f), 2),
          "a widened card goes back to its size by its button, and its display widens it again");
}

// Slots past the 16 the chain had at first: one at the chain's start folds by a bit of its own, and moved by its
// grip past the next card takes its place in the order.
void testLaterSlots(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const int later = kFxSlots - 3;
    setChain(model, {later, 0, kFxSlots - 1});
    model.values[paramIndex(later, FxParam::Type)] = static_cast<double>(FxType::Delay);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    model.values[paramIndex(kFxSlots - 1, FxParam::Type)] = static_cast<double>(FxType::Phaser);
    view.poll();
    click(view, kFxTab);
    const float cardTop = 179.5f; // an effect card's
    click(view, {fxCardX(model, 0) + fxCardW(model, 0) - 53.0f, cardTop + 18.5f}); // its fold chevron
    const bool folded = model.ui.fxFolded == 0 && model.ui.fxFoldedLater == 1u << (later - kFirstFxSlots);
    const float strip = fxCardX(model, 0) + fxCardW(model, 0) * 0.5f;
    drag(view, {strip, cardTop + 38.0f}, {strip + 200.0f, cardTop + 38.0f}); // its grip, past the next card
    const bool moved = fxChain(model) == std::vector<int>{0, later, kFxSlots - 1} &&
                       model.ui.fxFoldedLater == 1u << (later - kFirstFxSlots);
    view.mouseMove({600.0f, 328.0f}, {});
    if (!folded || !moved) {
        std::printf("      folded bits %u %u, chain", model.ui.fxFolded, model.ui.fxFoldedLater);
        for (int slot : fxChain(model))
            std::printf(" %d", slot);
        std::printf("\n");
    }
    check(folded && moved && renderer.save(view, dir / "28g-later-slots.png"),
          "an effect in a slot past the 16th folds and moves in the chain like the others");
}

// The effects graph on its slot grid: switching to it puts the chain on its main line, wired as it ran; the main
// line's + opens the list of effects with a split after them, which opens two bands with a + each; a band's + puts an
// effect on its band, and its list joins the bands, the line going on from the join with one + again; the join's card
// sets its bands' levels, and it can't be taken out while something comes after it; the split's crossover handle
// drags on its card; + EFFECT with the split picked puts an effect on a band; a split taken out puts its bands'
// effects on the main line, undoably; a wire's middle puts an effect, or a split with its join, there; a node under
// the mouse shows an x that removes it; nodes dragged and dropped, the others making room; the mouse along a wire
// showing its +; and going back to a chain asks first, then leaves the chain in the line's order.
void testFxGraph(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    auto centerOf = [](Rect r) { return Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    click(view, centerOf(detail::fxModeButton(1)));
    auto from = [&](int node, int input) {
        return fxPortOf(model.value(fxInputIndex(node, input, FxInputField::From)));
    };
    check(model.ui.fxGraphView && from(0, 0) == kFxPortIn && from(1, 0) == fxSlotPort(0) &&
              from(kFxOutNode, 0) == fxSlotPort(1) && from(kFxOutNode, 1) < 0 &&
              renderer.save(view, dir / "40a-fx-graph.png"),
          "switching to the effects graph shows the chain as its main line, wired as it runs");

    // The grid, unscrolled: IN in column 0, then a column a node, the high band a row up and the low band a row down.
    auto cell = [](int col, int row) { return detail::fxGridCell(col, row, 0.0f); };
    // The list a + opens, beside it and kept inside the editor: the effects, then a split or a join a little apart.
    auto plusItem = [&](Point plus, FxType type) { return fxListItem(view, besidePlus(plus), type, true); };
    auto plusNode = [&](Point plus) { return fxListSplit(view, besidePlus(plus)); };
    auto addAt = [&](Point plus, FxType type) {
        click(view, plus);
        click(view, plusItem(plus, type));
    };

    // The main line's + after the reverb: Split into two bands, with its join a column on, each empty band curving
    // out to a + on its row and into the join.
    const int split = fxSplitNode(0), join = fxSumNode(0);
    const int high = fxBandPort(0, kFxBands - 1), low = fxBandPort(0, 0);
    click(view, cell(3, 0));
    const bool listed = renderer.save(view, dir / "40b-fx-plus-list.png");
    click(view, plusNode(cell(3, 0)));
    view.mouseMove({600.0f, 428.0f}, {});
    check(listed && model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(split, 0) == fxSlotPort(1) &&
              model.value(paramIndex(0, SumParam::Used)) == 1.0 && from(join, 0) == high && from(join, 1) == low &&
              from(kFxOutNode, 0) == fxSumPort(0) && from(kFxOutNode, 1) < 0 && model.ui.fxSelected == split &&
              renderer.save(view, dir / "40c-fx-split.png"),
          "the main line's + puts a split there with its join, each band's + on its row between them");
    // Each + ringed in the color of the wire it is on: the high band's amber, the low band's blue, the main line's
    // grey after the join (read off the rendered pixels: down through the top of each ring, faint as they are).
    {
        auto ring = [&](Point m) { return redOverBlue(brightestAlong(renderer, {m.x, m.y - 11.0f}, {m.x, m.y - 6.0f})); };
        const int highRing = ring(cell(4, -1)), lowRing = ring(cell(4, 1)), lineRing = ring(cell(6, 0));
        std::printf("      the +s' rings, red over blue: high band %d, low band %d, main line %d\n", highRing, lowRing,
                    lineRing);
        check(highRing > 30 && lowRing < -30 && std::abs(lineRing) < 20,
              "each + in the graph is ringed in the color of the wire it is on");
    }

    // A delay on the high band and a compressor on the low one, from their +s; the band's list offers no split.
    addAt(cell(4, -1), FxType::Delay);
    addAt(cell(4, 1), FxType::Compressor);
    check(from(2, 0) == high && from(3, 0) == low && from(join, 0) == fxSlotPort(2) &&
              from(join, 1) == fxSlotPort(3) && fxChain(model) == std::vector<int>{0, 1, 2, 3} &&
              renderer.save(view, dir / "40d-fx-join.png"),
          "a band's + puts an effect on its band");

    // The shaper dropped at the low band's end, on its wire into the join: there; undone.
    {
        const Point drop = detail::fxGridAt(5.0f, 0.585f, 0.0f); // on its +
        view.mouseMove(cell(1, 0), {});
        view.mouseDown(cell(1, 0), {});
        view.mouseMove({cell(1, 0).x + 8.0f, cell(1, 0).y + 4.0f}, {});
        view.mouseMove(drop, {});
        for (int k = 0; k < 40; ++k)
            view.poll();
        view.mouseUp(drop, {});
        for (int k = 0; k < 40; ++k)
            view.poll();
        const bool atBandEnd = from(0, 0) == fxSlotPort(3) && from(join, 1) == fxSlotPort(0) &&
                               from(1, 0) == kFxPortIn;
        view.undo();
        view.mouseMove({600.0f, 428.0f}, {});
        check(atBandEnd && from(0, 0) == kFxPortIn && from(join, 1) == fxSlotPort(3),
              "a node dropped at a band's end, on its wire into the join, goes there");
    }

    // A phaser after the join (a column on from the bands' +s), from the main line's + again.
    addAt(cell(7, 0), FxType::Phaser);
    check(from(4, 0) == fxSumPort(0) && from(kFxOutNode, 0) == fxSlotPort(4),
          "the main line's + after a join puts an effect after it");

    // The join's node shows its split's card, the same as the split's own: its High knob takes the high band's level
    // down.
    click(view, cell(6, 0));
    const Rect card = detail::fxInspectorCard(1.0f);
    const Point highKnob = detail::splitKnob(card, 2); // Low on the left, the crossover, High on the right
    drag(view, highKnob, {highKnob.x, highKnob.y + 40.0f});
    const double highLevel = model.value(fxInputIndex(join, 0, FxInputField::Level));
    char highText[32] = {};
    formatParam(fxInputIndex(join, 0, FxInputField::Level), highLevel, highText, sizeof(highText));
    check(model.ui.fxSelected == join && highLevel < -1.0 &&
              model.value(fxInputIndex(join, 1, FxInputField::Level)) == 0.0 && model.openEdits == 0 &&
              std::string(highText).find(" dB") != std::string::npos && renderer.save(view, dir / "40e-fx-join-card.png"),
          "a join's node shows its split's card, which sets the bands' levels, in dB from 0 dB at the top");

    // Its high band's S, under its knob: soloed, an S in a circle at the split's and the join's top left, and the low
    // band's compressor and its wires faint, the high band's delay as it was (read off the rendered pixels); undone.
    {
        const Rect joinBox{cell(6, 0).x - detail::kFxJoinHalfW, cell(6, 0).y - detail::kFxJoinHalfH,
                           2.0f * detail::kFxJoinHalfW, 2.0f * detail::kFxJoinHalfH};
        const Point mark{joinBox.x + 1.0f, joinBox.y - 5.0f}; // inside the circle, above the box
        const Point splitMark{cell(3, 0).x - detail::kFxSplitHalfW + 1.0f, cell(3, 0).y - detail::kFxSplitHalfH - 5.0f};
        auto ring = [&](int row) { // the node's ring at its right, on the band's row
            const Point c = cell(4, row);
            return ringLevel(renderer, {c.x + detail::kFxNodeR, c.y - 3.0f}, 6.0f);
        };
        auto wireInto = [&](int row) { // the band's wire just before its node's arrowhead
            const Point c = cell(4, row);
            return ringLevel(renderer, {c.x - detail::kFxNodeR - 16.0f, c.y - 4.0f}, 8.0f);
        };
        view.mouseMove({600.0f, 428.0f}, {});
        renderer.save(view, dir / "40e2-fx-join-unsoloed.png");
        const uint32_t unsoloed = renderer.pixel(mark), splitUnsoloed = renderer.pixel(splitMark);
        const int lowRing = ring(1), highRing = ring(-1), lowWire = wireInto(1);
        // The main line's wires grey, each band's in its color, and the split's and the join's nodes in the bands'
        // colors, the high band's above, a split's coming in from grey at its left and a join's going out to grey at
        // its right (read off the rendered pixels: the outline down either edge, near its top and bottom).
        {
            const float mainX = 0.5f * (cell(2, 0).x + detail::kFxNodeR + cell(3, 0).x - detail::kFxSplitHalfW - 8.0f);
            const uint32_t mainWire = brightestAlong(renderer, {mainX, cell(3, 0).y - 3.0f}, {mainX, cell(3, 0).y + 3.0f});
            auto bandWire = [&](int row) {
                const Point c = cell(4, row);
                return brightestAlong(renderer, {c.x - detail::kFxNodeR - 16.0f, c.y - 4.0f},
                                      {c.x - detail::kFxNodeR - 16.0f, c.y + 4.0f});
            };
            // A node's edge at its left or right, 5 px from its top or bottom: clear of where wires meet it.
            auto edge = [&](int col, bool right, bool top) {
                const Point c = cell(col, 0);
                const float x = c.x + (right ? 1.0f : -1.0f) * detail::kFxSplitHalfW;
                const float y = c.y + (top ? -1.0f : 1.0f) * (detail::kFxSplitHalfH - 5.0f);
                return brightestAlong(renderer, {x - 3.0f, y}, {x + 3.0f, y});
            };
            const bool splitFades = grey(edge(3, false, true)) && grey(edge(3, false, false)) && amber(edge(3, true, true)) &&
                               blue(edge(3, true, false));
            const bool joinFades = amber(edge(6, false, true)) && blue(edge(6, false, false)) && grey(edge(6, true, true)) &&
                              grey(edge(6, true, false));
            std::printf("      wires: main line %06x, high band %06x, low band %06x; the split's right edge %06x above, "
                        "%06x below, its left %06x; the join's left edge %06x above, %06x below, its right %06x\n",
                        mainWire, bandWire(-1), bandWire(1), edge(3, true, true), edge(3, true, false),
                        edge(3, false, true), edge(6, false, true), edge(6, false, false), edge(6, true, true));
            check(grey(mainWire) && amber(bandWire(-1)) && blue(bandWire(1)) && splitFades && joinFades,
                  "the graph's main line is grey, each band's wires and half of the split's and join's nodes in its "
                  "color, fading in across a split and out across a join");
        }
        click(view, centerOf(detail::splitSoloButton(card, 2)));
        const bool soloed = model.value(paramIndex(0, SumParam::SoloHigh)) == 1.0 &&
                            model.value(paramIndex(0, SumParam::SoloLow)) == 0.0;
        view.mouseMove({600.0f, 428.0f}, {});
        const bool shot = renderer.save(view, dir / "40e2-fx-join-solo.png");
        const bool marked = renderer.pixel(mark) != unsoloed && renderer.pixel(splitMark) != splitUnsoloed;
        std::printf("      soloing the high band: the low band's ring %d -> %d, its wire %d -> %d, the high band's ring "
                    "%d -> %d\n",
                    lowRing, ring(1), lowWire, wireInto(1), highRing, ring(-1));
        const bool faint = ring(1) < lowRing - 40 && wireInto(1) < lowWire - 30 && ring(-1) == highRing;
        // Its low band's S then: both soloed, each on its own; undone, the high band's alone again.
        click(view, centerOf(detail::splitSoloButton(card, 0)));
        const bool both = model.value(paramIndex(0, SumParam::SoloHigh)) == 1.0 &&
                          model.value(paramIndex(0, SumParam::SoloLow)) == 1.0;
        view.undo();
        const bool back = model.value(paramIndex(0, SumParam::SoloHigh)) == 1.0 &&
                          model.value(paramIndex(0, SumParam::SoloLow)) == 0.0;
        view.undo();
        check(soloed && shot && marked && both && back && model.value(paramIndex(0, SumParam::SoloHigh)) == 0.0,
              "the split's card solos bands, each on its own, the split and join nodes marked with an S while one is");
        check(faint, "a band soloed leaves the other band's nodes and wires faint");
    }

    // The split's crossover handle, on its card in the inspector, dragged from 250 Hz to 500 Hz.
    click(view, cell(3, 0));
    const Rect display{card.x + 14.0f, card.y + 66.0f, card.w - 28.0f, 104.0f};
    const float handleY = (display.y + 12.0f + display.bottom() - 22.0f) * 0.5f; // where the bands cross, at 12 dB
    drag(view, {display.x + static_cast<float>(normFromCutoff(250.0)) * display.w, handleY},
         {display.x + static_cast<float>(normFromCutoff(500.0)) * display.w, handleY});
    check(model.ui.fxSelected == split &&
              std::abs(model.value(paramIndex(0, SplitParam::Low)) - normFromCutoff(500.0)) < 0.01 &&
              model.openEdits == 0 && renderer.save(view, dir / "40f-fx-split-card.png"),
          "a split's crossover handle drags across its card's display");

    // Its slope's list under the display, as a filter's is, 12 dB to begin with: 18 dB refused (the list staying
    // open), 24 dB picked, the bands' responses steeper; back to 12, undoably.
    const bool startsAt12 = model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
    const Rect slopeList = detail::splitSlopeButton(display);
    click(view, centerOf(slopeList));
    view.mouseMove(centerOf(detail::listMenuItem(slopeList, kFilterSlopes, 3)), {});
    const bool slopeListShot = renderer.save(view, dir / "40f1-fx-split-slope-list.png");
    click(view, centerOf(detail::listMenuItem(slopeList, kFilterSlopes, 2)));
    const bool refused18 = model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
    click(view, centerOf(detail::listMenuItem(slopeList, kFilterSlopes, 3))); // the list is still open
    const bool steep = model.value(paramIndex(0, SplitParam::Slope)) == kFilterSlopes - 1.0;
    const bool slopeShot = renderer.save(view, dir / "40f2-fx-split-slope.png");
    view.undo();
    check(startsAt12 && slopeListShot && refused18 && steep && slopeShot &&
              model.value(paramIndex(0, SplitParam::Slope)) == 1.0,
          "a split's slope list picks 6, 12 or 24 dB (not 18), 12 to begin with");

    // BANDS 3 in its card's title: an empty middle band opens between the two, shown, into the join's third input, its
    // crossover at 2500 Hz (more than two octaves above the low one's 500 Hz), its slope kept at 12 dB: its list opens,
    // refusing 6 dB and picking 24, undone. An effect on its +, along the
    // main line's row; its S soloing it alone. BANDS 2 then asks where its effect goes: onto the low band's end. Undone,
    // it is back on the middle band; without it, BANDS 2 asks nothing; undone, as it was.
    {
        const double highBefore = model.value(paramIndex(0, SplitParam::High));
        const int middle = fxBandPort(0, 1);
        click(view, centerOf(detail::splitBandsButton(card, 3)));
        view.mouseMove({600.0f, 428.0f}, {});
        const bool three = model.value(paramIndex(0, SplitParam::Bands)) == 3.0 && from(join, 2) == middle &&
                           model.ui.splitBand[0] == 2 &&
                           std::abs(model.value(paramIndex(0, SplitParam::High)) - normFromCutoff(2500.0)) < 0.01 &&
                           renderer.save(view, dir / "40f3-fx-split-three.png");
        {
            // Its display's three stretches, between the crossovers, show their bands: low, high, then middle again.
            const Rect shown{card.x + 14.0f, card.y + 66.0f, card.w - 28.0f, 70.0f};
            click(view, {shown.x + 6.0f, shown.y + 20.0f});
            const bool lowShown = model.ui.splitBand[0] == 0;
            click(view, {shown.right() - 6.0f, shown.y + 20.0f});
            const bool highShown = model.ui.splitBand[0] == 1;
            click(view, {shown.x + static_cast<float>(normFromCutoff(1118.0)) * shown.w, shown.y + 20.0f});
            view.mouseMove({600.0f, 428.0f}, {});
            check(lowShown && highShown && model.ui.splitBand[0] == 2,
                  "a click on a three-band split's display shows the band between the crossovers there");
        }
        bool threeSlopes = false;
        {
            const Rect threeDisplay{card.x + 14.0f, card.y + 66.0f, card.w - 28.0f, 70.0f};
            const Rect list = detail::splitSlopeButton(threeDisplay);
            const bool kept12 = model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
            click(view, centerOf(list));
            click(view, centerOf(detail::listMenuItem(list, kFilterSlopes, 0))); // 6 dB: refused, the list still open
            const bool refused6 = model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
            click(view, centerOf(detail::listMenuItem(list, kFilterSlopes, kFilterSlopes - 1)));
            const bool picked24 = model.value(paramIndex(0, SplitParam::Slope)) == kFilterSlopes - 1.0;
            view.undo();
            threeSlopes = kept12 && refused6 && picked24 && model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
        }
        // Low X turned up past the high crossover pushes it along, at least a third of an octave above; High X
        // turned down past the low one pushes that down. Each one undo step, taking both back, a run of wheel notches
        // too.
        bool pushed = false;
        {
            const int lowIndex = paramIndex(0, SplitParam::Low), highIndex = paramIndex(0, SplitParam::High);
            const double lowBefore = model.value(lowIndex), highBefore3 = model.value(highIndex);
            auto apart = [&] {
                const double low = cutoffFromNorm(model.value(lowIndex));
                return model.value(highIndex) >=
                       normFromCutoff(std::min(low * std::pow(2.0, 1.0 / 3.0), kMaxCutoff)) - 1e-9;
            };
            const Point lowKnob = detail::splitCrossKnob(card, 0), highKnob = detail::splitCrossKnob(card, 1);
            drag(view, lowKnob, {lowKnob.x, lowKnob.y - 120.0f});
            const bool upPushed = model.value(highIndex) > highBefore3 && apart() && model.openEdits == 0;
            view.undo();
            const bool upBack = model.value(lowIndex) == lowBefore && model.value(highIndex) == highBefore3;
            drag(view, highKnob, {highKnob.x, highKnob.y + 120.0f});
            const bool downPushed = model.value(lowIndex) < lowBefore && apart() && model.openEdits == 0;
            view.undo();
            const bool downBack = model.value(lowIndex) == lowBefore && model.value(highIndex) == highBefore3;
            // A run of wheel notches on Low X, pushing High along on its later notches only, is one undo step too.
            int notches = 0;
            while (model.value(highIndex) == highBefore3 && notches < 400) {
                view.wheel(lowKnob, 1.0f, {});
                ++notches;
            }
            for (int n = 0; n < 5; ++n)
                view.wheel(lowKnob, 1.0f, {});
            const bool wheelPushed = model.value(highIndex) > highBefore3 && apart() && notches > 1 && notches < 400;
            view.undo();
            const bool wheelBack = model.value(lowIndex) == lowBefore && model.value(highIndex) == highBefore3;
            std::printf("      a crossover pushing the other: up %d (undone %d), down %d (undone %d), by the wheel %d "
                        "(undone at once %d)\n",
                        upPushed, upBack, downPushed, downBack, wheelPushed, wheelBack);
            pushed = upPushed && upBack && downPushed && downBack && wheelPushed && wheelBack;
        }
        check(pushed, "a three-band split's crossover turned past the other pushes it along, undoably");
        addAt(cell(4, 0), FxType::Widener);
        int widener = -1;
        for (int slot = 0; slot < kFxSlots; ++slot)
            if (from(slot, 0) == middle)
                widener = slot;
        view.mouseMove({600.0f, 428.0f}, {});
        const bool onMiddle = widener >= 0 && from(join, 2) == fxSlotPort(widener) &&
                              renderer.save(view, dir / "40f4-fx-split-three-middle.png");
        // A phaser after it, from its + on the wire into the join: the middle band two nodes long, its + a column on and
        // the join a column after that. The high and low bands' wires into it run level along their rows past the
        // phaser's column, bending only in the gap before the join (read off the rendered pixels: each wire there on
        // its row, not below or above it).
        bool levelWires = false;
        {
            addAt(cell(5, 0), FxType::Phaser);
            view.mouseMove({600.0f, 428.0f}, {});
            const bool longShot = renderer.save(view, dir / "40f4a-fx-split-three-long.png");
            auto onRow = [&](int row) {
                const Point p{cell(5, row).x - 20.0f, cell(5, row).y}; // short of the + in the band's next place
                const uint32_t wire = renderer.pixel(p);
                return wire != renderer.pixel({p.x, p.y - 10.0f}) && wire != renderer.pixel({p.x, p.y + 10.0f});
            };
            // Each band's + shows without the mouse on it, in the band's next place: the high and low bands' a column
            // on from their last node, on their rows (read off the rendered pixels, clear of the wire: the +'s upright
            // stroke against its disc beside it, where without a + both are the background).
            auto plusAt = [&](Point p) {
                return renderer.pixel({p.x, p.y - 3.0f}) != renderer.pixel({p.x + 6.0f, p.y - 3.0f});
            };
            // The wire between the widener and the phaser, inside the middle band, shows none until the mouse is on it.
            const Point between{(cell(4, 0).x + detail::kFxNodeR + cell(5, 0).x - detail::kFxNodeR - 8.0f) * 0.5f,
                                cell(5, 0).y};
            levelWires = longShot && from(join, 2) != fxSlotPort(widener) && onRow(-1) && onRow(1) &&
                         plusAt(cell(5, -1)) && plusAt(cell(5, 1)) && !plusAt(between);
            // The delay taken out by its x, the high band empty: its wire on from its + still runs level along its row
            // past the phaser's column, bending only before the join.
            const Point delayAt = cell(4, -1);
            const Point delayX{delayAt.x + detail::kFxNodeR - 2.0f, delayAt.y - detail::kFxNodeHalfH + 2.0f};
            view.mouseMove(delayAt, {});
            view.mouseMove(delayX, {});
            click(view, delayX);
            view.mouseMove({600.0f, 428.0f}, {});
            const bool emptyShot = renderer.save(view, dir / "40f4b-fx-split-three-empty-high.png");
            levelWires = levelWires && emptyShot && from(join, 0) == fxBandPort(0, kFxBands - 1) && onRow(-1) &&
                         plusAt(cell(4, -1));
            view.undo();
            view.undo();
        }
        check(levelWires, "the bands' wires into a join run level along their rows, bending only before it, each "
                          "band's + showing in its next place, and none between its nodes; an empty band's too");
        click(view, cell(3, 0)); // the split's card again, in the effect's place
        click(view, centerOf(detail::splitSoloButton(card, 1)));
        click(view, centerOf(detail::splitSoloButton(card, 0))); // and the low band's too, the high band left out
        const bool soloMid = model.value(paramIndex(0, SumParam::SoloMid)) == 1.0 &&
                             model.value(paramIndex(0, SumParam::SoloHigh)) == 0.0 &&
                             model.value(paramIndex(0, SumParam::SoloLow)) == 1.0;
        view.mouseMove({600.0f, 428.0f}, {});
        const bool soloShot = renderer.save(view, dir / "40f5-fx-split-three-solo.png");
        view.undo();
        view.undo();
        // The row shows the container with its middle band.
        click(view, centerOf(detail::fxModeButton(0)));
        view.mouseMove({600.0f, 700.0f}, {});
        const bool rowShot = renderer.save(view, dir / "40f6-fx-row-split-three.png");
        click(view, centerOf(detail::fxModeButton(1)));
        click(view, cell(3, 0));
        const int lowEnd = from(join, 1); // what the low band ends in
        click(view, centerOf(detail::splitBandsButton(card, 2)));
        view.mouseMove({600.0f, 428.0f}, {});
        const bool asked = model.value(paramIndex(0, SplitParam::Bands)) == 3.0 &&
                           renderer.save(view, dir / "40f7-fx-split-three-confirm.png");
        click(view, centerOf(detail::confirmBandButton(detail::confirmBandsPanel(view.height()), 2)));
        const bool ontoLow = model.value(paramIndex(0, SplitParam::Bands)) == 2.0 && from(join, 2) < 0 &&
                             from(widener, 0) == lowEnd && from(join, 1) == fxSlotPort(widener) &&
                             model.value(paramIndex(widener, FxParam::Type)) == static_cast<double>(FxType::Widener);
        view.undo();
        const bool backOnMiddle = model.value(paramIndex(0, SplitParam::Bands)) == 3.0 &&
                                  from(join, 2) == fxSlotPort(widener) && from(widener, 0) == middle;
        view.undo(); // the widener gone, the middle band empty
        click(view, centerOf(detail::splitBandsButton(card, 2)));
        const bool straight = model.value(paramIndex(0, SplitParam::Bands)) == 2.0 && from(join, 2) < 0;
        view.undo();
        view.undo();
        std::printf("      three bands: split %d, an effect on the middle band %d, soloed alone %d, asked %d, onto the "
                    "low band %d, undone %d, an empty one straight back %d\n",
                    three, onMiddle, soloMid, asked, ontoLow, backOnMiddle, straight);
        // A split at 6 dB switched to three bands goes to 12 dB, which they have, in the same undo step.
        model.set(paramIndex(0, SplitParam::Slope), 0.0);
        view.poll();
        click(view, centerOf(detail::splitBandsButton(card, 3)));
        const bool sixTo12 = model.value(paramIndex(0, SplitParam::Bands)) == 3.0 &&
                             model.value(paramIndex(0, SplitParam::Slope)) == 1.0;
        view.undo();
        const bool sixBack = model.value(paramIndex(0, SplitParam::Bands)) == 2.0 &&
                             model.value(paramIndex(0, SplitParam::Slope)) == 0.0;
        model.set(paramIndex(0, SplitParam::Slope), 1.0);
        view.poll();
        check(threeSlopes && sixTo12 && sixBack,
              "three bands keep the split's slope, 12 or 24 dB (6 going to 12 as they split), their list refusing 6");
        check(three && onMiddle && soloMid && soloShot && rowShot && asked && ontoLow && backOnMiddle && straight &&
                  model.value(paramIndex(0, SplitParam::Bands)) == 2.0 && from(join, 2) < 0 &&
                  model.value(paramIndex(0, SplitParam::High)) == highBefore,
              "BANDS 3 splits in three, an empty middle band shown between, and BANDS 2 asks where its effects go");
    }

    // + EFFECT with the split picked: a flanger at the end of its high band, before the join; undone.
    addFx(view, FxType::Flanger);
    const bool onBand = from(5, 0) == fxSlotPort(2) && from(join, 0) == fxSlotPort(5);
    view.undo();
    check(onBand && from(join, 0) == fxSlotPort(2) && fxChain(model) == std::vector<int>{0, 1, 2, 3, 4},
          "+ EFFECT with a split picked puts the effect on one of its bands");

    // In the graph, a card in the inspector has no x (its node's x removes it): the reverb's power switch sits in the
    // corner where the x is in the row, switching it off; undone. The split's card has none either.
    {
        click(view, cell(2, 0));
        view.mouseMove({600.0f, 428.0f}, {});
        const bool powerShot = renderer.save(view, dir / "40f9-fx-inspector-power.png");
        click(view, centerOf(detail::cardPowerInGraph(card)));
        const bool off = model.value(paramIndex(1, FxParam::Enabled)) == 0.0 &&
                         model.value(paramIndex(1, FxParam::Type)) == static_cast<double>(FxType::Reverb);
        view.undo();
        click(view, cell(3, 0));
        click(view, centerOf(detail::cardClose(card)));
        // Where the dialog's Keep would be: had the corner asked, that would take the split out.
        click(view, centerOf(detail::confirmButton(detail::confirmPanel(view.height(), true), 1)));
        check(powerShot && off && model.value(paramIndex(1, FxParam::Enabled)) == 1.0 &&
                  model.value(paramIndex(0, SplitParam::Used)) == 1.0,
              "a card in the graph's inspector has its power switch in the corner, and no x");
    }

    // The split taken out by its node's x asks first, its bands holding effects: Cancel keeps it; Keep puts its bands'
    // effects on the main line, high band first; Delete takes them with it. Each undone.
    const Point splitNodeAt = cell(3, 0);
    const Point splitXAt{splitNodeAt.x + detail::kFxSplitHalfW - 2.0f, splitNodeAt.y - detail::kFxSplitHalfH + 2.0f};
    auto clickSplitX = [&] {
        view.mouseMove(splitNodeAt, {});
        view.mouseMove(splitXAt, {});
        click(view, splitXAt);
    };
    const Rect removePanel = detail::confirmPanel(view.height(), true);
    clickSplitX();
    view.mouseMove({600.0f, 428.0f}, {});
    const bool removeAsked = model.value(paramIndex(0, SplitParam::Used)) == 1.0 &&
                             renderer.save(view, dir / "40g0-fx-split-remove-confirm.png");
    click(view, centerOf(detail::confirmButton(removePanel, 0)));
    const bool cancelled = model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(join, 1) == fxSlotPort(3);
    clickSplitX();
    click(view, centerOf(detail::confirmButton(removePanel, 1)));
    const bool inRow = model.value(paramIndex(0, SplitParam::Used)) == 0.0 &&
                       model.value(paramIndex(0, SumParam::Used)) == 0.0 && from(2, 0) == fxSlotPort(1) &&
                       from(3, 0) == fxSlotPort(2) && from(4, 0) == fxSlotPort(3) &&
                       from(kFxOutNode, 0) == fxSlotPort(4);
    const bool rowShot = renderer.save(view, dir / "40g-fx-split-removed.png");
    view.undo();
    const bool keptUndone = model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(join, 1) == fxSlotPort(3);
    click(view, cell(3, 0));
    clickSplitX();
    click(view, centerOf(detail::confirmThirdButton(removePanel)));
    const bool deleted = model.value(paramIndex(0, SplitParam::Used)) == 0.0 &&
                         model.value(paramIndex(2, FxParam::Type)) == static_cast<double>(FxType::None) &&
                         model.value(paramIndex(3, FxParam::Type)) == static_cast<double>(FxType::None) &&
                         from(4, 0) == fxSlotPort(1) && from(kFxOutNode, 0) == fxSlotPort(4);
    const bool deleteShot = renderer.save(view, dir / "40g1-fx-split-deleted.png");
    view.undo();
    std::printf("      split removed: asked %d, cancelled %d, kept %d, undone %d, deleted %d\n", removeAsked, cancelled,
                inRow, keptUndone, deleted);
    check(removeAsked && cancelled && inRow && rowShot && keptUndone && deleted && deleteShot &&
              model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(join, 1) == fxSlotPort(3) &&
              model.value(paramIndex(2, FxParam::Type)) != static_cast<double>(FxType::None),
          "a split taken out asks whether its bands' effects stay: kept on the main line or deleted with it, undoably");

    // A wire's middle opens the list too, beside it: between the shaper and the reverb, a filter goes in there, and a
    // split goes in with its join right after it; on the wire from the delay into the join, the list has no split,
    // and a widener goes on the high band before the join. Each undone.
    auto middle = [](Point from, Point to) { return Point{(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f}; };
    auto gapItem = [&](Point gap, bool withNode, FxType type) { return fxListItem(view, besidePlus(gap), type, withNode); };
    const Point mainGap = middle({cell(1, 0).x + detail::kFxNodeR, cell(1, 0).y},
                                 {cell(2, 0).x - detail::kFxNodeR - 8.0f, cell(2, 0).y});
    view.mouseMove(mainGap, {});
    const bool gapShot = renderer.save(view, dir / "40i-fx-gap.png");
    click(view, mainGap);
    click(view, gapItem(mainGap, true, FxType::Filter));
    const bool between = from(5, 0) == fxSlotPort(0) && from(1, 0) == fxSlotPort(5);
    view.undo();
    click(view, mainGap);
    click(view, fxListSplit(view, besidePlus(mainGap)));
    const int split2 = fxSplitNode(1), join2 = fxSumNode(1);
    const bool splitBetween = model.value(paramIndex(1, SplitParam::Used)) == 1.0 &&
                              model.value(paramIndex(1, SumParam::Used)) == 1.0 && from(split2, 0) == fxSlotPort(0) &&
                              from(join2, 0) == fxBandPort(1, kFxBands - 1) && from(join2, 1) == fxBandPort(1, 0) &&
                              from(1, 0) == fxSumPort(1);
    // Its bands are empty: each shows its + with the mouse away (read off the rendered pixels).
    view.mouseMove({600.0f, 428.0f}, {});
    const bool splitShot = renderer.save(view, dir / "40j-fx-gap-split.png");
    bool emptyPluses = true;
    for (const int row : {-1, 1}) {
        const Point plus = cell(3, row);
        emptyPluses = emptyPluses && renderer.pixel({plus.x + 6.0f, plus.y + 6.0f}) != renderer.pixel({plus.x, plus.y + 30.0f});
    }
    view.undo();
    const Point bandGap = cell(5, -1); // the delay's +, in its band's next place
    click(view, bandGap);
    click(view, gapItem(bandGap, false, FxType::Widener));
    const bool onJoinedBand = from(5, 0) == fxSlotPort(2) && from(join, 0) == fxSlotPort(5);
    view.undo();
    check(gapShot && between && splitBetween && splitShot && emptyPluses && onJoinedBand && from(1, 0) == fxSlotPort(0) &&
              model.value(paramIndex(1, SplitParam::Used)) == 0.0 && from(join, 0) == fxSlotPort(2) &&
              fxChain(model) == std::vector<int>{0, 1, 2, 3, 4},
          "a wire's middle puts an effect there, or a split with its join right after it, its empty bands showing their +");

    // What goes through a node lights it: the shaper's output at half scale in the telemetry, a halo round it and a
    // glow along its wire out (read off the rendered pixels), gone again once it is silent.
    {
        const Point shaperAt = cell(1, 0);
        const Point halo{shaperAt.x, shaperAt.y - detail::kFxNodeR - 4.0f};
        const Point along{(shaperAt.x + detail::kFxNodeR + cell(2, 0).x - detail::kFxNodeR - 8.0f) * 0.5f, shaperAt.y + 3.0f};
        model.glowLevel = 0.5; // the glow setting where it starts; off, nothing glows
        view.mouseMove({600.0f, 428.0f}, {});
        renderer.save(view, dir / "40v-fx-quiet.png");
        const uint32_t quietHalo = renderer.pixel(halo), quietAlong = renderer.pixel(along);
        model.live.fxPortLevels[fxSlotPort(0)] = 0.5f;
        for (int i = 0; i < 60; ++i) // rising to it over a moment
            view.poll();
        const bool shot = renderer.save(view, dir / "40v-fx-glow.png");
        const bool lit = renderer.pixel(halo) != quietHalo && renderer.pixel(along) != quietAlong;
        model.glowLevel = 0.0;
        renderer.save(view, dir / "40v-fx-glow-off.png");
        const bool offDark = renderer.pixel(halo) == quietHalo;
        model.live.fxPortLevels[fxSlotPort(0)] = 0.0f;
        for (int i = 0; i < 60; ++i) // and falling back
            view.poll();
        renderer.save(view, dir / "40v-fx-quiet.png");
        check(shot && lit && offDark && renderer.pixel(halo) == quietHalo,
              "a node of the effects graph and its wire out glow as loud as what comes out of it, unless the glow "
              "setting is off");
    }

    // The mouse over the reverb shows an x in a circle at its upper right (read off the rendered pixels: none there
    // before), whose click removes it, the shaper going on into the split; undone.
    {
        const Point reverbAt = cell(2, 0);
        const Point x{reverbAt.x + detail::kFxNodeR - 2.0f, reverbAt.y - detail::kFxNodeHalfH + 2.0f}; // its corner
        view.mouseMove({600.0f, 428.0f}, {});
        renderer.save(view, dir / "40n-fx-node-plain.png");
        const uint32_t plain = renderer.pixel({x.x + 7.5f, x.y});
        view.mouseMove(reverbAt, {});
        view.mouseMove(x, {});
        const bool shot = renderer.save(view, dir / "40n-fx-node-x.png");
        const bool drawn = renderer.pixel({x.x + 7.5f, x.y}) != plain;
        click(view, x);
        const bool removed = model.value(paramIndex(1, FxParam::Type)) == static_cast<double>(FxType::None) &&
                             from(split, 0) == fxSlotPort(0);
        view.undo();
        view.mouseMove({600.0f, 428.0f}, {});
        check(shot && drawn && removed && from(1, 0) == fxSlotPort(0) && from(split, 0) == fxSlotPort(1),
              "a node under the mouse shows an x at its upper right, which removes it");
    }

    // And a power symbol in a circle at its upper left (read off the rendered pixels: none there before), whose click
    // switches the effect off and on again, each an undo step.
    {
        const Point reverbAt = cell(2, 0);
        const Point on{reverbAt.x - detail::kFxNodeR + 2.0f, reverbAt.y - detail::kFxNodeHalfH + 2.0f}; // its corner
        const int enabled = paramIndex(1, FxParam::Enabled);
        view.mouseMove({600.0f, 428.0f}, {});
        renderer.save(view, dir / "40n-fx-node-plain.png");
        const uint32_t plain = renderer.pixel({on.x - 7.5f, on.y});
        view.mouseMove(reverbAt, {});
        view.mouseMove(on, {});
        const bool shot = renderer.save(view, dir / "40n-fx-node-power.png");
        const bool drawn = renderer.pixel({on.x - 7.5f, on.y}) != plain;
        click(view, on);
        const bool off = model.value(enabled) == 0.0 && fxChain(model) == std::vector<int>{0, 1, 2, 3, 4};
        const bool offShot = renderer.save(view, dir / "40n-fx-node-off.png");
        click(view, on);
        const bool backOn = model.value(enabled) == 1.0;
        view.undo();
        const bool undone = model.value(enabled) == 0.0;
        view.undo();
        view.mouseMove({600.0f, 428.0f}, {});
        check(shot && drawn && off && offShot && backOn && undone && model.value(enabled) == 1.0,
              "an effect's node under the mouse shows a power switch at its upper left, which switches it off and on");
    }

    // Nodes dragged and dropped, the others gliding aside to make room while one is held: the phaser between the
    // shaper and the reverb, the delay from the high band to the end of the low band, and the split with its bands
    // and join, by the join, to the line's start. Each one undo step, undone.
    {
        auto settle = [&] {
            for (int k = 0; k < 40; ++k)
                view.poll();
        };
        auto dragNode = [&](Point from, Point to, const char *shot) {
            view.mouseMove(from, {});
            view.mouseDown(from, {});
            view.mouseMove({from.x + 8.0f, from.y + 4.0f}, {});
            view.mouseMove(to, {});
            settle(); // the others glide aside
            const bool saved = shot ? renderer.save(view, dir / shot) : true;
            view.mouseUp(to, {});
            settle();
            return saved;
        };
        // A step into the glide, the phaser's placeholder is where it would land already, the wire from the shaper
        // ending in its arrowhead there, as at a node (read off the rendered pixels: how many pixels down through the
        // arrowhead are lit, many more than a wire passing by lights).
        {
            const Point grab = cell(7, 0), to = detail::fxGridAt(1.5f, 0.0f, 0.0f);
            view.mouseMove(grab, {});
            view.mouseDown(grab, {});
            view.mouseMove({grab.x + 8.0f, grab.y + 4.0f}, {});
            view.mouseMove(to, {});
            view.poll();
            renderer.save(view, dir / "40o0-fx-drag-step.png");
            const float x = cell(2, 0).x - detail::kFxNodeR - 5.0f; // in the arrowhead, short of its tip
            int lit = 0;
            for (float y = cell(2, 0).y - 8.0f; y <= cell(2, 0).y + 8.0f; y += 1.0f) {
                const uint32_t color = renderer.pixel({x, y});
                lit += std::max({color >> 16 & 255, color >> 8 & 255, color & 255}) > 70u;
            }
            view.mouseUp(to, {});
            view.undo();
            settle();
            std::printf("      a step into the glide, lit down the arrowhead at the phaser's new place: %d px\n", lit);
            check(lit >= 5, "a dragged node's wires run to its placeholder where it would land, not along with it");
        }
        const bool roomShot = dragNode(cell(7, 0), detail::fxGridAt(1.5f, 0.0f, 0.0f), "40o-fx-drag-room.png");
        const bool phaserMoved = from(4, 0) == fxSlotPort(0) && from(1, 0) == fxSlotPort(4) &&
                                 from(kFxOutNode, 0) == fxSumPort(0);
        const bool droppedShot = renderer.save(view, dir / "40p-fx-dropped.png");
        view.undo();
        const bool phaserBack = from(4, 0) == fxSumPort(0) && from(1, 0) == fxSlotPort(0);
        dragNode(cell(4, -1), detail::fxGridAt(5.0f, 1.0f, 0.0f), nullptr); // onto the low band's +
        const bool delayMoved = from(3, 0) == low && from(2, 0) == fxSlotPort(3) && from(join, 1) == fxSlotPort(2) &&
                                from(join, 0) == high;
        view.undo();
        const bool groupShot = dragNode(cell(6, 0), detail::fxGridAt(0.5f, 0.0f, 0.0f), "40q-fx-drag-group.png");
        const bool groupMoved = from(split, 0) == kFxPortIn && from(0, 0) == fxSumPort(0) && from(4, 0) == fxSlotPort(1) &&
                                from(join, 0) == fxSlotPort(2) && from(join, 1) == fxSlotPort(3) &&
                                from(kFxOutNode, 0) == fxSlotPort(4);
        view.undo();
        // The phaser held over that gap, then over the reverb beyond it (where the gap opened pushed it): the place
        // stays open where it was, and that is where it lands.
        view.mouseMove(cell(7, 0), {});
        view.mouseDown(cell(7, 0), {});
        view.mouseMove({cell(7, 0).x + 8.0f, cell(7, 0).y + 4.0f}, {});
        view.mouseMove(detail::fxGridAt(1.5f, 0.0f, 0.0f), {});
        settle();
        view.mouseMove(detail::fxGridAt(3.0f, 0.0f, 0.0f), {});
        settle();
        view.mouseUp(detail::fxGridAt(3.0f, 0.0f, 0.0f), {});
        settle();
        const bool held = from(4, 0) == fxSlotPort(0) && from(1, 0) == fxSlotPort(4);
        view.undo();
        view.mouseMove({600.0f, 428.0f}, {});
        check(held, "a dragged node's place stays open until it is over another gap");
        // The phaser let go of with the mouse on the low band's wire just past the compressor, well short of the gap
        // at the band's +: anywhere on a wire, it lands where that wire goes, at the band's end; undone.
        {
            const bool shot = dragNode(cell(7, 0), detail::fxGridAt(4.4f, 1.0f, 0.0f), "40q1-fx-drop-on-wire.png");
            const bool onWire = from(4, 0) == fxSlotPort(3) && from(join, 1) == fxSlotPort(4) &&
                                from(kFxOutNode, 0) == fxSumPort(0);
            view.undo();
            settle();
            view.mouseMove({600.0f, 428.0f}, {});
            check(shot && onWire && from(4, 0) == fxSumPort(0) && from(join, 1) == fxSlotPort(3),
                  "a node let go of anywhere on a wire lands where that wire goes");
        }
        // While a node is dragged no + shows: the delay held over the low band's end, the high band left empty and its
        // + gone, its wire running straight through where it was, and the main line's + after the phaser gone too
        // (read off the rendered pixels: a +'s upright stroke against its disc beside it, where without a + both are
        // the background, and the wire against the background above it). Let go of there, undone.
        {
            auto plusAt = [&](Point p) {
                return renderer.pixel({p.x, p.y - 3.0f}) != renderer.pixel({p.x + 6.0f, p.y - 3.0f});
            };
            auto wireAt = [&](Point p) { return renderer.pixel(p) != renderer.pixel({p.x, p.y - 10.0f}); };
            renderer.save(view, dir / "40q2-fx-pluses.png");
            const bool before = plusAt(cell(8, 0)) && plusAt(cell(5, -1));
            const Point grab = cell(4, -1), to = detail::fxGridAt(5.0f, 1.0f, 0.0f);
            view.mouseMove(grab, {});
            view.mouseDown(grab, {});
            view.mouseMove({grab.x + 8.0f, grab.y + 4.0f}, {});
            view.mouseMove(to, {});
            settle();
            const bool shot = renderer.save(view, dir / "40q3-fx-drag-no-pluses.png");
            // The high band empty, its + would be a column on from the split; the main line's a column further on, the
            // low band two nodes long.
            const bool hidden = !plusAt(cell(4, -1)) && wireAt(cell(4, -1)) && !plusAt(cell(9, 0)) && wireAt(cell(9, 0));
            view.mouseUp(to, {});
            settle();
            view.undo();
            settle();
            view.mouseMove({600.0f, 428.0f}, {});
            std::printf("      +s before dragging %d, hidden while dragging %d\n", before, hidden);
            check(before && shot && hidden && from(2, 0) == high && from(join, 1) == fxSlotPort(3),
                  "while a node is dragged no + shows, an empty band's and the main line's too, the wires running "
                  "through");
        }
        check(roomShot && phaserMoved && droppedShot && phaserBack && delayMoved && groupShot && groupMoved &&
                  from(0, 0) == kFxPortIn && from(split, 0) == fxSlotPort(1) && from(2, 0) == high &&
                  fxChain(model) == std::vector<int>{0, 1, 2, 3, 4},
              "a node dragged onto another place moves there, a split with its bands and join, undoably");
    }

    // The mouse anywhere along a wire shows the + at its middle (read off the rendered pixels), though a click there
    // does nothing: only the + takes it.
    {
        const Point a{cell(1, 0).x + detail::kFxNodeR, cell(1, 0).y}, b{cell(2, 0).x - detail::kFxNodeR - 8.0f, cell(2, 0).y};
        const Point mid{(a.x + b.x) * 0.5f, a.y};
        view.mouseMove({600.0f, 428.0f}, {});
        renderer.save(view, dir / "40r-fx-wire-plain.png");
        const uint32_t plain = renderer.pixel({mid.x + 6.0f, mid.y + 6.0f});
        view.mouseMove({a.x + 5.0f, a.y + 1.0f}, {});
        const bool shot = renderer.save(view, dir / "40r-fx-wire-hover.png");
        const bool shown = renderer.pixel({mid.x + 6.0f, mid.y + 6.0f}) != plain;
        // Faint until the mouse is on it, as the + after the last effect (read off the rendered pixels: the brightest
        // along the +'s bar, with the mouse along the wire and on the +, and away from and on the line's +).
        auto bar = [&](Point m) {
            const uint32_t color = brightestAlong(renderer, {m.x - 3.0f, m.y}, {m.x + 3.0f, m.y});
            return std::max({static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                             static_cast<int>(color & 255)});
        };
        const int alongWire = bar(mid);
        view.mouseMove(mid, {});
        renderer.save(view, dir / "40r2-fx-wire-plus-hot.png");
        const int onWirePlus = bar(mid);
        const Point linePlus = cell(8, 0);
        const int lineIdle = bar(linePlus);
        view.mouseMove(linePlus, {});
        renderer.save(view, dir / "40r3-fx-line-plus-hot.png");
        const int lineHot = bar(linePlus);
        std::printf("      a wire's +: %d along the wire, %d on it; the line's +: %d away, %d on it\n", alongWire,
                    onWirePlus, lineIdle, lineHot);
        check(alongWire + 60 < onWirePlus && lineIdle + 60 < lineHot,
              "the graph's +s are faint until the mouse is on them");
        view.mouseMove({a.x + 5.0f, a.y + 1.0f}, {});
        click(view, {a.x + 5.0f, a.y + 1.0f});
        // Had that opened the list, this click would only close it.
        click(view, mid);
        click(view, fxListItem(view, besidePlus(mid), FxType::Filter, true));
        const bool added = from(5, 0) == fxSlotPort(0) && from(1, 0) == fxSlotPort(5);
        view.undo();
        view.mouseMove({600.0f, 428.0f}, {});
        check(shot && shown && added && from(1, 0) == fxSlotPort(0),
              "the mouse along a wire shows its +, which alone takes the click");
    }

    // Back to the row: two views of the same line, so nothing asks and nothing changes; the split shows as its
    // container there.
    const std::vector<int> before = fxChain(model);
    std::vector<double> wires;
    for (int i = kFxSplitBase; i < kShapeBase; ++i)
        wires.push_back(model.value(i));
    click(view, centerOf(detail::fxModeButton(0)));
    view.mouseMove({600.0f, 600.0f}, {});
    bool same = fxChain(model) == before;
    for (int i = kFxSplitBase; i < kShapeBase; ++i)
        same = same && model.value(i) == wires[i - kFxSplitBase];
    check(!model.ui.fxGraphView && same && model.value(paramIndex(0, SplitParam::Used)) == 1.0 &&
              renderer.save(view, dir / "40h-fx-to-chain.png"),
          "going back to the row changes nothing: the split shows there as its container");
}

// A split in the row (the chain view), the same line the graph shows: the arrow between two cards offers a split,
// which goes in there as a container, its low band shown and empty; a card dragged onto the band it shows goes on
// that band; its other tab shows the other band, whose dashed card adds an effect to it (the list offering no
// split); its knobs set the bands' levels, its S buttons solo one band at a time, and its crossover's handle drags;
// switching to the graph and back changes nothing; it folds to a strip and back; dragged by its title, it moves along
// the line with its bands; its x puts its bands' effects on the main line, the high band's first; and a card carried
// over a tab shows that band, to land on it.
// A split first in the row, put there by the arrow before the first card, the widener and the delay after it: the
// delay carried left over the widener, the container's band and its controls lands on the band, and on past the
// controls' left edge, where the panel's edge is, before the container, at the line's start.
void testFxRowSplitFirst(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Widener);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Delay);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    auto centerOf = [](Rect r) { return Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    auto from = [&](int node, int input) {
        return fxPortOf(model.value(fxInputIndex(node, input, FxInputField::From)));
    };
    auto settle = [&] {
        for (int k = 0; k < 40; ++k)
            view.poll();
    };
    const int split = fxSplitNode(0), low = fxBandPort(0, 0);
    const Point arrow{detail::fxCard(0.0f).x - detail::kFxGap * 0.5f, detail::fxCard(0.0f).center().y};
    click(view, arrow);
    const Rect menu = detail::fxMenu({0.0f, 0.0f}, true);
    const float left = detail::kFxPanel.x + 8.0f, right = detail::kFxPanel.right() - 8.0f - menu.w;
    const Point at{std::clamp(arrow.x - 0.5f * menu.w, left, right),
                   std::min(detail::kFxTop + 8.0f, detail::kFxPanel.bottom() - 4.0f - menu.h)};
    click(view, centerOf(detail::fxMenuNodeItem(at)));
    view.mouseMove({600.0f, 600.0f}, {});
    settle();
    const bool first = from(split, 0) == kFxPortIn && from(0, 0) == fxSumPort(0) && from(1, 0) == fxSlotPort(0);

    const Rect column = detail::fxCard(detail::kFxBoxEdge); // the container's controls
    const float delayAt = 2.0f + detail::kFxBoxAddSpan + 2.0f * detail::kFxBoxEdge;
    const Rect delayCard = detail::fxCard(delayAt);
    const Point grip{delayCard.x + 40.0f, delayCard.y + 17.0f};
    auto carry = [&](std::vector<Point> path) {
        view.mouseDown(grip, {});
        view.mouseMove({grip.x - 30.0f, grip.y}, {});
        for (const Point p : path) {
            view.mouseMove(p, {});
            settle();
        }
        view.mouseUp(path.back(), {});
        settle();
    };
    const std::vector<Point> over{{detail::fxCard(delayAt - 1.0f).x + 40.0f, grip.y}, // the widener
                                  {detail::fxCard(1.0f + detail::kFxBoxEdge).x + 20.0f, grip.y}, // the band's card
                                  {column.x + 100.0f, grip.y}};                                    // its controls
    carry(over);
    const bool onBand = from(1, 0) == low && from(0, 0) == fxSumPort(0);
    view.undo();
    settle();
    std::vector<Point> out = over;
    out.push_back({detail::kFxPanel.x + 4.0f, grip.y}); // past the controls' left edge, at the panel's
    carry(out);
    view.mouseMove({600.0f, 600.0f}, {});
    const bool shot = renderer.save(view, dir / "41a1-fx-row-before-first-split.png");
    const bool before = from(1, 0) == kFxPortIn && from(split, 0) == fxSlotPort(1) && from(0, 0) == fxSumPort(0);
    view.undo();
    settle();
    std::printf("      a split first: put there %d, the delay onto its band %d, before it %d\n", first, onBand, before);
    check(first && onBand && before && shot && from(split, 0) == kFxPortIn && from(1, 0) == fxSlotPort(0),
          "a card carried left past a container first in the row, beyond its controls, lands before it");
}

void testFxRow(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const FxType types[] = {FxType::Shaper, FxType::Widener, FxType::Delay};
    for (int slot = 0; slot < 3; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(types[slot]);
    setChain(model, {0, 1, 2});
    view.poll();
    click(view, kFxTab);
    auto centerOf = [](Rect r) { return Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    auto from = [&](int node, int input) {
        return fxPortOf(model.value(fxInputIndex(node, input, FxInputField::From)));
    };
    const int split = fxSplitNode(0), join = fxSumNode(0);
    const int high = fxBandPort(0, kFxBands - 1), low = fxBandPort(0, 0);
    const Rect column = detail::fxCard(1.0f + detail::kFxBoxEdge); // the container's controls, in the second place
    auto settle = [&] { // the cards gliding into their places
        for (int k = 0; k < 40; ++k)
            view.poll();
    };
    auto carry = [&](Point grip, std::vector<Point> path) { // a card by its title, along `path`, let go of at its end
        view.mouseDown(grip, {});
        for (const Point p : path) {
            view.mouseMove(p, {});
            settle();
        }
        view.mouseUp(path.back(), {});
        settle();
    };
    const Point bandCard{detail::fxCard(2.0f + detail::kFxBoxEdge).x + 20.0f, 0.0f}; // the container's dashed card's x

    // The shaper dragged by its title, near its left, a tenth of a card to the right: nothing moves. Three tenths, the
    // mouse still over the shaper's own place: it covers that much of the widener, which makes way at once, gliding
    // from the first step (read off the rendered pixels along the cards' title row: the row as the move left it, its
    // outline where the shaper would land over the widener, against a step on, the widener coming out from under it),
    // and let go of there the shaper goes after it. Undone.
    {
        const float pitch = detail::kFxCardW + detail::kFxGap;
        const Rect shaper = detail::fxCard(0.0f);
        const Point grip{shaper.x + 40.0f, shaper.y + 17.0f};
        auto rowPixels = [&] {
            std::vector<uint32_t> line;
            for (float x = detail::fxCard(0.0f).x; x < detail::fxCard(1.0f).x - 2.0f; x += 2.0f) // clear of the outline
                line.push_back(renderer.pixel({x, grip.y}));
            return line;
        };
        view.mouseDown(grip, {});
        view.mouseMove({grip.x + 0.1f * pitch, grip.y}, {});
        view.mouseUp({grip.x + 0.1f * pitch, grip.y}, {});
        settle();
        const bool stayed = fxChain(model) == std::vector<int>{0, 1, 2};
        view.mouseDown(grip, {});
        view.mouseMove({grip.x + 0.1f * pitch, grip.y}, {});
        view.mouseMove({grip.x + 0.3f * pitch, grip.y}, {});
        renderer.save(view, dir / "41a0-fx-row-nudge-before.png");
        const std::vector<uint32_t> before = rowPixels();
        view.poll();
        const bool shot = renderer.save(view, dir / "41a0-fx-row-nudge.png");
        const bool gliding = rowPixels() != before;
        view.mouseUp({grip.x + 0.3f * pitch, grip.y}, {});
        settle();
        const bool after = fxChain(model) == std::vector<int>{1, 0, 2};
        view.undo();
        settle();
        view.mouseMove({600.0f, 600.0f}, {});
        std::printf("      the shaper nudged: a tenth stays %d, three tenths glides %d and lands after %d\n", stayed,
                    gliding, after);
        check(stayed && shot && gliding && after && fxChain(model) == std::vector<int>{0, 1, 2},
              "a card dragged onto its neighbor makes it glide aside at once, a little of the way moving nothing");
    }

    // The arrow between the shaper and the widener: its list ends with the split, which goes in there.
    const Point arrow{detail::fxCard(1.0f).x - detail::kFxGap * 0.5f, detail::fxCard(1.0f).center().y};
    click(view, arrow);
    const Rect menu = detail::fxMenu({0.0f, 0.0f}, true); // kept inside the panel
    const float left = detail::kFxPanel.x + 8.0f, right = detail::kFxPanel.right() - 8.0f - menu.w;
    const Point at{std::clamp(arrow.x - 0.5f * menu.w, left, right),
                   std::min(detail::kFxTop + 8.0f, detail::kFxPanel.bottom() - 4.0f - menu.h)};
    const Point splitItem = centerOf(detail::fxMenuNodeItem(at));
    view.mouseMove(splitItem, {});
    const bool listShot = renderer.save(view, dir / "41a-fx-row-split-list.png");
    click(view, splitItem);
    const bool added = model.value(paramIndex(0, SplitParam::Used)) == 1.0 &&
                       model.value(paramIndex(0, SumParam::Used)) == 1.0 && from(split, 0) == fxSlotPort(0) &&
                       from(join, 0) == high && from(join, 1) == low && from(1, 0) == fxSumPort(0) &&
                       from(2, 0) == fxSlotPort(1) && model.ui.splitBand[0] == 0 && !model.ui.fxGraphView;
    view.mouseMove({600.0f, 600.0f}, {});
    check(listShot && added && renderer.save(view, dir / "41b-fx-row-split.png"),
          "the arrow between two cards puts a split there: a container, its low band shown, empty");

    // The widener, after the container, carried left over its band and its controls: let go of over the shaper's right
    // half it lands between the shaper and the split; carried on over the shaper's left half, before the shaper. Each
    // undone.
    {
        const Rect widenerCard = detail::fxCard(2.0f + detail::kFxBoxAddSpan + 2.0f * detail::kFxBoxEdge);
        const Point grip{widenerCard.x + 40.0f, widenerCard.y + 17.0f};
        const Rect shaperCard = detail::fxCard(0.0f);
        auto carryLeft = [&](float along) { // over the band, the controls, then the shaper `along` its width
            carry(grip, {{grip.x - 30.0f, grip.y}, {bandCard.x, grip.y}, {column.x + 100.0f, grip.y},
                         {column.x + 10.0f, grip.y}, {shaperCard.x + along * shaperCard.w, grip.y}});
        };
        carryLeft(0.75f);
        const bool beforeSplit = from(1, 0) == fxSlotPort(0) && from(split, 0) == fxSlotPort(1) &&
                                 from(2, 0) == fxSumPort(0);
        view.undo();
        settle();
        carryLeft(0.25f);
        const bool first = from(1, 0) == kFxPortIn && from(0, 0) == fxSlotPort(1) && from(split, 0) == fxSlotPort(0);
        view.undo();
        settle();
        view.mouseMove({600.0f, 600.0f}, {});
        std::printf("      the widener carried left past the container: between it and the shaper %d, before the "
                    "shaper %d\n",
                    beforeSplit, first);
        check(beforeSplit && first && from(1, 0) == fxSumPort(0) && from(split, 0) == fxSlotPort(0),
              "a card carried left over a container's controls lands before the container, and then before the card "
              "before it");
    }

    // The shaper, before the container, carried right. Let go of over the container's controls, near their right edge
    // too, it stays where it was (the band starting only past them, as the container shows). Carried on over its band
    // and out of its right side, it lands right after the container, before the widener, until the mouse is past the
    // widener's middle (where the widener shows, a card on), where it lands after the widener. Each undone.
    {
        const Rect shaperCard = detail::fxCard(0.0f);
        const Point grip{shaperCard.x + 40.0f, shaperCard.y + 17.0f};
        const float widenerAt = 2.0f + detail::kFxBoxAddSpan + 2.0f * detail::kFxBoxEdge; // where it shows
        auto carryRight = [&](std::vector<Point> path) {
            path.insert(path.begin(), {grip.x + 30.0f, grip.y});
            carry(grip, std::move(path));
        };
        auto unchanged = [&] { return from(split, 0) == fxSlotPort(0) && from(0, 0) == kFxPortIn; };
        carryRight({{column.x + 100.0f, grip.y}, {column.right() - 12.0f, grip.y}});
        const bool overControls = unchanged();
        if (!overControls) { // moved after all: back, for what follows
            view.undo();
            settle();
        }
        const Point band{bandCard.x, grip.y};
        carryRight({{column.x + 100.0f, grip.y}, band, {detail::fxCard(widenerAt).x + 20.0f, grip.y}});
        const bool afterSplit = from(split, 0) == kFxPortIn && from(0, 0) == fxSumPort(0) && from(1, 0) == fxSlotPort(0);
        view.undo();
        settle();
        carryRight({{column.x + 100.0f, grip.y}, band,
                    {detail::fxCard(widenerAt).x + 20.0f, grip.y},
                    {detail::fxCard(widenerAt).x + 0.7f * detail::kFxCardW, grip.y}});
        const bool afterWidener = from(split, 0) == kFxPortIn && from(1, 0) == fxSumPort(0) &&
                                  from(0, 0) == fxSlotPort(1) && from(2, 0) == fxSlotPort(0);
        view.undo();
        settle();
        view.mouseMove({600.0f, 600.0f}, {});
        std::printf("      the shaper carried right: over the controls it stays %d, out of the container right after it "
                    "%d, past the widener's middle after the widener %d\n",
                    overControls, afterSplit, afterWidener);
        check(overControls, "a card carried over a container's controls stays where it was, up to their edge");
        check(afterSplit && afterWidener && unchanged(),
              "a card carried right out of a container lands right after it, and past the next card once the mouse is "
              "past its middle");
    }

    // The widener's card, after the container, dragged onto the container's dashed card: onto its low band.
    const Rect widener = detail::fxCard(2.0f + detail::kFxBoxAddSpan + 2.0f * detail::kFxBoxEdge);
    const Point grip{widener.x + 40.0f, widener.y + 17.0f};
    const Point into{detail::fxCard(2.0f + detail::kFxBoxEdge).x + 20.0f, grip.y};
    view.mouseDown(grip, {});
    view.mouseMove({grip.x - 30.0f, grip.y}, {});
    view.mouseMove(into, {});
    settle();
    const bool dragShot = renderer.save(view, dir / "41c-fx-row-drag-into-band.png");
    view.mouseUp(into, {});
    settle();
    check(dragShot && from(1, 0) == low && from(join, 1) == fxSlotPort(1) && from(join, 0) == high &&
              from(2, 0) == fxSumPort(0),
          "a card dragged onto the band a container shows goes on that band");

    // Its HIGH tab shows the high band, empty: its dashed card right after the controls opens the list of effects
    // (no split), and a reverb goes on that band.
    click(view, centerOf(detail::splitTab(column, 1, 2)));
    const bool highByTab = model.ui.splitBand[0] == 1;
    // A click on its display left of the crossover shows the low band, and right of it the high band again.
    {
        const Rect shown{column.x + 14.0f, column.y + 66.0f, column.w - 28.0f, 104.0f};
        click(view, {shown.x + 10.0f, shown.y + 30.0f});
        const bool lowByDisplay = model.ui.splitBand[0] == 0;
        click(view, {shown.right() - 10.0f, shown.y + 30.0f});
        check(highByTab && lowByDisplay && model.ui.splitBand[0] == 1,
              "a click on a split's display between two crossovers shows that band");
    }
    const bool highShown = model.ui.splitBand[0] == 1;
    const Rect add = detail::fxBoxAddCard(detail::fxCard(2.0f + detail::kFxBoxEdge));
    const Point addAt = centerOf(add);
    click(view, addAt);
    view.mouseMove(fxListItem(view, addAt, FxType::Reverb), {});
    const bool bandListShot = renderer.save(view, dir / "41d-fx-row-band-list.png");
    check(fxGroupStrip(renderer, fxListAt(view, addAt), detail::kFxMenuLeftGroups - 1) &&
              !fxGroupStrip(renderer, fxListAt(view, addAt), detail::kFxMenuSplitGroup),
          "a band's list of effects has no SPLIT group (read off the rendered pixels)");
    click(view, fxListItem(view, addAt, FxType::Reverb));
    check(highShown && bandListShot && from(3, 0) == high && from(join, 0) == fxSlotPort(3) &&
              model.value(paramIndex(3, FxParam::Type)) == static_cast<double>(FxType::Reverb),
          "a container's other tab shows that band, whose dashed card adds an effect to it");

    // Its Low knob takes the low band's level down, and the low band's curve on its display with it (read off the
    // rendered pixels: the bluest pixel at the display's low end); its high band's S solos it, then its low band's S
    // takes over; both undone. Its crossover's handle drags from 250 Hz to 1 kHz.
    auto lowCurveY = [&] {
        const Rect shown{column.x + 14.0f, column.y + 66.0f, column.w - 28.0f, 104.0f};
        float at = 0.0f;
        int most = -256;
        for (float y = shown.y + 4.0f; y < shown.bottom() - 22.0f; y += 1.0f) {
            const uint32_t color = renderer.pixel({shown.x + 8.0f, y});
            const int blueness = static_cast<int>(color & 255) - static_cast<int>(color >> 16 & 255);
            if (blueness > most) {
                most = blueness;
                at = y;
            }
        }
        return at;
    };
    view.mouseMove({600.0f, 600.0f}, {});
    const bool fullShot = renderer.save(view, dir / "41d2-fx-row-levels-full.png");
    const float fullY = lowCurveY();
    // An arrow leads into the reverb on the band, and none into the dashed card after it (read off the rendered pixels:
    // the brightest pixel where each would be, against the panel's above them).
    auto brightest = [&](Point m) {
        int most = 0;
        for (float x = m.x - 5.0f; x <= m.x + 5.0f; x += 1.0f)
            for (float y = m.y - 8.0f; y <= m.y + 8.0f; y += 1.0f) {
                const uint32_t color = renderer.pixel({x, y});
                most = std::max({most, static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                                 static_cast<int>(color & 255)});
            }
        return most;
    };
    auto arrowAt = [&](float place) {
        const Rect next = detail::fxCard(place);
        return Point{next.x - detail::kFxGap * 0.5f, next.center().y};
    };
    const Point intoReverb = arrowAt(2.0f + detail::kFxBoxEdge), intoAdd = arrowAt(3.0f + detail::kFxBoxEdge);
    const int panel = brightest({intoAdd.x, intoAdd.y - 60.0f});
    check(fullShot && brightest(intoReverb) > panel + 40 && brightest(intoAdd) <= panel + 10,
          "a container's band has arrows between its cards, and none into its dashed card");
    const Rect reverbCard = detail::fxCard(2.0f + detail::kFxBoxEdge);
    const Point reverbIcon{reverbCard.x + detail::kFxIconX + 0.5f * detail::kFxIconW, reverbCard.y + 17.0f};
    const int reverbIconHeard = brightest(reverbIcon);
    const Point lowKnob = detail::splitKnob(column, 0);
    drag(view, lowKnob, {lowKnob.x, lowKnob.y + 40.0f});
    const double lowLevel = model.value(fxInputIndex(join, 1, FxInputField::Level));
    click(view, centerOf(detail::splitSoloButton(column, 2)));
    const bool soloHigh = model.value(paramIndex(0, SumParam::SoloHigh)) == 1.0 &&
                          model.value(paramIndex(0, SumParam::SoloLow)) == 0.0;
    click(view, centerOf(detail::splitSoloButton(column, 2))); // off again, each S on its own
    click(view, centerOf(detail::splitSoloButton(column, 0)));
    const bool soloLow = model.value(paramIndex(0, SumParam::SoloHigh)) == 0.0 &&
                         model.value(paramIndex(0, SumParam::SoloLow)) == 1.0;
    view.mouseMove({600.0f, 600.0f}, {});
    const bool soloShot = renderer.save(view, dir / "41e-fx-row-solo.png");
    const float loweredY = lowCurveY();
    std::printf("      the low band's curve at the display's low end: at %.0f px at full level, %.0f px at %.1f dB\n",
                fullY, loweredY, lowLevel);
    check(fullShot && soloShot && loweredY > fullY + 4.0f, "a band's level lowers its curve on the split's display");
    // With the low band soloed, the high band shown is faint, its reverb's icon dimmer (read off the rendered pixels).
    const int reverbIconSoloed = brightest(reverbIcon);
    std::printf("      the high band's reverb icon: %d, with the low band soloed %d\n", reverbIconHeard, reverbIconSoloed);
    check(reverbIconSoloed < reverbIconHeard - 60, "a container's band shows its cards faint while the other is soloed");
    view.undo();
    view.undo();
    view.undo();
    const Rect display{column.x + 14.0f, column.y + 66.0f, column.w - 28.0f, 104.0f};
    const float handleY = (display.y + 12.0f + display.bottom() - 22.0f) * 0.5f; // where the bands cross, at 12 dB
    drag(view, {display.x + static_cast<float>(normFromCutoff(250.0)) * display.w, handleY},
         {display.x + static_cast<float>(normFromCutoff(1000.0)) * display.w, handleY});
    check(lowLevel < -1.0 && model.value(fxInputIndex(join, 0, FxInputField::Level)) == 0.0 && soloHigh && soloLow &&
              soloShot && model.value(paramIndex(0, SumParam::SoloLow)) == 0.0 &&
              std::abs(model.value(paramIndex(0, SplitParam::Low)) - normFromCutoff(1000.0)) < 0.01 &&
              model.openEdits == 0,
          "a container's knobs set its bands' levels, its S buttons solo its bands, its crossover drags");

    // The graph and back: the same line, nothing changed.
    std::vector<double> before;
    for (int i = kFxSplitBase; i < kShapeBase; ++i)
        before.push_back(model.value(i));
    click(view, centerOf(detail::fxModeButton(1)));
    view.mouseMove({600.0f, 600.0f}, {});
    const bool graphShot = model.ui.fxGraphView && renderer.save(view, dir / "41f-fx-row-as-graph.png");
    click(view, centerOf(detail::fxModeButton(0)));
    bool same = !model.ui.fxGraphView;
    for (int i = kFxSplitBase; i < kShapeBase; ++i)
        same = same && model.value(i) == before[i - kFxSplitBase];
    check(graphShot && same, "switching to the graph and back leaves the split as it was");

    // Folded to a strip by the chevron left of its x, and unfolded by a click on the strip.
    click(view, centerOf(detail::cardFold(column, false)));
    const bool folded = model.ui.splitsFolded == 1u;
    view.mouseMove({600.0f, 600.0f}, {});
    const bool foldShot = renderer.save(view, dir / "41g-fx-row-folded.png");
    const Rect strip = detail::fxCard(1.0f, detail::kFoldedFx);
    click(view, {strip.center().x, strip.y + 150.0f});
    check(folded && foldShot && model.ui.splitsFolded == 0u, "a container folds to a strip, and unfolds");

    // Dragged by its title past the delay: the split goes after it, with its bands, the delay gliding into its place
    // (a step on, the row showing neither where they were nor where they go: read off the rendered pixels along the
    // cards' title row); undone. Carried past the container's end.
    const Point title{column.x + 60.0f, column.y + 17.0f};
    const float pastBox = detail::fxCard(1.0f + 2.0f * detail::kFxBoxEdge + 2.0f + detail::kFxBoxAddSpan + 0.3f).x;
    auto rowPixels = [&] {
        std::vector<uint32_t> line;
        for (float x = detail::kFxPanel.x + 12.0f; x < detail::kFxPanel.right() - 12.0f; x += 2.0f)
            line.push_back(renderer.pixel({x, title.y}));
        return line;
    };
    view.mouseDown(title, {});
    view.mouseMove({title.x + 20.0f, title.y}, {});
    view.mouseMove({pastBox, title.y}, {});
    const bool startShot = renderer.save(view, dir / "41h1-fx-row-glide-start.png");
    const std::vector<uint32_t> atStart = rowPixels();
    view.poll();
    const bool stepShot = renderer.save(view, dir / "41h2-fx-row-glide-step.png");
    const std::vector<uint32_t> aStepOn = rowPixels();
    settle();
    const bool carryShot = renderer.save(view, dir / "41h-fx-row-drag-container.png");
    check(startShot && stepShot && carryShot && aStepOn != atStart && aStepOn != rowPixels(),
          "cards dragged along the row glide aside, a step at a time");
    view.mouseUp({pastBox, title.y}, {});
    settle();
    const bool carried = from(2, 0) == fxSlotPort(0) && from(split, 0) == fxSlotPort(2) &&
                         from(kFxOutNode, 0) == fxSumPort(0) && from(1, 0) == low && from(3, 0) == high;
    view.undo();
    check(carryShot && carried && from(split, 0) == fxSlotPort(0) && from(2, 0) == fxSumPort(0),
          "a container dragged by its title moves along the line with its bands");

    // Its x, kept: the split goes, its bands' effects on the main line in its place, the high band's first; undone.
    click(view, centerOf({column.right() - 26.0f, column.y + 8.0f, 18.0f, 21.0f}));
    click(view, centerOf(detail::confirmButton(detail::confirmPanel(view.height(), true), 1)));
    const bool flattened = model.value(paramIndex(0, SplitParam::Used)) == 0.0 && from(3, 0) == fxSlotPort(0) &&
                           from(1, 0) == fxSlotPort(3) && from(2, 0) == fxSlotPort(1);
    view.undo();
    check(flattened && model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(1, 0) == low,
          "a container's x puts its bands' effects on the main line, the high band's first");

    // The delay carried over the LOW tab lights it (read off the rendered pixels), leaving the high band shown, and
    // let go of there lands at the low band's end, after the widener. Then the low band shown by its tab.
    const Rect delay = detail::fxCard(3.0f + detail::kFxBoxAddSpan + 2.0f * detail::kFxBoxEdge);
    const Point delayGrip{delay.x + 40.0f, delay.y + 17.0f};
    const Rect lowTabRect = detail::splitTab(column, 0, 2);
    const Point lowTab = centerOf(lowTabRect);
    const Point tabCorner{lowTabRect.x + 4.0f, lowTabRect.y + 4.0f}; // clear of its name and dot
    view.mouseMove({600.0f, 600.0f}, {});
    renderer.save(view, dir / "41i0-fx-row-tab-plain.png");
    const uint32_t tabPlain = renderer.pixel(tabCorner);
    view.mouseDown(delayGrip, {});
    view.mouseMove({delayGrip.x - 30.0f, delayGrip.y}, {});
    view.mouseMove(lowTab, {});
    const bool kept = model.ui.splitBand[0] == 1;
    const bool litShot = renderer.save(view, dir / "41i1-fx-row-tab-target.png");
    const bool lit = renderer.pixel(tabCorner) != tabPlain;
    view.mouseUp(lowTab, {});
    settle();
    const bool landed = from(2, 0) == fxSlotPort(1) && from(join, 1) == fxSlotPort(2) &&
                        from(kFxOutNode, 0) == fxSumPort(0) && model.ui.splitBand[0] == 1;
    click(view, lowTab);
    view.mouseMove({600.0f, 600.0f}, {});
    check(kept && litShot && lit && landed && model.ui.splitBand[0] == 0 &&
              renderer.save(view, dir / "41i-fx-row-low-band.png"),
          "a card carried over a container's tab lights it, leaves the band shown, and let go of there lands on its "
          "band");

    // The delay, now the low band's second card, carried freely. Moved within its own place, or straight down over
    // the modulation panel (the card outlined under the mouse there), nothing moves, nothing being after it on its band. Carried left over the widener it
    // goes before it; on over the container's controls it stays there, landing at the low band's start rather than
    // before the split. Carried on to the HIGH tab and let go of there, it lands at the high band's end, after the
    // reverb, the low band still shown. Each undone.
    {
        const Rect delayCard = detail::fxCard(3.0f + detail::kFxBoxEdge);
        const Point grip{delayCard.x + 40.0f, delayCard.y + 17.0f};
        const Point overWidener{detail::fxCard(2.0f + detail::kFxBoxEdge).x + 40.0f, grip.y};
        const Point overControls{column.x + 100.0f, grip.y};
        const Point highTab = centerOf(detail::splitTab(column, 1, 2));
        auto lowBand = [&] { return from(1, 0) == low && from(2, 0) == fxSlotPort(1) && from(join, 1) == fxSlotPort(2); };
        view.mouseDown(grip, {});
        view.mouseMove({grip.x + 60.0f, grip.y}, {});
        view.mouseUp({grip.x + 60.0f, grip.y}, {});
        settle();
        const bool withinOwn = lowBand();
        view.mouseDown(grip, {});
        view.mouseMove({grip.x + 20.0f, grip.y}, {});
        view.mouseMove({grip.x + 20.0f, 640.0f}, {});
        settle();
        const bool freeShot = renderer.save(view, dir / "41j-fx-row-carried-free.png");
        view.mouseUp({grip.x + 20.0f, 640.0f}, {});
        settle();
        const bool straightDown = lowBand();
        view.mouseDown(grip, {});
        view.mouseMove({grip.x - 30.0f, grip.y}, {});
        view.mouseMove(overWidener, {});
        view.mouseMove(overControls, {});
        view.mouseUp(overControls, {});
        settle();
        const bool bandStart = from(2, 0) == low && from(1, 0) == fxSlotPort(2) && from(join, 1) == fxSlotPort(1) &&
                               model.value(paramIndex(0, SplitParam::Used)) == 1.0 && from(split, 0) == fxSlotPort(0);
        view.undo();
        view.mouseDown(grip, {});
        view.mouseMove({grip.x - 30.0f, grip.y}, {});
        view.mouseMove(overWidener, {});
        view.mouseMove(overControls, {});
        view.mouseMove(highTab, {});
        const bool highShown = model.ui.splitBand[0] == 0; // the low band still shown
        view.mouseUp(highTab, {});
        settle();
        const bool onHigh = from(2, 0) == fxSlotPort(3) && from(join, 0) == fxSlotPort(2) && from(join, 1) == fxSlotPort(1);
        view.undo();
        std::printf("      carried freely: within its place %d, straight down %d, over the controls %d, onto the high "
                    "band %d %d\n",
                    withinOwn, straightDown, bandStart, highShown, onHigh);
        check(withinOwn && straightDown && freeShot && bandStart && highShown && onHigh && lowBand() &&
                  model.ui.splitBand[0] == 0,
              "a card at its band's end moves nothing carried right or down, waits over a container's controls, and "
              "carried over a tab lands on that band");
    }

    // The delay's title menu, on the low band of a split in two, offers to move it to the high band only, which puts
    // it at that band's end, after the reverb; undone. The shaper's, on the main line, offers no band.
    {
        const Rect delayCard = detail::fxCard(3.0f + detail::kFxBoxEdge);
        const std::string toHigh = trf(Str::MenuMoveToBand, {tr(Str::BandHigh)});
        const std::string toLow = trf(Str::MenuMoveToBand, {tr(Str::BandLow)});
        const std::string toMid = trf(Str::MenuMoveToBand, {tr(Str::BandMid)});
        int command = -1, moves = 0;
        for (const MenuItem &item : view.contextMenu({delayCard.x + 60.0f, delayCard.y + 17.0f})) {
            moves += item.label == toHigh || item.label == toLow || item.label == toMid;
            if (item.label == toHigh)
                command = item.command;
        }
        int shaperMoves = 0;
        for (const MenuItem &item : view.contextMenu({detail::fxCard(0.0f).x + 60.0f, detail::fxCard(0.0f).y + 17.0f}))
            shaperMoves += item.label == toHigh || item.label == toLow || item.label == toMid;
        view.contextMenu({delayCard.x + 60.0f, delayCard.y + 17.0f});
        view.menuCommand(command);
        const bool moved = from(2, 0) == fxSlotPort(3) && from(join, 0) == fxSlotPort(2) && from(join, 1) == fxSlotPort(1);
        view.undo();
        check(moves == 1 && command >= 0 && shaperMoves == 0 && moved && from(join, 1) == fxSlotPort(2) &&
                  from(join, 0) == fxSlotPort(3),
              "a card's menu on a split's band moves it to the end of another band, undoably");
    }

    // An arrow before the first card too (read off the rendered pixels: its brightest pixel against the panel's above
    // it), whose list puts a phaser at the line's start.
    {
        const Point first{detail::fxCard(0.0f).x - detail::kFxGap * 0.5f, detail::fxCard(0.0f).center().y};
        const bool drawn = brightest(first) > brightest({first.x, first.y - 60.0f}) + 30;
        // The arrow into the low band's second card, the delay, in the band's color too.
        const Point intoDelay{detail::fxCard(3.0f + detail::kFxBoxEdge).x - detail::kFxGap * 0.5f, first.y};
        const bool bandArrow =
            blue(brightestAlong(renderer, {intoDelay.x - 5.0f, intoDelay.y}, {intoDelay.x + 5.0f, intoDelay.y}));
        check(bandArrow, "the arrows along a container's band are all in its color");
        click(view, first);
        const Rect list = detail::fxMenu({0.0f, 0.0f}, true);
        const Point listAt{std::clamp(first.x - 0.5f * list.w, detail::kFxPanel.x + 8.0f,
                                      detail::kFxPanel.right() - 8.0f - list.w),
                           std::min(detail::kFxTop + 8.0f, detail::kFxPanel.bottom() - 4.0f - list.h)};
        const Rect phaser = detail::fxMenuItem(listAt, FxType::Phaser);
        click(view, {phaser.x + 0.5f * phaser.w, phaser.y + 0.5f * phaser.h});
        check(drawn && model.value(paramIndex(4, FxParam::Type)) == static_cast<double>(FxType::Phaser) &&
                  from(4, 0) == kFxPortIn && from(0, 0) == fxSlotPort(4),
              "an arrow goes before the first card, its list putting an effect at the line's start");
    }
}

// While only what keeps to its place moves, the editor repaints that place alone: the output meter and scope (the
// header), and the keys (the keyboard's panel); what a voice's sources move repaints the whole. Painted over the
// frame before, the part comes out as a whole paint of the same moment does, pixel for pixel.
void testPartialRepaint(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    model.ui.view = RoutingView::Matrix; // the routing graph's glow, which follows the output, isn't behind it
    View view(model);
    model.live.voices = 1;
    auto scopeAt = [&](float shift, float level) {
        for (size_t i = 0; i < model.live.outScope.size(); ++i)
            model.live.outScope[i] =
                level * std::sin(6.2831853f * (static_cast<float>(i) / model.live.outScope.size() * 2.0f + shift));
    };
    model.live.outPeak = {0.5f, 0.45f};
    scopeAt(0.0f, 0.5f);
    view.poll();
    const bool first = renderer.save(view, dir / "99c-partial-before.png");
    // Compares a repaint of the areas the last poll found changed, over the last frame, with a whole paint of the same
    // moment.
    auto samePixels = [&](const char *name) {
        bool repainted = true;
        for (const Rect &area : view.changedAreas())
            repainted = renderer.repaint(view, area) && repainted;
        const std::vector<uint32_t> part = renderer.image();
        const bool saved = renderer.save(view, dir / name);
        const std::vector<uint32_t> full = renderer.image();
        size_t differ = part.size() == full.size() ? 0 : full.size();
        for (size_t i = 0; i < part.size() && i < full.size(); ++i)
            differ += part[i] != full[i] ? 1 : 0;
        for (const Rect &area : view.changedAreas())
            std::printf("      repainting %.0fx%.0f at (%.0f, %.0f)\n", area.w, area.h, area.x, area.y);
        std::printf("      %zu pixel(s) unlike a whole paint\n", differ);
        return repainted && saved && differ == 0;
    };
    auto sameArea = [](Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
    auto only = [&](Rect r) { return view.changedAreas().size() == 1 && sameArea(view.changedAreas()[0], r); };
    const Rect header{0.0f, 0.0f, View::kWidth, detail::kHeaderH};

    // The meter falls and the scope takes another span: the header alone.
    model.live.outPeak = {0.3f, 0.2f};
    scopeAt(0.1f, 0.3f);
    const bool meterMoved = view.poll() && only(header);
    check(first && meterMoved && samePixels("99d-partial-header.png"),
          "the output meter and scope moving repaint the header alone, as a whole paint would");

    // A key goes down as the meter moves on: the keyboard's panel, and the header apart.
    model.live.keys[1] |= 1u << 28; // middle C
    model.live.outPeak = {0.25f, 0.2f};
    const bool keyMoved = view.poll() && view.changedAreas().size() == 2 &&
                          std::any_of(view.changedAreas().begin(), view.changedAreas().end(),
                                      [](Rect r) { return r.y > detail::kRouting.bottom() && r.h < 200.0f; });
    check(keyMoved && samePixels("99e-partial-keys.png"),
          "a key going down repaints the keyboard alone (and the header apart), as a whole paint would");

    // The Pitch Bend param moving, as a host automates it, moves the bend wheel: the keyboard's panel again.
    model.set(paramIndex(GlobalParam::PitchBend), -0.6);
    const bool wheelMoved = view.poll() && !only(view.wholeArea()) &&
                            std::any_of(view.changedAreas().begin(), view.changedAreas().end(),
                                        [](Rect r) { return r.y > detail::kRouting.bottom() && r.h < 200.0f; });
    check(wheelMoved && samePixels("99e2-partial-bend.png"),
          "the Pitch Bend param moving repaints the keyboard's panel, its wheel there, as a whole paint would");
    // So does what the host adds to it, its modulation of the param and its MIDI bend (see Telemetry::bend): the wheel
    // shows the param as set with that on top, here +0.9 on its -0.6.
    model.live.bend = 0.3f;
    model.live.bendSet = -0.6f;
    const bool hostMoved = view.poll() && !only(view.wholeArea()) &&
                           std::any_of(view.changedAreas().begin(), view.changedAreas().end(),
                                       [](Rect r) { return r.y > detail::kRouting.bottom() && r.h < 200.0f; });
    const std::vector<uint32_t> before = renderer.image();
    check(hostMoved && samePixels("99e3-partial-bend-host.png") && renderer.image() != before,
          "the host's modulation of the pitch bend moves the bend wheel, repainting the keyboard's panel, as a whole "
          "paint would");
    model.live.bend = model.live.bendSet = 0.0f;
    model.set(paramIndex(GlobalParam::PitchBend), 0.0);

    // Operator A's scope shown: its trace moving repaints the inspector (and the header apart, its meter showing).
    model.ui.selectedNode = 0;
    model.ui.opTab = InspectorTab::Scope;
    model.live.scopeBins = 2 * kScopeBinsPerCycle;
    for (int bin = 0; bin < model.live.scopeBins; ++bin)
        model.live.scope[bin] = 0.4f * std::sin(6.2831853f * bin / kScopeBinsPerCycle);
    view.poll();
    renderer.save(view, dir / "99f-partial-scope-before.png");
    for (int bin = 0; bin < model.live.scopeBins; ++bin)
        model.live.scope[bin] = 0.3f * std::sin(6.2831853f * bin / kScopeBinsPerCycle + 0.5f);
    const bool scopeMoved = view.poll() && std::any_of(view.changedAreas().begin(), view.changedAreas().end(),
                                                       [&](Rect r) { return sameArea(r, detail::kInspector); });
    check(scopeMoved && !only(view.wholeArea()) && samePixels("99g-partial-scope.png"),
          "an operator's scope moving repaints the inspector alone, as a whole paint would");

    // What a voice's sources move repaints where it shows: an LFO modulating nothing, its card alone; its modulation
    // of an operator's param, the panels' row too (the routing panel and the inspector); of a header knob, the header;
    // of another source's param, that source's card; an operator's envelope moving, the inspector alone; and with the
    // MODS view shown, its rows too. Each painted over the frame before as a whole paint of the same moment would be.
    {
        const Rect panels{0.0f, detail::kHeaderH, View::kWidth, detail::kRouting.bottom() - detail::kHeaderH};
        const Rect card0 = detail::sourceCard(0.0f), card1 = detail::sourceCard(1.0f);
        auto has = [&](Rect r) {
            return std::any_of(view.changedAreas().begin(), view.changedAreas().end(),
                               [&](Rect a) { return sameArea(a, r); });
        };
        auto areas = [&] { return view.changedAreas().size(); };
        // Polled until nothing moves (the scopes easing down), then painted whole: the frame the next part goes over.
        auto settle = [&](const char *name) {
            for (int i = 0; i < 40 && view.poll(); ++i)
                ;
            return renderer.save(view, dir / name);
        };
        // What painting `parts` costs, each painted over the last frame (offscreen, so a measure of the editor's own
        // work, not the screen's).
        auto paintMs = [&](std::vector<Rect> parts) {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i)
                for (const Rect &part : parts)
                    renderer.repaint(view, part);
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 10.0;
        };
        model.live.outPeak = {0.0f, 0.0f}; // the header's meter and scope still
        scopeAt(0.0f, 0.0f);
        model.ui.opTab = InspectorTab::Envelope;
        model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        model.live.sources[0] = 0.2f;
        model.live.lfoPhases[0] = 0.1f;
        settle("99g1-live-before.png");
        model.live.sources[0] = 0.6f;
        model.live.lfoPhases[0] = 0.3f;
        const bool alone = view.poll() && only(card0);
        const double cardMs = paintMs(view.changedAreas()), wholeMs = paintMs({view.wholeArea()});
        check(alone && samePixels("99g2-live-lfo-alone.png"),
              "an LFO moving that modulates nothing repaints its card alone, as a whole paint would");

        const int out = paramIndex(0, OpParam::Out);
        model.set(slotTargetIndex(0, 0), slotTargetValue(out));
        model.set(slotAmountIndex(0, 0), -0.5);
        model.live.modOffset[out] = -0.1f;
        model.live.slotOffset[0] = -0.1f;
        settle("99g3-live-op-before.png");
        model.live.sources[0] = 0.2f;
        model.live.modOffset[out] = -0.3f;
        model.live.slotOffset[0] = -0.3f;
        const bool onOp = view.poll() && areas() == 2 && has(card0) && has(panels);
        const double opMs = paintMs(view.changedAreas());
        check(onOp && samePixels("99g4-live-op.png"),
              "an LFO moving operator A's Out repaints its card and the panels' row, as a whole paint would");

        const int pan = paramIndex(GlobalParam::Pan);
        model.set(slotTargetIndex(0, 0), slotTargetValue(pan));
        model.live.modOffset[out] = 0.0f;
        model.live.modOffset[pan] = 0.2f;
        model.live.slotOffset[0] = 0.1f;
        settle("99g5-live-header-before.png");
        model.live.sources[0] = 0.7f;
        model.live.modOffset[pan] = 0.5f;
        model.live.slotOffset[0] = 0.25f;
        const bool onHeader = view.poll() && areas() == 2 && has(card0) && has(header);
        check(onHeader && samePixels("99g6-live-header.png"),
              "an LFO moving the header's Pan repaints its card and the header, as a whole paint would");

        model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        const int rate = paramIndex(1, SourceParam::Rate);
        model.set(slotTargetIndex(0, 0), slotTargetValue(rate));
        model.live.modOffset[pan] = 0.0f;
        model.live.modOffset[rate] = 0.05f;
        model.live.slotOffset[0] = 0.05f;
        settle("99g7-live-source-before.png");
        model.live.sources[0] = 0.3f;
        model.live.modOffset[rate] = 0.15f;
        model.live.slotOffset[0] = 0.15f;
        const bool onSource = view.poll() && areas() == 2 && has(card0) && has(card1);
        check(onSource && samePixels("99g8-live-source.png"),
              "an LFO moving another LFO's Rate repaints the two cards, as a whole paint would");

        model.set(slotTargetIndex(0, 0), 0.0);
        model.set(slotAmountIndex(0, 0), 0.0);
        model.live.modOffset[rate] = 0.0f;
        model.live.slotOffset[0] = 0.0f;
        model.live.opLevels[0] = model.live.opEnvLevels[0] = 0.8f;
        model.live.opEnvPos[0] = 2.3f; // in the decay
        settle("99g9-live-envelope-before.png");
        model.live.opLevels[0] = model.live.opEnvLevels[0] = 0.6f;
        model.live.opEnvPos[0] = 2.6f;
        const bool envelope = view.poll() && only(detail::kInspector);
        check(envelope && samePixels("99g10-live-envelope.png"),
              "operator A's envelope moving repaints the inspector alone, as a whole paint would");

        model.ui.view = RoutingView::Mods;
        model.set(slotTargetIndex(0, 0), slotTargetValue(pan));
        model.set(slotAmountIndex(0, 0), -0.5);
        model.live.modOffset[pan] = 0.2f;
        model.live.slotOffset[0] = 0.1f;
        settle("99g11-live-mods-before.png");
        model.live.sources[0] = 0.5f;
        model.live.modOffset[pan] = 0.4f;
        model.live.slotOffset[0] = 0.2f;
        const bool mods = view.poll() && areas() == 3 && has(card0) && has(header) && has(panels);
        check(mods && samePixels("99g12-live-mods.png"),
              "with the MODS view shown, an LFO's modulation moving repaints its rows too, as a whole paint would");
        std::printf("      painting offscreen: the whole editor %.1f ms, an LFO's card %.1f ms, with the panels' row "
                    "%.1f ms\n",
                    wholeMs, cardMs, opMs);

        model.ui.view = RoutingView::Matrix;
        model.set(slotTargetIndex(0, 0), 0.0);
        model.set(slotAmountIndex(0, 0), 0.0);
        model.set(paramIndex(0, SourceParam::Type), 0.0);
        model.set(paramIndex(1, SourceParam::Type), 0.0);
        model.live.modOffset[pan] = 0.0f;
        model.live.slotOffset[0] = 0.0f;
        model.live.sources[0] = 0.0f;
        model.live.lfoPhases[0] = 0.0f;
        model.live.opLevels[0] = model.live.opEnvLevels[0] = 0.0f;
        model.live.opEnvPos[0] = 0.0f;
    }

    // The notes end while a tail drives a compressor on: its meter moving repaints the panels' row, where the FX view
    // shows it, as a whole paint would; the host's tempo, which shows in captions anywhere, the whole editor.
    model.live.voices = 0;
    model.live.outPeak = {0.0f, 0.0f};
    model.live.scopeBins = 0;
    for (int i = 0; i < 20 && view.poll(); ++i) // the scopes' levels easing down
        ;
    const bool settled = !view.poll();
    renderer.save(view, dir / "99g13-compressor-before.png");
    model.live.fxReduction[0][0] = 3.0f;
    const bool reductionMoved =
        view.poll() && only({0.0f, detail::kHeaderH, View::kWidth, detail::kRouting.bottom() - detail::kHeaderH}) &&
        samePixels("99g14-compressor.png");
    model.live.tempo = 96.0f;
    const bool tempoMoved = view.poll() && only(view.wholeArea());
    const bool still = !view.poll();
    check(settled && reductionMoved && tempoMoved && still,
          "with no notes playing, a compressor's meter moving repaints the panels' row, as a whole paint would, and "
          "the host's tempo the editor");

    // With no notes playing, a free LFO's shared clock runs on: its dot moving repaints its card alone, as a whole
    // paint would. Switched to retrigger it shows no dot (the card's pixels change) and moving repaints nothing.
    model.live.fxReduction[0][0] = 0.0f;
    const bool idleBefore = model.idleLfosShown;
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(0, SourceParam::Free), 1.0);
    model.live.lfoPhases[0] = 0.2f;
    model.live.lfoRates[0] = 2.0f;
    model.live.age = 0.01f;
    view.poll();
    // The view tells the plugin, which keeps processing for it while a host would let it sleep.
    const bool idleTold = !idleBefore && model.idleLfosShown;
    renderer.save(view, dir / "99h-idle-lfo-before.png");
    model.live.lfoPhases[0] = 0.6f;
    const bool idleMoved = view.poll() && only(detail::sourceCard(0.0f));
    check(idleMoved && samePixels("99i-idle-lfo.png"),
          "a free LFO's dot moving with no notes playing repaints its card alone, as a whole paint would");
    const std::vector<uint32_t> withDot = renderer.image();
    model.set(paramIndex(0, SourceParam::Free), 0.0);
    view.poll();
    const bool noDot = renderer.save(view, dir / "99j-idle-lfo-retrigger.png") && renderer.image() != withDot;
    model.live.lfoPhases[0] = 0.9f;
    check(noDot && !view.poll(), "a retrigger LFO shows no dot with no notes playing, and repaints nothing");
    // While it modulates the effects as they ring out after the last note, the tail runs it: its dot shows, moving
    // with the tail's clock, and goes with the tail.
    const std::vector<uint32_t> idleCard = renderer.image();
    model.live.fxTail = true;
    model.live.fxTailLfos = 1u;
    model.live.lfoPhases[0] = 0.2f;
    view.poll();
    renderer.save(view, dir / "99k-tail-lfo.png");
    const std::vector<uint32_t> tailDot = renderer.image();
    model.live.lfoPhases[0] = 0.6f;
    const bool tailMoved = view.poll() && only(detail::sourceCard(0.0f));
    model.live.fxTail = false;
    model.live.fxTailLfos = 0u;
    view.poll();
    renderer.save(view, dir / "99l-tail-lfo-done.png");
    check(tailDot != idleCard && tailMoved && renderer.image() == idleCard,
          "a retrigger LFO the effects' tail runs shows its dot moving, and none once the tail ends");
    check(idleTold && !model.idleLfosShown,
          "the editor tells the plugin while a free LFO runs with no notes playing, and once none does");
}

// A macro's name that isn't valid UTF-8 (a state can hold any bytes there) drawn as best it can be, and the rest of
// the editor drawn as with a valid name: only the name's own pixels differ (read off the rendered pixels).
void testInvalidText(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    auto grid = [&] {
        std::vector<uint32_t> pixels;
        for (float y = 4.0f; y < view.height(); y += 9.0f)
            for (float x = 4.0f; x < View::kWidth; x += 9.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    model.ui.macroNames[0] = "Bad?";
    view.poll();
    const bool plain = renderer.save(view, dir / "99a-macro-name.png");
    const std::vector<uint32_t> before = grid();
    model.ui.macroNames[0] = "Bad\xFF";
    view.poll();
    const bool invalid = renderer.save(view, dir / "99b-macro-name-invalid.png");
    const std::vector<uint32_t> after = grid();
    size_t differ = 0;
    for (size_t i = 0; i < before.size(); ++i)
        differ += before[i] != after[i] ? 1 : 0;
    std::printf("      a macro name that isn't UTF-8: %zu of %zu sampled pixels differ from a valid one's\n", differ,
                before.size());
    check(plain && invalid && differ < before.size() / 200,
          "a name that isn't valid UTF-8 leaves the rest of the editor drawn");
}

// The list of effects opened on a band's + while the split goes: an undo closes the list, and a split taken away
// behind the editor's back (as a state load does) with the list still open puts the effect picked at the line's end
// rather than on a band no longer there.
void testFxListAfterUndo(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    click(view, detail::fxModeButton(1).center());
    auto cell = [](int col, int row) { return detail::fxGridCell(col, row, 0.0f); };
    auto plusItem = [&](Point plus, FxType type) { return fxListItem(view, besidePlus(plus), type, true); };
    auto plusNode = [&](Point plus) { return fxListSplit(view, besidePlus(plus)); };
    auto from = [&](int node, int input) {
        return fxPortOf(model.value(fxInputIndex(node, input, FxInputField::From)));
    };
    auto delayed = [&] {
        for (int slot = 0; slot < kFxSlots; ++slot)
            if (std::lround(model.value(paramIndex(slot, FxParam::Type))) == static_cast<int>(FxType::Delay))
                return slot;
        return -1;
    };
    click(view, cell(3, 0));
    click(view, plusNode(cell(3, 0))); // a split after the reverb
    const bool split = model.value(paramIndex(0, SplitParam::Used)) == 1.0;

    // Its high band's + opens the list; an undo takes the split away and closes the list, so the click on where its
    // Delay was adds nothing.
    click(view, cell(4, -1));
    view.undo();
    view.poll();
    click(view, plusItem(cell(4, -1), FxType::Delay));
    check(split && model.value(paramIndex(0, SplitParam::Used)) == 0.0 && delayed() < 0 &&
              renderer.save(view, dir / "40z-fx-list-undo.png"),
          "an undo closes the list of effects opened on a band's +");

    // Again, the split taken away without the editor's undo: the Delay picked goes at the line's end.
    view.redo();
    view.poll();
    click(view, cell(4, -1));
    model.undo();
    view.poll();
    click(view, plusItem(cell(4, -1), FxType::Delay));
    const int slot = delayed();
    check(model.value(paramIndex(0, SplitParam::Used)) == 0.0 && slot >= 0 &&
              fxChain(model) == std::vector<int>{0, 1, slot} && from(kFxOutNode, 0) == fxSlotPort(slot),
          "an effect picked from a band's list after its split went goes at the line's end");
}

// The graph's box takes the room the inspector's card leaves: beside a card one place wide (a delay's) it reaches
// further right than beside a wide one (a parametric EQ's), whose card takes some of it (read off the rendered
// pixels: just inside the wider box's right edge, against the box's empty top left).
void testFxGraphRoom(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    const Rect mode = detail::fxModeButton(1);
    click(view, {mode.x + mode.w * 0.5f, mode.y + mode.h * 0.5f});
    const Rect beside = detail::fxGraphArea(1.0f);
    const Point probe{beside.right() - 6.0f, beside.y + 20.0f}, empty{beside.x + 20.0f, beside.y + 20.0f};
    auto boxThere = [&](const char *shot) {
        view.mouseMove({600.0f, 700.0f}, {});
        renderer.save(view, dir / shot);
        return renderer.pixel(probe) == renderer.pixel(empty);
    };
    click(view, detail::fxGridCell(1, 0, 0.0f)); // the delay
    const bool narrow = model.ui.fxSelected == 0 && boxThere("40w-fx-graph-narrow-card.png");
    click(view, detail::fxGridCell(2, 0, 0.0f)); // the parametric EQ
    const bool wide = model.ui.fxSelected == 1 && !boxThere("40w2-fx-graph-wide-card.png");
    check(detail::fxGraphArea(1.5f).right() < beside.right() - 40.0f && narrow && wide,
          "the graph's box takes the room the inspector's card leaves, more beside a narrow card");
}

// The effects graph wider than its box: a scrollbar under the box, whose thumb dragged to its end scrolls the graph
// there, the main line's + at the end then taking a click; with the graph fitting, no scrollbar.
void testFxGraphScroll(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    for (int slot = 0; slot < 10; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Delay);
    model.values[paramIndex(9, FxParam::Type)] = static_cast<double>(FxType::ParamEq); // a wide card, last
    setChain(model, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    view.poll();
    click(view, kFxTab);
    const Rect mode = detail::fxModeButton(1);
    click(view, {mode.x + mode.w * 0.5f, mode.y + mode.h * 0.5f});
    // IN, ten effects, the + and OUT: as wide as 12 columns and OUT's half and a margin, scrolling as far as the box
    // beside the widest card needs (so picking a node never moves the graph).
    const Rect area = detail::fxGraphArea(1.5f);
    const float most = detail::kFxGridLeft + 12.0f * detail::kFxGridCol + detail::kFxEndHalfW + 16.0f - area.w;
    const Rect track = detail::fxGraphScrollTrack();
    // The thumb, at the track's left end, lighter under the mouse than the track (read off the rendered pixels).
    const float mid = track.y + track.h * 0.5f;
    auto light = [&](Point p) {
        const uint32_t px = renderer.pixel(p);
        return static_cast<int>((px >> 16 & 255) + (px >> 8 & 255) + (px & 255));
    };
    view.mouseMove({track.x + 8.0f, mid}, {});
    const bool barShown = renderer.save(view, dir / "40k-fx-graph-scroll.png") &&
                          light({track.x + 8.0f, mid}) > light({track.right() - 8.0f, mid}) + 30;
    drag(view, {track.x + 6.0f, mid}, {track.right() + 60.0f, mid});
    const bool scrolledShot = renderer.save(view, dir / "40l-fx-graph-scrolled.png");
    // Scrolled to its end, the Param EQ's node picked (its card wide, the box narrower) and then a delay's (the box
    // wider again) leave the graph where it is: the + after the last effect still takes the click there.
    click(view, detail::fxGridCell(10, 0, most));
    click(view, detail::fxGridCell(9, 0, most));
    const bool picked = model.ui.fxSelected == 8;
    const Point plus = detail::fxGridCell(11, 0, most);
    click(view, plus);
    click(view, fxListItem(view, besidePlus(plus), FxType::Reverb, true));
    const bool added = fxPortOf(model.value(fxInputIndex(kFxOutNode, 0, FxInputField::From))) == fxSlotPort(10) &&
                       model.value(paramIndex(10, FxParam::Type)) == static_cast<double>(FxType::Reverb);
    // Down to three effects, the graph fits: no scrollbar, the track's place plain panel again.
    for (int slot = 3; slot <= 10; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::None);
    view.poll();
    view.mouseMove({track.x + 8.0f, mid}, {});
    const bool gone = renderer.save(view, dir / "40m-fx-graph-fits.png") &&
                      renderer.pixel({track.x + 8.0f, mid}) == renderer.pixel({track.x - 6.0f, mid});
    check(most > 0.0f && barShown && scrolledShot && picked && added && gone,
          "the effects graph wider than its box has a scrollbar under it that scrolls it, picking a node moving it "
          "nowhere, and none once it fits");
}

// The bin at the FX panel's top right, in the chain and in the graph: with effects (or a split) it asks first, and a
// click around the dialog keeps them; going ahead removes every one, a split and its join too, in one undo step;
// with nothing to remove it asks nothing (read off the rendered pixels: the header isn't dimmed).
void testFxClear(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
    model.values[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
    setChain(model, {0, 1});
    view.poll();
    click(view, kFxTab);
    const Rect binRect = detail::fxClearButton();
    const Point bin{binRect.x + binRect.w * 0.5f, binRect.y + binRect.h * 0.5f};
    const Rect confirm{420.0f, std::round((view.height() - 150.0f) * 0.5f), 360.0f, 150.0f};
    const Point ok{confirm.right() - 64.0f, confirm.bottom() - 38.0f};
    const Point header{600.0f, 20.0f}; // the editor's title bar, which the dialog's backdrop dims
    renderer.save(view, dir / "40s-fx-clear-before.png");
    const uint32_t clear = renderer.pixel(header);

    // The chain: asked, kept by a click around the dialog, then removed, and back by undo.
    click(view, bin);
    const bool asked = renderer.save(view, dir / "40s-fx-clear-confirm.png") && renderer.pixel(header) != clear;
    click(view, {40.0f, 40.0f});
    const bool kept = fxChain(model) == std::vector<int>{0, 1};
    click(view, bin);
    click(view, ok);
    const bool emptied = fxChain(model).empty();
    view.undo();
    const bool back = fxChain(model) == std::vector<int>{0, 1};

    // The graph with a split after the effects: all of it goes, IN wired straight out.
    const Rect mode = detail::fxModeButton(1);
    click(view, {mode.x + mode.w * 0.5f, mode.y + mode.h * 0.5f});
    const Point plus = detail::fxGridCell(3, 0, 0.0f);
    click(view, plus);
    click(view, fxListSplit(view, besidePlus(plus)));
    const bool split = model.value(paramIndex(0, SplitParam::Used)) == 1.0 &&
                       model.value(paramIndex(0, SumParam::Used)) == 1.0;
    click(view, bin);
    click(view, ok);
    const bool graphEmptied =
        fxChain(model).empty() && model.value(paramIndex(0, SplitParam::Used)) == 0.0 &&
        model.value(paramIndex(0, SumParam::Used)) == 0.0 &&
        fxPortOf(model.value(fxInputIndex(kFxOutNode, 0, FxInputField::From))) == kFxPortIn &&
        fxPortOf(model.value(fxInputIndex(kFxOutNode, 1, FxInputField::From))) < 0 && renderer.save(view, dir / "40t-fx-cleared.png");

    // Nothing left: the bin asks nothing.
    view.mouseMove({600.0f, 428.0f}, {});
    renderer.save(view, dir / "40t-fx-cleared.png");
    const uint32_t plain = renderer.pixel(header);
    click(view, bin);
    view.mouseMove({600.0f, 428.0f}, {});
    renderer.save(view, dir / "40u-fx-clear-nothing.png");
    check(asked && kept && emptied && back && split && graphEmptied && renderer.pixel(header) == plain,
          "the bin removes every effect and split after asking, and asks nothing with none there");
}

void testFolding(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    setChain(model, {0, 1, 2});
    const FxType types[] = {FxType::Delay, FxType::Reverb, FxType::Phaser};
    for (int slot = 0; slot < 3; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(types[slot]);
    model.values[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    model.values[paramIndex(1, SourceParam::Type)] = static_cast<double>(SourceType::Env);
    view.poll();
    click(view, kFxTab);
    const float cardTop = 179.5f; // an effect card's
    auto fxFold = [&](int position) {
        return Point{fxCardX(model, position) + fxCardW(model, position) - 53.0f, cardTop + 18.5f};
    };
    // The power switch in its title row, as on a source card, switches the effect off and on; so does the icon.
    const Rect reverbCard{fxCardX(model, 1), cardTop, fxCardW(model, 1), 300.0f};
    click(view, detail::cardPower(reverbCard).center());
    const bool powerOff = model.value(paramIndex(1, FxParam::Enabled)) < 0.5;
    renderer.save(view, dir / "28f-fx-power-off.png");
    click(view, detail::cardPower(reverbCard).center());
    check(powerOff && model.value(paramIndex(1, FxParam::Enabled)) >= 0.5,
          "an effect card's power switch, in its title row as a source card's, switches it off and on");
    const Point icon = fxPower(model, 1);
    click(view, fxFold(1));
    const float strip = fxCardX(model, 1) + fxCardW(model, 1) * 0.5f;
    const bool fxFolded = model.ui.fxFolded == 1u << 1 && std::abs(strip - icon.x) < 0.01f; // the icon in the middle
    view.mouseMove(icon, {});
    const bool shot = renderer.save(view, dir / "28a-folded-icon.png");
    click(view, icon); // the folded card's icon, where it was before, switches it
    const bool switched = model.value(paramIndex(1, FxParam::Enabled)) < 0.5;
    click(view, {fxCardX(model, 2) + 40.0f, 428.0f}); // the next card, closed up: its own field, no change
    const bool closedUp = model.ui.fxFolded == 1u << 1 &&
                          std::abs(fxCardX(model, 2) - (46.0f + (1.0f + kFoldedSpan) * kFxStep)) < 0.1f;
    click(view, {strip, 328.0f}); // the strip's name unfolds it
    const bool fxUnfolded = model.ui.fxFolded == 0;
    click(view, fxFold(1));
    click(view, fxRemove(model, 0)); // removing another effect leaves the fold alone
    const bool kept = model.ui.fxFolded == 1u << 1 && fxChain(model).size() == 2;
    const std::vector<int> before = fxChain(model);
    const float folded = fxCardX(model, 0) + fxCardW(model, 0) * 0.5f; // the folded card, now first
    view.mouseMove({folded, cardTop + 38.0f}, {});
    const bool gripShot = renderer.save(view, dir / "28b-folded-grip.png");
    drag(view, {folded, cardTop + 38.0f}, {folded + 400.0f, cardTop + 38.0f}); // its grip, past the next card
    const bool fxMoved = fxChain(model) == std::vector<int>{before[1], before[0]} && model.ui.fxFolded == 1u << 1;

    const float sourceTop = 564.0f; // a source card's
    auto handlePixels = [&] { // the LFO's handle, which stays put as its card folds (read off the rendered pixels)
        std::vector<uint32_t> pixels;
        for (float dy = -12.0f; dy <= 12.0f; dy += 1.0f)
            for (float dx = -12.0f; dx <= 12.0f; dx += 1.0f)
                pixels.push_back(renderer.pixel({sourceHandle(0).x + dx, sourceHandle(0).y + dy}));
        return pixels;
    };
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "28c-unfolded-handle.png");
    const std::vector<uint32_t> handleOpen = handlePixels();
    click(view, {cardX(0) + kCardW - 53.0f, sourceTop + 17.5f}); // the LFO's chevron, left of its switch
    const bool sourceFolded = model.ui.sourcesFolded == 1u << 0;
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "28d-folded-handle.png");
    const bool handleStays = handlePixels() == handleOpen;
    const float envX = cardX(0) + 60.0f; // the envelope's card, closed up after the 48 px strip
    click(view, {envX + kCardW - 34.0f, sourceTop + 17.5f}); // its switch
    const bool envClosedUp = model.value(sourceEnabledIndex(1)) < 0.5;
    const float lfoStrip = cardX(0) + 24.0f;
    drag(view, {lfoStrip, sourceTop + 38.0f}, {lfoStrip + 300.0f, sourceTop + 38.0f}); // its grip, past the envelope
    const bool moved = shown(model) == std::vector<int>{1, 0} && model.ui.sourcesFolded == 1u << 0;
    view.mouseMove({600.0f, 328.0f}, {});
    check(fxFolded && shot && switched && closedUp && fxUnfolded && kept && gripShot && fxMoved && sourceFolded &&
              envClosedUp && moved && handleStays && renderer.save(view, dir / "28-folded.png"),
          "effect and LFO cards fold to strips that still switch, move by their grips and unfold, the effect's icon "
          "and the source's handle staying put");

    // A folded Random card's name: RND down the randomStrip and its number across, four lines and nothing after them
    // (read off the rendered pixels), where "RANDOM 1" took six.
    model.values[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Random);
    view.poll();
    renderer.save(view, dir / "28e-folded-random.png");
    const float randomStrip = cardX(1); // the folded card, after the envelope's
    auto inked = [&](int line) {
        const uint32_t card = renderer.pixel({randomStrip + 4.0f, sourceTop + 150.0f});
        for (float y = sourceTop + 104.0f + line * 13.0f + 1.0f; y < sourceTop + 104.0f + (line + 1) * 13.0f; y += 1.0f)
            for (float x = randomStrip + 8.0f; x < randomStrip + 40.0f; x += 1.0f)
                if (renderer.pixel({x, y}) != card)
                    return true;
        return false;
    };
    check(inked(0) && inked(1) && inked(2) && !inked(3) && inked(4) && !inked(5),
          "a folded Random card shows RND and its number, which fit the strip");
}

// A macro's knob as a target: an LFO's handle dropped on it modulating it, its depth handle under it; another
// macro's handle modulating it too, and its handle refused on the knob of the macro that moves it (a loop) and on
// its own; with no note playing, the knob's dot showing where the other macro takes it (read off the rendered pixels);
// and the target menu listing the macros' knobs by their names, the one that would loop greyed out.
void testMacroTargets(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const float y = view.height() - 179.0f;
    auto macroKnob = [&](int macro) { return Point{188.0f + 96.0f * macro, y}; };
    auto macroHandle = [&](int macro) { return Point{macroKnob(macro).x - 32.0f, y}; };
    const int knob2 = paramIndex(1, MacroParam::Value);
    addSource(view, SourceType::Lfo);
    drag(view, sourceHandle(0), macroKnob(1));
    const bool lfoOn = slotTarget(model.value(slotTargetIndex(0, 0))) == knob2;
    drag(view, macroHandle(0), macroKnob(1)); // Macro 1 onto Macro 2's knob
    const bool linked = slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == knob2;
    view.mouseMove({600.0f, 328.0f}, {});
    const bool shot = renderer.save(view, dir / "26m-macro-modulated.png");
    // Macro 2 onto Macro 1's knob would loop, and onto its own knob is its own: neither takes.
    drag(view, macroHandle(1), macroKnob(0));
    drag(view, macroHandle(1), macroKnob(1));
    view.mouseMove({600.0f, 328.0f}, {});
    const bool refused = slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0;
    check(lfoOn && linked && refused && shot,
          "a macro's knob takes an LFO's and another macro's modulation, but not one that would loop or its own");
    // With no note playing, Macro 2's knob shows where Macro 1 takes it: its dot on the arc moves as Macro 1 turns
    // (read off the rendered pixels around Macro 2's knob, clear of its name).
    auto knobPixels = [&] {
        std::vector<uint32_t> pixels;
        for (float dy = -16.0f; dy <= 6.0f; dy += 1.0f)
            for (float dx = -16.0f; dx <= 16.0f; dx += 1.0f)
                pixels.push_back(renderer.pixel({macroKnob(1).x + dx, y + dy}));
        return pixels;
    };
    model.set(paramIndex(0, MacroParam::Value), 0.0);
    view.poll();
    renderer.save(view, dir / "26n-macro-moved-none.png");
    const std::vector<uint32_t> still = knobPixels();
    model.set(paramIndex(0, MacroParam::Value), 1.0);
    view.poll();
    renderer.save(view, dir / "26o-macro-moved.png");
    const std::vector<uint32_t> moved = knobPixels();
    int differ = 0;
    for (size_t i = 0; i < still.size(); ++i)
        differ += still[i] != moved[i];
    std::printf("      Macro 2's knob: %d pixels change as Macro 1 turns up\n", differ);
    check(differ > 8, "with no note playing, a macro's knob shows where another macro takes it");
    // The target menu, from a new empty modulation of Macro 2: Macro 1's knob, which would loop, can't be picked,
    // and Macro 3's can, by its name.
    model.ui.macroNames[2] = "Space";
    model.ui.view = RoutingView::Mods;
    model.ui.emptyMods = {{kMaxSources + 1, -1}};
    view.poll();
    click(view, {300.0f, 242.0f}); // the empty row's target, under the two rows
    view.mouseMove({300.0f, 435.0f}, {}); // Macros, the last category
    const bool menuShot = renderer.save(view, dir / "26p-macro-target-menu.png");
    click(view, {520.0f, 435.0f}); // Macro 1, beside it: would loop
    const bool loopRefused = model.ui.emptyMods.size() == 1 && slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0;
    click(view, {520.0f, 479.0f}); // Space, Macro 3, the menu still open
    const bool picked = model.ui.emptyMods.empty() &&
                        slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) == paramIndex(2, MacroParam::Value);
    check(menuShot && loopRefused && picked,
          "the target menu lists the macros' knobs by name, refusing the one that would loop and the macro's own");
}

// The macros in the keyboard panel, above the keys: a knob each (centers 96 px apart, 179 px above the
// editor's bottom), a handle left of it to map it like a source, and its name under it, which a double-click
// changes.
void testMacros(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const float y = view.height() - 179.0f;
    auto macroKnob = [&](int macro) { return Point{188.0f + 96.0f * macro, y}; };
    // Until a macro modulates something, its handle, arc and name are faint, and so is the note source's handle
    // (read off the rendered pixels).
    const Point macroRing{macroKnob(0).x - 26.0f, y - 10.0f}, noteRing{982.0f, y - 10.0f};
    view.mouseMove({600.0f, 328.0f}, {});
    const bool idleShot = renderer.save(view, dir / "26a-macros-idle.png");
    const int macroIdle = ringLevel(renderer, macroRing, 20.0f), noteIdle = ringLevel(renderer, noteRing, 20.0f);
    drag(view, macroKnob(0), {macroKnob(0).x, y - 40.0f});
    const bool turned = model.value(paramIndex(0, MacroParam::Value)) > 0.1;
    drag(view, {macroKnob(0).x - 32.0f, y}, kFmAmountKnob); // its handle onto FM Amount
    const bool mapped =
        slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == paramIndex(GlobalParam::FmAmount);
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "26a2-macro-mapped.png");
    const int macroLive = ringLevel(renderer, macroRing, 20.0f);
    // With no note playing, FM Amount's knob shows where the macro takes it, a dot on its arc, which goes with the
    // macro turned back to 0 while the rest of the knob stays as it was (read off the rendered pixels).
    auto fmKnobPixels = [&] {
        std::vector<uint32_t> pixels;
        for (float dy = -30.0f; dy <= 30.0f; dy += 1.0f)
            for (float dx = -40.0f; dx <= 40.0f; dx += 1.0f)
                pixels.push_back(renderer.pixel({kFmAmountKnob.x + dx, kFmAmountKnob.y + dy}));
        return pixels;
    };
    const std::vector<uint32_t> turnedUp = fmKnobPixels();
    const double macroValue = model.value(paramIndex(0, MacroParam::Value));
    model.set(paramIndex(0, MacroParam::Value), 0.0);
    view.poll();
    renderer.save(view, dir / "26a3-macro-at-zero.png");
    const std::vector<uint32_t> turnedDown = fmKnobPixels();
    model.set(paramIndex(0, MacroParam::Value), macroValue);
    view.poll();
    int dotPixels = 0;
    for (size_t i = 0; i < turnedUp.size(); ++i)
        dotPixels += turnedUp[i] != turnedDown[i];
    std::printf("      FM Amount's knob with no note: %d pixels change with the macro turned to 0\n", dotPixels);
    check(model.live.voices == 0 && dotPixels > 8 && dotPixels < 120,
          "with no note playing, a knob shows where a macro takes it");
    // Once the last voice has ended, the effects' and Volume's knobs show where its modulation takes them as the
    // effects ring out: Volume's dot comes with the tail, and goes with it (read off the rendered pixels).
    {
        const Point volumeKnob{detail::headerKnobX(0) + 18.0f, kFmAmountKnob.y};
        auto volumePixels = [&] {
            view.poll();
            renderer.save(view, dir / "26a4-volume-tail.png");
            std::vector<uint32_t> pixels;
            for (float dy = -30.0f; dy <= 30.0f; dy += 1.0f)
                for (float dx = -40.0f; dx <= 40.0f; dx += 1.0f)
                    pixels.push_back(renderer.pixel({volumeKnob.x + dx, volumeKnob.y + dy}));
            return pixels;
        };
        const int volume = paramIndex(GlobalParam::Volume);
        const std::vector<uint32_t> before = volumePixels();
        model.live.modOffset[volume] = -12.0f;
        model.live.fxTail = true;
        const std::vector<uint32_t> tail = volumePixels();
        model.live.fxTail = false;
        const std::vector<uint32_t> after = volumePixels();
        model.live.modOffset[volume] = 0.0f;
        int tailPixels = 0;
        for (size_t i = 0; i < before.size(); ++i)
            tailPixels += before[i] != tail[i];
        std::printf("      Volume's knob in the effects' tail: %d pixels change with its modulation\n", tailPixels);
        check(tailPixels > 8 && tailPixels < 120 && after == before,
              "with no note playing, a knob shows the last voice's modulation while the effects ring out");
    }
    doubleClick(view, {macroKnob(0).x, y + 26.0f});
    type(view, "Bright");
    view.typeKey(View::Key::Enter);
    const bool renamed = model.ui.macroNames[0] == "Bright";
    view.undo();
    const bool undone = model.ui.macroNames[0].empty();
    view.redo();
    view.mouseMove({macroKnob(0).x - 32.0f, y}, {});
    check(turned && mapped && renamed && undone && renderer.save(view, dir / "26-macros.png"),
          "a macro's knob turns, its handle maps it, and a double-click renames it, undoably");
    // A double-click on a macro's knob sets it back to 0, or a bipolar one's to its middle.
    const int macro2 = paramIndex(1, MacroParam::Value);
    model.set(macro2, 0.8);
    doubleClick(view, macroKnob(1));
    const bool unipolarReset = model.value(macro2) == 0.0;
    model.set(paramIndex(1, MacroParam::Bipolar), 1.0);
    model.set(macro2, 0.8);
    doubleClick(view, macroKnob(1));
    const bool bipolarReset = model.value(macro2) == 0.5;
    model.set(paramIndex(1, MacroParam::Bipolar), 0.0);
    model.set(macro2, 0.0);
    view.poll();
    check(unipolarReset && bipolarReset, "a double-click sets a macro's knob to 0, or a bipolar one's to its middle");
    // The note source, right of the macros: its handle, left of its display, maps it the same way, onto filter 1's
    // cutoff here, and while a note plays its display names it (an A4 bent 30 cents up).
    const Point noteHandle{976.0f, y};
    click(view, kF1Tab);
    drag(view, noteHandle, ins(918.0f, 242.0f));
    const bool noteMapped =
        slotTarget(model.value(slotTargetIndex(kNoteSource, 0))) == paramIndex(0, FilterParam::Cutoff);
    model.live.voices = 1;
    model.live.notePitch = 69.3f;
    view.poll();
    view.mouseMove(noteHandle, {});
    const bool shot = renderer.save(view, dir / "26b-note-source.png");
    const int noteLive = ringLevel(renderer, noteRing, 20.0f);
    std::printf("      handles' brightness: macro %d idle, %d mapped; note %d idle, %d mapped\n", macroIdle, macroLive,
                noteIdle, noteLive);
    check(idleShot && macroIdle * 18 < macroLive * 10 && noteIdle * 18 < noteLive * 10,
          "a macro's and the note source's handles are faint until they modulate something");
    std::string removal;
    for (const MenuItem &item : view.contextMenu(ins(918.0f, 242.0f)))
        if (item.label.find(tr(Str::SourceNameNote)) != std::string::npos)
            removal = item.label;
    check(noteMapped && shot && removal == trf(Str::MenuRemoveModulation, {tr(Str::SourceNameNote)}),
          "the note source's handle maps it like a macro's, and its display names the note");

    // The mod wheel's handle, over the wheel below the strip's title, maps it the same way (faint until it does), onto
    // FM Amount here, where it starts upward, as every new modulation does.
    {
        const Point wheelHandle{87.0f, y + 21.0f}, wheelRing{93.0f, y + 11.0f};
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "26f-wheel-idle.png");
        const int idle = ringLevel(renderer, wheelRing, 20.0f);
        drag(view, wheelHandle, kFmAmountKnob);
        const bool wheelMapped =
            slotTarget(model.value(slotTargetIndex(kWheelSource, 0))) == paramIndex(GlobalParam::FmAmount) &&
            model.value(slotAmountIndex(kWheelSource, 0)) == 0.3;
        view.mouseMove({600.0f, 328.0f}, {});
        const bool wheelShot = renderer.save(view, dir / "26g-wheel-mapped.png");
        const int live = ringLevel(renderer, wheelRing, 20.0f);
        std::string wheelRemoval;
        for (const MenuItem &item : view.contextMenu(kFmAmountKnob))
            if (item.label.find(tr(Str::SourceNameWheel)) != std::string::npos)
                wheelRemoval = item.label;
        check(wheelMapped && wheelShot && idle * 18 < live * 10 &&
                  wheelRemoval == trf(Str::MenuRemoveModulation, {tr(Str::SourceNameWheel)}),
              "the mod wheel's handle maps it like a macro's, upward, and is faint until it does");
    }
    // The pitch bend's handle, over the bend wheel, maps it the same way (faint until it does), onto FM Amount too
    // here; it starts bipolar, so at rest it moves nothing.
    {
        const Point bendHandle{47.0f, y + 21.0f}, bendRing{53.0f, y + 11.0f};
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "26f2-bend-idle.png");
        const int idle = ringLevel(renderer, bendRing, 20.0f);
        view.mouseMove(bendHandle, {});
        const bool tip = renderer.save(view, dir / "26f3-bend-tooltip.png");
        drag(view, bendHandle, kFmAmountKnob);
        const bool bendMapped =
            slotTarget(model.value(slotTargetIndex(kBendSource, 0))) == paramIndex(GlobalParam::FmAmount) &&
            model.value(slotAmountIndex(kBendSource, 0)) > 0.0 && model.value(paramIndex(BendParam::Bipolar)) == 1.0;
        view.mouseMove({600.0f, 328.0f}, {});
        const bool bendShot = renderer.save(view, dir / "26g2-bend-mapped.png");
        const int live = ringLevel(renderer, bendRing, 20.0f);
        std::string bendRemoval;
        for (const MenuItem &item : view.contextMenu(kFmAmountKnob))
            if (item.label.find(tr(Str::SourceNameBend)) != std::string::npos)
                bendRemoval = item.label;
        std::printf("      bend handle's brightness: %d idle, %d mapped\n", idle, live);
        check(tip && bendMapped && bendShot && idle * 18 < live * 10 &&
                  bendRemoval == trf(Str::MenuRemoveModulation, {tr(Str::SourceNameBend)}),
              "the pitch bend's handle, over its wheel, maps it like a macro's, bipolar, and is faint until it does");
    }
    // The bend wheel is the Pitch Bend param: dragged, it edits it in one gesture, which a host follows (as Bitwig
    // shows the param's automation), springing back to the middle as it is let go, and leaves nothing to undo.
    {
        const Rect wheel = detail::bendWheel(detail::keyboardTop(false));
        const int bendParam = paramIndex(GlobalParam::PitchBend);
        const UndoStep *before = model.nextUndo();
        model.begun.clear();
        const Point up{wheel.center().x, wheel.y + wheel.h * 0.15f};
        view.mouseDown(wheel.center(), {});
        view.mouseMove(up, {});
        const double bent = model.value(bendParam);
        const bool wheelShot = renderer.save(view, dir / "26g3-bend-wheel-up.png");
        view.mouseUp(up, {});
        std::printf("      the bend wheel dragged up: Pitch Bend %.2f, %.2f let go\n", bent, model.value(bendParam));
        check(wheelShot && bent > 0.6 && model.value(bendParam) == 0.0 && model.begun == std::vector<int>{bendParam} &&
                  model.openEdits == 0 && model.nextUndo() == before,
              "the bend wheel edits the Pitch Bend param in one gesture, springing back to the middle, nothing to undo");
    }

    // The macros', the mod wheel's and the note source's menus pick their polarity too, the one in force checked:
    // from a macro's knob or handle, the wheel or its handle, and the note source's display. A bipolar macro's
    // arc starts from its knob's middle.
    {
        auto polarity = [&](Point at, const std::string &label) {
            bool checked = false;
            for (const MenuItem &item : view.contextMenu(at))
                if (item.label == label) {
                    checked = item.checked;
                    view.menuCommand(item.command);
                }
            return checked;
        };
        const bool macroWasUnipolar = polarity(macroKnob(0), tr(Str::MenuUnipolar));
        polarity(macroKnob(0), tr(Str::MenuBipolar));
        const bool macroBipolar = model.value(paramIndex(0, MacroParam::Bipolar)) == 1.0;
        const bool handleShowsIt = polarity({macroKnob(0).x - 32.0f, y}, tr(Str::MenuBipolar));
        polarity({74.0f + 13.0f, view.height() - 87.0f}, tr(Str::MenuBipolar)); // the wheel itself
        const bool wheelBipolar = model.value(paramIndex(WheelParam::Bipolar)) == 1.0;
        polarity({1026.0f, y}, tr(Str::MenuBipolar)); // the note source's display
        const bool noteBipolar = model.value(paramIndex(NoteParam::Bipolar)) == 1.0;
        view.mouseMove({600.0f, 328.0f}, {});
        const bool stripShot = renderer.save(view, dir / "26h-bipolar-strip.png");
        polarity({87.0f, y + 21.0f}, tr(Str::MenuUnipolar)); // the wheel's handle
        const bool bendWasBipolar = polarity({47.0f, y + 21.0f}, tr(Str::MenuUnipolar)); // the bend wheel's handle
        const bool bendUnipolar = model.value(paramIndex(BendParam::Bipolar)) == 0.0;
        polarity({47.0f, y + 21.0f}, tr(Str::MenuBipolar));
        polarity(macroKnob(0), tr(Str::MenuUnipolar));
        polarity({1026.0f, y}, tr(Str::MenuUnipolar));
        check(macroWasUnipolar && macroBipolar && handleShowsIt && wheelBipolar && noteBipolar && stripShot &&
                  !bendWasBipolar && bendUnipolar && model.value(paramIndex(BendParam::Bipolar)) == 1.0 &&
                  model.value(paramIndex(0, MacroParam::Bipolar)) == 0.0 &&
                  model.value(paramIndex(WheelParam::Bipolar)) == 0.0 && model.value(paramIndex(NoteParam::Bipolar)) == 0.0,
              "the macros', the wheels' and the note source's menus make them unipolar or bipolar");
    }

    // The octave shift's small buttons, under the keys' right end, move the keys an octave: the first C, a C1, plays
    // a C2 after +, and a C1 again after -.
    {
        const Point firstC{140.0f, y + 121.0f}, down{1101.0f, y + 153.0f}, up{1169.0f, y + 153.0f};
        click(view, firstC);
        const int before = model.lastNote;
        click(view, up);
        click(view, firstC);
        const int shifted = model.lastNote;
        view.mouseMove(up, {});
        const bool upShot = renderer.save(view, dir / "26i-octave-up.png");
        click(view, down);
        click(view, firstC);
        check(before == 24 && shifted == 36 && model.lastNote == 24 && upShot,
              "the octave shift's buttons under the keys move them an octave up and back");
    }

    // Collapsed by its chevron or its title, the keyboard panel keeps its top row and the editor gets 120 px
    // shorter: the keys, the wheels and the octave shift go, and the wheels' handles stay under the title, where they
    // still map the wheels, onto filter 1's cutoff here.
    {
        const Point toggle{35.0f, y - 8.0f}, title{80.0f, y - 8.0f};
        const float open = view.height();
        click(view, toggle);
        const bool shorter = model.ui.keysCollapsed && view.height() == open - (detail::kKeyboardH - detail::kKeyboardBarH);
        drag(view, {87.0f, y + 21.0f}, ins(918.0f, 242.0f));
        drag(view, {47.0f, y + 21.0f}, ins(918.0f, 242.0f));
        const bool wheelMapped =
            slotTarget(model.value(slotTargetIndex(kWheelSource, 1))) == paramIndex(0, FilterParam::Cutoff) &&
            slotTarget(model.value(slotTargetIndex(kBendSource, 1))) == paramIndex(0, FilterParam::Cutoff);
        view.mouseMove(toggle, {});
        const bool collapsedShot = renderer.save(view, dir / "26j-keys-collapsed.png");
        click(view, title);
        check(shorter && wheelMapped && collapsedShot && !model.ui.keysCollapsed && view.height() == open,
              "the keyboard panel collapses to its top row, the wheels' handles staying in it, and opens again");
    }

    // Bend's knob, in the header after Pitch: the wheel turns it a semitone.
    {
        const double bendWas = model.value(paramIndex(GlobalParam::BendRange));
        view.wheel({detail::headerKnobX(3) + 18.0f, kFmAmountKnob.y}, 1.0f, {});
        check(model.value(paramIndex(GlobalParam::BendRange)) == bendWas + 1.0, "Bend's knob in the header turns");
    }

    // Its display opens its curve over the routing panel: the notes from Low to High across, their knobs in the
    // title row (whole notes as they are dragged); a double-click adds a point to its three, and the reset button
    // goes back to every MIDI note along the cutoff knobs' curve.
    click(view, {1026.0f, y});
    const int low = paramIndex(NoteParam::Low), high = paramIndex(NoteParam::High);
    const double lowFrom = model.value(low);
    drag(view, {182.0f + kRoutingDx, 152.0f}, {182.0f + kRoutingDx, 52.0f}); // Low's knob, 100 px up: ten notes, landing on a whole one
    const bool raised = model.value(low) == std::round(lowFrom) + 10.0;
    view.doubleClick(noteCurveAt(0.5, 0.6), {});
    const bool pointAdded = model.value(paramIndex(NoteParam::Curve)) == 4.0;
    // Steps, under the plot left of the grid's controls, has a plain click draw a step here too; Points goes back.
    click(view, {514.0f + kRoutingDx, 486.0f});
    click(view, noteCurveAt(0.2, -0.5));
    const bool stepped = model.layout().shapeSteps && model.value(paramIndex(NoteParam::Curve)) > 4.0;
    view.undo();
    click(view, {446.0f + kRoutingDx, 486.0f});
    click(view, noteCurveAt(0.2, -0.5));
    const bool modes = stepped && !model.layout().shapeSteps && model.value(paramIndex(NoteParam::Curve)) == 4.0;
    model.live.notePitch = 72.0f;
    view.poll();
    view.mouseMove(noteCurveAt(0.5, 0.6), {});
    const bool curveShot = renderer.save(view, dir / "26c-note-curve.png");
    click(view, kNoteCurveReset);
    const bool reset = model.value(low) == paramTable()[low].def && model.value(high) == paramTable()[high].def &&
                       model.value(paramIndex(NoteParam::Curve)) == 3.0;
    click(view, kRoutingEditorDone);
    check(raised && pointAdded && modes && curveShot && reset,
          "the note source's display opens its curve, with its range's knobs, Points | Steps working there, and the "
          "reset button puts both back");
}

// The output meter left of Volume: each side's peak in the theme's color, and the zone past full scale red while the
// clip light is on (read off the rendered pixels).
void testMeter(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.live.outPeak = {0.5f, 0.125f}; // -6 and -18 dB
    view.poll();
    const bool shot = renderer.save(view, dir / "26d-meter.png");
    const uint32_t left = renderer.pixel({kHeaderStart + 2.5f, 59.0f}), right = renderer.pixel({kHeaderStart + 11.5f, 59.0f}); // about -14 dB
    const uint32_t dark = renderer.pixel({kHeaderStart + 2.5f, 46.5f});
    model.live.outPeak = {1.4f, 1.4f};
    model.live.clipHold = 1.0f;
    view.poll();
    const bool clipShot = renderer.save(view, dir / "26e-meter-clipping.png");
    const uint32_t lit = renderer.pixel({kHeaderStart + 2.5f, 46.5f});
    check(shot && clipShot && (left & 255) > 200 && (right & 255) < 100 && (dark >> 16 & 255) < 120 &&
              (lit >> 16 & 255) > 200 && (lit >> 8 & 255) < 100,
          "the output meter shows each side's peak, and its clip light past full scale");
}

// The output scope left of the meter: with nothing captured, its display empty; then the telemetry's span, two
// periods of a quiet sine, drawn across it at its peak, the first crest at the display's top and the second trough at
// its bottom (read off the rendered pixels).
void testOutScope(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    view.poll();
    const float innerX = kOutScopeX + 5.0f, innerW = kOutScopeW - 10.0f;
    const float top = kOutScopeY + 4.0f, bottom = kOutScopeY + kOutScopeH - 4.0f;
    auto binX = [&](double bin) { return innerX + innerW * static_cast<float>(bin / (kOutScopeBins - 1)); };
    const Point crest{binX((kOutScopeBins - 1) / 8.0), top}, trough{binX(7.0 * (kOutScopeBins - 1) / 8.0), bottom};
    const bool idleShot = renderer.save(view, dir / "26f-out-scope-idle.png");
    const uint32_t idleCrest = renderer.pixel(crest), idleTrough = renderer.pixel(trough);
    for (int bin = 0; bin < kOutScopeBins; ++bin)
        model.live.outScope[bin] =
            0.2f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 2.0 * bin / (kOutScopeBins - 1)));
    view.poll();
    const bool shot = renderer.save(view, dir / "26g-out-scope.png");
    const uint32_t litCrest = renderer.pixel(crest), litTrough = renderer.pixel(trough);
    std::printf("output scope: crest %06x -> %06x, trough %06x -> %06x\n", idleCrest, litCrest, idleTrough, litTrough);
    check(idleShot && shot && (idleCrest & 255) < 100 && (idleTrough & 255) < 100 && (litCrest & 255) > 140 &&
              (litTrough & 255) > 140,
          "the output scope is empty with nothing captured, and draws the captured span at its peak");

    // A span 8 dB quieter (still above the 24 dB the scope lifts at most) is drawn to the level it eases down from, not
    // at once to its own peak: smaller at first, its crest at the top again some polls later.
    for (int bin = 0; bin < kOutScopeBins; ++bin)
        model.live.outScope[bin] =
            0.08f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 2.0 * bin / (kOutScopeBins - 1)));
    const bool easing = view.poll();
    renderer.save(view, dir / "26h-out-scope-easing.png");
    const uint32_t easingCrest = renderer.pixel(crest);
    int polls = 0;
    while (view.poll() && polls < 600)
        ++polls;
    renderer.save(view, dir / "26i-out-scope-eased.png");
    const uint32_t easedCrest = renderer.pixel(crest);
    std::printf("output scope easing: crest %06x, after %d polls %06x\n", easingCrest, polls, easedCrest);
    check(easing && (easingCrest & 255) < 100 && polls > 10 && polls < 100 && (easedCrest & 255) > 140,
          "the output scope's height eases down to a quieter span, repainting until it gets there");

    // Spans too small to draw (the browser demo's output never quite reaches 0) paint nothing again as they change;
    // one that draws does, and so does the next one replacing it, even too small to draw.
    auto fill = [&](float level) {
        for (int bin = 0; bin < kOutScopeBins; ++bin)
            model.live.outScope[bin] = level * static_cast<float>(((bin * 7 + 3) % 11) - 5);
    };
    fill(0.0f);
    view.poll();
    const bool settled = !view.poll();
    fill(1e-12f);
    const bool tinyFirst = view.poll();
    fill(-2e-12f);
    const bool tinySecond = view.poll();
    fill(0.01f);
    const bool drawing = view.poll();
    fill(1e-12f);
    const bool replaced = view.poll();
    std::printf("output scope repaints: settled %d, tiny %d %d, drawing %d, replaced %d\n", settled, tinyFirst,
                tinySecond, drawing, replaced);
    check(settled && !tinyFirst && !tinySecond && drawing && replaced,
          "the output scope's spans too small to draw paint nothing again, and one that draws does");
}

// The three-band dynamics, alone in the chain: a card one and a half places wide (from x 46) whose display (14 px in,
// from y 222) has a bar a band, the high band on top (y 229, 246 and 263, 12 tall), levels across from -80 dB (-kMdRangeDb) at x 98
// (38 px into the display) to 0 dB 40 px short of its right end. A band's row picks the band its knobs set; its threshold lines, on every band, drag across (each
// stopping at the other), picking their band, Alt-dragged up they turn its ratio, and a double-click puts the
// threshold back. LINK makes the knobs and every band's lines set the low band; RMS switches the detector to peaks.
// Its knobs for which way a threshold or ratio works tell it in a tooltip under their label, and the bars show what the
// telemetry says.
// The amp's card: added after the shaper in the list of effects; its model and cabinet lists picking, each an undo
// step; the cabinet's power switch dimming its controls (read off the rendered pixels), which still turn; the mic
// dragged on its display setting Position and Distance in one undo step, and double-clicked back; the drive meter
// following the telemetry and the display's curve moving with Treble and with Position (read off the rendered
// pixels); and its title's menu offering the oversampling, the one in force checked.
void testAmp(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    addFx(view, FxType::Amp);
    auto at = [&](FxParam f) { return paramIndex(0, f); };
    auto centerOf = [](Rect r) { return Point{r.x + r.w * 0.5f, r.y + r.h * 0.5f}; };
    check(model.value(at(FxParam::Type)) == static_cast<double>(FxType::Amp) && detail::fxOfferedPlace(FxType::Amp) == 1,
          "the list of effects adds an amp, offered after the shaper");
    const Rect card = detail::fxCard(0.0f, detail::fxSpan(FxType::Amp));
    const Point away{600.0f, 628.0f};
    view.mouseMove(away, {});
    check(renderer.save(view, dir / "9f-amp.png"), "render the amp's card");
    check(model.value(at(FxParam::AmpModel)) == static_cast<double>(AmpModel::Crunch) &&
              model.value(at(FxParam::AmpCabOn)) == 1.0 &&
              model.value(at(FxParam::AmpCab)) == static_cast<double>(AmpCab::Closed4x12),
          "a new amp is a Crunch into a closed 4x12, the cabinet on");

    // The model's list picking Heavy; undone.
    constexpr int kModels = static_cast<int>(AmpModel::Count), kCabs = static_cast<int>(AmpCab::Count);
    click(view, centerOf(detail::ampModelButton(card)));
    view.mouseMove(away, {});
    const bool modelsShot = renderer.save(view, dir / "9f1-amp-models.png");
    click(view, centerOf(detail::listMenuItem(detail::ampModelButton(card), kModels, static_cast<int>(AmpModel::Heavy))));
    const bool heavy = model.value(at(FxParam::AmpModel)) == static_cast<double>(AmpModel::Heavy);
    view.undo();
    check(modelsShot && heavy && model.value(at(FxParam::AmpModel)) == static_cast<double>(AmpModel::Crunch),
          "the amp's model list picks a model, undoably");
    // The cabinet's list picking a 1x15; undone.
    click(view, centerOf(detail::ampCabButton(card)));
    view.mouseMove(away, {});
    const bool cabsShot = renderer.save(view, dir / "9f2-amp-cabinets.png");
    click(view, centerOf(detail::listMenuItem(detail::ampCabButton(card), kCabs, static_cast<int>(AmpCab::Bass1x15))));
    const bool bass = model.value(at(FxParam::AmpCab)) == static_cast<double>(AmpCab::Bass1x15);
    view.undo();
    check(cabsShot && bass && model.value(at(FxParam::AmpCab)) == static_cast<double>(AmpCab::Closed4x12),
          "the amp's cabinet list picks a cabinet, undoably");
    // The mic's three under the cabinet's display.
    click(view, centerOf(detail::ampMicChoice(card, static_cast<int>(AmpMic::Ribbon))));
    const bool ribbon = model.value(at(FxParam::AmpMic)) == static_cast<double>(AmpMic::Ribbon);
    view.undo();
    check(ribbon && model.value(at(FxParam::AmpMic)) == static_cast<double>(AmpMic::Dynamic),
          "the amp's mic switch picks its mic, undoably");

    // The cabinet switched off: its display, mic switch and knobs dim (Position's ring, read off the pixels), Output's
    // doesn't, and Position still turns.
    const Point position = detail::ampKnob(card, true, 0, 0), output = detail::ampKnob(card, true, 0, 1);
    // On the rings (radius 12 of the 28 px knobs, whose middles are left unfilled), a little past where they start at
    // the lower left, inside Position's value arc.
    const Point ring{position.x - 11.5f, position.y + 3.5f}, outRing{output.x - 11.5f, output.y + 3.5f};
    renderer.save(view, dir / "9f-amp.png");
    const uint32_t ringOn = renderer.pixel(ring), outOn = renderer.pixel(outRing);
    click(view, centerOf(detail::ampCabPower(card)));
    view.mouseMove(away, {});
    const bool offShot = renderer.save(view, dir / "9f3-amp-cabinet-off.png");
    const bool dimmed = renderer.pixel(ring) != ringOn && renderer.pixel(outRing) == outOn;
    const double before = model.value(at(FxParam::AmpPosition));
    drag(view, position, {position.x, position.y - 30.0f});
    const bool turns = model.value(at(FxParam::AmpPosition)) > before;
    view.undo();
    view.undo();
    check(offShot && dimmed && turns && model.value(at(FxParam::AmpCabOn)) == 1.0,
          "the amp's cabinet switched off dims its controls, which still turn");

    // The mic dragged up and to the right on its display: Position and Distance both, one undo step; a double-click
    // puts it back where it starts.
    const Rect cabDisplay = detail::ampCabDisplay(card);
    const Point mic = detail::ampMicPoint(cabDisplay, model.value(at(FxParam::AmpPosition)),
                                          model.value(at(FxParam::AmpDistance)));
    drag(view, mic, {mic.x + 30.0f, mic.y - 10.0f});
    const bool moved = model.value(at(FxParam::AmpPosition)) > 0.35 && model.value(at(FxParam::AmpDistance)) > 0.3;
    view.undo();
    const bool undone = std::abs(model.value(at(FxParam::AmpPosition)) - 0.3) < 1e-9 &&
                        std::abs(model.value(at(FxParam::AmpDistance)) - normFromAmpDistance(2.0)) < 1e-9;
    drag(view, mic, {mic.x + 30.0f, mic.y - 10.0f});
    const Point movedMic = detail::ampMicPoint(cabDisplay, model.value(at(FxParam::AmpPosition)),
                                               model.value(at(FxParam::AmpDistance)));
    doubleClick(view, movedMic);
    check(moved && undone && std::abs(model.value(at(FxParam::AmpPosition)) - 0.3) < 1e-9,
          "the amp's mic drags across and away from the cone, one undo step, and a double-click puts it back");

    // The drive meter, at the tone display's right edge, filling with the telemetry.
    const Rect tone = detail::ampToneDisplay(card);
    const Point meter{tone.right() - 9.0f, tone.bottom() - 12.0f};
    renderer.save(view, dir / "9f-amp.png");
    const uint32_t empty = renderer.pixel(meter);
    model.live.fxReduction[0] = {18.0f, 0.0f, 0.0f};
    view.poll();
    const bool meterShot = renderer.save(view, dir / "9f4-amp-driven.png");
    const bool filled = renderer.pixel(meter) != empty;
    model.live.fxReduction[0] = {0.0f, 0.0f, 0.0f};
    view.poll();
    check(meterShot && filled, "the amp's drive meter follows the telemetry");
    // The curve: Treble up lifts its top end, the mic toward the cone's edge takes it down (read off a column at
    // 3 kHz, where the cabinet's breakup peak sits).
    auto column = [&] {
        renderer.save(view, dir / "9f-amp.png");
        std::vector<uint32_t> out;
        const float x = tone.x + 6.0f + (tone.w - 26.0f) * static_cast<float>(std::log10(3000.0 / 20.0) / 3.0);
        for (float y = tone.y + 18.0f; y < tone.bottom() - 6.0f; y += 1.0f)
            out.push_back(renderer.pixel({x, y}));
        return out;
    };
    const std::vector<uint32_t> flat = column();
    model.set(at(FxParam::AmpTreble), 1.0);
    const bool treble = column() != flat;
    model.set(at(FxParam::AmpTreble), 0.5);
    model.set(at(FxParam::AmpPosition), 1.0);
    const bool edge = column() != flat;
    model.set(at(FxParam::AmpPosition), 0.3);
    check(treble && edge, "the amp's display follows its Treble and its mic's position");

    // Its title's menu: the oversampling, 4x checked, and 8x picked.
    const Point title{card.x + 80.0f, card.y + 17.0f};
    bool checked4 = false;
    int eight = -1;
    for (const MenuItem &item : view.contextMenu(title)) {
        if (item.label == trf(Str::MenuOversampling, {"4x"}))
            checked4 = item.checked;
        if (item.label == trf(Str::MenuOversampling, {"8x"}))
            eight = item.command;
    }
    view.menuCommand(eight);
    const bool set = model.value(at(FxParam::AmpOversampling)) == 3.0;
    view.undo();
    check(checked4 && set && model.value(at(FxParam::AmpOversampling)) == 2.0,
          "the amp's title menu picks its oversampling, the one in force checked");
}

// The utility: added last from the list of effects, leaving the sound as it is (0 dB, centered, 100 % wide, Mono Bass
// Off), its Gain at 0 dB below the strip's top (+12 dB); the handles on its display: Mono Bass's dragged up from Off, Pan's across, Width's out along the band's edge and
// Gain's down its strip, each one undo step; the band narrowing into its middle below Mono Bass's line (read off the
// rendered pixels); and Mono Bass's handle double-clicked back to Off.
void testUtility(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    addFx(view, FxType::Utility);
    auto at = [&](FxParam f) { return paramIndex(0, f); };
    check(model.value(at(FxParam::Type)) == static_cast<double>(FxType::Utility) &&
              detail::fxOfferedPlace(FxType::Utility) == detail::kFxTypes - 1,
          "the list of effects adds a utility, offered last, in its stereo and phase group");
    const double unity = utilityGainNorm(1.0); // Gain at 0 dB
    check(model.value(at(FxParam::UtilityGain)) == unity && model.value(at(FxParam::UtilityPan)) == 0.0 &&
              model.value(at(FxParam::UtilityWidth)) == 1.0 && model.value(at(FxParam::UtilityMonoBass)) == 0.0,
          "a new utility leaves the sound as it is, its Mono Bass Off");
    const Rect display = detail::fxDisplayIn(detail::fxCard(0.0f), FxType::Utility);
    const Rect strip = detail::utilityStrip(display), field = detail::utilityField(display);
    const Point away{600.0f, 628.0f};

    // Mono Bass's handle, at the field's bottom while it is Off, up to a third of the way.
    drag(view, {field.right() - 6.0f, field.bottom()}, {field.right() - 6.0f, field.bottom() - 0.3f * field.h});
    const double mono = model.value(at(FxParam::UtilityMonoBass));
    // Pan's, at the top of the middle line, a third of the way to the left.
    drag(view, {field.x + 0.5f * field.w, field.y + 4.0f}, {field.x + 0.35f * field.w, field.y + 4.0f});
    const double pan = model.value(at(FxParam::UtilityPan));
    // Width's, on the band's right edge (the side with more room), out to half as wide again.
    const float middle = detail::utilityMiddle(field, pan);
    const float widthY = field.y + detail::kUtilityWidthHandleY * field.h;
    drag(view, {middle + 0.25f * field.w, widthY}, {middle + 0.375f * field.w, widthY});
    const double width = model.value(at(FxParam::UtilityWidth));
    // Gain's, at 0 dB on the strip, down to its middle: -6 dB.
    drag(view, {strip.center().x, strip.bottom() - static_cast<float>(unity) * strip.h},
         {strip.center().x, strip.y + 0.5f * strip.h});
    const double gain = model.value(at(FxParam::UtilityGain));
    std::printf("      utility's handles: mono bass %.3f, pan %.3f, width %.3f, gain %.3f\n", mono, pan, width, gain);
    check(std::abs(mono - 0.3) < 0.02 && std::abs(pan + 0.3) < 0.02 && std::abs(width - 1.5) < 0.03 &&
              std::abs(gain - 0.5) < 0.02,
          "the utility's display handles set Mono Bass, Pan, Width and Gain");
    view.undo();
    const bool gainUndone = model.value(at(FxParam::UtilityGain)) == unity && model.value(at(FxParam::UtilityWidth)) == width;
    view.redo();
    check(gainUndone && model.value(at(FxParam::UtilityGain)) == gain, "each drag of a utility handle is one undo step");

    // The band: wide above Mono Bass's line, gone into its middle below it, where the display shows through.
    view.mouseMove(away, {});
    const bool shot = renderer.save(view, dir / "9g-utility.png");
    const float spread = detail::utilitySpread(field, width), x = middle + 0.6f * spread;
    auto bandAt = [&](float y) { return renderer.pixel({x, y}) != renderer.pixel({field.right() - 14.0f, y}); };
    const bool high = bandAt(field.y + 0.2f * field.h), low = bandAt(field.bottom() - 0.04f * field.h);
    check(shot && high && !low, "the utility's band narrows into its middle below Mono Bass's line");

    // Mono Bass's handle double-clicked: Off again.
    doubleClick(view, {field.right() - 6.0f, field.bottom() - static_cast<float>(mono) * field.h});
    check(model.value(at(FxParam::UtilityMonoBass)) == 0.0, "a double-click puts a utility handle back where it starts");
}

void testMultiDyn(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, kFxTab);
    addFx(view, FxType::MultiDyn);
    check(model.value(paramIndex(0, FxParam::Type)) == static_cast<double>(FxType::MultiDyn),
          "the list of effects adds a three-band dynamics");
    auto x = [](double db) {
        return 98.0f + static_cast<float>((db + detail::kMdRangeDb) / detail::kMdRangeDb) * (1.5f * kFxStep - 22.0f - 106.0f);
    };
    constexpr float kHighBar = 229.0f, kMidBar = 246.0f;
    const float mid = kMidBar + 6.0f, high = kHighBar + 6.0f; // down the middle of their bars
    click(view, {74.0f, mid});
    const Point above = fxKnob(model, 0, 2, 5, 142.0f);
    drag(view, above, {above.x, above.y - 20.0f});
    check(model.value(mdAboveIndex(0, 1)) > -20.0 && model.value(mdAboveIndex(0, 0)) == -20.0,
          "a three-band dynamics band's row picks which band its knobs set");
    view.undo();
    // The mid band's Below line dragged to -50 dB, and its Above line dragged past it.
    drag(view, {x(-40.0), mid}, {x(-50.0), mid});
    const bool below = std::abs(model.value(mdBelowIndex(0, 1)) + 50.0) < 0.5 && model.value(mdBelowIndex(0, 0)) == -40.0;
    drag(view, {x(-20.0), mid}, {x(-55.0), mid});
    const bool stopped = model.value(mdAboveIndex(0, 1)) == model.value(mdBelowIndex(0, 1));
    view.undo();
    view.undo();
    check(below && stopped && model.value(mdBelowIndex(0, 1)) == -40.0 && model.value(mdAboveIndex(0, 1)) == -20.0,
          "a three-band dynamics' threshold lines drag across, Above stopping at Below, undoably");
    Modifiers alt;
    alt.alt = true;
    view.mouseDown({x(-20.0), mid}, alt);
    view.mouseMove({x(-20.0), mid - 40.0f}, alt);
    view.mouseUp({x(-20.0), mid - 40.0f}, alt);
    const bool ratio =
        model.value(mdDownIndex(0, 1)) > normFromDynRatio(3.0) && model.value(mdAboveIndex(0, 1)) == -20.0;
    drag(view, {x(-20.0), mid}, {x(-30.0), mid});
    doubleClick(view, {x(-30.0), mid});
    check(ratio && model.value(mdAboveIndex(0, 1)) == -20.0,
          "Alt-dragging a threshold line turns its ratio, and a double-click puts the threshold back");
    // The ratio's lines, moving with their threshold: at 3:1, what goes in 6 dB over Above (-14 dB) comes out at
    // -18 dB, so a line stands there, and none between it and the next (-16 dB). At 1:1 the line moves on to -14 dB.
    view.mouseMove({600.0f, 628.0f}, {});
    model.set(mdDownIndex(0, 1), normFromDynRatio(3.0));
    const bool linesShot = renderer.save(view, dir / "9e1-3band-dynamics-ratio-lines.png");
    const bool line = renderer.pixel({x(-18.0), mid}) != renderer.pixel({x(-17.1), mid});
    model.set(mdDownIndex(0, 1), normFromDynRatio(1.0));
    renderer.save(view, dir / "9e1-3band-dynamics-ratio-lines.png");
    const bool moved = renderer.pixel({x(-18.0), mid}) == renderer.pixel({x(-17.1), mid}) &&
                       renderer.pixel({x(-14.0), mid}) != renderer.pixel({x(-15.1), mid});
    model.set(mdDownIndex(0, 1), normFromDynRatio(3.0));
    check(linesShot && line && moved, "a three-band dynamics' bar draws its ratio as lines packed toward the threshold");
    // The zones beyond the lines drag up and down for their side's ratio: the mid band's upward one left of Below up
    // for more, and the high band's downward one right of Above down for more, which picks the high band.
    drag(view, {x(-55.0), mid}, {x(-55.0), mid - 40.0f});
    const bool upward = model.value(mdUpIndex(0, 1)) > normFromDynRatio(2.5) &&
                        model.value(mdDownIndex(0, 1)) == normFromDynRatio(3.0);
    drag(view, {x(-5.0), high}, {x(-5.0), high + 40.0f});
    const bool downward = model.value(mdDownIndex(0, 2)) > normFromDynRatio(3.5) &&
                          model.value(mdUpIndex(0, 2)) == normFromDynRatio(2.0);
    drag(view, above, {above.x, above.y - 20.0f}); // the high band's knobs show now
    const bool picked = model.value(mdAboveIndex(0, 2)) > -20.0 && model.value(mdAboveIndex(0, 1)) == -20.0;
    view.undo();
    view.undo();
    view.undo();
    check(upward && downward && picked && model.value(mdUpIndex(0, 1)) == normFromDynRatio(2.0) &&
              model.value(mdDownIndex(0, 2)) == normFromDynRatio(3.0),
          "a three-band dynamics' zones drag up and down for their ratios, undoably, picking their band");
    // The high band's Above line, dragged while the mid band is picked, picks the high band: the knobs set it now.
    click(view, {74.0f, mid});
    drag(view, {x(-20.0), high}, {x(-25.0), high});
    const bool dragged = std::abs(model.value(mdAboveIndex(0, 2)) + 25.0) < 0.5;
    drag(view, above, {above.x, above.y - 20.0f});
    check(dragged && model.value(mdAboveIndex(0, 2)) > -24.0 && model.value(mdAboveIndex(0, 1)) == -20.0,
          "dragging a three-band dynamics' line on another band picks that band");
    // Hovered, the Above knob says what it is under its label (its value now).
    view.mouseMove({600.0f, 628.0f}, {});
    const bool shot = renderer.save(view, dir / "9e-3band-dynamics.png");
    const uint32_t bare = renderer.pixel({above.x, above.y + 48.0f});
    view.mouseMove(above, {});
    const bool tipShot = renderer.save(view, dir / "9f-3band-dynamics-tip.png");
    check(shot && tipShot && renderer.pixel({above.x, above.y + 48.0f}) != bare,
          "a three-band dynamics' Above knob shows its tooltip under its label");
    // The mid band at -20 dB pulled down 6 dB: its meter, down the bar's middle, fills up to -26 dB in teal, then
    // amber up to -20 dB.
    view.mouseMove({600.0f, 628.0f}, {});
    const uint32_t empty = renderer.pixel({x(-45.0), mid});
    model.live.fxBandLevel[0] = {0.0f, 0.1f, 0.0f};
    model.live.fxBandGain[0] = {0.0f, -6.0f, 0.0f};
    view.poll();
    const bool levelShot = renderer.save(view, dir / "9g-3band-dynamics-levels.png");
    const uint32_t filled = renderer.pixel({x(-45.0), mid});
    const uint32_t taken = renderer.pixel({x(-23.0), mid});
    const uint32_t past = renderer.pixel({x(-15.0), mid});
    check(levelShot && filled != empty && taken != filled && past != filled && past != taken,
          "a three-band dynamics' bar shows its band's level and gain");
    // The meter shows the dynamics alone: the band's Gain, after them, at +12 dB leaves the 6 dB taken off in amber
    // (it would have turned it into 6 dB added, in the accent, past what goes in).
    model.set(mdGainIndex(0, 1), 12.0);
    view.poll();
    const bool gainShot = renderer.save(view, dir / "9g1-3band-dynamics-levels-gain.png");
    const bool gainLeftOut = gainShot && renderer.pixel({x(-23.0), mid}) == taken && renderer.pixel({x(-15.0), mid}) == past;
    model.set(mdGainIndex(0, 1), 0.0);
    view.poll();
    check(gainLeftOut, "a three-band dynamics' meter shows what its dynamics do, its band's Gain left out");
    // LINK: an edit of one band, by its line or its knob, sets every band's same field, as one undo step; RMS off
    // follows peaks.
    const Rect mdCard = detail::fxCard(0.0f, detail::fxSpan(FxType::MultiDyn));
    click(view, detail::mdLinkButton(mdCard).center());
    drag(view, {x(-40.0), mid}, {x(-45.0), mid});
    bool linkedLine = true;
    for (int band = 0; band < kMcBands; ++band)
        linkedLine = linkedLine && std::abs(model.value(mdBelowIndex(0, band)) + 45.0) < 0.5;
    drag(view, above, {above.x, above.y - 20.0f}); // the mid band's knob, its line picked it
    const double raised = model.value(mdAboveIndex(0, 1));
    const bool linked = model.value(paramIndex(0, FxParam::MdLink)) == 1.0 && raised > -20.0 &&
                        model.value(mdAboveIndex(0, 0)) == raised && model.value(mdAboveIndex(0, 2)) == raised;
    view.undo();
    bool undone = true;
    for (int band = 0; band < kMcBands; ++band)
        undone = undone && model.value(mdAboveIndex(0, band)) < raised;
    click(view, detail::mdRmsButton(mdCard).center());
    view.mouseMove({600.0f, 628.0f}, {});
    check(linkedLine && linked && undone && model.value(paramIndex(0, FxParam::MdRms)) == 0.0 &&
              renderer.save(view, dir / "9h-3band-dynamics-linked.png"),
          "a three-band dynamics' LINK makes an edit of one band set every band's, undoably, and RMS switches to peaks");

    // OTT, from its card's menu (with Link still on): each band set after the OTT preset (see mdOttSettings()), Link
    // off, as one undo step. Another effect's menu doesn't offer it.
    auto pick = [&](Point at, Str label) {
        bool offered = false;
        for (const MenuItem &item : view.contextMenu(at))
            if (item.label == tr(label)) {
                offered = true;
                view.menuCommand(item.command);
            }
        return offered;
    };
    const double aboveBefore = model.value(mdAboveIndex(0, 1));
    const double attackBefore = model.value(paramIndex(0, FxParam::MdAttack));
    const bool offered = pick(fxGrip(model, 0), Str::MenuOtt);
    bool ott = offered;
    for (const auto &[index, value] : mdOttSettings(0))
        ott = ott && model.value(index) == value;
    ott = ott && model.value(paramIndex(0, FxParam::MdLink)) == 0.0 &&
          std::abs(model.value(mdAboveIndex(0, 1)) + 37.3) < 1e-9 &&
          std::isinf(dynRatio(model.value(mdDownIndex(0, 2)))) &&
          std::abs(dynRatio(model.value(mdUpIndex(0, 0))) - 4.17) < 1e-6 &&
          std::abs(cutoffFromNorm(model.value(paramIndex(0, FxParam::MdLowFreq))) - 88.3) < 0.01 &&
          model.value(paramIndex(0, FxParam::MdRange)) == 36.0;
    const bool ottShot = renderer.save(view, dir / "9i-3band-dynamics-ott.png");
    view.undo();
    const bool ottUndone = model.value(paramIndex(0, FxParam::MdLink)) == 1.0 &&
                           model.value(mdAboveIndex(0, 1)) == aboveBefore &&
                           model.value(paramIndex(0, FxParam::MdAttack)) == attackBefore;
    addFx(view, FxType::Delay);
    check(ott && ottShot && ottUndone && !pick(fxGrip(model, 1), Str::MenuOtt),
          "a three-band dynamics' menu sets it as OTT, undoably, and no other effect's menu offers OTT");
}

// The dashed cards after the sources: an LFO's above an envelope's, each adding one.
// The dashed card after the sources (with none yet, in the first place) opens the list of source types at the mouse,
// from 8 px in: an entry 26 px a type, from y 536 + 12. LFO adds an LFO; after it, a click around the list closes it
// adding nothing, and Envelope adds an envelope.
void testSourceShadow(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    // The list opens where the card is clicked: its entries from there, not from the card's corner.
    const Point first{cardX(0) + 46.0f, 628.0f};
    auto entry = [&](Point click, SourceType type) { return sourceListItem(view, click, type); };
    click(view, first);
    view.mouseMove(entry(first, SourceType::Midi), {});
    const bool menuShot = renderer.save(view, dir / "29b-source-menu.png");
    click(view, entry(first, SourceType::Lfo));
    const bool lfo = shown(model) == std::vector<int>{0} &&
                     model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Lfo);
    const Point second{cardX(1) + 30.0f, 588.0f}; // after it, higher up
    click(view, second);
    click(view, {600.0f, 328.0f}); // around the list
    const bool closed = shown(model) == std::vector<int>{0};
    click(view, second);
    click(view, entry(second, SourceType::Env));
    const bool env = shown(model) == std::vector<int>{0, 1} &&
                     model.value(paramIndex(1, SourceParam::Type)) == static_cast<double>(SourceType::Env);
    view.mouseMove({cardX(2) + 46.0f, 628.0f}, {});
    check(menuShot && lfo && closed && env && renderer.save(view, dir / "29-source-shadow.png"),
          "the dashed card after the sources opens the list of source types at the mouse, which adds the one picked");
    // Left open while the panel collapses (as a state load or another editor can do), the list shuts with it, and
    // doesn't come back when the panel opens again: a click where its LFO entry was opens it afresh instead.
    const Point third{cardX(2) + 46.0f, 628.0f};
    click(view, third);
    model.ui.sourcesCollapsed = true;
    view.poll();
    model.ui.sourcesCollapsed = false;
    view.poll();
    click(view, entry(third, SourceType::Lfo));
    check(shown(model) == std::vector<int>{0, 1}, "the list of source types shuts when its dashed card goes");
}

// Velocity, at the strip's right end after the note source: its handle maps it like the note source's (faint until
// it does), its display shows the newest voice's velocity (0 to 127) and opens its curve over the routing panel,
// where a double-click adds a point and the reset button brings back the straight line; its menu picks its polarity.
void testVelocity(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const float y = view.height() - 179.0f;
    const Point handle{1092.0f, y}, ring{1098.0f, y - 10.0f}, display{1142.0f, y};
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "26k-velocity-idle.png");
    const int idle = ringLevel(renderer, ring, 20.0f);
    drag(view, handle, kOpPhaseKnob);
    const bool mapped = slotTarget(model.value(slotTargetIndex(kVelocitySource, 0))) == paramIndex(0, OpParam::Phase) &&
                        model.value(slotAmountIndex(kVelocitySource, 0)) == 0.3;
    model.live.voices = 1;
    model.live.noteVelocity = 100.0f / 127.0f;
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    const bool shot = renderer.save(view, dir / "26l-velocity.png");
    const int live = ringLevel(renderer, ring, 20.0f);
    std::string removal;
    for (const MenuItem &item : view.contextMenu(kOpPhaseKnob))
        if (item.label.find(tr(Str::SourceNameVelocity)) != std::string::npos)
            removal = item.label;
    std::printf("      velocity's handle: brightness %d idle, %d mapped\n", idle, live);
    check(mapped && shot && idle * 18 < live * 10 &&
              removal == trf(Str::MenuRemoveModulation, {tr(Str::SourceNameVelocity)}),
          "velocity's handle maps it like the note source's, and is faint until it does");
    // Its display's menu makes it bipolar, and back.
    bool bipolar = false;
    for (const MenuItem &item : view.contextMenu(display))
        if (item.label == tr(Str::MenuBipolar))
            view.menuCommand(item.command);
    bipolar = model.value(paramIndex(VelocityParam::Bipolar)) == 1.0;
    for (const MenuItem &item : view.contextMenu(display))
        if (item.label == tr(Str::MenuUnipolar))
            view.menuCommand(item.command);
    check(bipolar && model.value(paramIndex(VelocityParam::Bipolar)) == 0.0,
          "velocity's display menu makes it unipolar or bipolar");
    // Its curve, opened from the display: a straight line to begin with, two points; a double-click adds a third,
    // and the reset button takes it back to the line.
    const int curve = paramIndex(VelocityParam::Curve);
    click(view, display);
    view.doubleClick(noteCurveAt(0.5, 0.6), {});
    const bool pointAdded = model.value(curve) == 3.0;
    view.mouseMove(noteCurveAt(0.5, 0.6), {});
    const bool curveShot = renderer.save(view, dir / "26m-velocity-curve.png");
    click(view, kNoteCurveReset);
    const bool reset = model.value(curve) == 2.0 && model.value(shapeFieldIndex(curve, 1, PointField::Y)) == 1.0;
    click(view, display); // a second click closes it
    view.mouseMove({600.0f, 328.0f}, {});
    check(pointAdded && curveShot && reset && renderer.save(view, dir / "26n-velocity-closed.png"),
          "velocity's display opens its curve, where a point is added, and the reset button straightens it");
}

// A wire in the graph bends when dragged sideways (its label following its middle), keeps the bend through a save of
// the layout and back, goes back to how it bowed with a double-click or Reset Bend, and each is an undo step.
// A route's start dragged onto another node comes from there, with its amount and mode; an OUT line's start too.
void testRouteFrom(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(paramIndex(2, OpParam::Enabled), 1.0);
    model.set(matrixIndex(1, 0), 0.4);
    model.set(routeModeIndex(1, 0), static_cast<double>(RouteMode::LinearFm));
    view.poll();
    const UiLayout &layout = model.layout();
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1), c = nodeCenter(layout, 2);
    const detail::Wire w = detail::wireBetween(1, b, 0, a, kDefaultWireBend);
    const float d = std::hypot(w.control.x - w.start.x, w.control.y - w.start.y);
    // Its grip reaches back along the wire from where it leaves B: 16 px out still takes it.
    const Point grip{w.start.x + (w.control.x - w.start.x) / d * 16.0f, w.start.y + (w.control.y - w.start.y) / d * 16.0f};
    view.mouseMove(grip, {});
    const bool lit = renderer.save(view, dir / "41b-route-start-lit.png");
    view.mouseDown(grip, {});
    view.mouseMove({(grip.x + c.x) * 0.5f, (grip.y + c.y) * 0.5f}, {});
    view.mouseMove(c, {});
    const bool shot = renderer.save(view, dir / "41a-route-from.png");
    view.mouseUp(c, {});
    const bool moved = model.value(matrixIndex(1, 0)) == 0.0 && std::abs(model.value(matrixIndex(2, 0)) - 0.4) < 1e-6 &&
                       model.value(routeModeIndex(2, 0)) == static_cast<double>(RouteMode::LinearFm) &&
                       model.value(routeModeIndex(1, 0)) == 0.0;
    // A's line to OUT, by where it leaves A, onto B: B goes out at A's level, A no longer does.
    const double out = model.value(paramIndex(0, OpParam::Out));
    const detail::OutWire aOut = detail::outWireOf(0, a); // toward OUT
    drag(view, {aOut.start.x + aOut.dir.x * 4.0f, aOut.start.y + aOut.dir.y * 4.0f}, b);
    const bool outMoved = model.value(paramIndex(0, OpParam::Out)) == 0.0 &&
                          model.value(paramIndex(1, OpParam::Out)) == out;
    view.undo();
    view.undo();
    check(lit && shot && moved && outMoved && model.value(matrixIndex(1, 0)) == 0.4 && model.value(matrixIndex(2, 0)) == 0.0,
          "a route's start dragged onto another node comes from there, with its amount and mode, undoably");
}

// Ctrl+drag puts a node on its graph's grid, the nearest point it fits at: a second one dragged there goes to the next
// free point. While Ctrl is held over the graph, the grid's points show.
void testNodeGrid(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    for (int op : {1, 2})
        model.set(paramIndex(op, OpParam::Enabled), 1.0);
    view.poll();
    UiLayout &layout = model.layout();
    const Modifiers ctrl{false, true, false};
    auto onGrid = [&](int node, Point *at) {
        const Point c = nodeCenter(layout, node);
        for (int row = 0; row < detail::kRoutingGridRows; ++row)
            for (int col = 0; col < detail::kRoutingGridCols; ++col) {
                const Point g = detail::gridPoint(detail::kGraph, detail::kRoutingGridCols, detail::kRoutingGridRows, col, row);
                if (std::abs(g.x - c.x) < 0.5f && std::abs(g.y - c.y) < 0.5f) {
                    if (at)
                        *at = g;
                    return true;
                }
            }
        return false;
    };
    view.mouseMove({400.0f, 300.0f}, ctrl);
    const bool marks = renderer.save(view, dir / "42a-grid-marks.png");
    const Point to{430.0f, 230.0f};
    const Point b = nodeCenter(layout, 1);
    view.mouseDown(b, ctrl);
    view.mouseMove({(b.x + to.x) * 0.5f, (b.y + to.y) * 0.5f}, ctrl);
    view.mouseMove(to, ctrl);
    const bool dragShot = renderer.save(view, dir / "42b-grid-drag.png");
    view.mouseUp(to, ctrl);
    Point bAt{}, cAt{};
    const bool bSnapped = onGrid(1, &bAt);
    // C dragged with Ctrl onto where B landed: the next free point.
    const Point c = nodeCenter(layout, 2);
    view.mouseDown(c, ctrl);
    view.mouseMove({(c.x + bAt.x) * 0.5f, (c.y + bAt.y) * 0.5f}, ctrl);
    view.mouseMove(bAt, ctrl);
    view.mouseUp(bAt, ctrl);
    const bool cSnapped = onGrid(2, &cAt) && std::hypot(cAt.x - bAt.x, cAt.y - bAt.y) > 1.0f;
    // A plain drag leaves a node where it is let go of, off the grid.
    view.mouseMove({600.0f, 428.0f}, {});
    drag(view, cAt, {cAt.x + 13.0f, cAt.y + 7.0f});
    check(marks && dragShot && bSnapped && cSnapped && !onGrid(2, nullptr),
          "Ctrl+drag puts a node on the grid's nearest free point, and the grid's points show while Ctrl is held");
}

void testWireBend(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(matrixIndex(1, 0), 0.5);
    model.ui.keptRoutes.push_back(matrixIndex(1, 0));
    // B up and to the left of A, far enough that a third of the way along its wire is clear of the wire's label.
    model.ui.nodeX[0] = 0.5f;
    model.ui.nodeY[0] = 0.55f;
    model.ui.nodeX[1] = 0.2f;
    model.ui.nodeY[1] = 0.1f;
    view.poll();
    const UiLayout &layout = model.layout();
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1);
    const float span = std::hypot(a.x - b.x, a.y - b.y);
    const Point left{-(a.y - b.y) / span, (a.x - b.x) / span}; // left of B -> A
    // A point along the wire at `t` (0 at B's outline, 1 at the arrowhead's base by A), as it bows by `bend` of the
    // span (mirrors wireBetween() in routing.h: it leaves and meets the circles toward its control point).
    auto onWire = [&](float bend, float t) {
        const Point control{(a.x + b.x) * 0.5f + left.x * span * bend, (a.y + b.y) * 0.5f + left.y * span * bend};
        auto toward = [&](Point from, float by) {
            const float d = std::hypot(control.x - from.x, control.y - from.y);
            return Point{from.x + (control.x - from.x) / d * by, from.y + (control.y - from.y) / d * by};
        };
        const Point start = toward(b, 24.0f), end = toward(a, 32.0f);
        const float s = 1.0f - t;
        return Point{s * s * start.x + 2.0f * s * t * control.x + t * t * end.x,
                     s * s * start.y + 2.0f * s * t * control.y + t * t * end.y};
    };
    const Point grab = onWire(0.18f, 0.35f);
    view.mouseMove(grab, {});
    const bool lit = renderer.save(view, dir / "2g-wire-hover.png") && view.cursor() == CursorKind::Move;
    // Dragged 30 px further left, the curve's middle goes 30 px along: the bend grows by twice that share of the span.
    view.mouseDown(grab, {});
    view.mouseMove({grab.x + left.x * 15.0f, grab.y + left.y * 15.0f}, {});
    view.mouseMove({grab.x + left.x * 30.0f, grab.y + left.y * 30.0f}, {});
    view.mouseUp({grab.x + left.x * 30.0f, grab.y + left.y * 30.0f}, {});
    auto bendOf = [&] {
        for (const auto &[route, bend] : layout.wireBends)
            if (route == matrixIndex(1, 0))
                return bend;
        return kDefaultWireBend;
    };
    const float bent = bendOf();
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "2h-wire-bent.png");
    std::printf("      B -> A's wire dragged 30 px left: bend %.3f, from %.2f\n", bent, kDefaultWireBend);
    // Its label, at the curve's middle, went along: dragging it now changes the route's amount.
    const Point label = onWire(bent, 0.5f);
    drag(view, label, {label.x, label.y - 20.0f});
    const bool labelFollows = model.value(matrixIndex(1, 0)) > 0.5;
    view.undo();
    check(lit && shot && std::abs(bent - (kDefaultWireBend + 60.0f / span)) < 0.01f && labelFollows &&
              model.value(matrixIndex(1, 0)) == 0.5,
          "a wire dragged sideways bends, its label following its middle");
    view.undo();
    const bool undone = bendOf() == kDefaultWireBend;
    view.redo();
    // Dragged the other way past straight, it bows to its right; a double-click on it brings back the bow it
    // started with.
    const Point now = onWire(bendOf(), 0.35f);
    drag(view, now, {now.x - left.x * 120.0f, now.y - left.y * 120.0f});
    const bool right = bendOf() < 0.0f;
    doubleClick(view, onWire(bendOf(), 0.35f));
    const bool reset = bendOf() == kDefaultWireBend && layout.wireBends.empty();
    view.undo();
    bool menuReset = false;
    for (const MenuItem &item : view.contextMenu(onWire(bendOf(), 0.35f)))
        if (item.label == tr(Str::MenuResetBend)) {
            view.menuCommand(item.command);
            menuReset = bendOf() == kDefaultWireBend;
        }
    check(undone && right && reset && menuReset, "a wire bends either way, and a double-click or Reset Bend undoes it");
    // Disconnected, the wire's bend goes with it.
    const Point again = onWire(bendOf(), 0.35f);
    drag(view, again, {again.x + left.x * 30.0f, again.y + left.y * 30.0f});
    const bool bentAgain = !layout.wireBends.empty();
    for (const MenuItem &item : view.contextMenu(onWire(bendOf(), 0.35f)))
        if (item.label == tr(Str::MenuDisconnect))
            view.menuCommand(item.command);
    check(bentAgain && layout.wireBends.empty() && model.value(matrixIndex(1, 0)) == 0.0,
          "a wire disconnected takes its bend with it");
    // Along the graph's top, B right of A, a wire bowed up as far as it goes would leave the panel: it bows only as
    // far as stays inside, nothing drawn over the view tabs above (read off the rendered pixels), and keeps its bend.
    model.set(matrixIndex(1, 0), 0.5);
    model.ui.keptRoutes.push_back(matrixIndex(1, 0));
    model.ui.nodeX[0] = 0.2f;
    model.ui.nodeX[1] = 0.8f;
    model.ui.nodeY[0] = model.ui.nodeY[1] = 0.0f;
    view.poll();
    auto aboveGraph = [&] {
        std::vector<uint32_t> pixels;
        for (float y = 94.0f; y < 132.0f; y += 2.0f)
            for (float x = 150.0f; x < 750.0f; x += 3.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    const bool plainShot = renderer.save(view, dir / "2i-wire-at-edge.png");
    const std::vector<uint32_t> plain = aboveGraph();
    model.ui.wireBends.push_back({matrixIndex(1, 0), kMaxWireBend});
    view.poll();
    const bool fitShot = renderer.save(view, dir / "2j-wire-at-edge-bent.png");
    check(plainShot && fitShot && aboveGraph() == plain && bendOf() == kMaxWireBend,
          "a wire bowed out of the panel bows only as far as stays inside");
}

// How a route acts on its operator (RouteMode): a wire label under the mouse opens into a pill, its mode's part on
// the left (a click opens the list of modes, the wheel steps them) and an x on the right that removes the route, as an
// OUT label's does; resting, a label shows a mode other than Phase inside it. The mode is also picked from the
// right-click menus of its label and its matrix cell (whose corner shows the glyph), and from the operator page's
// INPUTS tab; each pick an undo step. Moved onto another operator by its arrowhead, a route keeps its mode;
// disconnected, it starts as Phase.
// The INPUTS tab's rows 30 px apart: a modulated depth showing its handles under it and its ranges just above it,
// its number left as it was (read off the rendered pixels); past the six rows the tab holds, the wheel scrolls the
// rows and its scrollbar's thumb dragged brings them back.
void testInputsScroll(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    // B..F, F1 and F2 into A, and A's feedback: eight rows.
    for (int op = 1; op < kOps; ++op) {
        model.set(paramIndex(op, OpParam::Enabled), 1.0);
        model.set(detail::routeParam(op, 0), 0.5);
    }
    for (int filter = 0; filter < kFilters; ++filter) {
        model.set(paramIndex(filter, FilterParam::Enabled), 1.0);
        model.set(detail::routeParam(kOps + filter, 0), 0.5);
    }
    model.set(detail::routeParam(0, 0), 0.3);
    model.ui.selectedNode = 0;
    view.poll();
    click(view, opTabPoint(InspectorTab::Inputs));
    view.mouseMove({600.0f, 468.0f}, {});
    // The first row's depth, 48 px wide at the inspector's right, and the rows' place in it.
    const Rect depth{detail::kInspector.x + 300.0f - 16.0f - 48.0f, detail::kInspector.y + 186.0f, 48.0f, 18.0f};
    auto pixels = [&](float top, float bottom) {
        std::vector<uint32_t> found;
        for (float y = top; y < bottom; y += 1.0f)
            for (float x = depth.x + 4.0f; x < depth.right() - 4.0f; x += 1.0f)
                found.push_back(renderer.pixel({x, y}));
        return found;
    };
    renderer.save(view, dir / "5l-op-inputs-many.png");
    const std::vector<uint32_t> number = pixels(depth.y + 4.0f, depth.bottom() - 4.0f);
    const std::vector<uint32_t> under = pixels(depth.bottom() + 1.0f, depth.bottom() + 9.0f);
    const std::vector<uint32_t> top = pixels(depth.y - 5.0f, depth.y - 2.0f);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(slotTargetIndex(0, 0), slotTargetValue(detail::routeParam(1, 0)));
    model.set(slotAmountIndex(0, 0), 0.3);
    view.poll();
    const bool modShot = renderer.save(view, dir / "5m-op-inputs-modulated.png");
    check(modShot && pixels(depth.y + 4.0f, depth.bottom() - 4.0f) == number &&
              pixels(depth.bottom() + 1.0f, depth.bottom() + 9.0f) != under && pixels(depth.y - 5.0f, depth.y - 2.0f) != top,
          "an INPUTS row's modulated depth has its handles under it and its ranges just above it, its number as it was");

    // The sixth row's depth is F1's; two notches down over the names, the eighth's, the feedback's, is there.
    const Point sixth{depth.center().x, depth.center().y + 5.0f * 30.0f};
    view.wheel(ins(900.0f, 330.0f), -2.0f, {});
    view.poll();
    const bool scrolledShot = renderer.save(view, dir / "5n-op-inputs-scrolled.png");
    drag(view, sixth, {sixth.x, sixth.y - 20.0f});
    const bool feedback = model.value(detail::routeParam(0, 0)) > 0.3 && model.value(detail::routeParam(kOps, 0)) == 0.5;
    view.undo();
    // The thumb, at the bar's foot now, dragged up to its top: the first row is B's again.
    const Point thumb{detail::kInspector.x + 300.0f - 9.0f, detail::kInspector.y + detail::kInspector.h - 20.0f};
    drag(view, thumb, {thumb.x, thumb.y - 200.0f});
    view.poll();
    drag(view, depth.center(), {depth.center().x, depth.center().y - 20.0f});
    const bool back = model.value(detail::routeParam(1, 0)) > 0.5;
    view.undo();
    // Dragged up 60 px with the middle button over the names, the rows follow: the feedback's is the sixth again.
    view.middleDown(ins(900.0f, 420.0f));
    view.mouseMove(ins(900.0f, 360.0f), {});
    view.middleUp(ins(900.0f, 360.0f));
    drag(view, sixth, {sixth.x, sixth.y - 20.0f});
    const bool panned = model.value(detail::routeParam(0, 0)) > 0.3 && model.value(detail::routeParam(kOps, 0)) == 0.5;
    check(scrolledShot && feedback && back && panned,
          "the INPUTS tab's rows scroll under the wheel, the middle button and their scrollbar's thumb");
}

void testRouteModes(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(paramIndex(2, OpParam::Enabled), 1.0);
    model.set(matrixIndex(1, 0), 0.5);
    model.ui.keptRoutes.push_back(matrixIndex(1, 0));
    model.ui.selectedNode = 0;
    view.poll();
    const UiLayout &layout = model.layout();
    const int mode = routeModeIndex(1, 0);
    // B -> A's label, in the middle of its wire as it bows to begin with (see testWireBend()), and the glyph left of it.
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1);
    const float span = std::hypot(a.x - b.x, a.y - b.y);
    const Point left{-(a.y - b.y) / span, (a.x - b.x) / span};
    const Point control{(a.x + b.x) * 0.5f + left.x * span * kDefaultWireBend,
                        (a.y + b.y) * 0.5f + left.y * span * kDefaultWireBend};
    auto toward = [&](Point from, float by) {
        const float d = std::hypot(control.x - from.x, control.y - from.y);
        return Point{from.x + (control.x - from.x) / d * by, from.y + (control.y - from.y) / d * by};
    };
    const Point start = toward(b, 24.0f), end = toward(a, 32.0f);
    const Point label{0.25f * start.x + 0.5f * control.x + 0.25f * end.x,
                      0.25f * start.y + 0.5f * control.y + 0.25f * end.y};
    // The pill's mode part, left of the label (38 px across) and 28 px across; its x, 20 px across, right of it; and
    // where a resting label shows a mode other than Phase, at its left.
    const Point modePart{label.x - 19.0f - 14.0f, label.y}, removePart{label.x + 19.0f + 10.0f, label.y};
    const Point glyph{label.x - 24.0f, label.y};
    auto pixelsAround = [&](Point at) {
        std::vector<uint32_t> pixels;
        for (float y = at.y - 5.0f; y <= at.y + 5.0f; y += 1.0f)
            for (float x = at.x - 5.0f; x <= at.x + 5.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    auto glyphPixels = [&] { return pixelsAround(glyph); };
    view.mouseMove({600.0f, 468.0f}, {});
    const bool plainShot = renderer.save(view, dir / "2k-route-phase.png");
    const std::vector<uint32_t> phaseAway = glyphPixels(), removeAway = pixelsAround(removePart);
    // Under the mouse, the label opens into its pill (read off the rendered pixels); a click on its mode part opens
    // the list of modes, Ring its last.
    view.mouseMove(label, {});
    const bool pillShot = renderer.save(view, dir / "2k2-route-pill.png");
    const bool opens = pixelsAround(removePart) != removeAway;
    view.mouseMove(modePart, {});
    click(view, modePart);
    const Point ringItem{modePart.x - 14.0f + 56.0f, label.y + 9.0f + 4.0f + 4.0f + 3 * 22.0f + 11.0f};
    view.mouseMove(ringItem, {});
    const bool listShot = renderer.save(view, dir / "2l-route-mode-list.png");
    click(view, ringItem);
    const bool ring = model.value(mode) == static_cast<double>(RouteMode::Ring);
    view.mouseMove({600.0f, 468.0f}, {});
    const bool ringShot = renderer.save(view, dir / "2m-route-ring.png");
    const bool shows = glyphPixels() != phaseAway;
    view.undo();
    const bool undone = model.value(mode) == 0.0;
    view.redo();
    check(plainShot && pillShot && opens && listShot && ring && ringShot && shows && undone && model.value(mode) == 3.0,
          "a wire label under the mouse opens into a pill whose mode part lists its route's modes, and resting shows a "
          "mode other than Phase (read off the rendered pixels)");
    // The wheel over the mode part steps the modes, a run of notches one undo step, and stops at the ends.
    view.mouseMove(label, {});
    view.mouseMove(modePart, {});
    view.wheel(modePart, -1.0f, {});
    const bool stepped = model.value(mode) == static_cast<double>(RouteMode::ExpFm);
    view.wheel(modePart, -1.0f, {});
    view.wheel(modePart, -1.0f, {});
    view.wheel(modePart, -1.0f, {});
    const bool bottom = model.value(mode) == 0.0;
    view.undo();
    check(stepped && bottom && model.value(mode) == static_cast<double>(RouteMode::Ring),
          "the wheel over a label's mode part steps its route's modes, undoably");

    // The label's right-click menu lists the modes, the one in force checked; picking Exp FM sets it.
    bool checked = false;
    for (const MenuItem &item : view.contextMenu(label)) {
        checked = checked || (item.label == routeModeName(RouteMode::Ring) && item.checked);
        if (item.label == routeModeName(RouteMode::ExpFm))
            view.menuCommand(item.command);
    }
    check(checked && model.value(mode) == static_cast<double>(RouteMode::ExpFm),
          "a wire label's menu lists its route's modes and picks one");

    // In the matrix, the cell's corner shows the glyph of a mode other than Phase (read off the rendered pixels), and
    // its menu picks Linear FM.
    model.ui.view = RoutingView::Matrix;
    view.poll();
    const Point cell = gridCellCenter(1, 0);
    const Point corner{cell.x - 14.3f + 7.0f, cell.y - 14.3f + 7.0f};
    auto cornerPixels = [&] {
        std::vector<uint32_t> pixels;
        for (float y = corner.y - 4.0f; y <= corner.y + 4.0f; y += 1.0f)
            for (float x = corner.x - 4.0f; x <= corner.x + 4.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    view.mouseMove({600.0f, 468.0f}, {});
    const bool matrixShot = renderer.save(view, dir / "6b-matrix-route-mode.png");
    const std::vector<uint32_t> expCorner = cornerPixels();
    for (const MenuItem &item : view.contextMenu(cell))
        if (item.label == routeModeName(RouteMode::LinearFm))
            view.menuCommand(item.command);
    const bool linear = model.value(mode) == static_cast<double>(RouteMode::LinearFm);
    model.set(mode, 0.0);
    view.poll();
    renderer.save(view, dir / "6c-matrix-route-phase.png");
    check(matrixShot && linear && cornerPixels() != expCorner,
          "a matrix cell shows its route's mode in its corner, and its menu picks one");

    // The INPUTS tab: a row per route into A, B's and its feedback's; B's buttons pick its mode, its label its depth.
    model.ui.view = RoutingView::Graph;
    model.set(matrixIndex(0, 0), 0.25);
    model.ui.keptRoutes.push_back(matrixIndex(0, 0));
    view.poll();
    click(view, opTabPoint(InspectorTab::Inputs));
    const bool tab = layout.opTab == InspectorTab::Inputs;
    const float row = detail::kInspector.y + 186.0f + 9.0f;
    auto modeButton = [&](RouteMode m) { return Point{detail::kInspector.x + 48.0f + 13.0f + static_cast<int>(m) * 26.0f, row}; };
    click(view, modeButton(RouteMode::Ring));
    const bool ringPicked = model.value(mode) == static_cast<double>(RouteMode::Ring);
    view.mouseMove(modeButton(RouteMode::ExpFm), {});
    const bool inputsShot = renderer.save(view, dir / "5k-op-inputs.png");
    const Point depth{detail::kInspector.x + 300.0f - 16.0f - 24.0f, row};
    drag(view, depth, {depth.x, depth.y - 20.0f});
    const bool deeper = model.value(matrixIndex(1, 0)) > 0.5;
    view.undo();
    view.undo();
    const bool undoneTwice = model.value(matrixIndex(1, 0)) == 0.5 && model.value(mode) == 0.0;
    view.redo();
    check(tab && ringPicked && inputsShot && deeper && undoneTwice && model.value(mode) == 3.0,
          "the INPUTS tab lists the routes into an operator, its buttons picking a route's mode and its label the depth");

    // B -> A's arrowhead dragged onto C: B -> C keeps its amount and Ring, and B -> A is Phase again.
    model.set(matrixIndex(0, 0), 0.0);
    view.poll();
    drag(view, arrowOf(b, a), nodeCenter(layout, 2));
    const bool moved = model.value(matrixIndex(1, 2)) == 0.5 && model.value(routeModeIndex(1, 2)) == 3.0 &&
                       model.value(mode) == 0.0 && model.value(matrixIndex(1, 0)) == 0.0;
    // Removed by its pill's x, a route goes, undoably, and comes back as Phase; an OUT label's x takes its node's
    // output away.
    model.set(matrixIndex(1, 0), 0.5);
    model.set(mode, 2.0);
    model.ui.keptRoutes.push_back(matrixIndex(1, 0));
    view.poll();
    view.mouseMove(label, {});
    view.mouseMove(removePart, {});
    const bool removeShot = renderer.save(view, dir / "2n-route-remove.png");
    click(view, removePart);
    const bool removed = model.value(matrixIndex(1, 0)) == 0.0 && model.value(mode) == 0.0;
    view.undo();
    const bool back = model.value(matrixIndex(1, 0)) == 0.5 && model.value(mode) == 2.0;
    const Point out = outLabelOf(layout, 0);
    view.mouseMove(out, {});
    view.mouseMove({out.x + 30.0f, out.y}, {});
    click(view, {out.x + 30.0f, out.y});
    check(moved && removeShot && removed && back && model.value(paramIndex(0, OpParam::Out)) == 0.0,
          "a route moved onto another operator keeps its mode; a label's x removes its route (back as Phase) and an "
          "OUT label's its node's output");
}

// The node the inspector shows is filled with its color and ringed in it in the graph, and picking another node moves
// both there (read off the rendered pixels); a switched-off node's ghost is filled while it is the one shown.
// A node's line to OUT bends as a wire does: straight to begin with, dragged sideways from a third of the way along
// (clear of its label in the middle) it bows that way, as one undo step, and a double-click on it straightens it.
void testOutBend(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    const UiLayout &layout = model.layout();
    const int out = paramIndex(0, OpParam::Out);
    const Point a = nodeCenter(layout, 0);
    auto along = [&](float bend, float t) {
        const detail::OutWire w = detail::outWireOf(0, a, bend);
        const float s = 1.0f - t;
        return Point{s * s * w.start.x + 2.0f * s * t * w.control.x + t * t * w.end.x,
                     s * s * w.start.y + 2.0f * s * t * w.control.y + t * t * w.end.y};
    };
    auto bendOf = [&] {
        for (const auto &[route, bend] : layout.wireBends)
            if (route == out)
                return bend;
        return 0.0f;
    };
    const detail::OutWire straight = detail::outWireOf(0, a);
    const Point grab = along(0.0f, 0.3f);
    const Point side{-straight.dir.y * 30.0f, straight.dir.x * 30.0f}; // to its right
    drag(view, grab, {grab.x + side.x, grab.y + side.y});
    const float bent = bendOf();
    view.mouseMove({600.0f, 600.0f}, {});
    const bool shot = renderer.save(view, dir / "1o-out-bent.png");
    view.undo();
    const bool undone = bendOf() == 0.0f;
    view.redo();
    doubleClick(view, along(bent, 0.3f));
    std::printf("      A's line to OUT bent to %.3f\n", bent);
    check(shot && bent != 0.0f && undone && bendOf() == 0.0f && model.value(out) == 1.0,
          "a node's line to OUT bends when dragged sideways, undoably, and a double-click straightens it");
}

// The routing graph's glow: a bloom from OUT, dithered rather than stepped (along a stretch of the panel's bottom, under
// the ghosts and clear of the rings and rays, neighboring pixels differ many times over, where a glow in steps would
// change only at its few edges); as bright and as far out as what the nodes send to OUT is loud (the effects' IN),
// rising to it over a few polls rather than at once; as strong as the glow setting says, and gone with it Off; and its
// level moving repainting the panel as a whole paint would (read off the rendered pixels, in Spectrum).
void testRadarGlow(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    model.themeName = "Spectrum";
    model.glowLevel = 0.75;
    View view(model);
    auto output = [&](float level) { // what the nodes send to OUT at `level` of full scale: the effects' IN, 12 dB down
        model.live.fxPortLevels[kFxPortIn] = 0.25f * level;
    };
    auto settle = [&] {
        for (int i = 0; i < 120; ++i)
            view.poll();
    };
    output(1.0f);
    settle();
    view.mouseMove({600.0f, 600.0f}, {});
    const bool shot = renderer.save(view, dir / "1n-radar-glow.png");
    const float y = detail::kRouting.bottom() - 6.0f;
    int changes = 0;
    uint32_t last = renderer.pixel({kOutCore.x - 260.0f, y});
    for (float x = kOutCore.x - 259.0f; x <= kOutCore.x - 160.0f; x += 1.0f) {
        const uint32_t p = renderer.pixel({x, y});
        changes += p != last ? 1 : 0;
        last = p;
    }
    std::printf("      the glow along 100 px of the panel's bottom: %d changes between neighbors\n", changes);
    check(shot && changes >= 20, "the routing graph's glow is dithered, not stepped (read off the rendered pixels)");

    // Its brightest channel by the core's left, and far out, past half the outermost ring's reach.
    const Point byCore{kOutCore.x - 70.0f, kOutCore.y + 60.0f}, farOut{kOutCore.x - 330.0f, kOutCore.y + 80.0f};
    auto brightness = [&](Point at) {
        const uint32_t p = renderer.pixel(at);
        return static_cast<int>(std::max({p >> 16 & 255, p >> 8 & 255, p & 255}));
    };
    auto shown = [&](const char *file) {
        view.mouseMove({600.0f, 600.0f}, {});
        renderer.save(view, dir / file);
        return std::pair{brightness(byCore), brightness(farOut)};
    };
    const auto [loud, loudFar] = shown("1n-radar-glow-loud.png");
    output(0.0f);
    settle();
    const auto [quiet, quietFar] = shown("1n-radar-glow-quiet.png");
    output(1.0f);
    view.poll(); // one poll in: on its way up
    const auto [rising, risingFar] = shown("1n-radar-glow-rising.png");
    settle();
    model.glowLevel = 0.3;
    view.poll();
    const auto [lower, lowerFar] = shown("1n-radar-glow-lower.png");
    model.glowLevel = 0.0;
    view.poll();
    const auto [off, offFar] = shown("1n-radar-glow-off.png");
    output(0.0f);
    settle();
    const auto [none, noneFar] = shown("1n-radar-glow-off.png");
    std::printf("      the glow by OUT: %d loud (%d far out), %d in silence (%d far out), %d one poll after it got loud, "
                "%d with the setting at 30 %%, %d with it Off\n",
                loud, loudFar, quiet, quietFar, rising, lower, off);
    // Its level moving repaints the panel (and the header, its meter), as a whole paint would.
    model.glowLevel = 0.75;
    view.poll();
    renderer.save(view, dir / "1n-radar-glow-repaint-before.png");
    output(0.8f);
    const bool moved = view.poll() && std::any_of(view.changedAreas().begin(), view.changedAreas().end(), [](Rect r) {
        return r.x == detail::kFxPanel.x && r.y == detail::kFxPanel.y && r.w == detail::kFxPanel.w;
    });
    bool repainted = true;
    for (const Rect &area : view.changedAreas())
        repainted = renderer.repaint(view, area) && repainted;
    const std::vector<uint32_t> part = renderer.image();
    const bool whole = renderer.save(view, dir / "1n-radar-glow-repaint.png") && renderer.image() == part;
    check(loud > quiet + 8 && loudFar > quietFar && quiet > none && rising > quiet && rising < loud && lower < loud &&
              off == none && moved && repainted && whole,
          "the routing graph's glow is as bright and as far out as the output is loud, rising to it over a moment, as "
          "strong as the glow setting says, and its level moving repaints the panel, as a whole paint would");
}

void testSelectedNode(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.ui.selectedNode = 0;
    view.poll();
    const UiLayout &layout = model.layout();
    view.mouseMove({600.0f, 468.0f}, {});
    // Inside a circle, left of its letter, and on its left 5 px out, where the ring goes.
    auto inside = [&](int node) {
        const Point c = nodeCenter(layout, node);
        return renderer.pixel({c.x - 14.0f, c.y});
    };
    auto ring = [&](int node) { // over its top, clear of the lens's leader; by its brightest channel
        const Point c = nodeCenter(layout, node);
        const uint32_t p = renderer.pixel({c.x, c.y - 24.0f - 5.0f});
        return std::max({p >> 16 & 255, p >> 8 & 255, p & 255});
    };
    const bool shotA = renderer.save(view, dir / "1g-selected-node.png");
    const uint32_t aIn = inside(0), bIn = inside(1), aRing = ring(0), bRing = ring(1);
    // The lens's leader, a dotted line in A's color level from A's left across to the lens: the brightest pixel along
    // that row, just right of the lens, while A is shown and once B is.
    auto leader = [&] {
        const float y = nodeCenter(layout, 0).y;
        unsigned most = 0;
        for (float x = detail::kInspector.right() + 2.0f; x < detail::kInspector.right() + 14.0f; x += 1.0f) {
            const uint32_t p = renderer.pixel({x, y});
            most = std::max({most, p >> 16 & 255, p >> 8 & 255, p & 255});
        }
        return most;
    };
    const unsigned leaderA = leader();
    click(view, nodeTab(1));
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shotB = renderer.save(view, dir / "1h-selected-node-b.png");
    const unsigned leaderB = leader();
    std::printf("      the lens's leader at A's height: %u with A shown, %u with B\n", leaderA, leaderB);
    check(leaderA > leaderB + 20, "a dotted leader runs from the node the inspector shows across to its lens (read off "
                                  "the rendered pixels)");
    // (Where its ring goes, an unshown node shows the backdrop, which differs from place to place.)
    const bool moved = inside(1) == aIn && inside(0) == bIn && ring(1) > ring(0) + 60;
    check(shotA && shotB && aIn != bIn && aRing > bRing + 60 && moved,
          "the node the inspector shows is filled and ringed in the graph, and picking another moves both (read off the "
          "rendered pixels)");
    // C and D are off, the first two ghosts docked under OUT: C's filled while it is the one shown.
    auto ghost = [&](int k) { return renderer.pixel({ghostOf(model, 2 + k).x - 7.0f, ghostOf(model, 2 + k).y}); };
    const bool plainShot = renderer.save(view, dir / "1i-ghosts.png");
    const uint32_t cPlain = ghost(0), dPlain = ghost(1);
    click(view, nodeTab(2));
    model.set(paramIndex(2, OpParam::Enabled), 0.0); // the tab switches it on; off again, it is a ghost still shown
    view.poll();
    view.mouseMove({600.0f, 468.0f}, {});
    const bool ghostShot = renderer.save(view, dir / "1j-selected-ghost.png");
    check(plainShot && ghostShot && layout.selectedNode == 2 && ghost(0) != cPlain && ghost(1) == dPlain,
          "a switched-off node's ghost is filled while the inspector shows it (read off the rendered pixels)");
}

// The MODS tab: every modulation as a row (LFO 1's eleven, Env 1's and Macro 1's, more than fit, so a bar scrolls
// them). A row's Amount bar, aux and its menu, target and source change it from the table, and the switch after a
// source's name its polarity; the menus cascade by category; and a source's handle dropped on a depth handle makes it
// that modulation's aux.
void testMods(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Env));
    const OpParam fillers[] = {OpParam::Detune,  OpParam::Transpose, OpParam::Out,     OpParam::Phase,
                               OpParam::Attack,  OpParam::Decay,     OpParam::Sustain, OpParam::Release,
                               OpParam::Hold,    OpParam::UnisonDetune};
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Ratio)));
    model.set(slotAmountIndex(0, 0), 0.4);
    model.set(slotScaleIndex(0, 0), 0.5);
    model.set(slotAuxIndex(0, 0), slotAuxValue(kMaxSources + 1)); // Macro 2
    for (int k = 0; k < 10; ++k) {
        model.set(slotTargetIndex(0, k + 1), slotTargetValue(paramIndex(0, fillers[k])));
        model.set(slotAmountIndex(0, k + 1), 0.2);
    }
    model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
    model.set(slotAmountIndex(1, 0), 0.3);
    model.set(slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, FilterParam::Resonance)));
    model.set(slotAmountIndex(kMaxSources, 0), 0.35);
    view.poll();
    // The tab, after FX; the first row's middle at y 154, its cells across from x 28 (see layout.h's kMods...).
    click(view, {314.0f, 116.0f});
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "35-mods.png");
    check(model.ui.view == RoutingView::Mods && shot, "the MODS tab shows every modulation as a row");
    const float y = 182.0f;
    auto slotIs = [&](int source, int slot, int target) {
        return slotTarget(model.value(slotTargetIndex(source, slot))) == target;
    };
    // Its Amount bar, 56 px wide from x 764, takes the click's place; the aux's x takes Macro 2 away; the switch after
    // LFO 1's name, from x 186, makes the LFO unipolar, and again bipolar.
    click(view, {764.0f + 0.75f * 56.0f, y});
    click(view, {1102.0f, y});
    click(view, {198.0f, y});
    const bool unipolar = model.value(paramIndex(0, SourceParam::Unipolar)) == 1.0;
    click(view, {198.0f, y});
    check(std::abs(model.value(slotScaleIndex(0, 0)) - 0.75) < 1e-6 && model.value(slotAuxIndex(0, 0)) == 0.0 &&
              model.value(slotScaleIndex(0, 1)) == 1.0 && unipolar &&
              model.value(paramIndex(0, SourceParam::Unipolar)) == 0.0,
          "a MODS row's Amount bar and aux x change its modulation alone, and the switch after its source's name the "
          "source's polarity");
    // The row's curve, 52 px wide from x 696: dragged up 50 px it bends to +50 %, in one undo step, drawn in its
    // source's color (read off the rendered pixels); dragged down past the bottom it stops at -100 %; a double-click
    // straightens it again.
    {
        const Point curve{722.0f, y};
        const int index = slotCurveIndex(0, 0);
        drag(view, curve, {curve.x, curve.y - 50.0f});
        const double up = model.value(index);
        view.mouseMove(curve, {});
        const bool bentShot = renderer.save(view, dir / "35t-mods-curve.png");
        bool colored = false; // LFO 1's blue somewhere along the bent line
        for (float px = 702.0f; px < 742.0f && !colored; px += 1.0f)
            for (float py = y - 9.0f; py <= y + 9.0f && !colored; py += 1.0f) {
                const uint32_t c = renderer.pixel({px, py});
                const int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                colored = b > 150 && b > r + 60 && g > r;
            }
        view.undo();
        const double undone = model.value(index);
        view.redo();
        drag(view, curve, {curve.x, curve.y + 400.0f});
        const double floor = model.value(index);
        doubleClick(view, curve);
        check(std::abs(up - 0.5) < 0.02 && undone == 0.0 && floor == -1.0 && model.value(index) == 0.0 && bentShot &&
                  colored && model.value(slotCurveIndex(0, 1)) == 0.0,
              "a MODS row's curve bends up and down by a drag, undoably, drawn in its source's color, and a "
              "double-click straightens it");
    }
    // + Aux opens None and the sources by category; over Envelopes (the third entry) Env 1 shows beside it, and
    // picking it makes it the aux.
    click(view, {950.0f, y});
    view.mouseMove({940.0f, 195.0f + 4.0f + 2.0f * 22.0f + 11.0f}, {});
    const bool auxShot = renderer.save(view, dir / "35e-mods-aux-menu.png");
    click(view, {1080.0f, 239.0f + 4.0f + 11.0f});
    check(auxShot && slotAuxSource(model.value(slotAuxIndex(0, 0))) == 1, "+ Aux's menu picks an aux from its category");
    // The target opens every target by category, under its search (30 px, the level 240 px wide from x 234):
    // Operators, then B, then (under PITCH, after Ratio and Pitch) Detune, three levels deep.
    click(view, {300.0f, y});
    view.mouseMove({300.0f, 228.0f + 4.0f + 22.0f + 11.0f}, {});       // Operators
    view.mouseMove({530.0f, 250.0f + 4.0f + 22.0f + 11.0f}, {});       // B
    const Point detune{650.0f, 272.0f + 4.0f + 20.0f + 2.0f * 22.0f + 11.0f};
    view.mouseMove(detune, {});
    const bool menuShot = renderer.save(view, dir / "35a-mods-target-menu.png");
    click(view, detune);
    const bool retargeted = slotIs(0, 0, paramIndex(1, OpParam::Detune)) &&
                            std::abs(model.value(slotAmountIndex(0, 0)) - 0.4) < 1e-9 &&
                            std::abs(model.value(slotScaleIndex(0, 0)) - 0.75) < 1e-6 &&
                            slotAuxSource(model.value(slotAuxIndex(0, 0))) == 1;
    view.undo();
    check(menuShot && retargeted && slotIs(0, 0, paramIndex(0, OpParam::Ratio)),
          "a row's target menu moves its modulation to another target, keeping the rest, undoably");
    // Its source moves it to a free slot of another source, Macro 1 here, with its depth, Amount and aux, in one undo
    // step.
    click(view, {90.0f, y});
    view.mouseMove({80.0f, 198.0f + 4.0f + 2.0f * 22.0f + 11.0f}, {}); // Macros
    view.mouseMove({200.0f, 242.0f + 4.0f + 11.0f}, {});              // Macro 1
    const bool sourceShot = renderer.save(view, dir / "35b-mods-source-menu.png");
    click(view, {200.0f, 242.0f + 4.0f + 11.0f});
    const bool moved = slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                       slotIs(kMaxSources, 1, paramIndex(0, OpParam::Ratio)) &&
                       std::abs(model.value(slotAmountIndex(kMaxSources, 1)) - 0.4) < 1e-9 &&
                       std::abs(model.value(slotScaleIndex(kMaxSources, 1)) - 0.75) < 1e-6 &&
                       slotAuxSource(model.value(slotAuxIndex(kMaxSources, 1))) == 1;
    view.undo();
    check(sourceShot && moved && slotIs(0, 0, paramIndex(0, OpParam::Ratio)) &&
              slotTarget(model.value(slotTargetIndex(kMaxSources, 1))) < 0,
          "a row's source menu moves its modulation to another source with its settings, undoably");
    // Thirteen rows: the wheel over them (not on a bar, which it turns) scrolls them, gliding a row a notch; two
    // notches down, the top row's x is LFO 1's third modulation's.
    view.wheel({300.0f, 328.0f}, -2.0f, {});
    for (int i = 0; i < 30; ++i)
        view.poll();
    view.mouseMove({600.0f, 468.0f}, {});
    const bool scrolledShot = renderer.save(view, dir / "35c-mods-scrolled.png");
    click(view, {1144.0f, y});
    const bool removed = slotTarget(model.value(slotTargetIndex(0, 2))) < 0 && slotIs(0, 1, paramIndex(0, fillers[0]));
    view.undo();
    check(scrolledShot && removed && slotIs(0, 2, paramIndex(0, fillers[1])),
          "the wheel scrolls the MODS view's rows, the scrolled-to row's controls under the mouse");
    // Scrolled to the end, Macro 1's row (the last of the thirteen, the add row after it) has a bypass dot like the
    // others: a click bypasses its modulation, and another lets it go again.
    view.wheel({300.0f, 328.0f}, -10.0f, {});
    for (int i = 0; i < 40; ++i)
        view.poll();
    // The rows are 30 px apart, 28 high, in a list 326 high whose first row's middle is at y 154: the fourteen (the
    // add row too) scroll as far as the first shown is row 3.1 (see View::modsEnd()).
    const float lastFirst = (14.0f * 30.0f - 2.0f - 326.0f) / 30.0f;
    const Point macroDot{41.0f, 182.0f + (12.0f - lastFirst) * 30.0f};
    click(view, macroDot);
    view.mouseMove({600.0f, 468.0f}, {});
    const bool macroBypassed = model.value(slotBypassIndex(kMaxSources, 0)) == 1.0;
    const bool macroBypassShot = renderer.save(view, dir / "35l-mods-macro-bypassed.png");
    click(view, macroDot);
    check(macroBypassed && macroBypassShot && model.value(slotBypassIndex(kMaxSources, 0)) == 0.0,
          "a macro's modulation has a bypass dot in the MODS view, like a source's");
    // Macro 2's handle dropped on the aux cell of Macro 1's row (the last, + Aux in it) makes Macro 2 its aux; its
    // own row's aux refuses it, and undo takes it away.
    const float auxStrip = view.height() - 179.0f;
    const Point auxCell{950.0f, macroDot.y};
    drag(view, {252.0f, auxStrip}, auxCell);
    const bool macroAux = slotAuxSource(model.value(slotAuxIndex(kMaxSources, 0))) == kMaxSources + 1;
    view.undo();
    drag(view, {156.0f, auxStrip}, auxCell); // Macro 1's own handle, 96 px before Macro 2's
    check(macroAux && slotAuxSource(model.value(slotAuxIndex(kMaxSources, 0))) < 0,
          "a source's handle dropped on a MODS row's aux cell becomes that modulation's aux, undoably, never its own");
    // Macro 2's handle dropped on LFO 1's depth handle along Op A's Ratio readout makes Macro 2 that modulation's aux.
    // Modulating nothing itself, Macro 2 isn't faint as Macro 3 is, which does nothing (read off the rendered
    // pixels, the macros' handles 96 px apart).
    click(view, {64.0f, 116.0f}); // back to ROUTING, with the inspector
    const float strip = view.height() - 179.0f;
    // The depth handle's menu offers to remove an aux only once it has one.
    const std::string removeAux = trf(Str::MenuRemoveAux, {nameOf(model, kMaxSources + 1)});
    auto auxEntry = [&] {
        for (const MenuItem &item : view.contextMenu(kRatioDot))
            if (item.label == removeAux)
                return item.command;
        return 0;
    };
    const bool noAuxEntry = auxEntry() == 0;
    drag(view, {252.0f, strip}, kRatioDot);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool ringShot = renderer.save(view, dir / "35d-aux-ring.png");
    const int auxRing = ringLevel(renderer, {252.0f + 6.0f, strip - 10.0f}, 20.0f);
    const int idleRing = ringLevel(renderer, {348.0f + 6.0f, strip - 10.0f}, 20.0f);
    check(ringShot && slotAuxSource(model.value(slotAuxIndex(0, 0))) == kMaxSources + 1 &&
              slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0 && auxRing > idleRing + 40,
          "a source's handle dropped on a depth handle becomes that modulation's aux, and lights as one used");
    // Right-clicked, the depth handle's menu takes the aux away, undoably.
    const int auxCommand = auxEntry();
    if (auxCommand != 0)
        view.menuCommand(auxCommand);
    const bool auxGone = slotAuxSource(model.value(slotAuxIndex(0, 0))) < 0;
    view.undo();
    check(noAuxEntry && auxCommand != 0 && auxGone &&
              slotAuxSource(model.value(slotAuxIndex(0, 0))) == kMaxSources + 1,
          "a depth handle's menu removes its aux, undoably, and offers it only with one");
    view.mouseMove({600.0f, 328.0f}, {});
    // Hovering that depth handle draws the dashed line to it from LFO 1's handle, on screen on the first card: half
    // way along the line, bowed to one side, the pixels change (read off the rendered pixels).
    const Point from = sourceHandle(0), to = kRatioDot;
    const Point d{to.x - from.x, to.y - from.y};
    const Point middle{(from.x + to.x) * 0.5f + d.y * 0.075f, (from.y + to.y) * 0.5f - d.x * 0.075f};
    auto aroundMiddle = [&] {
        std::vector<uint32_t> pixels;
        for (float dy = -4.0f; dy <= 4.0f; dy += 1.0f)
            for (float dx = -4.0f; dx <= 4.0f; dx += 1.0f)
                pixels.push_back(renderer.pixel({middle.x + dx, middle.y + dy}));
        return pixels;
    };
    const std::vector<uint32_t> plain = aroundMiddle();
    view.mouseMove(to, {});
    const bool linkShot = renderer.save(view, dir / "35i-depth-handle-link.png");
    const std::vector<uint32_t> shown = aroundMiddle();
    check(linkShot && shown != plain, "a hovered depth handle draws the line from its source's handle");
    // The settings' LINES draws it as strongly as it says: more at 100 %, and not at all when off.
    model.links = 1.0;
    const bool strongShot = renderer.save(view, dir / "35i1-link-strong.png");
    const bool strong = aroundMiddle() != shown && aroundMiddle() != plain;
    model.links = 0.0;
    const bool offShot = renderer.save(view, dir / "35i2-link-off.png");
    check(strongShot && offShot && strong && aroundMiddle() == plain,
          "a source's dashed line is drawn as strongly as LINES says, and not at all when off");
    model.links = 0.75;
    // Macro 2 is that modulation's aux: hovering its handle draws a dotted line to the depth handle, bowed the other
    // way from the dashed ones, and hovering the depth handle draws it from the aux's handle too.
    const Point auxFrom{252.0f, strip};
    const Point ad{to.x - auxFrom.x, to.y - auxFrom.y};
    const Point auxMiddle{(auxFrom.x + to.x) * 0.5f - ad.y * 0.075f, (auxFrom.y + to.y) * 0.5f + ad.x * 0.075f};
    auto aroundAux = [&] {
        std::vector<uint32_t> pixels;
        for (float dy = -4.0f; dy <= 4.0f; dy += 1.0f)
            for (float dx = -4.0f; dx <= 4.0f; dx += 1.0f)
                pixels.push_back(renderer.pixel({auxMiddle.x + dx, auxMiddle.y + dy}));
        return pixels;
    };
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "35j0-aux-plain.png"); // the last render had the depth handle hovered
    const std::vector<uint32_t> auxPlain = aroundAux();
    view.mouseMove(auxFrom, {});
    const bool auxLinkShot = renderer.save(view, dir / "35j-aux-link.png");
    const bool fromHandle = aroundAux() != auxPlain;
    view.mouseMove({600.0f, 328.0f}, {});
    view.mouseMove(to, {});
    renderer.save(view, dir / "35k-aux-link-from-depth-handle.png");
    check(auxLinkShot && fromHandle && aroundAux() != auxPlain,
          "an aux's handle hovered draws a dotted line to the depth handle it scales, and that handle hovered to it");
}

// A modulation moved to another source takes what modulates its depth along: LFO 1's on Op A's Ratio, its depth
// modulated by LFO 2, moved to Env 1 leaves LFO 2 modulating Env 1's depth; moved to Macro 1, whose depths can't be
// modulated, it takes LFO 2's modulation away with it. The menus open as in testMods(): the first row's source at
// x 90, y 154, its menu's categories 22 px apart from y 174 (LFOs, Envelopes, Macros), each opening beside it from x 176.
// A MODS row's depth bar marks where its modulation takes its target now, on the bar's scale: an LFO's from the
// newest voice while a note plays (and nowhere with none), a macro's from its knob, notes or not (read off the
// rendered pixels). The first row's bar runs 164 px from x 474, its middle at x 556, the next row 30 px down.
void testModsLive(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
    model.set(slotAmountIndex(0, 0), 0.5);
    model.set(slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, FilterParam::Resonance)));
    model.set(slotAmountIndex(kMaxSources, 0), 0.6);
    model.set(paramIndex(0, MacroParam::Bipolar), 0.0);
    model.set(paramIndex(0, MacroParam::Value), 0.0);
    view.poll();
    click(view, {314.0f, 116.0f}); // the MODS tab
    auto shoot = [&](const char *name, float y) {
        view.poll();
        view.mouseMove({600.0f, 468.0f}, {});
        renderer.save(view, dir / name);
        std::vector<uint32_t> pixels; // just above the bar, where only the tick reaches
        for (float x = 476.0f; x < 636.0f; x += 1.0f)
            pixels.push_back(renderer.pixel({x, y - 3.0f}));
        return pixels;
    };
    auto tickAt = [](const std::vector<uint32_t> &with, const std::vector<uint32_t> &without, float x) {
        const size_t i = static_cast<size_t>(x - 476.0f);
        return with[i] != without[i];
    };
    const float lfoRow = 182.0f, macroRow = 212.0f;
    model.live.voices = 0;
    model.live.slotOffset[0] = 0.25f;
    const std::vector<uint32_t> silent = shoot("35z-mods-live-none.png", lfoRow);
    model.live.voices = 1;
    const std::vector<uint32_t> up = shoot("35z-mods-live.png", lfoRow);
    model.live.slotOffset[0] = -0.25f;
    const std::vector<uint32_t> down = shoot("35z-mods-live-down.png", lfoRow);
    model.live.voices = 0;
    check(tickAt(up, silent, 556.0f + 20.5f) && tickAt(down, silent, 556.0f - 20.5f) &&
              !tickAt(up, silent, 556.0f - 20.5f) && !tickAt(down, silent, 556.0f + 20.5f),
          "a MODS row's depth bar marks where a playing note's LFO takes its target, and nothing with no note");
    // Macro 1 at 0 sits in the middle; at 0.5 its tick is 0.3 of the way, 24.6 px right.
    const std::vector<uint32_t> rest = shoot("35z-mods-live-macro0.png", macroRow);
    model.set(paramIndex(0, MacroParam::Value), 0.5);
    const std::vector<uint32_t> turned = shoot("35z-mods-live-macro.png", macroRow);
    check(tickAt(turned, rest, 556.0f + 24.6f) && tickAt(turned, rest, 556.0f),
          "a MODS row's depth bar marks where a macro takes its target, with no note playing");
}

// The mouse over a node of the routing graph shows an x in a circle at its upper right (read off the rendered pixels:
// none there before), whose click switches it off, undoably; a filter's box has one at its corner.
void testNodeDelete(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    UiLayout &layout = model.ui;
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(paramIndex(0, FilterParam::Enabled), 1.0);
    view.poll();
    const Point away{600.0f, 468.0f};
    auto removes = [&](int node, int enabled, Point x, const char *plainShot, const char *xShot) {
        view.mouseMove(away, {});
        renderer.save(view, dir / plainShot);
        const uint32_t plain = renderer.pixel({x.x + 7.5f, x.y});
        view.mouseMove(nodeCenter(layout, node), {});
        view.mouseMove(x, {});
        const bool shot = renderer.save(view, dir / xShot);
        const bool drawn = renderer.pixel({x.x + 7.5f, x.y}) != plain;
        click(view, x);
        const bool off = model.value(enabled) == 0.0;
        view.undo();
        view.mouseMove(away, {});
        view.poll();
        return shot && drawn && off && model.value(enabled) == 1.0;
    };
    const Point b = nodeCenter(layout, 1);
    check(removes(1, paramIndex(1, OpParam::Enabled), {b.x + 0.72f * 24.0f, b.y - 0.72f * 24.0f}, "1k-node-plain.png",
                  "1k-node-x.png"),
          "an operator under the mouse shows an x at its upper right, which switches it off, undoably");
    const Point f = nodeCenter(layout, kOps);
    check(removes(kOps, paramIndex(0, FilterParam::Enabled), {f.x + 33.0f - 2.0f, f.y - 17.0f + 2.0f},
                  "1l-filter-plain.png", "1l-filter-x.png"),
          "a filter under the mouse shows an x at its box's corner, which switches it off, undoably");
}

// A wire's label slides along its wire, off a node sitting at the wire's middle: A modulates B across the panel, C
// sitting where A -> B's label would be. The wheel over C's middle then turns nothing (on the label, which is drawn over
// the nodes, it would turn the route's amount); the snapshot shows where the label went.
void testLabelClearOfNode(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    UiLayout &layout = model.ui;
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(paramIndex(2, OpParam::Enabled), 1.0);
    const int route = matrixIndex(0, 1);
    model.set(route, 0.5);
    layout.nodeX[0] = 0.2f;
    layout.nodeX[1] = 0.8f;
    layout.nodeY[0] = layout.nodeY[1] = 0.35f;
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1);
    // The wire bowed as in view.cpp, its start 24 px off A toward the bow's control point and its end 32 px off B.
    const float span = std::hypot(b.x - a.x, b.y - a.y);
    const Point left{-(b.y - a.y) / span, (b.x - a.x) / span};
    const Point control{(a.x + b.x) * 0.5f + left.x * span * kDefaultWireBend,
                        (a.y + b.y) * 0.5f + left.y * span * kDefaultWireBend};
    auto toward = [&](Point from, float by) {
        const float d = std::hypot(control.x - from.x, control.y - from.y);
        return Point{from.x + (control.x - from.x) / d * by, from.y + (control.y - from.y) / d * by};
    };
    const Point start = toward(a, 24.0f), end = toward(b, 32.0f);
    const Point middle{0.25f * start.x + 0.5f * control.x + 0.25f * end.x,
                       0.25f * start.y + 0.5f * control.y + 0.25f * end.y};
    layout.nodeX[2] = (middle.x - detail::kGraph.x) / detail::kGraph.w;
    layout.nodeY[2] = (middle.y - detail::kGraph.y) / detail::kGraph.h;
    view.poll();
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "1m-label-clear-of-node.png");
    view.wheel(middle, 1.0f, {});
    check(shot && model.value(route) == 0.5, "a wire's label slides along its wire, clear of a node at its middle");
}

// Labels clear of each other: A -> B across the panel and C -> D down it, both straight, crossing at their middles.
// A -> B's label, placed first, stays at the crossing; C -> D's slides along its wire, off it. Where a label is is
// read off the right-click menu (a label's has Edit value, a wire's not), and whose by its Reset to default.
void testLabelsClearOfEachOther(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    UiLayout &layout = model.ui;
    for (int op = 1; op < 4; ++op)
        model.set(paramIndex(op, OpParam::Enabled), 1.0);
    const int across = matrixIndex(0, 1), down = matrixIndex(2, 3);
    model.set(across, 0.5);
    model.set(down, 0.3);
    layout.nodeX[0] = 0.2f;
    layout.nodeX[1] = 0.8f;
    layout.nodeY[0] = layout.nodeY[1] = 0.5f;
    layout.nodeX[2] = layout.nodeX[3] = 0.5f;
    layout.nodeY[2] = 0.05f;
    layout.nodeY[3] = 0.95f;
    layout.wireBends = {{across, 0.0f}, {down, 0.0f}};
    view.poll();
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1);
    const Point middle{0.5f * (a.x + b.x), a.y};
    const bool shot = renderer.save(view, dir / "1m-labels-clear.png");
    auto labelAt = [&](Point p) {
        for (const MenuItem &item : view.contextMenu(p))
            if (item.label == tr(Str::MenuEditValue))
                return true;
        return false;
    };
    const bool crossing = labelAt(middle);
    // C -> D's label, beyond A -> B's (9 px either way of its middle) and the gap between them.
    std::optional<Point> slid;
    for (float d = 20.0f; d <= 60.0f && !slid; d += 1.0f)
        for (const float side : {-1.0f, 1.0f})
            if (!slid && labelAt({middle.x, middle.y + side * d}))
                slid = Point{middle.x, middle.y + side * d};
    bool its = false;
    if (slid) {
        for (const MenuItem &item : view.contextMenu(*slid))
            if (item.label == tr(Str::MenuResetDefault)) {
                view.menuCommand(item.command);
                break;
            }
        its = model.value(across) == 0.5 && model.value(down) != 0.3;
    }
    check(shot && crossing && slid && its,
          "wire labels that would overlap slide apart along their wires, the first placed staying at its middle");
}

// A feedback loop's label moves off a wire's label: B feeds itself back, and A -> C runs straight above B, its label
// resting up and to the right of B's loop, where the loop's label would sit. The loop's label goes to the loop's left;
// the wire's stays at its middle. Labels are found and told apart by their right-click menus, as above.
void testLoopLabelClear(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    UiLayout &layout = model.ui;
    for (int op = 1; op < 3; ++op)
        model.set(paramIndex(op, OpParam::Enabled), 1.0);
    const int feedback = matrixIndex(1, 1), wire = matrixIndex(0, 2);
    model.set(feedback, 0.4);
    model.set(wire, 0.5);
    layout.nodeX[1] = 0.5f;
    layout.nodeY[1] = 0.6f;
    const Point b = nodeCenter(layout, 1);
    const Point middle{b.x + 52.0f, b.y - 47.0f}; // the wire's label, clear of B but over where the loop's would be
    layout.nodeX[0] = (middle.x - 200.0f - detail::kGraph.x) / detail::kGraph.w;
    layout.nodeX[2] = (middle.x + 200.0f - detail::kGraph.x) / detail::kGraph.w;
    layout.nodeY[0] = layout.nodeY[2] = (middle.y - detail::kGraph.y) / detail::kGraph.h;
    layout.wireBends = {{wire, 0.0f}};
    view.poll();
    const bool shot = renderer.save(view, dir / "1m-loop-label-clear.png");
    auto reset = [&](Point p) { // Reset to default on the label there, if there is one
        for (const MenuItem &item : view.contextMenu(p))
            if (item.label == tr(Str::MenuResetDefault)) {
                view.menuCommand(item.command);
                return;
            }
    };
    const Point loop{b.x, b.y - 24.0f - 9.0f}; // on B's outline, 24 px out
    reset({loop.x - 32.0f, loop.y});
    const bool left = model.value(feedback) != 0.4 && model.value(wire) == 0.5;
    reset({middle.x + 5.0f, middle.y});
    const bool stays = model.value(wire) != 0.5;
    check(shot && left && stays, "a feedback loop's label moves to the loop's left, off a wire's label on its right");
}

// A node switched on where another one shown sits comes back clear of it, in the same undo step as the switch; one
// whose place is free stays there. B was left on top of A; a filter in the middle of A, B and E too.
void testNodeSwitchOnPlace(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    UiLayout &layout = model.ui;
    layout.nodeX[1] = layout.nodeX[0];
    layout.nodeY[1] = layout.nodeY[0];
    const Point a = nodeCenter(layout, 0);
    view.poll();
    click(view, nodeTab(1));
    const Point b = nodeCenter(layout, 1);
    const float apart = std::hypot(b.x - a.x, b.y - a.y);
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "1f-op-on-clear.png");
    view.undo();
    const bool undone = model.value(paramIndex(1, OpParam::Enabled)) == 0.0 &&
                        layout.nodeX[1] == layout.nodeX[0] && layout.nodeY[1] == layout.nodeY[0];
    view.redo();
    std::printf("      B switched on over A lands %.1f px from it\n", apart);
    check(shot && model.value(paramIndex(1, OpParam::Enabled)) == 1.0 && apart >= 2.0f * 24.0f + 9.0f && undone,
          "an operator switched on over another one shown comes back clear of it, undoably");
    // E's place is free: it stays where it was.
    const float ex = layout.nodeX[4], ey = layout.nodeY[4];
    click(view, nodeTab(4));
    check(model.value(paramIndex(4, OpParam::Enabled)) == 1.0 && layout.nodeX[4] == ex && layout.nodeY[4] == ey,
          "a node switched on where nothing else is stays where it was");
    // Filter 1 put on A, switched on with its tab, goes clear of A, B and E.
    layout.boxX[0] = layout.nodeX[0];
    layout.boxY[0] = layout.nodeY[0];
    click(view, nodeTab(kOps));
    const Point f = nodeCenter(layout, kOps);
    bool clear = model.value(paramIndex(0, FilterParam::Enabled)) == 1.0;
    for (int op : {0, 1, 4}) { // a circle against the filter's box, 33 px by 17 px from its middle
        const Point c = nodeCenter(layout, op);
        const float dx = std::max(0.0f, std::abs(c.x - f.x) - 33.0f), dy = std::max(0.0f, std::abs(c.y - f.y) - 17.0f);
        clear = clear && std::hypot(dx, dy) >= 24.0f;
    }
    check(clear, "a filter switched on over operators comes back clear of them");
}

void testModsMoveDepth(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(2, SourceParam::Type), static_cast<double>(SourceType::Env));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Ratio)));
    model.set(slotAmountIndex(0, 0), 0.4);
    model.set(slotTargetIndex(1, 0), slotTargetValue(slotAmountIndex(0, 0)));
    model.set(slotAmountIndex(1, 0), 0.25);
    view.poll();
    click(view, {314.0f, 116.0f}); // MODS
    auto moveTo = [&](int category) {
        const float y = 202.0f + 22.0f * static_cast<float>(category) + 11.0f;
        click(view, {90.0f, 182.0f});
        view.mouseMove({80.0f, y}, {});
        view.mouseMove({200.0f, y}, {}); // the category's first source
        click(view, {200.0f, y});
    };
    moveTo(1); // Env 1
    const bool followed = slotTarget(model.value(slotTargetIndex(2, 0))) == paramIndex(0, OpParam::Ratio) &&
                          slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                          slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(2, 0) &&
                          std::abs(model.value(slotAmountIndex(1, 0)) - 0.25) < 1e-9;
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "35m-mods-moved-with-depth.png");
    view.undo();
    const bool back = slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Ratio) &&
                      slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(0, 0) &&
                      slotTarget(model.value(slotTargetIndex(2, 0))) < 0;
    check(followed && shot && back,
          "a modulation moved to another source takes what modulates its depth along, in one undo step");
    moveTo(2); // Macro 1
    check(slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == paramIndex(0, OpParam::Ratio) &&
              slotTarget(model.value(slotTargetIndex(1, 0))) < 0,
          "a modulation moved to a macro takes away what modulated its depth");
    // Removed by its row's x (at x 1144), it takes what modulated its depth with it, in one undo step.
    view.undo();
    click(view, {1144.0f, 182.0f});
    const bool removed = slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                         slotTarget(model.value(slotTargetIndex(1, 0))) < 0;
    view.undo();
    check(removed && slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Ratio) &&
              slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(0, 0),
          "a removed modulation takes what modulated its depth with it, in one undo step");
}

// A Follower source from + SOURCE's list (after Envelope): its card holds its input's list button and Peak | RMS on
// the top row, Attack, Release, Gain and Low Cut in a row under them and its trace across the card (see
// detail::followerInputButton() and on). It starts on OUT at 5 ms and 150 ms. Its list picks C and Filter 1, undoably;
// RMS and Peak set its detector; the wheel turns Gain and Low Cut, a double-click putting Low Cut back to Off. Its
// trace moves with what the newest voice gets from it, repainting its card alone, as a whole paint would. Folded, it
// reads FLW 1; the MODS view and the host call it Follower 1.
void testFollowerSource(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, detail::sourceMenuButton().center());
    view.mouseMove(sourceMenuEntry(SourceType::Follower), {});
    const bool listShot = renderer.save(view, dir / "11p-source-menu-follower.png");
    click(view, sourceMenuEntry(SourceType::Follower));
    const bool added = model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Follower) &&
                       model.value(paramIndex(0, SourceParam::FollowInput)) == static_cast<double>(FollowInput::Out) &&
                       model.value(paramIndex(0, SourceParam::Attack)) == normFromEnvTime(0.005) &&
                       model.value(paramIndex(0, SourceParam::Release)) == normFromEnvTime(0.15);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool cardShot = renderer.save(view, dir / "11q-follower-card.png");
    check(listShot && added && cardShot, "+ SOURCE adds a Follower on OUT, answering in 5 ms and letting go over 150 ms");

    const Rect card = detail::sourceCard(0.0f);
    const Rect button = detail::followerInputButton(card);
    const int input = paramIndex(0, SourceParam::FollowInput);
    const int count = static_cast<int>(FollowInput::Count);
    auto entry = [&](FollowInput which) {
        return detail::listMenuItem(button, count, static_cast<int>(which)).center();
    };
    click(view, button.center());
    view.mouseMove(entry(FollowInput::OpC), {});
    const bool inputListShot = renderer.save(view, dir / "11r-follower-input-list.png");
    click(view, entry(FollowInput::OpC));
    const bool onC = model.value(input) == static_cast<double>(FollowInput::OpC);
    click(view, button.center());
    click(view, entry(FollowInput::Filter1));
    const bool onFilter = model.value(input) == static_cast<double>(FollowInput::Filter1);
    view.undo();
    const bool undone = model.value(input) == static_cast<double>(FollowInput::OpC);
    view.undo();
    check(inputListShot && onC && onFilter && undone && model.value(input) == static_cast<double>(FollowInput::Out),
          "a Follower card's input list picks an operator and a filter, undoably");

    const int rms = paramIndex(0, SourceParam::FollowRms);
    click(view, detail::followerModeButton(card, 1).center());
    const bool onRms = model.value(rms) == 1.0;
    click(view, detail::followerModeButton(card, 0).center());
    check(onRms && model.value(rms) == 0.0, "a Follower card's Peak | RMS sets its detector");

    const int gain = paramIndex(0, SourceParam::FollowGain), lowCut = paramIndex(0, SourceParam::FollowLowCut);
    view.wheel(detail::followerKnob(card, 2), 3.0f, {});
    view.wheel(detail::followerKnob(card, 3), 3.0f, {});
    const bool turned = model.value(gain) > 0.0 && model.value(lowCut) > 0.0;
    doubleClick(view, detail::followerKnob(card, 3));
    check(turned && model.value(lowCut) == 0.0,
          "a Follower card's Gain and Low Cut knobs turn, and a double-click puts Low Cut back to Off");

    // Its trace, as the newest voice's level goes up: drawn higher across the display, repainting the card alone.
    auto sameArea = [](Rect a, Rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; };
    const Rect display = detail::followerDisplay(card);
    const Point high{display.right() - 8.0f, display.y + 8.0f};
    model.live.voices = 1;
    model.live.sources[0] = 0.0f;
    view.mouseMove({600.0f, 328.0f}, {});
    for (int i = 0; i < 80; ++i) // a trace along the bottom
        view.poll();
    renderer.save(view, dir / "11s-follower-trace-quiet.png");
    const uint32_t quiet = renderer.pixel(high);
    model.live.sources[0] = 0.9f;
    const bool polled = view.poll();
    const bool alone = view.changedAreas().size() == 1 && sameArea(view.changedAreas()[0], card);
    bool repainted = true;
    for (const Rect &area : view.changedAreas())
        repainted = renderer.repaint(view, area) && repainted;
    const std::vector<uint32_t> part = renderer.image();
    const bool traceShot = renderer.save(view, dir / "11t-follower-trace.png");
    const std::vector<uint32_t> full = renderer.image();
    const uint32_t loud = renderer.pixel(high);
    std::printf("follower trace: top right %06x -> %06x\n", quiet, loud);
    check(polled && alone && repainted && traceShot && part == full && loud != quiet,
          "a Follower's trace follows the newest voice, repainting its card alone as a whole paint would");
    model.live = Telemetry{};
    view.poll();

    auto get = [&](int index) { return model.value(index); };
    const bool named = gui::sourceName(model.layout(), get, 0) == trf(Str::SourceNameFollower, {"1"});
    click(view, detail::cardFold(card).center());
    view.mouseMove({600.0f, 328.0f}, {});
    const bool foldShot = renderer.save(view, dir / "11u-follower-folded.png");
    check(named && foldShot, "a Follower is named Follower 1, and folds to a strip");
}

// Modulations added in the MODS view: the row after the last adds an empty one and opens its source's menu; picking
// a source leaves it empty, waiting for a target, and picking a target then makes it a modulation of that source,
// each an undo step. The target menu's Global offers no Mod Wheel. An empty row's x removes it. With no
// modulations at all, the columns' names show above the button.
// + SOURCE in the title row opens the list of source types under it (the count of places taken beside it), which
// adds a Random and a MIDI source, each a card of its own: Random as a random LFO's,
// MIDI with its input's list button (where an LFO's shape is: from x 16 of the card, 122 px wide, at y 536 + 42) and
// right of it its controller (at x 172 of the card, turned up a notch by the wheel, while its input is a controller)
// and Slew (at x 228). The list opens under the button, an entry 22 px from y 604 + 4: CC, Pressure, Slide.
void testNewSources(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, detail::sourceMenuButton().center());
    view.mouseMove(sourceMenuEntry(SourceType::Random), {});
    const bool menuButtonShot = renderer.save(view, dir / "11d-source-menu-button.png");
    click(view, sourceMenuEntry(SourceType::Random));
    addSource(view, SourceType::Midi);
    addSource(view, SourceType::Midi);
    auto typeOf = [&](int place) {
        return static_cast<SourceType>(std::lround(model.value(paramIndex(place, SourceParam::Type))));
    };
    const bool added = typeOf(0) == SourceType::Random && typeOf(1) == SourceType::Midi && typeOf(2) == SourceType::Midi;
    const Point ccKnob{cardX(1) + 172.0f, 618.0f}, inputButton{cardX(2) + 16.0f + 61.0f, 564.0f + 42.0f + 11.0f};
    auto entry = [](float card, MidiInput input) {
        return Point{cardX(card) + 60.0f, 632.0f + 4.0f + 22.0f * static_cast<float>(input) + 11.0f};
    };
    view.wheel(ccKnob, 1.0f, {});
    // The button's tooltip says what its input follows (read off the rendered pixels along y 618, under it, against
    // the same view with the mouse away; the wording is in the snapshots).
    auto underButton = [&](const char *name) {
        std::vector<uint32_t> pixels;
        if (renderer.save(view, dir / name))
            for (float x = cardX(2); x <= cardX(2) + 240.0f; x += 2.0f)
                pixels.push_back(renderer.pixel({x, 646.0f}));
        return pixels;
    };
    view.mouseMove({600.0f, 328.0f}, {});
    const std::vector<uint32_t> away = underButton("11f-midi-input-away.png");
    view.mouseMove(inputButton, {});
    const std::vector<uint32_t> ccTip = underButton("11g-midi-cc-tooltip.png");
    click(view, inputButton);
    view.mouseMove(entry(2, MidiInput::Slide), {});
    const bool listShot = renderer.save(view, dir / "11i-midi-input-list.png");
    click(view, entry(2, MidiInput::Pressure));
    const bool onPressure = model.value(paramIndex(2, SourceParam::MidiInput)) == static_cast<double>(MidiInput::Pressure);
    view.mouseMove(inputButton, {});
    const std::vector<uint32_t> pressureTip = underButton("11h-midi-pressure-tooltip.png");
    view.wheel({cardX(2) + 172.0f, 618.0f}, 1.0f, {}); // no CC knob there now
    click(view, inputButton);
    click(view, entry(2, MidiInput::Slide));
    const bool onSlide = model.value(paramIndex(2, SourceParam::MidiInput)) == static_cast<double>(MidiInput::Slide);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool shot = renderer.save(view, dir / "11e-new-sources.png");
    check(ccTip != away && pressureTip != away && ccTip != pressureTip,
          "a MIDI card's input button shows a tooltip saying what its input follows");
    check(added && menuButtonShot && shot && listShot && model.value(paramIndex(1, SourceParam::CcNumber)) == 2.0 &&
              onPressure && onSlide && model.value(paramIndex(2, SourceParam::CcNumber)) == 1.0,
          "+ SOURCE's list adds Random and MIDI sources, a MIDI card's list picking pressure or slide over a controller");

    // A Random card's Unipolar | Bipolar, where an LFO's shape list is (from x 16 of the card, 122 px wide, at y 564 +
    // 42, 22 px high), each half of it: bipolar to begin with, Unipolar lit by a click (read off the rendered pixels,
    // clear of its icon), Bipolar again by the other; each an undo step.
    {
        auto polarity = [](int bipolar) {
            return Point{cardX(0) + 16.0f + 61.0f * (static_cast<float>(bipolar) + 0.5f), 564.0f + 42.0f + 11.0f};
        };
        const int bipolarParam = paramIndex(0, SourceParam::Bipolar);
        const Point clearOfIcon{polarity(0).x - 20.0f, polarity(0).y + 6.0f};
        const bool startsBipolar = model.value(bipolarParam) == 1.0;
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "11j-random-bipolar.png");
        const uint32_t unlit = renderer.pixel(clearOfIcon);
        click(view, polarity(0));
        const bool unipolar = model.value(bipolarParam) == 0.0;
        view.mouseMove({600.0f, 328.0f}, {});
        const bool polarShot = renderer.save(view, dir / "11j-random-unipolar.png");
        const bool lit = renderer.pixel(clearOfIcon) != unlit;
        click(view, polarity(1));
        const bool bipolar = model.value(bipolarParam) == 1.0;
        view.undo();
        check(startsBipolar && unipolar && polarShot && lit && bipolar && model.value(bipolarParam) == 0.0,
              "a Random card's Unipolar | Bipolar switch sets its polarity, undoably");
    }

    // A Curve source, an LFO of a drawn shape, from the list (second, after LFO): unipolar to begin with, its card
    // widened to draw its shape there (its power switch where a widened card's is), and its Unipolar | Bipolar where an
    // LFO's shape list is, setting its Unipolar, undoably.
    {
        FakeController fresh;
        View curveView(fresh);
        click(curveView, detail::sourceMenuButton().center());
        curveView.mouseMove(sourceMenuEntry(SourceType::Curve), {});
        const bool listShot = renderer.save(curveView, dir / "11k-source-menu-curve.png");
        click(curveView, sourceMenuEntry(SourceType::Curve));
        const int unipolarParam = paramIndex(0, SourceParam::Unipolar);
        const bool added = fresh.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Curve) &&
                           fresh.value(unipolarParam) == 1.0;
        const Point widePower = detail::sourcePower(0, detail::kWideSource).center();
        click(curveView, widePower);
        const bool wide = fresh.value(sourceEnabledIndex(0)) == 0.0;
        click(curveView, widePower);
        curveView.mouseMove({600.0f, 328.0f}, {});
        const bool shot = renderer.save(curveView, dir / "11l-curve-source.png");
        click(curveView, {cardX(0) + 16.0f + 61.0f * 1.5f, 564.0f + 42.0f + 11.0f}); // Bipolar
        const bool bipolar = fresh.value(unipolarParam) == 0.0;
        curveView.undo();
        check(listShot && added && wide && shot && bipolar && fresh.value(unipolarParam) == 1.0,
              "+ SOURCE adds a Curve source, unipolar and widened to draw its shape, its polarity set by its switch");
        // Folded, its name down the strip is CRV and its number, four lines and nothing after them (read off the
        // rendered pixels), as a Random card's RND.
        click(curveView, detail::cardFold(detail::sourceCard(0, detail::kWideSource)).center());
        curveView.mouseMove({600.0f, 328.0f}, {});
        const bool foldShot = renderer.save(curveView, dir / "11m-curve-folded.png");
        const float strip = cardX(0), top = 564.0f;
        auto inked = [&](int line) {
            const uint32_t card = renderer.pixel({strip + 4.0f, top + 150.0f});
            for (float y = top + 104.0f + line * 13.0f + 1.0f; y < top + 104.0f + (line + 1) * 13.0f; y += 1.0f)
                for (float x = strip + 8.0f; x < strip + 40.0f; x += 1.0f)
                    if (renderer.pixel({x, y}) != card)
                        return true;
            return false;
        };
        check(foldShot && inked(0) && inked(1) && inked(2) && !inked(3) && inked(4) && !inked(5),
              "a folded Curve card shows CRV and its number, which fit the strip");
    }
}

void testModsEmpty(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    // The columns' names, their middle at y 126: along it, under SOURCE (from x 54), the text's pixels differ from
    // the panel's (read off the rendered pixels).
    click(view, {314.0f, 116.0f}); // the MODS tab
    view.mouseMove({600.0f, 468.0f}, {});
    const bool noneShot = renderer.save(view, dir / "35e2-mods-none.png");
    bool headed = false;
    for (float x = 54.0f; x < 110.0f && !headed; x += 1.0f)
        headed = renderer.pixel({x, 154.0f}) != renderer.pixel({48.0f, 154.0f});
    check(noneShot && headed, "the MODS view shows its columns' names with no modulations");
    click(view, kRoutingTab);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Ratio)));
    model.set(slotAmountIndex(0, 0), 0.4);
    view.poll();
    click(view, {314.0f, 116.0f}); // the MODS tab
    // Under LFO 1's row (its middle at y 154), the button adds a second: its source's menu opens under its source,
    // at x 54 and y 200. Over Macros, the second entry, the macros show beside it; Macro 1 is picked.
    const float y = 212.0f;
    click(view, {100.0f, y});
    const bool added = model.ui.emptyMods == std::vector<std::pair<int, int>>{{-1, -1}};
    view.mouseMove({80.0f, 228.0f + 4.0f + 22.0f + 11.0f}, {}); // Macros
    view.mouseMove({230.0f, 228.0f + 4.0f + 22.0f + 11.0f}, {}); // Macro 1, beside it
    const bool menuShot = renderer.save(view, dir / "35f-mods-empty-source.png");
    click(view, {230.0f, 228.0f + 4.0f + 22.0f + 11.0f});
    const bool sourced = model.ui.emptyMods == std::vector<std::pair<int, int>>{{kMaxSources, -1}};
    view.mouseMove({600.0f, 468.0f}, {});
    const bool shot = renderer.save(view, dir / "35g-mods-empty.png");
    check(added && menuShot && sourced && shot && slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) < 0,
          "the MODS view's last row adds an empty modulation, which takes a source and modulates nothing yet");
    // Its target opens the targets under it, at x 234, under the menu's search (30 px): Global, the first category,
    // lists its params beside it from x 476, but not the mod wheel or the pitch bend; where it would come after them, a
    // click only closes the menu.
    std::vector<int> globals;
    for (int g = 0; g < kGlobalCount; ++g)
        if (isModTarget(g) && !isPlayed(g))
            globals.push_back(g);
    auto openGlobal = [&] {
        click(view, {300.0f, y});
        view.mouseMove({300.0f, 258.0f + 4.0f + 11.0f}, {});
    };
    auto globalEntry = [&](size_t k) { return Point{540.0f, 258.0f + 4.0f + 22.0f * k + 11.0f}; };
    openGlobal();
    const bool globalShot = renderer.save(view, dir / "35h-mods-global-targets.png");
    click(view, globalEntry(globals.size()));
    const bool noWheel = model.ui.emptyMods == std::vector<std::pair<int, int>>{{kMaxSources, -1}};
    openGlobal();
    click(view, globalEntry(0));
    const bool filled = model.ui.emptyMods.empty() && slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == globals[0];
    view.undo();
    const bool backEmpty = model.ui.emptyMods == std::vector<std::pair<int, int>>{{kMaxSources, -1}} &&
                           slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) < 0;
    view.undo();
    const bool backUnsourced = model.ui.emptyMods == std::vector<std::pair<int, int>>{{-1, -1}};
    check(globalShot && noWheel && filled && backEmpty && backUnsourced,
          "an empty modulation given a target becomes a modulation of its source, undoably; Global has no Mod Wheel");
    // Its x, at the row's right end, removes it.
    click(view, {1144.0f, y});
    check(model.ui.emptyMods.empty(), "an empty modulation's x removes it");
    // A card's button, its source modulating nothing (Env 1, on the second card), adds an empty modulation from it.
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Env));
    view.poll();
    click(view, {cardX(1) + 60.0f, 736.0f});
    view.mouseMove({600.0f, 468.0f}, {});
    const bool cardShot = renderer.save(view, dir / "35j-mods-from-card.png");
    check(cardShot && model.ui.view == RoutingView::Mods &&
              model.ui.emptyMods == std::vector<std::pair<int, int>>{{1, -1}},
          "a card's button, its source modulating nothing, adds an empty modulation from it in MODS");
    // With LFO 1's row and then Env 1's, a new empty modulation given LFO 1 moves up among LFO 1's rows: its target,
    // picked in the second row (its middle at y 184), makes it LFO 1's, Env 1's left as it was.
    click(view, {600.0f, 468.0f}); // closes the target menu the card's button opened
    model.ui.emptyMods.clear();
    model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(1, OpParam::Ratio)));
    view.poll();
    click(view, {100.0f, 242.0f}); // the button, under Env 1's row, opens the source menu at y 230
    view.mouseMove({80.0f, 258.0f + 4.0f + 11.0f}, {}); // LFOs
    // The menu's first entry under the mouse leaves LFO 1's row (its source's and slot's numbers the entry's) unlit:
    // it matches Env 1's between its aux and its x (read off the rendered pixels).
    const bool overRowsShot = renderer.save(view, dir / "35n-mods-menu-over-rows.png");
    const bool rowUnlit = renderer.pixel({1110.0f, 182.0f}) == renderer.pixel({1110.0f, 212.0f});
    check(overRowsShot && rowUnlit, "an open menu's entry under the mouse lights no MODS row");
    click(view, {230.0f, 258.0f + 4.0f + 11.0f});     // LFO 1, beside it
    const bool lfoSourced = model.ui.emptyMods == std::vector<std::pair<int, int>>{{0, -1}};
    view.mouseMove({600.0f, 468.0f}, {});
    const bool sortedShot = renderer.save(view, dir / "35k-mods-empty-sorted.png");
    openGlobal();
    click(view, globalEntry(0));
    check(lfoSourced && sortedShot && model.ui.emptyMods.empty() &&
              slotTarget(model.value(slotTargetIndex(0, 1))) == globals[0] &&
              slotTarget(model.value(slotTargetIndex(1, 0))) == paramIndex(1, OpParam::Ratio),
          "an empty modulation given a source with rows goes among them");
}

// The MODS view's filter and the target menu's search. LFO 1 modulates Op A's Ratio and Detune, Env 1 FM Amount and
// Macro 1 Filter 1's Resonance: four rows, their middles from y 154, 30 px apart, each one's x at x 1144. The filter's
// magnifier is at the right end of the columns' names (x 1156, y 126); open, its x is at x 1155.
void testModsSearch(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Env));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Ratio)));
    model.set(slotTargetIndex(0, 1), slotTargetValue(paramIndex(0, OpParam::Detune)));
    model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
    model.set(slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, FilterParam::Resonance)));
    for (auto [source, slot] : {std::pair{0, 0}, {0, 1}, {1, 0}, {kMaxSources, 0}})
        model.set(slotAmountIndex(source, slot), 0.3);
    view.poll();
    click(view, {314.0f, 116.0f}); // the MODS tab
    auto slotIs = [&](int source, int slot, int target) {
        return slotTarget(model.value(slotTargetIndex(source, slot))) == target;
    };
    const Point magnifier{1156.0f, 154.0f}, firstX{1144.0f, 182.0f};
    // The magnifier opens the filter, which takes the keyboard; "fm" leaves Env 1's row alone, first, and a click on
    // its x takes the keyboard back, the filter keeping its words.
    const bool idle = !view.typing();
    click(view, magnifier);
    const bool takesKeys = view.typing();
    type(view, "fm");
    view.mouseMove({600.0f, 468.0f}, {});
    const bool filterShot = renderer.save(view, dir / "35q-mods-filter.png");
    click(view, firstX);
    const bool removedEnv = slotTarget(model.value(slotTargetIndex(1, 0))) < 0 && slotIs(0, 0, paramIndex(0, OpParam::Ratio));
    const bool letGo = !view.typing();
    view.undo();
    check(idle && takesKeys && filterShot && removedEnv && letGo && slotIs(1, 0, paramIndex(GlobalParam::FmAmount)),
          "the MODS view's filter shows the rows whose source and target hold what is typed, and lets the keyboard "
          "go at a click elsewhere");
    // Every word counts, in any case: "LFO det" leaves LFO 1's Detune row alone. Its x empties it and keeps the
    // keyboard; Escape then lets it go.
    click(view, {1000.0f, 154.0f}); // in the open field, from x 968
    view.typeKey(View::Key::SelectAll);
    type(view, "LFO det");
    click(view, firstX);
    const bool removedDetune = slotTarget(model.value(slotTargetIndex(0, 1))) < 0 && slotIs(0, 0, paramIndex(0, OpParam::Ratio));
    view.undo();
    click(view, {1155.0f, 154.0f}); // the filter's x
    const bool emptied = view.typing() && view.copyText(false).empty();
    view.typeKey(View::Key::Escape);
    click(view, firstX); // the filter empty, the first row is LFO 1's Ratio again
    const bool all = slotTarget(model.value(slotTargetIndex(0, 0))) < 0;
    view.undo();
    check(removedDetune && emptied && !view.typing() && all && slotIs(0, 0, paramIndex(0, OpParam::Ratio)),
          "the MODS view's filter takes every word typed, its x empties it and Escape lets the keyboard go");
    // An operator's targets show in two columns, 140 px each: under Operators, then B, ENVELOPE heads the second,
    // its Attack beside the pitch's (the level from x 598, under the menu's 30 px search).
    click(view, {300.0f, 182.0f});
    view.mouseMove({300.0f, 228.0f + 4.0f + 22.0f + 11.0f}, {}); // Operators
    view.mouseMove({530.0f, 250.0f + 4.0f + 22.0f + 11.0f}, {}); // B
    const Point attack{800.0f, 272.0f + 4.0f + 20.0f + 11.0f};
    view.mouseMove(attack, {});
    const bool columnsShot = renderer.save(view, dir / "35s-mods-target-columns.png");
    click(view, attack);
    const bool attackPicked = slotIs(0, 0, paramIndex(1, OpParam::Attack));
    view.undo();
    check(columnsShot && attackPicked && slotIs(0, 0, paramIndex(0, OpParam::Ratio)),
          "an operator's targets show in two columns, the envelope's beside the pitch's");
    // The target menu takes the keyboard into its search: "b det" lists Op B's Detune, with its path, and Enter
    // picks it.
    click(view, {300.0f, 182.0f});
    const bool menuKeys = view.typing();
    type(view, "b det");
    view.mouseMove({600.0f, 468.0f}, {});
    const bool searchShot = renderer.save(view, dir / "35r-mods-target-search.png");
    view.typeKey(View::Key::Enter);
    const bool found = slotIs(0, 0, paramIndex(1, OpParam::Detune)) && !view.typing();
    view.undo();
    // "a release" finds Op A's Release, then its Release Curve: Down picks the second. Escape empties the search,
    // then closes the menu.
    click(view, {300.0f, 182.0f});
    type(view, "a release");
    view.typeKey(View::Key::Down);
    view.typeKey(View::Key::Enter);
    const bool second = slotIs(0, 0, paramIndex(0, OpParam::ReleaseCurve));
    view.undo();
    click(view, {300.0f, 182.0f});
    type(view, "zzz");
    view.typeKey(View::Key::Enter); // nothing found, nothing picked
    const bool nothing = view.typing() && slotIs(0, 0, paramIndex(0, OpParam::Ratio));
    view.typeKey(View::Key::Escape);
    const bool stillOpen = view.typing();
    view.typeKey(View::Key::Escape);
    check(menuKeys && searchShot && found && second && nothing && stillOpen && !view.typing() &&
              slotIs(0, 0, paramIndex(0, OpParam::Ratio)),
          "the target menu's search lists the targets found with their path, Up and Down pick one and Enter takes it");
    // "a det" finds Op A's Detune first, which LFO 1 modulates already through its second slot and so can't be
    // picked: the match after it is lit instead (read off the rendered pixels at the matches' right ends, 22 px
    // apart from y 215: the second's row lit, the third's not), and Enter takes that one.
    click(view, {300.0f, 182.0f});
    type(view, "a det");
    view.mouseMove({600.0f, 468.0f}, {});
    const bool skipShot = renderer.save(view, dir / "35t-mods-target-search-skip.png");
    auto matchPixel = [&](int match) { return renderer.pixel({466.0f, 243.0f + 22.0f * static_cast<float>(match)}); };
    const bool secondLit = matchPixel(1) != matchPixel(2);
    view.typeKey(View::Key::Enter);
    const int taken = slotTarget(model.value(slotTargetIndex(0, 0)));
    const bool tookLit = !view.typing() && taken >= 0 && taken != paramIndex(0, OpParam::Ratio) &&
                         taken != paramIndex(0, OpParam::Detune);
    view.undo();
    check(skipShot && secondLit && tookLit && slotIs(0, 0, paramIndex(0, OpParam::Ratio)),
          "the target menu's search lights the first match that can be picked, which Enter takes");
}

// Eight LFOs on Op A's Pitch: under the knob, two depth handles and a "+6" pill, nothing drawn outside the knob's
// column (read off the rendered pixels). The pill opens the list of all eight, whose rows bypass, set the depth of
// and remove their modulation as a card's rows do; a click elsewhere closes it. A macro's handle held over the pill
// opens the list under it, and let go of on a row makes the macro that row's aux; let go of anywhere else, the list
// closes again. Six on a matrix cell: two handles and a smaller pill, none in the next cell.
void testModPill(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    // Beside the knob, between its column and the next ones' (the Ratio readout's and Detune's), where the handles are.
    // Beside the knob, between its column and the next ones' (the Ratio readout's and Detune's), at the depth handles' height.
    auto beside = [&] {
        std::vector<uint32_t> pixels;
        for (float y = 252.0f; y < 263.0f; y += 1.0f)
            for (float x = 17.0f; x < 27.0f; x += 1.0f) {
                pixels.push_back(renderer.pixel({kPitchKnob.x - x, y}));
                pixels.push_back(renderer.pixel({kPitchKnob.x + x, y}));
            }
        return pixels;
    };
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "40-pill-none.png");
    const std::vector<uint32_t> none = beside();
    const int pitch = paramIndex(0, OpParam::Transpose);
    for (int s = 0; s < 8; ++s) {
        model.set(paramIndex(s, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        model.set(slotTargetIndex(s, 0), slotTargetValue(pitch));
        model.set(slotAmountIndex(s, 0), 0.05 + 0.03 * s);
    }
    view.poll();
    const bool shot = renderer.save(view, dir / "40a-pill.png");
    check(shot && beside() == none, "eight modulations on a knob show two handles and a pill, nothing outside its column");

    // The pill sits 2 px right of the two handles: 31 px across, centered under the knob, its middle 9.5 px right of
    // the knob's. Its list, 265 px wide and 8 more for its scrollbar, centered under it: the rows 24 px apart from
    // 24 px down, as on a card, six of them shown. From the knob's exact middle, as the inspector's knob row places it
    // (kPitchKnob rounds it).
    const Point pill{detail::kInspector.x + 16.0f + (300.0f - 32.0f) / 6.0f * 2.5f + 9.5f, 257.0f + kInsDy};
    const float listX = pill.x - 136.5f + 6.0f, rowsY = pill.y + 4.5f + 6.0f + 24.0f;
    auto row = [&](int k, float x) { return Point{listX + x, rowsY + k * 24.0f + 10.0f}; };
    view.mouseMove(pill, {});
    const bool hoverShot = renderer.save(view, dir / "40b-pill-hover.png");
    const uint32_t seventhAway = renderer.pixel(row(6, 60.0f));
    click(view, pill);
    view.mouseMove(row(3, 60.0f), {}); // on LFO 4's name: its ring is drawn wider
    const bool listShot = renderer.save(view, dir / "40c-pill-list.png");
    const uint32_t open = renderer.pixel(row(5, 60.0f));
    const bool sixShown = renderer.pixel(row(6, 60.0f)) == seventhAway; // no seventh row under the sixth
    drag(view, row(0, 116.0f + 38.0f), row(0, 116.0f + 57.0f)); // LFO 1's depth bar, to half way right: +50 %
    const bool deep = std::abs(model.value(slotAmountIndex(0, 0)) - 0.5) < 1e-6;
    // Two notches down over the names: LFO 7 and 8 are the fifth and sixth rows shown.
    view.wheel(row(3, 60.0f), -2.0f, {});
    view.mouseMove(row(3, 60.0f), {});
    const bool scrolledShot = renderer.save(view, dir / "40c2-pill-list-scrolled.png");
    click(view, row(5, 9.0f)); // LFO 8's bypass dot
    const bool bypassed = model.value(slotBypassIndex(7, 0)) == 1.0;
    click(view, row(4, 243.0f)); // LFO 7's remove
    const bool removed = slotTarget(model.value(slotTargetIndex(6, 0))) < 0;
    // Dragged down with the middle button, the rows follow back to the top: the first is LFO 1's again.
    view.middleDown(row(2, 60.0f));
    view.mouseMove(row(4, 60.0f), {});
    view.middleUp(row(4, 60.0f));
    click(view, row(0, 9.0f));
    const bool panned = model.value(slotBypassIndex(0, 0)) == 1.0;
    click(view, row(0, 9.0f));
    click(view, {600.0f, 328.0f}); // elsewhere: the list closes
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "40d-pill-closed.png");
    const uint32_t closed = renderer.pixel(row(5, 60.0f));
    check(hoverShot && listShot && sixShown && scrolledShot && bypassed && deep && removed && panned &&
              model.value(slotBypassIndex(0, 0)) == 0.0 && open != closed,
          "a pill's list shows six rows, scrolls by the wheel and the middle button, bypasses, sets the depth of and "
          "removes its modulations, and closes at a click elsewhere");

    // Macro 2's handle held over the pill opens the list; let go of on LFO 4's row, Macro 2 becomes its aux.
    const float strip = view.height() - 179.0f;
    view.mouseDown({252.0f, strip}, {});
    view.mouseMove({600.0f, strip - 100.0f}, {});
    view.mouseMove(pill, {});
    view.mouseMove(row(3, 60.0f), {});
    const bool dropShot = renderer.save(view, dir / "40e-pill-drop.png");
    view.mouseUp(row(3, 60.0f), {});
    const bool aux = slotAuxSource(model.value(slotAuxIndex(3, 0))) == kMaxSources + 1 &&
                     slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0;
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "40f-pill-aux.png");
    const bool stays = renderer.pixel(row(5, 60.0f)) == open;
    click(view, {600.0f, 328.0f});
    // Held over the pill and let go of elsewhere, the list closes again and nothing changes.
    view.mouseDown({252.0f, strip}, {});
    view.mouseMove(pill, {});
    view.mouseMove({600.0f, 328.0f}, {});
    view.mouseUp({600.0f, 328.0f}, {});
    renderer.save(view, dir / "40g-pill-sprung.png");
    check(dropShot && aux && stays && renderer.pixel(row(5, 60.0f)) == closed &&
              slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0,
          "a handle held over a pill opens its list, a row takes it as an aux, and elsewhere the list closes");

    // With LFO 4's aux, the list opens 96 px wider, an aux column after the names: LFO 4's row shows Macro 2's chip
    // (its dot read off the rendered pixels, where LFO 3's row has none), and the chip's x takes the aux away, the
    // list narrowing again.
    click(view, pill);
    view.mouseMove({600.0f, 328.0f}, {});
    const float wideX = std::clamp(pill.x - 184.5f, 4.0f, 1200.0f - 4.0f - 369.0f) + 6.0f; // held inside the window
    auto wideRow = [&](int k, float x) { return Point{wideX + x, rowsY + k * 24.0f + 10.0f}; };
    const bool chipShot = renderer.save(view, dir / "40g2-pill-aux-chip.png");
    const bool chip = renderer.pixel(wideRow(3, 126.0f)) != renderer.pixel(wideRow(2, 126.0f));
    click(view, wideRow(3, 201.0f)); // the chip's x
    const bool auxCleared = slotAuxSource(model.value(slotAuxIndex(3, 0))) < 0;
    view.mouseMove({600.0f, 328.0f}, {});
    renderer.save(view, dir / "40g3-pill-aux-cleared.png");
    const bool narrow = renderer.pixel(wideRow(3, 126.0f)) == renderer.pixel(wideRow(2, 126.0f));
    click(view, {600.0f, 328.0f});
    check(chipShot && chip && auxCleared && narrow,
          "a pill's list shows its modulations' auxes in a column of their own, and their x removes them");

    // LFO 3's handle hovered: its depth handle is in the pill, where its dashed line ends.
    view.mouseMove(sourceHandle(2), {});
    check(renderer.save(view, dir / "40h-pill-link.png"), "render a source's line ending at the pill holding its handle");

    // Six on the matrix's B -> C cell: two handles in its corner and a pill, none in the corner of B -> B left of it.
    click(view, {160.0f, 116.0f}); // MATRIX
    const Point cell = gridCellCenter(1, 2), left = gridCellCenter(1, 1);
    auto corner = [&] {
        std::vector<uint32_t> pixels;
        for (float y = cell.y + 6.0f; y < cell.y + 14.3f; y += 1.0f)
            for (float x = left.x; x < left.x + 15.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    view.mouseMove({600.0f, 468.0f}, {});
    renderer.save(view, dir / "40i-cell-none.png");
    const std::vector<uint32_t> plain = corner();
    const int route = matrixIndex(1, 2);
    for (int s = 8; s < 14; ++s) {
        model.set(paramIndex(s, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        model.set(slotTargetIndex(s, 0), slotTargetValue(route));
        model.set(slotAmountIndex(s, 0), 0.2);
    }
    view.poll();
    const bool cellShot = renderer.save(view, dir / "40j-cell-pill.png");
    check(cellShot && corner() == plain, "six modulations on a matrix cell show two handles and a pill, none in the next cell");

    // At the larger handle sizes, the cell's handles and pill are drawn bigger, in the same places (read off the
    // rendered pixels round them), and Small draws them as before.
    auto handles = [&] {
        std::vector<uint32_t> pixels;
        for (float y = cell.y + 4.0f; y < cell.y + 22.0f; y += 1.0f)
            for (float x = cell.x - 16.0f; x < cell.x + 20.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    const std::vector<uint32_t> smallHandles = handles();
    model.handles = 1;
    const bool mediumShot = renderer.save(view, dir / "40k-cell-pill-medium.png");
    const std::vector<uint32_t> mediumHandles = handles();
    model.handles = 2;
    const bool largeShot = renderer.save(view, dir / "40l-cell-pill-large.png");
    const std::vector<uint32_t> largeHandles = handles();
    model.handles = 0;
    renderer.save(view, dir / "40m-cell-pill-small.png");
    check(mediumShot && largeShot && mediumHandles != smallHandles && largeHandles != mediumHandles &&
              handles() == smallHandles,
          "the depth handles and pills are drawn in the size picked, Small as before");
}

// LFO 2 modulating LFO 1's depth on operator A's Ratio: hovering LFO 2's handle draws a dashed line to that
// depth bar in the MODS view, which has a dot for the modulation, like a matrix cell's.
void renderLfoOnLfo(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    for (int source = 0; source < 2; ++source)
        model.values[paramIndex(source, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    model.values[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(0, OpParam::Ratio));
    model.values[slotAmountIndex(0, 0)] = 0.4;
    model.values[slotTargetIndex(1, 0)] = slotTargetValue(slotAmountIndex(0, 0));
    model.values[slotAmountIndex(1, 0)] = 0.5;
    view.poll();
    // In the MODS view, LFO 1's row the top one: with Shift a depth bar moves from where it is, a hundredth as fast; half
    // its width (164 px from x 474) is 1 % then.
    click(view, {314.0f, 116.0f});
    const Point bar{474.0f + 82.0f, 182.0f};
    view.mouseDown(bar, {true, false});
    view.mouseMove({bar.x + 82.0f, bar.y}, {true, false});
    view.mouseUp({bar.x + 82.0f, bar.y}, {true, false});
    check(std::abs(model.value(slotAmountIndex(0, 0)) - 0.41) < 1e-6, "Shift drags a depth bar finely, from where it is");
    view.mouseMove(sourceHandle(1), {});
    const bool shot = renderer.save(view, dir / "18c-lfo-on-lfo.png");
    // Hovered, the dot says what the modulation takes its target through, in the target's units.
    const Point dot{474.0f + 164.0f - 4.0f, 168.0f + 4.0f}; // on LFO 1's depth bar
    view.mouseMove(dot, {});
    const bool popup = renderer.save(view, dir / "18d-depth-popup.png");
    view.mouseMove({600.0f, 328.0f}, {});
    doubleClick(view, dot);
    check(shot && popup && slotTarget(model.value(slotTargetIndex(1, 0))) < 0,
          "a depth another LFO modulates has a dashed link and a dot, which double-clicked removes it");
}

} // namespace

void run(const std::filesystem::path &dir, float scale) {
    Renderer renderer(scale);
    FakeController model;
    View view(model);
    const UiLayout &layout = model.layout();
    check(shown(model).empty(), "a new patch has no LFOs or envelopes");
    check(renderer.save(view, dir / "1-default.png"), "render default patch");
    click(view, kInitButton);
    click(view, kLoadButton);
    check(model.resets == 1 && model.loads == 1, "Init and Load go ahead at once while the patch is the default");

    // ADD LFO and ADD ENV fill the first free places, shown in the order they came.
    addSource(view, SourceType::Lfo);
    addSource(view, SourceType::Env);
    check(shown(model) == std::vector<int>{0, 1} &&
              model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Lfo) &&
              model.value(paramIndex(1, SourceParam::Type)) == static_cast<double>(SourceType::Env) &&
              model.value(sourceEnabledIndex(0)) == 1.0 && model.openEdits == 0,
          "the add buttons add an LFO and an envelope, switched on");
    // An LFO's and an envelope's handles are faint while they modulate nothing, like a macro's, and a switched-off
    // source's is faint too, like its card (read off the rendered pixels: its ring darkens).
    {
        const Point ring{sourceHandle(0).x + 6.0f, sourceHandle(0).y};
        const Point envRing{sourceHandle(1).x + 6.0f, sourceHandle(1).y};
        view.mouseMove({600.0f, 328.0f}, {});
        const bool idleShot = renderer.save(view, dir / "1a-source-idle.png");
        const int idle = ringLevel(renderer, ring, 20.0f), envIdle = ringLevel(renderer, envRing, 20.0f);
        model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
        model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
        view.mouseMove({600.0f, 328.0f}, {});
        const bool onShot = renderer.save(view, dir / "1b-source-on.png");
        const int on = ringLevel(renderer, ring, 20.0f), envOn = ringLevel(renderer, envRing, 20.0f);
        click(view, sourcePower(0));
        view.mouseMove({600.0f, 328.0f}, {});
        const bool offShot = renderer.save(view, dir / "1c-source-off.png");
        const int off = ringLevel(renderer, ring, 20.0f);
        click(view, sourcePower(0));
        model.set(slotTargetIndex(0, 0), 0.0);
        model.set(slotTargetIndex(1, 0), 0.0);
        std::printf("      LFO 1's handle: brightness %d idle, %d on, %d off; Env 1's %d idle, %d on\n", idle, on, off,
                    envIdle, envOn);
        check(idleShot && on > 200 && idle * 18 < on * 10 && envOn > 200 && envIdle * 18 < envOn * 10,
              "an LFO's and an envelope's handles are faint until they modulate something");
        check(onShot && offShot && model.value(sourceEnabledIndex(0)) == 1.0 && off * 18 < on * 10,
              "a switched-off source's handle looks switched off");
    }

    // Op A's envelope fills its display: the hold handle sits just after the peak, and
    // dragging it 20 % of the zoomed scale along gives a hold of 0.2.
    {
        const double total = normFromTime(1.5) + normFromTime(0.015); // A's decay and release
        const float zoom = static_cast<float>((248.0f - 40.0f) / total);
        drag(view, {930.0f + kInsDx, kOpEnvTop + 12.0f}, {930.0f + kInsDx + 0.2f * zoom, kOpEnvTop + 12.0f});
        check(std::abs(model.value(paramIndex(0, OpParam::Hold)) - 0.2) < 1e-3 && model.openEdits == 0,
              "dragging the hold handle sets the hold, at the zoom the drag started with");
    }
    // Dragged on past the display's end (at its bottom right), the release handle goes on lengthening the release,
    // while the envelope zooms out to stay in the display: nothing of it shows between the display and the panel's
    // edge.
    {
        const int release = paramIndex(0, OpParam::Release);
        const double before = model.value(release);
        auto edge = [&] {
            std::vector<uint32_t> column;
            for (float y = kOpEnvTop; y < kOpEnvBottom + 1.0f; y += 1.0f)
                column.push_back(renderer.pixel({1176.0f + kInsDx, y}));
            return column;
        };
        const bool shot = renderer.save(view, dir / "4c-envelope-dragged.png");
        const std::vector<uint32_t> still = edge();
        view.mouseDown({1158.0f + kInsDx, kOpEnvBottom - 10.0f}, {});
        view.mouseMove({1240.0f + kInsDx, kOpEnvBottom - 10.0f}, {});
        view.mouseMove({1320.0f + kInsDx, kOpEnvBottom - 10.0f}, {});
        const bool inside = renderer.save(view, dir / "4c-envelope-dragged.png") && edge() == still;
        view.mouseUp({1320.0f + kInsDx, kOpEnvBottom - 10.0f}, {});
        const bool longer = model.value(release) > before + 0.2;
        view.undo();
        check(shot && inside && longer && model.value(release) == before,
              "an envelope handle dragged past its display lengthens its segment, the envelope staying inside");
    }
    // Below 1 ms the attack's segment shrinks away: A's attack handle (1 ms, at the end of its 10 pixels from the
    // envelope's start at 910) dragged half way there sets 0.5 ms, and on past the start 0 ms, which draws the attack
    // as a vertical line up from the start (read off the rendered pixels: its column changes nearly from top to
    // bottom, where the 1 ms attack's steep slant crossed it only near the bottom).
    {
        const int attack = paramIndex(0, OpParam::Attack);
        auto column = [&] {
            std::vector<uint32_t> pixels;
            for (float y = kOpEnvTop + 14.0f; y < kOpEnvBottom - 14.0f; y += 1.0f)
                pixels.push_back(renderer.pixel({910.0f + kInsDx, y}));
            return pixels;
        };
        const bool before = renderer.save(view, dir / "4d-envelope-attack.png");
        const std::vector<uint32_t> slant = column();
        drag(view, {920.0f + kInsDx, kOpEnvTop + 12.0f}, {915.0f + kInsDx, kOpEnvTop + 12.0f});
        const double half = envTimeFromNorm(model.value(attack));
        drag(view, {915.0f + kInsDx, kOpEnvTop + 12.0f}, {900.0f + kInsDx, kOpEnvTop + 12.0f});
        const double zero = envTimeFromNorm(model.value(attack));
        const bool after = renderer.save(view, dir / "4d-envelope-attack-0ms.png");
        const std::vector<uint32_t> vertical = column();
        size_t changed = 0;
        for (size_t i = 0; i < slant.size(); ++i)
            changed += slant[i] != vertical[i] ? 1 : 0;
        view.undo();
        view.undo();
        std::printf("      attack dragged to %.2f ms, then %.2f ms; its column changed in %zu of %zu rows\n",
                    half * 1000.0, zero * 1000.0, changed, slant.size());
        check(before && after && std::abs(half - 0.0005) < 5e-5 && zero == 0.0 && changed * 10 >= slant.size() * 9 &&
                  model.value(attack) == 0.0,
              "an attack dragged below 1 ms takes fractions of a millisecond, and at 0 ms draws as a vertical line");
    }
    // A double-click on a segment's bend handle straightens it to 0 %, whatever it started as: the decay's, halfway
    // along the flat top of A's envelope (full sustain) and found there by its cursor, starts at +50 %. Hovered, its
    // readout goes at the display's top right, over the flat top, whose line doesn't show through it (read off the
    // rendered pixels: the readout is what changes from a render without it, the line the first row lit along much of
    // the way, in a color well bluer than it is red).
    {
        const int decayCurve = paramIndex(0, OpParam::DecayCurve);
        constexpr int kRows = 28, kColumns = 120;
        auto corner = [&] {
            std::vector<uint32_t> pixels;
            for (int row = 0; row < kRows; ++row)
                for (int column = 0; column < kColumns; ++column)
                    pixels.push_back(renderer.pixel({1040.0f + kInsDx + column, kOpEnvTop + row}));
            return pixels;
        };
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "4d-envelope-readout.png");
        const std::vector<uint32_t> plain = corner();
        Point bend{0.0f, 0.0f};
        for (float y = kOpEnvTop + 6.0f; y <= kOpEnvTop + 18.0f && bend.x == 0.0f; y += 2.0f)
            for (float x = 950.0f + kInsDx; x < 1150.0f + kInsDx && bend.x == 0.0f; x += 2.0f) {
                view.mouseMove({x, y}, {});
                if (view.cursor() == CursorKind::Vertical)
                    bend = {x, y};
            }
        const bool shot = renderer.save(view, dir / "4d-envelope-readout.png");
        const std::vector<uint32_t> hovered = corner();
        int top = kRows, bottom = -1, left = kColumns, right = -1, line = -1;
        for (int row = 0; row < kRows; ++row) {
            int lit = 0;
            for (int column = 0; column < kColumns; ++column) {
                const uint32_t color = plain[row * kColumns + column];
                lit += std::max({color >> 16 & 255, color >> 8 & 255, color & 255}) > 120;
                if (hovered[row * kColumns + column] != color) {
                    top = std::min(top, row);
                    bottom = row;
                    left = std::min(left, column);
                    right = std::max(right, column);
                }
            }
            if (line < 0 && lit >= 40)
                line = row;
        }
        auto blue = [](uint32_t color) {
            return static_cast<int>(color & 255) - static_cast<int>(color >> 16 & 255) > 60;
        };
        int under = 0, through = 0; // the line's pixels across the readout (inside its edges), without it and with it
        for (int column = left + 2; line >= 0 && column <= right - 2; ++column) {
            under += blue(plain[line * kColumns + column]);
            through += blue(hovered[line * kColumns + column]);
        }
        std::printf("      envelope readout: rows %d-%d, over the curve's line at row %d (%d pixels, %d showing)\n", top,
                    bottom, line, under, through);
        check(shot && top <= line && line <= bottom && under >= 20 && through == 0,
              "an envelope handle's readout is drawn over the curve");
        const double before = model.value(decayCurve);
        doubleClick(view, bend);
        const bool straight = model.value(decayCurve) == 0.0 &&
                              model.value(paramIndex(0, OpParam::AttackCurve)) == 0.0 &&
                              model.value(paramIndex(0, OpParam::ReleaseCurve)) == before;
        view.undo();
        check(bend.x > 0.0f && before == 0.5 && straight && model.value(decayCurve) == before,
              "a double-click on an envelope segment's bend handle straightens it to 0 %");
    }
    drag(view, kPitchKnob, {kPitchKnob.x, kPitchKnob.y - 30.0f});
    view.wheel(kPitchKnob, 1.0f, {});
    check(model.value(paramIndex(0, OpParam::Transpose)) == 4.0, "the pitch knob transposes in whole semitones");
    model.set(paramIndex(0, OpParam::Transpose), 0.0);
    check(isModTarget(paramIndex(0, OpParam::Transpose)), "an operator's pitch can be modulated");
    // An LFO's clock modes: one at a time, as Poly | Mono, each setting Free, Anchor and One-Shot for it in one undo
    // step; a click on the one lit leaves it, and the wheel steps along them. Unipolar is a switch of its own.
    {
        const int free = paramIndex(0, SourceParam::Free), anchor = paramIndex(0, SourceParam::Anchor),
                  oneShot = paramIndex(0, SourceParam::OneShot);
        auto modes = [&] {
            return std::array{model.value(free), model.value(anchor), model.value(oneShot)};
        };
        using Modes = std::array<double, 3>;
        click(view, oneShotToggle(0));
        click(view, unipolarToggle(0));
        const bool oneShotOn =
            modes() == Modes{0.0, 0.0, 1.0} && model.value(paramIndex(0, SourceParam::Unipolar)) == 1.0;
        click(view, anchorToggle(0));
        const bool anchored = modes() == Modes{0.0, 1.0, 0.0};
        click(view, anchorToggle(0));
        const bool stays = modes() == Modes{0.0, 1.0, 0.0};
        click(view, freeToggle(0));
        const bool freed = modes() == Modes{1.0, 0.0, 0.0};
        view.undo();
        const bool undone = modes() == Modes{0.0, 1.0, 0.0};
        click(view, retriggerToggle(0));
        const bool retriggered = modes() == Modes{0.0, 0.0, 0.0};
        view.wheel(retriggerToggle(0), 1.0f, {});
        const bool wheeled = modes() == Modes{1.0, 0.0, 0.0};
        click(view, retriggerToggle(0));
        click(view, unipolarToggle(0));
        check(oneShotOn && anchored && stays && freed && undone && retriggered && wheeled && modes() == Modes{} &&
                  model.value(paramIndex(0, SourceParam::Unipolar)) == 0.0,
              "an LFO's clock modes go one at a time, undoably, and its Unipolar switch stands apart");
        click(view, unipolarToggle(0)); // left unipolar, for the title menu's polarity check further on
    }

    // Drag LFO 1's handle onto the selected operator's Phase knob.
    view.mouseDown(sourceHandle(0), {});
    view.mouseMove({500.0f, 328.0f}, {});
    view.mouseMove(kOpPhaseKnob, {});
    renderer.save(view, dir / "2-dragging-lfo.png");
    view.mouseUp(kOpPhaseKnob, {});
    const int opPhase = paramIndex(0, OpParam::Phase);
    check(slotTarget(model.value(slotTargetIndex(0, 0))) == opPhase && model.value(slotAmountIndex(0, 0)) != 0.0,
          "dropping LFO 1 on a knob assigns its first slot");

    // Only A starts switched on. Switch B, C and D on (as their chips would), then E with its chip,
    // then wire B into A.
    bool othersOff = true;
    for (int op = 1; op < kOps; ++op)
        othersOff = othersOff && model.value(paramIndex(op, OpParam::Enabled)) == 0.0;
    check(model.value(paramIndex(0, OpParam::Enabled)) == 1.0 && othersOff, "a new patch has only operator A switched on");
    for (int op = 1; op < 4; ++op)
        model.set(paramIndex(op, OpParam::Enabled), 1.0);
    view.poll();
    click(view, nodeTab(4));
    check(model.value(paramIndex(4, OpParam::Enabled)) == 1.0 && layout.selectedNode == 4,
          "a switched-off operator's tab switches it on and selects it");
    // While it is off, its page is dimmed (read off the rendered pixels: the envelope's line darkens).
    const Point envColumn{1034.0f + kInsDx, kOpEnvTop};
    view.mouseMove({600.0f, 328.0f}, {});
    const bool pageOnShot = renderer.save(view, dir / "1d-op-on.png");
    const int pageOn = ringLevel(renderer, envColumn, kOpEnvBottom - kOpEnvTop);
    click(view, nodeTab(4));
    const bool tabOff = model.value(paramIndex(4, OpParam::Enabled)) == 0.0 && layout.selectedNode == 4;
    view.mouseMove({600.0f, 328.0f}, {});
    const bool pageOffShot = renderer.save(view, dir / "1e-op-off.png");
    const int pageOff = ringLevel(renderer, envColumn, kOpEnvBottom - kOpEnvTop);
    click(view, nodeTab(4));
    check(tabOff && model.value(paramIndex(4, OpParam::Enabled)) == 1.0, "the selected tab switches its operator off and on");
    std::printf("      E's page: brightness %d on, %d off\n", pageOn, pageOff);
    check(pageOnShot && pageOffShot && pageOn > 200 && pageOff * 10 < pageOn * 7,
          "a switched-off operator's page is dimmed");
    click(view, nodeTab(1));
    check(model.value(paramIndex(1, OpParam::Enabled)) == 1.0 && layout.selectedNode == 1,
          "another tab only selects an operator that is on");
    const Point a = nodeCenter(layout, 0), b = nodeCenter(layout, 1);
    drag(view, portOf(layout, 1), a);
    check(model.value(matrixIndex(1, 0)) > 0.0, "dragging B's port onto A makes B modulate A");
    click(view, a); // back to editing A

    // Env 1 onto FM Amount in the header, then onto the B -> A wire label.
    drag(view, sourceHandle(1), kFmAmountKnob);
    // B -> A bows sideways; its label sits near the midpoint, offset like in view.cpp.
    const float length = std::hypot(a.x - b.x, a.y - b.y);
    const Point u{(a.x - b.x) / length, (a.y - b.y) / length};
    const Point label{(a.x + b.x) * 0.5f - u.y * length * 0.09f, (a.y + b.y) * 0.5f + u.x * length * 0.09f};
    drag(view, sourceHandle(1), label);
    const int fmAmount = paramIndex(GlobalParam::FmAmount);
    check(slotTarget(model.value(slotTargetIndex(1, 0))) == fmAmount, "Env 1 slot 1 targets FM Amount");
    check(slotTarget(model.value(slotTargetIndex(1, 1))) == matrixIndex(1, 0), "Env 1 slot 2 targets the B -> A wire");

    // Turned down to 0 %, the B -> A wire stays, so modulation can still be dropped on its label;
    // only Disconnect removes it.
    auto canDisconnect = [&] {
        for (const MenuItem &item : view.contextMenu(label))
            if (item.label == tr(Str::MenuDisconnect))
                return item.command;
        return -1;
    };
    drag(view, label, {label.x, label.y + 200.0f});
    check(model.value(matrixIndex(1, 0)) == 0.0 && canDisconnect() >= 0, "a wire turned down to 0 % stays connected");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "2b-wire-at-zero.png"), "render a wire kept at 0 %");
    view.menuCommand(canDisconnect());
    check(canDisconnect() < 0, "Disconnect removes a wire kept at 0 %");
    // Turned on from outside, by the host, and dragged down before the editor polled: it stays too.
    model.set(matrixIndex(1, 0), 0.3);
    drag(view, label, {label.x, label.y + 200.0f});
    check(model.value(matrixIndex(1, 0)) == 0.0 && canDisconnect() >= 0,
          "a wire turned on by the host and dragged to 0 % before a poll stays connected");
    model.set(matrixIndex(1, 0), 0.3);
    view.poll();

    // An operator's output turned down to 0 % keeps its wire to OUT, like a wire between nodes.
    auto outDisconnect = [&](Point at) {
        for (const MenuItem &item : view.contextMenu(at))
            if (item.label == tr(Str::MenuDisconnect))
                return item.command;
        return -1;
    };
    const Point aOut = outLabelOf(layout, 0);
    drag(view, aOut, {aOut.x, aOut.y + 200.0f});
    check(model.value(paramIndex(0, OpParam::Out)) == 0.0 && outDisconnect(aOut) >= 0,
          "an output turned down to 0 % stays connected");
    model.set(paramIndex(0, OpParam::Out), 1.0);
    view.poll();
    // The inspector's last knob is the operator's Pan, bipolar: dragged up 40 px it moves 20 % of its range, to R40,
    // and a double-click centers it again.
    drag(view, kPanKnob, {kPanKnob.x, kPanKnob.y - 40.0f});
    const bool panned = std::abs(model.value(paramIndex(0, OpParam::Pan)) - 0.4) < 1e-6;
    doubleClick(view, kPanKnob);
    check(panned && model.value(paramIndex(0, OpParam::Pan)) == 0.0 && model.value(paramIndex(0, OpParam::Out)) == 1.0,
          "an operator's Pan knob turns from the middle and resets to it, its output left alone");
    // Modulation dropped on a silent operator's OUT cell in the matrix shows its wire to OUT; Disconnect takes both
    // away. (Its arrowhead's menu has Disconnect too; its label has slid down the line, off E under B.)
    click(view, b);
    click(view, kMatrixTab);
    drag(view, sourceHandle(0), gridCellCenter(1, kNodes));
    click(view, kRoutingTab);
    const Point bOut = outArrowOf(layout, 1);
    check(slotTarget(model.value(slotTargetIndex(0, 1))) == paramIndex(1, OpParam::Out) &&
              model.value(paramIndex(1, OpParam::Out)) == 0.0 && outDisconnect(bOut) >= 0,
          "modulating an operator's output at 0 % shows its wire to OUT");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "2c-modulated-out.png"), "render a modulated output at 0 %");
    view.menuCommand(outDisconnect(bOut));
    // (Asked along its line: by OUT's edge the arrowheads of the lines to it lie close, A's among them.)
    const Point bLine = detail::outWireOf(1, nodeCenter(layout, 1)).start;
    check(outDisconnect({bLine.x + 12.0f, bLine.y}) < 0 && slotTarget(model.value(slotTargetIndex(0, 1))) < 0,
          "Disconnect takes an output's wire away with its modulation");
    click(view, a);

    // Dragging a wire's arrowhead moves the route there, with its amount and modulation: B -> A onto C, Env 1
    // going along, and back by undo; A's arrow to OUT onto C, then A -> C's onto the bus, so A goes out again.
    // Let go of in empty space, an arrowhead takes its route away, with its modulation, and undo brings it back.
    {
        const Point cNode = nodeCenter(layout, 2);
        auto envTargets = [&](int param) {
            for (int slot = 0; slot < kSlots; ++slot)
                if (slotTarget(model.value(slotTargetIndex(1, slot))) == param)
                    return true;
            return false;
        };
        drag(view, sourceHandle(1), label);
        const double bToA = model.value(matrixIndex(1, 0));
        view.mouseDown(arrowOf(b, a), {});
        view.mouseMove({(a.x + cNode.x) * 0.5f + 60.0f, (a.y + cNode.y) * 0.5f}, {});
        view.mouseMove(cNode, {});
        const bool shot = renderer.save(view, dir / "2d-rerouting.png");
        view.mouseUp(cNode, {});
        const bool moved = model.value(matrixIndex(1, 0)) == 0.0 && model.value(matrixIndex(1, 2)) == bToA &&
                           envTargets(matrixIndex(1, 2)) && !envTargets(matrixIndex(1, 0));
        view.mouseMove({400.0f, 328.0f}, {});
        const bool movedShot = renderer.save(view, dir / "2e-rerouted.png");
        view.undo();
        check(shot && movedShot && moved && model.value(matrixIndex(1, 2)) == 0.0 &&
                  model.value(matrixIndex(1, 0)) == bToA && envTargets(matrixIndex(1, 0)),
              "dragging a wire's arrowhead onto another node moves the route with its modulation, undoably");
        const int aOutParam = paramIndex(0, OpParam::Out);
        drag(view, outArrowOf(layout, 0), cNode);
        const bool outMoved = model.value(aOutParam) == 0.0 && model.value(matrixIndex(0, 2)) == 1.0;
        drag(view, arrowOf(a, cNode), kOutCore);
        check(outMoved && model.value(aOutParam) == 1.0 && model.value(matrixIndex(0, 2)) == 0.0,
              "an arrow to OUT moves onto a node, and a wire's arrow onto the OUT bus");
        view.mouseDown(arrowOf(b, a), {});
        view.mouseMove({400.0f, 298.0f}, {});
        view.mouseMove({400.0f, 268.0f}, {});
        const bool removingShot = renderer.save(view, dir / "2f-removing.png");
        view.mouseUp({400.0f, 268.0f}, {});
        const bool removed = model.value(matrixIndex(1, 0)) == 0.0 && !envTargets(matrixIndex(1, 0)) &&
                             canDisconnect() < 0;
        view.undo();
        check(removingShot && removed && model.value(matrixIndex(1, 0)) == bToA && envTargets(matrixIndex(1, 0)),
              "an arrowhead let go of away from the nodes and the bus removes its route with its modulation, undoably");
        // The mouse taken away mid-drag (the window losing its capture) drops nothing: the arrowhead held away from
        // the nodes leaves its route be, LFO 1's handle held over A's Out knob assigns nothing (let go of there, it
        // does), and a knob's turn ends where it got to, its gesture closed as one undo step.
        {
            auto used = [&](int source) {
                int count = 0;
                for (int slot = 0; slot < kSlots; ++slot)
                    count += slotTarget(model.value(slotTargetIndex(source, slot))) >= 0 ? 1 : 0;
                return count;
            };
            view.mouseDown(arrowOf(b, a), {});
            view.mouseMove({400.0f, 298.0f}, {});
            view.mouseMove({400.0f, 268.0f}, {});
            view.cancelDrag();
            const bool routeKept = model.value(matrixIndex(1, 0)) == bToA && envTargets(matrixIndex(1, 0));
            const int lfoUsed = used(0);
            view.mouseDown(sourceHandle(0), {});
            view.mouseMove({sourceHandle(0).x + 40.0f, sourceHandle(0).y - 40.0f}, {});
            view.mouseMove(kPanKnob, {});
            view.cancelDrag();
            const bool unassigned = used(0) == lfoUsed;
            drag(view, sourceHandle(0), kPanKnob);
            const bool assigns = used(0) == lfoUsed + 1;
            view.undo();
            const double fm = model.value(fmAmount);
            view.mouseDown(kFmAmountKnob, {});
            view.mouseMove({kFmAmountKnob.x, kFmAmountKnob.y - 20.0f}, {});
            view.cancelDrag();
            const bool turned = model.value(fmAmount) > fm && model.openEdits == 0;
            view.undo();
            check(routeKept && unassigned && assigns && used(0) == lfoUsed && turned && model.value(fmAmount) == fm,
                  "a drag called off when the mouse is taken away drops nothing, and ends its gesture");
        }
        view.menuCommand(canDisconnect()); // B -> A goes, with Env 1's modulation, as before
        model.set(matrixIndex(1, 0), 0.3);
        view.poll();
    }

    // Drag the Phase knob up by 40 px: +20 % of its range.
    view.mouseDown(kOpPhaseKnob, {});
    view.mouseMove({kOpPhaseKnob.x, kOpPhaseKnob.y - 40.0f}, {});
    view.mouseUp({kOpPhaseKnob.x, kOpPhaseKnob.y - 40.0f}, {});
    check(std::abs(model.value(opPhase) - 0.2) < 1e-6 && model.openEdits == 0, "knob drag sets the value, edits closed");

    // Connect C to A by dragging from C's port.
    view.mouseDown(portOf(layout, 2), {});
    view.mouseMove({(nodeCenter(layout, 2).x + a.x) * 0.5f, a.y - 40.0f}, {});
    view.mouseMove(a, {});
    renderer.save(view, dir / "3-connecting.png");
    view.mouseUp(a, {});
    check(model.value(matrixIndex(2, 0)) > 0.0, "dragging a port onto a node creates a wire");
    // Anywhere on a node's edge connects it, while further inside a drag moves it. A click on the edge only picks the
    // node: no feedback.
    {
        const Point c = nodeCenter(layout, 2);
        view.mouseMove({c.x - 22.0f, c.y}, {});
        check(renderer.save(view, dir / "3-edge-hover.png"), "render a node's edge lit under the mouse");
        click(view, {c.x, c.y - 22.0f});
        check(layout.selectedNode == 2 && model.value(matrixIndex(2, 2)) == 0.0,
              "a click on a node's edge picks it, without feedback");
        // Held back over its own node, once it has been well clear of it, the route being dragged loops round over its
        // top in a small circle and back into it, where letting go makes its feedback; standing on the ring around it
        // whether the node is selected or not. A drag only just past its edge and back shows no loop (read off the
        // rendered pixels, over the node's top, clear of its ring).
        {
            auto level = [](uint32_t color) {
                return std::max({static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                                 static_cast<int>(color & 255)});
            };
            auto overTop = [&] {
                int most = 0;
                for (float dx = -12.0f; dx <= 12.0f; dx += 3.0f)
                    most = std::max(most, level(brightestAlong(renderer, {c.x + dx, c.y - 36.0f},
                                                               {c.x + dx, c.y - 60.0f})));
                return most;
            };
            // The highest row over the node's middle that something bright is drawn on: the loop's top.
            auto loopTop = [&](int idle) {
                for (float y = c.y - 60.0f; y <= c.y - 36.0f; y += 1.0f)
                    for (float dx = -3.0f; dx <= 3.0f; dx += 1.0f)
                        if (level(renderer.pixel({c.x + dx, y})) > idle + 60)
                            return y - c.y;
                return 0.0f;
            };
            auto holdOverItself = [&](const char *shot) {
                view.mouseDown({c.x - 22.0f, c.y}, {});
                view.mouseMove({c.x - 40.0f, c.y - 30.0f}, {});
                view.mouseMove({c.x + 4.0f, c.y + 2.0f}, {});
                renderer.save(view, dir / shot);
            };
            renderer.save(view, dir / "3-edge-picked.png");
            const int idle = overTop();
            view.mouseDown({c.x - 22.0f, c.y}, {});
            view.mouseMove({c.x - 34.0f, c.y}, {}); // 10 px past its edge
            view.mouseMove({c.x + 4.0f, c.y + 2.0f}, {});
            renderer.save(view, dir / "3-connecting-near.png");
            const int edgeOnly = overTop();
            view.cancelDrag();
            holdOverItself("3-connecting-itself.png"); // C selected, by the click on its edge
            const int looped = overTop();
            const float selectedTop = loopTop(idle);
            view.mouseUp({c.x + 4.0f, c.y + 2.0f}, {});
            const bool fed = model.value(matrixIndex(2, 2)) == 0.25;
            view.undo();
            model.ui.selectedNode = 0;
            holdOverItself("3-connecting-itself-unselected.png");
            const float plainTop = loopTop(idle);
            view.cancelDrag();
            model.ui.selectedNode = 2;
            std::printf("      over the node's top: %d idle, %d back from just past its edge, %d held over itself; "
                        "the loop's top %.0f px over it selected, %.0f not\n",
                        idle, edgeOnly, looped, -selectedTop, -plainTop);
            check(edgeOnly < idle + 20 && fed && looped > idle + 60 && model.value(matrixIndex(2, 2)) == 0.0,
                  "a route held back over its own node once clear of it loops into it, and let go there feeds it "
                  "back; one only just past its edge shows no loop");
            check(selectedTop <= -48.0f && selectedTop >= -54.0f && plainTop == selectedTop,
                  "the loop stands on the ring around its node, selected or not");
        }
        drag(view, {c.x - 22.0f, c.y}, b);
        check(model.value(matrixIndex(2, 1)) > 0.0, "dragging from a node's left edge onto another connects them");
        view.undo();
        const float x = layout.nodeX[2];
        drag(view, {c.x - 8.0f, c.y}, {c.x + 22.0f, c.y});
        check(layout.nodeX[2] > x && model.value(matrixIndex(2, 1)) == 0.0, "a drag from inside a node moves it");
        view.undo();
        // Dragged onto another, it stops short of it, its outline 10 px from the other's; past it, it goes on.
        const Point bAt = nodeCenter(layout, 1);
        view.mouseDown({c.x - 8.0f, c.y}, {});
        view.mouseMove({bAt.x + 5.0f, bAt.y + 2.0f}, {});
        const Point held = nodeCenter(layout, 2);
        check(renderer.save(view, dir / "3-node-blocked.png"), "render a node held off another");
        view.mouseUp({bAt.x + 5.0f, bAt.y + 2.0f}, {});
        const float apart = std::hypot(held.x - bAt.x, held.y - bAt.y);
        view.undo();
        drag(view, {c.x - 8.0f, c.y}, {bAt.x + 100.0f, bAt.y});
        const float past = nodeCenter(layout, 2).x - (bAt.x + 100.0f);
        view.undo();
        std::printf("      held %.1f px from the other node, %.1f px off the mouse past it\n", apart, past);
        check(apart > 57.0f && apart < 59.0f && std::abs(past) < 0.5f,
              "a node dragged onto another is held off it, and goes on past it");
    }
    click(view, a);

    // Operator A gets a saw wave.
    click(view, waveButton(static_cast<int>(OpWave::Saw)));
    view.mouseMove(waveButton(static_cast<int>(OpWave::Saw)), {});
    check(renderer.save(view, dir / "3a-wave-name.png"), "render a wave button's name over the knobs");
    check(std::lround(model.value(paramIndex(0, OpParam::Wave))) == static_cast<long>(OpWave::Saw),
          "wave buttons set the selected operator's wave");

    // Unison, on its tab: 30 px up on a stepped knob is 3 steps, from 1 to 4 copies.
    click(view, opTabPoint(InspectorTab::Unison));
    check(layout.opTab == InspectorTab::Unison, "the UNISON tab shows the operator's unison");
    drag(view, kUnisonKnob, {kUnisonKnob.x, kUnisonKnob.y - 30.0f});
    check(model.value(paramIndex(0, OpParam::Unison)) == 4.0, "the unison knob steps through copy counts");
    drag(view, kPhaseKnob, {kPhaseKnob.x, kPhaseKnob.y - 100.0f});
    check(std::abs(model.value(paramIndex(0, OpParam::PhaseRandom)) - 0.5) < 1e-6,
          "the phase knob sets the operator's phase randomization");

    // The voice limit drags on a log scale: 80 px up doubles it twice, 16 to 64.
    drag(view, kVoicesKnob, {kVoicesKnob.x, kVoicesKnob.y - 80.0f});
    check(model.value(paramIndex(GlobalParam::Voices)) == 64.0, "the voices knob doubles every 40 px");
    view.mouseMove(kUnisonKnob, {});
    check(renderer.save(view, dir / "3b-unison.png"), "render an operator with unison");

    // Noise: the slope knob takes the ratio knob's place, and the unison row shows the spectrum.
    click(view, kNoiseButton);
    drag(view, kSlopeKnob, {kSlopeKnob.x, kSlopeKnob.y - 50.0f});
    check(model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Noise) &&
              model.value(paramIndex(0, OpParam::NoiseSlope)) > 0.0,
          "a noise operator's first knob sets its slope");
    view.mouseMove(kSlopeKnob, {});
    check(renderer.save(view, dir / "3b-noise.png"), "render a noise operator");
    model.set(paramIndex(0, OpParam::Wave), 0.0);
    model.set(paramIndex(0, OpParam::NoiseSlope), 0.0);

    // The SCOPE tab asks the audio thread for A's output over 2 cycles. With no note it draws the sine faintly; a
    // frame handed in through the telemetry is drawn at its level, across 2 cycles or, picked in its corner, 4; a
    // slow operator's span up to its cursor. Read off the rendered pixels: the trace's first peak, at a quarter of
    // the first cycle, 0.8 of the way up to the full-scale line.
    {
        const Telemetry before = model.live;
        click(view, opTabPoint(InspectorTab::Warp));
        view.poll();
        check(layout.opTab == InspectorTab::Warp && model.scopeOp == 0 && model.scopeCycles == 2,
              "the WARP tab asks for its operator's output");
        auto peakAt = [&](int cycles) { // the first peak: 8 px across the inner display, half its height up
            const Rect s = detail::kScopeRect;
            const float half = 0.5f * (s.h - 46.0f), middle = s.y + 26.0f + half;
            const float x = s.x + 8.0f + (s.w - 16.0f) * (0.25f / cycles);
            return ringLevel(renderer, {x, middle - 0.8f * half - 4.0f}, 8.0f);
        };
        auto frame = [&](int cycles, float cursor) {
            model.live.voices = 1;
            model.live.scopeBins = cycles * kScopeBinsPerCycle;
            model.live.scopeCursor = cursor;
            for (int bin = 0; bin < model.live.scopeBins; ++bin)
                model.live.scope[bin] =
                    0.8f * static_cast<float>(std::sin(2.0 * std::numbers::pi * bin / kScopeBinsPerCycle));
            view.poll();
        };
        model.live.voices = 0;
        model.live.scopeBins = 0;
        view.poll();
        view.mouseMove({600.0f, 328.0f}, {});
        const bool idleShot = renderer.save(view, dir / "3e-scope-idle.png");
        const int idle = peakAt(2);
        frame(2, -1.0f);
        const bool shot = renderer.save(view, dir / "3e-scope.png");
        const int traced = peakAt(2);
        click(view, scopeCycleButton(2));
        frame(4, -1.0f);
        const bool fourShot = renderer.save(view, dir / "3e-scope-4.png");
        const int four = peakAt(4);
        const bool asked = layout.scopeCycles == 4 && model.scopeCycles == 4;
        frame(4, 0.5f);
        check(renderer.save(view, dir / "3e-scope-filling.png"), "render a slow operator's scope as it fills");
        std::printf("      scope's first peak: %d with no note, %d over 2 cycles, %d over 4\n", idle, traced, four);
        check(idleShot && shot && fourShot && asked && traced > 150 && four > 150 && idle * 10 < traced * 6,
              "the scope draws the operator's output over the cycles picked");
        // A noise operator has no scope: its page shows the envelope, and nothing is captured.
        model.set(paramIndex(0, OpParam::Wave), static_cast<double>(OpWave::Noise));
        view.poll();
        check(model.scopeOp == -1, "a noise operator's page asks for no scope");
        model.set(paramIndex(0, OpParam::Wave), 0.0);
        // A hidden tab with a modulated knob shows a dot: LFO 1 on A's decay, on the ENVELOPE tab.
        const Rect envelopeTab = detail::opTab(static_cast<int>(InspectorTab::Envelope));
        const Point dot{envelopeTab.right() - 3.0f, envelopeTab.y + 5.0f};
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "3e-scope-tab-dot.png");
        const int plain = ringLevel(renderer, dot, 4.0f);
        model.set(slotTargetIndex(0, 7), slotTargetValue(paramIndex(0, OpParam::Decay)));
        model.set(slotAmountIndex(0, 7), 0.3);
        view.poll();
        const bool dotShot = renderer.save(view, dir / "3e-scope-tab-dot.png");
        const int lit = ringLevel(renderer, dot, 4.0f);
        model.set(slotTargetIndex(0, 7), 0.0);
        model.set(slotAmountIndex(0, 7), 0.0);
        std::printf("      ENVELOPE tab's corner: %d, %d with its decay modulated\n", plain, lit);
        check(dotShot && lit > plain + 60, "a hidden tab with a modulated knob shows a dot");
        // A source's handle dragged over a tab opens it; let go of in empty space, it assigns nothing.
        view.mouseDown(sourceHandle(0), {});
        view.mouseMove({sourceHandle(0).x + 40.0f, sourceHandle(0).y - 40.0f}, {});
        view.mouseMove(opTabPoint(InspectorTab::Unison), {});
        const bool opened = layout.opTab == InspectorTab::Unison;
        view.mouseUp({600.0f, 328.0f}, {});
        view.poll();
        check(opened && model.scopeOp == -1, "a dragged handle opens the tab it is held over, and the scope stops");
        model.live = before;
        click(view, opTabPoint(InspectorTab::Warp));
        click(view, scopeCycleButton(1));
        click(view, opTabPoint(InspectorTab::Envelope));
        view.poll();
        check(layout.opTab == InspectorTab::Envelope && layout.scopeCycles == 2, "the ENVELOPE tab is back");
    }

    // The WARP tab's warp: its buttons pick the warp, its knob the amount and NOTE (note sync) the hard sync, each
    // an undo step; with no note its display draws a square as the warp reads it, read off the rendered pixels: with
    // Squeeze at +50 % the first cycle's high half runs on past its middle (high at 60 % of it, where the plain square
    // is low), and synced to the note at a ratio of 1.5 the note's cycle restarts it.
    {
        const int warp = paramIndex(0, OpParam::Warp), amount = paramIndex(0, OpParam::WarpAmount),
                  sync = paramIndex(0, OpParam::Sync);
        model.set(paramIndex(0, OpParam::Wave), static_cast<double>(OpWave::Square));
        click(view, opTabPoint(InspectorTab::Warp));
        view.poll();
        const bool shown = layout.opTab == InspectorTab::Warp;
        // Where along the display's two cycles (0..2) the trace is lit at the full-scale line, top or bottom.
        auto litAt = [&](Rect inner, float cycles, float at, bool top) {
            const float x = inner.x + inner.w * at / cycles;
            return ringLevel(renderer, {x, (top ? inner.y : inner.bottom()) - 3.0f}, 6.0f);
        };
        const Rect r = detail::kScopeRect;
        const Rect inner{r.x + 8.0f, r.y + 26.0f, r.w - 16.0f, r.h - 46.0f};
        view.mouseMove({600.0f, 328.0f}, {});
        renderer.save(view, dir / "3w-warp-off.png");
        const int plainHigh = litAt(inner, 2.0f, 0.6f, true), plainLow = litAt(inner, 2.0f, 0.6f, false);
        click(view, detail::warpButton(static_cast<int>(OpWarp::Squeeze)).center());
        const bool picked = std::lround(model.value(warp)) == static_cast<long>(OpWarp::Squeeze);
        drag(view, detail::kWarpAmountKnob, {detail::kWarpAmountKnob.x, detail::kWarpAmountKnob.y - 40.0f});
        const bool turned = model.value(amount) > 0.05;
        model.set(amount, 0.5);
        view.poll();
        view.mouseMove({600.0f, 328.0f}, {});
        const bool squeezedShot = renderer.save(view, dir / "3w-warp-squeeze.png");
        const int squeezedHigh = litAt(inner, 2.0f, 0.6f, true), squeezedLow = litAt(inner, 2.0f, 0.6f, false);
        std::printf("      a square at 60 %% of its cycle: high %d, low %d plain; high %d, low %d squeezed\n",
                    plainHigh, plainLow, squeezedHigh, squeezedLow);
        // The plain square is drawn faintly, a stand-in for the trace; the warped one in the accent.
        check(shown && picked && turned && squeezedShot && plainLow > plainHigh + 30 &&
                  squeezedHigh > squeezedLow + 60,
              "the WARP tab picks a warp and its amount, and its display draws the square's pulse width");
        view.mouseMove(detail::warpButton(static_cast<int>(OpWarp::Sync)).center(), {});
        check(renderer.save(view, dir / "3w-warp-tip.png"), "render a warp button's tooltip");
        // The Sync warp (the wave played through more times a cycle, cut off at its end) is a warp like the others,
        // apart from the NOTE switch: picked, undone, Squeeze back.
        click(view, detail::warpButton(static_cast<int>(OpWarp::Sync)).center());
        view.mouseMove({600.0f, 328.0f}, {});
        const bool syncWarpShot = renderer.save(view, dir / "3w-warp-sync-warp.png");
        const bool syncWarp = std::lround(model.value(warp)) == static_cast<long>(OpWarp::Sync) &&
                              model.value(sync) == 0.0;
        view.undo();
        check(syncWarpShot && syncWarp && std::lround(model.value(warp)) == static_cast<long>(OpWarp::Squeeze),
              "the Sync warp is picked apart from the NOTE switch, undoably");
        // NOTE: on, the summary says so and the display shows the note's cycles; then each step undone.
        model.set(paramIndex(0, OpParam::Ratio), normFromRatio(1.5));
        click(view, detail::kSyncButton.center());
        const bool synced = model.value(sync) == 1.0;
        view.mouseMove({600.0f, 328.0f}, {});
        check(renderer.save(view, dir / "3w-warp-sync.png") && synced, "the NOTE switch turns note sync on");
        view.undo(); // sync
        const bool syncUndone = model.value(sync) == 0.0;
        model.set(paramIndex(0, OpParam::Ratio), normFromRatio(1.0));
        view.undo(); // the amount's drag
        view.undo(); // the warp
        check(syncUndone && model.value(amount) == 0.0 && model.value(warp) == 0.0,
              "the warp, its amount and the NOTE switch are undone a step each");
        // A noise operator has no wave to show or warp: its page shows the envelope in the WARP tab's place.
        model.set(paramIndex(0, OpParam::Wave), static_cast<double>(OpWave::Noise));
        view.poll();
        view.mouseMove({600.0f, 328.0f}, {});
        check(renderer.save(view, dir / "3w-warp-noise.png"), "render a noise operator with the WARP tab picked");
        model.set(paramIndex(0, OpParam::Wave), 0.0);
        click(view, opTabPoint(InspectorTab::Envelope));
        view.poll();
        check(layout.opTab == InspectorTab::Envelope, "the ENVELOPE tab is back after WARP");
    }

    // While notes play, the ENVELOPE tab draws a line across at the newest voice's envelope level, as a mod
    // envelope's card does. Read off the rendered pixels: across the sustain's stretch, the row at a level of 0.5
    // lights up and the row at 0.25 doesn't.
    {
        const Telemetry before = model.live;
        const float top = kOpEnvTop + 12.0f, bottom = kOpEnvBottom - 10.0f; // the envelope's full and empty levels
        auto rowAt = [&](float level) { // the brightest pixel along that row, by its strongest channel
            const float y = std::round(bottom - level * (bottom - top));
            int most = 0;
            for (float x = 1000.0f + kInsDx; x <= 1100.0f + kInsDx; x += 1.0f) {
                const uint32_t color = renderer.pixel({x, y});
                most = std::max({most, static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                                 static_cast<int>(color & 255)});
            }
            return most;
        };
        auto shot = [&](int voices, const char *name) {
            model.live.voices = voices;
            model.live.opEnvLevels[0] = 0.5f;
            view.poll();
            view.mouseMove({600.0f, 328.0f}, {});
            renderer.save(view, dir / name);
            return std::pair{rowAt(0.5f), rowAt(0.25f)};
        };
        const auto [idleHalf, idleQuarter] = shot(0, "3f-env-level-idle.png");
        const auto [half, quarter] = shot(1, "3f-env-level.png");
        std::printf("      envelope's level line: %d -> %d at 0.5, %d -> %d at 0.25\n", idleHalf, half, idleQuarter,
                    quarter);
        check(half > idleHalf + 25 && std::abs(quarter - idleQuarter) < 6,
              "an operator's envelope shows the newest voice's level as a line across");
        model.live = before;
        view.poll();
    }

    // And a dot on the curve, as far along the stage the envelope is in as it has come: on A's ENVELOPE tab and on
    // Env 1's card. Read off the rendered pixels: where a region turned whitest (by its weakest channel) since a
    // render with no note is the dot, and the curve, rendered with no note, passes within 3 px of it. A note let go
    // during the decay releases from above the sustain, where the curve doesn't go: its dot keeps to the curve,
    // below the line at its level.
    {
        const Telemetry before = model.live;
        const int attackTime = paramIndex(0, OpParam::Attack), sustainLevel = paramIndex(0, OpParam::Sustain);
        const double attackWas = model.value(attackTime), sustainWas = model.value(sustainLevel);
        model.set(attackTime, 0.5); // a slope the dot can be seen on, and a decay that falls
        model.set(sustainLevel, 0.3);
        struct Region {
            int left, top, w, h;
        };
        auto capture = [&](Region g, const char *name) {
            view.poll();
            view.mouseMove({600.0f, 328.0f}, {});
            renderer.save(view, dir / name);
            std::vector<uint32_t> pixels(static_cast<size_t>(g.w) * g.h);
            for (int y = 0; y < g.h; ++y)
                for (int x = 0; x < g.w; ++x)
                    pixels[y * g.w + x] =
                        renderer.pixel({static_cast<float>(g.left + x), static_cast<float>(g.top + y)});
            return pixels;
        };
        auto white = [](uint32_t color) {
            return std::min({static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                             static_cast<int>(color & 255)});
        };
        auto bright = [](uint32_t color) {
            return std::max({static_cast<int>(color >> 16 & 255), static_cast<int>(color >> 8 & 255),
                             static_cast<int>(color & 255)});
        };
        struct Dot {
            int x = 0, y = 0, gain = 0;
            bool onCurve = false;
        };
        auto find = [&](Region g, const std::vector<uint32_t> &idle, const std::vector<uint32_t> &shown) {
            Dot dot; // its middle: the middle of the pixels that turned at least half as white as the most
            for (int i = 0; i < g.w * g.h; ++i)
                dot.gain = std::max(dot.gain, white(shown[i]) - white(idle[i]));
            double sumX = 0.0, sumY = 0.0;
            int count = 0;
            for (int i = 0; i < g.w * g.h; ++i)
                if (2 * (white(shown[i]) - white(idle[i])) >= dot.gain) {
                    sumX += i % g.w;
                    sumY += i / g.w;
                    ++count;
                }
            if (count > 0) {
                dot.x = static_cast<int>(std::lround(sumX / count));
                dot.y = static_cast<int>(std::lround(sumY / count));
            }
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const int x = dot.x + dx, y = dot.y + dy;
                    if (x >= 0 && x < g.w && y >= 0 && y < g.h && bright(idle[y * g.w + x]) > 120)
                        dot.onCurve = true;
                }
            dot.x += g.left;
            dot.y += g.top;
            return dot;
        };
        // A: half way along its attack, decay and release; then just into a release from 0.8, above its sustain of
        // 0.3.
        const Region op{896 + static_cast<int>(kInsDx), static_cast<int>(kOpEnvTop), 276, static_cast<int>(kOpEnvBottom - kOpEnvTop)};
        const float top = kOpEnvTop + 12.0f, bottom = kOpEnvBottom - 10.0f;
        model.live.voices = 0;
        const std::vector<uint32_t> opIdle = capture(op, "3f-env-dot-idle.png");
        auto opDot = [&](float position, float level, const char *name) {
            model.live.voices = 1;
            model.live.opEnvPos[0] = position;
            model.live.opEnvLevels[0] = level;
            return find(op, opIdle, capture(op, name));
        };
        const Dot attack = opDot(0.5f, 0.5f, "3f-env-dot-attack.png");
        const Dot decay = opDot(2.5f, 0.4f, "3f-env-dot-decay.png");
        const Dot release = opDot(4.5f, 0.1f, "3f-env-dot.png");
        const Dot early = opDot(4.05f, 0.8f, "3f-env-dot-early-release.png");
        const float lineY = bottom - 0.8f * (bottom - top);
        std::printf("      envelope's dot: (%d, %d) in the attack, (%d, %d) in the decay, (%d, %d) in the release, "
                    "(%d, %d) releasing from above the sustain (its line at %.0f), lighter by %d, %d, %d, %d\n",
                    attack.x, attack.y, decay.x, decay.y, release.x, release.y, early.x, early.y, lineY, attack.gain,
                    decay.gain, release.gain, early.gain);
        check(attack.gain > 60 && decay.gain > 60 && release.gain > 60 && attack.onCurve && decay.onCurve &&
                  release.onCurve && attack.x + 20 < decay.x && decay.x + 20 < release.x,
              "an operator's envelope shows a dot on its curve, along the stage it is in");
        check(early.gain > 60 && early.onCurve && early.y > lineY + 20.0f,
              "a release from above the sustain keeps its dot on the curve");
        // Env 1's card, half way up its attack.
        const Region card{318, 600, 276, 100};
        model.live = before;
        model.live.voices = 0;
        const std::vector<uint32_t> cardIdle = capture(card, "3f-env-card-dot-idle.png");
        model.live.voices = 1;
        model.live.sources[1] = 0.5f;
        model.live.envPos[1] = 0.5f;
        const Dot cardDot = find(card, cardIdle, capture(card, "3f-env-card-dot.png"));
        std::printf("      Env 1's dot: (%d, %d), lighter by %d\n", cardDot.x, cardDot.y, cardDot.gain);
        check(cardDot.gain > 60 && cardDot.onCurve, "a mod envelope's card shows a dot on its curve");
        model.set(attackTime, attackWas);
        model.set(sustainLevel, sustainWas);
        model.live = before;
        view.poll();
    }

    // Fixed pitch: the keyboard on A's summary line switches it, setting Freq and Mult to where the ratio put A4 in
    // one undo step. Freq and Mult take the Ratio readout's place (Mult stepping, 10 px up a step), and Pitch and
    // Detune, dimmed, don't turn (read off the rendered pixels: the Pitch knob's column darkens).
    {
        const Point kFixedSwitch = detail::kFixedSwitch.center();
        const Point pitchColumn{kPitchKnob.x, 226.0f + kInsDy};
        const int fixed = paramIndex(0, OpParam::Fixed), freq = paramIndex(0, OpParam::FixedFreq),
                  mult = paramIndex(0, OpParam::FixedMult);
        const double tracked = 440.0 * ratioFromNorm(model.value(paramIndex(0, OpParam::Ratio)));
        view.mouseMove({600.0f, 328.0f}, {});
        const bool trackingShot = renderer.save(view, dir / "3f-tracking.png");
        const int lit = ringLevel(renderer, pitchColumn, 32.0f);
        click(view, kFixedSwitch);
        const double fixedAt = fixedHz(model.value(freq), model.value(mult));
        const bool set = model.value(fixed) == 1.0 && model.value(mult) == 3.0 && std::abs(fixedAt - tracked) < 0.01;
        // Mult reads "×1", its × two bytes in UTF-8: typed into, the caret steps over it whole, and Backspace and
        // Delete take all of it, so "10" is left to read as a value either way.
        {
            const Point multNumber{kMultKnob.x, 266.0f + kInsDy};
            doubleClick(view, multNumber);
            for (View::Key key : {View::Key::End, View::Key::Left, View::Key::Backspace, View::Key::End})
                view.typeKey(key);
            type(view, "0");
            view.typeKey(View::Key::Enter);
            const bool backspaced = model.value(mult) == 4.0;
            view.undo();
            doubleClick(view, multNumber);
            for (View::Key key : {View::Key::Home, View::Key::Delete, View::Key::End})
                view.typeKey(key);
            type(view, "0");
            view.typeKey(View::Key::Enter);
            const bool deleted = model.value(mult) == 4.0;
            view.undo();
            check(backspaced && deleted && model.value(mult) == 3.0,
                  "typing into Mult steps over its × whole, Backspace and Delete taking all of it");
        }
        // A source's handle held meanwhile rings the knobs it can be dropped on, Freq among them, but not the dimmed
        // Pitch, which takes no drop (read off the rendered pixels: a strip across the top of each ring, with the
        // handle held and after the drag was called off).
        {
            const Point freqKnob{kMultKnob.x - 45.0f, kMultKnob.y};
            auto strip = [&](Point knob) {
                std::vector<uint32_t> pixels;
                for (float y = knob.y - 34.0f; y <= knob.y - 14.0f; y += 1.0f)
                    pixels.push_back(renderer.pixel({knob.x, y}));
                return pixels;
            };
            view.mouseMove({600.0f, 328.0f}, {}); // nothing under the mouse lit, held or not
            view.mouseDown(sourceHandle(1), {});
            view.mouseMove({sourceHandle(1).x + 40.0f, sourceHandle(1).y - 40.0f}, {});
            view.mouseMove({600.0f, 328.0f}, {});
            const bool heldShot = renderer.save(view, dir / "3f-fixed-drop-targets.png");
            const std::vector<uint32_t> freqHeld = strip(freqKnob), pitchHeld = strip(kPitchKnob);
            view.cancelDrag();
            view.mouseMove({600.0f, 328.0f}, {});
            const bool freeShot = renderer.save(view, dir / "3f-fixed-no-drag.png");
            check(heldShot && freeShot && freqHeld != strip(freqKnob) && pitchHeld == strip(kPitchKnob),
                  "a dimmed knob isn't ringed as a drop target");
        }
        drag(view, kMultKnob, {kMultKnob.x, kMultKnob.y - 10.0f}); // Mult, in the readout's right half: x1 to x10
        const double pitch = model.value(paramIndex(0, OpParam::Transpose));
        drag(view, kPitchKnob, {kPitchKnob.x, kPitchKnob.y - 30.0f});
        view.mouseMove({600.0f, 328.0f}, {});
        const bool fixedShot = renderer.save(view, dir / "3f-fixed.png");
        const int dim = ringLevel(renderer, pitchColumn, 32.0f);
        view.mouseMove(kFixedSwitch, {});
        check(renderer.save(view, dir / "3f-fixed-tooltip.png"), "render the fixed switch's tooltip");
        const bool stepped = model.value(mult) == 4.0;
        const bool still = model.value(paramIndex(0, OpParam::Transpose)) == pitch;
        view.undo();
        const bool multBack = model.value(mult) == 3.0 && model.value(fixed) == 1.0;
        view.undo();
        std::printf("      A fixed at %.1f Hz (its ratio's at A4: %.1f); Pitch knob's column %d tracking, %d fixed\n",
                    fixedAt, tracked, lit, dim);
        check(trackingShot && fixedShot && set && stepped && still && multBack && model.value(fixed) == 0.0 &&
                  dim * 10 < lit * 7,
              "the keyboard switch fixes an operator's pitch where its ratio was, its knobs becoming Freq and Mult");
        // The wheel over the switch does what a click does: up, it fixes the operator where its ratio was; up again,
        // nothing more; down, it follows the keyboard again.
        view.wheel(kFixedSwitch, 1.0f, {});
        const bool wheeledOn = model.value(fixed) == 1.0 && model.value(mult) == 3.0 &&
                               std::abs(fixedHz(model.value(freq), model.value(mult)) - tracked) < 0.01;
        view.wheel(kFixedSwitch, 1.0f, {});
        const bool stays = model.value(fixed) == 1.0;
        view.wheel(kFixedSwitch, -1.0f, {});
        const bool wheeledOff = model.value(fixed) == 0.0;
        view.undo();
        view.undo();
        check(wheeledOn && stays && wheeledOff && model.value(fixed) == 0.0,
              "the wheel over the keyboard switch fixes an operator as a click does");
    }

    // Play switches: Mono on, Legato on (it starts off), off and on again, Glide half way up its range (100 px up),
    // Always on.
    click(view, kMonoButton);
    click(view, kLegatoToggle);
    click(view, kLegatoToggle);
    const bool legatoFlipped = model.value(paramIndex(GlobalParam::Legato)) == 0.0;
    click(view, kLegatoToggle);
    drag(view, kGlideKnob, {kGlideKnob.x, kGlideKnob.y - 100.0f});
    click(view, kAlwaysToggle);
    check(model.value(paramIndex(GlobalParam::Mono)) == 1.0 && legatoFlipped &&
              model.value(paramIndex(GlobalParam::Legato)) == 1.0 &&
              std::abs(model.value(paramIndex(GlobalParam::Glide)) - 0.5 * kMaxGlideNorm) < 1e-6 &&
              model.value(paramIndex(GlobalParam::GlideAlways)) == 1.0,
          "play switches and the glide knob set their params");
    view.mouseMove(kLegatoToggle, {});
    check(renderer.save(view, dir / "3c-play.png"), "render the play controls with a tooltip");

    // Switch filter 1 on and route A through it: A's output moves into the filter.
    click(view, nodeTab(kOps));
    click(view, nodeCenter(layout, 0)); // A selected again, so the F1 tab below selects rather than switches
    view.mouseMove({400.0f, 328.0f}, {});
    renderer.save(view, dir / "3d-filter-unfed.png");
    const uint32_t unfed = tabDot(renderer, kOps);
    // Like an operator or a shaper, it starts routed nowhere; its edge dragged onto the bus sends it out, with its
    // line to OUT, though nothing goes into it, and that line's arrowhead let go of in empty space takes it away.
    const int f1Out = paramIndex(0, FilterParam::Out);
    const bool startsNowhere = model.value(f1Out) == 0.0;
    drag(view, portOf(layout, kOps), kOutCore);
    view.mouseMove({400.0f, 328.0f}, {});
    const bool routedOut = model.value(f1Out) == 1.0 && renderer.save(view, dir / "3d-filter-out.png");
    drag(view, outArrowOf(layout, kOps), {kOutCore.x, kOutCore.y + 110.0f});
    check(startsNowhere && routedOut && model.value(f1Out) == 0.0,
          "a filter goes out by its edge dragged onto the bus, and stops by its arrowhead dragged away");
    drag(view, portOf(layout, 0), nodeCenter(layout, kOps));
    view.mouseMove({400.0f, 328.0f}, {});
    renderer.save(view, dir / "3e-filter-fed.png");
    const uint32_t fed = tabDot(renderer, kOps);
    check(model.value(sendIndex(0, 0)) == 1.0 && model.value(paramIndex(0, OpParam::Out)) == 0.0 &&
              model.value(f1Out) == 1.0,
          "connecting a carrier to a filter routes its output through the filter, which goes out in its place");
    // The F1 tab then has the dot that says it goes out, in the filters' purple, as an operator's tab has.
    check((unfed & 255) < 120 && (fed & 255) > 200 && (fed >> 8 & 255) < 170,
          "a filter's tab shows a dot while it sounds out, as an operator's does");

    // Filter 1 in the inspector: switch types, drag the response handle to lower the cutoff.
    click(view, kF1Tab);
    check(layout.selectedNode == kOps, "the F1 tab selects filter 1");
    const Point f2Tab{kF1Tab.x + 26.0f, kF1Tab.y};
    click(view, f2Tab);
    check(model.value(paramIndex(1, FilterParam::Enabled)) == 1.0 && layout.selectedNode == kOps + 1,
          "clicking a switched-off filter's tab switches it on");
    model.set(paramIndex(1, FilterParam::Enabled), 0.0);
    click(view, kF1Tab);
    click(view, typeButton(static_cast<int>(FilterType::Comb)));
    check(std::lround(model.value(paramIndex(0, FilterParam::Type))) == static_cast<long>(FilterType::Comb),
          "type buttons set the filter type");
    click(view, typeButton(static_cast<int>(FilterType::LowPass)));
    // The slope's list: a low-pass comes in 6 to 24 dB per octave; a band-pass only in 12 and 24, the others faint
    // in its list, where a click on one leaves the list open.
    const int slope = paramIndex(0, FilterParam::Slope);
    click(view, kSlopeButton);
    view.mouseMove(slopeItem(3), {});
    const bool listShot = renderer.save(view, dir / "4a-slope-list.png");
    click(view, slopeItem(3));
    const bool steep = model.value(slope) == 3.0;
    click(view, kSlopeButton);
    click(view, slopeItem(0));
    const bool gentle = model.value(slope) == 0.0;
    click(view, typeButton(static_cast<int>(FilterType::BandPass)));
    click(view, kSlopeButton);
    view.mouseMove(slopeItem(2), {});
    const bool bandListShot = renderer.save(view, dir / "4b-slope-list-band.png");
    // The hand only over an entry it can pick: not over 18 dB, the list's own edge or around the list.
    const bool refusedArrow = view.cursor() == CursorKind::Arrow;
    view.mouseMove(slopeItem(3), {});
    const bool pickHand = view.cursor() == CursorKind::Hand;
    const Rect slopeList = detail::listMenu(detail::kFilterSlopeButton, kFilterSlopes);
    view.mouseMove({slopeList.center().x, slopeList.y + 2.0f}, {});
    const bool edgeArrow = view.cursor() == CursorKind::Arrow;
    view.mouseMove({slopeList.center().x, slopeList.bottom() + 12.0f}, {});
    const bool aroundArrow = view.cursor() == CursorKind::Arrow;
    check(refusedArrow && pickHand && edgeArrow && aroundArrow,
          "an open list shows the hand only over an entry it can pick");
    click(view, slopeItem(2));
    const bool refused = model.value(slope) == 0.0;
    click(view, slopeItem(3)); // the list is still open
    const bool bandSteep = model.value(slope) == 3.0;
    click(view, typeButton(static_cast<int>(FilterType::LowPass)));
    model.set(slope, kDefaultSlope);
    check(listShot && steep && gentle && bandListShot && refused && bandSteep,
          "the slope's list picks 6 to 24 dB, a band-pass's only 12 and 24");
    const int cutoff = paramIndex(0, FilterParam::Cutoff);
    const double cutoffBefore = model.value(cutoff);
    const Rect response = detail::kFilterDisplay; // its handle as View places it, 10 px inside the display
    const Point handle{response.x + 10.0f + static_cast<float>(cutoffBefore) * (response.w - 20.0f),
                       response.bottom() - 10.0f -
                           static_cast<float>(model.value(paramIndex(0, FilterParam::Resonance))) * (response.h - 20.0f)};
    drag(view, handle, {handle.x - 50.0f, handle.y - 20.0f});
    check(model.value(cutoff) < cutoffBefore - 0.15, "dragging the response handle moves the cutoff");
    // Its last knob is Mix, where Out was (the line to OUT's label sets that): turned down, what goes in shows through
    // the response, lifting it past the cutoff (read off the rendered pixels), and undone it is all filter again.
    {
        const int mix = paramIndex(0, FilterParam::Mix);
        const Point mixKnob = ins(884.0f + 34.0f + 4.0f * 58.0f, 242.0f);
        // The display's right half, well past the cutoff, where the response lifts.
        auto pastCutoff = [&] {
            std::vector<uint32_t> pixels;
            for (float y = response.y + 10.0f; y < response.bottom() - 10.0f; y += 1.0f)
                for (float x = response.center().x; x < response.right() - 10.0f; x += 1.0f)
                    pixels.push_back(renderer.pixel({x, y}));
            return pixels;
        };
        view.mouseMove({1000.0f, 148.0f}, {});
        renderer.save(view, dir / "4-filter-mix-full.png");
        const std::vector<uint32_t> full = pastCutoff();
        drag(view, mixKnob, {mixKnob.x, mixKnob.y + 100.0f});
        const double turned = model.value(mix);
        view.mouseMove({1000.0f, 148.0f}, {});
        renderer.save(view, dir / "4-filter-mix.png");
        const std::vector<uint32_t> mixed = pastCutoff();
        view.undo();
        view.mouseMove({1000.0f, 148.0f}, {});
        renderer.save(view, dir / "4-filter-mix-undone.png");
        int moved = 0, back = 0;
        const std::vector<uint32_t> undone = pastCutoff();
        for (size_t i = 0; i < full.size(); ++i) {
            moved += full[i] != mixed[i];
            back += full[i] != undone[i];
        }
        std::printf("      the filter's Mix at %.0f %%: %d pixels of its response past the cutoff change\n",
                    turned * 100.0, moved);
        check(model.value(mix) == 1.0 && turned < 0.8 && moved > 300 && back == 0,
              "a filter's Mix knob blends what goes in back in, its response following it");
    }
    model.set(paramIndex(0, FilterParam::Resonance), 0.55);
    // Behind the response, what the filter puts out, measured: a 220 Hz saw's harmonics, falling away past the
    // cutoff.
    const double cutoffHz = cutoffFromNorm(model.value(cutoff));
    for (int band = 0; band < kSpectrumBands; ++band) {
        const double hz = cutoffFromNorm((band + 0.5) / kSpectrumBands), over = hz / cutoffHz;
        model.live.filterSpectrum[0][band] = hz < 200.0 ? 1e-3f : static_cast<float>(0.5 * 220.0 / hz / (1.0 + over * over));
    }
    view.poll();
    view.mouseMove({1000.0f, 148.0f}, {});
    check(renderer.save(view, dir / "4-filter.png"), "render the filter inspector, with what it puts out behind");
    model.live.filterSpectrum = {};
    view.poll();

    // A busier patch with notes playing.
    model.set(paramIndex(5, OpParam::Enabled), 1.0); // F
    model.set(paramIndex(1, FilterParam::Enabled), 1.0);
    model.set(matrixIndex(3, 2), 0.35);
    model.set(matrixIndex(3, 3), 0.15);
    model.set(paramIndex(2, OpParam::Out), 0.0);
    model.set(sendIndex(2, 1), 0.8);
    model.set(filterLinkIndex(0, 1), 0.5);
    model.set(filterModIndex(1, 4), 0.4); // F2 modulates E
    model.set(paramIndex(4, OpParam::Out), 0.6);
    model.set(paramIndex(4, OpParam::Wave), static_cast<double>(OpWave::HalfSine));
    model.set(matrixIndex(5, 4), 0.45);
    model.set(paramIndex(5, OpParam::Ratio), normFromRatio(3.0));
    addSource(view, SourceType::Lfo); // LFO 2, in place 2
    model.set(paramIndex(2, SourceParam::Shape), 1.0);
    model.set(slotTargetIndex(2, 0), slotTargetValue(paramIndex(GlobalParam::Pan)));
    model.set(slotAmountIndex(2, 0), 0.4);
    model.ui.selectedNode = 1;
    // Bent envelope segments: B's attack rushes in and its decay lingers, Env 1's release starts slow.
    model.set(paramIndex(1, OpParam::Attack), 0.3);
    model.set(paramIndex(1, OpParam::Sustain), 0.4);
    model.set(paramIndex(1, OpParam::AttackCurve), 0.6);
    model.set(paramIndex(1, OpParam::DecayCurve), -0.4);
    model.set(paramIndex(1, SourceParam::ReleaseCurve), -0.6);
    model.live.voices = 3;
    model.live.opLevels = {0.9f, 0.6f, 0.4f, 0.7f, 0.5f, 0.3f};
    model.live.sources = {0.4f, 0.7f, -0.6f};
    model.live.lfoPhases = {0.1f, 0.0f, 0.6f};
    model.live.modOffset[fmAmount] = 0.25f;
    model.live.modOffset[paramIndex(GlobalParam::Pan)] = -0.3f;
    model.live.modOffset[matrixIndex(1, 0)] = 0.2f; // Env 1 on the B -> A wire: it swells, its label shows where
    for (int key : {48, 52, 55}) // the host holds a C major chord, which lights up on the keyboard
        model.live.keys[key / 32] |= 1u << (key % 32);
    view.poll();
    view.mouseMove(kDecayKnob, {}); // hover Op B's decay knob
    check(renderer.save(view, dir / "5-patch.png"), "render a busier patch");
    // Unwired, F still plays but isn't heard, so it stays dark.
    model.set(matrixIndex(5, 4), 0.0);
    model.ui.keptRoutes.clear();
    check(renderer.save(view, dir / "5a-unheard.png"), "render an operator that isn't heard");
    model.set(matrixIndex(5, 4), 0.45);
    // The Routing tab shown runs on down the panel's left edge, its corner square; the matrix's is round.
    auto cornerSquare = [&] { return renderer.pixel({18.0f, 134.0f}) == renderer.pixel({18.0f, 150.0f}); };
    const bool routingCorner = cornerSquare();

    // The same routing as a matrix: double-click toggles C -> F2 off.
    click(view, kMatrixTab);
    check(layout.view == RoutingView::Matrix, "the Matrix tab switches the routing panel to the grid");
    view.mouseMove(gridCellCenter(0, kOps), {});
    check(renderer.save(view, dir / "6-matrix.png"), "render the matrix view");
    // The tab shown opens the panel's top edge under it, the edge running on under the others; one under the mouse
    // shows a tab's shape.
    // (Read above the inspector's lens, which floats 6 px under the panel's top.)
    auto edgeOpen = [&](Point tab) { return renderer.pixel({tab.x, 132.0f}) == renderer.pixel({tab.x, 135.0f}); };
    const bool tabJoined = edgeOpen(kMatrixTab) && !edgeOpen(kRoutingTab) && !edgeOpen(kFxTab);
    check(routingCorner && !cornerSquare(), "the Routing tab shown runs on down its panel's left edge");
    const uint32_t fxTabAlone = renderer.pixel({kFxTab.x, 104.0f});
    view.mouseMove(kFxTab, {});
    check(tabJoined && renderer.save(view, dir / "6a-view-tab-hovered.png") &&
              renderer.pixel({kFxTab.x, 104.0f}) != fxTabAlone,
          "the tab shown opens its panel's edge, and a hovered one shows a tab's shape");
    view.doubleClick({gridCellCenter(1, 0).x, gridCellCenter(1, 0).y + 10.0f}, {}); // Env 1's dot on B -> A, centered
    check(slotTarget(model.value(slotTargetIndex(1, 1))) < 0 &&
              slotTarget(model.value(slotTargetIndex(1, 0))) == fmAmount,
          "double-clicking a modulation dot removes that modulation");
    view.doubleClick(gridCellCenter(2, kOps + 1), {});
    check(model.value(sendIndex(2, 1)) == 0.0, "double-clicking a matrix cell toggles that route");
    click(view, kRoutingTab);
    check(layout.view == RoutingView::Graph, "the Routing tab switches back to the graph");

    // Right-click an operator to switch it off; it leaves the graph and its tab dims.
    const auto nodeMenu = view.contextMenu(nodeCenter(layout, 5));
    if (!nodeMenu.empty())
        view.menuCommand(nodeMenu.front().command);
    check(model.value(paramIndex(5, OpParam::Enabled)) == 0.0, "an operator's menu switches it off");
    // F and the shapers, which are off, wait docked under OUT as dashed ghosts (F hovered here).
    const Point ghostF = ghostOf(model, 5);
    view.mouseMove(ghostF, {});
    check(renderer.save(view, dir / "7-switched-off.png"), "render after switching F off");
    const int selectedBefore = layout.selectedNode;
    click(view, kOpFTab);
    check(model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && layout.selectedNode == 5,
          "clicking a switched-off operator's tab switches it on and selects it");
    model.set(paramIndex(5, OpParam::Enabled), 0.0);
    model.ui.selectedNode = selectedBefore;
    click(view, ghostF);
    check(model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && layout.selectedNode == 5,
          "an operator's ghost in the graph switches it on and selects it");
    model.set(paramIndex(5, OpParam::Enabled), 0.0);
    model.ui.selectedNode = selectedBefore;

    // Shaper 1: switch it on with its tab, send A into it by dragging A's port, then run it onto B, into F1 and onto
    // the OUT bus, as any node goes out. F1 feeds it back, round in a loop. F2 feeds the earlier F1, and F1 itself
    // by its edge dragged back onto it, at 25 % as an operator's feedback starts (each undone again).
    check(kNodes == kOps + 4, "the graph has six operators, two filters and two shapers");
    const int s1 = kOps + kFilters;
    click(view, nodeTab(s1));
    view.mouseMove({400.0f, 328.0f}, {});
    renderer.save(view, dir / "7a-shaper-on.png");
    const uint32_t shaperAlone = tabDot(renderer, s1); // its tab, selected, red
    check(model.value(paramIndex(0, ShaperParam::Enabled)) == 1.0 && layout.selectedNode == s1,
          "the S1 tab switches shaper 1 on and shows it");
    drag(view, portOf(layout, 0), nodeCenter(layout, s1));
    check(model.value(shaperSendIndex(0, 0)) == 1.0, "dragging A's port onto S1 sends A into shaper 1");
    drag(view, portOf(layout, s1), nodeCenter(layout, 1));
    drag(view, portOf(layout, s1), nodeCenter(layout, kOps));
    check(model.value(shaperModIndex(0, 1)) == 0.3 && model.value(shaperFilterIndex(0, 0)) == 1.0,
          "dragging S1's port onto B makes it modulate B, and onto F1 feeds it into filter 1");
    // (From their boxes' bottom edges here: on their right edges the wires to F1 and to F2 now leave them.)
    auto bottomEdge = [&](int node) { return Point{nodeCenter(layout, node).x, nodeCenter(layout, node).y + 17.0f}; };
    drag(view, bottomEdge(kOps), nodeCenter(layout, s1));
    drag(view, portOf(layout, kOps + 1), nodeCenter(layout, kOps));
    const bool backward = model.value(filterLinkIndex(1, 0)) > 0.0;
    view.undo();
    drag(view, portOf(layout, kOps), nodeCenter(layout, kOps));
    const bool itself = model.value(filterLinkIndex(0, 0)) == 0.25;
    view.undo();
    check(backward && itself && model.value(filterLinkIndex(1, 0)) == 0.0 && model.value(filterLinkIndex(0, 0)) == 0.0,
          "a filter's edge dragged onto an earlier filter feeds it, and onto itself feeds it back");
    drag(view, bottomEdge(s1), kOutCore); // onto OUT
    check(model.value(filterLinkIndex(0, 1)) == 0.5 && model.value(shaperModIndex(0, 1)) == 0.3 &&
              model.value(shaperFilterIndex(0, 0)) == 1.0 && model.value(shaperSendIndex(0, 0)) == 1.0 &&
              model.value(filterShaperIndex(0, 0)) > 0.0 && model.value(paramIndex(0, ShaperParam::Out)) == 1.0,
          "a filter feeds a shaper, and a shaper goes out onto the bus");
    // Dragged from its edge back onto itself, S1 feeds itself back, at 25 % as an operator does, still going out.
    drag(view, bottomEdge(s1), nodeCenter(layout, s1));
    check(model.value(shaperShaperIndex(0, 0)) == 0.25 && model.value(paramIndex(0, ShaperParam::Out)) == 1.0,
          "a shaper's edge dragged onto itself feeds it back into itself");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "7b-shapers.png"), "render a shaper in the graph, and its page");
    // Fed by A and going out, S1's tab (still selected) has the dot, dark on its red.
    check((shaperAlone >> 16 & 255) > 150 && (tabDot(renderer, s1) >> 16 & 255) < 80,
          "a shaper's tab shows a dot while it sounds out");
    click(view, kMatrixTab);
    check(renderer.save(view, dir / "7c-shapers-matrix.png"), "render the matrix with F switched off");
    model.set(shaperShaperIndex(0, 0), 0.0);
    std::erase(model.ui.keptRoutes, shaperShaperIndex(0, 0));
    // There a name works like its node's tab: on a node that is off, it switches it on and selects it; on the
    // selected one, it switches it off and on again; on another that is on, it only selects that one.
    const int selectedThen = layout.selectedNode;
    click(view, {detail::kGridOrigin.x - 18.0f, gridCellCenter(5, 0).y}); // F's row name
    const bool fromName = model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && layout.selectedNode == 5;
    click(view, {detail::kGridOrigin.x - 18.0f, gridCellCenter(5, 0).y}); // again, now that F is selected
    const bool offByName = model.value(paramIndex(5, OpParam::Enabled)) == 0.0 && layout.selectedNode == 5;
    view.mouseMove({gridCellCenter(0, 5).x, gridCellCenter(0, 5).y - 28.0f}, {});
    const bool selectedShot = renderer.save(view, dir / "7d-matrix-selected.png");
    click(view, {gridCellCenter(0, 5).x, gridCellCenter(0, 5).y - 28.0f}); // F's column name
    const bool onByName = model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && layout.selectedNode == 5;
    click(view, {gridCellCenter(0, 0).x, gridCellCenter(0, 0).y - 28.0f}); // A's column name: A is on
    const bool selectsOnly = layout.selectedNode == 0 && model.value(paramIndex(0, OpParam::Enabled)) == 1.0 &&
                             model.value(paramIndex(5, OpParam::Enabled)) == 1.0;
    check(fromName && offByName && onByName && selectsOnly && selectedShot,
          "a matrix name works like its node's tab: it selects, and on the selected node it switches it");
    model.set(paramIndex(5, OpParam::Enabled), 0.0);
    drag(view, gridCellCenter(5, 1), {gridCellCenter(5, 1).x, gridCellCenter(5, 1).y - 30.0f}); // F -> B
    check(model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && model.value(matrixIndex(5, 1)) > 0.0,
          "dragging a matrix route of a node that is off switches it on");
    model.set(matrixIndex(5, 1), 0.0);
    std::erase(model.ui.keptRoutes, matrixIndex(5, 1));
    // A filter's row has its shapers' cells: double-clicking F2 -> S2 switches the route on, and S2 with it.
    const int s2 = kOps + kFilters + 1;
    doubleClick(view, gridCellCenter(kOps + 1, s2));
    check(model.value(filterShaperIndex(1, 1)) == 1.0 && model.value(paramIndex(1, ShaperParam::Enabled)) == 1.0,
          "the matrix routes a filter into a shaper");
    view.undo();
    // Undone, the route isn't kept at 0 % either, so no wire is left of it; nor when the wheel switched it on. A press
    // on the empty cell that drifts a pixel before it goes up, as between a double-click's clicks, sets nothing, so
    // the double-click after it still finds the cell empty and switches the route on.
    {
        const int route = filterShaperIndex(1, 1);
        auto kept = [&] {
            return std::find(model.ui.keptRoutes.begin(), model.ui.keptRoutes.end(), route) != model.ui.keptRoutes.end();
        };
        const bool undoneGone = model.value(route) == 0.0 && !kept();
        const Point cell = gridCellCenter(kOps + 1, s2);
        view.wheel(cell, 1.0f, {});
        const bool wheeledOn = model.value(route) > 0.0 && kept();
        view.undo();
        const bool wheelUndone = model.value(route) == 0.0 && !kept();
        view.mouseDown(cell, {});
        view.mouseMove({cell.x, cell.y - 1.0f}, {});
        view.mouseUp({cell.x, cell.y - 1.0f}, {});
        const bool untouched =
            model.value(route) == 0.0 && model.value(paramIndex(1, ShaperParam::Enabled)) == 0.0 && model.openEdits == 0;
        view.doubleClick({cell.x, cell.y - 1.0f}, {});
        view.mouseUp({cell.x, cell.y - 1.0f}, {});
        const bool connected = model.value(route) == 1.0 && kept();
        view.undo();
        check(undoneGone && wheeledOn && wheelUndone && untouched && connected && model.value(route) == 0.0 && !kept(),
              "undo takes a route switched on out of the routes kept at 0 %, and a press's jitter sets no route");
    }
    // A shaper's row has the shapers' cells too: double-clicking S1 -> S2 switches it on at 100 %, and S2 with it, and
    // S1's own cell, on the diagonal, feeds it back into itself at 25 %.
    {
        doubleClick(view, gridCellCenter(s1, s2));
        const bool chained = model.value(shaperShaperIndex(0, 1)) == 1.0 &&
                             model.value(paramIndex(1, ShaperParam::Enabled)) == 1.0;
        view.undo();
        doubleClick(view, gridCellCenter(s1, s1));
        const bool fedBack = model.value(shaperShaperIndex(0, 0)) == 0.25;
        view.undo();
        check(chained && fedBack && model.value(shaperShaperIndex(0, 0)) == 0.0 &&
                  model.value(shaperShaperIndex(0, 1)) == 0.0 && model.value(paramIndex(1, ShaperParam::Enabled)) == 0.0,
              "the matrix routes a shaper into another shaper and into itself");
    }
    model.set(paramIndex(5, OpParam::Enabled), 0.0);
    model.ui.selectedNode = selectedThen;
    click(view, kRoutingTab);

    // Right-click menu on FM Amount lists the Env 1 modulation.
    const auto menu = view.contextMenu(kFmAmountKnob);
    bool listsEnv1 = false;
    for (const MenuItem &item : menu)
        listsEnv1 = listsEnv1 || item.label == trf(Str::MenuRemoveModulation, {trf(Str::SourceNameEnv, {"1"})});
    check(listsEnv1, "context menu offers removing Env 1 modulation");

    // LFO 1 starts synced to the tempo, in quarter notes: its knob picks a note value. Its sync switch, the effects'
    // metronome, frees it and syncs it again.
    const int sync = paramIndex(0, SourceParam::Sync), division = paramIndex(0, SourceParam::Division);
    const double divisionBefore = model.value(division);
    const bool quarters = model.value(sync) == 1.0 && divisionBefore == paramTable()[division].def &&
                          divisionName(static_cast<int>(divisionBefore)) == std::string("1/4");
    drag(view, rateKnob(0), {rateKnob(0).x, rateKnob(0).y - 20.0f});
    click(view, sourceSync(0));
    const bool freed = model.value(sync) == 0.0;
    view.mouseMove(sourceSync(0), {});
    const bool syncShot = renderer.save(view, dir / "4b-lfo-free.png");
    click(view, sourceSync(0));
    check(quarters && freed && syncShot && model.value(sync) == 1.0 && model.value(division) == divisionBefore + 2.0,
          "an LFO starts synced in quarter notes, its knob a note value, and its sync switch frees it");

    // Its Phase knob sets where notes start it, which a tick on its display marks; free, the knob goes faint and the
    // tick away (read off the rendered pixels: the tick under the curve's peak at 25 %, the knob's arc).
    const int phase = paramIndex(0, SourceParam::Phase);
    drag(view, phaseKnob(0), {phaseKnob(0).x, phaseKnob(0).y - 50.0f});
    view.mouseMove({400.0f, 328.0f}, {});
    const Point tick{cardX(0) + 26.0f + 0.25f * (kCardW - 52.0f), 706.0f};
    const Point arc{phaseKnob(0).x - 12.4f, phaseKnob(0).y + 4.0f}; // on the arc, near its start at the lower left
    auto brightness = [&](Point p) {
        const uint32_t color = renderer.pixel(p);
        return (color >> 16 & 255) + (color >> 8 & 255) + (color & 255);
    };
    const bool phaseShot = renderer.save(view, dir / "4c-lfo-phase.png");
    const uint32_t tickThen = brightness(tick), arcThen = brightness(arc);
    click(view, freeToggle(0));
    view.mouseMove({400.0f, 328.0f}, {});
    const bool freeShot = renderer.save(view, dir / "4d-lfo-phase-free.png");
    const bool faint = brightness(tick) < tickThen && brightness(arc) < arcThen;
    click(view, retriggerToggle(0));
    check(std::abs(model.value(phase) - 0.25) < 1e-6 && phaseShot && freeShot && faint &&
              model.value(paramIndex(0, SourceParam::Free)) == 0.0,
          "an LFO's Phase knob sets its start, marked on its display, and goes faint while the LFO is free");
    // Anchored to the host's transport, its Phase is where it stood at the song's start: the knob and the tick stay
    // as they were (read off the rendered pixels).
    const int anchor = paramIndex(0, SourceParam::Anchor);
    click(view, anchorToggle(0));
    view.mouseMove({400.0f, 328.0f}, {});
    const bool anchorShot = renderer.save(view, dir / "4e-lfo-anchor.png");
    const uint32_t tickNow = brightness(tick), arcNow = brightness(arc);
    std::printf("      anchored: Phase tick %u (was %u), arc %u (was %u)\n", tickNow, tickThen, arcNow, arcThen);
    view.mouseMove(anchorToggle(0), {});
    const bool tipShot = renderer.save(view, dir / "4f-lfo-anchor-tip.png");
    const bool anchored = model.value(anchor) == 1.0;
    click(view, retriggerToggle(0));
    check(anchorShot && tipShot && anchored && tickNow == tickThen && arcNow == arcThen && model.value(anchor) == 0.0,
          "an anchored LFO keeps its Phase in use");
    model.set(phase, 0.0);

    // The FX chain starts empty and covers the routing panel and the inspector.
    click(view, kFxTab);
    check(layout.view == RoutingView::Fx && fxChain(model).empty(), "the FX tab shows an empty effects chain");
    click(view, kFxMenuButton);
    view.mouseMove(fxMenuItem(FxType::Delay), {});
    check(renderer.save(view, dir / "8a-fx-empty.png"), "render the empty FX chain with the ADD EFFECT list open");
    click(view, {600.0f, 428.0f}); // anywhere else closes the list
    check(fxChain(model).empty(), "a click outside the ADD EFFECT list closes it");
    // The dashed card at the chain's end (its first place, while it is empty) opens the list where it is clicked.
    const Point shadowClick{fxCardX(model, 0) + fxCardW(model, 0) * 0.5f, 268.0f};
    click(view, shadowClick);
    view.mouseMove(fxListItem(view, shadowClick, FxType::Shaper, true), {});
    check(renderer.save(view, dir / "8a2-fx-shadow-list.png"), "render the list of effects at the mouse");
    click(view, fxListItem(view, shadowClick, FxType::Shaper, true));
    check(fxChain(model) == std::vector<int>{0} &&
              model.value(paramIndex(0, FxParam::Type)) == static_cast<double>(FxType::Shaper),
          "the dashed card at the chain's end adds an effect");

    // Then a delay, a parametric EQ, a filter and a second delay, each after the last.
    const FxType added[] = {FxType::Shaper, FxType::Delay, FxType::ParamEq, FxType::Filter, FxType::Delay};
    for (size_t i = 1; i < std::size(added); ++i)
        addFx(view, added[i]);
    bool addedInOrder = fxChain(model) == std::vector<int>{0, 1, 2, 3, 4};
    for (int slot = 0; slot < 5; ++slot)
        addedInOrder = addedInOrder && std::lround(model.value(paramIndex(slot, FxParam::Type))) ==
                                           static_cast<long>(added[slot]) &&
                       model.value(paramIndex(slot, FxParam::Enabled)) == 1.0;
    check(addedInOrder && model.openEdits == 0, "the add buttons fill the chain, the same effect more than once");
    // At the chain's right end, the dashed card clicked by its right edge opens its list moved back left inside the
    // editor, over the card before it, and a switched-off card's dimming stays under the list (read off the rendered
    // pixels: the list looks the same over it either way).
    {
        const Point rightClick{std::min(fxCardX(model, 5) + fxCardW(model, 5), 32.0f + 1136.0f) - 6.0f, 328.0f};
        const Point at = fxListAt(view, rightClick, true);
        click(view, rightClick);
        const Point inList{std::max(at.x, fxCardX(model, 4)) + 8.0f, 358.0f}; // over the second delay's card
        std::printf("      list at %.0f (clicked at %.0f), the second delay's card %.0f to %.0f\n", at.x, rightClick.x,
                    fxCardX(model, 4), fxCardX(model, 4) + fxCardW(model, 4));
        const bool moved = at.x + 8.0f < rightClick.x && inList.x > fxCardX(model, 4) &&
                           inList.x < fxCardX(model, 4) + fxCardW(model, 4);
        renderer.save(view, dir / "8a3-fx-list-right.png");
        const uint32_t overOn = renderer.pixel(inList);
        model.set(paramIndex(4, FxParam::Enabled), 0.0);
        const bool shot = renderer.save(view, dir / "8a4-fx-list-over-off.png");
        const uint32_t overOff = renderer.pixel(inList);
        model.set(paramIndex(4, FxParam::Enabled), 1.0);
        click(view, {600.0f, 168.0f}); // anywhere else closes it
        check(moved && shot && overOn == overOff && fxChain(model).size() == 5,
              "the list of effects stays inside the editor, and over a switched-off card's dimming");
    }

    // The filter card's type buttons, in a row: Comb is the fifth.
    click(view, {fxCardX(model, 3) + 14.0f + 4.0f * 29.4f + 13.0f, 180.0f + 44.0f + 10.0f});
    check(std::lround(model.value(paramIndex(3, FxParam::FilterType))) == static_cast<long>(FilterType::Comb),
          "the filter card's buttons pick the filter type");

    // The parametric EQ's first band: dragging its handle 30 px up is +10 dB.
    const float peqW = fxCardW(model, 2) - 44.0f; // its display's plot, inside the wider card
    const Point band{fxCardX(model, 2) + 22.0f + static_cast<float>(model.value(peqFreqIndex(2, 0))) * peqW, 292.0f};
    drag(view, band, {band.x + 20.0f, band.y - 30.0f});
    check(std::abs(model.value(peqGainIndex(2, 0)) - 10.0) < 0.01 && model.value(peqFreqIndex(2, 0)) > 0.4 &&
              model.openEdits == 0,
          "dragging a band handle sets its frequency and gain");
    // Alt-dragged 40 px up, the handle sets the band's Q instead (a fifth of its range, as on a knob), and stays.
    {
        const Point moved{band.x + 20.0f, band.y - 30.0f};
        const double freq = model.value(peqFreqIndex(2, 0)), gain = model.value(peqGainIndex(2, 0));
        const double q = model.value(peqQIndex(2, 0));
        view.mouseDown(moved, {false, false, true});
        view.mouseMove({moved.x + 15.0f, moved.y - 40.0f}, {false, false, true});
        view.mouseUp({moved.x + 15.0f, moved.y - 40.0f}, {false, false, true});
        check(std::abs(model.value(peqQIndex(2, 0)) - (q + 0.2)) < 1e-6 && model.value(peqFreqIndex(2, 0)) == freq &&
                  model.value(peqGainIndex(2, 0)) == gain && model.openEdits == 0,
              "Alt-dragging a band handle up and down sets its Q");
    }
    // The bands' chips under its display, 26 px tall at 368: each its power switch (26 px) on the left, its number,
    // and its shape (34 px) on the right; the band picked's Freq, Gain and Q at 436, a quarter of the card each, then
    // Amount.
    const float peqCardX = fxCardX(model, 2), peqCardW = fxCardW(model, 2);
    const float chipW = (peqCardW - 28.0f - 8.0f) / 3.0f;
    auto chipX = [&](int b) { return peqCardX + 14.0f + static_cast<float>(b) * (chipW + 4.0f); };
    auto chipPower = [&](int b) { return Point{chipX(b) + 13.0f, 381.0f}; };
    auto chipNumber = [&](int b) { return Point{chipX(b) + 26.0f + (chipW - 60.0f) * 0.5f, 381.0f}; };
    auto chipShape = [&](int b) { return Point{chipX(b) + chipW - 17.0f, 381.0f}; };
    // A shape's list opens above its chip, inside the card: 124 px wide, an item 22 px from 228.
    auto peqShapeItem = [&](int b, PeqType t) {
        const float x = std::clamp(chipX(b) + chipW * 0.5f, peqCardX + 8.0f + 62.0f, peqCardX + peqCardW - 8.0f - 62.0f);
        return Point{x, 228.0f + static_cast<float>(t) * 22.0f + 11.0f};
    };
    auto peqKnob = [&](int i) { return Point{peqCardX + peqCardW * (static_cast<float>(i) + 0.5f) / 4.0f, 436.0f}; };
    // The second band's shape comes from a list that opens above its chip: a high cut.
    click(view, chipShape(1));
    view.mouseMove(peqShapeItem(1, PeqType::HighCut), {});
    check(renderer.save(view, dir / "8e-peq-shapes.png"), "render the list of EQ band shapes");
    click(view, peqShapeItem(1, PeqType::HighCut));
    check(model.value(peqTypeIndex(2, 1)) == static_cast<double>(PeqType::HighCut),
          "the band's list picks its shape");
    // The knobs set the band picked: the second, its shape clicked; a click on the first's number switches it off
    // and picks it, and another switches it back on.
    {
        const double first = model.value(peqGainIndex(2, 0)), second = model.value(peqGainIndex(2, 1));
        drag(view, peqKnob(1), {peqKnob(1).x, peqKnob(1).y - 40.0f});
        const double secondTurned = model.value(peqGainIndex(2, 1));
        const bool turnedSecond = secondTurned > second && model.value(peqGainIndex(2, 0)) == first;
        click(view, chipNumber(0));
        const bool firstOff = model.value(peqOnIndex(2, 0)) == 0.0;
        drag(view, peqKnob(1), {peqKnob(1).x, peqKnob(1).y + 40.0f});
        const bool turnedFirst = model.value(peqGainIndex(2, 0)) < first &&
                                 model.value(peqGainIndex(2, 1)) == secondTurned;
        click(view, chipNumber(0));
        check(firstOff && model.value(peqOnIndex(2, 0)) == 1.0,
              "a Param EQ chip's number switches its band off and on, as its power symbol does");
        model.set(peqGainIndex(2, 0), first);
        model.set(peqGainIndex(2, 1), second);
        check(turnedSecond && turnedFirst, "a Param EQ's knobs set the band whose chip was clicked");
        // Amount, the fourth, is the whole EQ's, whichever band is picked.
        drag(view, peqKnob(3), {peqKnob(3).x, peqKnob(3).y + 40.0f});
        const bool amount = model.value(paramIndex(2, FxParam::PeqAmount)) < 1.0 &&
                            model.value(peqGainIndex(2, 0)) == first && model.value(peqGainIndex(2, 1)) == second;
        check(renderer.save(view, dir / "8e1-peq-amount.png"), "render the Param EQ's knobs, Amount turned down");
        model.set(paramIndex(2, FxParam::PeqAmount), 1.0);
        check(amount, "a Param EQ's Amount knob sets the whole EQ's amount");
    }
    // The third band switched off, its handle faint on the 0 dB line: a press on it switches it on and picks
    // it (and moves nothing), undoably; its chip's power symbol switches it off and on again, and its shape's list
    // stays inside the card. Made a notch, its handle stays on the 0 dB line and only moves across.
    {
        model.set(peqOnIndex(2, 2), 0.0);
        const Point third{peqCardX + 22.0f + static_cast<float>(model.value(peqFreqIndex(2, 2))) * peqW, 292.0f};
        const double freq = model.value(peqFreqIndex(2, 2));
        const bool startsOff = model.value(peqOnIndex(2, 2)) == 0.0;
        click(view, third);
        const bool switchedOn = model.value(peqOnIndex(2, 2)) == 1.0;
        view.undo();
        const bool undone = model.value(peqOnIndex(2, 2)) == 0.0;
        view.redo();
        const double before = model.value(peqQIndex(2, 2));
        drag(view, peqKnob(2), {peqKnob(2).x, peqKnob(2).y - 30.0f});
        const bool picked = model.value(peqQIndex(2, 2)) > before && model.value(peqFreqIndex(2, 2)) == freq;
        model.set(peqQIndex(2, 2), before);
        click(view, chipPower(2));
        const bool off = model.value(peqOnIndex(2, 2)) == 0.0;
        click(view, chipPower(2));
        const bool on = off && model.value(peqOnIndex(2, 2)) == 1.0;
        click(view, chipShape(2));
        view.mouseMove(peqShapeItem(2, PeqType::Notch), {});
        check(renderer.save(view, dir / "8e2-peq-third-band.png"), "render the EQ's third band's list of shapes");
        click(view, peqShapeItem(2, PeqType::Notch));
        const bool shaped = model.value(peqTypeIndex(2, 2)) == static_cast<double>(PeqType::Notch);
        const double gain = model.value(peqGainIndex(2, 2)), q = model.value(peqQIndex(2, 2));
        drag(view, third, {third.x + 12.0f, third.y - 30.0f});
        const bool across = model.value(peqFreqIndex(2, 2)) > freq && model.value(peqGainIndex(2, 2)) == gain &&
                            model.value(peqQIndex(2, 2)) == q;
        check(renderer.save(view, dir / "8e3-peq-notch.png"), "render the EQ's third band as a notch");
        model.set(peqFreqIndex(2, 2), freq);
        click(view, chipPower(2));
        check(startsOff && switchedOn && undone && picked,
              "a Param EQ band's handle, pressed while the band is off, switches it on and picks it, undoably");
        check(on && shaped && model.value(peqOnIndex(2, 2)) == 0.0,
              "a Param EQ chip's power symbol switches its band off and on, and its shape's list picks its shape");
        check(across, "a Param EQ notch's handle moves only across");
    }
    model.set(peqGainIndex(2, 1), -6.0);
    model.set(peqQIndex(2, 1), 0.2);
    model.set(paramIndex(3, FxParam::FilterType), static_cast<double>(FilterType::BandPass));
    model.set(paramIndex(3, FxParam::FilterResonance), 0.6);
    model.set(paramIndex(0, FxParam::ShaperDrive), 18.0);
    // The shaper's curve comes from a list under its button.
    const Point clipButton{fxCardX(model, 0) + 60.0f, 180.0f + 44.0f + 11.0f};
    auto clipItem = [&](ClipMode mode) {
        return Point{fxCardX(model, 0) + 40.0f, 180.0f + 44.0f + 22.0f + 4.0f + 4.0f + static_cast<int>(mode) * 22.0f + 11.0f};
    };
    click(view, clipButton);
    view.mouseMove(clipItem(ClipMode::SineFold), {});
    check(renderer.save(view, dir / "8b-clip-menu.png"), "render the shaper's list of curves");
    click(view, clipItem(ClipMode::Asymmetric));
    check(std::lround(model.value(paramIndex(0, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Asymmetric),
          "the shaper's list picks its curve");
    // Custom draws the curve in the card, to edit right there: its plot is the display less 8 px all round
    // (the card less 44 px across, 100 down), the full swing 6 px short of its edges. A double-click adds a point, a
    // drag moves it.
    click(view, clipButton);
    click(view, clipItem(ClipMode::Custom));
    auto cardAt = [&](double x, double y) {
        return Point{fxCardX(model, 0) + 22.0f + static_cast<float>(x) * (kFxCardW - 44.0f),
                     312.0f - static_cast<float>(y) * (50.0f - 6.0f)};
    };
    view.doubleClick(cardAt(0.8, 0.4), {}); // under the bend handle of the curve's upper half
    const int curveX2 = shapeFieldIndex(clipCurveCountIndex(0), 2, PointField::X);
    check(std::lround(model.value(paramIndex(0, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Custom) &&
              model.value(clipCurveCountIndex(0)) == 4.0 && std::abs(model.value(curveX2) - 0.8) < 0.01,
          "picking Custom draws the curve in the shaper's card, where a double-click adds a point");
    drag(view, cardAt(0.8, 0.4), cardAt(0.8, 0.6));
    check(std::abs(model.value(shapeFieldIndex(clipCurveCountIndex(0), 2, PointField::Y)) - 0.6) < 0.03,
          "a point of the curve drags in the card");
    view.mouseMove(cardAt(0.8, 0.6), {});
    check(renderer.save(view, dir / "8d-clip-custom.png"), "render the curve being edited in the card");
    // Its edit button, a pencil, opens the big editor, which edits the same curve.
    click(view, {fxCardX(model, 0) + 134.0f, 180.0f + 190.0f - 12.0f}); // the pencil, at the display's bottom right
    view.doubleClick(clipAt(0.3, -0.5), {});
    check(model.value(clipCurveCountIndex(0)) == 5.0 &&
              std::abs(model.value(shapeFieldIndex(clipCurveCountIndex(0), 1, PointField::X)) - 0.3) < 0.01,
          "the pencil opens the big curve editor, which adds points too");
    view.mouseMove(clipAt(0.3, -0.5), {});
    check(renderer.save(view, dir / "8c-clip-curve.png"), "render the shaper's big curve editor");
    {
        // Snap, in its title row, snaps a dragged point to the grid without Ctrl.
        click(view, {742.0f, 152.0f});
        drag(view, clipAt(0.3, -0.5), clipAt(0.33, -0.45));
        const double x = model.value(shapeFieldIndex(clipCurveCountIndex(0), 1, PointField::X));
        const double y = model.value(shapeFieldIndex(clipCurveCountIndex(0), 1, PointField::Y));
        const bool snapped = model.layout().shapeSnap && std::abs(x - 5.0 / 16.0) < 1e-9 && std::abs(y + 0.5) < 1e-9;
        view.undo();
        click(view, {742.0f, 152.0f});
        check(snapped && !model.layout().shapeSnap, "Snap snaps a shaper curve's points in its big editor");
    }
    {
        // Shift+click draws steps here too, as many as on an LFO's shape: one in every column of a grid of 32.
        const int grid = model.layout().shapeGridX;
        model.layout().shapeGridX = 32;
        for (int step = 0; step < 32; ++step)
            click(view, clipAt((step + 0.5) / 32.0, step % 2 ? -0.5 : 0.5), {true, false});
        const int last = shapeFieldIndex(clipCurveCountIndex(0), 63, PointField::Y);
        check(model.value(clipCurveCountIndex(0)) == 64.0 && std::abs(model.value(last) + 0.5) < 0.01,
              "a shaper's curve takes a step in every column of a grid of 32");
        for (int step = 0; step < 32; ++step)
            view.undo();
        model.layout().shapeGridX = grid;
        check(model.value(clipCurveCountIndex(0)) == 5.0, "and its steps undo");
    }
    click(view, kClipDone);
    view.mouseMove({600.0f, 428.0f}, {});
    check(layout.view == RoutingView::Fx, "the done button goes back to the chain");
    model.set(paramIndex(0, FxParam::ShaperMode), static_cast<double>(ClipMode::Asymmetric));
    model.set(paramIndex(1, FxParam::DelayPingPong), 1.0);
    model.set(paramIndex(1, FxParam::DelayTone), -0.4);
    model.set(paramIndex(4, FxParam::DelaySync), 0.0);
    model.set(paramIndex(4, FxParam::DelayWidth), 0.3);
    // The first delay's LINK switch, the middle of three across the card under its display.
    click(view, {fxCardX(model, 1) + fxCardW(model, 1) * 0.5f, 180.0f + 171.0f});
    check(model.value(paramIndex(1, FxParam::DelayLink)) == 1.0, "the delay's LINK switch links its sides");
    // The shaper's LINK, its chain under the display in the card's middle (with no SYM beside it, but for a Custom
    // curve), starts on; a click unlinks it, another links it again.
    const Point clipLink{fxCardX(model, 0) + fxCardW(model, 0) * 0.5f, 180.0f + 194.0f + 10.0f};
    const bool linkedFirst = model.value(paramIndex(0, FxParam::ShaperLink)) == 1.0;
    click(view, clipLink);
    const bool unlinked = model.value(paramIndex(0, FxParam::ShaperLink)) == 0.0;
    click(view, clipLink);
    check(linkedFirst && unlinked && model.value(paramIndex(0, FxParam::ShaperLink)) == 1.0,
          "the shaper's LINK starts on and switches");
    click(view, {clipLink.x - 40.0f, clipLink.y}); // where SYM shows for a Custom curve
    check(model.value(paramIndex(0, FxParam::ShaperSymmetric)) == 0.0, "the shaper's SYM is hidden but for Custom");
    {
        // For a Custom curve, SYM shows left of LINK, the two centered together: switched on, it puts a point in the middle of a drawn curve that has none there, and
        // the curve's left half follows the right one: a double-click there adds the mirror image of a point on the
        // right, and the middle point can't be deleted. Undone, the middle point goes with the switch.
        const int mode = paramIndex(0, FxParam::ShaperMode), symmetric = paramIndex(0, FxParam::ShaperSymmetric);
        const int count = clipCurveCountIndex(0);
        auto point = [&](int p, PointField field) { return model.value(shapeFieldIndex(count, p, field)); };
        model.set(mode, static_cast<double>(ClipMode::Custom));
        const int thirdX = shapeFieldIndex(count, 2, PointField::X); // 0, 0.3, then the middle one
        const double thirdWas = model.value(thirdX);
        model.set(thirdX, 0.6);
        click(view, {clipLink.x - 25.0f, clipLink.y});
        const bool middled = model.value(symmetric) == 1.0 && model.value(count) == 6.0 &&
                             std::abs(point(2, PointField::X) - 0.5) < 1e-9;
        view.doubleClick(cardAt(0.15, -0.3), {});
        const bool mirroredPoint = model.value(count) == 7.0 && std::abs(point(5, PointField::X) - 0.85) < 0.01 &&
                                   std::abs(point(5, PointField::Y) - 0.3) < 0.03;
        view.doubleClick(cardAt(0.5, point(2, PointField::Y)), {});
        view.mouseMove(cardAt(0.85, 0.3), {});
        check(middled && mirroredPoint && model.value(count) == 7.0 &&
                  renderer.save(view, dir / "8g-clip-symmetric.png"),
              "the shaper's SYM puts a point in the middle, and the curve's left half mirrors its right");
        view.undo();
        view.undo();
        model.set(thirdX, thirdWas);
        model.set(mode, static_cast<double>(ClipMode::Asymmetric));
        check(model.value(count) == 5.0 && model.value(symmetric) == 0.0, "SYM undoes with its middle point");
    }
    click(view, fxPower(model, 4));
    // Still under the mouse, the icon of the delay just switched off is grey, as its card is: neither lit up nor in
    // the delay's blue.
    bool greyIcon = renderer.save(view, dir / "8f-fx-off-icon.png");
    uint32_t brightest = 0; // across its first bar
    for (float dx = 10.5f; dx <= 13.5f; dx += 0.5f) {
        const uint32_t color = renderer.pixel({fxCardX(model, 4) + dx, 197.0f});
        brightest = (color >> 16 & 255) > (brightest >> 16 & 255) ? color : brightest;
    }
    const int red = static_cast<int>(brightest >> 16 & 255), blue = static_cast<int>(brightest & 255);
    greyIcon = greyIcon && red > 90 && red < 200 && blue - red < 40;
    check(model.value(paramIndex(4, FxParam::Enabled)) == 0.0 && greyIcon,
          "a card's icon turns its effect off, and greys out with it even under the mouse");
    // The parametric EQ's analyzer: a tone's harmonics over a falling floor.
    for (int bin = 0; bin < kSpectrumBands; ++bin) {
        const double hz = cutoffFromNorm((bin + 0.5) / kSpectrumBands);
        double level = 0.02 * std::pow(200.0 / hz, 0.6);
        for (int k = 1; k <= 12; ++k)
            level = std::max(level, 0.4 / k * std::exp(-std::pow(std::log(hz / (220.0 * k)) * 12.0, 2.0)));
        model.live.fxSpectrum[2][bin] = static_cast<float>(level);
    }
    view.mouseMove(band, {});
    view.poll();
    check(renderer.save(view, dir / "8-fx.png"), "render the FX chain, the EQ showing what it puts out");

    // Drag the shaper's card two places along.
    view.mouseDown(fxGrip(model, 0), {});
    view.mouseMove({fxGrip(model, 0).x + 200.0f, 212.0f}, {});
    view.mouseMove({fxGrip(model, 0).x + 390.0f, 212.0f}, {});
    for (int k = 0; k < 40; ++k) // the others gliding aside
        view.poll();
    check(renderer.save(view, dir / "9-fx-reorder.png"), "render a card being dragged");
    view.mouseUp({fxGrip(model, 0).x + 390.0f, 212.0f}, {});
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4}, "dragging a card's title moves that effect along the chain");
    // Dragged by its icon, a card moves just the same, and stays switched as it was; undo puts it back.
    const Point shaperIcon = fxPower(model, 2);
    view.mouseDown(shaperIcon, {});
    view.mouseMove({shaperIcon.x - 160.0f, 212.0f}, {});
    view.mouseMove({shaperIcon.x - 320.0f, 212.0f}, {});
    view.mouseUp({shaperIcon.x - 320.0f, 212.0f}, {});
    const bool iconMoved = fxChain(model) == std::vector<int>{1, 0, 2, 3, 4} &&
                           model.value(paramIndex(0, FxParam::Enabled)) == 1.0;
    view.undo();
    check(iconMoved && fxChain(model) == std::vector<int>{1, 2, 0, 3, 4},
          "dragging a card's icon moves it without switching it");
    view.mouseMove(fxPower(model, 2), {});
    check(renderer.save(view, dir / "9a-fx-icon.png"), "render a card's icon under the mouse, with its tooltip");

    // A new Env 2 onto the delay's Mix knob; the delay leads the chain now.
    addSource(view, SourceType::Env);
    drag(view, sourceHandle(3), fxKnob(model, 0, 2, 3, 266.0f));
    check(slotTarget(model.value(slotTargetIndex(3, 0))) == paramIndex(1, FxParam::DelayMix),
          "an envelope can modulate an effect knob");

    // Duplicate the parametric EQ from its title's menu: the copy follows it, with every
    // setting, and its type comes last, so a host rescans the copy's fields once they hold
    // the original's. Then remove the copy.
    model.set(peqGainIndex(2, 1), -7.5);
    model.sets.clear();
    for (const MenuItem &item : view.contextMenu(fxGrip(model, 1)))
        if (item.label == tr(Str::MenuDuplicate))
            view.menuCommand(item.command);
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4, 5} &&
              sameFields(model, paramIndex(5, FxParam::Type), paramIndex(2, FxParam::Type), kFxParamCount) &&
              setLast(model, paramIndex(5, FxParam::Type), kFxParamCount, paramIndex(5, FxParam::Type)),
          "Duplicate copies an effect, settings and all, to the end of the chain");
    // Seven places with the two EQs' wider cards: the copy scrolled into view, one place along.
    fxScrolled = 1.0f;
    click(view, fxRemove(model, 5));
    fxScrolled = 0.0f;
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4}, "the x takes an effect out of the chain");
    // The arrow between the first two cards turns into a plus under the mouse, and adds an effect there: the list
    // opens over the cards, its middle at the arrow, and Delay, picked from it, goes in second. (The row was left
    // half a place along; the wheel over the panel's title row takes it back to the start.)
    {
        view.wheel({600.0f, 153.0f}, 10.0f, {});
        const Point arrow{fxCardX(model, 1) - (kFxStep - kFxCardW) * 0.5f, 332.0f};
        view.mouseMove(arrow, {});
        const bool hoverShot = renderer.save(view, dir / "8d-fx-insert.png");
        click(view, arrow);
        // The list's top: under the title row, or as high as keeps it (a split after the effects) inside the panel.
        const Rect menu = detail::fxMenu({0.0f, 0.0f}, true); // its middle at the arrow, kept inside the panel
        const float top = std::min(188.0f, detail::kFxPanel.bottom() - 4.0f - menu.h);
        const float left = std::clamp(arrow.x - 0.5f * menu.w, detail::kFxPanel.x + 8.0f,
                                      detail::kFxPanel.right() - 8.0f - menu.w);
        const Point delay = rectCenter(detail::fxMenuItem({left, top}, FxType::Delay));
        view.mouseMove(delay, {});
        const bool insertListShot = renderer.save(view, dir / "8e-fx-insert-list.png");
        click(view, delay);
        const std::vector<int> chain = fxChain(model);
        const bool inserted = chain.size() == 6 && chain[0] == 1 && chain[2] == 2 && chain[3] == 0 &&
                              model.value(paramIndex(chain[1], FxParam::Type)) == static_cast<double>(FxType::Delay);
        view.undo();
        check(hoverShot && insertListShot && inserted && fxChain(model) == std::vector<int>{1, 2, 0, 3, 4},
              "the arrow between two effects adds one there, undoably");
    }

    // More effects than fit (seven cards, seven and a half places with the EQ's): the one added last scrolls
    // into view, so the compressors land on screen places 4 and 5. (The three-band dynamics' card has a test of its
    // own: see testMultiDyn().)
    addFx(view, FxType::Compressor);
    addFx(view, FxType::Compressor);
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4, 5, 6} &&
              model.value(paramIndex(5, FxParam::Type)) == static_cast<double>(FxType::Compressor) &&
              model.value(paramIndex(6, FxParam::Type)) == static_cast<double>(FxType::Compressor),
          "the chain grows past six effects, compressors included");
    fxScrolled = 1.5f;
    model.live.fxReduction[5] = {4.5f, 0.0f, 0.0f};
    model.live.fxReduction[6] = {2.0f, 0.0f, 0.0f};
    view.poll();
    view.mouseMove({600.0f, 328.0f}, {});
    check(renderer.save(view, dir / "9b-compressors.png"), "render the compressors in a scrolled chain");
    // The scrollbar under the cards: eight and a half places with the dashed card at the end, six on screen,
    // scrolled one and a half along. Its thumb is six of those parts of the 1136 px track, with 334 px to move
    // in; dragging it a third of that back leaves the cards between places, and all the way back shows the
    // first card again.
    view.mouseDown({681.0f, 493.0f}, {});
    view.mouseMove({681.0f - 111.0f, 493.0f}, {});
    check(renderer.save(view, dir / "9c-fx-half-scrolled.png"), "render the chain scrolled half a card");
    view.mouseMove({681.0f - 300.0f, 493.0f}, {});
    view.mouseUp({681.0f - 300.0f, 493.0f}, {});
    fxScrolled = 0.0f;
    click(view, fxPower(model, 0));
    const bool scrolledBack = model.value(paramIndex(1, FxParam::Enabled)) == 0.0;
    click(view, fxPower(model, 0));
    // Back to the end to take the compressors out: the chain gets shorter, and its scroll with it; the wheel
    // over the panel's title row takes it back to the start.
    view.mouseDown({681.0f, 493.0f}, {});
    view.mouseMove({681.0f + 600.0f, 493.0f}, {});
    view.mouseUp({681.0f + 600.0f, 493.0f}, {});
    fxScrolled = 2.5f;
    click(view, fxRemove(model, 6));
    fxScrolled = 1.5f;
    click(view, fxRemove(model, 5));
    view.wheel({600.0f, 153.0f}, 10.0f, {});
    fxScrolled = 0.0f;
    check(scrolledBack && fxChain(model) == std::vector<int>{1, 2, 0, 3, 4},
          "dragging the scrollbar scrolls the chain back");

    // LFO 2's place made a Curve source: a click on its display widens its card, whose full-size button opens its
    // shape over the routing panel. The default shape is a triangle, with points at (0, -1) and (0.5, 1).
    model.set(paramIndex(2, SourceParam::Type), static_cast<double>(SourceType::Curve));
    view.poll();
    click(view, lfoDisplay(2));
    click(view, wideButton(2, 2));
    const int count = shapeCountIndex(2);
    auto pointValue = [&](int point, PointField field) { return model.value(shapePointIndex(2, point, field)); };
    check(model.value(count) == 2.0 && pointValue(0, PointField::Y) == -1.0 && pointValue(1, PointField::X) == 0.5,
          "a Curve source has a freeform shape, a triangle to begin with");
    view.doubleClick(shapeAt(0.75, -0.4), {});
    check(model.value(count) == 3.0 && std::abs(pointValue(2, PointField::X) - 0.75) < 0.01 &&
              std::abs(pointValue(2, PointField::Y) + 0.4) < 0.01 && pointValue(1, PointField::X) == 0.5,
          "double-clicking the editor adds a point in order");
    drag(view, shapeAt(0.5, 1.0), shapeAt(0.4, 0.5));
    check(std::abs(pointValue(1, PointField::X) - 0.4) < 0.01 && std::abs(pointValue(1, PointField::Y) - 0.5) < 0.01 &&
              model.openEdits == 0,
          "dragging a point moves it");
    drag(view, shapeAt(0.2, -0.25), shapeAt(0.2, -0.25 + 40.0 / 126.0));
    check(pointValue(0, PointField::Curve) < -0.3, "dragging a segment's handle bends it");
    model.live.voices = 1;
    model.live.lfoPhases[2] = 0.62f;
    model.live.sources[2] = -0.4f;
    view.poll();
    view.mouseMove(shapeAt(0.75, -0.4), {});
    check(renderer.save(view, dir / "10-lfo-shape.png"), "render the shape editor");
    model.set(paramIndex(2, SourceParam::Unipolar), 1.0);
    check(renderer.save(view, dir / "10a-lfo-shape-unipolar.png"), "render a unipolar shape, filled from the bottom");
    model.set(paramIndex(2, SourceParam::Unipolar), 0.0);
    view.doubleClick(shapeAt(0.75, -0.4), {});
    check(model.value(count) == 2.0 && std::abs(pointValue(1, PointField::X) - 0.4) < 0.01,
          "double-clicking a point deletes it");
    // A point dragged past its neighbor stops at its time: a line straight up or down, drawn through both.
    view.doubleClick(shapeAt(0.75, -0.4), {});
    drag(view, shapeAt(0.75, -0.4), shapeAt(0.3, -0.6));
    check(model.value(count) == 3.0 && pointValue(2, PointField::X) == pointValue(1, PointField::X) &&
              std::abs(pointValue(2, PointField::Y) + 0.6) < 0.01 &&
              renderer.save(view, dir / "10c-lfo-vertical.png"),
          "a point dragged onto its neighbor's time makes a vertical line");
    view.undo();
    view.undo();
    check(model.value(count) == 2.0 && std::abs(pointValue(1, PointField::X) - 0.4) < 0.01,
          "the vertical line undoes");
    // Shift+click draws a step between the grid lines either side, a point at each end. The next column's step
    // starts beside the first one's end, the shape jumping there. Redrawing the first keeps the next one's start,
    // and a quick second Shift+click there (a double-click) leaves it as it is, adding no point.
    {
        const double column = 1.0 / model.layout().shapeGridX;
        auto roughly = [](double a, double b) { return std::abs(a - b) < 0.01; };
        auto pointIs = [&](int point, double x, double y) {
            return std::abs(pointValue(point, PointField::X) - x) < 1e-9 && roughly(pointValue(point, PointField::Y), y);
        };
        click(view, shapeAt(2.5 * column, 0.25), {true, false});
        const bool first = model.value(count) == 4.0 && pointIs(1, 2.0 * column, 0.25) &&
                           pointIs(2, 3.0 * column, 0.25) && pointValue(1, PointField::Curve) == 0.0 &&
                           roughly(pointValue(3, PointField::X), 0.4);
        click(view, shapeAt(3.5 * column, -0.5), {true, false});
        const bool next = model.value(count) == 6.0 && pointIs(2, 3.0 * column, 0.25) &&
                          pointIs(3, 3.0 * column, -0.5) && pointIs(4, 4.0 * column, -0.5);
        click(view, shapeAt(2.5 * column, 0.75), {true, false});
        view.doubleClick(shapeAt(2.5 * column, 0.75), {true, false});
        view.mouseUp(shapeAt(2.5 * column, 0.75), {true, false});
        const bool redrawn = model.value(count) == 6.0 && pointIs(1, 2.0 * column, 0.75) &&
                             pointIs(2, 3.0 * column, 0.75) && pointIs(3, 3.0 * column, -0.5) &&
                             roughly(pointValue(5, PointField::X), 0.4) && model.openEdits == 0;
        view.mouseMove(shapeAt(0.7, 0.0), {});
        check(first && next && redrawn && renderer.save(view, dir / "10b-lfo-steps.png"),
              "Shift+click draws steps between the grid's lines");
        click(view, shapeAt(0.5 * column, 1.0), {true, false}); // the first column moves the first point
        check(model.value(count) == 7.0 && pointIs(0, 0.0, 1.0) && pointIs(1, column, 1.0) &&
                  pointIs(2, 2.0 * column, 0.75),
              "a step in the first column takes the first point");
        for (int step = 0; step < 4; ++step)
            view.undo();
        check(model.value(count) == 2.0 && roughly(pointValue(1, PointField::X), 0.4), "steps undo one at a time");
        // Held, a step drawn follows the mouse: up and down to its level, and on to the last column, leaving the
        // first as it was; the whole stroke one undo step.
        {
            const int lines = model.layout().shapeGridX;
            view.mouseDown(shapeAt(2.5 * column, 0.25), {true, false});
            view.mouseMove(shapeAt(2.5 * column, -0.5), {true, false});
            const bool level = model.value(count) == 4.0 && pointIs(1, 2.0 * column, -0.5) &&
                               pointIs(2, 3.0 * column, -0.5) && model.openEdits == 0;
            view.mouseMove(shapeAt(1.0 - 0.5 * column, 0.5), {});
            view.mouseUp(shapeAt(1.0 - 0.5 * column, 0.5), {});
            const bool moved = model.value(count) == 4.0 && roughly(pointValue(1, PointField::X), 0.4) &&
                               pointIs(2, (lines - 1) * column, 0.5) && pointIs(3, 1.0, 0.5) && model.openEdits == 0;
            view.undo();
            check(level && moved && model.value(count) == 2.0 && roughly(pointValue(1, PointField::X), 0.4),
                  "a step held follows the mouse, in one undo step");
        }
        // An LFO's shape holds 64 points: a step in every column of the finest grid, 32 across.
        const int grid = model.layout().shapeGridX;
        model.layout().shapeGridX = 32;
        for (int step = 0; step < 32; ++step)
            click(view, shapeAt((step + 0.5) / 32.0, step % 2 ? -0.5 : 0.5), {true, false});
        check(model.value(count) == 64.0 && pointIs(63, 1.0, -0.5), "a step fits in every column of a grid of 32");
        for (int step = 0; step < 32; ++step)
            view.undo();
        model.layout().shapeGridX = grid;
    }
    click(view, kShapeDone);
    click(view, fxPower(model, 0));
    check(model.value(paramIndex(1, FxParam::Enabled)) == 0.0,
          "the full-size editor's collapse button closes it and the FX view is back");
    click(view, wideButton(2, 0));

    // Four sources and the dashed cards, less than a card more than fit: dragged all the way, the scrollbar's thumb
    // reaches the track's right end (read off the rendered pixels).
    drag(view, {485.0f, 769.0f}, {485.0f + 500.0f, 769.0f});
    view.mouseMove({400.0f, 328.0f}, {});
    const bool endShot = renderer.save(view, dir / "11g-sources-scrolled-end.png");
    check(endShot && renderer.pixel({1168.0f, 769.0f}) == renderer.pixel({700.0f, 769.0f}),
          "the scrollbar's thumb reaches the end with the dashed cards less than a card past the row");
    view.wheel({60.0f, 541.0f}, 10.0f, {});

    // A fifth source scrolls the cards along to show it; the wheel over the panel scrolls back.
    addSource(view, SourceType::Lfo);
    click(view, sourcePower(3));
    check(shown(model) == std::vector<int>{0, 1, 2, 3, 4} && model.value(sourceEnabledIndex(4)) == 0.0,
          "a new source's card comes into view");
    click(view, {40.0f, 769.0f}); // the scrollbar's track, left of its thumb
    click(view, sourcePower(0));
    check(model.value(sourceEnabledIndex(0)) == 0.0, "clicking the scrollbar's track scrolls the cards back");
    click(view, sourcePower(0));
    // Five cards and the dashed ones, four places on screen. Dragged all the way, the scrollbar's thumb shows
    // the fifth card, the dashed ones after it.
    drag(view, {485.0f, 769.0f}, {485.0f + 500.0f, 769.0f});
    click(view, sourcePower(3.0f - kSourceShadowSpan));
    view.mouseMove({cardX(4.0f - kSourceShadowSpan) + 61.0f, 628.0f}, {});
    check(model.value(sourceEnabledIndex(4)) == 1.0 && renderer.save(view, dir / "11e-source-shadow.png"),
          "dragging the scrollbar's thumb scrolls the cards, the dashed ones last");
    view.wheel({60.0f, 541.0f}, 1.0f, {}); // two fifths of a card a notch
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "11b-sources-between.png"), "render the source cards between places");
    view.wheel({60.0f, 541.0f}, 10.0f, {});
    // While a dialog is open, the wheel scrolls nothing behind it: closed again, the first card is where it was.
    click(view, kSettingsButton);
    view.wheel({60.0f, 541.0f}, -10.0f, {});
    click(view, {20.0f, 328.0f}); // around the dialog, which only closes it
    click(view, sourcePower(0));
    check(model.value(sourceEnabledIndex(0)) == 0.0, "the wheel scrolls nothing behind the settings dialog");
    click(view, sourcePower(0));

    // Env 1's card moved within its own place, and carried straight up out of the panel (outlined under the mouse
    // there), moves nothing: the others shift only once the mouse is clear of its place.
    {
        const std::vector<int> order = shown(model);
        view.mouseDown(sourceGrip(1), {});
        view.mouseMove({sourceGrip(1).x + 100.0f, 588.0f}, {});
        view.mouseUp({sourceGrip(1).x + 100.0f, 588.0f}, {});
        const bool within = shown(model) == order;
        view.mouseDown(sourceGrip(1), {});
        view.mouseMove({sourceGrip(1).x + 20.0f, 588.0f}, {});
        view.mouseMove({sourceGrip(1).x + 20.0f, 300.0f}, {});
        const bool freeShot = renderer.save(view, dir / "11a0-source-carried-free.png");
        view.mouseUp({sourceGrip(1).x + 20.0f, 300.0f}, {});
        check(within && freeShot && shown(model) == order,
              "a source card moves nothing until the mouse is clear of its place, carried anywhere");
    }

    // Drag Env 1's card one place along.
    view.mouseDown(sourceGrip(1), {});
    view.mouseMove({sourceGrip(1).x + 150.0f, 588.0f}, {});
    view.mouseMove({sourceGrip(1).x + 280.0f, 588.0f}, {});
    check(renderer.save(view, dir / "11a-source-reorder.png"), "render a source card being dragged");
    view.mouseUp({sourceGrip(1).x + 280.0f, 588.0f}, {});
    check(shown(model) == std::vector<int>{0, 2, 1, 3, 4} && nameOf(model, 2) == trf(Str::SourceNameCurve, {"1"}),
          "dragging a card's title moves that source along");

    // Duplicate Curve 1 from its title's menu: the copy, with its settings and shape but none of its targets, follows it.
    model.sets.clear();
    for (const MenuItem &item : view.contextMenu(sourceGrip(1)))
        if (item.label == tr(Str::MenuDuplicate))
            view.menuCommand(item.command);
    {
        const int settings = paramIndex(0, SourceParam::Shape) - paramIndex(0, SourceParam::Type);
        bool noTargets = true;
        for (int slot = 0; slot < kSlots; ++slot)
            noTargets = noTargets && model.value(slotTargetIndex(5, slot)) == 0.0 &&
                        model.value(slotAmountIndex(5, slot)) == 0.0 && model.value(slotAuxIndex(5, slot)) == 0.0;
        check(shown(model) == std::vector<int>{0, 2, 5, 1, 3, 4} &&
                  sameFields(model, paramIndex(5, SourceParam::Enabled), paramIndex(2, SourceParam::Enabled), 1) &&
                  sameFields(model, paramIndex(5, SourceParam::Shape), paramIndex(2, SourceParam::Shape),
                             kSourceParamCount - settings) &&
                  sameFields(model, shapeCountIndex(5), shapeCountIndex(2), kShapeParamCount) &&
                  setLast(model, paramIndex(5, SourceParam::Type), kSourceParamCount, paramIndex(5, SourceParam::Type)) &&
                  slotTarget(model.value(slotTargetIndex(2, 0))) == paramIndex(GlobalParam::Pan) && noTargets &&
                  model.value(paramIndex(5, SourceParam::Type)) == static_cast<double>(SourceType::Curve) &&
                  nameOf(model, 5) == trf(Str::SourceNameCurve, {"2"}),
              "Duplicate copies a source in right after it, without its targets");
    }

    // A card's title menu picks the source's polarity, the one in force checked: an envelope (Env 1, the fourth
    // card) turns bipolar, and an LFO (the first, made unipolar by its switch earlier) bipolar and back.
    auto polarity = [&](Point at, const std::string &label) {
        bool checked = false;
        for (const MenuItem &item : view.contextMenu(at))
            if (item.label == label) {
                checked = item.checked;
                view.menuCommand(item.command);
            }
        return checked;
    };
    const bool envWasUnipolar = !polarity(sourceGrip(3), tr(Str::MenuBipolar));
    const bool envBipolar = model.value(paramIndex(1, SourceParam::EnvBipolar)) == 1.0;
    view.mouseMove({600.0f, 328.0f}, {});
    const bool envShot = renderer.save(view, dir / "11f-bipolar-env.png");
    const bool lfoWasUnipolar = !polarity(sourceGrip(0), tr(Str::MenuBipolar));
    const bool lfoBipolar = model.value(paramIndex(0, SourceParam::Unipolar)) == 0.0;
    polarity(sourceGrip(0), tr(Str::MenuUnipolar));
    check(envWasUnipolar && envBipolar && envShot && lfoWasUnipolar && lfoBipolar &&
              model.value(paramIndex(0, SourceParam::Unipolar)) == 1.0,
          "a source's title menu makes it unipolar or bipolar");

    // Remove Env 1 (now the fourth card): its slots go with it. A new envelope takes its place.
    click(view, sourceRemove(3));
    check(shown(model) == std::vector<int>{0, 2, 5, 3, 4} &&
              model.value(paramIndex(1, SourceParam::Type)) == static_cast<double>(SourceType::None) &&
              slotTarget(model.value(slotTargetIndex(1, 0))) < 0 && slotTarget(model.value(slotTargetIndex(1, 1))) < 0,
          "the x takes a source out with its modulation");
    addSource(view, SourceType::Env);
    check(shown(model) == std::vector<int>{0, 2, 5, 3, 4, 1} && slotTarget(model.value(slotTargetIndex(1, 0))) < 0 &&
              model.value(paramIndex(1, SourceParam::Sustain)) == paramTable()[paramIndex(1, SourceParam::Sustain)].def &&
              nameOf(model, 3) == trf(Str::SourceNameEnv, {"1"}) && nameOf(model, 1) == trf(Str::SourceNameEnv, {"2"}),
          "a new source fills the first free place, fresh, at the end");
    model.set(paramIndex(4, SourceParam::Shape), static_cast<double>(LfoShape::Saw));
    model.live.sources = {0.4f, 0.3f, -0.6f, 0.5f, 0.8f, -0.2f};
    model.live.lfoPhases = {0.1f, 0.0f, 0.62f, 0.0f, 0.35f, 0.62f};
    view.mouseMove({400.0f, 328.0f}, {});
    view.poll();
    check(renderer.save(view, dir / "11-sources.png"), "render six sources, scrolled");

    // A source with modulations has the button that shows them in MODS, counting them, under its display.
    view.wheel(kSourcesToggle, 10.0f, {}); // the first card back in view
    for (int k = 0; k < 5; ++k) {
        model.set(slotTargetIndex(0, kSlots - 1 - k), slotTargetValue(paramIndex(k, OpParam::Phase)));
        model.set(slotAmountIndex(0, kSlots - 1 - k), 0.1 * (k + 1));
    }
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "11c-card-mods.png"), "render a card counting its modulations");

    // Up to 16: then the add buttons go.
    for (int n = static_cast<int>(shown(model).size()); n < kMaxSources; ++n)
        addSource(view, n % 2 ? SourceType::Env : SourceType::Lfo);
    check(shown(model).size() == static_cast<size_t>(kMaxSources), "the panel holds 16 sources");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "12-sources-full.png"), "render 16 sources");

    // Collapsed, the modulation panel keeps its title row and the editor gets shorter.
    const float open = view.height();
    click(view, kSourcesToggle);
    check(layout.sourcesCollapsed && view.height() == open - 216.0f, "the toggle collapses the modulation panel");
    view.mouseMove({400.0f, 328.0f}, {});
    check(renderer.save(view, dir / "13-collapsed.png"), "render the collapsed modulation panel");
    click(view, kSourcesToggle);
    check(!layout.sourcesCollapsed && view.height() == open, "the toggle opens the panel again");

    // Init and Load ask before throwing away a changed patch; Save As never asks.
    const Rect confirm{420.0f, std::round((view.height() - 150.0f) * 0.5f), 360.0f, 150.0f};
    const Point cancel{confirm.x + 170.0f + 40.0f, confirm.bottom() - 38.0f};
    const Point ok{confirm.right() - 64.0f, confirm.bottom() - 38.0f};
    click(view, kInitButton);
    click(view, kSourcesToggle); // around the dialog: cancels it
    check(model.resets == 1 && !layout.sourcesCollapsed, "a click around the confirm dialog cancels it");
    click(view, kInitButton);
    view.mouseMove(ok, {});
    check(renderer.save(view, dir / "15-confirm.png"), "render the confirm dialog");
    click(view, cancel);
    check(model.resets == 1, "Cancel keeps the changed patch");
    click(view, kInitButton);
    click(view, ok);
    check(model.resets == 2, "the dialog's Init button resets the changed patch");
    // A file still to be picked goes unsaid, so the dialog before one loads is a line shorter: a click just above
    // where the longer one ends falls outside it and cancels, and Save then gets its click.
    const Rect shorter{420.0f, std::round((view.height() - 132.0f) * 0.5f), 360.0f, 132.0f};
    click(view, kLoadButton);
    view.mouseMove({shorter.right() - 64.0f, shorter.bottom() - 38.0f}, {});
    const bool fileConfirm = renderer.save(view, dir / "15a-confirm-file.png");
    click(view, {confirm.x + 40.0f, confirm.bottom() - 4.0f});
    click(view, kSaveAsButton);
    check(fileConfirm && model.loads == 1 && model.saves == 1, "the dialog before a file loads is a line shorter");
    click(view, kLoadButton);
    click(view, {shorter.right() - 64.0f, shorter.bottom() - 38.0f});
    check(model.loads == 2 && model.saves == 1, "Load asks first, Save As goes ahead");
    view.mouseMove(kSaveAsButton, {});
    check(renderer.save(view, dir / "15b-toolbar.png"), "render the preset icons with a tooltip");

    // Save writes the patch over its preset's file, asking first, and only while the patch has changed. A Save As
    // clicked after it shows whether a dialog took the click instead (its backdrop cancelling it).
    const std::wstring presetBefore = model.presetFile();
    const bool editedBefore = model.patchEdited();
    model.setPreset(L"Bell"); // unchanged: Save is greyed out and does nothing
    click(view, kSaveButton);
    click(view, kSaveAsButton);
    view.mouseMove(kSaveButton, {});
    const bool unchangedShot = renderer.save(view, dir / "15c-save-unchanged.png");
    const uint32_t greyed = renderer.pixel({kSaveButton.x - 6.5f, kSaveButton.y}); // the disk's left edge
    const uint32_t lit = renderer.pixel({kSaveAsButton.x - 6.5f, kSaveAsButton.y});
    check(unchangedShot && model.writes == 0 && model.saves == 2 && greyed != lit,
          "Save is greyed out and does nothing while the preset is unchanged");
    // Its dialog has one line, so it is the shorter one.
    const Point overwriteOk{shorter.right() - 64.0f, shorter.bottom() - 38.0f};
    const Point overwriteCancel{shorter.x + 170.0f + 40.0f, shorter.bottom() - 38.0f};
    model.setPreset(L"Bell", true);
    click(view, kSaveButton);
    view.mouseMove(overwriteOk, {});
    const bool overwriteShot = renderer.save(view, dir / "15d-confirm-overwrite.png");
    click(view, overwriteCancel);
    check(overwriteShot && model.writes == 0 && model.patchEdited(), "Save asks before overwriting, and Cancel keeps it");
    click(view, kSaveButton);
    click(view, overwriteOk);
    check(model.writes == 1 && model.written == L"Bell" && !model.patchEdited() && model.saves == 2,
          "Overwrite writes the patch over the preset's file, which is then unchanged");
    model.setPreset(L"Bell", true);
    model.writeFails = true;
    click(view, kSaveButton);
    click(view, overwriteOk);
    model.writeFails = false;
    check(model.writes == 2 && model.patchEdited(), "a preset that could not be written stays changed");
    model.setPreset(L""); // Init, unchanged: neither saves
    click(view, kSaveButton);
    click(view, kSaveAsButton);
    check(model.writes == 2 && model.saves == 2, "Save and Save As do nothing on an unchanged Init");
    model.setPreset(L"", true); // Init, changed: only Save As, which asks nothing
    click(view, kSaveButton);
    click(view, kSaveAsButton);
    view.mouseMove(kSaveButton, {});
    const bool initShot = renderer.save(view, dir / "15e-save-init.png");
    click(view, kSaveAsButton);
    check(initShot && model.writes == 2 && model.saves == 4, "Save does nothing on a changed Init, and Save As goes ahead");
    model.setPreset(presetBefore, editedBefore);

    // The cog after the preset buttons opens the settings dialog, which takes every click until it closes.
    const Point cog = kSettingsButton;
    const Rect dialog = detail::settingsPanel(view.height());
    auto option = [&](int i) { return Point{dialog.x + 24.0f + i * 64.0f + 29.0f, dialog.y + 96.0f}; };
    auto mpeSwitch = [&](int on) { return Point{dialog.x + 24.0f + on * 56.0f + 26.0f, dialog.y + 228.0f}; };
    auto mpeRange = [&](int i) { return Point{dialog.x + 190.0f + i * 42.0f + 19.0f, dialog.y + 228.0f}; };
    auto theme = [&](int i) {
        const float w = (dialog.w - 48.0f + 6.0f) / View::themeCount();
        return Point{dialog.x + 24.0f + i * w + (w - 6.0f) * 0.5f, dialog.y + 357.0f};
    };
    const Point closeDialog{dialog.right() - 26.0f, dialog.y + 26.0f};
    click(view, cog);
    view.mouseMove(mpeRange(1), {});
    check(renderer.save(view, dir / "14g-settings-mpe-off.png"), "render the settings dialog with MPE off");
    click(view, option(2));
    check(model.oversamplingFactor == 4, "the settings dialog picks the oversampling");
    // MPE: On and Off switch it, and the bend range's buttons pick how far its member channels bend, none of it
    // touching the patch.
    const ParamValues patchBefore = model.values;
    click(view, mpeSwitch(1));
    const bool mpeOn = model.mpeOn;
    click(view, mpeRange(1));
    const bool ranged = model.mpeRange == 24;
    click(view, mpeSwitch(0));
    const bool mpeOff = !model.mpeOn && model.mpeRange == 24;
    click(view, mpeSwitch(1));
    check(mpeOn && ranged && mpeOff && model.mpeOn && model.values == patchBefore,
          "the settings dialog switches MPE and picks its bend range, leaving the patch alone");
    view.mouseMove(option(3), {});
    check(renderer.save(view, dir / "14-settings.png"), "render the settings dialog");
    // Its theme buttons repaint the whole editor in another theme. The first is the default.
    const int midnight = detail::themeIndex("Midnight");
    bool themed = detail::themeIndex("") == 0 && std::string_view(View::themeName(0)) == "Spectrum" &&
                  std::string_view(View::themeName(1)) == "Porcelain";
    for (int i = 0; i < View::themeCount(); ++i) {
        if (i == midnight)
            continue;
        click(view, theme(i));
        themed = themed && model.themeName == View::themeName(i);
        if (std::string_view(View::themeName(i)) == "Light")
            themed = themed && renderer.save(view, dir / "14c-settings-light.png");
        click(view, closeDialog);
        view.mouseMove({600.0f, 328.0f}, {});
        std::string file = std::string("14d-theme-") + View::themeName(i) + ".png";
        std::transform(file.begin(), file.end(), file.begin(),
                       [](unsigned char ch) { return ch == ' ' ? '-' : static_cast<char>(std::tolower(ch)); });
        themed = themed && renderer.save(view, dir / file);
        click(view, kRoutingTab);
        view.mouseMove({600.0f, 328.0f}, {});
        file.insert(file.size() - 4, "-routing");
        themed = themed && renderer.save(view, dir / file);
        if (std::string_view(View::themeName(i)) == "Spectrum") { // its effects as a graph too
            click(view, kFxTab);
            click(view, detail::fxModeButton(1).center());
            view.mouseMove({600.0f, 328.0f}, {});
            themed = themed && renderer.save(view, dir / "14d-theme-spectrum-graph.png");
            click(view, detail::fxModeButton(0).center());
        }
        click(view, kFxTab);
        click(view, cog);
    }
    click(view, theme(midnight));
    check(themed && model.themeName == "Midnight",
          "the settings dialog lists Spectrum (the default) and Porcelain first, and picks the theme");
    // The GLOW slider sets how strongly the graphs' curves glow, on both pages, once it is let go.
    auto glowAt = [&](float amount) { return Point{dialog.x + 76.0f + 200.0f * amount, dialog.y + 429.0f}; };
    view.mouseDown(glowAt(0.2f), {});
    view.mouseMove(glowAt(0.75f), {});
    const bool kept = model.glowLevel == 0.0; // only once it is let go
    view.mouseUp(glowAt(0.75f), {});
    const bool glowing = kept && std::abs(model.glowLevel - 0.75) < 1e-3;
    view.mouseMove(glowAt(0.75f), {});
    const bool sliderShot = renderer.save(view, dir / "14f-glow-slider.png");
    click(view, closeDialog);
    view.mouseMove({600.0f, 328.0f}, {});
    const bool glowShots = sliderShot && renderer.save(view, dir / "14e-glow.png");
    click(view, kRoutingTab);
    view.mouseMove({600.0f, 328.0f}, {});
    check(glowing && glowShots && renderer.save(view, dir / "14e-glow-routing.png"),
          "the settings dialog's GLOW slider sets how strongly the curves glow");
    click(view, kFxTab);
    click(view, cog);
    view.mouseDown(glowAt(0.3f), {});
    view.mouseMove({glowAt(0.0f).x - 40.0f, glowAt(0.0f).y}, {});
    view.mouseUp({glowAt(0.0f).x - 40.0f, glowAt(0.0f).y}, {});
    check(model.glowLevel == 0.0, "the GLOW slider's left end turns it off");
    // HANDLES picks the depth handles' size, Small to begin with, leaving the patch alone.
    // LINES, under GLOW, sets how strongly a source's dashed lines are drawn, 75 % to begin with, once it is let go.
    auto linesAt = [&](float amount) { return Point{dialog.x + 76.0f + 200.0f * amount, dialog.y + 312.0f + 149.0f}; };
    const bool linesStart = model.links == 0.75;
    view.mouseDown(linesAt(0.75f), {});
    view.mouseMove(linesAt(0.4f), {});
    const bool linesKept = model.links == 0.75; // only once it is let go
    const bool linesShot = renderer.save(view, dir / "14i-settings-lines.png");
    view.mouseUp(linesAt(0.4f), {});
    const bool linesSet = std::abs(model.links - 0.4) < 1e-3;
    view.mouseDown(linesAt(0.4f), {});
    view.mouseMove({linesAt(1.0f).x + 40.0f, linesAt(1.0f).y}, {});
    view.mouseUp({linesAt(1.0f).x + 40.0f, linesAt(1.0f).y}, {});
    check(linesStart && linesKept && linesShot && linesSet && model.links == 1.0 && model.glowLevel == 0.0,
          "the settings dialog's LINES slider sets how strongly the sources' lines are drawn, and only that");
    // HUE, under the themes, turns the theme's main color round the color wheel once it is let go, the theme's own to
    // begin with; the accent then takes it (read off the rendered pixels: the slider's knob, in the accent).
    auto hueAt = [&](float hue) { return Point{dialog.x + 76.0f + 200.0f * hue, dialog.y + 312.0f + 85.0f}; };
    const bool hueStarts = model.hue < 0.0;
    view.mouseDown(hueAt(0.2f), {});
    view.mouseMove(hueAt(0.6f), {});
    const bool hueKept = model.hue < 0.0; // only once it is let go
    view.mouseUp(hueAt(0.6f), {});
    const bool hueSet = std::abs(model.hue - 0.6) < 1e-3;
    view.mouseMove({dialog.x + 24.0f, dialog.y + 30.0f}, {});
    const bool hueShot = renderer.save(view, dir / "14j-settings-hue.png");
    const float knobHue = detail::hueOf(Color::hex(renderer.pixel(hueAt(0.6f)) & 0xFFFFFF));
    check(hueStarts && hueKept && hueSet && hueShot && std::abs(knobHue - 0.6f) < 0.02f && model.themeName == "Midnight",
          "the settings dialog's HUE slider turns the theme's main color");
    // A double-click puts HUE back to the theme's own color, GLOW to half and LINES to 75 %.
    doubleClick(view, hueAt(0.3f));
    doubleClick(view, glowAt(0.9f));
    doubleClick(view, linesAt(0.1f));
    check(model.hue < 0.0 && model.glowLevel == 0.5 && model.links == 0.75,
          "a double-click puts the HUE, GLOW and LINES sliders back to where they start");
    model.glowLevel = 0.0;
    model.links = 0.75;
    // HANDLES picks the depth handles' size, Small to begin with, leaving the patch alone.
    auto handlesButton = [&](int size) { return Point{dialog.x + 76.0f + size * 68.0f + 32.0f, dialog.y + 312.0f + 184.0f}; };
    const ParamValues handlesBefore = model.values;
    const bool startsSmall = model.handles == 0;
    click(view, handlesButton(2));
    const bool picksLarge = model.handles == 2;
    view.mouseMove(handlesButton(1), {});
    const bool handlesShot = renderer.save(view, dir / "14h-settings-handles.png");
    click(view, handlesButton(1));
    const bool picksMedium = model.handles == 1;
    click(view, handlesButton(0));
    check(startsSmall && picksLarge && picksMedium && model.handles == 0 && handlesShot && model.values == handlesBefore,
          "the settings dialog's HANDLES picks the depth handles' size");
    click(view, closeDialog);
    view.mouseMove(cog, {});
    check(renderer.save(view, dir / "14b-settings-closed.png"), "render the cog after the dialog closed");
    click(view, cog);
    click(view, kSourcesToggle);
    check(!layout.sourcesCollapsed && model.oversamplingFactor == 4,
          "a click around the dialog only closes it");
    click(view, kSourcesToggle);
    check(layout.sourcesCollapsed, "once it is closed, the editor takes clicks again");
    click(view, kSourcesToggle);
    runMore(dir, renderer);
    // The README's, in the default theme (see release.ps1): Screamer's routing, Crystal Motion's effects as a chain and
    // Neon Pluck's as a graph, split low and high.
    renderExample(dir, renderer, "PD Crystal Motion", "24-example", 6, "Spectrum");
    renderExample(dir, renderer, "LD Screamer", "24c-screamer-example", 4, "Spectrum", true); // a note playing
    renderExample(dir, renderer, "BA Neon Pluck", "24b-split-example", 4, "Spectrum");
    renderExample(dir, renderer, "PD Crystal Motion", "42-porcelain-crystal", 6, "Porcelain");
    renderExample(dir, renderer, "BA Neon Pluck", "43-porcelain-neon", 4, "Porcelain");
    renderExample(dir, renderer, "BA Reese Bass", "25-reese", 4);
    renderExample(dir, renderer, "LD Folded Lead", "31-folded-lead", 2);
    renderExample(dir, renderer, "BA Fuzz Bass", "32-fuzz-bass", 2);
    renderExample(dir, renderer, "SQ Step Filter", "33-step-filter", 3);
    renderFactoryPresets(dir, renderer);
    renderLfoOnLfo(dir, renderer);
    testMacros(dir, renderer);
    testMacroTargets(dir, renderer);
    testVelocity(dir, renderer);
    testWireBend(dir, renderer);
    testRouteFrom(dir, renderer);
    testNodeGrid(dir, renderer);
    testRouteModes(dir, renderer);
    testInputsScroll(dir, renderer);
    testSelectedNode(dir, renderer);
    testRadarGlow(dir, renderer);
    testOutBend(dir, renderer);
    testModsEmpty(dir, renderer);
    testNewSources(dir, renderer);
    testFollowerSource(dir, renderer);
    testMods(dir, renderer);
    testModsMoveDepth(dir, renderer);
    testModsLive(dir, renderer);
    testNodeSwitchOnPlace(dir, renderer);
    testNodeDelete(dir, renderer);
    testLabelClearOfNode(dir, renderer);
    testLabelsClearOfEachOther(dir, renderer);
    testLoopLabelClear(dir, renderer);
    testModsSearch(dir, renderer);
    testMeter(dir, renderer);
    testOutScope(dir, renderer);
    renderNewEffects(dir, renderer);
    testChorus(dir, renderer);
    testShapers(dir, renderer);
    testWideLfoCard(dir, renderer);
    testFolding(dir, renderer);
    testFxGraph(dir, renderer);
    testFxListAfterUndo(dir, renderer);
    testInvalidText(dir, renderer);
    testPartialRepaint(dir, renderer);
    testFxGraphScroll(dir, renderer);
    testFxGraphRoom(dir, renderer);
    testFxRow(dir, renderer);
    testFxRowSplitFirst(dir, renderer);
    testFxClear(dir, renderer);
    testLaterSlots(dir, renderer);
    testSourceShadow(dir, renderer);
    testMultiDyn(dir, renderer);
    testAmp(dir, renderer);
    testUtility(dir, renderer);
    testCardEdgeScroll(dir, renderer);
    testMiddleDrag(dir, renderer);
    testTouchPan(dir, renderer);
    testCtrlCopy(dir, renderer);
    testDialogKeys(dir, renderer);
    testModFocus(dir, renderer);
    testCardOutline(dir, renderer);
    testFlatModulatedKnob(dir, renderer);
    testModEndMarks(dir, renderer);
    testFlatThemes(dir, renderer);
    testModPill(dir, renderer);
    testRatioDisplay(dir, renderer);
    testTempo(dir, renderer);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fmvibe-uishot <output-dir> [scale] [--presets]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // every line out before a crash, if there is one
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    const float scale = argc > 2 ? static_cast<float>(std::atof(argv[2])) : 1.0f;
    if (argc > 3 && std::strcmp(argv[3], "--presets") == 0) { // only the factory presets' routing views
        Renderer renderer(scale);
        renderFactoryPresets(dir, renderer);
    } else {
        run(dir, scale); // COM objects die before CoUninitialize
    }
#ifdef _WIN32
    CoUninitialize();
#endif
    std::printf("%s (images in %s)\n", failures ? "UI CHECKS FAILED" : "all UI checks passed", dir.string().c_str());
    return failures ? 1 : 0;
}
