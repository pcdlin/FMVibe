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
#include <bitset>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace fmvibe {

// What `make()` gave for the inputs it was last called with, made again only when they change, bit for bit: for
// settings a voice works out from its params each block, whose exp, pow and tan calls cost more than looking.
template <class T, size_t N>
class Memo {
public:
    template <class Make>
    const T &get(const std::array<double, N> &inputs, const Make &make) {
        if (std::memcmp(inputs.data(), inputs_.data(), sizeof inputs) != 0) {
            inputs_ = inputs;
            value_ = make();
        }
        return value_;
    }
    const T &last() const { return value_; } // what the last get() gave

private:
    std::array<double, N> inputs_ = [] { // none yet: params hold numbers, so the first get() makes it
        std::array<double, N> none;
        none.fill(std::numeric_limits<double>::quiet_NaN());
        return none;
    }();
    T value_{};
};

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
// The scope: an operator's output over up to kScopeMaxCycles of its cycles, kScopeBinsPerCycle points each.
constexpr int kScopeBinsPerCycle = 256;
constexpr int kScopeMaxCycles = 4;
constexpr int kScopeBins = kScopeBinsPerCycle * kScopeMaxCycles;
// A span of cycles that lasts longer than this, in seconds, shows as it fills, up to a cursor.
constexpr double kScopeSlowSpan = 0.2;
// The header's output scope: the output (the mean of its two sides, after Volume) over kOutScopePeriods periods of
// the newest note's pitch (what the effects' key tracking follows), kOutScopeBins points across. Its spans follow one
// another at that pitch, so whatever the wave, a sound that repeats with the note stands still; a new note starts
// them over. A high note's span holds a whole multiple of kOutScopePeriods, lasting at least kOutScopeMinSpan seconds.
constexpr int kOutScopeBins = 128;
constexpr int kOutScopePeriods = 2;
constexpr double kOutScopeMinSpan = 0.0005;
constexpr int kOutScopeTaps = 16; // the samples each point is interpolated from (see Synth::OutScopeCapture)

struct Telemetry {
    std::array<float, kModulatedCount> modOffset{}; // LFO/envelope modulation, in plain units
    // What each slot (source * kSlots + slot) adds to its target in the newest voice, as a share of the target's
    // range (what its depth bar shows); 0 for the slots no voice runs: the macros' and the mod wheel's, which the
    // editor works out itself.
    std::array<float, kAllSources * kSlots> slotOffset{};
    std::array<float, kMaxSources> sources{};      // LFO outputs (-1..1), envelope levels (0..1)
    // Where modulation takes each macro's knob, against its set value (with the host's modulation for every voice):
    // the newest voice's, or with none what every voice shares (the macros' and the mod wheel's slots on it).
    std::array<float, kMacros> macroOffset{};
    std::array<float, kMaxSources> lfoPhases{};    // 0..1
    std::array<float, kMaxSources> lfoRates{};     // cycles per second, to move the phases on between snapshots
    std::array<float, kMaxSources> lfoFrom{};      // a Random LFO's glide this cycle: the value it leaves,
    std::array<float, kMaxSources> lfoTo{};        // and the one it heads for (-1..1, before Unipolar)
    float notePitch = 0.0f;                     // its pitch, what the note source follows: semitones, 69 is A4
    float noteVelocity = 0.0f;                  // its velocity, what the velocity source follows: 0..1
    // Where the pitch bend stands, -1..1: what the pitch bend source puts out (the MIDI bend, the Pitch Bend param
    // with the host's modulation, and in the newest voice its own and its MPE channel's; with no voice, the MIDI bend
    // and the param with the host's modulation for every voice). And the Pitch Bend param as set, which that includes:
    // the bend wheel shows the param as the editor has it, with the rest on top.
    float bend = 0.0f;
    float bendSet = 0.0f;
    float fxPitch = 60.0f; // what the effects' key tracking follows: that pitch, kept once the voice has ended
    float age = 0.0f;                           // seconds since the audio thread took this snapshot
    std::array<float, kOps> opLevels{};         // operator envelope (the editor scales it by the Out)
    std::array<float, kOps> opEnvLevels{};      // operator envelope alone (0..1)
    // Where each operator's and mod envelope is, see Envelope::position(); -1 idle (or not an envelope).
    std::array<float, kOps> opEnvPos{};
    std::array<float, kMaxSources> envPos{};
    std::array<std::array<float, kMcBands>, kFxSlots> fxReduction{}; // compressor gain reduction, dB
    // Three-band dynamics' bands: their detectors' levels (1 is full scale) and their gains (dB), see
    // Effects::bandLevel().
    std::array<std::array<float, kMcBands>, kFxSlots> fxBandLevel{};
    std::array<std::array<float, kMcBands>, kFxSlots> fxBandGain{};
    // Parametric EQs', filters' and phase dispersers' output, see Effects::spectrum().
    std::array<Spectrum, kFxSlots> fxSpectrum{};
    std::array<Spectrum, kFilters> filterSpectrum{}; // each voice filter's output, all voices together
    std::array<float, kFxSlots> fxPhases{}; // phasers' and flangers' sweeps, see Effects::sweepPhase()
    std::array<float, kFxPorts> fxPortLevels{}; // how loud each port of the effects graph is, see Effects::portLevel()
    std::array<float, 2> outPeak{}; // the output's peak level per side (1 is full scale), falling kMeterFallDb a second
    float clipHold = 0.0f;          // seconds the clip light stays on, since the output last went past full scale
    int voices = 0;
    // With no voice left, whether the last one's modulation of the effects runs on through their tail (see
    // Synth::render()): modOffset and slotOffset then hold what it adds to the effects' params and Volume.
    bool fxTail = false;
    uint32_t fxTailLfos = 0; // and the LFOs it runs (a bit a place), whose lfoPhases, lfoRates, lfoFrom and lfoTo are its
    float tempo = 120.0f; // host tempo, for showing synced times
    std::array<uint32_t, 4> keys{}; // keys held down, one bit per MIDI key (key 0 is bit 0 of keys[0])
    // The scope's operator in that voice (see Synth::setScope()): its output (unison copies together, as other
    // operators hear it) at evenly spaced points of its phase, the first scopeBins of them; 0 while nothing is
    // captured. A slow operator's span shows while it fills: scopeCursor is how far (0..1), -1 for a whole span.
    std::array<float, kScopeBins> scope{};
    int scopeBins = 0;
    float scopeCursor = -1.0f;
    // The output scope's last whole span (see kOutScopeBins), as the output's level: all 0 while the synth doesn't
    // analyze (see Synth::setAnalyzing()).
    std::array<float, kOutScopeBins> outScope{};

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
    // How many of the allocated voices may sound at once. Lowered, it takes effect at once: the voices past it fade
    // out as stolen ones do.
    void setVoiceLimit(int limit);
    // Mono plays one voice that moves from note to note; legato keeps its envelopes
    // running between overlapping notes (in poly, a new note joins the envelopes of the
    // newest key held; see noteOn()). Glide happens on every note with glideAlways,
    // otherwise only between overlapping ones; its time is a param.
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
    // when `match` is all wildcards, otherwise for the matching voices only. Only params flagged modulatable take
    // it, and those after the voices' (the effects, the macros, the note source's and the mod wheel's) only for
    // every voice.
    void paramMod(int index, const NoteRef &match, double amount);
    // MIDI that isn't notes, on `channel` (0..15). With MPE on (see setMpe()), what comes on a member channel (1..15)
    // is the note's there alone: its pitch bend (-1..1 of the MPE bend range), its channel pressure and its CC 74
    // (slide), each kept for the channel, so a note that starts there later starts from them. Otherwise, a pitch bend
    // (-1..1, scaled by the Bend Range param) bends every voice, a controller's value is what the Midi sources follow
    // and the channel's pressure what they fall back on while a note has none of its own (see SourceType).
    // Polyphonic aftertouch is the pressure of the voices playing `match`, as CLAP's pressure expression is.
    // Three controllers act on notes too (and are still followed like the others): CC 64, the sustain pedal (see
    // setPedal()), CC 120, All Sound Off (see allSoundOff()) and CC 123, All Notes Off (see allNotesOff()).
    void pitchBend(int channel, double amount);
    void controlChange(int channel, int controller, double value);
    void channelPressure(int channel, double value);
    void polyPressure(const NoteRef &match, double value) {
        noteExpression(match, CLAP_NOTE_EXPRESSION_PRESSURE, value);
    }
    // MPE over MIDI (MIDI Polyphonic Expression), a setting of the instance rather than of the patch (see Settings in
    // state.h): while on, channel 1 is the master channel, whose bend, controllers and pressure apply to every voice,
    // and channels 2 to 16 each carry a note of their own, whose pitch bend (across `bendRange` semitones), channel
    // pressure and CC 74 are that note's alone (see pitchBend()). Off, every channel's apply to every voice, and the
    // member channels' bends are forgotten. CLAP's note expressions are per note either way.
    void setMpe(bool on, double bendRange);
    // The sustain pedal on `channel`: while it is down, a key let go leaves its note sounding as if still held, until
    // the pedal comes up. In mono, a key let go still hands the voice back to an earlier key held down, as without the
    // pedal; with none down, the pedal holds the note. A note the pedal holds counts as held for legato and glide:
    // a new note glides from it, a mono one carries on its envelopes, and in poly legato one joins it. A key struck
    // again while the pedal holds its note lets that note go into its release. Which notes a channel's pedal reaches
    // is as for All Notes Off below.
    void setPedal(int channel, bool down);
    // MIDI's panic messages, from `channel`: every note, or with MPE on, a member channel's own. All Notes Off lets
    // them go as their keys coming up would (the pedal, if down, holding them still, as MIDI has it); All Sound Off
    // ends them at once, fading out as a stolen voice does, and, for every note, has the effects' tails go too.
    void allNotesOff(int channel);
    void allSoundOff(int channel);
    // Host tempo in BPM, for synced LFOs and delay times.
    void setTempo(double bpm) {
        if (std::isfinite(bpm) && bpm > 0.0)
            tempo_ = std::clamp(bpm, 10.0, 999.0);
    }
    double tempo() const { return tempo_; }
    // The host's transport as the next render() starts: whether it plays, and where, in beats (quarter notes) and in
    // seconds from the song's start. Anchored LFOs stand where that puts them (see SourceParam); render() moves the
    // position on through its frames while the transport plays, so a block split by events keeps it. Without a call,
    // or while stopped, they run on as free ones. A position that isn't a number counts as stopped.
    void setTransport(bool playing, double beats, double seconds) {
        playing_ = playing && std::isfinite(beats) && std::isfinite(seconds);
        if (playing_) {
            songBeats_ = beats;
            songSeconds_ = seconds;
        }
    }

    // `sameParams`: the caller vouches that `params` are what the last render() had, so what follows from them alone
    // (see sharedStale_) isn't worked out again; the host's modulation since is taken in any case.
    void render(float *left, float *right, uint32_t frames, const ParamValues &params, bool sameParams = false);

    // Whether a voice still plays `note`'s key. A host without note ids keeps one voice per key, so a key struck again
    // keeps it alive; with note ids, an overlapping note on the same key is a voice of its own.
    bool isActive(const NoteRef &note) const;
    int activeVoiceCount() const;
    // Effects still sounding after the voices ended (delay echoes).
    bool effectsRinging() const { return effects_.ringing(); }
    // Whether the voice filters, parametric EQs and filter effects measure their output for the editor, as do the effects
    // graph's ports, and the output scope captures the output (see kOutScopeBins). Switched off, the scope forgets what it took.
    void setAnalyzing(bool on) {
        if (analyzing_ && !on)
            outScope_ = {};
        analyzing_ = on;
        effects_.setAnalyzing(on);
    }
    void telemetry(Telemetry &out) const;
    // Which operator's output the telemetry's scope captures from the newest voice (-1: none, and nothing is
    // done), over how many of its cycles (1 to kScopeMaxCycles).
    void setScope(int op, int cycles);

    // Notes whose voice went silent since the last clearEndedNotes().
    std::span<const NoteRef> endedNotes() const { return ended_; }
    void clearEndedNotes() { ended_.clear(); }

private:
    // An operator's unison copies: each one's phase increment, and apart from those, which follow its pitch, their
    // stereo gains, which follow only the spread and the number of copies (see renderBlock()).
    struct UnisonIncs {
        std::array<double, kMaxUnison> inc;
    };
    struct UnisonGains {
        std::array<double, kMaxUnison> gainL, gainR;
    };

    struct Operator {
        std::array<double, kMaxUnison> phases{}; // in cycles, one per unison copy
        double out = 0.0;     // latest mono output sample (what other operators hear)
        double prevOut = 0.0; // the one before, for feedback smoothing
        std::array<double, kMaxUnison> incNow{}; // the copies' phase increments, moving to each block's
        int incCopies = 0;     // how many copies incNow holds
        bool incKnown = false; // incNow holds the last block's, not stale: false for a new note and after being off
        Envelope env;
        Memo<EnvRates, 8> rates; // its envelope's, from its params
        Memo<UnisonIncs, 3> unisonIncs;   // its copies' increments, from its pitch, detune and copies
        Memo<UnisonGains, 2> unisonGains; // and their gains, from its spread and copies
        std::array<NoiseColor::State, 2> noise{}; // the Noise wave's color filters: middle and sides
        // Hard sync: each copy's master, a cycle of the note (as detuned as the copy), restarts the copy at the phase
        // the note started it at. A restart's step, and the bend in its slope, are smoothed over the samples either
        // side of it (polyBLEP and polyBLAMP): the one after's share waits in `syncCarry`.
        std::array<double, kMaxUnison> startPhases{}; // where the note started each copy
        std::array<double, kMaxUnison> syncPhases{};  // the masters', in cycles
        std::array<double, kMaxUnison> syncIncNow{};  // and their increments, moving to each block's
        std::array<double, kMaxUnison> syncCarry{};   // what the next sample adds, per copy
        bool syncOn = false;  // syncing: the masters run
        bool syncKnown = false; // syncIncNow holds the last block's
        int syncCopies = 0;     // for how many copies
        bool carrying = false;  // some copy's syncCarry isn't 0 yet
        // The warp it plays (see warp.h) and its amount, which goes to 0 before the warp changes to another one.
        OpWarp warp = OpWarp::Off;
        double warpNow = 0.0;
    };

    struct Lfo {
        double phase = 0.0;
        double hz = 0.0;   // the rate it last ran at
        double held = 0.0; // the Random shape's value for this cycle
        double from = 0.0; // and for the last one, which it glides from
        bool restart = false; // a note (re)started it: its next block moves it to its Phase
        double cycle = 0.0;   // an anchored one's cycle since the song's start, whole: Random steps as it changes
    };

    // One LFO/envelope slot's contribution to a param, in plain units.
    struct SlotMod {
        int index;
        double offset;
        int slot = -1; // whose: source * kSlots + slot
    };

    // What renderBlock() ramps sample by sample toward each block's values.
    struct Levels {
        std::array<double, kOps> out{};
        std::array<double, kOps> panL{}; // an operator's Pan as left and right gains, on all it puts out but its FM
        std::array<double, kOps> panR{};
        std::array<std::array<double, kFilters>, kOps> send{};
        std::array<std::array<double, kOps>, kOps> index{};        // [from][to], phase offset in cycles per unit of output
        std::array<std::array<double, kOps>, kFilters> filterIndex{}; // [filter][to]
        std::array<std::array<double, kFilters>, kFilters> link{};  // [from][to]
        std::array<double, kFilters> filterOut{};
        std::array<double, kFilters> filterMix{}; // how much of what a filter puts out is the filter, the rest its input
        std::array<std::array<double, kShapers>, kOps> shaperSend{};      // [op][shaper]
        std::array<std::array<double, kOps>, kShapers> shaperIndex{};     // [shaper][to], like index
        std::array<std::array<double, kFilters>, kShapers> shaperLink{};  // [shaper][filter]
        std::array<std::array<double, kShapers>, kFilters> filterShaper{}; // [filter][shaper]
        std::array<std::array<double, kShapers>, kShapers> shaperShaper{}; // [from][to], from == to into itself
        std::array<double, kShapers> shaperDirect{}; // straight to the output
        std::array<double, kShapers> shaperDrive{};  // gains
        std::array<double, kShapers> shaperOutput{};
        std::array<double, kShapers> shaperMix{};
    };

    struct Voice {
        bool active = false;
        bool released = false;
        bool sustained = false; // its key is up, but a sustain pedal holds it (see setPedal()); not released
        bool fading = false;    // stolen: fades out over a few milliseconds, then ends; its note has ended
        // Released, and its sound is over, but a mod envelope still releases: it runs its modulation only.
        bool silent = false;
        // Released, and its operators have faded: what the shapers get from the filters and the shapers, and the
        // filters from themselves and later filters, ramps away, so no loop of filters and shapers feeding each other
        // (or one feeding itself) can ring on for ever.
        bool tail = false;
        double fade = 1.0;
        bool fresh = false;     // just started: operator phases are set in its first block
        bool keepLfos = false;  // joined a held note in poly legato: its LFOs carry on as copied, taking no first step
        NoteRef note{};
        double velocity = 0.0;
        uint64_t order = 0;     // note-on sequence number, for stealing the oldest
        double peak = 0.0;      // loudest audible envelope, for stealing the quietest
        double tuning = 0.0;    // note expressions
        double exprGain = 1.0;
        double exprPan = 0.0;
        double pressure = -1.0; // its note's aftertouch, 0..1, or -1 while none came: the channel's stands in
        double slide = 0.5;     // its note's slide, 0..1, resting in the middle
        bool newRandom = false; // (re)started: its random LFOs take a new value in its next block
        std::array<double, kMaxSources> follow{}; // what Midi sources put out, 0..1
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
        std::array<Memo<EnvRates, 8>, kMaxSources> modEnvRates{};
        std::array<Memo<FilterSettings, 7>, kFilters> filterSettings{};
        std::array<Memo<ShaperSettings, 6>, kShapers> shaperSettings{};
        std::array<std::array<Filter, 2>, kFilters> filters{}; // [filter][channel]
        // Samples (at the oversampled rate) a filter whose inputs have stopped runs on for, ringing out: a comb's
        // line's length past the last time it put out more than kFilterRingFloor (see renderBlock()).
        std::array<int, kFilters> filterRing{};
        // Samples (at the oversampled rate) a released voice is kept for after a filter last put out more than the
        // voice's quiet floor: a comb's line's length, so the gaps between a plucked comb's pulses don't end it.
        std::array<int, kFilters> filterHeard{};
        std::array<double, kFilters> filterOut{};               // latest mono filter output (what operators hear)
        std::array<std::array<double, 2>, kFilters> filterLast{}; // latest filter output per channel (what shapers and loops hear)
        std::array<std::array<ClipState, 2>, kShapers> shapers{}; // [shaper][channel]
        std::array<double, kShapers> shaperOut{};               // latest mono shaper output (what operators hear)
        std::array<std::array<double, 2>, kShapers> shaperLast{}; // latest shaper output per channel (what shapers hear)
        // Channel 1 of the filters and shapers stood still through a block without stereo spread: it takes channel 0's
        // state before it runs again, rather than carrying on from where it stopped (a comb would ring again).
        bool rightStale = false;
        // The comb lines (see combLine()) that still hold an earlier note's sound: a comb clears them before it first
        // reads them, rather than every note clearing them, comb or not.
        std::array<std::array<bool, 2>, kFilters> combDirty{};
        std::array<double, kMaxSources> sourceValues{};
        // Slot modulation of the latest block. Sources read their own params (rates,
        // times, amounts) with these applied, so LFO 1 can modulate LFO 2's rate
        // one block later. Velocity's and the pitch bend's come after the places', then the note source's on the
        // macros' knobs and what the voice's own knobs make of the macros' and the mod wheel's slots, then those an aux
        // scales per voice, and the note source's others last.
        std::array<SlotMod, kAllSources * kSlots> slotMods{};
        // What each source that runs per voice puts out, 0..1 as a unipolar one would, for the slots it scales as an
        // aux: as of this block for the sources before them and velocity (and on a note's first block, the note
        // source), else the last block's. And each macro's knob as the voice has it: as every voice shares it, or for
        // a macro its sources move (see MacroGraph::voiceMoved), as they took it this block, after the places,
        // velocity and the pitch bend (the slots before that taking the last block's).
        std::array<double, kAllSources> auxOut{};
        int slotModCount = 0;
        std::array<double, kVoiceParamCount> mod{}; // per-voice host modulation
        // The params whose host modulation was set since the note started (to 0 too), each once: the others' is 0.
        std::array<uint16_t, kVoiceParamCount> modded{};
        int moddedCount = 0;
        std::bitset<kVoiceParamCount> modSet;
    };

    struct HeldNote {
        NoteRef note;
        double velocity;
    };

    // Sets a voice's host modulation of param `index`, and forgets all of it.
    static void setVoiceMod(Voice &v, int index, double amount);
    static void clearVoiceMods(Voice &v);
    Voice &allocateVoice();
    // The voice to steal at the limit: one already released if possible, then the quietest, then the oldest.
    Voice *stealable();
    // Sounding and still playing its note: not ended, not fading out.
    static bool live(const Voice &v) { return v.active && !v.fading; }
    Voice *newestVoice();
    Voice *voicePlaying(const NoteRef &note);
    bool voiceHas(const NoteRef &note) const;
    void moveVoice(Voice &v, const HeldNote &to, bool retrigger, bool glide);
    void releaseVoice(Voice &v);
    // A voice whose key is let go: the pedal holds it, or it releases.
    void letGo(Voice &v);
    // Whether a sustain pedal holding `v`'s note is down (see setPedal()).
    bool pedalHolds(const Voice &v) const;
    // Releases the voices the pedal held that no pedal down holds any more.
    void releaseUnpedaled();
    // The notes a message on `channel` reaches (see allNotesOff()), as a match: a member channel's with MPE on, else
    // every note.
    NoteRef channelMatch(int channel) const;
    void endVoice(Voice &v);
    void reportEnded(const NoteRef &note);
    // Adds the voice to `left` and `right`, which run at the voices' rate: hostFrames * oversampling_ samples, and
    // while analyzing, what its filters put out to filterTaps_.
    void renderBlock(Voice &v, double *left, double *right, uint32_t hostFrames, const ParamValues &params);
    // A synced or warped operator's sample (see renderBlock()): its copies' sum and, with `sides`, their left and
    // right. Kept out of the voices' loop: inside it, MSVC's code made the voices a twenty-fifth slower with one copy
    // when no operator was synced or warped.
    struct OpSample {
        double mono = 0.0, l = 0.0, r = 0.0;
    };
    static OpSample warpedSample(Operator &op, const float *table, double offset, double factor, int copies,
                                 const UnisonGains *gains, bool sides, const std::array<double, kMaxUnison> &syncIncStep,
                                 double warpStep);
    // Brings the filters' block down to the host rate and measures it.
    void analyzeFilters(uint32_t hostFrames, bool sounding, const ParamValues &params);
    // One-shot, it stops at the end of the cycle instead of starting the next. Chaos is the Random shape's.
    void advanceLfo(Lfo &lfo, double hz, uint32_t frames, uint32_t &rng, bool oneShot, double chaos) const;
    // Moves the free and anchored LFOs' shared clocks on by `frames`, which end `end` frames into this render(); with
    // no frames, only the anchored ones go to where the transport stands `end` frames in. Their params are what is
    // set with the host's, the macros' and the newest voice's modulation.
    void runSharedLfos(const ParamValues &params, const Voice *newest, uint32_t frames, uint32_t end);
    void startFade(Voice &v);
    float *combLine(const Voice &v, int filter, int channel);

    const ParamTable &table_;
    const WaveTables &waves_;
    std::array<CustomWave, kOps> customWaves_; // each operator's Custom wave, built from its partials as they change
    // How each route into an operator acts on it (see RouteMode), [from node][to operator], read once a render: these
    // params are stepped, past the ones voices read, and feedback is always Phase.
    std::array<std::array<RouteMode, kOps>, kRouteSources> routeModes_{};
    // Each shaper's drawn curve as a table, built again when its points change (see render()).
    std::array<ClipTable, kShapers> shaperTables_;
    NoiseColor noiseColor_;
    double sampleRate_ = 48000.0;
    int oversampling_ = 1;
    double rate_ = 48000.0; // the voices' rate: sampleRate_ * oversampling_
    int capacity_ = 0;      // voices that may play; voices_ has as many more (4 to 32) for stolen ones to fade out in
    double bend_ = 0.0;
    double bendSet_ = 0.0; // the Pitch Bend param as the last render() had it (see Telemetry::bendSet)
    std::array<double, 128> cc_{}; // the MIDI controllers' values, 0..1
    double channelPressure_ = 0.0;
    // MPE (see setMpe()): whether it is on, its bend range, and each member channel's bend (-1..1), pressure (0..1, or
    // -1 while none came, the master channel's standing in) and slide (0..1), by channel; channel 0, the master, is
    // unused.
    void startFromChannel(Voice &v) const; // with MPE on, a note on a member channel takes its channel's
    bool mpe_ = false;
    double mpeBendRange_ = 48.0;
    std::array<double, 16> channelBend_{};
    std::array<double, 16> channelPressures_{}; // filled with -1 by reset()
    std::array<double, 16> channelSlides_{};
    // The sustain pedals (see setPedal()): every note's, and with MPE on, each member channel's for its own notes.
    bool pedal_ = false;
    std::array<bool, 16> channelPedals_{};
    double tempo_ = 120.0;
    bool playing_ = false;     // the host's transport, as of the start of the next render() (see setTransport())
    double songBeats_ = 0.0;   // where, in beats
    double songSeconds_ = 0.0; // and in seconds
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
    // The macros' and the mod wheel's slots that an aux running per voice scales, which the voices add (source, slot).
    std::array<std::pair<int, int>, (kMacros + 1) * kSlots> voiceMacroSlots_{};
    int voiceMacroSlotCount_ = 0;
    // How the macros' knobs modulate one another (see MacroGraph), and the knobs as set (with the host's modulation for
    // every voice) and as the macros' and the mod wheel's slots on them take them, which every voice shares.
    MacroGraph macroGraph_;
    MacroKnobs macroSet_{};
    MacroKnobs macroKnobs_{};
    // The macros' and the mod wheel's slots that every voice shares but whose source's knob, or aux's, a source of the
    // voice moves (see MacroGraph::voiceMoved): what is the same for every voice, and what they add there
    // (`shared`, in macroMod_ already), which each voice takes back for what its own knobs make of it.
    struct VoiceMovedSlot {
        int source;
        int slot;
        int target;
        int aux = -1; // a macro or the mod wheel, or none
        double curve = 0.0;
        double depth = 0.0; // the slot's depth times its Amount, in the target's units
        bool bipolar = false;
        double shared = 0.0;
    };
    std::array<VoiceMovedSlot, (kMacros + 1) * kSlots> voiceMovedSlots_{};
    int voiceMovedSlotCount_ = 0;
    bool voiceMovedMacro(int source) const {
        return isMacro(source) && (macroGraph_.voiceMoved >> knobOf(source) & 1u) != 0;
    }
    void applyMacros(const ParamValues &params);
    // A param as set with the host's modulation for every voice, in its range: how the macros, the mod wheel and the
    // note source read their knobs, depths and range.
    double hostValue(const ParamValues &params, int index) const {
        return std::clamp(params[index] + globalMod_[index], table_[index].min, table_[index].max);
    }
    // The params the voices read, with the host's modulation for every voice and the macros' added: as summed
    // (voiceBase_), and in their ranges (voiceClamped_). None of it changes during a render(), so it is worked out
    // once there, when the first voice needs it, and each voice works out again only the params its own modulation
    // moves (see renderBlock()). On the heap, like macroMod_.
    // Whether what render() works out from the params and the host's modulation for every voice may be out of date,
    // for want of a render() told its params changed (see render()), or with that modulation since.
    bool sharedStale_ = true;
    // Each source's slots in use, in order: those with a target that aren't bypassed, and their targets, which no
    // modulation moves. Worked out once a render() whose params changed, when the first voice needs them; the voices
    // look over these rather than every slot (an Amount of 0 they still leave out, as it takes modulation).
    // What else of a slot is the same for every voice, worked out there too: the curve, the depth's scale (its
    // Amount, times an aux that is a macro's or the mod wheel's knob), the aux a voice's own source puts out (or -1),
    // the target's range, and the slot's depth with the host's modulation for every voice (which velocity's and the
    // note source's slots take, having no voice modulation of their own).
    struct SharedSlot {
        int slot;
        int target;
        double curve = 0.0;
        double scale = 1.0;
        int voiceAux = -1;
        double range = 0.0;
        double depth = 0.0;
    };
    std::array<std::array<SharedSlot, kSlots>, kAllSources> sharedSlots_{};
    std::array<int, kAllSources> sharedSlotCount_{};
    bool sharedSlotsReady_ = false;
    void shareSlots(const ParamValues &params);
    std::vector<double> voiceBase_ = std::vector<double>(kVoiceParamCount, 0.0);
    std::vector<double> voiceClamped_ = std::vector<double>(kVoiceParamCount, 0.0);
    bool voiceBaseReady_ = false;
    void shareVoiceParams(const ParamValues &params);
    // renderBlock()'s params for the voice it renders, voiceClamped_ but for those listed as changed: the voice's own
    // host and slot modulation, which the next voice puts back. Each param is listed once for each time it was set: a
    // voice sets those its host modulates once, and in each of two passes over its slots at most the params they
    // moved the pass before and those they move now, then those the note source moves.
    std::vector<double> voiceParams_ = std::vector<double>(kVoiceParamCount, 0.0);
    std::vector<int> voiceParamsChanged_ = std::vector<int>(kVoiceParamCount + 4 * kAllSources * kSlots + kSlots, 0);
    int voiceParamsChangedCount_ = 0;
    // The modulated params' ranges, apart from the rest of their table entries, so loops over them read only these.
    std::vector<double> paramMin_ = std::vector<double>(kModulatedCount, 0.0);
    std::vector<double> paramMax_ = std::vector<double>(kModulatedCount, 0.0);
    // renderBlock()'s scratch for adding up the slots that modulate a param: each pass of it has a number, and a
    // param's sum is valid in the pass whose number it has.
    std::vector<double> slotSum_ = std::vector<double>(kVoiceParamCount, 0.0);
    std::vector<uint32_t> slotPassOf_ = std::vector<uint32_t>(kVoiceParamCount, 0u);
    uint32_t slotPass_ = 0;
    // Whether one of a voice's mod envelopes that modulates something (switched on, with a target) is still going.
    bool modEnvReleasing(const Voice &v, const ParamValues &params) const;
    // The clocks free LFOs follow instead of their own: they run at the newest
    // voice's rate, like the effects, and keep going between notes.
    std::array<Lfo, kMaxSources> freeLfos_{};
    // The newest voice's slots that modulate the sources' own params, gathered for runSharedLfos() once a call.
    std::array<SlotMod, kAllSources * kSlots> sourceMods_{};
    uint32_t freeRng_ = 1;
    std::vector<Voice> voices_;
    // The active voices (Voice::active), in their places' order, so going over them adds them up as going over every
    // place did; a voice is large, and the places resting needn't be read. setActive() keeps it; reserved in prepare().
    std::vector<Voice *> active_;
    void setActive(Voice &v, bool on);
    int liveVoiceCount() const;
    // The active voices, as Voice&, for loops that leave the list as it is.
    template <class V>
    struct ActiveVoices {
        const std::vector<Voice *> &list;
        struct It {
            Voice *const *at;
            V &operator*() const { return **at; }
            It &operator++() {
                ++at;
                return *this;
            }
            bool operator!=(const It &other) const { return at != other.at; }
        };
        It begin() const { return {list.data()}; }
        It end() const { return {list.data() + list.size()}; }
    };
    ActiveVoices<Voice> active() { return {active_}; }
    ActiveVoices<const Voice> active() const { return {active_}; }
    // Calls `f` on each active voice in order, which may end it (or start it fading, which keeps it listed).
    template <class F>
    void forEachActive(const F &f) {
        for (size_t i = 0; i < active_.size();) {
            Voice *v = active_[i];
            f(*v);
            if (i < active_.size() && active_[i] == v)
                ++i;
        }
    }
    // The voices' sum at their rate, one control block long, and what brings it down to the host rate.
    std::array<std::vector<double>, 2> mix_;
    std::array<Decimator, 2> decimators_;
    // Comb delay lines per voice, filter and channel, kept out of Voice so voices stay small.
    ZeroBuffer combs_; // a voice's line is cleared before a comb first reads it, so most are never written
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
    // The effect params as summed for this render() (see render()), from kFxBase on, and those the newest voice's
    // slots moved in the last control block.
    std::vector<double> fxBase_ = std::vector<double>(kShapeBase - kFxBase, 0.0);
    std::array<int, kAllSources * kSlots> fxMoved_{};
    int fxMovedCount_ = 0;
    double fxPitch_ = 60.0;  // the newest voice's pitch, for the effects' key tracking; kept once it has ended
    // The effects' modulation once the last voice has ended (see render()): the LFOs run on, the free and anchored
    // ones on their shared clocks and the others from where the last voice left them, and the other sources' slots
    // keep what they last gave; until the effects have been silent for a while.
    struct FxTail {
        bool on = false;
        std::array<Lfo, kMaxSources> lfos{};
        std::array<double, kAllSources> auxOut{};
        std::array<SlotMod, kAllSources * kSlots> held{}; // the slots of sources other than the LFOs
        int heldCount = 0;
        double silent = 0.0; // seconds the effects have put out silence
        uint32_t lfoMask = 0; // the LFOs (a bit a place) whose slots modulate the effects in the last block
        uint32_t rng = 1;
    };
    FxTail fxTail_;
    std::array<SlotMod, kAllSources * kSlots> fxTailMods_{};
    int fxTailModCount_ = 0; // fxTailMods_ in use, as of the last control block
    const Voice *fxLast_ = nullptr; // the newest voice in the last control block, and its note-on number
    uint64_t fxLastOrder_ = 0;
    // Starts the tail from the voice that was newest, which has just ended; and its modulation for this block.
    void startFxTail(const Voice &v, const ParamValues &params);
    int fxTailBlock(const ParamValues &params, uint32_t frames);
    double volumeGain_ = -1.0; // Volume's gain after the effects, gliding toward the knob (-1: not yet set)
    double volumeGlide_ = 0.0;
    // The output meter's peaks and clip light (see Telemetry), taken off what render() puts out.
    std::array<double, 2> outPeak_{};
    double clipHold_ = 0.0;
    void meterOutput(const float *left, const float *right, uint32_t frames);
    // The scope (see Telemetry::scope): each sample of the operator goes in at its copy 0's phase, which FM doesn't
    // move, so a span starts where a cycle does and stands still under FM; the points between two samples are drawn
    // in between them. A span is published whole once it is full.
    struct ScopeCapture {
        int op = -1;
        int cycles = 2;
        uint64_t order = 0;     // the voice it captures, by its note-on number
        bool started = false;   // has a first sample
        bool live = false;      // a voice was captured in the last control block
        bool whole = false;     // frame holds a full span
        double lastPos = 0.0;   // where the last sample went, in cycles since the first
        double lastPhase = 0.0;
        double lastValue = 0.0;
        double inc = 0.0;       // cycles a sample, for telling a slow span
        int64_t next = 0;       // the next point to fill in the span
        int64_t taken = 0;      // samples that went into the span being filled
        bool waiting = false;   // a short span was published: spans pass unfilled for a while (see scopeTake())
        int64_t rest = 0;       // samples still to let go by before the next span is filled
        std::array<float, kScopeBins> filling{};
        std::array<float, kScopeBins> frame{};
    };
    ScopeCapture scope_;
    const Voice *scopeVoice_ = nullptr; // the voice whose operator the scope takes, this control block
    void scopeTake(double phase, double value);
    void scopeRestart();
    // The output scope (see kOutScopeBins): a phase runs through each span at the note's pitch, and each point is
    // taken as it passes the point's place, between two samples: band-limited, from the kOutScopeTaps samples around
    // that place (kOutScopeTaps / 2 - 1 samples back, so the ones after it have come), so a wave dense with high
    // partials looks the same wherever the samples fall in it. A span is published whole once it is full, and the
    // next one starts at once, where it ended.
    struct OutScopeCapture {
        uint64_t order = 0;  // the newest voice, by its note-on number, when the spans last started over
        double phase = 0.0;  // through the span being filled, 0..1
        int next = 0;        // the next point to fill
        std::array<float, kOutScopeTaps> history{}; // the last samples, the newest at newest
        int newest = 0;
        std::array<float, kOutScopeBins> filling{};
        std::array<float, kOutScopeBins> frame{};
    };
    OutScopeCapture outScope_;
    void captureOutput(const float *left, const float *right, uint32_t frames);
    std::vector<NoteRef> ended_; // reserved in prepare(), never grows on the audio thread
};

} // namespace fmvibe
