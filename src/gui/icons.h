#pragma once

// The editor's icons: the effects', filter types', band shapes' and LFO modes' little pictures, the switches
// drawn as icons, and the waves' and LFO shapes' outlines; with PointCache, which keeps their points and the graphs'
// curves (curveCache()), and the keys that tell those apart. Shared by the editor's own files only (see view.cpp), in
// namespace detail; icons.cpp draws them.

#include "view.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fmvibe::gui {

// Points that depend only on what their key holds (the params a curve is drawn from, where it goes), kept from
// frame to frame and worked out again only when any of that changes.
class PointCache {
public:
    // The points for `key` (its first value telling the curves apart), from make() unless they are kept already.
    template <class Make>
    std::vector<Point> get(const std::vector<double> &key, Make &&make);

private:
    struct Entry {
        uint64_t hash = 0;
        std::vector<double> key;
        std::vector<Point> points;
        uint64_t used = 0;
    };
    std::vector<Entry> entries_;
    uint64_t clock_ = 0;
};

// Keys are compared bit for bit (+0 and -0 apart), so points are only found again for a key that make() would turn
// into the same points. Past a few dozen curves, the one used least recently makes room for a new one.
template <class Make>
std::vector<Point> PointCache::get(const std::vector<double> &key, Make &&make) {
    constexpr size_t kMostEntries = 96;
    uint64_t hash = key.size();
    for (double v : key) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        hash = (hash ^ bits) * 0x9E3779B97F4A7C15ull;
        hash ^= hash >> 31;
    }
    ++clock_;
    for (Entry &entry : entries_)
        if (entry.hash == hash && entry.key.size() == key.size() &&
            std::memcmp(entry.key.data(), key.data(), key.size() * sizeof(double)) == 0) {
            entry.used = clock_;
            return entry.points;
        }
    std::vector<Point> points = make(); // before a place is picked for it, so make() may use the cache too
    Entry *slot = nullptr;
    if (entries_.size() < kMostEntries)
        slot = &entries_.emplace_back();
    else
        slot = &*std::min_element(entries_.begin(), entries_.end(),
                                  [](const Entry &a, const Entry &b) { return a.used < b.used; });
    slot->hash = hash;
    slot->key = key;
    slot->points = std::move(points);
    slot->used = clock_;
    return slot->points;
}

} // namespace fmvibe::gui

namespace fmvibe::gui::detail {

// The graphs' curves (the effects' and filters' responses, the envelopes, the drawn shapes), worked out again only
// when what they are drawn from changes: one cache for every editor, which all paint on the main thread, as the icons'
// is (see icons.cpp).
PointCache &curveCache();

// What tells the curves apart in a PointCache key: its first value, then where the curve goes.
enum class Curve {
    Filter = 1, ParamEq, Sweep, Driven, Drawn, Shaper, LfoEditor, ClipEditor, NoteEditor, NoteExact, Partials, Lfo,
    Envelope, Eq, LfoIcon, WaveIcon, ModeIcon, FilterIcon, PeqIcon, PolarityIcon, VelocityEditor, RouteIcon, WarpIcon,
    TypeIcon,
};
inline std::vector<double> curveKey(Curve curve, Rect r) {
    return {static_cast<double>(curve), r.x, r.y, r.w, r.h};
}

void powerIcon(Canvas &c, Point center, Color color);
// A transport's play triangle, about 9 x 9, or its stop square while `stop`: the SEQ card's Play button.
void transportIcon(Canvas &c, Point center, Color color, bool stop);
void keyboardIcon(Canvas &c, Point center, Color color, bool crossed);
void resetIcon(Canvas &c, Point center, Color color);
// About 15 x 13: an open folder (Load), a floppy disk (Save; with a plus in its corner, Save As), a cog (Settings)
// and three dots across (the overflow menu's button).
void folderIcon(Canvas &c, Point center, Color color);
void saveIcon(Canvas &c, Point center, Color color, bool plus);
void cogIcon(Canvas &c, Point center, Color color);
void moreIcon(Canvas &c, Point center, Color color);
// About 14 x 14: an arrow down into an open tray (the overflow menu's Import sysex).
void importIcon(Canvas &c, Point center, Color color);
// A tuning fork, about 8 x 15: its two prongs joined at the bottom and its stem below; the title bar's tuning mark.
void tuningForkIcon(Canvas &c, Point center, Color color);
// A die showing five, about 12 x 12: the partial editor's random button.
void diceIcon(Canvas &c, Point center, Color color);
void doneIcon(Canvas &c, Point center, Color color);
// A pencil, about 11 x 11, writing toward the lower left: the edit buttons'.
void editIcon(Canvas &c, Point center, Color color);
// An open book, about 14 x 11: the settings dialog's button that opens the user manual.
void bookIcon(Canvas &c, Point center, Color color);
// Two arrows pointing in at a middle line, across: a widened card back to its size; and two pointing out to opposite
// corners: the full-size editor.
void collapseIcon(Canvas &c, Point center, Color color);
void expandIcon(Canvas &c, Point center, Color color);
// A widened LFO card's tools, by ShapeEditor::ShapeTool: invert, reverse, shift left and right, copy and paste.
void shapeToolIcon(Canvas &c, Point center, int tool, Color color);
// What a click on a shape does: points (a dot on a line), steps (a stair); and snapping to the grid (a grid).
void pointsIcon(Canvas &c, Point center, Color color);
void stepsIcon(Canvas &c, Point center, Color color);
void gridIcon(Canvas &c, Point center, Color color);
// A double-headed arrow across (axis 0) or up and down (1), by the grid's steppers.
void axisIcon(Canvas &c, Point center, int axis, Color color);
void filterIcon(Canvas &c, Rect r, FilterType type, Color color);
bool isFxIconSwitch(FxParam field);
void fxSwitchIcon(Canvas &c, Point m, FxParam field, Color color);
void lfoClockIcon(Canvas &c, Point m, LfoClock clock, Color color, float scale = 1.0f);
// An operator's envelope's two kinds, about 16 x 10 around `m`: an AHDSR's shape (attack, hold, decay, sustain and
// release), or a curve through its points (a drawn one).
// An operator's envelope mode (0 ADSR, 1 Curve, 2 Rate/Level: its four levels as bars on a baseline, tall to short).
void envModeIcon(Canvas &c, Point m, int mode, Color color, float scale = 1.0f);
// A modulation's polarity, about 10 x 8 around `m`: a wave about a middle line for bipolar, humps up from a
// baseline for unipolar, like the LFO's UNIPOLAR switch.
void polarityIcon(Canvas &c, Point m, bool bipolar, Color color, float scale = 1.0f);
// A plus, or a minus: the phaser's and flanger's polarity.
void polaritySign(Canvas &c, Point m, bool minus, Color color);
// Two speakers facing apart, about 23 x 10 around `m`: STEREO, the phaser's and flanger's sides swept apart and the
// amp's two sides played apart.
void stereoIcon(Canvas &c, Point m, Color color);
// An eighth note, about 9 x 12 around `m`: an operator's NOTE switch, its wave restarted with each cycle of the note.
void noteIcon(Canvas &c, Point m, Color color);
// How a route acts on the operator it goes into, about 10 x 10 around `m` (times `scale`): Phase a phi (a ring with a stroke through
// it), Linear FM a straight rising line, Exp FM a curve rising ever faster, Ring a ring with a cross in it.
void routeModeIcon(Canvas &c, Point m, RouteMode mode, Color color, float scale = 1.0f);
// A feedback's mode (see FeedbackMode), about `m`: Smooth a smooth wave, DX7 one in steps, as the DX7's samples hold it.
void feedbackModeIcon(Canvas &c, Point m, FeedbackMode mode, Color color, float scale = 1.0f);
// An operator's warp: a cycle of a sine across `r` as the warp reads it at three quarters of its amount (Steps at
// its full amount; Off: the sine).
void warpIcon(Canvas &c, Rect r, OpWarp warp, Color color);
// How unison copies spread in pitch, about 12 x 9 around `m`: five ticks across, where five copies' pitches go.
void uniCurveIcon(Canvas &c, Point m, UniCurve curve, Color color);
// Where they sit, about 12 x 10 around `m`: five dots, rising in pitch from bottom to top, placed across as they sit.
void uniPanIcon(Canvas &c, Point m, UniPan pan, Color color);
void fxIcon(Canvas &c, Rect r, FxType type, Color color);
// A split of the effects graph: a line forking into two; and a join, two lines meeting and going on as one.
void splitIcon(Canvas &c, Rect r, Color color, int bands = 2); // a fork into two prongs, or three
void joinIcon(Canvas &c, Rect r, Color color, int bands = 2);
// A bin, about 12 x 14 around `m`: removing everything.
void binIcon(Canvas &c, Point m, Color color);
// The FX panel's switch between the chain (cards in a row) and the graph (nodes and wires).
void fxModeIcon(Canvas &c, Point m, bool graph, Color color);
void peqIcon(Canvas &c, Rect r, PeqType type, Color color);
// The type lists' icons, before an entry's name on a list's button and in its list, in a box about 16 x 10 as an LFO
// shape's: a shaper's curve (its transfer curve, a drawn one's points, Stack's layers), the reverb's algorithm and the
// amp's model (pictures of what they sound like), and a MIDI source's input.
void clipModeIcon(Canvas &c, Rect r, ClipMode mode, Color color);
void reverbTypeIcon(Canvas &c, Rect r, ReverbType type, Color color);
void ampModelIcon(Canvas &c, Rect r, AmpModel model, Color color);
void midiInputIcon(Canvas &c, Rect r, MidiInput input, Color color);

// Wave icons, one cycle across r.
std::vector<Point> lfoPoints(LfoShape shape, Rect r);
void customShapeDots(Canvas &c, Rect r, Color color);
std::vector<Point> wavePoints(OpWave wave, Rect r);
std::vector<Point> shapeIcon(LfoShape shape, Rect r);
std::vector<Point> waveIcon(OpWave wave, Rect r);

} // namespace fmvibe::gui::detail
