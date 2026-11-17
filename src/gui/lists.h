#pragma once

// The lists the editor's list buttons open (Kind::ListButton, see View's lists): which params open one, how many
// entries each offers, the value each entry picks and its name, and which have arrows that step along them. Shared by
// the View and the panels whose cards and pages hold lists (see inspectorpanel.h), in namespace detail.

#include "layout.h"
#include "../params.h"

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

// The value a list's `item`th entry picks: an LFO's shapes and a shaper's curves in the order offered, the others'
// by number.
inline int listItemValue(int param, int item) {
    return isLfoShape(param)   ? static_cast<int>(kLfoShapesOffered[item])
           : isClipMode(param) ? static_cast<int>(kClipModesOffered[item])
                               : item;
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

// A list whose button has arrows that step along it (see View::stepList()): a filter's type and a shaper's curve (a
// voice's or an effect's), an amp's model and cabinet, the reverb's algorithm and an LFO's shape.
inline bool isSteppableList(int param) {
    const FxParam field = fxFieldOf(param);
    return isFilterTypeList(param) || isClipMode(param) || field == FxParam::AmpModel || field == FxParam::AmpCab ||
           field == FxParam::ReverbType || isLfoShape(param);
}

// A voice filter's morph mode, or the filter effect's: whether its Morph type goes through the notch (a switch).
inline bool isMorphMode(int param) {
    const int filter = filterOfParam(param);
    return fxFieldOf(param) == FxParam::FilterMorphMode ||
           (filter >= 0 && param == paramIndex(filter, FilterParam::MorphMode));
}

// A choice that opens a list under its button (an LFO's shape, a MIDI source's input, a shaper's curve, the reverb's
// algorithm, a filter's type or slope): how many values it offers (an LFO's shapes kLfoShapesOffered, in their
// order), and their names.
inline int listCount(int param) {
    return param == paramIndex(SeqParam::Scale)       ? static_cast<int>(SeqScale::Count)
           : routeModeFrom(param) >= 0                ? static_cast<int>(RouteMode::Count)
           : isSlopeList(param)                       ? kFilterSlopes
           : isFilterTypeList(param)                  ? static_cast<int>(FilterType::Count)
           : isLfoShape(param)                        ? kLfoShapeItems
           : isMidiInput(param)                       ? static_cast<int>(MidiInput::Count)
           : fxFieldOf(param) == FxParam::ReverbType ? static_cast<int>(ReverbType::Count)
           : fxFieldOf(param) == FxParam::AmpModel   ? static_cast<int>(AmpModel::Count)
           : fxFieldOf(param) == FxParam::AmpCab     ? static_cast<int>(AmpCab::Count)
                                                      : static_cast<int>(ClipMode::Count);
}
inline std::string listItemName(int param, int value) {
    if (param == paramIndex(SeqParam::Scale))
        return seqScaleName(value);
    if (routeModeFrom(param) >= 0)
        return routeModeName(static_cast<RouteMode>(value));
    if (isSlopeList(param))
        return std::to_string(slopeDb(value)) + " dB";
    if (isFilterTypeList(param))
        return filterTypeName(static_cast<FilterType>(value));
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
