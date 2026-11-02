#pragma once

// The editor's icons: the effects', filter types', band shapes' and LFO modes' little pictures, the switches
// drawn as icons, and the waves' and LFO shapes' outlines; with PointCache::get, which keeps their points and the
// graphs' curves, and the keys that tell those apart. Shared by the editor's own files only (see view.cpp), in
// namespace detail; icons.cpp draws them.

#include "view.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fmvibe::gui {

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

// What tells the curves apart in a PointCache key: its first value, then where the curve goes.
enum class Curve {
    Filter = 1, ParamEq, Sweep, Driven, Drawn, Shaper, LfoEditor, ClipEditor, NoteEditor, NoteExact, Partials, Lfo,
    Envelope, Eq, LfoIcon, WaveIcon, ModeIcon, FilterIcon, PeqIcon, BandIcon,
};
inline std::vector<double> curveKey(Curve curve, Rect r) {
    return {static_cast<double>(curve), r.x, r.y, r.w, r.h};
}

void powerIcon(Canvas &c, Point center, Color color);
void filterIcon(Canvas &c, Rect r, FilterType type, Color color);
bool isFxIconSwitch(FxParam field);
void fxSwitchIcon(Canvas &c, Point m, FxParam field, Color color);
void mcBandIcon(Canvas &c, Rect r, int band, Color color);
void lfoModeIcon(Canvas &c, Point m, SourceParam mode, Color color);
void fxIcon(Canvas &c, Rect r, FxType type, Color color);
void peqIcon(Canvas &c, Rect r, PeqType type, Color color);

// Wave icons, one cycle across r.
std::vector<Point> lfoPoints(LfoShape shape, Rect r);
void customShapeDots(Canvas &c, Rect r, Color color);
std::vector<Point> wavePoints(OpWave wave, Rect r);
std::vector<Point> shapeIcon(LfoShape shape, Rect r);
std::vector<Point> waveIcon(OpWave wave, Rect r);

} // namespace fmvibe::gui::detail
