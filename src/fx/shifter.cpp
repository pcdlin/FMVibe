#include "shifter.h"

#include "../oversampler.h"

namespace fmvibe::fx {

namespace {

// Where the shifters' Hilbert pair keeps 90 degrees from: where Niemitalo's coefficients put it at 48 kHz.
constexpr double kHilbertLowHz = 43.68;

// One of the pair's all-pass chains. Each section is y[n] = c (x[n] + y[n-2]) - x[n-2].
inline double runChain(std::array<std::array<double, 4>, kMaxHilbertSections> &sections,
                       const std::array<double, kMaxHilbertSections> &c, int count, double in) {
    for (int i = 0; i < count; ++i) {
        std::array<double, 4> &s = sections[i];
        const double y = c[i] * (in + s[3]) - s[1];
        s[1] = s[0];
        s[0] = in;
        s[3] = s[2];
        s[2] = y;
        in = y;
    }
    return in;
}

} // namespace

HilbertCoefs hilbertCoefs(double sampleRate) {
    // Up to 48 kHz, six sections a chain keep their sides within a few hundredths of a degree of 90, the other
    // sideband about 70 dB down, where four (Niemitalo's) left it 44 dB down, which feedback brings back up each time
    // round; at higher rates, one more for each octave the band grows by keeps them as close.
    const int sections = sampleRate <= 50000.0 ? 6 : sampleRate <= 100000.0 ? 7 : sampleRate <= 200000.0 ? 8 : 9;
    std::array<double, 2 * kMaxHilbertSections> coefs{};
    halfBandCoefs(2 * sections, std::min(kHilbertLowHz / sampleRate, 0.1), coefs.data());
    HilbertCoefs out;
    out.sections = sections;
    for (int i = 0; i < sections; ++i) { // the smallest goes to the chain heard at once, and on alternately
        out.b[i] = coefs[2 * i];
        out.a[i] = coefs[2 * i + 1];
    }
    return out;
}

std::array<double, 2> Hilbert::process(double x, const HilbertCoefs &coefs) {
    const double first = runChain(a, coefs.a, coefs.sections, x);
    const double second = runChain(b, coefs.b, coefs.sections, x);
    const double inPhase = delayed;
    delayed = first;
    return {inPhase, -second}; // the second chain runs 90 degrees ahead: turned round, it lags like a Hilbert transform
}

double Hilbert::positive(double re, double im, const HilbertCoefs &coefs) {
    // With I and Q the pair's in-phase and quadrature chains, I(w) + j Q(w) is twice I(w) at a positive frequency,
    // where Q lags I by 90 degrees, and nothing at a negative one, where it leads. Its real part is I(re) - Q(im).
    const double first = runChain(a, coefs.a, coefs.sections, re);
    const double second = runChain(b, coefs.b, coefs.sections, im);
    const double inPhase = delayed;
    delayed = first;
    return 0.5 * (inPhase + second);
}

void Shifter::start(const Context &ctx) {
    *this = Shifter{};
    mix = ctx.at(FxParam::ShiftMix);
}

double Shifter::run(const Context &ctx, const HilbertCoefs &coefs, float *const *io, uint32_t n) {
    const double sampleRate = ctx.sampleRate;
    const double hz = shiftHz(ctx.at(FxParam::ShiftAmount), ctx.at(FxParam::ShiftRange));
    const double steps[2] = {hz / sampleRate, hz * (1.0 - 2.0 * ctx.at(FxParam::ShiftSpread)) / sampleRate};
    const double feedback = ctx.at(FxParam::ShiftFeedback);
    const double mixStep = rampStep(mix, ctx.at(FxParam::ShiftMix), n, ctx.rampShortest);
    // The oscillators as phasors turned a step each sample, rather than a cosine and a sine each: from where each
    // phase is after its first step this chunk, which keeps the turning from drifting past a chunk.
    std::array<double, 2> cosNow{}, sinNow{}, cosStep{}, sinStep{};
    for (int ch = 0; ch < 2; ++ch) {
        const double first = phase[ch] + steps[ch];
        const double angle = 2.0 * std::numbers::pi * (first - std::floor(first));
        cosNow[ch] = std::cos(angle);
        sinNow[ch] = std::sin(angle);
        cosStep[ch] = std::cos(2.0 * std::numbers::pi * steps[ch]);
        sinStep[ch] = std::sin(2.0 * std::numbers::pi * steps[ch]);
    }
    double peak = 0.0;
    for (uint32_t s = 0; s < n; ++s) {
        mix += mixStep;
        for (int ch = 0; ch < 2; ++ch) {
            phase[ch] += steps[ch];
            phase[ch] -= std::floor(phase[ch]);
            const double x = io[ch][s];
            const std::array<double, 2> parts = hilbert[ch].process(x + feedback * last[ch], coefs);
            // The analytic signal turned by the oscillator: every partial moves up by `hz` (down if negative). A
            // partial taken past 0 Hz or past half the rate lands on a negative frequency, whose real part would be
            // its mirror image, coming back the other way: only the positive frequencies go on.
            const double turnedRe = parts[0] * cosNow[ch] - parts[1] * sinNow[ch];
            const double turnedIm = parts[0] * sinNow[ch] + parts[1] * cosNow[ch];
            const double y = oneSided[ch].positive(turnedRe, turnedIm, coefs);
            const double c = cosNow[ch];
            cosNow[ch] = c * cosStep[ch] - sinNow[ch] * sinStep[ch];
            sinNow[ch] = sinNow[ch] * cosStep[ch] + c * sinStep[ch];
            last[ch] = y;
            io[ch][s] = static_cast<float>(x + (y - x) * mix);
            peak = std::max({peak, std::abs(x), std::abs(y)});
        }
    }
    return peak;
}

} // namespace fmvibe::fx
