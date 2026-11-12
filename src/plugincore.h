#pragma once

// What the CLAP and the VST3 plugin share: the engine and its param values, the editor's Controller (its undo
// history, presets and settings), the state's bytes and the editor window's life. Each format's plugin derives from
// it, feeds it its host's notes and values, and tells its host what the core did through the host...() hooks.

#include "gui/controller.h"
#include "gui/editor.h"
#include "midilearn.h"
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
    uint64_t valuesStamp() const override { return valuesStamp_.load(std::memory_order_acquire); }
    void telemetry(Telemetry &out) const override { telemetry_.load(out); }
    gui::UiLayout &layout() override { return layout_; }
    void layoutChanged() override;
    void playNote(int note, bool on) override;
    void setScope(int op, int cycles) override {
        scopeOp_.store(op, std::memory_order_relaxed);
        scopeCycles_.store(cycles, std::memory_order_relaxed);
    }
    void setIdleLfosShown(bool shown) override;
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
    bool mpe() const override { return mpe_.load(); }
    void setMpe(bool on) override;
    int mpeBendRange() const override { return mpeBendRange_.load(); }
    void setMpeBendRange(int semitones) override;
    std::string theme() const override { return userSettings.theme; }
    void setTheme(const std::string &name) override {
        userSettings.theme = name;
        writeUserSettings();
    }
    double accentHue() const override { return userSettings.hue; }
    void setAccentHue(double hue) override {
        userSettings.hue = hue < 0.0 ? -1.0 : std::clamp(hue, 0.0, 1.0);
        writeUserSettings();
    }
    double glow() const override { return userSettings.glow; }
    void setGlow(double amount) override {
        userSettings.glow = std::clamp(amount, 0.0, 1.0);
        writeUserSettings();
    }
    int handleSize() const override { return userSettings.handleSize; }
    void setHandleSize(int size) override {
        userSettings.handleSize = std::clamp(size, 0, kMaxHandleSize);
        writeUserSettings();
    }
    bool midiLearnOffered() const override { return true; }
    int midiCc(int index) const override { return midiLearn_.ccOf(index); }
    int midiLearnArmed() const override { return midiLearn_.armed(); }
    uint64_t midiLearnStamp() const override { return midiLearn_.stamp(); }
    void armMidiLearn(int index) override { midiLearn_.arm(index); }
    void forgetMidi(int index) override;
    double linkIntensity() const override { return userSettings.links; }
    void setLinkIntensity(double amount) override {
        userSettings.links = std::clamp(amount, 0.0, 1.0);
        writeUserSettings();
    }

    // Every param's value now. Any thread. ParamValues is large (about 150 KB): valuesInto() fills one in place, kept
    // off the stack.
    ParamValues valuesNow() const;
    void valuesInto(ParamValues &values) const;
    const gui::UiLayout &uiLayout() const { return layout_; }
    const Synth &synth() const { return synth_; }
    // How many voices may sound at once, from the Voices param.
    int voiceLimit() const;

    // Main thread, while inactive: allocates the voices, for the current voice limit and oversampling.
    void activate(double sampleRate);
    void deactivate() { telemetry_.store(*noTelemetry_); } // from one made ahead, not a 45 KB one on the stack
    // Main thread, while not processing (or the audio thread, between blocks).
    void resetVoices() { synth_.reset(); }

    // Audio thread. A block starts with beginBlock(), which takes what the editor did since the last one (its keys
    // and scope) and a copy of the params; renders in spans, between which the host's notes and values
    // go to synth() and setHostValue(); and ends with endBlock(), which hands the editor its telemetry. Denormals are
    // flushed to zero in between.
    void beginBlock();
    void render(float *left, float *right, uint32_t frames);
    void endBlock();
    Synth &synth() { return synth_; }
    // Audio thread: whether the open editor shows an LFO running with no note playing (see setIdleLfosShown()), so a
    // host is to keep processing though nothing sounds.
    bool editorWantsProcessing() const {
        return editorOpen_.load(std::memory_order_relaxed) && idleLfosShown_.load(std::memory_order_relaxed);
    }

    // A param's value from the host, which a block being processed hears from here on (`inBlock`: the audio thread,
    // or the main thread while no block is processed). For a moment after the plugin told the host a value, the
    // host's for that param is not taken: hosts hand values back a little later (Bitwig does), and taking them
    // would flip a preset loaded meanwhile back to the one before.
    void setHostValue(int index, double value, bool inBlock);
    // Audio thread: a hardware mod wheel (MIDI CC 1, on any channel) moved, to `value` (0..1). It moves the Mod Wheel
    // param, which the rest of the block hears and the editor's wheel shows, as the wheel's own knob would; it is
    // played rather than part of the patch, so nothing is undone or marked changed. True when the param changed: the
    // caller then tells the host its new value, in its place in the block, as a param edit from the plugin.
    bool playModWheel(double value);
    // Audio thread: MIDI controller `controller` (0..127, on any channel) moved, to `value` (0..1). A param waiting for
    // a CC learns it (see MidiLearn), and the param it is mapped to moves to where the CC puts it, played as the mod
    // wheel is (the rest of the block hears it, nothing is undone, the host is told). The param's index when it
    // changed and hosts are given it: the caller then tells the host its new value (valueNow()), in its place in the
    // block, as a param edit from the plugin. -1 otherwise; a change to a param hosts aren't given marks the project
    // changed instead, from the main thread. Hearing the CC (a MIDI source, the mod wheel) is the caller's.
    int playLearnedCc(int controller, double value);
    // Any thread: param `index`'s value now.
    double valueNow(int index) const { return values_[index].load(std::memory_order_relaxed); }

    // The state, the same bytes in either format: the patch, then (`settings`) the settings and the preset chunks.
    // Preset files leave out the settings, and loading one keeps them as they are. Main thread.
    bool saveState(const clap_ostream_t *stream, bool settings = true) const;
    bool loadState(const clap_istream_t *stream, bool settings = true);

    // Main thread: what hostRequestCallback() asks for.
    void onMainThread();

    // The editor window, while the host shows it. Main thread.
    gui::Editor *editor() const { return editor_.get(); }
    bool openEditor();
    void closeEditor();

    // Which source each place holds and which operators are fixed: what the remote pages follow (a page per source,
    // and a fixed operator's has Freq and Mult in Ratio's and Pitch's places), with the sources' order.
    static bool shapesSections(int index) {
        return sourceFieldOf(index) == SourceParam::Type || fixedSwitchOp(index) >= 0;
    }

protected:
    void performBeginEdit(int index) override {
        if (hostListed(index))
            hostBeginEdit(index);
    }
    void performSetValue(int index, double value) override;
    void performEndEdit(int index) override {
        if (hostListed(index))
            hostEndEdit(index);
    }
    void performStepBegin() override;
    void performStepEnd() override;
    void requestEditorSize(uint32_t width, uint32_t height) override { hostResizeEditor(width, height); }

    // How each format tells its host. Main thread, unless they say otherwise.
    // An edit in the editor (or a preset load) of param `index`, one hosts are given (see hostListed()): a gesture's
    // begin, its values and its end. An edit of another one marks the project changed instead.
    virtual void hostBeginEdit(int index) = 0;
    virtual void hostEdit(int index, double value) = 0;
    virtual void hostEndEdit(int index) = 0;
    // Every param's value may have changed: a state was loaded.
    virtual void hostValuesChanged() = 0;
    // The sections changed (see shapesSections()): the remote pages follow.
    virtual void hostSectionsChanged() {}
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
    bool restartPending() const { return restartAskedFor_.load() != 0; }

    // Main thread: tells the host when the remote pages differ.
    void sectionsChanged();

private:
    // Audio thread: param `index` set to `value` as a played control (the mod wheel, a learned CC): the rest of the
    // block hears it and the editor sees it. True when it changed.
    bool playValue(int index, double value);
    void applyPlayMode();
    void playKeys();
    void voiceLimitChanged();
    // A new oversampling factor takes a deactivate/activate, like more voices.
    void restartIfNeeded();
    // Asks the host for a restart when the voices or the oversampling need one, unless it was asked for this
    // already. Thread-safe.
    void askRestart();
    // The editor's height follows the modulation panel; asks the host to resize the window.
    void editorFitChanged();
    // Tells the host of every param that changed since `before`, as edits.
    void notifyParams(const ParamValues &before);
    // What a preset leaves as it is (gui::keepEditorLayout()): how big the editor is, whether the modulation panel
    // is collapsed, the shape editor's grid, the view shown.
    void keepEditorLayout(const gui::UiLayout &from);

    // A key of the editor's virtual keyboard, on its way to the audio thread.
    struct KeyEvent {
        int note;
        bool on;
    };

    Synth synth_;
    // Raising the voice limit past the allocated voices, or a new oversampling factor, needs a deactivate/activate:
    // what the restart was last asked for (the voices times 16, plus the factor), or 0. A host that doesn't restart
    // is asked again once something else needs one.
    std::atomic<int> restartAskedFor_{0};
    // Main thread: an undo or redo step being applied, and whether an edit in it marks the project changed.
    bool applyingStep_ = false;
    bool dirtyAfterStep_ = false;
    std::atomic<int> oversampling_{kDefaultOversampling};
    // MPE over MIDI and its member channels' bend range: settings, set on the main thread, read by the audio thread.
    std::atomic<bool> mpe_{false};
    std::atomic<int> mpeBendRange_{kDefaultMpeBendRange};
    // The MIDI CCs learned and the param waiting for one: a setting like MPE, saved in the settings chunk. The audio
    // thread learns and plays them; what it changed that the host hears of from the main thread (a mapping learned,
    // a param hosts aren't given moved) waits in midiDirty_ for onMainThread().
    MidiLearn midiLearn_;
    std::atomic<bool> midiDirty_{false};
    // Written by the audio thread (param events) and the main thread (editor,
    // state load), read by both, hence atomics. The audio thread works from a
    // copy taken at the start of each block, which the block's param events update.
    std::array<std::atomic<double>, kParamCount> values_;
    ParamValues snapshot_{};
    // How many times values_ was written outside a block (see storeValue()), and what of it the snapshot holds: a
    // block copies values_ only when they differ. Whether the snapshot changed since the synth last rendered, which
    // has it work out again what follows from the params (see Synth::render()).
    std::atomic<uint64_t> valuesVersion_{1};
    uint64_t snapshotVersion_ = 0;
    bool snapshotChanged_ = true;
    // Stores a value outside a block, so the next block's snapshot takes it; storing many at once, valuesChanged()
    // once after them does the same.
    void storeValue(int index, double value) {
        values_[index].store(value);
        valuesChanged();
    }
    void valuesChanged() {
        valuesVersion_.fetch_add(1, std::memory_order_release);
        stamp();
    }
    // Any thread, after a value is stored, whether or not the snapshot takes it there: what valuesStamp() moves on.
    void stamp() { valuesStamp_.fetch_add(1, std::memory_order_release); }
    std::atomic<uint64_t> valuesStamp_{1};
    // When each param's value last went out to the host (steady clock, ms); see setHostValue().
    std::array<std::atomic<int64_t>, kParamCount> toldAt_{};
    SpscQueue<KeyEvent, 256> keys_;
    SharedTelemetry telemetry_;
    // Audio thread: the telemetry's snapshots (see endBlock()), how often, the audio since the last one, whether the
    // editor was open at the last block, and where a snapshot is put together (on the heap: it is large).
    static constexpr double kTelemetrySeconds = 0.008;
    double sampleRate_ = 48000.0;
    double telemetryFrames_ = 0.0;
    bool telemetryOpen_ = false;
    std::unique_ptr<Telemetry> telemetryScratch_ = std::make_unique<Telemetry>();
    const std::unique_ptr<const Telemetry> noTelemetry_ = std::make_unique<const Telemetry>(); // what nothing playing shows
    std::atomic<bool> editorOpen_{false};
    std::atomic<bool> idleLfosShown_{false}; // see setIdleLfosShown()
    std::atomic<int> scopeOp_{-1}; // what the editor's scope shows (see Controller::setScope())
    std::atomic<int> scopeCycles_{2};
    unsigned int csr_ = 0; // the audio thread's floating-point control word, while a block is processed
    // Main thread: what the sections were when hosts were last told: whether each operator is fixed, then which
    // source each place holds and the sources' order.
    std::array<int64_t, kOps + 2 * kMaxSources> shownSections_{};
    gui::UiLayout layout_;
    std::unique_ptr<gui::Editor> editor_;
    // The preset library as last scanned, with each entry's file (the patch's own is the Controller's).
    // Main thread.
    PresetLibrary presets_;
};

} // namespace fmvibe
