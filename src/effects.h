#pragma once

#include "amp.h"
#include "analyzer.h"
#include "clipcurve.h"
#include "filter.h"
#include "fxlag.h"
#include "oversampler.h"
#include "params.h"
#include "zerobuffer.h"

// Each effect type a slot runs, in a module of its own (with what the editor draws of it).
#include "fx/ampfx.h"
#include "fx/chorus.h"
#include "fx/common.h"
#include "fx/compressor.h"
#include "fx/delay.h"
#include "fx/disperser.h"
#include "fx/lofi.h"
#include "fx/eq.h"
#include "fx/filterfx.h"
#include "fx/flanger.h"
#include "fx/multidyn.h"
#include "fx/parameq.h"
#include "fx/phaser.h"
#include "fx/reverb.h"
#include "fx/shaper.h"
#include "fx/shifter.h"
#include "fx/utility.h"
#include "fx/widener.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace fmvibe {

// The effects on the sum of the voices: kFxSlots slots, each holding a
// shaper, delay, DJ EQ, parametric EQ, filter, compressor, three-band
// dynamics, reverb, phaser, flanger, chorus, frequency shifter, phase disperser, widener, amp, utility or Lo-Fi, or
// nothing,
// run as the effects graph's wires take them from the input to the output (a chain is the slots one after another). An
// empty or switched-off slot costs nothing; switching one fades it in or out over a few milliseconds. Audio thread
// only, apart from construction and prepare().
class Effects {
public:
    // Allocates the delay lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void reset();

    // In place. Reads the slot params and the graph's from `p`; synced delays and sweeps use `tempo` (BPM),
    // and a filter's key tracking follows `pitch`, the newest voice's (semitones, 69 is A4).
    void process(float *left, float *right, uint32_t frames, const ParamValues &p, double tempo, double pitch);
    // Filter FM on the filter effect in `slot`: how many octaves its cutoff moves each sample of the next process(),
    // which takes at most kChunk frames while one is set; null for none. The caller keeps them until then.
    void setCutoffFm(int slot, const float *octaves) { cutoffFm_[slot] = octaves; }
    // Something is still to be heard after the input went silent: delay echoes, a reverb's tail.
    bool ringing() const { return tail_ > 0; }
    // Silences what the effects still hold (MIDI's All Sound Off): every effect running fades out over a few
    // milliseconds, as one switched off does, and then starts again cleared, as one switched on does.
    void flush() { flushing_ = true; }
    // How far a compressor or three-band dynamics in `slot` turns band `band` down right now, in dB (0 or more); a
    // compressor uses band 0, and an amp band 0 for how far its stages saturate. The most of the latest block, for the
    // editor's meters.
    float reduction(int slot, int band) const { return reduction_[slot][band]; }
    // A three-band dynamics' band `band` in `slot`: its detector's level (1 is full scale), the most of the latest
    // block, falling about 26 dB a second, and the gain (dB, below 0 pulling down, above lifting) between that level and
    // the same through the band's dynamics, held and falling alike: the gain it applies, while the level holds still.
    float bandLevel(int slot, int band) const { return bandLevel_[slot][band]; }
    float bandGain(int slot, int band) const { return bandGain_[slot][band]; }
    // The parametric EQs measure their output only while the editor can show it.
    void setAnalyzing(bool on) { analyzing_ = on; }
    // The level of what a parametric EQ or filter in `slot` puts out in spectrum band `band`, where a full-scale
    // sine reads 1: the most of the latest measurements, falling away about 55 dB a second.
    float spectrum(int slot, int band) const { return slots_[slot].analyzed.bands[band]; }
    // Where a phaser's, flanger's, chorus's or widener's sweep is, in cycles (the widener's first tap's, the chorus's
    // first voice's); 0 for other effects.
    float sweepPhase(int slot) const;
    // The effects graph's ports (see fxPortValue()): how loud what comes out of each is, the peak of either side
    // (1 is full scale), falling kPortFallDb a second; 0 for a port of a node the graph doesn't run, and for every port
    // while nothing analyzes (see setAnalyzing()). For the editor, which lights the graph's nodes and wires by them.
    static constexpr double kPortFallDb = 90.0;
    float portLevel(int port) const { return port >= 0 && port < kFxPorts ? portLevel_[port] : 0.0f; }

private:
    static constexpr uint32_t kChunk = fx::kChunk; // samples per parameter ramp
    using BiquadCoefs = fx::BiquadCoefs;
    using EqChannel = fx::EqChannel;
    using CrossoverCoefs = fx::CrossoverCoefs;

    // One place in the chain, holding every effect type's state (each a module of src/fx/), so that it takes another
    // type without allocating. `running` is the type it processes as; when the slot gets another type, the old one
    // fades out before the new one starts.
    struct Slot {
        FxType running = FxType::None;
        double fade = 0.0; // 0 = off .. 1 = fully in
        // Samples its wet sound is still silent for once it starts (a flanger's wait through zero, longer than a
        // fade): it fades in only after, else the dry sound would fade out into a gap.
        int prime = 0;
        fx::Shaper shaper;
        fx::Delay delay;
        fx::Eq eq;
        fx::ParamEq peq;
        fx::FilterFx filter;
        fx::Compressor comp;
        fx::MultiDyn dyn;
        fx::Reverb reverb;
        fx::Phaser phaser;
        fx::Flanger flanger;
        fx::Chorus chorus;
        fx::Shifter shifter;
        fx::Disperser disperser;
        fx::Widener widener;
        fx::Utility utility;
        fx::LoFi lofi;
        std::unique_ptr<fx::AmpFx> amp; // on the heap, from prepare(): two amps' state is large
        // The parametric EQ's, filter's or phase disperser's output, for its analyzer.
        Analyzed analyzed;
    };

    // What the effect in `slot` runs with (see fx::Context), and where it puts what the editor's meters show.
    fx::Context context(int slot, const ParamValues &p, double tempo, double pitch) const {
        return {p, slot, sampleRate_, rampShortest_, tempo, pitch, cutoffFm_[slot]};
    }
    std::array<const float *, kFxSlots> cutoffFm_{}; // see setCutoffFm()
    fx::Meters meters(int slot) { return {reduction_[slot], bandLevel_[slot], bandGain_[slot]}; }

    // A slot's effect in place on `io`, fading as it is switched; `keep` grows while it rings on.
    void runSlot(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo, double pitch,
                 int64_t &keep);
    // Clears the effect in `slot` as it is about to fade in, and jumps its ramps to the current settings.
    void start(int slot, const fx::Context &ctx);

    // The effects graph, which the effects always run as (a chain is a line of it). The nodes it runs are those OUT
    // hears, through the wires as set or still fading: a slot holding an effect, a split or a sum in use, and one
    // leaving the graph for as long as a wire fading out still reads it. Each sums the wires into it, each gliding to
    // its level (see mixInputs()), then runs into a buffer of its own, in the order orderGraph() works out once a
    // process(). A wire from a node that hasn't run yet in a chunk, which only a loop makes, reads what it put out the
    // chunk before.
    // A wire end's pad, its paths up and down and flangers' waits (see fxlag.h): as many as it lacks against the latest
    // path into its node, each a stereo UpDown or Wait taken from the pool (see Lags), the 2x ones first: as many as a
    // path can hold, one for each slot. While the pool hasn't enough (a graph of nearly every slot's effect
    // oversampled), it goes without, out of step, and takes them as soon as the pool has them again (see mixInputs()).
    static constexpr int kEndPaths = kFxSlots;
    struct WireEnd {
        int port = -1;      // what it comes from
        double level = 0.0; // its gain at the end of the last chunk; below 0, it takes the next chunk's at once
        double fade = 0.0;  // 0 out .. 1 in, along a raised cosine as heard (see mixInputs())
        FxLag pad;          // what it is put through to meet the node's other inputs in step
        int paths = 0;      // how many of the pool's it holds, in `path`
        int prime = 0;      // samples its pad's fresh wait still puts out silence for: it fades in only after
        std::array<uint8_t, kEndPaths> path{};
        // Whether it holds every path its pad asks for.
        bool padded() const { return paths == pad.count(); }
    };
    // A wire moved crossfades: what it comes from now fades in as what it came from before fades out. A wire whose pad
    // changes crossfades the same way, from the end with the pad it had to one with the new one.
    struct Wire {
        WireEnd now, was;
    };
    // A split's filters at one slope: 6, 12 or 24 for two bands, kThree12 or kThree24 for three (at 12 or 24 dB),
    // with their coefficients for the crossovers they were last worked out for.
    static constexpr int kThree12 = 112, kThree24 = 124;
    struct SplitFilters {
        std::array<EqChannel, 2> channels{};
        int mode = -1;
        double lowNorm = -1.0, highNorm = -1.0; // as the coefficients below were worked out for
        CrossoverCoefs coefs;                   // 24 dB: lp1 and hp1; three bands at 24: all of them
        BiquadCoefs onePoleLow, onePoleHigh;    // 6 and 12 dB, and three bands at 12: at the low crossover
        BiquadCoefs onePoleLow2, onePoleHigh2, allPass2; // three bands at 12: at the high crossover
    };
    struct Split {
        SplitFilters now, was; // was: at the slope it left, fading out over 5 ms as `now` fades in
        double wasFade = 0.0;  // 1 .. 0
        bool running = false;
    };
    // Whether a node is in the graph by its params: a slot holding an effect (or fading one out), a split or a sum in
    // use, the output.
    bool nodeWanted(int node, const ParamValues &p) const;
    void orderGraph(const ParamValues &p, bool asSet);
    // Whether the params orderGraph() reads (every wire's From, the slots' types, amps' and shapers' oversampling and
    // flangers' through zero, which set how late each path is, the splits' and sums' Used) differ from the last call's,
    // which it keeps.
    bool graphParamsChanged(const ParamValues &p);
    // Whether a chunk of `n` samples of `io` can go by without running the graph: exact silence going in, nothing
    // ringing, and every effect, wire and split where its params put it, with no fade under way.
    bool idleChunk(const float *const *io, uint32_t n, const ParamValues &p) const;
    // The phasers' and flangers' sweeps going round through such a chunk, so a synced one keeps time with the host.
    void idleSweeps(uint32_t n, const ParamValues &p, double tempo);
    // While the editor is open, the analyzers taking in such a chunk's silence, as running the effects would give
    // them, so their spectra fall away as they would.
    void idleAnalyzers(uint32_t n);
    bool loopsAsSet(const ParamValues &p) const; // whether the wires as set make a loop
    // Where a wire into `node` comes from as set: its From's port, or none (-1) while that port's node isn't in the
    // graph, so a wire from an effect taken away (which a host or a state can leave) fades out with it.
    int wirePort(int node, int input, const ParamValues &p) const;
    // A wire's gain as set: a sum's in dB and soloed as a join, every other's a share.
    double wireTarget(int node, int input, const ParamValues &p) const;
    // Every wire of `node` as its params set it, at its level and fully in.
    void setWires(int node, const ParamValues &p);
    // Where what comes out of `port` is this chunk; false for a port of a node not running, which reads silence.
    bool portOut(int port, const float *(&out)[2]) const;
    // Sums the wires into `node`. `frozen`, for a node only a wire fading out still reads, keeps them as they are.
    void mixInputs(int node, float (*mix)[kChunk], uint32_t n, const ParamValues &p, bool frozen);
    void splitCoefs(SplitFilters &f, double lowNorm, double highNorm) const;
    static void splitBands(SplitFilters &f, int ch, double x, double *bands);
    void runSplit(int split, uint32_t n, const ParamValues &p, const float (*mix)[kChunk]);
    void processGraph(float *const *io, uint32_t n, const ParamValues &p, double tempo, double pitch, int64_t &keep);
    void clearGraph();
    // A slot the graph no longer runs: it starts again cleared, as one switched on does.
    void idleSlot(int slot);

    double sampleRate_ = 48000.0;
    uint32_t rampShortest_ = 32; // the fewest samples a gain's or mix's ramp is spread over, at this rate
    // What every slot's effect of a type shares at this rate: the frequency shifters' Hilbert pairs, the reverbs'
    // lines and the DJ EQs' crossovers.
    fx::HilbertCoefs hilbert_ = fx::hilbertCoefs(48000.0);
    fx::ReverbTables reverbTables_;
    CrossoverCoefs eqCrossovers_;
    std::array<Slot, kFxSlots> slots_;
    int64_t tail_ = 0; // samples left to keep processing after the input went silent
    bool flushing_ = false; // fading every effect out, until all are (see flush())
    std::array<std::array<float, kMcBands>, kFxSlots> reduction_{};
    std::array<std::array<float, kMcBands>, kFxSlots> bandLevel_{}, bandGain_{};
    void feedAnalyzer(int slot, float *const *io, uint32_t n);
    bool analyzing_ = false;
    Analyzer analyzer_;

    std::array<std::array<float, kChunk>, 2> input_{}; // what came in, this chunk
    std::array<std::array<std::array<float, kChunk>, 2>, kFxSlots> slotOut_{};
    std::array<std::array<std::array<std::array<float, kChunk>, 2>, kFxBands>, kFxSplits> bandOut_{};
    std::array<std::array<std::array<float, kChunk>, 2>, kFxSums> sumOut_{};
    std::array<float, kFxPorts> portLevel_{}; // see portLevel()
    // `silent`: a chunk that went by idle, whose ports all read 0.
    void measurePorts(uint32_t n, bool silent = false);
    std::array<std::array<Wire, kFxSumInputs>, kFxNodes> wires_{}; // as many as a node takes at most
    static_assert(kFxSumInputs >= kFxInputs && kFxSumInputs >= kFxOutInputs);
    std::array<Split, kFxSplits> splits_{};
    std::array<int, kFxNodes> graphOrder_{};
    int graphOrderCount_ = 0;
    bool looped_ = false;   // orderGraph() found a loop through wires moving (see process())
    // The graph's order holds until what it is worked out from moves: those params (see graphParamsChanged()), a
    // fade (a wire's, or a slot's, which keeps an effect taken away in the graph while it fades out), a node starting
    // or stopping, or the graph cleared. Else ordering it every chunk would only find it as it was.
    bool orderStale_ = true;
    static constexpr int kGraphParams = [] {
        // The types, amps' and shapers' oversampling, flangers' through zero, Used switches.
        int count = 4 * kFxSlots + kFxSplits + kFxSums;
        for (int node = 0; node < kFxNodes; ++node)
            count += fxInputCount(node);
        return count;
    }();
    std::array<double, kGraphParams> graphParams_{}; // as the last call had them; NaN to begin with
    bool rewiring_ = false; // the output ducking out to put every wire as set at once
    double duck_ = 1.0;     // how far in the output is, 0 .. 1
    std::array<bool, kFxNodes> running_{};   // the nodes the graph runs this process() (see orderGraph())
    std::array<bool, kFxNodes> frozen_{};    // those only a wire fading out reads: their wires stay as they were
    std::array<bool, kFxNodes> ranBefore_{}; // the nodes it ran the process() before

    // A flanger's wait through zero (see fxlag.h), a side: a line exactly as many samples late, in Lags::waitLines.
    struct Wait {
        float *line = nullptr;
        int size = 0;
        int at = 0;
        void reset() {
            std::fill_n(line, size, 0.0f);
            at = 0;
        }
        void run(float *x, int frames) {
            for (int i = 0; i < frames; ++i) {
                const float y = line[at];
                line[at] = x[i];
                x[i] = y;
                at = at + 1 == size ? 0 : at + 1;
            }
        }
    };
    // Keeping the graph's paths in step (see fxlag.h). An oversampled effect's own dry sound (what it fades from as it
    // is switched, what it passes while off, an amp's Mix) goes up and down as its wet sound does, and a flanger's
    // through zero waits as its sound does, so the slot is as late whatever it does; it crossfades over 5 ms, from the
    // path it had, when its type, oversampling or through zero switch changes.
    struct SlotDry {
        std::array<UpDown, 2> now, was;
        std::array<Wait, 2> nowWait, wasWait; // used while `lag`, `wasLag` hold a wait
        FxLag lag, wasLag;                    // the slot's (see fxSlotLag()) that `now` and `was` are
        double wasFade = 0.0; // 1 .. 0
        bool live = false;    // whether the last chunk ran it (else it starts again from silence)
        int prime = 0;        // samples a fresh `nowWait` still puts out silence for: `was` fades out only after
    };
    // On the heap, from prepare(): about 600 KB, too much for the stack the tests keep an Effects on.
    static constexpr int kPoolPaths = 96;
    static_assert(kPoolPaths <= 256, "a wire end keeps its paths' numbers in bytes");
    struct Lags {
        std::array<std::array<UpDown, 2>, FxLag::kUpDowns> fresh; // a path at each factor, cleared, for the rate
        std::array<std::array<UpDown, 2>, kPoolPaths> pool;       // the wire ends' pads take theirs from here
        std::array<std::array<Wait, 2>, kPoolPaths> poolWait;     // or, for a wait, from here
        std::array<uint8_t, kPoolPaths> kind{};                   // the kind each pool path was taken as
        std::array<bool, kPoolPaths> taken{};
        std::array<SlotDry, kFxSlots> dry;
        ZeroBuffer waitLines; // every Wait's line, the pool's then the slots' (only those used taking memory)
    };
    std::unique_ptr<Lags> lags_;
    FxPads pads_{}; // each wire's pad as set (see orderGraph())
    // A wire end set to come from `port` with `pad`, which takes its paths from the pool, and one let go of, which
    // gives them back.
    void setEnd(WireEnd &e, int port, double level, double fade, const FxLag &pad);
    void dropEnd(WireEnd &e);
    // How many of the pool's paths no wire end holds.
    int freePaths() const;
    // What comes out of `from` through `e`'s pad, into `out`.
    void padEnd(const WireEnd &e, const float *const *from, float (*out)[kChunk], uint32_t n);
    // The slot's dry sound into `dry`, as late as its effect makes its wet sound (see SlotDry). Returns how many
    // samples from the chunk's start it is still silent for (a fresh wait filling), which the effect waits out.
    int slotDry(int slot, const float *const *io, float (*dry)[kChunk], uint32_t n, const ParamValues &p, bool needed);
};

} // namespace fmvibe
