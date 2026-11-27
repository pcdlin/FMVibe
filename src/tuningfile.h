#pragma once

// Tuning files, read into the patch's own tuning (see TuningParam), and what that tuning does to each key. Scala's scale
// (.scl) and keyboard mapping (.kbm), Ableton's Scala (.ascl, a .scl with its reference pitch in a comment) and
// AnaMark's tuning (.tun, every key's pitch). No files are opened here: the plugin reads them and hands their text over,
// so the browser demo and the tests build this too.

#include "params.h"
#include "uistrings.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmvibe {

// A scale: its degrees' cents above degree 0, the period the last, where it plays with no keyboard mapping (the key
// degree 0 is on, and a key's frequency), and the line describing it.
struct TuningScale {
    std::vector<double> cents;
    int middle = kTuningMiddle;
    int refKey = kTuningMiddle;
    double refHz = kTuningMiddleHz;
    std::string description;
};

// A keyboard mapping, as a .kbm has it: which degree each key plays, around its middle key, and the key at its
// reference frequency. No entries (size 0) is linear: a key plays as many degrees from degree 0 as it is from the
// middle key.
struct TuningMapping {
    int first = 0, last = 127;
    int middle = kTuningMiddle;
    int refKey = 69;
    double refHz = 440.0;
    int octave = 0;           // degrees the entries move by each time round them; 0: the scale's
    std::vector<int> entries; // the degree each plays, -1 for none
};

// What a file holds, by its extension (".scl", any case): a scale, Ableton's scale, AnaMark's tuning, a keyboard
// mapping, or none of these.
enum class TuningFile { Scala, AbletonScala, AnaMark, Mapping, Unknown };
TuningFile tuningFileOf(std::string_view extension);

// Each reads a file's text (UTF-8, a BOM skipped) into `out`, or says why it can't (Str::TuningNotScale and so on).
bool parseScl(std::string_view text, bool ableton, TuningScale &out, Str &why);
bool parseTun(std::string_view text, TuningScale &out, Str &why);
bool parseKbm(std::string_view text, TuningMapping &out, Str &why);

// The param changes that load a scale or a keyboard mapping into the patch, every field of its part; null takes it out
// again (back to the fields' defaults: 12-TET, or no mapping).
std::vector<std::pair<int, double>> scaleChanges(const TuningScale *scale);
std::vector<std::pair<int, double>> mappingChanges(const TuningMapping *mapping);

// What the patch's own tuning does to each key: how far it plays off 12-TET in semitones, and whether it is unmapped
// (a note there isn't to sound). Off with neither a scale nor a mapping: every key as 12-TET. Allocates nothing, so the
// synth works it out on the audio thread as the params change.
struct TuningTable {
    bool on = false;
    std::array<double, 128> retune{};
    std::array<bool, 128> unmapped{};
};
void tuningTable(const ParamValues &params, TuningTable &out);

} // namespace fmvibe
