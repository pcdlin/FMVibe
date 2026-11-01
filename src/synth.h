#pragma once

#include "effects.h"
#include "envelope.h"
#include "filter.h"
#include "noise.h"
#include "oversampler.h"
#include "params.h"
#include "wavetable.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace fmvibe {

// Identifies a note the way the host sent it, so the plugin can report its end.
struct NoteRef {
    int32_t noteId;
    int16_t port;
    int16_t channel;
    int16_t key;

    bool operator==(const NoteRef &) const = default;
};

// An LFO's output (-1..1) at `phase` (0..1), before any unipolar shift. Random holds `held`
// (see randomGlide() for its smoothing); Custom needs its points, see shapeValue().
double lfoValue(LfoShape shape, double phase, double held);
// The Random shape at `phase` of a cycle that goes from `from` (the last cycle's value) to `to`:
// it glides for `smooth` (0..1) of the cycle, easing in and out, then holds.
double randomGlide(double from, double to, double phase, double smooth);
// The Random shape's next value after `last`: a step of up to 0.1 x 20^chaos either way,
// folded back into -1..1, so at full chaos (a step of up to 2) it lands anywhere.
double randomStep(double last, double chaos, uint32_t &rng);

// What the editor animates: the most recently started voice that is still sounding.
// The output meter: how fast its peaks fall, in dB a second, and how long its clip light stays on after the output
// went past full scale, in seconds.
constexpr double kMeterFallDb = 24.0;
constexpr double kClipHold = 1.5;

struct Telemetry {
    std::array<float, kModulatedCount> modOffset{}; // LFO/envelope modulation, in plain units
    std::array<float, kMaxSources> sources{};      // LFO outputs (-1..1), envelope levels (0..1)
    std::array<float, kMaxSources> lfoPhases{};    // 0..1
    std::array<float, kMaxSources> lfoRates{};     // cycles per second, to move the phases on between snapshots
    std::array<float, kMaxSources> lfoFrom{};      // a Random LFO's glide this cycle: the value it leaves,
    std::array<float, kMaxSources> lfoTo{};        // and the one it heads for (-1..1, before Unipolar)
    float notePitch = 0.0f;                     // its pitch, what the note source follows: semitones, 69 is A4
    float fxPitch = 60.0f; // what the effects' key tracking follows: that pitch, kept once the voice has ended
    float age = 0.0f;                           // seconds since the audio thread took this snapshot
    std::array<float, kOps> opLevels{};         // operator envelope x amplitude
    std::array<std::array<float, kMcBands>, kFxSlots> fxReduction{}; // compressor gain reduction, dB
    std::array<Spectrum, kFxSlots> fxSpectrum{}; // parametric EQs' and filters' output, see Effects::spectrum()
    std::array<Spectrum, kFilters> filterSpectrum{}; // each voice filter's output, all voices together
    std::array<float, kFxSlots> fxPhases{}; // phasers' and flangers' sweeps, see Effects::sweepPhase()
    std::array<float, 2> outPeak{}; // the output's peak level per side (1 is full scale), falling kMeterFallDb a second
    float clipHold = 0.0f;          // seconds the clip light stays on, since the output last went past full scale
    int voices = 0;
    float tempo = 120.0f; // host tempo, for showing synced times
    std::array<uint32_t, 4> keys{}; // keys held down, one bit per MIDI key (key 0 is bit 0 of keys[0])

    bool keyDown(int key) const { return key >= 0 && key < 128 && (keys[key / 32] >> (key % 32) & 1u) != 0; }
};

// Six-operator FM voices with a full modulation matrix and per-operator
// unison, three per-voice stereo filters the operators can be routed through, and
// up to kMaxSources per-voice LFOs and mod envelopes, and the note source, whose slots can target any
// continuous param.
// The voices' sum runs through the effects. Knows nothing about CLAP: the plugin feeds it notes and param values and
// renders between events. Audio thread only, apart from construction and prepare().
class Synth {
public:
    Synth();

    // Allocates `capacity` voices that run at `oversampling` (1, 2, 4 or 8)
    // times the host rate. Main thread, while the plugin is inactive.
    void prepare(double sampleRate, int capacity, int oversampling = 1);
    void reset();
    int capacity() const { return capacity_; }
    int oversampling() const { return oversampling_; }
    // How many of the allocated voices may sound at once.
    void setVoiceLimit(int limit) { limit_ = std::clamp(limit, 1, std::max(capacity(), 1)); }
    // Mono plays one voice that moves from note to note; legato (mono only) keeps
    // its envelopes running between overlapping notes. Glide happens on every note
    // with glideAlways, otherwise only between overlapping ones; its time is a param.
    void setPlayMode(bool mono, bool legato, bool glideAlways) {
        mono_ = mono;
        legato_ = legato;
        glideAlways_ = glideAlways;
    }

    void noteOn(const NoteRef &note, double velocity);
    // -1 in any field of `match` is a wildcard, as in CLAP note events.
    void noteOff(const NoteRef &match);
    void choke(const NoteRef &match);
    void noteExpression(const NoteRef &match, int32_t expression, double value);
    // Host modulation: an offset in plain units on param `index`, for every voice
    // when `match` is all wildcards, otherwise for the matching voices only.
    void paramMod(int index, const NoteRef &match, double amount);
    // -1..1, scaled by the Bend Range param.
    void pitchBend(double amount) { bend_ = amount; }
    // The editor's bend wheel, on top of the host's pitch bend. Audio thread.
    void setWheelBend(double amount) { wheelBend_ = amount; }
    // Host tempo in BPM, for synced LFOs and delay times.
    void setTempo(double bpm) {
        if (std::isfinite(bpm) && bpm > 0.0)
            tempo_ = std::clamp(bpm, 10.0, 999.0);
    }

    void render(float *left, float *right, uint32_t frames, const ParamValues &params);

    bool isActive(const NoteRef &note) const;
    int activeVoiceCount() const;
    // Effects still sounding after the voices ended (delay echoes).
    bool effectsRinging() const { return effects_.ringing(); }
    // Whether the voice filters, parametric EQs and filter effects measure their output for the editor.
    void setAnalyzing(bool on) {
        analyzing_ = on;
        effects_.setAnalyzing(on);
    }
    void telemetry(Telemetry &out) const;

    // Notes whose voice went silent since the last clearEndedNotes().
    std::span<const NoteRef> endedNotes() const { return ended_; }
    void clearEndedNotes() { ended_.clear(); }

private:
    struct Operator {
        std::array<double, kMaxUnison> phases{}; // in cycles, one per unison copy
        double amp = 0.0;     // velocity, ramped per block
        double out = 0.0;     // latest mono output sample (what other operators hear)
        double prevOut = 0.0; // the one before, for feedback smoothing
        Envelope env;
        std::array<NoiseColor::State, 2> noise{}; // the Noise wave's color filters: middle and sides
    };

    struct Lfo {
        double phase = 0.0;
        double hz = 0.0;   // the rate it last ran at
        double held = 0.0; // the Random shape's value for this cycle
        double from = 0.0; // and for the last one, which it glides from
    };

    // One LFO/envelope slot's contribution to a param, in plain units.
    struct SlotMod {
        int index;
        double offset;
    };

    // What renderBlock() ramps sample by sample toward each block's values.
    struct Levels {
        std::array<double, kOps> out{};
        std::array<std::array<double, kFilters>, kOps> send{};
        std::array<std::array<double, kOps>, kOps> index{};        // [from][to], phase offset in cycles per unit of output
        std::array<std::array<double, kOps>, kFilters> filterIndex{}; // [filter][to]
        std::array<std::array<double, kFilters>, kFilters> link{};  // [from][to]
        std::array<double, kFilters> filterOut{};
        std::array<std::array<double, kShapers>, kOps> shaperSend{};      // [op][shaper]
        std::array<std::array<double, kOps>, kShapers> shaperIndex{};     // [shaper][to], like index
        std::array<std::array<double, kFilters>, kShapers> shaperLink{};  // [shaper][filter]
        std::array<double, kShapers> shaperDirect{}; // straight to the output
        std::array<double, kShapers> shaperDrive{};  // gains
        std::array<double, kShapers> shaperOutput{};
        std::array<double, kShapers> shaperMix{};
    };

    struct Voice {
        bool active = false;
        bool released = false;
        bool fading = false;    // stolen: fades out over a few milliseconds, then ends; its note has ended
        double fade = 1.0;
        bool fresh = false;     // just started: operator phases are set in its first block
        NoteRef note{};
        double velocity = 0.0;
        uint64_t order = 0;     // note-on sequence number, for stealing the oldest
        double peak = 0.0;      // loudest audible envelope, for stealing the quietest
        double tuning = 0.0;    // note expressions
        double exprGain = 1.0;
        double exprPan = 0.0;
        double fmScale = 0.0;   // ramped, like the output gains and levels
        Levels levels{};
        double gainL = 0.0;
        double gainR = 0.0;
        uint32_t rng = 1;
        double glide = 0.0;     // current pitch in semitones, sliding toward note.key
        double pitch = 0.0;     // what it played at in its last block, bend and tuning included: the note source's
        std::array<Operator, kOps> ops{};
        // Each source place has both, for whichever type it holds.
        std::array<Lfo, kMaxSources> lfos{};
        std::array<Envelope, kMaxSources> modEnvs{};
        std::array<std::array<Filter, 2>, kFilters> filters{}; // [filter][channel]
        std::array<double, kFilters> filterOut{};               // latest mono filter output (what operators hear)
        std::array<std::array<ClipState, 2>, kShapers> shapers{}; // [shaper][channel]
        std::array<double, kShapers> shaperOut{};               // latest mono shaper output (what operators hear)
        std::array<double, kMaxSources> sourceValues{};
        // Slot modulation of the latest block. Sources read their own params (rates,
        // times, amounts) with these applied, so LFO 1 can modulate LFO 2's rate
        // one block later. The note source's come last.
        std::array<SlotMod, (kMaxSources + 1) * kSlots> slotMods{};
        int slotModCount = 0;
        std::array<double, kVoiceParamCount> mod{}; // per-voice host modulation
    };

    struct HeldNote {
        NoteRef note;
        double velocity;
    };

    Voice &allocateVoice();
    // Sounding and still playing its note: not ended, not fading out.
    static bool live(const Voice &v) { return v.active && !v.fading; }
    Voice *newestVoice();
    Voice *voicePlaying(const NoteRef &note);
    bool voiceHas(const NoteRef &note) const;
    void moveVoice(Voice &v, const HeldNote &to, bool retrigger, bool glide);
    void releaseVoice(Voice &v);
    void endVoice(Voice &v);
    void reportEnded(const NoteRef &note);
    // Adds the voice to `left` and `right`, which run at the voices' rate: hostFrames * oversampling_ samples, and
    // while analyzing, what its filters put out to filterTaps_.
    void renderBlock(Voice &v, double *left, double *right, uint32_t hostFrames, const ParamValues &params);
    // Brings the filters' block down to the host rate and measures it.
    void analyzeFilters(uint32_t hostFrames, bool sounding, const ParamValues &params);
    // One-shot, it stops at the end of the cycle instead of starting the next. Chaos is the Random shape's.
    void advanceLfo(Lfo &lfo, double hz, uint32_t frames, uint32_t &rng, bool oneShot, double chaos) const;
    void startFade(Voice &v);
    float *combLine(const Voice &v, int filter, int channel);

    const ParamTable &table_;
    const WaveTables &waves_;
    std::array<CustomWave, kOps> customWaves_; // each operator's Custom wave, built from its partials as they change
    // Each shaper's drawn curve as a table, built again when its points change (see render()).
    std::array<ClipTable, kShapers> shaperTables_;
    NoiseColor noiseColor_;
    double sampleRate_ = 48000.0;
    int oversampling_ = 1;
    double rate_ = 48000.0; // the voices' rate: sampleRate_ * oversampling_
    int capacity_ = 0;      // voices that may play; voices_ has a few more for stolen ones to fade out in
    double bend_ = 0.0;
    double wheelBend_ = 0.0;
    double tempo_ = 120.0;
    uint64_t noteCounter_ = 0;
    int limit_ = 1;
    bool mono_ = false;
    bool legato_ = true;
    bool glideAlways_ = false;
    std::vector<HeldNote> held_; // keys down, oldest first; reserved in prepare()
    double lastKey_ = -1.0;      // where the next glide starts when no voice is sounding
    ParamValues globalMod_{};
    // What the macros and the mod wheel add to their targets, for every voice and the effects alike, worked out at
    // the start of each render; the targets touched last time, so they can be cleared again.
    std::vector<double> macroMod_ = std::vector<double>(kModulatedCount, 0.0); // on the heap: the synth is big enough
    std::array<int, (kMacros + 1) * kSlots> macroTargets_{}; // the macros' and the mod wheel's
    int macroTargetCount_ = 0;
    void applyMacros(const ParamValues &params);
    // The clocks free LFOs follow instead of their own: they run at the newest
    // voice's rate, like the effects, and keep going between notes.
    std::array<Lfo, kMaxSources> freeLfos_{};
    uint32_t freeRng_ = 1;
    std::vector<Voice> voices_;
    // The voices' sum at their rate, one control block long, and what brings it down to the host rate.
    std::array<std::vector<double>, 2> mix_;
    std::array<Decimator, 2> decimators_;
    // Comb delay lines per voice, filter and channel, kept out of Voice so voices stay small.
    std::vector<float> combs_;
    int combSize_ = 0;
    // While the editor is open, what each filter puts out, mono and summed over the voices: a control block of it at
    // the voices' rate, the way down to the host rate, and the analyzer that measures it there.
    bool analyzing_ = false;
    std::array<std::vector<double>, kFilters> filterTaps_;
    std::array<Decimator, kFilters> filterDecimators_;
    std::array<Analyzed, kFilters> filterAnalyzed_;
    Analyzer analyzer_;
    Effects effects_;
    ParamValues fxParams_{}; // effect params with the newest voice's modulation, per control block
    double fxPitch_ = 60.0;  // the newest voice's pitch, for the effects' key tracking; kept once it has ended
    // The output meter's peaks and clip light (see Telemetry), taken off what render() puts out.
    std::array<double, 2> outPeak_{};
    double clipHold_ = 0.0;
    void meterOutput(const float *left, const float *right, uint32_t frames);
    std::vector<NoteRef> ended_; // reserved in prepare(), never grows on the audio thread
};

} // namespace fmvibe
