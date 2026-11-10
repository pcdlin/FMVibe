#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fmvibe {

// A buffer of floats for delay lines, which start silent: allocated as zeros the system hands out a page at a time
// as the buffer is first written (on Windows), so one that is allocated but never used costs address space rather
// than memory, and allocating it costs no time filling it. The effects allocate every slot's lines for every type
// ahead of time, since the audio thread can't, and most are never used.
class ZeroBuffer {
public:
    ZeroBuffer() = default;
    ZeroBuffer(const ZeroBuffer &) = delete;
    ZeroBuffer &operator=(const ZeroBuffer &) = delete;
    ZeroBuffer(ZeroBuffer &&other) noexcept { take(other); }
    ZeroBuffer &operator=(ZeroBuffer &&other) noexcept {
        if (this != &other) {
            release();
            take(other);
        }
        return *this;
    }
    ~ZeroBuffer() { release(); }

    // `size` zeros, in place of what it held (the value is always 0, as std::vector's assign() would be given).
    // Main thread.
    void assign(size_t size, float = 0.0f);
    // Silent again, for a new start: nothing to do while it was never handed out since it was allocated, else every
    // value set to 0 (which touches every page). Afterwards it counts as written. `used`: how many values from its
    // start can be other than 0 (a line written a value at a time from its start since it was last silent holds
    // nothing past as many as were written), so only those need setting.
    void zero(size_t used = static_cast<size_t>(-1)) {
        if (!pristine_)
            std::fill_n(data_, std::min(std::max(used, stale_), size_), 0.0f);
        stale_ = cleanFrom_ = cleanTo_ = 0;
        pristine_ = false;
    }
    // Silent again, as zero(used) makes it, but cleared only as it is read (see clearStale()) rather than at once:
    // a line seconds long would take the audio thread a while to clear, more than a chunk's time at high rates.
    void restart(size_t used = static_cast<size_t>(-1)) {
        stale_ = pristine_ ? 0 : std::min(std::max(used, stale_), size_);
        cleanFrom_ = cleanTo_ = 0;
        pristine_ = false;
    }
    // Ahead of a chunk's reads of a line restarted `written` values ago, which is written a value at a time from its
    // start: silences what it still holds from before its restart in [from, to), where the reads behind its start
    // reach, and in the `ahead` values on from where it writes next, which reads may take in at no weight. What was
    // written since stands, what lies past what was written before (stale_) is silent already, and so is a stretch
    // cleared for reads before (cleanFrom_ to cleanTo_), which follows them along: a read moving on along a line
    // clears as far as it moves.
    void clearStale(uint64_t written, size_t ahead, size_t from, size_t to) {
        if (written >= stale_) {
            stale_ = cleanFrom_ = cleanTo_ = 0; // all of it written since
            return;
        }
        const size_t next = static_cast<size_t>(written);
        std::fill(data_ + next, data_ + std::min(next + ahead, stale_), 0.0f);
        from = std::max(from, next);
        to = std::min(to, stale_);
        if (from >= to)
            return;
        if (cleanTo_ <= next || to < cleanFrom_ || from > cleanTo_) { // apart from the stretch: a new one
            std::fill(data_ + from, data_ + to, 0.0f);
            cleanFrom_ = from;
            cleanTo_ = to;
            return;
        }
        if (from < cleanFrom_) {
            std::fill(data_ + from, data_ + cleanFrom_, 0.0f);
            cleanFrom_ = from;
        }
        if (to > cleanTo_) {
            std::fill(data_ + cleanTo_, data_ + to, 0.0f);
            cleanTo_ = to;
        }
    }

    float *data() { return data_; }
    const float *data() const { return data_; }
    size_t size() const { return size_; }
    float &operator[](size_t i) { return data_[i]; }
    const float &operator[](size_t i) const { return data_[i]; }
    float *begin() { return data_; }
    float *end() { return data_ + size_; }

private:
    void release();
    void take(ZeroBuffer &other) {
        data_ = other.data_;
        size_ = other.size_;
        paged_ = other.paged_;
        pristine_ = other.pristine_;
        stale_ = other.stale_;
        cleanFrom_ = other.cleanFrom_;
        cleanTo_ = other.cleanTo_;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    float *data_ = nullptr;
    size_t size_ = 0;
    bool paged_ = false;   // allocated as pages of their own (see assign())
    bool pristine_ = true; // all zeros as allocated: not handed out since (see zero())
    size_t stale_ = 0;     // after restart(): how far from its start it may still hold what it held before
    size_t cleanFrom_ = 0, cleanTo_ = 0; // a stretch of that cleared since, where reads were (see clearStale())
};

} // namespace fmvibe
