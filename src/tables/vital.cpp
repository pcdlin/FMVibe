// Vital's .vitaltable, rendered into frames as Vital would show them: a JSON file of groups, each a chain of components
// with keyframes along the table's 257 positions. A component is a source (a wave stored as samples, a line drawn from
// points, or a stretch of an audio file) or a modifier of what came before it in its group (a phase shift, a window, a
// filter, a slew limit, a fold or a warp). The groups' frames are averaged. Files from Vital's early versions store some
// of this differently, as the version in the file tells (see loadComponent()).

#include "readers.h"

#include "../fft.h"
#include "../opdata.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace fmvibe {

namespace {

constexpr int kSize = FrameTable::kFrameSize;
constexpr int kTop = kSize / 2 - 1; // the highest harmonic the modifiers work on
constexpr int kMostFrames = 257;    // a table's positions, 0 to 256
constexpr double kPi = std::numbers::pi;

using Spectrum = std::vector<std::complex<double>>;

// --- The JSON.

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::string> keys; // an object's, one per item
    const Json *find(const char *key) const {
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key)
                return &items[i];
        return nullptr;
    }
    double num(const char *key, double fallback) const {
        const Json *j = find(key);
        return j && (j->kind == Number || j->kind == Bool) ? j->number : fallback;
    }
    std::string str(const char *key) const {
        const Json *j = find(key);
        return j && j->kind == String ? j->text : std::string();
    }
};

class JsonParser {
public:
    JsonParser(const char *at, const char *end) : at_(at), end_(end) {}
    bool parse(Json &out) {
        const bool ok = value(out, 0);
        space();
        return ok && at_ == end_;
    }

private:
    void space() {
        while (at_ < end_ && std::isspace(static_cast<unsigned char>(*at_)))
            ++at_;
    }
    bool literal(const char *word) {
        const size_t n = std::strlen(word);
        if (static_cast<size_t>(end_ - at_) < n || std::memcmp(at_, word, n) != 0)
            return false;
        at_ += n;
        return true;
    }
    bool string(std::string &out) {
        if (at_ >= end_ || *at_ != '"')
            return false;
        ++at_;
        while (at_ < end_ && *at_ != '"') {
            if (*at_ == '\\') {
                if (++at_ >= end_)
                    return false;
                const char c = *at_++;
                switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                    if (end_ - at_ < 4)
                        return false;
                    at_ += 4;
                    out += '?';
                    break;
                default: out += c;
                }
            } else { // the run up to the next quote or escape at once: a table's audio is one long string
                const char *const run = at_;
                while (at_ < end_ && *at_ != '"' && *at_ != '\\')
                    ++at_;
                out.append(run, at_);
            }
        }
        if (at_ >= end_)
            return false;
        ++at_;
        return true;
    }
    bool value(Json &out, int depth) {
        if (depth > 64)
            return false;
        space();
        if (at_ >= end_)
            return false;
        const char c = *at_;
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            out.kind = object ? Json::Object : Json::Array;
            ++at_;
            space();
            if (at_ < end_ && *at_ == (object ? '}' : ']')) {
                ++at_;
                return true;
            }
            for (;;) {
                space();
                if (object) {
                    std::string key;
                    if (!string(key))
                        return false;
                    space();
                    if (at_ >= end_ || *at_ != ':')
                        return false;
                    ++at_;
                    out.keys.push_back(std::move(key));
                }
                out.items.emplace_back();
                if (!value(out.items.back(), depth + 1))
                    return false;
                space();
                if (at_ < end_ && *at_ == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < end_ && *at_ == (object ? '}' : ']')) {
                    ++at_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"')
            return out.kind = Json::String, string(out.text);
        if (literal("true"))
            return out.kind = Json::Bool, out.number = 1.0, true;
        if (literal("false"))
            return out.kind = Json::Bool, true;
        if (literal("null"))
            return true;
        char *stop = nullptr;
        const std::string number(at_, static_cast<size_t>(std::min<ptrdiff_t>(end_ - at_, 64)));
        out.number = std::strtod(number.c_str(), &stop);
        if (stop == number.c_str())
            return false;
        out.kind = Json::Number;
        at_ += stop - number.c_str();
        return true;
    }
    const char *at_, *end_;
};

// --- The audio in it, as base64.

int base64Digit(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1; // padding, line ends: skipped
}

std::vector<uint8_t> base64Bytes(const std::string &text) {
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int have = 0;
    for (char c : text) {
        const int v = base64Digit(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        if ((have += 6) >= 8) {
            have -= 8;
            bytes.push_back(static_cast<uint8_t>(acc >> have));
        }
    }
    return bytes;
}

// Its samples, stored as 32-bit floats or as 16-bit integers.
std::vector<float> samplesOf(const std::string &text, bool pcm) {
    const std::vector<uint8_t> bytes = base64Bytes(text);
    std::vector<float> out(bytes.size() / (pcm ? 2 : 4));
    for (size_t i = 0; i < out.size(); ++i) {
        if (pcm) {
            int16_t v;
            std::memcpy(&v, &bytes[i * 2], 2);
            out[i] = static_cast<float>(v) / 32768.0f;
        } else {
            std::memcpy(&out[i], &bytes[i * 4], 4);
            if (!std::isfinite(out[i]))
                out[i] = 0.0f;
        }
    }
    return out;
}

// --- The components.

enum class Kind { None, Wave, Line, File, Phase, Window, Filter, Slew, Fold, Warp };

// Files of Vital's first versions number the types, later ones name them.
Kind kindOf(const Json &component) {
    const Json *type = component.find("type");
    if (!type)
        return Kind::None;
    if (type->kind == Json::Number) {
        constexpr Kind kNumbered[] = {Kind::Wave,   Kind::Line,   Kind::File, Kind::Phase, Kind::Window,
                                      Kind::Filter, Kind::Slew,   Kind::Fold, Kind::Warp};
        const int n = static_cast<int>(type->number);
        return n >= 0 && n < static_cast<int>(std::size(kNumbered)) ? kNumbered[n] : Kind::None;
    }
    const std::string &name = type->text;
    return name == "Wave Source" || name == "Shepard Tone Source" ? Kind::Wave
           : name == "Line Source"                                ? Kind::Line
           : name == "Audio File Source"                          ? Kind::File
           : name == "Phase Shift"                                ? Kind::Phase
           : name == "Wave Window"                                ? Kind::Window
           : name == "Frequency Filter"                           ? Kind::Filter
           : name == "Slew Limiter"                               ? Kind::Slew
           : name == "Wave Folder"                                ? Kind::Fold
           : name == "Wave Warp"                                  ? Kind::Warp
                                                                  : Kind::None;
}

struct LinePoint {
    double x = 0.0, y = 0.0, power = 0.0;
};

struct Key {
    int position = 0;
    const Json *json = nullptr;
    std::vector<float> wave; // a wave source's
    Spectrum spectrum;       // of its wave, where keyframes are blended by their spectra
    std::vector<LinePoint> points; // a line source's: y from 0 at the top to 1 at the bottom
    bool smooth = false;
    double pull = 0.0;
};

struct Component {
    Kind kind = Kind::None;
    const Json *json = nullptr;
    std::vector<Key> keys; // by position
    int interpolation = 1; // between keyframes: 0 none (each holds until the next), else blended
    bool bySpectrum = true; // a wave source's keyframes blended by their spectra, not their samples
    std::vector<float> audio; // an audio file source's
};

// A curve from 0 to 1, straight at power 0, bending one way or the other as the power does.
double powerScale(double x, double power) {
    return std::abs(power) < 0.01 ? x : (std::exp(power * x) - 1.0) / (std::exp(power) - 1.0);
}

double signedPowerScale(double x, double power) {
    return x < 0.0 ? -powerScale(-x, power) : powerScale(x, power);
}

// A keyframe's line. A file from before 0.7.7 lists its points between -1 and 1 within the cycle, the line running on
// across the cycle's end from its last point to its first; a later one has them from the cycle's start to its end.
void loadLine(const Json &key, Key &out) {
    out.pull = key.num("pull_power", 0.0);
    const Json *line = key.find("line");
    const Json &holder = line && line->kind == Json::Object ? *line : key;
    const Json *points = holder.find("points"), *powers = holder.find("powers");
    if (!points || points->kind != Json::Array)
        return;
    const size_t count = points->items.size() / 2;
    auto powerAt = [&](size_t i) {
        return powers && i < powers->items.size() ? powers->items[i].number : 0.0;
    };
    for (size_t i = 0; i < count; ++i)
        out.points.push_back({points->items[i * 2].number, points->items[i * 2 + 1].number, powerAt(i)});
    out.smooth = holder.num("smooth", 0.0) != 0.0;
    if (holder.find("line") || line || out.points.empty())
        return;
    const LinePoint first = out.points.front(), last = out.points.back();
    const double span = first.x + 1.0 - last.x;
    const double edge = span < 0.001 ? 0.5 * (first.y + last.y) : last.y + (first.y - last.y) * (1.0 - last.x) / span;
    out.points.insert(out.points.begin(), {0.0, edge, 0.0});
    out.points.push_back({1.0, edge, 0.0});
    for (LinePoint &p : out.points)
        p.y = 0.5 * p.y + 0.5;
}

// `decode` false leaves the audio out: what a scan needs. What a file's version says of how it stores its audio: 16-bit
// wave sources from 0.3.7 up to 0.3.9, 16-bit audio files from 0.3.7.
Component loadComponent(const Json &json, int version, bool decode) {
    Component c;
    c.kind = kindOf(json);
    c.json = &json;
    c.interpolation = static_cast<int>(json.num("interpolation_style", 1.0));
    c.bySpectrum = json.num("interpolation", 1.0) != 0.0;
    const Json *keyframes = json.find("keyframes");
    if (c.kind == Kind::None || !keyframes || keyframes->kind != Json::Array)
        return c;
    if (decode && c.kind == Kind::File)
        c.audio = samplesOf(json.str("audio_file"), version >= 307);
    for (const Json &k : keyframes->items) {
        Key key;
        key.position = static_cast<int>(std::clamp(k.num("position", 0.0), 0.0, kMostFrames - 1.0));
        key.json = &k;
        if (c.kind == Kind::Wave) {
            const std::string &data = k.find("wave_data") ? k.find("wave_data")->text : std::string();
            if (decode) {
                key.wave = samplesOf(data, version >= 307 && version < 309);
                if (static_cast<int>(key.wave.size()) < kSize)
                    continue;
                key.wave.resize(kSize);
                if (c.bySpectrum)
                    key.spectrum = harmonicsOf(key.wave.data(), kSize, kTop);
            } else if (data.empty()) {
                continue;
            }
        } else if (c.kind == Kind::Line) {
            loadLine(k, key);
            if (key.points.empty())
                continue;
        }
        c.keys.push_back(std::move(key));
    }
    std::stable_sort(c.keys.begin(), c.keys.end(), [](const Key &a, const Key &b) { return a.position < b.position; });
    return c;
}

// The keyframes about a position, and how far it is from the first to the second.
struct Span {
    const Key *from, *to;
    double t;
    double at(const char *field, double fallback) const {
        const double a = from->json->num(field, fallback);
        return a + (to->json->num(field, fallback) - a) * t;
    }
};

Span spanAt(const Component &c, int position) {
    const std::vector<Key> &keys = c.keys;
    size_t after = 0;
    while (after < keys.size() && keys[after].position <= position)
        ++after;
    if (after == 0)
        return {&keys[0], &keys[0], 0.0};
    const Key *const from = &keys[after - 1];
    if (after == keys.size() || c.interpolation == 0)
        return {from, from, 0.0};
    const Key *const to = &keys[after];
    return {from, to, static_cast<double>(position - from->position) / (to->position - from->position)};
}

// Two spectra blended: each harmonic's level on a square-root scale, its phase along the shorter way round.
Spectrum blendSpectra(const Spectrum &a, const Spectrum &b, double t) {
    Spectrum out(a.size());
    out[0] = a[0] + (b[0] - a[0]) * t;
    for (size_t k = 1; k < a.size(); ++k) {
        const double from = std::sqrt(std::abs(a[k])), to = std::sqrt(std::abs(b[k]));
        const double level = from + (to - from) * t;
        out[k] = std::polar(level * level, std::arg(a[k]) + std::arg(std::conj(a[k]) * b[k]) * t);
    }
    return out;
}

void frameOf(const Spectrum &spectrum, float *frame) {
    cycleOf(spectrum, kTop, kSize, frame);
    const float dc = static_cast<float>(spectrum[0].real());
    for (int i = 0; i < kSize; ++i)
        frame[i] += dc;
}

// The frame's spectrum changed by `change`.
template <class Change>
void reshape(float *frame, Change change) {
    Spectrum spectrum = harmonicsOf(frame, kSize, kTop);
    change(spectrum);
    frameOf(spectrum, frame);
}

float peakOf(const float *frame) {
    float peak = 0.0f;
    for (int i = 0; i < kSize; ++i)
        peak = std::max(peak, std::abs(frame[i]));
    return peak;
}

// To a peak of 1; only down, without `lift`.
void normalizeFrame(float *frame, bool lift) {
    const float peak = peakOf(frame);
    if (peak > 0.0f && (lift || peak > 1.0f))
        for (int i = 0; i < kSize; ++i)
            frame[i] /= peak;
}

void renderLine(const Span &span, float *frame) {
    const std::vector<LinePoint> &from = span.from->points, &to = span.to->points;
    std::vector<LinePoint> points = from;
    if (span.t > 0.0 && from.size() == to.size()) {
        const double t = powerScale(span.t, span.from->pull - span.to->pull);
        for (size_t p = 0; p < points.size(); ++p) {
            points[p].x += (to[p].x - from[p].x) * t;
            points[p].y += (to[p].y - from[p].y) * t;
            points[p].power += (to[p].power - from[p].power) * t;
        }
    }
    size_t at = 0;
    for (int i = 0; i < kSize; ++i) {
        const double x = static_cast<double>(i) / (kSize - 1);
        while (at + 2 < points.size() && x > points[at + 1].x)
            ++at;
        const LinePoint &a = points[at], &b = points[std::min(at + 1, points.size() - 1)];
        double y = b.y;
        if (b.x > a.x) {
            double t = std::clamp((x - a.x) / (b.x - a.x), 0.0, 1.0);
            if (span.from->smooth)
                t = 0.5 - 0.5 * std::cos(kPi * t);
            y = a.y + (b.y - a.y) * std::clamp(powerScale(t, a.power), 0.0, 1.0);
        }
        frame[i] = static_cast<float>(1.0 - 2.0 * y);
    }
}

void renderFile(const Component &c, const Span &span, float *frame) {
    std::fill_n(frame, kSize, 0.0f);
    const std::vector<float> &audio = c.audio;
    if (audio.empty())
        return;
    auto read = [&audio](double at) { // along four points' curve
        const double place = std::clamp(at, 0.0, static_cast<double>(audio.size() - 1));
        const auto i = static_cast<int64_t>(place);
        const auto last = static_cast<int64_t>(audio.size()) - 1;
        const double t = place - static_cast<double>(i);
        const double a = audio[std::max<int64_t>(i - 1, 0)], b = audio[i], d = audio[std::min(i + 1, last)],
                     e = audio[std::min(i + 2, last)];
        return static_cast<float>(b + 0.5 * t * (d - a + t * (2.0 * a - 5.0 * b + 4.0 * d - e + t * (3.0 * (b - d) + e - a))));
    };
    const double start = std::max(span.at("start_position", 0.0), 0.0);
    const double window = std::max(c.json->num("window_size", kSize), 1.0);
    const double step = window / kSize;
    auto cycle = [&](double from, float *to) {
        for (int i = 0; i < kSize; ++i)
            to[i] = read(from + i * step);
    };
    const double cycles = start / window, whole = std::floor(cycles), part = cycles - whole;
    const int fadeStyle = static_cast<int>(c.json->num("fade_style", 0.0));
    if (fadeStyle == 1) { // a whole cycle at a time
        cycle(whole * window, frame);
    } else if (fadeStyle == 2 || fadeStyle == 3) { // between this cycle and the next, by samples or by spectra
        std::vector<float> next(kSize);
        cycle(whole * window, frame);
        cycle((whole + 1.0) * window, next.data());
        if (fadeStyle == 2) {
            for (int i = 0; i < kSize; ++i)
                frame[i] += (next[i] - frame[i]) * static_cast<float>(part);
        } else {
            frameOf(blendSpectra(harmonicsOf(frame, kSize, kTop), harmonicsOf(next.data(), kSize, kTop), part), frame);
        }
    } else { // the window from where it is, faded at its start into the window after it, so it loops
        const auto offset = static_cast<int64_t>(start / step + 0.5 * window + 0.5 * kSize) % kSize;
        for (int i = 0; i < kSize; ++i)
            frame[(offset + i) % kSize] = read(start + i * step);
        const int fades = static_cast<int>(std::clamp(span.at("window_fade", 1.0), 0.0, 1.0) * kSize);
        for (int i = 0; i < fades; ++i) {
            const double t = fades > 1 ? static_cast<double>(i) / (fades - 1) : 0.0;
            float &out = frame[(offset + i) % kSize];
            out += (read(start + window + t * fades * step) - out) * static_cast<float>(0.5 + 0.5 * std::cos(kPi * t));
        }
    }
    const int phaseStyle = static_cast<int>(c.json->num("phase_style", 0.0));
    if (phaseStyle == 1) { // each harmonic's phase set aside
        reshape(frame, [](Spectrum &s) {
            for (size_t k = 1; k < s.size(); ++k)
                s[k] = std::polar(std::abs(s[k]), k % 2 ? kPi / 2.0 : -kPi / 2.0);
        });
    } else if (phaseStyle == 2) { // and a random one in its place, the same for every frame
        reshape(frame, [&c](Spectrum &s) {
            auto seed = static_cast<uint32_t>(static_cast<int64_t>(c.json->num("random_seed", 0.0)));
            for (size_t k = 1; k < s.size(); ++k) {
                seed = seed * 1664525u + 1013904223u;
                s[k] = std::polar(std::abs(s[k]), (seed >> 8) * (2.0 * kPi / 16777216.0));
            }
        });
    }
    if (c.json->num("normalize_mult", 1.0) != 0.0)
        normalizeFrame(frame, c.json->num("normalize_gain", 0.0) != 0.0);
}

void renderPhase(const Component &c, const Span &span, float *frame) {
    const double phase = span.at("phase", 0.0), mix = span.at("mix", 1.0);
    const int style = static_cast<int>(c.json->num("style", 0.0));
    reshape(frame, [&](Spectrum &s) {
        for (size_t k = 1; k < s.size(); ++k) {
            const double n = static_cast<double>(k);
            const bool even = k % 2 == 0;
            std::complex<double> to;
            switch (style) {
            case 1: to = s[k] * std::polar(1.0, even ? -phase * n : phase * (n - 1.0)); break; // even and odd apart
            case 2: to = s[k] * std::polar(1.0, -phase); break;                                 // every harmonic alike
            case 3: to = s[k] * std::polar(1.0, even ? -phase : phase); break;                  // even against odd
            case 4: to = std::abs(s[k]); break;                                                 // its phase cleared
            default: to = s[k] * std::polar(1.0, -phase * n); break;                            // along the spectrum
            }
            s[k] = to * mix + s[k] * (1.0 - mix);
        }
    });
}

void renderWindow(const Component &c, const Span &span, float *frame) {
    const double left = span.at("left_position", 0.25), right = span.at("right_position", 0.75);
    const int shape = static_cast<int>(c.json->num("window_shape", 0.0));
    auto window = [shape](double t) {
        switch (shape) {
        case 0: return 0.5 - 0.5 * std::cos(kPi * t);
        case 1: return std::sin(0.5 * kPi * t);
        case 3: return t < 1.0 ? 0.0 : 1.0;
        case 4: return t * std::cos(kPi * (1.5 * t + 0.5));
        default: return t;
        }
    };
    for (int i = 0; i < kSize; ++i) {
        const double t = static_cast<double>(i) / (kSize - 1);
        if (t < left)
            frame[i] *= static_cast<float>(window(t / left));
        else if (t > right)
            frame[i] *= static_cast<float>(window((1.0 - t) / (1.0 - right)));
    }
}

void renderFilter(const Component &c, const Span &span, float *frame) {
    const double cutoff = std::exp2(span.at("cutoff", 4.0)), shape = span.at("shape", 0.5);
    const double slope = 1.0 / (1.0 + 127.0 * shape * shape), power = -9.0 + 18.0 * shape;
    const int style = static_cast<int>(c.json->num("style", 0.0));
    reshape(frame, [&](Spectrum &s) {
        for (size_t k = 1; k < s.size(); ++k) {
            const double delta = static_cast<double>(k) - cutoff;
            double gain;
            switch (style) {
            case 1: gain = 1.0 - std::abs(slope * delta); break;
            case 2: gain = 1.0 + slope * delta; break;
            case 3: { // teeth at the cutoff's odd multiples
                const double along = static_cast<double>(k) / (2.0 * cutoff);
                const double part = along - std::floor(along);
                gain = powerScale(2.0 * std::min(part, 1.0 - part), power);
                break;
            }
            default: gain = 1.0 - slope * delta; break;
            }
            s[k] *= std::clamp(gain, 0.0, 1.0);
        }
    });
    if (c.json->num("normalize", 1.0) != 0.0)
        normalizeFrame(frame, true);
}

void renderSlew(const Span &span, float *frame) {
    const double least = 1.0 / kSize;
    const auto up = static_cast<float>(2.0 / kSize / std::max(span.at("up_run_rise", 0.0), least));
    const auto down = static_cast<float>(2.0 / kSize / std::max(span.at("down_run_rise", 0.0), least));
    const std::vector<float> target(frame, frame + kSize);
    float now = frame[0];
    for (int i = 1; i <= 2 * kSize; ++i) { // round twice, so the cycle's end meets its start
        const int at = i % kSize;
        now += std::clamp(target[at] - now, -down, up);
        frame[at] = now;
    }
}

void renderFold(const Span &span, float *frame) {
    const double boost = span.at("fold_boost", 1.0);
    const double scale = std::max<double>(peakOf(frame), 1.0);
    for (int i = 0; i < kSize; ++i)
        frame[i] = static_cast<float>(std::sin(std::asin(std::clamp(frame[i] / scale, -1.0, 1.0)) * scale * boost));
}

void renderWarp(const Component &c, const Span &span, float *frame) {
    const double across = span.at("horizontal_power", 0.0), upDown = span.at("vertical_power", 0.0);
    const bool acrossOneWay = c.json->num("horizontal_asymmetric", 0.0) != 0.0;
    const bool upDownOneWay = c.json->num("vertical_asymmetric", 0.0) != 0.0;
    const std::vector<float> wave(frame, frame + kSize);
    for (int i = 0; i < kSize; ++i) {
        double x = static_cast<double>(i) / (kSize - 1);
        x = acrossOneWay ? powerScale(x, across) : 0.5 * signedPowerScale(2.0 * x - 1.0, across) + 0.5;
        const double place = std::clamp(x, 0.0, 1.0) * (kSize - 1);
        const int at = std::min(static_cast<int>(place), kSize - 2);
        const double v = std::clamp(wave[at] + (wave[at + 1] - wave[at]) * (place - at), -1.0, 1.0);
        frame[i] = static_cast<float>(upDownOneWay ? 2.0 * powerScale(0.5 * v + 0.5, upDown) - 1.0
                                                   : signedPowerScale(v, upDown));
    }
}

void render(const Component &c, int position, float *frame) {
    const Span span = spanAt(c, position);
    switch (c.kind) {
    case Kind::Wave:
        if (span.t <= 0.0) {
            std::copy_n(span.from->wave.data(), kSize, frame);
        } else if (c.bySpectrum) {
            frameOf(blendSpectra(span.from->spectrum, span.to->spectrum, span.t), frame);
        } else {
            for (int i = 0; i < kSize; ++i)
                frame[i] = span.from->wave[i] + (span.to->wave[i] - span.from->wave[i]) * static_cast<float>(span.t);
        }
        break;
    case Kind::Line: renderLine(span, frame); break;
    case Kind::File: renderFile(c, span, frame); break;
    case Kind::Phase: renderPhase(c, span, frame); break;
    case Kind::Window: renderWindow(c, span, frame); break;
    case Kind::Filter: renderFilter(c, span, frame); break;
    case Kind::Slew: renderSlew(span, frame); break;
    case Kind::Fold: renderFold(span, frame); break;
    case Kind::Warp: renderWarp(c, span, frame); break;
    case Kind::None: break;
    }
}

// The file's groups of components, and how many frames they make.
struct Table {
    std::vector<std::vector<Component>> groups;
    int count = 0;
};

ReadError loadTable(const Json &root, bool decode, Table &table) {
    const Json *groups = root.find("groups");
    if (!groups || groups->kind != Json::Array)
        return root.find("settings") ? ReadError::PresetOnly : ReadError::NotAudio;
    // Its version as one number, 0.3.7 as 307; none, as in Vital's first files, is 0.
    const std::string written = root.str("version");
    int version = 0;
    for (const char *at = written.c_str(), *const end = at + written.size(); at < end;) {
        char *stop = nullptr;
        version = version * 100 + static_cast<int>(std::strtol(at, &stop, 10));
        at = stop == at ? end : stop + 1;
    }
    bool source = false;
    int last = 0;
    for (const Json &group : groups->items) {
        const Json *components = group.find("components");
        if (!components || components->kind != Json::Array)
            continue;
        std::vector<Component> chain;
        for (const Json &json : components->items) {
            Component component = loadComponent(json, version, decode);
            if (component.keys.empty())
                continue;
            source = source || component.kind == Kind::Wave || component.kind == Kind::Line || component.kind == Kind::File;
            last = std::max(last, component.keys.back().position);
            chain.push_back(std::move(component));
        }
        if (!chain.empty())
            table.groups.push_back(std::move(chain));
    }
    if (!source)
        return ReadError::NotAudio;
    table.count = std::min(last + 1, kMostFrames);
    return ReadError::None;
}

bool parseJson(const uint8_t *bytes, size_t size, Json &root) {
    const auto *text = reinterpret_cast<const char *>(bytes);
    return JsonParser(text, text + size).parse(root) && root.kind == Json::Object;
}

} // namespace

ReadError readVitalTable(const uint8_t *bytes, size_t size, std::vector<float> &frames, int &count) {
    Json root;
    if (!parseJson(bytes, size, root))
        return ReadError::Broken;
    Table table;
    if (const ReadError error = loadTable(root, true, table); error != ReadError::None)
        return error;
    count = table.count;
    frames.assign(static_cast<size_t>(count) * kSize, 0.0f);
    std::vector<float> frame(kSize);
    for (int position = 0; position < count; ++position) {
        float *const out = &frames[static_cast<size_t>(position) * kSize];
        for (const std::vector<Component> &chain : table.groups) {
            std::fill(frame.begin(), frame.end(), 0.0f);
            for (const Component &component : chain)
                render(component, position, frame.data());
            for (int i = 0; i < kSize; ++i)
                out[i] += frame[i] / static_cast<float>(table.groups.size());
        }
    }
    return ReadError::None;
}

ReadError probeVitalTable(const uint8_t *bytes, size_t size, int &count) {
    Json root;
    if (!parseJson(bytes, size, root))
        return ReadError::Broken;
    Table table;
    const ReadError error = loadTable(root, false, table);
    count = table.count;
    return error;
}

} // namespace fmvibe
