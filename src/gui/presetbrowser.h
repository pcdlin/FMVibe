#pragma once

// The editor's presets panel: the preset bar's info (the preset's name, category and author, the (i) beside them) and
// what opens from it over the editor: the info card, the preset browser with its sidebar, list and search, the Save
// dialog (which also edits a preset's info) and its New Category, New Folder and Rename forms, with the folders'
// context menus. What the editor shows of the preset library and of a preset's info (see PresetInfo), through the
// controller's PresetStore. The preset bar's other buttons (Init, Load, Save, Save As and the arrows) are the View's,
// which loads a preset of the library through openPreset() and opens the Save dialog through openSave().

#include "controller.h"
#include "panel.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class PresetBrowser : public Panel {
public:
    explicit PresetBrowser(PanelHost &host);

    // The confirm dialog's questions this panel asks (see View::confirm_, whose other kinds are the View's): loading a
    // preset over a changed patch (PresetAction::Load's number), removing one of the user's own categories, and
    // deleting a preset folder.
    static constexpr int kConfirmLoad = 1;
    static constexpr int kConfirmCategoryRemove = 8; // asking before one of the user's own categories goes
    static constexpr int kConfirmFolderDelete = 9;   // asking before a preset folder goes to the Recycle Bin
    // The Save dialog's folder tree's context menu (and the browser's): a new folder in the folder the menu was opened
    // on, that folder in Explorer, renaming it and deleting it (see View::menuCommand(), which numbers the others).
    static constexpr int kNewFolderCommand = 44;
    static constexpr int kOpenFolderCommand = 45;
    static constexpr int kRenameFolderCommand = 46;
    static constexpr int kDeleteFolderCommand = 47;

    // What the preset browser, the info card and the Save dialog show: whether each is open, the presets the browser
    // lists (in its order) and the one picked.
    bool browserShown() const { return browserOpen_; }
    bool infoCardShown() const { return infoCardOpen_; }
    bool saveShown() const { return saveMode_ != SaveMode::None; }
    bool nameMenuShown() const { return nameMenu_ != 0; }
    std::vector<int> browserListed() const { return browserRows(); }
    int browserPicked() const { return browserPick_; }
    float browserSideScrolled() const { return browserSideScroll_; } // how far its sidebar is scrolled, in px
    std::vector<int> browserCategoryCounted() const { return browserCategoryCounts(); } // by browserCategoryList()'s rows
    // Loads a preset of the library, asking first if the patch changed since the last.
    void openPreset(int index, bool close = true);
    // Opens the Save dialog: to save the patch as a preset (Save As), or to edit the info of the library's preset
    // `index`.
    void openSave(int index = -1);

    bool modal() const override;
    void widgets(std::vector<Widget> &list) const override;
    void paint(Canvas &c) override;
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    void pressing(const Widget *hit) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doubleClick(const Widget &w, Point p, Modifiers mods) override;
    void drag(const DragState &d) override;
    bool wheel(Point p, float steps) override;
    std::optional<std::pair<int, float>> panAt(Point p) const override;
    std::optional<float> pan(int part, float first, float across, float down) override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;
    bool dialog() const override { return infoCardOpen_ || browserOpen_; }
    std::optional<bool> dialogKey(Key key) override;
    TextField *field(Field f) override;
    bool fieldFocus(std::optional<Field> &focus) const override;
    void fieldChanged(Field f) override;
    void fieldKey(Field f, Key key) override;
    ConfirmWords confirmWords(int kind) const override;
    void confirmed(int kind, int button) override;

private:
    using Kind = WidgetKind;

    // What the preset browser shows: every preset, the favorites, the recent ones, the factory presets, or a folder's.
    enum class BrowserScope { All, Favorites, Recent, Factory, Folder };
    // A row of the browser's folder tree: the factory presets', the preset folder's (at its top) or a subfolder's.
    struct TreeRow {
        BrowserScope scope; // Factory, or Folder
        std::string folder; // with Folder, its parts joined by '/' ("" the preset folder)
        int depth = 0;
        bool hasKids = false;
        bool open = false;
    };
    // A row of the category lists: its name as presets store it ("" for None), as shown, and its color (-1 grey).
    struct CategoryRow {
        std::string name;
        std::string label;
        int color = -1;
        bool divider = false; // a line before it: the user's own come after the built-in ones, None after them
    };

    Controller &controller() const { return host_.controller(); }
    // The preset library and the patch as a file (see Controller::presetStore()).
    PresetStore &presetStore() const { return host_.controller().presetStore(); }

    // The preset browser: while it is open it takes every click, and the confirm dialog goes over it.
    std::vector<Widget> browserWidgets() const;
    void paintBrowser(Canvas &c);
    // Its rows: each preset's index and name, with a header (-1 and the subfolder) before each subfolder's. While
    // something is typed in its search, only the presets whose subfolder and name hold every word of it.
    std::vector<int> browserRows(bool byCategory = true) const; // false: whatever the categories filter by
    // The browser takes the keyboard while it is open: what is typed goes into its search, Up and Down pick a
    // preset from the list, Enter loads it, and Escape empties the search, or then closes the browser.
    void browserKey(Key key);
    // Scrolls the list so the preset's row shows: about in the middle, or just inside the list.
    void revealBrowserRow(int preset, bool middle);

    // The preset bar's info, the info card, the browser and the Save dialog.
    void paintPresetBar(Canvas &c, const Widget &w, bool hovered);
    std::vector<Widget> infoCardWidgets() const;
    void paintInfoCard(Canvas &c);
    // Opens the browser on the preset loaded, in its folder; or on `preset`.
    void openBrowser(int preset = -1);
    void rememberBrowserPlace(); // the sidebar's entry picked, kept in the user's settings for the next browser
    Rect browserListArea() const;
    int browserVisible() const;
    std::vector<TreeRow> browserTree() const;
    std::vector<CategoryRow> browserCategoryList() const;
    static bool ownCategory(const CategoryRow &row);
    int presetsInCategory(const std::string &name) const; // of all of the library's, whatever the case
    int presetsInFolder(const std::string &folder) const;
    std::vector<int> browserCategoryCounts() const;
    // The category rows the Save dialog offers: the built-in ones and the user's own.
    std::vector<CategoryRow> saveCategoryList() const;
    // How the browser's sidebar lays out its rows, scrolled: each its kind, its row and where it is.
    struct SideItem {
        Kind kind;
        int a = -1;
        Rect rect;
    };
    std::vector<SideItem> browserSide() const;
    float browserSideEnd() const;
    Rect browserSideArea() const;  // the sidebar, and its scrollbar's track and thumb
    Rect browserSideTrack() const;
    Rect browserSideThumb() const; // how far it scrolls
    // Whether a preset's category is one the browser filters by (or no filter is on).
    bool categoryShown(const PresetEntry &entry) const;
    std::vector<Widget> saveWidgets() const;
    void paintSave(Canvas &c);
    void saveKey(Key key);
    void runSave();
    struct SaveLayout {
        Rect panel, name, author, description, folders, warning, toFile, cancel, save, close;
        std::vector<Rect> chips; // saveCategoryList(), then None, then + New
    };
    SaveLayout saveLayout() const;
    // The folders the Save dialog lists: the preset folder ("") first, then presetFolders().
    std::vector<std::string> saveFolders() const;
    // The New Category or New Folder form.
    void openNameMenu(int mode, Point at);
    std::vector<Widget> nameMenuWidgets() const;
    void paintNameMenu(Canvas &c);
    Rect nameMenuPanel() const;
    void nameMenuKey(Key key);
    void runNameMenu();
    // A category's color (see categoryColor()): a built-in one's, a user's own, or -1 for one this computer doesn't
    // know.
    int presetCategoryColor(const std::string &category) const;
    // A preset's pill: its category's in capitals, on its color; nothing for none. Its width.
    float paintCategoryPill(Canvas &c, Point at, float height, const std::string &category, float textSize);

    PanelHost &host_;
    std::optional<std::string> menuFolder_;  // the Save dialog's folder the open context menu was opened on
    Rect menuFolderRow_;                     // and where its row is
    std::string confirmFolder_;              // the folder kConfirmFolderDelete asks about, by its parts joined by '/'
    std::string renameFolder_;               // the folder the name form renames (nameMenu_ 4)
    std::string confirmCategory_;            // the category kConfirmCategoryRemove asks about, by its name
    int pendingPreset_ = -1;                 // the library preset a confirmed Load loads, or -1
    bool pendingClose_ = true;               // and whether loading it closes the browser
    bool browserOpen_ = false;
    BrowserScope browserScope_ = BrowserScope::Factory;
    std::string browserFolder_;                 // with Folder: which ("" the preset folder)
    std::set<std::string> browserOpenFolders_;  // opened in the tree ("" the preset folder)
    std::set<std::string> browserCategories_;   // filtered by, as stored ("" for None), in lower case
    bool browserSubfolders_ = true;             // a folder lists its subfolders' presets too
    float browserSideScroll_ = 0.0f;            // how far the sidebar is scrolled, in px
    float sideDragFrom_ = 0.0f;                 // its thumb dragged: where it was scrolled to as it was grabbed
    mutable std::vector<std::pair<int, Rect>> browserCrumbRects_; // where the folder's parts were painted
    mutable std::vector<std::pair<int, Rect>> browserChipRects_;  // and the category filters
    bool infoCardOpen_ = false;
    enum class SaveMode { None, Save, Info };
    SaveMode saveMode_ = SaveMode::None;
    int saveIndex_ = -1;          // editing info: the library's preset
    std::string saveFolder_;      // saving: where
    std::string saveCategory_;    // the category picked, as stored ("" for None)
    float saveFolderScroll_ = 0.0f;
    TextField saveName_, saveAuthor_, saveDescription_, newName_;
    Field saveFocus_ = Field::SaveName;
    int nameMenu_ = 0; // the form open: 1 New Category, 2 New Folder from the browser, 3 from the Save dialog, 4 Rename
    Point nameMenuAt_;
    int nameColor_ = kBuiltInCategories;
    int browserScroll_ = 0;                  // rows scrolled past, before clamping
    TextField browserSearch_;                // what is typed in its search
    bool browserSearching_ = false;          // and whether it has the keyboard: from a click on it to one elsewhere
    int browserPick_ = -1;                   // the preset Up and Down picked, which Enter loads, or -1
};

} // namespace fmvibe::gui
