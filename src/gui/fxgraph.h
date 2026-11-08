#pragma once

// The effects graph's look, shared by the editor's own files only (see fxgraph.cpp), in namespace detail.

#include "theme.h"

#include <algorithm>

namespace fmvibe::gui::detail {

// A split's bands by their port's number (0 low, 1 middle, 2 high), in the theme's band colors.
inline Color bandColor(int band) {
    return look.bands[std::clamp(band, 0, 2)];
}

// A split's bands as the line has them, by lane (1 the high band, 2 the low one, 3 the middle one): the port's band
// each comes out of (fxBandPort()), the join's input each goes into (the high band its first, the low one its second,
// the middle one its third), the join's solo of it, and its color.
inline int laneBand(int lane) {
    return lane == 1 ? 2 : lane == 3 ? 1 : 0;
}
inline int laneJoinInput(int lane) {
    return lane == 1 ? 0 : lane == 3 ? 2 : 1;
}
inline SumParam laneSolo(int lane) {
    return lane == 1 ? SumParam::SoloHigh : lane == 3 ? SumParam::SoloMid : SumParam::SoloLow;
}
inline Color laneColor(int lane) {
    return bandColor(laneBand(lane));
}
// A split's tabs by the band they show, as the layout keeps it (0 the low band, 1 the high one, 2 the middle one), and
// back; and where each tab sits, left to right: the low band's, the middle one's, the high one's.
inline int laneOfTab(int tab) {
    return tab == 1 ? 1 : tab == 2 ? 3 : 2;
}
inline int tabOfLane(int lane) {
    return lane == 1 ? 1 : lane == 3 ? 2 : 0;
}
inline int tabPosition(int tab, bool three) {
    return tab == 0 ? 0 : tab == 2 ? 1 : three ? 2 : 1;
}

// The splits' and joins' color: a neutral grey, as they shape the line rather than the sound.
inline Color splitColor() {
    return kDim;
}
inline Color sumColor() {
    return kDim;
}
// A split's controls stand on grey: its container's column and outline in the row, and its card in the graph.
inline Color splitFill() {
    return mix(kCardColor, kDim, 0.1f);
}
inline Color splitLine() {
    return mix(kCardColor, kDim, 0.5f);
}

// The effect cards' titles (see view.cpp).
const char *fxTitle(FxType type);

// A graph's grid points, while Ctrl is held: a faint + at each, and at `target` (the point a node dragged lands on, if
// any) a larger one in the accent.
void paintGridMarks(Canvas &c, Rect area, int cols, int rows, const Point *target);

} // namespace fmvibe::gui::detail
