#pragma once

// The Amp effect's sound: the models' preamp stages, tone stacks and power amps, the cabinets and mics after them,
// the oversampling the stages run at, and the responses the editor draws. Independent of the plugin formats, like
// effects.h, which runs it in an effect slot.

#include "oversampler.h"
#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fmvibe {

// A biquad's coefficients (y = b0 x + z1, as Effects::Biquad runs them) and its state.
struct AmpCoefs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};
struct AmpBiquad {
    double z1 = 0.0, z2 = 0.0;
    double process(double x, const AmpCoefs &c) {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};
enum class AmpFilter { LowPass, HighPass, Peak, LowShelf, HighShelf };
AmpCoefs ampCoefs(AmpFilter type, double hz, double q, double gainDb, double rate);
// A biquad's gain at `hz` (a factor).
double ampCoefsGain(const AmpCoefs &c, double hz, double rate);

// A preamp stage, triode-like: a coupling high-pass, a gain, an asymmetric curve with a bias (the side below it
// softer by `negative`, which gives the even harmonics), and a low-pass (the stage's own capacitance).
struct AmpStage {
    double highPassHz, lowPassHz, gainDb, bias, negative;
};
// A passive three-knob tone stack's parts (ohms and farads): the treble pot, the bass pot, the mid pot, the slope
// resistor and the three capacitors. Active, the stack is a bass shelf, a mid bell and a treble shelf instead.
struct AmpToneParts {
    bool active;
    double r1, r2, r3, r4, c1, c2, c3;
};
struct AmpModelSpec {
    double inputHighPassHz;      // what the input's coupling lets through
    double gainMinDb, gainMaxDb; // the preamp's drive over Gain 0..1
    int stages;
    std::array<AmpStage, 4> stage;
    AmpToneParts tone;
    double presenceHz;            // the Presence shelf's corner
    double powerMinDb, powerMaxDb; // the power amp's drive over Volume 0..1
    double sag;                   // how far the power amp's headroom drops as it works, 0..1
    double cleanLow;              // a clean low band blended in after the stages (the Bass model's), as a gain
    double trimDb;                // the level after it all, so the models come out about as loud as each other
};
constexpr int kAmpMaxStages = 4;
const AmpModelSpec &ampModelSpec(AmpModel model);

// The knobs, 0..1, as modulation takes them now.
struct AmpKnobs {
    double gain = 0.5, bass = 0.5, mid = 0.5, treble = 0.5, presence = 0.0, volume = 0.5;
    double position = 0.3, distance = 0.0; // the mic's (distance on the curve of ampDistanceCm())
};

// The tone stack's and Presence's response at `hz` for a model, as a gain (its loudest at the middle knobs about
// 1), for the editor.
double ampToneResponse(AmpModel model, const AmpKnobs &knobs, double hz);
// The cabinet's and mic's response at `hz`, as a gain, for the editor (the room's reflections left out).
double ampCabResponse(AmpCab cab, AmpMic mic, const AmpKnobs &knobs, double hz);

// The cabinet's filters: its low resonance (a resonant high-pass), a low-mid bump, the cone's breakup peak, a dip
// above it, a 24 dB per octave low-pass in two biquads, then the mic's peak and its low shelf (the proximity effect
// as it comes close, and its own low end).
constexpr int kCabFilters = 8;

// What a crossfade is needed for: the setting an amp runs at, apart from its knobs.
struct AmpConfig {
    AmpModel model = AmpModel::Crunch;
    AmpCab cab = AmpCab::Closed4x12;
    AmpMic mic = AmpMic::Dynamic;
    bool cabOn = true;
    bool stereo = false;
    int factor = 4; // oversampling
    bool operator==(const AmpConfig &) const = default;
};
AmpConfig ampConfig(const ParamValues &p, int slot);
AmpKnobs ampKnobs(const ParamValues &p, int slot);

// One amp at one setting, both sides: up to its oversampled rate, the preamp, the tone stack, Presence and the power
// amp, back down, then the cabinet and the mic. Audio thread only, apart from prepare().
class AmpCore {
public:
    static constexpr uint32_t kMaxFrames = 64; // a call's most frames
    static constexpr int kRoomLine = 2048;     // samples of the room's reflections each side keeps

    void prepare(double hostRate) { hostRate_ = hostRate; }
    // Cleared, at `config`, its knobs where `knobs` puts them.
    void start(const AmpConfig &config, const AmpKnobs &knobs);
    // In place, `n` <= kMaxFrames frames, the wet sound only. Returns how far the stages saturated, in dB (the most
    // in these frames).
    double process(float *const *io, uint32_t n, const AmpKnobs &knobs);
    const AmpConfig &config() const { return config_; }

private:
    struct Side {
        double inputLow = 0.0; // the input high-pass's low-pass state
        std::array<double, kAmpMaxStages> stageLow{}, stageOut{}; // each stage's high-pass and low-pass states
        double cleanLow1 = 0.0, cleanLow2 = 0.0;
        std::array<double, 3> tone{}; // the passive stack's third-order state
        std::array<AmpBiquad, 3> active{};
        AmpBiquad presence;
        double sag = 0.0; // the power amp's envelope
        double dcIn = 0.0, dcOut = 0.0;
        std::array<AmpBiquad, kCabFilters> cab{};
        std::array<float, kRoomLine> room{};
        int roomWrite = 0;
        double roomLow = 0.0;
        Upsampler up;
        Decimator down;
    };
    void setCoefs(const AmpKnobs &k);
    double runSide(Side &s, const float *in, float *out, uint32_t n, double gainFrom, double gainTo, double volFrom,
                   double volTo);

    AmpConfig config_;
    double hostRate_ = 48000.0;
    AmpKnobs knobs_;             // smoothed: the coefficients were last worked out from these
    AmpKnobs coefKnobs_;
    double inputGain_ = 1.0, powerGain_ = 1.0; // at the end of the last call
    std::array<Side, 2> sides_{};
    // Coefficients at the oversampled rate.
    double inputPole_ = 0.0, cleanPole_ = 0.0, sagAttack_ = 0.0, sagRelease_ = 0.0;
    std::array<double, kAmpMaxStages> hpPole_{}, lpPole_{}, stageGain_{}, stageZero_{}, stageSlope_{};
    std::array<double, 4> toneB_{}, toneA_{};
    std::array<AmpCoefs, 3> activeCoefs_{};
    AmpCoefs presenceCoefs_;
    // At the host rate.
    double dcPole_ = 0.0, roomPole_ = 0.0;
    std::array<AmpCoefs, kCabFilters> cabCoefs_{};
    std::array<double, 3> roomGain_{};
    std::array<int, 3> roomTap_{};
};

} // namespace fmvibe
