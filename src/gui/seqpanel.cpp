// The SEQ view: the sequencer's settings on a card in the inspector's place, and the piano roll of the pattern picked,
// with its tools, its ruler and loop, its keys and a lane of one value a note.
#include "seqpanel.h"

#include "icons.h"
#include "layout.h"
#include "modulationmodel.h"
#include "theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <string>
#include <vector>

namespace fmvibe::gui {

using namespace detail;

namespace {

const char *const kPitchNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string pitchName(int pitch) {
    return std::string(kPitchNames[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 1);
}

std::string caps(std::string text) {
    for (char &ch : text)
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
    return text;
}

constexpr double kNewVelocity = 100.0 / 127.0;
constexpr float kSeqScaleListW = 170.0f; // the scale's list, wider than its button: "Pentatonic Major" and the rest
constexpr double kShortestNote = 1.0 / 16.0; // steps
constexpr int kFieldCount = 5;
constexpr float kFieldWidths[kFieldCount] = {72.0f, 58.0f, 92.0f, 58.0f, 84.0f};
constexpr Str kFieldNames[kFieldCount] = {Str::SeqFieldNote, Str::SeqFieldVelocity, Str::SeqFieldChance,
                                          Str::SeqFieldLength, Str::SeqFieldOffset};
// How far the mouse goes for a step of each field, dragged up and down.
constexpr float kFieldPixels[kFieldCount] = {8.0f, 3.0f, 3.0f, 10.0f, 3.0f};

Rect seqToolButton(int b) { return {kSeqTools.x + b * 30.0f, kSeqTools.y, 30.0f, kSeqTools.h}; }
Rect seqSnapButton() { return {kSeqTools.x + 108.0f, kSeqTools.y, 76.0f, kSeqTools.h}; }
Rect seqStretchButton(int b) { return {kSeqTools.x + 192.0f + (b > 0 ? 0.0f : 32.0f), kSeqTools.y, 30.0f, kSeqTools.h}; }
Rect seqClearButton() { return {kSeqTools.x + kSeqTools.w - 26.0f, kSeqTools.y, 26.0f, kSeqTools.h}; }
Rect seqFlagButton(int flag) { // Slide, then Mute, left of the bin
    return {seqClearButton().x - 8.0f - (flag == 0 ? 104.0f : 50.0f), kSeqTools.y, flag == 0 ? 52.0f : 50.0f,
            kSeqTools.h};
}
Rect seqFieldRect(int field) { // left of Slide, the note first
    float x = seqFlagButton(0).x - 8.0f;
    for (int f = kFieldCount - 1; f >= field; --f)
        x -= kFieldWidths[f];
    return {x, kSeqTools.y, kFieldWidths[field], kSeqTools.h};
}
Rect seqCopyButton() { return {kSeqCard.x + kSeqCard.w - 12.0f - 26.0f, kSeqPatternY, 26.0f, 22.0f}; }
// The scale's row: its root, the scale, then FOLD at the card's right.
Rect seqRootBox() { return {kSeqCardControls, kSeqScaleY, 48.0f, 22.0f}; }
Rect seqScaleButton() { return {kSeqCardControls + 52.0f, kSeqScaleY, 102.0f, 22.0f}; }
Rect seqFoldButton() { return {kSeqCard.x + kSeqCard.w - 12.0f - 54.0f, kSeqScaleY, 54.0f, 22.0f}; }
Rect seqLaneTab(int lane) { return {kSeqGrid.x + lane * 76.0f, kSeqLaneTabs.y + 3.0f, 72.0f, kSeqLaneTabs.h - 6.0f}; }
// The zoom's buttons at the lane tabs' right: across out and in, then up and down out and in, each pair after its
// arrow.
Rect seqZoomButton(int b) {
    const float right = kSeqBox.x + kSeqBox.w - 6.0f;
    const float x = right - (4 - b) * 20.0f - (b < 2 ? 22.0f : 0.0f);
    return {x, kSeqLaneTabs.y + 2.0f, 18.0f, kSeqLaneTabs.h - 4.0f};
}
constexpr float kSeqMaxZoomY = 3.0f, kSeqMinZoomY = 0.6f;

int stepped(double value, int lo, int hi) {
    return std::isfinite(value) ? std::clamp(static_cast<int>(std::lround(value)), lo, hi) : lo;
}

} // namespace

SeqPanel::SeqPanel(PanelHost &host)
    : host_(host), edits_(host.modulationModel()), table_(paramTable()) {
    // The roll as the layout saved it: fitted only to a patch loaded from now (see step()).
    seqLoadsSeen_ = controller().patchLoads();
}

bool SeqPanel::wide() const {
    return controller().layout().view == RoutingView::Seq && !host_.shapeEditorOpen() && !host_.partialEditorOpen();
}

int SeqPanel::seqPattern() const {
    return stepped(value(paramIndex(SeqParam::Pattern)), 0, kSeqPatterns - 1);
}

int SeqPanel::seqSteps() const {
    return kSeqStepsPerBar * stepped(value(paramIndex(seqPattern(), SeqPatternParam::Length)), 1, kSeqBars);
}

std::vector<SeqPanel::SeqNote> SeqPanel::seqNotes() const {
    std::vector<SeqNote> notes;
    const int pattern = seqPattern();
    for (int slot = 0; slot < kSeqNotes; ++slot) {
        const double length = value(seqNoteIndex(pattern, slot, SeqNoteParam::Length));
        if (!(length > 0.0))
            continue;
        notes.push_back({slot, value(seqNoteIndex(pattern, slot, SeqNoteParam::Start)), length,
                         stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127),
                         value(seqNoteIndex(pattern, slot, SeqNoteParam::Velocity)),
                         value(seqNoteIndex(pattern, slot, SeqNoteParam::Chance)),
                         value(seqNoteIndex(pattern, slot, SeqNoteParam::Slide)) >= 0.5,
                         value(seqNoteIndex(pattern, slot, SeqNoteParam::Mute)) >= 0.5});
    }
    return notes;
}

std::vector<int> SeqPanel::seqPitchList() const {
    std::vector<int> pitches;
    const int scale = stepped(value(paramIndex(SeqParam::Scale)), 0, static_cast<int>(SeqScale::Count) - 1);
    const int root = stepped(value(paramIndex(SeqParam::Root)), 0, 127);
    const bool fold = controller().layout().seqFold;
    // Folded, the rows of the scale, and those of notes off it all the same (marked as off it): the pattern's notes,
    // and while notes are dragged, where they were, so the rows don't change under the mouse.
    std::array<bool, 128> noted{};
    if (fold && scale != 0) {
        const int pattern = seqPattern();
        for (int slot = 0; slot < kSeqNotes; ++slot)
            if (value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)) > 0.0)
                noted[static_cast<size_t>(stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127))] = true;
        if (dragging())
            for (const SeqNote &n : seqDrag_.origin)
                noted[static_cast<size_t>(n.pitch)] = true;
    }
    for (int p = 0; p < 128; ++p)
        if (!fold || seqInScale(scale, root, p) || noted[static_cast<size_t>(p)])
            pitches.push_back(p);
    return pitches;
}

int SeqPanel::seqFirstRow() const {
    const std::vector<int> pitches = seqPitchList();
    const std::vector<int> rows = seqRows();
    if (rows.empty())
        return 0;
    return static_cast<int>(std::lower_bound(pitches.begin(), pitches.end(), rows.front()) - pitches.begin());
}

void SeqPanel::seqShowFromRow(float row) {
    const std::vector<int> pitches = seqPitchList();
    if (pitches.empty())
        return;
    const int count = static_cast<int>(seqRows().size());
    const int first = std::clamp(static_cast<int>(std::lround(row)), 0,
                                 std::max(static_cast<int>(pitches.size()) - count, 0));
    controller().layout().seqLow = pitches[static_cast<size_t>(first)];
}

float SeqPanel::seqShownSteps() const {
    const float steps = static_cast<float>(seqSteps());
    return steps / std::clamp(seqZoomX_, 1.0f, std::max(steps / 2.0f, 1.0f));
}

float SeqPanel::seqScroll() const {
    return std::clamp(seqScrollX_, 0.0f, static_cast<float>(seqSteps()) - seqShownSteps());
}

void SeqPanel::zoomSeq(bool across, float factor, Point at) {
    if (across) {
        const double step = seqStepAt(at.x);
        const float steps = static_cast<float>(seqSteps());
        seqZoomX_ = std::clamp(seqZoomX_ * factor, 1.0f, std::max(steps / 2.0f, 1.0f));
        seqScrollX_ = static_cast<float>(step) - (at.x - kSeqGrid.x) / kSeqGrid.w * seqShownSteps();
        seqScrollX_ = seqScroll();
        return;
    }
    const float y = std::clamp(at.y, kSeqGrid.y, kSeqGrid.y + kSeqGrid.h - 1.0f);
    const float rowBelow = (kSeqGrid.y + kSeqGrid.h - y) / seqRowH(); // rows under the mouse
    const float row = static_cast<float>(seqFirstRow()) + rowBelow;
    seqZoomY_ = std::clamp(seqZoomY_ * factor, kSeqMinZoomY, kSeqMaxZoomY);
    seqShowFromRow(row - (kSeqGrid.y + kSeqGrid.h - y) / seqRowH());
}

Rect SeqPanel::seqScrollTrack() const {
    const float x = kSeqGrid.x + static_cast<float>(SeqLane::Count) * 76.0f + 8.0f;
    return {x, kSeqLaneTabs.y + kSeqLaneTabs.h * 0.5f - 3.0f, seqZoomButton(0).x - 30.0f - x, 6.0f};
}

Rect SeqPanel::seqScrollYThumb() const {
    const float total = static_cast<float>(std::max<size_t>(seqPitchList().size(), 1));
    const float count = static_cast<float>(seqRows().size()), first = static_cast<float>(seqFirstRow());
    const Rect track = kSeqScrollYTrack;
    const float h = std::max(count / total * track.h, 16.0f);
    const float top = track.y + track.h - (first + count) / total * track.h;
    return {track.x, std::clamp(top, track.y, track.y + track.h - h), track.w, h};
}

void SeqPanel::seqFit() {
    seqZoomX_ = 1.0f;
    seqScrollX_ = 0.0f;
    const std::vector<SeqNote> notes = seqNotes();
    if (notes.empty())
        return;
    int low = 127, high = 0;
    for (const SeqNote &n : notes) {
        low = std::min(low, n.pitch);
        high = std::max(high, n.pitch);
    }
    const std::vector<int> pitches = seqPitchList();
    auto indexOf = [&](int pitch) {
        return static_cast<float>(std::lower_bound(pitches.begin(), pitches.end(), pitch) - pitches.begin());
    };
    const float span = indexOf(high) - indexOf(low) + 1.0f;
    seqZoomY_ = std::clamp(kSeqGrid.h / (kSeqRowH * span), kSeqMinZoomY, 1.0f);
    const float count = static_cast<float>(seqRows().size());
    seqShowFromRow(indexOf(low) - std::max(count - span, 0.0f) * 0.5f);
}

bool SeqPanel::seqEdgeScroll() {
    if (!dragging() || !wide())
        return false;
    const SeqDrag::Mode mode = seqDrag_.mode;
    const bool across = mode == SeqDrag::Mode::Move || mode == SeqDrag::Mode::Resize ||
                        mode == SeqDrag::Mode::ResizeStart || mode == SeqDrag::Mode::Box || mode == SeqDrag::Mode::Paint;
    const bool rows = mode == SeqDrag::Mode::Move || mode == SeqDrag::Mode::Box || mode == SeqDrag::Mode::Paint;
    if (!across)
        return false;
    constexpr float kZone = 10.0f;
    const Point p = seqDrag_.now;
    bool moved = false;
    // Across, faster the further past the edge, up to a sixteenth of what shows a poll.
    const float left = kSeqGrid.x + kZone - p.x, right = p.x - (kSeqGrid.x + kSeqGrid.w - kZone);
    const float pull = left > 0.0f ? -std::min(left / 40.0f, 1.0f) : right > 0.0f ? std::min(right / 40.0f, 1.0f) : 0.0f;
    if (pull != 0.0f) {
        const float before = seqScroll();
        seqScrollX_ = seqScroll() + pull * seqShownSteps() / 16.0f;
        seqScrollX_ = seqScroll();
        moved = seqScrollX_ != before;
    }
    // Up and down, by rows: up to half a row a poll, whole rows at a time.
    const float above = kSeqGrid.y + kZone - p.y, below = p.y - (kSeqGrid.y + kSeqGrid.h - kZone);
    const float rise = above > 0.0f ? std::min(above / 30.0f, 1.0f) : below > 0.0f ? -std::min(below / 30.0f, 1.0f) : 0.0f;
    if (rows && rise != 0.0f) {
        seqEdgeRows_ += rise * 0.5f;
        if (std::abs(seqEdgeRows_) >= 1.0f) {
            const int first = seqFirstRow();
            seqShowFromRow(static_cast<float>(first) + std::trunc(seqEdgeRows_));
            seqEdgeRows_ -= std::trunc(seqEdgeRows_);
            moved = moved || seqFirstRow() != first;
        }
    } else {
        seqEdgeRows_ = 0.0f;
    }
    if (moved) // what is dragged follows the roll scrolling under the mouse
        seqDrag(p);
    return moved;
}

// The playhead, while the view shows, and the notes it plays glowing; the keys easing whether shown or not, so they
// show where they are.
void SeqPanel::poll(const Telemetry &t, std::vector<Rect> &repaint) {
    const Telemetry &was = host_.telemetry();
    bool moved = false;
    if (wide() && (t.seqPosition != was.seqPosition || t.seqPattern != was.seqPattern || t.seqKeys != was.seqKeys))
        moved = true;
    if (wide() && seqTrackGlows(t))
        moved = true;
    if (seqFollowKeys(t) && wide())
        moved = true;
    if (moved)
        repaint.push_back(kPanelsBand);
}

// The roll scrolling under a drag held at its edge; and a patch loaded (a preset, Init, the host's state), the roll
// fitted to its notes.
bool SeqPanel::step() {
    bool moved = seqEdgeScroll();
    if (controller().patchLoads() != seqLoadsSeen_) {
        seqLoadsSeen_ = controller().patchLoads();
        seqFit();
        moved = true;
    }
    return moved;
}

double SeqPanel::seqNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SeqPanel::seqTrackGlows(const Telemetry &t) {
    const bool plays = t.seqPosition >= 0.0f && t.seqPattern == seqPattern();
    const double now = seqNow();
    bool any = false;
    std::array<bool, kSeqNotes> inFirst{};
    for (const SeqNote &n : seqNotes()) {
        const size_t slot = static_cast<size_t>(n.slot);
        inFirst[slot] = plays && !n.mute && t.seqPosition >= n.start && t.seqPosition < n.start + 1.0;
        if (inFirst[slot] && !seqInFirstStep_[slot])
            seqGlowFrom_[slot] = now;
        any = any || seqGlow(n) > 0.0f;
    }
    seqInFirstStep_ = inFirst;
    const bool paint = any || seqGlowed_; // once more as the last glow goes
    seqGlowed_ = any;
    return paint;
}

bool SeqPanel::seqFollowKeys(const Telemetry &t) {
    constexpr float kRise = kKeyLightRise, kFall = kKeyLightFall;
    bool moved = false;
    for (int pitch = 0; pitch < 128; ++pitch) {
        const bool played = (t.seqKeys[static_cast<size_t>(pitch / 32)] >> (pitch % 32) & 1u) != 0;
        const bool heard = dragging() && seqDrag_.sounding == pitch;
        float &light = seqKeyLight_[static_cast<size_t>(pitch)];
        const float next = played || heard ? std::min(light + kRise, 1.0f) : std::max(light - kFall, 0.0f);
        moved = moved || next != light;
        light = next;
    }
    return moved;
}

float SeqPanel::seqGlow(const SeqNote &note) const {
    const double from = seqGlowFrom_[static_cast<size_t>(note.slot)];
    if (from < 0.0)
        return 0.0f;
    const int rate = stepped(value(paramIndex(SeqParam::Rate)), 0, static_cast<int>(SeqRate::Count) - 1);
    const float hostTempo = host_.telemetry().tempo;
    const double tempo = hostTempo > 0.0f ? hostTempo : 120.0;
    const double stepSeconds = 60.0 / (tempo * kSeqStepsPerBeat[rate]);
    const double lasts = std::clamp(note.length * stepSeconds, 0.12, 4.0);
    return static_cast<float>(std::clamp(1.0 - (seqNow() - from) / lasts, 0.0, 1.0));
}

Rect SeqPanel::seqScrollThumb() const {
    const Rect track = seqScrollTrack();
    const float steps = static_cast<float>(seqSteps());
    return {track.x + seqScroll() / steps * track.w, track.y, std::max(seqShownSteps() / steps * track.w, 12.0f), track.h};
}

std::vector<int> SeqPanel::seqRows() const {
    const UiLayout &layout = controller().layout();
    const int count = std::max(static_cast<int>(kSeqGrid.h / (kSeqRowH * std::clamp(seqZoomY_, kSeqMinZoomY, kSeqMaxZoomY))), 4);
    const std::vector<int> pitches = seqPitchList();
    const auto from = std::lower_bound(pitches.begin(), pitches.end(), layout.seqLow);
    const long first = std::clamp(static_cast<long>(from - pitches.begin()), 0L,
                                  std::max(static_cast<long>(pitches.size()) - count, 0L));
    return std::vector<int>(pitches.begin() + first, pitches.begin() + std::min<long>(first + count, static_cast<long>(pitches.size())));
}

float SeqPanel::seqRowH() const {
    return kSeqGrid.h / static_cast<float>(std::max<size_t>(seqRows().size(), 1));
}

float SeqPanel::seqStepX(double step) const {
    return kSeqGrid.x + static_cast<float>((step - seqScroll()) / seqShownSteps()) * kSeqGrid.w;
}

double SeqPanel::seqStepAt(float x) const {
    return seqScroll() + (x - kSeqGrid.x) / kSeqGrid.w * seqShownSteps();
}

float SeqPanel::seqRowY(int pitch) const {
    const std::vector<int> rows = seqRows();
    const auto at = std::find(rows.begin(), rows.end(), pitch);
    if (at == rows.end())
        return kSeqGrid.y + kSeqGrid.h + 100.0f;
    return kSeqGrid.y + kSeqGrid.h - static_cast<float>(at - rows.begin() + 1) * seqRowH();
}

int SeqPanel::seqPitchAt(float y) const {
    const std::vector<int> rows = seqRows();
    if (rows.empty())
        return 60;
    const int row = static_cast<int>(std::floor((kSeqGrid.y + kSeqGrid.h - y) / seqRowH()));
    return rows[static_cast<size_t>(std::clamp(row, 0, static_cast<int>(rows.size()) - 1))];
}

double SeqPanel::seqSnapped(double step, bool free) const {
    const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
    return free || grid <= 0.0 ? step : std::floor(step / grid + 1e-9) * grid;
}

Rect SeqPanel::seqNoteRect(const SeqNote &note) const {
    const float x0 = seqStepX(note.start), x1 = seqStepX(note.start + note.length);
    const float rh = seqRowH();
    return {x0, seqRowY(note.pitch) + 1.0f, std::max(x1 - x0, 3.0f), rh - 2.0f};
}

int SeqPanel::seqNoteAt(Point p, bool *atEnd, bool *atStart) const {
    const std::vector<SeqNote> notes = seqNotes();
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const Rect r = seqNoteRect(*it);
        if (p.x >= r.x - 1.0f && p.x < r.right() + 1.0f && p.y >= r.y - 1.0f && p.y < r.bottom() + 1.0f) {
            // Each end a strip of up to 6 px; on a short note, its right third and its left quarter, the middle
            // moving it.
            const bool end = p.x >= r.right() - std::min(6.0f, r.w * 0.34f);
            if (atEnd)
                *atEnd = end;
            if (atStart)
                *atStart = !end && p.x < r.x + std::min(6.0f, r.w * 0.25f);
            return it->slot;
        }
    }
    return -1;
}

int SeqPanel::seqFreeSlot() const {
    const int pattern = seqPattern();
    for (int slot = 0; slot < kSeqNotes; ++slot)
        if (!(value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)) > 0.0))
            return slot;
    return -1;
}

bool SeqPanel::seqSelected(int slot) const {
    return seqSelectionPattern_ == seqPattern() &&
           std::find(seqSelection_.begin(), seqSelection_.end(), slot) != seqSelection_.end();
}

std::vector<int> SeqPanel::seqPicked() const {
    std::vector<int> picked;
    if (seqSelectionPattern_ != seqPattern())
        return picked;
    for (int slot : seqSelection_)
        if (value(seqNoteIndex(seqPattern(), slot, SeqNoteParam::Length)) > 0.0)
            picked.push_back(slot);
    return picked;
}

// --- Widgets.

void SeqPanel::widgets(std::vector<Widget> &list) const {
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    const int pattern = seqPattern();
    // The card: its switch, the patterns and copying one, when it plays, the knobs, the order and root, the scale.
    add(Kind::Toggle, {kSeqCard.x + kSeqCard.w - 12.0f - 26.0f, kSeqTitleY, 26.0f, 22.0f}, paramIndex(SeqParam::Enabled));
    size_t from = list.size();
    for (int p = 0; p < kSeqPatterns; ++p)
        add(Kind::Choice, {kSeqCardControls + p * 30.0f, kSeqPatternY, 28.0f, 22.0f}, paramIndex(SeqParam::Pattern), -1, p);
    mergeButtons(list, from);
    add(Kind::SeqCopy, seqCopyButton());
    from = list.size();
    for (int p = 0; p < static_cast<int>(SeqPlay::Count); ++p)
        add(Kind::Choice, {kSeqCardControls + p * 62.0f, kSeqPlayY, 60.0f, 22.0f}, paramIndex(SeqParam::Play), -1, p);
    mergeButtons(list, from);
    // Two rows of four: Rate and Length over Octave and Transpose at the left, Swing and Gate over Humanize and
    // Velocity at the right.
    const int knobs[8] = {paramIndex(SeqParam::Rate),   paramIndex(pattern, SeqPatternParam::Length),
                          paramIndex(SeqParam::Swing),  paramIndex(SeqParam::Gate),
                          paramIndex(SeqParam::Octave), paramIndex(SeqParam::Transpose),
                          paramIndex(SeqParam::Humanize), paramIndex(SeqParam::Velocity)};
    const Str labels[8] = {Str::LabelRate,   Str::LabelLength,    Str::LabelSwing,    Str::LabelGate,
                           Str::LabelOctave, Str::LabelTranspose, Str::LabelHumanize, Str::LabelVelocity};
    for (int i = 0; i < 8; ++i) {
        const Rect at = seqKnobCenterRect(i);
        add(Kind::Knob, knobRect({at.x, at.y}, kSeqKnobD), knobs[i]);
        list.back().label = tr(labels[i]);
    }
    from = list.size();
    for (int o = 0; o < static_cast<int>(SeqOrder::Count); ++o)
        add(Kind::Choice, {kSeqCardControls + o * 30.0f, kSeqOrderY, 28.0f, 22.0f}, paramIndex(SeqParam::Order), -1, o);
    mergeButtons(list, from);
    add(Kind::SeqRoot, seqRootBox(), paramIndex(SeqParam::Root));
    add(Kind::ListButton, seqScaleButton(), paramIndex(SeqParam::Scale));
    add(Kind::SeqFold, seqFoldButton());

    // The tools over the roll: select | draw, the snap, stretching, the field of the notes picked, Slide, Mute, the bin.
    from = list.size();
    for (int b = 0; b < 2; ++b)
        add(Kind::SeqTool, seqToolButton(b), -1, -1, b);
    mergeButtons(list, from);
    add(Kind::SeqSnap, seqSnapButton());
    add(Kind::SeqStretch, seqStretchButton(2), -1, -1, 2);
    add(Kind::SeqStretch, seqStretchButton(-2), -1, -1, -2);
    for (int f = 0; f < kFieldCount; ++f)
        add(Kind::SeqField, seqFieldRect(f), -1, -1, f);
    for (int flag = 0; flag < 2; ++flag)
        add(Kind::SeqFlag, seqFlagButton(flag), -1, -1, flag);
    add(Kind::SeqClear, seqClearButton());

    // The roll: its ruler, its keys, the grid, the lane and its tabs.
    add(Kind::SeqRuler, kSeqRuler);
    add(Kind::SeqKeys, kSeqKeys);
    add(Kind::SeqGrid, kSeqGrid);
    add(Kind::SeqLane, kSeqLaneArea);
    for (int lane = 0; lane < static_cast<int>(SeqLane::Count); ++lane)
        add(Kind::SeqLaneTab, seqLaneTab(lane), -1, -1, lane);
    for (int b = 0; b < 4; ++b)
        add(Kind::SeqZoom, seqZoomButton(b), -1, -1, b);
    if (seqShownSteps() < static_cast<float>(seqSteps()))
        add(Kind::SeqScroll, seqScrollTrack().inset(-4.0f));
    add(Kind::SeqScrollY, {kSeqScrollYTrack.x - 3.0f, kSeqScrollYTrack.y, kSeqScrollYTrack.w + 5.0f, kSeqScrollYTrack.h});
}

Rect SeqPanel::seqList(Rect button, int count, float width) const {
    Rect menu{button.x, button.y + button.h + 4.0f, std::max(width, button.w), 8.0f + count * 22.0f};
    if (menu.y + menu.h > host_.height() - 8.0f) // no room below: above its button, inside the editor
        menu.y = std::max(8.0f, button.y - 4.0f - menu.h);
    return menu;
}

void SeqPanel::popups(std::vector<Widget> &list) const {
    if (!wide())
        return;
    auto item = [](Rect menu, int i) { return Rect{menu.x + 4.0f, menu.y + 4.0f + i * 22.0f, menu.w - 8.0f, 22.0f}; };
    auto add = [&list](Kind kind, Rect rect, int param = -1, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = rect;
        w.param = param;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    // The scale's list, under its button.
    if (host_.openList() == paramIndex(SeqParam::Scale)) {
        const int items = static_cast<int>(SeqScale::Count);
        const Rect menu = seqList(seqScaleButton(), items, kSeqScaleListW);
        add(Kind::ListMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        add(Kind::ListMenu, menu, -1, 1);
        for (int i = 0; i < items; ++i)
            add(Kind::ListItem, item(menu, i), host_.openList(), -1, i);
    }
    // The snap's list, under its button.
    if (seqSnapMenu_) {
        add(Kind::SeqSnapMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        list.back().popup = true;
        const Rect menu = seqList(seqSnapButton(), kSeqSnapCount, seqSnapButton().w);
        add(Kind::SeqSnapMenu, menu, -1, 1);
        list.back().popup = true;
        for (int i = 0; i < kSeqSnapCount; ++i) {
            add(Kind::SeqSnapItem, item(menu, i), -1, -1, i);
            list.back().popup = true;
        }
    }
}

// --- Editing, each an undo step.

int SeqPanel::addSeqNote(double start, int pitch, double length) {
    const int slot = seqFreeSlot();
    if (slot < 0) {
        host_.flash(trf(Str::MsgSeqFull, {std::to_string(kSeqNotes)}));
        return -1;
    }
    const int pattern = seqPattern();
    controller().beginStep();
    editMany({{seqNoteIndex(pattern, slot, SeqNoteParam::Start), std::clamp(start, 0.0, seqSteps() - kShortestNote)},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Length), std::max(length, kShortestNote)},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Pitch), std::clamp(pitch, 0, 127)},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Velocity), kNewVelocity},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Chance), 1.0},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Slide), 0.0},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Mute), 0.0}});
    seqDropCovered({slot});
    controller().endStep();
    seqSelection_ = {slot};
    seqSelectionPattern_ = pattern;
    return slot;
}

void SeqPanel::removeSeqNotes(const std::vector<int> &slots) {
    if (slots.empty())
        return;
    const int pattern = seqPattern();
    // Back to the defaults, so a preset saved sparse leaves the slot out.
    std::vector<std::pair<int, double>> changes;
    for (int slot : slots)
        for (int f = 0; f < kSeqNoteParamCount; ++f) {
            const int index = seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f));
            if (value(index) != table_[index].def)
                changes.push_back({index, table_[index].def});
        }
    controller().beginStep();
    editMany(changes);
    controller().endStep();
    std::erase_if(seqSelection_,
                  [&](int s) { return std::find(slots.begin(), slots.end(), s) != slots.end(); });
}

void SeqPanel::stepSeqField(int field, double steps) {
    const std::vector<int> picked = seqPicked();
    if (picked.empty() || steps == 0.0)
        return;
    const int pattern = seqPattern();
    const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
    std::vector<std::pair<int, double>> changes;
    for (int slot : picked) {
        auto at = [&](SeqNoteParam f) { return seqNoteIndex(pattern, slot, f); };
        switch (field) {
        case 0: { // a semitone a step, or a degree of the scale while the roll shows only it
            const int pitch = stepped(value(at(SeqNoteParam::Pitch)), 0, 127);
            int moved = pitch + static_cast<int>(std::lround(steps));
            if (controller().layout().seqFold) {
                const int scale = stepped(value(paramIndex(SeqParam::Scale)), 0, static_cast<int>(SeqScale::Count) - 1);
                const int root = stepped(value(paramIndex(SeqParam::Root)), 0, 127);
                moved = pitch;
                for (long n = std::lround(std::abs(steps)); n > 0;) {
                    moved += steps > 0.0 ? 1 : -1;
                    if (moved < 0 || moved > 127)
                        break;
                    if (seqInScale(scale, root, moved))
                        --n;
                }
            }
            changes.push_back({at(SeqNoteParam::Pitch), std::clamp(moved, 0, 127)});
            break;
        }
        case 1:
            changes.push_back({at(SeqNoteParam::Velocity),
                               std::clamp(std::round(value(at(SeqNoteParam::Velocity)) * 127.0 + steps) / 127.0, 1.0 / 127.0, 1.0)});
            break;
        case 2:
            changes.push_back({at(SeqNoteParam::Chance),
                               std::clamp(std::round(value(at(SeqNoteParam::Chance)) * 100.0 + steps) / 100.0, 0.0, 1.0)});
            break;
        case 3: {
            const double unit = grid > 0.0 ? grid : 1.0;
            changes.push_back({at(SeqNoteParam::Length),
                               std::clamp(value(at(SeqNoteParam::Length)) + steps * unit, kShortestNote,
                                          static_cast<double>(kSeqMaxSteps))});
            break;
        }
        default: { // a hundredth of a step at a time, within half a grid either way of its place on it
            const double unit = grid > 0.0 ? grid : 1.0;
            const double start = value(at(SeqNoteParam::Start));
            const double place = std::round(start / unit) * unit;
            const double offset = std::clamp(start - place + steps * 0.01, -0.5 * unit, 0.5 * unit - 1e-6);
            changes.push_back({at(SeqNoteParam::Start), std::clamp(place + offset, 0.0, kSeqMaxSteps - kShortestNote)});
            break;
        }
        }
    }
    editMany(changes);
}

void SeqPanel::flipSeqFlag(int flag) {
    const std::vector<int> picked = seqPicked();
    if (picked.empty())
        return;
    const int pattern = seqPattern();
    const SeqNoteParam field = flag == 0 ? SeqNoteParam::Slide : SeqNoteParam::Mute;
    const bool all = std::all_of(picked.begin(), picked.end(),
                                 [&](int slot) { return value(seqNoteIndex(pattern, slot, field)) >= 0.5; });
    std::vector<std::pair<int, double>> changes;
    for (int slot : picked)
        changes.push_back({seqNoteIndex(pattern, slot, field), all ? 0.0 : 1.0});
    controller().beginStep();
    editMany(changes);
    controller().endStep();
}

void SeqPanel::seqDropCovered(const std::vector<int> &kept) {
    if (kept.empty())
        return;
    const std::vector<SeqNote> notes = seqNotes();
    auto isKept = [&](int slot) { return std::find(kept.begin(), kept.end(), slot) != kept.end(); };
    std::vector<int> drop;
    for (const SeqNote &n : notes) {
        if (isKept(n.slot))
            continue;
        for (const SeqNote &k : notes)
            if (isKept(k.slot) && k.pitch == n.pitch && std::abs(k.start - n.start) < 1e-6) {
                drop.push_back(n.slot);
                break;
            }
    }
    std::vector<int> picked = seqSelection_; // the notes dropped leave what is picked, the rest stay picked
    removeSeqNotes(drop);
    std::erase_if(picked, [&](int s) { return std::find(drop.begin(), drop.end(), s) != drop.end(); });
    seqSelection_ = picked;
}

void SeqPanel::stretchSeq(double factor) {
    const int pattern = seqPattern();
    std::vector<int> slots = seqPicked();
    const bool whole = slots.empty();
    if (whole)
        for (const SeqNote &n : seqNotes())
            slots.push_back(n.slot);
    std::vector<std::pair<int, double>> changes;
    // From the first note picked (the pattern's start for every note), each start and length scaled.
    double origin = whole ? 0.0 : static_cast<double>(kSeqMaxSteps);
    for (int slot : slots)
        origin = std::min(origin, value(seqNoteIndex(pattern, slot, SeqNoteParam::Start)));
    for (int slot : slots) {
        const int start = seqNoteIndex(pattern, slot, SeqNoteParam::Start);
        const int length = seqNoteIndex(pattern, slot, SeqNoteParam::Length);
        changes.push_back({start, std::min(origin + (value(start) - origin) * factor, kSeqMaxSteps - kShortestNote)});
        changes.push_back({length, std::clamp(value(length) * factor, kShortestNote, static_cast<double>(kSeqMaxSteps))});
    }
    // The whole pattern twice as long takes twice the bars, as many as it can.
    if (whole && factor > 1.0) {
        const int bars = paramIndex(pattern, SeqPatternParam::Length);
        changes.push_back({bars, std::min(std::round(value(bars) * factor), static_cast<double>(kSeqBars))});
    }
    const bool curves = whole && (!seqCurvePoints(SeqCurve::Bend).empty() || !seqCurvePoints(SeqCurve::ModWheel).empty());
    if (changes.empty() && !curves)
        return;
    controller().beginStep();
    editMany(changes);
    seqDropCovered(slots);
    if (whole)
        for (int c = 0; c < kSeqCurves; ++c) {
            std::vector<SeqCurvePoint> points = seqCurvePoints(static_cast<SeqCurve>(c));
            for (SeqCurvePoint &point : points)
                point.x = std::min(point.x * factor, static_cast<double>(kSeqMaxSteps));
            setSeqCurve(static_cast<SeqCurve>(c), points);
        }
    controller().endStep();
}

// --- The lane's curves: the pattern's pitch bend and mod wheel.

bool SeqPanel::seqCurveLane() const {
    const SeqLane lane = controller().layout().seqLane;
    return lane == SeqLane::Bend || lane == SeqLane::ModWheel;
}

SeqCurve SeqPanel::seqLaneCurve() const {
    return controller().layout().seqLane == SeqLane::ModWheel ? SeqCurve::ModWheel : SeqCurve::Bend;
}

std::vector<SeqCurvePoint> SeqPanel::seqCurvePoints(SeqCurve curve) const {
    const int pattern = seqPattern();
    auto get = [this](int index) { return value(index); };
    std::vector<SeqCurvePoint> points;
    const int count = seqCurvePointCount(get, pattern, curve);
    for (int i = 0; i < count; ++i)
        points.push_back({value(seqCurveIndex(pattern, curve, i, PointField::X)),
                          value(seqCurveIndex(pattern, curve, i, PointField::Y)),
                          value(seqCurveIndex(pattern, curve, i, PointField::Curve))});
    return points;
}

void SeqPanel::setSeqCurve(SeqCurve curve, const std::vector<SeqCurvePoint> &points) {
    const int pattern = seqPattern();
    auto get = [this](int index) { return value(index); };
    const int count = std::min(static_cast<int>(points.size()), kSeqCurvePoints);
    const int was = seqCurvePointCount(get, pattern, curve);
    std::vector<std::pair<int, double>> changes;
    auto put = [&](int index, double v) {
        if (value(index) != v)
            changes.push_back({index, v});
    };
    put(seqCurveCountIndex(pattern, curve), count);
    for (int i = 0; i < std::max(count, was); ++i) {
        const bool kept = i < count;
        const int x = seqCurveIndex(pattern, curve, i, PointField::X);
        const int y = seqCurveIndex(pattern, curve, i, PointField::Y);
        const int bend = seqCurveIndex(pattern, curve, i, PointField::Curve);
        put(x, kept ? points[static_cast<size_t>(i)].x : table_[x].def);
        put(y, kept ? points[static_cast<size_t>(i)].y : table_[y].def);
        put(bend, kept ? points[static_cast<size_t>(i)].curve : table_[bend].def);
    }
    if (!changes.empty())
        editMany(changes);
}

float SeqPanel::seqCurveY(SeqCurve curve, double v) const {
    const float top = kSeqLaneArea.y + 6.0f, h = kSeqLaneArea.h - 12.0f;
    const double share = curve == SeqCurve::Bend ? (v + 1.0) * 0.5 : v;
    return top + h - static_cast<float>(std::clamp(share, 0.0, 1.0)) * h;
}

double SeqPanel::seqCurveValueAt(SeqCurve curve, float y) const {
    const float top = kSeqLaneArea.y + 6.0f, h = kSeqLaneArea.h - 12.0f;
    const double share = std::clamp((top + h - y) / h, 0.0f, 1.0f);
    return curve == SeqCurve::Bend ? share * 2.0 - 1.0 : share;
}

SeqCurvePoint SeqPanel::seqCurvePointFor(SeqCurve curve, Point p, bool snap) const {
    double x = std::clamp(seqSnapped(seqStepAt(p.x), !snap), 0.0, static_cast<double>(seqSteps()));
    double y = seqCurveValueAt(curve, p.y);
    if (snap) // the lane's lines: a quarter of the range apart
        y = curve == SeqCurve::Bend ? std::round(y * 2.0) / 2.0 : std::round(y * 4.0) / 4.0;
    return {x, y, 0.0};
}

std::vector<int> SeqPanel::seqCurvePicked() const {
    std::vector<int> points;
    if (!seqCurveLane() || seqCurvePick_.pattern != seqPattern() || seqCurvePick_.curve != seqLaneCurve())
        return points;
    const int count = static_cast<int>(seqCurvePoints(seqCurvePick_.curve).size());
    if (seqCurvePick_.count != count)
        return points; // points came or went since
    for (int point : seqCurvePick_.points)
        if (point >= 0 && point < count)
            points.push_back(point);
    return points;
}

void SeqPanel::setSeqCurvePick(std::vector<int> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    const SeqCurve curve = seqLaneCurve();
    seqCurvePick_ = {seqPattern(), curve, static_cast<int>(seqCurvePoints(curve).size()), std::move(points)};
}

void SeqPanel::moveSeqCurvePoints(SeqCurvePoint to) {
    // As a shape editor's points move together (see moveShapePoints()): as far as the one grabbed, as far as each
    // can, up and down within the curve's range and across between its nearest neighbors not picked.
    const SeqCurve curve = seqLaneCurve();
    std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    const std::vector<int> &picked = seqDrag_.curvePoints;
    const std::vector<SeqCurvePoint> &origin = seqDrag_.curveOrigin;
    const auto at = std::find(picked.begin(), picked.end(), seqDrag_.anchor);
    if (at == picked.end() || origin.size() != picked.size())
        return;
    const size_t g = static_cast<size_t>(at - picked.begin());
    const int count = static_cast<int>(points.size());
    const double low = curve == SeqCurve::Bend ? -1.0 : 0.0, steps = seqSteps();
    auto isPicked = [&](int point) { return std::find(picked.begin(), picked.end(), point) != picked.end(); };
    double dx = to.x - origin[g].x, dy = to.y - origin[g].y;
    for (size_t i = 0; i < picked.size(); ++i) {
        if (picked[i] >= count)
            return;
        dy = std::clamp(dy, std::min(low - origin[i].y, 0.0), std::max(1.0 - origin[i].y, 0.0));
        int below = picked[i] - 1, above = picked[i] + 1;
        while (below >= 0 && isPicked(below))
            --below;
        while (above < count && isPicked(above))
            ++above;
        const double from = below >= 0 ? points[static_cast<size_t>(below)].x : 0.0;
        const double until = above < count ? points[static_cast<size_t>(above)].x : steps;
        dx = std::clamp(dx, std::min(from - origin[i].x, 0.0), std::max(until - origin[i].x, 0.0));
    }
    for (size_t i = 0; i < picked.size(); ++i) {
        SeqCurvePoint &q = points[static_cast<size_t>(picked[i])];
        q.x = origin[i].x + dx;
        q.y = origin[i].y + dy;
    }
    setSeqCurve(curve, points);
}

int SeqPanel::addSeqCurvePoint(SeqCurve curve, SeqCurvePoint point) {
    std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    if (static_cast<int>(points.size()) >= kSeqCurvePoints) {
        host_.flash(trf(Str::MsgSeqCurveFull, {std::to_string(kSeqCurvePoints)}));
        return -1;
    }
    const auto at = std::upper_bound(points.begin(), points.end(), point.x,
                                     [](double s, const SeqCurvePoint &q) { return s < q.x; });
    const int index = static_cast<int>(at - points.begin());
    points.insert(at, point);
    setSeqCurve(curve, points);
    return index;
}

int SeqPanel::seqCurvePointAt(Point p) const {
    if (!seqCurveLane())
        return -1;
    const SeqCurve curve = seqLaneCurve();
    const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i) {
        const SeqCurvePoint &q = points[static_cast<size_t>(i)];
        if (std::hypot(p.x - seqStepX(q.x), p.y - seqCurveY(curve, q.y)) <= 6.0f)
            return i;
    }
    return -1;
}

Point SeqPanel::seqCurveBendPos(const std::vector<SeqCurvePoint> &points, SeqCurve curve, int segment) const {
    const SeqCurvePoint &a = points[static_cast<size_t>(segment)], &b = points[static_cast<size_t>(segment) + 1];
    return {(seqStepX(a.x) + seqStepX(b.x)) * 0.5f, seqCurveY(curve, a.y + (b.y - a.y) * shapeBend(0.5, a.curve))};
}

// A segment's middle takes a bend where it rises or falls and has room for its handle between its points.
int SeqPanel::seqCurveBendAt(Point p) const {
    if (!seqCurveLane())
        return -1;
    const SeqCurve curve = seqLaneCurve();
    const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    for (int i = 0; i + 1 < static_cast<int>(points.size()); ++i) {
        const SeqCurvePoint &a = points[static_cast<size_t>(i)], &b = points[static_cast<size_t>(i) + 1];
        if (seqStepX(b.x) - seqStepX(a.x) < 20.0f || std::abs(b.y - a.y) < 1e-6)
            continue;
        const Point handle = seqCurveBendPos(points, curve, i);
        if (std::hypot(p.x - handle.x, p.y - handle.y) <= 6.0f)
            return i;
    }
    return -1;
}

void SeqPanel::paintSeqCurve(double from, double fromValue, double step, double v, double unit) {
    const SeqCurve curve = seqLaneCurve();
    std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    const double lo = std::min(from, step), hi = std::max(from, step);
    const bool first = std::abs(step - from) < 1e-9; // the stroke's start: only its own point
    std::erase_if(points, [&](const SeqCurvePoint &q) {
        return (q.x > lo + 1e-9 && q.x < hi - 1e-9) || std::abs(q.x - step) < 1e-9;
    });
    const int lines = first ? 1 : static_cast<int>(std::lround((hi - lo) / unit));
    for (int k = 1; k <= lines; ++k) {
        if (static_cast<int>(points.size()) >= kSeqCurvePoints) {
            host_.flash(trf(Str::MsgSeqCurveFull, {std::to_string(kSeqCurvePoints)}));
            break;
        }
        const double t = first ? 1.0 : static_cast<double>(k) / lines;
        const double x = first ? step : from + (step - from) * t;
        const auto at = std::upper_bound(points.begin(), points.end(), x,
                                         [](double s, const SeqCurvePoint &q) { return s < q.x; });
        points.insert(at, {x, fromValue + (v - fromValue) * t, 0.0});
    }
    setSeqCurve(curve, points);
}

void SeqPanel::clearSeqPattern() {
    std::vector<int> slots;
    for (const SeqNote &n : seqNotes())
        slots.push_back(n.slot);
    controller().beginStep();
    removeSeqNotes(slots);
    for (int c = 0; c < kSeqCurves; ++c)
        setSeqCurve(static_cast<SeqCurve>(c), {});
    controller().endStep();
}

// Asked before every note of the pattern (and its curves) goes, which undo brings back.
Panel::ConfirmWords SeqPanel::confirmWords(int) const {
    return {trf(Str::ConfirmSeqClearTitle, {seqPatternName(seqPattern())}), tr(Str::ConfirmSeqClear),
            tr(Str::ButtonClearSeq), false};
}

void SeqPanel::confirmed(int kind, int button) {
    if (kind == kConfirmClear && button == 1)
        clearSeqPattern();
}

bool SeqPanel::seqPatternEmpty(int pattern) const {
    auto get = [this](int index) { return value(index); };
    for (int slot = 0; slot < kSeqNotes; ++slot)
        if (value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)) > 0.0)
            return false;
    for (int c = 0; c < kSeqCurves; ++c)
        if (seqCurvePointCount(get, pattern, static_cast<SeqCurve>(c)) > 0)
            return false;
    return true;
}

void SeqPanel::copySeqPattern() {
    const int from = seqPattern();
    int to = -1;
    for (int i = 1; i < kSeqPatterns && to < 0; ++i) {
        const int p = (from + i) % kSeqPatterns;
        if (seqPatternEmpty(p))
            to = p;
    }
    if (to < 0) {
        host_.flash(tr(Str::MsgSeqNoEmpty));
        return;
    }
    std::vector<std::pair<int, double>> changes;
    for (int f = 0; f < kSeqPatternParamCount; ++f) {
        const int src = paramIndex(from, static_cast<SeqPatternParam>(f));
        const int dst = paramIndex(to, static_cast<SeqPatternParam>(f));
        if (value(dst) != value(src))
            changes.push_back({dst, value(src)});
    }
    for (int slot = 0; slot < kSeqNotes; ++slot)
        for (int f = 0; f < kSeqNoteParamCount; ++f) {
            const int src = seqNoteIndex(from, slot, static_cast<SeqNoteParam>(f));
            const int dst = seqNoteIndex(to, slot, static_cast<SeqNoteParam>(f));
            if (value(dst) != value(src))
                changes.push_back({dst, value(src)});
        }
    for (int c = 0; c < kSeqCurves; ++c)
        for (int f = 0; f < kSeqCurveParamCount; ++f) {
            const int src = seqCurveCountIndex(from, static_cast<SeqCurve>(c)) + f;
            const int dst = seqCurveCountIndex(to, static_cast<SeqCurve>(c)) + f;
            if (value(dst) != value(src))
                changes.push_back({dst, value(src)});
        }
    controller().beginStep();
    editMany(changes);
    controller().endStep();
    host_.flash(trf(Str::MsgSeqCopied, {seqPatternName(from), seqPatternName(to)}));
}

// --- The notes copied, cut and pasted, and the roll's menu.

SeqPanel::SeqClip SeqPanel::seqCopyPicked() const {
    SeqClip clip;
    const std::vector<int> picked = seqPicked();
    if (picked.empty())
        return clip;
    const int pattern = seqPattern();
    double first = kSeqMaxSteps, end = 0.0;
    for (int slot : picked) {
        const double start = value(seqNoteIndex(pattern, slot, SeqNoteParam::Start));
        first = std::min(first, start);
        end = std::max(end, start + value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)));
    }
    for (int slot : picked) {
        std::array<double, kSeqNoteParamCount> fields{};
        for (int f = 0; f < kSeqNoteParamCount; ++f)
            fields[static_cast<size_t>(f)] = value(seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f)));
        fields[static_cast<size_t>(SeqNoteParam::Start)] -= first;
        clip.notes.push_back(fields);
    }
    // As far as they reach, rounded up to the snap's grid (a step with it off), so what goes after them stays on it.
    const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
    const double unit = grid > 0.0 ? grid : 1.0;
    clip.from = first;
    clip.span = std::max(std::ceil((end - first) / unit - 1e-9) * unit, unit);
    return clip;
}

bool SeqPanel::seqPaste(const SeqClip &clip, double at) {
    if (clip.notes.empty())
        return false;
    const int pattern = seqPattern();
    const double last = seqSteps() - kShortestNote;
    std::vector<int> pasted;
    controller().beginStep();
    for (const auto &fields : clip.notes) {
        const double start = at + fields[static_cast<size_t>(SeqNoteParam::Start)];
        if (start > last + 1e-9) // past the pattern's end
            continue;
        const int slot = seqFreeSlot();
        if (slot < 0) {
            host_.flash(trf(Str::MsgSeqFull, {std::to_string(kSeqNotes)}));
            break;
        }
        std::vector<std::pair<int, double>> changes;
        for (int f = 0; f < kSeqNoteParamCount; ++f)
            changes.push_back({seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f)),
                               f == static_cast<int>(SeqNoteParam::Start) ? std::max(start, 0.0)
                                                                          : fields[static_cast<size_t>(f)]});
        editMany(changes);
        pasted.push_back(slot);
    }
    if (!pasted.empty()) {
        seqSelection_ = pasted;
        seqSelectionPattern_ = pattern;
        seqDropCovered(pasted);
    }
    controller().endStep();
    return !pasted.empty();
}

namespace {
// The roll's menu's commands, after SeqPanel::kMenuCommands.
enum SeqMenu { kSeqCut, kSeqCopy, kSeqPaste, kSeqDuplicate, kSeqDelete, kSeqSelectAll, kSeqSlide, kSeqMute, kSeqQuantize };
} // namespace

bool SeqPanel::contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) {
    if (!hit || !wide() || hit->kind != Kind::SeqGrid)
        return false;
    const int pattern = seqPattern();
    if (seqSelectionPattern_ != pattern) { // another pattern shown: what was picked was another's
        seqSelection_.clear();
        seqSelectionPattern_ = pattern;
    }
    // On a note not picked, that note alone; elsewhere, what is picked stays.
    if (const int note = seqNoteAt(p); note >= 0 && !seqSelected(note))
        seqSelection_ = {note};
    seqMenuAt_ = std::clamp(seqSnapped(seqStepAt(p.x), false), 0.0, static_cast<double>(seqSteps()));
    const std::vector<int> picked = seqPicked();
    auto add = [&](int command, Str label, bool checked = false) {
        items.push_back({kMenuCommands + command, tr(label), checked});
    };
    auto all = [&](SeqNoteParam field) {
        return std::all_of(picked.begin(), picked.end(),
                           [&](int slot) { return value(seqNoteIndex(pattern, slot, field)) >= 0.5; });
    };
    if (!picked.empty()) {
        add(kSeqCut, Str::MenuSeqCut);
        add(kSeqCopy, Str::MenuSeqCopy);
    }
    if (!seqClip_.notes.empty())
        add(kSeqPaste, Str::MenuSeqPaste);
    if (!picked.empty()) {
        add(kSeqDuplicate, Str::MenuDuplicate);
        add(kSeqDelete, Str::MenuSeqDelete);
    }
    if (!seqNotes().empty()) {
        if (!items.empty())
            items.push_back({0, ""});
        add(kSeqSelectAll, Str::MenuSeqSelectAll);
    }
    if (!picked.empty()) {
        items.push_back({0, ""});
        add(kSeqSlide, Str::MenuSeqSlide, all(SeqNoteParam::Slide));
        add(kSeqMute, Str::MenuSeqMute, all(SeqNoteParam::Mute));
        items.push_back({0, ""});
        add(kSeqQuantize, Str::MenuSeqQuantize);
    }
    return true;
}

bool SeqPanel::menuCommand(int command) {
    if (command < kMenuCommands || command >= kMenuCommands + kMenuCount)
        return false;
    command -= kMenuCommands;
    if (!wide())
        return true;
    const int pattern = seqPattern();
    const std::vector<int> picked = seqPicked();
    switch (command) {
    case kSeqCut:
    case kSeqCopy:
        host_.sendKey(command == kSeqCut ? Key::Cut : Key::Copy, false); // as the keys do, through the editor
        break;
    case kSeqPaste: // where the menu was opened
        if (seqPaste(seqClip_, seqMenuAt_))
            seqClipNext_ = seqMenuAt_ + seqClip_.span;
        break;
    case kSeqDuplicate: { // right after them, leaving what was copied as it is
        const SeqClip clip = seqCopyPicked();
        seqPaste(clip, clip.from + clip.span);
        break;
    }
    case kSeqDelete:
        removeSeqNotes(picked);
        break;
    case kSeqSelectAll:
        host_.sendKey(Key::SelectAll, false);
        break;
    case kSeqSlide:
    case kSeqMute:
        flipSeqFlag(command == kSeqSlide ? 0 : 1);
        break;
    case kSeqQuantize: { // each start onto the snap's grid (a step with it off), as OFFSET's double-click puts it
        const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const double unit = grid > 0.0 ? grid : 1.0;
        std::vector<std::pair<int, double>> changes;
        for (int slot : picked) {
            const int start = seqNoteIndex(pattern, slot, SeqNoteParam::Start);
            changes.push_back({start, std::clamp(std::round(value(start) / unit) * unit, 0.0, seqSteps() - kShortestNote)});
        }
        if (changes.empty())
            break;
        controller().beginStep();
        editMany(changes);
        seqDropCovered(picked);
        controller().endStep();
        break;
    }
    default:
        break;
    }
    return true;
}

std::string SeqPanel::seqFieldText(int field) const {
    const std::vector<int> picked = seqPicked();
    if (picked.empty())
        return "\xE2\x80\x93"; // an en dash: nothing picked
    const int pattern = seqPattern();
    const int slot = picked.front();
    char text[32];
    switch (field) {
    case 0:
        return pitchName(stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127));
    case 1:
        return std::to_string(std::lround(value(seqNoteIndex(pattern, slot, SeqNoteParam::Velocity)) * 127.0));
    case 2:
        std::snprintf(text, sizeof text, "%ld %%", std::lround(value(seqNoteIndex(pattern, slot, SeqNoteParam::Chance)) * 100.0));
        return text;
    case 3:
        std::snprintf(text, sizeof text, "%g", std::round(value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)) * 100.0) / 100.0);
        return text;
    default: {
        const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const double unit = grid > 0.0 ? grid : 1.0;
        const double start = value(seqNoteIndex(pattern, slot, SeqNoteParam::Start));
        const long offset = std::lround((start - std::round(start / unit) * unit) * 100.0);
        if (offset == 0)
            return "0";
        std::snprintf(text, sizeof text, "%+ld %%", offset);
        return text;
    }
    }
}

// --- The keyboard.

// What the View hands it once the shape editors haven't taken the key, with nothing typed, no dialog open and no drag
// going on (see View::seqKey()).
bool SeqPanel::key(Key key, bool shift) {
    if (!wide())
        return false;
    // The lane's curve: Delete removes its points picked, and Ctrl+A over the lane picks every one.
    if (seqCurveLane() && key == Key::Delete && !seqCurvePicked().empty()) {
        const SeqCurve curve = seqLaneCurve();
        const std::vector<int> picked = seqCurvePicked();
        std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        for (auto it = picked.rbegin(); it != picked.rend(); ++it)
            points.erase(points.begin() + *it);
        controller().beginStep();
        setSeqCurve(curve, points);
        controller().endStep();
        seqCurvePick_ = SeqCurvePick{};
        return true;
    }
    if (seqCurveLane() && key == Key::SelectAll && host_.hovered() && host_.hovered()->kind == Kind::SeqLane) {
        std::vector<int> all(seqCurvePoints(seqLaneCurve()).size());
        std::iota(all.begin(), all.end(), 0);
        host_.clearShapePick();
        setSeqCurvePick(std::move(all));
        return true;
    }
    const int pattern = seqPattern();
    if (seqSelectionPattern_ != pattern) { // another pattern shown: what was picked was another's
        seqSelection_.clear();
        seqSelectionPattern_ = pattern;
    }
    if (key == Key::SelectAll) {
        // So that Delete then removes the notes, not a shape's points or the lane curve's picked before.
        host_.clearShapePick();
        seqCurvePick_ = SeqCurvePick{};
        seqSelection_.clear();
        for (const SeqNote &n : seqNotes())
            seqSelection_.push_back(n.slot);
        return true;
    }
    if (key == Key::Paste) { // at the step under the mouse on the roll, else after what was copied or pasted last
        if (seqClip_.notes.empty())
            return false;
        const Point mouse = host_.mouse();
        const double at = kSeqGrid.contains(mouse)
                              ? std::clamp(seqSnapped(seqStepAt(mouse.x), false), 0.0, static_cast<double>(seqSteps()))
                              : seqClipNext_;
        if (seqPaste(seqClip_, at))
            seqClipNext_ = at + seqClip_.span;
        return true;
    }
    const std::vector<int> picked = seqPicked();
    if (picked.empty() || (key != Key::Left && key != Key::Right && key != Key::Up && key != Key::Down &&
                           key != Key::Delete && key != Key::Copy && key != Key::Cut))
        return false;
    if (key == Key::Copy || key == Key::Cut) { // a paste then going after them, or for a cut where they were
        seqClip_ = seqCopyPicked();
        seqClipNext_ = key == Key::Cut ? seqClip_.from : seqClip_.from + seqClip_.span;
        if (key == Key::Cut)
            removeSeqNotes(picked);
        return true;
    }
    if (key == Key::Delete) {
        removeSeqNotes(picked);
        return true;
    }
    std::vector<std::pair<int, double>> changes;
    if (key == Key::Left || key == Key::Right) { // a grid step, as far as every note picked stays in the pattern
        const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        double by = (key == Key::Right ? 1.0 : -1.0) * (grid > 0.0 ? grid : 1.0);
        for (int slot : picked) {
            const double start = value(seqNoteIndex(pattern, slot, SeqNoteParam::Start));
            by = std::clamp(by, -start, seqSteps() - kShortestNote - start);
        }
        for (int slot : picked)
            changes.push_back({seqNoteIndex(pattern, slot, SeqNoteParam::Start),
                               value(seqNoteIndex(pattern, slot, SeqNoteParam::Start)) + by});
    } else if (shift) { // an octave, as far as every note picked stays a MIDI note
        int by = key == Key::Up ? 12 : -12;
        for (int slot : picked) {
            const int pitch = stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127);
            if (pitch + by < 0 || pitch + by > 127)
                by = 0;
        }
        for (int slot : picked)
            changes.push_back({seqNoteIndex(pattern, slot, SeqNoteParam::Pitch),
                               value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)) + by});
    } else { // a row: a semitone, or a degree of the scale
        controller().beginStep();
        stepSeqField(0, key == Key::Up ? 1.0 : -1.0);
        seqDropCovered(picked);
        controller().endStep();
        seqReveal(picked, key == Key::Up);
        return true;
    }
    controller().beginStep();
    editMany(changes);
    seqDropCovered(picked);
    controller().endStep();
    if (key == Key::Up || key == Key::Down)
        seqReveal(picked, key == Key::Up);
    return true;
}

void SeqPanel::seqReveal(const std::vector<int> &slots, bool up) {
    const std::vector<int> rows = seqRows();
    if (slots.empty() || rows.empty())
        return;
    const int pattern = seqPattern();
    int low = 127, high = 0;
    for (int slot : slots) {
        const int pitch = stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127);
        low = std::min(low, pitch);
        high = std::max(high, pitch);
    }
    if (low >= rows.front() && high <= rows.back())
        return; // all of them show
    const std::vector<int> pitches = seqPitchList();
    auto indexOf = [&](int pitch) {
        return static_cast<float>(std::lower_bound(pitches.begin(), pitches.end(), pitch) - pitches.begin());
    };
    const float count = static_cast<float>(rows.size());
    const float first = static_cast<float>(seqFirstRow());
    const float showHigh = indexOf(high) - count + 1.0f; // the first row that shows the highest at the top
    const float showLow = indexOf(low);                   // and the lowest at the bottom
    float to = first;
    if (high > rows.back())
        to = showHigh;
    if (low < rows.front() && (!up || high <= rows.back()))
        to = showLow;
    if (up && high - low >= static_cast<int>(count)) // more than fit: the highest, moving up
        to = showHigh;
    seqShowFromRow(to);
}

// --- The mouse.

void SeqPanel::seqHear(int pitch) {
    if (pitch == seqDrag_.sounding)
        return;
    if (seqDrag_.sounding >= 0)
        controller().auditionNote(seqDrag_.sounding, false);
    seqDrag_.sounding = pitch;
    if (pitch >= 0)
        controller().auditionNote(pitch, true);
}

void SeqPanel::pressing(const Widget *hit) { // the points picked on the lane's curve, at a click off the lane
    if (!(hit && hit->kind == Kind::SeqLane))
        seqCurvePick_ = SeqCurvePick{};
}

bool SeqPanel::press(const Widget &w, Point p, Modifiers mods) {
    return pressAs(w, p, mods, w);
}

bool SeqPanel::pressAs(const Widget &w, Point p, Modifiers mods, const Widget &held) {
    UiLayout &layout = controller().layout();
    const int pattern = seqPattern();
    if (seqSelectionPattern_ != pattern) { // another pattern shown: what was picked was another's
        seqSelection_.clear();
        seqSelectionPattern_ = pattern;
    }
    auto startDrag = [&](SeqDrag::Mode mode, bool step) {
        seqDrag_ = SeqDrag{};
        seqDrag_.mode = mode;
        seqDrag_.free = mods.alt;
        seqDrag_.grabStep = seqStepAt(p.x);
        seqDrag_.grabPitch = seqPitchAt(p.y);
        seqDrag_.start = seqDrag_.now = p;
        if (step)
            controller().beginStep();
        host_.startDrag(*this, held, p, step);
    };
    switch (w.kind) {
    case Kind::SeqTool:
        layout.seqDraw = w.b == 1;
        return true;
    case Kind::SeqSnap:
        seqSnapMenu_ = !seqSnapMenu_;
        return true;
    case Kind::SeqSnapMenu:
        if (w.a == 0)
            seqSnapMenu_ = false;
        return true;
    case Kind::SeqSnapItem:
        layout.seqSnap = w.b;
        seqSnapMenu_ = false;
        return true;
    case Kind::SeqStretch:
        stretchSeq(w.b > 0 ? 2.0 : 0.5);
        return true;
    case Kind::SeqField:
        if (!seqPicked().empty()) {
            startDrag(SeqDrag::Mode::Field, true);
            seqDrag_.anchor = w.b;
            seqDrag_.loopFrom = 0.0; // the steps applied so far
        }
        return true;
    case Kind::SeqFlag:
        flipSeqFlag(w.b);
        return true;
    case Kind::SeqClear:
        if (!seqPatternEmpty(seqPattern()))
            host_.askConfirm(*this, kConfirmClear);
        return true;
    case Kind::SeqCopy:
        copySeqPattern();
        return true;
    case Kind::SeqRoot: // up and down, as a knob
        host_.dragValue(w, p, mods);
        return true;
    case Kind::SeqFold:
        layout.seqFold = !layout.seqFold;
        return true;
    case Kind::SeqLaneTab:
        layout.seqLane = static_cast<SeqLane>(w.b);
        return true;
    case Kind::SeqZoom:
        zoomSeq(w.b < 2, w.b % 2 == 1 ? 1.5f : 1.0f / 1.5f, kSeqGrid.center());
        return true;
    case Kind::SeqScrollY: { // as the bar across: on the thumb, drag it; elsewhere it jumps to center on the click
        const Rect thumb = seqScrollYThumb();
        const float total = static_cast<float>(seqPitchList().size());
        if (p.y < thumb.y || p.y >= thumb.y + thumb.h)
            seqShowFromRow((kSeqScrollYTrack.y + kSeqScrollYTrack.h - p.y) / kSeqScrollYTrack.h * total -
                           0.5f * static_cast<float>(seqRows().size()));
        startDrag(SeqDrag::Mode::ScrollY, false);
        seqDrag_.grabStep = seqFirstRow();
        return true;
    }
    case Kind::SeqScroll: { // on the thumb, drag it; elsewhere on the track, it jumps to center on the click
        const Rect track = seqScrollTrack(), thumb = seqScrollThumb();
        if (p.x < thumb.x || p.x >= thumb.x + thumb.w)
            seqScrollX_ = (p.x - thumb.w * 0.5f - track.x) / track.w * static_cast<float>(seqSteps());
        seqScrollX_ = seqScroll();
        startDrag(SeqDrag::Mode::Scroll, false);
        seqDrag_.grabStep = seqScrollX_;
        return true;
    }
    case Kind::SeqKeys: { // the key heard alone, while it is held (sliding to the next as the mouse does)
        startDrag(SeqDrag::Mode::Audition, false);
        seqDrag_.anchor = seqPitchAt(p.y);
        seqHear(seqDrag_.anchor);
        return true;
    }
    case Kind::SeqRuler: { // an end of the loop, the whole of it, or a new loop across the bars outside it
        startDrag(seqRulerMode(p.x), true);
        seqDrag_.loopFrom = std::clamp(std::round(seqStepAt(p.x)), 0.0, static_cast<double>(seqSteps()));
        const auto [start, end] = seqLoop();
        seqDrag_.grabPitch = start; // where the loop was, for moving it
        seqDrag_.anchor = end;
        return true;
    }
    case Kind::SeqLane: {
        if (seqCurveLane()) {
            // As with notes: on a point, it moves; on a segment's middle, the segment bends; elsewhere, Draw puts a point
            // down and moves it while the button is held, or with Shift draws the curve, and Select does nothing (a
            // double-click puts a point down). Ctrl puts them on the grid. Each an undo step.
            // Points are picked as notes are: a box from empty space with Select (Ctrl or Shift adding it to those
            // picked), a point clicked picked alone, or with Ctrl or Shift picked along with the others or let go;
            // a point picked moves with the others.
            const SeqCurve curve = seqLaneCurve();
            const bool free = !mods.ctrl;
            const bool adds = mods.ctrl || mods.shift;
            seqCurveJustAdded_ = -1;
            if (const int point = seqCurvePointAt(p); point >= 0) {
                startDrag(SeqDrag::Mode::CurvePoint, true);
                seqDrag_.anchor = point;
                seqDrag_.free = free;
                std::vector<int> picked = seqCurvePicked();
                const bool was = std::find(picked.begin(), picked.end(), point) != picked.end();
                if (adds && was)
                    seqDrag_.curveUnpick = true;
                else if (adds)
                    picked.push_back(point);
                else if (!was)
                    picked = {point};
                else
                    seqDrag_.narrow = picked.size() > 1;
                setSeqCurvePick(picked);
                picked = seqCurvePicked();
                if (picked.size() > 1) {
                    const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
                    for (int q : picked)
                        seqDrag_.curveOrigin.push_back(points[static_cast<size_t>(q)]);
                    seqDrag_.curvePoints = std::move(picked);
                }
                return true;
            }
            if (const int segment = seqCurveBendAt(p); segment >= 0) {
                startDrag(SeqDrag::Mode::CurveBend, true);
                seqDrag_.anchor = segment;
                seqDrag_.startCurve = seqCurvePoints(curve)[static_cast<size_t>(segment)].curve;
                return true;
            }
            if (!layout.seqDraw) { // a box across the lane, picking the points it reaches
                startDrag(SeqDrag::Mode::CurveBox, false);
                seqDrag_.curveBase = adds ? seqCurvePicked() : std::vector<int>{};
                setSeqCurvePick(seqDrag_.curveBase);
                return true;
            }
            seqCurvePick_ = SeqCurvePick{};
            const double steps = seqSteps();
            if (mods.shift) { // a point at every grid line the stroke crosses, a step apart with the snap off
                const double grid = kSeqSnaps[std::clamp(layout.seqSnap, 0, kSeqSnapCount - 1)];
                const double unit = grid > 0.0 ? grid : 1.0;
                const double step = std::clamp(std::round(seqStepAt(p.x) / unit) * unit, 0.0, steps);
                startDrag(SeqDrag::Mode::CurvePaint, true);
                seqDrag_.free = free;
                seqDrag_.loopFrom = step;
                seqDrag_.startCurve = seqCurvePointFor(curve, p, !free).y;
                paintSeqCurve(step, seqDrag_.startCurve, step, seqDrag_.startCurve, unit);
                return true;
            }
            startDrag(SeqDrag::Mode::CurvePoint, true);
            const int index = addSeqCurvePoint(curve, seqCurvePointFor(curve, p, !free));
            seqDrag_.anchor = index;
            seqDrag_.free = free;
            seqCurveJustAdded_ = index;
            if (index >= 0)
                setSeqCurvePick({index});
            return true;
        }
        const int hit = seqLaneNoteAt(p);
        if (!layout.seqDraw) {
            // With Select, the dots are picked as their notes are in the roll: a box from empty space picking the notes
            // whose dots it holds, not their stems (Shift or Ctrl adding them to those picked), a dot clicked picking its
            // note alone, or with Shift or Ctrl along with the others (or letting it go, once let go unmoved); and a dot
            // dragged moves every note picked up or down together, each from where it was.
            const bool adds = mods.shift || mods.ctrl;
            if (hit < 0) {
                startDrag(SeqDrag::Mode::LaneBox, false);
                seqDrag_.boxBase = adds ? seqPicked() : std::vector<int>{};
                seqSelection_ = seqDrag_.boxBase;
                return true;
            }
            bool unpick = false, narrow = false;
            if (adds && seqSelected(hit))
                unpick = true;
            else if (adds)
                seqSelection_.push_back(hit);
            else if (!seqSelected(hit))
                seqSelection_ = {hit};
            else
                narrow = seqPicked().size() > 1;
            startDrag(SeqDrag::Mode::LaneGroup, true);
            seqDrag_.anchor = hit;
            seqDrag_.unpick = unpick;
            seqDrag_.narrow = narrow;
            for (const SeqNote &n : seqNotes())
                if (seqSelected(n.slot))
                    seqDrag_.origin.push_back(n);
            return true;
        }
        // With Draw, on the dot or stem of one of the notes picked, while more than one is: they move together, up and
        // down from where they are. Elsewhere, the notes under the mouse take its height.
        if (hit >= 0 && seqSelected(hit) && seqPicked().size() > 1) {
            startDrag(SeqDrag::Mode::LaneGroup, true);
            for (const SeqNote &n : seqNotes())
                if (seqSelected(n.slot))
                    seqDrag_.origin.push_back(n);
            return true;
        }
        startDrag(SeqDrag::Mode::Lane, true);
        seqDrag_.anchor = hit; // grabbed by its dot or stem: the cursor stays up and down
        seqDrag(p);
        return true;
    }
    case Kind::SeqGrid: {
        bool atEnd = false, atStart = false;
        const int hit = seqNoteAt(p, &atEnd, &atStart);
        if (hit < 0 && layout.seqDraw) {
            // A note put down where the grid takes it; held, the mouse up and down sets its velocity, or with Shift a
            // drag across paints more.
            const double grid = kSeqSnaps[std::clamp(layout.seqSnap, 0, kSeqSnapCount - 1)];
            startDrag(mods.shift ? SeqDrag::Mode::Paint : SeqDrag::Mode::Velocity, true);
            const int slot = addSeqNote(seqSnapped(seqStepAt(p.x), mods.alt), seqPitchAt(p.y), grid > 0.0 ? grid : 1.0);
            if (slot >= 0)
                seqDrag_.painted.push_back(slot);
            seqDrag_.anchor = slot;
            if (slot >= 0) // heard, alone, while the button is held
                seqHear(seqPitchAt(p.y));
            return true;
        }
        if (hit < 0) { // a box picking what it reaches; with Shift, adding to what is picked
            startDrag(SeqDrag::Mode::Box, false);
            seqDrag_.boxBase = mods.shift ? seqPicked() : std::vector<int>{};
            seqSelection_ = seqDrag_.boxBase;
            return true;
        }
        const bool edge = atEnd || atStart;
        // Shift+click adds it to the notes picked, or takes it out (once let go, unless it was dragged).
        bool unpick = false;
        if (mods.shift && !mods.ctrl && !edge && seqSelected(hit))
            unpick = true;
        else if (mods.shift && !mods.ctrl && !edge)
            seqSelection_.push_back(hit);
        else if (!seqSelected(hit))
            seqSelection_ = {hit};
        startDrag(atEnd ? SeqDrag::Mode::Resize : atStart ? SeqDrag::Mode::ResizeStart : SeqDrag::Mode::Move, true);
        // Dragged with Shift (or Alt) held, it follows the mouse off the grid: an end sets the length finely, and the
        // note moved lands anywhere, its OFFSET saying how far off the grid.
        seqDrag_.free = mods.alt || mods.shift;
        seqDrag_.unpick = unpick;
        seqDrag_.anchor = hit;
        if (mods.ctrl && !atEnd && !atStart) { // Ctrl: the notes picked copied, and the copies carried
            std::vector<int> copies;
            for (int slot : seqPicked()) {
                const int copy = seqFreeSlot();
                if (copy < 0) {
                    host_.flash(trf(Str::MsgSeqFull, {std::to_string(kSeqNotes)}));
                    break;
                }
                std::vector<std::pair<int, double>> changes;
                for (int f = 0; f < kSeqNoteParamCount; ++f)
                    changes.push_back({seqNoteIndex(pattern, copy, static_cast<SeqNoteParam>(f)),
                                       value(seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f)))});
                editMany(changes);
                if (slot == hit)
                    seqDrag_.anchor = copy;
                copies.push_back(copy);
            }
            if (!copies.empty())
                seqSelection_ = copies;
        }
        for (const SeqNote &n : seqNotes())
            if (seqSelected(n.slot))
                seqDrag_.origin.push_back(n);
        // The note clicked heard, alone, while the button is held; moved to another row, that row's.
        seqHear(stepped(value(seqNoteIndex(pattern, seqDrag_.anchor, SeqNoteParam::Pitch)), 0, 127));
        return true;
    }
    default:
        return false;
    }
}

void SeqPanel::drag(const DragState &d) {
    const Point p = d.now;
    const Modifiers mods = d.mods;
    seqDrag_.now = p;
    // A note moved, or its end, follows the mouse off the grid while Shift or Alt is held.
    if (seqDrag_.mode == SeqDrag::Mode::Move || seqDrag_.mode == SeqDrag::Mode::Resize ||
        seqDrag_.mode == SeqDrag::Mode::ResizeStart)
        seqDrag_.free = mods.shift || mods.alt;
    // A point of the lane's curve, or a stroke drawing it, on the grid while Ctrl is held.
    if (seqDrag_.mode == SeqDrag::Mode::CurvePoint || seqDrag_.mode == SeqDrag::Mode::CurvePaint)
        seqDrag_.free = !mods.ctrl;
    seqDrag(p);
}

void SeqPanel::seqDrag(Point p) {
    SeqDrag &d = seqDrag_;
    const int pattern = seqPattern();
    const double grid = d.free ? 0.0 : kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
    auto onGrid = [&](double step) { return grid > 0.0 ? std::round(step / grid) * grid : step; };
    const int steps = seqSteps();
    switch (d.mode) {
    case SeqDrag::Mode::Move:
    case SeqDrag::Mode::Resize:
    case SeqDrag::Mode::ResizeStart: {
        const auto anchor = std::find_if(d.origin.begin(), d.origin.end(), [&](const SeqNote &n) { return n.slot == d.anchor; });
        if (anchor == d.origin.end())
            break;
        const double moved = seqStepAt(p.x) - d.grabStep;
        std::vector<std::pair<int, double>> changes;
        if (d.mode == SeqDrag::Mode::Move) {
            // The note grabbed lands on the grid, the others moving as far; up and down by rows.
            double shift = onGrid(anchor->start + moved) - anchor->start;
            double earliest = steps, latest = 0.0;
            for (const SeqNote &n : d.origin) {
                earliest = std::min(earliest, n.start);
                latest = std::max(latest, n.start);
            }
            shift = std::clamp(shift, -earliest, steps - kShortestNote - latest);
            const std::vector<int> rows = seqRows();
            const int pitchShift = seqPitchAt(p.y) - d.grabPitch;
            for (const SeqNote &n : d.origin) {
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Start), n.start + shift});
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Pitch), std::clamp(n.pitch + pitchShift, 0, 127)});
            }
            seqHear(std::clamp(anchor->pitch + pitchShift, 0, 127));
        } else if (d.mode == SeqDrag::Mode::ResizeStart) {
            // The start of the note grabbed onto the grid, its end staying, the others' starts as far: none past
            // its end (leaving the shortest note) nor before the pattern's start.
            double shift = onGrid(anchor->start + moved) - anchor->start;
            for (const SeqNote &n : d.origin) {
                const double shortest = grid > 0.0 ? std::min(grid, n.length) : kShortestNote;
                shift = std::clamp(shift, -n.start, n.length - shortest);
            }
            for (const SeqNote &n : d.origin) {
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Start), n.start + shift});
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Length), n.length - shift});
            }
        } else { // the end of the note grabbed onto the grid, the others' ends as far
            const double end = anchor->start + anchor->length;
            const double shift = onGrid(end + moved) - end;
            for (const SeqNote &n : d.origin)
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Length),
                                   std::clamp(n.length + shift, grid > 0.0 ? std::min(grid, n.length) : kShortestNote,
                                              static_cast<double>(kSeqMaxSteps))});
        }
        editMany(changes);
        break;
    }
    case SeqDrag::Mode::Box: {
        const float x0 = std::min(d.start.x, p.x), x1 = std::max(d.start.x, p.x);
        const float y0 = std::min(d.start.y, p.y), y1 = std::max(d.start.y, p.y);
        seqSelection_ = d.boxBase;
        for (const SeqNote &n : seqNotes()) {
            const Rect r = seqNoteRect(n);
            if (r.x < x1 && r.right() > x0 && r.y < y1 && r.bottom() > y0 && !seqSelected(n.slot))
                seqSelection_.push_back(n.slot);
        }
        break;
    }
    case SeqDrag::Mode::Velocity: { // the note just drawn, a step of velocity a pixel up or down from 100
        if (d.anchor < 0)
            break;
        const double level = std::clamp(std::round(kNewVelocity * 127.0 + (d.start.y - p.y)), 1.0, 127.0);
        std::vector<std::pair<int, double>> changes{{seqNoteIndex(pattern, d.anchor, SeqNoteParam::Velocity), level / 127.0}};
        if (d.extend && !d.origin.empty()) { // across: right moves its end, left its start, the other end staying
            const SeqNote &o = d.origin.front();
            const double moved = seqStepAt(p.x) - d.grabStep;
            const double end = o.start + o.length;
            const double shortest = grid > 0.0 ? std::min(grid, o.length) : kShortestNote;
            if (moved >= 0.0) {
                const double length = std::clamp(onGrid(end + moved) - o.start, shortest, static_cast<double>(kSeqMaxSteps));
                changes.push_back({seqNoteIndex(pattern, d.anchor, SeqNoteParam::Length), length});
            } else {
                const double start = std::clamp(onGrid(o.start + moved), std::max(0.0, end - kSeqMaxSteps), end - shortest);
                changes.push_back({seqNoteIndex(pattern, d.anchor, SeqNoteParam::Start), start});
                changes.push_back({seqNoteIndex(pattern, d.anchor, SeqNoteParam::Length), end - start});
            }
        }
        editMany(changes);
        break;
    }
    case SeqDrag::Mode::Paint: { // a note in each cell of the grid the mouse goes over that has none starting there
        if (!kSeqGrid.contains(p))
            break;
        const double at = seqSnapped(seqStepAt(p.x), d.free);
        const int pitch = seqPitchAt(p.y);
        for (const SeqNote &n : seqNotes())
            if (n.pitch == pitch && std::abs(n.start - at) < 1e-6)
                return;
        const double length = grid > 0.0 ? grid : 1.0;
        const int slot = addSeqNote(at, pitch, length);
        if (slot >= 0)
            d.painted.push_back(slot);
        seqSelection_ = d.painted;
        break;
    }
    case SeqDrag::Mode::LaneGroup: { // the notes picked, each as far up or down as the mouse has gone
        const double moved = (d.start.y - p.y) / (kSeqLaneArea.h - 12.0f);
        const bool velocity = controller().layout().seqLane == SeqLane::Velocity;
        std::vector<std::pair<int, double>> changes;
        for (const SeqNote &n : d.origin)
            changes.push_back(velocity ? std::pair{seqNoteIndex(pattern, n.slot, SeqNoteParam::Velocity),
                                                   std::clamp(std::round((n.velocity + moved) * 127.0), 1.0, 127.0) / 127.0}
                                       : std::pair{seqNoteIndex(pattern, n.slot, SeqNoteParam::Chance),
                                                   std::clamp(std::round((n.chance + moved) * 100.0), 0.0, 100.0) / 100.0});
        editMany(changes);
        break;
    }
    case SeqDrag::Mode::CurvePoint: { // across between its neighbors (on the grid but with Shift or Alt), and up and down
        const SeqCurve curve = seqLaneCurve();
        std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        if (d.anchor < 0 || d.anchor >= static_cast<int>(points.size()))
            break;
        const size_t i = static_cast<size_t>(d.anchor);
        const double lo = i > 0 ? points[i - 1].x : 0.0;
        const double hi = i + 1 < points.size() ? points[i + 1].x : static_cast<double>(steps);
        const SeqCurvePoint to = seqCurvePointFor(curve, p, !d.free);
        if (!d.curvePoints.empty()) { // the points picked, after it
            moveSeqCurvePoints(to);
            break;
        }
        points[i].x = std::clamp(to.x, lo, std::max(lo, hi));
        points[i].y = to.y;
        setSeqCurve(curve, points);
        break;
    }
    case SeqDrag::Mode::CurveBend: { // its middle follows the mouse up and down, as far as a bend takes it
        const SeqCurve curve = seqLaneCurve();
        std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        if (d.anchor < 0 || d.anchor + 1 >= static_cast<int>(points.size()))
            break;
        const size_t i = static_cast<size_t>(d.anchor);
        const double rise = points[i + 1].y > points[i].y ? 1.0 : -1.0; // a bend further up lowers a rising middle
        points[i].curve = std::clamp(d.startCurve - rise * (d.start.y - p.y) / 40.0, -1.0, 1.0);
        setSeqCurve(curve, points);
        break;
    }
    case SeqDrag::Mode::LaneBox: { // the notes whose dots it holds (not their stems), and those picked before it
        const float x0 = std::min(d.start.x, p.x), x1 = std::max(d.start.x, p.x);
        const float y0 = std::min(d.start.y, p.y), y1 = std::max(d.start.y, p.y);
        seqSelection_ = d.boxBase;
        for (const SeqNote &n : seqNotes()) {
            const Point dot = seqLaneDot(n);
            if (dot.x >= x0 && dot.x <= x1 && dot.y >= y0 && dot.y <= y1 && !seqSelected(n.slot))
                seqSelection_.push_back(n.slot);
        }
        break;
    }
    case SeqDrag::Mode::CurveBox: { // the points inside it, and those picked before it
        const Rect box{std::min(d.start.x, p.x), std::min(d.start.y, p.y), std::abs(p.x - d.start.x),
                       std::abs(p.y - d.start.y)};
        const SeqCurve curve = seqLaneCurve();
        const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        std::vector<int> picked = d.curveBase;
        for (int i = 0; i < static_cast<int>(points.size()); ++i) {
            const SeqCurvePoint &q = points[static_cast<size_t>(i)];
            if (box.contains({seqStepX(q.x), seqCurveY(curve, q.y)}))
                picked.push_back(i);
        }
        setSeqCurvePick(std::move(picked));
        break;
    }
    case SeqDrag::Mode::CurvePaint: {
        const double snap = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const double unit = snap > 0.0 ? snap : 1.0;
        const double step = std::clamp(std::round(seqStepAt(p.x) / unit) * unit, 0.0, static_cast<double>(steps));
        if (std::abs(step - d.loopFrom) < 1e-9)
            break; // still on the line it painted last
        const double v = seqCurvePointFor(seqLaneCurve(), p, !d.free).y;
        paintSeqCurve(d.loopFrom, d.startCurve, step, v, unit);
        d.loopFrom = step;
        d.startCurve = v;
        break;
    }
    case SeqDrag::Mode::Lane: { // the notes starting under the mouse take the height it is at
        const double level = std::clamp((kSeqLaneArea.y + kSeqLaneArea.h - 6.0f - p.y) / (kSeqLaneArea.h - 12.0f), 0.0f, 1.0f);
        const bool velocity = controller().layout().seqLane == SeqLane::Velocity;
        std::vector<std::pair<int, double>> changes;
        for (const SeqNote &n : seqLaneNotesAt(p.x)) {
            changes.push_back(velocity ? std::pair{seqNoteIndex(pattern, n.slot, SeqNoteParam::Velocity),
                                                   std::max(std::round(level * 127.0), 1.0) / 127.0}
                                       : std::pair{seqNoteIndex(pattern, n.slot, SeqNoteParam::Chance),
                                                   std::round(level * 100.0) / 100.0});
        }
        editMany(changes);
        break;
    }
    case SeqDrag::Mode::Loop:
    case SeqDrag::Mode::LoopMove:
    case SeqDrag::Mode::LoopStart:
    case SeqDrag::Mode::LoopEnd: {
        const double at = std::clamp(std::round(seqStepAt(p.x)), 0.0, static_cast<double>(steps));
        const int startIndex = paramIndex(pattern, SeqPatternParam::LoopStart);
        const int endIndex = paramIndex(pattern, SeqPatternParam::LoopEnd);
        const double stored = value(endIndex);
        double start = value(startIndex), end = stored <= 0.0 ? steps : std::min(stored, static_cast<double>(steps));
        if (d.mode == SeqDrag::Mode::Loop) {
            start = std::min(d.loopFrom, at);
            end = std::max(d.loopFrom, at);
            if (end - start < 1.0)
                break;
        } else if (d.mode == SeqDrag::Mode::LoopMove) { // as long as it was, a step at a time, inside the pattern
            const double length = d.anchor - d.grabPitch;
            start = std::clamp(d.grabPitch + std::round(seqStepAt(p.x) - d.loopFrom), 0.0, steps - length);
            end = start + length;
        } else if (d.mode == SeqDrag::Mode::LoopStart) {
            start = std::min(at, end - 1.0);
        } else {
            end = std::max(at, start + 1.0);
        }
        editMany({{startIndex, start}, {endIndex, end >= steps ? 0.0 : end}}); // to the end: following the length
        break;
    }
    case SeqDrag::Mode::Field: {
        const double want = std::round((d.start.y - p.y) / kFieldPixels[std::clamp(d.anchor, 0, kFieldCount - 1)]);
        stepSeqField(d.anchor, want - d.loopFrom);
        d.loopFrom = want;
        break;
    }
    case SeqDrag::Mode::Scroll:
        seqScrollX_ = static_cast<float>(d.grabStep) + (p.x - d.start.x) / seqScrollTrack().w * steps;
        seqScrollX_ = seqScroll();
        break;
    case SeqDrag::Mode::ScrollY: // up for higher rows
        seqShowFromRow(static_cast<float>(d.grabStep) +
                       (d.start.y - p.y) / kSeqScrollYTrack.h * static_cast<float>(seqPitchList().size()));
        break;
    case SeqDrag::Mode::Audition:
        d.anchor = seqPitchAt(p.y);
        seqHear(d.anchor);
        break;
    case SeqDrag::Mode::None:
        break;
    }
}

void SeqPanel::drop(const DragState &d) {
    seqHear(-1);
    const SeqDrag::Mode mode = seqDrag_.mode;
    const bool moved = d.moved;
    if (seqDrag_.unpick && !moved) // a Shift+click on a note picked: no longer picked
        std::erase(seqSelection_, seqDrag_.anchor);
    if (mode == SeqDrag::Mode::LaneGroup && seqDrag_.narrow && !moved) // a click on one of several dots picked
        seqSelection_ = {seqDrag_.anchor};
    if (mode == SeqDrag::Mode::CurvePoint && !moved && (seqDrag_.curveUnpick || seqDrag_.narrow)) {
        // A point clicked: with Ctrl or Shift, one picked let go; without, one of several picked picked alone.
        std::vector<int> picked = seqCurvePicked();
        if (seqDrag_.curveUnpick)
            std::erase(picked, seqDrag_.anchor);
        else
            picked = {seqDrag_.anchor};
        setSeqCurvePick(std::move(picked));
    }
    if (mode == SeqDrag::Mode::Move || mode == SeqDrag::Mode::ResizeStart ||
        (mode == SeqDrag::Mode::Field && (seqDrag_.anchor == 0 || seqDrag_.anchor == 4)))
        seqDropCovered(seqPicked()); // in the drag's undo step, which closes after this
    if (seqDrag_.extend && moved) // a note extended backward may cover others
        seqDropCovered({seqDrag_.anchor});
    seqDrag_ = SeqDrag{};
}

void SeqPanel::release() { // a note still heard stops (a release that never came)
    seqHear(-1);
    seqDrag_ = SeqDrag{};
}

bool SeqPanel::doubleClick(const Widget &w, Point p, Modifiers mods) {
    const int pattern = seqPattern();
    switch (w.kind) {
    case Kind::SeqGrid: { // on a note, deletes it; elsewhere, puts one down
        if (const int hit = seqNoteAt(p); hit >= 0) {
            removeSeqNotes({hit});
            return true;
        }
        const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const int pitch = seqPitchAt(p.y);
        controller().beginStep(); // the note and its velocity, one undo step
        if (const int slot = addSeqNote(seqSnapped(seqStepAt(p.x), false), pitch, grid > 0.0 ? grid : 1.0); slot >= 0) {
            // Held, the mouse up and down sets its velocity, as Draw's click does; heard while the button is held, as a
            // note clicked is: until it comes up (see drop()).
            seqDrag_ = SeqDrag{};
            seqDrag_.mode = SeqDrag::Mode::Velocity;
            seqDrag_.anchor = slot;
            seqDrag_.extend = true;
            seqDrag_.grabStep = seqStepAt(p.x);
            for (const SeqNote &n : seqNotes())
                if (n.slot == slot)
                    seqDrag_.origin.push_back(n);
            seqDrag_.start = seqDrag_.now = p;
            // Its undo step open; and as the press it comes in place of left the drag (see View::doubleClick()), on
            // no widget.
            host_.startDrag(*this, Widget{}, p, true);
            seqHear(pitch);
        } else {
            controller().endStep();
        }
        return true;
    }
    case Kind::SeqRuler: // the loop over the whole pattern again
        controller().beginStep();
        editMany({{paramIndex(pattern, SeqPatternParam::LoopStart), 0.0}, {paramIndex(pattern, SeqPatternParam::LoopEnd), 0.0}});
        controller().endStep();
        return true;
    case Kind::SeqRoot:
        edit(w.param, table_[w.param].def);
        return true;
    case Kind::SeqLane: { // the notes there back to their velocity or chance to begin with
        if (seqCurveLane()) {
            const SeqCurve curve = seqLaneCurve();
            std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
            if (const int point = seqCurvePointAt(p); point >= 0) {
                if (point == seqCurveJustAdded_) { // the press before put it down: it stays
                    seqCurveJustAdded_ = -1;
                    return true;
                }
                points.erase(points.begin() + point);
            } else if (const int segment = seqCurveBendAt(p); segment >= 0) {
                points[static_cast<size_t>(segment)].curve = 0.0;
            } else { // elsewhere, a point put down there (on the grid with Ctrl)
                controller().beginStep();
                addSeqCurvePoint(curve, seqCurvePointFor(curve, p, mods.ctrl));
                controller().endStep();
                return true;
            }
            controller().beginStep();
            setSeqCurve(curve, points);
            controller().endStep();
            return true;
        }
        const bool velocity = controller().layout().seqLane == SeqLane::Velocity;
        std::vector<std::pair<int, double>> changes;
        for (const SeqNote &n : seqLaneNotesAt(p.x))
            changes.push_back({seqNoteIndex(pattern, n.slot, velocity ? SeqNoteParam::Velocity : SeqNoteParam::Chance),
                               velocity ? kNewVelocity : 1.0});
        controller().beginStep();
        editMany(changes);
        controller().endStep();
        return true;
    }
    case Kind::SeqZoom: // a click each
    case Kind::SeqKeys: // a key heard again, held, as fast as it is clicked (Windows sends the second click this way)
        return pressAs(w, p, {}, Widget{}); // as the press before left the drag (see above)
    case Kind::SeqField: { // the notes picked back to velocity 100, a full chance, or no offset off the grid
        const std::vector<int> picked = seqPicked();
        if (picked.empty() || (w.b != 1 && w.b != 2 && w.b != 4))
            return true;
        const double grid = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const double unit = grid > 0.0 ? grid : 1.0;
        std::vector<std::pair<int, double>> changes;
        for (int slot : picked) {
            if (w.b == 1)
                changes.push_back({seqNoteIndex(pattern, slot, SeqNoteParam::Velocity), kNewVelocity});
            else if (w.b == 2)
                changes.push_back({seqNoteIndex(pattern, slot, SeqNoteParam::Chance), 1.0});
            else {
                const int start = seqNoteIndex(pattern, slot, SeqNoteParam::Start);
                changes.push_back({start, std::round(value(start) / unit) * unit});
            }
        }
        controller().beginStep();
        editMany(changes);
        controller().endStep();
        return true;
    }
    case Kind::SeqTool:
    case Kind::SeqSnap:
    case Kind::SeqStretch:
    case Kind::SeqFlag:
        return true; // a double-click is two clicks of theirs, which the first press took
    default:
        return false;
    }
}

bool SeqPanel::wheelOn(const Widget &w, Point p, float steps, Modifiers mods) {
    UiLayout &layout = controller().layout();
    const bool roll = w.kind == Kind::SeqGrid || w.kind == Kind::SeqKeys || w.kind == Kind::SeqRuler ||
                      w.kind == Kind::SeqLane || w.kind == Kind::SeqScroll || w.kind == Kind::SeqScrollY ||
                      w.kind == Kind::SeqZoom || w.kind == Kind::SeqLaneTab;
    // Over the roll, Ctrl zooms across and Alt up and down, at the mouse; Shift scrolls across, as the wheel turned
    // sideways does.
    if (roll && mods.ctrl) {
        zoomSeq(true, std::pow(1.25f, steps), p);
        return true;
    }
    if (roll && mods.alt) {
        zoomSeq(false, std::pow(1.25f, steps), p);
        return true;
    }
    if (roll && (mods.shift || w.kind == Kind::SeqScroll || w.kind == Kind::SeqRuler)) {
        seqScrollX_ = seqScroll() - steps * seqShownSteps() * 0.125f;
        seqScrollX_ = seqScroll();
        return true;
    }
    switch (w.kind) {
    case Kind::SeqGrid:
    case Kind::SeqKeys:
    case Kind::SeqScrollY: { // the rows, a few at a time
        seqShowFromRow(static_cast<float>(seqFirstRow()) + std::round(steps * 3.0f)); // up shows higher rows
        return true;
    }
    case Kind::SeqField:
        controller().beginStep(true);
        stepSeqField(w.b, std::round(steps));
        if (w.b == 0 || w.b == 4) // its pitch or its place: what it lands on goes
            seqDropCovered(seqPicked());
        controller().endStep();
        return true;
    case Kind::SeqRoot:
        controller().beginStep(true);
        edit(w.param, value(w.param) + std::round(steps));
        controller().endStep();
        return true;
    case Kind::SeqSnap:
        layout.seqSnap = std::clamp(layout.seqSnap + (steps > 0.0f ? -1 : 1), 0, kSeqSnapCount - 1);
        return true;
    case Kind::SeqZoom: // its axis, in or out a notch
        zoomSeq(w.b < 2, std::pow(1.25f, steps), kSeqGrid.center());
        return true;
    case Kind::SeqLane:
    case Kind::SeqTool:
    case Kind::SeqStretch:
    case Kind::SeqFlag:
    case Kind::SeqClear:
    case Kind::SeqCopy:
    case Kind::SeqFold:
    case Kind::SeqLaneTab:
        return true;
    default:
        return false;
    }
}

bool SeqPanel::wheelAcross(Point p, float steps) {
    if (!(wide() && kSeqBox.contains(p)))
        return false;
    seqScrollX_ = seqScroll() + steps * seqShownSteps() * 0.125f; // the roll, an eighth of what it shows a notch
    seqScrollX_ = seqScroll();
    return true;
}

// The roll, across and up and down (one part, 0): what was under the mouse follows it, never held at an end.
std::optional<std::pair<int, float>> SeqPanel::panAt(Point p) const {
    if (wide() && Rect{kSeqBox.x, kSeqRuler.y, kSeqBox.w, kSeqLaneArea.y + kSeqLaneArea.h - kSeqRuler.y}.contains(p))
        return std::pair{0, seqScroll()};
    return std::nullopt;
}

void SeqPanel::panBegin(int) {
    seqPanStart_ = {seqScroll(), static_cast<float>(seqFirstRow())};
}

std::optional<float> SeqPanel::pan(int, float, float across, float down) {
    seqScrollX_ = seqPanStart_.x - across / kSeqGrid.w * seqShownSteps();
    seqScrollX_ = seqScroll();
    seqShowFromRow(seqPanStart_.y + down / seqRowH());
    return std::nullopt;
}

std::string SeqPanel::tooltip(const Widget &w) const {
    if ((w.kind == Kind::Choice || w.kind == Kind::Toggle) && isSeqParam(w.param)) { // the card's switches
        if (w.param == paramIndex(SeqParam::Enabled))
            return tr(Str::TipSeqPower);
        if (w.param == paramIndex(SeqParam::Pattern))
            return trf(Str::TipSeqPattern, {seqPatternName(w.b)});
        if (w.param == paramIndex(SeqParam::Play)) {
            const Str plays[] = {Str::HintSeqHost, Str::HintSeqKey, Str::HintSeqLatch};
            return tr(plays[std::clamp(w.b, 0, static_cast<int>(SeqPlay::Count) - 1)]);
        }
        if (w.param == paramIndex(SeqParam::Order)) {
            const Str orders[] = {Str::SeqOrderForward, Str::SeqOrderReverse, Str::SeqOrderPingPong, Str::SeqOrderRandom};
            return tr(orders[std::clamp(w.b, 0, static_cast<int>(SeqOrder::Count) - 1)]);
        }
    }
    switch (w.kind) {
    case Kind::SeqTool:
        return tr(w.b == 1 ? Str::TipSeqDraw : Str::TipSeqSelect);
    case Kind::SeqSnap:
        return seqSnapMenu_ ? std::string() : std::string(tr(Str::TipSeqSnap));
    case Kind::SeqStretch:
        return tr(w.b > 0 ? Str::TipSeqDouble : Str::TipSeqHalve);
    case Kind::SeqField:
        return trf(Str::TipSeqField, {tr(kFieldNames[std::clamp(w.b, 0, kFieldCount - 1)])});
    case Kind::SeqFlag:
        return tr(w.b == 0 ? Str::TipSeqSlide : Str::TipSeqMute);
    case Kind::SeqClear:
        return tr(Str::TipSeqClear);
    case Kind::SeqCopy:
        return tr(Str::TipSeqCopy);
    case Kind::SeqRoot:
        return tr(Str::TipSeqRoot);
    case Kind::SeqFold:
        return tr(Str::TipSeqFold);
    case Kind::SeqKeys:
        return tr(Str::TipSeqKeys);
    case Kind::SeqRuler:
        return tr(Str::TipSeqRuler);
    case Kind::SeqZoom: {
        const Str tips[] = {Str::TipSeqZoomOutX, Str::TipSeqZoomInX, Str::TipSeqZoomOutY, Str::TipSeqZoomInY};
        return tr(tips[std::clamp(w.b, 0, 3)]);
    }
    case Kind::SeqScroll:
        return tr(Str::TipSeqScroll);
    case Kind::SeqScrollY:
        return tr(Str::TipSeqScrollY);
    case Kind::SeqLaneTab:
        return w.b == static_cast<int>(SeqLane::Bend)       ? std::string(tr(Str::TipSeqLaneBend))
               : w.b == static_cast<int>(SeqLane::ModWheel) ? std::string(tr(Str::TipSeqLaneWheel))
                                                            : std::string();
    default:
        return {};
    }
}

// The keys and the ruler are tall or wide: their tooltips go under the mouse.
bool SeqPanel::tipAtMouse(const Widget &w) const {
    return w.kind == Kind::SeqKeys || w.kind == Kind::SeqRuler;
}

// --- Painting.

void SeqPanel::paintBack(Canvas &c) {
    const int pattern = seqPattern();
    const bool on = value(paramIndex(SeqParam::Enabled)) >= 0.5;
    // The card, as the inspector's lens.
    softShadow(c, kSeqCard, kRadiusPanel, 4, 1.25f, 4.0f, lightTheme() ? 0.05f : 0.16f);
    c.fillRect(kSeqCard, kRaised.alpha(0.92f), kRadiusPanel);
    if (outlined())
        c.strokeRect(kSeqCard, kAccent.alpha(0.35f), 1.0f, kRadiusPanel);
    c.text({kSeqCardX, kSeqTitleY, 200.0f, 22.0f}, tr(Str::HeadingSequencer), kTextBody, kText, Align::Left, true);
    auto label = [&](float y, Str name, float x = kSeqCardX, float w = 60.0f) {
        c.text({x, y, w, 22.0f}, tr(name), kTextLabel, kDim, Align::Left, true);
    };
    label(kSeqPatternY, Str::LabelSeqPattern);
    label(kSeqPlayY, Str::LabelSeqPlay);
    label(kSeqOrderY, Str::LabelSeqOrder);
    label(kSeqScaleY, Str::LabelSeqScale);
    label(kSeqTools.y + 1.0f, Str::LabelSeqSnap, kSeqTools.x + 70.0f, 36.0f);

    // The roll's box.
    c.fillRect(kSeqBox, wellColor(), kRadiusCard);
    c.pushClip(kSeqBox);
    const int steps = seqSteps();
    const std::vector<int> rows = seqRows();
    const float rh = seqRowH();
    const bool fold = controller().layout().seqFold;
    const int scale = stepped(value(paramIndex(SeqParam::Scale)), 0, static_cast<int>(SeqScale::Count) - 1);
    const int root = stepped(value(paramIndex(SeqParam::Root)), 0, 127);
    const Color light = lightTheme() ? Color{0.0f, 0.0f, 0.0f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 1.0f};
    // How strongly the grid's lines show: the GRID setting, its 50 % the lines as they were drawn before it could be set.
    const float gridLines = static_cast<float>(std::clamp(host_.shownGrid(), 0.0f, 1.0f)) * 2.0f;
    for (size_t i = 0; i < rows.size(); ++i) {
        const int pitch = rows[i];
        const float y = kSeqGrid.y + kSeqGrid.h - (i + 1) * rh;
        // The rows' ground follows the keys alone, whatever the scale: a black key's darker, a white key's a little
        // lighter than the well; folded, the well as it is.
        if (!fold)
            c.fillRect({kSeqGrid.x, y, kSeqGrid.w, rh},
                       isBlackKey(pitch) ? kBackground.alpha(0.35f)
                                         : Color{1.0f, 1.0f, 1.0f, lightTheme() ? 0.4f : 0.03f});
        c.line({kSeqGrid.x, y + rh}, {kSeqGrid.x + kSeqGrid.w, y + rh},
               light.alpha((pitch % 12 == 0 ? 0.10f : 0.035f) * gridLines), 1.0f);
    }
    const float stepW = kSeqGrid.w / seqShownSteps(); // a step's width, as zoomed
    const int firstStep = static_cast<int>(std::floor(seqScroll()));
    const int lastStep = std::min(static_cast<int>(std::ceil(seqScroll() + seqShownSteps())), steps);
    c.pushClip({kSeqGrid.x, kSeqRuler.y, kSeqGrid.w, kSeqLaneArea.y + kSeqLaneArea.h - kSeqRuler.y});
    // A line at every snap (a step with it off; for triplets, a third of two steps), the beats and bars stronger
    // where they fall; lines closer than 4 px left out but for the beats and bars.
    {
        const double snap = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const double every = snap > 0.0 ? snap : 1.0;
        const bool fine = every * stepW >= 4.0f;
        for (long i = static_cast<long>(std::floor(seqScroll() / every)); i * every <= lastStep + 1e-9; ++i) {
            const double s = i * every;
            const bool beat = std::abs(s / 4.0 - std::round(s / 4.0)) < 1e-6;
            const bool bar = std::abs(s / kSeqStepsPerBar - std::round(s / kSeqStepsPerBar)) < 1e-6;
            if (!fine && !beat)
                continue;
            const float alpha = bar ? 0.16f : beat ? 0.08f : 0.03f;
            const float x = seqStepX(s);
            c.line({x, kSeqRuler.y}, {x, kSeqLaneArea.y + kSeqLaneArea.h}, light.alpha(alpha * gridLines), 1.0f);
        }
        // Beats the snap's lines miss (a triplet's), drawn all the same.
        for (int s = (firstStep / 4) * 4; s <= lastStep; s += 4)
            if (std::abs(s / every - std::round(s / every)) > 1e-6)
                c.line({seqStepX(s), kSeqRuler.y}, {seqStepX(s), kSeqLaneArea.y + kSeqLaneArea.h},
                       light.alpha((s % kSeqStepsPerBar == 0 ? 0.16f : 0.08f) * gridLines), 1.0f);
    }
    // The loop: what lies outside it dimmed, and its bar on the ruler.
    const int loopStart = stepped(value(paramIndex(pattern, SeqPatternParam::LoopStart)), 0, steps - 1);
    const int storedEnd = stepped(value(paramIndex(pattern, SeqPatternParam::LoopEnd)), 0, kSeqMaxSteps);
    const int loopEnd = storedEnd <= 0 ? steps : std::clamp(storedEnd, loopStart + 1, steps);
    const float lx0 = seqStepX(loopStart), lx1 = seqStepX(loopEnd);
    if (lx0 > kSeqGrid.x)
        c.fillRect({kSeqGrid.x, kSeqGrid.y, lx0 - kSeqGrid.x, kSeqGrid.h + kSeqLaneH}, kBackground.alpha(0.45f));
    if (lx1 < kSeqGrid.x + kSeqGrid.w)
        c.fillRect({lx1, kSeqGrid.y, kSeqGrid.x + kSeqGrid.w - lx1, kSeqGrid.h + kSeqLaneH}, kBackground.alpha(0.45f));
    c.fillRect({kSeqRuler.x, kSeqRuler.y, kSeqRuler.w, kSeqRuler.h}, kPanel.alpha(0.6f));
    c.fillRect({lx0, kSeqRuler.y + 3.0f, lx1 - lx0, 5.0f}, kAccent.alpha(on ? 0.85f : 0.4f), kRadiusMark);
    for (int bar = 0; bar < steps / kSeqStepsPerBar; ++bar)
        for (int beat = 0; beat < 4; ++beat) {
            const float x = seqStepX(bar * kSeqStepsPerBar + beat * 4);
            if (x < kSeqGrid.x - 40.0f || x > kSeqGrid.x + kSeqGrid.w || (beat > 0 && stepW * 4.0f < 30.0f))
                continue; // out of sight, or no room for the beats' numbers
            const std::string text = beat == 0 ? std::to_string(bar + 1)
                                               : std::to_string(bar + 1) + "." + std::to_string(beat + 1);
            c.text({x + 3.0f, kSeqRuler.y + 7.0f, 40.0f, 13.0f}, text, kTextSmall, beat == 0 ? kDim : kFaint, Align::Left);
        }
    c.popClip();

    // The notes: brighter as they are louder, dashed at a chance below full, hollow while muted; those picked ringed.
    const std::vector<SeqNote> notes = seqNotes();
    const Color ink = kAccent;
    c.pushClip(kSeqGrid);
    for (const SeqNote &n : notes) {
        const Rect r = seqNoteRect(n);
        if (r.y > kSeqGrid.y + kSeqGrid.h)
            continue; // its row isn't shown
        const bool picked = seqSelected(n.slot);
        // Full strength at full velocity, down to 40 % at none; a little dimmer while the sequencer is off.
        const float level = static_cast<float>(0.4 + 0.6 * std::clamp(n.velocity, 0.0, 1.0)) * (on ? 1.0f : 0.75f);
        if (const float glow = seqGlow(n); glow > 0.0f) // played just now: glowing, as the glow setting says
            featherRect(c, r, kRadiusMark, 8.0f, ink.alpha(0.6f * glow));
        if (n.mute) {
            c.strokeRect(r, kFaint, 1.0f, kRadiusMark, true);
        } else if (n.chance < 1.0) {
            c.fillRect(r, ink.alpha(level * 0.35f), kRadiusMark);
            c.strokeRect(r, ink.alpha(level), 1.0f, kRadiusMark, true);
        } else {
            c.fillRect(r, ink.alpha(level), kRadiusMark);
        }
        if (scale != 0 && !seqInScale(scale, root, n.pitch)) { // off the scale: hatched across
            c.pushClip(r);
            const Color hatch = kBackground.alpha(0.55f);
            for (float x = r.x - r.h; x < r.right(); x += 5.0f)
                c.line({x, r.bottom()}, {x + r.h, r.y}, hatch, 1.5f);
            c.popClip();
        }
        if (picked) {
            c.strokeRect(r.inset(-1.0f), kText, 1.5f, kRadiusMark + 1.0f);
            c.fillRect({r.right() - 3.0f, r.y + 2.0f, 2.0f, r.h - 4.0f}, kText, 1.0f);
            if (r.w >= 12.0f) // and at its start, room allowing
                c.fillRect({r.x + 1.0f, r.y + 2.0f, 2.0f, r.h - 4.0f}, kText, 1.0f);
        }
        if (n.slide) // a hook off its end, into the next note
            c.curve({r.right(), r.center().y}, {r.right() + 4.0f, r.center().y}, {r.right() + 6.0f, r.y - 2.0f},
                    kText.alpha(0.8f), 1.2f);
        if (n.chance < 1.0 && r.w > 26.0f && rh >= 9.0f)
            c.text({r.x + 3.0f, r.y, r.w - 4.0f, r.h}, std::to_string(std::lround(n.chance * 100.0)) + "%",
                   kTextSmall, kText, Align::Left);
    }
    // The box being dragged, picking what it reaches.
    if (dragging() && seqDrag_.mode == SeqDrag::Mode::Box) {
        const Rect box{std::min(seqDrag_.start.x, seqDrag_.now.x), std::min(seqDrag_.start.y, seqDrag_.now.y),
                       std::abs(seqDrag_.now.x - seqDrag_.start.x), std::abs(seqDrag_.now.y - seqDrag_.start.y)};
        c.fillRect(box, kAccent.alpha(0.12f));
        c.strokeRect(box, kAccent.alpha(0.7f), 1.0f);
    }
    c.popClip();
    if (notes.empty())
        c.text(kSeqGrid, tr(Str::HintSeqEmpty), kTextLabel, kFaint);

    // The keys: white ones across the strip, the black ones shorter over them; or with the rows folded to the scale,
    // each row's name.
    c.pushClip(kSeqKeys);
    if (fold) {
        c.fillRect(kSeqKeys, kPanel);
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = kSeqKeys.y + kSeqKeys.h - (i + 1) * rh;
            const bool outside = scale != 0 && !seqInScale(scale, root, rows[i]); // a row a note off the scale has
            if (outside)
                c.fillRect({kSeqKeys.x, y, kSeqKeys.w, rh}, kBackground.alpha(0.55f));
            c.text({kSeqKeys.x, y, kSeqKeys.w - 6.0f, rh}, pitchName(rows[i]), kTextSmall,
                   outside ? kFaint : rows[i] % 12 == root % 12 ? kText : kDim, Align::Right);
        }
    } else {
        const Color white = lightTheme() ? Color::hex(0xFFFFFF) : Color::hex(0xD6D9E4);
        const Color blackKey = Color::hex(0x1B1C25);
        const Color seam = Color::hex(0x8E92A6);
        c.fillRect(kSeqKeys, white);
        const float bw = kSeqKeys.w * 0.6f;
        for (size_t i = 0; i < rows.size(); ++i) {
            const int pitch = rows[i];
            const float y = kSeqKeys.y + kSeqKeys.h - (i + 1) * rh;
            const int pc = pitch % 12;
            if (pc == 4 || pc == 11) // the seams between two white keys side by side
                c.line({kSeqKeys.x, y}, {kSeqKeys.x + kSeqKeys.w, y}, seam, 1.0f);
            if (isBlackKey(pitch)) {
                c.line({kSeqKeys.x + bw, y + rh * 0.5f}, {kSeqKeys.x + kSeqKeys.w, y + rh * 0.5f}, seam, 1.0f);
                c.fillRect({kSeqKeys.x - 3.0f, y + 0.6f, bw + 3.0f, rh - 1.2f}, blackKey, 2.0f);
            }
            if (pc == 0)
                c.text({kSeqKeys.x, y, kSeqKeys.w - 4.0f, rh}, pitchName(pitch), kTextSmall, Color::hex(0x5D6075),
                       Align::Right, true);
        }
    }
    // The keys sounding: those whose notes the sequencer plays now, as written, and the one heard while it is held,
    // each lit as far as it has eased (see seqFollowKeys()).
    for (size_t i = 0; i < rows.size(); ++i) {
        const float lit = seqKeyLight_[static_cast<size_t>(std::clamp(rows[i], 0, 127))];
        if (lit > 0.0f)
            c.fillRect({kSeqKeys.x, kSeqKeys.y + kSeqKeys.h - (i + 1) * rh, kSeqKeys.w, rh},
                       kAccent.alpha(0.6f * lit));
    }
    c.popClip();
    c.line({kSeqKeys.x + kSeqKeys.w, kSeqKeys.y}, {kSeqKeys.x + kSeqKeys.w, kSeqKeys.y + kSeqKeys.h}, kBorder, 1.0f);

    // The lane: a stem for each note at its start, as high as its velocity or chance.
    const bool velocityLane = controller().layout().seqLane == SeqLane::Velocity;
    c.line({kSeqBox.x, kSeqLaneArea.y}, {kSeqBox.x + kSeqBox.w, kSeqLaneArea.y}, kBorder, 1.0f);
    const Color stem = velocityLane ? kAccent : look.sources[1];
    c.pushClip(kSeqLaneArea);
    std::vector<SeqNote> stems = notes; // the picked ones last, over the others
    std::stable_partition(stems.begin(), stems.end(), [&](const SeqNote &n) { return !seqSelected(n.slot); });
    if (seqCurveLane()) {
        // A curve instead: the line it rests on (the bend's 0, the mod wheel's bottom), the curve across the pattern,
        // filled down to that line, its points and the middles of the segments that bend.
        stems.clear();
        const SeqCurve curve = seqLaneCurve();
        const Color curveInk = look.sources[curve == SeqCurve::Bend ? kBendSource : kWheelSource];
        const float rest = seqCurveY(curve, 0.0);
        const float right = std::min(kSeqGrid.x + kSeqGrid.w, seqStepX(steps));
        // The grid: a line at each quarter of the range (what Ctrl puts points on), as strong as the GRID setting says;
        // across, the roll's lines run on down through the lane.
        for (int k = 0; k <= 4; ++k) {
            const float y = seqCurveY(curve, curve == SeqCurve::Bend ? -1.0 + k * 0.5 : k * 0.25);
            c.line({kSeqGrid.x, y}, {right, y}, light.alpha(0.05f * gridLines), 1.0f);
        }
        c.line({kSeqGrid.x, rest}, {right, rest}, kBorder, 1.0f);
        const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        if (!points.empty() && right > kSeqGrid.x) {
            std::vector<Point> line;
            for (float x = kSeqGrid.x;; x += 2.0f) {
                const float at = std::min(x, right);
                const double v = seqCurveAt(static_cast<int>(points.size()),
                                            [&](int i) { return points[static_cast<size_t>(i)]; }, seqStepAt(at));
                line.push_back({at, seqCurveY(curve, v)});
                if (at >= right)
                    break;
            }
            std::vector<Point> area = line;
            area.push_back({line.back().x, rest});
            area.push_back({line.front().x, rest});
            c.fillPolygon(area.data(), area.size(), curveInk.alpha(0.16f));
            c.polyline(line.data(), line.size(), curveInk.alpha(on ? 1.0f : 0.6f), 1.5f);
            const std::vector<int> picked = seqCurvePicked();
            const int hoveredPoint = dragging() && seqDrag_.mode == SeqDrag::Mode::CurvePoint
                                         ? seqDrag_.anchor
                                         : seqCurvePointAt(host_.mouse());
            const int hoveredBend = dragging() && seqDrag_.mode == SeqDrag::Mode::CurveBend
                                        ? seqDrag_.anchor
                                        : seqCurveBendAt(host_.mouse());
            for (int i = 0; i + 1 < static_cast<int>(points.size()); ++i) {
                const SeqCurvePoint &a = points[static_cast<size_t>(i)], &b = points[static_cast<size_t>(i) + 1];
                if (seqStepX(b.x) - seqStepX(a.x) >= 20.0f && std::abs(b.y - a.y) >= 1e-6)
                    c.strokeCircle(seqCurveBendPos(points, curve, i), i == hoveredBend ? 4.0f : 2.5f,
                                   curveInk.alpha(i == hoveredBend ? 1.0f : 0.7f), 1.2f);
            }
            for (int i = 0; i < static_cast<int>(points.size()); ++i) {
                const SeqCurvePoint &q = points[static_cast<size_t>(i)];
                const Point at{seqStepX(q.x), seqCurveY(curve, q.y)};
                c.fillCircle(at, 3.5f, curveInk);
                if (i == hoveredPoint)
                    c.strokeCircle(at, 6.0f, kText, 1.5f);
                else if (std::find(picked.begin(), picked.end(), i) != picked.end()) // picked: ringed in its color
                    c.strokeCircle(at, 6.0f, curveInk, 1.5f);
            }
        }
        // The box being dragged across the lane, picking the points it reaches.
        if (dragging() && seqDrag_.mode == SeqDrag::Mode::CurveBox) {
            const Rect box{std::min(seqDrag_.start.x, seqDrag_.now.x), std::min(seqDrag_.start.y, seqDrag_.now.y),
                           std::abs(seqDrag_.now.x - seqDrag_.start.x), std::abs(seqDrag_.now.y - seqDrag_.start.y)};
            c.fillRect(box, curveInk.alpha(0.12f));
            c.strokeRect(box, curveInk.alpha(0.7f), 1.0f);
        }
    }
    for (const SeqNote &n : stems) {
        const Point dot = seqLaneDot(n);
        const float bottom = kSeqLaneArea.y + kSeqLaneArea.h - 6.0f;
        const bool picked = seqSelected(n.slot);
        c.line({dot.x, bottom}, dot, stem.alpha(picked ? 1.0f : 0.6f), 2.0f);
        c.fillCircle(dot, picked ? 3.5f : 3.0f, stem.alpha(picked ? 1.0f : 0.8f));
        if (picked) // ringed, as the note is
            c.strokeCircle(dot, 6.0f, kText, 1.5f);
    }
    // The box being dragged across the lane, picking the notes whose dots it holds.
    if (dragging() && seqDrag_.mode == SeqDrag::Mode::LaneBox) {
        const Rect box{std::min(seqDrag_.start.x, seqDrag_.now.x), std::min(seqDrag_.start.y, seqDrag_.now.y),
                       std::abs(seqDrag_.now.x - seqDrag_.start.x), std::abs(seqDrag_.now.y - seqDrag_.start.y)};
        c.fillRect(box, stem.alpha(0.12f));
        c.strokeRect(box, stem.alpha(0.7f), 1.0f);
    }
    c.popClip();
    c.line({kSeqBox.x, kSeqLaneTabs.y}, {kSeqBox.x + kSeqBox.w, kSeqLaneTabs.y}, kBorder, 1.0f);
    // The zoom's arrows, before their buttons: across, and up and down.
    {
        const Color arrowInk = kFaint;
        const float y = kSeqLaneTabs.y + kSeqLaneTabs.h * 0.5f;
        const float ax = seqZoomButton(0).x - 12.0f;
        c.line({ax - 6.0f, y}, {ax + 6.0f, y}, arrowInk, 1.2f);
        const Point l[] = {{ax - 3.0f, y - 3.0f}, {ax - 6.0f, y}, {ax - 3.0f, y + 3.0f}};
        const Point r[] = {{ax + 3.0f, y - 3.0f}, {ax + 6.0f, y}, {ax + 3.0f, y + 3.0f}};
        c.polyline(l, 3, arrowInk, 1.2f);
        c.polyline(r, 3, arrowInk, 1.2f);
        const float vx = seqZoomButton(2).x - 10.0f;
        c.line({vx, y - 6.0f}, {vx, y + 6.0f}, arrowInk, 1.2f);
        const Point u[] = {{vx - 3.0f, y - 3.0f}, {vx, y - 6.0f}, {vx + 3.0f, y - 3.0f}};
        const Point d[] = {{vx - 3.0f, y + 3.0f}, {vx, y + 6.0f}, {vx + 3.0f, y + 3.0f}};
        c.polyline(u, 3, arrowInk, 1.2f);
        c.polyline(d, 3, arrowInk, 1.2f);
    }

    // Where it plays now, while it plays this pattern.
    const Telemetry &telemetry = host_.telemetry();
    if (telemetry.seqPosition >= 0.0f && telemetry.seqPattern == pattern &&
        seqStepX(telemetry.seqPosition) >= kSeqGrid.x && seqStepX(telemetry.seqPosition) <= kSeqGrid.x + kSeqGrid.w) {
        const float x = seqStepX(telemetry.seqPosition);
        c.line({x, kSeqRuler.y}, {x, kSeqLaneArea.y + kSeqLaneArea.h}, kText.alpha(0.8f), 1.2f);
        const Point head[] = {{x - 5.0f, kSeqRuler.y + 9.0f}, {x + 5.0f, kSeqRuler.y + 9.0f}, {x, kSeqRuler.y + 15.0f}};
        c.fillPolygon(head, 3, kText);
    }
    c.popClip();
}

Point SeqPanel::seqLaneDot(const SeqNote &note) const {
    const double v = controller().layout().seqLane == SeqLane::Velocity ? note.velocity : note.chance;
    const float bottom = kSeqLaneArea.y + kSeqLaneArea.h - 6.0f;
    return {seqStepX(note.start) + 1.5f, bottom - static_cast<float>(std::clamp(v, 0.0, 1.0)) * (kSeqLaneArea.h - 12.0f)};
}

int SeqPanel::seqLaneNoteAt(Point p) const {
    const std::vector<SeqNote> notes = seqNotes();
    int found = -1;
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const Point dot = seqLaneDot(*it);
        if (std::abs(p.x - dot.x) <= 5.0f && p.y >= dot.y - 6.0f && p.y <= kSeqLaneArea.y + kSeqLaneArea.h) {
            if (seqSelected(it->slot))
                return it->slot;
            if (found < 0)
                found = it->slot;
        }
    }
    return found;
}

std::vector<SeqPanel::SeqNote> SeqPanel::seqLaneNotesAt(float x) const {
    std::vector<SeqNote> at, picked;
    const float reach = std::max(3.0f, kSeqGrid.w / seqShownSteps() * 0.5f);
    for (const SeqNote &n : seqNotes())
        if (std::abs(seqStepX(n.start) - x) <= reach) {
            at.push_back(n);
            if (seqSelected(n.slot))
                picked.push_back(n);
        }
    // Of the notes starting together, the picked ones alone while any is picked.
    std::vector<SeqNote> out;
    for (const SeqNote &n : at) {
        const bool together = std::any_of(picked.begin(), picked.end(),
                                          [&](const SeqNote &q) { return std::abs(q.start - n.start) < 1e-6; });
        if (!together || seqSelected(n.slot))
            out.push_back(n);
    }
    return out;
}

std::pair<int, int> SeqPanel::seqLoop() const {
    const int pattern = seqPattern(), steps = seqSteps();
    const int start = stepped(value(paramIndex(pattern, SeqPatternParam::LoopStart)), 0, steps - 1);
    const int stored = stepped(value(paramIndex(pattern, SeqPatternParam::LoopEnd)), 0, kSeqMaxSteps);
    return {start, stored <= 0 ? steps : std::clamp(stored, start + 1, steps)};
}

SeqPanel::SeqDrag::Mode SeqPanel::seqRulerMode(float x) const {
    const auto [start, end] = seqLoop();
    const float x0 = seqStepX(start), x1 = seqStepX(end);
    if (std::abs(x - x1) <= 6.0f)
        return SeqDrag::Mode::LoopEnd;
    if (std::abs(x - x0) <= 6.0f)
        return SeqDrag::Mode::LoopStart;
    return x > x0 && x < x1 ? SeqDrag::Mode::LoopMove : SeqDrag::Mode::Loop;
}

std::optional<CursorKind> SeqPanel::cursor(const Widget &w) const {
    if (w.kind == Kind::SeqRuler) { // left and right over an end of the loop or a new one, moving over the loop
        const SeqDrag::Mode mode = dragging() ? seqDrag_.mode : seqRulerMode(host_.mouse().x);
        return mode == SeqDrag::Mode::LoopMove ? CursorKind::Move : CursorKind::Horizontal;
    }
    if (w.kind == Kind::SeqLane && seqCurveLane()) { // moving over a point, up and down over a bend, a pen drawing
        if (dragging())
            return seqDrag_.mode == SeqDrag::Mode::CurveBend    ? CursorKind::Vertical
                   : seqDrag_.mode == SeqDrag::Mode::CurvePoint ? CursorKind::Move
                   : seqDrag_.mode == SeqDrag::Mode::CurveBox   ? CursorKind::Arrow
                                                                : CursorKind::Pen;
        if (seqCurvePointAt(host_.mouse()) >= 0)
            return CursorKind::Move;
        if (seqCurveBendAt(host_.mouse()) >= 0)
            return CursorKind::Vertical;
        return controller().layout().seqDraw ? std::optional<CursorKind>(CursorKind::Pen) : std::nullopt;
    }
    if (w.kind == Kind::SeqLane) { // up and down over a dot, or moving them; with Draw a pen elsewhere, drawing heights
        if (dragging())
            return seqDrag_.mode == SeqDrag::Mode::LaneBox                                ? CursorKind::Arrow
                   : seqDrag_.mode == SeqDrag::Mode::LaneGroup || seqDrag_.anchor >= 0 ? CursorKind::Vertical
                                                                                         : CursorKind::Pen;
        if (seqLaneNoteAt(host_.mouse()) >= 0)
            return CursorKind::Vertical;
        return controller().layout().seqDraw ? CursorKind::Pen : CursorKind::Arrow;
    }
    if (w.kind != Kind::SeqGrid)
        return std::nullopt;
    if (dragging()) {
        switch (seqDrag_.mode) {
        case SeqDrag::Mode::Paint:
            return CursorKind::Pen;
        case SeqDrag::Mode::Velocity:
            return CursorKind::Vertical;
        case SeqDrag::Mode::Move:
            return CursorKind::Move;
        case SeqDrag::Mode::Resize:
        case SeqDrag::Mode::ResizeStart:
            return CursorKind::Horizontal;
        default:
            return CursorKind::Arrow;
        }
    }
    // Over a note, what a drag does to it: its end stretches it, the rest moves it; elsewhere a pen while drawing.
    bool atEnd = false, atStart = false;
    if (seqNoteAt(host_.mouse(), &atEnd, &atStart) >= 0)
        return atEnd || atStart ? CursorKind::Horizontal : CursorKind::Move;
    return controller().layout().seqDraw ? CursorKind::Pen : CursorKind::Arrow;
}

bool SeqPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const UiLayout &layout = controller().layout();
    auto iconButton = [&](bool lit) { // raised, outlined where controls are, as the FX panel's bin
        c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
        if (outlined())
            c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
    };
    if ((w.kind == Kind::Choice || w.kind == Kind::Toggle) && isSeqParam(w.param)) {
        const bool on = w.kind == Kind::Choice ? std::lround(value(w.param)) == w.b : value(w.param) >= 0.5;
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        const Color ink = on ? litInk(kAccent) : hovered ? kText : kDim;
        if (w.param == paramIndex(SeqParam::Enabled)) {
            powerIcon(c, w.rect.center(), ink);
        } else if (w.param == paramIndex(SeqParam::Order)) { // arrows: on, back, to and fro, and a die's dots
            const Point m = w.rect.center();
            const auto order = static_cast<SeqOrder>(w.b);
            auto arrow = [&](float y, bool right) {
                const float d = right ? 1.0f : -1.0f;
                c.line({m.x - 6.0f * d, y}, {m.x + 6.0f * d, y}, ink, 1.4f);
                const Point head[] = {{m.x + 2.0f * d, y - 3.5f}, {m.x + 6.0f * d, y}, {m.x + 2.0f * d, y + 3.5f}};
                c.polyline(head, 3, ink, 1.4f);
            };
            if (order == SeqOrder::Forward || order == SeqOrder::Reverse)
                arrow(m.y, order == SeqOrder::Forward);
            else if (order == SeqOrder::PingPong) {
                arrow(m.y - 3.0f, true);
                arrow(m.y + 3.0f, false);
            } else
                for (const Point d : {Point{-4.0f, -4.0f}, Point{4.0f, -4.0f}, Point{0.0f, 0.0f}, Point{-4.0f, 4.0f},
                                      Point{4.0f, 4.0f}})
                    c.fillCircle({m.x + d.x, m.y + d.y}, 1.4f, ink);
        } else {
            const Str plays[] = {Str::SeqPlayHost, Str::SeqPlayKey, Str::SeqPlayLatch};
            const std::string text = w.param == paramIndex(SeqParam::Pattern)
                                         ? seqPatternName(w.b)
                                         : caps(tr(plays[std::clamp(w.b, 0, 2)]));
            c.text(w.rect, text, kTextLabel, on ? litInk(kText) : kDim, Align::Center, on);
        }
        return true;
    }
    switch (w.kind) {
    case Kind::SeqTool: {
        const bool on = layout.seqDraw == (w.b == 1);
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        const Color ink = on ? litInk(kAccent) : hovered ? kText : kDim;
        const Point m = w.rect.center();
        if (w.b == 0) { // a dashed box, picking
            c.strokeRect({m.x - 6.0f, m.y - 5.0f, 12.0f, 10.0f}, ink, 1.2f, 1.0f, true);
        } else { // a pencil
            c.line({m.x - 5.0f, m.y + 5.0f}, {m.x + 4.0f, m.y - 4.0f}, ink, 3.0f);
            c.line({m.x - 6.0f, m.y + 6.0f}, {m.x - 4.0f, m.y + 4.0f}, ink, 1.2f);
        }
        return true;
    }
    case Kind::SeqSnap: {
        const bool lit = hovered || seqSnapMenu_;
        c.fillRect(w.rect, look.flat ? wellColor() : (lit ? kAccent.alpha(0.18f) : kRaised), kRadiusControl);
        if (!look.flat)
            c.strokeRect(w.rect, kAccent.alpha(lit ? 0.9f : 0.45f), 1.0f, kRadiusControl);
        const int snap = std::clamp(layout.seqSnap, 0, kSeqSnapCount - 1);
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 26.0f, w.rect.h},
               kSeqSnapNames[snap] ? kSeqSnapNames[snap] : tr(Str::ValueOff), kTextBody, lit ? kText : kDim, Align::Left);
        const Point m{w.rect.x + w.rect.w - 12.0f, w.rect.center().y};
        const Point chevron[] = {{m.x - 3.5f, m.y - 2.0f}, {m.x, m.y + 2.0f}, {m.x + 3.5f, m.y - 2.0f}};
        c.polyline(chevron, 3, lit ? kText : kDim, 1.3f);
        return true;
    }
    case Kind::SeqSnapMenu:
        if (w.a == 1)
            paintMenuPanel(c, w.rect);
        return true;
    case Kind::SeqSnapItem: {
        const bool selected = layout.seqSnap == w.b;
        paintMenuRow(c, w.rect, hovered, selected);
        c.text({w.rect.x + 10.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h},
               kSeqSnapNames[w.b] ? kSeqSnapNames[w.b] : tr(Str::ValueOff), kTextBody, hovered || selected ? kText : kDim,
               Align::Left, selected);
        return true;
    }
    case Kind::SeqStretch:
        iconButton(hovered);
        c.text(w.rect, w.b > 0 ? "\xC3\x97" "2" : "\xC3\xB7" "2", kTextLabel, hovered ? kText : kDim, Align::Center, true);
        return true;
    case Kind::SeqField: {
        const bool any = !seqPicked().empty();
        // One well for the five, its corners round at its ends: each field its own part of it, filled once (a light
        // theme's well is a shade, which two fills over each other would darken).
        const Rect well{seqFieldRect(0).x, w.rect.y, seqFieldRect(kFieldCount - 1).right() - seqFieldRect(0).x, w.rect.h};
        c.pushClip(w.rect);
        c.fillRect(well, hovered && any ? wellHover() : wellColor(), kRadiusControl);
        c.popClip();
        const char *name = tr(kFieldNames[w.b]);
        const float nameW = c.textWidth(name, kTextSmall, true) + 14.0f;
        c.text({w.rect.x + 8.0f, w.rect.y, nameW, w.rect.h}, name, kTextSmall, kDim, Align::Left, true);
        c.text({w.rect.x + 8.0f + nameW - 6.0f, w.rect.y, w.rect.w - nameW - 2.0f, w.rect.h}, seqFieldText(w.b),
               kTextBody, !any ? kFaint : w.b == 0 ? kAccent : kText, Align::Left, w.b == 0);
        if (w.b == 1)
            c.line({w.rect.x, w.rect.y + 6.0f}, {w.rect.x, w.rect.bottom() - 6.0f}, kBorder, 1.0f);
        return true;
    }
    case Kind::SeqFlag: {
        const std::vector<int> picked = seqPicked();
        const SeqNoteParam field = w.b == 0 ? SeqNoteParam::Slide : SeqNoteParam::Mute;
        const bool on = !picked.empty() && std::all_of(picked.begin(), picked.end(), [&](int slot) {
            return value(seqNoteIndex(seqPattern(), slot, field)) >= 0.5;
        });
        host_.paintButtonFace(c, w, on, hovered && !picked.empty(), kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, tr(w.b == 0 ? Str::SwitchSlide : Str::SwitchMute), kTextLabel,
               on ? litInk(kText) : picked.empty() ? kFaint : kDim, Align::Center, on);
        return true;
    }
    case Kind::SeqClear: {
        const bool any = !seqPatternEmpty(seqPattern());
        iconButton(hovered && any);
        binIcon(c, w.rect.center(), !any ? kFaint : hovered ? kText : kDim);
        return true;
    }
    case Kind::SeqCopy: { // two sheets, one over the other
        iconButton(hovered);
        const Point m = w.rect.center();
        const Color ink = hovered ? kText : kDim;
        c.strokeRect({m.x - 5.0f, m.y - 5.0f, 8.0f, 9.0f}, ink, 1.2f, 1.5f);
        c.fillRect({m.x - 2.0f, m.y - 2.0f, 8.0f, 9.0f}, hovered ? kTrack : kRaised, 1.5f);
        c.strokeRect({m.x - 2.0f, m.y - 2.0f, 8.0f, 9.0f}, ink, 1.2f, 1.5f);
        return true;
    }
    case Kind::SeqRoot: {
        const bool lit = hovered || host_.isActive(w);
        c.fillRect(w.rect, lit ? wellHover() : wellColor(), kRadiusControl);
        c.text(w.rect, pitchName(stepped(value(w.param), 0, 127)), kTextBody, lit ? kText : kDim, Align::Center);
        return true;
    }
    case Kind::SeqFold: { // a well while off, as the root's beside it
        if (layout.seqFold)
            host_.paintButtonFace(c, w, true, hovered, kAccent, 0.22f, kRadiusControl);
        else
            c.fillRect(w.rect, hovered ? wellHover() : wellColor(), kRadiusControl);
        c.text(w.rect, tr(Str::SwitchFold), kTextLabel, layout.seqFold ? litInk(kText) : kDim, Align::Center,
               layout.seqFold);
        return true;
    }
    case Kind::SeqLaneTab: {
        const bool on = static_cast<int>(layout.seqLane) == w.b;
        if (on)
            c.fillRect(w.rect, kAccent.alpha(0.2f), kRadiusMark);
        const Str names[] = {Str::SeqLaneVelocity, Str::SeqLaneChance, Str::SeqLaneBend, Str::SeqLaneWheel};
        c.text(w.rect, tr(names[std::clamp(w.b, 0, static_cast<int>(SeqLane::Count) - 1)]), kTextSmall,
               on ? kText : hovered ? kText : kDim, Align::Center, true);
        return true;
    }
    case Kind::SeqZoom: {
        // In a well in a flat theme, whose raised face doesn't show, so they don't read as the arrows beside them.
        if (look.flat)
            c.fillRect(w.rect, hovered ? wellHover() : wellColor(), kRadiusControl);
        else
            iconButton(hovered);
        const Point m = w.rect.center();
        const Color ink = hovered ? kText : kDim;
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, ink, 1.4f);
        if (w.b % 2 == 1)
            c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, ink, 1.4f);
        return true;
    }
    case Kind::SeqScroll:
    case Kind::SeqScrollY: {
        const bool across = w.kind == Kind::SeqScroll;
        const Rect track = across ? seqScrollTrack() : kSeqScrollYTrack;
        const Rect thumb = across ? seqScrollThumb() : seqScrollYThumb();
        const bool lit = hovered || (dragging() &&
                                     seqDrag_.mode == (across ? SeqDrag::Mode::Scroll : SeqDrag::Mode::ScrollY));
        c.fillRect(track, kTrack, kRadiusMark);
        c.fillRect(thumb, lit ? kText : kDim, kRadiusMark);
        return true;
    }
    case Kind::SeqRuler:
    case Kind::SeqKeys:
    case Kind::SeqGrid:
    case Kind::SeqLane:
        return true; // drawn with the roll (see paintBack())
    default:
        return false;
    }
}

} // namespace fmvibe::gui
