// Audio quality measurements on the engine itself: aliasing, zipper noise from
// stepped modulation, clicks when a voice is stolen or a param jumps between
// events, and what oversampling costs. Prints the numbers and checks them
// against limits. Usage: fmvibe-quality

#include "factory.h"
#include "gui/controller.h"
#include "state.h"
#include "synth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace fmvibe;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kBlock = 256;
constexpr size_t kFftSize = 65536;

double keyHz(int key) {
    return 440.0 * std::exp2((key - 69) / 12.0);
}

// In-place radix-2 FFT; the size is a power of two.
void fft(std::vector<std::complex<double>> &x) {
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> step = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> a = x[i + k], b = x[i + k + len / 2] * w;
                x[i + k] = a + b;
                x[i + k + len / 2] = a - b;
                w *= step;
            }
        }
    }
}

// Power per bin up to Nyquist, Blackman-Harris windowed (sidelobes below -92 dB).
std::vector<double> powerSpectrum(const std::vector<float> &signal) {
    std::vector<std::complex<double>> x(kFftSize);
    for (size_t i = 0; i < kFftSize; ++i) {
        const double t = 2.0 * std::numbers::pi * static_cast<double>(i) / kFftSize;
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        x[i] = signal[i] * w;
    }
    fft(x);
    std::vector<double> power(kFftSize / 2);
    for (size_t i = 0; i < power.size(); ++i)
        power[i] = std::norm(x[i]);
    return power;
}

// Energy outside the bins of the expected partials, against the energy in
// them, in dB, over the audible band (20 Hz to 20 kHz).
double unexpectedDb(const std::vector<float> &signal, const std::vector<double> &hz) {
    const std::vector<double> power = powerSpectrum(signal);
    const double binHz = kSampleRate / kFftSize;
    std::vector<bool> expected(power.size(), false);
    for (double f : hz) {
        const long center = std::lround(f / binHz);
        for (long b = center - 6; b <= center + 6; ++b)
            if (b >= 0 && b < static_cast<long>(power.size()))
                expected[static_cast<size_t>(b)] = true;
    }
    double in = 1e-30, out = 1e-30;
    const size_t top = std::min(power.size(), static_cast<size_t>(20000.0 / binHz));
    for (size_t b = static_cast<size_t>(std::ceil(20.0 / binHz)); b < top; ++b)
        (expected[b] ? in : out) += power[b];
    return 10.0 * std::log10(out / in);
}

std::vector<double> harmonics(double f0) {
    std::vector<double> hz;
    for (double f = f0; f < kSampleRate / 2; f += f0)
        hz.push_back(f);
    return hz;
}

// Largest second difference: tiny for steady low tones, as big as the step for a click.
double roughness(const std::vector<float> &x, size_t from, size_t to) {
    double most = 0.0;
    for (size_t i = std::max<size_t>(from, 2); i < std::min(to, x.size()); ++i)
        most = std::max(most, std::abs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
    return most;
}

std::vector<float> joined(std::vector<float> a, const std::vector<float> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

class Engine {
public:
    explicit Engine(int oversampling, int voices = 16) {
        synth_.prepare(kSampleRate, voices, oversampling);
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(GlobalParam::Volume)] = 0.0; // full level, rather than the 12 dB of room it starts with
    }

    void noteOn(int key, int id = -1) { synth_.noteOn({id, 0, 0, static_cast<int16_t>(key)}, 1.0); }
    void noteOff(int key) { synth_.noteOff({-1, 0, 0, static_cast<int16_t>(key)}); }

    // Renders `frames` in host-sized blocks, or in pieces of `piece` frames; returns the left channel.
    std::vector<float> render(size_t frames, uint32_t piece = kBlock) {
        std::vector<float> left(frames), right(frames);
        for (size_t done = 0; done < frames; done += piece) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(piece, frames - done));
            synth_.render(left.data() + done, right.data() + done, n, p);
        }
        return left;
    }

    Synth &synth() { return synth_; }
    ParamValues p{};

private:
    Synth synth_;
};

// Plays `key` for a moment, then records kFftSize samples of it.
template <class Setup>
std::vector<float> steadyTone(int oversampling, int key, const Setup &setup) {
    Engine e(oversampling);
    setup(e.p);
    e.noteOn(key);
    e.render(4800);
    return e.render(kFftSize);
}

void lfoOn(ParamValues &p, int target, double amount, double hz) {
    p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
    p[sourceEnabledIndex(0)] = 1.0;
    p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Sine);
    p[paramIndex(0, SourceParam::Sync)] = 0.0;
    p[paramIndex(0, SourceParam::Rate)] = normFromRate(hz);
    p[slotTargetIndex(0, 0)] = slotTargetValue(target);
    p[slotAmountIndex(0, 0)] = amount;
}

// Level (dB) of a sine at `hz`, run at `factor` times the host rate, after the decimator.
double throughDecimator(int factor, double hz) {
    Decimator d;
    d.prepare(kSampleRate, factor);
    const size_t frames = 24000, settle = 4000;
    std::vector<double> in(frames * factor);
    for (size_t i = 0; i < in.size(); ++i)
        in[i] = std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / (kSampleRate * factor));
    std::vector<float> out(frames, 0.0f);
    for (size_t done = 0; done < frames; done += 32)
        d.process(in.data() + done * factor, out.data() + done, 32);
    double sum = 0.0;
    for (size_t i = settle; i < frames; ++i)
        sum += static_cast<double>(out[i]) * out[i];
    return 10.0 * std::log10(2.0 * sum / static_cast<double>(frames - settle) + 1e-30);
}

// --- The effects chain: a reverb rings on while what went in still waits in its pre-delay; a jump in an effect's
// param ramps however the host's events split the block; a drawn shaper curve passes a quiet, slow sine as drawn.
void effectsChainChecks() {
    std::printf("effects chain:\n");
    auto defaults = [] {
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        return p;
    };
    {
        // A 10 ms burst into a reverb with a 200 ms pre-delay, in the synth's 32-sample pieces: the reverb tells the
        // host it rings on until its tail has been heard, and stops after that.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        p[paramIndex(0, FxParam::ReverbPreDelay)] = normFromTime(0.2);
        p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(1.0);
        constexpr uint32_t kPiece = 32;
        float left[kPiece], right[kPiece];
        double heard = 0.0, done = -1.0, after = 0.0;
        for (size_t t = 0; t < static_cast<size_t>(4.0 * kSampleRate); t += kPiece) {
            for (uint32_t s = 0; s < kPiece; ++s) {
                const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate;
                left[s] = right[s] = t + s < 480 ? static_cast<float>(0.5 * std::sin(phase)) : 0.0f;
            }
            fx.process(left, right, kPiece, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kPiece; ++s) {
                const double level = std::max(std::abs(left[s]), std::abs(right[s]));
                heard = level > 1e-5 ? (t + s) / kSampleRate : heard;
                after = done >= 0.0 ? std::max(after, level) : after;
            }
            if (done < 0.0 && !fx.ringing())
                done = (t + kPiece) / kSampleRate;
        }
        std::printf("      reverb, 200 ms pre-delay: heard until %.2f s, done at %.2f s, loudest after that %.0f dB\n",
                    heard, done, 20.0 * std::log10(after + 1e-30));
        check(done > heard && after < 1e-5,
              "a reverb rings on while its input waits in the pre-delay, until its tail is heard");
    }
    {
        // As Volume's (see the clicks), a jump rendered in single frames ramps as smoothly as in whole blocks.
        struct Jump {
            const char *name;
            FxType type;
            FxParam param;
            double from, to;
        };
        const Jump jumps[] = {
            {"compressor makeup +12 dB", FxType::Compressor, FxParam::CompMakeup, 0.0, 12.0},
            {"delay mix 0 to 100 %", FxType::Delay, FxParam::DelayMix, 0.0, 1.0},
            {"reverb mix 0 to 100 %", FxType::Reverb, FxParam::ReverbMix, 0.0, 1.0},
            {"DJ EQ low band killed", FxType::Eq, FxParam::EqLow, 0.0, -1.0},
            {"3-band dynamics output +12 dB", FxType::MultiDyn, FxParam::MdOutput, 0.0, 12.0},
        };
        double worst = 0.0;
        for (const Jump &jump : jumps) {
            auto roughJump = [&jump](uint32_t piece) {
                Engine e(2);
                e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(jump.type);
                e.p[paramIndex(0, jump.param)] = jump.from;
                e.noteOn(48);
                const std::vector<float> before = e.render(4800, piece);
                e.p[paramIndex(0, jump.param)] = jump.to;
                const std::vector<float> both = joined(before, e.render(2400, piece));
                return roughness(both, before.size() - 2, both.size());
            };
            const double ratio = roughJump(1) / roughJump(kBlock);
            std::printf("      %-30s %7.1fx in 1-frame blocks\n", jump.name, ratio);
            worst = std::max(worst, ratio);
        }
        check(worst < 1.5, "an effect param jump ramps even when events split the block into single frames");
    }
    {
        // A parametric EQ band switched on and off fades rather than clicking: a low cut at 5 kHz, which takes most
        // of a C3 away, switched in single frames, under 4 times as rough as the note before it (as a stolen voice).
        Engine e(2);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
        e.p[peqTypeIndex(0, 2)] = static_cast<double>(PeqType::LowCut);
        e.p[peqFreqIndex(0, 2)] = normFromCutoff(5000.0);
        e.noteOn(48);
        const std::vector<float> before = e.render(4800, 1);
        e.p[peqOnIndex(0, 2)] = 1.0;
        const std::vector<float> on = e.render(2400, 1);
        e.p[peqOnIndex(0, 2)] = 0.0;
        const std::vector<float> all = joined(joined(before, on), e.render(2400, 1));
        const double steady = roughness(all, 2400, before.size());
        const size_t switchedOff = before.size() + on.size();
        const double switched = std::max(roughness(all, before.size() - 2, before.size() + 480),
                                         roughness(all, switchedOff - 2, switchedOff + 480));
        double level = 0.0;
        for (size_t i = before.size() + 480; i < switchedOff; ++i)
            level = std::max(level, static_cast<double>(std::abs(all[i])));
        double dryLevel = 0.0;
        for (size_t i = 2400; i < before.size(); ++i)
            dryLevel = std::max(dryLevel, static_cast<double>(std::abs(all[i])));
        std::printf("      EQ band switched on and off: %.2fx as rough as the note, its low cut leaving x%.3f\n",
                    switched / steady, level / dryLevel);
        check(switched < 4.0 * steady && level < 0.1 * dryLevel,
              "a parametric EQ band switched on and off fades in and out without a click");
    }
    {
        // A sine at -40 dB and 50 Hz through the shaper effect's drawn curve (as it starts: smooth, here made odd),
        // at unity gain: each sample comes out as the curve has it at the middle of its step, through the DC blocker
        // every drawn curve has.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Custom);
        p[paramIndex(0, FxParam::ShaperSymmetric)] = 1.0;
        p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
        p[paramIndex(0, FxParam::ShaperLink)] = 0.0;
        p[paramIndex(0, FxParam::ShaperOutput)] = 0.0;
        p[paramIndex(0, FxParam::ShaperMix)] = 1.0;
        const int curve = shaperParams(0).curve;
        auto get = [&p](int index) { return p[index]; };
        std::vector<float> left(kBlock), right(kBlock);
        double last = 0.0, worst = 0.0, dcIn = 0.0, dcOut = 0.0;
        const double pole = clipBlockerPole(kSampleRate);
        for (size_t t = 0; t < static_cast<size_t>(kSampleRate); t += kBlock) {
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double phase = 2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate;
                left[s] = right[s] = static_cast<float>(0.01 * std::sin(phase));
            }
            const std::vector<float> in = left;
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double shaped = shaperCurveValue(get, ClipMode::Custom, curve, true, 0.5 * (in[s] + last));
                const double drawn = shaped - dcIn + pole * dcOut;
                dcIn = shaped;
                dcOut = drawn;
                if (t + s >= 480) // once faded in
                    worst = std::max(worst, std::abs(left[s] - drawn));
                last = in[s];
            }
        }
        std::printf("      drawn shaper curve, a -40 dB 50 Hz sine: off the curve by %.0f dB at most\n",
                    20.0 * std::log10(worst + 1e-30));
        check(worst < 1e-4, "a drawn shaper curve passes a quiet, slow sine as drawn, without spikes");
    }
    {
        // A drawn curve's points moved (here every one halfway down to the middle, the curve halved): the curve
        // glides there rather than stepping what comes out, in whole blocks and in single frames.
        auto roughMove = [&defaults](uint32_t piece) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
            p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Custom);
            p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
            p[paramIndex(0, FxParam::ShaperMix)] = 1.0;
            const int curve = shaperParams(0).curve;
            std::vector<float> out, left(piece), right(piece);
            double steady = -1.0;
            size_t moved = 0;
            for (size_t t = 0; t < static_cast<size_t>(kSampleRate); t += piece) {
                if (steady < 0.0 && t >= static_cast<size_t>(kSampleRate / 2)) {
                    moved = t;
                    steady = roughness(out, out.size() / 2, out.size());
                    for (int i = 0; i < shapePointCount(p[curve]); ++i)
                        p[shapeFieldIndex(curve, i, PointField::Y)] *= 0.5;
                }
                for (uint32_t s = 0; s < piece; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.5 * std::sin(2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), piece, p, 120.0, 60.0);
                out.insert(out.end(), left.begin(), left.end());
            }
            return roughness(out, moved - 2, out.size()) / steady;
        };
        const double whole = roughMove(kBlock), single = roughMove(1);
        std::printf("      %-30s %7.1fx, %.1fx in 1-frame blocks\n", "drawn shaper curve moved", whole, single);
        check(whole < 4.0 && single < 4.0, "a drawn shaper curve's moved points glide in instead of clicking");
    }
    {
        // A drawn curve's points dragged up and down at 2 Hz by a fifth (as its middle point is, which sets what
        // silence comes out at), under a sound that spends longer above 0 than below: what comes out puts out nothing
        // below the tone, as plain and made symmetric (which then jumps at the middle).
        auto worstDc = [&defaults](bool symmetric) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
            p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Custom);
            p[paramIndex(0, FxParam::ShaperSymmetric)] = symmetric ? 1.0 : 0.0;
            p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
            p[paramIndex(0, FxParam::ShaperMix)] = 1.0;
            const int curve = shaperParams(0).curve;
            std::vector<double> rest(static_cast<size_t>(shapePointCount(p[curve])));
            for (size_t i = 0; i < rest.size(); ++i)
                rest[i] = p[shapeFieldIndex(curve, static_cast<int>(i), PointField::Y)];
            std::vector<float> out, left(kBlock), right(kBlock);
            for (size_t t = 0; t < static_cast<size_t>(2.0 * kSampleRate); t += kBlock) {
                const double offset = 0.2 * std::sin(2.0 * std::numbers::pi * 2.0 * static_cast<double>(t) / kSampleRate);
                for (size_t i = 0; i < rest.size(); ++i)
                    p[shapeFieldIndex(curve, static_cast<int>(i), PointField::Y)] = rest[i] + offset;
                for (uint32_t s = 0; s < kBlock; ++s) {
                    const double phase = 2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate;
                    left[s] = right[s] = static_cast<float>(0.3 * std::sin(phase) + 0.2 * std::cos(2.0 * phase));
                }
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                out.insert(out.end(), left.begin(), left.end());
            }
            // What is left below the 50 Hz tone: averaged over a cycle twice, which takes out its harmonics and the
            // sidebands the drag puts beside them, as an average over each cycle alone wouldn't.
            const size_t cycle = static_cast<size_t>(kSampleRate / 50.0);
            auto average = [cycle](const std::vector<double> &x) {
                std::vector<double> y;
                double sum = 0.0;
                for (size_t i = 0; i < x.size(); ++i) {
                    sum += x[i] - (i >= cycle ? x[i - cycle] : 0.0);
                    if (i + 1 >= cycle)
                        y.push_back(sum / static_cast<double>(cycle));
                }
                return y;
            };
            const std::vector<double> slow = average(average(std::vector<double>(out.begin(), out.end())));
            double worst = 0.0;
            for (size_t i = static_cast<size_t>(kSampleRate / 2); i < slow.size(); ++i)
                worst = std::max(worst, std::abs(slow[i]));
            return worst;
        };
        const double plain = worstDc(false), symmetric = worstDc(true);
        std::printf("      drawn shaper curve dragged: DC %.0f dB, %.0f dB made symmetric\n",
                    20.0 * std::log10(plain + 1e-30), 20.0 * std::log10(symmetric + 1e-30));
        check(plain < 1e-3 && symmetric < 1e-3, "a drawn shaper curve's points dragged put out no DC");
    }
    // Runs `fx` for `seconds` of a sine at `hz` and `level` (0 for silence); returns the loudest sample out.
    auto run = [](Effects &fx, const ParamValues &p, double seconds, double hz, double level) {
        std::vector<float> left(kBlock), right(kBlock);
        float peak = 0.0f;
        for (size_t t = 0; t < static_cast<size_t>(seconds * kSampleRate); t += kBlock) {
            for (uint32_t s = 0; s < kBlock; ++s)
                left[s] = right[s] = static_cast<float>(
                    level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t + s) / kSampleRate));
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kBlock; ++s)
                peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
        }
        return peak;
    };
    {
        // The three-band dynamics on a 1 kHz sine, which the mid band takes. As it starts (RMS, Above -20 dB at 3:1,
        // Below -40 dB at 2:1, 12 dB of Range), a quiet sine comes out louder, by half its RMS level's distance under
        // Below, and a loud one quieter, its RMS level's distance over Above shrunk to a third.
        auto settled = [&](ParamValues &p, double level) { // the gain (dB) once the detector has settled, and the band's
            Effects fx;
            fx.prepare(kSampleRate);
            std::vector<float> left(kBlock), right(kBlock);
            double power = 0.0; // the output's mean square over the last quarter second, against the input's
            for (size_t t = 0; t < static_cast<size_t>(1.25 * kSampleRate); t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        level * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock && t >= static_cast<size_t>(kSampleRate); ++s)
                    power += left[s] * left[s] / (0.25 * kSampleRate);
            }
            const double out = 10.0 * std::log10(power / (0.5 * level * level) + 1e-30);
            return std::array<double, 3>{out, fx.bandGain(0, 1),
                                         20.0 * std::log10(fx.bandLevel(0, 1) + 1e-30)};
        };
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        // The low and high bands hold only what leaks past the crossovers, quiet: at 1:1 up, they leave it be.
        p[mdUpIndex(0, 0)] = p[mdUpIndex(0, 2)] = normFromDynRatio(1.0);
        const double rms = 20.0 * std::log10(std::sqrt(0.5)); // a sine's RMS level against its peak
        const double quietDb = -50.0, loudDb = -6.0;
        const auto quiet = settled(p, std::pow(10.0, quietDb / 20.0));
        const auto loud = settled(p, std::pow(10.0, loudDb / 20.0));
        const double lift = 0.5 * (-40.0 - (quietDb + rms));
        const double pull = (-20.0 + (loudDb + rms + 20.0) / 3.0) - (loudDb + rms);
        std::printf("      3-band dynamics, a -50 dB sine: %+.1f dB (expected %+.1f, the band says %+.1f at %.1f dB)\n",
                    quiet[0], lift, quiet[1], quiet[2]);
        std::printf("      3-band dynamics, a -6 dB sine: %+.1f dB (expected %+.1f, the band says %+.1f at %.1f dB)\n",
                    loud[0], pull, loud[1], loud[2]);
        // The output's gain strays a little from the band's: the other bands pass what leaks into them as it is.
        check(std::abs(quiet[0] - lift) < 0.75 && std::abs(loud[0] - pull) < 0.75,
              "the three-band dynamics lifts a quiet sine below Below and pulls a loud one down above Above");
        check(std::abs(quiet[1] - lift) < 0.5 && std::abs(loud[1] - pull) < 0.5 &&
                  std::abs(quiet[2] - (quietDb + rms)) < 0.5 && std::abs(loud[2] - (loudDb + rms)) < 0.5,
              "a three-band dynamics band tells the editor its level and its gain");
        // Range caps the lift: at 20:1, a sine 40 dB under Below comes up only Range's 12 dB.
        p[mdUpIndex(0, 1)] = normFromDynRatio(20.0);
        const double capped = settled(p, std::pow(10.0, -80.0 / 20.0))[0];
        // Peaks: off RMS, the loud sine's peak is what goes over Above (the detector holding it between peaks).
        p[paramIndex(0, FxParam::MdRms)] = 0.0;
        const auto peaks = settled(p, std::pow(10.0, loudDb / 20.0));
        const double peakPull = (-20.0 + (loudDb + 20.0) / 3.0) - loudDb;
        // At 1:1 both ways, the bands just sum back up.
        for (int band = 0; band < kMcBands; ++band)
            p[mdUpIndex(0, band)] = p[mdDownIndex(0, band)] = normFromDynRatio(1.0);
        const double flat = settled(p, std::pow(10.0, loudDb / 20.0))[0];
        std::printf("      3-band dynamics: a -80 dB sine at 20:1 up %+.1f dB, peaks %+.1f dB (expected %+.1f, the band "
                    "says %+.1f), 1:1 both ways %+.2f dB\n",
                    capped, peaks[0], peakPull, peaks[1], flat);
        check(std::abs(capped - 12.0) < 0.5, "the three-band dynamics lifts no more than its Range");
        check(std::abs(peaks[0] - peakPull) < 0.75 && std::abs(peaks[1] - peakPull) < 0.5,
              "the three-band dynamics follows peaks with RMS off");
        check(std::abs(flat) < 0.05, "the three-band dynamics at 1:1 both ways leaves the level alone");
        // Link is the editor's, which sets every band's field at once (see Controller::linkedBands()): the engine plays
        // each band its own settings, linked or not, so the low band at 1:1 leaves the mid band's loud sine pulled down.
        p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        p[mdUpIndex(0, 0)] = p[mdDownIndex(0, 0)] = normFromDynRatio(1.0);
        const double unlinked = settled(p, std::pow(10.0, loudDb / 20.0))[0];
        p[paramIndex(0, FxParam::MdLink)] = 1.0;
        const double linked = settled(p, std::pow(10.0, loudDb / 20.0))[0];
        std::printf("      3-band dynamics, the low band at 1:1: the mid band's sine %+.1f dB, linked %+.1f dB\n",
                    unlinked, linked);
        check(unlinked < -3.0 && std::abs(linked - unlinked) < 0.05,
              "a linked three-band dynamics' bands play their own settings");
        // At its top, infinity, a ratio holds the level at its threshold: the loud sine's RMS level comes out at
        // Above. Its text says so, and reads back.
        p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        p[mdUpIndex(0, 0)] = p[mdUpIndex(0, 2)] = normFromDynRatio(1.0);
        p[mdDownIndex(0, 1)] = 1.0;
        const auto held = settled(p, std::pow(10.0, loudDb / 20.0));
        const double toAbove = -20.0 - (loudDb + rms);
        char text[32];
        formatParam(mdDownIndex(0, 1), 1.0, text, sizeof(text));
        double typed = 0.0, typedFour = 0.0;
        const bool readBack = parseParam(mdDownIndex(0, 1), text, &typed) && typed == 1.0 &&
                              parseParam(mdDownIndex(0, 1), "4:1", &typedFour) &&
                              std::abs(dynRatio(typedFour) - 4.0) < 1e-9;
        std::printf("      3-band dynamics at %s: the band says %+.1f dB (expected %+.1f)\n", text, held[1], toAbove);
        check(std::abs(held[1] - toAbove) < 0.5 && std::string(text) == "\xE2\x88\x9E:1" && readBack,
              "a three-band dynamics ratio at infinity holds the level at its threshold, and reads as such");
    }
    {
        // The retired three-band compressor in a state from before version 6 loads as a three-band dynamics doing what
        // it did: its settings in the dynamics' fields (thresholds as Above, ratios as Down), the upward side at 1:1,
        // the compressor's knee, peaks; an LFO's modulation of its mid threshold now aims at the mid band's Above; its
        // own fields back at their defaults. Linked, its bands all took the low band's threshold, ratio and gain, and
        // so the dynamics' bands take the low band's values.
        ParamValues p = defaults();
        p[paramIndex(3, FxParam::Type)] = static_cast<double>(FxType::MultiComp);
        p[paramIndex(3, FxParam::McLowFreq)] = normFromCutoff(150.0);
        p[mcThresholdIndex(3, 1)] = -33.0;
        p[mcRatioIndex(3, 2)] = 7.0;
        p[mcGainIndex(3, 0)] = 4.0;
        p[paramIndex(3, FxParam::McRelease)] = normFromTime(0.4);
        p[paramIndex(3, FxParam::McOutput)] = -5.0;
        p[paramIndex(3, FxParam::McLink)] = 1.0;
        p[slotTargetIndex(0, 0)] = static_cast<double>(idOfIndex(mcThresholdIndex(3, 1))) + 1.0;
        p[slotAmountIndex(0, 0)] = 0.25;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, gui::UiLayout{}, true);
        auto load = [&bytes](uint32_t version, ParamValues &loaded) { // the patch, as saved by that version
            std::vector<unsigned char> saved = bytes;
            std::memcpy(saved.data() + sizeof(uint32_t), &version, sizeof(version)); // after the magic
            MemoryReader reader{saved.data(), saved.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            return readParams(&in, loaded);
        };
        ParamValues loaded;
        const bool read = load(5, loaded);
        auto at = [&](FxParam f) { return loaded[paramIndex(3, f)]; };
        const bool type = at(FxParam::Type) == static_cast<double>(FxType::MultiDyn);
        const bool moved = std::abs(at(FxParam::MdLowFreq) - normFromCutoff(150.0)) < 1e-9 &&
                           loaded[mdAboveIndex(3, 1)] == loaded[mdAboveIndex(3, 0)] &&
                           loaded[mdDownIndex(3, 2)] == loaded[mdDownIndex(3, 0)] && loaded[mdGainIndex(3, 0)] == 4.0 &&
                           loaded[mdGainIndex(3, 1)] == 4.0 && loaded[mdGainIndex(3, 2)] == 4.0 &&
                           std::abs(at(FxParam::MdRelease) - normFromTime(0.4)) < 1e-9 && at(FxParam::MdOutput) == -5.0 &&
                           at(FxParam::MdLink) == 1.0 && at(FxParam::MdAttack) == paramTable()[paramIndex(3, FxParam::McAttack)].def;
        bool upward = at(FxParam::MdKnee) == kMcKnee && at(FxParam::MdRms) == 0.0;
        for (int band = 0; band < kMcBands; ++band)
            upward = upward && loaded[mdUpIndex(3, band)] == normFromDynRatio(1.0);
        const bool aimed = slotTarget(loaded[slotTargetIndex(0, 0)]) == mdAboveIndex(3, 1) &&
                           loaded[slotAmountIndex(0, 0)] == 0.25;
        const bool cleared = loaded[mcThresholdIndex(3, 1)] == paramTable()[mcThresholdIndex(3, 1)].def &&
                             loaded[paramIndex(3, FxParam::McLink)] == 0.0;
        check(read && type && moved && upward && aimed && cleared,
              "a state's three-band compressor loads as a three-band dynamics doing what it did, its modulation along");
        // Unlinked, each band's own carry over.
        ParamValues apart;
        {
            ParamValues unlinkedComp = p;
            unlinkedComp[paramIndex(3, FxParam::McLink)] = 0.0;
            std::vector<unsigned char> saved;
            const clap_ostream_t to = memoryOut(saved);
            writePatch(&to, unlinkedComp, gui::UiLayout{}, true);
            uint32_t five = 5;
            std::memcpy(saved.data() + sizeof(uint32_t), &five, sizeof(five));
            MemoryReader reader{saved.data(), saved.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            readParams(&in, apart);
        }
        check(apart[mdAboveIndex(3, 1)] == -33.0 && std::abs(apart[mdDownIndex(3, 2)] - normFromDynRatio(7.0)) < 1e-12 &&
                  apart[mdGainIndex(3, 0)] == 4.0 && apart[mdGainIndex(3, 1)] == 0.0 &&
                  apart[paramIndex(3, FxParam::MdLink)] == 0.0,
              "an unlinked three-band compressor's bands carry their own settings over");
        // A later state holding one (a host can still set the type) loads back as it was saved.
        ParamValues kept;
        const bool same = load(6, kept) && std::equal(kept.begin(), kept.end(), p.begin());
        check(same, "a state from version 6 on loads a slot's three-band compressor as it was saved");
    }
    {
        // The filter effect switched to another type or slope and back: the stage it left out starts from silence,
        // rather than playing back what it held then.
        auto stale = [&](FilterType first, int firstSlope, FilterType second, int secondSlope) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Filter);
            p[paramIndex(0, FxParam::FilterCutoff)] = normFromCutoff(80.0);
            p[paramIndex(0, FxParam::FilterResonance)] = 0.9;
            p[paramIndex(0, FxParam::FilterMix)] = 1.0;
            p[paramIndex(0, FxParam::FilterType)] = static_cast<double>(first);
            p[paramIndex(0, FxParam::FilterSlope)] = firstSlope;
            run(fx, p, 0.5, 80.0, 0.5);
            p[paramIndex(0, FxParam::FilterType)] = static_cast<double>(second);
            p[paramIndex(0, FxParam::FilterSlope)] = secondSlope;
            run(fx, p, 2.0, 0.0, 0.0);
            p[paramIndex(0, FxParam::FilterType)] = static_cast<double>(first);
            p[paramIndex(0, FxParam::FilterSlope)] = firstSlope;
            return run(fx, p, 0.5, 0.0, 0.0);
        };
        const float comb = stale(FilterType::LowPass, 1, FilterType::Comb, 1);
        const float slope = stale(FilterType::LowPass, 3, FilterType::LowPass, 1);
        const float back = stale(FilterType::Comb, 1, FilterType::LowPass, 1);
        std::printf("      filter effect back from another type: low-pass %.1g, 24 dB %.1g, comb %.1g\n", comb, slope,
                    back);
        check(comb < 1e-4f && slope < 1e-4f && back < 1e-4f,
              "the filter effect switched to another type or slope and back plays nothing it held before");
    }
    {
        // A plate (six diffusers a side) switched to a hall (four) and back, after the hall's tail: silence.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
        p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(0.2);
        p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Plate);
        run(fx, p, 0.5, 440.0, 0.5);
        p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Hall);
        run(fx, p, 3.0, 0.0, 0.0);
        p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Plate);
        const float back = run(fx, p, 0.5, 0.0, 0.0);
        std::printf("      a plate back from a hall that rang out: %.1g\n", back);
        check(back < 1e-4f, "a reverb switched back to its plate plays nothing its diffusers held before");
    }
    {
        // An asymmetric shaper driven hard, then silence: the effects keep running while its DC blocker settles,
        // and stop only once what comes out is too quiet to hear, rather than stepping to silence.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Asymmetric);
        p[paramIndex(0, FxParam::ShaperDrive)] = 24.0;
        run(fx, p, 0.5, 110.0, 0.5);
        std::vector<float> left(kBlock, 0.0f), right(kBlock, 0.0f);
        float last = 0.0f;
        for (int b = 0; b < 1000 && fx.ringing(); ++b) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            last = std::max(std::abs(left.back()), std::abs(right.back()));
        }
        std::printf("      an asymmetric shaper's tail: %.1f dB when the effects stop\n", 20.0 * std::log10(last + 1e-30));
        check(!fx.ringing() && last < 1e-5f, "the effects run on until a shaper's DC blocker has settled");
    }
    {
        // A drawn curve made symmetric that doesn't pass through the middle puts out nothing for silence (the
        // middle of its jump), rather than its level there; and an input that isn't a number reads inside its table.
        std::vector<double> table(kClipTableSize);
        for (int i = 0; i <= kClipTableSteps; ++i)
            table[static_cast<size_t>(i)] = 0.5; // level all across
        for (int i = 1; i <= kClipTableSteps; ++i)
            table[static_cast<size_t>(kClipTableSteps + 1 + i)] = table[static_cast<size_t>(kClipTableSteps + i)] +
                                                                 0.5 * kClipTableStep;
        const ClipCurve curve{ClipMode::Custom, table.data(), true};
        ClipState state;
        double out = 0.0;
        for (int s = 0; s < 1000; ++s)
            out = state.run(curve, 0.0, 1.0, 1.0, 1.0, false, clipBlockerPole(kSampleRate));
        const bool inside = std::isfinite(clipTableValue(table.data(), std::nan(""))) &&
                            std::isfinite(clipTableIntegral(table.data(), std::nan("")));
        check(out == 0.0 && inside, "a symmetric drawn curve puts out silence for silence, and reads NaN in its table");
    }
}

// --- Notes, voices and their state: every note reports its end once, in mono, on a key struck again and when a
// lower voice limit cuts it; a velocity or expression that isn't a number; host modulation of the macros; a comb
// read at its line's end; a voice's right channel back from a stretch in mono; and an envelope's curve straightened
// for a while.
void noteAndVoiceChecks() {
    std::printf("notes and voices:\n");
    // Renders `blocks` host blocks, counting into `ends` (by note id, 0 to 9) the notes whose end the plugin reports:
    // ended, and no voice left that the host takes for theirs (see Plugin::reportEndedNotes()). False if anything
    // came out that isn't a number.
    auto play = [](Engine &e, int blocks, int (&ends)[10]) {
        std::vector<float> left(kBlock), right(kBlock);
        bool finite = true;
        for (int b = 0; b < blocks; ++b) {
            e.synth().render(left.data(), right.data(), kBlock, e.p);
            for (uint32_t s = 0; s < kBlock; ++s)
                finite = finite && std::isfinite(left[s]) && std::isfinite(right[s]);
            for (const NoteRef &note : e.synth().endedNotes())
                if (!e.synth().isActive(note) && note.noteId >= 0 && note.noteId < 10)
                    ++ends[note.noteId];
            e.synth().clearEndedNotes();
        }
        return finite;
    };
    {
        // Mono: a note let go and taken over by the next while it still releases is over then; each ends once.
        Engine e(2);
        e.synth().setPlayMode(true, true, false);
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.5);
        int ends[10] = {};
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        play(e, 10, ends);
        e.synth().noteOff({1, 0, 0, 60});
        play(e, 2, ends);
        e.synth().noteOn({2, 0, 0, 64}, 1.0);
        play(e, 10, ends);
        e.synth().noteOff({2, 0, 0, 64});
        play(e, 400, ends);
        std::printf("      mono, a note taken over in its release: ends %d time(s), the next %d\n", ends[1], ends[2]);
        check(ends[1] == 1 && ends[2] == 1 && e.synth().activeVoiceCount() == 0,
              "mono: a note taken over in its release ends once, and so does the next");
    }
    {
        // A key struck again while its last note still rings: with note ids, the last note's end is reported once it
        // is over, though the key sounds on; without them, the host keeps one voice per key, which the new note keeps.
        auto strikeAgain = [&play](Engine &e, int32_t first, int32_t second, int (&ends)[10]) {
            e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.3);
            e.synth().noteOn({first, 0, 0, 60}, 1.0);
            play(e, 4, ends);
            e.synth().noteOff({first, 0, 0, 60});
            play(e, 1, ends);
            e.synth().noteOn({second, 0, 0, 60}, 1.0);
            play(e, 200, ends);
        };
        Engine e(2), plain(2);
        int ends[10] = {}, plainEnds[10] = {};
        strikeAgain(e, 1, 2, ends);
        strikeAgain(plain, -1, -1, plainEnds);
        std::printf("      a key struck again while it rings: the last note ends %d time(s), the new one %d\n", ends[1],
                    ends[2]);
        check(ends[1] == 1 && ends[2] == 0 && e.synth().activeVoiceCount() == 1 &&
                  plain.synth().isActive({-1, 0, 0, 60}),
              "with note ids, a note on a key struck again reports its end, and the new one sounds on");
    }
    {
        // A velocity or a note expression that isn't a number, on an FM pair (where it would reach the phases): the
        // note stays finite, and ends once let go.
        bool fine = true;
        const int32_t expressions[] = {-1, CLAP_NOTE_EXPRESSION_TUNING, CLAP_NOTE_EXPRESSION_VOLUME,
                                       CLAP_NOTE_EXPRESSION_PAN};
        for (int32_t expression : expressions) {
            Engine e(2);
            e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
            e.p[paramIndex(1, OpParam::Out)] = 0.0;
            e.p[matrixIndex(1, 0)] = 0.5;
            int ends[10] = {};
            e.synth().noteOn({1, 0, 0, 60}, expression < 0 ? std::nan("") : 1.0);
            if (expression >= 0)
                e.synth().noteExpression({1, 0, 0, 60}, expression, std::nan(""));
            bool finite = play(e, 20, ends);
            e.synth().noteOff({1, 0, 0, 60});
            finite = play(e, 100, ends) && finite;
            fine = fine && finite && ends[1] == 1;
        }
        check(fine, "a velocity or note expression that isn't a number leaves the note finite, and it ends");
    }
    {
        // Eight notes sounding, and the voice limit lowered to two: the six oldest fade out at once, as stolen ones
        // do, each reporting its end once. A new note then steals the oldest of the two left.
        Engine e(2);
        int ends[10] = {};
        for (int k = 0; k < 8; ++k)
            e.synth().noteOn({k, 0, 0, static_cast<int16_t>(48 + k)}, 1.0);
        play(e, 4, ends);
        e.synth().setVoiceLimit(2);
        play(e, 4, ends);
        const int cut = e.synth().activeVoiceCount();
        const bool oldest = std::all_of(ends, ends + 6, [](int n) { return n == 1; }) && ends[6] == 0 && ends[7] == 0;
        e.synth().noteOn({8, 0, 0, 60}, 1.0);
        play(e, 4, ends);
        std::printf("      eight notes, the limit lowered to two: %d voice(s) left; a ninth note: %d\n", cut,
                    e.synth().activeVoiceCount());
        check(cut == 2 && oldest && ends[6] == 1 && ends[7] == 0 && e.synth().activeVoiceCount() == 2,
              "a lower voice limit cuts the voices past it at once, each note ending once");
    }
    {
        // Host modulation for every voice, of a macro's knob, the mod wheel, the note source's depth or velocity's,
        // moves their targets as turning them does: each takes A's Out down by 100 %, so modulated all the way, A goes
        // silent. The note source puts out all of it at C4, the top of its range here, its curve's end raised to the
        // top, and velocity all of it for a note at full velocity.
        auto peakWith = [](int source, int modulated, double amount, double mod) {
            Engine e(2);
            e.p[slotTargetIndex(source, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(source, 0)] = amount;
            e.p[paramIndex(NoteParam::High)] = 60.0;
            const int curve = paramIndex(NoteParam::Curve);
            e.p[shapeFieldIndex(curve, static_cast<int>(e.p[curve]) - 1, PointField::Y)] = 1.0;
            e.synth().paramMod(modulated, {-1, -1, -1, -1}, mod);
            e.noteOn(60);
            const std::vector<float> out = e.render(9600);
            float peak = 0.0f;
            for (size_t i = 4800; i < out.size(); ++i)
                peak = std::max(peak, std::abs(out[i]));
            return peak;
        };
        const int note = slotAmountIndex(kNoteSource, 0);
        const float macro = peakWith(kMaxSources, paramIndex(0, MacroParam::Value), -1.0, 1.0);
        const float wheel = peakWith(kWheelSource, paramIndex(GlobalParam::ModWheel), -1.0, 1.0);
        const float depth = peakWith(kNoteSource, note, 0.0, -1.0);
        const float velocity = peakWith(kVelocitySource, slotAmountIndex(kVelocitySource, 0), 0.0, -1.0);
        const float still = peakWith(kMaxSources, paramIndex(0, MacroParam::Value), -1.0, 0.0);
        std::printf("      A's peak, modulated away by the host through a macro %.4f, the mod wheel %.4f, the note "
                    "source %.4f, velocity %.4f; %.3f unmodulated\n", macro, wheel, depth, velocity, still);
        check(macro < 1e-3f && wheel < 1e-3f && depth < 1e-3f && velocity < 1e-3f && still > 0.5f,
              "host modulation of a macro, the mod wheel, the note source's and velocity's depth moves their targets");
    }
    {
        // A comb whose delay lies a hair over a whole number of samples (at a cutoff of 1 kHz at 2x, say) reads only
        // inside its line: the float past the line's end, which it would read for the line's start, is never heard
        // (here it isn't a number).
        bool inside = true;
        const double rate = 2.0 * kSampleRate;
        const int size = combLength(rate);
        for (double hz : {1000.0, 1600.0, 3200.0, 4800.0}) {
            std::vector<float> line(static_cast<size_t>(size) + 1, 0.0f);
            line[static_cast<size_t>(size)] = std::nanf("");
            Filter comb;
            const FilterSettings s =
                filterSettings(FilterType::Comb, 1, cutoffFromNorm(normFromCutoff(hz)), 1.0, 0.0, rate);
            for (int i = 0; i < 3 * size; ++i)
                inside = inside && std::isfinite(comb.process(i == 0 ? 1.0 : 0.0, s, line.data(), size));
        }
        check(inside, "a comb never reads past the end of its line");
    }
    {
        // A resonant comb rung in stereo (two unison copies spread apart); then the spread goes, and the comb's input
        // with it, while it rings out; then the spread comes back. The right channel carries on from where the left
        // is, quiet by then, rather than from where it stood still, ringing again.
        Engine e(2);
        const int spread = paramIndex(0, OpParam::UnisonSpread);
        e.p[paramIndex(0, OpParam::Out)] = 0.0;
        e.p[paramIndex(0, OpParam::Decay)] = normFromTime(0.01);
        e.p[paramIndex(0, OpParam::Unison)] = 2.0;
        e.p[spread] = 1.0;
        e.p[sendIndex(0, 0)] = 1.0;
        e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
        e.p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Comb);
        e.p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(110.0);
        e.p[paramIndex(0, FilterParam::KeyTrack)] = 0.0;
        e.p[paramIndex(0, FilterParam::Resonance)] = 1.0;
        e.p[paramIndex(0, FilterParam::Out)] = 1.0;
        e.noteOn(45);
        e.render(24000);
        e.p[spread] = 0.0;
        e.p[paramIndex(0, OpParam::Sustain)] = 0.0;
        e.render(96000);
        e.p[spread] = 1.0;
        std::vector<float> left(kBlock), right(kBlock);
        float peak[2] = {};
        for (int b = 0; b < 10; ++b) {
            e.synth().render(left.data(), right.data(), kBlock, e.p);
            for (uint32_t s = 0; s < kBlock; ++s) {
                peak[0] = std::max(peak[0], std::abs(left[s]));
                peak[1] = std::max(peak[1], std::abs(right[s]));
            }
        }
        std::printf("      a comb back in stereo after ringing out in mono: left %.4f, right %.4f\n", peak[0], peak[1]);
        check(peak[1] < 0.01f && std::abs(peak[1] - peak[0]) < 1e-3f,
              "a voice's right channel comes back from mono where the left is, not where it stood still");
    }
    {
        // An envelope's decay bent, then straightened for a while, then bent again just as before: it carries on
        // along its curve as if it had stayed bent, not from where it was when straightened.
        auto rates = [](double curve) {
            return envRates(0.0, 0.0, normFromTime(1.0), 0.0, 0.5, 0.0, curve, 0.5, kSampleRate);
        };
        const EnvRates bent = rates(0.5), straight = rates(0.0);
        Envelope changed, along;
        changed.start();
        along.start();
        for (int i = 0; i < 4900; ++i)
            changed.next(bent);
        for (int i = 0; i < 9600; ++i)
            changed.next(straight);
        changed.next(bent);
        for (int i = 0; i < 4900 + 9600 + 1; ++i)
            along.next(bent);
        std::printf("      a decay bent again after a straight stretch: %.4f, bent all along %.4f\n", changed.level(),
                    along.level());
        check(std::abs(changed.level() - along.level()) < 1e-6,
              "an envelope's curve bent again after a straight stretch carries on along it");
    }
    {
        // An envelope's attack, decay and release at 0 ms take a sample each, bent or straight, and times between 0
        // and 1 ms (below norm 0, which stays 1 ms) as long as they say; after a 0 ms decay a sustain change is still
        // followed over a millisecond, not in a step, and a release from there ends at once.
        auto rates = [](double time, double sustain, double curve) {
            return envRates(time, 0.0, time, sustain, time, curve, curve, curve, kSampleRate);
        };
        bool instant = true;
        for (const double curve : {0.0, 1.0, -1.0}) {
            const EnvRates zero = rates(kMinEnvNorm, 0.5, curve);
            Envelope env;
            env.start();
            const double attack = env.next(zero);
            for (int i = 0; i < 2; ++i) // the hold's one sample and the decay's
                env.next(zero);
            const double decayed = env.level();
            env.release();
            const double released = env.next(zero);
            instant = instant && attack == 1.0 && std::abs(decayed - 0.5) < 1e-9 && released == 0.0 && env.idle();
        }
        // Half way through a straight attack of 0.5 ms and of 1 ms: 12 and 24 samples in.
        auto halfway = [&](double norm, int samples) {
            Envelope env;
            env.start();
            const EnvRates r = rates(norm, 0.5, 0.0);
            for (int i = 0; i < samples; ++i)
                env.next(r);
            return env.level();
        };
        const double half = halfway(normFromEnvTime(0.0005), 12), one = halfway(0.0, 24);
        Envelope held;
        held.start();
        for (int i = 0; i < 4; ++i)
            held.next(rates(kMinEnvNorm, 0.5, 0.0));
        const EnvRates raised = rates(kMinEnvNorm, 1.0, 0.0);
        const double step = held.next(raised);
        for (int i = 1; i < 96; ++i) // 2 ms
            held.next(raised);
        std::printf("      0 ms envelope segments: %s; half way through a 0.5 ms attack %.3f, a 1 ms one %.3f; a sustain "
                    "raised from 0.5 to 1 after a 0 ms decay: %.3f a sample later, %.4f 2 ms later\n",
                    instant ? "a sample each" : "longer", half, one, step, held.level());
        check(instant && std::abs(half - 0.5) < 0.05 && std::abs(one - 0.5) < 0.05 && step > 0.5 && step < 0.7 &&
                  held.level() > 0.999,
              "a 0 ms attack, decay and release take a sample each, a 0.5 ms and a 1 ms one their time, and a sustain "
              "change after a 0 ms decay is still smoothed");
    }
    {
        // A voice with a 0 ms attack, its sine started at its peak (Phase 25 %), plays at full level from its first
        // sample: nothing but its envelopes shapes a new voice's start, its gains and FM amount starting where they
        // are set rather than ramping in over its first block.
        Engine e(1);
        e.p[paramIndex(0, OpParam::Attack)] = kMinEnvNorm;
        e.p[paramIndex(0, OpParam::Phase)] = 0.25;
        e.noteOn(45);
        const std::vector<float> out = e.render(48);
        std::printf("      a 0 ms attack at Phase 25 %%: its first samples %.3f %.3f %.3f\n", out[0], out[1], out[2]);
        check(out[0] > 0.99f && out[1] > 0.99f, "a 0 ms attack starts at full level from the first sample");
    }
    {
        // Every voice taken by notes that have played a while, then a chord: the chord takes the oldest voices, and
        // each of its notes sounds, rather than each new note stealing the one started a moment before it (whose
        // level is still unknown). A slow attack's notes count at the top of their attack, so a new note steals an
        // old one rather than the newest.
        Engine e(2, 4);
        int ends[10] = {};
        for (int k = 0; k < 4; ++k)
            e.synth().noteOn({k, 0, 0, static_cast<int16_t>(48 + k)}, 1.0);
        play(e, 20, ends);
        e.synth().noteOn({4, 0, 0, 60}, 1.0);
        e.synth().noteOn({5, 0, 0, 64}, 1.0);
        e.synth().noteOn({6, 0, 0, 67}, 1.0);
        play(e, 4, ends);
        const bool chord = ends[4] == 0 && ends[5] == 0 && ends[6] == 0 && ends[0] == 1 && ends[1] == 1 &&
                           ends[2] == 1 && ends[3] == 0;
        Engine slow(2, 2);
        slow.p[paramIndex(0, OpParam::Attack)] = normFromTime(2.0);
        int slowEnds[10] = {};
        slow.synth().noteOn({0, 0, 0, 48}, 1.0);
        play(slow, 20, slowEnds);
        slow.synth().noteOn({1, 0, 0, 52}, 1.0);
        play(slow, 2, slowEnds);
        slow.synth().noteOn({2, 0, 0, 55}, 1.0);
        play(slow, 2, slowEnds);
        std::printf("      a chord into full voices: %d of its 3 notes stolen; slow attacks: the oldest stolen %d time(s)\n",
                    ends[4] + ends[5] + ends[6], slowEnds[0]);
        check(chord && slowEnds[0] == 1 && slowEnds[1] == 0 && slowEnds[2] == 0,
              "a chord into voices that have all played takes the oldest, and every note of it sounds");
    }
    {
        // A note with an id ending while the same key, played without one (the editor's keyboard), sounds on: its
        // end is still reported.
        Engine e(2);
        int ends[10] = {};
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.05);
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        e.synth().noteOn({-1, 0, 0, 60}, 1.0);
        play(e, 4, ends);
        e.synth().noteOff({1, 0, 0, 60});
        play(e, 100, ends);
        check(ends[1] == 1 && e.synth().isActive({-1, 0, 0, 60}),
              "a note with an id reports its end while its key sounds on without one");
    }
    {
        // Mono, two keys down, all notes off: the voice releases where it is, on the newer key, and each note ends
        // once.
        Engine e(2);
        e.synth().setPlayMode(true, true, false);
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.3);
        int ends[10] = {};
        e.synth().noteOn({1, 0, 0, 57}, 1.0);
        play(e, 4, ends);
        e.synth().noteOn({2, 0, 0, 60}, 1.0);
        play(e, 4, ends);
        e.synth().noteOff({-1, -1, -1, -1});
        play(e, 1, ends);
        const bool stays = e.synth().isActive({2, 0, 0, 60}) && !e.synth().isActive({1, 0, 0, 57});
        play(e, 400, ends);
        check(stays && ends[1] == 1 && ends[2] == 1 && e.synth().activeVoiceCount() == 0,
              "mono: all notes off releases the voice on the key it plays, and each note ends once");
    }
    {
        // A reset (the host's, on a transport jump) ends the notes playing, and the next block reports them, a mono
        // note a later one took over too.
        Engine e(2);
        e.synth().setPlayMode(true, true, false);
        int ends[10] = {};
        e.synth().noteOn({1, 0, 0, 57}, 1.0);
        play(e, 4, ends);
        e.synth().noteOn({2, 0, 0, 60}, 1.0);
        play(e, 4, ends);
        e.synth().reset();
        play(e, 1, ends);
        check(ends[1] == 1 && ends[2] == 1 && e.synth().activeVoiceCount() == 0,
              "a reset reports the end of every note playing");
    }
    {
        // One voice, and forty notes at once: each steals the one before, and every end is reported, however many
        // more they are than the voices.
        Engine e(2, 1);
        for (int k = 0; k < 40; ++k)
            e.synth().noteOn({k, 0, 0, static_cast<int16_t>(40 + k)}, 1.0);
        e.render(kBlock);
        const size_t ended = e.synth().endedNotes().size();
        std::printf("      forty notes at once on one voice: %zu end(s) reported\n", ended);
        check(ended == 39, "a burst of notes far past the voices reports every end");
    }
    {
        // An operator into a resonant comb that goes out, its send then turned to 0: the comb rings on, rather than
        // stopping at once.
        Engine e(2);
        e.p[paramIndex(0, OpParam::Out)] = 0.0;
        e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
        e.p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Comb);
        e.p[paramIndex(0, FilterParam::Resonance)] = 1.0;
        e.p[paramIndex(0, FilterParam::Out)] = 1.0;
        e.p[sendIndex(0, 0)] = 1.0;
        e.noteOn(57);
        e.render(9600);
        e.p[sendIndex(0, 0)] = 0.0;
        const std::vector<float> after = e.render(9600);
        float ring = 0.0f;
        for (size_t s = 960; s < 2400; ++s)
            ring = std::max(ring, std::abs(after[s]));
        std::printf("      a comb whose send went to 0, 20-50 ms on: %.1f dB\n", 20.0 * std::log10(ring + 1e-30));
        check(ring > 1e-3f, "a voice filter whose send goes to 0 rings out");
    }
}

// Whether every (id, value) pair a preset's file stores names a param and holds a value in its range, which loading
// it would otherwise quietly clamp.
bool storedInRange(const FactoryPreset &preset) {
    MemoryReader reader{preset.data.data(), preset.data.size()};
    const clap_istream_t stream = memoryIn(reader);
    uint32_t header[3];
    if (stream.read(&stream, header, sizeof header) != sizeof header)
        return false;
    for (uint32_t n = 0; n < header[2]; ++n) {
        uint32_t id;
        double value;
        if (stream.read(&stream, &id, sizeof id) != sizeof id || stream.read(&stream, &value, sizeof value) != sizeof value)
            return false;
        const int index = indexOfId(id);
        if (index < 0 || !(value >= paramTable()[index].min && value <= paramTable()[index].max))
            return false;
    }
    return true;
}

// Whether a preset's file is as the preset tool leaves it: sparse, and holding only what loading it gives.
bool storedSparse(const FactoryPreset &preset) {
    ParamValues values;
    gui::UiLayout layout;
    if (!loadFactoryPreset(preset, values, layout))
        return false;
    std::vector<unsigned char> bytes;
    const clap_ostream_t stream = memoryOut(bytes);
    return writePatch(&stream, values, layout, true) &&
           std::equal(bytes.begin(), bytes.end(), preset.data.begin(), preset.data.end());
}

void presetContentChecks() {
    std::printf("factory preset files:\n");
    bool sparse = true;
    for (const FactoryPreset &preset : factoryPresets()) {
        if (!storedSparse(preset)) {
            std::printf("      %-20s isn't sparse: run fmvibe-presettool on its file\n",preset.name.c_str());
            sparse = false;
        }
    }
    check(sparse, "every factory preset's file is sparse, as fmvibe-presettool writes it");
}

// The scope: an operator's output, phase-locked to it, captured only while asked for.
void scopeChecks() {
    std::printf("scope:\n");
    auto telemetry = std::make_unique<Telemetry>(); // large: the analyzers' spectra are in it
    // How alike a captured span is to `shape` (a cycle, 0..1 -> value) repeated over it: 1 is the same up to level.
    auto likeness = [&](double (*shape)(double)) {
        double dot = 0.0, own = 0.0, its = 0.0;
        for (int bin = 0; bin < telemetry->scopeBins; ++bin) {
            const double want = shape(static_cast<double>(bin) / kScopeBinsPerCycle);
            dot += telemetry->scope[bin] * want;
            own += telemetry->scope[bin] * telemetry->scope[bin];
            its += want * want;
        }
        return own > 0.0 ? dot / std::sqrt(own * its) : 0.0;
    };
    auto sine = [](double x) { return std::sin(2.0 * std::numbers::pi * x); };
    auto square = [](double x) { return x - std::floor(x) < 0.5 ? 1.0 : -1.0; };

    Engine e(2);
    e.noteOn(69);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    const bool idle = telemetry->scopeBins == 0;
    e.synth().setScope(0, 2);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    const double sineLike = likeness(sine);
    const bool whole = telemetry->scopeBins == 2 * kScopeBinsPerCycle && telemetry->scopeCursor < 0.0f;
    std::printf("      a sine at A4 over 2 cycles: %d points, %.4f like a sine from its start\n", telemetry->scopeBins,
                sineLike);
    check(idle && whole && sineLike > 0.999,
          "the scope captures nothing until asked, then a sine's cycles from where they start");

    // It stands still from one span to the next, FM bending it or not.
    auto still = [&] {
        e.synth().telemetry(*telemetry);
        const std::vector<float> first(telemetry->scope.begin(), telemetry->scope.begin() + telemetry->scopeBins);
        e.render(kBlock * 3 + 7);
        e.synth().telemetry(*telemetry);
        double most = 0.0, peak = 0.0;
        for (size_t bin = 0; bin < first.size(); ++bin) {
            most = std::max(most, static_cast<double>(std::abs(telemetry->scope[bin] - first[bin])));
            peak = std::max(peak, static_cast<double>(std::abs(first[bin])));
        }
        return peak > 0.1 && most < 0.02 * peak;
    };
    const bool sineStill = still();
    e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
    e.p[matrixIndex(1, 0)] = 0.5;
    e.render(4800);
    const bool fmStill = still();
    const double fmLike = likeness(sine);
    std::printf("      B into A at 50 %%: %.4f like a sine\n", fmLike);
    check(sineStill && fmStill && fmLike < 0.98, "FM changes the scope's span, which stands still from one to the next");

    // A modulator at half the ratio makes the two cycles differ.
    e.p[paramIndex(1, OpParam::Ratio)] = normFromRatio(0.5);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    double apart = 0.0;
    for (int bin = 0; bin < kScopeBinsPerCycle; ++bin)
        apart = std::max(apart, static_cast<double>(
                                    std::abs(telemetry->scope[bin] - telemetry->scope[bin + kScopeBinsPerCycle])));
    std::printf("      B at ratio 0.5: the two cycles differ by up to %.3f\n", apart);
    check(apart > 0.2, "a modulator at half the ratio makes the scope's two cycles differ");
    e.p[paramIndex(1, OpParam::Enabled)] = 0.0;

    // A square, one cycle; a low note's span, over 4 cycles, shows as it fills; asked for nothing, nothing again.
    e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Square);
    e.synth().setScope(0, 1);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    const double squareLike = likeness(square);
    e.noteOff(69);
    e.render(48000);
    e.noteOn(12); // 16 Hz: 4 cycles take a quarter of a second
    e.synth().setScope(0, 4);
    e.render(2400);
    e.synth().telemetry(*telemetry);
    const float cursor = telemetry->scopeCursor;
    e.synth().setScope(-1, 4);
    e.render(kBlock);
    e.synth().telemetry(*telemetry);
    std::printf("      a square: %.4f like one; a low note's span filled %.2f of the way\n", squareLike, cursor);
    check(squareLike > 0.95 && cursor > 0.0f && cursor < 1.0f && telemetry->scopeBins == 0,
          "the scope takes a square's shape, shows a slow span as it fills, and stops when not asked");

    // A high operator, a cycle of 4.5 samples (24 x A4 at 1x): the scope still shows a sine, standing still (but
    // for the lines between samples moving a little with where the samples fall in the cycle), and costs little more
    // than the voice, letting most short spans go by. Timed as the fastest of a few runs.
    {
        Engine high(1);
        high.p[paramIndex(0, OpParam::Ratio)] = normFromRatio(24.0);
        high.noteOn(69);
        high.render(4800);
        auto fastest = [&] {
            double best = 1e9;
            for (int run = 0; run < 5; ++run) {
                const auto start = std::chrono::steady_clock::now();
                high.render(48000);
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
            return best;
        };
        const double without = fastest();
        high.synth().setScope(0, 1);
        const double with = fastest();
        high.synth().telemetry(*telemetry);
        const double highLike = likeness(sine);
        const std::vector<float> first(telemetry->scope.begin(), telemetry->scope.begin() + kScopeBinsPerCycle);
        high.render(kBlock * 5 + 3);
        high.synth().telemetry(*telemetry);
        double most = 0.0;
        for (int bin = 0; bin < kScopeBinsPerCycle; ++bin)
            most = std::max(most, static_cast<double>(std::abs(telemetry->scope[bin] - first[bin])));
        std::printf("      A at 10.6 kHz: %.3f like a sine, moving by %.4f; a second of it %.2f ms, %.2f ms scoped\n",
                    highLike, most, without * 1000.0, with * 1000.0);
        check(telemetry->scopeBins == kScopeBinsPerCycle && telemetry->scopeCursor < 0.0f && highLike > 0.9 &&
                  most < 0.1 && with < 1.5 * without + 0.0005,
              "a high operator's scope shows its cycle, standing still, for little more than the voice costs");
    }
}

// The header's output scope: the output over two periods of the newest note, its spans following one another at the
// note's pitch, captured only while the synth analyzes for the editor.
void outScopeChecks() {
    std::printf("output scope:\n");
    auto telemetry = std::make_unique<Telemetry>(); // large: the analyzers' spectra are in it
    auto peakOf = [&] {
        float peak = 0.0f;
        for (float v : telemetry->outScope)
            peak = std::max(peak, std::abs(v));
        return peak;
    };
    // How alike the span is to two cycles of a sine, from wherever in its cycle (the decimators delay it a little): 1
    // is the same up to level.
    auto sineLike = [&] {
        double sin = 0.0, cos = 0.0, own = 0.0, its = 0.0;
        for (int bin = 0; bin < kOutScopeBins - 1; ++bin) { // the last point is the next span's first
            const double angle = 2.0 * std::numbers::pi * kOutScopePeriods * bin / (kOutScopeBins - 1);
            sin += telemetry->outScope[bin] * std::sin(angle);
            cos += telemetry->outScope[bin] * std::cos(angle);
            own += telemetry->outScope[bin] * telemetry->outScope[bin];
            its += std::sin(angle) * std::sin(angle);
        }
        return own > 0.0 ? std::hypot(sin, cos) / std::sqrt(own * its) : 0.0;
    };
    // It stands still from one span to the next: it moves by less than 3 % of its peak.
    Engine e(2);
    double moved = 0.0;
    auto still = [&] {
        e.synth().telemetry(*telemetry);
        const auto first = telemetry->outScope;
        e.render(kBlock * 3 + 7);
        e.synth().telemetry(*telemetry);
        double most = 0.0, peak = 0.0;
        for (int bin = 0; bin < kOutScopeBins; ++bin) {
            most = std::max(most, static_cast<double>(std::abs(telemetry->outScope[bin] - first[bin])));
            peak = std::max(peak, static_cast<double>(std::abs(first[bin])));
        }
        moved = peak > 0.0 ? most / peak : 1.0;
        return peak > 0.1 && most < 0.03 * peak;
    };

    e.noteOn(69);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    const bool idle = peakOf() == 0.0f;
    e.synth().setAnalyzing(true);
    e.render(4800);
    e.synth().telemetry(*telemetry);
    const double like = sineLike();
    const float level = peakOf();
    const bool sineStill = still();
    std::printf("      an A4 sine: %.4f like two cycles of one, at %.3f\n", like, level);
    check(idle && like > 0.999 && level > 0.5 && sineStill,
          "the output scope captures nothing until the synth analyzes, then two periods of the note, standing still");

    // Whatever the wave: one crossing 0 going up many times a period, and one repeating every other period (its two
    // periods differing), stand still too.
    auto crossings = [&] {
        int count = 0;
        for (int bin = 1; bin < kOutScopeBins; ++bin)
            count += telemetry->outScope[bin - 1] < 0.0f && telemetry->outScope[bin] >= 0.0f;
        return count;
    };
    e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
    e.p[paramIndex(1, OpParam::Ratio)] = normFromRatio(3.0);
    e.p[matrixIndex(1, 0)] = 1.0;
    e.render(4800);
    const bool richStill = still();
    const double richMoved = moved;
    const int rich = crossings();
    const double richLike = sineLike();
    e.p[paramIndex(1, OpParam::Ratio)] = normFromRatio(0.5);
    e.p[matrixIndex(1, 0)] = 0.5;
    e.render(4800);
    const bool halfStill = still();
    const double halfMoved = moved;
    double apart = 0.0;
    for (int bin = 0; bin < (kOutScopeBins - 1) / 2; ++bin)
        apart = std::max(apart, static_cast<double>(std::abs(telemetry->outScope[bin] -
                                                             telemetry->outScope[bin + (kOutScopeBins - 1) / 2])));
    std::printf("      B at ratio 3 into A: %d rising crossings, %.4f like a sine, moving by %.4f; B at ratio 0.5: the "
                "periods differ by up to %.3f, moving by %.4f\n",
                rich, richLike, richMoved, apart, halfMoved);
    check(richStill && rich > 2 * kOutScopePeriods && richLike < 0.98 && halfStill && apart > 0.1,
          "the output scope stands still for a wave crossing 0 many times a period, and one repeating every other "
          "period");
    e.p[paramIndex(1, OpParam::Enabled)] = 0.0;

    // A high note's span holds more periods, at least kOutScopeMinSpan long, and stands still too, wherever the samples
    // fall in its periods.
    {
        Engine high(2);
        high.synth().setAnalyzing(true);
        high.noteOn(117); // 7 kHz: 2 periods would be 0.29 ms
        high.render(4800);
        high.synth().telemetry(*telemetry);
        const auto first = telemetry->outScope;
        high.render(kBlock * 5 + 3);
        high.synth().telemetry(*telemetry);
        double most = 0.0, peak = 0.0;
        for (int bin = 0; bin < kOutScopeBins; ++bin) {
            most = std::max(most, static_cast<double>(std::abs(telemetry->outScope[bin] - first[bin])));
            peak = std::max(peak, static_cast<double>(std::abs(first[bin])));
        }
        std::printf("      a 7 kHz note: moving by %.4f of its peak\n", peak > 0.0 ? most / peak : 1.0);
        check(peak > 0.1 && most < 0.03 * peak, "a high note's output scope stands still, over more periods");
    }

    // Let go of, the note fades to silence, which the scope takes too; switched off, it shows nothing.
    e.noteOff(69);
    e.render(48000);
    e.synth().telemetry(*telemetry);
    const float silent = peakOf();
    e.noteOn(69);
    e.render(4800);
    e.synth().setAnalyzing(false);
    e.render(kBlock);
    e.synth().telemetry(*telemetry);
    std::printf("      a second after the note: %.6f; switched off: %.6f\n", silent, peakOf());
    check(silent < 1e-4f && peakOf() == 0.0f, "the output scope falls silent with the output, and shows nothing off");
}

} // namespace

int main() {
    const int factors[] = {1, 2, 4, 8};
    // --- The decimator: flat up to 20 kHz, and what would fold into the audible band gone.
    std::printf("decimator (dB):\n");
    for (int factor : {2, 4, 8}) {
        double ripple = 0.0;
        for (double hz : {50.0, 1000.0, 10000.0, 18000.0, 20000.0})
            ripple = std::max(ripple, std::abs(throughDecimator(factor, hz)));
        double leak = -300.0;
        for (int k = 1; k < factor; ++k) // tones that fold onto 1 kHz and 19.5 kHz
            for (double hz : {k * kSampleRate - 1000.0, k * kSampleRate + 1000.0, k * kSampleRate - 19500.0,
                              k * kSampleRate + 19500.0})
                if (hz > kSampleRate / 2 && hz < kSampleRate * factor / 2)
                    leak = std::max(leak, throughDecimator(factor, hz));
        std::printf("      %dx: passband ripple %.3f, worst alias %.1f\n", factor, ripple, leak);
        check(ripple < 0.1 && leak < -90.0, "the decimator is flat to 20 kHz and keeps aliases below -90 dB");
    }

    // --- Aliasing: the energy that isn't a harmonic of the note.
    struct AliasCase {
        const char *name;
        int key;
        void (*setup)(ParamValues &);
        double limit2x; // dB at 2x oversampling
    };
    const AliasCase cases[] = {
        {"saw at C7", 96,
         [](ParamValues &p) { p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw); }, -70.0},
        {"FM B(x4) -> A 80% at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(1, OpParam::Enabled)] = 1.0;
             p[paramIndex(1, OpParam::Ratio)] = normFromRatio(4.0);
             p[matrixIndex(1, 0)] = 0.8;
         },
         -80.0},
        // Linear FM by a harmonic modulator is harmonic too (its mean frequency stays), as is Ring; Exp FM's isn't.
        {"linear FM B(x4) -> A 60% at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(1, OpParam::Enabled)] = 1.0;
             p[paramIndex(1, OpParam::Ratio)] = normFromRatio(4.0);
             p[matrixIndex(1, 0)] = 0.6;
             p[routeModeIndex(1, 0)] = static_cast<double>(RouteMode::LinearFm);
         },
         -80.0},
        {"ring B(x4) -> A 100% at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(1, OpParam::Enabled)] = 1.0;
             p[paramIndex(1, OpParam::Ratio)] = normFromRatio(4.0);
             p[matrixIndex(1, 0)] = 1.0;
             p[routeModeIndex(1, 0)] = static_cast<double>(RouteMode::Ring);
         },
         -80.0},
        {"feedback A 70% at C5", 72, [](ParamValues &p) { p[matrixIndex(0, 0)] = 0.7; }, -70.0},
        // At full feedback the operator is close to chaos, so oversampling helps less.
        {"feedback A 100% at C5", 72, [](ParamValues &p) { p[matrixIndex(0, 0)] = 1.0; }, -25.0},
        {"saw into driven filter at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Cutoff)] = 1.0;
             p[paramIndex(0, FilterParam::Resonance)] = 0.0;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -45.0},
    };
    std::printf("aliasing (energy off the harmonics, dB):\n");
    for (const AliasCase &c : cases) {
        double db[4];
        for (int f = 0; f < 4; ++f)
            db[f] = unexpectedDb(steadyTone(factors[f], c.key, c.setup), harmonics(keyHz(c.key)));
        std::printf("      %-30s 1x %7.1f   2x %7.1f   4x %7.1f   8x %7.1f\n", c.name, db[0], db[1], db[2], db[3]);
        char what[96];
        std::snprintf(what, sizeof(what), "%s: below %.0f dB at 2x", c.name, c.limit2x);
        check(db[1] < c.limit2x, what);
        std::snprintf(what, sizeof(what), "%s: oversampling doesn't make it worse", c.name);
        check(db[3] < db[0] + 3.0, what);
    }

    // --- Zipper: modulation stepping once per control block leaves partials
    // around multiples of the control rate. A 20 Hz LFO on a 220 Hz sine should
    // give only the sidebands it makes.
    std::printf("zipper (energy off the expected partials, dB):\n");
    {
        const int key = 57;
        const double f0 = keyHz(key), lfo = 20.0;
        auto tremolo = [&](ParamValues &p) {
            p[paramIndex(0, OpParam::Out)] = 0.5;
            lfoOn(p, paramIndex(0, OpParam::Out), 0.4, lfo);
        };
        const double out = unexpectedDb(steadyTone(1, key, tremolo), {f0, f0 - lfo, f0 + lfo});
        std::printf("      %-30s %7.1f\n", "LFO on operator Out", out);
        check(out < -70.0, "LFO on an operator's level doesn't zipper");

        // Index modulation makes sidebands at f0 +- k * lfo around every harmonic.
        auto indexMod = [&](ParamValues &p) {
            p[paramIndex(1, OpParam::Enabled)] = 1.0;
            p[matrixIndex(1, 0)] = 0.3;
            lfoOn(p, matrixIndex(1, 0), 0.2, lfo);
        };
        std::vector<double> partials;
        for (double h : harmonics(f0))
            for (int k = -12; k <= 12; ++k)
                partials.push_back(h + k * lfo);
        const double matrix = unexpectedDb(steadyTone(1, key, indexMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on B -> A", matrix);
        check(matrix < -80.0, "LFO on a matrix amount doesn't zipper");

        auto cutoffMod = [&](ParamValues &p) {
            p[paramIndex(0, OpParam::Out)] = 0.0;
            p[sendIndex(0, 0)] = 1.0;
            p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            p[paramIndex(0, FilterParam::Out)] = 1.0;
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::LowPass);
            p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(220.0);
            p[paramIndex(0, FilterParam::KeyTrack)] = 0.0;
            p[paramIndex(0, FilterParam::Resonance)] = 0.0;
            lfoOn(p, paramIndex(0, FilterParam::Cutoff), 0.1, lfo);
        };
        const double cutoff = unexpectedDb(steadyTone(1, key, cutoffMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on filter cutoff", cutoff);
        check(cutoff < -80.0, "LFO on a filter cutoff doesn't zipper");
    }

    // --- The random LFO: how far its steps go with chaos, and how smoothing glides between them.
    std::printf("random LFO:\n");
    {
        uint32_t rng = 12345u;
        double last = 0.0, widest = 0.0;
        for (int n = 0; n < 2000; ++n) {
            const double next = randomStep(last, 0.0, rng);
            widest = std::max(widest, std::abs(next - last));
            last = next;
        }
        std::printf("      widest step at 0 %% chaos  %7.3f\n", widest);
        check(widest <= 0.1 + 1e-12, "at 0 % chaos a random LFO steps at most a tenth of the way either way");
        int quarters[4] = {};
        for (int n = 0; n < 4000; ++n) {
            last = randomStep(last, 1.0, rng);
            ++quarters[std::clamp(static_cast<int>((last + 1.0) * 2.0), 0, 3)];
        }
        std::printf("      values per quarter at full chaos  %d %d %d %d\n", quarters[0], quarters[1], quarters[2],
                    quarters[3]);
        check(std::all_of(std::begin(quarters), std::end(quarters), [](int n) { return n > 850 && n < 1150; }),
              "at full chaos it lands anywhere, evenly");
        double jump = 0.0;
        for (int i = 0; i < 1000; ++i)
            jump = std::max(jump, std::abs(randomGlide(-1.0, 1.0, (i + 1) / 1000.0, 1.0) -
                                           randomGlide(-1.0, 1.0, i / 1000.0, 1.0)));
        check(randomGlide(-1.0, 1.0, 0.0, 1.0) == -1.0 && jump < 0.004 && randomGlide(-1.0, 1.0, 0.5, 0.25) == 1.0,
              "smoothing glides from one value to the next, for its share of the cycle");
        auto wobble = [](ParamValues &p) {
            p[paramIndex(0, OpParam::Out)] = 0.5;
            lfoOn(p, paramIndex(0, OpParam::Out), 0.4, 40.0);
            p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Random);
            p[paramIndex(0, SourceParam::RandomSmooth)] = 1.0;
            p[paramIndex(0, SourceParam::RandomChaos)] = 0.5;
        };
        const std::vector<float> out = steadyTone(2, 57, wobble);
        check(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }),
              "a smoothed random LFO on an operator's level stays finite");
    }

    // --- A freeform LFO's points sharing a time: a spike up at 0.5 and a drop at the cycle's end. Either side of a
    // line straight up or down the shape runs on as drawn; at its time the last point in order counts, not the
    // topmost. Its outline passes through every point of a spike.
    {
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        auto set = [&](int point, double x, double y) {
            p[shapePointIndex(0, point, PointField::X)] = x;
            p[shapePointIndex(0, point, PointField::Y)] = y;
            p[shapePointIndex(0, point, PointField::Curve)] = 0.0;
        };
        p[shapeCountIndex(0)] = 6.0;
        set(0, 0.0, -0.5);
        set(1, 0.5, -0.5);
        set(2, 0.5, 0.75);
        set(3, 0.5, -0.25);
        set(4, 1.0, -0.25);
        set(5, 1.0, 0.25);
        auto get = [&](int index) { return p[index]; };
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
        const bool sides = near(shapeValue(get, 0, 0.25), -0.5) && near(shapeValue(get, 0, 0.75), -0.25);
        const bool last = near(shapeValue(get, 0, 0.5), -0.25) && near(shapeValue(get, 0, 0.0), -0.5);
        bool spike = false;
        shapeOutline(get, 0, 100, [&](double x, double y) { spike = spike || (near(x, 0.5) && near(y, 0.75)); });
        std::printf("freeform LFO: %.2f at a spike, %.2f at the start\n", shapeValue(get, 0, 0.5), shapeValue(get, 0, 0.0));
        check(sides && last && spike, "a freeform LFO's points at one time make a vertical line, its last counting");
        // A ramp from 0 at the start to 1 at the cycle's end: where a one-shot stays (a phase of 1), the last point
        // counts, not the first one the cycle would wrap round to.
        p[shapeCountIndex(0)] = 2.0;
        set(0, 0.0, 0.0);
        set(1, 1.0, 1.0);
        std::printf("      a ramp to its cycle's end: %.2f at the start, %.2f at the end\n", shapeValue(get, 0, 0.0),
                    shapeValue(get, 0, 1.0));
        check(near(shapeValue(get, 0, 0.0), 0.0) && near(shapeValue(get, 0, 0.5), 0.5) &&
                  near(shapeValue(get, 0, 1.0), 1.0),
              "a freeform LFO ending on a point at the cycle's end stays there, as a one-shot does");
    }

    // --- An anchored LFO stands where the host's transport puts it, as if it had started at its Phase at the song's
    // start: synced, by the beats, else by the seconds, whenever a note starts; a block split into pieces keeps the
    // position moving; stopped, it runs on from where it was; a one-shot isn't anchored; Random steps each cycle.
    std::printf("anchored LFO:\n");
    {
        auto telemetry = std::make_unique<Telemetry>();
        // A sine on quarter notes at 120 BPM (2 Hz), anchored, and a note started where the transport stands: the
        // phase of LFO 1 one control block in, when it was read (a one-shot's a control block on from its start).
        auto anchored = [](Engine &e) {
            e.synth().setTempo(120.0);
            lfoOn(e.p, paramIndex(0, OpParam::Out), 0.1, 2.0);
            e.p[paramIndex(0, SourceParam::Sync)] = 1.0;
            e.p[paramIndex(0, SourceParam::Division)] = 13.0; // 1/4
            e.p[paramIndex(0, SourceParam::Anchor)] = 1.0;
        };
        auto phase = [&](Engine &e) {
            e.synth().telemetry(*telemetry);
            return static_cast<double>(telemetry->lfoPhases[0]);
        };
        auto phaseAt = [&](double beats, auto setup) {
            Engine e(1);
            anchored(e);
            setup(e);
            e.synth().setTransport(true, beats, beats * 0.5);
            e.noteOn(57);
            e.render(32);
            return phase(e);
        };
        auto none = [](Engine &) {};
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-4; };
        const double atBeat = phaseAt(2.25, none), later = phaseAt(7.6, none);
        const double offset = phaseAt(2.25, [](Engine &e) { e.p[paramIndex(0, SourceParam::Phase)] = 0.5; });
        const double free = phaseAt(1.1, [](Engine &e) { // 2 Hz unsynced, 0.55 s in: 1.1 cycles
            e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
        });
        const double oneShot = phaseAt(2.25, [](Engine &e) { e.p[paramIndex(0, SourceParam::OneShot)] = 1.0; });
        std::printf("      at beat 2.25: %.4f (Phase 50 %%: %.4f), at 7.6: %.4f; unsynced 0.55 s in: %.4f; a one-shot: "
                    "%.4f\n", atBeat, offset, later, free, oneShot);
        check(near(atBeat, 0.25) && near(offset, 0.75) && near(later, 0.6) && near(free, 0.1) && oneShot < 0.01,
              "an anchored LFO stands where the transport puts it, from its Phase, synced or not; a one-shot doesn't");

        // 0.1 s from the song's start, rendered in blocks of 480 and in pieces of 7, the transport set once: both at
        // a fifth of a cycle. Then stopped: on from there, another fifth a tenth of a second later.
        Engine whole(1), pieces(1);
        for (Engine *e : {&whole, &pieces}) {
            anchored(*e);
            e->synth().setTransport(true, 0.0, 0.0);
            e->noteOn(57);
        }
        whole.render(4800, 480);
        pieces.render(4800, 7);
        whole.render(32);
        pieces.render(32);
        const double inBlocks = phase(whole), inPieces = phase(pieces);
        whole.synth().setTransport(false, 0.0, 0.0);
        whole.render(4800);
        const double stopped = phase(whole);
        std::printf("      0.1 s in: %.4f in blocks, %.4f in pieces; stopped, 0.1 s later: %.4f\n", inBlocks, inPieces,
                    stopped);
        check(near(inBlocks, 0.2) && near(inPieces, 0.2),
              "an anchored LFO follows the transport through blocks split into pieces");
        check(near(stopped, 0.4), "an anchored LFO runs on from where it is while the transport is stopped");

        // Random: the value it holds through a cycle, and a new one in the next.
        Engine random(1);
        anchored(random);
        random.p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Random);
        random.noteOn(57);
        auto heldAt = [&](double beats) {
            random.synth().setTransport(true, beats, beats * 0.5);
            random.render(32);
            random.synth().telemetry(*telemetry);
            return telemetry->lfoTo[0];
        };
        const float first = heldAt(4.2), same = heldAt(4.9), next = heldAt(5.1);
        std::printf("      random: %.3f at beat 4.2, %.3f at 4.9, %.3f at 5.1\n", first, same, next);
        check(first == same && next != same, "an anchored random LFO steps as the transport passes a cycle");
    }

    // --- The parametric EQ's and phase disperser's analyzers: a sine shows in its band, at its level (a flat EQ
    // passes it as it is, and so does a disperser all wet, an all-pass).
    std::printf("parametric EQ and phase disperser analyzers:\n");
    for (FxType type : {FxType::ParamEq, FxType::Disperser}) {
        const bool eq = type == FxType::ParamEq;
        Effects fx;
        fx.prepare(kSampleRate);
        fx.setAnalyzing(true);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(type);
        p[paramIndex(0, FxParam::DisperserMix)] = 1.0;
        std::vector<float> left(kBlock), right(kBlock);
        size_t t = 0;
        for (int block = 0; block < 40; ++block) {
            for (uint32_t s = 0; s < kBlock; ++s, ++t)
                left[s] = right[s] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * t / kSampleRate));
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
        }
        int loudest = 0;
        for (int band = 1; band < kSpectrumBands; ++band)
            if (fx.spectrum(0, band) > fx.spectrum(0, loudest))
                loudest = band;
        const int expected = static_cast<int>(normFromCutoff(1000.0) * kSpectrumBands);
        const double db = 20.0 * std::log10(fx.spectrum(0, loudest));
        std::printf("      a 1 kHz sine at -6 dB through the %s: band %d (expected %d) at %.1f dB\n",
                    eq ? "EQ" : "disperser", loudest, expected, db);
        check(std::abs(loudest - expected) <= 1 && std::abs(db + 6.02) < 1.5,
              eq ? "the EQ's analyzer shows a sine in its band, at its level"
                 : "the phase disperser's analyzer shows a sine in its band, at its level");
    }

    // --- Synced sweeps: the phaser and flanger go round once per note value of the host tempo, whatever their
    // Rate (here standing still). A quarter takes 0.5 s at 120 bpm and 1 s at 60, so a quarter of a second in,
    // the sweep is half way round, or a quarter.
    std::printf("synced sweeps (cycles after 0.25 s of a 1/4):\n");
    {
        auto phaseAfter = [](FxType type, double tempo) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            const bool phaser = type == FxType::Phaser;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(type);
            p[paramIndex(0, phaser ? FxParam::PhaserRate : FxParam::FlangerRate)] = 0.0;
            p[paramIndex(0, phaser ? FxParam::PhaserSync : FxParam::FlangerSync)] = 1.0;
            p[paramIndex(0, phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision)] = 13.0; // 1/4
            std::vector<float> left(kBlock), right(kBlock);
            for (uint32_t done = 0, total = static_cast<uint32_t>(kSampleRate / 4); done < total; done += kBlock)
                fx.process(left.data(), right.data(), std::min(kBlock, total - done), p, tempo, 60.0);
            return fx.sweepPhase(0);
        };
        bool synced = true;
        for (FxType type : {FxType::Phaser, FxType::Flanger}) {
            const double fast = phaseAfter(type, 120.0), slow = phaseAfter(type, 60.0);
            std::printf("      %-8s %.3f at 120 bpm, %.3f at 60 bpm\n", type == FxType::Phaser ? "phaser" : "flanger",
                        fast, slow);
            synced = synced && std::abs(fast - 0.5) < 1e-3 && std::abs(slow - 0.25) < 1e-3;
        }
        check(synced, "synced, the phaser's and flanger's sweeps follow the host tempo");
    }

    // --- The reverb's tails, from an impulse into both sides (2.5 s decay, at full wet). Late on, a tail's
    // spectrum, averaged over a second, keeps close to its own third-octave smoothing: no resonance rings on above
    // the rest, as a plain network's would, its lines' modes standing still. Its two sides come out unalike. Without
    // damping, every frequency falls 60 dB in about the set time.
    std::printf("reverb tails (an impulse, 2.5 s decay):\n");
    {
        auto impulse = [](ReverbType type, double damping) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
            p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(type);
            p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(2.5);
            p[paramIndex(0, FxParam::ReverbDamping)] = damping;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 0.0;
            p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
            const size_t total = static_cast<size_t>(4.0 * kSampleRate);
            std::vector<float> left(total), right(total);
            left[0] = right[0] = 1.0f;
            for (size_t done = 0; done < total; done += kBlock)
                fx.process(left.data() + done, right.data() + done,
                           static_cast<uint32_t>(std::min<size_t>(kBlock, total - done)), p, 120.0, 60.0);
            return std::pair{left, right};
        };
        // The mean of how far (dB) the averaged spectrum of 0.3 to 1.5 s strays from its smoothing, 200 Hz to 5 kHz.
        auto lumpiness = [](const std::vector<float> &tail) {
            constexpr size_t kN = 4096;
            std::vector<double> power(kN / 2, 0.0);
            for (size_t start = static_cast<size_t>(0.3 * kSampleRate); start + kN < static_cast<size_t>(1.5 * kSampleRate);
                 start += kN / 2) {
                std::vector<std::complex<double>> x(kN);
                for (size_t i = 0; i < kN; ++i)
                    x[i] = tail[start + i] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kN));
                fft(x);
                for (size_t k = 0; k < kN / 2; ++k)
                    power[k] += std::norm(x[k]);
            }
            const double binHz = kSampleRate / kN;
            double stray = 0.0;
            int bins = 0;
            for (size_t k = static_cast<size_t>(200.0 / binHz); k < static_cast<size_t>(5000.0 / binHz); ++k) {
                double around = 0.0;
                int count = 0;
                for (size_t j = static_cast<size_t>(k / 1.122); j <= static_cast<size_t>(k * 1.122); ++j, ++count)
                    around += power[j];
                stray += std::abs(10.0 * std::log10(power[k] * count / around));
                ++bins;
            }
            return stray / bins;
        };
        const char *const names[] = {"hall", "room", "plate", "shimmer"};
        bool smooth = true;
        for (int t = 0; t < static_cast<int>(ReverbType::Count); ++t) {
            const auto [left, right] = impulse(static_cast<ReverbType>(t), 0.4);
            double ll = 0.0, rr = 0.0, lr = 0.0;
            for (size_t i = static_cast<size_t>(0.1 * kSampleRate); i < static_cast<size_t>(1.5 * kSampleRate); ++i) {
                ll += left[i] * left[i];
                rr += right[i] * right[i];
                lr += left[i] * right[i];
            }
            const double lumps = lumpiness(left), correlation = lr / std::sqrt(ll * rr);
            std::printf("      %-8s tail strays %.2f dB from its smoothing, sides correlate %+.2f\n", names[t], lumps,
                        correlation);
            smooth = smooth && lumps < 2.5 && std::abs(correlation) < 0.15;
        }
        check(smooth, "every reverb algorithm's tail is smooth, without lasting resonances, and its sides unalike");
        // The decay, in the octave around 1 kHz: the energy left there (Schroeder's backward integral) falls from -5
        // to -35 dB in half the decay time.
        const auto [left, right] = impulse(ReverbType::Hall, 0.0);
        size_t n = 1;
        while (n < left.size())
            n *= 2;
        std::vector<std::complex<double>> octave(n);
        for (size_t i = 0; i < left.size(); ++i)
            octave[i] = {left[i], right[i]}; // both sides at once: they come back as the real and imaginary parts
        fft(octave);
        for (size_t k = 0; k < n; ++k) {
            const double hz = std::min(k, n - k) * kSampleRate / n;
            octave[k] = hz > 707.0 && hz < 1414.0 ? std::conj(octave[k]) : 0.0; // conjugated, for the inverse
        }
        fft(octave);
        std::vector<double> remaining(left.size());
        double energy = 0.0;
        for (size_t i = left.size(); i-- > 0;)
            remaining[i] = energy += std::norm(octave[i]);
        double at5 = 0.0, at35 = 0.0;
        for (size_t i = 0; i < remaining.size(); ++i) {
            const double db = 10.0 * std::log10(remaining[i] / remaining[0]);
            at5 = at5 == 0.0 && db < -5.0 ? i / kSampleRate : at5;
            at35 = at35 == 0.0 && db < -35.0 ? i / kSampleRate : at35;
        }
        const double t60 = 2.0 * (at35 - at5);
        std::printf("      undamped hall falls 60 dB around 1 kHz in %.2f s (set to 2.5 s)\n", t60);
        check(std::abs(t60 / 2.5 - 1.0) < 0.15, "an undamped reverb's tail falls 60 dB in its decay time");
    }

    // --- A shimmer's tail after a 0.4 s A4 sine (4 s decay, at full wet), with Shimmer half and full up. As a whole it
    // falls 60 dB in about the decay time; its energy stays on the octaves (within 3 %), where the shifter puts it; and
    // each of the first two octaves up is a narrow line, not smeared by the shifter's crossfades: most of its energy
    // within 4 Hz. At the longest decay, undamped, with Shimmer full up, it still dies away.
    std::printf("shimmer tails (an A4 sine, 4 s decay):\n");
    {
        auto shimmer = [](double amount, double decay, double damping, double seconds) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
            p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Shimmer);
            p[paramIndex(0, FxParam::ReverbShimmer)] = amount;
            p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(decay);
            p[paramIndex(0, FxParam::ReverbDamping)] = damping;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 0.0;
            p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
            const size_t total = static_cast<size_t>(seconds * kSampleRate), note = static_cast<size_t>(0.4 * kSampleRate);
            std::vector<float> left(total, 0.0f), right(total, 0.0f);
            for (size_t i = 0; i < note; ++i) {
                const double fade = std::min({1.0, i / 240.0, (note - i) / 240.0});
                left[i] = right[i] =
                    static_cast<float>(0.3 * fade * std::sin(2.0 * std::numbers::pi * 440.0 * i / kSampleRate));
            }
            for (size_t done = 0; done < total; done += kBlock)
                fx.process(left.data() + done, right.data() + done,
                           static_cast<uint32_t>(std::min<size_t>(kBlock, total - done)), p, 120.0, 60.0);
            return left;
        };
        // The power spectrum of [from, to) s, Hann-windowed pieces of n samples averaged, and the energy in a band.
        auto spectrum = [](const std::vector<float> &x, double from, double to, size_t n) {
            std::vector<double> power(n / 2, 0.0);
            for (size_t s = static_cast<size_t>(from * kSampleRate); s + n <= static_cast<size_t>(to * kSampleRate);
                 s += n / 2) {
                std::vector<std::complex<double>> a(n);
                for (size_t i = 0; i < n; ++i)
                    a[i] = x[s + i] * (0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n));
                fft(a);
                for (size_t k = 0; k < n / 2; ++k)
                    power[k] += std::norm(a[k]);
            }
            return power;
        };
        auto band = [](const std::vector<double> &power, double lo, double hi) {
            const double bin = kSampleRate / (2.0 * power.size());
            double e = 0.0;
            for (size_t k = static_cast<size_t>(lo / bin); k <= static_cast<size_t>(hi / bin); ++k)
                e += power[k];
            return e;
        };
        bool lasts = true, onOctaves = true, narrow = true;
        for (double amount : {0.5, 1.0}) {
            const std::vector<float> tail = shimmer(amount, 4.0, 0.4, 4.5);
            // Half-second pieces from 0.5 to 4 s into the tail (0.9 to 4.4 s): the level they fall by, and what lies
            // off the octaves.
            double first = 0.0, last = 0.0, off = 0.0;
            constexpr int kPieces = 7;
            for (int piece = 0; piece < kPieces; ++piece) {
                const double from = 0.9 + 0.5 * piece;
                const std::vector<double> power = spectrum(tail, from, from + 0.5, 8192);
                const double total = band(power, 40.0, 20000.0);
                double on = 0.0;
                for (int k = 0; k <= 5; ++k)
                    on += band(power, 440.0 * std::exp2(k) * 0.97, 440.0 * std::exp2(k) * 1.03);
                off += 10.0 * std::log10((total - on) / total) / kPieces;
                first = piece == 0 ? total : first;
                last = total;
            }
            const double t60 = 60.0 * 0.5 * (kPieces - 1) / (10.0 * std::log10(first / last));
            const std::vector<double> fine = spectrum(tail, 0.7, 2.7, 32768);
            auto line = [&](double hz) { return 10.0 * std::log10(band(fine, hz - 4.0, hz + 4.0) / band(fine, hz - 60.0, hz + 60.0)); };
            const double octave = line(880.0), two = line(1760.0);
            std::printf("      Shimmer %3.0f %%: falls 60 dB in %.2f s, %.1f dB of it off the octaves, 880 and 1760 Hz "
                        "within 4 Hz %.1f and %.1f dB\n",
                        100.0 * amount, t60, off, octave, two);
            lasts = lasts && t60 > 0.7 * 4.0 && t60 < 1.35 * 4.0;
            onOctaves = onOctaves && off < -11.0;
            narrow = narrow && octave > -2.5 && two > -5.0;
        }
        check(lasts, "a shimmer's tail falls 60 dB in about its decay time, with Shimmer half and full up");
        check(onOctaves, "a shimmer's tail keeps its energy on the octaves its shifter climbs to");
        check(narrow, "a shimmer's shifted octaves are narrow lines, not smeared by the shifter's crossfades");
        const std::vector<float> longest = shimmer(1.0, 20.0, 0.0, 30.0);
        auto energy = [&](double from) {
            double e = 0.0;
            for (size_t i = static_cast<size_t>(from * kSampleRate); i < static_cast<size_t>((from + 1.0) * kSampleRate); ++i)
                e += longest[i] * longest[i];
            return e;
        };
        const double early = energy(5.0), late = energy(28.0);
        std::printf("      20 s decay, undamped, Shimmer full: 28 s against 5 s %.1f dB\n", 10.0 * std::log10(late / early));
        check(std::isfinite(late) && late < 0.1 * early, "a shimmer's tail dies away at its longest decay, undamped");
    }

    // --- The voice filters' analyzers: an A4 sine through filter 1 shows in its band, turned down by a low-pass
    // an octave under it; filter 2, switched off, shows nothing.
    std::printf("voice filter analyzers:\n");
    {
        auto measure = [](double cutoffHz) {
            Engine e(2);
            e.synth().setAnalyzing(true);
            e.p[paramIndex(0, OpParam::Out)] = 0.0;
            e.p[sendIndex(0, 0)] = 1.0;
            e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            e.p[paramIndex(0, FilterParam::Out)] = 1.0;
            e.p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::LowPass);
            e.p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(cutoffHz);
            e.p[paramIndex(0, FilterParam::KeyTrack)] = 0.0;
            e.noteOn(69);
            e.render(9600);
            Telemetry t;
            e.synth().telemetry(t);
            return t;
        };
        const Telemetry open = measure(20000.0), closed = measure(220.0);
        int loudest = 0;
        for (int band = 1; band < kSpectrumBands; ++band)
            if (open.filterSpectrum[0][band] > open.filterSpectrum[0][loudest])
                loudest = band;
        const int expected = static_cast<int>(normFromCutoff(440.0) * kSpectrumBands);
        const double db = 20.0 * std::log10(open.filterSpectrum[0][loudest]);
        const double closedDb = 20.0 * std::log10(std::max(closed.filterSpectrum[0][loudest], 1e-9f));
        const bool offSilent = std::all_of(open.filterSpectrum[1].begin(), open.filterSpectrum[1].end(),
                                           [](float level) { return level == 0.0f; });
        std::printf("      an A4 sine through F1: band %d (expected %d) at %.1f dB, %.1f dB under a 220 Hz low-pass\n",
                    loudest, expected, db, closedDb);
        check(std::abs(loudest - expected) <= 1 && db > -12.0 && db < 3.0 && closedDb < db - 9.0 && offSilent,
              "a voice filter's analyzer shows what goes through it, and a switched-off one nothing");
    }

    // --- The example presets: every value in its param's range, and each one sounds, at a sane level.
    std::printf("example presets (peak of a held C4, half a second):\n");
    {
        bool sane = true;
        for (const FactoryPreset &preset : factoryPresets()) {
            Engine e(2);
            gui::UiLayout layout;
            const bool inRange = storedInRange(preset) && loadFactoryPreset(preset, e.p, layout);
            e.noteOn(60);
            const std::vector<float> out = e.render(static_cast<size_t>(kSampleRate / 2));
            float peak = 0.0f;
            bool finite = true;
            for (float x : out) {
                finite = finite && std::isfinite(x);
                peak = std::max(peak, std::abs(x));
            }
            std::printf("      %-16s %6.3f%s\n", preset.name.c_str(), peak, inRange ? "" : "  (a value out of range)");
            sane = sane && inRange && finite && peak > 0.01f && peak < 2.0f;
        }
        check(sane, "the example presets hold valid values and sound at a sane level");
    }
    presetContentChecks();
    scopeChecks();
    outScopeChecks();

    // --- Clicks. Roughness is the largest second difference; a steady low sine has almost none.
    std::printf("clicks (largest second difference, against a smooth reference):\n");
    {
        // A third note over a limit of two steals the oldest voice while it sounds.
        double worst = 0.0, steady = 0.0;
        for (int trial = 0; trial < 8; ++trial) {
            Engine e(2, 2);
            e.synth().setVoiceLimit(2);
            e.p[paramIndex(0, OpParam::Attack)] = normFromTime(0.005);
            e.noteOn(48);
            e.render(2400 + 37 * trial);
            e.noteOn(52);
            const std::vector<float> before = e.render(2400);
            e.noteOn(55);
            const std::vector<float> both = joined(before, e.render(2400));
            steady = std::max(steady, roughness(both, 0, before.size()));
            worst = std::max(worst, roughness(both, before.size() - 2, both.size()));
        }
        std::printf("      %-30s %7.1fx\n", "stolen voice", worst / steady);
        check(worst < 4.0 * steady, "a stolen voice fades out instead of clicking");

        // Poly legato: a note played over a held one at its sustain joins it there, a square starting at full level;
        // it doesn't step in (it fades in over 5 ms, its gain ramping in over its first block besides).
        worst = steady = 0.0;
        for (int trial = 0; trial < 8; ++trial) {
            Engine e(2);
            e.synth().setPlayMode(false, true, false);
            e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Square);
            e.p[paramIndex(0, OpParam::Sustain)] = 1.0;
            e.noteOn(48);
            const std::vector<float> before = e.render(4800 + 37 * trial);
            e.noteOn(55);
            const std::vector<float> both = joined(before, e.render(2400));
            steady = std::max(steady, roughness(both, 2400, before.size()));
            worst = std::max(worst, roughness(both, before.size() - 2, both.size()));
        }
        std::printf("      %-30s %7.1fx\n", "poly legato join", worst / steady);
        check(worst < 4.0 * steady, "a note joining a held one in poly legato fades in instead of clicking");
        // The fade itself: the joined voice alone (both notes less the held one alone, the same otherwise) stays
        // under 40 % of its full level in its first millisecond, where its gain's ramp over its first block alone
        // would have it all but there.
        {
            auto run = [](bool join) {
                Engine e(2);
                e.synth().setPlayMode(false, true, false);
                e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Square);
                e.p[paramIndex(0, OpParam::Sustain)] = 1.0;
                e.noteOn(48);
                e.render(4800);
                if (join)
                    e.noteOn(55);
                return e.render(2400);
            };
            const std::vector<float> both = run(true), held = run(false);
            double early = 0.0, later = 0.0;
            for (size_t i = 0; i < both.size() && i < held.size(); ++i) {
                const double joinedVoice = std::abs(static_cast<double>(both[i]) - held[i]);
                if (i < kSampleRate / 1000)
                    early = std::max(early, joinedVoice);
                else if (i >= kSampleRate / 100 && i < kSampleRate / 50)
                    later = std::max(later, joinedVoice);
            }
            std::printf("      %-30s %7.2f of its level in its first ms\n", "poly legato fade-in",
                        later > 0.0 ? early / later : 1.0);
            check(later > 0.0 && early < 0.4 * later, "a note joining a held one in poly legato fades in over 5 ms");
        }
        // A Random LFO in the joined voice carries on with the held voice's step: the newest voice's glide, just after
        // the join, is the one the held voice was on.
        {
            Engine e(2);
            e.synth().setPlayMode(false, true, false);
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
            e.p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Random);
            e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
            e.p[paramIndex(0, SourceParam::Rate)] = normFromRate(0.05); // no new step meanwhile
            e.noteOn(48);
            e.render(4800);
            Telemetry heldLfo, joinedLfo;
            e.synth().telemetry(heldLfo);
            e.noteOn(55);
            e.render(kBlock);
            e.synth().telemetry(joinedLfo);
            check(joinedLfo.lfoFrom[0] == heldLfo.lfoFrom[0] && joinedLfo.lfoTo[0] == heldLfo.lfoTo[0],
                  "a Random LFO in a voice joining a held one in poly legato carries on with the held one's step");
        }

        // Volume jumps 12 dB: rendered in single frames, as when events split the
        // block, it should ramp as smoothly as in whole blocks.
        auto volumeJump = [](uint32_t piece) {
            Engine e(2);
            e.p[paramIndex(GlobalParam::Volume)] = -12.0;
            e.noteOn(48);
            const std::vector<float> before = e.render(4800, piece);
            e.p[paramIndex(GlobalParam::Volume)] = 0.0;
            const std::vector<float> both = joined(before, e.render(2400, piece));
            return roughness(both, before.size() - 2, both.size());
        };
        const double ratio = volumeJump(1) / volumeJump(kBlock);
        std::printf("      %-30s %7.1fx\n", "volume jump, 1-frame blocks", ratio);
        check(ratio < 1.5, "a param jump ramps even when events split the block into single frames");
    }

    effectsChainChecks();

    // --- The mod wheel leaves the FM alone until it is assigned: an FM pair sounds the same with it all the way up.
    {
        auto fmPair = [](double wheel) {
            Engine e(2);
            e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
            e.p[paramIndex(1, OpParam::Out)] = 0.0;
            e.p[matrixIndex(1, 0)] = 0.5;
            e.p[paramIndex(GlobalParam::ModWheel)] = wheel;
            e.noteOn(69);
            return e.render(9600);
        };
        const std::vector<float> resting = fmPair(0.0), up = fmPair(1.0);
        double most = 0.0;
        for (size_t i = 0; i < resting.size(); ++i)
            most = std::max(most, std::abs(static_cast<double>(resting[i]) - up[i]));
        std::printf("mod wheel up, unassigned: largest change %.2g\n", most);
        check(most == 0.0, "the mod wheel no longer turns up the FM by itself");
    }

    // --- Volume comes after the effects: turned down 12 dB, what a hard-clipping shaper puts out comes down 12 dB
    // with it, rather than the shaper clipping just as hard on a quieter input.
    std::printf("volume after the effects:\n");
    {
        auto clipped = [](double volume) {
            Engine e(2);
            e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
            e.p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Hard);
            e.p[paramIndex(0, FxParam::ShaperDrive)] = 24.0;
            e.p[paramIndex(0, FxParam::ShaperLink)] = 0.0;
            e.p[paramIndex(GlobalParam::Volume)] = volume;
            e.noteOn(57);
            const std::vector<float> out = e.render(9600);
            float peak = 0.0f;
            for (size_t i = 4800; i < out.size(); ++i)
                peak = std::max(peak, std::abs(out[i]));
            return peak;
        };
        const float full = clipped(0.0), down = clipped(-12.0);
        std::printf("      clipped sine: peak %.3f at 0 dB, %.3f at -12 dB (x%.3f)\n", full, down, down / full);
        check(std::abs(down / full - 0.2512f) < 0.005f, "Volume sets the level after the effects");
    }

    // --- A mod envelope releases after its key is let go, its voice running on (silently, once its operators have
    // faded) for as long as that takes: half way through a straight 0.5 s release it is at half its level, and what
    // it modulates with it; once the release is over, the voice ends.
    std::printf("mod envelope release:\n");
    {
        Engine e(2);
        const int fm = paramIndex(GlobalParam::FmAmount);
        e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Env);
        e.p[slotTargetIndex(0, 0)] = slotTargetValue(fm);
        e.p[slotAmountIndex(0, 0)] = 0.3;
        e.p[paramIndex(0, SourceParam::Release)] = normFromTime(0.5);
        e.p[paramIndex(0, SourceParam::ReleaseCurve)] = 0.0;
        e.noteOn(60);
        e.render(9600);
        e.noteOff(60);
        e.render(12000); // a quarter of a second
        Telemetry t;
        e.synth().telemetry(t);
        const float level = t.sources[0], offset = t.modOffset[fm];
        const int releasing = t.voices;
        e.render(19200);
        e.synth().telemetry(t);
        std::printf("      a quarter second into a 0.5 s release: level %.3f, FM Amount %+.3f, %d voice(s); %d after it\n",
                    level, offset, releasing, t.voices);
        check(releasing == 1 && std::abs(level - 0.5f) < 0.05f && std::abs(offset - level * 0.3f * 2.0f) < 1e-3f &&
                  t.voices == 0,
              "a mod envelope releases after the note, its voice running on until it is done");
    }

    // --- The output meter: each side's peak, falling 24 dB a second once the sound stops; past full scale (six
    // sines in step, 6 dB down) its clip light comes on and stays on.
    std::printf("output meter:\n");
    {
        auto peakOf = [](const std::vector<float> &x, size_t from) {
            float peak = 0.0f;
            for (size_t i = from; i < x.size(); ++i)
                peak = std::max(peak, std::abs(x[i]));
            return peak;
        };
        Engine e(2);
        e.p[paramIndex(GlobalParam::Volume)] = -18.0;
        e.noteOn(69);
        const float peak = peakOf(e.render(9600), 4800);
        Telemetry t;
        e.synth().telemetry(t);
        const float held = t.outPeak[0];
        const bool dark = t.clipHold == 0.0f;
        e.noteOff(69);
        e.render(24000); // half a second
        e.synth().telemetry(t);
        const double fell = 20.0 * std::log10(held / t.outPeak[0]);
        Engine loud(2);
        loud.p[paramIndex(GlobalParam::Volume)] = -6.0;
        for (int op = 1; op < kOps; ++op) {
            loud.p[paramIndex(op, OpParam::Enabled)] = 1.0;
            loud.p[paramIndex(op, OpParam::Out)] = 1.0;
        }
        loud.noteOn(69);
        const float over = peakOf(loud.render(4800), 0);
        loud.synth().telemetry(t);
        std::printf("      A4 sine: peak %.3f, metered %.3f, %.1f dB lower half a second after it ends; "
                    "six of them 6 dB down: peak %.2f, clip light on for %.2f s more\n",
                    peak, held, fell, over, t.clipHold);
        check(std::abs(held / peak - 1.0f) < 0.02f && dark && fell > 10.0 && fell < 13.0 && over > 1.0f &&
                  t.clipHold > 1.0f,
              "the output meter follows the peak, falls 24 dB a second, and lights up past full scale");
    }

    noteAndVoiceChecks();

    // --- What oversampling costs: 16 voices of a 6-operator stack, one second.
    std::printf("cost (16 voices, 6 operators, ms per second of audio):\n");
    for (int factor : factors) {
        Engine e(factor);
        for (int op = 1; op < kOps; ++op) {
            e.p[paramIndex(op, OpParam::Enabled)] = 1.0;
            e.p[matrixIndex(op, op - 1)] = 0.3;
        }
        for (int v = 0; v < 16; ++v)
            e.noteOn(40 + v);
        const auto start = std::chrono::steady_clock::now();
        const std::vector<float> out = e.render(static_cast<size_t>(kSampleRate));
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("      %dx %8.1f ms\n", factor, ms);
        check(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }),
                "full load stays finite");
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
