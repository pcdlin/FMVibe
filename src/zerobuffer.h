#pragma once

#include <algorithm>
#include <cstddef>

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
            std::fill_n(data_, std::min(used, size_), 0.0f);
        pristine_ = false;
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
        other.data_ = nullptr;
        other.size_ = 0;
    }

    float *data_ = nullptr;
    size_t size_ = 0;
    bool paged_ = false;   // allocated as pages of their own (see assign())
    bool pristine_ = true; // all zeros as allocated: not handed out since (see zero())
};

} // namespace fmvibe
