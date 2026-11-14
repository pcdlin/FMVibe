#pragma once

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace fmvibe {

// Topology-preserving state-variable filter (Zavalishin/Simper): stays stable
// when its cutoff moves from block to block.
class Svf {
public:
    struct Coefs {
        double a1 = 1.0;
        double a2 = 0.0;
        double a3 = 0.0;
        double k = 2.0; // damping, 1/Q
    };

    struct Outputs {
        double low;
        double band;
        double high;
    };

    static Coefs coefs(double cutoffHz, double k, double sampleRate) {
        const double g = std::tan(std::numbers::pi * std::clamp(cutoffHz, 10.0, 0.45 * sampleRate) / sampleRate);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        return {a1, g * a1, g * g * a1, k};
    }

    Outputs process(double in, const Coefs &c) {
        const double v3 = in - ic2_;
        const double v1 = c.a1 * ic1_ + c.a2 * v3;
        const double v2 = ic2_ + c.a2 * ic1_ + c.a3 * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        return {v2, v1, in - c.k * v1 - v2};
    }

private:
    double ic1_ = 0.0;
    double ic2_ = 0.0;
};

// Comb delay line length for a sample rate: long enough for the lowest cutoff.
inline int combLength(double sampleRate) {
    return static_cast<int>(std::ceil(sampleRate / kMinCutoff)) + 4;
}

// Resonance 0..1 to SVF damping: 0 is Butterworth (Q 0.71), 1 rings at Q of about 18.
inline double filterDamping(double resonance) {
    return std::numbers::sqrt2 * (1.0 - 0.96 * std::clamp(resonance, 0.0, 1.0));
}
// The same for a 6 dB stage, taken from the SVF as low + band (or high + band): at 0 that is a plain one-pole, and
// resonance adds a peak at the cutoff as high as a 12 dB stage's, the slope past it staying 6 dB per octave.
inline double onePoleDamping(double resonance) {
    return std::numbers::sqrt2 * filterDamping(resonance);
}

inline double combFeedback(double resonance) {
    return 0.97 * std::clamp(resonance, 0.0, 1.0);
}

// The types that self-oscillate at the top of their resonance: the ladder, the diode and the Sallen-Key's.
constexpr bool selfOscillates(FilterType type) {
    return type == FilterType::Ladder || type == FilterType::Diode || type == FilterType::SallenKeyLow ||
           type == FilterType::SallenKeyHigh;
}
// The highest resonance a voice's self-oscillating filter keeps once its operators have faded (see Voice::tail), below
// where it oscillates, so its ringing dies away and the voice can end.
constexpr double kTailResonance = 0.9;
// How far past where it starts to oscillate a self-oscillating type's feedback goes, as a share of that point: its
// resonance itself up to 90 %, then rising faster to 1.5 at the top. They oscillate from about 93 % on; the further
// past it, the harder their saturation has to work to hold the tone, so the louder they ring.
inline double resonanceFeedback(double resonance) {
    const double r = std::clamp(resonance, 0.0, 1.0);
    const double past = std::max(0.0, (r - kTailResonance) / (1.0 - kTailResonance));
    return r + 0.5 * past * past;
}

// The ladder's feedback around its four stages: it oscillates from 4, the loop's gain at the cutoff, where each stage
// passes a quarter of it at 45 degrees.
inline double ladderFeedback(double resonance) {
    return 4.0 * resonanceFeedback(resonance);
}
// The ladder's level at DC falls to 1 / (1 + k) as its feedback k rises, thinning the note as a transistor ladder
// does; it takes back a quarter of that by putting its input in at 1 + k/4.
inline double ladderInputGain(double feedback) {
    return 1.0 + 0.25 * feedback;
}
// The analog types' saturation: a rational curve close to tanh, smooth to its flat ends at 3 times its scale (its
// slope 0 there) and without a library call. Odd, so silence stays silence.
inline double analogSaturate(double x, double scale) {
    const double t = std::clamp(x / scale, -3.0, 3.0);
    return scale * t * (27.0 + t * t) / (27.0 + 9.0 * t * t);
}
// The ladder's, at the input of each of its stages and in its loop, as a transistor ladder's pairs saturate: at the
// scale of the levels an operator sends, so they come out warmed and Drive pushes them on into grit, while
// self-oscillation and loud input are held.
constexpr double kLadderSaturationScale = 1.5;

// The diode ladder: four one-pole stages, each loading the one before by half as much as a passive RC ladder's would
// (kDiodeCoupling), so its poles spread apart; normalized, stage n follows (y[n-1] - y[n]) - c (y[n] - y[n+1]), the
// first taking what goes in, the last nothing above it. Its loop turns half a cycle at kDiodeTurn times the stages'
// rate, where the cutoff is put, and passes 1 / kDiodeOscFeedback of it there, the feedback it oscillates from. Below
// that its peak sits lower, rising toward the cutoff with the resonance, as a diode ladder's does.
constexpr double kDiodeCoupling = 0.5;
constexpr double kDiodeTurn = 1.0552897060221724;
constexpr double kDiodeOscFeedback = 8.61776859504132;
inline double diodeFeedback(double resonance) {
    return kDiodeOscFeedback * resonanceFeedback(resonance);
}
// It loses less of its bass than the transistor ladder: half of it put back.
inline double diodeInputGain(double feedback) {
    return 1.0 + 0.5 * feedback;
}
// Its saturation, at its input alone (the input less the feedback): higher than the ladder's, since it saturates
// once where the ladder does five times, so that its self-oscillation rings out about as loud.
constexpr double kDiodeSaturationScale = 2.0;

// The Sallen-Key's feedback K: its damping is 2 - K, so it oscillates from 2.
inline double sallenKeyFeedback(double resonance) {
    return 2.0 * resonanceFeedback(resonance);
}
// Its clipper, in the feedback alone, as the MS-20's diodes are: low, so the resonance screams and breaks up.
constexpr double kSallenKeySaturationScale = 0.4;

// The Formant type's vowels, A, E, I, O and U, from a bass voice: their first three formants' frequencies in Hz and
// levels in dB. Vowel goes from one to the next in equal steps (formantVowel()).
struct FormantVowel {
    double hz[3];
    double db[3];
};
inline constexpr FormantVowel kFormantVowels[] = {
    {{600.0, 1040.0, 2250.0}, {0.0, -7.0, -9.0}},   // A
    {{400.0, 1620.0, 2400.0}, {0.0, -12.0, -9.0}},  // E
    {{250.0, 1750.0, 2600.0}, {0.0, -30.0, -16.0}}, // I
    {{400.0, 750.0, 2400.0}, {0.0, -11.0, -21.0}},  // O
    {{350.0, 600.0, 2400.0}, {0.0, -20.0, -32.0}},  // U
};
constexpr int kFormantVowelCount = static_cast<int>(std::size(kFormantVowels));
// The formants at a Vowel 0..1, the frequencies taken across on a log scale and the levels in dB (given back as
// gains), and moved by `shift`.
inline FormantVowel formantVowel(double vowel, double shift) {
    const double at = std::clamp(vowel, 0.0, 1.0) * (kFormantVowelCount - 1);
    const int from = std::min(static_cast<int>(at), kFormantVowelCount - 2);
    const double t = at - from;
    FormantVowel v;
    for (int i = 0; i < 3; ++i) {
        const FormantVowel &a = kFormantVowels[from], &b = kFormantVowels[from + 1];
        v.hz[i] = shift * a.hz[i] * std::pow(b.hz[i] / a.hz[i], t);
        v.db[i] = std::pow(10.0, (a.db[i] + (b.db[i] - a.db[i]) * t) / 20.0);
    }
    return v;
}
// Cutoff shifts the formants by half as many octaves as it moves from its middle (kDefaultCutoff), Key Track with it,
// over some two and a half octaves either way; Resonance narrows them, from a Q of 4 to 30.
inline double formantShift(double cutoffHz) {
    return std::sqrt(cutoffHz / cutoffFromNorm(kDefaultCutoff));
}
inline double formantQ(double resonance) {
    return 4.0 + 26.0 * std::clamp(resonance, 0.0, 1.0);
}

// The Morph type's shares of the state-variable filter's low-pass, band-pass (at unity gain at its center) and
// high-pass at a Morph 0..1: through the band-pass, low-pass into band-pass over the first half and band-pass into
// high-pass over the second; through the notch (low-pass and high-pass together), the high-pass rising to meet the
// low-pass over the first half and the low-pass falling away over the second. Each is the plain response at 0, 0.5
// and 1.
struct MorphWeights {
    double low, band, high;
};
inline MorphWeights morphWeights(double morph, MorphMode mode) {
    const double m = std::clamp(morph, 0.0, 1.0);
    if (mode == MorphMode::Notch)
        return m < 0.5 ? MorphWeights{1.0, 0.0, 2.0 * m} : MorphWeights{2.0 - 2.0 * m, 0.0, 1.0};
    return m < 0.5 ? MorphWeights{1.0 - 2.0 * m, 2.0 * m, 0.0} : MorphWeights{0.0, 2.0 - 2.0 * m, 2.0 * m - 1.0};
}

// One filter's settings for a block, derived from its params.
struct FilterSettings {
    FilterType type = FilterType::LowPass;
    // The slope as it plays (see playedSlope()): 6 dB is the resonant stage alone in its one-pole form, 12 the
    // resonant stage, and 18 and 24 add a plain second stage after it, one-pole or Butterworth, so the resonance stays
    // a single peak.
    int slope = kDefaultSlope;
    double drive = 1.0; // input gain into the saturator; 1 = clean
    double driveRoot = 1.0; // its square root, which the saturator's output is divided by
    Svf::Coefs main;
    Svf::Coefs second;
    double combDelay = 2.0; // samples
    double combFeedback = 0.0;
    double combGain = 1.0;
    // The analog types' feedback (the ladder's and diode's k, the Sallen-Key's K) and what their input is scaled by.
    double feedback = 0.0;
    double inputGain = 1.0;
    // The ladder's: each stage's share of its input, g / (1 + g) (each stage putting out G x + (1 - G) times its
    // state), and 1 / (1 + k G^4), which solves the loop for the fourth stage's output.
    double ladderG = 0.0;
    double ladderSolve = 1.0;
    // The diode's: its stages' rate g, the factors that solve (I - g M) y = r for them (the inverse of each pivot and
    // the upper diagonal over it, see Filter::process()), its stages' answer to a unit input, and 1 / (1 + k h[3]).
    double diodeG = 0.0;
    double diodePivot[4]{};
    double diodeUpper[4]{};
    double diodeUnit[4]{};
    double diodeSolve = 1.0;
    // The Sallen-Key's: its integrators' g, 1 / (1 + g), and 1 / (1 + g - g K / (1 + g)), which solves its loop.
    double skG = 0.0;
    double skA = 1.0;
    double skSolve = 1.0;
    // The Formant type's three band-passes and their levels; the Morph type's shares (its stages main and second).
    Svf::Coefs formants[3];
    double formantGain[3]{};
    MorphWeights morph{1.0, 0.0, 0.0};
};

// cutoffHz already includes key tracking. `slope` (0..3, as stored) only matters to the types with a slope, `vowel` to
// the Formant type and `morph` and `mode` to the Morph type.
inline FilterSettings filterSettings(FilterType type, int slope, double cutoffHz, double resonance, double drive,
                                     double sampleRate, double vowel = 0.0, double morph = 0.0,
                                     MorphMode mode = MorphMode::Band) {
    FilterSettings s;
    s.type = type;
    s.slope = playedSlope(type, slope);
    s.drive = 1.0 + 8.0 * std::clamp(drive, 0.0, 1.0);
    s.driveRoot = std::sqrt(s.drive);
    // An integrator's or one-pole's rate for the cutoff, prewarped so it lands where the analog circuit's does.
    const double g = std::tan(std::numbers::pi * std::clamp(cutoffHz, 10.0, 0.45 * sampleRate) / sampleRate);
    switch (type) {
    case FilterType::LowPass:
    case FilterType::HighPass:
    case FilterType::BandPass:
    case FilterType::Notch:
        s.main = Svf::coefs(cutoffHz, s.slope == 0 ? onePoleDamping(resonance) : filterDamping(resonance), sampleRate);
        if (s.slope >= 2) // a one-pole for 18 dB, a Butterworth for 24
            s.second = Svf::coefs(cutoffHz, s.slope == 2 ? 2.0 : std::numbers::sqrt2, sampleRate);
        break;
    case FilterType::Comb:
        s.combDelay = std::max(sampleRate / cutoffHz, 2.0);
        s.combFeedback = combFeedback(resonance);
        s.combGain = std::sqrt(1.0 - s.combFeedback * s.combFeedback); // keeps broadband level steady
        break;
    case FilterType::Ladder: {
        s.ladderG = g / (1.0 + g);
        s.feedback = ladderFeedback(resonance);
        s.inputGain = ladderInputGain(s.feedback);
        const double g2 = s.ladderG * s.ladderG;
        s.ladderSolve = 1.0 / (1.0 + s.feedback * g2 * g2);
        break;
    }
    case FilterType::Diode: {
        // (I - g M) y = s + g u e1 is tridiagonal: below its diagonal -g, above it -g c, along it 1 + g (1 + c) but
        // the last row's 1 + g. Its pivots and the upper diagonal over them are worked out here, once, and so is
        // the stages' answer to a unit input.
        const double c = kDiodeCoupling;
        const double gd = g / kDiodeTurn;
        s.diodeG = gd;
        double upper = 0.0; // the previous row's upper diagonal over its pivot
        for (int i = 0; i < 4; ++i) {
            const double diagonal = 1.0 + gd * (i < 3 ? 1.0 + c : 1.0);
            const double pivot = diagonal + (i > 0 ? gd * upper : 0.0); // less (-g) times the row above's upper
            s.diodePivot[i] = 1.0 / pivot;
            upper = i < 3 ? -gd * c / pivot : 0.0;
            s.diodeUpper[i] = upper;
        }
        double forward[4];
        for (int i = 0; i < 4; ++i)
            forward[i] = ((i == 0 ? gd : 0.0) + (i > 0 ? gd * forward[i - 1] : 0.0)) * s.diodePivot[i];
        for (int i = 3; i >= 0; --i)
            s.diodeUnit[i] = forward[i] - (i < 3 ? s.diodeUpper[i] * s.diodeUnit[i + 1] : 0.0);
        s.feedback = diodeFeedback(resonance);
        s.inputGain = diodeInputGain(s.feedback);
        s.diodeSolve = 1.0 / (1.0 + s.feedback * s.diodeUnit[3]);
        break;
    }
    case FilterType::SallenKeyLow:
    case FilterType::SallenKeyHigh:
        s.skG = g;
        s.skA = 1.0 / (1.0 + g);
        s.feedback = sallenKeyFeedback(resonance);
        s.skSolve = 1.0 / (1.0 + g - g * s.feedback * s.skA);
        break;
    case FilterType::Formant: {
        const FormantVowel v = formantVowel(vowel, formantShift(cutoffHz));
        const double k = 1.0 / formantQ(resonance);
        for (int i = 0; i < 3; ++i) {
            s.formants[i] = Svf::coefs(v.hz[i], k, sampleRate);
            s.formantGain[i] = v.db[i];
        }
        break;
    }
    case FilterType::Morph: // the state-variable filter's stages, as the types it morphs between, their outputs shared out
        s.main = Svf::coefs(cutoffHz, filterDamping(resonance), sampleRate);
        if (s.slope >= 2) // a Butterworth for 24, shared out the same way
            s.second = Svf::coefs(cutoffHz, std::numbers::sqrt2, sampleRate);
        s.morph = morphWeights(morph, mode);
        break;
    case FilterType::Count:
        break;
    }
    return s;
}

// One channel of one voice's filter. The comb's delay line (`combSize` floats,
// see combLength) is owned by the synth.
class Filter {
public:
    double process(double in, const FilterSettings &s, float *comb, int combSize) {
#ifdef __EMSCRIPTEN__
        // WebAssembly can't flush denormals to zero as the plugin's process() does, and a voice's filters ringing
        // out in silence decay into them, at many times the CPU. A signal far below hearing keeps them above.
        guard_ = -guard_;
        in += guard_;
#endif
        // A stage (or the comb's line) that stood still while the type or slope left it out still holds the sound
        // of then: it starts from silence when it is taken up again, rather than playing that back.
        const unsigned uses = usesOf(s);
        if (const unsigned taken = uses & ~uses_; taken && uses_) {
            if (taken & kUsesA)
                a_ = {};
            if (taken & kUsesB)
                b_ = {};
            if (taken & kUsesAnalog) // the analog types' stages, which they share
                state_ = {};
            if (taken & kUsesFormant)
                formants_ = {};
            if (taken & kUsesComb) {
                std::fill_n(comb, combSize, 0.0f);
                combPos_ = 0;
            }
        }
        uses_ = uses;
        // Drive saturates what goes in, taken back down by the drive's square root, except into the analog types
        // (the ladder, the diode and the Sallen-Key's), which take it at full level, so their own saturation works on
        // top of it, and take their output down instead.
        if (s.drive > 1.0) {
            // The saturator averaged over the last sample step (antiderivative
            // anti-aliasing), so the harmonics it adds alias far less.
            const double driven = in * s.drive;
            const double dx = driven - lastDriven_;
            double y;
            if (std::abs(dx) < 1e-6) {
                y = std::tanh(0.5 * (driven + lastDriven_));
                lastKnown_ = false;
            } else {
                const double from = lastKnown_ ? lastLogCosh_ : logCosh(lastDriven_);
                lastLogCosh_ = logCosh(driven);
                lastKnown_ = true;
                y = (lastLogCosh_ - from) / dx;
            }
            lastDriven_ = driven;
            in = selfOscillates(s.type) ? y : y / s.driveRoot;
        } else {
            lastDriven_ = in;
            lastKnown_ = false;
        }
        switch (s.type) {
        case FilterType::LowPass:
        case FilterType::HighPass:
        case FilterType::BandPass:
        case FilterType::Notch: {
            const double y = stage(a_, in, s.type, s.main, s.slope == 0);
            return s.slope >= 2 ? stage(b_, y, s.type, s.second, s.slope == 2) : y;
        }
        case FilterType::Comb: {
            double read = combPos_ - std::min(s.combDelay, combSize - 2.0);
            if (read < 0.0)
                read += combSize;
            if (read >= combSize) // a hair below 0 rounds up to the line's end when wrapped: that is its start
                read -= combSize;
            const int i = static_cast<int>(read);
            // The next sample round the line, and the write moving on, wrapped by a compare rather than a division.
            const int next = i + 1 == combSize ? 0 : i + 1;
            const double delayed = comb[i] + (comb[next] - comb[i]) * (read - i);
            const double y = in + s.combFeedback * delayed;
            comb[combPos_] = static_cast<float>(y);
            combPos_ = combPos_ + 1 >= combSize ? 0 : combPos_ + 1;
            return y * s.combGain;
        }
        case FilterType::Ladder: {
            // The loop solved for the fourth stage's output as if it were linear (each stage puts out G x plus its
            // state's share), then the stages run, each saturating what it takes in, the first the input less the
            // feedback. What comes out is taken down by the drive's square root, as what goes into the other types
            // is. The slope picks the stage it is taken from; the resonance always comes from around all four.
            const double G = s.ladderG, keep = 1.0 - G;
            const double carried =
                ((state_[0] * keep * G + state_[1] * keep) * G + state_[2] * keep) * G + state_[3] * keep;
            const double G2 = G * G;
            const double x = in * s.inputGain;
            const double fourth = (G2 * G2 * x + carried) * s.ladderSolve;
            double y = x - s.feedback * fourth;
            double out = 0.0;
            for (int i = 0; i < 4; ++i) {
                const double v = (analogSaturate(y, kLadderSaturationScale) - state_[i]) * G;
                y = v + state_[i];
                state_[i] = y + v;
                if (i == s.slope)
                    out = y;
            }
            return out / s.driveRoot;
        }
        case FilterType::Diode: {
            // The stages solved for what their states alone put out (the system's pivots worked out once a block),
            // the fourth one's output then found with the feedback as if it were linear, the input less the feedback
            // saturated, and the stages taking it: what they put out is their states' share plus their answer to it.
            double alone[4];
            double forward = 0.0;
            for (int i = 0; i < 4; ++i) {
                forward = (state_[i] + s.diodeG * forward) * s.diodePivot[i];
                alone[i] = forward;
            }
            for (int i = 2; i >= 0; --i)
                alone[i] -= s.diodeUpper[i] * alone[i + 1];
            const double x = in * s.inputGain;
            const double fourth = (alone[3] + s.diodeUnit[3] * x) * s.diodeSolve;
            const double u = analogSaturate(x - s.feedback * fourth, kDiodeSaturationScale);
            double out = 0.0;
            for (int i = 0; i < 4; ++i) {
                const double y = alone[i] + s.diodeUnit[i] * u;
                state_[i] = 2.0 * y - state_[i];
                out = y; // the fourth's, the last
            }
            return out / s.driveRoot;
        }
        case FilterType::SallenKeyLow:
        case FilterType::SallenKeyHigh: {
            // Two integrators, the first taking the input and the feedback K times the clipped difference between
            // their outputs (the band its loop rings in), the second the first's output. The loop solved as if the
            // clipper weren't there gives the difference to clip; the integrators then run with it. The high-pass is
            // what is left of the input past the band's and the low-pass's shares.
            const double g = s.skG, a = s.skA, k = s.feedback;
            const double first = (state_[0] + g * in - g * k * a * state_[1]) * s.skSolve;
            const double clipped = k * analogSaturate(a * (first - state_[1]), kSallenKeySaturationScale);
            const double y1 = (state_[0] + g * (in + clipped)) * a;
            const double y2 = (state_[1] + g * y1) * a;
            state_[0] = 2.0 * y1 - state_[0];
            state_[1] = 2.0 * y2 - state_[1];
            const double out = s.type == FilterType::SallenKeyLow ? y2 : in - 2.0 * (y1 - y2) + clipped - y2;
            return out / s.driveRoot;
        }
        case FilterType::Formant: { // three band-passes, each at unity gain at its center, by its formant's level
            double out = 0.0;
            for (int i = 0; i < 3; ++i)
                out += s.formantGain[i] * s.formants[i].k * formants_[i].process(in, s.formants[i]).band;
            return out;
        }
        case FilterType::Morph: { // each stage's three outputs, each by its share; at 24 dB a second stage after the first
            const Svf::Outputs o = a_.process(in, s.main);
            const double y = s.morph.low * o.low + s.morph.band * s.main.k * o.band + s.morph.high * o.high;
            if (s.slope < 2)
                return y;
            const Svf::Outputs p = b_.process(y, s.second);
            return s.morph.low * p.low + s.morph.band * s.second.k * p.band + s.morph.high * p.high;
        }
        case FilterType::Count:
            break;
        }
        return in;
    }

private:
    // log(cosh(x)), the antiderivative of tanh, without overflowing for large x.
    static double logCosh(double x) {
        const double a = std::abs(x);
        return a + std::log1p(std::exp(-2.0 * a)) - std::numbers::ln2;
    }

    // One 12 dB stage of the types with a slope, or for a low- or high-pass a 6 dB one (`onePole`).
    static double stage(Svf &svf, double in, FilterType type, const Svf::Coefs &c, bool onePole) {
        const Svf::Outputs o = svf.process(in, c);
        switch (type) {
        case FilterType::LowPass:
            return onePole ? o.low + o.band : o.low;
        case FilterType::HighPass:
            return onePole ? o.high + o.band : o.high;
        case FilterType::BandPass:
            return c.k * o.band; // unity gain at the center
        case FilterType::Notch:
            return o.low + o.high;
        default:
            return o.low;
        }
    }

    // What the type and slope ran last time (uses_, 0 before the first time): the state-variable stages (the Morph
    // type's too), the comb's line, an analog type's stages (each its own, though they share state_), or the formants.
    static constexpr unsigned kUsesA = 1, kUsesB = 2, kUsesComb = 4, kUsesLadder = 8, kUsesDiode = 16,
                              kUsesSallenKey = 32, kUsesFormant = 64;
    static constexpr unsigned kUsesAnalog = kUsesLadder | kUsesDiode | kUsesSallenKey;
    static unsigned usesOf(const FilterSettings &s) {
        switch (s.type) {
        case FilterType::Comb:
            return kUsesComb;
        case FilterType::Ladder:
            return kUsesLadder;
        case FilterType::Diode:
            return kUsesDiode;
        case FilterType::SallenKeyLow:
        case FilterType::SallenKeyHigh:
            return kUsesSallenKey;
        case FilterType::Formant:
            return kUsesFormant;
        default:
            return s.slope >= 2 ? kUsesA | kUsesB : kUsesA;
        }
    }
    Svf a_;
    Svf b_;
    // The ladder's, the diode's and the Sallen-Key's (the first two) stages' states.
    std::array<double, 4> state_{};
    std::array<Svf, 3> formants_{}; // the Formant type's band-passes
    unsigned uses_ = 0;
    int combPos_ = 0;
    double lastDriven_ = 0.0; // the saturator's previous input
    double lastLogCosh_ = 0.0; // and logCosh() of it, when worked out already (lastKnown_)
    bool lastKnown_ = false;
#ifdef __EMSCRIPTEN__
    double guard_ = 1e-20; // see process()
#endif
};

// Magnitude response at `hz`, from the analog prototypes, for drawing. `slope` is as stored (0..3). `mix` blends the
// filter with what goes in, as Filter::process()'s callers do: the two are summed with their phases, so a notch at 50 %
// dips about 6 dB, and a band-pass's skirts flatten out 6 dB down.
inline double filterResponse(FilterType type, int slope, double cutoffHz, double resonance, double hz,
                             double mix = 1.0, double vowel = 0.0, double morph = 0.0,
                             MorphMode mode = MorphMode::Band) {
    using Complex = std::complex<double>;
    const double w = hz / cutoffHz;
    const Complex s{0.0, w};
    slope = playedSlope(type, slope);
    auto stage = [&](double k, bool onePole) {
        const Complex d = s * s + k * s + 1.0;
        switch (type) {
        case FilterType::LowPass: // (1 + s) over the second order for the one-pole form
            return (onePole ? 1.0 + s : Complex{1.0}) / d;
        case FilterType::HighPass: // (s^2 + s)
            return (onePole ? s * s + s : s * s) / d;
        case FilterType::BandPass:
            return k * s / d;
        case FilterType::Notch:
            return (s * s + 1.0) / d;
        default:
            return 1.0 / d;
        }
    };
    Complex h{1.0};
    switch (type) {
    case FilterType::LowPass:
    case FilterType::HighPass:
    case FilterType::BandPass:
    case FilterType::Notch:
        h = slope == 0 ? stage(onePoleDamping(resonance), true) : stage(filterDamping(resonance), false);
        if (slope == 2)
            h *= stage(2.0, true);
        else if (slope == 3)
            h *= stage(std::numbers::sqrt2, false);
        break;
    case FilterType::Comb: {
        const double fb = combFeedback(resonance);
        h = std::sqrt(1.0 - fb * fb) / (1.0 - fb * std::polar(1.0, -2.0 * std::numbers::pi * hz / cutoffHz));
        break;
    }
    case FilterType::Ladder: { // the stage the slope taps, (1 + s)^(4 - n) / ((1 + s)^4 + k), scaled as its input is
        const double k = ladderFeedback(resonance);
        const Complex pole = 1.0 + s;
        Complex taps{1.0};
        for (int i = slope + 1; i < 4; ++i)
            taps *= pole;
        const Complex all = pole * pole * pole * pole;
        h = ladderInputGain(k) * taps / (all + k);
        break;
    }
    case FilterType::Diode: {
        // The fourth stage's answer to its input, T, solved from (s/w I - M) y = e1 with the stages' rate w putting
        // the loop's half turn at the cutoff; then T / (1 + k T), scaled as its input is.
        const double c = kDiodeCoupling;
        const Complex z = s * kDiodeTurn;
        const Complex diagonal = z + 1.0 + c;
        Complex upper[4], forward[4];
        Complex pivot = diagonal;
        upper[0] = -c / pivot;
        forward[0] = 1.0 / pivot;
        for (int i = 1; i < 4; ++i) {
            pivot = (i < 3 ? diagonal : z + 1.0) + upper[i - 1]; // less (-1) times the row above's upper
            upper[i] = i < 3 ? -c / pivot : Complex{0.0};
            forward[i] = forward[i - 1] / pivot; // the input's share passed up from below
        }
        const Complex t = forward[3];
        const double k = diodeFeedback(resonance);
        h = diodeInputGain(k) * t / (1.0 + k * t);
        break;
    }
    case FilterType::SallenKeyLow:
    case FilterType::SallenKeyHigh: { // 1 or s^2 over s^2 + (2 - K) s + 1, its peak held short of infinite
        const double k = std::min(sallenKeyFeedback(resonance), 1.99);
        const Complex d = s * s + (2.0 - k) * s + 1.0;
        h = (type == FilterType::SallenKeyLow ? Complex{1.0} : s * s) / d;
        break;
    }
    case FilterType::Formant: { // three band-passes, each at unity gain at its center, by its formant's level
        const FormantVowel v = formantVowel(vowel, formantShift(cutoffHz));
        const double k = 1.0 / formantQ(resonance);
        h = 0.0;
        for (int i = 0; i < 3; ++i) {
            const Complex f{0.0, hz / v.hz[i]};
            h += v.db[i] * k * f / (f * f + k * f + 1.0);
        }
        break;
    }
    case FilterType::Morph: { // each stage's low-pass, band-pass and high-pass, by its share; a Butterworth after for 24
        const MorphWeights w = morphWeights(morph, mode);
        auto shared = [&](double k) { return (w.low + w.band * k * s + w.high * s * s) / (s * s + k * s + 1.0); };
        h = shared(filterDamping(resonance));
        if (slope == 3)
            h *= shared(std::numbers::sqrt2);
        break;
    }
    case FilterType::Count:
        break;
    }
    mix = std::clamp(mix, 0.0, 1.0);
    return std::abs(1.0 - mix + mix * h);
}

} // namespace fmvibe
