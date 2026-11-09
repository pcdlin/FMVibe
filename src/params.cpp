#include "params.h"
#include "uistrings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace fmvibe {

namespace {

constexpr Str kLfoShapeNames[] = {Str::LfoSine,   Str::LfoTriangle, Str::LfoSaw,
                                  Str::LfoSquare, Str::LfoRandom,   Str::LfoCustom};
static_assert(std::size(kLfoShapeNames) == static_cast<size_t>(LfoShape::Count));
constexpr Str kWaveNames[] = {Str::WaveSine,     Str::WaveTriangle, Str::WaveSaw,     Str::WaveSquare,
                              Str::WavePulse,    Str::WaveHalfSine, Str::WaveAbsSine, Str::WaveNoise,
                              Str::WaveCustom,   Str::WaveAltSine,  Str::WaveOrgan,   Str::WaveVowel};
static_assert(std::size(kWaveNames) == static_cast<size_t>(OpWave::Count));
constexpr Str kFilterTypeNames[] = {Str::FilterLowPass, Str::FilterHighPass, Str::FilterBandPass, Str::FilterNotch,
                                    Str::FilterComb};
constexpr Str kFilterTypeShortNames[] = {Str::FilterShortLowPass, Str::FilterShortHighPass, Str::FilterShortBandPass,
                                         Str::FilterShortNotch, Str::FilterShortComb};
const char *const kFilterSlopeNames[] = {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct"};
static_assert(std::size(kFilterSlopeNames) == static_cast<size_t>(kFilterSlopes));
static_assert(std::size(kFilterTypeNames) == static_cast<size_t>(FilterType::Count));
static_assert(std::size(kFilterTypeShortNames) == static_cast<size_t>(FilterType::Count));
const char *const kDivisionNames[] = {"4/1D",  "4/1",  "4/1T",  "2/1D",  "2/1",  "2/1T",  "1/1D",
                                      "1/1",   "1/1T", "1/2D",  "1/2",   "1/2T", "1/4D",  "1/4",
                                      "1/4T",  "1/8D", "1/8",   "1/8T",  "1/16D", "1/16", "1/16T",
                                      "1/32D", "1/32", "1/32T", "1/64D", "1/64",  "1/64T"};
static_assert(std::size(kDivisionNames) == static_cast<size_t>(kDivisions));
constexpr Str kClipModeNames[] = {Str::ClipHard,   Str::ClipSoft,     Str::ClipAsymmetric, Str::ClipCubic,
                                  Str::ClipGentle, Str::ClipSineFold, Str::ClipFold,       Str::ClipCustom};
static_assert(std::size(kClipModeNames) == static_cast<size_t>(ClipMode::Count));
constexpr Str kPeqTypeNames[] = {Str::PeqPeak,   Str::PeqLowShelf, Str::PeqHighShelf,
                                 Str::PeqLowCut, Str::PeqHighCut,  Str::PeqNotch};
static_assert(std::size(kPeqTypeNames) == static_cast<size_t>(PeqType::Count));
constexpr Str kReverbTypeNames[] = {Str::ReverbHall, Str::ReverbRoom, Str::ReverbPlate, Str::ReverbShimmer};
static_assert(std::size(kReverbTypeNames) == static_cast<size_t>(ReverbType::Count));
constexpr Str kAmpModelNames[] = {Str::AmpClean, Str::AmpBlues, Str::AmpCrunch, Str::AmpLead,
                                  Str::AmpHeavy, Str::AmpBass,  Str::AmpFuzz};
static_assert(std::size(kAmpModelNames) == static_cast<size_t>(AmpModel::Count));
constexpr Str kAmpCabNames[] = {Str::CabOpen1x12, Str::CabOpen2x12, Str::CabClosed4x12, Str::CabBass4x10,
                                Str::CabBass1x15};
static_assert(std::size(kAmpCabNames) == static_cast<size_t>(AmpCab::Count));
constexpr Str kAmpMicNames[] = {Str::MicDynamic, Str::MicCondenser, Str::MicRibbon};
static_assert(std::size(kAmpMicNames) == static_cast<size_t>(AmpMic::Count));
constexpr Str kRouteModeNames[] = {Str::RoutePhase, Str::RouteLinearFm, Str::RouteExpFm, Str::RouteRing};
static_assert(std::size(kRouteModeNames) == static_cast<size_t>(RouteMode::Count));
constexpr Str kFxNames[] = {Str::FxEmpty,     Str::FxShaper,      Str::FxDelay,      Str::FxDjEq,
                            Str::FxParamEq,   Str::FxFilter,      Str::FxCompressor, Str::FxMultiComp,
                            Str::FxReverb,    Str::FxPhaser,      Str::FxFlanger,    Str::FxFreqShifter,
                            Str::FxDisperser, Str::FxWidener,   Str::FxMultiDyn,   Str::FxAmp};
static_assert(std::size(kFxNames) == static_cast<size_t>(FxType::Count));
constexpr Str kSourceTypeNames[] = {Str::SourceEmpty, Str::SourceLfo, Str::SourceEnvelope, Str::SourceRandom,
                                    Str::SourceMidi};
constexpr Str kMidiInputNames[] = {Str::MidiInputCc, Str::MidiInputPressure, Str::MidiInputSlide};
static_assert(std::size(kMidiInputNames) == static_cast<size_t>(MidiInput::Count));
static_assert(std::size(kSourceTypeNames) == static_cast<size_t>(SourceType::Count));
// A drawn shaper curve's points as it starts, (x, y, curve) each: an S, flat at the ends, steep through the middle.
constexpr double kClipCurveDefaults[3][3] = {{0.0, -1.0, 0.35}, {0.5, 0.0, -0.35}, {1.0, 1.0, 0.0}};
constexpr int kTwoWhole = 4; // "2/1"
constexpr int kWhole = 7;    // "1/1"
constexpr int kQuarter = 13; // "1/4"
constexpr int kEighthDotted = 15;

// "1,2,3,…,16": the retired order's slots in the order they ran.
std::string fxOrderName(double value) {
    std::string name;
    for (int slot : retiredFxOrder(value))
        name += (name.empty() ? "" : ",") + std::to_string(slot + 1);
    return name;
}

// Reads the slot numbers back from an order's name; false unless each slot comes up once.
bool parseFxOrder(const char *text, double *out) {
    RetiredFxOrder order{};
    int count = 0;
    bool seen[kFirstFxSlots] = {};
    for (const char *c = text; *c;) {
        if (*c < '0' || *c > '9') {
            ++c;
            continue;
        }
        char *end = nullptr;
        const long number = std::strtol(c, &end, 10);
        c = end;
        const int slot = static_cast<int>(number) - 1;
        if (slot < 0 || slot >= kFirstFxSlots || seen[slot] || count == kFirstFxSlots)
            return false;
        seen[slot] = true;
        order[count++] = slot;
    }
    if (count != kFirstFxSlots)
        return false;
    *out = static_cast<double>(permutationRank<kFirstFxSlots>(order));
    return true;
}

template <size_t N>
const char *nameAt(const char *const (&names)[N], double value) {
    return names[std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(N) - 1)];
}
template <size_t N>
const char *nameAt(const Str (&names)[N], double value) {
    return tr(names[std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(N) - 1)]);
}

std::string letter(int op) {
    return std::string(1, static_cast<char>('A' + op));
}

// A source as hosts name its fields ("Mod 3", "Macro 2", "Note"), or None for -1: an Aux param's value text.
std::string auxSourceName(int source) {
    return source < 0             ? std::string(tr(Str::ValueNone))
           : isVelocity(source) ? std::string(tr(Str::ParamVelocitySource))
           : isWheel(source)    ? std::string(tr(Str::ParamModWheel))
           : isNote(source)     ? std::string(tr(Str::ParamNote))
           : isMacro(source)    ? trf(Str::ParamMacro, {std::to_string(source - kMaxSources + 1)})
                                : trf(Str::ParamMod, {std::to_string(source + 1)});
}

// Filled in place: the table is too large for a host thread's stack.
void buildTable(ParamTable &t) {
    auto add = [&t](int index, std::string name, std::string module, ParamKind kind, double min, double max,
                    double def) {
        // Continuous params accept host modulation, globally or (those the voices read) per voice (Bitwig's
        // modulators, for example); stepped ones are plain automation. Slot targets
        // and what each effect slot or source place holds are wiring, set from the editor, so hosts
        // don't offer them for automation. The voice limit reallocates voices, so
        // it isn't automatable either. Freeform LFO points are drawn in the editor,
        // slots are bypassed there and the chain order is set by dragging; hosts aren't given them (see
        // hostParamCount()).
        const bool shape = kind == ParamKind::ShapeCount || kind == ParamKind::ShapePoint ||
                           kind == ParamKind::SlotBypass || kind == ParamKind::SlotAux;
        const bool wiring = kind == ParamKind::ModTarget || kind == ParamKind::FxType ||
                            kind == ParamKind::SourceType || kind == ParamKind::FxOrder ||
                            kind == ParamKind::FxPosition || kind == ParamKind::FxPort;
        clap_param_info_flags flags = wiring || kind == ParamKind::VoiceCount || shape ? 0u : CLAP_PARAM_IS_AUTOMATABLE;
        // The chain order is set by dragging the cards; hosts can't do anything useful with it.
        if (shape || kind == ParamKind::FxOrder || kind == ParamKind::FxPosition || kind == ParamKind::FxPort)
            flags |= CLAP_PARAM_IS_HIDDEN;
        if (isStepped(kind))
            flags |= CLAP_PARAM_IS_STEPPED;
        if (!shape && (!isStepped(kind) || kind == ParamKind::Ratio || modulatedInSteps(index)))
            flags |= CLAP_PARAM_IS_MODULATABLE;
        // The effects, the macros, the note source's and the mod wheel's are shared by all voices.
        if ((flags & CLAP_PARAM_IS_MODULATABLE) && index < kVoiceParamCount)
            flags |= CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY;
        if (kind == ParamKind::LfoShape || kind == ParamKind::OpWave || kind == ParamKind::FilterType ||
            kind == ParamKind::FilterSlope || kind == ParamKind::Toggle || kind == ParamKind::SyncDivision ||
            kind == ParamKind::ClipMode || kind == ParamKind::FxType || kind == ParamKind::SourceType ||
            kind == ParamKind::PeqType || kind == ParamKind::ReverbType || kind == ParamKind::MidiInput ||
            kind == ParamKind::RouteMode || kind == ParamKind::AmpModel || kind == ParamKind::AmpCab ||
            kind == ParamKind::AmpMic || kind == ParamKind::AmpOversampling)
            flags |= CLAP_PARAM_IS_ENUM;
        t[index] = {idOfIndex(index), std::move(name), std::move(module), kind, min, max, def, flags};
    };

    // Slopes of an envelope's attack, decay and release, in that order. By default
    // the attack is a straight line and the decay and release are exponential.
    auto addCurves = [&](int attackCurve, const std::string &name, const std::string &module) {
        const Str stages[] = {Str::ParamAttackCurve, Str::ParamDecayCurve, Str::ParamReleaseCurve};
        for (int i = 0; i < 3; ++i)
            add(attackCurve + i, name + " " + tr(stages[i]), module, ParamKind::BipolarPercent, -1.0, 1.0,
                i == 0 ? 0.0 : kExponentialCurve);
    };

    const char *const global = tr(Str::ModuleGlobal);
    // The level after the effects, for the whole output: hosts modulate it globally, not per note. At 0 dB a
    // full-scale operator comes out at full scale; it starts 12 dB down, room for chords and stacked operators.
    add(paramIndex(GlobalParam::Volume), tr(Str::ParamVolume), global, ParamKind::Decibel, -60.0, 12.0, -12.0);
    t[paramIndex(GlobalParam::Volume)].flags &=
        ~static_cast<clap_param_info_flags>(CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY);
    add(paramIndex(GlobalParam::FmAmount), tr(Str::ParamFmAmount), global, ParamKind::Percent, 0.0, 2.0, 1.0);
    add(paramIndex(GlobalParam::BendRange), tr(Str::ParamBendRange), global, ParamKind::Semitones, 0.0, 24.0, 2.0);
    add(paramIndex(GlobalParam::Pitch), tr(Str::ParamPitch), global, ParamKind::SemitoneOffset, -48.0, 48.0, 0.0);
    add(paramIndex(GlobalParam::Pan), tr(Str::ParamPan), global, ParamKind::Pan, -1.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::Voices), tr(Str::ParamVoices), global, ParamKind::VoiceCount, 1.0, kMaxVoices, 16.0);
    add(paramIndex(GlobalParam::Mono), tr(Str::ParamMono), global, ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::Legato), tr(Str::ParamLegato), global, ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::Glide), tr(Str::ParamGlide), global, ParamKind::TimeOrOff, 0.0, kMaxGlideNorm,
        0.0);
    add(paramIndex(GlobalParam::GlideAlways), tr(Str::ParamGlideAlways), global, ParamKind::Toggle, 0.0, 1.0, 0.0);
    // Retired: a state from before version 8 holds the order here, which loads onto the places (see readParams()).
    add(paramIndex(GlobalParam::RetiredFxOrder), tr(Str::ParamFxOrder), tr(Str::ModuleFx), ParamKind::FxOrder, 0.0,
        static_cast<double>(kRetiredFxOrders - 1), 0.0);
    // Retired: a state from before version 9 holds the chain's order here, which loads as the effects graph's line (see
    // readParams()).
    for (int position = 0; position < kFxSlots; ++position)
        add(fxPositionIndex(position), trf(Str::ParamFxPosition, {std::to_string(position + 1)}), tr(Str::ModuleFx),
            ParamKind::FxPosition, 0.0, kFxSlots - 1.0, position);
    for (int split = 0; split < kFxSplits; ++split) {
        const std::string name = trf(Str::ParamSplit, {std::to_string(split + 1)});
        const std::string module = tr(Str::ModuleFx) + ("/" + name);
        add(paramIndex(split, SplitParam::Used), name + " " + tr(Str::ParamSplitUsed), module, ParamKind::Toggle, 0.0,
            1.0, 0.0);
        add(paramIndex(split, SplitParam::Bands), name + " " + tr(Str::ParamSplitBands), module, ParamKind::Count, 2.0,
            static_cast<double>(kFxBands), 2.0);
        add(paramIndex(split, SplitParam::Low), name + " " + tr(Str::ParamSplitLow), module, ParamKind::Cutoff, 0.0,
            1.0, normFromCutoff(250.0));
        add(paramIndex(split, SplitParam::High), name + " " + tr(Str::ParamSplitHigh), module, ParamKind::Cutoff, 0.0,
            1.0, normFromCutoff(2500.0));
        add(paramIndex(split, SplitParam::Slope), name + " " + tr(Str::ParamSlope), module, ParamKind::FilterSlope,
            0.0, kFilterSlopes - 1.0, 1.0); // 12 dB to begin with
    }
    for (int sum = 0; sum < kFxSums; ++sum) {
        const std::string name = trf(Str::ParamSum, {std::to_string(sum + 1)});
        add(paramIndex(sum, SumParam::Used), name + " " + tr(Str::ParamSplitUsed), tr(Str::ModuleFx) + ("/" + name),
            ParamKind::Toggle, 0.0, 1.0, 0.0);
        add(paramIndex(sum, SumParam::SoloHigh), name + " " + tr(Str::ParamSoloHigh), tr(Str::ModuleFx) + ("/" + name),
            ParamKind::Toggle, 0.0, 1.0, 0.0);
        add(paramIndex(sum, SumParam::SoloLow), name + " " + tr(Str::ParamSoloLow), tr(Str::ModuleFx) + ("/" + name),
            ParamKind::Toggle, 0.0, 1.0, 0.0);
        add(paramIndex(sum, SumParam::SoloMid), name + " " + tr(Str::ParamSoloMid), tr(Str::ModuleFx) + ("/" + name),
            ParamKind::Toggle, 0.0, 1.0, 0.0);
    }
    // The input straight out to begin with, no other wire; one connected starts at full level.
    for (int node = 0; node < kFxNodes; ++node) {
        const std::string name = node == kFxOutNode       ? std::string(tr(Str::ParamFxOut))
                                 : node < kFxSlots        ? trf(Str::ParamFx, {std::to_string(node + 1)})
                                 : fxNodeSum(node) >= 0   ? trf(Str::ParamSum, {std::to_string(fxNodeSum(node) + 1)})
                                                          : trf(Str::ParamSplit, {std::to_string(node - kFxSlots + 1)});
        const std::string module = tr(Str::ModuleFx) + ("/" + name);
        for (int input = 0; input < fxInputCount(node); ++input) {
            const std::string wire = name + " " + trf(Str::ParamFxInput, {std::to_string(input + 1)}) + " ";
            const bool straight = node == kFxOutNode && input == 0;
            add(fxInputIndex(node, input, FxInputField::From), wire + tr(Str::ParamFxInputFrom), module,
                ParamKind::FxPort, 0.0, static_cast<double>(kFxPorts), straight ? fxPortValue(kFxPortIn) : 0.0);
            // A join's first two inputs take its split's high and low bands, as the editor wires them.
            const bool band = fxNodeSum(node) >= 0 && input < 2;
            const std::string level =
                band ? name + " " + tr(input == 0 ? Str::BandHigh : Str::BandLow) : wire + tr(Str::ParamFxInputLevel);
            if (fxNodeSum(node) >= 0) // in dB, full at 0 dB to begin with (see sumLevelGain())
                add(fxInputIndex(node, input, FxInputField::Level), level, module, ParamKind::Decibel, kSumFloorDb,
                    0.0, 0.0);
            else
                add(fxInputIndex(node, input, FxInputField::Level), level, module, ParamKind::Percent, 0.0, 1.0, 1.0);
        }
    }
    // The mod wheel moves its targets for every voice, like a macro: hosts modulate it globally too.
    add(paramIndex(GlobalParam::ModWheel), tr(Str::ParamModWheel), global, ParamKind::Percent, 0.0, 1.0, 0.0);
    t[paramIndex(GlobalParam::ModWheel)].flags &=
        ~static_cast<clap_param_info_flags>(CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY);

    // Default patch: operator A, a plain sine carrier; the others start switched
    // off, to be added from the routing panel. Every operator has the same settings otherwise.
    for (int op = 0; op < kOps; ++op) {
        const std::string name = trf(Str::ParamOp, {letter(op)});
        const std::string module = tr(Str::ModuleOperators) + ("/" + name);
        const bool first = op == 0;
        // The ratio as it was, a whole part and a fraction on top: kept for their ids, they do nothing and hosts don't
        // see them. A state from before Ratio loads its ratio from them (see readParams()).
        add(paramIndex(op, OpParam::RetiredCoarse), name + " " + tr(Str::ParamCoarse), module, ParamKind::Ratio, 0.0,
            32.0, 1.0);
        t[paramIndex(op, OpParam::RetiredCoarse)].flags = CLAP_PARAM_IS_HIDDEN;
        add(paramIndex(op, OpParam::RetiredFine), name + " " + tr(Str::ParamFine), module, ParamKind::Percent, 0.0, 1.0,
            0.0);
        t[paramIndex(op, OpParam::RetiredFine)].flags = CLAP_PARAM_IS_HIDDEN;
        add(paramIndex(op, OpParam::Ratio), name + " " + tr(Str::ParamRatio), module, ParamKind::OpRatio, 0.0, 1.0,
            normFromRatio(1.0));
        add(paramIndex(op, OpParam::Detune), name + " " + tr(Str::ParamDetune), module, ParamKind::Cents, -50.0, 50.0,
            0.0);
        add(paramIndex(op, OpParam::Out), name + " " + tr(Str::ParamOut), module, ParamKind::Percent, 0.0, 1.0,
            first ? 1.0 : 0.0);
        add(paramIndex(op, OpParam::Pan), name + " " + tr(Str::ParamPan), module, ParamKind::Pan, -1.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Attack), name + " " + tr(Str::ParamAttack), module, ParamKind::EnvTime,
            kMinEnvNorm, kMaxEnvNorm, 0.0);
        add(paramIndex(op, OpParam::Decay), name + " " + tr(Str::ParamDecay), module, ParamKind::EnvTime, kMinEnvNorm,
            kMaxEnvNorm, normFromTime(1.5));
        add(paramIndex(op, OpParam::Sustain), name + " " + tr(Str::ParamSustain), module, ParamKind::Percent, 0.0,
            1.0, 1.0);
        add(paramIndex(op, OpParam::Release), name + " " + tr(Str::ParamRelease), module, ParamKind::EnvTime,
            kMinEnvNorm, kMaxEnvNorm, normFromTime(0.015));
        add(paramIndex(op, OpParam::Hold), name + " " + tr(Str::ParamHold), module, ParamKind::TimeOrOff, 0.0,
            kMaxEnvNorm, 0.0);
        add(paramIndex(op, OpParam::Transpose), name + " " + tr(Str::ParamPitch), module, ParamKind::Transpose, -48.0,
            48.0, 0.0);
        // What was the operator's velocity scaling: kept for its id, it does nothing and hosts don't see it.
        add(paramIndex(op, OpParam::RetiredVelocity), name + " " + tr(Str::ParamRetired), module, ParamKind::Percent,
            0.0, 1.0, 0.0);
        t[paramIndex(op, OpParam::RetiredVelocity)].flags = CLAP_PARAM_IS_HIDDEN;
        add(paramIndex(op, OpParam::Phase), name + " " + tr(Str::ParamPhase), module, ParamKind::Percent, 0.0,
            1.0, 0.0);
        add(paramIndex(op, OpParam::Enabled), name + " " + tr(Str::ParamOn), module, ParamKind::Toggle, 0.0, 1.0,
            first ? 1.0 : 0.0);
        add(paramIndex(op, OpParam::Wave), name + " " + tr(Str::ParamWave), module, ParamKind::OpWave, 0.0,
            static_cast<double>(OpWave::Count) - 1.0, 0.0);
        for (int filter = 0; filter < kFilters; ++filter)
            add(sendIndex(op, filter), letter(op) + " → F" + std::to_string(filter + 1), tr(Str::ModuleMatrix),
                ParamKind::Percent, 0.0, 1.0, 0.0);
        for (int shaper = 0; shaper < kShapers; ++shaper)
            add(shaperSendIndex(op, shaper), letter(op) + " → S" + std::to_string(shaper + 1), tr(Str::ModuleMatrix),
                ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Unison), name + " " + tr(Str::ParamUnison), module, ParamKind::Count, 1.0,
            kMaxUnison, 1.0);
        add(paramIndex(op, OpParam::UnisonDetune), name + " " + tr(Str::ParamUnisonDetune), module,
            ParamKind::CentsAmount, 0.0, 100.0, 20.0);
        add(paramIndex(op, OpParam::UnisonSpread), name + " " + tr(Str::ParamUnisonSpread), module,
            ParamKind::Percent, 0.0, 1.0, 0.5);
        add(paramIndex(op, OpParam::PhaseRandom), name + " " + tr(Str::ParamPhaseRandom), module, ParamKind::Percent,
            0.0, 1.0, 0.0);
        addCurves(paramIndex(op, OpParam::AttackCurve), name, module);
        add(paramIndex(op, OpParam::NoiseSlope), name + " " + tr(Str::ParamNoiseSlope), module, ParamKind::NoiseSlope,
            0.0, 1.0, 0.0);
        // Fixed, it plays 440 Hz until its Freq and Mult are set (the editor sets them to where the ratio was).
        add(paramIndex(op, OpParam::Fixed), name + " " + tr(Str::ParamFixed), module, ParamKind::Toggle, 0.0, 1.0,
            0.0);
        add(paramIndex(op, OpParam::FixedFreq), name + " " + tr(Str::ParamFixedFreq), module, ParamKind::FixedFreq,
            0.0, 1.0, normFromFixedFreq(440.0));
        add(paramIndex(op, OpParam::FixedMult), name + " " + tr(Str::ParamFixedMult), module, ParamKind::FixedMult,
            0.0, kFixedMultCount - 1.0, 3.0);
        // The Custom wave starts as a sine: the first partial alone, of 32.
        add(partialCountIndex(op), name + " " + tr(Str::ParamPartials), module, ParamKind::ShapeCount,
            kPartialCounts[0], kPartials, kDefaultPartials);
        for (int partial = 0; partial < kPartials; ++partial) {
            const std::string n = name + " " + trf(Str::ParamPartial, {std::to_string(partial + 1)});
            add(partialLevelIndex(op, partial), n + " " + tr(Str::ParamLevel), module, ParamKind::ShapePoint, 0.0, 1.0,
                partial == 0 ? 1.0 : 0.0);
            add(partialPhaseIndex(op, partial), n + " " + tr(Str::ParamPhase), module, ParamKind::ShapePoint, 0.0,
                1.0, 0.0);
        }
    }

    for (int from = 0; from < kOps; ++from) {
        for (int to = 0; to < kOps; ++to) {
            const std::string name =
                from == to ? trf(Str::ParamOpFeedback, {letter(from)}) : letter(from) + " → " + letter(to);
            add(matrixIndex(from, to), name, tr(Str::ModuleMatrix), ParamKind::Percent, 0.0, 1.0, 0.0);
        }
    }
    for (int filter = 0; filter < kFilters; ++filter)
        for (int to = 0; to < kOps; ++to)
            add(filterModIndex(filter, to), "F" + std::to_string(filter + 1) + " → " + letter(to),
                tr(Str::ModuleMatrix), ParamKind::Percent, 0.0, 1.0, 0.0);

    // Filter 1 defaults to a warm low-pass and filter 2 to a band-pass for something different, both at
    // kDefaultCutoff. Neither follows the note until its Key Track is turned up (or the note source is put on its
    // cutoff), and like the shapers and operators B-F, neither goes out until it is routed there.
    struct FilterDefaults {
        FilterType type;
        double slope;
        double resonance;
    };
    const FilterDefaults filterDefaults[] = {{FilterType::LowPass, kDefaultSlope, 0.3},
                                             {FilterType::BandPass, kDefaultSlope, 0.5}};
    static_assert(std::size(filterDefaults) == static_cast<size_t>(kFilters));
    for (int filter = 0; filter < kFilters; ++filter) {
        const std::string name = trf(Str::ParamFilter, {std::to_string(filter + 1)});
        const std::string module = tr(Str::ModuleFilters) + ("/" + name);
        const FilterDefaults &d = filterDefaults[filter];
        add(paramIndex(filter, FilterParam::Type), name + " " + tr(Str::ParamType), module, ParamKind::FilterType, 0.0,
            static_cast<double>(FilterType::Count) - 1.0, static_cast<double>(d.type));
        add(paramIndex(filter, FilterParam::Cutoff), name + " " + tr(Str::ParamCutoff), module, ParamKind::Cutoff,
            0.0, 1.0, kDefaultCutoff);
        add(paramIndex(filter, FilterParam::Resonance), name + " " + tr(Str::ParamResonance), module,
            ParamKind::Percent, 0.0, 1.0, d.resonance);
        add(paramIndex(filter, FilterParam::KeyTrack), name + " " + tr(Str::ParamKeyTrack), module,
            ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(filter, FilterParam::Drive), name + " " + tr(Str::ParamDrive), module, ParamKind::Percent, 0.0,
            1.0, 0.0);
        add(paramIndex(filter, FilterParam::Out), name + " " + tr(Str::ParamOut), module, ParamKind::Percent, 0.0,
            1.0, 0.0);
        add(paramIndex(filter, FilterParam::Slope), name + " " + tr(Str::ParamSlope), module, ParamKind::FilterSlope,
            0.0, kFilterSlopes - 1.0, d.slope);
        add(paramIndex(filter, FilterParam::Enabled), name + " " + tr(Str::ParamOn), module, ParamKind::Toggle, 0.0,
            1.0, 0.0);
    }
    for (int from = 0; from < kFilters; ++from) {
        const std::string tag = "F" + std::to_string(from + 1);
        for (int to = 0; to < kFilters; ++to)
            add(filterLinkIndex(from, to),
                to == from ? trf(Str::ParamOpFeedback, {tag}) : tag + " → F" + std::to_string(to + 1),
                tr(Str::ModuleFilters), ParamKind::Percent, 0.0, 1.0, 0.0);
    }
    for (int filter = 0; filter < kFilters; ++filter)
        for (int shaper = 0; shaper < kShapers; ++shaper)
            add(filterShaperIndex(filter, shaper), "F" + std::to_string(filter + 1) + " → S" + std::to_string(shaper + 1),
                tr(Str::ModuleMatrix), ParamKind::Percent, 0.0, 1.0, 0.0);

    // Shapers start switched off, with the Soft curve, linked to their drive, routed nowhere; a drawn curve starts as
    // an S, like the shaper effect's.
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        const std::string tag = "S" + std::to_string(shaper + 1);
        const std::string name = trf(Str::ParamShaper, {std::to_string(shaper + 1)});
        const std::string module = tr(Str::ModuleShapers) + ("/" + name);
        add(paramIndex(shaper, ShaperParam::Enabled), name + " " + tr(Str::ParamOn), module, ParamKind::Toggle, 0.0,
            1.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Mode), name + " " + tr(Str::ParamCurve), module, ParamKind::ClipMode, 0.0,
            static_cast<double>(ClipMode::Count) - 1.0, static_cast<double>(ClipMode::Soft));
        add(paramIndex(shaper, ShaperParam::Drive), name + " " + tr(Str::ParamDrive), module, ParamKind::Decibel, 0.0,
            36.0, 6.0);
        add(paramIndex(shaper, ShaperParam::Output), name + " " + tr(Str::ParamOutput), module, ParamKind::Decibel,
            -24.0, 6.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Mix), name + " " + tr(Str::ParamMix), module, ParamKind::Percent, 0.0,
            1.0, 1.0);
        add(paramIndex(shaper, ShaperParam::Link), name + " " + tr(Str::ParamLink), module, ParamKind::Toggle, 0.0,
            1.0, 1.0);
        for (int filter = 0; filter < kFilters; ++filter)
            add(shaperFilterIndex(shaper, filter), tag + " → F" + std::to_string(filter + 1), tr(Str::ModuleMatrix),
                ParamKind::Percent, 0.0, 1.0, 0.0);
        for (int to = 0; to < kOps; ++to)
            add(shaperModIndex(shaper, to), tag + " → " + letter(to), tr(Str::ModuleMatrix), ParamKind::Percent, 0.0,
                1.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Out), name + " " + tr(Str::ParamToOut), module, ParamKind::Percent, 0.0,
            1.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Symmetric), name + " " + tr(Str::ParamSymmetric), module,
            ParamKind::Toggle, 0.0, 1.0, 0.0);
        for (int to = 0; to < kShapers; ++to)
            add(shaperShaperIndex(shaper, to),
                to == shaper ? trf(Str::ParamOpFeedback, {tag}) : tag + " → S" + std::to_string(to + 1),
                tr(Str::ModuleMatrix), ParamKind::Percent, 0.0, 1.0, 0.0);
        const int base = shaperCurveCountIndex(shaper);
        add(base, name + " " + tr(Str::ParamCurve) + " " + tr(Str::ParamPoints), module, ParamKind::ShapeCount, 2.0,
            kMaxShapePoints, 3.0);
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = name + " " + tr(Str::ParamCurve) + " " + std::to_string(point + 1);
            const bool used = point < 3;
            add(shapeFieldIndex(base, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? kClipCurveDefaults[point][0] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? kClipCurveDefaults[point][1] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " " + tr(Str::ParamCurve), module,
                ParamKind::ShapePoint, -1.0, 1.0, used ? kClipCurveDefaults[point][2] : 0.0);
        }
    }

    // The macros start at nothing, mapped to nothing.
    for (int macro = 0; macro < kMacros; ++macro) {
        const std::string name = trf(Str::ParamMacro, {std::to_string(macro + 1)});
        const std::string module = tr(Str::ModuleMacros) + ("/" + name);
        add(paramIndex(macro, MacroParam::Value), name, module, ParamKind::Percent, 0.0, 1.0, 0.0);
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotTargetIndex(kMaxSources + macro, slot), name + " " + trf(Str::ParamTarget, {n}), module,
                ParamKind::ModTarget, 0.0, kMaxParamId + 1.0, 0.0);
            add(slotAmountIndex(kMaxSources + macro, slot), name + " " + trf(Str::ParamAmount, {n}), module,
                ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
        }
        add(paramIndex(macro, MacroParam::Bipolar), name + " " + tr(Str::ParamBipolar), module, ParamKind::Toggle,
            0.0, 1.0, 0.0);
    }

    // The note source starts mapped to nothing, over every MIDI note and on to the pitch at 20 kHz, its curve
    // tracking the note on a cutoff (below).
    const std::string note = tr(Str::ParamNote), wheel = tr(Str::ParamModWheel);
    for (int slot = 0; slot < kSlots; ++slot) {
        const std::string n = std::to_string(slot + 1);
        add(slotTargetIndex(kNoteSource, slot), note + " " + trf(Str::ParamTarget, {n}), note, ParamKind::ModTarget,
            0.0, kMaxParamId + 1.0, 0.0);
        add(slotAmountIndex(kNoteSource, slot), note + " " + trf(Str::ParamAmount, {n}), note,
            ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
    }
    // The mod wheel starts mapped to nothing, like the macros.
    for (int slot = 0; slot < kSlots; ++slot) {
        const std::string n = std::to_string(slot + 1);
        add(slotTargetIndex(kWheelSource, slot), wheel + " " + trf(Str::ParamTarget, {n}), wheel,
            ParamKind::ModTarget, 0.0, kMaxParamId + 1.0, 0.0);
        add(slotAmountIndex(kWheelSource, slot), wheel + " " + trf(Str::ParamAmount, {n}), wheel,
            ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
    }
    // Both start unipolar, like the macros: they take their targets one way.
    add(paramIndex(WheelParam::Bipolar), wheel + " " + tr(Str::ParamBipolar), wheel, ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(NoteParam::Bipolar), note + " " + tr(Str::ParamBipolar), note, ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(NoteParam::Low), note + " " + tr(Str::ParamLow), note, ParamKind::NotePitch, 0.0, kMaxNotePitch,
        0.0);
    add(paramIndex(NoteParam::High), note + " " + tr(Str::ParamHigh), note, ParamKind::NotePitch, 0.0, kMaxNotePitch,
        kMaxNotePitch);
    // Its curve starts drawn along the cutoff knobs' over that range: nothing up to the pitch at 20 Hz, then straight
    // up to the top at 20 kHz; the points it doesn't use wait at the last one.
    {
        const int base = paramIndex(NoteParam::Curve);
        const std::string curve = note + " " + tr(Str::ParamCurve);
        const double kneeX = pitchOfHz(kMinCutoff) / kMaxNotePitch, topY = 1.0;
        add(base, curve + " " + tr(Str::ParamPoints), note, ParamKind::ShapeCount, 2.0, kMaxNotePoints, 3.0);
        for (int point = 0; point < kMaxNotePoints; ++point) {
            const std::string n = curve + " " + std::to_string(point + 1);
            add(shapeFieldIndex(base, point, PointField::X), n + " X", note, ParamKind::ShapePoint, 0.0, 1.0,
                point == 0 ? 0.0 : point == 1 ? kneeX : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", note, ParamKind::ShapePoint, -1.0, 1.0,
                point < 2 ? -1.0 : topY);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " " + tr(Str::ParamCurve), note,
                ParamKind::ShapePoint, -1.0, 1.0, 0.0);
        }
    }

    // The velocity source starts mapped to nothing, unipolar like the note source, its curve a straight line: it puts
    // out the velocity as it came. The points it doesn't use wait at the last one.
    {
        const std::string velocity = tr(Str::ParamVelocitySource);
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotTargetIndex(kVelocitySource, slot), velocity + " " + trf(Str::ParamTarget, {n}), velocity,
                ParamKind::ModTarget, 0.0, kMaxParamId + 1.0, 0.0);
            add(slotAmountIndex(kVelocitySource, slot), velocity + " " + trf(Str::ParamAmount, {n}), velocity,
                ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
        }
        const int base = paramIndex(VelocityParam::Curve);
        const std::string curve = velocity + " " + tr(Str::ParamCurve);
        add(base, curve + " " + tr(Str::ParamPoints), velocity, ParamKind::ShapeCount, 2.0, kMaxNotePoints, 2.0);
        for (int point = 0; point < kMaxNotePoints; ++point) {
            const std::string n = curve + " " + std::to_string(point + 1);
            add(shapeFieldIndex(base, point, PointField::X), n + " X", velocity, ParamKind::ShapePoint, 0.0, 1.0,
                point == 0 ? 0.0 : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", velocity, ParamKind::ShapePoint, -1.0, 1.0,
                point == 0 ? -1.0 : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " " + tr(Str::ParamCurve), velocity,
                ParamKind::ShapePoint, -1.0, 1.0, 0.0);
        }
        add(paramIndex(VelocityParam::Bipolar), velocity + " " + tr(Str::ParamBipolar), velocity, ParamKind::Toggle,
            0.0, 1.0, 0.0);
    }

    // Every slot of every source has an aux, an Amount and a curve, and those of the sources under the keyboard their bypass.
    for (int source = 0; source < kAllSources; ++source) {
        const std::string name = isVelocity(source) ? tr(Str::ParamVelocitySource)
                                 : isWheel(source)  ? tr(Str::ParamModWheel)
                                 : isNote(source)   ? tr(Str::ParamNote)
                                 : isMacro(source)  ? trf(Str::ParamMacro, {std::to_string(source - kMaxSources + 1)})
                                                    : trf(Str::ParamMod, {std::to_string(source + 1)});
        const std::string module = isNoteBorne(source) || isWheel(source) ? name
                                   : isMacro(source) ? tr(Str::ModuleMacros) + ("/" + name)
                                                     : tr(Str::ModuleModulation) + ("/" + name);
        // No aux, and all of the depth.
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotAuxIndex(source, slot), name + " " + trf(Str::ParamAux, {n}), module, ParamKind::SlotAux, 0.0,
                static_cast<double>(kAllSources), 0.0);
            add(slotScaleIndex(source, slot), name + " " + trf(Str::ParamAuxAmount, {n}), module, ParamKind::Percent,
                0.0, 1.0, 1.0);
            add(slotCurveIndex(source, slot), name + " " + trf(Str::ParamSlotCurve, {n}), module,
                ParamKind::BipolarPercent, -1.0, 1.0, 0.0); // a straight line
            if (isStripSource(source)) // the places' bypasses come with their other fields
                add(slotBypassIndex(source, slot), name + " " + trf(Str::ParamBypass, {n}), module,
                    ParamKind::SlotBypass, 0.0, 1.0, 0.0);
        }
    }

    // Every route into an operator starts as phase modulation. Feedback is always that: its field is hidden.
    for (int from = 0; from < kRouteSources; ++from) {
        const std::string tag = from < kOps                ? letter(from)
                                : from < kOps + kFilters ? "F" + std::to_string(from - kOps + 1)
                                                          : "S" + std::to_string(from - kOps - kFilters + 1);
        for (int to = 0; to < kOps; ++to) {
            add(routeModeIndex(from, to), trf(Str::ParamRouteMode, {tag + " → " + letter(to)}), tr(Str::ModuleMatrix),
                ParamKind::RouteMode, 0.0, static_cast<double>(RouteMode::Count) - 1.0, 0.0);
            if (from == to)
                t[routeModeIndex(from, to)].flags = CLAP_PARAM_IS_HIDDEN;
        }
    }

    // The effects chain starts empty.
    for (int slot = 0; slot < kFxSlots; ++slot) {
        const std::string name = trf(Str::ParamFx, {std::to_string(slot + 1)});
        const std::string module = tr(Str::ModuleFx) + ("/" + name);
        auto fx = [&](FxParam field, const std::string &label, ParamKind kind, double min, double max, double def) {
            add(paramIndex(slot, field), name + " " + label, module, kind, min, max, def);
        };
        fx(FxParam::Type, tr(Str::ParamType), ParamKind::FxType, 0.0, static_cast<double>(FxType::Count) - 1.0, 0.0);
        fx(FxParam::Enabled, tr(Str::ParamOn), ParamKind::Toggle, 0.0, 1.0, 1.0);

        fx(FxParam::ShaperMode, tr(Str::ParamShaperCurve), ParamKind::ClipMode, 0.0,
           static_cast<double>(ClipMode::Count) - 1.0, static_cast<double>(ClipMode::Soft));
        fx(FxParam::ShaperDrive, tr(Str::ParamShaperDrive), ParamKind::Decibel, 0.0, 36.0, 6.0);
        fx(FxParam::ShaperOutput, tr(Str::ParamShaperOutput), ParamKind::Decibel, -24.0, 6.0, 0.0);
        fx(FxParam::ShaperMix, tr(Str::ParamShaperMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        // The drawn curve starts as an S: flat at the ends, steep through the middle.
        const std::string curve = name + " " + tr(Str::ParamShaperCurve);
        add(clipCurveCountIndex(slot), curve + " " + tr(Str::ParamPoints), module, ParamKind::ShapeCount, 2.0,
            kMaxShapePoints, 3.0);
        const auto &curveDefaults = kClipCurveDefaults;
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = curve + " " + std::to_string(point + 1);
            const bool used = point < 3;
            const int base = clipCurveCountIndex(slot);
            add(shapeFieldIndex(base, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? curveDefaults[point][0] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? curveDefaults[point][1] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " " + tr(Str::ParamCurve), module,
                ParamKind::ShapePoint, -1.0, 1.0, used ? curveDefaults[point][2] : 0.0);
        }

        // A dotted eighth on the left and a quarter on the right.
        fx(FxParam::DelayTimeL, tr(Str::ParamDelayTimeL), ParamKind::Time, 0.0, normFromTime(kMaxFreeDelay),
           normFromTime(0.375));
        fx(FxParam::DelayTimeR, tr(Str::ParamDelayTimeR), ParamKind::Time, 0.0, normFromTime(kMaxFreeDelay),
            normFromTime(0.5));
        fx(FxParam::DelaySync, tr(Str::ParamDelaySync), ParamKind::Toggle, 0.0, 1.0, 1.0);
        fx(FxParam::DelayLink, tr(Str::ParamDelayLink), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::DelayDivisionL, tr(Str::ParamDelayDivisionL), ParamKind::SyncDivision, 0.0, kDivisions - 1.0,
           kEighthDotted);
        fx(FxParam::DelayDivisionR, tr(Str::ParamDelayDivisionR), ParamKind::SyncDivision, 0.0, kDivisions - 1.0,
            kQuarter);
        fx(FxParam::DelayFeedback, tr(Str::ParamDelayFeedback), ParamKind::Percent, 0.0, 0.95, 0.4);
        fx(FxParam::DelayTone, tr(Str::ParamDelayTone), ParamKind::DelayTone, -1.0, 1.0, 0.0);
        fx(FxParam::DelayWidth, tr(Str::ParamDelayWidth), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::DelayPingPong, tr(Str::ParamDelayPingPong), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::DelayMix, tr(Str::ParamDelayMix), ParamKind::Percent, 0.0, 1.0, 0.3);

        fx(FxParam::EqLow, tr(Str::ParamEqLow), ParamKind::EqGain, -1.0, 1.0, 0.0);
        fx(FxParam::EqMid, tr(Str::ParamEqMid), ParamKind::EqGain, -1.0, 1.0, 0.0);
        fx(FxParam::EqHigh, tr(Str::ParamEqHigh), ParamKind::EqGain, -1.0, 1.0, 0.0);

        // Flat to begin with, all three bands on: one in the lows, one in the mids and one in the highs.
        const double bandHz[kPeqBands] = {250.0, 1000.0, 4000.0};
        for (int band = 0; band < kPeqBands; ++band) {
            const std::string b = name + " " + trf(Str::ParamPeqBand, {std::to_string(band + 1)});
            add(peqFreqIndex(slot, band), b + " " + tr(Str::ParamFreq), module, ParamKind::Cutoff, 0.0, 1.0,
                normFromCutoff(bandHz[band]));
            add(peqGainIndex(slot, band), b + " " + tr(Str::ParamGain), module, ParamKind::Decibel, -kMaxPeqGain,
                kMaxPeqGain, 0.0);
            add(peqQIndex(slot, band), b + " " + tr(Str::ParamQ), module, ParamKind::Q, 0.0, 1.0, normFromQ(1.0));
            add(peqTypeIndex(slot, band), b + " " + tr(Str::ParamType), module, ParamKind::PeqType, 0.0,
                static_cast<double>(PeqType::Count) - 1.0, static_cast<double>(PeqType::Peak));
            add(peqOnIndex(slot, band), b + " " + tr(Str::ParamOn), module, ParamKind::Toggle, 0.0, 1.0,
                1.0);
        }
        fx(FxParam::PeqAmount, tr(Str::ParamPeqAmount), ParamKind::Percent, 0.0, 1.0, 1.0);

        fx(FxParam::FilterType, tr(Str::ParamFilterType), ParamKind::FilterType, 0.0,
           static_cast<double>(FilterType::Count) - 1.0, static_cast<double>(FilterType::LowPass));
        fx(FxParam::FilterSlope, tr(Str::ParamFilterSlope), ParamKind::FilterSlope, 0.0, kFilterSlopes - 1.0,
            kDefaultSlope);
        fx(FxParam::FilterCutoff, tr(Str::ParamFilterCutoff), ParamKind::Cutoff, 0.0, 1.0, kDefaultCutoff);
        fx(FxParam::FilterResonance, tr(Str::ParamFilterResonance), ParamKind::Percent, 0.0, 1.0, 0.2);
        fx(FxParam::FilterDrive, tr(Str::ParamFilterDrive), ParamKind::Percent, 0.0, 1.0, 0.0);
        fx(FxParam::FilterMix, tr(Str::ParamFilterMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::FilterKeyTrack, tr(Str::ParamFilterKeyTrack), ParamKind::Percent, 0.0, 1.0, 0.0);

        fx(FxParam::CompThreshold, tr(Str::ParamCompThreshold), ParamKind::Decibel, -60.0, 0.0, -18.0);
        fx(FxParam::CompRatio, tr(Str::ParamCompRatio), ParamKind::CompRatio, 1.0, 20.0, 4.0);
        fx(FxParam::CompKnee, tr(Str::ParamCompKnee), ParamKind::Decibel, 0.0, 24.0, 6.0);
        fx(FxParam::CompAttack, tr(Str::ParamCompAttack), ParamKind::Time, 0.0, normFromTime(0.5), normFromTime(0.01));
        fx(FxParam::CompRelease, tr(Str::ParamCompRelease), ParamKind::Time, 0.0, normFromTime(2.0),
            normFromTime(0.15));
        fx(FxParam::CompMakeup, tr(Str::ParamCompMakeup), ParamKind::Decibel, 0.0, 24.0, 0.0);
        fx(FxParam::CompMix, tr(Str::ParamCompMix), ParamKind::Percent, 0.0, 1.0, 1.0);

        fx(FxParam::McLowFreq, tr(Str::ParamMcLowCrossover), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(200.0));
        fx(FxParam::McHighFreq, tr(Str::ParamMcHighCrossover), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(2500.0));
        const Str bandNames[kMcBands] = {Str::BandLow, Str::BandMid, Str::BandHigh};
        for (int band = 0; band < kMcBands; ++band) {
            const std::string b = name + " " + trf(Str::ParamMcBand, {tr(bandNames[band])});
            add(mcThresholdIndex(slot, band), b + " " + tr(Str::ParamThreshold), module, ParamKind::Decibel, -60.0, 0.0,
                -20.0);
            add(mcRatioIndex(slot, band), b + " " + tr(Str::ParamRatio), module, ParamKind::CompRatio, 1.0, 20.0, 3.0);
            add(mcGainIndex(slot, band), b + " " + tr(Str::ParamGain), module, ParamKind::Decibel, -12.0, 12.0, 0.0);
        }
        fx(FxParam::McAttack, tr(Str::ParamMcAttack), ParamKind::Time, 0.0, normFromTime(0.5), normFromTime(0.01));
        fx(FxParam::McRelease, tr(Str::ParamMcRelease), ParamKind::Time, 0.0, normFromTime(2.0), normFromTime(0.15));
        fx(FxParam::McMix, tr(Str::ParamMcMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::McOutput, tr(Str::ParamMcOutput), ParamKind::Decibel, -24.0, 24.0, 0.0);
        fx(FxParam::McLink, tr(Str::ParamMcLink), ParamKind::Toggle, 0.0, 1.0, 0.0);

        // Three-band dynamics: each band pulled down 3:1 above -20 dB and lifted 2:1 below -40 dB, at most 12 dB,
        // following the bands' RMS levels.
        fx(FxParam::MdLowFreq, tr(Str::ParamMdLowCrossover), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(200.0));
        fx(FxParam::MdHighFreq, tr(Str::ParamMdHighCrossover), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(2500.0));
        for (int band = 0; band < kMcBands; ++band) {
            const std::string b = name + " " + trf(Str::ParamMdBand, {tr(bandNames[band])});
            add(mdAboveIndex(slot, band), b + " " + tr(Str::ParamAboveThreshold), module, ParamKind::Decibel,
                kMdThresholdMinDb, 0.0, -20.0);
            add(mdDownIndex(slot, band), b + " " + tr(Str::ParamDownRatio), module, ParamKind::DynRatio, 0.0, 1.0,
                normFromDynRatio(3.0));
            add(mdBelowIndex(slot, band), b + " " + tr(Str::ParamBelowThreshold), module, ParamKind::Decibel,
                kMdThresholdMinDb, 0.0, -40.0);
            add(mdUpIndex(slot, band), b + " " + tr(Str::ParamUpRatio), module, ParamKind::DynRatio, 0.0, 1.0,
                normFromDynRatio(2.0));
            add(mdGainIndex(slot, band), b + " " + tr(Str::ParamGain), module, ParamKind::Decibel, -12.0, 12.0, 0.0);
        }
        fx(FxParam::MdAttack, tr(Str::ParamMdAttack), ParamKind::Time, 0.0, normFromTime(0.5), normFromTime(0.01));
        fx(FxParam::MdRelease, tr(Str::ParamMdRelease), ParamKind::Time, 0.0, normFromTime(2.0), normFromTime(0.15));
        fx(FxParam::MdKnee, tr(Str::ParamMdKnee), ParamKind::Decibel, 0.0, 24.0, 6.0);
        fx(FxParam::MdRange, tr(Str::ParamMdRange), ParamKind::Decibel, 0.0, 36.0, 12.0);
        fx(FxParam::MdMix, tr(Str::ParamMdMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::MdOutput, tr(Str::ParamMdOutput), ParamKind::Decibel, -24.0, 24.0, 0.0);
        fx(FxParam::MdLink, tr(Str::ParamMdLink), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::MdRms, tr(Str::ParamMdRms), ParamKind::Toggle, 0.0, 1.0, 1.0);
        fx(FxParam::ShaperLink, tr(Str::ParamShaperLink), ParamKind::Toggle, 0.0, 1.0,
            1.0); // turning the drive keeps the level
        fx(FxParam::ShaperSymmetric, tr(Str::ParamShaperSymmetric), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::ReverbType, tr(Str::ParamReverbType), ParamKind::ReverbType, 0.0,
           static_cast<double>(ReverbType::Count) - 1.0, static_cast<double>(ReverbType::Hall));

        // A medium room, a little dark, held back 20 ms.
        fx(FxParam::ReverbSize, tr(Str::ParamReverbSize), ParamKind::Percent, 0.0, 1.0, 0.6);
        fx(FxParam::ReverbDecay, tr(Str::ParamReverbDecay), ParamKind::Time, normFromTime(0.2), 1.0, normFromTime(2.5));
        fx(FxParam::ReverbDamping, tr(Str::ParamReverbDamping), ParamKind::Percent, 0.0, 1.0, 0.4);
        fx(FxParam::ReverbPreDelay, tr(Str::ParamReverbPreDelay), ParamKind::TimeOrOff, 0.0, normFromTime(0.25),
           normFromTime(0.02));
        fx(FxParam::ReverbWidth, tr(Str::ParamReverbWidth), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::ReverbMix, tr(Str::ParamReverbMix), ParamKind::Percent, 0.0, 1.0, 0.3);
        // A shimmer climbs an octave each time round, half of what goes round shifted.
        fx(FxParam::ReverbShimmerPitch, tr(Str::ParamReverbShimmerPitch), ParamKind::Transpose, -24.0, 24.0, 12.0);
        fx(FxParam::ReverbShimmer, tr(Str::ParamReverbShimmer), ParamKind::Percent, 0.0, 1.0, 0.5);

        fx(FxParam::PhaserRate, tr(Str::ParamPhaserRate), ParamKind::SweepRate, 0.0, normFromRate(20.0),
            normFromRate(0.4));
        fx(FxParam::PhaserDepth, tr(Str::ParamPhaserDepth), ParamKind::Percent, 0.0, 1.0, 0.6);
        fx(FxParam::PhaserCenter, tr(Str::ParamPhaserCenter), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(700.0));
        fx(FxParam::PhaserFeedback, tr(Str::ParamPhaserFeedback), ParamKind::BipolarPercent, -1.0, 1.0, 0.5);
        fx(FxParam::PhaserStages, tr(Str::ParamPhaserStages), ParamKind::Count, 2.0, kMaxPhaserStages, 4.0);
        fx(FxParam::PhaserMix, tr(Str::ParamPhaserMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::PhaserStereo, tr(Str::ParamPhaserStereo), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::PhaserNegative, tr(Str::ParamPhaserNegative), ParamKind::Toggle, 0.0, 1.0, 0.0);
        // Synced, a bar a sweep.
        fx(FxParam::PhaserSync, tr(Str::ParamPhaserSync), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::PhaserDivision, tr(Str::ParamPhaserDivision), ParamKind::SyncDivision, 0.0, kDivisions - 1.0,
            kWhole);

        fx(FxParam::DisperserFreq, tr(Str::ParamDisperserFreq), ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(600.0));
        fx(FxParam::DisperserStages, tr(Str::ParamDisperserStages), ParamKind::Count, 1.0, kMaxDisperserStages, 8.0);
        fx(FxParam::DisperserPinch, tr(Str::ParamDisperserPinch), ParamKind::Percent, 0.0, 1.0, 0.5);
        fx(FxParam::DisperserMix, tr(Str::ParamDisperserMix), ParamKind::Percent, 0.0, 1.0, 1.0);

        // Wide from the start, the lows left in the middle: the sides above 150 Hz, half in.
        fx(FxParam::WidenerSize, tr(Str::ParamWidenerSize), ParamKind::Percent, 0.0, 1.0, 0.5);
        fx(FxParam::WidenerWidth, tr(Str::ParamWidenerWidth), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::WidenerHighPass, tr(Str::ParamWidenerHighPass), ParamKind::Cutoff, 0.0,
           normFromCutoff(kMaxWidenerHighPass), normFromCutoff(150.0));
        fx(FxParam::WidenerMix, tr(Str::ParamWidenerMix), ParamKind::Percent, 0.0, 1.0, 0.5);

        fx(FxParam::FlangerRate, tr(Str::ParamFlangerRate), ParamKind::SweepRate, 0.0, normFromRate(20.0),
            normFromRate(0.25));
        fx(FxParam::FlangerDepth, tr(Str::ParamFlangerDepth), ParamKind::Percent, 0.0, 1.0, 0.7);
        fx(FxParam::FlangerDelay, tr(Str::ParamFlangerDelay), ParamKind::Time, 0.0, normFromTime(0.015),
            normFromTime(0.003));
        fx(FxParam::FlangerFeedback, tr(Str::ParamFlangerFeedback), ParamKind::BipolarPercent, -1.0, 1.0, 0.5);
        // Half wet at full Mix, like the phaser.
        fx(FxParam::FlangerMix, tr(Str::ParamFlangerMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        // Stereo to begin with, unlike the phaser: the classic flanger's sides sweep apart.
        fx(FxParam::FlangerStereo, tr(Str::ParamFlangerStereo), ParamKind::Toggle, 0.0, 1.0, 1.0);
        fx(FxParam::FlangerNegative, tr(Str::ParamFlangerNegative), ParamKind::Toggle, 0.0, 1.0, 0.0);

        // A crunchy amp at its middle settings into a closed 4x12, a dynamic mic close to it, a little off its center.
        fx(FxParam::AmpModel, tr(Str::ParamAmpModel), ParamKind::AmpModel, 0.0,
           static_cast<double>(AmpModel::Count) - 1.0, static_cast<double>(AmpModel::Crunch));
        fx(FxParam::AmpGain, tr(Str::ParamAmpGain), ParamKind::AmpDial, 0.0, 1.0, 0.5);
        fx(FxParam::AmpBass, tr(Str::ParamAmpBass), ParamKind::AmpDial, 0.0, 1.0, 0.5);
        fx(FxParam::AmpMid, tr(Str::ParamAmpMid), ParamKind::AmpDial, 0.0, 1.0, 0.5);
        fx(FxParam::AmpTreble, tr(Str::ParamAmpTreble), ParamKind::AmpDial, 0.0, 1.0, 0.5);
        fx(FxParam::AmpPresence, tr(Str::ParamAmpPresence), ParamKind::AmpDial, 0.0, 1.0, 0.0);
        fx(FxParam::AmpVolume, tr(Str::ParamAmpVolume), ParamKind::AmpDial, 0.0, 1.0, 0.5);
        fx(FxParam::AmpOutput, tr(Str::ParamAmpOutput), ParamKind::Decibel, -24.0, 12.0, 0.0);
        fx(FxParam::AmpMix, tr(Str::ParamAmpMix), ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::AmpStereo, tr(Str::ParamAmpStereo), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::AmpCabOn, tr(Str::ParamAmpCabOn), ParamKind::Toggle, 0.0, 1.0, 1.0);
        fx(FxParam::AmpCab, tr(Str::ParamAmpCab), ParamKind::AmpCab, 0.0, static_cast<double>(AmpCab::Count) - 1.0,
           static_cast<double>(AmpCab::Closed4x12));
        fx(FxParam::AmpMic, tr(Str::ParamAmpMic), ParamKind::AmpMic, 0.0, static_cast<double>(AmpMic::Count) - 1.0,
           static_cast<double>(AmpMic::Dynamic));
        fx(FxParam::AmpPosition, tr(Str::ParamAmpPosition), ParamKind::Percent, 0.0, 1.0, 0.3);
        fx(FxParam::AmpDistance, tr(Str::ParamAmpDistance), ParamKind::AmpDistance, 0.0, 1.0,
           normFromAmpDistance(2.0));
        fx(FxParam::AmpOversampling, tr(Str::ParamAmpOversampling), ParamKind::AmpOversampling, 0.0,
           kAmpOversamplings - 1.0, kDefaultAmpOversampling);
        // Synced, two bars a sweep.
        fx(FxParam::FlangerSync, tr(Str::ParamFlangerSync), ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::FlangerDivision, tr(Str::ParamFlangerDivision), ParamKind::SyncDivision, 0.0, kDivisions - 1.0,
            kTwoWhole);

        // No shift until turned, then up to 100 Hz either way.
        fx(FxParam::ShiftRange, tr(Str::ParamShiftRange), ParamKind::ShiftRange, 0.0, 1.0, normFromShiftRange(100.0));
        fx(FxParam::ShiftAmount, tr(Str::ParamShiftAmount), ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
        fx(FxParam::ShiftSpread, tr(Str::ParamShiftSpread), ParamKind::Percent, 0.0, 1.0, 0.0);
        fx(FxParam::ShiftFeedback, tr(Str::ParamShiftFeedback), ParamKind::Percent, 0.0, 0.9, 0.0);
        fx(FxParam::ShiftMix, tr(Str::ParamShiftMix), ParamKind::Percent, 0.0, 1.0, 1.0);
    }

    // Sources start empty; the editor gives a place its type. Host names stay
    // with the place, whichever type it holds.
    for (int source = 0; source < kMaxSources; ++source) {
        const std::string name = trf(Str::ParamMod, {std::to_string(source + 1)});
        const std::string module = tr(Str::ModuleModulation) + ("/" + name);
        auto field = [&](SourceParam f, const std::string &label, ParamKind kind, double min, double max, double def) {
            add(paramIndex(source, f), name + " " + label, module, kind, min, max, def);
        };
        field(SourceParam::Type, tr(Str::ParamType), ParamKind::SourceType, 0.0,
            static_cast<double>(SourceType::Count) - 1.0, 0.0);
        field(SourceParam::Enabled, tr(Str::ParamOn), ParamKind::Toggle, 0.0, 1.0, 1.0);
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotTargetIndex(source, slot), name + " " + trf(Str::ParamTarget, {n}), module, ParamKind::ModTarget,
                0.0, kMaxParamId + 1.0, 0.0);
            add(slotAmountIndex(source, slot), name + " " + trf(Str::ParamAmount, {n}), module,
                ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
        }

        field(SourceParam::Shape, tr(Str::ParamShape), ParamKind::LfoShape, 0.0,
            static_cast<double>(LfoShape::Count) - 1.0, 0.0);
        field(SourceParam::Rate, tr(Str::ParamRate), ParamKind::Rate, 0.0, 1.0, normFromRate(2.0));
        field(SourceParam::Sync, tr(Str::ParamSync), ParamKind::Toggle, 0.0, 1.0,
            1.0); // an LFO starts on quarter notes
        field(SourceParam::Division, tr(Str::ParamDivision), ParamKind::SyncDivision, 0.0, kDivisions - 1.0, kQuarter);
        field(SourceParam::Free, tr(Str::ParamFree), ParamKind::Toggle, 0.0, 1.0, 0.0);
        field(SourceParam::Unipolar, tr(Str::ParamUnipolar), ParamKind::Toggle, 0.0, 1.0, 0.0);
        field(SourceParam::OneShot, tr(Str::ParamOneShot), ParamKind::Toggle, 0.0, 1.0, 0.0);
        // Random starts as plain sample and hold: steps, each landing anywhere.
        field(SourceParam::RandomSmooth, tr(Str::ParamSmooth), ParamKind::Percent, 0.0, 1.0, 0.0);
        field(SourceParam::RandomChaos, tr(Str::ParamChaos), ParamKind::Percent, 0.0, 1.0, 1.0);

        field(SourceParam::Attack, tr(Str::ParamAttack), ParamKind::EnvTime, kMinEnvNorm, kMaxEnvNorm, 0.0);
        field(SourceParam::Decay, tr(Str::ParamDecay), ParamKind::EnvTime, kMinEnvNorm, kMaxEnvNorm,
              normFromTime(0.5));
        field(SourceParam::Sustain, tr(Str::ParamSustain), ParamKind::Percent, 0.0, 1.0, 1.0);
        // The release an operator's envelope starts with.
        field(SourceParam::Release, tr(Str::ParamRelease), ParamKind::EnvTime, kMinEnvNorm, kMaxEnvNorm,
              normFromTime(0.015));
        field(SourceParam::Hold, tr(Str::ParamHold), ParamKind::TimeOrOff, 0.0, kMaxEnvNorm, 0.0);
        addCurves(paramIndex(source, SourceParam::AttackCurve), name, module);
        field(SourceParam::EnvBipolar, tr(Str::ParamBipolar), ParamKind::Toggle, 0.0, 1.0,
            0.0); // takes its targets one way
        field(SourceParam::Phase, tr(Str::ParamPhase), ParamKind::Percent, 0.0, 1.0, 0.0);
        // Random's and Midi's: following what comes in at once, from the mod wheel's controller, one way.
        field(SourceParam::Slew, tr(Str::ParamSlew), ParamKind::Time, 0.0, kMaxEnvNorm, 0.0);
        field(SourceParam::CcNumber, tr(Str::ParamCc), ParamKind::Count, 0.0, 127.0, 1.0);
        field(SourceParam::Bipolar, tr(Str::ParamBipolar), ParamKind::Toggle, 0.0, 1.0, 0.0);
        field(SourceParam::MidiInput, tr(Str::ParamInput), ParamKind::MidiInput, 0.0,
              static_cast<double>(MidiInput::Count) - 1.0, 0.0);
        field(SourceParam::Anchor, tr(Str::ParamAnchor), ParamKind::Toggle, 0.0, 1.0, 0.0);

        // Freeform shapes start as a triangle: up from the bottom to the top at the middle of the
        // cycle, and down again as the last segment wraps around.
        const std::string shape = name + " " + tr(Str::ParamShape);
        add(shapeCountIndex(source), shape + " " + tr(Str::ParamPoints), module, ParamKind::ShapeCount, 2.0,
            kMaxShapePoints, 2.0);
        const double defaults[2][2] = {{0.0, -1.0}, {0.5, 1.0}};
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = shape + " " + std::to_string(point + 1);
            const bool used = point < 2;
            add(shapePointIndex(source, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? defaults[point][0] : 1.0);
            add(shapePointIndex(source, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? defaults[point][1] : 0.0);
            add(shapePointIndex(source, point, PointField::Curve), n + " " + tr(Str::ParamCurve), module,
                ParamKind::ShapePoint, -1.0, 1.0, 0.0);
        }
        for (int slot = 0; slot < kSlots; ++slot)
            add(slotBypassIndex(source, slot), name + " " + trf(Str::ParamBypass, {std::to_string(slot + 1)}), module,
                ParamKind::SlotBypass, 0.0, 1.0, 0.0);
    }
}

// snprintf cuts text that doesn't fit at the buffer's end, which can split a
// multi-byte character such as "→"; hosts reject that (Bitwig fails to create
// the plugin), so a partial last character goes.
void dropPartialCharacter(char *text) {
    const size_t length = std::strlen(text);
    size_t start = length; // where the last character starts
    while (start > 0 && (static_cast<unsigned char>(text[start - 1]) & 0xC0) == 0x80)
        --start;
    if (start == 0)
        return;
    const auto lead = static_cast<unsigned char>(text[start - 1]);
    const size_t size = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
    if (length - (start - 1) < size)
        text[start - 1] = '\0';
}

void formatHz(double hz, char *out, uint32_t capacity) {
    hz = std::round(hz);
    if (hz < 1000.0)
        std::snprintf(out, capacity, "%.0f Hz", hz);
    else
        std::snprintf(out, capacity, "%.2f kHz", hz / 1000.0);
}

// "300", "300 Hz", "1.2k", "1.2 kHz" -> Hz; 0 if there is no number.
double parseHz(const char *text) {
    char *end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || !std::isfinite(value))
        return 0.0;
    while (*end == ' ')
        ++end;
    return *end == 'k' || *end == 'K' ? value * 1000.0 : value;
}

bool equalsIgnoreCase(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    return *a == *b;
}

template <size_t N>
int findName(const char *const (&names)[N], const char *text) {
    for (size_t i = 0; i < N; ++i)
        if (equalsIgnoreCase(text, names[i]))
            return static_cast<int>(i);
    return -1;
}
template <size_t N>
int findName(const Str (&names)[N], const char *text) {
    for (size_t i = 0; i < N; ++i)
        if (equalsIgnoreCase(text, tr(names[i])))
            return static_cast<int>(i);
    return -1;
}

} // namespace

const char *waveName(OpWave wave) {
    return nameAt(kWaveNames, static_cast<double>(wave));
}

const char *filterTypeName(FilterType type) {
    return nameAt(kFilterTypeNames, static_cast<double>(type));
}

const char *filterTypeShortName(FilterType type) {
    return nameAt(kFilterTypeShortNames, static_cast<double>(type));
}

const char *divisionName(int division) {
    return nameAt(kDivisionNames, division);
}

const char *lfoShapeName(LfoShape shape) {
    return nameAt(kLfoShapeNames, static_cast<double>(shape));
}

const char *clipModeName(ClipMode mode) {
    return nameAt(kClipModeNames, static_cast<double>(mode));
}

const char *peqTypeName(PeqType type) {
    return nameAt(kPeqTypeNames, static_cast<double>(type));
}

const char *sourceTypeName(SourceType type) {
    return nameAt(kSourceTypeNames, static_cast<double>(type));
}

const char *reverbTypeName(ReverbType type) {
    return nameAt(kReverbTypeNames, static_cast<double>(type));
}

const char *ampModelName(AmpModel model) {
    return nameAt(kAmpModelNames, static_cast<double>(model));
}

const char *ampCabName(AmpCab cab) {
    return nameAt(kAmpCabNames, static_cast<double>(cab));
}

const char *ampMicName(AmpMic mic) {
    return nameAt(kAmpMicNames, static_cast<double>(mic));
}

const char *routeModeName(RouteMode mode) {
    return nameAt(kRouteModeNames, static_cast<double>(mode));
}

const char *midiInputName(MidiInput input) {
    return nameAt(kMidiInputNames, static_cast<double>(input));
}

const char *fxName(FxType fx) {
    return nameAt(kFxNames, static_cast<double>(fx));
}

std::string fxPortName(int port) {
    if (port < 0 || port >= kFxPorts)
        return tr(Str::ValueNone);
    if (port == kFxPortIn)
        return tr(Str::ValueFxIn);
    if (const int slot = fxPortSlot(port); slot >= 0)
        return trf(Str::ParamFx, {std::to_string(slot + 1)});
    if (const int sum = fxPortSum(port); sum >= 0)
        return trf(Str::ParamSum, {std::to_string(sum + 1)});
    const Str bands[kFxBands] = {Str::BandLow, Str::BandMid, Str::BandHigh};
    return trf(Str::ParamSplit, {std::to_string(fxPortSplit(port) + 1)}) + " " + tr(bands[fxPortBand(port)]);
}

const ParamTable &paramTable() {
    static const std::unique_ptr<const ParamTable> table = [] {
        auto t = std::make_unique<ParamTable>();
        buildTable(*t);
        return t;
    }();
    return *table;
}

namespace {
struct HostList {
    std::vector<int> indexes;          // by host index
    std::array<int, kParamCount> host; // each param's host index, -1 for none
};

const HostList &hostList() {
    static const std::unique_ptr<const HostList> list = [] {
        auto l = std::make_unique<HostList>();
        const ParamTable &table = paramTable();
        // Every source's slots: what each modulates, its aux, curve and bypass are set in the editor; its depth and
        // Amount hosts may automate (see hostParamCount()).
        std::vector<bool> wiring(kParamCount, false), kept(kParamCount, false);
        for (int source = 0; source < kAllSources; ++source)
            for (int s = 0; s < kSlots; ++s) {
                for (int i : {slotTargetIndex(source, s), slotAuxIndex(source, s), slotCurveIndex(source, s),
                              slotBypassIndex(source, s)})
                    wiring[i] = true;
                kept[slotAmountIndex(source, s)] = kept[slotScaleIndex(source, s)] = true;
            }
        for (int i = 0; i < kParamCount; ++i) {
            const bool listed = kept[i] || (!(table[i].flags & CLAP_PARAM_IS_HIDDEN) && !wiring[i] &&
                                            fxSlotOf(i) < 0 && sourceOf(i) < 0 && !isFxGraphParam(i));
            l->host[i] = listed ? static_cast<int>(l->indexes.size()) : -1;
            if (listed)
                l->indexes.push_back(i);
        }
        return l;
    }();
    return *list;
}
} // namespace

int hostParamCount() { return static_cast<int>(hostList().indexes.size()); }

int hostParamIndex(int hostIndex) {
    const HostList &l = hostList();
    return hostIndex >= 0 && hostIndex < static_cast<int>(l.indexes.size()) ? l.indexes[hostIndex] : -1;
}

bool hostListed(int index) { return index >= 0 && index < kParamCount && hostList().host[index] >= 0; }

bool isRetired(int index) {
    if (index < kOpBase || index >= kOpBase + kOps * kOpParamCount)
        return false;
    const auto field = static_cast<OpParam>((index - kOpBase) % kOpParamCount);
    return field == OpParam::RetiredCoarse || field == OpParam::RetiredFine || field == OpParam::RetiredVelocity;
}

bool isStepped(ParamKind kind) {
    return kind == ParamKind::Ratio || kind == ParamKind::Semitones || kind == ParamKind::Count ||
           kind == ParamKind::VoiceCount || kind == ParamKind::Toggle || kind == ParamKind::LfoShape ||
           kind == ParamKind::OpWave || kind == ParamKind::FilterType || kind == ParamKind::ModTarget ||
           kind == ParamKind::SyncDivision || kind == ParamKind::ClipMode || kind == ParamKind::FxOrder ||
           kind == ParamKind::FxPosition || kind == ParamKind::FxPort || kind == ParamKind::FxType || kind == ParamKind::SourceType || kind == ParamKind::ShapeCount ||
           kind == ParamKind::FilterSlope || kind == ParamKind::SlotBypass || kind == ParamKind::PeqType ||
           kind == ParamKind::ReverbType || kind == ParamKind::FixedMult || kind == ParamKind::SlotAux ||
           kind == ParamKind::MidiInput || kind == ParamKind::RouteMode || kind == ParamKind::AmpModel ||
           kind == ParamKind::AmpCab || kind == ParamKind::AmpMic || kind == ParamKind::AmpOversampling;
}

bool isBipolar(ParamKind kind) {
    return kind == ParamKind::BipolarPercent || kind == ParamKind::Cents || kind == ParamKind::SemitoneOffset ||
           kind == ParamKind::Transpose || kind == ParamKind::Pan || kind == ParamKind::EqGain || kind == ParamKind::DelayTone;
}

bool formatParam(int index, double value, char *out, uint32_t capacity) {
    if (index < 0 || index >= kParamCount || capacity == 0)
        return false;
    switch (paramTable()[index].kind) {
    case ParamKind::Decibel:
        std::snprintf(out, capacity, "%.1f dB", value);
        break;
    case ParamKind::Percent:
        std::snprintf(out, capacity, "%.1f %%", value * 100.0);
        break;
    case ParamKind::BipolarPercent:
        std::snprintf(out, capacity, "%+.1f %%", value * 100.0);
        break;
    case ParamKind::TimeOrOff:
        if (value <= 0.0) {
            std::snprintf(out, capacity, "%s", tr(Str::ValueOff));
            break;
        }
        [[fallthrough]];
    case ParamKind::Time:
    case ParamKind::EnvTime: {
        const double seconds = paramTable()[index].kind == ParamKind::EnvTime ? envTimeFromNorm(value) : timeFromNorm(value);
        const double ms = std::round(seconds * 10000.0) / 10.0;
        if (ms < 1000.0)
            std::snprintf(out, capacity, "%.1f ms", ms);
        else
            std::snprintf(out, capacity, "%.2f s", seconds);
        break;
    }
    case ParamKind::Toggle:
    case ParamKind::SlotBypass:
        std::snprintf(out, capacity, "%s", value >= 0.5 ? tr(Str::ValueOn) : tr(Str::ValueOff));
        break;
    case ParamKind::Rate:
    case ParamKind::SweepRate:
        std::snprintf(out, capacity, "%.2f Hz",
                      paramTable()[index].kind == ParamKind::SweepRate ? sweepRateHz(value) : rateFromNorm(value));
        break;
    case ParamKind::Ratio:
        if (std::lround(value) <= 0)
            std::snprintf(out, capacity, "0.5");
        else
            std::snprintf(out, capacity, "%ld", std::lround(value));
        break;
    case ParamKind::Cents:
        std::snprintf(out, capacity, "%+.1f ct", value);
        break;
    case ParamKind::CentsAmount:
        std::snprintf(out, capacity, "%.1f ct", value);
        break;
    case ParamKind::Semitones:
        std::snprintf(out, capacity, "%ld st", std::lround(value));
        break;
    case ParamKind::Transpose: // whole semitones as set in the editor; in between, from automation
        if (std::abs(value - std::round(value)) < 0.005)
            std::snprintf(out, capacity, "%+ld st", std::lround(value));
        else
            std::snprintf(out, capacity, "%+.2f st", value);
        break;
    case ParamKind::Count:
    case ParamKind::VoiceCount:
        std::snprintf(out, capacity, "%ld", std::lround(value));
        break;
    case ParamKind::SemitoneOffset:
        std::snprintf(out, capacity, "%+.2f st", value);
        break;
    case ParamKind::Pan: {
        const long percent = std::lround(value * 100.0);
        if (percent == 0)
            std::snprintf(out, capacity, "C");
        else
            std::snprintf(out, capacity, "%c%ld", percent < 0 ? 'L' : 'R', std::labs(percent));
        break;
    }
    case ParamKind::Cutoff:
        formatHz(cutoffFromNorm(value), out, capacity);
        break;
    case ParamKind::DelayTone: {
        // Within 1 % of the middle, neither filter is noticeable.
        if (std::abs(value) < 0.01) {
            std::snprintf(out, capacity, "%s", tr(Str::ValueOpen));
            break;
        }
        const DelayTone tone = delayTone(value);
        char hz[32];
        formatHz(value < 0.0 ? tone.lowPassHz : tone.highPassHz, hz, sizeof(hz));
        std::snprintf(out, capacity, "%s %s", value < 0.0 ? "LP" : "HP", hz);
        break;
    }
    case ParamKind::Q:
        std::snprintf(out, capacity, "Q %.2f", qFromNorm(value));
        break;
    case ParamKind::CompRatio:
        std::snprintf(out, capacity, "%.1f:1", value);
        break;
    case ParamKind::DynRatio:
        if (value >= 1.0)
            std::snprintf(out, capacity, "\xE2\x88\x9E:1"); // infinity
        else
            std::snprintf(out, capacity, dynRatio(value) < 100.0 ? "%.1f:1" : "%.0f:1", dynRatio(value));
        break;
    case ParamKind::FxType:
        std::snprintf(out, capacity, "%s", nameAt(kFxNames, value));
        break;
    case ParamKind::SourceType:
        std::snprintf(out, capacity, "%s", nameAt(kSourceTypeNames, value));
        break;
    case ParamKind::SlotAux:
        std::snprintf(out, capacity, "%s", auxSourceName(slotAuxSource(value)).c_str());
        break;
    case ParamKind::LfoShape:
        std::snprintf(out, capacity, "%s", nameAt(kLfoShapeNames, value));
        break;
    case ParamKind::OpWave:
        std::snprintf(out, capacity, "%s", nameAt(kWaveNames, value));
        break;
    case ParamKind::FilterType:
        std::snprintf(out, capacity, "%s", nameAt(kFilterTypeNames, value));
        break;
    case ParamKind::FilterSlope:
        std::snprintf(out, capacity, "%s", nameAt(kFilterSlopeNames, value));
        break;
    case ParamKind::FixedMult:
        std::snprintf(out, capacity, "\xC3\x97%g", fixedMult(value)); // x0.001 ... x10
        break;
    case ParamKind::ModTarget: {
        const int target = slotTarget(value);
        std::snprintf(out, capacity, "%s", target < 0 ? tr(Str::ValueNone) : paramTable()[target].name.c_str());
        break;
    }
    case ParamKind::SyncDivision:
        std::snprintf(out, capacity, "%s", nameAt(kDivisionNames, value));
        break;
    case ParamKind::ClipMode:
        std::snprintf(out, capacity, "%s", nameAt(kClipModeNames, value));
        break;
    case ParamKind::PeqType:
        std::snprintf(out, capacity, "%s", nameAt(kPeqTypeNames, value));
        break;
    case ParamKind::ReverbType:
        std::snprintf(out, capacity, "%s", nameAt(kReverbTypeNames, value));
        break;
    case ParamKind::AmpModel:
        std::snprintf(out, capacity, "%s", nameAt(kAmpModelNames, value));
        break;
    case ParamKind::AmpCab:
        std::snprintf(out, capacity, "%s", nameAt(kAmpCabNames, value));
        break;
    case ParamKind::AmpMic:
        std::snprintf(out, capacity, "%s", nameAt(kAmpMicNames, value));
        break;
    case ParamKind::AmpOversampling:
        std::snprintf(out, capacity, "%dx", ampOversampling(value));
        break;
    case ParamKind::AmpDial:
        std::snprintf(out, capacity, "%.1f", value * 10.0);
        break;
    case ParamKind::AmpDistance: { // by what it rounds to, so 99.96 cm shows as 1.00 m, as typing that gives
        const double cm = ampDistanceCm(value);
        if (cm < 9.995)
            std::snprintf(out, capacity, "%.2f cm", cm);
        else if (cm < 99.95)
            std::snprintf(out, capacity, "%.1f cm", cm);
        else
            std::snprintf(out, capacity, "%.2f m", cm / 100.0);
        break;
    }
    case ParamKind::RouteMode:
        std::snprintf(out, capacity, "%s", nameAt(kRouteModeNames, value));
        break;
    case ParamKind::MidiInput:
        std::snprintf(out, capacity, "%s", nameAt(kMidiInputNames, value));
        break;
    case ParamKind::FxOrder:
        std::snprintf(out, capacity, "%s", fxOrderName(value).c_str());
        break;
    case ParamKind::FxPosition: // the slot, by its number
        std::snprintf(out, capacity, "%ld", std::lround(value) + 1);
        break;
    case ParamKind::FxPort:
        std::snprintf(out, capacity, "%s", fxPortName(fxPortOf(value)).c_str());
        break;
    case ParamKind::ShapeCount:
        std::snprintf(out, capacity, "%ld", std::lround(value));
        break;
    case ParamKind::ShapePoint:
        std::snprintf(out, capacity, "%.3f", value);
        break;
    case ParamKind::NoiseSlope: {
        // Named at the three classic slopes, in dB per octave in between.
        const double db = std::round(value * 60.0) / 10.0;
        const char *const name = db == 0.0   ? tr(Str::NoiseWhite)
                                 : db == 3.0 ? tr(Str::NoisePink)
                                 : db == 6.0 ? tr(Str::NoiseBrown)
                                             : nullptr;
        if (name)
            std::snprintf(out, capacity, "%s", name);
        else
            std::snprintf(out, capacity, "-%.1f dB/oct", db);
        break;
    }
    case ParamKind::EqGain: {
        const double gain = eqGain(value);
        const double db = 20.0 * std::log10(gain);
        if (gain < 1e-4)
            std::snprintf(out, capacity, "%s", tr(Str::ValueKill));
        else
            std::snprintf(out, capacity, "%+.1f dB", std::abs(db) < 0.05 ? 0.0 : db); // never "-0.0"
        break;
    }
    case ParamKind::NotePitch: { // "C4", or "C4 +12" between notes, from -49 to +50 cents
        const long total = std::lround(value * 100.0);
        const long note = (total + 49 >= 0 ? total + 49 : total + 49 - 99) / 100;
        const long cents = total - note * 100;
        const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const long octave = (note < 0 ? note - 11 : note) / 12 - 1;
        const char *name = names[((note % 12) + 12) % 12];
        if (cents == 0)
            std::snprintf(out, capacity, "%s%ld", name, octave);
        else
            std::snprintf(out, capacity, "%s%ld %+ld", name, octave, cents);
        break;
    }
    case ParamKind::OpRatio:
        std::snprintf(out, capacity, "%.2f", ratioHundredths(value) / 100.0);
        break;
    case ParamKind::FixedFreq:
    case ParamKind::ShiftRange: {
        const double hz = paramTable()[index].kind == ParamKind::FixedFreq ? fixedFreqFromNorm(value)
                                                                            : shiftRangeFromNorm(value);
        // By what it rounds to, so 99.97 Hz shows as 100 Hz, as typing that gives.
        if (hz < 9.995)
            std::snprintf(out, capacity, "%.2f Hz", hz);
        else if (hz < 99.95)
            std::snprintf(out, capacity, "%.1f Hz", hz);
        else
            formatHz(hz, out, capacity);
        break;
    }
    }
    dropPartialCharacter(out);
    return true;
}

bool parseParam(int index, const char *text, double *out) {
    if (index < 0 || index >= kParamCount)
        return false;
    const ParamDef &def = paramTable()[index];

    const int named = def.kind == ParamKind::LfoShape       ? findName(kLfoShapeNames, text)
                      : def.kind == ParamKind::OpWave       ? findName(kWaveNames, text)
                      : def.kind == ParamKind::FilterType   ? std::max(findName(kFilterTypeNames, text),
                                                                       findName(kFilterTypeShortNames, text))
                      : def.kind == ParamKind::SyncDivision ? findName(kDivisionNames, text)
                      : def.kind == ParamKind::ClipMode     ? findName(kClipModeNames, text)
                      : def.kind == ParamKind::FxType       ? findName(kFxNames, text)
                      : def.kind == ParamKind::SourceType   ? findName(kSourceTypeNames, text)
                      : def.kind == ParamKind::FilterSlope  ? findName(kFilterSlopeNames, text)
                      : def.kind == ParamKind::PeqType      ? findName(kPeqTypeNames, text)
                      : def.kind == ParamKind::ReverbType   ? findName(kReverbTypeNames, text)
                      : def.kind == ParamKind::AmpModel     ? findName(kAmpModelNames, text)
                      : def.kind == ParamKind::AmpCab       ? findName(kAmpCabNames, text)
                      : def.kind == ParamKind::AmpMic       ? findName(kAmpMicNames, text)
                      : def.kind == ParamKind::MidiInput    ? findName(kMidiInputNames, text)
                      : def.kind == ParamKind::RouteMode    ? findName(kRouteModeNames, text)
                                                            : -1;
    if (named >= 0) {
        *out = named;
        return true;
    }
    if (def.kind == ParamKind::SlotAux) { // a source's name as formatParam() writes it, or None
        for (int source = -1; source < kAllSources; ++source)
            if (equalsIgnoreCase(text, auxSourceName(source).c_str())) {
                *out = slotAuxValue(source);
                return true;
            }
        return false;
    }
    if (def.kind == ParamKind::Toggle || def.kind == ParamKind::SlotBypass || def.kind == ParamKind::TimeOrOff) {
        if (equalsIgnoreCase(text, tr(Str::ValueOff))) {
            *out = 0.0;
            return true;
        }
        if (def.kind != ParamKind::TimeOrOff && equalsIgnoreCase(text, tr(Str::ValueOn))) {
            *out = 1.0;
            return true;
        }
    }
    if (def.kind == ParamKind::EqGain && equalsIgnoreCase(text, tr(Str::ValueKill))) {
        *out = -1.0;
        return true;
    }
    if (def.kind == ParamKind::SyncDivision)
        return false; // names only: "1/4" would otherwise read as the number 1
    if (def.kind == ParamKind::FxOrder)
        return parseFxOrder(text, out);
    if (def.kind == ParamKind::FxPort) { // a port's name
        for (int port = -1; port < kFxPorts; ++port)
            if (equalsIgnoreCase(text, fxPortName(port).c_str())) {
                *out = fxPortValue(port);
                return true;
            }
        return false;
    }
    if (def.kind == ParamKind::FxPosition) { // a slot's number
        char *end = nullptr;
        const long slot = std::strtol(text, &end, 10);
        if (end == text || slot < 1 || slot > kFxSlots)
            return false;
        *out = static_cast<double>(slot - 1);
        return true;
    }
    if (def.kind == ParamKind::DelayTone) {
        // "Open", "LP 2 kHz", "HP 300 Hz", or a bare percentage.
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        const char pass = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0] ? text[1] : 0)));
        if (equalsIgnoreCase(text, tr(Str::ValueOpen))) {
            *out = 0.0;
            return true;
        }
        if ((kind == 'L' || kind == 'H') && pass == 'P') {
            const double hz = parseHz(text + 2);
            if (hz <= 0.0)
                return false;
            const double norm = normFromCutoff(std::max(hz, kMinCutoff));
            // A filter typed is at least the 1 % shown as one, not Open.
            *out = kind == 'L' ? std::clamp((norm - 1.0) * 1.5, -1.0, -0.01) : std::clamp(norm / 0.77, 0.01, 1.0);
            return true;
        }
    }
    if (def.kind == ParamKind::NoiseSlope) {
        const Str names[] = {Str::NoiseWhite, Str::NoisePink, Str::NoiseBrown};
        for (int i = 0; i < 3; ++i) {
            if (equalsIgnoreCase(text, tr(names[i]))) {
                *out = i * 0.5;
                return true;
            }
        }
    }
    if (def.kind == ParamKind::Q && (text[0] == 'Q' || text[0] == 'q'))
        ++text;
    if (def.kind == ParamKind::NotePitch) { // "C4", "C#4", "Db4", "C-1", each with cents after it if any
        const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        constexpr int kLetters[] = {9, 11, 0, 2, 4, 5, 7}; // A to G
        if (letter >= 'A' && letter <= 'G') {
            int note = kLetters[letter - 'A'];
            const char *c = text + 1;
            if (*c == '#')
                ++note, ++c;
            else if (*c == 'b')
                --note, ++c;
            char *end = nullptr;
            // Held to a few octaves past the range's ends, so a long run of digits can't overflow.
            const long octave = std::clamp(std::strtol(c, &end, 10), -10L, 20L);
            if (end == c)
                return false;
            double cents = 0.0;
            while (*end == ' ')
                ++end;
            if (*end == '+' || *end == '-')
                cents = std::strtod(end, nullptr);
            if (!std::isfinite(cents))
                return false;
            *out = std::clamp((octave + 1) * 12.0 + note + cents / 100.0, def.min, def.max);
            return true;
        }
    }
    if (def.kind == ParamKind::ModTarget) {
        if (equalsIgnoreCase(text, tr(Str::ValueNone))) {
            *out = 0.0;
            return true;
        }
        for (int i = 0; i < kParamCount; ++i) {
            if (isModTarget(i) && equalsIgnoreCase(text, paramTable()[i].name.c_str())) {
                *out = slotTargetValue(i);
                return true;
            }
        }
        return false;
    }
    if (def.kind == ParamKind::Pan) {
        // "C", "L30", "R30"
        const char side = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        if (side == 'C' && text[1] == '\0') {
            *out = 0.0;
            return true;
        }
        if (side == 'L' || side == 'R') {
            const double percent = std::strtod(text + 1, nullptr);
            if (!std::isfinite(percent))
                return false;
            *out = std::clamp((side == 'L' ? -percent : percent) / 100.0, -1.0, 1.0);
            return true;
        }
    }

    if (def.kind == ParamKind::DynRatio && // "∞", "∞:1" or "inf" is the top of the range
        (std::strncmp(text, "\xE2\x88\x9E", 3) == 0 || std::strncmp(text, "inf", 3) == 0 ||
         std::strncmp(text, "Inf", 3) == 0)) {
        *out = 1.0;
        return true;
    }
    if (def.kind == ParamKind::FixedMult || def.kind == ParamKind::OpRatio) { // "×10", "x10" or "10"
        if (std::strncmp(text, "\xC3\x97", 2) == 0)
            text += 2;
        else if (*text == 'x' || *text == 'X')
            ++text;
    }
    char *end = nullptr;
    double value = std::strtod(text, &end);
    if (end == text || !std::isfinite(value))
        return false;
    while (*end == ' ')
        ++end;

    switch (def.kind) {
    case ParamKind::Percent:
    case ParamKind::BipolarPercent:
    case ParamKind::DelayTone:
        value /= 100.0;
        break;
    case ParamKind::CompRatio: // "4:1", "4.0:1" or "4"
        break;
    case ParamKind::DynRatio: // likewise
        value = normFromDynRatio(std::max(value, 1.0));
        break;
    case ParamKind::Q:
        value = normFromQ(std::max(value, kMinQ));
        break;
    case ParamKind::Time:
    case ParamKind::TimeOrOff:
    case ParamKind::EnvTime: {
        // "s" means seconds, anything else (including a bare number) milliseconds.
        const double seconds = *end == 's' || *end == 'S' ? value : value / 1000.0;
        if (def.kind == ParamKind::Time)
            value = normFromTime(std::max(seconds, kMinTime));
        else if (def.kind == ParamKind::EnvTime) // on down to 0 ms
            value = normFromEnvTime(seconds);
        else // below the shortest time, off; the shortest is above 0, which is Off
            value = seconds < kMinTime ? 0.0 : std::max(normFromTime(seconds), kLeastNorm);
        break;
    }
    case ParamKind::Toggle:
    case ParamKind::SlotBypass:
        value = value >= 0.5 ? 1.0 : 0.0;
        break;
    case ParamKind::Rate:
        value = normFromRate(std::max(value, kMinRate));
        break;
    case ParamKind::SweepRate: // below the slowest LFO rate, still; the slowest is above 0, which is still
        value = value < kMinRate ? 0.0 : std::max(normFromRate(value), kLeastNorm);
        break;
    case ParamKind::Cutoff: {
        const double hz = *end == 'k' || *end == 'K' ? value * 1000.0 : value;
        value = normFromCutoff(std::max(hz, kMinCutoff));
        break;
    }
    case ParamKind::Ratio:
        value = value < 0.75 ? 0.0 : std::round(value);
        break;
    case ParamKind::Semitones:
    case ParamKind::Count:
    case ParamKind::VoiceCount:
    case ParamKind::LfoShape:
    case ParamKind::OpWave:
    case ParamKind::FilterType:
        value = std::round(value);
        break;
    case ParamKind::Pan:
        value /= 100.0; // a bare number is percent right
        break;
    case ParamKind::EqGain: // dB, inverting eqGain()
        value = value >= 0.0 ? std::pow(10.0, value / 20.0) - 1.0 : std::pow(10.0, value / 40.0) - 1.0;
        break;
    case ParamKind::ClipMode:
    case ParamKind::PeqType:
    case ParamKind::ReverbType:
    case ParamKind::AmpModel:
    case ParamKind::AmpCab:
    case ParamKind::AmpMic:
    case ParamKind::MidiInput:
    case ParamKind::RouteMode:
    case ParamKind::FxOrder:
    case ParamKind::FxPosition:
    case ParamKind::FxPort:
    case ParamKind::FxType:
    case ParamKind::SourceType:
    case ParamKind::SlotAux:
    case ParamKind::ShapeCount:
        value = std::round(value);
        break;
    case ParamKind::ShapePoint:
        break;
    case ParamKind::NoiseSlope: // dB per octave, with or without the minus
        value = std::abs(value) / 6.0;
        break;
    case ParamKind::FilterSlope: // dB per octave, to the nearest slope
        value = std::round(std::abs(value) / 6.0) - 1.0;
        break;
    case ParamKind::AmpOversampling: // "4x" or "4": the factor, to the nearest one offered
        value = std::clamp(std::round(std::log2(std::max(value, 1.0))), 0.0, kAmpOversamplings - 1.0);
        break;
    case ParamKind::AmpDial: // 0 to 10
        value /= 10.0;
        break;
    case ParamKind::AmpDistance: // cm, or m with an m
        value = normFromAmpDistance(*end == 'm' && end[1] != 'm' ? value * 100.0 : value);
        break;
    case ParamKind::ShiftRange: // Hz, or kHz with a k
        value = normFromShiftRange(*end == 'k' || *end == 'K' ? value * 1000.0 : value);
        break;
    case ParamKind::OpRatio: // "3.5", or a fraction such as "7/2"; to the hundredth the editor shows
        if (*end == '/') {
            const double below = std::strtod(end + 1, nullptr);
            if (!(below > 0.0) || !std::isfinite(below))
                return false;
            value /= below;
        }
        value = normFromRatio(std::clamp(std::round(value * 100.0) / 100.0, kMinRatio, kMaxRatio));
        break;
    case ParamKind::FixedFreq:
        value = normFromFixedFreq(*end == 'k' || *end == 'K' ? value * 1000.0 : value);
        break;
    case ParamKind::FixedMult: { // the nearest factor, on a log scale
        int nearest = 0;
        for (int k = 1; k < kFixedMultCount; ++k)
            if (std::abs(std::log10(std::max(value, 1e-6)) - std::log10(kFixedMults[k])) <
                std::abs(std::log10(std::max(value, 1e-6)) - std::log10(kFixedMults[nearest])))
                nearest = k;
        value = nearest;
        break;
    }
    case ParamKind::SyncDivision:
    case ParamKind::Transpose:
    case ParamKind::NotePitch: // a bare number is a MIDI note
    case ParamKind::Decibel:
    case ParamKind::Cents:
    case ParamKind::CentsAmount:
    case ParamKind::SemitoneOffset:
    case ParamKind::ModTarget:
        break;
    }
    *out = std::clamp(value, def.min, def.max);
    return true;
}

} // namespace fmvibe
