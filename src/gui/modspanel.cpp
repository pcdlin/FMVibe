#include "modspanel.h"
#include "lists.h"

#include "fxgraph.h"
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
// menus its rows' sources, targets and auxes open (see modspanel.h).
namespace fmvibe::gui {

using namespace detail;

namespace {

// `name` without `prefix` and the space after it, where it starts so: a param's name within its group.
std::string afterPrefix(const std::string &name, const std::string &prefix) {
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 && name[prefix.size()] == ' '
               ? name.substr(prefix.size() + 1)
               : name;
}

} // namespace

ModsPanel::ModsPanel(PanelHost &host)
    : host_(host), mods_(host.modulationModel()), routing_(host.routingModel()), fx_(host.fxGraphModel()),
      table_(paramTable()) {}

bool ModsPanel::wide() const {
    return controller().layout().view == RoutingView::Mods && !host_.shapeEditorOpen() && !host_.partialEditorOpen();
}

void ModsPanel::forgetScoped() {
    rows_.forget();
    cascadeTree_.forget();
}

// An empty modulation from a source that has rows goes after them, the others at the end. The filter leaves out the
// modulations whose source and target names don't hold every word of it, but not the empty ones.
std::vector<Assignment> ModsPanel::modsRows() const {
    return rows_.get(host_, [this] { return modsRowsNow(); });
}

std::vector<Assignment> ModsPanel::modsRowsNow() const {
    std::vector<Assignment> rows = mods_.assignments();
    if (const std::vector<std::string> words = modsFilter_.words(); !words.empty())
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const Assignment &m) {
                                      return !TextField::holds(
                                          mods_.sourceName(m.source) + " " + mods_.paramName(m.target), words);
                                  }),
                   rows.end());
    for (int row = 0; row < static_cast<int>(controller().layout().emptyMods.size()); ++row) {
        const auto [source, target] = mods_.emptyMod(row);
        const Assignment empty{
            source, emptyModSlot(row), target, 0.0, false, source >= 0 && mods_.sourceBipolar(source), 1.0, -1};
        const auto last = std::find_if(rows.rbegin(), rows.rend(), [&](const Assignment &m) {
            return source >= 0 && m.source == source;
        });
        rows.insert(last == rows.rend() ? rows.end() : last.base(), empty);
    }
    return rows;
}

// The rows and, after them, the one that adds an empty modulation.
float ModsPanel::modsEnd() const {
    const float rows = static_cast<float>(modsRows().size() + 1);
    return std::max((rows * kModsRowStep - (kModsRowStep - kModsRowH) - kModsList.h) / kModsRowStep, 0.0f);
}

float ModsPanel::modsFirst() const {
    return std::clamp(modsGlide_, 0.0f, modsEnd());
}

void ModsPanel::scrollMods(float first, bool glide) {
    modsScroll_ = std::clamp(first, 0.0f, modsEnd());
    if (!glide)
        modsGlide_ = modsScroll_;
}

Rect ModsPanel::revealModsRow(int source, int slot) {
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
bool ModsPanel::glideMods() {
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

Rect ModsPanel::modsThumb() const {
    const Rect track = modsScrollTrack();
    const float rows = static_cast<float>(modsRows().size() + 1);
    const float shown = kModsList.h / kModsRowStep;
    const float h = std::max(track.h * std::min(shown / std::max(rows, 1.0f), 1.0f), 16.0f);
    return {track.x, track.y + (track.h - h) * modsFirst() / std::max(modsEnd(), 0.001f), track.w, h};
}

void ModsPanel::addEmptyMod(int source) {
    const int row = mods_.addEmptyMod(source);
    if (row < 0)
        return;
    const Rect r = revealModsRow(source, emptyModSlot(row));
    if (source < 0)
        openCascade(CascadeKind::Source, -1, emptyModSlot(row), modsCell(r, kModsSource));
    else
        openCascade(CascadeKind::Target, source, emptyModSlot(row), modsCell(r, kModsTarget));
}

Rect ModsPanel::modsFilterRect() const {
    return modsFilter(modsFilterOpen());
}

std::string ModsPanel::modsFilterCount() const {
    const std::vector<Assignment> rows = modsRows();
    const auto shown = std::count_if(rows.begin(), rows.end(), [](const Assignment &m) { return m.slot >= 0; });
    return trf(Str::ModsMatching, {std::to_string(shown), std::to_string(mods_.assignments().size())});
}

void ModsPanel::modsFilterKey(Key key) {
    if (key == Key::Enter)
        modsFiltering_ = false;
    else if (key == Key::Escape && !modsFilter_.text.empty())
        host_.clearField(Field::ModsFilter);
    else if (key == Key::Escape)
        modsFiltering_ = false;
}

void ModsPanel::showSource(int source) {
    host_.clearField(Field::ModsFilter); // all of its rows
    const std::vector<Assignment> rows = modsRows();
    for (size_t row = 0; row < rows.size(); ++row)
        if (rows[row].source == source) { // its first row at the top, as far as the rows go
            scrollMods(static_cast<float>(row), false);
            return;
        }
    addEmptyMod(source);
}

void ModsPanel::fillEmptyMod(int row, int source, int target) {
    if (const std::optional<std::pair<int, int>> shown = mods_.fillEmptyMod(row, source, target))
        revealModsRow(shown->first, shown->second);
}

// While the target menu's search holds something, its first level lists what it finds in the categories, flat, each
// with its path.
std::shared_ptr<const ModsPanel::MenuNode> ModsPanel::cascadeTree() const {
    return cascadeTree_.get(host_, [this] { return std::make_shared<const MenuNode>(cascadeTreeNow()); });
}

ModsPanel::MenuNode ModsPanel::cascadeTreeNow() const {
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

ModsPanel::MenuNode ModsPanel::cascadeCategories() const {
    MenuNode root;
    if (!cascade_)
        return root;
    const Cascade &menu = *cascade_;
    auto get = [this](int index) { return value(index); };
    const int target = mods_.modsRowTarget(menu.source, menu.slot);
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
    // A list of sources longer than a column holds (every place holding an LFO, say) goes on in columns beside it, about
    // as tall, so it stays inside the editor.
    auto inColumns = [](MenuNode node) {
        const size_t columns = (node.children.size() + kCascadeColumnRows - 1) / kCascadeColumnRows;
        const size_t rows = columns > 1 ? (node.children.size() + columns - 1) / columns : node.children.size();
        for (size_t i = rows; i < node.children.size(); i += rows)
            node.children[i].newColumn = true;
        return node;
    };

    if (menu.kind != CascadeKind::Target) {
        // A source can't take a modulation it has already, nor one past its slots; an aux can't be the row's own source.
        // An empty modulation without a target yet takes any source with a slot free.
        auto sourcePick = [&](int source) {
            MenuNode node = pick(mods_.sourceName(source), source, kSourceColors[source]);
            if (menu.kind == CascadeKind::Aux) {
                if (source == menu.source) {
                    node.enabled = false;
                    node.note = tr(Str::CascadeItself);
                }
                return node;
            }
            if (source == menu.source)
                return node;
            const std::vector<int> used = mods_.usedSlots(source);
            if (target >= 0 && !mods_.canModulate(source, target)) {
                node.enabled = false;
                node.note = mods_.refusedNote(source, target);
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
        for (int source : mods_.sources())
            (runsLfo(mods_.sourceTypeOf(source))             ? lfos // Random among them, a random LFO
             : mods_.sourceTypeOf(source) == SourceType::Env ? envs
                                                             : others)
                .children.push_back(sourcePick(source));
        keep(inColumns(std::move(lfos)));
        keep(inColumns(std::move(envs)));
        keep(inColumns(std::move(others)));
        MenuNode macros = category(tr(Str::CascadeMacros));
        for (int macro = 0; macro < kMacros; ++macro)
            macros.children.push_back(sourcePick(kMaxSources + macro));
        keep(std::move(macros));
        MenuNode keyboard = category(tr(Str::CascadeKeyboard));
        for (int source : {kNoteSource, kVelocitySource, kWheelSource, kBendSource})
            keyboard.children.push_back(sourcePick(source));
        keep(std::move(keyboard));
        MenuNode operators = category(tr(Str::CascadeOperators));
        for (int op = 0; op < kOps; ++op)
            operators.children.push_back(sourcePick(kOpSource + op));
        keep(std::move(operators));
        markHolds(root);
        return root;
    }

    // Targets: what the row's source modulates already, through another slot, can't be picked.
    std::vector<int> taken;
    if (menu.source >= 0)
        for (int s : mods_.usedSlots(menu.source))
            if (s != menu.slot)
                taken.push_back(slotTarget(value(slotTargetIndex(menu.source, s))));
    auto targetPick = [&](std::string label, int index, std::optional<Color> color = std::nullopt) {
        MenuNode node = pick(std::move(label), index, color);
        if (std::find(taken.begin(), taken.end(), index) != taken.end()) {
            node.enabled = false;
            node.note = tr(Str::CascadeAlready);
        } else if (!mods_.canModulate(menu.source, index)) {
            node.enabled = false;
            node.note = mods_.refusedNote(menu.source, index);
        }
        return node;
    };
    auto add = [&](MenuNode &to, int index, const std::string &prefix) {
        if (isModTarget(index))
            to.children.push_back(targetPick(afterPrefix(table_[index].name, prefix), index));
    };

    // The mod wheel and the pitch bend are played, not set: they aren't offered.
    MenuNode global = category(tr(Str::CascadeGlobal));
    for (int g = 0; g < kGlobalCount; ++g)
        if (!isPlayed(g))
            add(global, g, "");
    keep(std::move(global));

    MenuNode operators = category(tr(Str::CascadeOperators));
    for (int op = 0; op < kOps; ++op) {
        MenuNode node = category(kNodeNames[op]);
        node.note = routing_.nodeOn(op)
                        ? waveName(static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave)))))
                        : tr(Str::CascadeOff);
        const std::string prefix = trf(Str::ParamOp, {kNodeNames[op]});
        // In two columns, about as tall: the pitch, level, phase (and the warp's amount) and unison, then the envelope
        // and the noise.
        const std::pair<Str, std::vector<OpParam>> sections[] = {
            {Str::CascadePitch, {OpParam::Ratio, OpParam::Transpose, OpParam::Detune, OpParam::FixedFreq}},
            {Str::CascadeLevel, {OpParam::Out, OpParam::Pan}},
            {Str::CascadePhase, {OpParam::Phase, OpParam::WarpAmount}},
            {Str::CascadeUnison,
             {OpParam::UnisonDetune, OpParam::UnisonSpread, OpParam::UnisonBlend, OpParam::PhaseRandom}},
            {Str::CascadeEnvelope,
             {OpParam::Attack, OpParam::Hold, OpParam::Decay, OpParam::Sustain, OpParam::Release, OpParam::AttackCurve,
              OpParam::DecayCurve, OpParam::ReleaseCurve}},
            {Str::CascadeNoise, {OpParam::NoiseSlope}},
            {Str::CascadeKeys, {OpParam::KeyBreak, OpParam::KeyLeftDepth, OpParam::KeyRightDepth}}};
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
        for (FilterParam field :
             {FilterParam::Cutoff, FilterParam::Resonance, FilterParam::KeyTrack, FilterParam::Drive, FilterParam::Mix})
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
    const FxLine line = fx_.line();
    const std::vector<int> chain = FxGraphModel::chain(line);
    for (size_t pos = 0; pos < chain.size(); ++pos) {
        const int slot = chain[pos];
        MenuNode node = category(std::to_string(pos + 1) + " \xC2\xB7 " + fxName(fx_.slotType(slot)));
        node.color = fxTypeColor(fx_.slotType(slot));
        const std::string prefix = trf(Str::ParamFx, {std::to_string(slot + 1)});
        for (int f = static_cast<int>(FxParam::Enabled) + 1; f < kFxParamCount; ++f) {
            const int index = paramIndex(slot, static_cast<FxParam>(f));
            if (fieldInUse(get, index))
                add(node, index, prefix);
        }
        if (!node.children.empty())
            effects.children.push_back(std::move(node));
    }
    // Then the splits' crossovers and their joins' band levels, in the line's order.
    for (const FxLineItem &item : line) {
        if (!fxNodeSplit(item.node))
            continue;
        const int split = item.node - kFxSlots;
        MenuNode node = category(trf(Str::NodeSplit, {std::to_string(split + 1)}));
        node.color = splitColor();
        add(node, paramIndex(split, SplitParam::Low), trf(Str::ParamSplit, {std::to_string(split + 1)}));
        if (item.three) // and its high crossover, split in three
            add(node, paramIndex(split, SplitParam::High), trf(Str::ParamSplit, {std::to_string(split + 1)}));
        effects.children.push_back(std::move(node));
        if (!item.joined)
            continue;
        MenuNode join = category(trf(Str::NodeJoin, {std::to_string(split + 1)}));
        join.color = sumColor();
        for (int input = 0; input < (item.three ? 3 : 2); ++input) // the high band's, the low one's, a middle one's
            add(join, fxInputIndex(fxJoinNode(split), input, FxInputField::Level),
                trf(Str::ParamSum, {std::to_string(split + 1)}));
        effects.children.push_back(std::move(join));
    }
    keep(std::move(effects));

    // The sources' own knobs, then the depths of their modulations (another source's modulation scaled by this one).
    MenuNode modulation = category(tr(Str::CascadeModulation));
    for (int source : mods_.sources()) {
        MenuNode node = category(mods_.sourceName(source));
        node.color = kSourceColors[source];
        node.children.push_back(heading(Str::CascadeFields));
        for (int f = static_cast<int>(SourceParam::Enabled) + 1; f < kSourceParamCount; ++f) {
            const int index = paramIndex(source, static_cast<SourceParam>(f));
            if (index >= slotTargetIndex(source, 0) && index <= slotAmountIndex(source, kSlots - 1))
                continue; // the depths come below, by what they modulate
            if (fieldInUse(get, index) && isModTarget(index))
                node.children.push_back(
                    targetPick(afterPrefix(mods_.paramName(index), mods_.sourceName(source)), index));
        }
        const std::vector<int> used = mods_.usedSlots(source);
        bool depths = false;
        for (int s : used) {
            if (source == menu.source && s == menu.slot)
                continue; // a modulation can't scale its own depth
            if (!depths)
                node.children.push_back(heading(Str::CascadeDepths));
            depths = true;
            node.children.push_back(
                targetPick("\xE2\x86\x92 " + mods_.paramName(slotTarget(value(slotTargetIndex(source, s)))),
                           slotAmountIndex(source, s)));
        }
        modulation.children.push_back(std::move(node));
    }
    keep(inColumns(std::move(modulation)));

    // Then the macros' knobs, by their names and in their colors.
    MenuNode macroKnobs = category(tr(Str::CascadeMacros));
    for (int macro = 0; macro < kMacros; ++macro)
        macroKnobs.children.push_back(targetPick(mods_.sourceName(kMaxSources + macro),
                                                 paramIndex(macro, MacroParam::Value),
                                                 kSourceColors[kMaxSources + macro]));
    keep(std::move(macroKnobs));

    // And the sequencer's Swing, Gate, Velocity and Humanize (see isSeqFeel()).
    MenuNode sequencer = category(tr(Str::CascadeSequencer));
    for (int k = 0; k < kSeqFeelCount; ++k)
        add(sequencer, seqFeelIndex(k), std::string(tr(Str::CascadeSequencer)));
    keep(std::move(sequencer));
    markHolds(root);
    return root;
}

std::vector<const ModsPanel::MenuNode *> ModsPanel::cascadeLevels(const MenuNode &root) const {
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

Rect ModsPanel::cascadeSearchRect(Rect level) const {
    return {level.x + kCascadePad + 2.0f, level.y + kCascadePad + 2.0f, level.w - 2.0f * kCascadePad - 4.0f, 24.0f};
}

int ModsPanel::cascadeColumns(const MenuNode &level) {
    return 1 + static_cast<int>(std::count_if(level.children.begin(), level.children.end(),
                                              [](const MenuNode &item) { return item.newColumn; }));
}

Rect ModsPanel::cascadeItemRect(const MenuNode &level, Rect at, int index, size_t depth) const {
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
std::vector<Rect> ModsPanel::cascadeRects(const MenuNode &root) const {
    std::vector<Rect> rects;
    if (!cascade_)
        return rects;
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const float bottom = host_.height() - 8.0f;
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
            if (r.right() > View::kWidth - 8.0f) // no room on the right: on the left of the level it opens from
                r.x = before.x - 2.0f - w;
            r.y = from.y - kCascadePad;
        }
        r.y = std::max(std::min(r.y, bottom - h), 8.0f);
        r.x = std::clamp(r.x, 8.0f, View::kWidth - 8.0f - w);
        rects.push_back(r);
    }
    return rects;
}

void ModsPanel::openCascade(CascadeKind kind, int source, int slot, Rect from) {
    if (cascade_ && cascade_->kind == kind && cascade_->source == source && cascade_->slot == slot) {
        cascade_.reset(); // a second click closes it
        return;
    }
    cascade_ = Cascade{kind, source, slot, {from.x, from.bottom() + 2.0f}, {}};
    cascadeSearch_.clear();
}

void ModsPanel::cascadeKey(Key key) {
    if (!cascade_)
        return;
    if (key == Key::Escape) {
        if (cascadeSearch_.text.empty())
            cascade_.reset();
        else
            host_.clearField(Field::CascadeSearch);
        return;
    }
    if (cascadeSearch_.text.empty() || (key != Key::Up && key != Key::Down && key != Key::Enter))
        return;
    const std::shared_ptr<const MenuNode> tree = cascadeTree();
    const MenuNode &root = *tree;
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

void ModsPanel::pickCascade(int level, int index) {
    if (!cascade_)
        return;
    const std::shared_ptr<const MenuNode> tree = cascadeTree();
    const MenuNode &root = *tree;
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
            mods_.moveModulation(menu.source, menu.slot, item.pick);
        break;
    case CascadeKind::Target:
        if (empty >= 0)
            fillEmptyMod(empty, -1, item.pick);
        else
            mods_.retarget(menu.source, menu.slot, item.pick);
        break;
    case CascadeKind::Aux:
        mods_.setAux(menu.source, menu.slot, item.pick);
        break;
    }
}

void ModsPanel::paintCascade(Canvas &c) {
    if (!cascade_)
        return;
    const std::shared_ptr<const MenuNode> tree = cascadeTree();
    const MenuNode &root = *tree;
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const std::vector<Rect> rects = cascadeRects(root);
    for (size_t k = 0; k < levels.size(); ++k) {
        const MenuNode &level = *levels[k];
        const Rect r = rects[k];
        if (!look.flat) // a flat menu's panel brings its own, softer one
            c.fillRect({r.x + 2.0f, r.y + 4.0f, r.w, r.h}, kBackground.alpha(0.5f), kRadiusCard); // a shadow
        paintMenuPanel(c, r);
        const bool searching = k == 0 && cascadeSearches();
        if (searching) { // its search, and the x that empties it
            const Rect search = cascadeSearchRect(r);
            host_.paintField(c, Field::CascadeSearch, search, fieldTyped(search), tr(Str::CascadeSearchHint));
            if (!cascadeSearch_.text.empty()) {
                const Rect x = fieldClear(search);
                const std::optional<Widget> &hover = host_.hovered();
                const bool on = hover && hover->kind == Kind::SearchClear &&
                                hover->a == static_cast<int>(Field::CascadeSearch);
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
                c.text({at.x + 8.0f, at.y + 2.0f, at.w - 16.0f, at.h}, item.label, kTextSmall, kDim, Align::Left, true);
                continue;
            }
            const bool opened = k + 1 < levels.size() && cascade_->open[k] == static_cast<int>(i);
            const std::optional<Widget> &hover = host_.hovered();
            const bool hovered = hover && hover->kind == Kind::CascadeItem && hover->a == static_cast<int>(k) &&
                                 hover->b == static_cast<int>(i);
            paintMenuRow(c, at, opened || ((hovered || picked == static_cast<int>(i)) && item.enabled), false);
            const Color ink = item.enabled ? kText : kFaint;
            float x = at.x + 8.0f;
            if (item.color) {
                c.fillCircle({x + 4.0f, at.center().y}, 4.0f, item.color->alpha(item.enabled ? 1.0f : 0.4f));
                x += 14.0f;
            }
            if (!item.path.empty()) { // dim, before the label
                const float pathW = std::ceil(c.textWidth(item.path, kTextBody));
                c.text({x, at.y, at.right() - x - 8.0f, at.h}, item.path, kTextBody, kDim, Align::Left);
                x += pathW;
            }
            const float labelW = std::ceil(c.textWidth(item.label, kTextBody));
            c.text({x, at.y, at.right() - x - 8.0f, at.h}, item.label, kTextBody, ink, Align::Left);
            if (item.holds) // what the row has now is in here
                c.fillCircle({x + labelW + 7.0f, at.center().y}, 2.5f, kText);
            float right = at.right() - 8.0f;
            if (!item.children.empty()) { // a chevron for a category
                paintChevron(c, {right - 3.0f, at.center().y}, opened ? kText : kDim, ChevronWay::Right, true);
                right -= 12.0f;
            }
            if (item.checked) {
                const Point m{right - 5.0f, at.center().y};
                const Point check[] = {{m.x - 5.0f, m.y}, {m.x - 2.0f, m.y + 3.0f}, {m.x + 4.0f, m.y - 3.5f}};
                c.polyline(check, 3, kText, 1.5f);
                right -= 14.0f;
            }
            if (!item.note.empty())
                c.text({x + labelW + 12.0f, at.y, right - x - labelW - 12.0f, at.h}, item.note, kTextLabel, kDim,
                       Align::Right);
        }
    }
}

void ModsPanel::paintBack(Canvas &c) {
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
        c.text({modsCell(head, column).x, head.y, column.w + 20.0f, head.h}, tr(name), kTextSmall, kDim, Align::Left, true);
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
    const float countX = modsCell(head, kModsAux).x + c.textWidth(tr(Str::ModsHeadAux), kTextSmall, true) + 24.0f;
    const float countW = modsFilterRect().x - 10.0f - countX;
    std::string count;
    for (;;) {
        count = parts[0];
        for (size_t i = 1; i < parts.size(); ++i)
            count += " \xC2\xB7 " + parts[i];
        if (parts.size() == 1 || c.textWidth(count, kTextBody) <= countW)
            break;
        parts.pop_back();
    }
    if (!modsFilterOpen())
        c.text({countX, head.y, countW, head.h}, count, kTextBody, kDim, Align::Right);
    if (rows.empty()) { // only the button that adds one
        const std::string hint =
            modsFilter_.text.empty() ? std::string(tr(Str::HintNoMods)) : trf(Str::ModsNoMatch, {modsFilter_.text});
        c.text({kFxPanel.x, kModsList.y + 50.0f, kFxPanel.w, 30.0f}, hint, kTextBody, kDim);
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
        const std::optional<Widget> &hover = host_.hovered();
        const bool lit = (hover && hover->kind != Kind::CascadeMenu && hover->kind != Kind::CascadeItem &&
                          hover->a == m.source && hover->b == m.slot && kModsList.contains(host_.mouse()) &&
                          host_.isHovered(*hover)) ||
                         (cascade_ && cascade_->source == m.source && cascade_->slot == m.slot);
        c.fillRect(r, lit ? kTrack.alpha(0.7f) : kRaised, kRadiusControl);
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

// --- What the View hands the panel.

void ModsPanel::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    // A row per modulation, cut off at the list's ends while it scrolls, then one that adds an empty modulation.
    // The first of a source's rows names it, with its polarity switch after the name (an empty modulation among
    // them too); an empty modulation's row has only its source, its target and its remove.
    const std::vector<Assignment> rows = modsRows();
    const float first = modsFirst();
    const int top = static_cast<int>(std::floor(first));
    const size_t rowsStart = list.size();
    const int end = std::min(top + static_cast<int>(kModsList.h / kModsRowStep) + 2, static_cast<int>(rows.size()));
    for (int row = top; row < end; ++row) {
        const Assignment &m = rows[row];
        const Rect r = modsRow(static_cast<float>(row) - first);
        const bool empty = m.slot < 0;
        if (!empty)
            add(Kind::SlotBypass, modsCell(r, kModsBypass), slotBypassIndex(m.source, m.slot), m.source, m.slot);
        add(Kind::ModsSource, modsCell(r, kModsSource), -1, m.source, m.slot);
        const bool named = m.source < 0 || row == 0 || rows[row - 1].source != m.source;
        list.back().compact = named;
        if (named && m.source >= 0)
            add(Kind::SourcePolarity, modsCell(r, kModsPolarity), -1, m.source, m.slot);
        add(Kind::ModsTarget, modsCell(r, kModsTarget), -1, m.source, m.slot);
        if (empty) {
            add(Kind::SlotRemove, modsCell(r, kModsRemove), -1, m.source, m.slot);
            continue;
        }
        const Rect depth = modsCell(r, kModsDepth);
        add(Kind::SlotBar, {depth.x, depth.y, depth.w - 46.0f, depth.h}, slotAmountIndex(m.source, m.slot),
            m.source, m.slot);
        add(Kind::ValueText, {depth.right() - 44.0f, depth.y, 42.0f, depth.h}, slotAmountIndex(m.source, m.slot),
            m.source, m.slot);
        const Rect curve = modsCell(r, kModsCurve);
        add(Kind::ModsCurve, {curve.x, curve.y + 3.0f, curve.w, curve.h - 6.0f}, slotCurveIndex(m.source, m.slot),
            m.source, m.slot);
        const Rect scale = modsCell(r, kModsScale);
        add(Kind::ModsScale, {scale.x, scale.y, scale.w - 44.0f, scale.h}, slotScaleIndex(m.source, m.slot),
            m.source, m.slot);
        const Rect aux = modsCell(r, kModsAux);
        if (m.aux >= 0) {
            // The chip: its name opens the menu of auxes, its aux depth drags, its x takes the aux away.
            add(Kind::ModsAux, {aux.x, aux.y + 3.0f, aux.w - 24.0f - kAuxDepthW, aux.h - 6.0f}, -1, m.source,
                m.slot);
            add(Kind::ModsAuxDepth, {aux.right() - 22.0f - kAuxDepthW, aux.y + 3.0f, kAuxDepthW, aux.h - 6.0f},
                slotAuxDepthIndex(m.source, m.slot), m.source, m.slot);
            add(Kind::ModsAuxClear, {aux.right() - 22.0f, aux.y + 3.0f, 20.0f, aux.h - 6.0f}, -1, m.source, m.slot);
        } else {
            add(Kind::ModsAux, {aux.x, aux.y + 3.0f, 64.0f, aux.h - 6.0f}, -1, m.source, m.slot);
        }
        add(Kind::SlotRemove, modsCell(r, kModsRemove), -1, m.source, m.slot);
    }
    if (const Rect r = modsRow(static_cast<float>(rows.size()) - first); r.y < kModsList.bottom()) {
        const Rect cell = modsCell(r, kModsAdd);
        add(Kind::ModsAdd, {cell.x, cell.y + 3.0f, cell.w, cell.h - 6.0f});
    }
    for (size_t i = rowsStart; i < list.size(); ++i)
        list[i].clip = kModsList;
    if (modsEnd() > 0.0f)
        add(Kind::ModsScroll, modsScrollTrack().inset(-3.0f));
    const Rect filter = modsFilterRect();
    add(Kind::SearchField, filter, -1, static_cast<int>(Field::ModsFilter));
    if (!modsFilter_.text.empty())
        add(Kind::SearchClear, fieldClear(filter), -1, static_cast<int>(Field::ModsFilter));
}

// A row's cascading menu, over everything: around it, a click closes it.
void ModsPanel::popups(std::vector<Widget> &list) const {
    if (!cascade_ || !wide())
        return;
    const WidgetAdder add{list};
    const std::shared_ptr<const MenuNode> tree = cascadeTree();
    const MenuNode &root = *tree;
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const std::vector<Rect> rects = cascadeRects(root);
    add(Kind::CascadeMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
    for (size_t k = 0; k < levels.size(); ++k) {
        add(Kind::CascadeMenu, rects[k], -1, 1, static_cast<int>(k));
        for (size_t i = 0; i < levels[k]->children.size(); ++i)
            if (!levels[k]->children[i].heading)
                add(Kind::CascadeItem, cascadeItemRect(*levels[k], rects[k], static_cast<int>(i), k), -1,
                    static_cast<int>(k), static_cast<int>(i));
        if (k == 0 && cascadeSearches()) { // the search atop the first level
            const Rect search = cascadeSearchRect(rects[k]);
            add(Kind::SearchField, search, -1, static_cast<int>(Field::CascadeSearch));
            if (!cascadeSearch_.text.empty())
                add(Kind::SearchClear, fieldClear(search), -1, static_cast<int>(Field::CascadeSearch));
        }
    }
}

void ModsPanel::paint(Canvas &c) {
    if (cascade_ && wide())
        paintCascade(c);
}

bool ModsPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::SearchField: // its filter; the target menu's search is drawn with its menu
        if (w.a == static_cast<int>(Field::CascadeSearch))
            return true;
        if (w.a != static_cast<int>(Field::ModsFilter))
            return false;
        if (modsFilterOpen()) {
            host_.paintField(c, Field::ModsFilter, w.rect, fieldTyped(w.rect), tr(Str::ModsFilterHint),
                             modsFilter_.text.empty() ? std::string() : modsFilterCount());
        } else { // a magnifier, to open it
            if (hovered)
                c.fillRect(w.rect, kTrack, kRadiusControl);
            const Point lens{w.rect.center().x - 1.5f, w.rect.center().y - 1.5f};
            c.strokeCircle(lens, 4.5f, hovered ? kText : kDim, 1.3f);
            c.line({lens.x + 3.3f, lens.y + 3.3f}, {lens.x + 7.0f, lens.y + 7.0f}, hovered ? kText : kDim, 1.5f);
        }
        return true;
    case Kind::SearchClear: // the target menu's search's, drawn with its menu
        return w.a == static_cast<int>(Field::CascadeSearch);
    case Kind::ModsSource: {
        // The first of a source's rows has its handle's look and its name, the others a line in its color; under the
        // mouse, or while its menu is open, a chevron says it opens one. An empty modulation without a source has a
        // dashed handle and asks for one.
        const bool open = cascade_ && cascade_->kind == CascadeKind::Source && cascade_->source == w.a &&
                          cascade_->slot == w.b;
        if (w.a < 0) {
            const Point handle{w.rect.x + 7.0f, w.rect.center().y};
            c.strokeCircle(handle, 6.5f, hovered || open ? kDim : kFaint, 1.2f);
            const std::string ask = tr(Str::ModsPickSource);
            c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 34.0f, w.rect.h}, ask, kTextBody, hovered || open ? kText : kDim,
                   Align::Left);
            const float x = w.rect.x + 24.0f + std::min(c.textWidth(ask, kTextBody), w.rect.w - 40.0f);
            paintChevron(c, {x + 4.0f, w.rect.center().y}, kDim, ChevronWay::Down, true);
            break;
        }
        const Color color = kSourceColors[w.a];
        const bool bypassed = w.b >= 0 && mods_.slotBypassed(w.a, w.b);
        if (w.compact) {
            const Point handle{w.rect.x + 7.0f, w.rect.center().y};
            c.fillCircle(handle, 7.0f, color);
            c.fillCircle(handle, 2.5f, kPanel);
            const std::string name = mods_.sourceName(w.a);
            c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 34.0f, w.rect.h}, name, kTextBody, bypassed ? kDim : kText,
                   Align::Left, true);
            if (hovered || open) {
                const float x = w.rect.x + 24.0f + std::min(c.textWidth(name, kTextBody, true), w.rect.w - 40.0f);
                paintChevron(c, {x + 4.0f, w.rect.center().y}, kDim, ChevronWay::Down, true);
            }
        } else {
            c.line({w.rect.x + 7.0f, w.rect.y - 2.0f}, {w.rect.x + 7.0f, w.rect.bottom() + 2.0f}, color.alpha(0.35f),
                   2.0f);
            if (hovered || open)
                c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, mods_.sourceName(w.a), kTextBody, kDim,
                       Align::Left);
        }
        break;
    }
    case Kind::ModsTarget: {
        // An empty modulation without a target asks for one.
        const int target = mods_.modsRowTarget(w.a, w.b);
        const bool open = cascade_ && cascade_->kind == CascadeKind::Target && cascade_->source == w.a &&
                          cascade_->slot == w.b;
        if (hovered || open)
            c.fillRect(w.rect.inset(1.0f), kTrack, kRadiusMark);
        const std::string name = target >= 0 ? mods_.paramName(target) : std::string(tr(Str::ModsPickTarget));
        c.text({w.rect.x + 6.0f, w.rect.y, w.rect.w - 24.0f, w.rect.h}, name, kTextBody,
               target < 0 || (w.b >= 0 && mods_.slotBypassed(w.a, w.b)) ? kDim : kText, Align::Left);
        if (hovered || open || target < 0) {
            paintChevron(c, {w.rect.right() - 10.0f, w.rect.center().y}, kDim, ChevronWay::Down, true);
        }
        break;
    }
    case Kind::ModsScale: {
        // All of the depth at the right end, none at the left; its share in numbers after it.
        const double amount = value(w.param);
        const float cy = w.rect.center().y;
        const Color color = mods_.slotBypassed(w.a, w.b) ? kFaint : kDim;
        c.fillRect({w.rect.x, cy - 1.5f, w.rect.w, 3.0f}, kTrack, kRadiusMark);
        c.fillRect({w.rect.x, cy - 1.5f, static_cast<float>(amount) * w.rect.w, 3.0f}, hovered || active ? kText : color,
                   kRadiusMark);
        c.fillCircle({w.rect.x + static_cast<float>(amount) * w.rect.w, cy}, hovered || active ? 4.5f : 3.5f,
                     hovered || active ? kText : color);
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", amount * 100.0);
        c.text({w.rect.right() + 4.0f, w.rect.y, 38.0f, w.rect.h}, text, kTextLabel, kDim, Align::Right);
        break;
    }
    case Kind::ModsCurve: {
        // What the modulation takes from its source, along the source's swing (from the middle out for a bipolar
        // one): a straight line, faint, until the curve bends it.
        const double curve = value(w.param);
        const bool bipolar = mods_.sourceBipolar(w.a);
        const bool lit = hovered || active;
        c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusMark);
        const Rect box = w.rect.inset(5.0f);
        constexpr int kPoints = 24;
        Point points[kPoints];
        for (int k = 0; k < kPoints; ++k) {
            const double t = static_cast<double>(k) / (kPoints - 1);
            const double y = bipolar ? slotCurveValue(t - 0.5, curve, true) + 0.5 : slotCurveValue(t, curve, false);
            points[k] = {box.x + static_cast<float>(t) * box.w, box.bottom() - static_cast<float>(y) * box.h};
        }
        const Color color = mods_.slotBypassed(w.a, w.b) ? kFaint
                            : curve == 0.0       ? (lit ? kDim : kFaint)
                                                 : kSourceColors[w.a];
        c.polyline(points, kPoints, color, 1.4f);
        break;
    }
    case Kind::ModsAux: {
        // A chip in the aux's color, or a dashed one to add an aux.
        const int aux = slotAux([this](int index) { return value(index); }, w.a, w.b);
        const bool open = cascade_ && cascade_->kind == CascadeKind::Aux && cascade_->source == w.a &&
                          cascade_->slot == w.b;
        const float radius = w.rect.h * 0.5f;
        if (aux < 0) {
            if (look.flat) { // a filled pill, as + SOURCE
                paintPill(c, w.rect, hovered || open);
                c.text(w.rect, tr(Str::ModsAddAux), kTextLabel, kText, Align::Center, true);
                break;
            }
            c.strokeRect(w.rect, hovered || open ? kDim : kTrack, 1.0f, radius, true);
            c.text(w.rect, tr(Str::ModsAddAux), kTextLabel, hovered || open ? kText : kDim);
            break;
        }
        const Rect chip{w.rect.x, w.rect.y, w.rect.w + kAuxDepthW + 22.0f, w.rect.h};
        if (look.flat) { // tinted with its source's color, without an outline, more strongly than + Aux is filled
            c.fillRect(chip, kSourceColors[aux].alpha(hovered || open ? 0.32f : 0.22f), radius);
        } else {
            c.fillRect(chip, hovered || open ? kTrack : kRaised, radius);
            c.strokeRect(chip, kSourceColors[aux].alpha(0.5f), 1.0f, radius);
        }
        paintAuxDepthBar(c, chip, aux, value(slotAuxDepthIndex(w.a, w.b)));
        c.fillCircle({w.rect.x + 11.0f, w.rect.center().y}, 4.0f, kSourceColors[aux]);
        c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 22.0f, w.rect.h}, mods_.sourceName(aux), kTextLabel, kText,
               Align::Left, true);
        break;
    }
    case Kind::ModsAdd: {
        // A dashed button, as + Aux: lit under the mouse.
        const float radius = w.rect.h * 0.5f;
        if (look.flat) { // a filled pill, as + SOURCE
            paintPill(c, w.rect, hovered);
            c.text(w.rect, tr(Str::ModsAddModulation), kTextLabel, kText, Align::Center, true);
            break;
        }
        c.strokeRect(w.rect, hovered ? kDim : kTrack, 1.0f, radius, true);
        c.text(w.rect, tr(Str::ModsAddModulation), kTextLabel, hovered ? kText : kDim);
        break;
    }
    case Kind::ModsScroll: {
        const Rect track = modsScrollTrack();
        const bool lit = hovered || active;
        c.fillRect(track, kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(modsThumb(), lit ? kDim : kFaint, kRadiusMark);
        break;
    }
    case Kind::CascadeMenu:
    case Kind::CascadeItem:
        break; // drawn with their menu, over everything (see paintCascade())
    default:
        return false;
    }
    return true;
}

std::string ModsPanel::tooltip(const Widget &w) const {
    switch (w.kind) {
    case Kind::SearchField: // a field says what it is for in its hint; the filter, a magnifier until it opens
        return w.a == static_cast<int>(Field::ModsFilter) && !modsFilterOpen() ? std::string(tr(Str::TipModsFilter))
                                                                              : std::string();
    case Kind::ModsSource: // an empty modulation's picks one
        return tr(w.b < 0 ? Str::TipModsPickSource : Str::TipModsSource);
    case Kind::ModsTarget:
        return tr(w.b < 0 ? Str::TipModsPickTarget : Str::TipModsTarget);
    case Kind::ModsAux:
        return tr(Str::TipModsAux);
    case Kind::ModsScale:
        return tr(Str::TipModsScale);
    case Kind::ModsCurve: {
        char amount[32];
        formatParam(w.param, value(w.param), amount, sizeof(amount));
        return trf(Str::TipModsCurve, {amount});
    }
    default:
        return {};
    }
}

std::optional<CursorKind> ModsPanel::cursor(const Widget &w) const {
    switch (w.kind) {
    case Kind::SearchField: // the filter a button until it opens
        if (w.a != static_cast<int>(Field::ModsFilter))
            return std::nullopt;
        return modsFilterOpen() ? CursorKind::Text : CursorKind::Hand;
    case Kind::CascadeMenu: // its own panel and what is around it, where a click picks nothing
        return CursorKind::Arrow;
    case Kind::CascadeItem: { // a category opens; an entry that can't be picked does nothing
        const std::shared_ptr<const MenuNode> tree = cascade_ ? cascadeTree() : nullptr;
        if (!tree)
            return CursorKind::Arrow;
        const std::vector<const MenuNode *> levels = cascadeLevels(*tree);
        if (w.a < 0 || w.a >= static_cast<int>(levels.size()) || w.b < 0 ||
            w.b >= static_cast<int>(levels[w.a]->children.size()))
            return CursorKind::Arrow;
        const MenuNode &item = levels[w.a]->children[w.b];
        return !item.children.empty() || (!item.heading && item.enabled) ? CursorKind::Hand : CursorKind::Arrow;
    }
    default:
        return std::nullopt;
    }
}

bool ModsPanel::step() {
    return glideMods();
}

// In a cascading menu, the mouse over a category opens it beside it, and over an entry closes what is open past its
// level.
void ModsPanel::hoverChanged(const std::optional<Widget> &hit) {
    if (!cascade_ || !hit || hit->kind != Kind::CascadeItem)
        return;
    const std::shared_ptr<const MenuNode> tree = cascadeTree();
    const MenuNode &root = *tree;
    const std::vector<const MenuNode *> levels = cascadeLevels(root);
    const auto level = static_cast<size_t>(hit->a);
    if (level < levels.size() && hit->b < static_cast<int>(levels[level]->children.size())) {
        std::vector<int> &open = cascade_->open;
        if (!levels[level]->children[hit->b].children.empty()) {
            open.resize(level);
            open.push_back(hit->b);
        } else if (open.size() > level) {
            open.resize(level);
        }
    }
}

void ModsPanel::closePopups() {
    cascade_.reset();
}

// The filter lets the keyboard go at a click anywhere else.
void ModsPanel::pressing(const Widget *hit) {
    if (!(hit && (hit->kind == Kind::SearchField || hit->kind == Kind::SearchClear) &&
          hit->a == static_cast<int>(Field::ModsFilter)))
        modsFiltering_ = false;
}

bool ModsPanel::press(const Widget &w, Point p, Modifiers) {
    switch (w.kind) {
    case Kind::ModsSource:
        openCascade(CascadeKind::Source, w.a, w.b, w.rect);
        return true;
    case Kind::ModsTarget:
        openCascade(CascadeKind::Target, w.a, w.b, w.rect);
        return true;
    case Kind::ModsAux:
        openCascade(CascadeKind::Aux, w.a, w.b, w.rect);
        return true;
    case Kind::ModsAdd:
        addEmptyMod();
        return true;
    case Kind::CascadeMenu:
        if (w.a == 0) // a click anywhere else closes the menu
            cascade_.reset();
        return true;
    case Kind::CascadeItem:
        pickCascade(w.a, w.b);
        return true;
    case Kind::ModsScroll: {
        // On the thumb, drag it; elsewhere on the track, the thumb jumps to center on the click and can be dragged on.
        const Rect track = modsScrollTrack();
        const Rect thumb = modsThumb();
        if (p.y < thumb.y || p.y >= thumb.bottom())
            scrollMods((p.y - thumb.h * 0.5f - track.y) / std::max(track.h - thumb.h, 1.0f) * modsEnd(), false);
        host_.startDrag(*this, w, p, false);
        modsThumbFrom_ = modsFirst();
        return true;
    }
    case Kind::SearchClear: // the filter keeps the keyboard, for what is typed next; the View empties it
        if (w.a == static_cast<int>(Field::ModsFilter))
            modsFiltering_ = true;
        return false;
    case Kind::SearchField: { // the filter takes the keyboard
        if (w.a != static_cast<int>(Field::ModsFilter))
            return false;
        const bool opens = !modsFilterOpen();
        modsFiltering_ = true;
        if (!opens)
            return false; // the caret goes where it is clicked, as in any field
        modsFilter_.caret = modsFilter_.anchor = modsFilter_.text.size(); // the magnifier opens it
        return true;
    }
    default:
        return false;
    }
}

void ModsPanel::drag(const DragState &d) { // the rows follow the thumb
    const float room = modsScrollTrack().h - modsThumb().h;
    if (room > 0.0f)
        scrollMods(modsThumbFrom_ + (d.now.y - d.start.y) / room * modsEnd(), false);
}

bool ModsPanel::dragsOnTouch(const Widget &w) const {
    return w.kind == Kind::ModsScroll;
}

// A row's aux cell takes a source's handle as its modulation's aux, as a depth handle does: not its own source's, nor
// while it is scrolled partly out of the list.
bool ModsPanel::takesSource(const Widget &w, int source) const {
    return w.kind == Kind::ModsAux && w.a != source &&
           !(w.clip.w > 0.0f && (w.rect.y < w.clip.y || w.rect.bottom() > w.clip.bottom()));
}

void ModsPanel::paintSourceRing(Canvas &c, const Widget &w, bool over, Color color) {
    c.strokeRect(w.rect.inset(-2.0f), color.alpha(over ? 1.0f : 0.35f), over ? 2.0f : 1.0f, kRadiusCard);
}

bool ModsPanel::dropSource(const Widget &w, int source) {
    if (w.kind != Kind::ModsAux || w.a == source)
        return false;
    mods_.setAux(w.a, w.b, source);
    return true;
}

// Over the rows (but not on a bar), a notch scrolls them a row, gliding there.
bool ModsPanel::wheelBehind(Point p, float steps) {
    if (!wide() || !kModsList.contains(p) || steps == 0.0f)
        return false;
    scrollMods(std::clamp(modsScroll_, 0.0f, modsEnd()) - steps, true);
    return true;
}

// The middle button or two fingers move the rows up and down, anywhere over the view.
std::optional<std::pair<int, float>> ModsPanel::panAt(Point p) const {
    if (wide() && kFxPanel.contains(p))
        return std::pair{0, modsFirst()};
    return std::nullopt;
}

// The open menu's levels and entries take no pan; around them, the pan closes it and goes on.
bool ModsPanel::blocksPan(const Widget &w) const {
    return w.kind == Kind::CascadeItem || (w.kind == Kind::CascadeMenu && w.a != 0);
}

void ModsPanel::panBegin(int) {
    scrollMods(modsFirst(), false); // a glide the wheel started stops where it shows
}

std::optional<float> ModsPanel::pan(int, float first, float, float down) {
    const float wanted = first - down / kModsRowStep;
    const float to = std::clamp(wanted, 0.0f, std::max(modsEnd(), 0.0f));
    scrollMods(to, false);
    return to != wanted ? std::optional(to) : std::nullopt;
}

TextField *ModsPanel::field(Field f) {
    switch (f) {
    case Field::ModsFilter:
        return &modsFilter_;
    case Field::CascadeSearch:
        return &cascadeSearch_;
    default:
        return nullptr;
    }
}

// The target menu's search while its menu is open, else the filter once clicked, while the view shows.
bool ModsPanel::fieldFocus(std::optional<Field> &focus) const {
    if (cascadeSearches() && wide())
        focus = Field::CascadeSearch;
    else if (modsFiltering_ && wide())
        focus = Field::ModsFilter;
    else
        return false;
    return true;
}

// After what is typed changes: the rows from their top; the target menu's matches from the first that can be picked,
// nothing opened beside them.
void ModsPanel::fieldChanged(Field f) {
    switch (f) {
    case Field::ModsFilter:
        scrollMods(0.0f, false);
        break;
    case Field::CascadeSearch:
        if (cascade_) {
            cascade_->open.clear();
            cascade_->picked = 0;
            // The first match that can be picked: one the row's source modulates already is passed over, as Up and
            // Down pass it (see cascadeKey()). Worked out afresh, for the words typed now.
            const MenuNode root = cascadeTreeNow();
            for (size_t i = 0; i < root.children.size(); ++i)
                if (root.children[i].enabled) {
                    cascade_->picked = static_cast<int>(i);
                    break;
                }
        }
        break;
    default:
        break;
    }
}

void ModsPanel::fieldKey(Field f, Key key) {
    if (f == Field::ModsFilter)
        modsFilterKey(key);
    else if (f == Field::CascadeSearch)
        cascadeKey(key);
}

} // namespace fmvibe::gui
