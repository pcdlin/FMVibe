#include "dx7.h"

#include "algorithms.h"
#include "modulationmodel.h"
#include "routing.h"
#include "routingmodel.h"

#include "../state.h"
#include "../uistrings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <numbers>
#include <utility>

namespace fmvibe::gui::dx7 {

using namespace detail;

namespace {

// Each field's highest value, an operator's then the voice's (its name's characters are 7-bit).
constexpr uint8_t kOpMax[kOpBytes] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 3, 3, 7, 3, 7, 99, 1, 31, 99, 14};
constexpr uint8_t kVoiceMax[kVoiceBytes - kVoiceFields] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7, 1, 99, 99, 99, 99,
                                                           1,  5,  7,  48, 127, 127, 127, 127, 127, 127, 127, 127,
                                                           127, 127};
static_assert(kVoiceFields + Name + kNameBytes == kVoiceBytes);

// --- The DX7's curves, as the DX7 behaves: its level scale, its envelope's speeds and attack, its rate scaling, its
// LFO's delay, fixed frequencies and keyboard scaling follow what Raph Levien's reverse engineering of the chip found
// (published with msfa, Apache 2.0); the rest (the velocity curve, the LFO's speeds, the pitch envelope's levels and
// rates, the waits of segments that stay put, the pitch and amp modulation sensitivities) are fits of our own to its
// measured behaviour. No code is taken from msfa or from Dexed. Its levels and its envelope run in steps of 1/256 of a
// doubling of the operator's gain (about 0.0235 dB), "units" below; an operator at full output and full envelope
// stands at 3840 of them.

// An output or envelope level (0-99) on the DX7's level scale (0-127): 28 + the level from 20 up; below, steeper, a
// curve of ours from 0 up to 46 at 19, near the DX7's (whose levels there are 60 dB and more down).
int scaledLevel(int level) {
    level = std::clamp(level, 0, 99);
    return level >= 20 ? 28 + level : static_cast<int>(std::lround(46.0 * std::pow(level / 19.0, 0.7)));
}
// Where an operator at output level `out` (in units: its scaled level times 32) stands at envelope level `level`.
double envUnits(int level, double out) {
    return std::max(16.0, (scaledLevel(level) >> 1) * 64.0 + out - 4256.0);
}
// The gain of an operator standing at `units`: 2 at full level and envelope, a modulator then putting two whole cycles
// of phase (4 pi) into its carrier.
double unitGain(double units) {
    return std::exp2(units / 256.0 - 14.0);
}
// The velocity curve, 0-127: at sensitivity s, a velocity moves an operator 2 s times this many units (see
// velocityGain()); 15 up at 127, about none at 100, 246 down at 0.
double velocityValue(int velocity) {
    return 15.0 - 261.0 * (1.0 - std::pow(std::clamp(velocity, 0, 127) / 126.0, 0.3));
}
// The gain velocity gives an operator of sensitivity 0-7, against a note at velocity 100.
double velocityGain(int velocity, int sensitivity) {
    return std::exp2(sensitivity * (velocityValue(velocity) - velocityValue(100)) / 128.0);
}

// The LFO's rate, 0-99, in Hz: 0.06 Hz at 0, near enough linear to 10 Hz at 63, then faster to 49 Hz at 99.
double lfoHz(int speed) {
    if (speed <= 0)
        return 0.0625;
    if (speed <= 63)
        return 0.1605 * speed - 0.03;
    const double d = speed - 64.0;
    return 11.25 + 0.755 * d + 0.00944 * d * d;
}
// The LFO's delay, 0-99: how long it waits silent after a note starts, then how long it takes to fade in (0, 0 for
// none).
std::pair<double, double> lfoDelay(int delay) {
    const int a = 99 - std::clamp(delay, 0, 99);
    if (a == 99)
        return {0.0, 0.0};
    const int wait = (16 + (a & 15)) << (1 + (a >> 4));
    const int fade = std::max(0x80, wait & 0xff80);
    constexpr double kUnit = 85.25; // seconds, for a step of 1
    return {kUnit / wait, kUnit / fade};
}
// How far the LFO bends the pitch at its peak, in semitones, at pitch mod depth 0-99 and sensitivity 0-7: an octave
// at full depth and sensitivity 7, each sensitivity below it about 0.6 as far (a curve of ours), 1 half of 2.
double lfoPitchSemitones(int depth, int sensitivity) {
    sensitivity = std::clamp(sensitivity, 0, 7);
    const double sens = sensitivity == 0 ? 0.0 : std::pow(0.6, 7 - sensitivity) * (sensitivity == 1 ? 0.84 : 1.0);
    return 12.0 * std::clamp(depth, 0, 99) / 99.0 * sens;
}
// How far the LFO turns an operator down at its trough, as a gain (1 for not at all), at amp mod depth 0-99 and
// sensitivity 0-3: our own model, in dB growing with the depth, as far as silence at full depth and sensitivity 3, a
// gentle tremolo at 1 and 2 (4 and 8 dB at full depth).
double lfoAmpGain(int depth, int sensitivity) {
    static constexpr double kFullDb[4] = {0.0, 4.0, 8.0, 96.0};
    const double db = kFullDb[std::clamp(sensitivity, 0, 3)] * std::clamp(depth, 0, 99) / 99.0;
    return db <= 0.0 ? 1.0 : std::pow(10.0, -db / 20.0);
}

// The pitch envelope's levels, 0-99, in semitones: 50 in tune, 0.375 a level from 17 to 85, steeper out to four
// octaves down at 0 and up at 99.
double pitchEnvSemitones(int level) {
    double steps;
    if (level < 17) {
        const double d = 17.0 - level;
        steps = -33.0 - (0.3 * d * d + 0.5 * d);
    } else if (level > 85) {
        const double d = level - 85.0;
        steps = 35.0 + 0.4 * d * d + 0.9 * d;
    } else {
        steps = level - 50.0;
    }
    return steps * 12.0 / 32.0;
}
// How fast the pitch envelope moves at rate 0-99, in octaves a second.
double pitchEnvSpeed(int rate) {
    const double steps = rate <= 30 ? 1.0 + 0.55 * rate : 17.0 * std::exp2((rate - 30) / 18.5);
    return steps / 21.3;
}

// Detune 0-14 in cents (7 in tune): about 1.2 cents a step around C4, where the DX7's steps narrow as notes rise.
constexpr double kDetuneCents = 1.2;

// FM Amount, at its top: so a modulator at the DX7's full level, 4 pi, fits a route's 8 radians.
constexpr double kFmAmount = 2.0;

// --- The patch, edited through the editor's models as the editor edits one.

class PatchController final : public Controller {
public:
    PatchController(ParamValues &values, UiLayout &layout) : values_(values), layout_(layout) {}
    double value(int index) const override { return values_[index]; }
    void telemetry(Telemetry &) const override {}
    UiLayout &layout() override { return layout_; }
    void layoutChanged() override {}
    void playNote(int, bool, int) override {}
    void setScope(int, int) override {}
    int oversampling() const override { return kDefaultOversampling; }
    void setOversampling(int) override {}
    bool mpe() const override { return false; }
    void setMpe(bool) override {}
    int mpeBendRange() const override { return kDefaultMpeBendRange; }
    void setMpeBendRange(int) override {}
    EditorSettings editorSettings() const override { return {}; }

protected:
    void performBeginEdit(int) override {}
    void performSetValue(int index, double value) override { values_[index] = value; }
    void performEndEdit(int) override {}
    void storeEditorSettings(const EditorSettings &) override {}

private:
    ParamValues &values_;
    UiLayout &layout_;
};

// b + d * envCurve(x, c) closest to `want(x)` over the velocities 1-127 (x their share of 127), each held to 0..1, its
// error weighed by how loud it is, so the quiet end counts as much as the loud one: {b, d, c}.
std::array<double, 3> fitVelocity(const std::function<double(int)> &want) {
    std::array<double, 3> best{want(100), 0.0, 0.0};
    double least = 1e300;
    for (int ci = -50; ci <= 50; ++ci) {
        const double c = ci / 50.0;
        double sw = 0, sh = 0, st = 0, shh = 0, sht = 0;
        for (int v = 1; v <= 127; ++v) {
            const double h = envCurve(v / 127.0, c), y = want(v), w = 1.0 / ((y + 0.03) * (y + 0.03));
            sw += w, sh += w * h, st += w * y, shh += w * h * h, sht += w * h * y;
        }
        const double det = sw * shh - sh * sh;
        if (std::abs(det) < 1e-12)
            continue;
        const double d = (sw * sht - sh * st) / det, b = (st - d * sh) / sw;
        double err = 0;
        for (int v = 1; v <= 127; ++v) {
            const double y = want(v), got = std::clamp(b + d * envCurve(v / 127.0, c), 0.0, 1.0);
            err += (got - y) * (got - y) / ((y + 0.03) * (y + 0.03));
        }
        if (err < least) {
            least = err;
            best = {b, d, c};
        }
    }
    return best;
}

// The next free slot of `source`, or -1.
int freeSlot(const ParamValues &p, int source) {
    for (int slot = 0; slot < kSlots; ++slot)
        if (p[slotTargetIndex(source, slot)] <= 0.0)
            return slot;
    return -1;
}
// A modulation of `target` from `source`, if it has a slot free.
void modulate(ParamValues &p, int source, int target, double depth, double curve = 0.0, int aux = -1) {
    const int slot = freeSlot(p, source);
    if (slot < 0 || target < 0)
        return;
    p[slotTargetIndex(source, slot)] = slotTargetValue(target);
    p[slotAmountIndex(source, slot)] = std::clamp(depth, -1.0, 1.0);
    p[slotCurveIndex(source, slot)] = std::clamp(curve, -1.0, 1.0);
    if (aux >= 0)
        p[slotAuxIndex(source, slot)] = aux + 1.0;
}

void setPoint(ParamValues &p, int base, int point, double x, double y, double curve) {
    p[shapeFieldIndex(base, point, PointField::X)] = std::clamp(x, 0.0, 1.0);
    p[shapeFieldIndex(base, point, PointField::Y)] = std::clamp(y, -1.0, 1.0);
    p[shapeFieldIndex(base, point, PointField::Curve)] = std::clamp(curve, -1.0, 1.0);
}

} // namespace

std::string Voice::name() const {
    std::string name;
    for (int i = 0; i < kNameBytes; ++i) {
        const uint8_t c = data[kVoiceFields + Name + i];
        name += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : ' ';
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    return name;
}

Voice clamped(const Voice &voice) {
    Voice v = voice;
    for (int i = 0; i < kVoiceFields; ++i)
        v.data[i] = std::min(v.data[i], kOpMax[i % kOpBytes]);
    for (int i = kVoiceFields; i < kVoiceBytes; ++i)
        v.data[i] = std::min(v.data[i], kVoiceMax[i - kVoiceFields]);
    return v;
}

Voice initVoice() {
    Voice v;
    for (int op = 1; op <= 6; ++op) {
        uint8_t *o = v.data.data() + (6 - op) * kOpBytes;
        for (int i = 0; i < 4; ++i)
            o[Rate1 + i] = 99, o[Level1 + i] = i < 3 ? 99 : 0;
        o[BreakPoint] = 39; // C3
        o[OutputLevel] = op == 1 ? 99 : 0;
        o[Coarse] = 1;
        o[Detune] = 7;
    }
    uint8_t *f = v.data.data() + kVoiceFields;
    for (int i = 0; i < 4; ++i)
        f[PitchRate1 + i] = 99, f[PitchLevel1 + i] = 50;
    f[OscKeySync] = 1;
    f[LfoSpeed] = 35;
    f[LfoKeySync] = 1;
    f[PitchModSens] = 3;
    f[Transpose] = 24;
    const char *name = "INIT VOICE";
    for (int i = 0; i < kNameBytes; ++i)
        f[Name + i] = static_cast<uint8_t>(name[i]);
    return v;
}

Voice unpackVoice(const uint8_t *packed) {
    Voice v;
    for (int op = 0; op < 6; ++op) {
        const uint8_t *p = packed + 17 * op;
        uint8_t *o = v.data.data() + kOpBytes * op;
        std::copy(p, p + 11, o); // the rates, levels, break point and depths
        o[LeftCurve] = p[11] & 3;
        o[RightCurve] = (p[11] >> 2) & 3;
        o[RateScaling] = p[12] & 7;
        o[Detune] = (p[12] >> 3) & 15;
        o[AmpModSens] = p[13] & 3;
        o[VelocitySens] = (p[13] >> 2) & 7;
        o[OutputLevel] = p[14];
        o[OscMode] = p[15] & 1;
        o[Coarse] = (p[15] >> 1) & 31;
        o[Fine] = p[16];
    }
    uint8_t *f = v.data.data() + kVoiceFields;
    std::copy(packed + 102, packed + 110, f); // the pitch envelope
    f[AlgorithmNumber] = packed[110] & 31;
    f[Feedback] = packed[111] & 7;
    f[OscKeySync] = (packed[111] >> 3) & 1;
    f[LfoSpeed] = packed[112];
    f[LfoDelay] = packed[113];
    f[LfoPitchDepth] = packed[114];
    f[LfoAmpDepth] = packed[115];
    f[LfoKeySync] = packed[116] & 1;
    f[LfoWave] = (packed[116] >> 1) & 7;
    f[PitchModSens] = (packed[116] >> 4) & 7;
    f[Transpose] = packed[117];
    std::copy(packed + 118, packed + 128, f + Name);
    return clamped(v);
}

std::array<uint8_t, kPackedBytes> packVoice(const Voice &voice) {
    const Voice v = clamped(voice);
    std::array<uint8_t, kPackedBytes> packed{};
    for (int op = 0; op < 6; ++op) {
        uint8_t *p = packed.data() + 17 * op;
        const uint8_t *o = v.data.data() + kOpBytes * op;
        std::copy(o, o + 11, p);
        p[11] = static_cast<uint8_t>(o[LeftCurve] | o[RightCurve] << 2);
        p[12] = static_cast<uint8_t>(o[RateScaling] | o[Detune] << 3);
        p[13] = static_cast<uint8_t>(o[AmpModSens] | o[VelocitySens] << 2);
        p[14] = o[OutputLevel];
        p[15] = static_cast<uint8_t>(o[OscMode] | o[Coarse] << 1);
        p[16] = o[Fine];
    }
    const uint8_t *f = v.data.data() + kVoiceFields;
    std::copy(f, f + 8, packed.data() + 102);
    packed[110] = f[AlgorithmNumber];
    packed[111] = static_cast<uint8_t>(f[Feedback] | f[OscKeySync] << 3);
    packed[112] = f[LfoSpeed];
    packed[113] = f[LfoDelay];
    packed[114] = f[LfoPitchDepth];
    packed[115] = f[LfoAmpDepth];
    packed[116] = static_cast<uint8_t>(f[LfoKeySync] | f[LfoWave] << 1 | f[PitchModSens] << 4);
    packed[117] = f[Transpose];
    std::copy(f + Name, f + Name + kNameBytes, packed.data() + 118);
    return packed;
}

namespace {

// A dump's message: its header (format and byte count), the data, its checksum and the end.
std::vector<uint8_t> dumpMessage(uint8_t format, const std::vector<uint8_t> &data) {
    std::vector<uint8_t> m{0xf0, 0x43, 0x00, format, static_cast<uint8_t>(data.size() >> 7 & 0x7f),
                           static_cast<uint8_t>(data.size() & 0x7f)};
    m.insert(m.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (uint8_t b : data)
        sum += b;
    m.push_back(static_cast<uint8_t>((128 - (sum & 0x7f)) & 0x7f));
    m.push_back(0xf7);
    return m;
}

} // namespace

std::vector<uint8_t> bankSysex(const std::vector<Voice> &voices) {
    std::vector<uint8_t> data;
    for (int i = 0; i < kBankVoices; ++i) {
        const auto packed = packVoice(i < static_cast<int>(voices.size()) ? voices[i] : initVoice());
        data.insert(data.end(), packed.begin(), packed.end());
    }
    return dumpMessage(9, data);
}

std::vector<uint8_t> voiceSysex(const Voice &voice) {
    const Voice v = clamped(voice);
    return dumpMessage(0, std::vector<uint8_t>(v.data.begin(), v.data.end()));
}

std::vector<Voice> parseSysex(const uint8_t *bytes, size_t size) {
    std::vector<Voice> voices;
    auto bank = [&](const uint8_t *data) {
        for (int i = 0; i < kBankVoices; ++i)
            voices.push_back(unpackVoice(data + kPackedBytes * i));
    };
    bool messages = false;
    for (size_t at = 0; at < size;) {
        if (bytes[at] != 0xf0) {
            ++at;
            continue;
        }
        messages = true;
        size_t end = at + 1;
        while (end < size && bytes[end] != 0xf7)
            ++end;
        const uint8_t *m = bytes + at;
        const size_t length = end - at; // without its end
        // Yamaha's, a bulk dump (sub-status 0, any channel): a 32-voice bank (format 9) or a single voice (format 0).
        if (length >= 6 && m[1] == 0x43 && (m[2] & 0xf0) == 0) {
            const size_t data = length - 6;
            if (m[3] == 9 && data >= kBankVoices * kPackedBytes) {
                bank(m + 6);
            } else if (m[3] == 0 && data >= kVoiceBytes) {
                Voice v;
                std::copy(m + 6, m + 6 + kVoiceBytes, v.data.begin());
                voices.push_back(clamped(v));
            }
        }
        at = end + 1;
    }
    if (!messages && size == kBankVoices * kPackedBytes)
        bank(bytes);
    return voices;
}

void toPatch(const Voice &dxVoice, ParamValues &p, UiLayout &layout) {
    const Voice voice = clamped(dxVoice);
    const ParamTable &table = paramTable();
    for (int i = 0; i < kParamCount; ++i)
        p[i] = table[i].def;
    layout = UiLayout{};
    PatchController controller(p, layout);
    RoutingModel routing(controller);
    ModulationModel modulation(controller);

    const Algorithm &algorithm = kAlgorithms[voice.voice(AlgorithmNumber)];
    routing.applyAlgorithm(voice.voice(AlgorithmNumber)); // the six operators' 32 come first
    p[paramIndex(GlobalParam::FmAmount)] = kFmAmount;
    p[paramIndex(GlobalParam::Pitch)] = voice.voice(Transpose) - 24.0;

    // Each operator's gain at its full envelope (2 at the top, see unitGain()), and what it modulates and puts out.
    std::array<double, kOps> full{};
    std::array<std::vector<std::pair<int, double>>, kOps> levels; // (param, value at velocity 100)
    double heard = 0.0;
    for (int op = 0; op < kOps; ++op) {
        const int dxOp = op + 1;
        const double out = scaledLevel(voice.op(dxOp, OutputLevel)) * 32.0;
        full[op] = voice.op(dxOp, OutputLevel) > 0 ? unitGain(envUnits(99, out)) : 0.0;
        if (algorithm.carriers >> op & 1) {
            const double level = full[op] / 2.0;
            levels[op].push_back({outParam(op), level});
            heard += level * level;
        }
        for (int to = 0; to < kOps; ++to) {
            const int fb = voice.voice(Feedback);
            const bool feedback = op == algorithm.feedbackFrom && to == algorithm.feedbackTo;
            double amount = 0.0;
            if (feedback && to == op) { // phase from the average of its last two samples, 2^(fb - 8) of it
                amount = fb > 0 ? std::sqrt(std::exp2(fb - 7.0) * full[op] / kFmAmount) : 0.0;
                p[feedbackModeIndex(op)] = static_cast<double>(FeedbackMode::Dx7); // at the DX7's rate, as it runs
            }
            else if (feedback) // the same, into an operator up its stack
                amount = fb > 0 ? std::sqrt(2.0 * std::numbers::pi * std::exp2(fb - 8.0) * full[op] / (8.0 * kFmAmount))
                                : 0.0;
            else if (algorithm.mods[to] >> op & 1) // its gain, in cycles of phase, into a route's 8 radians
                amount = std::sqrt(2.0 * std::numbers::pi * full[op] / (8.0 * kFmAmount));
            else
                continue;
            levels[op].push_back({routeParam(op, to), std::min(amount, 1.0)});
        }
        for (const auto &[param, value] : levels[op])
            p[param] = value;

        // Its pitch.
        const int coarse = voice.op(dxOp, Coarse), fine = voice.op(dxOp, Fine), detune = voice.op(dxOp, Detune);
        if (voice.op(dxOp, OscMode) == 1) {
            double hz = std::pow(10.0, (coarse & 3) + fine / 100.0);
            if (detune > 7)
                hz *= std::exp2((detune - 7) * 13457.0 / (1 << 24));
            const bool high = hz > kMaxFixedFreq;
            p[paramIndex(op, OpParam::Fixed)] = 1.0;
            p[paramIndex(op, OpParam::FixedMult)] = high ? 4.0 : 3.0; // x10, x1
            p[paramIndex(op, OpParam::FixedFreq)] = normFromFixedFreq(high ? hz / 10.0 : hz);
        } else {
            const double ratio = (coarse == 0 ? 0.5 : coarse) * (1.0 + fine / 100.0);
            p[paramIndex(op, OpParam::Ratio)] = normFromRatio(ratio);
            p[paramIndex(op, OpParam::Detune)] = (detune - 7) * kDetuneCents;
        }
        if (voice.voice(OscKeySync) == 0) // free-running oscillators: each note starts them anywhere
            p[paramIndex(op, OpParam::PhaseRandom)] = 1.0;

        // Its keyboard level scaling: the DX7 moves an operator's level from 17 notes over its break point (of the key
        // played, transposed, where the operator's Key Break is of the key played) in steps of three keys, a linear
        // side 0.0803 levels (0.75 dB) a step per unit of depth, an exponential one from 1/8 of that, doubling all it
        // moved every octave past the first, as keyScaleDb()'s Exp does. A side that lifts the level stops at the
        // DX7's top level (127 on its scale): held so it reaches no higher at the end of the keys on its side.
        const double breakNote = voice.op(dxOp, BreakPoint) + 17.0 - (voice.voice(Transpose) - 24.0);
        auto side = [&](int depth, int curve, double octaves) {
            const bool exp = curve == 1 || curve == 2; // -LIN, -EXP, +EXP, +LIN
            double db = 0.75 * depth * (exp ? 4.0 * 329.0 / 32768.0 : 4.0 * 329.0 / 4096.0); // an octave out
            if (curve >= 2) {
                const double headroom = 0.75 * (127 - scaledLevel(voice.op(dxOp, OutputLevel)));
                const double most = exp && octaves > 1.0 ? std::exp2(octaves - 1.0) : octaves;
                if (most > 0.0)
                    db = std::min(db, headroom / most);
            } else {
                db = -db;
            }
            return std::pair{std::clamp(db, -kMaxKeyDepthDb, kMaxKeyDepthDb),
                             static_cast<double>(exp ? KeyCurve::Exp : KeyCurve::Linear)};
        };
        const auto [leftDb, leftCurve] = side(voice.op(dxOp, LeftDepth), voice.op(dxOp, LeftCurve),
                                              std::max(breakNote - 21.0, 0.0) / 12.0);
        const auto [rightDb, rightCurve] = side(voice.op(dxOp, RightDepth), voice.op(dxOp, RightCurve),
                                                std::max(108.0 - breakNote, 0.0) / 12.0);
        p[paramIndex(op, OpParam::KeyBreak)] = std::clamp(breakNote, 0.0, 127.0);
        p[paramIndex(op, OpParam::KeyLeftDepth)] = leftDb;
        p[paramIndex(op, OpParam::KeyRightDepth)] = rightDb;
        p[paramIndex(op, OpParam::KeyLeftCurve)] = leftCurve;
        p[paramIndex(op, OpParam::KeyRightCurve)] = rightCurve;

        // Its envelope, as the DX7's: its rates and levels, and its rate scaling (see RateEnvelope).
        p[paramIndex(op, OpParam::EnvMode)] = static_cast<double>(OpEnvMode::Rates);
        for (int k = 0; k < 4; ++k) {
            p[paramIndex(op, static_cast<OpParam>(static_cast<int>(OpParam::EnvRate1) + k))] =
                voice.op(dxOp, static_cast<OpField>(Rate1 + k));
            p[paramIndex(op, static_cast<OpParam>(static_cast<int>(OpParam::EnvLevel1) + k))] =
                voice.op(dxOp, static_cast<OpField>(Level1 + k));
        }
        p[paramIndex(op, OpParam::EnvRateScale)] = voice.op(dxOp, RateScaling);
    }
    // The carriers together about as loud as one at full level.
    p[paramIndex(GlobalParam::Volume)] = table[paramIndex(GlobalParam::Volume)].def -
                                          10.0 * std::log10(std::max(heard, 1.0));

    // Velocity: each operator's levels fitted to the DX7's velocity curve, through a slot of the velocity source each.
    for (int op = 0; op < kOps; ++op) {
        const int sens = voice.op(op + 1, VelocitySens);
        if (sens == 0)
            continue;
        for (auto &[param, value] : levels[op]) {
            const bool out = param == outParam(op);
            const double at = value; // a route's amount goes as the square root of its depth
            const auto fit = fitVelocity([&](int v) {
                const double gain = velocityGain(v, sens);
                return std::min(at * (out ? gain : std::sqrt(gain)), 1.0);
            });
            if (freeSlot(p, kVelocitySource) < 0)
                break;
            p[param] = std::clamp(fit[0], 0.0, 1.0);
            modulate(p, kVelocitySource, param, fit[1], fit[2]);
            value = std::clamp(fit[0] + fit[1] * envCurve(100.0 / 127.0, fit[2]), 0.0, 1.0);
        }
    }

    // The LFO: one for the pitch, one for the level (each source swinging its targets one way or both), faded in
    // after the delay by a one-shot curve as their aux.
    const int wave = voice.voice(LfoWave);
    const double hz = lfoHz(voice.voice(LfoSpeed));
    const bool free = voice.voice(LfoKeySync) == 0;
    const double pitch = lfoPitchSemitones(voice.voice(LfoPitchDepth), voice.voice(PitchModSens));
    bool amp = false;
    for (int op = 0; op < kOps; ++op)
        amp = amp || (voice.op(op + 1, AmpModSens) > 0 && voice.voice(LfoAmpDepth) > 0 && full[op] > 0.0);
    int delay = -1;
    if (const auto [wait, fade] = lfoDelay(voice.voice(LfoDelay)); (pitch > 0.0 || amp) && wait + fade > 0.0) {
        delay = modulation.addSource(SourceType::Curve);
        if (delay >= 0) {
            // Silent, fading in and then holding in a loop at its end (the last tenth): one for every voice, every key
            // starting it again, as the DX7's delay is.
            p[paramIndex(delay, SourceParam::Sync)] = 0.0;
            p[paramIndex(delay, SourceParam::Global)] = 1.0;
            const double cycle = (wait + fade) / 0.9;
            p[paramIndex(delay, SourceParam::Rate)] = std::clamp(normFromRate(1.0 / cycle), 0.0, 1.0);
            p[paramIndex(delay, SourceParam::LoopStart)] = 0.9;
            p[paramIndex(delay, SourceParam::LoopEnd)] = 1.0;
            const int shape = shapeCountIndex(delay);
            p[shape] = 4.0;
            setPoint(p, shape, 0, 0.0, -1.0, 0.0);
            setPoint(p, shape, 1, 0.9 * wait / (wait + fade), -1.0, 0.0);
            setPoint(p, shape, 2, 0.9, 1.0, 0.0);
            setPoint(p, shape, 3, 1.0, 1.0, 0.0);
        }
    }
    auto lfo = [&](bool bipolar) {
        const bool random = wave == 5;
        // The DX7 turns an operator down the most where its LFO is lowest, so the level's LFO is the pitch's upside
        // down, which the slot's depth, turning one way only, can't do: half a cycle on for the waves that are
        // symmetric (below), an LFO's Saw, rising, for a saw down, and for a saw up a Curve source falling from its top
        // to its foot across the cycle, unipolar.
        const bool fall = !bipolar && wave == 2;
        const int s = modulation.addSource(random ? SourceType::Random : fall ? SourceType::Curve : SourceType::Lfo);
        if (s < 0)
            return s;
        static constexpr LfoShape kShapes[5] = {LfoShape::Triangle, LfoShape::Saw, LfoShape::Saw, LfoShape::Square,
                                                LfoShape::Sine};
        p[paramIndex(s, SourceParam::Sync)] = 0.0;
        p[paramIndex(s, SourceParam::Rate)] = std::clamp(normFromRate(hz), 0.0, 1.0);
        // Key sync on, every note starts it again for every voice, as the DX7's one LFO is; off, it runs free.
        p[paramIndex(s, free ? SourceParam::Free : SourceParam::Global)] = 1.0;
        if (fall) {
            const int shape = shapeCountIndex(s);
            p[shape] = 2.0;
            setPoint(p, shape, 0, 0.0, 1.0, 0.0);
            setPoint(p, shape, 1, 1.0, -1.0, 0.0);
        } else if (random) {
            p[paramIndex(s, SourceParam::RandomChaos)] = 1.0;
            p[paramIndex(s, SourceParam::RandomSmooth)] = 0.0;
            p[paramIndex(s, SourceParam::Bipolar)] = bipolar ? 1.0 : 0.0;
        } else {
            p[paramIndex(s, SourceParam::Shape)] = static_cast<double>(kShapes[wave]);
            p[paramIndex(s, SourceParam::Unipolar)] = bipolar ? 0.0 : 1.0;
            // The level's LFO turns an operator down as the pitch's falls: half a cycle on, where that is the same.
            if (!bipolar && wave != 1 && wave != 2)
                p[paramIndex(s, SourceParam::Phase)] = 0.5;
        }
        return s;
    };
    // What bends the pitch bends the fixed operators too, as on the DX7: their own Pitch, which the global one leaves
    // alone (as wide, 96 semitones, so the same depth).
    std::vector<int> pitches{paramIndex(GlobalParam::Pitch)};
    for (int op = 0; op < kOps; ++op)
        if (voice.op(op + 1, OscMode) == 1)
            pitches.push_back(paramIndex(op, OpParam::Transpose));
    if (pitch > 0.0)
        if (const int s = lfo(true); s >= 0) // the whole swing, half each way, on Pitch's 96 semitones
            for (int target : pitches)
                modulate(p, s, target, (wave == 1 ? -2.0 : 2.0) * pitch / 96.0, 0.0, delay);
    // The pitch bend, by Bend Range, on the fixed operators, which it doesn't bend by itself.
    for (size_t k = 1; k < pitches.size(); ++k)
        modulate(p, kBendSource, pitches[k], 2.0 * p[paramIndex(GlobalParam::BendRange)] / 96.0);
    if (amp)
        if (const int s = lfo(false); s >= 0)
            for (int op = 0; op < kOps; ++op) {
                const double gain = lfoAmpGain(voice.voice(LfoAmpDepth), voice.op(op + 1, AmpModSens));
                if (gain >= 1.0)
                    continue;
                for (const auto &[param, value] : levels[op]) {
                    const bool out = param == outParam(op);
                    modulate(p, s, param, -(1.0 - (out ? gain : std::sqrt(gain))) * value, 0.0, delay);
                }
            }

    // The pitch envelope: a curve held at L3 while the key is down, on Pitch, whose 96 semitones it swings half each
    // way, bipolar.
    bool bends = false;
    for (int i = 0; i < 4; ++i)
        bends = bends || voice.voice(static_cast<VoiceField>(PitchLevel1 + i)) != 50;
    if (bends) {
        const int s = modulation.addSource(SourceType::Curve);
        if (s >= 0) {
            std::array<double, 5> semis{}, at{};
            semis[0] = pitchEnvSemitones(voice.voice(static_cast<VoiceField>(PitchLevel1 + 3)));
            for (int i = 0; i < 4; ++i) {
                semis[i + 1] = pitchEnvSemitones(voice.voice(static_cast<VoiceField>(PitchLevel1 + i)));
                const double octaves = std::abs(semis[i + 1] - semis[i]) / 12.0;
                at[i + 1] = at[i] + octaves / pitchEnvSpeed(voice.voice(static_cast<VoiceField>(PitchRate1 + i)));
            }
            const double length = std::clamp(at[4], 1.0 / kMaxRate, 1.0 / kMinRate);
            const double span = std::max(at[4], 1e-9);
            const int shape = shapeCountIndex(s);
            p[shape] = 4.0; // the last segment wraps round to the first point, L4
            for (int i = 0; i < 4; ++i)
                setPoint(p, shape, i, at[4] > 0.0 ? at[i] / span : i / 4.0, semis[i] / 48.0, 0.0);
            const double hold = at[4] > 0.0 ? at[3] / span : 0.75;
            p[paramIndex(s, SourceParam::Unipolar)] = 0.0;
            p[paramIndex(s, SourceParam::Sync)] = 0.0;
            p[paramIndex(s, SourceParam::Rate)] = std::clamp(normFromRate(1.0 / length), 0.0, 1.0);
            p[paramIndex(s, SourceParam::CurveSustain)] = 1.0;
            p[paramIndex(s, SourceParam::LoopStart)] = hold;
            p[paramIndex(s, SourceParam::LoopEnd)] = hold;
            for (int target : pitches)
                modulate(p, s, target, 1.0);
        }
    }
    controller.clearHistory();
}

std::vector<Preset> presetsOf(const uint8_t *bytes, size_t size, const std::string &bank) {
    const std::vector<Voice> voices = parseSysex(bytes, size);
    std::vector<Preset> presets;
    auto values = std::make_unique<ParamValues>();
    for (size_t i = 0; i < voices.size(); ++i) {
        UiLayout layout;
        toPatch(voices[i], *values, layout);
        const std::string number = (i + 1 < 10 ? "0" : "") + std::to_string(i + 1);
        std::string source = bank.substr(0, kMaxPresetDescription / 2);
        layout.presetInfo.description = trf(Str::PresetDx7Voice, {number, source});
        // Its name in a file's: what a file's name can't hold replaced, and its number first, so two voices of a name
        // stay apart and the bank's order holds.
        std::string name = voices[i].name();
        for (char &c : name)
            if (std::strchr("\\/:*?\"<>|", c))
                c = '-';
        while (!name.empty() && (name.back() == '.' || name.back() == ' '))
            name.pop_back();
        Preset preset{name.empty() ? number : number + " " + name, {}};
        clap_ostream_t out = memoryOut(preset.bytes);
        if (writePatch(&out, *values, layout, true))
            presets.push_back(std::move(preset));
    }
    return presets;
}

} // namespace fmvibe::gui::dx7
