#include "synth.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <tuple>
#include <type_traits>

namespace fmvibe {

namespace {

constexpr uint32_t kControlBlock = 32;        // samples between modulation updates
constexpr double kMaxIndex = 8.0;             // radians of phase modulation at a 100 % matrix amount
constexpr double kMaxFeedback = std::numbers::pi;
constexpr int kFadeSlots = 4;        // extra voices for stolen ones to fade out in
constexpr double kStealFade = 0.003; // seconds a stolen voice takes to fade out
constexpr double kVolumeGlide = 0.005; // seconds Volume takes to glide most of the way to a new level
// How loud each voice goes into the effects, -12 dB: Volume comes after them, so drive and thresholds don't
// depend on it.
constexpr double kVoiceLevel = 0.25118864315095801;
// How far the shapers hear a filter's output go, +18 dB. A filter and a shaper feeding each other grow without end
// when part of the shaper's input goes through unshaped (Mix below 100 %) and a resonant filter lifts it past
// unity; held here, the loop stays finite.
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

// Cycles per second of the LFO in place `source`, reading its params through `get`.
template <class Get>
double lfoHz(const Get &get, int source, double tempo) {
    return get(paramIndex(source, SourceParam::Sync)) >= 0.5
               ? 1.0 / divisionSeconds(get(paramIndex(source, SourceParam::Division)), tempo)
               : rateFromNorm(get(paramIndex(source, SourceParam::Rate)));
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

Synth::Synth() : table_(paramTable()), waves_(waveTables()) {}

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
    capacity_ = std::clamp(capacity, 1, kMaxVoices);
    voices_.assign(static_cast<size_t>(capacity_ + kFadeSlots), Voice{});
    combSize_ = combLength(rate_);
    combs_.assign(voices_.size() * kFilters * 2 * combSize_, 0.0f);
    for (std::vector<double> &mix : mix_)
        mix.assign(static_cast<size_t>(kControlBlock) * oversampling_, 0.0);
    for (Decimator &d : decimators_)
        d.prepare(sampleRate, oversampling_);
    for (int f = 0; f < kFilters; ++f) {
        filterTaps_[f].assign(static_cast<size_t>(kControlBlock) * oversampling_, 0.0);
        filterDecimators_[f].prepare(sampleRate, oversampling_);
        Analyzer::prepare(filterAnalyzed_[f]);
    }
    analyzer_.prepare(sampleRate);
    effects_.prepare(sampleRate);
    noiseColor_.prepare(rate_, sampleRate);
    volumeGlide_ = 1.0 - std::exp(-1.0 / (kVolumeGlide * sampleRate));
    ended_.clear();
    ended_.reserve(voices_.size() * 2 + 16);
    held_.clear();
    held_.reserve(256);
    limit_ = capacity_;
    reset();
}

void Synth::reset() {
    for (Voice &v : voices_)
        v.active = v.fading = false;
    ended_.clear();
    held_.clear();
    lastKey_ = -1.0;
    freeLfos_ = {};
    for (Decimator &d : decimators_)
        d.reset();
    for (int f = 0; f < kFilters; ++f) {
        filterDecimators_[f].reset();
        filterAnalyzed_[f].clear();
    }
    effects_.reset();
    fxPitch_ = 60.0;
    volumeGain_ = -1.0; // set at once, the first time round
    outPeak_ = {};
    clipHold_ = 0.0;
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
    const bool overlapping = !held_.empty(); // another key is still down: played legato
    if (held_.size() == held_.capacity())
        held_.erase(held_.begin()); // forget the oldest key rather than allocate
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

    Voice &v = allocateVoice();
    v = Voice{};
    std::fill_n(combLine(v, 0, 0), static_cast<size_t>(kFilters) * 2 * combSize_, 0.0f);
    v.active = true;
    v.note = note;
    v.velocity = played.velocity;
    v.order = noteCounter_++;
    v.rng = static_cast<uint32_t>(v.order * 2654435761u) | 1u;
    v.glide = glide ? from : note.key;
    v.fresh = true;
    for (Operator &op : v.ops)
        op.env.start();
    for (Envelope &env : v.modEnvs)
        env.start();
    for (Lfo &lfo : v.lfos) // the first step comes in the voice's first block, with its chaos
        lfo.from = lfo.held = nextRandom(v.rng);
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
        if (mono_ && !held_.empty()) {
            // Back to the most recent key still down, as a legato move.
            reportEnded(note);
            moveVoice(*v, held_.back(), !legato_, true);
        }
    }
    // Release what is left, including voices whose keys weren't tracked (e.g.
    // started before a play-mode change).
    for (Voice &v : voices_)
        if (live(v) && !v.released && matches(match, v.note))
            releaseVoice(v);
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
    for (Voice &v : voices_)
        if (live(v) && matches(match, v.note))
            endVoice(v);
}

void Synth::moveVoice(Voice &v, const HeldNote &to, bool retrigger, bool glide) {
    // A voice still releasing its note, whose key is already up, takes the new one: the old note is over. (One
    // whose key is still down reports its end when that key is let go.)
    if (v.released)
        reportEnded(v.note);
    v.note = to.note;
    v.velocity = to.velocity;
    v.released = false;
    v.silent = false;
    v.tail = false;
    v.order = noteCounter_++;
    if (!glide)
        v.glide = to.note.key;
    // The new note brings its own host modulation and expressions.
    v.mod.fill(0.0);
    v.tuning = 0.0;
    v.exprGain = 1.0;
    v.exprPan = 0.0;
    if (retrigger) {
        // Envelopes restart from their current level, so there's no click.
        for (Operator &op : v.ops)
            op.env.start();
        for (Envelope &env : v.modEnvs)
            env.start();
        for (Lfo &lfo : v.lfos) { // Random carries on from the value it holds
            lfo.phase = 0.0;
            lfo.from = lfo.held;
        }
    }
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
    for (Voice &v : voices_)
        if (live(v) && (!newest || v.order > newest->order))
            newest = &v;
    return newest;
}

Synth::Voice *Synth::voicePlaying(const NoteRef &note) {
    for (Voice &v : voices_)
        if (live(v) && !v.released && v.note == note)
            return &v;
    return nullptr;
}

bool Synth::voiceHas(const NoteRef &note) const {
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice &v) { return live(v) && v.note == note; });
}

void Synth::noteExpression(const NoteRef &match, int32_t expression, double value) {
    if (!std::isfinite(value))
        return; // not a number, it would spread through the voice's levels and phases, and stay there
    for (Voice &v : voices_) {
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
        }
    }
}

void Synth::paramMod(int index, const NoteRef &match, double amount) {
    if (index < 0 || index >= kParamCount || !(table_[index].flags & CLAP_PARAM_IS_MODULATABLE) ||
        !std::isfinite(amount))
        return;
    if (match == NoteRef{-1, -1, -1, -1}) {
        globalMod_[index] = amount;
        return;
    }
    if (index >= kVoiceParamCount)
        return; // the effects, macros and the rest are shared by all voices
    for (Voice &v : voices_)
        if (live(v) && matches(match, v.note))
            v.mod[index] = amount;
}

void Synth::applyMacros(const ParamValues &params) {
    for (int i = 0; i < macroTargetCount_; ++i)
        macroMod_[macroTargets_[i]] = 0.0;
    macroTargetCount_ = 0;
    forEachMacroMod([this, &params](int i) { return hostValue(params, i); }, [this](int target, double offset) {
        macroMod_[target] += offset;
        macroTargets_[macroTargetCount_++] = target;
    });
}

void Synth::render(float *left, float *right, uint32_t frames, const ParamValues &params) {
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    applyMacros(params);
    for (int op = 0; op < kOps; ++op) { // the Custom waves follow their partials, built again when they change
        std::array<double, 2 * kPartials> partials{}; // those past the count stay silent
        const int count = partialCount(params[partialCountIndex(op)]);
        for (int k = 0; k < count; ++k) {
            partials[2 * k] = params[partialLevelIndex(op, k)];
            partials[2 * k + 1] = params[partialPhaseIndex(op, k)];
        }
        customWaves_[op].update(partials.data());
    }
    for (int sh = 0; sh < kShapers; ++sh) { // and the shapers' drawn curves their points
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        if (clipMode(params[fields.mode]) == ClipMode::Custom)
            shaperTables_[sh].update([&params](int index) { return params[index]; }, fields.curve);
    }
    for (uint32_t done = 0; done < frames; done += kControlBlock) {
        const uint32_t n = std::min(kControlBlock, frames - done);
        // The voices add up at their own rate, which comes down to the host's
        // before the effects. With no voice sounding, the way down starts from
        // silence again: voices end only once they are quiet, or when choked,
        // which should be silent at once.
        for (std::vector<double> &mix : mix_)
            std::fill_n(mix.begin(), n * oversampling_, 0.0);
        if (analyzing_)
            for (std::vector<double> &tap : filterTaps_)
                std::fill_n(tap.begin(), n * oversampling_, 0.0);
        bool sounding = false;
        for (Voice &v : voices_) {
            if (v.active) {
                renderBlock(v, mix_[0].data(), mix_[1].data(), n, params);
                sounding = true;
            }
        }
        for (int c = 0; c < 2; ++c) {
            if (sounding)
                decimators_[c].process(mix_[c].data(), (c == 0 ? left : right) + done, static_cast<int>(n));
            else
                decimators_[c].reset();
        }
        if (analyzing_)
            analyzeFilters(n, sounding, params);

        // The effects are shared by all voices, so LFOs and envelopes reach them
        // through the newest voice, like the editor shows; a filter's key tracking follows its pitch.
        const Voice *newest = newestVoice();
        for (int i = kFxBase; i < kShapeBase; ++i)
            fxParams_[i] = params[i] + globalMod_[i] + macroMod_[i];
        fxParams_[paramIndex(GlobalParam::FxOrder)] = params[paramIndex(GlobalParam::FxOrder)];
        std::copy(params.begin() + kClipCurveBase, params.end(), fxParams_.begin() + kClipCurveBase); // drawn, as set
        if (newest) {
            for (int m = 0; m < newest->slotModCount; ++m)
                if (newest->slotMods[m].index >= kFxBase)
                    fxParams_[newest->slotMods[m].index] += newest->slotMods[m].offset;
            fxPitch_ = newest->pitch;
        }

        // So are the free LFOs' clocks, which run even while no voice listens.
        auto shared = [&](int i) {
            double value = params[i] + globalMod_[i] + macroMod_[i];
            if (newest) {
                value += newest->mod[i];
                for (int m = 0; m < newest->slotModCount; ++m)
                    if (newest->slotMods[m].index == i)
                        value += newest->slotMods[m].offset;
            }
            return std::clamp(value, table_[i].min, table_[i].max);
        };
        for (int s = 0; s < kMaxSources; ++s)
            if (sourceType(params[paramIndex(s, SourceParam::Type)]) == SourceType::Lfo)
                advanceLfo(freeLfos_[s], lfoHz(shared, s, tempo_), n, freeRng_, false,
                           shared(paramIndex(s, SourceParam::RandomChaos)));
        for (int i = kFxBase; i < kShapeBase; ++i)
            fxParams_[i] = std::clamp(fxParams_[i], table_[i].min, table_[i].max);
        effects_.process(left + done, right + done, n, fxParams_, tempo_, fxPitch_);

        // Volume, after the effects: what is set, host and macro modulation, and the newest voice's LFOs and
        // envelopes, as the effects take theirs. It glides to a new level sample by sample, however the block is
        // split.
        const int volume = paramIndex(GlobalParam::Volume);
        double db = params[volume] + globalMod_[volume] + macroMod_[volume];
        if (newest)
            for (int m = 0; m < newest->slotModCount; ++m)
                if (newest->slotMods[m].index == volume)
                    db += newest->slotMods[m].offset;
        const double gain = std::pow(10.0, std::clamp(db, table_[volume].min, table_[volume].max) / 20.0);
        if (volumeGain_ < 0.0)
            volumeGain_ = gain;
        for (uint32_t s = done; s < done + n; ++s) {
            volumeGain_ += (gain - volumeGain_) * volumeGlide_;
            left[s] = static_cast<float>(left[s] * volumeGain_);
            right[s] = static_cast<float>(right[s] * volumeGain_);
        }
    }
    meterOutput(left, right, frames);
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
    return std::any_of(voices_.begin(), voices_.end(), [&](const Voice &v) {
        return live(v) && v.note.port == note.port && v.note.channel == note.channel && v.note.key == note.key &&
               (note.noteId == -1 || v.note.noteId == -1 || v.note.noteId == note.noteId);
    });
}

int Synth::activeVoiceCount() const {
    return static_cast<int>(std::count_if(voices_.begin(), voices_.end(), [](const Voice &v) { return v.active; }));
}

void Synth::telemetry(Telemetry &out) const {
    out = Telemetry{};
    out.tempo = static_cast<float>(tempo_);
    out.fxPitch = static_cast<float>(fxPitch_);
    out.outPeak = {static_cast<float>(outPeak_[0]), static_cast<float>(outPeak_[1])};
    out.clipHold = static_cast<float>(clipHold_);
    for (const HeldNote &held : held_)
        out.keys[held.note.key / 32] |= 1u << (held.note.key % 32);
    for (int slot = 0; slot < kFxSlots; ++slot) { // effects keep working on echoes after the voices end
        for (int band = 0; band < kMcBands; ++band)
            out.fxReduction[slot][band] = effects_.reduction(slot, band);
        for (int band = 0; band < kSpectrumBands; ++band)
            out.fxSpectrum[slot][band] = effects_.spectrum(slot, band);
        out.fxPhases[slot] = effects_.sweepPhase(slot);
    }
    for (int f = 0; f < kFilters; ++f) // and so do the filters' analyzers, as the filters ring out
        out.filterSpectrum[f] = filterAnalyzed_[f].bands;
    const Voice *newest = nullptr;
    for (const Voice &v : voices_) {
        if (!live(v))
            continue;
        ++out.voices;
        if (!newest || v.order > newest->order)
            newest = &v;
    }
    if (!newest)
        return;
    for (int m = 0; m < newest->slotModCount; ++m)
        out.modOffset[newest->slotMods[m].index] += static_cast<float>(newest->slotMods[m].offset);
    for (int i = 0; i < macroTargetCount_; ++i) // what the macros add, once for each target they share
        if (std::find(macroTargets_.begin(), macroTargets_.begin() + i, macroTargets_[i]) == macroTargets_.begin() + i)
            out.modOffset[macroTargets_[i]] += static_cast<float>(macroMod_[macroTargets_[i]]);
    for (int s = 0; s < kMaxSources; ++s) {
        out.sources[s] = static_cast<float>(newest->sourceValues[s]);
        out.lfoPhases[s] = static_cast<float>(newest->lfos[s].phase);
        out.lfoRates[s] = static_cast<float>(newest->lfos[s].hz);
        out.lfoFrom[s] = static_cast<float>(newest->lfos[s].from);
        out.lfoTo[s] = static_cast<float>(newest->lfos[s].held);
    }
    for (int i = 0; i < kOps; ++i)
        out.opLevels[i] = static_cast<float>(newest->ops[i].env.level() * newest->ops[i].amp);
    out.notePitch = static_cast<float>(newest->pitch);
}

void Synth::setVoiceLimit(int limit) {
    const int before = limit_;
    limit_ = std::clamp(limit, 1, std::max(capacity(), 1));
    if (limit_ >= before)
        return;
    for (int playing = static_cast<int>(std::count_if(voices_.begin(), voices_.end(), live)); playing > limit_;
         --playing)
        startFade(*stealable());
}

Synth::Voice *Synth::stealable() {
    auto rank = [](const Voice &v) { return std::tuple(!v.released, v.peak, v.order); };
    Voice *victim = nullptr;
    for (Voice &v : voices_)
        if (live(v) && (!victim || rank(v) < rank(*victim)))
            victim = &v;
    return victim;
}

Synth::Voice &Synth::allocateVoice() {
    // At the limit: steal one (see stealable()). It fades out in a place of its own.
    if (std::count_if(voices_.begin(), voices_.end(), live) >= limit_)
        if (Voice *victim = stealable())
            startFade(*victim);
    // A free place, else the fading voice closest to silence. There are more
    // places than the limit, so one of the two is always there.
    Voice *quietest = nullptr;
    for (Voice &v : voices_) {
        if (!v.active)
            return v;
        if (v.fading && (!quietest || v.fade < quietest->fade))
            quietest = &v;
    }
    return quietest ? *quietest : voices_.front();
}

void Synth::startFade(Voice &v) {
    if (!live(v))
        return;
    endVoice(v); // the note is over, though the sound takes a moment to go
    v.active = true;
    v.fading = true;
    v.fade = 1.0;
}

void Synth::endVoice(Voice &v) {
    if (!v.active)
        return;
    v.active = false;
    if (v.fading) { // its note ended when it started fading
        v.fading = false;
        return;
    }
    // A stolen or choked note is over even if its key is still down.
    for (size_t i = held_.size(); i-- > 0;)
        if (held_[i].note == v.note)
            held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
    reportEnded(v.note);
}

void Synth::reportEnded(const NoteRef &note) {
    if (std::find(ended_.begin(), ended_.end(), note) == ended_.end() && ended_.size() < ended_.capacity())
        ended_.push_back(note); // capacity was reserved in prepare(), so this never allocates
}

void Synth::renderBlock(Voice &v, double *left, double *right, uint32_t hostFrames, const ParamValues &params) {
    // This voice's param values before LFOs and envelopes: automation plus host
    // modulation, global and per voice. Freeform shapes are read from `params`;
    // slot modulation of the effects reaches them in render().
    using Values = std::array<double, kVoiceParamCount>;
    Values raw;
    for (int i = 0; i < kVoiceParamCount; ++i)
        raw[i] = params[i] + globalMod_[i] + macroMod_[i] + v.mod[i];
    auto withSlotMods = [&](Values &p) {
        p = raw;
        for (int m = 0; m < v.slotModCount; ++m)
            if (v.slotMods[m].index < kVoiceParamCount)
                p[v.slotMods[m].index] += v.slotMods[m].offset;
        for (int i = 0; i < kVoiceParamCount; ++i)
            p[i] = std::clamp(p[i], table_[i].min, table_[i].max);
    };

    // Modulation sources, read once per block at the host rate. They see the
    // previous block's slot modulation; the sound below sees this block's.
    Values p;
    withSlotMods(p);
    v.slotModCount = 0;
    // `swing` scales what the source puts out into its slots, from `center`: a bipolar LFO's -1..1 takes a depth of
    // 100 % from half its range below the set value to half above, as far as a unipolar one or an envelope takes it;
    // a bipolar envelope's 0..1 swings the same way about its middle.
    auto collectSlots = [&](int source, double value, double swing = 1.0, double center = 0.0) {
        v.sourceValues[source] = value;
        for (int s = 0; s < kSlots; ++s) {
            const int target = slotTarget(p[slotTargetIndex(source, s)]);
            const double amount = p[slotAmountIndex(source, s)];
            if (target >= 0 && amount != 0.0 && params[slotBypassIndex(source, s)] < 0.5)
                v.slotMods[v.slotModCount++] = {
                    target, swing * (value - center) * amount * (table_[target].max - table_[target].min)};
        }
    };
    // Empty places and switched-off sources stand still and modulate nothing.
    auto get = [&p](int i) { return p[i]; };
    for (int s = 0; s < kMaxSources; ++s) {
        const SourceType type = sourceType(p[paramIndex(s, SourceParam::Type)]);
        if (type == SourceType::None || p[sourceEnabledIndex(s)] < 0.5) {
            v.sourceValues[s] = 0.0;
            continue;
        }
        if (type == SourceType::Lfo) {
            Lfo &lfo = v.lfos[s];
            // A free LFO takes the shared clock, which render() moves on; switched
            // back to retrigger, it carries on from there. A one-shot is never free.
            const bool oneShot = p[paramIndex(s, SourceParam::OneShot)] >= 0.5;
            const bool free = !oneShot && p[paramIndex(s, SourceParam::Free)] >= 0.5;
            const double chaos = p[paramIndex(s, SourceParam::RandomChaos)];
            if (free)
                lfo = freeLfos_[s];
            else if (v.fresh)
                lfo.held = randomStep(lfo.from, chaos, v.rng);
            const auto shape = static_cast<LfoShape>(stepped(p[paramIndex(s, SourceParam::Shape)],
                                                             static_cast<int>(LfoShape::Count) - 1));
            const double value =
                shape == LfoShape::Custom ? shapeValue([&params](int i) { return params[i]; }, s, lfo.phase)
                : shape == LfoShape::Random
                    ? randomGlide(lfo.from, lfo.held, lfo.phase, p[paramIndex(s, SourceParam::RandomSmooth)])
                    : lfoValue(shape, lfo.phase, lfo.held);
            if (p[paramIndex(s, SourceParam::Unipolar)] >= 0.5)
                collectSlots(s, 0.5 * (value + 1.0));
            else
                collectSlots(s, value, 0.5);
            if (!free)
                advanceLfo(lfo, lfoHz(get, s, tempo_), hostFrames, v.rng, oneShot, chaos);
            continue;
        }
        Envelope &env = v.modEnvs[s];
        collectSlots(s, env.level(), 1.0, p[paramIndex(s, SourceParam::EnvBipolar)] >= 0.5 ? 0.5 : 0.0);
        const EnvRates rates =
            envRates(p[paramIndex(s, SourceParam::Attack)], p[paramIndex(s, SourceParam::Hold)],
                     p[paramIndex(s, SourceParam::Decay)],
                     p[paramIndex(s, SourceParam::Sustain)], p[paramIndex(s, SourceParam::Release)],
                     p[paramIndex(s, SourceParam::AttackCurve)], p[paramIndex(s, SourceParam::DecayCurve)],
                     p[paramIndex(s, SourceParam::ReleaseCurve)], sampleRate_);
        for (uint32_t i = 0; i < hostFrames; ++i)
            env.next(rates);
    }
    withSlotMods(p);

    // A new note starts each operator copy at phase 0, moved by up to Phase
    // Random of a cycle. Done here rather than at note-on, so host modulation
    // sent with the note applies.
    const bool starting = v.fresh;
    if (v.fresh) {
        v.fresh = false;
        for (int i = 0; i < kOps; ++i) {
            const double amount = p[paramIndex(i, OpParam::PhaseRandom)];
            for (int k = 0; k < kMaxUnison; ++k)
                v.ops[i].phases[k] = amount * 0.5 * (nextRandom(v.rng) + 1.0);
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
    const double bend = std::clamp(bend_ + wheelBend_, -1.0, 1.0);
    auto pitchNow = [&] {
        return v.glide + v.tuning + bend * p[paramIndex(GlobalParam::BendRange)] + p[paramIndex(GlobalParam::Pitch)];
    };
    double pitch = pitchNow();

    // The note source follows that pitch, through its range and curve, and modulates from this block on: a
    // cutoff it takes to the note sits on the pitch the operators play now. Its targets are worked out again with the other sources' modulation,
    // which they already hold; a target that moves the pitch itself moves it from here. Its range and slots take
    // only the host's modulation for every voice, no LFO's or envelope's.
    v.pitch = pitch;
    // Bipolar, it swings about the middle of its output, like a macro.
    const double note = noteSourceValue([&](int i) { return hostValue(params, i); }, pitch) -
                        (params[paramIndex(NoteParam::Bipolar)] >= 0.5 ? 0.5 : 0.0);
    const int noteModsStart = v.slotModCount;
    for (int s = 0; s < kSlots; ++s) {
        const int target = slotTarget(params[slotTargetIndex(kNoteSource, s)]);
        const double amount = hostValue(params, slotAmountIndex(kNoteSource, s));
        if (target >= 0 && amount != 0.0)
            v.slotMods[v.slotModCount++] = {target, note * amount * (table_[target].max - table_[target].min)};
    }
    for (int m = noteModsStart; m < v.slotModCount; ++m) {
        const int target = v.slotMods[m].index;
        if (target >= kVoiceParamCount)
            continue; // an effect's, which render() applies
        double modulated = raw[target];
        for (int k = 0; k < v.slotModCount; ++k)
            modulated += v.slotMods[k].index == target ? v.slotMods[k].offset : 0.0;
        p[target] = std::clamp(modulated, table_[target].min, table_[target].max);
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
    const double fmStep = (fmTarget - v.fmScale) * ramp;
    const double gainLStep = (gain * std::numbers::sqrt2 * std::cos(angle) - v.gainL) * ramp;
    const double gainRStep = (gain * std::numbers::sqrt2 * std::sin(angle) - v.gainR) * ramp;
    // A stolen voice fades out, then ends.
    const double fadeStep = v.fading ? 1.0 / (kStealFade * rate_) : 0.0;
    // White noise is as loud in the audible band as when it runs at the host rate.
    const double whiteGain = noiseColor_.coefs(0.0).gain;

    Levels target{};
    std::array<EnvRates, kOps> rates;
    std::array<double, kOps> ampStep;
    std::array<const float *, kOps> wave;
    std::array<bool, kOps> noise;
    // Noise has no pitch and no unison copies. Its color comes from a filter, and
    // Unison Spread widens it with a second noise on the sides.
    std::array<bool, kOps> colored;
    std::array<NoiseColor::Coefs, kOps> noiseCoefs;
    std::array<double, kOps> noiseMid;
    std::array<double, kOps> noiseSide;
    // Unison: each copy has its own increment and stereo gains. The copies'
    // mono sum, scaled by 1/sqrt(copies), is what other operators hear.
    std::array<int, kOps> copies;
    std::array<double, kOps> monoGain;
    std::array<std::array<double, kMaxUnison>, kOps> copyInc;
    std::array<std::array<double, kMaxUnison>, kOps> copyGainL;
    std::array<std::array<double, kMaxUnison>, kOps> copyGainR;
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
        const double ratio = coarseRatio(p[paramIndex(i, OpParam::Coarse)]) * (1.0 + p[paramIndex(i, OpParam::Fine)]);
        const double cents = p[paramIndex(i, OpParam::Detune)];
        const double semitones = p[paramIndex(i, OpParam::Transpose)];
        const double inc = std::min(noteHz * ratio * std::exp2(semitones / 12.0 + cents / 1200.0) / rate_, 0.5);
        const auto shape = static_cast<OpWave>(
            stepped(p[paramIndex(i, OpParam::Wave)], static_cast<int>(OpWave::Count) - 1));
        noise[i] = shape == OpWave::Noise;

        const double velocityScale = 1.0 - p[paramIndex(i, OpParam::Velocity)] * (1.0 - v.velocity);
        ampStep[i] = (velocityScale - op.amp) * ramp;
        rates[i] = envRates(p[paramIndex(i, OpParam::Attack)], p[paramIndex(i, OpParam::Hold)],
                            p[paramIndex(i, OpParam::Decay)],
                            p[paramIndex(i, OpParam::Sustain)], p[paramIndex(i, OpParam::Release)],
                            p[paramIndex(i, OpParam::AttackCurve)], p[paramIndex(i, OpParam::DecayCurve)],
                            p[paramIndex(i, OpParam::ReleaseCurve)], rate_);
        target.out[i] = opOn[i] ? p[paramIndex(i, OpParam::Out)] : 0.0;
        double heard = target.out[i];
        for (int f = 0; f < kFilters; ++f) {
            target.send[i][f] = opOn[i] && filterEnabled[f] ? p[sendIndex(i, f)] : 0.0;
            heard = std::max(heard, target.send[i][f]);
        }
        for (int sh = 0; sh < kShapers; ++sh) {
            target.shaperSend[i][sh] = opOn[i] && shaperEnabled[sh] ? p[shaperSendIndex(i, sh)] : 0.0;
            heard = std::max(heard, target.shaperSend[i][sh]);
        }

        // Copies spread evenly from -1 to 1: detuned by up to Unison Detune
        // cents and panned by up to Unison Spread.
        copies[i] = noise[i] ? 1
                             : std::clamp(static_cast<int>(std::lround(p[paramIndex(i, OpParam::Unison)])), 1,
                                          kMaxUnison);
        monoGain[i] = 1.0 / std::sqrt(static_cast<double>(copies[i]));
        const double detune = p[paramIndex(i, OpParam::UnisonDetune)];
        const double spread = p[paramIndex(i, OpParam::UnisonSpread)];
        // The table suits the highest copy, so none of them reaches past Nyquist.
        const double highest = copies[i] > 1 ? inc * std::exp2(detune / 1200.0) : inc;
        wave[i] = shape == OpWave::Custom ? customWaves_[i].table(highest) : waves_.table(shape, highest);
        if (noise[i]) {
            const double slope = p[paramIndex(i, OpParam::NoiseSlope)];
            colored[i] = slope > 0.0;
            if (colored[i])
                noiseCoefs[i] = noiseColor_.coefs(slope);
            // Left and right are mid +- side: uncorrelated at full spread, each as loud as the mid alone.
            noiseMid[i] = 1.0 / std::sqrt(1.0 + spread * spread);
            noiseSide[i] = spread * noiseMid[i];
        }
        for (int k = 0; k < copies[i]; ++k) {
            const double offset = copies[i] > 1 ? 2.0 * k / (copies[i] - 1) - 1.0 : 0.0;
            copyInc[i][k] = std::min(inc * std::exp2(offset * detune / 1200.0), 0.5);
            const double copyAngle = (offset * spread + 1.0) * std::numbers::pi / 4.0;
            copyGainL[i][k] = std::numbers::sqrt2 * std::cos(copyAngle) * monoGain[i];
            copyGainR[i][k] = std::numbers::sqrt2 * std::sin(copyAngle) * monoGain[i];
        }
        stereo = stereo || ((copies[i] > 1 || noise[i]) && spread > 0.0 && heard > 0.0);

        // Squared amounts give finer control over the subtle low end.
        for (int to = 0; to < kOps; ++to) {
            const double amount = opOn[i] ? p[matrixIndex(i, to)] : 0.0;
            target.index[i][to] = amount * amount * (i == to ? kMaxFeedback : kMaxIndex) / (2.0 * std::numbers::pi);
        }
        if (!opOn[i]) {
            // Off is off at once; switched on again, it comes back from silence.
            op.out = 0.0;
            op.prevOut = 0.0;
            v.levels.out[i] = 0.0;
            v.levels.send[i].fill(0.0);
            v.levels.shaperSend[i].fill(0.0);
            v.levels.index[i].fill(0.0);
        }
    }

    // A filter can feed any later one: link[from][to]. Filters modulate operators like operators do, and feed the
    // shapers, from the previous sample's output; in a voice's tail, no longer the shapers (see Voice::tail).
    for (int from = 0; from < kFilters; ++from)
        for (int to = from + 1; to < kFilters; ++to)
            target.link[from][to] = filterEnabled[from] && filterEnabled[to] ? p[filterLinkIndex(from, to)] : 0.0;
    for (int f = 0; f < kFilters; ++f) {
        for (int to = 0; to < kOps; ++to) {
            const double amount = filterEnabled[f] ? p[filterModIndex(f, to)] : 0.0;
            target.filterIndex[f][to] = amount * amount * kMaxIndex / (2.0 * std::numbers::pi);
        }
        for (int sh = 0; sh < kShapers; ++sh)
            target.filterShaper[f][sh] =
                filterEnabled[f] && shaperEnabled[sh] && !v.tail ? p[filterShaperIndex(f, sh)] : 0.0;
        target.filterOut[f] = p[paramIndex(f, FilterParam::Out)];
    }
    // A shaper clips what the operators and filters send it, like the shaper effect, and from there feeds the
    // filters, modulates operators and goes out, like a filter does.
    std::array<ClipCurve, kShapers> curve;
    std::array<bool, kShapers> block;
    for (int sh = 0; sh < kShapers; ++sh) {
        for (int to = 0; to < kOps; ++to) {
            const double amount = shaperEnabled[sh] ? p[shaperModIndex(sh, to)] : 0.0;
            target.shaperIndex[sh][to] = amount * amount * kMaxIndex / (2.0 * std::numbers::pi);
        }
        for (int f = 0; f < kFilters; ++f)
            target.shaperLink[sh][f] = shaperEnabled[sh] && filterEnabled[f] ? p[shaperFilterIndex(sh, f)] : 0.0;
        target.shaperDirect[sh] = shaperEnabled[sh] ? p[paramIndex(sh, ShaperParam::Out)] : 0.0;
        const ShaperSettings settings =
            shaperSettings([&p](int index) { return p[index]; }, shaperParams(kFxSlots + sh));
        target.shaperDrive[sh] = settings.drive;
        target.shaperOutput[sh] = settings.output;
        target.shaperMix[sh] = settings.mix;
        curve[sh] = {settings.mode, shaperTables_[sh].values.data(), settings.symmetric};
        block[sh] = clipNeedsBlocker(settings.mode, settings.symmetric);
    }

    // A new voice starts at its levels; after that they ramp.
    if (starting)
        v.levels = target;
    Levels step{};
    bool ramping = false;
    {
        const std::span<double> now = values(v.levels), to = values(target), by = values(step);
        for (size_t k = 0; k < now.size(); ++k) {
            if (std::abs(to[k] - now[k]) < 1e-9)
                now[k] = to[k];
            else
                ramping = true;
            by[k] = (to[k] - now[k]) * ramp;
        }
    }
    // Filters run only while something is routed into them (a route still
    // ramping down counts), and only once (on the left channel, copied right)
    // while the voice has no stereo spread.
    const int channels = stereo ? 2 : 1;
    if (stereo && v.rightStale) { // without spread, channel 1 would have been where channel 0 is
        for (int f = 0; f < kFilters; ++f) {
            v.filters[f][1] = v.filters[f][0];
            std::copy_n(combLine(v, f, 0), combSize_, combLine(v, f, 1));
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
    bool shaperMods = false, filterMods = false, filterShapes = false;
    for (int sh = 0; sh < kShapers; ++sh) {
        for (int to = 0; to < kOps; ++to) {
            const bool mods = routed(target.shaperIndex[sh][to], current.shaperIndex[sh][to]);
            shaperUsed[sh] = shaperUsed[sh] || mods;
            shaperMods = shaperMods || mods;
        }
        for (int f = 0; f < kFilters; ++f)
            shaperUsed[sh] = shaperUsed[sh] || routed(target.shaperLink[sh][f], current.shaperLink[sh][f]);
        shaperUsed[sh] =
            shaperEnabled[sh] && (shaperUsed[sh] || routed(target.shaperDirect[sh], current.shaperDirect[sh]));
    }
    for (int f = 0; f < kFilters; ++f)
        for (int to = 0; to < kOps; ++to)
            filterMods = filterMods || routed(target.filterIndex[f][to], current.filterIndex[f][to]);
    // What the operators feed, then what that feeds in turn: filters and shapers can feed each other, round in a
    // loop too, so it takes a few passes for what runs to settle.
    std::array<bool, kShapers> shaperOn{};
    std::array<bool, kFilters> filterOn{};
    for (int i = 0; i < kOps; ++i) {
        for (int f = 0; f < kFilters; ++f)
            filterOn[f] = filterOn[f] || (filterEnabled[f] && routed(target.send[i][f], current.send[i][f]));
        for (int sh = 0; sh < kShapers; ++sh)
            shaperOn[sh] =
                shaperOn[sh] || (shaperUsed[sh] && routed(target.shaperSend[i][sh], current.shaperSend[i][sh]));
    }
    for (int pass = 0; pass < kFilters + kShapers; ++pass) {
        for (int f = 0; f < kFilters; ++f) {
            for (int from = 0; from < f; ++from)
                filterOn[f] = filterOn[f] || (filterEnabled[f] && filterOn[from] &&
                                              routed(target.link[from][f], current.link[from][f]));
            for (int sh = 0; sh < kShapers; ++sh)
                filterOn[f] = filterOn[f] || (filterEnabled[f] && shaperOn[sh] &&
                                              routed(target.shaperLink[sh][f], current.shaperLink[sh][f]));
        }
        for (int sh = 0; sh < kShapers; ++sh)
            for (int f = 0; f < kFilters; ++f) {
                const bool shapes =
                    shaperUsed[sh] && filterOn[f] && routed(target.filterShaper[f][sh], current.filterShaper[f][sh]);
                shaperOn[sh] = shaperOn[sh] || shapes;
                filterShapes = filterShapes || shapes;
            }
    }
    for (int sh = 0; sh < kShapers; ++sh) {
        if (!shaperOn[sh]) {
            v.shapers[sh] = {};
            v.shaperOut[sh] = 0.0;
        }
    }
    const double pole = clipBlockerPole(rate_); // the shapers' DC blockers
    std::array<FilterSettings, kFilters> filter;
    for (int f = 0; f < kFilters; ++f) {
        const double cutoff = p[paramIndex(f, FilterParam::Cutoff)];
        const double cutoffHz =
            cutoffFromNorm(cutoff) * std::exp2((pitch - 60.0) / 12.0 * p[paramIndex(f, FilterParam::KeyTrack)]);
        const auto type = static_cast<FilterType>(
            stepped(p[paramIndex(f, FilterParam::Type)], static_cast<int>(FilterType::Count) - 1));
        filter[f] = filterSettings(type, filterSlope(p[paramIndex(f, FilterParam::Slope)]), cutoffHz,
                                   p[paramIndex(f, FilterParam::Resonance)], p[paramIndex(f, FilterParam::Drive)], rate_);
    }
    // What the filters put out, before their output levels, for the editor's analyzers.
    std::array<double *, kFilters> taps{};
    if (analyzing_)
        for (int f = 0; f < kFilters; ++f)
            taps[f] = filterTaps_[f].data();

    Levels &level = v.levels;
    double blockPeak = 0.0;
    for (uint32_t s = 0; s < frames; ++s) {
        v.fmScale += fmStep;
        v.gainL += gainLStep;
        v.gainR += gainRStep;
        v.fade = std::max(v.fade - fadeStep, 0.0);
        if (ramping) {
            const std::span<double> now = values(level), by = values(step);
            for (size_t k = 0; k < now.size(); ++k)
                now[k] += by[k];
        }
        std::array<double, 2> mix{};
        std::array<std::array<double, 2>, kFilters> toFilter{};
        std::array<std::array<double, 2>, kShapers> toShaper{};
        for (int i = 0; i < kOps; ++i) {
            if (!opOn[i])
                continue;
            Operator &op = v.ops[i];
            // Operators before i already hold this sample's output, later ones the
            // previous sample's. Feedback averages the last two outputs to stay stable.
            double pm = 0.0;
            for (int j = 0; j < kOps; ++j)
                pm += level.index[j][i] * (j == i ? 0.5 * (op.out + op.prevOut) : v.ops[j].out);
            if (filterMods)
                for (int f = 0; f < kFilters; ++f)
                    pm += level.filterIndex[f][i] * v.filterOut[f];
            if (shaperMods)
                for (int sh = 0; sh < kShapers; ++sh)
                    pm += level.shaperIndex[sh][i] * v.shaperOut[sh];
            const double offset = pm * v.fmScale;

            op.amp += ampStep[i];
            const double envLevel = op.env.next(rates[i]) * op.amp;
            double mono = 0.0, l = 0.0, r = 0.0;
            if (noise[i]) {
                mono = nextRandom(v.rng);
                mono = colored[i] ? noiseColor_.process(mono, noiseCoefs[i], op.noise[0]) : mono * whiteGain;
                l = r = mono * noiseMid[i];
                if (noiseSide[i] > 0.0) {
                    double side = nextRandom(v.rng);
                    side = colored[i] ? noiseColor_.process(side, noiseCoefs[i], op.noise[1]) : side * whiteGain;
                    l += side * noiseSide[i];
                    r -= side * noiseSide[i];
                }
            } else {
                for (int k = 0; k < copies[i]; ++k) {
                    const double x = WaveTables::read(wave[i], op.phases[k] + offset);
                    mono += x;
                    l += x * copyGainL[i][k];
                    r += x * copyGainR[i][k];
                    op.phases[k] += copyInc[i][k];
                    if (op.phases[k] >= 1.0)
                        op.phases[k] -= 1.0;
                }
            }
            op.prevOut = op.out;
            op.out = mono * monoGain[i] * envLevel;
            l *= envLevel;
            r *= envLevel;
            mix[0] += l * level.out[i];
            mix[1] += r * level.out[i];
            for (int f = 0; f < kFilters; ++f) {
                toFilter[f][0] += l * level.send[i][f];
                toFilter[f][1] += r * level.send[i][f];
            }
            for (int sh = 0; sh < kShapers; ++sh) {
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
            std::array<double, 2> y{};
            for (int c = 0; c < channels; ++c) // as the shaper effect runs (see Effects::runShaper())
                y[c] = v.shapers[sh][c].run(curve[sh], toShaper[sh][c], level.shaperDrive[sh], level.shaperOutput[sh],
                                            level.shaperMix[sh], block[sh], pole);
            if (!stereo)
                y[1] = y[0];
            v.shaperOut[sh] = 0.5 * (y[0] + y[1]);
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
            std::array<double, 2> y{};
            for (int c = 0; c < channels; ++c)
                y[c] = v.filters[f][c].process(toFilter[f][c], filter[f], combLine(v, f, c), combSize_);
            if (!stereo)
                y[1] = y[0];
            v.filterOut[f] = 0.5 * (y[0] + y[1]);
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
        peak = std::max(peak, v.ops[i].env.level() * heard);
    }
    v.peak = peak;
    v.tail = v.released && !sounding;
    if (v.fading && v.fade <= 0.0)
        endVoice(v);
    else if (v.released && !sounding && blockPeak < 1e-4) {
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
