#pragma once

// The lists the editor's list buttons open (Kind::ListButton, see View's lists): which params open one, how many
// entries each offers, the value each entry picks and its name, and which have arrows that step along them. Shared by
// the View and the panels whose cards and pages hold lists (see inspectorpanel.h), in namespace detail.

#include "layout.h"
#include "widget.h"
#include "../params.h"
#include "../uistrings.h"

#include <string>

namespace fmvibe::gui::detail {

// The type param that goes with a filter slope param, or -1 for other params.
inline int slopeTypeParam(int param) {
    if (fxFieldOf(param) == FxParam::FilterSlope)
        return paramIndex(fxSlotOf(param), FxParam::FilterType);
    for (int filter = 0; filter < kFilters; ++filter)
        if (paramIndex(filter, FilterParam::Slope) == param)
            return paramIndex(filter, FilterParam::Type);
    return -1;
}

inline bool isFilterSlope(int param) {
    return slopeTypeParam(param) >= 0;
}

// A split's slope: 6, 12 or 24 dB (18, which a filter has, it doesn't).
inline bool isSplitSlope(int param) {
    const int split = splitOf(param);
    return split >= 0 && param == paramIndex(split, SplitParam::Slope);
}

// Any slope's list: a filter's, the filter effect's or a split's.
inline bool isSlopeList(int param) {
    return isFilterSlope(param) || isSplitSlope(param);
}

inline bool isLfoShape(int param) {
    return sourceFieldOf(param) == SourceParam::Shape;
}

inline bool isClipMode(int param) {
    return fxFieldOf(param) == FxParam::ShaperMode || shaperFieldOf(param) == ShaperParam::Mode;
}


inline bool isMidiInput(int param) {
    return sourceFieldOf(param) == SourceParam::MidiInput;
}

// The voice filter a param is one of, or -1.
inline int filterOfParam(int param) {
    return param >= kFilterBase && param < kFilterBase + kFilters * kFilterParamCount
               ? (param - kFilterBase) / kFilterParamCount
               : -1;
}

// A voice filter's type, or the filter effect's.
inline bool isFilterTypeList(int param) {
    if (fxFieldOf(param) == FxParam::FilterType)
        return true;
    for (int filter = 0; filter < kFilters; ++filter)
        if (paramIndex(filter, FilterParam::Type) == param)
            return true;
    return false;
}

// The filter types as the editor shows them: families, which a filter's type list offers, of models, which the Model
// list beside its slope offers for a family of more than one (the low-passes, the high-passes and the band-passes).
// The editor's alone: the Type param stores the type itself, so hosts, presets and states see the types as they are.
enum class FilterFamily { LowPass, HighPass, BandPass, Notch, Comb, Formant, Morph, Count };
enum class FilterModel { Svf, Ladder, Diode, SallenKey, Count };
constexpr int kFilterFamilies = static_cast<int>(FilterFamily::Count);
constexpr int kMaxFilterModels = 4;
// Each family's types, its models, in the order its Model list offers them; the first the one the family is named by.
struct FilterFamilyTypes {
    FilterType types[kMaxFilterModels];
    int count;
};
constexpr FilterFamilyTypes kFilterFamilyTypes[] = {
    {{FilterType::LowPass, FilterType::Ladder, FilterType::Diode, FilterType::SallenKeyLow}, 4},
    {{FilterType::HighPass, FilterType::LadderHigh, FilterType::SallenKeyHigh}, 3},
    {{FilterType::BandPass, FilterType::LadderBand}, 2},
    {{FilterType::Notch}, 1},
    {{FilterType::Comb}, 1},
    {{FilterType::Formant}, 1},
    {{FilterType::Morph}, 1}};
static_assert(std::size(kFilterFamilyTypes) == static_cast<size_t>(kFilterFamilies), "the types of each family");
// Every type in exactly one family.
constexpr bool filterFamiliesCover() {
    for (int t = 0; t < static_cast<int>(FilterType::Count); ++t) {
        int found = 0;
        for (const FilterFamilyTypes &family : kFilterFamilyTypes)
            for (int m = 0; m < family.count; ++m)
                found += family.types[m] == static_cast<FilterType>(t) ? 1 : 0;
        if (found != 1)
            return false;
    }
    return true;
}
static_assert(filterFamiliesCover(), "every filter type in one family");

constexpr FilterType filterTypeAt(int value) {
    return static_cast<FilterType>(value < 0 ? 0 : value >= static_cast<int>(FilterType::Count)
                                                       ? static_cast<int>(FilterType::Count) - 1
                                                       : value);
}
constexpr FilterFamily filterFamilyOf(FilterType type) {
    for (int f = 0; f < kFilterFamilies; ++f)
        for (int m = 0; m < kFilterFamilyTypes[f].count; ++m)
            if (kFilterFamilyTypes[f].types[m] == type)
                return static_cast<FilterFamily>(f);
    return FilterFamily::LowPass;
}
constexpr const FilterFamilyTypes &filterFamilyTypes(FilterType type) {
    return kFilterFamilyTypes[static_cast<int>(filterFamilyOf(type))];
}
constexpr FilterModel filterModelOf(FilterType type) {
    switch (type) {
    case FilterType::Ladder:
    case FilterType::LadderHigh:
    case FilterType::LadderBand:
        return FilterModel::Ladder;
    case FilterType::Diode:
        return FilterModel::Diode;
    case FilterType::SallenKeyLow:
    case FilterType::SallenKeyHigh:
        return FilterModel::SallenKey;
    default:
        return FilterModel::Svf;
    }
}
// Whether a type's family offers models to pick from (its Model list shows).
constexpr bool hasFilterModels(FilterType type) {
    return filterFamilyTypes(type).count > 1;
}
// The type `family` plays in place of `now`: the same model, if the family has it, else the family's first.
constexpr FilterType filterTypeInFamily(FilterFamily family, FilterType now) {
    const FilterFamilyTypes &types = kFilterFamilyTypes[static_cast<int>(family)];
    for (int m = 0; m < types.count; ++m)
        if (filterModelOf(types.types[m]) == filterModelOf(now))
            return types.types[m];
    return types.types[0];
}
static_assert(filterTypeInFamily(FilterFamily::HighPass, FilterType::SallenKeyLow) == FilterType::SallenKeyHigh &&
                  filterTypeInFamily(FilterFamily::BandPass, FilterType::Diode) == FilterType::BandPass &&
                  filterTypeInFamily(FilterFamily::LowPass, FilterType::LadderBand) == FilterType::Ladder,
              "a family keeps the model where it has it");
// The family's name, its first type's ("Low-pass"), and a model's ("Ladder").
inline const char *filterFamilyName(FilterType type) {
    return filterTypeName(filterFamilyTypes(type).types[0]);
}
inline const char *filterModelName(FilterType type) {
    constexpr Str names[] = {Str::FilterModelSvf, Str::FilterModelLadder, Str::FilterModelDiode,
                             Str::FilterModelSallenKey};
    static_assert(std::size(names) == static_cast<size_t>(FilterModel::Count));
    return tr(names[static_cast<int>(filterModelOf(type))]);
}
inline const char *filterModelShortName(FilterType type) {
    constexpr Str names[] = {Str::FilterModelShortSvf, Str::FilterModelShortLadder, Str::FilterModelShortDiode,
                             Str::FilterModelShortSallenKey};
    static_assert(std::size(names) == static_cast<size_t>(FilterModel::Count));
    return tr(names[static_cast<int>(filterModelOf(type))]);
}

// A list button's `a` (and its arrows' and entries'): a filter's Model list, on the filter's Type param as its type's
// list is, with arrows (on a filter's page) or without (on the filter effect's card, which has the room for neither
// them nor the model's icon).
constexpr int kModelList = 1, kModelListPlain = 2;
inline bool isModelList(const Widget &w) {
    return (w.kind == WidgetKind::ListButton || w.kind == WidgetKind::ListStep || w.kind == WidgetKind::ListItem) &&
           w.a >= kModelList;
}

// A list whose button has arrows that step along it (see View::stepList()): a filter's type and a shaper's curve (a
// voice's or an effect's), an amp's model and cabinet, the reverb's algorithm and an LFO's shape.
inline bool isSteppableList(int param) {
    const FxParam field = fxFieldOf(param);
    return isFilterTypeList(param) || isClipMode(param) || field == FxParam::AmpModel || field == FxParam::AmpCab ||
           field == FxParam::ReverbType || isLfoShape(param);
}

// Adds widgets to a list: `add(kind, rect, param, a, b)`. A list button whose list has arrows gets them, over it.
struct WidgetAdder {
    std::vector<Widget> &list;
    void operator()(WidgetKind kind, Rect rect, int param = -1, int a = -1, int b = -1) const {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
        if (kind == WidgetKind::ListButton && isSteppableList(param) && a != kModelListPlain)
            for (int dir : {-1, 1}) {
                w.kind = WidgetKind::ListStep;
                w.rect = listStepButton(rect, dir);
                w.b = dir;
                list.push_back(w);
            }
    }
};

// A voice filter's morph mode, or the filter effect's: whether its Morph type goes through the notch (a switch).
inline bool isMorphMode(int param) {
    const int filter = filterOfParam(param);
    return fxFieldOf(param) == FxParam::FilterMorphMode ||
           (filter >= 0 && param == paramIndex(filter, FilterParam::MorphMode));
}

// The value a list's `item`th entry picks: an LFO's shapes and a shaper's curves in the order offered, a filter's type
// the family's or (`model`, its Model list) the model's type, from `shown`, the type now, keeping its model where the
// family has it, the others' by number.
inline int listItemValue(int param, int item, int shown = 0, bool model = false) {
    if (isFilterTypeList(param))
        return static_cast<int>(model ? filterFamilyTypes(filterTypeAt(shown)).types[item]
                                      : filterTypeInFamily(static_cast<FilterFamily>(item), filterTypeAt(shown)));
    return isLfoShape(param)   ? static_cast<int>(kLfoShapesOffered[item])
           : isClipMode(param) ? static_cast<int>(kClipModesOffered[item])
                               : item;
}

// A choice that opens a list under its button (an LFO's shape, a MIDI source's input, a shaper's curve, the reverb's
// algorithm, a filter's type or slope): how many values it offers (an LFO's shapes kLfoShapesOffered, in their
// order), and their names.
// A filter's type list offers its families, its Model list (`model`) the models of `shown`'s family.
inline int listCount(int param, int shown = 0, bool model = false) {
    return param == paramIndex(SeqParam::Scale)       ? static_cast<int>(SeqScale::Count)
           : routeModeFrom(param) >= 0                ? static_cast<int>(RouteMode::Count)
           : isSlopeList(param)                       ? kFilterSlopes
           : isFilterTypeList(param)                  ? (model ? filterFamilyTypes(filterTypeAt(shown)).count
                                                               : kFilterFamilies)
           : isLfoShape(param)                        ? kLfoShapeItems
           : isMidiInput(param)                       ? static_cast<int>(MidiInput::Count)
           : fxFieldOf(param) == FxParam::ReverbType ? static_cast<int>(ReverbType::Count)
           : fxFieldOf(param) == FxParam::AmpModel   ? static_cast<int>(AmpModel::Count)
           : fxFieldOf(param) == FxParam::AmpCab     ? static_cast<int>(AmpCab::Count)
                                                      : static_cast<int>(ClipMode::Count);
}
inline std::string listItemName(int param, int value, bool model = false) {
    if (param == paramIndex(SeqParam::Scale))
        return seqScaleName(value);
    if (routeModeFrom(param) >= 0)
        return routeModeName(static_cast<RouteMode>(value));
    if (isSlopeList(param))
        return std::to_string(slopeDb(value)) + " dB";
    if (isFilterTypeList(param)) // a type's family, or its model on its Model list
        return model ? filterModelName(filterTypeAt(value)) : filterFamilyName(filterTypeAt(value));
    if (isLfoShape(param))
        return lfoShapeName(static_cast<LfoShape>(value));
    if (isMidiInput(param))
        return midiInputName(static_cast<MidiInput>(value));
    if (fxFieldOf(param) == FxParam::AmpModel)
        return ampModelName(static_cast<AmpModel>(value));
    if (fxFieldOf(param) == FxParam::AmpCab)
        return ampCabName(static_cast<AmpCab>(value));
    return fxFieldOf(param) == FxParam::ReverbType ? reverbTypeName(static_cast<ReverbType>(value))
                                                    : clipModeName(static_cast<ClipMode>(value));
}

} // namespace fmvibe::gui::detail
