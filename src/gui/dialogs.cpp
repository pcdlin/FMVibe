#include "view.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cstdio>
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
    for (int i = 0; i < static_cast<int>(std::size(kOversamplingFactors)); ++i)
        add(Kind::SettingsOption, settingsOption(panel, i), -1, kOversamplingFactors[i]);
    for (int i = 0; i < kThemeCount; ++i)
        add(Kind::SettingsTheme, settingsTheme(panel, i), -1, i);
    add(Kind::SettingsGlow, settingsGlow(panel));
    add(Kind::SettingsClose, settingsClose(panel));
    return list;
}

void View::paintSettings(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = settingsPanel(height());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 200.0f, 24.0f}, tr(Str::SettingsTitle), 14.0f, kText, Align::Left, true);
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 60.0f, 200.0f, 16.0f}, tr(Str::SettingsOversampling), 10.5f, kFaint, Align::Left,
           true);
    c.text({panel.x + 24.0f, panel.y + 122.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhat), 10.0f, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 140.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhy), 10.0f, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 158.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingSaved), 10.5f, kFaint, Align::Left);
    c.line({panel.x + 24.0f, panel.y + 184.5f}, {panel.right() - 24.0f, panel.y + 184.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 192.0f, 200.0f, 16.0f}, tr(Str::SettingsLook), 10.5f, kFaint, Align::Left, true);
    const Rect glow = settingsGlow(panel);
    c.text({panel.x + 24.0f, glow.y, 48.0f, glow.h}, tr(Str::SettingsGlow), 10.5f, kDim, Align::Left, true);
    const float amount = shownGlow();
    char text[16];
    std::snprintf(text, sizeof(text), "%.0f %%", amount * 100.0f);
    c.text({glow.right() + 14.0f, glow.y, panel.right() - 24.0f - glow.right() - 14.0f, glow.h},
           amount > 0.0f ? text : tr(Str::ValueOff), 10.5f, amount > 0.0f ? kText : kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 294.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsLookSaved), 10.5f, kFaint, Align::Left);
    // The version and the license, at the bottom.
    c.line({panel.x + 24.0f, panel.y + 320.5f}, {panel.right() - 24.0f, panel.y + 320.5f}, kBorder, 1.0f);
    const Rect about{panel.x + 24.0f, panel.y + 328.0f, panel.w - 48.0f, 16.0f};
    c.text(about, trf(Str::SettingsVersion, {FMVIBE_VERSION}), 10.5f, kFaint, Align::Left, true);
    c.text(about, tr(Str::SettingsLicense), 10.5f, kDim, Align::Right);
    for (const Widget &w : settingsWidgets())
        paintWidget(c, w);
}

void View::runPreset(PresetAction action) {
    switch (action) {
    case PresetAction::Init:
        controller_.resetPreset();
        break;
    case PresetAction::Load: // a preset from the library (which closes the browser), or a file from the dialog
        if (pendingPreset_ >= 0) {
            if (controller_.loadPresetAt(pendingPreset_))
                browserOpen_ = false;
            else
                flash(tr(Str::MsgPresetUnreadable));
        } else {
            controller_.choosePreset(false);
        }
        break;
    case PresetAction::Save:
        controller_.choosePreset(true);
        break;
    }
}

// What replaces the patch: Init, or the preset picked from the library. A file still to be picked in the dialog goes
// unsaid.
std::string View::confirmWhat() const {
    if (confirm_ == static_cast<int>(PresetAction::Init))
        return tr(Str::ConfirmInit);
    const std::vector<PresetEntry> &presets = controller_.presets();
    return pendingPreset_ >= 0 && pendingPreset_ < static_cast<int>(presets.size())
               ? trf(Str::ConfirmLoadPreset, {presets[pendingPreset_].name})
               : std::string();
}

std::vector<View::Widget> View::confirmWidgets() const {
    const Rect panel = confirmPanel(height(), confirmWhat().empty());
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
    add(Kind::ConfirmButton, confirmButton(panel, 0), 0);
    add(Kind::ConfirmButton, confirmButton(panel, 1), 1);
    return list;
}

namespace {
constexpr size_t kSearchLength = 40; // the most a search holds
// ASCII letters in lower case, for a search that doesn't mind case.
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char &ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}
} // namespace

std::vector<std::pair<int, std::string>> View::browserRows() const {
    std::vector<std::string> words;
    const std::string search = lowered(browserSearch_);
    for (size_t at = 0; at < search.size();) {
        const size_t end = std::min(search.find(' ', at), search.size());
        if (end > at)
            words.push_back(search.substr(at, end - at));
        at = end + 1;
    }
    std::vector<std::pair<int, std::string>> rows;
    const std::vector<PresetEntry> &presets = controller_.presets();
    const PresetEntry *last = nullptr; // the last one listed
    for (size_t i = 0; i < presets.size(); ++i) {
        const std::string text = lowered(presets[i].category + " " + presets[i].name);
        if (!std::all_of(words.begin(), words.end(),
                         [&](const std::string &word) { return text.find(word) != std::string::npos; }))
            continue;
        if (!presets[i].category.empty() && (!last || presets[i].category != last->category))
            rows.push_back({-1, presets[i].category});
        rows.push_back({static_cast<int>(i), presets[i].name});
        last = &presets[i];
    }
    return rows;
}

void View::revealBrowserRow(int preset, bool middle) {
    const std::vector<std::pair<int, std::string>> rows = browserRows();
    const auto at = std::find_if(rows.begin(), rows.end(), [&](const auto &row) { return row.first == preset; });
    if (preset < 0 || at == rows.end()) {
        browserScroll_ = 0;
        return;
    }
    const int row = static_cast<int>(at - rows.begin());
    const int shown = browserVisibleRows(browserPanel(height()));
    const int most = std::max(static_cast<int>(rows.size()) - shown, 0);
    const int first = std::clamp(browserScroll_, 0, most);
    if (middle)
        browserScroll_ = std::max(row - shown / 2, 0);
    else if (row < first)
        browserScroll_ = row > 0 && rows[row - 1].first < 0 ? row - 1 : row; // with its subfolder's header
    else if (row >= first + shown)
        browserScroll_ = row - shown + 1;
    browserScroll_ = std::clamp(browserScroll_, 0, most);
}

// After the search changes: the list from its top, the first preset found picked.
void View::searchChanged() {
    browserPick_ = -1;
    if (!browserSearch_.empty())
        for (const auto &row : browserRows())
            if (row.first >= 0) {
                browserPick_ = row.first;
                break;
            }
    browserScroll_ = 0;
}

std::pair<size_t, size_t> View::searchSelection() const {
    return std::minmax(searchCaret_, searchAnchor_);
}

void View::eraseSearch(size_t from, size_t to) {
    browserSearch_.erase(from, to - from);
    searchCaret_ = searchAnchor_ = from;
}

// Over the selection; line breaks and tabs as spaces, the rest past printable ASCII left out, as much as fits.
void View::insertSearch(std::string_view text) {
    std::string clean;
    for (char ch : text)
        if (ch == '\n' || ch == '\r' || ch == '\t')
            clean += ' ';
        else if (ch >= 0x20 && ch <= 0x7E)
            clean += ch;
    const auto [from, to] = searchSelection();
    if (clean.empty() && from == to)
        return;
    eraseSearch(from, to);
    clean.resize(std::min(clean.size(), kSearchLength - std::min(browserSearch_.size(), kSearchLength)));
    browserSearch_.insert(from, clean);
    searchCaret_ = searchAnchor_ = from + clean.size();
    caretTicks_ = 0;
    searchChanged();
}

void View::clearSearch() {
    browserSearch_.clear();
    searchCaret_ = searchAnchor_ = 0;
    searchShift_ = 0.0f;
    searchChanged();
}

// The place between characters nearest `x`; before the search was painted with its text, its end.
size_t View::searchStopAt(float x) const {
    if (searchStops_.size() != browserSearch_.size() + 1)
        return browserSearch_.size();
    size_t best = 0;
    for (size_t i = 1; i < searchStops_.size(); ++i)
        if (std::abs(searchStops_[i] - x) < std::abs(searchStops_[best] - x))
            best = i;
    return best;
}

void View::browserChar(char32_t c) {
    if (confirm_ < 0 && c >= 0x20 && c <= 0x7E)
        insertSearch(std::string(1, static_cast<char>(c)));
}

void View::browserKey(Key key, bool shift) {
    if (confirm_ >= 0)
        return;
    caretTicks_ = 0;
    const auto [from, to] = searchSelection();
    // The caret to `at`; with Shift the selection grows or shrinks with it, without it there is none.
    auto moveTo = [&](size_t at) {
        searchCaret_ = at;
        if (!shift)
            searchAnchor_ = at;
    };
    switch (key) {
    case Key::Enter:
        if (browserPick_ >= 0)
            openPreset(browserPick_);
        break;
    case Key::Escape:
        if (browserSearch_.empty()) {
            browserOpen_ = false;
            hover_.reset();
        } else {
            clearSearch();
            revealBrowserRow(controller_.presetIndex(), true);
        }
        break;
    case Key::Backspace: // the selection, or the character before the caret
    case Key::Delete:    // or after it
        if (from != to) {
            eraseSearch(from, to);
            searchChanged();
        } else if (key == Key::Backspace && searchCaret_ > 0) {
            eraseSearch(searchCaret_ - 1, searchCaret_);
            searchChanged();
        } else if (key == Key::Delete && searchCaret_ < browserSearch_.size()) {
            eraseSearch(searchCaret_, searchCaret_ + 1);
            searchChanged();
        }
        break;
    case Key::Left: // without Shift, from a selection to its start
        moveTo(from != to && !shift ? from : (searchCaret_ > 0 ? searchCaret_ - 1 : 0));
        break;
    case Key::Right:
        moveTo(from != to && !shift ? to : std::min(searchCaret_ + 1, browserSearch_.size()));
        break;
    case Key::Home:
        moveTo(0);
        break;
    case Key::End:
        moveTo(browserSearch_.size());
        break;
    case Key::SelectAll:
        searchAnchor_ = 0;
        searchCaret_ = browserSearch_.size();
        break;
    case Key::Up:
    case Key::Down: { // from the one picked, or the preset loaded, to the next one listed that way
        std::vector<int> listed;
        for (const auto &row : browserRows())
            if (row.first >= 0)
                listed.push_back(row.first);
        if (listed.empty())
            break;
        const int start = browserPick_ >= 0 ? browserPick_ : controller_.presetIndex();
        const auto at = std::find(listed.begin(), listed.end(), start);
        const int step = key == Key::Down ? 1 : -1;
        const int place = at == listed.end() ? (step > 0 ? 0 : static_cast<int>(listed.size()) - 1)
                                             : std::clamp(static_cast<int>(at - listed.begin()) + step, 0,
                                                          static_cast<int>(listed.size()) - 1);
        browserPick_ = listed[place];
        revealBrowserRow(browserPick_, false);
        break;
    }
    }
}

std::vector<View::Widget> View::browserWidgets() const {
    const Rect panel = browserPanel(height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::BrowserBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::BrowserBackdrop, panel, 1);
    const std::vector<std::pair<int, std::string>> rows = browserRows();
    const Rect area = browserList(panel);
    const int shown = browserVisibleRows(panel);
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row)
        if (rows[row].first >= 0)
            add(Kind::BrowserRow, {area.x, area.y + (row - first) * kBrowserRowH, area.w, kBrowserRowH - 2.0f},
                rows[row].first);
    add(Kind::BrowserClose, browserClose(panel));
    add(Kind::BrowserFolder, browserFolder(panel));
    add(Kind::BrowserSearch, browserSearch(panel));
    if (!browserSearch_.empty())
        add(Kind::BrowserClear, browserClear(panel));
    return list;
}

void View::paintBrowser(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = browserPanel(height());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 120.0f, 24.0f}, tr(Str::PresetsTitle), 14.0f, kText, Align::Left, true);
    const std::vector<std::pair<int, std::string>> rows = browserRows();
    const size_t count = controller_.presets().size();
    const Rect search = browserSearch(panel);
    std::string counted =
        count == 1 ? std::string(tr(Str::PresetsCountOne)) : trf(Str::PresetsCount, {std::to_string(count)});
    if (!browserSearch_.empty()) {
        const auto found = std::count_if(rows.begin(), rows.end(), [](const auto &row) { return row.first >= 0; });
        counted = trf(Str::PresetsMatching, {std::to_string(found), std::to_string(count)});
    }
    c.text({panel.x + 104.0f, panel.y + 16.0f, search.x - 8.0f - panel.x - 104.0f, 20.0f}, counted, 10.5f, kFaint,
           Align::Left);
    // The search field: a magnifier, then what is typed, its selection behind it and the caret in it, or how to
    // search. Text too long for it scrolls, so the caret stays in sight; where each place between its characters
    // lands is kept for the mouse.
    c.fillRect(search, kBackground, 5.0f);
    c.strokeRect(search, browserSearch_.empty() ? kBorder : kAccent, 1.0f, 5.0f);
    const Point lens{search.x + 13.0f, search.center().y - 1.0f};
    c.strokeCircle(lens, 4.0f, kDim, 1.3f);
    c.line({lens.x + 3.0f, lens.y + 3.0f}, {lens.x + 6.5f, lens.y + 6.5f}, kDim, 1.5f);
    const Rect typed = browserTyped(panel);
    constexpr float kSearchSize = 11.0f;
    std::vector<float> stops;
    for (size_t i = 0; i <= browserSearch_.size(); ++i)
        stops.push_back(c.textWidth(std::string_view(browserSearch_).substr(0, i), kSearchSize));
    searchCaret_ = std::min(searchCaret_, browserSearch_.size());
    searchAnchor_ = std::min(searchAnchor_, browserSearch_.size());
    const float fits = typed.w - 2.0f, caretAt = stops[searchCaret_];
    if (caretAt - searchShift_ > fits)
        searchShift_ = caretAt - fits;
    searchShift_ = std::clamp(std::min(searchShift_, caretAt), 0.0f, std::max(stops.back() - fits, 0.0f));
    for (float &stop : stops)
        stop += typed.x - searchShift_;
    searchStops_ = stops;
    const auto [from, to] = searchSelection();
    if (browserSearch_.empty()) {
        c.text(typed, tr(Str::PresetsSearchHint), 10.5f, kFaint, Align::Left);
    } else {
        c.pushClip(typed);
        if (from != to)
            c.fillRect({stops[from], search.y + 5.0f, stops[to] - stops[from], search.h - 10.0f}, kAccent.alpha(0.35f),
                       2.0f);
        c.text({stops[0], typed.y, stops.back() - stops[0] + 20.0f, typed.h}, browserSearch_, kSearchSize, kText,
               Align::Left);
        c.popClip();
    }
    if (confirm_ < 0 && from == to && caretTicks_ / 32 % 2 == 0) {
        const float caret = std::min(stops[searchCaret_] + 0.5f, typed.right());
        c.line({caret, search.y + 5.0f}, {caret, search.bottom() - 5.0f}, kText, 1.0f);
    }
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);

    const Rect area = browserList(panel);
    if (rows.empty() && !browserSearch_.empty()) {
        c.text({area.x, area.center().y - 10.0f, area.w, 18.0f}, trf(Str::PresetsNoMatch, {browserSearch_}), 12.0f,
               kDim);
    } else if (rows.empty()) {
        c.text({area.x, area.center().y - 20.0f, area.w, 18.0f}, tr(Str::PresetsNone), 12.0f, kDim);
        c.text({area.x, area.center().y, area.w, 18.0f}, tr(Str::PresetsNoneHint), 10.5f, kFaint);
    }
    // Subfolders' headers; the presets' rows are widgets.
    const int shown = browserVisibleRows(panel);
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row)
        if (rows[row].first < 0)
            c.text({area.x + 4.0f, area.y + (row - first) * kBrowserRowH + 4.0f, area.w - 8.0f, kBrowserRowH - 4.0f},
                   rows[row].second, 10.5f, kFaint, Align::Left, true);
    if (static_cast<int>(rows.size()) > shown) { // where the list is
        const Rect track{area.right() + 4.0f, area.y, 3.0f, area.h};
        const float thumb = track.h * shown / rows.size();
        c.fillRect(track, kBorder, 1.5f);
        c.fillRect({track.x, track.y + (track.h - thumb) * first / (rows.size() - shown), track.w, thumb}, kFaint, 1.5f);
    }

    // The folder, cut from the left to fit beside its button.
    std::string folder;
    for (wchar_t ch : controller_.presetFolder())
        folder += ch < 0x80 ? static_cast<char>(ch) : '?';
    const float room = browserFolder(panel).x - panel.x - 36.0f;
    std::string shownFolder = folder;
    while (shownFolder.size() > 4 && c.textWidth(shownFolder, 10.5f) > room)
        shownFolder = "\xE2\x80\xA6" + folder.substr(folder.size() - (shownFolder.size() - 4));
    c.text({panel.x + 24.0f, panel.bottom() - 40.0f, room, 26.0f}, shownFolder, 10.5f, kFaint, Align::Left);
    for (const Widget &w : browserWidgets())
        paintWidget(c, w);
}

void View::openPreset(int index) {
    if (controller_.patchEdited()) {
        confirm_ = static_cast<int>(PresetAction::Load);
        pendingPreset_ = index;
        hover_.reset();
    } else if (controller_.loadPresetAt(index)) {
        browserOpen_ = false; // loaded from the browser, it closes; cancelled or unreadable, it stays for another pick
        hover_.reset();
    } else {
        flash(tr(Str::MsgPresetUnreadable));
    }
}

// Steps through the library, round from the last to the first; from Init, to the first or the last.
void View::stepPreset(int direction) {
    controller_.rescanPresets();
    const int count = static_cast<int>(controller_.presets().size());
    if (count == 0) {
        flash(tr(Str::MsgNoPresets));
        return;
    }
    const int now = controller_.presetIndex();
    openPreset(now < 0 ? (direction > 0 ? 0 : count - 1) : (now + direction + count) % count);
}

void View::paintConfirm(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const std::string what = confirmWhat();
    const Rect panel = confirmPanel(height(), what.empty());
    c.fillRect(panel, kPanel, 10.0f);
    c.strokeRect(panel, kBorder, 1.0f, 10.0f);
    c.text({panel.x + 24.0f, panel.y + 18.0f, panel.w - 48.0f, 22.0f}, tr(Str::ConfirmTitle), 14.0f, kText,
           Align::Left, true);
    float y = panel.y + 48.0f;
    if (!what.empty()) {
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, what, 10.5f, kDim, Align::Left);
        y += kConfirmLine;
    }
    c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, tr(Str::ConfirmSaveFirst), 10.5f, kDim, Align::Left);
    for (const Widget &w : confirmWidgets())
        paintWidget(c, w);
}

} // namespace fmvibe::gui
