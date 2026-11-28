// Audio quality measurements on the engine itself: aliasing, zipper noise from
// stepped modulation, clicks when a voice is stolen or a param jumps between
// events, and what oversampling costs. Prints the numbers and checks them
// against limits. Usage: fmvibe-quality

#include "factory.h"
#include "gui/controller.h"
#include "gui/dx7.h"
#include "state.h"
#include "synth.h"
#include "unison.h"
#include "rateenvelope.h"
#include "uistrings.h"
#include "warp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace fmvibe;

namespace {

// Every operator's unison copies from left to right, as they sat before version 13 brought Unison Pan (Alternate to
// begin with): a patch saved so, relabeled as an older version, loads as saved.
void asBeforeUnisonPan(ParamValues &p) {
    for (int op = 0; op < kOps; ++op)
        p[paramIndex(op, OpParam::UnisonPan)] = static_cast<double>(UniPan::Ramp);
}

// The sequencer playing with the host's transport, its Play to begin with before version 17 made it Key: a patch saved
// so, relabeled as an older version, loads as saved.
void asBeforeKeyPlay(ParamValues &p) { p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Host); }

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
        synth_->prepare(kSampleRate, voices, oversampling);
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(GlobalParam::Volume)] = 0.0; // full level, rather than the 12 dB of room it starts with
    }

    void noteOn(int key, int id = -1) { synth_->noteOn({id, 0, 0, static_cast<int16_t>(key)}, 1.0); }
    void noteOff(int key) { synth_->noteOff({-1, 0, 0, static_cast<int16_t>(key)}); }

    // Renders `frames` in host-sized blocks, or in pieces of `piece` frames; returns the left channel.
    std::vector<float> render(size_t frames, uint32_t piece = kBlock) {
        std::vector<float> left(frames), right(frames);
        for (size_t done = 0; done < frames; done += piece) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(piece, frames - done));
            synth_->render(left.data() + done, right.data() + done, n, p);
        }
        return left;
    }

    Synth &synth() { return *synth_; }
    ParamValues p{};

private:
    std::unique_ptr<Synth> synth_ = std::make_unique<Synth>(); // over a megabyte: not on the stack
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
        // As it was but for going up to the amp's oversampling (4x to begin with) and back down, as its wet sound does,
        // so the two stay in step (see fxlag.h).
        std::vector<float> late = in;
        UpDown path;
        path.prepare(kSampleRate, ampOversampling(kDefaultAmpOversampling));
        for (size_t i = 0; i < late.size(); i += 64)
            path.run(late.data() + i, static_cast<int>(std::min<size_t>(64, late.size() - i)));
        double gap = 0.0;
        for (size_t i = 0; i < in.size(); ++i)
            gap = std::max(gap, static_cast<double>(std::abs(dry.left[i] - late[i])));
        // (After the 5 ms the effect fades in over as it starts, while what went in still shows through.)
        const bool same = std::equal(mono.left.begin() + 4800, mono.left.end(), mono.right.begin() + 4800);
        check(same && rms(mono.right, 4800) > 0.01 && rms(stereo.right, 0) == 0.0 &&
                  rms(stereo.left, 4800) > 0.01,
              "the amp runs once on both sides' sum in mono, and once a side in stereo");
        std::printf("      Mix at 0: off the sound through the amp's oversampling's path by %.2g at most\n", gap);
        check(gap < 1e-6, "the amp's Mix at 0 leaves the sound alone, as late as its oversampling");
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

// --- A ramp that gates a cheaper path ends exactly where its knob is: a reverb's Diffusion turned away from 100 % and
// back, and a delay's Duck from 0 and back, at every rate and in short pieces, where each chunk goes a share of the
// way and its steps' sum may land a hair off. Off by any, the reverb would go on mixing its lines the long way, and the
// delay ducking, for good.
void rampEndChecks() {
    std::printf("ramps ending on their knobs:\n");
    int turns = 0, reverbOff = 0, delayOff = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
        for (const uint32_t piece : {32u, 7u}) {
            auto reverb = std::make_unique<fx::Reverb>();
            auto tables = std::make_unique<fx::ReverbTables>();
            auto delay = std::make_unique<fx::Delay>();
            reverb->prepare(rate);
            tables->prepare(rate);
            delay->prepare(rate);
            auto p = std::make_unique<ParamValues>();
            for (int i = 0; i < kParamCount; ++i)
                (*p)[i] = paramTable()[i].def;
            const fx::Context ctx{*p, 0, rate, fx::rampShortest(rate), 120.0, 60.0};
            (*p)[paramIndex(0, FxParam::ReverbDiffusion)] = 1.0;
            (*p)[paramIndex(0, FxParam::DelayDuck)] = 0.0;
            reverb->start(ctx);
            delay->start(ctx);
            std::vector<float> left(piece), right(piece);
            uint64_t done = 0;
            auto play = [&](double seconds) {
                for (const uint64_t end = done + static_cast<uint64_t>(seconds * rate); done < end; done += piece) {
                    for (uint32_t s = 0; s < piece; ++s)
                        left[s] = right[s] =
                            static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 220.0 * (done + s) / rate));
                    float *io[2] = {left.data(), right.data()};
                    reverb->run(ctx, *tables, io, piece);
                    for (uint32_t s = 0; s < piece; ++s)
                        left[s] = right[s] =
                            static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 220.0 * (done + s) / rate));
                    delay->run(ctx, io, piece);
                }
            };
            play(0.05);
            for (const double away : {0.73, 0.5, 0.37, 0.21, 0.05, 0.9, 0.333}) {
                (*p)[paramIndex(0, FxParam::ReverbDiffusion)] = away;
                (*p)[paramIndex(0, FxParam::DelayDuck)] = 1.0 - away;
                play(0.05);
                (*p)[paramIndex(0, FxParam::ReverbDiffusion)] = 1.0;
                (*p)[paramIndex(0, FxParam::DelayDuck)] = 0.0;
                play(0.05);
                reverbOff += reverb->diffusion != 1.0;
                delayOff += delay->duck != 0.0;
                ++turns;
            }
        }
    std::printf("      of %d turns back, the reverb's Diffusion off 100 %% after %d, the delay's Duck off 0 after %d\n",
                turns, reverbOff, delayOff);
    const bool reverbBack = reverbOff == 0, delayBack = delayOff == 0;
    check(reverbBack, "a reverb's Diffusion turned back to 100 % lands on it exactly, at every rate and piece size");
    check(delayBack, "a delay's Duck turned back to 0 lands on it exactly, at every rate and piece size");
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        // written, and after five, past their length, written all round; also with its times then turned to the
        // longest and the shortest, reading anywhere along its lines (which it clears only as they are read).
        bool clean = true;
        for (double played : {0.5, 5.0}) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            float after = run(0.3, 0.0);
            p[paramIndex(0, FxParam::DelayTimeL)] = p[paramIndex(0, FxParam::DelayTimeR)] = 1.0; // as long as it goes
            after = std::max(after, run(4.5, 0.0));
            p[paramIndex(0, FxParam::DelayTimeL)] = p[paramIndex(0, FxParam::DelayTimeR)] = 0.0;
            after = std::max(after, run(0.5, 0.0));
            std::printf("      a delay off and on again after %.1f s of a sine: %.0f dB of it after\n", played,
                        20.0 * std::log10(after + 1e-30));
            clean = clean && after < 1e-6f;
        }
        check(clean, "a delay switched off and on again plays back nothing it held");
    }
    {
        // A reverb likewise, its lines and its pre-delay (a room's, which reads its early reflections from it too): off
        // and on again after a sine, then at its largest size and longest pre-delay, it plays back nothing it held.
        bool clean = true;
        for (double played : {0.05, 3.0}) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
            p[paramIndex(0, FxParam::ReverbType)] = static_cast<double>(ReverbType::Room);
            p[paramIndex(0, FxParam::ReverbSize)] = 0.0;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 0.0;
            p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
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
            // Played at its largest first, so its lines and pre-delay hold sound all along them.
            p[paramIndex(0, FxParam::ReverbSize)] = 1.0;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 1.0;
            run(played, 0.5);
            p[paramIndex(0, FxParam::ReverbSize)] = 0.0;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 0.0;
            run(0.5, 0.5);
            p[paramIndex(0, FxParam::Enabled)] = 0.0;
            run(0.05, 0.0);
            p[paramIndex(0, FxParam::Enabled)] = 1.0;
            float after = run(0.2, 0.0);
            p[paramIndex(0, FxParam::ReverbSize)] = 1.0;
            p[paramIndex(0, FxParam::ReverbPreDelay)] = 1.0;
            after = std::max(after, run(1.0, 0.0));
            std::printf("      a reverb off and on again after %.2f s of a sine: %.0f dB of it after\n", played + 0.5,
                        20.0 * std::log10(after + 1e-30));
            clean = clean && after < 1e-6f;
        }
        check(clean, "a reverb switched off and on again plays back nothing it held, at any size or pre-delay");
    }
    {
        // A sample that isn't a number going into a delay and a reverb, and one louder than any sound (which would
        // ring on in them for good, or overflow): what comes out stays finite and sane, the tails ring out to silence,
        // and a sine after them comes out as it should.
        bool fine = true;
        for (const float bad : {std::numeric_limits<float>::quiet_NaN(), 3e38f}) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
        // A Lo-Fi puts silence out as silence, its levels half a step off 0 notwithstanding: at 1 bit after a reverb,
        // whose tail goes through it, the effects ring out and rest, putting out nothing (no DC to keep them going).
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::LoFi);
        p[paramIndex(1, FxParam::LoFiBits)] = 1.0;
        chain(p, {0, 1});
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
        const float sounding = run(0.5, 0.5);
        int waited = 0;
        while (fx.ringing() && waited++ < 120) // up to a minute for the reverb's tail
            run(0.5, 0.0);
        const float rested = run(1.0, 0.0);
        std::printf("      a 1-bit Lo-Fi after a reverb: %.2f sounding, rung out after %.1f s, %.1e after that\n", sounding,
                    0.5 * waited, rested);
        check(sounding > 0.1f && !fx.ringing() && rested == 0.0f,
              "a Lo-Fi puts silence out as silence, and the effects ring out through it");
    }
    {
        // Effects with nothing to do: once every tail has rung out, silence costs a small share of what a sound
        // through them does, and comes out as silence; a sound after it comes through again.
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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

        // So with the editor open, analyzing (a parametric EQ after them, whose analyzer takes in the silence as
        // running it would): the plugin then keeps processing with nothing sounding while a free LFO shows.
        const auto openHeap = std::make_unique<Effects>();
        Effects &open = *openHeap;
        open.prepare(kSampleRate);
        open.setAnalyzing(true);
        p[paramIndex(3, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
        chain(p, {0, 1, 2, 3});
        auto runOpen = [&](double seconds, double amplitude) {
            float peak = 0.0f;
            for (size_t t = 0, end = static_cast<size_t>(seconds * kSampleRate); t < end; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                open.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock; ++s)
                    peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
            }
            return peak;
        };
        auto timedOpen = [&](double seconds, double amplitude, float &peak) {
            double best = 1e9;
            for (int k = 0; k < 3; ++k) {
                const auto start = std::chrono::steady_clock::now();
                peak = std::max(peak, runOpen(seconds, amplitude));
                best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
            return best;
        };
        runOpen(0.5, 0.5);
        int openWaited = 0;
        while (open.ringing() && openWaited++ < 120)
            runOpen(0.5, 0.0);
        float openBusyPeak = 0.0f, openIdlePeak = 0.0f;
        const double openBusy = timedOpen(2.0, 0.01, openBusyPeak);
        while (open.ringing() && openWaited++ < 240)
            runOpen(0.5, 0.0);
        const double openIdle = timedOpen(2.0, 0.0, openIdlePeak);
        std::printf("      the same and a parametric EQ, analyzing: a quiet sine %.2f ms, silence once rung out %.3f ms\n",
                    openBusy * 1e3, openIdle * 1e3);
        check(openIdle < 0.25 * openBusy && openIdlePeak == 0.0f && openBusyPeak > 0.0f,
              "silence through effects that have rung out costs little while the editor analyzes them too");

        // The EQ alone, rung out at once: its spectrum goes on falling through the silence that goes by idle.
        const auto eqOnlyHeap = std::make_unique<Effects>();
        Effects &eqOnly = *eqOnlyHeap;
        eqOnly.prepare(kSampleRate);
        eqOnly.setAnalyzing(true);
        ParamValues q = defaults();
        q[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
        chain(q, {0});
        auto runEq = [&](double seconds, double amplitude) {
            for (size_t t = 0, end = static_cast<size_t>(seconds * kSampleRate); t < end; t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] = static_cast<float>(
                        amplitude * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(t + s) / kSampleRate));
                eqOnly.process(left.data(), right.data(), kBlock, q, 120.0, 60.0);
            }
        };
        auto loudestBand = [&] {
            float most = 0.0f;
            for (int band = 0; band < kSpectrumBands; ++band)
                most = std::max(most, eqOnly.spectrum(0, band));
            return most;
        };
        runEq(0.5, 0.5);
        int eqWaited = 0;
        while (eqOnly.ringing() && eqWaited++ < 100)
            runEq(0.01, 0.0);
        const float rungOut = loudestBand();
        runEq(0.5, 0.0);
        const float halfSecond = loudestBand();
        std::printf("      a parametric EQ's spectrum through idle silence: %.1f dB down in half a second\n",
                    20.0 * std::log10(std::max(halfSecond, 1e-30f) / std::max(rungOut, 1e-30f)));
        check(!eqOnly.ringing() && rungOut > 1e-3f && halfSecond < 0.1f * rungOut,
              "an analyzer's spectrum falls away through silence that goes by without running the effects");
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
            {"utility gain 0 to +12 dB", FxType::Utility, FxParam::UtilityGain, utilityGainNorm(1.0), 1.0},
            {"utility pan C to R50", FxType::Utility, FxParam::UtilityPan, 0.0, 0.5},
            {"utility width 100 to 0 %", FxType::Utility, FxParam::UtilityWidth, 1.0, 0.0},
            {"chorus mix 0 to 100 %", FxType::Chorus, FxParam::ChorusMix, 0.0, 1.0},
            {"chorus voices 2 to 6", FxType::Chorus, FxParam::ChorusVoices, 2.0, 6.0},
            {"chorus spread 0 to 100 %", FxType::Chorus, FxParam::ChorusSpread, 0.0, 1.0},
        };
        double worst = 0.0;
        for (const Jump &jump : jumps) {
            auto roughJump = [&jump](uint32_t piece) {
                Engine e(2);
                e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(jump.type);
                chain(e.p, {0});
                if (jump.type == FxType::Utility) // a side to the utility's width
                    e.p[paramIndex(0, OpParam::Pan)] = -0.7;
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
        // At a high rate, a jump ramps over as long a time as at 48 kHz, not as few samples: a delay's Mix taken from
        // 0 to 100 % and a wire's level to a quarter, on a steady level, change no faster in time at 96 or 192 kHz.
        auto steepest = [&](double rate, bool wire) { // the most the output moves in a second, at its steepest
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(rate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
            p[paramIndex(0, FxParam::DelayMix)] = 0.0;
            chain(p, {0});
            constexpr uint32_t kPiece = 32;
            float left[kPiece], right[kPiece];
            float last = 0.0f, most = 0.0f;
            const size_t before = static_cast<size_t>(0.05 * rate), after = static_cast<size_t>(0.01 * rate);
            for (size_t t = 0; t < before + after; t += kPiece) {
                if (t == before / kPiece * kPiece) {
                    if (wire)
                        p[fxInputIndex(kFxOutNode, 0, FxInputField::Level)] = 0.25;
                    else
                        p[paramIndex(0, FxParam::DelayMix)] = 1.0;
                }
                std::fill_n(left, kPiece, 0.5f);
                std::fill_n(right, kPiece, 0.5f);
                fx.process(left, right, kPiece, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kPiece; ++s) {
                    if (t + s > before / 2)
                        most = std::max(most, std::abs(left[s] - last));
                    last = left[s];
                }
            }
            return most * rate;
        };
        bool even = true;
        for (bool wire : {false, true}) {
            const double at48 = steepest(48000.0, wire);
            for (double rate : {96000.0, 192000.0}) {
                const double ratio = steepest(rate, wire) / at48;
                std::printf("      %s at %.0f kHz: %.2fx as steep in time as at 48 kHz\n",
                            wire ? "a wire's level to a quarter" : "a delay's Mix 0 to 100 %", rate / 1000.0, ratio);
                even = even && ratio < 1.1 && ratio > 0.5;
            }
        }
        check(even, "a jump ramps over as long a time at 96 and 192 kHz as at 48 kHz");
    }
    {
        // A frequency shifter moves a note up without its mirror image: 50 Hz to 10 kHz shifted up 30 Hz, the image 30
        // Hz below each at least 60 dB under it, at every common rate (its Hilbert pair worked out for the rate).
        auto level = [](const std::vector<float> &x, double hz, double rate) { // Hann-windowed
            std::complex<double> sum = 0.0;
            for (size_t i = 0; i < x.size(); ++i) {
                const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / x.size());
                sum += x[i] * w * std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate);
            }
            return std::abs(sum);
        };
        double worst = 1e9;
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
            double rateWorst = 1e9;
            for (double hz : {50.0, 100.0, 1000.0, 10000.0}) {
                const auto fxHeap = std::make_unique<Effects>();
                Effects &fx = *fxHeap;
                fx.prepare(rate);
                ParamValues p = defaults();
                p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::FreqShifter);
                p[paramIndex(0, FxParam::ShiftAmount)] = 0.3; // 30 Hz of its 100 Hz range
                p[paramIndex(0, FxParam::ShiftMix)] = 1.0;
                chain(p, {0});
                constexpr uint32_t kPiece = 32;
                float left[kPiece], right[kPiece];
                std::vector<float> out;
                for (size_t t = 0; t < static_cast<size_t>(1.5 * rate); t += kPiece) {
                    for (uint32_t s = 0; s < kPiece; ++s)
                        left[s] = right[s] = static_cast<float>(
                            0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t + s) / rate));
                    fx.process(left, right, kPiece, p, 120.0, 60.0);
                    if (t >= static_cast<size_t>(0.5 * rate))
                        out.insert(out.end(), left, left + kPiece);
                }
                rateWorst = std::min(rateWorst, 20.0 * std::log10(level(out, hz + 30.0, rate) /
                                                                  std::max(level(out, hz - 30.0, rate), 1e-30)));
            }
            std::printf("      a frequency shifter at %.1f kHz: the image at least %.1f dB under the shifted note\n",
                        rate / 1000.0, rateWorst);
            worst = std::min(worst, rateWorst);
        }
        check(worst > 60.0, "a frequency shifter's mirror image stays 60 dB down from 50 Hz up, at 44.1 to 192 kHz");

        // What a shift takes past 0 Hz or past half the rate is gone, rather than coming back the other way, and what
        // it leaves between them plays at the level it went in.
        auto shifted = [&](double rate, double hz, double shift, double feedback) { // a settled second of a sine at 0.5
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(rate);
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::FreqShifter);
            p[paramIndex(0, FxParam::ShiftRange)] = normFromShiftRange(kMaxShiftRange);
            p[paramIndex(0, FxParam::ShiftAmount)] = shift / kMaxShiftRange;
            p[paramIndex(0, FxParam::ShiftFeedback)] = feedback;
            p[paramIndex(0, FxParam::ShiftMix)] = 1.0;
            chain(p, {0});
            constexpr uint32_t kPiece = 32;
            float left[kPiece], right[kPiece];
            std::vector<float> out;
            for (size_t t = 0; t < static_cast<size_t>(1.5 * rate); t += kPiece) {
                for (uint32_t s = 0; s < kPiece; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.5 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t + s) / rate));
                fx.process(left, right, kPiece, p, 120.0, 60.0);
                if (t >= static_cast<size_t>(0.5 * rate))
                    out.insert(out.end(), left, left + kPiece);
            }
            return out;
        };
        double reflected = -1e9, keptOff = 0.0;
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
            const double nyquist = 0.5 * rate;
            struct Move {
                double hz, shift, back; // the sine, the shift and where the shift would bring it back
            };
            const Move moves[] = {{300.0, -500.0, 200.0},
                                  {440.0, -540.0, 100.0},
                                  {1000.0, -3000.0, 2000.0},
                                  {nyquist - 1000.0, 3000.0, nyquist - 2000.0},
                                  {nyquist - 200.0, 500.0, nyquist - 300.0}};
            for (const Move &m : moves) {
                const std::vector<float> out = shifted(rate, m.hz, m.shift, 0.0);
                // What `level` reads of a sine at 0.5: half its amplitude times the Hann window's sum.
                const double full = 0.5 * 0.5 * 0.5 * static_cast<double>(out.size());
                reflected = std::max(reflected, 20.0 * std::log10(std::max(level(out, m.back, rate), 1e-30) / full));
            }
            const Move kept[] = {{1000.0, -300.0, 700.0}, {300.0, -200.0, 100.0}, {5000.0, 3000.0, 8000.0}};
            for (const Move &m : kept) {
                const std::vector<float> out = shifted(rate, m.hz, m.shift, 0.0);
                const double full = 0.5 * 0.5 * 0.5 * static_cast<double>(out.size());
                keptOff = std::max(keptOff, std::abs(20.0 * std::log10(level(out, m.back, rate) / full)));
            }
        }
        std::printf("      a frequency shifter: what a shift takes past either end comes back at %.1f dB at most, what "
                    "stays between them within %.2f dB\n",
                    reflected, keptOff);
        check(reflected < -60.0, "a frequency shifter brings nothing back from past 0 Hz or half the rate");
        check(keptOff < 0.5, "a frequency shifter keeps the level of what it moves between 0 Hz and half the rate");
        // Just past 0 Hz, under where its Hilbert pair keeps 90 degrees, some comes back (the figures printed).
        for (double past : {80.0, 40.0, 20.0, 10.0}) {
            const std::vector<float> out = shifted(48000.0, 300.0, -300.0 - past, 0.0);
            std::printf("      a partial shifted %.0f Hz past 0 Hz comes back at %.1f dB\n", past,
                        20.0 * std::log10(std::max(level(out, past, 48000.0), 1e-30) /
                                          (0.5 * 0.5 * 0.5 * static_cast<double>(out.size()))));
        }

        // Fed back, a spiral down runs out at 0 Hz: 1030 Hz down by 100 Hz a time round passes 230 Hz and 130 Hz on its
        // way to 30 Hz, from where it would come back at 70 Hz.
        const std::vector<float> spiral = shifted(48000.0, 1030.0, -100.0, 0.9);
        const double spiralBack = 20.0 * std::log10(std::max(level(spiral, 70.0, 48000.0), 1e-30) /
                                                    level(spiral, 230.0, 48000.0));
        std::printf("      a frequency shifter's spiral down: 70 Hz %.1f dB under the 230 Hz it passes\n", -spiralBack);
        check(spiralBack < -40.0, "a frequency shifter's spiral runs out at 0 Hz rather than coming back up");
    }
    {
        // The effects keep the top of what goes through them where their knobs don't take it away: a quiet sine at
        // 1 kHz and one at 12 or 15 kHz come out of each within 1.5 dB of each other (the figures printed).
        auto through = [](ParamValues p, double hz) { // the RMS out over the RMS in of a -30 dB sine, settled
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            chain(p, {0});
            constexpr uint32_t kPiece = 64;
            float left[kPiece], right[kPiece];
            double in = 0.0, out = 0.0;
            for (size_t t = 0; t < static_cast<size_t>(0.6 * kSampleRate); t += kPiece) {
                for (uint32_t s = 0; s < kPiece; ++s)
                    left[s] = right[s] = static_cast<float>(
                        0.0316 * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(t + s) / kSampleRate));
                for (uint32_t s = 0; s < kPiece && t >= static_cast<size_t>(0.2 * kSampleRate); ++s)
                    in += static_cast<double>(left[s]) * left[s];
                fx.process(left, right, kPiece, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kPiece && t >= static_cast<size_t>(0.2 * kSampleRate); ++s)
                    out += static_cast<double>(left[s]) * left[s];
            }
            return 10.0 * std::log10(out / in);
        };
        auto tilt = [&](const ParamValues &p, double hz) { return through(p, hz) - through(p, 1000.0); };
        ParamValues shaper = defaults();
        shaper[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        shaper[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Soft);
        shaper[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
        shaper[paramIndex(0, FxParam::ShaperMix)] = 1.0;
        ParamValues filter = defaults();
        filter[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Filter);
        filter[paramIndex(0, FxParam::FilterType)] = static_cast<double>(FilterType::HighPass);
        filter[paramIndex(0, FxParam::FilterCutoff)] = 0.0;
        filter[paramIndex(0, FxParam::FilterResonance)] = 0.0;
        filter[paramIndex(0, FxParam::FilterDrive)] = 0.5;
        filter[paramIndex(0, FxParam::FilterMix)] = 1.0;
        const double shaperTilt = tilt(shaper, 15000.0), filterTilt = tilt(filter, 15000.0);
        std::printf("      15 kHz against 1 kHz: a soft shaper %+.2f dB, a driven high-pass filter %+.2f dB\n", shaperTilt,
                    filterTilt);
        check(std::abs(shaperTilt) < 1.5, "the shaper effect keeps a quiet sound's top");
        check(std::abs(filterTilt) < 1.5, "the filter effect's Drive keeps a quiet sound's top");

        // A parametric EQ's bands sound as the editor draws them (peqResponse()), in the top octaves too: a bell, a
        // dip, a high shelf, a low cut and a high cut up high, measured from 3 to 15 kHz, within 1.25 dB.
        struct Band {
            PeqType type;
            double hz, gainDb, q;
        };
        double farthest = 0.0;
        for (const Band &b : {Band{PeqType::Peak, 12000.0, 12.0, 0.707}, Band{PeqType::Peak, 12000.0, -12.0, 1.0},
                              Band{PeqType::HighShelf, 10000.0, 9.0, 0.707}, Band{PeqType::LowCut, 12000.0, 0.0, 0.707},
                              Band{PeqType::HighCut, 8000.0, 0.0, 0.707}}) {
            ParamValues p = defaults();
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
            p[paramIndex(0, FxParam::PeqAmount)] = 1.0;
            for (int band = 0; band < kPeqBands; ++band)
                p[peqOnIndex(0, band)] = band == 0 ? 1.0 : 0.0;
            p[peqTypeIndex(0, 0)] = static_cast<double>(b.type);
            p[peqFreqIndex(0, 0)] = normFromCutoff(b.hz);
            p[peqGainIndex(0, 0)] = b.gainDb;
            p[peqQIndex(0, 0)] = normFromQ(b.q);
            double off = 0.0;
            for (double hz : {3000.0, 6000.0, 9000.0, 12000.0, 15000.0})
                off = std::max(off, std::abs(through(p, hz) - 20.0 * std::log10(peqResponse(
                                                                         hz, b.type, normFromCutoff(b.hz), b.gainDb,
                                                                         normFromQ(b.q)))));
            farthest = std::max(farthest, off);
        }
        std::printf("      parametric EQ bands up high: off their drawn curves by %.2f dB at most\n", farthest);
        check(farthest < 1.25, "a parametric EQ's bands sound as drawn up to 15 kHz");

        // A delay's echoes keep their top: a 10 kHz burst, at a time between two samples (214.3 ms), its sixth echo at
        // Feedback 50 % the fifth power of a half under its first (-30.1 dB), within 0.5 dB.
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
        fx.prepare(kSampleRate);
        ParamValues p = defaults();
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
        chain(p, {0});
        p[paramIndex(0, FxParam::DelaySync)] = 0.0;
        p[paramIndex(0, FxParam::DelayTimeL)] = p[paramIndex(0, FxParam::DelayTimeR)] = normFromTime(0.2143);
        p[paramIndex(0, FxParam::DelayFeedback)] = 0.5;
        p[paramIndex(0, FxParam::DelayTone)] = 0.0;
        p[paramIndex(0, FxParam::DelayMix)] = 1.0;
        constexpr uint32_t kPiece = 64;
        float left[kPiece], right[kPiece];
        std::vector<float> out;
        const size_t burst = static_cast<size_t>(0.004 * kSampleRate);
        for (size_t t = 0; t < static_cast<size_t>(1.5 * kSampleRate); t += kPiece) {
            for (uint32_t s = 0; s < kPiece; ++s) {
                const size_t i = t + s;
                const double window = i < burst ? std::sin(std::numbers::pi * i / burst) : 0.0;
                left[s] = right[s] = static_cast<float>(
                    0.5 * window * window * std::sin(2.0 * std::numbers::pi * 10000.0 * i / kSampleRate));
            }
            fx.process(left, right, kPiece, p, 120.0, 60.0);
            out.insert(out.end(), left, left + kPiece);
        }
        auto echo = [&](int k) { // its energy, around where it comes
            const double at = k * 0.2143 * kSampleRate;
            double energy = 0.0;
            for (size_t i = static_cast<size_t>(at - 0.002 * kSampleRate); i < static_cast<size_t>(at + 0.008 * kSampleRate);
                 ++i)
                energy += static_cast<double>(out[i]) * out[i];
            return energy;
        };
        const double sixth = 10.0 * std::log10(echo(6) / echo(1));
        std::printf("      a delay's sixth echo of a 10 kHz burst: %.2f dB under its first (-30.10 without loss)\n",
                    sixth);
        check(std::abs(sixth - 20.0 * std::log10(std::pow(0.5, 5))) < 0.5, "a delay's echoes keep their top");
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
        // The utility's Mono Bass turned on from Off and off again fades between the sides as they are and
        // high-passed rather than clicking: at 500 Hz on a C3 panned hard left, which it takes half the left side of,
        // in single frames, under 4 times as rough as the note before it.
        Engine e(2);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Utility);
        chain(e.p, {0});
        e.p[paramIndex(0, OpParam::Pan)] = -1.0;
        e.noteOn(48);
        const std::vector<float> before = e.render(4800, 1);
        e.p[paramIndex(0, FxParam::UtilityMonoBass)] = normFromCutoff(500.0);
        const std::vector<float> on = e.render(2400, 1);
        e.p[paramIndex(0, FxParam::UtilityMonoBass)] = 0.0;
        const std::vector<float> all = joined(joined(before, on), e.render(2400, 1));
        const double steady = roughness(all, 2400, before.size());
        const size_t switchedOff = before.size() + on.size();
        const double switched = std::max(roughness(all, before.size() - 2, before.size() + 960),
                                         roughness(all, switchedOff - 2, switchedOff + 960));
        double level = 0.0, dryLevel = 0.0;
        for (size_t i = before.size() + 960; i < switchedOff; ++i)
            level = std::max(level, static_cast<double>(std::abs(all[i])));
        for (size_t i = 2400; i < before.size(); ++i)
            dryLevel = std::max(dryLevel, static_cast<double>(std::abs(all[i])));
        std::printf("      utility mono bass switched on and off: %.2fx as rough as the note, the left side x%.3f\n",
                    switched / steady, level / dryLevel);
        check(switched < 4.0 * steady && std::abs(level / dryLevel - 0.5) < 0.05,
              "the utility's Mono Bass switched on and off fades in and out without a click");
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
        // A flanger through zero at Mix 0 (the note as it is, 6.5 ms late) switched off and on, and put on a split's
        // high band, the low band's wire then waiting as long: what fades in waits for its wait to fill rather than
        // fading into a gap. On a D#3, whose period is about as long as the wait, so the note through the low band's
        // new pad is in step with itself as it was.
        auto flanger = [&](ParamValues &p, int slot) {
            p[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Flanger);
            p[paramIndex(slot, FxParam::FlangerMix)] = 0.0;
            p[paramIndex(slot, FxParam::FlangerThroughZero)] = 1.0;
        };
        auto flanged = [&](ParamValues &p) {
            line(p);
            flanger(p, 0);
        };
        const bool waitedOff = steadyEdit("a flanger through zero switched off", flanged, [&](ParamValues &p) {
            p[paramIndex(0, FxParam::Enabled)] = 0.0;
        });
        const bool waitedOn = steadyEdit(
            "a flanger through zero switched on",
            [&](ParamValues &p) {
                flanged(p);
                p[paramIndex(0, FxParam::Enabled)] = 0.0;
            },
            [&](ParamValues &p) { p[paramIndex(0, FxParam::Enabled)] = 1.0; });
        const bool waitedBand = steadyEdit(
            "a flanger through zero put on a split's high band", split, [&](ParamValues &p) { flanger(p, 0); }, 51);
        check(waitedOff && waitedOn && waitedBand,
              "a flanger through zero switched or put on a band fades in only once its wait has filled");
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        // at unity gain: each sample comes out as the curve has it, through the DC blocker every drawn curve has, a few
        // samples late (the shaper runs at twice the rate, through half-band filters up and down, which a 50 Hz sine
        // passes at its level, delayed).
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        std::vector<double> drawn, out; // the curve's, through a blocker; what came out
        double dcIn = 0.0, dcOut = 0.0;
        const double pole = clipBlockerPole(kSampleRate);
        for (size_t t = 0; t < static_cast<size_t>(kSampleRate); t += kBlock) {
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double phase = 2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate;
                left[s] = right[s] = static_cast<float>(0.01 * std::sin(phase));
            }
            const std::vector<float> in = left;
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double shaped = shaperCurveValue(get, ClipMode::Custom, curve, true, in[s]);
                drawn.push_back(shaped - dcIn + pole * dcOut);
                dcIn = shaped;
                dcOut = drawn.back();
                out.push_back(left[s]);
            }
        }
        // Off the curve by the least of any delay, in half samples (between two, their mean).
        double worst = 1e9;
        for (int half = 0; half <= 24; ++half) {
            const size_t whole = static_cast<size_t>(half / 2);
            double off = 0.0;
            for (size_t i = 480 + whole + 1; i < out.size(); ++i) { // once faded in
                const double late = half % 2 ? 0.5 * (drawn[i - whole] + drawn[i - whole - 1]) : drawn[i - whole];
                off = std::max(off, std::abs(out[i] - late));
            }
            worst = std::min(worst, off);
        }
        std::printf("      drawn shaper curve, a -40 dB 50 Hz sine: off the curve by %.0f dB at most\n",
                    20.0 * std::log10(worst + 1e-30));
        check(worst < 1e-4, "a drawn shaper curve passes a quiet, slow sine as drawn, without spikes");
    }
    {
        // A drawn curve's points moved (here every one halfway down to the middle, the curve halved): the curve
        // glides there rather than stepping what comes out, in whole blocks and in single frames.
        auto roughMove = [&defaults](uint32_t piece) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
        asBeforeUnisonPan(p);
        asBeforeKeyPlay(p);
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
        // Up to 0.3 (version 2), the envelopes' times and holds reached 0 to 1; since 0.4 (version 4) their attacks,
        // decays and releases kMinEnvNorm to kMaxEnvNorm and their holds 0 to kMaxEnvNorm, and a depth is a share of
        // its target's range: a version 2 state's depths load as deep as swings them as far as they swung, a version 3
        // state's (its times 0 to kMaxEnvNorm, holds as now) on the times alone, and a version 4 state's as saved.
        ParamValues p = defaults();
        p[slotTargetIndex(0, 0)] = static_cast<double>(idOfIndex(paramIndex(0, OpParam::Attack))) + 1.0;
        p[slotAmountIndex(0, 0)] = 0.5;
        p[slotTargetIndex(0, 1)] = static_cast<double>(idOfIndex(paramIndex(0, OpParam::Hold))) + 1.0;
        p[slotAmountIndex(0, 1)] = 0.5;
        p[slotTargetIndex(0, 2)] = static_cast<double>(idOfIndex(paramIndex(1, SourceParam::Release))) + 1.0;
        p[slotAmountIndex(0, 2)] = -0.4;
        p[slotTargetIndex(0, 3)] = static_cast<double>(idOfIndex(paramIndex(GlobalParam::Volume))) + 1.0;
        p[slotAmountIndex(0, 3)] = 0.3;
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
        const ParamValues two = load(2), three = load(3), four = load(4);
        const double times = kMaxEnvNorm - kMinEnvNorm;
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-12; };
        const bool fromTwo = near(two[slotAmountIndex(0, 0)], 0.5 / times) &&
                             near(two[slotAmountIndex(0, 1)], 0.5 / kMaxEnvNorm) &&
                             near(two[slotAmountIndex(0, 2)], -0.4 / times) && two[slotAmountIndex(0, 3)] == 0.3;
        const bool fromThree = near(three[slotAmountIndex(0, 0)], 0.5 * kMaxEnvNorm / times) &&
                               three[slotAmountIndex(0, 1)] == 0.5 && three[slotAmountIndex(0, 3)] == 0.3;
        const bool fromFour = four[slotAmountIndex(0, 0)] == 0.5 && four[slotAmountIndex(0, 1)] == 0.5 &&
                              four[slotAmountIndex(0, 2)] == -0.4;
        std::printf("      envelope depths from version 2: attack %.4f, hold %.4f (0.5 saved); version 3: %.4f, %.4f\n",
                    two[slotAmountIndex(0, 0)], two[slotAmountIndex(0, 1)], three[slotAmountIndex(0, 0)],
                    three[slotAmountIndex(0, 1)]);
        check(fromTwo && fromThree && fromFour,
              "a state up to 0.3 loads its depths on envelope times and holds as far as they swung");
    }
    {
        // Before version 11, a Random source took a value for each note and held it, and an LFO could have the Random
        // shape. The source loads as a random LFO standing still (unsynced at 0 Hz, on its own clock in each voice, the
        // LFO's fields it now uses as they start but for those), its polarity kept; an LFO with the Random shape as a
        // Random source running as it ran (its rate, clock and chaos kept, Bipolar standing for its Unipolar), its shape
        // and Unipolar back at their defaults; another LFO as it was. A version 11 state loads as saved.
        ParamValues p = defaults();
        auto set = [&p](int place, SourceParam f, double v) { p[paramIndex(place, f)] = v; };
        set(0, SourceParam::Type, static_cast<double>(SourceType::Random));
        set(0, SourceParam::Free, 1.0); // left by an LFO the place held, which the Random source ignored
        set(0, SourceParam::RandomSmooth, 0.5);
        set(0, SourceParam::Bipolar, 1.0);
        for (int place : {1, 2}) {
            set(place, SourceParam::Type, static_cast<double>(SourceType::Lfo));
            set(place, SourceParam::Shape, static_cast<double>(LfoShape::Random));
            set(place, SourceParam::Unipolar, place == 1 ? 1.0 : 0.0);
            set(place, SourceParam::Sync, 0.0);
            set(place, SourceParam::Rate, normFromRate(3.0));
            set(place, SourceParam::Anchor, 1.0);
            set(place, SourceParam::RandomChaos, 0.3);
        }
        set(3, SourceParam::Type, static_cast<double>(SourceType::Lfo));
        set(3, SourceParam::Shape, static_cast<double>(LfoShape::Saw));
        // An LFO at Rate 0, its slowest then (0.02 Hz), where 0 now stands still: it loads just above, as it played.
        set(4, SourceParam::Type, static_cast<double>(SourceType::Lfo));
        set(4, SourceParam::Sync, 0.0);
        set(4, SourceParam::Rate, 0.0);
        asBeforeUnisonPan(p);
        asBeforeKeyPlay(p);
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
        const ParamValues before = load(10), now = load(11);
        auto at = [&before](int place, SourceParam f) { return before[paramIndex(place, f)]; };
        auto def = [](int place, SourceParam f) { return paramTable()[paramIndex(place, f)].def; };
        const double random = static_cast<double>(SourceType::Random);
        const bool source = at(0, SourceParam::Type) == random && at(0, SourceParam::Sync) == 0.0 &&
                            at(0, SourceParam::Rate) == 0.0 && at(0, SourceParam::Free) == 0.0 &&
                            at(0, SourceParam::RandomSmooth) == def(0, SourceParam::RandomSmooth) &&
                            at(0, SourceParam::Bipolar) == 1.0;
        bool lfos = true;
        for (int place : {1, 2})
            lfos = lfos && at(place, SourceParam::Type) == random &&
                   at(place, SourceParam::Bipolar) == (place == 1 ? 0.0 : 1.0) &&
                   at(place, SourceParam::Rate) == normFromRate(3.0) && at(place, SourceParam::Sync) == 0.0 &&
                   at(place, SourceParam::Anchor) == 1.0 && at(place, SourceParam::RandomChaos) == 0.3 &&
                   at(place, SourceParam::Shape) == def(place, SourceParam::Shape) &&
                   at(place, SourceParam::Unipolar) == 0.0;
        const bool plain = at(3, SourceParam::Type) == static_cast<double>(SourceType::Lfo) &&
                           at(3, SourceParam::Shape) == static_cast<double>(LfoShape::Saw);
        const bool slowest = std::abs(sweepRateHz(at(4, SourceParam::Rate)) - rateFromNorm(0.0)) < 1e-6 &&
                             now[paramIndex(4, SourceParam::Rate)] == 0.0;
        check(source, "a state before version 11 loads its Random source as one standing still, taking a value a note");
        check(lfos && plain, "a state before version 11 loads its random LFO as a Random source running as it ran");
        check(slowest, "a state before version 11 loads an LFO at its slowest Rate playing as slowly, not standing still");
        check(std::equal(now.begin(), now.end(), p.begin()), "a version 11 state loads its sources as saved");
    }
    {
        // Before version 12, an LFO could have the Custom shape: it loads as a Curve source running as it ran, its
        // fields, points and polarity kept and its Shape back at its default; another LFO as it was. A version 12
        // state loads as saved.
        ParamValues p = defaults();
        auto set = [&p](int place, SourceParam f, double v) { p[paramIndex(place, f)] = v; };
        for (int place : {0, 1}) {
            set(place, SourceParam::Type, static_cast<double>(SourceType::Lfo));
            set(place, SourceParam::Shape, static_cast<double>(LfoShape::Custom));
            set(place, SourceParam::Unipolar, place == 0 ? 1.0 : 0.0);
            set(place, SourceParam::Sync, 0.0);
            set(place, SourceParam::Rate, normFromRate(4.0));
            set(place, SourceParam::Phase, 0.25);
            p[shapeCountIndex(place)] = 3.0;
            p[shapePointIndex(place, 2, PointField::Y)] = 0.6;
        }
        set(2, SourceParam::Type, static_cast<double>(SourceType::Lfo));
        set(2, SourceParam::Shape, static_cast<double>(LfoShape::Triangle));
        asBeforeUnisonPan(p);
        asBeforeKeyPlay(p);
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
        const ParamValues before = load(11), now = load(12);
        auto at = [&before](int place, SourceParam f) { return before[paramIndex(place, f)]; };
        bool curves = true;
        for (int place : {0, 1})
            curves = curves && at(place, SourceParam::Type) == static_cast<double>(SourceType::Curve) &&
                     at(place, SourceParam::Shape) == paramTable()[paramIndex(place, SourceParam::Shape)].def &&
                     at(place, SourceParam::Unipolar) == (place == 0 ? 1.0 : 0.0) &&
                     at(place, SourceParam::Rate) == normFromRate(4.0) && at(place, SourceParam::Sync) == 0.0 &&
                     at(place, SourceParam::Phase) == 0.25 && before[shapeCountIndex(place)] == 3.0 &&
                     before[shapePointIndex(place, 2, PointField::Y)] == 0.6;
        const bool plain = at(2, SourceParam::Type) == static_cast<double>(SourceType::Lfo) &&
                           at(2, SourceParam::Shape) == static_cast<double>(LfoShape::Triangle);
        check(curves && plain, "a state before version 12 loads its Custom LFO as a Curve source running as it ran");
        check(std::equal(now.begin(), now.end(), p.begin()), "a version 12 state loads its LFOs as saved");
    }
    {
        // Before version 14, a pattern's curves held their ends: a version 13 state's load with points at the pattern's
        // start and end playing as they played, a point past the end shaping the way to it as it did; a version 14 state's
        // load as saved, wrapping around.
        ParamValues p = defaults();
        auto put = [&p](int pattern, SeqCurve curve, std::vector<std::pair<double, double>> points) {
            p[seqCurveCountIndex(pattern, curve)] = static_cast<double>(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                p[seqCurveIndex(pattern, curve, static_cast<int>(i), PointField::X)] = points[i].first;
                p[seqCurveIndex(pattern, curve, static_cast<int>(i), PointField::Y)] = points[i].second;
            }
        };
        put(0, SeqCurve::Bend, {{4.0, 0.5}, {12.0, -0.5}});
        put(1, SeqCurve::ModWheel, {{8.0, 0.25}, {24.0, 0.75}});
        asBeforeKeyPlay(p);
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
        auto at = [](const ParamValues &v, int pattern, SeqCurve curve, double step) {
            auto get = [&v](int index) { return v[index]; };
            return seqCurveAt(seqCurvePointCount(get, pattern, curve), [&](int i) {
                return SeqCurvePoint{v[seqCurveIndex(pattern, curve, i, PointField::X)],
                                     v[seqCurveIndex(pattern, curve, i, PointField::Y)],
                                     v[seqCurveIndex(pattern, curve, i, PointField::Curve)]};
            }, step, 16.0);
        };
        const ParamValues before = load(13), now = load(14);
        const bool held = std::abs(at(before, 0, SeqCurve::Bend, 1.0) - 0.5) < 1e-9 &&
                          std::abs(at(before, 0, SeqCurve::Bend, 8.0)) < 1e-9 &&
                          std::abs(at(before, 0, SeqCurve::Bend, 15.5) + 0.5) < 1e-9 &&
                          std::abs(at(before, 1, SeqCurve::ModWheel, 2.0) - 0.25) < 1e-9 &&
                          std::abs(at(before, 1, SeqCurve::ModWheel, 12.0) - 0.375) < 1e-9 &&
                          std::abs(at(before, 1, SeqCurve::ModWheel, 16.0) - 0.5) < 1e-9;
        check(held, "a state before version 14 loads its sequencer curves playing as they played, holding their ends");
        check(std::equal(now.begin(), now.end(), p.begin()) && std::abs(at(now, 0, SeqCurve::Bend, 0.0)) < 1e-9,
              "a version 14 state loads its sequencer curves as saved, wrapping around the pattern's end");
    }
    {
        // Before version 15, the Sync warp reached 8 times a cycle rather than 16: a version 14 state's amount on a
        // Sync warp and the depth on it load 7/15 as large, playing as many times through a cycle as they did, a
        // Squeeze's and the depth on it as saved; a version 15 state's as saved.
        ParamValues p = defaults();
        const int sync = paramIndex(0, OpParam::WarpAmount), squeeze = paramIndex(1, OpParam::WarpAmount);
        p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Sync);
        p[paramIndex(1, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
        p[sync] = 0.75;
        p[squeeze] = 0.75;
        p[slotTargetIndex(0, 0)] = slotTargetValue(sync);
        p[slotAmountIndex(0, 0)] = 0.3;
        p[slotTargetIndex(0, 1)] = slotTargetValue(squeeze);
        p[slotAmountIndex(0, 1)] = 0.3;
        asBeforeKeyPlay(p);
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
        const ParamValues before = load(14), now = load(15);
        auto retiredCycles = [](double amount) { return 1.0 + (kRetiredMaxSyncWarp - 1.0) * std::abs(amount); };
        const bool scaled = std::abs(syncWarpCycles(before[sync]) - retiredCycles(0.75)) < 1e-9 &&
                            std::abs(syncWarpCycles(before[sync] + before[slotAmountIndex(0, 0)]) -
                                     retiredCycles(0.75 + 0.3)) < 1e-9 &&
                            before[squeeze] == 0.75 && before[slotAmountIndex(0, 1)] == 0.3;
        check(scaled, "a state before version 15 loads its Sync warp's amount and depth playing as they did");
        check(std::equal(now.begin(), now.end(), p.begin()), "a version 15 state loads its warps as saved");
    }
    {
        // Before version 17, the keys transposed a pattern from Root rather than C4: a version 16 state in Key with
        // Root on D4 loads 2 semitones down in Transpose, and one on G1 with Transpose at +7 29 semitones up (Transpose at
        // +24, Octave an octave up), each key playing what it played; one with the host's transport, which Root
        // never moved, as saved, and a version 17 state as saved. (Each state holds every param, as the plugin saves
        // one: Key is Play's default now, which a sparse one leaves out.) And Play to begin with was Host before version
        // 17: a sparse version 16 state, holding no Play, loads Host, a version 17 one Key.
        const int play = paramIndex(SeqParam::Play), root = paramIndex(SeqParam::Root);
        const int octave = paramIndex(SeqParam::Octave), transpose = paramIndex(SeqParam::Transpose);
        auto load = [](const ParamValues &p, uint32_t version, bool sparse = false) {
            std::vector<unsigned char> bytes;
            const clap_ostream_t out = memoryOut(bytes);
            writePatch(&out, p, gui::UiLayout{}, sparse);
            std::memcpy(bytes.data() + sizeof(uint32_t), &version, sizeof(version)); // after the magic
            MemoryReader reader{bytes.data(), bytes.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            ParamValues loaded;
            readParams(&in, loaded);
            return loaded;
        };
        ParamValues p = defaults();
        p[play] = static_cast<double>(SeqPlay::Key);
        p[root] = 62.0;
        const ParamValues d = load(p, 16);
        p[root] = 31.0;
        p[transpose] = 7.0;
        const ParamValues g = load(p, 16), now = load(p, 17);
        p[play] = static_cast<double>(SeqPlay::Host);
        const ParamValues host = load(p, 16);
        p[play] = static_cast<double>(SeqPlay::Key);
        auto shift = [&](const ParamValues &v) { return 12.0 * v[octave] + v[transpose]; };
        check(d[transpose] == -2.0 && d[octave] == 0.0 && d[root] == 62.0 && g[transpose] == 24.0 &&
                  g[octave] == 1.0 && shift(g) == 7.0 + 29.0 && host[transpose] == 7.0 && host[octave] == 0.0,
              "a state before version 17 playing from a key loads the Root it transposed from into Transpose");
        check(std::equal(now.begin(), now.end(), p.begin()), "a version 17 state loads its sequencer as saved");
        const ParamValues unsaid = load(defaults(), 16, true), keyed = load(defaults(), 17, true);
        check(unsaid[play] == static_cast<double>(SeqPlay::Host) && keyed[play] == static_cast<double>(SeqPlay::Key),
              "a state before version 17 without the sequencer's Play loads Host, its default then, and a later one Key");
        // Before version 18, a fixed operator's Pitch and Detune did nothing: a version 17 state's fixed A loads with
        // them at 0 and LFO 1's slot on its Pitch modulating nothing, operator B's (not fixed) as saved; a version 18
        // state as saved.
        ParamValues f = defaults();
        f[paramIndex(0, OpParam::Fixed)] = 1.0;
        for (int op : {0, 1}) {
            f[paramIndex(op, OpParam::Transpose)] = 7.0;
            f[paramIndex(op, OpParam::Detune)] = 20.0;
            f[slotTargetIndex(0, op)] = slotTargetValue(paramIndex(op, OpParam::Transpose));
            f[slotAmountIndex(0, op)] = 0.3;
        }
        const ParamValues before = load(f, 17), after = load(f, 18);
        check(before[paramIndex(0, OpParam::Transpose)] == 0.0 && before[paramIndex(0, OpParam::Detune)] == 0.0 &&
                  before[slotTargetIndex(0, 0)] == 0.0 && before[slotAmountIndex(0, 0)] == 0.0 &&
                  before[paramIndex(1, OpParam::Transpose)] == 7.0 && before[slotAmountIndex(0, 1)] == 0.3,
              "a state before version 18 loads a fixed operator's Pitch and Detune, which did nothing, at 0");
        check(std::equal(after.begin(), after.end(), f.begin()), "a version 18 state loads a fixed operator as saved");
        // And an operator's feedback mode, its route into itself's mode param, held nothing before version 18: a
        // version 17 state holding 1 there (as a host could set it) loads Smooth, a version 18 state DX7.
        ParamValues fb = defaults();
        fb[feedbackModeIndex(2)] = static_cast<double>(FeedbackMode::Dx7);
        const ParamValues fbBefore = load(fb, 17), fbAfter = load(fb, 18);
        check(fbBefore[feedbackModeIndex(2)] == static_cast<double>(FeedbackMode::Smooth) &&
                  fbAfter[feedbackModeIndex(2)] == static_cast<double>(FeedbackMode::Dx7),
              "a state before version 18 loads every feedback Smooth, whatever its mode param held");
    }
    {
        // The places past the first 16: a patch with an LFO in the last place on operator A's level, the 17th place's
        // aux Macro 3 and Macro 3's aux the 17th place, its drawn shape and its slot's curve, loads back as saved, the
        // auxes stored by the numbers sources are stored by (the macros' as before the later places came, the later
        // places' after the operators); and so does the editor's layout, the later places' order and folds at its end
        // and an empty modulation's source likewise.
        ParamValues p = defaults();
        const int last = kMaxSources - 1, later = kFirstSources, macro = kMaxSources + 2;
        p[paramIndex(last, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
        p[paramIndex(later, SourceParam::Type)] = static_cast<double>(SourceType::Env);
        p[slotTargetIndex(last, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
        p[slotAmountIndex(last, 0)] = -0.5;
        p[slotCurveIndex(last, 0)] = 0.25;
        p[slotTargetIndex(later, 0)] = slotTargetValue(paramIndex(0, OpParam::Pan));
        p[slotAmountIndex(later, 0)] = 0.4;
        p[slotAuxIndex(later, 0)] = slotAuxValue(macro);
        p[slotTargetIndex(macro, 0)] = slotTargetValue(paramIndex(1, OpParam::Out));
        p[slotAmountIndex(macro, 0)] = 0.2;
        p[slotAuxIndex(macro, 0)] = slotAuxValue(later);
        p[shapeCountIndex(last)] = 3.0;
        p[shapePointIndex(last, 2, PointField::Y)] = -0.75;
        gui::UiLayout ui;
        std::reverse(ui.sourceOrder.begin(), ui.sourceOrder.end());
        ui.sourcesFolded = 1u | 1u << last | 1u << later;
        ui.emptyMods = {{later, -1}, {macro, paramIndex(0, OpParam::Detune)}, {-1, -1}};
        ui.hiddenNodes = 1u << 1 | 1u << 5 | 1u << (kOps + kFilters); // B, F and S1 hidden from the graph while off
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, ui, true);
        MemoryReader reader{bytes.data(), bytes.size(), 0};
        const clap_istream_t in = memoryIn(reader);
        ParamValues loaded;
        const bool read = readParams(&in, loaded);
        const gui::UiLayout back = readLayout(&in);
        auto storedAs = [&](int index) { // the value the sparse patch holds for the param
            uint32_t count = 0;
            std::memcpy(&count, bytes.data() + 2 * sizeof(uint32_t), sizeof(count));
            const uint32_t id = idOfIndex(index);
            for (uint32_t k = 0; k < count; ++k) {
                const unsigned char *at = bytes.data() + 3 * sizeof(uint32_t) + k * (sizeof(uint32_t) + sizeof(double));
                uint32_t got;
                std::memcpy(&got, at, sizeof(got));
                if (got == id) {
                    double v;
                    std::memcpy(&v, at + sizeof(got), sizeof(v));
                    return v;
                }
            }
            return -1.0;
        };
        std::printf("      the 17th place's aux on Macro 3 stored as %.0f, Macro 3's on the 17th place as %.0f\n",
                    storedAs(slotAuxIndex(later, 0)), storedAs(slotAuxIndex(macro, 0)));
        check(read && std::equal(loaded.begin(), loaded.end(), p.begin()) && storedAs(slotAuxIndex(later, 0)) == 19.0 &&
                  storedAs(slotAuxIndex(macro, 0)) == 35.0,
              "a patch using the places past the 16th loads back as saved, its auxes stored by the sources' stored numbers");
        check(back.sourceOrder == ui.sourceOrder && back.sourcesFolded == ui.sourcesFolded && back.emptyMods == ui.emptyMods,
              "the editor's layout keeps the later places' order and folds, and an empty modulation's source");
        check(back.hiddenNodes == ui.hiddenNodes, "the editor's layout keeps the nodes hidden from the routing graph");
    }
    {
        // The filter effect switched to another type or slope and back: the stage it left out starts from silence,
        // rather than playing back what it held then.
        auto stale = [&](FilterType first, int firstSlope, FilterType second, int secondSlope) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
        const float ladder = stale(FilterType::Ladder, 3, FilterType::LowPass, 1);
        // The newer types, back from a low-pass, and the analog types, which share their stages, back from one another,
        // and from a morph.
        float newer = 0.0f;
        for (FilterType t : {FilterType::Diode, FilterType::SallenKeyLow, FilterType::SallenKeyHigh, FilterType::Formant,
                             FilterType::Morph, FilterType::LadderHigh, FilterType::LadderBand})
            newer = std::max(newer, stale(t, 1, FilterType::LowPass, 1));
        newer = std::max({newer, stale(FilterType::Diode, 3, FilterType::Ladder, 3),
                          stale(FilterType::Morph, 1, FilterType::SallenKeyLow, 1),
                          stale(FilterType::Ladder, 3, FilterType::Morph, 1),
                          stale(FilterType::LadderHigh, 3, FilterType::Ladder, 3),
                          stale(FilterType::Ladder, 3, FilterType::LadderBand, 3)});
        std::printf("      filter effect back from another type: low-pass %.1g, 24 dB %.1g, comb %.1g, ladder %.1g, the "
                    "newer types %.1g\n",
                    comb, slope, back, ladder, newer);
        check(comb < 1e-4f && slope < 1e-4f && back < 1e-4f && ladder < 1e-4f && newer < 1e-4f,
              "the filter effect switched to another type or slope and back plays nothing it held before");
    }
    {
        // A plate (six diffusers a side) switched to a hall (four) and back, after the hall's tail: silence.
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
        // Notes held in poly as mono turns on: a key let go doesn't move its voice onto a key the mono voice already
        // plays (which two voices would then play, one of them taken over without its end reported), and every note
        // ends once, through a lower voice limit and a later note too.
        Engine e(2, 2);
        e.synth().setPlayMode(false, false, false);
        int ends[10] = {};
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        e.synth().noteOn({2, 0, 0, 62}, 1.0);
        play(e, 4, ends);
        e.synth().setPlayMode(true, false, false);
        e.synth().noteOn({3, 0, 0, 64}, 1.0);
        play(e, 4, ends);
        e.synth().noteOff({1, 0, 0, 60});
        play(e, 4, ends);
        const bool playing3 = e.synth().isActive({3, 0, 0, 64});
        e.synth().setVoiceLimit(1);
        play(e, 10, ends);
        e.synth().noteOn({4, 0, 0, 65}, 1.0);
        play(e, 4, ends);
        e.synth().noteOff({-1, -1, -1, -1});
        play(e, 800, ends);
        std::printf("      poly to mono with keys held: ends %d %d %d %d, %d voice(s) left\n", ends[1], ends[2], ends[3],
                    ends[4], e.synth().activeVoiceCount());
        check(playing3 && ends[1] == 1 && ends[2] == 1 && ends[3] == 1 && ends[4] == 1 &&
                  e.synth().activeVoiceCount() == 0,
              "notes held as mono turns on, a key let go, a lower voice limit and a later note: each ends once");
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
        // An operator's output as a source: B, fixed and heard by nothing, on A's Out, set at half. The level A's note
        // peaks at, window by window (two of its periods each), from once B's envelope has settled.
        const int b = 1, source = kOpSource + b, aOut = paramIndex(0, OpParam::Out);
        auto setUp = [&](Engine &e, double hz, double sustain) {
            e.p[aOut] = 0.5;
            e.p[paramIndex(b, OpParam::Enabled)] = 1.0;
            e.p[paramIndex(b, OpParam::Fixed)] = 1.0;
            e.p[paramIndex(b, OpParam::FixedFreq)] = normFromFixedFreq(hz > kMaxFixedFreq ? hz / 10.0 : hz);
            e.p[paramIndex(b, OpParam::FixedMult)] = hz > kMaxFixedFreq ? 4.0 : 3.0; // x10, x1
            e.p[paramIndex(b, OpParam::Decay)] = normFromTime(0.02);
            e.p[paramIndex(b, OpParam::Sustain)] = sustain;
        };
        struct Range {
            float low = 1e9f, high = 0.0f;
        };
        auto levels = [](Engine &e) {
            e.noteOn(69);
            const std::vector<float> out = e.render(static_cast<size_t>(kSampleRate));
            Range r;
            constexpr size_t kWindow = 240;
            for (size_t at = out.size() / 4; at + kWindow <= out.size(); at += kWindow) {
                float peak = 0.0f;
                for (size_t i = at; i < at + kWindow; ++i)
                    peak = std::max(peak, std::abs(out[i]));
                r.low = std::min(r.low, peak);
                r.high = std::max(r.high, peak);
            }
            return r;
        };
        auto modulated = [&](double hz, double sustain, double depth, bool bipolar) {
            Engine e(2);
            setUp(e, hz, sustain);
            e.p[slotTargetIndex(source, 0)] = slotTargetValue(aOut);
            e.p[slotAmountIndex(source, 0)] = depth;
            e.p[paramIndex(b, OpSourceParam::Bipolar)] = bipolar ? 1.0 : 0.0;
            return levels(e);
        };
        Engine plainEngine(2);
        setUp(plainEngine, 5.0, 1.0);
        const Range plain = levels(plainEngine);
        const Range slow = modulated(5.0, 1.0, 1.0, true);
        const Range fast = modulated(12000.0, 1.0, 1.0, true);
        const Range lifted = modulated(5.0, 1.0, 0.5, false);
        const Range silent = modulated(5.0, 0.0, 0.5, false);
        Engine offEngine(2);
        setUp(offEngine, 5.0, 1.0);
        offEngine.p[paramIndex(b, OpParam::Enabled)] = 0.0;
        offEngine.p[slotTargetIndex(source, 0)] = slotTargetValue(aOut);
        offEngine.p[slotAmountIndex(source, 0)] = 1.0;
        const Range off = levels(offEngine);
        // As an aux: a macro taking A's Out away, scaled by a B that is silent, leaves it as it was.
        auto macroOn = [&](bool aux) {
            Engine e(2);
            setUp(e, 5.0, 0.0);
            e.p[paramIndex(0, MacroParam::Value)] = 1.0;
            e.p[slotTargetIndex(kMaxSources, 0)] = slotTargetValue(aOut);
            e.p[slotAmountIndex(kMaxSources, 0)] = -1.0;
            if (aux)
                e.p[slotAuxIndex(kMaxSources, 0)] = slotAuxValue(source);
            return levels(e);
        };
        const Range unscaled = macroOn(false), scaled = macroOn(true);
        std::printf("      A's peak %.3f set; under a 5 Hz operator %.3f to %.3f, a 12 kHz one %.3f to %.3f, unipolar "
                    "%.3f to %.3f, silent %.3f, switched off %.3f; a macro's %.4f, with a silent operator as its aux "
                    "%.3f\n",
                    plain.high, slow.low, slow.high, fast.low, fast.high, lifted.low, lifted.high, silent.high, off.high,
                    unscaled.high, scaled.high);
        check(slow.high > 1.8f * plain.high && slow.low < 0.2f * plain.high,
              "an operator at 5 Hz swings what it modulates both ways with its output");
        check(fast.low > 1.8f * plain.high,
              "an operator at 12 kHz moves what it modulates every sample, not averaging away");
        check(lifted.high > 1.8f * plain.high && lifted.low > 0.95f * plain.high && lifted.low < 1.1f * plain.high,
              "a unipolar operator takes what it modulates one way, from nothing up to its depth");
        check(std::abs(silent.high - plain.high) < 0.01f * plain.high && off.high == plain.high,
              "a silent operator, unipolar, and one switched off move nothing");
        check(unscaled.high < 1e-3f && std::abs(scaled.high - plain.high) < 0.01f * plain.high,
              "a silent operator as an aux scales its modulation to nothing");
    }
    {
        // Audio-rate modulation: B, fixed at 3 kHz and heard by nothing, on a param A's 440 Hz plays by. B goes through
        // two whole cycles a control block, so a param that took it by the block would put out none of its sidebands
        // around 3 kHz.
        const int a = 0, b = 1, c = 2, source = kOpSource + b;
        auto setUp = [&](ParamValues &p, double hz, int target, double depth) {
            p[paramIndex(b, OpParam::Enabled)] = 1.0;
            p[paramIndex(b, OpParam::Fixed)] = 1.0;
            p[paramIndex(b, OpParam::FixedFreq)] = normFromFixedFreq(hz > kMaxFixedFreq ? hz / 10.0 : hz);
            p[paramIndex(b, OpParam::FixedMult)] = hz > kMaxFixedFreq ? 4.0 : 3.0; // x10, x1
            p[paramIndex(b, OpParam::Out)] = 0.0;
            p[slotTargetIndex(source, 0)] = slotTargetValue(target);
            p[slotAmountIndex(source, 0)] = depth;
        };
        auto throughFilter = [&](ParamValues &p) {
            p[paramIndex(a, OpParam::Out)] = 0.0;
            p[sendIndex(a, 0)] = 1.0;
            p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(1000.0);
            p[paramIndex(0, FilterParam::Resonance)] = 0.5;
            p[paramIndex(0, FilterParam::Out)] = 1.0;
        };
        auto throughShaper = [&](ParamValues &p) {
            p[paramIndex(a, OpParam::Out)] = 0.0;
            p[shaperSendIndex(a, 0)] = 1.0;
            p[paramIndex(0, ShaperParam::Enabled)] = 1.0;
            p[paramIndex(0, ShaperParam::Out)] = 1.0;
            p[paramIndex(0, ShaperParam::Drive)] = 0.3;
        };
        auto fromC = [&](ParamValues &p) { // C into A, at A's pitch
            p[paramIndex(c, OpParam::Enabled)] = 1.0;
            p[paramIndex(c, OpParam::Out)] = 0.0;
            p[matrixIndex(c, a)] = 0.3;
        };
        struct Case {
            const char *name;
            int target;
            double depth;
            std::function<void(ParamValues &)> patch;
            int key = 69; // the key that plays A4 as patched
        };
        const auto none = [](ParamValues &) {};
        const std::vector<Case> cases{
            {"Out", paramIndex(a, OpParam::Out), 0.5, [&](ParamValues &p) { p[paramIndex(a, OpParam::Out)] = 0.5; }},
            {"Ratio", paramIndex(a, OpParam::Ratio), 0.02, none},
            {"Pitch", paramIndex(a, OpParam::Transpose), 0.05, none},
            {"Detune", paramIndex(a, OpParam::Detune), 1.0, none},
            {"Phase", paramIndex(a, OpParam::Phase), 0.02, none},
            {"Pan", paramIndex(a, OpParam::Pan), 0.5, none},
            {"Sustain", paramIndex(a, OpParam::Sustain), 0.5,
             [&](ParamValues &p) {
                 p[paramIndex(a, OpParam::Decay)] = normFromTime(0.01);
                 p[paramIndex(a, OpParam::Sustain)] = 0.5;
             }},
            // Two copies, one each side: three's outer copies, swung opposite ways, cancel each other's sidebands.
            {"Unison Detune", paramIndex(a, OpParam::UnisonDetune), 1.0,
             [&](ParamValues &p) { p[paramIndex(a, OpParam::Unison)] = 2.0; }},
            {"Warp Amount", paramIndex(a, OpParam::WarpAmount), 0.3,
             [&](ParamValues &p) {
                 p[paramIndex(a, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
                 p[paramIndex(a, OpParam::WarpAmount)] = 0.3;
             }},
            {"Freq", paramIndex(a, OpParam::FixedFreq), 0.05,
             [&](ParamValues &p) {
                 p[paramIndex(a, OpParam::Fixed)] = 1.0;
                 p[paramIndex(a, OpParam::FixedFreq)] = normFromFixedFreq(440.0);
                 p[paramIndex(a, OpParam::FixedMult)] = 3.0;
             }},
            {"Unison Spread", paramIndex(a, OpParam::UnisonSpread), 0.5,
             [&](ParamValues &p) {
                 p[paramIndex(a, OpParam::Unison)] = 2.0;
                 p[paramIndex(a, OpParam::UnisonSpread)] = 0.5;
             }},
            {"Unison Blend", paramIndex(a, OpParam::UnisonBlend), 1.0,
             [&](ParamValues &p) {
                 p[paramIndex(a, OpParam::Unison)] = 3.0;
                 p[paramIndex(a, OpParam::UnisonBlend)] = 0.5;
             }},
            {"send", sendIndex(a, 0), 0.5,
             [&](ParamValues &p) {
                 throughFilter(p);
                 p[sendIndex(a, 0)] = 0.5;
             }},
            {"a route's depth", matrixIndex(c, a), 0.3, fromC},
            {"FM Amount", paramIndex(GlobalParam::FmAmount), 0.3, fromC},
            {"the voice's Pitch", paramIndex(GlobalParam::Pitch), 0.05, none},
            {"the voice's Pan", paramIndex(GlobalParam::Pan), 0.5, none},
            {"a filter's Morph", paramIndex(0, FilterParam::Morph), 0.5,
             [&](ParamValues &p) {
                 throughFilter(p);
                 p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Morph);
                 p[paramIndex(0, FilterParam::Morph)] = 0.5;
             }},
            {"a shaper's Output", paramIndex(0, ShaperParam::Output), 0.5, throughShaper},
            {"a filter's Resonance", paramIndex(0, FilterParam::Resonance), 0.5, throughFilter},
            {"a filter's Drive", paramIndex(0, FilterParam::Drive), 0.5, throughFilter},
            {"a filter's Out", paramIndex(0, FilterParam::Out), 0.5, throughFilter},
            {"a shaper's Drive", paramIndex(0, ShaperParam::Drive), 0.5, throughShaper},
        };
        auto sidebands = [&](const Case &k, double depth) {
            const std::vector<double> power = powerSpectrum(steadyTone(2, k.key, [&](ParamValues &p) {
                k.patch(p);
                setUp(p, 3000.0, k.target, depth);
            }));
            auto around = [&](double hz) {
                const long center = std::lround(hz * kFftSize / kSampleRate);
                double sum = 1e-30;
                for (long n = center - 6; n <= center + 6; ++n)
                    sum += power[static_cast<size_t>(n)];
                return sum;
            };
            return 10.0 * std::log10((around(2560.0) + around(3440.0)) / around(440.0));
        };
        bool moved = true, still = true;
        for (const Case &k : cases) {
            const double on = sidebands(k, k.depth), off = sidebands(k, 0.0);
            std::printf("      a 3 kHz operator on A's %s: sidebands %.1f dB (%.1f without)\n", k.name, on, off);
            moved = moved && on > -60.0;
            still = still && off < -90.0;
        }
        check(moved, "an operator moves every param a voice plays by at audio rate, its sidebands heard");
        check(still, "without its modulation, none of those sidebands");

        // B at 7040 Hz deep on the Ratio of A at 3520 Hz, which swings it an octave either way: A plays its mean pitch,
        // which the swing lifts (by the mean of 2 to the power of it), and the sidebands around it every 7040 Hz;
        // anything else has folded back.
        {
            constexpr double kDepth = 0.3;
            const double octaves = 0.5 * kDepth * std::log2(kMaxRatio / kMinRatio); // at B's peaks
            double lift = 0.0;
            constexpr int kSteps = 100000;
            for (int k = 0; k < kSteps; ++k)
                lift += std::exp2(octaves * std::sin(2.0 * std::numbers::pi * k / kSteps));
            const double carrier = 3520.0 * lift / kSteps;
            std::vector<double> lines;
            for (int n = -8; n <= 8; ++n)
                lines.push_back(std::abs(carrier + n * 7040.0));
            std::vector<double> folded;
            for (const int factor : {1, 2, 4, 8})
                folded.push_back(unexpectedDb(steadyTone(factor, 69, [&](ParamValues &p) {
                    p[paramIndex(a, OpParam::Ratio)] = normFromRatio(8.0);
                    setUp(p, 7040.0, paramIndex(a, OpParam::Ratio), kDepth);
                }), lines));
            std::printf("      an operator swinging a Ratio an octave at audio rate, its aliasing at 1x %.1f dB, "
                        "2x %.1f, 4x %.1f, 8x %.1f\n",
                        folded[0], folded[1], folded[2], folded[3]);
            check(folded[1] < -40.0 && folded[3] < folded[0],
                  "an operator's pitch moved at audio rate keeps its aliasing far below what it plays, less so "
                  "oversampled");
        }

        // A on its own Ratio, Out and Phase, and B at full depth on everything at once: finite, and ending.
        bool finite = true, ended = true;
        for (const bool self : {true, false}) {
            Engine e(2);
            if (self) {
                const int own = kOpSource + a;
                const std::array<int, 3> targets{paramIndex(a, OpParam::Ratio), paramIndex(a, OpParam::Out),
                                                 paramIndex(a, OpParam::Phase)};
                for (int s = 0; s < 3; ++s) {
                    e.p[slotTargetIndex(own, s)] = slotTargetValue(targets[static_cast<size_t>(s)]);
                    e.p[slotAmountIndex(own, s)] = 1.0;
                }
            } else {
                throughFilter(e.p);
                setUp(e.p, 3000.0, paramIndex(0, FilterParam::Resonance), 1.0);
                const std::array<int, 6> targets{paramIndex(a, OpParam::Ratio), paramIndex(a, OpParam::Pan),
                                                 paramIndex(0, FilterParam::Cutoff), paramIndex(0, FilterParam::Drive),
                                                 paramIndex(GlobalParam::Pitch), sendIndex(a, 0)};
                for (int s = 1; s <= 6; ++s) {
                    e.p[slotTargetIndex(source, s)] = slotTargetValue(targets[static_cast<size_t>(s - 1)]);
                    e.p[slotAmountIndex(source, s)] = 1.0;
                }
            }
            e.p[paramIndex(a, OpParam::Release)] = normFromTime(0.01);
            e.p[paramIndex(b, OpParam::Release)] = normFromTime(0.01);
            e.noteOn(45);
            std::vector<float> out = e.render(24000);
            e.noteOff(45);
            const std::vector<float> after = e.render(96000);
            out.insert(out.end(), after.begin(), after.end());
            for (const float x : out)
                finite = finite && std::isfinite(x) && std::abs(x) < 64.0f;
            for (size_t s = after.size() - 4800; s < after.size(); ++s)
                ended = ended && std::abs(after[s]) < 1e-3f;
            ended = ended && e.synth().activeVoiceCount() == 0;
        }
        check(finite && ended, "an operator on its own pitch, level and phase, and on many params at once at full "
                               "depth, stays finite and ends with its note");

        // B's sidebands around A's 440 Hz, against it, in dB.
        auto sidebandDb = [](const std::vector<float> &out) {
            const std::vector<double> power = powerSpectrum(out);
            auto around = [&](double hz) {
                const long center = std::lround(hz * kFftSize / kSampleRate);
                double sum = 1e-30;
                for (long n = center - 6; n <= center + 6; ++n)
                    sum += power[static_cast<size_t>(n)];
                return sum;
            };
            return 10.0 * std::log10((around(2560.0) + around(3440.0)) / around(440.0));
        };

        // B on a filter's Cutoff (filter FM) and on its Resonance, which has the filter worked out every sample, then
        // its depth on the Resonance taken to 0: the cutoff's sidebands carry on as with B on the Cutoff alone.
        auto cutoffSidebands = [&](bool resonanceToo) {
            Engine e(2);
            throughFilter(e.p);
            setUp(e.p, 3000.0, paramIndex(0, FilterParam::Cutoff), 0.3);
            if (resonanceToo) {
                e.p[slotTargetIndex(source, 1)] = slotTargetValue(paramIndex(0, FilterParam::Resonance));
                e.p[slotAmountIndex(source, 1)] = 0.3;
            }
            e.noteOn(69);
            e.render(4800);
            e.p[slotAmountIndex(source, 1)] = 0.0;
            e.render(4800);
            return sidebandDb(e.render(kFftSize));
        };
        const double alone = cutoffSidebands(false), after = cutoffSidebands(true);
        std::printf("      filter FM: sidebands %.1f dB, %.1f dB once an operator on the Resonance stops\n", alone, after);
        check(alone > -60.0 && std::abs(after - alone) < 1.0,
              "filter FM carries on once the filter's other settings stop being moved at audio rate");

        // B on the voice's Pitch, A fixed at 440 Hz (which the voice's Pitch leaves alone) through a key-tracked
        // filter: the filter follows the pitch every sample, putting B's sidebands on A; without key tracking, none.
        auto trackedSidebands = [&](double keyTrack) {
            return sidebandDb(steadyTone(2, 69, [&](ParamValues &p) {
                throughFilter(p);
                p[paramIndex(a, OpParam::Fixed)] = 1.0;
                p[paramIndex(a, OpParam::FixedFreq)] = normFromFixedFreq(440.0);
                p[paramIndex(a, OpParam::FixedMult)] = 3.0; // x1
                p[paramIndex(0, FilterParam::KeyTrack)] = keyTrack;
                setUp(p, 3000.0, paramIndex(GlobalParam::Pitch), 0.05);
            }));
        };
        const double tracked = trackedSidebands(1.0), untracked = trackedSidebands(0.0);
        std::printf("      an operator on the voice's Pitch: a key-tracked filter's sidebands %.1f dB (%.1f untracked)\n",
                    tracked, untracked);
        check(tracked > -60.0 && untracked < -90.0,
              "a key-tracked filter follows the voice's Pitch as an operator moves it, every sample");
    }
    {
        // A voice filter switched off, its cutoff moved meanwhile, and on again starts at its cutoff as it is then, as
        // one that was there all along does: from silence, not sweeping from where it was. Unheard before, so the two
        // are alike up to there.
        auto reopened = [](double cutoffBefore) {
            Engine e(2);
            e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
            e.p[paramIndex(0, OpParam::Out)] = 0.0;
            e.p[sendIndex(0, 0)] = 1.0;
            e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            e.p[paramIndex(0, FilterParam::Out)] = 0.0;
            e.p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(cutoffBefore);
            e.noteOn(57);
            e.render(4800);
            e.p[paramIndex(0, FilterParam::Enabled)] = 0.0;
            e.render(kBlock);
            e.p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(5000.0);
            e.render(kBlock);
            e.p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            e.p[paramIndex(0, FilterParam::Out)] = 1.0;
            return e.render(4 * kBlock);
        };
        check(reopened(200.0) == reopened(5000.0),
              "a voice filter switched on again mid-note starts at its settings as they are then");
    }
    {
        // Filter FM: A through a filter, a voice's or the filter effect, whose Cutoff B moves, B fixed and heard by
        // nothing. At 3 kHz, B goes through two whole cycles a control block, so nothing of it would reach a filter
        // that only took modulation by the block.
        const int b = 1, source = kOpSource + b;
        auto setUp = [&](ParamValues &p, bool effect, FilterType type, double hz, double depth, double resonance) {
            p[paramIndex(b, OpParam::Enabled)] = 1.0;
            p[paramIndex(b, OpParam::Fixed)] = 1.0;
            p[paramIndex(b, OpParam::FixedFreq)] = normFromFixedFreq(hz > kMaxFixedFreq ? hz / 10.0 : hz);
            p[paramIndex(b, OpParam::FixedMult)] = hz > kMaxFixedFreq ? 4.0 : 3.0; // x10, x1
            p[paramIndex(b, OpParam::Out)] = 0.0;
            p[slotAmountIndex(source, 0)] = depth;
            if (effect) {
                p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Filter);
                p[paramIndex(0, FxParam::Enabled)] = 1.0;
                chain(p, {0});
                p[paramIndex(0, FxParam::FilterType)] = static_cast<double>(type);
                p[paramIndex(0, FxParam::FilterCutoff)] = normFromCutoff(1000.0);
                p[paramIndex(0, FxParam::FilterResonance)] = resonance;
                p[paramIndex(0, FxParam::FilterMix)] = 1.0;
                p[slotTargetIndex(source, 0)] = slotTargetValue(paramIndex(0, FxParam::FilterCutoff));
                return;
            }
            p[paramIndex(0, OpParam::Out)] = 0.0;
            p[sendIndex(0, 0)] = 1.0;
            p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(type);
            p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(1000.0);
            p[paramIndex(0, FilterParam::Resonance)] = resonance;
            p[paramIndex(0, FilterParam::Out)] = 1.0;
            p[slotTargetIndex(source, 0)] = slotTargetValue(paramIndex(0, FilterParam::Cutoff));
        };
        // The sidebands of A's 440 Hz around B's 3 kHz, against A's own level, in dB.
        auto sidebands = [&](bool effect, double depth) {
            const std::vector<double> power = powerSpectrum(steadyTone(2, 69, [&](ParamValues &p) {
                setUp(p, effect, FilterType::LowPass, 3000.0, depth, 0.3);
            }));
            auto around = [&](double hz) {
                const long center = std::lround(hz * kFftSize / kSampleRate);
                double sum = 1e-30;
                for (long k = center - 6; k <= center + 6; ++k)
                    sum += power[static_cast<size_t>(k)];
                return sum;
            };
            return 10.0 * std::log10((around(2560.0) + around(3440.0)) / around(440.0));
        };
        const double voiceFm = sidebands(false, 0.3), voicePlain = sidebands(false, 0.0);
        const double fxFm = sidebands(true, 0.3), fxPlain = sidebands(true, 0.0);
        std::printf("      filter FM's sidebands: a voice's filter %.1f dB (%.1f without), the filter effect %.1f dB "
                    "(%.1f without)\n", voiceFm, voicePlain, fxFm, fxPlain);
        check(voiceFm > -50.0 && voicePlain < -90.0, "an operator on a voice filter's Cutoff moves it at audio rate");
        check(fxFm > -50.0 && fxPlain < -90.0, "an operator on the filter effect's Cutoff moves it at audio rate");

        // Every type, resonance and depth at full, in both places: finite, and a voice's ending with its note.
        bool finite = true, ended = true;
        for (int t = 0; t < static_cast<int>(FilterType::Count); ++t)
            for (const bool effect : {false, true}) {
                Engine e(2);
                setUp(e.p, effect, static_cast<FilterType>(t), 3000.0, 1.0, 1.0);
                e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.01);
                e.p[paramIndex(b, OpParam::Release)] = normFromTime(0.01);
                e.noteOn(45);
                std::vector<float> out = e.render(24000);
                e.noteOff(45);
                const std::vector<float> after = e.render(144000);
                out.insert(out.end(), after.begin(), after.end());
                for (const float x : out)
                    finite = finite && std::isfinite(x) && std::abs(x) < 64.0f;
                if (!effect)
                    for (size_t s = after.size() - 4800; s < after.size(); ++s)
                        ended = ended && std::abs(after[s]) < 1e-3f;
            }
        check(finite, "filter FM at full depth and resonance stays finite for every filter type, in both places");
        check(ended, "a voice whose filter an operator moves still ends");

        // B at sixteen times A's pitch, deep into a resonant filter: what it adds are A's harmonics, and anything else
        // has folded back.
        for (const int factor : {1, 2, 4, 8}) {
            const double voice = unexpectedDb(steadyTone(factor, 69, [&](ParamValues &p) {
                setUp(p, false, FilterType::LowPass, 7040.0, 0.8, 0.7);
            }), harmonics(440.0));
            const double effect = unexpectedDb(steadyTone(factor, 69, [&](ParamValues &p) {
                setUp(p, true, FilterType::LowPass, 7040.0, 0.8, 0.7);
            }), harmonics(440.0));
            std::printf("      filter FM's aliasing at %dx: a voice's filter %.1f dB, the filter effect %.1f dB\n", factor,
                        voice, effect);
            check(voice < -40.0 && effect < -40.0, "filter FM's aliasing stays far below what it plays");
        }

        // A slow operator moves the cutoff as far as an LFO of its rate and depth.
        auto swing = [&](bool lfo) {
            Engine e(2);
            setUp(e.p, false, FilterType::LowPass, 5.0, lfo ? 0.0 : 0.3, 0.3);
            e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
            if (lfo)
                lfoOn(e.p, paramIndex(0, FilterParam::Cutoff), 0.3, 5.0);
            e.noteOn(45);
            const std::vector<float> out = e.render(static_cast<size_t>(kSampleRate));
            float low = 1e9f, high = 0.0f;
            for (size_t at = 12000; at + 1200 <= out.size(); at += 1200) {
                float peak = 0.0f;
                for (size_t i = at; i < at + 1200; ++i)
                    peak = std::max(peak, std::abs(out[i]));
                low = std::min(low, peak);
                high = std::max(high, peak);
            }
            return high / low;
        };
        const float byOperator = swing(false), byLfo = swing(true);
        std::printf("      a saw's level swings %.2f times under a 5 Hz operator, %.2f under an LFO\n", byOperator, byLfo);
        check(std::abs(byOperator / byLfo - 1.0f) < 0.15f, "a slow operator moves a cutoff as far as an LFO does");

        // A new note takes the filter effect's FM over from the note before; the effect switched off under it fades.
        auto steps = [&](bool second, bool switchOff, double depth = 0.5) {
            Engine e(2);
            setUp(e.p, true, FilterType::LowPass, 40.0, depth, 0.3);
            e.p[paramIndex(0, OpParam::Attack)] = normFromTime(0.02);
            e.noteOn(48);
            const std::vector<float> before = e.render(12000);
            if (second)
                e.noteOn(55);
            if (switchOff)
                e.p[paramIndex(0, FxParam::Enabled)] = 0.0;
            const std::vector<float> after = e.render(2400);
            return std::pair{roughness(before, 4800, before.size()), roughness(after, 0, after.size())};
        };
        const auto [steady, onNote] = steps(true, false);
        const double onOff = steps(false, true).second, plainOff = steps(false, true, 0.0).second;
        std::printf("      the filter effect's FM: steps of %.5f steady, %.5f at a new note, %.5f switched off (%.5f "
                    "without FM)\n", steady, onNote, onOff, plainOff);
        check(onNote < 3.0 * steady, "a new note takes the filter effect's FM over without a click");
        check(onOff < 1.5 * plainOff, "the filter effect switched off under filter FM is no rougher than without it");
    }
    {
        // A host's automation and modulation splitting blocks into pieces: render() told only which params changed
        // (paramChanged(), sameParams) plays sample for sample as one working out everything again for every piece.
        // The params moved: the operators', a route's, the voice's, a filter's, an LFO's rate, an effect's (one an
        // LFO moves, one a macro moves), and the slots' depths and a macro's knob, which change what the rest reads.
        auto setUp = [](Engine &e) {
            for (int op = 0; op < kOps; ++op) {
                e.p[paramIndex(op, OpParam::Enabled)] = 1.0;
                e.p[paramIndex(op, OpParam::Out)] = 0.3;
                if (op + 1 < kOps)
                    e.p[matrixIndex(op + 1, op)] = 0.3;
            }
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
            e.p[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(0, 0)] = 0.2;
            e.p[slotTargetIndex(0, 1)] = slotTargetValue(paramIndex(0, FxParam::DelayMix));
            e.p[slotAmountIndex(0, 1)] = 0.3;
            e.p[slotTargetIndex(kMaxSources, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(kMaxSources, 0)] = -0.2;
            e.p[slotTargetIndex(kMaxSources, 1)] = slotTargetValue(paramIndex(0, FxParam::DelayFeedback));
            e.p[slotAmountIndex(kMaxSources, 1)] = 0.2;
            e.p[paramIndex(0, MacroParam::Value)] = 0.5;
            e.p[slotTargetIndex(kWheelSource, 0)] = slotTargetValue(paramIndex(2, OpParam::Out));
            e.p[slotAmountIndex(kWheelSource, 0)] = -0.2;
            e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Delay);
            chain(e.p, {0});
        };
        const int moved[] = {paramIndex(0, OpParam::Out),      paramIndex(3, OpParam::Out),
                             matrixIndex(1, 0),                paramIndex(1, OpParam::Ratio),
                             paramIndex(GlobalParam::Pan),     paramIndex(GlobalParam::FmAmount),
                             paramIndex(0, FilterParam::Cutoff), paramIndex(0, SourceParam::Rate),
                             paramIndex(0, FxParam::DelayMix), paramIndex(0, FxParam::DelayFeedback),
                             paramIndex(0, FxParam::DelayTimeL), slotAmountIndex(0, 0),
                             slotAmountIndex(kMaxSources, 0),  paramIndex(0, MacroParam::Value),
                             paramIndex(GlobalParam::ModWheel), slotAmountIndex(kWheelSource, 0)};
        Engine full(2), told(2);
        setUp(full);
        setUp(told);
        uint32_t rng = 12345u;
        auto next = [&rng] {
            rng = rng * 1664525u + 1013904223u;
            return (rng >> 8) / 16777216.0;
        };
        const size_t frames = static_cast<size_t>(1.5 * kSampleRate);
        std::vector<float> fullOut[2] = {std::vector<float>(frames), std::vector<float>(frames)};
        std::vector<float> toldOut[2] = {std::vector<float>(frames), std::vector<float>(frames)};
        int changes = 0;
        bool first = true;
        for (size_t done = 0; done < frames;) {
            if (done == 0 || (done > 30000 && done < 30300)) // a chord, then a note let go and another struck
                for (Engine *e : {&full, &told}) {
                    if (done == 0)
                        for (int key : {48, 60, 67})
                            e->noteOn(key);
                    else {
                        e->noteOff(60);
                        e->noteOn(72);
                    }
                }
            for (int c = static_cast<int>(next() * 4.0); c > 0; --c) {
                // Half of them on those above, half on any param that can be modulated (a type or a slot's wiring
                // too, which has everything worked out again).
                const int i = next() < 0.5 ? moved[static_cast<size_t>(next() * std::size(moved))]
                                           : static_cast<int>(next() * kModulatedCount);
                const ParamDef &def = paramTable()[i];
                if (next() < 0.5) { // automation
                    const double value = def.min + next() * (def.max - def.min);
                    full.p[i] = told.p[i] = value;
                    told.synth().paramChanged(i);
                } else { // the host's modulation for every voice
                    const double amount = (next() - 0.5) * 0.6 * (def.max - def.min);
                    full.synth().paramMod(i, {-1, -1, -1, -1}, amount);
                    told.synth().paramMod(i, {-1, -1, -1, -1}, amount);
                }
                ++changes;
            }
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(1 + static_cast<size_t>(next() * 200.0), frames - done));
            full.synth().render(fullOut[0].data() + done, fullOut[1].data() + done, n, full.p, false);
            told.synth().render(toldOut[0].data() + done, toldOut[1].data() + done, n, told.p, !first);
            first = false;
            done += n;
        }
        size_t differ = 0;
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (size_t s = 0; s < frames; ++s) {
                differ += std::memcmp(&fullOut[c][s], &toldOut[c][s], sizeof(float)) != 0;
                peak = std::max(peak, std::abs(fullOut[c][s]));
            }
        std::printf("      %d changes over 1.5 s in pieces of 1-200 samples: %zu samples differ (peak %.3f)\n", changes,
                    differ, peak);
        check(differ == 0 && peak > 0.0f,
              "automation and modulation told param by param play as working everything out again does");
    }
    {
        // A macro's knob as a target: the macros in a chain, Macro 3 on Macro 2's knob, Macro 2 on Macro 1's (the order
        // they are worked out in isn't their numbers'), and Macro 1 taking A's Out down by 100 %. Macro 3 turned up
        // takes A down to silence through the other two; turned down, A plays. The telemetry says where the knobs stand.
        auto peakOf = [](Engine &e, int key, double velocity = 1.0) {
            e.synth().noteOn({-1, 0, 0, static_cast<int16_t>(key)}, velocity);
            const std::vector<float> out = e.render(9600);
            float peak = 0.0f;
            for (size_t i = 4800; i < out.size(); ++i)
                peak = std::max(peak, std::abs(out[i]));
            return peak;
        };
        auto link = [](Engine &e, int source, int slot, int target, double depth) {
            e.p[slotTargetIndex(source, slot)] = slotTargetValue(target);
            e.p[slotAmountIndex(source, slot)] = depth;
        };
        const int out = paramIndex(0, OpParam::Out);
        auto chain = [&](double macro3, MacroKnobs *knobs) {
            Engine e(2);
            link(e, kMaxSources, 0, out, -1.0);
            link(e, kMaxSources + 1, 0, paramIndex(0, MacroParam::Value), 1.0);
            link(e, kMaxSources + 2, 0, paramIndex(1, MacroParam::Value), 1.0);
            e.p[paramIndex(2, MacroParam::Value)] = macro3;
            const float peak = peakOf(e, 60);
            if (knobs) {
                auto t = std::make_unique<Telemetry>();
                e.synth().telemetry(*t);
                for (int m = 0; m < kMacros; ++m)
                    (*knobs)[m] = e.p[paramIndex(m, MacroParam::Value)] + t->macroOffset[m];
            }
            return peak;
        };
        MacroKnobs knobs{};
        const float down = chain(1.0, &knobs), up = chain(0.0, nullptr);
        std::printf("      A's peak through a chain of three macros: %.4f with the last turned up, %.3f down; knobs "
                    "%.2f %.2f %.2f\n", down, up, knobs[0], knobs[1], knobs[2]);
        check(down < 1e-3f && up > 0.5f && std::abs(knobs[0] - 1.0) < 1e-6 && std::abs(knobs[1] - 1.0) < 1e-6,
              "a macro's knob takes the modulation of another macro, along a chain of them, whatever their numbers");

        // A loop the editor doesn't make (Macro 1 and Macro 2 on each other's knobs), as a state can hold it: it plays
        // steadily, each render alike.
        auto loop = [&] {
            Engine e(2);
            link(e, kMaxSources, 0, out, -0.5);
            link(e, kMaxSources, 1, paramIndex(1, MacroParam::Value), 1.0);
            link(e, kMaxSources + 1, 0, paramIndex(0, MacroParam::Value), 0.7);
            e.p[paramIndex(0, MacroParam::Value)] = 0.4;
            return peakOf(e, 60);
        };
        const float looped = loop(), again = loop();
        std::printf("      two macros on each other's knobs: peak %.4f, then %.4f\n", looped, again);
        check(std::isfinite(looped) && looped == again && looped > 0.1f,
              "macros modulating each other's knobs in a loop play steadily and alike");

        // Per voice: velocity on Macro 2's knob, Macro 2 taking A's Out down by 100 %. A note at full velocity is
        // silent, a soft one plays; played together, each voice has its own knob: the loud note adds nothing to the
        // soft one. Its knob in the telemetry is the newest voice's.
        auto voiced = [&](bool loud, bool soft, MacroKnobs *knobs) {
            Engine e(2);
            link(e, kMaxSources + 1, 0, out, -1.0);
            link(e, kVelocitySource, 0, paramIndex(1, MacroParam::Value), 1.0);
            if (loud)
                e.synth().noteOn({-1, 0, 0, 60}, 1.0);
            if (soft)
                e.synth().noteOn({-1, 0, 0, 64}, 0.05);
            // (Half a second, measured over its second half: the loud note's first moment, before its knob is down,
            // has rung out of the DC blocker before the effects by then.)
            const std::vector<float> rendered = e.render(24000);
            if (knobs) {
                auto t = std::make_unique<Telemetry>();
                e.synth().telemetry(*t);
                (*knobs)[1] = t->macroOffset[1];
            }
            return std::vector<float>(rendered.begin() + 12000, rendered.end());
        };
        MacroKnobs softKnob{};
        const std::vector<float> loudAlone = voiced(true, false, nullptr), softAlone = voiced(false, true, &softKnob),
                                 both = voiced(true, true, nullptr);
        float loudPeak = 0.0f, softPeak = 0.0f, apart = 0.0f;
        for (size_t i = 0; i < both.size(); ++i) {
            loudPeak = std::max(loudPeak, std::abs(loudAlone[i]));
            softPeak = std::max(softPeak, std::abs(softAlone[i]));
            apart = std::max(apart, std::abs(both[i] - softAlone[i]));
        }
        std::printf("      velocity on a macro's knob: a loud note's peak %.4f, a soft one's %.3f, both less the soft "
                    "one %.6f; the soft note's knob %.3f\n", loudPeak, softPeak, apart, softKnob[1]);
        check(loudPeak < 1e-3f && softPeak > 0.5f && apart < 1e-4f && softKnob[1] > 0.0 && softKnob[1] < 0.2,
              "a source of each voice on a macro's knob moves what the macro modulates in that voice alone");
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
        // The Phase knob turned under a held note: once it has moved, the note plays on as one started there, plain
        // and synced; the move itself doesn't click; 100 % is 0 % again; the next note starts where the knob stands;
        // and a source's modulation moves it under the note too, either way of a knob at 0 %, without zipper.
        const int phase = paramIndex(0, OpParam::Phase);
        auto furthest = [](const std::vector<float> &a, const std::vector<float> &b) {
            double most = 0.0;
            for (size_t i = 0; i < a.size(); ++i)
                most = std::max(most, std::abs(static_cast<double>(a[i]) - b[i]));
            return most;
        };
        // The last of a note held with its Phase at `from` for a tenth of a second, then at `to` for a second, over
        // which what the move left in the DC blocker before the effects dies away.
        auto turned = [&](double from, double to, bool synced) {
            Engine e(1);
            e.p[paramIndex(0, OpParam::Sync)] = synced ? 1.0 : 0.0;
            e.p[paramIndex(0, OpParam::Ratio)] = normFromRatio(synced ? 2.5 : 1.0);
            e.p[phase] = from;
            e.noteOn(57);
            e.render(4800);
            e.p[phase] = to;
            e.render(48000);
            return e.render(2400);
        };
        const double plain = furthest(turned(0.0, 0.25, false), turned(0.25, 0.25, false));
        const double synced = furthest(turned(0.0, 0.25, true), turned(0.25, 0.25, true));
        const double round = furthest(turned(0.0, 1.0, false), turned(0.0, 0.0, false));

        // Half a cycle at once, in whole blocks and in single frames: the largest step between two samples, as a share
        // of the note's peak, where a jump there would step by up to twice the peak.
        auto steepest = [&](uint32_t piece) {
            Engine e(1);
            e.noteOn(45);
            e.render(4800);
            double peak = 0.0;
            for (const float x : e.render(2400))
                peak = std::max(peak, std::abs(static_cast<double>(x)));
            e.p[phase] = 0.5;
            const std::vector<float> out = e.render(480, piece);
            double most = 0.0;
            for (size_t i = 1; i < out.size(); ++i)
                most = std::max(most, std::abs(static_cast<double>(out[i]) - out[i - 1]));
            return most / peak;
        };
        const double whole = steepest(kBlock), single = steepest(1);

        // Played, let go and played again with the knob turned in between.
        auto again = [&](double from) {
            Engine e(1);
            e.p[phase] = from;
            e.noteOn(57);
            e.render(4800);
            e.p[phase] = 0.25;
            e.render(4800);
            e.noteOff(57);
            e.render(24000);
            e.noteOn(57);
            e.render(2400);
            return e.render(2400);
        };
        const double next = furthest(again(0.0), again(0.25));

        // A slow LFO on Phase, 40 % deep: how far an A4's phase swings, in cycles, and how far above and below its
        // middle, read off the note a window of ten cycles at a time. With the knob at 0 %, half of it would be lost if
        // Phase stopped there.
        struct Swing {
            double up = 0.0, down = 0.0;
        };
        auto swing = [&](double knob) {
            Engine e(1);
            e.p[phase] = knob;
            lfoOn(e.p, phase, 0.4, 2.0);
            e.noteOn(69);
            e.render(4800);
            const std::vector<float> out = e.render(48000);
            const size_t window = 1091; // ten cycles of 440 Hz, near enough
            const double w = 2.0 * std::numbers::pi * 440.0 / kSampleRate;
            std::vector<double> at; // unwrapped, in cycles
            for (size_t from = 0; from + window <= out.size(); from += window) {
                double re = 0.0, im = 0.0;
                for (size_t i = from; i < from + window; ++i) {
                    re += out[i] * std::cos(w * static_cast<double>(i));
                    im += out[i] * std::sin(w * static_cast<double>(i));
                }
                const double cycles = std::atan2(im, re) / (2.0 * std::numbers::pi);
                at.push_back(at.empty() ? cycles : at.back() + std::remainder(cycles - at.back(), 1.0));
            }
            const auto [low, high] = std::minmax_element(at.begin(), at.end());
            double mean = 0.0;
            for (const double x : at)
                mean += x / static_cast<double>(at.size());
            return Swing{*high - mean, mean - *low};
        };
        const Swing fromZero = swing(0.0), fromMiddle = swing(0.5);
        auto wobble = [&](ParamValues &p) { lfoOn(p, phase, 0.2, 20.0); };
        std::vector<double> sidebands;
        for (int k = -12; k <= 12; ++k)
            sidebands.push_back(keyHz(57) + k * 20.0);
        const double zipper = unexpectedDb(steadyTone(1, 57, wobble), sidebands);
        std::printf("      Phase turned under a note: %.6f off one started there, synced %.6f, 0 to 100 %% %.6f; steepest "
                    "step %.3f of its peak, in single frames %.3f; the next note %.6f off\n",
                    plain, synced, round, whole, single, next);
        std::printf("      an LFO 40 %% deep on Phase swings a note +%.3f/-%.3f cycles from a knob at 0 %%, +%.3f/-%.3f at "
                    "50 %%; at 20 Hz %.1f dB off its sidebands\n",
                    fromZero.up, fromZero.down, fromMiddle.up, fromMiddle.down, zipper);
        check(plain < 1e-4 && synced < 1e-4, "Phase turned under a held note plays on as a note started there");
        check(round == 0.0, "Phase turned from 0 to 100 % leaves a note as it was");
        check(whole < 0.15 && single < 0.15, "Phase turned half a cycle under a note doesn't click");
        check(next < 1e-4, "a note after Phase was turned starts where the knob stands");
        auto near = [](double x) { return std::abs(x - 0.2) < 0.02; };
        check(near(fromZero.up) && near(fromZero.down) && near(fromMiddle.up) && near(fromMiddle.down),
              "an LFO on Phase swings a held note's phase both ways, from a knob at 0 % too");
        check(zipper < -60.0, "an LFO on Phase doesn't zipper");
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

// Where the pitch bend stands, as the editor's bend wheel shows it (see Telemetry::bend): the MIDI bend and the Pitch
// Bend param with the host's modulation for every voice, and in the newest voice its own on top; and the param as set.
void bendTelemetryChecks() {
    std::printf("bend telemetry:\n");
    auto t = std::make_unique<Telemetry>(); // large: the analyzers' spectra are in it
    Engine e(2);
    const int bend = paramIndex(GlobalParam::PitchBend);
    e.p[bend] = 0.2;
    e.synth().paramMod(bend, {-1, -1, -1, -1}, 0.3);
    e.synth().pitchBend(0, 0.1);
    e.render(kBlock);
    e.synth().telemetry(*t);
    const float idle = t->bend, idleSet = t->bendSet;
    e.noteOn(69, 7);
    e.synth().paramMod(bend, {7, -1, -1, -1}, 0.2);
    e.render(2 * kBlock);
    e.synth().telemetry(*t);
    std::printf("      Pitch Bend 0.2, the host's modulation +0.3, its MIDI bend +0.1: %.3f with no note (as set %.3f); "
                "a note's own modulation +0.2: %.3f (as set %.3f)\n",
                idle, idleSet, t->bend, t->bendSet);
    check(std::abs(idle - 0.6f) < 1e-4f && idleSet == 0.2f && std::abs(t->bend - 0.8f) < 1e-4f && t->bendSet == 0.2f,
          "the telemetry tells where the bend stands, the host's MIDI bend and modulation on top of the param as set");

    // Bend Range is a setting, not a target: an LFO's slot on it (as a state from before may hold) and a host's
    // modulation of it leave a note bent up all the way playing as without them.
    const int range = paramIndex(GlobalParam::BendRange);
    auto bent = [&](bool modulated) {
        Engine b(2);
        b.p[bend] = 1.0;
        if (modulated) {
            lfoOn(b.p, range, 1.0, 5.0);
            b.synth().paramMod(range, {-1, -1, -1, -1}, 10.0);
        }
        b.noteOn(69);
        return b.render(9600);
    };
    check(!isModTarget(range) && !(paramTable()[range].flags & CLAP_PARAM_IS_MODULATABLE) && bent(true) == bent(false),
          "Bend Range takes no modulation, from a source or a host");
}

// How a low note's scope moves as the note is let go of, from a snapshot every 8 ms (as the plugin takes them for the
// editor) over two and a half of its spans: whether it held still while the note did, how many snapshots were the same
// as the one before, and the largest step of its peak from one snapshot to the next, as a share of how far the peak
// fell. Blending each span into the next, it moves at every snapshot in small steps; a span at a time, it would stand
// still for most of them, then jump.
struct ScopeMotion {
    bool heldStill = false;
    int repeats = 0;
    double largestStep = 1.0;
};
template <class Span> ScopeMotion lowNoteMotion(Engine &e, Telemetry &t, const Span &span) {
    auto take = [&] {
        e.synth().telemetry(t);
        const auto values = span(t);
        return std::vector<float>(values.begin(), values.end());
    };
    auto peakOf = [](const std::vector<float> &values) {
        float peak = 0.0f;
        for (float v : values)
            peak = std::max(peak, std::abs(v));
        return static_cast<double>(peak);
    };
    e.p[paramIndex(0, OpParam::Release)] = normFromEnvTime(2.0);
    e.noteOn(21); // A0, 27.5 Hz: two cycles take 73 ms
    e.render(24000); // the DC blocker before the effects settled from the note's start (see Synth::DcBlocker)
    ScopeMotion motion;
    const std::vector<float> held = take();
    e.render(kBlock * 3 + 7);
    const std::vector<float> later = take();
    double most = 0.0;
    for (size_t bin = 0; bin < held.size(); ++bin)
        most = std::max(most, static_cast<double>(std::abs(later[bin] - held[bin])));
    motion.heldStill = !held.empty() && peakOf(held) > 0.1 && most < 0.02 * peakOf(held);
    e.noteOff(21);
    std::vector<double> peaks;
    std::vector<float> last = take();
    for (int snapshot = 0; snapshot < 24; ++snapshot) {
        e.render(384);
        std::vector<float> now = take();
        motion.repeats += now == last;
        peaks.push_back(peakOf(now));
        last = std::move(now);
    }
    const double drop = peaks.front() - peaks.back();
    double largest = 0.0;
    for (size_t k = 1; k < peaks.size(); ++k)
        largest = std::max(largest, std::abs(peaks[k] - peaks[k - 1]));
    motion.largestStep = drop > 0.0 ? largest / drop : 1.0;
    return motion;
}

// Drive into the ladder and the diode: held under kAnalogDriveCeiling going in rather than 1, so a loud, driven sound
// reaches their own saturation, which takes the input less the feedback, and their resonance comes down as it does in a
// driven transistor ladder. A full-level saw through 85 % resonance: its peak at full Drive some 20 dB further under
// its fundamental than undriven, in a voice filter and the filter effect (which hears the voices 12 dB down), where
// held under 1 it came down 9 to 10 dB in a voice filter and 14 in the effect.
void driveIntoLadderChecks() {
    std::printf("drive into the ladder and the diode:\n");
    // A saw at C2 through it, cut off at 1 kHz at 85 % resonance.
    auto play = [](bool effect, FilterType type, double drive) {
        return steadyTone(2, 36, [&](ParamValues &p) {
            p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
            if (effect) {
                p[paramIndex(3, FxParam::Type)] = static_cast<double>(FxType::Filter);
                p[paramIndex(3, FxParam::FilterType)] = static_cast<double>(type);
                p[paramIndex(3, FxParam::FilterSlope)] = 3.0;
                p[paramIndex(3, FxParam::FilterCutoff)] = normFromCutoff(1000.0);
                p[paramIndex(3, FxParam::FilterResonance)] = 0.85;
                p[paramIndex(3, FxParam::FilterDrive)] = drive;
                chain(p, {3});
                return;
            }
            p[paramIndex(0, OpParam::Out)] = 0.0;
            p[sendIndex(0, 0)] = 1.0;
            p[paramIndex(0, FilterParam::Enabled)] = 1.0;
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(type);
            p[paramIndex(0, FilterParam::Slope)] = 3.0;
            p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(1000.0);
            p[paramIndex(0, FilterParam::Resonance)] = 0.85;
            p[paramIndex(0, FilterParam::Drive)] = drive;
            p[paramIndex(0, FilterParam::Out)] = 0.25;
        });
    };
    // The resonance's peak (the harmonics from 850 to 1180 Hz) against the fundamental, in dB.
    auto peak = [](const std::vector<float> &out) {
        const std::vector<double> power = powerSpectrum(out);
        const double binHz = kSampleRate / kFftSize;
        double band = 1e-30, fundamental = 1e-30;
        for (size_t b = static_cast<size_t>(850.0 / binHz); b < static_cast<size_t>(1180.0 / binHz); ++b)
            band += power[b];
        const long center = std::lround(keyHz(36) / binHz);
        for (long b = center - 3; b <= center + 3; ++b)
            fundamental += power[static_cast<size_t>(b)];
        return 10.0 * std::log10(band / fundamental);
    };
    bool pulled = true;
    for (bool effect : {false, true})
        for (FilterType type : {FilterType::Ladder, FilterType::Diode}) {
            const double undriven = peak(play(effect, type, 0.0)), driven = peak(play(effect, type, 1.0));
            std::printf("      %-7s %-15s its resonance %.1f dB over the fundamental undriven, %.1f at full Drive\n",
                        type == FilterType::Ladder ? "ladder" : "diode", effect ? "(effect)" : "(voice filter)",
                        undriven, driven);
            pulled = pulled && driven < undriven - (effect ? 16.0 : 15.0);
        }
    check(pulled, "a loud sound driven into the ladder and the diode pulls their resonance down");
}

// Drive keeps the level a sound goes in at: its saturator's output is brought back by followers of the power going in
// and coming out. A saw at C3 through a low-pass and a Sallen-Key low-pass, both open (at 20 kHz, without resonance), at
// -24, -12, 0 and +6 dB and Drive 50 % and 100 %, comes out within 1 dB of its level undriven, where Drive taking its
// output down by its square root put a full-level saw 5 dB under and a quiet one 9 dB over. And a note starting at full
// level, full Drive, peaks no more than 2 dB over where it settles. (One starting with a jump to full level, as an
// operator whose Phase starts it at its peak does, peaks about 4 dB over for its first cycle: the followers have heard
// only its loudest part.)
void driveLevelChecks() {
    std::printf("drive keeps the level:\n");
    const double rate = 96000.0; // the voices' at 2x
    auto run = [&](FilterType type, double drive, double level, std::vector<double> &out) {
        const FilterSettings s = filterSettings(type, 1, 20000.0, 0.0, drive, rate);
        Filter filter;
        std::vector<float> comb(16, 0.0f);
        out.resize(static_cast<size_t>(rate * 0.4));
        for (size_t i = 0; i < out.size(); ++i) { // from 0, as an operator's saw starts
            const double phase = std::fmod(0.5 + i * keyHz(48) / rate, 1.0);
            out[i] = filter.process(level * (2.0 * phase - 1.0), s, comb.data(), static_cast<int>(comb.size()));
        }
    };
    auto rmsFrom = [](const std::vector<double> &x, size_t from) {
        double sum = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            sum += x[i] * x[i];
        return std::sqrt(sum / static_cast<double>(x.size() - from));
    };
    const size_t settled = static_cast<size_t>(rate * 0.2);
    bool kept = true;
    double worst = 0.0;
    for (FilterType type : {FilterType::LowPass, FilterType::SallenKeyLow})
        for (double levelDb : {-24.0, -12.0, 0.0, 6.0}) {
            const double level = std::pow(10.0, levelDb / 20.0);
            std::vector<double> clean, out;
            run(type, 0.0, level, clean);
            const double undriven = rmsFrom(clean, settled);
            for (double drive : {0.5, 1.0}) {
                run(type, drive, level, out);
                const double change = 20.0 * std::log10(rmsFrom(out, settled) / undriven);
                worst = std::max(worst, std::abs(change));
                kept = kept && std::abs(change) < 1.0;
            }
        }
    std::vector<double> out;
    run(FilterType::LowPass, 1.0, 1.0, out);
    const size_t start = static_cast<size_t>(rate * 0.02);
    double onset = 0.0, after = 0.0;
    for (size_t i = 0; i < start; ++i)
        onset = std::max(onset, std::abs(out[i]));
    for (size_t i = settled; i < out.size(); ++i)
        after = std::max(after, std::abs(out[i]));
    std::printf("      saw at -24 to +6 dB, Drive 50 and 100 %%: level within %.2f dB of undriven; a note's first 20 ms "
                "peaking %.2f dB over where it settles\n",
                worst, 20.0 * std::log10(onset / after));
    check(kept, "a sound driven into a filter keeps its level, quiet or loud");
    check(onset < after * std::pow(10.0, 2.0 / 20.0), "a driven note starts at about the level it settles at");
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

    // A low note's span, quicker than a slow one, blends into the next.
    {
        Engine low(2);
        low.synth().setScope(0, 2);
        const ScopeMotion motion = lowNoteMotion(low, *telemetry, [](const Telemetry &t) {
            return std::span<const float>(t.scope.data(), static_cast<size_t>(t.scopeBins));
        });
        std::printf("      an A0 let go of: %d of 24 snapshots as the one before, its peak's largest step %.3f of its "
                    "fall\n",
                    motion.repeats, motion.largestStep);
        check(motion.heldStill && motion.repeats == 0 && motion.largestStep < 0.2,
              "a low note's scope stands still while held, and blends each span into the next, moving at every "
              "snapshot");
    }

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

    // A low note's spans blend one into the next.
    {
        Engine low(2);
        low.synth().setAnalyzing(true);
        const ScopeMotion motion = lowNoteMotion(
            low, *telemetry, [](const Telemetry &t) { return std::span<const float>(t.outScope.data(), t.outScope.size()); });
        std::printf("      an A0 let go of: %d of 24 snapshots as the one before, its peak's largest step %.3f of its "
                    "fall\n",
                    motion.repeats, motion.largestStep);
        check(motion.heldStill && motion.repeats == 0 && motion.largestStep < 0.2,
              "a low note's output scope stands still while held, and blends each span into the next, moving at every "
              "snapshot");
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

// What the switches below start from: operator A a saw at 2.5 times the note.
void syncWarpBase(ParamValues &p) {
    p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
    p[paramIndex(0, OpParam::Ratio)] = normFromRatio(2.5);
}

// --- An operator's hard sync and warp: band-limited (the energy off the note's harmonics, as the aliasing cases
// above), switched on and changed under a note without a click, a warp's amount modulated without zipper, a synced
// operator's ratio swept by an LFO staying on the note's harmonics and their sidebands, and a warp at 0 the plain wave.
void syncWarpChecks(const int (&factors)[4]) {
    std::printf("hard sync and warp:\n");
    struct Case {
        const char *name;
        int key;
        OpWave wave;
        bool sync;
        double ratio;
        OpWarp warp;
        double amount;
        double limit2x; // dB at 2x oversampling
    };
    const Case cases[] = {
        {"synced sine x3.3 at C6", 84, OpWave::Sine, true, 3.3, OpWarp::Off, 0.0, -60.0},
        {"synced saw x2.6 at C6", 84, OpWave::Saw, true, 2.6, OpWarp::Off, 0.0, -50.0},
        {"synced sine x7.7 at C5", 72, OpWave::Sine, true, 7.7, OpWarp::Off, 0.0, -60.0},
        {"Bend 100% sine at C6", 84, OpWave::Sine, false, 1.0, OpWarp::Bend, 1.0, -60.0},
        {"Bend -60% saw at C6", 84, OpWave::Saw, false, 1.0, OpWarp::Bend, -0.6, -60.0},
        {"Squeeze 80% square at C6", 84, OpWave::Square, false, 1.0, OpWarp::Squeeze, 0.8, -60.0},
        {"Squeeze -100% sine at C6", 84, OpWave::Sine, false, 1.0, OpWarp::Squeeze, -1.0, -60.0},
        {"Pulse 100% sine at C6", 84, OpWave::Sine, false, 1.0, OpWarp::Pulse, 1.0, -60.0},
        {"Pulse -70% saw at C6", 84, OpWave::Saw, false, 1.0, OpWarp::Pulse, -0.7, -60.0},
        {"Steps 100% sine at C6", 84, OpWave::Sine, false, 1.0, OpWarp::Steps, 1.0, -60.0},
        {"Steps -100% triangle at C6", 84, OpWave::Triangle, false, 1.0, OpWarp::Steps, -1.0, -58.0},
        {"synced Squeeze 60% square x2.4 C6", 84, OpWave::Square, true, 2.4, OpWarp::Squeeze, 0.6, -50.0},
        {"Sync warp 50% sine at C6", 84, OpWave::Sine, false, 1.0, OpWarp::Sync, 0.5, -60.0},
        {"Sync warp -80% saw at C6", 84, OpWave::Saw, false, 1.0, OpWarp::Sync, -0.8, -50.0},
        {"Sync warp 30% triangle at C7", 96, OpWave::Triangle, false, 1.0, OpWarp::Sync, 0.3, -60.0},
    };
    auto setup = [](const Case &c) {
        return [&c](ParamValues &p) {
            p[paramIndex(0, OpParam::Wave)] = static_cast<double>(c.wave);
            p[paramIndex(0, OpParam::Sync)] = c.sync ? 1.0 : 0.0;
            p[paramIndex(0, OpParam::Ratio)] = normFromRatio(c.ratio);
            p[paramIndex(0, OpParam::Warp)] = static_cast<double>(c.warp);
            p[paramIndex(0, OpParam::WarpAmount)] = c.amount;
        };
    };
    for (const Case &c : cases) {
        double db[4];
        for (int f = 0; f < 4; ++f)
            db[f] = unexpectedDb(steadyTone(factors[f], c.key, setup(c)), harmonics(keyHz(c.key)));
        std::printf("      %-34s 1x %7.1f   2x %7.1f   4x %7.1f   8x %7.1f\n", c.name, db[0], db[1], db[2], db[3]);
        char what[128];
        std::snprintf(what, sizeof(what), "%s: below %.0f dB at 2x", c.name, c.limit2x);
        check(db[1] < c.limit2x, what);
        std::snprintf(what, sizeof(what), "%s: oversampling doesn't make it worse", c.name);
        check(db[3] < db[0] + 3.0, what);
    }

    // A warp at 0 is the plain wave, bit for bit; and with Sync and Warp at their defaults (as every patch from before
    // them has them) an operator plays as it did, which the factory presets' renders in fmvibe-bench also check.
    {
        auto plain = [](ParamValues &p) { p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw); };
        const std::vector<float> a = steadyTone(2, 69, plain);
        bool same = true;
        for (OpWarp warp : {OpWarp::Bend, OpWarp::Squeeze, OpWarp::Pulse, OpWarp::Steps, OpWarp::Sync}) {
            const std::vector<float> b = steadyTone(2, 69, [&](ParamValues &p) {
                plain(p);
                p[paramIndex(0, OpParam::Warp)] = static_cast<double>(warp);
            });
            same = same && a == b;
        }
        const ParamDef &sync = paramTable()[paramIndex(0, OpParam::Sync)];
        const ParamDef &warp = paramTable()[paramIndex(0, OpParam::Warp)];
        const ParamDef &amount = paramTable()[paramIndex(0, OpParam::WarpAmount)];
        check(same && sync.def == 0.0 && warp.def == 0.0 && amount.def == 0.0,
              "every warp at 0 plays the plain wave bit for bit, and sync and warp start off");
    }

    // Switched on and changed under a held A3, A a saw at 2.5 times it (in single frames too, as events split
    // blocks): nothing rougher than the synced or warped sound itself is, before or after, or the plain saw a warp
    // changed to another passes through.
    {
        struct Switch {
            const char *name;
            void (*before)(ParamValues &);
            void (*after)(ParamValues &);
        };
        const Switch switches[] = {
            {"sync switched on", [](ParamValues &p) { syncWarpBase(p); },
             [](ParamValues &p) { syncWarpBase(p), p[paramIndex(0, OpParam::Sync)] = 1.0; }},
            {"sync switched off", [](ParamValues &p) { syncWarpBase(p), p[paramIndex(0, OpParam::Sync)] = 1.0; },
             [](ParamValues &p) { syncWarpBase(p); }},
            {"Squeeze 90% switched on", [](ParamValues &p) { syncWarpBase(p); },
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
                 p[paramIndex(0, OpParam::WarpAmount)] = 0.9;
             }},
            {"Bend 90% to Steps 90%",
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Bend);
                 p[paramIndex(0, OpParam::WarpAmount)] = 0.9;
             },
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Steps);
                 p[paramIndex(0, OpParam::WarpAmount)] = 0.9;
             }},
            {"Squeeze 90% to Sync 70%",
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
                 p[paramIndex(0, OpParam::WarpAmount)] = 0.9;
             },
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Sync);
                 p[paramIndex(0, OpParam::WarpAmount)] = 0.7;
             }},
            {"Pulse -100% to +100%",
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Pulse);
                 p[paramIndex(0, OpParam::WarpAmount)] = -1.0;
             },
             [](ParamValues &p) {
                 syncWarpBase(p);
                 p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Pulse);
                 p[paramIndex(0, OpParam::WarpAmount)] = 1.0;
             }},
        };
        // A warp changed to another goes through its plain wave (an amount of 0) on the way, which counts too.
        double plain = 0.0;
        {
            Engine e(2);
            syncWarpBase(e.p);
            e.noteOn(57);
            plain = roughness(e.render(9600), 4800, 9600);
        }
        double worst = 0.0;
        for (const Switch &sw : switches)
            for (uint32_t piece : {kBlock, 1u}) {
                Engine e(2);
                sw.before(e.p);
                e.noteOn(57);
                const std::vector<float> before = e.render(9600, piece);
                sw.after(e.p);
                const std::vector<float> after = e.render(9600, piece);
                const std::vector<float> both = joined(before, after);
                const double steady = std::max({roughness(both, 4800, before.size() - 1),
                                                roughness(both, before.size() + 4800, both.size()), plain});
                const double switched = roughness(both, before.size() - 2, before.size() + 480);
                const double ratio = switched / steady;
                if (piece == kBlock)
                    std::printf("      %-34s %.2f times as rough as the sound itself\n", sw.name, ratio);
                worst = std::max(worst, ratio);
            }
        check(worst < 1.5, "sync and warp switched or changed under a note don't click");
    }

    // Modulated: a 20 Hz LFO on Squeeze's amount (a square's pulse width) and on a synced operator's ratio leaves only
    // the note's harmonics and their sidebands, not partials at the control rate.
    {
        const int key = 57;
        const double f0 = keyHz(key), lfo = 20.0;
        std::vector<double> partials;
        for (double h : harmonics(f0))
            for (int k = -16; k <= 16; ++k)
                partials.push_back(h + k * lfo);
        auto pwm = [&](ParamValues &p) {
            p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Square);
            p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
            p[paramIndex(0, OpParam::WarpAmount)] = 0.0;
            lfoOn(p, paramIndex(0, OpParam::WarpAmount), 0.3, lfo);
        };
        const double width = unexpectedDb(steadyTone(1, key, pwm), partials);
        std::printf("      %-34s %7.1f dB off the expected partials\n", "LFO on Squeeze (PWM)", width);
        check(width < -60.0, "an LFO on a warp's amount doesn't zipper");
        auto sweep = [&](ParamValues &p) {
            p[paramIndex(0, OpParam::Sync)] = 1.0;
            p[paramIndex(0, OpParam::Ratio)] = normFromRatio(3.0);
            lfoOn(p, paramIndex(0, OpParam::Ratio), 0.1, lfo);
        };
        const double swept = unexpectedDb(steadyTone(2, key, sweep), partials);
        std::printf("      %-34s %7.1f dB off the expected partials\n", "LFO on a synced ratio (sweep)", swept);
        check(swept < -60.0, "a synced operator's ratio swept stays on the note's harmonics, without zipper");
    }
}

// Unison Curve, Unison Pan and Blend: the spreads unison.h works out, and what an A4's copies then play.
void unisonModeChecks() {
    std::printf("unison modes:\n");
    {
        bool linear = true, center = true, supersaw = true, random = true, alternate = true, shuffled = true;
        bool blended = true;
        for (int n = 1; n <= kMaxUnison; ++n) {
            const UnisonSpreadOut even = unisonSpread(n, UniCurve::Linear, UniPan::Ramp, 0.0, 7u, 0);
            const UnisonSpreadOut mid = unisonSpread(n, UniCurve::Center, UniPan::Alternate, 0.0, 7u, 0);
            const UnisonSpreadOut a = unisonSpread(n, UniCurve::Random, UniPan::Random, 0.0, 7u, 2);
            const UnisonSpreadOut again = unisonSpread(n, UniCurve::Random, UniPan::Random, 0.0, 7u, 2);
            const UnisonSpreadOut other = unisonSpread(n, UniCurve::Random, UniPan::Random, 0.0, 8u, 2);
            std::vector<double> places, evenPlaces, dealt;
            bool differs = n == 1;
            for (int k = 0; k < n; ++k) {
                // Linear and Ramp are the even spread the copies always had, to the bit.
                const double was = n > 1 ? 2.0 * k / (n - 1) - 1.0 : 0.0;
                linear = linear && even.detune[k] == was && even.pan[k] == was && even.weight[k] == 1.0;
                center = center && std::abs(std::abs(mid.detune[k]) - was * was) < 1e-12 &&
                         (mid.detune[k] < 0.0) == (was < 0.0);
                // Random: one copy in each even stretch, the same for a seed, another for another seed.
                const double lo = -1.0 + 2.0 * k / n, hi = -1.0 + 2.0 * (k + 1) / n;
                random = random && (n == 1 ? a.detune[k] == 0.0 : a.detune[k] >= lo && a.detune[k] < hi) &&
                         a.detune[k] == again.detune[k] && a.pan[k] == again.pan[k];
                differs = differs || a.detune[k] != other.detune[k];
                // Alternate: copies next in pitch on opposite sides (or one in the middle), no two in one place.
                if (k > 0)
                    alternate = alternate && mid.pan[k] * mid.pan[k - 1] <= 0.0;
                places.push_back(mid.pan[k]);
                evenPlaces.push_back(was);
                dealt.push_back(a.pan[k]);
            }
            random = random && differs;
            std::sort(places.begin(), places.end());
            alternate = alternate && std::adjacent_find(places.begin(), places.end()) == places.end();
            // Random pan: the even places, dealt out.
            std::sort(dealt.begin(), dealt.end());
            shuffled = shuffled && dealt == evenPlaces;
            // Blend: the outer copies quieter than the middle ones, the squares summing to the count as before.
            const UnisonSpreadOut full = unisonSpread(n, UniCurve::Linear, UniPan::Ramp, 1.0, 7u, 0);
            double squares = 0.0;
            for (int k = 0; k < n; ++k)
                squares += full.weight[k] * full.weight[k];
            const double ratio = n % 2 ? 0.1 : 0.1 / (1.0 - 0.9 / (n - 1)); // the outermost over the middle one
            blended = blended && std::abs(squares - n) < 1e-9 &&
                      (n < 3 || std::abs(full.weight[0] / full.weight[n / 2] - ratio) < 1e-9);
        }
        const UnisonSpreadOut saw = unisonSpread(7, UniCurve::Supersaw, UniPan::Ramp, 0.0, 0u, 0);
        for (int k = 0; k < 7; ++k)
            supersaw = supersaw && std::abs(saw.detune[k] - kSupersawDetune[k]) < 1e-12;
        check(linear, "Linear and Ramp spread the copies evenly, as unison always did");
        check(center, "Center bunches the copies about the pitch, the outer ones as far out");
        check(supersaw, "Supersaw spaces seven copies as a supersaw does");
        check(random, "Random puts a copy in each stretch of the detune, the same through a note and new for another");
        check(alternate, "Alternate puts copies next in pitch on opposite sides, none in one place");
        check(shuffled, "Random pan deals the copies the even places");
        check(blended, "Blend turns the outer copies down, as loud together as before");
    }
    {
        // Unison Detune reaches 200 cents since version 13, 100 before: a version 12 state's depth on it (on operator
        // C's) loads half as deep, swinging it as far as it did, and a version 13 state's as saved. Unison Pan, new with
        // version 13 and Alternate to begin with: a version 12 state (which has none) loads Ramp, as its copies played,
        // and a version 13 state that leaves it out, as a sparse one does at its default, Alternate.
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(2, OpParam::UnisonDetune));
        p[slotAmountIndex(0, 0)] = 0.4;
        p[slotTargetIndex(0, 1)] = slotTargetValue(paramIndex(2, OpParam::Detune)); // not Unison Detune: as saved
        p[slotAmountIndex(0, 1)] = 0.4;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, gui::UiLayout{}, true);
        auto depths = [&bytes](uint32_t version) { // the two depths, from the patch as saved by that version
            std::vector<unsigned char> saved = bytes;
            std::memcpy(saved.data() + sizeof(uint32_t), &version, sizeof(version)); // after the magic
            MemoryReader reader{saved.data(), saved.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            ParamValues loaded;
            const bool read = readParams(&in, loaded);
            return std::tuple(read ? loaded[slotAmountIndex(0, 0)] : -1.0, loaded[slotAmountIndex(0, 1)],
                              loaded[paramIndex(4, OpParam::UnisonPan)]);
        };
        const auto [older, olderDetune, olderPan] = depths(12);
        const auto [now, nowDetune, nowPan] = depths(13);
        std::printf("      a depth of 0.4 on Unison Detune: %.2f from a version 12 state, %.2f from 13\n", older, now);
        check(older == 0.2 && olderDetune == 0.4 && now == 0.4 && nowDetune == 0.4,
              "a version 12 state's depth on Unison Detune loads half as deep, a version 13 state's as saved");
        check(paramTable()[paramIndex(4, OpParam::UnisonPan)].def == static_cast<double>(UniPan::Alternate) &&
                  olderPan == static_cast<double>(UniPan::Ramp) && nowPan == static_cast<double>(UniPan::Alternate),
              "Unison Pan starts on Alternate, and a version 12 state loads Ramp, as its copies played");
    }
    {
        // A source's Division reaches 8/1 since version 13, two steps below 4/1D: a version 12 state's depth on it (on
        // Mod 3's) loads 26/28 as deep, stepping it as far as it did, and its value as saved, a version 13 state's
        // depth as saved too. A depth on its Rate loads as saved.
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(2, SourceParam::Division)] = 7.0; // 1/1
        p[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(2, SourceParam::Division));
        p[slotAmountIndex(0, 0)] = 0.28;
        p[slotTargetIndex(0, 1)] = slotTargetValue(paramIndex(2, SourceParam::Rate)); // not Division: as saved
        p[slotAmountIndex(0, 1)] = 0.28;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, gui::UiLayout{}, true);
        auto loadedAt = [&bytes](uint32_t version) { // the two depths and the Division, as saved by that version
            std::vector<unsigned char> saved = bytes;
            std::memcpy(saved.data() + sizeof(uint32_t), &version, sizeof(version)); // after the magic
            MemoryReader reader{saved.data(), saved.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            ParamValues loaded;
            const bool read = readParams(&in, loaded);
            return std::tuple(read ? loaded[slotAmountIndex(0, 0)] : -1.0, loaded[slotAmountIndex(0, 1)],
                              loaded[paramIndex(2, SourceParam::Division)]);
        };
        const auto [older, olderRate, olderDivision] = loadedAt(12);
        const auto [now, nowRate, nowDivision] = loadedAt(13);
        std::printf("      a depth of 0.28 on a source's Division: %.4f from a version 12 state, %.4f from 13\n", older,
                    now);
        auto near = [](double a, double b) { return std::abs(a - b) < 1e-12; };
        check(near(older, 0.26) && olderRate == 0.28 && olderDivision == 7.0 && now == 0.28 && nowRate == 0.28 &&
                  nowDivision == 7.0,
              "a version 12 state's depth on a source's Division loads 26/28 as deep, a version 13 state's as saved");
    }

    // An A4's copies at 100 cents: what each one plays where, from a sine's lines.
    auto level = [](const std::vector<float> &signal, double hz) { // the loudest bin near it, in dB
        const std::vector<double> power = powerSpectrum(signal);
        const long center = std::lround(hz / (kSampleRate / kFftSize));
        double most = 1e-30;
        for (long b = center - 2; b <= center + 2; ++b)
            most = std::max(most, power[static_cast<size_t>(b)]);
        return 10.0 * std::log10(most);
    };
    struct Sides {
        std::vector<float> left, right;
    };
    auto play = [](int copies, UniCurve curve, UniPan pan, double blend, double spread = 1.0) {
        Engine e(2);
        e.p[paramIndex(0, OpParam::Unison)] = copies;
        e.p[paramIndex(0, OpParam::UnisonDetune)] = 100.0;
        e.p[paramIndex(0, OpParam::UnisonSpread)] = spread;
        e.p[paramIndex(0, OpParam::UnisonCurve)] = static_cast<double>(curve);
        e.p[paramIndex(0, OpParam::UnisonPan)] = static_cast<double>(pan);
        e.p[paramIndex(0, OpParam::UnisonBlend)] = blend;
        e.noteOn(69);
        e.render(4800);
        Sides s{std::vector<float>(kFftSize), std::vector<float>(kFftSize)};
        for (size_t done = 0; done < kFftSize; done += kBlock)
            e.synth().render(s.left.data() + done, s.right.data() + done, kBlock, e.p);
        return s;
    };
    auto cents = [](double c) { return 440.0 * std::exp2(c / 1200.0); };
    {
        // Five copies: Linear's inner pair at 50 cents, Center's at 25.
        const Sides lin = play(5, UniCurve::Linear, UniPan::Ramp, 0.0);
        const Sides mid = play(5, UniCurve::Center, UniPan::Ramp, 0.0);
        const double lin50 = level(lin.left, cents(-50.0)), lin25 = level(lin.left, cents(-25.0));
        const double mid50 = level(mid.left, cents(-50.0)), mid25 = level(mid.left, cents(-25.0));
        std::printf("      5 copies, -50 and -25 cents: Linear %.0f and %.0f dB, Center %.0f and %.0f dB\n", lin50, lin25,
                    mid50, mid25);
        check(lin50 - lin25 > 30.0 && mid25 - mid50 > 30.0, "Center plays its inner copies nearer the pitch than Linear");
    }
    {
        // Three copies: Ramp puts the lowest on the left, Alternate (mirrored outward from the middle) on the right.
        const Sides ramp = play(3, UniCurve::Linear, UniPan::Ramp, 0.0);
        const Sides alt = play(3, UniCurve::Linear, UniPan::Alternate, 0.0);
        const double rampSide = level(ramp.left, cents(-100.0)) - level(ramp.right, cents(-100.0));
        const double altSide = level(alt.left, cents(-100.0)) - level(alt.right, cents(-100.0));
        std::printf("      3 copies, the lowest left over right: Ramp %+.0f dB, Alternate %+.0f dB\n", rampSide, altSide);
        check(rampSide > 30.0 && altSide < -30.0, "Alternate moves the copies to other sides than Ramp");
    }
    {
        // Five copies, in the middle: at full Blend the outermost a tenth of the middle one, 20 dB under it; with
        // none, as loud.
        auto mono = [](const Sides &s) {
            std::vector<float> m(kFftSize);
            for (size_t i = 0; i < kFftSize; ++i)
                m[i] = s.left[i] + s.right[i];
            return m;
        };
        const std::vector<float> flat = mono(play(5, UniCurve::Linear, UniPan::Ramp, 0.0, 0.0));
        const std::vector<float> full = mono(play(5, UniCurve::Linear, UniPan::Ramp, 1.0, 0.0));
        const double flatGap = level(flat, 440.0) - level(flat, cents(100.0));
        const double fullGap = level(full, 440.0) - level(full, cents(100.0));
        std::printf("      5 copies, the outermost under the middle one: Blend 0 %.1f dB, full %.1f dB\n", flatGap,
                    fullGap);
        check(std::abs(flatGap) < 1.5 && std::abs(fullGap - 20.0) < 1.5,
              "Blend turns the outer copies down under the middle ones");
    }
    {
        // A modulator's copies at full Blend: in what its carrier plays, the outermost's sideband 20 dB under the
        // middle one's, as heard.
        auto modulated = [](double blend) {
            Engine e(2);
            e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
            e.p[paramIndex(1, OpParam::Out)] = 0.0;
            e.p[paramIndex(1, OpParam::Ratio)] = normFromRatio(0.5);
            e.p[paramIndex(1, OpParam::Unison)] = 5.0;
            e.p[paramIndex(1, OpParam::UnisonDetune)] = 100.0;
            e.p[paramIndex(1, OpParam::UnisonBlend)] = blend;
            e.p[matrixIndex(1, 0)] = 0.3;
            e.noteOn(69);
            e.render(4800);
            return e.render(kFftSize);
        };
        const std::vector<float> flat = modulated(0.0), full = modulated(1.0);
        // The first sideband above the carrier from the middle copy (220 Hz) and from the outermost (100 cents up).
        const double outer = 440.0 + 220.0 * std::exp2(100.0 / 1200.0);
        const double flatGap = level(flat, 660.0) - level(flat, outer);
        const double fullGap = level(full, 660.0) - level(full, outer);
        std::printf("      a modulator's outermost copy under its middle one, in its carrier: Blend 0 %.1f dB, full %.1f dB "
                    "(the middle one's sideband %.0f dB under the carrier)\n",
                    flatGap, fullGap, level(flat, 440.0) - level(flat, 660.0));
        check(std::abs(flatGap) < 1.5 && std::abs(fullGap - 20.0) < 2.0,
              "Blend turns a modulator's outer copies down in what its carrier plays");
    }
}

} // namespace

// The shaper effects in slots 0..slots-1 one after another, each Stack at `stack` and `driveDb`, all wet, the last
// unlinked at 0 dB output and (if `linkedRow`) the ones before it linked.
ParamValues stackParams(int slots, int stack, double driveDb, bool linkedRow = false) {
    ParamValues p{};
    for (int i = 0; i < kParamCount; ++i)
        p[i] = paramTable()[i].def;
    std::vector<int> order;
    for (int slot = 0; slot < slots; ++slot) {
        p[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        p[paramIndex(slot, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Stack);
        p[paramIndex(slot, FxParam::ShaperStack)] = stack;
        p[paramIndex(slot, FxParam::ShaperDrive)] = driveDb;
        p[paramIndex(slot, FxParam::ShaperLink)] = linkedRow && slot + 1 < slots ? 1.0 : 0.0;
        p[paramIndex(slot, FxParam::ShaperOutput)] = 0.0;
        p[paramIndex(slot, FxParam::ShaperMix)] = 1.0;
        order.push_back(slot);
    }
    chain(p, order);
    return p;
}

// `in` through the effects as `p` sets them, on both sides, in pieces of `piece` frames, `change` called before each
// with the frame it starts at; the left side, then (if `ringing`) whether the effects still ran at the end.
template <class Change>
std::vector<float> throughEffects(ParamValues p, const std::vector<float> &in, uint32_t piece, const Change &change,
                                  bool *ringing = nullptr) {
    const auto fxHeap = std::make_unique<Effects>();
    Effects &fx = *fxHeap;
    fx.prepare(kSampleRate);
    std::vector<float> left = in, right = in;
    for (size_t t = 0; t < in.size(); t += piece) {
        change(p, t);
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(piece, in.size() - t));
        fx.process(left.data() + t, right.data() + t, n, p, 120.0, 60.0);
    }
    if (ringing)
        *ringing = fx.ringing();
    return left;
}
std::vector<float> throughEffects(const ParamValues &p, const std::vector<float> &in, bool *ringing = nullptr) {
    return throughEffects(p, in, kBlock, [](ParamValues &, size_t) {}, ringing);
}

void stackChecks() {
    std::printf("stack:\n");
    // A saw-like tone at 110 Hz, its first eight harmonics, half a second at -6 dB, then a release of 50 ms and two
    // seconds of silence.
    const size_t held = static_cast<size_t>(kSampleRate / 2), release = static_cast<size_t>(0.05 * kSampleRate);
    std::vector<float> tone(held + release + static_cast<size_t>(2.0 * kSampleRate), 0.0f);
    for (size_t i = 0; i < held + release; ++i) {
        double x = 0.0;
        for (int k = 1; k <= 8; ++k)
            x += std::sin(2.0 * std::numbers::pi * 110.0 * k * static_cast<double>(i) / kSampleRate) / k;
        const double level = i < held ? 1.0 : 1.0 - static_cast<double>(i - held) / release;
        tone[i] = static_cast<float>(0.3 * level * x);
    }
    auto peakIn = [](const std::vector<float> &x, size_t from, size_t to) {
        float peak = 0.0f;
        bool finite = true;
        for (size_t i = from; i < std::min(to, x.size()); ++i) {
            finite = finite && std::isfinite(x[i]);
            peak = std::max(peak, std::abs(x[i]));
        }
        return finite ? peak : std::numeric_limits<float>::infinity();
    };
    {
        // A stack of three plays about as three shapers of one stage in a row, the first two linked (once the row's
        // DC blockers between them, which the stack hasn't, have settled; at a gentle drive, as the row cuts what its
        // stages put out past 20 kHz between them, and the stack, at twice the rate, not), and isn't one.
        const std::vector<float> stacked = throughEffects(stackParams(1, 3, 6.0), tone);
        const std::vector<float> row = throughEffects(stackParams(3, 1, 6.0, true), tone);
        const std::vector<float> single = throughEffects(stackParams(1, 1, 6.0), tone);
        // How far apart, in RMS against the stack's own, in dB, `other` taken as many samples later as matches best
        // (each shaper in the row going up to twice the rate and back adds a few samples).
        auto apartDb = [&](const std::vector<float> &other) {
            double best = std::numeric_limits<double>::infinity();
            for (size_t lag = 0; lag < 32; ++lag) {
                double diff = 0.0, own = 0.0;
                for (size_t i = held / 2; i < held; ++i) {
                    const double d = static_cast<double>(stacked[i]) - other[i + lag];
                    diff += d * d;
                    own += static_cast<double>(stacked[i]) * stacked[i];
                }
                best = std::min(best, 10.0 * std::log10(diff / own + 1e-30));
            }
            return best;
        };
        const double fromRow = apartDb(row), fromSingle = apartDb(single);
        std::printf("      a stack of 3 at 6 dB against 3 shapers in a row: %.1f dB apart; against 1: %.1f dB\n",
                    fromRow, fromSingle);
        check(fromRow < -18.0 && fromRow < fromSingle - 8.0,
              "a Stack curve's stack plays about as as many linked shapers in a row");
    }
    {
        // One stage at 0 dB passes a quiet 1 kHz sine at its level (its filters far from it), and at full drive
        // saturates a loud tone unevenly, one half reaching further than the other (once the DC blocker has centered
        // it), neither past 1.8 (its edges, nothing rounding them off, overshooting a little).
        std::vector<float> quiet(static_cast<size_t>(kSampleRate / 2));
        for (size_t i = 0; i < quiet.size(); ++i)
            quiet[i] = static_cast<float>(0.01 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / kSampleRate));
        const std::vector<float> passed = throughEffects(stackParams(1, 1, 0.0), quiet);
        double inSq = 0.0, outSq = 0.0;
        for (size_t i = 4800; i < quiet.size(); ++i) {
            inSq += static_cast<double>(quiet[i]) * quiet[i];
            outSq += static_cast<double>(passed[i]) * passed[i];
        }
        const double gainDb = 10.0 * std::log10(outSq / inSq);
        const std::vector<float> driven = throughEffects(stackParams(1, 1, 36.0), tone);
        float top = 0.0f, bottom = 0.0f;
        for (size_t i = 12000; i < held; ++i) { // once the DC blocker has settled
            top = std::max(top, driven[i]);
            bottom = std::min(bottom, driven[i]);
        }
        std::printf("      one stage: a quiet sine %.2f dB; at 36 dB of drive from %.3f to %.3f\n", gainDb, bottom, top);
        check(std::abs(gainDb) < 0.1 && std::abs(top + bottom) > 0.1f && top < 1.8f && -bottom < 1.8f,
              "a Stack curve's stage passes a quiet sine at its level and saturates a loud one unevenly");
    }
    {
        // No low-pass: stacked eight high at 0 dB, quiet sines from 1 to 15 kHz come out within a dB of their level
        // (at twice the rate, each stage lifting back what its anti-aliasing takes off the top).
        std::printf("      stacked 8 high, quiet sines:");
        double worst = 0.0;
        for (double hz : {1000.0, 5000.0, 10000.0, 15000.0}) {
            std::vector<float> quiet(static_cast<size_t>(kSampleRate / 4));
            for (size_t i = 0; i < quiet.size(); ++i)
                quiet[i] = static_cast<float>(0.01 * std::sin(2.0 * std::numbers::pi * hz * i / kSampleRate));
            const std::vector<float> passed = throughEffects(stackParams(1, 8, 0.0), quiet);
            double inSq = 0.0, outSq = 0.0;
            for (size_t i = 4800; i < quiet.size(); ++i) {
                inSq += static_cast<double>(quiet[i]) * quiet[i];
                outSq += static_cast<double>(passed[i]) * passed[i];
            }
            const double db = 10.0 * std::log10(outSq / inSq);
            worst = std::max(worst, std::abs(db));
            std::printf(" %.0f kHz %+.2f dB", hz / 1000.0, db);
        }
        std::printf("\n");
        check(worst < 1.0, "a Stack curve keeps its top: quiet sines to 15 kHz within a dB, stacked 8 high");
    }
    {
        // Stacked eight high and fully driven, what the stages' high-passes still hold as the note ends is driven on
        // by the stages after them: it rings on a moment past the note, further than a single stage, then stops.
        bool ringing8 = true, ringing1 = true;
        const std::vector<float> eight = throughEffects(stackParams(1, 8, 36.0), tone, &ringing8);
        const std::vector<float> one = throughEffects(stackParams(1, 1, 36.0), tone, &ringing1);
        const size_t end = held + release, ms = static_cast<size_t>(kSampleRate / 1000.0);
        auto lastHeard = [&](const std::vector<float> &x) { // ms after the note's end it last came over -60 dB
            size_t last = end;
            for (size_t i = end; i < x.size(); ++i)
                if (std::abs(x[i]) > 1e-3f)
                    last = i;
            return static_cast<double>(last - end) / ms;
        };
        const double heard8 = lastHeard(eight), heard1 = lastHeard(one);
        const float after8 = peakIn(eight, end + 1000 * ms, eight.size());
        const float after1 = peakIn(one, end + 1000 * ms, one.size());
        std::printf("      after the note, over -60 dB for %.0f ms stacked 8 high, %.0f ms with one stage; a second "
                    "on %.1e and %.1e, %s\n",
                    heard8, heard1, after8, after1, ringing8 || ringing1 ? "still running" : "stopped");
        check(heard8 > 20.0 && heard8 > 2.0 * heard1 && heard8 < 1000.0 && after8 < 1e-6f && after1 < 1e-6f &&
                  !ringing8 && !ringing1,
              "a Stack curve stacked high rings on past the note, longer than one stage, then stops");
    }
    {
        // The stack turned from 1 up to 8 and back down under the tone, in single frames: nothing louder than full
        // scale, nothing that isn't a number.
        const std::vector<float> moved =
            throughEffects(stackParams(1, 1, 24.0), tone, 1, [&](ParamValues &p, size_t t) {
                const double along = static_cast<double>(t) / static_cast<double>(held);
                p[paramIndex(0, FxParam::ShaperStack)] = along < 1.0 ? 1.0 + 7.0 * (1.0 - std::abs(2.0 * along - 1.0))
                                                                     : 1.0;
            });
        const float peak = peakIn(moved, 0, moved.size());
        std::printf("      the stack moved from 1 to 8 and back under a note: peak %.4f\n", peak);
        check(peak <= 2.0f, "a Stack curve's stack moved under a note stays finite, within the curve's reach");
    }
    {
        // A shaper effect switched from Soft to Stack and back under a quiet sine, which either curve passes about as
        // it is: the Stack path, at twice the rate, its half-bands a few samples late, fades in and out over the other
        // rather than jumping to it, so no sample steps further than the sine's own do.
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
        fx.prepare(kSampleRate);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
        p[paramIndex(0, FxParam::ShaperLink)] = 0.0;
        p[paramIndex(0, FxParam::ShaperOutput)] = 0.0;
        p[paramIndex(0, FxParam::ShaperMix)] = 1.0;
        p[paramIndex(0, FxParam::ShaperStack)] = 1.0;
        chain(p, {0});
        constexpr double kHz = 440.0, kLevel = 0.05;
        const uint32_t each = static_cast<uint32_t>(kSampleRate / 4);
        std::vector<float> left(kBlock), right(kBlock);
        double most = 0.0, last = 0.0, peak = 0.0;
        uint32_t t = 0;
        for (ClipMode mode : {ClipMode::Soft, ClipMode::Stack, ClipMode::Soft, ClipMode::Stack, ClipMode::Soft}) {
            p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(mode);
            for (uint32_t done = 0; done < each; done += kBlock, t += kBlock) {
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] =
                        static_cast<float>(kLevel * std::sin(2.0 * std::numbers::pi * kHz * (t + s) / kSampleRate));
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock; ++s) {
                    if (t + s > kSampleRate / 10) { // past the effect's fading in
                        most = std::max(most, static_cast<double>(std::abs(left[s] - last)));
                        peak = std::max(peak, static_cast<double>(std::abs(left[s])));
                    }
                    last = left[s];
                }
            }
        }
        const double steepest = most / (peak * 2.0 * std::numbers::pi * kHz / kSampleRate);
        std::printf("      Soft to Stack and back twice under a sine: steepest step x%.3f of the sine's\n", steepest);
        check(steepest < 1.15, "a shaper effect switched to Stack and back under a sine crossfades rather than clicks");
    }
    {
        // The low end thumps: as the note ends, what the stages' high-passes hold the stages after them drive on, so
        // far more comes out between 10 and 60 Hz in the 150 ms after it stacked eight high than with one stage.
        auto thump = [&](int stack) {
            const std::vector<float> out = throughEffects(stackParams(1, stack, 24.0), tone);
            const double low = 1.0 - std::exp(-2.0 * std::numbers::pi * 60.0 / kSampleRate);
            const double slow = 1.0 - std::exp(-2.0 * std::numbers::pi * 10.0 / kSampleRate);
            double a = 0.0, b = 0.0, c = 0.0, d = 0.0, end = 0.0;
            const size_t window = static_cast<size_t>(0.15 * kSampleRate), ended = held + release;
            for (size_t i = 0; i < ended + window; ++i) {
                a += (out[i] - a) * low;
                b += (a - b) * low;
                c += (out[i] - c) * slow;
                d += (c - d) * slow;
                if (i >= ended)
                    end = std::max(end, std::abs(b - d));
            }
            return end;
        };
        const double one = thump(1), eight = thump(8);
        std::printf("      10-60 Hz after the note: %.3f with one stage, %.3f stacked 8 high\n", one, eight);
        check(eight > 3.0 * one && eight > 0.3, "a Stack curve stacked high thumps its low end after the note");
    }
    {
        // A voice's shaper, the Stack curve stacked eight high at full drive, the operator heard only through it: the
        // voice rings on, and ends once the stages have rung out.
        Engine e(2);
        e.p[paramIndex(0, ShaperParam::Enabled)] = 1.0;
        e.p[paramIndex(0, ShaperParam::Mode)] = static_cast<double>(ClipMode::Stack);
        e.p[paramIndex(0, ShaperParam::Stack)] = 8.0;
        e.p[paramIndex(0, ShaperParam::Drive)] = 36.0;
        e.p[paramIndex(0, ShaperParam::Out)] = 1.0;
        e.p[shaperSendIndex(0, 0)] = 0.5;
        e.p[paramIndex(0, OpParam::Out)] = 0.0;
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.05);
        e.noteOn(45);
        const std::vector<float> note = e.render(static_cast<size_t>(kSampleRate / 2));
        const float notePeak = peakIn(note, 4800, note.size());
        e.noteOff(45);
        const std::vector<float> after = e.render(static_cast<size_t>(3.0 * kSampleRate));
        std::printf("      a voice's shaper stacked 8 high: peak %.3f, %d voice(s) left 3 s after its release\n",
                    notePeak, e.synth().activeVoiceCount());
        check(notePeak > 0.3f && std::isfinite(peakIn(after, 0, after.size())) && e.synth().activeVoiceCount() == 0,
              "a voice's Stack curve sounds stacked high, and the voice ends once it has rung out");
    }
}

// The sequencer: where its notes start and end, against the host's transport and from keys.
// A note's own bend (see SeqNoteBendParam): in the sequencer's plans, and through the synth.
void noteBendChecks() {
    constexpr double kStep = kSampleRate * 60.0 / 120.0 / 4.0; // a 1/16 step at 120 bpm: 6000 samples
    auto defaults = [] {
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        return p;
    };
    auto note = [](ParamValues &p, int slot, double start, double length, int pitch) {
        p[seqNoteIndex(0, slot, SeqNoteParam::Start)] = start;
        p[seqNoteIndex(0, slot, SeqNoteParam::Length)] = length;
        p[seqNoteIndex(0, slot, SeqNoteParam::Pitch)] = pitch;
    };
    auto bend = [](ParamValues &p, int slot, std::vector<SeqCurvePoint> points, int pattern = 0) {
        p[seqNoteBendCountIndex(pattern, slot)] = static_cast<double>(points.size());
        for (size_t i = 0; i < points.size(); ++i) {
            const int at = static_cast<int>(i);
            p[seqNoteBendIndex(pattern, slot, at, PointField::X)] = points[i].x;
            p[seqNoteBendIndex(pattern, slot, at, PointField::Y)] = points[i].y;
            p[seqNoteBendIndex(pattern, slot, at, PointField::Curve)] = points[i].curve;
        }
    };

    // The plans: a note 8 steps long bending a semitone a step. From the song's start, its event starting it unbent and
    // each stretch taking it on to where it is at the stretch's end; chased from step 4, starting bent by 4; with Gate
    // at 50 %, ending at step 4 holding the 4 it reached there.
    {
        ParamValues p = defaults();
        p[paramIndex(SeqParam::Enabled)] = 1.0;
        p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Host);
        note(p, 0, 0.0, 8.0, 60);
        bend(p, 0, {{0.0, 0.0, 0.0}, {8.0, 8.0, 0.0}});
        struct Planned {
            double onBend = std::numeric_limits<double>::quiet_NaN();
            double worst = 0.0;    // the furthest a stretch's bend ends from where it should
            double gap = 0.0;      // and starts from where the stretch before took it
            double highest = -1.0; // the highest it went
            double lastEnd = 0.0;  // where the last stretch with a bend ended, in steps
        };
        auto run = [&](const ParamValues &q, double beats, double steps, double lasts) {
            Sequencer seq;
            seq.prepare(kSampleRate);
            seq.configure(q);
            std::array<SeqEvent, Sequencer::kMaxEvents> out;
            Planned r;
            double before = std::numeric_limits<double>::quiet_NaN();
            for (double done = 0.0; done < steps * kStep;) {
                uint32_t covered = 0;
                const int n = seq.plan(512, 120.0, true, beats + done / kSampleRate * 2.0, out, covered);
                for (int i = 0; i < n; ++i)
                    if (out[i].on && std::isnan(r.onBend))
                        r.onBend = out[i].bend;
                done += covered;
                const double end = beats * 4.0 + done / kStep;
                for (const SeqBend &b : seq.bends()) {
                    r.worst = std::max(r.worst, std::abs(b.to - std::min(end, lasts)));
                    if (b.frame == 0 && std::isfinite(before))
                        r.gap = std::max(r.gap, std::abs(b.from - before));
                    r.highest = std::max(r.highest, b.to);
                    r.lastEnd = end;
                    before = b.to;
                }
            }
            return r;
        };
        const Planned plain = run(p, 0.0, 12.0, 8.0), chased = run(p, 1.0, 4.0, 8.0);
        ParamValues gated = p;
        gated[paramIndex(SeqParam::Gate)] = 0.5;
        const Planned half = run(gated, 0.0, 12.0, 4.0);
        std::printf("      a note's bend: starting at %.3f, off by %.2g at most (%.2g between stretches); chased from step "
                    "4, at %.3f; gated to 4 steps, reaching %.3f, its last stretch ending at step %.2f\n",
                    plain.onBend, plain.worst, plain.gap, chased.onBend, half.highest, half.lastEnd);
        check(plain.onBend == 0.0 && plain.worst < 1e-6 && plain.gap < 1e-6 && std::abs(plain.highest - 8.0) < 1e-9,
              "the sequencer plans a note's bend from where the note starts, stretch after stretch");
        check(std::abs(chased.onBend - 4.0) < 1e-6, "a note chased in its middle starts where its bend is there");
        check(half.worst < 1e-6 && std::abs(half.highest - 4.0) < 1e-9 && half.lastEnd < 4.0 + 512.0 / kStep + 1e-9,
              "a note Gate shortens holds the bend it reached at its end");
    }

    // Through the synth, in Key. The rising zero crossings of what it plays, and its pitch between two times (samples).
    auto crossings = [](const std::vector<float> &x) {
        std::vector<double> at;
        for (size_t i = 1; i < x.size(); ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                at.push_back(static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]));
        return at;
    };
    auto hzBetween = [&](const std::vector<float> &x, double from, double to) {
        std::vector<double> in;
        for (double t : crossings(x))
            if (t >= from && t <= to)
                in.push_back(t);
        return in.size() < 2 ? 0.0 : (in.size() - 1) * kSampleRate / (in.back() - in.front());
    };
    auto power = [](const std::vector<float> &x, size_t from, double hz) {
        std::complex<double> sum = 0.0;
        for (size_t i = from; i < from + kFftSize && i < x.size(); ++i)
            sum += static_cast<double>(x[i]) *
                   std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate);
        return std::norm(sum);
    };
    // Plays the pattern `setup` writes from a key, the first sample its first; `mono` in mono legato with a glide.
    auto play = [&](const auto &setup, double steps, bool mono = false) {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        setup(e.p);
        if (mono) {
            e.synth().setPlayMode(true, true, false);
            e.p[paramIndex(GlobalParam::Glide)] = normFromTime(0.3);
        }
        e.synth().setTempo(120.0);
        e.render(256); // the sequencer on, taking keys
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        return e.render(static_cast<size_t>(steps * kStep));
    };
    auto semitones = [](double hz, double from) { return 12.0 * std::log2(hz / from); };

    // An A4 held 2 steps, bent up 2 semitones over the next 2 and held there: A4, then a semitone up halfway, then B4.
    {
        const std::vector<float> x = play([&](ParamValues &p) {
            note(p, 0, 0.0, 16.0, 69);
            bend(p, 0, {{0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}, {4.0, 2.0, 0.0}});
        }, 6.0);
        const double before = hzBetween(x, 0.5 * kStep, 1.5 * kStep), halfway = hzBetween(x, 2.9 * kStep, 3.1 * kStep),
                     after = hzBetween(x, 4.5 * kStep, 5.5 * kStep);
        std::printf("      through the synth: %.2f Hz, then %.2f halfway along the bend and %.2f after it\n", before,
                    halfway, after);
        check(std::abs(semitones(before, 440.0)) < 0.02 && std::abs(semitones(halfway, 440.0) - 1.0) < 0.05 &&
                  std::abs(semitones(after, 440.0) - 2.0) < 0.02,
              "a sequencer note's bend bends it, along its points");
    }
    // A sweep of two octaves in a step, which a bend stepped once a stretch would take in stairs of a semitone: the pitch
    // following it smoothly.
    {
        const std::vector<float> x = play([&](ParamValues &p) {
            note(p, 0, 0.0, 4.0, 57);
            bend(p, 0, {{0.0, 0.0, 0.0}, {1.0, 24.0, 0.0}});
        }, 1.2);
        const std::vector<double> at = crossings(x);
        double worst = 0.0;
        for (size_t i = 1; i < at.size(); ++i) {
            const double mid = 0.5 * (at[i] + at[i - 1]);
            if (mid < 300.0 || mid > kStep - 300.0)
                continue;
            const double heard = semitones(kSampleRate / (at[i] - at[i - 1]), 220.0);
            worst = std::max(worst, std::abs(heard - 24.0 * mid / kStep));
        }
        std::printf("      a sweep of 24 semitones in a step following its line within %.3f semitones\n", worst);
        check(worst < 0.3, "a sequencer note's bend moves its pitch smoothly, not stretch by stretch");
    }
    // A chord of E4 and A4, the A4 bent up 2: the E4 playing on as it was, the A4 gone up to B4.
    {
        const std::vector<float> x = play([&](ParamValues &p) {
            note(p, 0, 0.0, 8.0, 64);
            note(p, 1, 0.0, 8.0, 69);
            bend(p, 1, {{0.0, 0.0, 0.0}, {1.0, 2.0, 0.0}});
        }, 6.0);
        const size_t from = static_cast<size_t>(2.0 * kStep);
        const double e4 = power(x, from, 329.63), a4 = power(x, from, 440.0), b4 = power(x, from, 493.88);
        std::printf("      a chord: E4 %.1f dB and A4 %.1f dB from B4\n", 10.0 * std::log10(e4 / b4),
                    10.0 * std::log10(a4 / b4));
        check(e4 > 0.25 * b4 && a4 < 0.01 * b4, "a sequencer note's bend bends its own voice alone");
    }
    // Mono legato, gliding: an A4 bent up 2 sliding into a B4 starts the glide from where it was bent to, so the pitch
    // stays at B4 with no dip back toward A4.
    {
        const std::vector<float> x = play([&](ParamValues &p) {
            note(p, 0, 0.0, 4.0, 69);
            p[seqNoteIndex(0, 0, SeqNoteParam::Slide)] = 1.0;
            bend(p, 0, {{0.0, 0.0, 0.0}, {1.0, 2.0, 0.0}});
            note(p, 1, 4.0, 4.0, 71);
        }, 6.0, true);
        const double before = hzBetween(x, 3.0 * kStep, 3.9 * kStep), joined = hzBetween(x, 4.05 * kStep, 4.4 * kStep);
        std::printf("      sliding on from a bend: %.2f Hz, then %.2f into the next note\n", before, joined);
        check(std::abs(semitones(before, 493.88)) < 0.03 && std::abs(semitones(joined, 493.88)) < 0.05,
              "a mono glide starts where the sequencer bent the note before");
    }
    // A patch with bends, the last note of the last pattern among them, loading back as saved.
    {
        ParamValues p = defaults();
        bend(p, kSeqNotes - 1, {{0.0, -3.0, 0.5}, {1.25, 0.0, 0.0}, {6.0, 24.0, -1.0}}, kSeqPatterns - 1);
        bend(p, 0, {{2.0, -24.0, 0.0}});
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, gui::UiLayout{}, true);
        MemoryReader reader{bytes.data(), bytes.size(), 0};
        const clap_istream_t in = memoryIn(reader);
        ParamValues loaded;
        readParams(&in, loaded);
        check(std::equal(loaded.begin(), loaded.end(), p.begin()), "a patch's note bends load back as saved");
    }
}

void sequencerChecks() {
    std::printf("sequencer:\n");
    constexpr double kStep = kSampleRate * 60.0 / 120.0 / 4.0; // a 1/16 step at 120 bpm: 6000 samples
    auto blank = [] {
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(SeqParam::Enabled)] = 1.0;
        p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Host);
        return p;
    };
    auto note = [](ParamValues &p, int slot, double start, double length, int pitch, int pattern = 0) {
        p[seqNoteIndex(pattern, slot, SeqNoteParam::Start)] = start;
        p[seqNoteIndex(pattern, slot, SeqNoteParam::Length)] = length;
        p[seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)] = pitch;
    };
    struct Event {
        double frame;
        bool on;
        int key;
    };
    // Plans `frames` from the song's start with the transport playing at 120 bpm, in stretches of `piece`; the host's
    // position off by `jitter` beats each stretch, early and late in turn, as rounding leaves it.
    auto plan = [](const ParamValues &p, double frames, uint32_t piece = 512, double jitter = 0.0) {
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(p);
        std::vector<Event> events;
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        int stretch = 0;
        for (double done = 0.0; done < frames;) {
            uint32_t covered = 0;
            const double off = stretch++ % 2 == 0 ? -jitter : jitter;
            const int n = seq.plan(piece, 120.0, true, done / kSampleRate * 2.0 + off, out, covered);
            for (int i = 0; i < n; ++i)
                events.push_back({done + out[i].frame, out[i].on, out[i].key});
            done += covered;
        }
        return events;
    };
    auto near = [](double a, double b) { return std::abs(a - b) <= 1.0; };

    // Two notes, at steps 0 and 4, a step long each: on at their steps, off a step later, the bar over and again.
    {
        ParamValues p = blank();
        note(p, 0, 0.0, 1.0, 60);
        note(p, 1, 4.0, 1.0, 64);
        const std::vector<Event> e = plan(p, 16.5 * kStep);
        std::printf("      %zu events; the second note on at %.0f, off at %.0f; the first again at %.0f\n", e.size(),
                    e.size() > 3 ? e[2].frame : -1.0, e.size() > 3 ? e[3].frame : -1.0, e.size() > 4 ? e[4].frame : -1.0);
        check(e.size() == 5 && e[0].on && e[0].key == 60 && near(e[0].frame, 0.0) && !e[1].on &&
                  near(e[1].frame, kStep) && e[2].on && e[2].key == 64 && near(e[2].frame, 4 * kStep) && !e[3].on &&
                  near(e[3].frame, 5 * kStep) && e[4].on && near(e[4].frame, 16 * kStep),
              "the sequencer plays its notes on their steps with the host's transport, the bar round again");
        // Planned in other stretches, the same.
        const std::vector<Event> odd = plan(p, 16.5 * kStep, 37);
        bool same = odd.size() == e.size();
        for (size_t i = 0; same && i < e.size(); ++i)
            same = odd[i].on == e[i].on && odd[i].key == e[i].key && near(odd[i].frame, e[i].frame);
        check(same, "the sequencer plans alike whatever the stretches");
        // Stretches a step long, each starting where a step does, the host's position a hair early or late there (as
        // a position handed in seconds and turned into beats comes): each note on once, as without, neither missed
        // where the stretch before ended short of its step nor played twice where it ended past it.
        const std::vector<Event> exact = plan(p, 16.5 * kStep, static_cast<uint32_t>(kStep));
        const std::vector<Event> jittered = plan(p, 16.5 * kStep, static_cast<uint32_t>(kStep), 1e-9);
        bool once = jittered.size() == exact.size() && exact.size() == 5;
        for (size_t i = 0; once && i < exact.size(); ++i)
            once = jittered[i].on == exact[i].on && jittered[i].key == exact[i].key &&
                   near(jittered[i].frame, exact[i].frame);
        std::printf("      stretches a step long, the host's position a hair off at each: %zu events (%zu as it is)\n",
                    jittered.size(), exact.size());
        check(once, "a position a hair off where a stretch starts on a step neither misses its notes nor plays them twice");
    }
    // It chases notes: the transport starting in the middle of a note plays it at once, for what is left of it, once,
    // but not one its chance leaves out, nor one already over; a note put down under the playhead plays as it is put
    // down; and from a key, or where another pattern takes over, none from before it (a note ringing across the loop's
    // end) starts.
    {
        // Plans `steps` with the transport playing from step `fromStep`, in stretches of 512; `edit` changes the params
        // (and the sequencer reads them again) after `editAfter` steps.
        auto planFrom = [&](ParamValues p, double fromStep, double steps, double editAfter = -1.0,
                            const std::function<void(ParamValues &)> &edit = {}) {
            Sequencer seq;
            seq.prepare(kSampleRate);
            seq.configure(p);
            std::vector<Event> events;
            std::array<SeqEvent, Sequencer::kMaxEvents> out;
            bool edited = false;
            for (double done = 0.0; done < steps * kStep;) {
                if (!edited && editAfter >= 0.0 && done >= editAfter * kStep) {
                    edit(p);
                    seq.configure(p);
                    edited = true;
                }
                uint32_t covered = 0;
                const int n = seq.plan(512, 120.0, true, (fromStep + done / kStep) / 4.0, out, covered);
                for (int i = 0; i < n; ++i)
                    events.push_back({done + out[i].frame, out[i].on, out[i].key});
                done += covered;
            }
            return events;
        };
        ParamValues p = blank();
        note(p, 0, 0.0, 4.0, 60);
        const std::vector<Event> mid = planFrom(p, 2.5, 4.0);
        const std::vector<Event> after = planFrom(p, 4.5, 4.0);
        ParamValues unlikely = p;
        unlikely[seqNoteIndex(0, 0, SeqNoteParam::Chance)] = 0.0;
        const std::vector<Event> skipped = planFrom(unlikely, 2.5, 4.0);
        std::printf("      started on step 2.5 of a note 4 steps long: %zu events, on at %.0f, off at %.0f\n", mid.size(),
                    mid.size() > 1 ? mid[0].frame : -1.0, mid.size() > 1 ? mid[1].frame : -1.0);
        check(mid.size() == 2 && mid[0].on && mid[0].key == 60 && near(mid[0].frame, 0.0) && !mid[1].on &&
                  near(mid[1].frame, 1.5 * kStep) && after.empty() && skipped.empty(),
              "the sequencer starting in the middle of a note plays what is left of it, once");
        // A note put down across the playhead, a step after it went by its start, plays from there, once.
        ParamValues empty = blank();
        const std::vector<Event> put =
            planFrom(empty, 0.0, 8.0, 2.0, [&](ParamValues &q) { note(q, 0, 1.0, 4.0, 62); });
        std::printf("      a note put down under the playhead: %zu events, on at %.0f, off at %.0f\n", put.size(),
                    put.size() > 1 ? put[0].frame : -1.0, put.size() > 1 ? put[1].frame : -1.0);
        check(put.size() == 2 && put[0].on && put[0].key == 62 && std::abs(put[0].frame - 2.0 * kStep) <= 512.0 &&
                  !put[1].on && near(put[1].frame, 5.0 * kStep),
              "a note put down under the playhead plays at once");
        // A note on the last step, ringing 4 steps on across the loop's end: from a key, nothing at the start; another
        // pattern taking over, nothing as it does.
        ParamValues ringing = blank();
        note(ringing, 0, 15.0, 4.0, 64);
        ParamValues keyed = ringing;
        keyed[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(keyed);
        seq.keyDown(60);
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        uint32_t covered = 0;
        const int fromKey = seq.plan(512, 120.0, true, 0.0, out, covered);
        ParamValues switching = blank();
        note(switching, 0, 15.0, 4.0, 64, 1);
        const std::vector<Event> switched = planFrom(switching, 0.0, 33.0, 4.0, [](ParamValues &q) {
            q[paramIndex(SeqParam::Pattern)] = 1.0;
        });
        std::printf("      a note across the loop's end: %d events from a key; another pattern's first on at step %.2f\n",
                    fromKey, switched.empty() ? -1.0 : switched[0].frame / kStep);
        check(fromKey == 0 && !switched.empty() && near(switched[0].frame, 31.0 * kStep),
              "a key, or another pattern taking over, chases no note from before it");
    }
    // The Speed reads as a multiple of a 1/16 step, with a times sign in UTF-8 (U+00D7), and reads back.
    {
        const int speed = paramIndex(SeqParam::Rate);
        const double threeQuarters = static_cast<double>(SeqRate::EighthTriplet); // x3/4: a step of a 1/8 triplet
        char text[32];
        formatParam(speed, threeQuarters, text, sizeof(text));
        double typed = -1.0;
        check(std::string(text) == "\xC3\x97" "3/4" && parseParam(speed, text, &typed) && typed == threeQuarters,
              "the sequencer's Speed reads as a times sign and its multiple, and reads back");
    }
    // Swing at 75 % puts the step after the beat three quarters of the way to the next; Gate at 50 % halves a note.
    {
        ParamValues p = blank();
        note(p, 0, 1.0, 1.0, 60);
        p[paramIndex(SeqParam::Swing)] = 0.75;
        p[paramIndex(SeqParam::Gate)] = 0.5;
        const std::vector<Event> e = plan(p, 4 * kStep);
        std::printf("      swung at 75 %%, gated at 50 %%: on at %.0f, off at %.0f\n", e.size() > 1 ? e[0].frame : -1.0,
                    e.size() > 1 ? e[1].frame : -1.0);
        check(e.size() == 2 && near(e[0].frame, 1.5 * kStep) && near(e[1].frame, 1.75 * kStep),
              "Swing delays every other step and Gate shortens the notes");
    }
    // Swing, Gate, Velocity and Humanize as modulation takes them (Sequencer::setFeel()), in place of what configure()
    // read: a Gate of 50 % handed it halves a note set at 100 %.
    {
        ParamValues p = blank();
        note(p, 0, 0.0, 1.0, 60);
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(p);
        seq.setFeel({0.5, 0.5, 1.0, 0.0});
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        std::vector<Event> events;
        for (double done = 0.0; done < 4.0 * kStep;) {
            uint32_t covered = 0;
            const int n = seq.plan(512, 120.0, true, done / kStep / 4.0, out, covered);
            for (int i = 0; i < n; ++i)
                events.push_back({done + out[i].frame, out[i].on, out[i].key});
            done += covered;
        }
        check(events.size() == 2 && near(events[0].frame, 0.0) && near(events[1].frame, 0.5 * kStep),
              "the sequencer plays Swing, Gate, Velocity and Humanize as modulation hands them to it");
    }
    // Through the synth, playing from a key: a macro's slot on Gate, the host's modulation of Swing and an LFO's slot on
    // Velocity take them where the sequencer plays them, and the telemetry says how far: the macro's full knob a quarter
    // of Gate's range down, the host's 0.1 up, the LFO swinging Velocity as the notes play.
    {
        auto t = std::make_unique<Telemetry>();
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        note(e.p, 0, 0.0, 1.0, 69);
        note(e.p, 1, 2.0, 1.0, 69);
        const int gate = paramIndex(SeqParam::Gate), swing = paramIndex(SeqParam::Swing);
        const int velocity = paramIndex(SeqParam::Velocity);
        e.p[paramIndex(0, MacroParam::Value)] = 1.0;
        e.p[slotTargetIndex(kMaxSources, 0)] = slotTargetValue(gate);
        e.p[slotAmountIndex(kMaxSources, 0)] = -0.25;
        e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
        e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
        e.p[paramIndex(0, SourceParam::Rate)] = 0.8; // a few Hz
        e.p[slotTargetIndex(0, 0)] = slotTargetValue(velocity);
        e.p[slotAmountIndex(0, 0)] = -0.5;
        e.synth().setTempo(120.0);
        e.render(256);
        e.synth().paramMod(swing, {-1, -1, -1, -1}, 0.1);
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        float lfoMost = 0.0f;
        for (int i = 0; i < 40; ++i) {
            e.render(480);
            e.synth().telemetry(*t);
            lfoMost = std::max(lfoMost, std::abs(t->seqFeel[2]));
        }
        const double gateRange = paramTable()[gate].max - paramTable()[gate].min;
        std::printf("      the sequencer's feel modulated: Gate %+.4f (a quarter of its range down is %+.4f), Swing %+.4f, "
                    "Velocity up to %.4f\n",
                    t->seqFeel[1], -0.25 * gateRange, t->seqFeel[0], lfoMost);
        check(std::abs(t->seqFeel[1] + 0.25 * gateRange) < 1e-4 && std::abs(t->seqFeel[0] - 0.1f) < 1e-5f &&
                  lfoMost > 0.05f && isModTarget(gate) && isModTarget(swing) && isModTarget(velocity) &&
                  isModTarget(paramIndex(SeqParam::Humanize)) && !isModTarget(paramIndex(SeqParam::Octave)),
              "a macro, the host and an LFO modulate the sequencer's Gate, Swing and Velocity");
    }
    // A macro's slot on Gate whose aux runs in each voice (velocity) reaches the sequencer too, from the newest voice's
    // slots: the macro's full knob a quarter of Gate's range down, scaled by the velocity of the note the sequencer plays.
    {
        auto t = std::make_unique<Telemetry>();
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        note(e.p, 0, 0.0, 1.0, 69);
        const int gate = paramIndex(SeqParam::Gate);
        e.p[paramIndex(0, MacroParam::Value)] = 1.0;
        e.p[slotTargetIndex(kMaxSources, 0)] = slotTargetValue(gate);
        e.p[slotAmountIndex(kMaxSources, 0)] = -0.25;
        e.p[slotAuxIndex(kMaxSources, 0)] = slotAuxValue(kVelocitySource);
        e.synth().setTempo(120.0);
        e.render(256);
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        for (int i = 0; i < 10; ++i)
            e.render(480);
        e.synth().telemetry(*t);
        const double gateRange = paramTable()[gate].max - paramTable()[gate].min;
        const double played = e.p[seqNoteIndex(0, 0, SeqNoteParam::Velocity)];
        std::printf("      Gate by a macro scaled by velocity: %+.4f (a quarter of its range down at velocity %.3f is "
                    "%+.4f)\n",
                    t->seqFeel[1], played, -0.25 * gateRange * played);
        check(played > 0.0 && std::abs(t->seqFeel[1] + 0.25 * gateRange * played) < 1e-4,
              "a macro's slot on the sequencer's Gate, its aux running in each voice, modulates it");
    }
    // The pattern picked takes modulation in whole steps: a macro's slot on it, a third of its range at the macro's full
    // knob, plays B from a key where A is set; at the knob's middle (half a step) it still plays A; and the telemetry
    // says how far it takes the pattern.
    {
        auto t = std::make_unique<Telemetry>();
        auto playing = [&](double knob) {
            Engine e(1);
            e.p[paramIndex(SeqParam::Enabled)] = 1.0;
            e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
            note(e.p, 0, 0.0, 4.0, 60, 0);
            note(e.p, 0, 0.0, 4.0, 72, 1);
            e.p[paramIndex(0, MacroParam::Value)] = knob;
            e.p[slotTargetIndex(kMaxSources, 0)] = slotTargetValue(paramIndex(SeqParam::Pattern));
            e.p[slotAmountIndex(kMaxSources, 0)] = 1.0 / 3.0;
            e.synth().setTempo(120.0);
            e.render(256);
            e.synth().noteOn({1, 0, 0, 60}, 1.0);
            e.render(2400);
            e.synth().telemetry(*t);
            return std::pair{t->seqPattern, t->seqFeel[4]};
        };
        const auto [full, fullOffset] = playing(1.0);
        const auto [half, halfOffset] = playing(0.4);
        std::printf("      a macro on the pattern: pattern %d at its full knob (%+.3f), %d at 0.4 (%+.3f)\n", full,
                    fullOffset, half, halfOffset);
        check(full == 1 && std::abs(fullOffset - 1.0f) < 1e-4f && half == 0 && isModTarget(paramIndex(SeqParam::Pattern)),
              "modulation picks the sequencer's pattern in whole steps");
    }
    // Reversed, the step a bar plays first is its last: a note on step 0 plays at step 15. Its loop from step 4 to 8
    // leaves out a note on step 0, and plays one on step 5 every four steps.
    {
        ParamValues p = blank();
        note(p, 0, 0.0, 0.5, 60);
        p[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Reverse);
        const std::vector<Event> back = plan(p, 16 * kStep);
        p[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Forward);
        note(p, 1, 5.0, 0.5, 62);
        p[paramIndex(0, SeqPatternParam::LoopStart)] = 4.0;
        p[paramIndex(0, SeqPatternParam::LoopEnd)] = 8.0;
        const std::vector<Event> looped = plan(p, 8 * kStep);
        int ons = 0;
        bool only62 = true;
        for (const Event &ev : looped)
            if (ev.on) {
                ++ons;
                only62 = only62 && ev.key == 62;
            }
        check(!back.empty() && near(back[0].frame, 15 * kStep) && ons == 2 && only62 && near(looped[0].frame, kStep),
              "Reverse plays the loop backward, and the loop plays only the notes in it");
    }
    // The playhead moves on steadily, backward through each step reversed (and on Ping-Pong's way back), forward
    // otherwise: from one stretch of 512 frames to the next, never further than a stretch's share of a step (but where
    // the loop comes round, or Ping-Pong turns, by a step).
    for (SeqOrder order : {SeqOrder::Forward, SeqOrder::Reverse, SeqOrder::PingPong}) {
        ParamValues p = blank();
        note(p, 0, 0.0, 0.5, 60);
        p[paramIndex(SeqParam::Order)] = static_cast<double>(order);
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(p);
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        const double share = 512.0 / kStep;
        float last = -1.0f;
        int jumps = 0, wrongWay = 0;
        for (double done = 0.0; done < 40 * kStep;) {
            uint32_t covered = 0;
            seq.plan(512, 120.0, true, done / kSampleRate * 2.0, out, covered);
            done += covered;
            const float now = seq.position();
            if (last >= 0.0f) {
                const double moved = now - last;
                if (std::abs(moved) > share + 1e-3)
                    ++jumps;
                else if ((order == SeqOrder::Reverse) != (moved < 0.0) && order != SeqOrder::PingPong)
                    ++wrongWay;
            }
            last = now;
        }
        // 40 steps: the loop round twice (Forward, Reverse at steps 16 and 32), or Ping-Pong turning twice (16 and 32).
        const int expected = 2;
        std::printf("      playhead, %s: %d jumps, %d the wrong way\n",
                    order == SeqOrder::Forward ? "forward" : order == SeqOrder::Reverse ? "reversed" : "ping-pong", jumps,
                    wrongWay);
        check(jumps <= expected && wrongWay == 0, "the playhead moves on steadily the way the pattern plays");
    }
    // The curves read where the playhead is: a bend from -1 at step 0 to 1 at step 16 stands at -0.5 on step 4 (and at
    // 0.5 played reversed, back through step 12), a segment bent by 1 lower than straight, a mod wheel of one point at
    // 0.75 everywhere, and a curve without points, or a sequencer not playing, nothing.
    {
        auto curveAt = [&](const ParamValues &p, double step, SeqCurve curve) {
            Sequencer seq;
            seq.prepare(kSampleRate);
            seq.configure(p);
            std::array<SeqEvent, Sequencer::kMaxEvents> out;
            uint32_t covered = 0;
            seq.plan(64, 120.0, true, step / 4.0, out, covered); // a 1/16 step is a quarter of a beat
            return seq.curve(curve);
        };
        auto point = [](ParamValues &p, SeqCurve curve, int i, double x, double y, double bend = 0.0) {
            p[seqCurveIndex(0, curve, i, PointField::X)] = x;
            p[seqCurveIndex(0, curve, i, PointField::Y)] = y;
            p[seqCurveIndex(0, curve, i, PointField::Curve)] = bend;
            p[seqCurveCountIndex(0, curve)] = std::max(p[seqCurveCountIndex(0, curve)], i + 1.0);
        };
        ParamValues p = blank();
        const double none = curveAt(p, 4.0, SeqCurve::Bend);
        point(p, SeqCurve::Bend, 0, 0.0, -1.0);
        point(p, SeqCurve::Bend, 1, 16.0, 1.0);
        point(p, SeqCurve::ModWheel, 0, 3.0, 0.75);
        const double bend = curveAt(p, 4.0, SeqCurve::Bend), wheel = curveAt(p, 9.0, SeqCurve::ModWheel);
        const double before = curveAt(p, 1.0, SeqCurve::ModWheel); // before its only point: that point's value
        p[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Reverse);
        const double reversed = curveAt(p, 4.0, SeqCurve::Bend);
        p[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Forward);
        p[seqCurveIndex(0, SeqCurve::Bend, 0, PointField::Curve)] = 1.0;
        const double bent = curveAt(p, 8.0, SeqCurve::Bend);
        p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key); // no key held: not playing
        const double idle = curveAt(p, 4.0, SeqCurve::Bend);
        std::printf("      curves: bend %.3f on step 4, %.3f reversed, %.3f bent on step 8; mod wheel %.3f, %.3f\n", bend,
                    reversed, bent, wheel, before);
        check(std::isnan(none) && std::abs(bend + 0.5) < 1e-3 && std::abs(reversed - 0.5) < 1e-3 && bent < -0.5 &&
                  std::abs(wheel - 0.75) < 1e-9 && std::abs(before - 0.75) < 1e-9 && std::isnan(idle),
              "the sequencer's curves stand where the playhead is, and do nothing without points");

        // Round the pattern's end: from a point at 1 on step 12 on to one at 0 on step 4, three quarters of the way up on
        // step 14, half on step 0 and a quarter on step 2; bent by the last point's curve, and a point past the pattern's
        // end left out.
        ParamValues w = blank();
        point(w, SeqCurve::Bend, 0, 4.0, 0.0);
        point(w, SeqCurve::Bend, 1, 12.0, 1.0);
        const double late = curveAt(w, 14.0, SeqCurve::Bend), round = curveAt(w, 0.0, SeqCurve::Bend),
                     early = curveAt(w, 2.0, SeqCurve::Bend), inside = curveAt(w, 8.0, SeqCurve::Bend);
        point(w, SeqCurve::Bend, 2, 40.0, -1.0);
        const double leftOut = curveAt(w, 14.0, SeqCurve::Bend);
        w[seqCurveIndex(0, SeqCurve::Bend, 1, PointField::Curve)] = 1.0;
        const double bentRound = curveAt(w, 0.0, SeqCurve::Bend);
        std::printf("      curves round the end: %.3f on step 14, %.3f on 0, %.3f on 2; bent %.3f on 0\n", late, round, early,
                    bentRound);
        check(std::abs(late - 0.75) < 1e-3 && std::abs(round - 0.5) < 1e-3 && std::abs(early - 0.25) < 1e-3 &&
                  std::abs(inside - 0.5) < 1e-3 && std::abs(leftOut - late) < 1e-9 &&
                  std::abs(bentRound - (1.0 - shapeBend(0.5, 1.0))) < 1e-3,
              "the sequencer's curves wrap around the pattern's end from their last point to their first");
    }
    // Through the synth, in Key: a bend curve at 1 bends an A4 the sequencer plays up by Bend Range (two semitones, to
    // B4); a mod wheel curve at 1 takes the mod wheel's slot on A's level all the way, silencing it.
    {
        auto play = [&](double bend, double wheel) {
            Engine e(1);
            e.p[paramIndex(SeqParam::Enabled)] = 1.0;
            e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
            note(e.p, 0, 0.0, 16.0, 69);
            e.p[slotTargetIndex(kWheelSource, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(kWheelSource, 0)] = -1.0;
            for (const auto &[curve, v] : {std::pair{SeqCurve::Bend, bend}, std::pair{SeqCurve::ModWheel, wheel}})
                if (std::isfinite(v)) {
                    e.p[seqCurveCountIndex(0, curve)] = 1.0;
                    e.p[seqCurveIndex(0, curve, 0, PointField::Y)] = v;
                }
            e.synth().setTempo(120.0);
            e.render(256); // the sequencer on, taking keys
            e.synth().noteOn({1, 0, 0, 60}, 1.0);
            e.render(4800);
            return e.render(8192);
        };
        auto power = [](const std::vector<float> &x, double hz) {
            std::complex<double> sum = 0.0;
            for (size_t i = 0; i < x.size(); ++i)
                sum += static_cast<double>(x[i]) *
                       std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate);
            return std::norm(sum);
        };
        auto rms = [](const std::vector<float> &x) {
            double sum = 0.0;
            for (float s : x)
                sum += static_cast<double>(s) * s;
            return std::sqrt(sum / static_cast<double>(x.size()));
        };
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<float> plain = play(nan, nan), bent = play(1.0, nan), quiet = play(nan, 1.0);
        const double b4 = 440.0 * std::pow(2.0, 2.0 / 12.0);
        std::printf("      through the synth: bent, B4 %.1f dB over A4; the mod wheel curve leaving %.4f of %.4f\n",
                    10.0 * std::log10(power(bent, b4) / power(bent, 440.0)), rms(quiet), rms(plain));
        check(power(plain, 440.0) > 100.0 * power(plain, b4) && power(bent, b4) > 100.0 * power(bent, 440.0) &&
                  rms(quiet) < 0.01 * rms(plain),
              "the bend curve bends what the sequencer plays, and the mod wheel curve moves the mod wheel");
    }
    noteBendChecks();
    // The telemetry tells the wheels where the curves take them as they play: the bend curve on top of the Pitch Bend
    // param as set, and what the mod wheel curve adds. Nothing before they play, or once they stop, by the key let go or
    // the sequencer switched off.
    {
        auto t = std::make_unique<Telemetry>();
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        e.p[paramIndex(GlobalParam::PitchBend)] = 0.25;
        note(e.p, 0, 0.0, 16.0, 69);
        for (const auto &[curve, v] : {std::pair{SeqCurve::Bend, 0.5}, std::pair{SeqCurve::ModWheel, 0.75}}) {
            e.p[seqCurveCountIndex(0, curve)] = 1.0;
            e.p[seqCurveIndex(0, curve, 0, PointField::Y)] = v;
        }
        e.synth().setTempo(120.0);
        struct Shown {
            float bend, bendSet, wheel;
            bool applied;
        };
        auto shown = [&] {
            e.synth().telemetry(*t);
            return Shown{t->bend, t->bendSet, t->seqWheel, e.synth().seqCurvesApplied()};
        };
        auto rests = [](const Shown &s) {
            return std::abs(s.bend - 0.25f) < 1e-4f && s.bendSet == 0.25f && s.wheel == 0.0f && !s.applied;
        };
        auto plays = [](const Shown &s) {
            return std::abs(s.bend - 0.75f) < 1e-4f && s.bendSet == 0.25f && std::abs(s.wheel - 0.75f) < 1e-6f && s.applied;
        };
        e.render(256); // the sequencer on, taking keys, no key down
        const Shown before = shown();
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        e.render(4800);
        const Shown playing = shown();
        e.synth().noteOff({1, 0, 0, 60});
        e.render(4800);
        const Shown letGo = shown();
        e.synth().noteOn({2, 0, 0, 60}, 1.0);
        e.render(4800);
        const Shown again = shown();
        e.p[paramIndex(SeqParam::Enabled)] = 0.0;
        e.render(256);
        const Shown off = shown();
        std::printf("      the wheels' telemetry: bend %.3f and mod wheel +%.3f playing; bend %.3f and +%.3f let go, %.3f and "
                    "+%.3f switched off\n",
                    playing.bend, playing.wheel, letGo.bend, letGo.wheel, off.bend, off.wheel);
        check(rests(before) && plays(playing) && rests(letGo) && plays(again) && rests(off),
              "the telemetry shows the wheels the sequencer's curves as they play, and nothing once they stop");
    }
    // A macro's curve takes the place of its knob as it plays: Macro 3 turning A's level down, a curve at 1 silences A
    // with the knob at 0, a curve at 0 leaves A whole with the knob at 1, a curve at 0.5 plays as a knob at 0.5 does
    // wherever the knob is, and another macro's curve does nothing to it. None stands anywhere before the sequencer has
    // played.
    {
        constexpr int kShown = 2;
        const SeqCurve curve = seqMacroCurve(kShown);
        auto start = [&](Engine &e, double knob, int curved, double v) {
            e.p[paramIndex(SeqParam::Enabled)] = 1.0;
            e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
            note(e.p, 0, 0.0, 16.0, 69);
            e.p[paramIndex(kShown, MacroParam::Value)] = knob;
            e.p[slotTargetIndex(kMaxSources + kShown, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(kMaxSources + kShown, 0)] = -1.0;
            if (curved >= 0) {
                e.p[seqCurveCountIndex(0, seqMacroCurve(curved))] = 1.0;
                e.p[seqCurveIndex(0, seqMacroCurve(curved), 0, PointField::Y)] = v;
            }
            e.synth().setTempo(120.0);
            e.render(256); // the sequencer on, taking keys
            e.synth().noteOn({1, 0, 0, 60}, 1.0);
            e.render(4800);
        };
        auto level = [&](double knob, int curved, double v) {
            Engine e(1);
            start(e, knob, curved, v);
            const std::vector<float> x = e.render(8192);
            double sum = 0.0;
            for (float s : x)
                sum += static_cast<double>(s) * s;
            return std::sqrt(sum / static_cast<double>(x.size()));
        };
        const double plain = level(0.0, -1, 0.0), half = level(0.5, -1, 0.0), silent = level(1.0, -1, 0.0);
        const double full = level(0.0, kShown, 1.0), zero = level(1.0, kShown, 0.0);
        const double fromLow = level(0.0, kShown, 0.5), fromHigh = level(1.0, kShown, 0.5);
        const double other = level(1.0, kShown + 1, 0.0);
        std::printf("      macro curve: level %.4f plain; a curve of 1 over a knob at 0 %.4f, of 0 over a knob at 1 %.4f, "
                    "of 0.5 %.4f and %.4f (a knob at 0.5 %.4f)\n",
                    plain, full, zero, fromLow, fromHigh, half);
        const Sequencer fresh;
        check(std::isnan(fresh.curve(curve)) && silent < 0.01 * plain && full < 0.01 * plain &&
                  std::abs(zero - plain) < 0.01 * plain && half < 0.9 * plain && std::abs(fromLow - half) < 0.01 * plain &&
                  std::abs(fromHigh - half) < 0.01 * plain && other < 0.01 * plain,
              "a macro's curve takes the place of its knob as it plays, moving what the macro modulates, and no other macro's");

        // The telemetry says how far the curve moves the knob from where it is set: from 0.75, a curve of 0.5 goes 0.25 down.
        auto t = std::make_unique<Telemetry>();
        Engine e(1);
        start(e, 0.75, kShown, 0.5);
        e.synth().telemetry(*t);
        const float playing = t->seqMacro[kShown], beside = t->seqMacro[kShown + 1];
        const bool applied = e.synth().seqCurvesApplied();
        e.synth().noteOff({1, 0, 0, 60});
        e.render(4800);
        e.synth().telemetry(*t);
        std::printf("      macro curve's telemetry: %+.3f playing, %+.3f let go\n", playing, t->seqMacro[kShown]);
        check(std::abs(playing + 0.25f) < 1e-6f && beside == 0.0f && applied && t->seqMacro[kShown] == 0.0f &&
                  !e.synth().seqCurvesApplied(),
              "the telemetry shows a macro's knob where the sequencer's curve holds it, and nothing once it stops");
    }
    // Muted or at no chance, a note doesn't play; at half, it plays some passes and not others, alike each time.
    {
        ParamValues p = blank();
        note(p, 0, 0.0, 0.5, 60);
        note(p, 1, 2.0, 0.5, 62);
        note(p, 2, 4.0, 0.5, 64);
        p[seqNoteIndex(0, 0, SeqNoteParam::Mute)] = 1.0;
        p[seqNoteIndex(0, 1, SeqNoteParam::Chance)] = 0.0;
        p[seqNoteIndex(0, 2, SeqNoteParam::Chance)] = 0.5;
        const std::vector<Event> e = plan(p, 64 * 16 * kStep, 4096);
        int played = 0, others = 0;
        for (const Event &ev : e)
            if (ev.on)
                (ev.key == 64 ? played : others)++;
        std::printf("      at half a chance, %d of 64 passes\n", played);
        check(others == 0 && played > 16 && played < 48 && plan(p, 64 * 16 * kStep, 4096).size() == e.size(),
              "a muted note or one at no chance doesn't play, and at half it plays about half the passes");
    }
    // Pattern B picked mid-bar takes over at the bar's end, and A's note on step 8 still plays before it.
    {
        ParamValues p = blank();
        note(p, 0, 8.0, 0.5, 60);
        note(p, 0, 0.0, 0.5, 72, 1);
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(p);
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        std::vector<Event> e;
        double done = 0.0;
        auto run = [&](double until) {
            while (done < until) {
                uint32_t covered = 0;
                const int n = seq.plan(512, 120.0, true, done / kSampleRate * 2.0, out, covered);
                for (int i = 0; i < n; ++i)
                    e.push_back({done + out[i].frame, out[i].on, out[i].key});
                done += covered;
            }
        };
        run(4 * kStep);
        p[paramIndex(SeqParam::Pattern)] = 1.0;
        seq.configure(p);
        run(17 * kStep);
        check(e.size() == 4 && e[0].key == 60 && near(e[0].frame, 8 * kStep) && e[2].key == 72 &&
                  near(e[2].frame, 16 * kStep) && seq.pattern() == 1,
              "another pattern takes over where the loop comes round");
    }
    // In Key, a key starts the pattern from its first step, transposed by the key from C4 (the scale's Root, on D here,
    // moving nothing), and letting it go stops it: through the synth, the key's end is reported, the sequencer's
    // notes' never.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        e.p[paramIndex(SeqParam::Root)] = 62.0;
        e.p[paramIndex(SeqParam::Scale)] = static_cast<double>(SeqScale::Minor);
        note(e.p, 0, 0.0, 1.0, 60);
        note(e.p, 1, 2.0, 1.0, 67);
        e.synth().setTempo(120.0);
        const std::vector<float> before = e.render(4800);
        e.synth().noteOn({5, 0, 0, 64}, 1.0);
        std::vector<float> held = e.render(static_cast<size_t>(2.5 * kStep));
        Telemetry t;
        e.synth().telemetry(t);
        // The roll lights the key the note is written on, not the key it plays: G4, not B4.
        auto lit = [&](int key) { return (t.seqKeys[static_cast<size_t>(key / 32)] >> (key % 32) & 1u) != 0; };
        const bool programmedLit = lit(67) && !lit(71);
        const std::vector<float> rest = e.render(static_cast<size_t>(1.5 * kStep));
        held.insert(held.end(), rest.begin(), rest.end());
        e.synth().telemetry(t);
        const float position = t.seqPosition;
        e.synth().noteOff({5, 0, 0, 64});
        bool keyEnded = false, seqEnded = false;
        for (const NoteRef &n : e.synth().endedNotes()) {
            keyEnded = keyEnded || (n.noteId == 5 && n.port == 0);
            seqEnded = seqEnded || n.port == kSeqPort;
        }
        e.synth().clearEndedNotes();
        e.render(48000);
        for (const NoteRef &n : e.synth().endedNotes())
            seqEnded = seqEnded || n.port == kSeqPort;
        const std::vector<float> after = e.render(4800);
        auto peak = [](const std::vector<float> &x) {
            float most = 0.0f;
            for (float s : x)
                most = std::max(most, std::abs(s));
            return most;
        };
        // The second note (G4 + 4 = B4, 493.9 Hz) sounds two steps in.
        const size_t from = static_cast<size_t>(2.2 * kStep);
        auto at = [&](double hz) { // the power at `hz` over 4096 samples from there
            std::complex<double> sum = 0.0;
            for (size_t i = 0; i < 4096; ++i)
                sum += static_cast<double>(held[from + i]) *
                       std::polar(1.0, -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate);
            return std::norm(sum);
        };
        std::printf("      Key: %.3f before, %.3f held, %.4f after; at its pattern's step %.2f\n", peak(before),
                    peak(held), peak(after), position);
        check(peak(before) == 0.0f && peak(held) > 0.1f && peak(after) == 0.0f && at(493.9) > 100.0 * at(392.0) &&
                  keyEnded && !seqEnded && std::abs(position - 4.0f) < 0.1f,
              "in Key, a key plays the pattern transposed by it from C4, whatever the scale, and stops it when let go");
        check(programmedLit, "the telemetry gives the keys of the notes playing as written, not as transposed");
    }
    // In Key, two notes on one key (their ids apart, as a host's overlapping notes or MPE have them): the first let go,
    // the pattern plays on while the other holds the key, and stops once it is let go too.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        for (int step = 0; step < 16; step += 2)
            note(e.p, step / 2, step, 1.0, 60);
        e.synth().setTempo(120.0);
        e.render(480);
        e.synth().noteOn({1, 0, 0, 60}, 1.0);
        e.render(static_cast<size_t>(kStep));
        e.synth().noteOn({2, 0, 0, 60}, 1.0);
        e.render(static_cast<size_t>(kStep));
        e.synth().noteOff({1, 0, 0, 60});
        auto peak = [](const std::vector<float> &x) {
            float most = 0.0f;
            for (float s : x)
                most = std::max(most, std::abs(s));
            return most;
        };
        const float onAfterFirst = peak(e.render(static_cast<size_t>(4 * kStep)));
        e.synth().noteOff({2, 0, 0, 60});
        e.render(48000);
        const float afterBoth = peak(e.render(static_cast<size_t>(4 * kStep)));
        std::printf("      Key, two notes on one key: %.3f once the first is let go, %.4f once both are\n", onAfterFirst,
                    afterBoth);
        check(onAfterFirst > 0.1f && afterBoth == 0.0f,
              "in Key, a pattern plays on while another note holds its key, and stops once every one is let go");
    }
    // A slide holds on into the next note, which ends it as it starts; one the next note starts within ends at its own
    // end, as written, not a step after.
    {
        ParamValues p = blank();
        note(p, 0, 0.0, 1.0, 60);
        p[seqNoteIndex(0, 0, SeqNoteParam::Slide)] = 1.0;
        note(p, 1, 1.5, 0.5, 62);
        note(p, 2, 4.0, 4.0, 64);
        p[seqNoteIndex(0, 2, SeqNoteParam::Slide)] = 1.0;
        note(p, 3, 6.0, 1.0, 65);
        const std::vector<Event> e = plan(p, 9 * kStep);
        auto offOf = [&](int key) {
            for (const Event &ev : e)
                if (!ev.on && ev.key == key)
                    return ev.frame;
            return -1.0;
        };
        std::printf("      slides: held into the next note to %.2f steps, overlapped ending at %.2f\n", offOf(60) / kStep,
                    offOf(64) / kStep);
        check(near(offOf(60), 1.5 * kStep) && near(offOf(64), 8.0 * kStep),
              "a slide holds on into the next note, and one the next note starts within ends as written");
    }
    // In Latch, a pattern a key started plays on once it is let go, and All Notes Off stops it.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Latch);
        for (int step = 0; step < 16; step += 2)
            note(e.p, step / 2, step, 1.0, 60);
        e.synth().setTempo(120.0);
        e.render(480);
        e.synth().noteOn({5, 0, 0, 60}, 1.0);
        e.render(static_cast<size_t>(kStep));
        e.synth().noteOff({5, 0, 0, 60});
        auto peak = [](const std::vector<float> &x) {
            float most = 0.0f;
            for (float s : x)
                most = std::max(most, std::abs(s));
            return most;
        };
        const float latched = peak(e.render(static_cast<size_t>(4 * kStep)));
        e.synth().allNotesOff(-1);
        e.render(48000);
        const float stopped = peak(e.render(static_cast<size_t>(4 * kStep)));
        std::printf("      Latch: %.3f let go, %.4f after All Notes Off\n", latched, stopped);
        check(latched > 0.1f && stopped == 0.0f && !e.synth().sequencerAwake(),
              "in Latch, the pattern plays on once the key is let go, and All Notes Off stops it");
    }
    // In Key, with an empty pattern, a note the editor plays to be heard (on kAuditionPort) sounds as a note rather
    // than starting the pattern (which would play nothing), and no end is reported for it.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        e.render(480);
        e.synth().noteOn({-1, kAuditionPort, 0, 69}, 0.8);
        const std::vector<float> heard = e.render(4800);
        e.synth().noteOff({-1, kAuditionPort, 0, 69});
        e.render(48000);
        bool reported = false;
        for (const NoteRef &n : e.synth().endedNotes())
            reported = reported || n.port == kAuditionPort;
        float peak = 0.0f;
        for (float s : heard)
            peak = std::max(peak, std::abs(s));
        check(peak > 0.1f && !reported, "in Key, a note the editor auditions sounds alone, its end not reported");
    }
    // The keys the telemetry shows held (the keyboard strip's) leave out the sequencer's notes: with the transport
    // playing its note, none, though a voice sounds; a key played shows.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Host);
        note(e.p, 0, 0.0, 4.0, 69);
        e.synth().setTempo(120.0);
        e.synth().setTransport(true, 0.0, 0.0);
        e.render(static_cast<size_t>(kStep));
        Telemetry t;
        e.synth().telemetry(t);
        const bool seqHidden = t.voices > 0 && !t.keyDown(69);
        e.noteOn(50);
        e.render(480);
        e.synth().telemetry(t);
        check(seqHidden && t.keyDown(50) && !t.keyDown(69),
              "the keys shown held leave the sequencer's notes out and show a key played");
    }
    // With the host's transport, through the synth: silent until the note's step, then sounding at its sample.
    {
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Host);
        note(e.p, 0, 1.0, 1.0, 69);
        e.synth().setTempo(120.0);
        e.synth().setTransport(true, 0.0, 0.0);
        const std::vector<float> out = e.render(static_cast<size_t>(2 * kStep), 100);
        size_t first = out.size();
        for (size_t i = 0; i < out.size() && first == out.size(); ++i)
            if (out[i] != 0.0f)
                first = i;
        std::printf("      first sound at sample %zu (its step at %.0f)\n", first, kStep);
        check(first >= static_cast<size_t>(kStep) && first < static_cast<size_t>(kStep) + 64,
              "the sequencer's note sounds from its sample, however the blocks are split");
    }
    // Plans `steps` more from `seq`, the transport playing from `fromStep` (Host's clock; the others ignore it), in
    // stretches of `piece`; `each` called before each stretch with the step it starts at, `positions` taking where it
    // plays after each.
    auto planOn = [&](Sequencer &seq, double fromStep, double steps, uint32_t piece = 512,
                      const std::function<void(double)> &each = {}, std::vector<float> *positions = nullptr) {
        std::vector<Event> events;
        std::array<SeqEvent, Sequencer::kMaxEvents> out;
        for (double done = 0.0; done < steps * kStep;) {
            if (each)
                each(fromStep + done / kStep);
            uint32_t covered = 0;
            const int n = seq.plan(piece, 120.0, true, (fromStep + done / kStep) / 4.0, out, covered);
            for (int i = 0; i < n; ++i)
                events.push_back({done + out[i].frame, out[i].on, out[i].key});
            if (positions)
                positions->push_back(seq.position());
            done += covered;
        }
        return events;
    };
    // The Play button (Sequencer::preview()): whatever Play says, the pattern from its loop's start on a clock of its
    // own, as written but for Octave and Transpose, the keys left to play notes; off, back to Play's way. In Key with no
    // key held and the loop from step 4: the note there at once, with Transpose's 2 semitones and no key's, the next two
    // steps on; turned off in that one, it ends, and the keys go to the sequencer again. Nothing while the sequencer is
    // off, and switched off, it stops. In Host, from the loop's start though the transport stands at step 37.5, and once
    // off, with the transport again, its next note where the bar comes round.
    {
        ParamValues p = blank();
        p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        p[paramIndex(SeqParam::Transpose)] = 2.0;
        p[paramIndex(0, SeqPatternParam::LoopStart)] = 4.0;
        note(p, 0, 4.0, 1.0, 60);
        note(p, 1, 6.0, 1.0, 64);
        Sequencer seq;
        seq.prepare(kSampleRate);
        seq.configure(p);
        const bool keysBefore = seq.takesKeys(), awakeBefore = seq.wantsProcessing();
        seq.preview(true);
        const bool keysDuring = seq.takesKeys(), awake = seq.wantsProcessing();
        const std::vector<Event> e = planOn(seq, 0.0, 2.5);
        seq.preview(false);
        const std::vector<Event> after = planOn(seq, 0.0, 2.0);
        std::printf("      played by the button in Key: %zu events, the first on %d at %.0f, the second on %d at %.0f; "
                    "stopped, %zu\n",
                    e.size(), e.empty() ? -1 : e[0].key, e.empty() ? -1.0 : e[0].frame, e.size() > 2 ? e[2].key : -1,
                    e.size() > 2 ? e[2].frame : -1.0, after.size());
        check(keysBefore && !awakeBefore && !keysDuring && awake && e.size() == 3 && e[0].on && e[0].key == 62 &&
                  near(e[0].frame, 0.0) && !e[1].on && near(e[1].frame, kStep) && e[2].on && e[2].key == 66 &&
                  near(e[2].frame, 2.0 * kStep) && after.size() == 1 && !after[0].on && after[0].key == 66 &&
                  near(after[0].frame, 0.0) && seq.takesKeys() && !seq.previewing(),
              "the Play button plays the pattern from its loop's start, as written but for Transpose, the keys left be");
        ParamValues off = p;
        off[paramIndex(SeqParam::Enabled)] = 0.0;
        Sequencer idle;
        idle.prepare(kSampleRate);
        idle.configure(off);
        idle.preview(true);
        const bool offIgnored = !idle.previewing();
        seq.preview(true);
        planOn(seq, 0.0, 0.5);
        seq.configure(off);
        check(offIgnored && !seq.previewing(), "the Play button plays nothing while the sequencer is off, and stops with it");

        // From a step (the first note picked): the note at 6 at once, the one at 4 only once the loop comes round; with
        // Reverse, the note at 6 and two steps later the one at 4; with Swing at 75 %, a note on the odd step 5 at once
        // too; from step 2, outside the loop, from its start.
        auto from = [&](const ParamValues &q, double step, double steps) {
            Sequencer f;
            f.prepare(kSampleRate);
            f.configure(q);
            f.preview(true, step);
            return planOn(f, 0.0, steps);
        };
        const std::vector<Event> at6 = from(p, 6.0, 3.0);
        ParamValues reverse = p;
        reverse[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Reverse);
        const std::vector<Event> back6 = from(reverse, 6.0, 2.5);
        ParamValues swung = p;
        swung[paramIndex(SeqParam::Swing)] = 0.75;
        note(swung, 2, 5.0, 1.0, 62);
        const std::vector<Event> at5 = from(swung, 5.0, 0.5);
        const std::vector<Event> outside = from(p, 2.0, 0.5);
        std::printf("      played from a step: from 6, on %d at %.0f (%zu events); reversed, then on %d at %.0f; swung from "
                    "5, on %d at %.0f; from 2, on %d\n",
                    at6.empty() ? -1 : at6[0].key, at6.empty() ? -1.0 : at6[0].frame, at6.size(),
                    back6.size() > 2 ? back6[2].key : -1, back6.size() > 2 ? back6[2].frame : -1.0,
                    at5.empty() ? -1 : at5[0].key, at5.empty() ? -1.0 : at5[0].frame,
                    outside.empty() ? -1 : outside[0].key);
        check(at6.size() == 2 && at6[0].on && at6[0].key == 66 && near(at6[0].frame, 0.0) && back6.size() == 3 &&
                  back6[0].key == 66 && near(back6[0].frame, 0.0) && back6[2].on && back6[2].key == 62 &&
                  near(back6[2].frame, 2.0 * kStep) && !at5.empty() && at5[0].on && at5[0].key == 64 &&
                  near(at5[0].frame, 0.0) && !outside.empty() && outside[0].key == 62 && near(outside[0].frame, 0.0),
              "the Play button starts at a step of the loop, the way the Order goes, and else at the loop's start");

        ParamValues host = blank();
        note(host, 0, 0.0, 1.0, 60);
        note(host, 1, 5.0, 1.0, 64);
        Sequencer h;
        h.prepare(kSampleRate);
        h.configure(host);
        h.preview(true);
        const std::vector<Event> fromStart = planOn(h, 37.5, 1.5);
        h.preview(false);
        const std::vector<Event> back = planOn(h, 39.0, 10.0);
        std::printf("      played by the button in Host at step 37.5: first on %d at %.0f; off again, the next on %d at step "
                    "%.2f\n",
                    fromStart.empty() ? -1 : fromStart[0].key, fromStart.empty() ? -1.0 : fromStart[0].frame,
                    back.empty() ? -1 : back[0].key, back.empty() ? -1.0 : 39.0 + back[0].frame / kStep);
        check(fromStart.size() == 2 && fromStart[0].on && fromStart[0].key == 60 && near(fromStart[0].frame, 0.0) &&
                  !back.empty() && back[0].on && back[0].key == 60 && near(back[0].frame, 9.0 * kStep),
              "in Host, the Play button plays from the loop's start, then the transport takes it back");
    }
    // Through the synth, in Key: the Play button plays the pattern, a key pressed meanwhile plays a note of its own beside
    // it, and the telemetry says it plays until it is turned off.
    {
        auto t = std::make_unique<Telemetry>();
        Engine e(1);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        note(e.p, 0, 0.0, 16.0, 69); // the whole bar long
        e.synth().setTempo(120.0);
        e.render(256);
        e.synth().setSeqPreview(true);
        e.render(512);
        const int patternOnly = e.synth().activeVoiceCount();
        e.noteOn(50);
        e.render(512);
        e.synth().telemetry(*t);
        const int withKey = e.synth().activeVoiceCount();
        const bool shown = t->seqPreview;
        e.synth().setSeqPreview(false);
        e.render(512);
        e.synth().telemetry(*t);
        std::printf("      the Play button through the synth in Key: %d voice, %d with a key pressed\n", patternOnly, withKey);
        check(patternOnly == 1 && withKey == 2 && shown && !t->seqPreview && e.synth().isActive({-1, 0, 0, 50}),
              "with the Play button on, keys play notes beside the pattern, and the telemetry says it plays");
    }
    // Random picks among the steps notes start in, each played on to the next note's: four quarter notes play one on
    // every beat and none between (several of them, the same picks each time it plays), the playhead moving on
    // steadily within a beat; and with notes at steps 0, 3 and 8, each time round the loop starts on a note, the picks
    // filling its 16 steps.
    {
        ParamValues p = blank();
        p[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Random);
        for (int i = 0; i < 4; ++i)
            note(p, i, 4.0 * i, 4.0, 60 + 2 * i);
        auto run = [&](const ParamValues &q, std::vector<float> *positions = nullptr) {
            Sequencer seq;
            seq.prepare(kSampleRate);
            seq.configure(q);
            return planOn(seq, 0.0, 64.0, 250, {}, positions);
        };
        std::vector<float> positions;
        const std::vector<Event> e = run(p, &positions);
        int ons = 0, offBeat = 0;
        std::vector<bool> heard(128, false);
        for (const Event &ev : e) {
            if (!ev.on)
                continue;
            ++ons;
            heard[static_cast<size_t>(ev.key)] = true;
            const double beat = ev.frame / kStep / 4.0;
            if (std::abs(beat - std::round(beat)) > 1e-3)
                ++offBeat;
        }
        const int kinds = static_cast<int>(std::count(heard.begin(), heard.end(), true));
        const std::vector<Event> again = run(p);
        bool same = again.size() == e.size();
        for (size_t i = 0; same && i < e.size(); ++i)
            same = again[i].on == e[i].on && again[i].key == e[i].key && near(again[i].frame, e[i].frame);
        const double share = 250.0 / kStep;
        int strayJumps = 0;
        // (A stretch ending on a beat ends a hair before it, as its frames add up: the jump shows a stretch later.)
        auto byBeat = [&](double t) { return std::abs(t - 4.0 * std::round(t / 4.0)) <= share * 1.01; };
        for (size_t i = 1; i < positions.size(); ++i) {
            const double moved = positions[i] - positions[i - 1];
            const double t0 = i * share, t1 = (i + 1) * share; // where the two stretches ended, in steps
            if ((moved < -1e-4 || moved > share + 1e-3) && !byBeat(t0) && !byBeat(t1))
                ++strayJumps;
        }
        ParamValues uneven = blank();
        uneven[paramIndex(SeqParam::Order)] = static_cast<double>(SeqOrder::Random);
        note(uneven, 0, 0.0, 1.0, 60);
        note(uneven, 1, 3.0, 1.0, 64);
        note(uneven, 2, 8.0, 1.0, 67);
        const std::vector<Event> u = run(uneven);
        int roundsOnNote = 0, unevenOns = 0;
        for (int round = 0; round < 4; ++round)
            roundsOnNote += std::any_of(u.begin(), u.end(), [&](const Event &ev) {
                return ev.on && near(ev.frame, 16.0 * round * kStep);
            });
        for (const Event &ev : u)
            unevenOns += ev.on;
        std::printf("      Random over quarter notes: %d notes on in 16 beats, %d off the beat, %d pitches, %s; the playhead "
                    "jumping %d times within a beat; notes at 0, 3 and 8: %d of 4 rounds starting on a note, %d notes on\n",
                    ons, offBeat, kinds, same ? "alike again" : "otherwise again", strayJumps, roundsOnNote, unevenOns);
        check(ons == 16 && offBeat == 0 && kinds >= 3 && same && strayJumps == 0 && roundsOnNote == 4 && unevenOns >= 8,
              "Random picks among the steps notes start in, playing on to the next note's, the loop keeping its length");
    }
    // The loop shortened step by step under the playhead in its second time round (Key, from a key; Host, with the
    // transport; and the Play button), a note on every step: it plays on from where it is, one note a step, up to the new
    // loop's end and round from its start, rather than jumping about and starting the notes it lands in the middle of;
    // and in Host, the transport jumping locks it to the bars again.
    {
        ParamValues p = blank();
        for (int s = 0; s < 16; ++s)
            note(p, s, s, 1.0, 60 + s);
        for (int mode = 0; mode < 3; ++mode) {
            ParamValues q = p;
            if (mode != 1)
                q[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
            Sequencer seq;
            seq.prepare(kSampleRate);
            seq.configure(q);
            if (mode == 0)
                seq.keyDown(60);
            else if (mode == 2)
                seq.preview(true);
            // In stretches of 250 samples, which a step's 6000 hold whole: the transport goes on from where it was.
            planOn(seq, 0.0, 26.25, 250);
            int end = 16;
            std::vector<float> positions;
            const std::vector<Event> e = planOn(
                seq, 26.25, 2.5, 250,
                [&](double) {
                    if (end > 12) { // the loop's end dragged a step a stretch
                        q[paramIndex(0, SeqPatternParam::LoopEnd)] = --end;
                        seq.configure(q);
                    }
                },
                &positions);
            std::vector<int> ons;
            for (const Event &ev : e)
                if (ev.on)
                    ons.push_back(ev.key);
            bool steady = true;
            for (size_t i = 1; i < positions.size(); ++i)
                if (positions[i] < positions[i - 1] && !(positions[i - 1] > 11.0f && positions[i] < 1.0f))
                    steady = false;
            const char *names[] = {"Key", "Host", "the Play button"};
            std::printf("      the loop shortened from 16 to 12 at step 10.25 (%s): %zu notes on (%d, %d), the playhead "
                        "%.2f to %.2f%s\n",
                        names[mode], ons.size(), ons.empty() ? -1 : ons[0], ons.size() > 1 ? ons[1] : -1,
                        positions.empty() ? -1.0f : positions.front(), positions.empty() ? -1.0f : positions.back(),
                        steady ? "" : ", jumping back");
            check(steady && ons.size() == 2 && ons[0] == 71 && ons[1] == 60,
                  "the loop shortened under the playhead: it plays on from where it is, each note once");
            if (mode == 1) {
                // (After the note it held ends.)
                const std::vector<Event> jumped = planOn(seq, 48.0, 0.5);
                const auto on = std::find_if(jumped.begin(), jumped.end(), [](const Event &ev) { return ev.on; });
                check(on != jumped.end() && on->key == 60 && near(on->frame, 0.0),
                      "the transport jumping locks the pattern to the bars again");
            }
        }
    }
}

// The layout chunk's floats after the preset's info: the order of the places past the first 16 and their folds,
// whether the SEQ view's roll bends notes, the names of the tuning's files (two empty texts, a float each) and the
// nodes hidden from the routing graph.
constexpr uint32_t kAfterInfo = kMaxSources - kFirstSources + 1 + 1 + 2 + 1;

// The layout chunk, the patch's last, cut short by its last `floats` floats: as saved before they came.
void cutLayout(std::vector<unsigned char> &bytes, uint32_t floats) {
    for (size_t at = bytes.size() - 4; at >= 4; at -= 4) {
        uint32_t count = 0;
        std::memcpy(&count, bytes.data() + at - 4, 4);
        if ((at + count * sizeof(float)) == bytes.size()) {
            count -= floats;
            std::memcpy(bytes.data() + at - 4, &count, 4);
            bytes.resize(bytes.size() - floats * sizeof(float));
            return;
        }
    }
}

// What a preset says about itself, which the layout keeps after the tempo: read back as written (in UTF-8, from a
// sparse patch and a full one, by readLayout() and by readPresetInfo(), which the library's scan uses), none from a
// layout from before it; and every factory preset's: FMVibe's, in a built-in category, with a line of at most 80
// characters. And a factory preset's name from up to 0.11, its category's two letters before it, naming the preset
// as it is now.
void presetInfoChecks() {
    std::printf("preset info:\n");
    const gui::PresetInfo written{"J\xC3\xBCrgen", "Texture", "A line about it, \xE2\x80\x9Cquoted\xE2\x80\x9D."};
    auto bytesOf = [&](bool sparse, const gui::PresetInfo &info) {
        gui::UiLayout ui;
        ui.presetInfo = info;
        ui.savedTempo = 120.0f;
        const auto values = std::make_unique<ParamValues>();
        for (int i = 0; i < kParamCount; ++i)
            (*values)[i] = paramTable()[i].def;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, *values, ui, sparse);
        return bytes;
    };
    bool ok = true;
    for (bool sparse : {true, false}) {
        std::vector<unsigned char> bytes = bytesOf(sparse, written);
        MemoryReader reader{bytes.data(), bytes.size(), 0};
        const clap_istream_t in = memoryIn(reader);
        const auto loaded = std::make_unique<ParamValues>();
        readParams(&in, *loaded);
        const gui::UiLayout layout = readLayout(&in);
        MemoryReader again{bytes.data(), bytes.size(), 0};
        const clap_istream_t scan = memoryIn(again);
        gui::PresetInfo scanned;
        ok = ok && layout.presetInfo == written && layout.savedTempo == 120.0f && readPresetInfo(&scan, scanned) &&
             scanned == written;
    }
    // From a file, as the library's scan reads the preset folder, seeking past the params: a sparse patch's info and
    // a full one's, and none from a file that ends among its params.
    auto fromFile = [](const std::vector<unsigned char> &bytes, gui::PresetInfo &info) {
#ifdef _MSC_VER // which deprecates tmpfile()
        FILE *file = nullptr;
        if (tmpfile_s(&file) != 0)
            file = nullptr;
#else
        FILE *file = std::tmpfile();
#endif
        if (!file)
            return false;
        const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        std::rewind(file);
        const bool read = written && readPresetInfo(file, info);
        std::fclose(file);
        return read;
    };
    bool files = true;
    for (bool sparse : {true, false}) {
        gui::PresetInfo scanned;
        files = files && fromFile(bytesOf(sparse, written), scanned) && scanned == written;
    }
    std::vector<unsigned char> cut = bytesOf(false, written);
    cut.resize(cut.size() / 2);
    gui::PresetInfo cutInfo;
    files = files && !fromFile(cut, cutInfo) && cutInfo.empty();
    check(files, "a preset file's info read past its params by seeking, as written, and none from one cut short");
    std::vector<unsigned char> old = bytesOf(true, {});
    cutLayout(old, 3 + kAfterInfo);
    MemoryReader reader{old.data(), old.size(), 0};
    const clap_istream_t in = memoryIn(reader);
    gui::PresetInfo none{"x", "y", "z"};
    const bool before = readPresetInfo(&in, none) && none.empty();
    std::vector<unsigned char> notState{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    MemoryReader junk{notState.data(), notState.size(), 0};
    const clap_istream_t junkIn = memoryIn(junk);
    gui::PresetInfo refused;
    const bool notRead = !readPresetInfo(&junkIn, refused);
    check(ok && before && notRead,
          "a preset's author, category and line read back as written, as none from before them, and nothing from a "
          "stream that isn't a state");
    bool factory = true;
    for (const FactoryPreset &preset : factoryPresets()) {
        MemoryReader r{preset.data.data(), preset.data.size(), 0};
        const clap_istream_t s = memoryIn(r);
        gui::PresetInfo info;
        const bool good = readPresetInfo(&s, info) && info.author == "FMVibe" &&
                          gui::builtInCategory(info.category) >= 0 && !info.description.empty() &&
                          info.description.size() <= gui::kMaxPresetDescription;
        if (!good)
            std::printf("      %-20s says \"%s\", \"%s\", \"%s\"\n", preset.name.c_str(), info.author.c_str(),
                        info.category.c_str(), info.description.c_str());
        factory = factory && good;
    }
    check(factory, "every factory preset says it is FMVibe's, in a built-in category, with a line of 80 characters at "
                   "most");
    const FactoryPreset *screamer = factoryPresetNamed("Screamer");
    check(screamer && factoryPresetNamed("LD Screamer") == screamer && factoryPresetNamed("ld screamer") == screamer &&
              !factoryPresetNamed("XX Screamer") && !factoryPresetNamed("BA Screamer Two"),
          "a factory preset's name from up to 0.11 (\"LD Screamer\") names the preset as it is now");
}

// The tempo a preset was saved at, which the layout keeps for the hosts that let the editor set the tempo: read back
// as written, a tempo the editor doesn't offer as none, and a layout from before it (cut short of it) as none too.
void savedTempoChecks() {
    std::printf("saved tempo:\n");
    auto roundTrip = [](float bpm, bool cut) {
        gui::UiLayout ui;
        ui.savedTempo = bpm;
        const auto values = std::make_unique<ParamValues>();
        for (int i = 0; i < kParamCount; ++i)
            (*values)[i] = paramTable()[i].def;
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, *values, ui, true);
        if (cut) // the layout, the patch's last chunk, as saved before the field came: without it, the preset's info
                 // after it (three empty texts, a float each) and what came after that
            cutLayout(bytes, 4 + kAfterInfo);
        MemoryReader reader{bytes.data(), bytes.size(), 0};
        const clap_istream_t in = memoryIn(reader);
        const auto loaded = std::make_unique<ParamValues>();
        readParams(&in, *loaded);
        return readLayout(&in).savedTempo;
    };
    const float kept = roundTrip(128.5f, false), none = roundTrip(0.0f, false), slow = roundTrip(5.0f, false);
    const float before = roundTrip(128.5f, true);
    std::printf("      128.5 -> %.2f, 0 -> %.2f, 5 -> %.2f, from before it -> %.2f\n", kept, none, slow, before);
    check(kept == 128.5f && none == 0.0f && slow == 0.0f && before == 0.0f,
          "a preset's saved tempo reads back as written, and as none below 20 BPM or from before it was kept");
}

// --- A Global LFO (see lfoGlobal()): one clock for every voice, which every note starts again. A square at 2 Hz,
// unipolar, takes A's level down to nothing for the first half of each cycle: an A4 held, sounding 0.3 s in, goes
// silent with an E5 struck then, both starting the LFO's cycle again, where a free LFO leaves the A4 sounding.
void globalLfoChecks() {
    std::printf("global LFO:\n");
    auto struck = [](bool global) {
        Engine e(2);
        lfoOn(e.p, paramIndex(0, OpParam::Out), -1.0, 2.0);
        e.p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Square);
        e.p[paramIndex(0, SourceParam::Unipolar)] = 1.0;
        e.p[paramIndex(0, global ? SourceParam::Global : SourceParam::Free)] = 1.0;
        e.noteOn(69);
        e.render(static_cast<size_t>(0.3 * kSampleRate));
        e.noteOn(76);
        e.render(static_cast<size_t>(0.02 * kSampleRate)); // the Out ramping down
        const std::vector<float> after = e.render(static_cast<size_t>(0.15 * kSampleRate));
        double sum = 0.0;
        for (float x : after)
            sum += static_cast<double>(x) * x;
        return 10.0 * std::log10(sum / after.size() + 1e-30);
    };
    const double global = struck(true), free = struck(false);
    std::printf("      after a second note: %.1f dB global, %.1f dB free\n", global, free);
    check(global < -40.0 && free > -30.0,
          "a Global LFO starts again for every voice as each note starts, where a free one runs on");
}

// --- Keyboard level scaling: an operator's level across the keys by its breakpoint, depths and curves (the Exp
// curve's as the DX7's), a modulator's index with it, and nothing changed while it is off.
void keyScaleChecks() {
    std::printf("keyboard scaling:\n");
    // The output's RMS level (dB) of a held note, once settled.
    auto levelDb = [](int key, const auto &setup) {
        Engine e(2);
        setup(e.p);
        e.noteOn(key);
        e.render(4800);
        const std::vector<float> out = e.render(kFftSize);
        double sum = 0.0;
        for (float x : out)
            sum += static_cast<double>(x) * x;
        return 10.0 * std::log10(sum / out.size() + 1e-30);
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 0.1; };
    {
        // Breakpoint at C4; the right side down 6 dB an octave, the left up 3: each note against itself unscaled.
        // Linear, three octaves up is 18 dB down; exponential, 6 dB the first octave, then doubling: 12 at two, 24 at
        // three, and 12 up three octaves down.
        auto scaled = [&](int key, double curve) {
            return levelDb(key, [&](ParamValues &p) {
                       p[paramIndex(0, OpParam::KeyBreak)] = 60.0;
                       p[paramIndex(0, OpParam::KeyRightDepth)] = -6.0;
                       p[paramIndex(0, OpParam::KeyLeftDepth)] = 3.0;
                       p[paramIndex(0, OpParam::KeyRightCurve)] = curve;
                       p[paramIndex(0, OpParam::KeyLeftCurve)] = curve;
                   }) -
                   levelDb(key, [](ParamValues &) {});
        };
        const double at60 = scaled(60, 0.0), up1 = scaled(72, 0.0), up3 = scaled(96, 0.0), down1 = scaled(48, 0.0),
                     down2 = scaled(36, 0.0), half = scaled(66, 0.0);
        const double expHalf = scaled(66, 1.0), expUp1 = scaled(72, 1.0), expUp2 = scaled(84, 1.0),
                     expUp3 = scaled(96, 1.0), expDown3 = scaled(24, 1.0);
        std::printf("      linear: C4 %+.2f, F#4 %+.2f, C5 %+.2f, C7 %+.2f, C3 %+.2f, C2 %+.2f dB; "
                    "exponential: F#4 %+.2f, C5 %+.2f, C6 %+.2f, C7 %+.2f, C1 %+.2f dB\n",
                    at60, half, up1, up3, down1, down2, expHalf, expUp1, expUp2, expUp3, expDown3);
        check(near(at60, 0.0) && near(up1, -6.0) && near(up3, -18.0) && near(half, -3.0) && near(down1, 3.0) &&
                  near(down2, 6.0),
              "level scaling moves an operator's level by its depth an octave from the breakpoint, either side");
        check(near(expHalf, -3.0) && near(expUp1, -6.0) && near(expUp2, -12.0) && near(expUp3, -24.0) &&
                  near(expDown3, 12.0),
              "an exponential side moves evenly to its depth an octave out, then doubles all it moved each octave");
        const double deep = levelDb(108, [](ParamValues &p) {
                                p[paramIndex(0, OpParam::KeyRightDepth)] = -kMaxKeyDepthDb;
                                p[paramIndex(0, OpParam::KeyRightCurve)] = 1.0;
                            }) -
                            levelDb(108, [](ParamValues &) {});
        std::printf("      C8 at -24 dB an octave exponential: %+.1f dB\n", deep);
        check(std::abs(deep - kKeyScaleFloorDb) < 1.0, "level scaling stops at its floor");
    }
    {
        // On a modulator, it takes the index down with the level: B modulating A at the same ratio, its right side at
        // -24 dB an octave from A2, leaves an A4 (two octaves up, 48 dB down) all but a sine, where unscaled it has a
        // strong 2nd harmonic.
        auto second = [&](double depth) {
            const std::vector<float> tone = steadyTone(2, 69, [&](ParamValues &p) {
                p[paramIndex(1, OpParam::Enabled)] = 1.0;
                p[matrixIndex(1, 0)] = 0.5;
                p[paramIndex(1, OpParam::KeyBreak)] = 45.0;
                p[paramIndex(1, OpParam::KeyRightDepth)] = depth;
            });
            const std::vector<double> power = powerSpectrum(tone);
            auto at = [&](double hz) {
                const long center = std::lround(hz / (kSampleRate / kFftSize));
                double most = 1e-30;
                for (long b = center - 3; b <= center + 3; ++b)
                    most = std::max(most, power[static_cast<size_t>(b)]);
                return 10.0 * std::log10(most);
            };
            return at(880.0) - at(440.0);
        };
        const double unscaled = second(0.0), scaled = second(-24.0);
        std::printf("      a modulator's 2nd harmonic under the fundamental: %.1f dB unscaled, %.1f dB scaled\n",
                    unscaled, scaled);
        check(unscaled > -20.0 && scaled < unscaled - 40.0, "level scaling on a modulator takes its index down");
    }
    {
        // Off, it changes nothing: a breakpoint moved with no depth plays the default patch's notes sample for sample,
        // a chord in single frames among them.
        auto play = [](double breakpoint, uint32_t piece) {
            Engine e(2);
            e.p[paramIndex(0, OpParam::KeyBreak)] = breakpoint;
            e.p[paramIndex(0, OpParam::Decay)] = normFromTime(0.3);
            e.p[paramIndex(0, OpParam::Sustain)] = 0.4;
            for (int key : {36, 60, 67, 96})
                e.noteOn(key);
            std::vector<float> out = e.render(9600, piece);
            for (int key : {36, 60, 67, 96})
                e.noteOff(key);
            const std::vector<float> tail = e.render(4800, piece);
            out.insert(out.end(), tail.begin(), tail.end());
            return out;
        };
        check(play(60.0, kBlock) == play(100.0, kBlock) && play(60.0, 1) == play(20.0, 1),
              "keyboard scaling off leaves the sound exactly as it was, wherever its breakpoint is");
    }
    {
        // Its params' text reads back as written, at their ends and between.
        bool ok = true;
        for (OpParam f : {OpParam::KeyBreak, OpParam::KeyLeftDepth, OpParam::KeyRightDepth, OpParam::KeyLeftCurve,
                          OpParam::KeyRightCurve}) {
            const int index = paramIndex(2, f);
            const ParamDef &def = paramTable()[index];
            for (double v : {def.min, def.def, 0.5 * (def.min + def.max), def.max - (def.max - def.min) * 1e-4, def.max}) {
                char text[64], again[64];
                double parsed = 0.0;
                ok = ok && formatParam(index, v, text, sizeof(text)) && parseParam(index, text, &parsed) &&
                     formatParam(index, parsed, again, sizeof(again)) && std::strcmp(text, again) == 0;
            }
        }
        char text[64];
        formatParam(paramIndex(0, OpParam::KeyRightDepth), -6.0, text, sizeof(text));
        const std::string depth = text;
        formatParam(paramIndex(0, OpParam::KeyRightCurve), 1.0, text, sizeof(text));
        std::printf("      a depth reads \"%s\", a curve \"%s\"\n", depth.c_str(), text);
        check(ok && depth == "-6.0 dB" && std::strcmp(text, tr(Str::KeyCurveExp)) == 0,
              "keyboard scaling's params read back as written");
    }
}

// --- DX7 voices (see gui/dx7.h): sysex read back as written, a bank and a single voice alike, and a voice played as
// the DX7 plays it: its pitch, transposed; a modulator's index from its output level, against the Bessel functions'
// sidebands; feedback's harmonics; a decay's time from its rate; velocity; the pitch envelope; and the LFO's rate.
void dx7Checks() {
    namespace dx7 = gui::dx7;
    std::printf("DX7 import:\n");
    // A voice with a little of everything, to read back as written.
    dx7::Voice voice = dx7::initVoice();
    for (int i = 0; i < dx7::kVoiceFields; ++i)
        voice.data[i] = static_cast<uint8_t>((i * 37 + 11) % 100);
    const char *name = "E.PIANO 1 ";
    for (int i = 0; i < dx7::kNameBytes; ++i)
        voice.data[dx7::kVoiceFields + dx7::Name + i] = static_cast<uint8_t>(name[i]);
    voice.data[dx7::kVoiceFields + dx7::AlgorithmNumber] = 4;
    voice.data[dx7::kVoiceFields + dx7::LfoWave] = 5;
    voice = dx7::clamped(voice);
    const std::vector<uint8_t> bank = dx7::bankSysex({voice});
    const std::vector<uint8_t> single = dx7::voiceSysex(voice);
    std::vector<uint8_t> both = bank;
    both.insert(both.end(), single.begin(), single.end());
    const std::vector<dx7::Voice> fromBank = dx7::parseSysex(bank.data(), bank.size());
    const std::vector<dx7::Voice> fromBoth = dx7::parseSysex(both.data(), both.size());
    const std::vector<dx7::Voice> fromRaw = dx7::parseSysex(bank.data() + 6, 32 * dx7::kPackedBytes);
    const std::vector<uint8_t> junk(500, 0x41);
    const auto init = dx7::packVoice(dx7::initVoice());
    std::printf("      bank %zu voices, bank and voice %zu, headerless %zu, junk %zu; first \"%s\"\n", fromBank.size(),
                fromBoth.size(), fromRaw.size(), dx7::parseSysex(junk.data(), junk.size()).size(),
                fromBank.empty() ? "" : fromBank[0].name().c_str());
    check(bank.size() == 4104 && single.size() == 163 && fromBank.size() == 32 && fromBoth.size() == 33 &&
              fromRaw.size() == 32 && dx7::parseSysex(junk.data(), junk.size()).empty() &&
              fromBank[0].data == voice.data && fromBank[1].data == dx7::initVoice().data &&
              fromBoth[32].data == voice.data && fromRaw[0].data == voice.data && fromBank[0].name() == "E.PIANO 1" &&
              init[15] == 2 && init[12] == 7 << 3 && init[116] == (1 | 3 << 4) && init[117] == 24,
          "a DX7 bank and a single voice read back as written, packed as the DX7 packs them");

    // A voice of a few operators on algorithm 32 (all of them out) or 1 (B into A), played on an A4 once settled.
    auto patch = [](int algorithm, auto setup) {
        dx7::Voice v = dx7::initVoice();
        v.data[dx7::kVoiceFields + dx7::AlgorithmNumber] = static_cast<uint8_t>(algorithm - 1);
        setup(v);
        return v;
    };
    auto set = [](dx7::Voice &v, int op, int field, int value) {
        v.data[(6 - op) * dx7::kOpBytes + field] = static_cast<uint8_t>(value);
    };
    auto play = [](const dx7::Voice &v, double velocity = 1.0, size_t settle = 9600) {
        Engine e(2);
        gui::UiLayout layout;
        dx7::toPatch(v, e.p, layout);
        e.synth().noteOn({-1, 0, 0, 69}, velocity);
        e.render(settle);
        return e.render(kFftSize);
    };
    auto at = [](const std::vector<float> &signal, double hz) { // the loudest bin near it, in dB
        const std::vector<double> power = powerSpectrum(signal);
        const long center = std::lround(hz / (kSampleRate / kFftSize));
        double most = 1e-30;
        for (long b = center - 3; b <= center + 3; ++b)
            most = std::max(most, power[static_cast<size_t>(b)]);
        return 10.0 * std::log10(most);
    };

    // INIT VOICE: a sine at the note; transposed an octave up, at the octave.
    const std::vector<float> sine = play(dx7::initVoice());
    const double sineOff = unexpectedDb(sine, {440.0});
    const std::vector<float> octave = play(patch(1, [](dx7::Voice &v) { v.data[dx7::kVoiceFields + dx7::Transpose] = 36; }));
    std::printf("      INIT VOICE: %.0f dB off 440 Hz; transposed +12: 880 Hz %.1f dB over 440\n", sineOff,
                at(octave, 880.0) - at(octave, 440.0));
    check(sineOff < -60.0 && at(octave, 880.0) - at(octave, 440.0) > 60.0,
          "a DX7 voice plays its operators at their ratios, transposed");

    // INIT VOICE's attack at rate 99, a step of the DX7's envelope (1.45 ms) long: ramping in through it, as the DX7's
    // gain ramps across each step, no sample stepping much further than the sine's own do (2 pi 440 / 48000 of its
    // peak) rather than jumping in at once.
    {
        Engine e(2);
        gui::UiLayout layout;
        dx7::toPatch(dx7::initVoice(), e.p, layout);
        e.noteOn(69);
        const std::vector<float> start = e.render(960);
        float peak = 0.0f, step = 0.0f;
        for (size_t i = 0; i < start.size(); ++i) {
            peak = std::max(peak, std::abs(start[i]));
            if (i > 0)
                step = std::max(step, std::abs(start[i] - start[i - 1]));
        }
        const float sineStep = peak * 2.0f * std::numbers::pi_v<float> * 440.0f / 48000.0f;
        std::printf("      INIT VOICE's attack: largest step %.4f, the sine's own %.4f\n", step, sineStep);
        check(peak > 0.01f && step < 1.5f * sineStep, "a DX7 voice's fast attack ramps in rather than clicking");
    }

    // A modulator at 3 times the carrier, at output levels 60 and 75: its first sidebands (4 and 2 times the note)
    // against the carrier as J1 against J0 at the index the DX7 gives it, 4 pi times 2^(level's units / 256 - 15).
    for (int level : {60, 75}) {
        const std::vector<float> fm = play(patch(1, [&](dx7::Voice &v) {
            set(v, 2, dx7::OutputLevel, level);
            set(v, 2, dx7::Coarse, 3);
        }));
        const double index = 4.0 * std::numbers::pi * std::exp2(((28 + level) * 32.0 - 4064.0) / 256.0);
        const double want = 20.0 * std::log10(std::abs(std::cyl_bessel_j(1.0, index) / std::cyl_bessel_j(0.0, index)));
        const double got = at(fm, 1760.0) - at(fm, 440.0), lower = at(fm, 880.0) - at(fm, 440.0);
        std::printf("      modulator at level %d (index %.2f): sidebands %.1f and %.1f dB, J1/J0 %.1f dB\n", level,
                    index, got, lower, want);
        check(std::abs(got - want) < 1.0 && std::abs(lower - want) < 1.0,
              "a DX7 modulator's output level gives the index the DX7 gives it");
    }

    // Feedback 5 on a lone operator: its phase moved by its own output, pi / 2 at the peak, so its 2nd harmonic stands
    // J2(2b) / 2 J1(b) under its 1st, b = pi / 2.
    {
        const std::vector<float> fb = play(patch(32, [&](dx7::Voice &v) {
            set(v, 1, dx7::OutputLevel, 0);
            set(v, 6, dx7::OutputLevel, 99);
            v.data[dx7::kVoiceFields + dx7::Feedback] = 5;
        }));
        const double b = std::numbers::pi / 2.0;
        const double want = 20.0 * std::log10(std::cyl_bessel_j(2.0, 2.0 * b) / (2.0 * std::cyl_bessel_j(1.0, b)));
        const double got = at(fb, 880.0) - at(fb, 440.0);
        std::printf("      feedback 5: 2nd harmonic %.1f dB under the 1st, %.1f wanted\n", got, want);
        check(std::abs(got - want) < 1.5, "DX7 feedback moves an operator's phase as far as the DX7's does");
    }

    // DX7 feedback runs at the DX7's rate whatever the voices' is: feedback 6 (pi at its peak, past where a saw's
    // harmonics are) giving its harmonics within 0.5 dB at 2x and 4x oversampling.
    {
        auto harmonicsAt = [&](int oversampling) {
            const dx7::Voice v = patch(32, [&](dx7::Voice &voice) {
                set(voice, 1, dx7::OutputLevel, 0);
                set(voice, 6, dx7::OutputLevel, 99);
                voice.data[dx7::kVoiceFields + dx7::Feedback] = 6;
            });
            Engine e(oversampling);
            gui::UiLayout layout;
            dx7::toPatch(v, e.p, layout);
            e.noteOn(69);
            e.render(9600);
            const std::vector<float> out = e.render(kFftSize);
            std::array<double, 4> h{};
            for (int k = 0; k < 4; ++k)
                h[k] = at(out, 440.0 * (k + 2)) - at(out, 440.0);
            return h;
        };
        const auto two = harmonicsAt(2), four = harmonicsAt(4);
        double most = 0.0;
        for (int k = 0; k < 4; ++k)
            most = std::max(most, std::abs(two[k] - four[k]));
        std::printf("      DX7 feedback 6: harmonics 2-5 at 2x %.1f %.1f %.1f %.1f dB, at 4x %.1f %.1f %.1f %.1f "
                    "(%.2f dB apart at most)\n",
                    two[0], two[1], two[2], two[3], four[0], four[1], four[2], four[3], most);
        check(most < 0.5, "DX7 feedback runs as the DX7's whatever the oversampling");
    }

    // A decay at rate 50 falls 0.75 dB a step... 2756 units, 64.8 dB a second: 40 dB down in 0.62 s.
    {
        const dx7::Voice v = patch(32, [&](dx7::Voice &voice) {
            set(voice, 1, dx7::Rate1 + 1, 50);
            set(voice, 1, dx7::Level1 + 1, 0);
            set(voice, 1, dx7::Level1 + 2, 0);
        });
        Engine e(2);
        gui::UiLayout layout;
        dx7::toPatch(v, e.p, layout);
        e.noteOn(69);
        const std::vector<float> out = e.render(96000);
        auto rmsDb = [&](size_t from) {
            double sum = 0.0;
            for (size_t i = from; i < from + 480; ++i)
                sum += out[i] * out[i];
            return 10.0 * std::log10(sum / 480.0 + 1e-30);
        };
        const double peak = rmsDb(0);
        size_t down = 0;
        while (down + 960 < out.size() && rmsDb(down) > peak - 40.0)
            down += 48;
        const double seconds = down / kSampleRate, want = 40.0 / (4.0 / (64.0 / 44100.0) * 6.0206 / 256.0);
        std::printf("      rate 50: 40 dB down in %.3f s, %.3f wanted\n", seconds, want);
        check(std::abs(seconds / want - 1.0) < 0.08, "a DX7 envelope's rate sets its decay's time as on the DX7");
    }

    // The Rate/Level envelope (see RateEnvelope), as imported voices play it. A voice of OP1 alone on an A4, its level
    // read a 10 ms window at a time (its RMS, dB), from `at` seconds; and the time it first falls `db` under `from`'s.
    auto rateVoice = [&](auto setup) {
        return patch(32, [&](dx7::Voice &voice) { setup(voice); });
    };
    auto rmsAt = [](const std::vector<float> &out, double at) {
        const size_t from = static_cast<size_t>(at * kSampleRate), n = 480;
        double sum = 0.0;
        for (size_t i = from; i < from + n && i < out.size(); ++i)
            sum += out[i] * out[i];
        return 10.0 * std::log10(sum / n + 1e-30);
    };
    auto fallTime = [&](const std::vector<float> &out, double from, double db) {
        const double start = rmsAt(out, from);
        for (double t = from; t + 0.02 < out.size() / kSampleRate; t += 0.005)
            if (rmsAt(out, t) < start - db)
                return t - from;
        return -1.0;
    };
    auto run = [](const dx7::Voice &v, int key, double seconds, double letGo = -1.0) {
        Engine e(2);
        gui::UiLayout layout;
        dx7::toPatch(v, e.p, layout);
        e.noteOn(key);
        std::vector<float> out;
        if (letGo >= 0.0) {
            out = e.render(static_cast<size_t>(letGo * kSampleRate));
            e.noteOff(key);
            const std::vector<float> rest = e.render(static_cast<size_t>((seconds - letGo) * kSampleRate));
            out.insert(out.end(), rest.begin(), rest.end());
        } else {
            out = e.render(static_cast<size_t>(seconds * kSampleRate));
        }
        return out;
    };
    // A decay at rate 20, longer than a drawn envelope may last: 2.03 dB a second, 40 dB down in 19.7 s.
    {
        const std::vector<float> out = run(rateVoice([&](dx7::Voice &v) {
                                               set(v, 1, dx7::Rate1 + 1, 20);
                                               set(v, 1, dx7::Level1 + 1, 0);
                                               set(v, 1, dx7::Level1 + 2, 0);
                                           }),
                                           69, 24.0);
        const double seconds = fallTime(out, 0.05, 40.0), want = 40.0 / (0.125 / RateEnvelope::kTickSeconds * 6.0206 / 256.0);
        std::printf("      rate 20: 40 dB down in %.2f s, %.2f wanted\n", seconds, want);
        check(std::abs(seconds / want - 1.0) < 0.05, "a DX7 envelope plays a decay longer than 10 s at its rate");
    }
    // Rate Scaling 7 an octave up, A4 to A5: its speed from 33 to 36 of the 64, the decay at rate 30 5/8 as long.
    {
        const dx7::Voice v = rateVoice([&](dx7::Voice &voice) {
            set(voice, 1, dx7::Rate1 + 1, 30);
            set(voice, 1, dx7::Level1 + 1, 0);
            set(voice, 1, dx7::Level1 + 2, 0);
            set(voice, 1, dx7::RateScaling, 7);
        });
        const double a4 = fallTime(run(v, 69, 1.5), 0.02, 40.0), a5 = fallTime(run(v, 81, 1.5), 0.02, 40.0);
        std::printf("      rate scaling 7: 40 dB down in %.3f s at A4, %.3f s at A5 (%.3f, 0.625 wanted)\n", a4, a5, a5 / a4);
        check(a4 > 0.0 && std::abs(a5 / a4 / 0.625 - 1.0) < 0.05, "a DX7 envelope's rate scaling speeds it up as the DX7's");
    }
    // Let go half way up a slow attack (rate 20 to L1 99), it releases at R4 (50, 64.8 dB a second) from where it is,
    // without a jump: as loud just after as just before, and 19.4 dB down 0.3 s on.
    {
        const std::vector<float> out = run(rateVoice([&](dx7::Voice &voice) {
                                               set(voice, 1, dx7::Rate1, 20);
                                               set(voice, 1, dx7::Rate1 + 3, 50);
                                           }),
                                           69, 1.5, 0.5);
        const double before = rmsAt(out, 0.49), after = rmsAt(out, 0.505), later = rmsAt(out, 0.805);
        std::printf("      let go mid-attack: %.1f dB before, %.1f just after, %.1f dB 0.3 s on (19.4 wanted)\n", before,
                    after, after - later);
        check(std::abs(after - before) < 1.0 && std::abs(after - later - 19.4) < 2.0,
              "a DX7 envelope let go mid-attack releases at its R4 from where it is");
    }
    // L4 heard (50): let go, it goes there and then fades out, its voice ending.
    {
        const std::vector<float> out = run(rateVoice([&](dx7::Voice &voice) { set(voice, 1, dx7::Level1 + 3, 50); }),
                                           69, 1.0, 0.2);
        const double end = rmsAt(out, 0.95);
        std::printf("      L4 at 50: %.0f dB once let go and done\n", end);
        check(end < -200.0, "a DX7 envelope that releases to a level heard still ends its voice");
    }
    // Its rates and levels moving under it (turned or modulated as a note plays): held at L3, L3 turned from 99 to 70
    // takes the level there, 21 dB down (63 to 49 halves of its scaled level, 64 units each); and R2 turned from 20 to
    // 60 half a second into a decay speeds it up there, from 2 dB a second to near 200.
    {
        Engine e(2);
        gui::UiLayout layout;
        dx7::toPatch(patch(32, [](dx7::Voice &) {}), e.p, layout);
        e.noteOn(69);
        std::vector<float> out = e.render(static_cast<size_t>(0.3 * kSampleRate));
        e.p[paramIndex(0, OpParam::EnvLevel3)] = 70.0;
        const std::vector<float> rest = e.render(static_cast<size_t>(0.5 * kSampleRate));
        out.insert(out.end(), rest.begin(), rest.end());
        const double held = rmsAt(out, 0.25), moved = rmsAt(out, 0.7);
        Engine d(2);
        dx7::toPatch(rateVoice([&](dx7::Voice &voice) {
                         set(voice, 1, dx7::Rate1 + 1, 20);
                         set(voice, 1, dx7::Level1 + 1, 0);
                         set(voice, 1, dx7::Level1 + 2, 0);
                     }),
                     d.p, layout);
        d.noteOn(69);
        std::vector<float> fall = d.render(static_cast<size_t>(0.5 * kSampleRate));
        d.p[paramIndex(0, OpParam::EnvRate2)] = 60.0;
        const std::vector<float> faster = d.render(static_cast<size_t>(0.5 * kSampleRate));
        fall.insert(fall.end(), faster.begin(), faster.end());
        const double before = rmsAt(fall, 0.49), after = rmsAt(fall, 0.74);
        std::printf("      L3 turned from 99 to 70 while held: %.1f dB (21 wanted); R2 turned up mid-decay: %.1f dB in "
                    "0.25 s\n",
                    moved - held, after - before);
        check(std::abs(moved - held + 21.0) < 2.0 && after - before < -30.0,
              "a DX7 envelope follows its levels and rates as they move under it, held at L3 too");
    }

    // Velocity sensitivity 7: a note at 64 some 16 dB under one at 127, as the DX7's velocity curve has it.
    {
        const dx7::Voice v = patch(32, [&](dx7::Voice &voice) {
            set(voice, 1, dx7::VelocitySens, 7);
            set(voice, 1, dx7::OutputLevel, 80);
        });
        const double loud = at(play(v, 1.0), 440.0), soft = at(play(v, 64.0 / 127.0), 440.0);
        const double want = 7.0 * 261.0 * (1.0 - std::pow(64.0 / 126.0, 0.3)) / 128.0 * 6.0206;
        std::printf("      velocity 64 against 127 at sensitivity 7: %.1f dB, %.1f wanted\n", soft - loud, -want);
        check(std::abs(soft - loud + want) < 1.5, "DX7 velocity sensitivity turns an operator down as the DX7 does");
    }

    // Keyboard level scaling, depth 99 to the right of break point 39 (17 keys over it, MIDI 56): -LIN a note's level
    // 23.25 dB down an octave out (4 steps of 3 keys at 31 levels of 0.75 dB, as the DX7 truncates them), -EXP 5.25 dB
    // two octaves out and 11.25 three; each note against itself at the break point's note. And +LIN on an operator at
    // full level lifting nothing, as on the DX7, which holds it at its top level.
    {
        auto levelAt = [&](int curve, int key, int level) {
            dx7::Voice v = patch(32, [&](dx7::Voice &voice) {
                set(voice, 1, dx7::RightDepth, 99);
                set(voice, 1, dx7::RightCurve, curve);
                set(voice, 1, dx7::OutputLevel, level);
            });
            Engine e(2);
            gui::UiLayout layout;
            dx7::toPatch(v, e.p, layout);
            e.noteOn(key);
            e.render(9600);
            const std::vector<float> out = e.render(kFftSize);
            double sum = 0.0;
            for (float x : out)
                sum += static_cast<double>(x) * x;
            return 10.0 * std::log10(sum / out.size() + 1e-30);
        };
        const double lin = levelAt(0, 68, 99) - levelAt(0, 56, 99);
        const double exp2Oct = levelAt(1, 80, 99) - levelAt(1, 56, 99), exp3Oct = levelAt(1, 92, 99) - levelAt(1, 56, 99);
        const double lifted = levelAt(3, 80, 99) - levelAt(3, 56, 99), liftedLower = levelAt(3, 68, 80) - levelAt(3, 56, 80);
        std::printf("      key scaling: -LIN %.2f dB an octave out (-23.25); -EXP %.2f two (-5.25), %.2f three (-11.25); "
                    "+LIN at level 99 %+.2f, at 80 %+.2f an octave out\n",
                    lin, exp2Oct, exp3Oct, lifted, liftedLower);
        check(std::abs(lin + 23.25) < 1.0 && std::abs(exp2Oct + 5.25) < 1.0 && std::abs(exp3Oct + 11.25) < 1.0 &&
                  std::abs(lifted) < 0.1 && liftedLower > 1.0 && liftedLower < 23.86 + 0.1,
              "a DX7 voice's keyboard level scaling moves its operators' levels as the DX7's does");
    }

    // The pitch envelope held at level 74, 24 steps of 3/8 semitone up: 9 semitones, an A4 at F#5.
    {
        const std::vector<float> bent = play(patch(1, [](dx7::Voice &v) {
            for (int i = 0; i < 3; ++i)
                v.data[dx7::kVoiceFields + dx7::PitchLevel1 + i] = 74;
        }));
        const double hz = 440.0 * std::exp2(9.0 / 12.0);
        std::printf("      pitch envelope at 74: %.0f dB at %.1f Hz over 440\n", at(bent, hz) - at(bent, 440.0), hz);
        check(at(bent, hz) - at(bent, 440.0) > 40.0, "a DX7 voice's pitch envelope bends its pitch");
    }

    // Amp modulation at full depth and sensitivity 3, the LFO at speed 10 (a cycle of 0.64 s): the DX7 turns the level
    // down the most where its LFO is lowest, so a saw up's level rises across the cycle and a saw down's falls.
    {
        auto tremolo = [&](int wave) {
            const std::vector<float> out = run(patch(1, [&](dx7::Voice &v) {
                                                   set(v, 1, dx7::AmpModSens, 3);
                                                   v.data[dx7::kVoiceFields + dx7::LfoAmpDepth] = 99;
                                                   v.data[dx7::kVoiceFields + dx7::LfoSpeed] = 10;
                                                   v.data[dx7::kVoiceFields + dx7::LfoWave] = static_cast<uint8_t>(wave);
                                               }),
                                               69, 1.0 / 1.575); // a cycle, at lfoHz(10)
            auto rms = [&](double from, double to) { // over a share of the cycle, in dB
                const size_t a = static_cast<size_t>(from * out.size()), b = static_cast<size_t>(to * out.size());
                double sum = 0.0;
                for (size_t i = a; i < b; ++i)
                    sum += static_cast<double>(out[i]) * out[i];
                return 10.0 * std::log10(sum / static_cast<double>(b - a) + 1e-30);
            };
            return rms(0.75, 0.95) - rms(0.05, 0.25); // its last fifth against its first, past the attack
        };
        const double up = tremolo(2), down = tremolo(1);
        std::printf("      tremolo across a cycle: saw up %+.1f dB, saw down %+.1f dB\n", up, down);
        check(up > 10.0 && down < -10.0, "a DX7 voice's amp modulation turns its level down where its LFO is lowest");
    }

    // The LFO at speed 35 runs at about 5.6 Hz (5.62 on the DX7), its pitch depth on Pitch; and the presets made of a
    // bank are named for their voices, in order, and read back as made.
    {
        dx7::Voice v = dx7::initVoice();
        v.data[dx7::kVoiceFields + dx7::LfoPitchDepth] = 50;
        v.data[dx7::kVoiceFields + dx7::LfoWave] = 4;
        const auto values = std::make_unique<ParamValues>();
        gui::UiLayout layout;
        dx7::toPatch(v, *values, layout);
        double hz = 0.0;
        for (int s = 0; s < kMaxSources; ++s)
            if ((*values)[paramIndex(s, SourceParam::Type)] == static_cast<double>(SourceType::Lfo))
                hz = rateFromNorm((*values)[paramIndex(s, SourceParam::Rate)]);
        const std::vector<uint8_t> two = dx7::bankSysex({voice, v});
        const std::vector<dx7::Preset> presets = dx7::presetsOf(two.data(), two.size(), "Rom 1A");
        bool same = presets.size() == 32;
        if (same) {
            dx7::toPatch(voice, *values, layout);
            MemoryReader reader{presets[0].bytes.data(), presets[0].bytes.size(), 0};
            const clap_istream_t in = memoryIn(reader);
            const auto loaded = std::make_unique<ParamValues>();
            same = readParams(&in, *loaded) && readLayout(&in).presetInfo.description == trf(Str::PresetDx7Voice, {"01", "Rom 1A"});
            for (int i = 0; i < kParamCount && same; ++i)
                same = std::abs((*loaded)[i] - (*values)[i]) <= 1e-6 * std::max(1.0, std::abs((*values)[i]));
        }
        std::printf("      LFO speed 35: %.2f Hz; presets \"%s\", \"%s\", \"%s\"\n", hz,
                    presets.empty() ? "" : presets[0].name.c_str(), presets.size() < 2 ? "" : presets[1].name.c_str(),
                    presets.size() < 32 ? "" : presets[31].name.c_str());
        check(std::abs(hz - 5.62) < 0.1 && same && presets[0].name == "01 E.PIANO 1" &&
                  presets[1].name == "02 INIT VOICE" && presets[31].name == "32 INIT VOICE",
              "a DX7 voice's LFO runs at its rate, and a bank's presets are named for its voices and read back as made");
    }
}

// --- DC: the voices' sum going into the effects has its DC taken out. A square squeezed to a narrow pulse comes
// out with none; a Ring route at unison (a sine squared: DC and the octave) goes into a hard shaper as the octave
// alone, clipped evenly (no 4th harmonic of the note), not lifted onto one side of the curve; and the output falls
// silent, exactly, once the note has rung out, so the effects can rest.
void dcChecks() {
    {
        Engine e(2);
        e.p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Square);
        e.p[paramIndex(0, OpParam::Warp)] = static_cast<double>(OpWarp::Squeeze);
        e.p[paramIndex(0, OpParam::WarpAmount)] = 0.8;
        e.noteOn(57);
        e.render(96000);
        const std::vector<float> out = e.render(48000);
        double mean = 0.0, peak = 0.0;
        for (float x : out) {
            mean += x;
            peak = std::max(peak, static_cast<double>(std::abs(x)));
        }
        mean /= static_cast<double>(out.size());
        std::printf("      a squeezed square's DC at the output: %.2e (peak %.2f)\n", mean, peak);
        check(std::abs(mean) < 1e-3, "a squeezed square comes out with no DC");
    }
    {
        Engine e(2);
        e.p[paramIndex(1, OpParam::Enabled)] = 1.0;
        e.p[matrixIndex(1, 0)] = 1.0;
        e.p[routeModeIndex(1, 0)] = static_cast<double>(RouteMode::Ring);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        e.p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Hard);
        e.p[paramIndex(0, FxParam::ShaperDrive)] = 24.0;
        chain(e.p, {0});
        const int key = 57;
        e.noteOn(key);
        e.render(48000);
        const std::vector<double> power = powerSpectrum(e.render(kFftSize));
        auto level = [&](double hz) {
            const long center = std::lround(hz * kFftSize / kSampleRate);
            double most = 1e-30;
            for (long b = center - 3; b <= center + 3; ++b)
                most = std::max(most, power[static_cast<size_t>(b)]);
            return 10.0 * std::log10(most);
        };
        const double octave = level(2.0 * keyHz(key)), uneven = level(4.0 * keyHz(key)) - octave;
        std::printf("      a ring at unison clipped hard: its 4th harmonic %.1f dB under the octave\n", -uneven);
        check(uneven < -40.0, "the voices' DC is taken out before the effects");
        e.noteOff(key);
        e.render(96000);
        std::vector<float> left(kBlock), right(kBlock);
        e.synth().render(left.data(), right.data(), kBlock, e.p);
        const bool silent = std::all_of(left.begin(), left.end(), [](float x) { return x == 0.0f; }) &&
                            std::all_of(right.begin(), right.end(), [](float x) { return x == 0.0f; });
        check(silent && !e.synth().effectsRinging(), "the DC blocker leaves silence once the note ends, and the effects rest");
    }
}

// The effects that run oversampled (the shaper and filter effects at twice the rate, an amp at its oversampling) are a
// few samples late, later toward the top, and a flanger through zero waits its dry sound: paths that meet beside them
// are put through as much up and down, and as long a wait, so they meet in step (see fxlag.h). Measured on an impulse
// once the effects have settled, through effects that leave the sound as it is (a filter, an amp and a flanger at
// Mix 0), across 20 Hz to 18 kHz: flat where they meet, and as late as the plugin says.
void latencyChecks() {
    std::printf("effects latency:\n");
    auto from = [](int node, int input) { return fxInputIndex(node, input, FxInputField::From); };
    auto level = [](int node, int input) { return fxInputIndex(node, input, FxInputField::Level); };
    auto port = [](int p) { return fxPortValue(p); };
    auto fresh = [&] {
        auto p = std::make_unique<ParamValues>();
        for (int i = 0; i < kParamCount; ++i)
            (*p)[i] = paramTable()[i].def;
        (*p)[from(kFxOutNode, 0)] = 0.0;
        return p;
    };
    // An effect leaving the sound as it is, but for running oversampled: a filter at Mix 0, an amp at Mix 0.
    auto filterAtNoMix = [](ParamValues &p, int slot) {
        p[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Filter);
        p[paramIndex(slot, FxParam::FilterMix)] = 0.0;
    };
    auto ampAtNoMix = [](ParamValues &p, int slot, int factorValue) {
        p[paramIndex(slot, FxParam::Type)] = static_cast<double>(FxType::Amp);
        p[paramIndex(slot, FxParam::AmpMix)] = 0.0;
        p[paramIndex(slot, FxParam::AmpOversampling)] = factorValue; // 1 << it
    };
    struct Response {
        double flatDb;   // the farthest the level strays from 0 dB, 20 Hz to 18 kHz
        double centroid; // how late, in samples (its impulse response's centroid)
    };
    auto measure = [&](const ParamValues &p) {
        auto fx = std::make_unique<Effects>();
        fx->prepare(kSampleRate);
        constexpr uint32_t kPiece = 64;
        float left[kPiece], right[kPiece];
        for (int i = 0; i < 100; ++i) { // the effects fading in
            std::fill_n(left, kPiece, 0.0f);
            std::fill_n(right, kPiece, 0.0f);
            fx->process(left, right, kPiece, p, 120.0, 60.0);
        }
        constexpr int kLength = 4096;
        std::vector<double> h(kLength);
        for (int t = 0; t < kLength; t += kPiece) {
            std::fill_n(left, kPiece, 0.0f);
            std::fill_n(right, kPiece, 0.0f);
            if (t == 0)
                left[0] = right[0] = 1.0f;
            fx->process(left, right, kPiece, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kPiece; ++s)
                h[t + s] = left[s];
        }
        Response r{0.0, 0.0};
        double moment = 0.0, sum = 0.0;
        for (int n = 0; n < kLength; ++n) {
            moment += n * h[n];
            sum += h[n];
        }
        r.centroid = moment / sum;
        for (double hz = 20.0; hz <= 18000.0; hz *= std::exp2(1.0 / 12.0)) {
            std::complex<double> at;
            for (int n = 0; n < kLength; ++n)
                at += h[n] * std::polar(1.0, -2.0 * std::numbers::pi * hz * n / kSampleRate);
            r.flatDb = std::max(r.flatDb, std::abs(20.0 * std::log10(std::abs(at))));
        }
        return r;
    };
    const std::array<double, FxLag::kKinds> delays = fxLagDelays(kSampleRate);
    auto lagOf = [](const ParamValues &p) { return fxOutputLag([&p](int i) { return p[i]; }); };

    // The input straight out beside a filter at Mix 0, at half each: in step, flat, where unaligned they'd cancel
    // near 5 kHz. Switched off, the filter is as late (its dry sound goes up and down too), so they stay in step.
    auto parallel = fresh();
    filterAtNoMix(*parallel, 0);
    (*parallel)[from(0, 0)] = port(kFxPortIn);
    (*parallel)[from(kFxOutNode, 0)] = port(fxSlotPort(0));
    (*parallel)[from(kFxOutNode, 1)] = port(kFxPortIn);
    (*parallel)[level(kFxOutNode, 0)] = 0.5;
    (*parallel)[level(kFxOutNode, 1)] = 0.5;
    const Response beside = measure(*parallel);
    (*parallel)[paramIndex(0, FxParam::Enabled)] = 0.0;
    const Response besideOff = measure(*parallel);
    const double besideLate = fxLagSamples(lagOf(*parallel), delays);

    // A split at 3 kHz, its low band through an amp at 4x and its high one through a filter, into its join: the
    // bands sum back flat, both as late as the amp and the filter one after the other.
    auto split = fresh();
    ampAtNoMix(*split, 0, 2);
    filterAtNoMix(*split, 1);
    (*split)[paramIndex(0, SplitParam::Used)] = 1.0;
    (*split)[paramIndex(0, SplitParam::Low)] = normFromCutoff(3000.0);
    (*split)[paramIndex(0, SumParam::Used)] = 1.0;
    (*split)[from(fxSplitNode(0), 0)] = port(kFxPortIn);
    (*split)[from(0, 0)] = port(fxBandPort(0, 0));
    (*split)[from(1, 0)] = port(fxBandPort(0, kFxBands - 1));
    (*split)[from(fxSumNode(0), 0)] = port(fxSlotPort(1));
    (*split)[from(fxSumNode(0), 1)] = port(fxSlotPort(0));
    (*split)[from(kFxOutNode, 0)] = port(fxSumPort(0));
    const Response bands = measure(*split);
    const double bandsLate = fxLagSamples(lagOf(*split), delays);
    // The crossover's all-pass makes the split itself late at low frequencies, with or without effects on its bands:
    // the same split with its bands straight into the join, against which the effects add their lag.
    auto bare = fresh();
    for (const int i : {paramIndex(0, SplitParam::Used), paramIndex(0, SplitParam::Low), paramIndex(0, SumParam::Used),
                        from(fxSplitNode(0), 0), from(kFxOutNode, 0)})
        (*bare)[i] = (*split)[i];
    (*bare)[from(fxSumNode(0), 0)] = port(fxBandPort(0, kFxBands - 1));
    (*bare)[from(fxSumNode(0), 1)] = port(fxBandPort(0, 0));
    const Response crossover = measure(*bare);

    // Two filters one after the other, beside an amp at 8x, both out at half.
    auto mixed = fresh();
    filterAtNoMix(*mixed, 0);
    filterAtNoMix(*mixed, 1);
    ampAtNoMix(*mixed, 2, 3);
    (*mixed)[from(0, 0)] = port(kFxPortIn);
    (*mixed)[from(1, 0)] = port(fxSlotPort(0));
    (*mixed)[from(2, 0)] = port(kFxPortIn);
    (*mixed)[from(kFxOutNode, 0)] = port(fxSlotPort(1));
    (*mixed)[from(kFxOutNode, 1)] = port(fxSlotPort(2));
    (*mixed)[level(kFxOutNode, 0)] = 0.5;
    (*mixed)[level(kFxOutNode, 1)] = 0.5;
    const Response kinds = measure(*mixed);
    const double kindsLate = fxLagSamples(lagOf(*mixed), delays);

    // Nine filters one after the other beside the input straight out, both out at half: the input's path put through
    // nine paths up and down (more than a pad once took, which then went without, out of step).
    auto long9 = fresh();
    for (int slot = 0; slot < 9; ++slot) {
        filterAtNoMix(*long9, slot);
        (*long9)[from(slot, 0)] = port(slot == 0 ? kFxPortIn : fxSlotPort(slot - 1));
    }
    (*long9)[from(kFxOutNode, 0)] = port(fxSlotPort(8));
    (*long9)[from(kFxOutNode, 1)] = port(kFxPortIn);
    (*long9)[level(kFxOutNode, 0)] = 0.5;
    (*long9)[level(kFxOutNode, 1)] = 0.5;
    const Response nine = measure(*long9);
    const double nineLate = fxLagSamples(lagOf(*long9), delays);

    // The input straight out beside a shaper at Mix 0 run at 4x, and at 8x: in step and flat, as late as fxlag.h
    // says a path at its oversampling is.
    auto shaperBeside = [&](int factorValue) {
        auto p = fresh();
        (*p)[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
        (*p)[paramIndex(0, FxParam::ShaperMix)] = 0.0;
        (*p)[paramIndex(0, FxParam::ShaperOversampling)] = factorValue; // 1 << it
        (*p)[from(0, 0)] = port(kFxPortIn);
        (*p)[from(kFxOutNode, 0)] = port(fxSlotPort(0));
        (*p)[from(kFxOutNode, 1)] = port(kFxPortIn);
        (*p)[level(kFxOutNode, 0)] = 0.5;
        (*p)[level(kFxOutNode, 1)] = 0.5;
        return std::make_pair(measure(*p), fxLagSamples(lagOf(*p), delays));
    };
    const auto [shaper4, shaper4Late] = shaperBeside(2);
    const auto [shaper8, shaper8Late] = shaperBeside(3);

    // The input straight out beside a flanger through zero at Mix 0, its Delay at its shortest and its longest, and
    // switched off: in step and flat, as late as its wait whatever its Delay, which turning it doesn't move.
    auto flangerBeside = [&](double delayNorm, bool enabled) {
        auto p = fresh();
        (*p)[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Flanger);
        (*p)[paramIndex(0, FxParam::FlangerMix)] = 0.0;
        (*p)[paramIndex(0, FxParam::FlangerThroughZero)] = 1.0;
        (*p)[paramIndex(0, FxParam::FlangerDelay)] = delayNorm;
        (*p)[paramIndex(0, FxParam::Enabled)] = enabled ? 1.0 : 0.0;
        (*p)[from(0, 0)] = port(kFxPortIn);
        (*p)[from(kFxOutNode, 0)] = port(fxSlotPort(0));
        (*p)[from(kFxOutNode, 1)] = port(kFxPortIn);
        (*p)[level(kFxOutNode, 0)] = 0.5;
        (*p)[level(kFxOutNode, 1)] = 0.5;
        return std::make_pair(measure(*p), fxLagSamples(lagOf(*p), delays));
    };
    const ParamDef &flangerDelay = paramTable()[paramIndex(0, FxParam::FlangerDelay)];
    const auto [flangerShort, flangerLate] = flangerBeside(flangerDelay.min, true);
    const auto [flangerLong, flangerLongLate] = flangerBeside(flangerDelay.max, true);
    const auto [flangerOff, flangerOffLate] = flangerBeside(flangerDelay.max, false);
    std::printf("      in beside a flanger through zero: within %.3f dB, %.2f late at its shortest Delay, within %.3f "
                "dB, %.2f late at its longest, within %.3f dB, %.2f late off (said %.2f)\n",
                flangerShort.flatDb, flangerShort.centroid, flangerLong.flatDb, flangerLong.centroid, flangerOff.flatDb,
                flangerOff.centroid, flangerLate);
    std::printf("      in beside a shaper at 4x: within %.3f dB, %.2f late (said %.2f); at 8x: within %.3f dB, %.2f "
                "late (said %.2f)\n",
                shaper4.flatDb, shaper4.centroid, shaper4Late, shaper8.flatDb, shaper8.centroid, shaper8Late);

    // Nothing oversampled: on time.
    auto plain = fresh();
    (*plain)[from(kFxOutNode, 0)] = port(kFxPortIn);
    const Response none = measure(*plain);

    std::printf("      one path up and down at 2x, 4x, 8x: %.2f, %.2f, %.2f samples late; the voices' decimator at 2x "
                "%.2f\n",
                delays[0], delays[1], delays[2], decimatorDelay(kSampleRate, 2));
    std::printf("      in beside a filter: within %.3f dB, %.2f samples late (said %.2f); the filter off: within %.3f "
                "dB, %.2f late\n",
                beside.flatDb, beside.centroid, besideLate, besideOff.flatDb, besideOff.centroid);
    std::printf("      split, an amp at 4x and a filter on its bands: within %.3f dB, %.2f late past the crossover's %.2f "
                "(said %.2f)\n",
                bands.flatDb, bands.centroid - crossover.centroid, crossover.centroid, bandsLate);
    std::printf("      two filters beside an amp at 8x: within %.3f dB, %.2f late (said %.2f); nothing oversampled: "
                "within %.3f dB, %.2f late\n",
                kinds.flatDb, kinds.centroid, kindsLate, none.flatDb, none.centroid);
    std::printf("      nine filters beside the input: within %.3f dB, %.2f late (said %.2f)\n", nine.flatDb, nine.centroid,
                nineLate);
    auto near = [](double a, double b) { return std::abs(a - b) < 0.05; };
    check(beside.flatDb < 0.05 && besideOff.flatDb < 0.05 && near(beside.centroid, besideLate) &&
              near(besideOff.centroid, besideLate) && besideLate > 3.0,
          "a path beside an oversampled effect meets it in step, switched on or off");
    check(nine.flatDb < 0.05 && near(nine.centroid, nineLate),
          "a path beside nine oversampled effects in a row meets them in step");
    check(shaper4.flatDb < 0.05 && shaper8.flatDb < 0.05 && near(shaper4.centroid, shaper4Late) &&
              near(shaper8.centroid, shaper8Late) && shaper8Late > shaper4Late,
          "a path beside a shaper oversampled 4x or 8x meets it in step, as late as its oversampling says");
    check(flangerShort.flatDb < 0.05 && flangerLong.flatDb < 0.05 && flangerOff.flatDb < 0.05 &&
              near(flangerShort.centroid, flangerLate) && near(flangerLong.centroid, flangerLate) &&
              near(flangerOff.centroid, flangerLate) && flangerLongLate == flangerLate &&
              flangerOffLate == flangerLate && flangerLate == throughZeroWaitSamples(kSampleRate),
          "a path beside a flanger through zero meets it in step, its wait the same whatever its Delay, on or off");
    check(bands.flatDb < 0.05 && kinds.flatDb < 0.05,
          "a split's bands through effects oversampled differently sum back flat, and so do paths that meet");
    check(near(bands.centroid - crossover.centroid, bandsLate) && near(kinds.centroid, kindsLate) && none.flatDb < 1e-6 &&
              std::abs(none.centroid) < 1e-6,
          "the output is as late as its latest path, as the plugin reports, and on time with none oversampled");
}

// The effects' newer controls, each on its own in the chain, on what `input(t, side)` gives (t in samples).
void newerEffectChecks() {
    std::printf("the effects' newer controls:\n");
    struct Run {
        std::vector<float> left, right;
    };
    auto run = [](FxType type, const std::vector<std::pair<FxParam, double>> &set, size_t frames, auto &&input,
                  double rate = kSampleRate) {
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
        fx.prepare(rate);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(type);
        for (const auto &[field, value] : set)
            p[paramIndex(0, field)] = value;
        chain(p, {0});
        Run out;
        std::vector<float> left(kBlock), right(kBlock);
        for (size_t t = 0; t < frames; t += kBlock) {
            for (uint32_t s = 0; s < kBlock; ++s) {
                left[s] = static_cast<float>(input(t + s, 0));
                right[s] = static_cast<float>(input(t + s, 1));
            }
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            out.left.insert(out.left.end(), left.begin(), left.end());
            out.right.insert(out.right.end(), right.begin(), right.end());
        }
        return out;
    };
    auto noise = [](size_t t, int side) {
        uint32_t x = static_cast<uint32_t>(t * 2 + side) * 2654435761u;
        x ^= x >> 15;
        x *= 2246822519u;
        x ^= x >> 13;
        return ((x & 0xffff) / 65535.0 - 0.5) * 0.5;
    };
    // Energy below `below` Hz or above `above` (one of them 0) in `x` from `from` on, a kFftSize long stretch.
    auto bandEnergy = [](const std::vector<float> &x, size_t from, double low, double high) {
        std::vector<float> part(x.begin() + static_cast<long>(from), x.begin() + static_cast<long>(from + kFftSize));
        const std::vector<double> power = powerSpectrum(part);
        double sum = 1e-30;
        for (size_t b = 1; b < power.size(); ++b) {
            const double hz = b * kSampleRate / kFftSize;
            if (hz >= low && hz < high)
                sum += power[b];
        }
        return sum;
    };

    // The reverb's Low Cut at 1 kHz takes its tail's bass away (below 200 Hz), High Cut at 2 kHz its top (above
    // 8 kHz); Diffusion at 0 leaves its first 125 ms spikier (its crest factor higher) than at 100 %, and its tail
    // sparser for longer.
    {
        const size_t frames = static_cast<size_t>(kSampleRate) + kFftSize;
        auto burst = [&](size_t t, int side) { return t < 4800 ? noise(t, side) : 0.0; };
        const std::vector<std::pair<FxParam, double>> wet{{FxParam::ReverbMix, 1.0}, {FxParam::ReverbDecay, normFromTime(4.0)}};
        auto with = [&](std::vector<std::pair<FxParam, double>> more) {
            more.insert(more.begin(), wet.begin(), wet.end());
            return run(FxType::Reverb, more, frames, burst).left;
        };
        const std::vector<float> plain = with({}), lowCut = with({{FxParam::ReverbLowCut, normFromCutoff(1000.0)}}),
                                 highCut = with({{FxParam::ReverbHighCut, normFromCutoff(2000.0)}});
        const double bassDb = 10.0 * std::log10(bandEnergy(lowCut, 9600, 20.0, 200.0) / bandEnergy(plain, 9600, 20.0, 200.0));
        const double topDb =
            10.0 * std::log10(bandEnergy(highCut, 9600, 8000.0, 20000.0) / bandEnergy(plain, 9600, 8000.0, 20000.0));
        // A click after a moment's silence (the effect fading in meanwhile), all wet, and the 125 ms after it: at 0 the
        // all-passes are plain delays, so it comes later.
        auto click = [](size_t t, int) { return t == 4800 ? 1.0 : 0.0; };
        auto earlyCrest = [&](double diffusion) {
            const std::vector<float> x =
                run(FxType::Reverb,
                    {{FxParam::ReverbMix, 1.0}, {FxParam::ReverbPreDelay, 0.0}, {FxParam::ReverbDiffusion, diffusion}},
                    4800 + 6000, click)
                    .left;
            double peak = 0.0, power = 0.0;
            for (size_t i = 4800; i < 4800 + 6000; ++i) {
                peak = std::max(peak, static_cast<double>(std::abs(x[i])));
                power += x[i] * x[i];
            }
            return peak / std::sqrt(power / 6000.0 + 1e-30);
        };
        const double spiky = earlyCrest(0.0), diffuse = earlyCrest(1.0);
        std::printf("      reverb: low cut at 1 kHz takes %.1f dB off its tail under 200 Hz, high cut at 2 kHz %.1f dB "
                    "over 8 kHz; its first 125 ms' crest factor %.1f at 0 %% diffusion, %.1f at 100 %%\n",
                    -bassDb, -topDb, spiky, diffuse);
        check(bassDb < -15.0 && topDb < -15.0, "the reverb's Low Cut and High Cut take its tail's ends away");
        check(spiky > 1.3 * diffuse, "the reverb's Diffusion at 0 leaves its early echoes spikier");

        // A click into a hall and a plate (4 s decay), at Diffusion 0, 50 and 100 %: how dense its echoes are 200 to
        // 300 ms on (the share of samples further out than the window's RMS, against noise's 31.7 %), how loud the
        // tail is, how alike its sides are and how smooth its spectrum from 0.3 to 1.5 s (as the reverbs' tails are
        // measured further down). At 0 the lines' mixing is loosened as well as the input's all-passes: its echoes stay
        // apart for longer, and fill in as Diffusion rises. The all-passes are plain delays at 0, holding a click back
        // longer than the effect would ring on after silence did it not wait for it: it still comes out.
        auto density = [](const std::vector<float> &x, size_t from, size_t to) {
            double power = 0.0;
            for (size_t i = from; i < to; ++i)
                power += x[i] * x[i];
            const double rms = std::sqrt(power / (to - from));
            size_t out = 0;
            for (size_t i = from; i < to; ++i)
                out += std::abs(x[i]) > rms;
            return out / static_cast<double>(to - from) / 0.3173;
        };
        auto lumps = [](const std::vector<float> &tail, size_t from) {
            constexpr size_t kN = 4096;
            std::vector<double> power(kN / 2, 0.0);
            for (size_t start = from + static_cast<size_t>(0.3 * kSampleRate);
                 start + kN < from + static_cast<size_t>(1.5 * kSampleRate); start += kN / 2) {
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
        bool spreads = true, rings = true, smooth = true;
        for (ReverbType type : {ReverbType::Hall, ReverbType::Plate}) {
            double dense[3], energy[3];
            for (int k = 0; k < 3; ++k) {
                const Run out = run(FxType::Reverb,
                                    {{FxParam::ReverbType, static_cast<double>(type)},
                                     {FxParam::ReverbMix, 1.0},
                                     {FxParam::ReverbPreDelay, 0.0},
                                     {FxParam::ReverbDecay, normFromTime(4.0)},
                                     {FxParam::ReverbDiffusion, 0.5 * k}},
                                    4800 + static_cast<size_t>(1.6 * kSampleRate), click);
                const size_t at = 4800;
                dense[k] = density(out.left, at + 9600, at + 14400);
                double ll = 0.0, rr = 0.0, lr = 0.0;
                for (size_t i = at + 4800; i < at + static_cast<size_t>(1.5 * kSampleRate); ++i) {
                    ll += out.left[i] * out.left[i];
                    rr += out.right[i] * out.right[i];
                    lr += out.left[i] * out.right[i];
                }
                energy[k] = ll;
                const double stray = lumps(out.left, at), correlation = lr / std::sqrt(ll * rr + 1e-30);
                std::printf("      reverb %s at %3.0f %% diffusion: echo density %.2f at 200-300 ms, tail %.1f dB, its "
                            "spectrum straying %.2f dB, sides correlating %+.2f\n",
                            type == ReverbType::Hall ? "hall " : "plate", 50.0 * k, dense[k],
                            10.0 * std::log10(ll + 1e-30), stray, correlation);
                smooth = smooth && stray < 2.5 && std::abs(correlation) < 0.15;
            }
            spreads = spreads && dense[0] < 0.5 && dense[0] < dense[1] - 0.1 && dense[2] > 0.85;
            rings = rings && energy[0] > 0.5 * energy[2];
        }
        check(spreads, "the reverb's Diffusion at 0 keeps its tail's echoes apart for longer, filling in as it rises");
        check(rings, "a click into a reverb at 0 % Diffusion comes out of it, its all-passes' delay waited for");
        check(smooth, "the reverb's tail is smooth and its sides unalike at any Diffusion");
    }

    // The delay's Duck: a sine at -6 dB for half a second with Duck full up takes the echoes away while it plays
    // (what comes out within 0.1 % of the dry sine past its first echo), and they come back once it stops.
    {
        auto sine = [](size_t t, int) {
            return t < 24000 ? 0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t / kSampleRate) : 0.0;
        };
        const std::vector<std::pair<FxParam, double>> base{{FxParam::DelayTimeL, normFromTime(0.1)},
                                                           {FxParam::DelayTimeR, normFromTime(0.1)},
                                                           {FxParam::DelayFeedback, 0.6},
                                                           {FxParam::DelayMix, 0.5}};
        std::vector<std::pair<FxParam, double>> ducked = base;
        ducked.push_back({FxParam::DelayDuck, 1.0});
        const std::vector<float> plain = run(FxType::Delay, base, 48000, sine).left,
                                 duck = run(FxType::Delay, ducked, 48000, sine).left;
        double during = 0.0, plainAfter = 0.0, duckAfter = 0.0;
        for (size_t t = 9600; t < 24000; ++t)
            during = std::max(during, std::abs(duck[t] - sine(t, 0)));
        for (size_t t = 40000; t < 48000; ++t) {
            plainAfter = std::max(plainAfter, static_cast<double>(std::abs(plain[t])));
            duckAfter = std::max(duckAfter, static_cast<double>(std::abs(duck[t])));
        }
        std::printf("      delay duck: %.4f off the dry sine while it plays; after it, echoes at %.3f (%.3f unducked)\n",
                    during, duckAfter, plainAfter);
        check(during < 0.0005 && duckAfter > 0.5 * plainAfter,
              "the delay's Duck takes its echoes away while what goes in is loud, and lets them back after");
    }

    // The shaper's Tone: a quiet sine the curve leaves straight (a Hard curve, below its ceiling) comes out as at 0
    // either way (its shelves undoing each other), while a loud one comes out unlike at 0. Its oversampling: a C7
    // through a hard curve aliases less at 4x and 8x than at 2x.
    {
        const double c7 = keyHz(96);
        auto quiet = [](size_t t, int) {
            return 0.1 * std::sin(2.0 * std::numbers::pi * 300.0 * t / kSampleRate) +
                   0.1 * std::sin(2.0 * std::numbers::pi * 5000.0 * t / kSampleRate);
        };
        auto loud = [](size_t t, int) { return 0.9 * std::sin(2.0 * std::numbers::pi * 300.0 * t / kSampleRate); };
        auto shaped = [&](double tone, auto &&input, double drive) {
            return run(FxType::Shaper,
                       {{FxParam::ShaperMode, static_cast<double>(ClipMode::Hard)},
                        {FxParam::ShaperDrive, drive},
                        {FxParam::ShaperLink, 0.0},
                        {FxParam::ShaperTone, tone}},
                       24000, input)
                .left;
        };
        auto most = [](const std::vector<float> &a, const std::vector<float> &b) {
            double m = 0.0;
            for (size_t i = 4800; i < a.size(); ++i)
                m = std::max(m, static_cast<double>(std::abs(a[i] - b[i])));
            return m;
        };
        const std::vector<float> quiet0 = shaped(0.0, quiet, 0.0);
        const double quietUp = most(shaped(1.0, quiet, 0.0), quiet0), quietDown = most(shaped(-1.0, quiet, 0.0), quiet0);
        const std::vector<float> loud0 = shaped(0.0, loud, 12.0);
        const double loudUp = most(shaped(1.0, loud, 12.0), loud0);
        std::printf("      shaper tone: a quiet sound %.1e / %.1e off at +/-100 %%, a loud one %.3f\n", quietUp,
                    quietDown, loudUp);
        check(quietUp < 1e-4 && quietDown < 1e-4 && loudUp > 0.05,
              "the shaper's Tone leaves what its curve leaves straight alone, and changes what it drives");
        auto aliasing = [&](double oversampling) {
            auto high = [c7](size_t t, int) { return 0.9 * std::sin(2.0 * std::numbers::pi * c7 * t / kSampleRate); };
            const std::vector<float> out = run(FxType::Shaper,
                                               {{FxParam::ShaperMode, static_cast<double>(ClipMode::Hard)},
                                                {FxParam::ShaperDrive, 18.0},
                                                {FxParam::ShaperOversampling, oversampling}},
                                               4800 + kFftSize, high)
                                           .left;
            return unexpectedDb(std::vector<float>(out.begin() + 4800, out.end()), harmonics(c7));
        };
        // Its oversampling changed under a quiet sine (which the curve leaves alone), in whole blocks and in single
        // frames: it ducks out and back in rather than clicking, no sample stepping further than the sine's own do.
        auto steepest = [&](uint32_t piece) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Shaper);
            p[paramIndex(0, FxParam::ShaperMode)] = static_cast<double>(ClipMode::Hard);
            p[paramIndex(0, FxParam::ShaperDrive)] = 0.0;
            p[paramIndex(0, FxParam::ShaperLink)] = 0.0;
            chain(p, {0});
            std::vector<float> left(piece), right(piece);
            double most = 0.0, last = 0.0;
            uint32_t t = 0;
            for (double value : {1.0, 2.0, 3.0, 1.0}) {
                p[paramIndex(0, FxParam::ShaperOversampling)] = value;
                for (uint32_t done = 0; done < 12000; done += piece, t += piece) {
                    for (uint32_t s = 0; s < piece; ++s)
                        left[s] = right[s] =
                            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * (t + s) / kSampleRate));
                    fx.process(left.data(), right.data(), piece, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < piece; ++s) {
                        if (t + s > 4800)
                            most = std::max(most, static_cast<double>(std::abs(left[s] - last)));
                        last = left[s];
                    }
                }
            }
            return most / (0.5 * 2.0 * std::numbers::pi * 440.0 / kSampleRate);
        };
        const double switchBlocks = steepest(kBlock), switchFrames = steepest(1);
        std::printf("      shaper oversampling 2x, 4x, 8x, 2x under a sine: steepest step x%.3f in whole blocks, x%.3f "
                    "in single frames\n",
                    switchBlocks, switchFrames);
        check(switchBlocks < 1.15 && switchFrames < 1.15,
              "the shaper's oversampling changed under a sine ducks out and back rather than clicking");
        const double at2 = aliasing(1.0), at4 = aliasing(2.0), at8 = aliasing(3.0);
        std::printf("      shaper aliasing on a hard-clipped C7: %.1f dB at 2x, %.1f at 4x, %.1f at 8x\n", at2, at4, at8);
        check(at4 < at2 - 6.0 && at8 < at4 - 3.0, "the shaper aliases less at 4x and 8x than at 2x");
    }

    // The compressor's detector high-pass: a loud 50 Hz sine under a quiet 2 kHz one pulls the 2 kHz down less with
    // it at 300 Hz.
    {
        auto bassy = [](size_t t, int) {
            return 0.7 * std::sin(2.0 * std::numbers::pi * 50.0 * t / kSampleRate) +
                   0.05 * std::sin(2.0 * std::numbers::pi * 2000.0 * t / kSampleRate);
        };
        auto treble = [&](double hp) {
            const std::vector<float> out =
                run(FxType::Compressor,
                    {{FxParam::CompThreshold, -24.0}, {FxParam::CompRatio, 8.0}, {FxParam::CompDetectorHp, hp}},
                    9600 + kFftSize, bassy)
                    .left;
            return 10.0 * std::log10(bandEnergy(out, 9600, 1900.0, 2100.0));
        };
        const double off = treble(0.0), on = treble(normFromCutoff(300.0));
        std::printf("      compressor detector HP at 300 Hz: a quiet 2 kHz under loud bass %.1f dB louder than without\n",
                    on - off);
        check(on - off > 6.0, "the compressor's detector high-pass keeps the bass from pulling the rest down");
    }

    // The flanger through zero, held still at its sweep's middle, negative and half wet: the swept copy as late as
    // the dry sound, so noise cancels to nothing; without it, the comb leaves most of it.
    {
        auto flanged = [&](double throughZero) {
            const std::vector<float> out = run(FxType::Flanger,
                                               {{FxParam::FlangerRate, 0.0},
                                                {FxParam::FlangerFeedback, 0.0},
                                                {FxParam::FlangerNegative, 1.0},
                                                {FxParam::FlangerStereo, 0.0},
                                                {FxParam::FlangerThroughZero, throughZero}},
                                               24000, noise)
                                           .left;
            double power = 0.0, in = 0.0;
            for (size_t t = 9600; t < 24000; ++t) {
                power += out[t] * out[t];
                in += noise(t, 0) * noise(t, 0);
            }
            return 10.0 * std::log10(power / in + 1e-30);
        };
        const double zero = flanged(1.0), plain = flanged(0.0);
        std::printf("      flanger at its sweep's middle, negative: %.1f dB through zero, %.1f dB without\n", zero, plain);
        check(zero < -60.0 && plain > -10.0, "the flanger through zero cancels everything where its sweep meets the dry sound");
    }

    // The phaser's Spread moves its notches: held still, four stages, its deepest notch between 100 Hz and 10 kHz
    // more than a sixth of an octave (four steps of the 1/24-octave grid it is looked for on) from where it is
    // unspread.
    {
        auto deepest = [&](double spread) {
            double lowest = 1e9, at = 0.0;
            for (double hz = 100.0; hz < 10000.0; hz *= std::exp2(1.0 / 24.0)) {
                auto sine = [hz](size_t t, int) { return 0.5 * std::sin(2.0 * std::numbers::pi * hz * t / kSampleRate); };
                const std::vector<float> out =
                    run(FxType::Phaser, {{FxParam::PhaserRate, 0.0}, {FxParam::PhaserFeedback, 0.0},
                                         {FxParam::PhaserSpread, spread}},
                        4800, sine)
                        .left;
                double peak = 0.0;
                for (size_t t = 2400; t < 4800; ++t)
                    peak = std::max(peak, static_cast<double>(std::abs(out[t])));
                if (peak < lowest) {
                    lowest = peak;
                    at = hz;
                }
            }
            return at;
        };
        const double plain = deepest(0.0), spread = deepest(1.0);
        std::printf("      phaser's deepest notch: %.0f Hz unspread, %.0f Hz spread\n", plain, spread);
        check(std::abs(std::log2(spread / plain)) > 1.0 / 6.0, "the phaser's Spread moves its notches");
    }

    // Lo-Fi: at 4 bits and the top rate, unsmoothed, every sample on one of 16 levels 1/8 apart, half a step off 0,
    // and at 1 bit on one of two, ±0.5; at 1 kHz, a new sample about 1000 times a second; Smooth taking the steps'
    // edges down; Mix at 0 the dry sound exactly.
    {
        auto ramp = [](size_t t, int) { return std::sin(2.0 * std::numbers::pi * 3.0 * t / kSampleRate) * 0.9; };
        auto levels = [&](double bits) {
            return run(FxType::LoFi, {{FxParam::LoFiBits, bits}, {FxParam::LoFiRate, 1.0}, {FxParam::LoFiSmooth, 0.0}},
                       24000, ramp)
                .left;
        };
        const std::vector<float> coarse = levels(4.0), one = levels(1.0);
        double offGrid = 0.0;
        std::set<float> found, foundOne;
        for (size_t t = 4800; t < coarse.size(); ++t) {
            offGrid = std::max(offGrid, std::abs(coarse[t] * 8.0 - 0.5 - std::round(coarse[t] * 8.0 - 0.5)));
            found.insert(coarse[t]);
            foundOne.insert(one[t]);
        }
        const bool twoLevels = foundOne == std::set<float>{-0.5f, 0.5f};
        auto held = [&](double smooth) {
            return run(FxType::LoFi,
                       {{FxParam::LoFiBits, 16.0}, {FxParam::LoFiRate, normFromCutoff(1000.0)}, {FxParam::LoFiSmooth, smooth}},
                       48000 + 4800, noise)
                .left;
        };
        const std::vector<float> raw = held(0.0), smoothed = held(1.0);
        int changes = 0;
        double rawStep = 0.0, smoothStep = 0.0;
        for (size_t t = 4801; t < 4800 + 48000; ++t) {
            changes += raw[t] != raw[t - 1];
            rawStep = std::max(rawStep, static_cast<double>(std::abs(raw[t] - raw[t - 1])));
            smoothStep = std::max(smoothStep, static_cast<double>(std::abs(smoothed[t] - smoothed[t - 1])));
        }
        const std::vector<float> dry = run(FxType::LoFi, {{FxParam::LoFiMix, 0.0}}, 9600, noise).left;
        bool same = true;
        for (size_t t = 0; t < dry.size(); ++t)
            same = same && dry[t] == static_cast<float>(noise(t, 0));
        std::printf("      lo-fi: 4 bits %.1e off its levels, %zu levels; 1 bit %zu levels; at 1 kHz %d new samples a "
                    "second, its steps %.3f, smoothed %.3f; Mix 0 %s\n",
                    offGrid, found.size(), foundOne.size(), changes, rawStep, smoothStep, same ? "dry" : "not dry");
        check(offGrid < 1e-6 && found.size() <= 16 && twoLevels && std::abs(changes - 1000) <= 2 &&
                  smoothStep < 0.25 * rawStep && same,
              "the Lo-Fi holds Bits' levels at Rate, Smooth takes the steps' edges off, and Mix at 0 is dry");
    }
}

// A tuning other than 12-TET, as MTS-ESP hands the synth one (see Synth::setTuning()): a stand-in that retunes each
// key by its own amount, leaves keys unmapped, and keeps the channel it was last asked with, and whether it was asked
// both with a channel and with none (which MTS-ESP's client takes amiss: see Synth::tuningChannel()).
struct TestTuning : Tuning {
    std::array<double, 128> by{};
    std::array<bool, 128> gone{};
    int channel = -2; // the channel asked last
    bool withChannel = false, withNone = false;
    bool mixed() const { return withChannel && withNone; }
    void asked(int ch) {
        channel = ch;
        (ch < 0 ? withNone : withChannel) = true;
    }
    double retune(int key, int ch) override {
        asked(ch);
        return by[key];
    }
    bool unmapped(int key, int ch) override {
        asked(ch);
        return gone[key];
    }
    bool master = false; // a master's, which plays over the patch's own tuning; else SysEx's, under it
    bool overrides() override { return master; }
};

// A tuning file's text loaded into `p` as the editor loads it (see tuningedits.h), true if it read.
bool loadTuning(ParamValues &p, std::string_view text, TuningFile file) {
    Str why = Str::TuningNotScale;
    if (file == TuningFile::Mapping) {
        TuningMapping mapping;
        if (!parseKbm(text, mapping, why))
            return false;
        for (const auto &[index, value] : mappingChanges(&mapping))
            p[index] = value;
        return true;
    }
    TuningScale scale;
    if (!(file == TuningFile::AnaMark ? parseTun(text, scale, why)
                                      : parseScl(text, file == TuningFile::AbletonScala, scale, why)))
        return false;
    for (const auto &[index, value] : scaleChanges(&scale))
        p[index] = value;
    return true;
}

void tuningChecks() {
    std::printf("tuning:\n");
    // The rising zero crossings of what plays, and its pitch between two times (samples), the default patch a sine.
    auto hzBetween = [](const std::vector<float> &x, double from, double to) {
        std::vector<double> at;
        for (size_t i = 1; i < x.size(); ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
                const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
                if (t >= from && t <= to)
                    at.push_back(t);
            }
        return at.size() < 2 ? 0.0 : (at.size() - 1) * kSampleRate / (at.back() - at.front());
    };
    auto semitones = [](double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }; // as a key, tuning and all
    const double second = kSampleRate;

    // An A4 a quarter tone up plays a quarter tone up; without the tuning, at 440 Hz.
    {
        TestTuning tuning;
        tuning.by[69] = 0.5;
        Engine e(2);
        e.synth().setTuning(&tuning, true);
        e.noteOn(69);
        const double tuned = semitones(hzBetween(e.render(24000), 4800.0, 24000.0));
        Engine plain(2);
        plain.noteOn(69);
        const double untuned = semitones(hzBetween(plain.render(24000), 4800.0, 24000.0));
        std::printf("      an A4 tuned a quarter tone up plays key %.3f, untuned %.3f\n", tuned, untuned);
        check(std::abs(tuned - 69.5) < 0.01 && std::abs(untuned - 69.0) < 0.01,
              "a note plays as far off 12-TET as its tuning says");
    }
    // The tuning changed under a held note: retuning continuously, the note follows it from the next block; on note
    // on, it keeps the tuning it started in, until the tuning is taken away (MTS-ESP switched off), back to 12-TET.
    for (bool continuous : {true, false}) {
        TestTuning tuning;
        Engine e(2);
        e.synth().setTuning(&tuning, continuous);
        e.noteOn(69);
        const std::vector<float> before = e.render(12000);
        tuning.by[69] = 1.0;
        const std::vector<float> after = e.render(12000);
        e.synth().setTuning(nullptr, continuous);
        const std::vector<float> gone = e.render(12000);
        const double a = semitones(hzBetween(before, 2400.0, 12000.0)), b = semitones(hzBetween(after, 512.0, 12000.0)),
                     c = semitones(hzBetween(gone, 512.0, 12000.0));
        std::printf("      %s: key %.3f, retuned a semitone up %.3f, the tuning gone %.3f\n",
                    continuous ? "continuous" : "note on", a, b, c);
        check(std::abs(a - 69.0) < 0.01 && std::abs(b - (continuous ? 70.0 : 69.0)) < 0.01 && std::abs(c - 69.0) < 0.01,
              continuous ? "retuning continuously, a held note follows its tuning as it changes"
                         : "retuning on note on, a held note keeps the tuning it started in");
    }
    // A patch's tuning taken up before the render that would see it change (as the plugins do as they take a block's
    // params, see Synth::retunePatch()): a note at the block's start, retuning on note on, plays in it from its start,
    // an A4 in 24-EDO 9 quarter tones over C4, key 64.5; one before the change is heard plays in 12-TET.
    {
        Engine e(2), late(2);
        for (Engine *engine : {&e, &late}) {
            engine->synth().setTuning(nullptr, false);
            engine->render(256); // its params heard once, 12-TET
            check(loadTuning(engine->p, "! 24.scl\n24-EDO\n24\n!\n" + [] {
                std::string degrees;
                for (int d = 1; d <= 24; ++d)
                    degrees += std::to_string(50 * d) + ".0\n";
                return degrees;
            }(), TuningFile::Scala), "a 24-EDO scale reads");
        }
        e.synth().retunePatch(e.p);
        e.noteOn(69);
        late.noteOn(69);
        const double at = semitones(hzBetween(e.render(12000), 2400.0, 12000.0));
        const double before = semitones(hzBetween(late.render(12000), 2400.0, 12000.0));
        std::printf("      a note as the patch's tuning comes: key %.3f, %.3f before it is taken up\n", at, before);
        check(std::abs(at - 64.5) < 0.01 && std::abs(before - 69.0) < 0.01,
              "a patch's tuning taken up with the block's params tunes a note at the block's start");
    }
    // Mono legato with a glide, from an A4 a quarter tone up into a B4 a quarter tone down: from 69.5 to 70.5.
    {
        TestTuning tuning;
        tuning.by[69] = 0.5;
        tuning.by[71] = -0.5;
        Engine e(2);
        e.synth().setTuning(&tuning, true);
        e.synth().setPlayMode(true, true, false);
        e.p[paramIndex(GlobalParam::Glide)] = normFromTime(0.3);
        e.noteOn(69);
        e.render(12000);
        e.noteOn(71);
        const std::vector<float> x = e.render(static_cast<size_t>(second));
        const double start = semitones(hzBetween(x, 0.0, 400.0)), end = semitones(hzBetween(x, 0.6 * second, second));
        std::printf("      gliding: from key %.3f to %.3f\n", start, end);
        check(std::abs(start - 69.5) < 0.1 && std::abs(end - 70.5) < 0.01,
              "a glide goes from one tuned note to the next");
        // And a change of tuning during a glide moves the note at once, the glide going on from there.
        tuning.by[73] = 0.25;
        e.noteOn(73);
        e.render(2400);
        tuning.by[73] = 1.25;
        const std::vector<float> on = e.render(static_cast<size_t>(second));
        const double settled = semitones(hzBetween(on, 0.6 * second, second));
        std::printf("      retuned a semitone up mid-glide: settling on key %.3f\n", settled);
        check(std::abs(settled - 74.25) < 0.01, "a glide carries on to the note as it is retuned meanwhile");
    }
    // An unmapped key plays nothing and its end is reported at once; in mono it doesn't take the voice over. The
    // channel the tuning is asked with is the note's, but none with MPE on.
    {
        TestTuning tuning;
        tuning.gone[61] = true;
        Engine e(2);
        e.synth().setTuning(&tuning, true);
        e.synth().noteOn({7, 0, 3, 61}, 1.0);
        const bool reported = e.synth().endedNotes().size() == 1 && e.synth().endedNotes()[0].noteId == 7;
        const bool channel = tuning.channel == 3;
        e.synth().clearEndedNotes();
        const std::vector<float> x = e.render(4800);
        float peak = 0.0f;
        for (float v : x)
            peak = std::max(peak, std::abs(v));
        e.synth().noteOff({7, 0, 3, 61});
        e.render(256);
        const bool once = e.synth().endedNotes().empty();
        // With MPE off, the editor's own note and a host's on no channel ask on the first channel, beside a note on
        // the fourth held meanwhile: never with none.
        e.synth().noteOn({9, 0, 3, 69}, 1.0);
        e.synth().noteOn({-1, kAuditionPort, 0, 64}, 1.0);
        const bool audition = tuning.channel == 0;
        e.synth().noteOn({10, 0, -1, 67}, 1.0);
        const bool none = tuning.channel == 0;
        e.render(512);
        const bool unmixed = !tuning.mixed();
        e.synth().setMpe(true, 48.0);
        e.synth().noteOn({8, 0, 3, 69}, 1.0);
        const bool mpe = tuning.channel == -1;
        std::printf("      an unmapped key: peak %.6f, its end reported %s\n", peak, reported && once ? "once" : "wrongly");
        check(reported && once && peak == 0.0f, "a key the tuning leaves unmapped plays nothing, its end reported once");
        check(channel && mpe, "the tuning is asked with the note's channel, and with none while MPE is on");
        check(audition && none && unmixed,
              "with MPE off, the editor's notes and a host's on no channel ask on the first channel, never with none");

        Engine mono(2);
        mono.synth().setTuning(&tuning, true);
        mono.synth().setPlayMode(true, true, false);
        mono.noteOn(69);
        mono.render(4800);
        mono.noteOn(61);
        const double held = semitones(hzBetween(mono.render(12000), 2400.0, 12000.0));
        check(std::abs(held - 69.0) < 0.01, "in mono, an unmapped key leaves the note playing as it was");
    }
    // In Key, the sequencer still takes an unmapped key: C#4 starts the pattern a semitone up, its A4 playing as the
    // tuning has the A#4, asked on the first channel beside the key's on the third, never with none.
    {
        constexpr double kStep = kSampleRate * 60.0 / 120.0 / 4.0;
        TestTuning tuning;
        tuning.gone[61] = true;
        tuning.by[70] = 0.5;
        Engine e(1);
        e.synth().setTuning(&tuning, true);
        e.p[paramIndex(SeqParam::Enabled)] = 1.0;
        e.p[paramIndex(SeqParam::Play)] = static_cast<double>(SeqPlay::Key);
        e.p[seqNoteIndex(0, 0, SeqNoteParam::Start)] = 0.0;
        e.p[seqNoteIndex(0, 0, SeqNoteParam::Length)] = 8.0;
        e.p[seqNoteIndex(0, 0, SeqNoteParam::Pitch)] = 69;
        e.synth().setTempo(120.0);
        e.render(256);
        e.synth().noteOn({1, 3, 2, 61}, 1.0);
        const std::vector<float> x = e.render(static_cast<size_t>(4.0 * kStep));
        const double played = semitones(hzBetween(x, 0.5 * kStep, 3.5 * kStep));
        std::printf("      the sequencer from an unmapped key: key %.3f, asked on channel %d\n", played, tuning.channel);
        check(std::abs(played - 70.5) < 0.01 && tuning.channel == 0 && !tuning.mixed(),
              "in Key, the sequencer still takes an unmapped key, and its notes play in the tuning");
    }

    // The patch's own tuning, from tuning files (see tuningfile.h).
    // A Scala scale: comments, cents (words after them ignored), a ratio and a whole number; too many notes and text
    // that isn't one refused, saying why.
    {
        TuningScale scale;
        Str why = Str::MenuSettings;
        const bool read = parseScl("! a test\nThree steps\n 3\n! between\n 100.0 a hundred\n3/2\n2\n", false, scale, why);
        const bool cents = read && scale.cents.size() == 3 && scale.cents[0] == 100.0 &&
                           std::abs(scale.cents[1] - 701.955) < 1e-3 && std::abs(scale.cents[2] - 1200.0) < 1e-9 &&
                           scale.description == "Three steps";
        std::string many = "Too many\n129\n";
        for (int k = 1; k <= 129; ++k)
            many += std::to_string(k * 10) + ".0\n";
        Str tooMany = Str::MenuSettings, notScale = Str::MenuSettings, short_ = Str::MenuSettings;
        const bool refused = !parseScl(many, false, scale, tooMany) && tooMany == Str::TuningTooManyNotes &&
                             !parseScl("Words\nno number\n", false, scale, notScale) && notScale == Str::TuningNotScale &&
                             !parseScl("Short\n3\n100.0\n", false, scale, short_) && short_ == Str::TuningNotScale;
        check(cents && refused, "a Scala scale reads its cents, ratios and whole numbers, and refuses too many notes or "
                                "text that isn't one");
    }
    // The table (to a millionth of a semitone: the scales are written to a millionth of a cent): a 12-TET scale plays as 12-TET; 19-EDO from middle C, each key a 19th of an octave on; a keyboard
    // mapping's reference key at its frequency and its unmapped keys; an AnaMark tuning's keys at their cents; and
    // Ableton's reference pitch.
    {
        auto edo = [](int n) {
            std::string text = std::to_string(n) + "-EDO\n" + std::to_string(n) + "\n";
            for (int k = 1; k <= n; ++k)
                text += std::to_string(1200.0 * k / n) + "\n";
            return text;
        };
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        TuningTable table;
        tuningTable(p, table);
        const bool off = !table.on;
        loadTuning(p, edo(12), TuningFile::Scala);
        tuningTable(p, table);
        double most = 0.0;
        for (double r : table.retune)
            most = std::max(most, std::abs(r));
        const bool twelve = table.on && most < 1e-9;
        loadTuning(p, edo(19), TuningFile::Scala);
        tuningTable(p, table);
        const bool nineteen = std::abs(table.retune[60]) < 1e-9 && std::abs(table.retune[61] - (12.0 / 19.0 - 1.0)) < 1e-6 &&
                              std::abs(table.retune[79] - (12.0 - 19.0)) < 1e-6 && std::abs(table.retune[41] - (7.0)) < 1e-6;
        std::printf("      19-EDO: C#4 %.4f, the octave up %.4f semitones off 12-TET\n", table.retune[61], table.retune[79]);
        // A mapping over 19-EDO: A4 (its reference) at 440 Hz, D#4 unmapped, and the keys round its 12 entries playing
        // degrees 0 to 11 an octave (19 degrees) apart.
        const bool mapped =
            loadTuning(p, "! test\n12\n0\n127\n60\n69\n440.0\n19\n0\n1\n2\nx\n4\n5\n6\n7\n8\n9\n10\n11\n", TuningFile::Mapping);
        tuningTable(p, table);
        const double degree9 = 1200.0 * 9 / 19.0, degree1 = 1200.0 * 1 / 19.0;
        const bool mapping = mapped && std::abs(table.retune[69]) < 1e-9 && table.unmapped[63] && !table.unmapped[64] &&
                             std::abs((69 + table.retune[69]) - (61 + table.retune[61]) - (degree9 - degree1) / 100.0) < 1e-6 &&
                             std::abs((72 + table.retune[72]) - (60 + table.retune[60]) - 12.0) < 1e-6;
        Str why = Str::MenuSettings;
        TuningMapping shortMap;
        const bool shortRefused = !parseKbm("12\n0\n127\n60\n69\n440\n12\n0\n1\n", shortMap, why) &&
                                  why == Str::TuningNotMapping;
        for (const auto &[index, value] : mappingChanges(nullptr))
            p[index] = value;
        // AnaMark: [Exact Tuning] over [Tuning], A4 a quarter tone up, at the default base frequency.
        const bool anaMark = loadTuning(p, "; a test\n[Tuning]\nnote 69 = 6900\n[Exact Tuning]\nnote 69 = 6950.0\n",
                                        TuningFile::AnaMark);
        tuningTable(p, table);
        const bool tun = anaMark && std::abs(table.retune[69] - 0.5) < 1e-9 && std::abs(table.retune[60]) < 1e-9;
        // Ableton: 12-TET with A4 at 432 Hz.
        const bool ableton = loadTuning(p, "! @ABL REFERENCE_PITCH 4 9 432.0\n12-TET\n12\n100.0\n200.0\n300.0\n400.0\n"
                                           "500.0\n600.0\n700.0\n800.0\n900.0\n1000.0\n1100.0\n2/1\n",
                                        TuningFile::AbletonScala);
        tuningTable(p, table);
        const double at432 = 12.0 * std::log2(432.0 / 440.0);
        const bool reference = ableton && std::abs(table.retune[69] - at432) < 1e-9 && std::abs(table.retune[60] - at432) < 1e-9;
        std::printf("      Ableton's A4 at 432 Hz: %.4f semitones off\n", table.retune[69]);
        check(off && twelve && nineteen, "a tuning file's table plays 12-TET as 12-TET and 19-EDO from middle C");
        check(mapping && shortRefused, "a keyboard mapping puts its reference key at its frequency, leaves its x keys "
                                       "unmapped and repeats its entries by its octave, and refuses missing entries");
        check(tun && reference, "an AnaMark tuning plays its keys at their exact cents, and Ableton's reference pitch "
                                "tunes the scale");
    }
    // Through the synth: an A4 the patch's tuning puts a quarter tone up plays there; a master's tuning plays over it
    // and MTS SysEx's under it; and a key a mapping leaves unmapped plays nothing.
    {
        const std::string quarter = "[Exact Tuning]\nnote 69 = 6950.0\n";
        Engine e(2);
        loadTuning(e.p, quarter, TuningFile::AnaMark);
        e.noteOn(69);
        const double patched = semitones(hzBetween(e.render(24000), 4800.0, 24000.0));
        TestTuning sysEx;
        sysEx.by[69] = -0.5;
        Engine under(2);
        loadTuning(under.p, quarter, TuningFile::AnaMark);
        under.synth().setTuning(&sysEx, true);
        under.noteOn(69);
        const double withSysEx = semitones(hzBetween(under.render(24000), 4800.0, 24000.0));
        TestTuning master;
        master.by[69] = -0.5;
        master.master = true;
        Engine over(2);
        loadTuning(over.p, quarter, TuningFile::AnaMark);
        over.synth().setTuning(&master, true);
        over.noteOn(69);
        const double withMaster = semitones(hzBetween(over.render(24000), 4800.0, 24000.0));
        std::printf("      the patch's A4 a quarter tone up: key %.3f; under SysEx's %.3f; under a master's %.3f\n", patched,
                    withSysEx, withMaster);
        check(std::abs(patched - 69.5) < 0.01 && std::abs(withSysEx - 69.5) < 0.01 && std::abs(withMaster - 68.5) < 0.01,
              "the patch's own tuning plays, over MTS SysEx's and under a master's");

        Engine gaps(2);
        loadTuning(gaps.p, "12\n0\n127\n60\n69\n440.0\n12\nx\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n", TuningFile::Mapping);
        gaps.render(256); // the table worked out from the params
        gaps.synth().noteOn({5, 0, 0, 60}, 1.0);
        const bool ended = gaps.synth().endedNotes().size() == 1;
        float peak = 0.0f;
        for (float v : gaps.render(4800))
            peak = std::max(peak, std::abs(v));
        check(ended && peak == 0.0f, "a key the patch's keyboard mapping leaves unmapped plays nothing");
    }
    // A preset keeps the tuning: written sparse and read back, the params and the files' names.
    {
        ParamValues p{};
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        loadTuning(p, "Pentatonic\n5\n200.0\n400.0\n700.0\n900.0\n2/1\n", TuningFile::Scala);
        loadTuning(p, "5\n0\n127\n60\n60\n261.6\n5\n0\n1\nx\n3\n4\n", TuningFile::Mapping);
        gui::UiLayout ui;
        ui.tuningName = "pentatonic";
        ui.mappingName = "Gaps \xC3\xA9";
        std::vector<unsigned char> bytes;
        const clap_ostream_t out = memoryOut(bytes);
        writePatch(&out, p, ui, true);
        MemoryReader reader{bytes.data(), bytes.size(), 0};
        const clap_istream_t in = memoryIn(reader);
        ParamValues loaded;
        const bool read = readParams(&in, loaded);
        const gui::UiLayout back = readLayout(&in);
        check(read && std::equal(loaded.begin(), loaded.end(), p.begin()) && back.tuningName == ui.tuningName &&
                  back.mappingName == ui.mappingName,
              "a preset keeps the patch's tuning and its files' names");
    }
}

int main() {
    const int factors[] = {1, 2, 4, 8};
    tuningChecks();
    dcChecks();
    sequencerChecks();
    savedTempoChecks();
    presetInfoChecks();
    globalLfoChecks();
    keyScaleChecks();
    dx7Checks();
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
        // The ladder saturates in each of its stages as well as through Drive (which feeds it at full level, held
        // under kAnalogDriveCeiling, so its stages take the loudest of it), resonant here and cut off above the note's
        // 4th harmonic: its stages' harmonics, which are what it sounds of, alias some 8 dB more than the driven
        // low-pass's at 2x (2 dB more than when Drive held what it fed it under 1), falling as the oversampling rises.
        {"saw into driven ladder at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Ladder);
             p[paramIndex(0, FilterParam::Slope)] = 3.0;
             p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(6000.0);
             p[paramIndex(0, FilterParam::Resonance)] = 0.7;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -40.0},
        // The diode, saturating at its input, and the Sallen-Key, clipping in its loop, driven and resonant likewise.
        // The diode's one saturation takes its input less all of its feedback, so it clips harder there than the
        // ladder's stages do, and aliases some 2 dB more at 2x (Drive's ceiling sharing the clipping out with it, 2 dB
        // less than when Drive held what it fed it under 1), falling as the oversampling rises.
        {"saw into driven diode at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Diode);
             p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(6000.0);
             p[paramIndex(0, FilterParam::Resonance)] = 0.7;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -35.0},
        // The ladder's high-pass and band-pass saturate in their stages as its low-pass does, Drive held under the
        // same ceiling; the high-pass, cut off under the note, passes all of its harmonics through them. Neither's peak
        // sits on the note's top harmonics as the low-pass's does here: the high-pass aliases some 7 dB less at 2x,
        // and the band-pass, its input taken down as its resonance rises, some 16 dB less.
        {"saw into driven LAD HP at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::LadderHigh);
             p[paramIndex(0, FilterParam::Slope)] = 3.0;
             p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(800.0);
             p[paramIndex(0, FilterParam::Resonance)] = 0.7;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -45.0},
        {"saw into driven LAD BP at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::LadderBand);
             p[paramIndex(0, FilterParam::Slope)] = 3.0;
             p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(3000.0);
             p[paramIndex(0, FilterParam::Resonance)] = 0.7;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -45.0},
        {"saw into driven Sallen-Key at C6", 84,
         [](ParamValues &p) {
             p[paramIndex(0, OpParam::Wave)] = static_cast<double>(OpWave::Saw);
             p[paramIndex(0, OpParam::Out)] = 0.0;
             p[sendIndex(0, 0)] = 1.0;
             p[paramIndex(0, FilterParam::Enabled)] = 1.0;
             p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::SallenKeyLow);
             p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(6000.0);
             p[paramIndex(0, FilterParam::Resonance)] = 0.7;
             p[paramIndex(0, FilterParam::Drive)] = 1.0;
             p[paramIndex(0, FilterParam::Out)] = 0.25;
         },
         -40.0},
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

    syncWarpChecks(factors);
    newerEffectChecks();
    unisonModeChecks();
    driveIntoLadderChecks();
    driveLevelChecks();

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
        // The same on a resonant ladder's.
        auto ladderMod = [&](ParamValues &p) {
            cutoffMod(p);
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Ladder);
            p[paramIndex(0, FilterParam::Resonance)] = 0.5;
        };
        const double ladder = unexpectedDb(steadyTone(1, key, ladderMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on ladder cutoff", ladder);
        check(ladder < -80.0, "LFO on a ladder's cutoff doesn't zipper");
        // And on a formant's vowel, which moves all three band-passes at once (the LFO taken off the cutoff onto it).
        auto vowelMod = [&](ParamValues &p) {
            cutoffMod(p);
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Formant);
            p[paramIndex(0, FilterParam::Cutoff)] = kDefaultCutoff;
            p[paramIndex(0, FilterParam::Vowel)] = 0.5;
            lfoOn(p, paramIndex(0, FilterParam::Vowel), 0.2, lfo);
        };
        const double vowel = unexpectedDb(steadyTone(1, key, vowelMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on formant vowel", vowel);
        check(vowel < -80.0, "LFO on a formant's vowel doesn't zipper");
        // And on a morph's place, which shares out the stage's outputs anew.
        auto morphMod = [&](ParamValues &p) {
            cutoffMod(p);
            p[paramIndex(0, FilterParam::Type)] = static_cast<double>(FilterType::Morph);
            p[paramIndex(0, FilterParam::Cutoff)] = normFromCutoff(440.0);
            p[paramIndex(0, FilterParam::Morph)] = 0.5;
            lfoOn(p, paramIndex(0, FilterParam::Morph), 0.3, lfo);
        };
        const double morph = unexpectedDb(steadyTone(1, key, morphMod), partials);
        std::printf("      %-30s %7.1f\n", "LFO on filter morph", morph);
        check(morph < -80.0, "LFO on a morph's place doesn't zipper");

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
    // --- The same for the effects' frequencies and the voice filters', which they take once per control block: an LFO on each, the
    // effect alone in the chain, puts out a sine's sidebands from its movement alone (all of them the sine's frequency
    // and the LFO's multiples), and nothing around the control rate's multiples, where steps would put them. At 7 Hz,
    // the LFO's sidebands keep clear of those.
    std::printf("zipper in the effects (energy off the sine's sidebands, dB; held still in brackets):\n");
    {
        const int key = 57;
        const double f0 = keyHz(key), lfo = 7.0;
        std::vector<double> sidebands;
        for (int k = -60; k <= 60; ++k)
            sidebands.push_back(f0 + k * lfo);
        struct Swept {
            const char *name;
            FxType type;
            FxParam param;
            double amount;
            std::vector<std::pair<FxParam, double>> set;
        };
        const Swept swept[] = {
            {"phase disperser Freq", FxType::Disperser, FxParam::DisperserFreq, 0.05,
             {{FxParam::DisperserFreq, normFromCutoff(f0)},
              {FxParam::DisperserStages, 16.0},
              {FxParam::DisperserPinch, 0.3},
              {FxParam::DisperserMix, 1.0}}},
            {"filter effect cutoff", FxType::Filter, FxParam::FilterCutoff, 0.1,
             {{FxParam::FilterType, static_cast<double>(FilterType::LowPass)},
              {FxParam::FilterCutoff, normFromCutoff(2.0 * f0)},
              {FxParam::FilterResonance, 0.5}}},
            {"filter effect formant vowel", FxType::Filter, FxParam::FilterVowel, 0.1,
             {{FxParam::FilterType, static_cast<double>(FilterType::Formant)}, {FxParam::FilterVowel, 0.375}}},
            {"filter effect morph place", FxType::Filter, FxParam::FilterMorph, 0.2,
             {{FxParam::FilterType, static_cast<double>(FilterType::Morph)},
              {FxParam::FilterCutoff, normFromCutoff(2.0 * f0)},
              {FxParam::FilterResonance, 0.3},
              {FxParam::FilterMorph, 0.25}}},
            {"param EQ band frequency", FxType::ParamEq, FxParam::PeqFreq1, 0.1,
             {{FxParam::PeqFreq1, normFromCutoff(2.0 * f0)}, {FxParam::PeqGain1, 12.0}, {FxParam::PeqQ1, 0.5}}},
            {"phaser center", FxType::Phaser, FxParam::PhaserCenter, 0.1,
             {{FxParam::PhaserRate, 0.0}, {FxParam::PhaserCenter, normFromCutoff(2.0 * f0)}}},
            {"widener cutoff", FxType::Widener, FxParam::WidenerHighPass, 0.2,
             {{FxParam::WidenerHighPass, normFromCutoff(f0)}, {FxParam::WidenerWidth, 1.0}}},
        };
        // The voice filters the same way: an LFO on a resonant low-pass's and ladder's cutoff, a formant's vowel and a
        // morph's place, the operator going out through the filter alone.
        // The ladder quietly, so its saturation's harmonics stay under what is measured. And the low-pass again with an
        // operator, silent, on its Resonance, which has the filter's settings worked out every sample (see
        // Synth::audioSample()): still along the cutoff's ramp.
        struct VoiceSwept {
            const char *name;
            FilterType type;
            FilterParam param;
            double amount, resonance, send;
            bool opOnResonance = false;
        };
        const VoiceSwept voiceSwept[] = {
            {"voice filter cutoff", FilterType::LowPass, FilterParam::Cutoff, 0.1, 0.5, 1.0},
            {"voice ladder cutoff", FilterType::Ladder, FilterParam::Cutoff, 0.1, 0.5, 0.01},
            {"voice formant vowel", FilterType::Formant, FilterParam::Vowel, 0.1, 0.0, 1.0},
            {"voice morph place", FilterType::Morph, FilterParam::Morph, 0.2, 0.3, 1.0},
            {"voice cutoff, op on Resonance", FilterType::LowPass, FilterParam::Cutoff, 0.1, 0.5, 1.0, true},
        };
        for (const VoiceSwept &sweep : voiceSwept) {
            auto setup = [&](ParamValues &p) {
                if (sweep.opOnResonance) { // B, faded out at once, on it
                    p[paramIndex(1, OpParam::Enabled)] = 1.0;
                    p[paramIndex(1, OpParam::Out)] = 0.0;
                    p[paramIndex(1, OpParam::Decay)] = normFromTime(0.001);
                    p[paramIndex(1, OpParam::Sustain)] = 0.0;
                    p[slotTargetIndex(kOpSource + 1, 0)] = slotTargetValue(paramIndex(0, FilterParam::Resonance));
                    p[slotAmountIndex(kOpSource + 1, 0)] = 0.1;
                }
                p[paramIndex(0, OpParam::Out)] = 0.0;
                p[sendIndex(0, 0)] = sweep.send;
                p[paramIndex(0, FilterParam::Enabled)] = 1.0;
                p[paramIndex(0, FilterParam::Out)] = 1.0;
                p[paramIndex(0, FilterParam::Type)] = static_cast<double>(sweep.type);
                p[paramIndex(0, FilterParam::Cutoff)] =
                    sweep.type == FilterType::Formant ? kDefaultCutoff : normFromCutoff(2.0 * f0);
                p[paramIndex(0, FilterParam::KeyTrack)] = 0.0;
                p[paramIndex(0, FilterParam::Resonance)] = sweep.resonance;
                p[paramIndex(0, FilterParam::Vowel)] = 0.375;
                p[paramIndex(0, FilterParam::Morph)] = 0.25;
                lfoOn(p, paramIndex(0, sweep.param), sweep.amount, lfo);
            };
            auto still = [&](ParamValues &p) {
                setup(p);
                p[slotAmountIndex(0, 0)] = 0.0;
            };
            const double db = unexpectedDb(steadyTone(1, key, setup), sidebands);
            const double held = unexpectedDb(steadyTone(1, key, still), sidebands);
            std::printf("      %-30s %7.1f  (%.1f)\n", sweep.name, db, held);
            char what[128];
            std::snprintf(what, sizeof(what), "an LFO on a %s doesn't zipper", sweep.name);
            check(db < -90.0, what);
        }
        for (const Swept &sweep : swept) {
            auto setup = [&](ParamValues &p) {
                p[paramIndex(0, FxParam::Type)] = static_cast<double>(sweep.type);
                chain(p, {0});
                for (const auto &[field, value] : sweep.set)
                    p[paramIndex(0, field)] = value;
                lfoOn(p, paramIndex(0, sweep.param), sweep.amount, lfo);
            };
            auto still = [&](ParamValues &p) {
                setup(p);
                p[slotAmountIndex(0, 0)] = 0.0;
            };
            const double db = unexpectedDb(steadyTone(1, key, setup), sidebands);
            const double held = unexpectedDb(steadyTone(1, key, still), sidebands);
            std::printf("      %-30s %7.1f  (%.1f)\n", sweep.name, db, held);
            char what[128];
            std::snprintf(what, sizeof(what), "an LFO on the %s doesn't zipper", sweep.name);
            check(db < -90.0, what);
        }
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
            p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Random);
            p[paramIndex(0, SourceParam::RandomSmooth)] = 1.0;
            p[paramIndex(0, SourceParam::RandomChaos)] = 0.5;
        };
        const std::vector<float> out = steadyTone(2, 57, wobble);
        check(std::all_of(out.begin(), out.end(), [](float x) { return std::isfinite(x); }),
              "a smoothed random LFO on an operator's level stays finite");

        // A Random source at 0 Hz (unsynced) stands still: each note takes a value of its own as it starts, at once
        // (its smoothing gliding none), anywhere (in each quarter of the range about as often), and holds it; a note
        // retriggering it in mono takes a new one.
        auto t = std::make_unique<Telemetry>(); // large: the analyzers' spectra are in it
        Engine e(1);
        e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Random);
        e.p[sourceEnabledIndex(0)] = 1.0;
        e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
        e.p[paramIndex(0, SourceParam::Rate)] = 0.0;
        e.p[paramIndex(0, SourceParam::RandomSmooth)] = 1.0;
        auto now = [&] {
            e.synth().telemetry(*t);
            return t->sources[0];
        };
        int notes[4] = {};
        bool held = true, own = true;
        float previous = -1.0f;
        for (int n = 0; n < 400; ++n) {
            e.noteOn(60 + n % 12);
            e.render(kBlock);
            const float first = now();
            e.render(2400);
            held = held && now() == first;
            own = own && first != previous;
            previous = first;
            ++notes[std::clamp(static_cast<int>(first * 4.0f), 0, 3)];
            e.noteOff(60 + n % 12);
            e.render(kBlock);
        }
        std::printf("      notes per quarter at 0 Hz  %d %d %d %d\n", notes[0], notes[1], notes[2], notes[3]);
        check(held && own && std::all_of(std::begin(notes), std::end(notes), [](int n) { return n > 60 && n < 140; }),
              "a Random source at 0 Hz takes a value anywhere for each note, at once, and holds it");
        e.synth().setPlayMode(true, false, false);
        e.noteOn(50);
        e.render(2400);
        const float before = now();
        e.noteOn(52);
        e.render(kBlock);
        const float retriggered = now();
        e.render(2400);
        check(retriggered != before && now() == retriggered,
              "a Random source at 0 Hz takes a new value as a note retriggers it in mono, and holds it");
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

        // A Curve source plays its drawn shape as an LFO with the retired Custom shape did, sample for sample: on an
        // operator's level, free at 3 Hz, its points a lopsided ramp, unipolar.
        auto drawn = [](SourceType type) {
            Engine e(2);
            lfoOn(e.p, paramIndex(0, OpParam::Out), 0.5, 3.0);
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(type);
            e.p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Custom);
            e.p[paramIndex(0, SourceParam::Unipolar)] = 1.0;
            e.p[shapeCountIndex(0)] = 3.0;
            e.p[shapePointIndex(0, 1, PointField::X)] = 0.3;
            e.p[shapePointIndex(0, 1, PointField::Y)] = 0.8;
            e.p[shapePointIndex(0, 1, PointField::Curve)] = 0.4;
            e.p[shapePointIndex(0, 2, PointField::X)] = 0.7;
            e.p[shapePointIndex(0, 2, PointField::Y)] = -0.6;
            e.noteOn(57);
            return e.render(24000);
        };
        check(drawn(SourceType::Curve) == drawn(SourceType::Lfo),
              "a Curve source plays its drawn shape as an LFO with the Custom shape did");
    }

    // --- An anchored LFO stands where the host's transport puts it, as if it had started at its Phase at the song's
    // start: synced, by the beats, else by the seconds, whenever a note starts; a block split into pieces keeps the
    // position moving; stopped, it runs on from where it was; a one-shot isn't anchored. Random is neither anchored nor one-shot: anchored,
    // it runs free, and one-shot, it retriggers, running on past its cycle's end.
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

        // Its slowest note values, below the effects' 4/1D: at beat 40, an 8/1 (32 beats, eight bars) is a quarter into
        // its second cycle, an 8/1T (21 1/3 beats) seven eighths, and a 4/1D (24 beats) two thirds, as it was.
        auto atDivision = [&](double division) {
            return phaseAt(40.0, [division](Engine &e) { e.p[paramIndex(0, SourceParam::Division)] = division; });
        };
        const double eightBars = atDivision(kSlowestDivision), eightTriplet = atDivision(kSlowestDivision + 1.0),
                     sixBars = atDivision(0.0);
        std::printf("      at beat 40: %.4f at 8/1, %.4f at 8/1T, %.4f at 4/1D\n", eightBars, eightTriplet, sixBars);
        check(near(eightBars, 0.25) && near(eightTriplet, 0.875) && near(sixBars, 2.0 / 3.0) &&
                  paramTable()[paramIndex(0, SourceParam::Division)].min == kSlowestDivision &&
                  divisionName(kSlowestDivision) == std::string("8/1") && divisionName(-1) == std::string("8/1T") &&
                  divisionName(0) == std::string("4/1D"),
              "a synced LFO reaches down to 8/1, a cycle every eight bars, its numbers from 4/1D on as they were");

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

        // Random, anchored, runs on the free clock (standing nowhere the transport puts it), and as a one-shot runs
        // on past its cycle's end: after 1.5 cycles from the note, half way through its second.
        auto randomPhase = [&](auto setup) {
            Engine e(1);
            anchored(e);
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Random);
            setup(e);
            e.synth().setTransport(true, 2.25, 2.25 * 0.5);
            e.noteOn(57);
            e.render(36000); // 0.75 s: 1.5 cycles at 2 Hz
            return phase(e);
        };
        const double randomAnchored = randomPhase(none), randomFree = randomPhase([](Engine &e) {
            e.p[paramIndex(0, SourceParam::Anchor)] = 0.0;
            e.p[paramIndex(0, SourceParam::Free)] = 1.0;
        });
        const double randomOneShot = randomPhase([](Engine &e) {
            e.p[paramIndex(0, SourceParam::Anchor)] = 0.0;
            e.p[paramIndex(0, SourceParam::OneShot)] = 1.0;
        });
        std::printf("      random: anchored %.4f, free %.4f; one-shot %.4f\n", randomAnchored, randomFree,
                    randomOneShot);
        check(near(randomAnchored, randomFree) && !near(randomAnchored, 0.25),
              "an anchored Random source runs free, not where the transport puts it");
        check(std::abs(randomOneShot - 0.5) < 0.01, "a one-shot Random source retriggers, running on past its cycle");
    }

    // --- A Curve's loop region and its Sustain. With the loop the whole curve (every Curve's before it had one, and a
    // new one's), it plays as it did, to the bit. Retrigger plays from its Phase, then round its loop alone; Free and
    // Anchor cycle the loop alone; One-Shot plays the whole curve once, whatever its loop. Sustain loops while the key
    // is down (or the pedal holds it); let go, it leaves the loop at once from the level it was at, plays the rest of
    // the curve and stays at its end, its voice running on until then.
    std::printf("Curve loop region and Sustain:\n");
    {
        auto telemetry = std::make_unique<Telemetry>();
        using Points = std::vector<std::array<double, 2>>;
        // A ramp from -1 to 1 at the curve's end (a point there), so its value tells where it is: 2 x - 1.
        const Points ramp{{0.0, -1.0}, {1.0, 1.0}};
        // An envelope's shape: up to 1 at a quarter, down to 0 at the half, up to 0.5 at three quarters, and back down
        // to the first point's -1 at the curve's end.
        const Points env{{0.0, -1.0}, {0.25, 1.0}, {0.5, 0.0}, {0.75, 0.5}};
        auto envAt = [](double x) {
            return x < 0.25 ? -1.0 + 8.0 * x : x < 0.5 ? 1.0 - 4.0 * (x - 0.25) : x < 0.75 ? 2.0 * (x - 0.5)
                                                                                           : 0.5 - 6.0 * (x - 0.75);
        };
        // A Curve in place 0, bipolar, unsynced at `hz`, on FM Amount (which a lone carrier doesn't hear), its shape
        // `points` with straight segments, its loop `start` to `end`.
        auto curveOn = [](Engine &e, double hz, const Points &points, double start = 0.0, double end = 1.0) {
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Curve);
            e.p[sourceEnabledIndex(0)] = 1.0;
            e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
            e.p[paramIndex(0, SourceParam::Rate)] = normFromRate(hz);
            e.p[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(GlobalParam::FmAmount));
            e.p[slotAmountIndex(0, 0)] = 0.25;
            e.p[shapeCountIndex(0)] = static_cast<double>(points.size());
            for (size_t i = 0; i < points.size(); ++i) {
                e.p[shapePointIndex(0, static_cast<int>(i), PointField::X)] = points[i][0];
                e.p[shapePointIndex(0, static_cast<int>(i), PointField::Y)] = points[i][1];
                e.p[shapePointIndex(0, static_cast<int>(i), PointField::Curve)] = 0.0;
            }
            e.p[paramIndex(0, SourceParam::LoopStart)] = start;
            e.p[paramIndex(0, SourceParam::LoopEnd)] = end;
        };
        // What telemetry shows after each control block of `seconds`: the Curve's value (as the block started), its
        // phase (as it ended), whether it has left its loop, what it carries on top of its curve, and the voices.
        struct Run {
            std::vector<double> values, phases, offsets;
            std::vector<bool> left;
            std::vector<int> voices;
            size_t size() const { return values.size(); }
        };
        auto run = [&](Engine &e, double seconds, Run &r) {
            for (long n = std::lround(seconds * kSampleRate / 32.0); n > 0; --n) {
                e.render(32);
                e.synth().telemetry(*telemetry);
                r.values.push_back(telemetry->sources[0]);
                r.phases.push_back(telemetry->lfoPhases[0]);
                r.offsets.push_back(telemetry->lfoOffsets[0]);
                r.left.push_back((telemetry->lfoLeft & 1u) != 0);
                r.voices.push_back(telemetry->voices);
            }
            return r.size();
        };
        const long perSecond = std::lround(kSampleRate / 32.0); // control blocks
        auto within = [](const Run &r, size_t from, size_t to, double low, double high) {
            for (size_t i = from; i < std::min(to, r.size()); ++i)
                if (r.voices[i] > 0 && (r.values[i] < low - 1e-6 || r.values[i] > high + 1e-6))
                    return false;
            return true;
        };
        auto range = [](const Run &r, size_t from, size_t to) {
            std::pair<double, double> lh{1e9, -1e9};
            for (size_t i = from; i < std::min(to, r.size()); ++i)
                lh = {std::min(lh.first, r.values[i]), std::max(lh.second, r.values[i])};
            return lh;
        };
        auto mostStep = [](const Run &r, size_t from, size_t to) { // the largest step from one block to the next
            double most = 0.0;
            for (size_t i = std::max<size_t>(from, 1); i < std::min(to, r.size()); ++i)
                if (r.voices[i] > 0 && r.voices[i - 1] > 0)
                    most = std::max(most, std::abs(r.values[i] - r.values[i - 1]));
            return most;
        };
        auto near = [](double a, double b, double within) { return std::abs(a - b) <= within; };

        // The whole curve as its loop: a retriggered one from its Phase, and a free one's shared clock with no note,
        // moving on a control block at a time as phase - floor(phase) took them, to the bit.
        for (const bool free : {false, true}) {
            Engine e(1);
            curveOn(e, 7.0, ramp);
            e.p[paramIndex(0, SourceParam::Phase)] = 0.3;
            e.p[paramIndex(0, SourceParam::Free)] = free ? 1.0 : 0.0;
            const double hz = sweepRateHz(e.p[paramIndex(0, SourceParam::Rate)]);
            if (!free)
                e.noteOn(57);
            double phase = free ? 0.0 : 0.3;
            bool same = true;
            int blocks = 0;
            for (; blocks < 3000 && same; ++blocks) {
                e.render(32);
                phase += hz * 32u / kSampleRate;
                if (phase >= 1.0)
                    phase -= std::floor(phase);
                e.synth().telemetry(*telemetry);
                same = telemetry->lfoPhases[0] == static_cast<float>(phase);
            }
            std::printf("      %s, its loop the whole curve: %s for %d blocks\n", free ? "free" : "retriggered",
                        same ? "as before" : "moved off", blocks);
            check(same, free ? "a free Curve with its loop the whole curve runs as it did, to the bit"
                             : "a retriggered Curve with its loop the whole curve runs as it did, to the bit");
        }

        // Retrigger, its loop [0.25, 0.75] of the ramp at 2 Hz: from its Phase (0) up through the loop to its end, at
        // 0.375 s, then round the loop alone, -0.5 to 0.5, every quarter of a second (half the curve at its rate).
        {
            Engine e(1);
            curveOn(e, 2.0, ramp, 0.25, 0.75);
            e.noteOn(57);
            Run r;
            run(e, 1.5, r);
            std::vector<size_t> rounds; // where it came round
            for (size_t i = 1; i < r.size(); ++i)
                if (r.values[i] < r.values[i - 1] - 0.5)
                    rounds.push_back(i);
            bool even = rounds.size() >= 3;
            for (size_t k = 1; k < rounds.size(); ++k)
                even = even && near(static_cast<double>(rounds[k] - rounds[k - 1]), perSecond * 0.25, 1.0);
            const auto [low, high] = range(r, rounds.empty() ? 0 : rounds[0], r.size());
            std::printf("      retriggered, looping [0.25, 0.75]: starts at %.3f, comes round %zu times, first at "
                        "%.3f s, then between %.3f and %.3f\n",
                        r.values[0], rounds.size(), rounds.empty() ? 0.0 : rounds[0] / static_cast<double>(perSecond),
                        low, high);
            check(r.values[0] == -1.0 && !rounds.empty() &&
                      near(static_cast<double>(rounds[0]), perSecond * 0.375, 2.0) && even &&
                      within(r, rounds.empty() ? 0 : rounds[0], r.size(), -0.5, 0.5) && low < -0.49 && high > 0.49,
                  "a retriggered Curve plays from its Phase into its loop, then round the loop alone at its speed");
        }

        // Free, the same loop: its shared clock comes round the loop alone once in it (from 0, a quarter of a cycle
        // in), with no note.
        {
            Engine e(1);
            curveOn(e, 2.0, ramp, 0.25, 0.75);
            e.p[paramIndex(0, SourceParam::Free)] = 1.0;
            Run r;
            run(e, 1.5, r);
            double low = 1.0, high = 0.0;
            for (size_t i = static_cast<size_t>(perSecond / 4); i < r.size(); ++i)
                low = std::min(low, r.phases[i]), high = std::max(high, r.phases[i]);
            std::printf("      free, looping [0.25, 0.75]: its clock between %.3f and %.3f\n", low, high);
            check(low >= 0.25 && low < 0.26 && high <= 0.75 && high > 0.74,
                  "a free Curve's shared clock cycles its loop alone");
        }

        // Anchored, the same loop, synced on quarter notes at 120 BPM or unsynced at 2 Hz: where the transport puts it
        // counting the loop's cycles from the song's start, where it stood at its Phase: at beat 2.4 with Phase 50 %,
        // (2.4 + 0.5 - 0.25) / 0.5 = 5.3 loops along, at 0.4; at 7.6, 14.7 along, at 0.6; unsynced 0.55 s in (1.1
        // cycles), at 0.6.
        {
            auto phaseAt = [&](double beats, double phase, bool synced) {
                Engine e(1);
                e.synth().setTempo(120.0);
                curveOn(e, 2.0, ramp, 0.25, 0.75);
                e.p[paramIndex(0, SourceParam::Sync)] = synced ? 1.0 : 0.0;
                e.p[paramIndex(0, SourceParam::Division)] = 13.0; // 1/4
                e.p[paramIndex(0, SourceParam::Anchor)] = 1.0;
                e.p[paramIndex(0, SourceParam::Phase)] = phase;
                e.synth().setTransport(true, beats, beats * 0.5);
                e.noteOn(57);
                e.render(32);
                e.synth().telemetry(*telemetry);
                return static_cast<double>(telemetry->lfoPhases[0]);
            };
            const double offset = phaseAt(2.4, 0.5, true), later = phaseAt(7.6, 0.0, true),
                         unsynced = phaseAt(1.1, 0.0, false);
            std::printf("      anchored, looping [0.25, 0.75]: %.4f at beat 2.4 (Phase 50 %%), %.4f at 7.6, %.4f "
                        "unsynced 0.55 s in\n",
                        offset, later, unsynced);
            check(near(offset, 0.4, 1e-4) && near(later, 0.6, 1e-4) && near(unsynced, 0.6, 1e-4),
                  "an anchored Curve stands on its loop's cycles from the song's start, from its Phase");
        }

        // One-Shot ignores the loop: the whole ramp once, staying at its end, 1.
        {
            Engine e(1);
            curveOn(e, 2.0, ramp, 0.25, 0.75);
            e.p[paramIndex(0, SourceParam::OneShot)] = 1.0;
            e.noteOn(57);
            Run r;
            run(e, 1.0, r);
            const auto [low, high] = range(r, 0, r.size());
            std::printf("      one-shot, its loop [0.25, 0.75]: from %.3f to %.3f, its phase at %.3f\n", low, high,
                        r.phases.back());
            check(low == -1.0 && r.values.back() == 1.0 && r.phases.back() == 1.0f,
                  "a one-shot Curve plays its whole curve once and stays at its end, whatever its loop");
        }

        // Sustain wins over the other switches, and only a Curve has it.
        {
            ParamValues p{};
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            auto get = [&p](int i) { return p[i]; };
            p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Curve);
            p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            p[paramIndex(0, SourceParam::OneShot)] = 1.0;
            p[paramIndex(0, SourceParam::Free)] = 1.0;
            const bool curve = lfoClock(get, 0) == LfoClock::Sustain && !lfoOneShot(get, 0) && !lfoShared(get, 0);
            p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Lfo);
            const bool lfo = lfoClock(get, 0) == LfoClock::OneShot && !lfoSustain(get, 0);
            check(curve && lfo && paramTable()[paramIndex(0, SourceParam::LoopEnd)].def == 1.0 &&
                      paramTable()[paramIndex(0, SourceParam::LoopStart)].def == 0.0 &&
                      !isModTarget(paramIndex(0, SourceParam::LoopStart)) &&
                      !isModTarget(paramIndex(0, SourceParam::LoopEnd)) &&
                      !hostListed(paramIndex(0, SourceParam::CurveSustain)),
                  "a Curve's Sustain wins over its other clock switches, an LFO has none, and its loop (the whole "
                  "curve to begin with) is no modulation target");
        }

        // Sustain, looping [0.25, 0.5] of the envelope at 1 Hz (down from 1 to 0, every quarter of a second): held for
        // 0.6 s, at 0.35 along (0.6) as the key goes up. Let go, it goes on from there at the loop's end, carrying
        // 0.6 on top of the curve, fading by the next point, so it slides from 0.6 to that point's 0.5 by 0.75, then
        // down to the curve's end, -1, where it stays half a second after the key went up; its voice runs on till then.
        {
            Engine e(1);
            curveOn(e, 1.0, env, 0.25, 0.5);
            e.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            e.noteOn(57);
            Run r;
            const size_t up = run(e, 0.6, r);
            e.noteOff(57);
            run(e, 1.0, r);
            const bool looped = within(r, static_cast<size_t>(perSecond / 4) + 1, up, 0.0, 1.0);
            const double step = mostStep(r, up - 4, up + 40);
            const double mid = r.values[up + static_cast<size_t>(perSecond / 8)],
                         onPoint = r.values[up + static_cast<size_t>(perSecond / 4)];
            size_t last = up; // the last block its voice played
            while (last + 1 < r.size() && r.voices[last + 1] > 0)
                ++last;
            const double ended = static_cast<double>(last + 1 - up) / perSecond;
            std::printf("      sustain: looped %s while held, %.3f as the key went up, then %.3f (step at most %.4f), "
                        "%.3f, %.3f on the next point; its voice ended %.3f s after the key, at %.3f\n",
                        looped ? "its loop" : "out of its loop", r.values[up - 1], r.values[up], step, mid, onPoint,
                        ended, r.values[last]);
            check(looped, "a Curve in Sustain loops its loop while the key is down");
            check(near(r.values[up], envAt(0.35), 0.01) && step < 0.006 && near(mid, 0.55, 0.01) &&
                      near(onPoint, 0.5, 0.01) && r.left[up] && near(r.offsets[up], 0.6, 0.01) &&
                      near(r.offsets[up + static_cast<size_t>(perSecond / 4) + 2], 0.0, 1e-6),
                  "a Curve in Sustain let go leaves its loop at once, from the level it was at, on to the next point");
            check(near(ended, 0.5, 0.01) && r.values[last] < -0.95,
                  "a Curve in Sustain let go plays on to its curve's end, its voice running on until then");
        }

        // The sustain pedal holds it in its loop once the key is up; let up, it leaves.
        {
            Engine e(1);
            curveOn(e, 1.0, env, 0.25, 0.5);
            e.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            e.noteOn(57);
            Run r;
            run(e, 0.6, r);
            e.synth().setPedal(0, true);
            e.noteOff(57);
            const size_t from = r.size();
            const size_t pedalUp = run(e, 1.0, r);
            e.synth().setPedal(0, false);
            run(e, 0.6, r);
            const bool held = within(r, from, pedalUp, 0.0, 1.0) && !r.left[pedalUp - 1] && r.voices[pedalUp - 1] == 1;
            std::printf("      sustain under the pedal: %s, then %s\n", held ? "looping" : "not looping",
                        r.left[pedalUp] ? "leaving" : "still looping");
            check(held && r.left[pedalUp] && r.voices.back() == 0,
                  "the sustain pedal holds a Curve in Sustain in its loop, and lets it go as it comes up");
        }

        // Nothing after the loop ([0.5, 1], to the curve's end): let go at 0.6 along (0.2), it glides to the end's -1
        // over 5 ms, rather than jumping there.
        {
            Engine e(1);
            curveOn(e, 1.0, env, 0.5, 1.0);
            e.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            e.noteOn(57);
            Run r;
            const size_t up = run(e, 0.6, r);
            e.noteOff(57);
            run(e, 0.1, r);
            const double step = mostStep(r, up - 2, up + 20);
            size_t at = up; // the first block at the end
            while (at < r.size() && r.voices[at] > 0 && r.values[at] > -1.0 + 1e-6)
                ++at;
            std::printf("      sustain, its loop to the curve's end: %.3f as the key went up, at the end (%.3f) "
                        "%zu blocks later, steps at most %.3f\n",
                        r.values[up], r.values[std::min(at, r.size() - 1)], at - up, step);
            check(near(r.values[up], envAt(0.6), 0.01) && at >= up + 7 && at <= up + 9 && step < 0.2 &&
                      r.values[std::min(at, r.size() - 1)] == -1.0,
                  "a Curve in Sustain with nothing after its loop glides to the curve's end over a few ms, let go");
        }

        // A loop ending on a point where the curve drops straight down ([0.25, 0.5], down from 1 to -0.5 at 0.5): let go,
        // it goes down with the drop over 5 ms, as with nothing after the loop, then follows the curve on from its foot,
        // rather than carrying the level it was at along the rest of the curve.
        {
            Engine e(1);
            const Points drop{{0.0, -1.0}, {0.25, 1.0}, {0.5, 1.0}, {0.5, -0.5}, {0.75, -0.75}};
            curveOn(e, 1.0, drop, 0.25, 0.5);
            e.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            e.noteOn(57);
            Run r;
            const size_t up = run(e, 0.6, r);
            e.noteOff(57);
            run(e, 0.3, r);
            auto get = [&e](int i) { return e.p[i]; };
            double off = 0.0; // how far from its curve, once the drop is over
            for (size_t i = up + 9; i < up + 300; ++i)
                off = std::max(off, std::abs(r.values[i] - shapeValue(get, 0, r.phases[i - 1])));
            const double step = mostStep(r, up, up + 9);
            std::printf("      sustain, its loop ending on a drop: %.3f as the key went up, steps at most %.3f, then "
                        "%.6f off its curve\n",
                        r.values[up], step, off);
            check(near(r.values[up], 1.0, 1e-6) && step < 0.25 && off < 1e-4,
                  "a Curve in Sustain whose loop ends on a drop goes down with it over a few ms, then follows its curve");
        }

        // Mono: a key let go and another struck in its release starts it again from its Phase, looping; legato, a key
        // played over a held one leaves it looping where it is, and it leaves its loop once both are up.
        {
            Engine e(1);
            curveOn(e, 1.0, env, 0.25, 0.5);
            e.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            e.synth().setPlayMode(true, false, false);
            e.noteOn(57);
            Run r;
            run(e, 0.6, r);
            e.noteOff(57);
            run(e, 0.1, r);
            const bool leaving = r.left.back();
            e.noteOn(60);
            const size_t again = run(e, 0.6, r) - static_cast<size_t>(0.6 * perSecond);
            const bool restarted = near(r.values[again], -1.0, 1e-6) && !r.left[again] && r.voices[again] == 1 &&
                                   within(r, again + static_cast<size_t>(perSecond / 4) + 1, r.size(), 0.0, 1.0);

            Engine l(1);
            curveOn(l, 1.0, env, 0.25, 0.5);
            l.p[paramIndex(0, SourceParam::CurveSustain)] = 1.0;
            l.synth().setPlayMode(true, true, false);
            l.noteOn(57);
            Run m;
            const size_t over = run(l, 0.6, m);
            l.noteOn(60);
            run(l, 0.5, m);
            l.noteOff(60); // back to the key still down
            const size_t both = run(l, 0.5, m);
            l.noteOff(57);
            run(l, 0.2, m);
            const bool kept = mostStep(m, over - 4, over + 40) < 0.006 && within(m, over, both, 0.0, 1.0) &&
                              !m.left[both - 1] && m.left.back() && m.voices[over] == 1;
            std::printf("      mono: %s let go, then %s by a new key; legato: %s\n", leaving ? "leaving" : "looping",
                        restarted ? "restarted" : "not restarted", kept ? "looping on, leaving once both are up"
                                                                         : "not as it should");
            check(leaving && restarted, "in mono, a new key in a Curve's release in Sustain starts it looping again");
            check(kept, "in mono legato, a Curve in Sustain loops on through the keys held");
        }

        // A loop of no length (only a host or a state makes one) holds at its place once there; one the other way
        // round plays as its ends in order; one outside 0..1, or not a number, as the whole curve.
        {
            auto values = [&](double start, double end) {
                Engine e(1);
                curveOn(e, 2.0, ramp, start, end);
                e.noteOn(57);
                Run r;
                run(e, 1.0, r);
                return r;
            };
            const Run none = values(0.3, 0.3), reversed = values(0.75, 0.25), inOrder = values(0.25, 0.75),
                      outside = values(-2.0, 5.0), notANumber = values(std::numeric_limits<double>::quiet_NaN(), 1.0),
                      whole = values(0.0, 1.0);
            const size_t settled = static_cast<size_t>(perSecond / 5); // 0.2 s: past 0.3 along at 2 Hz
            const auto [low, high] = range(none, settled, none.size());
            bool finite = true;
            for (const Run *r : {&none, &reversed, &outside, &notANumber})
                for (double v : r->values)
                    finite = finite && std::isfinite(v);
            std::printf("      a loop of no length at 0.3: between %.4f and %.4f once there\n", low, high);
            check(near(low, -0.4, 1e-6) && near(high, -0.4, 1e-6) && none.phases.back() == 0.3f,
                  "a Curve's loop of no length holds at its place");
            check(finite && reversed.values == inOrder.values && outside.values == whole.values &&
                      notANumber.values == whole.values,
                  "a Curve's loop the other way round plays in order, and one outside the curve as the whole curve");
        }
    }

    // --- An operator's drawn envelope (see DrawnEnvelope): a sample at a time along its shape, exactly as the shape
    // is drawn; held, One-Shot stays at its end and Sustain loops its loop; let go, it goes on from the level it was
    // at, no step larger than the shape's own, to its end, where it fades out over 5 ms and stands idle. Then in the
    // synth: an operator's level following it, and its voice ending once it has.
    std::printf("drawn operator envelopes:\n");
    {
        using Points = std::vector<std::array<double, 3>>; // x, y (-1..1), curve
        auto shapeOf = [](const Points &points) {
            std::vector<double> shape(kShapeParamCount, 0.0);
            shape[0] = static_cast<double>(points.size());
            for (size_t i = 0; i < points.size(); ++i)
                for (int f = 0; f < 3; ++f)
                    shape[shapeFieldIndex(0, static_cast<int>(i), static_cast<PointField>(f))] = points[i][f];
            return shape;
        };
        // The level the shape draws at x (a curve: its last point at the end), as shapeBend() bends each segment.
        auto drawn = [](const Points &points, double x) {
            size_t k = 0;
            while (k + 2 < points.size() && points[k + 1][0] <= x)
                ++k;
            const double x0 = points[k][0], x1 = k + 2 == points.size() ? 1.0 : points[k + 1][0];
            const double t = std::clamp((x - x0) / (x1 - x0), 0.0, 1.0);
            const double y = points[k][1] + (points[k + 1][1] - points[k][1]) * shapeBend(t, points[k][2]);
            return 0.5 * (y + 1.0);
        };
        constexpr double kRate = 48000.0;
        auto mostStep = [](const std::vector<double> &levels, size_t from, size_t to) {
            double most = 0.0;
            for (size_t i = std::max<size_t>(from, 1); i < std::min(to, levels.size()); ++i)
                most = std::max(most, std::abs(levels[i] - levels[i - 1]));
            return most;
        };

        // One-Shot, held: each sample where the shape draws it, bends and all, then its end level, held.
        {
            const Points points{{0.0, -1.0, 0.5}, {0.4, 1.0, -0.3}, {1.0, -0.2, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            DrawnEnvelope d;
            d.start();
            d.prepare(shape.data(), 4800.0, 0.0, 1.0, false, 0.0, kRate);
            double off = 0.0;
            for (int n = 1; n <= 4800; ++n)
                off = std::max(off, std::abs(d.next() - drawn(points, n / 4800.0)));
            double held = 0.0;
            for (int n = 0; n < 4800; ++n)
                held = std::max(held, std::abs(d.next() - 0.4));
            std::printf("      one-shot: %.2e off its shape, %.2e off its end held\n", off, held);
            check(off < 1e-9 && held < 1e-12 && !d.idle(),
                  "a drawn envelope plays its shape sample for sample, bends and all, and holds its end while held");
            // Let go there, it fades out over 5 ms (240 samples) and stands idle.
            d.release();
            d.prepare(shape.data(), 4800.0, 0.0, 1.0, false, 0.4, kRate);
            int samples = 0;
            while (!d.idle() && samples < 1000) {
                d.next();
                ++samples;
            }
            std::printf("      let go at its end: idle %d samples later\n", samples);
            check(samples >= 239 && samples <= 241, "a drawn envelope let go at its end fades out over 5 ms");
        }

        // A Slew lags what it puts out behind its shape, a one-pole lag: a shape jumping from nothing to full at its
        // middle, and its Sustain loop wrapping from full back to nothing, step no further than a 5 ms lag lets them
        // (under a 200th of the way a sample), where without one they jump; let go, once its end has faded out, what it
        // still puts out runs down to nothing in a straight line within the slew's time, no steeper than the lag took
        // it, and it stands idle then (rather than waiting for the lag to settle, 9 times as long).
        {
            const Points points{{0.0, -1.0, 0.0}, {0.5, -1.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 1.0, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            auto run = [&](double slew, bool sustain, int samples) {
                DrawnEnvelope d;
                d.start();
                d.prepare(shape.data(), 4800.0, 0.0, 1.0, sustain, 0.0, kRate, slew);
                std::vector<double> levels;
                for (int n = 0; n < samples; ++n)
                    levels.push_back(d.next());
                return levels;
            };
            const double jump = mostStep(run(0.0, false, 4800), 0, 4800);
            const double jumpSlewed = mostStep(run(240.0, false, 4800), 0, 4800);
            const double wrap = mostStep(run(0.0, true, 14400), 4900, 14400);
            const double wrapSlewed = mostStep(run(240.0, true, 14400), 4900, 14400);
            DrawnEnvelope d;
            d.start();
            d.prepare(shape.data(), 4800.0, 0.0, 1.0, false, 0.0, kRate, 240.0);
            for (int n = 0; n < 9600; ++n) // up to full and held there
                d.next();
            d.release();
            d.prepare(shape.data(), 4800.0, 0.0, 1.0, false, d.level(), kRate, 240.0);
            std::vector<double> released{d.level()};
            while (!d.idle() && released.size() < 48000)
                released.push_back(d.next());
            const int samples = static_cast<int>(released.size()) - 1;
            const double releaseStep = mostStep(released, 0, released.size());
            std::printf("      a 5 ms slew: the jump's step %.4f (%.2f without), the loop's wrap %.4f (%.2f without); "
                        "idle %d samples after it is let go, at %.1e, steps at most %.4f\n",
                        jumpSlewed, jump, wrapSlewed, wrap, samples, d.level(), releaseStep);
            check(jump > 0.99 && wrap > 0.99 && jumpSlewed < 1.0 / 200.0 && wrapSlewed < 1.0 / 200.0,
                  "a drawn envelope's Slew smooths its shape's jumps and its loop's wrap");
            check(samples > 240 && samples <= 2 * 240 + 4 && d.level() == 0.0 && releaseStep < 1.0 / 200.0,
                  "a slewed drawn envelope let go runs down to nothing within its slew's time after its fade, smoothly, "
                  "and stands idle then");
        }

        // Rising, as an AHDSR's attack counts for stealing, while short of its shape's highest point the first time:
        // a rise to full at 0.6 of its time, down after, rising until then and not after; an Envelope following it says
        // so, as of the block's start.
        {
            const Points points{{0.0, -1.0, 0.0}, {0.6, 1.0, 0.0}, {1.0, -1.0, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            DrawnEnvelope d;
            Envelope env;
            d.start();
            d.prepare(shape.data(), 1000.0, 0.0, 1.0, false, 0.0, kRate);
            env.follow(d);
            const bool atStart = d.rising() && env.rising();
            for (int n = 0; n < 590; ++n)
                d.next();
            const bool before = d.rising();
            for (int n = 0; n < 20; ++n)
                d.next();
            const bool after = !d.rising();
            d.prepare(shape.data(), 1000.0, 0.0, 1.0, false, d.level(), kRate);
            env.follow(d);
            check(atStart && before && after && !env.rising(),
                  "a drawn envelope counts as rising, as an attack does, only until its shape's highest point");
        }

        // An AHDSR releasing, followed by a drawn envelope already done (an operator switched back to its AHDSR after its
        // note was let go, and on to its drawn one again): the release runs on rather than stopping at once.
        {
            const std::vector<double> shape = shapeOf({{0.0, -1.0, 0.0}, {1.0, -1.0, 0.0}});
            const EnvRates rates = envRates(normFromTime(0.001), 0.0, normFromTime(0.1), 0.8, normFromTime(0.5), 0.0,
                                            0.0, 0.0, kRate);
            Envelope env;
            env.start();
            for (int n = 0; n < 24000; ++n)
                env.next(rates);
            env.release();
            for (int n = 0; n < 48; ++n)
                env.next(rates);
            const double was = env.level();
            DrawnEnvelope d; // never started: done, as one let go and faded out is
            d.prepare(shape.data(), 1000.0, 0.0, 1.0, false, 0.0, kRate);
            env.follow(d);
            const double next = env.next(rates);
            std::printf("      an AHDSR releasing at %.3f, followed by a done drawn envelope: %.3f next\n", was, next);
            check(d.idle() && !env.following() && was > 0.5 && next > 0.9 * was && next < was,
                  "an AHDSR releasing runs on when its operator goes back to a drawn envelope that is done");
        }

        // Sustain, its loop [0.25, 0.5] of a rise to 1 at a quarter and a fall to 0.5 by the half, flat after: held, it
        // rises once, then comes round its loop; let go mid-loop, it goes on from the level it was at, sliding onto the
        // shape by its end, no step past the shape's own, and fades out from there.
        {
            const Points points{{0.0, -1.0, 0.0}, {0.25, 1.0, 0.0}, {0.5, 0.0, 0.0}, {1.0, 0.0, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            DrawnEnvelope d;
            d.start();
            std::vector<double> levels;
            for (int block = 0; block < 1700; ++block) { // blocks of 32, a second's shape
                if (block == 1000) // two thirds round its loop the second time: let go
                    d.release();
                d.prepare(shape.data(), kRate, 0.25, 0.5, true, d.level(), kRate);
                for (int s = 0; s < 32; ++s)
                    levels.push_back(d.next());
            }
            const size_t up = 1000 * 32;
            double low = 1.0, high = 0.0;
            for (size_t i = 12100; i < up; ++i)
                low = std::min(low, levels[i]), high = std::max(high, levels[i]);
            const double own = 2.0 / kRate; // the rise's step, the shape's steepest
            const double step = mostStep(levels, up - 50, up + 20000);
            std::printf("      sustain: looped between %.3f and %.3f, %.3f as it was let go, steps at most %.2e "
                        "(its shape's %.2e), %.3f a quarter later\n",
                        low, high, levels[up - 1], step, own, levels[up + 12000 - 1]);
            check(low >= 0.5 - 1e-9 && high <= 1.0 + 1e-9 && low < 0.51 && high > 0.99,
                  "a drawn envelope in Sustain loops its loop while held");
            check(step <= own + 1e-9 && std::abs(levels[up + 12000 - 1] - (0.5 + 0.5 * (levels[up - 1] - 0.5))) < 1e-3,
                  "a drawn envelope let go leaves its loop from the level it was at, sliding onto its shape by its end");
        }

        // Its loop ending on a point the shape drops from (to 0.25 at once): let go, it goes down with the drop over
        // 5 ms, then plays its shape as drawn.
        {
            const Points points{{0.0, -1.0, 0.0}, {0.25, 1.0, 0.0}, {0.5, 1.0, 0.0}, {0.5, -0.5, 0.0}, {1.0, -1.0, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            DrawnEnvelope d;
            d.start();
            d.prepare(shape.data(), kRate, 0.25, 0.5, true, 0.0, kRate);
            for (int n = 0; n < 18000; ++n)
                d.next();
            d.release();
            d.prepare(shape.data(), kRate, 0.25, 0.5, true, d.level(), kRate);
            std::vector<double> levels;
            for (int n = 0; n < 2000; ++n)
                levels.push_back(d.next());
            double off = 0.0;
            for (int n = 300; n < 2000; ++n) // past the glide: on the shape from 0.5 on
                off = std::max(off, std::abs(levels[n] - drawn(points, 0.5 + (n + 1) / kRate)));
            const double step = mostStep(levels, 0, 300);
            std::printf("      a loop ending on a drop: steps at most %.4f gliding down, then %.2e off the shape\n", step,
                        off);
            check(step < 0.75 / 200.0 && off < 1e-9,
                  "a drawn envelope whose loop ends on a drop goes down with it over 5 ms, then plays its shape");
        }

        // Started again where it is (a mono note retriggering it), it goes on from that level, sliding onto its first
        // segment by that segment's end, rather than jumping back to its start.
        {
            const Points points{{0.0, -1.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, -1.0, 0.0}};
            const std::vector<double> shape = shapeOf(points);
            DrawnEnvelope d;
            d.start();
            d.prepare(shape.data(), kRate, 0.0, 1.0, false, 0.0, kRate);
            std::vector<double> levels;
            for (int n = 0; n < 24000; ++n)
                levels.push_back(d.next());
            d.start();
            d.prepare(shape.data(), kRate, 0.0, 1.0, false, d.level(), kRate);
            for (int n = 0; n < 24000; ++n)
                levels.push_back(d.next());
            const double step = mostStep(levels, 23990, 24010);
            std::printf("      started again at %.3f: steps at most %.2e there\n", levels[23999], step);
            check(step <= 2.0 / kRate + 1e-9 && std::abs(levels.back() - 1.0) < 1e-3,
                  "a drawn envelope started again goes on from the level it was at");
        }

        // In the synth: operator A's drawn envelope, One-Shot, a second long, a rise to full over its first quarter and
        // half back down by its end. Its level (as telemetry has it, after each block) follows the shape; held at the
        // end, it stays; let go, its voice ends about 5 ms later.
        auto telemetry = std::make_unique<Telemetry>();
        auto drawnOn = [&](Engine &e, const Points &points, bool sustain, double loopStart, double loopEnd) {
            e.p[paramIndex(0, OpParam::EnvMode)] = 1.0;
            e.p[paramIndex(0, OpParam::EnvSustain)] = sustain ? 1.0 : 0.0;
            e.p[paramIndex(0, OpParam::EnvLength)] = normFromTime(1.0);
            e.p[paramIndex(0, OpParam::EnvLoopStart)] = loopStart;
            e.p[paramIndex(0, OpParam::EnvLoopEnd)] = loopEnd;
            const int base = opEnvShapeIndex(0);
            e.p[base] = static_cast<double>(points.size());
            for (size_t i = 0; i < points.size(); ++i)
                for (int f = 0; f < 3; ++f)
                    e.p[shapeFieldIndex(base, static_cast<int>(i), static_cast<PointField>(f))] = points[i][f];
        };
        {
            const Points points{{0.0, -1.0, 0.0}, {0.25, 1.0, 0.0}, {1.0, 0.0, 0.0}};
            Engine e(1);
            drawnOn(e, points, false, 0.0, 1.0);
            const double seconds = timeFromNorm(normFromTime(1.0));
            e.noteOn(57);
            double off = 0.0, phaseOff = 0.0;
            for (int block = 1; block <= 1800; ++block) { // 1.2 s
                e.render(32);
                e.synth().telemetry(*telemetry);
                const double x = std::min(block * 32.0 / (seconds * kSampleRate), 1.0);
                off = std::max(off, std::abs(telemetry->opEnvLevels[0] - drawn(points, x)));
                phaseOff = std::max(phaseOff, std::abs(telemetry->opEnvPhase[0] - x));
            }
            e.noteOff(57);
            int blocks = 0;
            for (; blocks < 1000; ++blocks) {
                e.render(32);
                e.synth().telemetry(*telemetry);
                if (telemetry->voices == 0)
                    break;
            }
            std::printf("      in the synth: %.2e off its shape, phase %.2e off; let go at its end, its voice ended %.1f ms "
                        "later\n",
                        off, phaseOff, blocks * 32.0 / kSampleRate * 1000.0);
            check(off < 1e-5 && phaseOff < 1e-5,
                  "an operator plays its drawn envelope, its level and place following the shape");
            check(blocks * 32.0 / kSampleRate < 0.02, "a voice ends once its operator's drawn envelope has faded out");
        }
        // Sustain in the synth: held, it loops [0.25, 0.5], its voice playing on; let go, it plays on to its end, its
        // voice ending then (0.5 s on from the loop's end at a second's shape, its end level faded out over 5 ms).
        {
            const Points points{{0.0, -1.0, 0.0}, {0.25, 1.0, 0.0}, {0.5, 0.0, 0.0}, {1.0, 0.0, 0.0}};
            Engine e(1);
            drawnOn(e, points, true, 0.25, 0.5);
            e.noteOn(57);
            double low = 1.0, high = 0.0;
            for (int block = 1; block <= 3000; ++block) { // 2 s held
                e.render(32);
                e.synth().telemetry(*telemetry);
                if (block > 400)
                    low = std::min(low, static_cast<double>(telemetry->opEnvLevels[0])),
                    high = std::max(high, static_cast<double>(telemetry->opEnvLevels[0]));
            }
            e.noteOff(57);
            int blocks = 0;
            for (; blocks < 2000; ++blocks) {
                e.render(32);
                e.synth().telemetry(*telemetry);
                if (telemetry->voices == 0)
                    break;
            }
            const double ended = blocks * 32.0 / kSampleRate;
            std::printf("      sustain in the synth: looped between %.3f and %.3f for 2 s, its voice ended %.3f s after "
                        "the key\n",
                        low, high, ended);
            check(low > 0.49 && high < 1.0001 && high > 0.99 && ended > 0.49 && ended < 0.53,
                  "an operator's drawn envelope in Sustain loops while held, its voice playing on to its end once let go");
        }
    }

    // --- The parametric EQ's and phase disperser's analyzers: a sine shows in its band, at its level (a flat EQ
    // passes it as it is, and so does a disperser all wet, an all-pass).
    std::printf("parametric EQ and phase disperser analyzers:\n");
    for (FxType type : {FxType::ParamEq, FxType::Disperser}) {
        const bool eq = type == FxType::ParamEq;
        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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

    // --- The chorus's Voices, Spread, Delay and Depth and the flanger's Delay and Depth changed under a sine, all wet: a
    // chorus voice coming or going fades, the voices' sweeps glide to their new places apart, a voice moving between the
    // sides moves there over 10 ms, and a Delay or Depth that jumps (as one a note's own random value sets does as the
    // effects take up the new note) crossfades to where it reads over 10 ms, also in single frames and through zero, so
    // no sample steps further than a sine as loud as the loudest the voices play does (its pitch wavering a little with
    // the sweep): voices in phase can play louder than what went in.
    std::printf("chorus and flanger changed under a sine (steepest step against a sine's at its peak):\n");
    {
        auto steepest = [](FxType type, FxParam param, std::initializer_list<double> values, uint32_t piece = kBlock,
                           bool throughZero = false) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(type);
            if (type == FxType::Chorus) {
                p[paramIndex(0, FxParam::ChorusMix)] = 1.0;
                p[paramIndex(0, FxParam::ChorusSpread)] = 0.0;
            } else {
                p[paramIndex(0, FxParam::FlangerMix)] = 1.0;
                p[paramIndex(0, FxParam::FlangerThroughZero)] = throughZero ? 1.0 : 0.0;
            }
            chain(p, {0});
            constexpr double kHz = 440.0;
            const uint32_t each = static_cast<uint32_t>(kSampleRate / 4);
            std::vector<float> left(kBlock), right(kBlock);
            double most = 0.0, last = 0.0, peak = 0.0;
            uint32_t t = 0;
            for (double value : values) {
                p[paramIndex(0, param)] = value;
                for (uint32_t done = 0; done < each; done += kBlock, t += kBlock) {
                    for (uint32_t s = 0; s < kBlock; ++s)
                        left[s] = right[s] = static_cast<float>(
                            0.5 * std::sin(2.0 * std::numbers::pi * kHz * (t + s) / kSampleRate));
                    for (uint32_t s = 0; s < kBlock; s += piece)
                        fx.process(left.data() + s, right.data() + s, std::min(piece, kBlock - s), p, 120.0, 60.0);
                    for (uint32_t s = 0; s < kBlock; ++s) {
                        if (t + s > kSampleRate / 10) { // past its first voices' delays
                            most = std::max(most, static_cast<double>(std::abs(left[s] - last)));
                            peak = std::max(peak, static_cast<double>(std::abs(left[s])));
                        }
                        last = left[s];
                    }
                }
            }
            return most / (peak * 2.0 * std::numbers::pi * kHz / kSampleRate);
        };
        const double voices = steepest(FxType::Chorus, FxParam::ChorusVoices, {2.0, 6.0, 3.0, 2.0});
        const double spread = steepest(FxType::Chorus, FxParam::ChorusSpread, {0.0, 1.0, 0.0});
        std::printf("      chorus voices 2, 6, 3, 2: x%.3f   spread 0, 100, 0 %%: x%.3f\n", voices, spread);
        check(voices < 1.15 && spread < 1.15,
              "the chorus's Voices and Spread changed under a sine glide rather than click");
        // Each jump in whole blocks and in single frames: the steeper of the two.
        auto jumps = [&](const char *name, FxType type, FxParam param, std::initializer_list<double> values,
                         bool throughZero = false) {
            const double blocks = steepest(type, param, values, kBlock, throughZero);
            const double frames = steepest(type, param, values, 1, throughZero);
            std::printf("      %-34s x%.3f (x%.3f in single frames)\n", name, blocks, frames);
            return std::max(blocks, frames);
        };
        const double chorusDelay = jumps("chorus delay 3, 25, 8 ms:", FxType::Chorus, FxParam::ChorusDelay,
                                         {normFromTime(0.003), normFromTime(0.025), normFromTime(0.008)});
        const double chorusDepth =
            jumps("chorus depth 0, 100, 20 %:", FxType::Chorus, FxParam::ChorusDepth, {0.0, 1.0, 0.2});
        check(chorusDelay < 1.15 && chorusDepth < 1.15,
              "the chorus's Delay and Depth jumping under a sine crossfade rather than click");
        const std::initializer_list<double> flangerDelays{normFromTime(0.001), normFromTime(0.01), normFromTime(0.003)};
        const double flangerDelay = jumps("flanger delay 1, 10, 3 ms:", FxType::Flanger, FxParam::FlangerDelay,
                                          flangerDelays);
        const double flangerDepth =
            jumps("flanger depth 0, 100, 30 %:", FxType::Flanger, FxParam::FlangerDepth, {0.0, 1.0, 0.3});
        const double flangerZero = jumps("flanger delay through zero:", FxType::Flanger, FxParam::FlangerDelay,
                                         flangerDelays, true);
        check(flangerDelay < 1.15 && flangerDepth < 1.15 && flangerZero < 1.15,
              "the flanger's Delay and Depth jumping under a sine crossfade rather than click, through zero too");
    }

    // --- The phase disperser's Stages changed under a sine, all wet: the count it played fades out as the new one
    // fades in (see fx::Disperser), in whole blocks and in single frames alike. A switch from one count to another
    // would step as far as the two chains' phases differ, a click: its third difference (which passes the sine and
    // the chains' ringing at Freq at under a thousandth of their level, and a step almost whole) stays small.
    std::printf("phase disperser stages changed under a sine (largest third difference):\n");
    {
        auto roughest = [](uint32_t piece) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Disperser);
            p[paramIndex(0, FxParam::DisperserFreq)] = normFromCutoff(600.0);
            p[paramIndex(0, FxParam::DisperserPinch)] = 0.5;
            p[paramIndex(0, FxParam::DisperserMix)] = 1.0;
            chain(p, {0});
            constexpr double kHz = 220.0;
            const uint32_t each = static_cast<uint32_t>(kSampleRate / 4);
            std::vector<float> left(piece), right(piece);
            double most = 0.0;
            std::array<double, 3> last{};
            uint32_t t = 0;
            for (double stages : {8.0, 9.0, 24.0, 4.0, 16.0, 1.0, 64.0}) {
                p[paramIndex(0, FxParam::DisperserStages)] = stages;
                for (uint32_t done = 0; done < each; done += piece, t += piece) {
                    for (uint32_t s = 0; s < piece; ++s)
                        left[s] = right[s] = static_cast<float>(
                            0.5 * std::sin(2.0 * std::numbers::pi * kHz * (t + s) / kSampleRate));
                    fx.process(left.data(), right.data(), piece, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < piece; ++s) {
                        const double third = left[s] - 3.0 * last[0] + 3.0 * last[1] - last[2];
                        if (t + s > kSampleRate / 10) // past the first chain's own start
                            most = std::max(most, std::abs(third));
                        last = {left[s], last[0], last[1]};
                    }
                }
            }
            return most;
        };
        const double blocks = roughest(kBlock), frames = roughest(1);
        std::printf("      stages 8, 9, 24, 4, 16, 1, 64: %.5f in whole blocks, %.5f in single frames\n", blocks, frames);
        check(blocks < 0.002 && frames < 0.002, "the phase disperser's Stages changed under a sine fade rather than click");
    }

    // --- The phase disperser's Key Track and Stretch, each against the chain they make, worked out from the same
    // coefficients: an impulse through the effect, after a moment's silence while it fades in, comes out as through
    // that chain (to within the float it is kept in between effects).
    std::printf("phase disperser key track and stretch (impulse against the chain they make, largest difference):\n");
    {
        // What the effect puts out for an impulse, `pitch` the newest note's, at `rate`.
        auto impulse = [](double freqHz, double keyTrack, double stretch, double pitch, double rate) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(rate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Disperser);
            p[paramIndex(0, FxParam::DisperserFreq)] = normFromCutoff(freqHz);
            p[paramIndex(0, FxParam::DisperserStages)] = 8.0;
            p[paramIndex(0, FxParam::DisperserPinch)] = 0.6;
            p[paramIndex(0, FxParam::DisperserMix)] = 1.0;
            p[paramIndex(0, FxParam::DisperserKeyTrack)] = keyTrack;
            p[paramIndex(0, FxParam::DisperserStretch)] = stretch;
            chain(p, {0});
            const uint32_t settle = static_cast<uint32_t>(0.05 * rate) / kBlock * kBlock;
            std::vector<float> out;
            std::vector<float> left(kBlock), right(kBlock);
            for (uint32_t t = 0; t < settle + 16384; t += kBlock) {
                std::fill(left.begin(), left.end(), 0.0f);
                if (t == settle)
                    left[0] = 1.0f;
                right = left;
                fx.process(left.data(), right.data(), kBlock, p, 120.0, pitch);
                if (t >= settle)
                    out.insert(out.end(), left.begin(), left.end());
            }
            return out;
        };
        // The plain chain of 8 at `hz`, its delays `stretch` samples long: the unstretched one's impulse response
        // spread out, every stretch-th sample, silence between.
        auto expected = [](double hz, int stretch, double rate, size_t length) {
            const std::array<double, 5> b = disperserCoefs(hz, 0.6, rate, stretch);
            const fx::BiquadCoefs c{b[0], b[1], b[2], b[3], b[4]};
            std::array<fx::Biquad, 8> chain{};
            std::vector<double> out(length, 0.0);
            for (size_t m = 0; m * stretch < length; ++m) {
                double y = m == 0 ? 1.0 : 0.0;
                for (fx::Biquad &stage : chain)
                    y = stage.process(y, c);
                out[m * stretch] = y;
            }
            return out;
        };
        auto difference = [](const std::vector<float> &got, const std::vector<double> &want) {
            double most = 0.0;
            for (size_t i = 0; i < want.size(); ++i)
                most = std::max(most, std::abs(got[i] - want[i]));
            return most;
        };
        const double c4 = difference(impulse(500.0, 1.0, 1.0, 60.0, kSampleRate), expected(500.0, 1, kSampleRate, 16384));
        const double c5 = difference(impulse(500.0, 1.0, 1.0, 72.0, kSampleRate), expected(1000.0, 1, kSampleRate, 16384));
        const double half = difference(impulse(500.0, 0.5, 1.0, 84.0, kSampleRate), expected(1000.0, 1, kSampleRate, 16384));
        std::printf("      key track 100 %%: a C4 at Freq %.1e, a C5 an octave up %.1e; 50 %% and a C6 an octave up %.1e\n",
                    c4, c5, half);
        check(c4 < 1e-6 && c5 < 1e-6 && half < 1e-6,
              "the phase disperser's Key Track moves Freq with the note from C4 on, as the filter's does");
        const double by4 = difference(impulse(700.0, 0.0, 4.0, 60.0, kSampleRate), expected(700.0, 4, kSampleRate, 16384));
        const double by37 =
            difference(impulse(700.0, 0.0, 37.0, 60.0, kSampleRate), expected(700.0, 37, kSampleRate, 16384));
        const double by8At96 = difference(impulse(700.0, 0.0, 4.0, 60.0, 96000.0), expected(700.0, 8, 96000.0, 16384));
        std::printf("      stretch 4 %.1e, 37 %.1e, 4 at 96 kHz (8 samples) %.1e\n", by4, by37, by8At96);
        check(by4 < 1e-6 && by37 < 1e-6 && by8At96 < 1e-6 && disperserStretch(4.0, 96000.0) == 8 &&
                  disperserStretch(4.0, 44100.0) == 4 && disperserStretch(64.0, 192000.0) == 256,
              "the phase disperser's Stretch makes each all-pass's delays as many samples long at 48 kHz, as long a "
              "time at other rates");
        // Stretched, its delay peaks at Freq and again every 48 kHz / Stretch either way: at 32, 1.5 kHz apart, a
        // 400 Hz one's copies at 1.1 and 1.9 kHz.
        auto peakNear = [](double hz) {
            double best = 0.0, at = hz;
            for (double f = hz * 0.9; f < hz * 1.1; f += hz * 0.0002)
                if (const double d = disperserDelay(f, 400.0, 0.6, 8, kSampleRate, 32); d > best) {
                    best = d;
                    at = f;
                }
            return at;
        };
        const double first = peakNear(400.0), mirror = peakNear(1100.0), copy = peakNear(1900.0);
        std::printf("      stretch 32 at 400 Hz: its delay peaks at %.0f Hz, %.0f Hz and %.0f Hz\n", first, mirror, copy);
        check(std::abs(first / 400.0 - 1.0) < 0.01 && std::abs(mirror / 1100.0 - 1.0) < 0.01 &&
                  std::abs(copy / 1900.0 - 1.0) < 0.01,
              "stretched, the phase disperser's delay peaks at Freq and again every 48 kHz / Stretch, either way");
    }
    // Its Stretch changed under a sine, all wet: it fades out to the sine and back in at the new length, so its third
    // difference stays far under an instant switch's (1 to 1.6 here). A stretched chain starting from silence buzzes
    // at the rate over its stretch, as it does on any note, which the fade back in keeps down rather than takes away.
    // And the longest stretch, the most stages and the narrowest pinch stay finite and ring out.
    {
        auto roughest = [](uint32_t piece) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            fx.prepare(kSampleRate);
            ParamValues p;
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Disperser);
            p[paramIndex(0, FxParam::DisperserFreq)] = normFromCutoff(600.0);
            p[paramIndex(0, FxParam::DisperserMix)] = 1.0;
            chain(p, {0});
            const uint32_t each = static_cast<uint32_t>(kSampleRate / 4);
            std::vector<float> left(piece), right(piece);
            double most = 0.0;
            std::array<double, 3> last{};
            uint32_t t = 0;
            for (double stretch : {1.0, 4.0, 16.0, 64.0, 2.0, 1.0}) {
                p[paramIndex(0, FxParam::DisperserStretch)] = stretch;
                for (uint32_t done = 0; done < each; done += piece, t += piece) {
                    for (uint32_t s = 0; s < piece; ++s)
                        left[s] = right[s] = static_cast<float>(
                            0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * (t + s) / kSampleRate));
                    fx.process(left.data(), right.data(), piece, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < piece; ++s) {
                        const double third = left[s] - 3.0 * last[0] + 3.0 * last[1] - last[2];
                        if (t + s > kSampleRate / 10)
                            most = std::max(most, std::abs(third));
                        last = {left[s], last[0], last[1]};
                    }
                }
            }
            return most;
        };
        const double blocks = roughest(kBlock), frames = roughest(1);
        std::printf("      stretch 1, 4, 16, 64, 2, 1 under a sine: third difference %.5f in whole blocks, %.5f in single "
                    "frames\n", blocks, frames);
        check(blocks < 0.05 && frames < 0.05, "the phase disperser's Stretch changed under a sine fades rather than clicks");

        const auto fxHeap = std::make_unique<Effects>();
        Effects &fx = *fxHeap;
        fx.prepare(kSampleRate);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Disperser);
        p[paramIndex(0, FxParam::DisperserStages)] = 64.0;
        p[paramIndex(0, FxParam::DisperserPinch)] = 1.0;
        p[paramIndex(0, FxParam::DisperserStretch)] = 64.0;
        p[paramIndex(0, FxParam::DisperserKeyTrack)] = 1.0;
        chain(p, {0});
        std::vector<float> left(kBlock), right(kBlock);
        bool finite = true;
        double loud = 0.0, end = 0.0;
        for (uint32_t t = 0; t < 30 * static_cast<uint32_t>(kSampleRate); t += kBlock) {
            const bool sounding = t < kSampleRate / 2;
            for (uint32_t s = 0; s < kBlock; ++s)
                left[s] = right[s] = sounding ? static_cast<float>(((t + s) * 2654435761u >> 16 & 1023) / 1023.0 - 0.5) : 0.0f;
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 96.0); // a C7, its tracked Freq high up
            for (uint32_t s = 0; s < kBlock; ++s) {
                finite = finite && std::isfinite(left[s]) && std::isfinite(right[s]);
                loud = std::max(loud, static_cast<double>(std::abs(left[s])));
                if (t > 29 * static_cast<uint32_t>(kSampleRate))
                    end = std::max(end, static_cast<double>(std::abs(left[s])));
            }
        }
        std::printf("      64 stages, full pinch and stretch, noise for half a second: loudest %.2f, after 29 s %.1e\n",
                    loud, end);
        check(finite && loud < 4.0 && end < 1e-4,
              "the phase disperser at its longest stretch, most stages and narrowest pinch stays finite and rings out");
    }

    // --- The chorus's voices, held still (no Depth), all wet and in the middle: an impulse comes out once a voice,
    // each at its place along Delay (12.5 ms here, so every place falls on a sample), a little shorter or longer than it, as loud as 1 / sqrt(voices) (less
    // the share of it the bass takes past them). And swept, its voices together play a sine about as loud as it went
    // in, low and high, however many there are.
    std::printf("chorus voices (impulse's echoes at their places; a sine's level, all wet):\n");
    {
        auto chorusFx = [](Effects &fx, ParamValues &p, int voices, double depth) {
            fx.prepare(kSampleRate);
            for (int i = 0; i < kParamCount; ++i)
                p[i] = paramTable()[i].def;
            p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Chorus);
            p[paramIndex(0, FxParam::ChorusVoices)] = voices;
            p[paramIndex(0, FxParam::ChorusDepth)] = depth;
            p[paramIndex(0, FxParam::ChorusDelay)] = normFromTime(0.0125);
            p[paramIndex(0, FxParam::ChorusMix)] = 1.0;
            p[paramIndex(0, FxParam::ChorusSpread)] = 0.0;
            chain(p, {0});
        };
        bool placed = true;
        for (int voices : {2, 6}) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            ParamValues p;
            chorusFx(fx, p, voices, 0.0);
            const uint32_t total = static_cast<uint32_t>(0.02 * kSampleRate);
            static_assert(kSampleRate == 48000.0);
            std::vector<float> out, left(kBlock), right(kBlock);
            for (uint32_t done = 0; done < total; done += kBlock) {
                std::fill(left.begin(), left.end(), 0.0f);
                std::fill(right.begin(), right.end(), 0.0f);
                if (done == 0)
                    left[0] = right[0] = 1.0f;
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                out.insert(out.end(), left.begin(), left.end());
            }
            const double level = 1.0 / std::sqrt(static_cast<double>(voices));
            std::printf("      %d voices:", voices);
            for (int k = 0; k < voices; ++k) {
                const size_t at = static_cast<size_t>(std::lround(0.0125 * chorusShare(k, voices, 0.0, 0.0) * kSampleRate));
                std::printf(" %.3f at %zu", out[at], at);
                placed = placed && std::abs(out[at] / level - 0.97) < 0.03 &&
                         std::abs(out[at - 1]) < 0.05 && std::abs(out[at + 1]) < 0.05;
            }
            std::printf("\n");
        }
        check(placed, "the chorus's voices each wait their own place along Delay, as loud as 1 / sqrt(voices)");
        bool level = true;
        for (double hz : {440.0, 5000.0}) {
            std::printf("      %4.0f Hz:", hz);
            for (int voices : {2, 3, 4, 6}) {
                const auto fxHeap = std::make_unique<Effects>();
                Effects &fx = *fxHeap;
                ParamValues p;
                chorusFx(fx, p, voices, 0.5);
                const uint32_t total = static_cast<uint32_t>(2.0 * kSampleRate);
                std::vector<float> left(kBlock), right(kBlock);
                double in = 0.0, outSum = 0.0;
                for (uint32_t done = 0; done < total; done += kBlock) {
                    for (uint32_t s = 0; s < kBlock; ++s)
                        left[s] = right[s] =
                            static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * (done + s) / kSampleRate));
                    for (uint32_t s = 0; s < kBlock; ++s)
                        in += left[s] * left[s];
                    fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                    for (uint32_t s = 0; s < kBlock; ++s)
                        outSum += left[s] * left[s];
                }
                const double ratio = std::sqrt(outSum / in);
                std::printf(" x%.2f (%d)", ratio, voices);
                level = level && ratio > 0.6 && ratio < 1.5;
            }
            std::printf("\n");
        }
        check(level, "the chorus's voices together play about as loud as what went in, however many, low and high");

        // Its Feedback full up either way, Voices dropped from 6 to 2 under a loud sine and the sine then stopped: what
        // goes round doesn't grow while the voices fade out, and dies away after. Its peaks move about with the sweep,
        // which moves its resonances; grown out of hand, they would be orders of magnitude louder.
        bool steady = true;
        for (double feedback : {1.0, -1.0}) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
            ParamValues p;
            chorusFx(fx, p, 6, 0.5);
            p[paramIndex(0, FxParam::ChorusFeedback)] = feedback;
            std::vector<float> left(kBlock), right(kBlock);
            double before = 0.0, during = 0.0, after = 0.0; // with 6 voices, as they drop to 2, and once settled
            const uint32_t second = static_cast<uint32_t>(kSampleRate);
            for (uint32_t done = 0; done < 3 * second; done += kBlock) {
                if (done >= second)
                    p[paramIndex(0, FxParam::ChorusVoices)] = 2.0;
                for (uint32_t s = 0; s < kBlock; ++s)
                    left[s] = right[s] =
                        done < 2 * second
                            ? static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 700.0 * (done + s) / kSampleRate))
                            : 0.0f;
                fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
                for (uint32_t s = 0; s < kBlock; ++s) {
                    const double peak = std::max(std::abs(left[s]), std::abs(right[s]));
                    double &into = done < second ? before : done < second + second / 4 ? during : after;
                    if (done < 2 * second)
                        into = std::max(into, peak);
                }
            }
            double last = 0.0; // the last block's peak, a second after the sine stopped
            for (uint32_t s = 0; s < kBlock; ++s)
                last = std::max(last, static_cast<double>(std::max(std::abs(left[s]), std::abs(right[s]))));
            std::printf("      feedback %+.0f %%: peak %.2f with 6 voices, %.2f dropping to 2, %.2f with 2, then %.2g a "
                        "second after\n",
                        feedback * 100.0, before, during, after, last);
            steady = steady && std::isfinite(during) && during < 3.0 * std::max(before, after) && after < 2.5 &&
                     last < 1e-3;
        }
        check(steady, "the chorus's feedback stays steady as voices fade out, and dies away after");
    }

    // --- The phaser's and flanger's polarity, standing still with no feedback at full Mix (half wet). Positive, a
    // sine where the wet sound comes out turned over is taken away and one where it comes out as it went in plays on
    // at full level; negative, the other way round. A 2 ms flanger's first notch is at 250 Hz, and negative at
    // 500 Hz; four all-passes at 1 kHz turn a whole cycle there, half a cycle where each turns an eighth. Turned
    // over while a sine plays, the polarity fades through nothing: no sample steps further than the sine's own do.
    std::printf("polarity (level of a sine, in dB, positive and negative):\n");
    {
        auto run = [](FxType type, double hz, bool negative, bool turnOver, double *steepest) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
        // to -35 dB in half the decay time. Around 8 kHz too, a little faster for the damping's shelf (its corner at
        // 20 kHz), but not much: the lines' reads between samples, as they waver, take nothing from the top.
        const auto sides = impulse(ReverbType::Hall, 0.0);
        const auto &left = sides.first, &right = sides.second;
        size_t n = 1;
        while (n < left.size())
            n *= 2;
        auto decay =[&](double low, double high) { // the time to fall 60 dB in the band from `low` to `high` Hz
            std::vector<std::complex<double>> octave(n);
            for (size_t i = 0; i < left.size(); ++i)
                octave[i] = {left[i], right[i]}; // both sides at once: they come back as the real and imaginary parts
            fft(octave);
            for (size_t k = 0; k < n; ++k) {
                const double hz = std::min(k, n - k) * kSampleRate / n;
                octave[k] = hz > low && hz < high ? std::conj(octave[k]) : 0.0; // conjugated, for the inverse
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
            return 2.0 * (at35 - at5);
        };
        const double t60 = decay(707.0, 1414.0), high60 = decay(5657.0, 11314.0);
        std::printf("      undamped hall falls 60 dB in %.2f s around 1 kHz, %.2f s around 8 kHz (set to 2.5 s)\n", t60,
                    high60);
        check(std::abs(t60 / 2.5 - 1.0) < 0.15, "an undamped reverb's tail falls 60 dB in its decay time");
        check(high60 / t60 > 0.8, "an undamped reverb's tail keeps its top about as long");
    }

    // --- A shimmer's tail after a 0.4 s A4 sine (4 s decay, at full wet), with Shimmer half and full up. As a whole it
    // falls 60 dB in about the decay time; its energy stays on the octaves (within 3 %), where the shifter puts it; and
    // each of the first two octaves up is a narrow line, not smeared by the shifter's crossfades: most of its energy
    // within 4 Hz. At the longest decay, undamped, with Shimmer full up, it still dies away.
    std::printf("shimmer tails (an A4 sine, 4 s decay):\n");
    {
        auto shimmer = [](double amount, double decay, double damping, double seconds) {
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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
            const auto fxHeap = std::make_unique<Effects>();
            Effects &fx = *fxHeap;
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

    // --- The example presets' triads at full velocity, struck together as a host's quantized chord is, held 2 s and
    // let go. Their Volume keeps them at -1 dBFS at most (see AGENTS.md); this leaves that dB for what a note's
    // randomness moves. A drum preset is played a hit at a time.
    std::printf("example presets' triads (peak at full velocity, dBFS):\n");
    {
        const std::vector<std::vector<int>> chords = {{48, 52, 55}, {60, 64, 67}, {72, 76, 79}};
        const std::vector<std::vector<int>> hits = {{36}, {60}, {84}};
        constexpr int kHeld = static_cast<int>(2.0 * kSampleRate / kBlock), kLetGo = kHeld / 2;
        bool under = true;
        for (const FactoryPreset &preset : factoryPresets()) {
            ParamValues values;
            gui::UiLayout layout;
            loadFactoryPreset(preset, values, layout);
            // As the plugin plays it: its voices and play mode (see PluginCore::applyPlayMode()).
            auto on = [&values](GlobalParam p) { return values[paramIndex(p)] >= 0.5; };
            const int voices = static_cast<int>(std::lround(values[paramIndex(GlobalParam::Voices)]));
            float peak = 0.0f;
            for (const std::vector<int> &keys : layout.presetInfo.category == "Drum" ? hits : chords) {
                Engine e(2, voices);
                e.synth().setPlayMode(on(GlobalParam::Mono), on(GlobalParam::Legato), on(GlobalParam::GlideAlways));
                e.p = values;
                for (int key : keys)
                    e.noteOn(key);
                float left[kBlock], right[kBlock];
                for (int b = 0; b < kHeld + kLetGo; ++b) {
                    if (b == kHeld)
                        for (int key : keys)
                            e.noteOff(key);
                    e.synth().render(left, right, kBlock, e.p);
                    for (uint32_t s = 0; s < kBlock; ++s)
                        peak = std::max({peak, std::abs(left[s]), std::abs(right[s])});
                }
            }
            std::printf("      %-16s %6.1f\n", preset.name.c_str(), 20.0 * std::log10(peak + 1e-30f));
            under = under && peak < 1.0f;
        }
        check(under, "the example presets' triads at full velocity peak under 0 dBFS");
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
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Random);
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
    rampEndChecks();
    latencyChecks();
    stackChecks();
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

    // --- A Curve source's Slew lags what it puts out behind its curve: a square at 1 Hz, 10 ms after it steps up, has
    // gone all the way without one and about a tenth of it with a 100 ms slew (from where it was), as a MIDI source's
    // Slew follows its controller.
    std::printf("curve slew:\n");
    {
        auto at = [](double slew) {
            Engine e(2);
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Curve);
            e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
            e.p[paramIndex(0, SourceParam::Rate)] = normFromRate(1.0);
            e.p[paramIndex(0, SourceParam::CurveSlew)] = slew;
            e.p[shapeCountIndex(0)] = 4.0;
            const double square[4][3] = {{0.0, -1.0, 0.0}, {0.5, -1.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 1.0, 0.0}};
            for (int i = 0; i < 4; ++i)
                for (int f = 0; f < 3; ++f)
                    e.p[shapePointIndex(0, i, static_cast<PointField>(f))] = square[i][f];
            e.p[slotTargetIndex(0, 0)] = slotTargetValue(paramIndex(GlobalParam::FmAmount));
            e.p[slotAmountIndex(0, 0)] = 0.1;
            e.noteOn(60);
            Telemetry t;
            e.render(19200); // 0.4 s: low
            e.synth().telemetry(t);
            const float low = t.sources[0];
            e.render(4800 + 480); // 10 ms after the step up
            e.synth().telemetry(t);
            return std::pair{low, t.sources[0]};
        };
        const auto [low, plain] = at(0.0);
        const auto [lowSlewed, slewed] = at(normFromTime(0.1));
        const float share = (slewed - lowSlewed) / (plain - low);
        std::printf("      10 ms after the step: %.3f of the way with a 100 ms slew (from %.3f to %.3f without)\n", share,
                    low, plain);
        check(plain - low > 0.4f && std::abs(lowSlewed - low) < 1e-6f && share > 0.04f && share < 0.25f,
              "a Curve source's Slew lags what it puts out behind its curve");
    }
    // The same square through a 1 s Slew: a note joining a held one in poly legato carries on from where the held one's
    // Slew stands, rather than from its curve; and on a reverb's Mix, the effects' tail goes on from where the voice's
    // stood as it ended, rather than jumping onto the curve.
    {
        auto curveOn = [](Engine &e, double slew, int target) {
            e.p[paramIndex(0, SourceParam::Type)] = static_cast<double>(SourceType::Curve);
            e.p[paramIndex(0, SourceParam::Sync)] = 0.0;
            e.p[paramIndex(0, SourceParam::Rate)] = normFromRate(1.0);
            e.p[paramIndex(0, SourceParam::CurveSlew)] = slew;
            e.p[shapeCountIndex(0)] = 4.0;
            const double square[4][3] = {{0.0, -1.0, 0.0}, {0.5, -1.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 1.0, 0.0}};
            for (int i = 0; i < 4; ++i)
                for (int f = 0; f < 3; ++f)
                    e.p[shapePointIndex(0, i, static_cast<PointField>(f))] = square[i][f];
            e.p[slotTargetIndex(0, 0)] = slotTargetValue(target);
            e.p[slotAmountIndex(0, 0)] = 0.5;
        };
        auto joined = [&](double slew) {
            Engine e(2);
            e.synth().setPlayMode(false, true, false);
            curveOn(e, slew, paramIndex(GlobalParam::FmAmount));
            e.noteOn(48);
            e.render(28800); // 0.6 s: a tenth of a second past the step up
            Telemetry held, join;
            e.synth().telemetry(held);
            e.noteOn(55);
            e.render(kBlock);
            e.synth().telemetry(join);
            return std::pair{held.sources[0], join.sources[0]};
        };
        const auto [heldPlain, joinPlain] = joined(0.0);
        const auto [heldSlewed, joinSlewed] = joined(normFromTime(1.0));
        std::printf("      joining in poly legato: the held voice's Curve at %.3f (%.3f unslewed), the new one's %.3f\n",
                    heldSlewed, heldPlain, joinSlewed);
        check(heldPlain - heldSlewed > 0.2f && std::abs(joinSlewed - heldSlewed) < 0.02f && joinPlain == heldPlain,
              "a note joining a held one in poly legato carries its Curve's Slew on from the held one's");

        Engine e(2);
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.01);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        e.p[paramIndex(0, FxParam::ReverbDecay)] = 1.0;
        chain(e.p, {0});
        const int mix = paramIndex(0, FxParam::ReverbMix);
        e.p[mix] = 0.5;
        curveOn(e, normFromTime(1.0), mix);
        e.noteOn(57);
        e.render(26400); // 0.55 s: just past the step up
        e.noteOff(57);
        Telemetry t;
        float before = 0.0f, after = 0.0f;
        bool ended = false;
        for (int block = 0; block < 200 && !ended; ++block) {
            e.synth().telemetry(t);
            before = t.modOffset[mix];
            e.render(kBlock);
            e.synth().telemetry(t);
            ended = t.voices == 0 && t.fxTail;
            after = t.modOffset[mix];
        }
        std::printf("      the voice ending: Reverb Mix's modulation %+.3f, then %+.3f in the tail\n", before, after);
        check(ended && std::abs(after - before) < 0.02f,
              "the effects' tail carries a Curve's Slew on from where the voice's stood");
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

    // --- An LFO on an effect runs on through the effects' tail once the note's voice has ended: a square LFO on a
    // utility's Gain after a long reverb keeps the tail stepping up and down, retriggered or free.
    std::printf("effects' modulation after the last voice:\n");
    for (const bool free : {false, true}) {
        Engine e(2);
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.01);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        e.p[paramIndex(0, FxParam::ReverbDecay)] = 1.0; // its longest
        e.p[paramIndex(0, FxParam::ReverbMix)] = 1.0;
        e.p[paramIndex(1, FxParam::Type)] = static_cast<double>(FxType::Utility);
        chain(e.p, {0, 1});
        lfoOn(e.p, paramIndex(1, FxParam::UtilityGain), 0.5, 4.0);
        e.p[paramIndex(0, SourceParam::Shape)] = static_cast<double>(LfoShape::Square);
        e.p[paramIndex(0, SourceParam::Free)] = free ? 1.0 : 0.0;
        e.noteOn(57);
        e.render(14400);
        e.noteOff(57);
        e.render(9600);
        Telemetry t;
        e.synth().telemetry(t);
        const int voices = t.voices;
        const bool reported = t.fxTail && t.modOffset[paramIndex(1, FxParam::UtilityGain)] != 0.0f;
        // and that the tail runs the LFO, where its card shows it: moving on at its rate
        const float phaseBefore = t.lfoPhases[0];
        const bool lfoShown = t.fxTailLfos == 1u && std::abs(t.lfoRates[0] - 4.0f) < 0.01f;
        // The steps between sixteenths of a second: an eighth of a second each way, the LFO's half cycle.
        const std::vector<float> tail = e.render(48000);
        constexpr size_t kWindow = 3000;
        std::vector<double> levels;
        for (size_t at = 0; at + kWindow <= tail.size(); at += kWindow) {
            double sum = 0.0;
            for (size_t i = at; i < at + kWindow; ++i)
                sum += static_cast<double>(tail[i]) * tail[i];
            levels.push_back(10.0 * std::log10(sum / kWindow + 1e-30));
        }
        double step = 0.0;
        for (size_t k = 1; k < levels.size(); ++k)
            step = std::max(step, std::abs(levels[k] - levels[k - 1]));
        std::printf("      %-10s LFO on a utility after a reverb: %d voice(s), tail steps up to %.1f dB\n",
                    free ? "free" : "retrigger", voices, step);
        check(voices == 0 && step > 12.0, "an LFO keeps modulating the effects through their tail once the voice ends");
        e.render(3000); // a sixteenth of a second more: a quarter of a cycle on
        e.synth().telemetry(t);
        check(reported, "the editor is told where the tail's modulation takes the effects");
        check(lfoShown && t.fxTailLfos == 1u && t.lfoPhases[0] != phaseBefore,
              "the editor is told where the LFOs the tail runs are, as they move on");
    }
    // With a short reverb, the tail's modulation stops once the effects have been silent for a second.
    {
        Engine e(2);
        e.p[paramIndex(0, OpParam::Release)] = normFromTime(0.01);
        e.p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::Reverb);
        e.p[paramIndex(0, FxParam::ReverbDecay)] = normFromTime(0.3);
        chain(e.p, {0});
        lfoOn(e.p, paramIndex(0, FxParam::ReverbMix), 0.5, 4.0);
        e.noteOn(57);
        e.render(4800);
        e.noteOff(57);
        e.render(4800);
        Telemetry t;
        e.synth().telemetry(t);
        const bool running = t.voices == 0 && t.fxTail;
        e.render(static_cast<size_t>(6.0 * kSampleRate));
        e.synth().telemetry(t);
        check(running && !t.fxTail && t.modOffset[paramIndex(0, FxParam::ReverbMix)] == 0.0f,
              "the effects' modulation stops once they have rung out");
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
    bendTelemetryChecks();

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
