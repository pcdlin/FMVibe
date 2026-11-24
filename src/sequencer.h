#pragma once

#include "params.h"

#include <array>
#include <cstdint>
#include <span>

namespace fmvibe {

// A note the sequencer starts or ends, `frame` samples into the stretch it was planned for. Its notes go to the synth
// as notes on port kSeqPort, each with an id of its own, so the synth never reports their ends to the host.
struct SeqEvent {
    uint32_t frame = 0;
    bool on = false;
    int key = 0;
    double velocity = 0.0;
    int32_t id = 0;
    double bend = 0.0; // a note starting: its bend as it starts, in semitones
};
// Where a bent note's bend goes over the stretch planned: from `from` at `frame` (its start in the stretch, or 0) to
// `to` at the stretch's end, in semitones (see Sequencer::bends()).
struct SeqBend {
    int32_t id = 0;
    uint32_t frame = 0;
    double from = 0.0, to = 0.0;
};
constexpr int16_t kSeqPort = -2;
// The editor's notes heard on their own (a key or a note clicked in the SEQ view's roll): never taken by the
// sequencer as keys, and no host waits for their ends either.
constexpr int16_t kAuditionPort = -3;

// The sequencer: plays the pattern its params pick, from the host's transport (SeqPlay::Host, locked to its bars) or
// from a key (Key and Latch, transposed by the key from C4), or, whatever Play says, from the editor's Play button
// (preview(): from the loop's start, untransposed by a key). Time runs in played steps, counted from the song's
// start (Host) or from the key or the button that started it; each played step plays a step of the pattern's loop as
// the Order says (forward, back, to and fro, or at random among the steps notes start in, randomStep()), the notes
// starting in that step at the same place in it.
// Swing delays every other played step, Gate scales the notes' lengths, Humanize delays them and moves their velocity a
// little, the same way every time a step comes round again. A note's chance is drawn the same way. It chases notes: as
// it starts playing (or the transport jumps), and as the pattern changes, a note the playhead is in the middle of that
// isn't sounding starts at once, for what is left of it. The loop moved or resized while it plays, it plays on from
// the step it is at (keepPlace()), rather than where the new loop's cycles would put it; with the host's transport
// it locks to the bars again once the transport starts or jumps.
// Audio thread only.
class Sequencer {
public:
    static constexpr int kMaxSounding = 256; // notes it holds on at once; past that, the oldest ends
    static constexpr int kMaxEvents = 1024;  // what plan() fills in at most

    Sequencer() { forgetCurves(); }
    void prepare(double sampleRate) { sampleRate_ = sampleRate; }
    // Forgets the notes sounding (the synth ends them itself) and where it was.
    void reset();
    // Takes the settings from the params, at the start of each render(), and the notes and curves too where `changed`
    // (a sequencer param may have) or the pattern to play isn't the one last read.
    void configure(const ParamValues &params, bool changed = true);
    // Swing, Gate, Velocity and Humanize, and the pattern picked, as modulation takes them now, in plain units (each in
    // its range), from the next stretch planned on, in place of what configure() read (see Synth::feedSeqFeel()). The
    // pattern, rounded to a whole one, takes over where a pattern picked does: at once while nothing plays, else where
    // the loop comes round.
    void setFeel(const std::array<double, kSeqFeelCount> &feel);

    bool enabled() const { return enabled_; }
    // Whether keys go to the sequencer rather than playing notes: in Key and Latch, while it is on and the Play button
    // doesn't play it.
    bool takesKeys() const { return enabled_ && play_ != SeqPlay::Host && !previewing_; }
    // Whether it may start notes without a key or an event to wake it: on, playing with the host's transport, running
    // from a key or played by the Play button.
    bool wantsProcessing() const { return enabled_ && (play_ == SeqPlay::Host || keyRunning_ || previewing_); }

    // The editor's Play button: on, it plays the pattern from its loop's start on a clock of its own, as written but for
    // Octave and Transpose, whatever Play says (the host's transport and the keys leaving it be, the keys playing notes
    // as they would without it), until it is turned off, the sequencer is switched off or reset() comes. Off, it goes
    // back to playing as Play says. Nothing while the sequencer is off. `from`, a step of the pattern within its loop
    // (fractions between; the start of the first note picked in the roll), starts it there instead, going the way the
    // Order goes and the notes before it left out, Swing's delay of that step taken off so a note there starts at once;
    // -1, or a step outside the loop, or Random order, from the loop's start.
    void preview(bool on, double from = -1.0);
    bool previewing() const { return previewing_; }
    bool sounding() const { return soundingCount_ > 0; }

    // A key going down or up while it takes keys. Key plays while a key is down, from the newest; Latch starts with a
    // key and plays on once they are let go, a new key transposing it.
    void keyDown(int key);
    void keyUp(int key);
    // All notes off: Latch stops too, as Key does with its keys let go.
    void unlatch() {
        if (play_ == SeqPlay::Latch && keyCount_ == 0)
            keyRunning_ = false;
    }

    // The events of the next `frames` samples, in order, at `tempo`, with the host's transport as the stretch starts.
    // Returns how many it put in `out`; `covered` is how many of the frames it planned (all of them, unless it stopped
    // where the pattern changes, or `out` filled up): the next call plans on from there.
    int plan(uint32_t frames, double tempo, bool playing, double songBeats, std::span<SeqEvent> out, uint32_t &covered);
    // Every note it holds on, ended now.
    int stopAll(std::span<SeqEvent> out);

    // The pitches the notes it holds on are written at in the pattern, untransposed by Octave, Transpose or a key, a
    // bit each (pitch 0 is bit 0 of the first): the rows the roll lights.
    std::array<uint32_t, 4> soundingPitches() const {
        std::array<uint32_t, 4> pitches{};
        for (int i = 0; i < soundingCount_; ++i)
            pitches[static_cast<size_t>(sounding_[i].pitch / 32)] |= 1u << (sounding_[i].pitch % 32);
        return pitches;
    }
    // Where it plays, in the playing pattern's steps (fractions between), and which pattern; -1 while it doesn't.
    float position() const { return position_; }
    int pattern() const { return pattern_; }
    // Where the playing pattern's curve stands as the last plan()'s stretch starts, where it plays: the pitch bend
    // (-1..1), the mod wheel or a macro's knob (0..1). NaN while it doesn't play, or the curve has no points: it does
    // nothing then.
    double curve(SeqCurve c) const { return curveNow_[static_cast<size_t>(c)]; }
    // The bends of the bent notes it holds on over the last plan()'s stretch, those started in it too: each note's bend
    // runs in played steps from where it started, holding the value it reached at its end (it doesn't scale with Gate).
    std::span<const SeqBend> bends() const { return {bends_.data(), static_cast<size_t>(bendCount_)}; }

private:
    struct Note {
        double start, length, velocity, chance;
        int pitch;
        bool slide;
        int index; // its slot in the pattern, for drawing its chance and humanizing it
    };
    struct Sounding {
        int64_t step; // the played step it started in, and its slot in the pattern: which of the notes it is
        int index;
        int32_t id;
        int key;
        int pitch;   // as written in the pattern
        double begin; // played steps: where it started (before the stretch it was chased in, for a note chased)
        double end;  // played steps
        double off;  // when it ends at the latest: its end, or a slide's end and a step more
        bool slide;
        int pattern;  // which pattern it is from: one taking over leaves its bend where it was
        double bent;  // its bend where the last stretch ended
    };
    // A sounding note's bend at played step `t`, in semitones.
    double bendOf(const Sounding &s, double t) const;

    void readPattern(int pattern);
    // The loop of the pattern playing moved or resized from `oldStart`..`oldEnd`: the clock moves on by whole steps so
    // that the step it plays next is the one it played (or, out of the new loop, where that step falls in it), going the
    // same way, and the notes it holds on move with it, so that they aren't chased again.
    void keepPlace(int oldStart, int oldEnd);
    int loopStart() const { return loopStart_; }
    int loopLength() const { return loopEnd_ - loopStart_; }
    // The pattern step a played step plays.
    int sourceStep(int64_t step) const;
    // Random's: each time round the loop, picks among the steps notes start in, each played on to the next one's (round
    // the loop's end to the first), one after another, repeats too, until the loop's length is filled, the last cut
    // short there; the same picks for the same played steps each time. With no note in the loop, as Forward.
    int randomStep(int64_t step) const;
    bool backward(int64_t step) const; // whether a played step runs through its pattern step backward
    double swung(double t) const;
    // Where played step `t` (fractions between) is in the pattern: as position() says, back through a step played
    // backward.
    double patternPlace(double t) const;
    void forgetCurves() { curveNow_.fill(std::numeric_limits<double>::quiet_NaN()); }
    static double unit(int64_t step, int note, int salt); // 0..1, the same for the same inputs
    int pushOff(std::span<SeqEvent> out, int count, uint32_t frame, const Sounding &s) const;

    double sampleRate_ = 48000.0;
    const ParamValues *params_ = nullptr;
    bool enabled_ = false;
    SeqPlay play_ = SeqPlay::Host;
    int patternWanted_ = 0;
    int pattern_ = 0;
    int rate_ = static_cast<int>(SeqRate::Sixteenth);
    double swing_ = 0.5, gate_ = 1.0, velocityAmount_ = 1.0, humanize_ = 0.0;
    int shift_ = 0; // Octave and Transpose, in semitones
    SeqOrder order_ = SeqOrder::Forward;
    int loopStart_ = 0, loopEnd_ = kSeqStepsPerBar;
    std::array<Note, kSeqNotes> notes_{};
    int noteCount_ = 0;
    std::array<int, kSeqMaxSteps + 1> stepFirst_{}; // notes_ in order of their starts: the first in each step and on
    // The steps of the loop notes start in, in order, and how many steps each runs on to the next (the last round the
    // loop's end to the first): what Random picks among (randomStep()).
    std::array<int, kSeqMaxSteps> noteStep_{}, noteStepLength_{};
    int noteStepCount_ = 0;
    double longest_ = 0.0;                          // the longest note's length, in steps
    int read_ = -1; // the pattern notes_ and curves_ were read from, -1 for none
    std::array<std::array<SeqCurvePoint, kSeqCurvePoints>, kSeqCurves> curves_{}; // the playing pattern's
    std::array<int, kSeqCurves> curveCount_{};
    std::array<double, kSeqCurves> curveNow_{};
    int curvePeriod_ = kSeqStepsPerBar; // the playing pattern's length in steps, which its curves wrap around
    // The playing pattern's notes' bends, by their slots (Note::index).
    std::array<std::array<SeqCurvePoint, kSeqNoteBendPoints>, kSeqNotes> notePoints_{};
    std::array<uint8_t, kSeqNotes> notePointCount_{};
    std::array<SeqBend, kMaxSounding> bends_{};
    int bendCount_ = 0;

    // From keys: those held, newest last, and the one transposing; whether it runs, and its own clock (played steps).
    std::array<int, 16> keys_{};
    int keyCount_ = 0;
    int transposeKey_ = -1;
    bool keyRunning_ = false;
    bool keyStarted_ = false; // a key started it since the last plan(): its clock starts over
    double keyClock_ = 0.0;   // the clock of a run from a key or the Play button
    bool previewing_ = false; // the Play button plays it (see preview())
    double previewFrom_ = -1.0; // the pattern's step it starts at, -1 for the loop's start (see preview())
    double startFrom_ = 0.0;    // and where in played steps, unswung: the notes before it aren't chased
    // Whole played steps the clock runs ahead of the host's transport since a loop moved under it (see keepPlace()); 0
    // as the transport starts or jumps, which locks the pattern to its bars again.
    double hostShift_ = 0.0;

    std::array<Sounding, kMaxSounding> sounding_{};
    int soundingCount_ = 0;
    int32_t nextId_ = 1;
    double next_ = 0.0;   // where the last plan() ended, in played steps
    bool running_ = false; // planning from next_ on: it ran in the last plan()
    // Whether the next plan() chases the notes the playhead is in the middle of (it started, or the pattern was read
    // again), and how far back they may start: where it started, or where another pattern took over.
    bool chase_ = false;
    double chaseFrom_ = 0.0;
    float position_ = -1.0f;
};

} // namespace fmvibe
