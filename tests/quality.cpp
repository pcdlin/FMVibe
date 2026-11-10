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

// The effects in `slots` one after another from the input to the output: a chain, as the effects graph wires it.
void chain(ParamValues &p, const std::vector<int> &slots) {
    for (const auto &[index, value] : fxChainWires(slots))
        p[index] = value;
}

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

// --- The amp: every model about as loud as what goes in; its stages saturating harder the more Gain they get, Heavy
// harder than Clean; aliasing that falls as the oversampling rises; its tone controls, cabinet and mic moving the
// spectrum as they should; one amp for both sides in mono, one each in stereo; Mix at 0 leaving the sound alone; no DC
// out of its lopsided stages; and no click when its setting changes.
void ampChecks() {
    std::printf("amp:\n");
    // An amp in slot 0, alone in the chain, its fields set by `setup`, fed `in` on both sides (or the left only) in
    // blocks of `piece`; returns the left side, or the right with `right`.
    struct Run {
        std::vector<float> left, right;
    };
    auto run = [](const std::vector<float> &in, const std::map<FxParam, double> &fields, bool leftOnly = false,
                  uint32_t piece = kBlock) {
        auto fx = std::make_unique<Effects>(); // its slots are large: not on the stack
        fx->prepare(kSampleRate);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Amp);
        chain(p, {0});
        for (const auto &[field, value] : fields)
            p[paramIndex(0, field)] = value;
        Run out{in, leftOnly ? std::vector<float>(in.size(), 0.0f) : in};
        for (size_t done = 0; done < in.size(); done += piece) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(piece, in.size() - done));
            fx->process(out.left.data() + done, out.right.data() + done, n, p, 120.0, 60.0);
        }
        return out;
    };
    auto sine = [](double hz, double level, size_t frames) {
        std::vector<float> x(frames);
        for (size_t i = 0; i < frames; ++i)
            x[i] = static_cast<float>(level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate));
        return x;
    };
    auto rms = [](const std::vector<float> &x, size_t from) {
        double sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sum += static_cast<double>(x[i]) * x[i];
        return std::sqrt(sum / static_cast<double>(x.size() - from));
    };
    auto db = [](double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); };
    constexpr int kModels = static_cast<int>(AmpModel::Count);
    const char *const names[kModels] = {"Clean", "Blues", "Crunch", "Lead", "Heavy", "Bass", "Fuzz"};

    // Levels: a sine at -12 dBFS (as an operator into the effects) at the default knobs, cabinet on, comes out within
    // 2 dB of its level, every model; and the energy off the note's fundamental grows with Gain.
    {
        const std::vector<float> in = sine(220.0, 0.25, kFftSize + 9600);
        double worst = 0.0;
        bool saturating = true;
        std::vector<double> distortion(kModels);
        for (int m = 0; m < kModels; ++m) {
            const double level = db(rms(run(in, {{FxParam::AmpModel, m}}).left, 9600) / rms(in, 9600));
            auto offFundamental = [&](double gain) { // what isn't the note's fundamental, against it
                const std::vector<float> out = run(in, {{FxParam::AmpModel, m}, {FxParam::AmpGain, gain}}).left;
                return unexpectedDb(std::vector<float>(out.end() - kFftSize, out.end()), {220.0});
            };
            const double low = offFundamental(0.1), high = offFundamental(0.9);
            distortion[m] = high;
            std::printf("      %-6s level %+5.1f dB, harmonics %+6.1f dB at Gain 1, %+6.1f dB at Gain 9\n", names[m],
                        level, low, high);
            worst = std::max(worst, std::abs(level));
            saturating = saturating && high > low + 3.0;
        }
        check(worst < 2.0, "every amp model at its default knobs comes out about as loud as what goes in");
        check(saturating && distortion[static_cast<int>(AmpModel::Heavy)] > distortion[static_cast<int>(AmpModel::Clean)],
              "the amp's stages saturate more the more Gain they get, Heavy more than Clean");
    }
    // Aliasing: Heavy at full Gain and Volume on a C7, cabinet off, the energy that isn't a harmonic of the note, at
    // each oversampling: falling as it rises, and 35 dB down at the 4x it starts with. And its cost.
    {
        const double c7 = keyHz(96);
        const std::vector<float> in = sine(c7, 0.25, kFftSize + 4800);
        double previous = 0.0, at4 = 0.0;
        bool falling = true;
        for (int os = 0; os < kAmpOversamplings; ++os) {
            const auto start = std::chrono::steady_clock::now();
            const std::vector<float> out = run(in, {{FxParam::AmpModel, static_cast<double>(AmpModel::Heavy)},
                                                    {FxParam::AmpGain, 1.0},
                                                    {FxParam::AmpVolume, 1.0},
                                                    {FxParam::AmpCabOn, 0.0},
                                                    {FxParam::AmpOversampling, os}})
                                               .left;
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double alias = unexpectedDb(std::vector<float>(out.end() - kFftSize, out.end()), harmonics(c7));
            std::printf("      %dx: aliasing %6.1f dB, %.2f %% of a core\n", 1 << os, alias,
                        100.0 * seconds / (static_cast<double>(in.size()) / kSampleRate));
            if (os > 0)
                falling = falling && alias < previous;
            previous = alias;
            if (os == kDefaultAmpOversampling)
                at4 = alias;
        }
        check(falling && at4 < -35.0, "the amp's aliasing falls as its oversampling rises, 35 dB down at 4x");
    }
    // The tone: Treble up brightens and Bass up darkens (the energy at 3 kHz against 50-130 Hz of a quiet, bright input:
    // white noise into Clean at Gain 0, nearly linear); the cabinet takes the top away (above 8 kHz, 20 dB at least);
    // the mic toward the cone's edge is darker than at its center.
    {
        std::vector<float> noise(48000);
        uint32_t seed = 12345;
        for (float &v : noise) {
            seed = seed * 1664525u + 1013904223u;
            v = 0.02f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
        }
        auto band = [&](const std::map<FxParam, double> &fields, double from, double to) {
            std::map<FxParam, double> all = {{FxParam::AmpModel, static_cast<double>(AmpModel::Clean)},
                                             {FxParam::AmpGain, 0.0}};
            for (const auto &[f, v] : fields)
                all[f] = v;
            const std::vector<float> out = run(noise, all).left;
            std::vector<float> padded(kFftSize, 0.0f);
            std::copy(out.end() - 32768, out.end(), padded.begin());
            const std::vector<double> power = powerSpectrum(padded);
            const double binHz = kSampleRate / kFftSize;
            double sum = 1e-30;
            for (size_t b = static_cast<size_t>(from / binHz); b < static_cast<size_t>(to / binHz); ++b)
                sum += power[b];
            return 10.0 * std::log10(sum);
        };
        auto tilt = [&](const std::map<FxParam, double> &fields) {
            return band(fields, 2500.0, 3500.0) - band(fields, 50.0, 130.0);
        };
        const double middle = tilt({});
        const double treble = tilt({{FxParam::AmpTreble, 1.0}}) - tilt({{FxParam::AmpTreble, 0.0}});
        const double bass = tilt({{FxParam::AmpBass, 1.0}}) - tilt({{FxParam::AmpBass, 0.0}});
        const double cabTop = band({}, 8000.0, 16000.0) - band({}, 500.0, 1500.0);
        const double openTop =
            band({{FxParam::AmpCabOn, 0.0}}, 8000.0, 16000.0) - band({{FxParam::AmpCabOn, 0.0}}, 500.0, 1500.0);
        const double center = band({{FxParam::AmpPosition, 0.0}}, 3000.0, 6000.0),
                     edge = band({{FxParam::AmpPosition, 1.0}}, 3000.0, 6000.0);
        std::printf("      3 kHz against 50-130 Hz: %+.1f dB, Treble 10 against 0 %+.1f, Bass 10 against 0 %+.1f; the "
                    "cabinet's top %+.1f dB against none; the mic at the edge %+.1f dB at 3-6 kHz\n",
                    middle, treble, bass, cabTop - openTop, edge - center);
        check(treble > 6.0 && bass < -3.0, "the amp's Treble and Bass tilt its tone the right way");
        check(cabTop < openTop - 20.0 && edge < center - 2.0,
              "the amp's cabinet takes the top away, and its mic at the cone's edge is darker than at its center");
    }
    // Mono and stereo: a sound on the left only comes out on both sides with Stereo off, and on the left alone with it
    // on. Mix at 0 leaves the sound as it was.
    {
        const std::vector<float> in = sine(220.0, 0.25, 9600);
        const Run mono = run(in, {}, true), stereo = run(in, {{FxParam::AmpStereo, 1.0}}, true);
        const Run dry = run(in, {{FxParam::AmpMix, 0.0}});
        double gap = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
            gap = std::max(gap, static_cast<double>(std::abs(dry.left[i] - in[i])));
        // (After the 5 ms the effect fades in over as it starts, while what went in still shows through.)
        const bool same = std::equal(mono.left.begin() + 4800, mono.left.end(), mono.right.begin() + 4800);
        check(same && rms(mono.right, 4800) > 0.01 && rms(stereo.right, 0) == 0.0 &&
                  rms(stereo.left, 4800) > 0.01,
              "the amp runs once on both sides' sum in mono, and once a side in stereo");
        check(gap < 1e-6, "the amp's Mix at 0 leaves the sound alone");
    }
    // DC: the Fuzz's lopsided stages at full Gain put out none (the mean of a second's output).
    {
        const std::vector<float> in = sine(110.0, 0.25, 96000);
        const std::vector<float> out =
            run(in, {{FxParam::AmpModel, static_cast<double>(AmpModel::Fuzz)}, {FxParam::AmpGain, 1.0}}).left;
        double mean = 0.0;
        for (size_t i = 48000; i < out.size(); ++i)
            mean += out[i];
        mean /= 48000.0;
        std::printf("      the fuzz's DC: %.2e\n", mean);
        check(std::abs(mean) < 1e-3, "the amp's lopsided stages put out no DC");
    }
    // The room: with the mic a metre back, an impulse comes back about 7.7 ms later (its latest reflection), which a
    // mic up close doesn't hear.
    {
        std::vector<float> impulse(4800, 0.0f);
        impulse[100] = 0.05f;
        auto echo = [&](double distance) {
            const std::vector<float> out = run(impulse, {{FxParam::AmpModel, static_cast<double>(AmpModel::Clean)},
                                                         {FxParam::AmpGain, 0.0},
                                                         {FxParam::AmpDistance, distance}})
                                               .left;
            const size_t lag = 100 + static_cast<size_t>(std::lround(0.0077 * kSampleRate));
            return rms(std::vector<float>(out.begin() + static_cast<long>(lag) - 4, out.begin() + static_cast<long>(lag) + 20), 0) /
                   rms(std::vector<float>(out.begin() + static_cast<long>(lag) - 40, out.begin() + static_cast<long>(lag) - 16), 0);
        };
        const double far = echo(1.0), near = echo(0.0);
        std::printf("      the room's last reflection: %.1fx the tail before it a metre back, %.1fx up close\n", far, near);
        check(far > 1.6 && near < 1.3, "the amp's mic set back hears the room's reflections");
    }
    // Clicks: the model, the cabinet, the mic, the cabinet's switch and the oversampling changed under a C3 played in
    // single frames: each crossfades, no rougher than the note around it.
    {
        struct Change {
            const char *name;
            FxParam field;
            double to;
        };
        const Change changes[] = {
            {"model to Heavy", FxParam::AmpModel, static_cast<double>(AmpModel::Heavy)},
            {"cabinet to 1x12", FxParam::AmpCab, static_cast<double>(AmpCab::Open1x12)},
            {"mic to ribbon", FxParam::AmpMic, static_cast<double>(AmpMic::Ribbon)},
            {"cabinet off", FxParam::AmpCabOn, 0.0},
            {"oversampling to 1x", FxParam::AmpOversampling, 0.0},
        };
        double worst = 0.0;
        for (const Change &change : changes) {
            Engine e(2);
            e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Amp);
            chain(e.p, {0});
            e.noteOn(48);
            const std::vector<float> before = e.render(9600, 1);
            e.p[paramIndex(0, change.field)] = change.to;
            const std::vector<float> all = joined(before, e.render(2400, 1));
            const double steady = roughness(all, 4800, before.size());
            const double switched = roughness(all, before.size(), all.size());
            std::printf("      %-20s %.2fx as rough as the note\n", change.name, switched / steady);
            worst = std::max(worst, switched / steady);
        }
        check(worst < 2.0, "the amp's setting changed crossfades rather than clicking");
    }
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
        chain(p, {0});
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
        // A delay switched off and on again plays back nothing it held: after half a second of a sine, its lines part
        // written, and after five, past their length, written all round.
        bool clean = true;
        for (double played : {0.5, 5.0}) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
            p[paramIndex(0, FxParam::DelaySync)] = 0.0;
            p[paramIndex(0, FxParam::DelayTimeL)] = p[paramIndex(0, FxParam::DelayTimeR)] = normFromTime(1.0);
            p[paramIndex(0, FxParam::DelayFeedback)] = 0.9;
            p[paramIndex(0, FxParam::DelayMix)] = 1.0;
            chain(p, {0});
            constexpr uint32_t kPiece = 32;
            float left[kPiece], right[kPiece];
            size_t t = 0;
            auto run = [&](double seconds, double amplitude) {
                float peak = 0.0f;
                for (const size_t end = t + static_cast<size_t>(seconds * kSampleRate); t < end; t += kPiece) {
                    for (uint32_t s = 0; s < kPiece; ++s)
                        left[s] = right[s] = static_cast<float>(
                            amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                    fx.process(left, right, kPiece, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < kPiece; ++s)
                        peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
                }
                return peak;
            };
            run(played, 0.5);
            p[paramIndex(0, FxParam::Enabled)] = 0.0;
            run(0.05, 0.0); // faded out
            p[paramIndex(0, FxParam::Enabled)] = 1.0;
            const float after = run(2.0, 0.0);
            std::printf("      a delay off and on again after %.1f s of a sine: %.0f dB of it after\n", played,
                        20.0 * std::log10(after + 1e-30));
            clean = clean && after < 1e-6f;
        }
        check(clean, "a delay switched off and on again plays back nothing it held");
    }
    {
        // A sample that isn't a number going into a delay and a reverb, and one louder than any sound (which would
        // ring on in them for good, or overflow): what comes out stays finite and sane, the tails ring out to silence,
        // and a sine after them comes out as it should.
        bool fine = true;
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), 3e38f}) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
            p[paramIndex(0, FxParam::DelayFeedback)] = 0.5;
            p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Reverb);
            chain(p, {0, 1});
            constexpr uint32_t kPiece = 32;
            float left[kPiece], right[kPiece];
            size_t t = 0;
            bool finite = true;
            auto run = [&](size_t pieces, double amplitude, float spike) {
                float peak = 0.0f;
                for (size_t b = 0; b < pieces; ++b, t += kPiece) {
                    for (uint32_t s = 0; s < kPiece; ++s)
                        left[s] = right[s] = static_cast<float>(
                            amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                    if (b == 0 && spike != 0.0f)
                        left[5] = spike;
                    fx.process(left, right, kPiece, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < kPiece; ++s) {
                        finite = finite && std::abs(left[s]) < 100.0f && std::abs(right[s]) < 100.0f;
                        peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
                    }
                }
                return peak;
            };
            run(300, 0.5, 0.0f);
            run(1500, 0.0, bad); // a second
            for (int k = 0; k < 60 && fx.ringing(); ++k)
                run(1500, 0.0, 0.0f);
            const bool rungOut = !fx.ringing();
            const float after = run(300, 0.5, 0.0f);
            std::printf("      %s into a delay and a reverb: finite %d, rung out %d, a sine after it %.2f\n",
                        std::isnan(bad) ? "a NaN" : "3e38", finite, rungOut, after);
            fine = fine && finite && rungOut && after > 0.1f && after < 2.0f;
        }
        check(fine, "a sample that isn't a number or is louder than any sound leaves the effects sane, playing on");
    }
    {
        // A loop, which only params make: the input into a DJ EQ, which feeds a shaper that feeds it back, at half
        // level, and goes out. The shaper's effect taken away (as a host or a state can, leaving its wires): once it
        // has faded out, the output plays the EQ on, rather than ducking out for good.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Eq);
        p[fxInputIndex(1, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
        p[fxInputIndex(1, 1, FxInputField::From)] = fxPortValue(fxSlotPort(0));
        p[fxInputIndex(1, 1, FxInputField::Level)] = 0.5;
        p[fxInputIndex(0, 0, FxInputField::From)] = fxPortValue(fxSlotPort(1));
        p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(fxSlotPort(1));
        std::vector<float> left(kBlock), right(kBlock), out;
        size_t t = 0;
        auto run = [&](double seconds) {
            out.clear();
            for (const size_t end = t + static_cast<size_t>(seconds * kSampleRate); t < end; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.25 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                out.insert(out.end(), left.begin(), left.end());
            }
            // Each period's peak, the quietest and the loudest of them.
            const size_t period = static_cast<size_t>(kSampleRate / 440.0) + 1;
            float low = 1e9f, high = 0.0f;
            for (size_t from = 0; from + period <= out.size(); from += period) {
                float peak = 0.0f;
                for (size_t i = from; i < from + period; ++i)
                    peak = std::max(peak, std::abs(out[i]));
                low = std::min(low, peak);
                high = std::max(high, peak);
            }
            return std::pair(low, high);
        };
        const auto looped = run(0.5);
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::None);
        run(0.5);
        const auto [low, high] = run(0.5);
        std::printf("      a loop through an effect taken away: %.3f with it, %.3f to %.3f once it has gone\n",
                    looped.second, low, high);
        check(looped.second > 0.1f && std::abs(low - 0.25f) < 0.02f && std::abs(high - 0.25f) < 0.02f,
              "an effect taken out of a loop the params make leaves the output playing what is left");
    }
    {
        // A compressor pulling a loud sine down, then silence: it keeps going while its release runs out, rather than
        // stopping where it was, and a quiet sine after it comes out as it goes in.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Compressor);
        chain(p, {0});
        constexpr uint32_t kPiece = 32;
        float left[kPiece], right[kPiece];
        size_t t = 0;
        auto run = [&](double seconds, double amplitude, float *peak) {
            for (const size_t end = t + static_cast<size_t>(seconds * kSampleRate); t < end; t += kPiece) {
                for (uint32_t s = 0; s < kPiece; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left, right, kPiece, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kPiece && peak; ++s)
                    *peak = std::max(*peak, std::abs(left[s]));
            }
        };
        run(0.5, 0.5, nullptr); // -6 dB, some 9 dB over the threshold
        run(0.2, 0.0, nullptr);
        const bool releasing = fx.ringing();
        run(4.0, 0.0, nullptr);
        const bool released = !fx.ringing();
        float quiet = 0.0f;
        run(0.01, 0.03, &quiet); // -30 dB, under the threshold
        std::printf("      a compressor let go: ringing 0.2 s on %d, after 4 s %d; a quiet sine after it %.2f dB off\n",
                    releasing, !released, 20.0 * std::log10(quiet / 0.03));
        check(releasing && released && std::abs(20.0 * std::log10(quiet / 0.03)) < 0.1,
              "a compressor runs its release out in silence, and a quiet sound after it comes through untouched");
    }
    {
        // A resonant comb as long as its line (its cutoff below 20 Hz, key tracking on a low note) struck once: it
        // rings on between its pulses, however they fall in the pieces, and once it has rung out, a quiet sound after
        // it brings none of its ring back.
        for (int at : {0, 31}) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Filter);
            p[paramIndex(0, FxParam::FilterType)] = static_cast<double>(FilterType::Comb);
            p[paramIndex(0, FxParam::FilterCutoff)] = 0.0;
            p[paramIndex(0, FxParam::FilterResonance)] = 1.0;
            p[paramIndex(0, FxParam::FilterKeyTrack)] = 1.0;
            p[paramIndex(0, FxParam::FilterMix)] = 1.0;
            chain(p, {0});
            constexpr uint32_t kPiece = 32;
            float left[kPiece], right[kPiece];
            auto piece = [&](float level, size_t t, int impulse) {
                for (uint32_t s = 0; s < kPiece; ++s)
                    left[s] = right[s] = t + s == static_cast<size_t>(impulse) ? 0.5f : level;
                fx.process(left, right, kPiece, p, 120.0, 36.0); // C2
            };
            for (int k = 0; k < 100; ++k)
                piece(0.0f, 0, -1);
            int pulses = 0;
            for (size_t t = 0; t < static_cast<size_t>(20.0 * kSampleRate); t += kPiece) {
                piece(0.0f, t, at);
                for (uint32_t s = 0; s < kPiece; ++s)
                    pulses += std::abs(left[s]) > 0.01f ? 1 : 0;
            }
            const bool rungOut = !fx.ringing();
            float ghost = 0.0f;
            for (size_t t = 0; t < static_cast<size_t>(0.2 * kSampleRate); t += kPiece) {
                piece(1e-7f, t, -1);
                for (uint32_t s = 0; s < kPiece; ++s)
                    ghost = std::max(ghost, std::abs(left[s]));
            }
            std::printf("      a comb as long as its line struck at %d: %d pulses heard, rung out %d, %.1e after it\n", at,
                        pulses, rungOut, ghost);
            check(pulses > 20 && rungOut && ghost < 1e-4f,
                  "a comb as long as its line rings on between its pulses, and brings nothing back once rung out");
        }
    }
    {
        // Effects with nothing to do: once every tail has rung out, silence costs a small share of what a sound
        // through them does, and comes out as silence; a sound after it comes through again.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Delay);
        p[paramIndex(2, FxParam::Type)] = static_cast<double>(FxType::Phaser);
        chain(p, {0, 1, 2});
        std::vector<float> left(kBlock), right(kBlock);
        auto run = [&](double seconds, double amplitude) {
            float peak = 0.0f;
            for (size_t t = 0, end = static_cast<size_t>(seconds * kSampleRate); t < end; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock; ++s)
                    peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
            }
            return peak;
        };
        auto timed = [&](double seconds, double amplitude, float &peak) { // the best of three
            double best = 1e9;
            for (int k = 0; k < 3; ++k) {
                const auto start = std::chrono::steady_clock::now();
                peak = std::max(peak, run(seconds, amplitude));
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
            return best;
        };
        run(0.5, 0.5);
        int waited = 0;
        while (fx.ringing() && waited++ < 120) // up to a minute for the tails
            run(0.5, 0.0);
        float busyPeak = 0.0f, idlePeak = 0.0f, after = 0.0f;
        const double busy = timed(2.0, 0.01, busyPeak);
        while (fx.ringing() && waited++ < 240)
            run(0.5, 0.0);
        const double idle = timed(2.0, 0.0, idlePeak);
        after = run(0.1, 0.5);
        std::printf("      reverb, delay and phaser: 2 s of a quiet sine %.2f ms, of silence once rung out %.3f ms; after "
                    "it %.2f\n",
                    busy * 1e3, idle * 1e3, after);
        check(!fx.ringing() || waited < 240, "the effects' tails ring out");
        check(idle < 0.25 * busy && idlePeak == 0.0f && busyPeak > 0.0f && after > 0.1f,
              "silence through effects that have rung out costs little and comes out as silence");
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
            {"amp output +12 dB", FxType::Amp, FxParam::AmpOutput, 0.0, 12.0},
            {"amp mix 0 to 100 %", FxType::Amp, FxParam::AmpMix, 0.0, 1.0},
            {"amp gain 5 to 10", FxType::Amp, FxParam::AmpGain, 0.5, 1.0},
        };
        double worst = 0.0;
        for (const Jump &jump : jumps) {
            auto roughJump = [&jump](uint32_t piece) {
                Engine e(2);
                e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(jump.type);
                chain(e.p, {0});
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
        // So does a wire's level: the output's wire from the input taken from full to a quarter.
        auto roughJump = [](uint32_t piece) {
            Engine e(2);
            e.p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
            e.noteOn(48);
            const std::vector<float> before = e.render(4800, piece);
            e.p[fxInputIndex(kFxOutNode, 0, FxInputField::Level)] = 0.25;
            const std::vector<float> both = joined(before, e.render(2400, piece));
            return roughness(both, before.size() - 2, both.size());
        };
        const double ratio = roughJump(1) / roughJump(kBlock);
        std::printf("      %-30s %7.1fx in 1-frame blocks\n", "a wire's level to a quarter", ratio);
        check(ratio < 1.5, "a wire's level jump ramps even when events split the block into single frames");
    }
    {
        // A parametric EQ band switched on and off fades rather than clicking: a low cut at 5 kHz, which takes most
        // of a C3 away, switched in single frames, under 4 times as rough as the note before it (as a stolen voice).
        Engine e(2);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
        chain(e.p, {0});
        e.p[peqTypeIndex(0, 2)] = static_cast<double>(PeqType::LowCut);
        e.p[peqFreqIndex(0, 2)] = normFromCutoff(5000.0);
        e.p[peqOnIndex(0, 2)] = 0.0;
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
        // A wire of the effects graph connected and taken away fades rather than clicking: a second wire from the
        // input to the output, doubling a C3, then gone again, in single frames, under 4 times as rough as the note.
        Engine e(2);
        e.p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
        e.noteOn(48);
        const std::vector<float> before = e.render(4800, 1);
        e.p[fxInputIndex(kFxOutNode, 1, FxInputField::From)] = fxPortValue(kFxPortIn);
        const std::vector<float> on = e.render(2400, 1);
        e.p[fxInputIndex(kFxOutNode, 1, FxInputField::From)] = 0.0;
        const std::vector<float> all = joined(joined(before, on), e.render(2400, 1));
        const double steady = roughness(all, 2400, before.size());
        const size_t gone = before.size() + on.size();
        const double switched =
            std::max(roughness(all, before.size() - 2, before.size() + 480), roughness(all, gone - 2, gone + 480));
        double doubled = 0.0, single = 0.0;
        for (size_t i = before.size() + 480; i < gone; ++i)
            doubled = std::max(doubled, static_cast<double>(std::abs(all[i])));
        for (size_t i = 2400; i < before.size(); ++i)
            single = std::max(single, static_cast<double>(std::abs(all[i])));
        std::printf("      a wire connected and taken away: %.2fx as rough as the note, doubling it x%.3f\n",
                    switched / steady, doubled / single);
        check(switched < 4.0 * steady && std::abs(doubled / single - 2.0) < 0.05,
              "a wire of the effects graph connected and taken away fades in and out without a click");
    }
    {
        // Editing a graph while a C3 plays: neither a click (under 4 times as rough as the note) nor a dip (each
        // period's peak at least 90 % of the note's, where a wire fading out before the next fades in would leave a
        // gap). Each edit is what the editor writes: wires moved, nodes coming and going.
        auto from = [](int node, int input) { return fxInputIndex(node, input, FxInputField::From); };
        auto port = [](int p) { return fxPortValue(p); };
        // On a C3, or `note` (whose shorter period shows a dip as short as a 5 ms crossfade).
        auto steadyEdit = [&](const char *what, auto &&before, auto &&edit, int note = 48) {
            const size_t period = static_cast<size_t>(kSampleRate / (440.0 * std::pow(2.0, (note - 69) / 12.0))) + 1;
            Engine e(2);
            before(e.p);
            e.noteOn(note);
            const std::vector<float> settled = e.render(9600, 1);
            edit(e.p);
            const std::vector<float> all = joined(settled, e.render(2400, 1));
            const double steady = roughness(all, 4800, settled.size());
            const double switched = roughness(all, settled.size() - 2, settled.size() + 960);
            double peak = 0.0, least = 1e9;
            for (size_t i = 4800; i < settled.size(); ++i)
                peak = std::max(peak, static_cast<double>(std::abs(all[i])));
            for (size_t at = settled.size() - period; at + period < all.size(); at += period / 4) {
                double most = 0.0;
                for (size_t i = at; i < at + period; ++i)
                    most = std::max(most, static_cast<double>(std::abs(all[i])));
                least = std::min(least, most);
            }
            std::printf("      %-48s %.2fx as rough, its least period at %.3f\n", what, switched / steady,
                        least / peak);
            return switched < 4.0 * steady && least > 0.9 * peak;
        };
        // Parametric EQs as they start (every band at 0 dB, passing the note as it is) in slots 0 and 1, as a chain.
        auto eqs = [](ParamValues &p) {
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
            p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
        };
        auto line = [&](ParamValues &p) { // IN, the first EQ, OUT
            eqs(p);
            p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::None);
            p[from(0, 0)] = port(kFxPortIn);
            p[from(kFxOutNode, 0)] = port(fxSlotPort(0));
        };
        const bool added = steadyEdit("an effect added at the line's end", line, [&](ParamValues &p) {
            p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
            p[from(1, 0)] = port(fxSlotPort(0));
            p[from(kFxOutNode, 0)] = port(fxSlotPort(1));
        });
        const bool removed = steadyEdit("an effect taken out of the line", line, [&](ParamValues &p) {
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::None);
            p[from(0, 0)] = 0.0;
            p[from(kFxOutNode, 0)] = port(kFxPortIn);
        });
        // A split at 2 kHz, the first EQ on its high band and its low band empty, into its join and out.
        auto split = [&](ParamValues &p) {
            line(p);
            p[paramIndex(0, SplitParam::Used)] = 1.0;
            p[paramIndex(0, SplitParam::Low)] = normFromCutoff(2000.0);
            p[paramIndex(0, SumParam::Used)] = 1.0;
            p[from(fxSplitNode(0), 0)] = port(kFxPortIn);
            p[from(0, 0)] = port(fxBandPort(0, kFxBands - 1));
            p[from(fxSumNode(0), 0)] = port(fxSlotPort(0));
            p[from(fxSumNode(0), 1)] = port(fxBandPort(0, 0));
            p[from(kFxOutNode, 0)] = port(fxSumPort(0));
        };
        const bool unsplit = steadyEdit("a split and its join taken out", split, [&](ParamValues &p) {
            p[paramIndex(0, SplitParam::Used)] = 0.0;
            p[paramIndex(0, SumParam::Used)] = 0.0;
            p[from(fxSplitNode(0), 0)] = 0.0;
            p[from(fxSumNode(0), 0)] = 0.0;
            p[from(fxSumNode(0), 1)] = 0.0;
            p[from(0, 0)] = port(kFxPortIn);
            p[from(kFxOutNode, 0)] = port(fxSlotPort(0));
        });
        const bool sloped = steadyEdit("a split's slope from 24 to 6 dB", split, [&](ParamValues &p) {
            p[paramIndex(0, SplitParam::Slope)] = 0.0;
        });
        // A split at 60 Hz into its join, an A4 in its high band; split in three, as the editor's 2 | 3 switch writes
        // it (High at 2 kHz, the middle band into the join's third input), the A4 is in the middle band; and back. The
        // middle band fades in or out with its wire alone, not again with the split's crossfade.
        auto twoBands = [&](ParamValues &p, double slope) {
            p[paramIndex(0, SplitParam::Used)] = 1.0;
            p[paramIndex(0, SplitParam::Low)] = normFromCutoff(60.0);
            p[paramIndex(0, SplitParam::Slope)] = slope;
            p[paramIndex(0, SumParam::Used)] = 1.0;
            p[from(fxSplitNode(0), 0)] = port(kFxPortIn);
            p[from(fxSumNode(0), 0)] = port(fxBandPort(0, kFxBands - 1));
            p[from(fxSumNode(0), 1)] = port(fxBandPort(0, 0));
            p[from(kFxOutNode, 0)] = port(fxSumPort(0));
        };
        auto toThree = [&](ParamValues &p) {
            p[paramIndex(0, SplitParam::Bands)] = 3.0;
            p[paramIndex(0, SplitParam::High)] = normFromCutoff(2000.0);
            p[from(fxSumNode(0), 2)] = port(fxBandPort(0, 1));
        };
        auto toTwo = [&](ParamValues &p) {
            p[paramIndex(0, SplitParam::Bands)] = 2.0;
            p[from(fxSumNode(0), 2)] = 0.0;
        };
        bool banded = true;
        for (const double slope : {1.0, static_cast<double>(kFilterSlopes - 1)}) {
            const bool twelve = slope < 2.0;
            banded = steadyEdit(twelve ? "a split from two bands to three, at 12 dB" : "a split from two bands to three, at 24 dB",
                                [&](ParamValues &p) { twoBands(p, slope); }, toThree, 69) &&
                     steadyEdit(twelve ? "a split from three bands to two, at 12 dB" : "a split from three bands to two, at 24 dB",
                                [&](ParamValues &p) {
                                    twoBands(p, slope);
                                    toThree(p);
                                },
                                toTwo, 69) &&
                     banded;
        }
        check(added && removed && unsplit && sloped && banded,
              "editing the effects graph as a note plays neither clicks nor drops out");

        // The two EQs one after the other, then the other way round, as a card dragged along the row writes it: the
        // output ducks out and back in around the move (its wires crossfading would loop) rather than clicking.
        Engine e(2);
        eqs(e.p);
        chain(e.p, {0, 1});
        e.noteOn(48);
        const std::vector<float> settled = e.render(9600, 1);
        chain(e.p, {1, 0});
        const std::vector<float> all = joined(settled, e.render(2400, 1));
        const double steady = roughness(all, 4800, settled.size());
        const double switched = roughness(all, settled.size() - 2, settled.size() + 960);
        double back = 0.0, peak = 0.0;
        for (size_t i = 4800; i < settled.size(); ++i)
            peak = std::max(peak, static_cast<double>(std::abs(all[i])));
        for (size_t i = settled.size() + 960; i < all.size(); ++i)
            back = std::max(back, static_cast<double>(std::abs(all[i])));
        std::printf("      two effects swapped along the line: %.2fx as rough as the note, back at %.3f\n",
                    switched / steady, back / peak);
        check(switched < 4.0 * steady && back > 0.95 * peak,
              "two effects swapped along the line duck out and back in without a click");
    }
    {
        // An effect the output doesn't hear doesn't run: a delay fed from IN with nothing reading it neither lights its
        // port nor keeps the effects ringing on after its echoes would; read by OUT, it runs.
        Effects fx;
        fx.prepare(kSampleRate);
        fx.setAnalyzing(true); // as while the editor is open, which reads the ports
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
        p[fxInputIndex(0, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
        p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
        std::vector<float> left(4800), right(4800);
        auto sine = [&]() {
            for (int i = 0; i < 4800; ++i)
                left[i] = right[i] =
                    static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kSampleRate));
        };
        auto silence = [&](int blocks) {
            for (int k = 0; k < blocks; ++k) {
                std::fill(left.begin(), left.end(), 0.0f);
                std::fill(right.begin(), right.end(), 0.0f);
                fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
            }
        };
        sine();
        fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
        const float unheard = fx.portLevel(fxSlotPort(0));
        silence(2);
        const bool rings = fx.ringing();
        p[fxInputIndex(kFxOutNode, 1, FxInputField::From)] = fxPortValue(fxSlotPort(0));
        sine();
        fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
        const float heard = fx.portLevel(fxSlotPort(0));
        silence(2);
        std::printf("      a delay nothing reads: its port %.3f, ringing %d; read by OUT: %.3f, ringing %d\n", unheard,
                    rings, heard, fx.ringing());
        check(unheard == 0.0f && !rings && heard > 0.1f && fx.ringing(),
              "an effect in the graph runs only while the output hears it");
    }
    {
        // The effects graph's ports, as the editor lights its nodes and wires by them: a sine at half scale from IN
        // through a DJ EQ at its middle to OUT reads half scale at IN and out of the EQ, an effect not in the graph 0;
        // a second of silence after, every port has fallen about 90 dB. With nothing analyzing (no editor open), the
        // ports aren't measured and read nothing.
        Effects fx;
        fx.prepare(kSampleRate);
        fx.setAnalyzing(true);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Eq);
        p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Delay);
        p[fxInputIndex(0, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
        p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(fxSlotPort(0));
        std::vector<float> left(4800), right(4800);
        for (int i = 0; i < 4800; ++i)
            left[i] = right[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kSampleRate));
        fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
        const float in = fx.portLevel(kFxPortIn), eq = fx.portLevel(fxSlotPort(0)), apart = fx.portLevel(fxSlotPort(1));
        for (int k = 0; k < 10; ++k) {
            std::fill(left.begin(), left.end(), 0.0f);
            std::fill(right.begin(), right.end(), 0.0f);
            fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
        }
        const float fallen = fx.portLevel(fxSlotPort(0));
        fx.setAnalyzing(false);
        for (int i = 0; i < 4800; ++i)
            left[i] = right[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kSampleRate));
        fx.process(left.data(), right.data(), 4800, p, 120.0, 60.0);
        const float unmeasured = fx.portLevel(kFxPortIn) + fx.portLevel(fxSlotPort(0));
        std::printf("      graph ports: IN %.3f, EQ %.3f, apart %.3f, after a second %.1f dB down; not analyzing %.3f\n",
                    in, eq, apart, 20.0 * std::log10(eq / std::max(fallen, 1e-9f)), unmeasured);
        check(std::abs(in - 0.5f) < 0.01f && std::abs(eq - 0.5f) < 0.05f && apart == 0.0f &&
                  std::abs(20.0 * std::log10(eq / fallen) - Effects::kPortFallDb) < 3.0 && unmeasured == 0.0f,
              "the effects graph tells how loud each of its ports is while the editor analyzes, falling away after");
    }
    {
        // A sine at -40 dB and 50 Hz through the shaper effect's drawn curve (as it starts: smooth, here made odd),
        // at unity gain: each sample comes out as the curve has it at the middle of its step, through the DC blocker
        // every drawn curve has.
        Effects fx;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        chain(p, {0});
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
            chain(p, {0});
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
        // A shaper effect switched off, its drawn curve changed meanwhile (a preset loaded, say), and on again: it
        // plays the new curve from its start, as a shaper that never ran with the old one does, rather than gliding
        // in from the old one.
        auto play = [&defaults](bool before) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
            chain(p, {0});
            p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Custom);
            p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
            p[paramIndex(0, FxParam::ShaperMix)] = 1.0;
            const int curve = shaperParams(0).curve;
            std::vector<float> left(kBlock), right(kBlock), out;
            size_t t = 0;
            auto run = [&](size_t blocks, bool keep) {
                for (size_t b = 0; b < blocks; ++b, t += kBlock) {
                    for (uint32_t s = 0; s < kBlock; ++s)
                        left[s] = right[s] = static_cast<float>(
                            0.5 * std::sin(2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate));
                    fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                    if (keep)
                        out.insert(out.end(), left.begin(), left.end());
                }
            };
            if (before) { // with the curve as it starts, then switched off
                run(40, false);
                p[paramIndex(0, FxParam::Enabled)] = 0.0;
                run(4, false);
            } else {
                p[paramIndex(0, FxParam::Enabled)] = 0.0;
                run(44, false);
            }
            for (int i = 0; i < shapePointCount(p[curve]); ++i)
                p[shapeFieldIndex(curve, i, PointField::Y)] *= 0.5;
            p[paramIndex(0, FxParam::Enabled)] = 1.0;
            run(8, true);
            return out;
        };
        const std::vector<float> restarted = play(true), fresh = play(false);
        double apart = 0.0;
        for (size_t i = 0; i < restarted.size(); ++i)
            apart = std::max(apart, static_cast<double>(std::abs(restarted[i] - fresh[i])));
        std::printf("      a shaper effect on again with a new curve: %.1f dB from one that never ran the old one\n",
                    20.0 * std::log10(apart + 1e-30));
        check(apart < 1e-6, "a shaper effect switched on again plays its curve as it is now, not the one it last had");
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
            chain(p, {0});
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
        chain(p, {0});
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
        chain(p, {0});
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
        chain(p, {0});
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
        // A note out of silence: through silence the upward side lifts as far as Range lets it, and the note that
        // comes then isn't lifted by that while the detector's level still climbs (see kMdLiftHeadroom). Upward only
        // (20:1 below -20 dB, Range 36 dB): a -6 dB sine at 1 kHz comes in after half a second of silence, and nothing
        // in its first 50 ms comes out louder than it went in.
        auto onset = [](const ParamValues &p, double hz, double level, double ramp) {
            // A band-limited saw (40 harmonics, or a sine for 1 kHz) after silence, rising over `ramp` seconds: the
            // loudest sample in its first 50 ms, its peak and RMS level over its 0.5 s to 0.75 s, and the RMS level of
            // the loudest 5 ms of its first 100 ms, each in dB.
            Effects fx;
            fx.prepare(kSampleRate);
            const int harmonics = hz > 500.0 ? 1 : 40;
            const size_t start = static_cast<size_t>(0.5 * kSampleRate), end = static_cast<size_t>(1.25 * kSampleRate);
            std::vector<float> left(kBlock), right(kBlock);
            double first = 0.0, peak = 0.0, power = 0.0, window = 0.0, loudest = 0.0;
            const double windowLength = 0.005 * kSampleRate;
            for (size_t t = 0; t < end; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s) {
                    const double since = (static_cast<double>(t + s) - static_cast<double>(start)) / kSampleRate;
                    double x = 0.0;
                    for (int h = 1; since >= 0.0 && h <= harmonics; ++h)
                        x += std::sin(2.0 * std::numbers::pi * hz * h * since) / h;
                    left[s] = right[s] = static_cast<float>(
                        x * level * (harmonics > 1 ? 2.0 / std::numbers::pi : 1.0) *
                        (ramp > 0.0 ? std::clamp(since / ramp, 0.0, 1.0) : 1.0));
                }
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock; ++s) {
                    const double since = (static_cast<double>(t + s) - static_cast<double>(start)) / kSampleRate;
                    if (since >= 0.0 && since < 0.05)
                        first = std::max(first, static_cast<double>(std::abs(left[s])));
                    if (since >= 0.0 && since < 0.1) {
                        window += left[s] * left[s] / windowLength;
                        const size_t into = t + s - start;
                        if ((into + 1) % static_cast<size_t>(windowLength) == 0) {
                            loudest = std::max(loudest, window);
                            window = 0.0;
                        }
                    }
                    if (since >= 0.5 && since < 0.75) {
                        peak = std::max(peak, static_cast<double>(std::abs(left[s])));
                        power += left[s] * left[s] / (0.25 * kSampleRate);
                    }
                }
            }
            return std::array<double, 4>{20.0 * std::log10(first + 1e-30), 20.0 * std::log10(peak + 1e-30),
                                         10.0 * std::log10(power + 1e-30), 10.0 * std::log10(loudest + 1e-30)};
        };
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        chain(p, {0});
        for (int band = 0; band < kMcBands; ++band) {
            p[mdDownIndex(0, band)] = normFromDynRatio(1.0);
            p[mdUpIndex(0, band)] = normFromDynRatio(20.0);
            p[mdBelowIndex(0, band)] = -20.0;
        }
        p[paramIndex(0, FxParam::MdRange)] = 36.0;
        const auto lifted = onset(p, 1000.0, 0.5, 0.0);
        std::printf("      3-band dynamics lifting 36 dB through silence: a -6 dB sine's start peaks at %+.1f dB, the "
                    "note at %+.1f dB\n",
                    lifted[0], lifted[1]);
        check(lifted[0] - lifted[1] < 6.0, "a three-band dynamics doesn't lift a note out of silence by the silence's Range");
        // OTT, as its card's menu sets it (see mdOttSettings()), against the OTT preset as measured: sines
        // stepped in level in each band and saws bounced through it. A sine at 40 Hz, 1 kHz and 8 kHz, at four RMS
        // levels, comes out within a dB of where the preset put it (dB, RMS).
        p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        chain(p, {0});
        for (const auto &[index, value] : mdOttSettings(0))
            p[index] = value;
        auto steadySine = [&](double hz, double rmsDb) { // the output's RMS level (dB) once settled
            Effects fx;
            fx.prepare(kSampleRate);
            const double amplitude = std::sqrt(2.0) * std::pow(10.0, rmsDb / 20.0);
            std::vector<float> left(kBlock), right(kBlock);
            double power = 0.0;
            const size_t from = static_cast<size_t>(0.75 * kSampleRate), to = static_cast<size_t>(1.0 * kSampleRate);
            for (size_t t = 0; t < to; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock && t >= from; ++s)
                    power += left[s] * left[s] / static_cast<double>(to - from);
            }
            return 10.0 * std::log10(power + 1e-30);
        };
        struct Measured {
            double hz;
            std::array<double, 4> out; // at -69, -51, -33 and -15 dB going in
        };
        constexpr Measured kPreset[] = {{40.0, {-35.6, -30.6, -24.0, -21.8}},
                                         {1000.0, {-39.7, -33.1, -24.8, -21.9}},
                                         {8000.0, {-37.1, -31.1, -26.1, -25.4}}};
        constexpr double kLevels[] = {-69.0, -51.0, -33.0, -15.0};
        double worst = 0.0;
        for (const Measured &m : kPreset) {
            std::printf("      3-band dynamics as OTT, a sine at %.0f Hz, against the preset's:", m.hz);
            for (int i = 0; i < 4; ++i) {
                const double got = steadySine(m.hz, kLevels[i]);
                worst = std::max(worst, std::abs(got - m.out[i]));
                std::printf(" %.0f -> %.1f (%.1f)", kLevels[i], got, m.out[i]);
            }
            std::printf("\n");
        }
        check(worst < 1.0, "a three-band dynamics as OTT follows the curves the OTT preset was measured to have");
        // A saw at 110 Hz with a 5 ms attack, out of silence: a loud one (-1 dB) comes out some 14 dB down and a
        // quiet one (-30 dB) some 10 dB up, as the preset did (-14.3 and +10.5 dB), and neither comes out at the lift the
        // silence had (+41 dB on the low and high bands): its start (the bands' gains apart, so their parts of a
        // transient no longer cancel, and the downward side still catching up) is louder than the note, as the preset's
        // was (its loudest 5 ms +6.6 and +2.7 dB over), but no 5 ms of it by more than 15 dB, 6 for the quiet one.
        const double sawRms = 20.0 * std::log10(1.0 / std::sqrt(3.0)); // a saw's RMS level against its peak
        const auto loud = onset(p, 110.0, std::pow(10.0, -1.0 / 20.0), 0.005);
        const auto quiet = onset(p, 110.0, std::pow(10.0, -30.0 / 20.0), 0.005);
        std::printf("      3-band dynamics as OTT: a -1 dB saw %+.1f dB, a -30 dB one %+.1f dB; their loudest 5 ms as "
                    "they start %+.1f and %+.1f dB over the note\n",
                    loud[2] - (-1.0 + sawRms), quiet[2] - (-30.0 + sawRms), loud[3] - loud[2], quiet[3] - quiet[2]);
        check(std::abs(loud[2] - (-1.0 + sawRms) + 14.3) < 3.0 && std::abs(quiet[2] - (-30.0 + sawRms) - 10.5) < 3.0,
              "a three-band dynamics as OTT pulls a loud saw down and lifts a quiet one as the OTT preset does");
        check(loud[3] - loud[2] < 15.0 && quiet[3] - quiet[2] < 6.0,
              "a three-band dynamics as OTT doesn't pop as a note starts out of silence");
        // The card's meter for a band runs from its level going in to that level through the band's dynamics: a loud 1 kHz note (-6 dB) for half a second, then a tail at -70 dB. As the tail comes, the band lifts it
        // by much of its Range while the level meter, held, falls only about 26 dB a second; drawn as that held level
        // plus the lift, the meter's far end shot up past 0 dB. It stays where the note had it, or under, falling.
        {
            Effects fx;
            fx.prepare(kSampleRate);
            std::vector<float> left(kBlock), right(kBlock);
            double during = -1e9, after = -1e9;
            for (size_t t = 0; t < static_cast<size_t>(1.1 * kSampleRate); t += kBlock) {
                const bool tail = t >= static_cast<size_t>(0.5 * kSampleRate);
                const double amplitude = std::pow(10.0, (tail ? -70.0 : -6.0) / 20.0);
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                const double end = 20.0 * std::log10(fx.bandLevel(0, 1) + 1e-30) + fx.bandGain(0, 1);
                if (t >= static_cast<size_t>(0.25 * kSampleRate))
                    (tail ? after : during) = std::max(tail ? after : during, end);
            }
            std::printf("      3-band dynamics as OTT, the mid band's meter: its far end at %.1f dB during a loud note, "
                        "at most %.1f dB once a quiet tail follows\n",
                        during, after);
            check(after < during + 1.0 && after < 0.0,
                  "a three-band dynamics' meter doesn't shoot up as a band goes quiet and its lift comes");
        }
    }
    {
        // The retired three-band compressor in a state from before version 6 loads as a three-band dynamics doing what
        // it did: its settings in the dynamics' fields (thresholds as Above, ratios as Down), the upward side at 1:1,
        // the compressor's knee, peaks; an LFO's modulation of its mid threshold now aims at the mid band's Above, as deep
        // in dB (its range is wider since version 10, see the check after these); its
        // own fields back at their defaults. Linked, its bands all took the low band's threshold, ratio and gain, and
        // so the dynamics' bands take the low band's values.
        ParamValues p = defaults();
        p[paramIndex(3, FxParam::Type)] = static_cast<double>(FxType::MultiComp);
        chain(p, {3});
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
                           std::abs(loaded[slotAmountIndex(0, 0)] -
                                    0.25 * kMdRetiredThresholdMinDb / kMdThresholdMinDb) < 1e-12;
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
        // The three-band dynamics' thresholds reach down to -80 dB since version 10, -60 before, and a modulation's depth
        // is a share of its target's range: an earlier state's depth on one loads three quarters as deep, swinging it as
        // far as before, and a version 10 state's as saved. A depth elsewhere is left alone.
        ParamValues p = defaults();
        p[paramIndex(2, FxParam::Type)] = static_cast<double>(FxType::MultiDyn);
        chain(p, {2});
        p[slotTargetIndex(0, 0)] = static_cast<double>(idOfIndex(mdBelowIndex(2, 1))) + 1.0;
        p[slotAmountIndex(0, 0)] = 0.4;
        p[slotTargetIndex(0, 1)] = static_cast<double>(idOfIndex(mdAboveIndex(2, 2))) + 1.0;
        p[slotAmountIndex(0, 1)] = -0.2;
        p[slotTargetIndex(0, 2)] = static_cast<double>(idOfIndex(mdGainIndex(2, 1))) + 1.0;
        p[slotAmountIndex(0, 2)] = 0.4;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, gui::UiLayout{}, true);
        auto load = [&bytes](uint32_t version) {
            std::vector<unsigned char> saved = bytes;
            std::memcpy(saved.data() + sizeof(uint32_t), &version, sizeof(version)); // after the magic
            MemoryReader reader{saved.data(), saved.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            ParamValues loaded;
            readParams(&in, loaded);
            return loaded;
        };
        const ParamValues before = load(9), now = load(10);
        const double range = paramTable()[mdBelowIndex(2, 1)].max - paramTable()[mdBelowIndex(2, 1)].min;
        std::printf("      3-band dynamics thresholds: %.0f dB of range; depths from version 9 %.2f and %.2f (the Gain's "
                    "%.2f), from version 10 %.2f and %.2f\n",
                    range, before[slotAmountIndex(0, 0)], before[slotAmountIndex(0, 1)], before[slotAmountIndex(0, 2)],
                    now[slotAmountIndex(0, 0)], now[slotAmountIndex(0, 1)]);
        check(range == 80.0 && std::abs(before[slotAmountIndex(0, 0)] - 0.3) < 1e-12 &&
                  std::abs(before[slotAmountIndex(0, 1)] + 0.15) < 1e-12 && before[slotAmountIndex(0, 2)] == 0.4 &&
                  now[slotAmountIndex(0, 0)] == 0.4 && now[slotAmountIndex(0, 1)] == -0.2,
              "a state before version 10 loads its depths on the three-band dynamics' thresholds as far as they swung");
    }
    {
        // The filter effect switched to another type or slope and back: the stage it left out starts from silence,
        // rather than playing back what it held then.
        auto stale = [&](FilterType first, int firstSlope, FilterType second, int secondSlope) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Filter);
            chain(p, {0});
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
        chain(p, {0});
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
        chain(p, {0});
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
        // Without them, the last note's voice ending doesn't let go of the key struck again: it is still down.
        auto keys = std::make_unique<Telemetry>();
        plain.synth().telemetry(*keys);
        check(plain.synth().activeVoiceCount() == 1 && keys->keyDown(60),
              "without note ids, a note on a key struck again ending leaves the key down");
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
    {
        // A plucked comb: a short burst of noise into a 55 Hz comb at full resonance puts out pulses a period apart,
        // with silence between them longer than a block. Let go, the voice rings on through those gaps until its
        // tail has faded, and then ends.
        Engine e(2);
        e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Noise);
        e.p[paramIndex(0, OpParam::Attack)] = kMinEnvNorm;
        e.p[paramIndex(0, OpParam::Decay)] = 0.15; // about 4 ms
        e.p[paramIndex(0, OpParam::Sustain)] = 0.0;
        e.p[paramIndex(0, OpParam::Release)] = 0.0;
        e.p[paramIndex(0, OpParam::Out)] = 0.0;
        e.p[sendIndex(0, 0)] = 1.0;
        e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
        e.p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Comb);
        e.p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(55.0);
        e.p[paramIndex(0, FilterParam::KeyTrack)] = 0.0;
        e.p[paramIndex(0, FilterParam::Resonance)] = 1.0;
        e.p[paramIndex(0, FilterParam::Out)] = 1.0;
        e.noteOn(45);
        e.render(4800);
        e.noteOff(45);
        const std::vector<float> tail = e.render(24000); // half a second
        float late = 0.0f;
        for (size_t s = 19200; s < tail.size(); ++s) // 400-500 ms after it was let go
            late = std::max(late, std::abs(tail[s]));
        const bool ringing = e.synth().activeVoiceCount() == 1;
        int blocks = 0;
        while (e.synth().activeVoiceCount() > 0 && blocks < 1200) { // up to 6 s more
            e.render(kBlock);
            ++blocks;
        }
        std::printf("      a plucked comb let go: %.1f dB 400-500 ms on, ending %.2f s later\n",
                    20.0 * std::log10(late + 1e-30), blocks * static_cast<double>(kBlock) / kSampleRate);
        check(ringing && late > 1e-3f && e.synth().activeVoiceCount() == 0,
              "a plucked comb rings on through the gaps between its pulses once let go, then ends");
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

    // --- A Custom wave plays its partials: drawn while the operator plays a sine, then taken up as it switches to
    // Custom (the 1st and the 3rd), and changed partial by partial as it plays (the 3rd out, the 2nd in).
    std::printf("Custom wave:\n");
    {
        Engine e(2);
        for (int k = 0; k < kPartials; ++k) {
            e.p[partialLevelIndex(0, k)] = 0.0;
            e.p[partialPhaseIndex(0, k)] = 0.0;
        }
        e.p[partialLevelIndex(0, 0)] = 1.0;
        e.p[partialLevelIndex(0, 2)] = 0.5;
        e.noteOn(57); // A3, 220 Hz
        e.render(4800);
        e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Custom);
        e.render(4800);
        const std::vector<float> first = e.render(kFftSize);
        e.p[partialLevelIndex(0, 2)] = 0.0;
        e.p[partialLevelIndex(0, 1)] = 0.5;
        e.render(4800);
        const std::vector<float> second = e.render(kFftSize);
        auto at = [](const std::vector<float> &signal, double hz) { // the loudest bin near it, in dB
            const std::vector<double> power = powerSpectrum(signal);
            const long center = std::lround(hz / (kSampleRate / kFftSize));
            double most = 1e-30;
            for (long b = center - 3; b <= center + 3; ++b)
                most = std::max(most, power[static_cast<size_t>(b)]);
            return 10.0 * std::log10(most);
        };
        const double f1 = at(first, 220.0), f2 = at(first, 440.0), f3 = at(first, 660.0);
        const double s1 = at(second, 220.0), s2 = at(second, 440.0), s3 = at(second, 660.0);
        std::printf("      1st and 3rd: 2nd %.0f dB, 3rd %.1f dB under the 1st; then 1st and 2nd: 2nd %.1f, 3rd %.0f\n",
                    f2 - f1, f3 - f1, s2 - s1, s3 - s1);
        check(std::abs(f3 - f1 + 6.0) < 1.0 && f2 - f1 < -60.0 && std::abs(s2 - s1 + 6.0) < 1.0 && s3 - s1 < -60.0,
              "a Custom wave plays the partials drawn, taken up as it is picked and changed as it plays");
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

        // An operator's Pan, swept: its left channel's gain moves with it, smoothly.
        auto panMod = [&](ParamValues &p) { lfoOn(p, paramIndex(0, OpParam::Pan), 0.3, lfo); };
        const double pan = unexpectedDb(steadyTone(1, key, panMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on operator Pan", pan);
        check(pan < -80.0, "LFO on an operator's Pan doesn't zipper");
        // The effects graph's input straight to its output, its wire's level swung like operator Out's above.
        auto wireMod = [&](ParamValues &p) {
            const int level = fxInputIndex(kFxOutNode, 0, FxInputField::Level);
            p[fxInputIndex(kFxOutNode, 0, FxInputField::From)] = fxPortValue(kFxPortIn);
            p[level] = 0.5;
            lfoOn(p, level, 0.4, lfo);
        };
        const double wire = unexpectedDb(steadyTone(1, key, wireMod), {f0, f0 - lfo, f0 + lfo});
        std::printf("      %-30s %7.1f\n", "LFO on an effects graph wire", wire);
        check(wire < -70.0, "LFO on a wire of the effects graph doesn't zipper");
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
        chain(p, {0});
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
    {
        // Once the sound stops, an analyzer's bands fall about 55 dB a second, at 96 kHz as at 48.
        auto fallen = [](double rate) {
            Analyzer analyzer;
            analyzer.prepare(rate);
            Analyzed signal;
            Analyzer::prepare(signal);
            const int band = static_cast<int>(normFromCutoff(1000.0) * kSpectrumBands);
            for (int i = 0; i < static_cast<int>(0.2 * rate); ++i)
                analyzer.push(signal, static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / rate)));
            const float loud = signal.bands[band];
            for (int i = 0; i < static_cast<int>(0.5 * rate); ++i)
                analyzer.push(signal, 0.0f);
            return 20.0 * std::log10(loud / signal.bands[band]);
        };
        const double at48 = fallen(48000.0), at96 = fallen(96000.0);
        std::printf("      an analyzer's band half a second after a sine stops: %.1f dB down at 48 kHz, %.1f at 96\n",
                    at48, at96);
        check(std::abs(at48 - 27.5) < 3.0 && std::abs(at96 - 27.5) < 3.0,
              "an analyzer's bands fall away as fast at any sample rate");
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
            chain(p, {0});
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

    // --- The phaser's and flanger's polarity, standing still with no feedback at full Mix (half wet). Positive, a
    // sine where the wet sound comes out turned over is taken away and one where it comes out as it went in plays on
    // at full level; negative, the other way round. A 2 ms flanger's first notch is at 250 Hz, and negative at
    // 500 Hz; four all-passes at 1 kHz turn a whole cycle there, half a cycle where each turns an eighth. Turned
    // over while a sine plays, the polarity fades through nothing: no sample steps further than the sine's own do.
    std::printf("polarity (level of a sine, in dB, positive and negative):\n");
    {
        auto run = [](FxType type, double hz, bool negative, bool turnOver, double *steepest) {
            Effects fx;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            const bool phaser = type == FxType::Phaser;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(type);
            p[paramIndex(0, phaser ? FxParam::PhaserRate : FxParam::FlangerRate)] = 0.0;
            p[paramIndex(0, phaser ? FxParam::PhaserDepth : FxParam::FlangerDepth)] = 0.0;
            p[paramIndex(0, phaser ? FxParam::PhaserFeedback : FxParam::FlangerFeedback)] = 0.0;
            p[paramIndex(0, phaser ? FxParam::PhaserMix : FxParam::FlangerMix)] = 1.0;
            p[paramIndex(0, phaser ? FxParam::PhaserStereo : FxParam::FlangerStereo)] = 0.0;
            p[paramIndex(0, FxParam::PhaserCenter)] = normFromCutoff(1000.0);
            p[paramIndex(0, FxParam::PhaserStages)] = 4.0;
            p[paramIndex(0, FxParam::FlangerDelay)] = normFromTime(0.002);
            const int polarity = paramIndex(0, phaser ? FxParam::PhaserNegative : FxParam::FlangerNegative);
            p[polarity] = negative ? 1.0 : 0.0;
            chain(p, {0});
            const uint32_t total = static_cast<uint32_t>(kSampleRate / 2), turnAt = total / 2;
            std::vector<float> left(kBlock), right(kBlock);
            double sum = 0.0, last = 0.0;
            uint32_t measured = 0;
            *steepest = 0.0;
            for (uint32_t done = 0; done < total; done += kBlock) {
                const uint32_t n = std::min(kBlock, total - done);
                if (turnOver && done >= turnAt)
                    p[polarity] = negative ? 0.0 : 1.0;
                for (uint32_t s = 0; s < n; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.5 * std::sin(2.0 * std::numbers::pi * hz * (done + s) / kSampleRate));
                fx.process(left.data(), right.data(), n, p, 120.0, 60.0);
                for (uint32_t s = 0; s < n; ++s) {
                    if (done + s > total / 4) // past the first 125 ms
                        *steepest = std::max(*steepest, std::abs(left[s] - last));
                    last = left[s];
                    if (done + s >= total - total / 5) {
                        sum += left[s] * left[s];
                        ++measured;
                    }
                }
            }
            return 20.0 * std::log10(std::sqrt(sum / measured) / (0.5 / std::sqrt(2.0)) + 1e-12);
        };
        // Where each of four all-passes at 1 kHz turns an eighth of a cycle (by the bilinear transform's curve).
        const double eighth = kSampleRate / std::numbers::pi *
                              std::atan(std::tan(std::numbers::pi * 1000.0 / kSampleRate) * std::tan(std::numbers::pi / 8.0));
        bool flipped = true, smooth = true;
        for (FxType type : {FxType::Phaser, FxType::Flanger}) {
            const bool phaser = type == FxType::Phaser;
            const double positiveNotch = phaser ? eighth : 250.0, negativeNotch = phaser ? 1000.0 : 500.0;
            double steep = 0.0;
            const double a = run(type, positiveNotch, false, false, &steep), b = run(type, positiveNotch, true, false, &steep);
            const double c = run(type, negativeNotch, false, false, &steep), d = run(type, negativeNotch, true, false, &steep);
            std::printf("      %-8s %4.0f Hz: %6.1f, %6.1f   %4.0f Hz: %6.1f, %6.1f\n", phaser ? "phaser" : "flanger",
                        positiveNotch, a, b, negativeNotch, c, d);
            flipped = flipped && a < -40.0 && std::abs(b) < 0.5 && std::abs(c) < 0.5 && d < -40.0;
            for (bool negative : {false, true}) {
                // The sine's own steepest step, against the steepest once its polarity turns over and it fades out.
                const double hz = negative ? positiveNotch : negativeNotch;
                const double own = 0.5 * 2.0 * std::numbers::pi * hz / kSampleRate;
                const double level = run(type, hz, negative, true, &steep);
                std::printf("      %-8s turned %s at %4.0f Hz: steepest step %.4f (the sine's %.4f), then %.1f dB\n",
                            phaser ? "phaser" : "flanger", negative ? "positive" : "negative", hz, steep, own, level);
                smooth = smooth && steep < 1.05 * own && level < -40.0;
            }
        }
        check(flipped, "negative, the phaser's and flanger's notches move to where they had peaks");
        check(smooth, "the phaser's and flanger's polarity turned over mid-sine fades rather than clicking");
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
            chain(p, {0});
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
            chain(p, {0});
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
    {
        // Shimmer turned down to 0, in chunks as short as events cut them, until the reverb has rung out, then up
        // again: its shifters start from silence, so nothing of the tail before comes back.
        double worst = 0.0;
        for (int trial = 0; trial < 8; ++trial) {
            Effects fx;
            fx.prepare(kSampleRate);
            fx.setAnalyzing(true); // as with the editor open, so silence still runs the effects
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
            p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Shimmer);
            p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(0.5);
            p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
            const double amount = 0.4 + 0.07 * trial;
            p[paramIndex(0, FxParam::ReverbShimmer)] = amount;
            chain(p, {0});
            float left[32], right[32];
            for (int b = 0; b < 750; ++b) { // half a second of an A4
                for (int s = 0; s < 32; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * (b * 32 + s) / kSampleRate));
                fx.process(left, right, 32, p, 120.0, 60.0);
            }
            p[paramIndex(0, FxParam::ReverbShimmer)] = 0.0;
            for (int b = 0; b < 4000 && (b < 400 || fx.ringing()); ++b) { // up to 20 s
                const uint32_t n = 1 + static_cast<uint32_t>((b * 7 + trial * 5) % 32);
                std::fill_n(left, n, 0.0f);
                std::fill_n(right, n, 0.0f);
                fx.process(left, right, n, p, 120.0, 60.0);
            }
            p[paramIndex(0, FxParam::ReverbShimmer)] = amount;
            for (int b = 0; b < 1500; ++b) { // a second
                std::fill_n(left, 32, 0.0f);
                std::fill_n(right, 32, 0.0f);
                fx.process(left, right, 32, p, 120.0, 60.0);
                for (int s = 0; s < 32; ++s)
                    worst = std::max({worst, static_cast<double>(std::abs(left[s])), static_cast<double>(std::abs(right[s]))});
            }
        }
        std::printf("      Shimmer down to 0 and up again once rung out: %.0f dB of the tail before\n",
                    20.0 * std::log10(worst + 1e-30));
        check(worst < 1e-5, "a shimmer turned down and up again brings back nothing of the tail before");
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

        // A chord over voices that are all playing steals as many at once, more than a few: each fades out too.
        {
            double chordWorst = 0.0, chordSteady = 0.0;
            for (int trial = 0; trial < 4; ++trial) {
                Engine e(2, 8);
                e.p[paramIndex(0, OpParam::Attack)] = normFromTime(0.005);
                for (int k = 0; k < 8; ++k)
                    e.noteOn(36 + 2 * k);
                const std::vector<float> before = e.render(2400 + 37 * trial);
                for (int k = 0; k < 6; ++k)
                    e.noteOn(37 + 2 * k);
                const std::vector<float> both = joined(before, e.render(2400));
                chordSteady = std::max(chordSteady, roughness(both, 0, before.size()));
                chordWorst = std::max(chordWorst, roughness(both, before.size() - 2, both.size()));
            }
            std::printf("      %-30s %7.1fx\n", "six voices stolen at once", chordWorst / chordSteady);
            check(chordWorst < 4.0 * chordSteady, "a chord stealing six voices at once fades them out instead of clicking");
        }

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
    ampChecks();

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
            chain(e.p, {0});
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
