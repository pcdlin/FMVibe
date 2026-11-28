#include "parameq.h"

#include <complex>
#include <cstring>

namespace fmvibe {

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

namespace fx {

namespace {

// The z-plane pair (z^2 + c1 z + c2, as c1 and c2) of the analog pair s^2 + k1 s + k0 (s in units of the corner,
// whose angular frequency is `w0` radians a sample), each root s going to e^(s w0): the matched z-transform.
std::pair<double, double> matchedPair(double k1, double k0, double w0) {
    const double disc = k1 * k1 - 4.0 * k0;
    if (disc < 0.0) {
        const double decay = std::exp(-0.5 * k1 * w0);
        return {-2.0 * decay * std::cos(0.5 * std::sqrt(-disc) * w0), decay * decay};
    }
    const double root = std::sqrt(disc);
    const double r1 = std::exp(0.5 * (-k1 + root) * w0), r2 = std::exp(0.5 * (-k1 - root) * w0);
    return {-(r1 + r2), r1 * r2};
}

// The magnitude of b0 + b1 z^-1 + b2 z^-2 over 1 + a1 z^-1 + a2 z^-2 at `w` radians a sample.
double biquadMagnitude(const Coefs &c, double w) {
    const std::complex<double> z = std::polar(1.0, -w);
    return std::abs((c.b0 + (c.b1 + c.b2 * z) * z) / (1.0 + (c.a1 + c.a2 * z) * z));
}

// A parametric EQ band, from its params: the analog band peqResponse() draws, made digital so that it keeps its shape
// up to the rate's half. The bilinear transform (RBJ's cookbook) squeezes the whole analog response below the half
// rate, so a bell or shelf in the top octaves came out narrower and lower than drawn, a notch or cut steeper. Here the
// poles go where the analog ones are (the matched z-transform), and so do the zeros, the gain then set for the level at
// 0 Hz (at the half rate for the low cut, which has none there). The high cut keeps its zeros free: its poles
// matched, its zeros are those whose magnitude meets the analog one at 0 Hz, its corner and the half rate (Vicanek's
// "matched" design), where matched zeros, at infinity, would leave the top unbounded.
Coefs peqCoefs(PeqType type, double freq, double gainDb, double q, double sampleRate) {
    const double hz = std::min(cutoffFromNorm(freq), 0.45 * sampleRate);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * hz / sampleRate;
    const double bandwidth = 1.0 / qFromNorm(q);
    const double root = std::sqrt(a) * bandwidth;
    // The analog band's denominator and numerator as s^2 + k1 s + k0 (the numerator's scale left to the gain below).
    double d1 = bandwidth, d0 = 1.0, n1 = 0.0, n0 = 1.0;
    switch (type) {
    case PeqType::Peak:
    case PeqType::Count:
        d1 = bandwidth / a, n1 = bandwidth * a;
        break;
    case PeqType::LowShelf: // a (s^2 + root s + a) / (a s^2 + root s + 1)
        d1 = root / a, d0 = 1.0 / a, n1 = root, n0 = a;
        break;
    case PeqType::HighShelf: // a (a s^2 + root s + 1) / (s^2 + root s + a)
        d1 = root, d0 = a, n1 = root / a, n0 = 1.0 / a;
        break;
    case PeqType::LowCut:
    case PeqType::HighCut:
    case PeqType::Notch:
        break;
    }
    const auto [a1, a2] = matchedPair(d1, d0, w0);
    if (type == PeqType::HighCut) {
        // |N|^2 = B0 cos^2(w/2) + B1 sin^2(w/2) - 4 b0 b2 sin^2(w), from N's level at 0 (B0) and the half rate (B1):
        // B0, B1 and b0 b2 set so that N over the poles meets the analog magnitude at 0, the corner and the half rate.
        auto analog2 = [&](double w) { // the analog band's squared magnitude at w radians a sample
            const double s = w / w0;
            const double re = 1.0 - s * s, im = bandwidth * s;
            return 1.0 / (re * re + im * im);
        };
        const double p1 = std::sin(0.5 * w0) * std::sin(0.5 * w0), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
        const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
        const double B0 = A0, B1 = A1 * analog2(std::numbers::pi);
        const double B2 = (analog2(w0) * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
        const double r0 = std::sqrt(B0), r1 = std::sqrt(B1), sum = 0.5 * (r0 + r1); // N(1), N(-1); b0 + b2
        const double b0 = 0.5 * (sum + std::sqrt(std::max(sum * sum + B2, 0.0)));
        return {b0, 0.5 * (r0 - r1), b0 > 0.0 ? -B2 / (4.0 * b0) : 0.0, a1, a2};
    }
    double b1 = 0.0, b2 = 0.0;
    if (type == PeqType::LowCut) {
        b1 = -2.0, b2 = 1.0; // both zeros at 0 Hz
    } else {
        const auto zeros = matchedPair(n1, n0, w0);
        b1 = zeros.first, b2 = zeros.second;
    }
    Coefs c{1.0, b1, b2, a1, a2};
    // The level to meet: the analog band's at 0 Hz (1, or a shelf's a^2 or 1), or the low cut's at the half rate.
    double level = 1.0;
    if (type == PeqType::LowShelf)
        level = a * a;
    double at = 0.0;
    if (type == PeqType::LowCut) {
        at = std::numbers::pi;
        const double s = at / w0;
        level = s * s / std::hypot(1.0 - s * s, bandwidth * s);
    }
    const double gain = level / std::max(biquadMagnitude(c, at), 1e-30);
    c.b0 *= gain, c.b1 *= gain, c.b2 *= gain;
    return c;
}

} // namespace

void ParamEq::start(const Context &ctx) {
    *this = ParamEq{};
    amount = ctx.at(FxParam::PeqAmount);
}

void ParamEq::run(const Context &ctx, float *const *io, uint32_t n) {
    const ParamValues &p = ctx.p;
    const int slot = ctx.slot;
    float dry[2][kChunk];
    std::copy_n(io[0], n, dry[0]);
    std::copy_n(io[1], n, dry[1]);
    // Each band's coefficients ramp to where its params put them over the chunk (see BiquadRamp), so modulation
    // doesn't step them every control block. A band switched on or off fades between what it puts out and what it
    // takes in, over 5 ms. One switched off keeps running unheard, so it comes back without starting from silence.
    const double fade = 1.0 / (0.005 * ctx.sampleRate);
    for (int band = 0; band < kPeqBands; ++band) {
        const double target = p[peqOnIndex(slot, band)] >= 0.5 ? 1.0 : 0.0;
        double &heardShare = on[band];
        if (std::isnan(heardShare))
            heardShare = target;
        const PeqType type = peqType(p[peqTypeIndex(slot, band)]);
        const double gain = p[peqGainIndex(slot, band)];
        BiquadRamp &ramp = coefs[band];
        if (peqHasGain(type) && std::abs(gain) < 1e-3) {
            bands[band] = {}; // flat: leave the band out, and start it clean when it comes back
            ramp = BiquadRamp{};
            heardShare = target;
            continue;
        }
        const std::array<double, 5> by{static_cast<double>(type), p[peqFreqIndex(slot, band)], gain,
                                       p[peqQIndex(slot, band)], ctx.sampleRate};
        if (std::memcmp(by.data(), madeFrom[band].data(), sizeof by) != 0) {
            madeFrom[band] = by;
            made[band] = biquadCoefs(peqCoefs(type, by[1], gain, by[3], ctx.sampleRate));
        }
        ramp.begin(made[band], n, ctx.rampShortest);
        if (heardShare == target && !ramp.moving) {
            const BiquadCoefs &c = ramp.now;
            for (int ch = 0; ch < 2; ++ch) {
                Biquad &f = bands[band][ch];
                if (heardShare == 1.0)
                    for (uint32_t s = 0; s < n; ++s)
                        io[ch][s] = static_cast<float>(f.process(io[ch][s], c));
                else
                    for (uint32_t s = 0; s < n; ++s)
                        f.process(io[ch][s], c);
            }
            continue;
        }
        // Fading along a raised cosine, which starts and ends without a kink, or with the coefficients ramping.
        for (uint32_t s = 0; s < n; ++s) {
            heardShare = target > heardShare ? std::min(heardShare + fade, target) : std::max(heardShare - fade, target);
            const double heard = 0.5 - 0.5 * std::cos(std::numbers::pi * heardShare);
            ramp.next();
            for (int ch = 0; ch < 2; ++ch) {
                const double x = io[ch][s];
                io[ch][s] = static_cast<float>(x + heard * (bands[band][ch].process(x, ramp.now) - x));
            }
        }
        ramp.end();
    }
    // Amount mixes what the bands put out with what came in, ramping to a new value over the chunk.
    const double target = ctx.at(FxParam::PeqAmount);
    if (amount != 1.0 || target != 1.0) {
        const double step = rampStep(amount, target, n, ctx.rampShortest);
        for (uint32_t s = 0; s < n; ++s) {
            amount += step;
            for (int ch = 0; ch < 2; ++ch)
                io[ch][s] = static_cast<float>(dry[ch][s] + amount * (io[ch][s] - dry[ch][s]));
        }
        if (std::abs(amount - target) < 1e-9) // arrived, but for rounding
            amount = target;
    }
}

} // namespace fx
} // namespace fmvibe
