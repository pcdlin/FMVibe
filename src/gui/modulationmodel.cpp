#include "modulationmodel.h"

#include "fxgraphmodel.h"
#include "routing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

// --- Sources

std::vector<int> ModulationModel::sources() const {
    return shownSources(controller_.layout(), [this](int index) { return value(index); });
}

SourceType ModulationModel::sourceTypeOf(int source) const {
    return isStripSource(source) ? SourceType::None : sourceType(value(paramIndex(source, SourceParam::Type)));
}

LfoShape ModulationModel::lfoShapeOf(int source) const {
    return lfoShape([this](int i) { return value(i); }, source);
}

std::string ModulationModel::sourceName(int source) const {
    return fmvibe::gui::sourceName(controller_.layout(), [this](int index) { return value(index); }, source);
}

namespace {

// The words every field of an effect type begins with past its slot's "FX n" ("Chorus" for "Chorus Rate", "3-Band Dyn"
// for "3-Band Dyn Low Gain"), where the number of a repeated effect goes; empty for a type whose fields share none.
const std::array<std::string, static_cast<size_t>(FxType::Count)> &fxFieldPrefixes(const ParamTable &table) {
    static const auto prefixes = [&table] {
        std::array<std::string, static_cast<size_t>(FxType::Count)> out{};
        std::array<std::vector<std::string>, static_cast<size_t>(FxType::Count)> shared{};
        std::array<bool, static_cast<size_t>(FxType::Count)> seen{};
        const size_t slotName = trf(Str::ParamFx, {"1"}).size() + 1;
        for (int f = 0; f < kFxParamCount; ++f) {
            const auto type = static_cast<size_t>(fxTypeOfField(static_cast<FxParam>(f)));
            const std::string &name = table[paramIndex(0, static_cast<FxParam>(f))].name;
            std::vector<std::string> words;
            for (size_t at = slotName; at < name.size();) {
                const size_t end = std::min(name.find(' ', at), name.size());
                words.push_back(name.substr(at, end - at));
                at = end + 1;
            }
            if (!words.empty())
                words.pop_back(); // the field's own word
            if (!seen[type]) {
                shared[type] = words;
                seen[type] = true;
            } else {
                size_t n = 0;
                while (n < shared[type].size() && n < words.size() && shared[type][n] == words[n])
                    ++n;
                shared[type].resize(n);
            }
        }
        for (size_t type = 0; type < out.size(); ++type)
            for (const std::string &word : shared[type])
                out[type] += (out[type].empty() ? "" : " ") + word;
        return out;
    }();
    return prefixes;
}

} // namespace

std::string ModulationModel::fxParamName(int index) const {
    const int slot = fxSlotOf(index);
    const std::string &name = table_[index].name;
    const std::string slotName = trf(Str::ParamFx, {std::to_string(slot + 1)});
    const FxType type = fxType(value(paramIndex(slot, FxParam::Type)));
    if (type == FxType::None || name.compare(0, slotName.size() + 1, slotName + " ") != 0)
        return name;
    const std::string field = name.substr(slotName.size() + 1);
    // Numbered in the line's order among the effects of its type, where there are more than one. The line is worked
    // out from the wires only then: most patches hold one effect of a type, whose names are asked for on every paint.
    int number = 0, count = 0;
    bool another = false;
    for (int other = 0; other < kFxSlots && !another; ++other)
        another = other != slot && fxType(value(paramIndex(other, FxParam::Type))) == type;
    if (another)
        for (int other : FxGraphModel(controller_).chain())
            if (fxType(value(paramIndex(other, FxParam::Type))) == type) {
                ++count;
                if (other == slot)
                    number = count;
            }
    const std::string n = count > 1 && number > 0 ? std::to_string(number) : std::string();
    // FX before it all, so an effect stands apart from a voice's shaper or filter of the same name ("FX Shaper 1 Drive",
    // "Shaper 1 Drive").
    auto fx = [](const std::string &named) { return trf(Str::ParamFxNamed, {named}); };
    if (fxTypeOfField(fxFieldOf(index)) == FxType::None) // a field every effect has: by the effect's name
        return fx(std::string(fxName(type)) + (n.empty() ? "" : " " + n) + " " + field);
    if (n.empty())
        return fx(field);
    const std::string &prefix = fxFieldPrefixes(table_)[static_cast<size_t>(type)];
    if (!prefix.empty() && field.compare(0, prefix.size() + 1, prefix + " ") == 0)
        return fx(prefix + " " + n + field.substr(prefix.size()));
    return fx(field + " " + n);
}

std::string ModulationModel::paramName(int index) const {
    if (index < 0 || index >= kParamCount)
        return std::string();
    if (const int owner = bypassSourceOf(index); owner >= 0) {
        const int target = slotTarget(value(slotTargetIndex(owner, bypassSlotOf(index))));
        return target >= 0 ? trf(Str::ParamNameBypassOn, {sourceName(owner), paramName(target)})
                           : trf(Str::ParamNameBypass, {sourceName(owner)});
    }
    if (isMacroKnob(index)) // by the name it is given
        return sourceName(kMaxSources + macroOf(index));
    if (fxSlotOf(index) >= 0)
        return fxParamName(index);
    const int source = sourceOf(index);
    const std::string &name = table_[index].name;
    if (source < 0)
        return name;
    const size_t place = trf(Str::ParamMod, {std::to_string(source + 1)}).size() + 1; // see buildTable()
    return sourceName(source) + " " + name.substr(place);
}

bool ModulationModel::sourceOn(int source) const {
    if (isOpSource(source)) // an operator switched off puts out nothing
        return value(paramIndex(source - kOpSource, OpParam::Enabled)) >= 0.5;
    return isStripSource(source) || value(sourceEnabledIndex(source)) >= 0.5;
}

bool ModulationModel::sourceBipolar(int source) const {
    return fmvibe::sourceBipolar([this](int index) { return value(index); }, source);
}

double ModulationModel::resetValue(int param) const {
    // A bipolar macro moves nothing at its knob's middle, so that is where it goes back to.
    if (const int macro = macroOf(param);
        macro >= 0 && param == paramIndex(macro, MacroParam::Value) && sourceBipolar(kMaxSources + macro))
        return 0.5;
    return table_[param].def;
}

// --- Modulations

std::vector<Assignment> ModulationModel::assignments() const {
    return assignments(sources());
}

std::vector<Assignment> ModulationModel::assignments(const std::vector<int> &places) const {
    std::vector<Assignment> list;
    std::vector<int> all = places;
    for (int macro = 0; macro < kMacros; ++macro) // the macros count as sources after the places, then the note's
        all.push_back(kMaxSources + macro);         // and the mod wheel's
    all.push_back(kNoteSource);
    all.push_back(kWheelSource);
    all.push_back(kVelocitySource);
    all.push_back(kBendSource);
    for (int op = 0; op < kOps; ++op)
        all.push_back(kOpSource + op);
    for (int s : all) {
        const bool bipolar = sourceBipolar(s);
        for (int slot = 0; slot < kSlots; ++slot) {
            const int target = slotTarget(value(slotTargetIndex(s, slot)));
            if (target >= 0)
                list.push_back({s, slot, target, value(slotAmountIndex(s, slot)), slotBypassed(s, slot), bipolar,
                                value(slotScaleIndex(s, slot)),
                                slotAux([this](int index) { return value(index); }, s, slot)});
        }
    }
    return list;
}

std::vector<int> ModulationModel::usedSlots(int source) const {
    std::vector<int> slots;
    for (int slot = 0; slot < kSlots; ++slot)
        if (slotTarget(value(slotTargetIndex(source, slot))) >= 0)
            slots.push_back(slot);
    return slots;
}

void ModulationModel::assign(int source, int target) {
    if (!canModulate(source, target)) {
        say(refusedMessage(source, target));
        return;
    }
    const UndoScope step(controller_); // with the route it may keep
    int free = -1;
    for (int slot = kSlots - 1; slot >= 0; --slot) {
        const int current = slotTarget(value(slotTargetIndex(source, slot)));
        if (current == target) {
            say(trf(Str::MsgAlreadyModulates, {sourceName(source), paramName(target)}));
            return;
        }
        if (current < 0)
            free = slot;
    }
    if (free < 0) {
        say(trf(Str::MsgSlotsFull, {sourceName(source), std::to_string(kSlots)}));
        return;
    }
    const double amount = 0.3; // a moderate depth, always upward to begin with, swinging as its source does
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, free), slotTargetValue(target)},
                                                {slotAmountIndex(source, free), amount},
                                                {slotScaleIndex(source, free), 1.0},
                                                {slotCurveIndex(source, free), 0.0},
                                                {slotAuxDepthIndex(source, free), 1.0},
                                                {slotAuxIndex(source, free), 0.0},
                                                {slotBypassIndex(source, free), 0.0}};
    editMany(changes);
    // A modulated route opens and closes with the modulation, so its wire shows, even at 0 %.
    std::vector<int> &kept = controller_.layout().keptRoutes;
    if (isRouteParam(target) && std::find(kept.begin(), kept.end(), target) == kept.end()) {
        kept.push_back(target);
        controller_.layoutChanged();
    }
}

void ModulationModel::removeSlot(int source, int slot) {
    if (const int row = emptyModOf(slot); row >= 0) {
        removeEmptyMod(row);
        return;
    }
    const UndoScope step(controller_); // with what goes along
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, slot), 0.0},
                                                {slotAmountIndex(source, slot), 0.0},
                                                {slotScaleIndex(source, slot), 1.0},
                                                {slotCurveIndex(source, slot), 0.0},
                                                {slotAuxDepthIndex(source, slot), 1.0},
                                                {slotAuxIndex(source, slot), 0.0},
                                                {slotBypassIndex(source, slot), 0.0}};
    editMany(changes);
    // What modulated its depth goes too, rather than wait on the emptied slot to scale the next modulation there;
    // and an empty modulation onto it loses its target. The slot is empty by now, so two modulations of each other's
    // depths end.
    const int depth = slotAmountIndex(source, slot);
    for (const Assignment &m : assignments())
        if (m.target == depth)
            removeSlot(m.source, m.slot);
    bool emptied = false;
    for (auto &[from, onto] : controller_.layout().emptyMods)
        if (onto == depth) {
            onto = -1;
            emptied = true;
        }
    if (emptied)
        controller_.layoutChanged();
}

void ModulationModel::forgetEmptyTargets(const std::function<bool(int)> &gone) {
    bool emptied = false;
    for (auto &[from, target] : controller_.layout().emptyMods)
        if (target >= 0 && gone(target)) {
            target = -1;
            emptied = true;
        }
    if (emptied)
        controller_.layoutChanged();
}

bool ModulationModel::slotBypassed(int source, int slot) const {
    return slot >= 0 && value(slotBypassIndex(source, slot)) >= 0.5;
}

void ModulationModel::toggleBypass(int source, int slot) {
    if (slot >= 0) // an empty modulation has nothing to bypass
        edit(slotBypassIndex(source, slot), slotBypassed(source, slot) ? 0.0 : 1.0);
}

void ModulationModel::setSourcePolarity(int source, bool bipolar) {
    const Polarity polarity = polarityOf(source, sourceTypeOf(source));
    edit(polarity.index, bipolar ? polarity.bipolar : 1.0 - polarity.bipolar);
}

bool ModulationModel::closesLoop(int source, int target) const {
    return isMacro(source) && isMacroKnob(target) && target != stripKnobIndex(source) &&
           macroReaches([this](int i) { return value(i); }, macroOf(target), knobOf(source));
}

bool ModulationModel::canModulate(int source, int target) const {
    return isModTarget(target) && !(hasKnob(source) && target == stripKnobIndex(source)) &&
           !closesLoop(source, target);
}

std::string ModulationModel::refusedNote(int source, int target) const {
    return closesLoop(source, target) ? tr(Str::CascadeLoop) : tr(Str::CascadeOwnKnob);
}

std::string ModulationModel::refusedMessage(int source, int target) const {
    return closesLoop(source, target) ? trf(Str::MsgMacroLoop, {sourceName(source), paramName(target)})
                                      : trf(Str::MsgOwnKnob, {sourceName(source)});
}

void ModulationModel::moveModulation(int source, int slot, int to) {
    const int target = slotTarget(value(slotTargetIndex(source, slot)));
    if (target < 0 || to < 0 || to == source)
        return;
    if (!canModulate(to, target)) {
        say(refusedMessage(to, target));
        return;
    }
    int free = -1;
    for (int s = kSlots - 1; s >= 0; --s) {
        const int current = slotTarget(value(slotTargetIndex(to, s)));
        if (current == target) {
            say(trf(Str::MsgAlreadyModulates, {sourceName(to), paramName(target)}));
            return;
        }
        if (current < 0)
            free = s;
    }
    if (free < 0) {
        say(trf(Str::MsgSlotsFull, {sourceName(to), std::to_string(kSlots)}));
        return;
    }
    // It swings as its new source does; an aux that would be the new source itself goes.
    const int aux = slotAux([this](int index) { return value(index); }, source, slot);
    const UndoScope step(controller_);
    std::vector<std::pair<int, double>> changes{
        {slotTargetIndex(to, free), slotTargetValue(target)},
        {slotAmountIndex(to, free), value(slotAmountIndex(source, slot))},
        {slotScaleIndex(to, free), value(slotScaleIndex(source, slot))},
        {slotCurveIndex(to, free), value(slotCurveIndex(source, slot))},
        {slotAuxDepthIndex(to, free), value(slotAuxDepthIndex(source, slot))},
        {slotAuxIndex(to, free), slotAuxValue(aux == to ? -1 : aux)},
        {slotBypassIndex(to, free), slotBypassed(source, slot) ? 1.0 : 0.0}};
    editMany(changes);
    // What modulates its depth follows it to its new slot where that depth can be modulated (an LFO's or an
    // envelope's), unless its source modulates that already; else it goes, rather than wait in the old slot for the
    // next modulation there. So does an empty modulation onto it.
    const int depth = slotAmountIndex(source, slot), newDepth = slotAmountIndex(to, free);
    for (const Assignment &m : assignments()) {
        if (m.target != depth)
            continue;
        const std::vector<int> used = usedSlots(m.source);
        const bool follows = isModTarget(newDepth) && std::none_of(used.begin(), used.end(), [&](int s) {
                                 return slotTarget(value(slotTargetIndex(m.source, s))) == newDepth;
                             });
        if (follows)
            edit(slotTargetIndex(m.source, m.slot), slotTargetValue(newDepth));
        else
            removeSlot(m.source, m.slot);
    }
    bool emptied = false;
    for (auto &[from, onto] : controller_.layout().emptyMods)
        if (onto == depth) {
            onto = isModTarget(newDepth) ? newDepth : -1;
            emptied = true;
        }
    if (emptied)
        controller_.layoutChanged();
    removeSlot(source, slot);
}

void ModulationModel::retarget(int source, int slot, int target) {
    const int current = slotTarget(value(slotTargetIndex(source, slot)));
    if (current < 0 || target == current || target < 0)
        return;
    if (!canModulate(source, target)) {
        say(refusedMessage(source, target));
        return;
    }
    for (int s = 0; s < kSlots; ++s)
        if (s != slot && slotTarget(value(slotTargetIndex(source, s))) == target) {
            say(trf(Str::MsgAlreadyModulates, {sourceName(source), paramName(target)}));
            return;
        }
    const UndoScope step(controller_); // with the route it may keep
    edit(slotTargetIndex(source, slot), slotTargetValue(target));
    // A modulated route opens and closes with the modulation, so its wire shows, even at 0 %, as in assign().
    std::vector<int> &kept = controller_.layout().keptRoutes;
    if (isRouteParam(target) && std::find(kept.begin(), kept.end(), target) == kept.end()) {
        kept.push_back(target);
        controller_.layoutChanged();
    }
}

// Taking the aux away puts its depth back to all of it, for the next aux; another aux keeps it.
void ModulationModel::setAux(int source, int slot, int aux) {
    if (aux == source)
        aux = -1;
    if (aux >= 0) {
        edit(slotAuxIndex(source, slot), slotAuxValue(aux));
        return;
    }
    std::vector<std::pair<int, double>> changes{{slotAuxIndex(source, slot), 0.0},
                                                {slotAuxDepthIndex(source, slot), 1.0}};
    const UndoScope step(controller_);
    editMany(changes);
}

// --- Empty modulations

std::pair<int, int> ModulationModel::emptyMod(int row) const {
    const std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return {-1, -1};
    const auto [source, target] = empty[row];
    const bool sourceThere = source >= 0 && source < kAllSources &&
                             (isStripSource(source) || sourceTypeOf(source) != SourceType::None);
    return {sourceThere ? source : -1, isModTarget(target) ? target : -1};
}

int ModulationModel::modsRowTarget(int source, int slot) const {
    if (const int row = emptyModOf(slot); row >= 0)
        return emptyMod(row).second;
    return source >= 0 && slot >= 0 ? slotTarget(value(slotTargetIndex(source, slot))) : -1;
}

int ModulationModel::addEmptyMod(int source) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (static_cast<int>(empty.size()) >= kMaxEmptyMods) {
        say(trf(Str::MsgEmptyModsFull, {std::to_string(kMaxEmptyMods)}));
        return -1;
    }
    const UndoScope step(controller_);
    empty.push_back({source, -1});
    controller_.layoutChanged();
    return static_cast<int>(empty.size()) - 1;
}

std::optional<std::pair<int, int>> ModulationModel::fillEmptyMod(int row, int source, int target) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return std::nullopt;
    auto [s, t] = emptyMod(row);
    s = source >= 0 ? source : s;
    t = target >= 0 ? target : t;
    if (s < 0 || t < 0) {
        {
            const UndoScope step(controller_);
            empty[row] = {s, t};
            controller_.layoutChanged();
        }
        return std::pair{s, emptyModSlot(row)};
    }
    if (!canModulate(s, t)) {
        say(refusedMessage(s, t));
        return std::nullopt;
    }
    auto modulates = [&] {
        for (int slot = 0; slot < kSlots; ++slot)
            if (slotTarget(value(slotTargetIndex(s, slot))) == t)
                return true;
        return false;
    };
    if (modulates()) {
        say(trf(Str::MsgAlreadyModulates, {sourceName(s), paramName(t)}));
        return std::nullopt;
    }
    {
        const UndoScope step(controller_);
        assign(s, t); // refuses a source with no slot free, saying so
        if (modulates()) {
            empty.erase(empty.begin() + row);
            controller_.layoutChanged();
        }
    }
    for (int slot = 0; slot < kSlots; ++slot)
        if (slotTarget(value(slotTargetIndex(s, slot))) == t)
            return std::pair{s, slot};
    return std::nullopt;
}

void ModulationModel::removeEmptyMod(int row) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return;
    const UndoScope step(controller_);
    empty.erase(empty.begin() + row);
    controller_.layoutChanged();
}

// --- Source places

void ModulationModel::setSourceOrder(const std::vector<int> &shown) {
    UiLayout &layout = controller_.layout();
    std::array<int, kMaxSources> order{};
    size_t n = 0;
    for (int source : shown)
        order[n++] = source;
    for (int source : layout.sourceOrder)
        if (std::find(shown.begin(), shown.end(), source) == shown.end())
            order[n++] = source;
    if (order == layout.sourceOrder)
        return;
    layout.sourceOrder = order;
    controller_.layoutChanged();
}

int ModulationModel::addSource(SourceType type, int copyOf) {
    const UndoScope step(controller_); // with where it shows
    std::vector<int> shown = sources();
    int source = -1;
    for (int s = 0; s < kMaxSources && source < 0; ++s)
        if (sourceTypeOf(s) == SourceType::None)
            source = s;
    if (source < 0) {
        say(trf(Str::MsgSourcesFull, {std::to_string(kMaxSources)}));
        return -1;
    }
    // Every field and its freeform shape: fresh, or as in the original. Its slots start empty either way: a copy
    // modulates nothing until it is given targets of its own. The type comes last, as in FxGraphModel::fillEffect().
    std::vector<std::pair<int, double>> changes;
    auto fresh = [&](int index) { changes.push_back({index, table_[index].def}); };
    auto take = [&](int index, int from) {
        changes.push_back({index, copyOf >= 0 ? value(from) : table_[index].def});
    };
    for (int f = 0; f < kSourceParamCount; ++f) {
        const auto field = static_cast<SourceParam>(f);
        if (field == SourceParam::Type)
            continue;
        if (f >= static_cast<int>(SourceParam::Slots) && f < static_cast<int>(SourceParam::Slots) + 2 * kSlots)
            fresh(paramIndex(source, field)); // a slot's target and depth
        else
            take(paramIndex(source, field), paramIndex(std::max(copyOf, 0), field));
    }
    for (int f = 0; f < kShapeParamCount; ++f)
        take(shapeCountIndex(source) + f, shapeCountIndex(std::max(copyOf, 0)) + f);
    for (int slot = 0; slot < kSlots; ++slot) {
        fresh(slotBypassIndex(source, slot));
        fresh(slotScaleIndex(source, slot));
        fresh(slotCurveIndex(source, slot));
        fresh(slotAuxDepthIndex(source, slot));
        fresh(slotAuxIndex(source, slot));
    }
    // A new Random source swings both ways, as an LFO does; Bipolar, which MIDI shares, starts off. A new Curve source
    // swings one way, Unipolar on, where an LFO's starts off.
    if (type == SourceType::Random && copyOf < 0)
        changes.push_back({paramIndex(source, SourceParam::Bipolar), 1.0});
    if (type == SourceType::Curve && copyOf < 0)
        changes.push_back({paramIndex(source, SourceParam::Unipolar), 1.0});
    changes.push_back({paramIndex(source, SourceParam::Type), static_cast<double>(type)});
    const auto at = copyOf >= 0 ? std::find(shown.begin(), shown.end(), copyOf) + 1 : shown.end();
    shown.insert(at, source);
    setSourceOrder(shown);
    setFolded(false, source, false);
    editMany(changes);
    return source;
}

void ModulationModel::removeSource(int source) {
    const UndoScope step(controller_);
    setFolded(false, source, false);
    // Its own slots, and every slot that modulates one of its knobs; slots it scales as their aux lose it.
    for (const Assignment &m : assignments()) {
        if (m.source == source || sourceOf(m.target) == source)
            removeSlot(m.source, m.slot);
        else if (m.aux == source)
            setAux(m.source, m.slot, -1);
    }
    // Empty modulations from it lose their source, and those onto one of its knobs their target.
    bool emptied = false;
    for (auto &[from, target] : controller_.layout().emptyMods) {
        const bool fromIt = from == source, ontoIt = target >= 0 && sourceOf(target) == source;
        from = fromIt ? -1 : from;
        target = ontoIt ? -1 : target;
        emptied = emptied || fromIt || ontoIt;
    }
    if (emptied)
        controller_.layoutChanged();
    edit(paramIndex(source, SourceParam::Type), static_cast<double>(SourceType::None));
}

} // namespace fmvibe::gui
