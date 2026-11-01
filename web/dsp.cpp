// The browser demo's audio side: the synth behind a few C functions, which web/site/worklet.js calls from the
// AudioWorklet. It does what Plugin::process() and Plugin::activate() do for a host, with the editor's
// edits and notes arriving as messages instead of CLAP events. Built as a standalone wasm module, with no
// Emscripten JS around it, since that doesn't run in a worklet.

#include "params.h"
#include "synth.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>

using namespace fmvibe;

namespace {

constexpr int kMaxFrames = 4096;               // a render quantum is 128 frames, but browsers may pick others
constexpr double kKeyVelocity = 100.0 / 127.0; // as the plugin's virtual keyboard

Synth synth;
ParamValues values{};
float left[kMaxFrames];
float right[kMaxFrames];
Telemetry telemetry{};
double sampleRate = 48000.0;
int oversampling = 2;
double wheelBend = 0.0;

int voiceLimit() {
    return std::clamp(static_cast<int>(std::lround(values[paramIndex(GlobalParam::Voices)])), 1, kMaxVoices);
}

void applyPlayMode() {
    auto on = [](GlobalParam p) { return values[paramIndex(p)] >= 0.5; };
    synth.setPlayMode(on(GlobalParam::Mono), on(GlobalParam::Legato), on(GlobalParam::GlideAlways));
}

} // namespace

extern "C" {

// The param values, for the page to fill before dsp_init().
EMSCRIPTEN_KEEPALIVE double *dsp_values() {
    return values.data();
}

EMSCRIPTEN_KEEPALIVE int dsp_param_count() {
    return kParamCount;
}

EMSCRIPTEN_KEEPALIVE void dsp_init(double rate, int factor) {
    sampleRate = rate;
    oversampling = factor;
    synth.prepare(sampleRate, voiceLimit(), oversampling);
}

EMSCRIPTEN_KEEPALIVE void dsp_set(int index, double value) {
    if (index >= 0 && index < kParamCount && std::isfinite(value))
        values[index] = value;
}

// Notes from the editor's keyboard and the computer's, like MIDI notes without a note id.
EMSCRIPTEN_KEEPALIVE void dsp_note(int key, int on) {
    const NoteRef note{-1, 0, 0, static_cast<int16_t>(std::clamp(key, 0, 127))};
    if (on)
        synth.noteOn(note, kKeyVelocity);
    else
        synth.noteOff(note);
}

EMSCRIPTEN_KEEPALIVE void dsp_bend(double amount) {
    wheelBend = amount;
}

EMSCRIPTEN_KEEPALIVE void dsp_set_oversampling(int factor) {
    oversampling = factor;
}

EMSCRIPTEN_KEEPALIVE float *dsp_left() {
    return left;
}

EMSCRIPTEN_KEEPALIVE float *dsp_right() {
    return right;
}

EMSCRIPTEN_KEEPALIVE void dsp_render(int frames) {
    frames = std::clamp(frames, 0, kMaxFrames);
    // More voices than are allocated, or a new oversampling factor, take a fresh prepare(): what a host's
    // restart does for the plugin. It allocates, so the sound stops for a moment.
    if (voiceLimit() > synth.capacity() || oversampling != synth.oversampling())
        synth.prepare(sampleRate, voiceLimit(), oversampling);
    synth.setWheelBend(wheelBend);
    synth.setAnalyzing(true); // the editor is always open
    synth.setVoiceLimit(voiceLimit());
    applyPlayMode();
    synth.render(left, right, static_cast<uint32_t>(frames), values);
    synth.clearEndedNotes(); // nobody to report them to
}

// The editor's view of the newest voice, taken now; the page sends these bytes over as they are.
EMSCRIPTEN_KEEPALIVE const Telemetry *dsp_telemetry() {
    synth.telemetry(telemetry);
    return &telemetry;
}

EMSCRIPTEN_KEEPALIVE int dsp_telemetry_size() {
    return static_cast<int>(sizeof(Telemetry));
}

} // extern "C"
