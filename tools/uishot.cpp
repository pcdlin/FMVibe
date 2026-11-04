// Renders the editor view offscreen into PNGs, replaying a few mouse
// interactions on the way, so the look and the drag logic can be checked
// without a host. Usage: fmvibe-uishot <output-dir> [scale]

#include "factory.h"
#include "gui/canvas.h"
#include "gui/view.h"
#include "uistrings.h"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
    void pitchWheel(double) override {}
    void setScope(int op, int cycles) override {
        scopeOp = op;
        scopeCycles = cycles;
    }
    void resetPreset() override { ++resets; }
    bool loadPreset(const std::wstring &) override { return false; }
    bool savePreset(const std::wstring &) override { return false; }
    void choosePreset(bool save) override { ++(save ? saves : loads); }
    int resets = 0, loads = 0, saves = 0;
    // A small library: one preset at the top, two in a subfolder.
    void rescanPresets() override {}
    const std::vector<PresetEntry> &presets() const override { return library; }
    // As the plugin does: a preset's file is its name here, and loading one is an undo step that sets every
    // param (each preset with its own volume) and lands on the preset, unchanged.
    int presetIndex() const override {
        for (size_t i = 0; i < library.size(); ++i)
            if (presetFile() == std::wstring(library[i].name.begin(), library[i].name.end()))
                return static_cast<int>(i);
        return -1;
    }
    std::string presetName() const override {
        return presetIndex() < 0 ? tr(Str::PresetInit) : library[presetIndex()].name;
    }
    using Controller::setPreset;
    bool loadPresetAt(int index) override {
        const UndoScope step(*this);
        recordAll();
        values[paramIndex(GlobalParam::Volume)] = -20.0 + 2.0 * index;
        setPreset(std::wstring(library[index].name.begin(), library[index].name.end()));
        return true;
    }
    std::wstring presetFolder() const override { return L"C:\\Users\\Someone\\Documents\\FMVibe\\presets"; }
    void openPresetFolder() override { ++folderOpens; }
    std::vector<PresetEntry> library{{"Init Bass", ""}, {"Bell", "Factory"}, {"Pad", "Factory"}};
    int folderOpens = 0;
    int oversampling() const override { return oversamplingFactor; }
    void setOversampling(int factor) override { oversamplingFactor = factor; }
    std::string theme() const override { return themeName; }
    void setTheme(const std::string &name) override { themeName = name; }
    double glow() const override { return glowLevel; }
    void setGlow(double amount) override { glowLevel = amount; }

protected:
    void performBeginEdit(int) override { ++openEdits; }
    void performSetValue(int index, double v) override {
        values[index] = v;
        sets.push_back(index);
    }
    void performEndEdit(int) override { --openEdits; }

public:

    void set(int index, double v) { values[index] = v; }

    ParamValues values{};
    int oversamplingFactor = 2;
    std::string themeName;
    double glowLevel = 0.0;
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

class Renderer {
public:
    explicit Renderer(float scale) : scale_(scale) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    }

    bool save(View &view, const std::filesystem::path &path) {
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
        return SUCCEEDED(wic_->CreateStream(stream.GetAddressOf())) &&
               SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
               SUCCEEDED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) &&
               SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
               SUCCEEDED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) &&
               SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)) &&
               SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
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

// Screen positions of a few controls in the default layout (mirrors src/gui/layout.h).
constexpr Point kFineKnob{967.0f, 214.0f};
constexpr Point kOutKnob{1146.0f, 214.0f};
constexpr float kBusY = 446.0f; // the routing graph's OUT line
constexpr Point kPitchKnob{1012.0f, 214.0f};
constexpr Point kUnisonKnob{934.0f, 310.0f}; // on the UNISON tab
constexpr Point kPhaseKnob{1134.0f, 310.0f};
constexpr Point kSlopeKnob{922.0f, 214.0f};
constexpr Point kDecayKnob{1034.0f, 426.0f};
// An operator's envelope display, on its ENVELOPE tab (kOpEnvRect): its top and bottom.
constexpr float kOpEnvTop = 288.0f;
constexpr float kOpEnvBottom = 398.0f;
constexpr Point kFmAmountKnob{359.0f, 32.0f};
constexpr Point kVoicesKnob{975.0f, 32.0f}; // past the second divider, where Bend was
constexpr Point kMonoButton{753.0f, 21.0f};
constexpr Point kLegatoToggle{731.0f, 44.0f};
constexpr Point kGlideKnob{817.0f, 32.0f};
constexpr Point kAlwaysToggle{899.0f, 32.0f};
// The view tabs sit above the panels.
constexpr Point kRoutingTab{64.0f, 88.0f};
constexpr Point kMatrixTab{160.0f, 88.0f};
constexpr Point kFxTab{240.0f, 88.0f};
constexpr Point kF1Tab{1068.0f, 124.0f};
// Init, Load, Save and the settings cog at the right end of the view tabs' row.
constexpr Point kInitButton{1068.0f, 84.0f};
constexpr Point kLoadButton{1102.0f, 84.0f};
constexpr Point kSaveButton{1136.0f, 84.0f};
constexpr Point kSettingsButton{1170.0f, 84.0f};
constexpr Point kOpFTab{1037.0f, 124.0f};
constexpr Point kSourcesToggle{60.0f, 513.0f};

// The modulation panel: chips and add buttons in its title row, then four cards, 277 wide (and after them the
// dashed cards that add sources, 93 wide), at screen places 0..3, fractions while scrolled.
constexpr float kCardW = 277.0f;
constexpr float kSourceShadowSpan = (93.0f + 12.0f) / (kCardW + 12.0f);
float cardX(float card) {
    return 28.0f + card * (kCardW + 12.0f);
}

// ADD LFO or ADD ENV, after the panel's toggle.
Point sourceAdd(SourceType type) { // + LFO, + ENV, + RANDOM, + MIDI, 88 px apart
    return {128.0f + 88.0f * static_cast<float>(static_cast<int>(type) - 1) + 41.0f, 513.0f};
}

Point sourceGrip(float card) {
    return {cardX(card) + 90.0f, 544.0f}; // over its name
}

Point sourceHandle(float card) {
    return {cardX(card) + 24.0f, 543.0f};
}

// An LFO's sync switch, a square left of its Rate knob.
Point sourceSync(float card) {
    return {cardX(card) + 155.0f, 592.0f};
}

Point sourcePower(float card) {
    return {cardX(card) + kCardW - 50.0f, 545.0f};
}

Point sourceRemove(float card) {
    return {cardX(card) + kCardW - 17.0f, 545.0f};
}

// An LFO card's shape list: its button, and a shape in the list it opens.
Point shapeButton(float card) {
    return {cardX(card) + 80.0f, 589.0f};
}

Point shapeItem(float card, LfoShape shape) {
    return {shapeButton(card).x, 619.0f + static_cast<int>(shape) * 22.0f};
}

// An LFO card's switches under its shape list.
Point freeToggle(float card) {
    return {cardX(card) + 35.0f, 616.0f};
}

Point oneShotToggle(float card) {
    return {cardX(card) + 77.0f, 616.0f};
}

Point unipolarToggle(float card) {
    return {cardX(card) + 119.0f, 616.0f};
}

// Its Rate and Phase knobs, over a random LFO's Smooth and Chaos.
Point rateKnob(float card) {
    return {cardX(card) + 186.0f, 592.0f};
}

Point phaseKnob(float card) {
    return {cardX(card) + 236.0f, 592.0f};
}

// The places holding a source, in the order they show, and what the editor calls one.
std::vector<int> shown(FakeController &model) {
    return shownSources(model.layout(), [&](int index) { return model.value(index); });
}

std::string nameOf(FakeController &model, int source) {
    return sourceName(model.layout(), [&](int index) { return model.value(index); }, source);
}

// The slots holding an effect, in chain order.
std::vector<int> fxChain(const FakeController &model) {
    std::vector<int> chain;
    for (int slot : fxOrder(model.value(paramIndex(GlobalParam::FxOrder))))
        if (std::lround(model.value(paramIndex(slot, FxParam::Type))) != 0)
            chain.push_back(slot);
    return chain;
}

// FX cards, by position in the chain: 22 apart and 171 wide (spread over the panel's 1136), the parametric EQ's one
// and a half card places wide, a folded card a strip 38 wide: the icon's place, 18 wide and 10 in from the card's
// left edge, and as much on its right.
constexpr float kFxCardW = 171.0f;
constexpr float kFxStep = kFxCardW + (1136.0f - 6.0f * kFxCardW) / 5.0f;
constexpr float kFoldedSpan = (38.0f + kFxStep - kFxCardW) / kFxStep;
float fxSpan(const FakeController &model, int slot) {
    if ((model.ui.fxFolded >> slot) & 1u)
        return kFoldedSpan;
    return std::lround(model.value(paramIndex(slot, FxParam::Type))) == static_cast<long>(FxType::ParamEq) ? 1.5f : 1.0f;
}
// How far the test has left the chain scrolled, in card places: cards sit that much left of their places.
float fxScrolled = 0.0f;
float fxCardX(const FakeController &model, int position) {
    const std::vector<int> chain = fxChain(model);
    float place = 0.0f;
    for (int i = 0; i < position && i < static_cast<int>(chain.size()); ++i)
        place += fxSpan(model, chain[i]);
    return 32.0f + (place - fxScrolled) * kFxStep;
}
float fxCardW(const FakeController &model, int position) {
    const std::vector<int> chain = fxChain(model);
    const float span = position < static_cast<int>(chain.size()) ? fxSpan(model, chain[position]) : 1.0f;
    return span * kFxStep - (kFxStep - kFxCardW);
}

Point fxGrip(const FakeController &model, int position) {
    return {fxCardX(model, position) + 40.0f, 169.0f};
}

// The effect's icon at its title's left, which switches it, on a folded card too.
Point fxPower(const FakeController &model, int position) {
    return {fxCardX(model, position) + 19.0f, 169.0f};
}

Point fxRemove(const FakeController &model, int position) {
    return {fxCardX(model, position) + fxCardW(model, position) - 17.0f, 170.0f};
}

// Knob `index` of `count` across a card, in the row at `y` within it.
Point fxKnob(const FakeController &model, int position, int index, int count, float y = 240.0f) {
    return {fxCardX(model, position) + fxCardW(model, position) / count * (index + 0.5f), 152.0f + y};
}

// ADD EFFECT at the FX panel's top left, and the entry for `type` in the list it opens.
constexpr Point kFxMenuButton{90.0f, 124.0f};
Point fxMenuItem(FxType type) {
    return {122.0f, 155.0f + (static_cast<int>(type) - 1) * 26.0f};
}


// The freeform shape editor: x 0..1 across, y -1..1 up. An LFO's spans the routing panel and the inspector, its
// plot 1088 wide; the note source's curve, like the partial editor, fills the routing panel only, 772 wide.
Point shapeAt(double x, double y, float width = 1088.0f) {
    return {56.0f + static_cast<float>(x) * width, 302.0f - static_cast<float>(y) * 126.0f};
}
Point noteCurveAt(double x, double y) {
    return shapeAt(x, y, 772.0f);
}
constexpr Point kShapeDone{1154.0f, 124.0f};
constexpr Point kRoutingEditorDone{838.0f, 124.0f}; // the note curve's and the partial editor's
// A shaper's big curve editor is a 520 px panel in the middle of the effects' panel, its plot 440 wide.
Point clipAt(double x, double y) {
    return {380.0f + static_cast<float>(x) * 440.0f, 302.0f - static_cast<float>(y) * 126.0f};
}
constexpr Point kClipDone{830.0f, 124.0f};

// The common waves' buttons (Sine, Triangle, Saw, Square, Noise), then "..." with the others.
Point waveButton(int button) {
    constexpr float kStep = (300.0f - 32.0f + 4.0f) / 6.0f;
    return {900.0f + button * kStep + (kStep - 4.0f) * 0.5f, 177.0f};
}
const Point kNoiseButton = waveButton(4);
// An operator page's tabs, ENVELOPE, UNISON and SCOPE (by InspectorTab), and the scope's buttons for 1, 2 and 4
// cycles in its display's top right corner.
Point opTabPoint(InspectorTab tab) {
    constexpr float kX[] = {1009.0f, 937.0f, 1071.0f};
    return {kX[static_cast<int>(tab)], 270.0f};
}
Point scopeCycleButton(int choice) {
    return {1112.0f + choice * 20.0f, 302.0f};
}
const Point kMoreWavesButton = waveButton(5);

Point typeButton(int type) {
    return {900.0f + type * 36.0f + 16.0f, 177.0f};
}

// The slope's list button at the end of the type buttons' row, and its entries, 6 to 24 dB, which open under it.
constexpr Point kSlopeButton{1137.0f, 177.0f};
Point slopeItem(int slope) {
    return {1137.0f, 207.0f + slope * 22.0f};
}

// Every node has its row and column, A..F, F1 and F2, then S1 and S2; the output is the last column.
Point gridCellCenter(int row, int col) {
    return {72.0f + col * 32.6f + 14.3f, 140.0f + row * 32.6f + 14.3f};
}

Point nodeCenter(const UiLayout &layout, int node) {
    if (node >= kOps)
        return {56.0f + layout.boxX[node - kOps] * 772.0f, 174.0f + layout.boxY[node - kOps] * 226.0f};
    return {56.0f + layout.nodeX[node] * 772.0f, 174.0f + layout.nodeY[node] * 226.0f};
}

// A node's label on its line to OUT: halfway between its outline and the arrowhead over the bus, or left of the
// line where the node sits too close to the bus for it (mirrors outWireOf() in view.cpp). The arrowhead's middle.
Point outLabelOf(const UiLayout &layout, int node) {
    const Point c = nodeCenter(layout, node);
    const float top = c.y + (node >= kOps ? 17.0f : 24.0f), end = kBusY - 10.0f;
    return {end - top < 18.0f ? c.x - 28.0f : c.x, 0.5f * (top + end)};
}
Point outArrowOf(const UiLayout &layout, int node) {
    return {nodeCenter(layout, node).x, kBusY - 6.0f};
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
    return {912.0f + node * 25.0f + (node >= kOps ? 6.0f : 0.0f) + (node >= kOps + kFilters ? 6.0f : 0.0f), 124.0f};
}

// A shaper's page: its curve's list, the symmetric and link switches, the Drive, Output, Mix and To Out knobs, the
// list's entries, and its drawn curve in the display (x 0..1 across, y -1..1 up), with its edit button in the display's
// corner.
constexpr Point kShaperList{1009.0f, 177.0f};
constexpr Point kShaperSym{1096.0f, 177.0f};
constexpr Point kShaperLink{1146.0f, 177.0f};
Point shaperKnob(int knob) {
    return {900.0f + 268.0f / 4.0f * (knob + 0.5f), 214.0f}; // Drive, Output, Mix, To Out
}
Point shaperListItem(ClipMode mode) {
    return {1009.0f, 192.0f + 4.0f + static_cast<int>(mode) * 22.0f + 11.0f};
}
Point shaperCurveAt(double x, double y) {
    return {908.0f + static_cast<float>(x) * 252.0f, 310.0f - static_cast<float>(y) * 42.24f};
}
constexpr Point kShaperEdit{1150.0f, 353.0f};

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

// The color of the dot in the top right corner of a node's tab in the inspector, in what was saved last.
uint32_t tabDot(const Renderer &renderer, int node) {
    return renderer.pixel({nodeTab(node).x + 7.0f, 118.0f});
}

void click(View &view, Point p, Modifiers mods = {}) {
    view.mouseDown(p, mods);
    view.mouseUp(p, mods);
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
constexpr Point kUndoButton{986.0f, 84.0f};
constexpr Point kRedoButton{1020.0f, 84.0f};
// The LFO shape editor's reset button (the note curve's further left), and its grid's buttons: per axis (0 across)
// a step down (part 0), the count, a step up.
constexpr Point kShapeReset{1120.0f, 124.0f};
constexpr Point kNoteCurveReset{804.0f, 124.0f};
Point shapeGrid(int axis, int part) {
    constexpr float kParts[] = {18.0f, 28.0f, 18.0f};
    const float x = 1144.0f - (axis == 0 ? 148.0f : 64.0f) + (part > 0 ? 18.0f : 0.0f) + (part > 1 ? 28.0f : 0.0f);
    return {x + kParts[part] * 0.5f, 458.0f};
}
// A random LFO's Smooth (0) and Chaos (1) knobs, right of its narrowed display.
Point randomKnob(float card, int knob) {
    return {cardX(card) + 186.0f + knob * 50.0f, 649.0f};
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
    const int fine = paramIndex(0, OpParam::Fine);

    // A knob drag is one step: undo takes it back, redo brings it again.
    drag(view, kFineKnob, {kFineKnob.x, kFineKnob.y - 40.0f});
    check(std::abs(model.value(fine) - 0.2) < 1e-6 && model.nextUndo() != nullptr, "a knob drag is an undo step");
    click(view, kUndoButton);
    const bool undone = model.value(fine) == 0.0;
    click(view, kRedoButton);
    check(undone && std::abs(model.value(fine) - 0.2) < 1e-6 && model.openEdits == 0,
          "the undo and redo buttons take a drag back and bring it again");
    // A run of wheel notches on one knob undoes at once.
    for (int n = 0; n < 3; ++n)
        view.wheel(kFineKnob, 1.0f, {});
    view.undo();
    check(std::abs(model.value(fine) - 0.2) < 1e-6, "a run of wheel notches is one undo step");
    // Moving a node is a step too.
    const Point a = nodeCenter(model.layout(), 0);
    drag(view, a, {a.x + 100.0f, a.y - 50.0f});
    const float movedX = model.layout().nodeX[0];
    view.undo();
    check(movedX != 0.5f && model.layout().nodeX[0] == 0.5f, "undo puts a dragged node back");
    view.redo();
    check(model.layout().nodeX[0] == movedX, "redo moves it again");
    view.undo();

    // Typing values: a double-click on a knob's number opens a field, Enter sets what it reads as.
    doubleClick(view, {kFineKnob.x, 238.0f});
    const bool typingOn = view.typing();
    type(view, "50");
    view.mouseMove({kFineKnob.x, 238.0f}, {});
    check(typingOn && renderer.save(view, dir / "16-typing.png"), "render a value being typed");
    view.typeKey(View::Key::Enter);
    check(!view.typing() && std::abs(model.value(fine) - 0.5) < 1e-9, "a typed value sets the knob");
    view.undo();
    check(std::abs(model.value(fine) - 0.2) < 1e-6, "a typed value is an undo step");
    doubleClick(view, {401.0f, 35.0f}); // FM Amount's number, beside it in the header
    type(view, "150 %");
    view.typeKey(View::Key::Enter);
    const int fmAmount = paramIndex(GlobalParam::FmAmount);
    check(std::abs(model.value(fmAmount) - 1.5) < 1e-9, "a header knob takes a typed value too");
    doubleClick(view, {401.0f, 35.0f});
    type(view, "70");
    view.typeKey(View::Key::Escape);
    doubleClick(view, {401.0f, 35.0f});
    type(view, "lots");
    view.typeKey(View::Key::Enter);
    doubleClick(view, {401.0f, 35.0f});
    view.typeKey(View::Key::Enter); // as shown: no change, no step
    check(std::abs(model.value(fmAmount) - 1.5) < 1e-9, "Escape, text that isn't a value, or no change leave it");
    doubleClick(view, {401.0f, 35.0f});
    view.typeKey(View::Key::Backspace);
    type(view, "0");
    click(view, {600.0f, 300.0f}); // a click elsewhere takes the value
    check(std::abs(model.value(fmAmount)) < 1e-9 && !view.typing(), "a click elsewhere takes the value typed");
    doubleClick(view, kFineKnob);
    check(!view.typing() && model.value(fine) == 0.0, "a double-click on the knob itself still resets it");
    view.undo();
    // The right-click menu's "Edit value" types into the knob too.
    auto menuEntry = [&](Point at, const std::string &label) {
        for (const MenuItem &item : view.contextMenu(at))
            if (item.label == label)
                return item.command;
        return -1;
    };
    view.menuCommand(menuEntry(kFineKnob, tr(Str::MenuEditValue)));
    const bool menuTyping = view.typing();
    type(view, "35");
    view.typeKey(View::Key::Enter);
    check(menuTyping && std::abs(model.value(fine) - 0.35) < 1e-9, "a knob's menu has Edit value, which types into it");
    // Ctrl+Click types into a route: A's label on its line to OUT in the graph, then its OUT cell in the matrix.
    const Point aOut = outLabelOf(model.layout(), 0);
    view.mouseDown(aOut, {false, true});
    type(view, "50");
    view.mouseMove({600.0f, 300.0f}, {});
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

    // A random LFO shows Smooth and Chaos beside a preview of what they do.
    check(rateFromNorm(1.0) == 100.0, "LFOs run up to 100 Hz");
    click(view, sourceAdd(SourceType::Lfo));
    click(view, shapeButton(0));
    const bool shapeListShot = renderer.save(view, dir / "17-lfo-shape-list.png");
    click(view, shapeItem(0, LfoShape::Random));
    check(shapeListShot &&
              std::lround(model.value(paramIndex(0, SourceParam::Shape))) == static_cast<long>(LfoShape::Random),
          "an LFO's shape list picks its shape");
    drag(view, randomKnob(0, 0), {randomKnob(0, 0).x, randomKnob(0, 0).y - 120.0f});
    drag(view, randomKnob(0, 1), {randomKnob(0, 1).x, randomKnob(0, 1).y + 140.0f});
    check(std::abs(model.value(paramIndex(0, SourceParam::RandomSmooth)) - 0.6) < 1e-6 &&
              std::abs(model.value(paramIndex(0, SourceParam::RandomChaos)) - 0.3) < 1e-6,
          "a random LFO's Smooth and Chaos knobs set them");
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "17-random-lfo.png"), "render a smoothed random LFO");
    // Running, it shows its own last cycles instead, swept across: six cycles in, the newest is in the second
    // quarter, where the live dot rides it.
    model.live.voices = 1;
    const float glides[] = {0.3f, -0.4f, 0.5f, 0.1f, -0.6f, 0.2f, 0.7f};
    for (int cycle = 0; cycle < 6; ++cycle) {
        model.live.lfoFrom[0] = glides[cycle];
        model.live.lfoTo[0] = glides[cycle + 1];
        model.live.lfoPhases[0] = 0.7f;
        model.live.sources[0] = glides[cycle + 1];
        view.poll();
    }
    check(renderer.save(view, dir / "17a-random-sweep.png"), "render a running random LFO's own cycles, swept across");
    // Between snapshots the dot moves on at the LFO's rate; run past the end of the cycle before a snapshot brings
    // the next one, it waits there rather than jump back to the start of the same quarter, and so it does when a
    // snapshot's phase has wrapped round ahead of its glide.
    const Point held{cardX(0) + 26.0f + 0.5f * 125.0f, 660.0f - 0.7f * 18.0f}; // the end of the second quarter
    auto dotAt = [&](Point p) {
        const uint32_t color = renderer.pixel(p);
        return (color >> 16 & 255) > 190 && (color >> 8 & 255) > 190 && (color & 255) > 190;
    };
    model.live.lfoPhases[0] = 0.98f;
    model.live.lfoRates[0] = 2.0f;
    model.live.age = 0.05f; // a tenth of a cycle on
    view.poll();
    const bool waits = renderer.save(view, dir / "17a2-random-cycle-end.png") && dotAt(held);
    model.live.lfoPhases[0] = 0.02f;
    model.live.lfoRates[0] = 0.0f;
    model.live.age = 0.0f;
    view.poll();
    const bool stays = renderer.save(view, dir / "17a2-random-cycle-end.png") && dotAt(held);
    check(waits && stays, "a running random LFO's dot waits at the end of its cycle for the next one");
    model.live = Telemetry{};
    view.poll();

    // A card lists no modulations: the button under its display counts them and shows them in the MODS view, where
    // LFO 1's first row is the top one (its middle at y 154). There a row's number takes a typed depth, its depth
    // bar's menu has Edit depth and its dot bypasses it; back in ROUTING, the modulated knob's menu resumes it.
    for (int k = 0; k < 6; ++k) {
        model.set(slotTargetIndex(0, k), slotTargetValue(paramIndex(k, OpParam::Fine)));
        model.set(slotAmountIndex(0, k), 0.1 * (k + 1));
    }
    view.poll();
    const Point modsButton{cardX(0) + 60.0f, 708.0f};
    view.mouseMove(modsButton, {});
    const bool buttonShot = renderer.save(view, dir / "17b-card-mods-button.png");
    click(view, modsButton);
    check(buttonShot && model.ui.view == RoutingView::Mods, "a source card's button shows its modulations in MODS");
    const Point topNumber{701.0f, 154.0f};
    doubleClick(view, topNumber);
    type(view, "-25");
    view.typeKey(View::Key::Enter);
    check(std::abs(model.value(slotAmountIndex(0, 0)) + 0.25) < 1e-9, "a slot's number takes a typed depth");
    view.menuCommand(menuEntry({576.0f, 154.0f}, tr(Str::MenuEditDepth))); // on the depth bar
    type(view, "45");
    view.typeKey(View::Key::Enter);
    check(std::abs(model.value(slotAmountIndex(0, 0)) - 0.45) < 1e-9, "a depth bar's menu has Edit depth");
    click(view, {41.0f, 154.0f});
    const bool slotBypassed = model.value(slotBypassIndex(0, 0)) == 1.0;
    view.mouseMove({400.0f, 440.0f}, {});
    check(slotBypassed && renderer.save(view, dir / "17c-bypassed-slot.png"), "render a bypassed modulation");
    click(view, kRoutingTab);
    view.menuCommand(menuEntry(kFineKnob, trf(Str::MenuResume, {trf(Str::SourceNameLfo, {"1"})})));
    check(model.value(slotBypassIndex(0, 0)) == 0.0, "a row's dot bypasses its modulation, and the knob's menu resumes it");

    // Until a source modulates something, hovering its handle shows a "Drag to assign" tooltip below it (read off
    // the rendered pixels, against the same view with the mouse away).
    auto belowHandle = [&](const char *name) {
        std::vector<uint32_t> pixels;
        if (renderer.save(view, dir / name))
            for (float x = sourceHandle(0).x - 40.0f; x <= sourceHandle(0).x + 40.0f; x += 2.0f)
                pixels.push_back(renderer.pixel({x, 572.0f}));
        return pixels;
    };
    std::vector<double> targetsBefore;
    for (int slot = 0; slot < kSlots; ++slot) {
        targetsBefore.push_back(model.value(slotTargetIndex(0, slot)));
        model.set(slotTargetIndex(0, slot), 0.0);
    }
    view.poll();
    view.mouseMove({600.0f, 300.0f}, {});
    const std::vector<uint32_t> away = belowHandle("18a-handle-away.png");
    view.mouseMove(sourceHandle(0), {});
    const bool tipped = belowHandle("18b-handle-tooltip.png") != away;
    for (int slot = 0; slot < kSlots; ++slot)
        model.set(slotTargetIndex(0, slot), targetsBefore[slot]);
    view.poll();

    // Hovering a source's handle draws lines to what it modulates, and no tooltip.
    drag(view, sourceHandle(0), kFmAmountKnob);
    view.mouseMove({600.0f, 300.0f}, {});
    const std::vector<uint32_t> linkedAway = belowHandle("18-source-links.png");
    view.mouseMove(sourceHandle(0), {});
    check(tipped && belowHandle("18-source-links.png") == linkedAway,
          "a source's handle shows its tooltip only until it modulates something");

    // The uncommon waves sit in a list behind "...".
    click(view, kMoreWavesButton);
    view.mouseMove({1102.0f, 232.0f}, {});
    check(renderer.save(view, dir / "19-more-waves.png"), "render the list of more waves");
    click(view, {1102.0f, 208.0f}); // its first entry, Pulse
    view.mouseMove({600.0f, 300.0f}, {});
    check(model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Pulse) &&
              renderer.save(view, dir / "19b-pulse.png"),
          "the list behind ... picks an uncommon wave");
    // Custom, at the end of that list, opens the partial editor: a stroke across the level bars draws them
    // (half way up the bars, which run from -60 dB to 0 dB, is -30 dB), and a double-click takes a bar to nothing.
    click(view, kMoreWavesButton);
    click(view, {1102.0f, 208.0f + 6.0f * 24.0f});
    const bool customPicked = model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Custom);
    // Picked after the 25 % pulse, the partials start as the pulse's: the first loudest, the fourth silent.
    const bool fromPulse = std::abs(model.value(partialLevelIndex(0, 0)) - 1.0) < 1e-9 &&
                           std::abs(model.value(partialLevelIndex(0, 1)) - std::sqrt(0.5)) < 0.01 &&
                           model.value(partialLevelIndex(0, 3)) == 0.0;
    check(fromPulse, "picking Custom starts its partials from the wave it was");
    auto barX = [](int partial, int count = kDefaultPartials) { return 56.0f + (partial + 0.5f) * 772.0f / count; };
    const float halfway = 286.0f + 84.0f * 0.5f;
    drag(view, {barX(1), halfway}, {barX(3), halfway});
    doubleClick(view, {barX(0), halfway});
    bool drawn = model.value(partialLevelIndex(0, 0)) == 0.0;
    for (int partial = 1; partial <= 3; ++partial)
        drawn = drawn && std::abs(20.0 * std::log10(model.value(partialLevelIndex(0, partial))) + 30.0) < 1.5;
    drag(view, {barX(2), 402.0f + 48.0f * 0.75f}, {barX(2), 402.0f + 48.0f * 0.75f});
    view.mouseMove({barX(2), halfway}, {});
    check(customPicked && drawn && std::abs(model.value(partialPhaseIndex(0, 2)) - 0.25) < 0.03 &&
              renderer.save(view, dir / "23-partials.png"),
          "picking Custom opens the partial editor, which draws levels and phases");
    // Its resolution, 16, 32 or 64 partials, in the title row: at 64, the 50th can be drawn; at 16, the bars widen
    // to the first 16 and the 50th, unheard, keeps its level, there again at 64.
    auto partialCount = [](int choice) { return Point{177.0f + choice * 36.0f, 124.0f}; };
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
    click(view, kRoutingEditorDone);
    // While the wave is Custom, a pencil on the summary line, left of the keyboard switch, opens the partial editor
    // again, lit while it is open, and a second click closes it (read off the rendered pixels: the editor's title).
    {
        constexpr Point kPartialEdit{1095.0f, 150.0f};
        auto partialsTitle = [&] {
            std::vector<uint32_t> pixels;
            for (float y = 114.0f; y < 134.0f; y += 2.0f)
                for (float x = 34.0f; x < 150.0f; x += 3.0f)
                    pixels.push_back(renderer.pixel({x, y}));
            return pixels;
        };
        view.mouseMove({600.0f, 300.0f}, {});
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
        view.mouseMove({600.0f, 300.0f}, {});
        const bool shut = renderer.save(view, dir / "23f-partials-shut.png") && partialsTitle() == closed;
        check(closedShot && openShot && opened && draws && shut &&
                  model.value(partialLevelIndex(0, 5)) == level,
              "a Custom wave's edit button opens its partial editor, and a second click closes it");
    }
    // The list's newer waves: Alt Sine, Organ and Vowel, between Abs Sine and Custom.
    click(view, kMoreWavesButton);
    click(view, {1102.0f, 208.0f + 4.0f * 24.0f});
    check(model.value(paramIndex(0, OpParam::Wave)) == static_cast<double>(OpWave::Organ), "the list picks Organ");
    view.undo();

    // The shape editor spans the inspector, which it hides: no operator tab or knob where they were. Its grid: a
    // step up across is 24 lines, and Ctrl snaps a point to them.
    click(view, shapeButton(0));
    click(view, shapeItem(0, LfoShape::Custom));
    const double fineBefore = model.value(paramIndex(0, OpParam::Fine));
    drag(view, kFineKnob, {kFineKnob.x, kFineKnob.y - 30.0f});
    check(model.value(paramIndex(0, OpParam::Fine)) == fineBefore && model.value(shapeCountIndex(0)) == 2.0,
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
    click(view, kShapeDone);

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
    click(view, {600.0f, 400.0f});
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
    view.mouseMove({600.0f, 300.0f}, {});
    check(renderer.save(view, dir / "21b-new-fx.png"), "render the reverb, phaser, flanger and frequency shifter");
    // The reverb's algorithm comes from a list under its button; its display follows the Size and the algorithm.
    click(view, {fxCardX(model, 1) + 40.0f, 152.0f + 44.0f + 11.0f});
    view.mouseMove({fxCardX(model, 1) + 40.0f, 237.0f + 3.0f * 22.0f}, {});
    const bool listed = renderer.save(view, dir / "21c-reverb-list.png");
    click(view, {fxCardX(model, 1) + 40.0f, 237.0f + 22.0f}); // Room
    check(listed && model.value(paramIndex(1, FxParam::ReverbType)) == static_cast<double>(ReverbType::Room),
          "the reverb's list picks its algorithm");
    model.set(paramIndex(1, FxParam::ReverbSize), 0.1);
    view.mouseMove({600.0f, 300.0f}, {});
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
    view.mouseMove({600.0f, 200.0f}, {});
    check(routeModulated && renderer.save(view, dir / "18b-modulated-route.png"),
          "render a modulated route, its wire swelling and its label showing the range");
    // That route turned down to 0 %: B, sounding, lights up only while its modulation opens the route again, just
    // above it (read off the rendered pixels).
    const double routed = model.value(matrixIndex(1, 0));
    model.set(matrixIndex(1, 0), 0.0);
    model.live.opLevels[1] = 1.0f;
    auto aboveB = [&](float offset) {
        model.live.modOffset[matrixIndex(1, 0)] = offset;
        view.poll();
        renderer.save(view, dir / "18b2-modulated-route-glow.png");
        return renderer.pixel({bNode.x, bNode.y - 28.0f});
    };
    const uint32_t shut = aboveB(0.0f), opened = aboveB(0.3f);
    check(opened != shut, "an operator lights up while modulation opens its route from 0 %");
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
        return static_cast<int>(renderer.pixel({910.0f + 0.75f * 248.0f, 301.0f}) >> 16 & 255);
    };
    const int fillC4 = fillAt(60.0f), fillC7 = fillAt(96.0f);
    check(fillC7 > fillC4 + 10, "a filter's response moves with the note its key tracking follows");
    model.set(keyTrack, 0.0);
    model.live = Telemetry{};
    view.poll();

    // A modulated filter: its response shows how far the modulation reaches, and follows it while notes play.
    drag(view, sourceHandle(0), {918.0f, 214.0f}); // onto its Cutoff knob
    bool cutoffModulated = false;
    for (int slot = 0; slot < kSlots; ++slot)
        cutoffModulated = cutoffModulated || slotTarget(model.value(slotTargetIndex(0, slot))) == paramIndex(0, FilterParam::Cutoff);
    model.live.voices = 1;
    model.live.modOffset[paramIndex(0, FilterParam::Cutoff)] = 0.18f;
    view.poll();
    view.mouseMove({600.0f, 200.0f}, {});
    check(cutoffModulated && renderer.save(view, dir / "18c-modulated-filter.png"),
          "render a modulated filter's response, its reach and where it is now");
    model.live = Telemetry{};
    view.poll();
    click(view, {912.0f, 124.0f}); // back to operator A

    // Presets: the bar shows Init; the arrows step through the library, asking first while the patch has
    // changed since it was loaded; the browser lists them by subfolder.
    check(model.presetName() == tr(Str::PresetInit) && model.patchEdited(), "a changed patch shows Init, changed");
    const Point nextPreset{787.0f, 84.0f}, previousPreset{413.0f, 84.0f}, presetBox{600.0f, 84.0f};
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
    const Point headerGap{300.0f, 4.0f}; // above the header's knobs
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
    // Undo and redo bring back which preset it was, and whether it had changed since.
    model.loadPresetAt(2);
    view.undo();
    const bool backToBell = model.presetName() == "Bell" && !model.patchEdited() &&
                            model.value(paramIndex(GlobalParam::Volume)) == -18.0;
    view.redo();
    check(backToBell && model.presetName() == "Pad" && !model.patchEdited(),
          "undo and redo across a preset load bring back the preset it was");
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
        click(view, sourceAdd(SourceType::Lfo));
    model.set(paramIndex(4, SourceParam::Sync), 0.0); // the fifth free, its knob a rate rather than a note value
    view.wheel({60.0f, 513.0f}, 10.0f, {}); // back to the first card
    view.mouseDown(sourceHandle(0), {});
    view.mouseMove({1180.0f, 650.0f}, {});
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
    view.wheel({600.0f, 125.0f}, 10.0f, {}); // back to the start
    fxScrolled = 0.0f;
    const int firstFx = fxChain(model).front();
    view.mouseDown(fxGrip(model, 0), {});
    view.mouseMove({fxGrip(model, 0).x + 300.0f, 169.0f}, {});
    view.mouseMove({1180.0f, 169.0f}, {}); // at the panel's right end
    for (int n = 0; n < 80; ++n)
        view.poll();
    const bool fxShot = renderer.save(view, dir / "8c-fx-edge-scroll.png");
    view.mouseUp({1180.0f, 169.0f}, {});
    check(fxShot && fxChain(model).size() == 8 && fxChain(model).back() == firstFx,
          "an effect card held at the row's right end scrolls the row along, and lands at the end");

    for (int n = 0; n < 5; ++n)
        click(view, sourceAdd(SourceType::Lfo));
    view.wheel({60.0f, 513.0f}, 10.0f, {}); // back to the first card
    view.mouseDown(sourceGrip(0), {});
    view.mouseMove({sourceGrip(0).x + 200.0f, 544.0f}, {});
    view.mouseMove({1180.0f, 544.0f}, {});
    for (int n = 0; n < 80; ++n)
        view.poll();
    view.mouseUp({1180.0f, 544.0f}, {});
    const bool last = shown(model) == std::vector<int>{1, 2, 3, 4, 0};
    // Scrolled to the end, it shows in the fifth place, before the dashed cards.
    const Point grip = sourceGrip(3.0f - kSourceShadowSpan);
    view.mouseDown(grip, {});
    view.mouseMove({grip.x - 200.0f, 544.0f}, {});
    view.mouseMove({20.0f, 544.0f}, {}); // at the panel's left end
    for (int n = 0; n < 80; ++n)
        view.poll();
    const bool sourceShot = renderer.save(view, dir / "11f-source-edge-scroll.png");
    view.mouseUp({20.0f, 544.0f}, {});
    check(last && sourceShot && shown(model) == std::vector<int>{0, 1, 2, 3, 4},
          "an LFO card held at either end of the row scrolls the row along, and lands at that end");
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
    view.wheel({600.0f, 125.0f}, 10.0f, {}); // back to the start
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
    const Point grab{700.0f, 300.0f}; // on a card's knob
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
        click(view, sourceAdd(SourceType::Lfo));
    view.wheel({60.0f, 513.0f}, 10.0f, {}); // back to the first card
    const std::vector<int> sources = shown(model);
    view.middleDown({cardX(1.0f) + 120.0f, 650.0f});
    view.mouseMove({cardX(0.0f) + 120.0f, 650.0f}, {});
    view.middleUp({cardX(0.0f) + 120.0f, 650.0f});
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
    const OpParam targets[] = {OpParam::Fine,    OpParam::Detune,  OpParam::Transpose, OpParam::Out,
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
    click(modsView, {314.0f, 88.0f}); // the MODS tab
    auto removedOnly = [&](int slot) {
        click(modsView, {1144.0f, 154.0f}); // the top row's x
        bool ok = true;
        for (int k = 0; k < 11; ++k)
            ok = ok && (slotTarget(mods.value(slotTargetIndex(0, k))) < 0) == (k == slot);
        modsView.undo();
        return ok && slotTarget(mods.value(slotTargetIndex(0, slot))) == paramIndex(0, targets[slot]);
    };
    modsView.middleDown({600.0f, 300.0f});
    modsView.mouseMove({610.0f, 300.0f - 60.0f}, {});
    const bool modsShot = renderer.save(modsView, dir / "35p-mods-middle-drag.png");
    modsView.middleUp({610.0f, 300.0f - 60.0f});
    const bool scrolled = removedOnly(2);
    modsView.middleDown({600.0f, 200.0f});
    modsView.mouseMove({600.0f, 200.0f + 400.0f}, {});
    modsView.middleUp({600.0f, 200.0f + 400.0f});
    check(modsShot && scrolled && removedOnly(0), "the middle button drags the MODS view's rows up and down");

    // The preset browser's list of forty, five rows up: the top row is the sixth preset's.
    FakeController presets;
    View presetView(presets);
    presets.library.clear();
    for (int n = 0; n < 40; ++n)
        presets.library.push_back({"Preset " + std::to_string(n), ""});
    presets.loadPresetAt(0); // unchanged since, so picking another doesn't ask first
    click(presetView, {600.0f, 84.0f}); // the preset's name opens the browser
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
// ranges on Op A's Fine (set to the middle) overlap above the knob, Env 1's on top, and hovering either handle
// changes what shows there (read off the rendered pixels).
void testModFocus(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, sourceAdd(SourceType::Lfo));
    click(view, sourceAdd(SourceType::Env));
    model.set(paramIndex(0, OpParam::Fine), 0.5);
    drag(view, sourceHandle(0), kFineKnob);
    drag(view, sourceHandle(1), kFineKnob);
    auto ring = [&] { // above the knob, where both ranges run
        std::vector<uint32_t> pixels;
        for (float y = 192.0f; y < 207.0f; y += 1.0f)
            for (float x = kFineKnob.x - 20.0f; x < kFineKnob.x + 21.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    view.mouseMove({600.0f, 300.0f}, {});
    const bool plainShot = renderer.save(view, dir / "17c-mod-ranges.png");
    const std::vector<uint32_t> plain = ring();
    view.mouseMove({kFineKnob.x - 5.0f, 229.0f}, {}); // LFO 1's handle, the first under the knob
    const bool lfoShot = renderer.save(view, dir / "17d-mod-range-hovered.png");
    const std::vector<uint32_t> lfo = ring();
    view.mouseMove({kFineKnob.x + 5.0f, 229.0f}, {}); // Env 1's, the second
    renderer.save(view, dir / "17e-mod-range-hovered.png");
    const std::vector<uint32_t> env = ring();
    check(model.value(slotTargetIndex(0, 0)) == slotTargetValue(paramIndex(0, OpParam::Fine)) &&
              model.value(slotTargetIndex(1, 0)) == slotTargetValue(paramIndex(0, OpParam::Fine)) && plainShot &&
              lfoShot && lfo != plain && env != plain && lfo != env,
          "a hovered depth handle draws its range wider, over the others");
}

// The examples that use the most of the synth, as the preset library writes them: Init, then their changes,
// with their own node places and their name in the preset bar. `prefix` names the images: its routing, matrix
// and effects.
void renderExample(const std::filesystem::path &dir, Renderer &renderer, const std::string &name,
                   const std::string &prefix, size_t effects) {
    FakeController model;
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
    view.mouseMove({600.0f, 300.0f}, {});
    bool shots = renderer.save(view, dir / (prefix + "-routing.png"));
    click(view, kMatrixTab);
    view.mouseMove({600.0f, 300.0f}, {});
    shots = shots && renderer.save(view, dir / (prefix + "-matrix.png"));
    click(view, kFxTab);
    view.mouseMove({600.0f, 300.0f}, {});
    shots = shots && renderer.save(view, dir / (prefix + "-fx.png"));
    check(shots && fxChain(model).size() == effects,
          ("render " + name + "'s routing, matrix and " + std::to_string(effects) + " effects").c_str());
}

// A stereo phaser and a flanger mid-sweep (their LFOs where the audio left them), a phase disperser and a filter,
// each with its output's spectrum behind its curve, and a widener, half of it in the middle.
void renderNewEffects(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(GlobalParam::FxOrder)] = fxOrderStarting({0, 1, 2, 3, 4});
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
    click(view, {fxCardX(model, 0) + 31.0f, 152.0f + 171.0f});
    const int division = paramIndex(0, FxParam::PhaserDivision), rate = paramIndex(0, FxParam::PhaserRate);
    const double divisionBefore = model.value(division), rateBefore = model.value(rate);
    drag(view, fxKnob(model, 0, 0, 3, 206.0f), {fxKnob(model, 0, 0, 3, 206.0f).x, fxKnob(model, 0, 0, 3, 206.0f).y - 20.0f});
    check(model.value(paramIndex(0, FxParam::PhaserSync)) == 1.0 && model.value(division) != divisionBefore &&
              model.value(rate) == rateBefore,
          "the phaser's SYNC turns its Rate knob into a note value");
    // The flanger's STEREO, in the middle under its display like the phaser's, is on to begin with.
    const int flangerStereo = paramIndex(1, FxParam::FlangerStereo);
    const Point stereo{fxCardX(model, 1) + fxCardW(model, 1) * 0.5f, 152.0f + 171.0f};
    click(view, stereo);
    const bool flangerMono = model.value(flangerStereo) == 0.0;
    click(view, stereo);
    check(flangerMono && model.value(flangerStereo) == 1.0, "the flanger's STEREO switches its sides apart and together");
    // The filter's Key Track, the third knob of its first row.
    const int keyTrack = paramIndex(3, FxParam::FilterKeyTrack);
    drag(view, fxKnob(model, 3, 2, 3, 206.0f), {fxKnob(model, 3, 2, 3, 206.0f).x, fxKnob(model, 3, 2, 3, 206.0f).y - 20.0f});
    check(model.value(keyTrack) > 0.0, "the filter effect's Key Track knob turns");
    view.mouseMove({600.0f, 300.0f}, {});
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
        const uint32_t px = renderer.pixel({fxCardX(model, 3) + 24.0f + 0.85f * (fxCardW(model, 3) - 48.0f), 272.0f});
        return static_cast<int>((px >> 16 & 255) + (px >> 8 & 255) + (px & 255));
    };
    const int fillC4 = fillAt(60.0f), fillC7 = fillAt(96.0f);
    check(fillC7 > fillC4 + 10, "the filter effect's response moves with the note its key tracking follows");
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
    model.set(paramIndex(0, OpParam::Enabled), 1.0);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(shaperSendIndex(0, shaper), 0.8);
    model.set(shaperSendIndex(1, shaper), 0.5);
    model.set(shaperModIndex(shaper, 2), 0.4);
    model.set(paramIndex(2, OpParam::Enabled), 1.0);
    view.mouseMove({600.0f, 300.0f}, {});
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
void testFolding(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.values[paramIndex(GlobalParam::FxOrder)] = fxOrderStarting({0, 1, 2});
    const FxType types[] = {FxType::Delay, FxType::Reverb, FxType::Phaser};
    for (int slot = 0; slot < 3; ++slot)
        model.values[paramIndex(slot, FxParam::Type)] = static_cast<double>(types[slot]);
    model.values[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    model.values[paramIndex(1, SourceParam::Type)] = static_cast<double>(SourceType::Env);
    view.poll();
    click(view, kFxTab);
    const float cardTop = 151.5f; // an effect card's
    auto fxFold = [&](int position) {
        return Point{fxCardX(model, position) + fxCardW(model, position) - 39.0f, cardTop + 18.5f};
    };
    const Point icon = fxPower(model, 1);
    click(view, fxFold(1));
    const float strip = fxCardX(model, 1) + fxCardW(model, 1) * 0.5f;
    const bool fxFolded = model.ui.fxFolded == 1u << 1 && std::abs(strip - icon.x) < 0.01f; // the icon in the middle
    view.mouseMove(icon, {});
    const bool shot = renderer.save(view, dir / "28a-folded-icon.png");
    click(view, icon); // the folded card's icon, where it was before, switches it
    const bool switched = model.value(paramIndex(1, FxParam::Enabled)) < 0.5;
    click(view, {fxCardX(model, 2) + 40.0f, 400.0f}); // the next card, closed up: its own field, no change
    const bool closedUp = model.ui.fxFolded == 1u << 1 &&
                          std::abs(fxCardX(model, 2) - (32.0f + (1.0f + kFoldedSpan) * kFxStep)) < 0.1f;
    click(view, {strip, 300.0f}); // the strip's name unfolds it
    const bool fxUnfolded = model.ui.fxFolded == 0;
    click(view, fxFold(1));
    click(view, fxRemove(model, 0)); // removing another effect leaves the fold alone
    const bool kept = model.ui.fxFolded == 1u << 1 && fxChain(model).size() == 2;

    const float sourceTop = 536.0f; // a source card's
    click(view, {cardX(0) + kCardW - 69.0f, sourceTop + 17.5f}); // the LFO's chevron, left of its switch
    const bool sourceFolded = model.ui.sourcesFolded == 1u << 0;
    const float envX = cardX(0) + 0.2f * (kCardW + 12.0f); // the envelope's card, closed up
    click(view, {envX + kCardW - 43.0f, sourceTop + 17.5f}); // its switch
    const bool envClosedUp = model.value(sourceEnabledIndex(1)) < 0.5;
    const float lfoStrip = cardX(0) + 0.5f * (0.2f * (kCardW + 12.0f) - 12.0f);
    drag(view, {lfoStrip, sourceTop + 38.0f}, {lfoStrip + 300.0f, sourceTop + 38.0f}); // its grip, past the envelope
    const bool moved = shown(model) == std::vector<int>{1, 0} && model.ui.sourcesFolded == 1u << 0;
    view.mouseMove({600.0f, 300.0f}, {});
    check(fxFolded && shot && switched && closedUp && fxUnfolded && kept && sourceFolded && envClosedUp && moved &&
              renderer.save(view, dir / "28-folded.png"),
          "effect and LFO cards fold to strips that still switch, move and unfold, the effect's icon staying put");
}

// The macros in the keyboard panel, above the keys: a knob each (centers 112.75 px apart, 179 px above the
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
    view.mouseMove({600.0f, 300.0f}, {});
    const bool idleShot = renderer.save(view, dir / "26a-macros-idle.png");
    const int macroIdle = ringLevel(renderer, macroRing, 20.0f), noteIdle = ringLevel(renderer, noteRing, 20.0f);
    drag(view, macroKnob(0), {macroKnob(0).x, y - 40.0f});
    const bool turned = model.value(paramIndex(0, MacroParam::Value)) > 0.1;
    drag(view, {macroKnob(0).x - 32.0f, y}, kFmAmountKnob); // its handle onto FM Amount
    const bool mapped =
        slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == paramIndex(GlobalParam::FmAmount);
    view.mouseMove({600.0f, 300.0f}, {});
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
    drag(view, noteHandle, {918.0f, 214.0f});
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
    for (const MenuItem &item : view.contextMenu({918.0f, 214.0f}))
        if (item.label.find(tr(Str::SourceNameNote)) != std::string::npos)
            removal = item.label;
    check(noteMapped && shot && removal == trf(Str::MenuRemoveModulation, {tr(Str::SourceNameNote)}),
          "the note source's handle maps it like a macro's, and its display names the note");

    // The mod wheel's handle, in the row between the macros and the note source, maps it the same way (faint until it
    // does), onto FM Amount here, where it starts upward, as every new modulation does.
    {
        const Point wheelHandle{928.0f, y}, wheelRing{934.0f, y - 10.0f};
        view.mouseMove({600.0f, 300.0f}, {});
        renderer.save(view, dir / "26f-wheel-idle.png");
        const int idle = ringLevel(renderer, wheelRing, 20.0f);
        drag(view, wheelHandle, kFmAmountKnob);
        const bool wheelMapped =
            slotTarget(model.value(slotTargetIndex(kWheelSource, 0))) == paramIndex(GlobalParam::FmAmount) &&
            model.value(slotAmountIndex(kWheelSource, 0)) == 0.3;
        view.mouseMove({600.0f, 300.0f}, {});
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
        view.mouseMove({600.0f, 300.0f}, {});
        const bool stripShot = renderer.save(view, dir / "26h-bipolar-strip.png");
        polarity({928.0f, y}, tr(Str::MenuUnipolar)); // the wheel's handle
        polarity(macroKnob(0), tr(Str::MenuUnipolar));
        polarity({1026.0f, y}, tr(Str::MenuUnipolar));
        check(macroWasUnipolar && macroBipolar && handleShowsIt && wheelBipolar && noteBipolar && stripShot &&
                  model.value(paramIndex(0, MacroParam::Bipolar)) == 0.0 &&
                  model.value(paramIndex(WheelParam::Bipolar)) == 0.0 && model.value(paramIndex(NoteParam::Bipolar)) == 0.0,
              "the macros', the mod wheel's and the note source's menus make them unipolar or bipolar");
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
        const bool shot = renderer.save(view, dir / "26i-octave-up.png");
        click(view, down);
        click(view, firstC);
        check(before == 24 && shifted == 36 && model.lastNote == 24 && shot,
              "the octave shift's buttons under the keys move them an octave up and back");
    }

    // Collapsed by its chevron or its title, the keyboard panel keeps its top row and the editor gets 120 px
    // shorter: the keys, the wheels and the octave shift go, and the mod wheel's handle stays in the row between the
    // macros and the note source, where it still maps the wheel, onto filter 1's cutoff here. Bend's knob, over the
    // bend wheel (27 px below the row's middle, at x 46), stays in the row too, and the wheel turns it a semitone.
    {
        const Point toggle{35.0f, y - 8.0f}, title{80.0f, y - 8.0f};
        const float open = view.height();
        click(view, toggle);
        const bool shorter = model.ui.keysCollapsed && view.height() == open - 120.0f;
        const Point wheelHandle{928.0f, y};
        drag(view, wheelHandle, {918.0f, 214.0f});
        const bool wheelMapped =
            slotTarget(model.value(slotTargetIndex(kWheelSource, 1))) == paramIndex(0, FilterParam::Cutoff);
        const double bendWas = model.value(paramIndex(GlobalParam::BendRange));
        view.wheel({46.0f, y + 27.0f}, 1.0f, {});
        const bool bendTurned = model.value(paramIndex(GlobalParam::BendRange)) == bendWas + 1.0;
        view.mouseMove(toggle, {});
        const bool shot = renderer.save(view, dir / "26j-keys-collapsed.png");
        click(view, title);
        check(shorter && wheelMapped && bendTurned && shot && !model.ui.keysCollapsed && view.height() == open,
              "the keyboard panel collapses to its top row, the mod wheel's handle and Bend's knob staying in it, "
              "and opens again");
    }

    // Its display opens its curve over the routing panel: the notes from Low to High across, their knobs in the
    // title row (whole notes as they are dragged); a double-click adds a point to its three, and the reset button
    // goes back to every MIDI note along the cutoff knobs' curve.
    click(view, {1026.0f, y});
    const int low = paramIndex(NoteParam::Low), high = paramIndex(NoteParam::High);
    const double lowFrom = model.value(low);
    drag(view, {182.0f, 124.0f}, {182.0f, 24.0f}); // Low's knob, 100 px up: ten notes, landing on a whole one
    const bool raised = model.value(low) == std::round(lowFrom) + 10.0;
    view.doubleClick(noteCurveAt(0.5, 0.6), {});
    const bool pointAdded = model.value(paramIndex(NoteParam::Curve)) == 4.0;
    model.live.notePitch = 72.0f;
    view.poll();
    view.mouseMove(noteCurveAt(0.5, 0.6), {});
    const bool curveShot = renderer.save(view, dir / "26c-note-curve.png");
    click(view, kNoteCurveReset);
    const bool reset = model.value(low) == paramTable()[low].def && model.value(high) == paramTable()[high].def &&
                       model.value(paramIndex(NoteParam::Curve)) == 3.0;
    click(view, kRoutingEditorDone);
    check(raised && pointAdded && curveShot && reset,
          "the note source's display opens its curve, with its range's knobs, and the reset button puts both back");
}

// The output meter left of Volume: each side's peak in the theme's color, and the zone past full scale red while the
// clip light is on (read off the rendered pixels).
void testMeter(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.live.outPeak = {0.5f, 0.125f}; // -6 and -18 dB
    view.poll();
    const bool shot = renderer.save(view, dir / "26d-meter.png");
    const uint32_t left = renderer.pixel({211.5f, 27.0f}), right = renderer.pixel({220.5f, 27.0f}); // about -14 dB
    const uint32_t dark = renderer.pixel({211.5f, 14.5f});
    model.live.outPeak = {1.4f, 1.4f};
    model.live.clipHold = 1.0f;
    view.poll();
    const bool clipShot = renderer.save(view, dir / "26e-meter-clipping.png");
    const uint32_t lit = renderer.pixel({211.5f, 14.5f});
    check(shot && clipShot && (left & 255) > 200 && (right & 255) < 100 && (dark >> 16 & 255) < 120 &&
              (lit >> 16 & 255) > 200 && (lit >> 8 & 255) < 100,
          "the output meter shows each side's peak, and its clip light past full scale");
}

// The dashed cards after the sources: an LFO's above an envelope's, each adding one.
// The dashed card after the sources (with none yet, in the first place) opens the list of source types over itself,
// from 8 px in: an entry 26 px a type, from y 536 + 12. LFO adds an LFO; after it, a click around the list closes it
// adding nothing, and Envelope adds an envelope.
void testSourceShadow(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    auto entry = [](float card, SourceType type) {
        return Point{cardX(card) + 70.0f, 536.0f + 12.0f + 26.0f * (static_cast<float>(type) - 1.0f) + 12.0f};
    };
    click(view, {cardX(0) + 46.0f, 600.0f});
    view.mouseMove(entry(0, SourceType::Midi), {});
    const bool menuShot = renderer.save(view, dir / "29b-source-menu.png");
    click(view, entry(0, SourceType::Lfo));
    const bool lfo = shown(model) == std::vector<int>{0} &&
                     model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Lfo);
    click(view, {cardX(1) + 46.0f, 600.0f}); // after it
    click(view, {600.0f, 300.0f});           // around the list
    const bool closed = shown(model) == std::vector<int>{0};
    click(view, {cardX(1) + 46.0f, 600.0f});
    click(view, entry(1, SourceType::Env));
    const bool env = shown(model) == std::vector<int>{0, 1} &&
                     model.value(paramIndex(1, SourceParam::Type)) == static_cast<double>(SourceType::Env);
    view.mouseMove({cardX(2) + 46.0f, 600.0f}, {});
    check(menuShot && lfo && closed && env && renderer.save(view, dir / "29-source-shadow.png"),
          "the dashed card after the sources opens the list of source types, which adds the one picked");
    // Left open while the panel collapses (as a state load or another editor can do), the list shuts with it, and
    // doesn't come back when the panel opens again: a click where its LFO entry was opens it afresh instead.
    click(view, {cardX(2) + 46.0f, 600.0f});
    model.ui.sourcesCollapsed = true;
    view.poll();
    model.ui.sourcesCollapsed = false;
    view.poll();
    click(view, entry(2, SourceType::Lfo));
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
    view.mouseMove({600.0f, 300.0f}, {});
    renderer.save(view, dir / "26k-velocity-idle.png");
    const int idle = ringLevel(renderer, ring, 20.0f);
    drag(view, handle, kFineKnob);
    const bool mapped = slotTarget(model.value(slotTargetIndex(kVelocitySource, 0))) == paramIndex(0, OpParam::Fine) &&
                        model.value(slotAmountIndex(kVelocitySource, 0)) == 0.3;
    model.live.voices = 1;
    model.live.noteVelocity = 100.0f / 127.0f;
    view.poll();
    view.mouseMove({600.0f, 300.0f}, {});
    const bool shot = renderer.save(view, dir / "26l-velocity.png");
    const int live = ringLevel(renderer, ring, 20.0f);
    std::string removal;
    for (const MenuItem &item : view.contextMenu(kFineKnob))
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
    view.mouseMove({600.0f, 300.0f}, {});
    check(pointAdded && curveShot && reset && renderer.save(view, dir / "26n-velocity-closed.png"),
          "velocity's display opens its curve, where a point is added, and the reset button straightens it");
}

// A wire in the graph bends when dragged sideways (its label following its middle), keeps the bend through a save of
// the layout and back, goes back to how it bowed with a double-click or Reset Bend, and each is an undo step.
void testWireBend(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(1, OpParam::Enabled), 1.0);
    model.set(matrixIndex(1, 0), 0.5);
    model.ui.keptRoutes.push_back(matrixIndex(1, 0));
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
    view.mouseMove({600.0f, 440.0f}, {});
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
        for (float y = 66.0f; y < 104.0f; y += 2.0f)
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
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Fine)));
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
    click(view, {314.0f, 88.0f});
    view.mouseMove({600.0f, 440.0f}, {});
    const bool shot = renderer.save(view, dir / "35-mods.png");
    check(model.ui.view == RoutingView::Mods && shot, "the MODS tab shows every modulation as a row");
    const float y = 154.0f;
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
    // + Aux opens None and the sources by category; over Envelopes (the third entry) Env 1 shows beside it, and
    // picking it makes it the aux.
    click(view, {950.0f, y});
    view.mouseMove({940.0f, 167.0f + 4.0f + 2.0f * 22.0f + 11.0f}, {});
    const bool auxShot = renderer.save(view, dir / "35e-mods-aux-menu.png");
    click(view, {1080.0f, 211.0f + 4.0f + 11.0f});
    check(auxShot && slotAuxSource(model.value(slotAuxIndex(0, 0))) == 1, "+ Aux's menu picks an aux from its category");
    // The target opens every target by category, under its search (30 px, the level 240 px wide from x 234):
    // Operators, then B, then (under PITCH) Detune, three levels deep.
    click(view, {300.0f, y});
    view.mouseMove({300.0f, 200.0f + 4.0f + 22.0f + 11.0f}, {});       // Operators
    view.mouseMove({530.0f, 222.0f + 4.0f + 22.0f + 11.0f}, {});       // B
    const Point detune{650.0f, 244.0f + 4.0f + 20.0f + 2.0f * 22.0f + 11.0f};
    view.mouseMove(detune, {});
    const bool menuShot = renderer.save(view, dir / "35a-mods-target-menu.png");
    click(view, detune);
    const bool retargeted = slotIs(0, 0, paramIndex(1, OpParam::Detune)) &&
                            std::abs(model.value(slotAmountIndex(0, 0)) - 0.4) < 1e-9 &&
                            std::abs(model.value(slotScaleIndex(0, 0)) - 0.75) < 1e-6 &&
                            slotAuxSource(model.value(slotAuxIndex(0, 0))) == 1;
    view.undo();
    check(menuShot && retargeted && slotIs(0, 0, paramIndex(0, OpParam::Fine)),
          "a row's target menu moves its modulation to another target, keeping the rest, undoably");
    // Its source moves it to a free slot of another source, Macro 1 here, with its depth, Amount and aux, in one undo
    // step.
    click(view, {90.0f, y});
    view.mouseMove({80.0f, 170.0f + 4.0f + 2.0f * 22.0f + 11.0f}, {}); // Macros
    view.mouseMove({200.0f, 214.0f + 4.0f + 11.0f}, {});              // Macro 1
    const bool sourceShot = renderer.save(view, dir / "35b-mods-source-menu.png");
    click(view, {200.0f, 214.0f + 4.0f + 11.0f});
    const bool moved = slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                       slotIs(kMaxSources, 1, paramIndex(0, OpParam::Fine)) &&
                       std::abs(model.value(slotAmountIndex(kMaxSources, 1)) - 0.4) < 1e-9 &&
                       std::abs(model.value(slotScaleIndex(kMaxSources, 1)) - 0.75) < 1e-6 &&
                       slotAuxSource(model.value(slotAuxIndex(kMaxSources, 1))) == 1;
    view.undo();
    check(sourceShot && moved && slotIs(0, 0, paramIndex(0, OpParam::Fine)) &&
              slotTarget(model.value(slotTargetIndex(kMaxSources, 1))) < 0,
          "a row's source menu moves its modulation to another source with its settings, undoably");
    // Thirteen rows: the wheel over them (not on a bar, which it turns) scrolls them, gliding a row a notch; two
    // notches down, the top row's x is LFO 1's third modulation's.
    view.wheel({300.0f, 300.0f}, -2.0f, {});
    for (int i = 0; i < 30; ++i)
        view.poll();
    view.mouseMove({600.0f, 440.0f}, {});
    const bool scrolledShot = renderer.save(view, dir / "35c-mods-scrolled.png");
    click(view, {1144.0f, y});
    const bool removed = slotTarget(model.value(slotTargetIndex(0, 2))) < 0 && slotIs(0, 1, paramIndex(0, fillers[0]));
    view.undo();
    check(scrolledShot && removed && slotIs(0, 2, paramIndex(0, fillers[1])),
          "the wheel scrolls the MODS view's rows, the scrolled-to row's controls under the mouse");
    // Scrolled to the end, Macro 1's row (the last of the thirteen, the add row after it) has a bypass dot like the
    // others: a click bypasses its modulation, and another lets it go again.
    view.wheel({300.0f, 300.0f}, -10.0f, {});
    for (int i = 0; i < 40; ++i)
        view.poll();
    // The rows are 30 px apart, 28 high, in a list 326 high whose first row's middle is at y 154: the fourteen (the
    // add row too) scroll as far as the first shown is row 3.1 (see View::modsEnd()).
    const float lastFirst = (14.0f * 30.0f - 2.0f - 326.0f) / 30.0f;
    const Point macroDot{41.0f, 154.0f + (12.0f - lastFirst) * 30.0f};
    click(view, macroDot);
    view.mouseMove({600.0f, 440.0f}, {});
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
    // Macro 2's handle dropped on LFO 1's depth handle under Op A's Fine knob makes Macro 2 that modulation's aux.
    // Modulating nothing itself, Macro 2 isn't faint as Macro 3 is, which does nothing (read off the rendered
    // pixels, the macros' handles 96 px apart).
    click(view, {64.0f, 88.0f}); // back to ROUTING, with the inspector
    const float strip = view.height() - 179.0f;
    drag(view, {252.0f, strip}, {kFineKnob.x, 229.0f});
    view.mouseMove({600.0f, 300.0f}, {});
    const bool ringShot = renderer.save(view, dir / "35d-aux-ring.png");
    const int auxRing = ringLevel(renderer, {252.0f + 6.0f, strip - 10.0f}, 20.0f);
    const int idleRing = ringLevel(renderer, {348.0f + 6.0f, strip - 10.0f}, 20.0f);
    check(ringShot && slotAuxSource(model.value(slotAuxIndex(0, 0))) == kMaxSources + 1 &&
              slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0 && auxRing > idleRing + 40,
          "a source's handle dropped on a depth handle becomes that modulation's aux, and lights as one used");
    // Hovering that depth handle draws the dashed line to it from LFO 1's handle, on screen on the first card: half
    // way along the line, bowed to one side, the pixels change (read off the rendered pixels).
    const Point from = sourceHandle(0), to{kFineKnob.x, 229.0f};
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
    check(linkShot && aroundMiddle() != plain, "a hovered depth handle draws the line from its source's handle");
}

// A modulation moved to another source takes what modulates its depth along: LFO 1's on Op A's Fine, its depth
// modulated by LFO 2, moved to Env 1 leaves LFO 2 modulating Env 1's depth; moved to Macro 1, whose depths can't be
// modulated, it takes LFO 2's modulation away with it. The menus open as in testMods(): the first row's source at
// x 90, y 154, its menu's categories 22 px apart from y 174 (LFOs, Envelopes, Macros), each opening beside it from x 176.
void testModsMoveDepth(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(2, SourceParam::Type), static_cast<double>(SourceType::Env));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Fine)));
    model.set(slotAmountIndex(0, 0), 0.4);
    model.set(slotTargetIndex(1, 0), slotTargetValue(slotAmountIndex(0, 0)));
    model.set(slotAmountIndex(1, 0), 0.25);
    view.poll();
    click(view, {314.0f, 88.0f}); // MODS
    auto moveTo = [&](int category) {
        const float y = 174.0f + 22.0f * static_cast<float>(category) + 11.0f;
        click(view, {90.0f, 154.0f});
        view.mouseMove({80.0f, y}, {});
        view.mouseMove({200.0f, y}, {}); // the category's first source
        click(view, {200.0f, y});
    };
    moveTo(1); // Env 1
    const bool followed = slotTarget(model.value(slotTargetIndex(2, 0))) == paramIndex(0, OpParam::Fine) &&
                          slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                          slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(2, 0) &&
                          std::abs(model.value(slotAmountIndex(1, 0)) - 0.25) < 1e-9;
    view.mouseMove({600.0f, 440.0f}, {});
    const bool shot = renderer.save(view, dir / "35m-mods-moved-with-depth.png");
    view.undo();
    const bool back = slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Fine) &&
                      slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(0, 0) &&
                      slotTarget(model.value(slotTargetIndex(2, 0))) < 0;
    check(followed && shot && back,
          "a modulation moved to another source takes what modulates its depth along, in one undo step");
    moveTo(2); // Macro 1
    check(slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) == paramIndex(0, OpParam::Fine) &&
              slotTarget(model.value(slotTargetIndex(1, 0))) < 0,
          "a modulation moved to a macro takes away what modulated its depth");
    // Removed by its row's x (at x 1144), it takes what modulated its depth with it, in one undo step.
    view.undo();
    click(view, {1144.0f, 154.0f});
    const bool removed = slotTarget(model.value(slotTargetIndex(0, 0))) < 0 &&
                         slotTarget(model.value(slotTargetIndex(1, 0))) < 0;
    view.undo();
    check(removed && slotTarget(model.value(slotTargetIndex(0, 0))) == paramIndex(0, OpParam::Fine) &&
              slotTarget(model.value(slotTargetIndex(1, 0))) == slotAmountIndex(0, 0),
          "a removed modulation takes what modulated its depth with it, in one undo step");
}

// Modulations added in the MODS view: the row after the last adds an empty one and opens its source's menu; picking
// a source leaves it empty, waiting for a target, and picking a target then makes it a modulation of that source,
// each an undo step. The target menu's Global offers no Mod Wheel. An empty row's x removes it. With no
// modulations at all, the columns' names show above the button.
// The title row's last two buttons add a Random and a MIDI source, each a card of its own: Random with no knobs,
// MIDI with its input's list button (where an LFO's shape is: from x 16 of the card, 122 px wide, at y 536 + 42) and
// right of it its controller (at x 172 of the card, turned up a notch by the wheel, while its input is a controller)
// and Slew (at x 228). The list opens under the button, an entry 22 px from y 604 + 4: CC, Pressure, Slide. Each
// lists in the MODS view's source menu under Note & MIDI.
void testNewSources(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    click(view, sourceAdd(SourceType::Random));
    click(view, sourceAdd(SourceType::Midi));
    click(view, sourceAdd(SourceType::Midi));
    auto typeOf = [&](int place) {
        return static_cast<SourceType>(std::lround(model.value(paramIndex(place, SourceParam::Type))));
    };
    const bool added = typeOf(0) == SourceType::Random && typeOf(1) == SourceType::Midi && typeOf(2) == SourceType::Midi;
    const Point ccKnob{cardX(1) + 172.0f, 590.0f}, inputButton{cardX(2) + 16.0f + 61.0f, 536.0f + 42.0f + 11.0f};
    auto entry = [](float card, MidiInput input) {
        return Point{cardX(card) + 60.0f, 604.0f + 4.0f + 22.0f * static_cast<float>(input) + 11.0f};
    };
    view.wheel(ccKnob, 1.0f, {});
    // The button's tooltip says what its input follows (read off the rendered pixels along y 618, under it, against
    // the same view with the mouse away; the wording is in the snapshots).
    auto underButton = [&](const char *name) {
        std::vector<uint32_t> pixels;
        if (renderer.save(view, dir / name))
            for (float x = cardX(2); x <= cardX(2) + 240.0f; x += 2.0f)
                pixels.push_back(renderer.pixel({x, 618.0f}));
        return pixels;
    };
    view.mouseMove({600.0f, 300.0f}, {});
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
    view.wheel({cardX(2) + 172.0f, 590.0f}, 1.0f, {}); // no CC knob there now
    click(view, inputButton);
    click(view, entry(2, MidiInput::Slide));
    const bool onSlide = model.value(paramIndex(2, SourceParam::MidiInput)) == static_cast<double>(MidiInput::Slide);
    view.mouseMove({600.0f, 300.0f}, {});
    const bool shot = renderer.save(view, dir / "11e-new-sources.png");
    check(ccTip != away && pressureTip != away && ccTip != pressureTip,
          "a MIDI card's input button shows a tooltip saying what its input follows");
    check(added && shot && listShot && model.value(paramIndex(1, SourceParam::CcNumber)) == 2.0 && onPressure &&
              onSlide && model.value(paramIndex(2, SourceParam::CcNumber)) == 1.0,
          "the title row adds Random and MIDI sources, a MIDI card's list picking pressure or slide over a controller");
    // MPE, beside the Pan knob: a click switches it on, and its menu picks the member channels' bend range.
    click(view, {639.0f, 32.0f});
    const bool mpeOn = model.value(paramIndex(GlobalParam::Mpe)) == 1.0;
    for (const MenuItem &item : view.contextMenu({639.0f, 32.0f}))
        if (item.label == trf(Str::MenuMpeBendRange, {"24"}))
            view.menuCommand(item.command);
    view.mouseMove({639.0f, 32.0f}, {});
    check(mpeOn && model.value(paramIndex(GlobalParam::MpeBendRange)) == 24.0 &&
              renderer.save(view, dir / "3g-mpe-switch.png"),
          "the MPE switch beside the Pan knob switches MPE, and its menu picks the bend range");
}

void testModsEmpty(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    // The columns' names, their middle at y 126: along it, under SOURCE (from x 54), the text's pixels differ from
    // the panel's (read off the rendered pixels).
    click(view, {314.0f, 88.0f}); // the MODS tab
    view.mouseMove({600.0f, 440.0f}, {});
    const bool noneShot = renderer.save(view, dir / "35e2-mods-none.png");
    bool headed = false;
    for (float x = 54.0f; x < 110.0f && !headed; x += 1.0f)
        headed = renderer.pixel({x, 126.0f}) != renderer.pixel({48.0f, 126.0f});
    check(noneShot && headed, "the MODS view shows its columns' names with no modulations");
    click(view, kRoutingTab);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Fine)));
    model.set(slotAmountIndex(0, 0), 0.4);
    view.poll();
    click(view, {314.0f, 88.0f}); // the MODS tab
    // Under LFO 1's row (its middle at y 154), the button adds a second: its source's menu opens under its source,
    // at x 54 and y 200. Over Macros, the second entry, the macros show beside it; Macro 1 is picked.
    const float y = 184.0f;
    click(view, {100.0f, y});
    const bool added = model.ui.emptyMods == std::vector<std::pair<int, int>>{{-1, -1}};
    view.mouseMove({80.0f, 200.0f + 4.0f + 22.0f + 11.0f}, {}); // Macros
    view.mouseMove({230.0f, 200.0f + 4.0f + 22.0f + 11.0f}, {}); // Macro 1, beside it
    const bool menuShot = renderer.save(view, dir / "35f-mods-empty-source.png");
    click(view, {230.0f, 200.0f + 4.0f + 22.0f + 11.0f});
    const bool sourced = model.ui.emptyMods == std::vector<std::pair<int, int>>{{kMaxSources, -1}};
    view.mouseMove({600.0f, 440.0f}, {});
    const bool shot = renderer.save(view, dir / "35g-mods-empty.png");
    check(added && menuShot && sourced && shot && slotTarget(model.value(slotTargetIndex(kMaxSources, 0))) < 0,
          "the MODS view's last row adds an empty modulation, which takes a source and modulates nothing yet");
    // Its target opens the targets under it, at x 234, under the menu's search (30 px): Global, the first category,
    // lists its params beside it from x 476, but not the mod wheel; where it would come after them, a click only
    // closes the menu.
    std::vector<int> globals;
    for (int g = 0; g < kGlobalCount; ++g)
        if (isModTarget(g) && g != paramIndex(GlobalParam::ModWheel))
            globals.push_back(g);
    auto openGlobal = [&] {
        click(view, {300.0f, y});
        view.mouseMove({300.0f, 230.0f + 4.0f + 11.0f}, {});
    };
    auto globalEntry = [&](size_t k) { return Point{540.0f, 230.0f + 4.0f + 22.0f * k + 11.0f}; };
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
    click(view, {cardX(1) + 60.0f, 708.0f});
    view.mouseMove({600.0f, 440.0f}, {});
    const bool cardShot = renderer.save(view, dir / "35j-mods-from-card.png");
    check(cardShot && model.ui.view == RoutingView::Mods &&
              model.ui.emptyMods == std::vector<std::pair<int, int>>{{1, -1}},
          "a card's button, its source modulating nothing, adds an empty modulation from it in MODS");
    // With LFO 1's row and then Env 1's, a new empty modulation given LFO 1 moves up among LFO 1's rows: its target,
    // picked in the second row (its middle at y 184), makes it LFO 1's, Env 1's left as it was.
    click(view, {600.0f, 440.0f}); // closes the target menu the card's button opened
    model.ui.emptyMods.clear();
    model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(1, OpParam::Fine)));
    view.poll();
    click(view, {100.0f, 214.0f}); // the button, under Env 1's row, opens the source menu at y 230
    view.mouseMove({80.0f, 230.0f + 4.0f + 11.0f}, {}); // LFOs
    // The menu's first entry under the mouse leaves LFO 1's row (its source's and slot's numbers the entry's) unlit:
    // it matches Env 1's between its aux and its x (read off the rendered pixels).
    const bool overRowsShot = renderer.save(view, dir / "35n-mods-menu-over-rows.png");
    const bool rowUnlit = renderer.pixel({1110.0f, 154.0f}) == renderer.pixel({1110.0f, 184.0f});
    check(overRowsShot && rowUnlit, "an open menu's entry under the mouse lights no MODS row");
    click(view, {230.0f, 230.0f + 4.0f + 11.0f});     // LFO 1, beside it
    const bool lfoSourced = model.ui.emptyMods == std::vector<std::pair<int, int>>{{0, -1}};
    view.mouseMove({600.0f, 440.0f}, {});
    const bool sortedShot = renderer.save(view, dir / "35k-mods-empty-sorted.png");
    openGlobal();
    click(view, globalEntry(0));
    check(lfoSourced && sortedShot && model.ui.emptyMods.empty() &&
              slotTarget(model.value(slotTargetIndex(0, 1))) == globals[0] &&
              slotTarget(model.value(slotTargetIndex(1, 0))) == paramIndex(1, OpParam::Fine),
          "an empty modulation given a source with rows goes among them");
}

// The MODS view's filter and the target menu's search. LFO 1 modulates Op A's Fine and Detune, Env 1 FM Amount and
// Macro 1 Filter 1's Resonance: four rows, their middles from y 154, 30 px apart, each one's x at x 1144. The filter's
// magnifier is at the right end of the columns' names (x 1156, y 126); open, its x is at x 1155.
void testModsSearch(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, SourceParam::Type), static_cast<double>(SourceType::Lfo));
    model.set(paramIndex(1, SourceParam::Type), static_cast<double>(SourceType::Env));
    model.set(slotTargetIndex(0, 0), slotTargetValue(paramIndex(0, OpParam::Fine)));
    model.set(slotTargetIndex(0, 1), slotTargetValue(paramIndex(0, OpParam::Detune)));
    model.set(slotTargetIndex(1, 0), slotTargetValue(paramIndex(GlobalParam::FmAmount)));
    model.set(slotTargetIndex(kMaxSources, 0), slotTargetValue(paramIndex(0, FilterParam::Resonance)));
    for (auto [source, slot] : {std::pair{0, 0}, {0, 1}, {1, 0}, {kMaxSources, 0}})
        model.set(slotAmountIndex(source, slot), 0.3);
    view.poll();
    click(view, {314.0f, 88.0f}); // the MODS tab
    auto slotIs = [&](int source, int slot, int target) {
        return slotTarget(model.value(slotTargetIndex(source, slot))) == target;
    };
    const Point magnifier{1156.0f, 126.0f}, firstX{1144.0f, 154.0f};
    // The magnifier opens the filter, which takes the keyboard; "fm" leaves Env 1's row alone, first, and a click on
    // its x takes the keyboard back, the filter keeping its words.
    const bool idle = !view.typing();
    click(view, magnifier);
    const bool takesKeys = view.typing();
    type(view, "fm");
    view.mouseMove({600.0f, 440.0f}, {});
    const bool filterShot = renderer.save(view, dir / "35q-mods-filter.png");
    click(view, firstX);
    const bool removedEnv = slotTarget(model.value(slotTargetIndex(1, 0))) < 0 && slotIs(0, 0, paramIndex(0, OpParam::Fine));
    const bool letGo = !view.typing();
    view.undo();
    check(idle && takesKeys && filterShot && removedEnv && letGo && slotIs(1, 0, paramIndex(GlobalParam::FmAmount)),
          "the MODS view's filter shows the rows whose source and target hold what is typed, and lets the keyboard "
          "go at a click elsewhere");
    // Every word counts, in any case: "LFO det" leaves LFO 1's Detune row alone. Its x empties it and keeps the
    // keyboard; Escape then lets it go.
    click(view, {1000.0f, 126.0f}); // in the open field, from x 968
    view.typeKey(View::Key::SelectAll);
    type(view, "LFO det");
    click(view, firstX);
    const bool removedDetune = slotTarget(model.value(slotTargetIndex(0, 1))) < 0 && slotIs(0, 0, paramIndex(0, OpParam::Fine));
    view.undo();
    click(view, {1155.0f, 126.0f}); // the filter's x
    const bool emptied = view.typing() && view.copyText(false).empty();
    view.typeKey(View::Key::Escape);
    click(view, firstX); // the filter empty, the first row is LFO 1's Fine again
    const bool all = slotTarget(model.value(slotTargetIndex(0, 0))) < 0;
    view.undo();
    check(removedDetune && emptied && !view.typing() && all && slotIs(0, 0, paramIndex(0, OpParam::Fine)),
          "the MODS view's filter takes every word typed, its x empties it and Escape lets the keyboard go");
    // An operator's targets show in two columns, 140 px each: under Operators, then B, ENVELOPE heads the second,
    // its Attack beside Fine (the level from x 598, under the menu's 30 px search).
    click(view, {300.0f, 154.0f});
    view.mouseMove({300.0f, 200.0f + 4.0f + 22.0f + 11.0f}, {}); // Operators
    view.mouseMove({530.0f, 222.0f + 4.0f + 22.0f + 11.0f}, {}); // B
    const Point attack{800.0f, 244.0f + 4.0f + 20.0f + 11.0f};
    view.mouseMove(attack, {});
    const bool columnsShot = renderer.save(view, dir / "35s-mods-target-columns.png");
    click(view, attack);
    const bool attackPicked = slotIs(0, 0, paramIndex(1, OpParam::Attack));
    view.undo();
    check(columnsShot && attackPicked && slotIs(0, 0, paramIndex(0, OpParam::Fine)),
          "an operator's targets show in two columns, the envelope's beside the pitch's");
    // The target menu takes the keyboard into its search: "b det" lists Op B's Detune, with its path, and Enter
    // picks it.
    click(view, {300.0f, 154.0f});
    const bool menuKeys = view.typing();
    type(view, "b det");
    view.mouseMove({600.0f, 440.0f}, {});
    const bool searchShot = renderer.save(view, dir / "35r-mods-target-search.png");
    view.typeKey(View::Key::Enter);
    const bool found = slotIs(0, 0, paramIndex(1, OpParam::Detune)) && !view.typing();
    view.undo();
    // "a release" finds Op A's Release, then its Release Curve: Down picks the second. Escape empties the search,
    // then closes the menu.
    click(view, {300.0f, 154.0f});
    type(view, "a release");
    view.typeKey(View::Key::Down);
    view.typeKey(View::Key::Enter);
    const bool second = slotIs(0, 0, paramIndex(0, OpParam::ReleaseCurve));
    view.undo();
    click(view, {300.0f, 154.0f});
    type(view, "zzz");
    view.typeKey(View::Key::Enter); // nothing found, nothing picked
    const bool nothing = view.typing() && slotIs(0, 0, paramIndex(0, OpParam::Fine));
    view.typeKey(View::Key::Escape);
    const bool stillOpen = view.typing();
    view.typeKey(View::Key::Escape);
    check(menuKeys && searchShot && found && second && nothing && stillOpen && !view.typing() &&
              slotIs(0, 0, paramIndex(0, OpParam::Fine)),
          "the target menu's search lists the targets found with their path, Up and Down pick one and Enter takes it");
}

// Eight LFOs on Op A's Fine: under the knob, two depth handles and a "+6" pill, nothing drawn outside the knob's
// column (read off the rendered pixels). The pill opens the list of all eight, whose rows bypass, set the depth of
// and remove their modulation as a card's rows do; a click elsewhere closes it. A macro's handle held over the pill
// opens the list under it, and let go of on a row makes the macro that row's aux; let go of anywhere else, the list
// closes again. Six on a matrix cell: two handles and a smaller pill, none in the next cell.
void testModPill(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    model.set(paramIndex(0, OpParam::Fine), 0.5);
    // Beside the knob, between its column and the next ones' (Ratio's and Pitch's), at the depth handles' height.
    auto beside = [&] {
        std::vector<uint32_t> pixels;
        for (float y = 224.0f; y < 235.0f; y += 1.0f)
            for (float x = 17.0f; x < 27.0f; x += 1.0f) {
                pixels.push_back(renderer.pixel({kFineKnob.x - x, y}));
                pixels.push_back(renderer.pixel({kFineKnob.x + x, y}));
            }
        return pixels;
    };
    view.mouseMove({600.0f, 300.0f}, {});
    renderer.save(view, dir / "40-pill-none.png");
    const std::vector<uint32_t> none = beside();
    const int fine = paramIndex(0, OpParam::Fine);
    for (int s = 0; s < 8; ++s) {
        model.set(paramIndex(s, SourceParam::Type), static_cast<double>(SourceType::Lfo));
        model.set(slotTargetIndex(s, 0), slotTargetValue(fine));
        model.set(slotAmountIndex(s, 0), 0.05 + 0.03 * s);
    }
    view.poll();
    const bool shot = renderer.save(view, dir / "40a-pill.png");
    check(shot && beside() == none, "eight modulations on a knob show two handles and a pill, nothing outside its column");

    // The pill sits right of the two handles: 30 px across, centered under the knob, its middle 9 px right of the
    // knob's. Its list, 265 px wide, centered under it: the rows 24 px apart from 24 px down, as on a card.
    const Point pill{kFineKnob.x + 9.0f, 229.0f};
    const float listX = pill.x - 132.5f + 6.0f, rowsY = pill.y + 4.5f + 6.0f + 24.0f;
    auto row = [&](int k, float x) { return Point{listX + x, rowsY + k * 24.0f + 10.0f}; };
    view.mouseMove(pill, {});
    const bool hoverShot = renderer.save(view, dir / "40b-pill-hover.png");
    click(view, pill);
    view.mouseMove(row(3, 60.0f), {}); // on LFO 4's name: its ring is drawn wider
    const bool listShot = renderer.save(view, dir / "40c-pill-list.png");
    const uint32_t open = renderer.pixel(row(5, 60.0f));
    click(view, row(7, 9.0f)); // LFO 8's bypass dot
    const bool bypassed = model.value(slotBypassIndex(7, 0)) == 1.0;
    drag(view, row(0, 116.0f + 38.0f), row(0, 116.0f + 57.0f)); // LFO 1's depth bar, to half way right: +50 %
    const bool deep = std::abs(model.value(slotAmountIndex(0, 0)) - 0.5) < 1e-6;
    click(view, row(6, 243.0f)); // LFO 7's remove
    const bool removed = slotTarget(model.value(slotTargetIndex(6, 0))) < 0;
    click(view, {600.0f, 300.0f}); // elsewhere: the list closes
    view.mouseMove({600.0f, 300.0f}, {});
    renderer.save(view, dir / "40d-pill-closed.png");
    const uint32_t closed = renderer.pixel(row(5, 60.0f));
    check(hoverShot && listShot && bypassed && deep && removed && open != closed,
          "a pill's list bypasses, sets the depth of and removes its modulations, and closes at a click elsewhere");

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
    view.mouseMove({600.0f, 300.0f}, {});
    renderer.save(view, dir / "40f-pill-aux.png");
    const bool stays = renderer.pixel(row(5, 60.0f)) == open;
    click(view, {600.0f, 300.0f});
    // Held over the pill and let go of elsewhere, the list closes again and nothing changes.
    view.mouseDown({252.0f, strip}, {});
    view.mouseMove(pill, {});
    view.mouseMove({600.0f, 300.0f}, {});
    view.mouseUp({600.0f, 300.0f}, {});
    renderer.save(view, dir / "40g-pill-sprung.png");
    check(dropShot && aux && stays && renderer.pixel(row(5, 60.0f)) == closed &&
              slotTarget(model.value(slotTargetIndex(kMaxSources + 1, 0))) < 0,
          "a handle held over a pill opens its list, a row takes it as an aux, and elsewhere the list closes");

    // LFO 3's handle hovered: its depth handle is in the pill, where its dashed line ends.
    view.mouseMove(sourceHandle(2), {});
    check(renderer.save(view, dir / "40h-pill-link.png"), "render a source's line ending at the pill holding its handle");

    // Six on the matrix's B -> C cell: two handles in its corner and a pill, none in the corner of B -> B left of it.
    click(view, {160.0f, 88.0f}); // MATRIX
    const Point cell = gridCellCenter(1, 2), left = gridCellCenter(1, 1);
    auto corner = [&] {
        std::vector<uint32_t> pixels;
        for (float y = cell.y - 14.3f; y < cell.y - 6.0f; y += 1.0f)
            for (float x = left.x; x < left.x + 15.0f; x += 1.0f)
                pixels.push_back(renderer.pixel({x, y}));
        return pixels;
    };
    view.mouseMove({600.0f, 440.0f}, {});
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
}

// LFO 2 modulating LFO 1's depth on operator A's Fine: hovering LFO 2's handle draws a dashed line to that
// depth bar in the MODS view, which has a dot for the modulation, like a matrix cell's.
void renderLfoOnLfo(const std::filesystem::path &dir, Renderer &renderer) {
    FakeController model;
    View view(model);
    for (int source = 0; source < 2; ++source)
        model.values[paramIndex(source, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    model.values[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(0, OpParam::Fine));
    model.values[slotAmountIndex(0, 0)] = 0.4;
    model.values[slotTargetIndex(1, 0)] = slotTargetValue(slotAmountIndex(0, 0));
    model.values[slotAmountIndex(1, 0)] = 0.5;
    view.poll();
    // In the MODS view, LFO 1's row the top one: with Shift a depth bar moves from where it is, a tenth as fast; half
    // its width (204 px from x 474) is 10 % then.
    click(view, {314.0f, 88.0f});
    const Point bar{474.0f + 102.0f, 154.0f};
    view.mouseDown(bar, {true, false});
    view.mouseMove({bar.x + 102.0f, bar.y}, {true, false});
    view.mouseUp({bar.x + 102.0f, bar.y}, {true, false});
    check(std::abs(model.value(slotAmountIndex(0, 0)) - 0.5) < 1e-6, "Shift drags a depth bar finely, from where it is");
    view.mouseMove(sourceHandle(1), {});
    const bool shot = renderer.save(view, dir / "18c-lfo-on-lfo.png");
    // Hovered, the dot says what the modulation takes its target through, in the target's units.
    const Point dot{474.0f + 204.0f - 4.0f, 140.0f + 4.0f}; // on LFO 1's depth bar
    view.mouseMove(dot, {});
    const bool popup = renderer.save(view, dir / "18d-depth-popup.png");
    view.mouseMove({600.0f, 300.0f}, {});
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
    click(view, sourceAdd(SourceType::Lfo));
    click(view, sourceAdd(SourceType::Env));
    check(shown(model) == std::vector<int>{0, 1} &&
              model.value(paramIndex(0, SourceParam::Type)) == static_cast<double>(SourceType::Lfo) &&
              model.value(paramIndex(1, SourceParam::Type)) == static_cast<double>(SourceType::Env) &&
              model.value(sourceEnabledIndex(0)) == 1.0 && model.openEdits == 0,
          "the add buttons add an LFO and an envelope, switched on");
    // A switched-off source's handle is faint, like its card (read off the rendered pixels: its ring darkens).
    {
        const Point ring{sourceHandle(0).x + 6.0f, sourceHandle(0).y};
        view.mouseMove({600.0f, 300.0f}, {});
        const bool onShot = renderer.save(view, dir / "1b-source-on.png");
        const int on = ringLevel(renderer, ring, 20.0f);
        click(view, sourcePower(0));
        view.mouseMove({600.0f, 300.0f}, {});
        const bool offShot = renderer.save(view, dir / "1c-source-off.png");
        const int off = ringLevel(renderer, ring, 20.0f);
        click(view, sourcePower(0));
        std::printf("      LFO 1's handle: brightness %d on, %d off\n", on, off);
        check(onShot && offShot && model.value(sourceEnabledIndex(0)) == 1.0 && on > 200 && off * 18 < on * 10,
              "a switched-off source's handle looks switched off");
    }

    // Op A's envelope fills its display: the hold handle sits just after the peak, and
    // dragging it 20 % of the zoomed scale along gives a hold of 0.2.
    {
        const double total = normFromTime(1.5) + normFromTime(0.015); // A's decay and release
        const float zoom = static_cast<float>((248.0f * 0.84f - 40.0f) / total);
        drag(view, {930.0f, kOpEnvTop + 12.0f}, {930.0f + 0.2f * zoom, kOpEnvTop + 12.0f});
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
                column.push_back(renderer.pixel({1176.0f, y}));
            return column;
        };
        const bool shot = renderer.save(view, dir / "4c-envelope-dragged.png");
        const std::vector<uint32_t> still = edge();
        view.mouseDown({1158.0f, kOpEnvBottom - 10.0f}, {});
        view.mouseMove({1240.0f, kOpEnvBottom - 10.0f}, {});
        view.mouseMove({1320.0f, kOpEnvBottom - 10.0f}, {});
        const bool inside = renderer.save(view, dir / "4c-envelope-dragged.png") && edge() == still;
        view.mouseUp({1320.0f, kOpEnvBottom - 10.0f}, {});
        const bool longer = model.value(release) > before + 0.2;
        view.undo();
        check(shot && inside && longer && model.value(release) == before,
              "an envelope handle dragged past its display lengthens its segment, the envelope staying inside");
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
                    pixels.push_back(renderer.pixel({1040.0f + column, kOpEnvTop + row}));
            return pixels;
        };
        view.mouseMove({600.0f, 300.0f}, {});
        renderer.save(view, dir / "4d-envelope-readout.png");
        const std::vector<uint32_t> plain = corner();
        Point bend{0.0f, 0.0f};
        for (float y = kOpEnvTop + 6.0f; y <= kOpEnvTop + 18.0f && bend.x == 0.0f; y += 2.0f)
            for (float x = 950.0f; x < 1150.0f && bend.x == 0.0f; x += 2.0f) {
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
    click(view, oneShotToggle(0));
    click(view, unipolarToggle(0));
    const bool modesOn = model.value(paramIndex(0, SourceParam::OneShot)) == 1.0 &&
                         model.value(paramIndex(0, SourceParam::Unipolar)) == 1.0;
    click(view, oneShotToggle(0));
    check(modesOn && model.value(paramIndex(0, SourceParam::OneShot)) == 0.0,
          "an LFO's switches make it one-shot and unipolar");

    // Drag LFO 1's handle onto the selected operator's Fine knob.
    view.mouseDown(sourceHandle(0), {});
    view.mouseMove({500.0f, 300.0f}, {});
    view.mouseMove(kFineKnob, {});
    renderer.save(view, dir / "2-dragging-lfo.png");
    view.mouseUp(kFineKnob, {});
    const int fine = paramIndex(0, OpParam::Fine);
    check(slotTarget(model.value(slotTargetIndex(0, 0))) == fine && model.value(slotAmountIndex(0, 0)) != 0.0,
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
    const Point envColumn{1034.0f, kOpEnvTop};
    view.mouseMove({600.0f, 300.0f}, {});
    const bool pageOnShot = renderer.save(view, dir / "1d-op-on.png");
    const int pageOn = ringLevel(renderer, envColumn, kOpEnvBottom - kOpEnvTop);
    click(view, nodeTab(4));
    const bool tabOff = model.value(paramIndex(4, OpParam::Enabled)) == 0.0 && layout.selectedNode == 4;
    view.mouseMove({600.0f, 300.0f}, {});
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
    view.mouseMove({400.0f, 300.0f}, {});
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
    // Modulation dropped on a silent operator's Out knob shows its wire to OUT; Disconnect takes both away.
    click(view, b);
    drag(view, sourceHandle(0), kOutKnob);
    const Point bOut = outLabelOf(layout, 1);
    check(slotTarget(model.value(slotTargetIndex(0, 1))) == paramIndex(1, OpParam::Out) &&
              model.value(paramIndex(1, OpParam::Out)) == 0.0 && outDisconnect(bOut) >= 0,
          "modulating an operator's Out knob shows its wire to OUT");
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "2c-modulated-out.png"), "render a modulated output at 0 %");
    view.menuCommand(outDisconnect(bOut));
    check(outDisconnect(bOut) < 0 && slotTarget(model.value(slotTargetIndex(0, 1))) < 0,
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
        view.mouseMove({400.0f, 300.0f}, {});
        const bool movedShot = renderer.save(view, dir / "2e-rerouted.png");
        view.undo();
        check(shot && movedShot && moved && model.value(matrixIndex(1, 2)) == 0.0 &&
                  model.value(matrixIndex(1, 0)) == bToA && envTargets(matrixIndex(1, 0)),
              "dragging a wire's arrowhead onto another node moves the route with its modulation, undoably");
        const int aOutParam = paramIndex(0, OpParam::Out);
        drag(view, outArrowOf(layout, 0), cNode);
        const bool outMoved = model.value(aOutParam) == 0.0 && model.value(matrixIndex(0, 2)) == 1.0;
        drag(view, arrowOf(a, cNode), {cNode.x + 150.0f, kBusY});
        check(outMoved && model.value(aOutParam) == 1.0 && model.value(matrixIndex(0, 2)) == 0.0,
              "an arrow to OUT moves onto a node, and a wire's arrow onto the OUT bus");
        view.mouseDown(arrowOf(b, a), {});
        view.mouseMove({400.0f, 270.0f}, {});
        view.mouseMove({400.0f, 240.0f}, {});
        const bool removingShot = renderer.save(view, dir / "2f-removing.png");
        view.mouseUp({400.0f, 240.0f}, {});
        const bool removed = model.value(matrixIndex(1, 0)) == 0.0 && !envTargets(matrixIndex(1, 0)) &&
                             canDisconnect() < 0;
        view.undo();
        check(removingShot && removed && model.value(matrixIndex(1, 0)) == bToA && envTargets(matrixIndex(1, 0)),
              "an arrowhead let go of away from the nodes and the bus removes its route with its modulation, undoably");
        view.menuCommand(canDisconnect()); // B -> A goes, with Env 1's modulation, as before
        model.set(matrixIndex(1, 0), 0.3);
        view.poll();
    }

    // Drag the Fine knob up by 40 px: +20 % of its range.
    view.mouseDown(kFineKnob, {});
    view.mouseMove({kFineKnob.x, kFineKnob.y - 40.0f}, {});
    view.mouseUp({kFineKnob.x, kFineKnob.y - 40.0f}, {});
    check(std::abs(model.value(fine) - 0.2) < 1e-6 && model.openEdits == 0, "knob drag sets the value, edits closed");

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
        click(view, opTabPoint(InspectorTab::Scope));
        view.poll();
        check(layout.opTab == InspectorTab::Scope && model.scopeOp == 0 && model.scopeCycles == 2,
              "the SCOPE tab asks for its operator's output");
        auto peakAt = [&](int cycles) { // the first peak: 8 px across the inner display, 67 px from the middle up
            const float x = 908.0f + 252.0f * (0.25f / cycles);
            return ringLevel(renderer, {x, 383.0f - 0.8f * 67.0f - 4.0f}, 8.0f);
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
        view.mouseMove({600.0f, 300.0f}, {});
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
        const Point dot{971.0f, 263.0f};
        view.mouseMove({600.0f, 300.0f}, {});
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
        view.mouseUp({600.0f, 300.0f}, {});
        view.poll();
        check(opened && model.scopeOp == -1, "a dragged handle opens the tab it is held over, and the scope stops");
        model.live = before;
        click(view, opTabPoint(InspectorTab::Scope));
        click(view, scopeCycleButton(1));
        click(view, opTabPoint(InspectorTab::Envelope));
        view.poll();
        check(layout.opTab == InspectorTab::Envelope && layout.scopeCycles == 2, "the ENVELOPE tab is back");
    }

    // Fixed pitch: the keyboard on A's summary line switches it, setting Freq and Mult to where the ratio put A4 in
    // one undo step. Freq and Mult take Ratio's and Fine's places (Mult stepping, 10 px up a step), and Pitch and
    // Detune, dimmed, don't turn (read off the rendered pixels: the Pitch knob's column darkens).
    {
        constexpr Point kFixedSwitch{1125.0f, 150.0f};
        const Point pitchColumn{kPitchKnob.x, 198.0f};
        const int fixed = paramIndex(0, OpParam::Fixed), freq = paramIndex(0, OpParam::FixedFreq),
                  mult = paramIndex(0, OpParam::FixedMult);
        const double tracked = 440.0 * coarseRatio(model.value(paramIndex(0, OpParam::Coarse))) *
                               (1.0 + model.value(paramIndex(0, OpParam::Fine)));
        view.mouseMove({600.0f, 300.0f}, {});
        const bool trackingShot = renderer.save(view, dir / "3f-tracking.png");
        const int lit = ringLevel(renderer, pitchColumn, 32.0f);
        click(view, kFixedSwitch);
        const double fixedAt = fixedHz(model.value(freq), model.value(mult));
        const bool set = model.value(fixed) == 1.0 && model.value(mult) == 3.0 && std::abs(fixedAt - tracked) < 0.01;
        drag(view, kFineKnob, {kFineKnob.x, kFineKnob.y - 10.0f}); // Mult, in Fine's place: x1 to x10
        const double pitch = model.value(paramIndex(0, OpParam::Transpose));
        drag(view, kPitchKnob, {kPitchKnob.x, kPitchKnob.y - 30.0f});
        view.mouseMove({600.0f, 300.0f}, {});
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
    }

    // Play switches: Mono on, Legato on (it starts off), off and on again, Glide to 0.5 (100 px up), Always on.
    click(view, kMonoButton);
    click(view, kLegatoToggle);
    click(view, kLegatoToggle);
    const bool legatoFlipped = model.value(paramIndex(GlobalParam::Legato)) == 0.0;
    click(view, kLegatoToggle);
    drag(view, kGlideKnob, {kGlideKnob.x, kGlideKnob.y - 100.0f});
    click(view, kAlwaysToggle);
    check(model.value(paramIndex(GlobalParam::Mono)) == 1.0 && legatoFlipped &&
              model.value(paramIndex(GlobalParam::Legato)) == 1.0 &&
              std::abs(model.value(paramIndex(GlobalParam::Glide)) - 0.5) < 1e-6 &&
              model.value(paramIndex(GlobalParam::GlideAlways)) == 1.0,
          "play switches and the glide knob set their params");
    view.mouseMove(kLegatoToggle, {});
    check(renderer.save(view, dir / "3c-play.png"), "render the play controls with a tooltip");

    // Switch filter 1 on and route A through it: A's output moves into the filter.
    click(view, nodeTab(kOps));
    click(view, nodeCenter(layout, 0)); // A selected again, so the F1 tab below selects rather than switches
    view.mouseMove({400.0f, 300.0f}, {});
    renderer.save(view, dir / "3d-filter-unfed.png");
    const uint32_t unfed = tabDot(renderer, kOps);
    // Like an operator or a shaper, it starts routed nowhere; its edge dragged onto the bus sends it out, with its
    // line to OUT, though nothing goes into it, and that line's arrowhead let go of in empty space takes it away.
    const int f1Out = paramIndex(0, FilterParam::Out);
    const bool startsNowhere = model.value(f1Out) == 0.0;
    drag(view, portOf(layout, kOps), {nodeCenter(layout, kOps).x + 120.0f, kBusY});
    view.mouseMove({400.0f, 300.0f}, {});
    const bool routedOut = model.value(f1Out) == 1.0 && renderer.save(view, dir / "3d-filter-out.png");
    drag(view, outArrowOf(layout, kOps), {nodeCenter(layout, kOps).x + 120.0f, kBusY - 60.0f});
    check(startsNowhere && routedOut && model.value(f1Out) == 0.0,
          "a filter goes out by its edge dragged onto the bus, and stops by its arrowhead dragged away");
    drag(view, portOf(layout, 0), nodeCenter(layout, kOps));
    view.mouseMove({400.0f, 300.0f}, {});
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
    const Point handle{910.0f + static_cast<float>(cutoffBefore) * 248.0f,
                       356.0f - static_cast<float>(model.value(paramIndex(0, FilterParam::Resonance))) * 92.0f};
    drag(view, handle, {handle.x - 50.0f, handle.y - 20.0f});
    check(model.value(cutoff) < cutoffBefore - 0.15, "dragging the response handle moves the cutoff");
    model.set(paramIndex(0, FilterParam::Resonance), 0.55);
    // Behind the response, what the filter puts out, measured: a 220 Hz saw's harmonics, falling away past the
    // cutoff.
    const double cutoffHz = cutoffFromNorm(model.value(cutoff));
    for (int band = 0; band < kSpectrumBands; ++band) {
        const double hz = cutoffFromNorm((band + 0.5) / kSpectrumBands), over = hz / cutoffHz;
        model.live.filterSpectrum[0][band] = hz < 200.0 ? 1e-3f : static_cast<float>(0.5 * 220.0 / hz / (1.0 + over * over));
    }
    view.poll();
    view.mouseMove({1000.0f, 120.0f}, {});
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
    model.set(paramIndex(5, OpParam::Coarse), 3.0);
    click(view, sourceAdd(SourceType::Lfo)); // LFO 2, in place 2
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
    auto cornerSquare = [&] { return renderer.pixel({18.0f, 106.0f}) == renderer.pixel({18.0f, 140.0f}); };
    const bool routingCorner = cornerSquare();

    // The same routing as a matrix: double-click toggles C -> F2 off.
    click(view, kMatrixTab);
    check(layout.view == RoutingView::Matrix, "the Matrix tab switches the routing panel to the grid");
    view.mouseMove(gridCellCenter(0, kOps), {});
    check(renderer.save(view, dir / "6-matrix.png"), "render the matrix view");
    // The tab shown opens the panel's top edge under it, the edge running on under the others; one under the mouse
    // shows a tab's shape.
    auto edgeOpen = [&](Point tab) { return renderer.pixel({tab.x, 104.0f}) == renderer.pixel({tab.x, 112.0f}); };
    const bool tabJoined = edgeOpen(kMatrixTab) && !edgeOpen(kRoutingTab) && !edgeOpen(kFxTab);
    check(routingCorner && !cornerSquare(), "the Routing tab shown runs on down its panel's left edge");
    const uint32_t fxTabAlone = renderer.pixel({kFxTab.x, 76.0f});
    view.mouseMove(kFxTab, {});
    check(tabJoined && renderer.save(view, dir / "6a-view-tab-hovered.png") &&
              renderer.pixel({kFxTab.x, 76.0f}) != fxTabAlone,
          "the tab shown opens its panel's edge, and a hovered one shows a tab's shape");
    view.doubleClick({gridCellCenter(1, 0).x + 10.0f, gridCellCenter(1, 0).y - 10.0f}, {}); // Env 1's dot on B -> A
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
    // F and the shapers, which are off, wait in the graph's top left as dashed ghosts (F hovered here).
    const Point ghostF{44.0f, 126.0f};
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
    // the OUT bus, as any node goes out. F1 feeds it back, round in a loop, but F2 can't feed the lower F1.
    check(kNodes == kOps + 4, "the graph has six operators, two filters and two shapers");
    const int s1 = kOps + kFilters;
    click(view, nodeTab(s1));
    view.mouseMove({400.0f, 300.0f}, {});
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
    drag(view, portOf(layout, kOps), nodeCenter(layout, s1));
    drag(view, portOf(layout, kOps + 1), nodeCenter(layout, kOps));
    drag(view, portOf(layout, s1), {nodeCenter(layout, s1).x + 120.0f, kBusY}); // onto the OUT bus
    check(model.value(filterLinkIndex(0, 1)) == 0.5 && model.value(shaperModIndex(0, 1)) == 0.3 &&
              model.value(shaperFilterIndex(0, 0)) == 1.0 && model.value(shaperSendIndex(0, 0)) == 1.0 &&
              model.value(filterShaperIndex(0, 0)) > 0.0 && model.value(paramIndex(0, ShaperParam::Out)) == 1.0,
          "a filter feeds a shaper, though not a lower filter, and a shaper goes out onto the bus");
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "7b-shapers.png"), "render a shaper in the graph, and its page");
    // Fed by A and going out, S1's tab (still selected) has the dot, dark on its red.
    check((shaperAlone >> 16 & 255) > 150 && (tabDot(renderer, s1) >> 16 & 255) < 80,
          "a shaper's tab shows a dot while it sounds out");
    click(view, kMatrixTab);
    check(renderer.save(view, dir / "7c-shapers-matrix.png"), "render the matrix with F switched off");
    // There a name works like its node's tab: on a node that is off, it switches it on and selects it; on the
    // selected one, it switches it off and on again; on another that is on, it only selects that one.
    const int selectedThen = layout.selectedNode;
    click(view, {54.0f, gridCellCenter(5, 0).y}); // F's row name
    const bool fromName = model.value(paramIndex(5, OpParam::Enabled)) == 1.0 && layout.selectedNode == 5;
    click(view, {54.0f, gridCellCenter(5, 0).y}); // again, now that F is selected
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
    view.mouseMove({400.0f, 300.0f}, {});
    const Point tick{cardX(0) + 26.0f + 0.25f * (kCardW - 52.0f), 678.0f};
    const Point arc{phaseKnob(0).x - 12.4f, phaseKnob(0).y + 4.0f}; // on the arc, near its start at the lower left
    auto brightness = [&](Point p) {
        const uint32_t color = renderer.pixel(p);
        return (color >> 16 & 255) + (color >> 8 & 255) + (color & 255);
    };
    const bool phaseShot = renderer.save(view, dir / "4c-lfo-phase.png");
    const uint32_t tickThen = brightness(tick), arcThen = brightness(arc);
    click(view, freeToggle(0));
    view.mouseMove({400.0f, 300.0f}, {});
    const bool freeShot = renderer.save(view, dir / "4d-lfo-phase-free.png");
    const bool faint = brightness(tick) < tickThen && brightness(arc) < arcThen;
    click(view, freeToggle(0));
    check(std::abs(model.value(phase) - 0.25) < 1e-6 && phaseShot && freeShot && faint &&
              model.value(paramIndex(0, SourceParam::Free)) == 0.0,
          "an LFO's Phase knob sets its start, marked on its display, and goes faint while the LFO is free");
    model.set(phase, 0.0);

    // The FX chain starts empty and covers the routing panel and the inspector.
    click(view, kFxTab);
    check(layout.view == RoutingView::Fx && fxChain(model).empty(), "the FX tab shows an empty effects chain");
    click(view, kFxMenuButton);
    view.mouseMove(fxMenuItem(FxType::Delay), {});
    check(renderer.save(view, dir / "8a-fx-empty.png"), "render the empty FX chain with the ADD EFFECT list open");
    click(view, {600.0f, 400.0f}); // anywhere else closes the list
    check(fxChain(model).empty(), "a click outside the ADD EFFECT list closes it");
    // The dashed card at the chain's end (its first place, while it is empty) opens the list over itself.
    click(view, {fxCardX(model, 0) + fxCardW(model, 0) * 0.5f, 300.0f});
    auto shadowItem = [](FxType type) { return Point{130.0f, 176.0f + (static_cast<int>(type) - 1) * 26.0f}; };
    view.mouseMove(shadowItem(FxType::Shaper), {});
    check(renderer.save(view, dir / "8a2-fx-shadow-list.png"), "render the list of effects over the dashed card");
    click(view, shadowItem(FxType::Shaper));
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
    // At the chain's right end, the dashed card opens its list over the card before it, and a switched-off card's
    // dimming stays under the list (read off the rendered pixels: the list looks the same over it either way).
    {
        click(view, {fxCardX(model, 5) + 40.0f, 300.0f});
        const Point inList{1010.0f, 330.0f}; // over the second delay's card
        renderer.save(view, dir / "8a3-fx-list-right.png");
        const uint32_t overOn = renderer.pixel(inList);
        model.set(paramIndex(4, FxParam::Enabled), 0.0);
        const bool shot = renderer.save(view, dir / "8a4-fx-list-over-off.png");
        const uint32_t overOff = renderer.pixel(inList);
        model.set(paramIndex(4, FxParam::Enabled), 1.0);
        click(view, {600.0f, 140.0f}); // anywhere else closes it
        check(shot && overOn == overOff && fxChain(model).size() == 5,
              "the list of effects stays over a switched-off card's dimming");
    }

    // The filter card's type buttons, in a row: Comb is the fifth.
    click(view, {fxCardX(model, 3) + 14.0f + 4.0f * 29.4f + 13.0f, 152.0f + 44.0f + 10.0f});
    check(std::lround(model.value(paramIndex(3, FxParam::FilterType))) == static_cast<long>(FilterType::Comb),
          "the filter card's buttons pick the filter type");

    // The parametric EQ's first band: dragging its handle 30 px up is +10 dB.
    const float peqW = fxCardW(model, 2) - 44.0f; // its display's plot, inside the wider card
    const Point band{fxCardX(model, 2) + 22.0f + static_cast<float>(model.value(peqFreqIndex(2, 0))) * peqW, 264.0f};
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
    // The second band's shape comes from a list that opens above its button: a high cut.
    const Point peqType2{fxCardX(model, 2) + fxCardW(model, 2) * 0.125f, 418.0f};
    click(view, peqType2);
    view.mouseMove({peqType2.x + 38.0f, 297.0f + 4.0f * 22.0f}, {});
    check(renderer.save(view, dir / "8e-peq-shapes.png"), "render the list of EQ band shapes");
    click(view, {peqType2.x + 38.0f, 297.0f + 4.0f * 22.0f});
    check(model.value(peqTypeIndex(2, 1)) == static_cast<double>(PeqType::HighCut),
          "the band's list picks its shape");
    model.set(peqGainIndex(2, 1), -6.0);
    model.set(peqQIndex(2, 1), 0.2);
    model.set(paramIndex(3, FxParam::FilterType), static_cast<double>(FilterType::BandPass));
    model.set(paramIndex(3, FxParam::FilterResonance), 0.6);
    model.set(paramIndex(0, FxParam::ShaperDrive), 18.0);
    // The shaper's curve comes from a list under its button.
    const Point clipButton{fxCardX(model, 0) + 60.0f, 152.0f + 44.0f + 11.0f};
    auto clipItem = [&](ClipMode mode) {
        return Point{fxCardX(model, 0) + 40.0f, 152.0f + 44.0f + 22.0f + 4.0f + 4.0f + static_cast<int>(mode) * 22.0f + 11.0f};
    };
    click(view, clipButton);
    view.mouseMove(clipItem(ClipMode::SineFold), {});
    check(renderer.save(view, dir / "8b-clip-menu.png"), "render the shaper's list of curves");
    click(view, clipItem(ClipMode::Asymmetric));
    check(std::lround(model.value(paramIndex(0, FxParam::ShaperMode))) == static_cast<long>(ClipMode::Asymmetric),
          "the shaper's list picks its curve");
    // Custom draws the curve in the card, to edit right there: its plot is the display less 8 px all round
    // (127 x 100), the full swing 6 px short of its edges. A double-click adds a point, a drag moves it.
    click(view, clipButton);
    click(view, clipItem(ClipMode::Custom));
    auto cardAt = [&](double x, double y) {
        return Point{fxCardX(model, 0) + 22.0f + static_cast<float>(x) * 127.0f,
                     284.0f - static_cast<float>(y) * (50.0f - 6.0f)};
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
    click(view, {fxCardX(model, 0) + 134.0f, 152.0f + 190.0f - 12.0f}); // the pencil, at the display's bottom right
    view.doubleClick(clipAt(0.3, -0.5), {});
    check(model.value(clipCurveCountIndex(0)) == 5.0 &&
              std::abs(model.value(shapeFieldIndex(clipCurveCountIndex(0), 1, PointField::X)) - 0.3) < 0.01,
          "the pencil opens the big curve editor, which adds points too");
    view.mouseMove(clipAt(0.3, -0.5), {});
    check(renderer.save(view, dir / "8c-clip-curve.png"), "render the shaper's big curve editor");
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
    view.mouseMove({600.0f, 400.0f}, {});
    check(layout.view == RoutingView::Fx, "the done button goes back to the chain");
    model.set(paramIndex(0, FxParam::ShaperMode), static_cast<double>(ClipMode::Asymmetric));
    model.set(paramIndex(1, FxParam::DelayPingPong), 1.0);
    model.set(paramIndex(1, FxParam::DelayTone), -0.4);
    model.set(paramIndex(4, FxParam::DelaySync), 0.0);
    model.set(paramIndex(4, FxParam::DelayWidth), 0.3);
    // The first delay's LINK switch, the middle of three across the card under its display.
    click(view, {fxCardX(model, 1) + fxCardW(model, 1) * 0.5f, 152.0f + 171.0f});
    check(model.value(paramIndex(1, FxParam::DelayLink)) == 1.0, "the delay's LINK switch links its sides");
    // The shaper's LINK, its chain under the display in the card's middle (with no SYM beside it, but for a Custom
    // curve), starts on; a click unlinks it, another links it again.
    const Point clipLink{fxCardX(model, 0) + fxCardW(model, 0) * 0.5f, 152.0f + 194.0f + 10.0f};
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
        const uint32_t color = renderer.pixel({fxCardX(model, 4) + dx, 169.0f});
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
    view.mouseMove({fxGrip(model, 0).x + 200.0f, 184.0f}, {});
    view.mouseMove({fxGrip(model, 0).x + 390.0f, 184.0f}, {});
    check(renderer.save(view, dir / "9-fx-reorder.png"), "render a card being dragged");
    view.mouseUp({fxGrip(model, 0).x + 390.0f, 184.0f}, {});
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4}, "dragging a card's title moves that effect along the chain");
    // Dragged by its icon, a card moves just the same, and stays switched as it was; undo puts it back.
    const Point shaperIcon = fxPower(model, 2);
    view.mouseDown(shaperIcon, {});
    view.mouseMove({shaperIcon.x - 160.0f, 184.0f}, {});
    view.mouseMove({shaperIcon.x - 320.0f, 184.0f}, {});
    view.mouseUp({shaperIcon.x - 320.0f, 184.0f}, {});
    const bool iconMoved = fxChain(model) == std::vector<int>{1, 0, 2, 3, 4} &&
                           model.value(paramIndex(0, FxParam::Enabled)) == 1.0;
    view.undo();
    check(iconMoved && fxChain(model) == std::vector<int>{1, 2, 0, 3, 4},
          "dragging a card's icon moves it without switching it");
    view.mouseMove(fxPower(model, 2), {});
    check(renderer.save(view, dir / "9a-fx-icon.png"), "render a card's icon under the mouse, with its tooltip");

    // A new Env 2 onto the delay's Mix knob; the delay leads the chain now.
    click(view, sourceAdd(SourceType::Env));
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
        view.wheel({600.0f, 125.0f}, 10.0f, {});
        const Point arrow{fxCardX(model, 1) - (kFxStep - kFxCardW) * 0.5f, 304.0f};
        view.mouseMove(arrow, {});
        const bool hoverShot = renderer.save(view, dir / "8d-fx-insert.png");
        click(view, arrow);
        const Point delay{arrow.x, 160.0f + 4.0f + 26.0f + 12.0f}; // the list's second entry
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
    // into view, so the compressors land on screen places 4 and 5.
    addFx(view, FxType::Compressor);
    addFx(view, FxType::MultiComp);
    check(fxChain(model) == std::vector<int>{1, 2, 0, 3, 4, 5, 6} &&
              model.value(paramIndex(5, FxParam::Type)) == static_cast<double>(FxType::Compressor) &&
              model.value(paramIndex(6, FxParam::Type)) == static_cast<double>(FxType::MultiComp),
          "the chain grows past six effects, compressors included");
    fxScrolled = 1.5f;
    // The three-band card's MID button shows the mid band's knobs; LINK makes them set the low band, which
    // every band follows.
    click(view, {fxCardX(model, 6) + 14.0f + 37.25f + 16.0f, 152.0f + 44.0f + 11.0f});
    drag(view, fxKnob(model, 6, 0, 3, 160.0f), {fxKnob(model, 6, 0, 3, 160.0f).x, fxKnob(model, 6, 0, 3, 160.0f).y - 20.0f});
    check(model.value(mcThresholdIndex(6, 1)) > -20.0 && model.value(mcThresholdIndex(6, 0)) == -20.0,
          "a three-band card's band buttons pick which band its knobs set");
    click(view, {fxCardX(model, 6) + 14.0f + 3.0f * 37.25f + 16.0f, 152.0f + 44.0f + 11.0f});
    drag(view, fxKnob(model, 6, 1, 3, 160.0f), {fxKnob(model, 6, 1, 3, 160.0f).x, fxKnob(model, 6, 1, 3, 160.0f).y - 20.0f});
    const bool linked = model.value(paramIndex(6, FxParam::McLink)) == 1.0 && model.value(mcRatioIndex(6, 0)) > 3.0 &&
                        model.value(mcRatioIndex(6, 1)) == 3.0;
    view.mouseMove({600.0f, 300.0f}, {});
    check(linked && renderer.save(view, dir / "9d-3band-linked.png"), "LINK makes the knobs set the low band for all");
    click(view, {fxCardX(model, 6) + 14.0f + 3.0f * 37.25f + 16.0f, 152.0f + 44.0f + 11.0f});
    drag(view, fxKnob(model, 6, 2, 3, 268.0f), {fxKnob(model, 6, 2, 3, 268.0f).x, fxKnob(model, 6, 2, 3, 268.0f).y - 20.0f});
    check(model.value(paramIndex(6, FxParam::McOutput)) > 0.0, "a three-band card's Output knob sets its level");
    model.live.fxReduction[5] = {4.5f, 0.0f, 0.0f};
    model.live.fxReduction[6] = {2.0f, 6.0f, 1.0f};
    view.poll();
    view.mouseMove({600.0f, 300.0f}, {});
    check(renderer.save(view, dir / "9b-compressors.png"), "render the compressors in a scrolled chain");
    // The scrollbar under the cards: eight and a half places with the dashed card at the end, six on screen,
    // scrolled one and a half along. Its thumb is six of those parts of the 1136 px track, with 334 px to move
    // in; dragging it a third of that back leaves the cards between places, and all the way back shows the
    // first card again.
    view.mouseDown({681.0f, 465.0f}, {});
    view.mouseMove({681.0f - 111.0f, 465.0f}, {});
    check(renderer.save(view, dir / "9c-fx-half-scrolled.png"), "render the chain scrolled half a card");
    view.mouseMove({681.0f - 300.0f, 465.0f}, {});
    view.mouseUp({681.0f - 300.0f, 465.0f}, {});
    fxScrolled = 0.0f;
    click(view, fxPower(model, 0));
    const bool scrolledBack = model.value(paramIndex(1, FxParam::Enabled)) == 0.0;
    click(view, fxPower(model, 0));
    // Back to the end to take the compressors out: the chain gets shorter, and its scroll with it; the wheel
    // over the panel's title row takes it back to the start.
    view.mouseDown({681.0f, 465.0f}, {});
    view.mouseMove({681.0f + 600.0f, 465.0f}, {});
    view.mouseUp({681.0f + 600.0f, 465.0f}, {});
    fxScrolled = 2.5f;
    click(view, fxRemove(model, 6));
    fxScrolled = 1.5f;
    click(view, fxRemove(model, 5));
    view.wheel({600.0f, 125.0f}, 10.0f, {});
    fxScrolled = 0.0f;
    check(scrolledBack && fxChain(model) == std::vector<int>{1, 2, 0, 3, 4},
          "dragging the scrollbar scrolls the chain back");

    // A freeform LFO 2: picking Custom opens its shape editor over the routing panel.
    // The default shape is a triangle, with points at (0, -1) and (0.5, 1).
    click(view, shapeButton(2));
    click(view, shapeItem(2, LfoShape::Custom));
    const int count = shapeCountIndex(2);
    auto pointValue = [&](int point, PointField field) { return model.value(shapePointIndex(2, point, field)); };
    check(std::lround(model.value(paramIndex(2, SourceParam::Shape))) == static_cast<long>(LfoShape::Custom) &&
              model.value(count) == 2.0 && pointValue(0, PointField::Y) == -1.0 && pointValue(1, PointField::X) == 0.5,
          "the Custom button gives LFO 2 a freeform shape, a triangle to begin with");
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
    check(model.value(paramIndex(1, FxParam::Enabled)) == 0.0, "the done button closes the editor and the FX view is back");

    // Four sources and the dashed cards, less than a card more than fit: dragged all the way, the scrollbar's thumb
    // reaches the track's right end (read off the rendered pixels).
    drag(view, {485.0f, 741.0f}, {485.0f + 500.0f, 741.0f});
    view.mouseMove({400.0f, 300.0f}, {});
    const bool endShot = renderer.save(view, dir / "11g-sources-scrolled-end.png");
    check(endShot && renderer.pixel({1168.0f, 741.0f}) == renderer.pixel({700.0f, 741.0f}),
          "the scrollbar's thumb reaches the end with the dashed cards less than a card past the row");
    view.wheel({60.0f, 513.0f}, 10.0f, {});

    // A fifth source scrolls the cards along to show it; the wheel over the panel scrolls back.
    click(view, sourceAdd(SourceType::Lfo));
    click(view, sourcePower(3));
    check(shown(model) == std::vector<int>{0, 1, 2, 3, 4} && model.value(sourceEnabledIndex(4)) == 0.0,
          "a new source's card comes into view");
    click(view, {40.0f, 741.0f}); // the scrollbar's track, left of its thumb
    click(view, sourcePower(0));
    check(model.value(sourceEnabledIndex(0)) == 0.0, "clicking the scrollbar's track scrolls the cards back");
    click(view, sourcePower(0));
    // Five cards and the dashed ones, four places on screen. Dragged all the way, the scrollbar's thumb shows
    // the fifth card, the dashed ones after it.
    drag(view, {485.0f, 741.0f}, {485.0f + 500.0f, 741.0f});
    click(view, sourcePower(3.0f - kSourceShadowSpan));
    view.mouseMove({cardX(4.0f - kSourceShadowSpan) + 61.0f, 600.0f}, {});
    check(model.value(sourceEnabledIndex(4)) == 1.0 && renderer.save(view, dir / "11e-source-shadow.png"),
          "dragging the scrollbar's thumb scrolls the cards, the dashed ones last");
    view.wheel({60.0f, 513.0f}, 1.0f, {}); // two fifths of a card a notch
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "11b-sources-between.png"), "render the source cards between places");
    view.wheel({60.0f, 513.0f}, 10.0f, {});

    // Drag Env 1's card one place along.
    view.mouseDown(sourceGrip(1), {});
    view.mouseMove({sourceGrip(1).x + 150.0f, 560.0f}, {});
    view.mouseMove({sourceGrip(1).x + 280.0f, 560.0f}, {});
    check(renderer.save(view, dir / "11a-source-reorder.png"), "render a source card being dragged");
    view.mouseUp({sourceGrip(1).x + 280.0f, 560.0f}, {});
    check(shown(model) == std::vector<int>{0, 2, 1, 3, 4} && nameOf(model, 2) == trf(Str::SourceNameLfo, {"2"}),
          "dragging a card's title moves that source along");

    // Duplicate LFO 2 from its title's menu: the copy, with its targets and shape, follows it.
    model.sets.clear();
    for (const MenuItem &item : view.contextMenu(sourceGrip(1)))
        if (item.label == tr(Str::MenuDuplicate))
            view.menuCommand(item.command);
    check(shown(model) == std::vector<int>{0, 2, 5, 1, 3, 4} &&
              sameFields(model, paramIndex(5, SourceParam::Type), paramIndex(2, SourceParam::Type), kSourceParamCount) &&
              sameFields(model, shapeCountIndex(5), shapeCountIndex(2), kShapeParamCount) &&
              setLast(model, paramIndex(5, SourceParam::Type), kSourceParamCount, paramIndex(5, SourceParam::Type)) &&
              slotTarget(model.value(slotTargetIndex(5, 0))) == paramIndex(GlobalParam::Pan) &&
              model.value(paramIndex(5, SourceParam::Shape)) == static_cast<double>(LfoShape::Custom) &&
              nameOf(model, 5) == trf(Str::SourceNameLfo, {"3"}),
          "Duplicate copies a source in right after it");

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
    view.mouseMove({600.0f, 300.0f}, {});
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
    click(view, sourceAdd(SourceType::Env));
    check(shown(model) == std::vector<int>{0, 2, 5, 3, 4, 1} && slotTarget(model.value(slotTargetIndex(1, 0))) < 0 &&
              model.value(paramIndex(1, SourceParam::Sustain)) == paramTable()[paramIndex(1, SourceParam::Sustain)].def &&
              nameOf(model, 3) == trf(Str::SourceNameEnv, {"1"}) && nameOf(model, 1) == trf(Str::SourceNameEnv, {"2"}),
          "a new source fills the first free place, fresh, at the end");
    model.set(paramIndex(4, SourceParam::Shape), static_cast<double>(LfoShape::Saw));
    model.live.sources = {0.4f, 0.3f, -0.6f, 0.5f, 0.8f, -0.2f};
    model.live.lfoPhases = {0.1f, 0.0f, 0.62f, 0.0f, 0.35f, 0.62f};
    view.mouseMove({400.0f, 300.0f}, {});
    view.poll();
    check(renderer.save(view, dir / "11-sources.png"), "render six sources, scrolled");

    // A source with modulations has the button that shows them in MODS, counting them, under its display.
    view.wheel(kSourcesToggle, 10.0f, {}); // the first card back in view
    for (int k = 0; k < 5; ++k) {
        model.set(slotTargetIndex(0, kSlots - 1 - k), slotTargetValue(paramIndex(k, OpParam::Fine)));
        model.set(slotAmountIndex(0, kSlots - 1 - k), 0.1 * (k + 1));
    }
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "11c-card-mods.png"), "render a card counting its modulations");

    // Up to 16: then the add buttons go.
    for (int n = static_cast<int>(shown(model).size()); n < kMaxSources; ++n)
        click(view, sourceAdd(n % 2 ? SourceType::Env : SourceType::Lfo));
    check(shown(model).size() == static_cast<size_t>(kMaxSources), "the panel holds 16 sources");
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "12-sources-full.png"), "render 16 sources");

    // Collapsed, the modulation panel keeps its title row and the editor gets shorter.
    const float open = view.height();
    click(view, kSourcesToggle);
    check(layout.sourcesCollapsed && view.height() == open - 216.0f, "the toggle collapses the modulation panel");
    view.mouseMove({400.0f, 300.0f}, {});
    check(renderer.save(view, dir / "13-collapsed.png"), "render the collapsed modulation panel");
    click(view, kSourcesToggle);
    check(!layout.sourcesCollapsed && view.height() == open, "the toggle opens the panel again");

    // Init and Load ask before throwing away a changed patch; Save never asks.
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
    click(view, kSaveButton);
    check(fileConfirm && model.loads == 1 && model.saves == 1, "the dialog before a file loads is a line shorter");
    click(view, kLoadButton);
    click(view, {shorter.right() - 64.0f, shorter.bottom() - 38.0f});
    check(model.loads == 2 && model.saves == 1, "Load asks first, Save goes ahead");
    view.mouseMove(kSaveButton, {});
    check(renderer.save(view, dir / "15b-toolbar.png"), "render the preset icons with a tooltip");

    // The cog after the preset buttons opens the settings dialog, which takes every click until it closes.
    const Point cog = kSettingsButton;
    const Rect dialog{(View::kWidth - 380.0f) * 0.5f, std::round((view.height() - 364.0f) * 0.5f), 380.0f, 364.0f};
    auto option = [&](int i) { return Point{dialog.x + 24.0f + i * 64.0f + 29.0f, dialog.y + 96.0f}; };
    auto theme = [&](int i) {
        const float w = (dialog.w - 48.0f + 6.0f) / View::themeCount();
        return Point{dialog.x + 24.0f + i * w + (w - 6.0f) * 0.5f, dialog.y + 229.0f};
    };
    const Point closeDialog{dialog.right() - 26.0f, dialog.y + 26.0f};
    click(view, cog);
    click(view, option(2));
    check(model.oversamplingFactor == 4, "the settings dialog picks the oversampling");
    view.mouseMove(option(3), {});
    check(renderer.save(view, dir / "14-settings.png"), "render the settings dialog");
    // Its theme buttons repaint the whole editor in another theme.
    bool themed = true;
    for (int i = 1; i < View::themeCount(); ++i) {
        click(view, theme(i));
        themed = themed && model.themeName == View::themeName(i);
        if (i == View::themeCount() - 1)
            themed = themed && renderer.save(view, dir / "14c-settings-light.png");
        click(view, closeDialog);
        view.mouseMove({600.0f, 300.0f}, {});
        std::string file = std::string("14d-theme-") + View::themeName(i) + ".png";
        std::transform(file.begin(), file.end(), file.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        themed = themed && renderer.save(view, dir / file);
        click(view, kRoutingTab);
        view.mouseMove({600.0f, 300.0f}, {});
        file.insert(file.size() - 4, "-routing");
        themed = themed && renderer.save(view, dir / file);
        click(view, kFxTab);
        click(view, cog);
    }
    click(view, theme(0));
    check(themed && model.themeName == View::themeName(0), "the settings dialog picks the theme");
    // The GLOW slider sets how strongly the graphs' curves glow, on both pages, once it is let go.
    auto glowAt = [&](float amount) { return Point{dialog.x + 76.0f + 200.0f * amount, dialog.y + 269.0f}; };
    view.mouseDown(glowAt(0.2f), {});
    view.mouseMove(glowAt(0.75f), {});
    const bool kept = model.glowLevel == 0.0; // only once it is let go
    view.mouseUp(glowAt(0.75f), {});
    const bool glowing = kept && std::abs(model.glowLevel - 0.75) < 1e-3;
    view.mouseMove(glowAt(0.75f), {});
    const bool sliderShot = renderer.save(view, dir / "14f-glow-slider.png");
    click(view, closeDialog);
    view.mouseMove({600.0f, 300.0f}, {});
    const bool glowShots = sliderShot && renderer.save(view, dir / "14e-glow.png");
    click(view, kRoutingTab);
    view.mouseMove({600.0f, 300.0f}, {});
    check(glowing && glowShots && renderer.save(view, dir / "14e-glow-routing.png"),
          "the settings dialog's GLOW slider sets how strongly the curves glow");
    click(view, kFxTab);
    click(view, cog);
    view.mouseDown(glowAt(0.3f), {});
    view.mouseMove({glowAt(0.0f).x - 40.0f, glowAt(0.0f).y}, {});
    view.mouseUp({glowAt(0.0f).x - 40.0f, glowAt(0.0f).y}, {});
    check(model.glowLevel == 0.0, "the GLOW slider's left end turns it off");
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
    renderExample(dir, renderer, "PD Crystal Motion", "24-example", 6);
    renderExample(dir, renderer, "BA Reese Bass", "25-reese", 4);
    renderExample(dir, renderer, "LD Folded Lead", "31-folded-lead", 2);
    renderExample(dir, renderer, "BA Fuzz Bass", "32-fuzz-bass", 2);
    renderExample(dir, renderer, "SQ Step Filter", "33-step-filter", 3);
    renderLfoOnLfo(dir, renderer);
    testMacros(dir, renderer);
    testVelocity(dir, renderer);
    testWireBend(dir, renderer);
    testModsEmpty(dir, renderer);
    testNewSources(dir, renderer);
    testMods(dir, renderer);
    testModsMoveDepth(dir, renderer);
    testModsSearch(dir, renderer);
    testMeter(dir, renderer);
    renderNewEffects(dir, renderer);
    testShapers(dir, renderer);
    testFolding(dir, renderer);
    testSourceShadow(dir, renderer);
    testCardEdgeScroll(dir, renderer);
    testMiddleDrag(dir, renderer);
    testModFocus(dir, renderer);
    testModPill(dir, renderer);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fmvibe-uishot <output-dir> [scale]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    std::setvbuf(stdout, nullptr, _IONBF, 0); // every line out before a crash, if there is one
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    run(dir, argc > 2 ? static_cast<float>(std::atof(argv[2])) : 1.0f); // COM objects die before CoUninitialize
    CoUninitialize();
    std::printf("%s (images in %s)\n", failures ? "UI CHECKS FAILED" : "all UI checks passed", dir.string().c_str());
    return failures ? 1 : 0;
}
