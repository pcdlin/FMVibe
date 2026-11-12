#pragma once

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe {

constexpr int kOps = 6;     // operators A..F
constexpr int kMacros = 8;  // macro knobs, under the keyboard
constexpr int kMaxSources = 16; // places for LFOs and mod envelopes; see SourceType
constexpr int kSlots = 16;  // target slots per source
constexpr int kFirstSlots = 4; // the slots sources had at first; see idOfIndex()
constexpr int kFilters = 2;
// Each filter can feed every filter, itself too (see filterLinkIndex()). The links into later filters came first.
constexpr int kForwardFilterLinks = kFilters * (kFilters - 1) / 2;
constexpr int kFilterLinks = kFilters * kFilters;
constexpr int kShapers = 2; // per-voice waveshapers, which feed and are fed by the filters and modulate operators
constexpr int kFxSlots = 32; // places for effects; the effects graph's wires put them in order (see FxInputField)
constexpr int kFirstFxSlots = 16; // the slots the chain had at first; see idOfIndex()
// The effects graph, which the effects always run as: besides the slots, kFxSplits splits, each cutting what goes in
// into up to kFxBands bands by frequency; every slot and split takes up to kFxInputs wires in, the output kFxOutInputs.
// A chain is the graph's plainest case: the slots one after another from the input to the output.
constexpr int kFxSplits = 4;
constexpr int kFxBands = 3;
constexpr int kFxInputs = 4;
constexpr int kFxOutInputs = 8;
// And kFxSums sums, each adding up to kFxSumInputs wires into one output, for the paths it joins to go on together.
constexpr int kFxSums = 4;
constexpr int kFxSumInputs = 8;
constexpr int kMaxShapePoints = 64; // per freeform LFO or shaper curve, enough for a step a column on a grid of 32
constexpr int kMaxNotePoints = 16;  // on the note source's curve

constexpr int kMaxVoices = 512; // highest voice limit
constexpr int kMaxUnison = 16;  // copies per operator

// Voices is the polyphony limit. Mono plays one voice at a time; Legato (mono only) keeps
// envelopes running between overlapping notes; Glide slides the pitch from the
// previous note, on every note when GlideAlways is on, else only between
// overlapping ones. RetiredFxOrder was the order of the first kFirstFxSlots effect slots as one number; it does
// nothing since the chain's order is a param a place (see fxOrder()), kept so the ids after it stay.
// ModWheel is the mod wheel's position, 0..1: like a macro's knob, it moves only what it is assigned to (see
// WheelParam). PitchBend (-1..1) bends as the MIDI pitch bend does, added to it and to the editor's bend wheel: a
// param, so a host can automate and modulate the bend (per note too) as it does other params, and the pitch bend
// source follows it.
enum class GlobalParam {
    Volume,
    FmAmount,
    BendRange,
    Pitch,
    Pan,
    Voices,
    Mono,
    Legato,
    Glide,
    GlideAlways,
    RetiredFxOrder,
    ModWheel,
    PitchBend,
    Count
};
// Out goes straight to the output; the sends go into the filters (see sendIndex()) and the shapers
// (see shaperSendIndex()).
enum class OpParam {
    RetiredCoarse, // was the ratio's whole part, 0 (0.5) to 32; does nothing since Ratio, kept so the ids after it stay
    RetiredFine,   // was the ratio's fraction on top, 0..1 times it; likewise
    Detune,
    Out,
    Attack,
    Decay,
    Sustain,
    Release,
    RetiredVelocity, // was how much note velocity scaled the operator; does nothing, kept so the ids after it stay
    Wave,
    Unison,       // copies of the operator, 1 = off
    UnisonDetune, // cents of the outermost copies
    UnisonSpread, // stereo width of the copies; for Noise, how far left and right part
    Enabled,      // off: silent, skipped, and hidden from the routing views
    PhaseRandom,  // 0: every note starts the waveform (each unison copy) at Phase; 1: up to a cycle later, at random
    AttackCurve,  // envelope segment slopes, -1..1; see envCurve()
    DecayCurve,
    ReleaseCurve,
    NoiseSlope,   // the Noise wave's spectrum, 0..1: 0 white, 0.5 pink, 1 brown; see NoiseColor
    Hold,         // how long the envelope stays at full level between attack and decay; 0 is none
    Transpose,    // semitones, on top of the ratio; continuous, so modulation glides
    Sends,        // one per filter
    ShaperSends = Sends + kFilters, // one per shaper
    Fixed = ShaperSends + kShapers, // on: plays at FixedFreq x FixedMult whatever the note (see fixedHz())
    FixedFreq,                      // 0..1 on a log curve, see fixedFreqFromNorm()
    FixedMult,                      // one of kFixedMults, by number
    Phase,                          // 0..1 of a cycle: where a note starts the waveform (see PhaseRandom)
    Ratio,                          // 0..1 on a log curve, 0.5 to 64 times the note; see ratioFromNorm()
    Pan,                            // -1..1: where what it puts out (to the output, the filters, the shapers) sits;
                                    // what other operators hear of it stays mono
    Count
};
// Slope: 6, 12, 18 or 24 dB per octave, for the types that have one (see hasSlope() and filterSlope()).
// Shapers: how much of the filter goes into each shaper (see filterShaperIndex()). Mix, added later, blends what goes
// in (before Drive) with what the filter makes of it, as the filter effect's does: 1 (to begin with) all filter.
// Everything the filter puts out (to OUT, the operators, the filters and the shapers) is that blend. Stored in projects
// by number: only append.
enum class FilterParam {
    Type,
    Cutoff,
    Resonance,
    KeyTrack,
    Drive,
    Out,
    Enabled,
    Slope,
    Shapers,
    Mix = Shapers + kShapers,
    Count
};
// A shaper in each voice: the shaper effect's curves (see ClipMode), drive, output, mix and link,
// and a drawn curve of its own for Custom (see shaperCurveCountIndex()); shaperParams() finds the fields of either.
// The operators and the filters (see filterShaperIndex()) send into it; it feeds the filters (Filters, one per
// filter), modulates operators like an
// operator does (Mods, one per operator) and goes straight to the output by Out, like an operator (Output is
// its level after the curve). Symmetric mirrors a Custom curve's upper half onto its lower one (see ClipCurve).
// Shapers, one per shaper, feed the shapers, itself among them (see shaperShaperIndex()).
// Stored in projects by number: only append.
enum class ShaperParam {
    Enabled,
    Mode,
    Drive,
    Output,
    Mix,
    Link,
    Filters,
    Mods = Filters + kFilters,
    Out = Mods + kOps,
    Symmetric,
    Shapers,
    Count = Shapers + kShapers
};
// Modulation sources: LFOs and mod envelopes. Like the effect slots, each of
// the kMaxSources places holds a source of either type, or none, and has the
// fields of both; only those of its type are used (and shown to hosts). Stored
// in projects by number: only append.
// Random and Midi came later. Random is a random LFO: it runs as an LFO does, with the LFO's Rate, Sync, Division,
// Free, One-Shot and Anchor, and takes a new value each cycle (see RandomChaos and RandomSmooth below); at a Rate of
// 0 Hz (unsynced) it takes one as each note starts (see lfoHeld()) and holds it. It has no Shape, Phase or Unipolar,
// but its Bipolar. Midi follows what its MidiInput picks: a MIDI controller, CcNumber, the same for every voice; pressure,
// a voice's own note's (MIDI polyphonic aftertouch, CLAP's pressure expression, or channel pressure on an MPE member
// channel) once some came, else the channel's; or slide, a voice's own note's (CLAP's brightness expression, or
// CC 74 on an MPE member channel), resting in the middle.
// It follows that through Slew, a time constant (at its least, 1 ms, all but at once). Both are unipolar unless
// their Bipolar is on, which swings their depths across the set value as a bipolar LFO's (see sourceBipolar()).
enum class SourceType { None, Lfo, Env, Random, Midi, Count };
// What a Midi source follows. Stored in projects by number: only append.
enum class MidiInput { Cc, Pressure, Slide, Count };
// After Type and Enabled come kSlots (target, amount) pairs. A slot's target
// param holds the id + 1 of the param it modulates (0 = unused); its amount is
// -1..1 of that param's range. A synced LFO runs at a note value (Division) of
// the host tempo instead of at Rate. A source that is off runs nothing and
// modulates nothing. A free LFO isn't restarted by notes: it runs on one clock
// all voices share. A unipolar LFO swings 0..1 instead of -1..1, so it only
// pushes its targets one way, like an envelope; a bipolar one swings a slot's depth
// across the set value, half below and half above (100 % is -50 % to +50 %). A one-shot LFO runs one cycle
// from each note and then stays where the cycle ends; it is never free. At a Rate of 0 Hz (its lowest), an LFO stands
// still. A
// Random source takes a new value each cycle: Chaos sets how far it may jump from
// the last one (at 100 % anywhere), Smooth how much of the cycle it glides there. Phase is where in its cycle an
// LFO starts with each note (a one-shot runs from there to the cycle's end); a free one ignores it. An
// anchored LFO (Anchor, added later) is tied to the host's transport: while it plays, the LFO stands where it would
// had it started at Phase at the song's start and run at its rate since (in beats when synced, else in seconds), all
// voices sharing it; while the transport is stopped, or the host gives no position, it runs on as a free one. A
// one-shot is never anchored. An
// envelope is unipolar
// unless EnvBipolar, which swings its depth across the set value like a bipolar LFO's (see sourceBipolar()).
enum class SourceParam {
    Type,
    Enabled,
    Slots,
    // LFO.
    Shape = Slots + 2 * kSlots,
    Rate,
    Sync,
    Division,
    Free,
    Unipolar,
    OneShot,
    // Mod envelope.
    Attack,
    Decay,
    Sustain,
    Release,
    Hold,
    AttackCurve,
    DecayCurve,
    ReleaseCurve,
    // Added later, so they sit after the fields above: ids go by position. Random's fields (an LFO's once, for its
    // retired Random shape).
    RandomSmooth,
    RandomChaos,
    EnvBipolar, // a mod envelope's field
    Phase,      // an LFO's
    // Random's and Midi's (see SourceType).
    Slew,      // Midi's
    CcNumber,  // Midi's: 0..127, while its input is a controller
    Bipolar,   // both's
    MidiInput, // Midi's (see MidiInput)
    Anchor,    // an LFO's (see lfoAnchored())
    Count
};
// A freeform LFO shape, one per source: PointCount points, then
// kMaxShapePoints (x, y, curve) triples. Points are sorted by x (0..1) and the
// first sits at 0; y is -1..1. A point's curve (-1..1) bends the segment that
// leaves it, and the last segment wraps around to the first point. Hidden from
// hosts: the editor draws them. A shaper's drawn curve (ClipMode::Custom) has
// the same layout, one per effect slot and voice shaper, but doesn't wrap: see
// clipCurveValue().
// A macro's knob (Value, 0..1) and, like a source's, kSlots (target, amount) pairs: what the knob sets, times
// each amount, is added to its target for every voice and for the effects alike. Hosts see the knobs; the
// editor sets the slots. In the editor and its modulation lists a macro counts as source kMaxSources + macro.
// Bipolar swings each depth across the set value, nothing at the knob's middle (see sourceBipolar()).
// Stored in projects by number: only append.
enum class MacroParam { Value, Slots, Bipolar = Slots + 2 * kSlots, Count };
constexpr int kMacroParamCount = static_cast<int>(MacroParam::Count);
constexpr int kNoteSource = kMaxSources + kMacros; // the note source (see NoteParam)
constexpr int kWheelSource = kNoteSource + 1;      // the mod wheel (see WheelParam)
constexpr int kVelocitySource = kWheelSource + 1;  // the velocity source (see VelocityParam)
constexpr int kBendSource = kVelocitySource + 1;    // the pitch bend (see BendParam)
// The places, the macros, the note source, the mod wheel, velocity and the pitch bend.
constexpr int kAllSources = kBendSource + 1;
constexpr bool isMacro(int source) { return source >= kMaxSources && source < kNoteSource; }
constexpr bool isNote(int source) { return source == kNoteSource; }
constexpr bool isWheel(int source) { return source == kWheelSource; }
constexpr bool isVelocity(int source) { return source == kVelocitySource; }
constexpr bool isBend(int source) { return source == kBendSource; }
// The sources under the keyboard that put out a knob's value (see stripKnobIndex()): the macros and the mod wheel.
constexpr bool hasKnob(int source) { return isMacro(source) || isWheel(source); }
// The sources that run per voice from what the note brings, each with a curve of its own: the note source and
// velocity.
constexpr bool isNoteBorne(int source) { return isNote(source) || isVelocity(source); }
// The macros, the note source, the mod wheel, velocity and the pitch bend, under the keyboard: no card, type, switch or
// bypasses.
constexpr bool isStripSource(int source) { return source >= kMaxSources; }
enum class ShapeParam { PointCount, Points, Count = Points + 3 * kMaxShapePoints };
enum class PointField { X, Y, Curve };
// The note source's curve: laid out the same, with kMaxNotePoints points.
constexpr int kNoteCurveParamCount = static_cast<int>(ShapeParam::Points) + 3 * kMaxNotePoints;
// The note source: kSlots (target, amount) pairs, like a macro's without a knob, then the range of notes it
// spans (Low to High, semitones in MIDI numbering) and a curve drawn across it, laid out like a shaper's
// (see noteSourceValue()). Per voice, it puts out where the voice's pitch (bend and glide included) falls in
// the range, through the curve, 0..1. As it starts, the range spans every MIDI note, C-1 to G9, and on to the
// pitch at 20 kHz (kMaxNotePitch), and the curve is drawn along each note's place on the cutoff knobs' curve (see
// cutoffNormOfPitch()), so that it puts a cutoff at 0 modulated by 100 % right on the note. It counts as source
// kNoteSource, after the macros. Bipolar, like a macro's. Stored in projects by number: only append.
enum class NoteParam {
    Slots,
    Low = Slots + 2 * kSlots,
    High,
    Curve,
    Bipolar = Curve + kNoteCurveParamCount,
    Count
};
constexpr int kNoteParamCount = static_cast<int>(NoteParam::Count);
// The mod wheel: kSlots (target, amount) pairs, a macro's with GlobalParam::ModWheel for its knob (see
// stripKnobIndex()), and Bipolar like a macro's. It counts as source kWheelSource, after the note source. Stored in
// projects by number: only append.
enum class WheelParam { Slots, Bipolar = Slots + 2 * kSlots, Count };
constexpr int kWheelParamCount = static_cast<int>(WheelParam::Count);
// The velocity source: kSlots (target, amount) pairs, like the note source's, then a curve drawn across the
// velocities, laid out like the note source's (see velocitySourceValue()), and Bipolar like a macro's. Per voice, it
// puts out the velocity its note came with (0..1) through the curve, 0..1. As it starts, the curve is a straight
// line, so it puts out the velocity as it came. It counts as source kVelocitySource, after the mod wheel. Stored in
// projects by number: only append.
enum class VelocityParam { Slots, Curve = Slots + 2 * kSlots, Bipolar = Curve + kNoteCurveParamCount, Count };
constexpr int kVelocityParamCount = static_cast<int>(VelocityParam::Count);
// The pitch bend: kSlots (target, amount) pairs, like the mod wheel's, and Bipolar like a macro's, on to begin with. Per
// voice, it puts out where the bend stands, 0..1, its middle at rest: the host's bend and the editor's bend wheel and,
// with MPE on, a note's own channel's too (see Synth::renderBlock()). It counts as source kBendSource, after velocity.
// Stored in projects by number: only append.
enum class BendParam { Slots, Bipolar = Slots + 2 * kSlots, Count };
constexpr int kBendParamCount = static_cast<int>(BendParam::Count);
// Each slot of every source also has an Aux and an Amount (see slotAuxIndex()): its depth is scaled by Amount (0..1,
// all of it to begin with) and, while it has an aux, by what that other source puts out now, 0..1, taken as unipolar
// (see auxValue()). Aux holds the aux's source number + 1 (0 = none): wiring like a target, set in the editor. Stored
// in projects by number: only append.
enum class SlotAuxParam { Aux, Amount, Count };
constexpr int kSlotAuxFields = static_cast<int>(SlotAuxParam::Count);

// Stored in projects by number: only append. Random is retired, the Random source taking its place: the editor no
// longer offers it, and a state from before version 11 loads an LFO with it as a Random source (see state.cpp). An
// LFO still holding it (a state can) plays it as a Random source would. A Random source runs as an LFO of this shape
// (see lfoShape()).
enum class LfoShape { Sine, Triangle, Saw, Square, Random, Custom, Count };
// Stored in projects by number: only append.
// Custom is a wave drawn from partials: see partialLevelIndex(). Alt Sine is a sine at twice the rate for the first
// half of the cycle and silence for the second, like the classic FM chips'; Organ a stack of drawbar harmonics; Vowel
// a buzz through two formant peaks, an "ah".
enum class OpWave { Sine, Triangle, Saw, Square, Pulse, HalfSine, AbsSine, Noise, Custom, AltSine, Organ, Vowel, Count };
// Stored in projects by number: only append.
enum class FilterType { LowPass, HighPass, BandPass, Notch, Comb, Count };
// How a route into an operator (from an operator, a filter or a shaper) acts on it; see routeModeIndex(). Phase adds
// the source to where the operator reads its wave (phase modulation, what FM synths call FM); Linear FM adds it to the
// operator's frequency, through zero once deep enough; Exp FM moves its pitch by octaves; Ring multiplies its output
// by the source, all the way at 100 % and as classic AM at 50 %. Feedback is always Phase. Stored in projects by
// number: only append.
enum class RouteMode { Phase, LinearFm, ExpFm, Ring, Count };
inline RouteMode routeMode(double value) {
    return static_cast<RouteMode>(std::clamp(std::lround(value), 0L, static_cast<long>(RouteMode::Count) - 1L));
}
// The types that come in more than one slope.
constexpr bool hasSlope(FilterType type) { return type <= FilterType::Notch; }
// A filter's slope (FilterParam::Slope, and the filter effect's): 6, 12, 18 or 24 dB per octave, stored by number,
// 12 to begin with. Low- and high-passes come in all four; a band-pass or notch only in 12 and 24, so 6 and 18 play
// as the slope above them. Stored in projects by number: only append.
constexpr int kFilterSlopes = 4;
constexpr int kDefaultSlope = 1;
inline int filterSlope(double value) { return static_cast<int>(std::clamp(std::lround(value), 0L, kFilterSlopes - 1L)); }
constexpr int slopeDb(int slope) { return 6 * (slope + 1); }
constexpr bool slopeFits(FilterType type, int slope) {
    return type <= FilterType::HighPass || (hasSlope(type) && slope % 2 == 1);
}
constexpr int playedSlope(FilterType type, int slope) { return type <= FilterType::HighPass ? slope : slope | 1; }

// Effects, run on the sum of all voices. Each slot of the chain holds one
// effect of any type, or none, so a type can be used several times. A slot has
// the fields of every type; only those of its type are used (and shown to
// hosts). Stored in projects by number: only append.
enum class FxType {
    None,
    Shaper, // the voices' shapers on the whole mix
    Delay,
    Eq,
    ParamEq,
    Filter,
    Compressor,
    MultiComp, // retired: the three-band compressor, which a state loads as a MultiDyn (see readParams())
    Reverb,
    Phaser,
    Flanger,
    FreqShifter,
    Disperser,
    Widener,
    MultiDyn, // three bands, each compressed downward above one threshold and upward below another
    Amp,      // a guitar amp: preamp stages, a tone stack and a power amp, then a cabinet (see src/amp.h)
    Utility,  // gain, balance, stereo width and the bass in mono
    Chorus,   // a few voices of the sound, each a short delay swept a share of a cycle apart from the next
    Count
};
// Stored in projects by number: only append. Cubic is x - x^3/3 scaled to reach 1 at 1;
// Gentle is x / (1 + |x|), which never quite gets there; the folds fold back what goes
// past 1, smoothly (Sine Fold) or in straight lines (Fold); Custom is a drawn curve.
enum class ClipMode { Hard, Soft, Asymmetric, Cubic, Gentle, SineFold, Fold, Custom, Count };
// A parametric EQ band's shape: a bell, a shelf, a 12 dB per octave cut (where Q is the resonance and the gain does
// nothing) or a notch (where Q sets the width and the gain does nothing). Stored in projects by number: only append.
enum class PeqType { Peak, LowShelf, HighShelf, LowCut, HighCut, Notch, Count };
// The reverb's algorithm: Hall, the plain network; Room, shorter and darker, with distinct early
// reflections; Plate, denser and brighter; Shimmer, a brighter hall that feeds its own tail back into itself
// pitch-shifted, so each pass climbs (or falls) another interval. Stored in projects by number: only append.
enum class ReverbType { Hall, Room, Plate, Shimmer, Count };
// The amp's models (see kAmpModels in src/amp.h), the cabinets after it and the mics in front of them, and how many
// times the host rate the amp's stages run at (1x, 2x, 4x or 8x). Stored in projects by number: only append.
enum class AmpModel { Clean, Blues, Crunch, Lead, Heavy, Bass, Fuzz, Count };
enum class AmpCab { Open1x12, Open2x12, Closed4x12, Bass4x10, Bass1x15, Count };
enum class AmpMic { Dynamic, Condenser, Ribbon, Count };
constexpr int kAmpOversamplings = 4; // 1x, 2x, 4x, 8x: the factor is 1 << the value
constexpr int kDefaultAmpOversampling = 2;
template <class E>
inline E storedEnum(double value) { // a stored list's member, the first for a value out of its range
    const long t = std::lround(value);
    return static_cast<E>(t < 0 || t >= static_cast<long>(E::Count) ? 0 : t);
}
inline int ampOversampling(double value) { // the factor
    return 1 << static_cast<int>(std::clamp(std::lround(value), 0L, kAmpOversamplings - 1L));
}
// The mic's distance from the speaker: 0..1 on a log curve from 1 cm to 1 m.
constexpr double kMinAmpDistanceCm = 1.0, kMaxAmpDistanceCm = 100.0;
inline double ampDistanceCm(double norm) {
    return kMinAmpDistanceCm * std::pow(kMaxAmpDistanceCm / kMinAmpDistanceCm, std::clamp(norm, 0.0, 1.0));
}
inline double normFromAmpDistance(double cm) {
    return std::clamp(std::log(std::max(cm, kMinAmpDistanceCm) / kMinAmpDistanceCm) /
                          std::log(kMaxAmpDistanceCm / kMinAmpDistanceCm),
                      0.0, 1.0);
}
constexpr bool isCut(PeqType type) { return type == PeqType::LowCut || type == PeqType::HighCut; }
// Whether a band's Gain does anything: a cut's and a notch's don't.
constexpr bool peqHasGain(PeqType type) { return !isCut(type) && type != PeqType::Notch; }
inline PeqType peqType(double value) {
    const long t = std::lround(value);
    return static_cast<PeqType>(t < 0 ? 0 : (t >= static_cast<long>(PeqType::Count) ? 0 : t));
}
enum class FxParam {
    Type,
    Enabled,
    // Shaper: the voices' shapers' curves and knobs, on the whole mix (see ShaperParam, shaperParams()).
    ShaperMode,
    ShaperDrive,
    ShaperOutput,
    ShaperMix,
    // Delay. Synced, each side waits a note value (Division) of the host tempo
    // instead of its Time. Linked (DelayLink, at the end), the right side waits
    // as long as the left. Tone is a low-pass below the middle and a high-pass
    // above it (see delayTone()); Width narrows the echoes toward mono.
    DelayTimeL,
    DelayTimeR,
    DelaySync,
    DelayDivisionL,
    DelayDivisionR,
    DelayFeedback,
    DelayTone,
    DelayWidth,
    DelayPingPong,
    DelayMix,
    // DJ-style three-band isolator: band gains -1 (kill) .. 1 (+6 dB); see eqGain().
    EqLow,
    EqMid,
    EqHigh,
    // Two-band parametric EQ: bell bands.
    PeqFreq1,
    PeqGain1,
    PeqQ1,
    PeqFreq2,
    PeqGain2,
    PeqQ2,
    // The same filter types as the voices' filters, on the whole mix.
    FilterType,
    FilterCutoff,
    FilterResonance,
    FilterDrive,
    FilterMix,
    FilterSlope,
    // Compressor: stereo-linked and peak-sensing, with a soft knee Knee dB wide;
    // Makeup is gain after it. See compressorCurve().
    CompThreshold,
    CompRatio,
    CompKnee,
    CompAttack,
    CompRelease,
    CompMakeup,
    CompMix,
    // The retired three-band compressor's fields, kept for the states that hold them: loading one, its slot takes the
    // three-band dynamics, set from them (see readParams()). Linkwitz-Riley crossovers at LowFreq and HighFreq split
    // the signal, and each band was compressed on its own (threshold, ratio, and gain after it) with a fixed knee;
    // attack, release and mix were shared.
    McLowFreq,
    McHighFreq,
    McThreshold1,
    McRatio1,
    McGain1,
    McThreshold2,
    McRatio2,
    McGain2,
    McThreshold3,
    McRatio3,
    McGain3,
    McAttack,
    McRelease,
    McMix,
    // Added later, so they sit after the fields above: ids go by position.
    DelayLink,
    McOutput, // the three-band compressor's level after the mix
    // Reverb: a feedback delay network. Size scales its delay lines, Decay is the time it
    // takes to fall 60 dB, Damping darkens the tail, Pre-Delay holds it back from the dry sound.
    ReverbSize,
    ReverbDecay,
    ReverbDamping,
    ReverbPreDelay,
    ReverbWidth,
    ReverbMix,
    // Phaser: Stages all-passes whose corner an LFO sweeps up to Depth x 2.5 octaves either way of
    // Center, with feedback from the last stage into the first. Mix at 100 % is half wet, where the notches
    // are deepest (see sweepWet()).
    PhaserRate,
    PhaserDepth,
    PhaserCenter,
    PhaserFeedback,
    PhaserStages,
    PhaserMix,
    // Flanger: a short delay an LFO sweeps by Depth around Delay, fed back on itself. Mix like the phaser's.
    FlangerRate,
    FlangerDepth,
    FlangerDelay,
    FlangerFeedback,
    FlangerMix,
    // Frequency shifter: moves every partial by Shift (-1..1) of Range Hz (ShiftRange, at the end; see shiftHz()),
    // the right side the other way as Spread goes up; Feedback sends the shifted sound round again.
    ShiftAmount,
    ShiftSpread,
    ShiftFeedback,
    ShiftMix,
    // The parametric EQ bands' shapes (see PeqType).
    PeqType1,
    PeqType2,
    // The three-band compressor linked: every band uses the low band's threshold, ratio and gain; each
    // keeps its own for when it is unlinked.
    McLink,
    // The shaper's output linked to its drive: it comes down as the drive goes up, by as much as keeps the
    // level about even (see clipOutputDb()), so turning the drive changes the tone more than the level.
    ShaperLink,
    ReverbType, // see ReverbType
    PhaserStereo, // the phaser's right side a quarter cycle ahead of its left; off, both sweep together
    // Phase disperser: Stages second-order all-passes at Freq in a row, each as narrow as Pinch sets (see
    // disperserCoefs()): the sound around Freq comes out later than the rest, smeared in time, at full level.
    DisperserFreq,
    DisperserStages,
    DisperserPinch,
    DisperserMix,
    // Widener: short delays, slowly swept, of the sound above HighPass, added as its sides (Width
    // 100 %) or as a chorus in the middle (0 %), so the mono sum keeps the dry sound; Size sets how long the
    // delays are and how far they sweep (see widenerTap()).
    WidenerSize,
    WidenerWidth,
    WidenerHighPass,
    WidenerMix,
    // The phaser's and flanger's sweeps synced: a cycle per note value (Division) of the host tempo instead of
    // their Rate (see sweepHz()).
    PhaserSync,
    PhaserDivision,
    FlangerSync,
    FlangerDivision,
    // The filter's cutoff follows the newest voice's pitch, like a voice filter's Key Track.
    FilterKeyTrack,
    ShiftRange, // the frequency shifter's range, which its Shift takes a share of
    // The shimmer reverb: the interval (semitones) its tail is shifted by each time round, and how much of what
    // goes round is shifted.
    ReverbShimmerPitch,
    ReverbShimmer,
    FlangerStereo, // the flanger's right side a quarter cycle ahead of its left; off, both sweep together
    // The shaper's Custom curve odd: what it does to the input above 0, mirrored for the input below (see ClipCurve).
    ShaperSymmetric,
    // Three-band dynamics: Linkwitz-Riley crossovers at LowFreq and HighFreq split the signal, and each band is pulled
    // down above its Above threshold (by its Down ratio) and lifted below its Below threshold (by its Up ratio, at
    // most Range dB), then takes its Gain (see dynamicsGain()). The detector follows each band's peaks or, with Rms,
    // its RMS level; Attack and Release, Knee, Range, Mix and Output are shared. Link is the editor's: while it is on,
    // an edit of one band's threshold, ratio or gain sets every band's (see Controller::linkedBands()); each band
    // always plays its own.
    MdLowFreq,
    MdHighFreq,
    MdAbove1,
    MdDown1,
    MdBelow1,
    MdUp1,
    MdGain1,
    MdAbove2,
    MdDown2,
    MdBelow2,
    MdUp2,
    MdGain2,
    MdAbove3,
    MdDown3,
    MdBelow3,
    MdUp3,
    MdGain3,
    MdAttack,
    MdRelease,
    MdKnee,
    MdRange,
    MdMix,
    MdOutput,
    MdLink,
    MdRms,
    // The parametric EQ's third band (its first two are PeqFreq1..PeqQ2 and PeqType1, PeqType2), then whether each
    // band plays: a band switched off leaves the sound alone (see peqOnIndex()).
    PeqFreq3,
    PeqGain3,
    PeqQ3,
    PeqType3,
    PeqOn1,
    PeqOn2,
    PeqOn3,
    PeqAmount, // the parametric EQ's output against what went in, as a mix: 0 leaves the sound alone
    // The phaser's and flanger's polarity: negative turns their wet sound over, and their feedback with it, so every
    // notch moves to where a peak was and the Feedback knob keeps its sense (see sweepPolarity()).
    PhaserNegative,
    FlangerNegative,
    // Amp: Model picks the preamp stages, the tone stack and the power amp (see AmpModel); Gain drives the preamp,
    // Bass, Mid and Treble set the tone stack, Presence a shelf into the power amp, which Volume drives. The cabinet
    // (CabOn, Cab) after it is heard through a mic (Mic) at Position (0 its center, 1 its edge) and Distance (see
    // ampDistanceCm()). Stereo runs a side an amp; off, one amp runs on their sum. Oversampling is how many times the
    // host rate the stages run at (see ampOversampling()). Gain to Volume are 0..1, shown as 0 to 10, as on an amp.
    AmpModel,
    AmpGain,
    AmpBass,
    AmpMid,
    AmpTreble,
    AmpPresence,
    AmpVolume,
    AmpOutput,
    AmpMix,
    AmpStereo,
    AmpCabOn,
    AmpCab,
    AmpMic,
    AmpPosition,
    AmpDistance,
    AmpOversampling,
    // Utility: Gain from silence up to +12 dB (see utilityGain()), Pan a balance that turns the other side down, Width
    // the sides against the middle (0 mono, 1 as it was, 2 twice as wide), and MonoBass a 12 dB per octave high-pass on
    // the sides, so the bass below it is mono; at 0 it is off (see utilityMonoBassHz()).
    UtilityGain,
    UtilityPan,
    UtilityWidth,
    UtilityMonoBass,
    // Chorus: Voices (2, 3, 4 or 6) copies of the sound above its bass, each delayed by Delay (each voice a little
    // shorter or longer) swept by Depth (up to half of it either way) at Rate, the voices' sweeps a share of a cycle
    // apart, so their pitches waver against each other. Spread pans the voices across the sides (0 all in the middle,
    // 1 from one side to the other); Feedback sends what the voices put out on each side back into its line. Mix takes
    // the dry sound over to the voices alone (see Effects::runChorus()).
    ChorusRate,
    ChorusDepth,
    ChorusDelay,
    ChorusFeedback,
    ChorusSpread,
    ChorusMix,
    ChorusVoices,
    Count
};
// A split in the effects graph: Used puts it in the graph; Bands (2 or 3) cuts what goes in at Low, and at High too
// for three, with Linkwitz-Riley crossovers whose bands sum back flat. Low and High are on the cutoff curve, High kept
// a third of an octave above Low (see splitCrossovers()). Slope is two bands' steepness, as a filter's slope is
// stored: 6, 12 or 24 dB per octave (18 plays as 24; three bands part at 12 or 24, 6 playing as 12). Stored in projects by number: only
// append.
enum class SplitParam { Used, Bands, Low, High, Slope, Count };
// A wire into a node of the effects graph: From is the port it comes from (fxPortValue(), 0 for no wire) and Level how
// much of it goes in. Stored in projects by number: only append.
enum class FxInputField { From, Level, Count };
// A sum's (a join's) wires' levels are in dB, from kSumFloorDb, silence, up to 0 dB; every other wire's is a share,
// 0..1. sumLevelGain() is a sum's level as a gain.
constexpr double kSumFloorDb = -60.0;
inline double sumLevelGain(double db) {
    return db <= kSumFloorDb ? 0.0 : std::pow(10.0, std::min(db, 0.0) / 20.0);
}
// A sum in the effects graph: Used puts it in the graph. As a join, SoloHigh and SoloLow solo its first two inputs, the
// high band and the low band, and SoloMid its third, a three-band split's middle band (see sumSoloGain()). Stored in
// projects by number: only append.
enum class SumParam { Used, SoloHigh, SoloLow, SoloMid, Count };
// The shaper's output level in dB, from its Output and Drive, linked or not. Linked, it comes down by as much as
// keeps a sine at -6 dBFS as loud (in RMS) as it went in: dB for dB with the drive while the driven sine stays
// under the ceiling, then less and less as the clipping squares it off, about 9 dB at the full 36 dB of drive.
// Louder sounds come out a little quieter, quieter ones a little louder.
inline double clipOutputDb(double outputDb, double driveDb, bool linked) {
    if (!linked)
        return outputDb;
    constexpr double kReference = 0.5;                             // the sine's peak
    const double peak = kReference * std::pow(10.0, driveDb / 20.0); // driven
    // Its mean square once a hard clip at 1 has cut off its tops, from the angle where it first reaches 1.
    double meanSquare = 0.5 * peak * peak;
    if (peak > 1.0) {
        const double edge = std::asin(1.0 / peak);
        meanSquare = 1.0 + 2.0 / std::numbers::pi *
                               (edge * (0.5 * peak * peak - 1.0) - 0.5 * peak * std::sqrt(1.0 - 1.0 / (peak * peak)));
    }
    return outputDb + 10.0 * std::log10(0.5 * kReference * kReference / meanSquare);
}
constexpr int kMaxPhaserStages = 12;
// The chorus's voices (the editor offers kChorusVoiceChoices of them; a host or modulation can set 5 too), and its
// Delay knob's top (seconds).
constexpr int kMinChorusVoices = 2, kMaxChorusVoices = 6;
constexpr int kChorusVoiceChoices[] = {2, 3, 4, 6};
constexpr double kMaxChorusDelay = 0.04;
// Each chorus voice waits Delay times its share (chorusShare()): 1, a little more or less for each voice
// (kChorusStagger of it at the most, either way: chorusStagger()), swept by Depth up to kChorusSweep of it either way
// with its sweep at `lfo` (-1..1), its sweep chorusOffset() of a cycle behind the first voice's; and drifting slowly
// at a pace of its own (kChorusDriftHz) by up to kChorusDrift of it at full Depth. The voices' places along the delays
// are interleaved, the first voice's the shortest and the second's the longest, so voices beside each other between
// the sides wait unalike.
constexpr double kChorusSweep = 0.5;
constexpr double kChorusStagger = 0.2;
constexpr double kChorusDrift = 0.1;
inline double chorusStagger(int voice, int voices) {
    if (voices <= 1)
        return 0.0;
    const int place = voice % 2 == 0 ? voice / 2 : voices - 1 - voice / 2;
    return kChorusStagger * (-1.0 + 2.0 * place / (voices - 1.0));
}
inline double chorusShare(int voice, int voices, double depth, double lfo) {
    return 1.0 + chorusStagger(voice, voices) + kChorusSweep * std::clamp(depth, 0.0, 1.0) * lfo;
}
inline double chorusOffset(int voice, int voices) {
    return static_cast<double>(voice) / voices;
}
constexpr int kMaxDisperserStages = 64;
constexpr double kMaxWidenerHighPass = 2000.0; // Hz, the top of the widener's High Pass knob
constexpr double kMaxUtilityWidth = 2.0;       // the utility's Width at its top: the sides twice as loud
constexpr double kMaxUtilityMonoBass = 500.0;  // Hz, the top of the utility's Mono Bass knob
// The utility's Gain knob (0..1) as a gain: its cube, scaled so the top is kMaxUtilityGainDb, so 0 is silence, the
// middle about -6 dB and 0.63 0 dB (where it starts), the knob turning evenly through what is heard.
// utilityGainNorm() is the knob for a gain.
constexpr double kMaxUtilityGainDb = 12.0;
constexpr double kMaxUtilityGain = 3.9810717055349722; // +12 dB
inline double utilityGain(double norm) {
    const double x = std::clamp(norm, 0.0, 1.0);
    return x * x * x * kMaxUtilityGain;
}
inline double utilityGainNorm(double gain) { return std::cbrt(std::clamp(gain / kMaxUtilityGain, 0.0, 1.0)); }
constexpr int kPeqBands = 3;
constexpr int kMcBands = 3;
constexpr double kMcKnee = 6.0; // dB
// The three-band dynamics' detectors: the RMS one's time constant, and how fast the peak one falls back between peaks
// (its time constant), so a wave's zero crossings don't read as silence to the upward side.
constexpr double kMdRmsSeconds = 0.01;
constexpr double kMdPeakSeconds = 0.02;
// The three-band dynamics' upward side lifts a band only so far that its peaks, which the peak detector catches the
// moment they come, stay this far (dB) above Below: a note coming in out of silence isn't lifted by the Range the
// silence before it was, while the detector's level still climbs. A steady sound's peaks sit a few dB over its RMS
// level, which the lift takes up to Below at most, so it lifts them as its curve says.
constexpr double kMdLiftHeadroom = 12.0;
// The three-band dynamics' Above and Below thresholds reach down to here (dB); before state version 10, to
// kMdRetiredThresholdMinDb, so a modulation's depth on one loads scaled (see readParams()).
constexpr double kMdThresholdMinDb = -80.0;
constexpr double kMdRetiredThresholdMinDb = -60.0;

// --- Layout. An *index* is a position in the param table, used by the DSP.
// An *id* is what hosts store in projects and automation; ids are grouped per
// section with spare room, so fields can be added without renumbering.

constexpr int kGlobalCount = static_cast<int>(GlobalParam::Count);
constexpr int kOpParamCount = static_cast<int>(OpParam::Count);
constexpr int kSourceParamCount = static_cast<int>(SourceParam::Count);
constexpr int kFilterParamCount = static_cast<int>(FilterParam::Count);
constexpr int kShaperParamCount = static_cast<int>(ShaperParam::Count);
constexpr int kFxParamCount = static_cast<int>(FxParam::Count);
constexpr int kShapeParamCount = static_cast<int>(ShapeParam::Count);

constexpr int kOpBase = kGlobalCount;
constexpr int kMatrixBase = kOpBase + kOps * kOpParamCount;
constexpr int kFilterModBase = kMatrixBase + kOps * kOps;
constexpr int kFilterBase = kFilterModBase + kFilters * kOps;
constexpr int kFilterLinkBase = kFilterBase + kFilters * kFilterParamCount;
constexpr int kShaperBase = kFilterLinkBase + kFilterLinks;
constexpr int kSourceBase = kShaperBase + kShapers * kShaperParamCount;
constexpr int kFxBase = kSourceBase + kMaxSources * kSourceParamCount;
constexpr int kSplitParamCount = static_cast<int>(SplitParam::Count);
constexpr int kFxInputFields = static_cast<int>(FxInputField::Count);
// The effects graph's splits and wires, after the slots: shared by the voices like them, and modulated like them.
constexpr int kFxSplitBase = kFxBase + kFxSlots * kFxParamCount;
constexpr int kFxInputNodes = kFxSlots + kFxSplits; // the nodes with kFxInputs inputs: the slots, then the splits
constexpr int kFxInputBase = kFxSplitBase + kFxSplits * kSplitParamCount;
constexpr int kFxOutInputBase = kFxInputBase + kFxInputNodes * kFxInputs * kFxInputFields;
constexpr int kSumParamCount = static_cast<int>(SumParam::Count);
constexpr int kFxSumBase = kFxOutInputBase + kFxOutInputs * kFxInputFields;
constexpr int kFxSumInputBase = kFxSumBase + kFxSums * kSumParamCount;
constexpr int kShapeBase = kFxSumInputBase + kFxSums * kFxSumInputs * kFxInputFields; // the effect params run up to here
constexpr int kClipCurveBase = kShapeBase + kMaxSources * kShapeParamCount;
// Each source's slots can be bypassed one by one: the slot stops modulating but keeps its target and depth.
constexpr int kSlotBypassBase = kClipCurveBase + kFxSlots * kShapeParamCount;
// Each operator's Custom wave: the level (0..1) and phase (0..1 of a cycle) of each of its first kPartials
// harmonics, and how many of them it plays (16, 32 or 64; see partialCountIndex()): the ones past that keep their
// values, unheard. Hidden from hosts, like the drawn shapes; only ever read as set. The first kFirstPartials sit
// here, the ones added later after the mod wheel (see partialLevelIndex()).
constexpr int kPartials = 64;
constexpr int kFirstPartials = 32;
constexpr int kPartialCounts[] = {16, 32, 64}; // what the partial editor offers
constexpr int kDefaultPartials = 32;
constexpr int kPartialBase = kSlotBypassBase + kMaxSources * kSlots;
// The macros: a knob each and, like a source, kSlots (target, amount) pairs (see MacroParam).
constexpr int kMacroBase = kPartialBase + kOps * 2 * kFirstPartials;
// The note source's slots (see NoteParam).
constexpr int kNoteBase = kMacroBase + kMacros * kMacroParamCount;
// Each voice shaper's drawn curve, laid out like a shaper effect's: hidden from hosts, only ever read as set.
constexpr int kShaperCurveBase = kNoteBase + kNoteParamCount;
// The mod wheel's slots (see WheelParam).
constexpr int kWheelBase = kShaperCurveBase + kShapers * kShapeParamCount;
// How many partials each operator's Custom wave plays, then the partials past the first kFirstPartials.
constexpr int kPartialCountBase = kWheelBase + kWheelParamCount;
constexpr int kLaterPartialBase = kPartialCountBase + kOps;
// The velocity source (see VelocityParam).
constexpr int kVelocityBase = kLaterPartialBase + kOps * 2 * (kPartials - kFirstPartials);
// The pitch bend source (see BendParam).
constexpr int kBendBase = kVelocityBase + kVelocityParamCount;
// Each slot's aux and Amount (see SlotAuxParam): kSlots pairs a source, for the places, the macros, the note source,
// the mod wheel, velocity and the pitch bend in turn.
constexpr int kSlotAuxBase = kBendBase + kBendParamCount;
// The bypasses of the slots of the sources under the keyboard (the macros, the note source, the mod wheel, velocity
// and the pitch bend in turn), like the places' (see kSlotBypassBase).
constexpr int kStripBypassBase = kSlotAuxBase + kAllSources * kSlots * kSlotAuxFields;
// Each slot's curve (-1..1, 0 a straight line), which bends what its source puts out before the depth takes it, as
// an envelope segment's curve bends the segment (see slotCurveValue()): kSlots a source, for every source in turn.
constexpr int kSlotCurveBase = kStripBypassBase + (kAllSources - kMaxSources) * kSlots;
// How each route into an operator acts on it (see RouteMode): a block per node it comes from (the operators, the
// filters, then the shapers), a field per operator it goes into. An operator's own field, its feedback, is always
// Phase and hidden.
constexpr int kRouteSources = kOps + kFilters + kShapers;
constexpr int kRouteModeBase = kSlotCurveBase + kAllSources * kSlots;
// The effects chain's order as a state from before version 9 holds it: the slot at each of its kFxSlots places, in the
// order they ran (see fxOrder()). Loading one wires them as the effects graph's line (see readParams()); since then the
// graph's wires alone hold the order, and these do nothing.
constexpr int kFxPositionBase = kRouteModeBase + kRouteSources * kOps;
constexpr int kParamCount = kFxPositionBase + kFxSlots;
// The params before the shapes: what LFOs, envelopes and hosts can modulate,
// and what voices read with modulation applied. Shapes are only ever read as set.
constexpr int kModulatedCount = kShapeBase;
// The params voices read: everything before the effects, which all voices share.
constexpr int kVoiceParamCount = kFxBase;

constexpr int paramIndex(GlobalParam p) { return static_cast<int>(p); }
constexpr int paramIndex(int op, OpParam p) { return kOpBase + op * kOpParamCount + static_cast<int>(p); }
// Amount by which operator `from` modulates operator `to`; from == to is feedback.
constexpr int matrixIndex(int from, int to) { return kMatrixBase + from * kOps + to; }
// Amount by which the output of `filter` modulates operator `to`, like an operator would.
constexpr int filterModIndex(int filter, int to) { return kFilterModBase + filter * kOps + to; }
constexpr int paramIndex(int source, SourceParam p) {
    return kSourceBase + source * kSourceParamCount + static_cast<int>(p);
}
constexpr int paramIndex(int filter, FilterParam p) {
    return kFilterBase + filter * kFilterParamCount + static_cast<int>(p);
}
constexpr int paramIndex(int slot, FxParam p) { return kFxBase + slot * kFxParamCount + static_cast<int>(p); }
constexpr int paramIndex(int split, SplitParam p) {
    return kFxSplitBase + split * kSplitParamCount + static_cast<int>(p);
}
constexpr int paramIndex(int sum, SumParam p) { return kFxSumBase + sum * kSumParamCount + static_cast<int>(p); }
// How much of a join's input passes its solos: while any band is soloed, only the soloed ones (inputs 0, 1 and 2, the
// high, the low and the middle band; any other input of the sum goes on as it is).
template <class Get>
double sumSoloGain(int sum, int input, Get &&get) {
    const bool high = get(paramIndex(sum, SumParam::SoloHigh)) >= 0.5, low = get(paramIndex(sum, SumParam::SoloLow)) >= 0.5,
               mid = get(paramIndex(sum, SumParam::SoloMid)) >= 0.5;
    if (!(high || low || mid) || input > 2)
        return 1.0;
    return (input == 0 ? high : input == 1 ? low : mid) ? 1.0 : 0.0;
}
// The effects graph's nodes that take wires in: a slot (0..kFxSlots - 1), a split (kFxSlots + split), the output, then
// a sum (fxSumNode()), kFxNodes in all.
constexpr int kFxOutNode = kFxInputNodes;
constexpr int kFxNodes = kFxOutNode + 1 + kFxSums;
constexpr int fxSplitNode(int split) { return kFxSlots + split; }
constexpr int fxSumNode(int sum) { return kFxOutNode + 1 + sum; }
constexpr int fxNodeSum(int node) { return node > kFxOutNode && node < kFxNodes ? node - kFxOutNode - 1 : -1; }
constexpr bool fxNodeSplit(int node) { return node >= kFxSlots && node < kFxInputNodes; }
constexpr int fxInputCount(int node) {
    return node == kFxOutNode ? kFxOutInputs : fxNodeSum(node) >= 0 ? kFxSumInputs : kFxInputs;
}
constexpr int fxInputIndex(int node, int input, FxInputField f) {
    return node == kFxOutNode ? kFxOutInputBase + input * kFxInputFields + static_cast<int>(f)
           : fxNodeSum(node) >= 0
               ? kFxSumInputBase + (fxNodeSum(node) * kFxSumInputs + input) * kFxInputFields + static_cast<int>(f)
               : kFxInputBase + (node * kFxInputs + input) * kFxInputFields + static_cast<int>(f);
}
// Ports, what wires come from: the effects' input (the voices), each slot's output, each split's bands, then each sum's
// output.
constexpr int kFxPortIn = 0;
constexpr int kFxBandPorts = 1 + kFxSlots; // the first split's first band
constexpr int kFxSumPorts = kFxBandPorts + kFxSplits * kFxBands;
constexpr int kFxPorts = kFxSumPorts + kFxSums;
constexpr int fxSlotPort(int slot) { return 1 + slot; }
constexpr int fxBandPort(int split, int band) { return kFxBandPorts + split * kFxBands + band; }
constexpr int fxSumPort(int sum) { return kFxSumPorts + sum; }
constexpr int fxPortSlot(int port) { return port >= 1 && port <= kFxSlots ? port - 1 : -1; }
constexpr int fxPortSplit(int port) {
    return port >= kFxBandPorts && port < kFxSumPorts ? (port - kFxBandPorts) / kFxBands : -1;
}
constexpr int fxPortBand(int port) {
    return port >= kFxBandPorts && port < kFxSumPorts ? (port - kFxBandPorts) % kFxBands : -1;
}
constexpr int fxPortSum(int port) { return port >= kFxSumPorts && port < kFxPorts ? port - kFxSumPorts : -1; }
// The node a port comes out of: -1 for the effects' input.
constexpr int fxPortNode(int port) {
    return port == kFxPortIn          ? -1
           : fxPortSlot(port) >= 0    ? fxPortSlot(port)
           : fxPortSplit(port) >= 0   ? fxSplitNode(fxPortSplit(port))
                                      : fxSumNode(fxPortSum(port));
}
// A From param's value for a port (-1 for none), and back: -1 for none or anything that isn't a port.
constexpr double fxPortValue(int port) { return port < 0 ? 0.0 : port + 1.0; }
inline int fxPortOf(double value) {
    if (!(value >= 0.5 && value < kFxPorts + 0.5)) // NaN too
        return -1;
    return static_cast<int>(std::lround(value)) - 1;
}
// The split a param belongs to, -1 for other params.
constexpr int splitOf(int index) {
    return index >= kFxSplitBase && index < kFxInputBase ? (index - kFxSplitBase) / kSplitParamCount : -1;
}
// Whether a param is one of a join's solos.
constexpr bool isJoinSolo(int index) {
    return index >= kFxSumBase && index < kFxSumInputBase && (index - kFxSumBase) % kSumParamCount != 0;
}
// The join (a sum) whose band's level `index` is, the Level of one of its first three inputs (the high, the low and a
// three-band split's middle band); -1 for any other param.
constexpr int fxSumOfLevel(int index) {
    if (index < kFxSumInputBase || index >= kShapeBase)
        return -1;
    const int within = index - kFxSumInputBase;
    const int input = (within / kFxInputFields) % kFxSumInputs;
    return within % kFxInputFields == static_cast<int>(FxInputField::Level) && input < 3
               ? within / (kFxSumInputs * kFxInputFields)
               : -1;
}
// Whether a param belongs to the effects graph: a split's field, a join's or a wire's.
constexpr bool isFxGraphParam(int index) {
    return index >= kFxSplitBase && index < kShapeBase;
}

constexpr int paramIndex(int shaper, ShaperParam p) {
    return kShaperBase + shaper * kShaperParamCount + static_cast<int>(p);
}
// How much of operator `op` goes into `shaper`, of `shaper` into `filter`, of `filter` into `shaper` and of `from`
// into shaper `to` (from == to feeding itself back), and how strongly `shaper` modulates operator `to`.
constexpr int shaperSendIndex(int op, int shaper) { return paramIndex(op, OpParam::ShaperSends) + shaper; }
constexpr int shaperFilterIndex(int shaper, int filter) { return paramIndex(shaper, ShaperParam::Filters) + filter; }
constexpr int filterShaperIndex(int filter, int shaper) { return paramIndex(filter, FilterParam::Shapers) + shaper; }
constexpr int shaperShaperIndex(int from, int to) { return paramIndex(from, ShaperParam::Shapers) + to; }
constexpr int shaperModIndex(int shaper, int to) { return paramIndex(shaper, ShaperParam::Mods) + to; }
constexpr int shaperCurveCountIndex(int shaper) { return kShaperCurveBase + shaper * kShapeParamCount; }
// The source a param belongs to, and which of its fields it is; -1 / Count for other params.
constexpr int sourceOf(int index) {
    return index >= kSourceBase && index < kFxBase ? (index - kSourceBase) / kSourceParamCount : -1;
}
constexpr SourceParam sourceFieldOf(int index) {
    return sourceOf(index) < 0 ? SourceParam::Count : static_cast<SourceParam>((index - kSourceBase) % kSourceParamCount);
}
// Whether a source of type `t` runs as an LFO, on an LFO's clock: an LFO, and Random, a random LFO.
constexpr bool runsLfo(SourceType t) { return t == SourceType::Lfo || t == SourceType::Random; }
// Whether a source of type `t` uses field `f`: every source uses Type, Enabled and its slots.
constexpr bool sourceFieldUsedBy(SourceParam f, SourceType t) {
    if (f < SourceParam::Shape)
        return true;
    switch (f) {
    case SourceParam::EnvBipolar:
        return t == SourceType::Env;
    case SourceParam::Shape:
    case SourceParam::Unipolar:
    case SourceParam::Phase:
        return t == SourceType::Lfo;
    case SourceParam::Rate:
    case SourceParam::Sync:
    case SourceParam::Division:
    case SourceParam::Free:
    case SourceParam::OneShot:
    case SourceParam::Anchor:
        return runsLfo(t);
    case SourceParam::RandomSmooth:
    case SourceParam::RandomChaos:
        return t == SourceType::Random;
    case SourceParam::Slew:
    case SourceParam::CcNumber:
    case SourceParam::MidiInput:
        return t == SourceType::Midi;
    case SourceParam::Bipolar:
        return t == SourceType::Random || t == SourceType::Midi;
    default:
        return f < SourceParam::Attack ? t == SourceType::Lfo : t == SourceType::Env;
    }
}
// Whether the LFO in place `source` follows the host's transport (see SourceParam), or runs on one clock all voices
// share (an anchored one too, while the transport is stopped), reading its params through `get`. A one-shot is
// neither.
template <class Get>
bool lfoAnchored(const Get &get, int source) {
    return get(paramIndex(source, SourceParam::Anchor)) >= 0.5 && get(paramIndex(source, SourceParam::OneShot)) < 0.5;
}
template <class Get>
bool lfoShared(const Get &get, int source) {
    return get(paramIndex(source, SourceParam::OneShot)) < 0.5 &&
           (get(paramIndex(source, SourceParam::Free)) >= 0.5 ||
            get(paramIndex(source, SourceParam::Anchor)) >= 0.5);
}
// What an LFO's Free, Anchor and One-Shot make of it, as the engine reads them (One-Shot over Anchor over Free): the
// editor picks one, setting the switches for it.
enum class LfoClock { Retrigger, Free, Anchor, OneShot, Count };
template <class Get>
LfoClock lfoClock(const Get &get, int source) {
    return get(paramIndex(source, SourceParam::OneShot)) >= 0.5  ? LfoClock::OneShot
           : get(paramIndex(source, SourceParam::Anchor)) >= 0.5 ? LfoClock::Anchor
           : get(paramIndex(source, SourceParam::Free)) >= 0.5   ? LfoClock::Free
                                                                 : LfoClock::Retrigger;
}
inline SourceType sourceType(double value) {
    const long t = std::lround(value);
    return static_cast<SourceType>(t < 0 ? 0 : (t >= static_cast<long>(SourceType::Count) ? 0 : t));
}
// The shape the source in place `source` runs as an LFO: its Shape, or Random for a Random source.
template <class Get>
LfoShape lfoShape(const Get &get, int source) {
    if (sourceType(get(paramIndex(source, SourceParam::Type))) == SourceType::Random)
        return LfoShape::Random;
    const long shape = std::lround(get(paramIndex(source, SourceParam::Shape)));
    return static_cast<LfoShape>(std::clamp(shape, 0l, static_cast<long>(LfoShape::Count) - 1));
}
// Whether the LFO in place `source` stands still: unsynced, at a Rate of 0 Hz. A random one then takes a value of its
// own as each note starts, at once, and holds it.
template <class Get>
bool lfoHeld(const Get &get, int source) {
    return get(paramIndex(source, SourceParam::Sync)) < 0.5 && get(paramIndex(source, SourceParam::Rate)) <= 0.0;
}
// The shaper a param belongs to, and which of its fields it is; -1 / Count for other params.
constexpr int shaperOf(int index) {
    return index >= kShaperBase && index < kSourceBase ? (index - kShaperBase) / kShaperParamCount : -1;
}
constexpr ShaperParam shaperFieldOf(int index) {
    return shaperOf(index) < 0 ? ShaperParam::Count : static_cast<ShaperParam>((index - kShaperBase) % kShaperParamCount);
}
// The effect slot a param belongs to, and which of its fields it is; -1 / Count for other params.
constexpr int fxSlotOf(int index) {
    return index >= kFxBase && index < kFxSplitBase ? (index - kFxBase) / kFxParamCount : -1;
}
constexpr FxParam fxFieldOf(int index) {
    return fxSlotOf(index) < 0 ? FxParam::Count : static_cast<FxParam>((index - kFxBase) % kFxParamCount);
}
// The effect type each slot field belongs to, a run of fields a line, in FxParam's order: None for the fields every
// slot uses (Type, Enabled). A field added to FxParam needs a line here too (fxFieldRunsCoverAll() checks).
struct FxFieldRun {
    FxParam first, last;
    FxType type;
};
constexpr FxFieldRun kFxFieldRuns[] = {
    {FxParam::Type, FxParam::Enabled, FxType::None},
    {FxParam::ShaperMode, FxParam::ShaperMix, FxType::Shaper},
    {FxParam::DelayTimeL, FxParam::DelayMix, FxType::Delay},
    {FxParam::EqLow, FxParam::EqHigh, FxType::Eq},
    {FxParam::PeqFreq1, FxParam::PeqQ2, FxType::ParamEq},
    {FxParam::FilterType, FxParam::FilterSlope, FxType::Filter},
    {FxParam::CompThreshold, FxParam::CompMix, FxType::Compressor},
    {FxParam::McLowFreq, FxParam::McMix, FxType::MultiComp},
    {FxParam::DelayLink, FxParam::DelayLink, FxType::Delay},
    {FxParam::McOutput, FxParam::McOutput, FxType::MultiComp},
    {FxParam::ReverbSize, FxParam::ReverbMix, FxType::Reverb},
    {FxParam::PhaserRate, FxParam::PhaserMix, FxType::Phaser},
    {FxParam::FlangerRate, FxParam::FlangerMix, FxType::Flanger},
    {FxParam::ShiftAmount, FxParam::ShiftMix, FxType::FreqShifter},
    {FxParam::PeqType1, FxParam::PeqType2, FxType::ParamEq},
    {FxParam::McLink, FxParam::McLink, FxType::MultiComp},
    {FxParam::ShaperLink, FxParam::ShaperLink, FxType::Shaper},
    {FxParam::ReverbType, FxParam::ReverbType, FxType::Reverb},
    {FxParam::PhaserStereo, FxParam::PhaserStereo, FxType::Phaser},
    {FxParam::DisperserFreq, FxParam::DisperserMix, FxType::Disperser},
    {FxParam::WidenerSize, FxParam::WidenerMix, FxType::Widener},
    {FxParam::PhaserSync, FxParam::PhaserDivision, FxType::Phaser},
    {FxParam::FlangerSync, FxParam::FlangerDivision, FxType::Flanger},
    {FxParam::FilterKeyTrack, FxParam::FilterKeyTrack, FxType::Filter},
    {FxParam::ShiftRange, FxParam::ShiftRange, FxType::FreqShifter},
    {FxParam::ReverbShimmerPitch, FxParam::ReverbShimmer, FxType::Reverb},
    {FxParam::FlangerStereo, FxParam::FlangerStereo, FxType::Flanger},
    {FxParam::ShaperSymmetric, FxParam::ShaperSymmetric, FxType::Shaper},
    {FxParam::MdLowFreq, FxParam::MdRms, FxType::MultiDyn},
    {FxParam::PeqFreq3, FxParam::PeqAmount, FxType::ParamEq},
    {FxParam::PhaserNegative, FxParam::PhaserNegative, FxType::Phaser},
    {FxParam::FlangerNegative, FxParam::FlangerNegative, FxType::Flanger},
    {FxParam::AmpModel, FxParam::AmpOversampling, FxType::Amp},
    {FxParam::UtilityGain, FxParam::UtilityMonoBass, FxType::Utility},
    {FxParam::ChorusRate, FxParam::ChorusVoices, FxType::Chorus},
};
// Whether the runs take every field once, in order, up to the last.
constexpr bool fxFieldRunsCoverAll() {
    int next = 0;
    for (const FxFieldRun &run : kFxFieldRuns) {
        if (static_cast<int>(run.first) != next || run.last < run.first)
            return false;
        next = static_cast<int>(run.last) + 1;
    }
    return next == kFxParamCount;
}
static_assert(fxFieldRunsCoverAll(), "every FxParam field needs its effect type in kFxFieldRuns, in order");
// The runs spread out, a type per field.
constexpr std::array<FxType, kFxParamCount> kFxFieldTypes = [] {
    std::array<FxType, kFxParamCount> types{};
    for (const FxFieldRun &run : kFxFieldRuns)
        for (int f = static_cast<int>(run.first); f <= static_cast<int>(run.last); ++f)
            types[static_cast<size_t>(f)] = run.type;
    return types;
}();
constexpr FxType fxTypeOfField(FxParam f) {
    const int field = static_cast<int>(f);
    return field >= 0 && field < kFxParamCount ? kFxFieldTypes[static_cast<size_t>(field)] : FxType::None;
}
inline FxType fxType(double value) {
    const long t = std::lround(value);
    return static_cast<FxType>(t < 0 ? 0 : (t >= static_cast<long>(FxType::Count) ? 0 : t));
}
// Fields of band `band` of the parametric EQ: the first two bands' sit together, the third's came later.
constexpr int peqFreqIndex(int slot, int band) {
    return band < 2 ? paramIndex(slot, FxParam::PeqFreq1) + 3 * band : paramIndex(slot, FxParam::PeqFreq3);
}
constexpr int peqGainIndex(int slot, int band) { return peqFreqIndex(slot, band) + 1; }
constexpr int peqQIndex(int slot, int band) { return peqFreqIndex(slot, band) + 2; }
static_assert(peqQIndex(0, 1) == paramIndex(0, FxParam::PeqQ2) && peqQIndex(0, 2) == paramIndex(0, FxParam::PeqQ3));
constexpr int peqTypeIndex(int slot, int band) {
    return band < 2 ? paramIndex(slot, FxParam::PeqType1) + band : paramIndex(slot, FxParam::PeqType3);
}
static_assert(peqTypeIndex(0, 1) == paramIndex(0, FxParam::PeqType2) &&
              peqTypeIndex(0, 2) == paramIndex(0, FxParam::PeqType3));
constexpr int peqOnIndex(int slot, int band) { return paramIndex(slot, FxParam::PeqOn1) + band; }
static_assert(peqOnIndex(0, 2) == paramIndex(0, FxParam::PeqOn3));
// The band whose shape `typeIndex` is, or -1.
constexpr int peqBandOfType(int typeIndex) {
    const int slot = fxSlotOf(typeIndex);
    for (int band = 0; slot >= 0 && band < kPeqBands; ++band)
        if (peqTypeIndex(slot, band) == typeIndex)
            return band;
    return -1;
}
// Fields of band `band` of the three-band compressor.
constexpr int mcThresholdIndex(int slot, int band) { return paramIndex(slot, FxParam::McThreshold1) + 3 * band; }
constexpr int mcRatioIndex(int slot, int band) { return mcThresholdIndex(slot, band) + 1; }
constexpr int mcGainIndex(int slot, int band) { return mcThresholdIndex(slot, band) + 2; }
static_assert(mcGainIndex(0, 2) == paramIndex(0, FxParam::McGain3));
// Fields of band `band` of the three-band dynamics.
constexpr int mdAboveIndex(int slot, int band) { return paramIndex(slot, FxParam::MdAbove1) + 5 * band; }
constexpr int mdDownIndex(int slot, int band) { return mdAboveIndex(slot, band) + 1; }
constexpr int mdBelowIndex(int slot, int band) { return mdAboveIndex(slot, band) + 2; }
constexpr int mdUpIndex(int slot, int band) { return mdAboveIndex(slot, band) + 3; }
constexpr int mdGainIndex(int slot, int band) { return mdAboveIndex(slot, band) + 4; }
static_assert(mdGainIndex(0, 2) == paramIndex(0, FxParam::MdGain3));
constexpr int shapeCountIndex(int source) { return kShapeBase + source * kShapeParamCount; }
constexpr int shapePointIndex(int source, int point, PointField field) {
    return shapeCountIndex(source) + static_cast<int>(ShapeParam::Points) + 3 * point + static_cast<int>(field);
}
// Fields of a shape, LFO or shaper curve, by the index of its point count.
constexpr int shapeFieldIndex(int base, int point, PointField field) {
    return base + static_cast<int>(ShapeParam::Points) + 3 * point + static_cast<int>(field);
}
constexpr int clipCurveCountIndex(int slot) { return kClipCurveBase + slot * kShapeParamCount; }
// Shapers come as effects and in the voices, alike but for their params' places. A shaper unit is an effect slot
// (0..kFxSlots - 1), or kFxSlots + a voice shaper; its fields, `curve` being its drawn curve's point count.
constexpr int kShaperUnits = kFxSlots + kShapers;
struct ShaperParams {
    int mode, drive, output, mix, link, curve, symmetric;
};
constexpr ShaperParams shaperParams(int unit) {
    if (unit < kFxSlots)
        return {paramIndex(unit, FxParam::ShaperMode),  paramIndex(unit, FxParam::ShaperDrive),
                paramIndex(unit, FxParam::ShaperOutput), paramIndex(unit, FxParam::ShaperMix),
                paramIndex(unit, FxParam::ShaperLink),   clipCurveCountIndex(unit),
                paramIndex(unit, FxParam::ShaperSymmetric)};
    const int shaper = unit - kFxSlots;
    return {paramIndex(shaper, ShaperParam::Mode),   paramIndex(shaper, ShaperParam::Drive),
            paramIndex(shaper, ShaperParam::Output), paramIndex(shaper, ShaperParam::Mix),
            paramIndex(shaper, ShaperParam::Link),   shaperCurveCountIndex(shaper),
            paramIndex(shaper, ShaperParam::Symmetric)};
}
// Every source's slots can be bypassed: the places' with their own, the others' after the slots' auxes.
constexpr int slotBypassIndex(int source, int slot) {
    return isStripSource(source) ? kStripBypassBase + (source - kMaxSources) * kSlots + slot
                                 : kSlotBypassBase + source * kSlots + slot;
}
// The source a bypass param belongs to; -1 for other params.
constexpr int partialLevelIndex(int op, int partial) {
    return partial < kFirstPartials
               ? kPartialBase + op * 2 * kFirstPartials + 2 * partial
               : kLaterPartialBase + op * 2 * (kPartials - kFirstPartials) + 2 * (partial - kFirstPartials);
}
constexpr int partialPhaseIndex(int op, int partial) { return partialLevelIndex(op, partial) + 1; }
constexpr int partialCountIndex(int op) { return kPartialCountBase + op; }
// How many partials a Custom wave plays, from its count param.
inline int partialCount(double value) { return static_cast<int>(std::clamp(std::lround(value), 1L, long{kPartials})); }
constexpr int bypassSourceOf(int index) {
    return index >= kSlotBypassBase && index < kPartialBase ? (index - kSlotBypassBase) / kSlots
           : index >= kStripBypassBase && index < kSlotCurveBase ? kMaxSources + (index - kStripBypassBase) / kSlots
                                                                 : -1;
}
// The slot a bypass param belongs to, for bypassSourceOf()'s source.
constexpr int bypassSlotOf(int index) {
    return (index - (index >= kStripBypassBase ? kStripBypassBase : kSlotBypassBase)) % kSlots;
}
// How much of operator `op` goes into `filter`.
constexpr int sendIndex(int op, int filter) { return paramIndex(op, OpParam::Sends) + filter; }
// How much of filter `from`'s output feeds filter `to`, from == to feeding it back into itself. The links into later
// filters keep the places they had when they were the only ones; those into the filter itself or an earlier one
// follow them.
constexpr int filterLinkIndex(int from, int to) {
    int forward = 0, back = kForwardFilterLinks;
    for (int f = 0; f < kFilters; ++f) {
        for (int t = 0; t < kFilters; ++t) {
            int &link = t > f ? forward : back;
            if (f == from && t == to)
                return kFilterLinkBase + link;
            ++link;
        }
    }
    return -1;
}
static_assert(filterLinkIndex(kFilters - 2, kFilters - 1) == kFilterLinkBase + kForwardFilterLinks - 1 &&
              filterLinkIndex(kFilters - 1, kFilters - 1) == kFilterLinkBase + kFilterLinks - 1);
constexpr int paramIndex(int macro, MacroParam p) {
    return kMacroBase + macro * kMacroParamCount + static_cast<int>(p);
}
constexpr int paramIndex(NoteParam p) { return kNoteBase + static_cast<int>(p); }
constexpr int paramIndex(WheelParam p) { return kWheelBase + static_cast<int>(p); }
constexpr int paramIndex(VelocityParam p) { return kVelocityBase + static_cast<int>(p); }
constexpr int paramIndex(BendParam p) { return kBendBase + static_cast<int>(p); }
// The knob whose value a macro (kMaxSources + macro) or the mod wheel puts out.
constexpr int stripKnobIndex(int source) {
    return isWheel(source) ? paramIndex(GlobalParam::ModWheel) : paramIndex(source - kMaxSources, MacroParam::Value);
}
// The macro a param belongs to; -1 for other params.
constexpr int macroOf(int index) {
    return index >= kMacroBase && index < kNoteBase ? (index - kMacroBase) / kMacroParamCount : -1;
}
// A source's slots, for kMaxSources + macro on a macro's, for kNoteSource the note source's, for kWheelSource
// the mod wheel's, for kVelocitySource the velocity source's and for kBendSource the pitch bend's.
constexpr int slotTargetIndex(int source, int slot) {
    return isBend(source)       ? paramIndex(BendParam::Slots) + 2 * slot
           : isVelocity(source) ? paramIndex(VelocityParam::Slots) + 2 * slot
           : isWheel(source)  ? paramIndex(WheelParam::Slots) + 2 * slot
           : isNote(source)   ? paramIndex(NoteParam::Slots) + 2 * slot
           : isMacro(source)  ? paramIndex(source - kMaxSources, MacroParam::Slots) + 2 * slot
                              : paramIndex(source, SourceParam::Slots) + 2 * slot;
}
constexpr int slotAmountIndex(int source, int slot) { return slotTargetIndex(source, slot) + 1; }
constexpr int slotAuxIndex(int source, int slot) {
    return kSlotAuxBase + (source * kSlots + slot) * kSlotAuxFields + static_cast<int>(SlotAuxParam::Aux);
}
constexpr int slotScaleIndex(int source, int slot) {
    return kSlotAuxBase + (source * kSlots + slot) * kSlotAuxFields + static_cast<int>(SlotAuxParam::Amount);
}
constexpr int slotCurveIndex(int source, int slot) { return kSlotCurveBase + source * kSlots + slot; }
// How the route from node `from` (an operator, then the filters, then the shapers, as the routing graph numbers them)
// into operator `to` acts on it.
constexpr int routeModeIndex(int from, int to) { return kRouteModeBase + from * kOps + to; }
// The node a route's mode param comes from and the operator it goes into, or -1 for other params.
constexpr int routeModeFrom(int index) {
    return index >= kRouteModeBase && index < kParamCount ? (index - kRouteModeBase) / kOps : -1;
}
constexpr int routeModeTo(int index) {
    return index >= kRouteModeBase && index < kParamCount ? (index - kRouteModeBase) % kOps : -1;
}
// The source an aux or Amount param belongs to; -1 for other params.
constexpr int auxSourceOf(int index) {
    return index >= kSlotAuxBase && index < kSlotAuxBase + kAllSources * kSlots * kSlotAuxFields
               ? (index - kSlotAuxBase) / (kSlots * kSlotAuxFields)
               : -1;
}
// The source an Aux param's value names, or -1 for none.
inline int slotAuxSource(double value) {
    const long source = std::lround(value) - 1;
    return source >= 0 && source < kAllSources ? static_cast<int>(source) : -1;
}
inline double slotAuxValue(int source) { return source < 0 ? 0.0 : source + 1.0; }
// The sources that run in each voice (the places, the note source, velocity and the pitch bend), whose output differs
// from voice to voice; the macros and the mod wheel are the same for all.
constexpr bool runsPerVoice(int source) { return !hasKnob(source); }
// The on/off switch of a source.
constexpr int sourceEnabledIndex(int source) { return paramIndex(source, SourceParam::Enabled); }
// The switch that makes a source bipolar, and its value when it does: an LFO's Unipolar off, the Bipolar of an
// envelope (as of `type`), a macro, the note source, the mod wheel, velocity or the pitch bend on.
struct Polarity {
    int index;
    double bipolar;
};
constexpr Polarity polarityOf(int source, SourceType type) {
    return isBend(source)             ? Polarity{paramIndex(BendParam::Bipolar), 1.0}
           : isVelocity(source)       ? Polarity{paramIndex(VelocityParam::Bipolar), 1.0}
           : isWheel(source)          ? Polarity{paramIndex(WheelParam::Bipolar), 1.0}
           : isNote(source)           ? Polarity{paramIndex(NoteParam::Bipolar), 1.0}
           : isMacro(source)          ? Polarity{paramIndex(source - kMaxSources, MacroParam::Bipolar), 1.0}
           : type == SourceType::Lfo  ? Polarity{paramIndex(source, SourceParam::Unipolar), 0.0}
           : type == SourceType::Env  ? Polarity{paramIndex(source, SourceParam::EnvBipolar), 1.0}
                                      : Polarity{paramIndex(source, SourceParam::Bipolar), 1.0};
}
// Whether a source swings its slots' depths across the set value, half below and half above, rather than from it one
// way: LFOs to begin with, the other sources once they are switched to it. `get(index)` returns a param value.
template <class Get>
bool sourceBipolar(const Get &get, int source) {
    const SourceType type = isStripSource(source) ? SourceType::None : sourceType(get(paramIndex(source, SourceParam::Type)));
    const Polarity polarity = polarityOf(source, type);
    return (get(polarity.index) >= 0.5) == (polarity.bipolar >= 0.5);
}

namespace ids {

struct Section {
    int firstIndex;
    int blocks;    // operators, LFOs, ...
    int fields;    // params per block
    clap_id base;  // id of the first param
    clap_id stride; // id distance between blocks
};

constexpr Section kSections[] = {
    {0, 1, kGlobalCount, 0, 100},
    {kOpBase, kOps, kOpParamCount, 100, 50},
    {kMatrixBase, kOps, kOps, 400, 10},
    {kFilterModBase, kFilters, kOps, 500, 10},
    {kFilterBase, kFilters, kFilterParamCount, 800, 20},
    {kFilterLinkBase, 1, kFilterLinks, 900, 100},
    {kShaperBase, kShapers, kShaperParamCount, 600, 50},

    {kSlotBypassBase, kMaxSources, kSlots, 9000, 20},
    {kPartialBase, kOps, 2 * kFirstPartials, 10000, 100},
    {kMacroBase, kMacros, kMacroParamCount, 11000, 40},
    {kNoteBase, 1, kNoteParamCount, 11400, 100},
    {kWheelBase, 1, kWheelParamCount, 11500, 100},
    {kPartialCountBase, 1, kOps, 14000, 100},
    {kLaterPartialBase, kOps, 2 * (kPartials - kFirstPartials), 14100, 100},
    {kVelocityBase, 1, kVelocityParamCount, 11600, 100},
    {kBendBase, 1, kBendParamCount, 11700, 100},
    // A block per source, in source order: sources added later take blocks after these.
    {kSlotAuxBase, kAllSources, kSlots * kSlotAuxFields, 22000, 40},
    // A block per source under the keyboard, in source order, after the places' bypasses.
    {kStripBypassBase, kAllSources - kMaxSources, kSlots, 9320, 20},
    // A block per source, in source order, as the auxes'.
    {kSlotCurveBase, kAllSources, kSlots, 24000, 20},
    // A block per node a route comes from, in the routing graph's order: ids that were never used, between the
    // shapers' and the filters'.
    {kRouteModeBase, kRouteSources, kOps, 700, 10},
    // The chain's places, in order.
    {kFxPositionBase, 1, kFxSlots, 33400, 100},
    // The effects graph: the splits, the wires into the slots and splits, then the wires into the output.
    {kFxSplitBase, kFxSplits, kSplitParamCount, 34000, 20},
    {kFxInputBase, kFxInputNodes, kFxInputs * kFxInputFields, 34200, 20},
    {kFxOutInputBase, 1, kFxOutInputs * kFxInputFields, 35000, 100},
    // Its sums, then the wires into them.
    {kFxSumBase, kFxSums, kSumParamCount, 35100, 20},
    {kFxSumInputBase, kFxSums, kFxSumInputs * kFxInputFields, 35200, 20},
};

// Sources had kFirstSlots slots at first. Their fields keep the ids they had
// then, from 2800 on, and the slots added later have ids of their own, from 6000 on.
constexpr int kFirstSlotsEnd = static_cast<int>(SourceParam::Slots) + 2 * kFirstSlots; // first later slot field
constexpr int kLaterSlotFields = 2 * (kSlots - kFirstSlots);
constexpr int kSourceFields = kSourceParamCount - kLaterSlotFields; // the fields with ids from 2800 on
constexpr clap_id kSourceIdBase = 2800, kSourceIdStride = 40;
constexpr clap_id kLaterSlotIdBase = 6000, kLaterSlotIdStride = 30;
static_assert(kSourceFields <= static_cast<int>(kSourceIdStride) &&
              kLaterSlotFields <= static_cast<int>(kLaterSlotIdStride));

// Sections whose fields ran out of room: a block's fields have ids in one range after another, the first `fields` of
// them from the first range's `base` on, `stride` a block, the next from the second's, and so on; the last range takes
// all the rest. A stride of 0 ends the list.
struct IdRange {
    int fields;
    clap_id base, stride;
};
constexpr int kMaxIdRanges = 5;
constexpr int kRestFields = 1 << 20; // the last range's: every field left
struct SplitSection {
    int firstIndex;
    int blocks;
    int fields;
    IdRange ranges[kMaxIdRanges];
};

constexpr SplitSection kSplitSections[] = {
    // Effect slots: the fields added once the 80 had run out, those added once the next 20 had too, those added once
    // the next 40 had, and those added once the next 20 had. The slots past the kFirstFxSlots the chain had at first
    // have every field in a range of their own.
    {kFxBase,
     kFirstFxSlots,
     kFxParamCount,
     {{80, 1400, 80}, {20, 12000, 20}, {40, 25000, 40}, {20, 36000, 20}, {kRestFields, 36400, 40}}},
    {kFxBase + kFirstFxSlots * kFxParamCount, kFxSlots - kFirstFxSlots, kFxParamCount, {{kRestFields, 26000, 200}}},
    // Freeform LFO shapes and shaper curves: the points past the 16 they had at first (33 for an LFO's).
    {kShapeBase, kMaxSources, kShapeParamCount, {{100, 4000, 100}, {kRestFields, 15000, 100}}},
    {kClipCurveBase, kFirstFxSlots, kShapeParamCount, {{49, 7000, 50}, {kRestFields, 17000, 200}}},
    {kClipCurveBase + kFirstFxSlots * kShapeParamCount, kFxSlots - kFirstFxSlots, kShapeParamCount,
     {{kRestFields, 30000, 200}}},
    {kShaperCurveBase, kShapers, kShapeParamCount, {{49, 13000, 50}, {kRestFields, 21000, 200}}},
};

} // namespace ids

constexpr clap_id idOfIndex(int index) {
    for (const ids::SplitSection &s : ids::kSplitSections) {
        const int local = index - s.firstIndex;
        if (local < 0 || local >= s.blocks * s.fields)
            continue;
        const clap_id block = static_cast<clap_id>(local / s.fields);
        int field = local % s.fields;
        for (const ids::IdRange &r : s.ranges) {
            if (r.stride == 0)
                break;
            if (field < r.fields)
                return r.base + block * r.stride + static_cast<clap_id>(field);
            field -= r.fields;
        }
        return CLAP_INVALID_ID;
    }
    if (const int source = sourceOf(index); source >= 0) {
        const int field = (index - kSourceBase) % kSourceParamCount;
        const int later = field - ids::kFirstSlotsEnd; // among the later slots' fields
        if (later >= 0 && later < ids::kLaterSlotFields)
            return ids::kLaterSlotIdBase + static_cast<clap_id>(source) * ids::kLaterSlotIdStride + later;
        return ids::kSourceIdBase + static_cast<clap_id>(source) * ids::kSourceIdStride +
               static_cast<clap_id>(later < 0 ? field : field - ids::kLaterSlotFields);
    }
    for (const ids::Section &s : ids::kSections) {
        const int local = index - s.firstIndex;
        if (local >= 0 && local < s.blocks * s.fields)
            return s.base + static_cast<clap_id>(local / s.fields) * s.stride + static_cast<clap_id>(local % s.fields);
    }
    return CLAP_INVALID_ID;
}

// -1 for ids this version doesn't know.
constexpr int indexOfId(clap_id id) {
    for (const ids::SplitSection &s : ids::kSplitSections) {
        int start = 0; // the first field of the range
        for (const ids::IdRange &r : s.ranges) {
            if (r.stride == 0)
                break;
            if (id >= r.base && id < r.base + static_cast<clap_id>(s.blocks) * r.stride) {
                const int block = static_cast<int>((id - r.base) / r.stride);
                const int field = static_cast<int>((id - r.base) % r.stride);
                return field < std::min(r.fields, s.fields - start) ? s.firstIndex + block * s.fields + start + field
                                                                    : -1;
            }
            start += std::min(r.fields, s.fields);
        }
    }
    if (id >= ids::kSourceIdBase && id < ids::kSourceIdBase + kMaxSources * ids::kSourceIdStride) {
        const int source = static_cast<int>((id - ids::kSourceIdBase) / ids::kSourceIdStride);
        const int field = static_cast<int>((id - ids::kSourceIdBase) % ids::kSourceIdStride);
        if (field >= ids::kSourceFields)
            return -1;
        return kSourceBase + source * kSourceParamCount +
               (field < ids::kFirstSlotsEnd ? field : field + ids::kLaterSlotFields);
    }
    if (id >= ids::kLaterSlotIdBase && id < ids::kLaterSlotIdBase + kMaxSources * ids::kLaterSlotIdStride) {
        const int source = static_cast<int>((id - ids::kLaterSlotIdBase) / ids::kLaterSlotIdStride);
        const int field = static_cast<int>((id - ids::kLaterSlotIdBase) % ids::kLaterSlotIdStride);
        return field < ids::kLaterSlotFields ? kSourceBase + source * kSourceParamCount + ids::kFirstSlotsEnd + field
                                             : -1;
    }
    for (const ids::Section &s : ids::kSections) {
        if (id < s.base || id >= s.base + static_cast<clap_id>(s.blocks) * s.stride)
            continue;
        const int block = static_cast<int>((id - s.base) / s.stride);
        const int field = static_cast<int>((id - s.base) % s.stride);
        return field < s.fields ? s.firstIndex + block * s.fields + field : -1;
    }
    return -1;
}

// Over the params from `from` to before `to`, which a static_assert takes a part at a time: all at once would take
// the compiler more steps than it allows an expression.
constexpr bool idsRoundTrip(int from, int to) {
    for (const ids::Section &s : ids::kSections)
        if (s.fields > static_cast<int>(s.stride))
            return false;
    for (const ids::SplitSection &s : ids::kSplitSections) {
        int start = 0;
        for (const ids::IdRange &r : s.ranges) {
            if (r.stride == 0)
                break;
            if (std::clamp(s.fields - start, 0, r.fields) > static_cast<int>(r.stride))
                return false;
            start += std::min(r.fields, s.fields);
        }
        if (start < s.fields) // fields past the last range
            return false;
    }
    for (int i = from; i < to && i < kParamCount; ++i)
        if (indexOfId(idOfIndex(i)) != i)
            return false;
    return true;
}
// (MSVC allows an expression 1,048,576 steps, fewer than GCC, so the parts are small.)
static_assert(kParamCount <= 8 * 3000);
static_assert(idsRoundTrip(0, 3000), "param id layout overlaps");
static_assert(idsRoundTrip(3000, 6000), "param id layout overlaps");
static_assert(idsRoundTrip(6000, 9000), "param id layout overlaps");
static_assert(idsRoundTrip(9000, 12000), "param id layout overlaps");
static_assert(idsRoundTrip(12000, 15000), "param id layout overlaps");
static_assert(idsRoundTrip(15000, 18000), "param id layout overlaps");
static_assert(idsRoundTrip(18000, 21000), "param id layout overlaps");
static_assert(idsRoundTrip(21000, 24000), "param id layout overlaps");
// Projects store params by id: effect fields keep theirs as later ones come (the 80th was the last in the
// first section),
static_assert(idOfIndex(paramIndex(1, FxParam::ReverbType)) == 1400 + 80 + 79 &&
              idOfIndex(paramIndex(1, FxParam::PhaserStereo)) == 12000 + 20);
// Projects store params by id: the fields sources had before the later slots keep theirs.
static_assert(idOfIndex(slotAmountIndex(1, kFirstSlots - 1)) == 2849 && idOfIndex(paramIndex(1, SourceParam::Shape)) == 2850 &&
              idOfIndex(slotTargetIndex(1, kFirstSlots)) == 6030);
// And the partials Custom waves had at first keep theirs.
static_assert(idOfIndex(partialPhaseIndex(1, kFirstPartials - 1)) == 10163 &&
              idOfIndex(partialLevelIndex(1, kFirstPartials)) == 14200);
// And the points LFO shapes had at first (the 33rd's fields the last of the first section), and shaper curves.
static_assert(idOfIndex(shapePointIndex(1, 15, PointField::Curve)) == 4148 &&
              idOfIndex(shapePointIndex(1, 32, PointField::Curve)) == 4199 &&
              idOfIndex(shapePointIndex(1, 33, PointField::X)) == 15100);
static_assert(idOfIndex(shapeFieldIndex(clipCurveCountIndex(1), 15, PointField::Curve)) == 7098 &&
              idOfIndex(shapeFieldIndex(clipCurveCountIndex(1), 16, PointField::X)) == 17200 &&
              idOfIndex(shapeFieldIndex(shaperCurveCountIndex(1), 15, PointField::Curve)) == 13098 &&
              idOfIndex(shapeFieldIndex(shaperCurveCountIndex(1), 16, PointField::X)) == 21200);

// --- Pinned ids. Projects, automation and preset files store params by id, so a released id never changes. The
// round trip above only checks that the layout holds together; these pin each section's ids as they stand: its
// first field and its last one so far, at its first and last block. If one fails, a change has renumbered stored
// params (a count below changed, or a field went in before others or moved): undo it, and append new fields at the
// end of their enum instead; a section out of room goes on in a later id range, as the effects' fields past the
// 80th do. Appending a field leaves these as they are.

// The counts the layout comes from. Each one moves the fields laid out after it: OpParam's ShaperSends after its
// Sends (kFilters), ShaperParam's Mods and Out (kFilters, kOps), the fields after the slots of sources, macros, the
// note source and the mod wheel (kSlots), and blocks past the room their section has.
static_assert(kOps == 6 && kMacros == 8 && kMaxSources == 16 && kSlots == 16 && kFirstSlots == 4 && kFilters == 2 &&
                  kShapers == 2 && kFxSlots == 32 && kFirstFxSlots == 16 && kMaxShapePoints == 64 && kMaxNotePoints == 16 &&
                  kPartials == 64 && kFirstPartials == 32,
              "a count the param ids are laid out by changed: see Pinned ids");
static_assert(idOfIndex(paramIndex(GlobalParam::Volume)) == 0 && idOfIndex(paramIndex(GlobalParam::ModWheel)) == 11 &&
                  idOfIndex(paramIndex(GlobalParam::PitchBend)) == 12,
              "global param ids moved");
static_assert(idOfIndex(kFxPositionBase) == 33400 && idOfIndex(kFxPositionBase + kFxSlots - 1) == 33431,
              "chain order param ids moved");
static_assert(idOfIndex(paramIndex(0, SplitParam::Used)) == 34000 &&
                  idOfIndex(paramIndex(kFxSplits - 1, SplitParam::Slope)) == 34064 &&
                  idOfIndex(fxInputIndex(0, 0, FxInputField::From)) == 34200 &&
                  idOfIndex(fxInputIndex(kFxInputNodes - 1, kFxInputs - 1, FxInputField::Level)) == 34907 &&
                  idOfIndex(fxInputIndex(kFxOutNode, 0, FxInputField::From)) == 35000 &&
                  idOfIndex(fxInputIndex(kFxOutNode, kFxOutInputs - 1, FxInputField::Level)) == 35015 &&
                  kFxSplits == 4 && kFxBands == 3 && kFxInputs == 4 && kFxOutInputs == 8,
              "effects graph param ids moved");
static_assert(idOfIndex(paramIndex(0, SumParam::Used)) == 35100 && idOfIndex(paramIndex(kFxSums - 1, SumParam::SoloMid)) == 35163 &&
                  idOfIndex(fxInputIndex(fxSumNode(0), 0, FxInputField::From)) == 35200 &&
                  idOfIndex(fxInputIndex(fxSumNode(kFxSums - 1), kFxSumInputs - 1, FxInputField::Level)) == 35275 &&
                  kFxSums == 4 && kFxSumInputs == 8,
              "effects graph sum ids moved");
// A wire's From is stored by number: the input, the slots, then the splits' bands.
static_assert(fxPortValue(kFxPortIn) == 1.0 && fxPortValue(fxSlotPort(kFxSlots - 1)) == 33.0 &&
                  fxPortValue(fxBandPort(0, 0)) == 34.0 && fxPortValue(fxBandPort(kFxSplits - 1, kFxBands - 1)) == 45.0 &&
                  fxPortValue(fxSumPort(0)) == 46.0 && fxPortValue(fxSumPort(kFxSums - 1)) == 49.0,
              "the effects graph's port numbers moved");
static_assert(idOfIndex(paramIndex(0, OpParam::RetiredCoarse)) == 100 && idOfIndex(paramIndex(0, OpParam::Transpose)) == 120 &&
                  idOfIndex(sendIndex(0, 0)) == 121 && idOfIndex(shaperSendIndex(0, 0)) == 123 &&
                  idOfIndex(shaperSendIndex(kOps - 1, kShapers - 1)) == 374 &&
                  idOfIndex(paramIndex(0, OpParam::Fixed)) == 125 &&
                  idOfIndex(paramIndex(kOps - 1, OpParam::FixedMult)) == 377 &&
                  idOfIndex(paramIndex(0, OpParam::RetiredVelocity)) == 108 &&
                  idOfIndex(paramIndex(kOps - 1, OpParam::Phase)) == 378 &&
                  idOfIndex(paramIndex(kOps - 1, OpParam::Ratio)) == 379 &&
                  idOfIndex(paramIndex(kOps - 1, OpParam::Pan)) == 380 && kOpParamCount <= 50,
              "operator param ids moved");
static_assert(idOfIndex(matrixIndex(0, 0)) == 400 && idOfIndex(matrixIndex(kOps - 1, kOps - 1)) == 455,
              "matrix param ids moved");
static_assert(idOfIndex(filterModIndex(0, 0)) == 500 && idOfIndex(filterModIndex(kFilters - 1, kOps - 1)) == 515,
              "filter modulation param ids moved");
static_assert(idOfIndex(paramIndex(0, FilterParam::Type)) == 800 &&
                  idOfIndex(paramIndex(0, FilterParam::Slope)) == 807 && idOfIndex(filterShaperIndex(0, 0)) == 808 &&
                  idOfIndex(filterShaperIndex(kFilters - 1, kShapers - 1)) == 829 &&
                  idOfIndex(paramIndex(0, FilterParam::Mix)) == 810 &&
                  idOfIndex(paramIndex(kFilters - 1, FilterParam::Mix)) == 830 && kFilterParamCount <= 20,
              "filter param ids moved");
static_assert(kFilterLinks == 4 && idOfIndex(filterLinkIndex(0, 1)) == 900 && idOfIndex(filterLinkIndex(0, 0)) == 901 &&
                  idOfIndex(filterLinkIndex(1, 0)) == 902 && idOfIndex(filterLinkIndex(1, 1)) == 903,
              "filter link param ids moved");
static_assert(idOfIndex(paramIndex(0, ShaperParam::Enabled)) == 600 && idOfIndex(shaperFilterIndex(0, 0)) == 606 &&
                  idOfIndex(shaperModIndex(0, 0)) == 608 && idOfIndex(paramIndex(0, ShaperParam::Out)) == 614 &&
                  idOfIndex(paramIndex(kShapers - 1, ShaperParam::Symmetric)) == 665 &&
                  idOfIndex(shaperShaperIndex(0, 0)) == 616 &&
                  idOfIndex(shaperShaperIndex(kShapers - 1, kShapers - 1)) == 667,
              "shaper param ids moved");
static_assert(idOfIndex(paramIndex(0, SourceParam::Type)) == 2800 &&
                  idOfIndex(slotAmountIndex(kMaxSources - 1, kFirstSlots - 1)) == 3409 &&
                  idOfIndex(paramIndex(0, SourceParam::Shape)) == 2810 &&
                  idOfIndex(paramIndex(kMaxSources - 1, SourceParam::EnvBipolar)) == 3427 &&
                  idOfIndex(paramIndex(0, SourceParam::Phase)) == 2828 &&
                  idOfIndex(paramIndex(kMaxSources - 1, SourceParam::Phase)) == 3428 &&
                  idOfIndex(paramIndex(0, SourceParam::Slew)) == 2829 &&
                  idOfIndex(paramIndex(kMaxSources - 1, SourceParam::MidiInput)) == 3432 &&
                  idOfIndex(paramIndex(0, SourceParam::Anchor)) == 2833 &&
                  idOfIndex(paramIndex(kMaxSources - 1, SourceParam::Anchor)) == 3433 &&
                  idOfIndex(slotTargetIndex(0, kFirstSlots)) == 6000 &&
                  idOfIndex(slotAmountIndex(kMaxSources - 1, kSlots - 1)) == 6473,
              "source param ids moved");
static_assert(idOfIndex(paramIndex(0, FxParam::Type)) == 1400 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::ReverbType)) == 2679 &&
                  idOfIndex(paramIndex(0, FxParam::PhaserStereo)) == 12000 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::ShaperSymmetric)) == 12318 &&
                  idOfIndex(paramIndex(0, FxParam::MdLowFreq)) == 12019 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::MdLowFreq)) == 12319 &&
                  idOfIndex(paramIndex(0, FxParam::MdHighFreq)) == 25000 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::MdRms)) == 25623 &&
                  idOfIndex(paramIndex(0, FxParam::PeqFreq3)) == 25024 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::PeqOn3)) == 25630 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::PeqAmount)) == 25631 &&
                  idOfIndex(paramIndex(0, FxParam::PhaserNegative)) == 25032 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::FlangerNegative)) == 25633 &&
                  idOfIndex(paramIndex(kFirstFxSlots, FxParam::Type)) == 26000 &&
                  idOfIndex(paramIndex(kFirstFxSlots, FxParam::PhaserStereo)) == 26080 &&
                  idOfIndex(paramIndex(kFxSlots - 1, FxParam::PeqAmount)) == 29131 &&
                  idOfIndex(paramIndex(kFxSlots - 1, FxParam::FlangerNegative)) == 29133 &&
                  idOfIndex(paramIndex(0, FxParam::AmpModel)) == 25034 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::AmpPresence)) == 25639 &&
                  idOfIndex(paramIndex(0, FxParam::AmpVolume)) == 36000 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::AmpOversampling)) == 36309 &&
                  idOfIndex(paramIndex(kFirstFxSlots, FxParam::AmpModel)) == 26134 &&
                  idOfIndex(paramIndex(kFxSlots - 1, FxParam::AmpOversampling)) == 29149 &&
                  idOfIndex(paramIndex(0, FxParam::UtilityGain)) == 36010 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::UtilityMonoBass)) == 36313 &&
                  idOfIndex(paramIndex(kFirstFxSlots, FxParam::UtilityGain)) == 26150 &&
                  idOfIndex(paramIndex(kFxSlots - 1, FxParam::UtilityMonoBass)) == 29153 &&
                  idOfIndex(paramIndex(0, FxParam::ChorusRate)) == 36014 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::ChorusMix)) == 36319 &&
                  idOfIndex(paramIndex(0, FxParam::ChorusVoices)) == 36400 &&
                  idOfIndex(paramIndex(kFirstFxSlots - 1, FxParam::ChorusVoices)) == 37000 &&
                  idOfIndex(paramIndex(kFirstFxSlots, FxParam::ChorusRate)) == 26154 &&
                  idOfIndex(paramIndex(kFxSlots - 1, FxParam::ChorusVoices)) == 29160,
              "effect slot param ids moved");
static_assert(idOfIndex(shapeCountIndex(0)) == 4000 &&
                  idOfIndex(shapePointIndex(kMaxSources - 1, 32, PointField::Curve)) == 5599 &&
                  idOfIndex(shapePointIndex(0, 33, PointField::X)) == 15000 &&
                  idOfIndex(shapePointIndex(kMaxSources - 1, kMaxShapePoints - 1, PointField::Curve)) == 16592,
              "freeform LFO shape param ids moved");
static_assert(idOfIndex(clipCurveCountIndex(0)) == 7000 &&
                  idOfIndex(shapeFieldIndex(clipCurveCountIndex(kFirstFxSlots - 1), 15, PointField::Curve)) == 7798 &&
                  idOfIndex(shapeFieldIndex(clipCurveCountIndex(0), 16, PointField::X)) == 17000 &&
                  idOfIndex(shapeFieldIndex(clipCurveCountIndex(kFirstFxSlots - 1), kMaxShapePoints - 1,
                                            PointField::Curve)) == 20143 &&
                  idOfIndex(clipCurveCountIndex(kFirstFxSlots)) == 30000 &&
                  idOfIndex(shapeFieldIndex(clipCurveCountIndex(kFxSlots - 1), kMaxShapePoints - 1,
                                            PointField::Curve)) == 33192,
              "shaper effect curve param ids moved");
static_assert(idOfIndex(slotBypassIndex(0, 0)) == 9000 &&
                  idOfIndex(slotBypassIndex(kMaxSources - 1, kSlots - 1)) == 9315,
              "slot bypass param ids moved");
static_assert(idOfIndex(slotBypassIndex(kMaxSources, 0)) == 9320 &&
                  idOfIndex(slotBypassIndex(kNoteSource, 0)) == 9480 &&
                  idOfIndex(slotBypassIndex(kWheelSource, 0)) == 9500 &&
                  idOfIndex(slotBypassIndex(kVelocitySource, kSlots - 1)) == 9535 &&
                  idOfIndex(slotBypassIndex(kBendSource, kSlots - 1)) == 9555,
              "strip source bypass param ids moved");
static_assert(idOfIndex(partialLevelIndex(0, 0)) == 10000 &&
                  idOfIndex(partialPhaseIndex(kOps - 1, kFirstPartials - 1)) == 10563,
              "Custom wave partial param ids moved");
static_assert(idOfIndex(paramIndex(0, MacroParam::Value)) == 11000 &&
                  idOfIndex(paramIndex(kMacros - 1, MacroParam::Bipolar)) == 11313,
              "macro param ids moved");
static_assert(idOfIndex(paramIndex(NoteParam::Slots)) == 11400 && idOfIndex(paramIndex(NoteParam::Low)) == 11432 &&
                  idOfIndex(paramIndex(NoteParam::Curve)) == 11434 &&
                  idOfIndex(shapeFieldIndex(paramIndex(NoteParam::Curve), kMaxNotePoints - 1, PointField::Curve)) ==
                      11482 &&
                  idOfIndex(paramIndex(NoteParam::Bipolar)) == 11483,
              "note source param ids moved");
static_assert(idOfIndex(paramIndex(WheelParam::Slots)) == 11500 && idOfIndex(paramIndex(WheelParam::Bipolar)) == 11532,
              "mod wheel param ids moved");
static_assert(idOfIndex(paramIndex(VelocityParam::Slots)) == 11600 &&
                  idOfIndex(paramIndex(VelocityParam::Curve)) == 11632 &&
                  idOfIndex(shapeFieldIndex(paramIndex(VelocityParam::Curve), kMaxNotePoints - 1, PointField::Curve)) ==
                      11680 &&
                  idOfIndex(paramIndex(VelocityParam::Bipolar)) == 11681,
              "velocity source param ids moved");
static_assert(idOfIndex(paramIndex(BendParam::Slots)) == 11700 && idOfIndex(paramIndex(BendParam::Bipolar)) == 11732,
              "pitch bend source param ids moved");
static_assert(idOfIndex(slotAuxIndex(0, 0)) == 22000 && idOfIndex(slotScaleIndex(0, 0)) == 22001 &&
                  idOfIndex(slotScaleIndex(0, kSlots - 1)) == 22031 &&
                  idOfIndex(slotAuxIndex(kVelocitySource, 0)) == 23040 &&
                  idOfIndex(slotScaleIndex(kVelocitySource, kSlots - 1)) == 23071 &&
                  idOfIndex(slotAuxIndex(kBendSource, 0)) == 23080 &&
                  idOfIndex(slotScaleIndex(kBendSource, kSlots - 1)) == 23111,
              "slot aux param ids moved");
static_assert(idOfIndex(slotCurveIndex(0, 0)) == 24000 && idOfIndex(slotCurveIndex(0, kSlots - 1)) == 24015 &&
                  idOfIndex(slotCurveIndex(kVelocitySource, kSlots - 1)) == 24535 &&
                  idOfIndex(slotCurveIndex(kBendSource, kSlots - 1)) == 24555,
              "slot curve param ids moved");
static_assert(idOfIndex(routeModeIndex(0, 0)) == 700 && idOfIndex(routeModeIndex(1, 0)) == 710 &&
                  idOfIndex(routeModeIndex(kRouteSources - 1, kOps - 1)) == 795,
              "route mode param ids moved");
static_assert(idOfIndex(shaperCurveCountIndex(0)) == 13000 &&
                  idOfIndex(shapeFieldIndex(shaperCurveCountIndex(kShapers - 1), 15, PointField::Curve)) == 13098 &&
                  idOfIndex(shapeFieldIndex(shaperCurveCountIndex(0), 16, PointField::X)) == 21000 &&
                  idOfIndex(shapeFieldIndex(shaperCurveCountIndex(kShapers - 1), kMaxShapePoints - 1,
                                            PointField::Curve)) == 21343,
              "voice shaper curve param ids moved");
static_assert(idOfIndex(partialCountIndex(0)) == 14000 && idOfIndex(partialCountIndex(kOps - 1)) == 14005 &&
                  idOfIndex(partialLevelIndex(0, kFirstPartials)) == 14100 &&
                  idOfIndex(partialPhaseIndex(kOps - 1, kPartials - 1)) == 14663,
              "Custom wave partial count and later partial param ids moved");

// Waves, filter types, effect types, source types, LFO shapes, shaper curves, EQ band shapes and reverb algorithms
// are stored by number too: each member keeps its number. Append a new one just before Count, and here.
template <class E>
constexpr bool numberedInOrder(std::initializer_list<E> members) { // each member's number is its place in the list
    int place = 0;
    for (E member : members)
        if (static_cast<int>(member) != place++)
            return false;
    return true;
}
static_assert(numberedInOrder({OpWave::Sine, OpWave::Triangle, OpWave::Saw, OpWave::Square, OpWave::Pulse,
                               OpWave::HalfSine, OpWave::AbsSine, OpWave::Noise, OpWave::Custom, OpWave::AltSine,
                               OpWave::Organ, OpWave::Vowel, OpWave::Count}),
              "OpWave is stored by number: only append");
static_assert(numberedInOrder({FilterType::LowPass, FilterType::HighPass, FilterType::BandPass, FilterType::Notch,
                               FilterType::Comb, FilterType::Count}),
              "FilterType is stored by number: only append");
static_assert(numberedInOrder({FxType::None, FxType::Shaper, FxType::Delay, FxType::Eq, FxType::ParamEq,
                               FxType::Filter, FxType::Compressor, FxType::MultiComp, FxType::Reverb, FxType::Phaser,
                               FxType::Flanger, FxType::FreqShifter, FxType::Disperser, FxType::Widener,
                               FxType::MultiDyn, FxType::Amp, FxType::Utility, FxType::Chorus, FxType::Count}),
              "FxType is stored by number: only append");
static_assert(numberedInOrder({AmpModel::Clean, AmpModel::Blues, AmpModel::Crunch, AmpModel::Lead, AmpModel::Heavy,
                               AmpModel::Bass, AmpModel::Fuzz, AmpModel::Count}),
              "AmpModel is stored by number: only append");
static_assert(numberedInOrder({AmpCab::Open1x12, AmpCab::Open2x12, AmpCab::Closed4x12, AmpCab::Bass4x10,
                               AmpCab::Bass1x15, AmpCab::Count}),
              "AmpCab is stored by number: only append");
static_assert(numberedInOrder({AmpMic::Dynamic, AmpMic::Condenser, AmpMic::Ribbon, AmpMic::Count}),
              "AmpMic is stored by number: only append");
static_assert(numberedInOrder({MidiInput::Cc, MidiInput::Pressure, MidiInput::Slide, MidiInput::Count}),
              "MidiInput is stored by number: only append");
static_assert(numberedInOrder({SourceType::None, SourceType::Lfo, SourceType::Env, SourceType::Random,
                               SourceType::Midi, SourceType::Count}),
              "SourceType is stored by number: only append");
static_assert(numberedInOrder({SlotAuxParam::Aux, SlotAuxParam::Amount, SlotAuxParam::Count}),
              "SlotAuxParam is stored by number: only append");
static_assert(numberedInOrder({LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Square, LfoShape::Random,
                               LfoShape::Custom, LfoShape::Count}),
              "LfoShape is stored by number: only append");
static_assert(numberedInOrder({ClipMode::Hard, ClipMode::Soft, ClipMode::Asymmetric, ClipMode::Cubic, ClipMode::Gentle,
                               ClipMode::SineFold, ClipMode::Fold, ClipMode::Custom, ClipMode::Count}),
              "ClipMode is stored by number: only append");
static_assert(numberedInOrder({PeqType::Peak, PeqType::LowShelf, PeqType::HighShelf, PeqType::LowCut, PeqType::HighCut,
                               PeqType::Notch, PeqType::Count}),
              "PeqType is stored by number: only append");
static_assert(numberedInOrder({ReverbType::Hall, ReverbType::Room, ReverbType::Plate, ReverbType::Shimmer,
                               ReverbType::Count}),
              "ReverbType is stored by number: only append");
static_assert(numberedInOrder({RouteMode::Phase, RouteMode::LinearFm, RouteMode::ExpFm, RouteMode::Ring,
                               RouteMode::Count}),
              "RouteMode is stored by number: only append");

// The highest id in use; sections aren't in id order.
constexpr clap_id maxParamId() {
    clap_id highest = 0;
    for (const ids::Section &s : ids::kSections)
        highest = std::max(highest, s.base + static_cast<clap_id>(s.blocks - 1) * s.stride +
                                        static_cast<clap_id>(s.fields - 1));
    for (const ids::SplitSection &s : ids::kSplitSections) {
        const clap_id block = static_cast<clap_id>(s.blocks - 1);
        int start = 0;
        for (const ids::IdRange &r : s.ranges) {
            if (r.stride == 0 || start >= s.fields)
                break;
            highest = std::max(highest, r.base + block * r.stride +
                                            static_cast<clap_id>(std::min(r.fields, s.fields - start) - 1));
            start += r.fields;
        }
    }
    highest = std::max(highest, ids::kSourceIdBase + (kMaxSources - 1) * ids::kSourceIdStride + ids::kSourceFields - 1);
    return std::max(highest,
                    ids::kLaterSlotIdBase + (kMaxSources - 1) * ids::kLaterSlotIdStride + ids::kLaterSlotFields - 1);
}
constexpr clap_id kMaxParamId = maxParamId();

// --- Value mappings. Times and LFO rates are stored as 0..1 on a log curve, so
// host knobs and host modulation get useful resolution at the short end.

constexpr double kMinTime = 0.001;
constexpr double kMaxTime = 20.0;
// The longest attack, hold, decay and release of an envelope (operators' and mod envelopes'): on the same curve as
// the other times, so their stored values keep their meaning, but their range stops short of its top. A state or
// preset with a longer one loads it as this.
constexpr double kMaxEnvTime = 10.0;
constexpr double kMinRate = 0.02;
constexpr double kMaxRate = 100.0;
constexpr double kMinCutoff = 20.0;
constexpr double kMaxCutoff = 20000.0;

inline double timeFromNorm(double x) { return kMinTime * std::pow(kMaxTime / kMinTime, x); }
inline double normFromTime(double seconds) { return std::log(seconds / kMinTime) / std::log(kMaxTime / kMinTime); }
// Where kMaxEnvTime sits on that curve: the top of the envelopes' attack, hold, decay and release.
inline const double kMaxEnvNorm = normFromTime(kMaxEnvTime);
// The longest glide, on the same curve; a state or preset with a longer one loads it as this.
constexpr double kMaxGlideTime = 6.0;
inline const double kMaxGlideNorm = normFromTime(kMaxGlideTime);
// The norm a typed time or rate at the shortest or slowest shown is kept at: above 0, which is Off or still, and
// shown as that shortest time or slowest rate.
constexpr double kLeastNorm = 1e-6;
// An envelope's attack, decay and release go down to 0 ms, below the 1 ms the Time curve starts at: on a straight
// line from 0 ms at kMinEnvNorm to 1 ms at 0, as steep as the curve where it starts, so the knob turns through it
// evenly. Norms from 0 up keep their meaning on the Time curve (0 is 1 ms, as it always was).
inline const double kMinEnvNorm = -1.0 / std::log(kMaxTime / kMinTime);
inline double envTimeFromNorm(double x) {
    return x >= 0.0 ? timeFromNorm(x) : kMinTime * std::max(1.0 - x / kMinEnvNorm, 0.0);
}
inline double normFromEnvTime(double seconds) {
    return seconds >= kMinTime ? normFromTime(seconds) : (1.0 - std::max(seconds, 0.0) / kMinTime) * kMinEnvNorm;
}
inline double rateFromNorm(double x) { return kMinRate * std::pow(kMaxRate / kMinRate, x); }
inline double normFromRate(double hz) { return std::log(hz / kMinRate) / std::log(kMaxRate / kMinRate); }
// The phaser's and flanger's rates: an LFO's curve, but at its bottom the sweep stands still.
inline double sweepRateHz(double norm) { return norm <= 0.0 ? 0.0 : rateFromNorm(norm); }
inline double cutoffFromNorm(double x) { return kMinCutoff * std::pow(kMaxCutoff / kMinCutoff, x); }
inline double normFromCutoff(double hz) { return std::log(hz / kMinCutoff) / std::log(kMaxCutoff / kMinCutoff); }
// The utility's Mono Bass knob: off at its bottom, else on the cutoff curve, up to kMaxUtilityMonoBass.
inline double utilityMonoBassHz(double norm) { return norm <= 0.0 ? 0.0 : cutoffFromNorm(norm); }
// A split's crossovers in Hz from its Low and High params, High kept a third of an octave above Low, as the three-band
// dynamics' are.
inline std::array<double, 2> splitCrossovers(double lowNorm, double highNorm) {
    const double low = cutoffFromNorm(std::clamp(lowNorm, 0.0, 1.0));
    return {low, std::max(cutoffFromNorm(std::clamp(highNorm, 0.0, 1.0)), low * std::pow(2.0, 1.0 / 3.0))};
}
// Pitches in semitones (MIDI numbering, 69 is A4 at 440 Hz) and frequencies.
inline double pitchOfHz(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double hzOfPitch(double pitch) { return 440.0 * std::exp2((pitch - 69.0) / 12.0); }
// The top of the note source's range: the pitch at the cutoff knobs' top, 20 kHz, D#10 +8, past G9, the highest
// MIDI note (C-1, the lowest, is its bottom).
inline const double kMaxNotePitch = pitchOfHz(kMaxCutoff);
// Where a pitch sits on the cutoff knobs' curve, kept within 0..1 (20 Hz to 20 kHz): what the note source puts
// out for it to take a cutoff at 0, modulated by 100 %, right onto the note.
inline double cutoffNormOfPitch(double pitch) { return std::clamp(normFromCutoff(hzOfPitch(pitch)), 0.0, 1.0); }
// Where the filters' cutoffs (the voices' and the filter effect's) start: the knobs' middle, about 632 Hz. A bipolar
// note source swings its targets about the middle of what it puts out, so there, modulated by 100 % by it, a cutoff
// sits right on the note.
constexpr double kDefaultCutoff = 0.5;
// The delay line holds this much; free delay times go up to kMaxFreeDelay.
constexpr double kMaxDelay = 4.0;
constexpr double kMaxFreeDelay = 2.0;
// The frequency shifter's range, 0..1 for kMinShiftRange..kMaxShiftRange Hz on a log curve, and the shift (Hz) its
// Shift (-1..1) takes of it: a narrow range gives the beating, phasing kind the whole travel of the Shift knob.
constexpr double kMinShiftRange = 1.0;
constexpr double kMaxShiftRange = 5000.0;
inline double shiftRangeFromNorm(double x) { return kMinShiftRange * std::pow(kMaxShiftRange / kMinShiftRange, x); }
inline double normFromShiftRange(double hz) {
    return std::log(std::clamp(hz, kMinShiftRange, kMaxShiftRange) / kMinShiftRange) /
           std::log(kMaxShiftRange / kMinShiftRange);
}
inline double shiftHz(double shift, double rangeNorm) { return shift * shiftRangeFromNorm(rangeNorm); }
// A fixed operator's frequency: Freq, 0..1 for kMinFixedFreq..kMaxFixedFreq Hz on a log curve, times its Mult, one
// of kFixedMults (stored by number: only append).
constexpr double kMinFixedFreq = 1.0;
constexpr double kMaxFixedFreq = 2000.0;
constexpr double kFixedMults[] = {0.001, 0.01, 0.1, 1.0, 10.0};
constexpr int kFixedMultCount = static_cast<int>(std::size(kFixedMults));
static_assert(kFixedMultCount == 5 && kFixedMults[3] == 1.0, "the stored Mult list changed: only append to it");
inline double fixedFreqFromNorm(double x) { return kMinFixedFreq * std::pow(kMaxFixedFreq / kMinFixedFreq, x); }
inline double normFromFixedFreq(double hz) {
    return std::log(std::clamp(hz, kMinFixedFreq, kMaxFixedFreq) / kMinFixedFreq) /
           std::log(kMaxFixedFreq / kMinFixedFreq);
}
inline double fixedMult(double value) {
    return kFixedMults[std::clamp(static_cast<int>(std::lround(value)), 0, kFixedMultCount - 1)];
}
inline double fixedHz(double freqNorm, double mult) { return fixedFreqFromNorm(freqNorm) * fixedMult(mult); }
// The operator whose Fixed switch the param is, or -1.
inline int fixedSwitchOp(int index) {
    for (int op = 0; op < kOps; ++op)
        if (index == paramIndex(op, OpParam::Fixed))
            return op;
    return -1;
}
// The old Coarse was stepped 0..32, 0 standing for a ratio of 0.5; states before Ratio load through it.
inline double coarseRatio(double coarse) {
    const long c = std::lround(coarse);
    return c <= 0 ? 0.5 : static_cast<double>(c);
}

// An operator's Ratio: 0..1 on a log curve from 0.5 to 64 times the note, so a modulation's depth is the same
// interval wherever the ratio is set, and 1 to 2 takes as much of a host's knob as 32 to 64.
constexpr double kMinRatio = 0.5;
constexpr double kMaxRatio = 64.0;
// Modulation takes it an octave further, to 128: past the top of its range by a seventh of it, the share of the
// curve's seven octaves an octave is. The ratio set stays within 0.5 to 64.
constexpr double kMaxModRatio = 128.0;
constexpr double kRatioModNorm = 8.0 / 7.0;
static_assert(kMaxRatio / kMinRatio == 128.0 && kMaxModRatio == 2.0 * kMaxRatio,
              "kRatioModNorm is the norm an octave past kMaxRatio on a seven-octave curve");
inline double ratioFromNorm(double x) { return kMinRatio * std::pow(kMaxRatio / kMinRatio, x); }
inline double normFromRatio(double ratio) {
    return std::clamp(std::log(std::max(ratio, 1e-9) / kMinRatio) / std::log(kMaxRatio / kMinRatio), 0.0, 1.0);
}
// The ratio as the editor shows and sets it: in hundredths, 50 to 6400.
inline long ratioHundredths(double norm) { return std::clamp(std::lround(ratioFromNorm(norm) * 100.0), 50L, 6400L); }

// Note values for tempo sync, from 4/1 dotted down to a 1/64 triplet, slowest
// first. Stored in projects by number.
constexpr int kDivisions = 27;
// Length of a division in quarter notes: each power of two comes dotted, straight and as a triplet.
inline double divisionBeats(int division) {
    constexpr double kFactors[] = {1.5, 1.0, 2.0 / 3.0};
    division = division < 0 ? 0 : (division >= kDivisions ? kDivisions - 1 : division);
    return 16.0 / static_cast<double>(1 << (division / 3)) * kFactors[division % 3];
}
inline double divisionSeconds(double division, double tempo) {
    return divisionBeats(static_cast<int>(std::lround(division))) * 60.0 / tempo;
}
// How fast the phaser, flanger or chorus (`type`) in `slot` sweeps (Hz): at its Rate, or, a phaser or flanger
// synced, a cycle per note value at `tempo` BPM. `get(index)` returns a param value.
template <class Get>
double sweepHz(const Get &get, int slot, FxType type, double tempo) {
    auto at = [&](FxParam f) { return get(paramIndex(slot, f)); };
    if (type == FxType::Chorus) // never synced
        return sweepRateHz(at(FxParam::ChorusRate));
    const bool phaser = type == FxType::Phaser;
    if (at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync) >= 0.5)
        return 1.0 / divisionSeconds(at(phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision), tempo);
    return sweepRateHz(at(phaser ? FxParam::PhaserRate : FxParam::FlangerRate));
}

// The permutation of 0..N-1 at `rank` in lexicographic order, and back.
template <int N>
constexpr std::array<int, N> permutationAt(int64_t rank) {
    std::array<int, N> pool{};
    for (int i = 0; i < N; ++i)
        pool[i] = i;
    std::array<int, N> result{};
    int64_t block = 1; // (N - 1)!
    for (int i = 2; i < N; ++i)
        block *= i;
    for (int pos = 0, left = N; pos < N; ++pos, --left) {
        const int pick = static_cast<int>(rank / block);
        rank %= block;
        result[pos] = pool[pick];
        for (int i = pick; i + 1 < left; ++i)
            pool[i] = pool[i + 1];
        if (left > 1)
            block /= left - 1;
    }
    return result;
}
template <int N>
constexpr int64_t permutationRank(const std::array<int, N> &order) {
    int64_t rank = 0;
    for (int pos = 0; pos < N; ++pos) {
        int smaller = 0; // later entries below this one
        for (int later = pos + 1; later < N; ++later)
            smaller += order[later] < order[pos] ? 1 : 0;
        rank = rank * (N - pos) + smaller;
    }
    return rank;
}

// The effects chain's order as a state from before version 9 holds it: the slot at each of its places, a param a place
// (fxPositionIndex()), in the order they ran. Empty slots take no part. Any values make an order: a place holding no
// slot, or one an earlier place holds, is passed over, and the slots no place holds run after the others, by number.
// `get(index)` returns a param value.
using FxOrder = std::array<int, kFxSlots>;
constexpr int fxPositionIndex(int position) { return kFxPositionBase + position; }
template <class Get>
FxOrder fxOrder(const Get &get) {
    FxOrder order{};
    std::array<bool, kFxSlots> taken{};
    int n = 0;
    for (int position = 0; position < kFxSlots; ++position) {
        const double value = get(fxPositionIndex(position));
        const long slot = std::isfinite(value) ? std::lround(value) : -1;
        if (slot >= 0 && slot < kFxSlots && !taken[slot]) {
            taken[slot] = true;
            order[n++] = static_cast<int>(slot);
        }
    }
    for (int slot = 0; slot < kFxSlots; ++slot)
        if (!taken[slot])
            order[n++] = slot;
    return order;
}
// The (param, value) pairs that set `order`: a place each.
inline std::vector<std::pair<int, double>> fxOrderChanges(const FxOrder &order) {
    std::vector<std::pair<int, double>> changes;
    for (int position = 0; position < kFxSlots; ++position)
        changes.push_back({fxPositionIndex(position), static_cast<double>(order[position])});
    return changes;
}
// The order that runs the slots in `first` in that order, then the others by number.
inline FxOrder fxOrderStarting(std::initializer_list<int> first) {
    FxOrder order{};
    std::array<bool, kFxSlots> taken{};
    size_t n = 0;
    for (int slot : first) {
        order[n++] = slot;
        taken[slot] = true;
    }
    for (int slot = 0; slot < kFxSlots; ++slot)
        if (!taken[slot])
            order[n++] = slot;
    return order;
}

// The effects graph's wires (their From params) that run `slots` one after another from the input to the output, every
// other wire taken away: a chain, as the graph has it. With none, the input goes straight out.
inline std::vector<std::pair<int, double>> fxChainWires(const std::vector<int> &slots) {
    std::vector<std::pair<int, double>> changes;
    for (int node = 0; node < kFxNodes; ++node)
        for (int input = 0; input < fxInputCount(node); ++input)
            changes.push_back({fxInputIndex(node, input, FxInputField::From), 0.0});
    auto wire = [&changes](int node, int port) {
        for (auto &change : changes)
            if (change.first == fxInputIndex(node, 0, FxInputField::From))
                change.second = fxPortValue(port);
    };
    int port = kFxPortIn;
    for (int slot : slots) {
        wire(slot, port);
        port = fxSlotPort(slot);
    }
    wire(kFxOutNode, port);
    return changes;
}

// RetiredFxOrder: the order of the first kFirstFxSlots slots as one number, one of their kRetiredFxOrders
// permutations in lexicographic order, which a state from before version 8 holds (see readParams()). 16! fits a
// double exactly; 32! wouldn't, hence a param a place.
constexpr int64_t factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }
constexpr int64_t kRetiredFxOrders = factorial(kFirstFxSlots);
static_assert(kRetiredFxOrders < (int64_t{1} << 53));
using RetiredFxOrder = std::array<int, kFirstFxSlots>;
inline RetiredFxOrder retiredFxOrder(double value) {
    const int64_t i = std::isfinite(value) ? std::llround(value) : 0;
    return permutationAt<kFirstFxSlots>(i < 0 ? 0 : (i >= kRetiredFxOrders ? kRetiredFxOrders - 1 : i));
}
// The order a retired one stands for: its slots, then the later ones by number.
inline FxOrder fxOrderFromRetired(double value) {
    FxOrder order{};
    const RetiredFxOrder first = retiredFxOrder(value);
    for (int i = 0; i < kFxSlots; ++i)
        order[i] = i < kFirstFxSlots ? first[i] : i;
    return order;
}
static_assert(permutationRank<kFirstFxSlots>(permutationAt<kFirstFxSlots>(437)) == 437);
static_assert(permutationRank<kFirstFxSlots>(permutationAt<kFirstFxSlots>(kRetiredFxOrders - 1)) ==
              kRetiredFxOrders - 1);

// --- Freeform LFO shapes.

// How a segment eases from one point to the next: t (0..1) bent by curve
// (-1..1). Positive curves start slow and end fast, negative ones the reverse.
inline double shapeBend(double t, double curve) {
    if (std::abs(curve) < 1e-3)
        return t;
    const double a = 8.0 * curve;
    return std::expm1(a * t) / std::expm1(a);
}
inline int shapePointCount(double value, int most = kMaxShapePoints) { // the note curve's: kMaxNotePoints
    const long n = std::lround(value);
    return static_cast<int>(n < 2 ? 2 : (n > most ? most : n));
}
// The shape's value (-1..1) at `phase` (0..1). `get(index)` returns a param
// value, so the DSP and the editor read points the same way. Points may share
// a time, a line straight up or down: at that time the last of them counts,
// the one the shape goes on from.
constexpr double kShapeSameTime = 1e-9;
template <class Get>
double shapeValue(const Get &get, int source, double phase) {
    const int count = shapePointCount(get(shapeCountIndex(source)));
    auto x = [&](int point) { return point == 0 ? 0.0 : get(shapePointIndex(source, point, PointField::X)); };
    int from = 0;
    while (from + 1 < count && x(from + 1) <= phase)
        ++from;
    const double x0 = x(from);
    const double y0 = get(shapePointIndex(source, from, PointField::Y));
    const bool wraps = from + 1 == count;
    const double x1 = wraps ? 1.0 : x(from + 1);
    const double y1 = get(shapePointIndex(source, wraps ? 0 : from + 1, PointField::Y));
    if (x1 - x0 < 1e-9) // a last point at the cycle's end, where a one-shot stays: it counts, not the first
        return y0;
    const double t = std::clamp((phase - x0) / (x1 - x0), 0.0, 1.0);
    return y0 + (y1 - y0) * shapeBend(t, get(shapePointIndex(source, from, PointField::Curve)));
}

// The shape's outline, for drawing it: `put(x, y)` along each segment, about `steps` across the cycle, and every
// point of a line straight up or down, so a spike shows however narrow. It ends at the cycle's end, back at the
// first point's level.
template <class Get, class Put>
void shapeOutline(const Get &get, int source, int steps, const Put &put) {
    const int count = shapePointCount(get(shapeCountIndex(source)));
    auto x = [&](int point) { return point == 0 ? 0.0 : get(shapePointIndex(source, point, PointField::X)); };
    auto y = [&](int point) { return get(shapePointIndex(source, point, PointField::Y)); };
    for (int point = 0; point < count; ++point) {
        const bool wraps = point + 1 == count;
        const double x0 = x(point), x1 = wraps ? 1.0 : x(point + 1);
        const double y0 = y(point), y1 = y(wraps ? 0 : point + 1);
        const double curve = get(shapePointIndex(source, point, PointField::Curve));
        put(x0, y0);
        const int n = x1 - x0 < kShapeSameTime ? 0 : std::max(1, static_cast<int>(std::ceil((x1 - x0) * steps)));
        for (int i = 1; i < n; ++i) {
            const double t = static_cast<double>(i) / n;
            put(x0 + (x1 - x0) * t, y0 + (y1 - y0) * shapeBend(t, curve));
        }
    }
    put(1.0, y(0));
}

// A shaper's drawn curve: the output (-1..1) for input `x`. The points run
// across the input range -1..1 (their x, 0..1, maps to it): the first sits at
// the left end and the last at the right, and past them the curve stays level.
// Each point's curve bends the segment to the next one. `base` is the curve's
// point count index (clipCurveCountIndex()).
template <class Get>
double clipCurveValue(const Get &get, int base, double x) {
    const int count = shapePointCount(get(base));
    auto px = [&](int point) {
        return point == 0 ? 0.0 : (point == count - 1 ? 1.0 : get(shapeFieldIndex(base, point, PointField::X)));
    };
    const double at = std::clamp(0.5 * (x + 1.0), 0.0, 1.0);
    int from = 0;
    while (from + 2 < count && px(from + 1) <= at)
        ++from;
    const double x0 = px(from), x1 = px(from + 1);
    const double y0 = get(shapeFieldIndex(base, from, PointField::Y));
    const double y1 = get(shapeFieldIndex(base, from + 1, PointField::Y));
    if (x1 - x0 < 1e-9)
        return y1;
    const double t = std::clamp((at - x0) / (x1 - x0), 0.0, 1.0);
    return y0 + (y1 - y0) * shapeBend(t, get(shapeFieldIndex(base, from, PointField::Curve)));
}

// The note source's output (0..1) for a pitch in semitones: where it falls between Low and High (outside them,
// at their ends; High may lie below Low), through the drawn curve, whose points run across that range and
// whose y (-1..1) is 0..1 of the output. As it starts, the output follows the cutoff knobs' curve (see
// NoteParam).
template <class Get>
double noteSourceValue(const Get &get, double pitch) {
    const double low = get(paramIndex(NoteParam::Low)), span = get(paramIndex(NoteParam::High)) - low;
    const double x = std::abs(span) < 1e-6 ? (pitch >= low ? 1.0 : 0.0) : std::clamp((pitch - low) / span, 0.0, 1.0);
    return std::clamp(0.5 * (clipCurveValue(get, paramIndex(NoteParam::Curve), 2.0 * x - 1.0) + 1.0), 0.0, 1.0);
}

// The velocity source's output (0..1) for a note's velocity (0..1): the velocity through the drawn curve, whose
// points run across the velocities and whose y (-1..1) is 0..1 of the output (see VelocityParam).
template <class Get>
double velocitySourceValue(const Get &get, double velocity) {
    const double x = std::clamp(velocity, 0.0, 1.0);
    return std::clamp(0.5 * (clipCurveValue(get, paramIndex(VelocityParam::Curve), 2.0 * x - 1.0) + 1.0), 0.0, 1.0);
}

// Envelope segment slopes. A curve (-1..1) sets how fast a segment moves at
// first: 0 is a straight line, positive curves start fast and settle (like a
// capacitor charging), negative ones start slow and rush at the end. Either
// way the segment arrives in its time. The steepness is k = curve * ln(10^6),
// so kExponentialCurve covers 60 dB of a fall in the segment's time: the
// default for decays and releases.
constexpr double kCurveSteepness = 13.815510557964274; // ln(10^6)
constexpr double kExponentialCurve = 0.5;
constexpr double kLinearSteepness = 1e-3; // below this |k|, a segment is a straight line

// How far along a segment is (0..1) at time t (0..1) for steepness k.
inline double envProgress(double t, double k) {
    if (std::abs(k) < kLinearSteepness)
        return t;
    return std::expm1(-k * t) / std::expm1(-k);
}
// The time (0..1) at which a segment of steepness k has come `progress` (0..1) of the way.
inline double envProgressTime(double progress, double k) {
    progress = std::clamp(progress, 0.0, 1.0);
    if (std::abs(k) < kLinearSteepness)
        return progress;
    return std::clamp(-std::log1p(progress * std::expm1(-k)) / k, 0.0, 1.0);
}
inline double envCurve(double t, double curve) {
    return envProgress(t, curve * kCurveSteepness);
}
// What a slot takes from its source, bent by the slot's curve (-1..1) as an envelope segment is: unipolar, its 0..1
// along envCurve(); bipolar (-0.5..0.5 about the set value), each half of it from the middle out, so that the middle
// stays where it is and the two ends swing alike. 0 leaves it as it is.
inline double slotCurveValue(double value, double curve, bool bipolar) {
    if (std::abs(curve * kCurveSteepness) < kLinearSteepness)
        return value;
    if (!bipolar)
        return envCurve(std::clamp(value, 0.0, 1.0), curve);
    const double half = 0.5 * envCurve(std::clamp(2.0 * std::abs(value), 0.0, 1.0), curve);
    return value < 0.0 ? -half : half;
}

// A compressor's static curve: the output level (dB) for an input level (dB).
// Above the threshold, levels rise 1/ratio as fast; within the knee, centered
// on the threshold, the slope eases from 1 to 1/ratio.
inline double compressorCurve(double inDb, double thresholdDb, double ratio, double kneeDb) {
    const double over = inDb - thresholdDb;
    if (2.0 * over <= -kneeDb)
        return inDb;
    if (kneeDb > 0.0 && 2.0 * std::abs(over) < kneeDb) {
        const double into = over + 0.5 * kneeDb;
        return inDb + (1.0 / ratio - 1.0) * into * into / (2.0 * kneeDb);
    }
    return thresholdDb + over / ratio;
}

// The three-band dynamics' ratios, stored as 0..1: how much of a level's distance from the threshold the band takes
// away, 1 - 1 / ratio. 0 is 1:1, 0.5 2:1, 0.75 4:1 and 1 infinity (the level held at the threshold), so a knob or
// a drag turns the ratio up evenly in what it does to the level.
inline double dynRatio(double norm) {
    return norm >= 1.0 ? std::numeric_limits<double>::infinity() : 1.0 / (1.0 - std::max(norm, 0.0));
}
inline double normFromDynRatio(double ratio) { return std::isinf(ratio) ? 1.0 : 1.0 - 1.0 / std::max(ratio, 1.0); }

// The three-band dynamics' gain (dB) for a band at `inDb`: above `aboveDb` pulled down by `down`, as a compressor
// does, and below `belowDb` lifted by `up`, its mirror image (a level the gap under the threshold shrinks by the
// ratio), lifting at most `rangeDb`. A Below threshold over the Above one is taken as the Above one.
inline double dynamicsGain(double inDb, double aboveDb, double down, double belowDb, double up, double kneeDb,
                           double rangeDb) {
    const double downGain = compressorCurve(inDb, aboveDb, down, kneeDb) - inDb;
    const double below = std::min(belowDb, aboveDb);
    const double lift = -inDb - compressorCurve(-inDb, -below, up, kneeDb); // the compressor's curve, mirrored
    return downGain + std::clamp(lift, 0.0, std::max(rangeDb, 0.0));
}

// The three-band dynamics' fields as the OTT item of its card's menu sets them (the changes, as param index and value):
// the well-known OTT ("over the top") multiband preset, fitted to what it does to a test signal bounced through it
// (sines stepped in level in each band, level jumps, saws and plucks). Its crossovers (88.3 Hz, 2.5 kHz) and ratios
// carry over as they are: every band pulled down hard above Above (66.7:1, the high band's ∞:1) and lifted 4.17:1
// below Below. Its detector reads RMS. Measured, its curves come out as these bands' with:
// - every threshold 7.1 dB under the one it shows (its levels read that much over this detector's), and its bands'
//   gains 5.2 dB over theirs, through Output (the bands' Gain stops at 12 dB; at 100 % mix it is the same), so a loud
//   saw comes out some 14 dB down and a quiet one comes up toward it;
// - a 21 dB knee, and Range all the way up (its lift has no limit): within 0.3 dB of its curves, every band;
// - its attacks and releases, per band there (47.8, 22.4 and 13.5 ms; 282, 282 and 132 ms, how long it takes to
//   settle), shared here: 1 ms and 30 ms follow its jumps best, the low and mid bands' releasing best at 40 ms and the
//   high band's at 15.
// Link goes off first, so each band takes its own values.
inline std::vector<std::pair<int, double>> mdOttSettings(int slot) {
    struct Band {
        double above, down, below, up, gain;
    };
    constexpr double kInf = std::numeric_limits<double>::infinity();
    constexpr Band kBands[kMcBands] = {{-33.8, 66.7, -40.8, 4.17, 10.3}, // low, as the preset shows them
                                       {-30.2, 66.7, -41.8, 4.17, 5.7},  // mid
                                       {-35.5, kInf, -40.8, 4.17, 10.3}}; // high
    constexpr double kThresholdShift = -7.1, kMakeup = 5.2; // dB, as measured
    std::vector<std::pair<int, double>> changes{
        {paramIndex(slot, FxParam::MdLink), 0.0},
        {paramIndex(slot, FxParam::MdLowFreq), normFromCutoff(88.3)},
        {paramIndex(slot, FxParam::MdHighFreq), normFromCutoff(2500.0)},
    };
    for (int b = 0; b < kMcBands; ++b) {
        changes.push_back({mdAboveIndex(slot, b), kBands[b].above + kThresholdShift});
        changes.push_back({mdDownIndex(slot, b), normFromDynRatio(kBands[b].down)});
        changes.push_back({mdBelowIndex(slot, b), kBands[b].below + kThresholdShift});
        changes.push_back({mdUpIndex(slot, b), normFromDynRatio(kBands[b].up)});
        changes.push_back({mdGainIndex(slot, b), kBands[b].gain});
    }
    changes.insert(changes.end(), {{paramIndex(slot, FxParam::MdAttack), normFromTime(0.001)},
                                   {paramIndex(slot, FxParam::MdRelease), normFromTime(0.03)},
                                   {paramIndex(slot, FxParam::MdKnee), 21.0},
                                   {paramIndex(slot, FxParam::MdRange), 36.0},
                                   {paramIndex(slot, FxParam::MdMix), 1.0},
                                   {paramIndex(slot, FxParam::MdOutput), kMakeup},
                                   {paramIndex(slot, FxParam::MdRms), 1.0}});
    return changes;
}

// EQ band gain: -1 kills the band, 0 leaves it alone, 1 is +6 dB.
inline double eqGain(double value) {
    return value >= 0.0 ? 1.0 + value : (1.0 + value) * (1.0 + value);
}

// Bandwidth of a parametric EQ band, stored as 0..1 on a log curve.
constexpr double kMinQ = 0.3;
constexpr double kMaxQ = 10.0;
inline double qFromNorm(double x) { return kMinQ * std::pow(kMaxQ / kMinQ, x); }
inline double normFromQ(double q) { return std::log(q / kMinQ) / std::log(kMaxQ / kMinQ); }
constexpr double kMaxPeqGain = 18.0; // dB either way

// The delay's tone, -1..1: below 0 a low-pass sweeps down from 20 kHz to 200 Hz,
// above 0 a high-pass sweeps up from 20 Hz to 4 kHz. The high-pass stays at
// 20 Hz otherwise, which keeps rumble from building up in the echoes.
struct DelayTone {
    double lowPassHz;  // 0: no low-pass
    double highPassHz;
};
inline DelayTone delayTone(double tone) {
    return {tone < 0.0 ? cutoffFromNorm(1.0 + tone * 2.0 / 3.0) : 0.0,
            cutoffFromNorm(std::max(tone, 0.0) * 0.77)};
}

// --- Param table.

enum class ParamKind {
    Decibel,
    Percent,
    BipolarPercent,
    Time,
    Ratio,
    Cents,          // bipolar
    CentsAmount,    // unipolar
    Semitones,      // stepped
    Transpose,      // bipolar semitones: continuous for modulation, but the editor sets whole ones
    SemitoneOffset, // continuous, bipolar
    Count,          // stepped whole number
    VoiceCount,     // stepped, shown and dragged on a log scale
    Toggle,         // stepped Off / On
    TimeOrOff,      // like Time, but 0 is Off
    EnvTime,        // like Time, but on down to 0 ms below it (an envelope's attack, decay and release)
    Pan,
    Cutoff, // 0..1 on a log curve, shown in Hz
    LfoShape,
    OpWave,
    FilterType,
    ModTarget,
    SyncDivision, // stepped note value
    ClipMode,
    FxOrder,    // the retired chain order, see retiredFxOrder()
    FxPosition, // the slot at a place in the effects chain, see fxOrder()
    FxType,
    SourceType,
    DelayTone,  // -1..1, see delayTone()
    Q,          // 0..1 on a log curve, see qFromNorm()
    EqGain,     // -1..1, see eqGain()
    CompRatio,  // 1..20, shown as "4.0:1"
    ShapeCount, // stepped; how many points a drawn shape has, or partials a Custom wave plays: hidden from hosts
    ShapePoint, // continuous, but not a modulation target
    NoiseSlope, // 0..1, shown as White / Pink / Brown or in dB per octave
    FilterSlope, // stepped: 12 or 24 dB per octave
    ShiftRange,  // 0..1 on a log curve, shown in Hz; see shiftRangeFromNorm()
    SlotBypass,  // stepped Off / On, set in the editor and hidden from hosts
    PeqType,     // a parametric EQ band's shape
    ReverbType,  // the reverb's algorithm
    MidiInput,   // what a Midi source follows
    SweepRate,   // 0..1 on a log curve, shown in Hz, its lowest setting 0 Hz: held still (see sweepRateHz())
    NotePitch,   // semitones in MIDI numbering, shown as a note and its cents; continuous, but the editor sets whole ones
    FixedFreq,   // 0..1 on a log curve, shown in Hz; see fixedFreqFromNorm()
    FixedMult,   // stepped: one of kFixedMults, shown as x0.001 and so on
    SlotAux,      // stepped: the source + 1 that scales a slot (0 = none), set in the editor and hidden from hosts
    OpRatio,      // 0..1 on a log curve, shown as a ratio to two places; see ratioFromNorm()
    DynRatio,     // 0..1 for 1:1 to infinity, shown as "4.0:1" or "∞:1"; see dynRatio()
    RouteMode,    // stepped: how a route acts on the operator it goes into (see RouteMode)
    FxPort,       // stepped: the port a wire of the effects graph comes from (see fxPortValue()), 0 for none
    AmpModel,     // stepped: the amp's model (see AmpModel)
    AmpCab,       // stepped: the amp's cabinet (see AmpCab)
    AmpMic,       // stepped: the mic in front of the amp's cabinet (see AmpMic)
    AmpOversampling, // stepped: 1x, 2x, 4x or 8x (see ampOversampling())
    AmpDial,      // 0..1, shown as 0.0 to 10.0, as an amp's knobs are numbered
    AmpDistance,  // 0..1 on a log curve, shown in cm or m; see ampDistanceCm()
    Fader,        // 0..1, a gain from silence to +12 dB, shown in dB down to -inf dB; see utilityGain()
    CutoffOrOff,  // like Cutoff, but 0 is Off; see utilityMonoBassHz()
};
// A port's name: "In", "FX 3", "Split 1 Low"; "None" for -1.
std::string fxPortName(int port);

struct ParamDef {
    clap_id id;
    std::string name;
    std::string module; // host-side grouping, "/"-separated
    ParamKind kind;
    double min;
    double max;
    double def;
    clap_param_info_flags flags;
};

using ParamTable = std::array<ParamDef, kParamCount>;
using ParamValues = std::array<double, kParamCount>;

const ParamTable &paramTable();

// The params hosts are given, in table order: the globals, the operators, the matrix, the filters, the shapers, the
// route modes into operators, the macros' knobs, the note source's, the mod wheel's and velocity's own, and every
// source's slots' depths and Amounts. Left out are those the table marks CLAP_PARAM_IS_HIDDEN, which are drawn and
// wired in the editor only (freeform shapes, shaper curves, Custom waves' partials, the chain order, the retired
// fields and the feedback's route modes), the slots' targets, auxes, curves and bypasses, the effect slots' fields
// and the source places' own. They are still params everywhere else: the state, presets, undo and the editor's
// modulation take them as any other.
int hostParamCount();
// The param at `hostIndex` in that list, or -1 past its end.
int hostParamIndex(int hostIndex);
// Whether hosts are given param `index`. A value a host sends for one it isn't given is still taken.
bool hostListed(int index);

bool isStepped(ParamKind kind);
// Dragged and scrolled in whole steps in the editor: the stepped kinds, the operators' pitch and the notes.
inline bool snapsToSteps(ParamKind kind) {
    return isStepped(kind) || kind == ParamKind::Transpose || kind == ParamKind::NotePitch;
}
// Displayed and modulated around the middle of the range rather than from the minimum.
bool isBipolar(ParamKind kind);
// A stepped param that modulation still drives, a step at a time (its value rounded): an LFO's synced rate.
constexpr bool modulatedInSteps(int index) { return sourceFieldOf(index) == SourceParam::Division; }
// Continuous params are the ones LFOs, envelopes and host modulators can drive, and those modulatedInSteps(); not the
// retired ones (see isRetired()), which do nothing.
// Not the macros' own knobs: they are read as set.
bool isRetired(int index);
// The wheels' positions, played rather than set: the mod wheel and the pitch bend. Not part of the patch's edits (no
// undo step), nor a target the editor offers modulation.
constexpr bool isPlayed(int index) {
    return index == paramIndex(GlobalParam::ModWheel) || index == paramIndex(GlobalParam::PitchBend);
}
inline bool isModTarget(int index) {
    return index >= 0 && index < kMacroBase && !isRetired(index) &&
           (!isStepped(paramTable()[index].kind) || modulatedInSteps(index)) &&
           paramTable()[index].kind != ParamKind::ShapePoint;
}
// Value of a slot target param pointing at param `index`, and back (-1 = none or invalid).
inline double slotTargetValue(int index) { return index < 0 ? 0.0 : idOfIndex(index) + 1.0; }
inline int slotTarget(double value) {
    if (value < 0.5) // rounds to 0, none: most slots, which the voices look over every block
        return -1;
    const long id = std::lround(value) - 1;
    const int index = id < 0 ? -1 : indexOfId(static_cast<clap_id>(id));
    return isModTarget(index) ? index : -1;
}

// A slot's aux (see SlotAuxParam): the source that scales it, or -1. A source can't scale its own slots.
template <class Get>
int slotAux(const Get &get, int source, int slot) {
    const int aux = slotAuxSource(get(slotAuxIndex(source, slot)));
    return aux == source ? -1 : aux;
}

// What the macros and the mod wheel add to their targets, the same for every voice and the effects, notes playing or
// not: calls `add(target, offset, source, slot)` for each slot in use, the offset in plain units. `get(index)` returns a param
// value. A slot whose aux runs per voice (an LFO, say) is left to the voices, which add it with their aux's value
// (see Synth::renderBlock()); one scaled by a macro or the mod wheel takes that knob's value here.
template <class Get, class Add>
void forEachMacroMod(const Get &get, const Add &add) {
    for (int source = kMaxSources; source < kAllSources; ++source) {
        if (!hasKnob(source))
            continue;
        // Bipolar, the knob's middle moves nothing, and its ends half the depth either way.
        const bool bipolar = sourceBipolar(get, source);
        const double knob = get(stripKnobIndex(source)) - (bipolar ? 0.5 : 0.0);
        for (int slot = 0; slot < kSlots; ++slot) {
            const int target = slotTarget(get(slotTargetIndex(source, slot)));
            const double amount = get(slotAmountIndex(source, slot));
            if (target < 0 || target >= kModulatedCount || amount == 0.0 || get(slotBypassIndex(source, slot)) >= 0.5)
                continue;
            const int aux = slotAux(get, source, slot);
            if (aux >= 0 && runsPerVoice(aux))
                continue;
            const double scale = get(slotScaleIndex(source, slot)) * (aux >= 0 ? get(stripKnobIndex(aux)) : 1.0);
            add(target,
                slotCurveValue(knob, get(slotCurveIndex(source, slot)), bipolar) * amount * scale *
                    (paramTable()[target].max - paramTable()[target].min),
                source, slot);
        }
    }
}

// Whether param `index` does anything with the current settings: an effect
// slot's fields only count while the slot holds their type, and a source's
// likewise. Hosts don't see the others. `get(index)` returns a param value.
template <class Get>
bool fieldInUse(const Get &get, int index) {
    if (const int slot = fxSlotOf(index); slot >= 0) {
        const FxParam field = fxFieldOf(index);
        if (field == FxParam::Type)
            return true;
        const FxType type = fxType(get(paramIndex(slot, FxParam::Type)));
        const FxType owner = fxTypeOfField(field);
        return type != FxType::None && (owner == FxType::None || owner == type);
    }
    if (const int source = sourceOf(index); source >= 0) {
        const SourceParam field = sourceFieldOf(index);
        if (field == SourceParam::Type)
            return true;
        const SourceType type = sourceType(get(paramIndex(source, SourceParam::Type)));
        return type != SourceType::None && sourceFieldUsedBy(field, type);
    }
    return true;
}

// Text for the host's generic UI; parseParam() reads back whatever formatParam() writes.
bool formatParam(int index, double value, char *out, uint32_t capacity);
bool parseParam(int index, const char *text, double *out);

const char *lfoShapeName(LfoShape shape);
const char *waveName(OpWave wave);
const char *filterTypeName(FilterType type);
const char *filterTypeShortName(FilterType type);
const char *divisionName(int division);
const char *clipModeName(ClipMode mode);
const char *peqTypeName(PeqType type);
const char *reverbTypeName(ReverbType type);
const char *ampModelName(AmpModel model);
const char *ampCabName(AmpCab cab);
const char *ampMicName(AmpMic mic);
const char *routeModeName(RouteMode mode);
const char *midiInputName(MidiInput input);
const char *sourceTypeName(SourceType type);
const char *fxName(FxType fx);

} // namespace fmvibe
