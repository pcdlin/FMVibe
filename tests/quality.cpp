// Audio quality measurements on the engine itself: aliasing, zipper noise from
// stepped modulation, clicks when a voice is stolen or a param jumps between
// events, and what oversampling costs. Prints the numbers and checks them
// against limits. Usage: fmvibe-quality

#include "factory.h"
#include "synth.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iterator>
#include <numbers>
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
        p[paramIndex(GlobalParam::Volume)] = 0.0;
        for (int op = 0; op < kOps; ++op)
            p[paramIndex(op, OpParam::Velocity)] = 0.0;
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
             p[paramIndex(1, OpParam::Coarse)] = 4.0;
             p[matrixIndex(1, 0)] = 0.8;
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

    // --- The parametric EQ's analyzer: a sine shows in its band, at its level (a flat EQ passes it as it is).
    std::printf("parametric EQ analyzer:\n");
    {
        Effects fx;
        fx.prepare(kSampleRate);
        fx.setAnalyzing(true);
        ParamValues p;
        for (int i = 0; i < kParamCount; ++i)
            p[i] = paramTable()[i].def;
        p[paramIndex(0, FxParam::Type)] = static_cast<double>(FxType::ParamEq);
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
        std::printf("      a 1 kHz sine at -6 dB: band %d (expected %d) at %.1f dB\n", loudest, expected, db);
        check(std::abs(loudest - expected) <= 1 && std::abs(db + 6.02) < 1.5, "the EQ's analyzer shows a sine in its band, at its level");
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
            for (int i = 0; i < kParamCount; ++i) // as the preset file has it: Init, then its changes
                e.p[i] = paramTable()[i].def;
            bool inRange = true;
            for (const auto &[index, value] : preset.changes) {
                inRange = inRange && index >= 0 && index < kParamCount && value >= paramTable()[index].min &&
                          value <= paramTable()[index].max;
                if (index >= 0 && index < kParamCount)
                    e.p[index] = value;
            }
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

    // --- The output meter: each side's peak, falling 24 dB a second once the sound stops; past full scale (six
    // sines in step, 6 dB up) its clip light comes on and stays on.
    std::printf("output meter:\n");
    {
        auto peakOf = [](const std::vector<float> &x, size_t from) {
            float peak = 0.0f;
            for (size_t i = from; i < x.size(); ++i)
                peak = std::max(peak, std::abs(x[i]));
            return peak;
        };
        Engine e(2);
        e.p[paramIndex(GlobalParam::Volume)] = -6.0;
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
        loud.p[paramIndex(GlobalParam::Volume)] = 6.0;
        for (int op = 1; op < kOps; ++op) {
            loud.p[paramIndex(op, OpParam::Enabled)] = 1.0;
            loud.p[paramIndex(op, OpParam::Out)] = 1.0;
        }
        loud.noteOn(69);
        const float over = peakOf(loud.render(4800), 0);
        loud.synth().telemetry(t);
        std::printf("      A4 sine: peak %.3f, metered %.3f, %.1f dB lower half a second after it ends; "
                    "six of them 6 dB up: peak %.2f, clip light on for %.2f s more\n",
                    peak, held, fell, over, t.clipHold);
        check(std::abs(held / peak - 1.0f) < 0.02f && dark && fell > 10.0 && fell < 13.0 && over > 1.0f &&
                  t.clipHold > 1.0f,
              "the output meter follows the peak, falls 24 dB a second, and lights up past full scale");
    }

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
