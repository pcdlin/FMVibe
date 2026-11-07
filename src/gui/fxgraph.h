#pragma once

// The effects graph's look, shared by the editor's own files only (see fxgraph.cpp), in namespace detail.

#include "theme.h"

namespace fmvibe::gui::detail {

// A split's bands: the low one in the delay's blue, the high one in the DJ EQ's amber (and the middle one, which the
// editor doesn't use, in the flanger's lime).
inline Color bandColor(int band) {
    return fxTypeColor(band == 0 ? FxType::Delay : band == 1 ? FxType::Flanger : FxType::Eq);
}

// The splits' and joins' color: a neutral grey, as they shape the line rather than the sound.
inline Color splitColor() {
    return kDim;
}
inline Color sumColor() {
    return kDim;
}

// The effect cards' titles (see view.cpp).
const char *fxTitle(FxType type);

// A graph's grid points, while Ctrl is held: a faint + at each, and at `target` (the point a node dragged lands on, if
// any) a larger one in the accent.
void paintGridMarks(Canvas &c, Rect area, int cols, int rows, const Point *target);

} // namespace fmvibe::gui::detail
