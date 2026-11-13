#pragma once

// Where an operator's unison copies go: each one's share of the detune (-1..1, times Unison Detune), its place across the
// stereo field (-1..1, times Unison Spread) and its weight (1 for every copy until Blend turns the outer ones down). The
// voices (synth.cpp) and the UNISON tab's display (view.cpp) both work them out here, so the display shows what plays.

#include "params.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fmvibe {

struct UnisonSpreadOut {
    std::array<double, kMaxUnison> detune{}; // copy k's share of the detune, -1..1, the copies in rising pitch for all
                                             // but Random
    std::array<double, kMaxUnison> pan{};    // its place, -1 (left) to 1 (right)
    std::array<double, kMaxUnison> weight{}; // its level, the copies' squares summing to their count (as 1 each does)
};

// A number from 0 up to 1 for each (seed, n): the Random modes' draws, the same for a note's seed every block.
inline double unisonDraw(uint32_t seed, uint32_t n) {
    uint32_t x = seed * 0x9E3779B9u + n * 0x85EBCA6Bu + 0x27D4EB2Fu;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

// The supersaw's seven copies, as a classic one spaces them: unevenly, the outer ones far out. Other counts read
// along these.
inline constexpr std::array<double, 7> kSupersawDetune{-1.0, -0.5716, -0.1774, 0.0, 0.1810, 0.5650, 0.9766};

// Copy k of `copies` along the copies' even spread, -1..1 (0 for one copy): Linear's detune and Ramp's place.
inline double unisonEven(int k, int copies) { return copies > 1 ? 2.0 * k / (copies - 1) - 1.0 : 0.0; }

// `seed` is the note's (any number with no note): the Random modes draw from it, and from the operator, so each
// operator of a note spreads its own way.
inline UnisonSpreadOut unisonSpread(int copies, UniCurve curve, UniPan pan, double blend, uint32_t seed, int op) {
    UnisonSpreadOut out;
    copies = std::clamp(copies, 1, kMaxUnison);
    const uint32_t opSeed = seed * 31u + static_cast<uint32_t>(op) * 0x632BE5ABu;
    for (int k = 0; k < copies; ++k) {
        const double even = unisonEven(k, copies);
        double d = even;
        switch (curve) {
        case UniCurve::Linear:
            break;
        case UniCurve::Center: // most of them near the pitch, the outer ones as far as Linear's
            d = even * std::abs(even);
            break;
        case UniCurve::Supersaw: {
            const double at = copies > 1 ? static_cast<double>(k) / (copies - 1) * (kSupersawDetune.size() - 1) : 3.0;
            const int i = std::min(static_cast<int>(at), static_cast<int>(kSupersawDetune.size()) - 2);
            d = kSupersawDetune[i] + (kSupersawDetune[i + 1] - kSupersawDetune[i]) * (at - i);
            break;
        }
        case UniCurve::Random: // one copy in each of `copies` even stretches of the range, anywhere along it
            d = copies > 1 ? -1.0 + (k + unisonDraw(opSeed, static_cast<uint32_t>(k))) * 2.0 / copies : 0.0;
            break;
        default:
            break;
        }
        out.detune[k] = d;
    }
    // Alternate: the copies about the middle of the pitch order take sides in turn, mirrored outward, so copies next to
    // each other in pitch sit on opposite sides and no two share a place.
    std::array<int, kMaxUnison> order{};
    if (pan == UniPan::Random) { // the even places dealt out at random
        for (int k = 0; k < copies; ++k)
            order[k] = k;
        for (int k = copies - 1; k > 0; --k)
            std::swap(order[k], order[std::min(k, static_cast<int>(unisonDraw(opSeed, 100u + k) * (k + 1)))]);
    }
    for (int k = 0; k < copies; ++k) {
        switch (pan) {
        case UniPan::Alternate: {
            const int fromMiddle = std::abs(2 * k - (copies - 1)) / 2;
            out.pan[k] = unisonEven(k, copies) * (fromMiddle % 2 ? -1.0 : 1.0);
            break;
        }
        case UniPan::Random:
            out.pan[k] = unisonEven(order[k], copies);
            break;
        default:
            out.pan[k] = unisonEven(k, copies);
            break;
        }
    }
    // Blend: the copies turned down by how far out their detune takes them, to a tenth at full Blend, then all scaled
    // together so their sum is as loud as before.
    if (blend > 0.0 && copies > 1) {
        double squares = 0.0;
        for (int k = 0; k < copies; ++k) {
            out.weight[k] = 1.0 - 0.9 * std::clamp(blend, 0.0, 1.0) * std::abs(out.detune[k]);
            squares += out.weight[k] * out.weight[k];
        }
        const double scale = std::sqrt(copies / squares);
        for (int k = 0; k < copies; ++k)
            out.weight[k] *= scale;
    } else {
        for (int k = 0; k < copies; ++k)
            out.weight[k] = 1.0;
    }
    return out;
}

} // namespace fmvibe
