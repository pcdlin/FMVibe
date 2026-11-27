#include "tuningfile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fmvibe {

namespace {

constexpr double kAnaMarkBaseHz = 8.1757989156437073336; // AnaMark's note 0 at 0 cents: MIDI key 0 in 12-TET

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// The text's lines, a BOM skipped, each without its line end.
std::vector<std::string_view> lines(std::string_view text) {
    if (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0)
        text.remove_prefix(3);
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        out.push_back(text.substr(0, end));
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return out;
}

// The first whitespace-separated word of a line.
std::string_view firstWord(std::string_view line) {
    line = trimmed(line);
    const size_t end = line.find_first_of(" \t");
    return line.substr(0, end);
}

// A number that is all of `word`, finite.
bool number(std::string_view word, double &out) {
    if (word.empty() || word.size() > 63)
        return false;
    char buffer[64];
    std::memcpy(buffer, word.data(), word.size());
    buffer[word.size()] = '\0';
    char *end = nullptr;
    out = std::strtod(buffer, &end);
    return end == buffer + word.size() && std::isfinite(out);
}

bool integer(std::string_view word, int &out) {
    double value = 0.0;
    if (!number(word, value) || value != std::floor(value) || std::abs(value) > 1.0e6)
        return false;
    out = static_cast<int>(value);
    return true;
}

// A Scala pitch: cents (with a period in it) or a ratio (a/b, or a whole number), as cents.
bool scalaPitch(std::string_view word, double &cents) {
    if (word.find('.') != std::string_view::npos)
        return number(word, cents);
    const size_t slash = word.find('/');
    double over = 0.0, under = 1.0;
    if (!number(word.substr(0, slash), over) ||
        (slash != std::string_view::npos && !number(word.substr(slash + 1), under)) || over <= 0.0 || under <= 0.0)
        return false;
    cents = 1200.0 * std::log2(over / under);
    return std::isfinite(cents);
}

long floorDiv(long a, long b) {
    const long q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// A key the scale's reference falls off the keys from, moved by periods back onto them (its frequency with it), and
// degree 0's key likewise: the pitches stay where they were.
void ontoKeys(TuningScale &scale) {
    const int n = static_cast<int>(scale.cents.size());
    const double period = scale.cents.back();
    while (scale.refKey > 127) {
        scale.refKey -= n;
        scale.refHz *= std::exp2(-period / 1200.0);
    }
    while (scale.refKey < 0) {
        scale.refKey += n;
        scale.refHz *= std::exp2(period / 1200.0);
    }
    while (scale.middle > 127)
        scale.middle -= n;
    while (scale.middle < 0)
        scale.middle += n;
}

} // namespace

TuningFile tuningFileOf(std::string_view extension) {
    const std::string ext = lower(extension);
    return ext == ".scl"    ? TuningFile::Scala
           : ext == ".ascl" ? TuningFile::AbletonScala
           : ext == ".tun"  ? TuningFile::AnaMark
           : ext == ".kbm"  ? TuningFile::Mapping
                            : TuningFile::Unknown;
}

bool parseScl(std::string_view text, bool ableton, TuningScale &out, Str &why) {
    TuningScale scale;
    bool described = false, counted = false;
    int count = 0;
    // Ableton's reference: the degree in an octave (counted from octave 4, where degree 0 is middle C) and its pitch.
    bool referenced = false;
    int refOctave = 0, refDegree = 0;
    double refHz = 0.0;
    for (std::string_view line : lines(text)) {
        const std::string_view body = trimmed(line);
        if (!body.empty() && body.front() == '!') {
            // ! @ABL REFERENCE_PITCH <octave> <degree> <Hz>
            if (!ableton)
                continue;
            std::string_view rest = trimmed(body.substr(1));
            if (rest.substr(0, 4) != "@ABL")
                continue;
            rest = trimmed(rest.substr(4));
            if (firstWord(rest) != "REFERENCE_PITCH")
                continue;
            rest = trimmed(rest.substr(std::strlen("REFERENCE_PITCH")));
            std::string_view words[3];
            for (std::string_view &word : words) {
                word = firstWord(rest);
                rest = trimmed(rest.substr(word.size()));
            }
            referenced = integer(words[0], refOctave) && integer(words[1], refDegree) && number(words[2], refHz) &&
                         refHz > 0.0;
            continue;
        }
        if (!described) { // the description, which may be empty
            scale.description = std::string(body);
            described = true;
        } else if (!counted) {
            if (!integer(firstWord(body), count) || count < 1) {
                why = Str::TuningNotScale;
                return false;
            }
            if (count > kMaxTuningDegrees) {
                why = Str::TuningTooManyNotes;
                return false;
            }
            counted = true;
        } else if (!body.empty() && static_cast<int>(scale.cents.size()) < count) {
            double cents = 0.0;
            if (!scalaPitch(firstWord(body), cents) || std::abs(cents) > kMaxTuningCents) {
                why = Str::TuningNotScale;
                return false;
            }
            scale.cents.push_back(cents);
        }
    }
    if (!counted || static_cast<int>(scale.cents.size()) < count) {
        why = Str::TuningNotScale;
        return false;
    }
    if (referenced) {
        const int n = count;
        scale.middle = kTuningMiddle + (refOctave - 4) * n;
        scale.refKey = scale.middle + refDegree;
        scale.refHz = refHz;
        ontoKeys(scale);
    }
    out = std::move(scale);
    return true;
}

bool parseTun(std::string_view text, TuningScale &out, Str &why) {
    // Each key's cents over the base frequency: [Exact Tuning]'s where it has them, else [Tuning]'s, else 12-TET's.
    std::array<double, 128> coarse{}, exact{};
    for (int key = 0; key < 128; ++key)
        coarse[key] = exact[key] = 100.0 * key;
    bool hasCoarse = false, hasExact = false;
    double baseHz = kAnaMarkBaseHz;
    std::string section, name;
    for (std::string_view line : lines(text)) {
        std::string_view body = trimmed(line);
        if (const size_t comment = body.find(';'); comment != std::string_view::npos)
            body = trimmed(body.substr(0, comment));
        if (body.empty())
            continue;
        if (body.front() == '[') {
            section = lower(body);
            hasCoarse = hasCoarse || section == "[tuning]";
            hasExact = hasExact || section == "[exact tuning]";
            continue;
        }
        const size_t equals = body.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string key = lower(trimmed(body.substr(0, equals)));
        const std::string_view value = trimmed(body.substr(equals + 1));
        if (section == "[info]" && key == "name") {
            std::string_view quoted = value;
            if (quoted.size() >= 2 && quoted.front() == '"' && quoted.back() == '"')
                quoted = quoted.substr(1, quoted.size() - 2);
            name = std::string(quoted);
            continue;
        }
        if (section == "[exact tuning]" && key == "basefreq") {
            double hz = 0.0;
            if (number(value, hz) && hz > 0.0)
                baseHz = hz;
            continue;
        }
        if (key.rfind("note", 0) != 0 || (section != "[tuning]" && section != "[exact tuning]"))
            continue;
        int note = -1;
        double cents = 0.0;
        if (!integer(trimmed(std::string_view(key).substr(4)), note) || note < 0 || note > 127 ||
            !number(value, cents) || std::abs(cents) > kMaxTuningCents) {
            why = Str::TuningNotAnaMark;
            return false;
        }
        (section == "[exact tuning]" ? exact : coarse)[note] = cents;
    }
    if (!hasCoarse && !hasExact) {
        why = Str::TuningNotAnaMark;
        return false;
    }
    const std::array<double, 128> &cents = hasExact ? exact : coarse;
    // As a scale of 128 degrees from key 0, which degree 0 is on and which plays at its pitch: key k plays degree k.
    TuningScale scale;
    scale.cents.resize(kMaxTuningDegrees);
    for (int key = 1; key < 128; ++key)
        scale.cents[key - 1] = cents[key] - cents[0];
    scale.cents[127] = 2.0 * scale.cents[126] - (scale.cents.size() > 2 ? scale.cents[125] : 0.0); // a step on: unplayed
    scale.middle = 0;
    scale.refKey = 0;
    scale.refHz = baseHz * std::exp2(cents[0] / 1200.0);
    scale.description = name;
    if (!std::isfinite(scale.refHz) || scale.refHz < kMinTuningHz || scale.refHz > kMaxTuningHz) {
        why = Str::TuningNotAnaMark;
        return false;
    }
    out = std::move(scale);
    return true;
}

bool parseKbm(std::string_view text, TuningMapping &out, Str &why) {
    TuningMapping mapping;
    int size = -1;
    int field = 0; // the fields read so far: size, first, last, middle, reference key, its Hz, octave, then entries
    for (std::string_view line : lines(text)) {
        const std::string_view body = trimmed(line);
        if (body.empty() || body.front() == '!')
            continue;
        const std::string_view word = firstWord(body);
        bool ok = true;
        switch (field) {
        case 0:
            ok = integer(word, size) && size >= 0;
            if (ok && size > kMaxMapKeys) {
                why = Str::TuningMapTooLong;
                return false;
            }
            break;
        case 1:
            ok = integer(word, mapping.first) && mapping.first >= 0 && mapping.first <= 127;
            break;
        case 2:
            ok = integer(word, mapping.last) && mapping.last >= 0 && mapping.last <= 127;
            break;
        case 3:
            ok = integer(word, mapping.middle) && mapping.middle >= 0 && mapping.middle <= 127;
            break;
        case 4:
            ok = integer(word, mapping.refKey) && mapping.refKey >= 0 && mapping.refKey <= 127;
            break;
        case 5:
            ok = number(word, mapping.refHz) && mapping.refHz >= kMinTuningHz && mapping.refHz <= kMaxTuningHz;
            break;
        case 6:
            ok = integer(word, mapping.octave) && mapping.octave >= 0 && mapping.octave <= kMaxTuningDegrees;
            break;
        default:
            if (static_cast<int>(mapping.entries.size()) < size) {
                int degree = -1;
                if (word == "x" || word == "X")
                    mapping.entries.push_back(-1);
                else if (integer(word, degree) && degree >= 0 && degree <= kMaxMapDegree)
                    mapping.entries.push_back(degree);
                else
                    ok = false;
            }
            break;
        }
        if (!ok) {
            why = Str::TuningNotMapping;
            return false;
        }
        ++field;
    }
    if (field < 7 || static_cast<int>(mapping.entries.size()) < size) {
        why = Str::TuningNotMapping;
        return false;
    }
    out = std::move(mapping);
    return true;
}

std::vector<std::pair<int, double>> scaleChanges(const TuningScale *scale) {
    const ParamTable &table = paramTable();
    std::vector<std::pair<int, double>> changes;
    auto set = [&](int index, double value) { changes.emplace_back(index, value); };
    const int n = scale ? static_cast<int>(scale->cents.size()) : 0;
    set(paramIndex(TuningParam::Degrees), n);
    set(paramIndex(TuningParam::Middle), scale ? scale->middle : table[paramIndex(TuningParam::Middle)].def);
    set(paramIndex(TuningParam::RefKey), scale ? scale->refKey : table[paramIndex(TuningParam::RefKey)].def);
    set(paramIndex(TuningParam::RefHz), scale ? scale->refHz : table[paramIndex(TuningParam::RefHz)].def);
    for (int degree = 0; degree < kMaxTuningDegrees; ++degree)
        set(tuningDegreeIndex(degree), degree < n ? scale->cents[degree] : table[tuningDegreeIndex(degree)].def);
    return changes;
}

std::vector<std::pair<int, double>> mappingChanges(const TuningMapping *mapping) {
    const ParamTable &table = paramTable();
    std::vector<std::pair<int, double>> changes;
    auto set = [&](TuningParam f, double value) { changes.emplace_back(paramIndex(f), value); };
    auto def = [&](TuningParam f) { return table[paramIndex(f)].def; };
    const int size = mapping ? static_cast<int>(mapping->entries.size()) : 0;
    set(TuningParam::MapOn, mapping ? 1.0 : 0.0);
    set(TuningParam::MapSize, size);
    set(TuningParam::MapFirst, mapping ? mapping->first : def(TuningParam::MapFirst));
    set(TuningParam::MapLast, mapping ? mapping->last : def(TuningParam::MapLast));
    set(TuningParam::MapMiddle, mapping ? mapping->middle : def(TuningParam::MapMiddle));
    set(TuningParam::MapRefKey, mapping ? mapping->refKey : def(TuningParam::MapRefKey));
    set(TuningParam::MapRefHz, mapping ? mapping->refHz : def(TuningParam::MapRefHz));
    set(TuningParam::MapOctave, mapping ? mapping->octave : def(TuningParam::MapOctave));
    for (int entry = 0; entry < kMaxMapKeys; ++entry)
        changes.emplace_back(tuningMapIndex(entry),
                             entry < size ? mapping->entries[entry] : table[tuningMapIndex(entry)].def);
    return changes;
}

void tuningTable(const ParamValues &p, TuningTable &out) {
    auto whole = [&](int index) { return static_cast<int>(std::lround(p[index])); };
    const int degrees = std::clamp(whole(paramIndex(TuningParam::Degrees)), 0, kMaxTuningDegrees);
    const bool mapped = p[paramIndex(TuningParam::MapOn)] >= 0.5;
    out.on = degrees > 0 || mapped;
    out.retune.fill(0.0);
    out.unmapped.fill(false);
    if (!out.on)
        return;
    // The scale: its own, or 12-TET's under a mapping alone.
    const int n = degrees > 0 ? degrees : 12;
    auto degreeCents = [&](int r) { return degrees > 0 ? p[tuningDegreeIndex(r - 1)] : 100.0 * r; }; // 1..n
    const double period = degreeCents(n);
    auto centsOf = [&](long degree) {
        const long octave = floorDiv(degree, n);
        const int r = static_cast<int>(degree - octave * n);
        return static_cast<double>(octave) * period + (r == 0 ? 0.0 : degreeCents(r));
    };
    // The mapping: the .kbm's, or with none the scale's own reference, every key playing in a line.
    const int size = mapped ? std::clamp(whole(paramIndex(TuningParam::MapSize)), 0, kMaxMapKeys) : 0;
    const int first = mapped ? whole(paramIndex(TuningParam::MapFirst)) : 0;
    const int last = mapped ? whole(paramIndex(TuningParam::MapLast)) : 127;
    const int middle = whole(paramIndex(mapped ? TuningParam::MapMiddle : TuningParam::Middle));
    const int refKey = whole(paramIndex(mapped ? TuningParam::MapRefKey : TuningParam::RefKey));
    const double refHz = p[paramIndex(mapped ? TuningParam::MapRefHz : TuningParam::RefHz)];
    const int mapOctave = mapped ? whole(paramIndex(TuningParam::MapOctave)) : 0;
    const int octave = mapOctave > 0 ? mapOctave : n;
    // The degree a key plays, false for none; the reference key's, though unmapped, as the entry's place would.
    auto degreeOf = [&](int key, bool reference, long &degree) {
        if (!reference && (key < first || key > last))
            return false;
        const long offset = key - middle;
        if (size == 0) {
            degree = offset;
            return true;
        }
        const long round = floorDiv(offset, size);
        const int place = static_cast<int>(offset - round * size);
        int entry = whole(tuningMapIndex(place));
        if (entry < 0) {
            if (!reference)
                return false;
            entry = place;
        }
        degree = entry + round * octave;
        return true;
    };
    long refDegree = 0;
    degreeOf(refKey, true, refDegree);
    const double refCents = centsOf(refDegree);
    const double refOff = 12.0 * std::log2(refHz / 440.0) + 69.0; // the reference's pitch, as a key in 12-TET
    for (int key = 0; key < 128; ++key) {
        long degree = 0;
        if (!degreeOf(key, false, degree)) {
            out.unmapped[key] = true;
            continue;
        }
        out.retune[key] = refOff + (centsOf(degree) - refCents) / 100.0 - key;
    }
}

} // namespace fmvibe
