// The browser demo's editor side: the plugin's View on the page's canvas, with a Controller that keeps the patch
// here on the main thread and sends every change to the audio side (web/dsp.cpp, through web/site/main.js and
// the worklet). It stands in for Plugin's Controller half, minus the host: presets are the built-in factory ones,
// held in memory, and nothing is read from or written to files.

#include "factory.h"
#include "gui/canvas.h"
#include "gui/controller.h"
#include "gui/view.h"
#include "params.h"
#include "synth.h"
#include "uistrings.h"

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace fmvibe;
using namespace fmvibe::gui;

namespace {

// What the page does for the editor: Module.fmvHost in web/site/main.js.
EM_JS_DEPS(fmv_host, "$UTF8ToString,$stringToUTF8,$lengthBytesUTF8");
EM_JS(void, js_param, (int index, double value), { Module.fmvHost.param(index, value); });
EM_JS(void, js_all_params, (const double *values, int count),
      { Module.fmvHost.allParams(HEAPF64.slice(values >> 3, (values >> 3) + count)); });
EM_JS(void, js_note, (int key, int on), { Module.fmvHost.note(key, on != 0); });
EM_JS(void, js_bend, (double amount), { Module.fmvHost.bend(amount); });
EM_JS(void, js_scope, (int op, int cycles), { Module.fmvHost.scope(op, cycles); });
EM_JS(void, js_oversampling, (int factor), { Module.fmvHost.oversampling(factor); });
EM_JS(void, js_layout_changed, (), { Module.fmvHost.layoutChanged(); });
EM_JS(void, js_toast, (const char *utf8), { Module.fmvHost.toast(UTF8ToString(utf8)); });
// The user's settings (theme, glow) live in the browser's storage; -1 when there is none.
EM_JS(int, js_setting, (const char *key, char *out, int capacity), {
    const value = Module.fmvHost.setting(UTF8ToString(key));
    if (value === null || value === undefined)
        return -1;
    stringToUTF8(value, out, capacity);
    return lengthBytesUTF8(value);
});
EM_JS(void, js_set_setting, (const char *key, const char *value),
      { Module.fmvHost.setSetting(UTF8ToString(key), UTF8ToString(value)); });

constexpr int kDefaultOversampling = 2;
constexpr char kFirstPreset[] = "PD Crystal Motion"; // the patch in docs/editor.png

std::string setting(const char *key) {
    char buffer[256];
    const int length = js_setting(key, buffer, sizeof(buffer));
    return length < 0 ? std::string() : std::string(buffer, std::min<size_t>(length, sizeof(buffer) - 1));
}

// Whether `a` sorts before `b`, ignoring case (as the plugin's preset library).
bool lessNoCase(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

class WebController final : public Controller {
public:
    WebController() {
        const ParamTable &table = paramTable();
        for (int i = 0; i < kParamCount; ++i)
            values_[i] = table[i].def;
        examples_ = factoryPresets();
        std::sort(examples_.begin(), examples_.end(),
                  [](const FactoryPreset &a, const FactoryPreset &b) { return lessNoCase(a.name, b.name); });
        for (const FactoryPreset &preset : examples_)
            library_.push_back({preset.name, "Factory"});
        theme_ = setting("theme");
        const std::string glow = setting("glow");
        glow_ = glow.empty() ? 0.5 : std::clamp(std::atof(glow.c_str()), 0.0, 1.0);
    }

    const ParamValues &values() const { return values_; }
    Telemetry &received() { return received_; }
    void stamp(double ms) {
        stampMs_ = ms;
        heard_ = true;
    }

    double value(int index) const override { return values_[index]; }
    // The newest snapshot from the audio side, aged by how long ago it was taken (as SharedTelemetry::load()).
    void telemetry(Telemetry &out) const override {
        if (!heard_) {
            out = Telemetry{};
            return;
        }
        out = received_;
        out.age = static_cast<float>(std::max(0.0, emscripten_get_now() - stampMs_) / 1000.0);
    }
    UiLayout &layout() override { return layout_; }
    void layoutChanged() override { js_layout_changed(); }
    void playNote(int note, bool on) override { js_note(note, on); }
    void pitchWheel(double amount) override { js_bend(amount); }
    // The view asks every poll; only a change goes to the worklet.
    void setScope(int op, int cycles) override {
        if (op == scopeOp_ && cycles == scopeCycles_)
            return;
        scopeOp_ = op;
        scopeCycles_ = cycles;
        js_scope(op, cycles);
    }

    // Init and loading an example are undo steps like any edit in the editor, as in the plugin.
    void resetPreset() override {
        UndoScope step(*this);
        recordAll();
        const ParamTable &table = paramTable();
        for (int i = 0; i < kParamCount; ++i)
            values_[i] = table[i].def;
        replaceLayout(UiLayout{});
        js_all_params(values_.data(), kParamCount);
        layoutChanged();
        setPreset(L"");
    }
    bool loadPreset(const std::wstring &) override { return false; }
    bool savePreset(const std::wstring &) override { return false; }
    void choosePreset(bool) override { js_toast(tr(Str::DemoNoFiles)); }
    void rescanPresets() override {}
    const std::vector<PresetEntry> &presets() const override { return library_; }
    int presetIndex() const override {
        for (size_t i = 0; i < examples_.size(); ++i)
            if (fileOf(i) == presetFile())
                return static_cast<int>(i);
        return -1;
    }
    std::string presetName() const override {
        const int index = presetIndex();
        return index < 0 ? tr(Str::PresetInit) : examples_[index].name;
    }
    bool loadPresetAt(int index) override {
        if (index < 0 || index >= static_cast<int>(examples_.size()))
            return false;
        const FactoryPreset &preset = examples_[index];
        UndoScope step(*this);
        recordAll();
        const ParamTable &table = paramTable();
        for (int i = 0; i < kParamCount; ++i)
            values_[i] = table[i].def;
        for (const auto &[param, value] : preset.changes)
            values_[param] = std::clamp(value, table[param].min, table[param].max);
        replaceLayout(exampleLayout(preset));
        js_all_params(values_.data(), kParamCount);
        layoutChanged();
        setPreset(fileOf(index)); // in the step, so undoing it brings back the preset before
        return true;
    }
    std::wstring presetFolder() const override {
        const std::string name = tr(Str::DemoPresetFolder);
        return std::wstring(name.begin(), name.end()); // the browser shows a folder as far as it is ASCII
    }
    void openPresetFolder() override { js_toast(tr(Str::DemoNoFiles)); }

    int oversampling() const override { return oversampling_; }
    void setOversampling(int factor) override {
        oversampling_ = factor >= 8 ? 8 : factor >= 4 ? 4 : factor >= 2 ? 2 : 1;
        js_oversampling(oversampling_);
    }
    std::string theme() const override { return theme_; }
    void setTheme(const std::string &name) override {
        theme_ = name;
        js_set_setting("theme", name.c_str());
    }
    double glow() const override { return glow_; }
    void setGlow(double amount) override {
        glow_ = std::clamp(amount, 0.0, 1.0);
        js_set_setting("glow", std::to_string(glow_).c_str());
    }

    // Opens on an example rather than Init, so the first note sounds like something; nothing to undo yet.
    void start() {
        for (size_t i = 0; i < examples_.size(); ++i)
            if (examples_[i].name == kFirstPreset)
                loadPresetAt(static_cast<int>(i));
        clearHistory();
    }

protected:
    void performBeginEdit(int) override {}
    void performSetValue(int index, double value) override {
        values_[index] = value;
        js_param(index, value);
    }
    void performEndEdit(int) override {}

private:
    // Where an example lives, as far as presetFile() goes: a made-up path in a made-up folder.
    std::wstring fileOf(size_t index) const {
        const std::string &name = examples_[index].name;
        return L"Factory/" + std::wstring(name.begin(), name.end()); // the examples' names are ASCII
    }
    // A patch's layout, with what belongs to the editor kept (as Plugin::keepEditorLayout()).
    void replaceLayout(const UiLayout &patch) {
        const UiLayout before = layout_;
        layout_ = patch;
        layout_.zoom = before.zoom;
        layout_.sourcesCollapsed = before.sourcesCollapsed;
        layout_.keysCollapsed = before.keysCollapsed;
        layout_.shapeGridX = before.shapeGridX;
        layout_.shapeGridY = before.shapeGridY;
        layout_.opTab = before.opTab;
        layout_.scopeCycles = before.scopeCycles;
    }

    ParamValues values_{};
    UiLayout layout_;
    int scopeOp_ = -1;
    int scopeCycles_ = 2;
    std::vector<FactoryPreset> examples_;
    std::vector<PresetEntry> library_;
    Telemetry received_{};
    double stampMs_ = 0.0;
    bool heard_ = false;
    int oversampling_ = kDefaultOversampling;
    std::string theme_;
    double glow_ = 0.5;
};

std::unique_ptr<WebController> controller;
std::unique_ptr<View> view;
Resources resources;
std::vector<MenuItem> menu; // the context menu last asked for

Modifiers mods(int shift, int ctrl) {
    return {shift != 0, ctrl != 0};
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void ui_init() {
    controller = std::make_unique<WebController>();
    view = std::make_unique<View>(*controller);
    controller->start();
}

EMSCRIPTEN_KEEPALIVE float ui_width() {
    return View::kWidth;
}

EMSCRIPTEN_KEEPALIVE float ui_height() {
    return view->height();
}

EMSCRIPTEN_KEEPALIVE int ui_poll() {
    return view->poll();
}

// The page has set the transform from the editor's units to the canvas.
EMSCRIPTEN_KEEPALIVE void ui_paint() {
    view->refreshTelemetry();
    Canvas canvas(resources);
    view->paint(canvas);
}

// 1 if what the editor shows changed, so it wants a paint.
EMSCRIPTEN_KEEPALIVE int ui_mouse_move(float x, float y, int shift, int ctrl) {
    return view->mouseMove({x, y}, mods(shift, ctrl)) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void ui_mouse_down(float x, float y, int shift, int ctrl) {
    view->mouseDown({x, y}, mods(shift, ctrl));
}

EMSCRIPTEN_KEEPALIVE void ui_mouse_up(float x, float y, int shift, int ctrl) {
    view->mouseUp({x, y}, mods(shift, ctrl));
}

EMSCRIPTEN_KEEPALIVE void ui_mouse_leave() {
    view->mouseLeave();
}

EMSCRIPTEN_KEEPALIVE void ui_double_click(float x, float y, int shift, int ctrl) {
    view->doubleClick({x, y}, mods(shift, ctrl));
}

EMSCRIPTEN_KEEPALIVE void ui_wheel(float x, float y, float steps, int shift, int ctrl) {
    view->wheel({x, y}, steps, mods(shift, ctrl));
}

// The menu for a right-click there: how many items, then each item's command (0 for a separator) and label.
EMSCRIPTEN_KEEPALIVE int ui_context_menu(float x, float y) {
    menu = view->contextMenu({x, y});
    return static_cast<int>(menu.size());
}

EMSCRIPTEN_KEEPALIVE int ui_menu_command(int item) {
    return item >= 0 && item < static_cast<int>(menu.size()) ? menu[item].command : 0;
}

EMSCRIPTEN_KEEPALIVE const char *ui_menu_label(int item) {
    return item >= 0 && item < static_cast<int>(menu.size()) ? menu[item].label.c_str() : "";
}

// 1 for an item with a check mark: the choice in force.
EMSCRIPTEN_KEEPALIVE int ui_menu_checked(int item) {
    return item >= 0 && item < static_cast<int>(menu.size()) && menu[item].checked ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void ui_menu_pick(int command) {
    view->menuCommand(command);
}

EMSCRIPTEN_KEEPALIVE int ui_cursor() {
    return static_cast<int>(view->cursor());
}

EMSCRIPTEN_KEEPALIVE void ui_undo() {
    view->undo();
}

EMSCRIPTEN_KEEPALIVE void ui_redo() {
    view->redo();
}

EMSCRIPTEN_KEEPALIVE int ui_typing() {
    return view->typing();
}

EMSCRIPTEN_KEEPALIVE void ui_type_char(int codepoint) {
    view->typeChar(static_cast<char32_t>(codepoint));
}

// A View::Key, by number; Shift extends the search's selection.
EMSCRIPTEN_KEEPALIVE void ui_type_key(int key, int shift) {
    view->typeKey(static_cast<View::Key>(key), shift != 0);
}

// What Ctrl+C (or Ctrl+X, with `cut`) takes for the clipboard, as UTF-8; what Ctrl+V brings is typed in.
EMSCRIPTEN_KEEPALIVE const char *ui_copy(int cut) {
    static std::string copied;
    copied = view->copyText(cut != 0);
    return copied.c_str();
}

EMSCRIPTEN_KEEPALIVE void ui_end_typing(int commit) {
    view->endTyping(commit != 0);
}

// The patch, for the audio side to start from.
EMSCRIPTEN_KEEPALIVE const double *ui_values() {
    return controller->values().data();
}

EMSCRIPTEN_KEEPALIVE int ui_param_count() {
    return kParamCount;
}

EMSCRIPTEN_KEEPALIVE int ui_oversampling() {
    return controller->oversampling();
}

// Where the page copies each telemetry snapshot from the audio side, then stamps it with the time it came.
EMSCRIPTEN_KEEPALIVE Telemetry *ui_telemetry_buffer() {
    return &controller->received();
}

EMSCRIPTEN_KEEPALIVE int ui_telemetry_size() {
    return static_cast<int>(sizeof(Telemetry));
}

EMSCRIPTEN_KEEPALIVE void ui_telemetry_stamp(double nowMs) {
    controller->stamp(nowMs);
}

} // extern "C"
