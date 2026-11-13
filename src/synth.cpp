#include "synth.h"
#include "unison.h"
#include "warp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

namespace fmvibe {

namespace {

// MPE: channels 2 to 16, numbered from 0, each carry a note of their own (see Synth::setMpe()).
bool memberChannel(int channel) { return channel >= 1 && channel < 16; }

constexpr uint32_t kControlBlock = 32;        // samples between modulation updates
constexpr double kWarpSlew = 250.0;           // how fast an operator's warp amount moves at most, a second
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
}

namespace {

// A voice's ramped levels as one run of numbers, to move them all at once.
template <class Levels>
std::span<double> values(Levels &levels) {
    static_assert(std::is_standard_layout_v<Levels> && sizeof(Levels) % sizeof(double) == 0);
    return {reinterpret_cast<double *>(&levels), sizeof(Levels) / sizeof(double)};
}

} // namespace

float *Synth::combLine(const Voice &v, int filter, int channel) {
    const size_t voice = static_cast<size_t>(&v - voices_.data());
    return &combs_[((voice * kFilters + filter) * 2 + channel) * combSize_];
}

void Synth::prepare(double sampleRate, int capacity, int oversampling) {
    sampleRate_ = sampleRate;
    oversampling_ = oversampling >= 8 ? 8 : oversampling >= 4 ? 4 : oversampling >= 2 ? 2 : 1;
    rate_ = sampleRate * oversampling_;
    bend_ = 0.0;
    globalMod_.fill(0.0);
    sharedStale_ = true;
    changedCount_ = 0;
    capacity_ = std::clamp(capacity, 1, kMaxVoices);
    voices_.assign(static_cast<size_t>(capacity_ + std::clamp(capacity_, kFadeSlots, kMaxFadeSlots)), Voice{});
    active_.clear();
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
    effects_.prepare(sampleRate);
    noiseColor_.prepare(rate_, sampleRate);
    volumeGlide_ = 1.0 - std::exp(-1.0 / (kVolumeGlide * sampleRate));
    ended_.clear();
    // Every note that ends in one block is reported at its end: room for a burst of them far past the voices.
    ended_.reserve(std::max<size_t>(voices_.size() * 2 + 16, kMaxEndedNotes));
    held_.clear();
    held_.reserve(256);
    limit_ = capacity_;
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
    effects_.reset();
    fxPitch_ = 60.0;
    fxTail_.on = false;
    fxLast_ = nullptr;
    volumeGain_ = -1.0; // set at once, the first time round
    outPeak_ = {};
    clipHold_ = 0.0;
    outScope_ = {};
}

void Synth::advanceLfo(Lfo &lfo, double hz, uint32_t frames, uint32_t &rng, bool oneShot, double chaos) const {
    lfo.hz = hz;
    lfo.phase += hz * frames / sampleRate_;
    if (oneShot && lfo.phase >= 1.0) { // stays at the end of its one cycle
        lfo.phase = 1.0;
        lfo.hz = 0.0;
    } else if (lfo.phase >= 1.0) {
        lfo.phase -= std::floor(lfo.phase);
        lfo.from = lfo.held; // Random takes a new value each cycle
        lfo.held = randomStep(lfo.held, chaos, rng);
    }
}

void Synth::noteOn(const NoteRef &note, double velocity) {
    if (note.key < 0 || note.key > 127 || voices_.empty())
        return;
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

    // Glide starts wherever the newest voice is right now, or at the last note.
    Voice *newest = newestVoice();
    const double from = newest ? newest->glide : lastKey_;
    const bool glide = from >= 0.0 && (glideAlways_ || overlapping);
    lastKey_ = note.key;

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
        std::array<Envelope, kMaxSources> modEnvs;
        std::array<Lfo, kMaxSources> lfos;
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
            joined->modEnvs = with->modEnvs;
            joined->lfos = with->lfos;
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
    v.glide = glide ? from : note.key;
    v.fresh = true;
    v.newRandom = true;
    v.auxOut.fill(0.0); // a new note's sources have put out nothing yet
    startFromChannel(v);
    if (joined) { // fading in, since its envelopes may start well above nothing
        for (int op = 0; op < kOps; ++op)
            v.ops[op].env = joined->ops[op];
        v.modEnvs = joined->modEnvs;
        v.lfos = joined->lfos;
        v.keepLfos = true;
        v.fade = 0.0;
        return;
    }
    for (Operator &op : v.ops)
        op.env.start();
    for (Envelope &env : v.modEnvs)
        env.start();
    for (Lfo &lfo : v.lfos) { // the first step and the start phase come in the voice's first block
        lfo.from = lfo.held = nextRandom(v.rng);
        lfo.restart = true;
    }
}

void Synth::noteOff(const NoteRef &match) {
    // Let go of the matching keys.
    for (size_t i = held_.size(); i-- > 0;) {
        if (!matches(match, held_[i].note))
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
                                       [&](const HeldNote &held) { return !matches(match, held.note); });
        if (mono_ && down != held_.rend() && !voicePlaying(down->note)) {
            reportEnded(note);
            moveVoice(*v, *down, !legato_, true);
        }
    }
    // Release what is left, including voices whose keys weren't tracked (e.g.
    // started before a play-mode change); the pedal holds them instead while it is down.
    for (Voice *v : active_)
        if (live(*v) && !v->released && !v->sustained && matches(match, v->note))
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
}

void Synth::allSoundOff(int channel) {
    const NoteRef match = channelMatch(channel);
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
        effects_.flush();
}

void Synth::choke(const NoteRef &match) {
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
    if (!glide)
        v.glide = to.note.key;
    // The new note brings its own host modulation and expressions.
    clearVoiceMods(v);
    v.tuning = 0.0;
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
        for (Envelope &env : v.modEnvs)
            env.start();
        for (Lfo &lfo : v.lfos) { // Random carries on from the value it holds (standing still, takes a new one)
            lfo.phase = 0.0;
            lfo.from = lfo.held;
            lfo.restart = true;
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
    for (Envelope &env : v.modEnvs)
        env.release();
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
    macroKnobs_ = sharedMacroKnobs(get, macroGraph_);
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
            bipolar,
            offset};
    });
    voiceMacroSlotCount_ = 0;
    for (int source = kMaxSources; source < kAllSources; ++source) {
        if (!hasKnob(source))
            continue;
        for (int slot = 0; slot < kSlots; ++slot) {
            const int target = slotTarget(params[slotTargetIndex(source, slot)]);
            if (const int aux = slotAux(get, source, slot); aux >= 0 && runsPerVoice(aux) && target >= 0 &&
                                                             target < kModulatedCount &&
                                                             params[slotBypassIndex(source, slot)] < 0.5)
                voiceMacroSlots_[voiceMacroSlotCount_++] = {source, slot};
        }
    }
}

void Synth::render(float *left, float *right, uint32_t frames, const ParamValues &params, bool sameParams) {
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
        bool sounding = false;
        forEachActive([&](Voice &v) { // in their order, as they always summed; one may end as it renders
            renderBlock(v, mix_[0].data(), mix_[1].data(), n, params);
            sounding = true;
        });
        for (int c = 0; c < 2; ++c) {
            if (sounding)
                decimators_[c].process(mix_[c].data(), (c == 0 ? left : right) + done, static_cast<int>(n));
            else
                decimators_[c].reset();
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
        effects_.process(left + done, right + done, n, fxParams_, tempo_, fxPitch_);
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
                                    slotCurveIndex(source, slot), slotBypassIndex(source, slot)})
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

void Synth::startFxTail(const Voice &v, const ParamValues &params) {
    fxTail_.on = true;
    fxTail_.silent = 0.0;
    fxTail_.lfos = v.lfos;
    fxTail_.auxOut = v.auxOut;
    fxTail_.rng = v.rng;
    // The LFOs' slots are worked out again each block; the other sources' keep what they gave last.
    fxTail_.heldCount = 0;
    const int volume = paramIndex(GlobalParam::Volume);
    for (int m = 0; m < v.slotModCount; ++m) {
        const SlotMod &mod = v.slotMods[m];
        if ((mod.index < kFxBase || mod.index >= kShapeBase) && mod.index != volume)
            continue;
        const int source = mod.slot / kSlots;
        if (source < kMaxSources && runsLfo(sourceType(params[paramIndex(source, SourceParam::Type)])))
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
        // A free or anchored LFO goes on along its shared clock, as a voice's would; another from where it stood.
        Lfo &lfo = fxTail_.lfos[s];
        const bool free = lfoShared(get, s);
        if (free)
            lfo = freeLfos_[s];
        const double value = lfoOutput(get, params, s, lfo);
        const bool bipolar = sourceBipolar(get, s);
        fxTail_.auxOut[s] = 0.5 * (value + 1.0);
        const double swing = bipolar ? 0.5 * value : 0.5 * (value + 1.0);
        for (int k = 0; k < sharedSlotCount_[s]; ++k) {
            const SharedSlot &slot = sharedSlots_[s][k];
            if ((slot.target < kFxBase || slot.target >= kShapeBase) && slot.target != volume)
                continue;
            const double amount = hostValue(params, slotAmountIndex(s, slot.slot));
            if (amount == 0.0)
                continue;
            const double scale = slot.voiceAux >= 0 ? slot.scale * fxTail_.auxOut[slot.voiceAux] : slot.scale;
            fxTailMods_[count++] = {slot.target, slotCurveValue(swing, slot.curve, bipolar) * amount * scale * slot.range,
                                    s * kSlots + slot.slot};
            fxTail_.lfoMask |= 1u << s;
        }
        if (!free)
            advanceLfo(lfo, lfoHz(get, s, tempo_), frames, fxTail_.rng,
                       lfoOneShot(get, s), get(paramIndex(s, SourceParam::RandomChaos)));
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
        if (!playing_ || !lfoAnchored(shared, s)) {
            if (frames > 0)
                advanceLfo(lfo, lfoHz(shared, s, tempo_), frames, freeRng_, false, chaos);
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
        const double at = cycles + shared(paramIndex(s, SourceParam::Phase));
        lfo.hz = lfoHz(shared, s, tempo_);
        lfo.phase = at - std::floor(at);
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
        bend_ + std::clamp(bendSet_ + globalMod_[pitchBend], table_[pitchBend].min, table_[pitchBend].max), -1.0, 1.0));
    out.clipHold = static_cast<float>(clipHold_);
    if (analyzing_)
        outScope_.blend.show(outScope_.frame, kOutScopeBins, out.outScope);
    for (const HeldNote &held : held_)
        out.keys[held.note.key / 32] |= 1u << (held.note.key % 32);
    for (int slot = 0; slot < kFxSlots; ++slot) { // effects keep working on echoes after the voices end
        for (int band = 0; band < kMcBands; ++band) {
            out.fxReduction[slot][band] = effects_.reduction(slot, band);
            out.fxBandLevel[slot][band] = effects_.bandLevel(slot, band);
            out.fxBandGain[slot][band] = effects_.bandGain(slot, band);
        }
        for (int band = 0; band < kSpectrumBands; ++band)
            out.fxSpectrum[slot][band] = effects_.spectrum(slot, band);
        out.fxPhases[slot] = effects_.sweepPhase(slot);
    }
    for (int port = 0; port < kFxPorts; ++port)
        out.fxPortLevels[port] = effects_.portLevel(port);
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
        if (std::find(macroTargets_.begin(), macroTargets_.begin() + i, macroTargets_[i]) == macroTargets_.begin() + i)
            out.modOffset[macroTargets_[i]] += static_cast<float>(macroMod_[macroTargets_[i]]);
    for (int s = 0; s < kMaxSources; ++s) {
        out.sources[s] = static_cast<float>(newest->sourceValues[s]);
        out.lfoPhases[s] = static_cast<float>(newest->lfos[s].phase);
        out.lfoRates[s] = static_cast<float>(newest->lfos[s].hz);
        out.lfoFrom[s] = static_cast<float>(newest->lfos[s].from);
        out.lfoTo[s] = static_cast<float>(newest->lfos[s].held);
        out.envPos[s] = static_cast<float>(newest->modEnvs[s].position(newest->modEnvRates[s].last()));
    }
    for (int i = 0; i < kOps; ++i) {
        out.opLevels[i] = static_cast<float>(newest->ops[i].env.level());
        out.opEnvLevels[i] = static_cast<float>(newest->ops[i].env.level());
        out.opEnvPos[i] = static_cast<float>(newest->ops[i].env.position(newest->ops[i].rates.last()));
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

void Synth::reportEnded(const NoteRef &note) {
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
    for (int source = 0; source < kAllSources; ++source) {
        int count = 0;
        for (int s = 0; s < kSlots; ++s) {
            const int target = slotTarget(params[slotTargetIndex(source, s)]);
            if (target < 0 || params[slotBypassIndex(source, s)] >= 0.5)
                continue;
            SharedSlot &slot = sharedSlots_[source][count++];
            slot.slot = s;
            slot.target = target;
            slot.curve = hostValue(params, slotCurveIndex(source, s));
            slot.scale = hostValue(params, slotScaleIndex(source, s));
            slot.voiceAux = -1;
            if (const int aux = slotAux(get, source, s); aux >= 0) {
                if (runsPerVoice(aux) || voiceMovedMacro(aux)) // a macro the voice's sources move, as the voice has it
                    slot.voiceAux = aux;
                else
                    slot.scale *= macroKnobs_[knobOf(aux)];
            }
            slot.range = table_[target].max - table_[target].min;
            slot.depth = hostValue(params, slotAmountIndex(source, s));
        }
        sharedSlotCount_[source] = count;
    }
    sharedSlotsReady_ = true;
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
        UnisonGains c{};
        for (int k = 0; k < copies; ++k) {
            const double copyAngle = (at.pan[k] * spread + 1.0) * std::numbers::pi / 4.0;
            c.gainL[k] = std::numbers::sqrt2 * std::cos(copyAngle) * monoGain * at.weight[k];
            c.gainR[k] = std::numbers::sqrt2 * std::sin(copyAngle) * monoGain * at.weight[k];
        }
        weights = at.weight;
        return c;
    });
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::array<double, kMaxUnison> Synth::unisonDetunes(int index, int copies, const UnisonModes &modes) {
    return unisonSpread(copies, modes.curve, modes.pan, modes.blend, modes.seed, index).detune;
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
            if (i >= kVoiceParamCount)
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
            scale *= runsPerVoice(aux) || isMacro(aux) ? v.auxOut[aux] : macroKnobs_[kMacros];
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
    const double bend = std::clamp(bend_ + p[paramIndex(GlobalParam::PitchBend)], -1.0, 1.0);
    const double memberBend =
        mpe_ && memberChannel(v.note.channel) ? channelBend_[v.note.channel] * mpeBendRange_ : 0.0;
    auto pitchNow = [&] {
        return v.glide + v.tuning + memberBend + bend * p[paramIndex(GlobalParam::BendRange)] +
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
        return slot.voiceAux >= 0 ? slot.scale * v.auxOut[slot.voiceAux] : slot.scale;
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
            Lfo &lfo = v.lfos[s];
            // A free LFO takes the shared clock, which render() moves on; switched
            // back to retrigger, it carries on from there. A one-shot is never free.
            // So does an anchored one, which render() moves to where the transport stands.
            const bool oneShot = lfoOneShot(get, s);
            const bool free = lfoShared(get, s);
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
            const double value = lfoOutput(get, params, s, lfo);
            const bool lfoUnipolar = !sourceBipolar(get, s);
            collectSlots(s, lfoUnipolar ? 0.5 * (value + 1.0) : value, 0.5 * (value + 1.0), 0.5 * value, !lfoUnipolar);
            if (!free)
                advanceLfo(lfo, lfoHz(get, s, tempo_), hostFrames, v.rng, oneShot, chaos);
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
        const EnvRates &rates = v.modEnvRates[s].get(times, [&] {
            return envRates(times[0], times[1], times[2], times[3], times[4], times[5], times[6], times[7],
                            sampleRate_);
        });
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
                                      slot.depth * (slot.aux >= 0 ? voiceKnob(slot.aux) : 1.0) -
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
        v.keepLfos = false;
        for (int i = 0; i < kOps; ++i) {
            const double start = p[paramIndex(i, OpParam::Phase)];
            const double amount = p[paramIndex(i, OpParam::PhaseRandom)];
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

    // Glide: the pitch closes 99 % of the gap to the note in the glide time.
    const double glideTime = p[paramIndex(GlobalParam::Glide)];
    if (glideTime <= 0.0 || std::abs(v.glide - v.note.key) < 1e-3)
        v.glide = v.note.key;
    else
        v.glide = v.note.key + (v.glide - v.note.key) *
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
            modulated += v.slotMods[k].index == target ? v.slotMods[k].offset : 0.0;
        set(target, inRange(target, modulated));
    }
    if (v.slotModCount > noteModsStart)
        pitch = pitchNow();

    // A released voice whose sound is over runs on only while a mod envelope that modulates something releases,
    // so what it modulates (an effect's knob, which follows the newest voice, and what the editor shows) follows
    // the release down rather than jumping back; it renders nothing meanwhile, and ends once they have.
    if (v.silent) {
        v.peak = 0.0;
        if (v.fading || !modEnvReleasing(v, params))
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

        const std::array<double, 8> times{
            p[paramIndex(i, OpParam::Attack)],      p[paramIndex(i, OpParam::Hold)],
            p[paramIndex(i, OpParam::Decay)],       p[paramIndex(i, OpParam::Sustain)],
            p[paramIndex(i, OpParam::Release)],     p[paramIndex(i, OpParam::AttackCurve)],
            p[paramIndex(i, OpParam::DecayCurve)], p[paramIndex(i, OpParam::ReleaseCurve)]};
        rates[i] = op.rates.get(times, [&] {
            return envRates(times[0], times[1], times[2], times[3], times[4], times[5], times[6], times[7], rate_);
        });
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
        // The Random modes draw from the note: a seed of its own, the same every block.
        const UnisonModes modes{
            static_cast<UniCurve>(
                stepped(p[paramIndex(i, OpParam::UnisonCurve)], static_cast<int>(UniCurve::Count) - 1)),
            static_cast<UniPan>(stepped(p[paramIndex(i, OpParam::UnisonPan)], static_cast<int>(UniPan::Count) - 1)),
            std::clamp(p[paramIndex(i, OpParam::UnisonBlend)], 0.0, 1.0), static_cast<uint32_t>(v.order)};
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
        warped[i] = op.syncOn || op.warp != OpWarp::Off || op.carrying || (modes.blend > 0.0 && copies[i] > 1);

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
            {p[fields.drive], p[fields.mode], p[fields.output], p[fields.link], p[fields.mix], p[fields.symmetric]},
            [&] { return shaperSettings([&p](int index) { return p[index]; }, fields); });
        target.shaperDrive[sh] = settings.drive;
        target.shaperOutput[sh] = settings.output;
        target.shaperMix[sh] = settings.mix;
        curve[sh] = shaperTables_[sh].curve(settings.mode, settings.symmetric);
        block[sh] = clipNeedsBlocker(settings.mode);
    }

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
            shaperUsed[sh] = shaperUsed[sh] || routed(target.shaperIndex[sh][to], current.shaperIndex[sh][to]);
        for (int f = 0; f < kFilters; ++f)
            shaperUsed[sh] = shaperUsed[sh] || routed(target.shaperLink[sh][f], current.shaperLink[sh][f]);
        shaperUsed[sh] =
            shaperEnabled[sh] && (shaperUsed[sh] || routed(target.shaperDirect[sh], current.shaperDirect[sh]));
    }
    // A shaper feeding another one that is used is used too; one feeding only itself isn't.
    for (int pass = 0; pass < kShapers; ++pass)
        for (int sh = 0; sh < kShapers; ++sh)
            for (int to = 0; to < kShapers; ++to)
                shaperUsed[sh] = shaperUsed[sh] || (shaperEnabled[sh] && to != sh && shaperUsed[to] &&
                                                    routed(target.shaperShaper[sh][to], current.shaperShaper[sh][to]));
    // What the operators feed, then what that feeds in turn: filters and shapers can feed each other, round in a
    // loop too, so it takes a few passes for what runs to settle.
    std::array<bool, kShapers> shaperOn{};
    std::array<bool, kFilters> filterOn{};
    for (int f = 0; f < kFilters; ++f) // still ringing after its inputs stopped
        filterOn[f] = filterEnabled[f] && v.filterRing[f] > 0;
    for (int i = 0; i < kOps; ++i) {
        for (int f = 0; f < kFilters; ++f)
            filterOn[f] = filterOn[f] || (filterEnabled[f] && routed(target.send[i][f], current.send[i][f]));
        for (int sh = 0; sh < kShapers; ++sh)
            shaperOn[sh] =
                shaperOn[sh] || (shaperUsed[sh] && routed(target.shaperSend[i][sh], current.shaperSend[i][sh]));
    }
    for (int pass = 0; pass < kFilters + kShapers; ++pass) {
        for (int f = 0; f < kFilters; ++f) {
            for (int from = 0; from < kFilters; ++from)
                filterOn[f] = filterOn[f] || (from != f && filterEnabled[f] && filterOn[from] &&
                                              routed(target.link[from][f], current.link[from][f]));
            for (int sh = 0; sh < kShapers; ++sh)
                filterOn[f] = filterOn[f] || (filterEnabled[f] && shaperOn[sh] &&
                                              routed(target.shaperLink[sh][f], current.shaperLink[sh][f]));
        }
        for (int sh = 0; sh < kShapers; ++sh) {
            for (int f = 0; f < kFilters; ++f) {
                const bool shapes =
                    shaperUsed[sh] && filterOn[f] && routed(target.filterShaper[f][sh], current.filterShaper[f][sh]);
                shaperOn[sh] = shaperOn[sh] || shapes;
                filterShapes = filterShapes || shapes;
            }
            for (int from = 0; from < kShapers; ++from)
                shaperOn[sh] = shaperOn[sh] || (from != sh && shaperUsed[sh] && shaperOn[from] &&
                                                routed(target.shaperShaper[from][sh], current.shaperShaper[from][sh]));
        }
    }
    bool shaperShapes = false; // whether any shaper that runs hears one (itself among them)
    for (int sh = 0; sh < kShapers; ++sh) {
        if (!shaperOn[sh]) {
            v.shapers[sh] = {};
            v.shaperOut[sh] = 0.0;
            v.shaperLast[sh] = {};
            continue;
        }
        for (int from = 0; from < kShapers; ++from)
            shaperShapes = shaperShapes ||
                           (shaperOn[from] && routed(target.shaperShaper[from][sh], current.shaperShaper[from][sh]));
    }
    const double pole = clipBlockerPole(rate_); // the shapers' DC blockers
    // The settings of the filters that run, and their comb lines.
    std::array<const FilterSettings *, kFilters> filter{};
    std::array<std::array<float *, 2>, kFilters> combs{};
    bool filterBack = false; // whether any filter that runs hears itself or a later one
    for (int f = 0; f < kFilters; ++f) {
        if (!filterOn[f]) { // when it runs again, it starts from silence
            v.filters[f] = {};
            v.combDirty[f] = {true, true};
            v.filterRing[f] = 0;
            v.filterHeard[f] = 0;
            v.filterLast[f] = {};
            continue;
        }
        combs[f] = {combLine(v, f, 0), combLine(v, f, 1)};
        for (int from = f; from < kFilters; ++from)
            filterBack = filterBack || (filterOn[from] && routed(target.link[from][f], current.link[from][f]));
        // Without key tracking the pitch moves nothing (2 to the power of 0 is exactly 1): left out, so a pitch that
        // moves doesn't have the settings worked out again.
        const double keyTrack = p[paramIndex(f, FilterParam::KeyTrack)];
        const std::array<double, 7> inputs{
            p[paramIndex(f, FilterParam::Cutoff)], keyTrack, keyTrack != 0.0 ? pitch : 0.0,
            p[paramIndex(f, FilterParam::Type)],   p[paramIndex(f, FilterParam::Slope)],
            p[paramIndex(f, FilterParam::Resonance)], p[paramIndex(f, FilterParam::Drive)]};
        filter[f] = &v.filterSettings[f].get(inputs, [&] {
            const double cutoffHz = cutoffFromNorm(inputs[0]) * std::exp2((pitch - 60.0) / 12.0 * inputs[1]);
            const auto type = static_cast<FilterType>(stepped(inputs[3], static_cast<int>(FilterType::Count) - 1));
            return filterSettings(type, filterSlope(inputs[4]), cutoffHz, inputs[5], inputs[6], rate_);
        });
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
                if (inUse(current.index[j][i], step.index[j][i]))
                    r.ops[r.opCount++] = j;
            for (int f = 0; f < kFilters; ++f)
                if (inUse(current.filterIndex[f][i], step.filterIndex[f][i]))
                    r.filters[r.filterCount++] = f;
            for (int sh = 0; sh < kShapers; ++sh)
                if (inUse(current.shaperIndex[sh][i], step.shaperIndex[sh][i]))
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
                if (filterOn[f] && inUse(current.send[i][f], step.send[i][f]))
                    r.sends[r.sendCount++] = f;
            for (int sh = 0; sh < kShapers; ++sh)
                if (shaperOn[sh] && inUse(current.shaperSend[i][sh], step.shaperSend[i][sh]))
                    r.shaperSends[r.shaperSendCount++] = sh;
            r.out = inUse(current.out[i], step.out[i]);
        }
    }

    // Frequency modulation takes an operator up to its frequency times 2^(its Exp depths) plus its Linear ones, with
    // sources at full scale: its table suits that, so it stays below Nyquist there too.
    for (int i = 0; i < kOps; ++i) {
        if (!opOn[i] || noise[i] || !routes[i].pitched)
            continue;
        const double fm = std::max(fmTarget, v.fmScale);
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
            reach(routeModes_[r.ops[k]][i], current.index[r.ops[k]][i], target.index[r.ops[k]][i]);
        for (int k = 0; k < r.filterCount; ++k)
            reach(routeModes_[kOps + r.filters[k]][i], current.filterIndex[r.filters[k]][i],
                  target.filterIndex[r.filters[k]][i]);
        for (int k = 0; k < r.shaperCount; ++k)
            reach(routeModes_[kOps + kFilters + r.shapers[k]][i], current.shaperIndex[r.shapers[k]][i],
                  target.shaperIndex[r.shapers[k]][i]);
        const double highest = std::min(highestInc[i] * (std::exp2(std::min(ex, 16.0)) + lin), 0.5);
        wave[i] = opShapes[i] == OpWave::Custom ? customWaves_[i].table(highest) : waves_.table(opShapes[i], highest);
    }

    Levels &level = v.levels;
    double blockPeak = 0.0;
    std::array<double, kFilters> filterPeak{};
    const int scopeOp = &v == scopeVoice_ ? scope_.op : -1;
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
                    warpedSample(op, wave[i], offset, factor, copies[i], copyGains[i], v.copyWeights[i].data(),
                                 route.out || route.sendCount > 0 || route.shaperSendCount > 0, syncIncStep[i],
                                 warpStep[i]);
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
            if (i == scopeOp)
                scopeTake(scopePhase, op.out);
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
            for (int c = 0; c < channels; ++c) // as the shaper effect runs (see Effects::runShaper())
                y[c] = v.shapers[sh][c].run(curve[sh], toShaper[sh][c], level.shaperDrive[sh], level.shaperOutput[sh],
                                            level.shaperMix[sh], block[sh], pole);
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
    for (int i = 0; i < kOps; ++i)
        if (opOn[i] && !noise[i])
            for (int k = 0; k < copies[i]; ++k)
                v.ops[i].incNow[k] = copy[i]->inc[k];
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

    // Once released, the voice ends when every operator that is heard (directly
    // or through a filter or shaper) has faded and the filters have rung out. A stolen
    // one ends when its fade is done.
    bool sounding = false;
    double peak = 0.0;
    for (int i = 0; i < kOps; ++i) {
        double heard = target.out[i];
        for (int f = 0; f < kFilters; ++f)
            heard = std::max(heard, target.send[i][f]);
        for (int sh = 0; sh < kShapers; ++sh)
            heard = std::max(heard, target.shaperSend[i][sh]);
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
        if (modEnvReleasing(v, params))
            v.silent = true; // its sound is over, but not its modulation
        else
            endVoice(v);
    }
}

bool Synth::modEnvReleasing(const Voice &v, const ParamValues &params) const {
    for (int s = 0; s < kMaxSources; ++s) {
        if (v.modEnvs[s].idle() || sourceType(params[paramIndex(s, SourceParam::Type)]) != SourceType::Env ||
            params[sourceEnabledIndex(s)] < 0.5)
            continue;
        for (int slot = 0; slot < kSlots; ++slot)
            if (slotTarget(params[slotTargetIndex(s, slot)]) >= 0 && params[slotAmountIndex(s, slot)] != 0.0 &&
                params[slotBypassIndex(s, slot)] < 0.5)
                return true;
    }
    return false;
}

} // namespace fmvibe
