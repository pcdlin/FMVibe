#pragma once

// Reading other plugins' wavetable and sample files into a Wavetable or Sample operator's data (see opdata.h): WAV
// (RIFF, RF64, WAVE_FORMAT_EXTENSIBLE; PCM 8 to 32 bits, float 32 and 64; up to 8 channels, summed to mono) with the
// chunks that give a table's frame size (Serum's clm, u-he's uhWT, Surge's srge and srgo, cue points, smpl), FLAC,
// Surge's and Bitwig's .wt and Vital's .vitaltable. The format is told by the content, not the name. Independent of
// Windows and of threads: the browser demo reads the files a user picks with it, and tests/quality.cpp covers it.

#include "../uistrings.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fmvibe {

// Why a file can't be read, each with its own message (see readErrorText()).
enum class ReadError {
    None,
    NotAudio,      // neither audio nor a table FMVibe knows
    Broken,        // cut short, or its sizes don't hold together
    Unsupported,   // a sample format FMVibe doesn't read
    Encrypted,     // Native Instruments' and Arturia's own formats
    PresetOnly,    // a preset, whose table lives inside it (Serum's .fxp and .SerumPreset, Vital's .vital)
    LineSource,    // a Vital table drawn with lines, which FMVibe can't read yet
    Empty,         // no audio in it
    TooLong,       // a sample past kMaxSampleLength
    NotTable,      // asked for a table, and none of its chunks or lengths gives a frame size
};
Str readErrorText(ReadError error);

// What a file holds, as the patch would store it (see OpData): a table's frames (each FrameTable::kFrameSize samples,
// the DC out of each, the loudest peaking at 1), or a sample (mono, peaking at 1).
struct TableFile {
    ReadError error = ReadError::None;
    bool sample = false;
    std::vector<float> data;
    int frames = 0;           // a table's frames, a sample's length
    uint32_t rate = 44100;    // a sample's
    float root = 60.0f;       // a sample's root and loop, from its smpl chunk (loopEnd 0: none)
    uint32_t loopStart = 0, loopEnd = 0;
    bool sixteenBit = false;  // its samples had 16 bits or fewer
    // What the file said of itself: its format tag (shown in the table browser), its frames' size before they were
    // brought to FrameTable::kFrameSize (0 for a sample), whether that size was guessed (no chunk gave it), and its
    // frames before any were thinned out.
    std::string format;
    int frameSize = 0;
    bool guessed = false;
    int fileFrames = 0;
};

// How a file is read: as a table (its frame size from its chunks, else `frameSize` when it isn't 0, else guessed) or
// as a sample.
enum class ReadAs { Table, Sample };
TableFile readTableFile(const uint8_t *bytes, size_t size, const std::string &name, ReadAs as, int frameSize = 0);

// A file's bytes, read from where `at` asks: what scanning reads of a file, its headers and chunks but not its audio.
// Returns how many came (fewer at the end).
using ReadAt = std::function<size_t(uint64_t at, void *to, size_t size)>;
// What a scan lists of a file (see library.h), from its headers alone: as readTableFile() would read it as a table,
// but for its audio.
struct TableInfo {
    ReadError error = ReadError::None;
    bool sample = false; // it would read as a sample, not a table
    std::string format;
    int frameSize = 0;   // as the file has it
    bool guessed = false;
    int frames = 0;      // a table's frames as the file has them (before thinning), a sample's length
};
TableInfo probeTableFile(const ReadAt &read, uint64_t size, const std::string &name, int frameSize = 0);

// A frame size for a file of `length` samples that no chunk gives one for: `folder` (a folder's own setting) when it
// isn't 0, else 2048 when the length divides by it, else the first of 1024, 512, 256, 4096 and 128 that divides it into
// 2 to 512 frames, else the whole of it as one frame when it is 4096 samples or less; 0 if none (read it as a sample).
int guessFrameSize(int64_t length, int folder);

// The DC out of each frame and one gain for them all, from the loudest, as tables are stored; frames past
// FrameTable::kMaxFrames thinned evenly; frames of `size` (128 to 4096) brought to FrameTable::kFrameSize through their
// spectrum. Reads `count` frames from `samples`.
std::vector<float> normalizeFrames(const float *samples, int count, int size, int &frames);

// Decodes a FLAC stream: its samples, mono (channels summed), and its rate and bits; false if it isn't one or is broken.
bool decodeFlac(const uint8_t *bytes, size_t size, std::vector<float> &mono, uint32_t &rate, int &bits);

// Writes `frames` frames of `size` samples as a WAV of 32-bit floats with Serum's clm chunk, as FMVibe exports a table.
std::vector<uint8_t> writeTableWav(const float *frames, int count, int size, uint32_t rate = 44100);

} // namespace fmvibe
