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
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
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
        p[paramIndex(GlobalParam::Volume)] = 12.0; // the voices go into the effects 12 dB down: back to full level
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
        // A sine at -40 dB and 50 Hz through the shaper effect's drawn curve (as it starts: smooth, here made odd so
        // no DC blocker follows), at unity gain: each sample comes out as the curve has it at the middle of its step.
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
        double last = 0.0, worst = 0.0;
        for (size_t t = 0; t < static_cast<size_t>(kSampleRate); t += kBlock) {
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double phase = 2.0 * std::numbers::pi * 50.0 * static_cast<double>(t + s) / kSampleRate;
                left[s] = right[s] = static_cast<float>(0.01 * std::sin(phase));
            }
            const std::vector<float> in = left;
            fx.process(left.data(), right.data(), kBlock, p, 120.0, 60.0);
            for (uint32_t s = 0; s < kBlock; ++s) {
                const double drawn = shaperCurveValue(get, ClipMode::Custom, curve, true, 0.5 * (in[s] + last));
                if (t + s >= 480) // once faded in
                    worst = std::max(worst, std::abs(left[s] - drawn));
                last = in[s];
            }
        }
        std::printf("      drawn shaper curve, a -40 dB 50 Hz sine: off the curve by %.0f dB at most\n",
                    20.0 * std::log10(worst + 1e-30));
        check(worst < 1e-4, "a drawn shaper curve passes a quiet, slow sine as drawn, without spikes");
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
        // Host modulation for every voice, of a macro's knob, the mod wheel or the note source's depth, moves their
        // targets as turning them does: each takes A's Out down by 100 %, so modulated all the way, A goes silent.
        // The note source puts out all of it at C4, the top of its range here.
        auto peakWith = [](int source, int modulated, double amount, double mod) {
            Engine e(2);
            e.p[slotTargetIndex(source, 0)] = slotTargetValue(paramIndex(0, OpParam::Out));
            e.p[slotAmountIndex(source, 0)] = amount;
            e.p[paramIndex(NoteParam::High)] = 60.0;
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
        const float still = peakWith(kMaxSources, paramIndex(0, MacroParam::Value), -1.0, 0.0);
        std::printf("      A's peak, modulated away by the host through a macro %.4f, the mod wheel %.4f, the note "
                    "source %.4f; %.3f unmodulated\n", macro, wheel, depth, still);
        check(macro < 1e-3f && wheel < 1e-3f && depth < 1e-3f && still > 0.5f,
              "host modulation of a macro, the mod wheel and the note source's depth moves their targets");
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
}

// --- The factory presets' contents: each one pinned by a checksum, so an edit can't change a preset unnoticed.
// When a preset is changed on purpose, update its checksum here (the check prints the new one); a new preset gets
// a line too.
struct PinnedPreset {
    const char *name;
    uint64_t checksum;
};
constexpr PinnedPreset kPresetChecksums[] = {
    {"KY Electric Piano", 0x4f25ed3795409f2dull},
    {"BA FM Bass", 0xdb4512b0784f2a90ull},
    {"KY Glass Bell", 0x9112811c8b922d88ull},
    {"PD Warm Pad", 0xbd71a9521240edd8ull},
    {"KY Drawbar Organ", 0x5253f61b36869235ull},
    {"PL Pluck Delay", 0x54bdfad9cabec732ull},
    {"PD Crystal Motion", 0xd9dc08d9966b8b31ull},
    {"BA Reese Bass", 0xcf6ca8ebd4d2c4ebull},
    {"BA Wobble Bass", 0x052b5bd725de4ac5ull},
    {"PD Shimmer Pad", 0x2941affb37adab8aull},
    {"SQ Trance Gate", 0x2c09c3637889d5dcull},
    {"LD FM Brass", 0x7e6f2b150bfa91d8ull},
    {"FX Industrial Hit", 0xa1cd1a7b125f51d8ull},
    {"LD Folded Lead", 0x8aea699d01cb8c70ull},
    {"BA Fuzz Bass", 0x99895bc872e2c284ull},
    {"PL Comb String", 0x7069c6afdc9fa794ull},
    {"PL Chip Pluck", 0xc4274cc781d3610dull},
    {"SQ Step Filter", 0xd511c60c618cd3b7ull},
    {"SQ Glass Steps", 0x94a0df1834a25bceull},
    {"LD Vowel Lead", 0x28b0b789d131828full},
    {"FX Laser Zap", 0xb500242d44284008ull},
    {"FX Noise Riser", 0x0e08d1adac580a1dull},
};

// FNV-1a over the preset's name, the params it sets (by id, so params added later leave it alone, in id order, each
// with the value it ends up with), its nodes' places and its macros' names. Whole numbers go in exactly, other values
// rounded to a float: a last bit that two C libraries' log() round apart doesn't count.
uint64_t presetChecksum(const FactoryPreset &preset) {
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](const void *data, size_t size) {
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ static_cast<const unsigned char *>(data)[i]) * 1099511628211ull;
    };
    auto addCount = [&](size_t count) {
        const uint32_t n = static_cast<uint32_t>(count);
        add(&n, sizeof n);
    };
    auto addText = [&](const std::string &text) { add(text.c_str(), text.size() + 1); };
    addText(preset.name);
    std::map<clap_id, double> values; // a param set twice ends up with the later value
    for (const auto &[index, value] : preset.changes)
        values[idOfIndex(index)] = value;
    addCount(values.size());
    for (const auto &[id, value] : values) {
        add(&id, sizeof id);
        if (std::abs(value) < 1e15 && value == std::round(value)) {
            const int64_t whole = static_cast<int64_t>(value);
            add(&whole, sizeof whole);
        } else {
            const float rounded = static_cast<float>(value);
            add(&rounded, sizeof rounded);
        }
    }
    addCount(preset.nodes.size());
    for (const auto &[x, y] : preset.nodes) {
        add(&x, sizeof x);
        add(&y, sizeof y);
    }
    addCount(preset.macros.size());
    for (const std::string &macro : preset.macros)
        addText(macro);
    return hash;
}

void presetContentChecks() {
    std::printf("factory preset contents:\n");
    const std::vector<FactoryPreset> presets = factoryPresets();
    bool same = true;
    for (const FactoryPreset &preset : presets) {
        const uint64_t checksum = presetChecksum(preset);
        const PinnedPreset *pinned = std::find_if(std::begin(kPresetChecksums), std::end(kPresetChecksums),
                                                  [&](const PinnedPreset &p) { return preset.name == p.name; });
        if (pinned != std::end(kPresetChecksums) && pinned->checksum == checksum)
            continue;
        std::printf("      %-20s %s: its checksum is now 0x%016llxull\n", preset.name.c_str(),
                    pinned == std::end(kPresetChecksums) ? "is new" : "changed",
                    static_cast<unsigned long long>(checksum));
        same = false;
    }
    for (const PinnedPreset &pinned : kPresetChecksums) {
        auto named = [&](const FactoryPreset &p) { return p.name == pinned.name; };
        if (std::none_of(presets.begin(), presets.end(), named)) {
            std::printf("      %-20s is gone\n", pinned.name);
            same = false;
        }
    }
    check(same, "every factory preset holds what its checksum pins (one changed on purpose: update kPresetChecksums)");
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
    presetContentChecks();

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
