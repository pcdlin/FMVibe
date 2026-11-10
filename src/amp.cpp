#include "amp.h"

#include <complex>
#include <numbers>

namespace fmvibe {

namespace {

constexpr double kPi = std::numbers::pi;

// tanh, closely and cheaply: a continued fraction, which reaches 1 at 4.97 and is held there past it.
inline double fastTanh(double x) {
    x = std::clamp(x, -4.97, 4.97);
    const double x2 = x * x;
    return x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2))) /
           (135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2)));
}

inline double dbGain(double db) {
    return std::pow(10.0, db / 20.0);
}

// A one-pole low-pass's step toward its input, at `hz`.
inline double onePole(double hz, double rate) {
    return 1.0 - std::exp(-2.0 * kPi * std::min(hz, 0.45 * rate) / rate);
}

// The triode-like curve: tanh above the bias, a softer one (by `negative`) below it.
inline double stageCurve(double v, double negative) {
    return v >= 0.0 ? fastTanh(v) : fastTanh(negative * v) / negative;
}
inline double stageSlope(double v, double negative) {
    const double t = std::tanh(v >= 0.0 ? v : negative * v);
    return 1.0 - t * t;
}

// The models. The tone stacks' parts are our own, chosen on the classic passive three-knob circuit: American (scooped,
// Clean and the Fuzz's amp), British (mid-forward, Blues, Crunch and Lead) and a deeper-scooped British (Heavy); the
// Bass model's stack is active.
constexpr AmpToneParts kAmerican{false, 250e3, 1e6, 22e3, 47e3, 270e-12, 22e-9, 22e-9};
constexpr AmpToneParts kBritish{false, 220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9};
constexpr AmpToneParts kScooped{false, 250e3, 1e6, 25e3, 39e3, 330e-12, 22e-9, 47e-9};
constexpr AmpToneParts kActive{true, 0, 0, 0, 0, 0, 0, 0};

// {high-pass Hz, low-pass Hz, gain dB, bias, negative}
constexpr AmpModelSpec kAmpModels[] = {
    // Clean: one gentle stage with headroom to spare, a big power amp.
    {30.0, -6.0, 20.0, 1, {{{40.0, 12000.0, 0.0, 0.08, 0.75}}}, kAmerican, 4000.0, -12.0, 12.0, 0.15, 0.0, -6.0},
    // Blues: two stages, a loose low end, breaking up early, and plenty of sag.
    {50.0, 0.0, 30.0, 2, {{{70.0, 9000.0, 0.0, 0.2, 0.6}, {40.0, 7000.0, 6.0, 0.1, 0.7}}}, kBritish, 3500.0, -9.0,
     15.0, 0.6, 0.0, -13.8},
    // Crunch: two stages, tighter, mid-forward.
    {80.0, 6.0, 36.0, 2, {{{120.0, 10000.0, 0.0, 0.2, 0.6}, {60.0, 7000.0, 6.0, 0.15, 0.7}}}, kBritish, 3800.0, -9.0,
     15.0, 0.4, 0.0, -14.6},
    // Lead: three stages, each cutting a little more top, for a smooth sustain.
    {100.0, 12.0, 44.0, 3,
     {{{150.0, 9000.0, 0.0, 0.25, 0.55}, {100.0, 6500.0, 6.0, 0.15, 0.7}, {60.0, 5500.0, 6.0, 0.1, 0.8}}}, kBritish,
     3800.0, -9.0, 12.0, 0.3, 0.0, -14.1},
    // Heavy: four stages behind a tight input, high gain, little sag.
    {500.0, 18.0, 50.0, 4,
     {{{600.0, 9000.0, 0.0, 0.3, 0.5},
       {250.0, 7000.0, 6.0, 0.2, 0.6},
       {120.0, 6000.0, 6.0, 0.1, 0.7},
       {60.0, 5000.0, 0.0, 0.05, 0.8}}},
     kScooped, 4200.0, -9.0, 12.0, 0.15, 0.0, -10.4},
    // Bass: one stage for grit on top, the lows blended in clean under it, an active stack.
    {25.0, 0.0, 30.0, 1, {{{150.0, 8000.0, 0.0, 0.15, 0.7}}}, kActive, 3000.0, -12.0, 12.0, 0.3, 1.0, -12.3},
    // Fuzz: a fuzz pedal, two hard, lopsided transistor stages, into a clean amp.
    {120.0, 0.0, 44.0, 2, {{{150.0, 8000.0, 0.0, 0.4, 0.25}, {80.0, 4500.0, 12.0, 0.25, 0.9}}}, kAmerican, 4000.0,
     -12.0, 9.0, 0.2, 0.0, -12.1},
};
static_assert(std::size(kAmpModels) == static_cast<size_t>(AmpModel::Count), "a spec for each amp model");

// The cabinets: {low resonance Hz and Q, low-mid bump Hz, dB and Q, breakup peak Hz, dB and Q, the dip above it Hz,
// dB and Q, low-pass Hz}. Closed backs resonate tighter, the bass cabinets reach lower and roll off sooner.
struct CabSpec {
    double hpHz, hpQ, lowHz, lowDb, lowQ, breakHz, breakDb, breakQ, dipHz, dipDb, dipQ, lpHz;
};
constexpr CabSpec kCabs[] = {
    {90.0, 1.2, 220.0, 1.5, 1.0, 2400.0, 4.0, 1.8, 1200.0, -3.0, 1.5, 5200.0}, // 1x12 Open
    {80.0, 1.3, 180.0, 2.5, 1.0, 2200.0, 4.5, 1.8, 1000.0, -3.0, 1.4, 5000.0}, // 2x12 Open
    {75.0, 1.6, 140.0, 3.0, 0.9, 2000.0, 3.5, 1.6, 800.0, -2.5, 1.2, 4600.0},  // 4x12 Closed
    {55.0, 1.2, 120.0, 2.0, 0.8, 2800.0, 3.0, 1.4, 600.0, -2.0, 1.0, 4500.0},  // 4x10 Bass
    {45.0, 1.1, 90.0, 3.0, 0.9, 1600.0, 2.0, 1.4, 700.0, -3.0, 1.2, 3000.0},   // 1x15 Bass
};
static_assert(std::size(kCabs) == static_cast<size_t>(AmpCab::Count), "a spec for each cabinet");

// The mics: {peak Hz, dB and Q, their own low end in dB, the most their proximity effect lifts it}.
struct MicSpec {
    double peakHz, peakDb, peakQ, lowDb, proximityDb;
};
constexpr MicSpec kMics[] = {
    {5000.0, 4.0, 1.2, -2.0, 6.0},  // Dynamic: a presence peak, a little low cut
    {10000.0, 2.5, 0.8, 0.0, 3.0},  // Condenser: flatter, more air
    {7000.0, -4.0, 0.6, 2.0, 8.0},  // Ribbon: dark and smooth, more low end
};
static_assert(std::size(kMics) == static_cast<size_t>(AmpMic::Count), "a spec for each mic");

// The room a mic set back hears: three early reflections, from about 15 cm on, all of them at 1 m.
constexpr double kRoomTaps[3] = {0.0013, 0.0031, 0.0077};
constexpr double kRoomLevels[3] = {0.45, 0.32, 0.22};

double roomAmount(double distanceNorm) {
    const double cm = ampDistanceCm(distanceNorm);
    return std::clamp((std::log(cm) - std::log(15.0)) / (std::log(100.0) - std::log(15.0)), 0.0, 1.0);
}

// The cabinet's and mic's filters at `rate`.
std::array<AmpCoefs, kCabFilters> cabCoefs(AmpCab cab, AmpMic mic, const AmpKnobs &k, double rate) {
    const CabSpec &c = kCabs[static_cast<int>(cab)];
    const MicSpec &m = kMics[static_cast<int>(mic)];
    const double p = std::clamp(k.position, 0.0, 1.0), room = roomAmount(k.distance);
    const double cm = ampDistanceCm(k.distance);
    // Toward the cone's edge the top falls away and the breakup peak drops; farther off, the proximity lift goes and
    // the top dulls a little.
    const double proximity = m.proximityDb * std::clamp(1.0 - std::log(cm) / std::log(30.0), 0.0, 1.0);
    const double lp = c.lpHz * (1.0 - 0.45 * p) * (1.0 - 0.15 * room);
    return {ampCoefs(AmpFilter::HighPass, c.hpHz, c.hpQ, 0.0, rate),
            ampCoefs(AmpFilter::Peak, c.lowHz, c.lowQ, c.lowDb, rate),
            ampCoefs(AmpFilter::Peak, c.breakHz * (1.0 - 0.25 * p), c.breakQ, c.breakDb - 5.0 * p, rate),
            ampCoefs(AmpFilter::Peak, c.dipHz, c.dipQ, c.dipDb, rate),
            ampCoefs(AmpFilter::LowPass, lp, 0.54, 0.0, rate),
            ampCoefs(AmpFilter::LowPass, lp, 1.31, 0.0, rate),
            ampCoefs(AmpFilter::Peak, m.peakHz, m.peakQ, m.peakDb - 2.0 * p, rate),
            ampCoefs(AmpFilter::LowShelf, 160.0, 0.7, m.lowDb + proximity, rate)};
}

// The passive stack's analog transfer function, b and a from s^0 up, from its parts and its knobs (0..1; the bass
// pot's taper is log).
void toneAnalog(const AmpToneParts &t, const AmpKnobs &k, std::array<double, 4> &b, std::array<double, 4> &a) {
    const double R1 = t.r1, R2 = t.r2, R3 = t.r3, R4 = t.r4, C1 = t.c1, C2 = t.c2, C3 = t.c3;
    const double tr = std::clamp(k.treble, 0.0, 1.0), m = std::clamp(k.mid, 0.0, 1.0);
    const double l = std::exp((std::clamp(k.bass, 0.0, 1.0) - 1.0) * 3.4);
    b[0] = 0.0;
    b[1] = tr * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    b[2] = tr * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
           l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    b[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + tr * C1 * C2 * C3 * R1 * R3 * R4 -
           tr * m * C1 * C2 * C3 * R1 * R3 * R4 + tr * l * C1 * C2 * C3 * R1 * R2 * R4;
    a[0] = 1.0;
    a[1] = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    a[2] = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
           (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 +
            C2 * C3 * R3 * R4);
    a[3] = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
           l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
}

double toneAnalogGain(const AmpToneParts &t, const AmpKnobs &k, double hz) {
    std::array<double, 4> b{}, a{};
    toneAnalog(t, k, b, a);
    const std::complex<double> s(0.0, 2.0 * kPi * hz);
    std::complex<double> num = 0.0, den = 0.0, power = 1.0;
    for (int i = 0; i < 4; ++i) {
        num += b[i] * power;
        den += a[i] * power;
        power *= s;
    }
    return std::abs(num / den);
}

// The passive stack's loudest at the middle knobs, which its output is scaled by so it is about 1 there: worked
// out once a model, as the plugin loads, so neither the audio thread nor the editor works it out (or waits on the
// other doing so) the first time an amp runs.
const std::array<double, static_cast<size_t>(AmpModel::Count)> kToneNorms = [] {
    std::array<double, static_cast<size_t>(AmpModel::Count)> out{};
    for (size_t m = 0; m < out.size(); ++m) {
        const AmpToneParts &t = kAmpModels[m].tone;
        double most = 1e-6;
        if (!t.active)
            for (int i = 0; i <= 60; ++i)
                most = std::max(most, toneAnalogGain(t, AmpKnobs{}, 20.0 * std::pow(1000.0, i / 60.0)));
        out[m] = t.active ? 1.0 : 1.0 / most;
    }
    return out;
}();

double toneNorm(AmpModel model) {
    return kToneNorms[std::clamp(static_cast<size_t>(model), size_t{0}, kToneNorms.size() - 1)];
}

std::array<AmpCoefs, 3> activeCoefs(const AmpKnobs &k, double rate) {
    return {ampCoefs(AmpFilter::LowShelf, 90.0, 0.7, (k.bass - 0.5) * 30.0, rate),
            ampCoefs(AmpFilter::Peak, 600.0, 0.8, (k.mid - 0.5) * 24.0, rate),
            ampCoefs(AmpFilter::HighShelf, 3000.0, 0.7, (k.treble - 0.5) * 30.0, rate)};
}

constexpr double kPresenceDb = 10.0; // Presence's shelf at its top
constexpr double kSmoothSeconds = 0.02; // how fast the knobs' coefficients follow them

} // namespace

AmpCoefs ampCoefs(AmpFilter type, double hz, double q, double gainDb, double rate) {
    // The audio EQ cookbook's biquads.
    const double w = 2.0 * kPi * std::clamp(hz, 5.0, 0.45 * rate) / rate;
    const double cw = std::cos(w), alpha = std::sin(w) / (2.0 * q);
    const double A = std::pow(10.0, gainDb / 40.0);
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (type) {
    case AmpFilter::LowPass:
        b0 = b2 = (1.0 - cw) * 0.5;
        b1 = 1.0 - cw;
        a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
        break;
    case AmpFilter::HighPass:
        b0 = b2 = (1.0 + cw) * 0.5;
        b1 = -(1.0 + cw);
        a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
        break;
    case AmpFilter::Peak:
        b0 = 1.0 + alpha * A, b1 = -2.0 * cw, b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A, a1 = -2.0 * cw, a2 = 1.0 - alpha / A;
        break;
    case AmpFilter::LowShelf:
    case AmpFilter::HighShelf: {
        const double s = type == AmpFilter::LowShelf ? -1.0 : 1.0; // flips the cosine terms
        const double root = 2.0 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1.0) + s * (A - 1.0) * cw + root);
        b1 = -2.0 * s * A * ((A - 1.0) + s * (A + 1.0) * cw);
        b2 = A * ((A + 1.0) + s * (A - 1.0) * cw - root);
        a0 = (A + 1.0) - s * (A - 1.0) * cw + root;
        a1 = 2.0 * s * ((A - 1.0) - s * (A + 1.0) * cw);
        a2 = (A + 1.0) - s * (A - 1.0) * cw - root;
        break;
    }
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

double ampCoefsGain(const AmpCoefs &c, double hz, double rate) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * kPi * hz / rate), z2 = z1 * z1;
    return std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
}

const AmpModelSpec &ampModelSpec(AmpModel model) {
    return kAmpModels[std::clamp(static_cast<int>(model), 0, static_cast<int>(AmpModel::Count) - 1)];
}

double ampToneResponse(AmpModel model, const AmpKnobs &knobs, double hz) {
    constexpr double kRate = 192000.0; // for the biquads: high enough to stay clear of their warping
    const AmpModelSpec &spec = ampModelSpec(model);
    double gain = 1.0;
    if (spec.tone.active) {
        for (const AmpCoefs &c : activeCoefs(knobs, kRate))
            gain *= ampCoefsGain(c, hz, kRate);
    } else {
        gain = toneAnalogGain(spec.tone, knobs, hz) * toneNorm(model);
    }
    return gain * ampCoefsGain(ampCoefs(AmpFilter::HighShelf, spec.presenceHz, 0.7, knobs.presence * kPresenceDb, kRate),
                               hz, kRate);
}

double ampCabResponse(AmpCab cab, AmpMic mic, const AmpKnobs &knobs, double hz) {
    constexpr double kRate = 96000.0;
    double gain = 1.0;
    for (const AmpCoefs &c : cabCoefs(cab, mic, knobs, kRate))
        gain *= ampCoefsGain(c, hz, kRate);
    return gain;
}

AmpConfig ampConfig(const ParamValues &p, int slot) {
    auto at = [&](FxParam f) { return p[paramIndex(slot, f)]; };
    AmpConfig c;
    c.model = storedEnum<AmpModel>(at(FxParam::AmpModel));
    c.cab = storedEnum<AmpCab>(at(FxParam::AmpCab));
    c.mic = storedEnum<AmpMic>(at(FxParam::AmpMic));
    c.cabOn = at(FxParam::AmpCabOn) >= 0.5;
    c.stereo = at(FxParam::AmpStereo) >= 0.5;
    c.factor = ampOversampling(at(FxParam::AmpOversampling));
    return c;
}

AmpKnobs ampKnobs(const ParamValues &p, int slot) {
    auto at = [&](FxParam f) { return std::clamp(p[paramIndex(slot, f)], 0.0, 1.0); };
    return {at(FxParam::AmpGain),   at(FxParam::AmpBass),   at(FxParam::AmpMid),
            at(FxParam::AmpTreble), at(FxParam::AmpPresence), at(FxParam::AmpVolume),
            at(FxParam::AmpPosition), at(FxParam::AmpDistance)};
}

void AmpCore::start(const AmpConfig &config, const AmpKnobs &knobs) {
    config_ = config;
    knobs_ = knobs;
    const AmpModelSpec &spec = ampModelSpec(config.model);
    const double rate = hostRate_ * config.factor;
    for (Side &s : sides_) {
        s = Side{};
        s.up.prepare(hostRate_, config.factor);
        s.down.prepare(hostRate_, config.factor);
    }
    inputPole_ = onePole(spec.inputHighPassHz, rate);
    cleanPole_ = onePole(250.0, rate);
    sagAttack_ = 1.0 - std::exp(-1.0 / (0.005 * rate));
    sagRelease_ = 1.0 - std::exp(-1.0 / (0.15 * rate));
    for (int i = 0; i < kAmpMaxStages; ++i) {
        const AmpStage &st = spec.stage[i];
        hpPole_[i] = i < spec.stages ? onePole(st.highPassHz, rate) : 0.0;
        lpPole_[i] = i < spec.stages ? onePole(st.lowPassHz, rate) : 1.0;
        stageGain_[i] = dbGain(st.gainDb);
        stageZero_[i] = i < spec.stages ? stageCurve(st.bias, st.negative) : 0.0;
        stageSlope_[i] = i < spec.stages ? stageSlope(st.bias, st.negative) : 1.0;
    }
    dcPole_ = onePole(10.0, hostRate_);
    roomPole_ = onePole(5000.0, hostRate_);
    for (int i = 0; i < 3; ++i)
        roomTap_[i] = std::clamp(static_cast<int>(std::lround(kRoomTaps[i] * hostRate_)), 1, kRoomLine - 1);
    inputGain_ = dbGain(spec.gainMinDb + knobs.gain * (spec.gainMaxDb - spec.gainMinDb));
    powerGain_ = dbGain(spec.powerMinDb + knobs.volume * (spec.powerMaxDb - spec.powerMinDb));
    setCoefs(knobs);
}

void AmpCore::setCoefs(const AmpKnobs &k) {
    coefKnobs_ = k;
    const AmpModelSpec &spec = ampModelSpec(config_.model);
    const double rate = hostRate_ * config_.factor;
    if (spec.tone.active) {
        activeCoefs_ = activeCoefs(k, rate);
    } else {
        // The bilinear transform of the third-order stack: s = c (1 - x) / (1 + x), x = 1/z, times (1 + x)^3.
        std::array<double, 4> b{}, a{};
        toneAnalog(spec.tone, k, b, a);
        constexpr double kExpand[4][4] = {{1, 3, 3, 1}, {1, 1, -1, -1}, {1, -1, -1, 1}, {1, -3, 3, -1}};
        const double c = 2.0 * rate;
        std::array<double, 4> B{}, A{};
        double ck = 1.0;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                B[j] += b[i] * ck * kExpand[i][j];
                A[j] += a[i] * ck * kExpand[i][j];
            }
            ck *= c;
        }
        const double norm = toneNorm(config_.model);
        for (int j = 0; j < 4; ++j) {
            toneB_[j] = B[j] / A[0] * norm;
            toneA_[j] = A[j] / A[0];
        }
    }
    presenceCoefs_ = ampCoefs(AmpFilter::HighShelf, spec.presenceHz, 0.7, k.presence * kPresenceDb, rate);
    cabCoefs_ = cabCoefs(config_.cab, config_.mic, k, hostRate_);
    const double room = roomAmount(k.distance);
    for (int i = 0; i < 3; ++i)
        roomGain_[i] = kRoomLevels[i] * room;
}

double AmpCore::runSide(Side &s, const float *in, float *out, uint32_t n, double gainFrom, double gainTo,
                        double volFrom, double volTo) {
    const AmpModelSpec &spec = ampModelSpec(config_.model);
    const int factor = config_.factor;
    const int m = static_cast<int>(n) * factor;
    double buf[Upsampler::kMaxSamples];
    s.up.process(in, buf, static_cast<int>(n));
    std::array<double, kAmpMaxStages + 1> peakIn{}, peakOut{};
    const double sagDepth = 0.5 * spec.sag;
    const double trim = dbGain(spec.trimDb);
    for (int i = 0; i < m; ++i) {
        const double t = (i + 1.0) / m;
        double x = buf[i];
        s.inputLow += inputPole_ * (x - s.inputLow);
        x -= s.inputLow;
        const double clean = x;
        for (int st = 0; st < spec.stages; ++st) {
            const AmpStage &stage = spec.stage[st];
            s.stageLow[st] += hpPole_[st] * (x - s.stageLow[st]);
            x -= s.stageLow[st];
            const double u = x * stageGain_[st] * (st == 0 ? gainFrom + (gainTo - gainFrom) * t : 1.0);
            const double y = (stageCurve(u + stage.bias, stage.negative) - stageZero_[st]) / stageSlope_[st];
            peakIn[st] = std::max(peakIn[st], std::abs(u));
            peakOut[st] = std::max(peakOut[st], std::abs(y));
            s.stageOut[st] += lpPole_[st] * (y - s.stageOut[st]);
            x = s.stageOut[st];
        }
        if (spec.cleanLow > 0.0) { // the lows, clean, two poles below 250 Hz
            s.cleanLow1 += cleanPole_ * (clean - s.cleanLow1);
            s.cleanLow2 += cleanPole_ * (s.cleanLow1 - s.cleanLow2);
            x += spec.cleanLow * s.cleanLow2;
        }
        if (spec.tone.active) {
            for (int b = 0; b < 3; ++b)
                x = s.active[b].process(x, activeCoefs_[b]);
        } else {
            const double y = toneB_[0] * x + s.tone[0];
            s.tone[0] = toneB_[1] * x - toneA_[1] * y + s.tone[1];
            s.tone[1] = toneB_[2] * x - toneA_[2] * y + s.tone[2];
            s.tone[2] = toneB_[3] * x - toneA_[3] * y;
            x = y;
        }
        x = s.presence.process(x, presenceCoefs_);
        // The power amp: its headroom drops as it works (sag), and comes back.
        const double drive = x * (volFrom + (volTo - volFrom) * t);
        const double headroom = 1.0 - sagDepth * std::min(s.sag, 1.0);
        const double y = headroom * fastTanh(drive / headroom);
        s.sag += (std::abs(y) > s.sag ? sagAttack_ : sagRelease_) * (std::abs(y) - s.sag);
        peakIn[kAmpMaxStages] = std::max(peakIn[kAmpMaxStages], std::abs(drive));
        peakOut[kAmpMaxStages] = std::max(peakOut[kAmpMaxStages], std::abs(y));
        buf[i] = y * trim;
    }
    std::fill_n(out, n, 0.0f);
    s.down.process(buf, out, static_cast<int>(n));
    for (uint32_t i = 0; i < n; ++i) {
        double x = out[i];
        s.dcIn += dcPole_ * (x - s.dcIn); // a DC blocker
        x -= s.dcIn;
        if (config_.cabOn) {
            for (int f = 0; f < kCabFilters; ++f)
                x = s.cab[f].process(x, cabCoefs_[f]);
            if (roomGain_[0] > 0.0) {
                double echoes = 0.0;
                for (int e = 0; e < 3; ++e)
                    echoes += roomGain_[e] * s.room[(s.roomWrite - roomTap_[e] + kRoomLine) % kRoomLine];
                s.room[s.roomWrite] = static_cast<float>(x);
                s.roomLow += roomPole_ * (echoes - s.roomLow);
                x += s.roomLow;
            } else {
                s.room[s.roomWrite] = static_cast<float>(x);
            }
            s.roomWrite = (s.roomWrite + 1) % kRoomLine;
        }
        out[i] = static_cast<float>(x);
    }
    double driveDb = 0.0;
    for (int st = 0; st <= kAmpMaxStages; ++st)
        if (peakOut[st] > 1e-9 && peakIn[st] > peakOut[st])
            driveDb += 20.0 * std::log10(peakIn[st] / peakOut[st]);
    return driveDb;
}

double AmpCore::process(float *const *io, uint32_t n, const AmpKnobs &target) {
    n = std::min(n, kMaxFrames);
    const AmpModelSpec &spec = ampModelSpec(config_.model);
    // The knobs glide; the coefficients follow them while they move.
    const double step = 1.0 - std::exp(-static_cast<double>(n) / (kSmoothSeconds * hostRate_));
    auto glide = [&](double &v, double to) { v += (to - v) * step; };
    glide(knobs_.gain, target.gain);
    glide(knobs_.bass, target.bass);
    glide(knobs_.mid, target.mid);
    glide(knobs_.treble, target.treble);
    glide(knobs_.presence, target.presence);
    glide(knobs_.volume, target.volume);
    glide(knobs_.position, target.position);
    glide(knobs_.distance, target.distance);
    auto moved = [](double a, double b) { return std::abs(a - b) > 1e-5; };
    if (moved(knobs_.bass, coefKnobs_.bass) || moved(knobs_.mid, coefKnobs_.mid) ||
        moved(knobs_.treble, coefKnobs_.treble) || moved(knobs_.presence, coefKnobs_.presence) ||
        moved(knobs_.position, coefKnobs_.position) || moved(knobs_.distance, coefKnobs_.distance))
        setCoefs(knobs_);
    const double gainTo = dbGain(spec.gainMinDb + knobs_.gain * (spec.gainMaxDb - spec.gainMinDb));
    const double volTo = dbGain(spec.powerMinDb + knobs_.volume * (spec.powerMaxDb - spec.powerMinDb));
    double drive = 0.0;
    if (config_.stereo) {
        float out[2][kMaxFrames];
        for (int c = 0; c < 2; ++c)
            drive = std::max(drive, runSide(sides_[c], io[c], out[c], n, inputGain_, gainTo, powerGain_, volTo));
        for (int c = 0; c < 2; ++c)
            std::copy_n(out[c], n, io[c]);
    } else { // one amp, on the sum
        float mono[kMaxFrames]{}, out[kMaxFrames];
        for (uint32_t i = 0; i < n; ++i)
            mono[i] = 0.5f * (io[0][i] + io[1][i]);
        drive = runSide(sides_[0], mono, out, n, inputGain_, gainTo, powerGain_, volTo);
        std::copy_n(out, n, io[0]);
        std::copy_n(out, n, io[1]);
    }
    inputGain_ = gainTo;
    powerGain_ = volTo;
    return drive;
}

} // namespace fmvibe
