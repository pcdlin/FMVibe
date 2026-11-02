#include "icons.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace fmvibe::gui::detail {

namespace {

// The icons' points, which depend on nothing but which icon and where: one cache for every editor, which all paint
// on the main thread (as `look` in theme.h assumes).
PointCache icons;

} // namespace

// The power symbol: a ring open at the top, with a stroke down into the gap.
void powerIcon(Canvas &c, Point center, Color color) {
    constexpr float kRadius = 5.0f, kGap = 0.7f; // radians either side of 12 o'clock
    c.arc(center, kRadius, kGap, 2.0f * std::numbers::pi_v<float> - kGap, color, 1.6f);
    c.line({center.x, center.y - kRadius - 1.5f}, {center.x, center.y - 0.5f}, color, 1.6f);
}

// A filter type's response in a few strokes, for the type buttons: frequency across, level up.
void filterIcon(Canvas &c, Rect r, FilterType type, Color color) {
    std::vector<double> key = curveKey(Curve::FilterIcon, r);
    key.push_back(static_cast<double>(type));
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        auto peak = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
        for (int i = 0; i <= 24; ++i) {
            const float x = i / 24.0f;
            float y = 0.0f;
            switch (type) {
            case FilterType::LowPass: // flat, a little resonance at the corner, then down
                y = x < 0.55f ? 0.35f + 0.35f * peak(x, 0.55f, 80.0f) : 0.7f - 3.4f * (x - 0.55f);
                break;
            case FilterType::HighPass:
                y = x > 0.45f ? 0.35f + 0.35f * peak(x, 0.45f, 80.0f) : 0.7f - 3.4f * (0.45f - x);
                break;
            case FilterType::BandPass:
                y = -0.9f + 1.7f * peak(x, 0.5f, 30.0f);
                break;
            case FilterType::Notch:
                y = 0.5f - 1.4f * peak(x, 0.5f, 120.0f);
                break;
            case FilterType::Comb: // evenly spaced teeth
                y = -0.8f + 1.5f * std::pow(std::abs(std::cos(3.0f * kPi * x)), 6.0f);
                break;
            case FilterType::Count:
                break;
            }
            points.push_back({r.x + x * r.w, r.center().y - std::clamp(y, -1.0f, 1.0f) * r.h * 0.5f});
        }
        return points;
    });
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// An effect's tempo sync switch: the delay's, the phaser's or the flanger's.
bool isFxSync(FxParam field) {
    return field == FxParam::DelaySync || field == FxParam::PhaserSync || field == FxParam::FlangerSync;
}

// Effect switches drawn as icons, named in their tooltips: a metronome for any effect's SYNC, two chain links for
// the delay's, the shaper's and the three-band compressor's LINK, a ball bouncing between two paddles for the
// delay's PING-PONG, and for the shaper's SYM an odd curve, its halves turned about the middle.
bool isFxIconSwitch(FxParam field) {
    return isFxSync(field) || field == FxParam::DelayLink || field == FxParam::ShaperLink ||
           field == FxParam::McLink || field == FxParam::DelayPingPong || field == FxParam::ShaperSymmetric;
}
void fxSwitchIcon(Canvas &c, Point m, FxParam field, Color color) {
    if (isFxSync(field)) {
        const Point body[] = {{m.x - 5.0f, m.y + 5.5f}, {m.x - 2.0f, m.y - 5.5f}, {m.x + 2.0f, m.y - 5.5f},
                              {m.x + 5.0f, m.y + 5.5f}, {m.x - 5.0f, m.y + 5.5f}};
        c.polyline(body, std::size(body), color, 1.3f);
        c.line({m.x, m.y + 3.5f}, {m.x + 5.5f, m.y - 6.0f}, color, 1.3f);
        c.fillCircle({m.x + 2.8f, m.y - 1.4f}, 1.6f, color);
    } else if (field == FxParam::DelayLink || field == FxParam::ShaperLink || field == FxParam::McLink) {
        c.strokeRect({m.x - 9.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
        c.strokeRect({m.x - 2.0f, m.y - 3.5f, 11.0f, 7.0f}, color, 1.4f, 3.5f);
    } else if (field == FxParam::ShaperSymmetric) {
        c.line({m.x, m.y - 6.0f}, {m.x, m.y + 6.0f}, color.alpha(0.45f), 1.0f);
        std::vector<Point> points;
        for (int i = 0; i <= 24; ++i) { // a curve rising to level off, its left half the right one turned over
            const float t = i / 12.0f - 1.0f;
            points.push_back({m.x + 9.0f * t, m.y - 5.0f * std::tanh(2.5f * t)});
        }
        c.polyline(points.data(), points.size(), color, 1.4f);
        c.fillCircle(m, 1.6f, color);
    } else {
        c.line({m.x - 8.5f, m.y - 5.5f}, {m.x - 8.5f, m.y + 5.5f}, color, 1.6f);
        c.line({m.x + 8.5f, m.y - 5.5f}, {m.x + 8.5f, m.y + 5.5f}, color, 1.6f);
        const Point path[] = {{m.x - 6.0f, m.y - 4.0f}, {m.x + 6.0f, m.y}, {m.x - 6.0f, m.y + 4.0f}};
        c.polyline(path, std::size(path), color.alpha(0.6f), 1.0f);
        c.fillCircle({m.x - 4.5f, m.y + 3.0f}, 1.8f, color);
    }
}

// A three-band compressor's band, for its button: its slice of the spectrum (frequency across) raised over a faint
// baseline, the low band falling away to the right, the mid one a plateau, the high one rising from the left.
void mcBandIcon(Canvas &c, Rect r, int band, Color color) {
    c.line({r.x, r.bottom()}, {r.right(), r.bottom()}, color.alpha(0.35f), 1.0f);
    std::vector<double> key = curveKey(Curve::BandIcon, r);
    key.push_back(band);
    const std::vector<Point> points = icons.get(key, [&] {
        auto rise = [](float x, float at) { // 0 well below `at`, 1 well above, smoothly across a crossover
            const float t = std::clamp((x - at) / 0.22f + 0.5f, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        std::vector<Point> points;
        for (int i = 0; i <= 24; ++i) {
            const float x = i / 24.0f;
            const float level = band == 0 ? 1.0f - rise(x, 0.45f)
                                : band == 1 ? rise(x, 0.25f) * (1.0f - rise(x, 0.75f))
                                            : rise(x, 0.55f);
            points.push_back({r.x + x * r.w, r.bottom() - level * r.h});
        }
        return points;
    });
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// An LFO's mode switches, named in their tooltips: FREE an infinity sign, ONE-SHOT a single cycle that then stays
// where it ends, UNIPOLAR a wave that only rises from its baseline.
void lfoModeIcon(Canvas &c, Point m, SourceParam mode, Color color) {
    const std::vector<double> key{static_cast<double>(Curve::ModeIcon), m.x, m.y, static_cast<double>(mode)};
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        if (mode == SourceParam::Free) {
            for (int i = 0; i <= 48; ++i) {
                const float t = i / 48.0f * 2.0f * kPi, d = 1.0f + std::sin(t) * std::sin(t);
                points.push_back({m.x + 10.0f * std::cos(t) / d, m.y + 11.0f * std::sin(t) * std::cos(t) / d});
            }
        } else if (mode == SourceParam::OneShot) {
            for (int i = 0; i <= 24; ++i)
                points.push_back({m.x - 10.0f + i * 0.5f, m.y - 4.5f * std::sin(i / 24.0f * 2.0f * kPi)});
            points.push_back({m.x + 10.0f, m.y});
        } else {
            for (int i = 0; i <= 32; ++i)
                points.push_back(
                    {m.x - 10.0f + i * 0.625f, m.y + 5.0f - 4.5f * (1.0f - std::cos(i / 32.0f * 4.0f * kPi))});
        }
        return points;
    });
    if (mode == SourceParam::OneShot)
        c.fillCircle(points.back(), 1.7f, color);
    else if (mode != SourceParam::Free)
        c.line({m.x - 11.0f, m.y + 5.0f}, {m.x + 11.0f, m.y + 5.0f}, color.alpha(0.5f), 1.0f);
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// A small picture of each effect, for its card's title and the ADD EFFECT list.
void fxIcon(Canvas &c, Rect r, FxType type, Color color) {
    std::vector<Point> points;
    auto at = [&](float x, float y) { points.push_back({r.x + x * r.w, r.center().y - y * r.h * 0.5f}); };
    auto stroke = [&](Color tint) {
        c.polyline(points.data(), points.size(), tint, 1.4f);
        points.clear();
    };
    auto curve = [&](auto &&y) { // y(x) for x across, -1..1 up
        for (int i = 0; i <= 24; ++i)
            at(i / 24.0f, y(i / 24.0f));
        stroke(color);
    };
    auto bar = [&](float x, float height, Color tint) {
        c.line({r.x + x * r.w, r.bottom()}, {r.x + x * r.w, r.bottom() - height * r.h}, tint, 1.6f);
    };
    auto notch = [](float x, float at, float narrow) { return std::exp(-(x - at) * (x - at) * narrow); };
    switch (type) {
    case FxType::Shaper: // a sine with its top and bottom cut flat
        curve([](float x) { return std::clamp(1.7f * std::sin(2.0f * kPi * x), -1.0f, 1.0f) * 0.8f; });
        break;
    case FxType::Delay: // the sound, then echoes, each softer
        bar(0.1f, 1.0f, color);
        bar(0.5f, 0.6f, color);
        bar(0.9f, 0.32f, color);
        break;
    case FxType::Eq: // three faders
        for (int k = 0; k < 3; ++k) {
            const float x = r.x + (0.17f + 0.33f * k) * r.w;
            const float y = r.y + (k == 1 ? 0.7f : (k == 0 ? 0.35f : 0.5f)) * r.h;
            c.line({x, r.y}, {x, r.bottom()}, color.alpha(0.45f), 1.0f);
            c.fillRect({x - 2.5f, y - 1.5f, 5.0f, 3.0f}, color, 1.0f);
        }
        break;
    case FxType::ParamEq: // a bell
        curve([&](float x) { return -0.6f + 1.4f * notch(x, 0.5f, 25.0f); });
        break;
    case FxType::Filter: // a low-pass, with a little resonance at its corner
        curve([&](float x) { return x < 0.5f ? 0.3f + 0.4f * notch(x, 0.5f, 60.0f) : 0.7f - 3.2f * (x - 0.5f); });
        break;
    case FxType::Compressor: // output against input, bending over at the threshold
    case FxType::MultiComp:  // the same, over three bands
        if (type == FxType::MultiComp)
            for (float x : {1.0f / 3.0f, 2.0f / 3.0f})
                c.line({r.x + x * r.w, r.y}, {r.x + x * r.w, r.bottom()}, color.alpha(0.4f), 1.0f);
        curve([](float x) { return x < 0.55f ? -1.0f + 2.4f * x : 0.32f + 0.5f * (x - 0.55f); });
        break;
    case FxType::Reverb: // a dense tail dying away
        for (int k = 0; k < 8; ++k)
            bar(0.06f + 0.125f * k, 1.0f - 0.115f * k, color.alpha(k == 0 ? 1.0f : 0.75f));
        break;
    case FxType::Phaser: // a few wide notches
        curve([&](float x) { return 0.6f - 1.4f * (notch(x, 0.28f, 90.0f) + notch(x, 0.72f, 90.0f)); });
        break;
    case FxType::Flanger: // a comb of narrow notches
        curve([&](float x) {
            float y = 0.6f;
            for (int k = 0; k < 4; ++k)
                y -= 1.5f * notch(x, 0.125f + 0.25f * k, 500.0f);
            return y;
        });
        break;
    case FxType::Disperser: { // a chirp: the sound smeared, its cycles drawn out
        std::vector<Point> chirp;
        for (int i = 0; i <= 40; ++i) {
            const float x = i / 40.0f;
            chirp.push_back({r.x + x * r.w, r.y + r.h * (0.5f - 0.42f * std::sin(2.0f * kPi * (1.0f + 3.0f * x) * x))});
        }
        c.polyline(chirp.data(), chirp.size(), color, 1.4f);
        break;
    }
    case FxType::FreqShifter: { // a partial, moved along
        bar(0.12f, 0.9f, color.alpha(0.4f));
        bar(0.88f, 0.9f, color);
        const float y = r.y + 0.35f * r.h;
        c.line({r.x + 0.28f * r.w, y}, {r.x + 0.7f * r.w, y}, color, 1.4f);
        const Point head[] = {{r.x + 0.76f * r.w, y}, {r.x + 0.64f * r.w, y - 3.0f}, {r.x + 0.64f * r.w, y + 3.0f}};
        c.fillPolygon(head, 3, color);
        break;
    }
    case FxType::Widener: { // the middle, and the sound spreading out of it to both sides
        const float cx = r.center().x;
        c.line({cx, r.y}, {cx, r.bottom()}, color.alpha(0.5f), 1.4f);
        for (float side : {-1.0f, 1.0f}) {
            const Point wing[] = {{cx + side * 0.2f * r.w, r.y + 0.1f * r.h},
                                  {cx + side * 0.48f * r.w, r.center().y},
                                  {cx + side * 0.2f * r.w, r.bottom() - 0.1f * r.h}};
            c.polyline(wing, 3, color, 1.4f);
        }
        break;
    }
    case FxType::None:
    case FxType::Count:
        break;
    }
}

// A band shape's response in a few strokes: frequency across, level up. Cuts show a little resonance
// at their corner, which tells them from the shelves.
void peqIcon(Canvas &c, Rect r, PeqType type, Color color) {
    std::vector<double> key = curveKey(Curve::PeqIcon, r);
    key.push_back(static_cast<double>(type));
    const std::vector<Point> points = icons.get(key, [&] {
        std::vector<Point> points;
        const double q = normFromQ(isCut(type) ? 2.2 : 0.7);
        for (int i = 0; i <= 20; ++i) {
            const double x = i / 20.0;
            const double db = 20.0 * std::log10(peqResponse(cutoffFromNorm(0.15 + 0.7 * x), type, 0.5, 9.0, q));
            points.push_back({r.x + static_cast<float>(x) * r.w,
                              r.center().y - static_cast<float>(std::clamp(db, -15.0, 15.0) / 15.0) * r.h * 0.5f});
        }
        return points;
    });
    c.polyline(points.data(), points.size(), color, 1.4f);
}

// --- Wave icons, one cycle across r.

std::vector<Point> lfoPoints(LfoShape shape, Rect r) {
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (shape) {
    case LfoShape::Sine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 48.0f));
        break;
    case LfoShape::Triangle:
        at(0.0f, 0.0f);
        at(0.25f, 1.0f);
        at(0.75f, -1.0f);
        at(1.0f, 0.0f);
        break;
    case LfoShape::Saw:
        at(0.0f, -1.0f);
        at(1.0f, 1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Square:
        at(0.0f, 1.0f);
        at(0.5f, 1.0f);
        at(0.5f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case LfoShape::Random: {
        const float steps[] = {0.35f, -0.7f, 0.85f, -0.25f};
        for (int i = 0; i < 4; ++i) {
            at(i / 4.0f, steps[i]);
            at((i + 1) / 4.0f, steps[i]);
        }
        break;
    }
    case LfoShape::Custom: // straight lines between points you can drag; see customShapeDots()
        at(0.0f, -0.7f);
        at(0.35f, 0.8f);
        at(0.7f, -0.1f);
        at(1.0f, 0.6f);
        break;
    case LfoShape::Count:
        break;
    }
    return points;
}

// The Custom icon's line has a dot on each point, like the shape editor's handles.
void customShapeDots(Canvas &c, Rect r, Color color) {
    for (const Point &point : lfoPoints(LfoShape::Custom, r)) {
        c.fillCircle(point, 2.6f, kRaised);
        c.strokeCircle(point, 2.0f, color, 1.3f);
    }
}

std::vector<Point> wavePoints(OpWave wave, Rect r) {
    switch (wave) {
    case OpWave::Sine:
        return lfoPoints(LfoShape::Sine, r);
    case OpWave::Triangle:
        return lfoPoints(LfoShape::Triangle, r);
    case OpWave::Saw:
        return lfoPoints(LfoShape::Saw, r);
    case OpWave::Square:
        return lfoPoints(LfoShape::Square, r);
    default:
        break;
    }
    std::vector<Point> points;
    auto at = [&](float t, float v) { points.push_back({r.x + t * r.w, r.center().y - v * r.h * 0.5f}); };
    switch (wave) {
    case OpWave::Pulse:
        at(0.0f, 1.0f);
        at(0.25f, 1.0f);
        at(0.25f, -1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::HalfSine:
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, 2.0f * std::sin(kPi * i / 24.0f) - 1.0f);
        at(1.0f, -1.0f);
        break;
    case OpWave::AbsSine:
        for (int i = 0; i <= 48; ++i)
            at(i / 48.0f, 2.0f * std::abs(std::sin(2.0f * kPi * i / 48.0f)) - 1.0f);
        break;
    case OpWave::AltSine: // a whole sine in the first half, then flat
        for (int i = 0; i <= 24; ++i)
            at(i / 48.0f, std::sin(2.0f * kPi * i / 24.0f));
        at(1.0f, 0.0f);
        break;
    case OpWave::Organ: // its drawbars added up
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.44f * (std::sin(t) + std::sin(2.0f * t) +
                                   0.5f * (std::sin(3.0f * t) + std::sin(4.0f * t)) +
                                   0.25f * (std::sin(6.0f * t) + std::sin(8.0f * t))));
        }
        break;
    case OpWave::Vowel: // a ripple at the formant, dying away through each cycle
        for (int i = 0; i <= 48; ++i) {
            const float t = i / 48.0f;
            at(t, std::exp(-3.5f * t) * std::sin(2.0f * kPi * 4.0f * t + 0.6f));
        }
        break;
    case OpWave::Noise: {
        const float values[] = {0.1f, 0.8f, -0.5f, 0.3f, -0.9f, 0.6f, -0.2f, 0.9f, -0.7f, 0.2f, -0.4f, 0.7f, -0.8f};
        for (int i = 0; i < 13; ++i)
            at(i / 12.0f, values[i]);
        break;
    }
    case OpWave::Custom: // a few partials added up, standing for whatever was drawn
        for (int i = 0; i <= 48; ++i) {
            const float t = 2.0f * kPi * i / 48.0f;
            at(i / 48.0f, 0.65f * std::sin(t) + 0.35f * std::sin(3.0f * t + 0.6f) + 0.2f * std::sin(5.0f * t));
        }
        break;
    default:
        break;
    }
    return points;
}

// An LFO shape's or a wave's icon, as lfoPoints() and wavePoints() draw it, kept with the other icons.
std::vector<Point> shapeIcon(LfoShape shape, Rect r) {
    std::vector<double> key = curveKey(Curve::LfoIcon, r);
    key.push_back(static_cast<double>(shape));
    return icons.get(key, [&] { return lfoPoints(shape, r); });
}
std::vector<Point> waveIcon(OpWave wave, Rect r) {
    std::vector<double> key = curveKey(Curve::WaveIcon, r);
    key.push_back(static_cast<double>(wave));
    return icons.get(key, [&] { return wavePoints(wave, r); });
}

} // namespace fmvibe::gui::detail
