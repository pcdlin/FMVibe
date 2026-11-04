#pragma once

// What the CLAP and the VST3 plugin share: the engine and its param values, the editor's Controller (its undo
// history, presets and settings), the state's bytes and the editor window's life. Each format's plugin derives from
// it, feeds it its host's notes and values, and tells its host what the core did through the host...() hooks.

#include "gui/controller.h"
#include "gui/editor.h"
#include "presets.h"
#include "sharedtelemetry.h"
#include "state.h"
#include "synth.h"
#include "usersettings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fmvibe {

// Single-producer single-consumer ring buffer: editor (main thread) to audio thread.
template <class T, size_t N>
class SpscQueue {
public:
    bool push(const T &item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        items_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Producer only: how many more items fit for certain (the consumer may make more room meanwhile).
    size_t room() const {
        const size_t head = head_.load(std::memory_order_relaxed);
        return (tail_.load(std::memory_order_acquire) + N - head - 1) % N;
    }

    bool pop(T &item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        item = items_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return true;
    }

private:
    std::array<T, N> items_{};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

// The plugin's name, maker and description, as both formats report them.
inline constexpr char kPluginName[] = "FMVibe";
inline constexpr char kPluginVendor[] = "pcdlin";
inline constexpr char kPluginDescription[] = "Six-operator FM synthesizer";

// Milliseconds on the steady clock.
int64_t steadyMs();

class PluginCore : public gui::Controller {
public:
    PluginCore();
    ~PluginCore() override;
    PluginCore(const PluginCore &) = delete;
    PluginCore &operator=(const PluginCore &) = delete;

    // gui::Controller, main thread.
    double value(int index) const override { return values_[index].load(); }
    void telemetry(Telemetry &out) const override { telemetry_.load(out); }
    gui::UiLayout &layout() override { return layout_; }
    void layoutChanged() override;
    void playNote(int note, bool on) override;
    void pitchWheel(double amount) override { wheelBend_.store(amount); }
    void setScope(int op, int cycles) override {
        scopeOp_.store(op, std::memory_order_relaxed);
        scopeCycles_.store(cycles, std::memory_order_relaxed);
    }
    void resetPreset() override;
    bool loadPreset(const std::wstring &path) override;
    bool savePreset(const std::wstring &path) override;
    void choosePreset(bool save) override;
    void rescanPresets() override { presets_.rescan(); }
    const std::vector<gui::PresetEntry> &presets() const override { return presets_.entries(); }
    int presetIndex() const override;
    std::string presetName() const override;
    bool loadPresetAt(int index) override;
    std::wstring presetFolder() const override;
    void openPresetFolder() override;
    int oversampling() const override { return oversampling_.load(); }
    void setOversampling(int factor) override;
    std::string theme() const override { return userSettings.theme; }
    void setTheme(const std::string &name) override {
        userSettings.theme = name;
        writeUserSettings();
    }
    double glow() const override { return userSettings.glow; }
    void setGlow(double amount) override {
        userSettings.glow = std::clamp(amount, 0.0, 1.0);
        writeUserSettings();
    }

    // Every param's value now. Any thread.
    ParamValues valuesNow() const;
    const gui::UiLayout &uiLayout() const { return layout_; }
    const Synth &synth() const { return synth_; }
    // How many voices may sound at once, from the Voices param.
    int voiceLimit() const;

    // Main thread, while inactive: allocates the voices, for the current voice limit and oversampling.
    void activate(double sampleRate);
    void deactivate() { telemetry_.store(Telemetry{}); }
    // Main thread, while not processing (or the audio thread, between blocks).
    void resetVoices() { synth_.reset(); }

    // Audio thread. A block starts with beginBlock(), which takes what the editor did since the last one (its keys,
    // bend wheel and scope) and a copy of the params; renders in spans, between which the host's notes and values
    // go to synth() and setHostValue(); and ends with endBlock(), which hands the editor its telemetry. Denormals are
    // flushed to zero in between.
    void beginBlock();
    void render(float *left, float *right, uint32_t frames);
    void endBlock();
    Synth &synth() { return synth_; }

    // A param's value from the host, which a block being processed hears from here on (`inBlock`: the audio thread,
    // or the main thread while no block is processed). For a moment after the plugin told the host a value, the
    // host's for that param is not taken: hosts hand values back a little later (Bitwig does), and taking them
    // would flip a preset loaded meanwhile back to the one before.
    void setHostValue(int index, double value, bool inBlock);

    // The state, the same bytes in either format: the patch, then (`settings`) the settings and the preset chunks.
    // Preset files leave out the settings, and loading one keeps them as they are. Main thread.
    bool saveState(const clap_ostream_t *stream, bool settings = true) const;
    bool loadState(const clap_istream_t *stream, bool settings = true);

    // Main thread: what hostRequestCallback() asks for.
    void onMainThread() { sectionsChanged(); }

    // The editor window, while the host shows it. Main thread.
    gui::Editor *editor() const { return editor_.get(); }
    bool openEditor();
    void closeEditor();

    // Which effect each slot holds, the chain order, which source each place holds, each reverb's algorithm and
    // which operators are fixed: what hosts see of the effects and sources.
    static bool shapesSections(int index) {
        return fxFieldOf(index) == FxParam::Type || sourceFieldOf(index) == SourceParam::Type ||
               index == paramIndex(GlobalParam::FxOrder) || fxFieldOf(index) == FxParam::ReverbType ||
               fixedSwitchOp(index) >= 0; // a fixed operator's page has Freq and Mult in Ratio's and Fine's places
    }

protected:
    void performBeginEdit(int index) override { hostBeginEdit(index); }
    void performSetValue(int index, double value) override;
    void performEndEdit(int index) override { hostEndEdit(index); }

    // How each format tells its host. Main thread, unless they say otherwise.
    // An edit in the editor (or a preset load) of param `index`: a gesture's begin, its values and its end.
    virtual void hostBeginEdit(int index) = 0;
    virtual void hostEdit(int index, double value) = 0;
    virtual void hostEndEdit(int index) = 0;
    // Every param's value may have changed: a state was loaded.
    virtual void hostValuesChanged() = 0;
    // The sections changed (see shapesSections()): `fieldsShown` when which fields hosts see changed with them.
    virtual void hostSectionsChanged(bool fieldsShown) = 0;
    // The project has changes to save.
    virtual void hostMarkDirty() = 0;
    // The voice limit changed.
    virtual void hostVoicesChanged() {}
    // The editor would be this big now (in physical pixels): its height follows the modulation panel.
    virtual void hostResizeEditor(uint32_t width, uint32_t height) = 0;
    // Any thread: a deactivate and activate, for more voices than are allocated or another oversampling factor.
    virtual void hostRequestRestart() = 0;
    // Any thread: onMainThread(), soon.
    virtual void hostRequestCallback() = 0;
    // The virtual keyboard played a note, while the host may have stopped processing.
    virtual void hostRequestProcess() {}

    // The host was told param `index`'s value just now (see setHostValue()).
    void told(int index, int64_t now) { toldAt_[index].store(now, std::memory_order_relaxed); }
    // Main thread: whether a restart was asked for and not yet done (activate() clears it).
    bool restartPending() const { return restartRequested_.load(); }

    // Main thread. A slot's or place's fields only show in hosts while it holds their type, and each effect and
    // source gets a remote page.
    void sectionsChanged();

private:
    void applyPlayMode();
    void playKeys();
    void voiceLimitChanged();
    // A new oversampling factor takes a deactivate/activate, like more voices.
    void restartIfNeeded();
    // The editor's height follows the modulation panel; asks the host to resize the window.
    void editorFitChanged();
    // Tells the host of every param that changed since `before`, as edits.
    void notifyParams(const ParamValues &before);
    // What a preset leaves as it is: how big the editor is, whether the modulation panel is
    // collapsed, the shape editor's grid.
    void keepEditorLayout(const gui::UiLayout &from);

    // A key of the editor's virtual keyboard, on its way to the audio thread.
    struct KeyEvent {
        int note;
        bool on;
    };

    Synth synth_;
    // Raising the voice limit past the allocated voices, or a new oversampling
    // factor, needs a deactivate/activate.
    std::atomic<bool> restartRequested_{false};
    std::atomic<int> oversampling_{kDefaultOversampling};
    // Written by the audio thread (param events) and the main thread (editor,
    // state load), read by both, hence atomics. The audio thread works from a
    // copy taken at the start of each block, which the block's param events update.
    std::array<std::atomic<double>, kParamCount> values_;
    ParamValues snapshot_{};
    // When each param's value last went out to the host (steady clock, ms); see setHostValue().
    std::array<std::atomic<int64_t>, kParamCount> toldAt_{};
    SpscQueue<KeyEvent, 256> keys_;
    std::atomic<double> wheelBend_{0.0};
    SharedTelemetry telemetry_;
    std::atomic<bool> editorOpen_{false};
    std::atomic<int> scopeOp_{-1}; // what the editor's scope shows (see Controller::setScope())
    std::atomic<int> scopeCycles_{2};
    unsigned int csr_ = 0; // the audio thread's floating-point control word, while a block is processed
    // Main thread: what the sections were when hosts were last told: the effect types, then the chain order, the
    // source types, the sources' order and each reverb's algorithm.
    std::array<int64_t, kFxSlots + 1 + 2 * kMaxSources + kFxSlots> shownSections_{};
    gui::UiLayout layout_;
    std::unique_ptr<gui::Editor> editor_;
    // The preset library as last scanned, with each entry's file (the patch's own is the Controller's).
    // Main thread.
    PresetLibrary presets_;
};

} // namespace fmvibe
