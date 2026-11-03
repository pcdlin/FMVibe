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
// depend on it. Volume gives the 12 dB back, so at 0 dB a full-scale operator comes out at full scale.
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

Synth::Synth() : table_(paramTable()), waves_(waveTables()) {
    for (int i = 0; i < kModulatedCount; ++i) {
        paramMin_[i] = table_[i].min;
        paramMax_[i] = table_[i].max;
    }
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
    for (std::array<bool, 2> &dirty : v.combDirty) // cleared once a comb reads them (see renderBlock())
        dirty = {true, true};
    v.active = true;
    v.note = note;
    v.velocity = played.velocity;
    v.order = noteCounter_++;
    v.rng = static_cast<uint32_t>(v.order * 2654435761u) | 1u;
    v.glide = glide ? from : note.key;
    v.fresh = true;
    v.auxOut.fill(0.0); // a new note's sources have put out nothing yet
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
    clearVoiceMods(v);
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
            lfo.restart = true;
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
            setVoiceMod(v, index, amount);
}

void Synth::applyMacros(const ParamValues &params) {
    for (int i = 0; i < macroTargetCount_; ++i)
        macroMod_[macroTargets_[i]] = 0.0;
    macroTargetCount_ = 0;
    auto get = [this, &params](int i) { return hostValue(params, i); };
    forEachMacroMod(get, [this](int target, double offset) {
        macroMod_[target] += offset;
        macroTargets_[macroTargetCount_++] = target;
    });
    voiceMacroSlotCount_ = 0;
    for (int source = kMaxSources; source < kAllSources; ++source) {
        if (isNoteBorne(source))
            continue;
        for (int slot = 0; slot < kSlots; ++slot)
            if (const int aux = slotAux(get, source, slot); aux >= 0 && runsPerVoice(aux) &&
                                                             slotTarget(params[slotTargetIndex(source, slot)]) >= 0 &&
                                                             params[slotBypassIndex(source, slot)] < 0.5)
                voiceMacroSlots_[voiceMacroSlotCount_++] = {source, slot};
    }
}

void Synth::render(float *left, float *right, uint32_t frames, const ParamValues &params) {
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    applyMacros(params);
    voiceBaseReady_ = false; // what the voices share is summed again for these params, once a voice needs it
    for (int op = 0; op < kOps; ++op) // the Custom waves follow their partials, built again when they change
        customWaves_[op].update(
            partialCount(params[partialCountIndex(op)]), [&](int k) { return params[partialLevelIndex(op, k)]; },
            [&](int k) { return params[partialPhaseIndex(op, k)]; });
    for (int sh = 0; sh < kShapers; ++sh) { // and the shapers' drawn curves their points
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        if (clipMode(params[fields.mode]) == ClipMode::Custom)
            shaperTables_[sh].update([&params](int index) { return params[index]; }, fields.curve);
    }
    // The effects' params with the host's and the macros' modulation, which stay as they are through a render(): as
    // summed, and in their ranges in fxParams_, where each control block adds the newest voice's slot modulation to
    // the params it moves. Their drawn curves are read as set; of the rest, the effects read only FxOrder.
    for (int i = kFxBase; i < kShapeBase; ++i) {
        fxBase_[i - kFxBase] = params[i] + globalMod_[i] + macroMod_[i];
        fxParams_[i] = std::clamp(fxBase_[i - kFxBase], paramMin_[i], paramMax_[i]);
    }
    fxMovedCount_ = 0;
    fxParams_[paramIndex(GlobalParam::FxOrder)] = params[paramIndex(GlobalParam::FxOrder)];
    std::copy(params.begin() + kClipCurveBase, params.begin() + kSlotBypassBase, fxParams_.begin() + kClipCurveBase);
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
        // The params the last block's slots moved go back as they were; then each param the newest voice's slots
        // move takes its slots' offsets in their order, and its range.
        auto inRange = [this](int i, double value) { return std::clamp(value, paramMin_[i], paramMax_[i]); };
        for (int k = 0; k < fxMovedCount_; ++k)
            fxParams_[fxMoved_[k]] = inRange(fxMoved_[k], fxBase_[fxMoved_[k] - kFxBase]);
        fxMovedCount_ = 0;
        if (newest) {
            for (int m = 0; m < newest->slotModCount; ++m) {
                const int i = newest->slotMods[m].index;
                if (i >= kFxBase && i < kShapeBase) { // a param moved twice is listed twice: that does no harm
                    fxParams_[i] = fxBase_[i - kFxBase];
                    fxMoved_[fxMovedCount_++] = i;
                }
            }
            for (int m = 0; m < newest->slotModCount; ++m)
                if (newest->slotMods[m].index >= kFxBase && newest->slotMods[m].index < kShapeBase)
                    fxParams_[newest->slotMods[m].index] += newest->slotMods[m].offset;
            for (int k = 0; k < fxMovedCount_; ++k)
                fxParams_[fxMoved_[k]] = inRange(fxMoved_[k], fxParams_[fxMoved_[k]]);
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
}

void Synth::scopeTake(double phase, double value) {
    ScopeCapture &s = scope_;
    const int total = s.cycles * kScopeBinsPerCycle;
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
    out = Telemetry{};
    if (scope_.op >= 0 && scope_.live) {
        // A slow span (or the first one, before it is whole) shows as it fills; a quick one a whole span at a time.
        const int total = scope_.cycles * kScopeBinsPerCycle;
        const bool slow = scope_.inc <= 0.0 || scope_.cycles / (scope_.inc * rate_) > kScopeSlowSpan;
        const bool filling = slow || !scope_.whole;
        std::copy_n((filling ? scope_.filling : scope_.frame).begin(), total, out.scope.begin());
        out.scopeBins = total;
        out.scopeCursor = filling ? static_cast<float>(scope_.next) / static_cast<float>(total) : -1.0f;
    }
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
    out.noteVelocity = static_cast<float>(newest->velocity);
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

void Synth::shareVoiceParams(const ParamValues &params) {
    for (int i = 0; i < kVoiceParamCount; ++i) {
        voiceBase_[i] = params[i] + globalMod_[i] + macroMod_[i];
        // As a voice without host modulation of its own has it: with its modulation of 0 added (see renderBlock()).
        voiceClamped_[i] = std::clamp(voiceBase_[i] + 0.0, paramMin_[i], paramMax_[i]);
    }
    voiceBaseReady_ = true;
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
    // the params the slots move, and puts back first those the slots of its last call moved.
    using Values = std::array<double, kVoiceParamCount>;
    Values p;
    std::copy(voiceClamped_.begin(), voiceClamped_.end(), p.begin());
    for (int k = 0; k < v.moddedCount; ++k)
        p[v.modded[k]] = inRange(v.modded[k], raw(v.modded[k]));
    std::array<int, kAllSources * kSlots> moved;
    int movedCount = 0;
    auto withSlotMods = [&] {
        for (int k = 0; k < movedCount; ++k)
            p[moved[k]] = inRange(moved[k], raw(moved[k]));
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
            p[moved[k]] = inRange(moved[k], slotSum_[moved[k]]);
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
            scale *= runsPerVoice(aux) ? v.auxOut[aux] : hostValue(params, stripKnobIndex(aux));
        return scale;
    };
    auto collectSlots = [&](int source, double shown, double unipolar, double bipolar, bool bipolarSource) {
        v.sourceValues[source] = shown;
        v.auxOut[source] = unipolar;
        const double swing = bipolarSource ? bipolar : unipolar;
        for (int s = 0; s < kSlots; ++s) {
            const int target = slotTarget(p[slotTargetIndex(source, s)]);
            const double amount = p[slotAmountIndex(source, s)];
            if (target >= 0 && amount != 0.0 && params[slotBypassIndex(source, s)] < 0.5) {
                v.slotMods[v.slotModCount++] = {
                    target, swing * amount * scaleOf(source, s) * (table_[target].max - table_[target].min)};
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
            // A note starts it at its Phase (100 % is 0 % again), read here so that host modulation sent with the
            // note applies. A free one keeps the shared clock, and Random its steps.
            if (lfo.restart && shape != LfoShape::Random) {
                const double start = p[paramIndex(s, SourceParam::Phase)];
                lfo.phase = start - std::floor(start);
            }
            lfo.restart = false;
            const double value =
                shape == LfoShape::Custom ? shapeValue([&params](int i) { return params[i]; }, s, lfo.phase)
                : shape == LfoShape::Random
                    ? randomGlide(lfo.from, lfo.held, lfo.phase, p[paramIndex(s, SourceParam::RandomSmooth)])
                    : lfoValue(shape, lfo.phase, lfo.held);
            const bool lfoUnipolar = p[paramIndex(s, SourceParam::Unipolar)] >= 0.5;
            collectSlots(s, lfoUnipolar ? 0.5 * (value + 1.0) : value, 0.5 * (value + 1.0), 0.5 * value, !lfoUnipolar);
            if (!free)
                advanceLfo(lfo, lfoHz(get, s, tempo_), hostFrames, v.rng, oneShot, chaos);
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
        for (uint32_t i = 0; i < hostFrames; ++i)
            env.next(rates);
    }
    // Velocity, through its curve, from what the note came with: like the note source's, its depths take only the
    // host's modulation for every voice. It modulates from the note's first block, as the LFOs and envelopes do.
    {
        const double out = velocitySourceValue(getParam, v.velocity);
        v.auxOut[kVelocitySource] = out;
        const double swing = params[paramIndex(VelocityParam::Bipolar)] >= 0.5 ? out - 0.5 : out;
        for (int s = 0; s < kSlots; ++s) {
            const int target = slotTarget(params[slotTargetIndex(kVelocitySource, s)]);
            const double amount = hostValue(params, slotAmountIndex(kVelocitySource, s));
            if (target < 0 || amount == 0.0 || params[slotBypassIndex(kVelocitySource, s)] >= 0.5)
                continue;
            v.slotMods[v.slotModCount++] = {
                target, swing * amount * scaleOf(kVelocitySource, s) * (table_[target].max - table_[target].min)};
        }
    }
    // The macros' and the mod wheel's slots that an aux of this voice scales (see applyMacros()).
    for (int k = 0; k < voiceMacroSlotCount_; ++k) {
        const auto [source, s] = voiceMacroSlots_[k];
        const int target = slotTarget(params[slotTargetIndex(source, s)]);
        const double amount = hostValue(params, slotAmountIndex(source, s));
        if (target < 0 || amount == 0.0)
            continue;
        const double swing =
            hostValue(params, stripKnobIndex(source)) - (sourceBipolar(getParam, source) ? 0.5 : 0.0);
        v.slotMods[v.slotModCount++] = {
            target, swing * amount * scaleOf(source, s) * (table_[target].max - table_[target].min)};
    }
    withSlotMods();

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
    const double note = noteSourceValue([&](int i) { return hostValue(params, i); }, pitch);
    v.auxOut[kNoteSource] = note;
    const double swing = note - (params[paramIndex(NoteParam::Bipolar)] >= 0.5 ? 0.5 : 0.0);
    const int noteModsStart = v.slotModCount;
    for (int s = 0; s < kSlots; ++s) {
        const int target = slotTarget(params[slotTargetIndex(kNoteSource, s)]);
        const double amount = hostValue(params, slotAmountIndex(kNoteSource, s));
        if (target >= 0 && amount != 0.0 && params[slotBypassIndex(kNoteSource, s)] < 0.5) {
            v.slotMods[v.slotModCount++] = {
                target, swing * amount * scaleOf(kNoteSource, s) * (table_[target].max - table_[target].min)};
        }
    }
    for (int m = noteModsStart; m < v.slotModCount; ++m) {
        const int target = v.slotMods[m].index;
        if (target >= kVoiceParamCount)
            continue; // an effect's, which render() applies
        double modulated = raw(target);
        for (int k = 0; k < v.slotModCount; ++k)
            modulated += v.slotMods[k].index == target ? v.slotMods[k].offset : 0.0;
        p[target] = inRange(target, modulated);
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
    std::array<const UnisonCopies *, kOps> copy;
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
            const double ratio =
                coarseRatio(p[paramIndex(i, OpParam::Coarse)]) * (1.0 + p[paramIndex(i, OpParam::Fine)]);
            const double cents = p[paramIndex(i, OpParam::Detune)];
            const double semitones = p[paramIndex(i, OpParam::Transpose)];
            hz = noteHz * ratio * std::exp2(semitones / 12.0 + cents / 1200.0);
        }
        const double inc = std::min(hz / rate_, 0.5);
        const auto shape = static_cast<OpWave>(
            stepped(p[paramIndex(i, OpParam::Wave)], static_cast<int>(OpWave::Count) - 1));
        noise[i] = shape == OpWave::Noise;

        const double velocityScale = 1.0 - p[paramIndex(i, OpParam::Velocity)] * (1.0 - v.velocity);
        ampStep[i] = (velocityScale - op.amp) * ramp;
        const std::array<double, 8> times{
            p[paramIndex(i, OpParam::Attack)],      p[paramIndex(i, OpParam::Hold)],
            p[paramIndex(i, OpParam::Decay)],       p[paramIndex(i, OpParam::Sustain)],
            p[paramIndex(i, OpParam::Release)],     p[paramIndex(i, OpParam::AttackCurve)],
            p[paramIndex(i, OpParam::DecayCurve)], p[paramIndex(i, OpParam::ReleaseCurve)]};
        rates[i] = op.rates.get(times, [&] {
            return envRates(times[0], times[1], times[2], times[3], times[4], times[5], times[6], times[7], rate_);
        });
        target.out[i] = p[paramIndex(i, OpParam::Out)];
        double heard = target.out[i];
        for (int f = 0; f < kFilters; ++f) {
            target.send[i][f] = filterEnabled[f] ? p[sendIndex(i, f)] : 0.0;
            heard = std::max(heard, target.send[i][f]);
        }
        for (int sh = 0; sh < kShapers; ++sh) {
            target.shaperSend[i][sh] = shaperEnabled[sh] ? p[shaperSendIndex(i, sh)] : 0.0;
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
        copy[i] = &op.unison.get({inc, detune, spread, static_cast<double>(copies[i])}, [&] {
            UnisonCopies c{};
            for (int k = 0; k < copies[i]; ++k) {
                const double offset = copies[i] > 1 ? 2.0 * k / (copies[i] - 1) - 1.0 : 0.0;
                c.inc[k] = std::min(inc * std::exp2(offset * detune / 1200.0), 0.5);
                const double copyAngle = (offset * spread + 1.0) * std::numbers::pi / 4.0;
                c.gainL[k] = std::numbers::sqrt2 * std::cos(copyAngle) * monoGain[i];
                c.gainR[k] = std::numbers::sqrt2 * std::sin(copyAngle) * monoGain[i];
            }
            return c;
        });
        stereo = stereo || ((copies[i] > 1 || noise[i]) && spread > 0.0 && heard > 0.0);

        // Squared amounts give finer control over the subtle low end.
        for (int to = 0; to < kOps; ++to) {
            const double amount = p[matrixIndex(i, to)];
            target.index[i][to] = amount * amount * (i == to ? kMaxFeedback : kMaxIndex) / (2.0 * std::numbers::pi);
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
        const ShaperParams fields = shaperParams(kFxSlots + sh);
        const ShaperSettings &settings = v.shaperSettings[sh].get(
            {p[fields.drive], p[fields.mode], p[fields.output], p[fields.link], p[fields.mix], p[fields.symmetric]},
            [&] { return shaperSettings([&p](int index) { return p[index]; }, fields); });
        target.shaperDrive[sh] = settings.drive;
        target.shaperOutput[sh] = settings.output;
        target.shaperMix[sh] = settings.mix;
        curve[sh] = {settings.mode, shaperTables_[sh].values.data(), settings.symmetric, shaperTables_[sh].version};
        block[sh] = clipNeedsBlocker(settings.mode, settings.symmetric);
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
    // The settings of the filters that run.
    std::array<const FilterSettings *, kFilters> filter{};
    for (int f = 0; f < kFilters; ++f) {
        if (!filterOn[f])
            continue;
        const std::array<double, 7> inputs{
            p[paramIndex(f, FilterParam::Cutoff)], p[paramIndex(f, FilterParam::KeyTrack)], pitch,
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
                    std::fill_n(combLine(v, f, c), combSize_, 0.0f);
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
        bool out = false; // whether it goes straight to the output
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
            for (int f = 0; f < kFilters; ++f)
                if (filterOn[f] && inUse(current.send[i][f], step.send[i][f]))
                    r.sends[r.sendCount++] = f;
            for (int sh = 0; sh < kShapers; ++sh)
                if (shaperOn[sh] && inUse(current.shaperSend[i][sh], step.shaperSend[i][sh]))
                    r.shaperSends[r.shaperSendCount++] = sh;
            r.out = inUse(current.out[i], step.out[i]);
        }
    }

    Levels &level = v.levels;
    double blockPeak = 0.0;
    const int scopeOp = &v == scopeVoice_ ? scope_.op : -1;
    for (uint32_t s = 0; s < frames; ++s) {
        v.fmScale += fmStep;
        v.gainL += gainLStep;
        v.gainR += gainRStep;
        v.fade = std::max(v.fade - fadeStep, 0.0);
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
            for (int k = 0; k < route.opCount; ++k) {
                const int j = route.ops[k];
                pm += level.index[j][i] * (j == i ? 0.5 * (op.out + op.prevOut) : v.ops[j].out);
            }
            for (int k = 0; k < route.filterCount; ++k)
                pm += level.filterIndex[route.filters[k]][i] * v.filterOut[route.filters[k]];
            for (int k = 0; k < route.shaperCount; ++k)
                pm += level.shaperIndex[route.shapers[k]][i] * v.shaperOut[route.shapers[k]];
            const double offset = pm * v.fmScale;

            op.amp += ampStep[i];
            const double envLevel = op.env.next(rates[i]) * op.amp;
            const double scopePhase = i == scopeOp ? op.phases[0] : 0.0; // where this sample is read
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
            } else if (envLevel == 0.0) {
                // Silent (an envelope at rest, or held or released at nothing): what it puts out is 0 whatever the
                // wave, so its copies only move on as they would have.
                for (int k = 0; k < copies[i]; ++k) {
                    op.phases[k] += copy[i]->inc[k];
                    if (op.phases[k] >= 1.0)
                        op.phases[k] -= 1.0;
                }
            } else {
                for (int k = 0; k < copies[i]; ++k) {
                    const double x = WaveTables::read(wave[i], op.phases[k] + offset);
                    mono += x;
                    l += x * copy[i]->gainL[k];
                    r += x * copy[i]->gainR[k];
                    op.phases[k] += copy[i]->inc[k];
                    if (op.phases[k] >= 1.0)
                        op.phases[k] -= 1.0;
                }
            }
            op.prevOut = op.out;
            op.out = mono * monoGain[i] * envLevel;
            if (i == scopeOp)
                scopeTake(scopePhase, op.out);
            l *= envLevel;
            r *= envLevel;
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
                y[c] = v.filters[f][c].process(toFilter[f][c], *filter[f], combLine(v, f, c), combSize_);
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
