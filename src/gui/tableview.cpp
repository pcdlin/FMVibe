// The Wavetable and Sample operators' part of the editor: the source switch over an operator's waves and the pickers
// under it, its TABLE and SAMPLE tabs, and the table browser over the routing panel (see View::tableBrowsing()).

#include "view.h"

#include "icons.h"
#include "layout.h"
#include "theme.h"
#include "warp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

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

// The table browser's list's scrollbar, along its right edge, and its thumb's least height (as the editor's other
// scrollbars have one), so a list of thousands of files still has a thumb to grab.
constexpr Rect kTbScrollTrack{kTbList.x + kTbList.w - 5.0f, kTbList.y, 4.0f, kTbList.h};
constexpr float kTbThumbMin = 24.0f;

constexpr int kTbRenameCommand = 60000;
constexpr int kTbFrameSizeCommand = 60001; // + 0 Auto, 1 256, 2 512, 3 1024, 4 2048
constexpr int kTbRescanCommand = 60010;
constexpr int kTbShowCommand = 60011;
constexpr int kTbRemoveCommand = 60012;
constexpr int kTbFrameSizes[] = {0, 256, 512, 1024, 2048};

// The source switch's names, and its tooltips.
constexpr Str kSourceNames[] = {Str::SourceWave, Str::SourceTable, Str::SourceSample};
constexpr Str kSourceTips[] = {Str::TipSourceWave, Str::TipSourceTable, Str::TipSourceSample};

int sourceOfWave(OpWave wave) { return wave == OpWave::Wavetable ? 1 : wave == OpWave::Sample ? 2 : 0; }

// A chevron pointing left (-1), right (1) or down (0), about `c`.
void chevron(Canvas &c, Point at, int dir, Color color) {
    if (dir == 0) {
        const Point p[] = {{at.x - 3.5f, at.y - 1.5f}, {at.x, at.y + 2.0f}, {at.x + 3.5f, at.y - 1.5f}};
        c.polyline(p, 3, color, 1.4f);
    } else {
        const float d = static_cast<float>(dir);
        const Point p[] = {{at.x - 1.5f * d, at.y - 4.0f}, {at.x + 2.0f * d, at.y}, {at.x - 1.5f * d, at.y + 4.0f}};
        c.polyline(p, 3, color, 1.4f);
    }
}

// A table's glyph: three waves, one behind another.
void tableGlyph(Canvas &c, Rect r, Color color) {
    for (int k = 2; k >= 0; --k) {
        Point p[9];
        for (int i = 0; i < 9; ++i) {
            const float t = i / 8.0f;
            p[i] = {r.x + k * 2.0f + t * (r.w - 4.0f),
                    r.y + r.h - 3.0f - k * 2.5f - std::sin(t * 2.0f * std::numbers::pi_v<float>) * 2.5f};
        }
        c.polyline(p, 9, k == 0 ? color : color.alpha(0.45f), 1.2f);
    }
}

// A sample's glyph: a burst that dies away.
void sampleGlyph(Canvas &c, Rect r, Color color) {
    const float mid = r.center().y;
    for (int i = 0; i < 7; ++i) {
        const float x = r.x + 1.0f + i * (r.w - 2.0f) / 6.0f;
        const float h = (0.5f * r.h - 1.0f) * (1.0f - i / 8.0f) * (i % 2 == 0 ? 1.0f : 0.6f);
        c.line({x, mid - h}, {x, mid + h}, color, 1.4f);
    }
}

} // namespace

bool View::isTableOp(int op) const {
    return opWave(value(paramIndex(op, OpParam::Wave))) == OpWave::Wavetable;
}

bool View::isSampleOp(int op) const {
    return opWave(value(paramIndex(op, OpParam::Wave))) == OpWave::Sample;
}

int View::opTabsOf(int op) const {
    const OpTabs tabs = isNoise(op) ? OpTabs::Noise : isTableOp(op) ? OpTabs::Table : isSampleOp(op) ? OpTabs::Sample : OpTabs::Plain;
    return static_cast<int>(tabs);
}

std::string View::dataName(int op) const {
    if (isTableOp(op)) {
        const uint32_t key = dataKey(value(paramIndex(op, OpParam::Table)));
        if (key < kFactoryTables)
            return factoryTableName(static_cast<int>(key));
        const OpData *d = controller_.opData(key);
        return d ? d->name : std::string(tr(Str::TableMissing));
    }
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Sample)));
    const OpData *d = key ? controller_.opData(key) : nullptr;
    return d ? d->name : std::string(tr(Str::SampleNone));
}

void View::opDataWidgets(std::vector<Widget> &list, int op, InspectorTab tab) const {
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
    if (table) {
        add(Kind::TableStep, tableStepButton(-1), -1, op, -1);
        add(Kind::TablePicker, kTablePicker, paramIndex(op, OpParam::Table), op);
        add(Kind::TableStep, tableStepButton(1), -1, op, 1);
    } else if (sample) {
        add(Kind::SamplePicker, kSamplePicker, paramIndex(op, OpParam::Sample), op);
        add(Kind::DataImport, kSampleImport, -1, op, static_cast<int>(DataKind::Sample));
    }
    if (tab != InspectorTab::Source)
        return;
    if (table) {
        // Position is its display (see isTableDisplay()), dragged and modulated as a knob is.
        add(Kind::Knob, kOpDisplay, paramIndex(op, OpParam::TablePos), op).label = tr(Str::LabelPosition);
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
    const OpData *d = key ? controller_.opData(key) : nullptr;
    if (!d || d->kind != DataKind::Sample) {
        add(Kind::DataImport, kOpDisplay.inset(10.0f), -1, op, static_cast<int>(DataKind::Sample));
    } else {
        // The loop's brackets over its upper part, Start's tab along its foot, and the fade's ramp at the loop's end.
        const Rect area = sampleArea(kOpDisplay);
        auto x = [&](OpParam p) { return area.x + static_cast<float>(std::clamp(value(paramIndex(op, p)), 0.0, 1.0)) * area.w; };
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

bool View::paintDataWidget(Canvas &c, const Widget &w, bool hovered, bool active) {
    switch (w.kind) {
    case Kind::SourceSwitch: {
        const bool lit = sourceOfWave(opWave(value(paramIndex(w.a, OpParam::Wave)))) == w.b;
        paintButtonFace(c, w, lit, hovered, kAccent, 0.18f, kRadiusControl);
        c.text(w.rect, tr(kSourceNames[std::clamp(w.b, 0, 2)]), kTextSmall,
               lit ? litInk(kAccent) : (hovered ? kText : kDim), Align::Center, true);
        return true;
    }
    case Kind::TableStep:
        paintButtonFace(c, w, false, hovered, kAccent, 0.18f, kRadiusControl);
        chevron(c, w.rect.center(), w.b, hovered ? kText : kDim);
        return true;
    case Kind::TablePicker:
    case Kind::SamplePicker: {
        const bool open = tableBrowsing() == w.a;
        paintButtonFace(c, w, open, hovered, kAccent, 0.18f, kRadiusControl);
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
        paintButtonFace(c, w, false, hovered, kAccent, 0.18f, kRadiusControl);
        const Point m = w.rect.center(); // an arrow down into a tray
        const Color ink = hovered ? kText : kDim;
        c.line({m.x, m.y - 5.0f}, {m.x, m.y + 2.0f}, ink, 1.4f);
        chevron(c, {m.x, m.y + 1.0f}, 0, ink);
        const Point tray[] = {{m.x - 5.0f, m.y + 2.0f}, {m.x - 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y + 5.0f}, {m.x + 5.0f, m.y + 2.0f}};
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
            const Color color = look.fx;
            c.line({x, area.y}, {x, area.bottom()}, color, hot ? 2.0f : 1.5f);
            c.fillRect({x - 4.0f, area.bottom() - 9.0f, 8.0f, 9.0f}, color, kRadiusMark);
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
        c.text(w.rect, std::string(tr(Str::LabelRoot)) + " " + note, kTextSmall, hot ? kText : kDim, Align::Right, true);
        return true;
    }
    case Kind::TbSource: {
        TableLibrary *lib = library();
        const bool selected = tbSource_ == w.a && tablesSearch_.text.empty();
        if (selected || hovered)
            c.fillRect(w.rect, selected ? kAccent.alpha(0.18f) : kTrack, kRadiusControl);
        std::string name, count;
        bool missing = false, scanning = false;
        if (w.a == -2) {
            name = tr(Str::TablesFactory);
            count = std::to_string(kFactoryTables);
        } else if (w.a == -1) {
            name = tr(Str::TablesInPatch);
            int n = 0;
            for (uint32_t key : controller_.data().keys())
                n += key >= kUserDataBase ? 1 : 0;
            count = std::to_string(n);
        } else if (lib && w.a < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[w.a];
            name = s.folder.name;
            missing = s.missing;
            scanning = s.scanning;
            count = std::to_string(s.entries.size());
        }
        const Color ink = missing ? kFaint : selected ? kText : kDim;
        c.pushClip({w.rect.x + 8.0f, w.rect.y, w.rect.w - 48.0f, w.rect.h});
        c.text({w.rect.x + 8.0f, w.rect.y, w.rect.w - 48.0f, w.rect.h}, name, kTextBody, ink, Align::Left, selected);
        c.popClip();
        c.text({w.rect.right() - 44.0f, w.rect.y, 38.0f, w.rect.h}, scanning ? "\xE2\x80\xA6" : count, kTextSmall,
               kFaint, Align::Right);
        return true;
    }
    case Kind::TbAddFolder:
        c.text({w.rect.x + 8.0f, w.rect.y, w.rect.w - 8.0f, w.rect.h}, tr(Str::TablesAddFolder), kTextBody,
               hovered ? kText : kAccent, Align::Left);
        return true;
    case Kind::TbGroup: {
        const std::vector<TbRow> rows = tbRows();
        if (w.a < 0 || w.a >= static_cast<int>(rows.size()))
            return true;
        const TbRow &row = rows[w.a];
        const std::string id = std::to_string(row.source) + "|" + row.group;
        const bool folded = std::find(tbFolded_.begin(), tbFolded_.end(), id) != tbFolded_.end();
        chevron(c, {w.rect.x + 8.0f, w.rect.center().y}, folded ? 1 : 0, hovered ? kText : kFaint);
        c.text({w.rect.x + 18.0f, w.rect.y, w.rect.w - 18.0f, w.rect.h}, row.name, kTextLabel, hovered ? kText : kFaint,
               Align::Left, true);
        return true;
    }
    case Kind::TbRow: {
        const std::vector<TbRow> rows = tbRows();
        if (w.a < 0 || w.a >= static_cast<int>(rows.size()))
            return true;
        const TbRow &row = rows[w.a];
        const int op = tableBrowsing();
        // The one the operator plays, the one picked with the keys, and the one under the mouse.
        bool current = false;
        if (op >= 0 && row.source < 0) {
            const int p = paramIndex(op, tbKind_ == DataKind::Table ? OpParam::Table : OpParam::Sample);
            current = dataKey(value(p)) == row.key;
        } else if (op >= 0) {
            const int p = paramIndex(op, tbKind_ == DataKind::Table ? OpParam::Table : OpParam::Sample);
            if (const OpData *d = controller_.opData(dataKey(value(p))); d && !d->origin.empty() && library() &&
                                                                         row.source < static_cast<int>(library()->sources().size()))
                current = d->origin == library()->sources()[row.source].folder.name + "/" +
                                           library()->sources()[row.source].entries[row.entry].path;
        }
        const bool unreadable = row.error != ReadError::None ||
                                (tbKind_ == DataKind::Sample && row.source == -2) ||
                                (row.source == -1 && row.sample != (tbKind_ == DataKind::Sample));
        if (current || tbPick_ == w.a || hovered)
            c.fillRect(w.rect, current ? kAccent.alpha(0.2f) : tbPick_ == w.a ? kAccent.alpha(0.1f) : kTrack,
                       kRadiusControl);
        const Color ink = unreadable ? kFaint : current ? kText : kDim;
        // A thumbnail: the first, middle and last frames over one another, or a sample's glyph.
        const Rect thumb{w.rect.x + 6.0f, w.rect.y + 3.0f, 40.0f, w.rect.h - 6.0f};
        c.fillRect(thumb, kDisplayColor, kRadiusMark);
        std::shared_ptr<const OpData> data;
        if (row.source < 0)
            data = controller_.data().get(row.key);
        else
            for (const auto &[id, read] : tbRead_)
                if (id == std::to_string(row.source) + "|" + row.group + "/" + row.name)
                    data = read;
        if (data && data->kind == DataKind::Table && data->frames > 0) {
            const float height = thumb.h * 0.4f / tablePeak(*data);
            for (int k = 0; k < 3; ++k) {
                const int f = k * (data->frames - 1) / 2;
                const float *x = &data->pcm[static_cast<size_t>(f) * FrameTable::kFrameSize];
                Point p[33];
                for (int i = 0; i <= 32; ++i)
                    p[i] = {thumb.x + 2.0f + i * (thumb.w - 4.0f) / 32.0f,
                            thumb.center().y - x[i * (FrameTable::kFrameSize / 32) % FrameTable::kFrameSize] * height};
                c.polyline(p, 33, (unreadable ? kFaint : kAccent).alpha(k == 2 ? 1.0f : 0.4f), 1.0f);
            }
        } else if (row.sample || (data && data->kind == DataKind::Sample)) {
            sampleGlyph(c, thumb.inset(4.0f), unreadable ? kFaint : kAccent);
        } else {
            tableGlyph(c, thumb.inset(3.0f), unreadable ? kFaint : kDim);
        }
        const float tagW = row.tag.empty() ? 0.0f : 36.0f;
        const Rect name{thumb.right() + 8.0f, w.rect.y, w.rect.w - 46.0f - 8.0f - 72.0f - tagW, w.rect.h};
        c.pushClip(name);
        c.text(name, row.name, kTextBody, ink, Align::Left, current);
        c.popClip();
        std::string frames = row.sample || (data && data->kind == DataKind::Sample) ? std::string(tr(Str::TablesSampleRow))
                             : row.frames == 1 ? std::string(tr(Str::TablesOneFrame))
                                               : trf(Str::TablesFrames, {std::to_string(row.frames)});
        c.text({w.rect.right() - 72.0f - tagW, w.rect.y, 68.0f, w.rect.h}, frames, kTextSmall, kFaint, Align::Right);
        if (!row.tag.empty()) {
            const Rect chip{w.rect.right() - tagW + 2.0f, w.rect.y + 5.0f, tagW - 6.0f, w.rect.h - 10.0f};
            c.strokeRect(chip, kBorder, 1.0f, kRadiusMark);
            c.text(chip, row.tag, kTextSmall, kFaint);
        }
        return true;
    }
    case Kind::TbDone:
    case Kind::TbImport:
        paintButtonFace(c, w, w.kind == Kind::TbDone, hovered, kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, tr(w.kind == Kind::TbDone ? Str::TablesDone : Str::TablesImport), kTextLabel,
               w.kind == Kind::TbDone ? litInk(kText) : hovered ? kText : kDim, Align::Center, w.kind == Kind::TbDone);
        return true;
    case Kind::TbScroll:
        return true;
    case Kind::TbScrollBar: // as the MODS view's
        c.fillRect(kTbScrollTrack, kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(tbThumb(), hovered || active ? kDim : kFaint, kRadiusMark);
        return true;
    default:
        return false;
    }
}

// The frames as stored aren't scaled yet (a factory table's least of all): drawn at the gain the synth plays them at,
// the loudest frame's peak at full scale, none reaches past where it is drawn. Found once a table, in the curve cache.
float View::tablePeak(const OpData &table) {
    const size_t samples = static_cast<size_t>(std::max(table.frames, 0)) * FrameTable::kFrameSize;
    if (samples == 0 || table.pcm.size() < samples)
        return 1.0f;
    const std::vector<double> key = {static_cast<double>(Curve::TableFrame), static_cast<double>(table.key),
                                     static_cast<double>(table.frames), static_cast<double>(table.pcm[0]),
                                     static_cast<double>(table.pcm[samples / 2]), -1.0};
    const float peak = curves_.get(key, [&] {
        float most = 0.0f;
        for (size_t i = 0; i < samples; ++i)
            most = std::max(most, std::abs(table.pcm[i]));
        return std::vector<Point>{{most, 0.0f}};
    })[0].x;
    return peak > 0.0f ? peak : 1.0f;
}

void View::paintTableStack(Canvas &c, Rect r, const OpData &table, int most, float position, Color color, OpWarp warp,
                           double amount) {
    const int frames = table.frames;
    if (frames <= 0 || table.pcm.size() < static_cast<size_t>(frames) * FrameTable::kFrameSize)
        return;
    // The front frame at the bottom left, the last at the top right, as if seen from a little above and to the side.
    const float depthX = r.w * 0.22f, depthY = r.h * 0.34f;
    const float width = r.w - depthX - 12.0f, amp = (r.h - depthY - 14.0f) * 0.45f;
    const float baseX = r.x + 6.0f, baseY = r.bottom() - 7.0f - amp;
    constexpr int kPoints = 96;
    const float scale = amp / tablePeak(table);
    // A frame read through coefs: the frames behind plainly, the one played as the operator's warp reads it.
    auto frameAt = [&](double f, float t, Color ink, float thickness, const WarpCoefs &coefs) {
        // By the table's key and frames (a file previewed, under the key 0, apart from Basic Shapes by its frames' count).
        const std::vector<double> key = {static_cast<double>(Curve::TableFrame), static_cast<double>(table.key),
                                         static_cast<double>(frames), static_cast<double>(table.pcm[0]),
                                         static_cast<double>(table.pcm[table.pcm.size() / 2]), f, r.x, r.y, r.w, r.h,
                                         static_cast<double>(coefs.mode), coefs.a, coefs.b, coefs.c, scale};
        const std::vector<Point> points = curves_.get(key, [&] {
            std::vector<Point> p(kPoints + 1);
            const int a = std::clamp(static_cast<int>(f), 0, frames - 1), b = std::min(a + 1, frames - 1);
            const float blend = static_cast<float>(f - a);
            const float *x = &table.pcm[static_cast<size_t>(a) * FrameTable::kFrameSize];
            const float *y = &table.pcm[static_cast<size_t>(b) * FrameTable::kFrameSize];
            constexpr int kSize = FrameTable::kFrameSize;
            for (int i = 0; i <= kPoints; ++i) {
                double at = coefs.mode == OpWarp::Off ? static_cast<double>(i) / kPoints
                                                      : warpPhase(coefs, static_cast<double>(i) / kPoints);
                at = (at - std::floor(at)) * kSize;
                const int s = std::min(static_cast<int>(at), kSize - 1), s1 = (s + 1) % kSize;
                const float frac = static_cast<float>(at - s);
                const float xa = x[s] + (x[s1] - x[s]) * frac, ya = y[s] + (y[s1] - y[s]) * frac;
                const float v = xa * (1.0f - blend) + ya * blend;
                p[i] = {static_cast<float>(i) / kPoints * width, -v * scale};
            }
            return p;
        });
        std::vector<Point> moved(points.size());
        const float dx = baseX + t * depthX, dy = baseY - t * depthY;
        for (size_t i = 0; i < points.size(); ++i)
            moved[i] = {points[i].x + dx, points[i].y + dy};
        c.polyline(moved.data(), moved.size(), ink, thickness);
    };
    c.pushClip(r.inset(-2.0f)); // and whatever a frame does, kept to where it is drawn
    const int shown = std::max(1, std::min(most, frames));
    for (int j = shown - 1; j >= 0; --j) { // the back first, fainter
        const double f = shown > 1 ? std::round(static_cast<double>(j) * (frames - 1) / (shown - 1)) : 0.0;
        const float t = frames > 1 ? static_cast<float>(f / (frames - 1)) : 0.0f;
        frameAt(f, t, color.alpha(0.12f + 0.38f * (1.0f - t)), 1.0f, WarpCoefs{});
    }
    if (position >= 0.0f) { // the frame played, lit at its depth
        const double f = std::clamp(position, 0.0f, 1.0f) * (frames - 1);
        frameAt(f, frames > 1 ? static_cast<float>(f / (frames - 1)) : 0.0f, color, 2.0f, warpCoefs(warp, amount));
    }
    c.popClip();
}

void View::paintTableTab(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Table)));
    const OpData *d = controller_.opData(key);
    if (!d || d->kind != DataKind::Table) {
        c.text(r, tr(Str::TableMissing), kTextBody, kDim);
        return;
    }
    // What plays: the newest voice's Position, or with no note the knob's with its modulation as the editor has it.
    const int pos = paramIndex(op, OpParam::TablePos);
    const float knob = static_cast<float>(value(pos));
    const float played = telemetry_.voices > 0 && telemetry_.opTablePos[op] >= 0.0f
                             ? telemetry_.opTablePos[op]
                             : std::clamp(knob + telemetry_.modOffset[pos], 0.0f, 1.0f);
    const Rect stack{r.x + 4.0f, r.y + 6.0f, r.w - 30.0f, r.h - 10.0f};
    paintTableStack(c, stack, *d, 32, played, kAccent, opWarp(op), value(paramIndex(op, OpParam::WarpAmount)));
    // The meter along the right (see meterAt()): Position's value ringed, each modulation's reach a band in its
    // source's color as a knob's rings draw it (faint while its source is off or it is bypassed, wider while its depth
    // handle is under the mouse), the frame played a diamond.
    auto along = [&](double v) { return meterAt(r, v); };
    c.line(along(0.0), along(1.0), kBorder, 2.0f);
    const double n = norm(pos);
    auto band = [&](const Assignment &m, float width, float alpha) {
        const auto [down, up] = modReach(m);
        c.line(along(n + down), along(n + up), ringColor(kSourceColors[m.source], kAccent).alpha(alpha), width);
    };
    std::vector<Assignment> focus;
    for (const Assignment &m : assignments()) {
        if (m.target != pos)
            continue;
        if (modFocused(m))
            focus.push_back(m);
        else
            band(m, 3.0f, (m.bipolar ? 0.6f : 0.9f) * (sourceOn(m.source) && !m.bypassed ? 1.0f : 0.3f));
    }
    for (const Assignment &f : focus)
        band(f, 5.0f, sourceOn(f.source) && !f.bypassed ? 1.0f : 0.4f);
    c.strokeCircle(along(knob), 4.0f, kDim, 1.2f);
    const Point d0 = along(played);
    const Point diamond[] = {{d0.x, d0.y - 5.0f}, {d0.x + 4.0f, d0.y}, {d0.x, d0.y + 5.0f}, {d0.x - 4.0f, d0.y}};
    c.fillPolygon(diamond, 4, kAccent);
    char text[32];
    formatParam(pos, played, text, sizeof(text));
    c.text({r.x + 8.0f, r.y + 4.0f, 120.0f, 14.0f}, text, kTextSmall, kDim, Align::Left);
}

void View::paintSampleTab(Canvas &c, Rect r, int op) {
    c.fillRect(r, kDisplayColor, kRadiusCard);
    const uint32_t key = dataKey(value(paramIndex(op, OpParam::Sample)));
    const OpData *d = key ? controller_.opData(key) : nullptr;
    if (!d || d->kind != DataKind::Sample || d->peakMin.empty())
        return; // the dashed card says what to do
    const Rect area = sampleArea(r);
    // The loop's stretch, faintly; its fade a ramp up to its end.
    const bool loop = sampleLoop(value(paramIndex(op, OpParam::LoopMode))) != SampleLoop::Off;
    auto x = [&](OpParam p) { return area.x + static_cast<float>(std::clamp(value(paramIndex(op, p)), 0.0, 1.0)) * area.w; };
    const float lo = std::min(x(OpParam::LoopStart), x(OpParam::LoopEnd)), hi = std::max(x(OpParam::LoopStart), x(OpParam::LoopEnd));
    if (loop) {
        c.fillRect({lo, area.y, hi - lo, area.h}, kAccent.alpha(0.08f));
        const float fade = static_cast<float>(value(paramIndex(op, OpParam::LoopFade))) * 0.5f * (hi - lo);
        if (fade > 0.5f) {
            const Point ramp[] = {{hi - fade, area.bottom()}, {hi, area.y}, {hi, area.bottom()}};
            c.fillPolygon(ramp, 3, kAccent.alpha(0.15f));
        }
    }
    // The waveform: each pixel column's lowest and highest, from its peaks.
    // Reversed, back to front: as it plays, Start, the loop and the playhead's shares along it as they are.
    const bool reversed = value(paramIndex(op, OpParam::SampleReverse)) >= 0.5;
    const std::vector<double> key2 = {static_cast<double>(Curve::SampleWave), static_cast<double>(d->key), area.x,
                                      area.y, area.w, area.h, reversed ? 1.0 : 0.0};
    const std::vector<Point> outline = curves_.get(key2, [&] {
        const int columns = std::max(static_cast<int>(area.w), 2);
        std::vector<Point> top, bottom;
        for (int i = 0; i < columns; ++i) {
            const int from = i * OpData::kPeakColumns / columns;
            const int to = std::max(from + 1, (i + 1) * OpData::kPeakColumns / columns);
            float lo2 = 0.0f, hi2 = 0.0f;
            for (int k = from; k < to && k < OpData::kPeakColumns; ++k) {
                lo2 = std::min(lo2, d->peakMin[k]);
                hi2 = std::max(hi2, d->peakMax[k]);
            }
            const float px = area.x + ((reversed ? columns - 1 - i : i) + 0.5f) * area.w / columns;
            top.push_back({px, area.center().y - hi2 * area.h * 0.48f});
            bottom.push_back({px, area.center().y - lo2 * area.h * 0.48f - 0.5f});
        }
        top.insert(top.end(), bottom.rbegin(), bottom.rend());
        return top;
    });
    c.fillPolygon(outline.data(), outline.size(), kAccent.alpha(0.55f));
    c.line({area.x, area.center().y}, {area.right(), area.center().y}, kBorder, 1.0f);
    // Where the newest voice's first copy plays.
    if (telemetry_.voices > 0 && telemetry_.opSamplePos[op] >= 0.0f) {
        const float px = area.x + telemetry_.opSamplePos[op] * area.w;
        c.line({px, area.y - 2.0f}, {px, area.bottom() + 2.0f}, kText, 1.5f);
    }
    c.text({r.x + 8.0f, r.y + 4.0f, r.w - 80.0f, 14.0f}, d->name, kTextSmall, kDim, Align::Left);
    // The loop's and the mode's names, under their buttons.
    c.text({sampleLoopButton(0, kOpControlsY).x, kOpControlsY + 11.0f, 3.0f * kSampleChoiceW, 12.0f}, tr(Str::LabelLoop),
           kTextSmall, kFaint, Align::Center, true);
    c.text({sampleModeButton(0, kOpControlsY).x, kOpControlsY + 11.0f, 3.0f * kSampleChoiceW, 12.0f}, tr(Str::LabelMode),
           kTextSmall, kFaint, Align::Center, true);
}

// --- The table browser.

void View::openTableBrowser(int op, DataKind kind) {
    if (tableBrowser_ == op && tbKind_ == kind) { // a second click closes it
        closeTableBrowser();
        return;
    }
    tableBrowser_ = op;
    tbKind_ = kind;
    tbPick_ = -1;
    tbSearching_ = false; // the keys play notes until the search is clicked
    tbScroll_ = 0.0f;
    partialEditor_ = -1;
    shapeEditor_ = -1;
    clearField(Field::Tables);
    // The source the operator's data came from, else Factory for a table and the patch for a sample.
    tbSource_ = kind == DataKind::Table ? -2 : -1;
    const double playing = value(paramIndex(op, kind == DataKind::Table ? OpParam::Table : OpParam::Sample));
    if (const OpData *d = controller_.opData(dataKey(playing)); d) {
        if (d->key >= kUserDataBase)
            tbSource_ = -1;
        if (TableLibrary *lib = library(); lib && !d->origin.empty())
            for (size_t s = 0; s < lib->sources().size(); ++s)
                if (d->origin.starts_with(lib->sources()[s].folder.name + "/"))
                    tbSource_ = static_cast<int>(s);
    }
    if (TableLibrary *lib = library())
        lib->poll();
    hover_.reset();
}

void View::closeTableBrowser() {
    tableBrowser_ = -1;
    tbSearching_ = false;
    hover_.reset();
}

void View::showTableSource(int source) {
    if (source >= 0) {
        tbSource_ = source;
        tbScroll_ = 0.0f;
        tbPick_ = -1;
        clearField(Field::Tables);
    }
}

// --- Files dropped from outside the editor.

std::optional<View::FileDropTarget> View::fileDropTarget(Point p) const {
    if (dialogOpen() || inspectorCovered() || partialEditing() >= 0)
        return std::nullopt;
    const int op = controller_.layout().selectedNode;
    if (op < 0 || op >= kOps || !isSampleOp(op) || opTabShown(op) != InspectorTab::Source || !kOpDisplay.contains(p))
        return std::nullopt;
    return FileDropTarget{op, DataKind::Sample};
}

bool View::fileDragOver(std::optional<Point> p) {
    const std::optional<FileDropTarget> target = p ? fileDropTarget(*p) : std::nullopt;
    if (target == fileDrag_)
        return false;
    fileDrag_ = target;
    return true;
}

std::optional<std::string> View::fileDrop(Point p, const std::vector<uint8_t> &bytes, const std::string &name) {
    fileDrag_.reset();
    const std::optional<FileDropTarget> target = fileDropTarget(p);
    if (!target)
        return std::nullopt;
    endTyping(true);
    return controller_.importOperatorData(target->op, target->kind, bytes, name);
}

std::vector<View::TbRow> View::tbRows() const {
    std::vector<TbRow> rows;
    const std::vector<std::string> words = tablesSearch_.words();
    const bool searching = !words.empty();
    TableLibrary *lib = library();
    auto folded = [&](int source, const std::string &group) {
        return !searching &&
               std::find(tbFolded_.begin(), tbFolded_.end(), std::to_string(source) + "|" + group) != tbFolded_.end();
    };
    auto listSource = [&](int source, const std::string &heading) {
        std::vector<TbRow> found;
        if (source == -2) {
            for (uint32_t t = 0; t < kFactoryTables; ++t) {
                TbRow row;
                row.source = -2;
                row.key = t;
                row.name = factoryTableName(static_cast<int>(t));
                row.frames = 64;
                found.push_back(row);
            }
        } else if (source == -1) {
            for (uint32_t key : controller_.data().keys()) {
                const OpData *d = key >= kUserDataBase ? controller_.data().find(key) : nullptr;
                if (!d)
                    continue;
                TbRow row;
                row.source = -1;
                row.key = key;
                row.name = d->name;
                row.frames = d->frames;
                row.sample = d->kind == DataKind::Sample;
                row.tag = d->format;
                found.push_back(row);
            }
        } else if (lib && source < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[source];
            for (size_t e = 0; e < s.entries.size(); ++e) {
                const LibraryEntry &entry = s.entries[e];
                TbRow row;
                row.source = source;
                row.entry = static_cast<int>(e);
                row.name = entry.name;
                row.group = entry.group;
                row.frames = entry.info.frames;
                row.sample = entry.info.sample;
                row.guessed = entry.info.guessed;
                row.error = entry.info.error;
                row.tag = entry.info.format.empty() ? "" : entry.info.format + (entry.info.guessed ? "?" : "");
                found.push_back(row);
            }
        }
        std::string lastGroup = "\x01";
        bool headed = false;
        for (TbRow &row : found) {
            if (searching && !TextField::holds(heading + " " + row.group + " " + row.name, words))
                continue;
            if (searching && !headed) { // each source's matches under its name
                TbRow head;
                head.header = true;
                head.source = source;
                head.name = heading;
                head.group = "\x02";
                rows.push_back(head);
                headed = true;
            }
            if (!searching && row.group != lastGroup && !row.group.empty()) { // a subfolder's header
                TbRow head;
                head.header = true;
                head.source = source;
                head.group = row.group;
                head.name = row.group;
                rows.push_back(head);
            }
            lastGroup = row.group;
            if (!folded(source, row.group))
                rows.push_back(row);
        }
    };
    if (!searching) {
        listSource(tbSource_, "");
        return rows;
    }
    listSource(-2, tr(Str::TablesFactory));
    listSource(-1, tr(Str::TablesInPatch));
    for (size_t s = 0; lib && s < lib->sources().size(); ++s)
        listSource(static_cast<int>(s), lib->sources()[s].folder.name);
    return rows;
}

float View::tbEnd() const {
    return std::max(static_cast<float>(tbRows().size()) - kTbList.h / kTbRowH, 0.0f);
}

std::shared_ptr<const OpData> View::tbData(const TbRow &row) const {
    if (row.header)
        return nullptr;
    if (row.source < 0)
        return controller_.data().get(row.key);
    const std::string id = std::to_string(row.source) + "|" + row.group + "/" + row.name;
    for (const auto &[have, data] : tbRead_)
        if (have == id)
            return data;
    TableLibrary *lib = library();
    std::vector<uint8_t> bytes;
    if (!lib || row.error != ReadError::None || !lib->readEntry(row.source, row.entry, bytes))
        return nullptr;
    const LibrarySource &s = lib->sources()[row.source];
    const TableFile file = readTableFile(bytes.data(), bytes.size(), s.entries[row.entry].path,
                                         row.sample ? ReadAs::Sample : ReadAs::Table, s.folder.frameSize);
    std::shared_ptr<OpData> data;
    if (file.error == ReadError::None) {
        data = std::make_shared<OpData>();
        data->kind = file.sample ? DataKind::Sample : DataKind::Table;
        data->name = row.name;
        data->format = file.format;
        data->frames = file.frames;
        data->rate = file.rate;
        data->pcm = file.data;
        if (file.sample)
            buildPeaks(*data);
    }
    tbRead_.insert(tbRead_.begin(), {id, data});
    if (tbRead_.size() > 48)
        tbRead_.pop_back();
    return data;
}

void View::tableBrowserWidgets(std::vector<Widget> &list) const {
    auto add = [&list](Kind kind, Rect rect, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    add(Kind::TbDone, kTbDone);
    add(Kind::TbImport, kTbImport);
    add(Kind::SearchField, kTbSearch, static_cast<int>(Field::Tables));
    if (!tablesSearch_.text.empty())
        add(Kind::SearchClear, fieldClear(kTbSearch), static_cast<int>(Field::Tables));
    // The sources: Factory and the patch's own, then the library's folders, then + Add folder... after them.
    add(Kind::TbSource, tbSourceRow(0), -2);
    add(Kind::TbSource, tbSourceRow(1), -1);
    int row = 2;
    if (TableLibrary *lib = library()) {
        for (size_t s = 0; s < lib->sources().size() && tbSourceRow(row + 1).bottom() <= kTbSources.bottom(); ++s)
            add(Kind::TbSource, tbSourceRow(row++), static_cast<int>(s));
        add(Kind::TbAddFolder, tbSourceRow(row));
    }
    // The list, cut off at its ends as it scrolls.
    add(Kind::TbScroll, kTbList);
    const std::vector<TbRow> rows = tbRows();
    const float first = std::clamp(tbScroll_, 0.0f, tbEnd());
    for (int r = static_cast<int>(first); r < static_cast<int>(rows.size()); ++r) {
        const float at = static_cast<float>(r) - first;
        if (at * kTbRowH > kTbList.h)
            break;
        add(rows[r].header ? Kind::TbGroup : Kind::TbRow, tbListRow(at), r);
        list.back().clip = kTbList;
    }
    if (tbEnd() > 0.0f) // over the rows' right ends, a little wider than it is drawn
        add(Kind::TbScrollBar, kTbScrollTrack.inset(-3.0f));
}

Rect View::tbThumb() const {
    const float end = tbEnd(), shown = kTbList.h / kTbRowH;
    const float h = std::clamp(kTbScrollTrack.h * shown / (shown + end), std::min(kTbThumbMin, kTbScrollTrack.h),
                               kTbScrollTrack.h);
    const float at = end > 0.0f ? std::clamp(tbScroll_, 0.0f, end) / end : 0.0f;
    return {kTbScrollTrack.x, kTbScrollTrack.y + (kTbScrollTrack.h - h) * at, kTbScrollTrack.w, h};
}

void View::tbScrollDrag(Point p) {
    const float room = kTbScrollTrack.h - tbThumb().h;
    if (room > 0.0f)
        tbScroll_ = std::clamp(static_cast<float>(drag_.startValue) + (p.y - drag_.start.y) / room * tbEnd(), 0.0f,
                               tbEnd());
}

void View::paintTableBrowser(Canvas &c) {
    const Rect panel = kTableBrowser.inset(-6.0f);
    c.fillRect(panel, kPanel, kRadiusCard);
    c.text({kTableBrowser.x, kTableBrowser.y + 4.0f, 160.0f, 26.0f}, tr(Str::TablesTitle), kTextBody, kText,
           Align::Left, true);
    paintField(c, Field::Tables, kTbSearch, fieldTyped(kTbSearch), tr(Str::TablesSearchHint));
    c.line({kTbSources.right() + 6.0f, kTbTop}, {kTbSources.right() + 6.0f, kTbBottom}, kBorder, 1.0f);
    c.line({kTbList.right() + 6.0f, kTbTop}, {kTbList.right() + 6.0f, kTbBottom}, kBorder, 1.0f);
    c.text({kTableBrowser.x, kTbBottom + 4.0f, kTableBrowser.w, 18.0f}, tr(Str::TablesFooter), kTextLabel, kFaint,
           Align::Left);
    // A list with nothing in it says so.
    const std::vector<TbRow> rows = tbRows();
    TableLibrary *lib = library();
    if (rows.empty()) {
        std::string say = tr(Str::TablesEmpty);
        if (!tablesSearch_.text.empty())
            say = trf(Str::TablesNoMatch, {tablesSearch_.text});
        else if (lib && tbSource_ >= 0 && tbSource_ < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[tbSource_];
            say = s.missing ? trf(Str::TablesMissing, {s.folder.path}) : s.scanning ? tr(Str::TablesScanning) : say;
        }
        c.text({kTbList.x, kTbList.y + 20.0f, kTbList.w, 20.0f}, say, kTextBody, kDim);
    }
    // The preview: the row under the mouse, else the one picked with the keys, else what the operator plays.
    std::optional<TbRow> shown;
    if (hover_ && hover_->kind == Kind::TbRow && hover_->a >= 0 && hover_->a < static_cast<int>(rows.size()))
        shown = rows[hover_->a];
    else if (tbPick_ >= 0 && tbPick_ < static_cast<int>(rows.size()))
        shown = rows[tbPick_];
    const Rect view{kTbPreview.x, kTbPreview.y, kTbPreview.w, kTbPreview.h * 0.62f};
    c.fillRect(view, kDisplayColor, kRadiusCard);
    std::shared_ptr<const OpData> data;
    if (shown)
        data = tbData(*shown);
    else if (const int op = tableBrowsing(); op >= 0)
        data = controller_.data().get(
            dataKey(value(paramIndex(op, tbKind_ == DataKind::Table ? OpParam::Table : OpParam::Sample))));
    if (data && data->kind == DataKind::Table) {
        paintTableStack(c, view.inset(4.0f), *data, 24, -1.0f, kAccent);
    } else if (data && data->kind == DataKind::Sample && !data->peakMin.empty()) {
        const Rect a = view.inset(8.0f);
        std::vector<Point> top, bottom;
        for (int i = 0; i < 128; ++i) {
            const int k = i * OpData::kPeakColumns / 128;
            const float px = a.x + (i + 0.5f) * a.w / 128.0f;
            top.push_back({px, a.center().y - data->peakMax[k] * a.h * 0.48f});
            bottom.push_back({px, a.center().y - data->peakMin[k] * a.h * 0.48f});
        }
        top.insert(top.end(), bottom.rbegin(), bottom.rend());
        c.fillPolygon(top.data(), top.size(), kAccent.alpha(0.55f));
    } else {
        c.text(view, tr(Str::TablesNoPreview), kTextLabel, kFaint);
    }
    // What it is: its path, its format, its frame size (read or guessed) and frames.
    if (shown || data) {
        std::vector<std::string> lines;
        if (shown && shown->source >= 0 && lib && shown->source < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[shown->source];
            lines.push_back(s.folder.name + " \xE2\x80\xBA " +
                            (shown->group.empty() ? shown->name : shown->group + " \xE2\x80\xBA " + shown->name));
            if (shown->error != ReadError::None)
                lines.push_back(tr(readErrorText(shown->error)));
            const LibraryEntry &e = s.entries[shown->entry];
            if (!e.info.format.empty())
                lines.push_back(trf(Str::TablesFormat, {e.info.format}));
            if (e.info.frameSize > 0)
                lines.push_back(trf(e.info.guessed ? Str::TablesFrameSizeGuessed : Str::TablesFrameSize,
                                    {std::to_string(e.info.frameSize)}));
        } else if (data) {
            lines.push_back(data->origin.empty() ? data->name : data->origin);
            if (!data->format.empty())
                lines.push_back(trf(Str::TablesFormat, {data->format}));
        }
        if (data && data->kind == DataKind::Table)
            lines.push_back(data->frames == 1 ? std::string(tr(Str::TablesOneFrame))
                                              : trf(Str::TablesFrames, {std::to_string(data->frames)}));
        float y = view.bottom() + 8.0f;
        for (const std::string &line : lines) {
            c.pushClip({kTbPreview.x, y, kTbPreview.w, 16.0f});
            c.text({kTbPreview.x, y, kTbPreview.w, 16.0f}, line, kTextLabel, kDim, Align::Left);
            c.popClip();
            y += 17.0f;
        }
    }
}

// --- Input.

void View::switchSource(int op, int source) {
    const int wave = paramIndex(op, OpParam::Wave);
    const OpWave now = opWave(value(wave));
    if (sourceOfWave(now) == source)
        return;
    const UndoScope step(controller_);
    UiLayout &layout = controller_.layout();
    if (!isDataWave(now) && layout.plainWave[op] != static_cast<int>(now)) {
        layout.plainWave[op] = static_cast<int>(now);
        controller_.layoutChanged();
    }
    if (source == 0) {
        edit(wave, std::clamp(layout.plainWave[op], 0, static_cast<int>(OpWave::Vowel)));
        return;
    }
    edit(wave, static_cast<double>(source == 1 ? OpWave::Wavetable : OpWave::Sample));
    if (layout.opTab != InspectorTab::Source) { // picking either opens its tab
        layout.opTab = InspectorTab::Source;
        controller_.layoutChanged();
    }
}

void View::stepTable(int op, int dir) {
    const int param = paramIndex(op, OpParam::Table);
    const uint32_t key = dataKey(value(param));
    if (key < kFactoryTables) { // the factory tables round
        controller_.pickTable(op, (key + kFactoryTables + (dir > 0 ? 1 : kFactoryTables - 1)) % kFactoryTables);
        return;
    }
    const OpData *d = controller_.opData(key);
    TableLibrary *lib = library();
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
                const std::string said = controller_.importOperatorData(
                    op, DataKind::Table, bytes, slash == std::string::npos ? e.path : e.path.substr(slash + 1),
                    src.folder.name + "/" + e.path, src.folder.frameSize);
                if (!said.empty())
                    flash(said);
                return;
            }
        }
    }
    // Else the patch's own tables round.
    std::vector<uint32_t> tables;
    for (uint32_t k : controller_.data().keys())
        if (const OpData *t = k >= kUserDataBase ? controller_.data().find(k) : nullptr; t && t->kind == DataKind::Table)
            tables.push_back(k);
    const auto at = std::find(tables.begin(), tables.end(), key);
    if (tables.size() > 1 && at != tables.end()) {
        const int n = static_cast<int>(tables.size());
        controller_.pickTable(op, tables[(static_cast<int>(at - tables.begin()) + dir + n) % n]);
    }
}

void View::pickTbRow(const TbRow &row) {
    const int op = tableBrowsing();
    if (op < 0 || row.header || row.error != ReadError::None)
        return;
    if (row.source == -2) {
        if (tbKind_ == DataKind::Table)
            controller_.pickTable(op, row.key);
        return;
    }
    if (row.source == -1) {
        const OpData *d = controller_.data().find(row.key);
        if (!d)
            return;
        if (d->kind == DataKind::Table && tbKind_ == DataKind::Table) {
            controller_.pickTable(op, row.key);
        } else if (d->kind == DataKind::Sample && tbKind_ == DataKind::Sample) {
            const UndoScope step(controller_);
            controller_.setValue(paramIndex(op, OpParam::Sample), static_cast<double>(row.key));
            controller_.setValue(paramIndex(op, OpParam::Wave), static_cast<double>(OpWave::Sample));
        }
        return;
    }
    TableLibrary *lib = library();
    std::vector<uint8_t> bytes;
    if (!lib || row.source >= static_cast<int>(lib->sources().size()) || !lib->readEntry(row.source, row.entry, bytes)) {
        flash(trf(Str::MsgImportFailed, {row.name, tr(Str::ReadBroken)}));
        return;
    }
    const LibrarySource &s = lib->sources()[row.source];
    const std::string &path = s.entries[row.entry].path;
    const size_t slash = path.rfind('/');
    const std::string said = controller_.importOperatorData(op, tbKind_, bytes,
                                                            slash == std::string::npos ? path : path.substr(slash + 1),
                                                            s.folder.name + "/" + path, s.folder.frameSize);
    if (!said.empty())
        flash(said);
}

bool View::dataPress(const Widget &w, Point p, Modifiers mods) {
    switch (w.kind) {
    case Kind::SourceSwitch:
        switchSource(w.a, w.b);
        return true;
    case Kind::TableStep:
        stepTable(w.a, w.b);
        return true;
    case Kind::TablePicker:
        openTableBrowser(w.a, DataKind::Table);
        return true;
    case Kind::SamplePicker:
        openTableBrowser(w.a, DataKind::Sample);
        return true;
    case Kind::DataImport:
        controller_.chooseOperatorFile(w.a, static_cast<DataKind>(w.b));
        return true;
    case Kind::Knob: // Position's display: a press sets Position where the mouse is, on the meter's scale, and a drag
                     // goes on setting it, one undo step; with Shift, a knob's fine drag from where it is
        if (!isTableDisplay(w) || mods.shift)
            return false;
        controller_.beginEdit(w.param);
        drag_.kind = DragKind::TablePosition;
        tablePosDrag(p);
        return true;
    case Kind::SampleRoot:
        drag_.kind = DragKind::Value;
        drag_.startValue = value(w.param);
        controller_.beginEdit(w.param);
        return true;
    case Kind::SampleHandle:
        drag_.kind = DragKind::SampleHandle;
        drag_.startValue = value(w.param);
        drag_.snap = mods.alt;
        controller_.beginEdit(w.param);
        return true;
    case Kind::TbSource:
        tbSource_ = w.a;
        tbScroll_ = 0.0f;
        tbPick_ = -1;
        clearField(Field::Tables);
        return true;
    case Kind::TbAddFolder:
        controller_.chooseTableFolder();
        return true;
    case Kind::TbGroup: {
        const std::vector<TbRow> rows = tbRows();
        if (w.a >= 0 && w.a < static_cast<int>(rows.size()) && rows[w.a].group != "\x02") {
            const std::string id = std::to_string(rows[w.a].source) + "|" + rows[w.a].group;
            if (const auto at = std::find(tbFolded_.begin(), tbFolded_.end(), id); at != tbFolded_.end())
                tbFolded_.erase(at);
            else
                tbFolded_.push_back(id);
        }
        return true;
    }
    case Kind::TbRow: {
        const std::vector<TbRow> rows = tbRows();
        if (w.a >= 0 && w.a < static_cast<int>(rows.size())) {
            tbPick_ = w.a;
            pickTbRow(rows[w.a]);
        }
        return true;
    }
    case Kind::TbDone:
        closeTableBrowser();
        return true;
    case Kind::TbImport:
        if (tableBrowsing() >= 0)
            controller_.chooseOperatorFile(tableBrowsing(), tbKind_);
        return true;
    case Kind::TbScroll:
        return true;
    case Kind::TbScrollBar: { // as the MODS view's: off the thumb, it jumps to center on the click, then drags on
        const Rect thumb = tbThumb();
        if (p.y < thumb.y || p.y >= thumb.bottom())
            tbScroll_ = std::clamp((p.y - thumb.h * 0.5f - kTbScrollTrack.y) /
                                       std::max(kTbScrollTrack.h - thumb.h, 1.0f) * tbEnd(),
                                   0.0f, tbEnd());
        drag_.kind = DragKind::TbScrollBar;
        drag_.startValue = std::clamp(tbScroll_, 0.0f, tbEnd());
        return true;
    }
    default:
        return false;
    }
}

void View::dataDrag(Point p, bool alt) {
    const Widget &w = drag_.widget;
    const Rect area = sampleArea(kOpDisplay);
    double v = std::clamp((p.x - area.x) / area.w, 0.0f, 1.0f);
    if (drag_.fine)
        v = std::clamp(drag_.startValue + (p.x - drag_.start.x) / area.w * 0.1, 0.0, 1.0);
    if (w.b == 3) { // the fade: how far before the loop's end its ramp starts, over half the loop
        const double a = value(paramIndex(w.a, OpParam::LoopStart)), b = value(paramIndex(w.a, OpParam::LoopEnd));
        const double half = 0.5 * std::abs(b - a);
        v = half > 0.0 ? std::clamp((std::max(a, b) - v) / half, 0.0, 1.0) : 0.0;
    } else if (alt && (w.b == 1 || w.b == 2)) { // a loop's point onto the nearest zero crossing going up
        const uint32_t key = dataKey(value(paramIndex(w.a, OpParam::Sample)));
        if (const OpData *d = controller_.opData(key); d && d->frames > 2) {
            const auto at = static_cast<int64_t>(v * (d->frames - 1));
            const int64_t reach = std::max<int64_t>(d->frames / 200, 64);
            int64_t best = -1;
            for (int64_t k = std::max<int64_t>(at - reach, 1); k <= std::min<int64_t>(at + reach, d->frames - 1); ++k)
                if (d->pcm[k - 1] <= 0.0f && d->pcm[k] > 0.0f && (best < 0 || std::abs(k - at) < std::abs(best - at)))
                    best = k;
            if (best >= 0)
                v = static_cast<double>(best) / (d->frames - 1);
        }
    }
    if (v != value(w.param))
        controller_.setValue(w.param, v);
}

void View::tablePosDrag(Point p) {
    const int param = drag_.widget.param;
    if (const double v = meterValue(kOpDisplay, p.y); v != value(param))
        controller_.setValue(param, v);
}

bool View::dataDoubleClick(const Widget &w) {
    if (w.kind == Kind::SampleHandle) { // back to where it starts
        edit(w.param, table_[w.param].def);
        return true;
    }
    if (w.kind == Kind::SampleRoot) {
        startTyping(w);
        return true;
    }
    return false;
}

bool View::dataWheel(Point p, float steps) {
    if (tableBrowsing() < 0 || !kTbList.contains(p))
        return false;
    tbScroll_ = std::clamp(tbScroll_ - steps * 3.0f, 0.0f, tbEnd());
    return true;
}

void View::tablesKey(Key key) {
    const std::vector<TbRow> rows = tbRows();
    switch (key) {
    case Key::Enter: // plays the row picked, and lets the keyboard go, so its keys play it
        if (tbPick_ >= 0 && tbPick_ < static_cast<int>(rows.size()))
            pickTbRow(rows[tbPick_]);
        tbSearching_ = false;
        break;
    case Key::Escape: // empties the search, then lets the keyboard go
        if (!tablesSearch_.text.empty())
            clearField(Field::Tables);
        else
            tbSearching_ = false;
        break;
    case Key::Up:
    case Key::Down: { // to the next row that is a table, that way
        const int step = key == Key::Down ? 1 : -1;
        int at = tbPick_;
        for (int tries = 0; tries < static_cast<int>(rows.size()); ++tries) {
            at = at < 0 ? (step > 0 ? 0 : static_cast<int>(rows.size()) - 1) : at + step;
            if (at < 0 || at >= static_cast<int>(rows.size()))
                return;
            if (!rows[at].header)
                break;
        }
        if (at < 0 || at >= static_cast<int>(rows.size()) || rows[at].header)
            return;
        tbPick_ = at;
        const float shown = kTbList.h / kTbRowH;
        if (at < tbScroll_)
            tbScroll_ = static_cast<float>(at);
        else if (at + 1 > tbScroll_ + shown)
            tbScroll_ = at + 1 - shown;
        tbScroll_ = std::clamp(tbScroll_, 0.0f, tbEnd());
        break;
    }
    default:
        break;
    }
}

std::vector<MenuItem> View::tbSourceMenu(int source) {
    std::vector<MenuItem> items;
    TableLibrary *lib = library();
    if (!lib || source < 0 || source >= static_cast<int>(lib->sources().size()))
        return items;
    tbMenuSource_ = source;
    const int size = lib->sources()[source].folder.frameSize;
    items.push_back({kTbRenameCommand, tr(Str::MenuRename)});
    items.push_back({0, ""});
    for (int k = 0; k < static_cast<int>(std::size(kTbFrameSizes)); ++k)
        items.push_back({kTbFrameSizeCommand + k,
                         trf(Str::MenuFrameSize, {k == 0 ? std::string(tr(Str::TablesFolderAuto))
                                                         : std::to_string(kTbFrameSizes[k])}),
                         size == kTbFrameSizes[k]});
    items.push_back({0, ""});
    items.push_back({kTbRescanCommand, tr(Str::MenuRescan)});
    items.push_back({kTbShowCommand, tr(Str::MenuShowFolder)});
    items.push_back({0, ""});
    items.push_back({kTbRemoveCommand, tr(Str::MenuRemove)});
    return items;
}

bool View::tbMenuCommand(int command) {
    if (command < kTbRenameCommand || command > kTbRemoveCommand)
        return false;
    TableLibrary *lib = library();
    const int source = tbMenuSource_;
    if (!lib || source < 0 || source >= static_cast<int>(lib->sources().size()))
        return true;
    if (command == kTbRenameCommand) {
        Widget w;
        w.kind = Kind::TbSource;
        w.a = source;
        w.rect = tbSourceRow(source + 2);
        const std::string name = lib->sources()[source].folder.name;
        typing_ = Typing{w, name, name.size(), true};
        caretTicks_ = 0;
        return true;
    }
    if (command >= kTbFrameSizeCommand && command < kTbFrameSizeCommand + static_cast<int>(std::size(kTbFrameSizes)))
        lib->setFrameSize(source, kTbFrameSizes[command - kTbFrameSizeCommand]);
    else if (command == kTbRescanCommand)
        lib->rescan(source);
    else if (command == kTbShowCommand)
        controller_.showTableFolder(source);
    else if (command == kTbRemoveCommand) {
        lib->removeFolder(source);
        tbSource_ = tbSource_ == source ? -2 : tbSource_ > source ? tbSource_ - 1 : tbSource_;
    }
    controller_.tableFoldersChanged();
    return true;
}

std::string View::dataTip(const Widget &w) const {
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
        return w.rect.w > 40.0f ? std::string() : tr(w.b == static_cast<int>(DataKind::Table) ? Str::TipImportTable
                                                                                             : Str::TipImportSample);
    case Kind::Knob:
        return isTableDisplay(w) ? std::string(tr(Str::TipTableDisplay)) : std::string();
    case Kind::SampleHandle: {
        const Str tips[] = {Str::TipSampleStart, Str::TipLoopStart, Str::TipLoopEnd, Str::TipLoopFade};
        return tr(tips[std::clamp(w.b, 0, 3)]);
    }
    case Kind::SampleRoot:
        return tr(Str::TipSampleRoot);
    case Kind::TbRow: {
        const std::vector<TbRow> rows = tbRows();
        if (w.a >= 0 && w.a < static_cast<int>(rows.size()) && rows[w.a].error != ReadError::None)
            return tr(readErrorText(rows[w.a].error));
        return {};
    }
    case Kind::TbSource: {
        TableLibrary *lib = library();
        if (lib && w.a >= 0 && w.a < static_cast<int>(lib->sources().size()) && lib->sources()[w.a].missing)
            return trf(Str::TablesMissing, {lib->sources()[w.a].folder.path});
        return {};
    }
    default:
        return {};
    }
}

} // namespace fmvibe::gui
