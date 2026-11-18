// A Wavetable or Sample operator's part of the inspector (InspectorPanel, see inspectorpanel.h): the source switch on
// its page's title row, a sample's picker in its waves' place, its TABLE tab (the table's picker over its frames,
// Position's control, with its warp) and its SAMPLE tab (its waveform with Start's and the loop's handles, the ROOT readout, LOOP, MODE and the
// reverse switch), and a file dragged in from outside dropped on that display. The table browser the pickers open is
// TableBrowser's (see tablebrowser.h).

#include "inspectorpanel.h"

#include "icons.h"
#include "layout.h"
#include "tablebrowser.h"
#include "theme.h"
#include "../tables.h"
#include "../uistrings.h"
#include "../warp.h"

#include <algorithm>
#include <cmath>

namespace fmvibe::gui {

using namespace detail;

namespace {

// Where a Sample operator's waveform goes in its display: under the ROOT readout's line.
Rect sampleArea(Rect display) { return {display.x + 8.0f, display.y + 22.0f, display.w - 16.0f, display.h - 30.0f}; }

// The TABLE tab's meter along its display's right edge, 10 px in from its foot (Position 0) to its top (1): where a
// value is on it, and the value a height on the display stands for, which a press there sets.
Point meterAt(Rect display, double v) {
    const float foot = display.bottom() - 10.0f, top = display.y + 10.0f;
    return {display.right() - 14.0f, foot + (top - foot) * static_cast<float>(std::clamp(v, 0.0, 1.0))};
}
double meterValue(Rect display, float y) {
    const float foot = display.bottom() - 10.0f, top = display.y + 10.0f;
    return std::clamp(static_cast<double>((foot - y) / (foot - top)), 0.0, 1.0);
}

// The source switch's names, and its tooltips.
constexpr Str kSourceNames[] = {Str::SourceWave, Str::SourceTable, Str::SourceSample};
constexpr Str kSourceTips[] = {Str::TipSourceWave, Str::TipSourceTable, Str::TipSourceSample};

int sourceOfWave(OpWave wave) { return wave == OpWave::Wavetable ? 1 : wave == OpWave::Sample ? 2 : 0; }

} // namespace

bool isTableDisplay(const Widget &w) {
    return w.kind == WidgetKind::Knob && w.param >= kOpBase && w.param < kOpBase + kOps * kOpParamCount &&
           (w.param - kOpBase) % kOpParamCount == static_cast<int>(OpParam::TablePos);
}

bool InspectorPanel::isTableOp(int op) const {
    return opWave(value(paramIndex(op, OpParam::Wave))) == OpWave::Wavetable;
}

bool InspectorPanel::isSampleOp(int op) const {
    return opWave(value(paramIndex(op, OpParam::Wave))) == OpWave::Sample;
}

OpTabs InspectorPanel::opTabsOf(int op) const {
    return isNoise(op)       ? OpTabs::Noise
           : isTableOp(op)  ? OpTabs::Table
           : isSampleOp(op) ? OpTabs::Sample
                            : OpTabs::Plain;
}

std::string InspectorPanel::dataName(int op) const {
    if (isTableOp(op)) {
        const uint32_t key = dataKey(value(paramIndex(op, OpParam::Table)));
        if (key < kFactoryTables)
            return factoryTableName(static_cast<int>(key));
        const OpData *d = controller().opData(key);
        return d ? d->name : std::string(tr(Str::TableMissing));
    }
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Sample)));
    const OpData *d = key ? controller().opData(key) : nullptr;
    return d ? d->name : std::string(tr(Str::SampleNone));
}

int InspectorPanel::fileDropOp(Point p) const {
    const int op = controller().layout().selectedNode;
    if (covered() || shapes_.partialEditing() >= 0 || op < 0 || op >= kOps || !isSampleOp(op) ||
        opTabShown(op) != InspectorTab::Source || !kOpDisplay.contains(p))
        return -1;
    return op;
}

// --- Widgets.

void InspectorPanel::sourceWidgets(std::vector<Widget> &list, int op, InspectorTab tab) const {
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) -> Widget & {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
        return list.back();
    };
    const size_t switches = list.size();
    for (int s = 0; s < 3; ++s)
        add(Kind::SourceSwitch, opSourceButton(s), -1, op, s);
    mergeButtons(list, switches);
    const bool table = isTableOp(op), sample = isSampleOp(op);
    if (sample) {
        add(Kind::SamplePicker, kSamplePicker, paramIndex(op, OpParam::Sample), op);
        add(Kind::DataImport, kSampleImport, -1, op, static_cast<int>(DataKind::Sample));
    }
    if (tab != InspectorTab::Source)
        return;
    if (table) {
        add(Kind::TableStep, tableStepButton(-1), -1, op, -1);
        add(Kind::TablePicker, kTablePicker, paramIndex(op, OpParam::Table), op);
        add(Kind::TableStep, tableStepButton(1), -1, op, 1);
        // Position is its display (see isTableDisplay()), dragged and modulated as a knob is.
        add(Kind::Knob, kTableDisplay, paramIndex(op, OpParam::TablePos), op).label = tr(Str::LabelPosition);
        // Its warp, as the WARP tab of a plain wave has it: the warps' buttons, the amount and note sync.
        const size_t warps = list.size();
        for (int m = 0; m < static_cast<int>(OpWarp::Count); ++m)
            add(Kind::Choice, warpButton(m), paramIndex(op, OpParam::Warp), op, m);
        mergeButtons(list, warps);
        add(Kind::Knob, knobRect(kWarpAmountKnob, kOpKnobD), paramIndex(op, OpParam::WarpAmount)).label =
            tr(Str::LabelAmount);
        add(Kind::Toggle, kSyncButton, paramIndex(op, OpParam::Sync));
        return;
    }
    if (!sample)
        return;
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Sample)));
    const OpData *d = key ? controller().opData(key) : nullptr;
    if (!d || d->kind != DataKind::Sample) {
        add(Kind::DataImport, kOpDisplay.inset(10.0f), -1, op, static_cast<int>(DataKind::Sample));
    } else {
        // The loop's brackets over its upper part, Start's tab along its foot, and the fade's ramp at the loop's end.
        const Rect area = sampleArea(kOpDisplay);
        auto x = [&](OpParam p) {
            return area.x + static_cast<float>(std::clamp(value(paramIndex(op, p)), 0.0, 1.0)) * area.w;
        };
        const bool loop = sampleLoop(value(paramIndex(op, OpParam::LoopMode))) != SampleLoop::Off;
        const float lo = std::min(x(OpParam::LoopStart), x(OpParam::LoopEnd)),
                    hi = std::max(x(OpParam::LoopStart), x(OpParam::LoopEnd));
        add(Kind::SampleHandle, {x(OpParam::LoopStart) - 6.0f, area.y, 12.0f, area.h * 0.6f},
            paramIndex(op, OpParam::LoopStart), op, 1);
        add(Kind::SampleHandle, {x(OpParam::LoopEnd) - 6.0f, area.y, 12.0f, area.h * 0.6f},
            paramIndex(op, OpParam::LoopEnd), op, 2);
        if (loop) {
            const float fade = static_cast<float>(value(paramIndex(op, OpParam::LoopFade))) * 0.5f * (hi - lo);
            add(Kind::SampleHandle, {hi - fade - 6.0f, area.y - 2.0f, 12.0f, 12.0f}, paramIndex(op, OpParam::LoopFade),
                op, 3);
        }
        add(Kind::SampleHandle, {x(OpParam::SampleStart) - 6.0f, area.bottom() - 14.0f, 12.0f, 16.0f},
            paramIndex(op, OpParam::SampleStart), op, 0);
        add(Kind::SampleRoot, sampleRootBox(kOpDisplay), paramIndex(op, OpParam::SampleRoot), op);
    }
    const size_t loops = list.size();
    for (int m = 0; m < 3; ++m)
        add(Kind::Choice, sampleLoopButton(m, kOpControlsY), paramIndex(op, OpParam::LoopMode), op, m);
    mergeButtons(list, loops);
    const size_t modes = list.size();
    for (int m = 0; m < 3; ++m)
        add(Kind::Choice, sampleModeButton(m, kOpControlsY), paramIndex(op, OpParam::SampleMode), op, m);
    mergeButtons(list, modes);
    add(Kind::Toggle, sampleReverseButton(kOpControlsY), paramIndex(op, OpParam::SampleReverse), op);
}

// --- Painting.

bool InspectorPanel::paintSourceWidget(Canvas &c, const Widget &w, bool hovered) {
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::SourceSwitch: {
        const bool lit = sourceOfWave(opWave(value(paramIndex(w.a, OpParam::Wave)))) == w.b;
        host_.paintButtonFace(c, w, lit, hovered, kAccent, 0.18f, kRadiusControl);
        c.text(w.rect, tr(kSourceNames[std::clamp(w.b, 0, 2)]), kTextSmall,
               lit ? litInk(kAccent) : (hovered ? kText : kDim), Align::Center, true);
        return true;
    }
    case Kind::TableStep:
        host_.paintButtonFace(c, w, false, hovered, kAccent, 0.18f, kRadiusControl);
        chevron(c, w.rect.center(), w.b, hovered ? kText : kDim);
        return true;
    case Kind::TablePicker:
    case Kind::SamplePicker: {
        const bool open = tables_.browsing() == w.a;
        host_.paintButtonFace(c, w, open, hovered, kAccent, 0.18f, kRadiusControl);
        const Color ink = open ? litInk(kAccent) : hovered ? kText : kDim;
        const Rect glyph{w.rect.x + 5.0f, w.rect.y + 4.0f, 14.0f, w.rect.h - 8.0f};
        if (w.kind == Kind::TablePicker)
            tableGlyph(c, glyph, ink);
        else
            sampleGlyph(c, glyph, ink);
        const Rect name{glyph.right() + 5.0f, w.rect.y, w.rect.right() - glyph.right() - 20.0f, w.rect.h};
        c.pushClip(name);
        c.text(name, dataName(w.a), kTextLabel, open ? litInk(kAccent) : kText, Align::Left);
        c.popClip();
        chevron(c, {w.rect.right() - 9.0f, w.rect.center().y}, 0, ink);
        return true;
    }
    case Kind::DataImport: {
        if (w.rect.w > 40.0f) { // a Sample operator with none: a dashed card that opens the dialog
            const Color ring = hovered ? kAccent : kFaint;
            c.strokeRect(w.rect, ring, 1.0f, kRadiusCard, true);
            sampleGlyph(c, {w.rect.center().x - 10.0f, w.rect.center().y - 22.0f, 20.0f, 16.0f}, ring);
            c.text({w.rect.x, w.rect.center().y - 2.0f, w.rect.w, 18.0f}, tr(Str::SampleImportCard), kTextBody,
                   hovered ? kText : kDim);
            return true;
        }
        host_.paintButtonFace(c, w, false, hovered, kAccent, 0.18f, kRadiusControl);
        const Point m = w.rect.center(); // an arrow down into a tray
        const Color ink = hovered ? kText : kDim;
        c.line({m.x, m.y - 5.0f}, {m.x, m.y + 2.0f}, ink, 1.4f);
        chevron(c, {m.x, m.y + 1.0f}, 0, ink);
        const Point tray[] = {
            {m.x - 5.0f, m.y + 2.0f}, {m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y + 2.0f}};
        c.polyline(tray, 4, ink, 1.2f);
        return true;
    }
    case Kind::Knob: // Position's: the display painted with the tab (see paintTableTab()), outlined under the mouse
        if (!isTableDisplay(w))
            return false;
        if (hovered || active)
            c.strokeRect(w.rect, kFaint, 1.0f, kRadiusCard);
        return true;
    case Kind::SampleHandle: {
        const Rect area = sampleArea(kOpDisplay);
        const float x = w.rect.center().x;
        const bool hot = hovered || active;
        const bool loop = sampleLoop(value(paramIndex(w.a, OpParam::LoopMode))) != SampleLoop::Off;
        if (w.b == 0) { // Start: a line down the display, its tab at the foot
            c.line({x, area.y}, {x, area.bottom()}, kFxColor, hot ? 2.0f : 1.5f);
            c.fillRect({x - 4.0f, area.bottom() - 9.0f, 8.0f, 9.0f}, kFxColor, kRadiusMark);
        } else if (w.b == 3) { // the fade's grip
            c.fillCircle(w.rect.center(), hot ? 4.5f : 3.5f, kAccent);
        } else { // a loop's bracket, its foot turned in toward the loop
            const Color color = loop ? kAccent : kFaint;
            const float in = (w.b == 1) == (value(paramIndex(w.a, OpParam::LoopStart)) <=
                                             value(paramIndex(w.a, OpParam::LoopEnd)))
                                 ? 5.0f
                                 : -5.0f;
            const Point bracket[] = {{x + in, area.y}, {x, area.y}, {x, area.bottom()}, {x + in, area.bottom()}};
            c.polyline(bracket, 4, hot ? kText : color, hot ? 2.0f : 1.5f);
        }
        return true;
    }
    case Kind::SampleRoot: {
        char note[32];
        formatParam(w.param, value(w.param), note, sizeof(note));
        const bool hot = hovered || active;
        c.text(w.rect, std::string(tr(Str::LabelRoot)) + " " + note, kTextSmall, hot ? kText : kDim, Align::Right,
               true);
        return true;
    }
    default:
        return false;
    }
}

void InspectorPanel::paintTableTab(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Table)));
    const OpData *d = controller().opData(key);
    if (!d || d->kind != DataKind::Table) {
        c.text(r, tr(Str::TableMissing), kTextBody, kDim);
        return;
    }
    // What plays: the newest voice's Position, or with no note the knob's with its modulation as the editor has it.
    const Telemetry &t = host_.telemetry();
    const int pos = paramIndex(op, OpParam::TablePos);
    const float knob = static_cast<float>(value(pos));
    const float played = t.voices > 0 && t.opTablePos[op] >= 0.0f ? t.opTablePos[op]
                                                                  : std::clamp(knob + t.modOffset[pos], 0.0f, 1.0f);
    const Rect stack{r.x + 4.0f, r.y + 6.0f, r.w - 30.0f, r.h - 10.0f};
    paintTableStack(c, stack, *d, 32, played, kAccent, opWarp(op), value(paramIndex(op, OpParam::WarpAmount)));
    // The meter along the right (see meterAt()): Position's value ringed, each modulation's reach a band in its
    // source's color as a knob's rings draw it (faint while its source is off or it is bypassed, wider while its depth
    // handle is under the mouse or dragged), the frame played a diamond.
    auto along = [&](double v) { return meterAt(r, v); };
    c.line(along(0.0), along(1.0), kBorder, 2.0f);
    const double n = mods_.norm(pos);
    auto band = [&](const Assignment &m, float width, float alpha) {
        const auto [down, up] = modReach(m);
        c.line(along(n + down), along(n + up), ringColor(kSourceColors[m.source], kAccent).alpha(alpha), width);
    };
    const std::optional<Widget> held = host_.dragged();
    const std::optional<Widget> &over = held ? held : host_.hovered();
    auto focused = [&](const Assignment &m) {
        return over && (over->kind == Kind::ModDot || (over->popup && over->kind != Kind::ModList)) &&
               over->a == m.source && over->b == m.slot;
    };
    std::vector<Assignment> focus;
    for (const Assignment &m : assignments()) {
        if (m.target != pos)
            continue;
        if (focused(m))
            focus.push_back(m);
        else
            band(m, 3.0f, (m.bipolar ? 0.6f : 0.9f) * (mods_.sourceOn(m.source) && !m.bypassed ? 1.0f : 0.3f));
    }
    for (const Assignment &f : focus)
        band(f, 5.0f, mods_.sourceOn(f.source) && !f.bypassed ? 1.0f : 0.4f);
    c.strokeCircle(along(knob), 4.0f, kDim, 1.2f);
    const Point d0 = along(played);
    const Point diamond[] = {{d0.x, d0.y - 5.0f}, {d0.x + 4.0f, d0.y}, {d0.x, d0.y + 5.0f}, {d0.x - 4.0f, d0.y}};
    c.fillPolygon(diamond, 4, kAccent);
    char text[32];
    formatParam(pos, played, text, sizeof(text));
    c.text({r.x + 8.0f, r.y + 4.0f, 120.0f, 14.0f}, text, kTextSmall, kDim, Align::Left);
}

void InspectorPanel::paintSampleTab(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Sample)));
    const OpData *d = key ? controller().opData(key) : nullptr;
    if (!d || d->kind != DataKind::Sample || d->peakMin.empty())
        return; // the dashed card says what to do
    const Rect area = sampleArea(r);
    // The loop's stretch, faintly; its fade a ramp up to its end.
    const bool loop = sampleLoop(value(paramIndex(op, OpParam::LoopMode))) != SampleLoop::Off;
    auto x = [&](OpParam p) {
        return area.x + static_cast<float>(std::clamp(value(paramIndex(op, p)), 0.0, 1.0)) * area.w;
    };
    const float lo = std::min(x(OpParam::LoopStart), x(OpParam::LoopEnd)),
                hi = std::max(x(OpParam::LoopStart), x(OpParam::LoopEnd));
    if (loop) {
        c.fillRect({lo, area.y, hi - lo, area.h}, kAccent.alpha(0.08f));
        const float fade = static_cast<float>(value(paramIndex(op, OpParam::LoopFade))) * 0.5f * (hi - lo);
        if (fade > 0.5f) {
            const Point ramp[] = {{hi - fade, area.bottom()}, {hi, area.y}, {hi, area.bottom()}};
            c.fillPolygon(ramp, 3, kAccent.alpha(0.15f));
        }
    }
    // The waveform: each pixel column's lowest and highest, from its peaks. Reversed, back to front: as it plays,
    // Start, the loop and the playhead's shares along it as they are.
    const bool reversed = value(paramIndex(op, OpParam::SampleReverse)) >= 0.5;
    const std::vector<double> waveKey = {static_cast<double>(Curve::SampleWave), static_cast<double>(d->key), area.x,
                                         area.y, area.w, area.h, reversed ? 1.0 : 0.0};
    const std::vector<Point> outline = curveCache().get(waveKey, [&] {
        const int columns = std::max(static_cast<int>(area.w), 2);
        std::vector<Point> top, bottom;
        for (int i = 0; i < columns; ++i) {
            const int from = i * OpData::kPeakColumns / columns;
            const int to = std::max(from + 1, (i + 1) * OpData::kPeakColumns / columns);
            float low = 0.0f, high = 0.0f;
            for (int k = from; k < to && k < OpData::kPeakColumns; ++k) {
                low = std::min(low, d->peakMin[k]);
                high = std::max(high, d->peakMax[k]);
            }
            const float px = area.x + ((reversed ? columns - 1 - i : i) + 0.5f) * area.w / columns;
            top.push_back({px, area.center().y - high * area.h * 0.48f});
            bottom.push_back({px, area.center().y - low * area.h * 0.48f - 0.5f});
        }
        top.insert(top.end(), bottom.rbegin(), bottom.rend());
        return top;
    });
    c.fillPolygon(outline.data(), outline.size(), kAccent.alpha(0.55f));
    c.line({area.x, area.center().y}, {area.right(), area.center().y}, kBorder, 1.0f);
    // Where the newest voice's first copy plays.
    const Telemetry &t = host_.telemetry();
    if (t.voices > 0 && t.opSamplePos[op] >= 0.0f) {
        const float px = area.x + t.opSamplePos[op] * area.w;
        c.line({px, area.y - 2.0f}, {px, area.bottom() + 2.0f}, kText, 1.5f);
    }
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 80.0f, 14.0f}, d->name, kTextSmall, kDim, Align::Left);
    // The loop's and the mode's names, under their buttons.
    c.text({sampleLoopButton(0, kOpControlsY).x, kOpControlsY + 11.0f, 3.0f * kSampleChoiceW, 12.0f},
           tr(Str::LabelLoop), kTextSmall, kFaint, Align::Center, true);
    c.text({sampleModeButton(0, kOpControlsY).x, kOpControlsY + 11.0f, 3.0f * kSampleChoiceW, 12.0f},
           tr(Str::LabelMode), kTextSmall, kFaint, Align::Center, true);
}

std::string InspectorPanel::sourceTip(const Widget &w) const {
    switch (w.kind) {
    case Kind::SourceSwitch:
        return tr(kSourceTips[std::clamp(w.b, 0, 2)]);
    case Kind::TableStep:
        return tr(w.b < 0 ? Str::TipTablePrev : Str::TipTableNext);
    case Kind::TablePicker:
        return tr(Str::TipTablePicker);
    case Kind::SamplePicker:
        return tr(Str::TipSamplePicker);
    case Kind::DataImport:
        return w.rect.w > 40.0f ? std::string()
                                : tr(w.b == static_cast<int>(DataKind::Table) ? Str::TipImportTable
                                                                               : Str::TipImportSample);
    case Kind::Knob:
        return isTableDisplay(w) ? std::string(tr(Str::TipTableDisplay)) : std::string();
    case Kind::SampleHandle: {
        const Str tips[] = {Str::TipSampleStart, Str::TipLoopStart, Str::TipLoopEnd, Str::TipLoopFade};
        return tr(tips[std::clamp(w.b, 0, 3)]);
    }
    case Kind::SampleRoot:
        return tr(Str::TipSampleRoot);
    case Kind::Choice: // LOOP's and MODE's buttons are icons, named here
        if (opOfField(w.param, OpParam::LoopMode) >= 0)
            return sampleLoopName(sampleLoop(w.b));
        if (opOfField(w.param, OpParam::SampleMode) >= 0)
            return sampleModeName(sampleMode(w.b));
        return {};
    case Kind::Toggle:
        return opOfField(w.param, OpParam::SampleReverse) >= 0 ? std::string(tr(Str::TipSampleReverse))
                                                               : std::string();
    default:
        return {};
    }
}

std::optional<CursorKind> InspectorPanel::sourceCursor(const Widget &w) const {
    switch (w.kind) {
    case Kind::SampleRoot:
        return CursorKind::Vertical;
    case Kind::SampleHandle:
        return CursorKind::Horizontal;
    case Kind::SourceSwitch:
    case Kind::TableStep:
    case Kind::TablePicker:
    case Kind::SamplePicker:
    case Kind::DataImport:
        return CursorKind::Hand;
    default:
        return std::nullopt;
    }
}

// --- Input.

void InspectorPanel::switchSource(int op, int source) {
    const int wave = paramIndex(op, OpParam::Wave);
    const OpWave now = opWave(value(wave));
    if (sourceOfWave(now) == source)
        return;
    const UndoScope step(controller());
    UiLayout &layout = controller().layout();
    if (!isDataWave(now) && layout.plainWave[op] != static_cast<int>(now)) {
        layout.plainWave[op] = static_cast<int>(now);
        controller().layoutChanged();
    }
    if (source == 0) {
        edit(wave, std::clamp(layout.plainWave[op], 0, static_cast<int>(OpWave::Vowel)));
        return;
    }
    edit(wave, static_cast<double>(source == 1 ? OpWave::Wavetable : OpWave::Sample));
    if (layout.opTab != InspectorTab::Source) { // picking either opens its tab
        layout.opTab = InspectorTab::Source;
        controller().layoutChanged();
    }
}

void InspectorPanel::stepTable(int op, int dir) {
    const int param = paramIndex(op, OpParam::Table);
    const uint32_t key = dataKey(value(param));
    if (key < kFactoryTables) { // the factory tables round
        controller().pickTable(op, (key + kFactoryTables + (dir > 0 ? 1 : kFactoryTables - 1)) % kFactoryTables);
        return;
    }
    const OpData *d = controller().opData(key);
    TableLibrary *lib = controller().tableLibrary();
    if (d && !d->origin.empty() && lib) { // the next file in its folder that reads as a table
        lib->poll();
        for (size_t s = 0; s < lib->sources().size(); ++s) {
            const LibrarySource &src = lib->sources()[s];
            if (!d->origin.starts_with(src.folder.name + "/"))
                continue;
            const std::string path = d->origin.substr(src.folder.name.size() + 1);
            const auto at = std::find_if(src.entries.begin(), src.entries.end(),
                                         [&](const LibraryEntry &e) { return e.path == path; });
            if (at == src.entries.end())
                continue;
            const int count = static_cast<int>(src.entries.size());
            int next = static_cast<int>(at - src.entries.begin());
            for (int tries = 0; tries < count; ++tries) {
                next = (next + dir + count) % count;
                const LibraryEntry &e = src.entries[next];
                if (e.info.error != ReadError::None || e.info.sample)
                    continue;
                std::vector<uint8_t> bytes;
                if (!lib->readEntry(static_cast<int>(s), next, bytes))
                    continue;
                const size_t slash = e.path.rfind('/');
                const std::string said = controller().importOperatorData(
                    op, DataKind::Table, bytes, slash == std::string::npos ? e.path : e.path.substr(slash + 1),
                    src.folder.name + "/" + e.path, src.folder.frameSize);
                if (!said.empty())
                    host_.flash(said);
                return;
            }
        }
    }
    // Else the patch's own tables round.
    std::vector<uint32_t> tables;
    const DataStore &store = controller().data();
    for (uint32_t k : store.keys())
        if (const OpData *t = k >= kUserDataBase ? store.find(k) : nullptr; t && t->kind == DataKind::Table)
            tables.push_back(k);
    const auto at = std::find(tables.begin(), tables.end(), key);
    if (tables.size() > 1 && at != tables.end()) {
        const int n = static_cast<int>(tables.size());
        controller().pickTable(op, tables[(static_cast<int>(at - tables.begin()) + dir + n) % n]);
    }
}

bool InspectorPanel::sourcePress(const Widget &w, Point p, Modifiers mods) {
    switch (w.kind) {
    case Kind::SourceSwitch:
        switchSource(w.a, w.b);
        return true;
    case Kind::TableStep:
        stepTable(w.a, w.b);
        return true;
    case Kind::TablePicker:
        tables_.open(w.a, DataKind::Table);
        return true;
    case Kind::SamplePicker:
        tables_.open(w.a, DataKind::Sample);
        return true;
    case Kind::DataImport:
        controller().chooseOperatorFile(w.a, static_cast<DataKind>(w.b));
        return true;
    case Kind::Knob: // Position's display: a press sets Position where the mouse is, on the meter's scale, and a drag
                     // goes on from there, one undo step; with Shift it stays where it is, for a fine drag
        if (!isTableDisplay(w))
            return false;
        controller().beginEdit(w.param);
        if (const double v = meterValue(kTableDisplay, p.y); !mods.shift && v != value(w.param))
            controller().setValue(w.param, v);
        drag_ = {DragKind::TablePosition, w, static_cast<float>(value(w.param)), mods.shift, p.y};
        host_.startDrag(*this, w, p, false);
        return true;
    case Kind::SampleRoot: // up and down, as a knob's value
        host_.dragValue(w, p, mods);
        return true;
    case Kind::SampleHandle:
        controller().beginEdit(w.param);
        drag_ = {DragKind::SampleHandle, w, static_cast<float>(value(w.param)), mods.shift};
        host_.startDrag(*this, w, p, false);
        return true;
    default:
        return false;
    }
}

// Across the display, where the mouse is (finely with Shift, from where it was); the fade how far before the loop's
// end its ramp starts, over half the loop; with Alt, a loop's point onto the nearest zero crossing going up.
void InspectorPanel::sampleDrag(const DragState &d) {
    const Widget &w = drag_.widget;
    const Rect area = sampleArea(kOpDisplay);
    double v = std::clamp((d.now.x - area.x) / area.w, 0.0f, 1.0f);
    if (drag_.fine)
        v = std::clamp(drag_.startValue + (d.now.x - d.start.x) / area.w * 0.1, 0.0, 1.0);
    if (w.b == 3) {
        const double a = value(paramIndex(w.a, OpParam::LoopStart)), b = value(paramIndex(w.a, OpParam::LoopEnd));
        const double half = 0.5 * std::abs(b - a);
        v = half > 0.0 ? std::clamp((std::max(a, b) - v) / half, 0.0, 1.0) : 0.0;
    } else if (d.mods.alt && (w.b == 1 || w.b == 2)) {
        const uint32_t key = dataKey(value(paramIndex(w.a, OpParam::Sample)));
        if (const OpData *data = controller().opData(key); data && data->frames > 2) {
            const auto at = static_cast<int64_t>(v * (data->frames - 1));
            const int64_t reach = std::max<int64_t>(data->frames / 200, 64);
            int64_t best = -1;
            const int64_t last = std::min<int64_t>(at + reach, data->frames - 1);
            for (int64_t k = std::max<int64_t>(at - reach, 1); k <= last; ++k)
                if (data->pcm[k - 1] <= 0.0f && data->pcm[k] > 0.0f &&
                    (best < 0 || std::abs(k - at) < std::abs(best - at)))
                    best = k;
            if (best >= 0)
                v = static_cast<double>(best) / (data->frames - 1);
        }
    }
    if (v != value(w.param))
        controller().setValue(w.param, v);
}

// Up and down along the meter's scale, a tenth as far as the mouse with Shift: Shift pressed or let go of part-way goes
// on from where Position is, so it doesn't jump.
void InspectorPanel::tablePosDrag(const DragState &d) {
    const int param = drag_.widget.param;
    if (d.mods.shift != drag_.fine) {
        drag_.fine = d.mods.shift;
        drag_.startValue = static_cast<float>(value(param));
        drag_.startY = d.now.y;
    }
    const double travel = (drag_.startY - d.now.y) / (kTableDisplay.h - 20.0f) * (drag_.fine ? 0.1 : 1.0);
    if (const double v = std::clamp(drag_.startValue + travel, 0.0, 1.0); v != value(param))
        controller().setValue(param, v);
}

bool InspectorPanel::sourceDoubleClick(const Widget &w) {
    if (w.kind == Kind::SampleHandle) { // back to where it starts
        edit(w.param, table_[w.param].def);
        return true;
    }
    return false; // the ROOT readout typed into, as the View types a value (see View::doubleClick())
}

} // namespace fmvibe::gui
