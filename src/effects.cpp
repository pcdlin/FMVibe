#include "effects.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <numbers>

namespace fmvibe {

namespace {

double dbToGain(double db) {
    return std::pow(10.0, db / 20.0);
}

enum class BiquadType { LowPass, HighPass, AllPass };

struct Coefs {
    double b0, b1, b2, a1, a2;
};

// Butterworth-Q biquads (RBJ cookbook): two in a row make a Linkwitz-Riley crossover.
Coefs biquad(BiquadType type, double hz, double sampleRate) {
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosW = std::cos(w);
    const double alpha = std::sin(w) / std::numbers::sqrt2; // sin(w) / (2Q), Q = 1/sqrt(2)
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    switch (type) {
    case BiquadType::LowPass:
        b0 = b2 = (1.0 - cosW) * 0.5;
        b1 = 1.0 - cosW;
        break;
    case BiquadType::HighPass:
        b0 = b2 = (1.0 + cosW) * 0.5;
        b1 = -(1.0 + cosW);
        break;
    case BiquadType::AllPass:
        b0 = 1.0 - alpha;
        b1 = -2.0 * cosW;
        b2 = 1.0 + alpha;
        break;
    }
    const double a0 = 1.0 + alpha;
    return {b0 / a0, b1 / a0, b2 / a0, -2.0 * cosW / a0, (1.0 - alpha) / a0};
}

// A parametric EQ band (RBJ cookbook: peaking EQ, shelves, high- and low-pass, notch), from its params.
Coefs peqCoefs(PeqType type, double freq, double gainDb, double q, double sampleRate) {
    const double hz = std::min(cutoffFromNorm(freq), 0.45 * sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    const double cosW = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * qFromNorm(q));
    const double shelf = 2.0 * std::sqrt(a) * alpha;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (type) {
    case PeqType::Peak:
    case PeqType::Count:
        b0 = 1.0 + alpha * a, b1 = -2.0 * cosW, b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a, a1 = -2.0 * cosW, a2 = 1.0 - alpha / a;
        break;
    case PeqType::LowShelf:
        b0 = a * ((a + 1.0) - (a - 1.0) * cosW + shelf), b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosW - shelf);
        a0 = (a + 1.0) + (a - 1.0) * cosW + shelf, a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW);
        a2 = (a + 1.0) + (a - 1.0) * cosW - shelf;
        break;
    case PeqType::HighShelf:
        b0 = a * ((a + 1.0) + (a - 1.0) * cosW + shelf), b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosW - shelf);
        a0 = (a + 1.0) - (a - 1.0) * cosW + shelf, a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW);
        a2 = (a + 1.0) - (a - 1.0) * cosW - shelf;
        break;
    case PeqType::LowCut:
        b0 = 0.5 * (1.0 + cosW), b1 = -(1.0 + cosW), b2 = 0.5 * (1.0 + cosW);
        a0 = 1.0 + alpha, a1 = -2.0 * cosW, a2 = 1.0 - alpha;
        break;
    case PeqType::HighCut:
        b0 = 0.5 * (1.0 - cosW), b1 = 1.0 - cosW, b2 = 0.5 * (1.0 - cosW);
        a0 = 1.0 + alpha, a1 = -2.0 * cosW, a2 = 1.0 - alpha;
        break;
    case PeqType::Notch:
        b0 = 1.0, b1 = -2.0 * cosW, b2 = 1.0;
        a0 = 1.0 + alpha, a1 = -2.0 * cosW, a2 = 1.0 - alpha;
        break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}


// Per-sample smoothing factor for a time constant of `seconds`.
double smoothing(double seconds, double sampleRate) {
    return std::exp(-1.0 / (seconds * sampleRate));
}

constexpr uint32_t kRampShortest = 32; // the fewest samples a ramp of a gain or mix is spread over

// The step that ramps a gain or mix from `from` to `to` over a chunk of `n` samples. The synth hands the effects 32
// samples at a time; a chunk the host's events cut shorter goes only its share of the way, as the voices' levels
// do (see Synth::renderBlock()), so a jump ramps as smoothly however the block is split.
double rampStep(double from, double to, uint32_t n) {
    return (to - from) / std::max(n, kRampShortest);
}

// The phaser's and flanger's polarity turns over in this long, its wet sound and feedback fading through nothing
// rather than jumping, however the host splits the block.
constexpr double kPolaritySeconds = 0.01;

double moveToward(double from, double to, double step) {
    return from < to ? std::min(from + step, to) : std::max(from - step, to);
}

// A gain (dB) closer than this to where it is going is there: a release would otherwise only arrive as it underflows,
// a minute and more on, working its gain out every sample until then.
constexpr double kGainSnapDb = 1e-4;

// One step of a gain (dB) toward `target`, by the attack's coefficient while it has to come down, else the
// release's; there once within kGainSnapDb.
double followGain(double gain, double target, double attack, double release) {
    const double next = target + (gain - target) * (target < gain ? attack : release);
    return std::abs(next - target) < kGainSnapDb ? target : next;
}

// Moves a band's gain reduction (dB) toward what the curve asks for at detector
// level `peak`: at the attack pace while it has to come down, else at the release pace.
// `quiet`: the level (1 is full scale) below which the curve leaves it alone, under the knee, where the reduction
// heads for 0 without the logarithm worked out.
// Returns whether it is still on its way there.
bool followReduction(double &reduction, double peak, double thresholdDb, double ratio, double kneeDb, double attack,
                     double release, double quiet) {
    double target = 0.0;
    if (peak >= quiet) {
        const double inDb = 20.0 * std::log10(std::max(peak, 1e-9));
        target = compressorCurve(inDb, thresholdDb, ratio, kneeDb) - inDb;
    }
    reduction = followGain(reduction, target, attack, release);
    return reduction != target;
}

// One-pole smoothing coefficient for a corner at `hz`.
double onePole(double hz, double sampleRate) {
    return 1.0 - std::exp(-2.0 * std::numbers::pi * std::min(hz, 0.45 * sampleRate) / sampleRate);
}

// The reverb's lines (at full size) and each side's diffusers, in seconds: the lines spread evenly on a log scale,
// and no two share a factor that would make their echoes line up. The all-passes' gains go with the diffusers.
constexpr double kReverbSeconds[] = {0.0290, 0.0313, 0.0337, 0.0363, 0.0391, 0.0421, 0.0454, 0.0489,
                                     0.0527, 0.0568, 0.0612, 0.0659, 0.0710, 0.0765, 0.0825, kReverbLongest};
constexpr double kDiffuserSeconds[2][6] = {{0.00477, 0.00359, 0.01273, 0.00931, 0.00213, 0.00631},
                                           {0.00449, 0.00383, 0.01187, 0.01019, 0.00241, 0.00587}};
constexpr double kDiffuserGains[] = {0.7, 0.7, 0.6, 0.6, 0.65, 0.6};
// How each line's length wavers: up to this long (seconds) for the hall, each line at a pace of its own (Hz).
constexpr double kReverbWander = 0.0003;
constexpr double kWanderRates[] = {0.31, 0.83, 0.47, 0.97, 0.37, 0.71, 0.53, 0.89,
                                   0.41, 0.67, 0.59, 0.79, 0.34, 0.61, 0.43, 0.73};
// Which half of the lines each side of the input goes into, and how each side of the output signs them:
// orthogonal patterns, so the two sides come out different from the start.
constexpr double kReverbInSigns[] = {1, 1, -1, 1, 1, -1, -1, 1, -1, 1, 1, -1, 1, -1, 1, 1};
constexpr double kReverbOutSigns[2][16] = {{1, -1, 1, 1, -1, 1, -1, -1, 1, 1, 1, -1, -1, -1, 1, -1},
                                           {1, 1, -1, 1, 1, 1, -1, -1, -1, 1, -1, 1, -1, 1, 1, 1}};
// A shimmer's pitch shifters, one a side on the tail going back in: two read heads each, sweeping a window this long
// (seconds) at least kShimmerNearest behind, and crossfading as they jump back.
constexpr double kShimmerWindow = 0.1;
constexpr double kShimmerNearest = 0.002;
// How much of the energy its tail gives back a shimmer feeds in again shifted, the Shimmer knob up full: under unity,
// so each step of the interval is fainter than the last, even before the band-pass takes its share.
constexpr double kShimmerLoop = 0.7;
// How much of the stretch that feedback would give the tail its lines' decay takes back. Not all of it: the band-pass
// takes its share each time round, more of it the higher a shift up climbs, so the tail lasts about the decay time
// with Shimmer anywhere (a little longer half way, a little shorter full up).
constexpr double kShimmerStretch = 0.6;
constexpr double kMaxPreDelay = 0.25;
constexpr double kMaxFlangerDelay = 0.032; // the longest center delay at full depth, with room
constexpr double kWidenerSweep = 0.12;    // how far the widener's taps are swept either way, of their delay

// Reads a delay line `delay` samples behind `write`, between samples.
double readBehind(const ZeroBuffer &line, int write, double delay) {
    const int size = static_cast<int>(line.size());
    double pos = write - delay;
    while (pos < 0.0)
        pos += size;
    const int i0 = std::min(static_cast<int>(pos), size - 1);
    const int i1 = i0 + 1 == size ? 0 : i0 + 1;
    return line[i0] + (line[i1] - line[i0]) * (pos - i0);
}

// The reverb's size knob: from a quarter of the lines' full length to all of it.
double reverbScale(double size) {
    return 0.25 + 0.75 * std::clamp(size, 0.0, 1.0);
}

// One stage of the fast Walsh-Hadamard transform below: pairs `half` apart. Its size known when compiled, the
// compiler lays each stage out without loops.
template <size_t half>
void hadamardStage(std::array<double, 16> &x) {
    for (size_t i = 0; i < x.size(); i += 2 * half)
        for (size_t j = i; j < i + half; ++j) {
            const double a = x[j], b = x[j + half];
            x[j] = a + b;
            x[j + half] = a - b;
        }
}

// The fast Walsh-Hadamard transform of the lines' outputs, scaled to keep their energy.
void hadamard(std::array<double, 16> &x) {
    hadamardStage<1>(x);
    hadamardStage<2>(x);
    hadamardStage<4>(x);
    hadamardStage<8>(x);
    for (double &v : x)
        v *= 0.25;
}

} // namespace

double phaserCorner(double centerNorm, double depth, double lfo) {
    return cutoffFromNorm(centerNorm) * std::exp2(2.5 * depth * lfo);
}

double flangerDelay(double delayNorm, double depth, double lfo) {
    return timeFromNorm(delayNorm) * (1.0 + 0.95 * depth * lfo);
}

double sweepFeedback(double feedback) {
    return 0.9 * std::clamp(feedback, -1.0, 1.0);
}

double sweepGain(double feedback) {
    const double g = sweepFeedback(feedback);
    return std::sqrt(1.0 - g * g);
}

double phaserResponse(double hz, double cornerHz, int stages, double feedback, double mix, double polarity) {
    using Complex = std::complex<double>;
    const Complex s(0.0, hz / cornerHz);
    const Complex stage = (1.0 - s) / (1.0 + s); // the analog first-order all-pass
    Complex chain = 1.0;
    for (int k = 0; k < stages; ++k)
        chain *= stage;
    const double g = polarity * sweepFeedback(feedback);
    return std::abs(1.0 - mix + polarity * mix * sweepGain(feedback) * chain / (1.0 - g * chain));
}

double flangerResponse(double hz, double delaySeconds, double feedback, double mix, double polarity) {
    using Complex = std::complex<double>;
    const Complex delay = std::polar(1.0, -2.0 * std::numbers::pi * hz * delaySeconds);
    const double g = polarity * sweepFeedback(feedback);
    return std::abs(1.0 - mix + polarity * mix * sweepGain(feedback) * delay / (1.0 - g * delay));
}

double reverbDecay(double norm) {
    return timeFromNorm(norm);
}

double reverbPreDelay(double norm) {
    return norm <= 0.0 ? 0.0 : timeFromNorm(norm);
}

ReverbStyle reverbStyle(ReverbType type) {
    switch (type) {
    case ReverbType::Room: // tighter wavering: a small room's resonances are fewer and plainer
        return {0.45, 0.6, 4, 0.55, 0.9, false};
    case ReverbType::Plate: // a plate's metal hardly drifts
        return {0.6, 1.8, 6, 0.0, 0.6, false};
    case ReverbType::Shimmer: // a hall, brighter, a little more chorused
        return {1.0, 1.3, 4, 0.0, 1.3, true};
    case ReverbType::Hall:
    case ReverbType::Count:
        break;
    }
    return {1.0, 1.0, 4, 0.0, 1.0, false};
}

ReverbType reverbType(double value) {
    return static_cast<ReverbType>(std::clamp(static_cast<int>(std::lround(value)), 0,
                                              static_cast<int>(ReverbType::Count) - 1));
}

double reverbSpace(double sizeNorm, ReverbType type) {
    return reverbScale(sizeNorm) * reverbStyle(type).length;
}

std::array<double, 2> Effects::Hilbert::process(double x) {
    constexpr double kA[] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
    constexpr double kB[] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
    // Each section is y[n] = c^2 (x[n] + y[n-2]) - x[n-2].
    auto chain = [](std::array<std::array<double, 4>, 4> &sections, const double (&c)[4], double in) {
        for (int i = 0; i < 4; ++i) {
            std::array<double, 4> &s = sections[i];
            const double y = c[i] * c[i] * (in + s[3]) - s[1];
            s[1] = s[0];
            s[0] = in;
            s[3] = s[2];
            s[2] = y;
            in = y;
        }
        return in;
    };
    const double first = chain(a, kA, x);
    const double second = chain(b, kB, x);
    const double inPhase = delayed;
    delayed = first;
    return {inPhase, -second}; // the second chain runs 90 degrees ahead: turned round, it lags like a Hilbert transform
}

double eqResponse(double hz, double low, double mid, double high) {
    using Complex = std::complex<double>;
    const Complex s1(0.0, hz / kEqLowHz);
    const Complex s2(0.0, hz / kEqHighHz);
    const Complex lp1 = 1.0 / (s1 * s1 + std::numbers::sqrt2 * s1 + 1.0);
    const Complex hp1 = s1 * s1 * lp1;
    const Complex lp2 = 1.0 / (s2 * s2 + std::numbers::sqrt2 * s2 + 1.0);
    const Complex hp2 = s2 * s2 * lp2;
    const Complex ap2 = (s2 * s2 - std::numbers::sqrt2 * s2 + 1.0) * lp2;
    return std::abs(eqGain(low) * ap2 * lp1 * lp1 + eqGain(mid) * hp1 * hp1 * lp2 * lp2 +
                    eqGain(high) * hp1 * hp1 * hp2 * hp2);
}

double peqResponse(double hz, PeqType type, double freq, double gainDb, double q) {
    // The analog filters the digital ones are made from.
    using Complex = std::complex<double>;
    const Complex s(0.0, hz / cutoffFromNorm(freq));
    const double a = std::pow(10.0, gainDb / 40.0);
    const double bandwidth = 1.0 / qFromNorm(q);
    const double root = std::sqrt(a) * bandwidth;
    switch (type) {
    case PeqType::LowShelf:
        return std::abs(a * (s * s + root * s + a) / (a * s * s + root * s + 1.0));
    case PeqType::HighShelf:
        return std::abs(a * (a * s * s + root * s + 1.0) / (s * s + root * s + a));
    case PeqType::LowCut:
        return std::abs(s * s / (s * s + bandwidth * s + 1.0));
    case PeqType::HighCut:
        return std::abs(1.0 / (s * s + bandwidth * s + 1.0));
    case PeqType::Notch:
        return std::abs((s * s + 1.0) / (s * s + bandwidth * s + 1.0));
    case PeqType::Peak:
    case PeqType::Count:
        break;
    }
    return std::abs((s * s + s * bandwidth * a + 1.0) / (s * s + s * bandwidth / a + 1.0));
}

void Effects::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    const size_t length = static_cast<size_t>(std::ceil(kMaxDelay * sampleRate)) + 4;
    for (Slot &slot : slots_) {
        for (ZeroBuffer &line : slot.delay.lines)
            line.assign(length, 0.0f);
        for (ZeroBuffer &comb : slot.filter.combs)
            comb.assign(static_cast<size_t>(combLength(sampleRate)), 0.0f);
        slot.filter.settingsFor.fill(std::numeric_limits<double>::quiet_NaN()); // for this rate

        // Room behind the longest line for its wavering, the most of any algorithm.
        const double reach = kReverbLongest + 2.0 * kReverbWander * reverbStyle(ReverbType::Shimmer).wander;
        for (ZeroBuffer &line : slot.reverb.lines)
            line.assign(static_cast<size_t>(std::ceil(reach * sampleRate)) + 4, 0.0f);
        for (ZeroBuffer &line : slot.reverb.shifter)
            line.assign(static_cast<size_t>(std::ceil((kShimmerNearest + kShimmerWindow) * sampleRate)) + 4, 0.0f);
        const double longestDiffuser = std::max(*std::max_element(std::begin(kDiffuserSeconds[0]), std::end(kDiffuserSeconds[0])),
                                                *std::max_element(std::begin(kDiffuserSeconds[1]), std::end(kDiffuserSeconds[1])));
        const size_t widest = static_cast<size_t>(std::ceil(longestDiffuser * sampleRate)) + 4;
        for (auto &side : slot.reverb.diffusers)
            for (ZeroBuffer &diffuser : side)
                diffuser.assign(widest, 0.0f);
        // Room for a room's latest early reflection behind the longest pre-delay.
        const double behind = kMaxPreDelay + kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1];
        for (ZeroBuffer &line : slot.reverb.preDelay)
            line.assign(static_cast<size_t>(std::ceil(behind * sampleRate)) + 4, 0.0f);
        const double longestTap = kWidenerDelays[kWidenerTaps - 1] * (1.0 + kWidenerSweep);
        slot.widener.line.assign(static_cast<size_t>(std::ceil(longestTap * sampleRate)) + 4, 0.0f);
        for (ZeroBuffer &line : slot.flanger.lines)
            line.assign(static_cast<size_t>(std::ceil(kMaxFlangerDelay * sampleRate)) + 4, 0.0f);
        Analyzer::prepare(slot.analyzed, static_cast<int>(&slot - slots_.data()), kFxSlots + kFilters);
    }
    analyzer_.prepare(sampleRate);
    static_assert(std::size(kReverbSeconds) == kReverbLines && std::size(kWanderRates) == kReverbLines &&
                  std::size(kReverbInSigns) == kReverbLines && std::size(kDiffuserSeconds[0]) == kDiffusers &&
                  std::size(kDiffuserGains) == kDiffusers);
    double inverses = 0.0;
    for (int i = 0; i < kReverbLines; ++i) {
        reverbLengths_[i] = kReverbSeconds[i] * sampleRate;
        inverses += 1.0 / reverbLengths_[i];
        const double step = 2.0 * std::numbers::pi * kWanderRates[i] / sampleRate;
        wanderTurn_[i] = {std::cos(step), std::sin(step)};
    }
    reverbMeanLength_ = kReverbLines / inverses;
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < kDiffusers; ++i)
            diffuserLengths_[side][i] = static_cast<int>(std::lround(kDiffuserSeconds[side][i] * sampleRate));
    auto set = [&](BiquadCoefs &c, BiquadType type, double hz) {
        const Coefs b = biquad(type, hz, sampleRate);
        c = {b.b0, b.b1, b.b2, b.a1, b.a2};
    };
    set(lowPass1_, BiquadType::LowPass, kEqLowHz);
    set(highPass1_, BiquadType::HighPass, kEqLowHz);
    set(lowPass2_, BiquadType::LowPass, kEqHighHz);
    set(highPass2_, BiquadType::HighPass, kEqHighHz);
    set(allPass2_, BiquadType::AllPass, kEqHighHz);
    reset();
}

void Effects::reset() {
    portLevel_.fill(0.0f);
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
    running_.fill(false);
    frozen_.fill(false);
    ranBefore_.fill(false);
    graphOrderCount_ = 0; // the first chunk's nodes start with their wires as set, at once (see orderGraph())
    looped_ = rewiring_ = false;
    duck_ = 1.0;
}

double Effects::delayTarget(int slot, int side, const ParamValues &p, double tempo) const {
    if (p[paramIndex(slot, FxParam::DelayLink)] >= 0.5)
        side = 0; // linked, the right side waits as long as the left
    const double seconds =
        p[paramIndex(slot, FxParam::DelaySync)] >= 0.5
            ? divisionSeconds(p[paramIndex(slot, side == 0 ? FxParam::DelayDivisionL : FxParam::DelayDivisionR)], tempo)
            : timeFromNorm(p[paramIndex(slot, side == 0 ? FxParam::DelayTimeL : FxParam::DelayTimeR)]);
    const double size = static_cast<double>(slots_[slot].delay.lines[0].size());
    return std::clamp(seconds * sampleRate_, 1.0, size - 2.0);
}

// Clears an effect that is about to fade in, and jumps its ramps to the current settings.
void Effects::start(int slot, const ParamValues &p, double tempo) {
    Slot &s = slots_[slot];
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    switch (s.running) {
    case FxType::Shaper: {
        const ShaperSettings settings = shaperSettings([&p](int index) { return p[index]; }, shaperParams(slot));
        s.shaper = Shaper{};
        // Its drawn curve too, taken at once as it starts rather than gliding in from the one it last ran with (a
        // patch or two ago, say).
        s.curve.built = false;
        s.curve.points.fill(std::numeric_limits<double>::quiet_NaN());
        s.shaper.drive = settings.drive;
        s.shaper.output = settings.output;
        s.shaper.mix = settings.mix;
        break;
    }
    case FxType::Delay: {
        Delay &d = s.delay;
        for (ZeroBuffer &line : d.lines) // only what it ran through: lines 4 s long would take a while to clear
            line.zero(d.ran);
        d.ran = 0;
        d.write = 0;
        d.length = {delayTarget(slot, 0, p, tempo), delayTarget(slot, 1, p, tempo)};
        d.lowPass = {};
        d.highPass = {};
        d.feedback = at(FxParam::DelayFeedback);
        d.dry = delayDryLevel(at(FxParam::DelayMix));
        d.wet = delayWetLevel(at(FxParam::DelayMix));
        d.width = at(FxParam::DelayWidth);
        break;
    }
    case FxType::Eq:
        s.eq.channels = {};
        s.eq.gains = {eqGain(at(FxParam::EqLow)), eqGain(at(FxParam::EqMid)), eqGain(at(FxParam::EqHigh))};
        break;
    case FxType::ParamEq:
        s.peq = ParamEq{};
        s.peq.amount = at(FxParam::PeqAmount);
        s.analyzed.clear();
        break;
    case FxType::Filter:
        s.filter.channels = {};
        for (ZeroBuffer &comb : s.filter.combs)
            comb.zero();
        s.filter.mix = at(FxParam::FilterMix);
        s.analyzed.clear();
        break;
    case FxType::Compressor:
        s.comp = {0.0, dbToGain(at(FxParam::CompMakeup)), at(FxParam::CompMix)};
        break;
    case FxType::MultiDyn:
        s.dyn = MultiDyn{};
        for (int band = 0; band < kMcBands; ++band)
            s.dyn.gains[band] = dbToGain(p[mdGainIndex(slot, band)]);
        s.dyn.mix = at(FxParam::MdMix);
        s.dyn.output = dbToGain(at(FxParam::MdOutput));
        break;
    case FxType::Reverb: {
        Reverb &r = s.reverb;
        for (ZeroBuffer &line : r.lines)
            line.zero(r.ran);
        for (auto &side : r.diffusers)
            for (ZeroBuffer &diffuser : side)
                diffuser.zero(r.ran);
        for (ZeroBuffer &line : r.preDelay)
            line.zero(r.ran);
        for (ZeroBuffer &line : r.shifter)
            line.zero(r.ran);
        r.ran = 0;
        r.lineWrite = r.diffuserWrite = r.preWrite = r.shiftWrite = 0;
        r.shimmerPhase = 0.0;
        r.shimmerLow = {};
        r.shimmerBass = {};
        r.damp = {};
        r.gainsFor = {std::numeric_limits<double>::quiet_NaN()}; // the sample rate may have changed
        for (int i = 0; i < kReverbLines; ++i) { // each line's wavering starts somewhere else
            const double angle = 2.0 * std::numbers::pi * i * 0.618034;
            r.wanderCos[i] = std::cos(angle);
            r.wanderSin[i] = std::sin(angle);
        }
        r.shimmer = 0.0; // fades in with the rest
        r.size = reverbSpace(at(FxParam::ReverbSize), reverbType(at(FxParam::ReverbType)));
        r.preLength = reverbPreDelay(at(FxParam::ReverbPreDelay)) * sampleRate_;
        r.dry = delayDryLevel(at(FxParam::ReverbMix));
        r.wet = delayWetLevel(at(FxParam::ReverbMix));
        r.width = at(FxParam::ReverbWidth);
        break;
    }
    case FxType::Phaser:
        s.phaser = Phaser{};
        s.phaser.mix = sweepWet(at(FxParam::PhaserMix));
        s.phaser.polarity = sweepPolarity(at(FxParam::PhaserNegative));
        break;
    case FxType::Flanger:
        for (ZeroBuffer &line : s.flanger.lines)
            line.zero(s.flanger.ran);
        s.flanger.ran = 0;
        s.flanger.write = 0;
        s.flanger.last = {};
        s.flanger.phase = 0.0;
        s.flanger.delayKnown = false;
        s.flanger.mix = sweepWet(at(FxParam::FlangerMix));
        s.flanger.polarity = sweepPolarity(at(FxParam::FlangerNegative));
        break;
    case FxType::FreqShifter:
        s.shifter = Shifter{};
        s.shifter.mix = at(FxParam::ShiftMix);
        break;
    case FxType::Disperser:
        s.disperser = Disperser{};
        s.disperser.mix = at(FxParam::DisperserMix);
        s.analyzed.clear();
        break;
    case FxType::Widener: {
        Widener &e = s.widener;
        e.line.zero(e.ran);
        e.ran = 0;
        e.write = 0;
        e.highPass = Biquad{};
        e.phases = {0.0, 0.25, 0.5, 0.75}; // apart from the start, so the sides never sweep together
        e.size = at(FxParam::WidenerSize);
        e.width = at(FxParam::WidenerWidth);
        e.mix = at(FxParam::WidenerMix);
        break;
    }
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
        orderGraph(p, true);
        rewiring_ = looped_;
    }
    if (rewiring_)
        orderGraph(p, false);
    const double duckStep = 1.0 / (0.003 * sampleRate_);
    for (uint32_t done = 0; done < frames; done += kChunk) {
        const uint32_t n = std::min(kChunk, frames - done);
        float *const io[2] = {left + done, right + done};
        // Silence, with nothing left ringing and nothing on its way: it would come out as silence, so it does without
        // the effects' work (a host that keeps processing a quiet instance, as VST3 hosts do, pays nothing for them).
        if (idleChunk(io, n, p)) {
            idleSweeps(n, p, tempo);
            measurePorts(n);
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
    if (tail_ > 0 || analyzing_ || flushing_ || rewiring_ || duck_ < 1.0)
        return false;
    for (uint32_t s = 0; s < n; ++s)
        if (io[0][s] != 0.0f || io[1][s] != 0.0f)
            return false;
    auto wiresSettled = [&](int node) {
        for (int input = 0; input < fxInputCount(node); ++input) {
            const Wire &w = wires_[node][input];
            const int set = fxPortOf(p[fxInputIndex(node, input, FxInputField::From)]);
            if (w.now.port != set || w.was.port >= 0 ||
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
        const bool phaser = fx.running == FxType::Phaser;
        if (!phaser && fx.running != FxType::Flanger)
            continue;
        double &phase = phaser ? fx.phaser.phase : fx.flanger.phase;
        phase += sweepHz([&p](int k) { return p[k]; }, slot, phaser, tempo) / sampleRate_ * n;
        phase -= std::floor(phase);
        (phaser ? fx.phaser.coefKnown : fx.flanger.delayKnown) = false; // worked out again where the sweep is now
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
    if (!on && fx.fade <= 0.0) {
        reduction_[slot] = {};
        bandLevel_[slot] = {};
        bandGain_[slot] = {};
        fx.analyzed.bands = {};
        return;
    }
    if (fx.fade <= 0.0)
        start(slot, p, tempo);
    float dry[2][kChunk];
    std::copy_n(io[0], n, dry[0]);
    std::copy_n(io[1], n, dry[1]);
    switch (fx.running) {
    case FxType::Shaper:
        runShaper(slot, io, n, p);
        break;
    case FxType::Delay:
        // While anything goes in or comes out, another echo may follow.
        if (runDelay(slot, io, n, p, tempo) > 1e-5)
            keep = std::max(keep, static_cast<int64_t>(std::max(fx.delay.length[0], fx.delay.length[1]) +
                                                       0.05 * sampleRate_));
        break;
    case FxType::Eq:
        runEq(slot, io, n, p);
        break;
    case FxType::ParamEq:
        runParamEq(slot, io, n, p);
        break;
    case FxType::Filter:
        // A resonant comb rings on; keep going while it does.
        if (runFilter(slot, io, n, p, pitch) > 1e-5)
            keep = std::max(keep, static_cast<int64_t>(0.05 * sampleRate_));
        feedAnalyzer(slot, io, n);
        break;
    case FxType::Compressor:
    case FxType::MultiDyn:
        // A gain still releasing keeps them going, so the next sound doesn't start where it was left.
        if (fx.running == FxType::Compressor ? runCompressor(slot, io, n, p) : runMultiDyn(slot, io, n, p))
            keep = std::max(keep, static_cast<int64_t>(0.05 * sampleRate_));
        break;
    case FxType::Reverb:
    case FxType::Phaser:
    case FxType::Flanger:
    case FxType::FreqShifter:
    case FxType::Disperser:
    case FxType::Widener: {
        // Tails and feedback ring on; keep going while they do. A reverb's input may still wait in its
        // pre-delay, as a delay's in its lines.
        const double peak = fx.running == FxType::Reverb      ? runReverb(slot, io, n, p)
                            : fx.running == FxType::Phaser    ? runPhaser(slot, io, n, p, tempo)
                            : fx.running == FxType::Flanger   ? runFlanger(slot, io, n, p, tempo)
                            : fx.running == FxType::Disperser ? runDisperser(slot, io, n, p)
                            : fx.running == FxType::Widener  ? runWidener(slot, io, n, p)
                                                              : runShifter(slot, io, n, p);
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
    for (uint32_t s = 0; s < n; ++s) {
        fx.fade = std::clamp(fx.fade + (on ? fadeStep : -fadeStep), 0.0, 1.0);
        for (int c = 0; c < 2; ++c)
            io[c][s] = static_cast<float>(dry[c][s] + (io[c][s] - dry[c][s]) * fx.fade);
    }
}

Effects::CrossoverCoefs Effects::crossoverCoefs(double lowHz, double highHz) const {
    // Kept below where the biquads hold up, the high crossover at least a third of an octave above the low one.
    lowHz = std::min(lowHz, 0.3 * sampleRate_);
    highHz = std::clamp(highHz, lowHz * 1.26, 0.45 * sampleRate_);
    auto coefs = [&](BiquadType type, double hz) {
        const Coefs b = biquad(type, hz, sampleRate_);
        return BiquadCoefs{b.b0, b.b1, b.b2, b.a1, b.a2};
    };
    return {coefs(BiquadType::LowPass, lowHz), coefs(BiquadType::HighPass, lowHz), coefs(BiquadType::LowPass, highHz),
            coefs(BiquadType::HighPass, highHz), coefs(BiquadType::AllPass, highHz)};
}

void Effects::splitThree(EqChannel &e, double x, const CrossoverCoefs &c, double *bands) {
    bands[0] = e.lowPhase.process(e.lowB.process(e.lowA.process(x, c.lp1), c.lp1), c.ap2);
    const double rest = e.restB.process(e.restA.process(x, c.hp1), c.hp1);
    bands[1] = e.midB.process(e.midA.process(rest, c.lp2), c.lp2);
    bands[2] = e.highB.process(e.highA.process(rest, c.hp2), c.hp2);
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

double Effects::wireTarget(int node, int input, const ParamValues &p) const {
    const double set = p[fxInputIndex(node, input, FxInputField::Level)];
    const int sum = fxNodeSum(node);
    const double gain = sum >= 0 ? sumLevelGain(set) * sumSoloGain(sum, input, [&p](int i) { return p[i]; })
                                 : set; // a sum's in dB, and soloed as a join
    return std::isfinite(gain) ? std::clamp(gain, 0.0, 1.0) : 0.0;
}

void Effects::setWires(int node, const ParamValues &p) {
    for (int input = 0; input < fxInputCount(node); ++input) {
        const int port = fxPortOf(p[fxInputIndex(node, input, FxInputField::From)]);
        wires_[node][input] = {{port, wireTarget(node, input, p), port >= 0 ? 1.0 : 0.0}, {}};
    }
}

void Effects::idleSlot(int slot) {
    slots_[slot].fade = 0.0; // so start() clears it when it runs again
    reduction_[slot] = {};
    bandLevel_[slot] = {};
    bandGain_[slot] = {};
    slots_[slot].analyzed.bands = {};
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
            setWires(node, p);
        } else if (!runs && ranBefore_[node]) { // nothing hears it any more
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
    // One end of a wire into the mix, its level gliding to `target` across the chunk (or, in a chunk shorter than the
    // shortest ramp, that far toward it) as it fades toward `fadeTo` along a raised cosine over 5 ms, as a
    // parametric EQ band switched does.
    auto add = [&](WireEnd &e, double target, double fadeTo) {
        if (e.port < 0)
            return;
        if (e.fade <= 0.0 && fadeTo <= 0.0) { // out: free for the next move
            e.port = -1;
            return;
        }
        if (e.level < 0.0) // takes its level at once as it fades in
            e.level = target;
        const float *from[2] = {nullptr, nullptr};
        const bool heard = portOut(e.port, from); // a node not running reads silence
        const double step = rampStep(e.level, target, n);
        const double end = n >= kRampShortest ? target : e.level + step * n;
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
        // again while the one before still fades out, it fades out where it is first and only then takes the move.
        const int set = fxPortOf(p[fxInputIndex(node, input, FxInputField::From)]);
        if (set != w.now.port) {
            if (w.now.fade <= 0.0) {
                w.now = {set, -1.0, 0.0};
            } else if (w.was.fade <= 0.0) {
                w.was = w.now;
                w.now = {set, -1.0, 0.0};
            }
        }
        const bool asSet = w.now.port == set;
        add(w.now, asSet ? wireTarget(node, input, p) : w.now.level, asSet ? 1.0 : 0.0);
        add(w.was, w.was.level, 0.0);
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
        f.coefs = crossoverCoefs(hz[0], hz[1]);
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
        const Coefs lp = biquad(BiquadType::LowPass, low, sampleRate_);
        const Coefs hp = biquad(BiquadType::HighPass, low, sampleRate_);
        f.coefs.lp1 = {lp.b0, lp.b1, lp.b2, lp.a1, lp.a2};
        f.coefs.hp1 = {hp.b0, hp.b1, hp.b2, hp.a1, hp.a2};
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

void Effects::measurePorts(uint32_t n) {
    // Only the editor reads them: with none analyzing, they read nothing, and start from there when one does.
    if (!analyzing_) {
        portLevel_.fill(0.0f);
        return;
    }
    // This chunk's peak of each port that ran, against the last level fallen by the chunk's length.
    std::array<float, kFxPorts> peak{};
    auto peakOf = [n](const std::array<std::array<float, kChunk>, 2> &out) {
        float most = 0.0f;
        for (const auto &side : out)
            for (uint32_t s = 0; s < n; ++s)
                most = std::max(most, std::abs(side[s]));
        return most;
    };
    peak[kFxPortIn] = peakOf(input_);
    for (int i = 0; i < graphOrderCount_; ++i) {
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

// The voices' shapers run the same way (see Synth::renderBlock()), on what goes into them.
void Effects::runShaper(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Slot &place = slots_[slot];
    Shaper &sh = place.shaper;
    auto get = [&p](int index) { return p[index]; };
    const ShaperParams fields = shaperParams(slot);
    const ShaperSettings settings = shaperSettings(get, fields);
    if (settings.mode == ClipMode::Custom) { // a drawn curve glides to new points
        place.curve.update(get, fields.curve);
        place.curve.advance(n / sampleRate_);
    }
    ClipCurve curve = place.curve.curve(settings.mode, settings.symmetric);
    const double fadeStep = 1.0 / n;
    const bool block = clipNeedsBlocker(settings.mode);
    const double pole = clipBlockerPole(sampleRate_);
    const double driveStep = rampStep(sh.drive, settings.drive, n);
    const double outputStep = rampStep(sh.output, settings.output, n);
    const double mixStep = rampStep(sh.mix, settings.mix, n);
    for (uint32_t s = 0; s < n; ++s) {
        sh.drive += driveStep;
        sh.output += outputStep;
        sh.mix += mixStep;
        if (curve.from)
            curve.fade = (s + 1) * fadeStep;
        for (int ch = 0; ch < 2; ++ch)
            io[ch][s] = static_cast<float>(sh.sides[ch].run(curve, io[ch][s], sh.drive, sh.output, sh.mix, block, pole));
    }
}

double Effects::runDelay(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo) {
    Delay &d = slots_[slot].delay;
    d.ran += n; // counted ahead of what this chunk writes (see start())
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const double targets[2] = {delayTarget(slot, 0, p, tempo), delayTarget(slot, 1, p, tempo)};
    const double glide = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_)); // time changes glide, like tape
    const double mix = at(FxParam::DelayMix);
    const double feedbackStep = rampStep(d.feedback, at(FxParam::DelayFeedback), n);
    const double dryStep = rampStep(d.dry, delayDryLevel(mix), n);
    const double wetStep = rampStep(d.wet, delayWetLevel(mix), n);
    const double widthStep = rampStep(d.width, at(FxParam::DelayWidth), n);
    // The low-pass fades in over the first tenth of its travel, so the tone knob has no step at the middle.
    const double toneValue = at(FxParam::DelayTone);
    const DelayTone tone = delayTone(toneValue);
    const double lowPass = tone.lowPassHz > 0.0 ? onePole(tone.lowPassHz, sampleRate_) : 1.0;
    const double lowPassAmount = std::clamp(-toneValue * 10.0, 0.0, 1.0);
    const double highPass = onePole(tone.highPassHz, sampleRate_);
    const bool pingPong = at(FxParam::DelayPingPong) >= 0.5;
    const int size = static_cast<int>(d.lines[0].size());

    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        d.feedback += feedbackStep;
        d.dry += dryStep;
        d.wet += wetStep;
        d.width += widthStep;
        double wet[2];
        for (int c = 0; c < 2; ++c) {
            d.length[c] += (targets[c] - d.length[c]) * glide;
            double pos = d.write - d.length[c];
            if (pos < 0.0)
                pos += size;
            const int i0 = std::min(static_cast<int>(pos), size - 1);
            const int i1 = i0 + 1 == size ? 0 : i0 + 1;
            const float *line = d.lines[c].data();
            const double read = line[i0] + (line[i1] - line[i0]) * (pos - i0);
            // Each echo passes the tone filters again, so repeats get darker (or thinner).
            d.lowPass[c] += lowPass * (read - d.lowPass[c]);
            const double toned = read + (d.lowPass[c] - read) * lowPassAmount;
            d.highPass[c] += highPass * (toned - d.highPass[c]);
            wet[c] = toned - d.highPass[c];
        }
        const double x[2] = {io[0][s], io[1][s]};
        // Ping-pong: the input goes in on the left, and each echo crosses to the other side.
        d.lines[0][d.write] = static_cast<float>(pingPong ? 0.5 * (x[0] + x[1]) + d.feedback * wet[1]
                                                          : x[0] + d.feedback * wet[0]);
        d.lines[1][d.write] = static_cast<float>(pingPong ? d.feedback * wet[0] : x[1] + d.feedback * wet[1]);
        d.write = d.write + 1 == size ? 0 : d.write + 1;
        // Width narrows what comes out toward the middle; the echoes themselves keep their sides.
        const double middle = 0.5 * (wet[0] + wet[1]);
        const double side = 0.5 * (wet[0] - wet[1]) * d.width;
        io[0][s] = static_cast<float>(x[0] * d.dry + (middle + side) * d.wet);
        io[1][s] = static_cast<float>(x[1] * d.dry + (middle - side) * d.wet);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(wet[0]), std::abs(wet[1])});
    }
    return peak;
}

void Effects::runEq(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Eq &eq = slots_[slot].eq;
    const double targets[3] = {eqGain(p[paramIndex(slot, FxParam::EqLow)]), eqGain(p[paramIndex(slot, FxParam::EqMid)]),
                               eqGain(p[paramIndex(slot, FxParam::EqHigh)])};
    double steps[3];
    for (int b = 0; b < 3; ++b)
        steps[b] = rampStep(eq.gains[b], targets[b], n);
    for (uint32_t s = 0; s < n; ++s) {
        for (int b = 0; b < 3; ++b)
            eq.gains[b] += steps[b];
        for (int ch = 0; ch < 2; ++ch) {
            EqChannel &e = eq.channels[ch];
            const double x = io[ch][s];
            const double low = e.lowPhase.process(e.lowB.process(e.lowA.process(x, lowPass1_), lowPass1_), allPass2_);
            const double rest = e.restB.process(e.restA.process(x, highPass1_), highPass1_);
            const double mid = e.midB.process(e.midA.process(rest, lowPass2_), lowPass2_);
            const double high = e.highB.process(e.highA.process(rest, highPass2_), highPass2_);
            io[ch][s] = static_cast<float>(eq.gains[0] * low + eq.gains[1] * mid + eq.gains[2] * high);
        }
    }
}

void Effects::runParamEq(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    ParamEq &eq = slots_[slot].peq;
    float dry[2][kChunk];
    std::copy_n(io[0], n, dry[0]);
    std::copy_n(io[1], n, dry[1]);
    // New coefficients every chunk are smooth enough for knob moves and modulation. A band switched on or off fades
    // between what it puts out and what it takes in, over 5 ms. One switched off keeps running unheard, so it comes
    // back without starting from silence.
    const double fade = 1.0 / (0.005 * sampleRate_);
    for (int band = 0; band < kPeqBands; ++band) {
        const double target = p[peqOnIndex(slot, band)] >= 0.5 ? 1.0 : 0.0;
        double &on = eq.on[band];
        if (std::isnan(on))
            on = target;
        const PeqType type = peqType(p[peqTypeIndex(slot, band)]);
        const double gain = p[peqGainIndex(slot, band)];
        if (peqHasGain(type) && std::abs(gain) < 1e-3) {
            eq.bands[band] = {}; // flat: leave the band out, and start it clean when it comes back
            on = target;
            continue;
        }
        const Coefs b = peqCoefs(type, p[peqFreqIndex(slot, band)], gain, p[peqQIndex(slot, band)], sampleRate_);
        const BiquadCoefs c{b.b0, b.b1, b.b2, b.a1, b.a2};
        if (on == target) {
            for (int ch = 0; ch < 2; ++ch) {
                Biquad &f = eq.bands[band][ch];
                if (on == 1.0)
                    for (uint32_t s = 0; s < n; ++s)
                        io[ch][s] = static_cast<float>(f.process(io[ch][s], c));
                else
                    for (uint32_t s = 0; s < n; ++s)
                        f.process(io[ch][s], c);
            }
            continue;
        }
        // Along a raised cosine, which starts and ends without a kink.
        for (uint32_t s = 0; s < n; ++s) {
            on = target > on ? std::min(on + fade, target) : std::max(on - fade, target);
            const double heard = 0.5 - 0.5 * std::cos(std::numbers::pi * on);
            for (int ch = 0; ch < 2; ++ch) {
                const double x = io[ch][s];
                io[ch][s] = static_cast<float>(x + heard * (eq.bands[band][ch].process(x, c) - x));
            }
        }
    }
    // Amount mixes what the bands put out with what came in, ramping to a new value over the chunk.
    const double target = p[paramIndex(slot, FxParam::PeqAmount)];
    if (eq.amount != 1.0 || target != 1.0) {
        const double step = rampStep(eq.amount, target, n);
        for (uint32_t s = 0; s < n; ++s) {
            eq.amount += step;
            for (int ch = 0; ch < 2; ++ch)
                io[ch][s] = static_cast<float>(dry[ch][s] + eq.amount * (io[ch][s] - dry[ch][s]));
        }
        if (std::abs(eq.amount - target) < 1e-9) // arrived, but for rounding
            eq.amount = target;
    }
    feedAnalyzer(slot, io, n);
}

// What a parametric EQ, a filter or a phase disperser puts out goes to its analyzer, while the editor is open.
void Effects::feedAnalyzer(int slot, float *const *io, uint32_t n) {
    if (!analyzing_)
        return;
    Slot &place = slots_[slot];
    for (uint32_t s = 0; s < n; ++s)
        analyzer_.push(place.analyzed, 0.5f * (io[0][s] + io[1][s]));
}

double Effects::runFilter(int slot, float *const *io, uint32_t n, const ParamValues &p, double pitch) {
    FilterFx &filter = slots_[slot].filter;
    // Worked out again only when its params, or the pitch it tracks, move. Without key tracking the pitch moves
    // nothing (2 to the power of 0 is exactly 1), and is left out.
    const double keyTrack = p[paramIndex(slot, FxParam::FilterKeyTrack)];
    const std::array<double, 7> inputs{p[paramIndex(slot, FxParam::FilterType)],
                                       p[paramIndex(slot, FxParam::FilterCutoff)],
                                       keyTrack,
                                       keyTrack != 0.0 ? pitch : 0.0,
                                       p[paramIndex(slot, FxParam::FilterSlope)],
                                       p[paramIndex(slot, FxParam::FilterResonance)],
                                       p[paramIndex(slot, FxParam::FilterDrive)]};
    if (std::memcmp(inputs.data(), filter.settingsFor.data(), sizeof inputs) != 0) {
        filter.settingsFor = inputs;
        const auto type = static_cast<FilterType>(
            std::clamp(static_cast<int>(std::lround(inputs[0])), 0, static_cast<int>(FilterType::Count) - 1));
        // Key tracking moves the cutoff with the pitch from C4 on, as a voice filter's does.
        const double cutoffHz = cutoffFromNorm(inputs[1]) * std::exp2((pitch - 60.0) / 12.0 * keyTrack);
        filter.settings = filterSettings(type, filterSlope(inputs[4]), cutoffHz, inputs[5], inputs[6], sampleRate_);
    }
    const FilterSettings &settings = filter.settings;
    const double mixStep = rampStep(filter.mix, p[paramIndex(slot, FxParam::FilterMix)], n);
    const int combSize = static_cast<int>(filter.combs[0].size());
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        filter.mix += mixStep;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            const double y = filter.channels[ch].process(x, settings, filter.combs[ch].data(), combSize);
            io[ch][s] = static_cast<float>(x + (y - x) * filter.mix);
            peak = std::max(peak, std::abs(y));
        }
    }
    return peak;
}

bool Effects::runCompressor(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Compressor &c = slots_[slot].comp;
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const double threshold = at(FxParam::CompThreshold);
    const double ratio = at(FxParam::CompRatio);
    const double knee = at(FxParam::CompKnee);
    const double attack = smoothing(timeFromNorm(at(FxParam::CompAttack)), sampleRate_);
    const double release = smoothing(timeFromNorm(at(FxParam::CompRelease)), sampleRate_);
    const double makeupStep = rampStep(c.makeup, dbToGain(at(FxParam::CompMakeup)), n);
    const double mixStep = rampStep(c.mix, at(FxParam::CompMix), n);
    // Below the knee the curve leaves the level as it is (see compressorCurve()): compared as a level, it takes no
    // logarithm.
    const double quiet = dbToGain(threshold - 0.5 * std::max(knee, 0.0));
    double most = 0.0;
    // The reduction as a gain, worked out again only when it moves: below the threshold it rests at 0 dB.
    double gainOf = std::numeric_limits<double>::quiet_NaN(), reductionGain = 1.0;
    bool moving = false;
    for (uint32_t s = 0; s < n; ++s) {
        c.makeup += makeupStep;
        c.mix += mixStep;
        // Both sides follow the louder one, so the stereo image stays put.
        const double peak = std::max(std::abs(io[0][s]), std::abs(io[1][s]));
        moving = followReduction(c.reduction, peak, threshold, ratio, knee, attack, release, quiet);
        const double reduction = c.reduction;
        most = std::min(most, reduction);
        if (reduction != gainOf) {
            gainOf = reduction;
            reductionGain = dbToGain(reduction);
        }
        const double gain = reductionGain * c.makeup;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            io[ch][s] = static_cast<float>(x + (x * gain - x) * c.mix);
        }
    }
    reduction_[slot] = {static_cast<float>(-most), 0.0f, 0.0f};
    return moving;
}

bool Effects::runMultiDyn(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    MultiDyn &m = slots_[slot].dyn;
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const CrossoverCoefs crossovers = crossoverCoefs(cutoffFromNorm(at(FxParam::MdLowFreq)),
                                                     cutoffFromNorm(at(FxParam::MdHighFreq)));
    const double attack = smoothing(timeFromNorm(at(FxParam::MdAttack)), sampleRate_);
    const double release = smoothing(timeFromNorm(at(FxParam::MdRelease)), sampleRate_);
    const double knee = at(FxParam::MdKnee);
    const double range = at(FxParam::MdRange);
    const bool rms = at(FxParam::MdRms) >= 0.5;
    const double average = 1.0 - smoothing(kMdRmsSeconds, sampleRate_);
    const double peakFall = smoothing(kMdPeakSeconds, sampleRate_);
    const double fall = smoothing(0.33, sampleRate_); // the meters fall about 26 dB a second
    std::array<double, kMcBands> above{}, down{}, below{}, up{}, gainStep{}, flatFrom{}, flatTo{};
    for (int b = 0; b < kMcBands; ++b) { // each band its own; Link is the editor's, which edits them together
        above[b] = p[mdAboveIndex(slot, b)];
        down[b] = dynRatio(p[mdDownIndex(slot, b)]);
        below[b] = p[mdBelowIndex(slot, b)];
        up[b] = dynRatio(p[mdUpIndex(slot, b)]);
        gainStep[b] = rampStep(m.gains[b], dbToGain(p[mdGainIndex(slot, b)]), n);
        // Between the Below threshold's knee and the Above one's, the curve leaves the level as it is (see
        // dynamicsGain()): compared as a level, it takes no logarithm. Empty when the knees overlap.
        flatFrom[b] = dbToGain(std::min(below[b], above[b]) + 0.5 * std::max(knee, 0.0));
        flatTo[b] = dbToGain(above[b] - 0.5 * std::max(knee, 0.0));
    }
    const double mixStep = rampStep(m.mix, at(FxParam::MdMix), n);
    const double outputStep = rampStep(m.output, dbToGain(at(FxParam::MdOutput)), n);
    std::array<double, kMcBands> most{}, loudest{};
    // Each band's gain as a factor, worked out again only when it moves: between the thresholds it rests at 0 dB.
    std::array<double, kMcBands> gainOf, bandFactor{};
    gainOf.fill(std::numeric_limits<double>::quiet_NaN());
    bool moving = false; // whether a band's gain is still on its way to its curve's, in the chunk's last sample
    for (uint32_t s = 0; s < n; ++s) {
        moving = false;
        m.mix += mixStep;
        m.output += outputStep;
        double bands[2][kMcBands];
        for (int ch = 0; ch < 2; ++ch)
            splitThree(m.channels[ch], io[ch][s], crossovers, bands[ch]);
        double gain[kMcBands];
        for (int b = 0; b < kMcBands; ++b) {
            m.gains[b] += gainStep[b];
            // Both sides follow the louder one, so the stereo image stays put.
            const double peak = std::max(std::abs(bands[0][b]), std::abs(bands[1][b]));
            m.power[b] += (peak * peak - m.power[b]) * average;
            m.peak[b] = std::max(peak, m.peak[b] * peakFall);
            const double level = rms ? std::sqrt(m.power[b]) : m.peak[b];
            m.meter[b] = std::max(level, m.meter[b] * fall);
            loudest[b] = std::max(loudest[b], m.meter[b]);
            const double target = level > flatFrom[b] && level < flatTo[b]
                                      ? 0.0
                                      : dynamicsGain(20.0 * std::log10(std::max(level, 1e-9)), above[b], down[b],
                                                     below[b], up[b], knee, range);
            m.gain[b] = followGain(m.gain[b], target, attack, release);
            moving = moving || m.gain[b] != target;
            most[b] = std::min(most[b], m.gain[b]);
            if (m.gain[b] != gainOf[b]) {
                gainOf[b] = m.gain[b];
                bandFactor[b] = dbToGain(m.gain[b]);
            }
            gain[b] = bandFactor[b] * m.gains[b];
        }
        // The dry side is the bands' plain sum, so a partial mix doesn't comb with the crossovers' phase shift.
        for (int ch = 0; ch < 2; ++ch) {
            const double dry = bands[ch][0] + bands[ch][1] + bands[ch][2];
            const double wet = bands[ch][0] * gain[0] + bands[ch][1] * gain[1] + bands[ch][2] * gain[2];
            io[ch][s] = static_cast<float>((dry + (wet - dry) * m.mix) * m.output);
        }
    }
    for (int b = 0; b < kMcBands; ++b) {
        reduction_[slot][b] = static_cast<float>(-most[b]);
        bandLevel_[slot][b] = static_cast<float>(loudest[b]);
        bandGain_[slot][b] = static_cast<float>(m.gain[b]);
    }
    return moving;
}

double Effects::runReverb(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Reverb &r = slots_[slot].reverb;
    r.ran += n; // counted ahead of what this chunk writes (see start())
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate_)); // size and pre-delay changes glide
    const ReverbType type = reverbType(at(FxParam::ReverbType));
    const ReverbStyle style = reverbStyle(type);
    for (int c = 0; c < 2; ++c)
        for (int d = r.diffusersUsed; d < style.diffusers; ++d)
            r.diffusers[c][d].zero(r.ran);
    r.diffusersUsed = style.diffusers;
    const double sizeTarget = reverbSpace(at(FxParam::ReverbSize), type); // a new algorithm's length glides too
    const double preTarget = reverbPreDelay(at(FxParam::ReverbPreDelay)) * sampleRate_;
    // A room's early reflections spread with its size, after the pre-delay; they stay inside the buffer.
    const double echoScale = reverbScale(at(FxParam::ReverbSize)) * sampleRate_;
    const double echoRoom = static_cast<double>(r.preDelay[0].size()) - 2.0;
    const double mix = at(FxParam::ReverbMix);
    const double dryStep = rampStep(r.dry, delayDryLevel(mix), n);
    const double wetStep = rampStep(r.wet, delayWetLevel(mix), n);
    const double widthStep = rampStep(r.width, at(FxParam::ReverbWidth), n);
    constexpr double kInGain = 0.35355339059327373; // 1 / sqrt(8): each side goes into half the lines
    constexpr double kOutGain = 0.22; // about 1 / sqrt(16), a little under, for the level a mono sound had before
    // A shimmer feeds its tail back into its lines, band-passed and pitch-shifted, so each time round part of it
    // climbs (or falls) by the interval. It goes in past the all-passes: run through them again each time round,
    // the shifted tail would ring metallic. The share of the tail's energy that comes back, the Shimmer knob's times
    // kShimmerLoop, sets how much. Within a semitone the shifted tail would much be the tail itself, ringing on
    // rather than shimmering, so the loop fades out there.
    const double pitch = std::clamp(at(FxParam::ReverbShimmerPitch), -24.0, 24.0);
    const double ratio = std::exp2(pitch / 12.0);
    const double share = style.shimmer ? kShimmerLoop * std::clamp(at(FxParam::ReverbShimmer), 0.0, 1.0) *
                                             std::min(std::abs(pitch), 1.0)
                                       : 0.0;
    // Each line's gain takes it down 60 dB in the decay time, for the length it has at this chunk's size, and its
    // shelf as much again kReverbHighDecay as fast above the damping's corner: 20 kHz down to 1 kHz. What a shimmer
    // feeds back would stretch its tail by 1 / (1 - share) were nothing lost on the way, so its lines fall faster,
    // by kShimmerStretch of that, and the tail as a whole still takes about the decay time.
    const double corner =
        onePole(20000.0 * std::pow(0.05, at(FxParam::ReverbDamping)) * style.brightness, sampleRate_);
    const double decay = reverbDecay(at(FxParam::ReverbDecay)) * (1.0 - kShimmerStretch * share);
    if (r.gainsFor != std::array<double, 3>{r.size, share, decay}) { // worked out again once they move
        r.gainsFor = {r.size, share, decay};
        for (int i = 0; i < kReverbLines; ++i) {
            const double length = reverbLengths_[i] * r.size;
            r.gains[i] = std::pow(10.0, -3.0 * length / (decay * sampleRate_));
            r.highs[i] = std::pow(10.0, -3.0 * length / (decay * kReverbHighDecay * sampleRate_)) / r.gains[i];
        }
    }
    // What goes into a side comes out of each at kOutGain^2 of its energy and again each time round the lines,
    // falling 60 dB over the decay; both sides fed back, twice that comes round. The shifters keep the power of
    // what they read, so the gain they feed back at makes it the share.
    const double rounds = 1.0 - std::pow(10.0, -6.0 * reverbMeanLength_ * r.size / (decay * sampleRate_));
    const double shimmerTarget = std::sqrt(share * rounds / (2.0 * kOutGain * kOutGain));
    const double shimmerStep = rampStep(r.shimmer, shimmerTarget, n);
    const bool shifting = r.shimmer > 0.0 || shimmerTarget > 0.0;
    if (shifting && r.shimmer <= 0.0) { // starting again: nothing from the last time it ran goes back in
        for (ZeroBuffer &line : r.shifter)
            line.zero(r.ran);
        r.shimmerLow = {};
        r.shimmerBass = {};
    }
    // Each shifter's two heads sweep the window as fast as the interval asks, closing in for a shift up and falling
    // back for one down, each fading out (on a sine) as it jumps back across the window while the other, half a
    // window away, is heard in full; the right side's heads are a quarter of a cycle on, so the two sides' fades
    // don't dip together. What a tail holds is diffuse, so the two heads add up in power, which the fades keep.
    const double window = kShimmerWindow * sampleRate_;
    const double nearest = kShimmerNearest * sampleRate_;
    const double phaseStep = (1.0 - ratio) / window;
    // What the shifters read is band-passed on its way in, so what goes round doesn't build up rumble below or turn
    // harsh above: from 200 Hz (6 dB an octave) up to 8 kHz (two low-passes in a row, 24 dB an octave, so what
    // climbs past the top stops there), which the damping takes down to 2 kHz. For a shift up the top comes
    // down so it stays well under Nyquist shifted, and for a shift down the bottom goes up as far, so it stays above
    // 200 Hz.
    const Coefs low = biquad(BiquadType::LowPass,
                             std::min(8000.0 * std::pow(0.25, at(FxParam::ReverbDamping)),
                                      0.25 * sampleRate_ / std::max(ratio, 1.0)),
                             sampleRate_);
    const BiquadCoefs shimmerLow{low.b0, low.b1, low.b2, low.a1, low.a2};
    const double shimmerBass = onePole(200.0 / std::min(ratio, 1.0), sampleRate_);
    const int shiftSize = static_cast<int>(r.shifter[0].size());
    const std::array<double, kReverbLines> &gains = r.gains, &highs = r.highs;
    const double wander = kReverbWander * style.wander * sampleRate_;
    const int lineSize = static_cast<int>(r.lines[0].size());
    const int diffuserSize = static_cast<int>(r.diffusers[0][0].size());
    const int preSize = static_cast<int>(r.preDelay[0].size());

    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        r.dry += dryStep;
        r.wet += wetStep;
        r.width += widthStep;
        r.size += (sizeTarget - r.size) * glide;
        r.preLength += (preTarget - r.preLength) * glide;
        const double x[2] = {io[0][s], io[1][s]};
        double in[2], early[2] = {0.0, 0.0};
        for (int c = 0; c < 2; ++c) {
            r.preDelay[c][r.preWrite] = static_cast<float>(x[c]);
            in[c] = readBehind(r.preDelay[c], r.preWrite, r.preLength);
        }
        if (style.early > 0.0) {
            for (size_t e = 0; e < std::size(kRoomEchoSeconds); ++e) {
                const double behind = std::min(r.preLength + kRoomEchoSeconds[e] * echoScale, echoRoom);
                early[e % 2] += readBehind(r.preDelay[e % 2], r.preWrite, behind) * kRoomEchoLevels[e] * style.early;
            }
        }
        r.preWrite = r.preWrite + 1 == preSize ? 0 : r.preWrite + 1;
        // The shifted tail, going into the lines: each side into the half the other side's input doesn't, signed as
        // the left side of the output takes them.
        double fed[2] = {0.0, 0.0};
        if (shifting) {
            r.shimmer += shimmerStep;
            // The left side's heads read at the phase a and a + 1/2, faded by sin(pi a) and |cos(pi a)|, which the
            // phase going round 0..1 makes |sin| and |cos| of it unwrapped; the right side's, a quarter on.
            const double sinA = std::sin(std::numbers::pi * r.shimmerPhase);
            const double cosA = std::cos(std::numbers::pi * r.shimmerPhase);
            constexpr double kHalf = std::numbers::sqrt2 / 2.0;
            const double fades[2][2] = {{sinA, std::abs(cosA)},
                                        {std::abs(sinA + cosA) * kHalf, std::abs(cosA - sinA) * kHalf}};
            for (int c = 0; c < 2; ++c) {
                double a = r.shimmerPhase + 0.25 * c;
                a -= a >= 1.0 ? 1.0 : 0.0;
                const double b = a >= 0.5 ? a - 0.5 : a + 0.5;
                const double heads = readBehind(r.shifter[c], r.shiftWrite, nearest + a * window) * fades[c][0] +
                                     readBehind(r.shifter[c], r.shiftWrite, nearest + b * window) * fades[c][1];
                fed[c] = heads * r.shimmer;
            }
            r.shimmerPhase += phaseStep;
            r.shimmerPhase -= std::floor(r.shimmerPhase);
        }
        std::array<std::array<int, kDiffusers>, 2> diffuserTap;
        for (int c = 0; c < 2; ++c)
            for (int d = 0; d < style.diffusers; ++d)
                diffuserTap[c][d] = r.diffuserWrite >= diffuserLengths_[c][d]
                                        ? r.diffuserWrite - diffuserLengths_[c][d]
                                        : r.diffuserWrite - diffuserLengths_[c][d] + diffuserSize;
        for (int c = 0; c < 2; ++c) {
            for (int d = 0; d < style.diffusers; ++d) {
                const double g = kDiffuserGains[d];
                const double v = r.diffusers[c][d][diffuserTap[c][d]]; // a whole number of samples behind
                const double w = in[c] + g * v;
                r.diffusers[c][d][r.diffuserWrite] = static_cast<float>(w);
                in[c] = v - g * w;
            }
        }
        r.diffuserWrite = r.diffuserWrite + 1 == diffuserSize ? 0 : r.diffuserWrite + 1;

        std::array<double, kReverbLines> out; // each set below
        for (int i = 0; i < kReverbLines; ++i) {
            const double c = r.wanderCos[i], sn = r.wanderSin[i];
            r.wanderCos[i] = c * wanderTurn_[i][0] - sn * wanderTurn_[i][1];
            r.wanderSin[i] = sn * wanderTurn_[i][0] + c * wanderTurn_[i][1];
            out[i] = readBehind(r.lines[i], r.lineWrite, reverbLengths_[i] * r.size + wander * (1.0 + r.wanderSin[i]));
        }
        double wet[2] = {0.0, 0.0};
        std::array<double, kReverbLines> back; // each set below
        for (int i = 0; i < kReverbLines; ++i) {
            wet[0] += out[i] * kReverbOutSigns[0][i];
            wet[1] += out[i] * kReverbOutSigns[1][i];
            r.damp[i] += corner * (out[i] - r.damp[i]);
            back[i] = (r.damp[i] + highs[i] * (out[i] - r.damp[i])) * gains[i];
        }
        hadamard(back);
        for (int i = 0; i < kReverbLines; ++i)
            r.lines[i][r.lineWrite] = static_cast<float>(
                back[i] + (in[i % 2] * kReverbInSigns[i] + fed[(i + 1) % 2] * kReverbOutSigns[0][i]) * kInGain);
        r.lineWrite = r.lineWrite + 1 == lineSize ? 0 : r.lineWrite + 1;
        wet[0] *= kOutGain;
        wet[1] *= kOutGain;
        if (shifting) { // what the shifters will read, band-passed
            for (int c = 0; c < 2; ++c) {
                const double top = r.shimmerLow[c][1].process(r.shimmerLow[c][0].process(wet[c], shimmerLow), shimmerLow);
                r.shimmerBass[c] += shimmerBass * (top - r.shimmerBass[c]);
                r.shifter[c][r.shiftWrite] = static_cast<float>(top - r.shimmerBass[c]);
            }
            r.shiftWrite = r.shiftWrite + 1 == shiftSize ? 0 : r.shiftWrite + 1;
        }
        wet[0] += early[0];
        wet[1] += early[1];
        const double middle = 0.5 * (wet[0] + wet[1]);
        const double side = 0.5 * (wet[0] - wet[1]) * r.width;
        io[0][s] = static_cast<float>(x[0] * r.dry + (middle + side) * r.wet);
        io[1][s] = static_cast<float>(x[1] * r.dry + (middle - side) * r.wet);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(wet[0]), std::abs(wet[1])});
    }
    // What goes in now comes out after the pre-delay, and in a room its last early reflection later still: until
    // then, the reverb may be silent with more to come.
    r.reach = style.early > 0.0
                  ? std::min(r.preLength + kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1] * echoScale, echoRoom)
                  : r.preLength;
    // Faded out (over more than one chunk, if events cut them short), turned down or switched to another algorithm:
    // the ramp leaves a hair either side of 0, which would keep it shifting or skip clearing its lines next time.
    if (shimmerTarget == 0.0 && std::abs(r.shimmer) < 1e-9)
        r.shimmer = 0.0;
    // The wavering phasors keep their length.
    for (int i = 0; i < kReverbLines; ++i) {
        const double length = std::hypot(r.wanderCos[i], r.wanderSin[i]);
        r.wanderCos[i] /= length;
        r.wanderSin[i] /= length;
    }
    return peak;
}

double Effects::runPhaser(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo) {
    Phaser &ph = slots_[slot].phaser;
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const double step = sweepHz([&p](int i) { return p[i]; }, slot, true, tempo) / sampleRate_;
    const double centerHz = cutoffFromNorm(at(FxParam::PhaserCenter)); // phaserCorner()'s, the same all chunk
    const double depth = at(FxParam::PhaserDepth);
    const double feedback = sweepFeedback(at(FxParam::PhaserFeedback));
    const double gain = sweepGain(at(FxParam::PhaserFeedback));
    const int stages = std::clamp(static_cast<int>(std::lround(at(FxParam::PhaserStages))), 1, kMaxPhaserStages);
    for (int ch = 0; ch < 2; ++ch)
        for (int k = stages; k < kMaxPhaserStages; ++k)
            ph.stages[ch][k] = 0.0;
    const double mixStep = rampStep(ph.mix, sweepWet(at(FxParam::PhaserMix)), n);
    const double polarity = sweepPolarity(at(FxParam::PhaserNegative));
    const double polarityStep = 2.0 / (kPolaritySeconds * sampleRate_); // the whole way, -1 to 1
    const double spread = at(FxParam::PhaserStereo) >= 0.5 ? 0.25 : 0.0; // the right side's head start
    // The all-passes' coefficient where the sweep is at `phase`, on each side.
    auto coefAt = [&](double phase, int ch) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (phase + spread * ch));
        const double corner = centerHz * std::exp2(2.5 * depth * lfo); // phaserCorner()
        const double t = std::tan(std::numbers::pi * std::clamp(corner, 20.0, 0.45 * sampleRate_) / sampleRate_);
        return (t - 1.0) / (t + 1.0);
    };
    if (!ph.coefKnown) {
        ph.coef = {coefAt(ph.phase, 0), coefAt(ph.phase, 1)};
        ph.coefKnown = true;
    }
    double peak = 0.0;
    // Every kSweepStep samples the coefficient is worked out where the sweep will be at the step's last sample, and
    // moves there in a straight line from where it was.
    std::array<double, 2> coefTo{}, coefStep{};
    uint32_t stepLeft = 0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            double ahead = ph.phase;
            for (uint32_t k = 0; k < stepLeft; ++k) { // as the samples move it, below
                ahead += step;
                ahead -= std::floor(ahead);
            }
            for (int ch = 0; ch < 2; ++ch) {
                coefTo[ch] = coefAt(ahead, ch);
                coefStep[ch] = (coefTo[ch] - ph.coef[ch]) / stepLeft;
            }
        }
        --stepLeft;
        ph.mix += mixStep;
        ph.polarity = moveToward(ph.polarity, polarity, polarityStep);
        ph.phase += step;
        ph.phase -= std::floor(ph.phase);
        for (int ch = 0; ch < 2; ++ch) {
            ph.coef[ch] = stepLeft == 0 ? coefTo[ch] : ph.coef[ch] + coefStep[ch]; // exactly there at the step's end
            const double a = ph.coef[ch];
            const double x = io[ch][s];
            double y = x + ph.polarity * feedback * ph.last[ch];
            for (int k = 0; k < stages; ++k) { // y = a x + z; z = x - a y
                double &z = ph.stages[ch][k];
                const double out = a * y + z;
                z = y - a * out;
                y = out;
            }
            ph.last[ch] = y;
            io[ch][s] = static_cast<float>(x + (ph.polarity * y * gain - x) * ph.mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

std::array<double, 5> disperserCoefs(double freqNorm, double pinch, double sampleRate) {
    const double hz = std::min(cutoffFromNorm(freqNorm), 0.45 * sampleRate);
    const double w = 2.0 * std::numbers::pi * hz / sampleRate;
    const double q = 0.3 * std::pow(20.0, std::clamp(pinch, 0.0, 1.0));
    const double alpha = std::sin(w) / (2.0 * q), a0 = 1.0 + alpha;
    return {(1.0 - alpha) / a0, -2.0 * std::cos(w) / a0, 1.0, -2.0 * std::cos(w) / a0, (1.0 - alpha) / a0};
}

double disperserDelay(double hz, double freqNorm, double pinch, int stages, double sampleRate) {
    // An all-pass's delay is how fast its phase turns with frequency.
    const std::array<double, 5> c = disperserCoefs(freqNorm, pinch, sampleRate);
    auto phase = [&](double w) {
        const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
        return std::arg((c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2));
    };
    const double w = 2.0 * std::numbers::pi * std::min(hz, 0.45 * sampleRate) / sampleRate;
    const double dw = 1e-4 * w + 1e-7;
    double turn = phase(w + dw) - phase(w - dw);
    turn -= 2.0 * std::numbers::pi * std::round(turn / (2.0 * std::numbers::pi));
    return -turn / (2.0 * dw) * stages / sampleRate;
}

float Effects::sweepPhase(int slot) const {
    const Slot &s = slots_[slot];
    return static_cast<float>(s.running == FxType::Phaser     ? s.phaser.phase
                              : s.running == FxType::Flanger  ? s.flanger.phase
                              : s.running == FxType::Widener ? s.widener.phases[0]
                                                              : 0.0);
}

double widenerTap(int tap, double size, double lfo) {
    return kWidenerDelays[tap] * (0.2 + 0.8 * std::clamp(size, 0.0, 1.0)) * (1.0 + kWidenerSweep * lfo);
}

double widenerSpread(double hz, double highPassNorm) {
    const std::complex<double> s(0.0, hz / cutoffFromNorm(highPassNorm));
    return std::abs(s * s / (s * s + std::numbers::sqrt2 * s + 1.0));
}

double Effects::runWidener(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Widener &e = slots_[slot].widener;
    e.ran += n; // counted ahead of what this chunk writes (see start())
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const Coefs hp = biquad(BiquadType::HighPass,
                            std::min(cutoffFromNorm(at(FxParam::WidenerHighPass)), 0.45 * sampleRate_), sampleRate_);
    const BiquadCoefs highPass{hp.b0, hp.b1, hp.b2, hp.a1, hp.a2};
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate_)); // size changes glide, like tape
    const double sizeTarget = at(FxParam::WidenerSize);
    const double widthStep = rampStep(e.width, at(FxParam::WidenerWidth), n);
    const double mixStep = rampStep(e.mix, at(FxParam::WidenerMix), n);
    const int size = static_cast<int>(e.line.size());
    // The taps' sweeps as phasors turned a step each sample, rather than a sine each (see runShifter()).
    std::array<double, kWidenerTaps> sinNow{}, cosNow{}, sinStep{}, cosStep{};
    for (int k = 0; k < kWidenerTaps; ++k) {
        const double step = kWidenerRates[k] / sampleRate_;
        const double first = e.phases[k] + step;
        const double angle = 2.0 * std::numbers::pi * (first - std::floor(first));
        sinNow[k] = std::sin(angle);
        cosNow[k] = std::cos(angle);
        sinStep[k] = std::sin(2.0 * std::numbers::pi * step);
        cosStep[k] = std::cos(2.0 * std::numbers::pi * step);
    }
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        e.size += (sizeTarget - e.size) * glide;
        e.width += widthStep;
        e.mix += mixStep;
        double taps[kWidenerTaps];
        for (int k = 0; k < kWidenerTaps; ++k) {
            e.phases[k] += kWidenerRates[k] / sampleRate_;
            e.phases[k] -= std::floor(e.phases[k]);
            const double lfo = sinNow[k];
            const double sn = sinNow[k];
            sinNow[k] = sn * cosStep[k] + cosNow[k] * sinStep[k];
            cosNow[k] = cosNow[k] * cosStep[k] - sn * sinStep[k];
            taps[k] = readBehind(e.line, e.write, std::clamp(widenerTap(k, e.size, lfo) * sampleRate_, 1.0, size - 2.0));
        }
        const double x[2] = {io[0][s], io[1][s]};
        e.line[e.write] = static_cast<float>(e.highPass.process(0.5 * (x[0] + x[1]), highPass));
        e.write = e.write + 1 == size ? 0 : e.write + 1;
        // The sides: what the taps on the left have and those on the right lack, which cancels in a mono sum.
        // The middle: all four together, a chorus.
        const double side = 0.5 * (taps[0] - taps[1] + taps[2] - taps[3]) * e.width;
        const double middle = 0.35 * (taps[0] + taps[1] + taps[2] + taps[3]) * (1.0 - e.width);
        io[0][s] = static_cast<float>(x[0] + (middle + side) * e.mix);
        io[1][s] = static_cast<float>(x[1] + (middle - side) * e.mix);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(taps[0]), std::abs(taps[3])});
    }
    return peak;
}

double Effects::runDisperser(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Disperser &d = slots_[slot].disperser;
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const std::array<double, 5> b = disperserCoefs(at(FxParam::DisperserFreq), at(FxParam::DisperserPinch), sampleRate_);
    const BiquadCoefs c{b[0], b[1], b[2], b[3], b[4]};
    const int stages =
        std::clamp(static_cast<int>(std::lround(at(FxParam::DisperserStages))), 1, kMaxDisperserStages);
    for (int ch = 0; ch < 2; ++ch)
        for (int k = stages; k < kMaxDisperserStages; ++k)
            d.stages[ch][k] = Biquad{};
    const double mixStep = rampStep(d.mix, at(FxParam::DisperserMix), n);
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        d.mix += mixStep;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = io[ch][s];
            double y = x;
            for (int k = 0; k < stages; ++k)
                y = d.stages[ch][k].process(y, c);
            io[ch][s] = static_cast<float>(x + (y - x) * d.mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

double Effects::runFlanger(int slot, float *const *io, uint32_t n, const ParamValues &p, double tempo) {
    Flanger &f = slots_[slot].flanger;
    f.ran += n; // counted ahead of what this chunk writes (see start())
    auto at = [&](FxParam field) { return p[paramIndex(slot, field)]; };
    const double step = sweepHz([&p](int i) { return p[i]; }, slot, false, tempo) / sampleRate_;
    const double delaySeconds = timeFromNorm(at(FxParam::FlangerDelay)); // flangerDelay()'s, the same all chunk
    const double depth = at(FxParam::FlangerDepth);
    const double feedback = sweepFeedback(at(FxParam::FlangerFeedback));
    const double gain = sweepGain(at(FxParam::FlangerFeedback));
    const double mixStep = rampStep(f.mix, sweepWet(at(FxParam::FlangerMix)), n);
    const double polarity = sweepPolarity(at(FxParam::FlangerNegative));
    const double polarityStep = 2.0 / (kPolaritySeconds * sampleRate_);
    const double spread = at(FxParam::FlangerStereo) >= 0.5 ? 0.25 : 0.0; // the right side's head start
    const int size = static_cast<int>(f.lines[0].size());
    // The delay in samples where the sweep is at `phase`, on each side: worked out every kSweepStep samples, as the
    // phaser's coefficient is (see runPhaser()).
    auto delayAt = [&](double phase, int ch) {
        const double lfo = std::sin(2.0 * std::numbers::pi * (phase + spread * ch));
        return std::clamp(delaySeconds * (1.0 + 0.95 * depth * lfo) * sampleRate_, 1.0, size - 2.0); // flangerDelay()
    };
    if (!f.delayKnown) {
        f.delay = {delayAt(f.phase, 0), delayAt(f.phase, 1)};
        f.delayKnown = true;
    }
    std::array<double, 2> delayTo{}, delayStep{};
    uint32_t stepLeft = 0;
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        if (stepLeft == 0) {
            stepLeft = std::min(kSweepStep, n - s);
            double ahead = f.phase;
            for (uint32_t k = 0; k < stepLeft; ++k) {
                ahead += step;
                ahead -= std::floor(ahead);
            }
            for (int ch = 0; ch < 2; ++ch) {
                delayTo[ch] = delayAt(ahead, ch);
                delayStep[ch] = (delayTo[ch] - f.delay[ch]) / stepLeft;
            }
        }
        --stepLeft;
        f.mix += mixStep;
        f.polarity = moveToward(f.polarity, polarity, polarityStep);
        f.phase += step;
        f.phase -= std::floor(f.phase);
        for (int ch = 0; ch < 2; ++ch) {
            f.delay[ch] = stepLeft == 0 ? delayTo[ch] : f.delay[ch] + delayStep[ch];
            const double delayed = readBehind(f.lines[ch], f.write, f.delay[ch]);
            const double x = io[ch][s];
            f.lines[ch][f.write] = static_cast<float>(x + f.polarity * feedback * delayed);
            f.last[ch] = delayed;
            io[ch][s] = static_cast<float>(x + (f.polarity * delayed * gain - x) * f.mix);
            peak = std::max({peak, std::abs(x), std::abs(delayed)});
        }
        f.write = f.write + 1 == size ? 0 : f.write + 1;
    }
    return peak;
}

double Effects::runShifter(int slot, float *const *io, uint32_t n, const ParamValues &p) {
    Shifter &sh = slots_[slot].shifter;
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    const double hz = shiftHz(at(FxParam::ShiftAmount), at(FxParam::ShiftRange));
    const double steps[2] = {hz / sampleRate_, hz * (1.0 - 2.0 * at(FxParam::ShiftSpread)) / sampleRate_};
    const double feedback = at(FxParam::ShiftFeedback);
    const double mixStep = rampStep(sh.mix, at(FxParam::ShiftMix), n);
    // The oscillators as phasors turned a step each sample, rather than a cosine and a sine each: from where each
    // phase is after its first step this chunk, which keeps the turning from drifting past a chunk.
    std::array<double, 2> cosNow{}, sinNow{}, cosStep{}, sinStep{};
    for (int ch = 0; ch < 2; ++ch) {
        const double first = sh.phase[ch] + steps[ch];
        const double angle = 2.0 * std::numbers::pi * (first - std::floor(first));
        cosNow[ch] = std::cos(angle);
        sinNow[ch] = std::sin(angle);
        cosStep[ch] = std::cos(2.0 * std::numbers::pi * steps[ch]);
        sinStep[ch] = std::sin(2.0 * std::numbers::pi * steps[ch]);
    }
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        sh.mix += mixStep;
        for (int ch = 0; ch < 2; ++ch) {
            sh.phase[ch] += steps[ch];
            sh.phase[ch] -= std::floor(sh.phase[ch]);
            const double x = io[ch][s];
            const std::array<double, 2> parts = sh.hilbert[ch].process(x + feedback * sh.last[ch]);
            // The analytic signal turned by the oscillator: every partial moves up by `hz` (down if negative).
            const double y = parts[0] * cosNow[ch] - parts[1] * sinNow[ch];
            const double c = cosNow[ch];
            cosNow[ch] = c * cosStep[ch] - sinNow[ch] * sinStep[ch];
            sinNow[ch] = sinNow[ch] * cosStep[ch] + c * sinStep[ch];
            sh.last[ch] = y;
            io[ch][s] = static_cast<float>(x + (y - x) * sh.mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

} // namespace fmvibe
