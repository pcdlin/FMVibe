#include "view.h"

#include "fxgraph.h"
#include "icons.h"
#include "layout.h"
#include "routing.h"
#include "theme.h"
#include "../effects.h"
#include "../filter.h"
#include "../uistrings.h"
#include "../wavetable.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <numbers>
#include <numeric>
#include <string_view>

namespace fmvibe::gui {

// The look, the layout, the icons and the routing graph's model are in theme.h, layout.h, icons.h and routing.h,
// in namespace detail. dialogs.cpp, routing.cpp and shapes.cpp hold the View's methods for the dialogs, the
// routing graph and the drawn shapes.
using namespace detail;

namespace detail {
// The effect cards' titles, by FxType from the shaper on.
constexpr Str kFxTitles[] = {Str::FxTitleShaper, Str::FxTitleDelay, Str::FxTitleDjEq, Str::FxTitleParamEq,
                             Str::FxTitleFilter, Str::FxTitleCompressor, Str::FxTitleMultiComp, Str::FxTitleReverb,
                             Str::FxTitlePhaser, Str::FxTitleFlanger, Str::FxTitleFreqShifter, Str::FxTitleDisperser,
                             Str::FxTitleWidener, Str::FxTitleMultiDyn, Str::FxTitleAmp, Str::FxTitleUtility};
static_assert(std::size(kFxTitles) == static_cast<size_t>(FxType::Count) - 1);
const char *fxTitle(FxType type) {
    return type == FxType::None ? "" : tr(kFxTitles[static_cast<int>(type) - 1]);
}
} // namespace detail

namespace {
constexpr Str kMcBandNames[kMcBands] = {Str::BandButtonLow, Str::BandButtonMid, Str::BandButtonHigh};
constexpr Str kViewNames[] = {Str::ViewRouting, Str::ViewMatrix, Str::ViewFx, Str::ViewMods};
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
    if (flare > 0.0f && r.x < kRouting.x + 1.0f)
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

// The type param that goes with a filter slope param, or -1 for other params.
int slopeTypeParam(int param) {
    if (fxFieldOf(param) == FxParam::FilterSlope)
        return paramIndex(fxSlotOf(param), FxParam::FilterType);
    for (int filter = 0; filter < kFilters; ++filter)
        if (paramIndex(filter, FilterParam::Slope) == param)
            return paramIndex(filter, FilterParam::Type);
    return -1;
}

bool isFilterSlope(int param) {
    return slopeTypeParam(param) >= 0;
}

// A split's slope: 6, 12 or 24 dB (18, which a filter has, it doesn't).
bool isSplitSlope(int param) {
    const int split = splitOf(param);
    return split >= 0 && param == paramIndex(split, SplitParam::Slope);
}

// Any slope's list: a filter's, the filter effect's or a split's.
bool isSlopeList(int param) {
    return isFilterSlope(param) || isSplitSlope(param);
}

bool isLfoShape(int param) {
    return sourceFieldOf(param) == SourceParam::Shape;
}

// An LFO shape's icon, as its list shows it, in a box about 16 x 10.
void lfoShapeIcon(Canvas &c, Rect r, LfoShape shape, Color color) {
    const std::vector<Point> points = shapeIcon(shape, r);
    c.polyline(points.data(), points.size(), color, 1.5f);
    if (shape == LfoShape::Custom)
        customShapeDots(c, r, color);
}

bool isMidiInput(int param) {
    return sourceFieldOf(param) == SourceParam::MidiInput;
}

// A choice that opens a list under its button (an LFO's shape, a MIDI source's input, a shaper's curve, the reverb's
// algorithm, a filter's slope): which values it has, and their names.
int listCount(int param) {
    return routeModeFrom(param) >= 0                  ? static_cast<int>(RouteMode::Count)
           : isSlopeList(param)                       ? kFilterSlopes
           : isLfoShape(param)                        ? static_cast<int>(LfoShape::Count)
           : isMidiInput(param)                       ? static_cast<int>(MidiInput::Count)
           : fxFieldOf(param) == FxParam::ReverbType ? static_cast<int>(ReverbType::Count)
           : fxFieldOf(param) == FxParam::AmpModel   ? static_cast<int>(AmpModel::Count)
           : fxFieldOf(param) == FxParam::AmpCab     ? static_cast<int>(AmpCab::Count)
                                                      : static_cast<int>(ClipMode::Count);
}
std::string listItemName(int param, int value) {
    if (routeModeFrom(param) >= 0)
        return routeModeName(static_cast<RouteMode>(value));
    if (isSlopeList(param))
        return std::to_string(slopeDb(value)) + " dB";
    if (isLfoShape(param))
        return lfoShapeName(static_cast<LfoShape>(value));
    if (isMidiInput(param))
        return midiInputName(static_cast<MidiInput>(value));
    if (fxFieldOf(param) == FxParam::AmpModel)
        return ampModelName(static_cast<AmpModel>(value));
    if (fxFieldOf(param) == FxParam::AmpCab)
        return ampCabName(static_cast<AmpCab>(value));
    return fxFieldOf(param) == FxParam::ReverbType ? reverbTypeName(static_cast<ReverbType>(value))
                                                    : clipModeName(static_cast<ClipMode>(value));
}

const char *const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string noteName(int note) {
    return std::string(kNoteNames[note % 12]) + std::to_string(note / 12 - 1);
}

std::string percent(double value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f%%", value * 100.0);
    return text;
}

bool isLfoSync(int param) {
    return sourceFieldOf(param) == SourceParam::Sync;
}

// "LP", or "LP 24" at a slope other than 12 dB per octave, as it plays.
std::string filterLabel(FilterType type, double slope) {
    const int played = playedSlope(type, filterSlope(slope));
    return std::string(filterTypeShortName(type)) +
           (hasSlope(type) && played != kDefaultSlope ? " " + std::to_string(slopeDb(played)) : "");
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

// A point on segment 0 (attack, p0 to p1), 1 (decay, ph to p2) or 2 (release,
// p2 to p4), at time t (0..1) along it, bent by its curve as the DSP bends it.
Point envSegmentPoint(const EnvShape &s, int segment, double t, double curve, double sustain) {
    const double f = envCurve(t, curve);
    const double level = segment == 0 ? f : segment == 1 ? sustain + (1.0 - sustain) * (1.0 - f) : sustain * (1.0 - f);
    const Point from = segment == 0 ? s.p0 : segment == 1 ? s.ph : s.p2;
    const Point to = segment == 0 ? s.p1 : segment == 1 ? s.p2 : s.p4;
    return {from.x + static_cast<float>(t) * (to.x - from.x), s.bottom - static_cast<float>(level) * (s.bottom - s.top)};
}

// --- Filter response display: x is the cutoff knob's log scale, y is dB.

constexpr float kResponseMinDb = -36.0f;
constexpr float kResponseMaxDb = 18.0f;

std::vector<int> filterPointParams(int a, int b);

Point filterHandle(Rect r, double cutoff, double resonance) {
    return {r.x + 10.0f + static_cast<float>(cutoff) * (r.w - 20.0f),
            r.bottom() - 10.0f - static_cast<float>(resonance) * (r.h - 20.0f)};
}

// A name in capitals, as card titles and the dashed cards' words are.
std::string upperCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
}

// --- The plus before the name on + EFFECT and + SOURCE.

constexpr float kAddPlusArm = 3.5f;                    // half its width
constexpr float kAddPlusW = 2.0f * kAddPlusArm + 5.0f; // with the gap before the name
// How far the name goes up: centering its line, descenders and all, leaves the capitals under the plus's middle.
constexpr float kAddNameLift = 0.6f;

// The plus, its left end at `left`.
void paintAddPlus(Canvas &c, Point left, Color color) {
    const Point m{left.x + kAddPlusArm, left.y};
    c.line({m.x - kAddPlusArm, m.y}, {m.x + kAddPlusArm, m.y}, color, 1.5f);
    c.line({m.x, m.y - kAddPlusArm}, {m.x, m.y + kAddPlusArm}, color, 1.5f);
}

// A menu's row under the mouse, and the one it has picked, as every list and menu marks them: in neutral tones, the
// row's icon alone taking a color.
void paintMenuRow(Canvas &c, Rect r, bool hovered, bool selected) {
    if (hovered || selected)
        c.fillRect(r, hovered ? kTrack : kTrack.alpha(0.5f), kRadiusControl);
}

// + EFFECT and + SOURCE: a plus and the name, then a chevron for the list it opens. Neutral at rest, in the theme's
// accent under the mouse and while its list is open; dimmed while every place is taken (`full`).
void paintMenuButton(Canvas &c, Rect r, const char *name, bool hovered, bool open, bool full) {
    const bool lit = (hovered || open) && !full;
    const Color ink = full ? kFaint : (lit ? kText : kDim);
    c.fillRect(r, lit ? kAccent.alpha(0.18f) : kRaised, kRadiusControl);
    c.strokeRect(r, lit ? kAccent.alpha(0.9f) : kBorder, 1.0f, kRadiusControl);
    paintAddPlus(c, {r.x + 10.0f, r.center().y}, ink);
    c.text({r.x + 10.0f + kAddPlusW, r.y - kAddNameLift, r.w - 30.0f - kAddPlusW, r.h}, name, kTextLabel, ink,
           Align::Left, true);
    const Point m{r.right() - 13.0f, r.center().y};
    const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
    c.polyline(chevron, 3, ink, 1.5f);
}

} // namespace

float View::heightFor(const UiLayout &layout) {
    return keyboardPanel(keyboardTop(layout.sourcesCollapsed), layout.keysCollapsed).bottom() + 16.0f;
}

View::View(Controller &controller) : controller_(controller), table_(paramTable()) {
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

View::DerivedScope::DerivedScope(const View &view) : view_(view) {
    if (view_.derivedDepth_++ == 0) {
        view_.derived_ = Derived{};
        view_.fxDerived_ = FxDerived{};
        view_.fxRowDerived_.reset();
    }
}

View::DerivedScope::~DerivedScope() {
    if (--view_.derivedDepth_ == 0) {
        view_.derived_ = Derived{};
        view_.fxDerived_ = FxDerived{};
        view_.fxRowDerived_.reset();
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

double View::value(int index) const {
    return controller_.value(index);
}

bool View::isNoise(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Noise);
}

double View::norm(int index) const {
    const ParamDef &def = table_[index];
    if (def.kind == ParamKind::VoiceCount) // 1..max on a log scale
        return std::log(value(index)) / std::log(def.max);
    return (value(index) - def.min) / (def.max - def.min);
}

void View::edit(int index, double v) {
    if (!std::isfinite(v)) // std::clamp would pass a NaN on
        return;
    const ParamDef &def = table_[index];
    controller_.beginEdit(index);
    // A node switched on comes back into the graph clear of the others, in the same undo step.
    int switchedOn = -1;
    if (v >= 0.5 && value(index) < 0.5)
        for (int node = 0; node < kNodes; ++node)
            if (enabledParam(node) == index)
                switchedOn = node;
    controller_.setValue(index, std::clamp(v, def.min, def.max));
    if (switchedOn >= 0)
        placeSwitchedOn(switchedOn);
    // A route switched on is kept in the same undo step, which takes it out again with its value; kept only by the
    // next poll, after the step, undo would leave its wire at 0 %.
    if (value(index) > 0.0 && isRouteParam(index))
        keepRoutes();
    controller_.endEdit(index);
}

std::vector<View::Assignment> View::assignments() const {
    std::vector<Assignment> list;
    std::vector<int> all = sources();
    for (int macro = 0; macro < kMacros; ++macro) // the macros count as sources after the places, then the note's
        all.push_back(kMaxSources + macro);         // and the mod wheel's
    all.push_back(kNoteSource);
    all.push_back(kWheelSource);
    all.push_back(kVelocitySource);
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

std::vector<int> View::usedSlots(int source) const {
    const bool keeps = source >= 0 && source < kAllSources;
    if (keeps && derived_.slots[source]) // only kept while a scope goes on
        return *derived_.slots[source];
    std::vector<int> slots;
    for (int slot = 0; slot < kSlots; ++slot)
        if (slotTarget(value(slotTargetIndex(source, slot))) >= 0)
            slots.push_back(slot);
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

void View::assign(int source, int target) {
    const UndoScope step(controller_); // with the route it may keep
    int free = -1;
    for (int slot = kSlots - 1; slot >= 0; --slot) {
        const int current = slotTarget(value(slotTargetIndex(source, slot)));
        if (current == target) {
            flash(trf(Str::MsgAlreadyModulates, {sourceName(source), paramName(target)}));
            return;
        }
        if (current < 0)
            free = slot;
    }
    if (free < 0) {
        flash(trf(Str::MsgSlotsFull, {sourceName(source), std::to_string(kSlots)}));
        return;
    }
    const double amount = 0.3; // a moderate depth, always upward to begin with, swinging as its source does
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, free), slotTargetValue(target)},
                                                {slotAmountIndex(source, free), amount},
                                                {slotScaleIndex(source, free), 1.0},
                                                {slotCurveIndex(source, free), 0.0},
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

void View::removeSlot(int source, int slot) {
    if (const int row = emptyModOf(slot); row >= 0) {
        removeEmptyMod(row);
        return;
    }
    const UndoScope step(controller_); // with what goes along
    std::vector<std::pair<int, double>> changes{{slotTargetIndex(source, slot), 0.0},
                                                {slotAmountIndex(source, slot), 0.0},
                                                {slotScaleIndex(source, slot), 1.0},
                                                {slotCurveIndex(source, slot), 0.0},
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

bool View::slotBypassed(int source, int slot) const {
    return slot >= 0 && value(slotBypassIndex(source, slot)) >= 0.5;
}

void View::toggleBypass(int source, int slot) {
    if (slot >= 0) // an empty modulation has nothing to bypass
        edit(slotBypassIndex(source, slot), slotBypassed(source, slot) ? 0.0 : 1.0);
}

void View::setSourcePolarity(int source, bool bipolar) {
    const Polarity polarity = polarityOf(source, sourceTypeOf(source));
    edit(polarity.index, bipolar ? polarity.bipolar : 1.0 - polarity.bipolar);
}

float View::wireBend(int param) const {
    for (const auto &[route, bend] : controller_.layout().wireBends)
        if (route == param)
            return bend;
    return kDefaultWireBend;
}

void View::setWireBend(int param, float bend) {
    std::vector<std::pair<int, float>> &bends = controller_.layout().wireBends;
    std::erase_if(bends, [&](const auto &b) { return b.first == param; });
    bend = std::clamp(bend, -kMaxWireBend, kMaxWireBend);
    if (bend != kDefaultWireBend)
        bends.push_back({param, bend});
    controller_.layoutChanged();
}

bool View::droppable(const Widget &w, int source) const {
    const bool own = isStripSource(source) && !isNoteBorne(source) && w.param == stripKnobIndex(source);
    // A list's row takes it as an aux instead; a disabled widget, which the mouse passes by, takes nothing.
    return w.param >= 0 && isModTarget(w.param) && !own && !w.popup && !w.disabled &&
           (w.kind == Kind::Knob || w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::GridCell ||
            w.kind == Kind::SlotBar);
}

Color View::fxColor(int slot) const {
    return fxTypeColor(fxSlotType(slot));
}

Color View::listColor(int param) const {
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
        return slope == 2 ? kFilterSlopes - 1 : slope == 0 && splitThree(splitOf(param)) ? 1 : slope;
    }
    if (isFilterSlope(param))
        return playedSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), filterSlope(value(param)));
    return static_cast<int>(std::lround(value(param)));
}

bool View::listItemFits(int param, int item) const {
    if (isSplitSlope(param)) // 6, 12 or 24 dB; three bands 12 or 24
        return item != 2 && !(item == 0 && splitThree(splitOf(param)));
    return !isFilterSlope(param) || slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), item);
}

Color View::clipColor(int unit) const {
    return unit < kFxSlots ? fxColor(unit) : kShaperColor;
}

bool View::inspectorCovered() const {
    const int shape = shapeEditing();
    return fxWide() || modsWide() || (shape >= 0 && !isStripCurve(shape) && !isShaperCurve(shape));
}

bool View::fxWide() const {
    return controller_.layout().view == RoutingView::Fx && shapeEditing() < 0;
}

FxType View::fxSlotType(int slot) const {
    return fxType(value(paramIndex(slot, FxParam::Type)));
}

std::vector<int> View::fxChain() const {
    if (derived_.chain) // only kept while a scope goes on
        return *derived_.chain;
    std::vector<int> chain;
    for (const FxLineItem &item : fxLine()) {
        if (item.node < kFxSlots)
            chain.push_back(item.node);
        for (int lane : item.lanes())
            chain.insert(chain.end(), item.band(lane).begin(), item.band(lane).end());
    }
    if (derivedDepth_ > 0)
        derived_.chain = chain;
    return chain;
}

bool View::fxFolded(int slot) const {
    const UiLayout &layout = controller_.layout();
    return slot < kFirstFxSlots ? (layout.fxFolded >> slot) & 1u : (layout.fxFoldedLater >> (slot - kFirstFxSlots)) & 1u;
}

bool View::sourceFolded(int source) const {
    return !isStripSource(source) && ((controller_.layout().sourcesFolded >> source) & 1u);
}

float View::fxCardSpan(int slot) const {
    return fxFolded(slot) ? kFoldedFx : fxSpan(fxSlotType(slot));
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

// Folding a card or unfolding it; a new effect or source starts unfolded.
void View::setFolded(bool fx, int index, bool folded) {
    UiLayout &layout = controller_.layout();
    const bool later = fx && index >= kFirstFxSlots; // the slots past the first 16 have bits of their own
    uint32_t &bits = later ? layout.fxFoldedLater : fx ? layout.fxFolded : layout.sourcesFolded;
    const int bit = later ? index - kFirstFxSlots : index;
    if (!fx && folded && index == wideLfo_) // a widened card unfolds at its normal size
        wideLfo_ = -1;
    const uint32_t before = bits;
    bits = folded ? bits | (1u << bit) : bits & ~(1u << bit);
    if (bits != before)
        controller_.layoutChanged();
}

float View::fxScroll() const {
    return std::clamp(fxScroll_, 0.0f, std::max(scrollLength(ScrollArea::Fx) - kVisibleFx, 0.0f));
}

// On screen, at least in part, for a card `span` places wide.
static bool onScreen(float place, int shown, float span = 1.0f) {
    return place > -span && place < static_cast<float>(shown);
}

float View::scrollLength(ScrollArea area) const {
    if (area == ScrollArea::FxGraph)
        return fxGraphScrollMost(fxGridLayout(fxLine())) + fxGraphArea().w;
    std::optional<float> &kept = area == ScrollArea::Sources ? derived_.sourcesLength : derived_.fxLength;
    if (kept) // only kept while a scope goes on
        return *kept;
    float length = 0.0f;
    if (area == ScrollArea::Sources) {
        const std::vector<int> shown = sources();
        length = sourceOffset(shown, shown.size()) + (sourceShadow() ? kSourceShadowSpan : 0.0f);
    } else {
        length = fxRow(fxLine()).items.back() + (fxShadow() ? 1.0f : 0.0f);
    }
    if (derivedDepth_ > 0)
        kept = length;
    return length;
}

bool View::fxShadow() const {
    return fxChain().size() < static_cast<size_t>(kFxSlots);
}

bool View::sourceMenuShown() const {
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
    if (!sourceShadow() || controller_.layout().sourcesCollapsed || !onScreen(place, kVisibleCards, kSourceShadowSpan))
        return std::nullopt;
    return sourceCard(place, kSourceShadowSpan);
}

Point View::menuAtMouse(Point mouse, Rect menu) const {
    constexpr float kMargin = 4.0f;
    return {std::clamp(mouse.x, kMargin, std::max(kWidth - kMargin - menu.w, kMargin)),
            std::clamp(mouse.y, kMargin, std::max(height() - kMargin - menu.h, kMargin))};
}

float View::scrollShown(ScrollArea area) const {
    return area == ScrollArea::Fx        ? static_cast<float>(kVisibleFx)
           : area == ScrollArea::Sources ? static_cast<float>(kVisibleCards)
                                         : fxGraphArea().w;
}

float View::scrollFirst(ScrollArea area) const {
    return area == ScrollArea::Fx        ? fxScroll()
           : area == ScrollArea::Sources ? sourceScroll()
                                         : fxGraphScroll(fxGridLayout(fxLine()));
}

void View::setScroll(ScrollArea area, float first) {
    // The getters clamp the top end.
    (area == ScrollArea::Fx ? fxScroll_ : area == ScrollArea::Sources ? sourceScroll_ : fxGraphScroll_) =
        std::max(first, 0.0f);
}

Rect View::scrollThumb(ScrollArea area) const {
    const Rect track = area == ScrollArea::Fx        ? fxScrollTrack()
                       : area == ScrollArea::Sources ? sourceScrollTrack()
                                                     : fxGraphScrollTrack();
    const float cards = std::max(scrollLength(area), 1.0f);
    const float shown = scrollShown(area);
    const float width = std::max(track.w * std::min(shown, cards) / cards, 24.0f);
    const float hidden = cards - shown; // less than a card when only the dashed cards at the end are out of view
    return {track.x + (hidden > 0.0f ? (track.w - width) * scrollFirst(area) / hidden : 0.0f), track.y, width,
            track.h};
}

void View::addEffect(FxType type, int copyOf) {
    const UndoScope step(controller_); // with its wires
    int slot = -1;
    for (int s = 0; s < kFxSlots && slot < 0; ++s)
        if (fxSlotType(s) == FxType::None)
            slot = s;
    if (slot < 0) {
        flash(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        return;
    }
    // The slot gets its settings, the original's for a copy, then its type last: that makes the host rescan
    // the slot's fields, which by then hold what they should.
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < kFxParamCount; ++f) {
        const int index = paramIndex(slot, static_cast<FxParam>(f));
        if (static_cast<FxParam>(f) != FxParam::Type)
            changes.push_back(
                {index, copyOf >= 0 ? value(paramIndex(copyOf, static_cast<FxParam>(f))) : table_[index].def});
    }
    for (int f = 0; f < kShapeParamCount; ++f) { // the shaper's drawn curve
        const int index = clipCurveCountIndex(slot) + f;
        changes.push_back({index, copyOf >= 0 ? value(clipCurveCountIndex(copyOf) + f) : table_[index].def});
    }
    changes.push_back({paramIndex(slot, FxParam::Type), static_cast<double>(type)});
    setFolded(true, slot, false);
    if (shapeEditor_ == kMaxSources + slot) // left open for an effect the slot held before
        shapeEditor_ = -1;
    editMany(changes);
    fxBand_[slot] = copyOf >= 0 ? fxBand_[copyOf] : 0;
    // Where the list was opened, after the node the graph shows, or a copy after its original; either view scrolls to
    // show it.
    fxPlaceNew(slot, copyOf);
}

void View::removeEffect(int slot) {
    const UndoScope step(controller_);
    setFolded(true, slot, false);
    // Modulation of its knobs goes with it.
    for (const Assignment &m : assignments())
        if (fxSlotOf(m.target) == slot)
            removeSlot(m.source, m.slot);
    if (shapeEditor_ == kMaxSources + slot)
        shapeEditor_ = -1;
    // Out of the line, which closes up behind it.
    FxLine line = fxLine();
    for (FxLineItem &item : line)
        for (int lane : {1, 2, 3}) {
            std::vector<int> &band = item.band(lane);
            band.erase(std::remove(band.begin(), band.end(), slot), band.end());
        }
    line.erase(std::remove_if(line.begin(), line.end(), [slot](const FxLineItem &i) { return i.node == slot; }),
               line.end());
    writeFxLine(line);
    edit(paramIndex(slot, FxParam::Type), static_cast<double>(FxType::None));
}

bool View::fxAnyToClear() const {
    const FxLine line = fxLine();
    return !line.empty();
}

void View::clearFx() {
    const UndoScope step(controller_);
    for (const Assignment &m : assignments()) // the modulation of their knobs goes with them
        if (fxSlotOf(m.target) >= 0)
            removeSlot(m.source, m.slot);
    const std::vector<int> chain = fxChain();
    for (int slot : chain) {
        setFolded(true, slot, false);
        if (shapeEditor_ == kMaxSources + slot)
            shapeEditor_ = -1;
    }
    writeFxLine({}); // IN straight out, every split and join out of use
    std::vector<std::pair<int, double>> changes;
    for (int slot : chain)
        changes.push_back({paramIndex(slot, FxParam::Type), static_cast<double>(FxType::None)});
    editMany(changes);
    controller_.layout().fxSelected = -1;
    controller_.layoutChanged();
    fxScroll_ = 0.0f;
    fxGraphScroll_ = 0.0f;
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

void View::editMany(const std::vector<std::pair<int, double>> &changes) {
    for (const auto &[index, v] : changes)
        controller_.beginEdit(index);
    for (const auto &[index, v] : changes)
        controller_.setValue(index, std::clamp(v, table_[index].min, table_[index].max));
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        controller_.endEdit(it->first);
}

bool View::edgeScroll() {
    // A handle being dragged onto a target scrolls whichever row it is over; a card being moved, its own row.
    const bool handle = drag_.kind == DragKind::ModSource;
    const bool card = (drag_.kind == DragKind::FxMove || drag_.kind == DragKind::SourceMove) && drag_.moved;
    if (!handle && !card)
        return false;
    // Within kZone of a panel's left or right edge, or past it, the row moves up to kStep cards a poll. A card
    // grabbed that close to an edge only scrolls once it is taken further toward it than where it was grabbed.
    constexpr float kZone = 28.0f, kStep = 0.06f;
    const Point p = drag_.now;
    auto pull = [&](Rect panel) {
        if (handle && (p.y < panel.y || p.y >= panel.bottom()))
            return 0.0f;
        const float leftEdge = card ? std::min(panel.x + kZone, drag_.start.x - 4.0f) : panel.x + kZone;
        const float rightEdge = card ? std::max(panel.right() - kZone, drag_.start.x + 4.0f) : panel.right() - kZone;
        const float left = (leftEdge - p.x) / kZone;
        const float right = (p.x - rightEdge) / kZone;
        return left > 0.0f ? -std::min(left, 1.0f) : (right > 0.0f ? std::min(right, 1.0f) : 0.0f);
    };
    for (ScrollArea area : {ScrollArea::Sources, ScrollArea::Fx}) {
        const bool sources = area == ScrollArea::Sources;
        if (sources ? controller_.layout().sourcesCollapsed : !fxWide())
            continue;
        if (card && sources != (drag_.kind == DragKind::SourceMove))
            continue;
        const float speed = pull(sources ? modPanel(false) : kFxPanel);
        if (speed == 0.0f)
            continue;
        const float before = scrollFirst(area);
        setScroll(area, before + kStep * (speed < 0.0f ? -0.35f + 0.65f * speed : 0.35f + 0.65f * speed));
        return scrollFirst(area) != before;
    }
    return false;
}

void View::flash(std::string message) {
    flash_ = std::move(message);
    flashTicks_ = 160; // ~2.5 s of polls
}

int View::keyboardBase() const {
    return kBaseNote + 12 * octave_;
}

std::vector<int> View::sources() const {
    if (derived_.sources) // only kept while a scope goes on
        return *derived_.sources;
    std::vector<int> shown = shownSources(controller_.layout(), [this](int index) { return value(index); });
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

SourceType View::sourceTypeOf(int source) const {
    return isStripSource(source) ? SourceType::None : sourceType(value(paramIndex(source, SourceParam::Type)));
}

std::string View::sourceName(int source) const {
    return fmvibe::gui::sourceName(controller_.layout(), [this](int index) { return value(index); }, source);
}

std::string View::paramName(int index) const {
    if (index < 0 || index >= kParamCount)
        return std::string();
    if (const int owner = bypassSourceOf(index); owner >= 0) {
        const int target = slotTarget(value(slotTargetIndex(owner, bypassSlotOf(index))));
        return target >= 0 ? trf(Str::ParamNameBypassOn, {sourceName(owner), paramName(target)})
                           : trf(Str::ParamNameBypass, {sourceName(owner)});
    }
    const int source = sourceOf(index);
    const std::string &name = table_[index].name;
    if (source < 0)
        return name;
    const size_t place = trf(Str::ParamMod, {std::to_string(source + 1)}).size() + 1; // see buildTable()
    return sourceName(source) + " " + name.substr(place);
}

bool View::sourceOn(int source) const {
    return isStripSource(source) || value(sourceEnabledIndex(source)) >= 0.5;
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
    return sourceOn(source) && sourceUsed(source);
}

float View::sourceScroll() const {
    return std::clamp(sourceScroll_, 0.0f, std::max(scrollLength(ScrollArea::Sources) - kVisibleCards, 0.0f));
}

float View::cardOf(int source) const {
    if (source >= 0 && source < kMaxSources && derived_.cards[source]) // only kept while a scope goes on
        return *derived_.cards[source];
    float card = kOffScreen;
    if (!controller_.layout().sourcesCollapsed) {
        const std::vector<int> shown = sourcesShown();
        const auto at = std::find(shown.begin(), shown.end(), source);
        const float place = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
        card = at != shown.end() && onScreen(place, kVisibleCards, sourceSpan(source)) ? place : kOffScreen;
    }
    if (derivedDepth_ > 0 && source >= 0 && source < kMaxSources)
        derived_.cards[source] = card;
    return card;
}

void View::revealSource(int source) {
    const std::vector<int> shown = sources();
    const float place = sourceOffset(shown, static_cast<size_t>(std::find(shown.begin(), shown.end(), source) - shown.begin()));
    const float span = sourceSpan(source);
    const float first = sourceScroll();
    sourceScroll_ = place < first ? place : (place + span > first + kVisibleCards ? place + span - kVisibleCards : first);
}

void View::setSourceOrder(const std::vector<int> &shown) {
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

void View::addSource(SourceType type, int copyOf) {
    const UndoScope step(controller_); // with where it shows
    std::vector<int> shown = sources();
    int source = -1;
    for (int s = 0; s < kMaxSources && source < 0; ++s)
        if (sourceTypeOf(s) == SourceType::None)
            source = s;
    if (source < 0) {
        flash(trf(Str::MsgSourcesFull, {std::to_string(kMaxSources)}));
        return;
    }
    // Every field and its freeform shape: fresh, or as in the original. Its slots start empty either way: a copy
    // modulates nothing until it is given targets of its own. The type comes last, as in addEffect().
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
        fresh(slotAuxIndex(source, slot));
    }
    changes.push_back({paramIndex(source, SourceParam::Type), static_cast<double>(type)});
    const auto at = copyOf >= 0 ? std::find(shown.begin(), shown.end(), copyOf) + 1 : shown.end();
    shown.insert(at, source);
    setSourceOrder(shown);
    setFolded(false, source, false);
    if (shapeEditor_ == source) // left open for a source the place held before
        shapeEditor_ = -1;
    if (wideLfo_ == source)
        wideLfo_ = -1;
    editMany(changes);
    if (controller_.layout().sourcesCollapsed) {
        controller_.layout().sourcesCollapsed = false;
        controller_.layoutChanged();
    }
    revealSource(source);
}

void View::removeSource(int source) {
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
    if (shapeEditor_ == source)
        shapeEditor_ = -1;
    if (wideLfo_ == source)
        wideLfo_ = -1;
    edit(paramIndex(source, SourceParam::Type), static_cast<double>(SourceType::None));
}

// Snapshots come once per audio block, which can be tens of milliseconds apart;
// in between, the phase moves on at the LFO's rate, so the dot glides. A host
// that stopped processing doesn't send it far. A one-shot stops at the end of its cycle.
double View::lfoPhaseNow(int lfo) const {
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    const double phase = telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age;
    if (value(paramIndex(lfo, SourceParam::OneShot)) >= 0.5)
        return std::min(phase, 1.0);
    return phase - std::floor(phase);
}

LfoClock View::lfoClockOf(int lfo) const {
    return lfoClock([this](int i) { return value(i); }, lfo);
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

bool View::lfoPhaseUsed(int lfo) const {
    // An anchored LFO stood at its Phase at the song's start.
    auto get = [this](int i) { return value(i); };
    return (!lfoShared(get, lfo) || lfoAnchored(get, lfo)) &&
           std::lround(value(paramIndex(lfo, SourceParam::Shape))) != static_cast<long>(LfoShape::Random);
}

double View::lfoValueNow(int lfo) const {
    const auto shape = static_cast<LfoShape>(std::clamp(
        static_cast<int>(std::lround(value(paramIndex(lfo, SourceParam::Shape)))), 0, static_cast<int>(LfoShape::Count) - 1));
    if (shape == LfoShape::Random) { // can't be told ahead
        const double out = telemetry_.sources[lfo];
        return value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? out * 2.0 - 1.0 : out;
    }
    if (shape == LfoShape::Custom)
        return shapeValue([this](int index) { return value(index); }, lfo, lfoPhaseNow(lfo));
    return lfoValue(shape, lfoPhaseNow(lfo), 0.0);
}

void View::traceRandomLfos() {
    // A new cycle is a new glide; one that stops running, or isn't random any more, starts over.
    for (int s = 0; s < kMaxSources; ++s) {
        RandomTrace &trace = randomTraces_[s];
        const bool random = sourceTypeOf(s) == SourceType::Lfo &&
                            std::lround(value(paramIndex(s, SourceParam::Shape))) == static_cast<long>(LfoShape::Random);
        if (!random || telemetry_.voices == 0 || !sourceOn(s)) {
            trace.count = 0;
            continue;
        }
        const std::pair glide{telemetry_.lfoFrom[s], telemetry_.lfoTo[s]};
        const float phase = telemetry_.lfoPhases[s];
        if (trace.count > 0 && trace.glides[(trace.count - 1) % kRandomCycles] == glide) {
            trace.ended = trace.ended || phase < trace.phase;
            trace.phase = phase;
            continue;
        }
        trace.glides[trace.count % kRandomCycles] = glide;
        ++trace.count;
        trace.phase = phase;
        trace.ended = false;
    }
}

bool View::randomSweep(int lfo) const {
    return telemetry_.voices > 0 && sourceOn(lfo) && randomTraces_[lfo].count > 0;
}

double View::randomPhaseNow(int lfo) const {
    if (randomTraces_[lfo].ended)
        return 1.0;
    const double age = std::clamp(static_cast<double>(telemetry_.age), 0.0, 0.1);
    return std::min(telemetry_.lfoPhases[lfo] + telemetry_.lfoRates[lfo] * age, 1.0);
}

bool View::poll() {
    bool changed = false;
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
    for (int i = 0; i < kParamCount; ++i) {
        const double v = controller_.value(i);
        if (v != seen_[i]) {
            seen_[i] = v;
            changed = true;
        }
    }
    if (changed)
        keepRoutes(); // whatever turned a route on, the editor or the host
    closeGoneLists(); // the host may have changed what a list belongs to
    // The audio thread captures the operator whose scope shows, and nothing while none does.
    const UiLayout &layout = controller_.layout();
    const bool scope = layout.selectedNode < kOps && !inspectorCovered() &&
                       opTabShown(layout.selectedNode) == InspectorTab::Scope;
    controller_.setScope(scope ? layout.selectedNode : -1, layout.scopeCycles);
    Telemetry t;
    controller_.telemetry(t);
    everywhere = everywhere || changed; // a param: it may show anywhere
    const Rect header{0.0f, 0.0f, kWidth, kHeaderH};
    // While notes play, what they move: the sources, the modulation on the knobs, the envelopes and levels show
    // anywhere; the playing count in the header, the keys on the keyboard and the operator's scope in the inspector
    // only. An LFO's dot glides on from its snapshot by the snapshot's age (see lfoPhaseNow()), so it moves even
    // between snapshots alike.
    if ((t.voices > 0) != (telemetry_.voices > 0))
        everywhere = true;
    else if (t.voices != telemetry_.voices)
        in(header);
    if (t.voices > 0 || telemetry_.voices > 0) {
        const bool moved =
            t.modOffset != telemetry_.modOffset || t.slotOffset != telemetry_.slotOffset ||
            t.sources != telemetry_.sources || t.lfoPhases != telemetry_.lfoPhases ||
            t.lfoRates != telemetry_.lfoRates || t.lfoFrom != telemetry_.lfoFrom || t.lfoTo != telemetry_.lfoTo ||
            t.notePitch != telemetry_.notePitch || t.noteVelocity != telemetry_.noteVelocity ||
            t.fxPitch != telemetry_.fxPitch || t.opLevels != telemetry_.opLevels ||
            t.opEnvLevels != telemetry_.opEnvLevels || t.opEnvPos != telemetry_.opEnvPos ||
            t.envPos != telemetry_.envPos ||
            (t.voices > 0 && std::any_of(t.lfoRates.begin(), t.lfoRates.end(), [](float r) { return r != 0.0f; }));
        everywhere = everywhere || moved;
        if (t.scope != telemetry_.scope || t.scopeBins != telemetry_.scopeBins ||
            t.scopeCursor != telemetry_.scopeCursor)
            in(kInspector);
    }
    // A compressor's and an amp's meters move on while a tail drives them once the notes have ended, and the host's
    // tempo shows in the synced effects' captions whether notes play or not.
    if (t.fxReduction != telemetry_.fxReduction || t.tempo != telemetry_.tempo)
        everywhere = true;
    if (t.keys != telemetry_.keys) {
        const bool collapsed = controller_.layout().sourcesCollapsed;
        in(keyboardPanel(keyboardTop(collapsed), controller_.layout().keysCollapsed));
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
    // The operator scope's level, of the part of its span filled so far; none while it captures nothing.
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
        in(kInspector);
    // A three-band dynamics' bars move with its bands' levels and gains, until they settle.
    for (int slot : fxChain())
        if (fxSlotType(slot) == FxType::MultiDyn)
            for (int band = 0; band < kMcBands; ++band)
                if (std::abs(t.fxBandLevel[slot][band] - telemetry_.fxBandLevel[slot][band]) >
                        0.01f * std::max(t.fxBandLevel[slot][band], 1e-5f) ||
                    std::abs(t.fxBandGain[slot][band] - telemetry_.fxBandGain[slot][band]) > 0.05f)
                    everywhere = true;
    // The effects graph's nodes and wires glow with what goes through them; with the glow off, they show nothing of it.
    if (fxWide() && fxGraph() && shownGlow() > 0.0f)
        for (int port = 0; port < kFxPorts; ++port)
            if (std::abs(t.fxPortLevels[port] - telemetry_.fxPortLevels[port]) > 0.002f * (1.0f + t.fxPortLevels[port]))
                everywhere = true;
    // Phasers, flangers and wideners sweep while the audio runs.
    if (t.age < 0.5f)
        for (int slot : fxChain())
            if (fxSlotType(slot) == FxType::Phaser || fxSlotType(slot) == FxType::Flanger ||
                fxSlotType(slot) == FxType::Widener)
                everywhere = true;
    // An analyzer's spectrum goes on falling away after the audio stops, until it is gone; while notes play, the
    // filters' move along with them.
    auto showing = [](const Spectrum &spectrum) {
        return std::any_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; });
    };
    if (t.age < 1.5f)
        everywhere = everywhere || std::any_of(t.fxSpectrum.begin(), t.fxSpectrum.end(), showing) ||
                  std::any_of(t.filterSpectrum.begin(), t.filterSpectrum.end(), showing);
    telemetry_ = t;
    traceRandomLfos();
    if (flashTicks_ > 0 && --flashTicks_ == 0)
        everywhere = true;
    if (edgeScroll()) {
        everywhere = true;
        if (drag_.kind == DragKind::FxMove && drag_.moved) // where it lands follows the row scrolling under it
            retargetRowDrag(drag_.now);
    }
    if (glideMods())
        everywhere = true;
    if (glideFxGraph()) // the effects graph's nodes making room for one dragged, or settling after
        everywhere = true;
    if (glideFxRow()) // the row's cards and containers likewise
        everywhere = true;
    if (sourceMenuOpen_ && !sourceMenuShown()) { // its card or button went: scrolled away, the panel shut, the places full
        sourceMenuOpen_ = false;
        everywhere = true;
    }
    if (modList_ && !modListPillShown()) { // its pill went: fewer modulations, or what showed it changed
        modList_.reset();
        everywhere = true;
    }
    if (themeIndex(controller_.theme()) != theme_ || shownGlow() != glow_ || shownLinks() != links_ ||
        controller_.handleSize() != handles_) // picked in another editor
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

Point View::knobCenter(const Widget &w) {
    if (isRatioDisplay(w))
        return kRatioBox.center();
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
    return w.compact ? p.x >= w.rect.x + compactTextX(w) - 4.0f : p.y >= w.rect.y + w.rect.w;
}

Rect View::envRect(int env) const {
    // Mod envelopes are only drawn while a card shows them; otherwise any card's display will do.
    const float card = env < kOps ? kOffScreen : cardOf(env - kOps);
    return env < kOps ? kOpEnvRect : sourceDisplay(card == kOffScreen ? 0.0f : card);
}

float View::envScale(int env) const {
    const float fit = envFitScale(envRect(env), envIndices(env), [this](int index) { return value(index); });
    // While one of its handles is dragged, the zoom holds still, so the point stays under the mouse, until the
    // envelope would run past the display's end: then it zooms out to keep all of it in.
    if ((drag_.kind == DragKind::EnvPoint || drag_.kind == DragKind::EnvCurve) && drag_.widget.a == env)
        return std::min(drag_.envScale, fit);
    return fit;
}

void View::mergeButtons(std::vector<Widget> &list, size_t from) {
    if (list.size() < from + 2)
        return;
    Rect group = list[from].rect;
    group.w = list.back().rect.right() - group.x;
    for (size_t i = from; i < list.size(); ++i) {
        Widget &w = list[i];
        w.group = group;
        w.partLeft = i == from ? group.x : (list[i - 1].rect.right() + w.rect.x) * 0.5f;
        w.partRight = i + 1 == list.size() ? group.right() : (w.rect.right() + list[i + 1].rect.x) * 0.5f;
    }
}

std::vector<View::Widget> View::widgets() const {
    const DerivedScope scope(*this);
    std::vector<Widget> list;
    list.reserve(400);
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    auto merge = [&list](size_t from) { mergeButtons(list, from); };
    // A wire's or OUT label in the graph: the label, then, while it is a pill (see labelPill()), its mode's part and its
    // x, all three drawn as one.
    auto labelWidgets = [&](Kind kind, int param, int mode, Rect idle, int from, int to) {
        const LabelPill pill = labelPill(param, mode, idle);
        add(kind, pill.depth, param, from, kind == Kind::OutLabel ? -1 : to);
        list.back().modeGlyph = pill.glyph;
        list.back().graph = true;
        if (!pill.open)
            return;
        list.back().group = pill.whole;
        if (mode >= 0) {
            add(Kind::LabelMode, pill.mode, mode, from, to);
            list.back().group = pill.whole;
        }
        add(Kind::LabelRemove, pill.remove, param, from, to);
        list.back().group = pill.whole;
    };
    auto knob = [&](int param, Point center, float diameter, const char *label) {
        add(Kind::Knob, knobRect(center, diameter), param);
        list.back().label = label;
    };
    // A drawn shaper curve is edited right in its display, as in the big editor its edit button opens.
    auto drawnCurve = [&](int unit, Rect display) {
        const int shape = kMaxSources + unit;
        add(Kind::ShapeArea, display, -1, shape);
        const int count = shapePointCount(shape);
        for (int point = 0; point + 1 < count; ++point) {
            if (shapePointX(shape, point + 1) - shapePointX(shape, point) < 1e-6 || !pointShown(shape, point))
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(shape, point);
            add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(shape, point, PointField::Curve),
                shape, point);
            list.back().compact = true;
        }
        for (int point = 0; point < count; ++point) {
            if (!pointShown(shape, point))
                continue;
            const Point c = shapePointPos(shape, point);
            add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, shape, point);
            list.back().compact = true;
        }
        // The edit button in the corner a curve leaves empty
        add(Kind::ClipDisplay, clipEditButton(display), -1, unit);
    };

    // Header.
    auto headerKnob = [&](int param, float x, float width, const char *label) {
        add(Kind::Knob, {x, kStripY + 12.0f, width, 40.0f}, param);
        list.back().label = label;
        list.back().compact = true;
    };
    const int globals[] = {paramIndex(GlobalParam::Volume), paramIndex(GlobalParam::FmAmount),
                           paramIndex(GlobalParam::Pitch), paramIndex(GlobalParam::Pan)};
    const Str globalLabels[] = {Str::LabelVolume, Str::LabelFmAmount, Str::LabelPitch, Str::LabelPan};
    for (int i = 0; i < 4; ++i)
        headerKnob(globals[i], kHeaderKnobX + i * kHeaderKnobW, kHeaderKnobW, tr(globalLabels[i]));
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
    // Voices, past the second divider, beside the count. (Bend's knob is over the bend wheel, in the keyboard strip.)
    headerKnob(paramIndex(GlobalParam::Voices), kVoicesX, kVoicesW, tr(Str::LabelVoices));

    // Routing panel, as a graph, as a matrix, or the effects chain; or the shape
    // editor of a freeform LFO, in place of all three.
    const int editing = shapeEditing();
    for (int v = 0; v < static_cast<int>(std::size(kViewTabs)); ++v)
        add(Kind::ViewTab, kViewTabs[v], -1, v);
    const RoutingView view = controller_.layout().view;
    // Switched-off nodes are left out of the graph, and greyed out in the matrix; their tabs switch them back on.
    const int partials = partialEditing();
    const std::vector<int> on = enabledNodes();
    if (editing >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::ShapeReset, shapeReset(), -1, editing);
        for (int axis = 0; axis < 2; ++axis)
            for (int part = 0; part < 3; ++part)
                add(Kind::ShapeGrid, shapeGridButton(axis, part), -1, axis, part - 1);
        add(Kind::ShapeArea, shapeArea().inset(-8.0f), -1, editing);
        for (int mode = 0; mode < 3; ++mode) { // Points | Steps in one pill, and Snap, as in a widened LFO card
            add(Kind::ShapeMode, shapeModeButton(mode), -1, editing, mode);
            if (mode == 1)
                merge(list.size() - 2);
        }
        if (isNoteCurve(editing)) { // the range across, in the title row, after the title, close together
            const char *const labels[] = {tr(Str::LabelLow), tr(Str::LabelHigh)};
            const int range[] = {paramIndex(NoteParam::Low), paramIndex(NoteParam::High)};
            for (int i = 0; i < 2; ++i) {
                add(Kind::Knob, {kRouting.x + 148.0f + i * 96.0f, kRouting.y, 92.0f, 40.0f}, range[i]);
                list.back().label = labels[i];
                list.back().compact = true;
            }
        }
        const int count = shapePointCount(editing);
        for (int point = 0; point < count - (isCurve(editing) ? 1 : 0); ++point) { // a curve doesn't wrap
            const double next = point + 1 < count ? shapePointX(editing, point + 1) : 1.0;
            if (next - shapePointX(editing, point) < 1e-6 || !pointShown(editing, point))
                continue; // a jump straight up or down has nothing to bend
            const Point c = shapeCurvePos(editing, point);
            add(Kind::ShapeCurve, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, shapeIndex(editing, point, PointField::Curve),
                editing, point);
        }
        for (int point = 0; point < count; ++point) {
            if (!pointShown(editing, point))
                continue;
            const Point c = shapePointPos(editing, point);
            add(Kind::ShapePoint, {c.x - 7.0f, c.y - 7.0f, 14.0f, 14.0f}, -1, editing, point);
        }
    } else if (partials >= 0) {
        add(Kind::ShapeDone, shapeDone());
        add(Kind::PartialReset, shapeReset(), -1, partials);
        const size_t counts = list.size();
        for (int choice = 0; choice < static_cast<int>(std::size(kPartialCounts)); ++choice)
            add(Kind::Choice, partialCountButton(choice), partialCountIndex(partials), -1, kPartialCounts[choice]);
        merge(counts);
        add(Kind::PartialArea, kPartialLevels, -1, partials, 0);
        add(Kind::PartialArea, kPartialPhases, -1, partials, 1);
    } else if (view == RoutingView::Mods) {
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
                add(Kind::ModsAux, {aux.x, aux.y + 3.0f, aux.w - 24.0f, aux.h - 6.0f}, -1, m.source, m.slot);
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
    } else if (view == RoutingView::Fx) {
        add(Kind::FxMenuButton, fxMenuButton());
        if (scrollLength(ScrollArea::Fx) > kVisibleFx + 0.01f && !fxGraph())
            add(Kind::Scrollbar, fxScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::Fx));
        if (fxGraph() && scrollLength(ScrollArea::FxGraph) > fxGraphArea().w + 0.5f) // wider than its box
            add(Kind::Scrollbar, fxGraphScrollTrack().inset(-4.0f), -1, static_cast<int>(ScrollArea::FxGraph));
        const size_t cardsStart = list.size();
        // A card's widgets, where it shows: in the row, or in the effects graph's inspector (`graph`), where it
        // neither folds nor moves.
        auto cardWidgets = [&](int slot, size_t pos, Rect card, bool graph) {
            const FxType type = fxSlotType(slot);
            auto at = [slot](FxParam f) { return paramIndex(slot, f); };
            // The title row, as every card's: the icon at the title's left (which also switches the effect on and
            // off, and drags the card like the title does), then the fold button, the power switch and the cross. In
            // the graph, only the power switch, in the cross's corner: the node's own cross removes the effect.
            const Rect icon = fxIconRect(card).inset(-4.0f);
            if (!graph && fxFolded(slot)) { // a strip: the icon, a grip, the switch and cross, then the rest unfolds it
                add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
                add(Kind::FxGrip, {card.x, card.y + 30.0f, card.w, 16.0f}, -1, slot, static_cast<int>(pos));
                add(Kind::Toggle, {card.center().x - 10.0f, card.y + 50.0f, 20.0f, 19.0f}, at(FxParam::Enabled));
                add(Kind::FxRemove, {card.center().x - 7.0f, card.y + 73.0f, 14.0f, 21.0f}, -1, slot);
                add(Kind::FxFold, {card.x, card.y + 98.0f, card.w, card.h - 98.0f}, -1, slot);
                return;
            }
            if (!graph) // in the graph, the card stays in the inspector
                add(Kind::FxGrip, {card.x, card.y, cardTitleRight(card) - card.x, 34.0f}, -1, slot,
                    static_cast<int>(pos));
            add(Kind::FxPower, icon, at(FxParam::Enabled), slot, static_cast<int>(pos));
            if (!graph)
                add(Kind::FxFold, cardFold(card), -1, slot);
            add(Kind::Toggle, graph ? cardPowerInGraph(card) : cardPower(card), at(FxParam::Enabled));
            if (!graph)
                add(Kind::FxRemove, cardClose(card), -1, slot);
            const float row = card.y + 44.0f; // mode buttons and switches
            auto knobs = [&](std::initializer_list<int> params, std::initializer_list<const char *> labels,
                             float y = 240.0f) {
                const float step = card.w / static_cast<float>(params.size());
                const float diameter = params.size() > 3 ? 32.0f : (y == 240.0f ? 36.0f : 30.0f);
                for (size_t i = 0; i < params.size(); ++i)
                    if (params.begin()[i] >= 0) // -1 leaves its place empty
                        knob(params.begin()[i], {card.x + step * (static_cast<float>(i) + 0.5f), card.y + y}, diameter,
                             labels.begin()[i]);
            };
            switch (type) {
            case FxType::Shaper: {
                add(Kind::ListButton, clipModeButton(card), at(FxParam::ShaperMode));
                const bool custom = clipMode(value(at(FxParam::ShaperMode))) == ClipMode::Custom;
                const size_t switches = list.size();
                if (custom)
                    add(Kind::Toggle, clipSymButton(card), at(FxParam::ShaperSymmetric));
                add(Kind::Toggle, clipLinkButton(card, custom), at(FxParam::ShaperLink));
                merge(switches);
                if (custom)
                    drawnCurve(slot, fxDisplayIn(card, type));
                knobs({at(FxParam::ShaperDrive), at(FxParam::ShaperOutput), at(FxParam::ShaperMix)},
                      {tr(Str::LabelDrive), tr(Str::LabelOutput), tr(Str::LabelMix)});
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
                // Synced, the time knobs pick note values and show them as their labels. Linked, the
                // left one sets both sides.
                const bool synced = value(at(FxParam::DelaySync)) >= 0.5;
                const int timeL = synced ? at(FxParam::DelayDivisionL) : at(FxParam::DelayTimeL);
                const int timeR = synced ? at(FxParam::DelayDivisionR) : at(FxParam::DelayTimeR);
                const bool linked = value(at(FxParam::DelayLink)) >= 0.5;
                knobs({timeL, linked ? -1 : timeR, at(FxParam::DelayFeedback)},
                      {synced ? nullptr : tr(linked ? Str::LabelTime : Str::LabelTimeL),
                       synced ? nullptr : tr(Str::LabelTimeR), tr(Str::LabelFeedback)},
                      206.0f);
                knobs({at(FxParam::DelayTone), at(FxParam::DelayWidth), at(FxParam::DelayMix)},
                      {tr(Str::LabelTone), tr(Str::LabelWidth), tr(Str::LabelMix)}, 266.0f);
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
                const int band = std::min(fxBand_[slot], kPeqBands - 1);
                const int params[] = {peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band),
                                      at(FxParam::PeqAmount)};
                const Str labels[] = {Str::LabelFreq, Str::LabelGain, Str::LabelQ, Str::LabelAmount};
                for (int i = 0; i < 4; ++i)
                    knob(params[i], peqKnobCenter(card, i), kPeqKnobD, tr(labels[i]));
                break;
            }
            case FxType::Filter: {
                // The filter types in a row of icons, and the slope's list under the response.
                constexpr int kTypes = static_cast<int>(FilterType::Count);
                const float w = (card.w - 28.0f + 4.0f) / kTypes;
                const size_t types = list.size();
                for (int t = 0; t < kTypes; ++t)
                    add(Kind::Choice, {card.x + 14.0f + t * w, row, w - 4.0f, 20.0f}, at(FxParam::FilterType), -1, t);
                merge(types);
                add(Kind::ListButton, slopeButton(fxDisplayIn(card, type)), at(FxParam::FilterSlope));
                // The response's handle, as a voice filter's: b = 1 marks the effect's.
                const Point handle = filterHandle(fxDisplayIn(card, type), value(at(FxParam::FilterCutoff)),
                                                  value(at(FxParam::FilterResonance)));
                add(Kind::FilterPoint, {handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f}, -1, slot, 1);
                knobs({at(FxParam::FilterCutoff), at(FxParam::FilterResonance), at(FxParam::FilterKeyTrack)},
                      {tr(Str::LabelCutoff), tr(Str::LabelReso), tr(Str::LabelKeyTrack)}, 206.0f);
                knobs({at(FxParam::FilterDrive), -1, at(FxParam::FilterMix)},
                      {tr(Str::LabelDrive), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            }
            case FxType::Compressor:
                knobs({at(FxParam::CompThreshold), at(FxParam::CompRatio), at(FxParam::CompKnee)},
                      {tr(Str::LabelThreshold), tr(Str::LabelRatio), tr(Str::LabelKnee)}, 206.0f);
                knobs({at(FxParam::CompAttack), at(FxParam::CompRelease), at(FxParam::CompMakeup),
                       at(FxParam::CompMix)},
                      {tr(Str::LabelAttack), tr(Str::LabelRelease), tr(Str::LabelMakeup), tr(Str::LabelMix)}, 266.0f);
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
                const int band = fxBand_[slot];
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
                if (reverbType(value(at(FxParam::ReverbType))) == ReverbType::Shimmer) {
                    // Its interval and how much of what goes round it shifts, among the others.
                    knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay),
                           at(FxParam::ReverbShimmerPitch)},
                          {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay), tr(Str::LabelPitch)},
                          206.0f);
                    knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbWidth), at(FxParam::ReverbShimmer),
                           at(FxParam::ReverbMix)},
                          {tr(Str::LabelDamp), tr(Str::LabelWidth), tr(Str::LabelShimmer), tr(Str::LabelMix)}, 266.0f);
                    break;
                }
                knobs({at(FxParam::ReverbSize), at(FxParam::ReverbDecay), at(FxParam::ReverbPreDelay)},
                      {tr(Str::LabelSize), tr(Str::LabelDecay), tr(Str::LabelPreDelay)}, 206.0f);
                knobs({at(FxParam::ReverbDamping), at(FxParam::ReverbWidth), at(FxParam::ReverbMix)},
                      {tr(Str::LabelDamp), tr(Str::LabelWidth), tr(Str::LabelMix)}, 266.0f);
                break;
            case FxType::Phaser:
            case FxType::Flanger: {
                // Under the display: at the left, over the Rate knob, SYNC (the delay's metronome), then STEREO, two
                // switches of their own; at the right the polarity, + and - as one choice. Synced, the Rate knob
                // picks a note value and shows it as its label.
                const bool phaser = type == FxType::Phaser;
                const int sync = at(phaser ? FxParam::PhaserSync : FxParam::FlangerSync);
                const Rect display = fxDisplayIn(card, type);
                const float under = display.bottom() + 4.0f;
                add(Kind::Toggle, {display.x, under, 32.0f, 18.0f}, sync);
                add(Kind::Toggle, {display.x + 36.0f, under, 60.0f, 18.0f},
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
                    knobs({rate, at(FxParam::PhaserDepth), at(FxParam::PhaserCenter)},
                          {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelCenter)}, 206.0f);
                    knobs({at(FxParam::PhaserFeedback), at(FxParam::PhaserStages), at(FxParam::PhaserMix)},
                          {tr(Str::LabelFeedback), tr(Str::LabelStages), tr(Str::LabelMix)}, 266.0f);
                    break;
                }
                knobs({rate, at(FxParam::FlangerDepth), at(FxParam::FlangerDelay)},
                      {synced ? nullptr : tr(Str::LabelRate), tr(Str::LabelDepth), tr(Str::LabelDelay)}, 206.0f);
                knobs({at(FxParam::FlangerFeedback), -1, at(FxParam::FlangerMix)},
                      {tr(Str::LabelFeedback), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            }
            case FxType::FreqShifter:
                // The range, then the share of it the shift takes.
                knobs({at(FxParam::ShiftRange), at(FxParam::ShiftAmount), at(FxParam::ShiftSpread)},
                      {tr(Str::LabelRange), tr(Str::LabelShift), tr(Str::LabelSpread)}, 206.0f);
                knobs({at(FxParam::ShiftFeedback), -1, at(FxParam::ShiftMix)},
                      {tr(Str::LabelFeedback), nullptr, tr(Str::LabelMix)}, 266.0f);
                break;
            case FxType::Disperser:
                knobs({at(FxParam::DisperserFreq), at(FxParam::DisperserStages), at(FxParam::DisperserPinch),
                       at(FxParam::DisperserMix)},
                      {tr(Str::LabelFreq), tr(Str::LabelStages), tr(Str::LabelPinch), tr(Str::LabelMix)});
                break;
            case FxType::Widener:
                knobs({at(FxParam::WidenerSize), at(FxParam::WidenerWidth), at(FxParam::WidenerHighPass),
                       at(FxParam::WidenerMix)},
                      {tr(Str::LabelSize), tr(Str::LabelWidth), tr(Str::LabelHighPass), tr(Str::LabelMix)});
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
                      206.0f);
                knobs({at(FxParam::UtilityWidth), at(FxParam::UtilityMonoBass)},
                      {tr(Str::LabelWidth), tr(Str::LabelMonoBass)}, 266.0f);
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
        };
        if (fxGraph()) {
            fxGraphWidgets(list);
            if (const int node = fxSelectedNode(); node >= 0 && node < kFxSlots)
                cardWidgets(node, 0, fxInspector(node), true);
            else if (fxNodeSplit(node)) // a split and its join share their controls' card
                splitControlWidgets(list, node - kFxSlots, fxInspector(node), false);
            else if (fxNodeSum(node) >= 0)
                splitControlWidgets(list, fxNodeSum(node), fxInspector(node), false);
        } else {
            // The row: the effects' cards, and each split's container, its controls, its band's cards and its dashed
            // card that adds to that band.
            const FxRow row = fxRowShown();
            const float scroll = fxScroll();
            for (const FxRowCard &card : row.cards)
                if (const float place = card.place - scroll; onScreen(place, kVisibleFx, card.span))
                    cardWidgets(card.slot, 0, fxCard(place, card.span), false);
            carriedWidgets_.clear();
            if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && fxDrag_->item.node < kFxSlots &&
                !fxFolded(fxDrag_->item.node)) {
                const size_t mark = list.size();
                cardWidgets(fxDrag_->item.node, 0, fxCard(fxDrag_->startPlace - drag_.scrollStart, fxDrag_->span),
                            false);
                carriedWidgets_.assign(list.begin() + static_cast<std::ptrdiff_t>(mark), list.end());
                list.resize(mark);
            }
            for (const FxRowBox &box : row.boxes) {
                const float place = box.place - scroll;
                if (box.folded) { // a strip: the icon and a grip drag it, the cross, then the rest unfolds it
                    if (!onScreen(place, kVisibleFx, box.span))
                        continue;
                    const Rect strip = fxCard(place, box.span);
                    add(Kind::FxBoxGrip, {strip.x, strip.y, strip.w, 46.0f}, -1, fxSplitNode(box.split));
                    add(Kind::FxNodeRemove, {strip.center().x - 9.0f, strip.y + 50.0f, 18.0f, 21.0f}, -1,
                        fxSplitNode(box.split));
                    add(Kind::FxBoxFold, {strip.x, strip.y + 76.0f, strip.w, strip.h - 76.0f}, -1, box.split);
                    continue;
                }
                if (const float column = boxColumn(box) - scroll; onScreen(column, kVisibleFx))
                    splitControlWidgets(list, box.split, fxCard(column, 1.0f), true);
                if (const float at = box.add - scroll; fxShadow() && onScreen(at, kVisibleFx, kFxBoxAddSpan)) {
                    const Rect card = fxCard(at);
                    add(Kind::FxBoxAdd, fxBoxAddCard(card), -1, box.lane, box.item);
                }
            }
            if (const float place = row.items.back() - scroll; fxShadow() && onScreen(place, kVisibleFx))
                add(Kind::FxShadow, fxCard(place));
            // The arrows between the cards, while there is room for another effect: a click adds one there. On the
            // main line before each item, the first too; in a container, before each of its band's cards (the first
            // after its controls). None goes before a dashed card, which adds there itself.
            auto insert = [&](float place, int lane, int item, int at) {
                if (place - scroll >= 0.0f && place - scroll < kVisibleFx) {
                    const Rect card = fxCard(place - scroll);
                    add(Kind::FxInsert, {card.x - kFxGap * 0.5f - 10.0f, card.center().y - 14.0f, 20.0f, 28.0f}, -1,
                        lane, item * kFxGapStride + at);
                }
            };
            for (size_t i = 0; i + 1 < row.items.size() && fxShadow(); ++i)
                insert(row.items[i], 0, static_cast<int>(i), 0);
            for (const FxRowBox &box : row.boxes) {
                if (box.folded || !fxShadow())
                    continue;
                int k = 0;
                for (const FxRowCard &card : row.cards)
                    if (card.at.lane == box.lane && card.at.item == box.item)
                        insert(card.place, box.lane, box.item, k++);
            }
        }
        for (size_t i = cardsStart; i < list.size() && !fxGraph(); ++i)
            list[i].clip = fxCardClip();
        for (int graph = 0; graph < 2; ++graph) // the switch between the chain and the graph
            add(Kind::FxMode, fxModeButton(graph), -1, -1, graph);
        merge(list.size() - 2);
        add(Kind::FxClear, fxClearButton());
    } else if (view == RoutingView::Matrix) {
        // Rows feed columns; the last column is the output. The rows' and columns' names work like the
        // nodes' tabs.
        for (int row = 0; row < kNodes; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                add(Kind::GridCell, gridCell(row, col), gridParam(row, col), row, col);
            }
        }
        for (int node = 0; node < kNodes; ++node) {
            const Rect row = gridCell(node, 0), col = gridCell(0, node);
            add(Kind::MatrixLabel, {row.x - 32.0f, row.y, 28.0f, row.h}, -1, node);
            add(Kind::MatrixLabel, {col.x - 2.0f, col.y - kMatrixNamesH, col.w + 4.0f, 18.0f}, -1, node);
        }
    } else {
        // The wires between the nodes, under everything else: dragged sideways, they bend. Then the nodes, each with
        // its edge, then depth labels and arrowheads on top of them.
        const int rerouted = rerouting();
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (from == to || !wired(param) || param == rerouted)
                    continue;
                const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(param));
                const float left = std::min({w.start.x, w.control.x, w.end.x});
                const float top = std::min({w.start.y, w.control.y, w.end.y});
                const float right = std::max({w.start.x, w.control.x, w.end.x});
                const float bottom = std::max({w.start.y, w.control.y, w.end.y});
                add(Kind::Wire, Rect{left, top, right - left, bottom - top}.inset(-kWireGrab), param, from, to);
            }
        }
        for (int node : on) {
            const Point c = nodeCenter(node);
            const Rect r = isBoxNode(node)
                               ? Rect{c.x - kFilterHalfW, c.y - kFilterHalfH, 2.0f * kFilterHalfW, 2.0f * kFilterHalfH}
                               : Rect{c.x - kNodeRadius, c.y - kNodeRadius, 2.0f * kNodeRadius, 2.0f * kNodeRadius};
            add(Kind::Node, r, -1, node);
            add(Kind::Port, r.inset(-kEdgeOut), -1, node);
        }
        const int moving = rerouting(); // follows the mouse, without its label
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (!wired(param) || param == moving)
                    continue;
                Rect idle;
                if (from == to) {
                    const Point loop = loopCenter(from, nodeCenter(from));
                    idle = chip({loop.x + 32.0f, loop.y}, 44.0f);
                } else {
                    const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(param));
                    idle = chip(labelAlong(w.start, w.control, w.end, w.label, 38.0f));
                }
                labelWidgets(Kind::WireLabel, param, routeModeParam(from, to), idle, from, to);
            }
        }
        for (int node : on)
            if (goesOut(node) && outParam(node) != moving) {
                const OutWire w = outWireOf(node, nodeCenter(node));
                // Beside the line where it is too short for the label to fit along it; else along it, clear of the
                // nodes.
                const Point at = w.label.x != w.start.x
                                     ? w.label
                                     : labelAlong(w.start, {0.5f * (w.start.x + w.end.x), 0.5f * (w.start.y + w.end.y)},
                                                  w.end, w.label, 40.0f);
                labelWidgets(Kind::OutLabel, outParam(node), -1, chip(at, 40.0f), node, kNodes);
            }
        // The arrowheads, over the labels: dragged onto another node or the bus, they move their routes there.
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (from == to || !wired(param) || param == moving)
                    continue;
                const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(param));
                add(Kind::WireEnd, wireEndGrip(w.end, w.tangent), param, from, to);
            }
            if (goesOut(from) && outParam(from) != moving)
                add(Kind::WireEnd, wireEndGrip(outWireOf(from, nodeCenter(from)).end, {0.0f, 1.0f}), outParam(from),
                    from, kNodes);
        }
        // Where the wires leave their nodes, over the nodes' edges: dragged onto another node, they come from there.
        auto startGrip = [](Point start, Point toward) {
            return wireStartGrip(start, normalize({toward.x - start.x, toward.y - start.y}));
        };
        for (int from : on) {
            for (int to : on) {
                const int param = routeParam(from, to);
                if (from == to || !wired(param) || param == moving)
                    continue;
                const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(param));
                add(Kind::WireStart, startGrip(w.start, w.control), param, from, to);
            }
            if (goesOut(from) && outParam(from) != moving) {
                const OutWire w = outWireOf(from, nodeCenter(from));
                add(Kind::WireStart, startGrip(w.start, w.end), outParam(from), from, kNodes);
            }
        }
        // The node under the mouse: an x in a circle at its upper right, over its edge, which switches it off.
        for (int node : on)
            if (node == routingHoveredNode()) {
                const Point x = nodeDeleteAt(node, nodeCenter(node));
                add(Kind::NodeDelete, {x.x - 8.0f, x.y - 8.0f, 16.0f, 16.0f}, -1, node);
            }
        // The nodes that are off, as dashed ghosts in the graph's top left, over any node moved there.
        float x = kRouting.x + 16.0f;
        for (int node = 0; node < kNodes; ++node) {
            if (nodeOn(node))
                continue;
            const float w = isBoxNode(node) ? 34.0f : 24.0f;
            add(Kind::NodeGhost, {x, kRouting.y + 10.0f, w, 24.0f}, -1, node);
            x += w + 6.0f;
        }
    }

    // Inspector: tabs, then the selected operator, filter or shaper. The FX view covers it.
    const size_t inspectorStart = list.size();
    const int node = controller_.layout().selectedNode;
    const float tabsX = kInspector.x + 16.0f;
    for (int i = 0; i < kNodes; ++i) // the operators', the filters' and the shapers', a little apart
        add(Kind::NodeTab,
            {tabsX + i * 25.0f + (isBoxNode(i) ? 6.0f : 0.0f) + (isShaperNode(i) ? 6.0f : 0.0f), kInspector.y + 9.0f,
             24.0f, 22.0f},
            -1, i);
    add(Kind::Toggle, {kInspector.right() - 16.0f - 26.0f, kInspector.y + 37.0f, 26.0f, 19.0f}, enabledParam(node));
    auto knobRow = [&](const int (&params)[5], const char *const (&labels)[5]) {
        for (int i = 0; i < 5; ++i)
            if (params[i] >= 0)
                knob(params[i], {kInspector.x + 34.0f + i * 58.0f, kInspector.y + 110.0f}, 34.0f, labels[i]);
    };
    auto opKnobRow = [&](const int (&params)[6], const char *const (&labels)[6]) {
        const float step = (kInspector.w - 32.0f) / 6.0f;
        for (int i = 0; i < 6; ++i)
            if (params[i] >= 0)
                knob(params[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 110.0f}, 32.0f, labels[i]);
    };
    const Str envLabels[] = {Str::LabelAttack, Str::LabelHold, Str::LabelDecay, Str::LabelSustain, Str::LabelRelease};
    // What feeds a filter or shaper, from the other nodes that are on.
    auto inputKnobs = [&](int into) {
        std::vector<int> sources;
        for (int from = 0; from < kNodes; ++from)
            if (from != into && nodeOn(from))
                sources.push_back(from);
        const float spacing = (kInspector.w - 32.0f) / std::max<size_t>(sources.size(), 6);
        for (size_t i = 0; i < sources.size(); ++i)
            knob(routeParam(sources[i], into), {kInspector.x + 16.0f + spacing * (i + 0.5f), kInspector.y + 314.0f},
                 28.0f, kNodeNames[sources[i]]);
    };
    if (isShaperNode(node)) {
        // The curve's list and the symmetric and link switches, then drive, output, mix and what goes straight out;
        // a drawn curve is edited in the display unless its big editor has it.
        const int shaper = shaperOfNode(node);
        const bool custom = clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom;
        add(Kind::ListButton, shaperModeButton(custom), paramIndex(shaper, ShaperParam::Mode));
        const size_t switches = list.size();
        if (custom)
            add(Kind::Toggle, kShaperSymButton, paramIndex(shaper, ShaperParam::Symmetric));
        add(Kind::Toggle, kShaperLinkButton, paramIndex(shaper, ShaperParam::Link));
        merge(switches);
        const int params[] = {paramIndex(shaper, ShaperParam::Drive), paramIndex(shaper, ShaperParam::Output),
                              paramIndex(shaper, ShaperParam::Mix), paramIndex(shaper, ShaperParam::Out)};
        const Str labels[] = {Str::LabelDrive, Str::LabelOutput, Str::LabelMix, Str::LabelToOut};
        const float step = (kInspector.w - 32.0f) / 4.0f;
        for (int i = 0; i < 4; ++i)
            knob(params[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 110.0f}, 34.0f, tr(labels[i]));
        if (clipMode(value(paramIndex(shaper, ShaperParam::Mode))) == ClipMode::Custom &&
            shapeEditing() != shaperCurve(shaper))
            drawnCurve(kFxSlots + shaper, kShaperDisplay);
        inputKnobs(node);
    } else if (node < kOps) {
        const int op = node;
        const size_t waves = list.size();
        for (int b = 0; b + 1 < kWaveButtons; ++b)
            add(Kind::WaveButton, waveButton(b), -1, op, static_cast<int>(kCommonWaves[b]));
        add(Kind::WaveMore, waveButton(kWaveButtons - 1), -1, op);
        merge(waves);
        // The tabs, a noise operator's without the scope, then what the shown one holds.
        const InspectorTab tab = opTabShown(op);
        for (int t = 0; t < static_cast<int>(InspectorTab::Count); ++t)
            if (!isNoise(op) || static_cast<InspectorTab>(t) != InspectorTab::Scope)
                add(Kind::OpTab, opTab(t), -1, t);
        if (tab == InspectorTab::Scope)
            for (int k = 0; k < 3; ++k)
                add(Kind::ScopeCycles, scopeCycleButton(k), -1, op, kScopeCycleChoices[k]);
        if (isNoise(op)) {
            // Its slope and stereo spread take the place of the pitch knobs; the unison row shows the spectrum. Noise
            // has no cycle to start, so no Phase.
            const int params[] = {paramIndex(op, OpParam::NoiseSlope), paramIndex(op, OpParam::UnisonSpread), -1, -1,
                                  -1, paramIndex(op, OpParam::Pan)};
            const char *const labels[] = {tr(Str::LabelSlope), tr(Str::LabelSpread), nullptr,
                                          nullptr,             nullptr,              tr(Str::LabelPan)};
            opKnobRow(params, labels);
        } else {
            // The Ratio readout takes the first two places; fixed, Freq and Mult take its place, and Pitch and Detune,
            // which it ignores, are dimmed.
            const bool fixed = isFixed(op);
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
            opKnobRow(params, labels);
            for (size_t i = row; i < list.size(); ++i)
                list[i].disabled = fixed && (list[i].param == params[2] || list[i].param == params[3]);
            if (!fixed) {
                add(Kind::Knob, {kRatioBox.x, kRatioBox.y, kRatioBox.w, kRatioBox.h + 19.0f},
                    paramIndex(op, OpParam::Ratio));
                list.back().label = tr(Str::LabelRatio);
            }
            add(Kind::Toggle, kFixedSwitch, paramIndex(op, OpParam::Fixed));
            if (isCustomWave(op))
                add(Kind::PartialEdit, kPartialEditButton, -1, op);
            const int unison[] = {paramIndex(op, OpParam::Unison), paramIndex(op, OpParam::UnisonDetune),
                                  paramIndex(op, OpParam::UnisonSpread), paramIndex(op, OpParam::PhaseRandom)};
            const Str unisonLabels[] = {Str::LabelUnison, Str::LabelUniDetune, Str::LabelUniSpread, Str::LabelRndPhase};
            const float step = (kInspector.w - 32.0f) / 4.0f;
            if (tab == InspectorTab::Unison)
                for (int i = 0; i < 4; ++i)
                    knob(unison[i], {kInspector.x + 16.0f + step * (i + 0.5f), kUnisonKnobsY}, 34.0f,
                         tr(unisonLabels[i]));
        }
        if (tab == InspectorTab::Inputs) {
            // A row per route into it: the name shows its node, the buttons pick its mode, the label sets its
            // depth as a wire's does. Its feedback comes last, always Phase.
            const std::vector<int> inputs = inputsOf(op);
            const bool feedback = wired(routeParam(op, op));
            const float step = inputsStep(static_cast<int>(inputs.size()) + (feedback ? 1 : 0));
            float y = kInputsTop;
            for (int from : inputs) {
                add(Kind::MatrixLabel, inputsName(y), -1, from);
                const size_t buttons = list.size();
                for (int m = 0; m < static_cast<int>(RouteMode::Count); ++m)
                    add(Kind::Choice, inputsMode(y, m), routeModeParam(from, op), from, m);
                merge(buttons);
                add(Kind::WireLabel, inputsDepth(y), routeParam(from, op), from, op);
                y += step;
            }
            if (feedback)
                add(Kind::WireLabel, inputsDepth(y), routeParam(op, op), op, op);
        }
        if (tab == InspectorTab::Envelope) {
            const EnvIndices e = envIndices(op);
            const int ahdsr[] = {e.attack, e.hold, e.decay, e.sustain, e.release};
            const float step = (kInspector.w - 32.0f) / 5.0f;
            for (int i = 0; i < 5; ++i)
                knob(ahdsr[i], {kInspector.x + 16.0f + step * (i + 0.5f), kInspector.y + 322.0f}, 30.0f,
                     tr(envLabels[i]));
        }
    } else {
        const int filter = node - kOps;
        const size_t types = list.size();
        for (int t = 0; t < static_cast<int>(FilterType::Count); ++t)
            add(Kind::TypeButton, filterTypeButton(t), -1, filter, t);
        merge(types);
        add(Kind::ListButton, kFilterSlopeButton, paramIndex(filter, FilterParam::Slope));
        const int params[] = {paramIndex(filter, FilterParam::Cutoff), paramIndex(filter, FilterParam::Resonance),
                              paramIndex(filter, FilterParam::KeyTrack), paramIndex(filter, FilterParam::Drive),
                              paramIndex(filter, FilterParam::Out)};
        const char *const labels[] = {tr(Str::LabelCutoff), tr(Str::LabelResonance), tr(Str::LabelKeyTrack),
                                      tr(Str::LabelDrive), tr(Str::LabelOut)};
        knobRow(params, labels);
        const Point handle = filterHandle(kFilterDisplay, value(params[0]), value(params[1]));
        add(Kind::FilterPoint, {handle.x - 8.0f, handle.y - 8.0f, 16.0f, 16.0f}, -1, filter);
        inputKnobs(node);
    }

    const bool wide = inspectorCovered();
    if (wide)
        list.resize(inspectorStart);

    // Envelope handles, for the selected operator and the mod envelopes on screen.
    std::vector<int> envs;
    // Cards partly scrolled out are there too, cut off at the row's ends: (position, screen place).
    const std::vector<int> cardSources = sourcesShown();
    const bool collapsed = controller_.layout().sourcesCollapsed;
    std::vector<std::pair<int, float>> cards;
    for (int position = 0; position < static_cast<int>(cardSources.size()) && !collapsed; ++position)
        if (const float place = sourceOffset(cardSources, position) - sourceScroll();
            onScreen(place, kVisibleCards, sourceSpan(cardSources[position])))
            cards.push_back({position, place});
    for (const auto &[position, card] : cards) // a folded card shows no envelope
        if (sourceTypeOf(cardSources[position]) == SourceType::Env && !sourceFolded(cardSources[position]))
            envs.push_back(kOps + cardSources[position]);
    if (node < kOps && !wide && opTabShown(node) == InspectorTab::Envelope)
        envs.push_back(node);
    for (int env : envs) {
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

    // Modulation panel: the collapse toggle, the add buttons, the scrollbar, then the cards.
    add(Kind::SourcesToggle, sourcesToggle());
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
        const bool lfo = sourceTypeOf(s) == SourceType::Lfo;
        const bool widened = s == wideLfo();
        const bool widens = lfo && !widened && std::lround(value(paramIndex(s, SourceParam::Shape))) ==
                                                   static_cast<long>(LfoShape::Custom);
        const float gripEnd = widened ? lfoCardButton(card, 1).x - 4.0f // short of the reset, leftmost
                              : widens ? lfoWidenButton(card).x - 4.0f
                                       : cardTitleRight(p);
        add(Kind::SourceGrip, {p.x, p.y, gripEnd - p.x, 34.0f}, -1, s, position);
        add(Kind::SourceFold, cardFold(p), -1, s);
        add(Kind::SourceHandle, sourceHandleRect(p), -1, s);
        add(Kind::Toggle, sourcePower(card, span), sourceEnabledIndex(s));
        add(Kind::SourceRemove, sourceRemove(card, span), -1, s);
        if (widens)
            add(Kind::ShapeWiden, lfoWidenButton(card), -1, s);
        if (widened) {
            // Its shape in the card: the title row's buttons, the tools and modes where a normal card's display is,
            // the grid's steppers under the plot, and the plot with its points; greyed out, taking no input and
            // without its points, while the full-size editor has the shape.
            const bool elsewhere = shapeEditing() == s;
            const size_t editorStart = list.size();
            add(Kind::ShapeCollapse, lfoCardButton(card, 0), -1, s);
            add(Kind::ShapeReset, lfoCardButton(card, 1), -1, s);
            add(Kind::ShapeExpand, lfoCardButton(card, 2), -1, s);
            for (int tool = 0; tool < kShapeTools; ++tool)
                add(Kind::ShapeTool, lfoToolButton(card, tool), -1, s, tool);
            for (int mode = 0; mode < 3; ++mode) {
                add(Kind::ShapeMode, lfoModeButton(card, mode), -1, s, mode);
                if (mode == 1) // Points | Steps in one pill
                    merge(list.size() - 2);
            }
            for (int axis = 0; axis < 2; ++axis)
                for (int part = 0; part < 3; ++part)
                    add(Kind::ShapeGrid, lfoCardGridButton(card, axis, part), -1, axis, part - 1);
            add(Kind::ShapeArea, lfoCardPlot(card).inset(-8.0f), -1, s);
            for (size_t i = editorStart; i < list.size(); ++i)
                list[i].disabled = elsewhere;
            if (!elsewhere) {
                const int count = shapePointCount(s);
                for (int point = 0; point < count; ++point) {
                    const double next = point + 1 < count ? shapePointX(s, point + 1) : 1.0;
                    if (next - shapePointX(s, point) < 1e-6)
                        continue; // a jump straight up or down has nothing to bend
                    const Point c = shapeCurvePos(s, point);
                    add(Kind::ShapeCurve, {c.x - 5.0f, c.y - 5.0f, 10.0f, 10.0f}, shapeIndex(s, point, PointField::Curve),
                        s, point);
                    list.back().compact = true;
                }
                for (int point = 0; point < count; ++point) {
                    const Point c = shapePointPos(s, point);
                    add(Kind::ShapePoint, {c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f}, -1, s, point);
                    list.back().compact = true;
                }
            }
        }
        if (lfo) {
            add(Kind::ListButton, lfoShapeButton(card), paramIndex(s, SourceParam::Shape));
            if (listMenu_ == paramIndex(s, SourceParam::Shape))
                lfoList = lfoShapeButton(card);
            const bool random =
                std::lround(value(paramIndex(s, SourceParam::Shape))) == static_cast<long>(LfoShape::Random);
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
            const size_t modes = list.size();
            for (int i = 0; i < kLfoModes; ++i)
                add(Kind::Choice, lfoModeSwitch(card, i), paramIndex(s, SourceParam::Free), s, i);
            merge(modes);
            add(Kind::Toggle, lfoUnipolarSwitch(card), paramIndex(s, SourceParam::Unipolar));
            const bool synced = value(paramIndex(s, SourceParam::Sync)) >= 0.5;
            knob(synced ? paramIndex(s, SourceParam::Division) : paramIndex(s, SourceParam::Rate),
                 lfoKnob(card, 0), 30.0f, synced ? nullptr : tr(Str::LabelRate));
            knob(paramIndex(s, SourceParam::Phase), lfoKnob(card, 1), 30.0f, tr(Str::LabelPhase));
        } else if (sourceTypeOf(s) == SourceType::Env) {
            const EnvIndices e = envIndices(kOps + s);
            const int params[] = {e.attack, e.hold, e.decay, e.sustain, e.release};
            const float step = (p.w - 24.0f) / 5.0f;
            for (int i = 0; i < 5; ++i)
                knob(params[i], {p.x + 12.0f + step * (i + 0.5f), p.y + 54.0f}, 32.0f, tr(envLabels[i]));
        } else if (sourceTypeOf(s) == SourceType::Midi) {
            // Its input's list button, where an LFO's shape is, and right of it the controller (while that is its
            // input) and Slew; Random has none. Its list opens as an LFO's shapes do.
            const int input = paramIndex(s, SourceParam::MidiInput);
            add(Kind::ListButton, midiInputButton(p), input);
            if (listMenu_ == input)
                lfoList = midiInputButton(p);
            if (std::lround(value(input)) == static_cast<long>(MidiInput::Cc))
                knob(paramIndex(s, SourceParam::CcNumber), {p.x + 172.0f, p.y + 54.0f}, 32.0f, tr(Str::LabelCc));
            knob(paramIndex(s, SourceParam::Slew), {p.x + 228.0f, p.y + 54.0f}, 32.0f, tr(Str::LabelSlew));
        } else if (sourceTypeOf(s) == SourceType::Random) {
            // Unipolar | Bipolar under what it does, one of them lit, as an LFO's clock modes are.
            const size_t polarity = list.size();
            for (int bipolar = 0; bipolar < 2; ++bipolar)
                add(Kind::Choice, randomPolaritySwitch(card, bipolar), paramIndex(s, SourceParam::Bipolar), s, bipolar);
            merge(polarity);
        }
        // Its modulations are listed in the MODS view, which the button under its display shows.
        add(Kind::SourceMods, sourceModsButton(card), -1, s);
    }
    if (const auto shadow = sourceShadowCard())
        add(Kind::SourceShadow, *shadow);
    for (size_t i = sourceCardsStart; i < list.size(); ++i)
        if (list[i].clip.w <= 0.0f)
            list[i].clip = sourceCardClip();

    // Depth handles in the gap under each modulated knob, and small ones in the bottom right corner of each
    // modulated wire label and the top right of each matrix cell and source's depth bar (another source modulating
    // its depth). As many as
    // the knob's, label's or cell's width holds; past that, as many as fit beside a "+n" pill holding the rest. The
    // handles and pills come in the user's size; they stay as far apart at each.
    const ModHandleSize &sizes = modHandleSize(controller_.handleSize());
    const size_t count = list.size();
    std::vector<Assignment> here;
    for (size_t i = 0; i < count; ++i) {
        const Kind kind = list[i].kind;
        if (kind != Kind::Knob && kind != Kind::WireLabel && kind != Kind::OutLabel && kind != Kind::GridCell &&
            kind != Kind::SlotBar)
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
        if (kind != Kind::Knob) {
            // Leftwards from the corner, 8 px apart on a label and 7 on a cell or depth bar, no further out than
            // stays inside it at the user's size; past what fits, as many as leave room for the pill. A wire's or
            // OUT label in the graph has them centered just under it and its ranges on its outline's top, so its
            // depth stays readable; on the operator page's INPUTS tab, along its bottom, its ranges along its top. A
            // cell and a depth bar have them in their top right corner, their ranges along their bottom.
            const bool cell = kind == Kind::GridCell || kind == Kind::SlotBar;
            const float margin = std::max(cell ? 4.0f : 3.0f, sizes.dotCompact + 1.0f), step = cell ? 7.0f : 8.0f,
                        y = cell ? r.y + 4.0f : list[i].graph ? r.bottom() + 0.75f + sizes.dotCompact : r.bottom() - 2.0f;
            const int fit = static_cast<int>((r.w - 2.0f * margin) / step) + 1;
            int shown = n;
            float pillW = 0.0f;
            if (n > fit) {
                // The pill's left edge stays 1 px inside: 4.25 px left of the last handle shown, or at the corner.
                pillW = n - std::max(fit - 2, 0) >= 10 ? sizes.pillWideCompact : sizes.pillCompact;
                const float room = r.w - margin - 4.25f - pillW - 1.0f;
                shown = room < 0.0f ? 0 : std::min(static_cast<int>(std::floor(room / step)) + 1, n - 1);
            }
            // The first handle's center; in the graph, the handles and pill as a group centered under the label.
            float first = r.right() - margin;
            if (list[i].graph) {
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
        const Point base = isRatioDisplay(list[i]) ? Point{r.center().x, kRatioBox.bottom() - 1.0f}
                           : list[i].compact       ? Point{r.x + 18.0f, r.y + 37.0f}
                                                   : Point{r.center().x, r.y + r.w - 1.0f};
        // The Ratio readout keeps its handles clear of its box's rounded corners.
        constexpr float kStep = 10.0f;
        const float width = isRatioDisplay(list[i]) ? r.w - 9.0f : list[i].compact ? 34.0f : r.w;
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

    // The macros: their knobs, the names under them and their handles.
    const float macros = keyboardTop(controller_.layout().sourcesCollapsed);
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

    // Keyboard strip. The white keys come first, so the black keys on top win the hits. Collapsed, only its top row
    // stays, where the mod wheel's handle is anyway.
    const float keys = keyboardTop(controller_.layout().sourcesCollapsed);
    const bool keysCollapsed = controller_.layout().keysCollapsed;
    add(Kind::KeysToggle, keysToggle(keys));
    add(Kind::MacroHandle, wheelHandle(keys), -1, kWheelSource);
    // The bend range, over the bend wheel, as a header knob: within the top row, so it stays while the keys are folded.
    add(Kind::Knob, bendKnob(keys), paramIndex(GlobalParam::BendRange));
    list.back().label = tr(Str::LabelBend);
    list.back().compact = true;
    list.back().shrunk = true;
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
    if (controller_.hasTempo())
        add(Kind::TempoBox, kTempoBox);
    add(Kind::PresetStep, presetStep(-1), -1, -1);
    add(Kind::PresetNameBox, kPresetName);
    add(Kind::PresetStep, presetStep(1), -1, 1);
    for (int action = 0; action < 4; ++action)
        add(Kind::PresetButton, toolbarButton(action), -1, action);
    add(Kind::SettingsButton, toolbarButton(4));

    // The list of source types, under + SOURCE or where the dashed card was clicked, while that is on screen.
    if (sourceMenuOpen_ && sourceMenuShown()) {
        const Point at = sourceMenuTopLeft();
        add(Kind::SourceMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::SourceMenu, sourceMenu(at), -1, 1);
        for (int t = 1; t <= kSourceTypes; ++t)
            add(Kind::SourceMenuItem, sourceMenuItem(at, static_cast<SourceType>(t)), -1, t);
    }
    if (fxMenuOpen_ && fxWide()) {
        // Opened for the main line (from a + or a wire of the graph, from the row's arrows, its dashed card or ADD
        // EFFECT), it offers a split (with its join) after the effects; for a band, effects only.
        const bool withNode = fxSlot_.lane == 0;
        add(Kind::FxMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::FxMenu, fxMenu(fxMenuAt_, withNode), -1, 1);
        for (FxType type : kFxOffered)
            add(Kind::FxAdd, fxMenuItem(fxMenuAt_, type), -1, static_cast<int>(type));
        if (withNode)
            for (int bands : {2, 3})
                add(Kind::FxAddNode, fxMenuNodeItem(fxMenuAt_, bands), -1, -1, bands);
    }
    // The open list of an effect's choice (the shaper's curve, the reverb's algorithm), under its button,
    // while that card is on screen and still holds that effect.
    const FxParam listField = fxFieldOf(listMenu_);
    const int listSlot = fxSlotOf(listMenu_);
    if (listSlot >= 0 && fxWide() && fxTypeOfField(listField) == fxSlotType(listSlot) &&
        (fxGraph() || !fxFolded(listSlot))) {
        if (const std::optional<Rect> shown = fxCardOf(listSlot)) {
            const Rect card = *shown;
            const Rect button = listField == FxParam::ReverbType    ? reverbTypeButton(card)
                                : listField == FxParam::FilterSlope ? slopeButton(fxDisplayIn(card, FxType::Filter))
                                : listField == FxParam::AmpModel    ? ampModelButton(card)
                                : listField == FxParam::AmpCab      ? ampCabButton(card)
                                                                    : clipModeButton(card);
            const int items = listCount(listMenu_);
            add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::ListMenu, listMenu(button, items), -1, 1);
            for (int item = 0; item < items; ++item)
                add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
        }
    }
    // A split's list of slopes, under its button on its card (in the row's container or the graph's inspector).
    if (const int split = splitOf(listMenu_); split >= 0 && isSplitSlope(listMenu_) && fxWide()) {
        if (const std::optional<Rect> shown = splitCardOf(split)) {
            const Rect button = splitSlopeButton(splitDisplay(*shown, splitThree(split)));
            const int items = listCount(listMenu_);
            add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
            add(Kind::ListMenu, listMenu(button, items), -1, 1);
            for (int item = 0; item < items; ++item)
                add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
        }
    }
    // An LFO's list of shapes, under its button on its card, cut off at the row's ends like the card, though
    // reaching past its bottom.
    if (lfoList.w > 0.0f) {
        const int items = listCount(listMenu_);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::ListMenu, listMenu(lfoList, items), -1, 1);
        list.back().clip = sourceListClip();
        for (int item = 0; item < items; ++item) {
            add(Kind::ListItem, listMenuItem(lfoList, items, item), listMenu_, -1, item);
            list.back().clip = sourceListClip();
        }
    }
    // A shaper's list of curves or a filter's slopes, under its button on its page.
    const int pageNode = controller_.layout().selectedNode;
    const bool shaperList = shaperOf(listMenu_) >= 0 && pageNode == kOps + kFilters + shaperOf(listMenu_);
    const bool slopeList =
        listMenu_ >= 0 && isFilterNode(pageNode) && listMenu_ == paramIndex(pageNode - kOps, FilterParam::Slope);
    if ((shaperList || slopeList) && !inspectorCovered()) {
        const Rect button =
            slopeList ? kFilterSlopeButton : shaperModeButton(clipMode(value(listMenu_)) == ClipMode::Custom);
        const int items = listCount(listMenu_);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::ListMenu, listMenu(button, items), -1, 1);
        for (int item = 0; item < items; ++item)
            add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
    }
    // A route's list of modes, under the mode's part of its label's pill, while the graph shows that route.
    if (const int from = routeModeFrom(listMenu_), to = routeModeTo(listMenu_);
        from >= 0 && controller_.layout().view == RoutingView::Graph && shapeEditing() < 0 && partialEditing() < 0 &&
        nodeOn(from) && nodeOn(to) && wired(routeParam(from, to)) && rerouting() != routeParam(from, to)) {
        const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(routeParam(from, to)));
        const Rect part =
            labelPill(routeParam(from, to), listMenu_, chip(labelAlong(w.start, w.control, w.end, w.label, 38.0f))).mode;
        const Rect button{part.x, part.y, 112.0f, part.h};
        const int items = listCount(listMenu_);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::ListMenu, listMenu(button, items), -1, 1);
        for (int item = 0; item < items; ++item)
            add(Kind::ListItem, listMenuItem(button, items, item), listMenu_, -1, item);
    }
    // The open list of a parametric EQ band's shapes, above its button, while that card is on screen.
    if (peqMenu_ >= 0 && fxWide() && fxSlotType(fxSlotOf(peqMenu_)) == FxType::ParamEq) {
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
    // The open list of more waves, under "...", while its operator shows in the inspector.
    if (waveMenu_ >= 0 && waveMenu_ == controller_.layout().selectedNode && !inspectorCovered()) {
        add(Kind::WaveMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
        add(Kind::WaveMenu, waveMenu(), -1, 1);
        for (size_t i = 0; i < std::size(kMoreWaves); ++i)
            add(Kind::WaveMenuItem, waveMenuItem(static_cast<int>(i)), -1, waveMenu_, static_cast<int>(kMoreWaves[i]));
    }
    // A MODS row's cascading menu, over everything: around it, a click closes it.
    if (cascade_ && modsWide()) {
        const std::shared_ptr<const MenuNode> tree = cascadeTree();
        const MenuNode &root = *tree;
        const std::vector<const MenuNode *> levels = cascadeLevels(root);
        const std::vector<Rect> rects = cascadeRects(root);
        add(Kind::CascadeMenu, {0.0f, 0.0f, View::kWidth, height()}, -1, 0);
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
        for (size_t k = 0; k < rows.size(); ++k) {
            const int s = rows[k].source, slot = rows[k].slot;
            const size_t first = list.size();
            const Rect r{panel.x + kModListPad, panel.y + kModListTitleH + static_cast<float>(k) * kModListRowStep,
                         kCardW - 24.0f + shift, 20.0f};
            add(Kind::ModListRow, r, -1, s, slot);
            add(Kind::SlotBypass, {r.x + 1.0f, r.y, 16.0f, r.h}, slotBypassIndex(s, slot), s, slot);
            if (auxOf(rows[k]) >= 0) { // the x at the end of its chip (see paintModListRow())
                const Rect chip = modListAuxChip(r);
                add(Kind::ModsAuxClear, {chip.right() - 20.0f, chip.y + 2.0f, 18.0f, chip.h - 4.0f}, -1, s, slot);
            }
            add(Kind::SlotBar, {r.x + 116.0f + shift, r.y, 76.0f, r.h}, slotAmountIndex(s, slot), s, slot);
            add(Kind::ValueText, {r.x + 194.0f + shift, r.y, 38.0f, r.h}, slotAmountIndex(s, slot), s, slot);
            add(Kind::SlotRemove, {r.right() - 20.0f, r.y, 20.0f, r.h}, -1, s, slot);
            for (size_t i = first; i < list.size(); ++i)
                list[i].popup = true;
        }
    }
    return list;
}

std::optional<View::Widget> View::hitTest(Point p) const {
    const DerivedScope scope(*this); // asked while painting, the widgets painted
    const bool overlay = settingsOpen_ || confirm_ >= 0 || browserOpen_;
    const std::vector<Widget> dialog = settingsOpen_   ? settingsWidgets()
                                       : confirm_ >= 0 ? confirmWidgets()
                                       : browserOpen_  ? browserWidgets()
                                                       : std::vector<Widget>();
    const std::vector<Widget> &list = overlay ? dialog : scopedWidgets();
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        if ((it->clip.w > 0.0f && !it->clip.contains(p)) || it->disabled)
            continue;
        if (it->kind == Kind::Node && !isBoxNode(it->a)) {
            const Point c = it->rect.center();
            if (std::hypot(p.x - c.x, p.y - c.y) <= kNodeRadius)
                return *it;
        } else if (it->kind == Kind::Port) {
            if (onNodeEdge(it->a, it->rect.center(), p))
                return *it;
        } else if (it->kind == Kind::Wire) {
            const Wire w = wireBetween(it->a, nodeCenter(it->a), it->b, nodeCenter(it->b), wireBend(it->param));
            if (it->rect.contains(p) && distanceToWire(w, p) <= kWireGrab)
                return *it;
        } else if (it->kind == Kind::FxWire) {
            if (it->rect.contains(p) && fxWireHit(*it, p))
                return *it;
        } else if (it->rect.contains(p)) {
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
    const bool panelsShown = touches({0.0f, kHeaderH, kWidth, lowerTop - kHeaderH});
    const bool lowerShown = touches({0.0f, lowerTop, kWidth, whole.bottom() - lowerTop});
    constexpr float kWidgetReach = 64.0f;
    if (partial)
        c.pushClip(area);
    const DerivedScope scope(*this);
    const std::vector<Widget> &list = scopedWidgets();
    std::fill(macroOffsets_.begin(), macroOffsets_.end(), 0.0f);
    macroSlotOffsets_.fill(0.0f);
    forEachMacroMod([this](int i) { return value(i); }, [this](int target, double offset, int source, int slot) {
        macroOffsets_[target] += static_cast<float>(offset);
        macroSlotOffsets_[source * kSlots + slot] =
            static_cast<float>(offset / (table_[target].max - table_[target].min));
    });

    theme_ = themeIndex(controller_.theme());
    look = kThemes[theme_];
    glow_ = glowing = shownGlow();
    links_ = shownLinks();
    handles_ = controller_.handleSize();
    if (partial)
        c.fillRect(area, kBackground);
    else
        c.clear(kBackground);
    paintPanels(c);
    if (headerShown)
        paintHeader(c);
    if (shapeEditing() >= 0)
        paintShapeEditor(c, shapeEditing());
    else if (partialEditing() >= 0)
        paintPartialEditor(c, partialEditing());
    else if (panelsShown) switch (controller_.layout().view) {
    case RoutingView::Graph:
        paintRouting(c);
        break;
    case RoutingView::Matrix:
        paintMatrixLabels(c);
        break;
    case RoutingView::Fx:
        paintFx(c);
        break;
    case RoutingView::Mods:
        paintMods(c);
        break;
    }
    if (!inspectorCovered() && panelsShown)
        paintInspector(c);
    if (lowerShown) {
        paintKeyboard(c);
        paintNoteSource(c, keyboardTop(controller_.layout().sourcesCollapsed));
        paintVelocitySource(c, keyboardTop(controller_.layout().sourcesCollapsed));
        paintSources(c);
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
            paintWidget(c, w);
            if (w.disabled || w.faint) // dimmed as a switched-off node's page is
                c.fillRect(w.rect.inset(-1.0f), (w.faint ? kCardColor : kPanel).alpha(0.6f), kRadiusMark);
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
    // A switched-off source's card is dimmed below its title row (a folded one's name is already dim).
    c.pushClip(sourceCardClip());
    for (int s : sources()) {
        const float card = cardOf(s);
        if (card == kOffScreen || sourceOn(s) || sourceFolded(s))
            continue;
        const Rect p = sourceCard(card, sourceSpan(s));
        c.fillRect({p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}, kPanel.alpha(0.6f), kRadiusCard - 1.0f);
    }
    c.popClip();
    // So is a switched-off effect's.
    if (shapeEditing() < 0 && controller_.layout().view == RoutingView::Fx && fxGraph()) {
        if (const int node = fxSelectedNode(); node >= 0 && node < kFxSlots &&
                                               value(paramIndex(node, FxParam::Enabled)) < 0.5) {
            const Rect p = fxInspector(node);
            c.fillRect({p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}, kCardColor.alpha(0.6f), kRadiusCard - 1.0f);
        }
    } else if (shapeEditing() < 0 && controller_.layout().view == RoutingView::Fx) {
        c.pushClip(fxCardClip());
        const FxRow row = fxRowShown();
        for (const FxRowCard &card : row.cards) {
            const float pos = card.place - fxScroll();
            if (!onScreen(pos, kVisibleFx, card.span) || value(paramIndex(card.slot, FxParam::Enabled)) >= 0.5 ||
                fxFolded(card.slot))
                continue;
            const Rect p = fxCard(pos, card.span);
            c.fillRect({p.x + 1.0f, p.y + 36.0f, p.w - 2.0f, p.h - 37.0f}, kCardColor.alpha(0.6f), kRadiusCard - 1.0f);
        }
        // The card or container carried, faint where it would land until it is let go of; and the cards of a band a
        // container shows that its join doesn't let through, the other band soloed.
        const FxLine line = fxLineShown();
        for (const FxRowCard &card : row.cards) {
            const float pos = card.place - fxScroll();
            if (!onScreen(pos, kVisibleFx, card.span))
                continue;
            if (card.faint)
                c.fillRect(fxCard(pos, card.span).inset(-1.0f), kPanel.alpha(0.65f), kRadiusCard + 1.0f);
            else if (card.at.lane != 0 && card.at.item < static_cast<int>(line.size()) &&
                     fxBandSoloedOut(line[card.at.item], card.at.lane))
                c.fillRect(fxCard(pos, card.span).inset(-1.0f), kPanel.alpha(1.0f - kSoloedOutAlpha), kRadiusCard + 1.0f);
        }
        for (const FxRowBox &box : row.boxes) {
            if (!box.faint || !onScreen(box.place - fxScroll(), kVisibleFx, box.span))
                continue;
            if (box.folded)
                c.fillRect(fxCard(box.place - fxScroll(), box.span).inset(-1.0f), kPanel.alpha(0.65f), kRadiusCard + 1.0f);
            else
                c.fillRect(fxCard(boxColumn(box) - fxScroll(), box.span - 2.0f * kFxBoxEdge).inset(-kFxBoxPad - 1.0f),
                           kPanel.alpha(0.65f), kFxBoxRadius + 1.0f);
        }
        c.popClip();
    }
    // And the inspector's page while its operator, filter or shaper is off, below the tabs, summary and switch.
    if (!inspectorCovered() && !nodeOn(controller_.layout().selectedNode))
        c.fillRect({kInspector.x + 1.0f, kInspector.y + 60.0f, kInspector.w - 2.0f, kInspector.h - 61.0f},
                   kPanel.alpha(0.6f), kRadiusPanel - 1.0f);
    paintWidgets(true);
    // The card being dragged: where it would land only its outline, and a faint image of it under the mouse wherever
    // it goes (over every panel, held to none), before it snaps into place: an effect card or a split's container, or
    // a source's card. The image is the card painted at its landing place, moved, with the widgets on it (not the
    // row's dashed card, which may glide through there).
    auto outlined = [&](Rect landing, Rect row, float radius, Color color) {
        c.pushClip(row);
        c.fillRect(landing.inset(-1.0f), kPanel, radius + 1.0f);
        c.strokeRect(landing, color.alpha(0.6f), 1.0f, radius);
        c.popClip();
    };
    auto carried = [&](Rect landing, Rect start, Rect row, float radius, Color color, auto paintCard) {
        outlined(landing, row, radius, color);
        const Point offset{start.x + drag_.now.x - drag_.start.x - landing.x,
                           start.y + drag_.now.y - drag_.start.y - landing.y};
        c.pushLayer(offset, 0.5f, landing.inset(-1.0f));
        c.pushClip(landing);
        paintCard();
        for (const Widget &w : list)
            if (!w.popup && w.kind != Kind::FxShadow && w.rect.x < landing.right() && w.rect.right() > landing.x && w.rect.y < landing.bottom() &&
                w.rect.bottom() > landing.y)
                paintWidget(c, w);
        c.popClip();
        c.popLayer();
    };
    if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && !fxGraph()) {
        const int node = fxDrag_->item.node;
        const FxRow row = fxRowShown();
        std::optional<Rect> landing;
        float radius = kRadiusCard;
        for (const FxRowCard &card : row.cards)
            if (card.slot == node)
                landing = fxCard(card.place - fxScroll(), card.span);
        for (const FxRowBox &box : row.boxes)
            if (fxSplitNode(box.split) == node) {
                landing = box.folded ? fxCard(box.place - fxScroll(), box.span)
                                     : fxCard(boxColumn(box) - fxScroll(), box.span - 2.0f * kFxBoxEdge)
                                           .inset(-kFxBoxPad);
                radius = box.folded ? kRadiusCard : kFxBoxRadius;
            }
        Rect start = fxCard(fxDrag_->startPlace - drag_.scrollStart, fxDrag_->span);
        if (node < kFxSlots && !fxFolded(node)) {
            // An effect card: its outline where it lands, if that shows (not on a band its container hides), and its
            // image painted afresh where it was picked up from, moved under the mouse.
            if (landing)
                outlined(*landing, fxCardClip(), radius, fxColor(node));
            // Its glow and the knobs' labels reach a little past it.
            c.pushLayer({drag_.now.x - drag_.start.x, drag_.now.y - drag_.start.y}, 0.5f, start.inset(-24.0f));
            paintFxCard(c, node, start, true);
            for (const Widget &w : carriedWidgets_)
                paintWidget(c, w);
            c.popLayer();
        } else if (landing) { // a container, or a folded card: painted at its landing place, moved
            if (node >= kFxSlots && landing->w > start.w) // a container's outline stands out round it
                start = start.inset(-kFxBoxPad);
            carried(*landing, start, fxCardClip(), radius, node < kFxSlots ? fxColor(node) : splitLine(),
                    [&] { paintFx(c, *landing); });
        }
    }
    if (drag_.kind == DragKind::SourceMove && drag_.moved) {
        const int source = drag_.widget.a;
        const std::vector<int> shown = sourcesShown();
        const auto at = std::find(shown.begin(), shown.end(), source);
        if (at != shown.end()) {
            const float place = sourceOffset(shown, static_cast<size_t>(at - shown.begin())) - sourceScroll();
            carried(sourceCard(place, sourceSpan(source)),
                    sourceCard(sourceOffset(sources(), drag_.widget.b) - drag_.scrollStart, sourceSpan(source)),
                    sourceCardClip(), kRadiusCard, kSourceColors[source], [&] { paintSource(c, place, source); });
        }
    }

    if (drag_.kind == DragKind::RerouteFrom && drag_.moved) {
        const int to = drag_.widget.b;
        // From the mouse to where it goes; away from the nodes it is faint: let go of there, it goes.
        const int source = nodeAt(drag_.now);
        const Color color = source < 0 ? kFaint : nodeColor(source);
        const Point end = to == kNodes ? Point{drag_.now.x, kBusY - 10.0f}
                                       : outlineToward(to, nodeCenter(to), drag_.now);
        const Point dir = normalize({end.x - drag_.now.x, end.y - drag_.now.y});
        const Point base{end.x - dir.x * 8.0f, end.y - dir.y * 8.0f};
        c.line(drag_.now, base, color, 2.0f, true);
        arrowHead(c, base, dir, color);
        c.fillCircle(drag_.now, 3.5f, color);
        if (source >= 0) { // a route that is there already from that node rings faintly
            const int param = routeTo(source, to);
            const Color ring = param == drag_.widget.param || !wired(param) ? color : kFaint;
            const Point center = nodeCenter(source);
            if (isBoxNode(source))
                c.strokeRect({center.x - kFilterHalfW - 5.0f, center.y - kFilterHalfH - 5.0f,
                              2.0f * kFilterHalfW + 10.0f, 2.0f * kFilterHalfH + 10.0f},
                             ring, 2.0f, kRadiusControl + 5.0f);
            else
                c.strokeCircle(center, kNodeRadius + 5.0f, ring, 2.0f);
        }
    } else if (drag_.kind == DragKind::Connect || rerouting() >= 0) {
        const int from = drag_.widget.a;
        // A moving route away from the nodes and the bus is faint: let go of there, it goes.
        const bool removing = rerouting() >= 0 && nodeAt(drag_.now) < 0 && !onBus(drag_.now);
        const Color color = removing ? kFaint : nodeColor(from);
        // The route leaves its node toward the mouse; a moving one has its arrowhead under it.
        const Point origin = nodeCenter(from);
        const Point start = outlineToward(from, origin, drag_.now);
        if (nodeAt(drag_.now) == from && drag_.leftNode) {
            // Held back over its own node, once it has been away from it, it loops back into it: let go of there,
            // the node feeds itself.
            const LoopArc loop = loopbackArc(from, origin); // on the ring that rings the node it is held over
            const float end = loop.to - 8.0f / loop.radius; // an arrowhead's length short of the outline
            std::array<Point, 33> points;
            for (size_t i = 0; i < points.size(); ++i)
                points[i] = loop.at(loop.from + (end - loop.from) * static_cast<float>(i) /
                                                    static_cast<float>(points.size() - 1));
            c.polyline(points.data(), points.size(), color, 2.0f, true);
            const Point tip = loop.at(loop.to);
            arrowHead(c, points.back(), normalize({tip.x - points.back().x, tip.y - points.back().y}), color);
        } else if (drag_.kind == DragKind::Connect) {
            if (drag_.moved)
                c.line(start, drag_.now, color, 2.0f, true);
        } else {
            const Point dir = normalize({drag_.now.x - origin.x, drag_.now.y - origin.y});
            const Point base{drag_.now.x - dir.x * 4.0f, drag_.now.y - dir.y * 4.0f};
            c.line(start, base, color, 2.0f, true);
            arrowHead(c, base, dir, color);
        }
        const int target = nodeAt(drag_.now);
        if (target >= 0) {
            const Point center = nodeCenter(target);
            // A route a moving one would find there already rings faintly.
            const int param = routeParam(from, target);
            const bool fits = drag_.kind == DragKind::Connect || param == drag_.widget.param || !wired(param);
            const Color ring = fits ? color : kFaint;
            if (isBoxNode(target))
                c.strokeRect({center.x - kFilterHalfW - 5.0f, center.y - kFilterHalfH - 5.0f,
                              2.0f * kFilterHalfW + 10.0f, 2.0f * kFilterHalfH + 10.0f},
                             ring, 2.0f, kRadiusControl + 5.0f);
            else
                c.strokeCircle(center, kNodeRadius + 5.0f, ring, 2.0f);
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
    if (cascade_ && modsWide())
        paintCascade(c);
    if (typing_)
        paintTyping(c);
    if (browserOpen_)
        paintBrowser(c);
    if (settingsOpen_)
        paintSettings(c);
    else if (confirm_ >= 0)
        paintConfirm(c);
    else
        paintTooltip(c);

    // Under the preset bar, over the panels' title rows.
    if (flashTicks_ > 0) {
        const Rect toast{View::kWidth * 0.5f - 220.0f, kPanelsTop - 2.0f, 440.0f, 28.0f};
        c.fillRect(toast, kRaised, kRadiusCard);
        c.strokeRect(toast, kBorder, 1.0f, kRadiusCard);
        c.text(toast, flash_, kTextValue, kText);
    }
    if (partial)
        c.popClip();
}

void View::paintPanels(Canvas &c) {
    const bool collapsed = controller_.layout().sourcesCollapsed;
    std::vector<Rect> panels = {modPanel(collapsed),
                                keyboardPanel(keyboardTop(collapsed), controller_.layout().keysCollapsed)};
    if (shapeEditing() >= 0 && isClipCurve(shapeEditing()))
        panels.push_back(shapePanel()); // the shaper curve's editor, narrower than the effects it covers
    else if (inspectorCovered())
        panels.push_back(kFxPanel);
    else
        panels.insert(panels.end(), {kRouting, kInspector});
    for (const Rect &p : panels) {
        c.fillRect(p, kPanel, kPanelRadius);
        c.strokeRect(p, kBorder, 1.0f, kPanelRadius);
    }
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
    const float top = keyboardTop(controller_.layout().sourcesCollapsed);
    const Rect handle = wheelHandle(top); // the mod wheel's name, under its handle
    c.text({handle.x - 30.0f, top + kStripRow + 19.0f, handle.w + 60.0f, 16.0f}, tr(Str::HeadingMod), kTextLabel, kFaint,
           Align::Center, true);
    if (controller_.layout().keysCollapsed)
        return;
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
    // Lit while held: clicked here, or played by the host.
    if (active || telemetry_.keyDown(keyboardBase() + w.a))
        color = kAccent;
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
    c.strokeRect(r, hovered || active ? kDim : kBorder, 1.0f, kRadiusCard);
    if (w.kind == Kind::ModWheel) {
        // Fills up from the bottom with the wheel's value, in the color of its handle; bipolar, from the middle, where
        // it moves nothing.
        const float y = bottom - (bottom - top) * static_cast<float>(value(w.param));
        const float from = sourceBipolar(kWheelSource) ? 0.5f * (top + bottom) : bottom;
        c.fillRect({r.x + inset, std::min(y, from), r.w - 2.0f * inset, std::abs(from - y)},
                   kSourceColors[kWheelSource].alpha(0.6f), kRadiusMark);
        if (from < bottom)
            c.line({r.x + 3.0f, from}, {r.right() - 3.0f, from}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    } else {
        // Springs back to the middle: fills up or down from the center.
        const float mid = r.y + r.h * 0.5f;
        const float y = mid - static_cast<float>(bend_) * (r.h * 0.5f - inset);
        c.fillRect({r.x + inset, std::min(mid, y), r.w - 2.0f * inset, std::abs(y - mid)}, kFilterColor.alpha(0.6f),
                   kRadiusMark);
        c.line({r.x + 3.0f, mid}, {r.right() - 3.0f, mid}, kFaint, 1.0f);
        c.line({r.x + 3.0f, y}, {r.right() - 3.0f, y}, kText, 2.0f);
    }
}

void View::paintRouting(Canvas &c) {
    c.text({kHintX, kRouting.y + 10.0f, kRouting.right() - 16.0f - kHintX, 22.0f},
           tr(Str::HintRouting), kTextLabel, kFaint, Align::Right);
    if (routingGridShown()) { // Ctrl held: the grid's points, under everything, the one a node dragged lands on lit
        const Point target = drag_.kind == DragKind::Node ? nodeCenter(drag_.widget.a) : Point{};
        paintGridMarks(c, kGraph, kRoutingGridCols, kRoutingGridRows, drag_.kind == DragKind::Node ? &target : nullptr);
    }

    // Output bus along the bottom, lit while a new or moving route would go there.
    const int moving = rerouting(); // drawn following the mouse instead
    const int busParam = drag_.kind == DragKind::Connect || moving >= 0 ? outParam(drag_.widget.a) : -1;
    const bool busTarget = busParam >= 0 && onBus(drag_.now) &&
                           (drag_.kind == DragKind::Connect || busParam == moving || !wired(busParam));
    c.line({kRouting.x + 48.0f, kBusY}, {kRouting.right() - 20.0f, kBusY}, busTarget ? kAccent : kTrack, 3.0f);
    c.text({kRouting.x + 14.0f, kBusY - 9.0f, 30.0f, 18.0f}, tr(Str::BusOut), kTextLabel, kDim, Align::Left, true);

    // Modulated routes swell and thin with their modulation while notes play.
    const std::vector<int> on = enabledNodes();
    auto focused = [&](int param) {
        return (hover_ && hover_->param == param && drag_.kind == DragKind::None) ||
               ((drag_.kind == DragKind::Value || drag_.kind == DragKind::Bend) && drag_.widget.param == param);
    };
    for (int node : on) {
        const int param = outParam(node);
        if (!goesOut(node) || param == moving)
            continue;
        const float out = static_cast<float>(liveAmount(param));
        const OutWire w = outWireOf(node, nodeCenter(node));
        const Color base = isBoxNode(node) ? nodeColor(node) : kText;
        const Color color = focused(param) ? kText : base.alpha(0.25f + 0.5f * out);
        c.line(w.start, w.end, color, 1.5f + 2.0f * out);
        arrowHead(c, w.end, {0.0f, 1.0f}, color);
    }

    for (int from : on) {
        for (int to : on) {
            const int param = routeParam(from, to);
            if (!wired(param) || param == moving)
                continue;
            const float amount = static_cast<float>(liveAmount(param));
            const Color base = routeColor(from, to);
            const Color color = focused(param) ? kText : base.alpha(0.35f + 0.65f * amount);
            const float width = 1.2f + 3.0f * amount;
            if (from == to) {
                c.strokeCircle(loopCenter(from, nodeCenter(from)), 9.0f, color, width);
                continue;
            }
            const Wire w = wireBetween(from, nodeCenter(from), to, nodeCenter(to), wireBend(param));
            c.curve(w.start, w.control, w.end, color, width);
            arrowHead(c, w.end, w.tangent, color);
        }
    }
}

// What slot `slot` of `source` adds to its target now, as a share of the target's range: a macro's or the mod wheel's
// from its knob, notes playing or not; another source's in the newest voice, while one plays. None while it modulates
// nothing: bypassed, or its source switched off.
std::optional<double> View::liveSlotOffset(int source, int slot) const {
    if (source < 0 || slot < 0 || slotBypassed(source, slot) || !sourceOn(source))
        return std::nullopt;
    const int aux = slotAux([this](int i) { return value(i); }, source, slot);
    if (!runsPerVoice(source) && (aux < 0 || !runsPerVoice(aux)))
        return macroSlotOffsets_[source * kSlots + slot];
    if (telemetry_.voices <= 0)
        return std::nullopt;
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

// How far a modulation takes its target from the set value, down and up: bipolar, half its depth either way;
// unipolar, all of it one way.
std::pair<double, double> View::modReach(const Assignment &m) const {
    const double depth = m.amount * m.scale; // as far as an aux, if it has one, lets it go
    if (m.bipolar)
        return {-0.5 * std::abs(depth), 0.5 * std::abs(depth)};
    return {std::min(depth, 0.0), std::max(depth, 0.0)};
}

bool View::sourceBipolar(int source) const {
    return fmvibe::sourceBipolar([this](int index) { return value(index); }, source);
}

double View::resetValue(int param) const {
    // A bipolar macro moves nothing at its knob's middle, so that is where it goes back to.
    if (const int macro = macroOf(param);
        macro >= 0 && param == paramIndex(macro, MacroParam::Value) && sourceBipolar(kMaxSources + macro))
        return 0.5;
    return table_[param].def;
}

void View::paintModRange(Canvas &c, Rect bar, int param) {
    const double n = norm(param);
    // The modulation whose depth handle is under the mouse goes last, twice as thick, over the others.
    auto range = [&](const Assignment &m, bool focused) {
        const auto [down, up] = modReach(m);
        const double from = std::clamp(n + down, 0.0, 1.0);
        const double to = std::clamp(n + up, 0.0, 1.0);
        const float alpha = sourceOn(m.source) && !m.bypassed ? 1.0f : (focused ? 0.4f : 0.3f);
        const float h = focused ? 2.0f * bar.h : bar.h;
        c.fillRect({bar.x + static_cast<float>(from) * bar.w, bar.center().y - 0.5f * h,
                    std::max(static_cast<float>(to - from) * bar.w, 1.5f), h},
                   kSourceColors[m.source].alpha(alpha), kRadiusMark);
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

void View::paintMatrixLabels(Canvas &c) {
    // A node that is off has its name greyed out, like its row and column of cells; the name under the mouse
    // lights up. The node the inspector shows has its names on a plate, like its tab: a click on them switches
    // it off or on again, as on the tab, and a click on another name selects that node (switching it on).
    const int selected = controller_.layout().selectedNode;
    auto nameColor = [&](int node) {
        if (hover_ && hover_->kind == Kind::MatrixLabel && hover_->a == node)
            return kText;
        const Color color = isBoxNode(node) ? nodeColor(node) : (node == selected ? kText : kDim);
        return nodeOn(node) ? color : (isBoxNode(node) ? color.alpha(0.35f) : kFaint);
    };
    const Color plate = nodeColor(selected).alpha(nodeOn(selected) ? 0.22f : 0.1f);
    const Rect first = gridCell(selected, 0), top = gridCell(0, selected); // the first cells of its row and column
    c.fillRect({first.x - 31.0f, first.y + 3.0f, 26.0f, first.h - 6.0f}, plate, kRadiusControl);
    c.fillRect({top.x - 1.0f, top.y - kMatrixNamesH, top.w + 2.0f, 18.0f}, plate, kRadiusControl);
    for (int col = 0; col < kGridCols; ++col) {
        const Rect cell = gridCell(0, col);
        const bool out = col == kGridCols - 1;
        c.text({cell.x - 6.0f, cell.y - kMatrixNamesH, cell.w + 12.0f, 18.0f},
               out ? tr(Str::BusOut) : kNodeNames[col], kTextValue, out ? kText : nameColor(col), Align::Center, true);
    }
    for (int row = 0; row < kNodes; ++row) {
        const Rect cell = gridCell(row, 0);
        c.text({cell.x - 32.0f, cell.y, 28.0f, cell.h}, kNodeNames[row], kTextValue, nameColor(row), Align::Center, true);
    }
    c.text({kLegendX, kRouting.y + 10.0f, kRouting.right() - 16.0f - kLegendX, 22.0f},
           tr(Str::HintMatrix), kTextLabel, kFaint, Align::Right);

    // Legend beside the grid.
    const float width = kRouting.right() - 16.0f - kLegendX;
    float y = kRouting.y + 72.0f;
    c.text({kLegendX, y - 22.0f, width, 16.0f}, tr(Str::HeadingHowToRead), kTextLabel, kFaint, Align::Left, true);
    struct Entry {
        Color color;
        bool outline;
        const char *text;
    };
    const Entry entries[] = {
        {kAccent, false, tr(Str::LegendFm)},
        {kAccent, true, tr(Str::LegendFeedback)},
        {kFilterColor, false, tr(Str::LegendFilter)},
        {kShaperColor, false, tr(Str::LegendShaper)},
        {kText, false, tr(Str::LegendOut)},
    };
    for (const Entry &e : entries) {
        const Rect swatch{kLegendX, y + 3.0f, 14.0f, 14.0f};
        if (e.outline)
            c.strokeRect(swatch, e.color.alpha(0.6f), 1.0f, kRadiusMark);
        else
            c.fillRect(swatch, e.color.alpha(0.45f), kRadiusMark);
        c.text({kLegendX + 24.0f, y, width - 24.0f, 20.0f}, e.text, kTextBody, kDim, Align::Left);
        y += 26.0f;
    }
    y += 10.0f;
    const Str notes[] = {Str::MatrixClick, Str::MatrixClickThen};
    for (size_t i = 0; i < std::size(notes); ++i)
        c.text({kLegendX, y + static_cast<float>(i) * 20.0f, width, 18.0f}, tr(notes[i]), kTextBody, kFaint, Align::Left);
}

void View::paintFx(Canvas &c, std::optional<Rect> only) {
    // Left out with `only`: what isn't across from it.
    auto away = [&](Rect r) { return only && (r.right() <= only->x || r.x >= only->right()); };
    if (!only) {
        const std::vector<int> chain = fxChain();
        // How many of the effects' places are taken, beside ADD EFFECT.
        const Rect count{fxMenuButton().right() + 10.0f, fxMenuButton().y, 44.0f, fxMenuButton().h};
        const bool full = chain.size() >= static_cast<size_t>(kFxSlots);
        c.text(count, std::to_string(chain.size()) + "/" + std::to_string(kFxSlots), kTextBody, full ? kText : kDim,
               Align::Left, full);
        const float hintX = count.right() + 8.0f;
        if (!chain.empty()) // how to work the cards or the graph's nodes, while there are some
            c.text({hintX, kFxPanel.y + 10.0f, fxClearButton().x - 12.0f - hintX, 22.0f},
                   tr(fxGraph() ? Str::HintFxGraph : Str::HintFx), kTextLabel, kFaint, Align::Right);
    }
    if (fxGraph()) {
        paintFxGraph(c);
        return;
    }
    const FxRow row = fxRowShown();
    const FxLine shownLine = fxLineShown();
    const float scroll = fxScroll();
    // In the gap before the card at `place`, an arrow: the signal runs left to right. Under the mouse, or with its
    // list open, it is a plus that adds an effect there (in the line, `at` on `lane` of item `item`).
    auto chevron = [&](float place, int lane, int item, int at, Color color) {
        const Rect next = fxCard(place - scroll);
        const Point m{next.x - kFxGap * 0.5f, next.center().y};
        if (away({m.x - 8.0f, m.y - 8.0f, 16.0f, 16.0f}))
            return;
        const int b = item * kFxGapStride + at;
        const bool lit = (hover_ && hover_->kind == Kind::FxInsert && hover_->a == lane && hover_->b == b) ||
                         (fxMenuOpen_ && fxSlot_.lane == lane && fxSlot_.item == item && fxSlot_.place == at);
        if (lit) {
            c.fillCircle(m, 8.0f, kAccent.alpha(0.25f));
            c.strokeCircle(m, 8.0f, kAccent, 1.2f);
            c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, kText, 1.5f);
            c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, kText, 1.5f);
            return;
        }
        const Point arrow[] = {{m.x - 3.0f, m.y - 6.0f}, {m.x + 3.0f, m.y}, {m.x - 3.0f, m.y + 6.0f}};
        c.polyline(arrow, 3, color, 1.5f);
    };
    // The grip's dots on a folded strip, lit while it is hovered or held.
    auto gripDots = [&](Rect strip, bool lit) {
        for (int d = 0; d < 6; ++d)
            c.fillCircle({strip.center().x - 5.0f + (d / 2) * 5.0f, strip.y + 35.0f + (d % 2) * 5.0f}, 1.4f,
                         lit ? kText : kFaint);
    };
    c.pushClip(fxCardClip());
    for (size_t i = 0; i + 1 < row.items.size(); ++i) // before each item on the main line, the first too
        chevron(row.items[i], 0, static_cast<int>(i), 0, kFaint);
    // Each split's container under its cards: its outline round them a little out, its controls on grey at its left
    // (out to the outline above, below and before them, and only to their card place after them),
    // then the cards of the band it shows (painted below) and its dashed card (a widget).
    for (const FxRowBox &box : row.boxes) {
        const float place = box.place - scroll;
        if (!onScreen(place, kVisibleFx, box.span))
            continue;
        // Unfolded, inside the room it keeps before and after it.
        const Rect whole = box.folded ? fxCard(place, box.span)
                                      : fxCard(boxColumn(box) - scroll, box.span - 2.0f * kFxBoxEdge);
        if (away(whole.inset(-kFxBoxPad)))
            continue;
        const bool held = drag_.kind == DragKind::FxMove && drag_.widget.kind == Kind::FxBoxGrip &&
                          drag_.widget.a == fxSplitNode(box.split);
        const bool hot = held || (hover_ && hover_->kind == Kind::FxBoxGrip &&
                                  hover_->a == fxSplitNode(box.split) && drag_.kind == DragKind::None);
        const Color outline = held ? kText.alpha(0.7f) : splitLine();
        if (box.folded) { // a strip: its icon, a grip, its cross, then its name down the rest
            c.fillRect(whole, splitFill(), kRadiusCard);
            c.strokeRect(whole, outline, held ? 1.6f : 1.3f, kRadiusCard);
            splitIcon(c, fxIconRect(whole), hot ? kText : kDim, splitThree(box.split) ? 3 : 2);
            gripDots(whole, hot);
            paintDownward(c, {whole.x, whole.y + 82.0f, whole.w, whole.h - 90.0f},
                          trf(Str::NodeSplit, {std::to_string(box.split + 1)}), kText);
            continue;
        }
        const Rect outer = whole.inset(-kFxBoxPad);
        const Rect column = fxCard(boxColumn(box) - scroll, 1.0f);
        // The grey ends at the controls' card place, so the arrow into the band is as far from it as from a card.
        const float bandX = column.right();
        c.fillRect(outer, splitFill(), kFxBoxRadius);
        c.fillRect({bandX, outer.y, outer.right() - bandX, outer.h}, kPanel, kFxBoxRadius);
        c.fillRect({bandX, outer.y, kFxBoxRadius, outer.h}, kPanel);
        c.strokeRect(outer, outline, held ? 1.6f : 1.3f, kFxBoxRadius);
        paintSplitControls(c, box.split, column, true);
        // The arrows along its band, into each of its cards, in the band's color; none into its dashed card.
        // Faint along a band its join doesn't let through, the other band soloed.
        const float heard = box.item < static_cast<int>(shownLine.size()) &&
                                    fxBandSoloedOut(shownLine[box.item], box.lane)
                                ? kSoloedOutAlpha
                                : 1.0f;
        const Color band = laneColor(box.lane).alpha(heard);
        int k = 0;
        for (const FxRowCard &card : row.cards)
            if (card.at.lane == box.lane && card.at.item == box.item) {
                chevron(card.place, box.lane, box.item, k, band);
                ++k;
            }
    }
    for (const FxRowCard &card : row.cards) {
        const int slot = card.slot;
        const FxType type = fxSlotType(slot);
        const float pos = card.place - scroll;
        if (!onScreen(pos, kVisibleFx, card.span))
            continue;
        const Rect r = fxCard(pos, card.span);
        if (away(r))
            continue;
        const bool on = value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
        // Lit while grabbed by its title; by its icon only once it moves, as a click there only switches it.
        const bool dragged = drag_.kind == DragKind::FxMove && drag_.widget.kind != Kind::FxBoxGrip &&
                             drag_.widget.a == slot && (drag_.moved || drag_.widget.kind != Kind::FxPower);
        if (fxFolded(slot)) {
            const Color fx = fxTypeColor(type);
            c.fillRect(r, kCardColor, kRadiusCard);
            c.strokeRect(r, dragged ? fx : (on ? fx.alpha(0.45f) : kBorder), dragged ? 1.5f : 1.0f, kRadiusCard);
            // its icon and cross (widgets), the grip's dots between, then its name down the strip
            gripDots(r, (hover_ && hover_->kind == Kind::FxGrip && hover_->a == slot && drag_.kind == DragKind::None) ||
                            dragged);
            paintDownward(c, {r.x, r.y + 104.0f, r.w, r.h - 110.0f}, fxTitle(type), on ? kText : kDim);
            continue;
        }
        paintFxCard(c, slot, r, dragged);
    }
    c.popClip();

}

// A card's frame, title, display and what goes with them, wherever it shows: in the row, or in the effects graph's
// inspector. Its knobs and buttons are widgets, painted with the others.
void View::paintFxCard(Canvas &c, int slot, Rect card, bool dragged) {
    const FxType type = fxSlotType(slot);
    const bool on = value(paramIndex(slot, FxParam::Enabled)) >= 0.5;
    const Color fx = fxTypeColor(type); // each type of effect in a color of its own
    c.fillRect(card, kCardColor, kRadiusCard);
    c.strokeRect(card, dragged ? fx : (on ? fx.alpha(0.45f) : kBorder), dragged ? 1.5f : 1.0f, kRadiusCard);
    // Title row: the effect's icon (a widget of its own, which also switches it) and its name, up to the fold button.
    const float titleRight = fxGraph() ? cardPowerInGraph(card).x - 3.0f : cardTitleRight(card);
    const Rect titleRect{card.x + 33.0f, card.y + 6.0f, titleRight - card.x - 33.0f, 20.0f};
    const char *title = fxTitle(type);
    float titleSize = kTextBody; // smaller for a long name, so it fits beside the icon
    while (titleSize > kTextSmall && c.textWidth(title, titleSize, true) > titleRect.w)
        titleSize -= 0.5f;
    c.text(titleRect, title, titleSize, on ? kText : kDim, Align::Left, true);

    const Rect display = fxDisplayIn(card, type);
    c.fillRect(display, kDisplayColor, kRadiusCard);
    switch (type) {
    case FxType::Shaper:
        paintShaperCurve(c, display, slot);
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
        paintFilterResponse(c, display, paramIndex(slot, FxParam::FilterType),
                            paramIndex(slot, FxParam::FilterSlope), paramIndex(slot, FxParam::FilterCutoff),
                            paramIndex(slot, FxParam::FilterResonance), paramIndex(slot, FxParam::FilterKeyTrack),
                            telemetry_.fxPitch, fx, &telemetry_.fxSpectrum[slot]);
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
    case FxType::Amp: {
        paintAmp(c, display, slot);
        // The cabinet's display beside it, dimmed while the cabinet is off, and the line between the two sides.
        const Rect cab = ampCabDisplay(card);
        c.fillRect(cab, kDisplayColor, kRadiusCard);
        paintAmpCab(c, cab, slot);
        if (value(paramIndex(slot, FxParam::AmpCabOn)) < 0.5)
            c.fillRect(cab, kCardColor.alpha(0.6f), kRadiusCard);
        if (!on)
            c.fillRect(cab, kPanel.alpha(0.55f), kRadiusCard);
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
            linked ? std::string(tr(Str::CaptionAllBands)) : trf(Str::CaptionBandKnobs, {tr(kMcBandNames[fxBand_[slot]])});
        const float y = card.y + kMdLineY, width = std::ceil(c.textWidth(name, kTextSmall, true)) + 8.0f;
        c.line({card.x + 14.0f + width, y}, {mdRmsButton(card).x - 6.0f, y}, kBorder, 1.0f);
        c.text({card.x + 14.0f, y - 7.0f, width, 14.0f}, name, kTextSmall, on ? fx : kDim, Align::Left, true);
    }
    if (!on)
        c.fillRect(display, kPanel.alpha(0.55f), kRadiusCard); // dimmed while switched off
}

void View::paintShapeEditor(Canvas &c, int shape) {
    if (isNoteCurve(shape)) {
        paintNoteCurveEditor(c);
        return;
    }
    if (isVelocityCurve(shape)) {
        paintVelocityCurveEditor(c);
        return;
    }
    if (isClipCurve(shape) || isShaperCurve(shape)) {
        paintClipCurveEditor(c, shape);
        return;
    }
    const int lfo = shape;
    const Color color = kSourceColors[lfo];
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 104.0f, 22.0f};
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, trf(Str::TitleShape, {sourceName(lfo)}), kTextBody, color, Align::Center, true);
    c.text({title.right() + 12.0f, kRouting.y + 10.0f, shapeReset().x - title.right() - 24.0f, 22.0f},
           shapeHint(), kTextLabel, kFaint, Align::Right);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const double zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? -1.0 : 0.0;
    paintShapeGrid(c, shapeArea(), zero, false);
    paintShapeGridControls(c);

    // The curve, filled toward the zero line.
    auto get = [this](int index) { return value(index); };
    std::vector<double> key = curveKey(Curve::LfoEditor, r);
    keyShape(key, shapeCountIndex(lfo));
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        shapeOutline(get, lfo, 400, [&](double x, double y) { points.push_back(shapeAt(x, y)); });
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, zero));
    area.push_back(shapeAt(0.0, zero));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);

    if (telemetry_.voices > 0 && sourceOn(lfo)) {
        const Point dot = shapeAt(lfoPhaseNow(lfo), lfoValueNow(lfo));
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse, in numbers.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == lfo) {
        const double x = shapePointX(lfo, focus->b);
        const std::string cycle = number("%.1f", x * 100.0);
        // What the LFO puts out there: a unipolar one maps -1..1 onto 0..1.
        const double y = value(shapePointIndex(lfo, focus->b, PointField::Y));
        const std::string level = zero < 0.0 ? number("%.2f", 0.5 * (y + 1.0)) : number("%+.2f", y);
        c.text({r.x, r.bottom() + 10.0f, shapeModeButton(0).x - 12.0f - r.x, 16.0f},
               trf(Str::CaptionShapePoint, {std::to_string(focus->b + 1), cycle, level}), kTextLabel, kDim, Align::Left);
    }
}

std::string View::shapeHint() const {
    const UiLayout &layout = controller_.layout();
    return trf(Str::HintShape, {tr(layout.shapeSteps ? Str::HintShapeSteps : Str::HintShapePoints),
                                tr(layout.shapeSnap ? Str::HintCtrlFree : Str::HintCtrlSnap)});
}

// A shaper's drawn curve: input across, output up, the plain line (no change) for reference.
void View::paintClipCurveEditor(Canvas &c, int shape) {
    const int unit = shape - kMaxSources;
    const Color fx = clipColor(unit);
    const Rect panel = shapePanel();
    const Rect title{panel.x + 16.0f, panel.y + 9.0f, 132.0f, 22.0f};
    c.fillRect(title, fx.alpha(0.18f), kRadiusControl);
    c.text(title,
           unit < kFxSlots ? trf(Str::TitleFxShaperCurve, {std::to_string(unit + 1)})
                           : trf(Str::TitleShaperCurve, {std::to_string(unit - kFxSlots + 1)}),
           kTextBody, fx, Align::Center, true);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), 0.0, true);
    paintShapeGridControls(c);
    c.line(shapeAt(0.0, -1.0), shapeAt(1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    c.text({r.right() - 120.0f, r.bottom() - 18.0f, 116.0f, 14.0f}, tr(Str::AxisInput), kTextLabel, kFaint, Align::Right);
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOutput), kTextLabel, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    const bool symmetric = curveSymmetric(shape);
    std::vector<double> key = curveKey(Curve::ClipEditor, r);
    key.push_back(symmetric ? 1.0 : 0.0);
    keyShape(key, base);
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 400; ++i) {
            const double x = i / 400.0;
            points.push_back(shapeAt(x, shaperCurveValue(get, ClipMode::Custom, base, symmetric, 2.0 * x - 1.0)));
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, 0.0));
    area.push_back(shapeAt(0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    if (symmetric) // the mirrored half, which follows the other, behind a veil
        c.fillRect({r.x, r.y, r.w * 0.5f, r.h}, kDisplayColor.alpha(0.35f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    paintMirroredPoints(c, shape, 4.0f);

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    const Rect below{r.x, r.bottom() + 10.0f, shapeGridButton(0, 0).x - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const std::string in = number("%+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0);
        const std::string out = number("%+.2f", value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text(below, trf(Str::CaptionCurvePoint, {std::to_string(focus->b + 1), in, out}), kTextLabel, kDim, Align::Left);
    } else {
        c.text(below, shapeHint(), kTextLabel, kFaint, Align::Left);
    }
}

// The note source's curve: the notes from Low to High across (the Cs between them named under the plot), what
// the source puts out up, from nothing at the bottom to all of it at the top, and for reference, dashed, what would
// take a cutoff at 0 by 100 % onto the note (the line it starts as); while notes play, a dot where the newest
// voice is.
void View::paintNoteCurveEditor(Canvas &c) {
    const int shape = kNoteCurve;
    const Color color = shapeColor(shape);
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 120.0f, 22.0f};
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, tr(Str::TitleNoteCurve), kTextBody, color, Align::Center, true);
    const float hintX = kRouting.x + 148.0f + 2.0f * 96.0f + 12.0f; // after Low and High
    c.text({hintX, kRouting.y + 10.0f, shapeReset().x - 12.0f - hintX, 22.0f}, shapeHint(), kTextLabel, kFaint,
           Align::Right);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), -1.0, false);
    paintShapeGridControls(c);
    const double low = value(paramIndex(NoteParam::Low)), high = value(paramIndex(NoteParam::High));
    std::vector<double> key = curveKey(Curve::NoteExact, r);
    key.insert(key.end(), {low, high});
    const std::vector<Point> exact = curves_.get(key, [&] { // what would put a cutoff at 0 by 100 % on the note
        std::vector<Point> exact;
        for (int i = 0; i <= 120; ++i) {
            const double x = i / 120.0;
            exact.push_back(shapeAt(x, 2.0 * cutoffNormOfPitch(low + x * (high - low)) - 1.0));
        }
        return exact;
    });
    c.polyline(exact.data(), exact.size(), kFaint.alpha(0.6f), 1.0f, true);
    for (int octave = 0; octave * 12.0 <= kMaxNotePitch; ++octave) {
        const double x = high == low ? -1.0 : (octave * 12.0 - low) / (high - low);
        if (x > 0.03 && x < 0.97)
            c.text({shapeAt(x, -1.0).x - 20.0f, r.bottom() - 16.0f, 40.0f, 14.0f}, noteName(octave * 12), kTextLabel, kFaint);
    }
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOut), kTextLabel, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    key = curveKey(Curve::NoteEditor, r);
    keyShape(key, base);
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 400; ++i) {
            const double x = i / 400.0;
            points.push_back(shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0)));
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, -1.0));
    area.push_back(shapeAt(0.0, -1.0));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (telemetry_.voices > 0) {
        const double x = high == low ? (telemetry_.notePitch >= low ? 1.0 : 0.0)
                                     : std::clamp((telemetry_.notePitch - low) / (high - low), 0.0, 1.0);
        const Point dot = shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0));
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    const Rect below{r.x, r.bottom() + 10.0f, shapeModeButton(0).x - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char note[32];
        formatParam(paramIndex(NoteParam::Low), low + shapePointX(shape, focus->b) * (high - low), note, sizeof(note));
        const std::string out = number("%.0f", 50.0 * (value(shapeIndex(shape, focus->b, PointField::Y)) + 1.0));
        c.text(below, trf(Str::CaptionNotePoint, {std::to_string(focus->b + 1), note, out}), kTextLabel, kDim, Align::Left);
    } else {
        c.text(below, tr(Str::HintNoteAxes), kTextLabel, kFaint, Align::Left);
    }
}

// Velocity's curve: the velocities across (0 to 127, marked every 32), what the source puts out up, from nothing at
// the bottom to all of it at the top, and for reference, dashed, the straight line it starts as; while notes play, a
// dot where the newest voice's velocity is.
void View::paintVelocityCurveEditor(Canvas &c) {
    const int shape = kVelocityCurve;
    const Color color = shapeColor(shape);
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 136.0f, 22.0f};
    c.fillRect(title, color.alpha(0.18f), kRadiusControl);
    c.text(title, tr(Str::TitleVelocityCurve), kTextBody, color, Align::Center, true);
    c.text({title.right() + 12.0f, kRouting.y + 10.0f, shapeReset().x - title.right() - 24.0f, 22.0f}, shapeHint(),
           kTextLabel, kFaint, Align::Right);

    const Rect r = shapeArea();
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    paintShapeGrid(c, shapeArea(), -1.0, false);
    paintShapeGridControls(c);
    c.line(shapeAt(0.0, -1.0), shapeAt(1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    for (int velocity = 32; velocity < 127; velocity += 32)
        c.text({shapeAt(velocity / 127.0, -1.0).x - 20.0f, r.bottom() - 16.0f, 40.0f, 14.0f}, std::to_string(velocity),
               kTextLabel, kFaint);
    c.text({r.right() - 120.0f, r.bottom() - 18.0f, 116.0f, 14.0f}, tr(Str::AxisVelocity), kTextLabel, kFaint, Align::Right);
    c.text({r.x + 6.0f, r.y + 4.0f, 80.0f, 14.0f}, tr(Str::AxisOut), kTextLabel, kFaint, Align::Left);

    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    std::vector<double> key = curveKey(Curve::VelocityEditor, r);
    keyShape(key, base);
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 400; ++i) {
            const double x = i / 400.0;
            points.push_back(shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0)));
        }
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(shapeAt(1.0, -1.0));
    area.push_back(shapeAt(0.0, -1.0));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (telemetry_.voices > 0) {
        const double x = std::clamp(static_cast<double>(telemetry_.noteVelocity), 0.0, 1.0);
        const Point dot = shapeAt(x, clipCurveValue(get, base, 2.0 * x - 1.0));
        c.fillCircle(dot, 8.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    // The point under the mouse in numbers, or how to edit, under the plot left of the grid's controls.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    const Rect below{r.x, r.bottom() + 10.0f, shapeModeButton(0).x - 12.0f - r.x, 16.0f};
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        const std::string in = number("%.0f", 127.0 * shapePointX(shape, focus->b));
        const std::string out = number("%.0f", 50.0 * (value(shapeIndex(shape, focus->b, PointField::Y)) + 1.0));
        c.text(below, trf(Str::CaptionVelocityPoint, {std::to_string(focus->b + 1), in, out}), kTextLabel, kDim,
               Align::Left);
    } else {
        c.text(below, tr(Str::HintVelocityAxes), kTextLabel, kFaint, Align::Left);
    }
}

int View::partialEditing() const {
    if (partialEditor_ < 0 || shapeEditing() >= 0)
        return -1;
    return isCustomWave(partialEditor_) ? partialEditor_ : -1;
}

int View::partialsShown(int op) const {
    return partialCount(value(partialCountIndex(op)));
}

void View::paintPartialEditor(Canvas &c, int op) {
    const Rect title{kRouting.x + 16.0f, kRouting.y + 9.0f, 120.0f, 22.0f};
    c.fillRect(title, kAccent.alpha(0.18f), kRadiusControl);
    c.text(title, trf(Str::TitlePartials, {kNodeNames[op]}), kTextBody, kAccent, Align::Center, true);
    // Only the partials the wave plays show; the ones past them keep their values for when there are more again.
    const int count = partialsShown(op);
    // The partial under the mouse in numbers, or how to draw.
    const Widget *focus = drag_.kind == DragKind::Partials ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    std::string hint = tr(Str::HintPartials);
    if (focus && focus->kind == Kind::PartialArea) {
        const Point at = drag_.kind == DragKind::Partials ? drag_.now : hoverPoint_;
        const int k = partialAt(focus->b == 0 ? kPartialLevels : kPartialPhases, at.x, count);
        const double level = value(partialLevelIndex(op, k));
        const std::string partial = std::to_string(k + 1), phase = number("%.0f", value(partialPhaseIndex(op, k)) * 360.0);
        hint = level > 0.0 ? trf(Str::CaptionPartial, {partial, number("%.1f", 20.0 * std::log10(level)), phase})
                           : trf(Str::CaptionPartialSilent, {partial, phase});
    }
    const float hintX = partialCountButton(static_cast<int>(std::size(kPartialCounts)) - 1).right() + 12.0f;
    c.text({hintX, kRouting.y + 10.0f, shapeReset().x - hintX - 12.0f, 22.0f}, hint, kTextLabel, kFaint, Align::Right);

    // The wave they make, one cycle, at its loudest from top to bottom, as the operator plays it.
    const Rect wave = kPartialWave;
    c.fillRect(wave.inset(-6.0f), kDisplayColor, kRadiusCard);
    c.line({wave.x, wave.center().y}, {wave.right(), wave.center().y}, kBorder, 1.0f);
    std::vector<double> key = curveKey(Curve::Partials, wave);
    for (int k = 0; k < count; ++k)
        key.insert(key.end(), {value(partialLevelIndex(op, k)), value(partialPhaseIndex(op, k))});
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<double> samples(241, 0.0);
        double peak = 0.0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const double x = static_cast<double>(i) / (samples.size() - 1);
            for (int k = 0; k < count; ++k)
                if (const double level = value(partialLevelIndex(op, k)); level > 0.0)
                    samples[i] +=
                        level * std::sin(2.0 * std::numbers::pi * ((k + 1) * x + value(partialPhaseIndex(op, k))));
            peak = std::max(peak, std::abs(samples[i]));
        }
        std::vector<Point> points;
        for (size_t i = 0; i < samples.size(); ++i) {
            const float y = static_cast<float>(peak > 0.0 ? samples[i] / peak : 0.0);
            points.push_back({wave.x + wave.w * i / (samples.size() - 1), wave.center().y - y * wave.h * 0.5f});
        }
        return points;
    });
    plotLine(c, points.data(), points.size(), kAccent, 2.0f);

    // Levels (in dB, a faint line every 12) and phases (a full cycle at the top) alike as bars from the bottom,
    // in different colors; a phase is faint while its partial is silent.
    for (int row = 0; row < 2; ++row) {
        const Rect area = row == 0 ? kPartialLevels : kPartialPhases;
        c.fillRect(area.inset(-6.0f), kDisplayColor, kRadiusCard);
        c.text({area.x - 2.0f, area.y - 22.0f, 80.0f, 12.0f}, tr(row == 0 ? Str::HeadingLevel : Str::HeadingPhase), kTextLabel,
               kFaint, Align::Left, true);
        for (int k = 0; k < count; ++k) {
            const Rect bar = partialBar(area, k, count);
            const double v = row == 0 ? partialBarFromLevel(value(partialLevelIndex(op, k)))
                                      : value(partialPhaseIndex(op, k));
            const float h = static_cast<float>(v) * bar.h;
            c.fillRect({bar.x, area.y, bar.w, area.h}, kRaised.alpha(0.5f), kRadiusMark);
            if (h > 0.5f) {
                const bool silent = row == 1 && value(partialLevelIndex(op, k)) <= 0.0;
                const float alpha = (0.35f + 0.5f * static_cast<float>(v)) * (silent ? 0.4f : 1.0f);
                c.fillRect({bar.x, bar.bottom() - h, bar.w, h}, (row == 0 ? kAccent : kFxColor).alpha(alpha), kRadiusMark);
            }
        }
        if (row == 0)
            for (int db = 0; db > kPartialFloorDb; db -= 12) {
                const double bar = partialBarFromLevel(std::pow(10.0, db / 20.0));
                const float y = area.bottom() - static_cast<float>(bar) * area.h;
                if (db < 0)
                    c.line({area.x, y}, {area.right(), y}, kBorder.alpha(0.5f), 1.0f);
                c.text({area.x - 38.0f, y - 6.0f, 30.0f, 12.0f}, std::to_string(db), kTextSmall, kFaint, Align::Right);
            }
    }
    // Each partial's number under it; of 64, the first and every fourth, where there is room for them.
    for (int k = 0; k < count; ++k) {
        if (count > kFirstPartials && k > 0 && (k + 1) % 4 != 0)
            continue;
        const Rect bar = partialBar(kPartialPhases, k, count);
        c.text({bar.center().x - 12.0f, kPartialPhases.bottom() + 8.0f, 24.0f, 12.0f}, std::to_string(k + 1), kTextLabel,
               kFaint);
    }
}

void View::paintShapeGrid(Canvas &c, Rect r, double zero, bool centered) {
    const int across = controller_.layout().shapeGridX;
    const int upDown = controller_.layout().shapeGridY;
    // Lines across stand out every quarter, or every third on a grid of thirds; a centered grid's middle more.
    const int major = across % 4 == 0 && across > 4 ? across / 4 : (across % 3 == 0 && across > 3 ? across / 3 : 0);
    for (int i = 1; i < across; ++i) {
        const float x = r.x + r.w * i / across;
        const float alpha = centered && 2 * i == across ? 1.0f : (major > 0 && i % major == 0 ? 0.9f : 0.35f);
        c.line({x, r.y}, {x, r.bottom()}, kBorder.alpha(alpha), 1.0f);
    }
    // Up and down: solid at zero, dashed elsewhere, fainter between the halves.
    for (int i = 0; i <= upDown; ++i) {
        const double y = -1.0 + 2.0 * i / upDown;
        const bool atZero = std::abs(y - zero) < 1e-9;
        const bool half = 4 * i % upDown == 0;
        c.line(areaAt(r, 0.0, y), areaAt(r, 1.0, y), kBorder.alpha(atZero ? 1.0f : (half ? 0.5f : 0.3f)), 1.0f,
               !atZero);
    }
}

// "GRID - 16 + x - 8 +" under the editor's right end; the buttons and counts are widgets.
void View::paintShapeGridControls(Canvas &c) {
    const Rect first = shapeGridButton(0, 0);
    c.text({first.x - 48.0f, first.y, 42.0f, first.h}, tr(Str::HeadingGrid), kTextLabel, kFaint, Align::Right, true);
    const Rect across = shapeGridButton(0, 2), upDown = shapeGridButton(1, 0);
    c.text({across.right(), across.y, upDown.x - across.right(), across.h}, "\xC3\x97", kTextBody, kFaint);
}

// Transfer curve: input level across, output up, with the drive, output and mix applied.
void View::paintShaperCurve(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int curve = kMaxSources + unit;
    const ClipMode mode = clipMode(value(clip.mode));
    if (mode == ClipMode::Custom && shapeEditing() != curve) { // with its points, unless its big editor has them
        paintClipCurveCard(c, r, unit);
        return;
    }
    const Rect inner = r.inset(8.0f);
    constexpr double kRange = 1.3;
    auto at = [&](double x, double y) {
        return Point{inner.center().x + static_cast<float>(x / kRange) * inner.w * 0.5f,
                     inner.center().y - static_cast<float>(std::clamp(y, -kRange, kRange) / kRange) * inner.h * 0.5f};
    };
    c.line(at(-kRange, 0.0), at(kRange, 0.0), kBorder, 1.0f);
    c.line(at(0.0, -kRange), at(0.0, kRange), kBorder, 1.0f);
    c.line(at(-kRange, 1.0), at(kRange, 1.0), kBorder.alpha(0.7f), 1.0f, true); // full scale
    c.line(at(-kRange, -1.0), at(kRange, -1.0), kBorder.alpha(0.7f), 1.0f, true);
    c.line(at(-kRange, -kRange), at(kRange, kRange), kFaint.alpha(0.6f), 1.0f);

    const double drive = std::pow(10.0, liveValue(clip.drive) / 20.0); // as modulation takes them now
    const double output =
        std::pow(10.0, clipOutputDb(liveValue(clip.output), liveValue(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = liveValue(clip.mix);
    auto get = [this](int index) { return value(index); };
    const bool symmetric = curveSymmetric(curve);
    auto shape = [&](double x) { return shaperCurveValue(get, mode, shapeBase(curve), symmetric, x); };
    std::vector<double> key = curveKey(Curve::Shaper, inner);
    key.insert(key.end(), {static_cast<double>(mode), symmetric ? 1.0 : 0.0, drive, output, mix});
    if (mode == ClipMode::Custom) // a drawn curve whose big editor has its points
        keyShape(key, shapeBase(curve));
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 240; ++i) {
            const double x = kRange * (2.0 * i / 240.0 - 1.0);
            points.push_back(at(x, x + (shape(x * drive) * output - x) * mix));
        }
        return points;
    });
    plotLine(c, points.data(), points.size(), fx, 2.0f);
}

// A symmetric curve's points, mirrored onto its left half as faint rings: they move with the ones they mirror.
void View::paintMirroredPoints(Canvas &c, int shape, float radius) {
    if (!curveSymmetric(shape))
        return;
    const Color color = shapeColor(shape).alpha(0.45f);
    for (int point = 0; point < shapePointCount(shape); ++point)
        if (pointShown(shape, point) && !middlePoint(shape, point))
            c.strokeCircle(shapeAt(shape, 1.0 - shapePointX(shape, point), -value(shapeIndex(shape, point, PointField::Y))),
                           radius, color, 1.0f);
}

// A drawn curve in its card, as in its editor (input across, output up, to full scale), with its points and
// bends to drag (widgets); faintly behind it, what the shaper makes of it with its drive, output and mix.
void View::paintClipCurveCard(Canvas &c, Rect r, int unit) {
    const Color fx = clipColor(unit);
    const ShaperParams clip = shaperParams(unit);
    const int shape = kMaxSources + unit;
    c.line(shapeAt(shape, 0.0, 0.0), shapeAt(shape, 1.0, 0.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.5, -1.0), shapeAt(shape, 0.5, 1.0), kBorder, 1.0f);
    c.line(shapeAt(shape, 0.0, -1.0), shapeAt(shape, 1.0, 1.0), kFaint.alpha(0.6f), 1.0f, true);
    auto get = [this](int index) { return value(index); };
    const int base = shapeBase(shape);
    const double drive = std::pow(10.0, liveValue(clip.drive) / 20.0); // as modulation takes them now
    const double output =
        std::pow(10.0, clipOutputDb(liveValue(clip.output), liveValue(clip.drive), value(clip.link) >= 0.5) / 20.0);
    const double mix = liveValue(clip.mix);
    const bool symmetric = curveSymmetric(shape);
    auto curve = [&](double in) { return shaperCurveValue(get, ClipMode::Custom, base, symmetric, in); };
    std::vector<double> key = curveKey(Curve::Drawn, shapeArea(shape));
    key.push_back(symmetric ? 1.0 : 0.0);
    keyShape(key, base);
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0, in = 2.0 * x - 1.0;
            points.push_back(shapeAt(shape, x, curve(in)));
        }
        return points;
    });
    key[0] = static_cast<double>(Curve::Driven);
    key.insert(key.end(), {drive, output, mix});
    const std::vector<Point> driven = curves_.get(key, [&] {
        std::vector<Point> driven;
        for (int i = 0; i <= 160; ++i) {
            const double x = i / 160.0, in = 2.0 * x - 1.0;
            const double out = in + (curve(in * drive) * output - in) * mix;
            driven.push_back(shapeAt(shape, x, std::clamp(out, -1.1, 1.1)));
        }
        return driven;
    });
    c.polyline(driven.data(), driven.size(), fx.alpha(0.35f), 1.0f);
    std::vector<Point> area = points;
    area.push_back(shapeAt(shape, 1.0, 0.0));
    area.push_back(shapeAt(shape, 0.0, 0.0));
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f));
    plotLine(c, points.data(), points.size(), fx, 2.0f);
    paintMirroredPoints(c, shape, 3.0f);

    // The point under the mouse, in numbers (input to output), in the top left corner, which a curve
    // leaves empty.
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == shape) {
        char text[48];
        std::snprintf(text, sizeof(text), "%+.2f \xE2\x86\x92 %+.2f", 2.0 * shapePointX(shape, focus->b) - 1.0,
                      value(shapeIndex(shape, focus->b, PointField::Y)));
        c.text({r.x + 6.0f, r.y + 4.0f, r.w - 12.0f, 14.0f}, text, kTextLabel, kDim, Align::Left);
    }
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

// What an effect or a voice filter puts out, measured, faintly behind its curves: 0 dB at the top down to -72 at
// the bottom. Once the audio stops (the snapshot gets old), it falls on at the analyzer's pace, 55 dB a second.
void View::paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum) {
    const float fall = 55.0f * std::max(telemetry_.age - 0.05f, 0.0f);
    if (fall >= 72.0f || std::none_of(spectrum.begin(), spectrum.end(), [](float level) { return level > 1e-4f; }))
        return;
    std::vector<Point> points{{area.x, area.bottom()}};
    for (int band = 0; band < kSpectrumBands; ++band) {
        const float db = std::clamp(20.0f * std::log10(std::max(spectrum[band], 1e-6f)) - fall, -72.0f, 0.0f);
        points.push_back(
            {area.x + (band + 0.5f) / kSpectrumBands * area.w, area.bottom() - (db + 72.0f) / 72.0f * area.h});
    }
    points.push_back({area.right(), area.bottom()});
    c.fillPolygon(points.data(), points.size(), kText.alpha(0.1f));
    c.polyline(points.data() + 1, points.size() - 2, kDim.alpha(0.6f), 1.0f);
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
    paintSpectrum(c, area, telemetry_.fxSpectrum[slot]);
    auto on = [&](int band) { return value(peqOnIndex(slot, band)) >= 0.5; };
    // The bands as modulation takes them now; their handles stay where they are set.
    auto live = [&](int param) { return liveValue(param); };
    auto curve = [&](int only) { // only one band, or every band that plays (-1)
        std::vector<double> key = curveKey(Curve::ParamEq, area);
        key.push_back(only);
        for (int band = 0; band < kPeqBands; ++band)
            key.insert(key.end(), {value(peqTypeIndex(slot, band)), live(peqFreqIndex(slot, band)),
                                   live(peqGainIndex(slot, band)), live(peqQIndex(slot, band)), on(band) ? 1.0 : 0.0});
        return curves_.get(key, [&] {
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
void View::paintDownward(Canvas &c, Rect r, std::string_view name, Color color) {
    size_t number = name.size();
    while (number > 0 && std::isdigit(static_cast<unsigned char>(name[number - 1])))
        --number;
    if (number == 0 || name[number - 1] != ' ')
        number = name.size(); // no number after a space: every character down
    size_t line = 0;
    for (size_t i = 0; i < name.size() && (line + 1) * 13.0f <= r.h; ++i, ++line) {
        const size_t length = i == number ? name.size() - i : 1;
        if (name[i] != ' ')
            c.text({r.x, r.y + line * 13.0f, r.w, 13.0f}, std::string(name.substr(i, length)), kTextLabel, color,
                   Align::Center, true);
        i += length - 1;
    }
}

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
    const std::vector<Point> points = curves_.get(key, [&] {
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
    c.fillRect(meter, kCardColor, kRadiusMark);
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
    c.text({plot.x + 4.0f, plot.y + 2.0f, plot.w, 14.0f}, trf(Str::CaptionReduction, {number("%.1f", reduction)}),
           kTextLabel, reduction > 0.05f ? kText : kFaint, Align::Left);
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
    const int shown = fxBand_[slot];
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
        c.fillRect(bar, kCardColor, kRadiusMark);
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
    const double rateHz = sweepHz([this](int i) { return liveValue(i); }, slot, phaser, telemetry_.tempo);
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
        const std::vector<Point> points = curves_.get(key, [&] {
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

// The partials of a 220 Hz tone, faint, and where the shifter moves them: the left side above the
// line, the right below, on a log axis from 50 Hz to 10 kHz.
// The disperser's delay across frequency: how much later than the rest each part of the sound comes out,
// rising to its peak at Freq, taller and narrower with more stages and more pinch. Behind it, on the same axis, what
// it puts out, measured.
void View::paintDisperser(Canvas &c, Rect r, int slot) {
    const Color fx = fxColor(slot);
    const Rect inner{r.x + 8.0f, r.y + 20.0f, r.w - 16.0f, r.h - 28.0f};
    auto at = [&](FxParam f) { return liveValue(paramIndex(slot, f)); }; // as modulation takes them now
    const int stages = static_cast<int>(std::lround(at(FxParam::DisperserStages)));
    constexpr double kRate = 48000.0;
    std::vector<double> delays;
    double most = 1e-4;
    for (int i = 0; i <= 160; ++i) {
        delays.push_back(disperserDelay(cutoffFromNorm(i / 160.0), at(FxParam::DisperserFreq),
                                        at(FxParam::DisperserPinch), stages, kRate));
        most = std::max(most, delays.back());
    }
    std::vector<Point> points;
    for (int i = 0; i <= 160; ++i)
        points.push_back({inner.x + inner.w * i / 160.0f,
                          inner.bottom() - static_cast<float>(delays[i] / (most * 1.1)) * inner.h});
    paintSpectrum(c, inner, telemetry_.fxSpectrum[slot]);
    c.line({inner.x, inner.bottom()}, {inner.right(), inner.bottom()}, kBorder, 1.0f);
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), fx.alpha(0.12f * static_cast<float>(at(FxParam::DisperserMix)) + 0.04f));
    plotLine(c, points.data(), points.size(), fx, 1.8f);
    char freq[32], text[80];
    formatParam(paramIndex(slot, FxParam::DisperserFreq), at(FxParam::DisperserFreq), freq, sizeof(freq));
    std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionDispersion, {freq, number("%.1f", most * 1000.0)}).c_str());
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
    c.fillRect(meter, kCardColor, kRadiusMark);
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
    c.strokeRect(box, color.alpha(lit ? 0.9f : 0.45f), 1.0f, kRadiusControl);
    // Behind the note, faintly, its curve across the box.
    auto get = [this](int index) { return value(index); };
    std::vector<Point> curve;
    for (int i = 0; i <= 24; ++i) {
        const double x = i / 24.0;
        const double y = clipCurveValue(get, paramIndex(NoteParam::Curve), 2.0 * x - 1.0);
        curve.push_back({box.x + 4.0f + static_cast<float>(x) * (box.w - 8.0f),
                         box.bottom() - 4.0f - static_cast<float>(0.5 * (y + 1.0)) * (box.h - 8.0f)});
    }
    c.polyline(curve.data(), curve.size(), color.alpha(0.3f), 1.2f);
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
    if (sourceBipolar(kNoteSource)) // swings both ways: a sign in its corner
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
    c.strokeRect(box, color.alpha(lit ? 0.9f : 0.45f), 1.0f, kRadiusControl);
    // Behind the number, faintly, its curve across the box, with a mark where the newest voice's velocity is.
    auto get = [this](int index) { return value(index); };
    auto at = [&](double x) {
        return Point{box.x + 4.0f + static_cast<float>(x) * (box.w - 8.0f),
                     box.bottom() - 4.0f - static_cast<float>(velocitySourceValue(get, x)) * (box.h - 8.0f)};
    };
    std::vector<Point> curve;
    for (int i = 0; i <= 24; ++i)
        curve.push_back(at(i / 24.0));
    c.polyline(curve.data(), curve.size(), color.alpha(0.3f), 1.2f);
    const bool playing = telemetry_.voices > 0;
    if (playing)
        c.fillCircle(at(telemetry_.noteVelocity), 2.0f, color.alpha(0.6f));
    const double velocity = std::clamp(static_cast<double>(telemetry_.noteVelocity), 0.0, 1.0);
    const std::string text = playing ? std::to_string(std::lround(127.0 * velocity)) : "\xE2\x80\x93";
    c.text(box, text, kTextBody, playing ? kText : kFaint, Align::Center, playing);
    c.text({box.x - 14.0f, top + kStripRow + 19.0f, box.w + 28.0f, 16.0f}, tr(Str::LabelVelocitySource), kTextLabel, kDim);
    if (sourceBipolar(kVelocitySource)) // swings both ways: a sign in its corner
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

void View::paintInspector(Canvas &c) {
    const int node = controller_.layout().selectedNode;
    // Leaves room for the on/off switch at the right.
    Rect summaryRect{kInspector.x + 16.0f, kInspector.y + 38.0f, kInspector.w - 84.0f, 18.0f};
    const char *dot = "  \xC2\xB7  ";

    if (isShaperNode(node)) {
        paintShaperPage(c, shaperOfNode(node), summaryRect);
        return;
    }
    if (node < kOps) {
        const int op = node;
        const double ratio = ratioHundredths(value(paramIndex(op, OpParam::Ratio))) / 100.0;
        std::string summary = std::string(kNodeNames[op]) + dot;
        summaryRect.w -= kFixedSwitch.w + 4.0f; // the keyboard switch, left of the power switch
        if (isCustomWave(op))
            summaryRect.w -= kPartialEditButton.w + 4.0f; // and a Custom wave's edit button, left of that
        if (isNoise(op)) {
            char slope[32];
            const int slopeParam = paramIndex(op, OpParam::NoiseSlope);
            formatParam(slopeParam, value(slopeParam), slope, sizeof(slope));
            summary += waveName(OpWave::Noise) + (dot + std::string(slope));
        } else {
            summary += waveName(static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))))) +
                       (dot + (isFixed(op) ? fixedHzText(op, false) : number("%.2f", ratio)));
        }
        const long copies = std::lround(value(paramIndex(op, OpParam::Unison)));
        if (copies > 1 && !isNoise(op))
            summary += dot + trf(Str::SummaryUnison, {std::to_string(copies)});
        std::string feeds;
        if (value(paramIndex(op, OpParam::Out)) > 0.0)
            feeds = tr(Str::SummaryOut);
        for (int to = 0; to < kNodes; ++to)
            if (to != op && nodeOn(to) && value(routeParam(op, to)) > 0.0)
                feeds += (feeds.empty() ? "" : ", ") + std::string(kNodeNames[to]);
        if (!feeds.empty())
            summary += dot + trf(Str::SummaryTo, {feeds});
        c.text(summaryRect, summary, kTextBody, kDim, Align::Left);
        // The tabs' line, which the shown one's accent sits on.
        c.line({kInspector.x + 16.0f, kOpTabsY + kOpTabH}, {kInspector.right() - 16.0f, kOpTabsY + kOpTabH}, kBorder,
               1.0f);
        switch (opTabShown(op)) {
        case InspectorTab::Unison:
            if (isNoise(op))
                paintNoiseSlope(c, kNoiseRect, op);
            else
                paintUnison(c, kUnisonRect, op);
            break;
        case InspectorTab::Envelope:
            paintEnvelope(c, kOpEnvRect, op, kAccent, true);
            break;
        case InspectorTab::Scope:
            paintScope(c, kScopeRect, op);
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
    std::string summary = std::string(kNodeNames[node]) + dot + filterTypeName(type);
    if (hasSlope(type))
        summary += " " + std::to_string(slopeDb(playedSlope(type, filterSlope(value(paramIndex(filter, FilterParam::Slope)))))) +
                   " dB";
    c.text(summaryRect, summary + boxRoutes(node), kTextBody, kDim, Align::Left);
    // A voice filter tracks the newest voice's note; with none, the curve is back at the knob's C4.
    paintFilterResponse(c, kFilterDisplay, paramIndex(filter, FilterParam::Type),
                        paramIndex(filter, FilterParam::Slope), paramIndex(filter, FilterParam::Cutoff),
                        paramIndex(filter, FilterParam::Resonance), paramIndex(filter, FilterParam::KeyTrack),
                        telemetry_.voices > 0 ? telemetry_.notePitch : 60.0, kFilterColor,
                        &telemetry_.filterSpectrum[filter]);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), kTextLabel, kFaint,
           Align::Left, true);
}

// A filter's or shaper's routes, for its page's summary: what goes into it, the filters and shapers it feeds and the
// output, and the operators it modulates.
std::string View::boxRoutes(int node) const {
    const std::string dot = "  \xC2\xB7  ";
    std::string from, to, mods;
    auto name = [](std::string &list, const char *node) { list += (list.empty() ? "" : ", ") + std::string(node); };
    for (int other = 0; other < kNodes; ++other) {
        if (other == node || !nodeOn(other))
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

void View::paintShaperPage(Canvas &c, int shaper, Rect summaryRect) {
    const std::string summary = std::string(kNodeNames[kOps + kFilters + shaper]) + "  \xC2\xB7  " +
                                clipModeName(clipMode(value(paramIndex(shaper, ShaperParam::Mode))));
    c.text(summaryRect, summary + boxRoutes(kOps + kFilters + shaper), kTextBody, kDim, Align::Left);
    c.fillRect(kShaperDisplay, kDisplayColor, kRadiusCard);
    paintShaperCurve(c, kShaperDisplay, kFxSlots + shaper);
    c.text({kInspector.x + 16.0f, kInspector.y + 276.0f, 120.0f, 14.0f}, tr(Str::HeadingInputs), kTextLabel, kFaint,
           Align::Left, true);
}

// Where the unison copies sit: left to right by stereo position, up and down by detune.
void View::paintUnison(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    c.line({r.center().x, r.y + 6.0f}, {r.center().x, r.bottom() - 6.0f}, kBorder, 1.0f);
    c.line({r.x + 18.0f, r.center().y}, {r.right() - 18.0f, r.center().y}, kBorder.alpha(0.6f), 1.0f);
    c.text({r.x + 4.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "L", kTextLabel, kFaint);
    c.text({r.right() - 16.0f, r.bottom() - 14.0f, 12.0f, 12.0f}, "R", kTextLabel, kFaint);
    const int copies = std::clamp(static_cast<int>(std::lround(value(paramIndex(op, OpParam::Unison)))), 1, kMaxUnison);
    const double detune = value(paramIndex(op, OpParam::UnisonDetune)) / 100.0;
    const double spread = value(paramIndex(op, OpParam::UnisonSpread));
    for (int k = 0; k < copies; ++k) {
        const double offset = copies > 1 ? 2.0 * k / (copies - 1) - 1.0 : 0.0;
        const Point dot{r.center().x + static_cast<float>(offset * spread) * (r.w * 0.5f - 24.0f),
                        r.center().y - static_cast<float>(offset * detune) * (r.h * 0.5f - 12.0f)};
        c.line({dot.x, r.center().y}, dot, (copies > 1 ? kAccent : kDim).alpha(0.4f), 1.5f);
        c.fillCircle(dot, 4.0f, copies > 1 ? kAccent : kDim);
    }
}

bool View::isCustomWave(int op) const {
    return std::lround(value(paramIndex(op, OpParam::Wave))) == static_cast<long>(OpWave::Custom);
}

bool View::isFixed(int op) const {
    return value(paramIndex(op, OpParam::Fixed)) >= 0.5;
}

// A fixed operator's Freq x Mult: "440 Hz" in the summary line, "440Hz" or "5.3kHz" inside its circle in the graph.
std::string View::fixedHzText(int op, bool compact) const {
    const double hz = fixedHz(value(paramIndex(op, OpParam::FixedFreq)), value(paramIndex(op, OpParam::FixedMult)));
    const char *unit = compact ? "Hz" : " Hz";
    if (hz >= 1000.0)
        return number(compact ? "%.2g" : "%.2f", hz / 1000.0) + (compact ? "kHz" : " kHz");
    return number(hz >= 100.0 ? "%.0f" : hz >= 10.0 ? "%.1f" : hz >= 0.1 ? "%.2f" : "%.3f", hz) + unit;
}

// A switch flips; the Fixed and symmetric switches with what goes along, as clicked or scrolled.
void View::flipSwitch(int param) {
    if (fxFieldOf(param) == FxParam::ShaperSymmetric || shaperFieldOf(param) == ShaperParam::Symmetric)
        switchSymmetric(param);
    else if (fixedSwitchOp(param) >= 0)
        switchFixed(fixedSwitchOp(param));
    else
        edit(param, value(param) >= 0.5 ? 0.0 : 1.0);
}

// Switched to fixed, the operator plays where its ratio put it at A4: its Freq and Mult are set to that, with the
// smallest Mult that keeps the Freq in its range, in the same undo step.
void View::switchFixed(int op) {
    const int fixed = paramIndex(op, OpParam::Fixed);
    std::vector<std::pair<int, double>> changes;
    if (!isFixed(op)) {
        const double hz = 440.0 * ratioFromNorm(value(paramIndex(op, OpParam::Ratio)));
        int mult = 0;
        while (mult + 1 < kFixedMultCount && hz / kFixedMults[mult] > kMaxFixedFreq)
            ++mult;
        changes.push_back({paramIndex(op, OpParam::FixedFreq), normFromFixedFreq(hz / kFixedMults[mult])});
        changes.push_back({paramIndex(op, OpParam::FixedMult), static_cast<double>(mult)});
    }
    changes.push_back({fixed, isFixed(op) ? 0.0 : 1.0});
    editMany(changes);
}

View::LabelPill View::labelPill(int param, int mode, Rect idle) const {
    auto mine = [&](const Widget &w) {
        return ((w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::LabelRemove) &&
                w.param == param) ||
               (w.kind == Kind::LabelMode && mode >= 0 && w.param == mode);
    };
    LabelPill pill;
    pill.open = (hover_ && mine(*hover_)) || (mode >= 0 && listMenu_ == mode) ||
                (drag_.kind != DragKind::None && mine(drag_.widget));
    // A mode other than Phase shows inside it at rest, at its left: the label widens by kGlyphW, still centered on its
    // wire, and its depth's text sits that much right of the middle, half of kGlyphW.
    constexpr float kGlyphW = 18.0f;
    const bool glyph = mode >= 0 && routeMode(value(mode)) != RouteMode::Phase;
    const float shift = glyph ? 0.5f * kGlyphW : 0.0f;
    if (!pill.open) {
        pill.glyph = glyph;
        pill.depth = glyph ? Rect{idle.x - shift, idle.y, idle.w + kGlyphW, idle.h} : idle;
        pill.whole = pill.depth;
        return pill;
    }
    // The depth's text stays where it rests, so it doesn't move as the pill opens and the mouse on it stays on it; the
    // mode's part goes on its left, over where a glyph shows at rest, and the x on its right.
    constexpr float kModeW = 28.0f, kRemoveW = 20.0f;
    idle.x += shift;
    pill.depth = idle;
    if (mode >= 0)
        pill.mode = {idle.x - kModeW, idle.y, kModeW, idle.h};
    pill.remove = {idle.right(), idle.y, kRemoveW, idle.h};
    const float left = mode >= 0 ? pill.mode.x : idle.x;
    pill.whole = {left, idle.y, pill.remove.right() - left, idle.h};
    return pill;
}

std::vector<int> View::inputsOf(int op) const {
    std::vector<int> inputs;
    for (int from = 0; from < kNodes; ++from)
        if (from != op && nodeOn(from) && wired(routeParam(from, op)))
            inputs.push_back(from);
    return inputs;
}

// The names of the nodes routed into the operator and their modes' names, its feedback's row, or a line saying nothing
// is routed into it yet; the buttons and labels draw themselves.
void View::paintInputs(Canvas &c, int op) {
    const std::vector<int> inputs = inputsOf(op);
    const bool feedback = wired(routeParam(op, op));
    if (inputs.empty() && !feedback) {
        c.text({kInspector.x + 16.0f, kInputsTop, kInspector.w - 32.0f, 40.0f},
               trf(Str::InputsNone, {nodeTitle(op)}), kTextBody, kDim, Align::Center);
        return;
    }
    const float step = inputsStep(static_cast<int>(inputs.size()) + (feedback ? 1 : 0));
    float y = kInputsTop;
    for (int from : inputs) {
        const bool hovered = hover_ && hover_->kind == Kind::MatrixLabel && hover_->a == from &&
                             hover_->rect.y == inputsName(y).y;
        c.text(inputsName(y), kNodeNames[from], kTextBody, hovered ? kText : nodeColor(from), Align::Left, true);
        const auto mode = routeMode(value(routeModeParam(from, op)));
        c.text(inputsModeName(y), routeModeName(mode), kTextLabel, kDim, Align::Left);
        y += step;
    }
    if (feedback) {
        c.text(inputsName(y), kNodeNames[op], kTextBody, nodeColor(op), Align::Left, true);
        c.text({inputsMode(y, 0).x, y, inputsDepth(y).x - inputsMode(y, 0).x - 6.0f, 18.0f},
               trf(Str::InputsFeedback, {kNodeNames[op]}), kTextLabel, kDim, Align::Left);
    }
}

InspectorTab View::opTabShown(int op) const {
    const InspectorTab tab = controller_.layout().opTab;
    return tab == InspectorTab::Scope && isNoise(op) ? InspectorTab::Envelope : tab;
}

// Whether a source modulates one of the knobs on an operator's tab, which shows a dot on the tab while it is hidden.
bool View::opTabModulated(int op, InspectorTab tab) const {
    std::vector<int> params;
    if (tab == InspectorTab::Unison && !isNoise(op))
        params = {paramIndex(op, OpParam::Unison), paramIndex(op, OpParam::UnisonDetune),
                  paramIndex(op, OpParam::UnisonSpread), paramIndex(op, OpParam::PhaseRandom)};
    if (tab == InspectorTab::Envelope) {
        const EnvIndices e = envIndices(op);
        params = {e.attack, e.hold, e.decay, e.sustain, e.release};
    }
    for (const Assignment &m : assignments())
        if (std::find(params.begin(), params.end(), m.target) != params.end())
            return true;
    return false;
}

// The operator's output in the newest voice, a point per bin across the display: the cycles the switch in the corner
// picks, at their real level between dashed lines at full scale, the level its peaks follow in the corner. A slow operator's span is drawn as it fills, up to
// its cursor, the last one's rest faintly after it. With nothing captured, the wave's plain shape is drawn faintly.
void View::paintScope(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const int cycles = controller_.layout().scopeCycles;
    const Rect inner{r.x + 8.0f, r.y + 28.0f, r.w - 16.0f, r.h - 40.0f};
    const float middle = inner.center().y;
    const float half = inner.h * 0.5f;
    c.line({inner.x, inner.y}, {inner.right(), inner.y}, kBorder, 1.0f, true);
    c.line({inner.x, inner.bottom()}, {inner.right(), inner.bottom()}, kBorder, 1.0f, true);
    c.line({inner.x, middle}, {inner.right(), middle}, kBorder.alpha(0.6f), 1.0f);
    for (int k = 1; k < cycles; ++k) {
        const float x = inner.x + inner.w * k / cycles;
        c.line({x, inner.y - 6.0f}, {x, inner.bottom() + 6.0f}, kBorder.alpha(0.6f), 1.0f, true);
    }

    const int bins = cycles * kScopeBinsPerCycle;
    if (telemetry_.voices == 0 || telemetry_.scopeBins != bins) {
        const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))));
        for (int k = 0; k < cycles; ++k) {
            const Rect cycle{inner.x + inner.w * k / cycles, inner.y, inner.w / cycles, inner.h};
            const std::vector<Point> points = wavePoints(wave, cycle);
            c.polyline(points.data(), points.size(), kDim.alpha(0.5f), 1.5f, true);
        }
        return;
    }
    // A bin per point, but no more points than the display has pixels across, twice over.
    const int step = std::max(1, bins / static_cast<int>(inner.w * 2.0f));
    auto at = [&](int bin) { // full scale at the dashed lines; louder runs a little past them
        const float level = std::clamp(telemetry_.scope[bin], -1.15f, 1.15f);
        return Point{inner.x + inner.w * bin / (bins - 1), middle - level * half};
    };
    const int cursor = telemetry_.scopeCursor < 0.0f
                           ? bins
                           : std::clamp(static_cast<int>(telemetry_.scopeCursor * bins), 0, bins);
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
void View::paintNoiseSlope(Canvas &c, Rect r, int op) {
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

void View::paintFilterResponse(Canvas &c, Rect r, int typeParam, int slopeParam, int cutoffParam,
                               int resonanceParam, int keyTrackParam, double pitch, Color color,
                               const Spectrum *spectrum) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const Rect inner{r.x + 10.0f, r.y + 10.0f, r.w - 20.0f, r.h - 20.0f};
    auto yOfDb = [&](float db) {
        const float t = (std::clamp(db, kResponseMinDb, kResponseMaxDb) - kResponseMinDb) /
                        (kResponseMaxDb - kResponseMinDb);
        return inner.bottom() - t * inner.h;
    };
    // A grid as the Param EQ's: every 12 dB, 0 dB the strongest, and a line at 100 Hz, 1 kHz and 10 kHz.
    for (float db : {-24.0f, -12.0f, 0.0f, 12.0f})
        c.line({inner.x, yOfDb(db)}, {inner.right(), yOfDb(db)}, kBorder.alpha(db == 0.0f ? 1.0f : 0.5f), 1.0f,
               db != 0.0f);
    for (double hz : {100.0, 1000.0, 10000.0}) {
        const float x = inner.x + static_cast<float>(normFromCutoff(hz)) * inner.w;
        c.line({x, inner.y}, {x, inner.bottom()}, kBorder.alpha(0.5f), 1.0f, true);
    }
    if (spectrum) // what the filter puts out, behind its response
        paintSpectrum(c, inner, *spectrum);

    const auto type = static_cast<FilterType>(std::lround(value(typeParam)));
    const double cutoff = value(cutoffParam);
    const double resonance = value(resonanceParam);
    const int steps = type == FilterType::Comb ? 240 : 96; // comb teeth need more points
    const double slope = value(slopeParam);
    auto response = [&](double cutoffNorm, double res) {
        std::vector<double> key = curveKey(Curve::Filter, inner);
        key.insert(key.end(), {static_cast<double>(type), slope, cutoffNorm, res});
        return curves_.get(key, [&] {
            std::vector<Point> points;
            for (int i = 0; i <= steps; ++i) {
                const double x = static_cast<double>(i) / steps;
                const double magnitude =
                    filterResponse(type, filterSlope(slope), cutoffFromNorm(cutoffNorm), res, cutoffFromNorm(x));
                const float db = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-6)));
                points.push_back({inner.x + static_cast<float>(x) * inner.w, yOfDb(db)});
            }
            return points;
        });
    };
    // Modulated, the curve shows how far its modulation takes it either way, faintly (as a knob's ring
    // does), and while notes play it follows where the newest voice is. Key tracking moves all of it with
    // `pitch` from C4 on, an octave per octave at 100 %, as far as the filter's cutoff goes (down to 10 Hz).
    const double keyShift = value(keyTrackParam) * (pitch - 60.0) / 12.0 / std::log2(kMaxCutoff / kMinCutoff);
    auto tracked = [&](double cutoffNorm) { return std::clamp(cutoffNorm + keyShift, normFromCutoff(10.0), 1.0); };
    auto reach = [&](int param, double set) {
        double low = set, high = set;
        forEachMod(param, [&](const Assignment &m) {
            if (m.bypassed || !sourceOn(m.source))
                return;
            const auto [down, up] = modReach(m);
            low += down;
            high += up;
        });
        return std::pair{std::clamp(low, 0.0, 1.0), std::clamp(high, 0.0, 1.0)};
    };
    const auto [cutoffLow, cutoffHigh] = reach(cutoffParam, cutoff);
    const auto [resonanceLow, resonanceHigh] = reach(resonanceParam, resonance);
    if (cutoffLow != cutoffHigh || resonanceLow != resonanceHigh) {
        for (const auto &[at, res] : {std::pair{cutoffLow, resonanceLow}, std::pair{cutoffHigh, resonanceHigh}}) {
            const std::vector<Point> edge = response(tracked(at), res);
            c.polyline(edge.data(), edge.size(), color.alpha(0.3f), 1.0f);
        }
    }
    const std::vector<Point> points = response(tracked(liveAmount(cutoffParam)), liveAmount(resonanceParam));
    std::vector<Point> area = points;
    area.push_back({inner.right(), inner.bottom()});
    area.push_back({inner.x, inner.bottom()});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    const float cutoffX = inner.x + static_cast<float>(cutoff) * inner.w;
    c.line({cutoffX, inner.y}, {cutoffX, inner.bottom()}, color.alpha(0.3f), 1.0f, true);
}

void View::paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    for (int i = 1; i < 4; ++i)
        c.line({r.x + 8.0f, r.y + r.h * i / 4.0f}, {r.right() - 8.0f, r.y + r.h * i / 4.0f}, kBorder.alpha(0.6f), 1.0f);

    const EnvIndices e = envIndices(env);
    const double sustain = value(e.sustain);
    const EnvShape s = envShape(r, e, [this](int index) { return value(index); }, envScale(env));
    const float height = s.bottom - s.top;
    std::vector<double> key = curveKey(Curve::Envelope, r);
    key.insert(key.end(), {s.scale, value(e.attack), value(e.hold), value(e.decay), sustain, value(e.release),
                           value(e.curve(0)), value(e.curve(1)), value(e.curve(2))});
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points{s.p0};
        for (int segment = 0; segment < 3; ++segment) {
            if (segment == 1)
                points.push_back(s.ph); // the end of the hold
            for (int i = 1; i <= 24; ++i)
                points.push_back(envSegmentPoint(s, segment, i / 24.0, value(e.curve(segment)), sustain));
        }
        return points;
    });

    std::vector<Point> area = points;
    area.push_back({s.p4.x, s.bottom});
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);

    // While notes play, a line across at the newest voice's level: an operator's envelope's, or a mod envelope's.
    if (live && telemetry_.voices > 0 && (env < kOps ? nodeOn(env) : sourceOn(env - kOps))) {
        const float level = std::clamp(env < kOps ? telemetry_.opEnvLevels[env] : telemetry_.sources[env - kOps], 0.0f, 1.0f);
        const float y = s.bottom - level * height;
        c.line({r.x + 8.0f, y}, {r.right() - 8.0f, y}, color.alpha(0.35f), 1.0f, true);
        // And a dot on the curve, as far along the stage the envelope is in as it has come: along the attack, decay
        // or release by its bend, across the hold, and at the sustain level's point through the sustain. Where the
        // envelope follows its knobs, that is where the line meets the curve. Where it doesn't (a note let go during
        // the decay releases from above the sustain; modulation moves its sustain), the dot keeps to the curve, and
        // the line shows the level.
        const float position = env < kOps ? telemetry_.opEnvPos[env] : telemetry_.envPos[env - kOps];
        if (position >= 0.0f) {
            const int stage = std::min(static_cast<int>(position), 4);
            const double along = std::clamp(static_cast<double>(position) - stage, 0.0, 1.0);
            Point dot = s.p2; // the sustain
            if (stage == 1)
                dot = {s.p1.x + static_cast<float>(along) * (s.ph.x - s.p1.x), s.top};
            else if (stage != 3)
                dot = envSegmentPoint(s, stage / 2, along, value(e.curve(stage / 2)), sustain);
            c.fillCircle(dot, 7.0f, color.alpha(0.25f));
            c.fillCircle(dot, 3.5f, kText);
        }
    }
    // A bipolar envelope swings its targets about its middle level: a line there, and a sign in the corner.
    if (env >= kOps && sourceBipolar(env - kOps)) {
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
    const float right = r.right() - (w.a >= kOps && sourceBipolar(w.a - kOps) ? 24.0f : 8.0f);
    const float width = c.textWidth(text, kTextLabel);
    c.fillRect({right - width - 5.0f, r.y + 2.0f, width + 10.0f, 14.0f}, kDisplayColor.alpha(0.85f), kRadiusMark);
    c.text({r.x, r.y + 2.0f, right - r.x, 14.0f}, text, kTextLabel, kText, Align::Right);
}

// How many cycles a random LFO's display shows.


void View::paintLfo(Canvas &c, Rect r, int lfo) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const Rect inner{r.x + 10.0f, r.y + 8.0f, r.w - 20.0f, r.h - 16.0f};
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const float zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? inner.bottom() : inner.center().y;
    c.line({inner.x, zero}, {inner.right(), zero}, kBorder, 1.0f);
    const auto shape = static_cast<LfoShape>(std::clamp(
        static_cast<int>(std::lround(value(paramIndex(lfo, SourceParam::Shape)))), 0, static_cast<int>(LfoShape::Count) - 1));
    // Where notes start it, past the cycle's start.
    if (const double start = value(paramIndex(lfo, SourceParam::Phase)); lfoPhaseUsed(lfo) && start > 0.0 && start < 1.0) {
        const float x = inner.x + static_cast<float>(start) * inner.w;
        c.line({x, r.y + 4.0f}, {x, r.bottom() - 4.0f}, kSourceColors[lfo].alpha(0.45f), 1.0f);
    }
    std::vector<Point> points;
    auto at = [&](double x, double y) {
        points.push_back({inner.x + static_cast<float>(x) * inner.w, inner.center().y - static_cast<float>(y) * inner.h * 0.5f});
    };
    if (shape != LfoShape::Random || !randomSweep(lfo)) {
        std::vector<double> key = curveKey(Curve::Lfo, inner);
        key.push_back(static_cast<double>(shape));
        if (shape == LfoShape::Custom)
            keyShape(key, shapeCountIndex(lfo));
        if (shape == LfoShape::Random)
            key.insert(key.end(), {value(paramIndex(lfo, SourceParam::RandomSmooth)),
                                   value(paramIndex(lfo, SourceParam::RandomChaos))});
        points = curves_.get(key, [&] {
            if (shape == LfoShape::Custom) {
                auto get = [this](int index) { return value(index); };
                shapeOutline(get, lfo, 96, at);
            } else if (shape == LfoShape::Random) {
                // Four cycles of a made-up run with this LFO's chaos and smoothing, until it runs.
                const double smooth = value(paramIndex(lfo, SourceParam::RandomSmooth));
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
    plotLine(c, points.data(), points.size(), color, 2.0f);

    // Running, a random LFO's last cycles as they came, swept across: each new cycle takes the next quarter,
    // round from the right end to the left, the older ones fainter, and the live dot rides the newest.
    const RandomTrace &trace = randomTraces_[lfo];
    const int newest = std::max(trace.count - 1, 0) % kRandomCycles;
    if (shape == LfoShape::Random && randomSweep(lfo)) {
        const double smooth = value(paramIndex(lfo, SourceParam::RandomSmooth));
        for (int k = 0; k < std::min(trace.count, kRandomCycles); ++k) {
            const int age = (newest - k + kRandomCycles) % kRandomCycles; // 0 for the newest
            const auto [from, to] = trace.glides[k];
            points.clear();
            for (int i = 0; i <= 24; ++i)
                at((k + i / 24.0) / kRandomCycles, randomGlide(from, to, i / 24.0, smooth));
            plotLine(c, points.data(), points.size(), color.alpha(1.0f - 0.2f * age), 2.0f);
        }
    }

    if (telemetry_.voices > 0 && sourceOn(lfo)) {
        const double x = shape == LfoShape::Random ? (newest + randomPhaseNow(lfo)) / kRandomCycles : lfoPhaseNow(lfo);
        const Point dot{inner.x + static_cast<float>(x) * inner.w,
                        inner.center().y - static_cast<float>(lfoValueNow(lfo)) * inner.h * 0.5f};
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }
}

// A widened LFO card's shape: the grid and the shape in the plot's frame, filled toward its zero line, and where
// the newest voice is; its points and bends are widgets, but for while the full-size editor has the shape, which the
// plot then only shows. Under it the point under the mouse in numbers, and an arrow by each of the grid's steppers.
void View::paintLfoCard(Canvas &c, float card, int lfo) {
    const Color color = kSourceColors[lfo];
    const Rect r = lfoCardPlot(card);
    c.fillRect(r.inset(-8.0f), kDisplayColor, kRadiusCard);
    // The zero line: in the middle, or at the bottom for a unipolar LFO, which only swings up from it.
    const double zero = value(paramIndex(lfo, SourceParam::Unipolar)) >= 0.5 ? -1.0 : 0.0;
    paintShapeGrid(c, r, zero, false);
    auto get = [this](int index) { return value(index); };
    std::vector<double> key = curveKey(Curve::LfoEditor, r);
    keyShape(key, shapeCountIndex(lfo));
    const std::vector<Point> points = curves_.get(key, [&] {
        std::vector<Point> points;
        shapeOutline(get, lfo, 200, [&](double x, double y) { points.push_back(areaAt(r, x, y)); });
        return points;
    });
    std::vector<Point> area = points;
    area.push_back(areaAt(r, 1.0, zero));
    area.push_back(areaAt(r, 0.0, zero));
    c.fillPolygon(area.data(), area.size(), color.alpha(0.12f));
    plotLine(c, points.data(), points.size(), color, 2.0f);
    if (telemetry_.voices > 0 && sourceOn(lfo)) {
        const Point dot = areaAt(r, lfoPhaseNow(lfo), lfoValueNow(lfo));
        c.fillCircle(dot, 7.0f, color.alpha(0.25f));
        c.fillCircle(dot, 3.5f, kText);
    }

    const Rect readout = lfoCardReadout(card);
    c.fillRect(readout, kDisplayColor, kRadiusControl);
    const Rect text{readout.x + 8.0f, readout.y, readout.w - 16.0f, readout.h};
    const Widget *focus = drag_.kind == DragKind::ShapePoint ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (focus && focus->kind == Kind::ShapePoint && focus->a == lfo && shapeEditing() != lfo) {
        // What the LFO puts out there: a unipolar one maps -1..1 onto 0..1.
        const double y = value(shapePointIndex(lfo, focus->b, PointField::Y));
        const std::string level = zero < 0.0 ? number("%.2f", 0.5 * (y + 1.0)) : number("%+.2f", y);
        c.text(text, trf(Str::CaptionCardPointNumber, {std::to_string(focus->b + 1)}), kTextLabel, kText, Align::Left,
               true);
        c.text(text, trf(Str::CaptionCardPoint, {number("%.1f", shapePointX(lfo, focus->b) * 100.0), level}), kTextLabel,
               kDim, Align::Right);
    } else {
        c.text(text, tr(Str::HintCardPoint), kTextLabel, kFaint, Align::Left);
    }
    for (int axis = 0; axis < 2; ++axis) {
        const Rect down = lfoCardGridButton(card, axis, 0);
        axisIcon(c, {down.x - 8.0f, down.center().y}, axis, kFaint);
    }
    // While the full-size editor has the shape, this row is greyed out like the rest of the card's editor (whose
    // widgets are dimmed as disabled ones are).
    if (shapeEditing() == lfo)
        c.fillRect({readout.x, readout.y, r.w + 16.0f, readout.h}, kPanel.alpha(0.6f), kRadiusMark);
}

void View::paintSources(Canvas &c) {
    if (controller_.layout().sourcesCollapsed)
        return; // the chips and the toggle are widgets
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
    const bool lfo = sourceTypeOf(s) == SourceType::Lfo;
    const bool dragged = drag_.kind == DragKind::SourceMove && drag_.widget.a == s;
    c.fillRect(p, kCardColor, kRadiusCard);
    c.strokeRect(p, dragged ? color : (sourceOn(s) ? color.alpha(0.35f) : kBorder), dragged ? 1.5f : 1.0f, kRadiusCard);
    if (sourceFolded(s)) { // the grip's dots under the handle (widgets), then its name down the strip
        const bool grip =
            (hover_ && hover_->kind == Kind::SourceGrip && hover_->a == s && drag_.kind == DragKind::None) || dragged;
        for (int d = 0; d < 6; ++d)
            c.fillCircle({p.center().x - 5.0f + (d / 2) * 5.0f, p.y + 35.0f + (d % 2) * 5.0f}, 1.4f, grip ? kText : kFaint);
        std::string name = sourceName(s);
        if (sourceTypeOf(s) == SourceType::Random) // "Random" too long for the strip: its short name, by its number
            name = trf(Str::SourceFoldedRandom, {name.substr(name.find_last_of(' ') + 1)});
        paintDownward(c, {p.x, p.y + 104.0f, p.w, p.h - 110.0f}, upperCase(name), sourceOn(s) ? color : kDim);
        return;
    }

    // Title row: the handle (a widget) and the name, grey while the source is off; the row drags the card.
    // In capitals, as an effect card's title.
    c.text({p.x + 40.0f, p.y + 7.0f, 80.0f, 21.0f}, upperCase(sourceName(s)), kTextBody, sourceOn(s) ? kText : kDim,
           Align::Left, true);

    if (s == wideLfo()) {
        paintLfoCard(c, card, s);
    } else if (lfo) {
        paintLfo(c, sourceDisplay(card, std::lround(value(paramIndex(s, SourceParam::Shape))) ==
                                             static_cast<long>(LfoShape::Random)),
                 s);
    } else if (sourceTypeOf(s) == SourceType::Env) {
        paintEnvelope(c, sourceDisplay(card), kOps + s, color, true);
    } else {
        // Random and MIDI: what the newest voice gets from it, as a bar from the left (from the middle while
        // it is bipolar, where its depths swing both ways), and in numbers; Random says what it does over its knobs'
        // row, having none.
        if (sourceTypeOf(s) == SourceType::Random)
            c.text({p.x + 16.0f, p.y + 40.0f, p.w - 32.0f, 30.0f}, tr(Str::HintRandomSource), kTextLabel, kDim);
        const Rect d = sourceDisplay(card);
        c.fillRect(d, kBackground, kRadiusCard);
        const bool live = sourceOn(s) && telemetry_.voices > 0;
        const double out = live ? std::clamp(static_cast<double>(telemetry_.sources[s]), 0.0, 1.0) : 0.0;
        const Rect track{d.x + 12.0f, d.center().y - 3.0f, d.w - 24.0f, 6.0f};
        c.fillRect(track, kTrack, kRadiusMark);
        const bool bipolar = sourceBipolar(s);
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

int View::routingHoveredNode() const {
    if (drag_.kind != DragKind::None || !hover_) // not while anything is dragged
        return -1;
    const Kind k = hover_->kind;
    return k == Kind::Node || k == Kind::Port || k == Kind::NodeDelete ? hover_->a : -1;
}

Point View::nodeDeleteAt(int node, Point center) {
    if (!isBoxNode(node)) // on the circle, up and to the right
        return {center.x + 0.72f * kNodeRadius, center.y - 0.72f * kNodeRadius};
    return {center.x + kFilterHalfW - 2.0f, center.y - kFilterHalfH + 2.0f}; // at a box's corner
}

void View::paintNode(Canvas &c, const Widget &w) {
    const bool hovered = isHovered(w) || isActive(w);
    const bool selected = controller_.layout().selectedNode == w.a;
    const Point center = w.rect.center();

    if (isBoxNode(w.a)) { // a filter's box with its type, or a shaper's, squarer, with its curve's name
        const bool shaper = isShaperNode(w.a);
        const float corner = shaper ? kRadiusMark : kRadiusControl;
        const Color color = nodeColor(w.a);
        const bool live = boxHasInput(w.a);
        if (live && telemetry_.voices > 0 && reachesOut(w.a))
            c.fillRect(w.rect.inset(-6.0f), color.alpha(0.12f), corner + 4.0f);
        // The node the inspector shows is filled with its color, like its tab's plate, and ringed in it.
        c.fillRect(w.rect, hovered ? kTrack : kRaised, corner);
        if (selected)
            c.fillRect(w.rect, color.alpha(hovered ? kSelectedFill + 0.07f : kSelectedFill), corner);
        c.strokeRect(w.rect, live ? color : kFaint, selected ? 2.0f : 1.5f, corner);
        if (selected)
            c.strokeRect(w.rect.inset(-kSelectedRing), color, 2.0f, corner + kSelectedRing);
        c.text({w.rect.x, w.rect.y + 2.0f, w.rect.w, 16.0f}, kNodeNames[w.a], kTextValue, kText, Align::Center, true);
        const int box = w.a - kOps; // the filter, or past them the shaper
        const std::string kind =
            shaper ? clipModeName(clipMode(value(paramIndex(shaperOfNode(w.a), ShaperParam::Mode))))
                   : filterLabel(static_cast<FilterType>(std::lround(value(paramIndex(box, FilterParam::Type)))),
                                 value(paramIndex(box, FilterParam::Slope)));
        c.text({w.rect.x, w.rect.y + 17.0f, w.rect.w, 14.0f}, kind, kTextLabel, selected ? kText.alpha(0.8f) : kDim);
        return;
    }

    const int op = w.a;
    bool heard = value(paramIndex(op, OpParam::Out)) > 0.0;
    for (int filter = 0; filter < kFilters; ++filter)
        heard = heard || (nodeOn(kOps + filter) && value(sendIndex(op, filter)) > 0.0);
    for (int shaper = 0; shaper < kShapers; ++shaper)
        heard = heard || (nodeOn(kOps + kFilters + shaper) && value(shaperSendIndex(op, shaper)) > 0.0);
    // Its halo lights up while playing only if it ends up in the output, as brightly as its envelope and the routes
    // that take it there let it through, where modulation takes them now: one an LFO opens from 0 % lights up too.
    const float level = std::min(telemetry_.opLevels[op], 1.0f) * static_cast<float>(std::sqrt(outLevel(op)));
    if (level > 0.01f) {
        if (glowing > 0.0f) // feathered, as strongly as the glow is set
            featherCircle(c, center, kNodeRadius, 16.0f, kAccent.alpha(0.3f * level));
        else // with the glow off, a plain ring of light, so a sounding operator still shows
            c.fillCircle(center, kNodeRadius + 7.0f, kAccent.alpha(0.22f * level));
    }
    if (std::lround(value(paramIndex(op, OpParam::Unison))) > 1 && !isNoise(op)) {
        // Unison: a stack of rings peeking out on both sides.
        c.strokeCircle({center.x - 4.5f, center.y}, kNodeRadius, (heard ? kAccent : kFaint).alpha(0.45f), 1.2f);
        c.strokeCircle({center.x + 4.5f, center.y}, kNodeRadius, (heard ? kAccent : kFaint).alpha(0.45f), 1.2f);
    }
    // The node the inspector shows is filled with the accent, like its tab's plate, and ringed in it.
    c.fillCircle(center, kNodeRadius, hovered ? kTrack : kRaised);
    if (selected)
        c.fillCircle(center, kNodeRadius, kAccent.alpha(hovered ? kSelectedFill + 0.07f : kSelectedFill));
    c.strokeCircle(center, kNodeRadius, heard ? kAccent : kFaint, selected ? 2.0f : 1.5f);
    if (selected)
        c.strokeCircle(center, kNodeRadius + kSelectedRing, kAccent, 2.0f);
    const Color caption = selected ? kText.alpha(0.8f) : kDim; // its wave and ratio, lighter on its fill
    // Wave glyph above the letter, ratio below it.
    const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(op, OpParam::Wave))));
    const std::vector<Point> glyph = waveIcon(wave, {center.x - 6.0f, center.y - 16.0f, 12.0f, 5.0f});
    c.polyline(glyph.data(), glyph.size(), caption, 1.0f);
    c.text({center.x - 20.0f, center.y - 11.0f, 40.0f, 20.0f}, kNodeNames[op], kTextHeading, kText, Align::Center, true);
    char ratio[16];
    const double r = ratioHundredths(value(paramIndex(op, OpParam::Ratio))) / 100.0;
    std::snprintf(ratio, sizeof(ratio), "\xC3\x97%.2g", r);
    if (isFixed(op))
        std::snprintf(ratio, sizeof(ratio), "%s", fixedHzText(op, true).c_str());
    if (isNoise(op)) {
        // No ratio: its color, or its slope between the named ones.
        const double db = std::round(value(paramIndex(op, OpParam::NoiseSlope)) * 60.0) / 10.0;
        const char *const name = db == 0.0   ? tr(Str::NoiseWhiteCaption)
                                 : db == 3.0 ? tr(Str::NoisePinkCaption)
                                 : db == 6.0 ? tr(Str::NoiseBrownCaption)
                                             : nullptr;
        if (name)
            std::snprintf(ratio, sizeof(ratio), "%s", name);
        else
            std::snprintf(ratio, sizeof(ratio), "-%.1fdB", db);
    }
    c.text({center.x - 22.0f, center.y + 5.0f, 44.0f, 14.0f}, ratio, kTextLabel, caption); // inside the ring
}

namespace {
// An edit button, the same pencil wherever it opens an editor: lit in `color` under the mouse, and outlined in it
// while what it opens is open.
void editButton(Canvas &c, Rect r, Color color, bool hovered, bool open) {
    c.fillRect(r, open ? color.alpha(0.22f) : hovered ? color.alpha(0.3f) : kRaised, kRadiusControl);
    if (open)
        c.strokeRect(r, color, 1.0f, kRadiusControl);
    editIcon(c, r.center(), open || hovered ? kText : kDim);
}
} // namespace

void View::paintButtonFace(Canvas &c, const Widget &w, Color fill, bool lit, Color color, float radius) {
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
    switch (w.kind) {
    case Kind::Knob:
        if (isRatioDisplay(w))
            paintRatio(c, w);
        else
            paintKnob(c, w);
        break;
    case Kind::Node:
        paintNode(c, w);
        break;
    case Kind::NodeDelete: { // as on the effects graph's nodes (see paintFxGraph())
        const Point x = w.rect.center();
        const Color ink = hovered ? kText : kDim;
        c.fillCircle(x, 7.5f, hovered ? kRaised : kCardColor);
        c.strokeCircle(x, 7.5f, ink, 1.2f);
        c.line({x.x - 3.0f, x.y - 3.0f}, {x.x + 3.0f, x.y + 3.0f}, ink, 1.4f);
        c.line({x.x + 3.0f, x.y - 3.0f}, {x.x - 3.0f, x.y + 3.0f}, ink, 1.4f);
        break;
    }
    case Kind::NodeGhost: {
        // Dashed, like the cards that add things, and in the node's color while hovered; the one the inspector shows
        // filled with its color, as a node shown in the graph is.
        const bool selected = controller_.layout().selectedNode == w.a;
        const Color color = hovered || selected ? nodeColor(w.a) : kFaint;
        const float corner = isShaperNode(w.a) ? kRadiusMark : isFilterNode(w.a) ? kRadiusControl : w.rect.h * 0.5f;
        if (selected)
            c.fillRect(w.rect, nodeColor(w.a).alpha(kSelectedFill), corner);
        c.strokeRect(w.rect, color, 1.0f, corner, true);
        c.text(w.rect, kNodeNames[w.a], kTextLabel, selected ? kText : color, Align::Center, true);
        break;
    }
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
        const char *text = power || modeChoice ? ""
                           : isPartialCount(w.param) ? count.c_str()
                           : w.param == paramIndex(GlobalParam::Mono) ? tr(w.b == 0 ? Str::SwitchPoly : Str::SwitchMono)
                           : w.param == paramIndex(GlobalParam::Legato) ? tr(Str::SwitchLegato)
                           : field == FxParam::ShaperMode               ? tr(clipModes[std::clamp(w.b, 0, 2)])
                           : field == FxParam::FilterType ? filterTypeShortName(static_cast<FilterType>(w.b))
                           : field == FxParam::PhaserStereo || field == FxParam::FlangerStereo ||
                                     field == FxParam::AmpStereo
                               ? tr(Str::SwitchStereo)
                           : field == FxParam::AmpMic ? tr(kMicShortNames[std::clamp(w.b, 0, 2)])
                           : field == FxParam::MdRms                                            ? tr(Str::SwitchRms)
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
        if (modeChoice)
            color = routeColor(routeModeFrom(w.param), routeModeTo(w.param));
        paintButtonFace(c, w, on ? color.alpha(0.22f) : (hovered ? kTrack : kRaised), on, color, kRadiusControl);
        if (field == FxParam::FilterType) // the filter types show as icons, named in their tooltips
            filterIcon(c, {w.rect.center().x - 10.0f, w.rect.y + 5.0f, 20.0f, w.rect.h - 10.0f},
                       static_cast<FilterType>(w.b), on ? color : kDim);
        else if (power)
            powerIcon(c, w.rect.center(), on ? color : (hovered ? kText : kDim));
        else if (modeChoice)
            routeModeIcon(c, w.rect.center(), static_cast<RouteMode>(w.b), on ? color : (hovered ? kText : kDim));
        else if (fixedSwitchOp(w.param) >= 0) // a keyboard, crossed out while the operator doesn't follow it
            keyboardIcon(c, w.rect.center(), on ? color : (hovered ? kText : kFaint), on);
        else if (shaperFieldOf(w.param) == ShaperParam::Link) // a voice shaper's link, as the shaper effect's
            fxSwitchIcon(c, w.rect.center(), FxParam::ShaperLink, on ? color : (hovered ? kText : kDim));
        else if (shaperFieldOf(w.param) == ShaperParam::Symmetric)
            fxSwitchIcon(c, w.rect.center(), FxParam::ShaperSymmetric, on ? color : (hovered ? kText : kDim));
        else if (field == FxParam::PhaserNegative || field == FxParam::FlangerNegative)
            polaritySign(c, w.rect.center(), w.b == 1, on ? color : (hovered ? kText : kDim));
        else if (isFxIconSwitch(field))
            fxSwitchIcon(c, w.rect.center(), field, on ? color : (hovered ? kText : kDim));
        else if (isLfoSync(w.param)) // an LFO's SYNC, the effects' metronome
            fxSwitchIcon(c, w.rect.center(), FxParam::DelaySync, on ? color : (hovered ? kText : kDim));
        else if (clockMode)
            lfoClockIcon(c, w.rect.center(), static_cast<LfoClock>(w.b), on ? color : (hovered ? kText : kDim));
        else if (sourceFieldOf(w.param) == SourceParam::Unipolar) // humps on a baseline, as a MODS row's
            polarityIcon(c, w.rect.center(), false, on ? color : (hovered ? kText : kDim));
        else if (sourceFieldOf(w.param) == SourceParam::Bipolar) // a Random card's: either one, as a MODS row's
            polarityIcon(c, w.rect.center(), w.b == 1, on ? color : (hovered ? kText : kDim));
        else
            c.text(w.rect, text, kTextLabel, on ? kText : kDim, Align::Center, on);
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
            const std::vector<Point> outline =
                viewTabOutline({w.rect.x + 0.5f, w.rect.y + 0.5f, w.rect.w - 1.0f, w.rect.h}, kViewTabFlare);
            std::vector<Point> fill = outline; // down past the panel's border, to open it without a seam left
            fill.push_back({outline.back().x, w.rect.bottom() + 2.0f});
            fill.push_back({outline.front().x, w.rect.bottom() + 2.0f});
            c.fillPolygon(fill.data(), fill.size(), kPanel);
            if (w.rect.x <= kRouting.x) // squaring the panel's corner its side runs down
                c.fillRect({w.rect.x, w.rect.bottom(), kPanelRadius, kPanelRadius}, kPanel);
            c.polyline(outline.data(), outline.size(), kBorder, 1.0f);
            c.line({w.rect.x + kViewTabRadius, w.rect.y + 2.0f}, {w.rect.right() - kViewTabRadius, w.rect.y + 2.0f},
                   kAccent, 2.0f);
        } else if (hovered) {
            const std::vector<Point> outline = viewTabOutline({w.rect.x, w.rect.y, w.rect.w, w.rect.h - 0.5f}, 0.0f);
            c.fillPolygon(outline.data(), outline.size(), kPanel.alpha(0.6f));
        }
        c.text(w.rect, tr(kViewNames[w.a]), kTextBody, selected || hovered ? kText : kDim, Align::Center, true);
        break;
    }
    case Kind::OpTab: {
        // Names on the tabs' line, the shown one's accent over it; a hidden one with a modulated knob has a dot.
        const int op = controller_.layout().selectedNode;
        const auto tab = static_cast<InspectorTab>(w.a);
        const bool selected = opTabShown(op) == tab;
        const Str names[] = {isNoise(op) ? Str::TabSpectrum : Str::TabUnison, Str::TabEnvelope, Str::TabScope,
                             Str::TabInputs};
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
        const bool selected = controller_.layout().scopeCycles == w.b;
        c.fillRect(w.rect, selected ? kAccent.alpha(0.22f) : (hovered ? kTrack : kRaised), kRadiusMark);
        if (selected)
            c.strokeRect(w.rect, kAccent, 1.0f, kRadiusMark);
        c.text(w.rect, std::to_string(w.b), kTextLabel, selected ? kText : (hovered ? kText : kDim), Align::Center,
               selected);
        break;
    }
    case Kind::NodeTab: {
        const bool selected = controller_.layout().selectedNode == w.a;
        const bool enabled = nodeOn(w.a);
        const Color color = nodeColor(w.a);
        c.fillRect(w.rect, selected ? color.alpha(enabled ? 1.0f : 0.35f) : (hovered ? kTrack : kRaised.alpha(enabled ? 1.0f : 0.4f)),
                   kRadiusControl);
        // An enabled node's name takes a little of its color.
        c.text(w.rect, kNodeNames[w.a], kTextBody, selected ? kBackground : (enabled ? mix(kDim, color, 0.3f) : kFaint),
               Align::Center, true);
        if (soundsOut(w.a))
            c.fillCircle({w.rect.right() - 5.0f, w.rect.y + 5.0f}, 2.0f, selected ? kBackground : color);
        break;
    }
    case Kind::WaveButton: {
        const bool selected = std::lround(value(paramIndex(w.a, OpParam::Wave))) == w.b;
        paintButtonFace(c, w, selected ? kAccent.alpha(0.18f) : (hovered ? kTrack : kRaised), selected, kAccent, kRadiusControl);
        const std::vector<Point> points = waveIcon(static_cast<OpWave>(w.b), w.rect.inset(7.0f));
        c.polyline(points.data(), points.size(), selected ? kAccent : kDim, 1.5f);
        break;
    }
    case Kind::TypeButton: {
        const bool selected = std::lround(value(paramIndex(w.a, FilterParam::Type))) == w.b;
        paintButtonFace(c, w, selected ? kFilterColor.alpha(0.18f) : (hovered ? kTrack : kRaised), selected, kFilterColor,
                        kRadiusControl);
        filterIcon(c, w.rect.inset(6.0f), static_cast<FilterType>(w.b), selected ? kFilterColor : kDim);
        break;
    }
    case Kind::Port: { // under the mouse, or dragged from, the node's outline glows in its color
        if (!hovered && !(drag_.kind == DragKind::Connect && drag_.widget.a == w.a))
            break;
        const Color color = nodeColor(w.a);
        const Rect node = w.rect.inset(kEdgeOut);
        if (isBoxNode(w.a)) {
            const float radius = isShaperNode(w.a) ? kRadiusMark : kRadiusControl;
            c.strokeRect(node.inset(-2.0f), color.alpha(0.35f), 5.0f, radius + 2.0f);
            c.strokeRect(node, color, 2.5f, radius);
        } else {
            c.strokeCircle(node.center(), kNodeRadius + 2.0f, color.alpha(0.35f), 5.0f);
            c.strokeCircle(node.center(), kNodeRadius, color, 2.5f);
        }
        break;
    }
    case Kind::WireEnd: // drawn with its wire, which lights up under the mouse, a circle around its arrowhead
        if (hovered) {
            if (w.b == kNodes) {
                wireEndHighlight(c, outWireOf(w.a, nodeCenter(w.a)).end, {0.0f, 1.0f}, nodeColor(w.a));
            } else {
                const Wire wire = wireBetween(w.a, nodeCenter(w.a), w.b, nodeCenter(w.b), wireBend(w.param));
                wireEndHighlight(c, wire.end, wire.tangent, nodeColor(w.a));
            }
        }
        break;
    case Kind::WireLabel:
    case Kind::OutLabel: {
        const double amount = value(w.param);
        const bool focused = hovered || active;
        const bool boxes = isBoxNode(w.a) || isBoxNode(w.b);
        const Color tint = w.kind == Kind::OutLabel ? kText.alpha(0.3f) : routeColor(w.a, w.b).alpha(boxes ? 0.6f : 0.5f);
        // As a pill, it draws all of it, its parts' lines and its own lit part; its parts draw what is on them.
        const bool pill = w.group.w > 0.0f;
        const Rect body = pill ? w.group : w.rect;
        c.fillRect(body, kRaised, body.h * 0.5f);
        if (pill && focused)
            c.fillRect(w.rect, kText.alpha(0.06f));
        c.strokeRect(body, focused || pill ? kText : tint, 1.0f, body.h * 0.5f);
        if (pill) {
            if (w.group.x < w.rect.x)
                c.line({w.rect.x, w.rect.y + 3.0f}, {w.rect.x, w.rect.bottom() - 3.0f}, kTrack, 1.0f);
            c.line({w.rect.right(), w.rect.y + 3.0f}, {w.rect.right(), w.rect.bottom() - 3.0f}, kTrack, 1.0f);
        }
        std::string text = percent(amount);
        if (w.kind == Kind::WireLabel && w.a == w.b)
            text = trf(Str::CaptionFeedback, {text});
        // On the INPUTS tab a modulated label's text sits a little lower, between its ranges along its top and its
        // handles along its bottom; in the graph they are over and under it, and its text stays in the middle.
        bool modulated = false;
        if (!w.graph)
            forEachMod(w.param, [&](const Assignment &) { modulated = true; });
        const float lift = modulated ? 0.0f : -1.0f; // unmodulated, its digits centered in it
        Rect textRect{w.rect.x, w.rect.y + lift, w.rect.w, w.rect.h};
        if (w.modeGlyph) { // its mode, other than Phase, at its left
            const auto mode = routeMode(value(routeModeParam(w.a, w.b)));
            routeModeIcon(c, {w.rect.x + 13.0f, w.rect.center().y}, mode, routeColor(w.a, w.b), 0.85f);
            textRect = {w.rect.x + 18.0f, w.rect.y + lift, w.rect.w - 18.0f, w.rect.h}; // where it sits when open
        }
        c.text(textRect, text, kTextLabel, focused ? kText : kDim);
        // In the graph, on its outline's top line (the line's middle half a pixel in); the handles go below.
        paintModRange(c, {w.rect.x + 7.0f, w.graph ? w.rect.y - 0.25f : w.rect.y + 2.5f, w.rect.w - 14.0f, 1.5f},
                      w.param);
        break;
    }
    case Kind::LabelMode: {
        // Its mode's glyph and a chevron, pointing up while its list is open; lit under the mouse and while open.
        const bool open = listMenu_ == w.param;
        const Color tint = routeColor(w.a, w.b);
        if (hovered || open) {
            c.pushClip(w.rect);
            c.fillRect(w.group.inset(1.0f), tint.alpha(open ? 0.3f : 0.2f), kRadiusCard);
            c.popClip();
        }
        const Point m{w.rect.x + 11.0f, w.rect.center().y};
        routeModeIcon(c, m, routeMode(value(w.param)), hovered || open ? kText : tint, 0.85f);
        const Point v{w.rect.right() - 7.0f, w.rect.center().y};
        const float dir = open ? -1.0f : 1.0f;
        const Point chevron[] = {{v.x - 2.5f, v.y - 1.2f * dir}, {v.x, v.y + 1.5f * dir}, {v.x + 2.5f, v.y - 1.2f * dir}};
        c.polyline(chevron, 3, hovered || open ? kText : kDim, 1.2f);
        break;
    }
    case Kind::LabelRemove: {
        if (hovered) {
            c.pushClip(w.rect);
            c.fillRect(w.group.inset(1.0f), kText.alpha(0.1f), kRadiusCard);
            c.popClip();
        }
        const Point m = w.rect.center();
        const Color ink = hovered ? kText : kDim;
        c.line({m.x - 3.0f, m.y - 3.0f}, {m.x + 3.0f, m.y + 3.0f}, ink, 1.3f);
        c.line({m.x - 3.0f, m.y + 3.0f}, {m.x + 3.0f, m.y - 3.0f}, ink, 1.3f);
        break;
    }
    case Kind::GridCell: {
        const double amount = value(w.param);
        const bool out = w.b == kGridCols - 1;
        const float a = static_cast<float>(liveAmount(w.param)); // the fill follows modulation (the macros' with no note)
        const bool boxes = isBoxNode(w.a) || (!out && isBoxNode(w.b));
        const Color fill = out ? kText.alpha(0.1f + 0.3f * a)
                               : routeColor(w.a, out ? w.a : w.b).alpha(0.12f + (boxes ? 0.5f : 0.55f) * a);
        // The row and column of a node that is off are greyed out; their routes keep their amounts.
        const bool off = !nodeOn(w.a) || (!out && !nodeOn(w.b));
        const float dim = off ? 0.35f : 1.0f;
        c.fillRect(w.rect, (amount > 0.0 ? fill : kRaised).alpha(dim), kRadiusControl);
        if (w.a == w.b)
            c.strokeRect(w.rect, nodeColor(w.a).alpha(0.35f * dim), 1.0f, kRadiusControl); // feedback diagonal
        if (hovered || active)
            c.strokeRect(w.rect, kText, 1.5f, kRadiusControl);
        char text[8];
        std::snprintf(text, sizeof(text), "%.0f", amount * 100.0);
        const bool shown = amount > 0.0 || wired(w.param); // a route kept at 0 % shows its 0, dimmed
        c.text(w.rect, shown ? text : "\xC2\xB7", kTextValue,
               off ? kFaint : (amount > 0.0 ? kText : (shown ? kDim : kFaint)), Align::Center, amount > 0.0 && !off);
        // A route that isn't Phase shows its mode's glyph in its top left corner.
        if (const int mode = out ? -1 : routeModeParam(w.a, w.b); mode >= 0 && shown) {
            const auto m = routeMode(value(mode));
            if (m != RouteMode::Phase)
                routeModeIcon(c, {w.rect.x + 6.5f, w.rect.y + 6.5f}, m, (amount > 0.0 ? kText : kDim).alpha(off ? 0.35f : 0.85f),
                              0.6f);
        }
        paintModRange(c, {w.rect.x + 4.0f, w.rect.bottom() - 5.0f, w.rect.w - 8.0f, 1.5f}, w.param);
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
    case Kind::SourcesToggle:
    case Kind::KeysToggle: {
        // A chevron pointing down while open, right while collapsed, and the panel's title beside it.
        const bool sources = w.kind == Kind::SourcesToggle;
        const bool collapsed = sources ? controller_.layout().sourcesCollapsed : controller_.layout().keysCollapsed;
        const Point m{w.rect.x + 9.0f, w.rect.center().y};
        const Point open[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        const Point shut[] = {{m.x - 2.0f, m.y - 4.0f}, {m.x + 2.5f, m.y}, {m.x - 2.0f, m.y + 4.0f}};
        c.polyline(collapsed ? shut : open, 3, hovered ? kText : kDim, 1.5f);
        c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h},
               tr(sources ? Str::HeadingModulation : Str::HeadingKeyboard), kTextBody, hovered ? kText : kDim, Align::Left,
               true);
        break;
    }
    case Kind::SourceHandle:
    case Kind::MacroHandle: {
        // A switched-off source's is faint, like its rings on knobs, and so is any source's while it modulates
        // nothing; either can still be dragged onto a target.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a].alpha(sourceLive(w.a) ? 1.0f : 0.3f);
        if (hovered || active)
            c.fillCircle(center, 13.0f, color.alpha(0.25f));
        c.fillCircle(center, 8.5f, color);
        c.fillCircle(center, 3.5f, kPanel);
        break;
    }
    case Kind::SlotBar: {
        const Color color = kSourceColors[w.a].alpha(slotBypassed(w.a, w.b) ? 0.4f : 1.0f);
        const double amount = value(w.param);
        const float cy = w.rect.center().y;
        const float mid = w.rect.center().x;
        const float x = mid + static_cast<float>(amount) * w.rect.w * 0.5f;
        c.fillRect({w.rect.x, cy - 2.0f, w.rect.w, 4.0f}, kTrack, kRadiusMark);
        c.fillRect({std::min(mid, x), cy - 2.0f, std::abs(x - mid), 4.0f}, color, kRadiusMark);
        c.line({mid, cy - 4.0f}, {mid, cy + 4.0f}, kFaint, 1.0f);
        c.fillCircle({x, cy}, hovered || active ? 5.0f : 4.0f, color);
        // Where the modulation takes its target now, on the bar's scale: a tick that swings with its source.
        if (const std::optional<double> now = liveSlotOffset(w.a, w.b)) {
            const float at = mid + static_cast<float>(std::clamp(*now, -1.0, 1.0)) * w.rect.w * 0.5f;
            c.line({at, cy - 4.5f}, {at, cy + 4.5f}, kBackground, 3.0f);
            c.line({at, cy - 4.0f}, {at, cy + 4.0f}, kText, 1.5f);
        }
        // Another source modulating this depth shows how far it takes it, under the bar.
        paintModRange(c, {w.rect.x, cy + 6.0f, w.rect.w, 1.5f}, w.param);
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
        // How many modulations the source has, and MODS with a chevron at the right; with none, an invitation to add
        // one there.
        const size_t count = usedSlots(w.a).size();
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, hovered ? kSourceColors[w.a].alpha(0.6f) : kBorder, 1.0f, kRadiusControl);
        const Point m{w.rect.right() - 12.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 2.0f, m.y - 4.0f}, {m.x + 2.0f, m.y}, {m.x - 2.0f, m.y + 4.0f}};
        c.polyline(chevron, 3, hovered ? kText : kDim, 1.4f);
        if (count == 0) {
            c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 30.0f, w.rect.h}, tr(Str::CardNoModulations), kTextBody,
                   hovered ? kText : kDim, Align::Left);
            break;
        }
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 80.0f, w.rect.h},
               count == 1 ? tr(Str::ModsCountOne) : trf(Str::ModsCount, {std::to_string(count)}), kTextBody, kText,
               Align::Left);
        c.text({w.rect.right() - 80.0f, w.rect.y, 58.0f, w.rect.h}, tr(Str::ViewMods), kTextLabel, hovered ? kText : kDim,
               Align::Right, true);
        break;
    }
    case Kind::ValueText:
        break; // drawn by its slot's bar
    case Kind::WaveMore: {
        // Three dots, or the glyph of the wave picked from the list, with a chevron.
        const auto wave = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
        const bool selected = isMoreWave(wave);
        const bool lit = hovered || waveMenu_ == w.a;
        paintButtonFace(c, w, selected ? kAccent.alpha(0.18f) : (lit ? kTrack : kRaised), selected, kAccent, kRadiusControl);
        if (selected) {
            const Rect icon{w.rect.x + 7.0f, w.rect.y + 7.0f, w.rect.w - 22.0f, w.rect.h - 14.0f};
            const std::vector<Point> points = waveIcon(wave, icon);
            c.polyline(points.data(), points.size(), kAccent, 1.5f);
            const Point m{w.rect.right() - 8.0f, w.rect.center().y};
            const Point chevron[] = {{m.x - 3.0f, m.y - 1.5f}, {m.x, m.y + 1.5f}, {m.x + 3.0f, m.y - 1.5f}};
            c.polyline(chevron, 3, lit ? kText : kDim, 1.2f);
        } else {
            for (int d = -1; d <= 1; ++d)
                c.fillCircle({w.rect.center().x + d * 5.0f, w.rect.center().y}, 1.6f, lit ? kText : kDim);
        }
        break;
    }
    case Kind::WaveMenu:
        if (w.a == 1) {
            c.fillRect(w.rect, kRaised, kRadiusCard);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
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
    case Kind::ShapeReset:
    case Kind::PartialReset:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        resetIcon(c, w.rect.center(), hovered ? kText : kDim);
        break;
    case Kind::ShapeGrid: {
        const UiLayout &layout = controller_.layout();
        if (w.b == 0) { // the count
            c.text(w.rect, std::to_string(w.a == 0 ? layout.shapeGridX : layout.shapeGridY), kTextBody,
                   hovered ? kText : kDim, Align::Center, true);
            break;
        }
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.5f, m.y}, {m.x + 3.5f, m.y}, color, 1.5f);
        if (w.b > 0)
            c.line({m.x, m.y - 3.5f}, {m.x, m.y + 3.5f}, color, 1.5f);
        break;
    }
    case Kind::UndoButton: {
        const bool redo = w.a == 1;
        const bool can = (redo ? controller_.nextRedo() : controller_.nextUndo()) != nullptr;
        c.fillRect(w.rect, hovered && can ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        // An arrow curving over the top, its head at the left end (undo) or the right (redo), pointing down.
        const Point m{w.rect.center().x, w.rect.center().y + 2.0f};
        const Color color = !can ? kFaint : (hovered ? kText : kDim);
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
    case Kind::FilterPoint: {
        const Point center = w.rect.center();
        const bool focused = hovered || active;
        const Color color = w.b == 1 ? fxColor(w.a) : kFilterColor;
        c.fillCircle(center, focused ? 7.0f : 5.5f, focused ? color : kPanel);
        c.strokeCircle(center, focused ? 7.0f : 5.5f, color, 2.0f);
        if (focused) {
            const std::vector<int> pair = filterPointParams(w.a, w.b);
            const int cutoff = pair[0];
            const int resonance = pair[1];
            char a[32], b[32], text[80];
            formatParam(cutoff, value(cutoff), a, sizeof(a));
            formatParam(resonance, value(resonance), b, sizeof(b));
            std::snprintf(text, sizeof(text), "%s", trf(Str::CaptionResonance, {a, b}).c_str());
            const Rect shown = w.b == 1 ? fxFilterArea(w.a) : kFilterDisplay;
            c.text({shown.x, shown.y + 2.0f, shown.w - 8.0f, 14.0f}, text, kTextLabel, kText, Align::Right);
        }
        break;
    }
    case Kind::FxGrip:
    case Kind::ShapeArea:
    case Kind::MatrixLabel:
    case Kind::NoteDisplay:
    case Kind::Wire:
        break; // drawn with their card, editor, matrix, the graph or the keyboard strip
    case Kind::FxPower: {
        // The effect's icon: in its color while the effect is on, grey while it is off, as its card is, whatever
        // the mouse does. An effect that is on lights it up while the card is grabbed, by its title or the icon,
        // and under the mouse it sits on a button, as a click switches it.
        const FxType type = fxSlotType(w.a);
        const bool grabbed = (drag_.kind == DragKind::FxMove && drag_.widget.a == w.a) ||
                             (hover_ && hover_->kind == Kind::FxGrip && hover_->a == w.a && drag_.kind == DragKind::None);
        if (hovered)
            c.fillRect(w.rect, kTrack, kRadiusControl);
        const bool on = value(w.param) >= 0.5;
        fxIcon(c, w.rect.inset(4.0f), type, !on ? kDim : (hovered || grabbed ? kText : fxTypeColor(type)));
        break;
    }
    case Kind::FxFold:
    case Kind::SourceFold: {
        // Unfolded, a chevron in the title row that folds the card; folded, the strip below the cross, lit
        // while hovered, which unfolds it.
        const bool folded = w.kind == Kind::FxFold ? fxFolded(w.a) : sourceFolded(w.a);
        if (folded) {
            if (hovered)
                c.fillRect(w.rect.inset(3.0f), kText.alpha(0.06f), kRadiusControl);
            break;
        }
        const Color color = hovered ? kText : kDim;
        const Point m = w.rect.center();
        const Point chevron[] = {{m.x + 2.5f, m.y - 5.0f}, {m.x - 2.5f, m.y}, {m.x + 2.5f, m.y + 5.0f}};
        c.polyline(chevron, 3, color, 1.5f);
        break;
    }
    case Kind::FxBoxFold: { // as an effect card's
        if (splitFolded(w.a)) {
            if (hovered)
                c.fillRect(w.rect.inset(3.0f), kText.alpha(0.06f), kRadiusControl);
            break;
        }
        const Color color = hovered ? kText : kDim;
        const Point m = w.rect.center();
        const Point chevron[] = {{m.x + 2.5f, m.y - 5.0f}, {m.x - 2.5f, m.y}, {m.x + 2.5f, m.y + 5.0f}};
        c.polyline(chevron, 3, color, 1.5f);
        break;
    }
    case Kind::FxBoxGrip: // the title row: drawn with the split's controls
        break;
    case Kind::SplitBand: {
        // The band's name on the tabs' line after a dot in its color, the one shown underlined in it, as an operator
        // page's tabs are in the accent. A band soloed has an S in its color in its dot's place; a hidden one, how many
        // effects it holds after its name.
        const int lane = laneOfTab(w.b);
        const bool shown = splitLane(w.a) == lane;
        const Color band = laneColor(lane);
        const char *name = tr(lane == 1 ? Str::TabBandHigh : lane == 3 ? Str::TabBandMid : Str::TabBandLow);
        const float width = c.textWidth(name, kTextLabel, true);
        size_t effects = 0;
        for (const FxLineItem &item : fxLine())
            if (item.node == fxSplitNode(w.a))
                effects = item.band(lane).size();
        const bool soloed = value(paramIndex(w.a, laneSolo(lane))) >= 0.5;
        // The dot (an S in a circle while soloed), the name and its count centered together, so three tabs keep to
        // their own.
        const bool counted = !shown && effects > 0;
        const float lead = soloed ? 4.0f : 0.0f; // the S's circle wider than the dot
        const float x = w.rect.center().x - 0.5f * (width + (counted ? 12.0f : 0.0f) - lead) + 4.0f,
                    y = w.rect.center().y;
        const float base = w.rect.bottom();
        // Lit in its band's color while an effect carried over it would land on that band.
        if (drag_.kind == DragKind::FxMove && drag_.moved && fxDrag_ && fxDrag_->item.node < kFxSlots &&
            w.rect.contains(drag_.now)) {
            c.fillRect(w.rect, band.alpha(0.18f), kRadiusControl);
            c.strokeRect(w.rect, band.alpha(0.8f), 1.2f, kRadiusControl);
        }
        c.line({w.rect.x, base - 0.5f}, {w.rect.right(), base - 0.5f}, mix(kCardColor, kDim, 0.25f), 1.0f);
        if (shown)
            c.line({w.rect.x + 8.0f, base - 1.0f}, {w.rect.right() - 8.0f, base - 1.0f}, band, 2.0f);
        if (soloed) {
            const Point badge{x - 11.0f, y};
            c.fillCircle(badge, 6.0f, band);
            c.text({badge.x - 6.0f, badge.y - 6.5f, 12.0f, 13.0f}, tr(Str::SoloMark), kTextSmall, kDisplayColor,
                   Align::Center, true);
        } else {
            c.fillCircle({x - 9.0f, y}, 3.0f, band.alpha(shown ? 1.0f : 0.45f));
        }
        c.text({x, w.rect.y, width + 2.0f, w.rect.h}, name, kTextLabel, shown || hovered ? kText : kDim, Align::Left, true);
        if (counted)
            c.text({x + width + 1.0f, w.rect.y, 16.0f, w.rect.h}, std::to_string(effects), kTextSmall, kDim.alpha(0.8f));
        break;
    }
    case Kind::SplitSolo: { // a plain S, filled in its band's color while soloed
        const bool on = value(w.param) >= 0.5;
        const Color band = laneColor(w.b);
        c.fillRect(w.rect, on ? band.alpha(0.22f) : (hovered ? kTrack : kCardColor), kRadiusControl);
        c.strokeRect(w.rect, on ? band : mix(kCardColor, kDim, 0.25f), 1.0f, kRadiusControl);
        c.text(w.rect, tr(Str::SoloMark), kTextLabel, on || hovered ? kText : kDim, Align::Center, true);
        break;
    }
    case Kind::FxBoxAdd: {
        // As the dashed card after the effects, narrower: a plus, then which band it adds to.
        const Color color = kAccent;
        if (hovered)
            c.fillRect(w.rect, color.alpha(0.07f), kRadiusCard);
        c.strokeRect(w.rect, hovered ? color.alpha(0.8f) : kFaint.alpha(0.5f), 1.2f, kRadiusCard, true);
        const float arm = 9.0f;
        const Point m{w.rect.center().x, w.rect.center().y - 14.0f};
        const Color ink = hovered ? kText : kFaint.alpha(0.8f);
        c.line({m.x - arm, m.y}, {m.x + arm, m.y}, ink, 2.0f);
        c.line({m.x, m.y - arm}, {m.x, m.y + arm}, ink, 2.0f);
        // In capitals, as ADD EFFECT and ADD SOURCE are.
        c.text({w.rect.x, m.y + arm + 6.0f, w.rect.w, 14.0f}, upperCase(tr(Str::ButtonAddToBand)), kTextLabel, ink,
               Align::Center, true);
        c.text({w.rect.x, m.y + arm + 20.0f, w.rect.w, 14.0f},
               upperCase(tr(w.a == 1 ? Str::TabBandHigh : w.a == 3 ? Str::TabBandMid : Str::TabBandLow)),
               kTextLabel, ink, Align::Center, true);
        break;
    }
    case Kind::MacroName: // the knob draws the name; hovered, a line under it says it can be changed
        if (hovered && !typing_)
            c.line({w.rect.x + 14.0f, w.rect.bottom() - 1.0f}, {w.rect.right() - 14.0f, w.rect.bottom() - 1.0f},
                   kFaint, 1.0f, true);
        break;
    case Kind::FxMenuButton: // its list, not the dashed card's
        paintMenuButton(c, w.rect, tr(Str::ButtonEffect), hovered, fxMenuOpen_ && fxMenuAt_.x < 0.0f,
                        fxChain().size() >= static_cast<size_t>(kFxSlots));
        break;
    case Kind::FxShadow:
    case Kind::SourceShadow: {
        // A dashed outline with a plus in it and what it adds under that, faint at rest so it stays behind the cards,
        // lit while hovered in the theme's accent, like + EFFECT and + SOURCE.
        const bool fx = w.kind == Kind::FxShadow;
        const Color color = kAccent;
        if (hovered)
            c.fillRect(w.rect, color.alpha(0.07f), kRadiusCard);
        c.strokeRect(w.rect, hovered ? color.alpha(0.8f) : kFaint.alpha(0.5f), 1.2f, kRadiusCard, true);
        const float arm = 12.0f;
        const Point m{w.rect.center().x, w.rect.center().y - arm};
        const Color ink = hovered ? kText : kFaint.alpha(0.8f);
        c.line({m.x - arm, m.y}, {m.x + arm, m.y}, ink, 2.0f);
        c.line({m.x, m.y - arm}, {m.x, m.y + arm}, ink, 2.0f);
        const char *what = tr(fx ? Str::ButtonAddEffect : Str::ButtonAddSource);
        c.text({w.rect.x, m.y + arm + 6.0f, w.rect.w, 16.0f}, what, kTextLabel, ink, Align::Center, true);
        break;
    }
    case Kind::FxInsert: // drawn with the arrows between the cards
        break;
    case Kind::FxMode: {
        const bool on = fxGraph() == (w.b == 1);
        paintButtonFace(c, w, on ? kAccent.alpha(0.22f) : (hovered ? kTrack : kRaised), on, kAccent, kRadiusControl);
        fxModeIcon(c, w.rect.center(), w.b == 1, on ? kAccent : (hovered ? kText : kDim));
        break;
    }
    case Kind::WireStart: // a circle with a dot where it leaves its node
        if (hovered) {
            const Point start = w.b == kNodes
                                    ? outWireOf(w.a, nodeCenter(w.a)).start
                                    : wireBetween(w.a, nodeCenter(w.a), w.b, nodeCenter(w.b), wireBend(w.param)).start;
            wireStartHighlight(c, start, nodeColor(w.a));
        }
        break;
    case Kind::FxClear: { // a bin, faint while there is nothing to remove
        const bool any = fxAnyToClear();
        paintButtonFace(c, w, hovered && any ? kTrack : kRaised, false, kAccent, kRadiusControl);
        binIcon(c, w.rect.center(), !any ? kFaint : hovered ? kText : kDim);
        break;
    }
    case Kind::FxNode: // drawn with the effects graph (see paintFxGraph())
    case Kind::FxWire:
    case Kind::FxSlot:
    case Kind::FxGap:
    case Kind::FxNodeDelete:
    case Kind::FxNodePower:
    case Kind::SplitPoint:
        break;
    case Kind::FxMenu:
        if (w.a == 1) { // the list's panel, with each group's name on a strip over its effects (and from the main line
                        // the splits')
            static constexpr Str kGroups[] = {Str::MenuFxDrive, Str::MenuFxEqFilter,   Str::MenuFxDynamics,
                                              Str::MenuFxSpace, Str::MenuFxModulation, Str::MenuFxStereo,
                                              Str::MenuFxSplit};
            static_assert(std::size(kGroups) == kFxMenuSplitGroup + 1, "each group of the list of effects has its name");
            c.fillRect(w.rect, kRaised, kRadiusCard);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
            const int groups = fxSlot_.lane == 0 ? kFxMenuSplitGroup + 1 : kFxMenuGroupCount;
            for (int g = 0; g < groups; ++g) {
                const Rect head = fxMenuHead(fxMenuAt_, g);
                const Rect strip{head.x, head.y, head.w, head.h - 2.0f};
                c.fillRect(strip, kPanel, kRadiusControl);
                c.text({strip.x + 8.0f, strip.y, strip.w - 16.0f, strip.h}, tr(kGroups[g]), kTextSmall, kText,
                       Align::Left, true);
            }
        }
        break;
    case Kind::SourceMenu:
        if (w.a == 1) { // the list's panel
            c.fillRect(w.rect, kRaised, kRadiusCard);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
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
    case Kind::FxAdd:
        paintMenuRow(c, w.rect, hovered, false);
        fxIcon(c, {w.rect.x + 9.0f, w.rect.center().y - 7.0f, 18.0f, 14.0f}, static_cast<FxType>(w.a),
               hovered ? fxTypeColor(static_cast<FxType>(w.a)) : kDim);
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, fxName(static_cast<FxType>(w.a)), kTextBody,
               hovered ? kText : kDim, Align::Left);
        break;
    case Kind::FxAddNode: { // in the splits' group, under the left column: a split in two bands or three
        const Color color = splitColor();
        const bool three = w.b == 3;
        paintMenuRow(c, w.rect, hovered, false);
        const Rect icon{w.rect.x + 9.0f, w.rect.center().y - 7.0f, 18.0f, 14.0f};
        splitIcon(c, icon, hovered ? color : kDim, three ? 3 : 2);
        c.text({w.rect.x + 36.0f, w.rect.y, w.rect.w - 46.0f, w.rect.h}, tr(three ? Str::MenuSplitThree : Str::MenuSplitBands),
               kTextBody, hovered ? kText : kDim, Align::Left);
        break;
    }
    case Kind::FxRemove:
    case Kind::FxNodeRemove:
    case Kind::SourceRemove: {
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
        if (std::min(fxBand_[w.a], kPeqBands - 1) == w.b && on)
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
    case Kind::LfoDisplay:
        // A freeform LFO's display opens its editor.
        if (std::lround(value(paramIndex(w.a, SourceParam::Shape))) == static_cast<long>(LfoShape::Custom))
            editButton(c, lfoEditButton(w.rect), kSourceColors[w.a], hovered, false);
        break;
    case Kind::PartialEdit: // a Custom wave's, lit while its partial editor is open
        editButton(c, w.rect, kAccent, hovered, partialEditing() == w.a);
        break;
    case Kind::ClipDisplay: // the edit button on a drawn curve's display
        editButton(c, w.rect, clipColor(w.a), hovered, false);
        break;
    case Kind::ListButton: {
        // A slope shows as it plays (a band-pass's 6 dB as 12), and faintly for a comb, which has none.
        const int shown = listShown(w.param);
        const bool relevant =
            !isFilterSlope(w.param) || hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param)))));
        const bool lit = relevant && (hovered || listMenu_ == w.param);
        const Color color = listColor(w.param);
        c.fillRect(w.rect, lit ? color.alpha(0.18f) : kRaised, kRadiusControl);
        c.strokeRect(w.rect, color.alpha(lit ? 0.9f : relevant ? 0.45f : 0.2f), 1.0f, kRadiusControl);
        const Color text = !relevant ? kFaint : lit ? kText : kDim;
        // An LFO's shape shows its icon before its name.
        const float indent = isLfoShape(w.param) ? 24.0f : 0.0f;
        if (isLfoShape(w.param))
            lfoShapeIcon(c, {w.rect.x + 10.0f, w.rect.center().y - 5.0f, 16.0f, 10.0f}, static_cast<LfoShape>(shown),
                         color);
        c.text({w.rect.x + 10.0f + indent, w.rect.y, w.rect.w - 30.0f - indent, w.rect.h}, listItemName(w.param, shown),
               kTextBody, text, Align::Left, true);
        const Point m{w.rect.right() - 13.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        c.polyline(chevron, 3, text, 1.5f);
        break;
    }
    case Kind::ListMenu:
        if (w.a == 1) {
            c.fillRect(w.rect, kRaised, kRadiusCard);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
        }
        break;
    case Kind::PeqBand: {
        // The chip: on a plate ringed in the band's color while its knobs show, its number in that color while it
        // plays, and a line between the number and the shape.
        const bool picked = std::min(fxBand_[w.a], kPeqBands - 1) == w.b;
        const bool on = value(peqOnIndex(w.a, w.b)) >= 0.5;
        const bool chipHovered = hover_ && hover_->a == w.a && hover_->b == w.b &&
                                 (hover_->kind == Kind::PeqBand || hover_->kind == Kind::PeqBandOn ||
                                  hover_->kind == Kind::PeqTypeButton);
        const Color color = kSourceColors[w.b];
        c.fillRect(w.rect, picked ? kTrack : (chipHovered ? kRaised : kCardColor), kRadiusControl);
        c.strokeRect(w.rect, picked ? color.alpha(on ? 0.9f : 0.5f) : kBorder, 1.0f, kRadiusControl);
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
            c.fillRect(w.rect, kRaised, kRadiusCard);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
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
        const float indent = isLfoShape(w.param) || modeChoice ? 24.0f : 0.0f;
        if (isLfoShape(w.param))
            lfoShapeIcon(c, {w.rect.x + 10.0f, w.rect.center().y - 5.0f, 16.0f, 10.0f}, static_cast<LfoShape>(w.b),
                         selected ? listColor(w.param) : kDim);
        if (modeChoice)
            routeModeIcon(c, {w.rect.x + 16.0f, w.rect.center().y}, static_cast<RouteMode>(w.b),
                          selected ? listColor(w.param) : kDim);
        c.text({w.rect.x + 10.0f + indent, w.rect.y, w.rect.w - 20.0f - indent, w.rect.h}, listItemName(w.param, w.b),
               kTextBody, !fits ? kFaint : hovered || selected ? kText : kDim, Align::Left, selected);
        break;
    }
    case Kind::ShapePoint: { // compact: smaller, in a shaper's card
        const bool focused = hovered || active;
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 5.0f : 3.8f) : (focused ? 6.5f : 5.0f);
        c.fillCircle(w.rect.center(), radius, focused ? color : kPanel);
        c.strokeCircle(w.rect.center(), radius, color, w.compact ? 1.5f : 2.0f);
        break;
    }
    case Kind::ShapeCurve: {
        const bool focused = hovered || active;
        const Color color = shapeColor(w.a);
        const float radius = w.compact ? (focused ? 3.8f : 2.4f) : (focused ? 4.5f : 3.0f);
        c.fillCircle(w.rect.center(), radius, focused ? kText : color.alpha(0.6f));
        break;
    }
    case Kind::EnvCurve: {
        const bool focused = hovered || active;
        const Color color = w.a < kOps ? kAccent : kSourceColors[w.a - kOps];
        c.fillCircle(w.rect.center(), focused ? 4.5f : 3.0f, focused ? kText : color.alpha(0.6f));
        break; // its readout goes over the handles (paintEnvReadout())
    }
    case Kind::PartialArea:
        break; // drawn with its editor
    case Kind::ShapeDone:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        doneIcon(c, w.rect.center(), kText);
        break;
    case Kind::ShapeCollapse:
    case Kind::ShapeExpand:
    case Kind::ShapeWiden:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        if (w.kind == Kind::ShapeCollapse)
            collapseIcon(c, w.rect.center(), hovered ? kText : kDim);
        else
            expandIcon(c, w.rect.center(), hovered ? kText : kDim);
        break;
    case Kind::ShapeTool: { // Paste faint while nothing is copied
        const bool can = w.b != static_cast<int>(ShapeTool::Paste) || shapeCopied();
        c.fillRect(w.rect, hovered && can ? kTrack : kRaised, kRadiusControl);
        shapeToolIcon(c, w.rect.center(), w.b, !can ? kFaint : hovered ? kText : kDim);
        break;
    }
    case Kind::ShapeMode: { // Points | Steps, the one picked lit, and Snap lit while it is on
        const UiLayout &layout = controller_.layout();
        const bool on = w.b == 2 ? layout.shapeSnap : layout.shapeSteps == (w.b == 1);
        const Color color = shapeColor(w.a);
        paintButtonFace(c, w, on ? color.alpha(0.22f) : (hovered ? kTrack : kRaised), on, color, kRadiusControl);
        const Color ink = on ? kText : (hovered ? kText : kDim);
        const Str names[] = {Str::SwitchPoints, Str::SwitchSteps, Str::SwitchSnap};
        const char *name = tr(names[w.b]);
        const float textW = c.textWidth(name, kTextLabel, on);
        const float left = w.rect.center().x - (16.0f + textW) * 0.5f;
        const Point icon{left + 6.0f, w.rect.center().y};
        if (w.b == 0)
            pointsIcon(c, icon, on ? color : ink);
        else if (w.b == 1)
            stepsIcon(c, icon, on ? color : ink);
        else
            gridIcon(c, icon, on ? color : ink);
        c.text({left + 16.0f, w.rect.y, textW + 2.0f, w.rect.h}, name, kTextLabel, ink, Align::Left, on);
        break;
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
        const bool off = !presetActionOn(static_cast<PresetAction>(w.a));
        c.fillRect(w.rect, hovered && !off ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        const Point m = w.rect.center();
        const Color color = off ? kFaint : hovered ? kText : kDim;
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
    case Kind::Octave: {
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusMark);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusMark);
        // A minus or a plus, drawn: a glyph this small sits off center.
        const Point m = w.rect.center();
        c.line({m.x - 3.0f, m.y}, {m.x + 3.0f, m.y}, kText, 1.2f);
        if (w.a > 0)
            c.line({m.x, m.y - 3.0f}, {m.x, m.y + 3.0f}, kText, 1.2f);
        break;
    }
    case Kind::SettingsButton: {
        const bool open = settingsOpen_;
        c.fillRect(w.rect, hovered || open ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        // A cog: eight teeth around a ring.
        const Point center = w.rect.center();
        const Color color = hovered || open ? kText : kDim;
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
    case Kind::SettingsHandles: {
        // A row of buttons, the one picked lit. The bend ranges fade while MPE is off, though they still take clicks.
        const bool on = w.kind == Kind::SettingsOption    ? controller_.oversampling() == w.b
                        : w.kind == Kind::SettingsMpe     ? controller_.mpe() == (w.b != 0)
                        : w.kind == Kind::SettingsHandles ? std::clamp(controller_.handleSize(), 0,
                                                                       kModHandleSizeCount - 1) == w.b
                                                          : controller_.mpeBendRange() == w.b;
        const bool faint = w.kind == Kind::SettingsMpeRange && !controller_.mpe();
        const Color accent = faint ? kDim : kAccent;
        if (w.group.w > 0.0f) // in its row's pill
            paintButtonFace(c, w, on ? accent.alpha(0.22f) : (hovered ? kTrack : kRaised), on, accent, kRadiusControl);
        else {
            c.fillRect(w.rect, on ? accent.alpha(0.22f) : (hovered ? kTrack : kRaised), kRadiusControl);
            c.strokeRect(w.rect, on ? accent : kBorder, 1.0f, kRadiusControl);
        }
        char factor[16];
        std::snprintf(factor, sizeof(factor), "%dx", w.b);
        static constexpr Str kHandleSizes[] = {Str::SettingsHandlesSmall, Str::SettingsHandlesMedium,
                                               Str::SettingsHandlesLarge};
        static_assert(std::size(kHandleSizes) == kModHandleSizeCount);
        const std::string label = w.kind == Kind::SettingsOption    ? std::string(factor)
                                  : w.kind == Kind::SettingsMpe     ? std::string(tr(w.b ? Str::ValueOn : Str::ValueOff))
                                  : w.kind == Kind::SettingsHandles ? std::string(tr(kHandleSizes[w.b]))
                                                                    : trf(Str::SettingsMpeRange, {std::to_string(w.b)});
        c.text(w.rect, label, kTextBody, on && !faint ? kText : kDim, Align::Center, on);
        break;
    }
    case Kind::SettingsGlow:
    case Kind::SettingsLinks: {
        // A track filled up to the amount, and a knob to drag along it.
        const float amount = w.kind == Kind::SettingsLinks ? shownLinks() : shownGlow();
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
        const bool on = themeIndex(controller_.theme()) == w.b;
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
    case Kind::SearchField: // the browser's and the target menu's drawn with them
        if (w.a == static_cast<int>(Field::ModsFilter)) {
            if (modsFilterOpen()) {
                paintField(c, Field::ModsFilter, w.rect, fieldTyped(w.rect), tr(Str::ModsFilterHint),
                           modsFilter_.text.empty() ? std::string() : modsFilterCount());
            } else { // a magnifier, to open it
                if (hovered)
                    c.fillRect(w.rect, kTrack, kRadiusControl);
                const Point lens{w.rect.center().x - 1.5f, w.rect.center().y - 1.5f};
                c.strokeCircle(lens, 4.5f, hovered ? kText : kDim, 1.3f);
                c.line({lens.x + 3.3f, lens.y + 3.3f}, {lens.x + 7.0f, lens.y + 7.0f}, hovered ? kText : kDim, 1.5f);
            }
        }
        break;
    case Kind::PresetNameBox: {
        // The preset's name, a dot while it has changed since it was loaded, and a chevron for the browser.
        const bool lit = hovered || browserOpen_;
        c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        c.text({w.rect.x + 28.0f, w.rect.y, w.rect.w - 56.0f, w.rect.h}, controller_.presetName(), kTextBody, kText,
               Align::Center, true);
        if (controller_.patchEdited())
            c.fillCircle({w.rect.right() - 30.0f, w.rect.center().y}, 2.5f, kAccent);
        const Point m{w.rect.right() - 14.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 4.0f, m.y - 2.0f}, {m.x, m.y + 2.5f}, {m.x + 4.0f, m.y - 2.0f}};
        c.polyline(chevron, 3, lit ? kText : kDim, 1.5f);
        break;
    }
    case Kind::TempoBox: {
        // A metronome, then the tempo and its unit, centered in the room right of it.
        const bool lit = hovered || active;
        c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        const Color ink = lit ? kText : kDim;
        const Point m{w.rect.x + 14.0f, w.rect.center().y};
        const Point body[] = {{m.x - 5.0f, m.y + 6.0f}, {m.x - 2.0f, m.y - 6.0f}, {m.x + 2.0f, m.y - 6.0f},
                              {m.x + 5.0f, m.y + 6.0f}, {m.x - 5.0f, m.y + 6.0f}};
        c.polyline(body, std::size(body), ink, 1.2f);
        c.line({m.x, m.y + 3.0f}, {m.x + 5.0f, m.y - 6.0f}, ink, 1.4f);
        if (typing_ && typing_->widget.kind == Kind::TempoBox)
            break; // the field being typed in covers the number
        const std::string number = tempoText(controller_.tempo());
        const char *unit = "BPM";
        const float numberW = c.textWidth(number, kTextValue, true), unitW = c.textWidth(unit, kTextLabel);
        const float room = w.rect.right() - (m.x + 7.0f);
        const float x = m.x + 7.0f + (room - numberW - 4.0f - unitW) * 0.5f;
        c.text({x, w.rect.y, numberW + 1.0f, w.rect.h}, number, kTextValue, kText, Align::Left, true);
        c.text({x + numberW + 4.0f, w.rect.y + 1.0f, unitW + 1.0f, w.rect.h}, unit, kTextLabel, kFaint, Align::Left);
        break;
    }
    case Kind::PresetStep: {
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        const Point m = w.rect.center();
        const float d = w.a < 0 ? 1.5f : -1.5f; // the chevron points the way it steps
        const Point chevron[] = {{m.x + d, m.y - 4.0f}, {m.x - d, m.y}, {m.x + d, m.y + 4.0f}};
        c.polyline(chevron, 3, hovered ? kText : kDim, 1.5f);
        break;
    }
    case Kind::BrowserRow: {
        // The preset loaded in the accent; the one picked from the keyboard, like a hovered one, outlined.
        const bool current = controller_.presetIndex() == w.a;
        const bool picked = browserPick_ == w.a;
        if (current)
            c.fillRect(w.rect, kAccent.alpha(0.18f), kRadiusControl);
        else if (hovered || picked)
            c.fillRect(w.rect, kTrack, kRadiusControl);
        if (current || picked)
            c.strokeRect(w.rect, picked ? kText.alpha(0.6f) : kAccent.alpha(0.6f), 1.0f, kRadiusControl);
        const std::vector<PresetEntry> &presets = controller_.presets();
        if (w.a < static_cast<int>(presets.size()))
            c.text({w.rect.x + 14.0f, w.rect.y, w.rect.w - 28.0f, w.rect.h}, presets[w.a].name, kTextBody,
                   current || hovered || picked ? kText : kDim, Align::Left, current);
        break;
    }
    case Kind::SearchClear: {
        if (w.a == static_cast<int>(Field::CascadeSearch))
            break; // drawn with its menu
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.5f, m.y - 3.5f}, {m.x + 3.5f, m.y + 3.5f}, color, 1.3f);
        c.line({m.x - 3.5f, m.y + 3.5f}, {m.x + 3.5f, m.y - 3.5f}, color, 1.3f);
        break;
    }
    case Kind::BrowserClose: {
        c.fillRect(w.rect, hovered ? kTrack : kPanel, kRadiusControl);
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 5.0f, m.y - 5.0f}, {m.x + 5.0f, m.y + 5.0f}, color, 1.5f);
        c.line({m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y - 5.0f}, color, 1.5f);
        break;
    }
    case Kind::BrowserFolder:
        c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
        c.text(w.rect, tr(Str::ButtonOpenFolder), kTextLabel, hovered ? kText : kDim, Align::Center, true);
        break;
    case Kind::ConfirmButton: {
        if (w.a == 4) { // a split removed with its bands' effects
            c.fillRect(w.rect, hovered ? kTrack : kRaised, kRadiusControl);
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
            c.text(w.rect, tr(Str::ButtonSplitDelete), kTextBody, kText, Align::Center);
            break;
        }
        if (w.a >= 2) { // a split's middle band's effects onto its low band (2) or its high one (3), in that color
            const Color band = laneColor(w.a == 2 ? 2 : 1);
            c.fillRect(w.rect, band.alpha(hovered ? 0.3f : 0.16f), kRadiusControl);
            c.strokeRect(w.rect, band, 1.0f, kRadiusControl);
            c.text(w.rect, tr(w.a == 2 ? Str::ButtonMidToLow : Str::ButtonMidToHigh), kTextBody, kText, Align::Center,
                   true);
            break;
        }
        const bool bands = confirm_ == kConfirmMidBand; // removing them is no more the way ahead than the others
        const bool ok = w.a == 1;
        const bool lit = ok && !bands;
        c.fillRect(w.rect, lit ? kAccent.alpha(hovered ? 0.35f : 0.22f) : (hovered ? kTrack : kRaised), kRadiusControl);
        c.strokeRect(w.rect, lit ? kAccent : kBorder, 1.0f, kRadiusControl);
        const char *label = tr(!ok                                                ? Str::ButtonCancel
                               : bands                                            ? Str::ButtonMidRemove
                               : confirm_ == kConfirmSplitRemove                  ? Str::ButtonSplitKeep
                               : confirm_ == kConfirmFxClear                      ? Str::ButtonClearFx
                               : confirm_ == static_cast<int>(PresetAction::Init) ? Str::ButtonInit
                               : confirm_ == static_cast<int>(PresetAction::Save) ? Str::ButtonOverwrite
                                                                                  : Str::ButtonLoad);
        c.text(w.rect, label, kTextBody, kText, Align::Center, lit);
        break;
    }
    case Kind::SplitBands: { // the BANDS switch: 2 | 3, the one it is lit in the accent
        const bool on = (w.b == 3) == splitThree(w.a);
        c.fillRect(w.rect, on ? kAccent.alpha(0.22f) : (hovered ? kTrack : kRaised), kRadiusControl);
        c.strokeRect(w.rect, on ? kAccent.alpha(0.8f) : kBorder, 1.0f, kRadiusControl);
        c.text(w.rect, std::to_string(w.b), kTextLabel, on || hovered ? kText : kDim, Align::Center, true);
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
    case Kind::ModDot: {
        // Compact: on a wire label, matrix cell or depth bar.
        // A bypassed modulation's dot is hollow.
        const Point center = w.rect.center();
        const ModHandleSize &sizes = modHandleSize(controller_.handleSize());
        const float radius = hovered || active ? (w.compact ? sizes.dotCompactHover : sizes.dotHover)
                                               : (w.compact ? sizes.dotCompact : sizes.dot);
        const Color color = kSourceColors[w.a].alpha(sourceOn(w.a) ? 1.0f : 0.35f);
        if (slotBypassed(w.a, w.b))
            c.strokeCircle(center, radius - 0.6f, color, 1.2f);
        else
            c.fillCircle(center, radius, color);
        // With an aux, a ring round it in the aux's color.
        if (const int aux = slotAux([this](int index) { return value(index); }, w.a, w.b); aux >= 0)
            c.strokeCircle(center, radius + 1.8f, kSourceColors[aux], 1.3f);
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
            const Point m{x + 4.0f, w.rect.center().y};
            const Point chevron[] = {{m.x - 3.5f, m.y - 1.5f}, {m.x, m.y + 2.0f}, {m.x + 3.5f, m.y - 1.5f}};
            c.polyline(chevron, 3, kDim, 1.3f);
            break;
        }
        const Color color = kSourceColors[w.a];
        const bool bypassed = w.b >= 0 && slotBypassed(w.a, w.b);
        if (w.compact) {
            const Point handle{w.rect.x + 7.0f, w.rect.center().y};
            c.fillCircle(handle, 7.0f, color);
            c.fillCircle(handle, 2.5f, kPanel);
            const std::string name = sourceName(w.a);
            c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 34.0f, w.rect.h}, name, kTextBody, bypassed ? kDim : kText,
                   Align::Left, true);
            if (hovered || open) {
                const float x = w.rect.x + 24.0f + std::min(c.textWidth(name, kTextBody, true), w.rect.w - 40.0f);
                const Point m{x + 4.0f, w.rect.center().y};
                const Point chevron[] = {{m.x - 3.5f, m.y - 1.5f}, {m.x, m.y + 2.0f}, {m.x + 3.5f, m.y - 1.5f}};
                c.polyline(chevron, 3, kDim, 1.3f);
            }
        } else {
            c.line({w.rect.x + 7.0f, w.rect.y - 2.0f}, {w.rect.x + 7.0f, w.rect.bottom() + 2.0f}, color.alpha(0.35f),
                   2.0f);
            if (hovered || open)
                c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, sourceName(w.a), kTextBody, kDim,
                       Align::Left);
        }
        break;
    }
    case Kind::ModsTarget: {
        // An empty modulation without a target asks for one.
        const int target = modsRowTarget(w.a, w.b);
        const bool open = cascade_ && cascade_->kind == CascadeKind::Target && cascade_->source == w.a &&
                          cascade_->slot == w.b;
        if (hovered || open)
            c.fillRect(w.rect.inset(1.0f), kTrack, kRadiusMark);
        const std::string name = target >= 0 ? paramName(target) : std::string(tr(Str::ModsPickTarget));
        c.text({w.rect.x + 6.0f, w.rect.y, w.rect.w - 24.0f, w.rect.h}, name, kTextBody,
               target < 0 || (w.b >= 0 && slotBypassed(w.a, w.b)) ? kDim : kText, Align::Left);
        if (hovered || open || target < 0) {
            const Point m{w.rect.right() - 10.0f, w.rect.center().y};
            const Point chevron[] = {{m.x - 3.5f, m.y - 1.5f}, {m.x, m.y + 2.0f}, {m.x + 3.5f, m.y - 1.5f}};
            c.polyline(chevron, 3, kDim, 1.3f);
        }
        break;
    }
    case Kind::ModsScale: {
        // All of the depth at the right end, none at the left; its share in numbers after it.
        const double amount = value(w.param);
        const float cy = w.rect.center().y;
        const Color color = slotBypassed(w.a, w.b) ? kFaint : kDim;
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
        const bool bipolar = sourceBipolar(w.a);
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
        const Color color = slotBypassed(w.a, w.b) ? kFaint
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
            c.strokeRect(w.rect, hovered || open ? kDim : kTrack, 1.0f, radius, true);
            c.text(w.rect, tr(Str::ModsAddAux), kTextLabel, hovered || open ? kText : kDim);
            break;
        }
        const Rect chip{w.rect.x, w.rect.y, w.rect.w + 22.0f, w.rect.h};
        c.fillRect(chip, hovered || open ? kTrack : kRaised, radius);
        c.strokeRect(chip, kSourceColors[aux].alpha(0.5f), 1.0f, radius);
        c.fillCircle({w.rect.x + 11.0f, w.rect.center().y}, 4.0f, kSourceColors[aux]);
        c.text({w.rect.x + 20.0f, w.rect.y, w.rect.w - 22.0f, w.rect.h}, sourceName(aux), kTextLabel, kText, Align::Left,
               true);
        break;
    }
    case Kind::ModsAdd: {
        // A dashed button, as + Aux: lit under the mouse.
        const float radius = w.rect.h * 0.5f;
        c.strokeRect(w.rect, hovered ? kDim : kTrack, 1.0f, radius, true);
        c.text(w.rect, tr(Str::ModsAddModulation), kTextLabel, hovered ? kText : kDim);
        break;
    }
    case Kind::ModsAuxClear: {
        const Point m = w.rect.center();
        const Color color = hovered ? kText : kDim;
        c.line({m.x - 3.0f, m.y - 3.0f}, {m.x + 3.0f, m.y + 3.0f}, color, 1.3f);
        c.line({m.x - 3.0f, m.y + 3.0f}, {m.x + 3.0f, m.y - 3.0f}, color, 1.3f);
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
    case Kind::SlotBypass: {
        // The row's dot in the source's color: filled while it modulates, hollow while bypassed.
        const Point center = w.rect.center();
        const Color color = kSourceColors[w.a];
        if (hovered)
            c.fillCircle(center, 6.5f, color.alpha(0.2f));
        if (slotBypassed(w.a, w.b))
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
        polarityIcon(c, w.rect.center(), sourceBipolar(w.a), hovered ? kText : kDim);
        break;
    }
    }
}

// The name tags of a hovered wave button and of a hovered or dragged mod dot, painted after
// every widget so the ones below don't cover them.
void View::paintHoverTags(Canvas &c, const std::vector<Widget> &list) {
    for (const Widget &w : list) {
        if ((((w.kind == Kind::WaveButton || w.kind == Kind::WaveMore) && waveMenu_ < 0) ||
             w.kind == Kind::TypeButton) &&
            isHovered(w)) {
            const auto picked = static_cast<OpWave>(std::lround(value(paramIndex(w.a, OpParam::Wave))));
            const char *name = w.kind == Kind::TypeButton   ? filterTypeName(static_cast<FilterType>(w.b))
                               : w.kind == Kind::WaveButton ? waveName(static_cast<OpWave>(w.b))
                               : isMoreWave(picked)         ? waveName(picked)
                                                            : tr(Str::TipMoreWaves);
            const Rect tip{w.rect.center().x - 40.0f, w.rect.bottom() + 3.0f, 80.0f, 16.0f};
            c.fillRect(tip, kRaised, kRadiusControl);
            c.text(tip, name, kTextLabel, kText);
        } else if (w.kind == Kind::ModDot && (isHovered(w) || isActive(w))) {
            const Point center = w.rect.center();
            const std::string text = modRangeText(w.a, w.b);
            const float width = std::ceil(c.textWidth(text, kTextLabel)) + 16.0f;
            const float x = std::clamp(center.x - width * 0.5f, 4.0f, View::kWidth - 4.0f - width);
            const Rect tip{x, center.y + 8.0f, width, 18.0f};
            c.fillRect(tip, kRaised, kRadiusControl);
            c.text(tip, text, kTextLabel, kText);
        }
    }
}

// A modulation in words: its source, and what it takes its target through in the target's own units, from
// half the depth below the set value to half above for a bipolar modulation, or from it one way for a unipolar one.
std::string View::modRangeText(int source, int slot) const {
    std::string text = sourceName(source);
    const int target = slotTarget(value(slotTargetIndex(source, slot)));
    if (target >= 0) {
        const ParamDef &def = table_[target];
        const double set = value(target);
        const double reach =
            value(slotAmountIndex(source, slot)) * value(slotScaleIndex(source, slot)) * (def.max - def.min);
        const bool both = sourceBipolar(source);
        const double start = both ? set - 0.5 * reach : set; // a negative depth runs the other way
        const double end = both ? set + 0.5 * reach : set + reach;
        char from[32], to[32];
        formatParam(target, std::clamp(start, def.min, def.max), from, sizeof(from));
        formatParam(target, std::clamp(end, def.min, def.max), to, sizeof(to));
        text += std::string("  ") + from + " \xE2\x86\x92 " + to;
    }
    if (slotBypassed(source, slot))
        text += std::string("  ") + tr(Str::CaptionBypassed);
    return text;
}

void View::paintKnob(Canvas &c, const Widget &w) {
    const ParamDef &def = table_[w.param];
    const bool focused = isHovered(w) || isActive(w);
    const Point center = knobCenter(w);
    const float radius = knobRadius(w);
    const double n = norm(w.param);
    const float angle = knobAngle(n);

    c.arc(center, radius - 2.0f, kKnobStart, kKnobStart + kKnobSweep, focused ? kFaint : kTrack, 3.5f);
    // A macro's knob in its handle's color, faint like the handle, and its name too, while it modulates nothing.
    const int macro = macroOf(w.param);
    // An LFO's Phase is faint too while notes don't start the LFO there.
    const bool idle = (macro >= 0 && !sourceLive(kMaxSources + macro)) ||
                      (isLfoPhase(w.param) && !lfoPhaseUsed(sourceOf(w.param)));
    // A join's band levels in their bands' colors (the high band's wire takes its first input).
    const int sum = fxSumOfLevel(w.param);
    const int levelOf = sum < 0 ? -1
                        : w.param == fxInputIndex(fxSumNode(sum), 0, FxInputField::Level) ? 1
                        : w.param == fxInputIndex(fxSumNode(sum), 1, FxInputField::Level) ? 2
                                                                                          : 3;
    const Color plain = sum >= 0 ? laneColor(levelOf) : kAccent;
    const Color arc = (macro >= 0 ? kSourceColors[kMaxSources + macro] : plain).alpha(idle ? 0.3f : 1.0f);
    // A bipolar macro moves nothing at the knob's middle, so its arc starts there.
    const bool fromMiddle = isBipolar(def.kind) || (macro >= 0 && sourceBipolar(kMaxSources + macro));
    c.arc(center, radius - 2.0f, fromMiddle ? 0.0f : kKnobStart, angle, arc, 3.5f);

    // Modulation ranges just outside the track, one ring per source. Bipolar sources swing both ways.
    // A switched-off source's ring is faint: it is kept, but does nothing. The one whose depth handle is under the
    // mouse is wider, over the others.
    // (A hovered pill focuses all of those it holds.)
    std::vector<Assignment> focus;
    forEachMod(w.param, [&](const Assignment &m) {
        if (modFocused(m)) {
            focus.push_back(m);
            return;
        }
        const auto [down, up] = modReach(m);
        const float alpha = (m.bipolar ? 0.6f : 0.9f) * (sourceOn(m.source) && !m.bypassed ? 1.0f : 0.3f);
        c.arc(center, radius + 2.5f, knobAngle(n + down), knobAngle(n + up), kSourceColors[m.source].alpha(alpha),
              2.5f);
    });
    for (const Assignment &f : focus) {
        const auto [down, up] = modReach(f);
        const float alpha = sourceOn(f.source) && !f.bypassed ? 1.0f : 0.4f;
        c.arc(center, radius + 3.0f, knobAngle(n + down), knobAngle(n + up), kSourceColors[f.source].alpha(alpha),
              4.0f);
    }

    c.fillCircle(center, radius - 7.0f, kKnobBody);
    c.strokeCircle(center, radius - 7.0f, kBorder, 1.0f);
    c.line(Canvas::onCircle(center, (radius - 7.0f) * 0.3f, angle), Canvas::onCircle(center, radius - 9.5f, angle),
           kText, 2.0f);

    // Where modulation currently takes the newest voice, or with no note playing where the macros take it.
    // One modulated in steps stands on the step it plays.
    float offset = liveOffset(w.param);
    if (offset != 0.0f && modulatedInSteps(w.param))
        offset = static_cast<float>(std::clamp(std::round(value(w.param) + offset), def.min, def.max) - value(w.param));
    if (offset != 0.0f)
        c.fillCircle(Canvas::onCircle(center, radius - 2.0f, knobAngle(n + offset / (def.max - def.min))),
                     2.5f, kText);

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
        const Rect label{w.rect.x - 14.0f, w.rect.y + w.rect.w + 2.0f, w.rect.w + 28.0f, 16.0f};
        c.text(label, focused || !w.label ? shown : std::string(w.label), kTextLabel,
               focused ? kText : (idle ? kFaint : kDim));
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
        const float alpha = (sourceOn(m.source) && !m.bypassed ? 1.0f : 0.35f) * (wide ? 1.0f : 0.8f);
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

    c.text({w.rect.x, kRatioBox.bottom() + 2.0f, w.rect.w, 16.0f}, w.label ? w.label : "", kTextLabel,
           focused ? kText : kDim);
}

// What the play switches do, below the one under the mouse.
void View::paintTooltip(Canvas &c) {
    if (!hover_ || drag_.kind != DragKind::None || (cascade_ && modsWide()) ||
        (hover_->kind != Kind::Choice && hover_->kind != Kind::Toggle && hover_->kind != Kind::NodeTab &&
         hover_->kind != Kind::FxRemove && hover_->kind != Kind::SourceRemove &&
         hover_->kind != Kind::PresetButton &&
         hover_->kind != Kind::PresetStep && hover_->kind != Kind::PresetNameBox &&
         hover_->kind != Kind::TempoBox && hover_->kind != Kind::BrowserFolder &&
         hover_->kind != Kind::SettingsButton && hover_->kind != Kind::UndoButton &&
         hover_->kind != Kind::ShapeReset && hover_->kind != Kind::PartialReset &&
         hover_->kind != Kind::PartialEdit && hover_->kind != Kind::ClipDisplay &&
         hover_->kind != Kind::ShapeDone && hover_->kind != Kind::ShapeCollapse && hover_->kind != Kind::ShapeExpand &&
         hover_->kind != Kind::ShapeWiden &&
         hover_->kind != Kind::ShapeTool && hover_->kind != Kind::ShapeMode &&
         !(hover_->kind == Kind::ShapeArea && hover_->a == wideLfo() && shapeEditing() != hover_->a) &&
         hover_->kind != Kind::SlotBypass && hover_->kind != Kind::MacroName &&
         hover_->kind != Kind::ModPill &&
         hover_->kind != Kind::SourcePolarity && hover_->kind != Kind::Wire && hover_->kind != Kind::ModsSource &&
         !(hover_->kind == Kind::LabelMode && listMenu_ != hover_->param) && hover_->kind != Kind::LabelRemove &&
         hover_->kind != Kind::ModsTarget && hover_->kind != Kind::ModsAux && hover_->kind != Kind::ModsAuxClear &&
         hover_->kind != Kind::ModsScale && hover_->kind != Kind::ModsCurve && hover_->kind != Kind::SearchField &&
         hover_->kind != Kind::MacroHandle && hover_->kind != Kind::SourceHandle &&
         hover_->kind != Kind::MatrixLabel && hover_->kind != Kind::FxPower && hover_->kind != Kind::FxNodePower &&
         hover_->kind != Kind::NoteDisplay && hover_->kind != Kind::WireEnd && hover_->kind != Kind::Port &&
         hover_->kind != Kind::FxInsert && hover_->kind != Kind::DynBand && hover_->kind != Kind::FxMode &&
         hover_->kind != Kind::FxNode && hover_->kind != Kind::FxSlot && hover_->kind != Kind::FxGap &&
         hover_->kind != Kind::FxNodeRemove && hover_->kind != Kind::FxNodeDelete && hover_->kind != Kind::FxClear &&
         hover_->kind != Kind::NodeDelete &&
         hover_->kind != Kind::WireStart &&
         hover_->kind != Kind::DynGrip && hover_->kind != Kind::DynZone && !(hover_->kind == Kind::Knob && dynKnobTip(hover_->param) != Str::Count) &&
         !(hover_->kind == Kind::ListButton && (isSlopeList(hover_->param) || isMidiInput(hover_->param))) &&
         !(hover_->kind == Kind::ShapeGrid && hover_->b != 0)))
        return;
    // A source's handle tells how to use it only until it modulates something.
    const bool handle = hover_->kind == Kind::MacroHandle || hover_->kind == Kind::SourceHandle;
    if (handle && sourceUsed(hover_->a))
        return;
    const int param = hover_->param;
    const FxParam field = fxFieldOf(param);
    std::string text;
    if (hover_->kind == Kind::SearchField) {
        if (hover_->a != static_cast<int>(Field::ModsFilter) || modsFilterOpen())
            return; // a field says what it is for in its hint
        text = tr(Str::TipModsFilter);
    } else if (hover_->kind == Kind::MacroName) {
        text = tr(Str::TipRenameMacro);
    } else if (hover_->kind == Kind::ModPill) {
        if (modListOn(*hover_))
            return; // its list shows them
        int count = 0;
        forEachMod(hover_->a, [&](const Assignment &) { ++count; });
        text = trf(Str::TipModPill, {std::to_string(count - hover_->b)});
    } else if (hover_->kind == Kind::NoteDisplay) {
        text = tr(isVelocity(hover_->a) ? Str::TipVelocityDisplay : Str::TipNoteDisplay);
    } else if (hover_->kind == Kind::FxMode) {
        text = tr(hover_->b == 1 ? Str::TipFxGraph : Str::TipFxChain);
    } else if (hover_->kind == Kind::FxNode) {
        text = tr(Str::TipFxNode);
    } else if (hover_->kind == Kind::FxSlot) {
        text = tr(hover_->a == 0 ? Str::TipFxSlot : Str::TipFxSlotBand);
    } else if (hover_->kind == Kind::FxClear) {
        text = tr(Str::TipFxClear);
    } else if (hover_->kind == Kind::FxGap) {
        text = tr(hover_->a == 0 ? Str::TipFxGap : Str::TipFxGapBand);
    } else if (hover_->kind == Kind::WireStart) {
        text = tr(Str::TipWireStart);
    } else if (hover_->kind == Kind::FxNodeRemove || hover_->kind == Kind::FxNodeDelete) {
        text = tr(hover_->a < kFxSlots           ? Str::TipFxRemove
                  : fxNodeSum(hover_->a) >= 0   ? Str::TipRemoveJoin
                                                : Str::TipRemoveSplit);
    } else if (hover_->kind == Kind::FxNodePower) { // only switches, unlike the card's icon, which also drags
        text = trf(value(param) >= 0.5 ? Str::TipSwitchNodeOff : Str::TipSwitchNodeOn, {fxName(fxSlotType(hover_->a))});
    } else if (hover_->kind == Kind::NodeDelete) {
        text = trf(Str::MenuSwitchOff, {nodeTitle(hover_->a)});
    } else if (hover_->kind == Kind::WireEnd) {
        text = tr(Str::TipWireEnd);
    } else if (hover_->kind == Kind::Port) {
        text = tr(Str::TipPort);
    } else if (hover_->kind == Kind::FxInsert) { // on the main line, a split too; in a container, on its band
        text = tr(hover_->a == 0 ? Str::TipFxInsert : Str::TipFxGapBand);
    } else if (hover_->kind == Kind::FxBoxAdd) {
        text = tr(Str::TipFxSlotBand);
    } else if (hover_->kind == Kind::FxBoxGrip) {
        text = tr(Str::TipSplitGrip);
    } else if (hover_->kind == Kind::SplitBand) {
        text = tr(Str::TipSplitBand);
    } else if (hover_->kind == Kind::SplitSolo) {
        text = tr(Str::TipSplitSolo);
    } else if (hover_->kind == Kind::SplitBands) {
        text = tr(hover_->b == 3 ? Str::TipSplitThree : Str::TipSplitTwo);
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
    } else if (hover_->kind == Kind::MacroHandle && isWheel(hover_->a)) {
        text = tr(Str::TipWheelHandle);
    } else if (hover_->kind == Kind::MacroHandle && isNote(hover_->a)) {
        text = tr(Str::TipNoteHandle);
    } else if (hover_->kind == Kind::MacroHandle && isVelocity(hover_->a)) {
        text = tr(Str::TipVelocityHandle);
    } else if (hover_->kind == Kind::MacroHandle) {
        text = trf(Str::TipMacroHandle, {macroName(controller_.layout(), hover_->a - kMaxSources)});
    } else if (hover_->kind == Kind::SourceHandle) {
        text = trf(Str::TipSourceHandle, {sourceName(hover_->a)});
    } else if (hover_->kind == Kind::NodeTab || hover_->kind == Kind::MatrixLabel) {
        const bool on = nodeOn(hover_->a);
        const std::string title = nodeTitle(hover_->a);
        text = controller_.layout().selectedNode != hover_->a
                   ? trf(on ? Str::TipShowNode : Str::TipSwitchOnAndShow, {title})
                   : trf(on ? Str::TipSwitchNodeOff : Str::TipSwitchNodeOn, {title});
    } else if (hover_->kind == Kind::NodeGhost) {
        text = trf(Str::TipNodeGhost, {nodeTitle(hover_->a)});
    } else if (hover_->kind == Kind::LabelMode) {
        text = trf(Str::TipRouteMode, {routeModeName(routeMode(value(param)))});
    } else if (hover_->kind == Kind::LabelRemove) {
        text = tr(Str::TipRemoveRoute);
    } else if (routeModeFrom(param) >= 0) { // an INPUTS row's buttons
        const Str tips[] = {Str::TipRoutePhase, Str::TipRouteLinearFm, Str::TipRouteExpFm, Str::TipRouteRing};
        static_assert(std::size(tips) == static_cast<size_t>(RouteMode::Count));
        text = tr(tips[std::clamp(hover_->b, 0, static_cast<int>(RouteMode::Count) - 1)]);
    } else if (isPartialCount(param)) {
        text = trf(Str::TipPartialCount, {std::to_string(hover_->b)});
    }
    else if (hover_->kind == Kind::PresetStep)
        text = tr(hover_->a < 0 ? Str::TipPreviousPreset : Str::TipNextPreset);
    else if (hover_->kind == Kind::PresetNameBox)
        text = tr(controller_.patchEdited() ? Str::TipPresetChanged : Str::TipPresetName);
    else if (hover_->kind == Kind::TempoBox)
        text = tr(Str::TipTempo);
    else if (hover_->kind == Kind::BrowserFolder)
        text = tr(Str::TipOpenFolder);
    else if (hover_->kind == Kind::PresetButton)
        switch (static_cast<PresetAction>(hover_->a)) {
        case PresetAction::Init:
            text = tr(Str::TipInit);
            break;
        case PresetAction::Load:
            text = tr(Str::TipLoad);
            break;
        case PresetAction::Save:
            text = !controller_.presetHasFile() ? tr(Str::TipSaveNoFile)
                   : controller_.patchEdited()  ? trf(Str::TipSave, {controller_.presetName()})
                                                : tr(Str::TipSaveUnchanged);
            break;
        case PresetAction::SaveAs:
            text = tr(presetActionOn(PresetAction::SaveAs) ? Str::TipSaveAs : Str::TipSaveUnchanged);
            break;
        }
    else if (hover_->kind == Kind::SettingsButton)
        text = tr(Str::TipSettings);
    else if (hover_->kind == Kind::SlotBypass)
        text = tr(slotBypassed(hover_->a, hover_->b) ? Str::TipBypassed : Str::TipBypass);
    else if (hover_->kind == Kind::SourcePolarity)
        text = trf(sourceBipolar(hover_->a) ? Str::TipSourceBipolar : Str::TipSourceUnipolar, {sourceName(hover_->a)});
    else if (hover_->kind == Kind::Wire)
        text = tr(Str::TipWire);
    else if (hover_->kind == Kind::ModsSource) // an empty modulation's picks one
        text = tr(hover_->b < 0 ? Str::TipModsPickSource : Str::TipModsSource);
    else if (hover_->kind == Kind::ModsTarget)
        text = tr(hover_->b < 0 ? Str::TipModsPickTarget : Str::TipModsTarget);
    else if (hover_->kind == Kind::ModsAux)
        text = tr(Str::TipModsAux);
    else if (hover_->kind == Kind::ModsAuxClear)
        text = tr(Str::TipModsAuxClear);
    else if (hover_->kind == Kind::ModsScale)
        text = tr(Str::TipModsScale);
    else if (hover_->kind == Kind::ModsCurve) {
        char amount[32];
        formatParam(hover_->param, value(hover_->param), amount, sizeof(amount));
        text = trf(Str::TipModsCurve, {amount});
    }
    else if (hover_->kind == Kind::UndoButton)
        text = undoText(hover_->a == 1);
    else if (hover_->kind == Kind::ShapeReset)
        text = tr(isNoteCurve(hover_->a)                                ? Str::TipResetNoteCurve
                  : isVelocityCurve(hover_->a)                          ? Str::TipResetVelocityCurve
                  : isClipCurve(hover_->a) || isShaperCurve(hover_->a) ? Str::TipResetCurve
                                                                       : Str::TipResetShape);
    else if (hover_->kind == Kind::PartialReset)
        text = tr(Str::TipResetPartials);
    else if (hover_->kind == Kind::PartialEdit)
        text = tr(partialEditing() == hover_->a ? Str::TipClosePartials : Str::TipEditPartials);
    else if (hover_->kind == Kind::ClipDisplay)
        text = tr(Str::TipEditCurve);
    else if (hover_->kind == Kind::ShapeDone)
        text = tr(Str::TipDone);
    else if (hover_->kind == Kind::ShapeCollapse)
        text = tr(Str::TipShapeCollapse);
    else if (hover_->kind == Kind::ShapeExpand)
        text = tr(Str::TipShapeExpand);
    else if (hover_->kind == Kind::ShapeWiden)
        text = tr(Str::TipShapeWiden);
    else if (hover_->kind == Kind::ShapeTool) {
        const Str tips[] = {Str::TipShapeInvert, Str::TipShapeReverse, Str::TipShapeShiftLeft,
                            Str::TipShapeShiftRight, Str::TipShapeCopy, Str::TipShapePaste};
        text = tr(tips[std::clamp(hover_->b, 0, kShapeTools - 1)]);
    } else if (hover_->kind == Kind::ShapeMode)
        text = tr(hover_->b == 0 ? Str::TipShapePoints : hover_->b == 1 ? Str::TipShapeSteps : Str::TipShapeSnap);
    else if (hover_->kind == Kind::ShapeGrid)
        text = tr(hover_->a == 0 ? Str::TipGridAcross : Str::TipGridUpDown);
    else if (hover_->kind == Kind::FxRemove)
        text = tr(Str::TipFxRemove);
    else if (hover_->kind == Kind::SourceRemove)
        text = tr(Str::TipSourceRemove);
    else if (isPowerParam(param))
        text = tr(Str::TipNodePower);
    else if (fixedSwitchOp(param) >= 0)
        text = tr(value(param) >= 0.5 ? Str::TipFixedOn : Str::TipFixedOff);
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
    else if (hover_->kind == Kind::ListButton && isSplitSlope(param))
        text = tr(Str::TipSlope);
    else if (hover_->kind == Kind::ListButton && isFilterSlope(param))
        text = tr(slopeFits(static_cast<FilterType>(std::lround(value(slopeTypeParam(param)))), 0) ? Str::TipSlope
                                                                                                  : Str::TipSlopeBand);
    else if (isLfoClockChoice(*hover_)) {
        const Str tips[] = {Str::TipRetrigger, Str::TipFree, Str::TipAnchor, Str::TipOneShot};
        text = tr(tips[std::clamp(hover_->b, 0, kLfoModes - 1)]);
    }
    else if (sourceFieldOf(param) == SourceParam::Unipolar)
        text = tr(Str::TipUnipolar);
    else if (sourceFieldOf(param) == SourceParam::Bipolar) // a Random card's Unipolar | Bipolar
        text = tr(hover_->b == 1 ? Str::TipBipolar : Str::TipUnipolar);
    else if (field == FxParam::DelaySync)
        text = tr(Str::TipDelaySync);
    else if (field == FxParam::PhaserSync || field == FxParam::FlangerSync)
        text = tr(Str::TipSweepSync);
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
    else if (field == FxParam::PhaserNegative || field == FxParam::FlangerNegative)
        text = tr(hover_->b == 1 ? Str::TipNegative : Str::TipPositive);
    else if (field == FxParam::ShaperLink || shaperFieldOf(param) == ShaperParam::Link)
        text = tr(Str::TipShaperLink);
    else if (field == FxParam::ShaperSymmetric || shaperFieldOf(param) == ShaperParam::Symmetric)
        text = tr(Str::TipShaperSymmetric);
    else if (field == FxParam::FilterType)
        text = filterTypeName(static_cast<FilterType>(hover_->b));
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
    c.fillRect(tip, kRaised, kRadiusCard);
    c.strokeRect(tip, kBorder, 1.0f, kRadiusCard);
    c.text(tip, text, kTextBody, kText);
}

// The pill: in the track's color, the count of what it holds in bold; lighter under the mouse, in the accent while
// its list is open, and hollow while everything it holds is bypassed or its source is off.
void View::paintModPill(Canvas &c, const Widget &w, bool hovered) {
    const bool open = modListOn(w);
    bool live = false;
    int index = 0;
    forEachMod(w.a, [&](const Assignment &m) {
        if (index++ >= w.b && sourceOn(m.source) && !m.bypassed)
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
    const ModHandleSize &sizes = modHandleSize(controller_.handleSize());
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
    listMenu_ = -1; // one open list at a time
    fxMenuOpen_ = false;
    cascade_.reset();
}

bool View::modListPillShown() const {
    const std::vector<Widget> list = widgets();
    return std::any_of(list.begin(), list.end(), [&](const Widget &w) { return w.kind == Kind::ModPill && modListOn(w); });
}

bool View::modListOn(const Widget &pill) const {
    return modList_ && modList_->target == pill.a && modList_->anchor.x == pill.rect.x &&
           modList_->anchor.y == pill.rect.y;
}

Rect View::modListPanel(Rect anchor, int rows, bool aux) const {
    const float w = kCardW - 24.0f + (aux ? kModListAuxW : 0.0f) + 2.0f * kModListPad;
    const float h = kModListTitleH + static_cast<float>(rows) * kModListRowStep + kModListPad - 4.0f;
    const float x = std::clamp(anchor.center().x - w * 0.5f, 4.0f, View::kWidth - 4.0f - w);
    float y = anchor.bottom() + 6.0f; // under the pill; over it when there is no room below
    if (y + h > height() - 4.0f)
        y = std::max(anchor.y - 6.0f - h, 4.0f);
    return {x, y, w, h};
}

void View::paintModList(Canvas &c, const Widget &w) {
    c.fillRect(w.rect.inset(-3.0f), kBackground.alpha(0.35f), kRadiusCard + 3.0f); // a soft edge against what it covers
    c.fillRect(w.rect, kPanel, kRadiusCard);
    c.strokeRect(w.rect, kBorder, 1.0f, kRadiusCard);
    int rows = 0;
    forEachMod(w.a, [&](const Assignment &) { ++rows; });
    const Rect title{w.rect.x + kModListPad + 4.0f, w.rect.y + 2.0f, w.rect.w - 2.0f * kModListPad - 8.0f,
                     kModListTitleH - 4.0f};
    const std::string count = rows == 1 ? std::string(tr(Str::ModListCountOne))
                                        : trf(Str::ModListCount, {std::to_string(rows)});
    const float countW = std::ceil(c.textWidth(count, kTextLabel));
    c.text({title.x, title.y, title.w - countW - 8.0f, title.h}, paramName(w.a), kTextBody, kText, Align::Left, true);
    c.text({title.right() - countW, title.y, countW, title.h}, count, kTextLabel, kDim, Align::Right);
}

// A row: its bypass dot, the source's name (dimmed while bypassed or off), its aux's chip as in the MODS view, its
// depth bar and its remove.
void View::paintModListRow(Canvas &c, const Widget &w) {
    const bool lit = hover_ && hover_->popup && hover_->a == w.a && hover_->b == w.b && drag_.kind == DragKind::None;
    c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
    const bool off = slotBypassed(w.a, w.b) || !sourceOn(w.a);
    const std::string name = sourceName(w.a);
    c.text({w.rect.x + 18.0f, w.rect.y, kSlotNameW, w.rect.h}, name, slotNameSize(c, name), off ? kFaint : kText,
           Align::Left);
    if (const int aux = slotAux([this](int index) { return value(index); }, w.a, w.b); aux >= 0) {
        const Rect chip = modListAuxChip(w.rect);
        const float radius = chip.h * 0.5f;
        c.fillRect(chip, kPanel, radius);
        c.strokeRect(chip, kSourceColors[aux].alpha(0.5f), 1.0f, radius);
        c.fillCircle({chip.x + 10.0f, chip.center().y}, 3.5f, kSourceColors[aux]);
        c.text({chip.x + 18.0f, chip.y, chip.w - 40.0f, chip.h}, sourceName(aux), kTextLabel, kText, Align::Left, true);
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
        if (w.kind == Kind::ModsAux && w.a != source) { // a MODS row's aux cell takes it as the aux too
            if (w.clip.w > 0.0f && (w.rect.y < w.clip.y || w.rect.bottom() > w.clip.bottom()))
                continue;
            const bool over = target && target->same(w);
            c.strokeRect(w.rect.inset(-2.0f), color.alpha(over ? 1.0f : 0.35f), over ? 2.0f : 1.0f, kRadiusCard);
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
        // each has its dots, so the dot is the one on this widget (a graph label's under it).
        Rect area = w.rect.inset(-2.0f);
        if (w.graph)
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
        const Color color = kSourceColors[source].alpha(sourceOn(source) && modulated ? 1.0f : 0.4f); // bypassed: faint
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
    const Color color = kSourceColors[aux].alpha(sourceOn(aux) ? 1.0f : 0.4f);
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
        return {kRatioBox.x, kRatioBox.y + 9.0f, kRatioBox.w, 22.0f};
    if (w.kind == Kind::Knob && w.compact)
        return {w.rect.x + compactTextX(w) - 4.0f, w.rect.y + (w.shrunk ? 14.0f : 17.0f), w.rect.w + 2.0f - (compactTextX(w) - 4.0f), 20.0f};
    if (w.kind == Kind::Knob)
        return {w.rect.center().x - 40.0f, w.rect.y + w.rect.w + 1.0f, 80.0f, 18.0f};
    if (w.kind == Kind::ModDot) // under the small dot
        return {w.rect.center().x - 36.0f, w.rect.bottom() + 3.0f, 72.0f, 18.0f};
    if (w.kind == Kind::WireLabel || w.kind == Kind::OutLabel || w.kind == Kind::GridCell) // over the label or cell
        return {w.rect.center().x - 36.0f, w.rect.center().y - 9.0f, 72.0f, 18.0f};
    if (w.kind == Kind::TempoBox) // right of the metronome
        return {w.rect.x + 24.0f, w.rect.y + 2.0f, w.rect.w - 27.0f, w.rect.h - 4.0f};
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
        return trf(redo ? Str::RedoParam : Str::UndoParam, {paramName(step->changes.front().index)});
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

// A voice filter's cutoff and resonance, or (b = 1) those of the filter effect in slot a.
std::vector<int> filterPointParams(int a, int b) {
    if (b == 1)
        return {paramIndex(a, FxParam::FilterCutoff), paramIndex(a, FxParam::FilterResonance)};
    return {paramIndex(a, FilterParam::Cutoff), paramIndex(a, FilterParam::Resonance)};
}

// A cut's handle sets its Q instead of its gain; a drag may touch either.
std::vector<int> peqPointParams(int slot, int band) {
    return {peqFreqIndex(slot, band), peqGainIndex(slot, band), peqQIndex(slot, band)};
}

} // namespace

void View::applyDrag(Point p) {
    const Widget &w = drag_.widget;
    auto set = [&](int index, double v) {
        v = std::clamp(v, table_[index].min, table_[index].max);
        if (v != value(index))
            controller_.setValue(index, v);
    };
    switch (drag_.kind) {
    case DragKind::Key: {
        // Sliding across keys moves the note; off the keys, the last one keeps sounding.
        const auto hit = hitTest(p);
        if (hit && hit->kind == Kind::Key && hit->a != w.a) {
            controller_.playNote(keyboardBase() + w.a, false);
            controller_.playNote(keyboardBase() + hit->a, true);
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
        bend_ = std::clamp((mid - p.y) / (w.rect.h * 0.5), -1.0, 1.0);
        controller_.pitchWheel(bend_);
        break;
    }
    case DragKind::Value: {
        const ParamDef &def = table_[w.param];
        if (w.kind == Kind::GridCell) // dragging a matrix cell switches on what it joins, in the same step
            switchOnRoute(w.a, w.b);
        if (isRatioDisplay(w)) { // the digit pressed, a step of its place every 6 px (24 with Shift), carrying
            const long steps = std::lround((drag_.start.y - p.y) / (drag_.fine ? 24.0f : 6.0f));
            set(w.param, steps != 0 ? ratioNorm(ratioHundredths(drag_.startValue) + steps * kRatioPlaces[w.b])
                                    : drag_.startValue);
        } else if (w.kind == Kind::SlotBar)
            set(w.param, drag_.fine ? drag_.startValue + (p.x - drag_.start.x) / (w.rect.w * 0.5f) * 0.01
                                    : (p.x - w.rect.center().x) / (w.rect.w * 0.5f));
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
    case DragKind::Node: {
        UiLayout &layout = controller_.layout();
        // Kept off the other nodes; Ctrl held, onto the grid's nearest point it fits at.
        const Point at = ctrlHeld_ ? routingGridPoint(w.a, p) : nodePlace(w.a, p);
        const float x = std::clamp((at.x - kGraph.x) / kGraph.w, 0.0f, 1.0f);
        const float y = std::clamp((at.y - kGraph.y) / kGraph.h, 0.0f, 1.0f);
        if (isBoxNode(w.a)) {
            layout.boxX[w.a - kOps] = x;
            layout.boxY[w.a - kOps] = y;
        } else {
            layout.nodeX[w.a] = x;
            layout.nodeY[w.a] = y;
        }
        break;
    }
    case DragKind::EnvPoint: {
        // Times go by the zoom the drag started with, so the point stays under the mouse; dragged past the
        // display's end, it goes on lengthening its segment while the display zooms out (see envScale()).
        const EnvIndices e = envIndices(w.a);
        const EnvShape s = envShape(envRect(w.a), e, [this](int index) { return value(index); }, drag_.envScale);
        auto time = [&](int param, float from, bool attack = false) {
            return envSpanTime(param, p.x - from, s.scale, attack);
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
        const Rect r = w.b == 1 ? fxFilterArea(w.a) : kFilterDisplay;
        const std::vector<int> params = filterPointParams(w.a, w.b);
        set(params[0], (p.x - r.x - 10.0f) / (r.w - 20.0f));
        set(params[1], (r.bottom() - 10.0f - p.y) / (r.h - 20.0f));
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
    case DragKind::ShapePoint: {
        // Points keep their order: each stays between its neighbors. The first one stays at the
        // start, and a curve's last one at the end. An LFO's may reach a neighbor's time, a line straight up or
        // down (see shapeValue()); a curve's keep a little apart.
        const int shape = w.a;
        const int point = w.b;
        double x = shapeXAt(shape, p);
        double y = shapeYAt(shape, p);
        if (drag_.snap) { // to the grid's lines
            const UiLayout &layout = controller_.layout();
            x = std::round(x * layout.shapeGridX) / layout.shapeGridX;
            const double half = 0.5 * layout.shapeGridY; // lines per unit of height
            y = std::round((y + 1.0) * half) / half - 1.0;
        }
        const int count = shapePointCount(shape);
        // Symmetric, the middle point stays in the middle, and the others right of it.
        if (point > 0 && !(isCurve(shape) && point + 1 == count) && !middlePoint(shape, point)) {
            const double low = std::max(shapePointX(shape, point - 1), curveSymmetric(shape) ? 0.5 : 0.0);
            const double high = point + 1 < count ? shapePointX(shape, point + 1) : 1.0;
            const double apart = isCurve(shape) ? 0.002 : 0.0;
            set(shapeIndex(shape, point, PointField::X), std::max(low + apart, std::min(x, high - apart)));
        }
        set(shapeIndex(shape, point, PointField::Y), y);
        break;
    }
    case DragKind::ShapeStep: // drawn again into the shape as it was, where the mouse is; where it doesn't fit, it stays
        addShapeStep(w.a, unmirrored(w.a, p), drag_.snap, drag_.shapeBefore);
        break;
    case DragKind::Partials: {
        // A quick sweep sets every bar it passed, along a line (in bar heights) from where the last move was.
        const int op = w.a;
        const Rect area = w.b == 0 ? kPartialLevels : kPartialPhases;
        const int k = partialAt(area, p.x, partialsShown(op));
        const double v = std::clamp((area.bottom() - p.y) / area.h, 0.0f, 1.0f);
        const int from = drag_.lastPartial < 0 ? k : drag_.lastPartial;
        const double fromValue = drag_.lastPartial < 0 ? v : drag_.startValue;
        for (int j = std::min(from, k); j <= std::max(from, k); ++j) {
            const double t = from == k ? 1.0 : static_cast<double>(j - from) / (k - from);
            const double bar = fromValue + (v - fromValue) * t;
            if (w.b == 0)
                set(partialLevelIndex(op, j), partialLevelFromBar(bar));
            else
                set(partialPhaseIndex(op, j), bar);
        }
        drag_.lastPartial = k;
        drag_.startValue = v;
        break;
    }
    case DragKind::ShapeCurve: {
        // Dragging up lifts the middle of the segment, whichever way it runs.
        const int shape = w.a;
        const int next = w.b + 1 == shapePointCount(shape) ? 0 : w.b + 1;
        const double rise = value(shapeIndex(shape, next, PointField::Y)) - value(shapeIndex(shape, w.b, PointField::Y));
        set(w.param, drag_.startValue - (rise < 0.0 ? -1.0 : 1.0) * (drag_.start.y - p.y) / 100.0);
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
        // Over an operator's tab, a dragged handle opens it, so its knobs can take the drop; over a pill, its list, so
        // one of its rows can take the handle as that modulation's aux.
        const auto at = hitTest(p);
        if (at && at->kind == Kind::OpTab)
            controller_.layout().opTab = static_cast<InspectorTab>(at->a);
        if (at && at->kind == Kind::ModPill && !modListOn(*at))
            openModList(*at, true);
        break;
    }
    case DragKind::Pan:
        applyPan(p);
        break;
    case DragKind::ModsScroll: { // the rows follow the thumb
        const float room = modsScrollTrack().h - modsThumb().h;
        if (room > 0.0f)
            scrollMods(static_cast<float>(drag_.startValue) + (p.y - drag_.start.y) / room * modsEnd(), false);
        break;
    }
    case DragKind::Bend: {
        // The wire's middle follows the mouse sideways, from where it was grabbed: it lies half as far out as the
        // curve's control point.
        const Point a = nodeCenter(w.a), b = nodeCenter(w.b);
        const float span = std::max(std::hypot(b.x - a.x, b.y - a.y), 1.0f);
        const Point left = leftOf(a, b);
        const float moved = (p.x - drag_.start.x) * left.x + (p.y - drag_.start.y) * left.y;
        // It stops where it would leave the panel.
        const float bend = fitBend(w.a, a, w.b, b,
                                   std::clamp(static_cast<float>(drag_.startValue) + 2.0f * moved / span,
                                              -kMaxWireBend, kMaxWireBend));
        if (bend != wireBend(w.param))
            setWireBend(w.param, bend);
        break;
    }
    case DragKind::SplitPoint:
    case DragKind::FxNodeMove:
        fxGraphDrag(p);
        break;
    case DragKind::Tempo: { // a BPM every 2 px up, in whole BPM; with Shift, a tenth of one, in tenths
        const double step = drag_.fine ? 0.1 : 1.0;
        const double moved = std::trunc((drag_.start.y - p.y) / 2.0f) * step;
        const double bpm = clampTempo(std::round((drag_.startValue + moved) / step) * step);
        if (bpm != controller_.tempo())
            controller_.setTempo(bpm);
        break;
    }
    case DragKind::FxMove: // where it would land follows the mouse, the others gliding aside
        if (drag_.moved) {
            startFxRowGlide();
            retargetRowDrag(p);
        }
        break;
    case DragKind::None:
    case DragKind::Connect:
    case DragKind::Reroute:
    case DragKind::RerouteFrom:
    case DragKind::SourceMove:
        break;
    }
}

bool View::mouseMove(Point p, Modifiers mods) {
    const Point before = hoverPoint_;
    hoverPoint_ = p;
    const bool ctrlChanged = setCtrl(mods.ctrl);
    if (drag_.kind == DragKind::None) {
        // The widget under the mouse, all of it (a tooltip goes under its rect), or for the partial editor's bars,
        // where along them the mouse is: its caption names the partial under it.
        std::optional<Widget> hit = hitTest(p);
        auto same = [](const Widget &a, const Widget &b) {
            return a.same(b) && a.label == b.label && a.compact == b.compact && a.shrunk == b.shrunk &&
                   std::memcmp(&a.rect, &b.rect, sizeof(Rect)) == 0 && std::memcmp(&a.clip, &b.clip, sizeof(Rect)) == 0;
        };
        // Ctrl going down or up, or the mouse going in or out of the routing graph with it held, shows or hides its
        // grid.
        const bool gridEdge = ctrlHeld_ && kRouting.contains(before) != kRouting.contains(p);
        bool changed = ctrlChanged || gridEdge || hit.has_value() != hover_.has_value() || (hit && !same(*hit, *hover_)) ||
                       (hit && hit->kind == Kind::PartialArea) ||
                       (hit && isRatioDisplay(*hit) && ratioDigit(p.x) != ratioDigit(before.x)); // its lit digit
        hover_ = std::move(hit);
        // In a cascading menu, the mouse over a category opens it beside it, and over an entry closes what is open
        // past its level.
        if (changed && cascade_ && hover_ && hover_->kind == Kind::CascadeItem) {
            const std::shared_ptr<const MenuNode> tree = cascadeTree();
            const MenuNode &root = *tree;
            const std::vector<const MenuNode *> levels = cascadeLevels(root);
            const auto level = static_cast<size_t>(hover_->a);
            if (level < levels.size() && hover_->b < static_cast<int>(levels[level]->children.size())) {
                std::vector<int> &open = cascade_->open;
                if (!levels[level]->children[hover_->b].children.empty()) {
                    open.resize(level);
                    open.push_back(hover_->b);
                } else if (open.size() > level) {
                    open.resize(level);
                }
            }
        }
        return changed;
    }
    if ((drag_.kind == DragKind::Value || drag_.kind == DragKind::ModWheel || drag_.kind == DragKind::Glow) &&
        mods.shift != drag_.fine) {
        // Switching to/from fine mode re-anchors the drag so the value doesn't jump.
        drag_.fine = mods.shift;
        drag_.start = p;
        drag_.startValue = drag_.kind == DragKind::Glow ? sliderDragged() : value(drag_.widget.param);
    }
    drag_.snap = drag_.kind == DragKind::ShapePoint || drag_.kind == DragKind::ShapeStep
                     ? shapeSnaps(drag_.widget.a, mods.ctrl)
                     : mods.ctrl;
    drag_.now = p;
    if (std::hypot(p.x - drag_.start.x, p.y - drag_.start.y) > 3.0f)
        drag_.moved = true;
    if ((drag_.kind == DragKind::Connect || drag_.kind == DragKind::Reroute) && !drag_.leftNode)
        drag_.leftNode = clearOfNode(drag_.widget.a, nodeCenter(drag_.widget.a), p);
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
    fxRowGlide_.on = false; // the row's cards still settling snap into place, where a click means them
    // Before a drag can take a route down to 0 %: the poll timer waits while the mouse moves, and a
    // state load may have replaced the layout since it last ran.
    keepRoutes();
    const auto hit = hitTest(p);
    // The MODS view's filter lets the keyboard go at a click anywhere else.
    if (!(hit && (hit->kind == Kind::SearchField || hit->kind == Kind::SearchClear) &&
          hit->a == static_cast<int>(Field::ModsFilter)))
        modsFiltering_ = false;
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
                      w.kind == Kind::OutLabel || w.kind == Kind::GridCell || w.kind == Kind::TempoBox)) {
        startTyping(w);
        return;
    }
    if (drawsStep(w, mods)) {
        // Held, the step follows the mouse, up and down and from column to column, as one undo step.
        const int shape = w.a;
        std::vector<double> before;
        for (int f = 0; f < shapeParamCount(shape); ++f)
            before.push_back(value(shapeBase(shape) + f));
        controller_.beginStep();
        const bool snap = shapeSnaps(shape, mods.ctrl);
        if (!addShapeStep(shape, unmirrored(shape, p), snap, before)) {
            controller_.endStep();
            flash(trf(Str::MsgShapeFull, {std::to_string(shapeMaxPoints(shape))}));
            return;
        }
        drag_.kind = DragKind::ShapeStep;
        drag_.widget = w;
        drag_.start = drag_.now = p;
        drag_.snap = snap;
        drag_.step = true;
        drag_.shapeBefore = std::move(before);
        return;
    }
    drag_.widget = w;
    drag_.start = drag_.now = p;
    drag_.fine = mods.shift;
    UiLayout &layout = controller_.layout();
    if (fxGraphPress(w, p)) // the effects graph's own
        return;

    switch (w.kind) {
    case Kind::Knob:
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
    case Kind::ModDot:
    case Kind::SlotBar:
    case Kind::ModsScale:
    case Kind::ModsCurve:
        drag_.kind = DragKind::Value;
        drag_.startValue = value(w.param);
        if (isRatioDisplay(w)) // the digit pressed is the one dragged, wherever the mouse goes
            drag_.widget.b = ratioDigit(p.x);
        controller_.beginEdit(w.param);
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
    case Kind::ModsSource:
        openCascade(CascadeKind::Source, w.a, w.b, w.rect);
        break;
    case Kind::ModsTarget:
        openCascade(CascadeKind::Target, w.a, w.b, w.rect);
        break;
    case Kind::ModsAux:
        openCascade(CascadeKind::Aux, w.a, w.b, w.rect);
        break;
    case Kind::ModsAdd:
        addEmptyMod();
        break;
    case Kind::ModsAuxClear:
        setAux(w.a, w.b, -1);
        break;
    case Kind::CascadeMenu:
        if (w.a == 0) // a click anywhere else closes the menu
            cascade_.reset();
        break;
    case Kind::CascadeItem:
        pickCascade(w.a, w.b);
        break;
    case Kind::ModsScroll: {
        // On the thumb, drag it; elsewhere on the track, the thumb jumps to center on the click and can be dragged on.
        const Rect track = modsScrollTrack();
        const Rect thumb = modsThumb();
        if (p.y < thumb.y || p.y >= thumb.bottom())
            scrollMods((p.y - thumb.h * 0.5f - track.y) / std::max(track.h - thumb.h, 1.0f) * modsEnd(), false);
        drag_.kind = DragKind::ModsScroll;
        drag_.startValue = modsFirst();
        break;
    }
    case Kind::Node:
        layout.selectedNode = w.a;
        drag_.kind = DragKind::Node;
        drag_.step = true; // moving it is an undo step
        controller_.beginStep();
        break;
    case Kind::Port:
        drag_.kind = DragKind::Connect;
        break;
    case Kind::NodeDelete:
        edit(enabledParam(w.a), 0.0); // switch it off, as its menu does
        hover_.reset();
        break;
    case Kind::WireEnd:
        drag_.kind = DragKind::Reroute;
        break;
    case Kind::WireStart:
        drag_.kind = DragKind::RerouteFrom;
        break;
    case Kind::Wire:
        drag_.kind = DragKind::Bend;
        drag_.startValue = fitBend(w.a, nodeCenter(w.a), w.b, nodeCenter(w.b), wireBend(w.param)); // as drawn
        drag_.step = true; // bending it is an undo step
        controller_.beginStep();
        break;
    case Kind::SourceHandle:
    case Kind::MacroHandle:
        drag_.kind = DragKind::ModSource;
        break;
    case Kind::EnvPoint:
        drag_.envScale = envScale(w.a); // before the drag starts, while it still fits the display
        drag_.kind = DragKind::EnvPoint;
        for (int index : envPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::EnvCurve:
        drag_.envScale = envScale(w.a);
        drag_.kind = DragKind::EnvCurve;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::FilterPoint:
        drag_.kind = DragKind::FilterPoint;
        for (int index : filterPointParams(w.a, w.b))
            controller_.beginEdit(index);
        break;
    case Kind::DynZone: // up and down for its ratio, as on a knob; its band's knobs show
        fxBand_[w.a] = w.b / 2;
        drag_.kind = DragKind::Value;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::DynGrip:
        fxBand_[w.a] = w.b / 2; // its band's knobs show
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
        fxBand_[w.a] = w.b; // its knobs show
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
    case Kind::FxMenuButton:
        if (fxChain().size() >= static_cast<size_t>(kFxSlots)) {
            flash(trf(Str::MsgChainFull, {std::to_string(kFxSlots)}));
        } else {
            fxMenuOpen_ = !fxMenuOpen_ || fxMenuAt_.x >= 0.0f; // from another list, to the button's
            fxMenuAt_ = {-1.0f, -1.0f};
            // In the row, at the main line's end; in the graph, after the node picked.
            fxSlot_ = fxGraph() ? FxSlotAt{} : FxSlotAt{0, static_cast<int>(fxLine().size()), 0};
        }
        break;
    case Kind::FxShadow: // the list at the mouse, kept inside the editor, for the main line's end
        fxMenuAt_ = menuAtMouse(p, fxMenu({0.0f, 0.0f}, true));
        fxMenuOpen_ = true;
        fxSlot_ = {0, static_cast<int>(fxLine().size()), 0};
        hover_.reset();
        break;
    case Kind::FxInsert: { // the list over the cards on either side, its middle at the arrow, kept inside the panel
        const Rect menu = fxMenu({0.0f, 0.0f}, w.a == 0); // the main line's with a split after the effects
        fxMenuAt_ = {std::clamp(w.rect.center().x - 0.5f * menu.w, kFxPanel.x + 8.0f, kFxPanel.right() - 8.0f - menu.w),
                     std::min(kFxTop + 8.0f, kFxPanel.bottom() - 4.0f - menu.h)};
        fxMenuOpen_ = true;
        fxSlot_ = {w.a, w.b / kFxGapStride, w.b % kFxGapStride};
        hover_.reset();
        break;
    }
    case Kind::SourceMenuButton:
        if (!sourceShadow()) {
            flash(trf(Str::MsgSourcesFull, {std::to_string(kMaxSources)}));
        } else {
            sourceMenuOpen_ = !sourceMenuOpen_ || sourceMenuAt_.x >= 0.0f; // from the dashed card's, to the button's
            sourceMenuAt_ = {-1.0f, -1.0f}; // collapsed, the panel opens with the source picked (addSource())
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
    case Kind::FxMenu:
        if (w.a == 0) // a click anywhere else closes the list
            fxMenuOpen_ = false;
        break;
    case Kind::FxAdd:
        fxMenuOpen_ = false;
        addEffect(static_cast<FxType>(w.a));
        fxSlot_ = {};
        break;
    case Kind::FxRemove:
        removeEffect(w.a);
        break;
    case Kind::FxClear: // asking first, while there is anything to remove
        if (fxAnyToClear()) {
            confirm_ = kConfirmFxClear;
            hover_.reset();
        }
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
        fxBand_[w.a] = w.b;
        break;
    case Kind::PeqBandOn: // its knobs show too
        fxBand_[w.a] = w.b;
        edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
        break;
    case Kind::SourcesToggle:
        layout.sourcesCollapsed = !layout.sourcesCollapsed;
        controller_.layoutChanged(); // asks the host for the new window size
        break;
    case Kind::KeysToggle:
        layout.keysCollapsed = !layout.keysCollapsed;
        controller_.layoutChanged();
        break;
    case Kind::NodeTab:
    case Kind::MatrixLabel:
        // The selected tab switches its operator or filter on and off; another tab selects that one,
        // switching it on if it was off. A name in the matrix does the same.
        if (layout.selectedNode == w.a)
            edit(enabledParam(w.a), nodeOn(w.a) ? 0.0 : 1.0);
        else if (!nodeOn(w.a))
            edit(enabledParam(w.a), 1.0);
        layout.selectedNode = w.a;
        break;
    case Kind::NodeGhost: // into the graph where it was, and into the inspector
        edit(enabledParam(w.a), 1.0);
        layout.selectedNode = w.a;
        break;
    case Kind::ViewTab:
        showView(static_cast<RoutingView>(w.a));
        break;
    case Kind::OpTab:
        layout.opTab = static_cast<InspectorTab>(w.a);
        break;
    case Kind::ScopeCycles:
        layout.scopeCycles = w.b;
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
    case Kind::FxGrip:
    case Kind::FxPower: // the icon: let go without moving, it switches the effect
        if (fxGraph()) { // in the graph's inspector, where the card doesn't move
            if (w.kind == Kind::FxPower)
                edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
            break;
        }
        drag_.kind = DragKind::FxMove;
        drag_.scrollStart = fxScroll();
        startRowDrag(w.a);
        break;
    case Kind::FxFold:
        setFolded(true, w.a, !fxFolded(w.a));
        hover_.reset();
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

    case Kind::ShapeWiden:
    case Kind::LfoDisplay: // a Custom LFO's widens its card, which then has the shape (and the full-size editor not)
        if (std::lround(value(paramIndex(w.a, SourceParam::Shape))) == static_cast<long>(LfoShape::Custom)) {
            wideLfo_ = w.a;
            if (shapeEditing() >= 0 && !isCurve(shapeEditing()))
                shapeEditor_ = -1;
            revealSource(w.a);
            hover_.reset();
        }
        break;
    case Kind::ShapeDone: // an LFO's full-size editor goes back into its widened card
        if (const int shape = shapeEditing(); shape >= 0 && !isCurve(shape)) {
            wideLfo_ = shape;
            revealSource(shape);
        }
        shapeEditor_ = -1;
        partialEditor_ = -1;
        break;
    case Kind::ShapeCollapse:
        wideLfo_ = -1;
        hover_.reset();
        break;
    case Kind::ShapeExpand:
        shapeEditor_ = w.a;
        partialEditor_ = -1;
        hover_.reset();
        break;
    case Kind::ShapeTool:
        applyShapeTool(w.a, static_cast<ShapeTool>(w.b));
        break;
    case Kind::ShapeMode:
        if (w.b == 2)
            layout.shapeSnap = !layout.shapeSnap;
        else
            layout.shapeSteps = w.b == 1;
        controller_.layoutChanged();
        break;
    case Kind::ClipDisplay:
        shapeEditor_ = kMaxSources + w.a;
        break;
    case Kind::PartialEdit: // a second click closes it
        partialEditor_ = partialEditing() == w.a ? -1 : w.a;
        shapeEditor_ = -1;
        break;
    case Kind::NoteDisplay:
        shapeEditor_ = shapeEditor_ == curveOf(w.a) ? -1 : curveOf(w.a); // a second click closes it
        hover_.reset();
        break;
    case Kind::ListButton:
        if (isFilterSlope(w.param) && !hasSlope(static_cast<FilterType>(std::lround(value(slopeTypeParam(w.param))))))
            break; // a comb has no slope: its list stays shut
        listMenu_ = listMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::LabelMode:
        listMenu_ = listMenu_ == w.param ? -1 : w.param;
        break;
    case Kind::LabelRemove:
        listMenu_ = -1;
        disconnect(w.param);
        hover_.reset();
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
        // Custom widens an LFO's card, to draw the shape right there; another shape takes it back to its size.
        if (isLfoShape(w.param) && w.b == static_cast<int>(LfoShape::Custom)) {
            const int lfo = sourceOf(w.param);
            wideLfo_ = lfo;
            if (shapeEditing() >= 0 && !isCurve(shapeEditing()) && shapeEditing() != lfo)
                shapeEditor_ = -1;
            revealSource(lfo);
        } else if (isLfoShape(w.param) && sourceOf(w.param) == wideLfo_) {
            wideLfo_ = -1;
        }
        break;
    case Kind::PeqTypeButton:
        fxBand_[w.a] = w.b;
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
        controller_.playNote(keyboardBase() + w.a, true);
        break;
    case Kind::Octave:
        octave_ = std::clamp(octave_ + w.a, -2, 2);
        break;
    case Kind::PresetButton: {
        // Init and Load ask first when they would throw away changes, and Save before it overwrites a preset's file.
        // Save and Save As do nothing while greyed out.
        pendingPreset_ = -1;
        const auto action = static_cast<PresetAction>(w.a);
        if (!presetActionOn(action))
            break;
        const bool ask = action == PresetAction::Save     ? controller_.writesPresetFiles()
                         : action == PresetAction::SaveAs ? false
                                                          : controller_.patchEdited();
        if (ask) {
            confirm_ = w.a;
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
        drag_.startValue = controller_.tempo();
        break;
    case Kind::PresetNameBox: {
        controller_.rescanPresets();
        browserOpen_ = true;
        clearField(Field::Browser);
        revealBrowserRow(controller_.presetIndex(), true); // the current preset about in the middle
        hover_.reset();
        break;
    }
    case Kind::BrowserBackdrop:
        if (w.a == 0) // a click around the browser closes it
            browserOpen_ = false;
        break;
    case Kind::BrowserRow:
        openPreset(w.a);
        break;
    case Kind::BrowserClose:
        browserOpen_ = false;
        hover_.reset();
        break;
    case Kind::BrowserFolder:
        controller_.openPresetFolder();
        break;
    case Kind::SearchClear: // the MODS view's filter keeps the keyboard, for what is typed next
        clearField(static_cast<Field>(w.a));
        if (w.a == static_cast<int>(Field::Browser))
            revealBrowserRow(controller_.presetIndex(), true);
        modsFiltering_ = w.a == static_cast<int>(Field::ModsFilter);
        hover_.reset();
        break;
    case Kind::SearchField: { // the caret where it is clicked, and a drag selects; with Shift, from where it was
        TextField &t = field(static_cast<Field>(w.a));
        caretTicks_ = 0;
        if (w.a == static_cast<int>(Field::ModsFilter) && !modsFilterOpen()) { // the magnifier opens it
            modsFiltering_ = true;
            t.caret = t.anchor = t.text.size();
            break;
        }
        modsFiltering_ = w.a == static_cast<int>(Field::ModsFilter);
        t.caret = t.stopAt(p.x);
        if (!mods.shift)
            t.anchor = t.caret;
        drag_.kind = DragKind::TextSelect;
        break;
    }
    case Kind::ConfirmBackdrop:
        if (w.a == 0) { // a click around the dialog cancels it
            confirm_ = -1;
            pendingPreset_ = -1;
        }
        break;
    case Kind::ConfirmButton: {
        const auto action = static_cast<PresetAction>(confirm_);
        const bool clear = confirm_ == kConfirmFxClear;
        const bool bands = confirm_ == kConfirmMidBand;
        const bool split = confirm_ == kConfirmSplitRemove;
        confirm_ = -1;
        hover_.reset();
        if (split) { // the split removed, its bands' effects kept on the main line (1) or with it (4)
            if (w.a == 1 || w.a == 4)
                removeFxNode(fxSplitNode(confirmSplit_), w.a == 4 ? 1 : 0);
            confirmSplit_ = -1;
        } else if (bands) { // back in two bands: the middle band's effects removed (1), onto the low band (2), the high one (3)
            if (w.a > 0)
                setSplitBands(confirmSplit_, 2, w.a == 1 ? 0 : w.a == 2 ? 2 : 1);
            confirmSplit_ = -1;
        } else if (w.a == 1 && clear)
            clearFx();
        else if (w.a == 1)
            runPreset(action);
        pendingPreset_ = -1;
        break;
    }
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
    case Kind::SettingsTheme:
        controller_.setTheme(kThemes[w.b].name);
        break;
    case Kind::SettingsHandles:
        controller_.setHandleSize(w.b);
        break;
    case Kind::SettingsGlow: // dragged; the amount is kept when it is let go
    case Kind::SettingsLinks:
        drag_.kind = DragKind::Glow;
        drag_.startValue = sliderDragged() = w.kind == Kind::SettingsLinks ? shownLinks() : shownGlow();
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::SettingsClose:
        settingsOpen_ = false;
        hover_.reset();
        break;
    case Kind::ModWheel:
        drag_.kind = DragKind::ModWheel;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        if (!drag_.fine)
            applyDrag(p);
        break;
    case Kind::BendWheel:
        drag_.kind = DragKind::BendWheel;
        applyDrag(p);
        break;
    case Kind::ShapeArea:
        break;
    case Kind::ShapePoint:
        drag_.kind = DragKind::ShapePoint;
        for (PointField field : {PointField::X, PointField::Y})
            controller_.beginEdit(shapeIndex(w.a, w.b, field));
        break;
    case Kind::ShapeCurve:
        drag_.kind = DragKind::ShapeCurve;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        break;
    case Kind::WaveButton:
        edit(paramIndex(w.a, OpParam::Wave), w.b);
        break;
    case Kind::TypeButton:
        edit(paramIndex(w.a, FilterParam::Type), w.b);
        break;
    case Kind::SlotRemove:
        removeSlot(w.a, w.b);
        break;
    case Kind::SlotBypass:
        toggleBypass(w.a, w.b);
        break;
    case Kind::SourcePolarity:
        setSourcePolarity(w.a, !sourceBipolar(w.a));
        break;
    case Kind::SourceMods:
        showSourceMods(w.a);
        break;
    case Kind::ValueText:
    case Kind::MacroName:
        break; // double-click types a value, or renames the macro
    case Kind::WaveMore:
        waveMenu_ = waveMenu_ == w.a ? -1 : w.a;
        break;
    case Kind::WaveMenu:
        if (w.a == 0) // a click anywhere else closes the list
            waveMenu_ = -1;
        break;
    case Kind::WaveMenuItem: {
        waveMenu_ = -1;
        const UndoScope step(controller_);
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
                editMany(changes);
            }
            partialEditor_ = w.a;
            shapeEditor_ = -1;
        }
        break;
    }
    case Kind::PartialArea:
        drag_.kind = DragKind::Partials;
        drag_.step = true; // one undo step for the whole stroke
        controller_.beginStep();
        applyDrag(p);
        break;
    case Kind::PartialReset: {
        std::vector<std::pair<int, double>> changes;
        for (int k = 0; k < kPartials; ++k) {
            changes.push_back({partialLevelIndex(w.a, k), table_[partialLevelIndex(w.a, k)].def});
            changes.push_back({partialPhaseIndex(w.a, k), table_[partialPhaseIndex(w.a, k)].def});
        }
        editMany(changes);
        break;
    }
    case Kind::ShapeReset:
        resetShape(w.a);
        break;
    case Kind::ShapeGrid:
        if (w.b != 0)
            stepShapeGrid(w.a, w.b);
        break;
    case Kind::UndoButton:
        if (w.a == 1)
            redo();
        else
            undo();
        break;
    default: // the effects graph's own (fxGraphPress()), and the kinds a press does nothing on
        break;
    }
}

void View::mouseUp(Point p, Modifiers) {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::Node:
        if (drag_.moved)
            controller_.layoutChanged();
        break;
    case DragKind::Connect: {
        if (!drag_.moved) { // a click on the edge picks the node, like one further inside
            controller_.layout().selectedNode = w.a;
            break;
        }
        const int to = nodeAt(p);
        if (to >= 0) {
            connect(w.a, to);
        } else if (onBus(p) && value(outParam(w.a)) <= 0.0) {
            edit(outParam(w.a), 1.0);
        }
        break;
    }
    case DragKind::RerouteFrom: {
        if (!drag_.moved) // a click leaves it be
            break;
        if (const int source = nodeAt(p); source >= 0)
            rerouteFrom(w.a, w.b, source);
        else // let go of off the nodes, the route goes, with its modulation
            disconnect(w.param);
        break;
    }
    case DragKind::Reroute: {
        if (!drag_.moved) // a click leaves it be
            break;
        const int into = nodeAt(p) >= 0 ? nodeAt(p) : (onBus(p) ? kNodes : -1);
        if (into >= 0)
            reroute(w.a, w.b, into);
        else // let go of off the nodes and the bus, the route goes, with its modulation
            disconnect(w.param);
        break;
    }
    case DragKind::ModSource: {
        const auto target = hitTest(p);
        // On a depth handle, a row of a pill's list or a MODS row's aux cell: it becomes that modulation's aux.
        const bool row = target && target->popup && target->kind != Kind::ModList;
        if (target && (target->kind == Kind::ModDot || target->kind == Kind::ModsAux || row) && target->a != w.a)
            setAux(target->a, target->b, w.a);
        else if (target && droppable(*target, w.a)) // only where the overlay rings a target
            assign(w.a, target->param);
        // A list that opened under the handle stays open only when the handle went onto one of its rows.
        if (modList_ && modList_->sprung) {
            if (row)
                modList_->sprung = false;
            else
                modList_.reset();
        }
        break;
    }
    case DragKind::Glow:
        if (w.kind == Kind::SettingsLinks)
            controller_.setLinkIntensity(linksDragged_);
        else
            controller_.setGlow(glowDragged_);
        break;
    case DragKind::FxMove: {
        drag_.now = p;
        if (!drag_.moved) { // a click: the icon switches the effect, and the card stays where it is
            fxDrag_.reset();
            if (w.kind == Kind::FxPower)
                edit(w.param, value(w.param) >= 0.5 ? 0.0 : 1.0);
            break;
        }
        retargetRowDrag(p);
        dropRowDrag(); // where it would land, along the main line or onto a container's band
        break;
    }
    case DragKind::SourceMove:
        drag_.now = p;
        if (drag_.moved) // a click leaves the card where it is
            setSourceOrder(sourcesShown());
        break;
    case DragKind::FxNodeMove: // where it is let go of, if it moved
        drag_.now = p;
        dropFxNode(true);
        break;
    case DragKind::Tempo: // each move set it already
        break;
    case DragKind::SplitPoint:
    case DragKind::Value:
    case DragKind::EnvPoint:
    case DragKind::EnvCurve:
    case DragKind::FilterPoint:
    case DragKind::PeqPoint:
    case DragKind::AmpMic:
    case DragKind::UtilityHandle:
    case DragKind::DynGrip:
    case DragKind::ShapePoint:
    case DragKind::ShapeCurve:
    case DragKind::ShapeStep:
    case DragKind::Partials:
    case DragKind::Key:
    case DragKind::ModWheel:
    case DragKind::BendWheel:
    case DragKind::Scroll:
    case DragKind::TextSelect:
    case DragKind::Bend: // each move set it already
    case DragKind::ModsScroll:
    case DragKind::Pan: // the left button let go of mid-pan ends it too
    case DragKind::None:
        break; // finishDrag() ends what they hold
    }
    finishDrag();
    hover_ = hitTest(p);
}

void View::cancelDrag() {
    if (drag_.kind == DragKind::None)
        return;
    if (drag_.kind == DragKind::Node && drag_.moved) // it stays where it was taken, as an undo step
        controller_.layoutChanged();
    if (drag_.kind == DragKind::ModSource && modList_ && modList_->sprung) // opened under the handle, for it alone
        modList_.reset();
    if (drag_.kind == DragKind::FxNodeMove) // back where it was taken from
        dropFxNode(false);
    if (drag_.kind == DragKind::FxMove) // a card or container stays where it was
        fxDrag_.reset();
    finishDrag();
    hover_.reset();
}

void View::finishDrag() {
    const Widget &w = drag_.widget;
    switch (drag_.kind) {
    case DragKind::Value:
        keepRoutes(); // a route it switched on, in its undo step (see edit())
        controller_.endEdit(w.param);
        break;
    case DragKind::EnvCurve:
    case DragKind::ShapeCurve:
    case DragKind::ModWheel:
    case DragKind::SplitPoint:
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
        for (int index : filterPointParams(w.a, w.b))
            controller_.endEdit(index);
        break;
    case DragKind::ShapePoint:
        for (PointField field : {PointField::Y, PointField::X})
            controller_.endEdit(shapeIndex(w.a, w.b, field));
        break;
    case DragKind::Key:
        controller_.playNote(keyboardBase() + w.a, false);
        break;
    case DragKind::BendWheel: // springs back to center
        bend_ = 0.0;
        controller_.pitchWheel(0.0);
        break;
    case DragKind::Glow: // the amount it was dragged to is kept only once it is let go (see mouseUp())
        glowDragged_ = linksDragged_ = -1.0f;
        break;
    case DragKind::Partials: // its undo step closes below
    case DragKind::ShapeStep:
    case DragKind::Node:
    case DragKind::Bend:
    case DragKind::ModsScroll:
    case DragKind::Connect:
    case DragKind::Reroute:
    case DragKind::RerouteFrom:
    case DragKind::ModSource:
    case DragKind::FxMove:
    case DragKind::SourceMove:
    case DragKind::Scroll:
    case DragKind::Pan:
    case DragKind::TextSelect:
    case DragKind::FxNodeMove:
    case DragKind::Tempo:
    case DragKind::None:
        break;
    }
    if (drag_.step)
        controller_.endStep();
    drag_ = Drag{};
}

void View::mouseLeave() {
    if (drag_.kind == DragKind::None)
        hover_.reset();
}

void View::middleDown(Point p) {
    if (drag_.kind != DragKind::None || settingsOpen_ || confirm_ >= 0) // a left-button drag, or a dialog, goes on
        return;
    std::optional<PanArea> area;
    float first = 0.0f;
    if (browserOpen_) { // only its list, over everything else
        if (!browserList(browserPanel(height())).contains(p))
            return;
        const int most = std::max(static_cast<int>(browserRows().size()) - browserVisibleRows(browserPanel(height())), 0);
        area = PanArea::Browser;
        first = static_cast<float>(std::clamp(browserScroll_, 0, most));
    } else {
        // On an open list or menu, it does nothing; elsewhere they close as a click there closes them, since what
        // they were opened on may scroll away.
        if (const auto hit = hitTest(p)) {
            const Kind k = hit->kind;
            if (hit->popup || k == Kind::ModList || k == Kind::ModListRow || k == Kind::FxAdd || k == Kind::FxAddNode ||
                k == Kind::SourceMenuItem ||
                k == Kind::ListItem || k == Kind::PeqTypeItem || k == Kind::WaveMenuItem || k == Kind::CascadeItem ||
                ((k == Kind::FxMenu || k == Kind::SourceMenu || k == Kind::ListMenu || k == Kind::PeqTypeMenu ||
                  k == Kind::WaveMenu ||
                  k == Kind::CascadeMenu) &&
                 hit->a != 0))
                return;
        }
        if (!controller_.layout().sourcesCollapsed && modPanel(false).contains(p)) {
            area = PanArea::Sources;
            first = sourceScroll();
        } else if (modsWide() && kFxPanel.contains(p)) {
            area = PanArea::Mods;
            first = modsFirst();
        } else if (fxWide() && fxGraph() && fxGraphArea().contains(p)) { // the graph, by pixels
            area = PanArea::Fx;
            first = fxGraphScroll(fxGridLayout(fxLine()));
        } else if (fxWide() && !fxGraph() && kFxPanel.contains(p)) {
            area = PanArea::Fx;
            first = fxScroll();
        }
        if (!area)
            return;
        modList_.reset();
        cascade_.reset();
        fxMenuOpen_ = false;
        sourceMenuOpen_ = false;
        listMenu_ = -1;
        peqMenu_ = -1;
        waveMenu_ = -1;
    }
    menuLive_ = false;
    endTyping(true);
    if (*area == PanArea::Mods)
        scrollMods(first, false); // a glide the wheel started stops where it shows
    drag_ = Drag{};
    drag_.kind = DragKind::Pan;
    drag_.widget.a = static_cast<int>(*area);
    drag_.start = drag_.now = p;
    drag_.startValue = first;
}

void View::middleUp(Point p) {
    if (drag_.kind != DragKind::Pan)
        return; // a left-button drag goes on
    finishDrag();
    hover_ = hitTest(p);
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
    case PanArea::Fx:
        if (fxGraph())
            fxGraphScroll_ = hold(first - across, fxGraphScrollMost(fxGridLayout(fxLine())));
        else
            setScroll(ScrollArea::Fx,
                      hold(first - across / (kFxCardW + kFxGap), scrollLength(ScrollArea::Fx) - kVisibleFx));
        break;
    case PanArea::Sources:
        setScroll(ScrollArea::Sources,
                  hold(first - across / (kCardW + kCardGap), scrollLength(ScrollArea::Sources) - kVisibleCards));
        break;
    case PanArea::Mods:
        scrollMods(hold(first - down / kModsRowStep, modsEnd()), false);
        break;
    case PanArea::Browser: { // in whole rows
        const int rows = static_cast<int>(browserRows().size()) - browserVisibleRows(browserPanel(height()));
        browserScroll_ = static_cast<int>(std::lround(hold(first - down / kBrowserRowH, static_cast<float>(rows))));
        break;
    }
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
        !(hit && (hit->kind == Kind::Key || drawsStep(*hit, mods))))
        return;
    if (hit && (hit->kind == Kind::Key || hit->kind == Kind::Octave || hit->kind == Kind::UndoButton ||
                hit->kind == Kind::PresetStep || hit->kind == Kind::FxPower || hit->kind == Kind::FxNodePower ||
                hit->kind == Kind::SlotBypass || (hit->kind == Kind::ShapeGrid && hit->b != 0) ||
                hit->kind == Kind::ShapeTool || drawsStep(*hit, mods))) {
        // Windows turns a quick second click into a double-click instead of a press:
        // play it as a press so fast repeated notes, octave steps, undos and steps aren't dropped.
        mouseDown(p, mods);
        return;
    }
    if (hit && (hit->kind == Kind::FxMode || hit->kind == Kind::FxSlot || hit->kind == Kind::FxGap ||
                hit->kind == Kind::FxNode)) {
        mouseDown(p, mods);
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
    if (hit && hit->kind == Kind::Wire) { // back to how it bows to begin with
        const UndoScope step(controller_);
        setWireBend(hit->param, kDefaultWireBend);
        return;
    }
    if (hit && hit->kind == Kind::SearchField) { // the word there, up to the spaces either side
        field(static_cast<Field>(hit->a)).selectWord(p.x);
        caretTicks_ = 0;
        return;
    }
    if (hit && hit->kind == Kind::PartialArea) { // that bar to nothing
        const int k = partialAt(hit->b == 0 ? kPartialLevels : kPartialPhases, p.x, partialsShown(hit->a));
        edit(hit->b == 0 ? partialLevelIndex(hit->a, k) : partialPhaseIndex(hit->a, k), 0.0);
        return;
    }
    if (hit && hit->kind == Kind::ShapeArea) {
        addShapePoint(hit->a, unmirrored(hit->a, p));
        return;
    }
    if (hit && hit->kind == Kind::ShapePoint) {
        removeShapePoint(hit->a, hit->b);
        return;
    }
    if (hit && (hit->kind == Kind::ShapeCurve || hit->kind == Kind::EnvCurve || hit->kind == Kind::ModsCurve)) {
        edit(hit->param, 0.0); // straight again: 0 %, whatever the segment started as
        return;
    }
    if (hit && hit->kind == Kind::DynZone) { // its ratio back to where it starts
        edit(hit->param, resetValue(hit->param));
        return;
    }
    if (hit && hit->kind == Kind::DynGrip) { // its threshold back to where it starts
        const int threshold = mdThreshold(hit->a, hit->b);
        edit(threshold, resetValue(threshold));
        return;
    }
    if (hit && hit->kind == Kind::PeqPoint) {
        const PeqType type = peqType(value(peqTypeIndex(hit->a, hit->b)));
        if (isCut(type))
            edit(peqQIndex(hit->a, hit->b), normFromQ(std::numbers::sqrt2 * 0.5)); // no resonance
        else if (type == PeqType::Notch)
            edit(peqQIndex(hit->a, hit->b), resetValue(peqQIndex(hit->a, hit->b))); // its width as it starts
        else
            edit(peqGainIndex(hit->a, hit->b), 0.0); // flat again
        return;
    }
    if (!hit || hit->param < 0)
        return;
    switch (hit->kind) {
    case Kind::ModDot:
        removeSlot(hit->a, hit->b);
        break;
    case Kind::Knob:
    case Kind::SlotBar:
        edit(hit->param, resetValue(hit->param));
        break;
    case Kind::OutLabel: // like a wire label: shown, so wired, so off
        disconnect(hit->param);
        break;
    case Kind::GridCell:
    case Kind::WireLabel: {
        // Toggle: off, or on at a typical amount for that kind of route.
        const int to = hit->b;
        const bool full = to >= kOps && hit->a != to; // into a filter, a shaper or the output; modulation starts lower
        const double on = full ? 1.0 : (hit->a == to ? 0.25 : 0.3);
        if (wired(hit->param)) {
            disconnect(hit->param);
        } else {
            const UndoScope step(controller_);
            if (hit->kind == Kind::GridCell) // a route in the matrix switches on what it joins
                switchOnRoute(hit->a, to);
            edit(hit->param, on);
        }
        break;
    }
    default:
        break;
    }
}

// The wheel's targets besides the params (see wheelNotches()): the preset browser's list, and the shape grid's axes.
constexpr int kWheelBrowser = kParamCount;
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
    if (browserOpen_) { // three rows a notch
        const int rows = static_cast<int>(browserRows().size());
        const int most = std::max(rows - browserVisibleRows(browserPanel(height())), 0);
        browserScroll_ = std::clamp(std::clamp(browserScroll_, 0, most) - wheelNotches(kWheelBrowser, steps * 3.0f), 0, most);
        return;
    }
    keepRoutes(); // as in mouseDown(): scrolling can take a route down to 0 % too
    const auto hit = hitTest(p);
    // Over a pill's list, only its depth bars take the wheel: nothing under the list scrolls.
    if (hit && (hit->popup || hit->kind == Kind::ModPill) && hit->kind != Kind::SlotBar && hit->kind != Kind::ValueText)
        return;
    // What moves in whole steps (the grid, a stepped param, an operator's pitch, a note) moves by whole notches.
    const int target = hit && hit->kind == Kind::ShapeGrid  ? kWheelGrid + hit->a
                       : hit && hit->kind == Kind::TempoBox ? kWheelTempo
                                                             : (hit ? hit->param : -1);
    const int whole = wheelNotches(target, steps);
    if (hit && hit->kind == Kind::TempoBox) { // a BPM a notch, onto whole ones; with Shift, a tenth of one
        const double step = mods.shift ? 0.1 : 1.0;
        if (whole != 0) {
            const double from = controller_.tempo();
            const double on = whole > 0 ? std::floor(from / step + 1e-6) : std::ceil(from / step - 1e-6);
            const double bpm = clampTempo((on + whole) * step);
            if (bpm != from)
                controller_.setTempo(bpm);
        }
        return;
    }
    if (hit && hit->kind == Kind::ShapeGrid) {
        if (whole != 0)
            stepShapeGrid(hit->a, whole);
        return;
    }
    if (hit && hit->kind == Kind::SplitBand) { // a split's tabs: down a tab to the right, up one to the left
        if (steps != 0.0f) {
            const bool three = splitThree(hit->a);
            const int count = three ? 3 : 2;
            const int at = tabPosition(controller_.layout().splitBand[hit->a] == 2 && !three
                                           ? 0
                                           : controller_.layout().splitBand[hit->a],
                                       three);
            const int to = std::clamp(at + (steps < 0.0f ? 1 : -1), 0, count - 1);
            for (int tab = 0; tab < count; ++tab)
                if (tabPosition(tab, three) == to)
                    showSplitBand(hit->a, tab);
        }
        return;
    }
    if (hit && hit->kind == Kind::SplitBands) { // the BANDS switch: down for three, up for two
        if (steps != 0.0f)
            setSplitBands(hit->a, steps < 0.0f ? 3 : 2);
        return;
    }
    if ((!hit || hit->param < 0) && modPanel(controller_.layout().sourcesCollapsed).contains(p) && steps != 0.0f) {
        setScroll(ScrollArea::Sources, sourceScroll() - steps * 0.4f);
        closeGoneLists(); // an LFO's list scrolled out with its card
        return;
    }
    // Over the MODS view's rows (but not on a bar), a notch scrolls them a row, gliding there.
    if ((!hit || hit->param < 0) && modsWide() && kModsList.contains(p) && steps != 0.0f) {
        scrollMods(std::clamp(modsScroll_, 0.0f, modsEnd()) - steps, true);
        return;
    }
    if ((!hit || hit->param < 0) && fxWide() && kFxPanel.contains(p) && steps != 0.0f && fxGraph()) {
        if (fxGraphArea().contains(p) || (hit && hit->kind == Kind::Scrollbar)) {
            // The graph sideways, half a column a notch, closing a +'s list.
            const FxGridLayout grid = fxGridLayout(fxLine());
            fxGraphScroll_ = std::clamp(fxGraphScroll(grid) - steps * 0.5f * kFxGridCol, 0.0f, fxGraphScrollMost(grid));
            if (fxSlot_.lane >= 0)
                fxMenuOpen_ = false;
        }
        return;
    }
    if ((!hit || hit->param < 0) && fxWide() && kFxPanel.contains(p) && steps != 0.0f) {
        setScroll(ScrollArea::Fx, fxScroll() - steps * 0.4f);
        closeGoneLists();
        return;
    }
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
    // the Fixed and symmetric switches bring along, and a count to the next one offered.
    if (hit->kind == Kind::Toggle) {
        if (whole != 0 && (whole > 0) != (value(hit->param) >= 0.5))
            flipSwitch(hit->param);
        return;
    }
    if (isLfoClockChoice(*hit)) { // to the next mode along, as the wheel turns, from the one lit
        const int to = std::clamp(static_cast<int>(lfoClockOf(hit->a)) + whole, 0, kLfoModes - 1);
        if (whole != 0)
            setLfoClock(hit->a, static_cast<LfoClock>(to));
        return;
    }
    if (const int split = splitOf(hit->param); split >= 0 && hit->param == paramIndex(split, SplitParam::Slope)) {
        const int slopes[] = {0, 1, kFilterSlopes - 1}; // 6, 12 and 24 dB, a notch a step (three bands from 12)
        const int first = splitThree(split) ? 1 : 0;
        int at = first;
        for (int k = first; k < 3; ++k)
            at = filterSlope(value(hit->param)) >= slopes[k] ? k : at;
        if (whole != 0)
            edit(hit->param, slopes[std::clamp(at + whole, first, 2)]);
        return;
    }
    if (hit->kind == Kind::LabelMode) { // to the next mode along, as the wheel turns, a run of notches one undo step
        const int to = std::clamp(static_cast<int>(routeMode(value(hit->param))) + whole, 0,
                                  static_cast<int>(RouteMode::Count) - 1);
        if (whole != 0 && to != static_cast<int>(routeMode(value(hit->param)))) {
            const UndoScope notches(controller_, true);
            edit(hit->param, to);
        }
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
    const double v = value(hit->param);
    if (def.kind == ParamKind::VoiceCount) { // about 20 % per notch, at least one voice
        const double scaled = std::round(v * std::exp2(whole * 0.25));
        edit(hit->param, whole > 0 ? std::max(v + 1.0, scaled) : std::min(v - 1.0, scaled));
        return;
    }
    if (def.kind == ParamKind::Transpose || def.kind == ParamKind::NotePitch) { // to the next whole semitone, and on
        edit(hit->param, whole > 0 ? std::floor(v + whole) : std::ceil(v + whole));
        return;
    }
    if (isStepped(def.kind))
        edit(hit->param, v + whole);
    else
        edit(hit->param, v + steps * (def.max - def.min) * (mods.shift ? 0.002 : 0.01));
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
constexpr int kResetBendCommand = 14;
constexpr int kOttCommand = 15;
// And from here on, each sets an amp's oversampling (see ampOversampling()).
constexpr int kAmpOversamplingCommand = 30;
// An effect on a split's band moved to another band of it: this plus the band's lane (1 high, 2 low, 3 middle).
constexpr int kMoveBandCommand = 40;
// And from here on, each sets the mode of the route the menu was opened on (see RouteMode).
constexpr int kRouteModeCommand = 16;
static_assert(kRouteModeCommand + static_cast<int>(RouteMode::Count) <= kAmpOversamplingCommand &&
              kAmpOversamplingCommand + kAmpOversamplings <= kMoveBandCommand && kMoveBandCommand + 3 < 100);

std::vector<MenuItem> View::contextMenu(Point p) {
    endTyping(true);
    std::vector<MenuItem> items;
    menuLive_ = true;
    menuParam_ = -1;
    menuFx_ = -1;
    menuSource_ = -1;
    menuPoint_ = {-1, -1};
    menuValue_.reset();
    const auto hit = hitTest(p);
    if (!hit)
        return items;
    if ((hit->kind == Kind::FxNode && hit->a >= kFxSlots) || hit->kind == Kind::FxBoxGrip) { // a split or a join
        menuFx_ = hit->a;
        items.push_back({7, tr(Str::MenuRemove)});
        return items;
    }
    if (hit->kind == Kind::FxGrip || hit->kind == Kind::FxPower || hit->kind == Kind::FxNode ||
        hit->kind == Kind::FxNodePower) {
        menuFx_ = hit->a;
        if (fxChain().size() < static_cast<size_t>(kFxSlots))
            items.push_back({6, tr(Str::MenuDuplicate)});
        items.push_back({8, tr(Str::MenuResetSettings)});
        if (fxSlotType(menuFx_) == FxType::MultiDyn)
            items.push_back({kOttCommand, tr(Str::MenuOtt)});
        if (fxSlotType(menuFx_) == FxType::Amp) { // how many times the host rate its stages run at, the one in force checked
            items.push_back({0, ""});
            const long now = std::lround(value(paramIndex(menuFx_, FxParam::AmpOversampling)));
            for (int i = 0; i < kAmpOversamplings; ++i)
                items.push_back({kAmpOversamplingCommand + i, trf(Str::MenuOversampling, {std::to_string(1 << i) + "x"}),
                                 now == i});
        }
        // On a split's band, in the row: the split's other bands it can move to.
        if (!fxGraph()) {
            const std::vector<int> bands = fxOtherBands(menuFx_);
            if (!bands.empty())
                items.push_back({0, ""});
            for (int lane : bands)
                items.push_back({kMoveBandCommand + lane,
                                 trf(Str::MenuMoveToBand, {tr(lane == 1   ? Str::BandHigh
                                                              : lane == 3 ? Str::BandMid
                                                                          : Str::BandLow)})});
        }
        items.push_back({0, ""});
        items.push_back({7, tr(Str::MenuRemove)});
        return items;
    }
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
        const bool bipolar = sourceBipolar(polaritySource);
        items.push_back({kUnipolarCommand, tr(Str::MenuUnipolar), !bipolar});
        items.push_back({kBipolarCommand, tr(Str::MenuBipolar), bipolar});
    };
    if (hit->kind == Kind::SourceGrip || hit->kind == Kind::SourceHandle) {
        if (!usedSlots(hit->a).empty())
            items.push_back({kClearCommand + hit->a, trf(Str::MenuClearTargets, {sourceName(hit->a)})});
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
            items.push_back({kClearCommand + polaritySource, trf(Str::MenuClearTargets, {sourceName(polaritySource)})});
            items.push_back({0, ""});
        }
        addPolarity();
        return items;
    }
    if (hit->kind == Kind::SlotBar || hit->kind == Kind::SlotRemove || hit->kind == Kind::ModDot ||
        hit->kind == Kind::ValueText || hit->kind == Kind::SlotBypass || hit->kind == Kind::ModsSource ||
        hit->kind == Kind::ModsTarget || hit->kind == Kind::ModsScale || hit->kind == Kind::ModsCurve ||
        hit->kind == Kind::ModsAux || hit->kind == Kind::ModsAuxClear || hit->kind == Kind::ModListRow) {
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
        if (const int aux = slotAux([this](int index) { return value(index); }, hit->a, hit->b); aux >= 0)
            items.push_back({kAuxClearCommand + hit->a * kSlots + hit->b, trf(Str::MenuRemoveAux, {sourceName(aux)})});
        items.push_back({0, ""});
        items.push_back({100 + hit->a * kSlots + hit->b, trf(Str::MenuRemoveModulation, {sourceName(hit->a)})});
        return items;
    }
    if (hit->kind == Kind::ShapePoint) {
        const bool end = isCurve(hit->a) && hit->b + 1 == shapePointCount(hit->a);
        if (hit->b > 0 && !end && shapePointCount(hit->a) > 2) {
            menuPoint_ = {hit->a, hit->b};
            items.push_back({5, tr(Str::MenuDeletePoint)});
        }
        return items;
    }
    if (hit->kind == Kind::ShapeCurve) {
        menuParam_ = hit->param;
        items.push_back({1, tr(Str::MenuStraighten)});
        return items;
    }
    if (hit->kind == Kind::Node || hit->kind == Kind::Port) { // the node, or its edge
        menuParam_ = enabledParam(hit->a);
        items.push_back({4, trf(Str::MenuSwitchOff, {nodeTitle(hit->a)})});
        return items;
    }
    // A route into an operator lists its modes, the one in force checked.
    auto addRouteModes = [&](int route) {
        const int mode = routeModeOfRoute(route);
        if (mode < 0)
            return;
        const auto now = routeMode(value(mode));
        for (int m = 0; m < static_cast<int>(RouteMode::Count); ++m)
            items.push_back({kRouteModeCommand + m, routeModeName(static_cast<RouteMode>(m)), static_cast<int>(now) == m});
        items.push_back({0, ""});
    };
    if (hit->kind == Kind::LabelMode || hit->kind == Kind::LabelRemove) {
        menuParam_ = hit->kind == Kind::LabelMode ? routeParam(hit->a, hit->b) : hit->param;
        addRouteModes(menuParam_);
        items.push_back({3, tr(Str::MenuDisconnect)});
        return items;
    }
    if (hit->kind == Kind::WireEnd || hit->kind == Kind::Wire) {
        menuParam_ = hit->param;
        addRouteModes(hit->param);
        if (hit->kind == Kind::Wire && wireBend(hit->param) != kDefaultWireBend)
            items.push_back({kResetBendCommand, tr(Str::MenuResetBend)});
        items.push_back({3, tr(Str::MenuDisconnect)});
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
    if ((hit->kind == Kind::WireLabel || hit->kind == Kind::GridCell) && wired(hit->param) &&
        routeModeOfRoute(hit->param) >= 0) {
        items.push_back({0, ""});
        addRouteModes(hit->param);
        items.pop_back(); // Disconnect follows the modes without a line between
    }
    if ((hit->kind == Kind::WireLabel || hit->kind == Kind::OutLabel || hit->kind == Kind::GridCell) &&
        wired(hit->param))
        items.push_back({3, tr(Str::MenuDisconnect)});
    std::vector<MenuItem> removals, bypasses;
    for (const Assignment &m : assignments()) {
        if (m.target != hit->param)
            continue;
        bypasses.push_back({kBypassCommand + m.source * kSlots + m.slot, bypassLabel(m.source, m.slot)});
        removals.push_back({100 + m.source * kSlots + m.slot, trf(Str::MenuRemoveModulation, {sourceName(m.source)})});
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
    return trf(slotBypassed(source, slot) ? Str::MenuResume : Str::MenuBypass, {sourceName(source)});
}

void View::menuCommand(int command) {
    if (!menuLive_) // what the menu was opened on may have changed or gone since
        return;
    if (command == 1 && menuParam_ >= 0) {
        edit(menuParam_, resetValue(menuParam_));
    } else if (command == 2 && menuParam_ >= 0) {
        const UndoScope step(controller_);
        for (const Assignment &m : assignments())
            if (m.target == menuParam_)
                removeSlot(m.source, m.slot);
    } else if (command == 5 && menuPoint_.first >= 0) {
        removeShapePoint(menuPoint_.first, menuPoint_.second);
    } else if (command == 7 && menuFx_ >= kFxSlots) {
        removeFxNode(menuFx_);
    } else if (menuFx_ >= kFxSlots) {
        // a split or a join offers nothing else
    } else if (command == 6 && menuFx_ >= 0) {
        addEffect(fxSlotType(menuFx_), menuFx_);
    } else if (command == 7 && menuFx_ >= 0) {
        removeEffect(menuFx_);
    } else if (command == 8 && menuFx_ >= 0) {
        // Every field back to its default, apart from which effect it is and whether it is on.
        std::vector<std::pair<int, double>> changes;
        for (int f = static_cast<int>(FxParam::Enabled) + 1; f < kFxParamCount; ++f) {
            const int index = paramIndex(menuFx_, static_cast<FxParam>(f));
            changes.push_back({index, table_[index].def});
        }
        for (int f = 0; f < kShapeParamCount; ++f)
            changes.push_back({clipCurveCountIndex(menuFx_) + f, table_[clipCurveCountIndex(menuFx_) + f].def});
        editMany(changes);
    } else if (command > kMoveBandCommand && command <= kMoveBandCommand + 3 && menuFx_ >= 0) {
        moveFxToBand(menuFx_, command - kMoveBandCommand);
    } else if (command == kOttCommand && menuFx_ >= 0 && fxSlotType(menuFx_) == FxType::MultiDyn) {
        editMany(mdOttSettings(menuFx_));
    } else if (command >= kAmpOversamplingCommand && command < kAmpOversamplingCommand + kAmpOversamplings &&
               menuFx_ >= 0 && fxSlotType(menuFx_) == FxType::Amp) {
        edit(paramIndex(menuFx_, FxParam::AmpOversampling), command - kAmpOversamplingCommand);
    } else if (command == 3 && menuParam_ >= 0) {
        disconnect(menuParam_);
    } else if (command == 4 && menuParam_ >= 0) {
        edit(menuParam_, 0.0); // switch a node off
    } else if (command == 9 && menuSource_ >= 0) {
        addSource(sourceTypeOf(menuSource_), menuSource_);
    } else if (command == 10 && menuSource_ >= 0) {
        removeSource(menuSource_);
    } else if (command == 11 && menuValue_) {
        startTyping(*menuValue_);
    } else if ((command == kUnipolarCommand || command == kBipolarCommand) && menuSource_ >= 0) {
        setSourcePolarity(menuSource_, command == kBipolarCommand);
    } else if (command >= kRouteModeCommand && command < kRouteModeCommand + static_cast<int>(RouteMode::Count) &&
               menuParam_ >= 0 && routeModeOfRoute(menuParam_) >= 0) {
        edit(routeModeOfRoute(menuParam_), command - kRouteModeCommand);
    } else if (command == kResetBendCommand && menuParam_ >= 0) {
        const UndoScope step(controller_);
        setWireBend(menuParam_, kDefaultWireBend);
    } else if (command >= 100 && command < kClearCommand) {
        removeSlot((command - 100) / kSlots, (command - 100) % kSlots);
    } else if (command >= kClearCommand && command < kClearCommand + kAllSources) {
        const UndoScope step(controller_);
        for (int slot : usedSlots(command - kClearCommand))
            removeSlot(command - kClearCommand, slot);
    } else if (command >= kBypassCommand && command < kBypassCommand + kAllSources * kSlots) {
        toggleBypass((command - kBypassCommand) / kSlots, (command - kBypassCommand) % kSlots);
    } else if (command >= kAuxClearCommand && command < kAuxClearCommand + kAllSources * kSlots) {
        setAux((command - kAuxClearCommand) / kSlots, (command - kAuxClearCommand) % kSlots, -1);
    }
}

void View::undo() {
    if (drag_.kind == DragKind::None && !typing_) {
        cascade_.reset(); // what its menu was opened on may change, or go
        listMenu_ = -1;
        peqMenu_ = -1;
        fxMenuOpen_ = false; // so is the place in the line the list of effects was opened at
        fxSlot_ = {};
        controller_.undo();
        menuLive_ = false;
    }
}

void View::redo() {
    if (drag_.kind == DragKind::None && !typing_) {
        cascade_.reset(); // what its menu was opened on may change, or go
        listMenu_ = -1;
        peqMenu_ = -1;
        fxMenuOpen_ = false; // so is the place in the line the list of effects was opened at
        fxSlot_ = {};
        controller_.redo();
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
        const std::string bpm = tempoText(controller_.tempo());
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
        switch (*f) {
        case Field::Browser:
            browserKey(key);
            break;
        case Field::ModsFilter:
            modsFilterKey(key);
            break;
        case Field::CascadeSearch:
            cascadeKey(key);
            break;
        }
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
    case Key::Home:
        t.caret = 0;
        break;
    case Key::End:
        t.caret = t.text.size();
        break;
    case Key::Up:
    case Key::Down:
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
        if (!commit || t.text.find_first_not_of(' ') == std::string::npos || t.text == tempoText(controller_.tempo()))
            return;
        if (end == text || !std::isfinite(bpm) || !(rest.empty() || rest == "bpm")) {
            flash(trf(Str::MsgBadValue, {t.text, tr(Str::TempoName)}));
            return;
        }
        if (clampTempo(bpm) != controller_.tempo())
            controller_.setTempo(clampTempo(bpm));
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
        flash(trf(Str::MsgBadValue, {text, paramName(t.widget.param)}));
}

CursorKind View::cursor() const {
    if (drag_.kind == DragKind::Pan)
        return CursorKind::Move;
    if (drag_.kind == DragKind::Connect || drag_.kind == DragKind::Reroute || drag_.kind == DragKind::ModSource ||
        drag_.kind == DragKind::RerouteFrom)
        return CursorKind::Hand;
    if (drag_.kind == DragKind::FxNodeMove && drag_.moved)
        return CursorKind::Move;
    if (drag_.kind == DragKind::Value &&
        (drag_.widget.kind == Kind::PeqPoint || drag_.widget.kind == Kind::DynGrip)) // Alt-dragged for Q or ratio
        return CursorKind::Vertical;
    const Widget *w = drag_.kind != DragKind::None ? &drag_.widget : (hover_ ? &*hover_ : nullptr);
    if (!w)
        return CursorKind::Arrow;
    switch (w->kind) {
    case Kind::DynZone:
    case Kind::Knob:
    case Kind::WireLabel:
    case Kind::OutLabel:
    case Kind::GridCell:
    case Kind::ModDot:
    case Kind::ShapeCurve:
    case Kind::EnvCurve:
    case Kind::TempoBox:
        return CursorKind::Vertical;
    case Kind::SplitPoint:
        return CursorKind::Horizontal;
    case Kind::ShapeArea:
    case Kind::SettingsBackdrop:
    case Kind::ConfirmBackdrop:
    case Kind::BrowserBackdrop:
    // An open list's own panel and what is around it, where a click picks nothing (though around it, it closes it),
    // and a pill's list's rows, which only take a source's handle dropped on them.
    case Kind::WaveMenu:
    case Kind::SourceMenu:
    case Kind::FxMenu:
    case Kind::ListMenu:
    case Kind::PeqTypeMenu:
    case Kind::CascadeMenu:
    case Kind::ModList:
    case Kind::ModListRow:
        return CursorKind::Arrow;
    case Kind::ListItem: // a band-pass's 6 and 18 dB slopes can't be picked
        return listItemFits(w->param, w->b) ? CursorKind::Hand : CursorKind::Arrow;
    case Kind::CascadeItem: { // a category opens; an entry that can't be picked does nothing
        const std::shared_ptr<const MenuNode> tree = cascade_ ? cascadeTree() : nullptr;
        if (!tree)
            return CursorKind::Arrow;
        const std::vector<const MenuNode *> levels = cascadeLevels(*tree);
        if (w->a < 0 || w->a >= static_cast<int>(levels.size()) || w->b < 0 ||
            w->b >= static_cast<int>(levels[w->a]->children.size()))
            return CursorKind::Arrow;
        const MenuNode &item = levels[w->a]->children[w->b];
        return !item.children.empty() || (!item.heading && item.enabled) ? CursorKind::Hand : CursorKind::Arrow;
    }
    case Kind::SearchField: // the MODS view's filter a button until it opens
        return w->a == static_cast<int>(Field::ModsFilter) && !modsFilterOpen() ? CursorKind::Hand
                                                                                       : CursorKind::Text;
    case Kind::SlotBar:
    case Kind::ModsScale:
    case Kind::DynGrip:
        return CursorKind::Horizontal;
    case Kind::ModsCurve:
        return CursorKind::Vertical;
    case Kind::Node:
    case Kind::EnvPoint:
    case Kind::FilterPoint:
    case Kind::PeqPoint:
    case Kind::AmpMic:
    case Kind::UtilityHandle:
    case Kind::FxGrip:
    case Kind::FxBoxGrip:
    case Kind::SourceGrip:
    case Kind::ShapePoint:
    case Kind::Wire:
    case Kind::FxNode:
        return CursorKind::Move;
    case Kind::FxWire: // only its + takes a click
        return CursorKind::Arrow;
    default:
        return CursorKind::Hand;
    }
}

} // namespace fmvibe::gui
