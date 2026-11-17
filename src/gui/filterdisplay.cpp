// A filter's display (see filterdisplay.h): its response, its spectrum and its handle, for the inspector's filter page
// and the filter effect's card alike.

#include "filterdisplay.h"

#include "icons.h"
#include "theme.h"
#include "../filter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace fmvibe::gui::detail {

namespace {

// --- Filter response display: x is the cutoff knob's log scale, y is dB.

constexpr float kResponseMinDb = -36.0f;
constexpr float kResponseMaxDb = 18.0f;

} // namespace

FilterParams voiceFilterParams(int filter) {
    return {paramIndex(filter, FilterParam::Type),     paramIndex(filter, FilterParam::Slope),
            paramIndex(filter, FilterParam::Cutoff),   paramIndex(filter, FilterParam::Resonance),
            paramIndex(filter, FilterParam::KeyTrack), paramIndex(filter, FilterParam::Mix),
            paramIndex(filter, FilterParam::Vowel),    paramIndex(filter, FilterParam::Morph),
            paramIndex(filter, FilterParam::MorphMode)};
}

FilterParams fxFilterParams(int slot) {
    return {paramIndex(slot, FxParam::FilterType),     paramIndex(slot, FxParam::FilterSlope),
            paramIndex(slot, FxParam::FilterCutoff),   paramIndex(slot, FxParam::FilterResonance),
            paramIndex(slot, FxParam::FilterKeyTrack), paramIndex(slot, FxParam::FilterMix),
            paramIndex(slot, FxParam::FilterVowel),    paramIndex(slot, FxParam::FilterMorph),
            paramIndex(slot, FxParam::FilterMorphMode)};
}

FilterType filterTypeOf(double value) {
    return static_cast<FilterType>(std::clamp(static_cast<int>(std::lround(value)), 0,
                                              static_cast<int>(FilterType::Count) - 1));
}

FilterKnobLabels filterKnobLabels(FilterType type, bool card) {
    if (type == FilterType::Formant)
        return {Str::LabelShift, Str::LabelQ};
    return {Str::LabelCutoff, card ? Str::LabelReso : Str::LabelResonance};
}

Point filterHandle(Rect r, double cutoff, double resonance) {
    return {r.x + 10.0f + static_cast<float>(cutoff) * (r.w - 20.0f),
            r.bottom() - 10.0f - static_cast<float>(resonance) * (r.h - 20.0f)};
}

std::pair<double, double> filterHandleValues(Rect r, Point p) {
    return {(p.x - r.x - 10.0f) / (r.w - 20.0f), (r.bottom() - 10.0f - p.y) / (r.h - 20.0f)};
}

// What an effect or a voice filter puts out, measured, faintly behind its curves: 0 dB at the top down to -72 at
// the bottom. Once the audio stops (the snapshot gets old), it falls on at the analyzer's pace, 55 dB a second.
void paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum, float age) {
    const float fall = 55.0f * std::max(age - 0.05f, 0.0f);
    if (fall >= 72.0f || std::none_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; }))
        return;
    std::vector<Point> points{{area.x, area.bottom()}};
    for (int band = 0; band < kSpectrumBands; ++band) {
        const float db = std::clamp(20.0f * std::log10(std::max(spectrum[band], 1e-6f)) - fall, -72.0f, 0.0f);
        points.push_back(
            {area.x + (band + 0.5f) / kSpectrumBands * area.w, area.bottom() - (db + 72.0f) / 72.0f * area.h});
    }
    points.push_back({area.right(), area.bottom()});
    c.fillPolygon(points.data(), points.size(), kText.alpha(0.1f));
    c.polyline(points.data() + 1, points.size() - 2, kDim.alpha(0.6f), 1.0f);
}

void paintFilterResponse(Canvas &c, Rect r, const FilterParams &f, const FilterReads &reads, double pitch, Color color,
                         const Spectrum *spectrum, float age) {
    const auto &value = reads.value;
    const auto &liveAmount = reads.live;
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const Rect inner{r.x + 10.0f, r.y + 10.0f, r.w - 20.0f, r.h - 20.0f};
    auto yOfDb = [&](float db) {
        const float t = (std::clamp(db, kResponseMinDb, kResponseMaxDb) - kResponseMinDb) /
                        (kResponseMaxDb - kResponseMinDb);
        return inner.bottom() - t * inner.h;
    };
    // A grid as the Param EQ's: every 12 dB, 0 dB the strongest, and a line at 100 Hz, 1 kHz and 10 kHz.
    for (float db : {-24.0f, -12.0f, 0.0f, 12.0f})
        c.line({inner.x, yOfDb(db)}, {inner.right(), yOfDb(db)}, kBorder.alpha(db == 0.0f ? 1.0f : 0.5f), 1.0f,
               db != 0.0f);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = inner.x + static_cast<float>(normFromCutoff(hz)) * inner.w;
        c.line({x, inner.y}, {x, inner.bottom()}, kBorder.alpha(0.5f), 1.0f, true);
    }
    if (spectrum) // what the filter puts out, behind its response
        paintSpectrum(c, inner, *spectrum, age);

    const auto type = static_cast<FilterType>(std::lround(value(f.type)));
    const double cutoff = value(f.cutoff);
    const double resonance = value(f.resonance);
    const int steps = type == FilterType::Comb ? 240 : 96; // comb teeth need more points
    const double slope = value(f.slope);
    // Mixed with what goes in, as it plays (and as modulation takes it, live), and the Formant type's vowel likewise.
    const double mix = std::clamp(liveAmount(f.mix), 0.0, 1.0);
    const double vowel = type == FilterType::Formant ? std::clamp(liveAmount(f.vowel), 0.0, 1.0) : 0.0;
    const double morph = type == FilterType::Morph ? std::clamp(liveAmount(f.morph), 0.0, 1.0) : 0.0;
    const MorphMode mode = type == FilterType::Morph ? morphMode(value(f.mode)) : MorphMode::Band;
    auto response = [&](double cutoffNorm, double res) {
        std::vector<double> key = curveKey(Curve::Filter, inner);
        key.insert(key.end(), {static_cast<double>(type), slope, cutoffNorm, res, mix, vowel, morph,
                               static_cast<double>(mode)});
        return curveCache().get(key, [&] {
            std::vector<Point> points;
            for (int i = 0; i <= steps; ++i) {
                const double x = static_cast<double>(i) / steps;
                const double magnitude = filterResponse(type, filterSlope(slope), cutoffFromNorm(cutoffNorm), res,
                                                        cutoffFromNorm(x), mix, vowel, morph, mode);
                const float db = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-6)));
                points.push_back({inner.x + static_cast<float>(x) * inner.w, yOfDb(db)});
            }
            return points;
        });
    };
    // Modulated, the curve shows how far its modulation takes it either way, faintly (as a knob's ring
    // does), and while notes play it follows where the newest voice is. Key tracking moves all of it with
    // `pitch` from C4 on, an octave per octave at 100 %, as far as the filter's cutoff goes (down to 10 Hz).
    const double keyShift = value(f.keyTrack) * (pitch - 60.0) / 12.0 / std::log2(kMaxCutoff / kMinCutoff);
    auto tracked = [&](double cutoffNorm) { return std::clamp(cutoffNorm + keyShift, normFromCutoff(10.0), 1.0); };
    const auto [cutoffLow, cutoffHigh] = reads.reach(f.cutoff, cutoff);
    const auto [resonanceLow, resonanceHigh] = reads.reach(f.resonance, resonance);
    if (cutoffLow != cutoffHigh || resonanceLow != resonanceHigh) {
        for (const auto &[at, res] : {std::pair{cutoffLow, resonanceLow}, std::pair{cutoffHigh, resonanceHigh}}) {
            const std::vector<Point> edge = response(tracked(at), res);
            c.polyline(edge.data(), edge.size(), color.alpha(0.3f), 1.0f);
        }
    }
    const std::vector<Point> points = response(tracked(liveAmount(f.cutoff)), liveAmount(f.resonance));
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    const float cutoffX = inner.x + static_cast<float>(cutoff) * inner.w;
    c.line({cutoffX, inner.y}, {cutoffX, inner.bottom()}, color.alpha(0.3f), 1.0f, true);
}

void paintFilterHandle(Canvas &c, Point center, bool focused, Color color, Rect r, const FilterParams &f,
                       const std::function<double(int)> &value) {
    c.fillCircle(center, focused ? 7.0f : 5.5f, focused ? color : kPanel);
    c.strokeCircle(center, focused ? 7.0f : 5.5f, color, 2.0f);
    if (focused) {
        char a[32], b[32], text[80];
        formatParam(f.cutoff, value(f.cutoff), a, sizeof(a));
        formatParam(f.resonance, value(f.resonance), b, sizeof(b));
        std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionResonance, {a, b}).c_str());
        c.text({r.x, r.y + 2.0f, r.w - 8.0f, 14.0f}, text, kTextLabel, kText, Align::Right);
    }
}

} // namespace fmvibe::gui::detail
