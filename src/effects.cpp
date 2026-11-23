#include "effects.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace fmvibe {

using fx::biquad;
using fx::BiquadType;
using fx::Coefs;
using fx::crossoverCoefs;
using fx::rampEnds;
using fx::rampStep;
using fx::splitThree;

namespace {

// Louder than any sound the effects take in or put out (+120 dB): a sample past it, or one that isn't a number, has
// gone wrong somewhere.
constexpr float kBrokenLevel = 1e6f;

} // namespace

void Effects::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    rampShortest_ = fx::rampShortest(sampleRate);
    hilbert_ = fx::hilbertCoefs(sampleRate);
    for (Slot &slot : slots_) {
        slot.shaper.prepare(sampleRate);
        slot.delay.prepare(sampleRate);
        slot.filter.prepare(sampleRate);
        slot.reverb.prepare(sampleRate);
        slot.widener.prepare(sampleRate);
        slot.flanger.prepare(sampleRate);
        slot.chorus.prepare(sampleRate);
        slot.disperser.prepare(sampleRate);
        Analyzer::prepare(slot.analyzed, static_cast<int>(&slot - slots_.data()), kFxSlots + kFilters);
        if (!slot.amp)
            slot.amp = std::make_unique<fx::AmpFx>();
        slot.amp->prepare(sampleRate);
    }
    analyzer_.prepare(sampleRate);
    reverbTables_.prepare(sampleRate);
    eqCrossovers_ = fx::Eq::crossovers(sampleRate);
    if (!lags_)
        lags_ = std::make_unique<Lags>();
    for (int kind = 0; kind < FxLag::kUpDowns; ++kind)
        for (UpDown &side : lags_->fresh[kind])
            side.prepare(sampleRate, FxLag::factorOf(kind));
    // The waits' lines, two for each pool path and four for each slot's dry sound.
    const int wait = throughZeroWaitSamples(sampleRate);
    lags_->waitLines.assign(static_cast<size_t>(wait) * 2 * (kPoolPaths + 2 * kFxSlots));
    float *line = &lags_->waitLines[0];
    auto place = [&](Wait &w) {
        w = {line, wait, 0};
        line += wait;
    };
    for (auto &path : lags_->poolWait)
        for (Wait &side : path)
            place(side);
    for (SlotDry &dry : lags_->dry) { // none, to take one designed for this rate (see slotDry())
        dry.now = dry.was = {};
        dry.lag = dry.wasLag = {};
        for (Wait &side : dry.nowWait)
            place(side);
        for (Wait &side : dry.wasWait)
            place(side);
    }
    reset();
}

void Effects::reset() {
    portLevel_.fill(0.0f);
    graphParams_.fill(std::numeric_limits<double>::quiet_NaN()); // so the first chunk orders the graph
    orderStale_ = true;
    // Each effect clears its state when it fades in again.
    for (Slot &slot : slots_) {
        slot.fade = 0.0;
        slot.running = FxType::None;
    }
    tail_ = 0;
    flushing_ = false;
    clearGraph();
    for (auto &node : wires_)
        node.fill(Wire{});
    if (lags_) { // every pad's paths back in the pool, and the slots' dry sound starting again from silence
        lags_->taken.fill(false);
        for (SlotDry &dry : lags_->dry) {
            dry.wasFade = 0.0;
            dry.live = false;
        }
    }
    pads_ = {};
    running_.fill(false);
    frozen_.fill(false);
    ranBefore_.fill(false);
    graphOrderCount_ = 0; // the first chunk's nodes start with their wires as set, at once (see orderGraph())
    looped_ = rewiring_ = false;
    duck_ = 1.0;
}

void Effects::start(int slot, const fx::Context &ctx) {
    Slot &s = slots_[slot];
    switch (s.running) {
    case FxType::Shaper:
        s.shaper.start(ctx);
        break;
    case FxType::Delay:
        s.delay.start(ctx);
        break;
    case FxType::Eq:
        s.eq.start(ctx);
        break;
    case FxType::ParamEq:
        s.peq.start(ctx);
        s.analyzed.clear();
        break;
    case FxType::Filter:
        s.filter.start(ctx);
        s.analyzed.clear();
        break;
    case FxType::Compressor:
        s.comp.start(ctx);
        break;
    case FxType::MultiDyn:
        s.dyn.start(ctx);
        break;
    case FxType::Reverb:
        s.reverb.start(ctx);
        break;
    case FxType::Phaser:
        s.phaser.start(ctx);
        break;
    case FxType::Flanger:
        s.flanger.start(ctx);
        break;
    case FxType::Chorus:
        s.chorus.start(ctx);
        break;
    case FxType::FreqShifter:
        s.shifter.start(ctx);
        break;
    case FxType::Disperser:
        s.disperser.start(ctx);
        s.analyzed.clear();
        break;
    case FxType::Widener:
        s.widener.start(ctx);
        break;
    case FxType::Utility:
        s.utility.start(ctx);
        break;
    case FxType::LoFi:
        s.lofi.start(ctx);
        break;
    case FxType::Amp:
        s.amp->start(ctx);
        break;
    case FxType::MultiComp: // retired: a state's loads as a three-band dynamics (see readParams())
    case FxType::None:
    case FxType::Count:
        break;
    }
}

void Effects::process(float *left, float *right, uint32_t frames, const ParamValues &p, double tempo, double pitch) {
    // Effects moved along the line crossfade their wires, which for a moment would make a loop (two effects swapped
    // each read the other) reading a chunk late, as rough as a comb. Instead the graph plays on as it was while the
    // output ducks out over 3 ms, every wire takes its place at once, and the output comes back in over 3 ms.
    if (!rewiring_) {
        // Ordered again only when something it reads may have moved (see orderStale_); a loop found last time
        // (which rewiring then plays out) is looked at again.
        if (graphParamsChanged(p) || orderStale_ || looped_) {
            orderStale_ = false;
            orderGraph(p, true);
        }
        rewiring_ = looped_;
    }
    if (rewiring_)
        orderGraph(p, false);
    const double duckStep = 1.0 / (0.003 * sampleRate_);
    for (uint32_t done = 0; done < frames; done += kChunk) {
        const uint32_t n = std::min(kChunk, frames - done);
        float *const io[2] = {left + done, right + done};
        // A sample that isn't a number, or is infinite or louder than any sound (see kBrokenLevel), would ring on in
        // every line and feedback it reaches, for good: it goes in as silence.
        for (int c = 0; c < 2; ++c)
            for (uint32_t s = 0; s < n; ++s)
                if (!(std::abs(io[c][s]) < kBrokenLevel))
                    io[c][s] = 0.0f;
        // Silence, with nothing left ringing and nothing on its way: it would come out as silence, so it does without
        // the effects' work (a host that keeps processing a quiet instance, as VST3 hosts do, pays nothing for them).
        if (idleChunk(io, n, p)) {
            idleSweeps(n, p, tempo);
            idleAnalyzers(n);
            measurePorts(n, true);
            continue;
        }
#ifdef __EMSCRIPTEN__
        // WebAssembly can't flush denormals to zero as the plugin's process() does, and tails decaying into them
        // cost many times the CPU. A signal far below hearing (-300 dB) keeps every state above them: a pulse every
        // fourth sample, whose low, middle and top frequencies no effect's filters take out all at once.
        for (uint32_t s = 0; s < n; s += 4) {
            io[0][s] += 1e-15f;
            io[1][s] += 1e-15f;
        }
#endif
        int64_t keep = 0; // how long this chunk keeps the effects going
        for (uint32_t s = 0; s < n && keep == 0; ++s)
            if (std::abs(io[0][s]) > 1e-6f || std::abs(io[1][s]) > 1e-6f)
                keep = static_cast<int64_t>(0.05 * sampleRate_); // filter and DC blocker tails

        processGraph(io, n, p, tempo, pitch, keep);
        measurePorts(n);
        if (rewiring_ || duck_ < 1.0) {
            for (uint32_t s = 0; s < n; ++s) {
                duck_ = rewiring_ ? std::max(duck_ - duckStep, 0.0) : std::min(duck_ + duckStep, 1.0);
                const float gain = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * duck_));
                io[0][s] *= gain;
                io[1][s] *= gain;
            }
            if (rewiring_ && duck_ <= 0.0) { // silent: the wires as set, at once
                for (int node = 0; node < kFxNodes; ++node)
                    if (ranBefore_[node])
                        setWires(node, p);
                rewiring_ = false;
                orderGraph(p, true);
            }
        }
        // One that comes out all the same, from inside an effect, has filled its state: every effect starts again
        // cleared, and the chunk is silence.
        bool sane = true;
        for (uint32_t s = 0; s < n && sane; ++s)
            sane = std::abs(io[0][s]) < kBrokenLevel && std::abs(io[1][s]) < kBrokenLevel;
        if (!sane) {
            std::fill_n(io[0], n, 0.0f);
            std::fill_n(io[1], n, 0.0f);
            reset();
            continue;
        }
        // Whatever still comes out keeps them going too: a DC blocker settling, an EQ's ring, which no check above
        // covers. Stopping there would step the output to silence.
        for (uint32_t s = 0; s < n && keep == 0; ++s)
            if (std::abs(io[0][s]) > 1e-6f || std::abs(io[1][s]) > 1e-6f)
                keep = static_cast<int64_t>(0.05 * sampleRate_);
        tail_ = std::max(tail_ - static_cast<int64_t>(n), keep);
        // Flushed, once every effect has faded out: they start again cleared, and so does what the graph holds.
        if (flushing_ && std::all_of(slots_.begin(), slots_.end(), [](const Slot &s) { return s.fade <= 0.0; })) {
            flushing_ = false;
            clearGraph();
        }
    }
}

bool Effects::idleChunk(const float *const *io, uint32_t n, const ParamValues &p) const {
    if (tail_ > 0 || flushing_ || rewiring_ || duck_ < 1.0)
        return false;
    for (uint32_t s = 0; s < n; ++s)
        if (io[0][s] != 0.0f || io[1][s] != 0.0f)
            return false;
    auto wiresSettled = [&](int node) {
        for (int input = 0; input < fxInputCount(node); ++input) {
            const Wire &w = wires_[node][input];
            const int set = wirePort(node, input, p);
            if (w.now.port != set || w.was.port >= 0 || (set >= 0 && w.now.pad != pads_[node][input]) ||
                (set >= 0 && (w.now.fade < 1.0 || w.now.level != wireTarget(node, input, p))))
                return false;
        }
        return true;
    };
    for (int i = 0; i < graphOrderCount_; ++i) {
        const int node = graphOrder_[i];
        if (frozen_[node] || !wiresSettled(node))
            return false;
        if (node < kFxSlots) { // fully in while on, fully out while off, as runSlot() takes it
            const Slot &fx = slots_[node];
            const FxType type = fxType(p[paramIndex(node, FxParam::Type)]);
            const FxType running = fx.running != type && fx.fade <= 0.0 ? type : fx.running;
            const bool on = running == type && type != FxType::None && p[paramIndex(node, FxParam::Enabled)] >= 0.5;
            if (on ? fx.fade < 1.0 : fx.fade > 0.0)
                return false;
        } else if (fxNodeSum(node) < 0) { // a split at its slope
            const int split = node - kFxSlots;
            const Split &sp = splits_[split];
            const bool three = std::lround(p[paramIndex(split, SplitParam::Bands)]) >= kFxBands;
            const int asked = slopeDb(filterSlope(p[paramIndex(split, SplitParam::Slope)]));
            const int mode = three ? (asked > 12 ? kThree24 : kThree12) : asked > 12 ? 24 : asked;
            if (!sp.running || sp.wasFade > 0.0 || sp.now.mode != mode)
                return false;
        }
    }
    return wiresSettled(kFxOutNode);
}

void Effects::idleSweeps(uint32_t n, const ParamValues &p, double tempo) {
    for (int i = 0; i < graphOrderCount_; ++i) {
        const int slot = graphOrder_[i];
        if (slot >= kFxSlots || slots_[slot].fade <= 0.0)
            continue;
        Slot &fx = slots_[slot];
        const FxType type = fx.running;
        if (type != FxType::Phaser && type != FxType::Flanger && type != FxType::Chorus)
            continue;
        const double cycles = sweepHz([&p](int k) { return p[k]; }, slot, type, tempo) / sampleRate_ * n;
        if (type == FxType::Phaser)
            fx.phaser.idle(cycles);
        else if (type == FxType::Chorus)
            fx.chorus.idle(cycles);
        else
            fx.flanger.idle(cycles);
    }
}

void Effects::idleAnalyzers(uint32_t n) {
    if (!analyzing_)
        return;
    float silence[2][kChunk]{};
    float *const io[2] = {silence[0], silence[1]};
    for (int i = 0; i < graphOrderCount_; ++i) {
        const int slot = graphOrder_[i];
        if (slot >= kFxSlots || slots_[slot].fade <= 0.0)
            continue;
        const FxType type = slots_[slot].running;
        if (type == FxType::ParamEq || type == FxType::Filter || type == FxType::Disperser)
            feedAnalyzer(slot, io, n);
    }
}

void Effects::runSlot(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo, double pitch,
                      int64_t &keep) {
    const double fadeStep = 1.0 / (0.005 * sampleRate_);
    Slot &fx = slots_[slot];
    const FxType type = fxType(p[paramIndex(slot, FxParam::Type)]);
    if (fx.running != type && fx.fade <= 0.0)
        fx.running = type;
    const bool on = !flushing_ && fx.running == type && type != FxType::None &&
                    p[paramIndex(slot, FxParam::Enabled)] >= 0.5;
    // What went in, as late as the effect makes its wet sound: for fading, passing it while off, and an amp's Mix.
    const bool ampMix = fx.running == FxType::Amp && fx.fade > 0.0 &&
                        (fx.amp->mix < 1.0 || p[paramIndex(slot, FxParam::AmpMix)] < 1.0);
    float dry[2][kChunk];
    const int dryWait = slotDry(slot, io, dry, n, p, !(on && fx.fade >= 1.0) || ampMix);
    if (!on && fx.fade <= 0.0) {
        fx.prime = 0; // started again from the start
        reduction_[slot] = {};
        bandLevel_[slot] = {};
        bandGain_[slot] = {};
        fx.analyzed.bands = {};
        std::copy_n(dry[0], n, io[0]);
        std::copy_n(dry[1], n, io[1]);
        return;
    }
    const auto ctx = context(slot, p, tempo, pitch); // an fx::Context (`fx` here is the slot)
    if (fx.fade <= 0.0 && fx.prime <= 0) {
        start(slot, ctx);
        // A flanger through zero puts out nothing until its dry sound's wait has filled.
        fx.prime = fxSlotWaits([&p](int i) { return p[i]; }, slot) ? throughZeroWaitSamples(sampleRate_) : 0;
    }
    // Each type's run, by the type it processes as: a static dispatch, every type's state held in the slot.
    switch (fx.running) {
    case FxType::Shaper:
        fx.shaper.run(ctx, io, n);
        break;
    case FxType::Delay:
        // While anything goes in or comes out, another echo may follow.
        if (fx.delay.run(ctx, io, n) > 1e-5)
            keep = std::max(keep, static_cast<int64_t>(std::max(fx.delay.length[0], fx.delay.length[1]) +
                                                       0.05 * sampleRate_));
        break;
    case FxType::Eq:
        fx.eq.run(ctx, eqCrossovers_, io, n);
        break;
    case FxType::ParamEq:
        fx.peq.run(ctx, io, n);
        feedAnalyzer(slot, io, n);
        break;
    case FxType::Filter:
        // A resonant comb rings on; keep going while it does, also through the gap between two of its pulses, which
        // is as long as its line at the most.
        if (fx.filter.run(ctx, io, n) > 1e-5)
            keep = std::max(keep, static_cast<int64_t>(0.05 * sampleRate_) + combLength(sampleRate_));
        feedAnalyzer(slot, io, n);
        break;
    case FxType::Compressor:
    case FxType::MultiDyn:
        // A gain still releasing keeps them going, so the next sound doesn't start where it was left.
        if (fx.running == FxType::Compressor ? fx.comp.run(ctx, io, n, meters(slot))
                                             : fx.dyn.run(ctx, io, n, meters(slot)))
            keep = std::max(keep, static_cast<int64_t>(0.05 * sampleRate_));
        break;
    case FxType::Amp:
        fx.amp->run(ctx, io, n, dry, meters(slot));
        break;
    case FxType::Utility:
        fx.utility.run(ctx, io, n);
        break;
    case FxType::LoFi:
        fx.lofi.run(ctx, io, n);
        break;
    case FxType::Reverb:
    case FxType::Phaser:
    case FxType::Flanger:
    case FxType::Chorus:
    case FxType::FreqShifter:
    case FxType::Disperser:
    case FxType::Widener: {
        // Tails and feedback ring on; keep going while they do. A reverb's input may still wait in its
        // pre-delay, as a delay's in its lines.
        const double peak = fx.running == FxType::Reverb      ? fx.reverb.run(ctx, reverbTables_, io, n)
                            : fx.running == FxType::Phaser    ? fx.phaser.run(ctx, io, n)
                            : fx.running == FxType::Flanger   ? fx.flanger.run(ctx, io, n)
                            : fx.running == FxType::Chorus    ? fx.chorus.run(ctx, io, n)
                            : fx.running == FxType::Disperser ? fx.disperser.run(ctx, io, n)
                            : fx.running == FxType::Widener  ? fx.widener.run(ctx, io, n)
                                                              : fx.shifter.run(ctx, hilbert_, io, n);
        const double waiting = fx.running == FxType::Reverb ? fx.reverb.reach : 0.0;
        if (peak > 1e-5)
            keep = std::max(keep, static_cast<int64_t>(waiting + 0.05 * sampleRate_));
        if (fx.running == FxType::Disperser)
            feedAnalyzer(slot, io, n);
        break;
    }
    case FxType::MultiComp: // retired, as in start()
    case FxType::None:
    case FxType::Count:
        break;
    }
    if (on && fx.fade >= 1.0)
        return;
    orderStale_ = true; // fading: an effect taken away is in the graph until it has faded out (see nodeWanted())
    for (uint32_t s = 0; s < n; ++s) {
        if (on && fx.prime > 0)
            --fx.prime; // its wet sound still silent: the dry one goes on
        else if (!on && static_cast<int>(s) < dryWait)
            ; // its dry sound still silent (a new path's wait filling): the wet one goes on
        else
            fx.fade = std::clamp(fx.fade + (on ? fadeStep : -fadeStep), 0.0, 1.0);
        for (int c = 0; c < 2; ++c)
            io[c][s] = static_cast<float>(dry[c][s] + (io[c][s] - dry[c][s]) * fx.fade);
    }
}

bool Effects::nodeWanted(int node, const ParamValues &p) const {
    if (node == kFxOutNode)
        return true;
    if (node < kFxSlots) // an effect fading out still runs
        return fxType(p[paramIndex(node, FxParam::Type)]) != FxType::None || slots_[node].fade > 0.0;
    if (const int sum = fxNodeSum(node); sum >= 0)
        return p[paramIndex(sum, SumParam::Used)] >= 0.5;
    return p[paramIndex(node - kFxSlots, SplitParam::Used)] >= 0.5;
}

int Effects::wirePort(int node, int input, const ParamValues &p) const {
    const int port = fxPortOf(p[fxInputIndex(node, input, FxInputField::From)]);
    if (port < 0)
        return -1;
    const int from = fxPortNode(port); // none for the input, which is always there
    return from < 0 || nodeWanted(from, p) ? port : -1;
}

double Effects::wireTarget(int node, int input, const ParamValues &p) const {
    const double set = p[fxInputIndex(node, input, FxInputField::Level)];
    const int sum = fxNodeSum(node);
    const double gain = sum >= 0 ? sumLevelGain(set) * sumSoloGain(sum, input, [&p](int i) { return p[i]; })
                                 : set; // a sum's in dB, and soloed as a join
    return std::isfinite(gain) ? std::clamp(gain, 0.0, 1.0) : 0.0;
}

void Effects::setWires(int node, const ParamValues &p) {
    orderStale_ = true;
    for (int input = 0; input < fxInputCount(node); ++input) {
        const int port = wirePort(node, input, p);
        Wire &w = wires_[node][input];
        dropEnd(w.was);
        w.was = {};
        setEnd(w.now, port, wireTarget(node, input, p), port >= 0 ? 1.0 : 0.0,
               port >= 0 ? pads_[node][input] : FxLag{});
    }
}

void Effects::dropEnd(WireEnd &e) {
    for (int k = 0; k < e.paths && lags_; ++k)
        lags_->taken[e.path[k]] = false;
    e.paths = 0;
}

int Effects::freePaths() const {
    return lags_ ? static_cast<int>(std::count(lags_->taken.begin(), lags_->taken.end(), false)) : 0;
}

void Effects::setEnd(WireEnd &e, int port, double level, double fade, const FxLag &pad) {
    dropEnd(e);
    e.port = port;
    e.level = level;
    e.fade = fade;
    e.prime = 0;
    e.pad = port >= 0 ? pad : FxLag{};
    if (!lags_ || e.pad.none() || e.pad.count() > kEndPaths)
        return;
    // All of its paths or none: one put through only some of them would be no nearer in step at the top.
    int found = 0;
    for (int i = 0; i < kPoolPaths && found < e.pad.count(); ++i)
        if (!lags_->taken[i])
            e.path[found++] = static_cast<uint8_t>(i);
    if (found < e.pad.count())
        return;
    int k = 0;
    for (int kind = 0; kind < FxLag::kKinds; ++kind)
        for (int j = 0; j < e.pad.paths[kind]; ++j) {
            const int i = e.path[k++];
            lags_->taken[i] = true;
            lags_->kind[i] = static_cast<uint8_t>(kind);
            if (kind == FxLag::kWait) {
                for (Wait &side : lags_->poolWait[i])
                    side.reset();
                e.prime = std::max(e.prime, lags_->poolWait[i][0].size); // silent until it has filled
            } else {
                lags_->pool[i] = lags_->fresh[kind]; // cleared, at that factor
            }
        }
    e.paths = found;
}

void Effects::padEnd(const WireEnd &e, const float *const *from, float (*out)[kChunk], uint32_t n) {
    for (int c = 0; c < 2; ++c) {
        if (from[c])
            std::copy_n(from[c], n, out[c]);
        else
            std::fill_n(out[c], n, 0.0f); // a node not running: its silence, through the pad as anything else
        for (int k = 0; k < e.paths; ++k) {
            const int i = e.path[k];
            if (lags_->kind[i] == FxLag::kWait)
                lags_->poolWait[i][c].run(out[c], static_cast<int>(n));
            else
                lags_->pool[i][c].run(out[c], static_cast<int>(n));
        }
    }
}

int Effects::slotDry(int slot, const float *const *io, float (*dry)[kChunk], uint32_t n, const ParamValues &p,
                     bool needed) {
    std::copy_n(io[0], n, dry[0]);
    std::copy_n(io[1], n, dry[1]);
    SlotDry &d = lags_->dry[slot];
    const auto get = [&p](int i) { return p[i]; };
    // Another path: the one it had fades out.
    if (const FxLag lag = fxSlotLag(get, slot); lag != d.lag && d.wasFade <= 0.0) {
        d.was = d.now;
        std::swap(d.wasWait, d.nowWait);
        d.wasLag = d.lag;
        d.lag = lag;
        const int factor = fxSlotFactor(get, slot);
        for (int c = 0; c < 2; ++c)
            d.now[c] = factor >= 2 ? lags_->fresh[FxLag::kindOf(factor)][c] : UpDown{};
        for (Wait &side : d.nowWait)
            side.reset();
        d.prime = d.lag.paths[FxLag::kWait] ? d.nowWait[0].size : 0;
        d.wasFade = d.live ? 1.0 : 0.0;
    }
    int prime = d.prime; // of this chunk's samples, how many the wait has yet to fill
    d.prime = std::max(d.prime - static_cast<int>(n), 0);
    if (!needed) {
        // The effect fully in: its dry sound isn't heard, and starts again from silence when it is. All but a wait,
        // which goes on filling: as long as the effect would fade out in, it would leave a gap to fade into.
        if (d.lag.paths[FxLag::kWait])
            for (int c = 0; c < 2; ++c) {
                float waited[kChunk];
                std::copy_n(io[c], n, waited);
                d.nowWait[c].run(waited, static_cast<int>(n));
            }
        d.live = false;
        d.wasFade = 0.0;
        return prime;
    }
    if (!d.live) {
        for (UpDown &side : d.now)
            side.reset();
        d.live = true;
    }
    float was[2][kChunk];
    const bool crossing = d.wasFade > 0.0;
    for (int c = 0; c < 2; ++c) {
        if (crossing) {
            std::copy_n(dry[c], n, was[c]);
            d.was[c].run(was[c], static_cast<int>(n));
            if (d.wasLag.paths[FxLag::kWait])
                d.wasWait[c].run(was[c], static_cast<int>(n));
        }
        d.now[c].run(dry[c], static_cast<int>(n));
        if (d.lag.paths[FxLag::kWait])
            d.nowWait[c].run(dry[c], static_cast<int>(n));
    }
    if (!crossing)
        return prime;
    const double fadeStep = 1.0 / (0.005 * sampleRate_);
    for (uint32_t s = 0; s < n; ++s) {
        if (prime > 0)
            --prime; // the new path's wait still silent: the one it had goes on
        else
            d.wasFade = std::max(d.wasFade - fadeStep, 0.0);
        const float w = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * d.wasFade));
        for (int c = 0; c < 2; ++c)
            dry[c][s] += (was[c][s] - dry[c][s]) * w;
    }
    return 0; // the path it had heard meanwhile
}

void Effects::idleSlot(int slot) {
    slots_[slot].fade = 0.0; // so start() clears it when it runs again
    if (lags_) { // and its dry sound: what its wait held is long gone
        SlotDry &d = lags_->dry[slot];
        d.live = false;
        for (Wait &side : d.nowWait)
            side.reset();
        d.prime = 0;
    }
    reduction_[slot] = {};
    bandLevel_[slot] = {};
    bandGain_[slot] = {};
    slots_[slot].analyzed.bands = {};
}

bool Effects::graphParamsChanged(const ParamValues &p) {
    bool changed = false;
    int k = 0;
    auto take = [&](double value) {
        // Compared as stored (NaN to begin with never equals): a change is copied in.
        if (!(graphParams_[k] == value)) {
            graphParams_[k] = value;
            changed = true;
        }
        ++k;
    };
    for (int node = 0; node < kFxNodes; ++node)
        for (int input = 0; input < fxInputCount(node); ++input)
            take(p[fxInputIndex(node, input, FxInputField::From)]);
    for (int slot = 0; slot < kFxSlots; ++slot) {
        take(p[paramIndex(slot, FxParam::Type)]);
        take(p[paramIndex(slot, FxParam::AmpOversampling)]);
        take(p[paramIndex(slot, FxParam::ShaperOversampling)]);
        take(p[paramIndex(slot, FxParam::FlangerThroughZero)]);
    }
    for (int split = 0; split < kFxSplits; ++split)
        take(p[paramIndex(split, SplitParam::Used)]);
    for (int sum = 0; sum < kFxSums; ++sum)
        take(p[paramIndex(sum, SumParam::Used)]);
    return changed;
}

bool Effects::loopsAsSet(const ParamValues &p) const {
    // Depth first along the wires as set from every node in the graph: back to a node on the way is a loop.
    std::array<uint8_t, kFxNodes> state{}; // 0 not reached, 1 on the way, 2 done
    std::array<std::pair<int, int>, kFxNodes> stack;
    for (int start = 0; start < kFxNodes; ++start) {
        if (state[start] != 0 || !nodeWanted(start, p))
            continue;
        int depth = 0;
        stack[depth++] = {start, 0};
        state[start] = 1;
        while (depth > 0) {
            auto &[node, input] = stack[depth - 1];
            if (input >= fxInputCount(node)) {
                state[node] = 2;
                --depth;
                continue;
            }
            const int port = fxPortOf(p[fxInputIndex(node, input++, FxInputField::From)]);
            const int from = port >= 0 ? fxPortNode(port) : -1;
            if (from < 0 || !nodeWanted(from, p))
                continue;
            if (state[from] == 1)
                return true;
            if (state[from] == 0) {
                state[from] = 1;
                stack[depth++] = {from, 0};
            }
        }
    }
    return false;
}

void Effects::orderGraph(const ParamValues &p, bool asSet) {
    // Back from OUT along the wires into each node, as set or still fading, depth first: each node runs after what
    // goes into it, but where a wire closes a loop, which reads the chunk before. A wire as set comes from a node only
    // while that node is in the graph (see nodeWanted()); one still fading out keeps the node it fades out of running,
    // `frozen` while no wire as set reads it, so a node leaves the graph without a click. The nodes OUT doesn't hear
    // don't run at all. Not `asSet`, only along the wires as they play, every node frozen: the graph as it was.
    // As set, a loop that only wires moving make (see process()) leaves everything as it was.
    std::array<int8_t, kFxNodes> wanted;
    wanted.fill(-1);
    auto wants = [&](int node) {
        if (wanted[node] < 0)
            wanted[node] = nodeWanted(node, p) ? 1 : 0;
        return wanted[node] == 1;
    };
    std::array<uint8_t, kFxNodes> state{}; // 0 not reached, 1 on the way, 2 done
    looped_ = false;
    std::array<bool, kFxNodes> readAsSet{};
    readAsSet[kFxOutNode] = true;
    graphOrderCount_ = 0;
    // A node, and the next of its wires' ends to look along: input * 3, then the end as set, now and was.
    std::array<std::pair<int, int>, kFxNodes> stack;
    int depth = 0;
    stack[depth++] = {kFxOutNode, 0};
    state[kFxOutNode] = 1;
    while (depth > 0) {
        auto &[node, next] = stack[depth - 1];
        const int ends = fxInputCount(node) * 3;
        int deeper = -1;
        while (next < ends && deeper < 0) {
            const int input = next / 3, end = next % 3;
            ++next;
            const Wire &w = wires_[node][input];
            int port = -1;
            const bool fading = end > 0;
            if (!fading) {
                if (asSet && wants(node))
                    port = fxPortOf(p[fxInputIndex(node, input, FxInputField::From)]);
            } else if (const WireEnd &e = end == 1 ? w.now : w.was; e.fade > 0.0) {
                port = e.port;
            }
            const int from = port >= 0 ? fxPortNode(port) : -1;
            if (from < 0 || (!fading && !wants(from)))
                continue;
            if (!fading)
                readAsSet[from] = true;
            if (state[from] == 0)
                deeper = from;
            else if (state[from] == 1) // back to a node on the way: a loop, which only wires moving make
                looped_ = true;
        }
        if (deeper >= 0) {
            state[deeper] = 1;
            stack[depth++] = {deeper, 0};
            continue;
        }
        state[node] = 2;
        if (node != kFxOutNode)
            graphOrder_[graphOrderCount_++] = node;
        --depth;
    }
    // How late each path is as set, and what each wire is to be put through to meet the others into its node in step
    // (see fxlag.h), which mixInputs() then crossfades each wire to.
    if (asSet)
        fxGraphLags([&p](int i) { return p[i]; }, wants, &pads_);
    if (looped_ && asSet && loopsAsSet(p)) // a loop as set (which the editor doesn't make) plays a chunk late
        looped_ = false;
    if (looped_ && asSet)
        return;
    for (int node = 0; node < kFxNodes; ++node) {
        const bool runs = state[node] == 2;
        running_[node] = runs;
        frozen_[node] = runs && !readAsSet[node];
        if (runs && !ranBefore_[node]) {
            // Nothing heard it before: its wires start as set, as the wires that now read it fade in.
            orderStale_ = true; // as it changes the wires, once more next time
            setWires(node, p);
        } else if (!runs && ranBefore_[node]) { // nothing hears it any more
            orderStale_ = true; // as it changes the wires, once more next time
            for (Wire &w : wires_[node]) {
                dropEnd(w.now);
                dropEnd(w.was);
            }
            wires_[node].fill(Wire{});
            if (node < kFxSlots)
                idleSlot(node);
            else if (fxNodeSplit(node))
                splits_[node - kFxSlots].running = false;
        }
        ranBefore_[node] = runs;
    }
}

bool Effects::portOut(int port, const float *(&out)[2]) const {
    const std::array<std::array<float, kChunk>, 2> *from = nullptr;
    if (port == kFxPortIn) {
        from = &input_;
    } else if (const int node = port >= 0 ? fxPortNode(port) : -1; node < 0 || !running_[node]) {
        return false;
    } else if (const int slot = fxPortSlot(port); slot >= 0) {
        from = &slotOut_[slot];
    } else if (const int sum = fxPortSum(port); sum >= 0) {
        from = &sumOut_[sum];
    } else {
        from = &bandOut_[fxPortSplit(port)][fxPortBand(port)];
    }
    out[0] = (*from)[0].data();
    out[1] = (*from)[1].data();
    return true;
}

void Effects::mixInputs(int node, float (*mix)[kChunk], uint32_t n, const ParamValues &p, bool frozen) {
    for (int c = 0; c < 2; ++c)
        std::fill_n(mix[c], n, 0.0f);
    const double fadeStep = 1.0 / (0.005 * sampleRate_);
    float padded[2][kChunk]; // what an end with a pad reads, through it
    // One end of a wire into the mix, its level gliding to `target` across the chunk (or, in a chunk shorter than the
    // shortest ramp, that far toward it) as it fades toward `fadeTo` along a raised cosine over 5 ms, as a
    // parametric EQ band switched does.
    auto add = [&](WireEnd &e, double target, double fadeTo) {
        if (e.port < 0)
            return;
        if (e.fade <= 0.0 && fadeTo <= 0.0) { // out: free for the next move
            orderStale_ = orderStale_ || e.port >= 0;
            setEnd(e, -1, 0.0, 0.0, {});
            return;
        }
        if (e.level < 0.0) // takes its level at once as it fades in
            e.level = target;
        const float *from[2] = {nullptr, nullptr};
        bool heard = portOut(e.port, from); // a node not running reads silence
        if (e.paths > 0) { // through its pad, which runs on silence too
            const float *const none[2] = {nullptr, nullptr};
            padEnd(e, heard ? from : none, padded, n);
            from[0] = padded[0];
            from[1] = padded[1];
            heard = true;
        }
        if (e.prime > 0) { // its pad's wait still filling: it waits to fade in, as long as this chunk is silent
            fadeTo = std::min(fadeTo, e.fade);
            e.prime = std::max(e.prime - static_cast<int>(n), 0);
        }
        const double step = rampStep(e.level, target, n, rampShortest_);
        const double end = rampEnds(e.level, target, n, rampShortest_) ? target : e.level + step * n;
        double level = e.level, fade = e.fade;
        if (fade >= 1.0 && fadeTo >= 1.0) { // in: only its level moves
            for (uint32_t s = 0; s < n && heard; ++s) {
                level += step;
                mix[0][s] += static_cast<float>(from[0][s] * level);
                mix[1][s] += static_cast<float>(from[1][s] * level);
            }
        } else {
            for (uint32_t s = 0; s < n; ++s) {
                level += step;
                fade = fadeTo > fade ? std::min(fade + fadeStep, fadeTo) : std::max(fade - fadeStep, fadeTo);
                if (!heard)
                    continue;
                const double gain = level * (0.5 - 0.5 * std::cos(std::numbers::pi * fade));
                mix[0][s] += static_cast<float>(from[0][s] * gain);
                mix[1][s] += static_cast<float>(from[1][s] * gain);
            }
        }
        e.level = end;
        orderStale_ = orderStale_ || fade != e.fade; // a fading end keeps what it comes from running
        e.fade = fade;
    };
    for (int input = 0; input < fxInputCount(node); ++input) {
        Wire &w = wires_[node][input];
        if (frozen) { // as it was, until what reads it has faded out
            add(w.now, std::max(w.now.level, 0.0), w.now.fade);
            add(w.was, std::max(w.was.level, 0.0), w.was.fade);
            continue;
        }
        // A wire moved or taken away crossfades: what it came from fades out as what it comes from now fades in. Moved
        // again while the one before still fades out, it fades out where it is first and only then takes the move. A
        // new pad (the paths into the node no longer as late as they were) crossfades the same way, and so does one
        // that went without its paths once the pool has them.
        const int set = wirePort(node, input, p);
        const FxLag pad = set >= 0 ? pads_[node][input] : FxLag{};
        const bool unpadded = !w.now.padded() && w.now.pad == pad && pad.count() <= kEndPaths &&
                              freePaths() >= pad.count();
        if (set != w.now.port || pad != w.now.pad || unpadded) {
            orderStale_ = true;
            if (w.now.fade <= 0.0) {
                setEnd(w.now, set, -1.0, 0.0, pad);
            } else if (w.was.fade <= 0.0) {
                dropEnd(w.was);
                w.was = w.now;
                w.now.paths = 0; // its pad's paths go with it, to `was`
                setEnd(w.now, set, -1.0, 0.0, pad);
            }
        }
        const bool asSet = w.now.port == set && w.now.pad == pad;
        const bool priming = w.now.prime > 0; // what it came from goes on until what it comes from is heard
        add(w.now, asSet ? wireTarget(node, input, p) : w.now.level, asSet ? 1.0 : 0.0);
        add(w.was, w.was.level, priming ? w.was.fade : 0.0);
    }
}

void Effects::splitCoefs(SplitFilters &f, double lowNorm, double highNorm) const {
    // Worked out again only as the crossovers move.
    if (lowNorm == f.lowNorm && highNorm == f.highNorm)
        return;
    f.lowNorm = lowNorm;
    f.highNorm = highNorm;
    const std::array<double, 2> hz = splitCrossovers(lowNorm, highNorm);
    if (f.mode == kThree24) {
        f.coefs = crossoverCoefs(hz[0], hz[1], sampleRate_);
        return;
    }
    // One-poles at `hz`: a low-pass, a high-pass and the all-pass the two make together (low less high).
    auto onePoles = [this](double hz, BiquadCoefs &low, BiquadCoefs &high, BiquadCoefs *all) {
        const double k = std::tan(std::numbers::pi * hz / sampleRate_);
        const double a = (k - 1.0) / (k + 1.0);
        low = {k / (1.0 + k), k / (1.0 + k), 0.0, a, 0.0};
        high = {1.0 / (1.0 + k), -1.0 / (1.0 + k), 0.0, a, 0.0};
        if (all)
            *all = {a, 1.0, 0.0, a, 0.0};
    };
    if (f.mode == kThree12) { // kept as crossoverCoefs() keeps them
        const double low = std::min(hz[0], 0.3 * sampleRate_);
        const double high = std::clamp(hz[1], low * 1.26, 0.45 * sampleRate_);
        onePoles(low, f.onePoleLow, f.onePoleHigh, nullptr);
        onePoles(high, f.onePoleLow2, f.onePoleHigh2, &f.allPass2);
        return;
    }
    const double low = std::min(hz[0], 0.3 * sampleRate_); // as crossoverCoefs() keeps it
    if (f.mode == 24) {
        f.coefs.lp1 = biquadCoefs(biquad(BiquadType::LowPass, low, sampleRate_));
        f.coefs.hp1 = biquadCoefs(biquad(BiquadType::HighPass, low, sampleRate_));
        return;
    }
    onePoles(low, f.onePoleLow, f.onePoleHigh, nullptr);
}

void Effects::splitBands(SplitFilters &f, int ch, double x, double *bands) {
    // Two bands' slope: 6 dB a one-pole low-pass with the rest as the high band; 12 dB a second-order Linkwitz-Riley
    // (two one-poles each way, the high band turned over so the two sum flat); 24 dB the fourth-order one, as three.
    EqChannel &e = f.channels[ch];
    if (f.mode == kThree24) {
        splitThree(e, x, f.coefs, bands);
        return;
    }
    if (f.mode == kThree12) {
        // Three at 12 dB: the second-order crossover at the low crossover, then again at the high one on what is above
        // it, each high side turned over. The middle and high bands sum to the high crossover's all-pass, so the low band
        // goes through it too: the three sum to the two all-passes, flat.
        bands[0] = e.lowPhase.process(e.lowB.process(e.lowA.process(x, f.onePoleLow), f.onePoleLow), f.allPass2);
        const double rest = -e.restB.process(e.restA.process(x, f.onePoleHigh), f.onePoleHigh);
        bands[1] = e.midB.process(e.midA.process(rest, f.onePoleLow2), f.onePoleLow2);
        bands[2] = -e.highB.process(e.highA.process(rest, f.onePoleHigh2), f.onePoleHigh2);
        return;
    }
    if (f.mode == 6) {
        bands[0] = e.lowA.process(x, f.onePoleLow);
        bands[2] = x - bands[0];
    } else if (f.mode == 12) {
        bands[0] = e.lowB.process(e.lowA.process(x, f.onePoleLow), f.onePoleLow);
        bands[2] = -e.restB.process(e.restA.process(x, f.onePoleHigh), f.onePoleHigh);
    } else {
        bands[0] = e.lowB.process(e.lowA.process(x, f.coefs.lp1), f.coefs.lp1);
        bands[2] = e.restB.process(e.restA.process(x, f.coefs.hp1), f.coefs.hp1);
    }
    bands[1] = 0.0; // the middle one silent
}

void Effects::runSplit(int split, uint32_t n, const ParamValues &p, const float (*mix)[kChunk]) {
    Split &sp = splits_[split];
    const bool three = std::lround(p[paramIndex(split, SplitParam::Bands)]) >= kFxBands;
    const int asked = slopeDb(filterSlope(p[paramIndex(split, SplitParam::Slope)]));
    // 18 plays as 24; three bands part at 12 or 24 dB (6 plays as 12).
    const int mode = three ? (asked > 12 ? kThree24 : kThree12) : asked > 12 ? 24 : asked;
    if (!sp.running) { // taken into the graph: it starts cleared
        sp.now = {};
        sp.now.mode = mode;
        sp.wasFade = 0.0;
        sp.running = true;
    } else if (mode != sp.now.mode && sp.wasFade <= 0.0) {
        // Another slope: fresh filters at it fade in as those at the one it leaves fade out. Changed again meanwhile,
        // it waits for that to end.
        sp.was = sp.now;
        sp.wasFade = 1.0;
        sp.now = {};
        sp.now.mode = mode;
    }
    const double low = p[paramIndex(split, SplitParam::Low)], high = p[paramIndex(split, SplitParam::High)];
    splitCoefs(sp.now, low, high);
    if (sp.wasFade > 0.0)
        splitCoefs(sp.was, low, high);
    const double fadeStep = 1.0 / (0.005 * sampleRate_);
    auto &out = bandOut_[split];
    // From two bands to three or back, the middle band comes or goes with its wire into the join, which fades it as
    // this crossfade would: it plays at full from the filters that have it (two bands' is silent), faded once only.
    auto isThree = [](int m) { return m == kThree12 || m == kThree24; };
    const bool midByWire = sp.wasFade > 0.0 && isThree(sp.now.mode) != isThree(sp.was.mode);
    for (uint32_t s = 0; s < n; ++s) {
        const bool crossing = sp.wasFade > 0.0;
        double before = 0.0; // how much of the filters it leaves is heard
        if (crossing) {
            sp.wasFade = std::max(sp.wasFade - fadeStep, 0.0);
            before = 0.5 - 0.5 * std::cos(std::numbers::pi * sp.wasFade);
        }
        for (int ch = 0; ch < 2; ++ch) {
            double bands[kFxBands];
            splitBands(sp.now, ch, mix[ch][s], bands);
            if (crossing) {
                double was[kFxBands];
                splitBands(sp.was, ch, mix[ch][s], was);
                for (int b = 0; b < kFxBands; ++b)
                    bands[b] = b == 1 && midByWire ? bands[b] + was[b] : bands[b] + (was[b] - bands[b]) * before;
            }
            for (int b = 0; b < kFxBands; ++b)
                out[b][ch][s] = static_cast<float>(bands[b]);
        }
    }
}

void Effects::processGraph(float *const *io, uint32_t n, const ParamValues &p, double tempo, double pitch,
                           int64_t &keep) {
    std::copy_n(io[0], n, input_[0].data());
    std::copy_n(io[1], n, input_[1].data());
    float mix[2][kChunk];
    for (int i = 0; i < graphOrderCount_; ++i) {
        const int node = graphOrder_[i];
        mixInputs(node, mix, n, p, frozen_[node]);
        if (node < kFxSlots) {
            std::copy_n(mix[0], n, slotOut_[node][0].data());
            std::copy_n(mix[1], n, slotOut_[node][1].data());
            float *const out[2] = {slotOut_[node][0].data(), slotOut_[node][1].data()};
            runSlot(node, out, n, p, tempo, pitch, keep);
        } else if (const int sum = fxNodeSum(node); sum >= 0) { // what goes in, as it is
            std::copy_n(mix[0], n, sumOut_[sum][0].data());
            std::copy_n(mix[1], n, sumOut_[sum][1].data());
        } else {
            runSplit(node - kFxSlots, n, p, mix);
        }
    }
    mixInputs(kFxOutNode, mix, n, p, rewiring_); // as it was, while it ducks out to rewire
    std::copy_n(mix[0], n, io[0]);
    std::copy_n(mix[1], n, io[1]);
}

void Effects::measurePorts(uint32_t n, bool silent) {
    // Only the editor reads them: with none analyzing, they read nothing, and start from there when one does.
    if (!analyzing_) {
        portLevel_.fill(0.0f);
        return;
    }
    // This chunk's peak of each port that ran, against the last level fallen by the chunk's length; in a chunk that
    // went by idle, every port's is 0 (what the ports hold is the last chunk that ran).
    std::array<float, kFxPorts> peak{};
    auto peakOf = [n](const std::array<std::array<float, kChunk>, 2> &out) {
        float most = 0.0f;
        for (const auto &side : out)
            for (uint32_t s = 0; s < n; ++s)
                most = std::max(most, std::abs(side[s]));
        return most;
    };
    if (!silent)
        peak[kFxPortIn] = peakOf(input_);
    for (int i = 0; i < graphOrderCount_ && !silent; ++i) {
        const int node = graphOrder_[i];
        if (node < kFxSlots) {
            peak[fxSlotPort(node)] = peakOf(slotOut_[node]);
        } else if (const int sum = fxNodeSum(node); sum >= 0) {
            peak[fxSumPort(sum)] = peakOf(sumOut_[sum]);
        } else {
            const int split = node - kFxSlots;
            for (int band = 0; band < kFxBands; ++band)
                peak[fxBandPort(split, band)] = peakOf(bandOut_[split][band]);
        }
    }
    const float fall = static_cast<float>(std::pow(10.0, -kPortFallDb * n / sampleRate_ / 20.0));
    for (int port = 0; port < kFxPorts; ++port)
        portLevel_[port] = std::isfinite(peak[port]) ? std::max(peak[port], portLevel_[port] * fall) : 0.0f;
}

void Effects::clearGraph() {
    orderStale_ = true;
    for (auto &out : sumOut_)
        for (auto &side : out)
            side.fill(0.0f);
    for (auto &out : slotOut_)
        for (auto &side : out)
            side.fill(0.0f);
    for (auto &split : bandOut_)
        for (auto &band : split)
            for (auto &side : band)
                side.fill(0.0f);
    for (Split &sp : splits_)
        sp.running = false;
}

// What a parametric EQ, a filter or a phase disperser puts out goes to its analyzer, while the editor is open.
void Effects::feedAnalyzer(int slot, float *const *io, uint32_t n) {
    if (!analyzing_)
        return;
    Slot &place = slots_[slot];
    for (uint32_t s = 0; s < n; ++s)
        analyzer_.push(place.analyzed, 0.5f * (io[0][s] + io[1][s]));
}

float Effects::sweepPhase(int slot) const {
    const Slot &s = slots_[slot];
    return static_cast<float>(s.running == FxType::Phaser     ? s.phaser.phase
                              : s.running == FxType::Flanger  ? s.flanger.phase
                              : s.running == FxType::Chorus   ? s.chorus.phase
                              : s.running == FxType::Widener ? s.widener.phases[0]
                                                              : 0.0);
}

} // namespace fmvibe
