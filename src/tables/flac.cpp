// A FLAC decoder, enough for wavetables and samples: every subframe type (constant, verbatim, fixed and LPC
// predictors), Rice-coded residuals with escapes, wasted bits and the stereo decorrelations, up to 8 channels and 32
// bits. Written for FMVibe rather than vendored; it reads only, and never past the bytes it is given.

#include "readers.h"

#include <algorithm>
#include <cstring>

namespace fmvibe {

namespace {

class Bits {
public:
    Bits(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    bool ok() const { return ok_; }
    size_t bytePos() const { return (bit_ + 7) / 8; }
    void alignByte() { bit_ = (bit_ + 7) & ~size_t{7}; }
    void seekByte(size_t at) { bit_ = at * 8; }
    uint32_t read(int n) { // up to 32 bits, most significant first
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (bit_ >= size_ * 8) {
                ok_ = false;
                return 0;
            }
            v = (v << 1) | ((data_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1u);
            ++bit_;
        }
        return static_cast<uint32_t>(v);
    }
    int32_t readSigned(int n) {
        if (n == 0)
            return 0;
        const uint32_t v = read(n);
        if (n >= 32)
            return static_cast<int32_t>(v);
        return (v & (1u << (n - 1))) ? static_cast<int32_t>(v) - static_cast<int32_t>(1u << n) : static_cast<int32_t>(v);
    }
    uint32_t unary() { // zeros until a one
        uint32_t count = 0;
        while (ok_) {
            if (bit_ >= size_ * 8) {
                ok_ = false;
                return 0;
            }
            if ((data_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1u) {
                ++bit_;
                return count;
            }
            ++bit_;
            if (++count > (1u << 24)) {
                ok_ = false;
                return 0;
            }
        }
        return 0;
    }

private:
    const uint8_t *data_;
    size_t size_;
    size_t bit_ = 0;
    bool ok_ = true;
};

bool residual(Bits &b, int blockSize, int order, int64_t *out) {
    const uint32_t method = b.read(2);
    if (method > 1)
        return false;
    const int paramBits = method == 0 ? 4 : 5;
    const uint32_t escape = method == 0 ? 15u : 31u;
    const int partitionOrder = static_cast<int>(b.read(4));
    const int partitions = 1 << partitionOrder;
    if ((blockSize >> partitionOrder) < order || (blockSize % partitions) != 0)
        return false;
    int at = order;
    for (int p = 0; p < partitions && b.ok(); ++p) {
        const int count = (blockSize >> partitionOrder) - (p == 0 ? order : 0);
        const uint32_t param = b.read(paramBits);
        if (param == escape) {
            const int raw = static_cast<int>(b.read(5));
            for (int i = 0; i < count; ++i)
                out[at++] = b.readSigned(raw);
        } else {
            for (int i = 0; i < count; ++i) {
                const uint64_t q = b.unary();
                const uint64_t v = (q << param) | b.read(static_cast<int>(param));
                out[at++] = (v & 1u) ? -static_cast<int64_t>(v >> 1) - 1 : static_cast<int64_t>(v >> 1);
            }
        }
    }
    return b.ok();
}

bool subframe(Bits &b, int blockSize, int bps, int64_t *out) {
    if (b.read(1) != 0)
        return false;
    const uint32_t type = b.read(6);
    int wasted = 0;
    if (b.read(1))
        wasted = static_cast<int>(b.unary()) + 1;
    if (wasted >= bps)
        return false;
    bps -= wasted;
    if (type == 0) { // constant
        const int64_t v = b.readSigned(bps);
        std::fill(out, out + blockSize, v);
    } else if (type == 1) { // verbatim
        for (int i = 0; i < blockSize; ++i)
            out[i] = b.readSigned(bps);
    } else if (type >= 8 && type <= 12) { // fixed
        const int order = static_cast<int>(type - 8);
        if (order > blockSize)
            return false;
        for (int i = 0; i < order; ++i)
            out[i] = b.readSigned(bps);
        if (!residual(b, blockSize, order, out))
            return false;
        for (int i = order; i < blockSize; ++i) {
            const int64_t r = out[i];
            switch (order) {
            case 0: out[i] = r; break;
            case 1: out[i] = r + out[i - 1]; break;
            case 2: out[i] = r + 2 * out[i - 1] - out[i - 2]; break;
            case 3: out[i] = r + 3 * out[i - 1] - 3 * out[i - 2] + out[i - 3]; break;
            default: out[i] = r + 4 * out[i - 1] - 6 * out[i - 2] + 4 * out[i - 3] - out[i - 4]; break;
            }
        }
    } else if (type >= 32) { // LPC
        const int order = static_cast<int>(type - 31);
        if (order > blockSize)
            return false;
        for (int i = 0; i < order; ++i)
            out[i] = b.readSigned(bps);
        const int precision = static_cast<int>(b.read(4)) + 1;
        if (precision == 16)
            return false;
        const int shift = b.readSigned(5);
        if (shift < 0)
            return false;
        int32_t coefs[32];
        for (int i = 0; i < order; ++i)
            coefs[i] = b.readSigned(precision);
        if (!residual(b, blockSize, order, out))
            return false;
        for (int i = order; i < blockSize; ++i) {
            int64_t sum = 0;
            for (int j = 0; j < order; ++j)
                sum += static_cast<int64_t>(coefs[j]) * out[i - 1 - j];
            out[i] += sum >> shift;
        }
    } else {
        return false;
    }
    if (wasted > 0)
        for (int i = 0; i < blockSize; ++i)
            out[i] *= int64_t{1} << wasted;
    return b.ok();
}

} // namespace

bool decodeFlac(const uint8_t *bytes, size_t size, std::vector<float> &mono, uint32_t &rate, int &bits) {
    mono.clear();
    if (size < 42 || std::memcmp(bytes, "fLaC", 4) != 0)
        return false;
    size_t at = 4;
    int channels = 0, streamBits = 0;
    uint64_t total = 0;
    rate = 0;
    for (bool last = false; !last;) {
        if (at + 4 > size)
            return false;
        last = (bytes[at] & 0x80) != 0;
        const int type = bytes[at] & 0x7F;
        const size_t length = (size_t{bytes[at + 1]} << 16) | (size_t{bytes[at + 2]} << 8) | bytes[at + 3];
        at += 4;
        if (at + length > size)
            return false;
        if (type == 0 && length >= 34) {
            Bits b(bytes + at, length);
            b.read(16);
            b.read(16);
            b.read(24);
            b.read(24);
            rate = b.read(20);
            channels = static_cast<int>(b.read(3)) + 1;
            streamBits = static_cast<int>(b.read(5)) + 1;
            total = (uint64_t{b.read(4)} << 32) | b.read(32);
        }
        at += length;
    }
    if (channels == 0 || rate == 0 || streamBits < 4)
        return false;
    bits = streamBits;
    if (total > 0)
        mono.reserve(static_cast<size_t>(std::min<uint64_t>(total, uint64_t{1} << 23)));
    std::vector<int64_t> samples[8];
    Bits b(bytes, size);
    b.seekByte(at);
    while (b.bytePos() + 2 < size) {
        // A frame's header: sync code, then how it is laid out.
        const size_t frameStart = b.bytePos();
        if (b.read(14) != 0x3FFE) {
            b.seekByte(frameStart + 1); // hunt for the next frame
            continue;
        }
        b.read(1);
        b.read(1);
        const uint32_t sizeCode = b.read(4), rateCode = b.read(4), assignment = b.read(4), bitsCode = b.read(3);
        b.read(1);
        { // the frame or sample number, UTF-8 coded
            const uint32_t first = b.read(8);
            int more = 0;
            for (uint32_t mask = 0x80; (first & mask) && mask > 1; mask >>= 1)
                ++more;
            for (int i = 1; i < more; ++i)
                b.read(8);
        }
        int blockSize = 0;
        if (sizeCode == 1)
            blockSize = 192;
        else if (sizeCode >= 2 && sizeCode <= 5)
            blockSize = 576 << (sizeCode - 2);
        else if (sizeCode == 6)
            blockSize = static_cast<int>(b.read(8)) + 1;
        else if (sizeCode == 7)
            blockSize = static_cast<int>(b.read(16)) + 1;
        else if (sizeCode >= 8)
            blockSize = 256 << (sizeCode - 8);
        if (rateCode == 12)
            b.read(8);
        else if (rateCode == 13 || rateCode == 14)
            b.read(16);
        b.read(8); // CRC-8
        constexpr int kBits[] = {0, 8, 12, 0, 16, 20, 24, 32};
        const int bps = bitsCode == 0 ? streamBits : kBits[bitsCode];
        const int frameChannels = assignment < 8 ? static_cast<int>(assignment) + 1 : 2;
        if (!b.ok() || blockSize <= 0 || bps == 0 || assignment > 10 || frameChannels > 8)
            return !mono.empty();
        for (int c = 0; c < frameChannels; ++c) {
            samples[c].assign(static_cast<size_t>(blockSize), 0);
            // The side channel of a decorrelated pair has a bit more.
            const bool side = (assignment == 8 && c == 1) || (assignment == 9 && c == 0) || (assignment == 10 && c == 1);
            if (!subframe(b, blockSize, bps + (side ? 1 : 0), samples[c].data()))
                return !mono.empty();
        }
        if (assignment == 8) { // left, side
            for (int i = 0; i < blockSize; ++i)
                samples[1][i] = samples[0][i] - samples[1][i];
        } else if (assignment == 9) { // side, right
            for (int i = 0; i < blockSize; ++i)
                samples[0][i] = samples[0][i] + samples[1][i];
        } else if (assignment == 10) { // mid, side
            for (int i = 0; i < blockSize; ++i) {
                const int64_t side = samples[1][i];
                const int64_t mid = samples[0][i] * 2 | (side & 1);
                samples[0][i] = (mid + side) >> 1;
                samples[1][i] = (mid - side) >> 1;
            }
        }
        const double scale = 1.0 / static_cast<double>(int64_t{1} << (bps - 1));
        for (int i = 0; i < blockSize; ++i) {
            double sum = 0.0;
            for (int c = 0; c < frameChannels; ++c)
                sum += static_cast<double>(samples[c][i]) * scale;
            mono.push_back(static_cast<float>(sum));
        }
        if (mono.size() > (size_t{1} << 26))
            return false;
        b.alignByte();
        b.read(16); // CRC-16
        if (total > 0 && mono.size() >= total) {
            mono.resize(static_cast<size_t>(total));
            break;
        }
    }
    return !mono.empty();
}

} // namespace fmvibe
