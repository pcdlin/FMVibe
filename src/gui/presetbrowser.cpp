// The preset bar's info and its card, the preset browser and the Save sheet: what the editor shows of the preset
// library and of a preset's info (see PresetInfo), as a panel of the editor (see presetbrowser.h).
#include "presetbrowser.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace fmvibe::gui {

using namespace detail;

namespace {

// The browser: a popup under the preset bar, its caret pointing up at it. A search row, then the sidebar (the
// library, the folder tree and the categories) beside the list (the folder's parts and the filters over the rows),
// then a strip with the preset picked.
constexpr float kBrowserW = 800.0f;
constexpr float kBrowserTop = kTitleBarH + 6.0f;
constexpr float kBrowserHeadH = 52.0f;
constexpr float kSideW = 210.0f;
constexpr float kCrumbH = 36.0f;
constexpr float kStripH = 62.0f;
constexpr float kRowH = 28.0f;
constexpr float kSideRowH = 26.0f;
constexpr float kCatRowH = 24.0f;

Rect browserPanelAt(float viewHeight) {
    return {std::round(kPresetName.center().x - kBrowserW * 0.5f), kBrowserTop, kBrowserW,
            std::min(600.0f, viewHeight - kBrowserTop - 12.0f)};
}
Rect subfoldersButton(Rect p) {
    return {p.right() - 12.0f - 112.0f, p.y + 12.0f, 112.0f, 28.0f};
}
Rect searchBox(Rect p) {
    const Rect r = subfoldersButton(p);
    return {p.x + 12.0f, p.y + 12.0f, r.x - 10.0f - (p.x + 12.0f), 28.0f};
}
Rect sidebar(Rect p) {
    return {p.x, p.y + kBrowserHeadH, kSideW, p.h - kBrowserHeadH - kStripH};
}
Rect crumbRow(Rect p) {
    return {p.x + kSideW, p.y + kBrowserHeadH, p.w - kSideW, kCrumbH};
}
Rect listAreaOf(Rect p) {
    return {p.x + kSideW + 8.0f, p.y + kBrowserHeadH + kCrumbH + 2.0f, p.w - kSideW - 24.0f,
            p.h - kBrowserHeadH - kCrumbH - kStripH - 4.0f};
}
Rect stripOf(Rect p) {
    return {p.x, p.bottom() - kStripH, p.w, kStripH};
}
Rect closeButton(Rect p) {
    const Rect s = stripOf(p);
    return {s.right() - 14.0f - 86.0f, s.y + 17.0f, 86.0f, 28.0f};
}
Rect saveHereButton(Rect p) {
    const Rect r = closeButton(p);
    return {r.x - 8.0f - 96.0f, r.y, 96.0f, 28.0f};
}
Rect showFileButton(Rect p, bool saveHere) { // before Save Here while that shows
    const Rect r = saveHere ? saveHereButton(p) : closeButton(p);
    return {r.x - 8.0f - 32.0f, r.y, 32.0f, 28.0f};
}
// A row's star, before the author's column at its right.
Rect starOf(Rect row) {
    return {row.right() - 84.0f - 6.0f - 18.0f, row.y + (row.h - 18.0f) * 0.5f, 18.0f, 18.0f};
}

// The info card, under the preset bar.
Rect infoCardPanel() {
    constexpr float w = 440.0f;
    return {std::round(kPresetName.center().x - w * 0.5f), kTitleBarH + 6.0f, w, 152.0f};
}
Rect infoStar(Rect card) {
    return {card.right() - 16.0f - 28.0f, card.y + 14.0f, 28.0f, 28.0f};
}
Rect infoPrimary(Rect card) {
    return {card.right() - 16.0f - 100.0f, card.y + 112.0f, 100.0f, 26.0f};
}
Rect infoBrowse(Rect card) {
    const Rect r = infoPrimary(card);
    return {r.x - 8.0f - 120.0f, r.y, 120.0f, 26.0f};
}
// The (i) in the preset bar, left of its chevron.
Rect infoButtonRect() {
    return {kPresetName.right() - 48.0f, kPresetName.center().y - 10.0f, 20.0f, 20.0f};
}

// The categories' colors: the built-in ones', in their order, then those a category of the user's own can take.
Color categoryColor(int color) {
    static constexpr Color kColors[] = {
        Color::hex(0xF0884A), Color::hex(0xF06A6A), Color::hex(0x3FC7B0), Color::hex(0xC6D94A), Color::hex(0xFF7AA8),
        Color::hex(0x5FB3FF), Color::hex(0xB48CFF), Color::hex(0xF2C14E), Color::hex(0xD08A5C), Color::hex(0x8FA3B8),
        Color::hex(0x7FB59A), Color::hex(0xC79AD6), Color::hex(0xD6C27A)};
    static_assert(std::size(kColors) == kBuiltInCategories + kCustomCategoryColors);
    return color >= 0 && color < static_cast<int>(std::size(kColors)) ? kColors[color] : Color::hex(0x9A9DB8);
}
// What is written on a category's color: dark, whatever the theme.
constexpr Color kPillInk = Color::hex(0x14121A);

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char &ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}
std::string upperAscii(std::string_view text) {
    std::string out(text);
    for (char &ch : out)
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<char>(ch - 'a' + 'A');
    return out;
}

// `text` cut to fit `width`, an ellipsis where it was cut (between whole UTF-8 characters).
std::string fitted(Canvas &c, const std::string &text, float size, bool bold, float width) {
    if (width <= 0.0f)
        return {};
    if (c.textWidth(text, size, bold) <= width)
        return text;
    const std::string ellipsis = "\xE2\x80\xA6";
    std::string cut = text;
    while (!cut.empty()) {
        do
            cut.pop_back();
        while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xC0) == 0x80);
        while (!cut.empty() && static_cast<unsigned char>(cut.back()) >= 0xC0) // a lead byte left alone
            cut.pop_back();
        if (c.textWidth(cut + ellipsis, size, bold) <= width)
            return cut + ellipsis;
    }
    return {};
}

// A folder's last part, or `top` for the preset folder itself.
std::string folderName(const std::string &folder, std::string_view top) {
    return folder.empty() ? std::string(top) : folder.substr(folder.rfind('/') + 1);
}

// A star: filled while it is a favorite, else outlined.
void paintStar(Canvas &c, Point m, float r, Color color, bool filled) {
    Point points[11];
    for (int i = 0; i < 10; ++i) {
        const float radius = i % 2 == 0 ? r : r * 0.45f;
        const float angle = -kPi * 0.5f + i * kPi / 5.0f;
        points[i] = {m.x + radius * std::cos(angle), m.y + radius * std::sin(angle)};
    }
    points[10] = points[0];
    if (filled)
        c.fillPolygon(points, 10, color);
    c.polyline(points, 11, color, 1.2f);
}

// A folder's outline (filled while it is shown), or the factory's sawtooth roof.
void paintFolderIcon(Canvas &c, Point at, Color color, bool filled, bool factory) {
    if (factory) {
        const Point roof[] = {{at.x, at.y + 11.0f}, {at.x, at.y + 4.5f},  {at.x + 3.5f, at.y + 6.5f},
                              {at.x + 3.5f, at.y + 4.5f}, {at.x + 7.0f, at.y + 6.5f}, {at.x + 7.0f, at.y + 2.0f},
                              {at.x + 11.0f, at.y + 2.0f}, {at.x + 11.0f, at.y + 11.0f}, {at.x, at.y + 11.0f}};
        if (filled)
            c.fillPolygon(roof, 8, color.alpha(0.35f));
        c.polyline(roof, std::size(roof), color, 1.2f);
        return;
    }
    const Point outline[] = {{at.x, at.y + 2.0f},        {at.x + 4.0f, at.y + 2.0f},  {at.x + 5.2f, at.y + 3.5f},
                             {at.x + 11.5f, at.y + 3.5f}, {at.x + 11.5f, at.y + 11.0f}, {at.x, at.y + 11.0f},
                             {at.x, at.y + 2.0f}};
    if (filled)
        c.fillPolygon(outline, 6, color.alpha(0.35f));
    c.polyline(outline, std::size(outline), color, 1.2f);
}

// A button's face, as the dialogs' are: the way ahead in the accent, the others plain. Gives the ink for its label.
// `raised` for a button on a ground near the editor's background (the browser's footer), where a dark flat theme's
// wells, the background's color, would melt into it: there they are lifted off it instead.
Color buttonFace(Canvas &c, Rect r, bool primary, bool hovered, bool enabled = true, bool raised = false) {
    if (primary && enabled) {
        if (look.flat) {
            c.fillRect(r, hovered ? mix(kAccent, kText, 0.15f) : kAccent, kRadiusControl);
            return litInk(kText);
        }
        c.fillRect(r, kAccent.alpha(hovered ? 0.35f : 0.22f), kRadiusControl);
        c.strokeRect(r, kAccent, 1.0f, kRadiusControl);
        return kText;
    }
    if (look.flat && raised && !lightTheme())
        c.fillRect(r, hovered && enabled ? kTrack : kRaised, kRadiusControl);
    else if (look.flat)
        c.fillRect(r, hovered && enabled ? wellHover() : wellColor(), kRadiusControl);
    else {
        c.fillRect(r, hovered && enabled ? kTrack : kRaised, kRadiusControl);
        c.strokeRect(r, kBorder, 1.0f, kRadiusControl);
    }
    return enabled ? (hovered ? kText : kDim) : kFaint;
}

// An x, to close.
void paintCross(Canvas &c, Point m, float r, Color color) {
    c.line({m.x - r, m.y - r}, {m.x + r, m.y + r}, color, 1.5f);
    c.line({m.x - r, m.y + r}, {m.x + r, m.y - r}, color, 1.5f);
}

// A caret on a popup's top edge, pointing up at what opened it.
void paintCaret(Canvas &c, Rect panel, float x) {
    const Point tip[] = {{x - 7.0f, panel.y + 0.5f}, {x, panel.y - 6.5f}, {x + 7.0f, panel.y + 0.5f}};
    c.fillPolygon(tip, 3, popupFill());
    if (!look.flat)
        c.polyline(tip, 3, kBorder, 1.0f);
}

// The preset `presets[index]` names, copied (a scan of the library moves the others in the list), if there is one.
std::optional<PresetEntry> entryAt(const std::vector<PresetEntry> &presets, int index) {
    if (index < 0 || index >= static_cast<int>(presets.size()))
        return std::nullopt;
    return presets[static_cast<size_t>(index)];
}

// Where `entry` is in `presets` (by its folder and name), or -1: a pick found again once the library has been scanned.
int indexOfEntry(const std::vector<PresetEntry> &presets, const std::optional<PresetEntry> &entry) {
    if (!entry)
        return -1;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].factory == entry->factory && presets[i].folder == entry->folder && presets[i].name == entry->name)
            return static_cast<int>(i);
    return -1;
}

} // namespace

PresetBrowser::PresetBrowser(PanelHost &host) : host_(host) {}

// --- What it shows over the editor: the info card, the browser, the Save sheet and the name form over it, each
// over the one before; the topmost takes every click.

bool PresetBrowser::modal() const {
    return nameMenu_ != 0 || saveMode_ != SaveMode::None || browserOpen_ || infoCardOpen_;
}

void PresetBrowser::widgets(std::vector<Widget> &list) const {
    const std::vector<Widget> own = nameMenu_ != 0               ? nameMenuWidgets()
                                    : saveMode_ != SaveMode::None ? saveWidgets()
                                    : browserOpen_                ? browserWidgets()
                                    : infoCardOpen_               ? infoCardWidgets()
                                                                  : std::vector<Widget>();
    list.insert(list.end(), own.begin(), own.end());
}

void PresetBrowser::paint(Canvas &c) {
    if (infoCardOpen_)
        paintInfoCard(c);
    if (browserOpen_)
        paintBrowser(c);
    if (saveMode_ != SaveMode::None)
        paintSave(c);
    if (nameMenu_ != 0)
        paintNameMenu(c);
}

// --- The preset bar and its info card

int PresetBrowser::presetCategoryColor(const std::string &category) const {
    const int built = builtInCategory(category);
    if (built >= 0)
        return built;
    for (const PresetCategory &own : presetStore().userCategories())
        if (sameName(own.name, category))
            return own.color;
    return -1;
}

float PresetBrowser::paintCategoryPill(Canvas &c, Point at, float height, const std::string &category, float textSize) {
    if (category.empty())
        return 0.0f;
    const int built = builtInCategory(category);
    const int color = presetCategoryColor(category);
    const std::string label = built >= 0 ? std::string(tr(kPresetCategories[built].pill)) : upperAscii(category);
    const float w = std::ceil(c.textWidth(label, textSize, true)) + 10.0f;
    const Rect pill{at.x, at.y, w, height};
    c.fillRect(pill, categoryColor(color), kRadiusMark);
    c.text(pill, label, textSize, kPillInk, Align::Center, true);
    return w;
}

void PresetBrowser::paintPresetBar(Canvas &c, const Widget &w, bool hovered) {
    // One line: the category at the left (a dot in its color and its name), the preset's name in the middle (with a dot while it has changed
    // since it was loaded), "by" its author against the info button, and a chevron for the browser at the right. A
    // name too long for the middle moves left, as far as the category, and the author gives up its room to it.
    const bool lit = hovered || browserOpen_;
    c.fillRect(w.rect, lit ? kTrack : kRaised, kRadiusControl);
    if (outlined())
        c.strokeRect(w.rect, browserOpen_ ? kAccent : kBorder, 1.0f, kRadiusControl);
    paintChevron(c, {w.rect.right() - 14.0f, w.rect.center().y}, lit ? kText : kDim,
                 browserOpen_ ? ChevronWay::Up : ChevronWay::Down);
    const bool hasFile = controller().presetHasFile();
    const PresetInfo &info = controller().layout().presetInfo;
    const float y = w.rect.center().y;
    float left = w.rect.x + 10.0f; // where the name may start
    if (hasFile && !info.category.empty()) { // a dot in its color, then its name in the dim text's
        const int built = builtInCategory(info.category);
        const std::string label = built >= 0 ? std::string(tr(kPresetCategories[built].label)) : info.category;
        c.fillCircle({w.rect.x + 13.0f, y}, 3.5f, categoryColor(presetCategoryColor(info.category)));
        const std::string cut = fitted(c, label, kTextLabel, false, 96.0f);
        const float labelW = std::ceil(c.textWidth(cut, kTextLabel));
        // A pixel higher than the name: the label's lower-case letters then sit on the bar's middle, as the dot does.
        c.text({w.rect.x + 21.0f, y - 9.0f, labelW + 2.0f, 16.0f}, cut, kTextLabel, kDim, Align::Left);
        left = w.rect.x + 21.0f + labelW + 12.0f;
    }
    const float right = infoButtonRect().x - 8.0f; // where the author ends
    const std::string by = hasFile && !info.author.empty() ? trf(Str::PresetsBy, {info.author}) : std::string();
    const float byW = by.empty() ? 0.0f : std::ceil(c.textWidth(by, kTextLabel));
    const float byKept = std::min(byW, 64.0f); // as much of the author as a long name leaves at least
    const float dot = controller().patchEdited() ? 10.0f : 0.0f;
    const float nameRoom = right - left - (by.empty() ? 0.0f : byKept + 12.0f) - dot;
    const std::string name = fitted(c, presetStore().presetName(), kTextBody, true, nameRoom);
    const float nameW = std::ceil(c.textWidth(name, kTextBody, true));
    // Centered, unless it would run into the author's least room: then as far left as it must be.
    const float nameEnd = right - (by.empty() ? 0.0f : byKept + 12.0f);
    float x = std::max(w.rect.center().x - (nameW + dot) / 2.0f, left);
    if (x + nameW + dot > nameEnd)
        x = std::max(left, nameEnd - nameW - dot);
    c.text({x - 1.0f, y - 8.0f, nameW + 2.0f, 16.0f}, name, kTextBody, kText, Align::Left, true);
    if (dot > 0.0f)
        c.fillCircle({x + nameW + 7.0f, y}, 2.5f, kAccent);
    const float byLeft = x + nameW + dot + 12.0f;
    if (!by.empty() && right - byLeft > 12.0f) {
        const std::string cut = fitted(c, by, kTextLabel, false, right - byLeft);
        c.text({byLeft, y - 9.0f, right - byLeft, 16.0f}, cut, kTextLabel, kDim, Align::Right); // as the category
    }
}
std::vector<Widget> PresetBrowser::infoCardWidgets() const {
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    const Rect card = infoCardPanel();
    add(Kind::InfoCard, {0.0f, 0.0f, View::kWidth, host_.height()}, 0);
    add(Kind::InfoCard, card, 1);
    // The preset bar stays live under it: its name and its info button close the card as they would open it.
    add(Kind::PresetNameBox, kPresetName, -1);
    add(Kind::PresetInfoButton, infoButtonRect(), -1);
    if (presetStore().presetIndex() >= 0) {
        add(Kind::InfoAction, infoStar(card), 0);
        add(Kind::InfoAction, infoBrowse(card), 1);
    }
    if (presetStore().writesPresetFiles() && (controller().presetFactory() || presetStore().presetIndex() >= 0))
        add(Kind::InfoAction, infoPrimary(card), 2);
    return list;
}

void PresetBrowser::paintInfoCard(Canvas &c) {
    const Rect card = infoCardPanel();
    paintMenuPanel(c, card, kRadiusCard);
    paintCaret(c, card, infoButtonRect().center().x);
    const PresetInfo &info = controller().layout().presetInfo;
    const bool factory = controller().presetFactory();
    const float left = card.x + 16.0f;
    float x = left;
    const float pill = paintCategoryPill(c, {x, card.y + 18.0f}, 16.0f, info.category, kTextSmall);
    if (pill > 0.0f)
        x += pill + 8.0f;
    const std::string name = fitted(c, presetStore().presetName(), kTextHeading, true,
                                    infoStar(card).x - 12.0f - x);
    c.text({x, card.y + 14.0f, infoStar(card).x - x, 24.0f}, name, kTextHeading, kText, Align::Left, true);
    // by its author, where it is (Factory or its folder) and the tempo it was saved at
    std::string meta = info.author.empty() ? std::string() : trf(Str::PresetsBy, {info.author});
    auto append = [&](const std::string &part) {
        if (!part.empty())
            meta += (meta.empty() ? "" : "  \xC2\xB7  ") + part;
    };
    const int index = presetStore().presetIndex();
    const std::vector<PresetEntry> &presets = presetStore().presets();
    if (factory)
        append(tr(Str::PresetsFactory));
    else if (index >= 0 && index < static_cast<int>(presets.size()))
        append(folderName(presets[index].folder, tr(Str::PresetsUser)));
    if (controller().layout().savedTempo > 0.0f) {
        char bpm[16];
        std::snprintf(bpm, sizeof(bpm), "%g", std::round(controller().layout().savedTempo * 10.0f) / 10.0f);
        append(trf(Str::PresetsFactoryTempo, {bpm}));
    }
    c.text({left, card.y + 40.0f, card.w - 32.0f, 16.0f}, fitted(c, meta, kTextLabel, false, card.w - 32.0f),
           kTextLabel, kDim, Align::Left);
    // The description, one line.
    const bool described = !info.description.empty();
    c.text({left, card.y + 68.0f, card.w - 32.0f, 20.0f},
           described ? fitted(c, info.description, kTextBody, false, card.w - 32.0f)
                     : std::string(tr(Str::PresetsNoDescription)),
           kTextBody, described ? kText : kFaint, Align::Left);
    c.line({card.x + 1.0f, card.y + 100.5f}, {card.right() - 1.0f, card.y + 100.5f}, kBorder, 1.0f);
    if (factory)
        c.text({left, infoPrimary(card).y, 120.0f, 26.0f}, tr(Str::PresetsReadOnly), kTextLabel, kFaint, Align::Left);
    for (const Widget &w : infoCardWidgets())
        if (w.kind == Kind::InfoAction)
            host_.paintWidget(c, w);
}

// --- The preset browser

Rect PresetBrowser::browserListArea() const {
    return listAreaOf(browserPanelAt(host_.height()));
}

int PresetBrowser::browserVisible() const {
    // A row is kRowH - 2 tall, the 2 a gap under it, which the last one shown needn't fit.
    return std::max(static_cast<int>((browserListArea().h + 2.0f) / kRowH), 1);
}

void PresetBrowser::openBrowser(int preset) {
    presetStore().rescanPresets();
    browserOpen_ = true;
    browserSearching_ = false; // its search takes the keyboard once clicked
    infoCardOpen_ = false;
    browserCategories_.clear();
    host_.clearField(Field::Browser);
    const std::vector<PresetEntry> &presets = presetStore().presets();
    if (preset >= 0 && preset < static_cast<int>(presets.size())) { // a preset to show: in its folder
        const PresetEntry &entry = presets[preset];
        browserScope_ = entry.factory ? BrowserScope::Factory : BrowserScope::Folder;
        browserFolder_ = entry.folder;
        rememberBrowserPlace();
    } else { // where it was left, as the user's settings keep it (Factory to begin with)
        const std::string place = presetStore().presetBrowserPlace();
        if (place == "all")
            browserScope_ = BrowserScope::All;
        else if (place == "favorites")
            browserScope_ = BrowserScope::Favorites;
        else if (place == "recent")
            browserScope_ = BrowserScope::Recent;
        else if (place == "factory")
            browserScope_ = BrowserScope::Factory;
        else if (place.starts_with("folder:")) {
            browserScope_ = BrowserScope::Folder;
            browserFolder_ = place.substr(7);
        }
        if (browserScope_ == BrowserScope::Folder && !browserFolder_.empty()) { // a folder since taken away
            const std::vector<std::string> folders = presetStore().presetFolders();
            if (std::find(folders.begin(), folders.end(), browserFolder_) == folders.end())
                browserScope_ = BrowserScope::Factory;
        }
    }
    if (browserScope_ == BrowserScope::Folder) { // the folder shown, opened down to it in the tree
        browserOpenFolders_.insert("");
        for (size_t at = browserFolder_.find('/'); at != std::string::npos; at = browserFolder_.find('/', at + 1))
            browserOpenFolders_.insert(browserFolder_.substr(0, at));
    }
    const int index = preset >= 0 ? preset : presetStore().presetIndex();
    browserPick_ = index;
    browserSideScroll_ = 0.0f;
    revealBrowserRow(index, true);
    host_.resetHover();
}

void PresetBrowser::rememberBrowserPlace() {
    switch (browserScope_) {
    case BrowserScope::All:
        presetStore().setPresetBrowserPlace("all");
        break;
    case BrowserScope::Favorites:
        presetStore().setPresetBrowserPlace("favorites");
        break;
    case BrowserScope::Recent:
        presetStore().setPresetBrowserPlace("recent");
        break;
    case BrowserScope::Factory:
        presetStore().setPresetBrowserPlace("factory");
        break;
    case BrowserScope::Folder:
        presetStore().setPresetBrowserPlace("folder:" + browserFolder_);
        break;
    }
}

bool PresetBrowser::categoryShown(const PresetEntry &entry) const {
    return browserCategories_.empty() || browserCategories_.count(lowerAscii(entry.info.category)) > 0;
}

std::vector<int> PresetBrowser::browserRows(bool byCategory) const {
    const std::vector<std::string> words = browserSearch_.words();
    const std::vector<PresetEntry> &presets = presetStore().presets();
    auto matches = [&](int i) {
        const PresetEntry &e = presets[i];
        if (byCategory && !categoryShown(e))
            return false;
        const int built = builtInCategory(e.info.category);
        const std::string category = built >= 0 ? std::string(tr(kPresetCategories[built].label)) : e.info.category;
        return TextField::holds(e.name + " " +
                                    (e.factory ? std::string(tr(Str::PresetsFactory)) : e.folder) + " " +
                                    e.info.author + " " + category + " " + e.info.description,
                                words);
    };
    std::vector<int> rows;
    if (browserScope_ == BrowserScope::Recent) {
        for (int i : presetStore().recentPresets())
            if (i >= 0 && i < static_cast<int>(presets.size()) && matches(i))
                rows.push_back(i);
        return rows;
    }
    const std::string under = browserFolder_.empty() ? std::string() : browserFolder_ + "/";
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const PresetEntry &e = presets[i];
        bool in = true;
        switch (browserScope_) {
        case BrowserScope::Favorites:
            in = presetStore().presetFavorite(i);
            break;
        case BrowserScope::Factory:
            in = e.factory;
            break;
        case BrowserScope::Folder:
            in = !e.factory && (e.folder == browserFolder_ ||
                                (browserSubfolders_ && (browserFolder_.empty() || e.folder.starts_with(under))));
            break;
        default:
            break;
        }
        if (in && matches(i))
            rows.push_back(i);
    }
    return rows;
}

void PresetBrowser::revealBrowserRow(int preset, bool middle) {
    const std::vector<int> rows = browserRows();
    const auto at = std::find(rows.begin(), rows.end(), preset);
    if (preset < 0 || at == rows.end()) {
        browserScroll_ = 0;
        return;
    }
    const int row = static_cast<int>(at - rows.begin());
    const int shown = browserVisible();
    const int most = std::max(static_cast<int>(rows.size()) - shown, 0);
    const int first = std::clamp(browserScroll_, 0, most);
    if (middle)
        browserScroll_ = std::max(row - shown / 2, 0);
    else if (row < first)
        browserScroll_ = row;
    else if (row >= first + shown)
        browserScroll_ = row - shown + 1;
    browserScroll_ = std::clamp(browserScroll_, 0, most);
}

void PresetBrowser::browserKey(Key key) {
    switch (key) {
    case Key::Enter:
        if (browserPick_ >= 0)
            openPreset(browserPick_);
        break;
    case Key::Escape:
        if (browserSearch_.text.empty()) {
            browserOpen_ = false;
            host_.resetHover();
        } else {
            host_.clearField(Field::Browser);
            browserPick_ = presetStore().presetIndex();
            revealBrowserRow(browserPick_, true);
        }
        break;
    case Key::Up:
    case Key::Down: { // from the one picked, or the preset loaded, to the next one listed that way
        const std::vector<int> listed = browserRows();
        if (listed.empty())
            break;
        const int start = browserPick_ >= 0 ? browserPick_ : presetStore().presetIndex();
        const auto at = std::find(listed.begin(), listed.end(), start);
        const int step = key == Key::Down ? 1 : -1;
        const int place = at == listed.end() ? (step > 0 ? 0 : static_cast<int>(listed.size()) - 1)
                                             : std::clamp(static_cast<int>(at - listed.begin()) + step, 0,
                                                          static_cast<int>(listed.size()) - 1);
        browserPick_ = listed[place];
        revealBrowserRow(browserPick_, false);
        break;
    }
    default:
        break;
    }
}

// Loading asks first while the patch has changed. Loaded from the browser, it closes it, unless `close` is false (a
// click on a preset, which leaves it open for the next); cancelled or unreadable, it stays for another pick.
void PresetBrowser::openPreset(int index, bool close) {
    if (index == presetStore().presetIndex() && !controller().patchEdited()) { // loaded already, as it is
        if (close)
            browserOpen_ = false;
        host_.resetHover();
    } else if (controller().patchEdited()) {
        pendingPreset_ = index;
        pendingClose_ = close;
        host_.askConfirm(*this, kConfirmLoad);
    } else if (presetStore().loadPresetAt(index)) {
        if (close)
            browserOpen_ = false;
        host_.resetHover();
    } else {
        host_.flash(tr(Str::MsgPresetUnreadable));
    }
}

std::vector<PresetBrowser::TreeRow> PresetBrowser::browserTree() const {
    std::vector<TreeRow> rows;
    rows.push_back({BrowserScope::Factory, {}, 0, false, false});
    const std::vector<std::string> folders = presetStore().presetFolders();
    auto hasKids = [&](const std::string &folder) {
        const std::string under = folder.empty() ? std::string() : folder + "/";
        return std::any_of(folders.begin(), folders.end(), [&](const std::string &f) { return f.starts_with(under); });
    };
    auto open = [&](const std::string &folder) { return browserOpenFolders_.count(folder) > 0; };
    rows.push_back({BrowserScope::Folder, {}, 0, hasKids(""), open("")});
    for (const std::string &folder : folders) {
        // Shown while every folder it is in is open.
        bool shown = open("");
        for (size_t at = folder.find('/'); shown && at != std::string::npos; at = folder.find('/', at + 1))
            shown = open(folder.substr(0, at));
        if (!shown)
            continue;
        const int depth = 1 + static_cast<int>(std::count(folder.begin(), folder.end(), '/'));
        rows.push_back({BrowserScope::Folder, folder, depth, hasKids(folder), open(folder)});
    }
    return rows;
}

std::vector<PresetBrowser::CategoryRow> PresetBrowser::saveCategoryList() const {
    std::vector<CategoryRow> rows;
    for (int i = 0; i < kBuiltInCategories; ++i)
        rows.push_back({kPresetCategories[i].name, tr(kPresetCategories[i].label), i, false});
    bool first = true;
    for (const PresetCategory &own : presetStore().userCategories()) {
        if (own.name.empty() || builtInCategory(own.name) >= 0 ||
            std::any_of(rows.begin(), rows.end(), [&](const CategoryRow &r) {
                return lowerAscii(r.name) == lowerAscii(own.name);
            }))
            continue;
        rows.push_back({own.name, own.name, own.color, first});
        first = false;
    }
    return rows;
}

// What the list would show of each of browserCategoryList()'s categories: the presets of the place picked in the
// sidebar that the search finds, whatever the categories filter by.
std::vector<int> PresetBrowser::browserCategoryCounts() const {
    const std::vector<CategoryRow> categories = browserCategoryList();
    const std::vector<PresetEntry> &presets = presetStore().presets();
    std::vector<int> counts(categories.size(), 0);
    for (int i : browserRows(false)) {
        const std::string key = lowerAscii(presets[i].info.category);
        for (size_t k = 0; k < categories.size(); ++k)
            if (lowerAscii(categories[k].name) == key) {
                ++counts[k];
                break;
            }
    }
    return counts;
}

int PresetBrowser::presetsInFolder(const std::string &folder) const { // subfolders' too
    const std::vector<PresetEntry> &presets = presetStore().presets();
    return static_cast<int>(std::count_if(presets.begin(), presets.end(), [&](const PresetEntry &e) {
        return !e.factory && (e.folder == folder || e.folder.starts_with(folder + "/"));
    }));
}

int PresetBrowser::presetsInCategory(const std::string &name) const {
    const std::string key = lowerAscii(name);
    const std::vector<PresetEntry> &presets = presetStore().presets();
    return static_cast<int>(std::count_if(presets.begin(), presets.end(),
                                          [&](const PresetEntry &e) { return lowerAscii(e.info.category) == key; }));
}

// The user's own categories, which they can remove: neither built in nor only found on presets, nor None.
bool PresetBrowser::ownCategory(const CategoryRow &row) {
    return !row.name.empty() && builtInCategory(row.name) < 0 && row.color >= 0;
}

std::vector<PresetBrowser::CategoryRow> PresetBrowser::browserCategoryList() const {
    std::vector<CategoryRow> rows = saveCategoryList();
    // Then the categories presets have that are neither (from another computer, say), in grey; then None.
    bool first = rows.size() == static_cast<size_t>(kBuiltInCategories);
    std::vector<std::string> found;
    for (const PresetEntry &e : presetStore().presets())
        if (!e.info.category.empty() &&
            std::none_of(rows.begin(), rows.end(),
                         [&](const CategoryRow &r) { return lowerAscii(r.name) == lowerAscii(e.info.category); }) &&
            std::none_of(found.begin(), found.end(),
                         [&](const std::string &f) { return lowerAscii(f) == lowerAscii(e.info.category); }))
            found.push_back(e.info.category);
    std::sort(found.begin(), found.end(), [](const std::string &a, const std::string &b) { return lowerAscii(a) < lowerAscii(b); });
    for (const std::string &name : found) {
        rows.push_back({name, name, -1, first});
        first = false;
    }
    rows.push_back({{}, tr(Str::PresetsNoneCategory), -1, true});
    return rows;
}

std::vector<PresetBrowser::SideItem> PresetBrowser::browserSide() const {
    const Rect side = sidebar(browserPanelAt(host_.height()));
    std::vector<SideItem> items;
    float y = side.y + 8.0f - browserSideScroll_;
    const Rect row{side.x + 6.0f, 0.0f, side.w - 12.0f, kSideRowH};
    for (int scope = 0; scope < 3; ++scope) {
        items.push_back({Kind::BrowserLibrary, scope, {row.x, y, row.w, kSideRowH}});
        y += kSideRowH + 1.0f;
    }
    // FOLDERS, its + making a new one.
    y += 10.0f;
    items.push_back({Kind::BrowserBackdrop, 10, {row.x, y, row.w, 20.0f}});
    items.push_back({Kind::BrowserNew, 0, {row.right() - 22.0f, y, 20.0f, 20.0f}});
    y += 22.0f;
    const std::vector<TreeRow> tree = browserTree();
    for (int i = 0; i < static_cast<int>(tree.size()); ++i) {
        items.push_back({Kind::BrowserTree, i, {row.x, y, row.w, kSideRowH}});
        if (tree[i].hasKids)
            items.push_back({Kind::BrowserTreeToggle, i, {row.x + 2.0f + tree[i].depth * 12.0f, y + 5.0f, 16.0f, 16.0f}});
        y += kSideRowH;
    }
    // CATEGORIES, and Clear while one filters (a new category is made in the Save sheet).
    y += 12.0f;
    items.push_back({Kind::BrowserBackdrop, 11, {row.x, y, row.w, 20.0f}});
    if (!browserCategories_.empty())
        items.push_back({Kind::BrowserCatClear, -1, {row.right() - 48.0f, y, 44.0f, 20.0f}});
    y += 22.0f;
    const std::vector<CategoryRow> categories = browserCategoryList();
    for (int i = 0; i < static_cast<int>(categories.size()); ++i) {
        if (categories[i].divider) {
            items.push_back({Kind::BrowserBackdrop, 12, {row.x + 8.0f, y + 4.0f, row.w - 16.0f, 1.0f}});
            y += 9.0f;
        }
        items.push_back({Kind::BrowserCategory, i, {row.x, y, row.w, kCatRowH}});
        if (ownCategory(categories[i])) // the user's own: an x at its right end, shown under the mouse, removes it
            items.push_back({Kind::BrowserCategoryRemove, i, {row.right() - 26.0f, y + 3.0f, 18.0f, 18.0f}});
        y += kCatRowH;
    }
    items.push_back({Kind::BrowserBackdrop, 13, {row.x, y, row.w, 8.0f}}); // its end
    return items;
}

Rect PresetBrowser::browserSideArea() const {
    return sidebar(browserPanelAt(host_.height()));
}

Rect PresetBrowser::browserSideTrack() const {
    const Rect side = sidebar(browserPanelAt(host_.height()));
    return {side.right() - 7.0f, side.y + 4.0f, 4.0f, side.h - 8.0f};
}

Rect PresetBrowser::browserSideThumb() const {
    const Rect side = sidebar(browserPanelAt(host_.height())), track = browserSideTrack();
    const float end = browserSideEnd();
    const float h = std::max(track.h * side.h / (side.h + end), 24.0f);
    return {track.x, track.y + (end > 0.0f ? (track.h - h) * browserSideScroll_ / end : 0.0f), track.w, h};
}

float PresetBrowser::browserSideEnd() const {
    const Rect side = sidebar(browserPanelAt(host_.height()));
    const std::vector<SideItem> items = browserSide();
    const float content = items.back().rect.bottom() + browserSideScroll_ - side.y + 8.0f; // room after the last row
    return std::max(content - side.h, 0.0f);
}

std::vector<Widget> PresetBrowser::browserWidgets() const {
    const Rect panel = browserPanelAt(host_.height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1, Rect clip = {}) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        w.clip = clip;
        list.push_back(w);
    };
    add(Kind::BrowserBackdrop, {0.0f, 0.0f, View::kWidth, host_.height()}, 0);
    add(Kind::BrowserBackdrop, panel, 1);
    // The preset bar stays live over it: its name closes the browser, as it opened it.
    add(Kind::PresetNameBox, kPresetName);
    add(Kind::SearchField, searchBox(panel), static_cast<int>(Field::Browser));
    if (!browserSearch_.text.empty())
        add(Kind::SearchClear, fieldClear(searchBox(panel)), static_cast<int>(Field::Browser));
    add(Kind::BrowserSubfolders, subfoldersButton(panel));
    const Rect side = sidebar(panel);
    for (const SideItem &item : browserSide())
        if (item.kind != Kind::BrowserBackdrop && item.rect.bottom() > side.y && item.rect.y < side.bottom())
            add(item.kind, item.rect, item.a, side);
    if (browserSideEnd() > 0.0f) // over the rows' right ends
        add(Kind::BrowserSideScroll, browserSideTrack().inset(-3.0f));
    for (const auto &[part, r] : browserCrumbRects_)
        add(Kind::BrowserCrumb, r, part);
    for (const auto &[category, r] : browserChipRects_)
        add(Kind::BrowserChip, r, category);
    const std::vector<int> rows = browserRows();
    const Rect area = listAreaOf(panel);
    const int shown = browserVisible();
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row) {
        const Rect r{area.x, area.y + (row - first) * kRowH, area.w, kRowH - 2.0f};
        add(Kind::BrowserRow, r, rows[row]);
        add(Kind::BrowserFav, starOf(r), rows[row]);
    }
    // Always there, greyed out while nothing is picked (and the file's, for a factory preset, which has none).
    add(Kind::BrowserClose, closeButton(panel));
    if (saveHereShown())
        add(Kind::BrowserSaveHere, saveHereButton(panel));
    if (presetStore().writesPresetFiles())
        add(Kind::BrowserShowFile, showFileButton(panel, saveHereShown()));
    return list;
}

void PresetBrowser::paintBrowser(Canvas &c) {
    const Rect panel = browserPanelAt(host_.height());
    paintMenuPanel(c, panel, kRadiusPanel);
    paintCaret(c, panel, kPresetName.center().x);
    const std::vector<PresetEntry> &presets = presetStore().presets();
    const std::vector<int> rows = browserRows();
    // The search, how many it lists of all of them at its right end.
    const Rect search = searchBox(panel);
    host_.paintField(c, Field::Browser, search, fieldTyped(search), tr(Str::PresetsSearchHint),
               trf(Str::PresetsShown, {std::to_string(rows.size()), std::to_string(presets.size())}));
    c.line({panel.x + 1.0f, panel.y + kBrowserHeadH - 0.5f}, {panel.right() - 1.0f, panel.y + kBrowserHeadH - 0.5f},
           kBorder, 1.0f);

    // The sidebar, a shade apart.
    const Rect side = sidebar(panel);
    c.fillRect(side, kText.alpha(lightTheme() ? 0.025f : 0.02f));
    c.line({side.right() - 0.5f, side.y}, {side.right() - 0.5f, side.bottom()}, kBorder, 1.0f);
    c.pushClip(side);
    const std::vector<TreeRow> tree = browserTree();
    const std::vector<CategoryRow> categories = browserCategoryList();
    const std::vector<std::string> folders = presetStore().presetFolders();
    std::vector<int> recent = presetStore().recentPresets();
    int favorites = 0;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i)
        favorites += presetStore().presetFavorite(i) ? 1 : 0;
    const std::vector<int> categoryCounts = browserCategoryCounts();
    // A preset counts in a folder, and with Subfolders on in the folders it is in.
    auto countIn = [&](const TreeRow &t) {
        if (t.scope == BrowserScope::Factory)
            return static_cast<int>(std::count_if(presets.begin(), presets.end(), [](const PresetEntry &e) { return e.factory; }));
        const std::string under = t.folder.empty() ? std::string() : t.folder + "/";
        return static_cast<int>(std::count_if(presets.begin(), presets.end(), [&](const PresetEntry &e) {
            return !e.factory && (e.folder == t.folder || (browserSubfolders_ && (t.folder.empty() || e.folder.starts_with(under))));
        }));
    };
    auto rowFace = [&](Rect r, bool on, bool hovered) {
        if (on)
            c.fillRect(r, kAccent.alpha(look.flat ? 0.16f : 0.2f), kRadiusControl);
        else if (hovered)
            c.fillRect(r, kText.alpha(0.06f), kRadiusControl);
    };
    auto count = [&](Rect r, int n) {
        c.text({r.right() - 40.0f, r.y, 32.0f, r.h}, std::to_string(n), kTextSmall, kFaint, Align::Right);
    };
    for (const SideItem &item : browserSide()) {
        const Rect r = item.rect;
        if (r.bottom() < side.y || r.y > side.bottom())
            continue;
        Widget probe;
        probe.kind = item.kind;
        probe.rect = r;
        probe.a = item.a;
        const bool hovered = host_.isHovered(probe);
        switch (item.kind) {
        case Kind::BrowserLibrary: {
            const auto scope = static_cast<BrowserScope>(item.a);
            const bool on = browserScope_ == scope;
            rowFace(r, on, hovered);
            const Point m{r.x + 15.0f, r.center().y};
            const Color icon = scope == BrowserScope::Favorites ? Color::hex(0xF2C14E) : (on ? kText : kDim);
            if (scope == BrowserScope::All) {
                c.strokeRect({m.x - 5.5f, m.y - 5.0f, 11.0f, 10.0f}, icon, 1.2f, kRadiusMark);
                c.line({m.x - 3.0f, m.y - 1.5f}, {m.x + 3.0f, m.y - 1.5f}, icon, 1.2f);
                c.line({m.x - 3.0f, m.y + 1.5f}, {m.x + 3.0f, m.y + 1.5f}, icon, 1.2f);
            } else if (scope == BrowserScope::Favorites) {
                paintStar(c, m, 6.0f, icon, true);
            } else {
                c.strokeCircle(m, 5.5f, icon, 1.2f);
                c.line(m, {m.x, m.y - 3.0f}, icon, 1.2f);
                c.line(m, {m.x + 2.5f, m.y + 1.5f}, icon, 1.2f);
            }
            const Str label = scope == BrowserScope::All ? Str::PresetsAll
                              : scope == BrowserScope::Favorites ? Str::PresetsFavorites
                                                                 : Str::PresetsRecent;
            c.text({r.x + 30.0f, r.y, r.w - 70.0f, r.h}, tr(label), kTextBody, on || hovered ? kText : kDim,
                   Align::Left, on);
            count(r, scope == BrowserScope::All ? static_cast<int>(presets.size())
                     : scope == BrowserScope::Favorites ? favorites
                                                        : static_cast<int>(recent.size()));
            break;
        }
        case Kind::BrowserBackdrop:
            if (item.a == 10 || item.a == 11)
                c.text({r.x + 8.0f, r.y, 120.0f, r.h}, tr(item.a == 10 ? Str::PresetsFolders : Str::PresetsCategories),
                       kTextSmall, kFaint, Align::Left, true);
            else if (item.a == 12)
                c.line({r.x, r.y + 0.5f}, {r.right(), r.y + 0.5f}, kBorder, 1.0f);
            break;
        case Kind::BrowserNew: {
            if (hovered)
                c.fillRect(r, kTrack, kRadiusMark);
            const Point m = r.center();
            const Color ink = hovered ? kText : kDim;
            c.line({m.x - 4.5f, m.y}, {m.x + 4.5f, m.y}, ink, 1.4f);
            c.line({m.x, m.y - 4.5f}, {m.x, m.y + 4.5f}, ink, 1.4f);
            break;
        }
        case Kind::BrowserCatClear:
            c.text(r, tr(Str::PresetsClear), kTextLabel, hovered ? kText : kAccent, Align::Right);
            break;
        case Kind::BrowserTree: {
            const TreeRow &t = tree[item.a];
            const bool on = browserScope_ == t.scope && (t.scope == BrowserScope::Factory || browserFolder_ == t.folder);
            rowFace(r, on, hovered);
            const float x = r.x + 20.0f + t.depth * 12.0f;
            paintFolderIcon(c, {x, r.y + 6.5f}, on ? kAccent : kDim, on, t.scope == BrowserScope::Factory);
            const std::string name = t.scope == BrowserScope::Factory ? std::string(tr(Str::PresetsFactory))
                                                                       : folderName(t.folder, tr(Str::PresetsUser));
            c.text({x + 19.0f, r.y, r.right() - 40.0f - x - 19.0f, r.h},
                   fitted(c, name, kTextBody, on, r.right() - 44.0f - x - 19.0f), kTextBody,
                   on || hovered ? kText : kDim, Align::Left, on);
            count(r, countIn(t));
            break;
        }
        case Kind::BrowserTreeToggle: {
            const Point m = r.center();
            const Color ink = hovered ? kText : kDim;
            const bool open = tree[item.a].open;
            paintChevron(c, m, ink, open ? ChevronWay::Down : ChevronWay::Right, true);
            break;
        }
        case Kind::BrowserCategory: {
            const CategoryRow &cat = categories[item.a];
            const bool on = browserCategories_.count(lowerAscii(cat.name)) > 0;
            Widget remove;
            remove.kind = Kind::BrowserCategoryRemove;
            remove.a = item.a;
            const bool removing = ownCategory(cat) && host_.isHovered(remove);
            const bool rowHovered = hovered || removing;
            rowFace(r, on, rowHovered);
            const Rect swatch{r.x + 11.0f, r.center().y - 4.0f, 8.0f, 8.0f};
            if (cat.name.empty())
                c.strokeRect(swatch, kFaint, 1.0f, 2.0f, true);
            else
                c.fillRect(swatch, categoryColor(cat.color), kRadiusMark);
            c.text({r.x + 30.0f, r.y, r.w - 70.0f, r.h}, fitted(c, cat.label, kTextBody, false, r.w - 74.0f),
                   kTextBody, on || rowHovered ? kText : kDim, Align::Left);
            if (!(ownCategory(cat) && rowHovered)) // its x in the count's place
                count(r, categoryCounts[item.a]);
            break;
        }
        case Kind::BrowserCategoryRemove: {
            Widget row;
            row.kind = Kind::BrowserCategory;
            row.a = item.a;
            for (const SideItem &other : browserSide())
                if (other.kind == Kind::BrowserCategory && other.a == item.a)
                    row.rect = other.rect;
            if (!hovered && !host_.isHovered(row))
                break;
            if (hovered)
                c.fillCircle(r.center(), 8.0f, kText.alpha(0.1f));
            const Point m = r.center();
            const Color ink = hovered ? kText : kDim;
            c.line({m.x - 3.0f, m.y - 3.0f}, {m.x + 3.0f, m.y + 3.0f}, ink, 1.3f);
            c.line({m.x - 3.0f, m.y + 3.0f}, {m.x + 3.0f, m.y - 3.0f}, ink, 1.3f);
            break;
        }
        default:
            break;
        }
    }
    c.popClip();
    if (browserSideEnd() > 0.0f) { // its scrollbar, lit under the mouse and while dragged
        Widget probe;
        probe.kind = Kind::BrowserSideScroll;
        probe.rect = browserSideTrack().inset(-3.0f);
        const bool lit = host_.isHovered(probe) || host_.isActive(probe);
        c.fillRect(browserSideTrack(), kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(browserSideThumb(), lit ? kDim : kFaint, kRadiusMark);
    }

    // The folder's parts, each a click away, then the category filters, each a click off.
    const Rect crumbs = crumbRow(panel);
    browserCrumbRects_.clear();
    browserChipRects_.clear();
    float x = crumbs.x + 14.0f;
    std::vector<std::string> parts;
    if (browserScope_ == BrowserScope::Folder) {
        parts.push_back(tr(Str::PresetsUser));
        for (size_t at = 0; !browserFolder_.empty() && at <= browserFolder_.size();) {
            const size_t end = std::min(browserFolder_.find('/', at), browserFolder_.size());
            parts.push_back(browserFolder_.substr(at, end - at));
            at = end + 1;
        }
    } else {
        parts.push_back(tr(browserScope_ == BrowserScope::All         ? Str::PresetsAll
                           : browserScope_ == BrowserScope::Favorites ? Str::PresetsFavorites
                           : browserScope_ == BrowserScope::Recent    ? Str::PresetsRecent
                                                                      : Str::PresetsFactory));
    }
    // The filters' chips from the right.
    float chipsLeft = crumbs.right() - 12.0f;
    std::vector<std::pair<int, Rect>> chips;
    for (int i = static_cast<int>(categories.size()) - 1; i >= 0; --i) {
        if (browserCategories_.count(lowerAscii(categories[i].name)) == 0)
            continue;
        const float w = std::ceil(c.textWidth(categories[i].label, kTextLabel, true)) + 26.0f;
        if (chipsLeft - w < crumbs.x + 200.0f)
            break;
        chipsLeft -= w;
        chips.push_back({i, {chipsLeft, crumbs.center().y - 10.0f, w, 20.0f}});
        chipsLeft -= 6.0f;
    }
    for (const auto &[i, r] : chips) {
        Widget probe;
        probe.kind = Kind::BrowserChip;
        probe.rect = r;
        probe.a = i;
        const bool hovered = host_.isHovered(probe);
        const Color fill = categories[i].name.empty() ? kFaint : categoryColor(categories[i].color);
        c.fillRect(r, hovered ? mix(fill, kText, 0.2f) : fill, kRadiusMark);
        c.text({r.x + 8.0f, r.y, r.w - 22.0f, r.h}, categories[i].label, kTextLabel, kPillInk, Align::Left, true);
        paintCross(c, {r.right() - 9.0f, r.center().y}, 2.5f, kPillInk);
        browserChipRects_.push_back({i, r});
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        if (i > 0) {
            c.text({x, crumbs.y, 12.0f, crumbs.h}, "\xE2\x80\xBA", kTextBody, kFaint, Align::Center);
            x += 14.0f;
        }
        const std::string part = fitted(c, parts[i], kTextBody, last, chipsLeft - 8.0f - x);
        const float w = std::ceil(c.textWidth(part, kTextBody, last));
        const Rect r{x - 2.0f, crumbs.center().y - 11.0f, w + 4.0f, 22.0f};
        Widget probe;
        probe.kind = Kind::BrowserCrumb;
        probe.rect = r;
        probe.a = static_cast<int>(i);
        const bool hovered = browserScope_ == BrowserScope::Folder && !last && host_.isHovered(probe);
        c.text({x, crumbs.y, w + 2.0f, crumbs.h}, part, kTextBody, last || hovered ? kText : kDim, Align::Left, last);
        if (browserScope_ == BrowserScope::Folder && !last)
            browserCrumbRects_.push_back({static_cast<int>(i), r});
        x += w + 6.0f;
    }

    // The rows: a star, the name (its folder before it where it isn't the folder shown), its category and its author.
    const Rect area = listAreaOf(panel);
    const int shown = browserVisible();
    const int first = std::clamp(browserScroll_, 0, std::max(static_cast<int>(rows.size()) - shown, 0));
    if (rows.empty()) {
        const bool filtered = !browserSearch_.text.empty() || !browserCategories_.empty();
        c.text({area.x, area.y + 40.0f, area.w, 20.0f},
               tr(filtered ? Str::PresetsNoMatchShort : Str::PresetsNoneHere), kTextValue, kDim);
        if (!filtered && browserScope_ == BrowserScope::Folder)
            c.text({area.x, area.y + 62.0f, area.w, 18.0f}, tr(Str::PresetsNoneHint), kTextLabel, kFaint);
    }
    const int loaded = presetStore().presetIndex();
    // The category pills in a column of their own before the star, each against it, as wide as the widest of those
    // listed (at most 96 px, a longer one cut off), so the descriptions end in line before them.
    float pillsW = 0.0f;
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row)
        if (const std::string &category = presets[rows[row]].info.category; !category.empty()) {
            const int built = builtInCategory(category);
            const std::string label = built >= 0 ? std::string(tr(kPresetCategories[built].pill)) : upperAscii(category);
            pillsW = std::max(pillsW, std::ceil(c.textWidth(label, kTextSmall, true)) + 10.0f);
        }
    pillsW = std::min(pillsW, 96.0f);
    for (int row = first; row < std::min(first + shown, static_cast<int>(rows.size())); ++row) {
        const int i = rows[row];
        const PresetEntry &e = presets[i];
        const Rect r{area.x, area.y + (row - first) * kRowH, area.w, kRowH - 2.0f};
        Widget probe;
        probe.kind = Kind::BrowserRow;
        probe.rect = r;
        probe.a = i;
        Widget star = probe;
        star.kind = Kind::BrowserFav;
        star.rect = starOf(r);
        const bool picked = browserPick_ == i, hovered = host_.isHovered(probe) || host_.isHovered(star);
        if (picked) {
            c.fillRect(r, kAccent.alpha(look.flat ? 0.16f : 0.2f), kRadiusControl);
            if (outlined())
                c.strokeRect(r, kAccent.alpha(0.6f), 1.0f, kRadiusControl);
        } else if (hovered) {
            c.fillRect(r, kText.alpha(0.06f), kRadiusControl);
        }
        const bool favorite = presetStore().presetFavorite(i);
        paintStar(c, star.rect.center(), 5.5f,
                  favorite ? Color::hex(0xF2C14E) : (host_.isHovered(star) ? kText : kFaint.alpha(hovered ? 1.0f : 0.6f)),
                  favorite);
        const float pillsRight = star.rect.x - 4.0f;
        if (pillsW > 0.0f && !e.info.category.empty()) {
            const int built = builtInCategory(e.info.category);
            const std::string label =
                built >= 0 ? std::string(tr(kPresetCategories[built].pill)) : upperAscii(e.info.category);
            const float w = std::min(std::ceil(c.textWidth(label, kTextSmall, true)) + 10.0f, pillsW);
            c.pushClip({pillsRight - pillsW, r.y, pillsW, r.h});
            paintCategoryPill(c, {pillsRight - w, r.center().y - 8.0f}, 16.0f, e.info.category, kTextSmall);
            c.popClip();
        }
        // The name and the description, short of the pills.
        const float nameRight = pillsRight - (pillsW > 0.0f ? pillsW + 12.0f : 8.0f);
        float nx = r.x + 10.0f;
        const bool elsewhere = browserScope_ != BrowserScope::Folder || e.folder != browserFolder_;
        if (elsewhere && browserScope_ != BrowserScope::Factory) {
            const std::string where = (e.factory ? std::string(tr(Str::PresetsFactory))
                                                 : folderName(e.folder, tr(Str::PresetsUser))) +
                                      " \xE2\x80\xBA";
            const std::string cut = fitted(c, where, kTextLabel, false, (nameRight - nx) * 0.45f);
            const float w = std::ceil(c.textWidth(cut, kTextLabel));
            c.text({nx, r.y + 1.0f, w + 2.0f, r.h}, cut, kTextLabel, kFaint, Align::Left);
            nx += w + 6.0f;
        }
        const std::string name = fitted(c, e.name, kTextBody, picked, nameRight - nx - 14.0f);
        const float nameW = std::ceil(c.textWidth(name, kTextBody, picked));
        // At rest well toward the text's color, standing out from the grey author and the faint description, and in
        // it under the mouse and picked.
        c.text({nx, r.y, nameW + 2.0f, r.h}, name, kTextBody, picked || hovered ? kText : mix(kDim, kText, 0.65f),
               Align::Left, picked);
        if (i == loaded)
            c.fillCircle({nx + nameW + 8.0f, r.center().y}, 2.5f, kAccent);
        // Its description, faint, in the room the name leaves.
        const float dx = nx + nameW + 18.0f;
        if (!e.info.description.empty() && nameRight - dx > 24.0f)
            c.text({dx, r.y + 1.0f, nameRight - dx, r.h},
                   fitted(c, e.info.description, kTextLabel, false, nameRight - dx), kTextLabel, kFaint, Align::Left);
        c.text({r.right() - 84.0f, r.y, 78.0f, r.h},
               fitted(c, e.info.author.empty() ? std::string("\xE2\x80\x94") : e.info.author, kTextLabel, false, 78.0f),
               kTextLabel, kDim, Align::Left);
    }
    if (static_cast<int>(rows.size()) > shown) { // where the list is
        const Rect track{area.right() + 6.0f, area.y, 3.0f, area.h};
        const float thumb = track.h * shown / rows.size();
        c.fillRect(track, kBorder.alpha(0.5f), kRadiusMark);
        c.fillRect({track.x, track.y + (track.h - thumb) * first / (rows.size() - shown), track.w, thumb}, kFaint,
                   kRadiusMark);
    }

    // The strip: the preset picked, its category, name, author and folder over its description.
    const Rect strip = stripOf(panel);
    // Set apart from the lists over it as a footer, half way to the editor's background (where a flat theme's buttons
    // are set in wells of the background): its lower corners the panel's, inside its outline.
    const Rect foot{panel.x + 1.0f, strip.y, panel.w - 2.0f, strip.h - 1.0f};
    const Color footInk = mix(kPanel, kBackground, 0.5f);
    c.fillRect(foot, footInk, kRadiusPanel - 1.0f);
    c.fillRect({foot.x, foot.y, foot.w, kRadiusPanel}, footInk);
    c.line({panel.x + 1.0f, strip.y + 0.5f}, {panel.right() - 1.0f, strip.y + 0.5f}, kBorder, 1.0f);
    if (browserPick_ >= 0 && browserPick_ < static_cast<int>(presets.size())) {
        const PresetEntry &e = presets[browserPick_];
        const float right =
            (presetStore().writesPresetFiles() ? showFileButton(panel, saveHereShown()).x : closeButton(panel).x) - 14.0f;
        float sx = strip.x + 16.0f;
        const float pill = paintCategoryPill(c, {sx, strip.y + 13.0f}, 16.0f, e.info.category, kTextSmall);
        if (pill > 0.0f)
            sx += pill + 8.0f;
        const std::string name = fitted(c, e.name, kTextValue, true, right - sx);
        const float nameW = std::ceil(c.textWidth(name, kTextValue, true));
        c.text({sx, strip.y + 10.0f, nameW + 2.0f, 22.0f}, name, kTextValue, kText, Align::Left, true);
        sx += nameW + 10.0f;
        std::string meta = e.info.author.empty() ? std::string() : trf(Str::PresetsBy, {e.info.author});
        const std::string where = e.factory ? std::string(tr(Str::PresetsFactory))
                                            : (e.folder.empty() ? std::string(tr(Str::PresetsUser))
                                                                : std::string(tr(Str::PresetsUser)) + " / " + e.folder);
        meta += (meta.empty() ? "" : "  \xC2\xB7  ") + where;
        c.text({sx, strip.y + 11.0f, std::max(right - sx, 0.0f), 22.0f}, fitted(c, meta, kTextLabel, false, right - sx),
               kTextLabel, kFaint, Align::Left);
        const bool described = !e.info.description.empty();
        c.text({strip.x + 16.0f, strip.y + 33.0f, right - strip.x - 16.0f, 18.0f},
               described ? fitted(c, e.info.description, kTextLabel, false, right - strip.x - 16.0f)
                         : std::string(tr(Str::PresetsNoDescription)),
               kTextLabel, described ? kDim : kFaint, Align::Left);
    }
    for (const Widget &w : browserWidgets())
        if (w.kind == Kind::SearchClear || w.kind == Kind::BrowserSubfolders || w.kind == Kind::BrowserClose ||
            w.kind == Kind::BrowserShowFile || w.kind == Kind::BrowserSaveHere)
            host_.paintWidget(c, w);
}

// --- The Save sheet, which also edits a preset's info and moves it

namespace {
// One column: the name and author, the categories, the description and the folder it goes in, a list button whose
// list of folders opens under it (or over it, where the editor ends).
constexpr float kSaveW = 540.0f;
constexpr float kChipW = 90.0f, kChipH = 28.0f, kChipGap = 7.0f;
constexpr float kFolderRowH = 26.0f;
constexpr float kFolderListW = 300.0f;
constexpr int kFolderRowsShown = 8; // the list's rows at most, Factory's among them; more scroll

std::string trimmed(const std::string &text) {
    const size_t from = text.find_first_not_of(' ');
    return from == std::string::npos ? std::string() : text.substr(from, text.find_last_not_of(' ') - from + 1);
}

// Whether `name` can name a file or a folder: Windows refuses these characters in one, and a name ending in a dot or
// a space.
bool fileNameOk(const std::string &name) {
    return !name.empty() && name.back() != '.' && name.find_first_of("\\/:*?\"<>|") == std::string::npos;
}

// A folder as the sheet names it: `top` (the preset folder), then its parts, " / " between them.
std::string folderPath(const std::string &folder, std::string_view top) {
    std::string path(top);
    for (size_t at = 0; !folder.empty() && at <= folder.size();) {
        const size_t end = std::min(folder.find('/', at), folder.size());
        path += " / " + folder.substr(at, end - at);
        at = end + 1;
    }
    return path;
}

// A padlock about `m`: its body filled, its shackle two legs over it joined by a half circle.
void paintPadlock(Canvas &c, Point m, Color color) {
    c.fillRect({m.x - 4.5f, m.y - 0.5f, 9.0f, 6.5f}, color, kRadiusMark);
    Point shackle[13];
    shackle[0] = {m.x - 2.75f, m.y};
    for (int k = 0; k <= 10; ++k) // the half circle over the legs, left to right
        shackle[1 + k] = Canvas::onCircle({m.x, m.y - 2.75f}, 2.75f, kPi * (k / 10.0f - 0.5f));
    shackle[12] = {m.x + 2.75f, m.y};
    c.polyline(shackle, 13, color, 1.3f);
}
} // namespace

std::vector<std::string> PresetBrowser::saveFolders() const {
    std::vector<std::string> folders{""};
    for (const std::string &f : presetStore().presetFolders())
        folders.push_back(f);
    return folders;
}

bool PresetBrowser::saveReplaces() const {
    const std::string name = trimmed(saveName_.text);
    if (saveMode_ == SaveMode::Save)
        return presetStore().presetExists(saveFolder_, name);
    const std::vector<PresetEntry> &presets = presetStore().presets();
    return saveMode_ == SaveMode::Info && saveIndex_ >= 0 && saveIndex_ < static_cast<int>(presets.size()) &&
           saveFolder_ != presets[saveIndex_].folder && presetStore().presetExists(saveFolder_, name);
}

PresetBrowser::SaveLayout PresetBrowser::saveLayout() const {
    SaveLayout l;
    const float left = 24.0f, inner = kSaveW - 2.0f * left;
    // Where each part sits from the panel's top, then the panel as tall as they take.
    const float body = 52.0f + 18.0f;
    const int chips = static_cast<int>(saveCategoryList().size()) + 2; // with None and + New
    const int perRow = static_cast<int>((inner + kChipGap) / (kChipW + kChipGap));
    const int chipRows = (chips + perRow - 1) / perRow;
    const float chipsTop = body + 80.0f;
    const float chipsEnd = chipsTop + chipRows * (kChipH + 8.0f) - 8.0f;
    const float descTop = chipsEnd + 32.0f;
    const float folderTop = descTop + 30.0f + 32.0f;
    const float bodyEnd = folderTop + 30.0f;
    const bool exists = saveReplaces();
    const float footer = bodyEnd + (exists ? 58.0f : 20.0f);
    const float h = footer + 30.0f + 20.0f;
    // It drops from the preset bar, as the info card does.
    const float x = std::clamp(std::round(kPresetName.center().x - kSaveW * 0.5f), 12.0f, View::kWidth - 12.0f - kSaveW);
    l.panel = {x, kTitleBarH + 6.0f, kSaveW, h};
    const Rect p = l.panel;
    l.name = {p.x + left, p.y + body + 18.0f, inner - 12.0f - 170.0f, 30.0f};
    l.author = {l.name.right() + 12.0f, l.name.y, 170.0f, 30.0f};
    for (int i = 0; i < chips; ++i)
        l.chips.push_back({p.x + left + (i % perRow) * (kChipW + kChipGap), p.y + chipsTop + (i / perRow) * (kChipH + 8.0f),
                           kChipW, kChipH});
    l.description = {p.x + left, p.y + descTop, inner, 30.0f};
    l.folder = {p.x + left, p.y + folderTop, inner, 30.0f};
    l.warning = exists ? Rect{p.x + 24.0f, p.y + bodyEnd + 16.0f, p.w - 48.0f, 28.0f} : Rect{};
    l.cancel = {p.right() - 24.0f - 96.0f - 8.0f - 86.0f, p.y + footer, 86.0f, 30.0f};
    l.save = {p.right() - 24.0f - 96.0f, p.y + footer, 96.0f, 30.0f};
    l.toFile = {p.x + 24.0f, p.y + footer, 120.0f, 30.0f};
    l.close = {p.right() - 38.0f, p.y + 14.0f, 24.0f, 24.0f};
    if (saveFolderList_) { // the rows (Factory's and the folders'), a line, then New Folder
        const int rows = std::min(static_cast<int>(saveFolders().size()) + 1, kFolderRowsShown);
        const float rowsH = rows * kFolderRowH, listH = 4.0f + rowsH + 9.0f + kFolderRowH + 4.0f;
        float y = l.folder.bottom() + 4.0f;
        if (y + listH > host_.height() - 8.0f)
            y = l.folder.y - 4.0f - listH;
        l.list = {l.folder.x, y, kFolderListW, listH};
        l.listRows = {l.list.x + 4.0f, l.list.y + 4.0f, l.list.w - 8.0f, rowsH};
        l.listNew = {l.list.x + 4.0f, l.listRows.bottom() + 9.0f, l.list.w - 8.0f, kFolderRowH - 1.0f};
    }
    return l;
}

void PresetBrowser::openSave(int index) {
    // Saving the patch: the folders as they are now, which nothing may have scanned yet (Init, the browser not opened).
    // Editing a preset's info keeps the list its index is into.
    if (index < 0)
        presetStore().rescanPresets();
    const std::vector<PresetEntry> &presets = presetStore().presets();
    PresetInfo info;
    if (index >= 0 && index < static_cast<int>(presets.size())) { // editing a preset's info, where it is
        const PresetEntry &e = presets[index];
        saveMode_ = SaveMode::Info;
        saveIndex_ = index;
        saveName_.text = e.name;
        info = e.info;
        saveFolder_ = e.folder;
        saveFocus_ = Field::SaveDescription;
    } else { // saving the patch, under its preset's name
        saveMode_ = SaveMode::Save;
        saveIndex_ = -1;
        const int now = presetStore().presetIndex();
        saveName_.text = controller().presetHasFile()
                             ? presetStore().presetName()
                             : std::string();
        info = controller().layout().presetInfo;
        // In its preset's folder, a preset of the user's; else in the folder the browser was left on; else in User.
        saveFolder_.clear();
        if (now >= 0 && now < static_cast<int>(presets.size()) && !presets[now].factory) {
            saveFolder_ = presets[now].folder;
        } else if (const std::string place = presetStore().presetBrowserPlace(); place.starts_with("folder:")) {
            const std::vector<std::string> folders = saveFolders();
            if (std::find(folders.begin(), folders.end(), place.substr(7)) != folders.end())
                saveFolder_ = place.substr(7);
        }
        saveFocus_ = Field::SaveName;
    }
    // The author: the preset's own; where it has none, the one the user typed last.
    saveAuthor_.text = info.author.empty() ? presetStore().presetAuthor() : info.author;
    saveAuthorTyped_ = false;

    saveDescription_.text = info.description;
    saveCategory_ = info.category;
    saveName_.most = kMaxPresetName;
    saveAuthor_.most = kMaxPresetAuthor;
    saveDescription_.most = kMaxPresetDescription;
    for (TextField *t : {&saveName_, &saveAuthor_, &saveDescription_})
        t->set(std::move(t->text)); // whole characters (UTF-8 from a file), as many as it holds
    // The name all selected, to type over.
    if (saveFocus_ == Field::SaveName)
        saveName_.anchor = 0;
    saveFolderList_ = false;
    saveFolderScroll_ = 0.0f;
    browserOpen_ = false;
    infoCardOpen_ = false;
    nameMenu_ = 0;
    host_.resetHover();
}

// The folders the user's presets are files in: not Factory's, nor All, Favorites and Recent, which aren't places.
bool PresetBrowser::saveHereShown() const {
    return browserScope_ == BrowserScope::Folder && presetStore().writesPresetFiles();
}

bool PresetBrowser::saveHereOn() const { // as Save As, greyed out on Init until the patch changes
    return controller().presetHasFile() || controller().patchEdited();
}

void PresetBrowser::openSaveIn(const std::string &folder) {
    openSave();
    const std::vector<std::string> folders = saveFolders();
    if (std::find(folders.begin(), folders.end(), folder) != folders.end()) // not gone since the browser opened
        saveFolder_ = folder;
}

float PresetBrowser::saveFolderListEnd() const {
    const int rows = static_cast<int>(saveFolders().size()) + 1; // Factory's too
    return std::max(rows - kFolderRowsShown, 0) * kFolderRowH;
}

void PresetBrowser::toggleSaveFolderList() {
    saveFolderList_ = !saveFolderList_;
    host_.resetHover();
    if (!saveFolderList_)
        return;
    // Scrolled just far enough to show the folder picked, whose row comes after Factory's.
    const std::vector<std::string> folders = saveFolders();
    const auto at = std::find(folders.begin(), folders.end(), saveFolder_);
    const float rowEnd = at == folders.end() ? 0.0f : static_cast<float>(at - folders.begin() + 2) * kFolderRowH;
    const float shown = static_cast<float>(std::min(static_cast<int>(folders.size()) + 1, kFolderRowsShown)) * kFolderRowH;
    saveFolderScroll_ = std::clamp(rowEnd - shown, 0.0f, saveFolderListEnd());
}

std::vector<Widget> PresetBrowser::saveWidgets() const {
    const SaveLayout l = saveLayout();
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1, Rect clip = {}) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        w.clip = clip;
        list.push_back(w);
    };
    add(Kind::SaveBackdrop, {0.0f, 0.0f, View::kWidth, host_.height()}, 0);
    add(Kind::SaveBackdrop, l.panel, 1);
    add(Kind::SaveClose, l.close);
    if (saveMode_ == SaveMode::Save)
        add(Kind::SearchField, l.name, static_cast<int>(Field::SaveName));
    add(Kind::SearchField, l.author, static_cast<int>(Field::SaveAuthor));
    const std::vector<CategoryRow> categories = saveCategoryList();
    for (size_t i = 0; i < l.chips.size(); ++i) {
        if (i < categories.size())
            add(Kind::SaveCategory, l.chips[i], static_cast<int>(i));
        else if (i == categories.size())
            add(Kind::SaveCategory, l.chips[i], -1);
        else
            add(Kind::SaveNewCategory, l.chips[i]);
    }
    add(Kind::SearchField, l.description, static_cast<int>(Field::SaveDescription));
    add(Kind::SaveFolderList, l.folder);
    if (saveMode_ == SaveMode::Save && presetStore().writesPresetFiles())
        add(Kind::SaveToFile, l.toFile);
    add(Kind::SaveButton, l.cancel, 0);
    add(Kind::SaveButton, l.save, 1);
    if (saveFolderList_) { // over the rest, which a click around it leaves alone, closing it
        add(Kind::SaveBackdrop, {0.0f, 0.0f, View::kWidth, host_.height()}, 2);
        add(Kind::SaveBackdrop, l.list, 3);
        const std::vector<std::string> folders = saveFolders();
        for (int i = 0; i < static_cast<int>(folders.size()); ++i) {
            const Rect r{l.listRows.x, l.listRows.y + (i + 1) * kFolderRowH - saveFolderScroll_, l.listRows.w,
                         kFolderRowH - 1.0f};
            if (r.bottom() > l.listRows.y && r.y < l.listRows.bottom())
                add(Kind::SaveFolderRow, r, i, l.listRows);
        }
        add(Kind::SaveNewFolder, l.listNew);
    }
    return list;
}

void PresetBrowser::paintSave(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, host_.height()}, kBackground.alpha(0.7f));
    const SaveLayout l = saveLayout();
    const Rect p = l.panel;
    if (look.panelShadow > 0.0f)
        softShadow(c, p, kRadiusPanel, 6, 1.5f, 4.0f, std::min(look.panelShadow * 1.5f, 0.4f));
    c.fillRect(p, kPanel, kRadiusPanel);
    if (look.panelShadow <= 0.0f)
        c.strokeRect(p, kBorder, 1.0f, kRadiusPanel);
    c.text({p.x + 24.0f, p.y + 14.0f, 300.0f, 24.0f}, tr(saveMode_ == SaveMode::Info ? Str::SaveInfoTitle : Str::SaveTitle),
           kTextHeading, kText, Align::Left, true);
    c.line({p.x + 1.0f, p.y + 50.5f}, {p.right() - 1.0f, p.y + 50.5f}, kBorder, 1.0f);
    auto label = [&](Rect field, std::string_view text, std::string_view after = {}) {
        c.text({field.x, field.y - 18.0f, field.w, 16.0f}, text, kTextLabel, kDim, Align::Left, true);
        if (!after.empty())
            c.text({field.x, field.y - 18.0f, field.w, 16.0f}, after, kTextLabel, kFaint, Align::Right);
    };
    label(l.name, tr(Str::SaveName));
    if (saveMode_ == SaveMode::Save) {
        host_.paintField(c, Field::SaveName, l.name, formFieldTyped(l.name), tr(Str::SaveNameHint), {}, false);
    } else { // the name stays: shown, not edited
        c.fillRect(l.name, wellColor(), kRadiusControl);
        c.text({l.name.x + 10.0f, l.name.y, l.name.w - 20.0f, l.name.h}, saveName_.text, kTextBody, kDim, Align::Left);
    }
    label(l.author, tr(Str::SaveAuthor));
    host_.paintField(c, Field::SaveAuthor, l.author, formFieldTyped(l.author), tr(Str::SaveAuthorHint), {}, false);
    label({l.chips.front().x, l.chips.front().y, 200.0f, 16.0f}, tr(Str::SaveCategory));
    const std::string count = trf(Str::SaveCount, {std::to_string(saveDescription_.text.size()),
                                                   std::to_string(kMaxPresetDescription)});
    label(l.description, tr(Str::SaveDescription), count);
    host_.paintField(c, Field::SaveDescription, l.description, formFieldTyped(l.description), tr(Str::SaveDescriptionHint), {},
               false);
    label(l.folder, tr(Str::SaveFolder));
    // Replacing a preset says so.
    if (l.warning.w > 0.0f) {
        const Color amber = Color::hex(0xF2C14E);
        c.fillRect(l.warning, amber.alpha(0.12f), kRadiusControl);
        c.strokeRect(l.warning, amber.alpha(0.45f), 1.0f, kRadiusControl);
        const Point m{l.warning.x + 16.0f, l.warning.center().y};
        const Point triangle[] = {{m.x, m.y - 6.0f}, {m.x + 6.5f, m.y + 5.0f}, {m.x - 6.5f, m.y + 5.0f}, {m.x, m.y - 6.0f}};
        c.polyline(triangle, 4, amber, 1.2f);
        c.line({m.x, m.y - 2.0f}, {m.x, m.y + 1.5f}, amber, 1.3f);
        c.text({l.warning.x + 32.0f, l.warning.y, l.warning.w - 40.0f, l.warning.h},
               trf(Str::SaveExists, {trimmed(saveName_.text)}), kTextLabel, kText, Align::Left);
    }
    for (const Widget &w : saveWidgets())
        if (w.kind != Kind::SearchField && w.kind != Kind::SaveBackdrop && w.kind != Kind::SaveFolderRow &&
            w.kind != Kind::SaveNewFolder)
            host_.paintWidget(c, w);
    if (saveFolderList_)
        paintSaveFolderList(c, l);
}

// The list of folders: Factory's locked (nothing can go in), then User and its folders, the one picked marked, then
// New Folder under a line.
void PresetBrowser::paintSaveFolderList(Canvas &c, const SaveLayout &l) {
    paintMenuPanel(c, l.list, kRadiusCard);
    const Rect box = l.listRows;
    c.pushClip(box);
    const Rect factory{box.x, box.y - saveFolderScroll_, box.w, kFolderRowH - 1.0f};
    paintFolderIcon(c, {factory.x + 10.0f, factory.y + 6.5f}, kFaint, false, true);
    c.text({factory.x + 30.0f, factory.y, factory.w - 60.0f, factory.h}, tr(Str::PresetsFactory), kTextBody, kFaint,
           Align::Left);
    paintPadlock(c, {factory.right() - 14.0f, factory.center().y}, kFaint);
    const std::vector<std::string> folders = saveFolders();
    const std::vector<Widget> widgets = saveWidgets();
    for (const Widget &w : widgets) {
        if (w.kind != Kind::SaveFolderRow || w.a < 0 || w.a >= static_cast<int>(folders.size()))
            continue;
        const Rect r = w.rect;
        const bool on = folders[w.a] == saveFolder_, hovered = host_.isHovered(w);
        paintMenuRow(c, r, hovered, on);
        const int depth =
            w.a == 0 ? 0 : 1 + static_cast<int>(std::count(folders[w.a].begin(), folders[w.a].end(), '/'));
        const float x = r.x + 10.0f + depth * 14.0f;
        paintFolderIcon(c, {x, r.y + 6.5f}, on ? kAccent : kDim, on, false);
        c.text({x + 20.0f, r.y, r.right() - x - 24.0f, r.h},
               fitted(c, folderName(folders[w.a], tr(Str::PresetsUser)), kTextBody, on, r.right() - x - 26.0f),
               kTextBody, on || hovered ? kText : kDim, Align::Left, on);
    }
    c.popClip();
    // More rows than it shows: a thumb at its right edge, as far down as it is scrolled.
    if (const float end = saveFolderListEnd(); end > 0.0f) {
        const float thumb = box.h * box.h / (box.h + end);
        c.fillRect({box.right() - 3.0f, box.y + (box.h - thumb) * saveFolderScroll_ / end, 3.0f, thumb}, kFaint,
                   kRadiusMark);
    }
    c.line({l.list.x + 1.0f, l.listNew.y - 4.5f}, {l.list.right() - 1.0f, l.listNew.y - 4.5f}, kBorder, 1.0f);
    for (const Widget &w : widgets)
        if (w.kind == Kind::SaveNewFolder)
            host_.paintWidget(c, w);
}

void PresetBrowser::saveKey(Key key) {
    if (key == Key::Escape && saveFolderList_) { // the list of folders closes first
        saveFolderList_ = false;
        host_.resetHover();
    } else if (key == Key::Enter) {
        saveFolderList_ = false;
        runSave();
    } else if (key == Key::Escape) {
        saveMode_ = SaveMode::None;
        host_.resetHover();
    } else if (key == Key::Tab || key == Key::BackTab) {
        stepSaveField(key == Key::Tab ? 1 : -1);
    }
}

// Tab and Shift+Tab: the sheet's next text field or the one before, round its ends, its text all selected to type
// over. Editing a preset's info, the name isn't one of them.
void PresetBrowser::stepSaveField(int step) {
    static constexpr Field kFields[] = {Field::SaveName, Field::SaveAuthor, Field::SaveDescription};
    const int first = saveMode_ == SaveMode::Save ? 0 : 1, count = 3 - first;
    const int at = static_cast<int>(std::find(std::begin(kFields), std::end(kFields), saveFocus_) - std::begin(kFields));
    TextField &left = *field(saveFocus_);
    left.anchor = left.caret; // a field shows its selection with or without the keyboard
    saveFocus_ = kFields[first + (at - first + step + count) % count];
    TextField &entered = *field(saveFocus_);
    entered.anchor = 0;
    entered.caret = entered.text.size();
}

void PresetBrowser::runSave() {
    const std::string name = trimmed(saveName_.text);
    if (saveMode_ == SaveMode::Save && !fileNameOk(name)) {
        host_.flash(tr(name.empty() ? Str::MsgNameEmpty : Str::MsgNameInvalid));
        saveFocus_ = Field::SaveName;
        return;
    }
    const PresetInfo info{trimmed(saveAuthor_.text), saveCategory_, trimmed(saveDescription_.text)};
    // Saved, it says nothing: the sheet closing shows it. A failure says so, the sheet staying open.
    if (saveMode_ == SaveMode::Info) { // its info written, then its file moved if another folder is picked
        if (saveIndex_ < 0 || saveIndex_ >= static_cast<int>(presetStore().presets().size())) {
            saveMode_ = SaveMode::None; // gone meanwhile
            host_.resetHover();
            return;
        }
        const PresetEntry entry = presetStore().presets()[saveIndex_]; // a copy: writing scans the library again
        if (!presetStore().setPresetInfo(saveIndex_, info)) {
            host_.flash(tr(Str::MsgPresetNotSaved));
            return;
        }
        if (saveFolder_ != entry.folder) {
            const std::vector<PresetEntry> &presets = presetStore().presets();
            int at = -1;
            for (int i = 0; i < static_cast<int>(presets.size()); ++i)
                if (!presets[i].factory && presets[i].folder == entry.folder && presets[i].name == entry.name)
                    at = i;
            if (!presetStore().movePreset(at, saveFolder_)) {
                host_.flash(tr(Str::MsgPresetNotMoved));
                return;
            }
        }
    } else if (!presetStore().savePresetIn(saveFolder_, name, info)) {
        host_.flash(tr(Str::MsgPresetNotSaved));
        return;
    }
    // An author typed in is the one the next preset starts with.
    if (saveAuthorTyped_ && !info.author.empty())
        presetStore().setPresetAuthor(info.author);
    saveMode_ = SaveMode::None;
    host_.resetHover();
}

// --- The New Category and New Folder forms

void PresetBrowser::openNameMenu(int mode, Point at) {
    nameMenu_ = mode;
    nameMenuAt_ = at;
    newName_.clear();
    newName_.most = mode == 1 ? kMaxPresetCategory : kMaxPresetName;
    nameColor_ = kBuiltInCategories; // the first of the user's colors
    host_.resetHover();
}

Rect PresetBrowser::nameMenuPanel() const {
    constexpr float w = 300.0f, h = 122.0f;
    return {std::clamp(nameMenuAt_.x, 8.0f, View::kWidth - 8.0f - w), std::clamp(nameMenuAt_.y, 8.0f, host_.height() - 8.0f - h),
            w, h};
}

namespace {
Rect nameField(Rect p) {
    return {p.x + 16.0f, p.y + 38.0f, p.w - 32.0f, 30.0f};
}
Rect nameButton(Rect p, int ok) {
    return {p.right() - 16.0f - (ok ? 70.0f : 148.0f), p.bottom() - 14.0f - 28.0f, 70.0f, 28.0f};
}
Rect nameSwatch(Rect p, int i) {
    return {p.x + 16.0f + i * 24.0f, p.bottom() - 14.0f - 24.0f, 20.0f, 20.0f};
}
} // namespace

std::vector<Widget> PresetBrowser::nameMenuWidgets() const {
    const Rect p = nameMenuPanel();
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::NameMenu, {0.0f, 0.0f, View::kWidth, host_.height()}, 0);
    add(Kind::NameMenu, p, 1);
    add(Kind::SearchField, nameField(p), static_cast<int>(Field::NewName));
    if (nameMenu_ == 1)
        for (int i = 0; i < kCustomCategoryColors; ++i)
            add(Kind::NameColor, nameSwatch(p, i), kBuiltInCategories + i);
    add(Kind::NameButton, nameButton(p, 0), 0);
    add(Kind::NameButton, nameButton(p, 1), 1);
    return list;
}

void PresetBrowser::paintNameMenu(Canvas &c) {
    const Rect p = nameMenuPanel();
    paintMenuPanel(c, p, kRadiusCard);
    c.text({p.x + 16.0f, p.y + 12.0f, p.w - 32.0f, 18.0f},
           tr(nameMenu_ == 1 ? Str::SaveNewCategory : nameMenu_ == 4 ? Str::RenameFolderTitle : Str::SaveNewFolder),
           kTextBody, kText, Align::Left, true);
    host_.paintField(c, Field::NewName, nameField(p), formFieldTyped(nameField(p)),
               tr(nameMenu_ == 1 ? Str::SaveCategoryHint : Str::SaveFolderHint), {}, false);
    for (const Widget &w : nameMenuWidgets())
        if (w.kind == Kind::NameColor || w.kind == Kind::NameButton)
            host_.paintWidget(c, w);
}

void PresetBrowser::nameMenuKey(Key key) {
    if (key == Key::Enter)
        runNameMenu();
    else if (key == Key::Escape) {
        nameMenu_ = 0;
        host_.resetHover();
    }
}

void PresetBrowser::runNameMenu() {
    const std::string name = trimmed(newName_.text);
    if (name.empty())
        return;
    if (nameMenu_ == 1) { // a category: a built-in one's name picks that one
        const int built = builtInCategory(name);
        if (built < 0)
            presetStore().addUserCategory({name, nameColor_});
        if (saveMode_ != SaveMode::None)
            saveCategory_ = built >= 0 ? kPresetCategories[built].name : name;
    } else if (nameMenu_ == 4) { // the browser's folder renamed, in the folder it is in
        if (!fileNameOk(name)) {
            host_.flash(tr(Str::MsgNameInvalid));
            return;
        }
        const size_t slash = renameFolder_.rfind('/');
        const std::string to = slash == std::string::npos ? name : renameFolder_.substr(0, slash + 1) + name;
        if (to != renameFolder_) {
            const std::vector<std::string> folders = presetStore().presetFolders();
            const bool taken = lowerAscii(to) != lowerAscii(renameFolder_) &&
                               std::any_of(folders.begin(), folders.end(), [&](const std::string &f) {
                                   return lowerAscii(f) == lowerAscii(to);
                               });
            if (taken) {
                host_.flash(tr(Str::MsgFolderExists));
                return;
            }
            std::optional<PresetEntry> picked = entryAt(presetStore().presets(), browserPick_);
            if (!presetStore().renamePresetFolder(renameFolder_, to)) {
                host_.flash(tr(Str::MsgFolderNotRenamed));
                return;
            }
            // What showed the folder or one in it follows it: the browser's tree and the folder it shows, and the
            // preset picked, found again in the library scanned again (another place in its list).
            auto moved = [&](std::string &f) {
                if (f == renameFolder_ || f.starts_with(renameFolder_ + "/"))
                    f = to + f.substr(renameFolder_.size());
            };
            if (picked && !picked->factory)
                moved(picked->folder);
            browserPick_ = indexOfEntry(presetStore().presets(), picked);
            std::set<std::string> opened; // the browser's tree keeps it and those in it opened
            for (std::string f : browserOpenFolders_) {
                moved(f);
                opened.insert(f);
            }
            browserOpenFolders_ = std::move(opened);
            if (browserScope_ == BrowserScope::Folder) {
                const std::string before = browserFolder_;
                moved(browserFolder_);
                if (browserFolder_ != before)
                    rememberBrowserPlace();
            }
        }
    } else { // a folder, in the folder shown (the browser) or picked (the Save sheet)
        if (!fileNameOk(name)) {
            host_.flash(tr(Str::MsgNameInvalid));
            return;
        }
        const std::string parent = nameMenu_ == 3 ? saveFolder_
                                   : browserScope_ == BrowserScope::Folder ? browserFolder_
                                                                           : std::string();
        const std::string folder = parent.empty() ? name : parent + "/" + name;
        if (!presetStore().makePresetFolder(folder)) {
            host_.flash(tr(Str::MsgFolderNotMade));
            return;
        }
        if (nameMenu_ == 3) {
            saveFolder_ = folder;
        } else {
            browserScope_ = BrowserScope::Folder;
            browserFolder_ = folder;
            browserOpenFolders_.insert(parent);
            browserOpenFolders_.insert("");
            browserScroll_ = 0;
            rememberBrowserPlace();
        }
    }
    nameMenu_ = 0;
    host_.resetHover();
}

// --- Painting, pressing, scrolling and the tooltips of all the above

bool PresetBrowser::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const std::vector<PresetEntry> &presets = presetStore().presets();
    const bool picked = browserPick_ >= 0 && browserPick_ < static_cast<int>(presets.size());
    switch (w.kind) {
    case Kind::PresetNameBox:
        paintPresetBar(c, w, hovered);
        return true;
    case Kind::PresetInfoButton: {
        const bool lit = hovered || infoCardOpen_;
        if (lit)
            c.fillRect(w.rect, infoCardOpen_ ? kAccent.alpha(0.3f) : kText.alpha(0.1f), kRadiusControl);
        const Point m = w.rect.center();
        const Color ink = lit ? kText : kDim;
        c.strokeCircle(m, 6.0f, ink, 1.2f);
        c.line({m.x, m.y - 0.5f}, {m.x, m.y + 3.0f}, ink, 1.4f);
        c.fillCircle({m.x, m.y - 3.0f}, 0.9f, ink);
        return true;
    }
    case Kind::InfoAction: {
        if (w.a == 0) {
            const bool favorite = presetStore().presetFavorite(presetStore().presetIndex());
            if (hovered)
                c.fillRect(w.rect, kText.alpha(0.08f), kRadiusControl);
            if (outlined())
                c.strokeRect(w.rect, kBorder, 1.0f, kRadiusControl);
            paintStar(c, w.rect.center(), 6.5f, favorite ? Color::hex(0xF2C14E) : (hovered ? kText : kDim), favorite);
            return true;
        }
        const bool primary = w.a == 2;
        const Color ink = buttonFace(c, w.rect, primary, hovered);
        Str label = Str::PresetsShowInBrowser;
        if (primary)
            label = controller().presetFactory()                        ? Str::PresetsSaveCopy
                    : controller().layout().presetInfo.empty()           ? Str::PresetsAddInfo
                                                                         : Str::PresetsEditInfo;
        c.text(w.rect, tr(label), kTextLabel, ink, Align::Center, primary);
        return true;
    }
    case Kind::BrowserSubfolders: {
        // Greyed out, keeping its setting, but for a folder of the user's: All, Favorites, Recent and Factory have none.
        // A switch and its label on the panel, without a button's face; the label lit under the mouse.
        const bool on = browserScope_ == BrowserScope::Folder;
        const Color ink = !on ? kFaint : hovered ? kText : kDim;
        const Rect track{w.rect.x + 10.0f, w.rect.center().y - 6.5f, 24.0f, 13.0f};
        c.fillRect(track, browserSubfolders_ ? (on ? kAccent : kAccent.alpha(0.35f)) : kBorder, track.h * 0.5f);
        c.fillCircle({browserSubfolders_ ? track.right() - 6.5f : track.x + 6.5f, track.center().y}, 4.5f,
                     !on ? kFaint : browserSubfolders_ ? litInk(kText) : kDim);
        c.text({track.right() + 8.0f, w.rect.y, w.rect.right() - track.right() - 10.0f, w.rect.h},
               tr(Str::PresetsSubfolders), kTextLabel, ink, Align::Left);
        return true;
    }
    case Kind::BrowserClose: {
        const Color ink = buttonFace(c, w.rect, false, hovered, true, true);
        c.text(w.rect, tr(Str::PresetsClose), kTextBody, ink, Align::Center);
        return true;
    }
    case Kind::BrowserSaveHere: {
        const bool on = saveHereOn();
        const Color ink = buttonFace(c, w.rect, true, hovered && on, on, true);
        c.text(w.rect, tr(Str::PresetsSaveHere), kTextBody, ink, Align::Center, on);
        return true;
    }
    case Kind::BrowserShowFile: {
        const bool on = picked && !presets[browserPick_].factory;
        const Color ink = buttonFace(c, w.rect, false, hovered && on, on, true);
        // A page with an arrow out of it.
        const Point m = w.rect.center();
        const Point page[] = {{m.x - 4.5f, m.y - 6.0f}, {m.x + 1.5f, m.y - 6.0f}, {m.x + 4.5f, m.y - 3.0f},
                              {m.x + 4.5f, m.y + 6.0f}, {m.x - 4.5f, m.y + 6.0f}, {m.x - 4.5f, m.y - 6.0f}};
        c.polyline(page, std::size(page), ink, 1.1f);
        c.line({m.x - 2.0f, m.y + 3.0f}, {m.x + 2.0f, m.y - 1.0f}, ink, 1.1f);
        c.line({m.x - 0.5f, m.y - 1.0f}, {m.x + 2.0f, m.y - 1.0f}, ink, 1.1f);
        c.line({m.x + 2.0f, m.y - 1.0f}, {m.x + 2.0f, m.y + 1.5f}, ink, 1.1f);
        return true;
    }
    case Kind::SaveCategory: {
        const std::vector<CategoryRow> categories = saveCategoryList();
        const bool none = w.a < 0 || w.a >= static_cast<int>(categories.size());
        const std::string name = none ? std::string() : categories[w.a].name;
        const bool on = lowerAscii(name) == lowerAscii(saveCategory_);
        const Color color = none ? kFaint : categoryColor(categories[w.a].color);
        if (on && !none) {
            c.fillRect(w.rect, hovered ? mix(color, kText, 0.15f) : color, kRadiusControl);
        } else {
            if (hovered)
                c.fillRect(w.rect, kText.alpha(0.06f), kRadiusControl);
            c.strokeRect(w.rect, on ? kAccent : kBorder, 1.0f, kRadiusControl, none);
        }
        const std::string label = none ? std::string(tr(Str::PresetsNoneCategory)) : categories[w.a].label;
        float x = w.rect.x + 10.0f;
        if (!none && !on) {
            c.fillRect({x, w.rect.center().y - 3.5f, 7.0f, 7.0f}, color, kRadiusMark);
            x += 13.0f;
        }
        c.text({x, w.rect.y, w.rect.right() - x - 6.0f, w.rect.h}, fitted(c, label, kTextLabel, on, w.rect.right() - x - 8.0f),
               kTextLabel, on && !none ? kPillInk : (on || hovered ? kText : kDim), none && !on ? Align::Center : Align::Left,
               on);
        return true;
    }
    case Kind::SaveNewCategory: {
        const bool open = nameMenu_ == 1;
        if (hovered || open)
            c.fillRect(w.rect, kAccent.alpha(0.12f), kRadiusControl);
        c.strokeRect(w.rect, hovered || open ? kAccent : kBorder, 1.0f, kRadiusControl, true);
        const Color ink = hovered || open ? kText : kDim;
        const Point m{w.rect.x + 24.0f, w.rect.center().y};
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, ink, 1.3f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, ink, 1.3f);
        c.text({m.x + 9.0f, w.rect.y, w.rect.right() - m.x - 12.0f, w.rect.h}, tr(Str::SaveNew), kTextLabel, ink,
               Align::Left);
        return true;
    }
    case Kind::SaveFolderList: { // as a field, the folder in it, then a chevron
        const bool open = saveFolderList_;
        c.fillRect(w.rect, kBackground, kRadiusControl);
        c.strokeRect(w.rect, open ? kAccent : hovered ? mix(kBorder, kText, 0.25f) : kBorder, 1.0f, kRadiusControl);
        paintFolderIcon(c, {w.rect.x + 10.0f, w.rect.center().y - 6.5f}, hovered || open ? kText : kDim, false, false);
        const float x = w.rect.x + 30.0f, right = w.rect.right() - 28.0f;
        c.text({x, w.rect.y, right - x, w.rect.h},
               fitted(c, folderPath(saveFolder_, tr(Str::PresetsUser)), kTextBody, false, right - x), kTextBody, kText,
               Align::Left);
        paintChevron(c, {w.rect.right() - 14.0f, w.rect.center().y}, hovered || open ? kText : kDim,
                     open ? ChevronWay::Up : ChevronWay::Down);
        return true;
    }
    case Kind::SaveNewFolder: { // the list's last row: a plus, then its name, in the accent
        paintMenuRow(c, w.rect, hovered, false);
        const Color ink = hovered ? kText : kAccent;
        const Point m{w.rect.x + 15.5f, w.rect.center().y};
        c.line({m.x - 4.0f, m.y}, {m.x + 4.0f, m.y}, ink, 1.3f);
        c.line({m.x, m.y - 4.0f}, {m.x, m.y + 4.0f}, ink, 1.3f);
        c.text({w.rect.x + 30.0f, w.rect.y, w.rect.w - 34.0f, w.rect.h}, tr(Str::SaveNewFolder), kTextBody, ink,
               Align::Left);
        return true;
    }
    case Kind::SaveToFile: {
        const Color ink = hovered ? kText : kAccent;
        c.text(w.rect, tr(Str::SaveToFile), kTextLabel, ink, Align::Left);
        return true;
    }
    case Kind::SaveButton: {
        const bool save = w.a == 1;
        const bool replace = save && saveReplaces();
        const Color ink = buttonFace(c, w.rect, save, hovered);
        c.text(w.rect, tr(!save ? Str::SaveCancel : replace ? Str::SaveReplace : Str::SaveSave), kTextBody, ink,
               Align::Center, save);
        return true;
    }
    case Kind::SaveClose:
        c.fillRect(w.rect, hovered ? kTrack : kPanel, kRadiusControl);
        paintCross(c, w.rect.center(), 5.0f, hovered ? kText : kDim);
        return true;
    case Kind::NameColor: {
        const bool on = nameColor_ == w.a;
        c.fillRect(w.rect, categoryColor(w.a), kRadiusMark);
        if (on)
            c.strokeRect({w.rect.x - 2.5f, w.rect.y - 2.5f, w.rect.w + 5.0f, w.rect.h + 5.0f}, kText, 1.5f,
                         kRadiusMark + 2.5f);
        else if (hovered)
            c.strokeRect(w.rect, kText.alpha(0.6f), 1.0f, kRadiusMark);
        return true;
    }
    case Kind::NameButton: {
        const bool ok = w.a == 1;
        const Color ink = buttonFace(c, w.rect, ok, hovered, !ok || !trimmed(newName_.text).empty());
        c.text(w.rect, tr(!ok ? Str::SaveCancel : nameMenu_ == 1 ? Str::SaveAdd : nameMenu_ == 4 ? Str::ButtonRenameFolder : Str::SaveCreate), kTextLabel, ink,
               Align::Center, ok);
        return true;
    }
    case Kind::InfoCard:
    case Kind::BrowserRow:
    case Kind::BrowserFav:
    case Kind::BrowserLibrary:
    case Kind::BrowserTree:
    case Kind::BrowserTreeToggle:
    case Kind::BrowserCategory:
    case Kind::BrowserCategoryRemove:
    case Kind::BrowserCatClear:
    case Kind::BrowserNew:
    case Kind::BrowserCrumb:
    case Kind::BrowserChip:
    case Kind::SaveBackdrop:
    case Kind::SaveFolderRow:
    case Kind::NameMenu:
        return true; // drawn with what they are in
    default:
        return false;
    }
}

bool PresetBrowser::press(const Widget &w, Point p, Modifiers) {
    const std::vector<PresetEntry> &presets = presetStore().presets();
    const bool picked = browserPick_ >= 0 && browserPick_ < static_cast<int>(presets.size());
    switch (w.kind) {
    case Kind::PresetNameBox:
        if (browserOpen_) {
            browserOpen_ = false;
            host_.resetHover();
        } else {
            openBrowser();
        }
        return true;
    case Kind::PresetInfoButton:
        infoCardOpen_ = !infoCardOpen_;
        browserOpen_ = false;
        host_.resetHover();
        return true;
    case Kind::InfoCard:
        if (w.a == 0)
            infoCardOpen_ = false;
        return true;
    case Kind::InfoAction: {
        const int now = presetStore().presetIndex();
        if (w.a == 0)
            presetStore().setPresetFavorite(now, !presetStore().presetFavorite(now));
        else if (w.a == 1)
            openBrowser(now);
        else if (controller().presetFactory())
            openSave();
        else
            openSave(now);
        return true;
    }
    case Kind::BrowserBackdrop:
        if (w.a == 0) // a click around the browser closes it, putting back the patch it previewed over
            browserOpen_ = false;
        return true;
    case Kind::BrowserRow: // picked and loaded, the browser staying open for the next
        browserPick_ = w.a;
        openPreset(w.a, false);
        return true;
    case Kind::BrowserFav:
        presetStore().setPresetFavorite(w.a, !presetStore().presetFavorite(w.a));
        return true;
    case Kind::BrowserSubfolders:
        if (browserScope_ != BrowserScope::Folder) // greyed out
            return true;
        browserSubfolders_ = !browserSubfolders_;
        browserScroll_ = 0;
        revealBrowserRow(browserPick_, true);
        return true;
    case Kind::BrowserLibrary:
        browserScope_ = static_cast<BrowserScope>(w.a);
        browserScroll_ = 0;
        rememberBrowserPlace();
        return true;
    case Kind::BrowserTree: {
        const std::vector<TreeRow> tree = browserTree();
        if (w.a < 0 || w.a >= static_cast<int>(tree.size()))
            return true;
        browserScope_ = tree[w.a].scope;
        browserFolder_ = tree[w.a].folder;
        if (tree[w.a].hasKids)
            browserOpenFolders_.insert(tree[w.a].folder);
        browserScroll_ = 0;
        rememberBrowserPlace();
        return true;
    }
    case Kind::BrowserTreeToggle: {
        const std::vector<TreeRow> tree = browserTree();
        if (w.a >= 0 && w.a < static_cast<int>(tree.size())) {
            const std::string &folder = tree[w.a].folder;
            if (!browserOpenFolders_.erase(folder))
                browserOpenFolders_.insert(folder);
            browserSideScroll_ = std::min(browserSideScroll_, browserSideEnd());
        }
        return true;
    }
    case Kind::BrowserCategory:
    case Kind::BrowserChip: {
        const std::vector<CategoryRow> categories = browserCategoryList();
        if (w.a >= 0 && w.a < static_cast<int>(categories.size())) {
            const std::string key = lowerAscii(categories[w.a].name);
            if (!browserCategories_.erase(key) && w.kind == Kind::BrowserCategory)
                browserCategories_.insert(key);
        }
        browserScroll_ = 0;
        host_.resetHover();
        return true;
    }
    case Kind::BrowserCategoryRemove: { // asking first
        const std::vector<CategoryRow> categories = browserCategoryList();
        if (w.a >= 0 && w.a < static_cast<int>(categories.size()) && ownCategory(categories[w.a])) {
            confirmCategory_ = categories[w.a].name;
            host_.askConfirm(*this, kConfirmCategoryRemove);
        }
        host_.resetHover();
        return true;
    }
    case Kind::BrowserCatClear:
        browserCategories_.clear();
        browserScroll_ = 0;
        host_.resetHover();
        return true;
    case Kind::BrowserNew:
        openNameMenu(2, {w.rect.x - 8.0f, w.rect.bottom() + 6.0f});
        return true;
    case Kind::BrowserCrumb: {
        std::string folder;
        int parts = 0;
        for (size_t at = 0; parts < w.a && at <= browserFolder_.size(); ++parts) {
            const size_t end = std::min(browserFolder_.find('/', at), browserFolder_.size());
            folder = browserFolder_.substr(0, end);
            at = end + 1;
        }
        browserFolder_ = folder;
        browserScroll_ = 0;
        rememberBrowserPlace();
        host_.resetHover();
        return true;
    }
    case Kind::BrowserSideScroll: { // on the thumb, drag it; elsewhere on the track it jumps to center on the click
        const Rect track = browserSideTrack(), thumb = browserSideThumb();
        if (p.y < thumb.y || p.y >= thumb.bottom())
            browserSideScroll_ = std::clamp((p.y - thumb.h * 0.5f - track.y) / std::max(track.h - thumb.h, 1.0f) *
                                                browserSideEnd(),
                                            0.0f, browserSideEnd());
        host_.startDrag(*this, w, p, false);
        sideDragFrom_ = browserSideScroll_;
        return true;
    }
    case Kind::BrowserClose: // as a click around it
        browserOpen_ = false;
        host_.resetHover();
        return true;
    case Kind::BrowserShowFile:
        if (picked && !presets[browserPick_].factory)
            presetStore().showPresetFile(browserPick_);
        return true;
    case Kind::BrowserSaveHere:
        if (saveHereOn())
            openSaveIn(browserFolder_);
        return true;
    case Kind::SaveBackdrop: // a stray click around it keeps what is typed; around the open list, closes the list
        if (w.a == 2) {
            saveFolderList_ = false;
            host_.resetHover();
        }
        return true;
    case Kind::SaveFolderList:
        toggleSaveFolderList();
        return true;
    case Kind::SaveCategory: {
        const std::vector<CategoryRow> categories = saveCategoryList();
        saveCategory_ = w.a >= 0 && w.a < static_cast<int>(categories.size()) ? categories[w.a].name : std::string();
        return true;
    }
    case Kind::SaveNewCategory:
        openNameMenu(1, {w.rect.x, w.rect.bottom() + 6.0f});
        return true;
    case Kind::SaveFolderRow: { // picked, the list closing
        const std::vector<std::string> folders = saveFolders();
        if (w.a >= 0 && w.a < static_cast<int>(folders.size()))
            saveFolder_ = folders[w.a];
        saveFolderList_ = false;
        host_.resetHover();
        return true;
    }
    case Kind::SaveNewFolder: { // in the folder picked, under the list's button
        saveFolderList_ = false;
        const Rect folder = saveLayout().folder;
        openNameMenu(3, {folder.x, folder.bottom() + 6.0f});
        return true;
    }
    case Kind::SaveToFile: // the file dialog, with what is typed as the preset's info
        controller().layout().presetInfo = {trimmed(saveAuthor_.text), saveCategory_, trimmed(saveDescription_.text)};
        if (saveAuthorTyped_ && !trimmed(saveAuthor_.text).empty())
            presetStore().setPresetAuthor(trimmed(saveAuthor_.text));
        saveMode_ = SaveMode::None;
        host_.resetHover();
        presetStore().choosePreset(true);
        return true;
    case Kind::SaveButton:
        if (w.a == 1)
            runSave();
        else {
            saveMode_ = SaveMode::None;
            host_.resetHover();
        }
        return true;
    case Kind::SaveClose:
        saveMode_ = SaveMode::None;
        host_.resetHover();
        return true;
    case Kind::NameMenu:
        if (w.a == 0)
            nameMenu_ = 0;
        return true;
    case Kind::NameColor:
        nameColor_ = w.a;
        return true;
    case Kind::NameButton:
        if (w.a == 1)
            runNameMenu();
        else
            nameMenu_ = 0;
        host_.resetHover();
        return true;
    case Kind::SearchField: // a field of the Save sheet takes the keyboard, and so does the browser's search
        if (w.a == static_cast<int>(Field::SaveName) || w.a == static_cast<int>(Field::SaveAuthor) ||
            w.a == static_cast<int>(Field::SaveDescription))
            saveFocus_ = static_cast<Field>(w.a);
        if (w.a == static_cast<int>(Field::Browser))
            browserSearching_ = true;
        return false; // the caret goes where it is clicked, as in any field
    case Kind::SearchClear: // the browser's search keeps the keyboard, for what is typed next
        if (w.a != static_cast<int>(Field::Browser))
            return false;
        host_.clearField(Field::Browser);
        revealBrowserRow(presetStore().presetIndex(), true);
        browserSearching_ = true;
        host_.resetHover();
        return true;
    default:
        return false;
    }
}

bool PresetBrowser::doubleClick(const Widget &w, Point, Modifiers) {
    if (w.kind == Kind::BrowserRow) {
        openPreset(w.a);
        return true;
    }
    // Quick clicks on these are clicks each.
    if (w.kind == Kind::BrowserFav || w.kind == Kind::BrowserTreeToggle || w.kind == Kind::BrowserCategory ||
        w.kind == Kind::BrowserSubfolders || w.kind == Kind::InfoAction || w.kind == Kind::PresetInfoButton) {
        press(w, w.rect.center(), {});
        return true;
    }
    return false;
}

bool PresetBrowser::wheel(Point p, float steps) {
    if (nameMenu_ != 0)
        return true;
    if (saveMode_ != SaveMode::None) {
        if (saveFolderList_ && saveLayout().list.contains(p))
            saveFolderScroll_ =
                std::clamp(saveFolderScroll_ - steps * kFolderRowH * 2.0f, 0.0f, saveFolderListEnd());
        return true;
    }
    if (!browserOpen_)
        return false;
    const Rect panel = browserPanelAt(host_.height());
    if (sidebar(panel).contains(p)) {
        browserSideScroll_ = std::clamp(browserSideScroll_ - steps * 3.0f * kSideRowH, 0.0f, browserSideEnd());
        return true;
    }
    const int most = std::max(static_cast<int>(browserRows().size()) - browserVisible(), 0);
    browserScroll_ = std::clamp(std::clamp(browserScroll_, 0, most) - static_cast<int>(std::lround(steps * 3.0f)), 0, most);
    return true;
}

std::string PresetBrowser::tooltip(const Widget &w) const {
    const std::vector<PresetEntry> &presets = presetStore().presets();
    switch (w.kind) {
    case Kind::PresetInfoButton:
        return infoCardOpen_ ? std::string() : std::string(tr(Str::TipPresetInfo));
    case Kind::InfoAction:
        if (w.a == 0) {
            PresetStore &store = presetStore();
            return tr(store.presetFavorite(store.presetIndex()) ? Str::TipUnfavorite : Str::TipFavorite);
        }
        return {};
    case Kind::BrowserFav:
        return tr(presetStore().presetFavorite(w.a) ? Str::TipUnfavorite : Str::TipFavorite);
    case Kind::BrowserShowFile:
        return tr(browserPick_ >= 0 && browserPick_ < static_cast<int>(presets.size()) && presets[browserPick_].factory
                      ? Str::TipShowFileFactory
                      : Str::TipShowFile);
    case Kind::BrowserSaveHere:
        return saveHereOn() ? trf(Str::TipSaveHere, {folderName(browserFolder_, tr(Str::PresetsUser))})
                            : std::string(tr(Str::TipSaveUnchanged));
    case Kind::BrowserNew:
        return tr(Str::TipNewFolder);
    case Kind::BrowserSubfolders:
        return tr(Str::TipSubfolders);
    case Kind::BrowserCategoryRemove:
        return tr(Str::TipRemoveCategory);
    case Kind::BrowserTreeToggle: {
        const std::vector<TreeRow> tree = browserTree();
        return w.a >= 0 && w.a < static_cast<int>(tree.size()) && tree[w.a].open ? tr(Str::TipCollapseFolder)
                                                                                 : tr(Str::TipExpandFolder);
    }
    case Kind::NameColor:
        return tr(Str::TipCategoryColor);
    default:
        return {};
    }
}


// --- The browser's search and the forms' fields, its keys, its pans, the folders' context menu and the questions it
// asks

// The browser's search lets the keyboard go at a click anywhere else.
void PresetBrowser::pressing(const Widget *hit) {
    if (!(hit && (hit->kind == Kind::SearchField || hit->kind == Kind::SearchClear) &&
          hit->a == static_cast<int>(Field::Browser)))
        browserSearching_ = false;
}

void PresetBrowser::drag(const DragState &d) { // the sidebar follows its thumb
    const float room = browserSideTrack().h - browserSideThumb().h;
    if (room > 0.0f)
        browserSideScroll_ = std::clamp(sideDragFrom_ + (d.now.y - d.start.y) / room * browserSideEnd(), 0.0f,
                                        browserSideEnd());
}

// While it is open, only the browser's list (part 0, in whole rows) and its sidebar (part 1, in px) pan, over
// everything else; the Save sheet, the forms and the info card take none.
std::optional<std::pair<int, float>> PresetBrowser::panAt(Point p) const {
    if (saveMode_ != SaveMode::None || nameMenu_ != 0 || infoCardOpen_ || !browserOpen_)
        return std::nullopt;
    if (browserSideArea().contains(p))
        return browserSideEnd() > 0.0f ? std::optional(std::pair{1, browserSideScroll_}) : std::nullopt;
    if (!browserListArea().contains(p))
        return std::nullopt;
    const int most = std::max(static_cast<int>(browserRows().size()) - browserVisible(), 0);
    return std::pair{0, static_cast<float>(std::clamp(browserScroll_, 0, most))};
}

std::optional<float> PresetBrowser::pan(int part, float first, float, float down) {
    std::optional<float> held;
    auto hold = [&](float wanted, float most) {
        const float to = std::clamp(wanted, 0.0f, std::max(most, 0.0f));
        if (to != wanted)
            held = to;
        return to;
    };
    if (part == 1) { // what was under the mouse follows it
        browserSideScroll_ = hold(first - down, browserSideEnd());
    } else { // in whole rows
        const int rows = static_cast<int>(browserRows().size()) - browserVisible();
        browserScroll_ = static_cast<int>(std::lround(hold(first - down / 28.0f, static_cast<float>(rows))));
    }
    return held;
}

// A folder of the browser's tree (not Factory, which is built in).
bool PresetBrowser::contextMenu(const std::optional<Widget> &hit, Point, std::vector<MenuItem> &items) {
    menuFolder_.reset();
    if (!hit || !browserOpen_ || (hit->kind != Kind::BrowserTree && hit->kind != Kind::BrowserTreeToggle))
        return false;
    const std::vector<TreeRow> tree = browserTree();
    if (hit->a >= 0 && hit->a < static_cast<int>(tree.size()) && tree[hit->a].scope == BrowserScope::Folder)
        menuFolder_ = tree[hit->a].folder;
    if (menuFolder_) {
        menuFolderRow_ = hit->rect;
        items.push_back({kNewFolderCommand, tr(Str::MenuNewFolder)});
        items.push_back({kOpenFolderCommand, tr(Str::MenuOpenInExplorer)});
        if (!menuFolder_->empty()) { // a subfolder, not the preset folder itself
            items.push_back({0, ""});
            items.push_back({kRenameFolderCommand, tr(Str::MenuRenameFolder)});
            items.push_back({kDeleteFolderCommand, tr(Str::MenuDeleteFolder)});
        }
    }
    return true;
}

bool PresetBrowser::menuCommand(int command) {
    if (!menuFolder_ || !browserOpen_)
        return false;
    if (command == kNewFolderCommand) { // in the folder clicked, which the browser then shows
        browserScope_ = BrowserScope::Folder;
        browserFolder_ = *menuFolder_;
        browserScroll_ = 0;
        rememberBrowserPlace();
        openNameMenu(2, {menuFolderRow_.x - 8.0f, menuFolderRow_.bottom() + 4.0f});
    } else if (command == kOpenFolderCommand) {
        presetStore().openPresetFolder(*menuFolder_);
    } else if (command == kRenameFolderCommand && !menuFolder_->empty()) { // its name, all selected, to type over
        renameFolder_ = *menuFolder_;
        openNameMenu(4, {menuFolderRow_.x, menuFolderRow_.bottom() + 4.0f});
        const size_t slash = renameFolder_.rfind('/');
        newName_.set(slash == std::string::npos ? renameFolder_ : renameFolder_.substr(slash + 1));
        newName_.anchor = 0;
    } else if (command == kDeleteFolderCommand && !menuFolder_->empty()) {
        confirmFolder_ = *menuFolder_;
        host_.askConfirm(*this, kConfirmFolderDelete);
    }
    return true;
}

std::optional<bool> PresetBrowser::dialogKey(Key key) {
    if (infoCardOpen_) { // the info card: Escape closes it, Enter goes to the host
        if (key != Key::Escape)
            return false;
        infoCardOpen_ = false;
        host_.resetHover();
        return true;
    }
    if (browserOpen_ && nameMenu_ == 0 && saveMode_ == SaveMode::None) {
        browserKey(key); // its search not clicked into: Enter loads the preset picked, Escape closes it
        return true;
    }
    return std::nullopt;
}

TextField *PresetBrowser::field(Field f) {
    switch (f) {
    case Field::Browser:
        return &browserSearch_;
    case Field::SaveName:
        return &saveName_;
    case Field::SaveAuthor:
        return &saveAuthor_;
    case Field::SaveDescription:
        return &saveDescription_;
    case Field::NewName:
        return &newName_;
    default:
        return nullptr;
    }
}

// The form over the rest has the keyboard: the name form's field, the Save sheet's field picked, or the browser's
// search once clicked.
bool PresetBrowser::fieldFocus(std::optional<Field> &focus) const {
    if (nameMenu_ != 0)
        focus = Field::NewName;
    else if (saveMode_ != SaveMode::None)
        focus = saveFocus_;
    else if (browserOpen_)
        focus = browserSearching_ ? std::optional(Field::Browser) : std::nullopt;
    else
        return false;
    return true;
}

// After what is typed in its search changes: the browser's list from its top, the first preset found picked.
void PresetBrowser::fieldChanged(Field f) {
    if (f == Field::SaveAuthor)
        saveAuthorTyped_ = true;
    if (f != Field::Browser)
        return;
    const std::vector<int> rows = browserRows();
    if (!browserSearch_.text.empty())
        browserPick_ = rows.empty() ? -1 : rows.front();
    browserScroll_ = 0;
}

void PresetBrowser::fieldKey(Field f, Key key) {
    switch (f) {
    case Field::Browser:
        browserKey(key);
        break;
    case Field::SaveName:
    case Field::SaveAuthor:
    case Field::SaveDescription:
        saveKey(key);
        break;
    case Field::NewName:
        nameMenuKey(key);
        break;
    default:
        break;
    }
}

// The preset picked from the library that replaces the patch (save it first); a folder and the presets that go with it;
// a category and the presets in it, which keep it.
Panel::ConfirmWords PresetBrowser::confirmWords(int kind) const {
    if (kind == kConfirmFolderDelete) {
        const int n = presetsInFolder(confirmFolder_);
        return {trf(Str::ConfirmFolderDeleteTitle, {confirmFolder_.substr(confirmFolder_.rfind('/') + 1)}),
                n == 0   ? std::string(tr(Str::ConfirmFolderDeleteEmpty))
                : n == 1 ? std::string(tr(Str::ConfirmFolderDeleteOne))
                         : trf(Str::ConfirmFolderDelete, {std::to_string(n)}),
                tr(Str::ButtonDeleteFolder), false};
    }
    if (kind == kConfirmCategoryRemove) {
        const int n = presetsInCategory(confirmCategory_);
        return {trf(Str::ConfirmCategoryRemoveTitle, {confirmCategory_}),
                n == 0   ? std::string(tr(Str::ConfirmCategoryRemoveUnused))
                : n == 1 ? std::string(tr(Str::ConfirmCategoryRemoveOne))
                         : trf(Str::ConfirmCategoryRemove, {std::to_string(n)}),
                tr(Str::ButtonRemoveCategory), false};
    }
    const std::vector<PresetEntry> &presets = presetStore().presets();
    return {tr(Str::ConfirmTitle),
            pendingPreset_ >= 0 && pendingPreset_ < static_cast<int>(presets.size())
                ? trf(Str::ConfirmLoadPreset, {presets[pendingPreset_].name})
                : std::string(),
            tr(Str::ButtonLoad), true};
}

void PresetBrowser::confirmed(int kind, int button) {
    if (kind == kConfirmFolderDelete) { // into the Recycle Bin
        if (button == 1) {
            const std::optional<PresetEntry> picked = entryAt(presetStore().presets(), browserPick_);
            if (presetStore().deletePresetFolder(confirmFolder_)) {
                // The preset picked found again in the library scanned again (none, if it was in the folder).
                browserPick_ = indexOfEntry(presetStore().presets(), picked);
                // The browser showing it, or one in it, shows the folder it was in.
                if (browserScope_ == BrowserScope::Folder &&
                    (browserFolder_ == confirmFolder_ || browserFolder_.starts_with(confirmFolder_ + "/"))) {
                    const size_t slash = confirmFolder_.rfind('/');
                    browserFolder_ = slash == std::string::npos ? std::string() : confirmFolder_.substr(0, slash);
                    browserScroll_ = 0;
                    rememberBrowserPlace();
                }
                browserSideScroll_ = std::min(browserSideScroll_, browserSideEnd());
            } else {
                host_.flash(tr(Str::MsgFolderNotDeleted));
            }
        }
        confirmFolder_.clear();
    } else if (kind == kConfirmCategoryRemove) { // out of the list; the presets in it keep it, listed in grey
        if (button == 1) {
            presetStore().removeUserCategory(confirmCategory_);
            browserSideScroll_ = std::min(browserSideScroll_, browserSideEnd());
        }
        confirmCategory_.clear();
    } else if (kind == kConfirmLoad && button == 1) { // the preset from the library (closing the browser, but for a click)
        if (presetStore().loadPresetAt(pendingPreset_)) {
            if (pendingClose_)
                browserOpen_ = false;
        }
        else
            host_.flash(tr(Str::MsgPresetUnreadable));
    }
    pendingPreset_ = -1;
}

} // namespace fmvibe::gui
