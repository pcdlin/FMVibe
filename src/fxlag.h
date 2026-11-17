#pragma once

// How late each path through the effects graph is, from the effects that run oversampled: the shaper and the filter
// effect at twice the rate, an amp at its oversampling. Each goes up and back down through half-band filters (see
// UpDown in oversampler.h), a few samples late and later still toward the top. Where paths meet, each is put through as
// many more such paths up and down as it lacks against the latest (see Effects::orderGraph()), so they meet in step at
// every frequency; the output is then as late as its latest path, which the plugin reports to the host as its latency.
// Independent of the plugin formats.

#include "params.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace fmvibe {

// How many paths up and down a path holds, at 2, 4 and 8 times the rate. Paths at the same factor are alike and add up
// in any order, so two paths holding as many of each are in step.
struct FxLag {
    static constexpr int kKinds = 3;
    static constexpr int kMost = 64; // of each, past which a path counts no more (a graph holds far fewer)
    std::array<uint8_t, kKinds> paths{};

    static constexpr int kindOf(int factor) { return factor >= 8 ? 2 : factor >= 4 ? 1 : 0; }
    static constexpr int factorOf(int kind) { return 2 << kind; }
    bool operator==(const FxLag &) const = default;
    int count() const { return paths[0] + paths[1] + paths[2]; }
    bool none() const { return count() == 0; }
    FxLag plus(const FxLag &o) const {
        FxLag r;
        for (int k = 0; k < kKinds; ++k)
            r.paths[k] = static_cast<uint8_t>(std::min(paths[k] + o.paths[k], kMost));
        return r;
    }
    FxLag most(const FxLag &o) const {
        FxLag r;
        for (int k = 0; k < kKinds; ++k)
            r.paths[k] = std::max(paths[k], o.paths[k]);
        return r;
    }
    // What `this` has more of than `o` (none where it has less).
    FxLag past(const FxLag &o) const {
        FxLag r;
        for (int k = 0; k < kKinds; ++k)
            r.paths[k] = static_cast<uint8_t>(std::max(paths[k] - o.paths[k], 0));
        return r;
    }
};

// How far an effect in `slot` runs oversampled, by its params: 2 for a shaper or filter, an amp's oversampling, else 1
// (none). By its type, whether it is switched on or not, so switching it doesn't move the paths through it.
template <class Get>
int fxSlotFactor(const Get &get, int slot) {
    switch (fxType(get(paramIndex(slot, FxParam::Type)))) {
    case FxType::Shaper:
    case FxType::Filter:
        return 2;
    case FxType::Amp:
        return ampOversampling(get(paramIndex(slot, FxParam::AmpOversampling)));
    default:
        return 1;
    }
}

template <class Get>
FxLag fxSlotLag(const Get &get, int slot) {
    FxLag lag;
    if (const int factor = fxSlotFactor(get, slot); factor >= 2)
        lag.paths[FxLag::kindOf(factor)] = 1;
    return lag;
}

// Each wire's pad, into `pads` when given (what the path into node n's input i is to be put through more), and how
// late the output is. Along the wires as set from OUT, through the nodes `wanted` takes (as Effects::nodeWanted()):
// a node's inputs meet as late as the latest of them, and an effect adds its own. A wire that closes a loop (which
// reads the chunk before) is left out.
using FxPads = std::array<std::array<FxLag, kFxSumInputs>, kFxNodes>;

template <class Get, class Wanted>
FxLag fxGraphLags(const Get &get, const Wanted &wanted, FxPads *pads) {
    static_assert(kFxSumInputs >= kFxInputs && kFxSumInputs >= kFxOutInputs);
    std::array<FxLag, kFxNodes> lag{};             // how late what comes out of each node is
    std::array<uint8_t, kFxNodes> state{};          // 0 not reached, 1 on the way, 2 done
    std::array<std::array<int8_t, kFxSumInputs>, kFxNodes> from{}; // the port each input reads, -1 none, -2 a loop
    std::array<std::pair<int, int>, kFxNodes> stack;
    if (pads)
        for (auto &node : *pads)
            node.fill(FxLag{});
    auto portLag = [&](int port) { return port == kFxPortIn ? FxLag{} : lag[fxPortNode(port)]; };
    int depth = 0;
    stack[depth++] = {kFxOutNode, 0};
    state[kFxOutNode] = 1;
    while (depth > 0) {
        auto &[node, input] = stack[depth - 1];
        if (input < fxInputCount(node)) {
            const int i = input++;
            int port = fxPortOf(get(fxInputIndex(node, i, FxInputField::From)));
            const int source = port >= 0 ? fxPortNode(port) : -1;
            if (source >= 0 && !wanted(source))
                port = -1;
            from[node][i] = static_cast<int8_t>(port);
            if (port < 0 || source < 0)
                continue;
            if (state[source] == 1) {
                from[node][i] = -2;
            } else if (state[source] == 0) {
                state[source] = 1;
                stack[depth++] = {source, 0};
            }
            continue;
        }
        // Every input looked along: they meet as late as the latest.
        FxLag in;
        for (int i = 0; i < fxInputCount(node); ++i)
            if (from[node][i] >= 0)
                in = in.most(portLag(from[node][i]));
        if (pads)
            for (int i = 0; i < fxInputCount(node); ++i)
                if (from[node][i] >= 0)
                    (*pads)[node][i] = in.past(portLag(from[node][i]));
        lag[node] = node < kFxSlots ? in.plus(fxSlotLag(get, node)) : in;
        state[node] = 2;
        --depth;
    }
    return lag[kFxOutNode];
}

// How late, in host samples, low frequencies come out of a path holding `lag`, from how late one path up and down at
// each factor is (`delays`, see upDownDelay()).
inline double fxLagSamples(const FxLag &lag, const std::array<double, FxLag::kKinds> &delays) {
    double samples = 0.0;
    for (int k = 0; k < FxLag::kKinds; ++k)
        samples += lag.paths[k] * delays[k];
    return samples;
}

// Whether a node is in the graph by its params alone: a slot holding an effect, a split or a sum in use, the output.
// Effects::nodeWanted() also keeps an effect fading out.
template <class Get>
bool fxNodeSet(const Get &get, int node) {
    if (node == kFxOutNode)
        return true;
    if (node < kFxSlots)
        return fxType(get(paramIndex(node, FxParam::Type))) != FxType::None;
    if (const int sum = fxNodeSum(node); sum >= 0)
        return get(paramIndex(sum, SumParam::Used)) >= 0.5;
    return get(paramIndex(node - kFxSlots, SplitParam::Used)) >= 0.5;
}

// Whether a param can make the output later or sooner: an effect's type, an amp's oversampling, the graph's wires,
// splits and joins.
constexpr bool fxLagParam(int index) {
    return isFxGraphParam(index) || fxFieldOf(index) == FxParam::Type || fxFieldOf(index) == FxParam::AmpOversampling;
}

// How late the output is by the params, as fxGraphLags() works it out.
template <class Get>
FxLag fxOutputLag(const Get &get) {
    return fxGraphLags(get, [&get](int node) { return fxNodeSet(get, node); }, nullptr);
}

} // namespace fmvibe
