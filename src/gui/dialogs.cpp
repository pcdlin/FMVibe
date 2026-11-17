#include "view.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace fmvibe::gui {

using namespace detail;

std::vector<View::Widget> View::settingsWidgets() const {
    const Rect panel = settingsPanel(height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    add(Kind::SettingsBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::SettingsBackdrop, panel, 1);
    // Each row of buttons in one pill, as Poly | Mono.
    size_t row = list.size();
    for (int i = 0; i < static_cast<int>(std::size(kOversamplingFactors)); ++i)
        add(Kind::SettingsOption, settingsOption(panel, i), -1, kOversamplingFactors[i]);
    mergeButtons(list, row);
    row = list.size();
    for (int on = 0; on < 2; ++on)
        add(Kind::SettingsMpe, settingsMpe(panel, on), -1, on);
    mergeButtons(list, row);
    row = list.size();
    for (int i = 0; i < static_cast<int>(std::size(kMpeBendRanges)); ++i)
        add(Kind::SettingsMpeRange, settingsMpeRange(panel, i), -1, kMpeBendRanges[i]);
    mergeButtons(list, row);
    for (int i = 0; i < kThemeCount; ++i)
        add(Kind::SettingsTheme, settingsTheme(panel, i), -1, i);
    add(Kind::SettingsHue, settingsHue(panel));
    add(Kind::SettingsGlow, settingsGlow(panel));
    add(Kind::SettingsLinks, settingsLinks(panel));
    add(Kind::SettingsGrid, settingsGrid(panel));
    row = list.size();
    for (int size = 0; size < kModHandleSizeCount; ++size)
        add(Kind::SettingsHandles, settingsHandles(panel, size), -1, size);
    mergeButtons(list, row);
    add(Kind::SettingsClose, settingsClose(panel));
    return list;
}

namespace {
// A dialog's panel, as the theme's panels are: outlined, or lifted by a soft shadow in a theme that lifts its panels so.
void paintDialogPanel(Canvas &c, Rect panel) {
    if (look.panelShadow > 0.0f)
        softShadow(c, panel, kRadiusPanel, 6, 1.5f, 4.0f, std::min(look.panelShadow * 1.5f, 0.4f));
    c.fillRect(panel, kPanel, kRadiusPanel);
    if (look.panelShadow <= 0.0f)
        c.strokeRect(panel, kBorder, 1.0f, kRadiusPanel);
}
} // namespace

void View::paintSettings(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = settingsPanel(height());
    paintDialogPanel(c, panel);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 200.0f, 24.0f}, tr(Str::SettingsTitle), kTextHeading, kText, Align::Left, true);
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 60.0f, 200.0f, 16.0f}, tr(Str::SettingsOversampling), kTextLabel, kFaint, Align::Left,
           true);
    c.text({panel.x + 24.0f, panel.y + 122.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhat), kTextLabel, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 140.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhy), kTextLabel, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 158.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingSaved), kTextLabel, kFaint, Align::Left);
    // MPE: Off | On, then BEND and its ranges, and what it does under them.
    const float mpeY = panel.y + kSettingsMpeY;
    c.line({panel.x + 24.0f, mpeY + 0.5f}, {panel.right() - 24.0f, mpeY + 0.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, mpeY + 8.0f, 200.0f, 16.0f}, tr(Str::SettingsMpe), kTextLabel, kFaint, Align::Left, true);
    c.text(settingsMpeBendLabel(panel), tr(Str::SettingsMpeBend), kTextLabel, controller_.mpe() ? kDim : kFaint,
           Align::Left, true);
    c.text({panel.x + 24.0f, mpeY + 66.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeWhat), kTextLabel, kDim,
           Align::Left);
    c.text({panel.x + 24.0f, mpeY + 84.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeWhy), kTextLabel, kDim,
           Align::Left);
    c.text({panel.x + 24.0f, mpeY + 102.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeSaved), kTextLabel, kFaint,
           Align::Left);
    // LOOK: the themes, their main color's hue, the glow, the sources' lines and the depth handles' size.
    const float lookY = panel.y + kSettingsLookY;
    c.line({panel.x + 24.0f, lookY + 0.5f}, {panel.right() - 24.0f, lookY + 0.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, lookY + 8.0f, 200.0f, 16.0f}, tr(Str::SettingsLook), kTextLabel, kFaint, Align::Left, true);
    // A slider's name before it, and its amount after (Off at 0).
    auto slider = [&](Rect track, Str name, float amount) {
        c.text({panel.x + 24.0f, track.y, kSettingsLabelW, track.h}, tr(name), kTextLabel, kDim, Align::Left, true);
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f %%", amount * 100.0f);
        c.text({track.right() + 14.0f, track.y, panel.right() - 24.0f - track.right() - 14.0f, track.h},
               amount > 0.0f ? text : tr(Str::ValueOff), kTextLabel, amount > 0.0f ? kText : kDim, Align::Left);
    };
    { // HUE, and its hue in degrees after it, or Default while it is the theme's own
        const Rect track = settingsHue(panel);
        c.text({panel.x + 24.0f, track.y, kSettingsLabelW, track.h}, tr(Str::SettingsHue), kTextLabel, kDim, Align::Left,
               true);
        const double hue = shownHue();
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f\xC2\xB0", hue * 360.0);
        c.text({track.right() + 14.0f, track.y, panel.right() - 24.0f - track.right() - 14.0f, track.h},
               hue >= 0.0 ? text : tr(Str::SettingsHueDefault), kTextLabel, hue >= 0.0 ? kText : kDim, Align::Left);
    }
    slider(settingsGlow(panel), Str::SettingsGlow, shownGlow());
    slider(settingsLinks(panel), Str::SettingsLinks, shownLinks());
    slider(settingsGrid(panel), Str::SettingsGrid, shownGrid());
    const Rect handles = settingsHandles(panel, 0);
    c.text({panel.x + 24.0f, handles.y, kSettingsLabelW, handles.h}, tr(Str::SettingsHandles), kTextLabel, kDim,
           Align::Left, true);
    c.text({panel.x + 24.0f, lookY + 244.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsLookSaved), kTextLabel, kFaint,
           Align::Left);
    // The version and the license, at the bottom.
    c.line({panel.x + 24.0f, lookY + 270.5f}, {panel.right() - 24.0f, lookY + 270.5f}, kBorder, 1.0f);
    const Rect about{panel.x + 24.0f, lookY + 278.0f, panel.w - 48.0f, 16.0f};
    c.text(about, trf(Str::SettingsVersion, {FMVIBE_VERSION}), kTextLabel, kFaint, Align::Left, true);
    c.text(about, tr(Str::SettingsLicense), kTextLabel, kDim, Align::Right);
    for (const Widget &w : settingsWidgets())
        paintWidget(c, w);
}

// Save only for a preset loaded from a file and changed since; Save As for any preset, but for Init only once changed.
bool View::presetActionOn(PresetAction action) const {
    switch (action) {
    case PresetAction::Save:
        return controller_.presetHasFile() && controller_.patchEdited();
    case PresetAction::SaveAs:
        return controller_.presetHasFile() || controller_.patchEdited();
    default:
        return true;
    }
}

void View::runPreset(PresetAction action) {
    switch (action) {
    case PresetAction::Init:
        presetStore().resetPreset();
        break;
    case PresetAction::Load: // a file (a preset from the library loads through the browser: see PresetBrowser)
        presetStore().choosePreset(false);
        break;
    case PresetAction::Save: // over the preset's file; with none to write (the demo, a factory preset), as Save As
        if (!presetStore().writesPresetFiles())
            presetStore().choosePreset(true);
        else if (!presetStore().presetWritable())
            presets_.openSave();
        else if (!controller_.overwritePreset()) // saved, it says nothing: Save greys out again
            flash(tr(Str::MsgPresetNotSaved));
        break;
    case PresetAction::SaveAs: // the Save dialog, where presets are files; the demo says it has none
        if (presetStore().writesPresetFiles())
            presets_.openSave();
        else
            presetStore().choosePreset(true);
        break;
    }
}

// What a panel's question says is the panel's (the preset browser's: the preset picked from the library that replaces
// the patch, a folder or a category that goes; the SEQ view's: its pattern's notes; the FX view's: the effects that
// go). Of the View's own: what replaces the patch, Init (a file still to be picked in the dialog goes unsaid), or, for
// Save, what overwriting the preset's file does.
std::string View::confirmWhat() const {
    if (confirmFrom_)
        return confirmFrom_->confirmWords(confirm_).what;
    if (confirm_ == static_cast<int>(PresetAction::Init))
        return tr(Str::ConfirmInit);
    if (confirm_ == static_cast<int>(PresetAction::Save))
        return tr(Str::ConfirmOverwrite);
    return std::string();
}

void View::answerConfirm(int button) {
    const auto action = static_cast<PresetAction>(confirm_);
    const int kind = confirm_;
    Panel *const from = confirmFrom_;
    confirm_ = -1;
    confirmFrom_ = nullptr;
    hover_.reset();
    if (from) { // a panel's question (the preset browser's: a preset loaded, a folder or a category removed; the SEQ
                // view's: its pattern cleared; the FX view's: its effects removed, a split removed or in two bands)
        from->confirmed(kind, button);
    } else if (button == 1) {
        runPreset(action);
    }
}

bool View::dialogOpen() const {
    return settingsOpen_ || confirm_ >= 0 ||
           std::any_of(panels_.begin(), panels_.end(), [](const Panel *panel) { return panel->dialog(); });
}

bool View::dialogKey(Key key) {
    if (typing_ || !dialogOpen() || (key != Key::Enter && key != Key::Escape))
        return false;
    // The panels' (the preset browser's info card and the browser itself, the panel of algorithms).
    if (!settingsOpen_ && confirm_ < 0)
        for (Panel *panel : panels_)
            if (const std::optional<bool> taken = panel->dialogKey(key))
                return *taken;
    if (!settingsOpen_ && confirm_ < 0) { // a panel's dialog left the keys to the View: Escape is taken, Enter not
        if (key != Key::Escape)
            return false;
        hover_.reset();
        return true;
    }
    if (settingsOpen_) { // nothing in it waits to be confirmed: either key closes it
        settingsOpen_ = false;
        hover_.reset();
        return true;
    }
    // Enter presses the button lit in the accent; a dialog that lights none (a split's middle band's, as no way ahead is
    // more likely than the others) leaves it open. Escape cancels, as a click around the dialog does.
    if (key == Key::Escape)
        answerConfirm(0);
    else if (!confirmFrom_ || confirmFrom_->confirmWords(confirm_).lit)
        answerConfirm(1);
    return true;
}

// A line shorter with one line to show: before a file still to be picked loads, before Save overwrites a preset, or
// as a panel's question says (a split's removal).
bool View::confirmShorter() const {
    return confirmWhat().empty() || confirm_ == static_cast<int>(PresetAction::Save) ||
           (confirmFrom_ && confirmFrom_->confirmWords(confirm_).shorter);
}

std::vector<View::Widget> View::confirmWidgets() const {
    const std::optional<Panel::ConfirmWords> words =
        confirmFrom_ ? std::optional(confirmFrom_->confirmWords(confirm_)) : std::nullopt;
    const bool bands = words && !words->choices.empty(); // a split's middle band's: its choices over the others
    const Rect panel = bands ? confirmBandsPanel(height()) : confirmPanel(height(), confirmShorter());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::ConfirmBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::ConfirmBackdrop, panel, 1);
    if (bands)
        for (int choice = 2; choice < 2 + static_cast<int>(words->choices.size()); ++choice)
            add(Kind::ConfirmButton, confirmBandButton(panel, choice), choice);
    if (words && !words->third.empty()) // a third way (a split with its bands' effects)
        add(Kind::ConfirmButton, confirmThirdButton(panel), 4);
    add(Kind::ConfirmButton, confirmButton(panel, 0), 0);
    add(Kind::ConfirmButton, confirmButton(panel, 1), 1);
    return list;
}

namespace {
// ASCII letters in lower case, for a search that doesn't mind case.
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char &ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}
} // namespace

bool TextField::startsCharacter(size_t at) const {
    return at >= text.size() || (static_cast<unsigned char>(text[at]) & 0xC0) != 0x80;
}

size_t TextField::before(size_t at) const {
    at = std::min(at, text.size());
    if (at == 0)
        return 0;
    do
        --at;
    while (at > 0 && !startsCharacter(at));
    return at;
}

size_t TextField::after(size_t at) const {
    if (at >= text.size())
        return text.size();
    do
        ++at;
    while (!startsCharacter(at));
    return at;
}

size_t TextField::characters() const {
    size_t n = 0;
    for (size_t at = 0; at < text.size(); at = after(at))
        ++n;
    return n;
}

void TextField::set(std::string value) {
    text = std::move(value);
    size_t end = 0;
    for (size_t n = 0; n < most && end < text.size(); ++n)
        end = after(end);
    text.resize(end);
    caret = anchor = text.size();
    shift = 0.0f;
}

void TextField::erase(size_t from, size_t to) {
    text.erase(from, to - from);
    caret = anchor = from;
}

bool TextField::insert(std::string_view typed) {
    std::string clean;
    for (char ch : typed)
        if (ch == '\n' || ch == '\r' || ch == '\t')
            clean += ' ';
        else if (ch >= 0x20 && ch <= 0x7E)
            clean += ch;
    const auto [from, to] = selection();
    if (clean.empty() && from == to)
        return false;
    erase(from, to);
    const size_t held = characters(); // ASCII typed: a byte a character
    clean.resize(std::min(clean.size(), most - std::min(held, most)));
    text.insert(from, clean);
    caret = anchor = from + clean.size();
    return true;
}

void TextField::clear() {
    text.clear();
    caret = anchor = 0;
    shift = 0.0f;
}

size_t TextField::stopAt(float x) const {
    if (stops.size() != text.size() + 1)
        return text.size();
    size_t best = 0;
    for (size_t i = after(0); i < stops.size(); i = after(i)) { // between whole characters only
        if (std::abs(stops[i] - x) < std::abs(stops[best] - x))
            best = i;
        if (i == text.size())
            break;
    }
    return best;
}

void TextField::selectWord(float x) {
    const size_t at = stopAt(x);
    size_t from = at, to = at;
    while (from > 0 && text[from - 1] != ' ')
        --from;
    while (to < text.size() && text[to] != ' ')
        ++to;
    anchor = from;
    caret = to;
}

bool TextField::key(Key key, bool extend) {
    const auto [from, to] = selection();
    // The caret to `at`; extending (Shift held), the selection grows or shrinks with it, else there is none.
    auto moveTo = [&](size_t at) {
        caret = at;
        if (!extend)
            anchor = at;
    };
    switch (key) {
    case Key::Backspace: // the selection, or the character before the caret
    case Key::Delete:    // or after it
        if (from != to)
            erase(from, to);
        else if (key == Key::Backspace && caret > 0)
            erase(before(caret), caret);
        else if (key == Key::Delete && caret < text.size())
            erase(caret, after(caret));
        else
            return false;
        return true;
    case Key::Left: // without Shift, from a selection to its start
        moveTo(from != to && !extend ? from : before(caret));
        break;
    case Key::Right:
        moveTo(from != to && !extend ? to : after(caret));
        break;
    case Key::Home:
        moveTo(0);
        break;
    case Key::End:
        moveTo(text.size());
        break;
    case Key::SelectAll:
        anchor = 0;
        caret = text.size();
        break;
    case Key::Enter:
    case Key::Escape:
    case Key::Up:
    case Key::Down:
        break;
    }
    return false;
}

std::vector<std::string> TextField::words() const {
    std::vector<std::string> words;
    const std::string search = lowered(text);
    for (size_t at = 0; at < search.size();) {
        const size_t end = std::min(search.find(' ', at), search.size());
        if (end > at)
            words.push_back(search.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

bool TextField::holds(std::string_view text, const std::vector<std::string> &words) {
    const std::string in = lowered(text);
    return std::all_of(words.begin(), words.end(),
                       [&](const std::string &word) { return in.find(word) != std::string::npos; });
}

// A panel's form, while it decides (the preset browser's), under a confirm dialog; a field of a panel shown in its
// place (the MODS view's filter and its target menu's search), under the settings too.
std::optional<View::Field> View::fieldTyping() const {
    for (const Panel *panel : panels_)
        if (std::optional<Field> focus; panel->fieldFocus(focus))
            return confirm_ < 0 && (panel->modal() || !settingsOpen_) ? focus : std::nullopt;
    return std::nullopt;
}

View::TextField &View::field(Field f) {
    for (Panel *panel : panels_) // each field's owner
        if (TextField *t = panel->field(f))
            return *t;
    static TextField none; // every field has its owner: never reached
    return none;
}

// After what is typed changes: a panel's field as its panel has it (see PresetBrowser::fieldChanged() and
// ModsPanel::fieldChanged()).
void View::fieldChanged(Field f) {
    caretTicks_ = 0;
    for (Panel *panel : panels_)
        panel->fieldChanged(f);
}

void View::clearField(Field f) {
    field(f).clear();
    fieldChanged(f);
}

void View::paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint, std::string_view after,
                      bool lens) {
    TextField &t = field(f);
    const bool focused = fieldTyping() == f;
    c.fillRect(box, kBackground, kRadiusControl);
    // In a flat theme, a search is outlined only while it is typed in or holds words; a form's field always is, in the
    // accent while it has the keyboard.
    if (!lens)
        c.strokeRect(box, focused ? kAccent : kBorder, 1.0f, kRadiusControl);
    else if (outlined() || focused || !t.text.empty())
        c.strokeRect(box, focused || !t.text.empty() ? kAccent : kBorder, 1.0f, kRadiusControl);
    if (lens) {
        const Point at{box.x + 13.0f, box.center().y - 1.0f};
        c.strokeCircle(at, 4.0f, kDim, 1.3f);
        c.line({at.x + 3.0f, at.y + 3.0f}, {at.x + 6.5f, at.y + 6.5f}, kDim, 1.5f);
    }
    if (!after.empty()) { // faint at the right end, the text short of it
        const float w = std::ceil(c.textWidth(after, kTextLabel));
        c.text({typed.right() - w, typed.y, w, typed.h}, after, kTextLabel, kFaint, Align::Right);
        typed.w = std::max(typed.w - w - 8.0f, 20.0f);
    }
    // Text too long for it scrolls, so the caret stays in sight; where each place between its characters lands is
    // kept for the mouse.
    constexpr float kSize = kTextBody;
    std::vector<float> stops;
    for (size_t i = 0; i <= t.text.size(); ++i)
        stops.push_back(c.textWidth(std::string_view(t.text).substr(0, i), kSize));
    t.caret = std::min(t.caret, t.text.size());
    t.anchor = std::min(t.anchor, t.text.size());
    const float fits = typed.w - 2.0f, caretAt = stops[t.caret];
    if (caretAt - t.shift > fits)
        t.shift = caretAt - fits;
    t.shift = std::clamp(std::min(t.shift, caretAt), 0.0f, std::max(stops.back() - fits, 0.0f));
    for (float &stop : stops)
        stop += typed.x - t.shift;
    t.stops = stops;
    const auto [from, to] = t.selection();
    if (t.text.empty()) {
        c.text(typed, hint, kTextLabel, kFaint, Align::Left);
    } else {
        c.pushClip(typed);
        if (from != to)
            c.fillRect({stops[from], box.y + 5.0f, stops[to] - stops[from], box.h - 10.0f}, kAccent.alpha(0.35f), kRadiusMark);
        c.text({stops[0], typed.y, stops.back() - stops[0] + 20.0f, typed.h}, t.text, kSize, kText, Align::Left);
        c.popClip();
    }
    if (focused && from == to && caretTicks_ / 32 % 2 == 0) {
        const float caret = std::min(stops[t.caret] + 0.5f, typed.right());
        c.line({caret, box.y + 5.0f}, {caret, box.bottom() - 5.0f}, kText, 1.0f);
    }
}

// Steps through the library, round from the last to the first; from Init, to the first or the last.
void View::stepPreset(int direction) {
    presetStore().rescanPresets();
    const int count = static_cast<int>(presetStore().presets().size());
    if (count == 0) {
        flash(tr(Str::MsgNoPresets));
        return;
    }
    const int now = presetStore().presetIndex();
    presets_.openPreset(now < 0 ? (direction > 0 ? 0 : count - 1) : (now + direction + count) % count);
}

void View::paintConfirm(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const std::string what = confirmWhat();
    // A panel's question (the preset browser's, the SEQ view's, the FX view's), which says whether to save the patch
    // first, and whether it has choices of its own over the others (where a split's middle band's effects go).
    const std::optional<Panel::ConfirmWords> words =
        confirmFrom_ ? std::optional(confirmFrom_->confirmWords(confirm_)) : std::nullopt;
    const bool bands = words && !words->choices.empty();
    const Rect panel = bands ? confirmBandsPanel(height()) : confirmPanel(height(), confirmShorter());
    paintDialogPanel(c, panel);
    const bool overwrite = confirm_ == static_cast<int>(PresetAction::Save);
    // What goes, undo brings back; or a category or folder, which nothing of the patch's needs.
    const bool clear = words && !words->saveFirst;
    c.text({panel.x + 24.0f, panel.y + 18.0f, panel.w - 48.0f, 22.0f},
           words       ? words->title
           : overwrite ? trf(Str::ConfirmOverwriteTitle, {presetStore().presetName()})
                       : std::string(tr(Str::ConfirmTitle)),
           kTextHeading, kText, Align::Left, true);
    float y = panel.y + 48.0f;
    if (!what.empty()) {
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, what, kTextLabel, kDim, Align::Left);
        y += kConfirmLine;
    }
    if (!overwrite && !clear) // nothing is thrown away by overwriting but the file, nor by clearing (undo has it)
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, tr(Str::ConfirmSaveFirst), kTextLabel, kDim, Align::Left);
    for (const Widget &w : confirmWidgets())
        paintWidget(c, w);
}

} // namespace fmvibe::gui
