// The SEQ view: the sequencer's settings on a card in the inspector's place, and the piano roll of the pattern picked,
// with its tools, its ruler and loop, its keys and a lane of one value a note.
#include "seqpanel.h"
#include "lists.h"

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

constexpr double kNewVelocity = 100.0 / 127.0;
constexpr float kSeqScaleListW = 170.0f; // the scale's list, wider than its button: "Pentatonic Major" and the rest
constexpr float kSeqBareMacroDot = 0.3f; // how strongly a macro's dot shows while it has no curve in the pattern
constexpr double kShortestNote = 1.0 / 16.0; // steps
constexpr int kFieldCount = kSeqFieldCount;
constexpr int kFieldBend = 5;
constexpr Str kFieldNames[kFieldCount] = {Str::SeqFieldNote,   Str::SeqFieldVelocity, Str::SeqFieldChance,
                                          Str::SeqFieldLength, Str::SeqFieldOffset,   Str::SeqFieldBend};
// How far the mouse goes for a step of each field, dragged up and down.
constexpr float kFieldPixels[kFieldCount] = {8.0f, 3.0f, 3.0f, 10.0f, 3.0f, 6.0f};
constexpr int kToolBend = 2; // the third tool
// The scale's row: its root, the scale, then FOLD at the card's right.
Rect seqRootBox() { return {kSeqCardControls, kSeqScaleY, 48.0f, 22.0f}; }
Rect seqScaleButton() { return {kSeqCardControls + 52.0f, kSeqScaleY, 102.0f, 22.0f}; }
Rect seqFoldButton() { return {kSeqCard.x + kSeqCard.w - 12.0f - 54.0f, kSeqScaleY, 54.0f, 22.0f}; }
Rect seqLaneTab(const SeqRoll &r, int lane) {
    return {r.grid.x + lane * kSeqLaneTabStep, r.laneTabs.y + 3.0f, 72.0f, r.laneTabs.h - 6.0f};
}
// The zoom's buttons at the lane tabs' right: across out and in, then up and down out and in, each pair after its
// arrow.
Rect seqZoomButton(const SeqRoll &r, int b) {
    const float right = r.box.x + r.box.w - 6.0f;
    const float x = right - (4 - b) * 20.0f - (b < 2 ? 22.0f : 0.0f);
    return {x, r.laneTabs.y + 2.0f, 18.0f, r.laneTabs.h - 4.0f};
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

bool SeqPanel::full() const {
    return controller().editorSettings().seqFull;
}

const detail::SeqRoll &SeqPanel::roll() const {
    return seqRoll(full());
}

void SeqPanel::toggleSize() {
    EditorSettings settings = controller().editorSettings();
    settings.seqFull = !settings.seqFull;
    controller().setEditorSettings(settings);
}

bool SeqPanel::seqOn() const { return value(paramIndex(SeqParam::Enabled)) >= 0.5; }

std::vector<Rect> SeqPanel::offAreas() const {
    if (!wide() || seqOn())
        return {};
    const float below = kSeqTitleY + 22.0f + 2.0f; // under the title row
    std::vector<Rect> areas{{kSeqCard.x, below, kSeqCard.w, kSeqCard.bottom() - below}, kSeqTools.inset(-2.0f), roll().box};
    if (full())
        areas.push_back(kSeqOverview);
    return areas;
}

bool SeqPanel::wide() const {
    return controller().layout().view == RoutingView::Seq && !host_.shapeEditorOpen() && !host_.partialEditorOpen();
}

int SeqPanel::seqPattern() const {
    return stepped(value(paramIndex(SeqParam::Pattern)), 0, kSeqPatterns - 1);
}

int SeqPanel::seqSteps() const {
    return seqStepsOf(seqPattern());
}

int SeqPanel::seqStepsOf(int pattern) const {
    return kSeqStepsPerBar * stepped(value(paramIndex(pattern, SeqPatternParam::Length)), 1, kSeqBars);
}

std::vector<SeqPanel::SeqNote> SeqPanel::seqNotes() const {
    return seqNotesOf(seqPattern());
}

std::vector<SeqPanel::SeqNote> SeqPanel::seqNotesOf(int pattern) const {
    std::vector<SeqNote> notes;
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
        seqScrollX_ = static_cast<float>(step) - (at.x - roll().grid.x) / roll().grid.w * seqShownSteps();
        seqScrollX_ = seqScroll();
        return;
    }
    const float y = std::clamp(at.y, roll().grid.y, roll().grid.y + roll().grid.h - 1.0f);
    const float rowBelow = (roll().grid.y + roll().grid.h - y) / seqRowH(); // rows under the mouse
    const float row = static_cast<float>(seqFirstRow()) + rowBelow;
    seqZoomY_ = std::clamp(seqZoomY_ * factor, kSeqMinZoomY, kSeqMaxZoomY);
    seqShowFromRow(row - (roll().grid.y + roll().grid.h - y) / seqRowH());
}

Rect SeqPanel::seqScrollTrack() const {
    const float x = roll().macroTab.right() + 10.0f;
    return {x, roll().laneTabs.y + roll().laneTabs.h * 0.5f - 3.0f, seqZoomButton(roll(), 0).x - 30.0f - x, 6.0f};
}

Rect SeqPanel::seqScrollYThumb() const {
    const float total = static_cast<float>(std::max<size_t>(seqPitchList().size(), 1));
    const float count = static_cast<float>(seqRows().size()), first = static_cast<float>(seqFirstRow());
    const Rect track = roll().scrollY;
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
    seqZoomY_ = std::clamp(roll().grid.h / (kSeqRowH * span), kSeqMinZoomY, 1.0f);
    const float count = static_cast<float>(seqRows().size());
    seqShowFromRow(indexOf(low) - std::max(count - span, 0.0f) * 0.5f);
}

bool SeqPanel::seqEdgeScroll() {
    if (!dragging() || !wide())
        return false;
    const SeqDrag::Mode mode = seqDrag_.mode;
    // A note bend's point (or the points picked) dragged, and a stroke drawing a bend, as notes are.
    const bool bend = mode == SeqDrag::Mode::BendPoint || mode == SeqDrag::Mode::BendGroup ||
                      mode == SeqDrag::Mode::BendPaint;
    const bool across = mode == SeqDrag::Mode::Move || mode == SeqDrag::Mode::Resize ||
                        mode == SeqDrag::Mode::ResizeStart || mode == SeqDrag::Mode::Box || mode == SeqDrag::Mode::Paint ||
                        bend;
    const bool rows = mode == SeqDrag::Mode::Move || mode == SeqDrag::Mode::Box || mode == SeqDrag::Mode::Paint || bend;
    if (!across)
        return false;
    constexpr float kZone = 10.0f;
    const Point p = seqDrag_.now;
    bool moved = false;
    // Across, faster the further past the edge (all the way 60 px on): from a quarter of the most at the zone's edge up
    // to a 96th of what shows a poll, about 0.6 of it a second at the editor's 60 polls, as fast as the rows go (half a
    // row a poll, of the 50 or so that show).
    const float left = roll().grid.x + kZone - p.x, right = p.x - (roll().grid.x + roll().grid.w - kZone);
    const float pull = left > 0.0f ? -std::min(left / 60.0f, 1.0f) : right > 0.0f ? std::min(right / 60.0f, 1.0f) : 0.0f;
    if (pull != 0.0f) {
        const float before = seqScroll();
        const float speed = std::copysign(0.25f + 0.75f * std::abs(pull), pull);
        seqScrollX_ = seqScroll() + speed * seqShownSteps() / 96.0f;
        seqScrollX_ = seqScroll();
        moved = seqScrollX_ != before;
    }
    // Up and down, by rows: up to half a row a poll, whole rows at a time.
    const float above = roll().grid.y + kZone - p.y, below = p.y - (roll().grid.y + roll().grid.h - kZone);
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
    if (!seqBlipPitches_.empty() && host_.now() >= seqBlipUntil_)
        seqBlipEnd();
    const Telemetry &was = host_.telemetry();
    bool moved = false;
    if (wide() && (t.seqPosition != was.seqPosition || t.seqPattern != was.seqPattern || t.seqKeys != was.seqKeys))
        moved = true;
    if (wide() && t.seqPreview != was.seqPreview) // the Play button, played or stopped
        repaint.push_back(kSeqPreviewButton);
    if (wide() && seqTrackGlows(t))
        moved = true;
    if (seqFollowKeys(t) && wide())
        moved = true;
    if (moved)
        repaint.push_back(full() ? kSeqBand : kPanelsBand); // at the full size, down over the modulation panel's place
}

// The roll scrolling under a drag held at its edge; and a patch loaded (a preset, Init, the host's state), the roll
// fitted to its notes.
bool SeqPanel::step() {
    bool moved = seqEdgeScroll();
    if (controller().patchLoads() != seqLoadsSeen_) {
        seqLoadsSeen_ = controller().patchLoads();
        // The notes and points picked were the patch's before: their slots now hold the new one's.
        seqSelection_.clear();
        seqCurvePick_ = SeqCurvePick{};
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
    const int count = std::max(static_cast<int>(roll().grid.h / (kSeqRowH * std::clamp(seqZoomY_, kSeqMinZoomY, kSeqMaxZoomY))), 4);
    const std::vector<int> pitches = seqPitchList();
    const auto from = std::lower_bound(pitches.begin(), pitches.end(), layout.seqLow);
    const long first = std::clamp(static_cast<long>(from - pitches.begin()), 0L,
                                  std::max(static_cast<long>(pitches.size()) - count, 0L));
    return std::vector<int>(pitches.begin() + first, pitches.begin() + std::min<long>(first + count, static_cast<long>(pitches.size())));
}

float SeqPanel::seqRowH() const {
    return roll().grid.h / static_cast<float>(std::max<size_t>(seqRows().size(), 1));
}

float SeqPanel::seqStepX(double step) const {
    return roll().grid.x + static_cast<float>((step - seqScroll()) / seqShownSteps()) * roll().grid.w;
}

double SeqPanel::seqStepAt(float x) const {
    return seqScroll() + (x - roll().grid.x) / roll().grid.w * seqShownSteps();
}

float SeqPanel::seqRowY(int pitch) const {
    const std::vector<int> rows = seqRows();
    const auto at = std::find(rows.begin(), rows.end(), pitch);
    if (at == rows.end())
        return roll().grid.y + roll().grid.h + 100.0f;
    return roll().grid.y + roll().grid.h - static_cast<float>(at - rows.begin() + 1) * seqRowH();
}

Point SeqPanel::seqBoxStart() const {
    return {seqStepX(seqDrag_.grabStep),
            roll().grid.y + roll().grid.h - (seqDrag_.boxRow - static_cast<float>(seqFirstRow())) * seqRowH()};
}

int SeqPanel::seqPitchAt(float y) const {
    const std::vector<int> rows = seqRows();
    if (rows.empty())
        return 60;
    const int row = static_cast<int>(std::floor((roll().grid.y + roll().grid.h - y) / seqRowH()));
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

// A note past the pattern's end (its Length shortened, which keeps the notes for when it grows again) neither plays
// nor shows: it isn't picked, so nothing moves it or moves the others by it.
bool SeqPanel::seqSelected(int slot) const {
    return seqSelectionPattern_ == seqPattern() &&
           std::find(seqSelection_.begin(), seqSelection_.end(), slot) != seqSelection_.end() &&
           value(seqNoteIndex(seqPattern(), slot, SeqNoteParam::Start)) < seqSteps();
}

std::vector<int> SeqPanel::seqPicked() const {
    std::vector<int> picked;
    if (seqSelectionPattern_ != seqPattern())
        return picked;
    for (int slot : seqSelection_)
        if (value(seqNoteIndex(seqPattern(), slot, SeqNoteParam::Length)) > 0.0 &&
            value(seqNoteIndex(seqPattern(), slot, SeqNoteParam::Start)) < seqSteps())
            picked.push_back(slot);
    return picked;
}

// --- Widgets.

void SeqPanel::widgets(std::vector<Widget> &list) const {
    const WidgetAdder add{list};
    const int pattern = seqPattern();
    // The card: its switch, the patterns and copying one, when it plays, the knobs, the order and root, the scale.
    add(Kind::Toggle, {kSeqCard.x + kSeqCard.w - 12.0f - 26.0f, kSeqTitleY, 26.0f, 22.0f}, paramIndex(SeqParam::Enabled));
    size_t from = list.size();
    for (int p = 0; p < kSeqPatterns; ++p)
        add(Kind::Choice, {kSeqCardControls + p * 30.0f, kSeqPatternY, 28.0f, 22.0f}, paramIndex(SeqParam::Pattern), -1, p);
    mergeButtons(list, from);
    add(Kind::SeqPreview, kSeqPreviewButton);
    add(Kind::SeqSize, kSeqSizeButton);
    for (int p = 0; p < kSeqPatterns && full(); ++p) // the overview under the card, at the full size
        add(Kind::SeqTile, seqTile(p), -1, -1, p);
    from = list.size();
    for (int p = 0; p < static_cast<int>(SeqPlay::Count); ++p)
        add(Kind::Choice, {kSeqCardControls + p * 62.0f, kSeqPlayY, 60.0f, 22.0f}, paramIndex(SeqParam::Play), -1, p);
    mergeButtons(list, from);
    // Two rows of four: Speed and Length over Octave and Transpose at the left, Swing and Gate over Humanize and
    // Velocity at the right.
    const int knobs[8] = {paramIndex(SeqParam::Rate),   paramIndex(pattern, SeqPatternParam::Length),
                          paramIndex(SeqParam::Swing),  paramIndex(SeqParam::Gate),
                          paramIndex(SeqParam::Octave), paramIndex(SeqParam::Transpose),
                          paramIndex(SeqParam::Humanize), paramIndex(SeqParam::Velocity)};
    const Str labels[8] = {Str::LabelSpeed,  Str::LabelLength,    Str::LabelSwing,    Str::LabelGate,
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

    // The tools over the roll: select | draw | bend, the snap, stretching, the field of the notes picked, Slide, Mute, the
    // bin.
    from = list.size();
    for (int b = 0; b < kSeqToolCount; ++b)
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
    add(Kind::SeqRuler, roll().ruler);
    add(Kind::SeqKeys, roll().keys);
    add(Kind::SeqGrid, roll().grid);
    add(Kind::SeqLane, roll().laneArea);
    for (int lane = 0; lane < static_cast<int>(SeqLane::Macro1); ++lane)
        add(Kind::SeqLaneTab, seqLaneTab(roll(), lane), -1, -1, lane);
    add(Kind::SeqMacroTab, seqMacroLabel(roll().macroTab));
    add(Kind::SeqMacroArrow, seqMacroArrow(roll().macroTab));
    for (int b = 0; b < 4; ++b)
        add(Kind::SeqZoom, seqZoomButton(roll(), b), -1, -1, b);
    if (seqShownSteps() < static_cast<float>(seqSteps()))
        add(Kind::SeqScroll, seqScrollTrack().inset(-4.0f));
    add(Kind::SeqScrollY, {roll().scrollY.x - 3.0f, roll().scrollY.y, roll().scrollY.w + 5.0f, roll().scrollY.h});
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
    const WidgetAdder add{list};
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
    // The macros' list, over its tab.
    if (seqMacroMenu_) {
        add(Kind::SeqMacroMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, -1, 0);
        list.back().popup = true;
        add(Kind::SeqMacroMenu, roll().macroList, -1, 1);
        list.back().popup = true;
        for (int macro = 0; macro < kMacros; ++macro) {
            add(Kind::SeqMacroItem, seqMacroRow(roll().macroList, macro), -1, -1, macro);
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
    start = std::clamp(start, 0.0, seqSteps() - kShortestNote);
    // As long as asked, but ending where the pattern does at the latest.
    length = std::clamp(length, kShortestNote, std::max(seqSteps() - start, kShortestNote));
    editMany({{seqNoteIndex(pattern, slot, SeqNoteParam::Start), start},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Length), length},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Pitch), std::clamp(pitch, 0, 127)},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Velocity), kNewVelocity},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Chance), 1.0},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Slide), 0.0},
              {seqNoteIndex(pattern, slot, SeqNoteParam::Mute), 0.0}});
    setSeqBend(slot, {}); // a slot's bend left behind, gone
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
    // Back to the defaults, its bend too, so a preset saved sparse leaves the slot out.
    std::vector<std::pair<int, double>> changes;
    for (int slot : slots)
        for (int index : seqSlotParams(pattern, slot))
            if (value(index) != table_[index].def)
                changes.push_back({index, table_[index].def});
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
    if (field == kFieldBend) { // each note's bend scaled, its farthest point a semitone a step further or nearer
        for (int slot : picked) {
            const double extreme = std::abs(seqBendExtreme(slot));
            if (extreme < 1e-9)
                continue;
            const double factor = std::clamp(extreme + steps, 0.0, kSeqMaxNoteBend) / extreme;
            std::vector<SeqCurvePoint> points = seqBendPoints(slot);
            for (SeqCurvePoint &q : points)
                q.y = std::clamp(q.y * factor, -kSeqMaxNoteBend, kSeqMaxNoteBend);
            setSeqBend(slot, points);
        }
        return;
    }
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
    // Two kept landing on one another (quantized onto one step, stretched against the end): the first stays.
    for (size_t i = 0; i < notes.size(); ++i) {
        const SeqNote &n = notes[i];
        if (!isKept(n.slot) || std::find(drop.begin(), drop.end(), n.slot) != drop.end())
            continue;
        for (size_t j = 0; j < i; ++j) {
            const SeqNote &k = notes[j];
            if (isKept(k.slot) && k.pitch == n.pitch && std::abs(k.start - n.start) < 1e-6 &&
                std::find(drop.begin(), drop.end(), k.slot) == drop.end()) {
                drop.push_back(n.slot);
                break;
            }
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
    bool curves = false;
    for (int c = 0; whole && c < kSeqCurves; ++c)
        curves = curves || !seqCurvePoints(static_cast<SeqCurve>(c)).empty();
    if (changes.empty() && !curves)
        return;
    controller().beginStep();
    editMany(changes);
    for (int slot : slots) { // its bend as much longer or shorter as the note
        std::vector<SeqCurvePoint> points = seqBendPoints(slot);
        for (SeqCurvePoint &point : points)
            point.x = std::min(point.x * factor, static_cast<double>(kSeqMaxSteps));
        setSeqBend(slot, points);
    }
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

// --- The lane's curves: the pattern's pitch bend, mod wheel and macros.

bool SeqPanel::seqCurveLane() const { return controller().layout().seqLane >= SeqLane::Bend; }

int SeqPanel::seqTabMacro() const {
    const SeqLane lane = controller().layout().seqLane;
    return seqLaneMacro(lane) ? static_cast<int>(lane) - static_cast<int>(SeqLane::Macro1) : seqLastMacro_;
}

void SeqPanel::showSeqMacro(int macro) {
    seqLastMacro_ = std::clamp(macro, 0, kMacros - 1);
    controller().layout().seqLane = static_cast<SeqLane>(static_cast<int>(SeqLane::Macro1) + seqLastMacro_);
}

SeqCurve SeqPanel::seqLaneCurve() const {
    const SeqLane lane = controller().layout().seqLane;
    return seqLaneMacro(lane) ? seqMacroCurve(static_cast<int>(lane) - static_cast<int>(SeqLane::Macro1))
           : lane == SeqLane::ModWheel ? SeqCurve::ModWheel
                                       : SeqCurve::Bend;
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
    const float top = roll().laneArea.y + 6.0f, h = roll().laneArea.h - 12.0f;
    const double share = curve == SeqCurve::Bend ? (v + 1.0) * 0.5 : v;
    return top + h - static_cast<float>(std::clamp(share, 0.0, 1.0)) * h;
}

double SeqPanel::seqCurveValueAt(SeqCurve curve, float y) const {
    const float top = roll().laneArea.y + 6.0f, h = roll().laneArea.h - 12.0f;
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

int SeqPanel::seqCurveSegments(const std::vector<SeqCurvePoint> &points) const {
    const double steps = seqSteps();
    return static_cast<int>(std::count_if(points.begin(), points.end(), [&](const SeqCurvePoint &q) { return q.x <= steps; }));
}

SeqCurvePoint SeqPanel::seqCurveSegmentEnd(const std::vector<SeqCurvePoint> &points, int segment) const {
    if (segment + 1 < seqCurveSegments(points))
        return points[static_cast<size_t>(segment) + 1];
    return {points.front().x + seqSteps(), points.front().y, points.front().curve};
}

bool SeqPanel::seqCurveBends(const std::vector<SeqCurvePoint> &points, int segment) const {
    const SeqCurvePoint &a = points[static_cast<size_t>(segment)];
    const SeqCurvePoint b = seqCurveSegmentEnd(points, segment);
    return seqStepX(b.x) - seqStepX(a.x) >= 20.0f && std::abs(b.y - a.y) >= 1e-6;
}

// The last segment's middle shows where it falls: before the pattern's end, or past it, back at the pattern's start.
Point SeqPanel::seqCurveBendPos(const std::vector<SeqCurvePoint> &points, SeqCurve curve, int segment) const {
    const SeqCurvePoint &a = points[static_cast<size_t>(segment)];
    const SeqCurvePoint b = seqCurveSegmentEnd(points, segment);
    const double middle = (a.x + b.x) * 0.5;
    return {seqStepX(middle > seqSteps() ? middle - seqSteps() : middle),
            seqCurveY(curve, a.y + (b.y - a.y) * shapeBend(0.5, a.curve))};
}

// A segment's middle takes a bend where it rises or falls and has room for its handle between its points.
int SeqPanel::seqCurveBendAt(Point p) const {
    if (!seqCurveLane())
        return -1;
    const SeqCurve curve = seqLaneCurve();
    const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
    for (int i = 0; i < seqCurveSegments(points); ++i) {
        if (!seqCurveBends(points, i))
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

// --- The notes' own bends.

namespace {
// A bend in semitones as the readouts show it, "+2 st", "-0.37 st", "0 st"; or as the field does, without the unit.
std::string bendText(double semitones, bool unit = true) {
    char text[24];
    if (std::abs(semitones) < 0.005)
        std::snprintf(text, sizeof text, "0");
    else if (std::abs(semitones - std::round(semitones)) < 0.005)
        std::snprintf(text, sizeof text, "%+ld", std::lround(semitones));
    else
        std::snprintf(text, sizeof text, "%+.2f", semitones);
    return unit ? trf(Str::SeqBendSemitones, {text}) : std::string(text);
}
} // namespace

bool SeqPanel::seqDrawing() const { return controller().layout().seqDraw && !controller().layout().seqBend; }
bool SeqPanel::seqBendTool() const { return controller().layout().seqBend; }

std::vector<int> SeqPanel::seqSlotParams(int pattern, int slot) {
    std::vector<int> params;
    for (int f = 0; f < kSeqNoteParamCount; ++f)
        params.push_back(seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f)));
    for (int f = 0; f < kSeqNoteBendParamCount; ++f)
        params.push_back(seqNoteBendCountIndex(pattern, slot) + f);
    return params;
}

std::vector<SeqCurvePoint> SeqPanel::seqBendPoints(int slot) const {
    const int pattern = seqPattern();
    auto get = [this](int index) { return value(index); };
    std::vector<SeqCurvePoint> points;
    const int count = seqNoteBendPointCount(get, pattern, slot);
    for (int i = 0; i < count; ++i)
        points.push_back({value(seqNoteBendIndex(pattern, slot, i, PointField::X)),
                          value(seqNoteBendIndex(pattern, slot, i, PointField::Y)),
                          value(seqNoteBendIndex(pattern, slot, i, PointField::Curve))});
    return points;
}

void SeqPanel::setSeqBend(int slot, const std::vector<SeqCurvePoint> &points) {
    const int pattern = seqPattern();
    auto get = [this](int index) { return value(index); };
    const int count = std::min(static_cast<int>(points.size()), kSeqNoteBendPoints);
    const int was = seqNoteBendPointCount(get, pattern, slot);
    std::vector<std::pair<int, double>> changes;
    auto put = [&](int index, double v) {
        if (value(index) != v)
            changes.push_back({index, v});
    };
    put(seqNoteBendCountIndex(pattern, slot), count);
    for (int i = 0; i < std::max(count, was); ++i) {
        const bool kept = i < count;
        const int x = seqNoteBendIndex(pattern, slot, i, PointField::X);
        const int y = seqNoteBendIndex(pattern, slot, i, PointField::Y);
        const int bend = seqNoteBendIndex(pattern, slot, i, PointField::Curve);
        put(x, kept ? points[static_cast<size_t>(i)].x : table_[x].def);
        put(y, kept ? std::clamp(points[static_cast<size_t>(i)].y, -kSeqMaxNoteBend, kSeqMaxNoteBend) : table_[y].def);
        put(bend, kept ? points[static_cast<size_t>(i)].curve : table_[bend].def);
    }
    if (!changes.empty())
        editMany(changes);
}

double SeqPanel::seqBendExtreme(int slot) const {
    double extreme = 0.0;
    for (const SeqCurvePoint &q : seqBendPoints(slot))
        if (std::abs(q.y) > std::abs(extreme))
            extreme = q.y;
    return extreme;
}

std::optional<SeqPanel::SeqNote> SeqPanel::seqNoteOf(int slot) const {
    for (const SeqNote &n : seqNotes())
        if (n.slot == slot)
            return n;
    return std::nullopt;
}

float SeqPanel::seqPitchY(double pitch) const {
    const std::vector<int> pitches = seqPitchList();
    if (pitches.empty())
        return roll().grid.y + roll().grid.h + 100.0f;
    double index;
    if (pitch <= pitches.front()) {
        index = pitch - pitches.front();
    } else if (pitch >= pitches.back()) {
        index = static_cast<double>(pitches.size() - 1) + (pitch - pitches.back());
    } else {
        const auto above = std::upper_bound(pitches.begin(), pitches.end(), pitch, [](double v, int q) { return v < q; });
        const size_t k = static_cast<size_t>(above - pitches.begin()) - 1;
        index = static_cast<double>(k) + (pitch - pitches[k]) / static_cast<double>(pitches[k + 1] - pitches[k]);
    }
    return roll().grid.y + roll().grid.h - static_cast<float>(index - seqFirstRow() + 0.5) * seqRowH();
}

double SeqPanel::seqPitchAtY(float y) const {
    const std::vector<int> pitches = seqPitchList();
    if (pitches.empty())
        return 60.0;
    const double index = (roll().grid.y + roll().grid.h - y) / seqRowH() - 0.5 + seqFirstRow();
    const double last = static_cast<double>(pitches.size() - 1);
    if (index <= 0.0)
        return pitches.front() + index;
    if (index >= last)
        return pitches.back() + (index - last);
    const size_t k = static_cast<size_t>(std::floor(index));
    return pitches[k] + (index - static_cast<double>(k)) * (pitches[k + 1] - pitches[k]);
}

float SeqPanel::seqBendY(const SeqNote &n, double semitones) const {
    return seqPitchY(n.pitch + semitones);
}

double SeqPanel::seqBendAtY(const SeqNote &n, float y) const {
    return seqPitchAtY(y) - n.pitch;
}

SeqPanel::BendHit SeqPanel::seqBendPointAt(Point p) const {
    if (!seqBendTool())
        return {};
    const std::vector<SeqNote> notes = seqNotes();
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const std::vector<SeqCurvePoint> points = seqBendPoints(it->slot);
        for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i) {
            const SeqCurvePoint &q = points[static_cast<size_t>(i)];
            if (std::hypot(p.x - seqStepX(it->start + q.x), p.y - seqBendY(*it, q.y)) <= 6.0f)
                return {it->slot, i};
        }
    }
    return {};
}

bool SeqPanel::seqBendSegmentBends(const SeqNote &n, const std::vector<SeqCurvePoint> &points, int segment) const {
    if (segment < 0 || segment + 1 >= static_cast<int>(points.size()))
        return false;
    const SeqCurvePoint &a = points[static_cast<size_t>(segment)], &b = points[static_cast<size_t>(segment) + 1];
    return seqStepX(n.start + b.x) - seqStepX(n.start + a.x) >= 20.0f && std::abs(b.y - a.y) >= 1e-6;
}

Point SeqPanel::seqBendSegmentPos(const SeqNote &n, const std::vector<SeqCurvePoint> &points, int segment) const {
    const SeqCurvePoint &a = points[static_cast<size_t>(segment)], &b = points[static_cast<size_t>(segment) + 1];
    return {seqStepX(n.start + (a.x + b.x) * 0.5), seqBendY(n, a.y + (b.y - a.y) * shapeBend(0.5, a.curve))};
}

SeqPanel::BendHit SeqPanel::seqBendSegmentAt(Point p) const {
    if (!seqBendTool())
        return {};
    const std::vector<SeqNote> notes = seqNotes();
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const std::vector<SeqCurvePoint> points = seqBendPoints(it->slot);
        for (int i = 0; i + 1 < static_cast<int>(points.size()); ++i) {
            if (!seqBendSegmentBends(*it, points, i))
                continue;
            const Point handle = seqBendSegmentPos(*it, points, i);
            if (std::hypot(p.x - handle.x, p.y - handle.y) <= 6.0f)
                return {it->slot, i};
        }
    }
    return {};
}

std::optional<SeqPanel::BendLineHit> SeqPanel::seqBendLineAt(Point p) const {
    if (!seqBendTool() || seqBendPointAt(p).slot >= 0 || seqBendSegmentAt(p).slot >= 0)
        return std::nullopt;
    const std::vector<SeqNote> notes = seqNotes();
    const double step = seqStepAt(p.x);
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const double t = step - it->start;
        if (t < 0.0 || t > it->length)
            continue;
        const std::vector<SeqCurvePoint> points = seqBendPoints(it->slot);
        if (points.empty())
            continue;
        const double v = seqNoteBendAt(static_cast<int>(points.size()),
                                       [&](int i) { return points[static_cast<size_t>(i)]; }, t);
        const float y = seqBendY(*it, v);
        if (std::abs(p.y - y) <= 5.0f)
            return BendLineHit{it->slot, {t, v, 0.0}, {p.x, y}};
    }
    return std::nullopt;
}

std::vector<SeqPanel::BendHit> SeqPanel::seqBendPicked() const {
    std::vector<BendHit> points;
    if (!seqBendTool() || seqBendPick_.pattern != seqPattern())
        return points;
    for (size_t i = 0; i < seqBendPick_.points.size(); ++i) {
        const BendHit &h = seqBendPick_.points[i];
        const int count = static_cast<int>(seqBendPoints(h.slot).size());
        if (count == seqBendPick_.counts[i] && h.index >= 0 && h.index < count &&
            value(seqNoteIndex(seqPattern(), h.slot, SeqNoteParam::Length)) > 0.0)
            points.push_back(h);
    }
    return points;
}

void SeqPanel::setSeqBendPick(std::vector<BendHit> points) {
    std::sort(points.begin(), points.end(),
              [](const BendHit &a, const BendHit &b) { return a.slot != b.slot ? a.slot < b.slot : a.index < b.index; });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    seqBendPick_ = {seqPattern(), points, {}};
    for (const BendHit &h : points)
        seqBendPick_.counts.push_back(static_cast<int>(seqBendPoints(h.slot).size()));
}

void SeqPanel::moveSeqBendPoints(Point p) {
    // As the lane's curve's picked points move together: as far as the one grabbed, as far as each can, up and down
    // within a bend's reach and across between its nearest neighbors not picked (or its note's ends).
    const SeqDrag &d = seqDrag_;
    const auto grabbed = std::find(d.bendPoints.begin(), d.bendPoints.end(), BendHit{d.anchor, d.bendIndex});
    const std::optional<SeqNote> anchor = seqNoteOf(d.anchor);
    if (grabbed == d.bendPoints.end() || !anchor || d.bendOrigin.size() != d.bendPoints.size())
        return;
    const SeqCurvePoint &from = d.bendOrigin[static_cast<size_t>(grabbed - d.bendPoints.begin())];
    const SeqCurvePoint to = seqBendPointFor(*anchor, p, d.free);
    double dx = to.x - from.x, dy = to.y - from.y;
    auto isPicked = [&](int slot, int index) {
        return std::find(d.bendPoints.begin(), d.bendPoints.end(), BendHit{slot, index}) != d.bendPoints.end();
    };
    for (size_t k = 0; k < d.bendPoints.size(); ++k) {
        const BendHit &h = d.bendPoints[k];
        const SeqCurvePoint &o = d.bendOrigin[k];
        const std::optional<SeqNote> n = seqNoteOf(h.slot);
        const std::vector<SeqCurvePoint> points = seqBendPoints(h.slot);
        if (!n || h.index >= static_cast<int>(points.size()))
            return;
        dy = std::clamp(dy, std::min(-kSeqMaxNoteBend - o.y, 0.0), std::max(kSeqMaxNoteBend - o.y, 0.0));
        int below = h.index - 1, above = h.index + 1;
        while (below >= 0 && isPicked(h.slot, below))
            --below;
        while (above < static_cast<int>(points.size()) && isPicked(h.slot, above))
            ++above;
        const double lo = below >= 0 ? points[static_cast<size_t>(below)].x : 0.0;
        const double hi = above < static_cast<int>(points.size()) ? points[static_cast<size_t>(above)].x : n->length;
        dx = std::clamp(dx, std::min(lo - o.x, 0.0), std::max(hi - o.x, 0.0));
    }
    std::vector<int> slots;
    for (const BendHit &h : d.bendPoints)
        if (std::find(slots.begin(), slots.end(), h.slot) == slots.end())
            slots.push_back(h.slot);
    for (int slot : slots) {
        std::vector<SeqCurvePoint> points = seqBendPoints(slot);
        for (size_t k = 0; k < d.bendPoints.size(); ++k)
            if (d.bendPoints[k].slot == slot) {
                SeqCurvePoint &q = points[static_cast<size_t>(d.bendPoints[k].index)];
                q.x = d.bendOrigin[k].x + dx;
                q.y = d.bendOrigin[k].y + dy;
            }
        setSeqBend(slot, points);
    }
    if (d.sounding >= 0) // the note grabbed heard at the pitch it bends to
        controller().auditionBend(from.y + dy);
}

void SeqPanel::removeSeqBendPoints(const std::vector<BendHit> &picked) {
    std::vector<int> slots;
    for (const BendHit &h : picked)
        if (std::find(slots.begin(), slots.end(), h.slot) == slots.end())
            slots.push_back(h.slot);
    controller().beginStep();
    for (int slot : slots) {
        std::vector<SeqCurvePoint> points = seqBendPoints(slot);
        for (int i = static_cast<int>(points.size()) - 1; i >= 0; --i)
            if (std::find(picked.begin(), picked.end(), BendHit{slot, i}) != picked.end())
                points.erase(points.begin() + i);
        if (points.size() == 1 && std::abs(points.front().y) < 1e-9) // a lone point at 0 bends nothing
            points.clear();
        setSeqBend(slot, points);
    }
    controller().endStep();
    seqBendPick_ = BendPick{};
}

int SeqPanel::seqBendNoteAt(Point p) const {
    const int slot = seqNoteAt(p);
    const std::optional<SeqNote> n = slot >= 0 ? seqNoteOf(slot) : std::nullopt;
    if (!n)
        return -1;
    const std::vector<SeqCurvePoint> points = seqBendPoints(slot);
    if (points.empty())
        return slot;
    const double v = seqNoteBendAt(static_cast<int>(points.size()), [&](int i) { return points[static_cast<size_t>(i)]; },
                                   std::clamp(seqStepAt(p.x) - n->start, 0.0, n->length));
    const float y = seqBendY(*n, v);
    const Rect bar = seqNoteRect(*n);
    return y >= bar.y - 1.0f && y <= bar.bottom() + 1.0f ? slot : -1;
}

int SeqPanel::seqBendLitNote(Point p) const {
    if (!seqBendTool() || !roll().grid.contains(p))
        return -1;
    if (const BendHit h = seqBendPointAt(p); h.slot >= 0)
        return h.slot;
    if (const BendHit h = seqBendSegmentAt(p); h.slot >= 0)
        return h.slot;
    if (const std::optional<BendLineHit> line = seqBendLineAt(p))
        return line->slot;
    return seqNoteAt(p);
}

bool SeqPanel::hoverMoves(Point before, Point now) const {
    if (!wide() || !seqBendTool() || !(roll().grid.contains(before) || roll().grid.contains(now)))
        return false;
    const bool wasIn = roll().grid.contains(before), isIn = roll().grid.contains(now);
    const std::optional<BendLineHit> lineBefore = wasIn ? seqBendLineAt(before) : std::nullopt;
    const std::optional<BendLineHit> lineNow = isIn ? seqBendLineAt(now) : std::nullopt;
    if (lineBefore || lineNow) // the faint point moves along the line, or comes or goes
        return true;
    return (wasIn ? seqBendPointAt(before) : BendHit{}) != (isIn ? seqBendPointAt(now) : BendHit{}) ||
           (wasIn ? seqBendSegmentAt(before) : BendHit{}) != (isIn ? seqBendSegmentAt(now) : BendHit{}) ||
           seqBendLitNote(before) != seqBendLitNote(now);
}

SeqCurvePoint SeqPanel::seqBendPointFor(const SeqNote &n, Point p, bool free) const {
    const double grid = free ? 0.0 : kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
    double step = seqStepAt(p.x);
    if (grid > 0.0)
        step = std::round(step / grid) * grid;
    const double x = std::clamp(step - n.start, 0.0, n.length);
    double v = seqBendAtY(n, p.y);
    if (free) {
        v = std::round(v * 100.0) / 100.0;
    } else { // onto the nearest row the roll can show: a semitone, or a degree of the scale while folded
        const std::vector<int> pitches = seqPitchList();
        const double pitch = n.pitch + v;
        int nearest = n.pitch;
        for (int q : pitches)
            if (std::abs(q - pitch) < std::abs(nearest - pitch))
                nearest = q;
        v = nearest - n.pitch;
    }
    return {x, std::clamp(v, -kSeqMaxNoteBend, kSeqMaxNoteBend), 0.0};
}

int SeqPanel::addSeqBendPoint(int slot, SeqCurvePoint point) {
    std::vector<SeqCurvePoint> points = seqBendPoints(slot);
    const bool start = points.empty() && point.x > 1e-9; // from the note's pitch as it starts
    if (static_cast<int>(points.size()) + (start ? 2 : 1) > kSeqNoteBendPoints) {
        host_.flash(trf(Str::MsgSeqBendFull, {std::to_string(kSeqNoteBendPoints)}));
        return -1;
    }
    if (start)
        points.push_back({0.0, 0.0, 0.0});
    const auto at = std::upper_bound(points.begin(), points.end(), point.x,
                                     [](double s, const SeqCurvePoint &q) { return s < q.x; });
    const int index = static_cast<int>(at - points.begin());
    points.insert(at, point);
    setSeqBend(slot, points);
    return index;
}

void SeqPanel::paintSeqBend(int slot, double from, double fromValue, double step, double v, double unit) {
    std::vector<SeqCurvePoint> points = seqBendPoints(slot);
    const double lo = std::min(from, step), hi = std::max(from, step);
    const bool first = std::abs(step - from) < 1e-9; // the stroke's start: only its own point
    std::erase_if(points, [&](const SeqCurvePoint &q) {
        return (q.x > lo + 1e-9 && q.x < hi - 1e-9) || std::abs(q.x - step) < 1e-9;
    });
    const int lines = first ? 1 : static_cast<int>(std::lround((hi - lo) / unit));
    for (int k = 1; k <= lines; ++k) {
        if (static_cast<int>(points.size()) >= kSeqNoteBendPoints) {
            if (!seqDrag_.bendFull)
                host_.flash(trf(Str::MsgSeqBendFull, {std::to_string(kSeqNoteBendPoints)}));
            seqDrag_.bendFull = true;
            break;
        }
        const double t = first ? 1.0 : static_cast<double>(k) / lines;
        const double x = first ? step : from + (step - from) * t;
        const auto at = std::upper_bound(points.begin(), points.end(), x,
                                         [](double s, const SeqCurvePoint &q) { return s < q.x; });
        points.insert(at, {x, fromValue + (v - fromValue) * t, 0.0});
    }
    setSeqBend(slot, points);
}

// Each bent note's bend, at its pitch: a line from its start along its points to its end, in the notes' color
// lightened, a faint ribbon down to the note's own row, dashed where the note is muted or plays at a chance, and past
// its end (a point left there by a note made shorter, which doesn't play) faint and dashed. With Bend, the bends stand
// out over the faded notes, every bent note shows its points and its segments' handles, the bend of the note under the
// mouse (or whose line, point or handle is) is lit, and the mouse over a line shows a faint point where a press puts
// one down.
void SeqPanel::paintSeqBends(Canvas &c, const std::vector<SeqNote> &notes) {
    const bool tool = seqBendTool();
    const Color ink = mix(kAccent, kText, 0.45f);
    const std::vector<BendHit> pickedPoints = seqBendPicked();
    const int lit = dragging() ? (seqDrag_.mode == SeqDrag::Mode::BendPoint || seqDrag_.mode == SeqDrag::Mode::BendCurve ||
                                          seqDrag_.mode == SeqDrag::Mode::BendPaint || seqDrag_.mode == SeqDrag::Mode::BendGroup
                                      ? seqDrag_.anchor
                                      : -1)
                               : seqBendLitNote(host_.mouse());
    const BendHit hotPoint = dragging() && (seqDrag_.mode == SeqDrag::Mode::BendPoint ||
                                            seqDrag_.mode == SeqDrag::Mode::BendGroup)
                                 ? BendHit{seqDrag_.anchor, seqDrag_.bendIndex}
                             : !dragging() ? seqBendPointAt(host_.mouse())
                                           : BendHit{};
    const BendHit hotSegment = dragging() && seqDrag_.mode == SeqDrag::Mode::BendCurve
                                   ? BendHit{seqDrag_.anchor, seqDrag_.bendIndex}
                               : !dragging() && hotPoint.slot < 0 ? seqBendSegmentAt(host_.mouse())
                                                                  : BendHit{};
    for (const SeqNote &n : notes) {
        const std::vector<SeqCurvePoint> points = seqBendPoints(n.slot);
        if (points.empty())
            continue;
        const bool picked = seqSelected(n.slot);
        const bool hot = tool && n.slot == lit;
        // Strong with Bend, which edits them; else faint, behind the notes they belong to.
        const float level = tool ? 1.0f : static_cast<float>(0.55 + 0.45 * std::clamp(n.velocity, 0.0, 1.0)) * 0.45f;
        const Color lineInk = hot ? kText : ink;
        const int count = static_cast<int>(points.size());
        auto at = [&](double t) {
            return Point{seqStepX(n.start + t),
                         seqBendY(n, seqNoteBendAt(count, [&](int i) { return points[static_cast<size_t>(i)]; }, t))};
        };
        auto trace = [&](double from, double to) {
            std::vector<Point> line;
            const int samples = std::clamp(static_cast<int>((seqStepX(n.start + to) - seqStepX(n.start + from)) / 3.0f), 8, 400);
            for (int k = 0; k <= samples; ++k)
                line.push_back(at(from + (to - from) * k / samples));
            return line;
        };
        std::vector<Point> line = trace(0.0, n.length);
        const float base = seqBendY(n, 0.0);
        std::vector<Point> area = line;
        area.push_back({line.back().x, base});
        area.push_back({line.front().x, base});
        c.fillPolygon(area.data(), area.size(), lineInk.alpha((hot ? 0.24f : tool ? 0.18f : 0.2f) * level));
        c.polyline(line.data(), line.size(), (n.mute ? kFaint : lineInk).alpha(level),
                   hot ? 2.6f : tool || picked ? 2.0f : 1.5f, n.mute || n.chance < 1.0);
        if (points.back().x > n.length + 1e-9) {
            const std::vector<Point> tail = trace(n.length, points.back().x);
            c.polyline(tail.data(), tail.size(), lineInk.alpha(0.35f * level), 1.2f, true);
        }
        if (!tool)
            continue;
        for (int i = 0; i + 1 < count; ++i)
            if (seqBendSegmentBends(n, points, i)) {
                const bool hot = hotSegment == BendHit{n.slot, i};
                c.strokeCircle(seqBendSegmentPos(n, points, i), hot ? 4.0f : 2.5f, ink.alpha(hot ? 1.0f : 0.7f), 1.2f);
            }
        for (int i = 0; i < count; ++i) {
            const SeqCurvePoint &q = points[static_cast<size_t>(i)];
            const Point dot{seqStepX(n.start + q.x), seqBendY(n, q.y)};
            c.fillCircle(dot, 3.5f, wellColor());
            c.strokeCircle(dot, 3.5f, ink.alpha(q.x > n.length + 1e-9 ? 0.4f : 1.0f), 1.5f);
            if (hotPoint == BendHit{n.slot, i})
                c.strokeCircle(dot, 6.0f, kText, 1.5f);
            else if (std::find(pickedPoints.begin(), pickedPoints.end(), BendHit{n.slot, i}) != pickedPoints.end())
                c.strokeCircle(dot, 6.0f, ink, 1.5f); // picked: ringed
        }
    }
    // The box being dragged, picking the points it holds.
    if (dragging() && seqDrag_.mode == SeqDrag::Mode::BendBox) {
        const Rect box{std::min(seqDrag_.start.x, seqDrag_.now.x), std::min(seqDrag_.start.y, seqDrag_.now.y),
                       std::abs(seqDrag_.now.x - seqDrag_.start.x), std::abs(seqDrag_.now.y - seqDrag_.start.y)};
        c.fillRect(box, ink.alpha(0.12f));
        c.strokeRect(box, ink.alpha(0.7f), 1.0f);
    }
    // The mouse over a bend's line: a faint point there, where a press puts one down.
    if (!dragging() && roll().grid.contains(host_.mouse()))
        if (const std::optional<BendLineHit> line = seqBendLineAt(host_.mouse())) {
            c.fillCircle(line->at, 3.5f, wellColor().alpha(0.6f));
            c.strokeCircle(line->at, 3.5f, ink.alpha(0.45f), 1.2f);
        }
    // While a point is dragged, how far it bends, by it.
    if (dragging() && (seqDrag_.mode == SeqDrag::Mode::BendPoint || seqDrag_.mode == SeqDrag::Mode::BendGroup))
        if (const std::optional<SeqNote> n = seqNoteOf(seqDrag_.anchor)) {
            const std::vector<SeqCurvePoint> points = seqBendPoints(n->slot);
            if (seqDrag_.bendIndex >= 0 && seqDrag_.bendIndex < static_cast<int>(points.size())) {
                const SeqCurvePoint &q = points[static_cast<size_t>(seqDrag_.bendIndex)];
                const std::string text = bendText(q.y);
                const float w = c.textWidth(text, kTextSmall, true) + 10.0f;
                const Point dot{seqStepX(n->start + q.x), seqBendY(*n, q.y)};
                const Rect pill{dot.x + 9.0f, dot.y - 18.0f, w, 14.0f};
                c.fillRect(pill, kRaised, kRadiusMark);
                c.strokeRect(pill, ink.alpha(0.6f), 1.0f, kRadiusMark);
                c.text(pill, text, kTextSmall, kText, Align::Center, true);
            }
        }
}

void SeqPanel::clearSeqPattern(int pattern) {
    // Its notes' and curves' params back to their defaults, so a preset saved sparse leaves them out; its length and
    // loop stay.
    std::vector<std::pair<int, double>> changes;
    const std::vector<int> params = seqPatternParams(pattern);
    for (size_t i = kSeqPatternParamCount; i < params.size(); ++i) // past its length and loop
        if (value(params[i]) != table_[params[i]].def)
            changes.push_back({params[i], table_[params[i]].def});
    if (changes.empty())
        return;
    if (pattern == seqPattern()) // what was picked goes with its notes
        seqSelection_.clear();
    controller().beginStep();
    editMany(changes);
    controller().endStep();
}

// Asked before every note of a pattern (and its curves) goes, and before a pattern pasted or copied over one that
// holds notes replaces them; undo brings them back.
Panel::ConfirmWords SeqPanel::confirmWords(int kind) const {
    if (kind == kConfirmOverwrite)
        return {trf(Str::ConfirmSeqOverwriteTitle, {seqPatternName(seqOverwrite_.to)}), tr(Str::ConfirmSeqOverwrite),
                tr(Str::ButtonOverwrite), false};
    return {trf(Str::ConfirmSeqClearTitle, {seqPatternName(seqClearing_)}), {}, tr(Str::ButtonClearSeq), false};
}

void SeqPanel::confirmed(int kind, int button) {
    if (kind == kConfirmClear && button == 1)
        clearSeqPattern(seqClearing_);
    if (kind == kConfirmOverwrite && button == 1)
        writeSeqPattern(seqOverwrite_.values, seqOverwrite_.to, seqOverwrite_.done);
    if (kind == kConfirmOverwrite)
        seqOverwrite_ = {};
}

void SeqPanel::askClearSeqPattern(int pattern) {
    if (seqPatternEmpty(pattern))
        return;
    seqClearing_ = pattern;
    host_.askConfirm(*this, kConfirmClear);
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

void SeqPanel::copySeqPattern(int from, int to) {
    offerSeqPattern(seqPatternValues(from), to, trf(Str::MsgSeqCopied, {seqPatternName(from), seqPatternName(to)}));
}

std::vector<int> SeqPanel::seqPatternParams(int pattern) const {
    std::vector<int> params;
    for (int f = 0; f < kSeqPatternParamCount; ++f)
        params.push_back(paramIndex(pattern, static_cast<SeqPatternParam>(f)));
    for (int slot = 0; slot < kSeqNotes; ++slot)
        for (int f = 0; f < kSeqNoteParamCount; ++f)
            params.push_back(seqNoteIndex(pattern, slot, static_cast<SeqNoteParam>(f)));
    for (int c = 0; c < kSeqCurves; ++c)
        for (int f = 0; f < kSeqCurveParamCount; ++f)
            params.push_back(seqCurveCountIndex(pattern, static_cast<SeqCurve>(c)) + f);
    for (int slot = 0; slot < kSeqNotes; ++slot) // the notes' bends
        for (int f = 0; f < kSeqNoteBendParamCount; ++f)
            params.push_back(seqNoteBendCountIndex(pattern, slot) + f);
    return params;
}

std::vector<double> SeqPanel::seqPatternValues(int pattern) const {
    std::vector<double> values;
    for (int index : seqPatternParams(pattern))
        values.push_back(value(index));
    return values;
}

void SeqPanel::writeSeqPattern(const std::vector<double> &values, int to, const std::string &done) {
    const std::vector<int> params = seqPatternParams(to);
    if (values.size() != params.size())
        return;
    std::vector<std::pair<int, double>> changes;
    for (size_t i = 0; i < params.size(); ++i)
        if (value(params[i]) != values[i])
            changes.push_back({params[i], values[i]});
    if (!changes.empty()) {
        controller().beginStep();
        editMany(changes);
        controller().endStep();
    }
    host_.flash(done);
}

void SeqPanel::copySeqPatternToClip(int pattern) {
    if (pattern < 0 || pattern >= kSeqPatterns)
        return;
    seqPatternClip_ = seqPatternValues(pattern);
    host_.flash(trf(Str::MsgSeqPatternCopied, {seqPatternName(pattern)}));
}

void SeqPanel::pasteSeqPattern(int to) {
    if (!seqPatternClip_.empty())
        offerSeqPattern(seqPatternClip_, to, trf(Str::MsgSeqPatternPasted, {seqPatternName(to)}));
}

void SeqPanel::offerSeqPattern(std::vector<double> values, int to, std::string done) {
    if (to < 0 || to >= kSeqPatterns)
        return;
    if (seqPatternEmpty(to) || values == seqPatternValues(to)) { // nothing there to lose
        writeSeqPattern(values, to, done);
        return;
    }
    seqOverwrite_.values = std::move(values);
    seqOverwrite_.to = to;
    seqOverwrite_.done = std::move(done);
    host_.askConfirm(*this, kConfirmOverwrite);
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
        std::array<double, kSeqSlotParamCount> fields{};
        const std::vector<int> params = seqSlotParams(pattern, slot);
        for (size_t f = 0; f < fields.size(); ++f)
            fields[f] = value(params[f]);
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
        const std::vector<int> params = seqSlotParams(pattern, slot);
        for (size_t f = 0; f < fields.size(); ++f)
            changes.push_back({params[f], f == static_cast<size_t>(SeqNoteParam::Start) ? std::max(start, 0.0) : fields[f]});
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
enum SeqMenu {
    kSeqCut, kSeqCopy, kSeqPaste, kSeqDuplicate, kSeqDelete, kSeqSelectAll, kSeqSlide, kSeqMute, kSeqQuantize,
    kSeqCopyPattern, kSeqPastePattern, kSeqClearPattern
};
} // namespace

bool SeqPanel::contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) {
    if (hit && wide() && hit->kind == Kind::SeqTile) { // a pattern's tile: copying, pasting over and clearing it
        seqMenuTile_ = hit->b;
        items.push_back({kMenuCommands + kSeqCopyPattern, tr(Str::MenuSeqCopyPattern)});
        if (!seqPatternClip_.empty())
            items.push_back({kMenuCommands + kSeqPastePattern, tr(Str::MenuSeqPastePattern)});
        if (!seqPatternEmpty(seqMenuTile_)) {
            items.push_back({0, ""});
            items.push_back({kMenuCommands + kSeqClearPattern, tr(Str::MenuSeqClearPattern)});
        }
        return true;
    }
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
    case kSeqCopyPattern:
        copySeqPatternToClip(seqMenuTile_);
        break;
    case kSeqPastePattern:
        pasteSeqPattern(seqMenuTile_);
        break;
    case kSeqClearPattern:
        askClearSeqPattern(seqMenuTile_);
        break;
    case kSeqCut:
    case kSeqCopy:
        host_.sendKey(command == kSeqCut ? Key::Cut : Key::Copy, false); // as the keys do, through the editor
        break;
    case kSeqPaste: // where the menu was opened
        if (seqPaste(seqClip_, seqMenuAt_))
            seqClipNext_ = seqMenuAt_ + seqClip_.span;
        break;
    case kSeqDuplicate:
        host_.sendKey(Key::Duplicate, false);
        break;
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
    case kFieldBend:
        return bendText(seqBendExtreme(slot), false);
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
// Its snap's list and list of macros (the scale's list is the View's, see PanelHost::openList()).
void SeqPanel::closePopups() {
    seqSnapMenu_ = false;
    seqMacroMenu_ = false;
}

bool SeqPanel::key(Key key, bool shift) {
    if (!wide())
        return false;
    // With Bend, Delete removes the bend points picked, and Ctrl+A over the roll picks every one.
    if (seqBendTool() && key == Key::Delete && !seqBendPicked().empty()) {
        removeSeqBendPoints(seqBendPicked());
        return true;
    }
    if (seqBendTool() && key == Key::SelectAll && roll().grid.contains(host_.mouse())) {
        std::vector<BendHit> all;
        for (const SeqNote &n : seqNotes())
            for (int i = 0; i < static_cast<int>(seqBendPoints(n.slot).size()); ++i)
                all.push_back({n.slot, i});
        setSeqBendPick(std::move(all));
        return true;
    }
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
    // Over a pattern's tile, Ctrl+C copies its pattern and Ctrl+V pastes the one copied over it.
    if ((key == Key::Copy || key == Key::Paste) && host_.hovered() && host_.hovered()->kind == Kind::SeqTile) {
        if (key == Key::Copy)
            copySeqPatternToClip(host_.hovered()->b);
        else if (!seqPatternClip_.empty())
            pasteSeqPattern(host_.hovered()->b);
        else
            return false;
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
            if (n.start < seqSteps()) // not those past the pattern's end
                seqSelection_.push_back(n.slot);
        return true;
    }
    if (key == Key::Paste) { // at the step under the mouse on the roll, else after what was copied or pasted last
        if (seqClip_.notes.empty())
            return false;
        const Point mouse = host_.mouse();
        const double at = roll().grid.contains(mouse)
                              ? std::clamp(seqSnapped(seqStepAt(mouse.x), false), 0.0, static_cast<double>(seqSteps()))
                              : seqClipNext_;
        if (seqPaste(seqClip_, at))
            seqClipNext_ = at + seqClip_.span;
        return true;
    }
    const std::vector<int> picked = seqPicked();
    if (picked.empty() || (key != Key::Left && key != Key::Right && key != Key::Up && key != Key::Down &&
                           key != Key::Delete && key != Key::Copy && key != Key::Cut && key != Key::Duplicate))
        return false;
    if (key == Key::Duplicate) { // right after them, leaving what was copied as it is; undone, they are picked again
        const SeqClip clip = seqCopyPicked();
        controller().beginStep();
        if (seqPaste(clip, clip.from + clip.span))
            controller().keepSeqPicks(pattern, picked, seqPicked());
        controller().endStep();
        return true;
    }
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
            const double end = start + value(seqNoteIndex(pattern, slot, SeqNoteParam::Length));
            by = std::clamp(by, std::min(-start, 0.0), std::max(seqSteps() - end, 0.0));
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
        seqBlip(picked);
        return true;
    }
    controller().beginStep();
    editMany(changes);
    seqDropCovered(picked);
    controller().endStep();
    if (key == Key::Up || key == Key::Down) {
        seqReveal(picked, key == Key::Up);
        seqBlip(picked);
    }
    return true;
}

void SeqPanel::seqBlip(const std::vector<int> &slots) {
    seqBlipEnd();
    const int pattern = seqPattern();
    for (int slot : slots) {
        if (value(seqNoteIndex(pattern, slot, SeqNoteParam::Length)) <= 0.0)
            continue; // dropped, covered by another
        const int pitch = stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127);
        if (std::find(seqBlipPitches_.begin(), seqBlipPitches_.end(), pitch) == seqBlipPitches_.end())
            seqBlipPitches_.push_back(pitch);
    }
    for (int pitch : seqBlipPitches_)
        controller().auditionNote(pitch, true);
    seqBlipUntil_ = host_.now() + kSeqBlipSeconds;
}

void SeqPanel::seqBlipEnd() {
    for (int pitch : seqBlipPitches_)
        controller().auditionNote(pitch, false);
    seqBlipPitches_.clear();
}

void SeqPanel::stepApplied(const UndoStep &step, bool forward) {
    if (step.seqPattern != seqPattern()) // it keeps no notes picked, or another pattern shows
        return;
    seqSelection_ = forward ? step.seqPickedAfter : step.seqPickedBefore;
    seqSelectionPattern_ = step.seqPattern;
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
        layout.seqBend = w.b == kToolBend;
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
        askClearSeqPattern(seqPattern());
        return true;
    case Kind::SeqSize:
        toggleSize();
        return true;
    case Kind::SeqPreview: // played or stopped, as it shows; switched off, nothing
        if (seqOn())
            controller().playSeqPreview(!host_.telemetry().seqPreview);
        return true;
    case Kind::SeqTile: {
        // What the roll shows, in the shown pattern's tile: dragged, it scrolls the roll (see seqDrag()).
        if (seqViewGrabbed(w.b, p)) {
            const std::vector<int> rows = seqRows();
            startDrag(SeqDrag::Mode::ViewRect, false);
            seqDrag_.grabStep = seqScroll();
            seqDrag_.grabPitch = rows.empty() ? 0 : rows.front();
            return true;
        }
        // Elsewhere, the tile: shown once let go of unmoved, copied once let go of on another tile (see drop()).
        startDrag(SeqDrag::Mode::Tile, false);
        seqDrag_.anchor = w.b;
        return true;
    }
    case Kind::SeqRoot: // up and down, as a knob
        host_.dragValue(w, p, mods);
        return true;
    case Kind::SeqFold:
        layout.seqFold = !layout.seqFold;
        return true;
    case Kind::SeqLaneTab:
        seqLastMacro_ = seqTabMacro();
        layout.seqLane = static_cast<SeqLane>(w.b);
        return true;
    case Kind::SeqMacroTab: // the macro its label names, the list left shut
        showSeqMacro(seqTabMacro());
        seqMacroMenu_ = false;
        return true;
    case Kind::SeqMacroArrow:
        seqMacroMenu_ = true;
        return true;
    case Kind::SeqMacroMenu:
        if (w.a == 0)
            seqMacroMenu_ = false;
        return true;
    case Kind::SeqMacroItem:
        showSeqMacro(w.b);
        seqMacroMenu_ = false;
        return true;
    case Kind::SeqZoom:
        zoomSeq(w.b < 2, w.b % 2 == 1 ? 1.5f : 1.0f / 1.5f, roll().grid.center());
        return true;
    case Kind::SeqScrollY: { // as the bar across: on the thumb, drag it; elsewhere it jumps to center on the click
        const Rect thumb = seqScrollYThumb();
        const float total = static_cast<float>(seqPitchList().size());
        if (p.y < thumb.y || p.y >= thumb.y + thumb.h)
            seqShowFromRow((roll().scrollY.y + roll().scrollY.h - p.y) / roll().scrollY.h * total -
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
            if (!seqDrawing()) { // a box across the lane, picking the points it reaches
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
        if (!seqDrawing()) {
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
        // With Bend: a note's point dragged (with the other points picked), a segment's middle bending it, and a note
        // pressed putting a point down where the mouse is and dragging it (from a point at its start at its pitch, where
        // it had none), or with Shift drawing its bend; each an undo step, the note picked and heard at its bend while
        // the button is held. Points are picked as notes are: a point clicked picked alone (on one of several picked,
        // once it is let go unmoved), with Shift or Ctrl along with the others (or let go, once let go unmoved), and a
        // box from empty space picking the points it holds (Shift or Ctrl adding them).
        if (seqBendTool()) {
            seqBendJustAdded_ = {};
            auto hearBent = [&](int slot, double semitones) {
                seqHear(stepped(value(seqNoteIndex(pattern, slot, SeqNoteParam::Pitch)), 0, 127));
                controller().auditionBend(semitones);
            };
            auto pick = [&](int slot) {
                if (!seqSelected(slot))
                    seqSelection_ = {slot};
            };
            const bool adds = mods.shift || mods.ctrl;
            if (const BendHit h = seqBendPointAt(p); h.slot >= 0) {
                std::vector<BendHit> picked = seqBendPicked();
                const bool was = std::find(picked.begin(), picked.end(), h) != picked.end();
                bool unpick = false, narrow = false;
                if (adds && was)
                    unpick = true;
                else if (adds)
                    picked.push_back(h);
                else if (!was)
                    picked = {h};
                else
                    narrow = picked.size() > 1;
                setSeqBendPick(picked);
                picked = seqBendPicked();
                startDrag(picked.size() > 1 ? SeqDrag::Mode::BendGroup : SeqDrag::Mode::BendPoint, true);
                seqDrag_.anchor = h.slot;
                seqDrag_.bendIndex = h.index;
                seqDrag_.free = mods.alt || mods.shift;
                seqDrag_.unpick = unpick;
                seqDrag_.narrow = narrow;
                if (picked.size() > 1) {
                    for (const BendHit &q : picked)
                        seqDrag_.bendOrigin.push_back(seqBendPoints(q.slot)[static_cast<size_t>(q.index)]);
                    seqDrag_.bendPoints = std::move(picked);
                }
                pick(h.slot);
                hearBent(h.slot, seqBendPoints(h.slot)[static_cast<size_t>(h.index)].y);
                return true;
            }
            if (const BendHit h = seqBendSegmentAt(p); h.slot >= 0) {
                startDrag(SeqDrag::Mode::BendCurve, true);
                seqDrag_.anchor = h.slot;
                seqDrag_.bendIndex = h.index;
                seqDrag_.startCurve = seqBendPoints(h.slot)[static_cast<size_t>(h.index)].curve;
                pick(h.slot);
                return true;
            }
            if (const std::optional<BendLineHit> line = seqBendLineAt(p)) { // a point put down on the line, as it runs
                startDrag(SeqDrag::Mode::BendPoint, true);
                seqDrag_.anchor = line->slot;
                seqDrag_.free = mods.alt;
                seqDrag_.bendIndex = addSeqBendPoint(line->slot, line->point);
                seqBendJustAdded_ = {line->slot, seqDrag_.bendIndex};
                setSeqBendPick(seqDrag_.bendIndex >= 0 ? std::vector<BendHit>{seqBendJustAdded_} : std::vector<BendHit>{});
                pick(line->slot);
                hearBent(line->slot, line->point.y);
                return true;
            }
            if (const int note = seqBendNoteAt(p); note >= 0) {
                const std::optional<SeqNote> n = seqNoteOf(note);
                pick(note);
                if (mods.shift) { // a point on every line of the grid the stroke crosses, a step apart with the snap off
                    const double grid = kSeqSnaps[std::clamp(layout.seqSnap, 0, kSeqSnapCount - 1)];
                    const double unit = grid > 0.0 ? grid : 1.0;
                    const SeqCurvePoint at = seqBendPointFor(*n, p, false);
                    const double step = at.x;
                    startDrag(SeqDrag::Mode::BendPaint, true);
                    seqBendPick_ = BendPick{};
                    seqDrag_.anchor = note;
                    seqDrag_.loopFrom = step;
                    seqDrag_.startCurve = at.y;
                    paintSeqBend(note, step, at.y, step, at.y, unit);
                    hearBent(note, at.y);
                    return true;
                }
                startDrag(SeqDrag::Mode::BendPoint, true);
                seqDrag_.anchor = note;
                seqDrag_.free = mods.alt;
                const SeqCurvePoint at = seqBendPointFor(*n, p, mods.alt);
                seqDrag_.bendIndex = addSeqBendPoint(note, at);
                seqBendJustAdded_ = {note, seqDrag_.bendIndex};
                setSeqBendPick(seqDrag_.bendIndex >= 0 ? std::vector<BendHit>{seqBendJustAdded_} : std::vector<BendHit>{});
                hearBent(note, at.y);
                return true;
            }
            startDrag(SeqDrag::Mode::BendBox, false); // a box picking the points it holds
            seqDrag_.bendPoints = adds ? seqBendPicked() : std::vector<BendHit>{};
            setSeqBendPick(seqDrag_.bendPoints);
            return true;
        }
        bool atEnd = false, atStart = false;
        const int hit = seqNoteAt(p, &atEnd, &atStart);
        if (hit < 0 && seqDrawing()) {
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
            seqDrag_.boxRow = static_cast<float>(seqFirstRow()) + (roll().grid.y + roll().grid.h - p.y) / seqRowH();
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
                const std::vector<int> from = seqSlotParams(pattern, slot), to = seqSlotParams(pattern, copy);
                for (size_t f = 0; f < from.size(); ++f)
                    changes.push_back({to[f], value(from[f])});
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
    seqDrag_.carried = seqDrag_.carried || d.moved; // a tile, off its place
    // A note moved, or its end, follows the mouse off the grid while Shift or Alt is held.
    if (seqDrag_.mode == SeqDrag::Mode::Move || seqDrag_.mode == SeqDrag::Mode::Resize ||
        seqDrag_.mode == SeqDrag::Mode::ResizeStart)
        seqDrag_.free = mods.shift || mods.alt;
    // A point of the lane's curve, or a stroke drawing it, on the grid while Ctrl is held; a note bend's point on the
    // roll off the grid while Shift or Alt is, and in the Note Bend lane on it while Ctrl is.
    if (seqDrag_.mode == SeqDrag::Mode::CurvePoint || seqDrag_.mode == SeqDrag::Mode::CurvePaint)
        seqDrag_.free = !mods.ctrl;
    if (seqDrag_.mode == SeqDrag::Mode::BendPoint || seqDrag_.mode == SeqDrag::Mode::BendGroup)
        seqDrag_.free = mods.shift || mods.alt;
    // A note held after a double-click: the cursor left and right or up and down as the mouse last went, once it has
    // gone a few pixels, so a hand's tremble doesn't flip it.
    if (seqDrag_.mode == SeqDrag::Mode::Velocity && seqDrag_.extend) {
        const float dx = std::abs(p.x - seqDrag_.turnedAt.x), dy = std::abs(p.y - seqDrag_.turnedAt.y);
        if (std::max(dx, dy) >= 4.0f) {
            seqDrag_.across = dx > dy;
            seqDrag_.turnedAt = p;
        }
    }
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
                latest = std::max(latest, n.start + n.length);
            }
            // Every note within the pattern, its end too. (Never past the low bound: a note already longer than what is
            // left of the pattern moves no further right, but the bounds never cross.)
            shift = std::clamp(shift, std::min(-earliest, 0.0), std::max(steps - latest, 0.0));
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
        } else { // the end of the note grabbed onto the grid, the others' ends as far, none past the pattern's end
            const double end = anchor->start + anchor->length;
            const double shift = onGrid(end + moved) - end;
            for (const SeqNote &n : d.origin) {
                const double shortest = grid > 0.0 ? std::min(grid, n.length) : kShortestNote;
                changes.push_back({seqNoteIndex(pattern, n.slot, SeqNoteParam::Length),
                                   std::clamp(n.length + shift, shortest, std::max(steps - n.start, shortest))});
            }
        }
        editMany(changes);
        break;
    }
    case SeqDrag::Mode::Box: { // from where it started on the roll, which may have scrolled since
        const Point from = seqBoxStart();
        const float x0 = std::min(from.x, p.x), x1 = std::max(from.x, p.x);
        const float y0 = std::min(from.y, p.y), y1 = std::max(from.y, p.y);
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
            if (moved >= 0.0) { // as far as the pattern's end
                const double length = std::clamp(onGrid(end + moved) - o.start, shortest, std::max(steps - o.start, shortest));
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
        if (!roll().grid.contains(p))
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
        const double moved = (d.start.y - p.y) / (roll().laneArea.h - 12.0f);
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
        if (d.anchor < 0 || d.anchor >= seqCurveSegments(points))
            break;
        const size_t i = static_cast<size_t>(d.anchor);
        // A bend further up lowers a rising middle.
        const double rise = seqCurveSegmentEnd(points, d.anchor).y > points[i].y ? 1.0 : -1.0;
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
        const double level = std::clamp((roll().laneArea.y + roll().laneArea.h - 6.0f - p.y) / (roll().laneArea.h - 12.0f), 0.0f, 1.0f);
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
                       (d.start.y - p.y) / roll().scrollY.h * static_cast<float>(seqPitchList().size()));
        break;
    case SeqDrag::Mode::Audition:
        d.anchor = seqPitchAt(p.y);
        seqHear(d.anchor);
        break;
    case SeqDrag::Mode::ViewRect: { // the roll scrolled as far as the rectangle goes in the tile, across and up and down
        const TileScale scale = seqTileScale(pattern);
        if (scale.stepW > 0.0f) {
            seqScrollX_ = static_cast<float>(d.grabStep) + (p.x - d.start.x) / scale.stepW;
            seqScrollX_ = seqScroll();
        }
        if (scale.rowH > 0.0f) {
            const int low = std::clamp(d.grabPitch + static_cast<int>(std::lround((d.start.y - p.y) / scale.rowH)), 0, 127);
            const std::vector<int> pitches = seqPitchList();
            seqShowFromRow(static_cast<float>(std::lower_bound(pitches.begin(), pitches.end(), low) - pitches.begin()));
        }
        break;
    }
    case SeqDrag::Mode::BendPoint: { // across between its neighbors (within the note), and up and down
        const std::optional<SeqNote> n = seqNoteOf(d.anchor);
        std::vector<SeqCurvePoint> points = seqBendPoints(d.anchor);
        if (!n || d.bendIndex < 0 || d.bendIndex >= static_cast<int>(points.size()))
            break;
        const size_t i = static_cast<size_t>(d.bendIndex);
        const double lo = i > 0 ? points[i - 1].x : 0.0;
        const double hi = i + 1 < points.size() ? points[i + 1].x : std::max(n->length, lo);
        const SeqCurvePoint to = seqBendPointFor(*n, p, d.free);
        points[i].x = std::clamp(to.x, lo, std::max(lo, hi));
        points[i].y = to.y;
        setSeqBend(d.anchor, points);
        if (d.sounding >= 0) // heard at the pitch it bends to
            controller().auditionBend(to.y);
        break;
    }
    case SeqDrag::Mode::BendGroup:
        moveSeqBendPoints(p);
        break;
    case SeqDrag::Mode::BendBox: { // the points inside it, and those picked before it
        const Rect box{std::min(d.start.x, p.x), std::min(d.start.y, p.y), std::abs(p.x - d.start.x),
                       std::abs(p.y - d.start.y)};
        std::vector<BendHit> picked = d.bendPoints;
        for (const SeqNote &n : seqNotes()) {
            const std::vector<SeqCurvePoint> points = seqBendPoints(n.slot);
            for (int i = 0; i < static_cast<int>(points.size()); ++i)
                if (box.contains({seqStepX(n.start + points[static_cast<size_t>(i)].x),
                                  seqBendY(n, points[static_cast<size_t>(i)].y)}))
                    picked.push_back({n.slot, i});
        }
        setSeqBendPick(std::move(picked));
        break;
    }
    case SeqDrag::Mode::BendCurve: { // its middle follows the mouse up and down, as far as a bend takes it
        std::vector<SeqCurvePoint> points = seqBendPoints(d.anchor);
        if (d.bendIndex < 0 || d.bendIndex + 1 >= static_cast<int>(points.size()))
            break;
        const size_t i = static_cast<size_t>(d.bendIndex);
        const double rise = points[i + 1].y > points[i].y ? 1.0 : -1.0; // a bend further up lowers a rising middle
        points[i].curve = std::clamp(d.startCurve - rise * (d.start.y - p.y) / 40.0, -1.0, 1.0);
        setSeqBend(d.anchor, points);
        break;
    }
    case SeqDrag::Mode::BendPaint: {
        const std::optional<SeqNote> n = seqNoteOf(d.anchor);
        if (!n)
            break;
        const double snap = kSeqSnaps[std::clamp(controller().layout().seqSnap, 0, kSeqSnapCount - 1)];
        const SeqCurvePoint at = seqBendPointFor(*n, p, false);
        if (std::abs(at.x - d.loopFrom) < 1e-9)
            break; // still on the line it painted last
        paintSeqBend(d.anchor, d.loopFrom, d.startCurve, at.x, at.y, snap > 0.0 ? snap : 1.0);
        d.loopFrom = at.x;
        d.startCurve = at.y;
        if (d.sounding >= 0)
            controller().auditionBend(at.y);
        break;
    }
    case SeqDrag::Mode::Tile: // carried (see paintDrag()) until it is let go of
    case SeqDrag::Mode::None:
        break;
    }
}

void SeqPanel::drop(const DragState &d) {
    seqHear(-1);
    const SeqDrag::Mode mode = seqDrag_.mode;
    const bool moved = d.moved;
    if (mode == SeqDrag::Mode::Tile) {
        const int from = seqDrag_.anchor, to = seqTileAt(d.now);
        seqDrag_ = SeqDrag{};
        if (!moved) {
            if (from != seqPattern())
                edit(paramIndex(SeqParam::Pattern), from);
        } else if (to >= 0 && to != from) {
            copySeqPattern(from, to);
        }
        return;
    }
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
    if ((mode == SeqDrag::Mode::BendPoint || mode == SeqDrag::Mode::BendGroup) && !moved &&
        (seqDrag_.unpick || seqDrag_.narrow)) {
        // A bend point clicked: with Shift or Ctrl, one picked let go; without, one of several picked picked alone.
        std::vector<BendHit> picked = seqBendPicked();
        const BendHit clicked{seqDrag_.anchor, seqDrag_.bendIndex};
        if (seqDrag_.unpick)
            std::erase(picked, clicked);
        else
            picked = {clicked};
        setSeqBendPick(std::move(picked));
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

// A note bend's point double-clicked goes (but one the press before put down, which stays; a lone point left at 0
// going with it, as it bends nothing), and a segment's middle straightens.
void SeqPanel::doubleClickBend(Point p) {
    if (const BendHit h = seqBendPointAt(p); h.slot >= 0) {
        if (h == seqBendJustAdded_) {
            seqBendJustAdded_ = {};
            return;
        }
        std::vector<SeqCurvePoint> points = seqBendPoints(h.slot);
        points.erase(points.begin() + h.index);
        if (points.size() == 1 && std::abs(points.front().y) < 1e-9)
            points.clear();
        controller().beginStep();
        setSeqBend(h.slot, points);
        controller().endStep();
        return;
    }
    if (const BendHit h = seqBendSegmentAt(p); h.slot >= 0) {
        std::vector<SeqCurvePoint> points = seqBendPoints(h.slot);
        points[static_cast<size_t>(h.index)].curve = 0.0;
        controller().beginStep();
        setSeqBend(h.slot, points);
        controller().endStep();
    }
}

bool SeqPanel::doubleClick(const Widget &w, Point p, Modifiers mods) {
    const int pattern = seqPattern();
    switch (w.kind) {
    case Kind::SeqGrid: { // on a note, deletes it; elsewhere, puts one down; with Bend, its bend's points and segments
        if (seqBendTool()) {
            doubleClickBend(p);
            return true;
        }
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
            seqDrag_.across = true; // left and right until the mouse goes up or down
            seqDrag_.grabStep = seqStepAt(p.x);
            for (const SeqNote &n : seqNotes())
                if (n.slot == slot)
                    seqDrag_.origin.push_back(n);
            seqDrag_.start = seqDrag_.now = seqDrag_.turnedAt = p;
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
    case Kind::SeqField: { // the notes picked back to velocity 100, a full chance, no offset off the grid, or no bend
        const std::vector<int> picked = seqPicked();
        if (!picked.empty() && w.b == kFieldBend) {
            controller().beginStep();
            for (int slot : picked)
                setSeqBend(slot, {});
            controller().endStep();
            return true;
        }
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
        if (w.b == 4) // onto the grid: what they land on goes, as Quantize has it
            seqDropCovered(picked);
        controller().endStep();
        return true;
    }
    case Kind::SeqTool:
    case Kind::SeqSnap:
    case Kind::SeqMacroTab:
    case Kind::SeqMacroArrow:
    case Kind::SeqStretch:
    case Kind::SeqFlag:
        return true; // a double-click is two clicks of theirs, which the first press took
    default:
        return false;
    }
}

bool SeqPanel::wheelOn(const Widget &w, Point p, float steps, Modifiers mods) {
    UiLayout &layout = controller().layout();
    const bool onRoll = w.kind == Kind::SeqGrid || w.kind == Kind::SeqKeys || w.kind == Kind::SeqRuler ||
                      w.kind == Kind::SeqLane || w.kind == Kind::SeqScroll || w.kind == Kind::SeqScrollY ||
                      w.kind == Kind::SeqZoom || w.kind == Kind::SeqLaneTab || w.kind == Kind::SeqMacroTab ||
                      w.kind == Kind::SeqMacroArrow;
    // Over the roll, Ctrl zooms across and Alt up and down, at the mouse; Shift scrolls across, as the wheel turned
    // sideways does.
    if (onRoll && mods.ctrl) {
        zoomSeq(true, std::pow(1.25f, steps), p);
        return true;
    }
    if (onRoll && mods.alt) {
        zoomSeq(false, std::pow(1.25f, steps), p);
        return true;
    }
    if (onRoll && (mods.shift || w.kind == Kind::SeqScroll || w.kind == Kind::SeqRuler)) {
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
    case Kind::SeqMacroTab: // along the list, as the snap's button does
    case Kind::SeqMacroArrow:
        showSeqMacro(seqTabMacro() + (steps > 0.0f ? -1 : 1));
        return true;
    case Kind::SeqZoom: // its axis, in or out a notch
        zoomSeq(w.b < 2, std::pow(1.25f, steps), roll().grid.center());
        return true;
    case Kind::SeqLane:
    case Kind::SeqTool:
    case Kind::SeqStretch:
    case Kind::SeqFlag:
    case Kind::SeqClear:
    case Kind::SeqFold:
    case Kind::SeqLaneTab:
        return true;
    default:
        return false;
    }
}

bool SeqPanel::wheelAcross(Point p, float steps) {
    if (!(wide() && roll().box.contains(p)))
        return false;
    seqScrollX_ = seqScroll() + steps * seqShownSteps() * 0.125f; // the roll, an eighth of what it shows a notch
    seqScrollX_ = seqScroll();
    return true;
}

// The roll, across and up and down (one part, 0): what was under the mouse follows it, never held at an end.
std::optional<std::pair<int, float>> SeqPanel::panAt(Point p) const {
    if (wide() && Rect{roll().box.x, roll().ruler.y, roll().box.w, roll().laneArea.y + roll().laneArea.h - roll().ruler.y}.contains(p))
        return std::pair{0, seqScroll()};
    return std::nullopt;
}

void SeqPanel::panBegin(int) {
    seqPanStart_ = {seqScroll(), static_cast<float>(seqFirstRow())};
}

std::optional<float> SeqPanel::pan(int, float, float across, float down) {
    seqScrollX_ = seqPanStart_.x - across / roll().grid.w * seqShownSteps();
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
    if (w.kind == Kind::SeqTile)
        return trf(Str::TipSeqTile, {seqPatternName(w.b)});
    if (w.kind == Kind::SeqSize)
        return tr(full() ? Str::TipSeqHalf : Str::TipSeqFull);
    if (w.kind == Kind::SeqPreview)
        return tr(!seqOn()                       ? Str::TipSeqPreviewOff
                  : host_.telemetry().seqPreview ? Str::TipSeqPreviewStop
                                                 : Str::TipSeqPreviewPlay);
    switch (w.kind) {
    case Kind::SeqTool:
        return tr(w.b == kToolBend ? Str::TipSeqBend : w.b == 1 ? Str::TipSeqDraw : Str::TipSeqSelect);
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
    case Kind::SeqMacroTab:
        return seqMacroMenu_ ? std::string() : std::string(tr(Str::TipSeqLaneMacro));
    case Kind::SeqMacroArrow:
        return seqMacroMenu_ ? std::string() : std::string(tr(Str::TipSeqLaneMacroPick));
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

// --- PATTERN as a modulation target.

namespace {
bool patternButton(const Widget &w) {
    return w.kind == WidgetKind::Choice && w.param == paramIndex(SeqParam::Pattern);
}
Rect patternButtons() { return seqPatternRow(); } // the four, merged into one row
} // namespace

bool SeqPanel::takesSource(const Widget &w, int source) const {
    return wide() && patternButton(w) && host_.modulationModel().canModulate(source, w.param);
}

void SeqPanel::paintSourceRing(Canvas &c, const Widget &w, bool over, Color color) {
    if (w.b != 0) // the row's ring, once
        return;
    const bool on = over || patternButtons().contains(host_.mouse());
    c.strokeRect(patternButtons().inset(-2.0f), color.alpha(on ? 1.0f : 0.35f), on ? 2.0f : 1.0f, kRadiusControl + 2.0f);
}

bool SeqPanel::dropSource(const Widget &w, int source) {
    if (!takesSource(w, source))
        return false;
    host_.modulationModel().assign(source, w.param);
    return true;
}

// The pattern modulation picks, as the dot on its reach shows it: as the sequencer plays it, or with nothing playing,
// where the macros and the mod wheel take it (the voices' sources take it nowhere then). Ringed among the buttons while
// it isn't the one set.
int SeqPanel::seqPatternModulated() const {
    const int pattern = paramIndex(SeqParam::Pattern);
    return std::clamp(static_cast<int>(std::lround(value(pattern) + host_.modulatedBy(pattern))), 0, kSeqPatterns - 1);
}

// --- The patterns' overview.

int SeqPanel::seqTileAt(Point p) const {
    for (int pattern = 0; pattern < kSeqPatterns; ++pattern)
        if (seqTile(pattern).contains(p))
            return pattern;
    return -1;
}

// A pattern's tile's notes area, as its tile draws it: the longest pattern's steps across, and up and down its own
// notes' pitches, at least an octave of them (none while it has no notes).
SeqPanel::TileScale SeqPanel::seqTileScale(int pattern) const {
    TileScale scale;
    scale.notes = seqTileNotes(seqTile(pattern));
    int longest = 1;
    for (int p = 0; p < kSeqPatterns; ++p)
        longest = std::max(longest, seqStepsOf(p));
    scale.steps = seqStepsOf(pattern);
    scale.stepW = scale.notes.w / static_cast<float>(longest);
    int low = 128, high = -1;
    for (const SeqNote &n : seqNotesOf(pattern)) {
        low = std::min(low, n.pitch);
        high = std::max(high, n.pitch);
    }
    if (high >= low) {
        const int span = std::max(high - low + 1, 12);
        scale.from = static_cast<float>(low) - 0.5f * static_cast<float>(span - (high - low + 1));
        scale.rowH = scale.notes.h / static_cast<float>(span);
    }
    return scale;
}

// What the roll shows of the pattern shown, in its tile: across the steps it shows and up and down its rows (as far as
// the tile reaches; all the way while the pattern has no notes to scale its rows by). None for another pattern.
std::optional<Rect> SeqPanel::seqViewRect(int pattern) const {
    if (pattern != seqPattern())
        return std::nullopt;
    const TileScale scale = seqTileScale(pattern);
    const Rect r = scale.notes;
    const float x0 = r.x + seqScroll() * scale.stepW, x1 = x0 + seqShownSteps() * scale.stepW;
    float y0 = r.y, y1 = r.bottom();
    if (const std::vector<int> rows = seqRows(); scale.rowH > 0.0f && !rows.empty()) {
        y0 = std::clamp(r.bottom() - (static_cast<float>(rows.back()) - scale.from + 1.0f) * scale.rowH, r.y,
                        r.bottom() - 2.0f);
        y1 = std::clamp(r.bottom() - (static_cast<float>(rows.front()) - scale.from) * scale.rowH, y0 + 2.0f,
                        r.bottom());
    }
    return Rect{std::round(x0) + 0.5f, std::round(y0) + 0.5f, std::round(x1) - std::round(x0) - 1.0f,
                std::round(y1) - std::round(y0) - 1.0f};
}

// Whether a press at `p` on a pattern's tile grabs what the roll shows of it: on the rectangle (or just round it), while
// the roll has somewhere to scroll to, across or up and down the tile's rows. Otherwise the tile is grabbed, to pick
// its pattern or copy it.
bool SeqPanel::seqViewGrabbed(int pattern, Point p) const {
    const std::optional<Rect> view = seqViewRect(pattern);
    if (!view || !view->inset(-2.0f).contains(p))
        return false;
    const Rect notes = seqTileNotes(seqTile(pattern));
    return seqShownSteps() < static_cast<float>(seqSteps()) - 1e-3f || view->h < notes.h - 3.0f;
}

// A pattern's tile: its letter, lit while it is the one shown, and its notes drawn small after it (see
// seqTileScale(); bar lines in it, outside its loop dimmed), fainter the softer it is and muted faintest; the pattern
// shown what the roll shows of it (seqViewRect(), brighter under the mouse and while it is dragged), and the playhead
// while it plays. Ringed while it is shown, and while one carried would land on it.
void SeqPanel::paintSeqTile(Canvas &c, int pattern, Rect tile, bool hovered, bool carried) {
    const bool shown = pattern == seqPattern();
    const bool landing = !carried && dragging() && seqDrag_.mode == SeqDrag::Mode::Tile && seqDrag_.carried &&
                         pattern != seqDrag_.anchor && seqTileAt(seqDrag_.now) == pattern;
    const Color light = lightTheme() ? Color{0.0f, 0.0f, 0.0f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 1.0f};
    c.fillRect(tile, wellColor(), kRadiusControl);
    if (shown || landing)
        c.fillRect(tile, kAccent.alpha(landing ? 0.2f : 0.12f), kRadiusControl);
    else if (hovered)
        c.fillRect(tile, light.alpha(0.04f), kRadiusControl);
    c.text({tile.x, tile.y, kSeqTileLetterW, tile.h}, seqPatternName(pattern), kTextBody,
           shown ? kAccent : hovered ? kText : kDim, Align::Center, true);

    const TileScale scale = seqTileScale(pattern);
    const Rect r = scale.notes;
    const int steps = scale.steps;
    const float stepW = scale.stepW;
    const float end = r.x + static_cast<float>(steps) * stepW;
    c.fillRect({r.x, r.y, end - r.x, r.h}, light.alpha(0.05f), kRadiusMark);
    for (int bar = kSeqStepsPerBar; bar < steps; bar += kSeqStepsPerBar) {
        const float x = std::round(r.x + static_cast<float>(bar) * stepW) + 0.5f;
        c.line({x, r.y}, {x, r.bottom()}, light.alpha(0.08f), 1.0f);
    }
    if (scale.rowH > 0.0f) {
        const float h = std::clamp(scale.rowH - 1.0f, 1.5f, 4.0f);
        c.pushClip({r.x, r.y, end - r.x, r.h});
        for (const SeqNote &n : seqNotesOf(pattern)) {
            const float x = r.x + static_cast<float>(n.start) * stepW;
            const float w = std::max(static_cast<float>(n.length) * stepW - 1.0f, 1.5f);
            const float y = r.bottom() - (static_cast<float>(n.pitch) - scale.from + 0.5f) * scale.rowH - 0.5f * h;
            const float alpha = n.mute ? 0.3f : 0.55f + 0.45f * static_cast<float>(std::clamp(n.velocity, 0.0, 1.0));
            c.fillRect({x, y, w, h}, kAccent.alpha(alpha), std::min(1.0f, 0.5f * h));
        }
        c.popClip();
    }
    const auto [loopStart, loopEnd] = seqLoopOf(pattern);
    if (loopStart > 0)
        c.fillRect({r.x, r.y, static_cast<float>(loopStart) * stepW, r.h}, wellColor().alpha(0.6f));
    if (loopEnd < steps)
        c.fillRect({r.x + static_cast<float>(loopEnd) * stepW, r.y, static_cast<float>(steps - loopEnd) * stepW, r.h},
                   wellColor().alpha(0.6f));
    if (const std::optional<Rect> view = seqViewRect(pattern); view && !carried) {
        const bool hot = (dragging() && seqDrag_.mode == SeqDrag::Mode::ViewRect) ||
                         (!dragging() && hovered && seqViewGrabbed(pattern, host_.mouse()));
        c.fillRect(*view, light.alpha(hot ? 0.1f : 0.06f), kRadiusMark);
        c.strokeRect(*view, kText.alpha(hot ? 0.85f : 0.55f), 1.0f, kRadiusMark);
    }
    const Telemetry &t = host_.telemetry();
    if (t.seqPosition >= 0.0f && t.seqPattern == pattern && t.seqPosition <= static_cast<float>(steps)) {
        const float x = std::round(r.x + t.seqPosition * stepW) + 0.5f;
        c.line({x, r.y - 2.0f}, {x, r.bottom() + 2.0f}, kText, 1.0f);
    }
    if (landing)
        c.strokeRect(tile, kAccent, 1.5f, kRadiusControl);
    else if (shown)
        c.strokeRect(tile, kAccent.alpha(0.6f), 1.0f, kRadiusControl);
}

// A tile carried onto another, half seen, following the mouse.
void SeqPanel::paintDrag(Canvas &c) {
    if (!wide() || !dragging() || seqDrag_.mode != SeqDrag::Mode::Tile || !seqDrag_.carried)
        return;
    const Rect from = seqTile(seqDrag_.anchor);
    const Point offset{seqDrag_.now.x - seqDrag_.start.x, seqDrag_.now.y - seqDrag_.start.y};
    c.pushLayer(offset, 0.6f, from.inset(-2.0f));
    paintSeqTile(c, seqDrag_.anchor, from, true, true);
    c.popLayer();
}

// --- Painting.

void SeqPanel::paintBack(Canvas &c) {
    const int pattern = seqPattern();
    // The card, as the inspector's lens; at the full size, the patterns' overview under it, a card alike; and the roll's
    // box.
    auto face = [&](Rect r) {
        softShadow(c, r, kRadiusPanel, 4, 1.25f, 4.0f, lightTheme() ? 0.05f : 0.16f);
        c.fillRect(r, kRaised.alpha(0.92f), kRadiusPanel);
        if (outlined())
            c.strokeRect(r, kAccent.alpha(0.35f), 1.0f, kRadiusPanel);
    };
    face(kSeqCard);
    if (full())
        face(kSeqOverview);
    c.fillRect(roll().box, wellColor(), kRadiusCard);
    // Switched off, all that is on them is drawn dimmed, but the power switch and the size button (whose widgets the
    // View draws as they are; the others it dims, see offAreas()).
    struct Dim {
        Canvas &c;
        bool on;
        Dim(Canvas &canvas, bool dim) : c(canvas), on(dim) {
            if (on)
                c.pushLayer({0.0f, 0.0f}, kDimOpacity, kSeqBand);
        }
        ~Dim() {
            if (on)
                c.popLayer();
        }
    } dim(c, !seqOn());
    c.text({kSeqCardX, kSeqTitleY, 200.0f, 22.0f}, tr(Str::HeadingSequencer), kTextBody, kText, Align::Left, true);
    auto label = [&](float y, Str name, float x = kSeqCardX, float w = 60.0f) {
        c.text({x, y, w, 22.0f}, tr(name), kTextLabel, kDim, Align::Left, true);
    };
    label(kSeqPatternY, Str::LabelSeqPattern);
    label(kSeqPlayY, Str::LabelSeqPlay);
    label(kSeqOrderY, Str::LabelSeqOrder);
    label(kSeqScaleY, Str::LabelSeqScale);
    label(kSeqTools.y + 1.0f, Str::LabelSeqSnap, kSeqSnapLabelX, 36.0f);
    // The overview's title; its tiles are widgets (see paintSeqTile()).
    if (full())
        c.text({kSeqCardX, kSeqOverviewTitleY, 200.0f, 22.0f}, tr(Str::HeadingSeqPatterns), kTextBody, kText,
               Align::Left, true);

    // The roll.
    c.pushClip(roll().box);
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
        const float y = roll().grid.y + roll().grid.h - (i + 1) * rh;
        // The rows' ground follows the keys alone, whatever the scale: a black key's darker, a white key's a little
        // lighter than the well; folded, the well as it is.
        if (!fold)
            c.fillRect({roll().grid.x, y, roll().grid.w, rh},
                       isBlackKey(pitch) ? kBackground.alpha(0.35f)
                                         : Color{1.0f, 1.0f, 1.0f, lightTheme() ? 0.4f : 0.03f});
        c.line({roll().grid.x, y + rh}, {roll().grid.x + roll().grid.w, y + rh},
               light.alpha((pitch % 12 == 0 ? 0.10f : 0.035f) * gridLines), 1.0f);
    }
    const float stepW = roll().grid.w / seqShownSteps(); // a step's width, as zoomed
    const int firstStep = static_cast<int>(std::floor(seqScroll()));
    const int lastStep = std::min(static_cast<int>(std::ceil(seqScroll() + seqShownSteps())), steps);
    c.pushClip({roll().grid.x, roll().ruler.y, roll().grid.w, roll().laneArea.y + roll().laneArea.h - roll().ruler.y});
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
            c.line({x, roll().ruler.y}, {x, roll().laneArea.y + roll().laneArea.h}, light.alpha(alpha * gridLines), 1.0f);
        }
        // Beats the snap's lines miss (a triplet's), drawn all the same.
        for (int s = (firstStep / 4) * 4; s <= lastStep; s += 4)
            if (std::abs(s / every - std::round(s / every)) > 1e-6)
                c.line({seqStepX(s), roll().ruler.y}, {seqStepX(s), roll().laneArea.y + roll().laneArea.h},
                       light.alpha((s % kSeqStepsPerBar == 0 ? 0.16f : 0.08f) * gridLines), 1.0f);
    }
    // The loop: what lies outside it dimmed, and its bar on the ruler.
    const int loopStart = stepped(value(paramIndex(pattern, SeqPatternParam::LoopStart)), 0, steps - 1);
    const int storedEnd = stepped(value(paramIndex(pattern, SeqPatternParam::LoopEnd)), 0, kSeqMaxSteps);
    const int loopEnd = storedEnd <= 0 ? steps : std::clamp(storedEnd, loopStart + 1, steps);
    const float lx0 = seqStepX(loopStart), lx1 = seqStepX(loopEnd);
    if (lx0 > roll().grid.x)
        c.fillRect({roll().grid.x, roll().grid.y, lx0 - roll().grid.x, roll().grid.h + kSeqLaneH}, kBackground.alpha(0.45f));
    if (lx1 < roll().grid.x + roll().grid.w)
        c.fillRect({lx1, roll().grid.y, roll().grid.x + roll().grid.w - lx1, roll().grid.h + kSeqLaneH}, kBackground.alpha(0.45f));
    c.fillRect({roll().ruler.x, roll().ruler.y, roll().ruler.w, roll().ruler.h}, kPanel.alpha(0.6f));
    c.fillRect({lx0, roll().ruler.y + 3.0f, lx1 - lx0, 5.0f}, kAccent.alpha(0.85f), kRadiusMark);
    for (int bar = 0; bar < steps / kSeqStepsPerBar; ++bar)
        for (int beat = 0; beat < 4; ++beat) {
            const float x = seqStepX(bar * kSeqStepsPerBar + beat * 4);
            if (x < roll().grid.x - 40.0f || x > roll().grid.x + roll().grid.w || (beat > 0 && stepW * 4.0f < 30.0f))
                continue; // out of sight, or no room for the beats' numbers
            const std::string text = beat == 0 ? std::to_string(bar + 1)
                                               : std::to_string(bar + 1) + "." + std::to_string(beat + 1);
            c.text({x + 3.0f, roll().ruler.y + 7.0f, 40.0f, 13.0f}, text, kTextSmall, beat == 0 ? kDim : kFaint, Align::Left);
        }
    c.popClip();

    // The notes: brighter as they are louder, dashed at a chance below full, hollow while muted; those picked ringed.
    const std::vector<SeqNote> notes = seqNotes();
    const Color ink = kAccent;
    c.pushClip(roll().grid);
    for (const SeqNote &n : notes) {
        const Rect r = seqNoteRect(n);
        if (r.y > roll().grid.y + roll().grid.h)
            continue; // its row isn't shown
        const bool picked = seqSelected(n.slot);
        // Full strength at full velocity, down to 40 % at none; faint with Bend, which edits the bends drawn over them.
        const float level =
            static_cast<float>(0.4 + 0.6 * std::clamp(n.velocity, 0.0, 1.0)) * (seqBendTool() ? 0.6f : 1.0f);
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
    paintSeqBends(c, notes);
    // The box being dragged, picking what it reaches.
    if (dragging() && seqDrag_.mode == SeqDrag::Mode::Box) {
        const Point from = seqBoxStart(), to = seqDrag_.now;
        const Rect box{std::min(from.x, to.x), std::min(from.y, to.y), std::abs(to.x - from.x), std::abs(to.y - from.y)};
        c.fillRect(box, kAccent.alpha(0.12f));
        c.strokeRect(box, kAccent.alpha(0.7f), 1.0f);
    }
    c.popClip();
    if (notes.empty())
        c.text(roll().grid, tr(Str::HintSeqEmpty), kTextLabel, kFaint);

    // The keys: white ones across the strip, the black ones shorter over them; or with the rows folded to the scale,
    // each row's name.
    c.pushClip(roll().keys);
    if (fold) {
        c.fillRect(roll().keys, kPanel);
        for (size_t i = 0; i < rows.size(); ++i) {
            const float y = roll().keys.y + roll().keys.h - (i + 1) * rh;
            const bool outside = scale != 0 && !seqInScale(scale, root, rows[i]); // a row a note off the scale has
            if (outside)
                c.fillRect({roll().keys.x, y, roll().keys.w, rh}, kBackground.alpha(0.55f));
            c.text({roll().keys.x, y, roll().keys.w - 6.0f, rh}, pitchName(rows[i]), kTextSmall,
                   outside ? kFaint : rows[i] % 12 == root % 12 ? kText : kDim, Align::Right);
        }
    } else {
        const Color white = lightTheme() ? Color::hex(0xFFFFFF) : Color::hex(0xD6D9E4);
        const Color blackKey = Color::hex(0x1B1C25);
        const Color seam = Color::hex(0x8E92A6);
        c.fillRect(roll().keys, white);
        const float bw = roll().keys.w * 0.6f;
        for (size_t i = 0; i < rows.size(); ++i) {
            const int pitch = rows[i];
            const float y = roll().keys.y + roll().keys.h - (i + 1) * rh;
            const int pc = pitch % 12;
            if (pc == 4 || pc == 11) // the seams between two white keys side by side
                c.line({roll().keys.x, y}, {roll().keys.x + roll().keys.w, y}, seam, 1.0f);
            if (isBlackKey(pitch)) {
                c.line({roll().keys.x + bw, y + rh * 0.5f}, {roll().keys.x + roll().keys.w, y + rh * 0.5f}, seam, 1.0f);
                c.fillRect({roll().keys.x - 3.0f, y + 0.6f, bw + 3.0f, rh - 1.2f}, blackKey, kRadiusMark);
            }
            if (pc == 0)
                c.text({roll().keys.x, y, roll().keys.w - 4.0f, rh}, pitchName(pitch), kTextSmall, Color::hex(0x5D6075),
                       Align::Right, true);
        }
    }
    // The keys sounding: those whose notes the sequencer plays now, as written, and the one heard while it is held,
    // each lit as far as it has eased (see seqFollowKeys()).
    for (size_t i = 0; i < rows.size(); ++i) {
        const float lit = seqKeyLight_[static_cast<size_t>(std::clamp(rows[i], 0, 127))];
        if (lit > 0.0f)
            c.fillRect({roll().keys.x, roll().keys.y + roll().keys.h - (i + 1) * rh, roll().keys.w, rh},
                       kAccent.alpha(0.6f * lit));
    }
    c.popClip();
    c.line({roll().keys.x + roll().keys.w, roll().keys.y}, {roll().keys.x + roll().keys.w, roll().keys.y + roll().keys.h}, kBorder, 1.0f);

    // The lane: a stem for each note at its start, as high as its velocity or chance.
    const bool velocityLane = controller().layout().seqLane == SeqLane::Velocity;
    // A slight line parting it from the roll, a pixel high: the faint text's color stands off the wells in every theme,
    // where the borders' all but melts into them.
    c.fillRect({roll().box.x, roll().laneArea.y, roll().box.w, 1.0f}, kFaint.alpha(0.55f));
    const Color stem = velocityLane ? kAccent : look.sources[1];
    c.pushClip(roll().laneArea);
    std::vector<SeqNote> stems = notes; // the picked ones last, over the others
    std::stable_partition(stems.begin(), stems.end(), [&](const SeqNote &n) { return !seqSelected(n.slot); });
    if (seqCurveLane()) {
        // A curve instead: the line it rests on (the bend's 0, the mod wheel's bottom), the curve across the pattern,
        // filled down to that line, its points and the middles of the segments that bend.
        stems.clear();
        const SeqCurve curve = seqLaneCurve();
        const int shownMacro = seqCurveMacro(curve);
        const Color curveInk = look.sources[shownMacro >= 0              ? kMaxSources + shownMacro
                                            : curve == SeqCurve::Bend ? kBendSource
                                                                      : kWheelSource];
        const float rest = seqCurveY(curve, 0.0);
        const float right = std::min(roll().grid.x + roll().grid.w, seqStepX(steps));
        // The grid: a line at each quarter of the range (what Ctrl puts points on), as strong as the GRID setting says;
        // across, the roll's lines run on down through the lane.
        for (int k = 0; k <= 4; ++k) {
            const float y = seqCurveY(curve, curve == SeqCurve::Bend ? -1.0 + k * 0.5 : k * 0.25);
            c.line({roll().grid.x, y}, {right, y}, light.alpha(0.05f * gridLines), 1.0f);
        }
        c.line({roll().grid.x, rest}, {right, rest}, kBorder, 1.0f);
        const std::vector<SeqCurvePoint> points = seqCurvePoints(curve);
        if (!points.empty() && right > roll().grid.x) {
            std::vector<Point> line;
            for (float x = roll().grid.x;; x += 2.0f) {
                const float at = std::min(x, right);
                const double v = seqCurveAt(static_cast<int>(points.size()),
                                            [&](int i) { return points[static_cast<size_t>(i)]; }, seqStepAt(at), seqSteps());
                line.push_back({at, seqCurveY(curve, v)});
                if (at >= right)
                    break;
            }
            std::vector<Point> area = line;
            area.push_back({line.back().x, rest});
            area.push_back({line.front().x, rest});
            c.fillPolygon(area.data(), area.size(), curveInk.alpha(0.16f));
            c.polyline(line.data(), line.size(), curveInk, 1.5f);
            const std::vector<int> picked = seqCurvePicked();
            const int hoveredPoint = dragging() && seqDrag_.mode == SeqDrag::Mode::CurvePoint
                                         ? seqDrag_.anchor
                                         : seqCurvePointAt(host_.mouse());
            const int hoveredBend = dragging() && seqDrag_.mode == SeqDrag::Mode::CurveBend
                                        ? seqDrag_.anchor
                                        : seqCurveBendAt(host_.mouse());
            for (int i = 0; i < seqCurveSegments(points); ++i) {
                if (seqCurveBends(points, i))
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
        const float bottom = roll().laneArea.y + roll().laneArea.h - 6.0f;
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
    c.line({roll().box.x, roll().laneTabs.y}, {roll().box.x + roll().box.w, roll().laneTabs.y}, kBorder, 1.0f);
    // The zoom's arrows, before their buttons: across, and up and down.
    {
        const Color arrowInk = kFaint;
        const float y = roll().laneTabs.y + roll().laneTabs.h * 0.5f;
        const float ax = seqZoomButton(roll(), 0).x - 12.0f;
        c.line({ax - 6.0f, y}, {ax + 6.0f, y}, arrowInk, 1.2f);
        const Point l[] = {{ax - 3.0f, y - 3.0f}, {ax - 6.0f, y}, {ax - 3.0f, y + 3.0f}};
        const Point r[] = {{ax + 3.0f, y - 3.0f}, {ax + 6.0f, y}, {ax + 3.0f, y + 3.0f}};
        c.polyline(l, 3, arrowInk, 1.2f);
        c.polyline(r, 3, arrowInk, 1.2f);
        const float vx = seqZoomButton(roll(), 2).x - 10.0f;
        c.line({vx, y - 6.0f}, {vx, y + 6.0f}, arrowInk, 1.2f);
        const Point u[] = {{vx - 3.0f, y - 3.0f}, {vx, y - 6.0f}, {vx + 3.0f, y - 3.0f}};
        const Point d[] = {{vx - 3.0f, y + 3.0f}, {vx, y + 6.0f}, {vx + 3.0f, y + 3.0f}};
        c.polyline(u, 3, arrowInk, 1.2f);
        c.polyline(d, 3, arrowInk, 1.2f);
    }

    // Where it plays now, while it plays this pattern.
    const Telemetry &telemetry = host_.telemetry();
    if (telemetry.seqPosition >= 0.0f && telemetry.seqPattern == pattern &&
        seqStepX(telemetry.seqPosition) >= roll().grid.x && seqStepX(telemetry.seqPosition) <= roll().grid.x + roll().grid.w) {
        const float x = seqStepX(telemetry.seqPosition);
        c.line({x, roll().ruler.y}, {x, roll().laneArea.y + roll().laneArea.h}, kText.alpha(0.8f), 1.2f);
        const Point head[] = {{x - 5.0f, roll().ruler.y + 9.0f}, {x + 5.0f, roll().ruler.y + 9.0f}, {x, roll().ruler.y + 15.0f}};
        c.fillPolygon(head, 3, kText);
    }
    c.popClip();
}

Point SeqPanel::seqLaneDot(const SeqNote &note) const {
    const double v = controller().layout().seqLane == SeqLane::Velocity ? note.velocity : note.chance;
    const float bottom = roll().laneArea.y + roll().laneArea.h - 6.0f;
    return {seqStepX(note.start) + 1.5f, bottom - static_cast<float>(std::clamp(v, 0.0, 1.0)) * (roll().laneArea.h - 12.0f)};
}

int SeqPanel::seqLaneNoteAt(Point p) const {
    const std::vector<SeqNote> notes = seqNotes();
    int found = -1;
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
        const Point dot = seqLaneDot(*it);
        if (std::abs(p.x - dot.x) <= 5.0f && p.y >= dot.y - 6.0f && p.y <= roll().laneArea.y + roll().laneArea.h) {
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
    const float reach = std::max(3.0f, roll().grid.w / seqShownSteps() * 0.5f);
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
    return seqLoopOf(seqPattern());
}

std::pair<int, int> SeqPanel::seqLoopOf(int pattern) const {
    const int steps = seqStepsOf(pattern);
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
    // A note held after a double-click, whose drag is on no widget: left and right while the mouse goes across, setting
    // its length, up and down while it goes up and down, setting its velocity.
    if (seqDrag_.mode == SeqDrag::Mode::Velocity && seqDrag_.extend)
        return seqDrag_.across ? CursorKind::Horizontal : CursorKind::Vertical;
    if (w.kind == Kind::SeqTile) { // a hand to pick it up; moving while it is carried, and over what the roll shows
        if (dragging())
            return seqDrag_.mode == SeqDrag::Mode::ViewRect || (seqDrag_.mode == SeqDrag::Mode::Tile && seqDrag_.carried)
                       ? CursorKind::Move
                       : CursorKind::Hand;
        return seqViewGrabbed(w.b, host_.mouse()) ? CursorKind::Move : CursorKind::Hand;
    }
    if (w.kind == Kind::SeqRuler) { // left and right over an end of the loop or a new one, moving over the loop
        const SeqDrag::Mode mode = dragging() ? seqDrag_.mode : seqRulerMode(host_.mouse().x);
        return mode == SeqDrag::Mode::LoopMove ? CursorKind::Move : CursorKind::Horizontal;
    }
    // With Bend, a note bend's point moves, a segment's middle goes up and down, the arrow stays over a bend's line (a
    // faint point showing where a press puts one down), a note drags up and down (bending it) where its bend runs over
    // its bar (elsewhere the arrow: the bar takes nothing there), a box picking points is the arrow and a stroke drawing its
    // bend is a pen.
    if (w.kind == Kind::SeqGrid && seqBendTool()) {
        if (dragging())
            return seqDrag_.mode == SeqDrag::Mode::BendCurve   ? CursorKind::Vertical
                   : seqDrag_.mode == SeqDrag::Mode::BendPoint || seqDrag_.mode == SeqDrag::Mode::BendGroup
                       ? CursorKind::Move
                   : seqDrag_.mode == SeqDrag::Mode::BendPaint ? CursorKind::Pen
                                                               : CursorKind::Arrow;
        if (seqBendPointAt(host_.mouse()).slot >= 0)
            return CursorKind::Move;
        if (seqBendSegmentAt(host_.mouse()).slot >= 0)
            return CursorKind::Vertical;
        if (seqBendLineAt(host_.mouse()))
            return CursorKind::Arrow;
        return seqBendNoteAt(host_.mouse()) >= 0 ? CursorKind::Vertical : CursorKind::Arrow;
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
        return seqDrawing() ? std::optional<CursorKind>(CursorKind::Pen) : std::nullopt;
    }
    if (w.kind == Kind::SeqLane) { // up and down over a dot, or moving them; with Draw a pen elsewhere, drawing heights
        if (dragging())
            return seqDrag_.mode == SeqDrag::Mode::LaneBox                                ? CursorKind::Arrow
                   : seqDrag_.mode == SeqDrag::Mode::LaneGroup || seqDrag_.anchor >= 0 ? CursorKind::Vertical
                                                                                         : CursorKind::Pen;
        if (seqLaneNoteAt(host_.mouse()) >= 0)
            return CursorKind::Vertical;
        return seqDrawing() ? CursorKind::Pen : CursorKind::Arrow;
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
    return seqDrawing() ? CursorKind::Pen : CursorKind::Arrow;
}

bool SeqPanel::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const UiLayout &layout = controller().layout();
    if ((w.kind == Kind::Choice || w.kind == Kind::Toggle) && isSeqParam(w.param)) {
        const bool on = w.kind == Kind::Choice ? std::lround(value(w.param)) == w.b : value(w.param) >= 0.5;
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        if (patternButton(w) && !on && seqPatternModulated() == w.b) // where modulation takes the pattern picked
            c.strokeRect(w.rect.inset(1.5f), kAccent, 1.5f, kRadiusControl - 1.5f);
        if (patternButton(w) && w.b == kSeqPatterns - 1) // under the row, once: its modulation's reach, as a bar's
            host_.paintModRange(c, seqPatternReach(), w.param);
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
                                         : std::string(tr(plays[std::clamp(w.b, 0, 2)]));
            c.text(w.rect, text, kTextLabel, on ? litInk(kText) : kDim, Align::Center, on);
        }
        return true;
    }
    switch (w.kind) {
    case Kind::SeqTile:
        paintSeqTile(c, w.b, w.rect, hovered, false);
        return true;
    case Kind::SeqPreview: {
        // The card's main action; while the pattern plays, Stop, lit. Switched off, the View draws it dimmed with the
        // card, and it does nothing.
        const bool playing = host_.telemetry().seqPreview;
        const Color ink = paintMainActionFace(c, w.rect, hovered && seqOn(), playing);
        transportIcon(c, {w.rect.x + 15.0f, w.rect.center().y}, ink, playing);
        c.text({w.rect.x + 25.0f, w.rect.y, w.rect.w - 29.0f, w.rect.h}, tr(playing ? Str::SeqPreviewStop : Str::SeqPreviewPlay),
               kTextLabel, ink, Align::Left, true);
        return true;
    }
    case Kind::SeqSize: // the Curve card's expand and collapse button: out to the full size, in to the half
        if (const Color ink = paintActionFace(c, w.rect, hovered); full())
            collapseIcon(c, w.rect.center(), ink);
        else
            expandIcon(c, w.rect.center(), ink);
        return true;
    case Kind::SeqTool: {
        const bool on = w.b == kToolBend ? layout.seqBend : !layout.seqBend && layout.seqDraw == (w.b == 1);
        host_.paintButtonFace(c, w, on, hovered, kAccent, 0.22f, kRadiusControl);
        const Color ink = on ? litInk(kAccent) : hovered ? kText : kDim;
        const Point m = w.rect.center();
        if (w.b == 0) { // a dashed box, picking
            c.strokeRect({m.x - 6.0f, m.y - 5.0f, 12.0f, 10.0f}, ink, 1.2f, 1.0f, true);
        } else if (w.b == kToolBend) { // a line bending up off a level one, an arrowhead at its end
            c.line({m.x - 7.0f, m.y + 4.0f}, {m.x - 2.0f, m.y + 4.0f}, ink, 1.4f);
            c.curve({m.x - 2.0f, m.y + 4.0f}, {m.x + 3.0f, m.y + 4.0f}, {m.x + 5.0f, m.y - 4.0f}, ink, 1.4f);
            const Point head[] = {{m.x + 2.0f, m.y - 3.0f}, {m.x + 5.5f, m.y - 5.5f}, {m.x + 7.0f, m.y - 1.5f}};
            c.polyline(head, 3, ink, 1.3f);
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
        paintChevron(c, {w.rect.right() - 13.0f, w.rect.center().y}, lit ? kText : kDim);
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
    case Kind::SeqMacroTab: {
        // Its label, as the tabs before it: lit only while the lane shows a macro, with a dot in the macro's color
        // (faint while the macro has no curve in this pattern); under the mouse, faintly lit apart from the arrow.
        const int macro = seqTabMacro();
        auto get = [this](int index) { return value(index); };
        const bool curve = seqCurvePointCount(get, seqPattern(), seqMacroCurve(macro)) > 0;
        const bool shown = seqLaneMacro(layout.seqLane);
        if (shown || hovered)
            c.fillRect(w.rect, kAccent.alpha(shown ? 0.2f : 0.1f), kRadiusMark);
        const Color ink = shown || hovered ? kText : kDim;
        const float y = w.rect.center().y;
        c.fillCircle({w.rect.x + 10.0f, y}, 3.5f, look.sources[kMaxSources + macro].alpha(curve ? 1.0f : kSeqBareMacroDot));
        c.text({w.rect.x + 18.0f, w.rect.y, w.rect.w - 20.0f, w.rect.h}, trf(Str::SeqLaneMacro, {std::to_string(macro + 1)}),
               kTextSmall, ink, Align::Left, true);
        return true;
    }
    case Kind::SeqMacroArrow: {
        // The arrow that opens the list: lit apart from the label, while the list is open and under the mouse, a thin line
        // parting the two while either is lit.
        const bool labelLit = seqLaneMacro(layout.seqLane) ||
                              (host_.hovered() && host_.hovered()->kind == Kind::SeqMacroTab);
        if (seqMacroMenu_ || hovered)
            c.fillRect(w.rect, kAccent.alpha(seqMacroMenu_ ? 0.2f : 0.1f), kRadiusMark);
        if (seqMacroMenu_ || hovered || labelLit)
            c.line({w.rect.x + 0.5f, w.rect.y + 3.0f}, {w.rect.x + 0.5f, w.rect.bottom() - 3.0f}, kFaint, 1.0f);
        paintChevron(c, w.rect.center(), seqMacroMenu_ || hovered ? kText : kDim,
                     seqMacroMenu_ ? ChevronWay::Up : ChevronWay::Down, true); // up while the list is open
        return true;
    }
    case Kind::SeqMacroMenu:
        if (w.a == 1)
            paintMenuPanel(c, w.rect);
        return true;
    case Kind::SeqMacroItem: {
        // A macro: its color, its name, and its curve in this pattern drawn small; without one, the color faint and a
        // faint line.
        const Color color = look.sources[kMaxSources + w.b];
        const std::vector<SeqCurvePoint> points = seqCurvePoints(seqMacroCurve(w.b));
        const bool selected = layout.seqLane == static_cast<SeqLane>(static_cast<int>(SeqLane::Macro1) + w.b);
        paintMenuRow(c, w.rect, hovered, selected);
        const float y = w.rect.center().y;
        c.fillCircle({w.rect.x + 11.0f, y}, 4.0f, color.alpha(points.empty() ? kSeqBareMacroDot : 1.0f));
        const Rect name{w.rect.x + 22.0f, w.rect.y, w.rect.w - 22.0f - 56.0f, w.rect.h};
        c.pushClip(name);
        c.text(name, macroName(layout, w.b), kTextBody, hovered || selected || !points.empty() ? kText : kDim, Align::Left,
               selected);
        c.popClip();
        const Rect spark{w.rect.right() - 50.0f, y - 6.0f, 44.0f, 12.0f};
        if (points.empty()) {
            c.line({spark.x, y}, {spark.right(), y}, kFaint.alpha(0.6f), 1.0f);
        } else {
            std::array<Point, 23> line;
            for (size_t i = 0; i < line.size(); ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(line.size() - 1);
                const double v = seqCurveAt(static_cast<int>(points.size()),
                                            [&](int p) { return points[static_cast<size_t>(p)]; }, t * seqSteps(), seqSteps());
                line[i] = {spark.x + static_cast<float>(t) * spark.w,
                           spark.bottom() - static_cast<float>(std::clamp(v, 0.0, 1.0)) * spark.h};
            }
            c.polyline(line.data(), line.size(), color, 1.3f);
        }
        return true;
    }
    case Kind::SeqStretch:
        c.text(w.rect, tr(w.b > 0 ? Str::SeqDoubleMark : Str::SeqHalveMark), kTextLabel,
               paintActionFace(c, w.rect, hovered), Align::Center, true);
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
        binIcon(c, w.rect.center(), paintActionFace(c, w.rect, hovered, !seqPatternEmpty(seqPattern())));
        return true;
    }
    case Kind::SeqRoot: {
        const bool lit = hovered || host_.isActive(w);
        c.fillRect(w.rect, lit ? wellHover() : wellColor(), kRadiusControl);
        // The scale's root is a pitch class: its octave does nothing (the keys transpose a pattern from C4).
        c.text(w.rect, kPitchNames[stepped(value(w.param), 0, 127) % 12], kTextBody, lit ? kText : kDim, Align::Center);
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
        c.text(w.rect, tr(names[std::clamp(w.b, 0, static_cast<int>(SeqLane::Macro1) - 1)]), kTextSmall,
               on ? kText : hovered ? kText : kDim, Align::Center, true);
        return true;
    }
    case Kind::SeqZoom: {
        // In a well in a flat theme, whose raised face doesn't show, so they don't read as the arrows beside them.
        if (look.flat)
            c.fillRect(w.rect, hovered ? wellHover() : wellColor(), kRadiusControl);
        else
            paintActionFace(c, w.rect, hovered);
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
        const Rect track = across ? seqScrollTrack() : roll().scrollY;
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
