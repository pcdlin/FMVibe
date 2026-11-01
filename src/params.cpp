#include "params.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>

namespace fmvibe {

namespace {

const char *const kLfoShapeNames[] = {"Sine", "Triangle", "Saw", "Square", "Random", "Custom"};
static_assert(std::size(kLfoShapeNames) == static_cast<size_t>(LfoShape::Count));
const char *const kWaveNames[] = {"Sine",      "Triangle", "Saw",   "Square", "Pulse",
                                  "Half Sine", "Abs Sine", "Noise", "Custom"};
static_assert(std::size(kWaveNames) == static_cast<size_t>(OpWave::Count));
const char *const kFilterTypeNames[] = {"Low-pass", "High-pass", "Band-pass", "Notch", "Comb"};
const char *const kFilterTypeShortNames[] = {"LP", "HP", "BP", "Notch", "Comb"};
const char *const kFilterSlopeNames[] = {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct"};
static_assert(std::size(kFilterSlopeNames) == static_cast<size_t>(kFilterSlopes));
static_assert(std::size(kFilterTypeNames) == static_cast<size_t>(FilterType::Count));
static_assert(std::size(kFilterTypeShortNames) == static_cast<size_t>(FilterType::Count));
const char *const kDivisionNames[] = {"4/1D",  "4/1",  "4/1T",  "2/1D",  "2/1",  "2/1T",  "1/1D",
                                      "1/1",   "1/1T", "1/2D",  "1/2",   "1/2T", "1/4D",  "1/4",
                                      "1/4T",  "1/8D", "1/8",   "1/8T",  "1/16D", "1/16", "1/16T",
                                      "1/32D", "1/32", "1/32T", "1/64D", "1/64",  "1/64T"};
static_assert(std::size(kDivisionNames) == static_cast<size_t>(kDivisions));
const char *const kClipModeNames[] = {"Hard", "Soft", "Asymmetric", "Cubic", "Gentle", "Sine Fold", "Fold", "Custom"};
static_assert(std::size(kClipModeNames) == static_cast<size_t>(ClipMode::Count));
const char *const kPeqTypeNames[] = {"Peak", "Low Shelf", "High Shelf", "Low Cut", "High Cut"};
static_assert(std::size(kPeqTypeNames) == static_cast<size_t>(PeqType::Count));
const char *const kReverbTypeNames[] = {"Hall", "Room", "Plate", "Shimmer"};
static_assert(std::size(kReverbTypeNames) == static_cast<size_t>(ReverbType::Count));
const char *const kFxNames[] = {"Empty",      "Shaper",      "Delay",  "DJ EQ",  "Param EQ", "Filter",
                                "Compressor", "3-Band Comp", "Reverb", "Phaser", "Flanger",  "Freq Shifter",
                                "Phase Disp", "Widener"};
static_assert(std::size(kFxNames) == static_cast<size_t>(FxType::Count));
const char *const kSourceTypeNames[] = {"Empty", "LFO", "Envelope"};
static_assert(std::size(kSourceTypeNames) == static_cast<size_t>(SourceType::Count));
// A drawn shaper curve's points as it starts, (x, y, curve) each: an S, flat at the ends, steep through the middle.
constexpr double kClipCurveDefaults[3][3] = {{0.0, -1.0, 0.35}, {0.5, 0.0, -0.35}, {1.0, 1.0, 0.0}};
constexpr int kTwoWhole = 4; // "2/1"
constexpr int kWhole = 7;    // "1/1"
constexpr int kQuarter = 13; // "1/4"
constexpr int kEighthDotted = 15;

// "1,2,3,…,16": the slots in the order they run.
std::string fxOrderName(double value) {
    std::string name;
    for (int slot : fxOrder(value))
        name += (name.empty() ? "" : ",") + std::to_string(slot + 1);
    return name;
}

// Reads the slot numbers back from an order's name; false unless each slot comes up once.
bool parseFxOrder(const char *text, double *out) {
    FxOrder order{};
    int count = 0;
    bool seen[kFxSlots] = {};
    for (const char *c = text; *c;) {
        if (*c < '0' || *c > '9') {
            ++c;
            continue;
        }
        char *end = nullptr;
        const long number = std::strtol(c, &end, 10);
        c = end;
        const int slot = static_cast<int>(number) - 1;
        if (slot < 0 || slot >= kFxSlots || seen[slot] || count == kFxSlots)
            return false;
        seen[slot] = true;
        order[count++] = slot;
    }
    if (count != kFxSlots)
        return false;
    *out = fxOrderValue(order);
    return true;
}

template <size_t N>
const char *nameAt(const char *const (&names)[N], double value) {
    return names[std::clamp(static_cast<int>(std::lround(value)), 0, static_cast<int>(N) - 1)];
}

std::string letter(int op) {
    return std::string(1, static_cast<char>('A' + op));
}

ParamTable buildTable() {
    ParamTable t{};
    auto add = [&t](int index, std::string name, std::string module, ParamKind kind, double min, double max,
                    double def) {
        // Continuous params accept host modulation, globally or per voice (Bitwig's
        // modulators, for example); stepped ones are plain automation. Slot targets
        // and what each effect slot or source place holds are wiring, set from the editor, so hosts
        // don't offer them for automation. The voice limit reallocates voices, so
        // it isn't automatable either. Freeform LFO points are drawn in the editor,
        // slots are bypassed there and the chain order is set by dragging; hosts don't show them.
        const bool shape = kind == ParamKind::ShapeCount || kind == ParamKind::ShapePoint ||
                           kind == ParamKind::SlotBypass;
        const bool wiring = kind == ParamKind::ModTarget || kind == ParamKind::FxType ||
                            kind == ParamKind::SourceType || kind == ParamKind::FxOrder;
        clap_param_info_flags flags = wiring || kind == ParamKind::VoiceCount || shape ? 0u : CLAP_PARAM_IS_AUTOMATABLE;
        // The chain order has 16! values; hosts can't do anything useful with it.
        if (shape || kind == ParamKind::FxOrder)
            flags |= CLAP_PARAM_IS_HIDDEN;
        if (isStepped(kind))
            flags |= CLAP_PARAM_IS_STEPPED;
        else if (!shape)
            flags |= CLAP_PARAM_IS_MODULATABLE | CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID |
                     CLAP_PARAM_IS_MODULATABLE_PER_KEY;
        if (kind == ParamKind::LfoShape || kind == ParamKind::OpWave || kind == ParamKind::FilterType ||
            kind == ParamKind::FilterSlope || kind == ParamKind::Toggle || kind == ParamKind::SyncDivision ||
            kind == ParamKind::ClipMode || kind == ParamKind::FxType || kind == ParamKind::SourceType ||
            kind == ParamKind::PeqType || kind == ParamKind::ReverbType)
            flags |= CLAP_PARAM_IS_ENUM;
        t[index] = {idOfIndex(index), std::move(name), std::move(module), kind, min, max, def, flags};
    };

    // Slopes of an envelope's attack, decay and release, in that order. By default
    // the attack is a straight line and the decay and release are exponential.
    auto addCurves = [&](int attackCurve, const std::string &name, const std::string &module) {
        const char *const stages[] = {" Attack Curve", " Decay Curve", " Release Curve"};
        for (int i = 0; i < 3; ++i)
            add(attackCurve + i, name + stages[i], module, ParamKind::BipolarPercent, -1.0, 1.0,
                i == 0 ? 0.0 : kExponentialCurve);
    };

    // The level after the effects, for the whole output: hosts modulate it globally, not per note.
    add(paramIndex(GlobalParam::Volume), "Volume", "Global", ParamKind::Decibel, -60.0, 12.0, 0.0);
    t[paramIndex(GlobalParam::Volume)].flags &=
        ~static_cast<clap_param_info_flags>(CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID | CLAP_PARAM_IS_MODULATABLE_PER_KEY);
    add(paramIndex(GlobalParam::FmAmount), "FM Amount", "Global", ParamKind::Percent, 0.0, 2.0, 1.0);
    add(paramIndex(GlobalParam::BendRange), "Bend Range", "Global", ParamKind::Semitones, 0.0, 24.0, 2.0);
    add(paramIndex(GlobalParam::Pitch), "Pitch", "Global", ParamKind::SemitoneOffset, -48.0, 48.0, 0.0);
    add(paramIndex(GlobalParam::Pan), "Pan", "Global", ParamKind::Pan, -1.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::Voices), "Voices", "Global", ParamKind::VoiceCount, 1.0, kMaxVoices, 16.0);
    add(paramIndex(GlobalParam::Mono), "Mono", "Global", ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::Legato), "Legato", "Global", ParamKind::Toggle, 0.0, 1.0, 1.0);
    add(paramIndex(GlobalParam::Glide), "Glide", "Global", ParamKind::TimeOrOff, 0.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::GlideAlways), "Glide Always", "Global", ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(GlobalParam::FxOrder), "FX Order", "FX", ParamKind::FxOrder, 0.0,
        static_cast<double>(kFxOrders - 1), 0.0);
    add(paramIndex(GlobalParam::ModWheel), "Mod Wheel", "Global", ParamKind::Percent, 0.0, 1.0, 0.0);

    // Default patch: operator A, a plain sine carrier; the others start switched
    // off, to be added from the routing panel. Every operator has the same settings otherwise.
    for (int op = 0; op < kOps; ++op) {
        const std::string name = "Op " + letter(op);
        const std::string module = "Operators/" + name;
        const bool first = op == 0;
        add(paramIndex(op, OpParam::Coarse), name + " Coarse", module, ParamKind::Ratio, 0.0, 32.0, 1.0);
        add(paramIndex(op, OpParam::Fine), name + " Fine", module, ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Detune), name + " Detune", module, ParamKind::Cents, -50.0, 50.0, 0.0);
        add(paramIndex(op, OpParam::Out), name + " Out", module, ParamKind::Percent, 0.0, 1.0, first ? 1.0 : 0.0);
        add(paramIndex(op, OpParam::Attack), name + " Attack", module, ParamKind::Time, 0.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Decay), name + " Decay", module, ParamKind::Time, 0.0, 1.0, normFromTime(1.5));
        add(paramIndex(op, OpParam::Sustain), name + " Sustain", module, ParamKind::Percent, 0.0, 1.0, 1.0);
        add(paramIndex(op, OpParam::Release), name + " Release", module, ParamKind::Time, 0.0, 1.0,
            normFromTime(0.015));
        add(paramIndex(op, OpParam::Hold), name + " Hold", module, ParamKind::TimeOrOff, 0.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Transpose), name + " Pitch", module, ParamKind::Transpose, -48.0, 48.0, 0.0);
        add(paramIndex(op, OpParam::Velocity), name + " Velocity", module, ParamKind::Percent, 0.0, 1.0, 0.5);
        add(paramIndex(op, OpParam::Enabled), name + " On", module, ParamKind::Toggle, 0.0, 1.0, first ? 1.0 : 0.0);
        add(paramIndex(op, OpParam::Wave), name + " Wave", module, ParamKind::OpWave, 0.0,
            static_cast<double>(OpWave::Count) - 1.0, 0.0);
        for (int filter = 0; filter < kFilters; ++filter)
            add(sendIndex(op, filter), letter(op) + " → F" + std::to_string(filter + 1), "Matrix", ParamKind::Percent,
                0.0, 1.0, 0.0);
        for (int shaper = 0; shaper < kShapers; ++shaper)
            add(shaperSendIndex(op, shaper), letter(op) + " → S" + std::to_string(shaper + 1), "Matrix",
                ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(op, OpParam::Unison), name + " Unison", module, ParamKind::Count, 1.0, kMaxUnison, 1.0);
        add(paramIndex(op, OpParam::UnisonDetune), name + " Unison Detune", module, ParamKind::CentsAmount, 0.0, 100.0,
            20.0);
        add(paramIndex(op, OpParam::UnisonSpread), name + " Unison Spread", module, ParamKind::Percent, 0.0, 1.0, 0.5);
        add(paramIndex(op, OpParam::PhaseRandom), name + " Phase Random", module, ParamKind::Percent, 0.0, 1.0,
            0.0);
        addCurves(paramIndex(op, OpParam::AttackCurve), name, module);
        add(paramIndex(op, OpParam::NoiseSlope), name + " Noise Slope", module, ParamKind::NoiseSlope, 0.0, 1.0,
            0.0);
        // The Custom wave starts as a sine: the first partial alone.
        for (int partial = 0; partial < kPartials; ++partial) {
            const std::string n = name + " Partial " + std::to_string(partial + 1);
            add(partialLevelIndex(op, partial), n + " Level", module, ParamKind::ShapePoint, 0.0, 1.0,
                partial == 0 ? 1.0 : 0.0);
            add(partialPhaseIndex(op, partial), n + " Phase", module, ParamKind::ShapePoint, 0.0, 1.0, 0.0);
        }
    }

    for (int from = 0; from < kOps; ++from) {
        for (int to = 0; to < kOps; ++to) {
            const std::string name = from == to ? letter(from) + " Feedback" : letter(from) + " → " + letter(to);
            add(matrixIndex(from, to), name, "Matrix", ParamKind::Percent, 0.0, 1.0, 0.0);
        }
    }
    for (int filter = 0; filter < kFilters; ++filter)
        for (int to = 0; to < kOps; ++to)
            add(filterModIndex(filter, to), "F" + std::to_string(filter + 1) + " → " + letter(to), "Matrix",
                ParamKind::Percent, 0.0, 1.0, 0.0);

    // Filter 1 defaults to a warm low-pass and filter 2 to a band-pass for something different. Neither follows
    // the note until its Key Track is turned up.
    struct FilterDefaults {
        FilterType type;
        double slope;
        double cutoffHz;
        double resonance;
    };
    const FilterDefaults filterDefaults[] = {{FilterType::LowPass, kDefaultSlope, 1200.0, 0.3},
                                             {FilterType::BandPass, kDefaultSlope, 800.0, 0.5}};
    static_assert(std::size(filterDefaults) == static_cast<size_t>(kFilters));
    for (int filter = 0; filter < kFilters; ++filter) {
        const std::string name = "Filter " + std::to_string(filter + 1);
        const std::string module = "Filters/" + name;
        const FilterDefaults &d = filterDefaults[filter];
        add(paramIndex(filter, FilterParam::Type), name + " Type", module, ParamKind::FilterType, 0.0,
            static_cast<double>(FilterType::Count) - 1.0, static_cast<double>(d.type));
        add(paramIndex(filter, FilterParam::Cutoff), name + " Cutoff", module, ParamKind::Cutoff, 0.0, 1.0,
            normFromCutoff(d.cutoffHz));
        add(paramIndex(filter, FilterParam::Resonance), name + " Resonance", module, ParamKind::Percent, 0.0, 1.0,
            d.resonance);
        add(paramIndex(filter, FilterParam::KeyTrack), name + " Key Track", module, ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(filter, FilterParam::Drive), name + " Drive", module, ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(filter, FilterParam::Out), name + " Out", module, ParamKind::Percent, 0.0, 1.0, 1.0);
        add(paramIndex(filter, FilterParam::Slope), name + " Slope", module, ParamKind::FilterSlope, 0.0,
            kFilterSlopes - 1.0, d.slope);
        add(paramIndex(filter, FilterParam::Enabled), name + " On", module, ParamKind::Toggle, 0.0, 1.0, 0.0);
    }
    for (int from = 0; from < kFilters; ++from)
        for (int to = from + 1; to < kFilters; ++to)
            add(filterLinkIndex(from, to), "F" + std::to_string(from + 1) + " → F" + std::to_string(to + 1), "Filters",
                ParamKind::Percent, 0.0, 1.0, 0.0);

    // Shapers start switched off, with the Soft curve, linked to their drive, routed nowhere; a drawn curve starts as
    // an S, like the shaper effect's.
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        const std::string tag = "S" + std::to_string(shaper + 1);
        const std::string name = "Shaper " + std::to_string(shaper + 1);
        const std::string module = "Shapers/" + name;
        add(paramIndex(shaper, ShaperParam::Enabled), name + " On", module, ParamKind::Toggle, 0.0, 1.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Mode), name + " Curve", module, ParamKind::ClipMode, 0.0,
            static_cast<double>(ClipMode::Count) - 1.0, static_cast<double>(ClipMode::Soft));
        add(paramIndex(shaper, ShaperParam::Drive), name + " Drive", module, ParamKind::Decibel, 0.0, 36.0, 6.0);
        add(paramIndex(shaper, ShaperParam::Output), name + " Output", module, ParamKind::Decibel, -24.0, 6.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Mix), name + " Mix", module, ParamKind::Percent, 0.0, 1.0, 1.0);
        add(paramIndex(shaper, ShaperParam::Link), name + " Link", module, ParamKind::Toggle, 0.0, 1.0, 1.0);
        for (int filter = 0; filter < kFilters; ++filter)
            add(shaperFilterIndex(shaper, filter), tag + " → F" + std::to_string(filter + 1), "Matrix",
                ParamKind::Percent, 0.0, 1.0, 0.0);
        for (int to = 0; to < kOps; ++to)
            add(shaperModIndex(shaper, to), tag + " → " + letter(to), "Matrix", ParamKind::Percent, 0.0, 1.0, 0.0);
        add(paramIndex(shaper, ShaperParam::Out), name + " To Out", module, ParamKind::Percent, 0.0, 1.0, 0.0);
        const int base = shaperCurveCountIndex(shaper);
        add(base, name + " Curve Points", module, ParamKind::ShapeCount, 2.0, kMaxShapePoints, 3.0);
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = name + " Curve " + std::to_string(point + 1);
            const bool used = point < 3;
            add(shapeFieldIndex(base, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? kClipCurveDefaults[point][0] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? kClipCurveDefaults[point][1] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " Curve", module, ParamKind::ShapePoint, -1.0,
                1.0, used ? kClipCurveDefaults[point][2] : 0.0);
        }
    }

    // The macros start at nothing, mapped to nothing.
    for (int macro = 0; macro < kMacros; ++macro) {
        const std::string name = "Macro " + std::to_string(macro + 1);
        const std::string module = "Macros/" + name;
        add(paramIndex(macro, MacroParam::Value), name, module, ParamKind::Percent, 0.0, 1.0, 0.0);
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotTargetIndex(kMaxSources + macro, slot), name + " Target " + n, module, ParamKind::ModTarget, 0.0,
                kMaxParamId + 1.0, 0.0);
            add(slotAmountIndex(kMaxSources + macro, slot), name + " Amount " + n, module, ParamKind::BipolarPercent,
                -1.0, 1.0, 0.0);
        }
        add(paramIndex(macro, MacroParam::Bipolar), name + " Bipolar", module, ParamKind::Toggle, 0.0, 1.0, 0.0);
    }

    // The note source starts mapped to nothing, over every MIDI note, in a straight line from nothing at C-1 to all
    // of it at G9.
    for (int slot = 0; slot < kSlots; ++slot) {
        const std::string n = std::to_string(slot + 1);
        add(slotTargetIndex(kNoteSource, slot), "Note Target " + n, "Note", ParamKind::ModTarget, 0.0,
            kMaxParamId + 1.0, 0.0);
        add(slotAmountIndex(kNoteSource, slot), "Note Amount " + n, "Note", ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
    }
    // The mod wheel starts mapped to nothing, like the macros.
    for (int slot = 0; slot < kSlots; ++slot) {
        const std::string n = std::to_string(slot + 1);
        add(slotTargetIndex(kWheelSource, slot), "Mod Wheel Target " + n, "Mod Wheel", ParamKind::ModTarget, 0.0,
            kMaxParamId + 1.0, 0.0);
        add(slotAmountIndex(kWheelSource, slot), "Mod Wheel Amount " + n, "Mod Wheel", ParamKind::BipolarPercent, -1.0,
            1.0, 0.0);
    }
    // Both start unipolar, like the macros: they take their targets one way.
    add(paramIndex(WheelParam::Bipolar), "Mod Wheel Bipolar", "Mod Wheel", ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(NoteParam::Bipolar), "Note Bipolar", "Note", ParamKind::Toggle, 0.0, 1.0, 0.0);
    add(paramIndex(NoteParam::Low), "Note Low", "Note", ParamKind::NotePitch, 0.0, kMaxNotePitch, 0.0);
    add(paramIndex(NoteParam::High), "Note High", "Note", ParamKind::NotePitch, 0.0, kMaxNotePitch, kMaxNotePitch);
    {
        const int base = paramIndex(NoteParam::Curve);
        add(base, "Note Curve Points", "Note", ParamKind::ShapeCount, 2.0, kMaxShapePoints, 2.0);
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = "Note Curve " + std::to_string(point + 1);
            add(shapeFieldIndex(base, point, PointField::X), n + " X", "Note", ParamKind::ShapePoint, 0.0, 1.0,
                point == 0 ? 0.0 : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", "Note", ParamKind::ShapePoint, -1.0, 1.0,
                point == 0 ? -1.0 : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " Curve", "Note", ParamKind::ShapePoint, -1.0,
                1.0, 0.0);
        }
    }

    // The effects chain starts empty.
    for (int slot = 0; slot < kFxSlots; ++slot) {
        const std::string name = "FX " + std::to_string(slot + 1);
        const std::string module = "FX/" + name;
        auto fx = [&](FxParam field, const std::string &label, ParamKind kind, double min, double max, double def) {
            add(paramIndex(slot, field), name + " " + label, module, kind, min, max, def);
        };
        fx(FxParam::Type, "Type", ParamKind::FxType, 0.0, static_cast<double>(FxType::Count) - 1.0, 0.0);
        fx(FxParam::Enabled, "On", ParamKind::Toggle, 0.0, 1.0, 1.0);

        fx(FxParam::ShaperMode, "Shaper Curve", ParamKind::ClipMode, 0.0, static_cast<double>(ClipMode::Count) - 1.0,
           static_cast<double>(ClipMode::Soft));
        fx(FxParam::ShaperDrive, "Shaper Drive", ParamKind::Decibel, 0.0, 36.0, 6.0);
        fx(FxParam::ShaperOutput, "Shaper Output", ParamKind::Decibel, -24.0, 6.0, 0.0);
        fx(FxParam::ShaperMix, "Shaper Mix", ParamKind::Percent, 0.0, 1.0, 1.0);
        // The drawn curve starts as an S: flat at the ends, steep through the middle.
        const std::string curve = name + " Shaper Curve";
        add(clipCurveCountIndex(slot), curve + " Points", module, ParamKind::ShapeCount, 2.0, kMaxShapePoints, 3.0);
        const auto &curveDefaults = kClipCurveDefaults;
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = curve + " " + std::to_string(point + 1);
            const bool used = point < 3;
            const int base = clipCurveCountIndex(slot);
            add(shapeFieldIndex(base, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? curveDefaults[point][0] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? curveDefaults[point][1] : 1.0);
            add(shapeFieldIndex(base, point, PointField::Curve), n + " Curve", module, ParamKind::ShapePoint, -1.0,
                1.0, used ? curveDefaults[point][2] : 0.0);
        }

        // A dotted eighth on the left and a quarter on the right.
        fx(FxParam::DelayTimeL, "Delay Time L", ParamKind::Time, 0.0, normFromTime(kMaxFreeDelay),
           normFromTime(0.375));
        fx(FxParam::DelayTimeR, "Delay Time R", ParamKind::Time, 0.0, normFromTime(kMaxFreeDelay), normFromTime(0.5));
        fx(FxParam::DelaySync, "Delay Sync", ParamKind::Toggle, 0.0, 1.0, 1.0);
        fx(FxParam::DelayLink, "Delay Link", ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::DelayDivisionL, "Delay Division L", ParamKind::SyncDivision, 0.0, kDivisions - 1.0,
           kEighthDotted);
        fx(FxParam::DelayDivisionR, "Delay Division R", ParamKind::SyncDivision, 0.0, kDivisions - 1.0, kQuarter);
        fx(FxParam::DelayFeedback, "Delay Feedback", ParamKind::Percent, 0.0, 0.95, 0.4);
        fx(FxParam::DelayTone, "Delay Tone", ParamKind::DelayTone, -1.0, 1.0, 0.0);
        fx(FxParam::DelayWidth, "Delay Width", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::DelayPingPong, "Delay Ping-Pong", ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::DelayMix, "Delay Mix", ParamKind::Percent, 0.0, 1.0, 0.3);

        fx(FxParam::EqLow, "DJ EQ Low", ParamKind::EqGain, -1.0, 1.0, 0.0);
        fx(FxParam::EqMid, "DJ EQ Mid", ParamKind::EqGain, -1.0, 1.0, 0.0);
        fx(FxParam::EqHigh, "DJ EQ High", ParamKind::EqGain, -1.0, 1.0, 0.0);

        // Flat to begin with: a band in the lows and one in the highs.
        const double bandHz[kPeqBands] = {250.0, 4000.0};
        for (int band = 0; band < kPeqBands; ++band) {
            const std::string b = "EQ Band " + std::to_string(band + 1);
            add(peqFreqIndex(slot, band), name + " " + b + " Freq", module, ParamKind::Cutoff, 0.0, 1.0,
                normFromCutoff(bandHz[band]));
            add(peqGainIndex(slot, band), name + " " + b + " Gain", module, ParamKind::Decibel, -kMaxPeqGain,
                kMaxPeqGain, 0.0);
            add(peqQIndex(slot, band), name + " " + b + " Q", module, ParamKind::Q, 0.0, 1.0, normFromQ(1.0));
            add(peqTypeIndex(slot, band), name + " " + b + " Type", module, ParamKind::PeqType, 0.0,
                static_cast<double>(PeqType::Count) - 1.0, static_cast<double>(PeqType::Peak));
        }

        fx(FxParam::FilterType, "Filter Type", ParamKind::FilterType, 0.0, static_cast<double>(FilterType::Count) - 1.0,
           static_cast<double>(FilterType::LowPass));
        fx(FxParam::FilterSlope, "Filter Slope", ParamKind::FilterSlope, 0.0, kFilterSlopes - 1.0, kDefaultSlope);
        fx(FxParam::FilterCutoff, "Filter Cutoff", ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(2000.0));
        fx(FxParam::FilterResonance, "Filter Resonance", ParamKind::Percent, 0.0, 1.0, 0.2);
        fx(FxParam::FilterDrive, "Filter Drive", ParamKind::Percent, 0.0, 1.0, 0.0);
        fx(FxParam::FilterMix, "Filter Mix", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::FilterKeyTrack, "Filter Key Track", ParamKind::Percent, 0.0, 1.0, 0.0);

        fx(FxParam::CompThreshold, "Comp Threshold", ParamKind::Decibel, -60.0, 0.0, -18.0);
        fx(FxParam::CompRatio, "Comp Ratio", ParamKind::CompRatio, 1.0, 20.0, 4.0);
        fx(FxParam::CompKnee, "Comp Knee", ParamKind::Decibel, 0.0, 24.0, 6.0);
        fx(FxParam::CompAttack, "Comp Attack", ParamKind::Time, 0.0, normFromTime(0.5), normFromTime(0.01));
        fx(FxParam::CompRelease, "Comp Release", ParamKind::Time, 0.0, normFromTime(2.0), normFromTime(0.15));
        fx(FxParam::CompMakeup, "Comp Makeup", ParamKind::Decibel, 0.0, 24.0, 0.0);
        fx(FxParam::CompMix, "Comp Mix", ParamKind::Percent, 0.0, 1.0, 1.0);

        fx(FxParam::McLowFreq, "3-Band Low Crossover", ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(200.0));
        fx(FxParam::McHighFreq, "3-Band High Crossover", ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(2500.0));
        const char *const bandNames[kMcBands] = {"Low", "Mid", "High"};
        for (int band = 0; band < kMcBands; ++band) {
            const std::string b = std::string("3-Band ") + bandNames[band];
            add(mcThresholdIndex(slot, band), name + " " + b + " Threshold", module, ParamKind::Decibel, -60.0, 0.0,
                -20.0);
            add(mcRatioIndex(slot, band), name + " " + b + " Ratio", module, ParamKind::CompRatio, 1.0, 20.0, 3.0);
            add(mcGainIndex(slot, band), name + " " + b + " Gain", module, ParamKind::Decibel, -12.0, 12.0, 0.0);
        }
        fx(FxParam::McAttack, "3-Band Attack", ParamKind::Time, 0.0, normFromTime(0.5), normFromTime(0.01));
        fx(FxParam::McRelease, "3-Band Release", ParamKind::Time, 0.0, normFromTime(2.0), normFromTime(0.15));
        fx(FxParam::McMix, "3-Band Mix", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::McOutput, "3-Band Output", ParamKind::Decibel, -24.0, 24.0, 0.0);
        fx(FxParam::McLink, "3-Band Link", ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::ShaperLink, "Shaper Link", ParamKind::Toggle, 0.0, 1.0, 1.0); // turning the drive keeps the level
        fx(FxParam::ReverbType, "Reverb Type", ParamKind::ReverbType, 0.0, static_cast<double>(ReverbType::Count) - 1.0,
           static_cast<double>(ReverbType::Hall));

        // A medium room, a little dark, held back 20 ms.
        fx(FxParam::ReverbSize, "Reverb Size", ParamKind::Percent, 0.0, 1.0, 0.6);
        fx(FxParam::ReverbDecay, "Reverb Decay", ParamKind::Time, normFromTime(0.2), 1.0, normFromTime(2.5));
        fx(FxParam::ReverbDamping, "Reverb Damping", ParamKind::Percent, 0.0, 1.0, 0.4);
        fx(FxParam::ReverbPreDelay, "Reverb Pre-Delay", ParamKind::TimeOrOff, 0.0, normFromTime(0.25),
           normFromTime(0.02));
        fx(FxParam::ReverbWidth, "Reverb Width", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::ReverbMix, "Reverb Mix", ParamKind::Percent, 0.0, 1.0, 0.3);
        // A shimmer climbs an octave each time round, half of what goes round shifted.
        fx(FxParam::ReverbShimmerPitch, "Reverb Shimmer Pitch", ParamKind::Transpose, -24.0, 24.0, 12.0);
        fx(FxParam::ReverbShimmer, "Reverb Shimmer", ParamKind::Percent, 0.0, 1.0, 0.5);

        fx(FxParam::PhaserRate, "Phaser Rate", ParamKind::SweepRate, 0.0, normFromRate(20.0), normFromRate(0.4));
        fx(FxParam::PhaserDepth, "Phaser Depth", ParamKind::Percent, 0.0, 1.0, 0.6);
        fx(FxParam::PhaserCenter, "Phaser Center", ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(700.0));
        fx(FxParam::PhaserFeedback, "Phaser Feedback", ParamKind::BipolarPercent, -1.0, 1.0, 0.5);
        fx(FxParam::PhaserStages, "Phaser Stages", ParamKind::Count, 2.0, kMaxPhaserStages, 4.0);
        fx(FxParam::PhaserMix, "Phaser Mix", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::PhaserStereo, "Phaser Stereo", ParamKind::Toggle, 0.0, 1.0, 0.0);
        // Synced, a bar a sweep.
        fx(FxParam::PhaserSync, "Phaser Sync", ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::PhaserDivision, "Phaser Division", ParamKind::SyncDivision, 0.0, kDivisions - 1.0, kWhole);

        fx(FxParam::DisperserFreq, "Phase Disp Freq", ParamKind::Cutoff, 0.0, 1.0, normFromCutoff(600.0));
        fx(FxParam::DisperserStages, "Phase Disp Stages", ParamKind::Count, 1.0, kMaxDisperserStages, 8.0);
        fx(FxParam::DisperserPinch, "Phase Disp Pinch", ParamKind::Percent, 0.0, 1.0, 0.5);
        fx(FxParam::DisperserMix, "Phase Disp Mix", ParamKind::Percent, 0.0, 1.0, 1.0);

        // Wide from the start, the lows left in the middle: the sides above 150 Hz, half in.
        fx(FxParam::WidenerSize, "Widener Size", ParamKind::Percent, 0.0, 1.0, 0.5);
        fx(FxParam::WidenerWidth, "Widener Width", ParamKind::Percent, 0.0, 1.0, 1.0);
        fx(FxParam::WidenerHighPass, "Widener High Pass", ParamKind::Cutoff, 0.0, normFromCutoff(kMaxWidenerHighPass),
           normFromCutoff(150.0));
        fx(FxParam::WidenerMix, "Widener Mix", ParamKind::Percent, 0.0, 1.0, 0.5);

        fx(FxParam::FlangerRate, "Flanger Rate", ParamKind::SweepRate, 0.0, normFromRate(20.0), normFromRate(0.25));
        fx(FxParam::FlangerDepth, "Flanger Depth", ParamKind::Percent, 0.0, 1.0, 0.7);
        fx(FxParam::FlangerDelay, "Flanger Delay", ParamKind::Time, 0.0, normFromTime(0.015), normFromTime(0.003));
        fx(FxParam::FlangerFeedback, "Flanger Feedback", ParamKind::BipolarPercent, -1.0, 1.0, 0.5);
        fx(FxParam::FlangerMix, "Flanger Mix", ParamKind::Percent, 0.0, 1.0, 0.5);
        // Synced, two bars a sweep.
        fx(FxParam::FlangerSync, "Flanger Sync", ParamKind::Toggle, 0.0, 1.0, 0.0);
        fx(FxParam::FlangerDivision, "Flanger Division", ParamKind::SyncDivision, 0.0, kDivisions - 1.0, kTwoWhole);

        // No shift until turned, then up to 100 Hz either way.
        fx(FxParam::ShiftRange, "Shift Range", ParamKind::ShiftRange, 0.0, 1.0, normFromShiftRange(100.0));
        fx(FxParam::ShiftAmount, "Freq Shift", ParamKind::BipolarPercent, -1.0, 1.0, 0.0);
        fx(FxParam::ShiftSpread, "Shift Spread", ParamKind::Percent, 0.0, 1.0, 0.0);
        fx(FxParam::ShiftFeedback, "Shift Feedback", ParamKind::Percent, 0.0, 0.9, 0.0);
        fx(FxParam::ShiftMix, "Shift Mix", ParamKind::Percent, 0.0, 1.0, 1.0);
    }

    // Sources start empty; the editor gives a place its type. Host names stay
    // with the place, whichever type it holds.
    for (int source = 0; source < kMaxSources; ++source) {
        const std::string name = "Mod " + std::to_string(source + 1);
        const std::string module = "Modulation/" + name;
        auto field = [&](SourceParam f, const std::string &label, ParamKind kind, double min, double max, double def) {
            add(paramIndex(source, f), name + " " + label, module, kind, min, max, def);
        };
        field(SourceParam::Type, "Type", ParamKind::SourceType, 0.0, static_cast<double>(SourceType::Count) - 1.0, 0.0);
        field(SourceParam::Enabled, "On", ParamKind::Toggle, 0.0, 1.0, 1.0);
        for (int slot = 0; slot < kSlots; ++slot) {
            const std::string n = std::to_string(slot + 1);
            add(slotTargetIndex(source, slot), name + " Target " + n, module, ParamKind::ModTarget, 0.0,
                kMaxParamId + 1.0, 0.0);
            add(slotAmountIndex(source, slot), name + " Amount " + n, module, ParamKind::BipolarPercent, -1.0, 1.0,
                0.0);
        }

        field(SourceParam::Shape, "Shape", ParamKind::LfoShape, 0.0, static_cast<double>(LfoShape::Count) - 1.0, 0.0);
        field(SourceParam::Rate, "Rate", ParamKind::Rate, 0.0, 1.0, normFromRate(2.0));
        field(SourceParam::Sync, "Sync", ParamKind::Toggle, 0.0, 1.0, 1.0); // an LFO starts on quarter notes
        field(SourceParam::Division, "Division", ParamKind::SyncDivision, 0.0, kDivisions - 1.0, kQuarter);
        field(SourceParam::Free, "Free", ParamKind::Toggle, 0.0, 1.0, 0.0);
        field(SourceParam::Unipolar, "Unipolar", ParamKind::Toggle, 0.0, 1.0, 0.0);
        field(SourceParam::OneShot, "One-Shot", ParamKind::Toggle, 0.0, 1.0, 0.0);
        // Random starts as plain sample and hold: steps, each landing anywhere.
        field(SourceParam::RandomSmooth, "Smooth", ParamKind::Percent, 0.0, 1.0, 0.0);
        field(SourceParam::RandomChaos, "Chaos", ParamKind::Percent, 0.0, 1.0, 1.0);

        field(SourceParam::Attack, "Attack", ParamKind::Time, 0.0, 1.0, 0.0);
        field(SourceParam::Decay, "Decay", ParamKind::Time, 0.0, 1.0, normFromTime(0.5));
        field(SourceParam::Sustain, "Sustain", ParamKind::Percent, 0.0, 1.0, 1.0);
        // The release an operator's envelope starts with.
        field(SourceParam::Release, "Release", ParamKind::Time, 0.0, 1.0, normFromTime(0.015));
        field(SourceParam::Hold, "Hold", ParamKind::TimeOrOff, 0.0, 1.0, 0.0);
        addCurves(paramIndex(source, SourceParam::AttackCurve), name, module);
        field(SourceParam::EnvBipolar, "Bipolar", ParamKind::Toggle, 0.0, 1.0, 0.0); // takes its targets one way

        // Freeform shapes start as a triangle: up from the bottom to the top at the middle of the
        // cycle, and down again as the last segment wraps around.
        const std::string shape = name + " Shape";
        add(shapeCountIndex(source), shape + " Points", module, ParamKind::ShapeCount, 2.0, kMaxShapePoints, 2.0);
        const double defaults[2][2] = {{0.0, -1.0}, {0.5, 1.0}};
        for (int point = 0; point < kMaxShapePoints; ++point) {
            const std::string n = shape + " " + std::to_string(point + 1);
            const bool used = point < 2;
            add(shapePointIndex(source, point, PointField::X), n + " X", module, ParamKind::ShapePoint, 0.0, 1.0,
                used ? defaults[point][0] : 1.0);
            add(shapePointIndex(source, point, PointField::Y), n + " Y", module, ParamKind::ShapePoint, -1.0, 1.0,
                used ? defaults[point][1] : 0.0);
            add(shapePointIndex(source, point, PointField::Curve), n + " Curve", module, ParamKind::ShapePoint, -1.0,
                1.0, 0.0);
        }
        for (int slot = 0; slot < kSlots; ++slot)
            add(slotBypassIndex(source, slot), name + " Bypass " + std::to_string(slot + 1), module,
                ParamKind::SlotBypass, 0.0, 1.0, 0.0);
    }
    return t;
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

const char *clipModeName(ClipMode mode) {
    return nameAt(kClipModeNames, static_cast<double>(mode));
}

const char *peqTypeName(PeqType type) {
    return nameAt(kPeqTypeNames, static_cast<double>(type));
}

const char *reverbTypeName(ReverbType type) {
    return nameAt(kReverbTypeNames, static_cast<double>(type));
}

const char *fxName(FxType fx) {
    return nameAt(kFxNames, static_cast<double>(fx));
}

const ParamTable &paramTable() {
    static const ParamTable table = buildTable();
    return table;
}

bool isStepped(ParamKind kind) {
    return kind == ParamKind::Ratio || kind == ParamKind::Semitones || kind == ParamKind::Count ||
           kind == ParamKind::VoiceCount || kind == ParamKind::Toggle || kind == ParamKind::LfoShape ||
           kind == ParamKind::OpWave || kind == ParamKind::FilterType || kind == ParamKind::ModTarget ||
           kind == ParamKind::SyncDivision || kind == ParamKind::ClipMode || kind == ParamKind::FxOrder ||
           kind == ParamKind::FxType || kind == ParamKind::SourceType || kind == ParamKind::ShapeCount ||
           kind == ParamKind::FilterSlope || kind == ParamKind::SlotBypass || kind == ParamKind::PeqType ||
           kind == ParamKind::ReverbType;
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
            std::snprintf(out, capacity, "Off");
            break;
        }
        [[fallthrough]];
    case ParamKind::Time: {
        const double seconds = timeFromNorm(value);
        const double ms = std::round(seconds * 10000.0) / 10.0;
        if (ms < 1000.0)
            std::snprintf(out, capacity, "%.1f ms", ms);
        else
            std::snprintf(out, capacity, "%.2f s", seconds);
        break;
    }
    case ParamKind::Toggle:
    case ParamKind::SlotBypass:
        std::snprintf(out, capacity, "%s", value >= 0.5 ? "On" : "Off");
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
            std::snprintf(out, capacity, "Open");
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
    case ParamKind::FxType:
        std::snprintf(out, capacity, "%s", nameAt(kFxNames, value));
        break;
    case ParamKind::SourceType:
        std::snprintf(out, capacity, "%s", nameAt(kSourceTypeNames, value));
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
    case ParamKind::ModTarget: {
        const int target = slotTarget(value);
        std::snprintf(out, capacity, "%s", target < 0 ? "None" : paramTable()[target].name.c_str());
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
    case ParamKind::FxOrder:
        std::snprintf(out, capacity, "%s", fxOrderName(value).c_str());
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
        const char *const name = db == 0.0 ? "White" : db == 3.0 ? "Pink" : db == 6.0 ? "Brown" : nullptr;
        if (name)
            std::snprintf(out, capacity, "%s", name);
        else
            std::snprintf(out, capacity, "-%.1f dB/oct", db);
        break;
    }
    case ParamKind::EqGain: {
        const double gain = eqGain(value);
        if (gain < 1e-4)
            std::snprintf(out, capacity, "Kill");
        else
            std::snprintf(out, capacity, "%+.1f dB", 20.0 * std::log10(gain));
        break;
    }
    case ParamKind::NotePitch: { // "C4", or "C4 +12" between notes
        const long note = std::lround(value);
        const long cents = std::lround((value - note) * 100.0);
        const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const long octave = (note < 0 ? note - 11 : note) / 12 - 1;
        const char *name = names[((note % 12) + 12) % 12];
        if (cents == 0)
            std::snprintf(out, capacity, "%s%ld", name, octave);
        else
            std::snprintf(out, capacity, "%s%ld %+ld", name, octave, cents);
        break;
    }
    case ParamKind::ShiftRange: {
        const double hz = shiftRangeFromNorm(value);
        if (hz < 10.0)
            std::snprintf(out, capacity, "%.2f Hz", hz);
        else if (hz < 100.0)
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
                                                            : -1;
    if (named >= 0) {
        *out = named;
        return true;
    }
    if (def.kind == ParamKind::Toggle || def.kind == ParamKind::SlotBypass || def.kind == ParamKind::TimeOrOff) {
        if (equalsIgnoreCase(text, "Off")) {
            *out = 0.0;
            return true;
        }
        if (def.kind != ParamKind::TimeOrOff && equalsIgnoreCase(text, "On")) {
            *out = 1.0;
            return true;
        }
    }
    if (def.kind == ParamKind::EqGain && equalsIgnoreCase(text, "Kill")) {
        *out = -1.0;
        return true;
    }
    if (def.kind == ParamKind::SyncDivision)
        return false; // names only: "1/4" would otherwise read as the number 1
    if (def.kind == ParamKind::FxOrder)
        return parseFxOrder(text, out);
    if (def.kind == ParamKind::DelayTone) {
        // "Open", "LP 2 kHz", "HP 300 Hz", or a bare percentage.
        const char kind = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        const char pass = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0] ? text[1] : 0)));
        if (equalsIgnoreCase(text, "Open")) {
            *out = 0.0;
            return true;
        }
        if ((kind == 'L' || kind == 'H') && pass == 'P') {
            const double hz = parseHz(text + 2);
            if (hz <= 0.0)
                return false;
            const double norm = normFromCutoff(std::max(hz, kMinCutoff));
            *out = kind == 'L' ? std::clamp((norm - 1.0) * 1.5, -1.0, 0.0) : std::clamp(norm / 0.77, 0.0, 1.0);
            return true;
        }
    }
    if (def.kind == ParamKind::NoiseSlope) {
        const char *const names[] = {"White", "Pink", "Brown"};
        for (int i = 0; i < 3; ++i) {
            if (equalsIgnoreCase(text, names[i])) {
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
            const long octave = std::strtol(c, &end, 10);
            if (end == c)
                return false;
            double cents = 0.0;
            while (*end == ' ')
                ++end;
            if (*end == '+' || *end == '-')
                cents = std::strtod(end, nullptr);
            *out = std::clamp((octave + 1) * 12.0 + note + cents / 100.0, def.min, def.max);
            return true;
        }
    }
    if (def.kind == ParamKind::ModTarget) {
        if (equalsIgnoreCase(text, "None")) {
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
            *out = std::clamp((side == 'L' ? -percent : percent) / 100.0, -1.0, 1.0);
            return true;
        }
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
    case ParamKind::Q:
        value = normFromQ(std::max(value, kMinQ));
        break;
    case ParamKind::Time:
    case ParamKind::TimeOrOff: {
        // "s" means seconds, anything else (including a bare number) milliseconds.
        const double seconds = *end == 's' || *end == 'S' ? value : value / 1000.0;
        value = normFromTime(std::max(seconds, kMinTime));
        break;
    }
    case ParamKind::Toggle:
    case ParamKind::SlotBypass:
        value = value >= 0.5 ? 1.0 : 0.0;
        break;
    case ParamKind::Rate:
        value = normFromRate(std::max(value, kMinRate));
        break;
    case ParamKind::SweepRate: // below the slowest LFO rate, still
        value = value < kMinRate ? 0.0 : normFromRate(value);
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
    case ParamKind::FxOrder:
    case ParamKind::FxType:
    case ParamKind::SourceType:
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
    case ParamKind::ShiftRange: // Hz, or kHz with a k
        value = normFromShiftRange(*end == 'k' || *end == 'K' ? value * 1000.0 : value);
        break;
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
