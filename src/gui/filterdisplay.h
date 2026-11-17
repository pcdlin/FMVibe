#pragma once

// A filter's display, as the inspector's filter page (a voice filter's) and the filter effect's card show it: its
// response from its params, faint curves either side as far as its modulation takes it, what it puts out measured
// behind it, and its handle, which drags its cutoff across and its resonance up and down. Shared by the editor's own
// files only (see view.cpp), in namespace detail; filterdisplay.cpp draws them.

#include "canvas.h"
#include "../analyzer.h"
#include "../params.h"
#include "../uistrings.h"

#include <functional>
#include <utility>

namespace fmvibe::gui::detail {

// A filter's params, a voice filter's or the filter effect's in its slot, as its display draws them.
struct FilterParams {
    int type, slope, cutoff, resonance, keyTrack, mix, vowel, morph, mode;
};
FilterParams voiceFilterParams(int filter);
FilterParams fxFilterParams(int slot);

// What a display reads of the patch: a param's value; where modulation takes it now, as a share of its range (0..1);
// and how far its modulations take it from `set` (a share of its range) down and up, held to 0..1.
struct FilterReads {
    std::function<double(int)> value;
    std::function<double(int)> live;
    std::function<std::pair<double, double>(int, double)> reach;
};

FilterType filterTypeOf(double value);

// What a filter's Cutoff and Resonance knobs are called for its type: a formant's shift its formants and narrow them;
// on the effect's card, Resonance is Reso.
struct FilterKnobLabels {
    Str cutoff, resonance;
};
FilterKnobLabels filterKnobLabels(FilterType type, bool card);

// Where the handle on a display `r` sits for a cutoff and a resonance (each 0..1), and what it sets dragged to `p`: its
// cutoff and resonance, as shares of their ranges (not held to them).
Point filterHandle(Rect r, double cutoff, double resonance);
std::pair<double, double> filterHandleValues(Rect r, Point p);

// An analyzer's spectrum in `area`, behind a response: as measured, falling away for as long as the measurement has
// aged (`age`, in seconds) until it is gone.
void paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum, float age);

// A filter's response in `r`, from its params; its key tracking follows `pitch` (semitones, 69 is A4). `spectrum`:
// what the filter puts out, measured `age` seconds ago, to show behind the response, if any.
void paintFilterResponse(Canvas &c, Rect r, const FilterParams &f, const FilterReads &reads, double pitch, Color color,
                         const Spectrum *spectrum = nullptr, float age = 0.0f);

// The handle at `center`, in `color`: larger and filled while under the mouse or dragged (`focused`), its cutoff and
// resonance then showing at the top right of its display `r`.
void paintFilterHandle(Canvas &c, Point center, bool focused, Color color, Rect r, const FilterParams &f,
                       const std::function<double(int)> &value);

} // namespace fmvibe::gui::detail
