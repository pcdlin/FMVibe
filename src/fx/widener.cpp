#include "widener.h"

#include <complex>

namespace fmvibe {

namespace {

constexpr double kWidenerSweep = 0.12; // how far the widener's taps are swept either way, of their delay

} // namespace

double widenerTap(int tap, double size, double lfo) {
    return kWidenerDelays[tap] * (0.2 + 0.8 * std::clamp(size, 0.0, 1.0)) * (1.0 + kWidenerSweep * lfo);
}

double widenerSpread(double hz, double highPassNorm) {
    const std::complex<double> s(0.0, hz / cutoffFromNorm(highPassNorm));
    return std::abs(s * s / (s * s + std::numbers::sqrt2 * s + 1.0));
}

namespace fx {

void Widener::prepare(double sampleRate) {
    const double longestTap = kWidenerDelays[kWidenerTaps - 1] * (1.0 + kWidenerSweep);
    line.assign(static_cast<size_t>(std::ceil(longestTap * sampleRate)) + 4, 0.0f);
}

void Widener::start(const Context &ctx) {
    line.zero(ran);
    ran = 0;
    write = 0;
    highPass = Biquad{};
    phases = {0.0, 0.25, 0.5, 0.75}; // apart from the start, so the sides never sweep together
    size = ctx.at(FxParam::WidenerSize);
    width = ctx.at(FxParam::WidenerWidth);
    mix = ctx.at(FxParam::WidenerMix);
}

double Widener::run(const Context &ctx, float *const *io, uint32_t n) {
    ran += n; // counted ahead of what this chunk writes (see start())
    const double sampleRate = ctx.sampleRate;
    const Coefs hp = biquad(BiquadType::HighPass,
                            std::min(cutoffFromNorm(ctx.at(FxParam::WidenerHighPass)), 0.45 * sampleRate), sampleRate);
    const BiquadCoefs highPassCoefs{hp.b0, hp.b1, hp.b2, hp.a1, hp.a2};
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate)); // size changes glide, like tape
    const double sizeTarget = ctx.at(FxParam::WidenerSize);
    const double widthStep = rampStep(width, ctx.at(FxParam::WidenerWidth), n, ctx.rampShortest);
    const double mixStep = rampStep(mix, ctx.at(FxParam::WidenerMix), n, ctx.rampShortest);
    const int length = static_cast<int>(line.size());
    // The taps' sweeps as phasors turned a step each sample, rather than a sine each (see Shifter::run()).
    std::array<double, kWidenerTaps> sinNow{}, cosNow{}, sinStep{}, cosStep{};
    for (int k = 0; k < kWidenerTaps; ++k) {
        const double step = kWidenerRates[k] / sampleRate;
        const double first = phases[k] + step;
        const double angle = 2.0 * std::numbers::pi * (first - std::floor(first));
        sinNow[k] = std::sin(angle);
        cosNow[k] = std::cos(angle);
        sinStep[k] = std::sin(2.0 * std::numbers::pi * step);
        cosStep[k] = std::cos(2.0 * std::numbers::pi * step);
    }
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        size += (sizeTarget - size) * glide;
        width += widthStep;
        mix += mixStep;
        double taps[kWidenerTaps];
        for (int k = 0; k < kWidenerTaps; ++k) {
            phases[k] += kWidenerRates[k] / sampleRate;
            phases[k] -= std::floor(phases[k]);
            const double lfo = sinNow[k];
            const double sn = sinNow[k];
            sinNow[k] = sn * cosStep[k] + cosNow[k] * sinStep[k];
            cosNow[k] = cosNow[k] * cosStep[k] - sn * sinStep[k];
            // On a cubic, which keeps the sides' top as the taps are swept.
            taps[k] = readCubic(line, write, std::clamp(widenerTap(k, size, lfo) * sampleRate, 3.0, length - 3.0));
        }
        const double x[2] = {io[0][s], io[1][s]};
        line[write] = static_cast<float>(highPass.process(0.5 * (x[0] + x[1]), highPassCoefs));
        write = write + 1 == length ? 0 : write + 1;
        // The sides: what the taps on the left have and those on the right lack, which cancels in a mono sum.
        // The middle: all four together, a chorus.
        const double side = 0.5 * (taps[0] - taps[1] + taps[2] - taps[3]) * width;
        const double middle = 0.35 * (taps[0] + taps[1] + taps[2] + taps[3]) * (1.0 - width);
        io[0][s] = static_cast<float>(x[0] + (middle + side) * mix);
        io[1][s] = static_cast<float>(x[1] + (middle - side) * mix);
        peak = std::max({peak, std::abs(x[0]), std::abs(x[1]), std::abs(taps[0]), std::abs(taps[3])});
    }
    return peak;
}

} // namespace fx
} // namespace fmvibe
