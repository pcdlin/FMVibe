#include "readers.h"

#include "../fft.h"
#include "../opdata.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>

namespace fmvibe {

Str readErrorText(ReadError error) {
    switch (error) {
    case ReadError::NotAudio: return Str::ReadNotAudio;
    case ReadError::Broken: return Str::ReadBroken;
    case ReadError::Unsupported: return Str::ReadUnsupported;
    case ReadError::Encrypted: return Str::ReadEncrypted;
    case ReadError::PresetOnly: return Str::ReadPresetOnly;
    case ReadError::LineSource: return Str::ReadLineSource;
    case ReadError::Empty: return Str::ReadEmpty;
    case ReadError::TooLong: return Str::ReadTooLong;
    case ReadError::NotTable: return Str::ReadNotTable;
    case ReadError::None: break;
    }
    return Str::ReadNotAudio;
}

int guessFrameSize(int64_t length, int folder) {
    if (length <= 0)
        return 0;
    if (folder > 0)
        return folder;
    if (length % 2048 == 0)
        return 2048;
    for (int size : {1024, 512, 256, 4096, 128})
        if (length % size == 0 && length / size >= 2 && length / size <= FrameTable::kMaxFrames)
            return size;
    return length <= 4096 ? static_cast<int>(length) : 0;
}

namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string &s, const char *tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

uint32_t le32(const uint8_t *p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint64_t le64(const uint8_t *p) { return uint64_t{le32(p)} | uint64_t{le32(p + 4)} << 32; }

// What a name alone says can't be read: other plugins' own formats.
ReadError knownUnreadable(const std::string &name) {
    const std::string n = lower(name);
    for (const char *tail : {".nmsv", ".nmsx", ".nmswt", ".ncw", ".nki", ".nmsx2"})
        if (endsWith(n, tail))
            return ReadError::Encrypted;
    for (const char *tail : {".fxp", ".fxb", ".serumpreset", ".vital", ".vitalbank", ".phaseplant"})
        if (endsWith(n, tail))
            return ReadError::PresetOnly;
    return ReadError::None;
}

// --- WAV: its chunks, read through `read`.

struct WavInfo {
    ReadError error = ReadError::None;
    int format = 0; // 1 PCM, 3 float
    int channels = 0, bits = 0;
    uint32_t rate = 0;
    uint64_t dataAt = 0, dataSize = 0;
    bool hasData = false;
    int clm = 0, srge = 0;
    bool uhwt = false, srgo = false;
    std::vector<uint32_t> cues;
    bool smpl = false;
    float root = 60.0f;
    uint32_t loopStart = 0, loopEnd = 0;
    bool smplLoop = false;
    int64_t length() const {
        const int width = bits / 8 * channels;
        return width > 0 ? static_cast<int64_t>(dataSize / static_cast<uint64_t>(width)) : 0;
    }
};

WavInfo parseWav(const ReadAt &read, uint64_t size) {
    WavInfo w;
    uint8_t head[12];
    if (size < 12 || read(0, head, 12) != 12)
        return w.error = ReadError::NotAudio, w;
    const bool rf64 = std::memcmp(head, "RF64", 4) == 0;
    if ((std::memcmp(head, "RIFF", 4) != 0 && !rf64) || std::memcmp(head + 8, "WAVE", 4) != 0)
        return w.error = ReadError::NotAudio, w;
    uint64_t at = 12, rf64Data = 0;
    bool fmt = false;
    for (int chunks = 0; at + 8 <= size && chunks < 4096; ++chunks) {
        uint8_t ch[8];
        if (read(at, ch, 8) != 8)
            break;
        uint64_t length = le32(ch + 4);
        const uint64_t body = at + 8;
        auto id = [&](const char *name) { return std::memcmp(ch, name, 4) == 0; };
        if (id("data") && rf64 && length == 0xFFFFFFFFu)
            length = rf64Data;
        if (body + length > size && !id("data"))
            break; // a chunk past the file's end: what came before stands
        uint8_t buf[64] = {};
        const size_t small = static_cast<size_t>(std::min<uint64_t>(length, sizeof(buf)));
        if (id("ds64")) {
            if (read(body, buf, std::min<size_t>(small, 28)) >= 16)
                rf64Data = le64(buf + 8);
        } else if (id("fmt ")) {
            if (read(body, buf, small) < 16)
                return w.error = ReadError::Broken, w;
            const uint16_t tag = le16(buf);
            w.channels = le16(buf + 2);
            w.rate = le32(buf + 4);
            w.bits = le16(buf + 14);
            w.format = tag == 0xFFFE && small >= 26 ? le16(buf + 24) : tag;
            fmt = true;
        } else if (id("data")) {
            w.dataAt = body;
            w.dataSize = std::min(length, size - body);
            w.hasData = true;
        } else if (id("clm ")) { // Serum: "<!>2048 ..."
            const size_t n = read(body, buf, small);
            const std::string text(reinterpret_cast<const char *>(buf), n);
            const size_t mark = text.find("<!>");
            if (mark != std::string::npos)
                w.clm = std::atoi(text.c_str() + mark + 3);
        } else if (id("uhWT")) {
            w.uhwt = true;
        } else if (id("srge") || id("srgo")) {
            if (read(body, buf, std::min<size_t>(small, 8)) == 8)
                w.srge = static_cast<int>(le32(buf + 4));
            w.srgo = id("srgo");
        } else if (id("cue ")) {
            uint8_t count[4];
            if (read(body, count, 4) == 4) {
                const uint32_t n = std::min<uint32_t>(le32(count), 1024);
                for (uint32_t k = 0; k < n; ++k) {
                    uint8_t point[24];
                    if (read(body + 4 + 24 * uint64_t{k}, point, 24) != 24)
                        break;
                    w.cues.push_back(le32(point + 20));
                }
            }
        } else if (id("smpl")) {
            const size_t n = read(body, buf, small);
            if (n >= 36) {
                w.smpl = true;
                const uint32_t unity = le32(buf + 12), fraction = le32(buf + 16);
                w.root = static_cast<float>(std::min<uint32_t>(unity, 127)) + static_cast<float>(fraction / 4294967296.0);
                if (le32(buf + 28) > 0 && n >= 60) {
                    w.smplLoop = true;
                    w.loopStart = le32(buf + 44);
                    w.loopEnd = le32(buf + 48) + 1;
                }
            }
        }
        at = body + length + (length & 1);
    }
    if (!fmt || !w.hasData)
        return w.error = ReadError::Broken, w;
    const bool pcm = w.format == 1 && (w.bits == 8 || w.bits == 16 || w.bits == 24 || w.bits == 32);
    const bool real = w.format == 3 && (w.bits == 32 || w.bits == 64);
    if (!pcm && !real)
        return w.error = ReadError::Unsupported, w;
    if (w.channels < 1 || w.channels > 8 || w.rate < 1000 || w.rate > 768000)
        return w.error = ReadError::Unsupported, w;
    if (w.length() == 0)
        return w.error = ReadError::Empty, w;
    return w;
}

std::vector<float> wavSamples(const uint8_t *bytes, const WavInfo &w) {
    const int64_t length = w.length();
    const int width = w.bits / 8;
    std::vector<float> out(static_cast<size_t>(length));
    const uint8_t *data = bytes + w.dataAt;
    for (int64_t i = 0; i < length; ++i) {
        double sum = 0.0;
        for (int c = 0; c < w.channels; ++c) {
            const uint8_t *p = data + (i * w.channels + c) * width;
            double x = 0.0;
            if (w.format == 3) {
                if (w.bits == 32) {
                    float f;
                    std::memcpy(&f, p, 4);
                    x = f;
                } else {
                    double d;
                    std::memcpy(&d, p, 8);
                    x = d;
                }
            } else if (w.bits == 8) {
                x = (p[0] - 128) / 128.0;
            } else if (w.bits == 16) {
                x = static_cast<int16_t>(le16(p)) / 32768.0;
            } else if (w.bits == 24) {
                const int32_t v = static_cast<int32_t>(uint32_t{p[0]} << 8 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 24) >> 8;
                x = v / 8388608.0;
            } else {
                x = static_cast<int32_t>(le32(p)) / 2147483648.0;
            }
            sum += std::isfinite(x) ? x : 0.0;
        }
        out[i] = static_cast<float>(sum);
    }
    return out;
}

// The frame size a WAV's chunks give, in the order the plan has them; 0 for none.
int wavFrameSize(const WavInfo &w, std::string &tag) {
    if (w.clm > 0)
        return tag = "clm", w.clm;
    if (w.uhwt)
        return tag = "uhWT", 2048;
    if (w.srge > 0)
        return tag = "srge", w.srge;
    if (w.cues.size() >= 2) {
        std::vector<uint32_t> cues = w.cues;
        std::sort(cues.begin(), cues.end());
        if (cues[1] > cues[0])
            return tag = "cue", static_cast<int>(cues[1] - cues[0]);
    }
    if (w.smpl)
        return tag = "smpl", w.smplLoop && w.loopEnd > w.loopStart ? static_cast<int>(w.loopEnd - w.loopStart) : 2048;
    return 0;
}

// --- Surge's and Bitwig's .wt.

struct WtHeader {
    bool ok = false;
    uint32_t size = 0;
    uint16_t count = 0, flags = 0;
};

WtHeader wtHeader(const uint8_t *head, uint64_t fileSize) {
    WtHeader h;
    if (fileSize < 12 || std::memcmp(head, "vawt", 4) != 0)
        return h;
    h.size = le32(head + 4);
    h.count = le16(head + 8);
    h.flags = le16(head + 10);
    h.ok = h.size >= 2 && h.size <= 4096 && (h.size & (h.size - 1)) == 0 && h.count >= 1 && h.count <= 512;
    return h;
}

// --- Vital's .vitaltable: JSON, with base64 floats.

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
        return j && j->kind == Number ? j->number : fallback;
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
            } else {
                out += *at_++;
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

std::vector<float> base64Floats(const std::string &text) {
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * 3 / 4);
    uint32_t acc = 0;
    int have = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+' || c == '-')
            v = 62;
        else if (c == '/' || c == '_')
            v = 63;
        else
            continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        if ((have += 6) >= 8) {
            have -= 8;
            bytes.push_back(static_cast<uint8_t>(acc >> have));
        }
    }
    std::vector<float> out(bytes.size() / 4);
    for (size_t i = 0; i < out.size(); ++i) {
        std::memcpy(&out[i], &bytes[i * 4], 4);
        if (!std::isfinite(out[i]))
            out[i] = 0.0f;
    }
    return out;
}

// A .vitaltable's first group's first source FMVibe reads, as frames of `size` (out), keyframes filled in between.
ReadError vitalFrames(const Json &root, std::vector<float> &frames, int &count, int &size) {
    const Json *groups = root.find("groups");
    if (!groups || groups->kind != Json::Array)
        return root.find("settings") ? ReadError::PresetOnly : ReadError::NotAudio;
    bool lines = false;
    for (const Json &group : groups->items) {
        const Json *components = group.find("components");
        if (!components || components->kind != Json::Array)
            continue;
        for (const Json &c : components->items) {
            const std::string type = c.str("type");
            const Json *keyframes = c.find("keyframes");
            if (type == "Line Source") {
                lines = true;
                continue;
            }
            if (!keyframes || keyframes->kind != Json::Array || keyframes->items.empty())
                continue;
            // Each keyframe's frame and its place (0 to 256).
            std::vector<std::pair<int, std::vector<float>>> keys;
            if (type == "Wave Source") {
                size = FrameTable::kFrameSize;
                for (const Json &k : keyframes->items) {
                    std::vector<float> wave = base64Floats(k.str("wave_data"));
                    if (static_cast<int>(wave.size()) >= size) {
                        wave.resize(static_cast<size_t>(size));
                        keys.push_back({static_cast<int>(std::clamp(k.num("position", 0.0), 0.0, 256.0)), std::move(wave)});
                    }
                }
            } else if (type == "Audio File Source") {
                const std::vector<float> audio = base64Floats(c.str("audio_file"));
                size = static_cast<int>(std::clamp(c.num("window_size", 2048.0), 16.0, 4096.0));
                for (const Json &k : keyframes->items) {
                    const auto start = static_cast<size_t>(std::max(0.0, k.num("start_position", 0.0)));
                    if (start + static_cast<size_t>(size) <= audio.size())
                        keys.push_back({static_cast<int>(std::clamp(k.num("position", 0.0), 0.0, 256.0)),
                                        std::vector<float>(audio.begin() + static_cast<ptrdiff_t>(start),
                                                           audio.begin() + static_cast<ptrdiff_t>(start) + size)});
                }
            } else {
                continue;
            }
            if (keys.empty())
                continue;
            std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            count = keys.back().first + 1;
            frames.assign(static_cast<size_t>(count) * size, 0.0f);
            for (int f = 0; f < count; ++f) {
                size_t k = 0;
                while (k + 1 < keys.size() && keys[k + 1].first <= f)
                    ++k;
                const auto &a = keys[k];
                const auto &b = k + 1 < keys.size() ? keys[k + 1] : keys[k];
                const float t = b.first > a.first ? static_cast<float>(f - a.first) / static_cast<float>(b.first - a.first) : 0.0f;
                for (int i = 0; i < size; ++i)
                    frames[static_cast<size_t>(f) * size + i] =
                        f < a.first ? a.second[i] : a.second[i] + (b.second[i] - a.second[i]) * std::clamp(t, 0.0f, 1.0f);
            }
            return ReadError::None;
        }
    }
    return lines ? ReadError::LineSource : ReadError::NotAudio;
}

// A cycle's harmonics 1..top as harmonicsOf() gives them, for any length: by the FFT for a power of two, else straight.
std::vector<std::complex<double>> anyHarmonics(const float *x, int n, int top) {
    if ((n & (n - 1)) == 0)
        return harmonicsOf(x, static_cast<size_t>(n), top);
    std::vector<std::complex<double>> out(static_cast<size_t>(top) + 1);
    for (int k = 1; k <= top; ++k) {
        std::complex<double> sum = 0.0;
        for (int i = 0; i < n; ++i)
            sum += static_cast<double>(x[i]) * std::polar(1.0, -2.0 * std::numbers::pi * k * i / n);
        out[k] = sum * (2.0 / n);
    }
    return out;
}

TableFile asSample(std::vector<float> samples, uint32_t rate, bool sixteen, const std::string &format) {
    TableFile out;
    out.sample = true;
    out.format = format;
    out.rate = rate;
    out.sixteenBit = sixteen;
    if (static_cast<int64_t>(samples.size()) > kMaxSampleLength)
        return out.error = ReadError::TooLong, out;
    float peak = 0.0f;
    for (float x : samples)
        peak = std::max(peak, std::abs(x));
    if (samples.empty() || peak <= 0.0f)
        return out.error = ReadError::Empty, out;
    for (float &x : samples)
        x /= peak;
    out.frames = static_cast<int>(samples.size());
    out.data = std::move(samples);
    return out;
}

TableFile asTable(const std::vector<float> &samples, int size, bool sixteen, const std::string &format, bool guessed) {
    TableFile out;
    out.format = format;
    out.sixteenBit = sixteen;
    out.guessed = guessed;
    out.frameSize = size;
    const int count = size > 0 ? static_cast<int>(samples.size() / static_cast<size_t>(size)) : 0;
    out.fileFrames = count;
    if (count < 1)
        return out.error = ReadError::NotTable, out;
    out.data = normalizeFrames(samples.data(), count, size, out.frames);
    float peak = 0.0f;
    for (float x : out.data)
        peak = std::max(peak, std::abs(x));
    if (peak <= 0.0f)
        return out.error = ReadError::Empty, out;
    return out;
}

} // namespace

std::vector<float> normalizeFrames(const float *samples, int count, int size, int &frames) {
    constexpr int kSize = FrameTable::kFrameSize;
    frames = std::min(count, FrameTable::kMaxFrames);
    std::vector<float> out(static_cast<size_t>(frames) * kSize, 0.0f);
    if (frames <= 0 || size <= 0)
        return frames = 0, std::vector<float>{};
    for (int f = 0; f < frames; ++f) {
        // Past kMaxFrames, frames are picked evenly from first to last.
        const int from = frames == count ? f : static_cast<int>(std::lround(static_cast<double>(f) * (count - 1) / (frames - 1)));
        const float *x = samples + static_cast<size_t>(from) * size;
        float *to = &out[static_cast<size_t>(f) * kSize];
        if (size == kSize) {
            double dc = 0.0;
            for (int i = 0; i < size; ++i)
                dc += x[i];
            dc /= size;
            for (int i = 0; i < size; ++i)
                to[i] = static_cast<float>(x[i] - dc);
        } else { // through its spectrum, its DC left out: past 1024 harmonics (from 4096), the rest dropped
            const int top = std::min(size / 2 - 1 + (size % 2), kSize / 2 - 1);
            cycleOf(anyHarmonics(x, size, std::max(top, 1)), std::max(top, 1), kSize, to);
        }
    }
    float peak = 0.0f;
    for (float v : out)
        peak = std::max(peak, std::abs(v));
    if (peak > 0.0f)
        for (float &v : out)
            v /= peak;
    return out;
}

TableFile readTableFile(const uint8_t *bytes, size_t size, const std::string &name, ReadAs as, int frameSize) {
    TableFile out;
    const ReadAt read = [&](uint64_t at, void *to, size_t n) -> size_t {
        if (at >= size)
            return 0;
        const size_t got = static_cast<size_t>(std::min<uint64_t>(n, size - at));
        std::memcpy(to, bytes + at, got);
        return got;
    };
    if (size >= 12 && (std::memcmp(bytes, "RIFF", 4) == 0 || std::memcmp(bytes, "RF64", 4) == 0)) {
        const WavInfo w = parseWav(read, size);
        if (w.error != ReadError::None)
            return out.error = w.error, out;
        std::vector<float> samples = wavSamples(bytes, w);
        if (as == ReadAs::Sample || w.srgo) {
            out = asSample(std::move(samples), w.rate, w.bits <= 16 && w.format == 1, "wav");
            if (out.error == ReadError::None && w.smpl) {
                out.root = w.root;
                if (w.smplLoop && w.loopEnd > w.loopStart && w.loopEnd <= static_cast<uint32_t>(out.frames)) {
                    out.loopStart = w.loopStart;
                    out.loopEnd = w.loopEnd;
                }
            }
            return out;
        }
        std::string tag = "wav";
        int frame = wavFrameSize(w, tag);
        const bool guessed = frame <= 0;
        if (guessed)
            frame = guessFrameSize(static_cast<int64_t>(samples.size()), frameSize);
        if (frame <= 0 || frame > 4096)
            return out.error = ReadError::NotTable, out;
        return asTable(samples, frame, w.bits <= 16 && w.format == 1, guessed ? "wav" : tag, guessed);
    }
    if (size >= 4 && std::memcmp(bytes, "fLaC", 4) == 0) {
        std::vector<float> samples;
        uint32_t rate = 0;
        int bits = 0;
        if (!decodeFlac(bytes, size, samples, rate, bits))
            return out.error = ReadError::Broken, out;
        if (as == ReadAs::Sample)
            return asSample(std::move(samples), rate, bits <= 16, "flac");
        const int frame = guessFrameSize(static_cast<int64_t>(samples.size()), frameSize);
        if (frame <= 0 || frame > 4096)
            return out.error = ReadError::NotTable, out;
        return asTable(samples, frame, bits <= 16, "flac", frameSize <= 0);
    }
    if (const WtHeader h = wtHeader(bytes, size); h.ok) {
        const bool int16 = (h.flags & 0x04) != 0;
        const size_t count = size_t{h.size} * h.count;
        if (12 + count * (int16 ? 2 : 4) > size)
            return out.error = ReadError::Broken, out;
        std::vector<float> samples(count);
        const double scale = (h.flags & 0x08) ? 1.0 / 32768.0 : 1.0 / 16384.0;
        for (size_t i = 0; i < count; ++i) {
            if (int16) {
                samples[i] = static_cast<float>(static_cast<int16_t>(le16(bytes + 12 + 2 * i)) * scale);
            } else {
                std::memcpy(&samples[i], bytes + 12 + 4 * i, 4);
                if (!std::isfinite(samples[i]))
                    samples[i] = 0.0f;
            }
        }
        if (as == ReadAs::Sample || (h.flags & 0x01))
            return asSample(std::move(samples), 44100, int16, "wt");
        return asTable(samples, static_cast<int>(h.size), int16, "wt", false);
    }
    // JSON: a .vitaltable, or a Vital preset.
    size_t first = 0;
    while (first < size && std::isspace(bytes[first]))
        ++first;
    if (first < size && bytes[first] == '{') {
        Json root;
        JsonParser parser(reinterpret_cast<const char *>(bytes), reinterpret_cast<const char *>(bytes) + size);
        if (!parser.parse(root) || root.kind != Json::Object)
            return out.error = ReadError::Broken, out;
        std::vector<float> frames;
        int count = 0, frame = 0;
        if (const ReadError e = vitalFrames(root, frames, count, frame); e != ReadError::None)
            return out.error = e, out;
        if (as == ReadAs::Sample)
            return asSample(std::move(frames), 44100, false, "vital");
        return asTable(frames, frame, false, "vital", false);
    }
    if (size >= 4 && std::memcmp(bytes, "CcnK", 4) == 0)
        return out.error = ReadError::PresetOnly, out;
    const ReadError named = knownUnreadable(name);
    return out.error = named != ReadError::None ? named : ReadError::NotAudio, out;
}

TableInfo probeTableFile(const ReadAt &read, uint64_t size, const std::string &name, int frameSize) {
    TableInfo info;
    uint8_t head[64] = {};
    const size_t got = read(0, head, sizeof(head));
    if (got >= 12 && (std::memcmp(head, "RIFF", 4) == 0 || std::memcmp(head, "RF64", 4) == 0)) {
        const WavInfo w = parseWav(read, size);
        info.error = w.error;
        info.format = "wav";
        if (w.error != ReadError::None)
            return info;
        const int64_t length = w.length();
        if (w.srgo)
            return info.sample = true, info.frames = static_cast<int>(std::min<int64_t>(length, INT32_MAX)), info;
        std::string tag = "wav";
        int frame = wavFrameSize(w, tag);
        info.guessed = frame <= 0;
        if (info.guessed)
            frame = guessFrameSize(length, frameSize);
        info.format = info.guessed ? "wav" : tag;
        if (frame <= 0 || frame > 4096)
            return info.sample = true, info.frames = static_cast<int>(std::min<int64_t>(length, INT32_MAX)), info;
        info.frameSize = frame;
        info.frames = static_cast<int>(length / frame);
        return info;
    }
    if (got >= 4 && std::memcmp(head, "fLaC", 4) == 0) {
        info.format = "flac";
        // The stream's length from its STREAMINFO, which comes first.
        if (got < 42 || (head[4] & 0x7F) != 0)
            return info.error = ReadError::Broken, info;
        const uint8_t *s = head + 8;
        const uint64_t total = (uint64_t{s[13] & 0x0Fu} << 32) | uint64_t{s[14]} << 24 | uint64_t{s[15]} << 16 |
                               uint64_t{s[16]} << 8 | s[17];
        const int frame = guessFrameSize(static_cast<int64_t>(total), frameSize);
        info.guessed = frameSize <= 0;
        if (frame <= 0 || frame > 4096)
            return info.sample = true, info.frames = static_cast<int>(std::min<uint64_t>(total, INT32_MAX)), info;
        info.frameSize = frame;
        info.frames = static_cast<int>(total / static_cast<uint64_t>(frame));
        return info;
    }
    if (const WtHeader h = wtHeader(head, size); h.ok) {
        info.format = "wt";
        info.sample = (h.flags & 0x01) != 0;
        info.frameSize = static_cast<int>(h.size);
        info.frames = info.sample ? static_cast<int>(h.size) * h.count : h.count;
        if (12 + uint64_t{h.size} * h.count * ((h.flags & 0x04) ? 2 : 4) > size)
            info.error = ReadError::Broken;
        return info;
    }
    size_t first = 0;
    while (first < got && std::isspace(head[first]))
        ++first;
    if (first < got && head[first] == '{') {
        // A JSON file has to be read whole to be known: Vital's tables are small.
        info.format = "vital";
        if (size > (uint64_t{64} << 20))
            return info.error = ReadError::Broken, info;
        std::vector<uint8_t> all(static_cast<size_t>(size));
        if (read(0, all.data(), all.size()) != all.size())
            return info.error = ReadError::Broken, info;
        const TableFile file = readTableFile(all.data(), all.size(), name, ReadAs::Table, frameSize);
        info.error = file.error;
        info.frameSize = file.frameSize;
        info.frames = file.fileFrames;
        return info;
    }
    if (got >= 4 && std::memcmp(head, "CcnK", 4) == 0)
        return info.error = ReadError::PresetOnly, info;
    const ReadError named = knownUnreadable(name);
    info.error = named != ReadError::None ? named : ReadError::NotAudio;
    return info;
}

std::vector<uint8_t> writeTableWav(const float *frames, int count, int size, uint32_t rate) {
    std::vector<uint8_t> out;
    auto word = [&](uint32_t w) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(w >> (8 * i)));
    };
    auto half = [&](uint16_t h) {
        out.push_back(static_cast<uint8_t>(h));
        out.push_back(static_cast<uint8_t>(h >> 8));
    };
    auto tag = [&](const char *t) { out.insert(out.end(), t, t + 4); };
    std::string clm = "<!>" + std::to_string(size) + " 10000000 wavetable (www.xferrecords.com)";
    clm.resize(48, ' ');
    const uint32_t samples = static_cast<uint32_t>(count) * static_cast<uint32_t>(size);
    tag("RIFF");
    word(4 + (8 + 16) + (8 + static_cast<uint32_t>(clm.size())) + (8 + samples * 4));
    tag("WAVE");
    tag("fmt ");
    word(16);
    half(3);
    half(1);
    word(rate);
    word(rate * 4);
    half(4);
    half(32);
    tag("clm ");
    word(static_cast<uint32_t>(clm.size()));
    out.insert(out.end(), clm.begin(), clm.end());
    tag("data");
    word(samples * 4);
    const auto *b = reinterpret_cast<const uint8_t *>(frames);
    out.insert(out.end(), b, b + samples * 4);
    return out;
}

} // namespace fmvibe
