#pragma once

// The effects graph as the editor edits it (see patchedits.h): a main line of effects and splits from IN to OUT, each
// split with a high and a low band of effects (and, split in three, a middle one between), closed by a join (the sum
// of its number) or, the line's last item, left open into OUT. The editor reads the line from the wires (line()) and
// writes the wires from it (writeLine()), so the engine runs what the FX view shows; filling and emptying the effect
// slots, splitting and copying go through it too. Nodes are numbered as the graph's params number them (a slot,
// kFxSlots + split, the output, then the sums); wires come from ports (fxSlotPort(), fxBandPort(), ...).

#include "modulationmodel.h"
#include "patchedits.h"

#include <algorithm>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace fmvibe::gui {

struct FxWireRef {
    int node;  // the node it goes into
    int input; // which of that node's inputs
    int port;  // where it comes from
};
// A split's bands go by lane: 1 the high one, 2 the low one, 3 the middle one, which only a split in three has.
struct FxLineItem {
    int node;            // an effect's slot, or a split's node
    std::vector<int> hi; // a split's bands: the effects' slots along each, in order
    std::vector<int> lo;
    bool joined = false; // closed by its join
    std::vector<int> mid;
    bool three = false; // split in three bands, `mid` between `hi` and `lo`
    std::vector<int> &band(int lane) { return lane == 1 ? hi : lane == 3 ? mid : lo; }
    const std::vector<int> &band(int lane) const { return lane == 1 ? hi : lane == 3 ? mid : lo; }
    // Its bands' lanes, top to bottom as the graph has them: the high band, the middle one, the low one.
    std::span<const int> lanes() const {
        static constexpr int kThreeLanes[] = {1, 3, 2}, kTwoLanes[] = {1, 2};
        return three ? std::span<const int>(kThreeLanes) : std::span<const int>(kTwoLanes);
    }
};
using FxLine = std::vector<FxLineItem>;
// Where the list of effects puts what it adds when it was opened in the graph: at the end of a lane (a +, `item`
// -1), or where a wire's middle was clicked: on the main line before item `item`, or on that item's band (lane 1
// the high one, 2 the low one, 3 the middle one) at `place`. Lane -1: neither, the list opened elsewhere.
struct FxSlotAt {
    int lane = -1;
    int item = -1;
    int place = 0;
};

// A split's join: the sum of the same number.
inline int fxJoinNode(int split) {
    return fxSumNode(split);
}

namespace detail {

// The bands of a split the grid uses: the high one along the row above the main line, the low one along the row below.
constexpr int kHighBand = kFxBands - 1;
constexpr int kLowBand = 0;

// Where an item of the line sits: on the main line, or along one of a split's bands.
struct LinePlace {
    int item = -1; // the main line's item it is, or holds it
    int lane = 0;  // 0 the main line, 1 the high band, 2 the low band, 3 the middle band
    int at = 0;    // its place along the band
};

inline LinePlace placeOf(const FxLine &line, int node) {
    for (int i = 0; i < static_cast<int>(line.size()); ++i) {
        if (line[i].node == node)
            return {i, 0, 0};
        for (int lane : line[i].lanes()) {
            const std::vector<int> &band = line[i].band(lane);
            if (const auto it = std::find(band.begin(), band.end(), node); it != band.end())
                return {i, lane, static_cast<int>(it - band.begin())};
        }
        if (line[i].joined && fxSumNode(line[i].node - kFxSlots) == node) // a split's join
            return {i, 0, 0};
    }
    return {};
}

inline bool openSplit(const FxLine &line) {
    return !line.empty() && fxNodeSplit(line.back().node) && !line.back().joined;
}

} // namespace detail

class FxGraphModel : public PatchEdits {
public:
    using PatchEdits::PatchEdits;

    FxType slotType(int slot) const;
    bool nodeShown(int node) const;
    std::vector<FxWireRef> wires() const;
    // The line the wires make, with any effect they leave out put at its end (on the high band while a split is open).
    FxLine line() const;
    // Wires the graph as `line` says, the splits and joins it holds in use and the others not, the join's band
    // levels kept, in the undo step the caller holds.
    void writeLine(const FxLine &line);
    // The slots that hold an effect, in the line's order: the main line's, and each split's high band's then its low
    // band's.
    static std::vector<int> chain(const FxLine &line);
    std::vector<int> chain() const { return chain(line()); }
    // `rest` with `item` put in at `at` (an effect onto a band takes only its node).
    static FxLine lineWith(FxLine rest, const FxLineItem &item, FxSlotAt at);
    // Whether the join of a split on the line lets a band of it through (`lane` 1 the high one, 2 the low one, 3 the
    // middle one): not while another band is soloed and it isn't.
    bool bandSoloedOut(const FxLineItem &item, int lane) const;

    // The first slot without an effect, or -1; and that slot filled (fresh, or as `copyOf`), unfolded, not in the line.
    int freeSlot() const;
    void fillEffect(int slot, FxType type, int copyOf);
    // Fills an empty slot with a fresh effect of `type`, or a copy of slot `copyOf`, and puts it in the line (see
    // placeNew()), picked, as one undo step: the slot, or -1 (with a message) where every one is taken.
    int addEffect(FxType type, int copyOf, FxSlotAt at);
    // An effect just put in `slot`, into the line: a copy right after its original (in the row at the line's end, or
    // its band's), else `where` the list was opened, else after the node picked, else at the line's end. Picked.
    void placeNew(int slot, int copyOf, FxSlotAt where);
    // Takes it out of the line, which closes up behind it, with the modulation of its knobs.
    void removeEffect(int slot);
    // Every effect (and every split and join) gone, one undo step, the modulation of their knobs with them: the slots
    // emptied.
    std::vector<int> clear();
    bool anyToClear() const;
    // A split with its join right after it, its bands empty (`bands` 2 or 3, see SplitParam::Bands): on the main line
    // at `at` (lane 0 before an item), else at the line's end, picked, one undo step. Its node, or -1 (with a message
    // where every split is in use; none while a split left open ends the line).
    int addSplit(int bands, FxSlotAt at);
    // The main line's item that holds `node` (a split's, for its join), if any.
    std::optional<FxLineItem> itemOf(int node) const;
    // An effect, or a split with its join (a join's removes its split). A split's bands' effects stay on the main line
    // in its place, high band first, or (`withEffects`) go with it, with their modulation. One undo step. The effects
    // removed.
    std::vector<int> removeNode(int node, bool withEffects);
    // The other bands of the split whose band holds effect `slot`, low to high by lane (none off a band); and the
    // effect moved to the end of one of them (`lane` as FxLineItem's), one undo step.
    std::vector<int> otherBands(int slot) const;
    void moveToBand(int slot, int lane);
    bool splitThree(int split) const; // split in three bands
    // The effects on a split's middle band (none while it isn't on the line or is split in two).
    std::vector<int> middleEffects(int split) const;
    // A split in two bands or three (see SplitParam::Bands), one undo step. Three: an empty middle band opens between
    // the two, shown, its crossover two octaves above the low one, and its join takes it in at 0 dB. Two: the middle
    // band goes, its effects onto the low band's end (`midTo` 2), the high band's start (1) or removed (any other).
    // The effects removed.
    std::vector<int> setSplitBands(int split, int bands, int midTo);
    // The band a split shows: 1 its high band, 2 its low one, 3 its middle one (FxSlotAt), as its tab says (UiLayout's
    // splitBand: 0 the low band, 1 the high one, 2 the middle one, the low one while it is split in two).
    int splitLane(int split) const;
    void showSplitBand(int split, int band); // 0 its low band, 1 its high one, 2 its middle one
    bool splitFolded(int split) const;
    void setSplitFolded(int split, bool folded);
    // A copy of `item` (an effect, or a split with its join and the effects on its bands): its settings, not its
    // modulation, in free slots and a free split, shown folded as the original; `copies` maps each node to its copy.
    // None, with a message, where there aren't enough.
    std::optional<FxLineItem> copyItem(const FxLineItem &item, std::map<int, int> &copies);

private:
    ModulationModel modulation() const { return ModulationModel(controller_, messages_); }
};

} // namespace fmvibe::gui
