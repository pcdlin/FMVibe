#include "sequencer.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace fmvibe {

namespace {

int stepped(double value, int last) {
    return std::isfinite(value) ? std::clamp(static_cast<int>(std::lround(value)), 0, last) : 0;
}

int64_t floorStep(double t) { return static_cast<int64_t>(std::floor(t)); }

int64_t wrap(int64_t step, int64_t length) {
    const int64_t m = step % length;
    return m < 0 ? m + length : m;
}

// A new note at its default velocity: what Velocity at 0 % plays every note at.
constexpr double kPlainVelocity = 100.0 / 127.0;
// The shortest note it plays, in steps, whatever the gate.
constexpr double kShortest = 1.0 / 64.0;
// How far Humanize at full delays a note, in steps, and moves its velocity either way, as a share of it.
constexpr double kHumanizeDelay = 0.25;
constexpr double kHumanizeVelocity = 0.3;

} // namespace

void Sequencer::reset() {
    soundingCount_ = 0;
    running_ = false;
    keyCount_ = 0;
    transposeKey_ = -1;
    keyRunning_ = keyStarted_ = false;
    keyClock_ = 0.0;
    previewing_ = false;
    previewFrom_ = -1.0;
    startFrom_ = 0.0;
    hostShift_ = 0.0;
    position_ = -1.0f;
    forgetCurves();
}

void Sequencer::preview(bool on, double from) {
    if (on == previewing_ || (on && !enabled_))
        return;
    previewing_ = on;
    previewFrom_ = on && std::isfinite(from) ? from : -1.0;
    // Either way it starts over: on, from the loop's start, its own clock; off, as Play says, which a key held to start
    // it must be pressed again for (the synth lets the keys it took go as they stop being taken).
    keyCount_ = 0;
    transposeKey_ = -1;
    keyRunning_ = false;
    keyStarted_ = on;
}

void Sequencer::configure(const ParamValues &params, bool changed) {
    params_ = &params;
    enabled_ = params[paramIndex(SeqParam::Enabled)] >= 0.5;
    if (!enabled_) { // nothing more to read: render() only ends what it holds
        keyRunning_ = false;
        previewing_ = false;
        running_ = false;
        noteCount_ = 0;
        read_ = -1; // read again once it is on
        forgetCurves();
        return;
    }
    play_ = static_cast<SeqPlay>(stepped(params[paramIndex(SeqParam::Play)], static_cast<int>(SeqPlay::Count) - 1));
    patternWanted_ = stepped(params[paramIndex(SeqParam::Pattern)], kSeqPatterns - 1);
    rate_ = stepped(params[paramIndex(SeqParam::Rate)], static_cast<int>(SeqRate::Count) - 1);
    auto real = [&](SeqParam p, double lo, double hi, double def) {
        const double v = params[paramIndex(p)];
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };
    swing_ = real(SeqParam::Swing, 0.5, 0.75, 0.5);
    gate_ = real(SeqParam::Gate, 0.05, 2.0, 1.0);
    velocityAmount_ = real(SeqParam::Velocity, 0.0, 1.0, 1.0);
    humanize_ = real(SeqParam::Humanize, 0.0, 1.0, 0.0);
    shift_ = 12 * static_cast<int>(std::lround(real(SeqParam::Octave, -3.0, 3.0, 0.0))) +
             static_cast<int>(std::lround(real(SeqParam::Transpose, -24.0, 24.0, 0.0)));
    order_ = static_cast<SeqOrder>(stepped(params[paramIndex(SeqParam::Order)], static_cast<int>(SeqOrder::Count) - 1));
    if (!enabled_ || play_ == SeqPlay::Host || (play_ == SeqPlay::Key && keyCount_ == 0))
        keyRunning_ = false;
    if (!running_)
        pattern_ = patternWanted_;
    if (changed || read_ != pattern_)
        readPattern(pattern_);
}

void Sequencer::setFeel(const std::array<double, kSeqFeelCount> &feel) {
    swing_ = feel[0];
    gate_ = feel[1];
    velocityAmount_ = feel[2];
    humanize_ = feel[3];
    patternWanted_ = std::clamp(static_cast<int>(std::lround(feel[4])), 0, kSeqPatterns - 1);
    if (enabled_ && !running_ && pattern_ != patternWanted_) {
        pattern_ = patternWanted_;
        readPattern(pattern_);
    }
}

void Sequencer::readPattern(int pattern) {
    noteCount_ = 0;
    const bool same = read_ == pattern;
    read_ = params_ ? pattern : -1;
    if (!params_)
        return;
    const ParamValues &p = *params_;
    const int steps = kSeqStepsPerBar * std::clamp(stepped(p[paramIndex(pattern, SeqPatternParam::Length)], kSeqBars), 1, kSeqBars);
    curvePeriod_ = steps;
    const int oldStart = loopStart_, oldEnd = loopEnd_;
    loopStart_ = std::clamp(stepped(p[paramIndex(pattern, SeqPatternParam::LoopStart)], kSeqMaxSteps), 0, steps - 1);
    const int end = stepped(p[paramIndex(pattern, SeqPatternParam::LoopEnd)], kSeqMaxSteps);
    loopEnd_ = end <= 0 ? steps : std::clamp(end, loopStart_ + 1, steps);
    if (running_ && same && (loopStart_ != oldStart || loopEnd_ != oldEnd))
        keepPlace(oldStart, oldEnd);
    for (int n = 0; n < kSeqNotes; ++n) {
        const double length = p[seqNoteIndex(pattern, n, SeqNoteParam::Length)];
        if (!(length > 0.0) || p[seqNoteIndex(pattern, n, SeqNoteParam::Mute)] >= 0.5)
            continue;
        const double start = p[seqNoteIndex(pattern, n, SeqNoteParam::Start)];
        if (!std::isfinite(start) || start < loopStart_ || start >= loopEnd_)
            continue; // outside the loop: not played
        const double velocity = p[seqNoteIndex(pattern, n, SeqNoteParam::Velocity)];
        const double chance = p[seqNoteIndex(pattern, n, SeqNoteParam::Chance)];
        notes_[noteCount_++] = {start,
                                std::min(length, static_cast<double>(kSeqMaxSteps)),
                                std::isfinite(velocity) ? std::clamp(velocity, 0.0, 1.0) : kPlainVelocity,
                                std::isfinite(chance) ? std::clamp(chance, 0.0, 1.0) : 1.0,
                                stepped(p[seqNoteIndex(pattern, n, SeqNoteParam::Pitch)], 127),
                                p[seqNoteIndex(pattern, n, SeqNoteParam::Slide)] >= 0.5,
                                n};
        // Its bend, its points held to their ranges and in order.
        const int points = seqNoteBendPointCount([&](int index) { return p[index]; }, pattern, n);
        double last = 0.0;
        for (int i = 0; i < points; ++i) {
            auto field = [&](PointField f, double lo, double hi) {
                const double v = p[seqNoteBendIndex(pattern, n, i, f)];
                return std::isfinite(v) ? std::clamp(v, lo, hi) : std::clamp(0.0, lo, hi);
            };
            last = std::max(last, field(PointField::X, 0.0, kSeqMaxSteps));
            notePoints_[static_cast<size_t>(n)][static_cast<size_t>(i)] = {
                last, field(PointField::Y, -kSeqMaxNoteBend, kSeqMaxNoteBend), field(PointField::Curve, -1.0, 1.0)};
        }
        notePointCount_[static_cast<size_t>(n)] = static_cast<uint8_t>(points);
    }
    // In order of their starts, so that plan() finds a step's notes at once; and the next plan() chases them, as they
    // may have changed under the playhead.
    std::stable_sort(notes_.begin(), notes_.begin() + noteCount_,
                     [](const Note &a, const Note &b) { return a.start < b.start; });
    longest_ = 0.0;
    for (int i = 0, s = 0; s <= kSeqMaxSteps; ++s) {
        while (i < noteCount_ && notes_[i].start < s)
            longest_ = std::max(longest_, notes_[i++].length);
        stepFirst_[static_cast<size_t>(s)] = i;
    }
    chase_ = true;
    // Its curves, their points held to their ranges and in order.
    auto get = [&](int index) { return p[index]; };
    for (int c = 0; c < kSeqCurves; ++c) {
        const SeqCurve curve = static_cast<SeqCurve>(c);
        const int count = seqCurvePointCount(get, pattern, curve);
        double last = 0.0;
        for (int i = 0; i < count; ++i) {
            auto field = [&](PointField f, double lo, double hi) {
                const double v = p[seqCurveIndex(pattern, curve, i, f)];
                return std::isfinite(v) ? std::clamp(v, lo, hi) : lo;
            };
            last = std::max(last, field(PointField::X, 0.0, kSeqMaxSteps));
            curves_[static_cast<size_t>(c)][static_cast<size_t>(i)] = {
                last, field(PointField::Y, curve == SeqCurve::Bend ? -1.0 : 0.0, 1.0), field(PointField::Curve, -1.0, 1.0)};
        }
        curveCount_[static_cast<size_t>(c)] = count;
    }
}

void Sequencer::keepPlace(int oldStart, int oldEnd) {
    if (order_ == SeqOrder::Random) // its steps follow no place in the loop
        return;
    // The step the next stretch starts in, and the pattern's step it played there, the old loop's way.
    const int64_t k = floorStep(next_);
    const int newStart = loopStart_, newEnd = loopEnd_;
    loopStart_ = oldStart;
    loopEnd_ = oldEnd;
    const int step = sourceStep(k);
    const bool back = backward(k);
    loopStart_ = newStart;
    loopEnd_ = newEnd;
    // Where in the new loop's cycle that step comes (out of the loop, where it falls in it), going the same way.
    const int64_t length = std::max(loopLength(), 1);
    const int64_t into = wrap(step - loopStart_, length);
    int64_t period = length, target = into;
    if (order_ == SeqOrder::Reverse) {
        target = length - 1 - into;
    } else if (order_ == SeqOrder::PingPong && length > 1) {
        period = 2 * length;
        target = back ? period - 1 - into : into;
    }
    int64_t d = target - wrap(k, period);
    // An even shift where an odd period allows one, the shorter way: Swing's pairs of steps stay where they were.
    if (d % 2 != 0 && period % 2 != 0)
        d += d < 0 ? period : -period;
    if (d == 0)
        return;
    const double shift = static_cast<double>(d);
    next_ += shift;
    keyClock_ += shift;
    hostShift_ += shift;
    chaseFrom_ += shift;
    for (int i = 0; i < soundingCount_; ++i) {
        Sounding &n = sounding_[i];
        n.step += d;
        n.begin += shift;
        n.end += shift;
        n.off += shift;
    }
}

double Sequencer::bendOf(const Sounding &s, double t) const {
    if (s.pattern != read_) // from a pattern another took over from: it stays where it was
        return s.bent;
    const auto &points = notePoints_[static_cast<size_t>(s.index)];
    return seqNoteBendAt(notePointCount_[static_cast<size_t>(s.index)],
                         [&](int i) { return points[static_cast<size_t>(i)]; },
                         std::clamp(t, s.begin, s.end) - s.begin);
}

double Sequencer::patternPlace(double t) const {
    const int64_t at = floorStep(t);
    const double into = t - static_cast<double>(at);
    return sourceStep(at) + (backward(at) ? 1.0 - into : into);
}

void Sequencer::keyDown(int key) {
    for (int i = 0; i < keyCount_; ++i)
        if (keys_[i] == key) {
            std::copy(keys_.begin() + i + 1, keys_.begin() + keyCount_, keys_.begin() + i);
            --keyCount_;
            break;
        }
    if (keyCount_ == static_cast<int>(keys_.size())) { // forget the oldest
        std::copy(keys_.begin() + 1, keys_.end(), keys_.begin());
        --keyCount_;
    }
    keys_[keyCount_++] = key;
    transposeKey_ = key;
    if (!keyRunning_) {
        keyRunning_ = true;
        keyStarted_ = true;
    }
}

void Sequencer::keyUp(int key) {
    for (int i = 0; i < keyCount_; ++i)
        if (keys_[i] == key) {
            std::copy(keys_.begin() + i + 1, keys_.begin() + keyCount_, keys_.begin() + i);
            --keyCount_;
            break;
        }
    if (keyCount_ > 0)
        transposeKey_ = keys_[keyCount_ - 1];
    else if (play_ != SeqPlay::Latch)
        keyRunning_ = false;
}

int Sequencer::sourceStep(int64_t step) const {
    const int64_t length = std::max(loopLength(), 1);
    switch (order_) {
    case SeqOrder::Reverse:
        return loopStart_ + static_cast<int>(length - 1 - wrap(step, length));
    case SeqOrder::PingPong: {
        if (length == 1)
            return loopStart_;
        // The end steps play twice at each turn, so the pattern keeps its meter (a loop of L steps takes 2L).
        const int64_t period = 2 * length;
        const int64_t i = wrap(step, period);
        return loopStart_ + static_cast<int>(i < length ? i : period - 1 - i);
    }
    case SeqOrder::Random:
        return loopStart_ + std::min(static_cast<int>(unit(step, -1, 3) * static_cast<double>(length)),
                                     static_cast<int>(length) - 1);
    default:
        return loopStart_ + static_cast<int>(wrap(step, length));
    }
}

bool Sequencer::backward(int64_t step) const {
    const int64_t length = std::max(loopLength(), 1);
    if (order_ == SeqOrder::Reverse)
        return true;
    if (order_ == SeqOrder::PingPong && length > 1)
        return wrap(step, 2 * length) >= length;
    return false;
}

double Sequencer::swung(double t) const {
    const double pair = 2.0 * std::floor(t * 0.5);
    const double x = t - pair;
    return x < 1.0 ? pair + x * 2.0 * swing_ : pair + 2.0 * swing_ + (x - 1.0) * (2.0 - 2.0 * swing_);
}

double Sequencer::unit(int64_t step, int note, int salt) {
    uint64_t x = static_cast<uint64_t>(step) * 0x9E3779B97F4A7C15ull ^
                 static_cast<uint64_t>(note + 7) * 0xC2B2AE3D27D4EB4Full ^
                 static_cast<uint64_t>(salt + 1) * 0x165667B19E3779F9ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0);
}

int Sequencer::pushOff(std::span<SeqEvent> out, int count, uint32_t frame, const Sounding &s) const {
    if (count < static_cast<int>(out.size()))
        out[count++] = {frame, false, s.key, 0.0, s.id};
    return count;
}

int Sequencer::stopAll(std::span<SeqEvent> out) {
    int count = 0;
    for (int i = 0; i < soundingCount_; ++i)
        count = pushOff(out, count, 0, sounding_[i]);
    soundingCount_ = 0;
    return count;
}

int Sequencer::plan(uint32_t frames, double tempo, bool playing, double songBeats, std::span<SeqEvent> out,
                    uint32_t &covered) {
    covered = frames;
    bendCount_ = 0;
    if (!enabled_ || frames == 0) {
        running_ = false;
        position_ = -1.0f;
        forgetCurves();
        return 0;
    }
    const double perFrame = std::max(tempo, 1.0) / 60.0 * kSeqStepsPerBeat[rate_] / sampleRate_;
    bool run;
    bool restarted = false;
    double from;
    const bool hostClock = play_ == SeqPlay::Host && !previewing_;
    if (hostClock) {
        run = playing && std::isfinite(songBeats);
        from = run ? songBeats * kSeqStepsPerBeat[rate_] + hostShift_ : 0.0;
    } else {
        if (keyStarted_) { // a key or the Play button started it again: its clock from the start, and what it played ends
            keyStarted_ = false;
            keyClock_ = 0.0;
            running_ = false;
            restarted = true;
            if (previewing_ && previewFrom_ >= 0.0) { // the button, from a step: the clock where its cycle plays it
                const double at = previewFrom_;
                previewFrom_ = -1.0;
                const int length = std::max(loopLength(), 1);
                if (at >= loopStart_ && at < loopEnd_ && order_ != SeqOrder::Random) {
                    const int step = static_cast<int>(std::floor(at));
                    const int into = step - loopStart_;
                    const int index = order_ == SeqOrder::Reverse ? length - 1 - into : into;
                    keyClock_ = swung(index + (at - step));
                    startFrom_ = index + (at - step);
                }
            }
        }
        run = keyRunning_ || previewing_;
        from = keyClock_;
    }
    int count = restarted ? stopAll(out) : 0;
    if (!run) {
        count += stopAll(out.subspan(static_cast<size_t>(count)));
        running_ = false;
        position_ = -1.0f;
        forgetCurves();
        if (pattern_ != patternWanted_) {
            pattern_ = patternWanted_;
            readPattern(pattern_);
        }
        return count;
    }
    if (running_ && std::abs(from - next_) > 1e-3) { // the transport jumped, or the rate changed: start over there
        count += stopAll(out.subspan(static_cast<size_t>(count)));
        running_ = false;
    } else if (running_) {
        // A hair off where the last stretch ended, as a host's position rounds (one handed in seconds and turned into
        // beats, say): it carries on from there, so a note starting on the seam starts once, neither falling between
        // the two stretches nor in both.
        from = next_;
    }
    if (!running_) { // starting: the notes the playhead is in the middle of play from here (from a step, none before it)
        chase_ = true;
        chaseFrom_ = startFrom_;
        startFrom_ = 0.0;
        if (hostClock) { // locked to the host's bars again
            from -= hostShift_;
            hostShift_ = 0.0;
        }
    }
    // Another pattern picked: it takes over where the loop comes round (at once, if it wasn't running), planning stopping
    // there so the next call plans the new one.
    if (pattern_ != patternWanted_) {
        const double length = std::max(loopLength(), 1);
        const double phase = from - std::floor(from / length) * length;
        if (!running_ || phase < 1e-9 || length - phase <= perFrame) {
            if (running_) // taking over as the loop comes round: none of its notes started before
                chaseFrom_ = from;
            pattern_ = patternWanted_;
            readPattern(pattern_);
        } else {
            covered = std::min(frames, static_cast<uint32_t>(std::floor((length - phase) / perFrame)));
            covered = std::max(covered, 1u);
        }
    }
    const double to = from + covered * perFrame;
    const int length = loopLength();
    // The curves where the stretch starts.
    const double place = patternPlace(from);
    for (size_t c = 0; c < curveNow_.size(); ++c)
        curveNow_[c] = seqCurveAt(curveCount_[c], [&](int i) { return curves_[c][static_cast<size_t>(i)]; }, place,
                                  static_cast<double>(curvePeriod_));

    // The notes starting in this stretch, in order; and where it chases them, first, those it is in the middle of that
    // aren't sounding (not those left out by their chance or their key), from the stretch's start.
    struct Start {
        double at, begin, end; // begin: where it started, before `at` for a note chased
        int64_t step;
        int index, key, pitch;
        double velocity;
        bool slide;
    };
    std::array<Start, kMaxSounding> starts;
    int startCount = 0;
    const bool chase = chase_ && soundingCount_ < kMaxSounding; // full, it would only take a note's place
    chase_ = false;
    if (length > 0 && noteCount_ > 0) {
        const int shift = shift_ + (!hostClock && !previewing_ && transposeKey_ >= 0 ? transposeKey_ - kSeqTransposeFrom : 0);
        auto sounding = [&](int64_t k, int index) {
            for (int i = 0; i < soundingCount_; ++i)
                if (sounding_[i].step == k && sounding_[i].index == index)
                    return true;
            return false;
        };
        // Back as far as a note started that may still be playing: its length, gated, and swung and humanized.
        const int64_t back = chase ? static_cast<int64_t>(std::ceil(longest_ * gate_)) + 1 : 0;
        for (int64_t k = floorStep(from) - 2 - back; k <= floorStep(to) && startCount < kMaxSounding; ++k) {
            const int step = sourceStep(k);
            const int last = stepFirst_[static_cast<size_t>(step) + 1];
            for (int i = stepFirst_[static_cast<size_t>(step)]; i < last && startCount < kMaxSounding; ++i) {
                const Note &n = notes_[i];
                if (unit(k, n.index, 0) >= n.chance)
                    continue;
                const double t = static_cast<double>(k) + (n.start - step);
                const double delay = humanize_ * kHumanizeDelay * unit(k, n.index, 1);
                const double at = swung(t) + delay;
                if (at >= to)
                    continue;
                const bool late = at < from; // started before the stretch: only chased
                if (late && (!chase || t < chaseFrom_ - 1e-9))
                    continue;
                const int key = n.pitch + shift;
                if (key < 0 || key > 127)
                    continue;
                const double end = std::max(swung(t + std::max(n.length * gate_, kShortest)) + delay, at + 1e-6);
                if (late && (end <= from || sounding(k, n.index)))
                    continue;
                double velocity = (1.0 - velocityAmount_) * kPlainVelocity + velocityAmount_ * n.velocity;
                velocity *= 1.0 + humanize_ * kHumanizeVelocity * (2.0 * unit(k, n.index, 2) - 1.0);
                starts[startCount++] = {late ? from : at, at, end, k, n.index, key, n.pitch,
                                        std::clamp(velocity, 1.0 / 127.0, 1.0), n.slide};
            }
        }
        std::stable_sort(starts.begin(), starts.begin() + startCount,
                         [](const Start &a, const Start &b) { return a.at < b.at; });
    }

    auto frameAt = [&](double t) {
        const double f = std::floor((t - from) / perFrame);
        return static_cast<uint32_t>(std::clamp(f, 0.0, static_cast<double>(covered - 1)));
    };
    // A bent note's bend over the stretch, from where it starts in it (or the stretch's start) to the stretch's end,
    // and where it was there, from which the next stretch carries on.
    auto bend = [&](Sounding &s) {
        // One from a pattern another took over from holds where its last stretch took it.
        if (bendCount_ == kMaxSounding || s.pattern != read_ || notePointCount_[static_cast<size_t>(s.index)] == 0)
            return;
        const double start = std::max(from, s.begin);
        bends_[static_cast<size_t>(bendCount_++)] = {s.id, start > from ? frameAt(start) : 0u, bendOf(s, start),
                                                     bendOf(s, to)};
        s.bent = bendOf(s, to);
    };
    auto remove = [&](int i) {
        std::copy(sounding_.begin() + i + 1, sounding_.begin() + soundingCount_, sounding_.begin() + i);
        --soundingCount_;
    };
    // Ends and starts in time order: the notes held ending before (or as) the next one starts, then that one, then the
    // slides it follows; then those ending before the stretch does.
    int next = 0;
    while (count < static_cast<int>(out.size())) {
        const double nextStart = next < startCount ? starts[next].at : to;
        int ending = -1;
        for (int i = 0; i < soundingCount_; ++i)
            if (sounding_[i].off <= nextStart && sounding_[i].off < to &&
                (ending < 0 || sounding_[i].off < sounding_[ending].off))
                ending = i;
        if (ending >= 0) {
            bend(sounding_[ending]);
            count = pushOff(out, count, frameAt(sounding_[ending].off), sounding_[ending]);
            remove(ending);
            continue;
        }
        if (next >= startCount)
            break;
        const Start &s = starts[next++];
        const uint32_t frame = frameAt(s.at);
        if (soundingCount_ == kMaxSounding) {
            count = pushOff(out, count, frame, sounding_[0]);
            remove(0);
        }
        if (count >= static_cast<int>(out.size()))
            break;
        const int32_t id = nextId_;
        nextId_ = nextId_ == INT32_MAX ? 1 : nextId_ + 1;
        SeqEvent &on = out[count++];
        on = {frame, true, s.key, s.velocity, id};
        // Slides waiting for the next note, which this is: they end as it starts. One it starts within ends at its own
        // end, as written, rather than a step after.
        for (int i = soundingCount_; i-- > 0;) {
            if (!sounding_[i].slide)
                continue;
            if (sounding_[i].end <= s.at) {
                bend(sounding_[i]);
                count = pushOff(out, count, frame, sounding_[i]);
                remove(i);
            } else {
                sounding_[i].off = sounding_[i].end;
                sounding_[i].slide = false;
            }
        }
        Sounding &started = sounding_[soundingCount_++];
        started = {s.step, s.index, id, s.key, s.pitch, s.begin, s.end, s.slide ? s.end + 1.0 : s.end, s.slide, read_, 0.0};
        on.bend = started.bent = bendOf(started, s.at);
    }
    for (int i = 0; i < soundingCount_; ++i)
        bend(sounding_[i]);

    next_ = to;
    running_ = true;
    if (!hostClock)
        keyClock_ = to;
    // The playhead along the step played, from its end back to its start where the pattern runs backward (Reverse, and
    // Ping-Pong on its way back), so it moves on smoothly either way.
    position_ = static_cast<float>(patternPlace(to));
    return count;
}

} // namespace fmvibe
