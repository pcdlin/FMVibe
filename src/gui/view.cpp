#include "view.h"

#include "algorithms.h"
#include "filterdisplay.h"
#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "lists.h"
#include "routing.h"
#include "theme.h"
#include "../effects.h"
#include "../filter.h"
#include "../uistrings.h"
#include "../unison.h"
#include "../warp.h"
#include "../wavetable.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <numbers>
#include <numeric>
#include <string_view>

namespace fmvibe::gui {

// The look, the layout, the icons, the routing graph's model, the lists' entries and a filter's display are in
// theme.h, layout.h, icons.h, routing.h, lists.h and filterdisplay.h, in namespace detail. dialogs.cpp holds the View's
// methods for the dialogs; the drawn shapes' editors, the routing panel and the inspector are panels of their own
// (shapeeditor.h, routingpanel.h, inspectorpanel.h).
using namespace detail;

namespace detail {
// The effect cards' titles, by FxType from the shaper on.
constexpr Str kFxTitles[] = {Str::FxTitleShaper, Str::FxTitleDelay, Str::FxTitleDjEq, Str::FxTitleParamEq,
                             Str::FxTitleFilter, Str::FxTitleCompressor, Str::FxTitleMultiComp, Str::FxTitleReverb,
                             Str::FxTitlePhaser, Str::FxTitleFlanger, Str::FxTitleFreqShifter, Str::FxTitleDisperser,
                             Str::FxTitleWidener, Str::FxTitleMultiDyn, Str::FxTitleAmp, Str::FxTitleUtility,
                             Str::FxTitleChorus, Str::FxTitleLoFi};
static_assert(std::size(kFxTitles) == static_cast<size_t>(FxType::Count) - 1);
const char *fxTitle(FxType type) {
    return type == FxType::None ? "" : tr(kFxTitles[static_cast<int>(type) - 1]);
}
} // namespace detail

namespace {
constexpr Str kMcBandNames[kMcBands] = {Str::BandButtonLow, Str::BandButtonMid, Str::BandButtonHigh};
constexpr Str kViewNames[] = {Str::ViewRouting, Str::ViewMatrix, Str::ViewFx, Str::ViewMods, Str::ViewSeq};
static_assert(std::size(kViewNames) == std::size(kViewTabs));

// A tempo as the title bar shows it, and as typing it starts: whole, or with the hundredths it holds ("92.5").
std::string tempoText(double bpm) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.2f", clampTempo(bpm));
    std::string s = text;
    s.erase(s.find_last_not_of('0') + 1);
    if (s.back() == '.')
        s.pop_back();
    return s;
}

// A view tab's outline over `r`, from its bottom-left corner up around its rounded top and down to its bottom-right
// one. With a flare, its sides curve out into the edge at its bottom by that much on either side, as the tab shown
// joins its panel; a tab over the panel's left edge runs straight down that edge instead, past its rounded corner.
// To meet the panel's border, which strokeRect draws inside the panel, the shown tab's `r` is the line its border's
// middle runs along: half a pixel inside the tab, its bottom on the middle of the panel's top border.
std::vector<Point> viewTabOutline(Rect r, float flare) {
    constexpr int kSteps = 8; // points per quarter circle
    std::vector<Point> points;
    auto quarter = [&](Point center, float radius, float from, float to) {
        for (int i = 0; i <= kSteps; ++i) {
            const float angle = from + (to - from) * i / kSteps;
            points.push_back({center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)});
        }
    };
    const float bottom = r.bottom(), right = r.right(), radius = kViewTabRadius;
    if (flare > 0.0f && r.x < kFxPanel.x + 1.0f)
        points.push_back({r.x, bottom + kPanelRadius});
    else if (flare > 0.0f)
        quarter({r.x - flare, bottom - flare}, flare, 0.5f * kPi, 0.0f);
    else
        points.push_back({r.x, bottom});
    quarter({r.x + radius, r.y + radius}, radius, kPi, 1.5f * kPi);
    quarter({right - radius, r.y + radius}, radius, 1.5f * kPi, 2.0f * kPi);
    if (flare > 0.0f)
        quarter({right + flare, bottom - flare}, flare, kPi, 0.5f * kPi);
    else
        points.push_back({right, bottom});
    return points;
}

// The tooltip of a three-band dynamics' knob that needs one, saying which way its threshold or ratio works; Count for
// other knobs.
Str dynKnobTip(int param) {
    const int slot = fxSlotOf(param);
    if (slot < 0 || param < mdAboveIndex(slot, 0) || param > mdGainIndex(slot, kMcBands - 1))
        return fxFieldOf(param) == FxParam::MdRange ? Str::TipUpRange : Str::Count;
    switch ((param - mdAboveIndex(slot, 0)) % 5) {
    case 0:
        return Str::TipDownThreshold;
    case 1:
        return Str::TipDownRatio;
    case 2:
        return Str::TipUpThreshold;
    case 3:
        return Str::TipUpRatio;
    default:
        return Str::Count; // the band's Gain says what it is
    }
}

// `value` as `format` prints it, for the numbers a wording takes.
std::string number(const char *format, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

// The output meter's level on `side` (1 is full scale) and its clip light, as of now: the snapshot's, fallen on for
// as long as the snapshot has aged, as the audio thread would have let them fall had it gone on.
float meterPeak(const Telemetry &t, int side) {
    return t.outPeak[side] * static_cast<float>(std::pow(10.0, -kMeterFallDb * t.age / 20.0));
}
bool meterClipped(const Telemetry &t) {
    return t.clipHold > t.age;
}

bool isSourcePower(int param) {
    return sourceFieldOf(param) == SourceParam::Enabled;
}

// An LFO shape's icon, as its list shows it, in a box about 16 x 10.
void lfoShapeIcon(Canvas &c, Rect r, LfoShape shape, Color color) {
    const std::vector<Point> points = shapeIcon(shape, r);
    c.polyline(points.data(), points.size(), color, 1.5f);
    if (shape == LfoShape::Custom)
        customShapeDots(c, r, color);
}

const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int note) {
    return std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 1);
}

bool isLfoSync(int param) {
    return sourceFieldOf(param) == SourceParam::Sync;
}

bool isLfoPhase(int param) {
    return sourceFieldOf(param) == SourceParam::Phase;
}

bool isPartialCount(int param) {
    return param >= partialCountIndex(0) && param <= partialCountIndex(kOps - 1);
}

// --- Envelopes, for operators (0..kOps-1) and mod envelopes (kOps + source).

struct EnvIndices {
    int attack;
    int hold;
    int decay;
    int sustain;
    int release;
    int curves; // attack, decay and release curves follow each other from here

    int curve(int segment) const { return curves + segment; }
};

EnvIndices envIndices(int env) {
    if (env < kOps)
        return {paramIndex(env, OpParam::Attack), paramIndex(env, OpParam::Hold), paramIndex(env, OpParam::Decay),
                paramIndex(env, OpParam::Sustain), paramIndex(env, OpParam::Release),
                paramIndex(env, OpParam::AttackCurve)};
    const int s = env - kOps;
    return {paramIndex(s, SourceParam::Attack), paramIndex(s, SourceParam::Hold), paramIndex(s, SourceParam::Decay),
            paramIndex(s, SourceParam::Sustain), paramIndex(s, SourceParam::Release),
            paramIndex(s, SourceParam::AttackCurve)};
}
static_assert(paramIndex(0, OpParam::ReleaseCurve) == paramIndex(0, OpParam::AttackCurve) + 2);
static_assert(paramIndex(0, SourceParam::ReleaseCurve) == paramIndex(0, SourceParam::AttackCurve) + 2);

constexpr Str kEnvSegmentNames[] = {Str::LabelAttack, Str::LabelDecay, Str::LabelRelease};

// Attack, hold, decay and release each get kEnvMinSegment pixels, so short
// ones keep their handles apart, plus `scale` pixels per unit of their
// (log-scaled) time; the release starts where the decay ends, at the sustain
// level, since the sustain has no length of its own. The scale is picked to
// fill the display (see envFitScale()), so short envelopes are zoomed in.
// Below 1 ms (an envelope time under 0, see kMinEnvNorm) a segment's own pixels shrink: an attack's all the way, so
// a 0 ms one goes straight up, a decay's and a release's to half, keeping their handles apart.
constexpr float kEnvMinSegment = 10.0f;

// What's left at 0 ms of a segment's kEnvMinSegment pixels.
float envLeastShare(bool attack) {
    return attack ? 0.0f : 0.5f;
}

// The pixels a segment's time `value` of param `param` takes.
float envSpan(int param, double value, float scale, bool attack = false) {
    const double min = paramTable()[param].min;
    if (value >= 0.0 || min >= 0.0)
        return kEnvMinSegment + static_cast<float>(std::max(value, 0.0)) * scale;
    const float share = envLeastShare(attack);
    return kEnvMinSegment * (share + (1.0f - share) * static_cast<float>(1.0 - value / min));
}

// The time of param `param` whose segment takes `span` pixels: envSpan() the other way round.
double envSpanTime(int param, float span, float scale, bool attack = false) {
    const double min = paramTable()[param].min;
    if (span >= kEnvMinSegment || min >= 0.0) // a hold to the left of its shortest place: none
        return (span - kEnvMinSegment) / scale;
    const float share = envLeastShare(attack);
    return min * (1.0 - std::clamp((span / kEnvMinSegment - share) / (1.0f - share), 0.0f, 1.0f));
}

struct EnvShape {
    Point p0, p1, ph, p2, p4; // start, peak, end of the hold, sustain level (where the release starts), end
    float scale;
    float top;
    float bottom;
};

template <class Get>
EnvShape envShape(Rect r, const EnvIndices &e, const Get &get, float scale) {
    const float left = r.x + 10.0f;
    auto span = [&](int param, bool attack = false) { return envSpan(param, get(param), scale, attack); };
    EnvShape s;
    s.scale = scale;
    s.top = r.y + 12.0f;
    s.bottom = r.bottom() - 10.0f;
    s.p0 = {left, s.bottom};
    s.p1 = {left + span(e.attack, true), s.top};
    s.ph = {s.p1.x + span(e.hold), s.top};
    s.p2 = {s.ph.x + span(e.decay), s.bottom - static_cast<float>(get(e.sustain)) * (s.bottom - s.top)};
    s.p4 = {s.p2.x + span(e.release), s.bottom};
    return s;
}

// The scale at which an envelope fills a display `r` wide. Below a combined
// 0.15 of normalized time (all four under about 4 ms), it stops zooming.
template <class Get>
float envFitScale(Rect r, const EnvIndices &e, const Get &get) {
    const float room = r.w - 20.0f - 4.0f * kEnvMinSegment;
    double total = 0.0;
    for (const int param : {e.attack, e.hold, e.decay, e.release})
        total += std::max(get(param), 0.0); // times under 1 ms take only their share of kEnvMinSegment
    return room / static_cast<float>(std::max(total, 0.15));
}

// The furthest an envelope in a display `r` wide zooms out: as far as fits its four times at their longest.
float envLeastScale(Rect r, const EnvIndices &e) {
    const float room = r.w - 20.0f - 4.0f * kEnvMinSegment;
    double total = 0.0;
    for (const int param : {e.attack, e.hold, e.decay, e.release})
        total += std::max(paramTable()[param].max, 0.0);
    return room / static_cast<float>(total);
}

// How far right handle `point` (0 the attack's peak, 1 the hold's end, 2 the decay's end, 3 the release's end) of
// envelope `s` in display `r` goes, the segments after it keeping their lengths, before the envelope's end runs past
// the display's.
float envPointLimit(Rect r, const EnvShape &s, int point) {
    const Point points[] = {s.p1, s.ph, s.p2, s.p4};
    return r.right() - 10.0f - (s.p4.x - points[point].x);
}

// A point on segment 0 (attack, p0 to p1), 1 (decay, ph to p2) or 2 (release,
// p2 to p4), at time t (0..1) along it, bent by its curve as the DSP bends it.
Point envSegmentPoint(const EnvShape &s, int segment, double t, double curve, double sustain) {
    const double f = envCurve(t, curve);
    const double level = segment == 0 ? f : segment == 1 ? sustain + (1.0 - sustain) * (1.0 - f) : sustain * (1.0 - f);
    const Point from = segment == 0 ? s.p0 : segment == 1 ? s.ph : s.p2;
    const Point to = segment == 0 ? s.p1 : segment == 1 ? s.p2 : s.p4;
    return {from.x + static_cast<float>(t) * (to.x - from.x), s.bottom - static_cast<float>(level) * (s.bottom - s.top)};
}

} // namespace

float View::heightFor(const UiLayout &layout) {
    return keyboardPanel(keyboardTop(), layout.keysCollapsed).bottom() + 16.0f;
}

View::View(Controller &controller)
    : controller_(controller), mods_(controller, [this](std::string m) { flash(std::move(m)); }),
      routing_(controller, [this](std::string m) { flash(std::move(m)); }),
      fx_(controller, [this](std::string m) { flash(std::move(m)); }), shapes_(*this), presets_(*this), seq_(*this),
      modsPanel_(*this), fxPanel_(*this), routingPanel_(*this), inspector_(*this, shapes_),
      panels_{&shapes_, &presets_, &seq_, &modsPanel_, &fxPanel_, &routingPanel_, &inspector_},
      table_(paramTable()) {
    poll();
}

int View::themeCount() {
    return kThemeCount;
}

const char *View::themeName(int index) {
    return kThemes[std::clamp(index, 0, kThemeCount - 1)].name;
}

View::~View() {
    finishDrag();
}

Panel *View::modalPanel() const {
    for (Panel *panel : panels_)
        if (panel->modal())
            return panel;
    return nullptr;
}

void View::paintCardLanding(Canvas &c, Rect landing, Rect row, float radius, Color color) {
    c.pushClip(row);
    c.fillRect(landing.inset(-1.0f), kPanel, radius + 1.0f);
    c.strokeRect(landing, color.alpha(0.6f), 1.0f, radius);
    c.popClip();
}

void View::paintCarriedCard(Canvas &c, Rect landing, Rect start, Rect row, float radius, Color color,
                            const std::function<void()> &paintCard, const std::vector<Widget> &widgets) {
    paintCardLanding(c, landing, row, radius, color);
    const Point offset{start.x + drag_.now.x - drag_.start.x - landing.x,
                       start.y + drag_.now.y - drag_.start.y - landing.y};
    c.pushLayer(offset, 0.5f, landing.inset(-1.0f));
    c.pushClip(landing);
    paintCard();
    for (const Widget &w : widgets)
        if (!w.popup && w.kind != Kind::FxShadow && w.rect.x < landing.right() && w.rect.right() > landing.x &&
            w.rect.y < landing.bottom() && w.rect.bottom() > landing.y)
            paintWidget(c, w);
    c.popClip();
    c.popLayer();
}

std::string View::panelTip(const Widget &w) const {
    for (const Panel *panel : panels_)
        if (std::string tip = panel->tooltip(w); !tip.empty())
            return tip;
    return {};
}

void View::askConfirm(Panel &from, int kind) {
    confirm_ = kind;
    confirmFrom_ = &from;
    hover_.reset();
}

// A drag going on still (one whose button-up never came: a double-click starting one, say) ends first, its gesture and
// undo step with it, as a press ends it (see mouseDown()).
void View::startDrag(Panel &from, const Widget &w, Point p, bool step) {
    if (drag_.kind != DragKind::None)
        finishDrag();
    drag_.kind = DragKind::Panel;
    drag_.widget = w;
    drag_.start = drag_.now = p;
    drag_.moved = false;
    drag_.step = step;
    drag_.panel = &from;
}

void View::dragValue(const Widget &w, Point p, Modifiers mods) {
    if (drag_.kind != DragKind::None) // as startDrag() does
        finishDrag();
    drag_.widget = w;
    drag_.start = drag_.now = p;
    drag_.fine = mods.shift;
    drag_.kind = DragKind::Value;
    drag_.startValue = value(w.param);
    controller_.beginEdit(w.param);
}

View::DerivedScope::DerivedScope(const View &view) : view_(view) {
    if (view_.derivedDepth_++ == 0) {
        view_.derived_ = Derived{};
        for (Panel *panel : view_.panels_) // and what the panels keep (see ScopeCache)
            panel->forgetScoped();
    }
}

View::DerivedScope::~DerivedScope() {
    if (--view_.derivedDepth_ == 0) {
        view_.derived_ = Derived{};
        for (Panel *panel : view_.panels_)
            panel->forgetScoped();
    }
}

View::ModTable View::modTable() const {
    ModTable table;
    table.all = assignments();
    table.byTarget.resize(table.all.size());
    std::iota(table.byTarget.begin(), table.byTarget.end(), 0);
    std::stable_sort(table.byTarget.begin(), table.byTarget.end(),
                     [&](int a, int b) { return table.all[a].target < table.all[b].target; });
    return table;
}

template <class F>
void View::forEachMod(int target, F &&f) const {
    std::optional<ModTable> fresh;
    const ModTable *table = nullptr;
    if (derivedDepth_ > 0) {
        if (!derived_.mods)
            derived_.mods = modTable();
        table = &*derived_.mods;
    } else {
        table = &fresh.emplace(modTable());
    }
    auto at = std::lower_bound(table->byTarget.begin(), table->byTarget.end(), target,
                               [&](int index, int t) { return table->all[index].target < t; });
    for (; at != table->byTarget.end() && table->all[*at].target == target; ++at)
        f(table->all[*at]);
}

// The scope drops them when it ends.
const std::vector<View::Widget> &View::scopedWidgets() const {
    if (!derived_.widgets)
        derived_.widgets = widgets();
    return *derived_.widgets;
}

// --- Model access.

std::vector<View::Assignment> View::assignments() const {
    return mods_.assignments(sources());
}

std::vector<int> View::usedSlots(int source) const {
    const bool keeps = source >= 0 && source < kAllSources;
    if (keeps && derived_.slots[source]) // only kept while a scope goes on
        return *derived_.slots[source];
    std::vector<int> slots = mods_.usedSlots(source);
    if (keeps && derivedDepth_ > 0)
        derived_.slots[source] = slots;
    return slots;
}

bool View::sourceUsed(int source) const {
    if (!usedSlots(source).empty())
        return true;
    const std::vector<Assignment> all = assignments();
    return std::any_of(all.begin(), all.end(), [&](const Assignment &m) { return m.aux == source; });
}

bool View::droppable(const Widget &w, int source) const {
    // A list's row takes it as an aux instead; a disabled widget, which the mouse passes by, takes nothing.
    return w.param >= 0 && mods_.canModulate(source, w.param) && !w.popup && !w.disabled &&
           (w.kind == Kind::Knob || w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::GridCell ||
            w.kind == Kind::SlotBar);
}

Color View::fxColor(int slot) const {
    return fxTypeColor(fx_.slotType(slot));
}

Color View::listColor(int param) const {
    if (isSeqParam(param))
        return kAccent;
    if (routeModeFrom(param) >= 0)
        return routeColor(routeModeFrom(param), routeModeTo(param));
    if (isLfoShape(param) || isMidiInput(param))
        return kSourceColors[sourceOf(param)];
    if (isSplitSlope(param))
        return splitColor();
    return shaperOf(param) >= 0 ? kShaperColor : fxSlotOf(param) >= 0 ? fxColor(fxSlotOf(param)) : kFilterColor;
}

int View::listShown(int param) const {
    if (isSplitSlope(param)) { // 18 plays as 24; three bands part at 12 or 24, 6 playing as 12
        const int slope = filterSlope(value(param));
        return slope == 2 ? kFilterSlopes - 1 : slope == 0 && fx_.splitThree(splitOf(param)) ? 1 : slope;
    }
    if (isFilterSlope(param))
        return playedSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), filterSlope(value(param)));
    return static_cast<int>(std::lround(value(param)));
}

bool View::listItemFits(int param, int item) const {
    if (isSplitSlope(param)) // 6, 12 or 24 dB; three bands 12 or 24
        return item != 2 && !(item == 0 && fx_.splitThree(splitOf(param)));
    return !isFilterSlope(param) || slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), item);
}

void View::stepList(int param, int dir) {
    // From the entry shown (from the list's end, for a value it doesn't offer) to the next one it can pick that way,
    // round its ends, as a pick from the list would: one undo step, the list closed.
    const int items = listCount(param);
    const int shown = listShown(param);
    int at = dir > 0 ? -1 : items;
    for (int item = 0; item < items; ++item)
        if (listItemValue(param, item) == shown)
            at = item;
    listMenu_ = -1;
    for (int k = 1; k <= items; ++k) {
        const int item = ((at + dir * k) % items + items) % items;
        const int to = listItemValue(param, item);
        if (!listItemFits(param, to))
            continue;
        if (to != shown)
            edit(param, to);
        // An LFO holding the retired Custom shape (a state can) goes back to its size as it takes another, as a pick does.
        if (isLfoShape(param))
            shapes_.narrow(sourceOf(param));
        return;
    }
}

// A switch flips; the symmetric switches with what goes along, as clicked or scrolled (the Fixed switch, with its Freq
// and Mult, is the inspector's).
void View::flipSwitch(int param) {
    if (fxFieldOf(param) == FxParam::ShaperSymmetric || shaperFieldOf(param) == ShaperParam::Symmetric)
        shapes_.switchSymmetric(param);
    else
        edit(param, value(param) >= 0.5 ? 0.0 : 1.0);
}

bool View::inspectorCovered() const {
    return inspector_.covered();
}

bool View::sourcesHidden() const {
    return seq_.wide() && seq_.full();
}

Rect View::panelsBand() const {
    return sourcesHidden() ? kSeqBand : kPanelsBand;
}

void View::showView(RoutingView view) {
    controller_.layout().view = view;
    for (Panel *panel : panels_) // their menus (the MODS view's), opened on what showed before
        panel->closePopups();
    shapes_.close();
    listMenu_ = -1;
}

double View::steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<Widget> View::dwellTarget(const std::optional<Widget> &at) const {
    if (!at)
        return std::nullopt;
    const RoutingView view = controller_.layout().view;
    switch (at->kind) {
    case Kind::ViewTab:
    case Kind::OpTab: // the operator page's
        return at;
    case Kind::NodeTab: // the inspector's tab of a node
        return Widget{Kind::Node, {}, -1, at->a};
    case Kind::Node:
    case Kind::Port: // a node's edge, its node
        if (view != RoutingView::Graph)
            return std::nullopt;
        return Widget{Kind::Node, {}, -1, at->a};
    case Kind::MatrixLabel: // a row's or column's name in the matrix, not one of the INPUTS tab's rows
        if (view != RoutingView::Matrix || kInspector.contains(at->rect.center()))
            return std::nullopt;
        return Widget{Kind::Node, {}, -1, at->a};
    default:
        return std::nullopt;
    }
}

bool View::dwellStep() {
    if (drag_.kind != DragKind::ModSource || !dwell_.over || dwell_.done || clock_() - dwell_.since < kDwellSeconds)
        return false;
    dwell_.done = true;
    const Widget &w = *dwell_.over;
    UiLayout &layout = controller_.layout();
    if (w.kind == Kind::ViewTab) {
        if (layout.view == static_cast<RoutingView>(w.a))
            return false;
        modList_.reset(); // a pill's, of the view it leaves
        showView(static_cast<RoutingView>(w.a));
        return true;
    }
    if (w.kind == Kind::OpTab) {
        if (layout.opTab == static_cast<InspectorTab>(w.a))
            return false;
        layout.opTab = static_cast<InspectorTab>(w.a);
        return true;
    }
    if (layout.selectedNode == w.a)
        return false;
    layout.selectedNode = w.a; // shown, as it is, on or off
    shapes_.closePartialsBut(w.a);
    return true;
}

void View::showSourceMods(int source) {
    showView(RoutingView::Mods);
    modsPanel_.showSource(source);
}

bool View::fxWide() const {
    return controller_.layout().view == RoutingView::Fx && shapeEditing() < 0;
}

std::vector<int> View::fxChain() const {
    if (derived_.chain) // only kept while a scope goes on
        return *derived_.chain;
    std::vector<int> chain = fx_.chain();
    if (derivedDepth_ > 0)
        derived_.chain = chain;
    return chain;
}

bool View::fxFolded(int slot) const {
    return mods_.folded(true, slot);
}

bool View::sourceFolded(int source) const {
    return !isStripSource(source) && mods_.folded(false, source);
}

float View::sourceSpan(int source) const {
    return sourceFolded(source) ? kFoldedSource : source == wideLfo() ? kWideSource : 1.0f;
}

float View::sourceOffset(const std::vector<int> &shown, size_t index) const {
    float offset = 0.0f;
    for (size_t i = 0; i < index && i < shown.size(); ++i)
        offset += sourceSpan(shown[i]);
    return offset;
}

// Folding a card or unfolding it; a widened LFO card folded unfolds at its normal size.
void View::setFolded(bool fx, int index, bool folded) {
    if (!fx && folded)
        shapes_.narrow(index);
    mods_.setFolded(fx, index, folded);
}

float View::scrollLength(ScrollArea area) const {
    if (area == ScrollArea::FxGraph)
        return fxPanel_.graphLength();
    if (area == ScrollArea::Fx)
        return fxPanel_.rowLength();
    std::optional<float> &kept = derived_.sourcesLength;
    if (kept) // only kept while a scope goes on
        return *kept;
    const std::vector<int> shown = sources();
    const float length = sourceOffset(shown, shown.size()) + (sourceShadow() ? kSourceShadowSpan : 0.0f);
    if (derivedDepth_ > 0)
        kept = length;
    return length;
}

bool View::sourceMenuShown() const {
    if (sourcesHidden()) // the SEQ view over the panel
        return false;
    if (sourceMenuAt_.x < 0.0f) // under + SOURCE, while there is a free place
        return sourceShadow();
    return sourceShadowCard().has_value();
}

Point View::sourceMenuTopLeft() const {
    if (sourceMenuAt_.x < 0.0f)
        return {sourceMenuButton().x, sourceMenuButton().bottom() + 4.0f};
    return sourceMenuAt_;
}

bool View::sourceShadow() const {
    return sources().size() < static_cast<size_t>(kMaxSources);
}

std::optional<Rect> View::sourceShadowCard() const {
    const std::vector<int> shown = sourcesShown();
    const float place = sourceOffset(shown, shown.size()) - sourceScroll();
    if (!sourceShadow() || sourcesHidden() || !onScreen(place, kVisibleCards, kSourceShadowSpan))
        return std::nullopt;
    return sourceCard(place, kSourceShadowSpan);
}

Point View::menuAtMouse(Point mouse, Rect menu) const {
    return menuAt(mouse, menu, height());
}

float View::scrollShown(ScrollArea area) const {
    return area == ScrollArea::Fx        ? static_cast<float>(kVisibleFx)
           : area == ScrollArea::Sources ? static_cast<float>(kVisibleCards)
                                         : fxPanel_.graphShown();
}

float View::scrollFirst(ScrollArea area) const {
    return area == ScrollArea::Fx        ? fxPanel_.rowScroll()
           : area == ScrollArea::Sources ? sourceScroll()
                                         : fxPanel_.graphScroll();
}

void View::setScroll(ScrollArea area, float first) {
    // The getters clamp the top end.
    if (area == ScrollArea::Fx)
        fxPanel_.setRowScroll(first);
    else if (area == ScrollArea::FxGraph)
        fxPanel_.setGraphScroll(first);
    else
        sourceScroll_ = std::max(first, 0.0f);
}

Rect View::scrollThumb(ScrollArea area) const {
    const Rect track = area == ScrollArea::Fx        ? fxScrollTrack()
                       : area == ScrollArea::Sources ? sourceScrollTrack()
                                                     : fxPanel_.graphScrollTrack();
    const float cards = std::max(scrollLength(area), 1.0f);
    const float shown = scrollShown(area);
    const float width = std::max(track.w * std::min(shown, cards) / cards, 24.0f);
    const float hidden = cards - shown; // less than a card when only the dashed cards at the end are out of view
    return {track.x + (hidden > 0.0f ? (track.w - width) * scrollFirst(area) / hidden : 0.0f), track.y, width,
            track.h};
}

Rect View::fxFilterArea(int slot) const {
    return fxDisplayIn(fxCardOf(slot).value_or(Rect{}), FxType::Filter);
}

Rect View::peqArea(int slot) const {
    return fxDisplayIn(fxCardOf(slot).value_or(Rect{}), FxType::ParamEq).inset(8.0f);
}

Rect View::mdDisplay(int slot) const {
    return fxDisplayIn(fxCardOf(slot).value_or(Rect{}), FxType::MultiDyn);
}

int View::mdThreshold(int slot, int line) const {
    return line % 2 == 1 ? mdBelowIndex(slot, line / 2) : mdAboveIndex(slot, line / 2);
}

bool View::edgeScroll() {
    // A handle being dragged onto a target scrolls whichever row it is over; a card being moved, its own row: the
    // source cards' here, the panels' (an effect card in the FX view's row) in their own steps.
    const bool handle = drag_.kind == DragKind::ModSource;
    const bool card = drag_.kind == DragKind::SourceMove && drag_.moved;
    if (!handle && !card)
        return false;
    const Point p = drag_.now;
    if (!sourcesHidden()) {
        const float speed = edgePull(kModPanel, p, card ? std::optional(drag_.start.x) : std::nullopt);
        if (speed != 0.0f) {
            const float before = scrollFirst(ScrollArea::Sources);
            setScroll(ScrollArea::Sources, before + edgeScrollStep(speed));
            return scrollFirst(ScrollArea::Sources) != before;
        }
    }
    if (handle)
        for (Panel *panel : panels_)
            if (panel->scrollToward(p))
                return true;
    return false;
}

void View::flash(std::string message) {
    flash_ = std::move(message);
    flashTicks_ = 160; // ~2.5 s of polls
}

int View::keyboardBase() const {
    return kBaseNote + 12 * octave_;
}

int View::keyVelocity(const Rect &key, Point p) {
    const float along = key.h > 0.0f ? (p.y - key.y) / key.h : 1.0f;
    // At least 1, so a key struck at its very top still sounds where velocity sets the level.
    return std::max(1, static_cast<int>(std::lround(std::clamp(along, 0.0f, 1.0f) * 127.0f)));
}

std::vector<int> View::sources() const {
    if (derived_.sources) // only kept while a scope goes on
        return *derived_.sources;
    std::vector<int> shown = mods_.sources();
    if (derivedDepth_ > 0)
        derived_.sources = shown;
    return shown;
}

std::vector<int> View::sourcesShown() const {
    if (derived_.sourcesShown) // only kept while a scope goes on
        return *derived_.sourcesShown;
    std::vector<int> shown = sourcesMoved();
    if (derivedDepth_ > 0)
        derived_.sourcesShown = shown;
    return shown;
}

std::vector<int> View::sourcesMoved() const {
    std::vector<int> shown = sources();
    if (drag_.kind != DragKind::SourceMove || !drag_.moved) // a click on a card's title moves nothing
        return shown;
    // While the mouse is still across from the card's own place (above or below it too: the card moves freely), nothing
    // moves. Once it is clear of it, the dragged card lands after the other cards whose middles are left of the mouse,
    // among those on screen, whatever the row scrolls to.
    const int source = drag_.widget.a;
    const Rect own = sourceCard(sourceOffset(shown, drag_.widget.b) - sourceScroll(), sourceSpan(source));
    if (drag_.now.x >= own.x && drag_.now.x < own.right())
        return shown;
    const float middle = (drag_.now.x - sourceCard(0.0f).x) / (kCardW + kCardGap) + sourceScroll();
    int first = 0, last = 0; // the cards that start on screen
    for (size_t i = 0; i < shown.size(); ++i) {
        const float place = sourceOffset(shown, i) - sourceScroll();
        if (place < -0.01f)
            first = static_cast<int>(i) + 1;
        if (place + sourceSpan(shown[i]) < kVisibleCards + 0.01f)
            last = static_cast<int>(i);
    }
    shown.erase(std::remove(shown.begin(), shown.end(), source), shown.end());
    int to = 0;
    float offset = 0.0f;
    for (int other : shown) {
        to += offset + 0.5f * sourceSpan(other) < middle;
        offset += sourceSpan(other);
    }
    to = std::clamp(to, first, std::max(last, first));
    shown.insert(shown.begin() + std::min(to, static_cast<int>(shown.size())), source);
    return shown;
}

bool View::modFocused(const Assignment &m) const {
    const Widget *w = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (!w)
        return false;
    // Its depth handle, or its row in a pill's list.
    if ((w->kind == Kind::ModDot || (w->popup && w->kind != Kind::ModList)) && w->a == m.source && w->b == m.slot)
        return true;
    // A hovered pill: the modulations it holds.
    if (w->kind == Kind::ModPill && drag_.kind == DragKind::None && w->a == m.target) {
        int index = 0, at = -1;
        forEachMod(m.target, [&](const Assignment &o) {
            if (o.source == m.source && o.slot == m.slot)
                at = index;
            ++index;
        });
        return at >= w->b;
    }
    return false;
}

bool View::sourceLive(int source) const {
    return mods_.sourceOn(source) && sourceUsed(source);
}

float View::sourceScroll() const {
    return std::clamp(sourceScroll_, 0.0f, std::max(scrollLength(ScrollArea::Sources) - kVisibleCards, 0.0f));
}

float View::cardOf(int source) const {
    if (source >= 0 && source < kMaxSources && derived_.cards[source]) // only kept while a scope goes on
        return *derived_.cards[source];
    float card = kOffScreen;
    if (!sourcesHidden()) {
        const std::vector<int> shown = sourcesShown();
        const auto at = std::find(shown.begin(), shown.end(), source);
        const float place = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
        card = at != shown.end() && onScreen(place, kVisibleCards, sourceSpan(source)) ? place : kOffScreen;
    }
    if (derivedDepth_ > 0 && source >= 0 && source < kMaxSources)
        derived_.cards[source] = card;
    return card;
}

Rect View::shapeHome(int shape) const {
    if (isClipCurve(shape)) // in its card
        return fxDisplayIn(fxCardOf(shape - kMaxSources).value_or(Rect{}), FxType::Shaper).inset(8.0f);
    if (isShaperCurve(shape)) // on its page in the inspector
        return kShaperDisplay.inset(8.0f);
    return lfoCardPlot(cardOf(shape)); // an LFO's, in its widened card
}

void View::revealSource(int source) {
    const std::vector<int> shown = sources();
    const float place = sourceOffset(shown, static_cast<size_t>(std::find(shown.begin(), shown.end(), source) - shown.begin()));
    const float span = sourceSpan(source);
    const float first = sourceScroll();
    sourceScroll_ = place < first ? place : (place + span > first + kVisibleCards ? place + span - kVisibleCards : first);
}

int View::addSource(SourceType type, int copyOf) {
    const int source = mods_.addSource(type, copyOf);
    if (source < 0)
        return -1;
    shapes_.shapeGone(source); // left open for a source the place held before
    if (type == SourceType::Curve && copyOf < 0) // widened, to draw its shape right there, as picking Custom did
        shapes_.widen(source);
    revealSource(source);
    return source;
}

void View::removeSource(int source) {
    mods_.removeSource(source);
    shapes_.shapeGone(source);
}

// Snapshots come once per audio block, which can be tens of milliseconds apart;
// in between, the phase moves on at the LFO's rate, so the dot glides. A host
// that stopped processing doesn't send it far. A one-shot stops at the end of its cycle.
double View::lfoPhaseNow(int lfo) const {
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    const double phase = telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age;
    if (lfoOneShot([this](int i) { return value(i); }, lfo))
        return std::min(phase, 1.0);
    return phase - std::floor(phase);
}

LfoClock View::lfoClockOf(int lfo) const {
    return lfoClock([this](int i) { return value(i); }, lfo);
}

int View::lfoClockCount(int lfo) const {
    return mods_.lfoShapeOf(lfo) == LfoShape::Random ? kRandomClocks : kLfoModes;
}

// Free, Anchor and One-Shot set for `clock` alone (none for Retrigger), in one undo step.
void View::setLfoClock(int lfo, LfoClock clock) {
    const std::pair<SourceParam, LfoClock> modes[] = {{SourceParam::Free, LfoClock::Free},
                                                      {SourceParam::Anchor, LfoClock::Anchor},
                                                      {SourceParam::OneShot, LfoClock::OneShot}};
    std::vector<std::pair<int, double>> changes;
    for (const auto &[field, mode] : modes) {
        const double to = mode == clock ? 1.0 : 0.0;
        if (value(paramIndex(lfo, field)) != to)
            changes.push_back({paramIndex(lfo, field), to});
    }
    if (!changes.empty())
        editMany(changes);
}

bool View::lfoRunsIdle(int lfo) const {
    return runsLfo(mods_.sourceTypeOf(lfo)) && mods_.sourceOn(lfo) &&
           lfoShared([this](int i) { return value(i); }, lfo);
}

bool View::lfoRunning(int lfo) const {
    return telemetry_.voices > 0 ? mods_.sourceOn(lfo) : lfoRunsIdle(lfo) || lfoInTail(telemetry_, lfo);
}

std::optional<std::pair<double, double>> View::lfoNow(int lfo) const {
    if (!lfoRunning(lfo))
        return std::nullopt;
    return std::pair{lfoPhaseNow(lfo), lfoValueNow(lfo)};
}

bool View::lfoInTail(const Telemetry &t, int lfo) {
    return t.fxTail && lfo >= 0 && lfo < kMaxSources && (t.fxTailLfos & (1u << lfo)) != 0;
}

bool View::lfoPhaseUsed(int lfo) const {
    // An anchored LFO stood at its Phase at the song's start.
    auto get = [this](int i) { return value(i); };
    return (!lfoShared(get, lfo) || lfoAnchored(get, lfo)) && mods_.lfoShapeOf(lfo) != LfoShape::Random;
}

double View::lfoValueNow(int lfo) const {
    const LfoShape shape = mods_.lfoShapeOf(lfo);
    if (shape == LfoShape::Random) { // can't be told ahead
        if (telemetry_.voices == 0) // the shared clock's glide (or the value it holds), which no voice puts out
            return lfoHeld([this](int i) { return value(i); }, lfo)
                       ? telemetry_.lfoTo[lfo]
                       : randomGlide(telemetry_.lfoFrom[lfo], telemetry_.lfoTo[lfo], randomPhaseNow(lfo),
                                     value(paramIndex(lfo, SourceParam::RandomSmooth)));
        const double out = telemetry_.sources[lfo];
        return mods_.sourceBipolar(lfo) ? out : out * 2.0 - 1.0;
    }
    if (shape == LfoShape::Custom)
        return shapeValue([this](int index) { return value(index); }, lfo, lfoPhaseNow(lfo));
    return lfoValue(shape, lfoPhaseNow(lfo), 0.0);
}

void View::traceRandomLfos() {
    // A new cycle is a new glide; one that stops running, or isn't random any more, starts over.
    for (int s = 0; s < kMaxSources; ++s) {
        RandomTrace &trace = randomTraces_[s];
        // Standing still, it has no cycles to trace: its display shows the value it holds.
        const bool random = runsLfo(mods_.sourceTypeOf(s)) && mods_.lfoShapeOf(s) == LfoShape::Random &&
                            !lfoHeld([this](int i) { return value(i); }, s);
        if (!random || !lfoRunning(s)) {
            trace.count = 0;
            continue;
        }
        const std::pair glide{telemetry_.lfoFrom[s], telemetry_.lfoTo[s]};
        const float phase = telemetry_.lfoPhases[s];
        if (trace.count > 0 && trace.glides[(trace.count - 1) % kRandomGlides] == glide) {
            trace.ended = trace.ended || phase < trace.phase;
            trace.phase = phase;
            continue;
        }
        trace.glides[trace.count % kRandomGlides] = glide;
        ++trace.count;
        trace.phase = phase;
        trace.ended = false;
    }
}

bool View::randomSweep(int lfo) const {
    return lfoRunning(lfo) && randomTraces_[lfo].count > 0;
}

double View::randomPhaseNow(int lfo) const {
    if (randomTraces_[lfo].ended)
        return 1.0;
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    return std::min(telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age, 1.0);
}

bool View::poll() {
    bool changed = false;
    bool bendMoved = false; // the Pitch Bend param alone, which the bend wheel shows
    // What changed and where it shows: the whole editor for anything that may show anywhere, or the union of the
    // places of what keeps to its own (see changedAreas()).
    bool everywhere = false;
    std::vector<Rect> areas;
    auto in = [&](Rect r) {
        for (const Rect &a : areas)
            if (a.x == r.x && a.y == r.y && a.w == r.w && a.h == r.h)
                return;
        areas.push_back(r);
    };
    // Every value looked at again only when one may have changed since (see Controller::valuesStamp()).
    if (const uint64_t stamp = controller_.valuesStamp(); stamp == 0 || stamp != seenStamp_) {
        seenStamp_ = stamp;
        for (int i = 0; i < kParamCount; ++i) {
            const double v = controller_.value(i);
            if (v != seen_[i]) {
                seen_[i] = v;
                // The Pitch Bend param shows on the bend wheel alone (a host automating it moves only that).
                if (i == paramIndex(GlobalParam::PitchBend))
                    bendMoved = true;
                else
                    changed = true;
            }
        }
    }
    if (changed)
        routing_.keepRoutes(); // whatever turned a route on, the editor or the host
    if (bendMoved)
        in(keyboardPanel(keyboardTop(), controller_.layout().keysCollapsed));
    closeGoneLists(); // the host may have changed what a list belongs to
    // The audio thread captures the operator whose scope shows, and nothing while none does.
    controller_.setScope(inspector_.scopeShown(), controller_.layout().scopeCycles);
    Telemetry t;
    controller_.telemetry(t);
    // Where the bend stands, and what the sequencer's curve adds to the mod wheel: the wheels.
    if (t.bend != telemetry_.bend || t.bendSet != telemetry_.bendSet || t.seqWheel != telemetry_.seqWheel)
        in(keyboardPanel(keyboardTop(), controller_.layout().keysCollapsed));
    // Where the sequencer's curves hold the macros' knobs: the knobs, and with no voice to tell where they take their
    // targets, whatever those are.
    for (int m = 0; m < kMacros; ++m)
        if (t.seqMacro[m] != telemetry_.seqMacro[m]) {
            in(keyboardPanel(keyboardTop(), controller_.layout().keysCollapsed));
            everywhere = everywhere || t.voices == 0;
        }
    everywhere = everywhere || changed; // a param: it may show anywhere
    const Rect header{0.0f, 0.0f, kWidth, kHeaderH};
    // The editor's bands, as paint() leaves out those an area doesn't reach: the panels' row (the routing panel or
    // the FX view, and the inspector, or the shape and partial editors over them; or the SEQ view, reaching down over
    // the source cards), and what lies under it (the source cards and the keyboard strip).
    const Rect panels = panelsBand();
    const Rect lower{0.0f, kRouting.bottom(), kWidth, wholeArea().bottom() - kRouting.bottom()};
    // A source's card, where its output, its LFO's dot, its envelope's and its own params' modulation show; or the
    // full-size shape editor, while it shows that source. A folded card or one scrolled away shows none of them.
    const std::vector<int> shown = sourcesShown();
    auto inCard = [&](int s) {
        if (shapeEditing() == s) {
            everywhere = true;
            return;
        }
        if (sourcesHidden() || sourceFolded(s))
            return;
        const auto at = std::find(shown.begin(), shown.end(), s);
        if (at == shown.end())
            return;
        const float card = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
        if (onScreen(card, kVisibleCards, sourceSpan(s)))
            in(sourceCard(card, sourceSpan(s)));
    };
    // Where modulation moving param `index` shows: its knobs and the displays drawn from it, with its depth handles,
    // in the band its section keeps to (a source's own params on its card), the MODS view's rows, and over anything
    // a pill's list may be open over.
    auto inTarget = [&](int index) {
        if (index < 0)
            return;
        if (modList_)
            everywhere = true;
        if (controller_.layout().view == RoutingView::Mods)
            in(panels);
        if (index < kOpBase) // the header's knobs; the mod wheel's, under the keys
            in(index == paramIndex(GlobalParam::ModWheel) ? lower : header);
        else if (isMacroKnob(index)) // a macro's, under the keys too
            in(lower);
        else if (sourceOf(index) >= 0)
            inCard(sourceOf(index));
        else // an operator's, the matrix's, a filter's, a shaper's: the routing panel and the inspector; an effect's
            in(panels);
    };
    // While notes play, or the last one's modulation runs on through the effects' tail, what they move, each where it
    // shows (above): the modulation, each slot's on its target too; the sources, each on its card (an LFO's dot glides
    // on from its snapshot by the snapshot's age, see lfoPhaseNow(), so it moves between snapshots alike); the playing
    // count in the header (the operators' envelopes and scope in the inspector it says of itself, below; the routing
    // graph's halos follow them below too). The note's pitch and velocity, which the filters',
    // the note source's and velocity's displays and the effects' key tracking follow, show anywhere, as does a voice
    // starting or the tail coming or going; they change seldom (a note, a glide, a bend).
    if ((t.voices > 0) != (telemetry_.voices > 0) || t.fxTail != telemetry_.fxTail)
        everywhere = true;
    else if (t.voices != telemetry_.voices)
        in(header);
    if (t.voices > 0 || telemetry_.voices > 0 || t.fxTail || telemetry_.fxTail) {
        if (t.notePitch != telemetry_.notePitch || t.noteVelocity != telemetry_.noteVelocity ||
            t.fxPitch != telemetry_.fxPitch)
            everywhere = true;
        for (int i = 0; i < kModulatedCount && !everywhere; ++i)
            if (t.modOffset[i] != telemetry_.modOffset[i])
                inTarget(i);
        for (int k = 0; k < kSeqFeelCount && !everywhere; ++k)
            if (t.seqFeel[k] != telemetry_.seqFeel[k])
                inTarget(seqFeelIndex(k));
        for (int m = 0; m < kMacros && !everywhere; ++m)
            if (t.macroOffset[m] != telemetry_.macroOffset[m])
                inTarget(paramIndex(m, MacroParam::Value));
        for (int k = 0; k < kAllSources * kSlots && !everywhere; ++k)
            if (t.slotOffset[k] != telemetry_.slotOffset[k])
                inTarget(slotTarget(value(slotTargetIndex(k / kSlots, k % kSlots))));
    }
    if (t.voices > 0 || telemetry_.voices > 0) {
        for (int s = 0; s < kMaxSources && !everywhere; ++s)
            if (t.sources[s] != telemetry_.sources[s] || t.lfoPhases[s] != telemetry_.lfoPhases[s] ||
                t.lfoRates[s] != telemetry_.lfoRates[s] || t.lfoFrom[s] != telemetry_.lfoFrom[s] ||
                t.lfoTo[s] != telemetry_.lfoTo[s] || t.envPos[s] != telemetry_.envPos[s] ||
                (t.voices > 0 && t.lfoRates[s] != 0.0f))
                inCard(s);
    }
    // A compressor's and an amp's meters move on while a tail drives them once the notes have ended, in the FX view;
    // the host's tempo shows in the synced effects' and LFOs' captions, and the title bar, whether notes play or not.
    if (t.fxReduction != telemetry_.fxReduction)
        in(panels);
    // What the panels show of it, each where it shows: the sequencer's playhead while the SEQ view shows, the notes it
    // plays glowing; the effects graph's glow; the inspector's operator envelope and scope, and its filter's spectrum.
    {
        std::vector<Rect> repaint;
        for (Panel *panel : panels_)
            panel->poll(t, repaint);
        for (const Rect &r : repaint)
            in(r);
    }
    if (t.tempo != telemetry_.tempo)
        everywhere = true;
    // The keyboard's keys ease up to lit and back down as they are held and let go (as the SEQ view's roll's),
    // clicked here or played by the host.
    {
        bool moved = false;
        for (int key = 0; key < 128; ++key) {
            const bool held = t.keyDown(key) || (drag_.kind == DragKind::Key && keyboardBase() + drag_.widget.a == key);
            float &light = keyLight_[static_cast<size_t>(key)];
            const float next = held ? std::min(light + kKeyLightRise, 1.0f) : std::max(light - kKeyLightFall, 0.0f);
            moved = moved || next != light;
            light = next;
        }
        if (t.keys != telemetry_.keys || moved) {
            in(keyboardPanel(keyboardTop(), controller_.layout().keysCollapsed));
        }
    }
    // The output meter falls while it shows anything, also once the audio has stopped and the snapshot ages, and
    // is painted once more as it goes.
    const float bottom = static_cast<float>(std::pow(10.0, kMeterFloorDb / 20.0));
    const bool metering = std::max(meterPeak(t, 0), meterPeak(t, 1)) > bottom || meterClipped(t);
    if (metering || metering_)
        in(header);
    metering_ = metering;
    // The output scope took another span, which draws or replaces one that drew; or the level it is drawn to moved.
    const bool outDrawn = outScopePeak(t) > kOutScopeFloor;
    if ((outDrawn || outScopePeak(telemetry_) > kOutScopeFloor) && t.outScope != telemetry_.outScope)
        in(header);
    if (followScopeLevel(outScopeLevel_, outScopePeak(t)) && outDrawn)
        in(header);
    // A three-band dynamics' bars move with its bands' levels and gains, until they settle: in the FX view, as the
    // effects' sweeps and spectra below.
    for (int slot : fxChain())
        if (fx_.slotType(slot) == FxType::MultiDyn)
            for (int band = 0; band < kMcBands; ++band)
                if (std::abs(t.fxBandLevel[slot][band] - telemetry_.fxBandLevel[slot][band]) >
                        0.01f * std::max(t.fxBandLevel[slot][band], 1e-5f) ||
                    std::abs(t.fxBandGain[slot][band] - telemetry_.fxBandGain[slot][band]) > 0.05f)
                    in(panels);
    // The routing graph's glow follows the output's level (see follow()), behind the routing graph and the effects
    // graph alike.
    if (routingPanel_.followOutput(t) && (routingPanel_.graphShown() || (fxWide() && fxGraph())))
        in(kFxPanel);
    // Phasers, flangers, choruses and wideners sweep while the audio runs.
    if (t.age < 0.5f)
        for (int slot : fxChain())
            if (fx_.slotType(slot) == FxType::Phaser || fx_.slotType(slot) == FxType::Flanger ||
                fx_.slotType(slot) == FxType::Chorus || fx_.slotType(slot) == FxType::Widener)
                in(panels);
    // An analyzer's spectrum goes on falling away after the audio stops, until it is gone; while notes play, the
    // filters' move along with them: the effects' in the FX view (the voice filters' on the inspector's filter page it
    // says of itself).
    auto showing = [](const Spectrum &spectrum) {
        return std::any_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; });
    };
    if (t.age < 1.5f && std::any_of(t.fxSpectrum.begin(), t.fxSpectrum.end(), showing))
        in(panels);
    // With no note playing, a free or anchored LFO's dot runs on along its card's display alone (or the full-size
    // shape editor showing it), gliding between snapshots while the audio runs; so does one the effects' tail runs.
    if (t.voices == 0 && telemetry_.voices == 0) {
        for (int position = 0; position < static_cast<int>(shown.size()); ++position) {
            const int s = shown[position];
            if (!(lfoRunsIdle(s) || lfoInTail(t, s) || lfoInTail(telemetry_, s)) || (t.lfoPhases[s] == telemetry_.lfoPhases[s] && (t.lfoRates[s] == 0.0f || t.age > 0.15f)))
                continue;
            if (shapeEditing() == s) {
                everywhere = true;
            } else if (!sourcesHidden() && !sourceFolded(s)) {
                const float card = sourceOffset(shown, position) - sourceScroll();
                if (onScreen(card, kVisibleCards, sourceSpan(s)))
                    in(sourceCard(card, sourceSpan(s)));
            }
        }
    }
    telemetry_ = t;
    // The routing graph's nodes' halos and its wires' swell follow what they show as the glow behind OUT does (see
    // RoutingPanel::followRoutes()); the panel is painted again while any moves visibly.
    if (routingPanel_.followRoutes() &&
        (routingPanel_.graphShown() || (fxWide() && fxGraph() && shownGlow() > 0.0f)))
        in(kFxPanel);
    traceRandomLfos();
    // A free or anchored LFO switched on runs whether notes play or not, and its card shows it: the plugin keeps
    // processing for it while a host would let it sleep.
    bool idleLfos = false;
    for (int s = 0; s < kMaxSources && !idleLfos; ++s)
        idleLfos = lfoRunsIdle(s);
    if (idleLfos != idleLfosShown_) {
        idleLfosShown_ = idleLfos;
        controller_.setIdleLfosShown(idleLfos);
    }
    if (flashTicks_ > 0 && --flashTicks_ == 0)
        everywhere = true;
    if (edgeScroll() || dwellStep())
        everywhere = true;
    // What moves by itself in the panels: the SEQ view's roll scrolling under a drag held at its edge, or fitted to a
    // patch loaded; the MODS view's rows gliding where the wheel sent them; the FX view's row under a card held at its
    // edge, and its cards and the graph's nodes gliding aside; the panel of algorithms closing as ALGORITHM goes.
    for (Panel *panel : panels_)
        if (panel->step())
            everywhere = true;
    // An envelope's display zooming: pushed out by a handle, which changes its params (anywhere they show), or easing
    // back to filling the display, on the inspector's ENVELOPE tab or its card.
    if (const int env = envZoom_.env; envZoomStep()) {
        if (drag_.kind == DragKind::EnvPoint)
            everywhere = true;
        else if (env < kOps)
            in(kInspector);
        else
            inCard(env - kOps);
    }
    if (sourceMenuOpen_ && !sourceMenuShown()) { // its card or button went: scrolled away, the panel shut, the places full
        sourceMenuOpen_ = false;
        everywhere = true;
    }
    if (modList_ && !modListPillShown()) { // its pill went: fewer modulations, or what showed it changed
        modList_.reset();
        everywhere = true;
    }
    const EditorSettings settings = controller_.editorSettings();
    if (themeIndex(settings.theme) != theme_ || shownGlow() != glow_ || shownLinks() != links_ || shownHue() != hue_ ||
        settings.handleSize != handles_) // picked in another editor
        everywhere = true;
    if ((typing_ || fieldTyping()) && ++caretTicks_ % 32 == 0)
        everywhere = true; // the caret blinks
    if (everywhere)
        areas = {wholeArea()};
    changedAreas_ = std::move(areas);
    return !changedAreas_.empty();
}

// --- Geometry.

bool View::isRatioDisplay(const Widget &w) {
    return w.kind == Kind::Knob && isRatioTarget(w.param);
}

bool View::isRatioTarget(int param) {
    return param >= 0 && paramTable()[param].kind == ParamKind::OpRatio;
}

bool View::isStepperParam(int param) {
    // The voice limit, an operator's unison count and fixed Mult, a shaper's Stack (a voice's and an effect's), a
    // MIDI source's controller number, and the sequencer's Speed, patterns' Length, Octave and Transpose; every other
    // stepped value stays a knob.
    if (param < 0)
        return false;
    if (param == paramIndex(GlobalParam::Voices) || opOfField(param, OpParam::Unison) >= 0 ||
        opOfField(param, OpParam::FixedMult) >= 0 || fxFieldOf(param) == FxParam::ShaperStack ||
        param == paramIndex(SeqParam::Rate) || param == paramIndex(SeqParam::Octave) ||
        param == paramIndex(SeqParam::Transpose))
        return true;
    for (int p = 0; p < kSeqPatterns; ++p)
        if (param == paramIndex(p, SeqPatternParam::Length))
            return true;
    for (int s = 0; s < kShapers; ++s)
        if (param == paramIndex(s, ShaperParam::Stack))
            return true;
    for (int s = 0; s < kMaxSources; ++s)
        if (param == paramIndex(s, SourceParam::CcNumber))
            return true;
    return false;
}

bool View::isStepper(const Widget &w) {
    return w.kind == Kind::Knob && w.well.w > 0.0f;
}

int View::stepperButton(const Widget &w, Point p) {
    if (!isStepper(w) || !w.well.contains(p))
        return 0;
    constexpr float kZone = 14.0f; // each end's button, as far in as its glyph and a little past it
    return p.x < w.well.x + kZone ? -1 : p.x > w.well.right() - kZone ? 1 : 0;
}

Point View::knobCenter(const Widget &w) {
    if (isRatioDisplay(w))
        return kRatioBox.center();
    if (isStepper(w))
        return w.well.center();
    if (!w.compact)
        return {w.rect.center().x, w.rect.y + w.rect.w * 0.5f};
    return w.shrunk ? Point{w.rect.x + 16.0f, w.rect.y + 16.0f} : Point{w.rect.x + 18.0f, w.rect.y + 20.0f};
}

float View::knobRadius(const Widget &w) {
    if (isRatioDisplay(w))
        return kRatioBox.h * 0.5f;
    if (!w.compact)
        return w.rect.w * 0.5f;
    return w.shrunk ? 14.0f : 17.0f;
}

// Where a compact knob's label and number start, right of its rect's left edge.
float View::compactTextX(const Widget &w) {
    return w.shrunk ? 37.0f : 42.0f;
}

// A knob's number shows under it, or beside it in the header.
bool View::onValue(const Widget &w, Point p) {
    if (w.kind == Kind::ValueText)
        return true;
    if (w.kind != Kind::Knob || isRatioDisplay(w)) // the Ratio readout's digits are its number
        return false;
    if (isStepper(w)) // its label, as a knob's (the field resets, as a knob does)
        return w.compact ? p.y < w.well.y : p.y >= w.rect.y + w.rect.w;
    return w.compact ? p.x >= w.rect.x + compactTextX(w) - 4.0f : p.y >= w.rect.y + w.rect.w;
}

Rect View::envRect(int env) const {
    // Mod envelopes are only drawn while a card shows them; otherwise any card's display will do.
    const float card = env < kOps ? kOffScreen : cardOf(env - kOps);
    return env < kOps ? kOpEnvRect : sourceDisplay(card == kOffScreen ? 0.0f : card);
}

float View::envFit(int env) const {
    const DerivedScope scope(*this);
    return envFitScale(envRect(env), envIndices(env), [this](int index) {
        const ParamDef &def = table_[index];
        double longest = value(index);
        forEachMod(index, [&](const Assignment &m) {
            if (!m.bypassed && mods_.sourceOn(m.source))
                longest += modReach(m).second * (def.max - def.min);
        });
        return std::clamp(longest, def.min, def.max);
    });
}

float View::envScale(int env) const {
    const float fit = envFit(env);
    // While its zoom is held, it stays still, so a point stays where it was put, until the envelope would run past the
    // display's end: then it zooms out to keep all of it in.
    return envZoom_.env == env ? std::min(envZoom_.scale, fit) : fit;
}

int View::envOfTime(int index) {
    for (int env = 0; env < kOps + kMaxSources; ++env) {
        const EnvIndices e = envIndices(env);
        if (index == e.attack || index == e.hold || index == e.decay || index == e.release)
            return env;
    }
    return -1;
}

int View::envDragged() const {
    if (drag_.kind == DragKind::EnvPoint || drag_.kind == DragKind::EnvCurve)
        return drag_.widget.a;
    return drag_.kind == DragKind::Value ? envOfTime(drag_.widget.param) : -1;
}

void View::envZoomLeft(Point p) {
    if (envZoom_.env >= 0 && !envZoom_.easing && envDragged() != envZoom_.env && !envRect(envZoom_.env).contains(p))
        envZoom_.easing = true;
}

bool View::envZoomStep() {
    const int env = envZoom_.env;
    if (env < 0)
        return false;
    const Rect r = envRect(env);
    const EnvIndices e = envIndices(env);
    auto get = [this](int index) { return value(index); };
    const float fit = envFit(env);
    const float scale = envZoom_.scale = std::min(envZoom_.scale, fit); // as drawn: never further in than fits
    if (drag_.kind == DragKind::EnvPoint && drag_.widget.a == env) {
        // Pushed past where it stops, up to 24 px further, a handle zooms the envelope out by up to 2.5 % a poll (and
        // at least a third of that), as a card held at a row's edge scrolls it; the handle staying at the display's
        // end, its segment grows. A handle grabbed past where it stops pushes only once taken further right.
        const float limit = envPointLimit(r, envShape(r, e, get, scale), drag_.widget.b);
        const float push = std::min((drag_.now.x - std::max(limit, drag_.start.x + 4.0f)) / 24.0f, 1.0f);
        const float least = envLeastScale(r, e);
        if (push <= 0.0f || scale <= least)
            return false;
        envZoom_.scale = std::max(scale * (1.0f - 0.025f * (0.35f + 0.65f * push)), least);
        applyDrag(drag_.now);
        return true;
    }
    if (!envZoom_.easing)
        return false;
    // Back to filling the display, a third of the way (on the zoom's log scale) each poll: settled within a few tenths
    // of a second.
    const float next = scale * std::pow(fit / scale, 0.35f);
    if (fit / next < 1.002f)
        envZoom_ = {};
    else
        envZoom_.scale = next;
    return true;
}

void View::envelopeWidgets(std::vector<Widget> &list, int env) const {
    const WidgetAdder add{list};
    const EnvIndices e = envIndices(env);
    const EnvShape s = envShape(envRect(env), e, [this](int index) { return value(index); }, envScale(env));
    // Curve handles halfway along each segment, under the points so those win where they overlap.
    for (int b = 0; b < 3; ++b) {
        const Point c = envSegmentPoint(s, b, 0.5, value(e.curve(b)), value(e.sustain));
        add(Kind::EnvCurve, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, e.curve(b), env, b);
    }
    const Point points[] = {s.p1, s.ph, s.p2, s.p4};
    for (int b = 0; b < 4; ++b)
        add(Kind::EnvPoint, {points[b].x - 7.0f, points[b].y - 7.0f, 14.0f, 14.0f}, -1, env, b);
    if (env >= kOps)
        for (int k = 0; k < 7; ++k)
            list[list.size() - 1 - k].clip = sourceCardClip();
}

std::vector<View::Widget> View::widgets() const {
    const DerivedScope scope(*this);
    std::vector<Widget> list;
    list.reserve(400);
    const WidgetAdder add{list};
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    auto knob = [&](int param, Point center, float diameter, const char *label) {
        add(Kind::Knob, knobRect(center, diameter), param);
        list.back().label = label;
    };
    // Header.
    auto headerKnob = [&](int param, float x, float width, const char *label) {
        add(Kind::Knob, {x, kStripY + 12.0f, width, 40.0f}, param);
        list.back().label = label;
        list.back().compact = true;
    };
    const int globals[kHeaderKnobs] = {paramIndex(GlobalParam::Volume), paramIndex(GlobalParam::FmAmount),
                                       paramIndex(GlobalParam::Pitch), paramIndex(GlobalParam::BendRange),
                                       paramIndex(GlobalParam::Pan)};
    const Str globalLabels[kHeaderKnobs] = {Str::LabelVolume, Str::LabelFmAmount, Str::LabelPitch, Str::LabelBend,
                                            Str::LabelPan};
    for (int i = 0; i < kHeaderKnobs; ++i)
        headerKnob(globals[i], headerKnobX(i), kHeaderKnobPlaces[i], tr(globalLabels[i]));
    // Play cluster: Poly | Mono over Legato, then the Glide knob with its Always switch beside its label (added after
    // the knob, so it takes the clicks there).
    const int mono = paramIndex(GlobalParam::Mono);
    const size_t play = list.size();
    add(Kind::Choice, {kPlayX, kStripY + 12.0f, 44.0f, 19.0f}, mono, -1, 0);
    add(Kind::Choice, {kPlayX + 44.0f, kStripY + 12.0f, 44.0f, 19.0f}, mono, -1, 1);
    merge(play);
    // Under them Legato.
    add(Kind::Toggle, kLegatoSwitch, paramIndex(GlobalParam::Legato));
    headerKnob(paramIndex(GlobalParam::Glide), kGlideX, kGlideW, tr(Str::LabelGlide));
    // On the label's row, level with Poly | Mono, clear of the value under it.
    add(Kind::Toggle, {kGlideX + kGlideW - 42.0f, kStripY + 12.0f, 52.0f, 19.0f}, paramIndex(GlobalParam::GlideAlways));
    // Voices, past the second divider, beside the count.
    headerKnob(paramIndex(GlobalParam::Voices), kVoicesX, kVoicesW, tr(Str::LabelVoices));

    // Routing panel, as a graph, as a matrix, or the effects chain; or the shape
    // editor of a freeform LFO, in place of all three.
    const int editing = shapeEditing();
    for (int v = 0; v < static_cast<int>(std::size(kViewTabs)); ++v)
        add(Kind::ViewTab, kViewTabs[v], -1, v);
    const RoutingView view = controller_.layout().view;
    const int partials = partialEditing();
    if (editing >= 0 || partials >= 0) {
        shapes_.widgets(list);
    } else if (view == RoutingView::Mods) {
        modsPanel_.widgets(list);
    } else if (view == RoutingView::Seq) {
        seq_.widgets(list);
    } else if (view == RoutingView::Fx) {
        // The scrollbars under the row and under the graph, which the View drags (see ScrollArea), then the view's own.
        if (fxPanel_.rowLength() > kVisibleFx + 0.01f && !fxGraph())
            add(Kind::Scrollbar, fxScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::Fx));
        if (fxGraph() && fxPanel_.graphLength() > fxPanel_.graphShown() + 0.5f) // wider than its box
            add(Kind::Scrollbar, fxPanel_.graphScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::FxGraph));
        fxPanel_.widgets(list);
    } else { // the routing graph or the matrix, with ALGORITHM
        routingPanel_.widgets(list);
    }

    // Inspector: the node picked, on its page (see InspectorPanel), while nothing covers it.
    if (!inspectorCovered())
        inspector_.widgets(list);

    // Envelope handles, for the mod envelopes on screen (the operator's, on its page's ENVELOPE tab, the inspector
    // lists).
    std::vector<int> envs;
    // Cards partly scrolled out are there too, cut off at the row's ends: (position, screen place).
    const std::vector<int> cardSources = sourcesShown();
    const bool collapsed = sourcesHidden(); // under the SEQ view
    std::vector<std::pair<int, float>> cards;
    for (int position = 0; position < static_cast<int>(cardSources.size()) && !collapsed; ++position)
        if (const float place = sourceOffset(cardSources, position) - sourceScroll();
            onScreen(place, kVisibleCards, sourceSpan(cardSources[position])))
            cards.push_back({position, place});
    for (const auto &[position, card] : cards) // a folded card shows no envelope
        if (mods_.sourceTypeOf(cardSources[position]) == SourceType::Env && !sourceFolded(cardSources[position]))
            envs.push_back(kOps + cardSources[position]);
    for (int env : envs)
        envelopeWidgets(list, env);

    const Str envLabels[] = {Str::LabelAttack, Str::LabelHold, Str::LabelDecay, Str::LabelSustain, Str::LabelRelease};
    // Modulation panel, unless the SEQ view covers it: the add button, the scrollbar, then the cards.
    if (!collapsed)
        add(Kind::SourceMenuButton, sourceMenuButton());
    if (scrollLength(ScrollArea::Sources) > kVisibleCards + 0.01f && !collapsed)
        add(Kind::Scrollbar, sourceScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::Sources));
    const size_t sourceCardsStart = list.size();
    Rect lfoList{}; // the button of the LFO whose shape list is open, on screen
    for (const auto &[position, card] : cards) {
        const int s = cardSources[position];
        if (sourceFolded(s)) { // a strip: the handle, a grip, the switch and cross, then the rest unfolds it
            const Rect p = sourceCard(card, kFoldedSource);
            add(Kind::SourceHandle, sourceHandleRect(p), -1, s); // where it is unfolded
            add(Kind::SourceGrip, {p.x, p.y + 30.0f, p.w, 16.0f}, -1, s, position);
            add(Kind::Toggle, {p.center().x - 10.0f, p.y + 50.0f, 20.0f, 19.0f}, sourceEnabledIndex(s));
            add(Kind::SourceRemove, {p.center().x - 7.0f, p.y + 73.0f, 14.0f, 21.0f}, -1, s);
            add(Kind::SourceFold, {p.x, p.y + 98.0f, p.w, p.h - 98.0f}, -1, s);
            continue;
        }
        const float span = sourceSpan(s);
        const Rect p = sourceCard(card, span);
        const bool lfo = runsLfo(mods_.sourceTypeOf(s)); // an LFO, or Random, a random one
        const bool widened = s == wideLfo();
        const bool widens = lfo && !widened && mods_.lfoShapeOf(s) == LfoShape::Custom;
        // Its always-shown buttons beside the switch: a widened card's three, or the one that widens it.
        const int tools = widened ? kWideSourceTools : widens ? 1 : 0;
        add(Kind::SourceGrip, {p.x, p.y, cardTitleRight(p, true, tools) - p.x, 34.0f}, -1, s, position);
        add(Kind::SourceFold, cardFold(p, true, tools), -1, s);
        add(Kind::SourceHandle, sourceHandleRect(p), -1, s);
        add(Kind::Toggle, sourcePower(card, span), sourceEnabledIndex(s));
        add(Kind::SourceRemove, sourceRemove(card, span), -1, s);
        if (widens)
            add(Kind::ShapeWiden, lfoWidenButton(card), -1, s);
        if (widened)
            shapes_.cardWidgets(list, s, card);
        if (lfo) {
            // An LFO's shape list, or a Random or Curve card's Unipolar | Bipolar in its place, one of them lit, as the
            // clock modes are: a Random card's sets its Bipolar, a Curve card's its Unipolar (on in the first).
            const bool random = mods_.sourceTypeOf(s) == SourceType::Random;
            const bool curve = mods_.sourceTypeOf(s) == SourceType::Curve;
            if (random || curve) {
                const size_t polarity = list.size();
                for (int bipolar = 0; bipolar < 2; ++bipolar)
                    add(Kind::Choice, randomPolaritySwitch(card, bipolar),
                        paramIndex(s, random ? SourceParam::Bipolar : SourceParam::Unipolar), s,
                        random ? bipolar : 1 - bipolar);
                merge(polarity);
            } else {
                add(Kind::ListButton, lfoShapeButton(card), paramIndex(s, SourceParam::Shape));
                if (listMenu_ == paramIndex(s, SourceParam::Shape))
                    lfoList = lfoShapeButton(card);
            }
            if (!widened)
                add(Kind::LfoDisplay, sourceDisplay(card, random), -1, s);
            if (random) {
                knob(paramIndex(s, SourceParam::RandomSmooth), randomKnob(card, 0), 30.0f, tr(Str::LabelSmooth));
                knob(paramIndex(s, SourceParam::RandomChaos), randomKnob(card, 1), 30.0f, tr(Str::LabelChaos));
            }
            // Synced, the knob picks a note value instead of a rate and shows it as its label.
            add(Kind::Toggle, sourceSync(card), paramIndex(s, SourceParam::Sync));
            // Its clock modes under the shape list, one of them lit, as Poly | Mono; each sets Free, Anchor and
            // One-Shot for it (see setLfoClock()), and stands for them all. Unipolar under the sync switch.
            // A Random or Curve card has no Unipolar switch, which its polarity stands for, and a Random one no Phase,
            // which it takes none of.
            const size_t modes = list.size();
            const int clocks = lfoClockCount(s);
            for (int i = 0; i < clocks; ++i)
                add(Kind::Choice, lfoModeSwitch(card, i, clocks), paramIndex(s, SourceParam::Free), s, i);
            merge(modes);
            if (!random && !curve)
                add(Kind::Toggle, lfoUnipolarSwitch(card), paramIndex(s, SourceParam::Unipolar));
            const bool synced = value(paramIndex(s, SourceParam::Sync)) >= 0.5;
            knob(synced ? paramIndex(s, SourceParam::Division) : paramIndex(s, SourceParam::Rate),
                 lfoKnob(card, 0), 30.0f, synced ? nullptr : tr(Str::LabelRate));
            if (!random)
                knob(paramIndex(s, SourceParam::Phase), lfoKnob(card, 1), 30.0f, tr(Str::LabelPhase));
        } else if (mods_.sourceTypeOf(s) == SourceType::Env) {
            const EnvIndices e = envIndices(kOps + s);
            const int params[] = {e.attack, e.hold, e.decay, e.sustain, e.release};
            const float step = (p.w - 24.0f) / 5.0f;
            for (int i = 0; i < 5; ++i)
                knob(params[i], {p.x + 12.0f + step * (i + 0.5f), p.y + 54.0f}, 32.0f, tr(envLabels[i]));
        } else if (mods_.sourceTypeOf(s) == SourceType::Midi) {
            // Its input's list button, where an LFO's shape is, and right of it the controller (while that is its
            // input) and Slew. Its list opens as an LFO's shapes do.
            const int input = paramIndex(s, SourceParam::MidiInput);
            add(Kind::ListButton, midiInputButton(p), input);
            if (listMenu_ == input)
                lfoList = midiInputButton(p);
            if (std::lround(value(input)) == static_cast<long>(MidiInput::Cc))
                knob(paramIndex(s, SourceParam::CcNumber), {p.x + 172.0f, p.y + 54.0f}, 32.0f, tr(Str::LabelCc));
            knob(paramIndex(s, SourceParam::Slew), {p.x + 228.0f, p.y + 54.0f}, 32.0f, tr(Str::LabelSlew));
        }
        // Its modulations are listed in the MODS view, which the button under its display shows.
        add(Kind::SourceMods, sourceModsButton(card), -1, s);
    }
    if (const auto shadow = sourceShadowCard())
        add(Kind::SourceShadow, *shadow);
    for (size_t i = sourceCardsStart; i < list.size(); ++i)
        if (list[i].clip.w <= 0.0f)
            list[i].clip = sourceCardClip();

    // The macros: their knobs, the names under them and their handles, before the depth handles, which a macro's knob
    // takes too.
    const float macros = keyboardTop();
    for (int macro = 0; macro < kMacros; ++macro) {
        const Point center = macroKnob(macros, macro);
        std::snprintf(macroLabels_[macro].data(), macroLabels_[macro].size(), "%s",
                      macroName(controller_.layout(), macro).c_str());
        knob(paramIndex(macro, MacroParam::Value), center, 34.0f, macroLabels_[macro].data());
        add(Kind::MacroName, {center.x - 0.5f * kMacroCell, center.y + 18.0f, kMacroCell, 17.0f}, -1, macro);
        add(Kind::MacroHandle, {center.x - 42.0f, center.y - 10.0f, 20.0f, 20.0f}, -1, kMaxSources + macro);
    }
    // The note source's handle and velocity's, after them, work the same way; their displays open their curves.
    add(Kind::MacroHandle, noteHandle(macros), -1, kNoteSource);
    add(Kind::NoteDisplay, noteDisplay(macros), -1, kNoteSource);
    add(Kind::MacroHandle, velocityHandle(macros), -1, kVelocitySource);
    add(Kind::NoteDisplay, velocityDisplay(macros), -1, kVelocitySource);

    // The steppers' fields. Under the knob's place, over its depth handles, as wide as the widgets beside it leave room
    // for (two steppers side by side sharing what is between them), from 36 to 56 px; on a side with nothing beside it
    // (the end of a row of knobs, whose card's edge is half a column out), half the way to the knob on the other side,
    // less 6 px. In the header and a title row, under its label where the knob and its number were.
    for (size_t i = 0; i < list.size(); ++i) {
        Widget &w = list[i];
        if (w.kind != Kind::Knob || !isStepperParam(w.param) || isRatioDisplay(w))
            continue;
        const Rect r = w.rect;
        if (w.compact) {
            w.well = {r.x + 2.0f, r.y + 15.0f, std::min(r.w - 6.0f, 64.0f), 21.0f};
            continue;
        }
        const float h = r.w >= 32.0f ? 22.0f : 20.0f;
        const float bottom = r.y + r.w - 5.5f; // clear of the depth handles under it
        const float cx = r.center().x, top = bottom - h;
        float sides[2] = {28.0f, 28.0f}; // left, right
        bool open[2] = {true, true};
        float column = 0.0f; // to the nearest knob beside it, center to center
        for (size_t j = 0; j < list.size(); ++j) {
            const Widget &o = list[j];
            if (j == i || o.kind == Kind::ModDot || o.kind == Kind::ModPill)
                continue;
            const bool isKnob = o.kind == Kind::Knob && !o.compact;
            const Rect b = isKnob ? Rect{o.rect.x, o.rect.y, o.rect.w, o.rect.w} : o.rect;
            if (b.bottom() <= top || b.y >= bottom || (b.x < cx && b.right() > cx))
                continue; // above or below it, or behind it (its card, its panel)
            const int side = b.x >= cx ? 1 : 0;
            const float dx = side ? b.x - cx : cx - b.right();
            open[side] = false;
            if (isKnob) {
                const float apart = std::abs(o.rect.center().x - cx);
                column = column > 0.0f ? std::min(column, apart) : apart;
            }
            sides[side] = std::min(sides[side], isKnob && isStepperParam(o.param)
                                                    ? std::abs(o.rect.center().x - cx) * 0.5f - 2.0f
                                                    : dx - 4.0f);
        }
        for (int side = 0; side < 2; ++side)
            if (open[side] && column > 0.0f)
                sides[side] = std::min(sides[side], column * 0.5f - 6.0f);
        float half = std::min(sides[0], sides[1]);
        if (w.clip.w > 0.0f)
            half = std::min({half, cx - w.clip.x - 6.0f, w.clip.right() - cx - 6.0f});
        half = std::max(half, 18.0f);
        w.well = {cx - half, top, 2.0f * half, h};
    }

    // Depth handles in the gap under each modulated knob, and small ones in the bottom right corner of each
    // modulated wire label and the top right of each matrix cell and source's depth bar (another source modulating
    // its depth). As many as
    // the knob's, label's or cell's width holds; past that, as many as fit beside a "+n" pill holding the rest. The
    // handles and pills come in the user's size; they stay as far apart at each.
    const ModHandleSize &sizes = modHandleSize(controller_.editorSettings().handleSize);
    const size_t count = list.size();
    std::vector<Assignment> here;
    for (size_t i = 0; i < count; ++i) {
        const Kind kind = list[i].kind;
        // PATTERN's row of buttons takes them once, under its first (see seqPatternRow()), as a knob does.
        const bool patternRow =
            kind == Kind::Choice && list[i].param == paramIndex(SeqParam::Pattern) && list[i].b == 0;
        if (kind != Kind::Knob && kind != Kind::WireLabel && kind != Kind::OutLabel && kind != Kind::GridCell &&
            kind != Kind::SlotBar && !patternRow)
            continue;
        here.clear();
        forEachMod(list[i].param, [&](const Assignment &m) { here.push_back(m); });
        if (here.empty())
            continue;
        const Rect r = list[i].rect;
        const int n = static_cast<int>(here.size());
        auto pill = [&](Rect rect, int shown, bool compact) {
            add(Kind::ModPill, rect, -1, list[i].param, shown);
            list.back().compact = compact;
            list.back().clip = list[i].clip;
            list.back().disabled = list[i].disabled;
        };
        if (kind != Kind::Knob && !patternRow) {
            // Leftwards from the corner, 8 px apart on a label and 7 on a cell or depth bar, no further out than
            // stays inside it at the user's size; past what fits, as many as leave room for the pill. A wire's or
            // OUT label, in the graph and on the operator page's INPUTS tab, has them centered just under it and its
            // ranges on its outline's top, so its depth stays readable. A matrix cell has them centered along its
            // foot, its ranges along its top; a depth bar in its top right corner, its ranges along its bottom.
            const bool cell = kind == Kind::GridCell || kind == Kind::SlotBar;
            const float margin = std::max(cell ? 4.0f : 3.0f, sizes.dotCompact + 1.0f), step = cell ? 7.0f : 8.0f,
                        y = kind == Kind::GridCell ? r.bottom() - 4.0f
                            : cell                 ? r.y + 4.0f
                                                   : r.bottom() + 0.75f + sizes.dotCompact;
            const int fit = static_cast<int>((r.w - 2.0f * margin) / step) + 1;
            int shown = n;
            float pillW = 0.0f;
            if (n > fit) {
                // The pill's left edge stays 1 px inside: 4.25 px left of the last handle shown, or at the corner.
                pillW = n - std::max(fit - 2, 0) >= 10 ? sizes.pillWideCompact : sizes.pillCompact;
                const float room = r.w - margin - 4.25f - pillW - 1.0f;
                shown = room < 0.0f ? 0 : std::min(static_cast<int>(std::floor(room / step)) + 1, n - 1);
            }
            // The first handle's center; under a label and along a matrix cell's foot, the handles and pill as a
            // group centered on it.
            float first = r.right() - margin;
            if (kind != Kind::SlotBar) {
                const float span = static_cast<float>(std::max(shown - 1, 0)) * step;
                const float right = shown > 0 ? sizes.dotCompact : 0.0f;
                const float left = n > fit ? -(shown > 0 ? span + 4.25f : 0.0f) - pillW : -span - sizes.dotCompact;
                first = r.center().x - 0.5f * (left + right);
            }
            for (int k = 0; k < shown; ++k) {
                const Point c{first - static_cast<float>(k) * step, y};
                add(Kind::ModDot, {c.x - 3.5f, c.y - 3.5f, 7.0f, 7.0f},
                    slotAmountIndex(here[k].source, here[k].slot), here[k].source, here[k].slot);
                list.back().compact = true;
                list.back().clip = list[i].clip; // a depth bar's is cut off with its card
            }
            if (n > fit) {
                const float edge =
                    first - static_cast<float>(std::max(shown - 1, 0)) * step - (shown > 0 ? 4.25f : 0.0f);
                pill({edge - pillW, y - 0.5f * sizes.pillCompactH, pillW, sizes.pillCompactH}, shown, true);
            }
            continue;
        }
        // Under a knob, 10 px apart and centered; a header knob's go under its knob, at its left.
        // The Ratio readout's go along its box's bottom edge.
        // A stepper's go under its field, across it.
        const bool stepper = isStepper(list[i]);
        const Rect well = list[i].well;
        const Point base = patternRow                       ? Point{seqPatternRow().center().x, kSeqPatternHandlesY}
                           : isRatioDisplay(list[i])         ? Point{r.center().x, kRatioBox.bottom() - 1.0f}
                           : stepper && list[i].compact ? Point{well.center().x, well.bottom() + 4.5f}
                           : list[i].compact            ? Point{r.x + 18.0f, r.y + 37.0f}
                                                        : Point{r.center().x, r.y + r.w - 1.0f};
        // The Ratio readout keeps its handles clear of its box's rounded corners.
        constexpr float kStep = 10.0f;
        const float width = patternRow                ? seqPatternRow().w
                            : isRatioDisplay(list[i]) ? r.w - 9.0f
                            : stepper               ? well.w - 6.0f
                            : list[i].compact       ? 34.0f
                                                    : r.w;
        const float dot = sizes.dot; // the handles stay inside that width at the user's size
        const int fit = static_cast<int>((width - 2.0f * dot) / kStep) + 1;
        // Past that, as many as fit beside the pill, 2 px left of it.
        constexpr float kPillGap = 2.0f;
        auto beside = [&](float pillW) {
            const float room = width - pillW - kPillGap - 2.0f * dot;
            return room < 0.0f ? 0 : std::min(static_cast<int>(std::floor(room / kStep)) + 1, n - 1);
        };
        int shown = n;
        float pillW = 0.0f;
        if (n > fit) {
            pillW = sizes.pill;
            shown = beside(pillW);
            if (n - shown >= 10) { // two digits
                pillW = sizes.pillWide;
                shown = beside(pillW);
            }
        }
        const float handles = shown > 0 ? static_cast<float>(shown - 1) * kStep + 2.0f * dot : 0.0f;
        const float left = base.x - (n > fit ? handles + (shown > 0 ? kPillGap : 0.0f) + pillW : handles) * 0.5f;
        for (int k = 0; k < shown; ++k) {
            const float x = left + dot + static_cast<float>(k) * kStep;
            add(Kind::ModDot, {x - 5.0f, base.y - 5.0f, 10.0f, 10.0f}, slotAmountIndex(here[k].source, here[k].slot),
                here[k].source, here[k].slot);
            list.back().clip = list[i].clip;
            list.back().disabled = list[i].disabled;
        }
        if (n > fit)
            pill({left + handles + (shown > 0 ? kPillGap : 0.0f), base.y - 0.5f * sizes.pillH, pillW, sizes.pillH}, shown, false);
    }

    // Keyboard strip. The white keys come first, so the black keys on top win the hits. Collapsed, only its top row
    // stays, where the wheels' handles are anyway.
    const float keys = keyboardTop();
    const bool keysCollapsed = controller_.layout().keysCollapsed;
    add(Kind::KeysToggle, keysToggle(keys));
    add(Kind::MacroHandle, bendHandle(keys), -1, kBendSource);
    add(Kind::MacroHandle, wheelHandle(keys), -1, kWheelSource);
    if (!keysCollapsed) {
        add(Kind::BendWheel, bendWheel(keys));
        add(Kind::ModWheel, modWheel(keys), paramIndex(GlobalParam::ModWheel));
        add(Kind::Octave, octaveButton(-1, keys), -1, -1);
        add(Kind::Octave, octaveButton(1, keys), -1, 1);
    }
    for (int key = 0; key < kKeyCount && !keysCollapsed; ++key) {
        if (isBlackKey(key))
            continue;
        const float x = kKeys.x + (key / 12 * 7 + kWhiteSlot[key % 12]) * kWhiteW;
        add(Kind::Key, {x, keys + kKeys.y, kWhiteW, kKeys.h}, -1, key);
    }
    for (int key = 0; key < kKeyCount && !keysCollapsed; ++key) {
        if (!isBlackKey(key))
            continue;
        const float x = kKeys.x + (key / 12 * 7 + kBlackAfter[key % 12] + 1) * kWhiteW - kBlackW * 0.5f;
        add(Kind::Key, {x, keys + kKeys.y, kBlackW, kBlackH}, -1, key);
    }
    for (int redo = 0; redo < 2; ++redo)
        add(Kind::UndoButton, undoButton(redo), -1, redo);
    // A host that lets the editor set its tempo has the synth play its own always: no H there.
    if (!controller_.hasTempo())
        add(Kind::TempoSync, kTempoSync);
    add(!controller_.hasTempo() && controller_.tempoSync() ? Kind::TempoHost : Kind::TempoBox, kTempoBox);
    add(Kind::PresetStep, presetStep(-1), -1, -1);
    add(Kind::PresetNameBox, kPresetName);
    if (controller_.presetHasFile())
        add(Kind::PresetInfoButton, {kPresetName.right() - 48.0f, kPresetName.center().y - 10.0f, 20.0f, 20.0f});
    add(Kind::PresetStep, presetStep(1), -1, 1);
    for (int action = 0; action < 4; ++action)
        add(Kind::PresetButton, toolbarButton(action), -1, action);
    add(Kind::SettingsButton, toolbarButton(4));

    // The list of source types, under + SOURCE or where the dashed card was clicked, while that is on screen.
    if (sourceMenuOpen_ && sourceMenuShown()) {
        const Point at = sourceMenuTopLeft();
        add(Kind::SourceMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::SourceMenu, sourceMenu(at), -1, 1);
        for (const SourceType type : kSourceMenuOrder)
            add(Kind::SourceMenuItem, sourceMenuItem(at, type), -1, static_cast<int>(type));
    }
    // The open list of an effect's choice (the shaper's curve, the reverb's algorithm), under its button,
    // while that card is on screen and still holds that effect.
    const FxParam listField = fxFieldOf(listMenu_);
    const int listSlot = fxSlotOf(listMenu_);
    if (listSlot >= 0 && fxWide() && fxTypeOfField(listField) == fx_.slotType(listSlot) &&
        (fxGraph() || !fxFolded(listSlot))) {
        if (const std::optional<Rect> shown = fxCardOf(listSlot)) {
            const Rect card = *shown;
            const Rect button = listField == FxParam::ReverbType    ? reverbTypeButton(card)
                                : listField == FxParam::FilterType  ? filterTypeListButton(card)
                                : listField == FxParam::FilterSlope
                                    ? (filterTypeOf(value(paramIndex(listSlot, FxParam::FilterType))) == FilterType::Morph
                                           ? morphSlopeButton(fxDisplayIn(card, FxType::Filter))
                                           : slopeButton(fxDisplayIn(card, FxType::Filter)))
                                : listField == FxParam::AmpModel    ? ampModelButton(card)
                                : listField == FxParam::AmpCab      ? ampCabButton(card)
                                                                    : clipModeButton(card, clipMode(value(listMenu_)) ==
                                                                                                    ClipMode::Custom);
            const int items = listCount(listMenu_);
            add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::ListMenu, listMenu(button, items), -1, 1);
            for (int item = 0; item < items; ++item)
                add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, listItemValue(listMenu_, item));
        }
    }
    // A split's list of slopes, under its button on its card (in the row's container or the graph's inspector).
    if (const int split = splitOf(listMenu_); split >= 0 && isSplitSlope(listMenu_) && fxWide()) {
        if (const std::optional<Rect> shown = fxPanel_.splitCardOf(split)) {
            const Rect button = splitSlopeButton(FxPanel::splitDisplay(*shown, fx_.splitThree(split)));
            const int items = listCount(listMenu_);
            add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::ListMenu, listMenu(button, items), -1, 1);
            for (int item = 0; item < items; ++item)
                add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, listItemValue(listMenu_, item));
        }
    }
    for (const Panel *panel : panels_) // theirs: the SEQ view's scale and snap lists, the MODS view's open menu, the
        panel->popups(list);           // panel of algorithms and a route's list of modes
    // An LFO's list of shapes, under its button on its card, cut off at the row's ends like the card, though
    // reaching past its bottom.
    if (lfoList.w > 0.0f) {
        const int items = listCount(listMenu_);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::ListMenu, listMenu(lfoList, items), -1, 1);
        list.back().clip = sourceListClip();
        for (int item = 0; item < items; ++item) {
            add(Kind::ListItem, listMenuItem(lfoList, items, item), listMenu_, -1, listItemValue(listMenu_, item));
            list.back().clip = sourceListClip();
        }
    }
    // The open list of a parametric EQ band's shapes, above its button, while that card is on screen.
    if (peqMenu_ >= 0 && fxWide() && fx_.slotType(fxSlotOf(peqMenu_)) == FxType::ParamEq) {
        const int slot = fxSlotOf(peqMenu_);
        const int band = peqBandOfType(peqMenu_);
        const std::optional<Rect> shown = fxCardOf(slot);
        if (band >= 0 && shown && (fxGraph() || !fxFolded(slot))) {
            const Rect card = *shown;
            add(Kind::PeqTypeMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::PeqTypeMenu, peqMenu(card, band), -1, 1);
            for (int t = 0; t < kPeqTypes; ++t)
                add(Kind::PeqTypeItem, peqMenuItem(card, band, t), peqMenu_, -1, t);
        }
    }
    // A pill's open list, over everything: its rows' bypass dots, depth bars, aux's x and removes work as in the MODS
    // view.
    if (modList_) {
        std::vector<Assignment> rows;
        forEachMod(modList_->target, [&](const Assignment &m) { rows.push_back(m); });
        auto auxOf = [this](const Assignment &m) {
            return slotAux([this](int index) { return value(index); }, m.source, m.slot);
        };
        const bool auxes = std::any_of(rows.begin(), rows.end(), [&](const Assignment &m) { return auxOf(m) >= 0; });
        const Rect panel = modListPanel(modList_->anchor, static_cast<int>(rows.size()), auxes);
        const float shift = auxes ? kModListAuxW : 0.0f; // the depth bar on, past the aux column
        add(Kind::ModList, panel, -1, modList_->target);
        list.back().popup = true;
        // Past kModListRows rows they scroll, cut off under the title and at the panel's foot.
        const Rect rowsArea{panel.x, panel.y + kModListTitleH - 2.0f, panel.w, panel.bottom() - 2.0f - (panel.y + kModListTitleH - 2.0f)};
        const float scrolled = modListFirst();
        for (size_t k = 0; k < rows.size(); ++k) {
            const int s = rows[k].source, slot = rows[k].slot;
            const size_t first = list.size();
            const Rect r{panel.x + kModListPad,
                         panel.y + kModListTitleH + (static_cast<float>(k) - scrolled) * kModListRowStep,
                         kCardW - 24.0f + shift, 20.0f};
            if (r.bottom() <= rowsArea.y || r.y >= rowsArea.bottom())
                continue;
            add(Kind::ModListRow, r, -1, s, slot);
            add(Kind::SlotBypass, {r.x + 1.0f, r.y, 16.0f, r.h}, slotBypassIndex(s, slot), s, slot);
            if (auxOf(rows[k]) >= 0) { // the x at the end of its chip (see paintModListRow())
                const Rect chip = modListAuxChip(r);
                add(Kind::ModsAuxDepth, modListAuxDepth(r), slotAuxDepthIndex(s, slot), s, slot);
                add(Kind::ModsAuxClear, {chip.right() - 20.0f, chip.y + 2.0f, 18.0f, chip.h - 4.0f}, -1, s, slot);
            }
            add(Kind::SlotBar, {r.x + 116.0f + shift, r.y, 76.0f, r.h}, slotAmountIndex(s, slot), s, slot);
            add(Kind::ValueText, {r.x + 194.0f + shift, r.y, 38.0f, r.h}, slotAmountIndex(s, slot), s, slot);
            add(Kind::SlotRemove, {r.right() - 20.0f, r.y, 20.0f, r.h}, -1, s, slot);
            for (size_t i = first; i < list.size(); ++i) {
                list[i].popup = true;
                if (modListEnd() > 0.0f)
                    list[i].clip = rowsArea;
            }
        }
        if (modListEnd() > 0.0f) {
            const Rect track = modListScrollTrack(panel);
            add(Kind::ModListScroll, {track.x - 3.0f, track.y, track.w + 5.0f, track.h});
            list.back().popup = true;
        }
    }
    return list;
}

// What an effect's card holds below its title row (see FxPanel::cardWidgets(), whose title row it is): its knobs,
// switches and lists, and the handles on its display.
void View::effectCardWidgets(std::vector<Widget> &list, int slot, Rect card) const {
    const WidgetAdder add{list};
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    auto knob = [&](int param, Point center, float diameter, const char *label) {
        add(Kind::Knob, knobRect(center, diameter), param);
        list.back().label = label;
    };
    // A drawn shaper curve is edited right in its display, as in the big editor its edit button (beside its curve's
    // list, ClipDisplay) opens.
    auto drawnCurve = [&](int unit, Rect display) { shapes_.drawnWidgets(list, unit, display); };
    const FxType type = fx_.slotType(slot);
    auto at = [slot](FxParam f) { return paramIndex(slot, f); };
    auto knobs = [&](std::initializer_list<int> params, std::initializer_list<const char *> labels,
                     float y = 240.0f) {
        const float step = card.w / static_cast<float>(params.size());
        // Four or more across a card of the usual width take smaller knobs, leaving room between them for their
        // labels; a wide card's keep their size.
        const bool narrow = card.w < 1.5f * kFxCardW;
        const float diameter = params.size() > 4 && narrow   ? 24.0f
                               : params.size() > 3 && narrow ? 26.0f
                               : params.size() > 3           ? 32.0f
                               : y == 240.0f                 ? 36.0f
                                                             : 30.0f;
        for (size_t i = 0; i < params.size(); ++i)
            if (params.begin()[i] >= 0) // -1 leaves its place empty
                knob(params.begin()[i], {card.x + step * (static_cast<float>(i) + 0.5f), card.y + y}, diameter,
                     labels.begin()[i]);
    };
    // Feedback and Mix as the lower row, kFxPairX in from the card's sides (see there).
    auto pair = [&](int feedback, int mix) {
        knob(feedback, {card.x + card.w * kFxPairX, card.y + kFxKnobsY2}, 30.0f, tr(Str::LabelFeedback));
        knob(mix, {card.right() - card.w * kFxPairX, card.y + kFxKnobsY2}, 30.0f, tr(Str::LabelMix));
    };
    switch (type) {
    case FxType::Shaper: {
        const ClipMode mode = clipMode(value(at(FxParam::ShaperMode)));
        const bool custom = mode == ClipMode::Custom, stacked = mode == ClipMode::Stack;
        add(Kind::ListButton, clipModeButton(card, custom), at(FxParam::ShaperMode));
        if (custom)
            add(Kind::ClipDisplay, clipEditButton(card), -1, slot);
        const size_t switches = list.size();
        if (custom)
            add(Kind::Toggle, clipSymButton(card), at(FxParam::ShaperSymmetric));
        add(Kind::Toggle, clipLinkButton(card, custom, stacked), at(FxParam::ShaperLink));
        merge(switches);
        // How many stages a Stack runs: a stepper right of LINK (its field placed with the other steppers'), the two
        // centered together, without a label (its rect the knob's place alone, clear of the knobs under it).
        if (stacked) {
            knob(at(FxParam::ShaperStack), clipStackCenter(card), kClipStackKnob, nullptr);
            list.back().rect.h = kClipStackKnob;
        }
        if (custom)
            drawnCurve(slot, fxDisplayIn(card, type));
        // Drive, Tone (which end the curve drives harder), Output and Mix. Its oversampling is in its title's
        // right-click menu.
        knobs({at(FxParam::ShaperDrive), at(FxParam::ShaperTone), at(FxParam::ShaperOutput), at(FxParam::ShaperMix)},
              {tr(Str::LabelDrive), tr(Str::LabelTone), tr(Str::LabelOutput), tr(Str::LabelMix)}, kShaperKnobsY);
        break;
    }
    case FxType::Delay: {
        // Sync, link and ping-pong, as icons, across the card under the display.
        const FxParam switches[] = {FxParam::DelaySync, FxParam::DelayLink, FxParam::DelayPingPong};
        const float w = (card.w - 28.0f + 4.0f) / 3.0f;
        const float under = fxDisplayIn(card, type).bottom() + 4.0f;
        const size_t first = list.size();
        for (int i = 0; i < 3; ++i)
            add(Kind::Toggle, {card.x + 14.0f + i * w, under, w - 4.0f, 18.0f}, at(switches[i]));
        merge(first);
        // The time knobs: Time L and Time R, or synced, note values, Rate L and Rate R; their values show under the
        // mouse, as other knobs' do. Linked, the left one sets both sides, named Time or Rate alone.
        const bool synced = value(at(FxParam::DelaySync)) >= 0.5;
        const int timeL = synced ? at(FxParam::DelayDivisionL) : at(FxParam::DelayTimeL);
        const int timeR = synced ? at(FxParam::DelayDivisionR) : at(FxParam::DelayTimeR);
        const bool linked = value(at(FxParam::DelayLink)) >= 0.5;
        const Str left = synced ? (linked ? Str::LabelRate : Str::LabelRateL)
                                : (linked ? Str::LabelTime : Str::LabelTimeL);
        knobs({timeL, linked ? -1 : timeR, at(FxParam::DelayFeedback)},
              {tr(left), tr(synced ? Str::LabelRateR : Str::LabelTimeR), tr(Str::LabelFeedback)}, kFxKnobsY1);
        knobs({at(FxParam::DelayTone), at(FxParam::DelayDuck), at(FxParam::DelayWidth), at(FxParam::DelayMix)},
              {tr(Str::LabelTone), tr(Str::LabelDuck), tr(Str::LabelWidth), tr(Str::LabelMix)}, kFxKnobsY2);
        break;
    }
    case FxType::Eq:
        knobs({at(FxParam::EqLow), at(FxParam::EqMid), at(FxParam::EqHigh)},
              {tr(Str::LabelLow), tr(Str::LabelMid), tr(Str::LabelHigh)});
        break;
    case FxType::ParamEq: {
        const Rect area = fxDisplayIn(card, type).inset(8.0f);
        for (int band = 0; band < kPeqBands; ++band) {
            const Point h = peqHandle(area, slot, band);
            add(Kind::PeqPoint, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f}, peqQIndex(slot, band), slot, band);
        }
        // Each band's chip: its power symbol and number switch it on and off, and its shape opens the list
        // of shapes; either picks the band, whose knobs show.
        for (int band = 0; band < kPeqBands; ++band) {
            const Rect chip = peqChip(card, band);
            add(Kind::PeqBand, chip, -1, slot, band);
            add(Kind::PeqBandOn, peqChipSwitch(chip), peqOnIndex(slot, band), slot, band);
            add(Kind::PeqTypeButton, peqChipShape(chip), peqTypeIndex(slot, band), slot, band);
        }
        // Then Amount, for the whole EQ; all four as large as a one-row card's.
        const int band = std::min(fxPanel_.band(slot), kPeqBands - 1);
        const int params[] = {peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band),
                              at(FxParam::PeqAmount)};
        const Str labels[] = {Str::LabelFreq, Str::LabelGain, Str::LabelQ, Str::LabelAmount};
        for (int i = 0; i < 4; ++i)
            knob(params[i], peqKnobCenter(card, i), kPeqKnobD, tr(labels[i]));
        break;
    }
    case FxType::Filter: {
        // The filter type's list across the card, and the slope's list under the response (a morph's with
        // its notch switch beside it); a formant's vowel or a morph's place between Drive and Mix.
        const auto filterType = filterTypeOf(value(at(FxParam::FilterType)));
        add(Kind::ListButton, filterTypeListButton(card), at(FxParam::FilterType));
        if (filterType == FilterType::Morph) {
            add(Kind::ListButton, morphSlopeButton(fxDisplayIn(card, type)), at(FxParam::FilterSlope));
            add(Kind::Toggle, morphNotchButton(fxDisplayIn(card, type)), at(FxParam::FilterMorphMode));
        } else {
            add(Kind::ListButton, slopeButton(fxDisplayIn(card, type)), at(FxParam::FilterSlope));
        }
        // The response's handle, as a voice filter's: b = 1 marks the effect's.
        const Point handle = filterHandle(fxDisplayIn(card, type), value(at(FxParam::FilterCutoff)),
                                          value(at(FxParam::FilterResonance)));
        add(Kind::FilterPoint, {handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f}, -1, slot, 1);
        const FilterKnobLabels names = filterKnobLabels(filterType, true);
        knobs({at(FxParam::FilterCutoff), at(FxParam::FilterResonance), at(FxParam::FilterKeyTrack)},
              {tr(names.cutoff), tr(names.resonance), tr(Str::LabelKeyTrack)}, kFxKnobsY1);
        const bool formant = filterType == FilterType::Formant, morph = filterType == FilterType::Morph;
        knobs({at(FxParam::FilterDrive),
               formant ? at(FxParam::FilterVowel) : morph ? at(FxParam::FilterMorph) : -1,
               at(FxParam::FilterMix)},
              {tr(Str::LabelDrive), formant ? tr(Str::LabelVowel) : morph ? tr(Str::LabelMorph) : nullptr,
               tr(Str::LabelMix)},
              kFxKnobsY2);
        break;
    }
    case FxType::Compressor:
        knobs({at(FxParam::CompThreshold), at(FxParam::CompRatio), at(FxParam::CompKnee), at(FxParam::CompDetectorHp)},
              {tr(Str::LabelThresh), tr(Str::LabelRatio), tr(Str::LabelKnee), tr(Str::LabelDetectorHp)}, kFxKnobsY1);
        knobs({at(FxParam::CompAttack), at(FxParam::CompRelease), at(FxParam::CompMakeup),
               at(FxParam::CompMix)},
              {tr(Str::LabelAttack), tr(Str::LabelRelease), tr(Str::LabelMakeup), tr(Str::LabelMix)}, kFxKnobsY2);
        break;
    case FxType::MultiDyn: {
        // RMS and LINK at the right end of the line over the shared knobs. Each band's row picks the band its knobs set, its threshold lines
        // drag across and its zones up and down, for their ratios. Linked, an edit of one band sets every
        // band's (see Controller::linkedBands()).
        const size_t switches = list.size();
        add(Kind::Toggle, mdRmsButton(card), at(FxParam::MdRms));
        add(Kind::Toggle, mdLinkButton(card), at(FxParam::MdLink));
        merge(switches);
        const Rect display = fxDisplayIn(card, type);
        for (int band = 0; band < kMcBands; ++band)
            add(Kind::DynBand, mdBandRow(display, band), -1, slot, band);
        for (int band = 0; band < kMcBands; ++band) {
            const Rect bar = mdBar(display, band);
            const double below = value(mdBelowIndex(slot, band)), above = value(mdAboveIndex(slot, band));
            for (const bool lower : {false, true})
                if (const Rect zone = mdZone(bar, std::min(below, above), above, lower); zone.w > 0.0f)
                    add(Kind::DynZone, zone, lower ? mdUpIndex(slot, band) : mdDownIndex(slot, band), slot,
                        2 * band + (lower ? 1 : 0));
            add(Kind::DynGrip, mdLineGrip(bar, std::min(below, above), above, true), mdUpIndex(slot, band),
                slot, 2 * band + 1);
            add(Kind::DynGrip, mdLineGrip(bar, std::min(below, above), above, false), mdDownIndex(slot, band),
                slot, 2 * band);
        }
        const int band = fxPanel_.band(slot);
        auto row = [&](std::initializer_list<int> params, std::initializer_list<const char *> labels, float y,
                       float diameter) {
            const float step = card.w / static_cast<float>(params.size());
            for (size_t i = 0; i < params.size(); ++i)
                knob(params.begin()[i], {card.x + step * (static_cast<float>(i) + 0.5f), card.y + y}, diameter,
                     labels.begin()[i]);
        };
        row({mdBelowIndex(slot, band), mdUpIndex(slot, band), mdAboveIndex(slot, band), mdDownIndex(slot, band),
             mdGainIndex(slot, band)},
            {tr(Str::LabelBelow), tr(Str::LabelUp), tr(Str::LabelAbove), tr(Str::LabelDown),
             tr(Str::LabelGain)},
            kMdBandKnobsY, 30.0f);
        row({at(FxParam::MdLowFreq), at(FxParam::MdHighFreq), at(FxParam::MdAttack), at(FxParam::MdRelease)},
            {tr(Str::LabelLowX), tr(Str::LabelHighX), tr(Str::LabelAttack), tr(Str::LabelRelease)},
            kMdSharedY1, 28.0f);
        row({at(FxParam::MdKnee), at(FxParam::MdRange), at(FxParam::MdMix), at(FxParam::MdOutput)},
            {tr(Str::LabelKnee), tr(Str::LabelRange), tr(Str::LabelMix), tr(Str::LabelOutput)}, kMdSharedY2,
            28.0f);
        break;
    }
    case FxType::Reverb:
        add(Kind::ListButton, reverbTypeButton(card), at(FxParam::ReverbType));
        // Three rows: the space; its tone, spread and diffusion; what goes in and how much is heard, Mix at the bottom
        // right as on the other cards. A shimmer's, four to a row, puts Diffuse after Pre (the labels' widths allow it
        // there) and its interval and how much of what goes round it shifts together in the middle of the second.
        if (reverbType(value(at(FxParam::ReverbType))) == ReverbType::Shimmer) {
            knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay),
                   at(FxParam::ReverbDiffusion)},
                  {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay), tr(Str::LabelDiffusion)},
                  kReverbKnobsY[0]);
            knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbShimmerPitch), at(FxParam::ReverbShimmer),
                   at(FxParam::ReverbWidth)},
                  {tr(Str::LabelDamp), tr(Str::LabelPitch), tr(Str::LabelShimmer), tr(Str::LabelWidth)},
                  kReverbKnobsY[1]);
        } else {
            knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay)},
                  {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay)}, kReverbKnobsY[0]);
            knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbWidth), at(FxParam::ReverbDiffusion)},
                  {tr(Str::LabelDamp), tr(Str::LabelWidth), tr(Str::LabelDiffusion)}, kReverbKnobsY[1]);
        }
        knobs({at(FxParam::ReverbLowCut), at(FxParam::ReverbHighCut), at(FxParam::ReverbMix)},
              {tr(Str::LabelLowCut), tr(Str::LabelHighCut), tr(Str::LabelMix)}, kReverbKnobsY[2]);
        break;
    case FxType::Phaser:
    case FxType::Flanger: {
        // Under the display: at the left, over the Rate knob, SYNC (the delay's metronome), in the middle STEREO (two
        // speakers facing apart), two switches of their own; at the right the polarity, + and - as one choice. Synced, the Rate knob
        // picks a note value and shows it as its label.
        const bool phaser = type == FxType::Phaser;
        const int sync = at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync);
        const Rect display = fxDisplayIn(card, type);
        const float under = display.bottom() + 4.0f;
        add(Kind::Toggle, {display.x, under, 32.0f, 18.0f}, sync);
        add(Kind::Toggle, {display.center().x - 22.0f, under, 44.0f, 18.0f},
            at(phaser ? FxParam::PhaserStereo : FxParam::FlangerStereo));
        const int negative = at(phaser ? FxParam::PhaserNegative : FxParam::FlangerNegative);
        const size_t polarity = list.size();
        add(Kind::Choice, {display.right() - 40.0f, under, 20.0f, 18.0f}, negative, -1, 0);
        add(Kind::Choice, {display.right() - 20.0f, under, 20.0f, 18.0f}, negative, -1, 1);
        merge(polarity);
        const bool synced = value(sync) >= 0.5;
        const int rate = synced ? at(phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision)
                                : at(phaser ? FxParam::PhaserRate : FxParam::FlangerRate);
        if (phaser) {
            knobs({rate, at(FxParam::PhaserDepth), at(FxParam::PhaserCenter), at(FxParam::PhaserSpread)},
                  {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelCenter),
                   tr(Str::LabelSpread)},
                  kFxKnobsY1);
            knobs({at(FxParam::PhaserStages), at(FxParam::PhaserFeedback), at(FxParam::PhaserMix)},
                  {tr(Str::LabelStages), tr(Str::LabelFeedback), tr(Str::LabelMix)}, kFxKnobsY2);
            break;
        }
        knobs({rate, at(FxParam::FlangerDepth), at(FxParam::FlangerDelay)},
              {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelDelay)}, kFxKnobsY1);
        pair(at(FxParam::FlangerFeedback), at(FxParam::FlangerMix));
        // Through zero, a switch in the place between them.
        add(Kind::Toggle, {card.center().x - 20.0f, card.y + kFxKnobsY2 - 9.0f, 40.0f, 18.0f},
            at(FxParam::FlangerThroughZero));
        break;
    }
    case FxType::Chorus: {
        // Under the display, in its middle, how many voices: 2, 3, 4 and 6 as one choice.
        const Rect display = fxDisplayIn(card, type);
        const size_t voices = list.size();
        for (int i = 0; i < static_cast<int>(std::size(kChorusVoiceChoices)); ++i)
            add(Kind::Choice, chorusVoicesButton(display, i), at(FxParam::ChorusVoices), -1,
                kChorusVoiceChoices[i]);
        merge(voices);
        knobs({at(FxParam::ChorusRate), at(FxParam::ChorusDepth), at(FxParam::ChorusDelay)},
              {tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelDelay)}, kFxKnobsY1);
        knobs({at(FxParam::ChorusFeedback), at(FxParam::ChorusSpread), at(FxParam::ChorusMix)},
              {tr(Str::LabelFeedback), tr(Str::LabelSpread), tr(Str::LabelMix)}, kFxKnobsY2);
        break;
    }
    case FxType::FreqShifter:
        // The range, then the share of it the shift takes.
        knobs({at(FxParam::ShiftRange), at(FxParam::ShiftAmount), at(FxParam::ShiftSpread)},
              {tr(Str::LabelRange), tr(Str::LabelShift), tr(Str::LabelSpread)}, kFxKnobsY1);
        pair(at(FxParam::ShiftFeedback), at(FxParam::ShiftMix));
        break;
    case FxType::Disperser:
        // Where it disperses and how, over what moves it with the note, how often it repeats and how much is heard.
        knobs({at(FxParam::DisperserFreq), at(FxParam::DisperserStages), at(FxParam::DisperserPinch)},
              {tr(Str::LabelFreq), tr(Str::LabelStages), tr(Str::LabelPinch)}, kFxKnobsY1);
        knobs({at(FxParam::DisperserKeyTrack), at(FxParam::DisperserStretch), at(FxParam::DisperserMix)},
              {tr(Str::LabelKeyTrack), tr(Str::LabelStretch), tr(Str::LabelMix)}, kFxKnobsY2);
        break;
    case FxType::Widener:
        knobs({at(FxParam::WidenerSize), at(FxParam::WidenerWidth), at(FxParam::WidenerHighPass),
               at(FxParam::WidenerMix)},
              {tr(Str::LabelSize), tr(Str::LabelWidth), tr(Str::LabelHighPass), tr(Str::LabelMix)});
        break;
    case FxType::LoFi:
        knobs({at(FxParam::LoFiBits), at(FxParam::LoFiRate), at(FxParam::LoFiSmooth), at(FxParam::LoFiMix)},
              {tr(Str::LabelBits), tr(Str::LabelRate), tr(Str::LabelSmooth), tr(Str::LabelMix)});
        break;
    case FxType::Utility: {
        // On its display, a handle for each knob: Gain's on the strip at its left, Pan's at the top of the
        // sound's middle line, Width's on the band's edge and Mono Bass's at the end of its line.
        const Rect display = fxDisplayIn(card, type);
        for (int handle = 0; handle < kUtilityHandles; ++handle) {
            const Point h = utilityHandlePoint(display, handle, false, slot);
            add(Kind::UtilityHandle, {h.x - 8.0f, h.y - 8.0f, 16.0f, 16.0f}, at(kUtilityHandleParams[handle]),
                slot, handle);
        }
        knobs({at(FxParam::UtilityGain), at(FxParam::UtilityPan)}, {tr(Str::LabelGain), tr(Str::LabelPan)},
              kFxKnobsY1);
        knobs({at(FxParam::UtilityWidth), at(FxParam::UtilityMonoBass)},
              {tr(Str::LabelWidth), tr(Str::LabelMonoBass)}, kFxKnobsY2);
        break;
    }
    case FxType::Amp: {
        // The amp's side: its model's list, STEREO under its display, Gain, Volume and Presence over Bass, Mid
        // and Treble. The cabinet's: its power switch and list, the mic on its display, the mic's three under
        // it, Position and Distance over Output and Mix. With the cabinet off, its own controls dim.
        add(Kind::ListButton, ampModelButton(card), at(FxParam::AmpModel));
        add(Kind::Toggle, ampStereoButton(card), at(FxParam::AmpStereo));
        knob(at(FxParam::AmpGain), ampKnob(card, false, 0, 0), kAmpKnobD, tr(Str::LabelGain));
        knob(at(FxParam::AmpVolume), ampKnob(card, false, 1, 0), kAmpKnobD, tr(Str::LabelVolume));
        knob(at(FxParam::AmpPresence), ampKnob(card, false, 2, 0), kAmpKnobD, tr(Str::LabelPresence));
        knob(at(FxParam::AmpBass), ampKnob(card, false, 0, 1), kAmpKnobD, tr(Str::LabelBass));
        knob(at(FxParam::AmpMid), ampKnob(card, false, 1, 1), kAmpKnobD, tr(Str::LabelMid));
        knob(at(FxParam::AmpTreble), ampKnob(card, false, 2, 1), kAmpKnobD, tr(Str::LabelTreble));
        add(Kind::Toggle, ampCabPower(card), at(FxParam::AmpCabOn));
        add(Kind::ListButton, ampCabButton(card), at(FxParam::AmpCab));
        const size_t cabControls = list.size();
        add(Kind::AmpMic,
            ampMicRect(ampCabDisplay(card), value(at(FxParam::AmpPosition)), value(at(FxParam::AmpDistance))),
            at(FxParam::AmpPosition), slot);
        const size_t mics = list.size();
        for (int mic = 0; mic < static_cast<int>(AmpMic::Count); ++mic)
            add(Kind::Choice, ampMicChoice(card, mic), at(FxParam::AmpMic), -1, mic);
        merge(mics);
        knob(at(FxParam::AmpPosition), ampKnob(card, true, 0, 0), kAmpKnobD, tr(Str::LabelPosition));
        knob(at(FxParam::AmpDistance), ampKnob(card, true, 1, 0), kAmpKnobD, tr(Str::LabelDistance));
        if (value(at(FxParam::AmpCabOn)) < 0.5)
            for (size_t i = cabControls; i < list.size(); ++i)
                list[i].faint = true;
        knob(at(FxParam::AmpOutput), ampKnob(card, true, 0, 1), kAmpKnobD, tr(Str::LabelOutput));
        knob(at(FxParam::AmpMix), ampKnob(card, true, 1, 1), kAmpKnobD, tr(Str::LabelMix));
        break;
    }
    case FxType::MultiComp: // retired: a state's loads as a three-band dynamics
    case FxType::None:
    case FxType::Count:
        break;
    }
}

std::optional<View::Widget> View::hitTest(Point p) const {
    const DerivedScope scope(*this); // asked while painting, the widgets painted
    Panel *const panel = modalPanel(); // a panel's popup or dialog, under the settings and the confirm dialog
    const bool overlay = settingsOpen_ || confirm_ >= 0 || panel != nullptr;
    std::vector<Widget> dialog = settingsOpen_   ? settingsWidgets()
                                 : confirm_ >= 0 ? confirmWidgets()
                                                 : std::vector<Widget>();
    if (!settingsOpen_ && confirm_ < 0 && panel)
        panel->widgets(dialog);
    const std::vector<Widget> &list = overlay ? dialog : scopedWidgets();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if ((it->clip.w > 0.0f && !it->clip.contains(p)) || it->disabled)
            continue;
        // A panel's own whose shape isn't its rect (a wire along its path, an operator's circle, a node's edge).
        std::optional<bool> own;
        for (const Panel *owner : panels_)
            if ((own = owner->hits(*it, p)))
                break;
        if (own) {
            if (*own)
                return *it;
        } else if (it->rect.contains(p) || (isStepper(*it) && it->well.contains(p))) {
            return *it;
        }
    }
    return std::nullopt;
}

bool View::isHovered(const Widget &w) const {
    return drag_.kind == DragKind::None && hover_ && hover_->same(w);
}

bool View::isActive(const Widget &w) const {
    return drag_.kind != DragKind::None && drag_.widget.same(w);
}

// --- Painting.

void View::paint(Canvas &c, Rect area) {
    // Part of the editor: clipped to it, and what keeps to a band of the editor away from it left out. The header,
    // the panels' row (the routing panel and the inspector, or what covers them) and what lies under them each
    // draw within their band; a widget within some way of its rect (its glow, its labels, its depth handles).
    const Rect whole = wholeArea();
    const bool partial = area.x > whole.x || area.y > whole.y || area.right() < whole.right() ||
                         area.bottom() < whole.bottom();
    auto touches = [&](Rect r) {
        return !partial || (r.x < area.right() && r.right() > area.x && r.y < area.bottom() && r.bottom() > area.y);
    };
    const float lowerTop = kRouting.bottom();
    const bool headerShown = touches({0.0f, 0.0f, kWidth, kHeaderH});
    const bool panelsShown = touches(panelsBand()); // the SEQ view's reaches down past lowerTop
    const bool lowerShown = touches({0.0f, lowerTop, kWidth, whole.bottom() - lowerTop});
    constexpr float kWidgetReach = 64.0f;
    if (partial)
        c.pushClip(area);
    const DerivedScope scope(*this);
    const std::vector<Widget> &list = scopedWidgets();
    std::fill(macroOffsets_.begin(), macroOffsets_.end(), 0.0f);
    macroSlotOffsets_.fill(0.0f);
    {
        // The macros' knobs as the macros' and the mod wheel's slots on them take them, then what the knobs move.
        auto get = [this](int i) { return value(i); };
        const MacroGraph graph = macroGraph(get);
        voiceMovedMacros_ = graph.voiceMoved;
        MacroKnobs set = macroKnobsSet(get);
        for (int m = 0; m < kMacros; ++m) // where the sequencer's curves hold them as they play
            set[m] = std::clamp(set[m] + static_cast<double>(telemetry_.seqMacro[m]), 0.0, 1.0);
        MacroKnobs knobs = set;
        runMacroGraph(
            graph, get, knobs, nullptr, [](int) { return -1.0; },
            [this](int source, int slot, double offset) {
                macroSlotOffsets_[source * kSlots + slot] = static_cast<float>(offset);
            });
        for (int m = 0; m < kMacros; ++m)
            macroKnobOffsets_[m] = static_cast<float>(knobs[m] - set[m]);
        forEachMacroMod(get, knobs, [this](int target, double offset, int source, int slot) {
            macroOffsets_[target] += static_cast<float>(offset);
            macroSlotOffsets_[source * kSlots + slot] =
                static_cast<float>(offset / (table_[target].max - table_[target].min));
        });
    }

    theme_ = themeIndex(controller_.editorSettings().theme);
    hue_ = shownHue();
    look = withAccentHue(kThemes[theme_], hue_); // its main color turned to the hue the settings pick
    glow_ = glowing = shownGlow();
    tintedCards = controller_.editorSettings().tintedCards;
    links_ = shownLinks();
    handles_ = controller_.editorSettings().handleSize;
    ++cardPaint_;
    if (partial) {
        c.fillRect(area, kBackground);
    } else {
        c.clear(kBackground);
        cardRects_.clear(); // the cards painted below note where they are again
    }
    paintPanels(c);
    if (headerShown)
        paintHeader(c);
    if (shapeEditing() >= 0 || partialEditing() >= 0)
        shapes_.paintBack(c);
    else if (panelsShown) switch (controller_.layout().view) {
    case RoutingView::Graph:
    case RoutingView::Matrix:
        routingPanel_.paintBack(c);
        break;
    case RoutingView::Fx:
        fxPanel_.paintBack(c);
        break;
    case RoutingView::Mods:
        modsPanel_.paintBack(c);
        break;
    case RoutingView::Seq:
        seq_.paintBack(c);
        break;
    }
    if (!inspectorCovered() && panelsShown)
        inspector_.paintBack(c);
    if (lowerShown) {
        paintKeyboard(c);
        paintNoteSource(c, keyboardTop());
        paintVelocitySource(c, keyboardTop());
        paintSources(c);
    }
    // A switched-off card's displays are dimmed under an overlay of its own face, which leaves the face as it is: as
    // though drawn at kDimOpacity. Its widgets, and every other dimmed one, are drawn at that opacity as they go.
    const std::vector<DimArea> dimmed = dimmedAreas();
    for (const DimArea &d : dimmed) {
        if (d.face.a <= 0.0f)
            continue; // the inspector's page dims itself (paintInspector())
        c.pushClip(d.clip);
        c.fillRect(d.area, d.face.alpha(1.0f - kDimOpacity), kRadiusCard - 1.0f);
        c.popClip();
    }
    // The open lists (of effects, curves, EQ shapes, waves) go over everything else, the dimmed cards included.
    auto paintWidgets = [&](bool lists) {
        for (const Widget &w : list) {
            const bool open = w.kind == Kind::FxMenu || w.kind == Kind::FxAdd || w.kind == Kind::FxAddNode ||
                              w.kind == Kind::SourceMenu ||
                              w.kind == Kind::SourceMenuItem || w.kind == Kind::ListMenu ||
                              w.kind == Kind::ListItem || w.kind == Kind::PeqTypeMenu ||
                              w.kind == Kind::PeqTypeItem || w.kind == Kind::WaveMenu ||
                              w.kind == Kind::WaveMenuItem || w.popup;
            if (open != lists || !touches(w.rect.inset(-kWidgetReach)))
                continue;
            if (w.clip.w > 0.0f)
                c.pushClip(w.clip);
            // A list open over a dimmed card is not dimmed with it.
            const bool dim = lists ? (w.disabled || w.faint) : widgetDimmed(w, dimmed);
            // An idle knob alone is fainter than a dimmed one, but readable (see kIdleOpacity).
            auto inDimmedArea = [&](const DimArea &a) { return a.area.contains(w.rect.center()); };
            const bool idleOnly = dim && !lists && knobIdle(w) && !w.disabled && !w.faint &&
                                  std::none_of(dimmed.begin(), dimmed.end(), inDimmedArea);
            if (dim)
                c.pushLayer({0.0f, 0.0f}, idleOnly ? kIdleOpacity : kDimOpacity, w.rect.inset(-kWidgetReach));
            paintWidget(c, w);
            if (dim)
                c.popLayer();
            if (w.clip.w > 0.0f)
                c.popClip();
        }
    };
    paintWidgets(false);
    // An envelope handle's readout goes over the curve and the other handles.
    for (const Widget &w : list)
        if ((w.kind == Kind::EnvPoint || w.kind == Kind::EnvCurve) && (isHovered(w) || isActive(w))) {
            if (w.clip.w > 0.0f)
                c.pushClip(w.clip);
            paintEnvReadout(c, w);
            if (w.clip.w > 0.0f)
                c.popClip();
        }
    for (Panel *panel : panels_) // what veils some of their widgets (the FX view's cards a card carried lands among)
        panel->paintOverWidgets(c);
    paintWidgets(true);
    for (Panel *panel : panels_) // what a drag of theirs carries (an effect's card or a split's container, a route)
        panel->paintDrag(c);
    if (drag_.kind == DragKind::SourceMove && drag_.moved) {
        const int source = drag_.widget.a;
        const std::vector<int> shown = sourcesShown();
        const auto at = std::find(shown.begin(), shown.end(), source);
        if (at != shown.end()) {
            const float place = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
            paintCarriedCard(c, sourceCard(place, sourceSpan(source)),
                             sourceCard(sourceOffset(sources(), drag_.widget.b) - drag_.scrollStart, sourceSpan(source)),
                             sourceCardClip(), kRadiusCard, kSourceColors[source], [&] { paintSource(c, place, source); },
                             list);
        }
    }
    if (drag_.kind == DragKind::ModSource)
        paintModOverlay(c, list);
    else if (hover_ && (hover_->kind == Kind::SourceHandle || hover_->kind == Kind::MacroHandle) &&
             drag_.kind == DragKind::None) {
        paintSourceLinks(c, list, hover_->a);
        paintAuxLinks(c, list, hover_->a);
    } else if (hover_ && hover_->kind == Kind::ModDot && drag_.kind == DragKind::None) {
        paintSourceLinks(c, list, hover_->a, hover_->b);
        if (const int aux = slotAux([this](int index) { return value(index); }, hover_->a, hover_->b); aux >= 0)
            paintAuxLinks(c, list, aux, hover_->a, hover_->b);
    }
    paintHoverTags(c, list);
    if (typing_)
        paintTyping(c);
    for (Panel *panel : panels_) // what they show over the editor: the preset browser's info card, browser and dialogs,
        panel->paint(c);         // the MODS view's open menu
    if (settingsOpen_) {
        paintSettings(c);
        paintTooltip(c); // its sliders'
    } else if (confirm_ >= 0)
        paintConfirm(c);
    else
        paintTooltip(c);

    // Under the preset bar, over the panels' title rows.
    if (flashTicks_ > 0) {
        const Rect toast{View::kWidth * 0.5f - 220.0f, kPanelsTop - 2.0f, 440.0f, 28.0f};
        paintMenuPanel(c, toast);
        c.text(toast, flash_, kTextValue, kText);
    }
    if (partial)
        c.popClip();
}

void View::paintPanels(Canvas &c) {
    const Rect keyboard = keyboardPanel(keyboardTop(), controller_.layout().keysCollapsed);
    std::vector<Rect> panels;
    if (sourcesHidden()) // the SEQ view, over the routing panel's place and the modulation panel's
        panels = {keyboard, kSeqPanel};
    else if (shapeEditing() >= 0 && isClipCurve(shapeEditing()))
        panels = {kModPanel, keyboard, shapes_.shapePanel()}; // the shaper curve's editor, narrower than the effects
    else // the effects or the modulations across it, or the routing panel with the inspector floating over it
        panels = {kModPanel, keyboard, kFxPanel};
    // A theme with soft shadows lifts its panels off the background with one, instead of an outline.
    if (look.panelShadow > 0.0f) {
        auto shadow = [&](Rect p, float radius) { softShadow(c, p, radius, 5, 1.2f, 2.0f, look.panelShadow); };
        // The panel under the tabs, while its first tab is the one shown, has that corner square (see Kind::ViewTab),
        // so its shadow is too there: round elsewhere, square beside the corner, and above it only the tab's.
        const bool firstShown = controller_.layout().view == RoutingView::Graph && shapeEditing() < 0 &&
                                partialEditing() < 0;
        for (const Rect &p : panels) {
            if (!firstShown || p.x != kFxPanel.x || p.y != kFxPanel.y) {
                shadow(p, kPanelRadius);
                continue;
            }
            const Rect corner{p.x - 12.0f, p.y - 12.0f, kPanelRadius + 12.0f, kPanelRadius + 12.0f};
            c.pushClip({corner.right(), p.y - 12.0f, p.w + 24.0f, p.h + 24.0f}); // right of the corner
            shadow(p, kPanelRadius);
            c.popClip();
            c.pushClip({p.x - 12.0f, corner.bottom(), corner.w, p.h + 24.0f}); // below it
            shadow(p, kPanelRadius);
            c.popClip();
            c.pushClip({corner.x, p.y, corner.w, corner.bottom() - p.y}); // beside it, under the panel's top
            shadow(p, 0.0f);
            c.popClip();
        }
    }
    for (const Rect &p : panels) {
        c.fillRect(p, kPanel, kPanelRadius);
        if (look.panelShadow <= 0.0f)
            c.strokeRect(p, kBorder, 1.0f, kPanelRadius);
    }
    // The panel's corner the first tab's side runs down, while it is the one shown, squared here (see Kind::ViewTab),
    // under the inspector's lens, which floats over it.
    const bool firstTab = controller_.layout().view == RoutingView::Graph && shapeEditing() < 0 && partialEditing() < 0;
    if (firstTab)
        c.fillRect({kFxPanel.x, kFxPanel.y, kPanelRadius, kPanelRadius}, kPanel);
}

void View::paintHeader(Canvas &c) {
    const Rect header{0.0f, 0.0f, View::kWidth, kHeaderH};
    c.fillGradient(header, look.headerTop, look.headerBottom);
    c.line({0.0f, kTitleBarH - 0.5f}, {View::kWidth, kTitleBarH - 0.5f}, kBorder.alpha(0.5f), 1.0f);
    c.line({0.0f, kHeaderH - 0.5f}, {View::kWidth, kHeaderH - 0.5f}, kBorder, 1.0f);
    // The name, in the title bar: "FM" in the operators' color, then "Vibe".
    const Rect name{24.0f, kTitleRowY + kTopRowH * 0.5f - 20.0f, 40.0f, 40.0f};
    c.text(name, "FM", kTextLogo, kAccent, Align::Left, true);
    c.text({53.0f, name.y, 100.0f, 40.0f}, "Vibe", kTextLogo, kText, Align::Left, true);
    // The version, faint and at half the name's height, on the name's baseline (text is centered in its box, so the
    // box sits a quarter of the name's height lower).
    const float versionX = 53.0f + c.textWidth("Vibe", kTextLogo, true) + 5.0f;
    c.text({versionX, name.y + 4.0f, 60.0f, 40.0f}, FMVIBE_VERSION, kTextBody, kFaint, Align::Left, true);

    // The output meter: each side's peak in the theme's color up to full scale, red past it, where the clip light
    // stays on for a while after the output went over.
    auto meterY = [](double db) {
        const double at = std::clamp((db - kMeterFloorDb) / (kMeterTopDb - kMeterFloorDb), 0.0, 1.0);
        return kMeter.bottom() - kMeter.h * static_cast<float>(at);
    };
    const float zero = meterY(0.0);
    const bool clipped = meterClipped(telemetry_);
    for (int side = 0; side < 2; ++side) {
        const Rect bar{kMeter.x + side * 9.0f, kMeter.y, 5.0f, kMeter.h};
        c.fillRect({bar.x, zero, bar.w, bar.bottom() - zero}, kTrack, kRadiusMark);
        c.fillRect({bar.x, bar.y, bar.w, zero - 1.0f - bar.y}, kMeterRed.alpha(clipped ? 1.0f : 0.2f), kRadiusMark);
        const float peak = meterPeak(telemetry_, side);
        if (peak <= 0.0f)
            continue;
        const float top = meterY(20.0 * std::log10(peak));
        if (top < bar.bottom())
            c.fillRect({bar.x, std::max(top, zero), bar.w, bar.bottom() - std::max(top, zero)}, kAccent, kRadiusMark);
        if (top < zero - 1.0f)
            c.fillRect({bar.x, top, bar.w, zero - 1.0f - top}, kMeterRed, kRadiusMark);
    }

    // The output scope: the last span the audio thread took (see kOutScopeBins), its level (see followScopeLevel()) at
    // the display's edges (but lifted by no more than 24 dB, so a fading tail shrinks away), over a faint middle line.
    c.fillRect(kOutScope, kDisplayColor, kRadiusControl);
    if (outlined())
        c.strokeRect(kOutScope, kBorder, 1.0f, kRadiusControl);
    const Rect inner{kOutScope.x + 5.0f, kOutScope.y + 4.0f, kOutScope.w - 10.0f, kOutScope.h - 8.0f};
    const float middle = inner.center().y;
    c.line({inner.x, middle}, {inner.right(), middle}, kBorder.alpha(0.6f), 1.0f);
    if (outScopePeak(telemetry_) > kOutScopeFloor) {
        const float level = std::max(outScopeLevel_, outScopePeak(telemetry_));
        const float gain = 0.5f * inner.h / std::max(level, 1.0f / 16.0f);
        std::array<Point, kOutScopeBins> points;
        for (int bin = 0; bin < kOutScopeBins; ++bin)
            points[bin] = {inner.x + inner.w * bin / (kOutScopeBins - 1),
                           middle - std::clamp(telemetry_.outScope[bin], -level, level) * gain};
        plotLine(c, points.data(), points.size(), kAccent, 1.5f);
    }

    for (float x : kDividers)
        c.line({x, kStripY + 14.0f}, {x, kStripY + 50.0f}, kBorder, 1.0f);

    char playing[16];
    std::snprintf(playing, sizeof(playing), "%d", telemetry_.voices);
    c.text({1110.0f, kStripY + 13.0f, 74.0f, 14.0f}, tr(Str::HeadingPlaying), kTextLabel, kDim, Align::Right, true);
    c.text({1110.0f, kStripY + 28.0f, 74.0f, 22.0f}, playing, kTextHeading, telemetry_.voices > 0 ? kText : kFaint, Align::Right,
           true);
}

void View::paintKeyboard(Canvas &c) {
    const float top = keyboardTop();
    if (controller_.layout().keysCollapsed) { // the wheels' names under their handles, with the wheels folded away
        for (const auto &[handle, name] : {std::pair{bendHandle(top), Str::HeadingBend}, std::pair{wheelHandle(top), Str::HeadingMod}})
            c.text({handle.x - 10.0f, handle.bottom() + 1.0f, handle.w + 20.0f, 12.0f}, tr(name), kTextLabel, kFaint,
                   Align::Center, true);
        return;
    }
    const Rect bend = bendWheel(top), mod = modWheel(top);
    c.text({bend.x - 16.0f, bend.bottom() + 4.0f, bend.w + 32.0f, 14.0f}, tr(Str::HeadingBend), kTextLabel, kFaint,
           Align::Center, true);
    c.text({mod.x - 16.0f, mod.bottom() + 4.0f, mod.w + 32.0f, 14.0f}, tr(Str::HeadingMod), kTextLabel, kFaint,
           Align::Center, true);
    const Rect down = octaveButton(-1, top);
    const Rect up = octaveButton(1, top);
    const std::string range = noteName(keyboardBase()) + " \xE2\x80\x93 " + noteName(keyboardBase() + kKeyCount - 1);
    c.text({down.right() + 2.0f, down.y, up.x - down.right() - 4.0f, down.h}, range, kTextLabel, kDim);
}

void View::paintKey(Canvas &c, const Widget &w, bool hovered, bool active) {
    const bool black = isBlackKey(w.a);
    Color color = black ? look.blackKey : look.whiteKey;
    if (hovered)
        color = black ? look.blackKeyHover : look.whiteKeyHover;
    // Lit while held, clicked here or played by the host, easing up and back down (see poll()).
    static_cast<void>(active); // the key clicked here is held in drag_, which poll() follows
    const int key = keyboardBase() + w.a;
    if (key >= 0 && key < 128)
        color = mix(color, kAccent, keyLight_[static_cast<size_t>(key)]);
    c.fillRect(w.rect, color, kRadiusMark);
    c.strokeRect(w.rect, kBorder, 1.0f, kRadiusMark);
    if (!black && w.a % 12 == 0) // label the C keys
        c.text({w.rect.x, w.rect.bottom() - 24.0f, w.rect.w, 16.0f}, noteName(keyboardBase() + w.a), kTextLabel,
               look.keyLabel);
}

void View::paintWheel(Canvas &c, const Widget &w, bool hovered, bool active) {
    const Rect r = w.rect;
    const float inset = 4.0f;
    const float top = r.y + inset;
    const float bottom = r.bottom() - inset;
    c.fillRect(r, kRaised, kRadiusCard);
    if (outlined())
        c.strokeRect(r, hovered || active ? kDim : kBorder, 1.0f, kRadiusCard);
    else if (hovered || active)
        c.fillRect(r, kTrack.alpha(0.5f), kRadiusCard);
    if (w.kind == Kind::ModWheel) {
        // Fills up from the bottom with the wheel's value, in the color of its handle; bipolar, from the middle, where
        // it moves nothing. Where it stands: the param as the editor has it, with what the sequencer's curve adds as it
        // plays.
        const float y = bottom - (bottom - top) * static_cast<float>(std::clamp(value(w.param) + telemetry_.seqWheel, 0.0, 1.0));
        const float from = mods_.sourceBipolar(kWheelSource) ? 0.5f * (top + bottom) : bottom;
        c.fillRect({r.x + inset, std::min(y, from), r.w - 2.0f * inset, std::abs(from - y)},
                   kSourceColors[kWheelSource].alpha(0.6f), kRadiusMark);
        if (from < bottom)
            c.line({r.x + 3.0f, from}, {r.right() - 3.0f, from}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    } else {
        // Springs back to the middle: fills up or down from the center. Where the bend stands: the Pitch Bend param as
        // the editor has it (so a drag moves it at once), with what is added on top (see Telemetry::bend): the host's
        // MIDI bend and modulation of the param, and the sequencer's bend curve.
        const float mid = r.y + r.h * 0.5f;
        const double bend =
            std::clamp(value(paramIndex(GlobalParam::PitchBend)) + telemetry_.bend - telemetry_.bendSet, -1.0, 1.0);
        const float y = mid - static_cast<float>(bend) * (r.h * 0.5f - inset);
        c.fillRect({r.x + inset, std::min(mid, y), r.w - 2.0f * inset, std::abs(y - mid)},
                   kSourceColors[kBendSource].alpha(0.6f), kRadiusMark);
        c.line({r.x + 3.0f, mid}, {r.right() - 3.0f, mid}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    }
}

// What slot `slot` of `source` adds to its target now, as a share of the target's range: a macro's or the mod wheel's
// from its knob, notes playing or not; another source's in the newest voice, while one plays, and on an effect's param
// as the effects ring out after the last one. None while it modulates nothing: bypassed, or its source switched off.
std::optional<double> View::liveSlotOffset(int source, int slot) const {
    if (source < 0 || slot < 0 || mods_.slotBypassed(source, slot) || !mods_.sourceOn(source))
        return std::nullopt;
    const int aux = slotAux([this](int i) { return value(i); }, source, slot);
    if (!runsPerVoice(source) && (aux < 0 || !runsPerVoice(aux))) {
        // What every voice shares; where the newest voice's own sources move its knob, or its aux's, and it is on
        // anything but a macro's knob, with what that voice adds to it.
        const int target = slotTarget(value(slotTargetIndex(source, slot)));
        const bool voiceMoved = voiceMovedMacro(source) || (aux >= 0 && voiceMovedMacro(aux));
        return macroSlotOffsets_[source * kSlots + slot] +
               (voiceMoved && !isMacroKnob(target) && telemetry_.voices > 0 ? telemetry_.slotOffset[source * kSlots + slot]
                                                                            : 0.0f);
    }
    if (telemetry_.voices <= 0) {
        const int target = slotTarget(value(slotTargetIndex(source, slot)));
        const bool effect = (target >= kFxBase && target < kShapeBase) || target == paramIndex(GlobalParam::Volume);
        if (!telemetry_.fxTail || !effect)
            return std::nullopt;
    }
    return telemetry_.slotOffset[source * kSlots + slot];
}

double View::liveAmount(int param) const {
    const ParamDef &def = table_[param];
    const double offset = liveOffset(param);
    return std::clamp((value(param) + offset - def.min) / (def.max - def.min), 0.0, 1.0);
}

double View::liveValue(int param) const {
    const ParamDef &def = table_[param];
    return std::clamp(value(param) + liveOffset(param), def.min, def.max);
}

FilterReads View::filterReads() const {
    return {[this](int index) { return value(index); }, [this](int index) { return liveAmount(index); },
            [this](int param, double set) {
                double low = set, high = set;
                forEachMod(param, [&](const Assignment &m) {
                    if (m.bypassed || !mods_.sourceOn(m.source))
                        return;
                    const auto [down, up] = modReach(m);
                    low += down;
                    high += up;
                });
                return std::pair{std::clamp(low, 0.0, 1.0), std::clamp(high, 0.0, 1.0)};
            }};
}

void View::paintModRange(Canvas &c, Rect bar, int param) {
    const double n = norm(param);
    // The modulation whose depth handle is under the mouse goes last, twice as thick, over the others.
    auto range = [&](const Assignment &m, bool focused) {
        const auto [down, up] = modReach(m);
        const double from = std::clamp(n + down, 0.0, 1.0);
        const double to = std::clamp(n + up, 0.0, 1.0);
        const float alpha = mods_.sourceOn(m.source) && !m.bypassed ? 1.0f : (focused ? 0.4f : 0.3f);
        const float h = focused ? 2.0f * bar.h : bar.h;
        const Color color = kSourceColors[m.source].alpha(alpha);
        c.fillRect({bar.x + static_cast<float>(from) * bar.w, bar.center().y - 0.5f * h,
                    std::max(static_cast<float>(to - from) * bar.w, 1.5f), h},
                   color, kRadiusMark);
        // Where its reach runs past the bar's end, so that it is cut off there, a line across it marks the end.
        const float half = 0.5f * h + 0.75f; // 3 px tall on a plain bar
        if (n + down < 0.0)
            c.line({bar.x, bar.center().y - half}, {bar.x, bar.center().y + half}, color, 1.5f);
        if (n + up > 1.0)
            c.line({bar.right(), bar.center().y - half}, {bar.right(), bar.center().y + half}, color, 1.5f);
    };
    bool any = false;
    std::vector<Assignment> focus;
    forEachMod(param, [&](const Assignment &m) {
        any = true;
        if (modFocused(m))
            focus.push_back(m);
        else
            range(m, false);
    });
    if (!any)
        return;
    for (const Assignment &f : focus)
        range(f, true);
    // Where the set amount is, and where the newest voice (or with no note playing, the macros) takes it now.
    c.fillRect({bar.x + static_cast<float>(n) * bar.w - 0.75f, bar.y - 1.0f, 1.5f, bar.h + 2.0f}, kDim, kRadiusMark);
    if (liveOffset(param) != 0.0f)
        c.fillCircle({bar.x + static_cast<float>(liveAmount(param)) * bar.w, bar.center().y}, 2.2f, kText);
}

// A card's frame, title, display and what goes with them, wherever it shows: in the row, or in the effects graph's
// inspector. Its knobs and buttons are widgets, painted with the others.
void View::paintFxCard(Canvas &c, int slot, Rect card, bool dragged) {
    const FxType type = fx_.slotType(slot);
    const bool on = value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
    const Color fx = fxTypeColor(type); // each type of effect in a color of its own
    paintCardFace(c, card, fx, on, kMaxSources + slot, dragged, 0.45f, kFxPanel);
    // Title row: the effect's icon (a widget of its own, which also switches it) and its name, up to the fold button.
    const float titleRight = fxGraph() ? cardPower(card, false).x - 3.0f : cardTitleRight(card);
    const Rect titleRect{card.x + 33.0f, card.y + kCardTitleY, titleRight - card.x - 33.0f, 20.0f};
    const char *title = fxTitle(type);
    float titleSize = kTextBody; // smaller for a long name, so it fits beside the icon
    while (titleSize > kTextSmall && c.textWidth(title, titleSize, true) > titleRect.w)
        titleSize -= 0.5f;
    c.text(titleRect, title, titleSize, on ? kText : kDim, Align::Left, true);

    const Rect display = fxDisplayIn(card, type);
    c.fillRect(display, kDisplayColor, kRadiusCard);
    switch (type) {
    case FxType::Shaper:
        shapes_.paintShaperCurve(c, display, slot);
        break;
    case FxType::Delay:
        paintDelay(c, display, slot);
        break;
    case FxType::Eq:
        paintEq(c, display, slot);
        break;
    case FxType::ParamEq:
        paintParamEq(c, display, slot);
        break;
    case FxType::Filter:
        paintFilterResponse(c, display, fxFilterParams(slot), filterReads(), telemetry_.fxPitch, fx,
                            &telemetry_.fxSpectrum[slot], telemetry_.age);
        break;
    case FxType::Compressor:
        paintCompressor(c, display, slot);
        break;
    case FxType::MultiDyn:
        paintMultiDyn(c, display, slot);
        break;
    case FxType::Reverb:
        paintReverb(c, display, slot);
        break;
    case FxType::Phaser:
    case FxType::Flanger:
        paintSweep(c, display, slot, type);
        break;
    case FxType::Chorus:
        paintChorus(c, display, slot);
        break;
    case FxType::FreqShifter:
        paintShifter(c, display, slot);
        break;
    case FxType::Disperser:
        paintDisperser(c, display, slot);
        break;
    case FxType::Widener:
        paintWidener(c, display, slot);
        break;
    case FxType::Utility:
        paintUtility(c, display, slot);
        break;
    case FxType::LoFi:
        paintLoFi(c, display, slot);
        break;
    case FxType::Amp: {
        paintAmp(c, display, slot);
        // The cabinet's display beside it, dimmed while the cabinet is off, and the line between the two sides.
        const Rect cab = ampCabDisplay(card);
        c.fillRect(cab, kDisplayColor, kRadiusCard);
        paintAmpCab(c, cab, slot);
        // Dimmed as its controls are (switched off, the whole card's body is dimmed instead, see dimmedAreas()).
        if (on && value(paramIndex(slot, FxParam::AmpCabOn)) < 0.5)
            c.fillRect(cab, cardFace(fx, on).alpha(1.0f - kDimOpacity), kRadiusCard);
        const float x = ampDivider(card);
        c.line({x, display.y}, {x, card.bottom() - 12.0f}, kBorder, 1.0f);
        break;
    }
    case FxType::MultiComp: // retired: a state's loads as a three-band dynamics
    case FxType::None:
    case FxType::Count:
        break;
    }
    if (type == FxType::MultiDyn) { // the band's own knobs above, the shared ones below, under a line named after it
        const bool linked = value(paramIndex(slot, FxParam::MdLink)) >= 0.5;
        const std::string name =
            linked ? std::string(tr(Str::CaptionAllBands)) : trf(Str::CaptionBandKnobs, {tr(kMcBandNames[fxPanel_.band(slot)])});
        const float y = card.y + kMdLineY, width = std::ceil(c.textWidth(name, kTextSmall, true)) + 8.0f;
        c.line({card.x + 14.0f + width, y}, {mdRmsButton(card).x - 6.0f, y}, kBorder, 1.0f);
        c.text({card.x + 14.0f, y - 7.0f, width, 14.0f}, name, kTextSmall, on ? fx : kDim, Align::Left, true);
    }
    // Switched off, its body is dimmed with its widgets (see dimmedAreas()).
}

// The dry signal and the echoes on a timeline: the left side's above the line,
// the right side's below. Ping-pong echoes take turns, each after its side's time.
void View::paintDelay(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 16.0f, r.y + 20.0f, r.w - 22.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const bool synced = at(FxParam::DelaySync) >= 0.5;
    const bool linked = at(FxParam::DelayLink) >= 0.5;
    const double divisions[2] = {at(FxParam::DelayDivisionL), at(linked ? FxParam::DelayDivisionL : FxParam::DelayDivisionR)};
    const double times[2] = {at(FxParam::DelayTimeL), at(linked ? FxParam::DelayTimeL : FxParam::DelayTimeR)};
    double seconds[2];
    for (int side = 0; side < 2; ++side)
        seconds[side] = std::clamp(synced ? divisionSeconds(divisions[side], telemetry_.tempo) : timeFromNorm(times[side]),
                                   kMinTime, kMaxDelay);
    const double feedback = at(FxParam::DelayFeedback);
    const double mix = at(FxParam::DelayMix);
    const bool pingPong = at(FxParam::DelayPingPong) >= 0.5;
    const float width = static_cast<float>(at(FxParam::DelayWidth));
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    c.text({r.x + 4.0f, inner.y, 10.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 10.0f, 12.0f}, "R", kTextLabel, kFaint);

    // Bars reach up for the left side and down for the right; narrower stereo
    // shows as each echo leaking onto the other side.
    auto bar = [&](double t, double level, int side, Color color) {
        const float x = inner.x + 2.0f + static_cast<float>(t) * (inner.w - 4.0f);
        const float h = static_cast<float>(std::min(level, 1.0)) * inner.h * 0.5f;
        const float own = h * (0.5f + 0.5f * width);
        const float other = h * (0.5f - 0.5f * width);
        const float up = side == 0 ? own : other;
        const float down = side == 0 ? other : own;
        c.fillRect({x - 1.5f, mid - up, 3.0f, up + down}, color, kRadiusMark);
    };
    bar(0.0, delayDryLevel(mix), 0, kDim);
    bar(0.0, delayDryLevel(mix), 1, kDim);
    const double span = 6.0 * std::max(seconds[0], seconds[1]);
    double level = delayWetLevel(mix);
    double pingPongTime = 0.0;
    for (int n = 1; n <= 16 && level > 0.01; ++n, level *= feedback) {
        auto echo = [&](int side, double t) {
            if (t <= span)
                bar(t / span, level, side, fx.alpha(0.35f + 0.65f * static_cast<float>(std::min(level, 1.0))));
        };
        if (pingPong) {
            const int side = (n - 1) % 2; // starts on the left, then crosses over
            pingPongTime += seconds[side];
            echo(side, pingPongTime);
        } else {
            echo(0, n * seconds[0]);
            echo(1, n * seconds[1]);
        }
    }

    // "1/8D | 1/4" or "375 ms | 500 ms"; linked, just "1/8D".
    char sides[2][32], text[80];
    for (int side = 0; side < 2; ++side) {
        if (synced)
            std::snprintf(sides[side], sizeof(sides[side]), "%s", divisionName(static_cast<int>(std::lround(divisions[side]))));
        else
            formatParam(paramIndex(slot, FxParam::DelayTimeL), normFromTime(seconds[side]), sides[side],
                        sizeof(sides[side]));
    }
    if (linked)
        std::snprintf(text, sizeof(text), "%s", sides[0]);
    else
        std::snprintf(text, sizeof(text), "%s  |  %s", sides[0], sides[1]);
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
    if (synced) {
        char bpm[16];
        std::snprintf(bpm, sizeof(bpm), "%.0f bpm", telemetry_.tempo);
        c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, bpm, kTextLabel, kFaint, Align::Right);
    }
}

// The bands' bells and what those that play add up to, on a log frequency axis.
Point View::peqHandle(Rect area, int slot, int band) const {
    // A cut's stands at its resonance's peak, and a notch's on the 0 dB line.
    const PeqType type = peqType(value(peqTypeIndex(slot, band)));
    const double db = isCut(type)                ? 20.0 * std::log10(qFromNorm(value(peqQIndex(slot, band))))
                      : type == PeqType::Notch ? 0.0
                                               : value(peqGainIndex(slot, band));
    return peqAt(area, value(peqFreqIndex(slot, band)), db);
}

void View::paintParamEq(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect area = r.inset(8.0f);
    for (double db : {-12.0, 0.0, 12.0})
        c.line(peqAt(area, 0.0, db), peqAt(area, 1.0, db), kBorder.alpha(db == 0.0 ? 1.0f : 0.5f), 1.0f, db != 0.0);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = area.x + static_cast<float>(normFromCutoff(hz)) * area.w;
        c.line({x, area.y}, {x, area.bottom()}, kBorder.alpha(0.5f), 1.0f, true);
    }
    paintSpectrum(c, area, telemetry_.fxSpectrum[slot], telemetry_.age);
    auto on = [&](int band) { return value(peqOnIndex(slot, band)) >= 0.5; };
    // The bands as modulation takes them now; their handles stay where they are set.
    auto live = [&](int param) { return liveValue(param); };
    auto curve = [&](int only) { // only one band, or every band that plays (-1)
        std::vector<double> key = curveKey(Curve::ParamEq, area);
        key.push_back(only);
        for (int band = 0; band < kPeqBands; ++band)
            key.insert(key.end(), {value(peqTypeIndex(slot, band)), live(peqFreqIndex(slot, band)),
                                   live(peqGainIndex(slot, band)), live(peqQIndex(slot, band)), on(band) ? 1.0 : 0.0});
        return curveCache().get(key, [&] {
            std::vector<Point> points;
            for (int i = 0; i <= 120; ++i) {
                const double x = i / 120.0;
                double magnitude = 1.0;
                for (int band = 0; band < kPeqBands; ++band)
                    if (only == band || (only < 0 && on(band)))
                        magnitude *= peqResponse(cutoffFromNorm(x), peqType(value(peqTypeIndex(slot, band))),
                                                 live(peqFreqIndex(slot, band)), live(peqGainIndex(slot, band)),
                                                 live(peqQIndex(slot, band)));
                points.push_back(peqAt(area, x, 20.0 * std::log10(std::max(magnitude, 1e-6))));
            }
            return points;
        });
    };
    // A cut falls far below the display, so the curves are cut off at its edge.
    c.pushClip(r);
    for (int band = 0; band < kPeqBands; ++band) {
        if (!on(band))
            continue;
        const std::vector<Point> points = curve(band);
        c.polyline(points.data(), points.size(), kSourceColors[band].alpha(0.35f), 1.0f);
    }
    std::vector<Point> points = curve(-1);
    std::vector<Point> area0 = points;
    area0.push_back(peqAt(area, 1.0, 0.0));
    area0.push_back(peqAt(area, 0.0, 0.0));
    c.fillPolygon(area0.data(), area0.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    c.popClip();
}

// The EQ's response, with the crossovers marked.
// A name down a folded card's strip, a letter a line, from the top of `r`; a number ending it ("LFO 13") on a line
// of its own, across.
void View::paintEq(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 8.0f, r.w - 16.0f, r.h - 24.0f};
    constexpr float kMinDb = -30.0f;
    constexpr float kMaxDb = 9.0f;
    auto yOfDb = [&](float db) {
        return inner.bottom() - (std::clamp(db, kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb) * inner.h;
    };
    auto xOfHz = [&](double hz) { return inner.x + static_cast<float>(normFromCutoff(hz)) * inner.w; };
    c.line({inner.x, yOfDb(0.0f)}, {inner.right(), yOfDb(0.0f)}, kBorder, 1.0f);
    for (double hz : {kEqLowHz, kEqHighHz})
        c.line({xOfHz(hz), inner.y}, {xOfHz(hz), inner.bottom()}, kBorder.alpha(0.8f), 1.0f, true);

    const double low = liveValue(paramIndex(slot, FxParam::EqLow));
    const double mid = liveValue(paramIndex(slot, FxParam::EqMid));
    const double high = liveValue(paramIndex(slot, FxParam::EqHigh));
    std::vector<double> key = curveKey(Curve::Eq, inner);
    key.insert(key.end(), {low, mid, high});
    const std::vector<Point> points = curveCache().get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 120; ++i) {
            const double x = i / 120.0;
            const double magnitude = eqResponse(cutoffFromNorm(x), low, mid, high);
            const float db = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-6)));
            points.push_back({inner.x + static_cast<float>(x) * inner.w, yOfDb(db)});
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);

    const float labelY = inner.bottom() + 2.0f;
    const float edges[] = {inner.x, xOfHz(kEqLowHz), xOfHz(kEqHighHz), inner.right()};
    for (int b = 0; b < 3; ++b)
        c.text({edges[b], labelY, edges[b + 1] - edges[b], 12.0f}, tr(kMcBandNames[b]), kTextLabel, kFaint, Align::Center,
               true);
}

// Static curves, input level across and output up, from -60 dB to 0, with the
// gain reduction as a meter on the right; the three-band one draws a curve per
// band, the one its card shows on top.
namespace {

constexpr float kCompRangeDb = 60.0f;
constexpr float kMeterRangeDb = 24.0f;

void compressorPlot(Canvas &c, Rect plot, double threshold, double ratio, double knee, Color color, float width) {
    std::vector<Point> points;
    for (int i = 0; i <= 60; ++i) {
        const double in = -kCompRangeDb + i * kCompRangeDb / 60.0;
        const double out = std::max(compressorCurve(in, threshold, ratio, knee), static_cast<double>(-kCompRangeDb));
        points.push_back({plot.x + static_cast<float>((in + kCompRangeDb) / kCompRangeDb) * plot.w,
                          plot.bottom() - static_cast<float>((out + kCompRangeDb) / kCompRangeDb) * plot.h});
    }
    if (width > 1.0f) // the curve shown, rather than a faint one behind it
        plotLine(c, points.data(), points.size(), color, width);
    else
        c.polyline(points.data(), points.size(), color, width);
}

// A downward bar for `db` of gain reduction.
void reductionMeter(Canvas &c, Rect meter, float db, Color color) {
    c.fillRect(meter, meterWell(), kRadiusMark);
    const float h = std::clamp(db / kMeterRangeDb, 0.0f, 1.0f) * meter.h;
    if (h > 0.5f)
        c.fillRect({meter.x, meter.y, meter.w, h}, color, kRadiusMark);
}

} // namespace

void View::paintCompressor(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect plot{r.x + 8.0f, r.y + 8.0f, r.w - 34.0f, r.h - 16.0f};
    c.line({plot.x, plot.bottom()}, {plot.right(), plot.y}, kBorder, 1.0f, true); // no compression
    const double threshold = liveValue(paramIndex(slot, FxParam::CompThreshold));
    const float tx = plot.x + static_cast<float>((threshold + kCompRangeDb) / kCompRangeDb) * plot.w;
    c.line({tx, plot.y}, {tx, plot.bottom()}, fx.alpha(0.3f), 1.0f, true);
    compressorPlot(c, plot, threshold, liveValue(paramIndex(slot, FxParam::CompRatio)),
                   liveValue(paramIndex(slot, FxParam::CompKnee)), fx, 2.0f);
    const float reduction = telemetry_.fxReduction[slot][0];
    reductionMeter(c, {r.right() - 18.0f, plot.y, 8.0f, plot.h}, reduction, fx);
    // Resting, the compressor reports its reduction as -0, which would read "-0.0".
    const bool reducing = reduction > 0.05f;
    c.text({plot.x + 4.0f, plot.y + 2.0f, plot.w, 14.0f},
           trf(Str::CaptionReduction, {number("%.1f", reducing ? reduction : 0.0f)}), kTextLabel,
           reducing ? kText : kFaint, Align::Left);
}

// The three-band dynamics: a bar a band, the high band on top, levels across. Left of the Below threshold, where the
// band lifts, and right of the Above one, where it pulls down, the bar is tinted (more under the mouse, as it drags
// for the ratio), and lines show the ratio: one for every 6 dB that goes in, where it comes out. Down the bar's middle
// runs a meter, half its height: up to what comes out in the card's color, then the gain, amber where the band takes
// some off (up to what went in) and in the accent where it adds some, with a tick across the bar where the input is.
// Its name is on the left (on a plate for the band its knobs set), and on the right its gain, or the threshold or
// ratio a line or zone sets while it is under the mouse or dragged.
void View::paintMultiDyn(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const bool linked = value(paramIndex(slot, FxParam::MdLink)) >= 0.5;
    const int shown = fxPanel_.band(slot);
    // The threshold line or zone under the mouse or held, which shows its threshold or ratio in its band's readout
    // (linked, in every band's, since the edit sets them all).
    const Widget *grip = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (grip && ((grip->kind != Kind::DynGrip && grip->kind != Kind::DynZone) || grip->a != slot))
        grip = nullptr;
    const Color taken = fxTypeColor(FxType::Eq), added = kAccent; // the DJ EQ's amber, in every theme
    for (int band = 0; band < kMcBands; ++band) {
        const bool set = linked || band == shown; // the band the knobs set; linked, every band, which edits go to
        const Rect bar = mdBar(r, band);
        const Rect row = mdBandRow(r, band);
        const bool rowHovered = hover_ && hover_->kind == Kind::DynBand && hover_->a == slot && hover_->b == band &&
                                drag_.kind == DragKind::None;
        const Rect name{row.x + 1.0f, bar.y - 1.0f, bar.x - row.x - 6.0f, bar.h + 2.0f};
        if (band == shown)
            c.fillRect(name, kRaised, kRadiusMark);
        c.text(name, tr(kMcBandNames[band]), kTextSmall, set || rowHovered ? kText : kDim, Align::Center, true);

        const float alpha = set ? 1.0f : 0.5f;
        c.fillRect(bar, meterWell(), kRadiusMark);
        const double above = value(mdAboveIndex(slot, band)), below = std::min(value(mdBelowIndex(slot, band)), above);
        const float xBelow = mdBarX(bar, below), xAbove = mdBarX(bar, above);
        // Its zones and ratio lines where modulation takes them now; the threshold lines, its grips, stay where set.
        const double liveAbove = liveValue(mdAboveIndex(slot, band));
        const double liveBelow = std::min(liveValue(mdBelowIndex(slot, band)), liveAbove);
        const float zoneBelow = mdBarX(bar, liveBelow), zoneAbove = mdBarX(bar, liveAbove);
        // The zones, faintly, the one under the mouse or dragged more.
        auto zoneLit = [&](bool lower) {
            return grip && grip->kind == Kind::DynZone && grip->b == 2 * band + (lower ? 1 : 0);
        };
        c.fillRect({bar.x, bar.y, zoneBelow - bar.x, bar.h}, fx.alpha((zoneLit(true) ? 0.2f : 0.08f) * alpha), 0.0f);
        c.fillRect({zoneAbove, bar.y, bar.right() - zoneAbove, bar.h},
                   fx.alpha((zoneLit(false) ? 0.2f : 0.08f) * alpha), 0.0f);
        // The meter, once the band has heard something.
        const float level = telemetry_.fxBandLevel[slot][band];
        double change = 0.0;
        float xIn = -1.0f;
        if (level > 1e-6f) {
            const double inDb = 20.0 * std::log10(level);
            change = telemetry_.fxBandGain[slot][band]; // the dynamics alone: the band's Gain comes after them
            xIn = mdBarX(bar, inDb);
            const float xOut = mdBarX(bar, inDb + change), y = bar.center().y - 0.5f * kMdMeterH;
            const float lo = std::min(xIn, xOut), hi = std::max(xIn, xOut);
            c.fillRect({bar.x, y, lo - bar.x, kMdMeterH}, fx.alpha(alpha), 0.0f);
            if (hi > lo)
                c.fillRect({lo, y, hi - lo, kMdMeterH}, (xOut < xIn ? taken : added).alpha(0.9f * alpha), 0.0f);
        }
        // In the zones, lines that move with their threshold, out to the bar's end: one for every 6 dB that goes in
        // past the threshold, drawn where the ratio takes it, so 6 dB apart at 1:1 and closing up evenly toward the
        // threshold as the ratio turns up (its stored value is the share of the distance it takes away). As a
        // threshold moves toward the bar's end, its lines go out past it; moving back, more come in, kMdRatioLines at
        // most. Past about 15:1 they stay 0.4 dB apart, up to infinity. The ratio alone: Range and the knee are left
        // out.
        auto ratioLines = [&](double threshold, double norm, double toward) {
            const double step = std::max(6.0 * (1.0 - norm), 0.4);
            for (int n = 1; n <= kMdRatioLines; ++n) {
                const double db = threshold + toward * step * n;
                if (toward > 0.0 ? db > 0.0 : db < -kMdRangeDb)
                    break;
                const float x = mdBarX(bar, db);
                c.line({x, bar.y}, {x, bar.bottom()}, kText.alpha(0.5f * alpha), 0.8f);
            }
        };
        ratioLines(liveAbove, liveValue(mdDownIndex(slot, band)), 1.0);
        ratioLines(liveBelow, liveValue(mdUpIndex(slot, band)), -1.0);
        if (xIn >= 0.0f) // where the input is, across the bar
            c.line({xIn, bar.y - 1.5f}, {xIn, bar.bottom() + 1.5f}, kText.alpha(alpha), 1.0f);
        for (float x : {xBelow, xAbove})
            c.line({x, bar.y - 2.0f}, {x, bar.bottom() + 2.0f}, fx.alpha(alpha), 1.2f);

        // The readout: the band's gain, or the threshold or ratio its line or zone sets.
        const Rect readout{bar.right() + 2.0f, bar.y - 1.0f, row.right() - bar.right() - 4.0f, bar.h + 2.0f};
        std::string text;
        Color ink = set ? kText : kDim;
        if (grip && (linked || grip->b / 2 == band)) { // linked, every band's, which the edit sets too
            const bool ratio = grip->kind == Kind::DynZone || drag_.kind == DragKind::Value; // a zone, or a line Alt-dragged
            const bool lower = grip->b % 2 == 1; // the Below threshold's line or the upward zone, with the Up ratio
            const int ratioParam = lower ? mdUpIndex(slot, band) : mdDownIndex(slot, band);
            char ratioText[32];
            formatParam(ratioParam, value(ratioParam), ratioText, sizeof(ratioText));
            const int threshold = mdThreshold(slot, 2 * band + (lower ? 1 : 0));
            text = ratio ? std::string(ratioText) : number("%.0f dB", value(threshold));
            ink = fx;
        } else {
            text = std::abs(change) < 0.05 ? "0.0" : number("%+.1f", change);
        }
        c.text(readout, text, kTextSmall, ink, Align::Right, set);
    }
    // The scale under the bars.
    const Rect bar = mdBar(r, 0);
    const float y = r.bottom() - 11.5f;
    const int bottom = -static_cast<int>(kMdRangeDb);
    for (int db = bottom; db <= 0; db += 20) {
        const float x = mdBarX(bar, db);
        const Align align = db == bottom ? Align::Left : (db == 0 ? Align::Right : Align::Center);
        const float left = align == Align::Left ? x : (align == Align::Right ? x - 24.0f : x - 12.0f);
        c.text({left, y, 24.0f, 11.0f}, std::to_string(db), kTextSmall, kDim, align);
    }
}

// The tail's level in dB across the height, so it falls in a straight line to nothing at the decay
// time, after the pre-delay; the dry sound stands at the start. Their heights follow the mix.
// The reverb in time: the dry sound at the start, then after the pre-delay a build-up to the full tail, which
// dies away over the decay. A bigger space builds up more slowly (shown stretched, so Size reads), with its
// early reflections spread wider, and its tail is sparser. A room's early reflections are distinct, a plate
// is dense from the start with none, and a shimmer's tail sparkles above it.
void View::paintReverb(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const ReverbType type = reverbType(at(FxParam::ReverbType));
    const double space = reverbSpace(at(FxParam::ReverbSize), type); // the lines' length, 0.11 to 1
    const double decay = reverbDecay(at(FxParam::ReverbDecay));
    const double pre = reverbPreDelay(at(FxParam::ReverbPreDelay));
    const double build = decay * (type == ReverbType::Plate ? 0.02 : 0.03 + 0.2 * space);
    const double span = 1.15 * (pre + decay);
    const float wet = static_cast<float>(delayWetLevel(at(FxParam::ReverbMix)));
    const float full = wet * 0.5f * inner.h;
    auto x = [&](double seconds) { return inner.x + static_cast<float>(seconds / span) * inner.w; };
    auto level = [&](double t) { // 0..1: rising over the build-up, then falling to nothing at the decay
        const double since = t - pre;
        if (since <= 0.0)
            return 0.0f;
        if (since < build)
            return static_cast<float>(since / build);
        return static_cast<float>(std::max(0.0, 1.0 - (since - build) / std::max(decay - build, 1e-3)));
    };
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    // A made-up texture of reflections under the outline: sparser in a bigger space, densest in a plate, and
    // darker with damping.
    const float damping = static_cast<float>(at(FxParam::ReverbDamping));
    const int count = type == ReverbType::Plate ? 120 : static_cast<int>(std::lround(130.0 - 90.0 * space));
    uint32_t rng = 0x9E3779B9u;
    auto random = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    for (int i = 0; i < count; ++i) {
        const double t = pre + build * 0.4 + (decay - build * 0.4) * i / count;
        const float h = level(t) * full * (0.35f + 0.65f * random());
        c.line({x(t), mid - h}, {x(t), mid + h}, fx.alpha(0.55f - 0.3f * damping), 1.0f);
    }
    // The early reflections over the build-up, as far apart as the room's (the others' first echoes from
    // their lines, fainter).
    if (type != ReverbType::Plate) {
        const double last = kRoomEchoSeconds[std::size(kRoomEchoSeconds) - 1];
        for (size_t e = 0; e < std::size(kRoomEchoSeconds); ++e) {
            const double t = pre + build * 0.9 * kRoomEchoSeconds[e] / last;
            const float h = full * static_cast<float>(kRoomEchoLevels[e]) * (type == ReverbType::Room ? 1.5f : 0.8f);
            c.line({x(t), mid - h}, {x(t), mid + h}, fx.alpha(0.9f), 1.5f);
        }
    }
    // A shimmer's shifted tail in sparks off the outline, above it for a shift up and below it for one down: each
    // step of the interval later, further out and fainter, the more so the less it shifts. The shifted tail goes
    // back into the reverb and builds up as the tail does, so each step comes to the fore about a fifth of the
    // decay after the last.
    const double interval = at(FxParam::ReverbShimmerPitch);
    const float amount = static_cast<float>(at(FxParam::ReverbShimmer));
    if (type == ReverbType::Shimmer && std::abs(interval) >= 0.5 && amount > 0.0f) {
        const double round = 0.2 * decay;
        const float side = interval > 0.0 ? -1.0f : 1.0f;
        for (int step = 1; step <= 3; ++step) {
            const double from = pre + step * round;
            const float strength = std::pow(amount, 0.5f * static_cast<float>(step));
            for (int k = 0; k < 14; ++k) {
                const double t = from + (pre + decay - from) * random();
                const float y = mid + side * (level(t) * full * (0.6f + 0.4f * strength) + 4.0f * step + 3.0f * random());
                c.fillCircle({x(t), std::clamp(y, inner.y, inner.bottom())}, 1.2f, fx.alpha(0.3f + 0.6f * strength));
            }
        }
    }
    const Point outline[] = {{x(pre), mid}, {x(pre + build), mid - full}, {x(pre + decay), mid},
                             {x(pre + build), mid + full}, {x(pre), mid}};
    plotLine(c, outline, 5, fx, 1.5f);
    const float dry = static_cast<float>(delayDryLevel(at(FxParam::ReverbMix))) * 0.5f * inner.h;
    c.fillRect({inner.x - 1.0f, mid - dry, 3.0f, 2.0f * dry}, kDim, kRadiusMark);

    char decayText[32], preText[32];
    formatParam(paramIndex(slot, FxParam::ReverbDecay), at(FxParam::ReverbDecay), decayText, sizeof(decayText));
    formatParam(paramIndex(slot, FxParam::ReverbPreDelay), at(FxParam::ReverbPreDelay), preText, sizeof(preText));
    std::string text = pre > 0.0 ? trf(Str::CaptionPreDelay, {decayText, preText}) : std::string(decayText);
    if (type == ReverbType::Shimmer) { // the interval it climbs by (or falls) in place of the pre-delay
        char pitchText[32];
        formatParam(paramIndex(slot, FxParam::ReverbShimmerPitch), interval, pitchText, sizeof(pitchText));
        text = trf(Str::CaptionShimmer, {decayText, pitchText});
    }
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
}

void View::paintSweep(Canvas &c, Rect r, int slot, FxType type) {
    const Color fx = fxColor(slot);
    const bool phaser = type == FxType::Phaser;
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 28.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const double feedback = at(phaser ? FxParam::PhaserFeedback : FxParam::FlangerFeedback);
    const double mix = sweepWet(at(phaser ? FxParam::PhaserMix : FxParam::FlangerMix));
    const double depth = at(phaser ? FxParam::PhaserDepth : FxParam::FlangerDepth);
    const int stages = static_cast<int>(std::lround(at(FxParam::PhaserStages)));
    const double polarity = sweepPolarity(at(phaser ? FxParam::PhaserNegative : FxParam::FlangerNegative));
    constexpr float kMinDb = -30.0f, kMaxDb = 9.0f;
    auto yOfDb = [&](double db) {
        return inner.bottom() - (std::clamp(static_cast<float>(db), kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb) * inner.h;
    };
    c.line({inner.x, yOfDb(0.0)}, {inner.right(), yOfDb(0.0)}, kBorder, 1.0f);
    // The response at both ends of the sweep faintly, and bright where its LFO is now (moved on from the
    // last snapshot at its rate), or in the middle while no audio runs. A stereo sweep's right side, a
    // quarter cycle ahead, is the thinner of the two bright curves.
    const bool running = telemetry_.age < 0.5f;
    const double rateHz = sweepHz([this](int i) { return liveValue(i); }, slot, type, telemetry_.tempo);
    const double phase = telemetry_.fxPhases[slot] + rateHz * std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    const bool stereo = at(phaser ? FxParam::PhaserStereo : FxParam::FlangerStereo) >= 0.5;
    struct Response {
        double lfo;
        float alpha, width;
    };
    std::vector<Response> curves = {{-1.0, 0.3f, 1.0f}, {1.0, 0.3f, 1.0f}};
    if (stereo && running)
        curves.push_back({std::sin(2.0 * std::numbers::pi * (phase + 0.25)), 0.7f, 1.2f});
    curves.push_back({running ? std::sin(2.0 * std::numbers::pi * phase) : 0.0, 1.0f, 1.8f});
    for (const Response &curve : curves) {
        std::vector<double> key = curveKey(Curve::Sweep, inner);
        key.insert(key.end(), {phaser ? 1.0 : 0.0, curve.lfo, feedback, mix, depth, static_cast<double>(stages),
                               at(phaser ? FxParam::PhaserCenter : FxParam::FlangerDelay), polarity});
        const std::vector<Point> points = curveCache().get(key, [&] {
            std::vector<Point> points;
            for (int i = 0; i <= 160; ++i) {
                const double x = i / 160.0;
                const double hz = cutoffFromNorm(x);
                const double lfo = curve.lfo;
                const double magnitude =
                    phaser
                        ? phaserResponse(hz, phaserCorner(at(FxParam::PhaserCenter), depth, lfo), stages, feedback,
                                         mix, polarity)
                        : flangerResponse(hz, flangerDelay(at(FxParam::FlangerDelay), depth, lfo), feedback, mix,
                                          polarity);
                points.push_back({inner.x + static_cast<float>(x) * inner.w,
                                  yOfDb(20.0 * std::log10(std::max(magnitude, 1e-6)))});
            }
            return points;
        });
        if (curve.width > 1.5f)
            plotLine(c, points.data(), points.size(), fx, curve.width);
        else
            c.polyline(points.data(), points.size(), fx.alpha(curve.alpha), curve.width);
    }
    char rate[32], shape[32], text[80];
    const bool synced = at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync) >= 0.5;
    const int rateParam = paramIndex(slot, synced ? (phaser ? FxParam::PhaserDivision : FxParam::FlangerDivision)
                                                  : (phaser ? FxParam::PhaserRate : FxParam::FlangerRate));
    formatParam(rateParam, liveValue(rateParam), rate, sizeof(rate));
    if (phaser)
        std::snprintf(shape, sizeof(shape), "%s", trf(Str::CaptionStages, {std::to_string(stages)}).c_str());
    else
        formatParam(paramIndex(slot, FxParam::FlangerDelay), at(FxParam::FlangerDelay), shape, sizeof(shape));
    std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", shape, rate);
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
}

// The chorus's voices' delays over a cycle of its sweep, each a lane about its own place along the delays (the dashed
// middle line Delay), as far either way as Depth swings it, a share of the cycle behind the one before; the first
// voice brightest, the others fainter. While the audio runs, a dot on each lane where its sweep is now. (Their slow
// drifts aren't drawn.)
void View::paintChorus(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner = chorusLanes(r);
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const int voices = static_cast<int>(std::clamp(std::lround(at(FxParam::ChorusVoices)),
                                                   static_cast<long>(kMinChorusVoices),
                                                   static_cast<long>(kMaxChorusVoices)));
    const double depth = at(FxParam::ChorusDepth);
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f, true);
    const bool running = telemetry_.age < 0.5f;
    const double rateHz = sweepHz([this](int i) { return liveValue(i); }, slot, FxType::Chorus, telemetry_.tempo);
    double phase = telemetry_.fxPhases[slot] + rateHz * std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    phase -= std::floor(phase);
    constexpr float kLaneAlpha[kMaxChorusVoices] = {1.0f, 0.75f, 0.6f, 0.48f, 0.38f, 0.3f};
    auto yAt = [&](double cycle, int voice) { return chorusLaneY(inner, voice, voices, depth, cycle); };
    for (int voice = voices - 1; voice >= 0; --voice) { // the first over the others
        std::vector<Point> lane;
        for (int i = 0; i <= 64; ++i) {
            const double x = i / 64.0;
            lane.push_back({inner.x + static_cast<float>(x) * inner.w, yAt(x, voice)});
        }
        if (voice == 0)
            plotLine(c, lane.data(), lane.size(), fx, 1.8f);
        else
            c.polyline(lane.data(), lane.size(), fx.alpha(kLaneAlpha[voice]), 1.4f);
        if (running)
            c.fillCircle({inner.x + static_cast<float>(phase) * inner.w, yAt(phase, voice)}, 2.5f,
                         voice == 0 ? fx : fx.alpha(kLaneAlpha[voice]));
    }
    char rate[32], text[80];
    const int rateParam = paramIndex(slot, FxParam::ChorusRate);
    formatParam(rateParam, liveValue(rateParam), rate, sizeof(rate));
    std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", trf(Str::CaptionVoices, {std::to_string(voices)}).c_str(),
                  rate);
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
}

// The partials of a 220 Hz tone, faint, and where the shifter moves them: the left side above the
// line, the right below, on a log axis from 50 Hz to 10 kHz.
// The disperser's delay across frequency: how much later than the rest each part of the sound comes out,
// rising to its peak at Freq (where Key Track moves it with the note), taller and narrower with more stages and more
// pinch, and coming again across the spectrum when stretched. Behind it, on the same axis, what it puts out, measured.
void View::paintDisperser(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 28.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const int stages = static_cast<int>(std::lround(at(FxParam::DisperserStages)));
    constexpr double kRate = 48000.0;
    const double center = disperserCenter(at(FxParam::DisperserFreq), at(FxParam::DisperserKeyTrack),
                                          telemetry_.fxPitch);
    const int stretch = disperserStretch(at(FxParam::DisperserStretch), kRate);
    // Finer across where it is stretched, so each copy's narrow peak is drawn.
    const int steps = 160 * std::min(stretch, 4);
    std::vector<double> delays;
    double most = 1e-4;
    for (int i = 0; i <= steps; ++i) {
        delays.push_back(disperserDelay(cutoffFromNorm(static_cast<double>(i) / steps), center,
                                        at(FxParam::DisperserPinch), stages, kRate, stretch));
        most = std::max(most, delays.back());
    }
    std::vector<Point> points;
    for (int i = 0; i <= steps; ++i)
        points.push_back({inner.x + inner.w * static_cast<float>(i) / static_cast<float>(steps),
                          inner.bottom() - static_cast<float>(delays[i] / (most * 1.1)) * inner.h});
    paintSpectrum(c, inner, telemetry_.fxSpectrum[slot], telemetry_.age);
    c.line({inner.x, inner.bottom()}, {inner.right(), inner.bottom()}, kBorder, 1.0f);
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f * static_cast<float>(at(FxParam::DisperserMix)) + 0.04f));
    plotLine(c, points.data(), points.size(), fx, 1.8f);
    // Where it centers, as Freq's text reads (moved by Key Track), and how late its peak comes.
    char freq[32], text[80];
    const int freqParam = paramIndex(slot, FxParam::DisperserFreq);
    formatParam(freqParam, std::clamp(normFromCutoff(center), 0.0, 1.0), freq, sizeof(freq));
    std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionDispersion, {freq, number("%.1f", most * 1000.0)}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
}

// The Lo-Fi's steps: two cycles of a sine (faint) and what it makes of it, held between the samples Rate takes (the
// sine a fiftieth of Rate, as a 240 Hz one is at 12 kHz) at the levels Bits leaves, smoothed as far as Smooth takes
// it. Over it, how many bits and the rate.
void View::paintLoFi(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 22.0f, r.w - 16.0f, r.h - 30.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const LoFiSettings set = loFiSettings(at(FxParam::LoFiBits), at(FxParam::LoFiRate), 48000.0);
    const double smoothing = at(FxParam::LoFiSmooth);
    constexpr int kSamples = 24; // across the display: two cycles of a twelfth of the rate's samples each, at most
    const double perCycle = std::clamp(set.rateHz / 240.0, 2.0, 400.0); // samples a cycle of the sine
    const double cycles = std::min(2.0, kSamples / perCycle * 2.0);
    auto y = [&](double v) { return inner.center().y - static_cast<float>(v) * 0.45f * inner.h; };
    std::vector<Point> sine, steps;
    for (int i = 0; i <= 96; ++i) {
        const double t = i / 96.0;
        sine.push_back({inner.x + static_cast<float>(t) * inner.w, y(0.9 * std::sin(2.0 * std::numbers::pi * cycles * t))});
    }
    plotLine(c, sine.data(), sine.size(), fx.alpha(0.3f), 1.2f);
    // Held samples, drawn as steps, their corners eased as far as Smooth takes them toward the smoothed line.
    const int held = std::max(1, static_cast<int>(std::lround(perCycle * cycles)));
    for (int k = 0; k <= held; ++k) {
        const double t0 = static_cast<double>(k) / held, t1 = std::min(1.0, static_cast<double>(k + 1) / held);
        const double v = loFiLevel(0.9 * std::sin(2.0 * std::numbers::pi * cycles * t0), set.step);
        const float x0 = inner.x + static_cast<float>(t0) * inner.w, x1 = inner.x + static_cast<float>(t1) * inner.w;
        const float eased = static_cast<float>(smoothing) * (x1 - x0) * 0.5f;
        if (k > 0)
            steps.push_back({x0 + eased, y(v)});
        else
            steps.push_back({x0, y(v)});
        if (k < held)
            steps.push_back({x1 - eased, y(v)});
    }
    plotLine(c, steps.data(), steps.size(), fx, 1.8f);
    char rate[32], text[80];
    formatParam(paramIndex(slot, FxParam::LoFiRate), at(FxParam::LoFiRate), rate, sizeof(rate));
    std::snprintf(text, sizeof(text), "%s",
                  trf(Str::CaptionLoFi, {std::to_string(static_cast<int>(std::lround(at(FxParam::LoFiBits)))), rate})
                      .c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
}

// The widener's picture of the stereo field: the sound's place across, left to right, and its frequency up, from
// 20 Hz at the bottom to 20 kHz at the top. The dry sound stays in the middle; above the high-pass, the taps
// spread it out to the sides as far as Width and Mix take it, wavering with their sweep while the audio runs.
// What Width leaves in the middle, a chorus, thickens the middle line.
void View::paintWidener(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 18.0f, r.y + 20.0f, r.w - 36.0f, r.h - 26.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const double size = at(FxParam::WidenerSize), width = at(FxParam::WidenerWidth), mix = at(FxParam::WidenerMix);
    const double highPass = at(FxParam::WidenerHighPass);
    const float mid = inner.center().x;
    auto yOf = [&](double norm) { return inner.bottom() - static_cast<float>(norm) * inner.h; };
    const bool running = telemetry_.age < 0.5f;
    const double phase =
        telemetry_.fxPhases[slot] + kWidenerRates[0] * std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    std::vector<Point> left, right, chorus;
    for (int i = 0; i <= 64; ++i) {
        const double y = i / 64.0;
        const double spread = widenerSpread(cutoffFromNorm(y), highPass);
        const double waver = running ? 1.0 + 0.08 * (0.3 + size) * std::sin(2.0 * std::numbers::pi * (phase + 2.5 * y))
                                     : 1.0;
        // On a square-root curve of the mix, so half of it already shows wide.
        const float half = 0.5f * inner.w * static_cast<float>(std::min(spread * width * std::sqrt(mix) * waver, 1.0));
        left.push_back({mid - half, yOf(y)});
        right.push_back({mid + half, yOf(y)});
        chorus.push_back({mid + 0.5f + 5.0f * static_cast<float>(spread * (1.0 - width) * std::sqrt(mix)), yOf(y)});
    }
    std::vector<Point> area = left;
    area.insert(area.end(), right.rbegin(), right.rend());
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.16f));
    std::vector<Point> thick = chorus;
    for (auto it = chorus.rbegin(); it != chorus.rend(); ++it)
        thick.push_back({2.0f * mid - it->x, it->y});
    c.fillPolygon(thick.data(), thick.size(), fx.alpha(0.5f));
    c.line({mid, inner.y}, {mid, inner.bottom()}, kDim, 1.0f);
    plotLine(c, left.data(), left.size(), fx, 1.6f);
    plotLine(c, right.data(), right.size(), fx, 1.6f);
    // Where the high-pass is: below it the sound stays as it was, in the middle.
    const float corner = yOf(highPass);
    c.line({inner.x, corner}, {inner.right(), corner}, kFaint, 1.0f, true);

    char hp[32], text[80];
    formatParam(paramIndex(slot, FxParam::WidenerHighPass), highPass, hp, sizeof(hp));
    const double shortest = widenerTap(0, size, 0.0) * 1000.0, longest = widenerTap(kWidenerTaps - 1, size, 0.0) * 1000.0;
    const char *const digits = longest < 10.0 ? "%.1f" : "%.0f";
    std::snprintf(text, sizeof(text), "%s",
                  trf(Str::CaptionWidener, {hp, number(digits, shortest), number(digits, longest)}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 12.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({r.right() - 16.0f, inner.bottom() - 12.0f, 12.0f, 12.0f}, "R", kTextLabel, kFaint);
}

// The utility's picture of the stereo field, as the widener's: the sound's place across, left to right, and its
// frequency up. The band is the sound, its middle where Pan puts it and as wide as Width makes it; below Mono Bass's
// line it narrows into its middle, mono. It fades as Gain goes down below 0 dB, gone at -inf, and the strip at its left
// shows Gain, a tick across it at 0 dB.
void View::paintUtility(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const double gainNorm = std::clamp(at(FxParam::UtilityGain), 0.0, 1.0), pan = at(FxParam::UtilityPan);
    const double width = std::clamp(at(FxParam::UtilityWidth), 0.0, kMaxUtilityWidth);
    const double mono = at(FxParam::UtilityMonoBass);
    const Rect strip = utilityStrip(r), field = utilityField(r);
    c.fillRect(strip, kTrack, kRadiusMark);
    if (const float level = strip.bottom() - static_cast<float>(gainNorm) * strip.h; gainNorm > 0.0)
        c.fillRect({strip.x, level, strip.w, strip.bottom() - level}, fx.alpha(0.8f), kRadiusMark);
    const double unity = utilityGainNorm(1.0);
    const float zeroDb = strip.bottom() - static_cast<float>(unity) * strip.h;
    c.line({strip.x - 3.0f, zeroDb}, {strip.right() + 3.0f, zeroDb}, kDim, 1.0f);

    const float middle = utilityMiddle(field, pan), spread = utilitySpread(field, width);
    auto yOf = [&](double norm) { return field.bottom() - static_cast<float>(norm) * field.h; };
    const float shown = static_cast<float>(std::min(gainNorm / unity, 1.0));
    std::vector<Point> left, right;
    for (int i = 0; i <= 64; ++i) {
        const double y = i / 64.0;
        const float half = spread * static_cast<float>(mono > 0.0 ? widenerSpread(cutoffFromNorm(y), mono) : 1.0);
        left.push_back({std::max(field.x, middle - half), yOf(y)});
        right.push_back({std::min(field.right(), middle + half), yOf(y)});
    }
    std::vector<Point> area = left;
    area.insert(area.end(), right.rbegin(), right.rend());
    if (shown > 0.0f)
        c.fillPolygon(area.data(), area.size(), fx.alpha(0.16f * shown));
    c.line({field.center().x, field.y}, {field.center().x, field.bottom()}, kFaint, 1.0f, true);
    if (shown > 0.0f) {
        plotLine(c, left.data(), left.size(), fx.alpha(shown), 1.6f);
        plotLine(c, right.data(), right.size(), fx.alpha(shown), 1.6f);
    }
    c.line({middle, field.y}, {middle, field.bottom()}, shown > 0.0f ? fx.alpha(0.3f + 0.7f * shown) : kDim, 1.6f);
    if (mono > 0.0) { // below it, the sides are high-passed away: the bass in the middle; its corner over it
        const float y = yOf(mono);
        c.line({field.x, y}, {field.right(), y}, kDim, 1.0f, true);
        char hz[32];
        formatParam(paramIndex(slot, FxParam::UtilityMonoBass), mono, hz, sizeof(hz));
        c.text({field.x + 2.0f, y - 13.0f, field.w - 16.0f, 12.0f}, trf(Str::CaptionUtilityMonoBass, {hz}).c_str(),
               kTextSmall, kDim, Align::Left);
    }

    char gain[32], panText[32];
    formatParam(paramIndex(slot, FxParam::UtilityGain), gainNorm, gain, sizeof(gain));
    formatParam(paramIndex(slot, FxParam::UtilityPan), pan, panText, sizeof(panText));
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, trf(Str::CaptionUtility, {gain, panText}).c_str(), kTextLabel,
           kDim, Align::Left);
    c.text({field.x - 2.0f, field.bottom() - 12.0f, 12.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({field.right() - 10.0f, field.bottom() - 12.0f, 12.0f, 12.0f}, "R", kTextLabel, kFaint);
}

Point View::utilityHandlePoint(Rect r, int handle, bool live, int slot) const {
    auto at = [&](FxParam f) {
        const int index = paramIndex(slot, f);
        return live ? liveValue(index) : value(index);
    };
    const Rect strip = utilityStrip(r), field = utilityField(r);
    const double pan = at(FxParam::UtilityPan);
    const float middle = utilityMiddle(field, pan);
    switch (handle) {
    case 0: // across the Gain strip, at its level
        return {strip.center().x,
                strip.bottom() - static_cast<float>(std::clamp(at(FxParam::UtilityGain), 0.0, 1.0)) * strip.h};
    case 1: // at the top of the sound's middle line
        return {middle, field.y + 4.0f};
    case 2: { // on the band's edge, on the side with more room
        const float reach = utilitySpread(field, std::clamp(at(FxParam::UtilityWidth), 0.0, kMaxUtilityWidth));
        return {std::clamp(middle + (pan > 0.0 ? -reach : reach), field.x, field.right()),
                field.y + kUtilityWidthHandleY * field.h};
    }
    default: // at the end of Mono Bass's line; off, at the bottom
        return {field.right() - 6.0f,
                field.bottom() - static_cast<float>(std::max(at(FxParam::UtilityMonoBass), 0.0)) * field.h};
    }
}

// The amp's display: the tone's frequency response, 20 Hz to 20 kHz across and +12 to -30 dB up: the model's tone stack
// with Presence, through the cabinet and mic while the cabinet is on (solid, filled) over the tone stack alone (faint,
// dashed); with the cabinet off, the tone stack alone, solid. At its right, the drive meter: how far its stages saturate
// now, up from the bottom, 36 dB at the top; its figure at the top left.
void View::paintAmp(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    auto at = [&](FxParam f) { return std::clamp(liveValue(paramIndex(slot, f)), 0.0, 1.0); }; // as modulation takes them
    AmpKnobs knobs;
    knobs.gain = at(FxParam::AmpGain);
    knobs.bass = at(FxParam::AmpBass);
    knobs.mid = at(FxParam::AmpMid);
    knobs.treble = at(FxParam::AmpTreble);
    knobs.presence = at(FxParam::AmpPresence);
    knobs.volume = at(FxParam::AmpVolume);
    knobs.position = at(FxParam::AmpPosition);
    knobs.distance = at(FxParam::AmpDistance);
    const AmpModel model = storedEnum<AmpModel>(value(paramIndex(slot, FxParam::AmpModel)));
    const AmpCab cab = storedEnum<AmpCab>(value(paramIndex(slot, FxParam::AmpCab)));
    const AmpMic mic = storedEnum<AmpMic>(value(paramIndex(slot, FxParam::AmpMic)));
    const bool cabOn = value(paramIndex(slot, FxParam::AmpCabOn)) >= 0.5;
    constexpr double kTopDb = 12.0, kBottomDb = -30.0;
    const Rect plot{r.x + 6.0f, r.y + 18.0f, r.w - 26.0f, r.h - 24.0f};
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = plot.x + plot.w * static_cast<float>(std::log10(hz / 20.0) / 3.0);
        c.line({x, plot.y}, {x, plot.bottom()}, kBorder, 1.0f);
    }
    const float zero = plot.y + plot.h * static_cast<float>(kTopDb / (kTopDb - kBottomDb));
    c.line({plot.x, zero}, {plot.right(), zero}, kBorder, 1.0f, true);
    constexpr int kSteps = 80;
    std::vector<Point> tone, out;
    for (int i = 0; i <= kSteps; ++i) {
        const double hz = 20.0 * std::pow(1000.0, static_cast<double>(i) / kSteps);
        const double t = ampToneResponse(model, knobs, hz);
        const float x = plot.x + plot.w * static_cast<float>(i) / kSteps;
        auto y = [&](double gain) {
            const double db = std::clamp(20.0 * std::log10(std::max(gain, 1e-6)), kBottomDb, kTopDb);
            return plot.y + plot.h * static_cast<float>((kTopDb - db) / (kTopDb - kBottomDb));
        };
        tone.push_back({x, y(t)});
        out.push_back({x, y(cabOn ? t * ampCabResponse(cab, mic, knobs, hz) : t)});
    }
    if (cabOn)
        c.polyline(tone.data(), tone.size(), fx.alpha(0.45f), 1.2f, true);
    std::vector<Point> area = out;
    area.push_back({plot.right(), plot.bottom()});
    area.push_back({plot.x, plot.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, out.data(), out.size(), fx, 1.8f);
    const float drive = telemetry_.fxReduction[slot][0];
    constexpr float kDriveRangeDb = 36.0f;
    const Rect meter{r.right() - 12.0f, r.y + 8.0f, 6.0f, r.h - 16.0f};
    c.fillRect(meter, meterWell(), kRadiusMark);
    const float h = std::clamp(drive / kDriveRangeDb, 0.0f, 1.0f) * meter.h;
    if (h > 0.5f)
        c.fillRect({meter.x, meter.bottom() - h, meter.w, h}, fx, kRadiusMark);
    c.text({r.x + 8.0f, r.y + 3.0f, r.w - 28.0f, 14.0f}, trf(Str::CaptionAmpDrive, {number("%.1f", drive)}),
           kTextSmall, drive > 0.05f ? kText : kDim, Align::Left);
}

// The cabinet's display: its speaker seen from the side at the left, the baffle and the cone behind it, and the mic in
// front of it (drawn by its widget), up from the cone's axis to its edge by Position and away to the right by Distance;
// set back far enough, faint arcs of the room it hears. The mic and its distance at the top left.
void View::paintAmpCab(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const double position = std::clamp(liveValue(paramIndex(slot, FxParam::AmpPosition)), 0.0, 1.0);
    const double distance = std::clamp(liveValue(paramIndex(slot, FxParam::AmpDistance)), 0.0, 1.0);
    const float baffle = ampBaffleX(r), axis = ampConeAxis(r), cone = ampConeRadius(r);
    c.fillRect({r.x + 4.0f, r.y + 16.0f, baffle - r.x - 10.0f, r.h - 22.0f}, kRaised, kRadiusMark);
    // The cone's profile, flaring out to the baffle from the voice coil behind it, and its dust cap.
    const Point coilTop{baffle - 9.0f, axis - 5.0f}, coilBottom{baffle - 9.0f, axis + 5.0f};
    c.curve({baffle, axis - cone}, {baffle - 3.0f, axis - cone * 0.35f}, coilTop, kDim, 1.3f);
    c.curve(coilBottom, {baffle - 3.0f, axis + cone * 0.35f}, {baffle, axis + cone}, kDim, 1.3f);
    c.line(coilTop, coilBottom, kDim, 1.3f);
    c.curve(coilTop, {baffle - 4.0f, axis}, coilBottom, kDim, 1.2f);
    c.line({baffle + 1.0f, axis - cone - 3.0f}, {baffle + 1.0f, axis + cone + 3.0f}, kFaint, 1.0f, true);
    c.line({baffle, axis}, {r.right() - 8.0f, axis}, kBorder, 1.0f, true); // the cone's axis
    const Point m = ampMicPoint(r, position, distance);
    const float room = static_cast<float>(std::clamp((distance - 0.59) / 0.41, 0.0, 1.0)); // from about 15 cm on
    for (int i = 1; i <= 2 && room > 0.0f; ++i) {
        const float dx = 6.0f + i * 6.0f, dy = 6.0f + i * 3.0f;
        c.curve({m.x + dx, m.y - dy}, {m.x + dx + 4.0f + i * 2.0f, m.y}, {m.x + dx, m.y + dy}, fx.alpha(0.4f * room),
                1.0f);
    }
    // Its cable, down to the display's bottom right.
    c.curve({m.x + 10.0f, m.y}, {m.x + 16.0f, r.bottom() - 8.0f}, {r.right() - 4.0f, r.bottom() - 8.0f}, kFaint, 1.0f);
    char cm[32];
    formatParam(paramIndex(slot, FxParam::AmpDistance), distance, cm, sizeof(cm));
    const AmpMic mic = storedEnum<AmpMic>(value(paramIndex(slot, FxParam::AmpMic)));
    c.text({r.x + 8.0f, r.y + 3.0f, r.w - 12.0f, 14.0f}, trf(Str::CaptionAmpMic, {ampMicName(mic), cm}), kTextSmall,
           kDim, Align::Left);
}

void View::paintNoteSource(Canvas &c, float top) {
    // The note the newest voice plays, as the note source follows it (bend and glide included): its name, and
    // how many cents off it is; a dash while nothing plays.
    const Rect box = noteDisplay(top);
    const Color color = kSourceColors[kNoteSource];
    const bool lit = (hover_ && hover_->kind == Kind::NoteDisplay && hover_->a == kNoteSource &&
                      drag_.kind == DragKind::None) ||
                     shapeEditing() == kNoteCurve; // it opens the curve, or has
    c.fillRect(box, lit ? kTrack : kDisplayColor, kRadiusControl);
    if (outlined())
        c.strokeRect(box, color.alpha(lit ? 0.9f : 0.45f), 1.0f, kRadiusControl);
    // Behind the note, its curve across the box: faint until the note source modulates something, as its handle is,
    // and lit once it does.
    auto get = [this](int index) { return value(index); };
    std::vector<Point> curve;
    for (int i = 0; i <= 24; ++i) {
        const double x = i / 24.0;
        const double y = clipCurveValue(get, paramIndex(NoteParam::Curve), 2.0 * x - 1.0);
        curve.push_back({box.x + 4.0f + static_cast<float>(x) * (box.w - 8.0f),
                         box.bottom() - 4.0f - static_cast<float>(0.5 * (y + 1.0)) * (box.h - 8.0f)});
    }
    const bool live = sourceLive(kNoteSource);
    c.polyline(curve.data(), curve.size(), color.alpha(live ? kLiveCurveOpacity : 0.3f), live ? 1.5f : 1.2f);
    std::string text = "\xE2\x80\x93";
    if (telemetry_.voices > 0) {
        const double pitch = telemetry_.notePitch;
        const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
        const long cents = std::lround((pitch - key) * 100.0);
        text = noteName(key);
        if (cents != 0)
            text += (cents > 0 ? " +" : " ") + std::to_string(cents);
    }
    c.text(box, text, kTextBody, telemetry_.voices > 0 ? kText : kFaint, Align::Center, telemetry_.voices > 0);
    c.text({box.x - 14.0f, top + kStripRow + 19.0f, box.w + 28.0f, 16.0f}, tr(Str::LabelNote), kTextLabel, kDim);
    if (mods_.sourceBipolar(kNoteSource)) // swings both ways: a sign in its corner
        c.text({box.right() - 13.0f, box.y + 1.0f, 11.0f, 11.0f}, "\xC2\xB1", kTextLabel, color, Align::Center, true);
}

void View::paintVelocitySource(Canvas &c, float top) {
    // The velocity the newest voice came with, as MIDI has it (0 to 127); a dash while nothing plays.
    const Rect box = velocityDisplay(top);
    const Color color = kSourceColors[kVelocitySource];
    const bool lit = (hover_ && hover_->kind == Kind::NoteDisplay && hover_->a == kVelocitySource &&
                      drag_.kind == DragKind::None) ||
                     shapeEditing() == kVelocityCurve; // it opens the curve, or has
    c.fillRect(box, lit ? kTrack : kDisplayColor, kRadiusControl);
    if (outlined())
        c.strokeRect(box, color.alpha(lit ? 0.9f : 0.45f), 1.0f, kRadiusControl);
    // Behind the number, its curve across the box, with a mark where the newest voice's velocity is: faint until
    // velocity modulates something, as its handle is, and lit once it does.
    auto get = [this](int index) { return value(index); };
    auto at = [&](double x) {
        return Point{box.x + 4.0f + static_cast<float>(x) * (box.w - 8.0f),
                     box.bottom() - 4.0f - static_cast<float>(velocitySourceValue(get, x)) * (box.h - 8.0f)};
    };
    std::vector<Point> curve;
    for (int i = 0; i <= 24; ++i)
        curve.push_back(at(i / 24.0));
    const bool live = sourceLive(kVelocitySource);
    c.polyline(curve.data(), curve.size(), color.alpha(live ? kLiveCurveOpacity : 0.3f), live ? 1.5f : 1.2f);
    const bool playing = telemetry_.voices > 0;
    if (playing)
        c.fillCircle(at(telemetry_.noteVelocity), 2.0f, color.alpha(live ? 1.0f : 0.6f));
    const double velocity = std::clamp(static_cast<double>(telemetry_.noteVelocity), 0.0, 1.0);
    const std::string text = playing ? std::to_string(std::lround(127.0 * velocity)) : "\xE2\x80\x93";
    c.text(box, text, kTextBody, playing ? kText : kFaint, Align::Center, playing);
    c.text({box.x - 14.0f, top + kStripRow + 19.0f, box.w + 28.0f, 16.0f}, tr(Str::LabelVelocitySource), kTextLabel, kDim);
    if (mods_.sourceBipolar(kVelocitySource)) // swings both ways: a sign in its corner
        c.text({box.right() - 13.0f, box.y + 1.0f, 11.0f, 11.0f}, "\xC2\xB1", kTextLabel, color, Align::Center, true);
}

void View::paintShifter(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 26.0f};
    const double hz =
        shiftHz(liveValue(paramIndex(slot, FxParam::ShiftAmount)), liveValue(paramIndex(slot, FxParam::ShiftRange)));
    const double spread = liveValue(paramIndex(slot, FxParam::ShiftSpread));
    const double shifts[2] = {hz, hz * (1.0 - 2.0 * spread)};
    const float mid = inner.center().y;
    c.line({inner.x, mid}, {inner.right(), mid}, kBorder, 1.0f);
    auto x = [&](double f) { return inner.x + static_cast<float>(std::log(f / 50.0) / std::log(200.0)) * inner.w; };
    for (int k = 1; k <= 12; ++k) {
        const float h = (0.5f * inner.h - 2.0f) / std::sqrt(static_cast<float>(k));
        const double f = 220.0 * k;
        c.line({x(f), mid - h}, {x(f), mid + h}, kFaint.alpha(0.6f), 1.0f);
        for (int side = 0; side < 2; ++side) {
            const double moved = f + shifts[side];
            if (moved < 50.0 || moved > 10000.0)
                continue; // shifted below nothing, or off the end
            const float tip = side == 0 ? mid - h : mid + h;
            c.line({x(moved), mid}, {x(moved), tip}, fx, 1.6f);
        }
    }
    // The shift in Hz, and the range it is a share of.
    char shift[32], range[32], text[80];
    if (std::abs(hz) < 0.005)
        std::snprintf(shift, sizeof(shift), "0 Hz");
    else if (std::abs(hz) < 10.0)
        std::snprintf(shift, sizeof(shift), "%+.2f Hz", hz);
    else if (std::abs(hz) < 1000.0)
        std::snprintf(shift, sizeof(shift), "%+.1f Hz", hz);
    else
        std::snprintf(shift, sizeof(shift), "%+.2f kHz", hz / 1000.0);
    formatParam(paramIndex(slot, FxParam::ShiftRange), liveValue(paramIndex(slot, FxParam::ShiftRange)), range,
                sizeof(range));
    std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionShiftOf, {shift, range}).c_str());
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 16.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
    c.text({r.x + 4.0f, inner.y, 10.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({r.x + 4.0f, inner.bottom() - 12.0f, 10.0f, 12.0f}, "R", kTextLabel, kFaint);
}

void View::paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    for (int i = 1; i < 4; ++i)
        c.line({r.x + 8.0f, r.y + r.h * i / 4.0f}, {r.right() - 8.0f, r.y + r.h * i / 4.0f}, kBorder.alpha(0.6f), 1.0f);

    const EnvIndices e = envIndices(env);
    // The curve as modulation takes it now. While that isn't where the knobs set it, a faint line shows where they do,
    // which the handles keep to.
    auto now = [this](int index) { return liveValue(index); };
    auto setAt = [this](int index) { return value(index); };
    const double sustain = now(e.sustain);
    const float scale = envScale(env);
    const EnvShape s = envShape(r, e, now, scale);
    const float height = s.bottom - s.top;
    auto curve = [&](const EnvShape &shape, const auto &get) {
        std::vector<Point> points{shape.p0};
        for (int segment = 0; segment < 3; ++segment) {
            if (segment == 1)
                points.push_back(shape.ph); // the end of the hold
            for (int i = 1; i <= 24; ++i)
                points.push_back(envSegmentPoint(shape, segment, i / 24.0, get(e.curve(segment)), get(e.sustain)));
        }
        return points;
    };
    const int shaping[] = {e.attack, e.hold, e.decay, e.sustain, e.release, e.curve(0), e.curve(1), e.curve(2)};
    const bool moved =
        std::any_of(std::begin(shaping), std::end(shaping), [&](int index) { return now(index) != value(index); });
    std::vector<Point> points;
    if (moved) { // a new curve every paint, kept out of the cache so that it pushes no others out
        points = curve(s, now);
    } else {
        std::vector<double> key = curveKey(Curve::Envelope, r);
        key.insert(key.end(), {s.scale, value(e.attack), value(e.hold), value(e.decay), sustain, value(e.release),
                               value(e.curve(0)), value(e.curve(1)), value(e.curve(2))});
        points = curveCache().get(key, [&] { return curve(s, setAt); });
    }

    std::vector<Point> area = points;
    area.push_back({s.p4.x, s.bottom});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    if (moved) {
        const std::vector<Point> knobs = curve(envShape(r, e, setAt, scale), setAt);
        c.polyline(knobs.data(), knobs.size(), color.alpha(0.4f), 1.25f);
    }
    plotLine(c, points.data(), points.size(), color, 2.0f);

    // While notes play, a line across at the newest voice's level: an operator's envelope's, or a mod envelope's.
    if (live && telemetry_.voices > 0 && (env < kOps ? routing_.nodeOn(env) : mods_.sourceOn(env - kOps))) {
        const float level = std::clamp(env < kOps ? telemetry_.opEnvLevels[env] : telemetry_.sources[env - kOps], 0.0f, 1.0f);
        const float y = s.bottom - level * height;
        c.line({r.x + 8.0f, y}, {r.right() - 8.0f, y}, color.alpha(0.35f), 1.0f, true);
        // And a dot on the curve, as far along the stage the envelope is in as it has come: along the attack, decay
        // or release by its bend, across the hold, and at the sustain level's point through the sustain. Where the
        // envelope follows its curve, that is where the line meets it. Where it doesn't (a note let go during the decay
        // releases from above the sustain), the dot keeps to the curve, and the line shows the level.
        const float position = env < kOps ? telemetry_.opEnvPos[env] : telemetry_.envPos[env - kOps];
        if (position >= 0.0f) {
            const int stage = std::min(static_cast<int>(position), 4);
            const double along = std::clamp(static_cast<double>(position) - stage, 0.0, 1.0);
            Point dot = s.p2; // the sustain
            if (stage == 1)
                dot = {s.p1.x + static_cast<float>(along) * (s.ph.x - s.p1.x), s.top};
            else if (stage != 3)
                dot = envSegmentPoint(s, stage / 2, along, now(e.curve(stage / 2)), sustain);
            c.fillCircle(dot, 7.0f, color.alpha(0.25f));
            c.fillCircle(dot, 3.5f, kText);
        }
    }
    // A bipolar envelope swings its targets about its middle level: a line there, and a sign in the corner.
    if (env >= kOps && mods_.sourceBipolar(env - kOps)) {
        const float middle = s.bottom - 0.5f * height;
        c.line({r.x + 8.0f, middle}, {r.right() - 8.0f, middle}, color.alpha(0.45f), 1.0f, true);
        c.text({r.right() - 20.0f, r.y + 3.0f, 14.0f, 14.0f}, "\xC2\xB1", kTextBody, color, Align::Center, true);
    }
}

void View::paintEnvReadout(Canvas &c, const Widget &w) {
    const EnvIndices e = envIndices(w.a);
    char a[32], b[32];
    std::string text;
    if (w.kind == Kind::EnvCurve) {
        formatParam(w.param, value(w.param), a, sizeof(a));
        text = trf(Str::CaptionEnvCurve, {tr(kEnvSegmentNames[w.b]), a});
    } else if (w.b == 0) {
        formatParam(e.attack, value(e.attack), a, sizeof(a));
        text = a;
    } else if (w.b == 1) {
        formatParam(e.hold, value(e.hold), a, sizeof(a));
        text = trf(Str::CaptionHold, {a});
    } else if (w.b == 2) {
        formatParam(e.decay, value(e.decay), a, sizeof(a));
        formatParam(e.sustain, value(e.sustain), b, sizeof(b));
        text = std::string(a) + "  \xC2\xB7  " + b;
    } else {
        formatParam(e.release, value(e.release), a, sizeof(a));
        text = a;
    }
    // On a backdrop of the display's color, which the curve under it shows through faintly; left of a bipolar
    // envelope's sign.
    const Rect r = envRect(w.a);
    const float right = r.right() - (w.a >= kOps && mods_.sourceBipolar(w.a - kOps) ? 24.0f : 8.0f);
    const float width = c.textWidth(text, kTextLabel);
    c.fillRect({right - width - 5.0f, r.y + 2.0f, width + 10.0f, 14.0f}, kDisplayColor.alpha(0.85f), kRadiusMark);
    c.text({r.x, r.y + 2.0f, right - r.x, 14.0f}, text, kTextLabel, kText, Align::Right);
}

// How many cycles a random LFO's display shows.

void View::paintLfo(Canvas &c, Rect r, int lfo) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const Rect inner{r.x + 10.0f, r.y + 8.0f, r.w - 20.0f, r.h - 16.0f};
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const float zero = mods_.sourceBipolar(lfo) ? inner.center().y : inner.bottom();
    c.line({inner.x, zero}, {inner.right(), zero}, kBorder, 1.0f);
    const LfoShape shape = mods_.lfoShapeOf(lfo);
    // A random one standing still takes a value for each note, at once: four made-up notes' steps, until one plays,
    // then a line across at the value the newest voice holds, the live dot at its right end.
    const bool held = shape == LfoShape::Random && lfoHeld([this](int i) { return value(i); }, lfo);
    const bool holding = held && lfoRunning(lfo);
    // Where notes start it, past the cycle's start.
    if (const double start = value(paramIndex(lfo, SourceParam::Phase)); lfoPhaseUsed(lfo) && start > 0.0 && start < 1.0) {
        const float x = inner.x + static_cast<float>(start) * inner.w;
        c.line({x, r.y + 4.0f}, {x, r.bottom() - 4.0f}, kSourceColors[lfo].alpha(0.45f), 1.0f);
    }
    std::vector<Point> points;
    auto at = [&](double x, double y) {
        points.push_back({inner.x + static_cast<float>(x) * inner.w, inner.center().y - static_cast<float>(y) * inner.h * 0.5f});
    };
    const bool sweep = shape == LfoShape::Random && !held && randomSweep(lfo);
    if (holding) {
        const double now = lfoValueNow(lfo);
        at(0.0, now);
        at(1.0, now);
    } else if (sweep) {
        // Running, a random LFO's own last cycles as they came, as one line scrolling left: the live dot at its right
        // end, the display four cycles wide.
        const RandomTrace &trace = randomTraces_[lfo];
        const double smooth = value(paramIndex(lfo, SourceParam::RandomSmooth));
        const double phase = randomPhaseNow(lfo);
        const int newest = trace.count - 1;
        for (int j = std::max(newest - (kRandomGlides - 1), 0); j <= newest; ++j) {
            const auto [from, to] = trace.glides[j % kRandomGlides];
            const double start = (kRandomCycles + (j - newest) - phase) / kRandomCycles; // where its cycle starts
            const double first = std::max(0.0, -start * kRandomCycles);                 // clipped at the left edge
            const double last = j == newest ? phase : 1.0;
            for (int i = points.empty() ? 0 : 1; i <= 24; ++i) {
                const double u = first + (last - first) * i / 24.0;
                at(start + u / kRandomCycles, randomGlide(from, to, u, smooth));
            }
        }
    } else {
        std::vector<double> key = curveKey(Curve::Lfo, inner);
        key.push_back(static_cast<double>(shape));
        if (shape == LfoShape::Custom)
            shapes_.keyShape(key, shapeCountIndex(lfo));
        const double smooth = held ? 0.0 : value(paramIndex(lfo, SourceParam::RandomSmooth)); // held, it glides none
        if (shape == LfoShape::Random)
            key.insert(key.end(), {smooth, value(paramIndex(lfo, SourceParam::RandomChaos))});
        points = curveCache().get(key, [&] {
            if (shape == LfoShape::Custom) {
                auto get = [this](int index) { return value(index); };
                shapeOutline(get, lfo, 96, at);
            } else if (shape == LfoShape::Random) {
                // Four cycles of a made-up run with this LFO's chaos and smoothing, until it runs.
                const double chaos = value(paramIndex(lfo, SourceParam::RandomChaos));
                uint32_t rng = 0x2545F491u;
                double from = 0.3;
                for (int cycle = 0; cycle < kRandomCycles; ++cycle) {
                    const double to = randomStep(from, chaos, rng);
                    for (int i = 0; i <= 24; ++i)
                        at((cycle + i / 24.0) / kRandomCycles, randomGlide(from, to, i / 24.0, smooth));
                    from = to;
                }
            } else {
                points = lfoPoints(shape, inner);
            }
            return points;
        });
    }
    const Color color = kSourceColors[lfo];
    if (look.flat && points.size() > 1) { // filled toward its zero line, as a widened card's shape is
        std::vector<Point> area = points;
        area.push_back({points.back().x, zero});
        area.push_back({points.front().x, zero});
        c.fillPolygon(area.data(), area.size(), color.alpha(0.14f));
    }
    plotLine(c, points.data(), points.size(), color, 2.0f);

    if (lfoRunning(lfo) && (sweep || holding || shape != LfoShape::Random)) {
        const Point dot = sweep || holding ? points.back()
                                : Point{inner.x + static_cast<float>(lfoPhaseNow(lfo)) * inner.w,
                                        inner.center().y - static_cast<float>(lfoValueNow(lfo)) * inner.h * 0.5f};
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }
}

void View::paintSources(Canvas &c) {
    if (sourcesHidden())
        return; // under the SEQ view
    // The panel's title, at its top left, in line with the cards' left edges.
    const Rect title = modTitle();
    c.text({kModPanel.x + 12.0f, title.y, title.w, title.h}, tr(Str::HeadingModulation), kTextBody, kDim, Align::Left,
           true);
    const std::vector<int> shown = sourcesShown();
    if (shown.empty()) {
        c.text({kModPanel.x, kCardTop + kCardH * 0.5f - 10.0f, kModPanel.w, 20.0f}, tr(Str::HintNoSources), kTextValue,
               kDim);
        return;
    }
    c.pushClip(sourceCardClip());
    for (int position = 0; position < static_cast<int>(shown.size()); ++position)
        if (const float card = sourceOffset(shown, position) - sourceScroll();
            onScreen(card, kVisibleCards, sourceSpan(shown[position])))
            paintSource(c, card, shown[position]);
    c.popClip();
}

namespace {
// A slot row's name, drawn smaller to fit its place when it is long, down to 7.5, which the canvas draws at about 9 px;
// its width is a straight line between the two.
float slotNameSize(Canvas &c, const std::string &name) {
    constexpr float kFull = kTextBody, kSmallest = 7.5f; // fitted to its room
    const float full = c.textWidth(name, kFull);
    if (full <= kSlotNameW)
        return kFull;
    const float smallest = c.textWidth(name, kSmallest);
    return smallest >= kSlotNameW
               ? kSmallest
               : std::floor((kFull - (full - kSlotNameW) / (full - smallest) * (kFull - kSmallest)) * 4.0f) / 4.0f;
}
} // namespace

void View::paintSource(Canvas &c, float card, int s) {
    const Rect p = sourceCard(card, sourceSpan(s));
    const Color color = kSourceColors[s];
    const bool lfo = runsLfo(mods_.sourceTypeOf(s)); // an LFO, or Random, a random one
    const bool dragged = drag_.kind == DragKind::SourceMove && drag_.widget.a == s;
    paintCardFace(c, p, color, mods_.sourceOn(s), s, dragged, 0.35f, sourceCardClip());
    if (sourceFolded(s)) { // the grip's dots under the handle (widgets), then its name down the strip
        const bool grip =
            (hover_ && hover_->kind == Kind::SourceGrip && hover_->a == s && drag_.kind == DragKind::None) || dragged;
        for (int d = 0; d < 6; ++d)
            c.fillCircle({p.center().x - 5.0f + (d / 2) * 5.0f, p.y + 35.0f + (d % 2) * 5.0f}, 1.4f, grip ? kText : kFaint);
        std::string name = mods_.sourceName(s);
        // "Random" and "Curve" too long for the strip: their short names, by their number.
        if (mods_.sourceTypeOf(s) == SourceType::Random || mods_.sourceTypeOf(s) == SourceType::Curve)
            name = trf(mods_.sourceTypeOf(s) == SourceType::Random ? Str::SourceFoldedRandom : Str::SourceFoldedCurve,
                       {name.substr(name.find_last_of(' ') + 1)});
        paintDownward(c, {p.x, p.y + 104.0f, p.w, p.h - 110.0f}, upperCase(name), mods_.sourceOn(s) ? color : kDim);
        return;
    }

    // Title row: the handle (a widget) and the name, grey while the source is off; the row drags the card.
    // In title case, as an effect card's title.
    c.text({p.x + 40.0f, p.y + 7.0f, 80.0f, 21.0f}, mods_.sourceName(s), kTextBody,
           mods_.sourceOn(s) ? kText : kDim, Align::Left, true);

    if (s == wideLfo()) {
        shapes_.paintLfoCard(c, card, s);
    } else if (lfo) {
        paintLfo(c, sourceDisplay(card, mods_.lfoShapeOf(s) == LfoShape::Random), s);
    } else if (mods_.sourceTypeOf(s) == SourceType::Env) {
        paintEnvelope(c, sourceDisplay(card), kOps + s, color, true);
    } else {
        // MIDI: what the newest voice gets from it, as a bar from the left (from the middle while it is bipolar, where
        // its depths swing both ways), and in numbers.
        const Rect d = sourceDisplay(card);
        c.fillRect(d, kBackground, kRadiusCard);
        const bool live = mods_.sourceOn(s) && telemetry_.voices > 0;
        const double out = live ? std::clamp(static_cast<double>(telemetry_.sources[s]), 0.0, 1.0) : 0.0;
        const Rect track{d.x + 12.0f, d.center().y - 3.0f, d.w - 24.0f, 6.0f};
        c.fillRect(track, kTrack, kRadiusMark);
        const bool bipolar = mods_.sourceBipolar(s);
        const float from = bipolar ? track.center().x : track.x;
        const float to = track.x + track.w * static_cast<float>(out);
        if (live)
            c.fillRect({std::min(from, to), track.y, std::abs(to - from), track.h}, color, kRadiusMark);
        if (bipolar)
            c.line({track.center().x, track.y - 4.0f}, {track.center().x, track.bottom() + 4.0f}, kFaint, 1.0f);
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", (bipolar ? out * 2.0 - 1.0 : out) * 100.0);
        if (live)
            c.text({d.x, d.y + 2.0f, d.w - 12.0f, 16.0f}, text, kTextLabel, kDim, Align::Right);
    }
    // Under the display, the button that shows its modulations in the MODS view (a widget).
}

// A switch's face: lit, a tint of `color` (`tint` strong) outlined in it; hot (under the mouse, say), the track's; else
// raised. In a flat theme, a well without an outline, lit filled solid with `color`, hot a little darker or lighter.
void View::paintButtonFace(Canvas &c, const Widget &w, bool lit, bool hot, Color color, float tint, float radius) {
    if (look.flat) {
        const Color face = lit ? color : hot ? wellHover() : wellColor();
        if (w.group.w <= 0.0f) {
            c.fillRect(w.rect, face, radius);
            return;
        }
        // Its part of the row's well, lit to its edges (the well's outer corners rounded), as the well is.
        c.pushClip({w.partLeft, w.group.y, w.partRight - w.partLeft, w.group.h});
        c.fillRect(w.group, face, radius);
        c.popClip();
        lastPartLit_ = lit;
        return;
    }
    const Color fill = lit ? color.alpha(tint) : hot ? kTrack : kRaised;
    if (w.group.w <= 0.0f) {
        c.fillRect(w.rect, fill, radius);
        if (lit)
            c.strokeRect(w.rect, color, 1.0f, radius);
        return;
    }
    // Its part of the pill: the pill painted within it, so only the pill's outer corners are rounded. The outline is
    // drawn inside the pill, and the lines between parts (round-capped) stop half a pixel short of its top and bottom,
    // so nothing reaches past it.
    const bool first = w.partLeft <= w.group.x, last = w.partRight >= w.group.right();
    if (first)
        lastPartLit_ = false;
    c.pushClip({w.partLeft, w.group.y, w.partRight - w.partLeft, w.group.h});
    const float top = w.group.y + 0.5f, bottom = w.group.bottom() - 0.5f;
    if (fill.a < 1.0f) // a lit part's tint goes over the pill's own fill
        c.fillRect(w.group, kRaised, radius);
    c.fillRect(w.group, fill, radius);
    c.strokeRect(w.group, lit ? color : kTrack, 1.0f, radius);
    if (lit) {
        if (!first)
            c.line({w.partLeft + 0.5f, top}, {w.partLeft + 0.5f, bottom}, color, 1.0f);
        if (!last)
            c.line({w.partRight - 0.5f, top}, {w.partRight - 0.5f, bottom}, color, 1.0f);
    } else if (!first && !lastPartLit_) {
        c.line({w.partLeft, top + 1.0f}, {w.partLeft, bottom - 1.0f}, kTrack, 1.0f);
    }
    c.popClip();
    lastPartLit_ = lit;
}

void View::paintWidget(Canvas &c, const Widget &w) {
    const bool hovered = isHovered(w);
    const bool active = isActive(w);
    for (Panel *panel : panels_) // the panels' own (the presets', see presetbrowser.cpp, and the SEQ view's)
        if (panel->paintWidget(c, w, hovered))
            return;
    switch (w.kind) {
    case Kind::Knob:
        if (isRatioDisplay(w))
            paintRatio(c, w);
        else if (isStepper(w))
            paintStepper(c, w);
        else
            paintKnob(c, w);
        break;
    case Kind::Choice:
    case Kind::Toggle: {
        const bool clockMode = isLfoClockChoice(w);
        const bool on = clockMode                ? static_cast<int>(lfoClockOf(w.a)) == w.b
                        : w.kind == Kind::Choice ? std::lround(value(w.param)) == w.b
                                                 : value(w.param) >= 0.5;
        const Str clipModes[] = {Str::SwitchHard, Str::SwitchSoft, Str::SwitchAsym};
        constexpr Str kMicShortNames[] = {Str::MicDynamicShort, Str::MicCondenserShort, Str::MicRibbonShort};
        static_assert(std::size(kMicShortNames) == static_cast<size_t>(AmpMic::Count));
        const FxParam field = fxFieldOf(w.param);
        // Operators', filters', sources' and effects' on switches show the power symbol rather than a word.
        const bool power =
            isPowerParam(w.param) || isSourcePower(w.param) || field == FxParam::Enabled || field == FxParam::AmpCabOn;
        const std::string count = std::to_string(w.b); // the partial editor's resolution
        const bool modeChoice = routeModeFrom(w.param) >= 0; // an operator's INPUTS row's, as glyphs
        const bool notchSwitch = isMorphMode(w.param); // a morph's, as the notch's icon
        const char *text = power || modeChoice || notchSwitch ? ""
                           : isPartialCount(w.param) || field == FxParam::ChorusVoices ? count.c_str()
                           : w.param == paramIndex(GlobalParam::Mono) ? tr(w.b == 0 ? Str::SwitchPoly : Str::SwitchMono)
                           : w.param == paramIndex(GlobalParam::Legato) ? tr(Str::SwitchLegato)
                           : field == FxParam::ShaperMode               ? tr(clipModes[std::clamp(w.b, 0, 2)])
                           : field == FxParam::AmpMic ? tr(kMicShortNames[std::clamp(w.b, 0, 2)])
                           : field == FxParam::MdRms                                            ? tr(Str::SwitchRms)
                           : field == FxParam::FlangerThroughZero ? tr(Str::SwitchThroughZero)
                           : isJoinSolo(w.param)                                                ? tr(Str::SwitchSolo)
                                                                                               : tr(Str::SwitchAlways);
        // LFO switches take their LFO's color, effect switches the effects' color.
        Color color = kAccent;
        if (sourceOf(w.param) >= 0)
            color = kSourceColors[sourceOf(w.param)];
        if (fxSlotOf(w.param) >= 0)
            color = fxColor(fxSlotOf(w.param));
        if (shaperOf(w.param) >= 0)
            color = kShaperColor;
        if (w.param >= kFilterBase && w.param < kFilterLinkBase) // a filter's
            color = kFilterColor;
        if (modeChoice)
            color = routeColor(routeModeFrom(w.param), routeModeTo(w.param));
        paintButtonFace(c, w, on, hovered, color, 0.22f, kRadiusControl);
        if (power)
            powerIcon(c, w.rect.center(), on ? litInk(color) : (hovered ? kText : kDim));
        else if (notchSwitch)
            filterIcon(c, {w.rect.center().x - 11.0f, w.rect.center().y - 5.0f, 22.0f, 10.0f}, FilterType::Notch,
                       on ? litInk(color) : (hovered ? kText : kDim));
        else if (modeChoice)
            routeModeIcon(c, w.rect.center(), static_cast<RouteMode>(w.b), on ? litInk(color) : (hovered ? kText : kDim));
        else if (opOfField(w.param, OpParam::Warp) >= 0) // a sine as the warp reads it
            warpIcon(c, {w.rect.center().x - 9.0f, w.rect.center().y - 5.0f, 18.0f, 10.0f}, static_cast<OpWarp>(w.b),
                     on ? litInk(color) : (hovered ? kText : kDim));
        else if (opOfField(w.param, OpParam::UnisonCurve) >= 0) // where five copies' pitches go
            uniCurveIcon(c, w.rect.center(), static_cast<UniCurve>(w.b), on ? litInk(color) : (hovered ? kText : kDim));
        else if (opOfField(w.param, OpParam::UnisonPan) >= 0) // and where they sit
            uniPanIcon(c, w.rect.center(), static_cast<UniPan>(w.b), on ? litInk(color) : (hovered ? kText : kDim));
        else if (fixedSwitchOp(w.param) >= 0) // a keyboard, crossed out while the operator doesn't follow it
            keyboardIcon(c, w.rect.center(), on ? litInk(color) : (hovered ? kText : kFaint), on);
        else if (shaperFieldOf(w.param) == ShaperParam::Link) // a voice shaper's link, as the shaper effect's
            fxSwitchIcon(c, w.rect.center(), FxParam::ShaperLink, on ? litInk(color) : (hovered ? kText : kDim));
        else if (shaperFieldOf(w.param) == ShaperParam::Symmetric)
            fxSwitchIcon(c, w.rect.center(), FxParam::ShaperSymmetric, on ? litInk(color) : (hovered ? kText : kDim));
        else if (field == FxParam::PhaserNegative || field == FxParam::FlangerNegative)
            polaritySign(c, w.rect.center(), w.b == 1, on ? litInk(color) : (hovered ? kText : kDim));
        else if (isFxIconSwitch(field))
            fxSwitchIcon(c, w.rect.center(), field, on ? litInk(color) : (hovered ? kText : kDim));
        else if (isLfoSync(w.param)) // an LFO's SYNC, the effects' metronome
            fxSwitchIcon(c, w.rect.center(), FxParam::DelaySync, on ? litInk(color) : (hovered ? kText : kDim));
        else if (clockMode)
            lfoClockIcon(c, w.rect.center(), static_cast<LfoClock>(w.b), on ? litInk(color) : (hovered ? kText : kDim),
                         switchIconScale());
        else if (sourceFieldOf(w.param) == SourceParam::Unipolar) // humps on a baseline, as a MODS row's; a Curve
                                                                   // card's choice either one
            polarityIcon(c, w.rect.center(), w.kind == Kind::Choice && w.b == 0,
                         on ? litInk(color) : (hovered ? kText : kDim), switchIconScale());
        else if (sourceFieldOf(w.param) == SourceParam::Bipolar) // a Random card's: either one, as a MODS row's
            polarityIcon(c, w.rect.center(), w.b == 1, on ? litInk(color) : (hovered ? kText : kDim), switchIconScale());
        else if (field == FxParam::PhaserStereo || field == FxParam::FlangerStereo || field == FxParam::AmpStereo)
            stereoIcon(c, w.rect.center(), on ? litInk(color) : (hovered ? kText : kDim)); // named in its tooltip
        else if (opOfField(w.param, OpParam::Sync) >= 0) // NOTE, named in its tooltip
            noteIcon(c, w.rect.center(), on ? litInk(color) : (hovered ? kText : kDim));
        else
            c.text(w.rect, text, kTextLabel, on ? litInk(kText) : kDim, Align::Center, on);
        break;
    }
    case Kind::ViewTab: {
        // Folder tabs. The one shown is part of its panel: filled like it, its outline running on into the panel's
        // top edge through curves that flare into it, the edge open beneath it, the accent along its top. The others
        // are their names, with a tab's shape faintly under the mouse. While an editor has the panels (shape, shaper
        // curve or partials), none of them is the one shown.
        const bool selected = controller_.layout().view == static_cast<RoutingView>(w.a) && shapeEditing() < 0 &&
                              partialEditing() < 0;
        if (selected) {
            if (look.panelShadow > 0.0f) { // its panel's soft shadow, round its top and sides, above the panel only
                c.pushClip({w.rect.x - 12.0f, w.rect.y - 12.0f, w.rect.w + 24.0f, w.rect.h + 12.0f});
                softShadow(c, w.rect, kRadiusCard, 4, 1.5f, 1.0f, look.panelShadow);
                c.popClip();
            }
            const std::vector<Point> outline =
                viewTabOutline({w.rect.x + 0.5f, w.rect.y + 0.5f, w.rect.w - 1.0f, w.rect.h}, kViewTabFlare);
            std::vector<Point> fill = outline; // down past the panel's border, to open it without a seam left
            fill.push_back({outline.back().x, w.rect.bottom() + 2.0f});
            fill.push_back({outline.front().x, w.rect.bottom() + 2.0f});
            c.fillPolygon(fill.data(), fill.size(), kPanel); // (the panel's corner its side runs down: see paintPanels())
            if (look.panelShadow <= 0.0f) // outlined as its panel is
                c.polyline(outline.data(), outline.size(), kBorder, 1.0f);
            c.line({w.rect.x + kViewTabRadius, w.rect.y + 2.0f}, {w.rect.right() - kViewTabRadius, w.rect.y + 2.0f},
                   kAccent, 2.0f);
        } else if (hovered) {
            const std::vector<Point> outline = viewTabOutline({w.rect.x, w.rect.y, w.rect.w, w.rect.h - 0.5f}, 0.0f);
            c.fillPolygon(outline.data(), outline.size(), kPanel.alpha(0.6f));
        }
        c.text(w.rect, tr(kViewNames[w.a]), kTextBody, selected || hovered ? kText : kDim, Align::Center, true);
        // A dot in its upper right while what it shows is at work: FX while an effect in the line is switched on, SEQ
        // while the sequencer is.
        const auto shows = static_cast<RoutingView>(w.a);
        bool working = false;
        if (shows == RoutingView::Fx) {
            const std::vector<int> chain = fxChain();
            working = std::any_of(chain.begin(), chain.end(),
                                  [&](int slot) { return value(paramIndex(slot, FxParam::Enabled)) >= 0.5; });
        } else if (shows == RoutingView::Seq) {
            working = value(paramIndex(SeqParam::Enabled)) >= 0.5;
        }
        if (working)
            c.fillCircle(viewTabDot(w.a), kTabDotRadius, kAccent);
        break;
    }
    case Kind::SourceMenuButton: { // and how many places are taken after it, as the effects' beside + EFFECT
        const bool full = !sourceShadow();
        paintMenuButton(c, w.rect, tr(Str::ButtonSource), hovered, sourceMenuOpen_ && sourceMenuAt_.x < 0.0f, full);
        c.text({w.rect.right() + 10.0f, w.rect.y, 44.0f, w.rect.h},
               std::to_string(sources().size()) + "/" + std::to_string(kMaxSources), kTextBody, full ? kText : kDim,
               Align::Left, full);
        break;
    }
    case Kind::Scrollbar: {
        // The widget is the track with some room around it, so the thin bar is easy to hit. Away from the mouse it
        // is drawn thinner still.
        const auto area = static_cast<ScrollArea>(w.a);
        const float thin = hovered || active ? 0.0f : 1.0f;
        const Rect track = w.rect.inset(4.0f), thumb = scrollThumb(area);
        c.fillRect({track.x, track.y + thin, track.w, track.h - 2.0f * thin}, kRaised, kRadiusMark - thin);
        c.fillRect({thumb.x, thumb.y + thin, thumb.w, thumb.h - 2.0f * thin}, hovered || active ? kDim : kFaint,
                   kRadiusMark - thin);
        break;
    }
    case Kind::SourceGrip:
        break; // drawn with its card
    case Kind::DynBand:
        break; // drawn with its display
    case Kind::DynZone:
        break; // drawn with its display
    case Kind::DynGrip: // its line, drawn with its display, lit while under the mouse or dragged
        if (hovered || active) {
            const Rect bar = mdBar(mdDisplay(w.a), w.b / 2);
            const float x = mdBarX(bar, value(mdThreshold(w.a, w.b)));
            c.line({x, bar.y - 2.5f}, {x, bar.bottom() + 2.5f}, kText, 2.0f);
        }
        break;
    case Kind::KeysToggle: {
        // A chevron pointing down while open, right while collapsed, and the strip's title beside it.
        const bool collapsed = controller_.layout().keysCollapsed;
        paintChevron(c, {w.rect.x + 9.0f, w.rect.center().y}, hovered ? kText : kDim,
                     collapsed ? ChevronWay::Right : ChevronWay::Down);
        c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, tr(Str::HeadingKeyboard), kTextBody,
               hovered ? kText : kDim, Align::Left, true);
        break;
    }
    case Kind::SourceHandle:
    case Kind::MacroHandle: {
        // A switched-off source's is faint, like its rings on knobs, and so is any source's while it modulates
        // nothing; either can still be dragged onto a target.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a].alpha(sourceLive(w.a) ? 1.0f : kIdleHandleOpacity);
        if (hovered || active)
            c.fillCircle(center, 13.0f, color.alpha(0.25f));
        c.fillCircle(center, 8.5f, color);
        c.fillCircle(center, 3.5f, kPanel);
        break;
    }
    case Kind::SlotBar: {
        const Color color = kSourceColors[w.a].alpha(mods_.slotBypassed(w.a, w.b) ? 0.4f : 1.0f);
        const double amount = value(w.param);
        const Rect track = slotBarTrack(w.rect);
        const float cy = track.center().y;
        const float mid = track.center().x;
        const float x = mid + static_cast<float>(amount) * track.w * 0.5f;
        c.fillRect({track.x, cy - 2.0f, track.w, 4.0f}, kTrack, kRadiusMark);
        c.fillRect({std::min(mid, x), cy - 2.0f, std::abs(x - mid), 4.0f}, color, kRadiusMark);
        c.line({mid, cy - 4.0f}, {mid, cy + 4.0f}, kFaint, 1.0f);
        c.fillCircle({x, cy}, hovered || active ? 5.0f : 4.0f, color);
        // Where the modulation takes its target now, on the bar's scale: a tick that swings with its source.
        if (const std::optional<double> now = liveSlotOffset(w.a, w.b)) {
            const float at = mid + static_cast<float>(std::clamp(*now, -1.0, 1.0)) * track.w * 0.5f;
            c.line({at, cy - 4.5f}, {at, cy + 4.5f}, kBackground, 3.0f);
            c.line({at, cy - 4.0f}, {at, cy + 4.0f}, kText, 1.5f);
        }
        // Another source modulating this depth shows how far it takes it, under the bar.
        paintModRange(c, {track.x, cy + 6.0f, track.w, 1.5f}, w.param);
        char text[16];
        std::snprintf(text, sizeof(text), "%+.0f%%", amount * 100.0);
        c.text({w.rect.right() + 2.0f, w.rect.y, 38.0f, w.rect.h}, text, kTextLabel, kDim, Align::Right);
        break;
    }
    case Kind::SlotRemove: {
        const Point center = w.rect.center();
        const Color color = hovered ? kText : kFaint;
        c.line({center.x - 3.5f, center.y - 3.5f}, {center.x + 3.5f, center.y + 3.5f}, color, 1.5f);
        c.line({center.x - 3.5f, center.y + 3.5f}, {center.x + 3.5f, center.y - 3.5f}, color, 1.5f);
        break;
    }
    case Kind::SourceMods: {
        // A link, without a face: how many modulations the source has, dim, and MODS with a chevron at the right in
        // the accent; with none, No modulations alone. Both light up under the mouse, the whole row taking the click.
        const size_t count = usedSlots(w.a).size();
        const Color ink = hovered ? kText : kDim;
        if (count == 0) {
            c.text(w.rect, tr(Str::CardNoModulations), kTextLabel, ink, Align::Left);
            break;
        }
        const Color link = hovered ? kText : kAccent;
        paintChevron(c, {w.rect.right() - 4.0f, w.rect.center().y}, link, ChevronWay::Right);
        c.text({w.rect.x, w.rect.y, w.rect.w - 80.0f, w.rect.h},
               count == 1 ? tr(Str::ModsCountOne) : trf(Str::ModsCount, {std::to_string(count)}), kTextLabel, ink,
               Align::Left);
        c.text({w.rect.right() - 80.0f, w.rect.y, 68.0f, w.rect.h}, tr(Str::ViewMods), kTextLabel, link, Align::Right,
               true);
        break;
    }
    case Kind::ValueText:
        break; // drawn by its slot's bar
    case Kind::UndoButton: {
        const bool redo = w.a == 1;
        const bool can = (redo ? controller_.nextRedo() : controller_.nextUndo()) != nullptr;
        const Color color = paintActionFace(c, w.rect, hovered, can);
        // An arrow curving over the top, its head at the left end (undo) or the right (redo), pointing down.
        const Point m{w.rect.center().x, w.rect.center().y + 2.0f};
        constexpr float kEnd = 1.9f, kRadius = 5.5f;
        c.arc(m, kRadius, -kEnd, kEnd, color, 1.5f);
        const float angle = redo ? kEnd : -kEnd;
        const Point end = Canvas::onCircle(m, kRadius, angle);
        // Clockwise along the circle is (cos, sin); undo runs the other way.
        const float way = redo ? 1.0f : -1.0f;
        const Point along{way * std::cos(angle), way * std::sin(angle)}, side{-along.y, along.x};
        const Point head[] = {{end.x + along.x * 3.5f, end.y + along.y * 3.5f},
                              {end.x + side.x * 3.0f, end.y + side.y * 3.0f},
                              {end.x - side.x * 3.0f, end.y - side.y * 3.0f}};
        c.fillPolygon(head, std::size(head), color);
        break;
    }
    case Kind::EnvPoint: {
        const Point center = w.rect.center();
        const Color color = w.a < kOps ? kAccent : kSourceColors[w.a - kOps];
        c.fillCircle(center, hovered || active ? 5.5f : 4.0f, hovered || active ? color : kPanel);
        c.strokeCircle(center, hovered || active ? 5.5f : 4.0f, color, 1.8f);
        break; // its readout goes over the other handles (paintEnvReadout())
    }
    case Kind::FilterPoint: // the filter effect's (a voice filter's is the inspector's)
        paintFilterHandle(c, w.rect.center(), hovered || active, fxColor(w.a), fxFilterArea(w.a), fxFilterParams(w.a),
                          [this](int index) { return value(index); });
        break;
    case Kind::MatrixLabel:
    case Kind::NoteDisplay:
        break; // drawn with their card, editor, matrix or the keyboard strip
    case Kind::FxFold:
    case Kind::SourceFold: {
        // Unfolded, a chevron in the title row that folds the card, shown while the card is under the mouse; folded,
        // the strip below the cross, lit while hovered, which unfolds it.
        const bool folded = w.kind == Kind::FxFold ? fxFolded(w.a) : sourceFolded(w.a);
        if (folded) {
            if (hovered)
                c.fillRect(w.rect.inset(3.0f), kText.alpha(0.06f), kRadiusControl);
            break;
        }
        if (!hovered && !cardFoldShown(w.kind == Kind::FxFold ? kMaxSources + w.a : w.a))
            break;
        paintTitleActionFace(c, w.rect, hovered);
        paintChevron(c, w.rect.center(), hovered ? kText : kDim, ChevronWay::Left);
        break;
    }
    case Kind::MacroName: // the knob draws the name; hovered, a line under it says it can be changed
        if (hovered && !typing_)
            c.line({w.rect.x + 14.0f, w.rect.bottom() - 1.0f}, {w.rect.right() - 14.0f, w.rect.bottom() - 1.0f},
                   kFaint, 1.0f, true);
        break;
    case Kind::FxShadow:
    case Kind::SourceShadow: {
        // A dashed outline with a plus in it and what it adds under that, faint at rest so it stays behind the cards,
        // lit while hovered in the theme's accent, like + EFFECT and + SOURCE.
        const bool fx = w.kind == Kind::FxShadow;
        const float arm = 12.0f;
        const Point m{w.rect.center().x, w.rect.center().y - arm};
        const Color ink = paintAddCard(c, w.rect, m, arm, hovered);
        const char *what = tr(fx ? Str::ButtonAddEffect : Str::ButtonAddSource);
        c.text({w.rect.x, m.y + arm + 6.0f, w.rect.w, 16.0f}, what, kTextLabel, ink, Align::Center, true);
        break;
    }
    case Kind::SourceMenu:
        if (w.a == 1) { // the list's panel
            paintMenuPanel(c, w.rect);
        }
        break;
    case Kind::SourceMenuItem: {
        // A small picture of what the type puts out, then its name.
        const auto type = static_cast<SourceType>(w.a);
        paintMenuRow(c, w.rect, hovered, false);
        const Color ink = hovered ? kText : kDim;
        const Rect icon{w.rect.x + 9.0f, w.rect.y + 6.0f, 18.0f, 12.0f};
        const float l = icon.x, r = icon.right(), t = icon.y, b = icon.bottom();
        if (type == SourceType::Lfo) {
            const std::vector<Point> wave = waveIcon(OpWave::Sine, icon);
            c.polyline(wave.data(), wave.size(), ink, 1.4f);
        } else if (type == SourceType::Env) {
            const Point env[] = {{l, b}, {l + 4.0f, t}, {l + 9.0f, t + 6.0f}, {r - 4.0f, t + 6.0f}, {r, b}};
            c.polyline(env, 5, ink, 1.4f);
        } else if (type == SourceType::Curve) { // its drawn shape, as an LFO's Custom was
            lfoShapeIcon(c, icon, LfoShape::Custom, ink);
        } else if (type == SourceType::Random) {
            const Point steps[] = {{l, t + 8.0f},  {l + 5.0f, t + 8.0f}, {l + 5.0f, t + 2.0f}, {l + 10.0f, t + 2.0f},
                                   {l + 10.0f, b}, {l + 14.0f, b},        {l + 14.0f, t + 5.0f}, {r, t + 5.0f}};
            c.polyline(steps, 8, ink, 1.4f);
        } else { // a fader
            c.line({l + 2.0f, (t + b) * 0.5f}, {r - 2.0f, (t + b) * 0.5f}, ink, 1.4f);
            c.fillRect({l + 9.0f, t + 1.0f, 4.0f, b - t - 2.0f}, ink, kRadiusMark);
        }
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, sourceTypeName(type), kTextBody, ink,
               Align::Left);
        break;
    }
    case Kind::FxRemove:
    case Kind::FxNodeRemove:
    case Kind::SourceRemove: {
        paintTitleActionFace(c, w.rect, hovered); // a small action, as the fold
        const Point center = w.rect.center();
        const Color color = hovered ? kText : kFaint;
        c.line({center.x - 4.0f, center.y - 4.0f}, {center.x + 4.0f, center.y + 4.0f}, color, 1.5f);
        c.line({center.x - 4.0f, center.y + 4.0f}, {center.x + 4.0f, center.y - 4.0f}, color, 1.5f);
        break;
    }
    case Kind::AmpMic: { // a capsule pointing at the cone, its front its color; ringed under the mouse
        const Color color = fxColor(w.a);
        Point m{w.rect.x + 9.0f, w.rect.y + 8.0f};
        if (const std::optional<Rect> card = fxCardOf(w.a)) { // where modulation has it, as the cable is drawn
            const Rect live = ampMicRect(ampCabDisplay(*card), liveValue(paramIndex(w.a, FxParam::AmpPosition)),
                                         liveValue(paramIndex(w.a, FxParam::AmpDistance)));
            m = {live.x + 9.0f, live.y + 8.0f};
        }
        if (hovered || active)
            c.strokeCircle({m.x + 3.0f, m.y}, 10.0f, color.alpha(0.45f), 3.0f);
        c.line({m.x, m.y}, {m.x + 4.0f, m.y}, kDim, 1.0f);
        c.fillRect({m.x - 3.0f, m.y - 3.5f, 13.0f, 7.0f}, color.alpha(0.3f), kRadiusMark);
        c.strokeRect({m.x - 3.0f, m.y - 3.5f, 13.0f, 7.0f}, color, 1.3f, kRadiusMark);
        c.fillCircle({m.x - 3.0f, m.y}, 3.5f, color);
        break;
    }
    case Kind::UtilityHandle: { // where modulation has it; a cap across the Gain strip, a dot for the others
        const Color color = fxColor(w.a);
        Point h = w.rect.center();
        if (const std::optional<Rect> card = fxCardOf(w.a))
            h = utilityHandlePoint(fxDisplayIn(*card, FxType::Utility), w.b, true, w.a);
        const bool focused = hovered || active;
        const Color edge = w.b == 3 && value(w.param) <= 0.0 ? kDim : color; // Mono Bass off
        if (w.b == 0) {
            const Rect cap{h.x - 6.0f, h.y - 2.5f, 12.0f, 5.0f};
            if (focused)
                c.strokeRect(cap.inset(-3.0f), edge.alpha(0.45f), 3.0f, kRadiusMark + 3.0f);
            c.fillRect(cap, focused ? edge : kText, kRadiusMark);
            break;
        }
        if (focused)
            c.strokeCircle(h, 9.0f, edge.alpha(0.45f), 3.0f);
        c.fillCircle(h, 4.5f, focused ? edge : kDisplayColor);
        c.strokeCircle(h, 4.5f, edge, 1.5f);
        break;
    }
    case Kind::PeqPoint: {
        const Point center = w.rect.center();
        // Ringed for the band whose knobs show; faint for one switched off, which can still be moved.
        const bool focused = hovered || active;
        const bool on = value(peqOnIndex(w.a, w.b)) >= 0.5;
        const Color color = on ? kSourceColors[w.b] : kFaint;
        if (std::min(fxPanel_.band(w.a), kPeqBands - 1) == w.b && on)
            c.strokeCircle(center, 10.5f, color.alpha(0.45f), 3.0f);
        c.fillCircle(center, focused ? 7.5f : 6.5f, focused ? color : kPanel);
        c.strokeCircle(center, focused ? 7.5f : 6.5f, color, on ? 1.8f : 1.2f);
        c.text({center.x - 8.0f, center.y - 7.0f, 16.0f, 14.0f}, std::to_string(w.b + 1), kTextLabel,
               focused ? kBackground : (on ? kText : kFaint), Align::Center, true);
        if (focused) {
            char freq[32], gain[32], q[32], text[112];
            formatParam(peqFreqIndex(w.a, w.b), value(peqFreqIndex(w.a, w.b)), freq, sizeof(freq));
            formatParam(peqGainIndex(w.a, w.b), value(peqGainIndex(w.a, w.b)), gain, sizeof(gain));
            formatParam(peqQIndex(w.a, w.b), value(peqQIndex(w.a, w.b)), q, sizeof(q));
            if (!peqHasGain(peqType(value(peqTypeIndex(w.a, w.b))))) // a cut and a notch have no gain
                std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s", freq, q);
            else
                std::snprintf(text, sizeof(text), "%s  \xC2\xB7  %s  \xC2\xB7  %s", freq, gain, q);
            const Rect area = peqArea(w.a);
            c.text({area.x, area.y - 6.0f, area.w, 14.0f}, text, kTextLabel, kText, Align::Center);
        }
        break;
    }
    case Kind::ListButton: {
        // A slope shows as it plays (a band-pass's 6 dB as 12), and faintly for a comb, which has none.
        const int shown = listShown(w.param);
        const bool relevant =
            !isFilterSlope(w.param) || hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param)))));
        const bool lit = relevant && (hovered || listMenu_ == w.param);
        const Color color = listColor(w.param);
        if (look.flat) { // filled dark, without an outline, tinted with its color under the mouse
            c.fillRect(w.rect, wellColor(), kRadiusControl);
            if (lit)
                c.fillRect(w.rect, color.alpha(0.24f), kRadiusControl);
        } else {
            c.fillRect(w.rect, lit ? color.alpha(0.18f) : kRaised, kRadiusControl);
            c.strokeRect(w.rect, color.alpha(lit ? 0.9f : relevant ? 0.45f : 0.2f), 1.0f, kRadiusControl);
        }
        const Color text = !relevant ? kFaint : lit ? kText : kDim;
        // An LFO's shape and a filter's type show their icons before their names.
        const bool filterType = isFilterTypeList(w.param);
        const float indent = isLfoShape(w.param) ? 24.0f : filterType ? 30.0f : 0.0f;
        // A list with arrows has them at its ends (see the ListStep widgets), and no chevron.
        const bool steps = isSteppableList(w.param);
        const float left = w.rect.x + (steps ? kListStepW + 2.0f : 10.0f);
        if (isLfoShape(w.param))
            lfoShapeIcon(c, {left, w.rect.center().y - 5.0f, 16.0f, 10.0f}, static_cast<LfoShape>(shown), color);
        if (filterType) {
            const Rect icon = filterTypeIconRect(w.rect);
            filterIcon(c, {icon.x + left - (w.rect.x + 10.0f), icon.y, icon.w, icon.h}, static_cast<FilterType>(shown),
                       color);
        }
        const float right = w.rect.right() - (steps ? kListStepW + 2.0f : 20.0f);
        c.text({left + indent, w.rect.y, right - left - indent, w.rect.h}, listItemName(w.param, shown),
               kTextBody, text, Align::Left, true);
        if (!steps)
            paintChevron(c, {w.rect.right() - 13.0f, w.rect.center().y}, text);
        break;
    }
    case Kind::ListStep: { // an arrow pointing the way it steps, lit under the mouse
        const Color color = listColor(w.param);
        if (hovered)
            c.fillRect(w.rect, color.alpha(look.flat ? 0.24f : 0.18f), kRadiusMark);
        const Point m = w.rect.center();
        const float d = w.b > 0 ? 1.0f : -1.0f;
        const Point arrow[] = {{m.x - 1.5f * d, m.y - 4.0f}, {m.x + 2.0f * d, m.y}, {m.x - 1.5f * d, m.y + 4.0f}};
        c.polyline(arrow, 3, hovered ? kText : kDim, 1.5f);
        break;
    }
    case Kind::ListMenu:
        if (w.a == 1) {
            paintMenuPanel(c, w.rect);
        }
        break;
    case Kind::PeqBand: {
        // The chip: on a plate ringed in the band's color while its knobs show, its number in that color while it
        // plays, and a line between the number and the shape.
        const bool picked = std::min(fxPanel_.band(w.a), kPeqBands - 1) == w.b;
        const bool on = value(peqOnIndex(w.a, w.b)) >= 0.5;
        const bool chipHovered = hover_ && hover_->a == w.a && hover_->b == w.b &&
                                 (hover_->kind == Kind::PeqBand || hover_->kind == Kind::PeqBandOn ||
                                  hover_->kind == Kind::PeqTypeButton);
        const Color color = kSourceColors[w.b];
        if (look.flat) { // as the switches: in a well without an outline, the picked one tinted with its band's color
            c.fillRect(w.rect, chipHovered && !picked ? wellHover() : wellColor(), kRadiusControl);
            if (picked)
                c.fillRect(w.rect, color.alpha(on ? 0.3f : 0.15f), kRadiusControl);
        } else {
            c.fillRect(w.rect, picked ? kTrack : (chipHovered ? kRaised : kCardColor), kRadiusControl);
            c.strokeRect(w.rect, picked ? color.alpha(on ? 0.9f : 0.5f) : kBorder, 1.0f, kRadiusControl);
        }
        // The number midway between the power symbol's edge (its ring 5 px about its middle, and its stroke) and the
        // line, raised 1.5 px so its middle meets the symbol's (whose stroke reaches above its ring).
        const Rect power = peqChipPower(w.rect), shape = peqChipShape(w.rect);
        const float left = power.center().x + 6.0f;
        c.text({left, w.rect.y - 1.5f, shape.x - left, w.rect.h}, std::to_string(w.b + 1), kTextHeading,
               !on ? kFaint : picked ? kText : color, Align::Center, true);
        c.line({shape.x, w.rect.y + 6.0f}, {shape.x, w.rect.bottom() - 6.0f}, kBorder, 1.0f);
        break;
    }
    case Kind::PeqBandOn: {
        const bool on = value(w.param) >= 0.5;
        powerIcon(c, peqChipPower(w.rect).center(), on ? kSourceColors[w.b] : (hovered ? kText : kFaint));
        break;
    }
    case Kind::PeqTypeButton: {
        // The band's shape on its chip, lit under the mouse and while its list is open.
        const bool lit = hovered || peqMenu_ == w.param;
        const bool on = value(peqOnIndex(w.a, w.b)) >= 0.5;
        if (lit)
            c.fillRect(w.rect.inset(3.0f), kSourceColors[w.b].alpha(0.18f), kRadiusMark);
        peqIcon(c, {w.rect.x + 7.0f, w.rect.y + 7.0f, w.rect.w - 14.0f, w.rect.h - 14.0f}, peqType(value(w.param)),
                on ? kSourceColors[w.b] : (lit ? kText : kFaint));
        break;
    }
    case Kind::PeqTypeMenu:
        if (w.a == 1) {
            paintMenuPanel(c, w.rect);
        }
        break;
    case Kind::PeqTypeItem: {
        const bool selected = peqType(value(w.param)) == static_cast<PeqType>(w.b);
        paintMenuRow(c, w.rect, hovered, selected);
        const int band = peqBandOfType(w.param);
        peqIcon(c, {w.rect.x + 8.0f, w.rect.y + 5.0f, 22.0f, w.rect.h - 10.0f}, static_cast<PeqType>(w.b),
                selected ? kSourceColors[band] : kDim);
        c.text({w.rect.x + 38.0f, w.rect.y, w.rect.w - 44.0f, w.rect.h}, peqTypeName(static_cast<PeqType>(w.b)), kTextBody,
               hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::ListItem: {
        // A band-pass's or notch's list shows the 6 and 18 dB slopes it doesn't come in faintly, and can't pick them.
        const bool fits = listItemFits(w.param, w.b);
        const bool selected = listShown(w.param) == w.b;
        paintMenuRow(c, w.rect, hovered && fits, selected);
        const bool modeChoice = routeModeFrom(w.param) >= 0;
        const bool filterType = isFilterTypeList(w.param);
        const float indent = isLfoShape(w.param) || modeChoice ? 24.0f : filterType ? 30.0f : 0.0f;
        if (isLfoShape(w.param))
            lfoShapeIcon(c, {w.rect.x + 10.0f, w.rect.center().y - 5.0f, 16.0f, 10.0f}, static_cast<LfoShape>(w.b),
                         selected ? listColor(w.param) : kDim);
        if (filterType)
            filterIcon(c, filterTypeIconRect(w.rect), static_cast<FilterType>(w.b), selected ? listColor(w.param) : kDim);
        if (modeChoice)
            routeModeIcon(c, {w.rect.x + 16.0f, w.rect.center().y}, static_cast<RouteMode>(w.b),
                          selected ? listColor(w.param) : kDim);
        c.text({w.rect.x + 10.0f + indent, w.rect.y, w.rect.w - 20.0f - indent, w.rect.h}, listItemName(w.param, w.b),
               kTextBody, !fits ? kFaint : hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::EnvCurve: {
        const bool focused = hovered || active;
        const Color color = w.a < kOps ? kAccent : kSourceColors[w.a - kOps];
        c.fillCircle(w.rect.center(), focused ? 4.5f : 3.0f, focused ? kText : color.alpha(0.6f));
        break; // its readout goes over the handles (paintEnvReadout())
    }
    case Kind::Key:
        paintKey(c, w, hovered, active);
        break;
    case Kind::BendWheel:
    case Kind::ModWheel:
        paintWheel(c, w, hovered, active);
        break;
    case Kind::PresetButton: {
        // Save and Save As greyed out while they have nothing to save.
        const Color color = paintActionFace(c, w.rect, hovered, presetActionOn(static_cast<PresetAction>(w.a)));
        const Point m = w.rect.center();
        switch (static_cast<PresetAction>(w.a)) {
        case PresetAction::Init:
            resetIcon(c, m, color);
            break;
        case PresetAction::Load: {
            // An open folder: a tab on the back, the front tilted forward.
            const Point back[] = {{m.x - 7.0f, m.y + 5.0f}, {m.x - 7.0f, m.y - 5.0f}, {m.x - 2.5f, m.y - 5.0f},
                                  {m.x - 1.0f, m.y - 3.5f}, {m.x + 5.0f, m.y - 3.5f}, {m.x + 5.0f, m.y - 1.0f}};
            const Point front[] = {{m.x - 7.0f, m.y + 5.0f}, {m.x - 4.5f, m.y - 1.0f}, {m.x + 7.5f, m.y - 1.0f},
                                   {m.x + 5.0f, m.y + 5.0f}, {m.x - 7.0f, m.y + 5.0f}};
            c.polyline(back, std::size(back), color, 1.4f);
            c.polyline(front, std::size(front), color, 1.4f);
            break;
        }
        case PresetAction::Save: {
            // A floppy disk: a clipped corner, the shutter at the top and the label below.
            const Point body[] = {{m.x - 6.5f, m.y - 6.5f}, {m.x + 3.5f, m.y - 6.5f}, {m.x + 6.5f, m.y - 3.5f},
                                  {m.x + 6.5f, m.y + 6.5f}, {m.x - 6.5f, m.y + 6.5f}, {m.x - 6.5f, m.y - 6.5f}};
            c.polyline(body, std::size(body), color, 1.4f);
            c.fillRect({m.x - 3.5f, m.y - 6.5f, 6.0f, 3.5f}, color);
            c.strokeRect({m.x - 3.5f, m.y + 1.0f, 7.0f, 5.5f}, color, 1.2f);
            break;
        }
        case PresetAction::SaveAs: {
            // The same disk, its bottom right corner left open for a plus: a new file.
            const Point body[] = {{m.x + 6.5f, m.y + 1.0f}, {m.x + 6.5f, m.y - 3.5f}, {m.x + 3.5f, m.y - 6.5f},
                                  {m.x - 6.5f, m.y - 6.5f}, {m.x - 6.5f, m.y + 6.5f}, {m.x + 1.0f, m.y + 6.5f}};
            c.polyline(body, std::size(body), color, 1.4f);
            c.fillRect({m.x - 3.5f, m.y - 6.5f, 6.0f, 3.5f}, color);
            const Point plus{m.x + 5.0f, m.y + 5.0f};
            c.line({plus.x - 3.5f, plus.y}, {plus.x + 3.5f, plus.y}, color, 1.5f);
            c.line({plus.x, plus.y - 3.5f}, {plus.x, plus.y + 3.5f}, color, 1.5f);
            break;
        }
        }
        break;
    }
    case Kind::Octave: { // in a flat theme in a well, without an outline, as its switches
        if (look.flat) {
            c.fillRect(w.rect, hovered ? wellHover() : wellColor(), kRadiusMark);
        } else {
            c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusMark);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusMark);
        }
        // A minus or a plus, drawn: a glyph this small sits off center.
        const Point m = w.rect.center();
        c.line({m.x - 3.0f, m.y}, {m.x + 3.0f, m.y}, kText, 1.2f);
        if (w.a > 0)
            c.line({m.x, m.y - 3.0f}, {m.x, m.y + 3.0f}, kText, 1.2f);
        break;
    }
    case Kind::SettingsButton: {
        const bool open = settingsOpen_;
        const Color color = paintActionFace(c, w.rect, hovered || open);
        // A cog: eight teeth around a ring.
        const Point center = w.rect.center();
        constexpr float kTau = 6.2831853f;
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = tooth * kTau / 8.0f;
            const float half = 0.2f;
            const Point outline[] = {Canvas::onCircle(center, 5.5f, angle - half * 1.3f),
                                     Canvas::onCircle(center, 8.0f, angle - half),
                                     Canvas::onCircle(center, 8.0f, angle + half),
                                     Canvas::onCircle(center, 5.5f, angle + half * 1.3f)};
            c.fillPolygon(outline, std::size(outline), color);
        }
        c.fillCircle(center, 6.0f, color);
        c.fillCircle(center, 2.6f, hovered || open ? kTrack : kRaised);
        break;
    }
    case Kind::SettingsBackdrop:
        break;
    case Kind::SettingsOption:
    case Kind::SettingsMpe:
    case Kind::SettingsMpeRange:
    case Kind::SettingsHandles:
    case Kind::SettingsCards: {
        // A row of buttons, the one picked lit. The bend ranges fade while MPE is off, though they still take clicks
        // (CARDS' buttons, disabled in a theme that outlines its cards, are dimmed by paintSettings()).
        const bool on = w.kind == Kind::SettingsOption    ? controller_.oversampling() == w.b
                        : w.kind == Kind::SettingsCards   ? controller_.editorSettings().tintedCards == (w.b != 0)
                        : w.kind == Kind::SettingsMpe     ? controller_.mpe() == (w.b != 0)
                        : w.kind == Kind::SettingsHandles ? std::clamp(controller_.editorSettings().handleSize, 0,
                                                                       kModHandleSizeCount - 1) == w.b
                                                          : controller_.mpeBendRange() == w.b;
        const bool faint = w.kind == Kind::SettingsMpeRange && !controller_.mpe();
        const Color accent = faint ? kDim : kAccent;
        paintButtonFace(c, w, on, hovered, accent, 0.22f, kRadiusControl); // in its row's pill, or on its own
        char factor[16];
        std::snprintf(factor, sizeof(factor), "%dx", w.b);
        static constexpr Str kHandleSizes[] = {Str::SettingsHandlesSmall, Str::SettingsHandlesMedium,
                                               Str::SettingsHandlesLarge};
        static_assert(std::size(kHandleSizes) == kModHandleSizeCount);
        const std::string label = w.kind == Kind::SettingsOption    ? std::string(factor)
                                  : w.kind == Kind::SettingsMpe     ? std::string(tr(w.b ? Str::ValueOn : Str::ValueOff))
                                  : w.kind == Kind::SettingsHandles ? std::string(tr(kHandleSizes[w.b]))
                                  : w.kind == Kind::SettingsCards
                                      ? std::string(tr(w.b ? Str::SettingsCardsTinted : Str::SettingsCardsPlain))
                                                                    : trf(Str::SettingsMpeRange, {std::to_string(w.b)});
        c.text(w.rect, label, kTextBody, on ? (faint ? litInk(kDim) : litInk(kText)) : kDim, Align::Center, on);
        break;
    }
    case Kind::SettingsHue: {
        // A track of every hue, the theme's main color as saturated and as light at each, and a knob at the hue shown
        // (the theme's own while none is picked).
        const Color own = kThemes[theme_].accent;
        const double hue = shownHue();
        const float at = hue >= 0.0 ? static_cast<float>(hue) : hueOf(own);
        const float y = w.rect.center().y;
        constexpr int kSteps = 48;
        const float step = w.rect.w / kSteps;
        c.fillRect({w.rect.x, y - 2.0f, w.rect.w, 4.0f}, kTrack, kRadiusMark);
        for (int k = 0; k < kSteps; ++k) // a little over each, so no seam shows between them
            c.fillRect({w.rect.x + k * step + (k == 0 ? 1.0f : 0.0f), y - 2.0f,
                        step + (k + 1 < kSteps ? 0.6f : -1.0f) - (k == 0 ? 1.0f : 0.0f), 4.0f},
                       withHue(own, (k + 0.5f) / kSteps));
        const float x = w.rect.x + at * w.rect.w;
        c.fillCircle({x, y}, 7.0f, hovered || active ? kText : kDim);
        c.fillCircle({x, y}, 3.0f, kAccent);
        break;
    }
    case Kind::SettingsGlow:
    case Kind::SettingsLinks:
    case Kind::SettingsGrid: {
        // A track filled up to the amount, and a knob to drag along it.
        const float amount = w.kind == Kind::SettingsLinks ? shownLinks()
                             : w.kind == Kind::SettingsGrid ? shownGrid()
                                                            : shownGlow();
        const float x = w.rect.x + amount * w.rect.w, y = w.rect.center().y;
        c.fillRect({w.rect.x, y - 2.0f, w.rect.w, 4.0f}, kTrack, kRadiusMark);
        c.fillRect({w.rect.x, y - 2.0f, x - w.rect.x, 4.0f}, kAccent, kRadiusMark);
        c.fillCircle({x, y}, 7.0f, hovered || active ? kText : kDim);
        c.fillCircle({x, y}, 3.0f, kAccent);
        break;
    }
    case Kind::SettingsTheme: {
        // Each theme in its own colors: its panel, its name, and its operator, filter and effect colors.
        const Palette &theme = kThemes[w.b];
        const bool on = themeIndex(controller_.editorSettings().theme) == w.b;
        c.fillRect(w.rect, hovered && !on ? theme.raised : theme.panel, kRadiusControl);
        c.strokeRect(w.rect, on ? kAccent : (hovered ? kDim : kBorder), on ? 1.5f : 1.0f, kRadiusControl);
        c.text({w.rect.x, w.rect.y, w.rect.w, w.rect.h - 4.0f}, tr(theme.label), kTextLabel, theme.text, Align::Center, on);
        const float third = (w.rect.w - 16.0f) / 3.0f;
        const Color stripe[] = {theme.accent, theme.filter, theme.fx};
        for (int i = 0; i < 3; ++i)
            c.fillRect({w.rect.x + 8.0f + i * third, w.rect.bottom() - 7.0f, third, 3.0f}, stripe[i]);
        break;
    }
    case Kind::ConfirmBackdrop:
    case Kind::BrowserBackdrop:
        break;
    case Kind::SearchField: // the panels' own: the browser's and the MODS view's drawn by them
        break;
    case Kind::TempoSync: { // a switch, lit while the synth follows the host's tempo
        const bool on = controller_.tempoSync();
        paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, tr(Str::TempoSyncLetter), kTextValue, on ? litInk(kText) : hovered ? kText : kDim, Align::Center,
               true);
        break;
    }
    case Kind::TempoBox:
    case Kind::TempoHost: {
        // The tempo and its unit, centered: the synth's own, or dimmed and lit by nothing, the host's it follows, as
        // the synth last played it.
        const bool host = w.kind == Kind::TempoHost;
        const bool lit = !host && (hovered || active);
        c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
        if (outlined())
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        if (typing_ && typing_->widget.kind == Kind::TempoBox)
            break; // the field being typed in covers the number
        const double bpm = host ? static_cast<double>(telemetry_.tempo) : controller_.ownTempo();
        const std::string number = tempoText(bpm);
        const char *unit = "BPM";
        const float numberW = c.textWidth(number, kTextValue, true), unitW = c.textWidth(unit, kTextLabel);
        const float x = w.rect.x + (w.rect.w - numberW - 4.0f - unitW) * 0.5f;
        c.text({x, w.rect.y, numberW + 1.0f, w.rect.h}, number, kTextValue, host ? kDim : kText, Align::Left, true);
        c.text({x + numberW + 4.0f, w.rect.y + 1.0f, unitW + 1.0f, w.rect.h}, unit, kTextLabel, kFaint, Align::Left);
        break;
    }
    case Kind::PresetStep: // the chevron points the way it steps
        paintChevron(c, w.rect.center(), paintActionFace(c, w.rect, hovered),
                     w.a < 0 ? ChevronWay::Left : ChevronWay::Right);
        break;
    case Kind::SearchClear: { // the target menu's drawn with its menu (see ModsPanel)
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.5f, m.y - 3.5f}, {m.x + 3.5f, m.y + 3.5f}, color, 1.3f);
        c.line({m.x - 3.5f, m.y + 3.5f}, {m.x + 3.5f, m.y - 3.5f}, color, 1.3f);
        break;
    }
    case Kind::ConfirmButton: {
        // A button's face: lit in `color` (the way ahead, or a band's), else plain; in a flat theme, as its switches, filled
        // solid with the color or set in a well, without an outline. Gives the ink for its label.
        auto face = [&](bool lit, Color color) {
            if (look.flat) {
                c.fillRect(w.rect, lit ? (hovered ? mix(color, kText, 0.15f) : color) : hovered ? wellHover() : wellColor(),
                           kRadiusControl);
                return lit ? litInk(kText) : kText;
            }
            c.fillRect(w.rect, lit ? color.alpha(hovered ? 0.35f : 0.22f) : (hovered ? kTrack : kRaised), kRadiusControl);
            c.strokeRect(w.rect, lit ? color : kBorder, 1.0f, kRadiusControl);
            return kText;
        };
        // A panel's question's words: its third way (a split removed with its bands' effects), and its choices (a
        // split's middle band's effects onto its low band or its high one), each in its color.
        const std::optional<Panel::ConfirmWords> words =
            confirmFrom_ ? std::optional(confirmFrom_->confirmWords(confirm_)) : std::nullopt;
        if (w.a == 4 && words) {
            c.text(w.rect, words->third, kTextBody, face(false, kAccent), Align::Center);
            break;
        }
        if (w.a >= 2 && words && w.a - 2 < static_cast<int>(words->choices.size())) {
            const auto &[label, color] = words->choices[static_cast<size_t>(w.a - 2)];
            const Color ink = face(true, color);
            c.text(w.rect, label, kTextBody, ink, Align::Center, true);
            break;
        }
        const bool ok = w.a == 1;
        const bool lit = ok && (!words || words->lit); // none lit where no way is more likely than the others
        const Color ink = face(lit, kAccent);
        const Str ahead = confirm_ == static_cast<int>(PresetAction::Init) ? Str::ButtonInit
                          : confirm_ == static_cast<int>(PresetAction::Save) ? Str::ButtonOverwrite
                                                                             : Str::ButtonLoad;
        const std::string label = !ok          ? std::string(tr(Str::ButtonCancel))
                                  : words        ? words->ok // a panel's question's
                                                 : std::string(tr(ahead));
        c.text(w.rect, label, kTextBody, ink, Align::Center, lit);
        break;
    }
    case Kind::SettingsClose: {
        c.fillRect(w.rect, hovered ? kTrack : kPanel, kRadiusControl);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 5.0f, m.y - 5.0f}, {m.x + 5.0f, m.y + 5.0f}, color, 1.5f);
        c.line({m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y - 5.0f}, color, 1.5f);
        break;
    }
    case Kind::SettingsManual:
        bookIcon(c, w.rect.center(), paintActionFace(c, w.rect, hovered));
        break;
    case Kind::ModDot: {
        // Compact: on a wire label, matrix cell or depth bar.
        // A bypassed modulation's dot is hollow.
        const Point center = w.rect.center();
        const ModHandleSize &sizes = modHandleSize(controller_.editorSettings().handleSize);
        const float radius = hovered || active ? (w.compact ? sizes.dotCompactHover : sizes.dotHover)
                                               : (w.compact ? sizes.dotCompact : sizes.dot);
        const Color color = kSourceColors[w.a].alpha(mods_.sourceOn(w.a) ? 1.0f : 0.35f);
        if (mods_.slotBypassed(w.a, w.b))
            c.strokeCircle(center, radius - 0.6f, color, 1.2f);
        else
            c.fillCircle(center, radius, color);
        // With an aux, a ring round it in the aux's color: as much of it as the aux depth, clockwise from the top, the
        // rest faint.
        if (const int aux = slotAux([this](int index) { return value(index); }, w.a, w.b); aux >= 0)
            paintAuxRing(c, center, radius + 1.8f, aux, value(slotAuxDepthIndex(w.a, w.b)));
        break;
    }
    case Kind::ModPill:
        paintModPill(c, w, hovered);
        break;
    case Kind::ModList:
        paintModList(c, w);
        break;
    case Kind::ModListRow:
        paintModListRow(c, w);
        break;
    case Kind::ModsAuxDepth: {
        // The aux depth's number, lit under the mouse and while dragged (the chip and its bar are its aux's).
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f%%", value(w.param) * 100.0);
        c.text(w.rect, text, kTextLabel, hovered || active ? kText : kDim, Align::Right);
        break;
    }
    case Kind::ModsAuxClear: {
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.0f, m.y - 3.0f}, {m.x + 3.0f, m.y + 3.0f}, color, 1.3f);
        c.line({m.x - 3.0f, m.y + 3.0f}, {m.x + 3.0f, m.y - 3.0f}, color, 1.3f);
        break;
    }
    case Kind::ModListScroll: { // the track is 3 px in from the widget's left, 4 px wide
        const bool lit = hovered || active;
        const Rect track{w.rect.x + 3.0f, w.rect.y, 4.0f, w.rect.h};
        c.fillRect(track, kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(modListThumb(track), lit ? kDim : kFaint, kRadiusMark);
        break;
    }
    case Kind::SlotBypass: {
        // The row's dot in the source's color: filled while it modulates, hollow while bypassed.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a];
        if (hovered)
            c.fillCircle(center, 6.5f, color.alpha(0.2f));
        if (mods_.slotBypassed(w.a, w.b))
            c.strokeCircle(center, 3.2f, color, 1.3f);
        else
            c.fillCircle(center, 3.5f, color);
        break;
    }
    case Kind::SourcePolarity: {
        // How the row's source swings its targets: a wave about its middle line for both ways, or humps up from its
        // baseline for one way.
        if (hovered)
            c.fillRect(w.rect.inset(1.0f), kTrack, kRadiusMark);
        polarityIcon(c, w.rect.center(), mods_.sourceBipolar(w.a), hovered ? kText : kDim);
        break;
    }
    }
}

// The name tags of a hovered or dragged mod dot, painted after every widget so the ones below don't cover them (a
// hovered wave button's is the inspector's).
void View::paintHoverTags(Canvas &c, const std::vector<Widget> &list) {
    for (const Widget &w : list) {
        if (w.kind == Kind::ModDot && (isHovered(w) || isActive(w))) {
            const Point center = w.rect.center();
            const std::string text = modRangeText(w.a, w.b);
            const float width = std::ceil(c.textWidth(text, kTextLabel)) + 16.0f;
            const float x = std::clamp(center.x - width * 0.5f, 4.0f, View::kWidth - 4.0f - width);
            const Rect tip{x, center.y + 8.0f, width, 18.0f};
            paintMenuPanel(c, tip, kRadiusControl);
            c.text(tip, text, kTextLabel, kText);
        }
    }
}

// A modulation in words: its source, and what it takes its target through in the target's own units, from
// half the depth below the set value to half above for a bipolar modulation, or from it one way for a unipolar one.
std::string View::modRangeText(int source, int slot) const {
    std::string text = mods_.sourceName(source);
    const int target = slotTarget(value(slotTargetIndex(source, slot)));
    if (target >= 0) {
        const ParamDef &def = table_[target];
        const double set = value(target);
        const double reach =
            value(slotAmountIndex(source, slot)) * value(slotScaleIndex(source, slot)) * (def.max - def.min);
        const bool both = mods_.sourceBipolar(source);
        const double start = both ? set - 0.5 * reach : set; // a negative depth runs the other way
        const double end = both ? set + 0.5 * reach : set + reach;
        char from[32], to[32];
        formatParam(target, std::clamp(start, def.min, def.max), from, sizeof(from));
        formatParam(target, std::clamp(end, def.min, def.max), to, sizeof(to));
        text += std::string("  ") + from + " \xE2\x86\x92 " + to;
    }
    if (mods_.slotBypassed(source, slot))
        text += std::string("  ") + tr(Str::CaptionBypassed);
    return text;
}

// In a theme that colors knobs by what they belong to: a source's or an effect's in its card's color, a filter's or a
// shaper's in its node's, as the switches on its page are; an operator's in the accent, as its page's switches are, its
// own color kept for the graph.
Color View::knobColor(int param) const {
    if (!look.flat)
        return kAccent;
    if (const int s = sourceOf(param); s >= 0)
        return kSourceColors[s];
    if (const int slot = fxSlotOf(param); slot >= 0)
        return fxTypeColor(fx_.slotType(slot));
    // A filter's, and the depths of the routes into or out of one (an operator's send, another filter's link, its
    // modulation of an operator), which its INPUTS tab shows, as the routes take its color.
    if (param >= kFilterModBase && param < kShaperBase)
        return kFilterColor;
    if (shaperOf(param) >= 0)
        return kShaperColor;
    if (param >= kOpBase && param < kMatrixBase) {
        const int field = (param - kOpBase) % kOpParamCount;
        if (field >= static_cast<int>(OpParam::Sends) && field < static_cast<int>(OpParam::ShaperSends))
            return kFilterColor;
        if (field >= static_cast<int>(OpParam::ShaperSends) && field < static_cast<int>(OpParam::Fixed))
            return kShaperColor;
    }
    return kAccent;
}

void View::paintKnob(Canvas &c, const Widget &w) {
    const ParamDef &def = table_[w.param];
    const bool focused = isHovered(w) || isActive(w);
    const Point center = knobCenter(w);
    const float radius = knobRadius(w);
    const double n = norm(w.param);
    const float angle = knobAngle(n);

    // A macro's knob in its handle's color. While it modulates nothing it is faint like the handle, and an LFO's Phase
    // while notes don't start the LFO there: dimmed as a whole, as every dimmed widget is (knobIdle()).
    const int macro = macroOf(w.param);
    // A join's band levels in their bands' colors (the high band's wire takes its first input).
    const int sum = fxSumOfLevel(w.param);
    const int levelOf = sum < 0 ? -1
                        : w.param == fxInputIndex(fxSumNode(sum), 0, FxInputField::Level) ? 1
                        : w.param == fxInputIndex(fxSumNode(sum), 1, FxInputField::Level) ? 2
                                                                                          : 3;
    const Color plain = sum >= 0 ? laneColor(levelOf) : knobColor(w.param);
    const Color own = macro >= 0 ? kSourceColors[kMaxSources + macro] : plain;
    // A bipolar macro moves nothing at the knob's middle, so its arc starts there.
    const bool fromMiddle = isBipolar(def.kind) || (macro >= 0 && mods_.sourceBipolar(kMaxSources + macro));
    // The track under it: in a theme that colors knobs by what they belong to, a faint tint of that color.
    // Modulated, it keeps them: a modulation ring in a color like its own is told apart from it instead (ringColor()).
    const Color arc = own;
    const Color track = look.flat ? own.alpha(focused ? 0.45f : 0.28f) : (focused ? kFaint : kTrack);
    c.arc(center, radius - 2.0f, kKnobStart, kKnobStart + kKnobSweep, track, 3.5f);
    c.arc(center, radius - 2.0f, fromMiddle ? 0.0f : kKnobStart, angle, arc, 3.5f);

    // Modulation ranges just outside the track, one ring per source. Bipolar sources swing both ways.
    // A switched-off source's ring is faint: it is kept, but does nothing. The one whose depth handle is under the
    // mouse is wider, over the others.
    // (A hovered pill focuses all of those it holds.)
    // Where a ring's reach runs past the knob's end, so that it is cut off there, a line across it marks the end.
    auto ring = [&](const Assignment &m, float r, float width, float alpha) {
        const auto [down, up] = modReach(m);
        const Color color = ringColor(kSourceColors[m.source], own).alpha(alpha);
        c.arc(center, r, knobAngle(n + down), knobAngle(n + up), color, width);
        const float half = 0.5f * width + 1.75f;
        for (const double end : {n + down < 0.0 ? 0.0 : -1.0, n + up > 1.0 ? 1.0 : -1.0})
            if (end >= 0.0)
                c.line(Canvas::onCircle(center, r - half, knobAngle(end)), Canvas::onCircle(center, r + half, knobAngle(end)),
                       color, 1.5f);
    };
    std::vector<Assignment> focus;
    forEachMod(w.param, [&](const Assignment &m) {
        if (modFocused(m)) {
            focus.push_back(m);
            return;
        }
        ring(m, radius + 2.5f, 2.5f,
             (m.bipolar ? 0.6f : 0.9f) * (mods_.sourceOn(m.source) && !m.bypassed ? 1.0f : 0.3f));
    });
    for (const Assignment &f : focus)
        ring(f, radius + 3.0f, 4.0f, mods_.sourceOn(f.source) && !f.bypassed ? 1.0f : 0.4f);

    // The cap, over a shadow where the theme has one; its pointer a line from the middle, or in a flat theme a dot by
    // its rim.
    const float cap = radius - 7.0f;
    if (look.capShadow > 0.0f) {
        c.fillCircle({center.x, center.y + 1.5f}, cap + 1.0f, Color{0.0f, 0.0f, 0.0f, look.capShadow * 0.5f});
        c.fillCircle({center.x, center.y + 1.0f}, cap + 0.5f, Color{0.0f, 0.0f, 0.0f, look.capShadow});
    }
    c.fillCircle(center, cap, kKnobBody);
    c.strokeCircle(center, cap, look.panelShadow > 0.0f ? kBorder.alpha(0.6f) : kBorder, 1.0f);
    if (look.flat)
        c.fillCircle(Canvas::onCircle(center, cap - 4.0f, angle), 2.0f, kText);
    else
        c.line(Canvas::onCircle(center, cap * 0.3f, angle), Canvas::onCircle(center, radius - 9.5f, angle), kText,
               2.0f);

    // Where modulation currently takes the newest voice, or with no note playing where the macros take it.
    // One modulated in steps stands on the step it plays.
    float offset = liveOffset(w.param);
    if (offset != 0.0f && modulatedInSteps(w.param))
        offset = static_cast<float>(std::clamp(std::round(value(w.param) + offset), def.min, def.max) - value(w.param));
    if (offset != 0.0f)
        c.fillCircle(Canvas::onCircle(center, radius - 2.0f, knobAngle(n + offset / (def.max - def.min))), 2.5f, kText);

    char text[64];
    formatParam(w.param, value(w.param), text, sizeof(text));
    std::string shown = text;
    if (def.kind == ParamKind::Ratio)
        shown = "\xC3\x97" + shown;
    if (w.compact) {
        const float textX = compactTextX(w);
        const float textWidth = w.rect.w - textX;
        const float labelY = w.shrunk ? 2.0f : 3.0f, valueY = w.shrunk ? 15.0f : 18.0f;
        c.text({w.rect.x + textX, w.rect.y + labelY, textWidth, 16.0f}, w.label, kTextLabel, kDim, Align::Left);
        c.text({w.rect.x + textX, w.rect.y + valueY, textWidth, 18.0f}, shown, kTextValue, kText, Align::Left, true);
    } else {
        // As wide as a 32 px knob's at the least, so a smaller knob's value still fits under it.
        const float labelW = std::max(w.rect.w + 28.0f, 60.0f);
        const Rect label{center.x - labelW * 0.5f, w.rect.y + w.rect.w + 2.0f, labelW, 16.0f};
        c.text(label, focused || !w.label ? shown : std::string(w.label), kTextLabel,
               focused ? kText : kDim);
    }
}

namespace {
// The Ratio readout's value for `hundredths`, held to its range.
double ratioNorm(long hundredths) { return normFromRatio(std::clamp(hundredths, 50L, 6400L) / 100.0); }

// A digit as a 7-segment display's, leaning right, its unlit segments faint; -1 for none lit.
void sevenSegments(Canvas &c, Rect r, int digit, Color on, Color off) {
    static constexpr const char *kLit[10] = {"abcdef", "bc",     "abdeg", "abcdg",   "bcfg",
                                             "acdfg",  "acdefg", "abc",   "abcdefg", "abcdfg"};
    constexpr float t = 2.6f, gap = 0.6f;
    const float xl = r.x + 1.0f + t * 0.5f, xr = r.right() - 1.0f - t * 0.5f;
    const float yt = r.y + t * 0.5f, yb = r.bottom() - t * 0.5f, ym = (yt + yb) * 0.5f;
    const float middle = r.center().y;
    auto lean = [&](float x, float y) { return Point{x + (middle - y) * 0.09f, y}; };
    auto across = [&](float x0, float x1, float y) {
        return std::array<Point, 6>{lean(x0, y), lean(x0 + t * 0.5f, y - t * 0.5f), lean(x1 - t * 0.5f, y - t * 0.5f),
                                    lean(x1, y), lean(x1 - t * 0.5f, y + t * 0.5f), lean(x0 + t * 0.5f, y + t * 0.5f)};
    };
    auto down = [&](float x, float y0, float y1) {
        return std::array<Point, 6>{lean(x, y0), lean(x + t * 0.5f, y0 + t * 0.5f), lean(x + t * 0.5f, y1 - t * 0.5f),
                                    lean(x, y1), lean(x - t * 0.5f, y1 - t * 0.5f), lean(x - t * 0.5f, y0 + t * 0.5f)};
    };
    // a to g: top, top right, bottom right, bottom, bottom left, top left, middle
    const std::array<Point, 6> segments[7] = {
        across(xl + gap, xr - gap, yt), down(xr, yt + gap, ym - gap), down(xr, ym + gap, yb - gap),
        across(xl + gap, xr - gap, yb), down(xl, ym + gap, yb - gap), down(xl, yt + gap, ym - gap),
        across(xl + gap, xr - gap, ym)};
    const std::string_view lit = digit >= 0 && digit <= 9 ? kLit[digit] : "";
    for (int s = 0; s < 7; ++s) {
        const bool shown = lit.find(static_cast<char>('a' + s)) != std::string_view::npos;
        c.fillPolygon(segments[s].data(), segments[s].size(), shown ? on : off);
    }
}
} // namespace

// The Ratio readout: four digits, the one under the mouse (or being dragged) lit behind, and over them a track from
// 0.5 to 64 in a straight line with the ratio's place and each modulation's reach in its source's color, as a knob's
// rings (an arrow after its end where modulation takes it on past 64).
void View::paintRatio(Canvas &c, const Widget &w) {
    const ParamDef &def = table_[w.param];
    const bool dragged = drag_.kind == DragKind::Value && drag_.widget.same(w);
    const bool focused = isHovered(w) || dragged;
    const double n = norm(w.param);
    c.fillRect(kRatioBox, kDisplayColor, kRadiusControl);
    if (outlined() || focused) // in a flat theme, as its other displays, outlined only under the mouse
        c.strokeRect(kRatioBox, focused ? kFaint : kBorder, 1.0f, kRadiusControl);

    const long h = ratioHundredths(value(w.param));
    const int digits[kRatioDigits] = {h >= 1000 ? static_cast<int>(h / 1000) : -1, static_cast<int>(h / 100 % 10),
                                      static_cast<int>(h / 10 % 10), static_cast<int>(h % 10)};
    const int lit = dragged                                             ? drag_.widget.b
                    : isHovered(w) && drag_.kind == DragKind::None ? ratioDigit(hoverPoint_.x)
                                                                        : -1;
    for (int k = 0; k < kRatioDigits; ++k) {
        const Rect r = ratioDigitRect(k);
        if (k == lit)
            c.fillRect(r.inset(-1.5f), kAccent.alpha(0.14f), kRadiusMark);
        sevenSegments(c, r, digits[k], kAccent, kAccent.alpha(0.08f));
    }
    const Rect ones = ratioDigitRect(1);
    constexpr float kPoint = 2.6f;
    c.fillRect({ones.right() + (kRatioPointW + 2.0f * kRatioDigitGap - kPoint) * 0.5f - 1.0f, ones.bottom() - kPoint,
                kPoint, kPoint},
               kAccent, kRadiusMark);

    // The track runs in a straight line from 0.5 to 64, the ratio's range, the value `v` (on Ratio's log curve) placed
    // by the ratio it plays. Modulation can take it on to 128 (see kRatioModNorm): a reach past 64 stops at the end,
    // with an arrow after it.
    auto along = [](double v) {
        const double ratio = std::min(ratioFromNorm(std::max(v, 0.0)), kMaxRatio);
        return kRatioTrack.x + static_cast<float>((ratio - kMinRatio) / (kMaxRatio - kMinRatio)) * kRatioTrack.w;
    };
    c.fillRect(kRatioTrack, kTrack, kRadiusMark);
    // A switched-off source's reach is faint; the one whose depth handle is under the mouse is wider, over the others.
    auto reach = [&](const Assignment &m, bool wide) {
        const auto [down, up] = modReach(m);
        const float a = along(n + down), b = along(n + up);
        const float alpha = (mods_.sourceOn(m.source) && !m.bypassed ? 1.0f : 0.35f) * (wide ? 1.0f : 0.8f);
        const float thick = wide ? 4.0f : 2.0f;
        const Color color = kSourceColors[m.source].alpha(alpha);
        c.fillRect({std::min(a, b), kRatioTrack.center().y - thick * 0.5f, std::max(std::abs(b - a), 1.0f), thick},
                   color, kRadiusMark);
        if (n + std::max(down, up) > 1.0 + 1e-9) { // on past 64
            const float x = kRatioTrack.right() + 1.0f, y = kRatioTrack.center().y;
            const Point arrow[] = {{x, y - 2.5f}, {x + 3.5f, y}, {x, y + 2.5f}};
            c.fillPolygon(arrow, std::size(arrow), color);
        }
    };
    std::vector<Assignment> focus;
    forEachMod(w.param, [&](const Assignment &m) {
        if (modFocused(m))
            focus.push_back(m);
        else
            reach(m, false);
    });
    for (const Assignment &f : focus)
        reach(f, true);
    c.fillRect({along(n) - 1.0f, kRatioTrack.y - 1.5f, 2.0f, kRatioTrack.h + 3.0f}, kText, kRadiusMark);
    // Where modulation currently takes the newest voice, or with no note playing where the macros take it.
    const float offset = liveOffset(w.param);
    if (offset != 0.0f)
        c.fillCircle({along(n + offset / (def.max - def.min)), kRatioTrack.center().y}, 2.5f, kText);

    c.text({w.rect.x, kRatioLabelY, w.rect.w, 16.0f}, w.label ? w.label : "", kTextLabel,
           focused ? kText : kDim);
}

// A stepper: its number in a field, a unit after it smaller and dimmer, and - and + at the field's ends, faint at rest
// where the number leaves them room and lit under the mouse (the unit making way where they need its room). Along the
// field's foot its steps: a tick each where they fit, else a track filled to the value, with each modulation's reach in
// its source's color over them, as the Ratio readout's track has it.
void View::paintStepper(Canvas &c, const Widget &w) {
    const ParamDef &def = table_[w.param];
    const bool dragged = drag_.kind == DragKind::Value && drag_.widget.same(w);
    const bool hovered = isHovered(w);
    const bool focused = hovered || dragged;
    const Rect well = w.well;
    const Color own = knobColor(w.param);
    c.fillRect(well, kDisplayColor, kRadiusControl);
    if (outlined() || focused) // in a flat theme, as its other displays, outlined only under the mouse
        c.strokeRect(well, focused ? kFaint : kBorder, 1.0f, kRadiusControl);

    const double v = value(w.param);
    char text[64];
    formatParam(w.param, v, text, sizeof(text));
    std::string number = text, unit;
    if (constexpr std::string_view st = " st"; number.size() > st.size() && number.ends_with(st)) {
        unit = number.substr(number.size() - st.size() + 1);
        number.resize(number.size() - st.size());
    }
    float size = w.compact ? kTextValue : kTextBody;
    constexpr float kZone = 14.0f; // each end's button (see stepperButton())
    float numberW = std::ceil(c.textWidth(number, size, true));
    float unitW = unit.empty() ? 0.0f : std::ceil(c.textWidth(unit, kTextSmall)) + 2.0f;
    const bool room = numberW + unitW + 4.0f <= well.w - 2.0f * kZone; // 2 px clear of each glyph's zone
    const int under = hovered ? stepperButton(w, hoverPoint_) : 0;
    // Where the number goes, between `left` and `right`, and which of - and + show (-1, +1, or both at 0).
    float left = well.x + kZone + 2.0f, right = well.right() - kZone - 2.0f;
    bool both = room;
    if (!room && !focused) { // at rest, neither showing: the whole field
        left = well.x + 3.0f;
        right = well.right() - 3.0f;
    } else if (!room) {
        // Under the mouse with too long a number: the unit makes way for - and +, the number then needing only to
        // clear their glyphs (2 px), not all of their zones; then it takes the small size; and if it still runs into
        // them, only the one under the mouse shows, the number moving off it into the other's room (with the mouse on
        // the number, neither shows).
        unit.clear();
        unitW = 0.0f;
        left = well.x + 12.5f;
        right = well.right() - 12.5f;
        if (numberW > right - left) {
            size = kTextSmall;
            numberW = std::ceil(c.textWidth(number, size, true));
        }
        both = numberW <= right - left;
        if (!both) {
            left = under < 0 ? well.x + kZone + 1.0f : well.x + 3.0f;
            right = under > 0 ? well.right() - kZone - 1.0f : well.right() - 3.0f;
        }
    }
    const float textH = well.h - 5.0f; // above the steps
    // Centered between its bounds, or held inside them where it is wider than its room.
    const float x = std::round(
        std::max(left, std::min(right - numberW - unitW, 0.5f * (left + right) - 0.5f * (numberW + unitW))));
    c.text({x, well.y, numberW + 2.0f, textH}, number, size, kText, Align::Left, true);
    if (!unit.empty())
        c.text({x + numberW + 2.0f, well.y + 1.5f, unitW, textH}, unit, kTextSmall, kDim, Align::Left);

    if (both || under != 0) {
        const float y = well.y + textH * 0.5f;
        for (const int side : {-1, 1}) {
            if (!both && side != under)
                continue;
            const bool end = side < 0 ? v <= def.min + 1e-9 : v >= def.max - 1e-9;
            const float bx = side < 0 ? well.x + 7.5f : well.right() - 7.5f;
            if (under == side && !end)
                c.fillRect({bx - 6.0f, y - 6.0f, 12.0f, 12.0f}, own.alpha(0.18f), kRadiusMark);
            const Color ink = end              ? kFaint.alpha(focused ? 0.7f : 0.35f)
                              : under == side ? kText
                              : focused       ? kDim
                                              : kDim.alpha(0.45f);
            c.line({bx - 3.0f, y}, {bx + 3.0f, y}, ink, 1.5f);
            if (side > 0)
                c.line({bx, y - 3.0f}, {bx, y + 3.0f}, ink, 1.5f);
        }
    }

    // The steps: a tick each, 2.5 px apart or more, or a track; a value `n` (normalized) placed on a tick's middle or
    // along the track, which fills from 0 where the range has it in its middle (a pitch), as a bipolar knob's arc does.
    const Rect track{well.x + 6.0f, well.bottom() - 4.5f, well.w - 12.0f, 2.0f};
    const int steps = std::max(2, static_cast<int>(std::lround(def.max - def.min)) + 1);
    const float pitch = track.w / static_cast<float>(steps);
    const bool ticks = pitch >= 2.5f;
    const double n = norm(w.param);
    auto along = [&](double at) {
        const double t = std::clamp(at, 0.0, 1.0);
        return ticks ? track.x + (static_cast<float>(t) * static_cast<float>(steps - 1) + 0.5f) * pitch
                     : track.x + static_cast<float>(t) * track.w;
    };
    // A switched-off source's reach is faint; the one whose depth handle is under the mouse goes over the others.
    std::vector<std::pair<std::pair<double, double>, Color>> reaches, focus;
    forEachMod(w.param, [&](const Assignment &m) {
        const auto [down, up] = modReach(m);
        const bool wide = modFocused(m);
        const float alpha = (mods_.sourceOn(m.source) && !m.bypassed ? 1.0f : 0.35f) * (wide ? 1.0f : 0.8f);
        (wide ? focus : reaches)
            .push_back({{n + std::min(down, up), n + std::max(down, up)}, kSourceColors[m.source].alpha(alpha)});
    });
    reaches.insert(reaches.end(), focus.begin(), focus.end());
    const int on = static_cast<int>(std::lround(v - def.min));
    if (ticks) {
        for (int k = 0; k < steps; ++k) {
            const double at = static_cast<double>(k) / (steps - 1);
            Color color = k == on ? own : kTrack;
            if (k != on)
                for (const auto &[span, reach] : reaches)
                    if (at >= span.first - 1e-9 && at <= span.second + 1e-9)
                        color = reach;
            c.fillRect({track.x + k * pitch + 0.5f, track.y, pitch - 1.0f, track.h}, color, kRadiusMark);
        }
    } else {
        c.fillRect(track, kTrack, kRadiusMark);
        const float from = def.min < 0.0 && def.max > 0.0 ? along(-def.min / (def.max - def.min)) : track.x;
        c.fillRect({std::min(from, along(n)), track.y, std::abs(along(n) - from), track.h}, own.alpha(0.5f),
                   kRadiusMark);
        for (const auto &[span, reach] : reaches) {
            const float a = along(span.first), b = along(span.second);
            c.fillRect({a, track.y, std::max(b - a, 1.0f), track.h}, reach, kRadiusMark);
        }
    }
    // Where modulation currently takes the newest voice, or with no note playing where the macros take it; one modulated
    // in steps on the step it plays.
    float offset = liveOffset(w.param);
    if (offset != 0.0f && modulatedInSteps(w.param))
        offset = static_cast<float>(std::clamp(std::round(v + offset), def.min, def.max) - v);
    if (offset != 0.0f)
        c.fillCircle({along(n + offset / (def.max - def.min)), track.center().y}, 2.5f, kText);

    const Rect r = w.rect;
    if (w.compact)
        c.text({r.x + 2.0f, r.y, r.w - 2.0f, 15.0f}, w.label ? w.label : "", kTextLabel, kDim, Align::Left);
    else
        c.text({r.x - 14.0f, r.y + r.w + 2.0f, r.w + 28.0f, 16.0f}, w.label ? w.label : "", kTextLabel,
               focused ? kText : kDim);
}

// What the play switches do, below the one under the mouse.
void View::paintTooltip(Canvas &c) {
    if (hover_ && drag_.kind == DragKind::None) // the panels' own (the presets', the SEQ view's)
        if (const std::string own = panelTip(*hover_); !own.empty()) {
            const float width = std::min(std::ceil(c.textWidth(own, kTextBody)) + 24.0f, View::kWidth - 16.0f);
            const float x = std::clamp(hover_->rect.center().x - width * 0.5f, 8.0f, View::kWidth - 8.0f - width);
            const bool atMouse = std::any_of(panels_.begin(), panels_.end(),
                                             [&](const Panel *panel) { return panel->tipAtMouse(*hover_); });
            const float below = atMouse ? hoverPoint_.y + 18.0f // a tall widget: under the mouse
                                        : hover_->rect.bottom() + 6.0f;
            const Rect tip{x, below, width, 22.0f};
            paintMenuPanel(c, tip);
            c.text(tip, own, kTextBody, kText);
            return;
        }
    if (!hover_ || drag_.kind != DragKind::None ||
        (hover_->kind != Kind::Choice && hover_->kind != Kind::Toggle &&
         hover_->kind != Kind::SourceRemove &&
         hover_->kind != Kind::PresetButton &&
         hover_->kind != Kind::PresetStep && hover_->kind != Kind::PresetNameBox &&
         hover_->kind != Kind::TempoBox && hover_->kind != Kind::TempoHost && hover_->kind != Kind::TempoSync &&
         hover_->kind != Kind::SettingsButton && hover_->kind != Kind::UndoButton &&
         hover_->kind != Kind::SettingsManual &&
         hover_->kind != Kind::SettingsHue && hover_->kind != Kind::SettingsGlow &&
         hover_->kind != Kind::SettingsLinks && hover_->kind != Kind::SettingsGrid &&
         hover_->kind != Kind::SlotBypass && hover_->kind != Kind::MacroName &&
         hover_->kind != Kind::ModPill &&
         hover_->kind != Kind::SourcePolarity &&
         hover_->kind != Kind::ModsAuxClear && hover_->kind != Kind::ModsAuxDepth &&
         hover_->kind != Kind::MacroHandle && hover_->kind != Kind::SourceHandle &&
         hover_->kind != Kind::NoteDisplay &&
         hover_->kind != Kind::DynBand &&
         hover_->kind != Kind::ListStep &&
         hover_->kind != Kind::DynGrip && hover_->kind != Kind::DynZone && !(hover_->kind == Kind::Knob && dynKnobTip(hover_->param) != Str::Count) &&
         !(hover_->kind == Kind::ListButton && (isSlopeList(hover_->param) || isMidiInput(hover_->param)))))
        return;
    // A source's handle tells how to use it only until it modulates something.
    const bool handle = hover_->kind == Kind::MacroHandle || hover_->kind == Kind::SourceHandle;
    if (handle && sourceUsed(hover_->a))
        return;
    const int param = hover_->param;
    const FxParam field = fxFieldOf(param);
    std::string text;
    if (hover_->kind == Kind::MacroName) {
        text = tr(Str::TipRenameMacro);
    } else if (hover_->kind == Kind::ModPill) {
        if (modListOn(*hover_))
            return; // its list shows them
        int count = 0;
        forEachMod(hover_->a, [&](const Assignment &) { ++count; });
        text = trf(Str::TipModPill, {std::to_string(count - hover_->b)});
    } else if (hover_->kind == Kind::NoteDisplay) {
        text = tr(isVelocity(hover_->a) ? Str::TipVelocityDisplay : Str::TipNoteDisplay);
    } else if (hover_->kind == Kind::DynBand) {
        const Str bands[] = {Str::BandLow, Str::BandMid, Str::BandHigh};
        text = trf(value(paramIndex(hover_->a, FxParam::MdLink)) >= 0.5 ? Str::TipMcBandLinked : Str::TipMcBand,
                   {tr(bands[hover_->b])});
    } else if (hover_->kind == Kind::DynGrip) {
        text = tr(hover_->b % 2 == 1 ? Str::TipUpGrip : Str::TipDownGrip);
    } else if (hover_->kind == Kind::DynZone) {
        text = tr(hover_->b % 2 == 1 ? Str::TipUpZone : Str::TipDownZone);
    } else if (hover_->kind == Kind::Knob) {
        text = tr(dynKnobTip(param));
    } else if (hover_->kind == Kind::MacroHandle && isOpSource(hover_->a)) {
        text = trf(Str::TipSourceHandle, {mods_.sourceName(hover_->a)});
    } else if (hover_->kind == Kind::MacroHandle && isWheel(hover_->a)) {
        text = tr(Str::TipWheelHandle);
    } else if (hover_->kind == Kind::MacroHandle && isNote(hover_->a)) {
        text = tr(Str::TipNoteHandle);
    } else if (hover_->kind == Kind::MacroHandle && isVelocity(hover_->a)) {
        text = tr(Str::TipVelocityHandle);
    } else if (hover_->kind == Kind::MacroHandle && isBend(hover_->a)) {
        text = tr(Str::TipBendHandle);
    } else if (hover_->kind == Kind::MacroHandle) {
        text = trf(Str::TipMacroHandle, {macroName(controller_.layout(), hover_->a - kMaxSources)});
    } else if (hover_->kind == Kind::SourceHandle) {
        text = trf(Str::TipSourceHandle, {mods_.sourceName(hover_->a)});
    } else if (isPartialCount(param)) {
        text = trf(Str::TipPartialCount, {std::to_string(hover_->b)});
    }
    else if (hover_->kind == Kind::PresetStep)
        text = tr(hover_->a < 0 ? Str::TipPreviousPreset : Str::TipNextPreset);
    else if (hover_->kind == Kind::PresetNameBox)
        text = tr(controller_.patchEdited() ? Str::TipPresetChanged : Str::TipPresetName);
    else if (hover_->kind == Kind::TempoBox)
        text = tr(Str::TipTempo);
    else if (hover_->kind == Kind::TempoHost)
        text = tr(Str::TipTempoHost);
    else if (hover_->kind == Kind::TempoSync)
        text = tr(Str::TipTempoSync);
    else if (hover_->kind == Kind::PresetButton)
        switch (static_cast<PresetAction>(hover_->a)) {
        case PresetAction::Init:
            text = tr(Str::TipInit);
            break;
        case PresetAction::Load:
            text = tr(Str::TipLoad);
            break;
        case PresetAction::Save: {
            PresetStore &store = presetStore();
            text = !controller_.presetHasFile()                         ? tr(Str::TipSaveNoFile)
                   : !controller_.patchEdited()                         ? tr(Str::TipSaveUnchanged)
                   : store.writesPresetFiles() && !store.presetWritable() ? tr(Str::TipSaveAs)
                                                                        : trf(Str::TipSave, {store.presetName()});
            break;
        }
        case PresetAction::SaveAs:
            text = tr(presetActionOn(PresetAction::SaveAs) ? Str::TipSaveAs : Str::TipSaveUnchanged);
            break;
        }
    else if (hover_->kind == Kind::SettingsButton)
        text = tr(Str::TipSettings);
    else if (hover_->kind == Kind::SettingsManual)
        text = tr(Str::TipManual);
    else if (hover_->kind == Kind::SettingsHue)
        text = tr(Str::TipSettingsHue);
    else if (hover_->kind == Kind::SettingsGlow)
        text = tr(Str::TipSettingsGlow);
    else if (hover_->kind == Kind::SettingsLinks)
        text = tr(Str::TipSettingsLinks);
    else if (hover_->kind == Kind::SettingsGrid)
        text = tr(Str::TipSettingsGrid);
    else if (hover_->kind == Kind::SlotBypass)
        text = tr(mods_.slotBypassed(hover_->a, hover_->b) ? Str::TipBypassed : Str::TipBypass);
    else if (hover_->kind == Kind::SourcePolarity)
        text = trf(mods_.sourceBipolar(hover_->a) ? Str::TipSourceBipolar : Str::TipSourceUnipolar,
                   {mods_.sourceName(hover_->a)});
    else if (hover_->kind == Kind::ModsAuxClear)
        text = tr(Str::TipModsAuxClear);
    else if (hover_->kind == Kind::ModsAuxDepth) {
        char amount[32];
        formatParam(hover_->param, value(hover_->param), amount, sizeof(amount));
        text = trf(Str::TipModsAuxDepth, {amount});
    }
    else if (hover_->kind == Kind::UndoButton)
        text = undoText(hover_->a == 1);
    else if (hover_->kind == Kind::SourceRemove)
        text = tr(Str::TipSourceRemove);
    else if (isSourcePower(param))
        text = tr(Str::TipSourcePower);
    else if (param == paramIndex(GlobalParam::Mono))
        text = tr(hover_->b == 0 ? Str::TipPoly : Str::TipMono);
    else if (param == paramIndex(GlobalParam::Legato))
        text = tr(Str::TipLegato);
    else if (field == FxParam::Enabled) // the icon on its card
        text = tr(value(param) >= 0.5 ? Str::TipFxOn : Str::TipFxOff);
    else if (isLfoSync(param))
        text = tr(Str::TipLfoSync);
    else if (hover_->kind == Kind::ListStep) {
        const bool next = hover_->b > 0;
        text = tr(isClipMode(param)                 ? (next ? Str::TipNextCurve : Str::TipPrevCurve)
                  : isLfoShape(param)               ? (next ? Str::TipNextShape : Str::TipPrevShape)
                  : field == FxParam::ReverbType    ? (next ? Str::TipNextAlgorithm : Str::TipPrevAlgorithm)
                  : field == FxParam::AmpModel      ? (next ? Str::TipNextModel : Str::TipPrevModel)
                  : field == FxParam::AmpCab        ? (next ? Str::TipNextCabinet : Str::TipPrevCabinet)
                                                    : (next ? Str::TipNextType : Str::TipPrevType));
    }
    else if (hover_->kind == Kind::ListButton && isSplitSlope(param))
        text = tr(Str::TipSlope);
    else if (hover_->kind == Kind::ListButton && isFilterSlope(param))
        text = tr(slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), 0) ? Str::TipSlope
                                                                                                  : Str::TipSlopeBand);
    else if (isLfoClockChoice(*hover_)) {
        const Str tips[] = {Str::TipRetrigger, Str::TipFree, Str::TipAnchor, Str::TipOneShot};
        text = tr(tips[std::clamp(hover_->b, 0, kLfoModes - 1)]);
    }
    else if (sourceFieldOf(param) == SourceParam::Unipolar) // the LFO's switch, or a Curve card's Unipolar | Bipolar
        text = tr(hover_->kind == Kind::Choice && hover_->b == 0 ? Str::TipBipolar : Str::TipUnipolar);
    else if (sourceFieldOf(param) == SourceParam::Bipolar) // a Random card's Unipolar | Bipolar
        text = tr(hover_->b == 1 ? Str::TipBipolar : Str::TipUnipolar);
    else if (field == FxParam::DelaySync)
        text = tr(Str::TipDelaySync);
    else if (field == FxParam::PhaserSync || field == FxParam::FlangerSync)
        text = tr(Str::TipSweepSync);
    else if (field == FxParam::ChorusVoices)
        text = tr(Str::TipChorusVoices);
    else if (field == FxParam::DelayPingPong)
        text = tr(Str::TipPingPong);
    else if (field == FxParam::DelayLink)
        text = tr(Str::TipDelayLink);
    else if (field == FxParam::MdLink)
        text = tr(Str::TipBandLink);
    else if (field == FxParam::MdRms)
        text = tr(Str::TipRms);
    else if (field == FxParam::PhaserStereo || field == FxParam::FlangerStereo)
        text = tr(Str::TipStereo);
    else if (field == FxParam::AmpStereo) // its icon's name, as the param's
        text = tr(Str::ParamAmpStereo);
    else if (field == FxParam::FlangerThroughZero)
        text = tr(Str::TipThroughZero);
    else if (isMorphMode(param))
        text = tr(Str::TipMorphNotch);
    else if (field == FxParam::PhaserNegative || field == FxParam::FlangerNegative)
        text = tr(hover_->b == 1 ? Str::TipNegative : Str::TipPositive);
    else if (field == FxParam::ShaperLink || shaperFieldOf(param) == ShaperParam::Link)
        text = tr(Str::TipShaperLink);
    else if (field == FxParam::ShaperSymmetric || shaperFieldOf(param) == ShaperParam::Symmetric)
        text = tr(Str::TipShaperSymmetric);
    else if (field == FxParam::ShaperMode)
        text = tr(hover_->b == 0 ? Str::TipClipHard : hover_->b == 1 ? Str::TipClipSoft : Str::TipClipAsym);
    else if (isMidiInput(param)) { // what the input picked follows
        const auto input = static_cast<MidiInput>(std::lround(value(param)));
        text = tr(input == MidiInput::Cc         ? Str::TipMidiCc
                  : input == MidiInput::Pressure ? Str::TipMidiPressure
                                                 : Str::TipMidiSlide);
    }
    else if (param == paramIndex(GlobalParam::GlideAlways))
        text = tr(Str::TipGlideAlways);
    else
        return; // a switch with nothing to say
    // As wide as its text, centered under the widget and kept inside the editor.
    const float width = std::min(std::ceil(c.textWidth(text, kTextBody)) + 24.0f, View::kWidth - 16.0f);
    const float x = std::clamp(hover_->rect.center().x - width * 0.5f, 8.0f, View::kWidth - 8.0f - width);
    const Rect tip{x, hover_->rect.bottom() + 6.0f, width, 22.0f}; // a knob's rect takes in its label
    paintMenuPanel(c, tip);
    c.text(tip, text, kTextBody, kText);
}

// The pill: in the track's color, the count of what it holds in bold; lighter under the mouse, in the accent while
// its list is open, and hollow while everything it holds is bypassed or its source is off.
void View::paintModPill(Canvas &c, const Widget &w, bool hovered) {
    const bool open = modListOn(w);
    bool live = false;
    int index = 0;
    forEachMod(w.a, [&](const Assignment &m) {
        if (index++ >= w.b && mods_.sourceOn(m.source) && !m.bypassed)
            live = true;
    });
    const float round = w.rect.h * 0.5f;
    if (open)
        c.fillRect(w.rect, kAccent, round);
    else if (live)
        c.fillRect(w.rect, hovered ? kFaint : kTrack, round);
    else
        c.strokeRect(w.rect.inset(0.5f), hovered ? kDim : kFaint, 1.0f, round);
    const std::string text = "+" + std::to_string(index - w.b);
    const ModHandleSize &sizes = modHandleSize(controller_.editorSettings().handleSize);
    c.text({w.rect.x - 2.0f, w.rect.y - 1.0f, w.rect.w + 4.0f, w.rect.h + 1.0f}, text,
           w.compact ? sizes.pillCompactText : sizes.pillText,
           open ? kBackground : (live ? kText : kDim), Align::Center, true);
}

// An open list closes once it no longer shows: its card scrolled out of view or folded, or its source or effect slot
// holding another type. Left open, it would come back with the card, or with the next of its type in that place.
void View::closeGoneLists() {
    if (listMenu_ < 0 && peqMenu_ < 0)
        return;
    bool list = false, peq = false;
    for (const Widget &w : widgets()) {
        list = list || (w.kind == Kind::ListItem && w.param == listMenu_);
        peq = peq || (w.kind == Kind::PeqTypeItem && w.param == peqMenu_);
    }
    if (!list)
        listMenu_ = -1;
    if (!peq)
        peqMenu_ = -1;
}

void View::openModList(const Widget &pill, bool sprung) {
    modList_ = ModListOpen{pill.a, pill.rect, sprung};
    modListScroll_ = 0.0f;
    listMenu_ = -1; // one open list at a time
    for (Panel *panel : panels_)
        panel->closePopups();
}

bool View::popupOpen() const {
    if (modList_ || sourceMenuOpen_ || listMenu_ >= 0 || peqMenu_ >= 0)
        return true;
    std::vector<Widget> list; // the panels' (the list of effects, the wave menu, a MODS row's menu, the SEQ view's lists)
    for (const Panel *panel : panels_)
        panel->popups(list);
    return !list.empty();
}

// Not while a value is typed, which takes Escape itself, nor during a drag (a handle held over a pill opens its list),
// which Escape leaves alone: the window then lets the key go on to the host.
bool View::popupTakesEscape() const {
    return !typing_ && drag_.kind == DragKind::None && popupOpen();
}

void View::closeMenus() {
    modList_.reset();
    for (Panel *panel : panels_)
        panel->closePopups();
    sourceMenuOpen_ = false;
    listMenu_ = -1;
    peqMenu_ = -1;
    inspector_.closeWaveMenu();
    hover_.reset();
}

bool View::modListPillShown() const {
    const std::vector<Widget> list = widgets();
    return std::any_of(list.begin(), list.end(), [&](const Widget &w) { return w.kind == Kind::ModPill && modListOn(w); });
}

bool View::modListOn(const Widget &pill) const {
    return modList_ && modList_->target == pill.a && modList_->anchor.x == pill.rect.x &&
           modList_->anchor.y == pill.rect.y;
}

int View::modListRowCount() const {
    int rows = 0;
    if (modList_)
        forEachMod(modList_->target, [&](const Assignment &) { ++rows; });
    return rows;
}

float View::modListEnd() const {
    return static_cast<float>(std::max(modListRowCount() - kModListRows, 0));
}

float View::modListFirst() const {
    return std::clamp(modListScroll_, 0.0f, modListEnd());
}

Rect View::modListScrollTrack(Rect panel) const {
    const float top = panel.y + kModListTitleH;
    return {panel.right() - kModListPad - 4.0f, top, 4.0f, panel.bottom() - kModListPad + 4.0f - top - 4.0f};
}

Rect View::modListThumb(Rect track) const {
    const float rows = static_cast<float>(std::max(modListRowCount(), 1));
    const float h = std::max(track.h * std::min(static_cast<float>(kModListRows) / rows, 1.0f), 16.0f);
    return {track.x, track.y + (track.h - h) * modListFirst() / std::max(modListEnd(), 0.001f), track.w, h};
}

Rect View::modListPanel(Rect anchor, int rows, bool aux) const {
    const bool scrolls = rows > kModListRows;
    const float w = kCardW - 24.0f + (aux ? kModListAuxW : 0.0f) + 2.0f * kModListPad + (scrolls ? kModListBarW : 0.0f);
    const float h =
        kModListTitleH + static_cast<float>(std::min(rows, kModListRows)) * kModListRowStep + kModListPad - 4.0f;
    const float x = std::clamp(anchor.center().x - w * 0.5f, 4.0f, View::kWidth - 4.0f - w);
    float y = anchor.bottom() + 6.0f; // under the pill; over it when there is no room below
    if (y + h > height() - 4.0f)
        y = std::max(anchor.y - 6.0f - h, 4.0f);
    return {x, y, w, h};
}

void View::paintModList(Canvas &c, const Widget &w) {
    if (look.flat) {
        paintMenuPanel(c, w.rect); // as the other lists
    } else {
        c.fillRect(w.rect.inset(-3.0f), kBackground.alpha(0.35f), kRadiusCard + 3.0f); // a soft edge against what it covers
        c.fillRect(w.rect, kPanel, kRadiusCard);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
    }
    int rows = 0;
    forEachMod(w.a, [&](const Assignment &) { ++rows; });
    const Rect title{w.rect.x + kModListPad + 4.0f, w.rect.y + 2.0f, w.rect.w - 2.0f * kModListPad - 8.0f,
                     kModListTitleH - 4.0f};
    const std::string count = rows == 1 ? std::string(tr(Str::ModListCountOne))
                                        : trf(Str::ModListCount, {std::to_string(rows)});
    const float countW = std::ceil(c.textWidth(count, kTextLabel));
    c.text({title.x, title.y, title.w - countW - 8.0f, title.h}, mods_.paramName(w.a), kTextBody, kText, Align::Left,
           true);
    c.text({title.right() - countW, title.y, countW, title.h}, count, kTextLabel, kDim, Align::Right);
}

// A row: its bypass dot, the source's name (dimmed while bypassed or off), its aux's chip as in the MODS view, its
// depth bar and its remove.
// The ring clockwise from the top for as much of it as the aux depth, the rest of it faint; all of it at 100 %.
void View::paintAuxRing(Canvas &c, Point center, float radius, int aux, double depth) const {
    const Color color = kSourceColors[aux];
    if (depth >= 0.9995) {
        c.strokeCircle(center, radius, color, 1.3f);
        return;
    }
    c.strokeCircle(center, radius, color.alpha(0.3f), 1.3f);
    if (depth > 0.0005)
        c.arc(center, radius, 0.0f, static_cast<float>(depth) * 2.0f * kPi, color, 1.3f);
}

void View::paintModListRow(Canvas &c, const Widget &w) {
    const bool lit = hover_ && hover_->popup && hover_->a == w.a && hover_->b == w.b && drag_.kind == DragKind::None;
    c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
    const bool off = mods_.slotBypassed(w.a, w.b) || !mods_.sourceOn(w.a);
    const std::string name = mods_.sourceName(w.a);
    c.text({w.rect.x + 18.0f, w.rect.y, kSlotNameW, w.rect.h}, name, slotNameSize(c, name), off ? kFaint : kText,
           Align::Left);
    if (const int aux = slotAux([this](int index) { return value(index); }, w.a, w.b); aux >= 0) {
        const Rect chip = modListAuxChip(w.rect);
        const float radius = chip.h * 0.5f;
        if (look.flat) { // as the MODS view's chip
            c.fillRect(chip, kSourceColors[aux].alpha(0.22f), radius);
        } else {
            c.fillRect(chip, kPanel, radius);
            c.strokeRect(chip, kSourceColors[aux].alpha(0.5f), 1.0f, radius);
        }
        paintAuxDepthBar(c, chip, aux, value(slotAuxDepthIndex(w.a, w.b)));
        c.fillCircle({chip.x + 10.0f, chip.center().y}, 3.5f, kSourceColors[aux]);
        c.text({chip.x + 18.0f, chip.y, chip.w - 40.0f - kAuxDepthW, chip.h}, mods_.sourceName(aux), kTextLabel, kText,
               Align::Left, true);
    }
}

void View::paintModOverlay(Canvas &c, const std::vector<Widget> &list) {
    const int source = drag_.widget.a;
    const Color color = kSourceColors[source];
    const auto target = hitTest(drag_.now);
    // What an open pill's list covers takes no drop: it isn't ringed over the list.
    const auto panel = std::find_if(list.begin(), list.end(), [](const Widget &w) { return w.kind == Kind::ModList; });
    auto covered = [&](Rect r) {
        if (panel == list.end())
            return false;
        const Rect p = panel->rect;
        return r.x < p.right() && r.right() > p.x && r.y < p.bottom() && r.bottom() > p.y;
    };
    for (const Widget &w : list) {
        if (w.disabled) // the mouse passes it by (see hitTest()), so it takes nothing
            continue;
        if (w.kind == Kind::ModListRow && w.a != source) { // so does a row of a pill's list
            const bool over = target && target->popup && target->a == w.a && target->b == w.b;
            c.strokeRect(w.rect.inset(-1.0f), color.alpha(over ? 1.0f : 0.35f), over ? 2.0f : 1.0f, kRadiusControl);
            continue;
        }
        if (w.popup || covered(isRatioDisplay(w)      ? kRatioBox.inset(-4.0f)
                               : isStepper(w)         ? w.well.inset(-4.0f)
                               : w.kind == Kind::Knob ? Rect{w.rect.x - 7.0f, w.rect.y - 7.0f, w.rect.w + 14.0f,
                                                             w.rect.w + 14.0f}
                                                      : w.rect.inset(-4.0f)))
            continue;
        if (w.kind == Kind::ModDot && w.a != source) { // a depth handle takes it as the modulation's aux
            if (w.clip.w > 0.0f && !w.clip.contains(w.rect.center()))
                continue;
            const bool over = target && target->same(w);
            c.strokeCircle(w.rect.center(), w.rect.w * 0.5f + 3.0f, color.alpha(over ? 1.0f : 0.35f),
                           over ? 2.0f : 1.0f);
            continue;
        }
        // A panel's own that takes it (a MODS row's aux cell, as the aux too).
        if (const auto owner = std::find_if(panels_.begin(), panels_.end(),
                                            [&](const Panel *candidate) { return candidate->takesSource(w, source); });
            owner != panels_.end()) {
            (*owner)->paintSourceRing(c, w, target && target->same(w), color);
            continue;
        }
        if (!droppable(w, source))
            continue;
        // A target scrolled out of its card or list isn't outlined; one partly out is outlined as far as it
        // shows, with room left round the clip for the ring drawn outside it.
        const bool clipped = w.clip.w > 0.0f;
        if (clipped && (w.rect.right() <= w.clip.x || w.rect.x >= w.clip.right() || w.rect.bottom() <= w.clip.y ||
                        w.rect.y >= w.clip.bottom()))
            continue;
        if (clipped)
            c.pushClip(w.clip.inset(-4.0f));
        const bool over = target && target->same(w);
        const Color ring = color.alpha(over ? 1.0f : 0.35f);
        if (isRatioDisplay(w)) {
            c.strokeRect(kRatioBox.inset(-4.0f), ring, over ? 2.5f : 1.5f, kRadiusCard);
        } else if (isStepper(w)) {
            c.strokeRect(w.well.inset(-4.0f), ring, over ? 2.5f : 1.5f, kRadiusControl + 4.0f);
        } else if (w.kind == Kind::Knob) {
            c.strokeCircle(knobCenter(w), knobRadius(w) + 6.0f, ring, over ? 2.5f : 1.5f);
        } else {
            c.strokeRect(w.rect.inset(w.kind == Kind::GridCell ? -1.0f : -3.0f), ring, over ? 2.0f : 1.0f, kRadiusControl);
        }
        if (clipped)
            c.popClip();
    }
    if (links_ > 0.0f) // 0.6 at the 0.75 LINES begins with
        c.line(drag_.start, drag_.now, color.alpha(0.8f * links_), 1.5f, true);
    c.fillCircle(drag_.now, 7.0f, color.alpha(0.5f));
    c.fillCircle(drag_.now, 3.0f, color);
}

void View::paintSourceLinks(Canvas &c, const std::vector<Widget> &list, int source, int slot) {
    std::vector<int> targets, bypassed;
    for (const Assignment &m : assignments())
        if (m.source == source && (slot < 0 || m.slot == slot))
            (m.bypassed ? bypassed : targets).push_back(m.target);
    const auto handle = std::find_if(list.begin(), list.end(),
                                     [&](const Widget &w) {
                                         return (w.kind == Kind::SourceHandle || w.kind == Kind::MacroHandle) &&
                                                w.a == source;
                                     });
    if ((targets.empty() && bypassed.empty()) || handle == list.end())
        return;
    const Point from = handle->rect.center();
    if (handle->clip.w > 0.0f && !handle->clip.contains(from)) // its card scrolled out of view
        return;
    for (const Widget &w : list) {
        const bool target = w.kind == Kind::Knob || w.kind == Kind::WireLabel || w.kind == Kind::OutLabel ||
                            w.kind == Kind::GridCell ||
                            w.kind == Kind::SlotBar; // the last: another source's depth
        const bool modulated = target && std::find(targets.begin(), targets.end(), w.param) != targets.end();
        const bool off = target && std::find(bypassed.begin(), bypassed.end(), w.param) != bypassed.end();
        if (!modulated && !off)
            continue;
        // The line ends at the source's own dot on the target: the one under a knob, or in the corner of a label,
        // cell or depth bar. A param can show twice (an output's knob and its label on the bus), and
        // each has its dots, so the dot is the one on this widget (a label's under it).
        Rect area = w.rect.inset(-2.0f);
        if (w.kind == Kind::WireLabel || w.kind == Kind::OutLabel)
            area.h += 8.0f;
        const auto dot = std::find_if(list.begin(), list.end(), [&](const Widget &d) {
            return d.kind == Kind::ModDot && d.a == source && (slot < 0 || d.b == slot) &&
                   area.contains(d.rect.center()) && slotTarget(value(slotTargetIndex(source, d.b))) == w.param;
        });
        // A dot the widget had no room for is in its pill: the line ends there.
        const auto pill = dot != list.end() ? list.end() : std::find_if(list.begin(), list.end(), [&](const Widget &d) {
            return d.kind == Kind::ModPill && d.a == w.param && area.contains(d.rect.center());
        });
        const Point to = dot != list.end()    ? dot->rect.center()
                         : pill != list.end() ? pill->rect.center()
                         : (w.kind == Kind::Knob ? knobCenter(w) : w.rect.center());
        if (w.clip.w > 0.0f && !w.clip.contains(to))
            continue;
        const Color color =
            kSourceColors[source].alpha(mods_.sourceOn(source) && modulated ? 1.0f : 0.4f); // bypassed: faint
        // Bowed to one side, so lines to targets in a row fan out rather than run over each other.
        const Point d{to.x - from.x, to.y - from.y};
        const Point control{(from.x + to.x) * 0.5f + d.y * 0.15f, (from.y + to.y) * 0.5f - d.x * 0.15f};
        if (links_ > 0.0f) // as strongly as the settings' LINES say
            c.curve(from, control, to, color.alpha(links_), 1.5f, true);
        if (dot != list.end()) // a ring round the dot the line points at
            c.strokeCircle(to, dot->compact ? 5.5f : 7.5f, color, 1.5f);
        else if (pill != list.end()) // or round the pill
            c.strokeRect(pill->rect.inset(-2.0f), color, 1.5f, pill->rect.h * 0.5f + 2.0f);
        if (isRatioDisplay(w))
            c.strokeRect(kRatioBox.inset(-3.0f), color.alpha(0.5f), 1.0f, kRadiusCard);
        else if (isStepper(w))
            c.strokeRect(w.well.inset(-3.0f), color.alpha(0.5f), 1.0f, kRadiusControl + 3.0f);
        else if (w.kind == Kind::Knob)
            c.strokeCircle(knobCenter(w), knobRadius(w) + 5.0f, color.alpha(0.5f), 1.0f);
        else
            c.strokeRect(w.rect.inset(w.kind == Kind::GridCell ? -1.0f : -3.0f), color.alpha(0.5f), 1.0f, kRadiusControl);
    }
}

void View::paintAuxLinks(Canvas &c, const std::vector<Widget> &list, int aux, int ofSource, int ofSlot) {
    const auto handle = std::find_if(list.begin(), list.end(), [&](const Widget &w) {
        return (w.kind == Kind::SourceHandle || w.kind == Kind::MacroHandle) && w.a == aux;
    });
    if (handle == list.end())
        return;
    const Point from = handle->rect.center();
    if (handle->clip.w > 0.0f && !handle->clip.contains(from)) // its card scrolled out of view
        return;
    const Color color = kSourceColors[aux].alpha(mods_.sourceOn(aux) ? 1.0f : 0.4f);
    for (const Widget &d : list) {
        if (d.kind != Kind::ModDot || (ofSource >= 0 && (d.a != ofSource || d.b != ofSlot)))
            continue;
        if (slotAux([this](int index) { return value(index); }, d.a, d.b) != aux)
            continue;
        const Point to = d.rect.center();
        if (d.clip.w > 0.0f && !d.clip.contains(to))
            continue;
        // Bowed to the other side from the modulation lines, so the two don't run over each other.
        const Point delta{to.x - from.x, to.y - from.y};
        const Point control{(from.x + to.x) * 0.5f - delta.y * 0.15f, (from.y + to.y) * 0.5f + delta.x * 0.15f};
        c.curveDotted(from, control, to, color.alpha(0.85f), 1.5f);
        c.strokeCircleDotted(to, d.compact ? 7.5f : 9.5f, color, 1.5f);
    }
}

Rect View::typingField() const {
    const Widget &w = typing_->widget;
    if (isRatioDisplay(w)) // over its digits
        return {kRatioBox.x, kRatioBox.y + 10.0f, kRatioBox.w, 22.0f};
    if (isStepper(w)) { // over its field, at least as wide as a knob's
        const float width = std::max(w.well.w, 60.0f);
        return {w.well.center().x - width * 0.5f, w.well.center().y - 10.0f, width, 20.0f};
    }
    if (w.kind == Kind::Knob && w.compact)
        return {w.rect.x + compactTextX(w) - 4.0f, w.rect.y + (w.shrunk ? 14.0f : 17.0f), w.rect.w + 2.0f - (compactTextX(w) - 4.0f), 20.0f};
    if (w.kind == Kind::Knob)
        return {w.rect.center().x - 40.0f, w.rect.y + w.rect.w + 1.0f, 80.0f, 18.0f};
    if (w.kind == Kind::ModDot) // under the small dot
        return {w.rect.center().x - 36.0f, w.rect.bottom() + 3.0f, 72.0f, 18.0f};
    if (w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::GridCell) // over the label or cell
        return {w.rect.center().x - 36.0f, w.rect.center().y - 9.0f, 72.0f, 18.0f};
    if (w.kind == Kind::TempoBox) // inside its border
        return {w.rect.x + 3.0f, w.rect.y + 2.0f, w.rect.w - 6.0f, w.rect.h - 4.0f};
    return {w.rect.x - 4.0f, w.rect.y, w.rect.w + 8.0f, w.rect.h};
}

// The text field, all of it selected until the first key, with a blinking caret.
void View::paintTyping(Canvas &c) {
    const Typing &t = *typing_;
    const Rect field = typingField();
    c.fillRect(field, kBackground, kRadiusControl);
    c.strokeRect(field, kAccent, 1.0f, kRadiusControl);
    constexpr float kSize = kTextBody;
    const float width = c.textWidth(t.text, kSize);
    const float x = std::max(field.center().x - width * 0.5f, field.x + 4.0f);
    if (t.selected && !t.text.empty())
        c.fillRect({x - 1.0f, field.y + 3.0f, width + 2.0f, field.h - 6.0f}, kAccent.alpha(0.35f), kRadiusMark);
    c.text({x, field.y, field.right() - 2.0f - x, field.h}, t.text, kSize, kText, Align::Left);
    if (caretTicks_ / 32 % 2 == 0) {
        const float caret = x + c.textWidth(std::string_view(t.text).substr(0, t.caret), kSize);
        c.line({caret, field.y + 3.0f}, {caret, field.bottom() - 3.0f}, kText, 1.0f);
    }
}

// "Undo Op A Fine", or how many params the step changes.
std::string View::undoText(bool redo) const {
    const UndoStep *step = redo ? controller_.nextRedo() : controller_.nextUndo();
    if (!step)
        return tr(redo ? Str::RedoNothing : Str::UndoNothing);
    if (step->changes.size() == 1)
        return trf(redo ? Str::RedoParam : Str::UndoParam, {mods_.paramName(step->changes.front().index)});
    if (step->changes.empty())
        return tr(redo ? Str::RedoLayout : Str::UndoLayout);
    return trf(redo ? Str::RedoChanges : Str::UndoChanges, {std::to_string(step->changes.size())});
}

// --- Input.

namespace {

std::vector<int> envPointParams(int env, int point) {
    const EnvIndices e = envIndices(env);
    if (point == 0)
        return {e.attack};
    if (point == 1)
        return {e.hold};
    if (point == 2)
        return {e.decay, e.sustain};
    return {e.release};
}

// The cutoff and resonance of the filter effect in `slot`, which the handle on its display drags (a voice filter's is
// the inspector's).
std::vector<int> filterPointParams(int slot) {
    return {paramIndex(slot, FxParam::FilterCutoff), paramIndex(slot, FxParam::FilterResonance)};
}

// A cut's handle sets its Q instead of its gain; a drag may touch either.
std::vector<int> peqPointParams(int slot, int band) {
    return {peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band)};
}

} // namespace

void View::applyDrag(Point p) {
    const Widget &w = drag_.widget;
    // A drag of an envelope's handle or time knob holds its zoom from its first move, before that moves anything.
    // Each move keeps the zoom as drawn, as far out as the move before took it.
    if (const int env = envDragged(); env >= 0 && (envZoom_.env != env || envZoom_.easing))
        envZoom_ = {env, envScale(env), false};
    else if (env >= 0)
        envZoom_.scale = envScale(env);
    auto set = [&](int index, double v) {
        v = std::clamp(v, table_[index].min, table_[index].max);
        if (v != value(index))
            controller_.setValue(index, v);
    };
    switch (drag_.kind) {
    case DragKind::Key: {
        // Sliding across keys moves the note, struck as hard as where the mouse enters the key says; off the keys,
        // the last one keeps sounding.
        const auto hit = hitTest(p);
        if (hit && hit->kind == Kind::Key && hit->a != w.a) {
            controller_.playNote(keyboardBase() + w.a, false, 0);
            controller_.playNote(keyboardBase() + hit->a, true, keyVelocity(hit->rect, p));
            drag_.widget.a = hit->a;
        }
        break;
    }
    // The sliders follow the mouse; with Shift, they move a hundredth as fast from where they were.
    case DragKind::ModWheel:
        set(w.param, drag_.fine ? drag_.startValue + (drag_.start.y - p.y) / w.rect.h * 0.01
                                : (w.rect.bottom() - p.y) / w.rect.h);
        break;
    case DragKind::TextSelect: {
        TextField &t = field(static_cast<Field>(w.a));
        t.caret = t.stopAt(p.x);
        caretTicks_ = 0;
        break;
    }
    case DragKind::Glow:
        sliderDragged() = std::clamp(drag_.fine ? static_cast<float>(drag_.startValue) + (p.x - drag_.start.x) / w.rect.w * 0.01f
                                                : (p.x - w.rect.x) / w.rect.w,
                                     0.0f, 1.0f);
        break;
    case DragKind::BendWheel: {
        const double mid = w.rect.y + w.rect.h * 0.5;
        set(paramIndex(GlobalParam::PitchBend), (mid - p.y) / (w.rect.h * 0.5));
        break;
    }
    case DragKind::Value: {
        const ParamDef &def = table_[w.param];
        for (Panel *panel : panels_) // what goes with it, in the same step (a matrix cell switches on what it joins)
            panel->draggingValue(w);
        if (isRatioDisplay(w)) { // the digit pressed, a step of its place every 6 px (24 with Shift), carrying
            const long steps = std::lround((drag_.start.y - p.y) / (drag_.fine ? 24.0f : 6.0f));
            set(w.param, steps != 0 ? ratioNorm(ratioHundredths(drag_.startValue) + steps * kRatioPlaces[w.b])
                                    : drag_.startValue);
        } else if (w.kind == Kind::SlotBar) {
            const Rect track = slotBarTrack(w.rect);
            set(w.param, drag_.fine ? drag_.startValue + (p.x - drag_.start.x) / (track.w * 0.5f) * 0.01
                                    : (p.x - track.center().x) / (track.w * 0.5f));
        } else if (w.kind == Kind::ModsAuxDepth) // from where it is, up: all of it every 100 px (10000 with Shift)
            set(w.param, drag_.startValue + (drag_.start.y - p.y) / (drag_.fine ? 10000.0 : 100.0));
        else if (w.kind == Kind::ModDot && w.a >= 0 && w.b >= 0 && w.param == slotAuxDepthIndex(w.a, w.b))
            set(w.param, drag_.startValue + (drag_.start.y - p.y) / (drag_.fine ? 10000.0 : 100.0)); // Alt-dragged, up
        else if (w.kind == Kind::ModsScale) // 0 at its left end, all of it at its right
            set(w.param, drag_.fine ? drag_.startValue + (p.x - drag_.start.x) / w.rect.w * 0.01
                                    : (p.x - w.rect.x) / w.rect.w);
        else if (w.kind == Kind::ModDot && w.a >= 0 && w.b >= 0 &&
                 isRatioTarget(slotTarget(value(slotTargetIndex(w.a, w.b))))) // an octave every 60 px (3000 with Shift)
            set(w.param, drag_.startValue + (drag_.start.y - p.y) / (drag_.fine ? 3000.0 : 60.0) * (1.0 / 7.0));
        else if (def.kind == ParamKind::VoiceCount) // doubles every 40 px
            set(w.param, std::round(drag_.startValue * std::exp2((drag_.start.y - p.y) / 40.0)));
        else if (snapsToSteps(def.kind)) { // a step every 10 px; a note lands on a whole one
            const double stepped = drag_.startValue + std::round((drag_.start.y - p.y) / 10.0f);
            set(w.param, def.kind == ParamKind::NotePitch ? std::round(stepped) : stepped);
        }
        else {
            // Up turns it up; a three-band dynamics' downward zone, which presses its band down, turns its ratio up
            // as it is dragged down.
            const double up = w.kind == Kind::DynZone && w.b % 2 == 0 ? p.y - drag_.start.y : drag_.start.y - p.y;
            set(w.param, drag_.startValue + up / (drag_.fine ? 10000.0 : 200.0) * (def.max - def.min));
        }
        break;
    }
    case DragKind::EnvPoint: {
        // Times go by the zoom held (see envZoom_), so the point stays under the mouse. Taken so far right that the
        // segments after it would run past the display's end, the display zooms out just enough to keep them in,
        // the point still under the mouse. Where they can't shrink (the release's end, or segments that are all
        // their least pixels), it stops at the end; pushed on past there, the display zooms out and its segment
        // grows (see envZoomStep()).
        const Rect r = envRect(w.a);
        const EnvIndices e = envIndices(w.a);
        auto get = [this](int index) { return value(index); };
        EnvShape s = envShape(r, e, get, envScale(w.a));
        if (p.x > envPointLimit(r, s, w.b)) {
            // The pixels the segments after it take: a part that stays, and one that grows with the zoom.
            const float least = envLeastScale(r, e);
            const float after = r.right() - 10.0f - envPointLimit(r, s, w.b);
            const EnvShape bare = envShape(r, e, get, 0.0f);
            const float fixed = r.right() - 10.0f - envPointLimit(r, bare, w.b);
            if (after - fixed > 1.0f) {
                const float fits = (r.right() - 10.0f - p.x - fixed) / ((after - fixed) / s.scale);
                envZoom_.scale = std::max(std::min(fits, s.scale), least);
                s = envShape(r, e, get, envScale(w.a));
            }
        }
        const float x = std::min(p.x, envPointLimit(r, s, w.b));
        auto time = [&](int param, float from, bool attack = false) {
            return envSpanTime(param, x - from, s.scale, attack);
        };
        if (w.b == 0) {
            set(e.attack, time(e.attack, s.p0.x, true));
        } else if (w.b == 1) {
            set(e.hold, time(e.hold, s.p1.x)); // to the left of its shortest place: none
        } else if (w.b == 2) {
            set(e.decay, time(e.decay, s.ph.x));
            set(e.sustain, (s.bottom - p.y) / (s.bottom - s.top));
        } else {
            set(e.release, time(e.release, s.p2.x));
        }
        break;
    }
    case DragKind::EnvCurve:
        // Dragging up lifts the middle of the segment: a faster attack start, a slower decay or release start.
        set(w.param, drag_.startValue + (w.b == 0 ? 1.0 : -1.0) * (drag_.start.y - p.y) / 100.0);
        break;
    case DragKind::FilterPoint: {
        const std::vector<int> params = filterPointParams(w.a);
        const auto [cutoff, resonance] = filterHandleValues(fxFilterArea(w.a), p);
        set(params[0], cutoff);
        set(params[1], resonance);
        break;
    }
    case DragKind::DynGrip: {
        // Across the bar for the threshold; Below stays at or under Above, and Above at or over Below.
        const Rect bar = mdBar(mdDisplay(w.a), w.b / 2);
        const double db = (p.x - bar.x) / bar.w * kMdRangeDb - kMdRangeDb;
        const int below = mdThreshold(w.a, (w.b / 2) * 2 + 1), above = mdThreshold(w.a, (w.b / 2) * 2);
        if (w.b % 2 == 1)
            set(below, std::min(db, value(above)));
        else
            set(above, std::max(db, value(below)));
        break;
    }
    case DragKind::AmpMic: { // up across the cone for its position, right away from it for its distance
        const std::optional<Rect> card = fxCardOf(w.a);
        if (!card)
            break;
        const Rect display = ampCabDisplay(*card);
        const Point lo = ampMicPoint(display, 0.0, 0.0), hi = ampMicPoint(display, 1.0, 1.0);
        set(paramIndex(w.a, FxParam::AmpPosition), std::clamp((lo.y - p.y) / (lo.y - hi.y), 0.0f, 1.0f));
        set(paramIndex(w.a, FxParam::AmpDistance), std::clamp((p.x - lo.x) / (hi.x - lo.x), 0.0f, 1.0f));
        break;
    }
    case DragKind::UtilityHandle: {
        const std::optional<Rect> card = fxCardOf(w.a);
        if (!card)
            break;
        const Rect display = fxDisplayIn(*card, FxType::Utility);
        const Rect strip = utilityStrip(display), field = utilityField(display);
        switch (w.b) {
        case 0: // Gain up the strip
            set(w.param, (strip.bottom() - p.y) / strip.h);
            break;
        case 1: // Pan across the field
            set(w.param, (p.x - field.center().x) / (field.w * 0.5f));
            break;
        case 2: // Width out from the middle, either way
            set(w.param, std::abs(p.x - utilityMiddle(field, value(paramIndex(w.a, FxParam::UtilityPan)))) /
                             utilitySpread(field, 1.0));
            break;
        default: // Mono Bass up the field; off at its bottom
            set(w.param, std::max((field.bottom() - p.y) / field.h, 0.0f));
            break;
        }
        break;
    }
    case DragKind::PeqPoint: {
        const Rect area = peqArea(w.a);
        set(peqFreqIndex(w.a, w.b), (p.x - area.x) / area.w);
        const double db = (area.center().y - p.y) / (area.h * 0.5f) * kPeqRangeDb;
        const PeqType type = peqType(value(peqTypeIndex(w.a, w.b)));
        if (isCut(type)) // up and down is the resonance's peak
            set(peqQIndex(w.a, w.b), normFromQ(std::clamp(std::pow(10.0, db / 20.0), kMinQ, kMaxQ)));
        else if (type != PeqType::Notch) // a notch's handle only moves across
            set(peqGainIndex(w.a, w.b), db);
        break;
    }
    case DragKind::Scroll: {
        // The cards follow the thumb.
        const auto area = static_cast<ScrollArea>(w.a);
        const float cards = scrollLength(area);
        const float shown = scrollShown(area);
        const float room = w.rect.w - 8.0f - scrollThumb(area).w;
        if (room > 0.0f)
            setScroll(area, static_cast<float>(drag_.startValue) + (p.x - drag_.start.x) / room * (cards - shown));
        break;
    }
    case DragKind::ModSource: {
        // Held a moment over a view's tab, a node's tab, an operator page's tab, or a node or its name in the graph or
        // the matrix, a dragged handle shows that view, node or tab (see dwellStep()), so what it shows can take the
        // drop; over a pill, its list, so one of its rows can take the handle as that modulation's aux.
        const auto at = hitTest(p);
        if (const std::optional<Widget> over = dwellTarget(at); !over)
            dwell_ = {};
        else if (!dwell_.over || !dwell_.over->same(*over))
            dwell_ = {over, clock_(), false};
        dwellStep();
        if (at && at->kind == Kind::ModPill && !modListOn(*at))
            openModList(*at, true);
        break;
    }
    case DragKind::Pan:
        applyPan(p);
        break;
    case DragKind::Panel: // a panel's own (the preset browser's sidebar's thumb, the SEQ view's roll, say)
        if (drag_.panel)
            drag_.panel->drag(drag_);
        break;
    case DragKind::ModListScroll: { // a pill's list's rows follow its thumb
        const Rect track{w.rect.x + 3.0f, w.rect.y, 4.0f, w.rect.h};
        const float room = track.h - modListThumb(track).h;
        if (room > 0.0f)
            modListScroll_ = std::clamp(
                static_cast<float>(drag_.startValue) + (p.y - drag_.start.y) / room * modListEnd(), 0.0f, modListEnd());
        break;
    }
    case DragKind::Tempo: { // a BPM every 2 px up, in whole BPM; with Shift, a tenth of one, in tenths
        const double step = drag_.fine ? 0.1 : 1.0;
        const double moved = std::trunc((drag_.start.y - p.y) / 2.0f) * step;
        const double bpm = clampTempo(std::round((drag_.startValue + moved) / step) * step);
        if (bpm != controller_.ownTempo())
            controller_.setOwnTempo(bpm);
        break;
    }
    case DragKind::None:
    case DragKind::SourceMove:
        break;
    }
}

// Ctrl, from the keyboard or the mouse's messages: a panel's grid shows while it is held (see PanelHost::ctrlHeld()).
bool View::setCtrl(bool held) {
    if (held == ctrlHeld_)
        return false;
    ctrlHeld_ = held;
    return true; // the grid's points show or go, and a node being dragged snaps or goes free
}

bool View::mouseMove(Point p, Modifiers mods) {
    const Point before = hoverPoint_;
    hoverPoint_ = p;
    mouseOver_ = true;
    const bool ctrlChanged = setCtrl(mods.ctrl);
    if (drag_.kind == DragKind::None) {
        envZoomLeft(p);
        // The widget under the mouse, all of it (a tooltip goes under its rect), or for the partial editor's bars,
        // where along them the mouse is: its caption names the partial under it.
        std::optional<Widget> hit = hitTest(p);
        auto same = [](const Widget &a, const Widget &b) {
            return a.same(b) && a.label == b.label && a.compact == b.compact && a.shrunk == b.shrunk &&
                   std::memcmp(&a.rect, &b.rect, sizeof(Rect)) == 0 && std::memcmp(&a.clip, &b.clip, sizeof(Rect)) == 0;
        };
        // Ctrl going down or up shows or hides a graph's grid, and so may the mouse going in or out of a panel's area
        // with it held (see Panel::hoverMoves()).
        const bool gridEdge = std::any_of(panels_.begin(), panels_.end(),
                                          [&](const Panel *panel) { return panel->hoverMoves(before, p); });
        // The card under the mouse, which shows its fold and x, and which a theme without card outlines outlines.
        const int card = cardAt(p);
        const bool cardChanged = card != hoverCard_;
        hoverCard_ = card;
        bool changed = cardChanged || ctrlChanged || gridEdge || hit.has_value() != hover_.has_value() ||
                       (hit && !same(*hit, *hover_)) ||
                       (hit && hit->kind == Kind::PartialArea) ||
                       (hit && isRatioDisplay(*hit) && ratioDigit(p.x) != ratioDigit(before.x)) || // its lit digit
                       (hit && stepperButton(*hit, p) != stepperButton(*hit, before)); // its lit - or +
        hover_ = std::move(hit);
        // What the panels open as the mouse goes over it (a category of the MODS view's cascading menu).
        if (changed)
            for (Panel *panel : panels_)
                panel->hoverChanged(hover_);
        return changed;
    }
    if ((drag_.kind == DragKind::Value || drag_.kind == DragKind::ModWheel || drag_.kind == DragKind::Glow) &&
        mods.shift != drag_.fine) {
        // Switching to/from fine mode re-anchors the drag so the value doesn't jump.
        drag_.fine = mods.shift;
        drag_.start = p;
        drag_.startValue = drag_.kind == DragKind::Glow ? sliderDragged() : value(drag_.widget.param);
    }
    drag_.now = p;
    if (!drag_.moved && std::hypot(p.x - drag_.start.x, p.y - drag_.start.y) > 3.0f) {
        drag_.moved = true;
        if (mods.ctrl) // a card or node carried with Ctrl held: a copy of it
            copyDragged();
    }
    drag_.mods = mods; // what a panel's drag goes by (the SEQ view's roll's, off the grid with Shift held, say)
    // A value moves only once the mouse has: the jitter of a click, or between a double-click's presses, leaves it
    // (and what a matrix cell joins) as it was.
    if ((drag_.kind == DragKind::Value || drag_.kind == DragKind::Tempo) && !drag_.moved)
        return true;
    applyDrag(p);
    return true;
}

void View::mouseDown(Point p, Modifiers mods) {
    setCtrl(mods.ctrl);
    pressed_ = hitTest(p); // before the press acts, for a double-click that may follow
    pressPoint_ = p;
    // A click in the field being typed in leaves it be; anywhere else, it takes the value typed.
    if (typing_ && typingField().contains(p))
        return;
    endTyping(true);
    finishDrag(); // a drag that never saw its button go up
    envZoomLeft(p);
    for (Panel *panel : panels_) // what glides into place settles there (the FX view's cards), where a click means it
        panel->settle();
    // Before a drag can take a route down to 0 %: the poll timer waits while the mouse moves, and a
    // state load may have replaced the layout since it last ran.
    routing_.keepRoutes();
    const auto hit = hitTest(p);
    // The MODS view's filter and the preset browser's search let the keyboard go at a click anywhere else, and the
    // points picked on the SEQ view's lane's curve let go at a click off the lane (see Panel::pressing()).
    for (Panel *panel : panels_)
        panel->pressing(hit ? &*hit : nullptr);
    // A pill's open list closes at a click anywhere else, which does nothing more; but not at a source's handle,
    // which can then be dragged onto one of its rows.
    if (modList_ && !(hit && (hit->popup || hit->kind == Kind::ModPill || hit->kind == Kind::SourceHandle ||
                              hit->kind == Kind::MacroHandle))) {
        modList_.reset();
        return;
    }
    if (!hit)
        return;
    const Widget &w = *hit;
    // Ctrl+Click types a value into whatever shows one: a knob, a modulation's depth, a route.
    if (mods.ctrl && (w.kind == Kind::Knob || w.kind == Kind::ValueText || w.kind == Kind::WireLabel ||
                      w.kind == Kind::OutLabel || w.kind == Kind::GridCell || w.kind == Kind::TempoBox ||
                      w.kind == Kind::ModsAuxDepth)) {
        startTyping(w);
        return;
    }
    // A stepper's - and + step it as a notch of the wheel does, an undo step each.
    if (const int side = stepperButton(w, p); side != 0) {
        edit(w.param, notchValue(w.param, side));
        return;
    }
    drag_.widget = w;
    drag_.start = drag_.now = p;
    drag_.fine = mods.shift;
    UiLayout &layout = controller_.layout();
    if (w.kind == Kind::SearchField) // a field pressed (a panel's own, which it may take): its caret shows at once
        caretTicks_ = 0;
    for (Panel *panel : panels_) // the panels' own (the presets', see presetbrowser.cpp, the SEQ, MODS and FX views')
        if (panel->press(w, p, mods))
            return;

    switch (w.kind) {
    case Kind::Knob:
    case Kind::ModDot:
    case Kind::SlotBar:
    case Kind::ModsScale:
    case Kind::ModsCurve:
    case Kind::ModsAuxDepth:
        drag_.kind = DragKind::Value;
        // A depth handle with an aux Alt-dragged sets the aux depth instead, up and down (see applyDrag()).
        if (w.kind == Kind::ModDot && mods.alt && w.a >= 0 && w.b >= 0 &&
            slotAux([this](int index) { return value(index); }, w.a, w.b) >= 0)
            drag_.widget.param = slotAuxDepthIndex(w.a, w.b);
        drag_.startValue = value(drag_.widget.param);
        if (isRatioDisplay(w)) // the digit pressed is the one dragged, wherever the mouse goes
            drag_.widget.b = ratioDigit(p.x);
        controller_.beginEdit(drag_.widget.param);
        if ((w.kind == Kind::SlotBar || w.kind == Kind::ModsScale) && !drag_.fine) // to the mouse at once; with
            applyDrag(p);                                                           // Shift, from where it is
        break;
    case Kind::ModPill: // opens its list, or closes it again
        if (modListOn(w))
            modList_.reset();
        else
            openModList(w, false);
        break;
    case Kind::ModList:
    case Kind::ModListRow:
        break; // the list stays open
    case Kind::ModsAuxClear:
        mods_.setAux(w.a, w.b, -1);
        break;
    case Kind::ModListScroll: { // as the MODS view's
        const Rect track{w.rect.x + 3.0f, w.rect.y, 4.0f, w.rect.h};
        const Rect thumb = modListThumb(track);
        if (p.y < thumb.y || p.y >= thumb.bottom())
            modListScroll_ = std::clamp((p.y - thumb.h * 0.5f - track.y) / std::max(track.h - thumb.h, 1.0f) * modListEnd(),
                                        0.0f, modListEnd());
        drag_.kind = DragKind::ModListScroll;
        drag_.startValue = modListFirst();
        break;
    }
    case Kind::SourceHandle:
    case Kind::MacroHandle:
        drag_.kind = DragKind::ModSource;
        break;
    case Kind::EnvPoint:
        drag_.kind = DragKind::EnvPoint;
        for (int index : envPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::EnvCurve:
        drag_.kind = DragKind::EnvCurve;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::FilterPoint:
        drag_.kind = DragKind::FilterPoint;
        for (int index : filterPointParams(w.a))
            controller_.beginEdit(index);
        break;
    case Kind::DynZone: // up and down for its ratio, as on a knob; its band's knobs show
        fxPanel_.showBand(w.a, w.b / 2);
        drag_.kind = DragKind::Value;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::DynGrip:
        fxPanel_.showBand(w.a, w.b / 2); // its band's knobs show
        if (mods.alt) { // up and down for its ratio, as on a knob; the line stays where it is
            drag_.kind = DragKind::Value;
            drag_.startValue = value(w.param);
            controller_.beginEdit(w.param);
            break;
        }
        drag_.kind = DragKind::DynGrip;
        controller_.beginEdit(mdThreshold(w.a, w.b));
        break;
    case Kind::AmpMic:
        drag_.kind = DragKind::AmpMic;
        controller_.beginEdit(paramIndex(w.a, FxParam::AmpPosition));
        controller_.beginEdit(paramIndex(w.a, FxParam::AmpDistance));
        break;
    case Kind::UtilityHandle:
        drag_.kind = DragKind::UtilityHandle;
        controller_.beginEdit(w.param);
        break;
    case Kind::PeqPoint:
        fxPanel_.showBand(w.a, w.b); // its knobs show
        if (value(peqOnIndex(w.a, w.b)) < 0.5) // a band switched off comes on, an undo step of its own
            edit(peqOnIndex(w.a, w.b), 1.0);
        if (mods.alt) { // up and down for its Q, as on a knob; the handle stays where it is
            drag_.kind = DragKind::Value;
            drag_.startValue = value(w.param);
            controller_.beginEdit(w.param);
            break;
        }
        drag_.kind = DragKind::PeqPoint;
        for (int index : peqPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::SourceMenuButton:
        if (!sourceShadow()) {
            flash(trf(Str::MsgSourcesFull, {std::to_string(kMaxSources)}));
        } else {
            sourceMenuOpen_ = !sourceMenuOpen_ || sourceMenuAt_.x >= 0.0f; // from the dashed card's, to the button's
            sourceMenuAt_ = {-1.0f, -1.0f};
        }
        break;
    case Kind::SourceShadow: // the list at the mouse, kept inside the editor
        sourceMenuAt_ = menuAtMouse(p, sourceMenu({0.0f, 0.0f}));
        sourceMenuOpen_ = true;
        hover_.reset();
        break;
    case Kind::SourceMenu:
        if (w.a == 0) // a click anywhere else closes the list
            sourceMenuOpen_ = false;
        break;
    case Kind::SourceMenuItem:
        sourceMenuOpen_ = false;
        addSource(static_cast<SourceType>(w.a));
        break;
    case Kind::Scrollbar: {
        // On the thumb, drag it; elsewhere on the track, the thumb jumps to center on the click and can be dragged on.
        const auto area = static_cast<ScrollArea>(w.a);
        const Rect thumb = scrollThumb(area);
        if (p.x < thumb.x || p.x >= thumb.right()) {
            const Rect track = w.rect.inset(4.0f);
            const float cards = scrollLength(area);
            const float shown = scrollShown(area);
            const float room = track.w - thumb.w;
            setScroll(area, room > 0.0f ? (p.x - thumb.w * 0.5f - track.x) / room * (cards - shown) : 0.0f);
        }
        drag_.kind = DragKind::Scroll;
        drag_.startValue = scrollFirst(area);
        break;
    }
    case Kind::DynBand:
    case Kind::PeqBand:
        fxPanel_.showBand(w.a, w.b);
        break;
    case Kind::PeqBandOn: // its knobs show too
        fxPanel_.showBand(w.a, w.b);
        edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
        break;
    case Kind::KeysToggle:
        layout.keysCollapsed = !layout.keysCollapsed;
        controller_.layoutChanged(); // asks the host for the new window size
        controller_.keysCollapsedSet(layout.keysCollapsed);
        break;
    case Kind::ViewTab:
        seqTabPressedShown_ = seq_.wide(); // a double-click on its tab, while it shows, changes its size
        showView(static_cast<RoutingView>(w.a));
        break;
    case Kind::SourceGrip:
        drag_.kind = DragKind::SourceMove;
        drag_.scrollStart = sourceScroll();
        drag_.step = true;
        controller_.beginStep();
        break;
    case Kind::SourceRemove:
        removeSource(w.a);
        break;
    case Kind::SourceFold:
        setFolded(false, w.a, !sourceFolded(w.a));
        hover_.reset();
        break;
    case Kind::Choice:
        if (isLfoClockChoice(w))
            setLfoClock(w.a, static_cast<LfoClock>(w.b));
        else
            edit(w.param, w.b);
        break;
    case Kind::Toggle:
        flipSwitch(w.param);
        break;

    case Kind::NoteDisplay:
        shapes_.toggleShape(curveOf(w.a)); // a second click closes it
        hover_.reset();
        break;
    case Kind::ListButton:
        if (isFilterSlope(w.param) && !hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param))))))
            break; // a comb has no slope: its list stays shut
        listMenu_ = listMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::ListStep:
        stepList(w.param, w.b);
        break;
    case Kind::ListMenu:
        if (w.a == 0) // a click anywhere else closes the list
            listMenu_ = -1;
        break;
    case Kind::ListItem:
        if (!listItemFits(w.param, w.b))
            break; // the list stays open for another pick
        listMenu_ = -1;
        edit(w.param, w.b); // a drawn curve is edited in the card, or the big editor
        // An LFO holding the retired Custom shape (a state can) goes back to its size as it takes another.
        if (isLfoShape(w.param))
            shapes_.narrow(sourceOf(w.param));
        break;
    case Kind::PeqTypeButton:
        fxPanel_.showBand(w.a, w.b);
        peqMenu_ = peqMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::PeqTypeMenu:
        if (w.a == 0) // a click anywhere else closes the list
            peqMenu_ = -1;
        break;
    case Kind::PeqTypeItem:
        peqMenu_ = -1;
        edit(w.param, w.b);
        break;
    case Kind::Key:
        drag_.kind = DragKind::Key;
        controller_.playNote(keyboardBase() + w.a, true, keyVelocity(w.rect, drag_.start));
        break;
    case Kind::Octave:
        octave_ = std::clamp(octave_ + w.a, -2, 2);
        break;
    case Kind::PresetButton: {
        // Init and Load ask first when they would throw away changes, and Save before it overwrites a preset's file.
        // Save and Save As do nothing while greyed out.
        const auto action = static_cast<PresetAction>(w.a);
        if (!presetActionOn(action))
            break;
        const bool overwrites = presetStore().writesPresetFiles() && presetStore().presetWritable();
        const bool ask = action == PresetAction::Save     ? overwrites
                         : action == PresetAction::SaveAs ? false
                                                          : controller_.patchEdited();
        if (ask) {
            confirm_ = w.a;
            confirmFrom_ = nullptr;
            hover_.reset();
        } else {
            runPreset(action);
        }
        break;
    }
    case Kind::PresetStep:
        stepPreset(w.a);
        break;
    case Kind::TempoBox:
        drag_.kind = DragKind::Tempo;
        drag_.startValue = controller_.ownTempo();
        break;
    case Kind::TempoSync:
        controller_.setTempoSync(!controller_.tempoSync());
        break;
    case Kind::SearchClear: // the field keeps the keyboard, for what is typed next (see the panels' press())
        clearField(static_cast<Field>(w.a));
        hover_.reset();
        break;
    case Kind::SearchField: { // the caret where it is clicked, and a drag selects; with Shift, from where it was. The
                              // field takes the keyboard (see the panels' press()).
        TextField &t = field(static_cast<Field>(w.a));
        t.caret = t.stopAt(p.x);
        if (!mods.shift)
            t.anchor = t.caret;
        drag_.kind = DragKind::TextSelect;
        break;
    }
    case Kind::ConfirmBackdrop:
        if (w.a == 0) { // a click around the dialog cancels it
            confirm_ = -1;
            confirmFrom_ = nullptr;
        }
        break;
    case Kind::ConfirmButton:
        answerConfirm(w.a);
        break;
    case Kind::SettingsButton:
        settingsOpen_ = true;
        hover_.reset();
        break;
    case Kind::SettingsBackdrop:
        if (w.a == 0) // a click around the dialog closes it
            settingsOpen_ = false;
        break;
    case Kind::SettingsOption:
        controller_.setOversampling(w.b);
        break;
    case Kind::SettingsMpe:
        controller_.setMpe(w.b != 0);
        break;
    case Kind::SettingsMpeRange:
        controller_.setMpeBendRange(w.b);
        break;
    case Kind::SettingsTheme: {
        EditorSettings settings = controller_.editorSettings();
        settings.theme = kThemes[w.b].name;
        controller_.setEditorSettings(settings);
        break;
    }
    case Kind::SettingsHandles: {
        EditorSettings settings = controller_.editorSettings();
        settings.handleSize = w.b;
        controller_.setEditorSettings(settings);
        break;
    }
    case Kind::SettingsCards: {
        EditorSettings settings = controller_.editorSettings();
        settings.tintedCards = w.b != 0;
        controller_.setEditorSettings(settings);
        break;
    }
    case Kind::SettingsGlow: // dragged; the amount is kept when it is let go
    case Kind::SettingsLinks:
    case Kind::SettingsGrid:
    case Kind::SettingsHue:
        drag_.kind = DragKind::Glow;
        drag_.startValue = sliderDragged() = w.kind == Kind::SettingsLinks ? shownLinks()
                                             : w.kind == Kind::SettingsGrid ? shownGrid()
                                             : w.kind == Kind::SettingsHue
                                                 ? static_cast<float>(shownHue() >= 0.0 ? shownHue()
                                                                                        : hueOf(kThemes[theme_].accent))
                                                 : shownGlow();
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::SettingsClose:
        settingsOpen_ = false;
        hover_.reset();
        break;
    case Kind::SettingsManual: // the dialog stays open behind the browser
        controller_.openManual();
        break;
    case Kind::ModWheel:
        drag_.kind = DragKind::ModWheel;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::BendWheel: // the Pitch Bend param, edited as the mod wheel's knob is (the host follows it)
        drag_.kind = DragKind::BendWheel;
        controller_.beginEdit(paramIndex(GlobalParam::PitchBend));
        applyDrag(p);
        break;
    case Kind::SlotRemove:
        mods_.removeSlot(w.a, w.b);
        break;
    case Kind::SlotBypass:
        mods_.toggleBypass(w.a, w.b);
        break;
    case Kind::SourcePolarity:
        mods_.setSourcePolarity(w.a, !mods_.sourceBipolar(w.a));
        break;
    case Kind::SourceMods:
        showSourceMods(w.a);
        break;
    case Kind::ValueText:
    case Kind::MacroName:
        break; // double-click types a value, or renames the macro
    case Kind::UndoButton:
        if (w.a == 1)
            redo();
        else
            undo();
        break;
    default: // the kinds a press does nothing on
        break;
    }
}

void View::mouseUp(Point p, Modifiers) {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::ModSource: {
        const auto target = hitTest(p);
        // On a depth handle or a row of a pill's list: it becomes that modulation's aux; on a panel's own that takes it
        // (a MODS row's aux cell, as the aux too), what that panel makes of it.
        const bool row = target && target->popup && target->kind != Kind::ModList;
        if (target && (target->kind == Kind::ModDot || row) && target->a != w.a)
            mods_.setAux(target->a, target->b, w.a);
        else if (target && droppable(*target, w.a)) // only where the overlay rings a target
            mods_.assign(w.a, target->param);
        else if (target)
            for (Panel *panel : panels_)
                if (panel->dropSource(*target, w.a))
                    break;
        // A list that opened under the handle stays open only when the handle went onto one of its rows.
        if (modList_ && modList_->sprung) {
            if (row)
                modList_->sprung = false;
            else
                modList_.reset();
        }
        break;
    }
    case DragKind::Glow: {
        EditorSettings settings = controller_.editorSettings();
        if (w.kind == Kind::SettingsLinks)
            settings.linkIntensity = linksDragged_;
        else if (w.kind == Kind::SettingsGrid)
            settings.gridIntensity = gridDragged_;
        else if (w.kind == Kind::SettingsHue)
            settings.accentHue = hueDragged_;
        else
            settings.glow = glowDragged_;
        controller_.setEditorSettings(settings);
        break;
    }
    case DragKind::SourceMove:
        drag_.now = p;
        if (drag_.moved) // a click leaves the card where it is
            mods_.setSourceOrder(sourcesShown());
        break;
    case DragKind::Tempo: // each move set it already
        break;
    case DragKind::Panel: // its panel hears of it, then of the drag's end below
        drag_.now = p;
        if (drag_.panel)
            drag_.panel->drop(drag_);
        break;
    case DragKind::Value:
    case DragKind::EnvPoint:
    case DragKind::EnvCurve:
    case DragKind::FilterPoint:
    case DragKind::PeqPoint:
    case DragKind::AmpMic:
    case DragKind::UtilityHandle:
    case DragKind::DynGrip:
    case DragKind::Key:
    case DragKind::ModWheel:
    case DragKind::BendWheel:
    case DragKind::Scroll:
    case DragKind::TextSelect:
    case DragKind::ModListScroll:
    case DragKind::Pan: // the left button let go of mid-pan ends it too
    case DragKind::None:
        break; // finishDrag() ends what they hold
    }
    finishDrag();
    envZoomLeft(p); // a knob's: its envelope's display is elsewhere
    hover_ = hitTest(p);
}

void View::cancelDrag() {
    if (drag_.kind == DragKind::None)
        return;
    if (drag_.kind == DragKind::ModSource && modList_ && modList_->sprung) // opened under the handle, for it alone
        modList_.reset();
    if (drag_.copyLayout) { // a copy made for it goes again, its undo step left empty
        std::vector<std::pair<int, double>> back;
        for (int i = 0; i < kParamCount; ++i)
            if (value(i) != drag_.copyBefore[i])
                back.push_back({i, drag_.copyBefore[i]});
        editMany(back);
        controller_.layout() = *drag_.copyLayout;
        controller_.layoutChanged();
    }
    finishDrag();
    hover_.reset();
    if (envZoom_.env >= 0) // the mouse gone, a zoom held goes back to filling the display
        envZoom_.easing = true;
}

void View::beginCopyDrag() {
    std::vector<double> before(kParamCount);
    for (int i = 0; i < kParamCount; ++i)
        before[i] = value(i);
    UiLayout layout = controller_.layout();
    if (!drag_.step) { // the copy and where it lands one undo step
        controller_.beginStep();
        drag_.step = true;
    }
    drag_.copyBefore = std::move(before);
    drag_.copyLayout = std::move(layout);
}

void View::cancelCopyDrag() {
    drag_.copyBefore.clear();
    drag_.copyLayout.reset();
}

void View::copyDragged() {
    if (drag_.kind != DragKind::SourceMove)
        return; // a panel's own makes its copy itself (see beginCopyDrag())
    beginCopyDrag();
    // Made right after its original, which the mouse stays over until it carries it elsewhere.
    const int source = drag_.widget.a;
    const float scroll = sourceScroll();
    const int copy = addSource(mods_.sourceTypeOf(source), source);
    if (copy < 0) {
        cancelCopyDrag();
        return;
    }
    setFolded(false, copy, sourceFolded(source));
    setScroll(ScrollArea::Sources, scroll); // as it was grabbed, not scrolled to show the copy
    drag_.widget.a = copy;
}

void View::finishDrag() {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::Value:
        routing_.keepRoutes(); // a route it switched on, in its undo step (see edit())
        controller_.endEdit(w.param);
        break;
    case DragKind::EnvCurve:
    case DragKind::ModWheel:
    case DragKind::UtilityHandle:
        controller_.endEdit(w.param);
        break;
    case DragKind::PeqPoint:
        for (int index : peqPointParams(w.a, w.b))
            controller_.endEdit(index);
        break;
    case DragKind::AmpMic:
        controller_.endEdit(paramIndex(w.a, FxParam::AmpPosition));
        controller_.endEdit(paramIndex(w.a, FxParam::AmpDistance));
        break;
    case DragKind::DynGrip:
        controller_.endEdit(mdThreshold(w.a, w.b));
        break;
    case DragKind::EnvPoint:
        for (int index : envPointParams(w.a, w.b))
            controller_.endEdit(index);
        break;
    case DragKind::FilterPoint:
        for (int index : filterPointParams(w.a))
            controller_.endEdit(index);
        break;
    case DragKind::Key:
        controller_.playNote(keyboardBase() + w.a, false, 0);
        break;
    case DragKind::BendWheel: // springs back to center, in the same gesture
        if (value(paramIndex(GlobalParam::PitchBend)) != 0.0)
            controller_.setValue(paramIndex(GlobalParam::PitchBend), 0.0);
        controller_.endEdit(paramIndex(GlobalParam::PitchBend));
        break;
    case DragKind::Glow: // the amount it was dragged to is kept only once it is let go (see mouseUp())
        glowDragged_ = linksDragged_ = gridDragged_ = hueDragged_ = -1.0f;
        break;
    case DragKind::ModListScroll:
    case DragKind::ModSource:
    case DragKind::SourceMove:
    case DragKind::Scroll:
    case DragKind::Pan:
    case DragKind::TextSelect:
    case DragKind::Tempo:
    case DragKind::None:
        break;
    case DragKind::Panel: // its panel hears of it (the SEQ view's note still heard stops, a release that never came)
        if (drag_.panel)
            drag_.panel->release();
        break;
    }
    if (drag_.step)
        controller_.endStep();
    drag_ = Drag{};
    dwell_ = {};
}

void View::mouseLeave() {
    mouseOver_ = false;
    if (drag_.kind == DragKind::None) {
        hover_.reset();
        hoverCard_ = -1;
        if (envZoom_.env >= 0)
            envZoom_.easing = true;
    }
}

void View::noteCard(int id, Rect card, Rect shown) {
    const float x = std::max(card.x, shown.x), y = std::max(card.y, shown.y);
    const float right = std::min(card.right(), shown.right()), bottom = std::min(card.bottom(), shown.bottom());
    // Where an earlier paint noted it goes (a partial paint notes the cards it repaints again, and a full one starts
    // over); where this one did stays, as on top of what it was painted under.
    std::erase_if(cardRects_, [&](const NotedCard &n) { return n.id == id && n.paint != cardPaint_; });
    if (right > x && bottom > y)
        cardRects_.push_back({id, {x, y, right - x, bottom - y}, cardPaint_});
}

int View::cardAt(Point p) const {
    for (auto it = cardRects_.rbegin(); it != cardRects_.rend(); ++it) // the last painted is on top
        if (it->rect.contains(p))
            return it->id;
    return -1;
}

// Whether the mouse is over the card as it is painted now: the card's widgets are painted after every card's face has
// noted where it is, so a card just moved, widened or scrolled under a resting mouse counts. A card painted within
// another counts as over that one too (an effect in a split's band, for its container).
bool View::cardFoldShown(int card) const {
    if (touched_)
        return true;
    if (!mouseOver_)
        return false;
    for (auto it = cardRects_.rbegin(); it != cardRects_.rend(); ++it)
        if (it->rect.contains(hoverPoint_)) {
            if (it->id == card)
                return true;
            const Rect in = it->rect;
            return std::any_of(it + 1, cardRects_.rend(), [&](const NotedCard &n) {
                return n.id == card && in.x >= n.rect.x && in.y >= n.rect.y && in.right() <= n.rect.right() &&
                       in.bottom() <= n.rect.bottom();
            });
        }
    return false;
}

// A card's outline: always, but in a theme whose cards go without one, only while it is dragged or under the mouse.
bool View::cardOutlined(int id, bool dragged) const {
    return !look.flat || dragged || (hoverCard_ == id && drag_.kind == DragKind::None);
}

// An effect's or a source's card under what it holds: its shadow, its fill in the cards' plain color in every theme
// (its own color goes on what is on it: its icon or handle, its knobs' arcs, its curves and its lit switches), or with
// tinted cards on in a flat theme tinted with its color, faintly while it is off (cardFace()), and its outline (in its color at `edge` while it is on), and noted as card `id`, as much of it as `shown` shows, for the
// mouse to find.
void View::paintCardFace(Canvas &c, Rect card, Color color, bool on, int id, bool dragged, float edge, Rect shown) {
    paintCardShadow(c, card);
    c.fillRect(card, cardFace(color, on), kRadiusCard);
    noteCard(id, card, shown);
    if (cardOutlined(id, dragged))
        c.strokeRect(card, dragged ? color : (on ? color.alpha(edge) : kBorder), dragged ? 1.5f : 1.0f, kRadiusCard);
}

std::vector<View::DimArea> View::dimmedAreas() const {
    std::vector<DimArea> areas;
    auto body = [](Rect p) { return Rect{p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}; };
    // A switched-off source's card below its title row (a folded one's name is already dim).
    if (!sourcesHidden())
        for (int s : sources()) {
            const float card = cardOf(s);
            if (card == kOffScreen || mods_.sourceOn(s) || sourceFolded(s))
                continue;
            areas.push_back({body(sourceCard(card, sourceSpan(s))), cardFace(kSourceColors[s], false), sourceCardClip()});
        }
    // So is a switched-off effect's, in the effects graph's inspector or along the row.
    for (const auto &[slot, p, clip] : fxPanel_.offCards())
        areas.push_back({body(p), cardFace(fxTypeColor(fx_.slotType(slot)), false), clip});
    // And the inspector's page while its operator, filter or shaper is off, below the tabs, title and switch.
    if (!inspectorCovered() && !routing_.nodeOn(controller_.layout().selectedNode))
        areas.push_back({kInspectorPage, Color{0.0f, 0.0f, 0.0f, 0.0f}, kInspectorPage});
    return areas;
}

// A macro's knob while it modulates nothing, and an LFO's Phase while notes don't start the LFO there: faint, as the
// macro's handle is, though they still turn.
bool View::knobIdle(const Widget &w) const {
    if (w.kind != Kind::Knob || w.param < 0)
        return false;
    const int macro = macroOf(w.param);
    return (macro >= 0 && !sourceLive(kMaxSources + macro)) ||
           (isLfoPhase(w.param) && !lfoPhaseUsed(sourceOf(w.param)));
}

// Whether a widget is drawn dimmed: one that takes no input, one that does nothing for now, an idle knob, or one in a
// dimmed area.
bool View::widgetDimmed(const Widget &w, const std::vector<DimArea> &areas) const {
    if (w.disabled || w.faint || knobIdle(w))
        return true;
    const Point m = w.rect.center();
    return std::any_of(areas.begin(), areas.end(), [&](const DimArea &a) { return a.area.contains(m); });
}

std::optional<View::PanStart> View::panAt(Point p) const {
    if (drag_.kind != DragKind::None || settingsOpen_ || confirm_ >= 0) // a left-button drag, or a dialog, goes on
        return std::nullopt;
    if (Panel *panel = modalPanel()) { // only what it pans, over everything else (the preset browser's list)
        if (const auto pan = panel->panAt(p))
            return PanStart{PanArea::Panel, pan->second, panel, pan->first};
        return std::nullopt;
    }
    // A pill's open list moves its rows when they scroll. Another open list or menu takes nothing.
    const auto hit = hitTest(p);
    if (hit && hit->popup && modList_)
        return modListEnd() > 0.0f ? std::optional(PanStart{PanArea::ModList, modListFirst()}) : std::nullopt;
    if (hit) {
        const Kind k = hit->kind;
        if (hit->popup || k == Kind::ModList || k == Kind::ModListRow || k == Kind::FxAdd || k == Kind::FxAddNode ||
            k == Kind::SourceMenuItem || k == Kind::ListItem || k == Kind::PeqTypeItem ||
            ((k == Kind::FxMenu || k == Kind::SourceMenu || k == Kind::ListMenu || k == Kind::PeqTypeMenu) &&
             hit->a != 0) ||
            std::any_of(panels_.begin(), panels_.end(), [&](const Panel *panel) { return panel->blocksPan(*hit); }))
            return std::nullopt;
    }
    if (!sourcesHidden() && kModPanel.contains(p))
        return PanStart{PanArea::Sources, sourceScroll()};
    for (Panel *panel : panels_) // what the panels shown in their places pan (the SEQ view's roll, the MODS rows, the
                                 // FX view's row and graph, the INPUTS tab's rows)
        if (const auto pan = panel->panAt(p))
            return PanStart{PanArea::Panel, pan->second, panel, pan->first};
    return std::nullopt;
}

void View::middleDown(Point p) {
    const auto pan = panAt(p);
    if (!pan)
        return;
    const PanArea area = pan->area;
    const float first = pan->first;
    // Open lists and menus close as a click there closes them, since what they were opened on may scroll away; but
    // not a pill's list that is being scrolled, nor what a panel's popup pans (the preset browser's list).
    if (!(area == PanArea::Panel && pan->panel->modal()) && area != PanArea::ModList) {
        closeMenus();
        routingPanel_.closeAlgorithms();
    }
    menuLive_ = false;
    endTyping(true);
    drag_ = Drag{};
    drag_.kind = DragKind::Pan;
    drag_.widget.a = static_cast<int>(area);
    drag_.widget.b = pan->part;
    drag_.panel = pan->panel;
    drag_.start = drag_.now = p;
    drag_.startValue = first;
    if (area == PanArea::Panel)
        pan->panel->panBegin(pan->part);
}

void View::middleUp(Point p) {
    if (drag_.kind != DragKind::Pan)
        return; // a left-button drag goes on
    finishDrag();
    hover_ = hitTest(p);
}

bool View::touchPanBegin(Point p) {
    // What the first finger pressed, or panned, ends, nothing dropped, and the pan starts over from between the two.
    touchPress_.reset();
    if (drag_.kind == DragKind::Pan)
        finishDrag();
    else
        cancelDrag();
    middleDown(p);
    return drag_.kind == DragKind::Pan;
}

bool View::dragsOnTouch(const Widget &w) const {
    switch (w.kind) {
    case Kind::Knob:
    case Kind::ModDot:
    case Kind::SlotBar:
    case Kind::ModsScale:
    case Kind::ModsCurve:
    case Kind::ModsAuxDepth:
    case Kind::ModListScroll:
    case Kind::Scrollbar:
    case Kind::SourceHandle:
    case Kind::MacroHandle:
    case Kind::EnvPoint:
    case Kind::EnvCurve:
    case Kind::FilterPoint:
    case Kind::DynZone:
    case Kind::DynGrip:
    case Kind::AmpMic:
    case Kind::UtilityHandle:
    case Kind::PeqPoint:
    case Kind::SourceGrip:
    case Kind::Key:
    case Kind::TempoBox:
    case Kind::SearchField:
    case Kind::SettingsGlow:
    case Kind::SettingsLinks:
    case Kind::SettingsGrid:
    case Kind::SettingsHue:
    case Kind::ModWheel:
    case Kind::BendWheel:
        return true;
    default: // and the panels' own (the shape editors' and the routing graph's among them)
        return std::any_of(panels_.begin(), panels_.end(), [&](const Panel *panel) { return panel->dragsOnTouch(w); });
    }
}

void View::touchDown(Point p, bool secondTap, float slop) {
    touchPress_.reset();
    touched_ = true;
    hoverPoint_ = p;
    if (panAt(p)) { // what it does waits until it moves or lifts
        touchPress_ = TouchPress{p, secondTap, slop};
        return;
    }
    if (secondTap)
        doubleClick(p, {});
    else
        mouseDown(p, {});
}

bool View::touchMove(Point p) {
    if (touchPress_) {
        const TouchPress press = *touchPress_;
        if (std::hypot(p.x - press.at.x, p.y - press.at.y) <= press.slop)
            return false;
        touchPress_.reset();
        // Off what drags, it pans what scrolls there; on it, it drags it, as the mouse would from where it landed.
        if (const auto hit = hitTest(press.at); !(hit && dragsOnTouch(*hit))) {
            middleDown(press.at);
            if (drag_.kind == DragKind::Pan)
                return mouseMove(p, {});
        }
        mouseDown(press.at, {});
    }
    return mouseMove(p, {});
}

void View::touchUp(Point p) {
    if (touchPress_) { // a tap: a click where it landed
        const TouchPress press = *touchPress_;
        touchPress_.reset();
        if (press.second)
            doubleClick(press.at, {});
        else
            mouseDown(press.at, {});
        mouseUp(press.at, {});
    } else if (drag_.kind == DragKind::Pan) {
        middleUp(p);
    } else {
        mouseUp(p, {});
    }
    hover_.reset(); // a finger lifted hovers over nothing
}

void View::touchCancel() {
    touchPress_.reset();
    cancelDrag();
    hover_.reset();
}

bool View::touchPanMove(Point p) {
    if (drag_.kind != DragKind::Pan)
        return false;
    hoverPoint_ = p;
    drag_.now = p;
    applyPan(p);
    return true;
}

void View::touchPanEnd(Point p) {
    middleUp(p);
}

void View::wheelAcross(Point p, float steps) {
    if ((drag_.kind != DragKind::None && drag_.kind != DragKind::Key) || settingsOpen_ || confirm_ >= 0 ||
        presets_.browserShown() || steps == 0.0f)
        return;
    menuLive_ = false;
    endTyping(true);
    const auto hit = hitTest(p);
    if (hit && (hit->popup || hit->kind == Kind::ModPill)) // a pill's list scrolls only up and down
        return;
    // As the wheel scrolls them, the other way round: a notch to the right moves on as a notch down does.
    if (!sourcesHidden() && kModPanel.contains(p)) {
        setScroll(ScrollArea::Sources, sourceScroll() + steps * 0.4f);
        closeGoneLists();
    } else { // the panels' (the SEQ view's roll, the FX view's row and graph)
        for (Panel *panel : panels_)
            if (panel->wheelAcross(p, steps))
                break;
    }
}

void View::applyPan(Point p) {
    // What was under the mouse as the button went down stays under it. Past either end, the row stops there, and
    // that end is where the mouse holds it from then on, so dragged back it follows at once.
    const float first = static_cast<float>(drag_.startValue);
    const float across = p.x - drag_.start.x, down = p.y - drag_.start.y;
    auto hold = [&](float wanted, float most) {
        const float to = std::clamp(wanted, 0.0f, std::max(most, 0.0f));
        if (to != wanted) {
            drag_.start = p;
            drag_.startValue = to;
        }
        return to;
    };
    switch (static_cast<PanArea>(drag_.widget.a)) {
    case PanArea::Sources:
        setScroll(ScrollArea::Sources,
                  hold(first - across / (kCardW + kCardGap), scrollLength(ScrollArea::Sources) - kVisibleCards));
        break;
    case PanArea::ModList:
        modListScroll_ = hold(first - down / kModListRowStep, modListEnd());
        break;
    case PanArea::Panel: // a panel's part, held at its ends as hold() holds the others
        if (drag_.panel)
            if (const std::optional<float> held = drag_.panel->pan(drag_.widget.b, first, across, down)) {
                drag_.start = p;
                drag_.startValue = *held;
            }
        break;
    }
}

void View::doubleClick(Point p, Modifiers mods) {
    const auto hit = hitTest(p);
    // It comes in place of a second press, so it goes with what the first one hit. Landing on something else, which
    // that press put there (closing the browser, a dialog or a list it was on, say) or which the mouse slipped onto,
    // it is dropped rather than edit what was never clicked; on another key, or drawing a step, it is still a press.
    // A press further away than a double-click allows wasn't its first one.
    constexpr float kDoubleClickSlop = 8.0f;
    const bool paired = std::hypot(p.x - pressPoint_.x, p.y - pressPoint_.y) <= kDoubleClickSlop;
    if (paired && !(hit && pressed_ && hit->same(*pressed_)) &&
        !(hit && (hit->kind == Kind::Key || shapes_.drawsStep(*hit, mods))))
        return;
    if (hit && std::any_of(panels_.begin(), panels_.end(),
                           [&](Panel *panel) { return panel->doubleClick(*hit, p, mods); }))
        return; // the panels' own (the presets', the SEQ view's, the routing graph's)
    // The SEQ tab double-clicked while the SEQ view shows (already as its first click came): its full size or its half.
    if (hit && hit->kind == Kind::ViewTab && hit->a == static_cast<int>(RoutingView::Seq) && seqTabPressedShown_ &&
        seq_.wide()) {
        seq_.toggleSize();
        return;
    }
    if (hit && (hit->kind == Kind::Key || hit->kind == Kind::Octave || hit->kind == Kind::UndoButton ||
                hit->kind == Kind::PresetStep ||
                hit->kind == Kind::SlotBypass || hit->kind == Kind::ListStep || stepperButton(*hit, p) != 0 ||
                shapes_.drawsStep(*hit, mods) ||
                std::any_of(panels_.begin(), panels_.end(), [&](const Panel *panel) { return panel->doublePress(*hit); }))) {
        // Windows turns a quick second click into a double-click instead of a press:
        // play it as a press so fast repeated notes, octave steps, undos, a list's arrows' steps, a stepper's - and +
        // and steps aren't dropped (and the panels' own: the FX view's switches, nodes and +s, the shape editors' grid
        // steppers and tools).
        mouseDown(p, mods);
        return;
    }
    if (hit && hit->kind == Kind::ModsAuxDepth) { // all of it again
        edit(hit->param, 1.0);
        return;
    }
    if (hit && hit->param >= 0 && onValue(*hit, p)) {
        startTyping(*hit);
        return;
    }
    if (hit && isRatioDisplay(*hit)) { // the digit under the mouse and those below it back to 1.00's: on the ones, 23.47 becomes 21.00
        const long span = kRatioPlaces[ratioDigit(p.x)] * 10;
        edit(hit->param, ratioNorm(ratioHundredths(value(hit->param)) / span * span + 100 % span));
        return;
    }
    if (hit && (hit->kind == Kind::MacroName || hit->kind == Kind::TempoBox)) {
        startTyping(*hit);
        return;
    }
    // The settings' sliders back to where they start: the theme's own color, the glow at half and the lines at 75 %.
    if (hit && (hit->kind == Kind::SettingsHue || hit->kind == Kind::SettingsGlow || hit->kind == Kind::SettingsLinks ||
                hit->kind == Kind::SettingsGrid)) {
        EditorSettings settings = controller_.editorSettings();
        if (hit->kind == Kind::SettingsHue)
            settings.accentHue = -1.0;
        else if (hit->kind == Kind::SettingsGlow)
            settings.glow = EditorSettings::kDefaultGlow;
        else if (hit->kind == Kind::SettingsLinks)
            settings.linkIntensity = EditorSettings::kDefaultLinkIntensity;
        else
            settings.gridIntensity = EditorSettings::kDefaultGridIntensity;
        controller_.setEditorSettings(settings);
        return;
    }
    if (hit && hit->kind == Kind::UtilityHandle) { // back where it starts
        edit(hit->param, table_[hit->param].def);
        return;
    }
    if (hit && hit->kind == Kind::AmpMic) { // back where it starts, in one undo step
        const UndoScope step(controller_);
        for (FxParam f : {FxParam::AmpPosition, FxParam::AmpDistance})
            edit(paramIndex(hit->a, f), table_[paramIndex(hit->a, f)].def);
        return;
    }
    if (hit && hit->kind == Kind::SearchField) { // the word there, up to the spaces either side
        field(static_cast<Field>(hit->a)).selectWord(p.x);
        caretTicks_ = 0;
        return;
    }
    if (hit && (hit->kind == Kind::EnvCurve || hit->kind == Kind::ModsCurve)) {
        edit(hit->param, 0.0); // straight again: 0 %, whatever the segment started as
        return;
    }
    if (hit && hit->kind == Kind::DynZone) { // its ratio back to where it starts
        edit(hit->param, mods_.resetValue(hit->param));
        return;
    }
    if (hit && hit->kind == Kind::DynGrip) { // its threshold back to where it starts
        const int threshold = mdThreshold(hit->a, hit->b);
        edit(threshold, mods_.resetValue(threshold));
        return;
    }
    if (hit && hit->kind == Kind::PeqPoint) {
        const PeqType type = peqType(value(peqTypeIndex(hit->a, hit->b)));
        if (isCut(type))
            edit(peqQIndex(hit->a, hit->b), normFromQ(std::numbers::sqrt2 * 0.5)); // no resonance
        else if (type == PeqType::Notch)
            edit(peqQIndex(hit->a, hit->b), mods_.resetValue(peqQIndex(hit->a, hit->b))); // its width as it starts
        else
            edit(peqGainIndex(hit->a, hit->b), 0.0); // flat again
        return;
    }
    if (!hit || hit->param < 0)
        return;
    switch (hit->kind) {
    case Kind::ModDot:
        mods_.removeSlot(hit->a, hit->b);
        break;
    case Kind::Knob:
    case Kind::SlotBar:
        edit(hit->param, mods_.resetValue(hit->param));
        break;
    default:
        break;
    }
}

// The wheel's targets besides the params (see wheelNotches()): the shape grid's axes (kParamCount was the preset
// browser's list's), and the tempo.
constexpr int kWheelGrid = kParamCount + 1; // and + 2, the grid's two axes
constexpr int kWheelTempo = kParamCount + 3;

int View::wheelNotches(int target, float steps) {
    if (target != wheelTarget_) {
        wheelTarget_ = target;
        wheelRest_ = 0.0f;
    }
    wheelRest_ += steps;
    const float notches = std::trunc(wheelRest_);
    wheelRest_ -= notches;
    return static_cast<int>(notches);
}

void View::wheel(Point p, float steps, Modifiers mods) {
    // Not while a drag goes on, which holds its gesture open and what it found under the mouse; a key held down still
    // lets the wheel turn things.
    if (drag_.kind != DragKind::None && drag_.kind != DragKind::Key)
        return;
    menuLive_ = false;
    endTyping(true);
    if (settingsOpen_ || confirm_ >= 0) // a dialog over everything: nothing behind it scrolls or turns
        return;
    // The preset browser's list and sidebar, the Save dialog's folders, and nothing under them.
    if (std::any_of(panels_.begin(), panels_.end(), [&](Panel *panel) { return panel->wheel(p, steps); }))
        return;
    routing_.keepRoutes(); // as in mouseDown(): scrolling can take a route down to 0 % too
    const auto hit = hitTest(p);
    if (hit && !hit->popup && std::any_of(panels_.begin(), panels_.end(), [&](Panel *panel) {
            return panel->wheelOn(*hit, p, steps, mods);
        }))
        return; // the panels' own (the SEQ view's, a route's label's mode, the Fixed switch)
    // Over a pill's list, its depth bars take the wheel and elsewhere it scrolls the rows, a row a notch, when they are
    // more than it shows: nothing under the list scrolls.
    if (hit && (hit->popup || hit->kind == Kind::ModPill) && hit->kind != Kind::SlotBar && hit->kind != Kind::ValueText) {
        if (hit->popup && modList_ && steps != 0.0f)
            modListScroll_ = std::clamp(modListFirst() - steps, 0.0f, modListEnd());
        return;
    }
    // What moves in whole steps (the grid, a stepped param, an operator's pitch, a note) moves by whole notches.
    const int target = hit && hit->kind == Kind::ShapeGrid  ? kWheelGrid + hit->a
                       : hit && hit->kind == Kind::TempoBox ? kWheelTempo
                                                             : (hit ? hit->param : -1);
    const int whole = wheelNotches(target, steps);
    if (hit && hit->kind == Kind::TempoBox) { // a BPM a notch, onto whole ones; with Shift, a tenth of one
        const double step = mods.shift ? 0.1 : 1.0;
        if (whole != 0) {
            const double from = controller_.ownTempo();
            const double on = whole > 0 ? std::floor(from / step + 1e-6) : std::ceil(from / step - 1e-6);
            const double bpm = clampTempo((on + whole) * step);
            if (bpm != from)
                controller_.setOwnTempo(bpm);
        }
        return;
    }
    if (hit && hit->kind == Kind::ShapeGrid) {
        if (whole != 0)
            shapes_.stepShapeGrid(hit->a, whole);
        return;
    }
    if (hit && hit->kind == Kind::SplitBand) { // a split's tabs: down a tab to the right, up one to the left
        if (steps != 0.0f) {
            const bool three = fx_.splitThree(hit->a);
            const int count = three ? 3 : 2;
            const int at = tabPosition(controller_.layout().splitBand[hit->a] == 2 && !three
                                           ? 0
                                           : controller_.layout().splitBand[hit->a],
                                       three);
            const int to = std::clamp(at + (steps < 0.0f ? 1 : -1), 0, count - 1);
            for (int tab = 0; tab < count; ++tab)
                if (tabPosition(tab, three) == to)
                    fx_.showSplitBand(hit->a, tab);
        }
        return;
    }
    if (hit && hit->kind == Kind::SplitBands) { // the BANDS switch: down for three, up for two
        if (steps != 0.0f)
            fxPanel_.setSplitBands(hit->a, steps < 0.0f ? 3 : 2);
        return;
    }
    if ((!hit || hit->param < 0) && !sourcesHidden() && kModPanel.contains(p) && steps != 0.0f) {
        setScroll(ScrollArea::Sources, sourceScroll() - steps * 0.4f);
        closeGoneLists(); // an LFO's list scrolled out with its card
        return;
    }
    // Over a panel's area, where what is under the mouse doesn't take it: the MODS view's rows (but not on a bar), the
    // FX view's row and graph, an operator's INPUTS rows (but not on a depth or a mode's button).
    if ((!hit || hit->param < 0) &&
        std::any_of(panels_.begin(), panels_.end(), [&](Panel *panel) { return panel->wheelBehind(p, steps); }))
        return;
    if (!hit || hit->param < 0)
        return;
    const ParamDef &def = table_[hit->param];
    if (isRatioDisplay(*hit)) { // a step of the digit under the mouse a notch, carrying
        if (whole != 0) {
            const UndoScope notches(controller_, true);
            edit(hit->param, ratioNorm(ratioHundredths(value(hit->param)) + whole * kRatioPlaces[ratioDigit(p.x)]));
        }
        return;
    }
    if (snapsToSteps(def.kind) && whole == 0) // short of a notch so far
        return;
    // A switch and the partial counts move as a click on them does: a switch on upward and off downward, with what
    // the symmetric switches bring along (the Fixed switch's, the inspector's), and a count to the next one offered.
    if (hit->kind == Kind::Toggle) {
        if (whole != 0 && (whole > 0) != (value(hit->param) >= 0.5))
            flipSwitch(hit->param);
        return;
    }
    if (isLfoClockChoice(*hit)) { // to the next mode along, as the wheel turns, from the one lit
        const int to = std::clamp(static_cast<int>(lfoClockOf(hit->a)) + whole, 0, lfoClockCount(hit->a) - 1);
        if (whole != 0)
            setLfoClock(hit->a, static_cast<LfoClock>(to));
        return;
    }
    if (const int split = splitOf(hit->param); split >= 0 && hit->param == paramIndex(split, SplitParam::Slope)) {
        const int slopes[] = {0, 1, kFilterSlopes - 1}; // 6, 12 and 24 dB, a notch a step (three bands from 12)
        const int first = fx_.splitThree(split) ? 1 : 0;
        int at = first;
        for (int k = first; k < 3; ++k)
            at = filterSlope(value(hit->param)) >= slopes[k] ? k : at;
        if (whole != 0)
            edit(hit->param, slopes[std::clamp(at + whole, first, 2)]);
        return;
    }
    if (isClipMode(hit->param)) { // to the next curve along its list, as the wheel turns
        const auto *first = std::begin(kClipModesOffered), *last = std::end(kClipModesOffered);
        const ClipMode now = clipMode(value(hit->param));
        const int at = static_cast<int>(std::find(first, last, now) - first);
        const int to = std::clamp(at + whole, 0, static_cast<int>(last - first) - 1);
        if (whole != 0 && kClipModesOffered[to] != now)
            edit(hit->param, static_cast<double>(kClipModesOffered[to]));
        return;
    }
    if (isLfoShape(hit->param)) { // to the next shape its list offers, as the wheel turns
        const auto *first = std::begin(kLfoShapesOffered), *last = std::end(kLfoShapesOffered);
        const auto now = static_cast<LfoShape>(std::lround(value(hit->param)));
        const auto at = std::find(first, last, now) - first; // the retired Random, not offered, steps from the start
        const int to = std::clamp(static_cast<int>(at == last - first ? 0 : at) + whole, 0, kLfoShapeItems - 1);
        if (whole != 0 && kLfoShapesOffered[to] != now)
            edit(hit->param, static_cast<double>(kLfoShapesOffered[to]));
        return;
    }
    if (isPartialCount(hit->param)) {
        const auto *first = std::begin(kPartialCounts), *last = std::end(kPartialCounts);
        const int now = partialCount(value(hit->param));
        const auto above = std::upper_bound(first, last, now) - first; // the first offered past it
        const auto below = std::lower_bound(first, last, now) - first; // the first offered from it on
        const int to = std::clamp(static_cast<int>(whole > 0 ? above - 1 + whole : below + whole), 0,
                                  static_cast<int>(last - first) - 1);
        if (whole != 0 && kPartialCounts[to] != now)
            edit(hit->param, kPartialCounts[to]);
        return;
    }
    const UndoScope notches(controller_, true); // a run of notches on one knob undoes at once
    if (snapsToSteps(def.kind))
        edit(hit->param, notchValue(hit->param, whole));
    else
        edit(hit->param, value(hit->param) + steps * (def.max - def.min) * (mods.shift ? 0.002 : 0.01));
}

double View::notchValue(int param, int whole) const {
    const ParamDef &def = table_[param];
    const double v = value(param);
    if (def.kind == ParamKind::VoiceCount) { // about 20 % per notch, at least one voice
        const double scaled = std::round(v * std::exp2(whole * 0.25));
        return whole > 0 ? std::max(v + 1.0, scaled) : std::min(v - 1.0, scaled);
    }
    if (def.kind == ParamKind::Transpose || def.kind == ParamKind::NotePitch) // to the next whole semitone, and on
        return whole > 0 ? std::floor(v + whole) : std::ceil(v + whole);
    return v + whole;
}

// Menu commands from here on clear all of a source's slots; those from 100 up to here remove one slot.
constexpr int kClearCommand = 100 + kAllSources * kSlots; // the removals first, the macros' and the note's too
// And from here on, each bypasses a slot or lets it go again.
constexpr int kBypassCommand = kClearCommand + kAllSources;
// And from here on, each takes a slot's aux away.
constexpr int kAuxClearCommand = kBypassCommand + kAllSources * kSlots;
// A source's polarity, in the menus of its card, its handle, and a macro's knob and name.
constexpr int kUnipolarCommand = 12;
constexpr int kBipolarCommand = 13;
// The routing panel's: Disconnect 3, Switch Off 4, Reset Bend 14 and a route's modes from 16 on (see
// RoutingPanel::kMenuDisconnect and on).
constexpr int kResetBendCommand = RoutingPanel::kMenuResetBend;
static_assert(RoutingPanel::kMenuDisconnect == 3 && RoutingPanel::kMenuSwitchOff == 4 && kResetBendCommand == 14);
// The FX view's: 6 to 8, 15, an amp's oversampling from 30 on and moving an effect to another band from 40 on (see
// FxPanel::kMenuDuplicate and on).
constexpr int kAmpOversamplingCommand = FxPanel::kAmpOversamplingCommand;
constexpr int kMoveBandCommand = FxPanel::kMoveBandCommand;
static_assert(FxPanel::kMenuDuplicate == 6 && FxPanel::kMenuRemove == 7 && FxPanel::kMenuReset == 8 &&
              FxPanel::kOttCommand == kResetBendCommand + 1);
// And from here on, each sets the mode of the route the menu was opened on (see RouteMode).
constexpr int kRouteModeCommand = RoutingPanel::kRouteModeCommand;
static_assert(kRouteModeCommand == kResetBendCommand + 2);
// The Save dialog's folder tree's, 44 to 47 (see PresetBrowser::kNewFolderCommand).
// Typing a modulation's aux depth, from its depth handle's or its aux chip's menu.
constexpr int kEditAuxDepthCommand = 48;
static_assert(PresetBrowser::kNewFolderCommand > kMoveBandCommand + 3 &&
              PresetBrowser::kOpenFolderCommand == PresetBrowser::kNewFolderCommand + 1 &&
              PresetBrowser::kRenameFolderCommand == PresetBrowser::kNewFolderCommand + 2 &&
              PresetBrowser::kDeleteFolderCommand == PresetBrowser::kNewFolderCommand + 3 &&
              kEditAuxDepthCommand > PresetBrowser::kDeleteFolderCommand &&
              kEditAuxDepthCommand < SeqPanel::kMenuCommands); // under the SEQ view's
static_assert(kRouteModeCommand + static_cast<int>(RouteMode::Count) <= kAmpOversamplingCommand &&
              kAmpOversamplingCommand + kAmpOversamplings <= kMoveBandCommand && kMoveBandCommand + 3 < 100);

std::vector<MenuItem> View::contextMenu(Point p) {
    endTyping(true);
    std::vector<MenuItem> items;
    menuLive_ = true;
    menuParam_ = -1;
    menuSource_ = -1;
    menuValue_.reset();
    menuAuxDepth_.reset();
    const auto hit = hitTest(p);
    // The panels' own (a folder of the preset browser's or the Save dialog's tree): each forgets its last menu, and
    // the first that owns what is hit fills this one.
    bool taken = false;
    for (Panel *panel : panels_)
        taken = panel->contextMenu(taken ? std::optional<Widget>() : hit, p, items) || taken;
    if (taken || !hit)
        return items;
    // Every source can swing its targets one way or both, from the menu of its card or handle (and a macro's knob, the
    // mod wheel and the note source's or velocity's display), the one in force checked.
    const int polaritySource = hit->kind == Kind::SourceGrip || hit->kind == Kind::SourceHandle ||
                                       hit->kind == Kind::MacroHandle || hit->kind == Kind::NoteDisplay
                                   ? hit->a
                               : hit->kind == Kind::MacroName              ? kMaxSources + hit->a
                               : hit->kind == Kind::ModWheel               ? kWheelSource
                               : hit->kind == Kind::Knob && macroOf(hit->param) >= 0 ? kMaxSources + macroOf(hit->param)
                                                                                     : -1;
    auto addPolarity = [&] {
        menuSource_ = polaritySource;
        const bool bipolar = mods_.sourceBipolar(polaritySource);
        items.push_back({kUnipolarCommand, tr(Str::MenuUnipolar), !bipolar});
        items.push_back({kBipolarCommand, tr(Str::MenuBipolar), bipolar});
    };
    if (hit->kind == Kind::SourceGrip || hit->kind == Kind::SourceHandle) {
        if (!usedSlots(hit->a).empty())
            items.push_back({kClearCommand + hit->a, trf(Str::MenuClearTargets, {mods_.sourceName(hit->a)})});
        if (sources().size() < static_cast<size_t>(kMaxSources))
            items.push_back({9, tr(Str::MenuDuplicate)});
        if (!items.empty())
            items.push_back({0, ""});
        addPolarity();
        items.push_back({0, ""});
        items.push_back({10, tr(Str::MenuRemove)});
        return items;
    }
    if (hit->kind == Kind::MacroHandle || hit->kind == Kind::MacroName || hit->kind == Kind::NoteDisplay) {
        if (!usedSlots(polaritySource).empty()) {
            items.push_back(
                {kClearCommand + polaritySource, trf(Str::MenuClearTargets, {mods_.sourceName(polaritySource)})});
            items.push_back({0, ""});
        }
        addPolarity();
        return items;
    }
    if (hit->kind == Kind::SlotBar || hit->kind == Kind::SlotRemove || hit->kind == Kind::ModDot ||
        hit->kind == Kind::ValueText || hit->kind == Kind::SlotBypass || hit->kind == Kind::ModsSource ||
        hit->kind == Kind::ModsTarget || hit->kind == Kind::ModsScale || hit->kind == Kind::ModsCurve ||
        hit->kind == Kind::ModsAux || hit->kind == Kind::ModsAuxClear || hit->kind == Kind::ModsAuxDepth ||
        hit->kind == Kind::ModListRow) {
        if (hit->b < 0) // an empty modulation: nothing to bypass, nor a depth to type
            return items;
        // The depth types where it shows: beside its bar, or under a knob's dot.
        if (hit->kind == Kind::SlotBar || hit->kind == Kind::ModDot || hit->kind == Kind::ValueText) {
            menuValue_ = *hit;
            if (hit->kind == Kind::SlotBar) {
                menuValue_->kind = Kind::ValueText;
                menuValue_->rect = {hit->rect.right() + 2.0f, hit->rect.y, 38.0f, hit->rect.h};
            }
            items.push_back({11, tr(Str::MenuEditDepth)});
        }
        items.push_back({kBypassCommand + hit->a * kSlots + hit->b, bypassLabel(hit->a, hit->b)});
        if (const int aux = slotAux([this](int index) { return value(index); }, hit->a, hit->b); aux >= 0) {
            // The aux depth types where it shows: on its chip, or under a knob's dot as the depth does.
            if (hit->kind == Kind::ModDot || hit->kind == Kind::ModsAuxDepth) {
                menuAuxDepth_ = *hit;
                menuAuxDepth_->param = slotAuxDepthIndex(hit->a, hit->b);
                items.push_back({kEditAuxDepthCommand, tr(Str::MenuEditAuxDepth)});
            }
            items.push_back(
                {kAuxClearCommand + hit->a * kSlots + hit->b, trf(Str::MenuRemoveAux, {mods_.sourceName(aux)})});
        }
        items.push_back({0, ""});
        items.push_back({100 + hit->a * kSlots + hit->b, trf(Str::MenuRemoveModulation, {mods_.sourceName(hit->a)})});
        return items;
    }
    if (hit->param < 0)
        return items;

    menuParam_ = hit->param;
    if (hit->kind == Kind::Knob || hit->kind == Kind::WireLabel || hit->kind == Kind::OutLabel ||
        hit->kind == Kind::GridCell) {
        menuValue_ = *hit;
        items.push_back({11, tr(Str::MenuEditValue)});
    }
    items.push_back({1, tr(Str::MenuResetDefault)});
    for (Panel *panel : panels_) // what a panel's own adds (a route's label's or cell's modes and Disconnect)
        panel->paramMenu(*hit, items);
    std::vector<MenuItem> removals, bypasses;
    for (const Assignment &m : assignments()) {
        if (m.target != hit->param)
            continue;
        bypasses.push_back({kBypassCommand + m.source * kSlots + m.slot, bypassLabel(m.source, m.slot)});
        removals.push_back(
            {100 + m.source * kSlots + m.slot, trf(Str::MenuRemoveModulation, {mods_.sourceName(m.source)})});
    }
    if (!removals.empty()) {
        items.push_back({0, ""});
        items.insert(items.end(), bypasses.begin(), bypasses.end());
        items.push_back({0, ""});
        items.insert(items.end(), removals.begin(), removals.end());
        if (removals.size() > 1)
            items.push_back({2, tr(Str::MenuRemoveAll)});
    }
    if (polaritySource >= 0) { // a macro's knob, or the mod wheel
        items.push_back({0, ""});
        addPolarity();
    }
    return items;
}

std::string View::bypassLabel(int source, int slot) const {
    return trf(mods_.slotBypassed(source, slot) ? Str::MenuResume : Str::MenuBypass, {mods_.sourceName(source)});
}

void View::menuCommand(int command) {
    if (!menuLive_) // what the menu was opened on may have changed or gone since
        return;
    // The SEQ view's roll's commands (see SeqPanel::contextMenu()), after the others under 100.
    static_assert(kMoveBandCommand + 3 < SeqPanel::kMenuCommands &&
                  SeqPanel::kMenuCommands + SeqPanel::kMenuCount <= 100);
    for (Panel *panel : panels_) // the panels' own (see contextMenu())
        if (panel->menuCommand(command))
            return;
    if (command == 1 && menuParam_ >= 0) {
        edit(menuParam_, mods_.resetValue(menuParam_));
    } else if (command == 2 && menuParam_ >= 0) {
        const UndoScope step(controller_);
        for (const Assignment &m : assignments())
            if (m.target == menuParam_)
                mods_.removeSlot(m.source, m.slot);
    } else if (command == 9 && menuSource_ >= 0) {
        addSource(mods_.sourceTypeOf(menuSource_), menuSource_);
    } else if (command == 10 && menuSource_ >= 0) {
        removeSource(menuSource_);
    } else if (command == 11 && menuValue_) {
        startTyping(*menuValue_);
    } else if (command == kEditAuxDepthCommand && menuAuxDepth_) {
        startTyping(*menuAuxDepth_);
    } else if ((command == kUnipolarCommand || command == kBipolarCommand) && menuSource_ >= 0) {
        mods_.setSourcePolarity(menuSource_, command == kBipolarCommand);
    } else if (command >= 100 && command < kClearCommand) {
        mods_.removeSlot((command - 100) / kSlots, (command - 100) % kSlots);
    } else if (command >= kClearCommand && command < kClearCommand + kAllSources) {
        const UndoScope step(controller_);
        for (int slot : usedSlots(command - kClearCommand))
            mods_.removeSlot(command - kClearCommand, slot);
    } else if (command >= kBypassCommand && command < kBypassCommand + kAllSources * kSlots) {
        mods_.toggleBypass((command - kBypassCommand) / kSlots, (command - kBypassCommand) % kSlots);
    } else if (command >= kAuxClearCommand && command < kAuxClearCommand + kAllSources * kSlots) {
        mods_.setAux((command - kAuxClearCommand) / kSlots, (command - kAuxClearCommand) % kSlots, -1);
    }
}

void View::undo() {
    if (drag_.kind == DragKind::None && !typing_) {
        for (Panel *panel : panels_) // what their menus were opened on may change, or go
            panel->closePopups();
        for (Panel *panel : panels_) // so is the place in the line the list of effects was opened at
            panel->undoing();
        listMenu_ = -1;
        peqMenu_ = -1;
        if (const UndoStep *step = controller_.undo())
            for (Panel *panel : panels_)
                panel->stepApplied(*step, false);
        menuLive_ = false;
    }
}

void View::redo() {
    if (drag_.kind == DragKind::None && !typing_) {
        for (Panel *panel : panels_) // what their menus were opened on may change, or go
            panel->closePopups();
        for (Panel *panel : panels_) // so is the place in the line the list of effects was opened at
            panel->undoing();
        listMenu_ = -1;
        peqMenu_ = -1;
        if (const UndoStep *step = controller_.redo())
            for (Panel *panel : panels_)
                panel->stepApplied(*step, true);
        menuLive_ = false;
    }
}

void View::startTyping(const Widget &w) {
    if (w.kind == Kind::MacroName) { // its name, all selected
        const std::string name = macroName(controller_.layout(), w.a);
        typing_ = Typing{w, name, name.size(), true};
        caretTicks_ = 0;
        return;
    }
    if (w.kind == Kind::TempoBox) { // the number, without its unit
        const std::string bpm = tempoText(controller_.ownTempo());
        typing_ = Typing{w, bpm, bpm.size(), true};
        caretTicks_ = 0;
        return;
    }
    char text[64];
    formatParam(w.param, value(w.param), text, sizeof(text));
    typing_ = Typing{w, text, std::string_view(text).size(), true};
    caretTicks_ = 0;
}

void View::typeChar(char32_t c) {
    if (!typing_) { // into the text field that has the keyboard, if any
        const auto f = fieldTyping();
        if (f && !(*f == Field::Browser && confirm_ >= 0) && c >= 0x20 && c <= 0x7E &&
            field(*f).insert(std::string(1, static_cast<char>(c))))
            fieldChanged(*f);
        return;
    }
    if (c < 0x20 || c > 0x7E)
        return;
    Typing &t = *typing_;
    if (t.selected) {
        t.text.clear();
        t.caret = 0;
        t.selected = false;
    }
    if (t.text.size() < 40)
        t.text.insert(t.caret++, 1, static_cast<char>(c));
    caretTicks_ = 0;
}

void View::typeKey(Key key, bool shift) {
    if (!typing_) { // the text field that has the keyboard edits its text, and its owner takes the rest
        const auto f = fieldTyping();
        if (!f || (*f == Field::Browser && confirm_ >= 0))
            return;
        caretTicks_ = 0;
        if (field(*f).key(key, shift))
            fieldChanged(*f);
        for (Panel *panel : panels_) // its panel's (the preset browser's search, the Save dialog's and the name form's
            panel->fieldKey(*f, key); // fields, the MODS view's filter and its target menu's search)
        return;
    }
    Typing &t = *typing_;
    caretTicks_ = 0;
    // The caret steps over whole characters: a value's text may hold one past ASCII in UTF-8, as Mult's ×.
    auto continues = [&](size_t at) {
        return at < t.text.size() && (static_cast<unsigned char>(t.text[at]) & 0xC0) == 0x80;
    };
    auto before = [&](size_t at) {
        do
            --at;
        while (at > 0 && continues(at));
        return at;
    };
    auto after = [&](size_t at) {
        do
            ++at;
        while (continues(at));
        return at;
    };
    switch (key) {
    case Key::Enter:
        endTyping(true);
        return;
    case Key::Escape:
        endTyping(false);
        return;
    case Key::SelectAll:
        t.selected = true;
        t.caret = t.text.size();
        return;
    case Key::Backspace:
    case Key::Delete:
        if (t.selected) {
            t.text.clear();
            t.caret = 0;
        } else if (key == Key::Backspace && t.caret > 0) {
            const size_t from = before(t.caret);
            t.text.erase(from, t.caret - from);
            t.caret = from;
        } else if (key == Key::Delete && t.caret < t.text.size()) {
            t.text.erase(t.caret, after(t.caret) - t.caret);
        }
        break;
    case Key::Left: // from a selection, to its start
        t.caret = t.selected || t.caret == 0 ? 0 : before(t.caret);
        break;
    case Key::Right:
        t.caret = t.selected || t.caret >= t.text.size() ? t.text.size() : after(t.caret);
        break;
    case Key::WordLeft: // from the caret, the selection going
        t.caret = TextField::wordBefore(t.text, t.caret);
        break;
    case Key::WordRight:
        t.caret = TextField::wordAfter(t.text, t.caret);
        break;
    case Key::Home:
        t.caret = 0;
        break;
    case Key::End:
        t.caret = t.text.size();
        break;
    case Key::Up:
    case Key::Down:
    case Key::Tab:
    case Key::BackTab:
        return;
    }
    t.selected = false;
}

std::string View::copyText(bool cut) {
    if (typing_) { // all of a typed value, while all of it is selected
        Typing &t = *typing_;
        if (!t.selected)
            return {};
        std::string text = t.text;
        if (cut) {
            t.text.clear();
            t.caret = 0;
            t.selected = false;
        }
        return text;
    }
    const auto f = fieldTyping();
    if (!f || (*f == Field::Browser && confirm_ >= 0))
        return {};
    TextField &t = field(*f);
    const auto [from, to] = t.selection();
    std::string text = t.text.substr(from, to - from);
    if (cut && from != to) {
        t.erase(from, to);
        fieldChanged(*f);
    }
    return text;
}

void View::pasteText(std::string_view text) {
    if (typing_) {
        for (char ch : text)
            typeChar(ch == '\n' || ch == '\r' || ch == '\t' ? U' ' : static_cast<char32_t>(static_cast<unsigned char>(ch)));
    } else if (const auto f = fieldTyping(); f && !(*f == Field::Browser && confirm_ >= 0)) {
        if (field(*f).insert(text))
            fieldChanged(*f);
    }
}

void View::endTyping(bool commit) {
    if (!typing_)
        return;
    const Typing t = std::move(*typing_);
    typing_.reset();
    if (t.widget.kind == Kind::MacroName) { // a new name, an undo step of its own; emptied, its number again
        const size_t start = t.text.find_first_not_of(' ');
        std::string name = start == std::string::npos ? std::string()
                                                      : t.text.substr(start, t.text.find_last_not_of(' ') - start + 1);
        name = name.substr(0, 24);
        if (name == trf(Str::MacroDefaultName, {std::to_string(t.widget.a + 1)}))
            name.clear();
        UiLayout &layout = controller_.layout();
        if (commit && name != layout.macroNames[t.widget.a]) {
            const UndoScope step(controller_);
            layout.macroNames[t.widget.a] = name;
            controller_.layoutChanged();
        }
        return;
    }
    if (t.widget.kind == Kind::TempoBox) { // a number of BPM ("bpm" after it allowed), held to the range
        const char *text = t.text.c_str();
        char *end = nullptr;
        const double bpm = std::strtod(text, &end);
        std::string rest = end;
        std::erase(rest, ' ');
        for (char &ch : rest)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!commit || t.text.find_first_not_of(' ') == std::string::npos ||
            t.text == tempoText(controller_.ownTempo()))
            return;
        if (end == text || !std::isfinite(bpm) || !(rest.empty() || rest == "bpm")) {
            flash(trf(Str::MsgBadValue, {t.text, tr(Str::TempoName)}));
            return;
        }
        if (clampTempo(bpm) != controller_.ownTempo())
            controller_.setOwnTempo(clampTempo(bpm));
        return;
    }
    // Spaces around it don't count. Left as it was shown, or emptied, the value stays as it is,
    // rather than being rounded to what the text says.
    const size_t first = t.text.find_first_not_of(' ');
    char shown[64];
    formatParam(t.widget.param, value(t.widget.param), shown, sizeof(shown));
    if (!commit || first == std::string::npos || t.text == shown)
        return;
    const std::string text = t.text.substr(first, t.text.find_last_not_of(' ') - first + 1);
    double v = 0.0;
    if (parseParam(t.widget.param, text.c_str(), &v))
        edit(t.widget.param, v);
    else
        flash(trf(Str::MsgBadValue, {text, mods_.paramName(t.widget.param)}));
}

// The panels', while nothing is typed, no dialog is open and no drag goes on: a shape editor's points picked, or one
// under the mouse, take Delete and Ctrl+A first (see ShapeEditor::key()), then the SEQ view's.
bool View::seqKey(Key key, bool shift) {
    if (typing() || dialogOpen() || drag_.kind != DragKind::None)
        return false;
    return std::any_of(panels_.begin(), panels_.end(), [&](Panel *panel) { return panel->key(key, shift); });
}

CursorKind View::cursor() const {
    if (drag_.kind == DragKind::Pan)
        return CursorKind::Move;
    if (drag_.kind == DragKind::ModSource)
        return CursorKind::Hand;
    if (drag_.kind == DragKind::Value &&
        (drag_.widget.kind == Kind::PeqPoint || drag_.widget.kind == Kind::DynGrip)) // Alt-dragged for Q or ratio
        return CursorKind::Vertical;
    const Widget *w = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (!w)
        return CursorKind::Arrow;
    for (const Panel *panel : panels_) // the panels' own (the SEQ view's roll, the FX view's cards and nodes, the
                                       // routing graph's nodes and wires)
        if (const std::optional<CursorKind> own = panel->cursor(*w))
            return *own;
    // A stepper: a hand over its - and +, which click; up and down over its number, which drags; elsewhere on it (round
    // its field, its label) the arrow. Dragged, up and down wherever the mouse goes.
    if (isStepper(*w) && drag_.kind == DragKind::None)
        return stepperButton(*w, hoverPoint_) != 0 ? CursorKind::Hand
               : w->well.contains(hoverPoint_)     ? CursorKind::Vertical
                                                   : CursorKind::Arrow;
    switch (w->kind) {
    case Kind::DynZone:
    case Kind::Knob:
    case Kind::ModDot:
    case Kind::EnvCurve:
    case Kind::TempoBox:
        return CursorKind::Vertical;
    case Kind::SettingsBackdrop:
    case Kind::ConfirmBackdrop:
    case Kind::BrowserBackdrop:
    case Kind::InfoCard:
    case Kind::SaveBackdrop:
    case Kind::NameMenu:
    case Kind::TempoHost: // the host's tempo, which takes no edits
    // An open list's own panel and what is around it, where a click picks nothing (though around it, it closes it),
    // and a pill's list's rows, which only take a source's handle dropped on them.
    case Kind::SourceMenu:
    case Kind::ListMenu:
    case Kind::PeqTypeMenu:
    case Kind::ModList:
    case Kind::ModListRow:
        return CursorKind::Arrow;
    case Kind::ListItem: // a band-pass's 6 and 18 dB slopes can't be picked
        return listItemFits(w->param, w->b) ? CursorKind::Hand : CursorKind::Arrow;
    case Kind::SearchField: // the MODS view's filter a button until it opens (see ModsPanel)
        return CursorKind::Text;
    case Kind::SlotBar:
    case Kind::ModsScale:
    case Kind::DynGrip:
        return CursorKind::Horizontal;
    case Kind::ModsCurve:
    case Kind::ModsAuxDepth:
        return CursorKind::Vertical;
    case Kind::EnvPoint:
    case Kind::FilterPoint:
    case Kind::PeqPoint:
    case Kind::AmpMic:
    case Kind::UtilityHandle:
    case Kind::SourceGrip:
        return CursorKind::Move;
    default:
        return CursorKind::Hand;
    }
}

} // namespace fmvibe::gui
