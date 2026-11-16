#pragma once

// The classic FM algorithms: the 32 layouts of six operators and the 8 of four that FM synths have long been played
// with, then the 5 shapes three operators can take and the 2 of two, which View::applyAlgorithm() sets the operators'
// routes to. Each is which operator modulates which, which go out, and where the feedback is: on an operator, or a
// route back up a stack (six operators' 4, from D into F, and 6, from E into F). The operators stand for the classic
// charts' 1 to 6 by A to F, A always a carrier.

#include "../params.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace fmvibe::gui {

struct Algorithm {
    int number = 0;               // 1 to 32 among six operators, 1 to 8 among four, 1 to 5 among three, 1 or 2 of two
    int ops = 0;                  // the operators it uses, from A: 6, 4, 3 or 2
    uint8_t carriers = 0;         // a bit an operator, A the lowest: those going out
    std::array<uint8_t, kOps> mods{}; // mods[to]: a bit for each operator that modulates `to`, the feedback aside
    int feedbackFrom = 0, feedbackTo = 0; // the feedback route: the same operator for one onto itself
};

namespace algorithm_detail {

constexpr int op(char letter) { return letter - 'A'; }

// "BA FE": B modulates A and F modulates E; carriers "AC"; feedback "F" (onto F) or "DF" (D into F).
constexpr Algorithm make(int number, int ops, const char *routes, const char *carriers, const char *feedback) {
    Algorithm a;
    a.number = number;
    a.ops = ops;
    for (const char *p = routes; *p; ++p)
        if (*p != ' ') {
            a.mods[op(p[1])] |= static_cast<uint8_t>(1 << op(p[0]));
            ++p;
        }
    for (const char *p = carriers; *p; ++p)
        a.carriers |= static_cast<uint8_t>(1 << op(*p));
    a.feedbackFrom = op(feedback[0]);
    a.feedbackTo = feedback[1] ? op(feedback[1]) : a.feedbackFrom;
    return a;
}

} // namespace algorithm_detail

// Six operators' first, by number, then four's, three's and two's. Six's and four's are checked against the charts of
// the classic six-operator synth's 32 (as Dexed's FmCore table has them) and the eight of the classic four-operator
// chips (OPM, OPN); two's are the classic two-operator chips' pair (OPL): B into A, or both out. Three operators have
// no classic chart: theirs are every shape they can take, ordered as four's are, from the stack to all of them out,
// the feedback on the top one as there.
inline constexpr std::array<Algorithm, 47> kAlgorithms = [] {
    using algorithm_detail::make;
    return std::array<Algorithm, 47>{
        make(1, 6, "BA FE ED DC", "AC", "F"),
        make(2, 6, "BA FE ED DC", "AC", "B"),
        make(3, 6, "CB BA FE ED", "AD", "F"),
        make(4, 6, "CB BA FE ED", "AD", "DF"),
        make(5, 6, "BA DC FE", "ACE", "F"),
        make(6, 6, "BA DC FE", "ACE", "EF"),
        make(7, 6, "BA DC EC FE", "AC", "F"),
        make(8, 6, "BA DC EC FE", "AC", "D"),
        make(9, 6, "BA DC EC FE", "AC", "B"),
        make(10, 6, "CB BA ED FD", "AD", "C"),
        make(11, 6, "CB BA ED FD", "AD", "F"),
        make(12, 6, "BA DC EC FC", "AC", "B"),
        make(13, 6, "BA DC EC FC", "AC", "F"),
        make(14, 6, "BA DC ED FD", "AC", "F"),
        make(15, 6, "BA DC ED FD", "AC", "B"),
        make(16, 6, "BA CA DC EA FE", "A", "F"),
        make(17, 6, "BA CA DC EA FE", "A", "B"),
        make(18, 6, "BA CA DA ED FE", "A", "C"),
        make(19, 6, "CB BA FD FE", "ADE", "F"),
        make(20, 6, "CA CB ED FD", "ABD", "C"),
        make(21, 6, "CA CB FD FE", "ABDE", "C"),
        make(22, 6, "BA FC FD FE", "ACDE", "F"),
        make(23, 6, "CB FD FE", "ABDE", "F"),
        make(24, 6, "FC FD FE", "ABCDE", "F"),
        make(25, 6, "FD FE", "ABCDE", "F"),
        make(26, 6, "CB ED FD", "ABD", "F"),
        make(27, 6, "CB ED FD", "ABD", "C"),
        make(28, 6, "BA DC ED", "ACF", "E"),
        make(29, 6, "DC FE", "ABCE", "F"),
        make(30, 6, "DC ED", "ABCF", "E"),
        make(31, 6, "FE", "ABCDE", "F"),
        make(32, 6, "", "ABCDEF", "F"),
        make(1, 4, "DC CB BA", "A", "D"),
        make(2, 4, "CB DB BA", "A", "D"),
        make(3, 4, "CB BA DA", "A", "D"),
        make(4, 4, "DC CA BA", "A", "D"),
        make(5, 4, "BA DC", "AC", "D"),
        make(6, 4, "DA DB DC", "ABC", "D"),
        make(7, 4, "DC", "ABC", "D"),
        make(8, 4, "", "ABCD", "D"),
        make(1, 3, "CB BA", "A", "C"), // a stack
        make(2, 3, "BA CA", "A", "C"), // two into one
        make(3, 3, "CA CB", "AB", "C"), // one into two
        make(4, 3, "CB", "AB", "C"),    // a pair beside a carrier
        make(5, 3, "", "ABC", "C"),
        make(1, 2, "BA", "A", "B"),
        make(2, 2, "", "AB", "B"),
    };
}();

// What the operators that are on form: their routes among them (feedback too) and which go out. Two patches with
// the same shape are the same algorithm, whatever their depths.
struct OpShape {
    uint8_t on = 0;
    uint8_t out = 0;
    uint64_t routes = 0; // a bit from * kOps + to
    bool operator==(const OpShape &) const = default;
};

constexpr uint64_t routeBit(int from, int to) {
    return uint64_t{1} << (from * kOps + to);
}

constexpr OpShape shapeOf(const Algorithm &a) {
    OpShape s;
    s.on = static_cast<uint8_t>((1 << a.ops) - 1);
    s.out = a.carriers;
    for (int to = 0; to < a.ops; ++to)
        for (int from = 0; from < a.ops; ++from)
            if (a.mods[to] >> from & 1)
                s.routes |= routeBit(from, to);
    s.routes |= routeBit(a.feedbackFrom, a.feedbackTo);
    return s;
}

// Whether `op` modulates another operator.
constexpr bool modulates(const Algorithm &a, int op) {
    for (int to = 0; to < a.ops; ++to)
        if (a.mods[to] >> op & 1)
            return true;
    return false;
}

// Each one sound: every operator goes out or modulates (not both), nothing outside its operators, the modulation
// running one way down to the carriers, and no two alike.
constexpr bool algorithmsSound() {
    for (size_t i = 0; i < kAlgorithms.size(); ++i) {
        const Algorithm &a = kAlgorithms[i];
        const int used = (1 << a.ops) - 1;
        if ((a.carriers & ~used) || !(a.carriers & 1) || a.feedbackFrom >= a.ops || a.feedbackTo >= a.ops)
            return false;
        for (int o = 0; o < kOps; ++o) {
            if (o >= a.ops ? a.mods[o] != 0 : (a.mods[o] & ~used) != 0)
                return false;
            if (o < a.ops && ((a.carriers >> o & 1) != 0) == modulates(a, o))
                return false;
        }
        // No loop: operators peeled off from the top, those nothing left modulates, until none is left.
        int left = used;
        for (int round = 0; round < a.ops && left; ++round)
            for (int o = 0; o < a.ops; ++o)
                if ((left >> o & 1) && (a.mods[o] & left) == 0)
                    left &= ~(1 << o);
        if (left)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (shapeOf(kAlgorithms[j]) == shapeOf(a))
                return false;
    }
    return true;
}
static_assert(algorithmsSound(), "each algorithm runs down to its carriers, and no two are alike");
// The sets, by how many operators their algorithms use, in the order the panel's switch shows them.
inline constexpr int kAlgorithmSets[] = {6, 4, 3, 2};

// Where the algorithms of `ops` operators start in kAlgorithms, and how many there are.
struct AlgorithmRange {
    int first = 0, count = 0;
};
constexpr AlgorithmRange algorithmSetRange(int ops) {
    AlgorithmRange range{-1, 0};
    for (int i = 0; i < static_cast<int>(kAlgorithms.size()); ++i)
        if (kAlgorithms[i].ops == ops) {
            if (range.first < 0)
                range.first = i;
            ++range.count;
        }
    return range;
}

// Each set together, in the switch's order, numbered from 1, and every algorithm in one.
constexpr bool algorithmSetsInOrder() {
    int next = 0;
    for (int ops : kAlgorithmSets) {
        const AlgorithmRange range = algorithmSetRange(ops);
        if (range.first != next)
            return false;
        for (int i = 0; i < range.count; ++i)
            if (kAlgorithms[range.first + i].number != i + 1)
                return false;
        next += range.count;
    }
    return next == static_cast<int>(kAlgorithms.size());
}
static_assert(algorithmSetsInOrder(), "six operators' 32, then four's 8, three's 5 and two's 2, each numbered from 1");
static_assert(algorithmSetRange(6).count == 32 && algorithmSetRange(4).first == 32 && algorithmSetRange(3).count == 5 &&
              algorithmSetRange(2).count == 2);

// Where an algorithm puts its operators: across, in the columns of the routing graph's snap grid (see
// kRoutingGridCols), the carriers in column 6, where A and D start, clear of the shapers' and filters' columns nearer
// OUT, and each operator two columns left of the furthest down the stack it modulates; down, 0 to 1 over the graph's
// height. Rows follow the chart: each carrier and what modulates it in rows of their own, top down from A, an operator
// modulated by several in the middle of them and one modulating several in the middle of those. The rows sit half the
// height apart, closer where more need it to fit; past five, every other one a column further left, so two nodes a
// row apart stay clear of each other.
struct AlgorithmPlaces {
    std::array<float, kOps> col{}, y{};
};

inline AlgorithmPlaces algorithmPlaces(const Algorithm &a) {
    constexpr int kCarrierCol = 6, kColStep = 2;
    AlgorithmPlaces places;
    std::array<int, kOps> depth{};
    for (int pass = 0; pass < a.ops; ++pass)
        for (int to = 0; to < a.ops; ++to)
            for (int from = 0; from < a.ops; ++from)
                if (a.mods[to] >> from & 1)
                    depth[from] = std::max(depth[from], depth[to] + 1);
    std::array<float, kOps> row{};
    std::array<bool, kOps> seen{};
    int slots = 0;
    auto visit = [&](auto &self, int op) -> void {
        seen[op] = true;
        float sum = 0.0f;
        int own = 0;
        for (int m = 0; m < a.ops; ++m)
            if ((a.mods[op] >> m & 1) && !seen[m]) {
                self(self, m);
                sum += row[m];
                ++own;
            }
        row[op] = own ? sum / static_cast<float>(own) : static_cast<float>(slots++);
    };
    for (int o = 0; o < a.ops; ++o)
        if ((a.carriers >> o & 1) && !seen[o])
            visit(visit, o);
    for (int o = 0; o < a.ops; ++o)
        if (!seen[o])
            visit(visit, o);
    for (int from = 0; from < a.ops; ++from) { // one modulating several: in the middle of them
        float sum = 0.0f;
        int targets = 0;
        for (int to = 0; to < a.ops; ++to)
            if (a.mods[to] >> from & 1) {
                sum += row[to];
                ++targets;
            }
        if (targets > 1)
            row[from] = sum / static_cast<float>(targets);
    }
    constexpr int kUnstaggered = 5;
    const float spacing = slots > 1 ? std::min(0.45f, 0.9f / static_cast<float>(slots - 1)) : 0.0f;
    for (int o = 0; o < a.ops; ++o) {
        places.col[o] = static_cast<float>(kCarrierCol - kColStep * depth[o]);
        places.y[o] = 0.5f + (row[o] - 0.5f * static_cast<float>(slots - 1)) * spacing;
        if (slots > kUnstaggered && static_cast<int>(row[o]) % 2 == 1)
            places.col[o] -= 1.0f;
    }
    return places;
}

} // namespace fmvibe::gui
