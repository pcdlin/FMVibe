#pragma once

#include "amp.h"
#include "analyzer.h"
#include "clipcurve.h"
#include "filter.h"
#include "params.h"
#include "zerobuffer.h"

#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace fmvibe {

// Crossovers of the DJ EQ.
constexpr double kEqLowHz = 250.0;
constexpr double kEqHighHz = 2500.0;

// Delay mix: the dry signal stays at full level up to 50 %, the echoes reach full level there.
inline double delayDryLevel(double mix) {
    return std::min(1.0, 2.0 * (1.0 - mix));
}
inline double delayWetLevel(double mix) {
    return std::min(1.0, 2.0 * mix);
}

// Magnitude response of the DJ EQ at `hz`, for band params (-1..1) low, mid and high.
double eqResponse(double hz, double low, double mid, double high);
// Magnitude response of one parametric EQ band at `hz`, from its params (frequency and Q normalized, gain in dB).
double peqResponse(double hz, PeqType type, double freq, double gainDb, double q);
// The phaser's and flanger's sweeps: the phaser's all-pass corner (Hz) and the flanger's delay
// (seconds) with their LFO at `lfo` (-1..1), from their params.
double phaserCorner(double centerNorm, double depth, double lfo);
double flangerDelay(double delayNorm, double depth, double lfo);
// How much the phaser's and flanger's feedback (-1..1 as set) sends round, and the gain that keeps
// their broadband level where it was without feedback.
double sweepFeedback(double feedback);
double sweepGain(double feedback);
// The phaser's and flanger's polarity from their Negative switch: 1, or -1 to turn their wet sound and their
// feedback over. Their response then is the positive one's with the wet sound's phase turned half a cycle, every
// notch moved to where a peak was.
inline double sweepPolarity(double negative) {
    return negative >= 0.5 ? -1.0 : 1.0;
}
// Magnitude responses, for drawing: the phaser with its corner at `cornerHz`, the flanger with its
// delay at `delaySeconds`, at `polarity` (see sweepPolarity()). At 50 % mix their notches go deepest.
double phaserResponse(double hz, double cornerHz, int stages, double feedback, double mix, double polarity);
double flangerResponse(double hz, double delaySeconds, double feedback, double mix, double polarity);
// The phase disperser's all-pass (b0, b1, b2, a1, a2) at Freq (0..1 on the cutoff curve), Pinch setting its Q
// from 0.3 to 6, and how long the sound at `hz` comes out after the rest through `stages` of them (seconds).
std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate);
double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate);
// The phaser's and flanger's Mix knobs run to 100 %, which is half wet, where their notches go deepest: past that
// they would only get shallower again.
inline double sweepWet(double mix) {
    return 0.5 * mix;
}
// The reverb's decay (seconds to fall 60 dB) and pre-delay (seconds) from their params.
double reverbDecay(double norm);
double reverbPreDelay(double norm);
// How each reverb algorithm sets up the network: its lines' length against the hall's, how much its damping
// lets through against the knob's, how many all-passes diffuse the input on each side, how loud its early
// reflections are, how far its lines' lengths waver against the hall's, and whether it feeds its tail back
// pitch-shifted (a shimmer).
struct ReverbStyle {
    double length;
    double brightness;
    int diffusers;
    double early;
    double wander;
    bool shimmer;
};
ReverbStyle reverbStyle(ReverbType type);
ReverbType reverbType(double value);
// The lines' length against the hall's longest, from the Size knob and the algorithm.
double reverbSpace(double sizeNorm, ReverbType type);
// A room's early reflections (seconds at full size, after the pre-delay) and their levels.
inline constexpr double kRoomEchoSeconds[] = {0.0071, 0.0113, 0.0167, 0.0229, 0.0311, 0.0409};
inline constexpr double kRoomEchoLevels[] = {0.55, 0.45, 0.38, 0.3, 0.24, 0.18};
// How much faster the tail decays above the damping's corner than the Decay knob says, the same for every line.
inline constexpr double kReverbHighDecay = 0.2;
// The lines' longest length at full size (seconds).
inline constexpr double kReverbLongest = 0.0889;

// The widener's taps: how long each waits at full size (seconds) and how fast it is swept (Hz).
// The first and third go to the left side, the others to the right.
constexpr int kWidenerTaps = 4;
inline constexpr double kWidenerDelays[kWidenerTaps] = {0.0097, 0.0133, 0.0179, 0.0231};
inline constexpr double kWidenerRates[kWidenerTaps] = {0.29, 0.37, 0.43, 0.53};
// Tap `tap`'s delay (seconds) with its sweep at `lfo` (-1..1), from the Size knob (0..1): from a fifth of its
// length, a tight doubling, to all of it, swept 12 % either way.
double widenerTap(int tap, double size, double lfo);
// How much of the sound at `hz` the widener widens, from its High Pass knob (0..1 on the cutoff curve): the
// response of its high-pass, 12 dB per octave.
double widenerSpread(double hz, double highPassNorm);

// The utility's gain on each side, from its Gain knob (0..1, see utilityGain()) and its Pan (-1..1): a balance, which
// turns the side it pans away from down and leaves the other as it is.
struct UtilityGains {
    double left, right;
};
UtilityGains utilityGains(double gainNorm, double pan);

// The effects on the sum of the voices: kFxSlots slots, each holding a
// shaper, delay, DJ EQ, parametric EQ, filter, compressor, three-band
// dynamics, reverb, phaser, flanger, frequency shifter, phase disperser, widener, amp or utility, or nothing, run as
// the effects graph's wires take them from the input to the output (a chain is the slots one after another). An
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
    // Where a phaser's, flanger's or widener's sweep is, in cycles (the widener's first tap's); 0 for other effects.
    float sweepPhase(int slot) const;
    // The effects graph's ports (see fxPortValue()): how loud what comes out of each is, the peak of either side
    // (1 is full scale), falling kPortFallDb a second; 0 for a port of a node the graph doesn't run, and for every port
    // while nothing analyzes (see setAnalyzing()). For the editor, which lights the graph's nodes and wires by them.
    static constexpr double kPortFallDb = 90.0;
    float portLevel(int port) const { return port >= 0 && port < kFxPorts ? portLevel_[port] : 0.0f; }

private:
    static constexpr uint32_t kChunk = 64; // samples per parameter ramp
    // How often the phaser and flanger work out where their sweep is, in samples, moving in a straight line between:
    // a sweep (100 Hz at most) bends too little over these few samples to tell, and each step costs a sine, an
    // exponential and a tangent per side.
    static constexpr uint32_t kSweepStep = 8;

    struct BiquadCoefs {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct Biquad { // transposed direct form II
        double z1 = 0.0, z2 = 0.0;

        double process(double x, const BiquadCoefs &c) {
            const double y = c.b0 * x + z1;
            z1 = c.b1 * x - c.a1 * y + z2;
            z2 = c.b2 * x - c.a2 * y;
            return y;
        }
    };

    struct Shaper {
        std::array<ClipState, 2> sides{};
        double drive = 1.0; // gains, ramped
        double output = 1.0;
        double mix = 1.0;
    };

    struct Delay {
        std::array<ZeroBuffer, 2> lines;
        int write = 0;
        uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
        std::array<double, 2> length{};   // samples, glide toward the set times
        std::array<double, 2> lowPass{};  // tone filter states
        std::array<double, 2> highPass{};
        double feedback = 0.0;
        double dry = 1.0;
        double wet = 0.0;
        double width = 1.0;
    };

    // Linkwitz-Riley crossovers: the low band gets an allpass so the three bands
    // sum back to a flat response.
    struct EqChannel {
        Biquad lowA, lowB, lowPhase, restA, restB, midA, midB, highA, highB;
    };

    struct Eq {
        std::array<EqChannel, 2> channels{};
        std::array<double, 3> gains{1.0, 1.0, 1.0};
    };

    struct ParamEq {
        std::array<std::array<Biquad, 2>, kPeqBands> bands{}; // per band, per channel
        // How much of each band is heard, gliding to its On switch (see runParamEq()); NaN until the first chunk,
        // which takes the switch as it is.
        static constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();
        std::array<double, kPeqBands> on{kUnset, kUnset, kUnset};
        double amount = 1.0; // ramping to its Amount (see runParamEq())
    };

    struct FilterFx {
        std::array<Filter, 2> channels{};
        std::array<ZeroBuffer, 2> combs; // comb delay lines, combLength() long
        double mix = 1.0;
        // Its settings, and the params (and pitch, while it tracks the key) they were worked out from: NaN until the
        // first chunk, so worked out again only when one moves (see runFilter()).
        FilterSettings settings;
        std::array<double, 7> settingsFor{std::numeric_limits<double>::quiet_NaN()};
    };

    // The gain reduction (dB, 0 or below) follows the static curve at the attack
    // or release pace.
    struct Compressor {
        double reduction = 0.0;
        double makeup = 1.0; // gains, ramped
        double mix = 1.0;
    };

    // Split like the DJ EQ, at crossovers set per chunk; each band's detector follows its peaks (`peak`, falling back slowly
    // between them), or its RMS level (`power`, a mean square). Its `envelope` follows that level at the attack pace
    // while it rises, else at the release pace, and its gain (dB) is dynamicsGain() of the envelope. Following the
    // level rather than the gain, a note out of silence takes the lift away within a few milliseconds, as the level
    // climbs tens of dB in its first moments, where a gain following at the attack pace would take tens of
    // milliseconds to come down from the whole Range. `meter` is the detector level the editor shows, falling slowly,
    // and `outMeter` that level through the band's gain, held and falling alike: the editor draws what the band does
    // from one to the other, so as a band goes quiet, the gain lifting what is left, both ends fall together rather
    // than the held level going in taking the lift of the quiet that follows it.
    struct MultiDyn {
        std::array<EqChannel, 2> channels{};
        std::array<double, kMcBands> peak{}, power{}, envelope{};
        std::array<double, kMcBands> gain{};
        std::array<double, kMcBands> meter{}, outMeter{};
        std::array<double, kMcBands> gains{1.0, 1.0, 1.0}; // band gains after the dynamics, ramped
        double mix = 1.0;
        double output = 1.0; // gain after the mix, ramped
    };

    // A feedback delay network: kReverbLines lines of different lengths, mixed through a Hadamard matrix
    // (which keeps their energy and spreads each line's evenly over all of them), each with the gain that makes
    // it fall 60 dB in the decay time and a high shelf that takes it down faster above the damping's corner, by as
    // much for every line. The lines' lengths waver slowly, each at its own pace, so the tail's resonances
    // smear rather than ring. Each side of the input goes through the pre-delay and a few all-passes that diffuse
    // it, then into half the lines; the lines' outputs, signed differently, make the two sides. A shimmer
    // band-passes each side of its tail, pitch-shifts it and feeds it back into the lines (past the all-passes), so
    // what goes round climbs (or falls) by the interval each time.
    static constexpr int kReverbLines = 16;
    static constexpr int kDiffusers = 6; // per side: a plate runs the input through all of them, the others through four
    struct Reverb {
        std::array<ZeroBuffer, kReverbLines> lines; // all as long as the longest, with room for the wavering
        std::array<std::array<ZeroBuffer, kDiffusers>, 2> diffusers; // per side
        std::array<ZeroBuffer, 2> preDelay;
        std::array<ZeroBuffer, 2> shifter; // a shimmer's band-passed tail, per side, its shifters read
        int lineWrite = 0;
        uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
        int diffuserWrite = 0;
        int preWrite = 0;
        int shiftWrite = 0;
        // The diffusers the algorithm ran last. Those it left out stood still, holding the sound of then, and are
        // cleared when one taken up again (see runReverb()), as the shifters are when a shimmer starts again.
        int diffusersUsed = kDiffusers;
        std::array<double, kReverbLines> damp{}; // the shelves' low-pass states
        // The lines' wavering: a sine each, as a turning phasor.
        std::array<double, kReverbLines> wanderCos{}, wanderSin{};
        // Shimmer: the shifters' read heads sweep the window (see kShimmerWindow) at a phase both sides share, the
        // right side's a quarter of a cycle on. What they read was band-passed on its way in: a low-pass, and what
        // a one-pole lets through below the high-pass corner, taken away.
        double shimmerPhase = 0.0;
        std::array<std::array<Biquad, 2>, 2> shimmerLow{}; // two in a row a side
        std::array<double, 2> shimmerBass{};
        double size = 1.0;      // the lines' length scale, gliding toward the knob
        double preLength = 0.0; // samples, gliding
        double reach = 0.0;     // samples before what goes in has all come out of the pre-delay (see runReverb())
        double dry = 1.0;       // ramped
        double wet = 0.0;
        double width = 1.0;
        double shimmer = 0.0;   // the gain the shifted tail goes back in at, ramped
        // Each line's gain and its shelf's (see runReverb()), and the size, share of the tail fed back shifted and
        // decay they were worked out for: NaN until the first chunk.
        std::array<double, kReverbLines> gains{}, highs{};
        std::array<double, 3> gainsFor{std::numeric_limits<double>::quiet_NaN()};
    };

    // First-order all-passes in a row, per side, their corners swept by the LFO; the right side's
    // LFO runs a quarter cycle ahead. Those past Stages wait cleared, so more start clean.
    struct Phaser {
        std::array<std::array<double, kMaxPhaserStages>, 2> stages{};
        std::array<double, 2> last{}; // the last stage's output, fed back to the first
        double phase = 0.0;           // the LFO, in cycles
        double mix = 0.5;
        double polarity = 1.0; // sweepPolarity()'s, moved there over 10 ms (kPolaritySeconds)
        // The all-passes' coefficient per side, worked out every kSweepStep samples and moved along in a straight
        // line between (see runPhaser()): where it was last worked out, and whether it has been yet.
        std::array<double, 2> coef{};
        bool coefKnown = false;
    };

    struct Flanger {
        std::array<ZeroBuffer, 2> lines;
        int write = 0;
        uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
        std::array<double, 2> last{}; // the delayed sound, fed back
        double phase = 0.0;
        double mix = 0.5;
        double polarity = 1.0; // as the phaser's
        // The delay per side in samples, worked out every kSweepStep samples like the phaser's coefficient.
        std::array<double, 2> delay{};
        bool delayKnown = false;
    };

    // Two all-pass chains whose outputs stay 90 degrees apart across the audio band (Olli
    // Niemitalo's design): a signal and its Hilbert transform.
    struct Hilbert {
        std::array<std::array<double, 4>, 4> a{}; // per section: x[n-1], x[n-2], y[n-1], y[n-2]
        std::array<std::array<double, 4>, 4> b{};
        double delayed = 0.0; // the first chain's output, one sample late

        // The in-phase and quadrature parts of `x`.
        std::array<double, 2> process(double x);
    };

    // The phase disperser's all-passes, per side; those past Stages wait cleared, so more start clean.
    struct Disperser {
        std::array<std::array<Biquad, kMaxDisperserStages>, 2> stages{};
        double mix = 1.0;
    };

    // The sound above the high-pass, mono, in a short line that kWidenerTaps swept taps read.
    struct Widener {
        ZeroBuffer line;
        int write = 0;
        uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
        Biquad highPass;
        std::array<double, kWidenerTaps> phases{}; // the taps' sweeps, in cycles
        double size = 0.5; // gliding toward the knob
        double width = 1.0; // ramped
        double mix = 0.5;
    };

    // The utility: the sides through a high-pass for its Mono Bass, then scaled by Width, and each side's gain (Gain
    // and the balance), ramped.
    struct Utility {
        Biquad highPass;
        double gainL = 1.0, gainR = 1.0, width = 1.0;
        double mono = 0.0; // how much of the high-passed sides is heard, 0 (Mono Bass off) .. 1, faded
        double hz = 0.0;   // the high-pass's corner: Mono Bass's, the last one set while it fades out
    };

    struct Shifter {
        std::array<Hilbert, 2> hilbert{};
        std::array<double, 2> phase{}; // the oscillators, in cycles
        std::array<double, 2> last{};  // the shifted sound, fed back
        double mix = 1.0;
    };

    // One place in the chain. `running` is the type it processes as; when the
    // slot gets another type, the old one fades out before the new one starts.
    // The amp: the one running at its setting (model, cabinet, mic, oversampling, ...) and, for 10 ms after the setting
    // changes, the one it left, fading out as the new one fades in.
    struct AmpFx {
        AmpCore now, was;
        double wasFade = 0.0;           // 1 .. 0
        double output = 1.0, mix = 1.0; // their gains at the end of the last chunk
    };
    struct Slot {
        FxType running = FxType::None;
        double fade = 0.0; // 0 = off .. 1 = fully in
        Shaper shaper;
        Delay delay;
        Eq eq;
        ParamEq peq;
        FilterFx filter;
        Compressor comp;
        MultiDyn dyn;
        Reverb reverb;
        Phaser phaser;
        Flanger flanger;
        Shifter shifter;
        Disperser disperser;
        Widener widener;
        Utility utility;
        std::unique_ptr<AmpFx> amp; // on the heap, from prepare(): two amps' state is large
        // The parametric EQ's, filter's or phase disperser's output, for its analyzer.
        Analyzed analyzed;
        // The shaper's drawn curve as a table (see runShaper()).
        ClipTable curve;
    };

    // A slot's effect in place on `io`, fading as it is switched; `keep` grows while it rings on.
    void runSlot(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo, double pitch,
                 int64_t &keep);

    // Linkwitz-Riley crossovers at `lowHz` and `highHz` (kept apart and below where the biquads hold up), and three
    // bands of `x` through them, the low one through an all-pass at the high crossover so the bands sum back flat.
    struct CrossoverCoefs {
        BiquadCoefs lp1, hp1, lp2, hp2, ap2;
    };
    CrossoverCoefs crossoverCoefs(double lowHz, double highHz) const;
    static void splitThree(EqChannel &e, double x, const CrossoverCoefs &c, double *bands);

    // The effects graph, which the effects always run as (a chain is a line of it). The nodes it runs are those OUT
    // hears, through the wires as set or still fading: a slot holding an effect, a split or a sum in use, and one
    // leaving the graph for as long as a wire fading out still reads it. Each sums the wires into it, each gliding to
    // its level (see mixInputs()), then runs into a buffer of its own, in the order orderGraph() works out once a
    // process(). A wire from a node that hasn't run yet in a chunk, which only a loop makes, reads what it put out the
    // chunk before.
    struct WireEnd {
        int port = -1;      // what it comes from
        double level = 0.0; // its gain at the end of the last chunk; below 0, it takes the next chunk's at once
        double fade = 0.0;  // 0 out .. 1 in, along a raised cosine as heard (see mixInputs())
    };
    // A wire moved crossfades: what it comes from now fades in as what it came from before fades out.
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
    // Whether the params orderGraph() reads (every wire's From, the slots' types, the splits' and sums' Used) differ
    // from the last call's, which it keeps.
    bool graphParamsChanged(const ParamValues &p);
    // Whether a chunk of `n` samples of `io` can go by without running the graph: exact silence going in, nothing
    // ringing, nobody analyzing, and every effect, wire and split where its params put it, with no fade under way.
    bool idleChunk(const float *const *io, uint32_t n, const ParamValues &p) const;
    // The phasers' and flangers' sweeps going round through such a chunk, so a synced one keeps time with the host.
    void idleSweeps(uint32_t n, const ParamValues &p, double tempo);
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

    void start(int slot, const ParamValues &p, double tempo);
    double delayTarget(int slot, int side, const ParamValues &p, double tempo) const;
    void runShaper(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runDelay(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    void runEq(int slot, float *const *io, uint32_t n, const ParamValues &p);
    void runParamEq(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runFilter(int slot, float *const *io, uint32_t n, const ParamValues &p, double pitch);
    // These two return whether a gain is still on its way to where the curve puts it (a release running out).
    bool runCompressor(int slot, float *const *io, uint32_t n, const ParamValues &p);
    bool runMultiDyn(int slot, float *const *io, uint32_t n, const ParamValues &p);
    // These return the loudest sample they put out or took in, so a tail keeps them running.
    double runReverb(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runPhaser(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    double runFlanger(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo);
    double runShifter(int slot, float *const *io, uint32_t n, const ParamValues &p);

    double sampleRate_ = 48000.0;
    std::array<double, kReverbLines> reverbLengths_{}; // at full size, in samples
    double reverbMeanLength_ = 0.0; // how long the lines are on average, for their rounds per second (harmonic mean)
    std::array<std::array<int, kDiffusers>, 2> diffuserLengths_{};
    std::array<std::array<double, 2>, kReverbLines> wanderTurn_{}; // each line's wavering: cos and sin of its step
    std::array<Slot, kFxSlots> slots_;
    BiquadCoefs lowPass1_, highPass1_, lowPass2_, highPass2_, allPass2_;
    int64_t tail_ = 0; // samples left to keep processing after the input went silent
    bool flushing_ = false; // fading every effect out, until all are (see flush())
    std::array<std::array<float, kMcBands>, kFxSlots> reduction_{};
    std::array<std::array<float, kMcBands>, kFxSlots> bandLevel_{}, bandGain_{};
    void feedAnalyzer(int slot, float *const *io, uint32_t n);
    double runDisperser(int slot, float *const *io, uint32_t n, const ParamValues &p);
    double runWidener(int slot, float *const *io, uint32_t n, const ParamValues &p);
    void runAmp(int slot, float *const *io, uint32_t n, const ParamValues &p);
    void runUtility(int slot, float *const *io, uint32_t n, const ParamValues &p);
    bool analyzing_ = false;
    Analyzer analyzer_;

    std::array<std::array<float, kChunk>, 2> input_{}; // what came in, this chunk
    std::array<std::array<std::array<float, kChunk>, 2>, kFxSlots> slotOut_{};
    std::array<std::array<std::array<std::array<float, kChunk>, 2>, kFxBands>, kFxSplits> bandOut_{};
    std::array<std::array<std::array<float, kChunk>, 2>, kFxSums> sumOut_{};
    std::array<float, kFxPorts> portLevel_{}; // see portLevel()
    void measurePorts(uint32_t n);
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
        int count = kFxSlots + kFxSplits + kFxSums; // the types and Used switches
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
};

} // namespace fmvibe
