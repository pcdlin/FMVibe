#pragma once

#include "common.h"

namespace fmvibe {

// The chorus's drifts' paces (Hz), a voice each (see chorusShare()). Below kChorusBassHz (a one-pole's corner), the
// sound goes past the voices, left as it is: the bass stays steady and in the middle.
inline constexpr double kChorusDriftHz[kMaxChorusVoices] = {0.23, 0.31, 0.19, 0.37, 0.27, 0.41};
constexpr double kChorusBassHz = 200.0;
// The chorus's voices' gains on each side: voice `voice` of `voices` panned at equal power between the sides by Spread
// (all in the middle at 0, from left to right in order at 1), and each scaled by 1 / sqrt(voices): voices that wait
// unalike add up about as loud as what went in, however many there are.
struct ChorusGains {
    double left, right;
};
ChorusGains chorusGains(int voice, int voices, double spread);

namespace fx {

// The chorus: a delay line a side, holding the sound above its bass, which each voice reads behind at its own swept
// delay (between samples on a cubic): the voices' sweeps a share of a cycle apart and their delays' places along them
// (both gliding there when Voices changes), their gains on each side ramped (so a voice coming or going fades). Delay
// and Depth moving slowly, the voices follow them; jumping (as one a note's own random value sets does, when the
// effects take up the new note), they crossfade from where they read to where they now put them.
// Where Delay and Depth have the voices read: Delay in samples, and how far the sweep and the drift take each voice off
// it, as shares of it.
struct ChorusReads {
    double delay = 0.0, swing = 0.0, wander = 0.0;
    bool operator==(const ChorusReads &) const = default;
};
struct Chorus {
    std::array<ZeroBuffer, 2> lines;
    int write = 0;
    uint64_t ran = 0; // samples run since it started: its lines hold nothing past as many from their start
    std::array<double, 2> bass{}; // the one-pole low-pass a side that takes the bass past the voices
    double phase = 0.0;           // the first voice's sweep, in cycles
    double mix = 0.5;
    std::array<double, kMaxChorusVoices> offsets{};      // each voice's sweep behind the first's, in cycles
    std::array<double, kMaxChorusVoices> stagger{};      // each voice's chorusStagger(), gliding
    std::array<double, kMaxChorusVoices> drift{};        // each voice's drift, in cycles
    std::array<double, kMaxChorusVoices> gainL{}, gainR{}; // ramped
    // Each voice's delay in samples, worked out every kSweepStep samples like the flanger's.
    std::array<double, kMaxChorusVoices> delay{};
    bool delayKnown = false;
    ChorusReads reads; // where the voices read
    // While a jump crossfades: where they read before it and each voice's delay there, and the samples the crossfade
    // has left of its fadeLength.
    ChorusReads fadeReads;
    std::array<double, kMaxChorusVoices> fadeDelay{};
    uint32_t fadeLeft = 0, fadeLength = 0;

    // Allocates the lines. Main thread, while the plugin is inactive.
    void prepare(double sampleRate);
    void start(const Context &ctx);
    // Returns the loudest sample it put out or took in, so its feedback keeps it running.
    double run(const Context &ctx, float *const *io, uint32_t n);
    // Its sweep going round `cycles` while the effects go by idle, worked out again where it is then.
    void idle(double cycles);
};

} // namespace fx
} // namespace fmvibe
