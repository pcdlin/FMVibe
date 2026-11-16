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
};
constexpr int16_t kSeqPort = -2;
// The editor's notes heard on their own (a key or a note clicked in the SEQ view's roll): never taken by the
// sequencer as keys, and no host waits for their ends either.
constexpr int16_t kAuditionPort = -3;

// The sequencer: plays the pattern its params pick, from the host's transport (SeqPlay::Host, locked to its bars) or
// from a key (Key and Latch, transposed by the key from Root). Time runs in played steps, counted from the song's
// start (Host) or from the key that started it (Key, Latch); each played step plays a step of the pattern's loop as
// the Order says (forward, back, to and fro, or at random), the notes starting in that step at the same place in it.
// Swing delays every other played step, Gate scales the notes' lengths, Humanize delays them and moves their velocity a
// little, the same way every time a step comes round again. A note's chance is drawn the same way.
// Audio thread only.
class Sequencer {
public:
    static constexpr int kMaxSounding = 256; // notes it holds on at once; past that, the oldest ends
    static constexpr int kMaxEvents = 1024;  // what plan() fills in at most

    void prepare(double sampleRate) { sampleRate_ = sampleRate; }
    // Forgets the notes sounding (the synth ends them itself) and where it was.
    void reset();
    // Takes the settings from the params, at the start of each render(), and the notes and curves too where `changed`
    // (a sequencer param may have) or the pattern to play isn't the one last read.
    void configure(const ParamValues &params, bool changed = true);

    bool enabled() const { return enabled_; }
    // Whether keys go to the sequencer rather than playing notes: in Key and Latch, while it is on.
    bool takesKeys() const { return enabled_ && play_ != SeqPlay::Host; }
    // Whether it may start notes without a key or an event to wake it: on, playing with the host's transport, or
    // running from a key.
    bool wantsProcessing() const { return enabled_ && (play_ == SeqPlay::Host || keyRunning_) ; }
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
    // (-1..1) or the mod wheel (0..1). NaN while it doesn't play, or the curve has no points: nothing to add then.
    double curve(SeqCurve c) const { return curveNow_[static_cast<size_t>(c)]; }

private:
    struct Note {
        double start, length, velocity, chance;
        int pitch;
        bool slide;
        int index; // its slot in the pattern, for drawing its chance and humanizing it
    };
    struct Sounding {
        int32_t id;
        int key;
        int pitch;   // as written in the pattern
        double end;  // played steps
        double off;  // when it ends at the latest: its end, or a slide's end and a step more
        bool slide;
    };

    void readPattern(int pattern);
    int loopStart() const { return loopStart_; }
    int loopLength() const { return loopEnd_ - loopStart_; }
    // The pattern step a played step plays.
    int sourceStep(int64_t step) const;
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
    int root_ = 60;
    int loopStart_ = 0, loopEnd_ = kSeqStepsPerBar;
    std::array<Note, kSeqNotes> notes_{};
    int noteCount_ = 0;
    int read_ = -1; // the pattern notes_ and curves_ were read from, -1 for none
    std::array<std::array<SeqCurvePoint, kSeqCurvePoints>, kSeqCurves> curves_{}; // the playing pattern's
    std::array<int, kSeqCurves> curveCount_{};
    std::array<double, kSeqCurves> curveNow_{std::numeric_limits<double>::quiet_NaN(),
                                             std::numeric_limits<double>::quiet_NaN()};

    // From keys: those held, newest last, and the one transposing; whether it runs, and its own clock (played steps).
    std::array<int, 16> keys_{};
    int keyCount_ = 0;
    int transposeKey_ = -1;
    bool keyRunning_ = false;
    bool keyStarted_ = false; // a key started it since the last plan(): its clock starts over
    double keyClock_ = 0.0;

    std::array<Sounding, kMaxSounding> sounding_{};
    int soundingCount_ = 0;
    int32_t nextId_ = 1;
    double next_ = 0.0;   // where the last plan() ended, in played steps
    bool running_ = false; // planning from next_ on: it ran in the last plan()
    float position_ = -1.0f;
};

} // namespace fmvibe
