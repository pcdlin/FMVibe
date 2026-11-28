#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace fmvibe {

// An operator's Rate/Level envelope (see OpEnvMode::Rates): four rates and four levels, 0-99 each, run as the DX7 runs
// its envelopes, so a voice imported from one plays its envelopes as it did there, and a voice made here can be
// edited as on a DX7.
//
// From each note it goes from where it is (silence for a new voice) to L1 at R1, then to L2 at R2 and to L3 at R3,
// and stays at L3 while the key is down; let go, it goes from wherever it is to L4 at R4. A rate is a speed, not a
// time: how long a segment takes follows from how far its level moves. Its levels are on the DX7's level scale (0.75 dB
// a step from 20 up, steeper below), in units of 1/256 of a doubling of the gain, as the DX7 counts them, its full
// level 3840 of them. A falling segment moves its level evenly in those units (evenly in dB), doubling its speed every
// four of its 64 speeds; a rising one jumps up from far below first (to 1716 units, about -50 dB) and rises the slower
// the louder it gets, as the DX7's attacks do; one that stays at its level waits as long as its rate says. Rate
// Scaling speeds every rate up for higher keys: by the scaling times a third of the keys above A0, an eighth of that
// in the 64 speeds. A fraction of a rate, a level or the scaling (modulated) goes between its whole values' speeds.
//
// It steps once every DX7 envelope tick (64 samples at 44.1 kHz, 1.45 ms), as the DX7 does, and the gain goes
// between ticks in a straight line, as the DX7's does, so a fast attack is a ramp of a tick rather than a click,
// whatever the voices' rate. Once let go and at L4, it is done: where L4 is still heard, it fades out over 5 ms there
// (a DX7 voice would sound on until stolen), so its voice can end.
//
// start() and release() only note what happened, as DrawnEnvelope's do: prepare(), before each block's samples, takes
// them up with the block's rates and levels.
class RateEnvelope {
public:
    static constexpr double kTickSeconds = 64.0 / 44100.0;
    static constexpr double kFadeSeconds = 0.005;
    static constexpr double kFullUnits = 3840.0; // L99 at full output
    static constexpr double kFloorUnits = 16.0;  // the least it goes down to

    struct Settings {
        std::array<double, 4> rates{99.0, 99.0, 99.0, 99.0};
        std::array<double, 4> levels{99.0, 99.0, 99.0, 0.0};
        double scaling = 0.0; // the 64 speeds every rate is sped up by (see scalingSpeeds())
    };

    // An output or envelope level (0-99) on the DX7's level scale (0-127): 28 + the level from 20 up; below, steeper.
    static double scaledLevel(double level) {
        level = std::clamp(level, 0.0, 99.0);
        if (level >= 20.0)
            return 28.0 + level;
        return 46.0 * std::pow(level / 19.0, 0.7); // ours, near the DX7's (whose levels there are 60 dB and more down)
    }
    // Where a level (0-99) puts it, in units, at full output.
    static double levelUnits(double level) {
        return std::max(kFloorUnits, std::floor(scaledLevel(level) * 0.5) * 64.0 + 4064.0 - 4256.0);
    }
    // Its gain at `units`, 1 at full level.
    static double gainOf(double units) { return std::exp2((units - kFullUnits) / 256.0); }
    // One of the 64 speeds (fractional between them) for rate 0-99: (rate * 41) >> 6 at whole rates.
    static double rateSpeed(double rate) {
        rate = std::clamp(rate, 0.0, 99.0);
        const double low = std::floor(rate);
        auto at = [](double r) { return static_cast<double>((static_cast<int>(r) * 41) >> 6); };
        return low >= 99.0 ? at(99.0) : at(low) + (at(low + 1.0) - at(low)) * (rate - low);
    }
    // How far a speed (0-63) moves a falling segment a tick, in units: 4 to 7 eighths of a doubling for each four.
    static double tickUnits(double speed) {
        speed = std::clamp(speed, 0.0, 63.0);
        const double four = std::floor(speed / 4.0);
        return (4.0 + (speed - 4.0 * four)) * std::exp2(four - 8.0);
    }
    // How long a segment that stays at its level waits at rate 0-99 (and its scaling): 40 s at rate 0, halving about
    // every six rates, a little over 10 ms at 76, then 20 samples at 44.1 kHz a rate to 99.
    static double staticSeconds(double rate) {
        rate = std::clamp(rate, 0.0, 99.0);
        return rate < 77.0 ? 40.0 * std::exp2(-0.158 * rate) : 20.0 * (99.0 - rate) / 44100.0;
    }
    // The 64 speeds Rate Scaling (0-7) adds at `key` (MIDI): the scaling times a third of the keys above A0 (up to 31),
    // an eighth of that, (scaling * x) >> 3 at whole scalings.
    static double scalingSpeeds(double scaling, double key) {
        scaling = std::clamp(scaling, 0.0, 7.0);
        const int x = std::clamp(static_cast<int>(std::floor(key + 0.5)) / 3 - 7, 0, 31);
        const double low = std::floor(scaling);
        auto at = [x](double s) { return static_cast<double>((static_cast<int>(s) * x) >> 3); };
        return low >= 7.0 ? at(7.0) : at(low) + (at(low + 1.0) - at(low)) * (scaling - low);
    }

    void start() {
        startAsked_ = true;
        releaseAsked_ = false;
        held_ = true;
    }
    void release() {
        if (held_)
            releaseAsked_ = true;
        held_ = false;
    }
    void stop() {
        stage_ = Stage::Idle;
        units_ = 0.0;
        gain_ = gainTo_ = step_ = 0.0;
    }

    bool idle() const { return stage_ == Stage::Idle && !startAsked_; }
    bool held() const { return held_; }
    bool holding() const { return stage_ == Stage::Hold; } // at L3, the key down
    // On its way up to L1 (as an AHDSR's attack is, for voice stealing).
    bool rising() const { return stage_ == Stage::Play && segment_ == 0 && rising_; }
    double level() const { return gain_; }
    int segment() const { return segment_; } // 0-3 on its way to L1-L4, 4 once done (for its display)
    // Where it is, for its display's dot: on its way to L1, L2 or L3, the segment (0-2) and how far along it is (by
    // its level, or by its time where it waits), from 0 up to 3; held at L3, 3; let go, 4 and how far to L4 it is;
    // done, -1.
    double progress() const {
        if (stage_ == Stage::Hold)
            return 3.0;
        if (stage_ != Stage::Play)
            return -1.0;
        const double along = waits_ > 0.0                       ? 1.0 - waits_ / std::max(waitTotal_, 1e-9)
                             : std::abs(target_ - from_) < 1e-9 ? 1.0
                                                                : (units_ - from_) / (target_ - from_);
        return (segment_ == 3 ? 4.0 : static_cast<double>(segment_)) + std::clamp(along, 0.0, 1.0);
    }

    // The block's settings and the voices' rate; `from`, the level the operator is at, which a new note going on from
    // where an Envelope left it starts at.
    void prepare(const Settings &settings, double sampleRate, double from) {
        settings_ = settings;
        tickSamples_ = kTickSeconds * sampleRate;
        fadeStep_ = 1.0 / std::max(kFadeSeconds * sampleRate, 1.0);
        if (startAsked_) {
            startAsked_ = false;
            if (stage_ == Stage::Idle || stage_ == Stage::Fade) { // from where the operator is, on the DX7's scale
                gain_ = std::clamp(from, 0.0, 1.0);
                units_ = gain_ > 0.0 ? std::max(kFullUnits + 256.0 * std::log2(gain_), 0.0) : 0.0;
                ticks_ = 0.0;
            }
            stage_ = Stage::Play;
            enter(0);
        }
        if (releaseAsked_) {
            releaseAsked_ = false;
            if (stage_ != Stage::Idle && stage_ != Stage::Fade)
                enter(3);
        }
        // Its rates, levels and scaling may move under it (modulated, or turned while a note plays): the segment it is
        // on goes on at its rate and toward its level as they are now, a wait for as much of its time as is left;
        // held at L3, it follows L3 there at R3.
        if (stage_ == Stage::Play) {
            target_ = levelUnits(settings_.levels[segment_]);
            speed_ = segmentSpeed(segment_);
            if (waitTotal_ > 0.0) {
                const double total = waitTicks(segment_);
                if (total != waitTotal_) {
                    waits_ *= total / waitTotal_;
                    waitTotal_ = total;
                }
            } else {
                rising_ = target_ > units_;
            }
        } else if (stage_ == Stage::Hold) {
            target_ = levelUnits(settings_.levels[2]);
            speed_ = segmentSpeed(2);
        }
    }

    double next() {
        switch (stage_) {
        case Stage::Idle:
            return 0.0;
        case Stage::Fade:
            gain_ -= fadeFrom_ * fadeStep_;
            if (gain_ <= 0.0)
                stop();
            return gain_;
        default:
            break;
        }
        if (--ticks_ <= 0.0) { // a tick: the level moves on, and the gain ramps to it over the tick
            ticks_ += tickSamples_;
            tick();
            if (stage_ == Stage::Idle || stage_ == Stage::Fade)
                return gain_;
            gainTo_ = gainOf(units_);
            step_ = (gainTo_ - gain_) / tickSamples_;
        }
        gain_ += step_;
        if ((step_ > 0.0 && gain_ > gainTo_) || (step_ < 0.0 && gain_ < gainTo_))
            gain_ = gainTo_;
        return gain_;
    }

private:
    enum class Stage { Idle, Play, Hold, Fade };

    // Into segment `segment` (0-3, 4 done) from where it is.
    void enter(int segment) {
        segment_ = segment;
        if (segment_ > 3) { // done: silent, or faded out from where it is
            if (gainOf(units_) < 1e-4) {
                stop();
            } else {
                fadeFrom_ = gain_;
                stage_ = Stage::Fade;
            }
            return;
        }
        stage_ = segment_ == 3 && held_ ? Stage::Hold : Stage::Play;
        if (stage_ == Stage::Hold) { // at L3 while the key is down, following it (see tick())
            target_ = levelUnits(settings_.levels[2]);
            speed_ = segmentSpeed(2);
            return;
        }
        const int i = segment_;
        target_ = levelUnits(settings_.levels[i]);
        rising_ = target_ > units_;
        waits_ = 0.0;
        if (std::abs(target_ - units_) < 0.5 || (i == 0 && settings_.levels[0] < 0.5))
            waits_ = waitTicks(i); // a segment that stays at its level waits
        waitTotal_ = waits_;
        from_ = rising_ ? std::max(units_, 1716.0) : units_; // where progress() counts from
        speed_ = segmentSpeed(i);
    }

    // Segment i's speed (of the 64), its rate's with the scaling's.
    double segmentSpeed(int i) const { return std::min(rateSpeed(settings_.rates[i]) + settings_.scaling, 63.0); }
    // How many ticks segment i waits where it stays at its level; an attack to nothing waits a twentieth as long.
    double waitTicks(int i) const {
        const double r = std::min(settings_.rates[i] + settings_.scaling, 99.0);
        double seconds = staticSeconds(r);
        if (i == 0 && settings_.levels[0] < 0.5 && r < 77.0)
            seconds /= 20.0;
        return std::max(seconds / kTickSeconds, 1e-9);
    }

    void tick() {
        if (stage_ == Stage::Hold) { // at L3 (target_ here, see prepare()), following it as it moves, at R3
            const double step = tickUnits(speed_);
            if (units_ < target_)
                units_ = std::min(units_ + std::max(std::floor(17.0 - units_ / 256.0), 1.0) * step, target_);
            else if (units_ > target_)
                units_ = std::max(units_ - step, target_);
            return;
        }
        if (waits_ > 0.0) {
            waits_ -= 1.0;
            if (waits_ <= 0.0)
                enter(segment_ + 1);
            return;
        }
        const double step = tickUnits(speed_);
        if (rising_) {
            units_ = std::max(units_, 1716.0); // an attack jumps up from below this first
            units_ += std::max(std::floor(17.0 - units_ / 256.0), 0.0) * step;
            if (units_ >= target_) {
                units_ = target_;
                enter(segment_ + 1);
            }
        } else {
            units_ -= step;
            if (units_ <= target_) {
                units_ = target_;
                enter(segment_ + 1);
            }
        }
    }

    Settings settings_{};
    Stage stage_ = Stage::Idle;
    bool held_ = false, startAsked_ = false, releaseAsked_ = false;
    int segment_ = 0;
    bool rising_ = false;
    double units_ = 0.0, target_ = 0.0, speed_ = 0.0, waits_ = 0.0;
    double from_ = 0.0, waitTotal_ = 0.0; // where the segment started, and how long it waits (for progress())
    double ticks_ = 0.0, tickSamples_ = 1.0;
    double gain_ = 0.0, gainTo_ = 0.0, step_ = 0.0; // what it puts out, ramping to gainTo_ over the tick
    double fadeFrom_ = 0.0, fadeStep_ = 1.0;
};

} // namespace fmvibe
