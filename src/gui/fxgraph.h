#pragma once

// The effects graph's look, shared by the editor's own files only (see fxgraph.cpp), in namespace detail.

#include "theme.h"

namespace fmvibe::gui::detail {

// Whether the editor lets a wire close a loop in the effects graph. The engine runs one with a chunk (64 samples) of
// delay, its source's output from the chunk before; the editor refuses them until they are made safe to play with.
constexpr bool kAllowFxLoops = false;

// A split's bands: the low one in the delay's blue, the middle one in the flanger's lime, the high one in the DJ EQ's
// amber.
inline Color bandColor(int band) {
    return fxTypeColor(band == 0 ? FxType::Delay : band == 1 ? FxType::Flanger : FxType::Eq);
}

// The splits' own color, the three-band dynamics' (which splits like them).
inline Color splitColor() {
    return fxTypeColor(FxType::MultiDyn);
}

// The effect cards' titles (see view.cpp).
const char *fxTitle(FxType type);

} // namespace fmvibe::gui::detail
