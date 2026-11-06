#include "zerobuffer.h"

#include <cstdlib>
#include <new>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fmvibe {

void ZeroBuffer::assign(size_t size, float) {
    release();
    if (size == 0)
        return;
#ifdef _WIN32
    // Smaller buffers come from the heap, zeroed at once: pages of their own would waste more than they save.
    constexpr size_t kPagedBytes = 64 * 1024;
    const size_t bytes = size * sizeof(float);
    if (bytes >= kPagedBytes) {
        // Committed pages read as zeros, and only take memory once written.
        data_ = static_cast<float *>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        paged_ = data_ != nullptr;
    }
#endif
    if (!data_) {
        data_ = static_cast<float *>(std::calloc(size, sizeof(float)));
        paged_ = false;
    }
    if (!data_) { // as std::vector would
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
        throw std::bad_alloc();
#else
        std::abort();
#endif
    }
    size_ = size;
    pristine_ = true;
}

void ZeroBuffer::release() {
    if (!data_)
        return;
#ifdef _WIN32
    if (paged_)
        VirtualFree(data_, 0, MEM_RELEASE);
    else
        std::free(data_);
#else
    std::free(data_);
#endif
    data_ = nullptr;
    size_ = 0;
    paged_ = false;
    pristine_ = true;
}

} // namespace fmvibe
