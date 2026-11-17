#include "reverb.h"

#include "delay.h"

namespace fmvibe {

namespace {

// The reverb's size knob: from a quarter of the lines' full length to all of it.
double reverbScale(double size) {
    return 0.25 + 0.75 * std::clamp(size, 0.0, 1.0);
}

} // namespace

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

namespace fx {

namespace {

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

void ReverbTables::prepare(double sampleRate) {
    static_assert(std::size(kReverbSeconds) == kReverbLines && std::size(kWanderRates) == kReverbLines &&
                  std::size(kReverbInSigns) == kReverbLines && std::size(kDiffuserSeconds[0]) == kDiffusers &&
                  std::size(kDiffuserGains) == kDiffusers);
    double inverses = 0.0;
    for (int i = 0; i < kReverbLines; ++i) {
        lengths[i] = kReverbSeconds[i] * sampleRate;
        inverses += 1.0 / lengths[i];
        const double step = 2.0 * std::numbers::pi * kWanderRates[i] / sampleRate;
        wanderTurn[i] = {std::cos(step), std::sin(step)};
    }
    meanLength = kReverbLines / inverses;
    for (int side = 0; side < 2; ++side)
        for (int i = 0; i < kDiffusers; ++i)
            diffuserLengths[side][i] = static_cast<int>(std::lround(kDiffuserSeconds[side][i] * sampleRate));
}

void Reverb::prepare(double sampleRate) {
    // Room behind the longest line for its wavering, the most of any algorithm.
    const double longest = kReverbLongest + 2.0 * kReverbWander * reverbStyle(ReverbType::Shimmer).wander;
    for (ZeroBuffer &line : lines)
        line.assign(static_cast<size_t>(std::ceil(longest * sampleRate)) + 4, 0.0f);
    for (ZeroBuffer &line : shifter)
        line.assign(static_cast<size_t>(std::ceil((kShimmerNearest + kShimmerWindow) * sampleRate)) + 4, 0.0f);
    const double longestDiffuser = std::max(*std::max_element(std::begin(kDiffuserSeconds[0]), std::end(kDiffuserSeconds[0])),
                                            *std::max_element(std::begin(kDiffuserSeconds[1]), std::end(kDiffuserSeconds[1])));
    const size_t widest = static_cast<size_t>(std::ceil(longestDiffuser * sampleRate)) + 4;
    for (auto &side : diffusers)
        for (ZeroBuffer &diffuser : side)
            diffuser.assign(widest, 0.0f);
    // Room for a room's latest early reflection behind the longest pre-delay.
    const double behind = kMaxPreDelay + kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1];
    for (ZeroBuffer &line : preDelay)
        line.assign(static_cast<size_t>(std::ceil(behind * sampleRate)) + 4, 0.0f);
}

void Reverb::start(const Context &ctx) {
    for (ZeroBuffer &line : lines) // these two cleared as they are read, the longest
        line.restart(ran);
    for (ZeroBuffer &line : preDelay)
        line.restart(ran);
    for (auto &side : diffusers)
        for (ZeroBuffer &diffuser : side)
            diffuser.zero(ran);
    for (ZeroBuffer &line : shifter)
        line.zero(ran);
    ran = 0;
    lineWrite = diffuserWrite = preWrite = shiftWrite = 0;
    shimmerPhase = 0.0;
    shimmerLow = {};
    shimmerBass = {};
    damp = {};
    lineRead = {};
    gainsFor = {std::numeric_limits<double>::quiet_NaN()}; // the sample rate may have changed
    for (int i = 0; i < kReverbLines; ++i) { // each line's wavering starts somewhere else
        const double angle = 2.0 * std::numbers::pi * i * 0.618034;
        wanderCos[i] = std::cos(angle);
        wanderSin[i] = std::sin(angle);
    }
    shimmer = 0.0; // fades in with the rest
    size = reverbSpace(ctx.at(FxParam::ReverbSize), reverbType(ctx.at(FxParam::ReverbType)));
    preLength = std::round(reverbPreDelay(ctx.at(FxParam::ReverbPreDelay)) * ctx.sampleRate);
    dry = delayDryLevel(ctx.at(FxParam::ReverbMix));
    wet = delayWetLevel(ctx.at(FxParam::ReverbMix));
    width = ctx.at(FxParam::ReverbWidth);
}

double Reverb::run(const Context &ctx, const ReverbTables &tables, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const double sampleRate = ctx.sampleRate;
    const std::array<double, kReverbLines> &lengths = tables.lengths;
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate)); // size and pre-delay changes glide
    const ReverbType type = reverbType(ctx.at(FxParam::ReverbType));
    const ReverbStyle style = reverbStyle(type);
    for (int c = 0; c < 2; ++c)
        for (int d = diffusersUsed; d < style.diffusers; ++d)
            diffusers[c][d].zero(ran);
    diffusersUsed = style.diffusers;
    const double sizeTarget = reverbSpace(ctx.at(FxParam::ReverbSize), type); // a new algorithm's length glides too
    // In whole samples, so once it has glided there it reads no samples between (which would dull the top a little).
    const double preTarget = std::round(reverbPreDelay(ctx.at(FxParam::ReverbPreDelay)) * sampleRate);
    // A room's early reflections spread with its size, after the pre-delay; they stay inside the buffer.
    const double echoScale = reverbScale(ctx.at(FxParam::ReverbSize)) * sampleRate;
    const double echoRoom = static_cast<double>(preDelay[0].size()) - 2.0;
    const double mix = ctx.at(FxParam::ReverbMix);
    const double dryStep = rampStep(dry, delayDryLevel(mix), n, ctx.rampShortest);
    const double wetStep = rampStep(wet, delayWetLevel(mix), n, ctx.rampShortest);
    const double widthStep = rampStep(width, ctx.at(FxParam::ReverbWidth), n, ctx.rampShortest);
    constexpr double kInGain = 0.35355339059327373; // 1 / sqrt(8): each side goes into half the lines
    constexpr double kOutGain = 0.22; // about 1 / sqrt(16), a little under, for the level a mono sound had before
    // A shimmer feeds its tail back into its lines, band-passed and pitch-shifted, so each time round part of it
    // climbs (or falls) by the interval. It goes in past the all-passes: run through them again each time round,
    // the shifted tail would ring metallic. The share of the tail's energy that comes back, the Shimmer knob's times
    // kShimmerLoop, sets how much. Within a semitone the shifted tail would much be the tail itself, ringing on
    // rather than shimmering, so the loop fades out there.
    const double pitch = std::clamp(ctx.at(FxParam::ReverbShimmerPitch), -24.0, 24.0);
    const double ratio = std::exp2(pitch / 12.0);
    const double share = style.shimmer ? kShimmerLoop * std::clamp(ctx.at(FxParam::ReverbShimmer), 0.0, 1.0) *
                                             std::min(std::abs(pitch), 1.0)
                                       : 0.0;
    // Each line's gain takes it down 60 dB in the decay time, for the length it has at this chunk's size, and its
    // shelf as much again kReverbHighDecay as fast above the damping's corner: 20 kHz down to 1 kHz. What a shimmer
    // feeds back would stretch its tail by 1 / (1 - share) were nothing lost on the way, so its lines fall faster,
    // by kShimmerStretch of that, and the tail as a whole still takes about the decay time.
    const double corner =
        onePole(20000.0 * std::pow(0.05, ctx.at(FxParam::ReverbDamping)) * style.brightness, sampleRate);
    const double decay = reverbDecay(ctx.at(FxParam::ReverbDecay)) * (1.0 - kShimmerStretch * share);
    if (gainsFor != std::array<double, 3>{size, share, decay}) { // worked out again once they move
        gainsFor = {size, share, decay};
        for (int i = 0; i < kReverbLines; ++i) {
            const double length = lengths[i] * size;
            gains[i] = std::pow(10.0, -3.0 * length / (decay * sampleRate));
            highs[i] = std::pow(10.0, -3.0 * length / (decay * kReverbHighDecay * sampleRate)) / gains[i];
        }
    }
    // What goes into a side comes out of each at kOutGain^2 of its energy and again each time round the lines,
    // falling 60 dB over the decay; both sides fed back, twice that comes round. The shifters keep the power of
    // what they read, so the gain they feed back at makes it the share.
    const double rounds = 1.0 - std::pow(10.0, -6.0 * tables.meanLength * size / (decay * sampleRate));
    const double shimmerTarget = std::sqrt(share * rounds / (2.0 * kOutGain * kOutGain));
    const double shimmerStep = rampStep(shimmer, shimmerTarget, n, ctx.rampShortest);
    const bool shifting = shimmer > 0.0 || shimmerTarget > 0.0;
    if (shifting && shimmer <= 0.0) { // starting again: nothing from the last time it ran goes back in
        for (ZeroBuffer &line : shifter)
            line.zero(ran);
        shimmerLow = {};
        shimmerBass = {};
    }
    // Each shifter's two heads sweep the window as fast as the interval asks, closing in for a shift up and falling
    // back for one down, each fading out (on a sine) as it jumps back across the window while the other, half a
    // window away, is heard in full; the right side's heads are a quarter of a cycle on, so the two sides' fades
    // don't dip together. What a tail holds is diffuse, so the two heads add up in power, which the fades keep.
    const double window = kShimmerWindow * sampleRate;
    const double nearest = kShimmerNearest * sampleRate;
    const double phaseStep = (1.0 - ratio) / window;
    // What the shifters read is band-passed on its way in, so what goes round doesn't build up rumble below or turn
    // harsh above: from 200 Hz (6 dB an octave) up to 8 kHz (two low-passes in a row, 24 dB an octave, so what
    // climbs past the top stops there), which the damping takes down to 2 kHz. For a shift up the top comes
    // down so it stays well under Nyquist shifted, and for a shift down the bottom goes up as far, so it stays above
    // 200 Hz.
    const Coefs low = biquad(BiquadType::LowPass,
                             std::min(8000.0 * std::pow(0.25, ctx.at(FxParam::ReverbDamping)),
                                      0.25 * sampleRate / std::max(ratio, 1.0)),
                             sampleRate);
    const BiquadCoefs lowCoefs{low.b0, low.b1, low.b2, low.a1, low.a2};
    const double bassCoef = onePole(200.0 / std::min(ratio, 1.0), sampleRate);
    const int shiftSize = static_cast<int>(shifter[0].size());
    const double wander = kReverbWander * style.wander * sampleRate;
    const int lineSize = static_cast<int>(lines[0].size());
    const int diffuserSize = static_cast<int>(diffusers[0][0].size());
    const int preSize = static_cast<int>(preDelay[0].size());
    // What the lines and the pre-delay held before the reverb started is cleared only where this chunk reads them,
    // behind its start (see ZeroBuffer::restart()): each line at the sizes it glides between, wavering, and the
    // pre-delay at the lengths it glides between, out to a room's latest reflection.
    const uint64_t written = ran - n;
    const double sizes[2] = {std::min(size, sizeTarget), std::max(size, sizeTarget)};
    for (int i = 0; i < kReverbLines; ++i) {
        const auto [from, to] = readsBehind(lineSize, lineWrite, n, lengths[i] * sizes[0],
                                            lengths[i] * sizes[1] + 2.0 * wander);
        lines[i].clearStale(written, n + 1, from, to);
    }
    const double preShortest = std::min(preLength, preTarget), preLongest = std::max(preLength, preTarget);
    const auto [preFrom, preTo] = readsBehind(
        preSize, preWrite, n, preShortest,
        style.early > 0.0 ? std::min(preLongest + kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1] * echoScale, echoRoom)
                          : preLongest);
    for (ZeroBuffer &line : preDelay)
        line.clearStale(written, n + 2, preFrom, preTo);

    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        dry += dryStep;
        wet += wetStep;
        width += widthStep;
        size += (sizeTarget - size) * glide;
        preLength += (preTarget - preLength) * glide;
        const double x[2] = {io[0][s], io[1][s]};
        double in[2], early[2] = {0.0, 0.0};
        for (int c = 0; c < 2; ++c) {
            preDelay[c][preWrite] = static_cast<float>(x[c]);
            in[c] = readBehind(preDelay[c], preWrite, preLength);
        }
        if (style.early > 0.0) {
            for (size_t e = 0; e < std::size(kRoomEchoSeconds); ++e) {
                const double behind = std::min(preLength + kRoomEchoSeconds[e] * echoScale, echoRoom);
                early[e % 2] += readBehind(preDelay[e % 2], preWrite, behind) * kRoomEchoLevels[e] * style.early;
            }
        }
        preWrite = preWrite + 1 == preSize ? 0 : preWrite + 1;
        // The shifted tail, going into the lines: each side into the half the other side's input doesn't, signed as
        // the left side of the output takes them.
        double fed[2] = {0.0, 0.0};
        if (shifting) {
            shimmer += shimmerStep;
            // The left side's heads read at the phase a and a + 1/2, faded by sin(pi a) and |cos(pi a)|, which the
            // phase going round 0..1 makes |sin| and |cos| of it unwrapped; the right side's, a quarter on.
            const double sinA = std::sin(std::numbers::pi * shimmerPhase);
            const double cosA = std::cos(std::numbers::pi * shimmerPhase);
            constexpr double kHalf = std::numbers::sqrt2 / 2.0;
            const double fades[2][2] = {{sinA, std::abs(cosA)},
                                        {std::abs(sinA + cosA) * kHalf, std::abs(cosA - sinA) * kHalf}};
            for (int c = 0; c < 2; ++c) {
                double a = shimmerPhase + 0.25 * c;
                a -= a >= 1.0 ? 1.0 : 0.0;
                const double b = a >= 0.5 ? a - 0.5 : a + 0.5;
                const double heads = readCubic(shifter[c], shiftWrite, nearest + a * window) * fades[c][0] +
                                     readCubic(shifter[c], shiftWrite, nearest + b * window) * fades[c][1];
                fed[c] = heads * shimmer;
            }
            shimmerPhase += phaseStep;
            shimmerPhase -= std::floor(shimmerPhase);
        }
        std::array<std::array<int, kDiffusers>, 2> diffuserTap;
        for (int c = 0; c < 2; ++c)
            for (int d = 0; d < style.diffusers; ++d)
                diffuserTap[c][d] = diffuserWrite >= tables.diffuserLengths[c][d]
                                        ? diffuserWrite - tables.diffuserLengths[c][d]
                                        : diffuserWrite - tables.diffuserLengths[c][d] + diffuserSize;
        for (int c = 0; c < 2; ++c) {
            for (int d = 0; d < style.diffusers; ++d) {
                const double g = kDiffuserGains[d];
                const double v = diffusers[c][d][diffuserTap[c][d]]; // a whole number of samples behind
                const double w = in[c] + g * v;
                diffusers[c][d][diffuserWrite] = static_cast<float>(w);
                in[c] = v - g * w;
            }
        }
        diffuserWrite = diffuserWrite + 1 == diffuserSize ? 0 : diffuserWrite + 1;

        std::array<double, kReverbLines> out; // each set below
        for (int i = 0; i < kReverbLines; ++i) {
            const double c = wanderCos[i], sn = wanderSin[i];
            wanderCos[i] = c * tables.wanderTurn[i][0] - sn * tables.wanderTurn[i][1];
            wanderSin[i] = sn * tables.wanderTurn[i][0] + c * tables.wanderTurn[i][1];
            // Between samples through a first-order all-pass, which waits `fraction` more at low frequencies and
            // passes every frequency at its level: read through a straight line, the top would lose a little each
            // time round (2 dB at 10 kHz at 48 kHz half way between), a tail's highs falling much faster than its
            // damping says. The fraction stays between 0.5 and 1.5, where the all-pass's pole is far from the circle,
            // so its step as the whole samples move on dies away at once; the lines' slow wavering hides it.
            const double behind = lengths[i] * size + wander * (1.0 + wanderSin[i]);
            const int whole = static_cast<int>(behind - 0.5);
            const double fraction = behind - whole;
            const double coef = (1.0 - fraction) / (1.0 + fraction);
            int newer = lineWrite - whole;
            newer += newer < 0 ? lineSize : 0;
            const int before = newer == 0 ? lineSize - 1 : newer - 1;
            out[i] = coef * (lines[i][newer] - lineRead[i]) + lines[i][before];
            lineRead[i] = out[i];
        }
        double tail[2] = {0.0, 0.0};
        std::array<double, kReverbLines> back; // each set below
        for (int i = 0; i < kReverbLines; ++i) {
            tail[0] += out[i] * kReverbOutSigns[0][i];
            tail[1] += out[i] * kReverbOutSigns[1][i];
            damp[i] += corner * (out[i] - damp[i]);
            back[i] = (damp[i] + highs[i] * (out[i] - damp[i])) * gains[i];
        }
        hadamard(back);
        for (int i = 0; i < kReverbLines; ++i)
            lines[i][lineWrite] = static_cast<float>(
                back[i] + (in[i % 2] * kReverbInSigns[i] + fed[(i + 1) % 2] * kReverbOutSigns[0][i]) * kInGain);
        lineWrite = lineWrite + 1 == lineSize ? 0 : lineWrite + 1;
        tail[0] *= kOutGain;
        tail[1] *= kOutGain;
        if (shifting) { // what the shifters will read, band-passed
            for (int c = 0; c < 2; ++c) {
                const double top = shimmerLow[c][1].process(shimmerLow[c][0].process(tail[c], lowCoefs), lowCoefs);
                shimmerBass[c] += bassCoef * (top - shimmerBass[c]);
                shifter[c][shiftWrite] = static_cast<float>(top - shimmerBass[c]);
            }
            shiftWrite = shiftWrite + 1 == shiftSize ? 0 : shiftWrite + 1;
        }
        tail[0] += early[0];
        tail[1] += early[1];
        const double middle = 0.5 * (tail[0] + tail[1]);
        const double side = 0.5 * (tail[0] - tail[1]) * width;
        io[0][s] = static_cast<float>(x[0] * dry + (middle + side) * wet);
        io[1][s] = static_cast<float>(x[1] * dry + (middle - side) * wet);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(tail[0]), std::abs(tail[1])});
    }
    // What goes in now comes out after the pre-delay, and in a room its last early reflection later still: until
    // then, the reverb may be silent with more to come.
    reach = style.early > 0.0
                ? std::min(preLength + kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1] * echoScale, echoRoom)
                : preLength;
    // Faded out (over more than one chunk, if events cut them short), turned down or switched to another algorithm:
    // the ramp leaves a hair either side of 0, which would keep it shifting or skip clearing its lines next time.
    if (shimmerTarget == 0.0 && std::abs(shimmer) < 1e-9)
        shimmer = 0.0;
    // The wavering phasors keep their length.
    for (int i = 0; i < kReverbLines; ++i) {
        const double length = std::hypot(wanderCos[i], wanderSin[i]);
        wanderCos[i] /= length;
        wanderSin[i] /= length;
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
