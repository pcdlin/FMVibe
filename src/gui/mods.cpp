#include "view.h"

#include "icons.h"
#include "layout.h"
#include "routing.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// The MODS view: every modulation as a row of a table over the routing panel and the inspector, and the cascading
// menus its rows' sources, targets and auxes open (see view.h).
namespace fmvibe::gui {

using namespace detail;

namespace {

// `name` without `prefix` and the space after it, where it starts so: a param's name within its group.
std::string afterPrefix(const std::string &name, const std::string &prefix) {
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 && name[prefix.size()] == ' '
               ? name.substr(prefix.size() + 1)
               : name;
}

// Whether `source` may modulate `target` at all: a macro or the mod wheel can't move its own knob.
bool canModulate(int source, int target) {
    return isModTarget(target) && !(isStripSource(source) && !isNoteBorne(source) && target == stripKnobIndex(source));
}

} // namespace

bool View::modsWide() const {
    return controller_.layout().view == RoutingView::Mods && shapeEditing() < 0 && partialEditing() < 0;
}

// An empty modulation from a source that has rows goes after them, the others at the end. The filter leaves out the
// modulations whose source and target names don't hold every word of it, but not the empty ones.
std::vector<View::Assignment> View::modsRows() const {
    if (derived_.modsRows) // only kept while a scope goes on
        return *derived_.modsRows;
    std::vector<Assignment> rows = modsRowsNow();
    if (derivedDepth_ > 0)
        derived_.modsRows = rows;
    return rows;
}

std::vector<View::Assignment> View::modsRowsNow() const {
    std::vector<Assignment> rows = assignments();
    if (const std::vector<std::string> words = modsFilter_.words(); !words.empty())
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Assignment &m) {
                                      return !TextField::holds(sourceName(m.source) + " " + paramName(m.target), words);
                                  }),
                   rows.end());
    for (int row = 0; row < static_cast<int>(controller_.layout().emptyMods.size()); ++row) {
        const auto [source, target] = emptyMod(row);
        const Assignment empty{source, emptyModSlot(row), target, 0.0, false, source >= 0 && sourceBipolar(source),
                               1.0, -1};
        const auto last = std::find_if(rows.rbegin(), rows.rend(), [&](const Assignment &m) {
            return source >= 0 && m.source == source;
        });
        rows.insert(last == rows.rend() ? rows.end() : last.base(), empty);
    }
    return rows;
}

std::pair<int, int> View::emptyMod(int row) const {
    const std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return {-1, -1};
    const auto [source, target] = empty[row];
    const bool sourceThere = source >= 0 && source < kAllSources &&
                             (isStripSource(source) || sourceTypeOf(source) != SourceType::None);
    return {sourceThere ? source : -1, isModTarget(target) ? target : -1};
}

int View::modsRowTarget(int source, int slot) const {
    if (const int row = emptyModOf(slot); row >= 0)
        return emptyMod(row).second;
    return source >= 0 && slot >= 0 ? slotTarget(value(slotTargetIndex(source, slot))) : -1;
}

// The rows and, after them, the one that adds an empty modulation.
float View::modsEnd() const {
    const float rows = static_cast<float>(modsRows().size() + 1);
    return std::max((rows * kModsRowStep - (kModsRowStep - kModsRowH) - kModsList.h) / kModsRowStep, 0.0f);
}

float View::modsFirst() const {
    return std::clamp(modsGlide_, 0.0f, modsEnd());
}

void View::scrollMods(float first, bool glide) {
    modsScroll_ = std::clamp(first, 0.0f, modsEnd());
    if (!glide)
        modsGlide_ = modsScroll_;
}

Rect View::revealModsRow(int source, int slot) {
    const std::vector<Assignment> rows = modsRows();
    const auto at = std::find_if(rows.begin(), rows.end(),
                                 [&](const Assignment &m) { return m.source == source && m.slot == slot; });
    if (at == rows.end())
        return modsRow(-1.0f);
    const auto row = static_cast<float>(at - rows.begin());
    auto group = at;
    while (group != rows.begin() && source >= 0 && (group - 1)->source == source)
        --group;
    const float shown = std::floor(kModsList.h / kModsRowStep);
    scrollMods(std::max(static_cast<float>(group - rows.begin()), row + 1.0f - shown), false);
    return modsRow(row - modsFirst());
}

// The shown place closes most of the way to where the wheel sent the rows each poll, then settles.
bool View::glideMods() {
    const float to = std::clamp(modsScroll_, 0.0f, modsEnd());
    const float from = modsFirst();
    if (std::abs(to - from) < 0.005f) {
        const bool moved = modsGlide_ != to;
        modsGlide_ = to;
        return moved;
    }
    modsGlide_ = from + (to - from) * 0.35f;
    return true;
}

Rect View::modsThumb() const {
    const Rect track = modsScrollTrack();
    const float rows = static_cast<float>(modsRows().size() + 1);
    const float shown = kModsList.h / kModsRowStep;
    const float h = std::max(track.h * std::min(shown / std::max(rows, 1.0f), 1.0f), 16.0f);
    return {track.x, track.y + (track.h - h) * modsFirst() / std::max(modsEnd(), 0.001f), track.w, h};
}

void View::moveModulation(int source, int slot, int to) {
    const int target = slotTarget(value(slotTargetIndex(source, slot)));
    if (target < 0 || to < 0 || to == source)
        return;
    if (!canModulate(to, target)) {
        flash(trf(Str::MsgOwnKnob, {sourceName(to)}));
        return;
    }
    int free = -1;
    for (int s = kSlots - 1; s >= 0; --s) {
        const int current = slotTarget(value(slotTargetIndex(to, s)));
        if (current == target) {
            flash(trf(Str::MsgAlreadyModulates, {sourceName(to), paramName(target)}));
            return;
        }
        if (current < 0)
            free = s;
    }
    if (free < 0) {
        flash(trf(Str::MsgSlotsFull, {sourceName(to), std::to_string(kSlots)}));
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

void View::retarget(int source, int slot, int target) {
    const int current = slotTarget(value(slotTargetIndex(source, slot)));
    if (current < 0 || target == current || target < 0)
        return;
    if (!canModulate(source, target)) {
        flash(trf(Str::MsgOwnKnob, {sourceName(source)}));
        return;
    }
    for (int s = 0; s < kSlots; ++s)
        if (s != slot && slotTarget(value(slotTargetIndex(source, s))) == target) {
            flash(trf(Str::MsgAlreadyModulates, {sourceName(source), paramName(target)}));
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

void View::setAux(int source, int slot, int aux) {
    edit(slotAuxIndex(source, slot), slotAuxValue(aux == source ? -1 : aux));
}

void View::addEmptyMod(int source) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (static_cast<int>(empty.size()) >= kMaxEmptyMods) {
        flash(trf(Str::MsgEmptyModsFull, {std::to_string(kMaxEmptyMods)}));
        return;
    }
    {
        const UndoScope step(controller_);
        empty.push_back({source, -1});
        controller_.layoutChanged();
    }
    const int row = static_cast<int>(empty.size()) - 1;
    const Rect r = revealModsRow(source, emptyModSlot(row));
    if (source < 0)
        openCascade(CascadeKind::Source, -1, emptyModSlot(row), modsCell(r, kModsSource));
    else
        openCascade(CascadeKind::Target, source, emptyModSlot(row), modsCell(r, kModsTarget));
}

Rect View::modsFilterRect() const {
    return modsFilter(modsFilterOpen());
}

std::string View::modsFilterCount() const {
    const std::vector<Assignment> rows = modsRows();
    const auto shown = std::count_if(rows.begin(), rows.end(), [](const Assignment &m) { return m.slot >= 0; });
    return trf(Str::ModsMatching, {std::to_string(shown), std::to_string(assignments().size())});
}

void View::modsFilterKey(Key key) {
    if (key == Key::Enter)
        modsFiltering_ = false;
    else if (key == Key::Escape && !modsFilter_.text.empty())
        clearField(Field::ModsFilter);
    else if (key == Key::Escape)
        modsFiltering_ = false;
}

void View::showView(RoutingView view) {
    controller_.layout().view = view;
    cascade_.reset();
    modsFiltering_ = false;
    shapeEditor_ = -1;
    partialEditor_ = -1;
    fxMenuOpen_ = false;
    listMenu_ = -1;
}

void View::showSourceMods(int source) {
    showView(RoutingView::Mods);
    clearField(Field::ModsFilter); // all of its rows
    const std::vector<Assignment> rows = modsRows();
    for (size_t row = 0; row < rows.size(); ++row)
        if (rows[row].source == source) { // its first row at the top, as far as the rows go
            scrollMods(static_cast<float>(row), false);
            return;
        }
    addEmptyMod(source);
}

void View::fillEmptyMod(int row, int source, int target) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return;
    auto [s, t] = emptyMod(row);
    s = source >= 0 ? source : s;
    t = target >= 0 ? target : t;
    if (s < 0 || t < 0) {
        {
            const UndoScope step(controller_);
            empty[row] = {s, t};
            controller_.layoutChanged();
        }
        revealModsRow(s, emptyModSlot(row));
        return;
    }
    if (!canModulate(s, t)) {
        flash(trf(Str::MsgOwnKnob, {sourceName(s)}));
        return;
    }
    auto modulates = [&] {
        for (int slot = 0; slot < kSlots; ++slot)
            if (slotTarget(value(slotTargetIndex(s, slot))) == t)
                return true;
        return false;
    };
    if (modulates()) {
        flash(trf(Str::MsgAlreadyModulates, {sourceName(s), paramName(t)}));
        return;
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
            revealModsRow(s, slot);
}

void View::removeEmptyMod(int row) {
    std::vector<std::pair<int, int>> &empty = controller_.layout().emptyMods;
    if (row < 0 || row >= static_cast<int>(empty.size()))
        return;
    const UndoScope step(controller_);
    empty.erase(empty.begin() + row);
    controller_.layoutChanged();
}

// While the target menu's search holds something, its first level lists what it finds in the categories, flat, each
// with its path.
View::MenuNode View::cascadeTree() const {
    if (derived_.cascade) // only kept while a scope goes on
        return *derived_.cascade;
    MenuNode tree = cascadeTreeNow();
    if (derivedDepth_ > 0)
        derived_.cascade = std::make_shared<const MenuNode>(tree);
    return tree;
}

View::MenuNode View::cascadeTreeNow() const {
    MenuNode root = cascadeCategories();
    if (!cascadeSearches() || cascadeSearch_.text.empty())
        return root;
    const std::vector<std::string> words = cascadeSearch_.words();
    MenuNode found;
    int more = 0;
    auto walk = [&](auto &self, const MenuNode &node, const std::string &path) -> void {
        for (const MenuNode &child : node.children) {
            if (!child.children.empty()) {
                self(self, child, path + child.label + " \xE2\x80\xBA ");
                continue;
            }
            if (child.heading || child.pick < 0 || !TextField::holds(path + child.label, words))
                continue;
            if (static_cast<int>(found.children.size()) >= kCascadeMatches) {
                ++more;
                continue;
            }
            MenuNode match = child;
            match.path = path;
            found.children.push_back(std::move(match));
        }
    };
    walk(walk, root, "");
    auto aside = [&](std::string label) { // can't be picked
        MenuNode node;
        node.label = std::move(label);
        node.enabled = false;
        found.children.push_back(std::move(node));
    };
    if (found.children.empty())
        aside(tr(Str::CascadeNoMatch));
    else if (more > 0)
        aside(trf(Str::CascadeMore, {std::to_string(more)}));
    return found;
}

View::MenuNode View::cascadeCategories() const {
    MenuNode root;
    if (!cascade_)
        return root;
    const Cascade &menu = *cascade_;
    auto get = [this](int index) { return value(index); };
    const int target = modsRowTarget(menu.source, menu.slot);
    const int current = menu.kind == CascadeKind::Target ? target
                        : menu.kind == CascadeKind::Aux  ? slotAux(get, menu.source, menu.slot)
                                                         : menu.source;
    auto category = [](std::string label) {
        MenuNode node;
        node.label = std::move(label);
        return node;
    };
    auto heading = [](Str label) {
        MenuNode node;
        node.label = tr(label);
        node.heading = true;
        node.enabled = false;
        return node;
    };
    auto pick = [&](std::string label, int value, std::optional<Color> color = std::nullopt) {
        MenuNode node;
        node.label = std::move(label);
        node.pick = value;
        node.color = color;
        node.checked = value == current;
        return node;
    };
    // Marks the categories that hold what is checked, and says whether `node` does.
    auto markHolds = [](MenuNode &node) {
        auto mark = [](auto &self, MenuNode &n) -> bool {
            bool holds = n.checked;
            for (MenuNode &child : n.children)
                holds = self(self, child) || holds;
            n.holds = holds && !n.children.empty();
            return holds;
        };
        mark(mark, node);
    };
    auto keep = [&](MenuNode node) { // a category with nothing in it isn't listed
        if (!node.children.empty())
            root.children.push_back(std::move(node));
    };

    if (menu.kind != CascadeKind::Target) {
        // A source can't take a modulation it has already, nor one past its slots; an aux can't be the row's own source.
        // An empty modulation without a target yet takes any source with a slot free.
        auto sourcePick = [&](int source) {
            MenuNode node = pick(sourceName(source), source, kSourceColors[source]);
            if (menu.kind == CascadeKind::Aux) {
                if (source == menu.source) {
                    node.enabled = false;
                    node.note = tr(Str::CascadeItself);
                }
                return node;
            }
            if (source == menu.source)
                return node;
            const std::vector<int> used = usedSlots(source);
            if (target >= 0 && !canModulate(source, target)) {
                node.enabled = false;
                node.note = tr(Str::CascadeOwnKnob);
            } else if (target >= 0 && std::any_of(used.begin(), used.end(), [&](int s) {
                           return slotTarget(value(slotTargetIndex(source, s))) == target;
                       })) {
                node.enabled = false;
                node.note = tr(Str::CascadeAlready);
            } else if (used.size() >= static_cast<size_t>(kSlots)) {
                node.enabled = false;
                node.note = tr(Str::CascadeSlotsFull);
            }
            return node;
        };
        if (menu.kind == CascadeKind::Aux)
            root.children.push_back(pick(tr(Str::ValueNone), -1));
        MenuNode lfos = category(tr(Str::CascadeLfos)), envs = category(tr(Str::CascadeEnvelopes)),
                 others = category(tr(Str::CascadeNoteMidi));
        for (int source : sources())
            (sourceTypeOf(source) == SourceType::Lfo   ? lfos
             : sourceTypeOf(source) == SourceType::Env ? envs
                                                       : others)
                .children.push_back(sourcePick(source));
        keep(std::move(lfos));
        keep(std::move(envs));
        keep(std::move(others));
        MenuNode macros = category(tr(Str::CascadeMacros));
        for (int macro = 0; macro < kMacros; ++macro)
            macros.children.push_back(sourcePick(kMaxSources + macro));
        keep(std::move(macros));
        MenuNode keyboard = category(tr(Str::CascadeKeyboard));
        for (int source : {kNoteSource, kVelocitySource, kWheelSource})
            keyboard.children.push_back(sourcePick(source));
        keep(std::move(keyboard));
        markHolds(root);
        return root;
    }

    // Targets: what the row's source modulates already, through another slot, can't be picked.
    std::vector<int> taken;
    if (menu.source >= 0)
        for (int s : usedSlots(menu.source))
            if (s != menu.slot)
                taken.push_back(slotTarget(value(slotTargetIndex(menu.source, s))));
    auto targetPick = [&](std::string label, int index, std::optional<Color> color = std::nullopt) {
        MenuNode node = pick(std::move(label), index, color);
        if (std::find(taken.begin(), taken.end(), index) != taken.end()) {
            node.enabled = false;
            node.note = tr(Str::CascadeAlready);
        } else if (!canModulate(menu.source, index)) {
            node.enabled = false;
            node.note = tr(Str::CascadeOwnKnob);
        }
        return node;
    };
    auto add = [&](MenuNode &to, int index, const std::string &prefix) {
        if (isModTarget(index))
            to.children.push_back(targetPick(afterPrefix(table_[index].name, prefix), index));
    };

    // The mod wheel is played, not set: it isn't offered.
    MenuNode global = category(tr(Str::CascadeGlobal));
    for (int g = 0; g < kGlobalCount; ++g)
        if (g != paramIndex(GlobalParam::ModWheel))
            add(global, g, "");
    keep(std::move(global));

    MenuNode operators = category(tr(Str::CascadeOperators));
    for (int op = 0; op < kOps; ++op) {
        MenuNode node = category(kNodeNames[op]);
        node.note = nodeOn(op) ? waveName(static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave)))))
                               : tr(Str::CascadeOff);
        const std::string prefix = trf(Str::ParamOp, {kNodeNames[op]});
        // In two columns, about as tall: the pitch, level, phase and unison, then the envelope and the noise.
        const std::pair<Str, std::vector<OpParam>> sections[] = {
            {Str::CascadePitch, {OpParam::Ratio, OpParam::Transpose, OpParam::Detune, OpParam::FixedFreq}},
            {Str::CascadeLevel, {OpParam::Out}},
            {Str::CascadePhase, {OpParam::Phase}},
            {Str::CascadeUnison, {OpParam::UnisonDetune, OpParam::UnisonSpread, OpParam::PhaseRandom}},
            {Str::CascadeEnvelope,
             {OpParam::Attack, OpParam::Hold, OpParam::Decay, OpParam::Sustain, OpParam::Release, OpParam::AttackCurve,
              OpParam::DecayCurve, OpParam::ReleaseCurve}},
            {Str::CascadeNoise, {OpParam::NoiseSlope}}};
        for (const auto &[title, fields] : sections) {
            node.children.push_back(heading(title));
            node.children.back().newColumn = title == Str::CascadeEnvelope;
            for (OpParam field : fields)
                add(node, paramIndex(op, field), prefix);
        }
        operators.children.push_back(std::move(node));
    }
    keep(std::move(operators));

    // The routes, by the node they come from: into each other node it can feed, and out, then its feedback last.
    MenuNode matrix = category(tr(Str::CascadeMatrix));
    for (int from = 0; from < kNodes; ++from) {
        MenuNode node = category(kNodeNames[from]);
        std::vector<int> order;
        for (int to = 0; to <= kNodes; ++to)
            if (to != from)
                order.push_back(to);
        order.push_back(from);
        for (int to : order) {
            const int param = routeTo(from, to);
            if (!isModTarget(param))
                continue;
            const std::string label = to == from    ? std::string(tr(Str::CascadeFeedback))
                                      : to == kNodes ? "\xE2\x86\x92 " + std::string(tr(Str::BusOut))
                                                     : "\xE2\x86\x92 " + std::string(kNodeNames[to]);
            node.children.push_back(targetPick(label, param, nodeColor(from)));
        }
        matrix.children.push_back(std::move(node));
    }
    keep(std::move(matrix));

    MenuNode filters = category(tr(Str::CascadeFilters));
    for (int filter = 0; filter < kFilters; ++filter) {
        MenuNode node = category(kNodeNames[kOps + filter]);
        const std::string prefix = trf(Str::ParamFilter, {std::to_string(filter + 1)});
        for (FilterParam field : {FilterParam::Cutoff, FilterParam::Resonance, FilterParam::KeyTrack, FilterParam::Drive})
            add(node, paramIndex(filter, field), prefix);
        filters.children.push_back(std::move(node));
    }
    keep(std::move(filters));

    MenuNode shapers = category(tr(Str::CascadeShapers));
    for (int shaper = 0; shaper < kShapers; ++shaper) {
        MenuNode node = category(kNodeNames[kOps + kFilters + shaper]);
        const std::string prefix = trf(Str::ParamShaper, {std::to_string(shaper + 1)});
        for (ShaperParam field : {ShaperParam::Drive, ShaperParam::Output, ShaperParam::Mix})
            add(node, paramIndex(shaper, field), prefix);
        shapers.children.push_back(std::move(node));
    }
    keep(std::move(shapers));

    // Only the effects in the chain, in its order, each with the knobs of its type.
    MenuNode effects = category(tr(Str::CascadeEffects));
    const std::vector<int> chain = fxChain();
    for (size_t pos = 0; pos < chain.size(); ++pos) {
        const int slot = chain[pos];
        MenuNode node = category(std::to_string(pos + 1) + " \xC2\xB7 " + fxName(fxSlotType(slot)));
        node.color = fxColor(slot);
        const std::string prefix = trf(Str::ParamFx, {std::to_string(slot + 1)});
        for (int f = static_cast<int>(FxParam::Enabled) + 1; f < kFxParamCount; ++f) {
            const int index = paramIndex(slot, static_cast<FxParam>(f));
            if (fieldInUse(get, index))
                add(node, index, prefix);
        }
        if (!node.children.empty())
            effects.children.push_back(std::move(node));
    }
    keep(std::move(effects));

    // The sources' own knobs, then the depths of their modulations (another source's modulation scaled by this one).
    MenuNode modulation = category(tr(Str::CascadeModulation));
    for (int source : sources()) {
        MenuNode node = category(sourceName(source));
        node.color = kSourceColors[source];
        node.children.push_back(heading(Str::CascadeFields));
        for (int f = static_cast<int>(SourceParam::Enabled) + 1; f < kSourceParamCount; ++f) {
            const int index = paramIndex(source, static_cast<SourceParam>(f));
            if (index >= slotTargetIndex(source, 0) && index <= slotAmountIndex(source, kSlots - 1))
                continue; // the depths come below, by what they modulate
            if (fieldInUse(get, index) && isModTarget(index))
                node.children.push_back(targetPick(afterPrefix(paramName(index), sourceName(source)), index));
        }
        const std::vector<int> used = usedSlots(source);
        bool depths = false;
        for (int s : used) {
            if (source == menu.source && s == menu.slot)
                continue; // a modulation can't scale its own depth
            if (!depths)
                node.children.push_back(heading(Str::CascadeDepths));
            depths = true;
            node.children.push_back(targetPick("\xE2\x86\x92 " + paramName(slotTarget(value(slotTargetIndex(source, s)))),
                                               slotAmountIndex(source, s)));
        }
        modulation.children.push_back(std::move(node));
    }
    keep(std::move(modulation));
    markHolds(root);
    return root;
}

std::vector<const View::MenuNode *> View::cascadeLevels(const MenuNode &root) const {
    std::vector<const MenuNode *> levels{&root};
    if (!cascade_)
        return levels;
    for (int index : cascade_->open) {
        const MenuNode *level = levels.back();
        if (index < 0 || index >= static_cast<int>(level->children.size()) || level->children[index].children.empty())
            break;
        levels.push_back(&level->children[index]);
    }
    return levels;
}

Rect View::cascadeSearchRect(Rect level) const {
    return {level.x + kCascadePad + 2.0f, level.y + kCascadePad + 2.0f, level.w - 2.0f * kCascadePad - 4.0f, 24.0f};
}

int View::cascadeColumns(const MenuNode &level) {
    return 1 + static_cast<int>(std::count_if(level.children.begin(), level.children.end(),
                                              [](const MenuNode &item) { return item.newColumn; }));
}

Rect View::cascadeItemRect(const MenuNode &level, Rect at, int index, size_t depth) const {
    const float top = at.y + kCascadePad + (depth == 0 && cascadeSearches() ? kCascadeSearchH : 0.0f);
    const float columnW = (at.w - 2.0f * kCascadePad) / static_cast<float>(cascadeColumns(level));
    float y = top;
    int column = 0;
    for (int i = 0; i <= index; ++i) {
        if (level.children[i].newColumn) {
            ++column;
            y = top;
        }
        if (i < index)
            y += level.children[i].heading ? kCascadeHeaderH : kCascadeItemH;
    }
    return {at.x + kCascadePad + columnW * static_cast<float>(column), y, columnW,
            level.children[index].heading ? kCascadeHeaderH : kCascadeItemH};
}

// Each level's columns as wide as its longest entry, in steps, and the level as tall as its tallest column; the first
// under what opened the menu, each next one beside the entry it opens from, all kept inside the editor.
std::vector<Rect> View::cascadeRects(const MenuNode &root) const {
    std::vector<Rect> rects;
    if (!cascade_)
        return rects;
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const float bottom = height() - 8.0f;
    for (size_t k = 0; k < levels.size(); ++k) {
        const MenuNode &level = *levels[k];
        size_t longest = 0;
        const bool search = k == 0 && cascadeSearches();
        float column = 0.0f, tallest = 0.0f;
        for (const MenuNode &item : level.children) {
            longest = std::max(longest, item.path.size() + item.label.size() + item.note.size() + (item.color ? 3 : 0));
            if (item.newColumn)
                column = 0.0f;
            column += item.heading ? kCascadeHeaderH : kCascadeItemH;
            tallest = std::max(tallest, column);
        }
        const float h = 2.0f * kCascadePad + (search ? kCascadeSearchH : 0.0f) + tallest;
        const float w = static_cast<float>(cascadeColumns(level)) *
                        std::clamp(std::ceil((48.0f + 6.4f * static_cast<float>(longest)) / 10.0f) * 10.0f,
                                   search ? kCascadeSearchW : 120.0f, search ? 400.0f : 320.0f);
        Rect r{cascade_->at.x, cascade_->at.y, w, h};
        if (k > 0) {
            const Rect before = rects.back();
            const Rect from = cascadeItemRect(*levels[k - 1], before, cascade_->open[k - 1], k - 1);
            r.x = before.right() + 2.0f;
            if (r.right() > kWidth - 8.0f) // no room on the right: on the left of the level it opens from
                r.x = before.x - 2.0f - w;
            r.y = from.y - kCascadePad;
        }
        r.y = std::max(std::min(r.y, bottom - h), 8.0f);
        r.x = std::clamp(r.x, 8.0f, kWidth - 8.0f - w);
        rects.push_back(r);
    }
    return rects;
}

void View::openCascade(CascadeKind kind, int source, int slot, Rect from) {
    if (cascade_ && cascade_->kind == kind && cascade_->source == source && cascade_->slot == slot) {
        cascade_.reset(); // a second click closes it
        return;
    }
    cascade_ = Cascade{kind, source, slot, {from.x, from.bottom() + 2.0f}, {}};
    cascadeSearch_.clear();
}

void View::cascadeKey(Key key) {
    if (!cascade_)
        return;
    if (key == Key::Escape) {
        if (cascadeSearch_.text.empty())
            cascade_.reset();
        else
            clearField(Field::CascadeSearch);
        return;
    }
    if (cascadeSearch_.text.empty() || (key != Key::Up && key != Key::Down && key != Key::Enter))
        return;
    const MenuNode root = cascadeTree();
    std::vector<int> picks; // the matches that can be picked
    for (size_t i = 0; i < root.children.size(); ++i)
        if (root.children[i].enabled)
            picks.push_back(static_cast<int>(i));
    if (picks.empty())
        return;
    const auto at = std::find(picks.begin(), picks.end(), cascade_->picked);
    const int place = at == picks.end() ? 0 : static_cast<int>(at - picks.begin());
    if (key == Key::Enter)
        pickCascade(0, picks[place]);
    else
        cascade_->picked = picks[std::clamp(place + (key == Key::Down ? 1 : -1), 0, static_cast<int>(picks.size()) - 1)];
}

void View::pickCascade(int level, int index) {
    if (!cascade_)
        return;
    const MenuNode root = cascadeTree();
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    if (level >= static_cast<int>(levels.size()) || index < 0 ||
        index >= static_cast<int>(levels[level]->children.size()))
        return;
    const MenuNode &item = levels[level]->children[index];
    if (!item.children.empty()) { // a category opens beside it
        cascade_->open.resize(level);
        cascade_->open.push_back(index);
        return;
    }
    if (item.heading || !item.enabled)
        return;
    const Cascade menu = *cascade_;
    cascade_.reset();
    const int empty = emptyModOf(menu.slot);
    switch (menu.kind) {
    case CascadeKind::Source:
        if (empty >= 0)
            fillEmptyMod(empty, item.pick, -1);
        else
            moveModulation(menu.source, menu.slot, item.pick);
        break;
    case CascadeKind::Target:
        if (empty >= 0)
            fillEmptyMod(empty, -1, item.pick);
        else
            retarget(menu.source, menu.slot, item.pick);
        break;
    case CascadeKind::Aux:
        setAux(menu.source, menu.slot, item.pick);
        break;
    }
}

void View::paintCascade(Canvas &c) {
    if (!cascade_)
        return;
    const MenuNode root = cascadeTree();
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const std::vector<Rect> rects = cascadeRects(root);
    for (size_t k = 0; k < levels.size(); ++k) {
        const MenuNode &level = *levels[k];
        const Rect r = rects[k];
        c.fillRect({r.x + 2.0f, r.y + 4.0f, r.w, r.h}, kBackground.alpha(0.5f), 8.0f); // a shadow
        c.fillRect(r, kRaised, 8.0f);
        c.strokeRect(r, kBorder, 1.0f, 8.0f);
        const bool searching = k == 0 && cascadeSearches();
        if (searching) { // its search, and the x that empties it
            const Rect search = cascadeSearchRect(r);
            paintField(c, Field::CascadeSearch, search, fieldTyped(search), tr(Str::CascadeSearchHint));
            if (!cascadeSearch_.text.empty()) {
                const Rect x = fieldClear(search);
                const bool on = hover_ && hover_->kind == Kind::SearchClear &&
                                hover_->a == static_cast<int>(Field::CascadeSearch);
                const Point m = x.center();
                c.line({m.x - 3.5f, m.y - 3.5f}, {m.x + 3.5f, m.y + 3.5f}, on ? kText : kDim, 1.3f);
                c.line({m.x - 3.5f, m.y + 3.5f}, {m.x + 3.5f, m.y - 3.5f}, on ? kText : kDim, 1.3f);
            }
        }
        // While it finds targets, the match Up and Down picked is lit as the mouse lights one.
        const int picked = searching && !cascadeSearch_.text.empty() ? cascade_->picked : -1;
        for (size_t i = 0; i < level.children.size(); ++i) {
            const MenuNode &item = level.children[i];
            const Rect at = cascadeItemRect(level, r, static_cast<int>(i), k);
            if (item.heading) {
                c.text({at.x + 8.0f, at.y + 2.0f, at.w - 16.0f, at.h}, item.label, 9.5f, kDim, Align::Left, true);
                continue;
            }
            const bool opened = k + 1 < levels.size() && cascade_->open[k] == static_cast<int>(i);
            const bool hovered = hover_ && hover_->kind == Kind::CascadeItem && hover_->a == static_cast<int>(k) &&
                                 hover_->b == static_cast<int>(i);
            if (opened || ((hovered || picked == static_cast<int>(i)) && item.enabled))
                c.fillRect(at, kTrack, 4.0f);
            const Color ink = item.enabled ? kText : kFaint;
            float x = at.x + 8.0f;
            if (item.color) {
                c.fillCircle({x + 4.0f, at.center().y}, 4.0f, item.color->alpha(item.enabled ? 1.0f : 0.4f));
                x += 14.0f;
            }
            if (!item.path.empty()) { // dim, before the label
                const float pathW = std::ceil(c.textWidth(item.path, 11.5f));
                c.text({x, at.y, at.right() - x - 8.0f, at.h}, item.path, 11.5f, kDim, Align::Left);
                x += pathW;
            }
            const float labelW = std::ceil(c.textWidth(item.label, 11.5f));
            c.text({x, at.y, at.right() - x - 8.0f, at.h}, item.label, 11.5f, ink, Align::Left);
            if (item.holds) // what the row has now is in here
                c.fillCircle({x + labelW + 7.0f, at.center().y}, 2.5f, kText);
            float right = at.right() - 8.0f;
            if (!item.children.empty()) { // a chevron for a category
                const Point m{right - 3.0f, at.center().y};
                const Point chevron[] = {{m.x - 2.0f, m.y - 4.0f}, {m.x + 2.0f, m.y}, {m.x - 2.0f, m.y + 4.0f}};
                c.polyline(chevron, 3, opened ? kText : kDim, 1.4f);
                right -= 12.0f;
            }
            if (item.checked) {
                const Point m{right - 5.0f, at.center().y};
                const Point check[] = {{m.x - 5.0f, m.y}, {m.x - 2.0f, m.y + 3.0f}, {m.x + 4.0f, m.y - 3.5f}};
                c.polyline(check, 3, kText, 1.5f);
                right -= 14.0f;
            }
            if (!item.note.empty())
                c.text({x + labelW + 12.0f, at.y, right - x - labelW - 12.0f, at.h}, item.note, 10.5f, kDim,
                       Align::Right);
        }
    }
}

void View::paintMods(Canvas &c) {
    const std::vector<Assignment> rows = modsRows();
    int modulations = 0, bypassed = 0, zero = 0, empty = 0;
    for (const Assignment &m : rows) {
        if (m.slot < 0) {
            ++empty;
            continue;
        }
        ++modulations;
        bypassed += m.bypassed ? 1 : 0;
        zero += m.amount == 0.0 ? 1 : 0;
    }
    const Rect head{kModsList.x, kModsHeaderY, kModsList.w, 20.0f};
    const std::pair<ModsColumn, Str> columns[] = {
        {kModsSource, Str::ModsHeadSource}, {kModsTarget, Str::ModsHeadTarget}, {kModsDepth, Str::ModsHeadDepth},
        {kModsCurve, Str::ModsHeadCurve},   {kModsScale, Str::ModsHeadAmount},
        {kModsAux, Str::ModsHeadAux}};
    for (const auto &[column, name] : columns)
        c.text({modsCell(head, column).x, head.y, column.w + 20.0f, head.h}, tr(name), 9.5f, kDim, Align::Left, true);
    // The count, at the right of the names' row after Aux's: what is bypassed, at 0 % and empty goes with it, as much
    // of it as fits.
    std::vector<std::string> parts{modulations == 1 ? tr(Str::ModsCountOne)
                                                    : trf(Str::ModsCount, {std::to_string(modulations)})};
    if (bypassed > 0)
        parts.push_back(trf(Str::ModsBypassed, {std::to_string(bypassed)}));
    if (zero > 0)
        parts.push_back(trf(Str::ModsAtZero, {std::to_string(zero)}));
    if (empty > 0)
        parts.push_back(trf(Str::ModsEmpty, {std::to_string(empty)}));
    // Left of the filter's magnifier; while the filter is open, it says how many it shows instead.
    const float countX = modsCell(head, kModsAux).x + c.textWidth(tr(Str::ModsHeadAux), 9.5f, true) + 24.0f;
    const float countW = modsFilterRect().x - 10.0f - countX;
    std::string count;
    for (;;) {
        count = parts[0];
        for (size_t i = 1; i < parts.size(); ++i)
            count += " \xC2\xB7 " + parts[i];
        if (parts.size() == 1 || c.textWidth(count, 11.0f) <= countW)
            break;
        parts.pop_back();
    }
    if (!modsFilterOpen())
        c.text({countX, head.y, countW, head.h}, count, 11.0f, kDim, Align::Right);
    if (rows.empty()) { // only the button that adds one
        const std::string hint =
            modsFilter_.text.empty() ? std::string(tr(Str::HintNoMods)) : trf(Str::ModsNoMatch, {modsFilter_.text});
        c.text({kFxPanel.x, kModsList.y + 50.0f, kFxPanel.w, 30.0f}, hint, 11.5f, kDim);
        return;
    }

    // The rows' backgrounds, the one under the mouse (or whose menu is open) lit; their arrows, and an empty
    // modulation's dot (the others' bypass dots are widgets). The cells are widgets.
    const float first = modsFirst();
    const int top = static_cast<int>(std::floor(first));
    c.pushClip(kModsList);
    for (int row = top; row < std::min(top + static_cast<int>(kModsList.h / kModsRowStep) + 2, static_cast<int>(rows.size()));
         ++row) {
        const Assignment &m = rows[row];
        const Rect r = modsRow(static_cast<float>(row) - first);
        // An open menu's items number their level and place in a and b, which aren't a row's.
        const bool lit = (hover_ && hover_->kind != Kind::CascadeMenu && hover_->kind != Kind::CascadeItem &&
                          hover_->a == m.source && hover_->b == m.slot && kModsList.contains(hoverPoint_) &&
                          drag_.kind == DragKind::None) ||
                         (cascade_ && cascade_->source == m.source && cascade_->slot == m.slot);
        c.fillRect(r, lit ? kTrack.alpha(0.7f) : kRaised, 5.0f);
        const Rect arrow = modsCell(r, kModsArrow);
        const Color faint = kFaint;
        c.line({arrow.x + 1.0f, arrow.center().y}, {arrow.right() - 3.0f, arrow.center().y}, faint, 1.3f);
        const Point tip{arrow.right() - 2.0f, arrow.center().y};
        const Point head2[] = {{tip.x - 4.0f, tip.y - 3.5f}, tip, {tip.x - 4.0f, tip.y + 3.5f}};
        c.polyline(head2, 3, faint, 1.3f);
        if (m.slot < 0) // an empty modulation: a hollow dot, until it has a source and a target
            c.strokeCircle(modsCell(r, kModsBypass).center(), 3.2f, kFaint, 1.2f);
    }
    c.popClip();
}

} // namespace fmvibe::gui
