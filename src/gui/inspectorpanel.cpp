// The inspector (InspectorPanel, see inspectorpanel.h): the lens over the routing panel's left end and the page of the
// node it shows, an operator's, a filter's or a shaper's: their widgets and popups, painting (the lens, the title, the
// unison copies, the scope, the noise's spectrum, the INPUTS rows, the filter's response and the shaper's curve), the
// waves and their list, the tabs, the filter's handle and the INPUTS rows' scrolling. The knobs, switches, lists,
// depths and envelope on its pages are the View's, as wherever they show; a filter's display is filterdisplay.h's,
// shared with the filter effect's card.

#include "inspectorpanel.h"

#include "filterdisplay.h"
#include "icons.h"
#include "layout.h"
#include "lists.h"
#include "routing.h"
#include "shapeeditor.h"
#include "tablebrowser.h"
#include "theme.h"
#include "../uistrings.h"
#include "../unison.h"
#include "../warp.h"
#include "../wavetable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>

namespace fmvibe::gui {

using namespace detail;

namespace {

// `value` as `format` prints it, for the numbers a wording takes.
std::string number(const char *format, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

} // namespace

InspectorPanel::InspectorPanel(PanelHost &host, ShapeEditor &shapes, TableBrowser &tables)
    : host_(host), shapes_(shapes), tables_(tables), mods_(host.modulationModel()), routing_(host.routingModel()),
      table_(paramTable()) {}

bool InspectorPanel::covered() const {
    const RoutingView view = controller().layout().view;
    const int shape = shapes_.shapeEditing();
    const bool editor = shape >= 0 || shapes_.partialEditing() >= 0; // over the panels' row
    return (view == RoutingView::Fx && shape < 0) ||
           ((view == RoutingView::Mods || view == RoutingView::Seq) && !editor) ||
           (shape >= 0 && !isStripCurve(shape) && !isShaperCurve(shape));
}

int InspectorPanel::scopeShown() const {
    const int node = controller().layout().selectedNode;
    return node < kOps && !covered() && opTabShown(node) == InspectorTab::Warp ? node : -1;
}

void InspectorPanel::forgetScoped() {
    assignments_.forget();
}

std::vector<Assignment> InspectorPanel::assignments() const {
    return assignments_.get(host_, [&] { return mods_.assignments(); });
}

bool InspectorPanel::isNoise(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Noise);
}

std::pair<double, double> InspectorPanel::reach(int param, double set) const {
    double low = set, high = set;
    for (const Assignment &m : assignments()) {
        if (m.target != param || m.bypassed || !mods_.sourceOn(m.source))
            continue;
        const auto [down, up] = modReach(m);
        low += down;
        high += up;
    }
    return {std::clamp(low, 0.0, 1.0), std::clamp(high, 0.0, 1.0)};
}

void InspectorPanel::showNode(int node) {
    // The selected tab switches its operator or filter on and off; another tab selects that one,
    // switching it on if it was off. A name in the matrix does the same.
    UiLayout &layout = controller().layout();
    if (layout.selectedNode == node)
        edit(enabledParam(node), routing_.nodeOn(node) ? 0.0 : 1.0);
    else if (!routing_.nodeOn(node))
        edit(enabledParam(node), 1.0);
    layout.selectedNode = node;
    shapes_.closePartialsBut(node); // another node shown closes the partial editor, and the table browser
    if (tables_.browsing() != node)
        tables_.close();
}

// --- Widgets.

void InspectorPanel::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    auto knob = [&](int param, Point center, float diameter, const char *label) {
        add(Kind::Knob, knobRect(center, diameter), param);
        list.back().label = label;
    };
    // A drawn shaper curve is edited right in its display, as in the big editor its edit button (beside its curve's
    // list, ClipDisplay) opens.
    auto drawnCurve = [&](int unit, Rect display) { shapes_.drawnWidgets(list, unit, display); };

    // Tabs, then the selected operator, filter or shaper.
    const int node = controller().layout().selectedNode;
    for (int i = 0; i < kNodes; ++i) // the operators', the filters' and the shapers', in three groups
        add(Kind::NodeTab, nodeTabRect(i), -1, i);
    add(Kind::Toggle, {kInspector.right() - 16.0f - 26.0f, kInspector.y + 37.0f, 26.0f, 19.0f}, enabledParam(node));
    if (node < kOps) // its letter, the handle of its output as a modulation source
        add(Kind::MacroHandle, kOpNameHandle, -1, kOpSource + node);
    auto knobRow =[&](const int (&params)[5], const char *const (&labels)[5]) {
        for (int i = 0; i < 5; ++i)
            if (params[i] >= 0)
                knob(params[i], {kInspector.x + 34.0f + i * 58.0f, kInspector.y + 110.0f}, 34.0f, labels[i]);
    };
    // An operator's block: the left column's two places (-2, -1), then the right column's four (see opKnobCenter()).
    auto opKnobRow = [&](const int (&params)[6], const char *const (&labels)[6]) {
        for (int i = 0; i < 6; ++i)
            if (params[i] >= 0)
                knob(params[i], opKnobCenter(i - 2), kOpKnobD, labels[i]);
    };
    const Str envLabels[] = {Str::LabelAttack, Str::LabelHold, Str::LabelDecay, Str::LabelSustain, Str::LabelRelease};
    // What feeds a filter or shaper, from the other nodes that are on.
    auto inputKnobs = [&](int into) {
        std::vector<int> sources;
        for (int from = 0; from < kNodes; ++from)
            if (from != into && routing_.nodeOn(from))
                sources.push_back(from);
        const float spacing = (kInspector.w - 32.0f) / std::max<size_t>(sources.size(), 6);
        for (size_t i = 0; i < sources.size(); ++i)
            knob(routeParam(sources[i], into), {kInspector.x + 16.0f + spacing * (i + 0.5f), kInspector.y + 314.0f},
                 28.0f, kNodeNames[sources[i]]);
    };
    if (isShaperNode(node)) {
        // The curve's list and the symmetric and link switches, then drive, output and mix, centered under the display
        // (the Stack curve's Stack knob after its drive, the four across), what goes straight out being its line to OUT's label;
        // a drawn curve is edited in the display unless its big editor has it.
        const int shaper = shaperOfNode(node);
        const bool custom = clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom;
        add(Kind::ListButton, shaperModeButton(custom), paramIndex(shaper, ShaperParam::Mode));
        if (custom)
            add(Kind::ClipDisplay, kShaperEditButton, -1, kFxSlots + shaper);
        const size_t switches = list.size();
        if (custom)
            add(Kind::Toggle, kShaperSymButton, paramIndex(shaper, ShaperParam::Symmetric));
        add(Kind::Toggle, kShaperLinkButton, paramIndex(shaper, ShaperParam::Link));
        merge(switches);
        const bool stacked = clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Stack;
        std::vector<int> params = {paramIndex(shaper, ShaperParam::Drive), paramIndex(shaper, ShaperParam::Output),
                                   paramIndex(shaper, ShaperParam::Mix)};
        std::vector<Str> labels = {Str::LabelDrive, Str::LabelOutput, Str::LabelMix};
        if (stacked) {
            params.insert(params.begin() + 1, paramIndex(shaper, ShaperParam::Stack));
            labels.insert(labels.begin() + 1, Str::LabelStack);
        }
        const float step = (kInspector.w - 32.0f) / 4.0f; // as far apart as four were, the three in the middle
        const float first = -0.5f * static_cast<float>(params.size() - 1);
        for (size_t i = 0; i < params.size(); ++i)
            knob(params[i],
                 {kInspector.x + 0.5f * kInspector.w + step * (first + static_cast<float>(i)), kInspector.y + 110.0f},
                 34.0f, tr(labels[i]));
        if (clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom &&
            shapes_.shapeEditing() != shaperCurve(shaper))
            drawnCurve(kFxSlots + shaper, kShaperDisplay);
        inputKnobs(node);
    } else if (node < kOps) {
        const int op = node;
        // A wave of its own: one segmented row, the list's button last in it. A table or a sample: their pickers (see
        // sourceWidgets()), under the source list.
        if (!isTableOp(op) && !isSampleOp(op)) {
            const size_t waves = list.size();
            for (int b = 0; b + 1 < kWaveButtons; ++b)
                add(Kind::WaveButton, waveButton(b), -1, op, static_cast<int>(kCommonWaves[b]));
            add(Kind::WaveMore, waveButton(kWaveButtons - 1), -1, op);
            merge(waves);
        }
        // The tabs, a noise operator's without the scope, a table's and a sample's with their own in WARP's place (a
        // table's holding its warp), then what the shown one holds.
        const InspectorTab tab = opTabShown(op);
        const OpTabs tabs = opTabsOf(op);
        for (int t = 0; t < static_cast<int>(InspectorTab::Count); ++t)
            if (const auto shown = static_cast<InspectorTab>(t);
                shown != InspectorTab::Scope && !(shown == InspectorTab::Warp && tabs != OpTabs::Plain) &&
                !(shown == InspectorTab::Source && tabs != OpTabs::Table && tabs != OpTabs::Sample))
                add(Kind::OpTab, opTab(t, tabs), -1, t);
        sourceWidgets(list, op, tab);
        if (tab == InspectorTab::Warp)
            for (int k = 0; k < 3; ++k)
                add(Kind::ScopeCycles, scopeCycleButton(k), -1, op, kScopeCycleChoices[k]);
        if (isNoise(op)) {
            // Its slope and stereo spread take the Ratio readout's place; the unison row shows the spectrum. Noise
            // has no cycle to start, so no Phase.
            const int params[] = {paramIndex(op, OpParam::NoiseSlope), paramIndex(op, OpParam::UnisonSpread), -1, -1,
                                  -1, paramIndex(op, OpParam::Pan)};
            const char *const labels[] = {tr(Str::LabelSlope), tr(Str::LabelSpread), nullptr,
                                          nullptr,             nullptr,              tr(Str::LabelPan)};
            opKnobRow(params, labels);
        } else {
            // The Ratio readout takes the first two places; fixed, Freq and Mult take its place, and Pitch and Detune,
            // which it ignores, are dimmed. A table or a sample plays at the note's pitch, with no Ratio.
            const bool fixed = isFixed(op), data = isTableOp(op) || isSampleOp(op);
            const int params[] = {fixed ? paramIndex(op, OpParam::FixedFreq) : -1,
                                  fixed ? paramIndex(op, OpParam::FixedMult) : -1,
                                  paramIndex(op, OpParam::Transpose), paramIndex(op, OpParam::Detune),
                                  paramIndex(op, OpParam::Phase), paramIndex(op, OpParam::Pan)};
            const char *const labels[] = {fixed ? tr(Str::LabelFreq) : nullptr,
                                          fixed ? tr(Str::LabelMult) : nullptr,
                                          tr(Str::LabelPitch),
                                          tr(Str::LabelDetune),
                                          tr(Str::LabelPhase),
                                          tr(Str::LabelPan)};
            const size_t row = list.size();
            const float knobY = data ? kOpDataKnobY : kOpKnobY;
            for (int i = 0; i < 6; ++i)
                if (params[i] >= 0)
                    knob(params[i], data && !fixed ? opWideKnobCenter(i - 2, knobY) : Point{opKnobCenter(i - 2).x, knobY},
                         kOpKnobD, labels[i]);
            // A sample has no cycle to start at Phase: Start takes its place, and its knob is dimmed, as Fixed dims
            // Pitch and Detune.
            for (size_t i = row; i < list.size(); ++i)
                list[i].disabled = (fixed && (list[i].param == params[2] || list[i].param == params[3])) ||
                                   (isSampleOp(op) && list[i].param == params[4]);
            if (!fixed && !data) {
                add(Kind::Knob, {kRatioBox.x, kRatioBox.y, kRatioBox.w, kOpLabelY + 17.0f - kRatioBox.y},
                    paramIndex(op, OpParam::Ratio));
                list.back().label = tr(Str::LabelRatio);
            }
            add(Kind::Toggle, kFixedSwitch, paramIndex(op, OpParam::Fixed));
            if (isCustomWave(op))
                add(Kind::PartialEdit, kPartialEditButton, -1, op);
            const int unison[] = {paramIndex(op, OpParam::Unison), paramIndex(op, OpParam::UnisonDetune),
                                  paramIndex(op, OpParam::UnisonSpread), paramIndex(op, OpParam::UnisonBlend),
                                  paramIndex(op, OpParam::PhaseRandom)};
            const Str unisonLabels[] = {Str::LabelUnison, Str::LabelDetune, Str::LabelSpread, Str::LabelBlend,
                                        Str::LabelRndPhase};
            const float step = (kInspector.w - 32.0f) / 5.0f;
            if (tab == InspectorTab::Unison) {
                // How the copies spread in pitch and where they sit, as buttons along the well's bottom, each its icon
                // (named in its tooltip), as the WARP tab's warps.
                size_t buttons = list.size();
                for (int m = 0; m < static_cast<int>(UniCurve::Count); ++m)
                    add(Kind::Choice, uniCurveButton(m), paramIndex(op, OpParam::UnisonCurve), op, m);
                merge(buttons);
                buttons = list.size();
                for (int m = 0; m < static_cast<int>(UniPan::Count); ++m)
                    add(Kind::Choice, uniPanButton(m), paramIndex(op, OpParam::UnisonPan), op, m);
                merge(buttons);
                for (int i = 0; i < 5; ++i)
                    knob(unison[i], {kInspector.x + 16.0f + step * (i + 0.5f), kUnisonKnobsY}, kOpKnobD,
                         tr(unisonLabels[i]));
            }
        }
        if (tab == InspectorTab::Inputs) {
            // A row per route into it: the name shows its node, the buttons pick its mode, the label sets its
            // depth as a wire's does. Its feedback comes last, always Phase.
            // Past what the tab holds they scroll, cut off at the list's ends.
            const std::vector<int> inputs = inputsOf(op);
            const bool feedback = routing_.wired(routeParam(op, op));
            auto shown = [](float y) { return y + kInputsRowSpan > kInputsList.y && y < kInputsList.bottom(); };
            for (size_t row = 0; row < inputs.size(); ++row) {
                const int from = inputs[row];
                const float y = inputsRowY(op, static_cast<int>(row));
                if (!shown(y))
                    continue;
                const size_t start = list.size();
                add(Kind::MatrixLabel, inputsName(y), -1, from);
                const size_t buttons = list.size();
                for (int m = 0; m < static_cast<int>(RouteMode::Count); ++m)
                    add(Kind::Choice, inputsMode(y, m), routeModeParam(from, op), from, m);
                merge(buttons);
                add(Kind::WireLabel, inputsDepth(y), routeParam(from, op), from, op);
                for (size_t i = start; i < list.size(); ++i)
                    list[i].clip = kInputsList;
            }
            if (const float y = inputsRowY(op, static_cast<int>(inputs.size())); feedback && shown(y)) {
                add(Kind::WireLabel, inputsDepth(y), routeParam(op, op), op, op);
                list.back().clip = kInputsList;
            }
            if (inputsEnd(op) > 0.0f)
                add(Kind::InputsScroll, inputsScrollTrack().inset(-3.0f));
        }
        if (tab == InspectorTab::Warp) {
            // The warps as buttons, each its icon (named in its tooltip), its amount, and hard sync to the note.
            const size_t buttons = list.size();
            for (int m = 0; m < static_cast<int>(OpWarp::Count); ++m)
                add(Kind::Choice, warpButton(m), paramIndex(op, OpParam::Warp), op, m);
            merge(buttons);
            knob(paramIndex(op, OpParam::WarpAmount), kWarpAmountKnob, kOpKnobD, tr(Str::LabelAmount));
            add(Kind::Toggle, kSyncButton, paramIndex(op, OpParam::Sync));
        }
        if (tab == InspectorTab::Envelope) {
            const int ahdsr[] = {paramIndex(op, OpParam::Attack), paramIndex(op, OpParam::Hold),
                                 paramIndex(op, OpParam::Decay), paramIndex(op, OpParam::Sustain),
                                 paramIndex(op, OpParam::Release)};
            const float step = (kInspector.w - 32.0f) / 5.0f;
            for (int i = 0; i < 5; ++i)
                knob(ahdsr[i], {kInspector.x + 16.0f + step * (i + 0.5f), kOpControlsY}, kOpKnobD,
                     tr(envLabels[i]));
        }
    } else {
        const int filter = node - kOps;
        const auto filterType = filterTypeOf(value(paramIndex(filter, FilterParam::Type)));
        add(Kind::ListButton, kFilterTypeButton, paramIndex(filter, FilterParam::Type)); // as a shaper's curve's
        if (filterType == FilterType::Morph) { // its slope's list and notch switch side by side
            add(Kind::ListButton, morphSlopeButton(kFilterDisplay), paramIndex(filter, FilterParam::Slope));
            add(Kind::Toggle, morphNotchButton(kFilterDisplay), paramIndex(filter, FilterParam::MorphMode));
        } else {
            add(Kind::ListButton, kFilterSlopeButton, paramIndex(filter, FilterParam::Slope));
        }
        const FilterKnobLabels names = filterKnobLabels(filterType, false);
        const int params[] = {paramIndex(filter, FilterParam::Cutoff), paramIndex(filter, FilterParam::Resonance),
                              paramIndex(filter, FilterParam::KeyTrack), paramIndex(filter, FilterParam::Drive),
                              paramIndex(filter, FilterParam::Mix)};
        const char *const labels[] = {tr(names.cutoff), tr(names.resonance), tr(Str::LabelKeyTrack),
                                      tr(Str::LabelDrive), tr(Str::LabelMix)};
        if (filterType == FilterType::Formant || filterType == FilterType::Morph) { // the six closer together
            const bool formant = filterType == FilterType::Formant;
            const int six[] = {params[0],
                               params[1],
                               paramIndex(filter, formant ? FilterParam::Vowel : FilterParam::Morph),
                               params[2],
                               params[3],
                               params[4]};
            const char *const sixLabels[] = {
                labels[0], labels[1], tr(formant ? Str::LabelVowel : Str::LabelMorph), labels[2], labels[3], labels[4]};
            for (int i = 0; i < 6; ++i)
                knob(six[i], filterSixKnob(i), kFilterSixKnobD, sixLabels[i]);
        } else {
            knobRow(params, labels);
        }
        const Point handle = filterHandle(kFilterDisplay, value(params[0]), value(params[1]));
        add(Kind::FilterPoint, {handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f}, -1, filter);
        inputKnobs(node);
    }
    // The operator's envelope's handles, on its ENVELOPE tab, as the View lists every envelope's.
    if (node < kOps && opTabShown(node) == InspectorTab::Envelope)
        host_.envelopeWidgets(list, node);
}

void InspectorPanel::popups(std::vector<Widget> &list) const {
    if (covered())
        return;
    const WidgetAdder add{list};
    const int open = host_.openList();
    // A shaper's list of curves or a filter's types or slopes, under its button on its page.
    const int pageNode = controller().layout().selectedNode;
    const bool shaperList = shaperOf(open) >= 0 && pageNode == kOps + kFilters + shaperOf(open);
    const bool slopeList =
        open >= 0 && isFilterNode(pageNode) && open == paramIndex(pageNode - kOps, FilterParam::Slope);
    const bool typeList =
        open >= 0 && isFilterNode(pageNode) && open == paramIndex(pageNode - kOps, FilterParam::Type);
    const bool morphSlope = slopeList && filterTypeOf(value(paramIndex(pageNode - kOps, FilterParam::Type))) ==
                                             FilterType::Morph;
    if (shaperList || slopeList || typeList) {
        const Rect button = morphSlope ? morphSlopeButton(kFilterDisplay)
                            : slopeList ? kFilterSlopeButton
                            : typeList  ? kFilterTypeButton
                                        : shaperModeButton(clipMode(value(open)) == ClipMode::Custom);
        const int items = listCount(open);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::ListMenu, listMenu(button, items), -1, 1);
        for (int item = 0; item < items; ++item)
            add(Kind::ListItem, listMenuItem(button, items, item), open, -1, listItemValue(open, item));
    }
    // The open list of more waves, under "...", while its operator shows in the inspector.
    if (waveMenu_ >= 0 && waveMenu_ == controller().layout().selectedNode) {
        add(Kind::WaveMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::WaveMenu, waveMenu(), -1, 1);
        for (size_t i = 0; i < std::size(kMoreWaves); ++i)
            add(Kind::WaveMenuItem, waveMenuItem(static_cast<int>(i)), -1, waveMenu_, static_cast<int>(kMoreWaves[i]));
    }
    // The open source list, under its button, while its operator shows in the inspector.
    if (sourceMenu_ >= 0 && sourceMenu_ == controller().layout().selectedNode) {
        add(Kind::OpSourceMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::OpSourceMenu, opSourceMenu(), -1, 1);
        for (int source = 0; source < 3; ++source)
            add(Kind::OpSourceItem, opSourceItem(source), -1, sourceMenu_, source);
    }
}

// --- Painting.

void InspectorPanel::paintBack(Canvas &c) {
    const int node = controller().layout().selectedNode;
    // The lens: a card floating over the panel's left end, the graph's backdrop faintly through it, outlined in the
    // color of the node it shows; a notch on its right side where the leader from that node meets it.
    const Color edge = nodeColor(node).alpha(0.35f);
    const Color face = kRaised.alpha(0.92f);
    // In a flat theme, as its cards, on its shadow alone without an outline.
    softShadow(c, kInspector, kRadiusPanel, 4, 1.25f, 4.0f, lightTheme() ? 0.05f : 0.16f); // clear of the panel's top
    c.fillRect(kInspector, face, kRadiusPanel);
    if (outlined())
        c.strokeRect(kInspector, edge, 1.0f, kRadiusPanel);
    // The routing graph shows the node, while it shows (as RoutingPanel::graphShown() has it).
    const bool graph = controller().layout().view == RoutingView::Graph && !host_.shapeEditorOpen() &&
                       !host_.partialEditorOpen() && !host_.tableBrowserOpen();
    if (const std::optional<Point> leader = lensLeaderStart(routing_, node, graph)) {
        const float x = kInspector.right(), y = leader->y;
        const Point notch[] = {{x - 1.0f, y - 6.0f}, {x + 6.0f, y}, {x - 1.0f, y + 6.0f}};
        c.fillPolygon(notch, 3, face);
        const Point side[] = {{x, y - 6.0f}, {x + 6.0f, y}, {x, y + 6.0f}};
        if (outlined())
            c.polyline(side, 3, edge, 1.0f);
    }
    // The tabs' groups, a hairline between two.
    for (int group = 1; group <= 2; ++group) {
        const float x = nodeTabDivider(group);
        c.line({x, kInspector.y + 13.0f}, {x, kInspector.y + 27.0f}, kBorder, 1.0f);
    }
    // The title row leaves room for the on/off switch at the right.
    Rect summaryRect{kTitleRow.x, kTitleRow.y, kInspector.w - 84.0f, kTitleRow.h};
    // While the node is off, the page under its title is drawn dimmed (its widgets are too, see dimmedAreas()).
    struct PageDim {
        Canvas &c;
        bool on;
        PageDim(Canvas &canvas, bool dim) : c(canvas), on(dim) {
            if (on)
                c.pushLayer({0.0f, 0.0f}, kDimOpacity, kInspectorPage);
        }
        ~PageDim() {
            if (on)
                c.popLayer();
        }
    };
    // A filter's response and a shaper's curve are drawn in the lens's own shade of the displays (inspectorDisplay()).
    struct LensDisplay {
        Color saved = look.display;
        LensDisplay() { look.display = inspectorDisplay(); }
        ~LensDisplay() { look.display = saved; }
    };

    if (isShaperNode(node)) {
        const int shaper = shaperOfNode(node);
        paintPageTitle(c, node, summaryRect, clipModeName(clipMode(value(paramIndex(shaper, ShaperParam::Mode)))),
                       boxRoutes(node));
        const PageDim page(c, !routing_.nodeOn(node));
        const LensDisplay display;
        paintShaperPage(c, shaper);
        return;
    }
    if (node < kOps) {
        const int op = node;
        // Its title row holds its letter, its output's handle as a modulation source, then the source list (Wave,
        // Wavetable or Sample) in a summary's place: what a summary would say, the wave, the ratio, the unison, the
        // warp and where it goes, the page and the graph show.
        c.text(kOpLetter, kNodeNames[op], kTextHeading, nodeColor(op), Align::Left, true);
        const PageDim page(c, !routing_.nodeOn(node));
        // The line between the operator's sound and its tabs, then the well: the tabs along its top edge, on a line
        // the shown one's accent sits on, and the display under them.
        c.line({kInspector.x + 16.0f, kOpRuleY}, {kInspector.right() - 16.0f, kOpRuleY}, kBorder, 1.0f);
        const InspectorTab shown = opTabShown(op);
        const Rect well = shown == InspectorTab::Inputs ? kOpWellTall : kOpWell;
        c.fillRect(well, kDisplayColor, kRadiusCard);
        c.line({well.x, kOpTabsY + kOpTabH - 0.5f}, {well.right(), kOpTabsY + kOpTabH - 0.5f}, kBorder, 1.0f);
        switch (shown) {
        case InspectorTab::Unison:
            if (isNoise(op))
                paintNoiseSlope(c, kNoiseRect, op);
            else
                paintUnison(c, kUnisonRect, op);
            break;
        case InspectorTab::Envelope:
            host_.paintEnvelope(c, kOpEnvRect, op, kAccent, true);
            break;
        case InspectorTab::Scope:
        case InspectorTab::Warp:
            paintScope(c, kScopeRect, op);
            break;
        case InspectorTab::Source:
            if (isTableOp(op))
                paintTableTab(c, kTableDisplay, op);
            else
                paintSampleTab(c, kSampleDisplay, op);
            if (fileDrop_ == op) { // a file held over the display: it lands here
                c.fillRect(kSampleDisplay, kAccent.alpha(0.10f), kRadiusCard);
                c.strokeRect(kSampleDisplay.inset(-3.0f), kAccent, 2.0f, kRadiusCard + 3.0f);
            }
            break;
        case InspectorTab::Inputs:
        case InspectorTab::Count:
            paintInputs(c, op);
            break;
        }
        return;
    }

    const int filter = node - kOps;
    const auto type = static_cast<FilterType>(std::lround(value(paramIndex(filter, FilterParam::Type))));
    std::string head = filterTypeName(type);
    if (hasSlope(type))
        head += " " + std::to_string(slopeDb(playedSlope(type, filterSlope(value(paramIndex(filter, FilterParam::Slope)))))) +
                " dB";
    paintPageTitle(c, node, summaryRect, head, boxRoutes(node));
    const PageDim page(c, !routing_.nodeOn(node));
    const LensDisplay display;
    // A voice filter tracks the newest voice's note; with none, the curve is back at the knob's C4.
    const Telemetry &t = host_.telemetry();
    const FilterReads reads{[this](int index) { return value(index); },
                            [this](int index) {
                                const ParamDef &def = table_[index];
                                return (host_.liveValue(index) - def.min) / (def.max - def.min);
                            },
                            [this](int param, double set) { return reach(param, set); }};
    paintFilterResponse(c, kFilterDisplay, voiceFilterParams(filter), reads, t.voices > 0 ? t.notePitch : 60.0,
                        kFilterColor, &t.filterSpectrum[filter], t.age);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), kTextLabel, kFaint,
           Align::Left, true);
}

// A filter's or shaper's routes, for its page's summary: what goes into it, the filters and shapers it feeds and the
// output, and the operators it modulates.
std::string InspectorPanel::boxRoutes(int node) const {
    const std::string dot = "  \xC2\xB7  ";
    std::string from, to, mods;
    auto name = [](std::string &list, const char *node) { list += (list.empty() ? "" : ", ") + std::string(node); };
    for (int other = 0; other < kNodes; ++other) {
        if (other == node || !routing_.nodeOn(other))
            continue;
        if (value(routeParam(other, node)) > 0.0)
            name(from, kNodeNames[other]);
        if (value(routeParam(node, other)) > 0.0)
            name(other < kOps ? mods : to, kNodeNames[other]);
    }
    if (value(outParam(node)) > 0.0)
        name(to, tr(Str::SummaryOut));
    std::string text = dot + (from.empty() ? std::string(tr(Str::SummaryNoInput)) : trf(Str::SummaryFrom, {from}));
    if (!to.empty())
        text += dot + trf(Str::SummaryTo, {to});
    if (!mods.empty())
        text += dot + trf(Str::SummaryMods, {mods});
    return text;
}

// A page's title: the node's letter in its color at the heading's size, then the summary's head (an operator's wave,
// a filter's type, a shaper's curve) in full text and the rest dimmed, cut off at r's right end.
void InspectorPanel::paintPageTitle(Canvas &c, int node, Rect r, const std::string &head, const std::string &rest) {
    const char *const letter = kNodeNames[node];
    const float letterW = std::ceil(c.textWidth(letter, kTextHeading, true)) + 8.0f;
    c.text({r.x, r.y, letterW, r.h}, letter, kTextHeading, nodeColor(node), Align::Left, true);
    const Rect text{r.x + letterW, r.y, std::max(r.w - letterW, 0.0f), r.h};
    const float headW = std::min(std::ceil(c.textWidth(head, kTextBody)) + 1.0f, text.w);
    c.text({text.x, text.y, headW, text.h}, head, kTextBody, kText, Align::Left);
    if (text.w > headW)
        c.text({text.x + headW, text.y, text.w - headW, text.h}, rest, kTextBody, kDim, Align::Left);
}

void InspectorPanel::paintShaperPage(Canvas &c, int shaper) {
    c.fillRect(kShaperDisplay, kDisplayColor, kRadiusCard);
    shapes_.paintShaperCurve(c, kShaperDisplay, kFxSlots + shaper);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), kTextLabel, kFaint,
           Align::Left, true);
}

// Where the unison copies sit: left to right by stereo position, up and down by detune.
void InspectorPanel::paintUnison(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    c.line({r.center().x, r.y + 6.0f}, {r.center().x, r.bottom() - 6.0f}, kBorder, 1.0f);
    c.line({r.x + 18.0f, r.center().y}, {r.right() - 18.0f, r.center().y}, kBorder.alpha(0.6f), 1.0f);
    c.text({r.x + 4.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({r.right() - 16.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "R", kTextLabel, kFaint);
    const int copies = std::clamp(static_cast<int>(std::lround(value(paramIndex(op, OpParam::Unison)))), 1, kMaxUnison);
    // Up and down by detune, its whole range from the middle to an edge.
    const double detune = value(paramIndex(op, OpParam::UnisonDetune)) / kMaxUnisonDetune;
    const double spread = value(paramIndex(op, OpParam::UnisonSpread));
    // As the voices spread them (unison.h); the Random modes as one note might.
    const auto curve = static_cast<UniCurve>(std::clamp(std::lround(value(paramIndex(op, OpParam::UnisonCurve))), 0L,
                                                        static_cast<long>(UniCurve::Count) - 1));
    const auto pan = static_cast<UniPan>(std::clamp(std::lround(value(paramIndex(op, OpParam::UnisonPan))), 0L,
                                                    static_cast<long>(UniPan::Count) - 1));
    const UnisonSpreadOut at =
        unisonSpread(copies, curve, pan, value(paramIndex(op, OpParam::UnisonBlend)), 1u, op);
    double loudest = 0.0;
    for (int k = 0; k < copies; ++k)
        loudest = std::max(loudest, at.weight[k]);
    for (int k = 0; k < copies; ++k) {
        const Point dot{r.center().x + static_cast<float>(at.pan[k] * spread) * (r.w * 0.5f - 24.0f),
                        r.center().y - static_cast<float>(at.detune[k] * detune) * (r.h * 0.5f - 10.0f)};
        // Blend shows in each copy's size and strength: the quieter, the smaller and fainter.
        const float level = static_cast<float>(at.weight[k] / loudest);
        const Color color = (copies > 1 ? kAccent : kDim).alpha(0.35f + 0.65f * level);
        c.line({dot.x, r.center().y}, dot, color.alpha(0.4f * level), 1.5f);
        c.fillCircle(dot, 1.5f + 2.5f * level, color);
    }
}

bool InspectorPanel::isCustomWave(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Custom);
}

bool InspectorPanel::isFixed(int op) const {
    return value(paramIndex(op, OpParam::Fixed)) >= 0.5;
}

// A fixed operator's Freq x Mult (see detail::fixedHzText()).
std::string InspectorPanel::fixedHzText(int op, bool compact) const {
    return detail::fixedHzText(value(paramIndex(op, OpParam::FixedFreq)), value(paramIndex(op, OpParam::FixedMult)),
                               compact);
}

// Switched to fixed, the operator plays where its ratio put it at A4 (a table or a sample, which has none, at the A4
// itself): its Freq and Mult are set to that, with the smallest Mult that keeps the Freq in its range, in the same undo
// step.
void InspectorPanel::switchFixed(int op) {
    const int fixed = paramIndex(op, OpParam::Fixed);
    std::vector<std::pair<int, double>> changes;
    if (!isFixed(op)) {
        const double hz = isTableOp(op) || isSampleOp(op) ? 440.0 : 440.0 * ratioFromNorm(value(paramIndex(op, OpParam::Ratio)));
        int mult = 0;
        while (mult + 1 < kFixedMultCount && hz / kFixedMults[mult] > kMaxFixedFreq)
            ++mult;
        changes.push_back({paramIndex(op, OpParam::FixedFreq), normFromFixedFreq(hz / kFixedMults[mult])});
        changes.push_back({paramIndex(op, OpParam::FixedMult), static_cast<double>(mult)});
    }
    changes.push_back({fixed, isFixed(op) ? 0.0 : 1.0});
    mods_.editMany(changes);
}

std::vector<int> InspectorPanel::inputsOf(int op) const {
    std::vector<int> inputs;
    for (int from = 0; from < kNodes; ++from)
        if (from != op && routing_.nodeOn(from) && routing_.wired(routeParam(from, op)))
            inputs.push_back(from);
    return inputs;
}

int InspectorPanel::inputsRows(int op) const {
    return static_cast<int>(inputsOf(op).size()) + (routing_.wired(routeParam(op, op)) ? 1 : 0);
}

float InspectorPanel::inputsEnd(int op) const {
    const int rows = inputsRows(op);
    if (rows == 0)
        return 0.0f;
    const float length = static_cast<float>(rows - 1) * kInputsStep + kInputsRowSpan;
    return std::max((length - (kInputsBottom - kInputsTop)) / kInputsStep, 0.0f);
}

float InspectorPanel::inputsFirst(int op) const {
    return std::clamp(inputsScroll_, 0.0f, inputsEnd(op));
}

float InspectorPanel::inputsRowY(int op, int row) const {
    return kInputsTop + (static_cast<float>(row) - inputsFirst(op)) * kInputsStep;
}

Rect InspectorPanel::inputsThumb(int op) const {
    const Rect track = inputsScrollTrack();
    const float rows = static_cast<float>(std::max(inputsRows(op), 1));
    const float shown = (kInputsBottom - kInputsTop) / kInputsStep;
    const float h = std::max(track.h * std::min(shown / rows, 1.0f), 16.0f);
    return {track.x, track.y + (track.h - h) * inputsFirst(op) / std::max(inputsEnd(op), 0.001f), track.w, h};
}

// The names of the nodes routed into the operator and their modes' names, its feedback's row, or a line saying nothing
// is routed into it yet; the buttons and labels draw themselves, cut off at the list's ends as they scroll.
void InspectorPanel::paintInputs(Canvas &c, int op) {
    const std::vector<int> inputs = inputsOf(op);
    const bool feedback = routing_.wired(routeParam(op, op));
    if (inputs.empty() && !feedback) {
        c.text({kInspector.x + 16.0f, kInputsTop, kInspector.w - 32.0f, 40.0f},
               trf(Str::InputsNone, {nodeTitle(op)}), kTextBody, kDim, Align::Center);
        return;
    }
    c.pushClip(kInputsList);
    for (size_t row = 0; row < inputs.size(); ++row) {
        const int from = inputs[row];
        const float y = inputsRowY(op, static_cast<int>(row));
        const std::optional<Widget> &hover = host_.hovered();
        const bool hovered = hover && hover->kind == Kind::MatrixLabel && hover->a == from &&
                             hover->rect.y == inputsName(y).y;
        c.text(inputsName(y), kNodeNames[from], kTextBody, hovered ? kText : nodeColor(from), Align::Left, true);
        const auto mode = routeMode(value(routeModeParam(from, op)));
        c.text(inputsModeName(y), routeModeName(mode), kTextLabel, kDim, Align::Left);
    }
    if (feedback) {
        const float y = inputsRowY(op, static_cast<int>(inputs.size()));
        c.text(inputsName(y), kNodeNames[op], kTextBody, nodeColor(op), Align::Left, true);
        c.text({inputsMode(y, 0).x, y, inputsDepth(y).x - inputsMode(y, 0).x - 6.0f, 18.0f},
               trf(Str::InputsFeedback, {kNodeNames[op]}), kTextLabel, kDim, Align::Left);
    }
    c.popClip();
}

InspectorTab InspectorPanel::opTabShown(int op) const {
    const InspectorTab tab = controller().layout().opTab;
    const bool data = isTableOp(op) || isSampleOp(op);
    // A table or a sample has no WARP: its TABLE or SAMPLE tab is in WARP's place (a table's with its warp). An
    // operator without a table or a sample, its envelope in the TABLE or SAMPLE tab's.
    if (tab == InspectorTab::Scope || tab == InspectorTab::Warp) // the scope is on the WARP tab
        return isNoise(op) ? InspectorTab::Envelope : data ? InspectorTab::Source : InspectorTab::Warp;
    if (tab == InspectorTab::Source && !data)
        return InspectorTab::Envelope;
    return tab;
}

// Whether a source modulates one of the knobs on an operator's tab, which shows a dot on the tab while it is hidden.
bool InspectorPanel::opTabModulated(int op, InspectorTab tab) const {
    std::vector<int> params;
    if (tab == InspectorTab::Unison && !isNoise(op))
        params = {paramIndex(op, OpParam::Unison), paramIndex(op, OpParam::UnisonDetune),
                  paramIndex(op, OpParam::UnisonSpread), paramIndex(op, OpParam::UnisonBlend),
                  paramIndex(op, OpParam::PhaseRandom)};
    if (tab == InspectorTab::Envelope)
        params = {paramIndex(op, OpParam::Attack), paramIndex(op, OpParam::Hold), paramIndex(op, OpParam::Decay),
                  paramIndex(op, OpParam::Sustain), paramIndex(op, OpParam::Release)};
    if (tab == InspectorTab::Warp && !isNoise(op))
        params = {paramIndex(op, OpParam::WarpAmount)};
    if (tab == InspectorTab::Source && isTableOp(op)) // Position, and the warp's amount beside it
        params = {paramIndex(op, OpParam::TablePos), paramIndex(op, OpParam::WarpAmount)};
    for (const Assignment &m : assignments())
        if (std::find(params.begin(), params.end(), m.target) != params.end())
            return true;
    return false;
}

OpWarp InspectorPanel::opWarp(int op) const {
    return static_cast<OpWarp>(std::clamp(std::lround(value(paramIndex(op, OpParam::Warp))), 0L,
                                          static_cast<long>(OpWarp::Count) - 1));
}

bool InspectorPanel::opSynced(int op) const {
    return !isNoise(op) && value(paramIndex(op, OpParam::Sync)) >= 0.5;
}

// `cycles` of the operator's wave across `r`, as it plays it: read through its warp (unless `plain`), and synced, a
// cycle of the note each, its own cycles restarting at its Phase (at its ratio over the note; fixed, over a C4). The
// wave is its icon's shape, or a Custom wave's partials, at full scale between r's top and bottom.
std::vector<Point> InspectorPanel::opWavePoints(int op, Rect r, int cycles, bool plain) const {
    const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))));
    std::function<double(double)> shape;
    if (wave == OpWave::Custom) {
        std::vector<std::pair<double, double>> partials; // level, phase
        const int count = std::clamp(static_cast<int>(std::lround(value(partialCountIndex(op)))), 1, kPartials);
        for (int k = 0; k < count; ++k)
            partials.emplace_back(value(partialLevelIndex(op, k)), value(partialPhaseIndex(op, k)));
        auto sum = [partials](double t) {
            double x = 0.0;
            for (size_t k = 0; k < partials.size(); ++k)
                x += partials[k].first * std::sin(2.0 * kPi * (partials[k].second + (k + 1.0) * t));
            return x;
        };
        double peak = 1e-9;
        for (int i = 0; i < 256; ++i)
            peak = std::max(peak, std::abs(sum(i / 256.0)));
        shape = [sum, peak](double t) { return sum(t) / peak; };
    } else {
        // Its icon's points along a cycle 1 wide, -1..1 tall: a value between two of them is on the line between.
        const std::vector<Point> points = wavePoints(wave, {0.0f, -1.0f, 1.0f, 2.0f});
        shape = [points](double t) {
            for (size_t k = 1; k < points.size(); ++k)
                if (points[k].x >= t && points[k].x > points[k - 1].x) {
                    const double f = (t - points[k - 1].x) / (points[k].x - points[k - 1].x);
                    return -(points[k - 1].y + (points[k].y - points[k - 1].y) * std::clamp(f, 0.0, 1.0));
                }
            return points.empty() ? 0.0 : -static_cast<double>(points.back().y);
        };
    }
    const WarpCoefs coefs = warpCoefs(plain ? OpWarp::Off : opWarp(op), value(paramIndex(op, OpParam::WarpAmount)));
    const bool synced = opSynced(op);
    double over = 1.0; // its cycles to the note's, synced
    if (synced) {
        const double fixed =
            fixedHz(value(paramIndex(op, OpParam::FixedFreq)), value(paramIndex(op, OpParam::FixedMult)));
        over = isFixed(op) ? fixed / 261.63
                           : ratioFromNorm(value(paramIndex(op, OpParam::Ratio))) *
                                 std::exp2(value(paramIndex(op, OpParam::Transpose)) / 12.0 +
                                           value(paramIndex(op, OpParam::Detune)) / 1200.0);
        over = std::clamp(over, 0.01, 64.0);
    }
    const double start = synced ? value(paramIndex(op, OpParam::Phase)) : 0.0;
    const int steps = std::max(64, static_cast<int>(r.w * 2.0f));
    std::vector<Point> points;
    points.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps * cycles;
        double p = t;
        if (synced) {
            const double note = std::min(t - std::floor(t), i == steps ? 1.0 : 0.999999);
            p = start + note * over;
        }
        p -= std::floor(p);
        if (i == steps && !synced)
            p = 1.0; // the cycle's end, not its start again
        const double y = std::clamp(shape(warpPhase(coefs, p)), -1.15, 1.15);
        points.push_back(
            {r.x + r.w * static_cast<float>(i) / steps, r.center().y - static_cast<float>(y) * r.h * 0.5f});
    }
    return points;
}

// The operator's output in the newest voice, a point per bin across the display: the cycles the switch in the corner
// picks, at their real level between dashed lines at full scale, the level its peaks follow in the corner. A slow operator's span is drawn as it fills, up to
// its cursor, the last one's rest faintly after it. With nothing captured, the wave's plain shape is drawn faintly, or,
// warped or synced, the wave as they make it over its plain shape; the warp is named along the bottom.
void InspectorPanel::paintScope(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const int cycles = controller().layout().scopeCycles;
    const Rect inner{r.x + 8.0f, r.y + 26.0f, r.w - 16.0f, r.h - 46.0f};
    const float middle = inner.center().y;
    const float half = inner.h * 0.5f;
    c.line({inner.x, inner.y}, {inner.right(), inner.y}, kBorder, 1.0f, true);
    c.line({inner.x, inner.bottom()}, {inner.right(), inner.bottom()}, kBorder, 1.0f, true);
    c.line({inner.x, middle}, {inner.right(), middle}, kBorder.alpha(0.6f), 1.0f);
    for (int k = 1; k < cycles; ++k) {
        const float x = inner.x + inner.w * k / cycles;
        c.line({x, inner.y - 6.0f}, {x, inner.bottom() + 6.0f}, kBorder.alpha(0.6f), 1.0f, true);
    }

    // The warp along the bottom: its name and amount, and SYNC while synced.
    const OpWarp warp = opWarp(op);
    const bool shaped = warp != OpWarp::Off || opSynced(op);
    if (shaped) {
        std::string caption = warpName(warp);
        if (warp != OpWarp::Off) {
            char amount[32];
            const int param = paramIndex(op, OpParam::WarpAmount);
            formatParam(param, value(param), amount, sizeof(amount));
            caption += std::string("  ") + amount;
        }
        if (opSynced(op))
            caption += std::string("  \xC2\xB7  ") + tr(Str::CaptionNoteSync);
        c.text({r.x + 8.0f, r.bottom() - 17.0f, r.w - 16.0f, 14.0f}, caption, kTextSmall, kDim, Align::Left);
    }

    const int bins = cycles * kScopeBinsPerCycle;
    if (host_.telemetry().voices == 0 || host_.telemetry().scopeBins != bins) {
        // No note: the wave as the operator plays it (warped, and synced a cycle of the note each), over its plain
        // shape faintly while either changes it.
        if (shaped) {
            const std::vector<Point> plain = opWavePoints(op, inner, cycles, true);
            c.polyline(plain.data(), plain.size(), kDim.alpha(0.45f), 1.0f, true);
        }
        // Unchanged, the plain shape faintly, as a stand-in for the trace; warped or synced, what they make of it.
        const std::vector<Point> points = opWavePoints(op, inner, cycles, false);
        if (shaped)
            c.polyline(points.data(), points.size(), kAccent.alpha(0.85f), 1.5f);
        else
            c.polyline(points.data(), points.size(), kDim.alpha(0.5f), 1.5f, true);
        return;
    }
    // A bin per point, but no more points than the display has pixels across, twice over.
    const int step = std::max(1, bins / static_cast<int>(inner.w * 2.0f));
    auto at = [&](int bin) { // full scale at the dashed lines; louder runs a little past them
        const float level = std::clamp(host_.telemetry().scope[bin], -1.15f, 1.15f);
        return Point{inner.x + inner.w * bin / (bins - 1), middle - level * half};
    };
    const int cursor = host_.telemetry().scopeCursor < 0.0f
                           ? bins
                           : std::clamp(static_cast<int>(host_.telemetry().scopeCursor * bins), 0, bins);
    std::vector<Point> fresh, stale;
    for (int bin = 0; bin < bins; bin += step)
        (bin < cursor ? fresh : stale).push_back(at(bin));
    if (!stale.empty() && !fresh.empty())
        stale.insert(stale.begin(), fresh.back());
    if (stale.size() > 1)
        c.polyline(stale.data(), stale.size(), kAccent.alpha(0.3f), 1.5f);
    if (fresh.size() > 1)
        plotLine(c, fresh.data(), fresh.size(), kAccent, 1.5f);
    if (cursor < bins) {
        const float x = inner.x + inner.w * cursor / bins;
        c.line({x, inner.y - 6.0f}, {x, inner.bottom() + 6.0f}, kDim, 1.0f);
    }
    if (opScopeLevel_ > 1e-4f) // its level, steadier than each span's peak (see followScopeLevel())
        c.text({r.x + 8.0f, r.y + 6.0f, 100.0f, 16.0f}, number("%.1f dB", 20.0 * std::log10(opScopeLevel_)), kTextLabel,
               kFaint, Align::Left);
}

// The noise's spectrum from 20 Hz to 20 kHz, over faint white, pink and brown slopes.
void InspectorPanel::paintNoiseSlope(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const Rect inner{r.x + 10.0f, r.y + 8.0f, r.w - 56.0f, r.h - 16.0f};
    // Every slope starts at the top left, at 20 Hz; brown falls the full height by 20 kHz.
    auto line = [&](double slope, Color color, float width) {
        c.line({inner.x, inner.y}, {inner.right(), inner.y + static_cast<float>(slope) * inner.h}, color, width);
    };
    const Str names[] = {Str::NoiseWhiteCaption, Str::NoisePinkCaption, Str::NoiseBrownCaption};
    for (int i = 0; i < 3; ++i) {
        line(i * 0.5, kBorder, 1.0f);
        const float y = inner.y + i * 0.5f * inner.h;
        c.text({inner.right() + 6.0f, y - 6.0f, 40.0f, 12.0f}, tr(names[i]), kTextLabel, kFaint, Align::Left);
    }
    const float drop = static_cast<float>(value(paramIndex(op, OpParam::NoiseSlope))) * inner.h;
    const Point slope[] = {{inner.x, inner.y}, {inner.right(), inner.y + drop}};
    plotLine(c, slope, 2, kAccent, 2.0f);
}

bool InspectorPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    if (paintSourceWidget(c, w, hovered)) // a Wavetable or Sample operator's (see opsource.cpp)
        return true;
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::OpTab: {
        // Names on the tabs' line, the shown one's accent over it; a hidden one with a modulated knob has a dot.
        const int op = controller().layout().selectedNode;
        const auto tab = static_cast<InspectorTab>(w.a);
        const bool selected = opTabShown(op) == tab;
        const Str names[] = {isNoise(op) ? Str::TabSpectrum : Str::TabUnison, Str::TabEnvelope, Str::TabScope,
                             Str::TabInputs, Str::TabWarp, isSampleOp(op) ? Str::TabSample : Str::TabTable};
        static_assert(std::size(names) == static_cast<size_t>(InspectorTab::Count));
        c.text(w.rect, tr(names[w.a]), kTextLabel, selected || hovered ? kText : kDim, Align::Center, true);
        if (selected)
            c.line({w.rect.x + 4.0f, w.rect.bottom() - 1.0f}, {w.rect.right() - 4.0f, w.rect.bottom() - 1.0f},
                   kAccent, 2.0f);
        else if (opTabModulated(op, tab))
            c.fillCircle({w.rect.right() - 3.0f, w.rect.y + 5.0f}, 2.0f, kAccent);
        break;
    }
    case Kind::ScopeCycles: {
        const bool selected = controller().layout().scopeCycles == w.b;
        host_.paintButtonFace(c, w, selected, hovered, kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, std::to_string(w.b), kTextLabel, selected ? litInk(kText) : (hovered ? kText : kDim),
               Align::Center, selected);
        break;
    }
    case Kind::MacroHandle: { // an operator's letter (painted with its page), lit under the mouse and while dragged
        if (!isOpSource(w.a))
            return false;
        if (hovered || active) {
            const int op = w.a - kOpSource;
            // As far either side of the letter, however wide it is.
            const float pad = kTitleRow.x - w.rect.x;
            const float width = c.textWidth(kNodeNames[op], kTextHeading, true);
            c.fillRect({w.rect.x, w.rect.y, width + 2.0f * pad, w.rect.h}, nodeColor(op).alpha(0.25f), kRadiusControl);
            c.text(kOpLetter, kNodeNames[op], kTextHeading, nodeColor(op), Align::Left, true);
        }
        break;
    }
    case Kind::NodeTab: {
        const bool selected = controller().layout().selectedNode == w.a;
        const bool enabled = routing_.nodeOn(w.a);
        const Color color = nodeColor(w.a);
        // On the lens, the panel's color, as the nodes in the graph are; a switched-off one's not filled.
        c.fillRect(w.rect, selected ? color.alpha(enabled ? 1.0f : 0.35f) : (hovered ? kTrack : kPanel.alpha(enabled ? 1.0f : 0.0f)),
                   kRadiusControl);
        // An enabled node's name takes a little of its color.
        // On its solid fill, in the ink a flat theme's lit switches take.
        const Color ink = look.flat ? litInk(kText) : kBackground;
        c.text(w.rect, kNodeNames[w.a], kTextBody, selected ? ink : (enabled ? mix(kDim, color, 0.3f) : kFaint),
               Align::Center, true);
        if (routing_.soundsOut(w.a))
            c.fillCircle({w.rect.right() - 5.0f, w.rect.y + 5.0f}, 2.0f, selected ? ink : color);
        break;
    }
    case Kind::WaveButton: {
        const bool selected = std::lround(value(paramIndex(w.a, OpParam::Wave))) == w.b;
        host_.paintButtonFace(c, w, selected, hovered, kAccent, 0.18f, kRadiusControl); // a part of the waves' row
        const std::vector<Point> points =
            waveIcon(static_cast<OpWave>(w.b), {w.rect.x + 6.0f, w.rect.y + 6.0f, w.rect.w - 12.0f, w.rect.h - 12.0f});
        c.polyline(points.data(), points.size(), selected ? litInk(kAccent) : kDim, 1.5f);
        break;
    }
    case Kind::WaveMore: {
        // Three dots, or the glyph of the wave picked from the list, with a chevron.
        const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
        const bool selected = isMoreWave(wave);
        const bool lit = hovered || waveMenu_ == w.a;
        host_.paintButtonFace(c, w, selected, lit, kAccent, 0.18f, kRadiusControl); // the last part of the waves' row
        if (selected) {
            const Rect icon{w.rect.x + 4.0f, w.rect.y + 6.0f, w.rect.w - 15.0f, w.rect.h - 12.0f};
            const std::vector<Point> points = waveIcon(wave, icon);
            c.polyline(points.data(), points.size(), litInk(kAccent), 1.5f);
            const Point m{w.rect.right() - 6.0f, w.rect.center().y};
            const Point chevron[] = {{m.x - 3.0f, m.y - 1.5f}, {m.x, m.y + 1.5f}, {m.x + 3.0f, m.y - 1.5f}};
            c.polyline(chevron, 3, look.flat ? litInk(kText) : lit ? kText : kDim, 1.2f);
        } else {
            for (int d = -1; d <= 1; ++d)
                c.fillCircle({w.rect.center().x + d * 5.0f, w.rect.center().y}, 1.6f, lit ? kText : kDim);
        }
        break;
    }
    case Kind::WaveMenu:
        if (w.a == 1) {
            paintMenuPanel(c, w.rect);
        }
        break;
    case Kind::WaveMenuItem: {
        const bool selected = std::lround(value(paramIndex(w.a, OpParam::Wave))) == w.b;
        paintMenuRow(c, w.rect, hovered, selected);
        const Rect icon{w.rect.x + 8.0f, w.rect.y + 7.0f, 22.0f, w.rect.h - 14.0f};
        const std::vector<Point> points = waveIcon(static_cast<OpWave>(w.b), icon);
        c.polyline(points.data(), points.size(), selected ? kAccent : kDim, 1.5f);
        c.text({icon.right() + 10.0f, w.rect.y, w.rect.right() - icon.right() - 14.0f, w.rect.h},
               waveName(static_cast<OpWave>(w.b)), kTextBody, hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::InputsScroll: {
        const bool lit = hovered || active;
        c.fillRect(inputsScrollTrack(), kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(inputsThumb(controller().layout().selectedNode), lit ? kDim : kFaint, kRadiusMark);
        break;
    }
    case Kind::FilterPoint: // a voice filter's; the filter effect's (b = 1) is the View's
        if (w.b == 1)
            return false;
        paintFilterHandle(c, w.rect.center(), hovered || active, kFilterColor, kFilterDisplay,
                          voiceFilterParams(w.a), [this](int index) { return value(index); });
        break;
    default:
        return false;
    }
    return true;
}

// The name tag of a hovered wave button, painted after every widget so the ones below don't cover it.
void InspectorPanel::paintOverWidgets(Canvas &c) {
    const std::optional<Widget> &w = host_.hovered();
    if (!w || (w->kind != Kind::WaveButton && w->kind != Kind::WaveMore) || waveMenu_ >= 0 || !host_.isHovered(*w) ||
        covered())
        return;
    std::vector<Widget> list; // still one of its widgets
    widgets(list);
    if (std::none_of(list.begin(), list.end(), [&](const Widget &own) { return own.same(*w); }))
        return;
    const auto picked = static_cast<OpWave>(std::lround(value(paramIndex(w->a, OpParam::Wave))));
    const char *name = w->kind == Kind::WaveButton ? waveName(static_cast<OpWave>(w->b))
                       : isMoreWave(picked)          ? waveName(picked)
                                                     : tr(Str::TipMoreWaves);
    const Rect tip{w->rect.center().x - 40.0f, w->rect.bottom() + 3.0f, 80.0f, 16.0f};
    paintMenuPanel(c, tip, kRadiusControl); // as a tooltip, which stands apart from a raised row too
    c.text(tip, name, kTextLabel, kText);
}

std::string InspectorPanel::tooltip(const Widget &w) const {
    if (std::string tip = sourceTip(w); !tip.empty()) // a Wavetable or Sample operator's (see opsource.cpp)
        return tip;
    const int param = w.param;
    if (w.kind == Kind::NodeTab || w.kind == Kind::MatrixLabel) {
        const bool on = routing_.nodeOn(w.a);
        const std::string title = nodeTitle(w.a);
        return controller().layout().selectedNode != w.a
                   ? trf(on ? Str::TipShowNode : Str::TipSwitchOnAndShow, {title})
                   : trf(on ? Str::TipSwitchNodeOff : Str::TipSwitchNodeOn, {title});
    }
    if (w.kind != Kind::Choice && w.kind != Kind::Toggle)
        return {};
    if (routeModeFrom(param) >= 0) { // an INPUTS row's buttons
        const Str tips[] = {Str::TipRoutePhase, Str::TipRouteLinearFm, Str::TipRouteExpFm, Str::TipRouteRing};
        static_assert(std::size(tips) == static_cast<size_t>(RouteMode::Count));
        return tr(tips[std::clamp(w.b, 0, static_cast<int>(RouteMode::Count) - 1)]);
    }
    if (isPowerParam(param))
        return tr(Str::TipNodePower);
    if (fixedSwitchOp(param) >= 0)
        return tr(value(param) >= 0.5 ? Str::TipFixedOn : Str::TipFixedOff);
    if (opOfField(param, OpParam::Sync) >= 0)
        return tr(Str::TipHardSync);
    if (opOfField(param, OpParam::Warp) >= 0) {
        const Str tips[] = {Str::TipWarpOff,   Str::TipWarpBend,  Str::TipWarpSqueeze,
                            Str::TipWarpPulse, Str::TipWarpSteps, Str::TipWarpSync};
        static_assert(std::size(tips) == static_cast<size_t>(OpWarp::Count));
        return tr(tips[std::clamp(w.b, 0, static_cast<int>(OpWarp::Count) - 1)]);
    }
    if (opOfField(param, OpParam::UnisonCurve) >= 0) {
        const Str tips[] = {Str::TipUniCurveLinear, Str::TipUniCurveCenter, Str::TipUniCurveSupersaw,
                            Str::TipUniCurveRandom};
        static_assert(std::size(tips) == static_cast<size_t>(UniCurve::Count));
        return tr(tips[std::clamp(w.b, 0, static_cast<int>(UniCurve::Count) - 1)]);
    }
    if (opOfField(param, OpParam::UnisonPan) >= 0) {
        const Str tips[] = {Str::TipUniPanRamp, Str::TipUniPanAlternate, Str::TipUniPanRandom};
        static_assert(std::size(tips) == static_cast<size_t>(UniPan::Count));
        return tr(tips[std::clamp(w.b, 0, static_cast<int>(UniPan::Count) - 1)]);
    }
    return {};
}

std::optional<CursorKind> InspectorPanel::cursor(const Widget &w) const {
    if (const std::optional<CursorKind> cursor = sourceCursor(w))
        return cursor;
    if (w.kind == Kind::WaveMenu) // the open list's own panel and what is around it, where a click picks nothing
        return CursorKind::Arrow;
    return std::nullopt;
}

// --- What moves.

// While notes play, the operator's envelope on its ENVELOPE tab and its scope; the scope's level, of the part of its
// span filled so far (none while it captures nothing); and the voice filters' spectra on the filter page, which go on
// falling away after the audio stops, until they are gone. Each only in the inspector.
void InspectorPanel::poll(const Telemetry &t, std::vector<Rect> &repaint) {
    const Telemetry &was = host_.telemetry();
    if (t.voices > 0 || was.voices > 0) {
        if (t.opLevels != was.opLevels || t.opEnvLevels != was.opEnvLevels || t.opEnvPos != was.opEnvPos ||
            t.opTablePos != was.opTablePos || t.opSamplePos != was.opSamplePos)
            repaint.push_back(kInspector);
        if (t.scope != was.scope || t.scopeBins != was.scopeBins || t.scopeCursor != was.scopeCursor)
            repaint.push_back(kInspector);
    }
    float opPeak = 0.0f;
    if (t.voices > 0 && t.scopeBins > 0) {
        const int bins = std::min(t.scopeBins, kScopeBins);
        const int filled =
            t.scopeCursor < 0.0f ? bins : std::clamp(static_cast<int>(t.scopeCursor * bins), 0, bins);
        for (int bin = 0; bin < filled; ++bin)
            opPeak = std::max(opPeak, std::abs(t.scope[bin]));
    } else {
        opScopeLevel_ = 0.0f;
    }
    if (followScopeLevel(opScopeLevel_, opPeak) && opScopeLevel_ > 1e-4f)
        repaint.push_back(kInspector);
    auto showing = [](const Spectrum &spectrum) {
        return std::any_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; });
    };
    if (t.age < 1.5f && std::any_of(t.filterSpectrum.begin(), t.filterSpectrum.end(), showing))
        repaint.push_back(kInspector);
}

// --- Input.

bool InspectorPanel::press(const Widget &w, Point p, Modifiers mods) {
    if (sourcePress(w, p, mods)) // a Wavetable or Sample operator's (see opsource.cpp)
        return true;
    UiLayout &layout = controller().layout();
    switch (w.kind) {
    case Kind::InputsScroll: { // as the MODS view's
        const int op = layout.selectedNode;
        const Rect track = inputsScrollTrack();
        const Rect thumb = inputsThumb(op);
        if (p.y < thumb.y || p.y >= thumb.bottom())
            inputsScroll_ = (p.y - thumb.h * 0.5f - track.y) / std::max(track.h - thumb.h, 1.0f) * inputsEnd(op);
        drag_ = {DragKind::InputsScroll, w, inputsFirst(op)};
        host_.startDrag(*this, w, p, false);
        break;
    }
    case Kind::NodeTab:
    case Kind::MatrixLabel:
        showNode(w.a);
        break;
    case Kind::OpTab:
        layout.opTab = static_cast<InspectorTab>(w.a);
        break;
    case Kind::ScopeCycles:
        layout.scopeCycles = w.b;
        break;
    case Kind::WaveButton:
        edit(paramIndex(w.a, OpParam::Wave), w.b);
        break;
    case Kind::WaveMore:
        waveMenu_ = waveMenu_ == w.a ? -1 : w.a;
        sourceMenu_ = -1;
        break;
    case Kind::WaveMenu:
        if (w.a == 0) // a click anywhere else closes the list
            waveMenu_ = -1;
        break;
    case Kind::WaveMenuItem: {
        waveMenu_ = -1;
        const UndoScope step(controller());
        const auto before = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
        edit(paramIndex(w.a, OpParam::Wave), w.b);
        if (w.b == static_cast<int>(OpWave::Custom)) { // a drawn wave opens its partials
            // The partials start as the wave it was, so it sounds the same until they are drawn; after
            // Noise, or Custom again, they stay as they were.
            std::array<double, 2 * kPartials> partials{};
            if (wavePartials(before, partials)) {
                std::vector<std::pair<int, double>> changes;
                for (int k = 0; k < kPartials; ++k) {
                    changes.push_back({partialLevelIndex(w.a, k), partials[2 * k]});
                    changes.push_back({partialPhaseIndex(w.a, k), partials[2 * k + 1]});
                }
                mods_.editMany(changes);
            }
            shapes_.openPartials(w.a);
        }
        break;
    }
    case Kind::FilterPoint:
        if (w.b == 1)
            return false; // the filter effect's, the View's
        drag_ = {DragKind::FilterPoint, w, 0.0f};
        for (int index : {voiceFilterParams(w.a).cutoff, voiceFilterParams(w.a).resonance})
            controller().beginEdit(index);
        host_.startDrag(*this, w, p, false);
        break;
    case Kind::Toggle: // the Fixed switch, with its Freq and Mult
        if (fixedSwitchOp(w.param) < 0)
            return false;
        switchFixed(fixedSwitchOp(w.param));
        break;
    default:
        return false;
    }
    return true;
}

void InspectorPanel::drag(const DragState &d) {
    const Point p = d.now;
    switch (drag_.kind) {
    case DragKind::FilterPoint: { // its cutoff across, its resonance up and down
        const auto [cutoff, resonance] = filterHandleValues(kFilterDisplay, p);
        auto set = [&](int index, double v) {
            v = std::clamp(v, table_[index].min, table_[index].max);
            if (v != value(index))
                controller().setValue(index, v);
        };
        const FilterParams f = voiceFilterParams(drag_.widget.a);
        set(f.cutoff, cutoff);
        set(f.resonance, resonance);
        break;
    }
    case DragKind::InputsScroll: { // the INPUTS tab's rows follow its thumb
        const int op = controller().layout().selectedNode;
        const float room = inputsScrollTrack().h - inputsThumb(op).h;
        if (room > 0.0f)
            inputsScroll_ =
                std::clamp(drag_.startValue + (p.y - d.start.y) / room * inputsEnd(op), 0.0f, inputsEnd(op));
        break;
    }
    case DragKind::SampleHandle:
        sampleDrag(d);
        break;
    case DragKind::TablePosition:
        tablePosDrag(d);
        break;
    case DragKind::None:
        break;
    }
}

void InspectorPanel::release() {
    if (drag_.kind == DragKind::FilterPoint) {
        const FilterParams f = voiceFilterParams(drag_.widget.a);
        for (int index : {f.cutoff, f.resonance})
            controller().endEdit(index);
    }
    if (drag_.kind == DragKind::SampleHandle || drag_.kind == DragKind::TablePosition)
        controller().endEdit(drag_.widget.param);
    drag_ = Drag{};
}

bool InspectorPanel::doubleClick(const Widget &w, Point, Modifiers) {
    return sourceDoubleClick(w);
}

bool InspectorPanel::doublePress(const Widget &w) const {
    return w.kind == Kind::TableStep; // quick clicks step on through the tables
}

bool InspectorPanel::dragsOnTouch(const Widget &w) const {
    return w.kind == Kind::InputsScroll;
}

// The Fixed switch as a click on it: on upward, off downward, with its Freq and Mult.
bool InspectorPanel::wheelOn(const Widget &w, Point, float steps, Modifiers) {
    if (w.kind != Kind::Toggle || fixedSwitchOp(w.param) < 0)
        return false;
    const int whole = host_.wheelNotches(w.param, steps);
    if (whole != 0 && (whole > 0) != (value(w.param) >= 0.5))
        switchFixed(fixedSwitchOp(w.param));
    return true;
}

// Over an operator's INPUTS rows (but not on a depth or a mode's button), a notch scrolls them a row.
bool InspectorPanel::wheelBehind(Point p, float steps) {
    if (!inputsScrollAt(p) || steps == 0.0f)
        return false;
    const int op = controller().layout().selectedNode;
    inputsScroll_ = std::clamp(inputsFirst(op) - steps, 0.0f, inputsEnd(op));
    return true;
}

// The list of more waves' and the source list's entries take no pan; around them, the pan closes them and goes on.
bool InspectorPanel::blocksPan(const Widget &w) const {
    return w.kind == Kind::WaveMenuItem || (w.kind == Kind::WaveMenu && w.a != 0) || w.kind == Kind::OpSourceItem ||
           (w.kind == Kind::OpSourceMenu && w.a != 0);
}

std::optional<std::pair<int, float>> InspectorPanel::panAt(Point p) const {
    if (!inputsScrollAt(p) || inputsEnd(controller().layout().selectedNode) <= 0.0f)
        return std::nullopt;
    return std::pair{0, inputsFirst(controller().layout().selectedNode)};
}

std::optional<float> InspectorPanel::pan(int, float first, float, float down) {
    const int op = controller().layout().selectedNode;
    const float wanted = first - down / kInputsStep;
    const float to = std::clamp(wanted, 0.0f, std::max(op >= 0 && op < kOps ? inputsEnd(op) : 0.0f, 0.0f));
    inputsScroll_ = to;
    return to != wanted ? std::optional(to) : std::nullopt;
}

bool InspectorPanel::inputsScrollAt(Point p) const {
    const int op = controller().layout().selectedNode;
    return op >= 0 && op < kOps && !covered() && opTabShown(op) == InspectorTab::Inputs && kInputsList.contains(p);
}

} // namespace fmvibe::gui
