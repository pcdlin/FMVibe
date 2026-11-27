#include "synth.h"
#include "unison.h"
#include "warp.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

// What only some patches use stays out of renderBlock()'s code, which MSVC builds worse the more goes into it.
#ifndef FMVIBE_NOINLINE // filter.h defines it too
#ifdef _MSC_VER
#define FMVIBE_NOINLINE __declspec(noinline)
#else
#define FMVIBE_NOINLINE __attribute__((noinline))
#endif
#endif

namespace fmvibe {

namespace {

// MPE: channels 2 to 16, numbered from 0, each carry a note of their own (see Synth::setMpe()).
bool memberChannel(int channel) { return channel >= 1 && channel < 16; }

constexpr uint32_t kControlBlock = 32;        // samples between modulation updates
// How many host samples apart a voice filter's settings are worked out again while its inputs ramp (see renderBlock()
// and kFilterInputSmooth).
constexpr uint32_t kFilterSubStep = 8;
// An operator's envelope's fields, in envRates()'s order: its times, sustain and curves.
constexpr std::array<OpParam, 8> kOpEnvFields{OpParam::Attack,  OpParam::Hold,        OpParam::Decay,
                                              OpParam::Sustain, OpParam::Release,     OpParam::AttackCurve,
                                              OpParam::DecayCurve, OpParam::ReleaseCurve};
std::array<double, 8> opEnvFields(const double *p, int op) {
    std::array<double, 8> fields;
    for (size_t k = 0; k < fields.size(); ++k)
        fields[k] = p[paramIndex(op, kOpEnvFields[k])];
    return fields;
}
EnvRates envRatesOf(const std::array<double, 8> &t, double rate) {
    return envRates(t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], rate);
}
constexpr double kPhaseModCycles = 64.0;      // how many cycles modulation takes an operator's Phase either way
constexpr double kWarpSlew = 250.0;           // how fast an operator's warp amount moves at most, a second
// Filter FM: the octaves a modulation at full swing and depth takes a cutoff, as far as the knob goes, and the seconds
// a filter effect's cutoff takes to get over the jump as the voice it follows changes.
const double kCutoffOctaves = std::log2(kMaxCutoff / kMinCutoff);
constexpr double kFxFmJump = 0.005;
// Whether a filter of this type follows filter FM: the Formant type's cutoff stays with the control blocks.
bool followsCutoffFm(double type) {
    return std::clamp(static_cast<int>(std::lround(type)), 0, static_cast<int>(FilterType::Count) - 1) !=
           static_cast<int>(FilterType::Formant);
}
constexpr double kMaxIndex = 8.0;             // radians of phase modulation at a 100 % matrix amount
constexpr double kMaxFeedback = std::numbers::pi;
// The other ways a route can act on an operator (see RouteMode), at a 100 % amount and FM Amount: Linear FM swings
// its frequency by this many times its own (through zero past 1, which an amount of 50 % reaches), Exp FM its pitch
// by this many octaves either way. Both take the amount squared, as Phase does; Ring takes it as it is, a blend from
// the operator as it was to it times the source.
constexpr double kMaxLinearDepth = 4.0;
constexpr double kMaxExpOctaves = 4.0;
// A route's level for its mode, from its amount: what the voice ramps between blocks.
double routeLevel(RouteMode mode, double amount, double phaseIndex) {
    switch (mode) {
    case RouteMode::LinearFm:
        return amount * amount * kMaxLinearDepth;
    case RouteMode::ExpFm:
        return amount * amount * kMaxExpOctaves;
    case RouteMode::Ring:
        return amount;
    default:
        return amount * amount * phaseIndex / (2.0 * std::numbers::pi);
    }
}
constexpr int kFadeSlots = 4;        // extra voices for stolen ones to fade out in, at least
constexpr int kMaxFadeSlots = 32;    // and at most: as many as the voices, so a chord over them all steals each
constexpr double kVoiceQuietFloor = 1e-4; // a released voice quieter than this, its operators idle, has ended
constexpr double kFilterRingFloor = 1e-6; // a voice filter quieter than this, its inputs stopped, has rung out
// Once the last voice has ended, the effects' modulation runs on until they have put out less than this (-100 dB,
// before Volume) for kFxTailHold seconds.
constexpr float kFxTailFloor = 1e-5f;
constexpr double kFxTailHold = 1.0;
constexpr size_t kMaxEndedNotes = 4096; // notes ending in one block that are reported (see reportEnded())
constexpr double kStealFade = 0.003; // seconds a stolen voice takes to fade out
constexpr double kJoinFade = 0.005;  // seconds a note joining a held one in poly legato takes to fade in
constexpr double kVolumeGlide = 0.005; // seconds Volume takes to glide most of the way to a new level
// How loud each voice goes into the effects, -12 dB: Volume comes after them, so drive and thresholds don't
// depend on it. Volume gives the 12 dB back, so at 0 dB a full-scale operator comes out at full scale.
constexpr double kVoiceLevel = 0.25118864315095801;
// How far the shapers hear a filter's or a shaper's output go, and a filter itself or a later one, +18 dB. A filter and a shaper feeding each other grow
// without end when part of the shaper's input goes through unshaped (Mix below 100 %) and a resonant filter lifts it
// past unity, and so do shapers feeding each other or themselves with little of their Mix; held here, the loop stays
// finite.
constexpr double kLoopLimit = 8.0;

class SineTable {
public:
    SineTable() {
        for (int i = 0; i <= kSize; ++i)
            table_[i] = std::sin(2.0 * std::numbers::pi * i / kSize);
    }

    // Phase in cycles, any range.
    double operator()(double phase) const {
        const double x = (phase - std::floor(phase)) * kSize;
        const int i = std::min(static_cast<int>(x), kSize - 1);
        return table_[i] + (table_[i + 1] - table_[i]) * (x - i);
    }

private:
    static constexpr int kSize = 4096;
    std::array<double, kSize + 1> table_;
};

const SineTable sine;

bool matches(const NoteRef &match, const NoteRef &note) {
    return (match.noteId == -1 || match.noteId == note.noteId) &&
           (match.port == -1 || match.port == note.port) &&
           (match.channel == -1 || match.channel == note.channel) &&
           (match.key == -1 || match.key == note.key);
}

// xorshift32, mapped to -1..1.
double nextRandom(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state / 2147483648.0 - 1.0;
}

int stepped(double value, int max) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, max);
}

// Cycles per second of the LFO in place `source`, reading its params through `get`: 0 at its lowest Rate.
template <class Get>
double lfoHz(const Get &get, int source, double tempo) {
    return get(paramIndex(source, SourceParam::Sync)) >= 0.5
               ? 1.0 / divisionSeconds(get(paramIndex(source, SourceParam::Division)), tempo)
               : sweepRateHz(get(paramIndex(source, SourceParam::Rate)));
}

// What the LFO in place `source` puts out where `lfo` stands, -1..1, reading its params through `get` and a Custom
// shape's points from `params`.
// A random one standing still holds the value it took, gliding to none.
template <class Get, class Lfo>
double lfoOutput(const Get &get, const ParamValues &params, int source, const Lfo &lfo) {
    const LfoShape shape = lfoShape(get, source);
    return shape == LfoShape::Custom ? shapeValue([&params](int i) { return params[i]; }, source, lfo.phase)
           : shape == LfoShape::Random && !lfoHeld(get, source)
               ? randomGlide(lfo.from, lfo.held, lfo.phase, get(paramIndex(source, SourceParam::RandomSmooth)))
               : lfoValue(shape, lfo.phase, lfo.held);
}

// How long a Curve in Sustain whose loop ends at the curve's end takes, let go, to glide from the level it was at to
// where the curve ends, rather than jumping there.
constexpr double kCurveGlide = 0.005; // seconds

// A Curve in Sustain whose key has gone up leaves its loop at once, wherever it is (before the loop or in it): it goes
// on from the loop's end, what it put out carried along on top of the curve as an offset that fades out by the next
// point after the loop (or over kCurveGlide where the loop ends at the curve's end), so it plays on from the level it
// was at and lands on that point. One past its loop's end already (a note started it there) plays on as it is. Its
// points are read from `params`, as lfoOutput() reads them.
template <class Lfo, class Release>
FMVIBE_NOINLINE void leaveLoop(const ParamValues &params, int source, Lfo &lfo, Release &release, LfoLoop loop) {
    lfo.left = true;
    release = {};
    if (lfo.phase >= loop.end)
        return;
    auto get = [&params](int i) { return params[i]; };
    const double level = shapeValue(get, source, lfo.phase);
    lfo.phase = loop.end;
    release.offset = level - shapeValue(get, source, loop.end);
    release.fadeFrom = loop.end;
    // It fades out by the next point after the loop's end, the end of the segment the curve goes on along: the point
    // after it, where the loop ends between two; where it ends on a point, the one after that (the curve's end, back
    // at its first point, after the last), and at once, over kCurveGlide, where that one sits there too (the curve
    // jumping straight up or down as the loop ends), or the loop ends at the curve's end.
    const int count = shapePointCount(params[shapeCountIndex(source)]);
    auto x = [&](int point) { return point == 0 ? 0.0 : params[shapePointIndex(source, point, PointField::X)]; };
    release.fadeTo = 1.0;
    for (int point = 0; point < count; ++point) {
        if (x(point) > loop.end + kShapeSameTime) {
            release.fadeTo = std::min(x(point), 1.0);
            break;
        }
        if (x(point) >= loop.end - kShapeSameTime) { // on the loop's end
            const double next = point + 1 < count ? std::min(x(point + 1), 1.0) : 1.0;
            release.fadeTo = next > loop.end + kShapeSameTime ? next : loop.end;
            break;
        }
    }
    release.glide = 1.0;
}

// What a Curve that has left its loop adds to its curve where it stands now (see leaveLoop()); 0 for any other.
template <class Lfo, class Release>
double leftOffset(const Lfo &lfo, const Release &release) {
    if (!lfo.left || release.offset == 0.0)
        return 0.0;
    if (release.fadeTo > release.fadeFrom)
        return release.offset *
               std::clamp((release.fadeTo - lfo.phase) / (release.fadeTo - release.fadeFrom), 0.0, 1.0);
    return release.offset * release.glide;
}

// Whether a Curve that has left its loop is still on its way to its curve's end, or its offset gliding out there.
// Standing still (at 0 Hz) short of it, it never gets there: it counts as there.
template <class Lfo, class Release>
bool leftGoing(const Lfo &lfo, const Release &release) {
    if (!lfo.left)
        return false;
    if (release.fadeTo <= release.fadeFrom && leftOffset(lfo, release) != 0.0) // gliding, whatever its rate
        return true;
    return lfo.phase < 1.0 && lfo.hz > 0.0;
}

} // namespace

double lfoValue(LfoShape shape, double phase, double held) {
    switch (shape) {
    case LfoShape::Sine:
        return sine(phase);
    case LfoShape::Triangle: {
        const double q = phase + 0.25 - std::floor(phase + 0.25);
        return 1.0 - 4.0 * std::abs(q - 0.5);
    }
    case LfoShape::Saw:
        return 2.0 * phase - 1.0;
    case LfoShape::Square:
        return phase < 0.5 ? 1.0 : -1.0;
    case LfoShape::Random:
        return held;
    case LfoShape::Custom: // needs the points; see shapeValue()
    case LfoShape::Count:
        break;
    }
    return 0.0;
}

double randomGlide(double from, double to, double phase, double smooth) {
    if (phase >= smooth)
        return to;
    return from + (to - from) * (0.5 - 0.5 * std::cos(std::numbers::pi * phase / smooth));
}

double randomStep(double last, double chaos, uint32_t &rng) {
    const double x = last + 0.1 * std::pow(20.0, std::clamp(chaos, 0.0, 1.0)) * nextRandom(rng);
    const double m = std::fmod(x + 1.0, 4.0); // folds back at -1 and 1: a period of 4
    const double p = m < 0.0 ? m + 4.0 : m;
    return p <= 2.0 ? p - 1.0 : 3.0 - p;
}

Synth::Synth() : table_(paramTable()), waves_(waveTables()) {
    for (int i = 0; i < kModulatedCount; ++i) {
        paramMin_[i] = table_[i].min;
        paramMax_[i] = table_[i].max;
    }
    // Modulation takes an operator's Ratio past its range, to 128 (see kRatioModNorm).
    for (int op = 0; op < kOps; ++op)
        paramMax_[paramIndex(op, OpParam::Ratio)] = kRatioModNorm;
    // And an operator's Phase round its cycle either way, past 0 % and 100 %, rather than stopping there.
    for (int op = 0; op < kOps; ++op) {
        paramMin_[paramIndex(op, OpParam::Phase)] = -kPhaseModCycles;
        paramMax_[paramIndex(op, OpParam::Phase)] = kPhaseModCycles;
    }
    findAudioTargets();
}

namespace {

// A voice's ramped levels as one run of numbers, to move them all at once.
template <class Levels>
std::span<double> values(Levels &levels) {
    static_assert(std::is_standard_layout_v<Levels> && sizeof(Levels) % sizeof(double) == 0);
    return {reinterpret_cast<double *>(&levels), sizeof(Levels) / sizeof(double)};
}

} // namespace

void Synth::findAudioTargets() {
    Levels probe;
    auto field = [&probe](const double &f) { return static_cast<int16_t>(&f - values(probe).data()); };
    auto put = [this](int index, AudioKind kind, int a, int b = -1, int16_t at = -1, AudioGate gate = AudioGate::None) {
        if (index >= 0 && index < kVoiceParamCount)
            audioTargets_[index] = {kind, gate, static_cast<int8_t>(a), static_cast<int8_t>(b), at};
    };
    for (int i = 0; i < kOps; ++i) {
        put(paramIndex(i, OpParam::Out), AudioKind::Level, i, -1, field(probe.out[i]), AudioGate::Op);
        put(paramIndex(i, OpParam::Pan), AudioKind::OpPan, i);
        put(paramIndex(i, OpParam::Phase), AudioKind::OpPhase, i, -1, field(probe.phase[i]));
        put(paramIndex(i, OpParam::Ratio), AudioKind::OpRatio, i);
        put(paramIndex(i, OpParam::Transpose), AudioKind::OpTranspose, i);
        put(paramIndex(i, OpParam::Detune), AudioKind::OpDetune, i);
        put(paramIndex(i, OpParam::FixedFreq), AudioKind::OpFixedFreq, i);
        put(paramIndex(i, OpParam::UnisonDetune), AudioKind::UnisonDetune, i);
        put(paramIndex(i, OpParam::UnisonSpread), AudioKind::UnisonSpread, i);
        put(paramIndex(i, OpParam::UnisonBlend), AudioKind::UnisonBlend, i);
        put(paramIndex(i, OpParam::WarpAmount), AudioKind::WarpAmount, i);
        put(paramIndex(i, OpParam::NoiseSlope), AudioKind::NoiseSlope, i);
        for (int k = 0; k < static_cast<int>(kOpEnvFields.size()); ++k)
            put(paramIndex(i, kOpEnvFields[static_cast<size_t>(k)]), AudioKind::OpEnv, i, k);
        for (int f = 0; f < kFilters; ++f)
            put(sendIndex(i, f), AudioKind::Level, i, f, field(probe.send[i][f]), AudioGate::OpFilter);
        for (int sh = 0; sh < kShapers; ++sh)
            put(shaperSendIndex(i, sh), AudioKind::Level, i, sh, field(probe.shaperSend[i][sh]),
                AudioGate::OpShaper);
        for (int to = 0; to < kOps; ++to)
            put(matrixIndex(i, to), AudioKind::Route, i, to, field(probe.index[i][to]));
    }
    for (int f = 0; f < kFilters; ++f) {
        for (int to = 0; to < kOps; ++to)
            put(filterModIndex(f, to), AudioKind::Route, kOps + f, to, field(probe.filterIndex[f][to]));
        for (int to = 0; to < kFilters; ++to)
            put(filterLinkIndex(f, to), AudioKind::Level, f, to, field(probe.link[f][to]), AudioGate::FilterFilter);
        for (int sh = 0; sh < kShapers; ++sh)
            put(filterShaperIndex(f, sh), AudioKind::Level, f, sh, field(probe.filterShaper[f][sh]),
                AudioGate::FilterShaper);
        put(paramIndex(f, FilterParam::Out), AudioKind::Level, f, -1, field(probe.filterOut[f]));
        put(paramIndex(f, FilterParam::Mix), AudioKind::Level, f, -1, field(probe.filterMix[f]));
        put(paramIndex(f, FilterParam::Cutoff), AudioKind::Filter, f, 0);
        put(paramIndex(f, FilterParam::KeyTrack), AudioKind::Filter, f, 1);
        put(paramIndex(f, FilterParam::Resonance), AudioKind::Filter, f, 5);
        put(paramIndex(f, FilterParam::Drive), AudioKind::Filter, f, 6);
        put(paramIndex(f, FilterParam::Vowel), AudioKind::Filter, f, 7);
        put(paramIndex(f, FilterParam::Morph), AudioKind::Filter, f, 8);
    }
    for (int sh = 0; sh < kShapers; ++sh) {
        for (int to = 0; to < kOps; ++to)
            put(shaperModIndex(sh, to), AudioKind::Route, kOps + kFilters + sh, to, field(probe.shaperIndex[sh][to]));
        for (int to = 0; to < kShapers; ++to)
            put(shaperShaperIndex(sh, to), AudioKind::Level, sh, to, field(probe.shaperShaper[sh][to]),
                AudioGate::ShaperShaper);
        for (int f = 0; f < kFilters; ++f)
            put(shaperFilterIndex(sh, f), AudioKind::Level, sh, f, field(probe.shaperLink[sh][f]),
                AudioGate::ShaperFilter);
        put(paramIndex(sh, ShaperParam::Out), AudioKind::Level, sh, -1, field(probe.shaperDirect[sh]),
            AudioGate::Shaper);
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        put(fields.drive, AudioKind::Shaper, sh, 0);
        put(fields.output, AudioKind::Shaper, sh, 1);
        put(fields.mix, AudioKind::Shaper, sh, 2);
    }
    put(paramIndex(GlobalParam::Pitch), AudioKind::GlobalPitch, -1);
    put(paramIndex(GlobalParam::FmAmount), AudioKind::FmAmount, -1);
    put(paramIndex(GlobalParam::Pan), AudioKind::GlobalPan, -1);
}

int Synth::audioLane(int target) {
    for (int l = 0; l < audioLaneCount_; ++l)
        if (audioLanes_[l].target == target)
            return l;
    AudioLane &lane = audioLanes_[audioLaneCount_];
    lane = {};
    lane.target = target;
    lane.at = audioTargets_[target];
    return audioLaneCount_++;
}

float *Synth::combLine(const Voice &v, int filter, int channel) {
    const size_t voice = static_cast<size_t>(&v - voices_.data());
    return &combs_[((voice * kFilters + filter) * 2 + channel) * combSize_];
}

void Synth::prepare(double sampleRate, int capacity, int oversampling) {
    // The notes still playing end here, as in reset(): the host waits for each one's end, which the next block
    // reports. (Gathered first: what holds them goes with the voices. The keys the sequencer took, reset() ends.)
    std::vector<NoteRef> ending(ended_.begin(), ended_.end());
    for (const Voice *v : active_)
        if (live(*v))
            ending.push_back(v->note);
    for (const HeldNote &held : held_) // a mono note a later one took over
        ending.push_back(held.note);
    sampleRate_ = sampleRate;
    oversampling_ = voiceFactor(oversampling);
    rate_ = sampleRate * oversampling_;
    bend_ = 0.0;
    globalMod_.fill(0.0);
    sharedStale_ = true;
    seqStale_ = true;
    changedCount_ = 0;
    capacity_ = std::clamp(capacity, 1, kMaxVoices);
    active_.clear(); // first: the voices it points at go, even if there is no memory for the new ones
    voices_.assign(static_cast<size_t>(capacity_ + std::clamp(capacity_, kFadeSlots, kMaxFadeSlots)), Voice{});
    active_.reserve(voices_.size()); // never grows past this on the audio thread
    combSize_ = combLength(rate_);
    combs_.assign(voices_.size() * kFilters * 2 * combSize_, 0.0f);
    for (std::vector<double> &mix : mix_)
        mix.assign(static_cast<size_t>(kControlBlock) * oversampling_, 0.0);
    for (Decimator &d : decimators_)
        d.prepare(sampleRate, oversampling_);
    for (int f = 0; f < kFilters; ++f) {
        filterTaps_[f].assign(static_cast<size_t>(kControlBlock) * oversampling_, 0.0);
        filterDecimators_[f].prepare(sampleRate, oversampling_);
        Analyzer::prepare(filterAnalyzed_[f], kFxSlots + f, kFxSlots + kFilters); // apart from the effects'
    }
    analyzer_.prepare(sampleRate);
    effects_->prepare(sampleRate);
    noiseColor_.prepare(rate_, sampleRate);
    volumeGlide_ = 1.0 - std::exp(-1.0 / (kVolumeGlide * sampleRate));
    voiceFmRange_ = cutoffFmRange(rate_);
    fxFmDelay_ = std::clamp(static_cast<int>(std::lround(decimatorDelay(sampleRate, oversampling_))), 0, kFxFmDelay - 1);
    fxFmDecay_ = std::exp(-1.0 / (kFxFmJump * sampleRate));
    { // the DC blocker's high-pass (see DcBlocker)
        const double k = std::tan(std::numbers::pi * 5.0 / sampleRate), norm = 1.0 / (1.0 + std::sqrt(2.0) * k + k * k);
        dcCoefs_ = {norm, -2.0 * norm, norm, 2.0 * (k * k - 1.0) * norm, (1.0 - std::sqrt(2.0) * k + k * k) * norm};
    }
    ended_.clear();
    // Every note that ends in one block is reported at its end: room for a burst of them far past the voices.
    ended_.reserve(std::max<size_t>({voices_.size() * 2 + 16, kMaxEndedNotes, ending.size()}));
    for (const NoteRef &note : ending)
        reportEnded(note);
    held_.clear();
    held_.reserve(256);
    limit_ = capacity_;
    seq_.prepare(sampleRate);
    reset();
}

void Synth::reset() {
    // The notes still playing end here: the next block reports them, as the host waits for each one's end. (A
    // fading voice's note was reported when it was stolen.)
    for (Voice *v : active_) {
        if (live(*v))
            reportEnded(v->note);
        v->active = v->fading = false;
    }
    active_.clear();
    for (const HeldNote &held : held_) // a mono note a later one took over
        reportEnded(held.note);
    held_.clear();
    releaseKeys({-1, -1, -1, -1});
    seq_.reset();
    lastKey_ = -1.0;
    freeLfos_ = {};
    cc_ = {};
    channelPressure_ = 0.0;
    channelBend_ = {};
    channelPressures_.fill(-1.0);
    channelSlides_.fill(0.5);
    pedal_ = false;
    channelPedals_ = {};
    for (Decimator &d : decimators_)
        d.reset();
    for (int f = 0; f < kFilters; ++f) {
        filterDecimators_[f].reset();
        filterAnalyzed_[f].clear();
    }
    effects_->reset();
    for (int slot = 0; slot < kFxSlots; ++slot) {
        effects_->setCutoffFm(slot, nullptr);
        fxCutoffFm_[slot] = {};
    }
    fxFmLive_ = false;
    fxFmOrder_ = 0;
    fxPitch_ = 60.0;
    fxTail_.on = false;
    fxLast_ = nullptr;
    volumeGain_ = -1.0; // set at once, the first time round
    dcBlockers_ = {};
    outPeak_ = {};
    clipHold_ = 0.0;
    outScope_ = {};
}

void Synth::advanceLfo(Lfo &lfo, double hz, uint32_t frames, uint32_t &rng, bool oneShot, double chaos,
                       LfoLoop loop, CurveRelease *release) const {
    lfo.hz = hz;
    const double before = lfo.phase;
    lfo.phase += hz * frames / sampleRate_;
    if (lfo.left) { // a Curve let go in Sustain: on to its curve's end, where it stays, its offset fading
        if (release && release->fadeTo <= release->fadeFrom)
            release->glide = std::max(release->glide - frames / (kCurveGlide * sampleRate_), 0.0);
        if (lfo.phase >= 1.0) {
            lfo.phase = 1.0;
            lfo.hz = 0.0;
        }
    } else if (oneShot) { // its whole cycle once, whatever its loop, staying at its end
        if (lfo.phase >= 1.0) {
            lfo.phase = 1.0;
            lfo.hz = 0.0;
        }
    } else if (loopPhase(before, lfo.phase, loop)) {
        if (!(loop.end > loop.start)) // a loop of no length: it stays at its start
            lfo.hz = 0.0;
        lfo.from = lfo.held; // Random takes a new value each cycle
        lfo.held = randomStep(lfo.held, chaos, rng);
    }
}

void Synth::noteOn(const NoteRef &note, double velocity) {
    if (note.key < 0 || note.key > 127 || voices_.empty())
        return;
    // In Key and Latch, the sequencer takes the host's keys: they start and transpose its pattern, and no voice plays
    // them (their ends are reported as they come up).
    if (note.port != kSeqPort && note.port != kAuditionPort && seq_.takesKeys()) {
        if (keysTakenCount_ == static_cast<int>(keysTaken_.size())) { // forget the oldest, reporting its end
            reportEnded(keysTaken_[0]);
            std::copy(keysTaken_.begin() + 1, keysTaken_.end(), keysTaken_.begin());
            --keysTakenCount_;
        }
        keysTaken_[keysTakenCount_++] = note;
        seq_.keyDown(note.key);
        return;
    }
    // A key the tuning leaves unmapped plays nothing.
    if (unmapped(note)) {
        reportEnded(note);
        return;
    }
    // A velocity that isn't a number plays as 0, so the note still plays out and ends.
    const HeldNote played{note, std::isfinite(velocity) ? std::clamp(velocity, 0.0, 1.0) : 0.0};
    // Another key is still down, or the pedal holds a note: played legato.
    const bool overlapping =
        !held_.empty() || std::any_of(active_.begin(), active_.end(), [](const Voice *v) { return live(*v) && v->sustained; });
    if (held_.size() == held_.capacity()) { // forget the oldest key rather than allocate
        const NoteRef forgotten = held_.front().note;
        held_.erase(held_.begin());
        if (!voiceHas(forgotten)) // a mono note a later one took over: no voice is left to report its end
            reportEnded(forgotten);
    }
    held_.push_back(played);

    // Glide starts wherever the newest voice is right now, bent as the sequencer bends its note, or at the last note.
    Voice *newest = newestVoice();
    const double from = newest ? newest->glide + newest->seqTune : lastKey_;
    const bool glide = from >= 0.0 && (glideAlways_ || overlapping);
    const double retune = retuneOf(note);
    lastKey_ = note.key + retune;

    if (mono_ && newest) {
        // Mono: the sounding voice moves to the new note. Legato keeps its
        // envelopes running if the previous key is still down.
        moveVoice(*newest, played, !(legato_ && overlapping && !newest->released), glide);
        return;
    }

    // Poly legato: a note played while another key is down joins the voice of the newest key still down, as a mono
    // voice moving to the note would carry on: its operators' and mod envelopes take up that voice's stage and level
    // (at its sustain, they start at their sustain), and its LFOs that start with notes that voice's phase, so the
    // chord swells and wavers together. With no key down, it joins the newest note the pedal holds. Taken before a
    // voice is stolen for it, which may be that one.
    struct Joined {
        std::array<Envelope, kOps> ops;
        std::array<DrawnEnvelope, kOps> drawn;
        std::array<Envelope, kMaxSources> modEnvs;
        std::array<Lfo, kMaxSources> lfos;
        std::array<double, kMaxSources> follow; // its Curves' Slew, where it has run a block (else none yet)
        bool followed = false;
    };
    std::optional<Joined> joined;
    if (legato_ && overlapping) {
        const Voice *with = nullptr;
        for (size_t i = held_.size() - 1; i-- > 0 && !with;) // the keys down before this one, newest first
            with = voicePlaying(held_[i].note);
        if (!with)
            for (const Voice *u : active_)
                if (live(*u) && u->sustained && (!with || u->order > with->order))
                    with = u;
        if (with) {
            joined.emplace();
            for (int op = 0; op < kOps; ++op)
                joined->ops[op] = with->ops[op].env;
            joined->drawn = with->drawn; // its Envelopes follow these from its first block on
            joined->modEnvs = with->modEnvs;
            joined->lfos = with->lfos;
            joined->follow = with->follow;
            joined->followed = !with->fresh;
        }
    }
    // A key struck again while the pedal holds its note: that note goes into its release, the new one taking over.
    for (Voice *u : active_)
        if (live(*u) && u->sustained && u->note.port == note.port && u->note.channel == note.channel &&
            u->note.key == note.key) {
            u->sustained = false;
            releaseVoice(*u);
        }

    Voice &v = allocateVoice();
    // As a new one, built in its place: assigning one would build it on the stack first (some 37 KB) and copy it.
    static_assert(std::is_trivially_destructible_v<Voice>);
    std::construct_at(&v);
    for (std::array<bool, 2> &dirty : v.combDirty) // cleared once a comb reads them (see renderBlock())
        dirty = {true, true};
    setActive(v, true);
    v.note = note;
    v.velocity = played.velocity;
    v.order = noteCounter_++;
    v.rng = static_cast<uint32_t>(v.order * 2654435761u) | 1u;
    v.retune = retune;
    v.glide = glide ? from : note.key + retune;
    v.fresh = true;
    v.newRandom = true;
    v.auxOut.fill(0.0); // a new note's sources have put out nothing yet
    startFromChannel(v);
    if (joined) { // fading in, since its envelopes may start well above nothing
        for (int op = 0; op < kOps; ++op)
            v.ops[op].env = joined->ops[op];
        v.drawn = joined->drawn;
        v.modEnvs = joined->modEnvs;
        v.lfos = joined->lfos;
        v.keepLfos = true;
        // Its Curves' Slew carries on from where the held voice's stood (MIDI sources start afresh, from v.fresh).
        if (joined->followed) {
            v.follow = joined->follow;
            v.keepFollow = true;
        }
        v.fade = 0.0;
        return;
    }
    for (Operator &op : v.ops)
        op.env.start();
    for (DrawnEnvelope &drawn : v.drawn)
        drawn.start();
    for (Envelope &env : v.modEnvs)
        env.start();
    // The first step and the start phase come in the voice's first block. The places past the first kFirstSources draw
    // their steps from a stream of their own, so that the voice's (its noise, its other draws) goes on as it did
    // before they came.
    uint32_t laterRng = static_cast<uint32_t>(v.order * 2246822519u) | 1u;
    for (int s = 0; s < kMaxSources; ++s) {
        Lfo &lfo = v.lfos[s];
        lfo.from = lfo.held = nextRandom(s < kFirstSources ? v.rng : laterRng);
        lfo.restart = true;
    }
}

bool Synth::releaseKeys(const NoteRef &match) {
    bool any = false;
    for (int i = keysTakenCount_; i-- > 0;) {
        if (!matches(match, keysTaken_[i]))
            continue;
        const NoteRef note = keysTaken_[i];
        std::copy(keysTaken_.begin() + i + 1, keysTaken_.begin() + keysTakenCount_, keysTaken_.begin() + i);
        --keysTakenCount_;
        reportEnded(note);
        // The key goes up for the sequencer once no other note holds it (two notes on one key, by their ids or MPE).
        if (std::none_of(keysTaken_.begin(), keysTaken_.begin() + keysTakenCount_,
                         [&](const NoteRef &other) { return other.key == note.key; }))
            seq_.keyUp(note.key);
        any = true;
    }
    // Let go of every key, the sequencer stops (in Key) and what it plays ends.
    if (any && !seq_.wantsProcessing())
        applySeqEvents(seq_.stopAll(seqEvents_));
    return any;
}

void Synth::applySeqEvent(const SeqEvent &e) {
    const NoteRef note{e.id, kSeqPort, 0, static_cast<int16_t>(e.key)};
    if (!e.on) {
        noteOff(note);
        return;
    }
    noteOn(note, e.velocity);
    // A bent note starts where its bend does, and goes on along it through the stretch.
    const SeqBend *bend = nullptr;
    for (const SeqBend &b : seq_.bends())
        if (b.id == e.id)
            bend = &b;
    if (e.bend == 0.0 && !bend)
        return;
    for (Voice *v : active_)
        if (live(*v) && v->note.port == kSeqPort && v->note.noteId == e.id) {
            v->seqTune = e.bend;
            if (bend)
                bendVoice(*v, *bend, seqPlanFrame_);
        }
}

void Synth::bendVoice(Voice &v, const SeqBend &b, uint32_t frame) {
    // Where the bend is `frame` frames into the stretch, and how far it goes a frame from there to the stretch's end.
    if (frame >= seqCovered_ || seqCovered_ <= b.frame) {
        v.seqTune = v.seqTuneTo = b.to;
        v.seqTuneLeft = 0;
        return;
    }
    if (frame > b.frame)
        v.seqTune = b.from + (b.to - b.from) * (frame - b.frame) / static_cast<double>(seqCovered_ - b.frame);
    else
        v.seqTune = b.from;
    v.seqTuneTo = b.to;
    v.seqTuneLeft = seqCovered_ - frame;
    v.seqTuneStep = (b.to - v.seqTune) / v.seqTuneLeft;
}

void Synth::applySeqBends(uint32_t covered) {
    seqCovered_ = covered;
    for (const SeqBend &b : seq_.bends())
        for (Voice *v : active_)
            if (live(*v) && v->note.port == kSeqPort && v->note.noteId == b.id)
                bendVoice(*v, b, 0);
}

void Synth::applySeqEvents(int count) {
    for (int i = 0; i < count; ++i)
        applySeqEvent(seqEvents_[i]);
}

void Synth::noteOff(const NoteRef &match) {
    if (match.port != kSeqPort)
        releaseKeys(match);
    // The sequencer's notes end only as it ends them: a host's note-off with wildcards leaves them alone.
    auto hits = [&](const NoteRef &note) { return matches(match, note) && (note.port != kSeqPort || match.port == kSeqPort); };
    // Let go of the matching keys.
    for (size_t i = held_.size(); i-- > 0;) {
        if (!hits(held_[i].note))
            continue;
        const NoteRef note = held_[i].note;
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        Voice *v = voicePlaying(note);
        if (!v) {
            // A mono note a later one took over: it won't be heard again.
            if (!voiceHas(note))
                reportEnded(note);
            continue;
        }
        // Back to the most recent key still down, as a legato move: one this note-off doesn't let go of too (all
        // notes off leaves none, and the voice releases where it is). A key another voice plays already (notes held
        // in poly as mono came on) isn't played twice: this voice releases instead.
        const auto down = std::find_if(held_.rbegin(), held_.rend(),
                                       [&](const HeldNote &held) { return !hits(held.note); });
        if (mono_ && down != held_.rend() && !voicePlaying(down->note)) {
            reportEnded(note);
            moveVoice(*v, *down, !legato_, true);
        }
    }
    // Release what is left, including voices whose keys weren't tracked (e.g.
    // started before a play-mode change); the pedal holds them instead while it is down.
    for (Voice *v : active_)
        if (live(*v) && !v->released && !v->sustained && hits(v->note))
            letGo(*v);
}

void Synth::letGo(Voice &v) {
    if (pedalHolds(v))
        v.sustained = true;
    else
        releaseVoice(v);
}

bool Synth::pedalHolds(const Voice &v) const {
    return pedal_ || (mpe_ && memberChannel(v.note.channel) && channelPedals_[v.note.channel]);
}

void Synth::releaseUnpedaled() {
    for (Voice *v : active_)
        if (live(*v) && v->sustained && !pedalHolds(*v)) {
            v->sustained = false;
            releaseVoice(*v);
        }
}

NoteRef Synth::channelMatch(int channel) const {
    return {-1, -1, static_cast<int16_t>(mpe_ && memberChannel(channel) ? channel : -1), -1};
}

void Synth::setPedal(int channel, bool down) {
    (mpe_ && memberChannel(channel) ? channelPedals_[channel] : pedal_) = down;
    if (!down)
        releaseUnpedaled();
}

void Synth::allNotesOff(int channel) {
    noteOff(channelMatch(channel));
    stopLatch();
}

// After all notes off, a pattern Latch plays on stops, and the notes it holds end.
void Synth::stopLatch() {
    seq_.unlatch();
    if (seq_.sounding() && !seq_.wantsProcessing())
        applySeqEvents(seq_.stopAll(seqEvents_));
}

void Synth::allSoundOff(int channel) {
    const NoteRef match = channelMatch(channel);
    releaseKeys(match);
    stopLatch();
    for (size_t i = held_.size(); i-- > 0;) {
        if (!matches(match, held_[i].note))
            continue;
        const NoteRef note = held_[i].note;
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        if (!voiceHas(note)) // a mono note a later one took over
            reportEnded(note);
    }
    forEachActive([&](Voice &v) {
        if (live(v) && matches(match, v.note))
            startFade(v); // its note ends now; its sound in a few milliseconds
    });
    if (match.channel < 0)
        effects_->flush();
}

void Synth::choke(const NoteRef &match) {
    if (match.port != kSeqPort)
        releaseKeys(match);
    for (size_t i = held_.size(); i-- > 0;) {
        if (!matches(match, held_[i].note))
            continue;
        const NoteRef note = held_[i].note;
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
        if (!voiceHas(note))
            reportEnded(note);
    }
    forEachActive([&](Voice &v) {
        if (live(v) && matches(match, v.note))
            endVoice(v);
    });
}

void Synth::moveVoice(Voice &v, const HeldNote &to, bool retrigger, bool glide) {
    // A voice still releasing its note, or held by the pedal, whose key is already up, takes the new one: the old
    // note is over. (One whose key is still down reports its end when that key is let go.)
    if (v.released || v.sustained)
        reportEnded(v.note);
    v.note = to.note;
    v.velocity = to.velocity;
    v.released = false;
    v.sustained = false;
    v.silent = false;
    v.tail = false;
    v.order = noteCounter_++;
    v.retune = retuneOf(to.note);
    if (!glide)
        v.glide = to.note.key + v.retune;
    else // from where the sequencer bent the note before
        v.glide += v.seqTune;
    // The new note brings its own host modulation and expressions, and its own bend.
    clearVoiceMods(v);
    v.tuning = 0.0;
    v.seqTune = v.seqTuneTo = v.seqTuneStep = 0.0;
    v.seqTuneLeft = 0;
    v.exprGain = 1.0;
    v.exprPan = 0.0;
    v.pressure = -1.0;
    v.slide = 0.5;
    startFromChannel(v);
    if (retrigger) {
        v.newRandom = true;
        // Envelopes restart from their current level, so there's no click.
        for (Operator &op : v.ops)
            op.env.start();
        for (DrawnEnvelope &drawn : v.drawn)
            drawn.start();
        for (Envelope &env : v.modEnvs)
            env.start();
        for (Lfo &lfo : v.lfos) { // Random carries on from the value it holds (standing still, takes a new one)
            lfo.phase = 0.0;
            lfo.from = lfo.held;
            lfo.restart = true;
            lfo.letGo = lfo.left = false; // a Curve in Sustain loops again
        }
    }
}

// The pressure and slide its channel sent before the note, as they came; pressure stays -1, the master channel's
// standing in, until the channel sends some.
void Synth::startFromChannel(Voice &v) const {
    if (!mpe_ || !memberChannel(v.note.channel))
        return;
    v.pressure = channelPressures_[v.note.channel];
    v.slide = channelSlides_[v.note.channel];
}

void Synth::releaseVoice(Voice &v) {
    v.released = true;
    for (Operator &op : v.ops)
        op.env.release();
    for (DrawnEnvelope &drawn : v.drawn)
        drawn.release();
    for (Envelope &env : v.modEnvs)
        env.release();
    for (Lfo &lfo : v.lfos) // a Curve in Sustain leaves its loop in its next block (see leaveLoop())
        lfo.letGo = true;
}

Synth::Voice *Synth::newestVoice() {
    Voice *newest = nullptr;
    for (Voice &v : active())
        if (live(v) && (!newest || v.order > newest->order))
            newest = &v;
    return newest;
}

Synth::Voice *Synth::voicePlaying(const NoteRef &note) {
    for (Voice &v : active())
        if (live(v) && !v.released && v.note == note)
            return &v;
    return nullptr;
}

bool Synth::voiceHas(const NoteRef &note) const {
    return std::any_of(active_.begin(), active_.end(), [&](const Voice *v) { return live(*v) && v->note == note; });
}

void Synth::noteExpression(const NoteRef &match, int32_t expression, double value) {
    if (!std::isfinite(value))
        return; // not a number, it would spread through the voice's levels and phases, and stay there
    for (Voice &v : active()) {
        if (!live(v) || !matches(match, v.note))
            continue;
        switch (expression) {
        case CLAP_NOTE_EXPRESSION_VOLUME:
            v.exprGain = std::clamp(value, 0.0, 4.0);
            break;
        case CLAP_NOTE_EXPRESSION_PAN:
            v.exprPan = std::clamp(value * 2.0 - 1.0, -1.0, 1.0);
            break;
        case CLAP_NOTE_EXPRESSION_TUNING:
            v.tuning = std::clamp(value, -120.0, 120.0);
            break;
        case CLAP_NOTE_EXPRESSION_PRESSURE:
            v.pressure = std::clamp(value, 0.0, 1.0);
            break;
        case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
            v.slide = std::clamp(value, 0.0, 1.0);
            break;
        }
    }
}

void Synth::setMpe(bool on, double bendRange) {
    const bool off = mpe_ && !on;
    if (off) {
        channelBend_ = {};
        channelPressures_.fill(-1.0);
        channelSlides_.fill(0.5);
        channelPedals_ = {};
    }
    mpe_ = on;
    if (off) // what only a member channel's pedal held goes
        releaseUnpedaled();
    if (std::isfinite(bendRange))
        mpeBendRange_ = std::clamp(bendRange, 0.0, 96.0);
}

void Synth::pitchBend(int channel, double amount) {
    if (!std::isfinite(amount))
        return;
    (mpe_ && memberChannel(channel) ? channelBend_[channel] : bend_) = std::clamp(amount, -1.0, 1.0);
}

void Synth::controlChange(int channel, int controller, double value) {
    if (controller < 0 || controller >= 128 || !std::isfinite(value))
        return;
    value = std::clamp(value, 0.0, 1.0);
    if (mpe_ && memberChannel(channel) && controller == 74) { // the slide of the note on that channel
        channelSlides_[channel] = value;
        for (Voice &v : active())
            if (live(v) && v.note.channel == channel)
                v.slide = value;
        return;
    }
    cc_[controller] = value;
    if (controller == 64) // the sustain pedal, down from the middle of its travel on
        setPedal(channel, value >= 0.5);
    else if (controller == 120)
        allSoundOff(channel);
    else if (controller == 123)
        allNotesOff(channel);
}

void Synth::channelPressure(int channel, double value) {
    if (!std::isfinite(value))
        return;
    value = std::clamp(value, 0.0, 1.0);
    if (mpe_ && memberChannel(channel)) { // the pressure of the note on that channel
        channelPressures_[channel] = value;
        for (Voice &v : active())
            if (live(v) && v.note.channel == channel)
                v.pressure = value;
        return;
    }
    channelPressure_ = value;
}

void Synth::paramMod(int index, const NoteRef &match, double amount) {
    if (index < 0 || index >= kParamCount || !(table_[index].flags & CLAP_PARAM_IS_MODULATABLE) ||
        !std::isfinite(amount))
        return;
    if (match == NoteRef{-1, -1, -1, -1}) {
        globalMod_[index] = amount;
        paramChanged(index); // what render() works out from it (see render())
        return;
    }
    if (index >= kVoiceParamCount)
        return; // the effects, macros and the rest are shared by all voices
    for (Voice &v : active())
        if (live(v) && matches(match, v.note))
            setVoiceMod(v, index, amount);
}

void Synth::applyMacros(const ParamValues &params) {
    for (int i = 0; i < macroTargetCount_; ++i)
        macroMod_[macroTargets_[i]] = 0.0;
    macroTargetCount_ = 0;
    auto get = [this, &params](int i) { return hostValue(params, i); };
    // The knobs first, as the macros' and the mod wheel's slots on them take them, then what the knobs move.
    macroGraph_ = macroGraph(get);
    macroSet_ = macroKnobsSet(get);
    for (int m = 0; m < kMacros; ++m) { // a curve playing holds its knob, the host's modulation on top; the editor shows it
        const double set = macroSet_[m];
        if (seqMacroHeld_ >> m & 1u)
            macroSet_[m] = std::clamp(seqMacro_[m] + globalMod_[paramIndex(m, MacroParam::Value)], 0.0, 1.0);
        seqMacroAdded_[m] = macroSet_[m] - set;
    }
    macroKnobs_ = macroSet_;
    runMacroGraph(macroGraph_, get, macroKnobs_, nullptr, [](int) { return -1.0; }, [](int, int, double) {});
    voiceMovedSlotCount_ = 0;
    forEachMacroMod(get, macroKnobs_, [&](int target, double offset, int source, int slot) {
        macroMod_[target] += offset;
        macroTargets_[macroTargetCount_++] = target;
        // A slot whose knob, or aux's, the voices' sources move: each voice takes this back for its own.
        const int aux = slotAux(get, source, slot);
        if (!voiceMovedMacro(source) && !(aux >= 0 && voiceMovedMacro(aux)))
            return;
        const bool bipolar = sourceBipolar(get, source);
        voiceMovedSlots_[voiceMovedSlotCount_++] = {
            source,
            slot,
            target,
            aux,
            get(slotCurveIndex(source, slot)),
            get(slotAmountIndex(source, slot)) * get(slotScaleIndex(source, slot)) *
                (table_[target].max - table_[target].min),
            get(slotAuxDepthIndex(source, slot)),
            bipolar,
            offset};
    });
    voiceMacroSlotCount_ = 0;
    for (int source = kMaxSources; source < kAllSources; ++source) {
        if (!hasKnob(source))
            continue;
        for (int slot = 0; slot < kSlots; ++slot) {
            const int target = slotTarget(params[slotTargetIndex(source, slot)]);
            // (The sequencer's feel too, past kModulatedCount: feedSeqFeel() takes it from the newest voice.)
            if (const int aux = slotAux(get, source, slot); aux >= 0 && runsPerVoice(aux) && target >= 0 &&
                                                             (target < kModulatedCount || isSeqFeel(target)) &&
                                                             params[slotBypassIndex(source, slot)] < 0.5)
                voiceMacroSlots_[voiceMacroSlotCount_++] = {source, slot};
        }
    }
}

void Synth::render(float *left, float *right, uint32_t frames, const ParamValues &params, bool sameParams) {
    // The patch's own tuning, key by key, as its params may have changed: what the notes started from here play in.
    if (!sameParams || !patchTuningRead_) {
        tuningTable(params, patchTuning_);
        patchTuningRead_ = true;
    }
    // The sequencer plans its notes a stretch at a time, and the voices render up to each note it starts or ends, at
    // its sample, and up to where its curves move on; through the stretches between, they run on. Its pattern is read
    // again only when its params may have changed.
    seq_.configure(params, seqStale_ || !sameParams);
    seqStale_ = false;
    // Switched off, set to Host or played by the Play button: the keys it took are over.
    if (keysTakenCount_ > 0 && !seq_.takesKeys())
        releaseKeys({-1, -1, -1, -1});
    if (!seq_.enabled()) {
        if (seq_.sounding())
            applySeqEvents(seq_.stopAll(seqEvents_));
        feedSeqFeel(params); // for the editor, which shows where modulation takes them
        followSeqCurves();
        renderSpan(left, right, frames, params, sameParams);
        return;
    }
    constexpr uint32_t kSeqStretch = 256;
    uint32_t rendered = 0; // the frames the voices have rendered; those planned past it, they render on through
    for (uint32_t done = 0; done < frames;) {
        uint32_t covered = 0;
        // Where the transport stands as the stretch starts: renderSpan() moves it on through the frames it renders.
        const double beats = playing_ ? songBeats_ + (done - rendered) / sampleRate_ * tempo_ / 60.0 : songBeats_;
        feedSeqFeel(params);
        const int count =
            seq_.plan(std::min(frames - done, kSeqStretch), tempo_, playing_, beats, seqEvents_, covered);
        seqCovered_ = std::max(covered, 1u);
        // The voices hear the curves where they stood up to here, and the bent notes' bends from here.
        const bool moved = seqCurvesMoved();
        if (moved || !seq_.bends().empty()) {
            if (done > rendered) {
                renderSpan(left + rendered, right + rendered, done - rendered, params, sameParams);
                sameParams = true;
                rendered = done;
            }
            if (moved)
                followSeqCurves();
            applySeqBends(seqCovered_);
        }
        for (int i = 0; i < count; ++i) {
            const uint32_t frame = done + std::min(seqEvents_[i].frame, covered);
            if (frame > rendered) {
                renderSpan(left + rendered, right + rendered, frame - rendered, params, sameParams);
                sameParams = true;
                rendered = frame;
            }
            seqPlanFrame_ = frame - done;
            applySeqEvent(seqEvents_[i]);
        }
        done += std::max(covered, 1u);
    }
    if (frames > rendered)
        renderSpan(left + rendered, right + rendered, frames - rendered, params, sameParams);
}

double Synth::seqCurveValue(SeqCurve c) const {
    const double v = seq_.curve(c);
    return std::isfinite(v) ? v : 0.0;
}

bool Synth::seqCurvesMoved() const {
    if (seqCurveValue(SeqCurve::Bend) != seqBend_ || seqCurveValue(SeqCurve::ModWheel) != seqWheel_)
        return true;
    for (int m = 0; m < kMacros; ++m) {
        const double knob = seq_.curve(seqMacroCurve(m));
        const bool held = std::isfinite(knob);
        if (held != ((seqMacroHeld_ >> m & 1u) != 0) || (held && knob != seqMacro_[m]))
            return true;
    }
    return false;
}

void Synth::followSeqCurves() {
    seqBend_ = seqCurveValue(SeqCurve::Bend);
    const double wheel = seqCurveValue(SeqCurve::ModWheel);
    if (wheel != seqWheel_) { // the mod wheel's knob moved, and what it moves with it
        seqWheel_ = wheel;
        paramChanged(paramIndex(GlobalParam::ModWheel));
    }
    uint32_t heldNow = 0;
    for (int m = 0; m < kMacros; ++m) {
        const double knob = seq_.curve(seqMacroCurve(m));
        const bool held = std::isfinite(knob);
        if (held)
            heldNow |= 1u << m;
        if (held != ((seqMacroHeld_ >> m & 1u) != 0) || (held && knob != seqMacro_[m])) {
            seqMacro_[m] = held ? knob : 0.0; // the macro's knob moved, and what it moves with it
            paramChanged(paramIndex(m, MacroParam::Value));
        }
    }
    seqMacroHeld_ = heldNow;
}

void Synth::renderSpan(float *left, float *right, uint32_t frames, const ParamValues &params, bool sameParams) {
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    bendSet_ = params[paramIndex(GlobalParam::PitchBend)];
    // What follows from the params and the host's modulation for every voice alone, worked out again only when one
    // of them may have changed since the last render(): a block split by events renders many times over. A few that
    // a host's automation or modulation moved, each only summed with the rest, take only their own sums again.
    if (!sameParams || sharedStale_)
        shareAll(params);
    else if (changedCount_ > 0)
        shareChanged(params);
    changedCount_ = 0;
    // Anchored LFOs go to where the transport stands: it may have jumped since the last render().
    runSharedLfos(params, newestVoice(), 0, 0);
    for (uint32_t done = 0; done < frames; done += kControlBlock) {
        const uint32_t n = std::min(kControlBlock, frames - done);
        for (int sh = 0; sh < kShapers; ++sh) // a drawn curve glides to new points, block by block
            if (clipMode(params[shaperParams(kFxSlots + sh).mode]) == ClipMode::Custom)
                shaperTables_[sh].advance(n / sampleRate_);
        // The voices add up at their own rate, which comes down to the host's
        // before the effects. With no voice sounding, the way down starts from
        // silence again: voices end only once they are quiet, or when choked,
        // which should be silent at once.
        for (std::vector<double> &mix : mix_)
            std::fill_n(mix.begin(), n * oversampling_, 0.0);
        if (analyzing_)
            for (std::vector<double> &tap : filterTaps_)
                std::fill_n(tap.begin(), n * oversampling_, 0.0);
        // The scope follows the newest voice; a voice newer than the one it took starts it again.
        scopeVoice_ = nullptr;
        if (scope_.op >= 0) {
            scopeVoice_ = newestVoice();
            if (scopeVoice_ && scopeVoice_->order != scope_.order) {
                scope_.order = scopeVoice_->order;
                scopeRestart();
            }
            scope_.live = scopeVoice_ != nullptr;
        }
        // The filter effects' FM follows the newest voice (see runOpSources()).
        fxFmCount_ = 0;
        fxFmOps_ = 0;
        fxFmVoice_ = nullptr;
        fxFmWanted_ = fxFmTargets_ != 0 && fxFmWantedNow(params);
        if (fxFmWanted_) {
            fxFmVoice_ = newestVoice();
            for (std::array<float, kFxFmBlock> &out : fxOpOut_)
                out.fill(0.0f);
        }
        bool sounding = false;
        forEachActive([&](Voice &v) { // in their order, as they always summed; one may end as it renders
            renderBlock(v, mix_[0].data(), mix_[1].data(), n, params);
            sounding = true;
        });
        for (int c = 0; c < 2; ++c) {
            if (sounding) {
                float *out = (c == 0 ? left : right) + done;
                decimators_[c].process(mix_[c].data(), out, static_cast<int>(n));
                // The voices' DC taken out before the effects (see DcBlocker); a block of silence clears it too.
                if (std::all_of(out, out + n, [](float x) { return x == 0.0f; }))
                    dcBlockers_[c] = {};
                else
                    for (uint32_t s = 0; s < n; ++s)
                        out[s] = dcBlockers_[c].run(out[s], dcCoefs_);
            } else {
                decimators_[c].reset();
                dcBlockers_[c] = {};
            }
        }
        if (analyzing_)
            analyzeFilters(n, sounding, params);

        // The effects are shared by all voices, so LFOs and envelopes reach them
        // through the newest voice, like the editor shows; a filter's key tracking follows its pitch. Once the last
        // voice has ended, its modulation runs on as the effects ring out (see fxTailBlock()).
        const Voice *newest = newestVoice();
        const SlotMod *fxMods = nullptr;
        int fxModCount = 0;
        if (newest) {
            fxTail_.on = false;
            fxLast_ = newest;
            fxLastOrder_ = newest->order;
            fxMods = newest->slotMods.data();
            fxModCount = newest->slotModCount;
            fxPitch_ = newest->pitch;
        } else {
            // The voice newest a block ago has ended (in this block's render, or between two): its data is as it left
            // it, unless a note has taken it over since.
            if (fxLast_ && fxLast_->order == fxLastOrder_ && !fxLast_->active)
                startFxTail(*fxLast_, params);
            fxLast_ = nullptr;
            if (fxTail_.on) {
                fxMods = fxTailMods_.data();
                fxModCount = fxTailModCount_ = fxTailBlock(params, n);
            }
        }
        // The params the last block's slots moved go back as they were; then each param the slots move takes their
        // offsets in their order, and its range.
        auto inRange = [this](int i, double value) { return std::clamp(value, paramMin_[i], paramMax_[i]); };
        for (int k = 0; k < fxMovedCount_; ++k)
            fxParams_[fxMoved_[k]] = inRange(fxMoved_[k], fxBase_[fxMoved_[k] - kFxBase]);
        fxMovedCount_ = 0;
        for (int m = 0; m < fxModCount; ++m) {
            const int i = fxMods[m].index;
            if (i >= kFxBase && i < kShapeBase) { // a param moved twice is listed twice: that does no harm
                fxParams_[i] = fxBase_[i - kFxBase];
                fxMoved_[fxMovedCount_++] = i;
            }
        }
        for (int m = 0; m < fxModCount; ++m)
            if (fxMods[m].index >= kFxBase && fxMods[m].index < kShapeBase)
                fxParams_[fxMods[m].index] += fxMods[m].offset;
        for (int k = 0; k < fxMovedCount_; ++k)
            fxParams_[fxMoved_[k]] = inRange(fxMoved_[k], fxParams_[fxMoved_[k]]);

        // So are the free and anchored LFOs' clocks, which run even while no voice listens.
        runSharedLfos(params, newest, n, done + n);
        if (fxFmWanted_ || fxFmLive_)
            runFxCutoffFm(n);
        effects_->process(left + done, right + done, n, fxParams_, tempo_, fxPitch_);
        // The tail's modulation stops once the effects have been silent for a while: the params then go back to
        // their knobs where nothing is heard, and the effects can rest.
        if (fxTail_.on) {
            float peak = 0.0f;
            for (uint32_t s = done; s < done + n; ++s)
                peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
            fxTail_.silent = peak < kFxTailFloor ? fxTail_.silent + n / sampleRate_ : 0.0;
            if (fxTail_.silent >= kFxTailHold)
                fxTail_.on = false;
        }

        // Volume, after the effects: what is set, host and macro modulation, and the slots' modulation, as the
        // effects take theirs. It glides to a new level sample by sample, however the block is split.
        const int volume = paramIndex(GlobalParam::Volume);
        double db = params[volume] + globalMod_[volume] + macroMod_[volume];
        for (int m = 0; m < fxModCount; ++m)
            if (fxMods[m].index == volume)
                db += fxMods[m].offset;
        const double gain =
            std::pow(10.0, std::clamp(db, table_[volume].min, table_[volume].max) / 20.0) / kVoiceLevel;
        if (volumeGain_ < 0.0)
            volumeGain_ = gain;
        for (uint32_t s = done; s < done + n; ++s) {
            volumeGain_ += (gain - volumeGain_) * volumeGlide_;
            left[s] = static_cast<float>(left[s] * volumeGain_);
            right[s] = static_cast<float>(right[s] * volumeGain_);
        }
    }
    meterOutput(left, right, frames);
    if (analyzing_)
        captureOutput(left, right, frames);
    if (playing_) {
        songSeconds_ += frames / sampleRate_;
        songBeats_ += frames / sampleRate_ * tempo_ / 60.0;
    }
}

void Synth::shareAll(const ParamValues &params) {
    sharedStale_ = false;
    applyMacros(params);
    for (int from = 0; from < kRouteSources; ++from)
        for (int to = 0; to < kOps; ++to)
            routeModes_[from][to] = from == to ? RouteMode::Phase : routeMode(params[routeModeIndex(from, to)]);
    voiceBaseReady_ = false; // what the voices share is summed again for these params, once a voice needs it
    for (int op = 0; op < kOps; ++op) // the Custom waves take their partials, built again once played (renderBlock())
        customWaves_[op].update(
            partialCount(params[partialCountIndex(op)]), [&](int k) { return params[partialLevelIndex(op, k)]; },
            [&](int k) { return params[partialPhaseIndex(op, k)]; });
    for (int sh = 0; sh < kShapers; ++sh) { // and the shapers' drawn curves their points
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        if (clipMode(params[fields.mode]) == ClipMode::Custom)
            shaperTables_[sh].update([&params](int index) { return params[index]; }, fields.curve);
    }
    // The effects' params with the host's and the macros' modulation, which stay as they are through a
    // render(): as summed, and in their ranges in fxParams_, where each control block adds the newest voice's
    // slot modulation to the params it moves (and puts back those it moved before: fxMoved_, kept while the
    // params stay). Their drawn curves are read as set.
    for (int i = kFxBase; i < kShapeBase; ++i) {
        fxBase_[i - kFxBase] = params[i] + globalMod_[i] + macroMod_[i];
        fxParams_[i] = std::clamp(fxBase_[i - kFxBase], paramMin_[i], paramMax_[i]);
    }
    fxMovedCount_ = 0;
    std::copy(params.begin() + kClipCurveBase, params.begin() + kSlotBypassBase,
              fxParams_.begin() + kClipCurveBase);
    sharedSlotsReady_ = false;
}

namespace {

// Whether the rest of what render() works out from the params (see Synth::shareAll()) reads param `index`, or its
// host modulation: the macros (their knobs and slots, and the slots on them), the slots' wiring and depths, the
// route modes, the Custom waves and the drawn curves. A param it doesn't read is only summed with the host's and the
// macros' modulation, for the voices or the effects, and only its own sum need be worked out again.
bool changesShared(int index) {
    static const std::vector<bool> reads = [] {
        std::vector<bool> r(kParamCount, false);
        for (int i = kShapeBase; i < kParamCount; ++i) // the drawn shapes and everything after them
            r[i] = true;
        for (int source = 0; source < kAllSources; ++source)
            for (int slot = 0; slot < kSlots; ++slot)
                for (const int i : {slotTargetIndex(source, slot), slotAmountIndex(source, slot),
                                    slotAuxIndex(source, slot), slotScaleIndex(source, slot),
                                    slotCurveIndex(source, slot), slotAuxDepthIndex(source, slot),
                                    slotBypassIndex(source, slot)})
                    r[i] = true;
        for (int source = kMaxSources; source < kAllSources; ++source) // the knobs: the Mod Wheel's is a global
            if (hasKnob(source))
                r[stripKnobIndex(source)] = true;
        for (int source = 0; source < kMaxSources; ++source) // what a place holds, which says how it reads its fields
            r[paramIndex(source, SourceParam::Type)] = true;
        for (int sh = 0; sh < kShapers; ++sh) // whether a voice shaper plays a drawn curve
            r[shaperParams(kFxSlots + sh).mode] = true;
        return r;
    }();
    return reads[index];
}

} // namespace

void Synth::shareChanged(const ParamValues &params) {
    for (int k = 0; k < changedCount_; ++k)
        if (changesShared(changed_[k])) {
            shareAll(params);
            return;
        }
    // As shareAll() and shareVoiceParams() sum them, sum for sum, so the voices and the effects play as they would.
    for (int k = 0; k < changedCount_; ++k) {
        const int i = changed_[k];
        const double base = params[i] + globalMod_[i] + macroMod_[i];
        if (i >= kFxBase) {
            fxBase_[i - kFxBase] = base;
            fxParams_[i] = std::clamp(base, paramMin_[i], paramMax_[i]); // a param a slot moves takes it each block
        } else if (voiceBaseReady_) { // else summed with the rest when a voice needs them
            voiceBase_[i] = base;
            voiceClamped_[i] = std::clamp(base + 0.0, paramMin_[i], paramMax_[i]);
            voiceParams_[i] = voiceClamped_[i]; // and where the last voice moved it, put back as its first block starts
        }
    }
}

bool Synth::holdsKeys() const {
    auto byKey = [](const NoteRef &note) { return note.port != kSeqPort && note.port != kAuditionPort; };
    if (keysTakenCount_ > 0)
        return true;
    for (const HeldNote &held : held_)
        if (byKey(held.note))
            return true;
    for (const Voice *v : active_)
        if (live(*v) && !v->released && byKey(v->note)) // a pedal's note too (sustained, not released)
            return true;
    return false;
}

void Synth::feedSeqFeel(const ParamValues &params) {
    // What is set, with the host's and the macros' modulation, and the slots of the newest voice, or with none, the
    // last one's as the effects ring out (see fxTailBlock()), as the effects take theirs: the sequencer is shared too.
    std::array<double, kSeqFeelCount> feel{};
    for (int k = 0; k < kSeqFeelCount; ++k) {
        const int i = seqFeelIndex(k);
        feel[k] = params[i] + globalMod_[i] + macroMod_[i];
    }
    const Voice *newest = newestVoice();
    const SlotMod *mods = newest ? newest->slotMods.data() : fxTail_.on ? fxTailMods_.data() : nullptr;
    const int count = newest ? newest->slotModCount : fxTail_.on ? fxTailModCount_ : 0;
    for (int m = 0; m < count; ++m)
        if (const int k = seqFeelOf(mods[m].index); k >= 0)
            feel[k] += mods[m].offset;
    for (int k = 0; k < kSeqFeelCount; ++k) {
        const int i = seqFeelIndex(k);
        feel[k] = std::isfinite(feel[k]) ? std::clamp(feel[k], table_[i].min, table_[i].max) : table_[i].def;
        seqFeel_[k] = feel[k] - std::clamp(params[i], table_[i].min, table_[i].max);
    }
    seq_.setFeel(feel);
}

void Synth::startFxTail(const Voice &v, const ParamValues &params) {
    fxTail_.on = true;
    fxTail_.silent = 0.0;
    fxTail_.lfos = v.lfos;
    fxTail_.curveReleases = v.curveReleases;
    fxTail_.follow = v.follow;
    fxTail_.auxOut = v.auxOut;
    std::fill_n(fxTail_.auxOut.begin() + kOpSource, kOps, 0.0); // the voice's operators have stopped
    fxTail_.rng = v.rng;
    // The LFOs' slots are worked out again each block; the other sources' keep what they gave last.
    fxTail_.heldCount = 0;
    const int volume = paramIndex(GlobalParam::Volume);
    for (int m = 0; m < v.slotModCount; ++m) {
        const SlotMod &mod = v.slotMods[m];
        if ((mod.index < kFxBase || mod.index >= kShapeBase) && mod.index != volume && !isSeqFeel(mod.index))
            continue;
        const int source = mod.slot / kSlots;
        if (source < kMaxSources && runsLfo(sourceType(params[paramIndex(source, SourceParam::Type)])))
            continue;
        // The voice's operators have stopped: nothing of a moment of their wave stays held, as a source or an aux.
        if (isOpSource(source) ||
            isOpSource(slotAux([&params](int i) { return params[i]; }, source, mod.slot % kSlots)))
            continue;
        fxTail_.held[fxTail_.heldCount++] = mod;
    }
}

int Synth::fxTailBlock(const ParamValues &params, uint32_t frames) {
    // The sources' params as every voice has them: what is set, with the host's and the macros' modulation.
    auto get = [&](int i) { return std::clamp(params[i] + globalMod_[i] + macroMod_[i], table_[i].min, table_[i].max); };
    if (!sharedSlotsReady_)
        shareSlots(params);
    const int volume = paramIndex(GlobalParam::Volume);
    int count = fxTail_.heldCount;
    std::copy_n(fxTail_.held.begin(), count, fxTailMods_.begin());
    fxTail_.lfoMask = 0;
    for (int s = 0; s < kMaxSources; ++s) {
        if (!runsLfo(sourceType(params[paramIndex(s, SourceParam::Type)])) || params[sourceEnabledIndex(s)] < 0.5)
            continue;
        // A free or anchored LFO goes on along its shared clock, as a voice's would; another from where it stood. A
        // Curve in Sustain was let go with its voice, and goes on to its curve's end.
        Lfo &lfo = fxTail_.lfos[s];
        const bool free = lfoShared(get, s);
        const LfoLoop loop = lfoLoop(get, s);
        if (free)
            lfo = freeLfos_[s];
        else if (!lfoSustain(get, s))
            lfo.left = false;
        else if (!lfo.left)
            leaveLoop(params, s, lfo, fxTail_.curveReleases[s], loop);
        const double curve = lfoOutput(get, params, s, lfo);
        double value = lfo.left ? std::clamp(curve + leftOffset(lfo, fxTail_.curveReleases[s]), -1.0, 1.0) : curve;
        // A Curve's Slew goes on from where the voice's stood, as it would have in the voice.
        if (sourceType(params[paramIndex(s, SourceParam::Type)]) == SourceType::Curve)
            value = curveSlew(fxTail_.follow[s], value, get(paramIndex(s, SourceParam::CurveSlew)), false, frames);
        const bool bipolar = sourceBipolar(get, s);
        fxTail_.auxOut[s] = 0.5 * (value + 1.0);
        const double swing = bipolar ? 0.5 * value : 0.5 * (value + 1.0);
        for (int k = 0; k < sharedSlotCount_[s]; ++k) {
            const SharedSlot &slot = sharedSlots_[s][k];
            if ((slot.target < kFxBase || slot.target >= kShapeBase) && slot.target != volume && !isSeqFeel(slot.target))
                continue;
            const double amount = hostValue(params, slotAmountIndex(s, slot.slot));
            if (amount == 0.0)
                continue;
            const double scale =
                slot.voiceAux >= 0 ? slot.scale * auxScale(slot.auxDepth, fxTail_.auxOut[slot.voiceAux]) : slot.scale;
            fxTailMods_[count++] = {slot.target, slotCurveValue(swing, slot.curve, bipolar) * amount * scale * slot.range,
                                    s * kSlots + slot.slot};
            fxTail_.lfoMask |= 1u << s;
        }
        if (!free)
            advanceLfo(lfo, lfoHz(get, s, tempo_), frames, fxTail_.rng, lfoOneShot(get, s),
                       get(paramIndex(s, SourceParam::RandomChaos)), loop, &fxTail_.curveReleases[s]);
    }
    return count;
}

void Synth::runSharedLfos(const ParamValues &params, const Voice *newest, uint32_t frames, uint32_t end) {
    // Each param read below would look through all of the newest voice's slots: those on the sources' params, usually
    // none, are gathered once instead.
    int sourceModCount = 0;
    if (newest)
        for (int m = 0; m < newest->slotModCount; ++m)
            if (sourceOf(newest->slotMods[m].index) >= 0)
                sourceMods_[sourceModCount++] = newest->slotMods[m];
    auto shared = [&](int i) {
        double value = params[i] + globalMod_[i] + macroMod_[i];
        if (newest) {
            value += newest->mod[i];
            for (int m = 0; m < sourceModCount; ++m)
                if (sourceMods_[m].index == i)
                    value += sourceMods_[m].offset;
        }
        return std::clamp(value, table_[i].min, table_[i].max);
    };
    for (int s = 0; s < kMaxSources; ++s) {
        if (!runsLfo(sourceType(params[paramIndex(s, SourceParam::Type)])))
            continue;
        Lfo &lfo = freeLfos_[s];
        const double chaos = shared(paramIndex(s, SourceParam::RandomChaos));
        const LfoLoop loop = lfoLoop(shared, s); // a Curve's cycles only its loop region
        if (!playing_ || !lfoAnchored(shared, s)) {
            if (frames > 0)
                advanceLfo(lfo, lfoHz(shared, s, tempo_), frames, freeRng_, false, chaos, loop);
            continue;
        }
        // The cycles it has run since the song's start: synced, a note value's beats each, else at its rate. Its
        // Phase is where it stood at the start. A random one is never anchored (see lfoAnchored()).
        const double seconds = end / sampleRate_;
        const double cycles =
            shared(paramIndex(s, SourceParam::Sync)) >= 0.5
                ? (songBeats_ + seconds * tempo_ / 60.0) /
                      divisionBeats(static_cast<int>(std::lround(shared(paramIndex(s, SourceParam::Division)))))
                : (songSeconds_ + seconds) * sweepRateHz(shared(paramIndex(s, SourceParam::Rate)));
        lfo.hz = lfoHz(shared, s, tempo_);
        lfo.phase = anchoredPhase(cycles, shared(paramIndex(s, SourceParam::Phase)), loop);
    }
}

void Synth::meterOutput(const float *left, const float *right, uint32_t frames) {
    const double seconds = frames / sampleRate_;
    const double fall = std::pow(10.0, -kMeterFallDb * seconds / 20.0);
    clipHold_ = std::max(clipHold_ - seconds, 0.0);
    for (int c = 0; c < 2; ++c) {
        const float *x = c == 0 ? left : right;
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames; ++i)
            peak = std::max(peak, std::abs(x[i]));
        outPeak_[c] = std::max(static_cast<double>(peak), outPeak_[c] * fall);
        if (peak > 1.0f)
            clipHold_ = kClipHold;
    }
}

namespace {

// The output scope's interpolation (see Synth::OutScopeCapture): for a place `row / kRows` of the way from one sample
// to the next, each of the samples around it weighed by a Hann-windowed sinc, the weights summing to 1. Built as the
// library loads.
constexpr int kOutScopeRows = 256;
constexpr int Taps = kOutScopeTaps;
using OutScopeKernel = std::array<std::array<float, Taps>, kOutScopeRows + 1>;
OutScopeKernel outScopeKernel() {
    OutScopeKernel kernel{};
    constexpr double half = Taps / 2;
    for (int row = 0; row <= kOutScopeRows; ++row) {
        const double at = static_cast<double>(row) / kOutScopeRows;
        double sum = 0.0;
        std::array<double, Taps> weights{};
        for (int k = 0; k < Taps; ++k) {
            const double u = half - 1.0 + at - k; // from the place to sample k, oldest first
            const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
            const double window = std::abs(u) < half ? 0.5 * (1.0 + std::cos(std::numbers::pi * u / half)) : 0.0;
            weights[k] = sinc * window;
            sum += weights[k];
        }
        for (int k = 0; k < Taps; ++k)
            kernel[row][k] = static_cast<float>(weights[k] / sum);
    }
    return kernel;
}
const OutScopeKernel kOutScopeKernel = outScopeKernel();

} // namespace

void Synth::captureOutput(const float *left, const float *right, uint32_t frames) {
    OutScopeCapture &s = outScope_;
    // A new note starts the spans over as it starts (this render() is its first), so a wave that starts at 0 shows
    // from about its start, but for the decimators' delay.
    if (const Voice *newest = newestVoice(); newest && newest->order != s.order) {
        s.order = newest->order;
        s.phase = 0.0;
        s.next = 0;
    }
    const double hz = std::clamp(440.0 * std::pow(2.0, (fxPitch_ - 69.0) / 12.0), 1.0, 0.5 * sampleRate_);
    const double periods = kOutScopePeriods * std::ceil(std::max(kOutScopeMinSpan * hz / kOutScopePeriods, 1.0));
    const double inc = hz / (periods * sampleRate_); // of a span, a sample
    auto place = [](int point) { return static_cast<double>(point) / (kOutScopeBins - 1); }; // 0..1 through the span
    const auto slow = static_cast<int64_t>(kScopeBlendSpan * sampleRate_);
    for (uint32_t i = 0; i < frames; ++i) {
        ++s.blend.since;
        double x = 0.5 * (static_cast<double>(left[i]) + right[i]);
        if (!std::isfinite(x))
            x = 0.0;
        s.newest = (s.newest + 1) % kOutScopeTaps;
        s.history[s.newest] = static_cast<float>(x);
        // The points the phase passes from the sample before to this one, in between them (taken kOutScopeTaps / 2 - 1
        // samples back). The last point is the span's end, where the next one starts: its first point, at the same
        // place.
        double from = s.phase, to = from + inc;
        for (;;) {
            while (s.next < kOutScopeBins && place(s.next) <= to) {
                const double at = std::clamp((place(s.next) - from) / inc, 0.0, 1.0);
                const auto &weights = kOutScopeKernel[static_cast<int>(std::lround(at * kOutScopeRows))];
                float value = 0.0f;
                for (int k = 0; k < kOutScopeTaps; ++k) // oldest first
                    value += weights[k] * s.history[(s.newest + 1 + k) % kOutScopeTaps];
                s.filling[s.next++] = value;
            }
            if (to < 1.0)
                break;
            if (s.next == kOutScopeBins) { // whole: published
                s.blend.publish(s.frame, kOutScopeBins, true, slow);
                s.frame = s.filling;
            }
            s.next = 0;
            from -= 1.0;
            to -= 1.0;
        }
        s.phase = to;
    }
}

// A filter that is switched off measures nothing; one that is on goes on measuring while nothing plays, so what it
// showed falls away.
void Synth::analyzeFilters(uint32_t hostFrames, bool sounding, const ParamValues &params) {
    for (int f = 0; f < kFilters; ++f) {
        Analyzed &analyzed = filterAnalyzed_[f];
        if (params[paramIndex(f, FilterParam::Enabled)] < 0.5) {
            analyzed.bands = {};
            continue;
        }
        std::array<float, kControlBlock> out{};
        if (sounding)
            filterDecimators_[f].process(filterTaps_[f].data(), out.data(), static_cast<int>(hostFrames));
        else
            filterDecimators_[f].reset();
        for (uint32_t s = 0; s < hostFrames; ++s)
            analyzer_.push(analyzed, out[s]);
    }
}

bool Synth::isActive(const NoteRef &note) const {
    return std::any_of(active_.begin(), active_.end(), [&](const Voice *v) {
        return live(*v) && v->note.port == note.port && v->note.channel == note.channel && v->note.key == note.key &&
               (note.noteId == -1 || v->note.noteId == note.noteId); // a note with an id is that note's alone
    });
}

int Synth::activeVoiceCount() const {
    return static_cast<int>(active_.size());
}

int Synth::liveVoiceCount() const {
    return static_cast<int>(std::count_if(active_.begin(), active_.end(), [](const Voice *v) { return live(*v); }));
}

void Synth::setActive(Voice &v, bool on) {
    v.active = on;
    const auto at = std::lower_bound(active_.begin(), active_.end(), &v, std::less<>());
    const bool listed = at != active_.end() && *at == &v;
    if (on && !listed)
        active_.insert(at, &v); // within what prepare() reserved
    else if (!on && listed)
        active_.erase(at);
}

void Synth::setScope(int op, int cycles) {
    op = op >= 0 && op < kOps ? op : -1;
    cycles = std::clamp(cycles, 1, kScopeMaxCycles);
    if (op == scope_.op && cycles == scope_.cycles)
        return;
    scope_.op = op;
    scope_.cycles = cycles;
    scope_.live = false;
    scopeRestart();
}

void Synth::scopeRestart() {
    scope_.started = false;
    scope_.whole = false;
    scope_.filling.fill(0.0f);
    scope_.frame.fill(0.0f);
    scope_.blend = {};
}

void Synth::scopeTake(double phase, double value) {
    ScopeCapture &s = scope_;
    const int total = s.cycles * kScopeBinsPerCycle;
    ++s.blend.since;
    double delta = phase - s.lastPhase;
    if (delta < -0.5) // copy 0 went on into its next cycle
        delta += 1.0;
    if (!s.started || delta < 0.0 || delta >= 0.5) {
        // The first sample, or the phases were set again (a note started over): the span goes on from here.
        s.started = true;
        s.waiting = false;
        s.taken = 0;
        s.lastPos = phase;
        s.next = static_cast<int64_t>(std::ceil(phase * kScopeBinsPerCycle));
        if (s.next >= total) {
            s.next -= total;
            s.lastPos -= s.cycles;
        }
    } else {
        double pos = s.lastPos + delta;
        // A span shorter than its points (a high operator) costs more points than samples, and a copy every few
        // samples: after one, the spans pass by unfilled until as many samples as points have gone, so the scope
        // costs about two writes a sample at most, and still shows a whole span tens of times a second.
        if (s.waiting) {
            s.rest = std::max<int64_t>(s.rest - 1, 0);
            if (pos >= s.cycles) { // the next span starts: filled if the rest is over
                s.lastPos -= s.cycles;
                pos -= s.cycles;
                if (s.rest == 0) {
                    s.waiting = false;
                    s.next = 0;
                    s.taken = 0;
                }
            }
        }
        if (!s.waiting) {
            ++s.taken;
            while (static_cast<double>(s.next) <= pos * kScopeBinsPerCycle) {
                const double t = (static_cast<double>(s.next) / kScopeBinsPerCycle - s.lastPos) / delta;
                s.filling[s.next] = static_cast<float>(s.lastValue + (value - s.lastValue) * t);
                if (++s.next == total) { // a whole span, published; the next one starts at 0
                    s.blend.publish(s.frame, total, s.whole, static_cast<int64_t>(kScopeBlendSpan * rate_));
                    std::copy_n(s.filling.begin(), total, s.frame.begin());
                    s.whole = true;
                    s.next = 0;
                    s.lastPos -= s.cycles;
                    pos -= s.cycles;
                    if (s.taken < total) {
                        s.waiting = true;
                        s.rest = total - s.taken;
                        break;
                    }
                    s.taken = 0;
                }
            }
        }
        s.lastPos = pos;
        s.inc = delta;
    }
    s.lastPhase = phase;
    s.lastValue = value;
}

void Synth::telemetry(Telemetry &out) const {
    // As a new one, built in its place: assigning one would build it on the stack first (some 45 KB, on the audio
    // thread about 60 times a second while the editor is open) and copy it.
    static_assert(std::is_trivially_destructible_v<Telemetry>);
    std::construct_at(&out);
    out.seqPosition = seq_.position();
    out.seqPattern = seq_.pattern();
    out.seqPreview = seq_.previewing();
    out.seqKeys = seq_.soundingPitches();
    out.seqWheel = static_cast<float>(seqWheel_);
    for (int m = 0; m < kMacros; ++m)
        out.seqMacro[m] = static_cast<float>(seqMacroAdded_[m]);
    for (int k = 0; k < kSeqFeelCount; ++k)
        out.seqFeel[k] = static_cast<float>(seqFeel_[k]);
    if (scope_.op >= 0 && scope_.live) {
        // A slow span (or the first one, before it is whole) shows as it fills; a quicker one a whole span at a time,
        // blending into the next (see kScopeBlendSpan).
        const int total = scope_.cycles * kScopeBinsPerCycle;
        const bool slow = scope_.inc <= 0.0 || scope_.cycles / (scope_.inc * rate_) > kScopeSlowSpan;
        const bool filling = slow || !scope_.whole;
        if (filling)
            std::copy_n(scope_.filling.begin(), total, out.scope.begin());
        else
            scope_.blend.show(scope_.frame, total, out.scope);
        out.scopeBins = total;
        out.scopeCursor = filling ? static_cast<float>(scope_.next) / static_cast<float>(total) : -1.0f;
    }
    out.tempo = static_cast<float>(tempo_);
    out.fxPitch = static_cast<float>(fxPitch_);
    out.outPeak = {static_cast<float>(outPeak_[0]), static_cast<float>(outPeak_[1])};
    // The bend with no voice playing: the MIDI bend and the param with the host's modulation for every voice (the
    // newest voice's, below, while one plays).
    const int pitchBend = paramIndex(GlobalParam::PitchBend);
    out.bendSet = static_cast<float>(bendSet_);
    out.bend = static_cast<float>(std::clamp(
        bend_ + seqBend_ + std::clamp(bendSet_ + globalMod_[pitchBend], table_[pitchBend].min, table_[pitchBend].max),
        -1.0, 1.0));
    out.clipHold = static_cast<float>(clipHold_);
    if (analyzing_)
        outScope_.blend.show(outScope_.frame, kOutScopeBins, out.outScope);
    // The keys played: the host's and the editor's keyboard's, those the sequencer takes as keys among them; not the
    // sequencer's own notes, nor those the editor plays to be heard.
    for (const HeldNote &held : held_)
        if (held.note.port != kSeqPort && held.note.port != kAuditionPort)
            out.keys[held.note.key / 32] |= 1u << (held.note.key % 32);
    for (int i = 0; i < keysTakenCount_; ++i)
        out.keys[keysTaken_[i].key / 32] |= 1u << (keysTaken_[i].key % 32);
    for (int slot = 0; slot < kFxSlots; ++slot) { // effects keep working on echoes after the voices end
        for (int band = 0; band < kMcBands; ++band) {
            out.fxReduction[slot][band] = effects_->reduction(slot, band);
            out.fxBandLevel[slot][band] = effects_->bandLevel(slot, band);
            out.fxBandGain[slot][band] = effects_->bandGain(slot, band);
        }
        for (int band = 0; band < kSpectrumBands; ++band)
            out.fxSpectrum[slot][band] = effects_->spectrum(slot, band);
        out.fxPhases[slot] = effects_->sweepPhase(slot);
    }
    for (int port = 0; port < kFxPorts; ++port)
        out.fxPortLevels[port] = effects_->portLevel(port);
    for (int f = 0; f < kFilters; ++f) // and so do the filters' analyzers, as the filters ring out
        out.filterSpectrum[f] = filterAnalyzed_[f].bands;
    const Voice *newest = nullptr;
    for (const Voice &v : active()) {
        if (!live(v))
            continue;
        ++out.voices;
        if (!newest || v.order > newest->order)
            newest = &v;
    }
    for (int m = 0; m < kMacros; ++m) // the macros' knobs as modulation takes them: in the newest voice, or every voice's
        out.macroOffset[m] = static_cast<float>((newest ? newest->auxOut[kMaxSources + m] : macroKnobs_[m]) - macroSet_[m]);
    if (!newest) {
        // The free and anchored LFOs' shared clocks run on with no voice, and the editor's cards show them running.
        for (int s = 0; s < kMaxSources; ++s) {
            out.lfoPhases[s] = static_cast<float>(freeLfos_[s].phase);
            out.lfoRates[s] = static_cast<float>(freeLfos_[s].hz);
            out.lfoFrom[s] = static_cast<float>(freeLfos_[s].from);
            out.lfoTo[s] = static_cast<float>(freeLfos_[s].held);
        }
        // What the last voice's modulation adds to the effects as they ring out, where the editor's knobs show it.
        if (fxTail_.on) {
            out.fxTail = true;
            out.fxTailLfos = fxTail_.lfoMask;
            for (int s = 0; s < kMaxSources; ++s) { // the LFOs it runs, where their cards show them
                if (!(fxTail_.lfoMask & (1u << s)))
                    continue;
                const Lfo &lfo = fxTail_.lfos[s];
                out.lfoPhases[s] = static_cast<float>(lfo.phase);
                out.lfoRates[s] = static_cast<float>(lfo.hz);
                out.lfoFrom[s] = static_cast<float>(lfo.from);
                out.lfoTo[s] = static_cast<float>(lfo.held);
                out.lfoOffsets[s] = static_cast<float>(leftOffset(lfo, fxTail_.curveReleases[s]));
                if (lfo.left)
                    out.lfoLeft |= 1u << s;
            }
            for (int m = 0; m < fxTailModCount_; ++m) {
                const SlotMod &mod = fxTailMods_[m];
                if (mod.index < kModulatedCount)
                    out.modOffset[mod.index] += static_cast<float>(mod.offset);
                if (mod.slot >= 0)
                    out.slotOffset[mod.slot] =
                        static_cast<float>(mod.offset / (table_[mod.index].max - table_[mod.index].min));
            }
        }
        return;
    }
    for (int m = 0; m < newest->slotModCount; ++m) {
        const SlotMod &mod = newest->slotMods[m];
        if (mod.index < kModulatedCount) // not a macro's knob, which macroOffset has
            out.modOffset[mod.index] += static_cast<float>(mod.offset);
        if (mod.slot >= 0)
            out.slotOffset[mod.slot] =
                static_cast<float>(mod.offset / (table_[mod.index].max - table_[mod.index].min));
    }
    for (int i = 0; i < macroTargetCount_; ++i) // what the macros add, once for each target they share
        if (macroTargets_[i] < kModulatedCount && // the sequencer's have seqFeel
            std::find(macroTargets_.begin(), macroTargets_.begin() + i, macroTargets_[i]) == macroTargets_.begin() + i)
            out.modOffset[macroTargets_[i]] += static_cast<float>(macroMod_[macroTargets_[i]]);
    for (int s = 0; s < kMaxSources; ++s) {
        out.sources[s] = static_cast<float>(newest->sourceValues[s]);
        out.lfoPhases[s] = static_cast<float>(newest->lfos[s].phase);
        out.lfoRates[s] = static_cast<float>(newest->lfos[s].hz);
        out.lfoFrom[s] = static_cast<float>(newest->lfos[s].from);
        out.lfoTo[s] = static_cast<float>(newest->lfos[s].held);
        out.lfoOffsets[s] = static_cast<float>(leftOffset(newest->lfos[s], newest->curveReleases[s]));
        if (newest->lfos[s].left)
            out.lfoLeft |= 1u << s;
        out.envPos[s] = static_cast<float>(newest->modEnvs[s].position(newest->modEnvRates[s].last()));
    }
    for (int i = 0; i < kOps; ++i) {
        out.opLevels[i] = static_cast<float>(newest->ops[i].env.level());
        out.opEnvLevels[i] = static_cast<float>(newest->ops[i].env.level());
        out.opEnvPos[i] = static_cast<float>(newest->ops[i].env.position(newest->ops[i].rates.last()));
        out.opEnvPhase[i] = newest->ops[i].env.following() ? static_cast<float>(newest->drawn[i].phase()) : -1.0f;
    }
    out.notePitch = static_cast<float>(newest->pitch);
    out.noteVelocity = static_cast<float>(newest->velocity);
    out.bend = static_cast<float>(2.0 * newest->auxOut[kBendSource] - 1.0); // the newest voice's (see renderBlock())
}

void Synth::setVoiceLimit(int limit) {
    const int before = limit_;
    limit_ = std::clamp(limit, 1, std::max(capacity(), 1));
    if (limit_ >= before)
        return;
    for (int playing = liveVoiceCount(); playing > limit_; --playing)
        startFade(*stealable());
}

Synth::Voice *Synth::stealable() {
    // A voice that hasn't played yet (one started by a note at the same moment, say) has no level to go by: it
    // counts as loud, or each note of a chord would steal the one before it. One the pedal holds, its key up, goes
    // after the released ones and before those whose keys are down.
    auto rank = [](const Voice &v) {
        return std::tuple(v.released ? 0 : v.sustained ? 1 : 2,
                          v.fresh ? std::numeric_limits<double>::infinity() : v.peak, v.order);
    };
    Voice *victim = nullptr;
    for (Voice &v : active())
        if (live(v) && (!victim || rank(v) < rank(*victim)))
            victim = &v;
    return victim;
}

Synth::Voice &Synth::allocateVoice() {
    // At the limit: steal one (see stealable()). It fades out in a place of its own.
    if (liveVoiceCount() >= limit_)
        if (Voice *victim = stealable())
            startFade(*victim);
    // The first free place, else the fading voice closest to silence. There are more places than the limit, so one
    // of the two is always there. The active voices are listed in their places' order: the first place they skip
    // is free, found without reading the voices that rest.
    size_t listed = 0;
    for (Voice &v : voices_) {
        if (listed < active_.size() && active_[listed] == &v)
            ++listed;
        else
            return v;
    }
    Voice *quietest = nullptr;
    for (Voice &v : active())
        if (v.fading && (!quietest || v.fade < quietest->fade))
            quietest = &v;
    return quietest ? *quietest : voices_.front();
}

void Synth::startFade(Voice &v) {
    if (!live(v))
        return;
    endVoice(v); // the note is over, though the sound takes a moment to go
    setActive(v, true);
    v.fading = true; // from where it is: one still fading in (see noteOn()) fades out sooner
}

void Synth::endVoice(Voice &v) {
    if (!v.active)
        return;
    setActive(v, false);
    if (v.fading) { // its note ended when it started fading
        v.fading = false;
        return;
    }
    // A stolen or choked note is over even if its key is still down. Once its key is up, a key held with the same
    // note (struck again without note ids while this one rang out) is another note's, and stays down.
    if (!v.released && !v.sustained) {
        const auto own = std::find_if(held_.begin(), held_.end(), [&](const HeldNote &h) { return h.note == v.note; });
        if (own != held_.end())
            held_.erase(own);
    }
    reportEnded(v.note);
}

// With MPE off, every note asks with a channel and with MPE on none does: MTS-ESP's client keeps whether the last note
// it filtered had a channel and picks the table it retunes the next by that, so a note asked with none between two on
// channel 2 would put a held one there on the master's table for every channel. The sequencer's and the editor's own
// notes, and a host's note on no channel, ask on the first channel, the one their NoteRef is on.
int Synth::tuningChannel(const NoteRef &note) const {
    if (mpe_)
        return -1;
    return note.channel >= 0 && note.channel <= 15 ? note.channel : 0;
}

// The tuning set where it overrides the patch's (an MTS-ESP master) or the patch has none; else the patch's own.
double Synth::retuneOf(const NoteRef &note) const {
    if (note.key < 0 || note.key > 127)
        return 0.0;
    double retune = 0.0;
    if (tuning_ && (!patchTuning_.on || tuning_->overrides()))
        retune = tuning_->retune(note.key, tuningChannel(note));
    else if (patchTuning_.on)
        retune = patchTuning_.retune[note.key];
    // Held to the keys' range and more, whatever a master sends: a pitch that isn't a number would end in silence.
    return std::isfinite(retune) ? std::clamp(retune, -128.0, 128.0) : 0.0;
}

bool Synth::unmapped(const NoteRef &note) const {
    if (note.key < 0 || note.key > 127)
        return false;
    if (tuning_ && (!patchTuning_.on || tuning_->overrides()))
        return tuning_->unmapped(note.key, tuningChannel(note));
    return patchTuning_.on && patchTuning_.unmapped[note.key];
}

void Synth::reportEnded(const NoteRef &note) {
    if (note.port == kSeqPort || note.port == kAuditionPort) // the sequencer's own, or the editor's: no host waits
        return;
    if (std::find(ended_.begin(), ended_.end(), note) == ended_.end() && ended_.size() < ended_.capacity())
        ended_.push_back(note); // capacity was reserved in prepare(), so this never allocates
}

void Synth::setVoiceMod(Voice &v, int index, double amount) {
    v.mod[index] = amount;
    if (!v.modSet[index]) {
        v.modSet[index] = true;
        v.modded[v.moddedCount++] = static_cast<uint16_t>(index);
    }
}

void Synth::clearVoiceMods(Voice &v) {
    for (int k = 0; k < v.moddedCount; ++k) {
        v.mod[v.modded[k]] = 0.0;
        v.modSet[v.modded[k]] = false;
    }
    v.moddedCount = 0;
}

void Synth::shareSlots(const ParamValues &params) {
    auto get = [&params](int i) { return params[i]; };
    opSourceMask_ = 0;
    fxFmTargets_ = 0;
    for (int source = 0; source < kAllSources; ++source) {
        int count = 0;
        for (int s = 0; s < kSlots; ++s) {
            const int target = slotTarget(params[slotTargetIndex(source, s)]);
            if (target < 0 || params[slotBypassIndex(source, s)] >= 0.5)
                continue;
            if (isOpSource(source))
                opSourceMask_ |= 1u << (source - kOpSource);
            if (const int aux = slotAux(get, source, s); isOpSource(aux))
                opSourceMask_ |= 1u << (aux - kOpSource);
            SharedSlot &slot = sharedSlots_[source][count++];
            slot.slot = s;
            slot.target = target;
            slot.curve = hostValue(params, slotCurveIndex(source, s));
            slot.scale = hostValue(params, slotScaleIndex(source, s));
            slot.voiceAux = -1;
            slot.auxDepth = hostValue(params, slotAuxDepthIndex(source, s));
            if (const int aux = slotAux(get, source, s); aux >= 0) {
                if (runsPerVoice(aux) || voiceMovedMacro(aux)) // a macro the voice's sources move, as the voice has it
                    slot.voiceAux = aux;
                else
                    slot.scale *= auxScale(slot.auxDepth, macroKnobs_[knobOf(aux)]);
            }
            slot.range = table_[target].max - table_[target].min;
            slot.depth = hostValue(params, slotAmountIndex(source, s));
            slot.fmFilter = slot.fmFx = -1;
            if (isOpSource(source)) {
                for (int f = 0; f < kFilters; ++f)
                    if (target == paramIndex(f, FilterParam::Cutoff))
                        slot.fmFilter = f;
                if (fxSlotOf(target) >= 0 && fxFieldOf(target) == FxParam::FilterCutoff) {
                    slot.fmFx = fxSlotOf(target);
                    fxFmTargets_ |= 1u << slot.fmFx;
                }
            }
        }
        sharedSlotCount_[source] = count;
    }
    sharedSlotsReady_ = true;
}

// Whether a slot an operator aims at holds a filter effect that follows filter FM (see runOpSources()): read every
// control block, since an effect's type or filter type changing leaves the shared slots as they are.
bool Synth::fxFmWantedNow(const ParamValues &params) const {
    for (uint32_t slots = fxFmTargets_; slots != 0; slots &= slots - 1) {
        const int slot = std::countr_zero(slots);
        if (fxType(params[paramIndex(slot, FxParam::Type)]) == FxType::Filter &&
            followsCutoffFm(params[paramIndex(slot, FxParam::FilterType)]))
            return true;
    }
    return false;
}

// Kept out of renderBlock(), which patches without an operator as a source run as they did.
FMVIBE_NOINLINE void Synth::runOpSources(Voice &v, const ParamValues &params) {
    voiceFmCount_ = 0;
    audioModCount_ = 0;
    audioLaneCount_ = 0;
    audioFilterMask_ = 0; // startAudioMods() sets it, and runs only for a voice with lanes: not the last voice's
    for (int op = 0; op < kOps; ++op) {
        if (!(opSourceMask_ >> op & 1u))
            continue;
        const int source = kOpSource + op;
        v.auxOut[source] = v.opLevel[op];
        if (params[paramIndex(op, OpParam::Enabled)] < 0.5)
            continue; // switched off, it stands still and modulates nothing
        const bool bipolar = params[paramIndex(op, OpSourceParam::Bipolar)] >= 0.5;
        const double swing = bipolar ? 0.5 * v.opMean[op] : v.opLevel[op];
        for (int k = 0; k < sharedSlotCount_[source]; ++k) {
            const SharedSlot &slot = sharedSlots_[source][k];
            if (slot.depth == 0.0)
                continue;
            const double scale =
                slot.voiceAux >= 0 ? slot.scale * auxScale(slot.auxDepth, v.auxOut[slot.voiceAux]) : slot.scale;
            // On a filter's Cutoff, it moves the filter sample by sample instead (see moveCutoffs(), runFxCutoffFm()).
            const CutoffFm fm{slot.fmFilter >= 0 ? slot.fmFilter : slot.fmFx, op,
                              slot.depth * scale * slot.range * kCutoffOctaves, slot.curve, bipolar};
            if (slot.fmFilter >= 0 && followsCutoffFm(params[paramIndex(slot.fmFilter, FilterParam::Type)])) {
                voiceFm_[voiceFmCount_++] = fm;
                continue;
            }
            if (slot.fmFx >= 0 && fxType(params[paramIndex(slot.fmFx, FxParam::Type)]) == FxType::Filter &&
                followsCutoffFm(params[paramIndex(slot.fmFx, FxParam::FilterType)])) {
                if (&v == fxFmVoice_) {
                    fxFm_[fxFmCount_++] = fm;
                    fxFmOps_ |= 1u << op;
                }
                continue;
            }
            const double offset = slotCurveValue(swing, slot.curve, bipolar) * slot.depth * scale * slot.range;
            // On a param the voice plays by, every sample (see AudioKind); the editor shows it as of the last block.
            if (slot.target < kVoiceParamCount && audioTargets_[slot.target].kind != AudioKind::None) {
                audioMods_[audioModCount_++] = {op, audioLane(slot.target), slot.depth * scale * slot.range, slot.curve,
                                                bipolar};
                v.slotMods[v.slotModCount++] = {slot.target, offset, source * kSlots + slot.slot, true};
                continue;
            }
            v.slotMods[v.slotModCount++] = {slot.target, offset, source * kSlots + slot.slot};
        }
    }
}

namespace {

// What an operator that moves something every sample swings it by, as it plays: bipolar, half its output either way;
// unipolar, from nothing up to as far as its envelope has risen (see runOpSources()). Through its slot's curve.
double opSwing(double out, double env, bool bipolar, double curve) {
    const double swing = bipolar ? 0.5 * std::clamp(out, -1.0, 1.0) : std::clamp(0.5 * (out + env), 0.0, 1.0);
    return curve != 0.0 ? slotCurveValue(swing, curve, bipolar) : swing;
}

} // namespace

double Synth::cutoffFmOctaves(const CutoffFm &fm, double out, double env) {
    return opSwing(out, env, fm.bipolar, fm.curve) * fm.octaves;
}

FMVIBE_NOINLINE bool Synth::startCutoffFm(std::array<const FilterSettings *, kFilters> &filter) {
    voiceFmBase_.fill(nullptr);
    bool any = false;
    for (int k = 0; k < voiceFmCount_; ++k) {
        const int f = voiceFm_[k].place;
        if (!filter[f] || voiceFmBase_[f])
            continue;
        // From its settings as they stand: those audioSample() works out every sample, for a filter it moves.
        voiceFmBase_[f] = audioFilterMask_ >> f & 1u ? &audioRun_.filterNow[f] : filter[f];
        voiceFmNow_[f] = *filter[f];
        filter[f] = &voiceFmNow_[f];
        any = true;
    }
    return any;
}

FMVIBE_NOINLINE void Synth::moveCutoffs(const Voice &v, uint32_t sample) {
    std::array<double, kFilters> octaves{};
    for (int k = 0; k < voiceFmCount_; ++k) {
        const CutoffFm &fm = voiceFm_[k];
        const Operator &op = v.ops[fm.op];
        octaves[fm.place] += cutoffFmOctaves(fm, op.out, op.env.level());
    }
    for (int f = 0; f < kFilters; ++f) {
        const FilterSettings *base = voiceFmBase_[f];
        // The diode's stages take a system solved anew: every other sample is enough at the voices' oversampling.
        if (base && (base->type != FilterType::Diode || (sample & 1u) == 0)) {
            if (audioFilterMask_ >> f & 1u) // its settings worked out anew this sample, all of them
                voiceFmNow_[f] = *base;
            modulateCutoff(voiceFmNow_[f], *base, std::exp2(octaves[f]), voiceFmRange_);
        }
    }
}

void Synth::runFxCutoffFm(uint32_t frames) {
    const uint64_t voice = fxFmVoice_ ? fxFmVoice_->order + 1 : 0;
    const bool changed = voice != fxFmOrder_;
    fxFmOrder_ = voice;
    fxFmLive_ = false;
    for (int slot = 0; slot < kFxSlots; ++slot) {
        FxCutoffFm &out = fxCutoffFm_[slot];
        bool listed = false;
        for (int k = 0; k < fxFmCount_ && !listed; ++k)
            listed = fxFm_[k].place == slot;
        if (!listed && !out.on)
            continue;
        std::array<double, kFxFmBlock> octaves{};
        for (int k = 0; k < fxFmCount_; ++k) {
            const CutoffFm &fm = fxFm_[k];
            if (fm.place != slot)
                continue;
            for (uint32_t s = 0; s < frames; ++s)
                octaves[s] += cutoffFmOctaves(fm, fxOpOut_[fm.op][s] / oversampling_, fxOpEnv_[fm.op]);
        }
        // Another voice, or the modulation coming or going: from where the cutoff was, over a few milliseconds.
        if (changed || listed != out.listed || !out.on)
            out.jump = out.last - octaves[0];
        out.listed = listed;
        float most = 0.0f;
        for (uint32_t s = 0; s < frames; ++s) {
            out.last = octaves[s] + out.jump;
            out.jump *= fxFmDecay_;
            // As late as the voices' sound coming down to the host's rate.
            out.line[out.linePos] = static_cast<float>(out.last);
            const int read = out.linePos - fxFmDelay_;
            out.octaves[s] = out.line[read < 0 ? read + kFxFmDelay : read];
            out.linePos = out.linePos + 1 == kFxFmDelay ? 0 : out.linePos + 1;
            most = std::max(most, std::abs(out.octaves[s]));
        }
        out.on = listed || most > 1e-5f || std::abs(out.jump) > 1e-5;
        fxFmLive_ = fxFmLive_ || out.on;
        effects_->setCutoffFm(slot, out.on ? out.octaves.data() : nullptr);
    }
}

FMVIBE_NOINLINE void Synth::finishOpSources(Voice &v, const std::array<bool, kOps> &opOn, uint32_t frames) {
    for (int op = 0; op < kOps; ++op) {
        if (&v == fxFmVoice_)
            fxOpEnv_[op] = opOn[op] ? v.ops[op].env.level() : 0.0;
        // Unison's copies can sum past full scale; a source puts out -1..1.
        const double mean = opOn[op] && frames > 0 ? std::clamp(v.opSum[op] / frames, -1.0, 1.0) : 0.0;
        v.opSum[op] = 0.0;
        v.opMean[op] = mean;
        // From nothing to the envelope's level as the wave swings, so that silence is 0, as an LFO's lowest point is.
        v.opLevel[op] = opOn[op] ? std::clamp(0.5 * (mean + v.ops[op].env.level()), 0.0, 1.0) : 0.0;
    }
}

namespace {

// An operator's Pan or the voice's as its left and right gains, equal-power and unity at the center.
double panLeft(double pan) {
    return std::numbers::sqrt2 * std::cos((std::clamp(pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0);
}
double panRight(double pan) {
    return std::numbers::sqrt2 * std::sin((std::clamp(pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0);
}

} // namespace

FMVIBE_NOINLINE bool Synth::startAudioMods(Voice &v, const double *p, double pitch, const Levels &target,
                                           const AudioBlock &b) {
    AudioRun &run = audioRun_;
    run.p = p;
    run.pitch = pitch;
    run.gain = kVoiceLevel * v.exprGain;
    run.exprPan = v.exprPan;
    run.rates = b.rates.data();
    run.noiseCoefs = b.noiseCoefs.data();
    run.noiseMid = b.noiseMid.data();
    run.noiseSide = b.noiseSide.data();
    run.copyGains = b.copyGains.data();
    run.copies = b.copies.data();
    run.opOn = b.opOn.data();
    run.noise = b.noise.data();
    run.filter = nullptr;
    run.pitchFilters = run.fmFilters = 0;
    run.savedCount = 0;
    audioReach_ = {};
    audioFmReach_ = 0.0;
    const std::span<double> reach = values(audioReach_);
    for (int i = 0; i < kOps; ++i) {
        run.fixed[i] = p[paramIndex(i, OpParam::Fixed)] >= 0.5;
        run.modes[i] = unisonModesOf(p, i, v.order);
        run.monoGain[i] = b.opOn[i] ? b.monoGain[i] : 1.0;
    }
    // Each lane's value before the operators: as set with the voice's host and other slots' modulation, unclamped.
    for (int l = 0; l < audioLaneCount_; ++l) {
        AudioLane &lane = audioLanes_[l];
        const int t = lane.target;
        lane.base = voiceBase_[t] + v.mod[t];
        for (int m = 0; m < v.slotModCount; ++m)
            if (v.slotMods[m].index == t && !v.slotMods[m].shown)
                lane.base += v.slotMods[m].offset;
        lane.set = p[t];
        lane.lo = paramMin_[t];
        lane.hi = paramMax_[t];
        lane.down = lane.up = 0.0;
    }
    for (int m = 0; m < audioModCount_; ++m) {
        const AudioMod &mod = audioMods_[m];
        AudioLane &lane = audioLanes_[mod.lane];
        const double low = mod.scale * (mod.bipolar ? -0.5 : 0.0), high = mod.scale * (mod.bipolar ? 0.5 : 1.0);
        lane.down += std::min({0.0, low, high});
        lane.up += std::max({0.0, low, high});
    }
    auto routeFrom = [&](int node) {
        return node < kOps            ? b.opOn[node]
               : node < kOps + kFilters ? b.filterEnabled[node - kOps]
                                        : b.shaperEnabled[node - kOps - kFilters];
    };
    auto pitchedOp = [&](int i) { return b.opOn[i] && !b.noise[i]; };
    std::array<double, kOps> pitchReach;
    pitchReach.fill(1.0);
    std::array<bool, kOps> panned{}, spread{};
    bool any = false;
    for (int l = 0; l < audioLaneCount_; ++l) {
        AudioLane &lane = audioLanes_[l];
        const AudioTarget &at = lane.at;
        const double low = std::clamp(lane.base + lane.down, lane.lo, lane.hi);
        const double high = std::clamp(lane.base + lane.up, lane.lo, lane.hi);
        const int a = at.a;
        bool live = true;
        switch (at.kind) {
        case AudioKind::Level: {
            const bool tail = v.tail;
            switch (at.gate) {
            case AudioGate::None:
                break;
            case AudioGate::Op:
                live = b.opOn[a];
                break;
            case AudioGate::OpFilter:
                live = b.opOn[a] && b.filterEnabled[at.b];
                break;
            case AudioGate::OpShaper:
                live = b.opOn[a] && b.shaperEnabled[at.b];
                break;
            case AudioGate::Shaper:
                live = b.shaperEnabled[a];
                break;
            case AudioGate::FilterFilter:
                live = b.filterEnabled[a] && b.filterEnabled[at.b] && (at.b > a || !tail);
                break;
            case AudioGate::FilterShaper:
                live = b.filterEnabled[a] && b.shaperEnabled[at.b] && !tail;
                break;
            case AudioGate::ShaperShaper:
                live = b.shaperEnabled[a] && b.shaperEnabled[at.b] && !tail;
                break;
            case AudioGate::ShaperFilter:
                live = b.shaperEnabled[a] && b.filterEnabled[at.b];
                break;
            }
            if (live)
                reach[at.field] = std::max(std::abs(low), std::abs(high));
            break;
        }
        case AudioKind::Route: {
            live = routeFrom(a) && b.opOn[at.b];
            const RouteMode mode = routeModes_[a][at.b];
            const double index = a == at.b ? kMaxFeedback : kMaxIndex;
            lane.from = routeLevel(mode, lane.set, index);
            if (live)
                reach[at.field] =
                    std::max(std::abs(routeLevel(mode, low, index)), std::abs(routeLevel(mode, high, index)));
            break;
        }
        case AudioKind::OpPan:
            live = b.opOn[a];
            lane.from = panLeft(lane.set);
            lane.fromR = panRight(lane.set);
            panned[a] = live;
            break;
        case AudioKind::OpPhase:
            live = pitchedOp(a);
            if (live)
                b.warped[a] = true; // the loop that reads its wave where its Phase has moved it
            break;
        case AudioKind::OpRatio:
            live = pitchedOp(a) && !run.fixed[a];
            lane.from = ratioFromNorm(std::clamp(lane.set, 0.0, kRatioModNorm));
            if (live)
                pitchReach[a] *= ratioFromNorm(std::clamp(high, 0.0, kRatioModNorm)) / lane.from;
            break;
        case AudioKind::OpTranspose:
            live = pitchedOp(a) && !run.fixed[a];
            if (live)
                pitchReach[a] *= std::exp2((high - lane.set) / 12.0);
            break;
        case AudioKind::OpDetune:
            live = pitchedOp(a) && !run.fixed[a];
            if (live)
                pitchReach[a] *= std::exp2((high - lane.set) / 1200.0);
            break;
        case AudioKind::OpFixedFreq:
            live = pitchedOp(a) && run.fixed[a];
            lane.from = fixedFreqFromNorm(lane.set);
            if (live)
                pitchReach[a] *= fixedFreqFromNorm(high) / lane.from;
            break;
        case AudioKind::GlobalPitch:
            for (int i = 0; i < kOps; ++i)
                if (pitchedOp(i) && !run.fixed[i])
                    pitchReach[i] *= std::exp2(std::max(high - lane.set, 0.0) / 12.0);
            for (int f = 0; f < kFilters; ++f) // a key-tracked filter follows it, every sample
                if (b.filterEnabled[f] && p[paramIndex(f, FilterParam::KeyTrack)] != 0.0) {
                    run.pitchFilters |= 1u << f;
                    audioFilterMask_ |= 1u << f;
                }
            break;
        case AudioKind::UnisonDetune:
            live = pitchedOp(a) && b.copies[a] > 1;
            if (live) {
                run.shares[a] = unisonDetunes(a, b.copies[a], run.modes[a]);
                pitchReach[a] *= std::exp2(std::max(high - lane.set, 0.0) / 1200.0);
            }
            break;
        case AudioKind::UnisonSpread:
        case AudioKind::UnisonBlend:
            live = b.opOn[a] && (at.kind == AudioKind::UnisonSpread ? b.copies[a] > 1 || b.noise[a]
                                                                      : !b.noise[a] && b.copies[a] > 1);
            if (live && !b.noise[a]) {
                // Each copy's place, which the samples spread, and its weight, as the block has them.
                const UnisonSpreadOut out = unisonSpread(b.copies[a], run.modes[a].curve, run.modes[a].pan,
                                                         run.modes[a].blend, run.modes[a].seed, a);
                run.copyPan[a] = out.pan;
                run.copyWeight[a] = out.weight;
            }
            if (live && at.kind == AudioKind::UnisonBlend)
                b.warped[a] = true; // the loop that weighs its copies
            spread[a] = spread[a] || live;
            break;
        case AudioKind::WarpAmount: {
            const Operator &op = v.ops[a];
            live = pitchedOp(a) && op.warp != OpWarp::Off;
            if (live) {
                const double was = std::max(std::abs(op.warpNow), std::abs(lane.set));
                const double most = std::min(std::max({was, std::abs(low), std::abs(high)}), 1.0);
                pitchReach[a] *= std::max(warpSqueeze(op.warp, most) / warpSqueeze(op.warp, was), 1.0);
            }
            break;
        }
        case AudioKind::OpEnv:
            live = b.opOn[a];
            if (live) // a switched-off one's rates aren't worked out
                run.baseRates[a] = b.rates[a];
            break;
        case AudioKind::NoiseSlope:
            live = b.opOn[a] && b.noise[a];
            if (live)
                b.colored[a] = true;
            break;
        case AudioKind::Filter:
            live = b.filterEnabled[a];
            if (live)
                audioFilterMask_ |= 1u << a;
            break;
        case AudioKind::Shaper:
            live = b.shaperEnabled[a];
            break;
        case AudioKind::FmAmount:
            audioFmReach_ = std::max(std::abs(low), std::abs(high));
            break;
        case AudioKind::GlobalPan:
            lane.from = run.gain * panLeft(lane.set + run.exprPan);
            lane.fromR = run.gain * panRight(lane.set + run.exprPan);
            break;
        case AudioKind::None:
            live = false;
            break;
        }
        lane.live = live;
        any = any || live;
    }
    // The tables suit the highest the operators take each one's pitch.
    for (int i = 0; i < kOps; ++i)
        if (pitchReach[i] > 1.0) {
            b.highestInc[i] = std::min(b.highestInc[i] * pitchReach[i], 0.5);
            b.wave[i] = b.shapes[i] == OpWave::Custom ? customWaves_[i].table(b.highestInc[i])
                                                       : waves_.table(b.shapes[i], b.highestInc[i]);
        }
    // The levels the routes are checked against, as far as the operators take each.
    audioTo_ = target;
    {
        const std::span<double> to = values(audioTo_);
        for (size_t k = 0; k < to.size(); ++k)
            to[k] = std::max(to[k], reach[k]);
    }
    // An operator panned or spread, or with its Pan or spread moved, is stereo while it is heard, or the operators may
    // make it heard.
    for (int i = 0; i < kOps; ++i) {
        if (!b.opOn[i])
            continue;
        const bool wide = panned[i] || spread[i] || p[paramIndex(i, OpParam::Pan)] != 0.0 ||
                          v.levels.panL[i] != v.levels.panR[i] ||
                          ((b.copies[i] > 1 || b.noise[i]) && p[paramIndex(i, OpParam::UnisonSpread)] > 0.0);
        if (!wide)
            continue;
        double heard = std::max(target.out[i], audioReach_.out[i]);
        for (int f = 0; f < kFilters; ++f)
            heard = std::max({heard, target.send[i][f], audioReach_.send[i][f]});
        for (int sh = 0; sh < kShapers; ++sh)
            heard = std::max({heard, target.shaperSend[i][sh], audioReach_.shaperSend[i][sh]});
        b.stereo = b.stereo || heard > 0.0;
    }
    return any;
}

FMVIBE_NOINLINE void Synth::audioSample(Voice &v, uint32_t sample) {
    AudioRun &run = audioRun_;
    const double *const p = run.p;
    audioRestore(); // the last sample's, with what the ramps have moved them by since
    auto save = [&run](double &at, double to) {
        run.saved[run.savedCount++] = {&at, at, to};
        at = to;
    };
    for (int l = 0; l < audioLaneCount_; ++l)
        audioLanes_[l].acc = 0.0;
    // What each operator puts out as it stands, before this sample: as a source does (see runOpSources()).
    for (int m = 0; m < audioModCount_; ++m) {
        const AudioMod &mod = audioMods_[m];
        const Operator &o = v.ops[mod.op];
        audioLanes_[mod.lane].acc += opSwing(o.out, o.env.level(), mod.bipolar, mod.curve) * mod.scale;
    }
    std::array<double, kOps> factor;
    factor.fill(1.0);
    std::array<double, kOps> detune{}, spreadTo{}, blendTo{};
    std::array<std::array<double, 8>, kOps> env;
    std::array<FilterInputs, kFilters> filterIn;
    std::array<std::array<double, 3>, kShapers> shaperIn;
    double global = 1.0;     // the voice's Pitch, as a factor
    double pitchShift = 0.0; // and in semitones, which key tracking follows
    uint32_t pitched = 0, detuned = 0, spreadMask = 0, blendMask = 0, envMask = 0, envTimes = 0, filterMask = 0,
             shaperMask = 0;
    // A filter's inputs where renderBlock()'s ramp has them at this sample (see kFilterInputSmooth), so what the
    // control blocks move still glides; the operators move them from there.
    auto seedFilter = [&](int f) {
        if (filterMask >> f & 1u)
            return;
        const FilterInputs &from = run.filterFrom[f], &to = run.filterTo[f];
        const double along = run.filterShare * std::min(1.0, (sample + 1.0) / run.frames);
        filterIn[f] = to;
        for (size_t i = 0; i < to.size(); ++i)
            if (kFilterInputSmooth[i] && along < 1.0)
                filterIn[f][i] = from[i] + (to[i] - from[i]) * along;
        if (to[FilterIn::Pitch] == 0.0) // left out without key tracking, which an operator may bring in
            filterIn[f][FilterIn::Pitch] = run.pitch;
        filterMask |= 1u << f;
    };
    const std::span<double> level = values(v.levels);
    for (int l = 0; l < audioLaneCount_; ++l) {
        AudioLane &lane = audioLanes_[l];
        if (!lane.live)
            continue;
        const double value = std::clamp(lane.base + lane.acc, lane.lo, lane.hi);
        const int a = lane.at.a;
        const uint32_t bit = 1u << std::max(a, 0);
        switch (lane.at.kind) {
        case AudioKind::Level:
        case AudioKind::OpPhase:
            save(level[lane.at.field], level[lane.at.field] + (value - lane.set));
            break;
        case AudioKind::Route: {
            const double to = routeLevel(routeModes_[a][lane.at.b], value, a == lane.at.b ? kMaxFeedback : kMaxIndex);
            save(level[lane.at.field], level[lane.at.field] + (to - lane.from));
            break;
        }
        case AudioKind::OpPan:
            save(v.levels.panL[a], v.levels.panL[a] + (panLeft(value) - lane.from));
            save(v.levels.panR[a], v.levels.panR[a] + (panRight(value) - lane.fromR));
            break;
        case AudioKind::OpRatio:
            factor[a] *= ratioFromNorm(std::clamp(value, 0.0, kRatioModNorm)) / lane.from;
            pitched |= bit;
            break;
        case AudioKind::OpTranspose:
            factor[a] *= std::exp2((value - lane.set) / 12.0);
            pitched |= bit;
            break;
        case AudioKind::OpDetune:
            factor[a] *= std::exp2((value - lane.set) / 1200.0);
            pitched |= bit;
            break;
        case AudioKind::OpFixedFreq:
            factor[a] *= fixedFreqFromNorm(value) / lane.from;
            pitched |= bit;
            break;
        case AudioKind::GlobalPitch:
            global *= std::exp2((value - lane.set) / 12.0);
            pitchShift += value - lane.set;
            break;
        case AudioKind::UnisonDetune:
            detune[a] = value - lane.set;
            detuned |= bit;
            break;
        case AudioKind::UnisonSpread:
            spreadTo[a] = value;
            spreadMask |= bit;
            break;
        case AudioKind::UnisonBlend:
            blendTo[a] = value;
            blendMask |= bit;
            break;
        case AudioKind::WarpAmount: {
            Operator &op = v.ops[a];
            save(op.warpNow, std::clamp(op.warpNow + (value - lane.set), -1.0, 1.0));
            break;
        }
        case AudioKind::OpEnv:
            if (!(envMask & bit)) {
                env[a] = opEnvFields(p, a);
                envMask |= bit;
            }
            env[a][lane.at.b] = value;
            if (lane.at.b != 3)
                envTimes |= bit;
            break;
        case AudioKind::NoiseSlope:
            run.noiseCoefs[a] = noiseColor_.coefs(value);
            break;
        case AudioKind::Filter:
            seedFilter(a);
            filterIn[a][lane.at.b] += value - lane.set;
            break;
        case AudioKind::Shaper:
            if (!(shaperMask & bit)) {
                const ShaperParams fields = shaperParams(kFxSlots + a);
                shaperIn[a] = {p[fields.drive], p[fields.output], p[fields.mix]};
                shaperMask |= bit;
            }
            shaperIn[a][lane.at.b] = value;
            break;
        case AudioKind::FmAmount:
            save(v.fmScale, v.fmScale + (value - lane.set));
            break;
        case AudioKind::GlobalPan:
            save(v.gainL, v.gainL + (run.gain * panLeft(value + run.exprPan) - lane.from));
            save(v.gainR, v.gainR + (run.gain * panRight(value + run.exprPan) - lane.fromR));
            break;
        case AudioKind::None: // never live (see startAudioMods()), listed for the switch's sake
            break;
        }
    }
    // The copies' increments, and the masters' of the synced, which follow the note and their copies' detune.
    if (pitched != 0 || detuned != 0 || global != 1.0)
        for (int i = 0; i < kOps; ++i) {
            if (!run.opOn[i] || run.noise[i])
                continue;
            const bool spreads = detuned >> i & 1u;
            const double by = factor[i] * (run.fixed[i] ? 1.0 : global);
            Operator &op = v.ops[i];
            if (by == 1.0 && !spreads && !(op.syncOn && global != 1.0))
                continue;
            for (int k = 0; k < run.copies[i]; ++k) {
                const double copy = spreads ? std::exp2(run.shares[i][k] * detune[i] / 1200.0) : 1.0;
                save(op.incNow[k], std::min(op.incNow[k] * by * copy, 0.5));
                if (op.syncOn)
                    save(op.syncIncNow[k], std::min(op.syncIncNow[k] * global * copy, 0.5));
            }
        }
    // The copies' places and weights.
    for (uint32_t ops = spreadMask | blendMask; ops != 0; ops &= ops - 1) {
        const int i = std::countr_zero(ops);
        const double width = spreadMask >> i & 1u ? spreadTo[i] : p[paramIndex(i, OpParam::UnisonSpread)];
        if (run.noise[i]) {
            run.noiseMid[i] = 1.0 / std::sqrt(1.0 + width * width);
            run.noiseSide[i] = width * run.noiseMid[i];
            continue;
        }
        const std::array<double, kMaxUnison> *pan = &run.copyPan[i], *weight = &run.copyWeight[i];
        UnisonSpreadOut out;
        if (blendMask >> i & 1u) {
            out = unisonSpread(run.copies[i], run.modes[i].curve, run.modes[i].pan, std::clamp(blendTo[i], 0.0, 1.0),
                               run.modes[i].seed, i);
            pan = &out.pan;
            weight = &out.weight;
            for (int k = 0; k < run.copies[i]; ++k)
                save(v.copyWeights[i][k], out.weight[k]);
        }
        run.gains[i] = copyGains(run.copies[i], *pan, *weight, width, run.monoGain[i]);
        run.copyGains[i] = &run.gains[i];
    }
    for (uint32_t ops = envMask; ops != 0; ops &= ops - 1) {
        const int i = std::countr_zero(ops);
        const std::array<double, 8> &t = env[i];
        if (envTimes >> i & 1u) {
            run.rates[i] = envRatesOf(t, rate_);
        } else {
            run.rates[i] = run.baseRates[i];
            run.rates[i].sustain = t[3];
        }
    }
    // The filters' settings: the diode's every other sample, as filter FM's are. A key-tracked filter follows the
    // voice's Pitch as the operators move it. Filter FM moves the cutoff from these, after the operators (see
    // moveCutoffs()), as it does a filter's settings that hold still.
    for (uint32_t fs = run.pitchFilters; fs != 0; fs &= fs - 1)
        seedFilter(std::countr_zero(fs));
    for (uint32_t fs = filterMask; fs != 0; fs &= fs - 1) {
        const int f = std::countr_zero(fs);
        if (!run.filter || !run.filter[f])
            continue; // not running
        FilterInputs &in = filterIn[f];
        const FilterType type = filterTypeOf(in[FilterIn::Type]);
        if (type == FilterType::Diode && (sample & 1u) != 0)
            continue;
        in[FilterIn::Pitch] += pitchShift;
        if (v.tail && selfOscillates(type)) // held below self-oscillation, as renderBlock() holds it
            in[FilterIn::Resonance] = std::min(in[FilterIn::Resonance], kTailResonance);
        run.filterNow[f] = filterSettingsFrom(in, rate_);
        if (!(run.fmFilters >> f & 1u))
            run.filter[f] = &run.filterNow[f];
    }
    for (uint32_t shs = shaperMask; shs != 0; shs &= shs - 1) {
        const int sh = std::countr_zero(shs);
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        const std::array<double, 3> &in = shaperIn[sh];
        const ShaperSettings now = shaperSettings(
            [&](int i) {
                return i == fields.drive ? in[0] : i == fields.output ? in[1] : i == fields.mix ? in[2] : p[i];
            },
            fields);
        const ShaperSettings &set = v.shaperSettings[sh].last();
        save(v.levels.shaperDrive[sh], v.levels.shaperDrive[sh] + (now.drive - set.drive));
        save(v.levels.shaperOutput[sh], v.levels.shaperOutput[sh] + (now.output - set.output));
        save(v.levels.shaperMix[sh], v.levels.shaperMix[sh] + (now.mix - set.mix));
        save(v.levels.shaperBetween[sh], v.levels.shaperBetween[sh] + (now.between - set.between));
    }
}

// The block's steps, but a level the operators take somewhere counts as moving, so its route runs.
FMVIBE_NOINLINE const Synth::Levels &Synth::audioSteps(const Levels &step) {
    audioBy_ = step;
    const std::span<double> by = values(audioBy_), reach = values(audioReach_);
    for (size_t k = 0; k < by.size(); ++k)
        if (reach[k] > 0.0 && by[k] == 0.0)
            by[k] = reach[k];
    return audioBy_;
}

// Puts back what audioSample() moved, with what has moved it since (the ramps', the increments' and the warp's steps).
FMVIBE_NOINLINE void Synth::audioRestore() {
    AudioRun &run = audioRun_;
    for (int k = run.savedCount - 1; k >= 0; --k) {
        const AudioRun::Saved &s = run.saved[k];
        *s.at = s.was + (*s.at - s.set);
    }
    run.savedCount = 0;
}

void Synth::shareVoiceParams(const ParamValues &params) {
    for (int i = 0; i < kVoiceParamCount; ++i) {
        voiceBase_[i] = params[i] + globalMod_[i] + macroMod_[i];
        // As a voice without host modulation of its own has it: with its modulation of 0 added (see renderBlock()).
        voiceClamped_[i] = std::clamp(voiceBase_[i] + 0.0, paramMin_[i], paramMax_[i]);
    }
    std::copy(voiceClamped_.begin(), voiceClamped_.end(), voiceParams_.begin());
    voiceParamsChangedCount_ = 0;
    voiceBaseReady_ = true;
}

// What a warped operator's copy adds to this sample and the next for the warp's bends its phase passes before the next
// sample, from `cycle` (`stretch` along the warp's stretch) on by `step`: each bend in its slope smoothed over the two
// samples (polyBLAMP), by the change in slope, in the output's units a sample: the wave's slope where the warp reads
// it there (over a sample at the faster side's speed) times the change in speed. Kept out of the voices' loop, which
// passes a bend only now and then.
namespace {

struct WarpBends {
    double now = 0.0, next = 0.0;
};
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
WarpBends warpBends(const WarpCoefs &coefs, const WarpKinks &kinks, const float *table, double cycle, double stretch,
                    double step) {
    WarpBends out;
    const double by = step * kinks.perCycle; // how far it goes, in stretches
    auto slopeAt = [&](double at, double span) { // the wave's, in its units a cycle
        return (WaveTables::read(table, at + span) - WaveTables::read(table, at - span)) / (2.0 * span);
    };
    auto smooth = [&](int o, double ahead) {
        const double until = ahead / by; // of a sample, to the bend
        const double after = 1.0 - until;
        if (coefs.mode == OpWarp::Sync) {
            // Sync's cycle end: the wave jumps from where its last cycle is cut off back to its start, as a hard
            // sync's restart does (polyBLEP), and its slope with it (polyBLAMP), at the same speed on either side.
            const double span = 0.5 * std::abs(step) * coefs.a;
            const double end = coefs.b, start = 0.0;
            const double jump =
                (WaveTables::read(table, start) - WaveTables::read(table, end)) * (step > 0.0 ? 1.0 : -1.0);
            const double bend = coefs.a * std::abs(step) * (slopeAt(start, span) - slopeAt(end, span));
            out.now += jump * 0.5 * after * after + bend * after * after * after / 6.0;
            out.next += -jump * 0.5 * until * until + bend * until * until * until / 6.0;
            return;
        }
        const double kink = cycle + ahead / kinks.perCycle;
        const double at = warpPhase(coefs, kink - std::floor(kink));
        const double span = 0.5 * std::abs(step) * std::max(kinks.before[o], kinks.after[o]);
        const double slope = slopeAt(at, span);
        const double bend = slope * std::abs(step) * (kinks.after[o] - kinks.before[o]);
        out.now += bend * after * after * after / 6.0;
        out.next += bend * until * until * until / 6.0;
    };
    for (int o = 0; o < kinks.count; ++o) {
        double ahead = kinks.offsets[o] - stretch;
        if (by > 0.0) {
            if (ahead <= 0.0)
                ahead += 1.0;
            for (; ahead <= by; ahead += 1.0)
                smooth(o, ahead);
        } else {
            if (ahead >= 0.0)
                ahead -= 1.0;
            for (; ahead >= by; ahead -= 1.0)
                smooth(o, ahead);
        }
    }
    return out;
}

} // namespace

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void Synth::unisonCopies(Operator &op, int index, int copies, double inc, double detune, double spread,
                         double monoGain, const UnisonModes &modes, const UnisonIncs *&incs,
                         const UnisonGains *&gains, std::array<double, kMaxUnison> &weights) {
    // The curve, the pan and the note's seed as one number, whole and exact: the seed only for the Random modes, so
    // the others keep their memos from note to note.
    const double seedKey = modes.curve == UniCurve::Random || modes.pan == UniPan::Random
                               ? static_cast<double>(modes.seed)
                               : 0.0;
    const double modesKey = static_cast<double>(modes.curve) +
                            static_cast<double>(UniCurve::Count) *
                                (static_cast<double>(modes.pan) + static_cast<double>(UniPan::Count) * seedKey);
    auto spreadOf = [&] { return unisonSpread(copies, modes.curve, modes.pan, modes.blend, modes.seed, index); };
    incs = &op.unisonIncs.get({inc, detune, static_cast<double>(copies), modesKey}, [&] {
        const UnisonSpreadOut at = spreadOf();
        UnisonIncs c{};
        for (int k = 0; k < copies; ++k)
            c.inc[k] = std::min(inc * std::exp2(at.detune[k] * detune / 1200.0), 0.5);
        return c;
    });
    gains = &op.unisonGains.get({spread, static_cast<double>(copies), modesKey, modes.blend}, [&] {
        const UnisonSpreadOut at = spreadOf();
        weights = at.weight;
        return copyGains(copies, at.pan, at.weight, spread, monoGain);
    });
}

Synth::UnisonModes Synth::unisonModesOf(const double *p, int op, uint64_t order) {
    // The Random modes draw from the note: a seed of its own, the same every block.
    return {static_cast<UniCurve>(
                stepped(p[paramIndex(op, OpParam::UnisonCurve)], static_cast<int>(UniCurve::Count) - 1)),
            static_cast<UniPan>(stepped(p[paramIndex(op, OpParam::UnisonPan)], static_cast<int>(UniPan::Count) - 1)),
            std::clamp(p[paramIndex(op, OpParam::UnisonBlend)], 0.0, 1.0), static_cast<uint32_t>(order)};
}

Synth::UnisonGains Synth::copyGains(int copies, const std::array<double, kMaxUnison> &pan,
                                    const std::array<double, kMaxUnison> &weight, double spread, double monoGain) {
    UnisonGains c{};
    for (int k = 0; k < copies; ++k) {
        const double angle = (pan[k] * spread + 1.0) * std::numbers::pi / 4.0;
        c.gainL[k] = std::numbers::sqrt2 * std::cos(angle) * monoGain * weight[k];
        c.gainR[k] = std::numbers::sqrt2 * std::sin(angle) * monoGain * weight[k];
    }
    return c;
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::array<double, kMaxUnison> Synth::unisonDetunes(int index, int copies, const UnisonModes &modes) {
    return unisonSpread(copies, modes.curve, modes.pan, modes.blend, modes.seed, index).detune;
}

// Takes what the last block's ramp moved operator `i`'s wave by into its copies' phases, and returns how far its Phase,
// modulated as the voice has it, still is from where the wave stands, the short way round (100 % is 0 % again), for
// this block's ramp.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
double Synth::turnPhase(Voice &v, int i, double phase) {
    Operator &op = v.ops[i];
    const double moved = v.levels.phase[i];
    for (int k = 0; k < kMaxUnison; ++k) {
        op.phases[k] += moved - std::floor(op.phases[k] + moved);
        op.startPhases[k] += moved - std::floor(op.startPhases[k] + moved);
    }
    v.levels.phase[i] = 0.0;
    v.phaseSet[i] += moved;
    const double left = std::remainder(phase - v.phaseSet[i], 1.0);
    if (std::abs(left) >= 1e-9)
        return left;
    v.phaseSet[i] = phase; // there
    return 0.0;
}

// Each copy read through its warp, and restarted by its master.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
Synth::OpSample Synth::warpedSample(Operator &op, const float *table, double offset, double factor, int copies,
                                    const UnisonGains *gains, const double *weights, bool sides,
                                    const std::array<double, kMaxUnison> &syncIncStep, double warpStep) {
    OpSample out;
    const WarpCoefs coefs = warpCoefs(op.warp, op.warpNow);
    const bool warping = coefs.mode != OpWarp::Off;
    const WarpKinks kinks = warpKinks(coefs);
    auto read = [&](double phase) {
        return warping ? WaveTables::readCycle(table, warpPhase(coefs, phase - std::floor(phase)))
                       : static_cast<double>(WaveTables::read(table, phase));
    };
    bool carrying = false;
    for (int k = 0; k < copies; ++k) {
        const double here = op.phases[k] + offset;
        const double cycle = here - std::floor(here); // where along its cycle it is, 0..1
        double x =
            (warping ? WaveTables::readCycle(table, warpPhase(coefs, cycle)) : WaveTables::read(table, here)) +
            op.syncCarry[k];
        op.syncCarry[k] = 0.0;
        const double advance = factor == 1.0 ? op.incNow[k] : std::clamp(op.incNow[k] * factor, -0.5, 0.5);
        op.phases[k] += advance;
        bool restarted = false;
        if (op.syncOn) {
            double master = op.syncPhases[k] + op.syncIncNow[k];
            if (master >= 1.0) {
                // The master's cycle ended `after` of a sample before the next one: the copy restarts at its start
                // there. Its step and the change in its slope are spread over this sample and the next as
                // band-limited ones would be (polyBLEP and polyBLAMP).
                master -= 1.0;
                const double after = std::min(master / op.syncIncNow[k], 1.0);
                const double at = op.phases[k] - after * advance; // where the restart cut it off
                const double start = op.startPhases[k];
                const double from = read(at + offset), to = read(start + offset);
                const double jump = to - from;
                const double bend = (read(start + advance + offset) - to) - (from - read(at - advance + offset));
                const double before = 1.0 - after;
                x += jump * 0.5 * after * after + bend * after * after * after / 6.0;
                op.syncCarry[k] = -jump * 0.5 * before * before + bend * before * before * before / 6.0;
                carrying = true;
                restarted = true;
                op.phases[k] = start + after * advance;
            }
            op.syncPhases[k] = master;
            op.syncIncNow[k] += syncIncStep[k];
        }
        // The warp's bends that the phase passes before the next sample (as it moves now), each smoothed as a sync's
        // restart is: by the change in the slope, over this sample and the next (polyBLAMP). Along each of the warp's
        // stretches (its steps, or the cycle), `ahead` is how far on a bend is.
        if (warping && !restarted && advance != 0.0) {
            const double along = cycle * kinks.perCycle;
            const double stretch = kinks.perCycle == 1.0 ? cycle : along - std::floor(along); // 0..1
            const double by = advance * kinks.perCycle; // how far it goes, in stretches
            // Only now and then does it pass one: then they are smoothed out of the loop's way.
            bool passes = false;
            for (int o = 0; o < kinks.count; ++o) {
                const double ahead = kinks.offsets[o] - stretch;
                passes = passes || (by > 0.0 ? (ahead > 0.0 ? ahead : ahead + 1.0) <= by
                                             : (ahead < 0.0 ? ahead : ahead - 1.0) >= by);
            }
            if (passes) {
                const WarpBends bends = warpBends(coefs, kinks, table, cycle, stretch, advance);
                x += bends.now;
                op.syncCarry[k] += bends.next;
                carrying = true;
            }
        }
        if (op.phases[k] >= 1.0 || op.phases[k] < 0.0)
            op.phases[k] -= std::floor(op.phases[k]);
        out.mono += x * weights[k];
        if (sides) {
            out.l += x * gains->gainL[k];
            out.r += x * gains->gainR[k];
        }
    }
    op.carrying = carrying;
    op.warpNow += warpStep;
    return out;
}

// What the voice's LFO in place `s` (an LFO, a Random source or a Curve) puts out this block (-1..1), as the block
// starts, moving it on by the block's `hostFrames`; `p` are the voice's params. Out of renderBlock(), whose code MSVC
// builds worse the more goes into it.
FMVIBE_NOINLINE double Synth::voiceLfo(Voice &v, int s, const double *p, const ParamValues &params,
                                       uint32_t hostFrames) const {
    auto get = [p](int i) { return p[i]; };
    Lfo &lfo = v.lfos[s];
    // A free LFO takes the shared clock, which render() moves on; switched
    // back to retrigger, it carries on from there. A one-shot is never free.
    // So does an anchored one, which render() moves to where the transport stands.
    const bool oneShot = lfoOneShot(get, s);
    const bool free = lfoShared(get, s);
    const LfoLoop loop = lfoLoop(get, s);
    const double chaos = p[paramIndex(s, SourceParam::RandomChaos)];
    // A note takes a new value for it to glide to; so does a note retriggering it in mono while it stands
    // still, where one running carries on from the value it holds. A joined voice's Random carries on with
    // the held one's step.
    if (free)
        lfo = freeLfos_[s];
    else if (v.newRandom && !v.keepLfos && (v.fresh || lfoHeld(get, s)))
        lfo.held = randomStep(lfo.from, chaos, v.rng);
    const LfoShape shape = lfoShape(get, s);
    // A note starts it at its Phase (100 % is 0 % again), read here so that host modulation sent with the
    // note applies. A free one keeps the shared clock, and Random its steps.
    if (lfo.restart && shape != LfoShape::Random) {
        const double start = p[paramIndex(s, SourceParam::Phase)];
        lfo.phase = start - std::floor(start);
    }
    lfo.restart = false;
    // A Curve in Sustain let go leaves its loop (see leaveLoop()); out of Sustain, it loops again.
    if (!lfoSustain(get, s))
        lfo.left = false;
    else if (lfo.letGo && !lfo.left)
        leaveLoop(params, s, lfo, v.curveReleases[s], loop);
    // Left in Sustain, the level it was at carried on top of the curve, within the curve's reach.
    const double curve = lfoOutput(get, params, s, lfo);
    const double value = lfo.left ? std::clamp(curve + leftOffset(lfo, v.curveReleases[s]), -1.0, 1.0) : curve;
    if (!free)
        advanceLfo(lfo, lfoHz(get, s, tempo_), hostFrames, v.rng, oneShot, chaos, loop, &v.curveReleases[s]);
    return value;
}

// A Curve's Slew: what it puts out lags behind its curve, as a MIDI source's follows its controller, from where the curve
// is as the note starts (`restart`). Off, it is the curve's value, which it keeps, so turned on it glides from there.
// `out` is what it put out last, `slew` its CurveSlew param.
FMVIBE_NOINLINE double Synth::curveSlew(double &out, double in, double slew, bool restart, uint32_t hostFrames) const {
    if (slew <= 0.0 || restart)
        out = in;
    else
        out = in + (out - in) * std::exp(-static_cast<double>(hostFrames) / (timeFromNorm(slew) * sampleRate_));
    return out;
}

// Out of renderBlock(), as only some patches play a drawn envelope.
FMVIBE_NOINLINE void Synth::followDrawn(Voice &v, int op, const double *p, const ParamValues &params) {
    Envelope &env = v.ops[op].env;
    DrawnEnvelope &drawn = v.drawn[op];
    if (p[paramIndex(op, OpParam::EnvMode)] < 0.5) { // switched back to its AHDSR, which goes on from where it is
        drawn.stop();
        env.unfollow(drawn.held());
        return;
    }
    const double seconds = p[paramIndex(op, OpParam::EnvSync)] >= 0.5
                               ? divisionSeconds(p[paramIndex(op, OpParam::EnvDivision)], tempo_)
                               : timeFromNorm(p[paramIndex(op, OpParam::EnvLength)]);
    double start = p[paramIndex(op, OpParam::EnvLoopStart)], end = p[paramIndex(op, OpParam::EnvLoopEnd)];
    start = std::isfinite(start) ? std::clamp(start, 0.0, 1.0) : 0.0;
    end = std::isfinite(end) ? std::clamp(end, 0.0, 1.0) : 1.0;
    const double slew = p[paramIndex(op, OpParam::EnvSlew)];
    drawn.prepare(&params[opEnvShapeIndex(op)], seconds * rate_, std::min(start, end), std::max(start, end),
                  p[paramIndex(op, OpParam::EnvSustain)] >= 0.5, env.level(), rate_,
                  slew > 0.0 ? timeFromNorm(slew) * rate_ : 0.0);
    env.follow(drawn);
}

void Synth::renderBlock(Voice &v, double *left, double *right, uint32_t hostFrames, const ParamValues &params) {
    // This voice's param values before LFOs and envelopes: automation plus host modulation, global and per voice.
    // Freeform shapes are read from `params`; slot modulation of the effects reaches them in render(). What every
    // voice shares is summed once a render() (see shareVoiceParams()), so a voice only adds its own host modulation,
    // where it has any (adding a modulation of 0 to the rest, as it always did).
    if (!voiceBaseReady_)
        shareVoiceParams(params);
    auto raw = [&](int i) { return voiceBase_[i] + v.mod[i]; };
    auto inRange = [this](int i, double value) { return std::clamp(value, paramMin_[i], paramMax_[i]); };

    // Then each param in its range, with the voice's slot modulation added to the params it moves, in the order it
    // comes. `p` starts as the shared values in their ranges with the voice's host modulation; withSlotMods() sets
    // the params the slots move, and puts back first those the slots of its last call moved. It is voiceParams_,
    // which holds the shared values but where the voice before changed them: those go back first, rather than every
    // voice copying all of them each block.
    for (int k = 0; k < voiceParamsChangedCount_; ++k)
        voiceParams_[voiceParamsChanged_[k]] = voiceClamped_[voiceParamsChanged_[k]];
    voiceParamsChangedCount_ = 0;
    double *const p = voiceParams_.data();
    auto set = [&](int i, double value) {
        p[i] = value;
        voiceParamsChanged_[voiceParamsChangedCount_++] = i;
    };
    for (int k = 0; k < v.moddedCount; ++k)
        set(v.modded[k], inRange(v.modded[k], raw(v.modded[k])));
    std::array<int, kAllSources * kSlots> moved;
    int movedCount = 0;
    auto withSlotMods = [&] {
        for (int k = 0; k < movedCount; ++k)
            set(moved[k], inRange(moved[k], raw(moved[k])));
        movedCount = 0;
        if (++slotPass_ == 0) { // the pass numbers wrapped around: forget the old ones
            std::fill(slotPassOf_.begin(), slotPassOf_.end(), 0u);
            slotPass_ = 1;
        }
        for (int m = 0; m < v.slotModCount; ++m) {
            const int i = v.slotMods[m].index;
            if (i >= kVoiceParamCount || v.slotMods[m].shown)
                continue;
            if (slotPassOf_[i] != slotPass_) { // the first slot on this param
                slotPassOf_[i] = slotPass_;
                slotSum_[i] = raw(i);
                moved[movedCount++] = i;
            }
            slotSum_[i] += v.slotMods[m].offset;
        }
        for (int k = 0; k < movedCount; ++k)
            set(moved[k], inRange(moved[k], slotSum_[moved[k]]));
    };

    // Modulation sources, read once per block at the host rate. They see the
    // previous block's slot modulation; the sound below sees this block's.
    withSlotMods();
    v.slotModCount = 0;
    // A unipolar slot takes the source's 0..1 out of `unipolar` times its depth, one way; a bipolar one takes
    // `bipolar`, which swings half the depth either way of the set value: a bipolar LFO's -1..1 halved, an envelope's
    // level about its middle. Each slot is one or the other as set for it, or as its source is, which `bipolarSource`
    // says (see slotBipolar()). `shown` is what the editor shows of the source.
    // Each slot's depth is scaled by its Amount and, with an aux, by what the aux puts out (see SlotAuxParam): a
    // macro's or the mod wheel's knob, or what a source of this voice put out last.
    auto getParam = [&params](int i) { return params[i]; };
    auto scaleOf = [&](int source, int s) {
        double scale = hostValue(params, slotScaleIndex(source, s));
        if (const int aux = slotAux(getParam, source, s); aux >= 0)
            scale *= auxScale(hostValue(params, slotAuxDepthIndex(source, s)),
                              runsPerVoice(aux) || isMacro(aux) ? v.auxOut[aux] : macroKnobs_[kMacros]);
        return scale;
    };
    // What slot `s` of `source` takes of the source's `swing`, through the slot's curve (see slotCurveValue()).
    auto bent = [&](int source, int s, double swing, bool bipolar) {
        return slotCurveValue(swing, hostValue(params, slotCurveIndex(source, s)), bipolar);
    };
    // The pitch the voice plays: its glide, tuning and the bend, with the params as `p` holds them when it is asked;
    // with MPE on, a note on a member channel bends with its channel too. The bend is the host's MIDI pitch bend and the
    // Pitch Bend param (the editor's bend wheel) together, the param with the host's modulation of it for this voice
    // (a modulator per note bends each note its own way).
    const double bend = std::clamp(bend_ + seqBend_ + p[paramIndex(GlobalParam::PitchBend)], -1.0, 1.0);
    const double memberBend =
        mpe_ && memberChannel(v.note.channel) ? channelBend_[v.note.channel] * mpeBendRange_ : 0.0;
    auto pitchNow = [&] {
        return v.glide + v.tuning + v.seqTune + memberBend + bend * p[paramIndex(GlobalParam::BendRange)] +
               p[paramIndex(GlobalParam::Pitch)];
    };
    // Velocity puts out what the note came with, known before the places run, so the slots it scales as their aux
    // take this block's value; on a note's first block, so do those the note source scales, from the pitch the note
    // starts at (after that, they take the last block's, as it follows the pitch further down).
    v.auxOut[kVelocitySource] = velocitySourceValue(getParam, v.velocity);
    // So does the pitch bend, where it stands now, 0..1 with its middle at rest: the host's MIDI bend and the Pitch Bend
    // param's, and the note's own channel's with MPE on, each a share of its own range.
    v.auxOut[kBendSource] =
        0.5 * (std::clamp(bend + (mpe_ && memberChannel(v.note.channel) ? channelBend_[v.note.channel] : 0.0), -1.0, 1.0) +
               1.0);
    if (v.fresh)
        v.auxOut[kNoteSource] = noteSourceValue([&](int i) { return hostValue(params, i); }, pitchNow());
    // The macros' knobs as every voice shares them, but those the voice's sources move (below), which a note starts
    // from there.
    for (int m = 0; m < kMacros; ++m)
        if (v.fresh || !(macroGraph_.voiceMoved >> m & 1u))
            v.auxOut[kMaxSources + m] = macroKnobs_[m];
    if (!sharedSlotsReady_)
        shareSlots(params);
    // A shared slot's depth scale in this voice: what is the same for every voice, times its own source's aux.
    auto sharedScale = [&](const SharedSlot &slot) {
        return slot.voiceAux >= 0 ? slot.scale * auxScale(slot.auxDepth, v.auxOut[slot.voiceAux]) : slot.scale;
    };
    auto collectSlots = [&](int source, double shown, double unipolar, double bipolar, bool bipolarSource) {
        v.sourceValues[source] = shown;
        v.auxOut[source] = unipolar;
        const double swing = bipolarSource ? bipolar : unipolar;
        for (int k = 0; k < sharedSlotCount_[source]; ++k) {
            const SharedSlot &slot = sharedSlots_[source][k];
            const double amount = p[slotAmountIndex(source, slot.slot)];
            if (amount != 0.0) {
                v.slotMods[v.slotModCount++] = {slot.target,
                                                slotCurveValue(swing, slot.curve, bipolarSource) * amount *
                                                    sharedScale(slot) * slot.range,
                                                source * kSlots + slot.slot};
            }
        }
    };
    // The operators, from what they put out over the last block, known before the places run, so the slots they scale
    // as their aux take it too.
    if (opSourceMask_ != 0)
        runOpSources(v, params);
    // Empty places and switched-off sources stand still and modulate nothing.
    auto get = [&p](int i) { return p[i]; };
    for (int s = 0; s < kMaxSources; ++s) {
        const SourceType type = sourceType(p[paramIndex(s, SourceParam::Type)]);
        if (type == SourceType::None || p[sourceEnabledIndex(s)] < 0.5) {
            v.sourceValues[s] = 0.0;
            v.auxOut[s] = 0.0; // scales nothing, as it modulates nothing
            continue;
        }
        if (runsLfo(type)) { // an LFO, or Random, a random one
            double value = voiceLfo(v, s, p, params, hostFrames);
            if (type == SourceType::Curve) // a joined voice's from where the held one's was
                value = curveSlew(v.follow[s], value, p[paramIndex(s, SourceParam::CurveSlew)],
                                  v.fresh && !v.keepFollow, hostFrames);
            const bool lfoUnipolar = !sourceBipolar(get, s);
            collectSlots(s, lfoUnipolar ? 0.5 * (value + 1.0) : value, 0.5 * (value + 1.0), 0.5 * value, !lfoUnipolar);
            continue;
        }
        if (type == SourceType::Midi) {
            // It follows its controller, or the note's pressure (else the channel's) or slide, through Slew, from
            // where it is as the note starts.
            double &out = v.follow[s];
            const auto input = static_cast<MidiInput>(
                stepped(p[paramIndex(s, SourceParam::MidiInput)], static_cast<int>(MidiInput::Count) - 1));
            const double in = input == MidiInput::Pressure ? (v.pressure >= 0.0 ? v.pressure : channelPressure_)
                              : input == MidiInput::Slide  ? v.slide
                                                           : cc_[static_cast<size_t>(
                                                                stepped(p[paramIndex(s, SourceParam::CcNumber)], 127))];
            out = v.fresh ? in
                          : in + (out - in) * std::exp(-static_cast<double>(hostFrames) /
                                                       (timeFromNorm(p[paramIndex(s, SourceParam::Slew)]) * sampleRate_));
            collectSlots(s, out, out, out - 0.5, p[paramIndex(s, SourceParam::Bipolar)] >= 0.5);
            continue;
        }
        Envelope &env = v.modEnvs[s];
        collectSlots(s, env.level(), env.level(), env.level() - 0.5, p[paramIndex(s, SourceParam::EnvBipolar)] >= 0.5);
        const std::array<double, 8> times{
            p[paramIndex(s, SourceParam::Attack)],      p[paramIndex(s, SourceParam::Hold)],
            p[paramIndex(s, SourceParam::Decay)],       p[paramIndex(s, SourceParam::Sustain)],
            p[paramIndex(s, SourceParam::Release)],     p[paramIndex(s, SourceParam::AttackCurve)],
            p[paramIndex(s, SourceParam::DecayCurve)], p[paramIndex(s, SourceParam::ReleaseCurve)]};
        const EnvRates &rates = v.modEnvRates[s].get(times, [&] { return envRatesOf(times, sampleRate_); });
        std::array<double, kControlBlock> levels; // as the block goes; the sources take its last
        env.run(rates, levels.data(), static_cast<int>(hostFrames));
    }
    // Velocity, through its curve, from what the note came with: like the note source's, its depths take only the
    // host's modulation for every voice. It modulates from the note's first block, as the LFOs and envelopes do.
    {
        const double out = v.auxOut[kVelocitySource]; // worked out before the places
        const bool bipolar = params[paramIndex(VelocityParam::Bipolar)] >= 0.5;
        const double swing = bipolar ? out - 0.5 : out;
        for (int k = 0; k < sharedSlotCount_[kVelocitySource]; ++k) {
            const SharedSlot &slot = sharedSlots_[kVelocitySource][k];
            if (slot.depth == 0.0)
                continue;
            v.slotMods[v.slotModCount++] = {slot.target,
                                            slotCurveValue(swing, slot.curve, bipolar) * slot.depth *
                                                sharedScale(slot) * slot.range,
                                            kVelocitySource * kSlots + slot.slot};
        }
    }
    // The pitch bend's, likewise, from where it stands this block. Bipolar, as it starts, it moves nothing at rest.
    {
        const bool bipolar = params[paramIndex(BendParam::Bipolar)] >= 0.5;
        const double swing = v.auxOut[kBendSource] - (bipolar ? 0.5 : 0.0);
        for (int k = 0; k < sharedSlotCount_[kBendSource]; ++k) {
            const SharedSlot &slot = sharedSlots_[kBendSource][k];
            if (slot.depth == 0.0)
                continue;
            v.slotMods[v.slotModCount++] = {slot.target,
                                            slotCurveValue(swing, slot.curve, bipolar) * slot.depth *
                                                sharedScale(slot) * slot.range,
                                            kBendSource * kSlots + slot.slot};
        }
    }
    // The macros' knobs as this voice's sources take them (see MacroGraph::voiceMoved): with what the places, velocity
    // and the pitch bend add to them this block, and the note source from where it stood the last block (on a note's
    // first, where it starts), then the macros' slots on one another. Then what those knobs make of the slots they
    // move, against what every voice shares of them (in macroMod_ already).
    // A macro's or the mod wheel's knob as the voice has it.
    auto voiceKnob = [&](int source) { return isMacro(source) ? v.auxOut[source] : macroKnobs_[kMacros]; };
    if (macroGraph_.voiceMoved != 0) {
        std::array<double, kMacros> extra{};
        for (int m = 0; m < v.slotModCount; ++m)
            if (isMacroKnob(v.slotMods[m].index))
                extra[macroOf(v.slotMods[m].index)] += v.slotMods[m].offset;
        const bool noteBipolar = params[paramIndex(NoteParam::Bipolar)] >= 0.5;
        for (int k = 0; k < sharedSlotCount_[kNoteSource]; ++k) {
            const SharedSlot &slot = sharedSlots_[kNoteSource][k];
            if (!isMacroKnob(slot.target) || slot.depth == 0.0)
                continue;
            const double offset = slotCurveValue(v.auxOut[kNoteSource] - (noteBipolar ? 0.5 : 0.0), slot.curve,
                                                 noteBipolar) *
                                  slot.depth * sharedScale(slot) * slot.range;
            v.slotMods[v.slotModCount++] = {slot.target, offset, kNoteSource * kSlots + slot.slot};
            extra[macroOf(slot.target)] += offset;
        }
        MacroKnobs knobs = macroSet_;
        runMacroGraph(
            macroGraph_, [&](int i) { return hostValue(params, i); }, knobs, extra.data(),
            [&](int aux) { return v.auxOut[aux]; }, [](int, int, double) {});
        for (int m = 0; m < kMacros; ++m)
            v.auxOut[kMaxSources + m] = knobs[m];
        for (int k = 0; k < voiceMovedSlotCount_; ++k) {
            const VoiceMovedSlot &slot = voiceMovedSlots_[k];
            const double offset = slotCurveValue(voiceKnob(slot.source) - (slot.bipolar ? 0.5 : 0.0), slot.curve,
                                                 slot.bipolar) *
                                      slot.depth * (slot.aux >= 0 ? auxScale(slot.auxDepth, voiceKnob(slot.aux)) : 1.0) -
                                  slot.shared;
            v.slotMods[v.slotModCount++] = {slot.target, offset, slot.source * kSlots + slot.slot};
        }
    }
    // The macros' and the mod wheel's slots that an aux of this voice scales (see applyMacros()).
    for (int k = 0; k < voiceMacroSlotCount_; ++k) {
        const auto [source, s] = voiceMacroSlots_[k];
        const int target = slotTarget(params[slotTargetIndex(source, s)]);
        const double amount = hostValue(params, slotAmountIndex(source, s));
        if (target < 0 || amount == 0.0)
            continue;
        const bool bipolar = sourceBipolar(getParam, source);
        const double swing = voiceKnob(source) - (bipolar ? 0.5 : 0.0);
        v.slotMods[v.slotModCount++] = {target,
                                        bent(source, s, swing, bipolar) * amount * scaleOf(source, s) *
                                            (table_[target].max - table_[target].min),
                                        source * kSlots + s};
    }
    withSlotMods();
    v.newRandom = false;

    // A new note starts each operator copy at its Phase, moved on by up to Phase
    // Random of a cycle. Done here rather than at note-on, so host modulation
    // sent with the note, and the sources' modulation as of the note's first block, apply.
    const bool starting = v.fresh;
    if (v.fresh) {
        v.fresh = false;
        v.keepLfos = v.keepFollow = false;
        for (int i = 0; i < kOps; ++i) {
            const double start = p[paramIndex(i, OpParam::Phase)];
            const double amount = p[paramIndex(i, OpParam::PhaseRandom)];
            v.phaseSet[i] = start;
            for (int k = 0; k < kMaxUnison; ++k) {
                const double phase = start + amount * 0.5 * (nextRandom(v.rng) + 1.0);
                v.ops[i].phases[k] = phase - std::floor(phase);
                v.ops[i].startPhases[k] = v.ops[i].phases[k];
                v.ops[i].syncPhases[k] = 0.0; // a cycle of the note starts with the note
                v.ops[i].syncCarry[k] = 0.0;
            }
            v.ops[i].carrying = false;
        }
    }

    // The sequencer's bend of the note moves on along the stretch it planned.
    if (v.seqTuneLeft > 0) {
        const uint32_t n = std::min(v.seqTuneLeft, hostFrames);
        v.seqTuneLeft -= n;
        v.seqTune = v.seqTuneLeft > 0 ? v.seqTune + v.seqTuneStep * n : v.seqTuneTo;
    }

    // Retuning continuously, the note follows its tuning as it changes, at once: what is left of a glide stays where
    // it was from the note. Without a tuning (taken away, or out of the patch), it goes back to 12-TET.
    if (tuneContinuous_ || !tuned()) {
        const double retune = retuneOf(v.note);
        v.glide += retune - v.retune;
        v.retune = retune;
    }
    // Glide: the pitch closes 99 % of the gap to the note in the glide time.
    const double tuned = v.note.key + v.retune;
    const double glideTime = p[paramIndex(GlobalParam::Glide)];
    if (glideTime <= 0.0 || std::abs(v.glide - tuned) < 1e-3)
        v.glide = tuned;
    else
        v.glide = tuned + (v.glide - tuned) *
                              std::exp(std::log(0.01) * hostFrames / (timeFromNorm(glideTime) * sampleRate_));

    // From here on the voice runs at its own rate, `frames` samples. Gains and
    // levels ramp toward the block's values over a control block's length,
    // however short this block is: blocks split by events then carry on the
    // same ramp rather than jumping.
    const uint32_t frames = hostFrames * static_cast<uint32_t>(oversampling_);
    const double ramp = 1.0 / (kControlBlock * oversampling_);
    double pitch = pitchNow();

    // The note source follows that pitch, through its range and curve, and modulates from this block on: a
    // cutoff it takes to the note sits on the pitch the operators play now. Its targets are worked out again with the other sources' modulation,
    // which they already hold; a target that moves the pitch itself moves it from here. Its range and slots take
    // only the host's modulation for every voice, no LFO's or envelope's.
    v.pitch = pitch;
    // Bipolar, it swings about the middle of its output, like a macro.
    const double note = noteSourceValue([&](int i) { return hostValue(params, i); }, pitch);
    v.auxOut[kNoteSource] = note;
    const bool noteBipolar = params[paramIndex(NoteParam::Bipolar)] >= 0.5;
    const double swing = note - (noteBipolar ? 0.5 : 0.0);
    const int noteModsStart = v.slotModCount;
    for (int k = 0; k < sharedSlotCount_[kNoteSource]; ++k) {
        const SharedSlot &slot = sharedSlots_[kNoteSource][k];
        if (slot.depth != 0.0 && !isMacroKnob(slot.target)) { // its slots on macros' knobs came with the macros
            v.slotMods[v.slotModCount++] = {slot.target,
                                            slotCurveValue(swing, slot.curve, noteBipolar) * slot.depth *
                                                sharedScale(slot) * slot.range,
                                            kNoteSource * kSlots + slot.slot};
        }
    }
    for (int m = noteModsStart; m < v.slotModCount; ++m) {
        const int target = v.slotMods[m].index;
        if (target >= kVoiceParamCount)
            continue; // an effect's, which render() applies
        double modulated = raw(target);
        for (int k = 0; k < v.slotModCount; ++k)
            modulated += v.slotMods[k].index == target && !v.slotMods[k].shown ? v.slotMods[k].offset : 0.0;
        set(target, inRange(target, modulated));
    }
    if (v.slotModCount > noteModsStart)
        pitch = pitchNow();

    // A released voice whose sound is over runs on only while a mod envelope that modulates something releases, or a
    // Curve in Sustain let go plays on to its end (see modulationReleasing()), so what it modulates (an effect's knob,
    // which follows the newest voice, and what the editor shows) follows the release down rather than jumping back;
    // it renders nothing meanwhile, and ends once they have.
    if (v.silent) {
        v.peak = 0.0;
        v.opMean.fill(0.0); // its operators play no more
        v.opLevel.fill(0.0);
        if (v.fading || !modulationReleasing(v, params))
            endVoice(v);
        return;
    }

    const double noteHz = 440.0 * std::exp2((pitch - 69.0) / 12.0);
    const double fmTarget = p[paramIndex(GlobalParam::FmAmount)];
    // The voices go into the effects at a level of their own; Volume sets the level after them (see render()).
    const double gain = kVoiceLevel * v.exprGain;
    // Equal-power pan, unity at the center.
    const double pan = std::clamp(p[paramIndex(GlobalParam::Pan)] + v.exprPan, -1.0, 1.0);
    const double angle = (pan + 1.0) * std::numbers::pi / 4.0;
    // A new voice starts at its gains and FM amount, as at its levels (below): its envelopes alone shape its start,
    // so a 0 ms attack starts at full level at once.
    if (starting) {
        v.fmScale = fmTarget;
        v.gainL = gain * std::numbers::sqrt2 * std::cos(angle);
        v.gainR = gain * std::numbers::sqrt2 * std::sin(angle);
    }
    const double fmStep = (fmTarget - v.fmScale) * ramp;
    const double gainLStep = (gain * std::numbers::sqrt2 * std::cos(angle) - v.gainL) * ramp;
    const double gainRStep = (gain * std::numbers::sqrt2 * std::sin(angle) - v.gainR) * ramp;
    // A stolen voice fades out, then ends; one joining a held note in poly legato fades in.
    const double fadeStep = v.fading ? -1.0 / (kStealFade * rate_) : v.fade < 1.0 ? 1.0 / (kJoinFade * rate_) : 0.0;
    // White noise is as loud in the audible band as when it runs at the host rate.
    const double whiteGain = noiseColor_.coefs(0.0).gain;

    Levels target{};
    std::array<EnvRates, kOps> rates;
    std::array<const float *, kOps> wave;
    std::array<OpWave, kOps> opShapes;   // each operator's wave, and the increment its table suits, for when
    std::array<double, kOps> highestInc; // frequency modulation takes it higher (see below)
    std::array<bool, kOps> noise;
    // Noise has no pitch and no unison copies. Its color comes from a filter, and
    // Unison Spread widens it with a second noise on the sides.
    std::array<bool, kOps> colored;
    std::array<NoiseColor::Coefs, kOps> noiseCoefs;
    std::array<double, kOps> noiseMid;
    std::array<double, kOps> noiseSide;
    // Unison: each copy has its own increment and stereo gains. The copies'
    // mono sum, scaled by 1/sqrt(copies), is what other operators hear (each by its weight once Blend moves them).
    std::array<int, kOps> copies;
    std::array<double, kOps> monoGain;
    std::array<std::array<double, kMaxUnison>, kOps> incStep; // per sample, toward the block's increments
    std::array<bool, kOps> incMoving{}; // whether any of an operator's increments moves this block
    std::array<const UnisonIncs *, kOps> copy;
    // Sync and the warp (see Operator): the operators that read their wave through either, or still carry a sync's
    // smoothing over, take a loop of their own (see warpedSample()), as do those whose copies Blend weighs. The
    // masters' increments move like the copies' do.
    std::array<bool, kOps> warped{};
    std::array<double, kOps> warpStep{};
    std::array<std::array<double, kMaxUnison>, kOps> syncIncStep;
    std::array<const UnisonGains *, kOps> copyGains;
    // Switched-off operators and filters are skipped and route nothing.
    std::array<bool, kOps> opOn;
    std::array<bool, kFilters> filterEnabled;
    for (int f = 0; f < kFilters; ++f)
        filterEnabled[f] = p[paramIndex(f, FilterParam::Enabled)] >= 0.5;
    std::array<bool, kShapers> shaperEnabled;
    for (int sh = 0; sh < kShapers; ++sh)
        shaperEnabled[sh] = p[paramIndex(sh, ShaperParam::Enabled)] >= 0.5;
    bool stereo = false;
    for (int i = 0; i < kOps; ++i) {
        Operator &op = v.ops[i];
        // Phase moved under the note, by its knob or its modulation, moves the wave along with it.
        const double phaseNow = p[paramIndex(i, OpParam::Phase)];
        if (phaseNow != v.phaseSet[i] || v.levels.phase[i] != 0.0)
            target.phase[i] = turnPhase(v, i, phaseNow);
        opOn[i] = p[paramIndex(i, OpParam::Enabled)] >= 0.5;
        if (!opOn[i]) {
            // Off is off at once; switched on again, it comes back from silence. It plays nothing and routes
            // nothing (its targets stay at 0), so there is nothing more to work out for it.
            op.out = 0.0;
            op.prevOut = 0.0;
            op.incKnown = false;
            op.syncOn = false;
            op.syncKnown = false;
            op.carrying = false;
            op.syncCarry.fill(0.0);
            op.warp = OpWarp::Off;
            op.warpNow = 0.0;
            v.levels.out[i] = 0.0;
            v.levels.send[i].fill(0.0);
            v.levels.shaperSend[i].fill(0.0);
            v.levels.index[i].fill(0.0);
            continue;
        }
        // Fixed, it plays at its Freq x Mult: the note, bend, glide and tuning, its ratio, Pitch and Detune don't move
        // it; its unison copies still spread about it.
        double hz = fixedHz(p[paramIndex(i, OpParam::FixedFreq)], p[paramIndex(i, OpParam::FixedMult)]);
        if (p[paramIndex(i, OpParam::Fixed)] < 0.5) {
            const double ratio = ratioFromNorm(std::clamp(p[paramIndex(i, OpParam::Ratio)], 0.0, kRatioModNorm));
            const double cents = p[paramIndex(i, OpParam::Detune)];
            const double semitones = p[paramIndex(i, OpParam::Transpose)];
            hz = noteHz * ratio * std::exp2(semitones / 12.0 + cents / 1200.0);
        }
        const double inc = std::min(hz / rate_, 0.5);
        const auto shape = static_cast<OpWave>(
            stepped(p[paramIndex(i, OpParam::Wave)], static_cast<int>(OpWave::Count) - 1));
        noise[i] = shape == OpWave::Noise;

        const std::array<double, 8> times = opEnvFields(p, i);
        rates[i] = op.rates.get(times, [&] { return envRatesOf(times, rate_); });
        if (p[paramIndex(i, OpParam::EnvMode)] >= 0.5 || op.env.following()) // its drawn envelope instead
            followDrawn(v, i, p, params);
        target.out[i] = p[paramIndex(i, OpParam::Out)];
        // Its Pan, equal-power and unity at the center, as the voice's (above): on what it puts out, to the output,
        // the filters and the shapers; what other operators hear of it stays mono.
        const double opPan = std::clamp(p[paramIndex(i, OpParam::Pan)], -1.0, 1.0);
        const double opAngle = (opPan + 1.0) * std::numbers::pi / 4.0;
        target.panL[i] = std::numbers::sqrt2 * std::cos(opAngle);
        target.panR[i] = std::numbers::sqrt2 * std::sin(opAngle);
        double heard = target.out[i];
        for (int f = 0; f < kFilters; ++f) {
            target.send[i][f] = filterEnabled[f] ? p[sendIndex(i, f)] : 0.0;
            heard = std::max(heard, target.send[i][f]);
        }
        for (int sh = 0; sh < kShapers; ++sh) {
            target.shaperSend[i][sh] = shaperEnabled[sh] ? p[shaperSendIndex(i, sh)] : 0.0;
            heard = std::max(heard, target.shaperSend[i][sh]);
        }

        // Copies spread from -1 to 1 as Unison Curve and Unison Pan say (see unison.h): detuned by up to Unison
        // Detune cents and panned by up to Unison Spread, Blend turning the outer ones down.
        copies[i] = noise[i] ? 1
                             : std::clamp(static_cast<int>(std::lround(p[paramIndex(i, OpParam::Unison)])), 1,
                                          kMaxUnison);
        monoGain[i] = 1.0 / std::sqrt(static_cast<double>(copies[i]));
        const double detune = p[paramIndex(i, OpParam::UnisonDetune)];
        const double spread = p[paramIndex(i, OpParam::UnisonSpread)];
        const UnisonModes modes = unisonModesOf(p, i, v.order);
        // The warp: its amount ramps to this block's over a control block, as the levels do; a warp changed to another
        // first takes its amount down to 0 (the plain wave, whatever the warp), then switches and takes it up again.
        double squeeze = 1.0; // how much faster the warp reads the wave at its steepest, for the table
        {
            const auto want = noise[i] ? OpWarp::Off
                                       : static_cast<OpWarp>(stepped(p[paramIndex(i, OpParam::Warp)],
                                                                      static_cast<int>(OpWarp::Count) - 1));
            const double amount =
                want == OpWarp::Off ? 0.0 : std::clamp(p[paramIndex(i, OpParam::WarpAmount)], -1.0, 1.0);
            if (starting) {
                op.warp = want;
                op.warpNow = amount;
            }
            if (want != op.warp && (op.warp == OpWarp::Off || op.warpNow == 0.0)) {
                op.warp = want;
                op.warpNow = 0.0;
            }
            const double to = want == op.warp ? amount : 0.0;
            if (std::abs(to - op.warpNow) < 1e-6) // close enough: there, rather than ever closer
                op.warpNow = to;
            // At most kWarpSlew of its range a second, so a jump (a host's, or the duck to 0 and back of a warp
            // changed) moves the wave's shape over a few milliseconds; modulation moves far slower than that.
            const double most = kWarpSlew / rate_;
            warpStep[i] = std::clamp((to - op.warpNow) * ramp, -most, most);
            if (op.warp == OpWarp::Off && want == OpWarp::Off)
                op.warpNow = 0.0;
            if (op.warp != OpWarp::Off)
                squeeze = warpSqueeze(op.warp, std::max(std::abs(op.warpNow), std::abs(to)));
        }
        // Sync to the note: switched on, a master starts a cycle of the note there.
        const bool syncing = !noise[i] && p[paramIndex(i, OpParam::Sync)] >= 0.5;
        if (syncing && !op.syncOn) {
            op.syncPhases.fill(0.0);
            op.syncKnown = false;
        }
        op.syncOn = syncing;
        if (noise[i]) { // noise has no cycle: what a sync carried is let go, and the warp with it
            op.carrying = false;
            op.warp = OpWarp::Off;
            op.warpNow = 0.0;
        }
        // Blended copies weigh into the mono sum other operators hear, which only that loop does: the plain loops
        // below, which most operators run, sum them as they are (a multiply a copy there cost every patch 2-3 %).
        // So does one whose Phase is moving its wave, for as long as that takes.
        warped[i] = op.syncOn || op.warp != OpWarp::Off || op.carrying || (modes.blend > 0.0 && copies[i] > 1) ||
                    (!noise[i] && target.phase[i] != 0.0);

        // The table suits the highest copy, so none of them reaches past Nyquist, and the warp's steepest stretch.
        const double highest = (copies[i] > 1 ? inc * std::exp2(detune / 1200.0) : inc) * squeeze;
        if (shape == OpWave::Custom)
            customWaves_[i].build(); // its tables follow its partials once a voice plays it
        wave[i] = shape == OpWave::Custom ? customWaves_[i].table(highest) : waves_.table(shape, highest);
        opShapes[i] = shape;
        highestInc[i] = highest;
        if (noise[i]) {
            const double slope = p[paramIndex(i, OpParam::NoiseSlope)];
            colored[i] = slope > 0.0;
            if (colored[i])
                noiseCoefs[i] = noiseColor_.coefs(slope);
            // Left and right are mid +- side: uncorrelated at full spread, each as loud as the mid alone.
            noiseMid[i] = 1.0 / std::sqrt(1.0 + spread * spread);
            noiseSide[i] = spread * noiseMid[i];
        }
        // The gains apart from the increments: a pitch that moves (vibrato, a bend, glide) works out only these.
        unisonCopies(op, i, copies[i], inc, detune, spread, monoGain[i], modes, copy[i], copyGains[i], v.copyWeights[i]);
        // Panned, or still ramping back from a pan, it is stereo too.
        const bool panned = opPan != 0.0 || v.levels.panL[i] != v.levels.panR[i];
        stereo = stereo || ((((copies[i] > 1 || noise[i]) && spread > 0.0) || panned) && heard > 0.0);
        // The copies' increments move to this block's over its length rather than jumping at its start, so
        // modulated pitch (Ratio most of all) doesn't step every control block.
        if (starting || !op.incKnown || op.incCopies != copies[i])
            for (int k = 0; k < copies[i]; ++k)
                op.incNow[k] = copy[i]->inc[k];
        op.incKnown = !noise[i]; // noise moves no increment along: back to a wave, it starts at its own
        op.incCopies = copies[i];
        for (int k = 0; k < copies[i]; ++k) {
            incStep[i][k] = (copy[i]->inc[k] - op.incNow[k]) / static_cast<double>(std::max<uint32_t>(frames, 1));
            incMoving[i] = incMoving[i] || incStep[i][k] != 0.0;
        }
        // The masters: each a cycle of the note, detuned as its copy is.
        if (op.syncOn) {
            const double noteInc = std::min(noteHz / rate_, 0.5);
            const std::array<double, kMaxUnison> shares = unisonDetunes(i, copies[i], modes);
            for (int k = 0; k < copies[i]; ++k) {
                const double to = std::min(noteInc * std::exp2(shares[k] * detune / 1200.0), 0.5);
                if (starting || !op.syncKnown || op.syncCopies != copies[i])
                    op.syncIncNow[k] = to;
                syncIncStep[i][k] = (to - op.syncIncNow[k]) / static_cast<double>(std::max<uint32_t>(frames, 1));
            }
            op.syncKnown = true;
            op.syncCopies = copies[i];
        }

        // Squared amounts give finer control over the subtle low end.
        for (int to = 0; to < kOps; ++to) {
            const double amount = p[matrixIndex(i, to)];
            target.index[i][to] = routeLevel(routeModes_[i][to], amount, i == to ? kMaxFeedback : kMaxIndex);
        }
    }

    // A filter can feed any filter, itself too: link[from][to]. A later filter hears an earlier one as it is this
    // sample; a filter hears itself and the filters after it from the previous sample, and in a voice's tail no longer
    // (see Voice::tail). Filters modulate operators like operators do, and feed the shapers, from the previous sample's
    // output; in a voice's tail, no longer the shapers.
    for (int from = 0; from < kFilters; ++from)
        for (int to = 0; to < kFilters; ++to)
            target.link[from][to] = filterEnabled[from] && filterEnabled[to] && (to > from || !v.tail)
                                        ? p[filterLinkIndex(from, to)]
                                        : 0.0;
    for (int f = 0; f < kFilters; ++f) {
        for (int to = 0; to < kOps; ++to) {
            const double amount = filterEnabled[f] ? p[filterModIndex(f, to)] : 0.0;
            target.filterIndex[f][to] = routeLevel(routeModes_[kOps + f][to], amount, kMaxIndex);
        }
        for (int sh = 0; sh < kShapers; ++sh)
            target.filterShaper[f][sh] =
                filterEnabled[f] && shaperEnabled[sh] && !v.tail ? p[filterShaperIndex(f, sh)] : 0.0;
        target.filterOut[f] = p[paramIndex(f, FilterParam::Out)];
        target.filterMix[f] = p[paramIndex(f, FilterParam::Mix)];
    }
    // A shaper clips what the operators, filters and shapers send it, like the shaper effect, and from there feeds
    // the filters and shapers, modulates operators and goes out, like a filter does. Shapers hear the shapers
    // before them as they are this sample, themselves and those after them from the previous one; in a voice's tail,
    // no longer (see Voice::tail).
    std::array<ClipCurve, kShapers> curve;
    std::array<bool, kShapers> block;
    for (int sh = 0; sh < kShapers; ++sh) {
        for (int to = 0; to < kShapers; ++to)
            target.shaperShaper[sh][to] =
                shaperEnabled[sh] && shaperEnabled[to] && !v.tail ? p[shaperShaperIndex(sh, to)] : 0.0;
        for (int to = 0; to < kOps; ++to) {
            const double amount = shaperEnabled[sh] ? p[shaperModIndex(sh, to)] : 0.0;
            target.shaperIndex[sh][to] = routeLevel(routeModes_[kOps + kFilters + sh][to], amount, kMaxIndex);
        }
        for (int f = 0; f < kFilters; ++f)
            target.shaperLink[sh][f] = shaperEnabled[sh] && filterEnabled[f] ? p[shaperFilterIndex(sh, f)] : 0.0;
        target.shaperDirect[sh] = shaperEnabled[sh] ? p[paramIndex(sh, ShaperParam::Out)] : 0.0;
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        const ShaperSettings &settings = v.shaperSettings[sh].get(
            {p[fields.drive], p[fields.mode], p[fields.output], p[fields.link], p[fields.mix], p[fields.symmetric],
             p[fields.stack]},
            [&] { return shaperSettings([&p](int index) { return p[index]; }, fields); });
        target.shaperDrive[sh] = settings.drive;
        target.shaperOutput[sh] = settings.output;
        target.shaperMix[sh] = settings.mix;
        target.shaperBetween[sh] = settings.between;
        curve[sh] = shaperTables_[sh].curve(settings.mode, settings.symmetric);
        curve[sh].stack(settings.stack, rate_);
        block[sh] = clipNeedsBlocker(settings.mode);
    }
    // Operators moving params the voice plays by, every sample (see AudioKind).
    const bool audioOn = opSourceMask_ != 0 && audioLaneCount_ > 0 &&
                         startAudioMods(v, p, pitch, target,
                                        {rates, wave, opShapes, highestInc, noise, colored, noiseCoefs, noiseMid,
                                         noiseSide, copies, monoGain, copyGains, warped, opOn, filterEnabled,
                                         shaperEnabled, stereo});
    // What the routes are checked against: the block's levels, or with the operators, as far as they take each.
    const Levels &routeTo = audioOn ? audioTo_ : target;

    // A new voice starts at its levels; after that they ramp.
    if (starting)
        v.levels = target;
    Levels step{};
    bool ramping = false;
    // The levels that move while the block ramps, so each sample moves only those. The others take a step of +0
    // each sample, which leaves them as they are, but for a -0, which the first such step makes +0: done here once.
    std::array<int, sizeof(Levels) / sizeof(double)> moving;
    int movingCount = 0;
    {
        const std::span<double> now = values(v.levels), to = values(target), by = values(step);
        for (size_t k = 0; k < now.size(); ++k) {
            if (std::abs(to[k] - now[k]) < 1e-9)
                now[k] = to[k];
            else
                ramping = true;
            by[k] = (to[k] - now[k]) * ramp;
        }
        if (ramping)
            for (size_t k = 0; k < now.size(); ++k) {
                if (by[k] != 0.0 || std::signbit(by[k]))
                    moving[movingCount++] = static_cast<int>(k);
                else
                    now[k] += by[k];
            }
    }
    const Levels &routeBy = audioOn ? audioSteps(step) : step; // and the steps, likewise
    // Filters run only while something is routed into them (a route still
    // ramping down counts), and only once (on the left channel, copied right)
    // while the voice has no stereo spread.
    const int channels = stereo ? 2 : 1;
    if (stereo && v.rightStale) { // without spread, channel 1 would have been where channel 0 is
        for (int f = 0; f < kFilters; ++f) {
            v.filters[f][1] = v.filters[f][0];
            if (!v.combDirty[f][0])
                std::copy_n(combLine(v, f, 0), combSize_, combLine(v, f, 1));
            v.combDirty[f][1] = v.combDirty[f][0];
        }
        for (int sh = 0; sh < kShapers; ++sh)
            v.shapers[sh][1] = v.shapers[sh][0];
    }
    v.rightStale = !stereo;
    // Shapers run only while something goes into them and their output goes somewhere. A route counts while it is
    // set or still ramping down.
    auto routed = [](double to, double now) { return std::max(to, now) > 0.0; };
    const Levels &current = v.levels;
    std::array<bool, kShapers> shaperUsed{};
    bool filterShapes = false;
    for (int sh = 0; sh < kShapers; ++sh) {
        for (int to = 0; to < kOps; ++to)
            shaperUsed[sh] = shaperUsed[sh] || routed(routeTo.shaperIndex[sh][to], current.shaperIndex[sh][to]);
        for (int f = 0; f < kFilters; ++f)
            shaperUsed[sh] = shaperUsed[sh] || routed(routeTo.shaperLink[sh][f], current.shaperLink[sh][f]);
        shaperUsed[sh] =
            shaperEnabled[sh] && (shaperUsed[sh] || routed(routeTo.shaperDirect[sh], current.shaperDirect[sh]));
    }
    // A shaper feeding another one that is used is used too; one feeding only itself isn't.
    for (int pass = 0; pass < kShapers; ++pass)
        for (int sh = 0; sh < kShapers; ++sh)
            for (int to = 0; to < kShapers; ++to)
                shaperUsed[sh] = shaperUsed[sh] || (shaperEnabled[sh] && to != sh && shaperUsed[to] &&
                                                    routed(routeTo.shaperShaper[sh][to], current.shaperShaper[sh][to]));
    // What the operators feed, then what that feeds in turn: filters and shapers can feed each other, round in a
    // loop too, so it takes a few passes for what runs to settle.
    std::array<bool, kShapers> shaperOn{};
    std::array<bool, kFilters> filterOn{};
    for (int f = 0; f < kFilters; ++f) // still ringing after its inputs stopped
        filterOn[f] = filterEnabled[f] && v.filterRing[f] > 0;
    for (int i = 0; i < kOps; ++i) {
        for (int f = 0; f < kFilters; ++f)
            filterOn[f] = filterOn[f] || (filterEnabled[f] && routed(routeTo.send[i][f], current.send[i][f]));
        for (int sh = 0; sh < kShapers; ++sh)
            shaperOn[sh] =
                shaperOn[sh] || (shaperUsed[sh] && routed(routeTo.shaperSend[i][sh], current.shaperSend[i][sh]));
    }
    for (int pass = 0; pass < kFilters + kShapers; ++pass) {
        for (int f = 0; f < kFilters; ++f) {
            for (int from = 0; from < kFilters; ++from)
                filterOn[f] = filterOn[f] || (from != f && filterEnabled[f] && filterOn[from] &&
                                              routed(routeTo.link[from][f], current.link[from][f]));
            for (int sh = 0; sh < kShapers; ++sh)
                filterOn[f] = filterOn[f] || (filterEnabled[f] && shaperOn[sh] &&
                                              routed(routeTo.shaperLink[sh][f], current.shaperLink[sh][f]));
        }
        for (int sh = 0; sh < kShapers; ++sh) {
            for (int f = 0; f < kFilters; ++f) {
                const bool shapes =
                    shaperUsed[sh] && filterOn[f] && routed(routeTo.filterShaper[f][sh], current.filterShaper[f][sh]);
                shaperOn[sh] = shaperOn[sh] || shapes;
                filterShapes = filterShapes || shapes;
            }
            for (int from = 0; from < kShapers; ++from)
                shaperOn[sh] = shaperOn[sh] || (from != sh && shaperUsed[sh] && shaperOn[from] &&
                                                routed(routeTo.shaperShaper[from][sh], current.shaperShaper[from][sh]));
        }
    }
    bool shaperShapes = false; // whether any shaper that runs hears one (itself among them)
    for (int sh = 0; sh < kShapers; ++sh) {
        if (!shaperOn[sh]) {
            if (v.shaperHeld[sh]) { // when it runs again, it starts from silence
                v.shapers[sh] = {};
                v.shaperHeld[sh] = false;
            }
            v.shaperOut[sh] = 0.0;
            v.shaperLast[sh] = {};
            continue;
        }
        v.shaperHeld[sh] = true;
        for (int from = 0; from < kShapers; ++from)
            shaperShapes = shaperShapes ||
                           (shaperOn[from] && routed(routeTo.shaperShaper[from][sh], current.shaperShaper[from][sh]));
    }
    const double pole = clipBlockerPole(rate_); // the shapers' DC blockers
    // The settings of the filters that run, and their comb lines; while a filter's inputs ramp (see below), its
    // settings as they stand at each step, and the inputs it ramps from and to over the block.
    std::array<const FilterSettings *, kFilters> filter{};
    std::array<FilterSettings, kFilters> filterStepped{};
    std::array<bool, kFilters> filterMoving{};
    // What a ramping filter scales its outputs by at the start and the end of the step it is in (see FilterGains),
    // and how many samples that step has.
    std::array<FilterGains, kFilters> gainsFrom{}, gainsTo{};
    std::array<uint32_t, kFilters> stepLength{};
    std::array<FilterInputs, kFilters> filterFrom{}, filterTo{};
    // How far along their ramp this block takes them (a block shorter than a control block going its share), and the
    // inputs `done` samples into it, at the end of the step that starts there.
    const double filterShare = std::min(1.0, static_cast<double>(hostFrames) / kControlBlock);
    const uint32_t filterStep = kFilterSubStep * static_cast<uint32_t>(oversampling_); // at the oversampled rate
    auto filterInputsAt = [&](int f, uint32_t done) {
        const double along =
            filterShare * std::min(1.0, static_cast<double>(done + filterStep) / static_cast<double>(frames));
        FilterInputs in = filterTo[f];
        for (size_t i = 0; i < in.size(); ++i)
            if (kFilterInputSmooth[i])
                in[i] = along >= 1.0 ? filterTo[f][i] : filterFrom[f][i] + (filterTo[f][i] - filterFrom[f][i]) * along;
        return in;
    };
    std::array<std::array<float *, 2>, kFilters> combs{};
    bool filterBack = false; // whether any filter that runs hears itself or a later one
    for (int f = 0; f < kFilters; ++f) {
        if (!filterOn[f]) { // when it runs again, it starts from silence, at its inputs as they are then
            if (v.filterHeld[f]) {
                v.filters[f] = {};
                v.filterHeld[f] = false;
                v.filterInputsNow[f][0] = std::numeric_limits<double>::quiet_NaN();
            }
            v.combDirty[f] = {true, true};
            v.filterRing[f] = 0;
            v.filterHeard[f] = 0;
            v.filterLast[f] = {};
            continue;
        }
        v.filterHeld[f] = true;
        combs[f] = {combLine(v, f, 0), combLine(v, f, 1)};
        for (int from = f; from < kFilters; ++from)
            filterBack = filterBack || (filterOn[from] && routed(routeTo.link[from][f], current.link[from][f]));
        FilterInputs inputs = filterInputs(
            p[paramIndex(f, FilterParam::Cutoff)], p[paramIndex(f, FilterParam::KeyTrack)], pitch,
            p[paramIndex(f, FilterParam::Type)], p[paramIndex(f, FilterParam::Slope)],
            p[paramIndex(f, FilterParam::Resonance)], p[paramIndex(f, FilterParam::Drive)],
            p[paramIndex(f, FilterParam::Vowel)], p[paramIndex(f, FilterParam::Morph)],
            p[paramIndex(f, FilterParam::MorphMode)]);
        // The ladder, the diode and the Sallen-Key's self-oscillate at the top of their resonance, which would ring
        // on without end: once the voice's operators have faded (see Voice::tail), it is held below that, so their
        // ringing dies away as a loop's does.
        if (v.tail && selfOscillates(filterTypeOf(inputs[FilterIn::Type])))
            inputs[FilterIn::Resonance] = std::min(inputs[FilterIn::Resonance], kTailResonance);
        // The inputs that move the filter smoothly ramp to the block's over a control block (a shorter block going its
        // share of the way), worked out again every kFilterSubStep samples while they move: taken once a block, an
        // LFO or envelope on the cutoff would step it every control block, a buzz at the control rate's multiples.
        // The type, the slope and the morph's mode switch at once, and a new type takes every input at once.
        FilterInputs &now = v.filterInputsNow[f];
        if (std::isnan(now[0]) || now[FilterIn::Type] != inputs[FilterIn::Type])
            now = inputs;
        filterMoving[f] = false;
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (!kFilterInputSmooth[i])
                now[i] = inputs[i];
            else if (now[i] != inputs[i])
                filterMoving[f] = true;
        }
        filterFrom[f] = now;
        filterTo[f] = inputs;
        if (filterMoving[f]) {
            for (size_t i = 0; i < inputs.size(); ++i) // where the block leaves them
                if (kFilterInputSmooth[i])
                    now[i] = filterShare >= 1.0 ? inputs[i] : now[i] + (inputs[i] - now[i]) * filterShare;
            // An operator moving one of its settings has audioSample() work them out every sample, along the ramp.
            if (audioOn && (audioFilterMask_ >> f & 1u))
                filterMoving[f] = false;
        }
        if (filterMoving[f]) {
            // As it was left: where the last block's ramp ended (the inputs it left, worked out at its last step), or
            // the settings it held still at.
            gainsFrom[f] = v.filterRamped[f] ? v.filterGainsLeft[f]
                                             : filterGains(v.filterSettings[f].get(filterFrom[f], [&] {
                                                   return filterSettingsFrom(filterFrom[f], rate_);
                                               }));
            filterStepped[f] = filterSettingsFrom(filterInputsAt(f, 0), rate_);
            gainsTo[f] = filterGains(filterStepped[f]);
            stepLength[f] = std::min(filterStep, frames);
            filter[f] = &filterStepped[f];
        } else {
            filter[f] = &v.filterSettings[f].get(inputs, [&] { return filterSettingsFrom(inputs, rate_); });
        }
        if (filter[f]->type == FilterType::Comb) // its lines start silent
            for (int c = 0; c < channels; ++c)
                if (v.combDirty[f][c]) {
                    std::fill_n(combs[f][c], combSize_, 0.0f);
                    v.combDirty[f][c] = false;
                }
    }
    // What the filters put out, before their output levels, for the editor's analyzers.
    std::array<double *, kFilters> taps{};
    if (analyzing_)
        for (int f = 0; f < kFilters; ++f)
            taps[f] = filterTaps_[f].data();

    // What each operator hears and where it goes this block: only the routes whose level is set or still ramping. The
    // others stay at 0 all block, and adding 0 times a finite output changes no sum, so leaving them out changes
    // nothing; those that are left keep their order, so the sums round as before. Sends into a filter or shaper
    // that doesn't run go nowhere.
    struct OpRoutes {
        std::array<int, kOps> ops;            // the operators that modulate it, itself among them for feedback
        std::array<int, kFilters> filters;    // the filters that modulate it
        std::array<int, kShapers> shapers;    // the shapers that modulate it
        std::array<int, kFilters> sends;      // the filters it feeds
        std::array<int, kShapers> shaperSends; // the shapers it feeds
        int opCount = 0, filterCount = 0, shaperCount = 0, sendCount = 0, shaperSendCount = 0;
        bool out = false;      // whether it goes straight to the output
        bool phaseOnly = true; // every route into it adds to its phase, as all did before the other modes came
        bool pitched = false;  // some route into it moves its frequency (Linear or Exp FM)
    };
    std::array<OpRoutes, kOps> routes{};
    {
        auto inUse = [](double now, double by) { return now != 0.0 || by != 0.0; };
        for (int i = 0; i < kOps; ++i) {
            if (!opOn[i])
                continue;
            OpRoutes &r = routes[i];
            for (int j = 0; j < kOps; ++j)
                if (inUse(current.index[j][i], routeBy.index[j][i]))
                    r.ops[r.opCount++] = j;
            for (int f = 0; f < kFilters; ++f)
                if (inUse(current.filterIndex[f][i], routeBy.filterIndex[f][i]))
                    r.filters[r.filterCount++] = f;
            for (int sh = 0; sh < kShapers; ++sh)
                if (inUse(current.shaperIndex[sh][i], routeBy.shaperIndex[sh][i]))
                    r.shapers[r.shaperCount++] = sh;
            auto noteMode = [&](RouteMode mode) {
                r.phaseOnly = r.phaseOnly && mode == RouteMode::Phase;
                r.pitched = r.pitched || mode == RouteMode::LinearFm || mode == RouteMode::ExpFm;
            };
            for (int k = 0; k < r.opCount; ++k)
                noteMode(routeModes_[r.ops[k]][i]);
            for (int k = 0; k < r.filterCount; ++k)
                noteMode(routeModes_[kOps + r.filters[k]][i]);
            for (int k = 0; k < r.shaperCount; ++k)
                noteMode(routeModes_[kOps + kFilters + r.shapers[k]][i]);
            for (int f = 0; f < kFilters; ++f)
                if (filterOn[f] && inUse(current.send[i][f], routeBy.send[i][f]))
                    r.sends[r.sendCount++] = f;
            for (int sh = 0; sh < kShapers; ++sh)
                if (shaperOn[sh] && inUse(current.shaperSend[i][sh], routeBy.shaperSend[i][sh]))
                    r.shaperSends[r.shaperSendCount++] = sh;
            r.out = inUse(current.out[i], routeBy.out[i]);
        }
    }

    // Frequency modulation takes an operator up to its frequency times 2^(its Exp depths) plus its Linear ones, with
    // sources at full scale: its table suits that, so it stays below Nyquist there too.
    for (int i = 0; i < kOps; ++i) {
        if (!opOn[i] || noise[i] || !routes[i].pitched)
            continue;
        const double fm = std::max(std::max(fmTarget, v.fmScale), audioOn ? audioFmReach_ : 0.0);
        double lin = 0.0, ex = 0.0;
        auto reach = [&](RouteMode mode, double now, double to) {
            const double depth = std::max(std::abs(now), std::abs(to)) * fm;
            if (mode == RouteMode::LinearFm)
                lin += depth;
            else if (mode == RouteMode::ExpFm)
                ex += depth;
        };
        const OpRoutes &r = routes[i];
        for (int k = 0; k < r.opCount; ++k)
            reach(routeModes_[r.ops[k]][i], current.index[r.ops[k]][i], routeTo.index[r.ops[k]][i]);
        for (int k = 0; k < r.filterCount; ++k)
            reach(routeModes_[kOps + r.filters[k]][i], current.filterIndex[r.filters[k]][i],
                  routeTo.filterIndex[r.filters[k]][i]);
        for (int k = 0; k < r.shaperCount; ++k)
            reach(routeModes_[kOps + kFilters + r.shapers[k]][i], current.shaperIndex[r.shapers[k]][i],
                  routeTo.shaperIndex[r.shapers[k]][i]);
        const double highest = std::min(highestInc[i] * (std::exp2(std::min(ex, 16.0)) + lin), 0.5);
        wave[i] = opShapes[i] == OpWave::Custom ? customWaves_[i].table(highest) : waves_.table(opShapes[i], highest);
    }

    Levels &level = v.levels;
    double blockPeak = 0.0;
    std::array<double, kFilters> filterPeak{};
    const int scopeOp = &v == scopeVoice_ ? scope_.op : -1;
    // The operators something listens to as they play, a single test a sample for the scope and the sources alike.
    const uint32_t tapped = opSourceMask_ | (scopeOp >= 0 ? 1u << scopeOp : 0u);
    const uint32_t fxOps = &v == fxFmVoice_ ? fxFmOps_ : 0u; // those the filter effects' FM hears, from the newest voice
    // Operators moving this voice's filters' cutoffs, sample by sample.
    const bool cutoffFm = opSourceMask_ != 0 && voiceFmCount_ > 0 && startCutoffFm(filter);
    if (audioOn) {
        audioRun_.filter = filter.data();
        audioRun_.filterFrom = filterFrom;
        audioRun_.filterTo = filterTo;
        audioRun_.filterShare = filterShare;
        audioRun_.frames = frames;
        for (int f = 0; f < kFilters && cutoffFm; ++f)
            if (filter[f] == &voiceFmNow_[f])
                audioRun_.fmFilters |= 1u << f;
    }
    for (uint32_t s = 0; s < frames; ++s) {
        v.fmScale += fmStep;
        v.gainL += gainLStep;
        v.gainR += gainRStep;
        v.fade = std::clamp(v.fade + fadeStep, 0.0, 1.0);
        for (ClipCurve &glide : curve) // a drawn curve gliding to new points, along this block
            if (glide.from)
                glide.fade = (s + 1.0) / frames;
        if (ramping) {
            const std::span<double> now = values(level), by = values(step);
            for (int m = 0; m < movingCount; ++m)
                now[moving[m]] += by[moving[m]];
        }
        if (audioOn)
            audioSample(v, s);
        std::array<double, 2> mix{};
        std::array<std::array<double, 2>, kFilters> toFilter{};
        std::array<std::array<double, 2>, kShapers> toShaper{};
        for (int i = 0; i < kOps; ++i) {
            if (!opOn[i])
                continue;
            Operator &op = v.ops[i];
            const OpRoutes &route = routes[i];
            // Operators before i already hold this sample's output, later ones the
            // previous sample's. Feedback averages the last two outputs to stay stable.
            double pm = 0.0;
            // The other modes: Linear and Exp FM scale its frequency by 2^ex + lin, Ring its output by ring.
            double lin = 0.0, ex = 0.0, ring = 1.0;
            if (route.phaseOnly) {
                for (int k = 0; k < route.opCount; ++k) {
                    const int j = route.ops[k];
                    pm += level.index[j][i] * (j == i ? 0.5 * (op.out + op.prevOut) : v.ops[j].out);
                }
                for (int k = 0; k < route.filterCount; ++k)
                    pm += level.filterIndex[route.filters[k]][i] * v.filterOut[route.filters[k]];
                for (int k = 0; k < route.shaperCount; ++k)
                    pm += level.shaperIndex[route.shapers[k]][i] * v.shaperOut[route.shapers[k]];
            } else {
                auto take = [&](RouteMode mode, double depth, double x) {
                    switch (mode) {
                    case RouteMode::LinearFm:
                        lin += depth * x;
                        break;
                    case RouteMode::ExpFm:
                        ex += depth * x;
                        break;
                    case RouteMode::Ring: {
                        const double d = std::min(depth * v.fmScale, 1.0);
                        ring *= 1.0 - d + d * x;
                        break;
                    }
                    default:
                        pm += depth * x;
                    }
                };
                for (int k = 0; k < route.opCount; ++k) {
                    const int j = route.ops[k];
                    take(routeModes_[j][i], level.index[j][i], j == i ? 0.5 * (op.out + op.prevOut) : v.ops[j].out);
                }
                for (int k = 0; k < route.filterCount; ++k) {
                    const int f = route.filters[k];
                    take(routeModes_[kOps + f][i], level.filterIndex[f][i], v.filterOut[f]);
                }
                for (int k = 0; k < route.shaperCount; ++k) {
                    const int sh = route.shapers[k];
                    take(routeModes_[kOps + kFilters + sh][i], level.shaperIndex[sh][i], v.shaperOut[sh]);
                }
            }
            const double offset = pm * v.fmScale;
            // How far its frequency goes this sample, past zero too; 1 while nothing moves it.
            const double factor =
                route.pitched ? (ex != 0.0 ? std::exp2(std::clamp(ex * v.fmScale, -16.0, 16.0)) : 1.0) + lin * v.fmScale
                              : 1.0;

            // Its envelope a sample at a time, here: run a block ahead into a buffer of its own (Envelope::run()), the
            // same levels made the voices a sixth slower with one copy in the plugins' MSVC build.
            const double envLevel = op.env.next(rates[i]);
            // A synced operator's scope follows the note's cycles, which its own restart with.
            const double scopePhase = i != scopeOp ? 0.0 : op.syncOn ? op.syncPhases[0] : op.phases[0];
            double mono = 0.0, l = 0.0, r = 0.0;
            if (warped[i]) {
                // Synced or warped (see Operator): in a function of its own, so the plain loops below are as they were.
                const OpSample out =
                    warpedSample(op, wave[i], offset + level.phase[i], factor, copies[i], copyGains[i],
                                 v.copyWeights[i].data(), route.out || route.sendCount > 0 || route.shaperSendCount > 0,
                                 syncIncStep[i], warpStep[i]);
                mono = out.mono;
                l = out.l;
                r = out.r;
            } else if (noise[i]) {
                mono = nextRandom(v.rng);
                mono = colored[i] ? noiseColor_.process(mono, noiseCoefs[i], op.noise[0]) : mono * whiteGain;
                l = r = mono * noiseMid[i];
                if (noiseSide[i] > 0.0) {
                    double side = nextRandom(v.rng);
                    side = colored[i] ? noiseColor_.process(side, noiseCoefs[i], op.noise[1]) : side * whiteGain;
                    l += side * noiseSide[i];
                    r -= side * noiseSide[i];
                }
            } else if (envLevel == 0.0) {
                // Silent (an envelope at rest, or held or released at nothing): what it puts out is 0 whatever the
                // wave, so its copies only move on as they would have.
                for (int k = 0; k < copies[i]; ++k) {
                    op.phases[k] += op.incNow[k];
                    if (op.phases[k] >= 1.0)
                        op.phases[k] -= 1.0;
                }
            } else {
                // Each copy read and moved on; only an operator heard (out, or into a filter or shaper) sums its
                // copies' sides, one that only modulates others needing its mono sum alone. Plain loops with a branch
                // the predictor learns: a templated lambda per case measured a tenth slower here with one copy.
                const bool sides = route.out || route.sendCount > 0 || route.shaperSendCount > 0;
                if (factor == 1.0) {
                    for (int k = 0; k < copies[i]; ++k) {
                        const double x = WaveTables::read(wave[i], op.phases[k] + offset);
                        mono += x;
                        if (sides) {
                            l += x * copyGains[i]->gainL[k];
                            r += x * copyGains[i]->gainR[k];
                        }
                        op.phases[k] += op.incNow[k];
                        if (op.phases[k] >= 1.0)
                            op.phases[k] -= 1.0;
                    }
                } else {
                    // Frequency modulation: each copy's increment scaled, held within Nyquist either way, so the phase
                    // moves less than a cycle a sample and one wrap brings it back.
                    for (int k = 0; k < copies[i]; ++k) {
                        const double x = WaveTables::read(wave[i], op.phases[k] + offset);
                        mono += x;
                        if (sides) {
                            l += x * copyGains[i]->gainL[k];
                            r += x * copyGains[i]->gainR[k];
                        }
                        op.phases[k] += std::clamp(op.incNow[k] * factor, -0.5, 0.5);
                        if (op.phases[k] >= 1.0)
                            op.phases[k] -= 1.0;
                        else if (op.phases[k] < 0.0)
                            op.phases[k] += 1.0;
                    }
                }
            }
            // The copies' increments move on toward the block's only while one differs from it: a pitch holding still,
            // as most do, leaves them as they are, without a store a copy and a sample (each copy reads its own alone).
            if (incMoving[i] && !noise[i])
                for (int k = 0; k < copies[i]; ++k)
                    op.incNow[k] += incStep[i][k];
            if (ring != 1.0) {
                mono *= ring;
                l *= ring;
                r *= ring;
            }
            op.prevOut = op.out;
            op.out = mono * monoGain[i] * envLevel;
            if (tapped >> i & 1u) {
                if (i == scopeOp) {
                    const double wavePhase = op.syncOn ? scopePhase : scopePhase + level.phase[i]; // where its wave stands
                    scopeTake(wavePhase - std::floor(wavePhase), op.out);
                }
                v.opSum[i] += op.out;
                if (fxOps >> i & 1u)
                    fxOpOut_[i][s / static_cast<uint32_t>(oversampling_)] += static_cast<float>(op.out);
            }
            l *= envLevel * level.panL[i];
            r *= envLevel * level.panR[i];
            if (route.out) {
                mix[0] += l * level.out[i];
                mix[1] += r * level.out[i];
            }
            for (int k = 0; k < route.sendCount; ++k) {
                const int f = route.sends[k];
                toFilter[f][0] += l * level.send[i][f];
                toFilter[f][1] += r * level.send[i][f];
            }
            for (int k = 0; k < route.shaperSendCount; ++k) {
                const int sh = route.shaperSends[k];
                toShaper[sh][0] += l * level.shaperSend[i][sh];
                toShaper[sh][1] += r * level.shaperSend[i][sh];
            }
        }

        for (int sh = 0; sh < kShapers; ++sh) {
            if (!shaperOn[sh])
                continue;
            if (filterShapes) // the filters, from the previous sample, as operators hear them
                for (int f = 0; f < kFilters; ++f)
                    for (int c = 0; c < 2; ++c)
                        toShaper[sh][c] += v.filterLast[f][c] * level.filterShaper[f][sh];
            if (shaperShapes) // the shapers before it from this sample, itself and those after it from the previous
                for (int from = 0; from < kShapers; ++from)
                    for (int c = 0; c < 2; ++c)
                        toShaper[sh][c] += v.shaperLast[from][c] * level.shaperShaper[from][sh];
            std::array<double, 2> y{};
            for (int c = 0; c < channels; ++c) // as the shaper effect runs (see fx::Shaper::run())
                y[c] = v.shapers[sh][c].run(curve[sh], toShaper[sh][c], level.shaperDrive[sh], level.shaperOutput[sh],
                                            level.shaperMix[sh], block[sh], pole, level.shaperBetween[sh]);
            if (!stereo)
                y[1] = y[0];
            v.shaperOut[sh] = 0.5 * (y[0] + y[1]);
            v.shaperLast[sh] = {std::clamp(y[0], -kLoopLimit, kLoopLimit), std::clamp(y[1], -kLoopLimit, kLoopLimit)};
            for (int c = 0; c < 2; ++c) {
                mix[c] += y[c] * level.shaperDirect[sh];
                for (int f = 0; f < kFilters; ++f)
                    toFilter[f][c] += y[c] * level.shaperLink[sh][f];
            }
        }

        for (int f = 0; f < kFilters; ++f) {
            if (!filterOn[f] || !filterMoving[f])
                continue;
            const uint32_t into = s % filterStep;
            const bool stepped = s > 0 && into == 0;
            if (stepped) { // its ramp's next step
                gainsFrom[f] = gainsTo[f];
                filterStepped[f] = filterSettingsFrom(filterInputsAt(f, s), rate_);
                gainsTo[f] = filterGains(filterStepped[f]);
                stepLength[f] = std::min(filterStep, frames - s);
            }
            const double t = static_cast<double>(into + 1) / stepLength[f];
            glideFilterGains(filterStepped[f], gainsFrom[f], gainsTo[f], t);
            // An operator moving its cutoff moves it from these settings (see moveCutoffs()): taken up whole at each
            // step, and what it scales its outputs by gliding in between, as here.
            if (filter[f] == &voiceFmNow_[f]) {
                if (stepped)
                    voiceFmNow_[f] = filterStepped[f];
                else
                    glideFilterGains(voiceFmNow_[f], gainsFrom[f], gainsTo[f], t);
            }
        }
        if (cutoffFm)
            moveCutoffs(v, s);
        for (int f = 0; f < kFilters; ++f) {
            if (!filterOn[f]) {
                v.filterOut[f] = 0.0;
                v.filterLast[f] = {};
                continue;
            }
            if (filterBack) // itself and the filters after it, from the previous sample
                for (int from = f; from < kFilters; ++from)
                    for (int c = 0; c < 2; ++c)
                        toFilter[f][c] += v.filterLast[from][c] * level.link[from][f];
            std::array<double, 2> y{};
            for (int c = 0; c < channels; ++c) { // blended with what goes in by its Mix, as the filter effect is
                const double in = toFilter[f][c];
                y[c] = in + (v.filters[f][c].process(in, *filter[f], combs[f][c], combSize_) - in) * level.filterMix[f];
            }
            if (!stereo)
                y[1] = y[0];
            v.filterOut[f] = 0.5 * (y[0] + y[1]);
            filterPeak[f] = std::max({filterPeak[f], std::abs(y[0]), std::abs(y[1])});
            v.filterLast[f] = {std::clamp(y[0], -kLoopLimit, kLoopLimit), std::clamp(y[1], -kLoopLimit, kLoopLimit)};
            if (taps[f])
                taps[f][s] += v.filterOut[f] * v.fade;
            for (int c = 0; c < 2; ++c) {
                mix[c] += y[c] * level.filterOut[f];
                for (int to = f + 1; to < kFilters; ++to)
                    toFilter[to][c] += y[c] * level.link[f][to];
            }
        }
        blockPeak = std::max({blockPeak, std::abs(mix[0]), std::abs(mix[1])});
        left[s] += mix[0] * v.gainL * v.fade;
        right[s] += mix[1] * v.gainR * v.fade;
    }
    if (audioOn)
        audioRestore();
    for (int i = 0; i < kOps; ++i)
        if (opOn[i] && !noise[i])
            for (int k = 0; k < copies[i]; ++k)
                v.ops[i].incNow[k] = copy[i]->inc[k];
    if (tapped != 0)
        finishOpSources(v, opOn, frames);
    bool filtersHeard = false; // whether a filter heard has put out anything within a comb line
    for (int f = 0; f < kFilters; ++f)
        if (filterOn[f]) {
            v.filterRing[f] = filterPeak[f] > kFilterRingFloor
                                  ? combSize_
                                  : std::max(0, v.filterRing[f] - static_cast<int>(frames));
            v.filterHeard[f] = filterPeak[f] * level.filterOut[f] >= kVoiceQuietFloor
                                   ? combSize_
                                   : std::max(0, v.filterHeard[f] - static_cast<int>(frames));
            filtersHeard = filtersHeard || v.filterHeard[f] > 0;
        }
    for (int f = 0; f < kFilters; ++f) { // where each ramp ended, which the next block's starts from
        v.filterRamped[f] = filterOn[f] && filterMoving[f];
        if (v.filterRamped[f])
            v.filterGainsLeft[f] = gainsTo[f];
    }

    // Once released, the voice ends when every operator that is heard (directly
    // or through a filter or shaper) has faded and the filters have rung out. A stolen
    // one ends when its fade is done.
    bool sounding = false;
    double peak = 0.0;
    for (int i = 0; i < kOps; ++i) {
        double heard = routeTo.out[i];
        for (int f = 0; f < kFilters; ++f)
            heard = std::max(heard, routeTo.send[i][f]);
        for (int sh = 0; sh < kShapers; ++sh)
            heard = std::max(heard, routeTo.shaperSend[i][sh]);
        if (heard <= 0.0)
            continue;
        sounding = sounding || !v.ops[i].env.idle();
        // An operator still rising counts at its attack's top, so a slow attack isn't stolen for being quiet yet.
        peak = std::max(peak, (v.ops[i].env.rising() ? 1.0 : v.ops[i].env.level()) * heard);
    }
    v.peak = peak;
    v.tail = v.released && !sounding;
    if (v.fading && v.fade <= 0.0)
        endVoice(v);
    else if (v.released && !sounding && blockPeak < kVoiceQuietFloor && !filtersHeard) {
        if (modulationReleasing(v, params))
            v.silent = true; // its sound is over, but not its modulation
        else
            endVoice(v);
    }
}

bool Synth::modulationReleasing(const Voice &v, const ParamValues &params) const {
    for (int s = 0; s < kMaxSources; ++s) {
        const SourceType type = sourceType(params[paramIndex(s, SourceParam::Type)]);
        const bool going = type == SourceType::Env ? !v.modEnvs[s].idle()
                                                   : type == SourceType::Curve &&
                                                         leftGoing(v.lfos[s], v.curveReleases[s]);
        if (!going || params[sourceEnabledIndex(s)] < 0.5)
            continue;
        for (int slot = 0; slot < kSlots; ++slot)
            if (slotTarget(params[slotTargetIndex(s, slot)]) >= 0 && params[slotAmountIndex(s, slot)] != 0.0 &&
                params[slotBypassIndex(s, slot)] < 0.5)
                return true;
    }
    return false;
}

} // namespace fmvibe
