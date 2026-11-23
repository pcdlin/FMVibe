#pragma once

// The patch's modulation as the editor edits it (see patchedits.h): the sources in their places, what each modulates
// through its slots (each slot's target, depth, Amount, curve, aux and bypass), the modulations the MODS view holds
// with no source or no target yet, and what a source may modulate at all. Each edit is one undo step.

#include "patchedits.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

struct Assignment {
    int source;
    int slot;
    int target;
    double amount;
    bool bypassed; // kept, but modulating nothing for now
    bool bipolar;  // swings its target both ways, as its source does (see sourceBipolar())
    double scale;  // its Amount, which scales the depth (see SlotAuxParam)
    int aux;       // the source that scales it too, or -1
};

// How far a modulation takes its target from the set value, down and up, as a share of its range: bipolar, half its
// depth either way; unipolar, all of it one way.
inline std::pair<double, double> modReach(const Assignment &m) {
    const double depth = m.amount * m.scale; // as far as an aux, if it has one, lets it go
    if (m.bipolar)
        return {-0.5 * std::abs(depth), 0.5 * std::abs(depth)};
    return {std::min(depth, 0.0), std::max(depth, 0.0)};
}

// An empty modulation's row stands in for a slot as emptyModSlot(its place in the list), always below -1;
// emptyModOf() takes it back, -1 for a slot of a source.
constexpr int emptyModSlot(int row) {
    return -2 - row;
}
constexpr int emptyModOf(int slot) {
    return slot <= -2 ? -2 - slot : -1;
}

class ModulationModel : public PatchEdits {
public:
    using PatchEdits::PatchEdits;

    // Modulation sources. The places that hold one, in the order they show.
    std::vector<int> sources() const;
    SourceType sourceTypeOf(int source) const;
    // The shape the source in place `source` runs as an LFO (see fmvibe::lfoShape()): Random for a Random source.
    LfoShape lfoShapeOf(int source) const;
    std::string sourceName(int source) const;
    // A param's name as the editor shows it: a source's fields go by the source's name, not their place's, and an
    // effect's by its type, not its slot's number (see fxParamName()).
    std::string paramName(int index) const;
    // An effect's field by the effect's name: "FX Chorus Rate" for a slot's "FX 7 Chorus Rate", "FX Chorus 2 Rate" for
    // the second chorus along the line where there are more, "FX Chorus On" for a field every effect has.
    std::string fxParamName(int index) const;
    bool sourceOn(int source) const;
    // Whether a source swings its targets both ways (see fmvibe::sourceBipolar()).
    bool sourceBipolar(int source) const;
    // What a double-click or Reset sets a param to: its default, or a bipolar macro's middle.
    double resetValue(int param) const;

    // Every modulation: the sources' slots that have a target, the places' (in `places`' order, as sources() has
    // them), then the macros', the note source's, the mod wheel's, velocity's, the pitch bend's and the operators'.
    std::vector<Assignment> assignments() const;
    std::vector<Assignment> assignments(const std::vector<int> &places) const;
    std::vector<int> usedSlots(int source) const;
    bool slotBypassed(int source, int slot) const;
    // A slot of `source` modulating `target` at a moderate depth, upward to begin with; refused, with a message, where
    // the source can't modulate it (see canModulate()), modulates it already or has no slot free.
    void assign(int source, int target);
    // A modulation gone (an empty one's row too), with what modulated its depth.
    void removeSlot(int source, int slot);
    // The MODS view's empty modulations onto a param `gone` says no longer is (an effect taken out, a split's
    // crossover out of use) lose their target, as a removed source's do, so nothing they are given modulates it.
    void forgetEmptyTargets(const std::function<bool(int)> &gone);
    void toggleBypass(int source, int slot);
    // Makes a source swing every one of its targets both ways, or one way.
    void setSourcePolarity(int source, bool bipolar);
    // Changing a modulation from its row. Moving it to another source takes a free slot there, keeping its target,
    // depth, polarity (as it is now), Amount, aux and bypass, as one undo step; refused when that source modulates the
    // target already or has no slot free. Changing its target keeps the rest; refused when the source modulates that
    // target already. An aux of -1 takes it away.
    void moveModulation(int source, int slot, int to);
    void retarget(int source, int slot, int target);
    void setAux(int source, int slot, int aux);
    // Whether `source` may modulate `target` at all: a macro or the mod wheel can't move its own knob, and a macro not
    // the knob of a macro its own knob moves (through the macros' modulations of one another), which would loop. And
    // what the menus note, and the message that flashes up, where it can't.
    bool canModulate(int source, int target) const;
    bool closesLoop(int source, int target) const;
    std::string refusedNote(int source, int target) const;
    std::string refusedMessage(int source, int target) const;

    // The MODS view's empty modulations (UiLayout::emptyMods). An empty modulation's source and target, -1 for none: a
    // source since removed, or a target no longer there, reads as none.
    std::pair<int, int> emptyMod(int row) const;
    // What a row modulates: its slot's target, or an empty modulation's.
    int modsRowTarget(int source, int slot) const;
    // Adds an empty modulation from `source` (-1 for none yet) at the end of the list, one undo step; its row, or -1,
    // refused past kMaxEmptyMods.
    int addEmptyMod(int source);
    // Picks an empty modulation's source, or its target (-1 leaves it as it is). With both, it becomes a modulation of
    // that source as assign() makes one, and leaves the list; refused, as assign() refuses, the row stays as it was.
    // One undo step. Where it is now, as a source and a slot (an empty one's emptyModSlot()); none where it was
    // refused.
    std::optional<std::pair<int, int>> fillEmptyMod(int row, int source, int target);
    void removeEmptyMod(int row);

    // Shows the sources in the order `shown`, the free places after them.
    void setSourceOrder(const std::vector<int> &shown);
    // Fills a free place with a fresh source of `type`, or a copy of `copyOf`, shown after the others or the original,
    // unfolded, with the modulation panel open: the place it fills, or -1 with none free.
    int addSource(SourceType type, int copyOf = -1);
    // Takes it out with its modulation, and whatever modulates its knobs.
    void removeSource(int source);
};

} // namespace fmvibe::gui
