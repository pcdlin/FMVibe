// The browser demo's audio side: the synth behind a few C functions, which web/site/worklet.js calls from the
// AudioWorklet. It does what Plugin::process() and Plugin::activate() do for a host, with the editor's
// edits and notes arriving as messages instead of CLAP events. Built as a standalone wasm module, with no
// Emscripten JS around it, since that doesn't run in a worklet.

#include "opdata.h"
#include "params.h"
#include "synth.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace fmvibe;

namespace {

constexpr int kMaxFrames = 4096; // a render quantum is 128 frames, but browsers may pick others

Synth synth;
ParamValues values{};
float left[kMaxFrames];
float right[kMaxFrames];
Telemetry telemetry{};
double sampleRate = 48000.0;
int oversampling = 2;

int voiceLimit() {
    return std::clamp(static_cast<int>(std::lround(values[paramIndex(GlobalParam::Voices)])), 1, kMaxVoices);
}

// The Wavetable and Sample operators' data, by registry entry: what the editor sent over (see dsp_data()), and the
// factory tables the synth found missing, made here. Everything here runs on the worklet's one thread, between
// blocks, so data let go of can be freed at once.
std::array<std::shared_ptr<const OpData>, DataRegistry::kEntries> held;
std::vector<float> incoming;

void registerData(uint32_t key, std::shared_ptr<const OpData> data) {
    DataRegistry &registry = synth.registry();
    int free = -1;
    for (int i = 0; i < DataRegistry::kEntries; ++i) {
        if (held[i] && held[i]->key == key) { // in place of what it held
            registry.set(i, key, data.get());
            held[i] = std::move(data);
            return;
        }
        if (!held[i] && free < 0)
            free = i;
    }
    if (free < 0)
        return;
    registry.set(free, key, data.get());
    held[free] = std::move(data);
}

void releaseData(uint32_t key) {
    for (int i = 0; i < DataRegistry::kEntries; ++i)
        if (held[i] && held[i]->key == key) {
            synth.registry().clear(i);
            held[i].reset();
        }
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

// Notes from the editor's keyboard and the computer's, like MIDI notes without a note id, at a MIDI velocity.
EMSCRIPTEN_KEEPALIVE void dsp_note(int key, int on, int velocity) {
    const NoteRef note{-1, 0, 0, static_cast<int16_t>(std::clamp(key, 0, 127))};
    if (on)
        synth.noteOn(note, std::clamp(velocity, 1, 127) / 127.0);
    else
        synth.noteOff(note);
}

// The page's tempo (BPM), standing in for a host's transport: synced LFOs, delays and sweeps follow it.
EMSCRIPTEN_KEEPALIVE void dsp_tempo(double bpm) {
    synth.setTempo(bpm);
}

EMSCRIPTEN_KEEPALIVE void dsp_scope(int op, int cycles) {
    synth.setScope(op, cycles);
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
    synth.setAnalyzing(true); // the editor is always open
    synth.setVoiceLimit(voiceLimit());
    applyPlayMode();
    synth.render(left, right, static_cast<uint32_t>(frames), values);
    synth.clearEndedNotes(); // nobody to report them to
    // A factory table a Wavetable operator found missing is made now, between blocks (a block may drop out meanwhile:
    // the demo has no other thread); the editor's own data comes over with dsp_data().
    for (const uint32_t key : synth.registry().takeMissing()) {
        if (key < kFactoryTables)
            registerData(key, makeFactoryTable(static_cast<int>(key)));
        else
            synth.registry().ignore(key); // until the editor sends it
    }
}

// A Wavetable or Sample operator's data from the editor, as the patch stores it: its samples go into the buffer this
// gives first, then dsp_data() builds it (here, between blocks) and hands it to the synth under its key.
EMSCRIPTEN_KEEPALIVE float *dsp_data_buffer(int count) {
    incoming.assign(static_cast<size_t>(std::max(count, 0)), 0.0f);
    return incoming.data();
}

EMSCRIPTEN_KEEPALIVE void dsp_data(unsigned key, int kind, unsigned rate, int frames, double root) {
    auto data = std::make_shared<OpData>();
    data->key = key;
    data->kind = kind == static_cast<int>(DataKind::Sample) ? DataKind::Sample : DataKind::Table;
    data->rate = rate;
    data->frames = frames;
    data->root = static_cast<float>(root);
    data->pcm = std::move(incoming);
    incoming.clear();
    buildOpData(*data);
    registerData(key, std::move(data));
}

EMSCRIPTEN_KEEPALIVE void dsp_data_release(unsigned key) {
    releaseData(key);
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
