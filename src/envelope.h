#pragma once

#include "params.h"

#include <algorithm>
#include <cmath>

namespace fmvibe {

// One attack, decay or release segment: its phase step per sample and its
// slope (see envProgress), with the per-sample factor and scale that let the
// envelope follow the curve with a multiply instead of an exp.
struct EnvSegment {
    double dt;   // phase step per sample; the segment lasts 1 / dt samples
    double k;    // steepness
    double step; // exp(-k * dt)
    double norm; // 1 / (1 - exp(-k))
};

// Per-sample values for one Envelope, derived from params.
struct EnvRates {
    EnvSegment attack;
    double hold; // samples at full level after the attack; 0 for none
    EnvSegment decay;
    EnvSegment release;
    double sustain;
    double followCoef; // after the decay, how the level follows sustain changes
};

// A segment of no time (0 ms) takes one sample.
inline EnvSegment envSegment(double seconds, double curve, double sampleRate) {
    const double dt = seconds > 0.0 ? 1.0 / (seconds * sampleRate) : 1.0;
    const double k = curve * kCurveSteepness;
    const bool straight = std::abs(k) < kLinearSteepness;
    return {dt, k, std::exp(-k * dt), straight ? 1.0 : -1.0 / std::expm1(-k)};
}

// Attack, hold, decay and release are normalized time params (see
// timeFromNorm; the attack, decay and release may be 0 ms, see envTimeFromNorm), and each segment arrives in its
// time; a hold of 0 is none. Curves are -1..1 (see envCurve). After the decay, the level follows sustain changes at
// the decay's pace, but over 1 ms at least: the sustain moves once a control block, and a 0 ms decay would step
// along with it. `speed` divides every time (an operator's rate scaling, see keyRateScale()); at 1 they are as set.
inline EnvRates envRates(double attack, double hold, double decay, double sustain, double release,
                         double attackCurve, double decayCurve, double releaseCurve, double sampleRate,
                         double speed = 1.0) {
    constexpr double kLn1000 = 6.907755278982137;
    const double decaySeconds = envTimeFromNorm(decay) / speed;
    return {
        envSegment(envTimeFromNorm(attack) / speed, attackCurve, sampleRate),
        hold > 0.0 ? timeFromNorm(hold) / speed * sampleRate : 0.0,
        envSegment(decaySeconds, decayCurve, sampleRate),
        envSegment(envTimeFromNorm(release) / speed, releaseCurve, sampleRate),
        sustain,
        std::exp(-kLn1000 / (std::max(decaySeconds, kMinTime) * sampleRate)),
    };
}

// AHDSR whose attack, decay and release can each be bent. Once the decay has
// arrived, the level keeps following the sustain param, so sustain changes
// while a note is held don't click. A restart rises from the current level at
// the attack's own pace, as if the attack had already come that far.
class Envelope {
public:
    void start() {
        stage_ = Stage::Attack;
        t_ = -1.0; // placed on the curve at the first sample, once the slope is known
    }
    void release() {
        if (stage_ == Stage::Idle)
            return;
        stage_ = Stage::Release;
        enterSegment();
    }
    void stop() {
        stage_ = Stage::Idle;
        level_ = 0.0;
    }

    bool idle() const { return stage_ == Stage::Idle; }
    // Still on its way up (attack or hold): it will be louder than it is now.
    bool rising() const { return stage_ == Stage::Attack || stage_ == Stage::Hold; }
    double level() const { return level_; }
    // Where it is, for the editor: its stage (0 attack, 1 hold, 2 decay, 3 sustain, 4 release) plus how far
    // along that stage (0..1; the sustain, which has no end, at 0); -1 when idle.
    double position(const EnvRates &r) const {
        switch (stage_) {
        case Stage::Attack:
            return std::clamp(t_, 0.0, 1.0);
        case Stage::Hold:
            return 1.0 + (r.hold > 0.0 ? std::min(held_ / r.hold, 1.0) : 1.0);
        case Stage::Decay:
            return 2.0 + std::clamp(t_, 0.0, 1.0);
        case Stage::Sustain:
            return 3.0;
        case Stage::Release:
            return 4.0 + std::clamp(t_, 0.0, 1.0);
        case Stage::Idle:
            break;
        }
        return -1.0;
    }

    double next(const EnvRates &r) {
        if (stage_ == Stage::Sustain) { // where a held note spends most of its time: short, so it can be inlined
            level_ = r.sustain + (level_ - r.sustain) * r.followCoef;
            return level_;
        }
        return nextStage(r);
    }

    // `n` levels in a row into `out`, as n calls of next() give them, for a voice's block. Each stage runs as a loop
    // of its own: the sustain, where a held note spends most of its time, and the decay and release, which take
    // most of the rest; the sustain and idle stages last until release() or start(), which no block calls.
    void run(const EnvRates &r, double *out, int n) {
        int s = 0;
        while (s < n) {
            switch (stage_) {
            case Stage::Sustain:
                for (; s < n; ++s)
                    out[s] = level_ = r.sustain + (level_ - r.sustain) * r.followCoef;
                break;
            case Stage::Idle: // at 0
                for (; s < n; ++s)
                    out[s] = level_;
                break;
            case Stage::Decay:
                while (s < n && stage_ == Stage::Decay)
                    out[s++] = decayStep(r);
                break;
            case Stage::Release:
                while (s < n && stage_ == Stage::Release)
                    out[s++] = releaseStep(r);
                break;
            case Stage::Attack:
            case Stage::Hold:
                out[s++] = nextStage(r);
                break;
            }
        }
    }

private:
    enum class Stage { Idle, Attack, Hold, Decay, Sustain, Release };

    double nextStage(const EnvRates &r) {
        switch (stage_) {
        case Stage::Attack:
            if (t_ < 0.0) {
                enterSegment();
                t_ = envProgressTime(level_, r.attack.k);
            }
            level_ = advance(r.attack);
            if (t_ >= 1.0) {
                level_ = 1.0;
                held_ = 0.0;
                stage_ = Stage::Hold;
            }
            break;
        case Stage::Hold:
            if (held_++ >= r.hold) {
                stage_ = Stage::Decay;
                enterSegment();
            }
            break;
        case Stage::Decay:
            return decayStep(r);
        case Stage::Release:
            return releaseStep(r);
        case Stage::Sustain: // see next()
        case Stage::Idle:
            break;
        }
        return level_;
    }

    double decayStep(const EnvRates &r) {
        level_ = r.sustain + (1.0 - r.sustain) * (1.0 - advance(r.decay));
        if (t_ >= 1.0)
            stage_ = Stage::Sustain;
        return level_;
    }

    double releaseStep(const EnvRates &r) {
        level_ = from_ * (1.0 - advance(r.release));
        if (t_ >= 1.0)
            stop();
        return level_;
    }

    void enterSegment() {
        from_ = level_;
        t_ = 0.0;
        resync_ = true;
    }

    // Moves one sample along the current segment; returns how far along it is (0..1).
    double advance(const EnvSegment &s) {
        t_ = std::min(t_ + s.dt, 1.0);
        if (std::abs(s.k) < kLinearSteepness) {
            resync_ = true; // e_ stands still meanwhile: bent again, even to the same slope, it starts over from t_
            return t_;
        }
        if (resync_ || s.k != k_) { // a new segment, or the slope changed
            k_ = s.k;
            resync_ = false;
            e_ = std::exp(-s.k * t_);
        } else {
            e_ *= s.step;
        }
        return std::clamp((1.0 - e_) * s.norm, 0.0, 1.0);
    }

    Stage stage_ = Stage::Idle;
    double level_ = 0.0;
    double from_ = 0.0; // level where the release started
    double t_ = 0.0;    // phase in the current segment, 0..1
    double held_ = 0.0; // samples spent in the hold
    double e_ = 1.0;    // exp(-k * t_)
    double k_ = 0.0;
    bool resync_ = true;
};

} // namespace fmvibe
