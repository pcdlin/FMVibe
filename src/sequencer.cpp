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
    position_ = -1.0f;
    forgetCurves();
}

void Sequencer::configure(const ParamValues &params, bool changed) {
    params_ = &params;
    enabled_ = params[paramIndex(SeqParam::Enabled)] >= 0.5;
    if (!enabled_) { // nothing more to read: render() only ends what it holds
        keyRunning_ = false;
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
    root_ = stepped(params[paramIndex(SeqParam::Root)], 127);
    if (!enabled_ || play_ == SeqPlay::Host || (play_ == SeqPlay::Key && keyCount_ == 0))
        keyRunning_ = false;
    if (!running_)
        pattern_ = patternWanted_;
    if (changed || read_ != pattern_)
        readPattern(pattern_);
}

void Sequencer::readPattern(int pattern) {
    noteCount_ = 0;
    read_ = params_ ? pattern : -1;
    if (!params_)
        return;
    const ParamValues &p = *params_;
    const int steps = kSeqStepsPerBar * std::clamp(stepped(p[paramIndex(pattern, SeqPatternParam::Length)], kSeqBars), 1, kSeqBars);
    curvePeriod_ = steps;
    loopStart_ = std::clamp(stepped(p[paramIndex(pattern, SeqPatternParam::LoopStart)], kSeqMaxSteps), 0, steps - 1);
    const int end = stepped(p[paramIndex(pattern, SeqPatternParam::LoopEnd)], kSeqMaxSteps);
    loopEnd_ = end <= 0 ? steps : std::clamp(end, loopStart_ + 1, steps);
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
    }
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
    if (play_ == SeqPlay::Host) {
        run = playing && std::isfinite(songBeats);
        from = run ? songBeats * kSeqStepsPerBeat[rate_] : 0.0;
    } else {
        if (keyStarted_) { // a key started it again: its clock from the start, and what it played ends
            keyStarted_ = false;
            keyClock_ = 0.0;
            running_ = false;
            restarted = true;
        }
        run = keyRunning_;
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
    // Another pattern picked: it takes over where the loop comes round (at once, if it wasn't running), planning stopping
    // there so the next call plans the new one.
    if (pattern_ != patternWanted_) {
        const double length = std::max(loopLength(), 1);
        const double phase = from - std::floor(from / length) * length;
        if (!running_ || phase < 1e-9 || length - phase <= perFrame) {
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

    // The notes starting in this stretch, in order.
    struct Start {
        double at, end;
        int key, pitch;
        double velocity;
        bool slide;
    };
    std::array<Start, kMaxSounding> starts;
    int startCount = 0;
    if (length > 0 && noteCount_ > 0) {
        const int shift = shift_ + (play_ != SeqPlay::Host && transposeKey_ >= 0 ? transposeKey_ - root_ : 0);
        for (int64_t k = floorStep(from) - 2; k <= floorStep(to) && startCount < kMaxSounding; ++k) {
            const int step = sourceStep(k);
            for (int i = 0; i < noteCount_ && startCount < kMaxSounding; ++i) {
                const Note &n = notes_[i];
                if (n.start < step || n.start >= step + 1)
                    continue;
                if (unit(k, n.index, 0) >= n.chance)
                    continue;
                const double t = static_cast<double>(k) + (n.start - step);
                const double delay = humanize_ * kHumanizeDelay * unit(k, n.index, 1);
                const double at = swung(t) + delay;
                if (at < from || at >= to)
                    continue;
                const int key = n.pitch + shift;
                if (key < 0 || key > 127)
                    continue;
                const double end = std::max(swung(t + std::max(n.length * gate_, kShortest)) + delay, at + 1e-6);
                double velocity = (1.0 - velocityAmount_) * kPlainVelocity + velocityAmount_ * n.velocity;
                velocity *= 1.0 + humanize_ * kHumanizeVelocity * (2.0 * unit(k, n.index, 2) - 1.0);
                starts[startCount++] = {at, end, key, n.pitch, std::clamp(velocity, 1.0 / 127.0, 1.0), n.slide};
            }
        }
        std::stable_sort(starts.begin(), starts.begin() + startCount,
                         [](const Start &a, const Start &b) { return a.at < b.at; });
    }

    auto frameAt = [&](double t) {
        const double f = std::floor((t - from) / perFrame);
        return static_cast<uint32_t>(std::clamp(f, 0.0, static_cast<double>(covered - 1)));
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
        out[count++] = {frame, true, s.key, s.velocity, id};
        // Slides waiting for the next note, which this is: they end as it starts. One it starts within ends at its own
        // end, as written, rather than a step after.
        for (int i = soundingCount_; i-- > 0;) {
            if (!sounding_[i].slide)
                continue;
            if (sounding_[i].end <= s.at) {
                count = pushOff(out, count, frame, sounding_[i]);
                remove(i);
            } else {
                sounding_[i].off = sounding_[i].end;
                sounding_[i].slide = false;
            }
        }
        sounding_[soundingCount_++] = {id, s.key, s.pitch, s.end, s.slide ? s.end + 1.0 : s.end, s.slide};
    }

    next_ = to;
    running_ = true;
    if (play_ != SeqPlay::Host)
        keyClock_ = to;
    // The playhead along the step played, from its end back to its start where the pattern runs backward (Reverse, and
    // Ping-Pong on its way back), so it moves on smoothly either way.
    position_ = static_cast<float>(patternPlace(to));
    return count;
}

} // namespace fmvibe
