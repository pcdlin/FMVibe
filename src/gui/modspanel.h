#pragma once

// The MODS view (see modspanel.cpp): every modulation as a row of a table over the routing panel and the inspector,
// the sources in the order of their modulations, then the empty modulations (UiLayout::emptyMods; one from a source
// that has rows goes after them instead) and a row that adds one; its filter; and the cascading menus its rows'
// sources, targets and auxes open. It shows in the panels' row while the MODS tab is picked and no full-size editor is
// over the row; the View lists its widgets there, and its open menu with the others' popups. A row's slot widgets (its
// bypass dot, depth bar and number, its source's polarity switch, its aux depth and the aux's x, its remove) are the
// View's, as they are wherever a slot shows (a pill's list, the source cards), and so is editing its Amount and curve
// (dragged, reset, their context menu), as the View edits every slot's params; the panel lists and paints them. The
// rest (the rows, the cells that open menus, + Aux and the row that adds a modulation, the scrollbar, the filter and the
// menus) is the panel's.

#include "fxgraphmodel.h"
#include "modulationmodel.h"
#include "panel.h"
#include "routingmodel.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class ModsPanel : public Panel {
public:
    explicit ModsPanel(PanelHost &host);

    // The MODS view shows, with no shape or partial editor over it.
    bool wide() const;
    // The MODS view, as it shows, its rows scrolled to the source's first; with none there yet, a new empty
    // modulation from it (see a card's SourceMods button, which shows the view first).
    void showSource(int source);

    void forgetScoped() override;
    void widgets(std::vector<Widget> &list) const override;
    void popups(std::vector<Widget> &list) const override; // the open cascading menu, over everything
    void paintBack(Canvas &c) override;
    void paint(Canvas &c) override; // the open cascading menu
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    bool step() override;
    void hoverChanged(const std::optional<Widget> &hit) override;
    void closePopups() override;
    void pressing(const Widget *hit) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    void drag(const DragState &d) override;
    bool dragsOnTouch(const Widget &w) const override;
    bool takesSource(const Widget &w, int source) const override;
    void paintSourceRing(Canvas &c, const Widget &w, bool over, Color color) override;
    bool dropSource(const Widget &w, int source) override;
    bool wheelBehind(Point p, float steps) override;
    std::optional<std::pair<int, float>> panAt(Point p) const override;
    bool blocksPan(const Widget &w) const override;
    void panBegin(int part) override;
    std::optional<float> pan(int part, float first, float across, float down) override;
    TextField *field(Field f) override;
    bool fieldFocus(std::optional<Field> &focus) const override;
    void fieldChanged(Field f) override;
    void fieldKey(Field f, Key key) override;

private:
    using Kind = WidgetKind;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const { return mods_.value(index); }

    // An empty modulation's row stands in for a slot as emptyModSlot(its place in the list) (see modulationmodel.h).
    // The rows, kept while the host's scope goes on.
    std::vector<Assignment> modsRows() const;
    std::vector<Assignment> modsRowsNow() const; // modsRows() worked out
    // Its rows scroll: how many are scrolled past as shown (gliding), the most that can be, and scrolling them, gliding
    // or at once; the thumb of its bar.
    float modsFirst() const;
    float modsEnd() const;
    void scrollMods(float first, bool glide);
    // Scrolls the rows at once to `slot`'s row of `source` (an empty modulation's too), with as many of its source's
    // rows above it as fit, and says where it is shown.
    Rect revealModsRow(int source, int slot);
    bool glideMods();
    Rect modsThumb() const;
    // Adds an empty modulation from `source` (-1 for none yet), at the end of the list or after its source's rows,
    // scrolls to it and opens the menu of what it lacks; refused past kMaxEmptyMods.
    void addEmptyMod(int source = -1);
    // Picks an empty modulation's source, or its target, as ModulationModel::fillEmptyMod() does; either way the rows
    // scroll to where it is now, among its source's.
    void fillEmptyMod(int row, int source, int target);
    // The cascading menus of a MODS row: its sources by category (LFOs, envelopes, macros, the keyboard's), the same
    // with None for its aux, and every target by category, only the effects in the chain among them. Each entry is a
    // category (with entries of its own), a heading, or a pick: a source (-1 for None) or a target param.
    enum class CascadeKind { Source, Target, Aux };
    struct MenuNode {
        std::string label;
        std::optional<Color> color; // a dot before the label
        bool heading = false;
        bool enabled = true;
        bool checked = false;  // what the row has now
        bool holds = false;    // a category that has what the row has now somewhere in it
        int pick = -2;         // a source or target; -2 for a category or heading
        std::string note;      // after the label, faint: why it can't be picked
        std::string path;      // before the label, dim: the categories a search found it in
        bool newColumn = false; // starts a column of its own level beside the one before (an operator's ENVELOPE)
        std::vector<MenuNode> children;
    };
    struct Cascade {
        CascadeKind kind = CascadeKind::Source;
        int source = -1, slot = -1; // the row
        Point at;                   // the first level's top left
        std::vector<int> open;      // the entry opened at each level so far
        int picked = 0;             // while its search finds targets: the match Up and Down picked, which Enter takes
    };
    // A target menu has a search atop its first level: while something is typed in it, that level lists the targets
    // whose path (categories, then name) holds every word of it, flat, each with its path, as many as
    // kCascadeMatches, then how many more there are.
    bool cascadeSearches() const { return cascade_ && cascade_->kind == CascadeKind::Target; }
    // Where the search goes in the first level, and so where that level's entries start.
    Rect cascadeSearchRect(Rect level) const;
    // The open menu's entries, kept while the host's scope goes on.
    std::shared_ptr<const MenuNode> cascadeTree() const;
    MenuNode cascadeTreeNow() const; // cascadeTree() worked out
    MenuNode cascadeCategories() const; // cascadeTree() before its search narrows it
    // The open menu's levels, from the first: their entries, and where they show.
    std::vector<const MenuNode *> cascadeLevels(const MenuNode &root) const;
    std::vector<Rect> cascadeRects(const MenuNode &root) const;
    // An entry of a level (`depth` of them from the first), in the level's place `at`: down its column, the level's
    // columns side by side, each as wide.
    Rect cascadeItemRect(const MenuNode &level, Rect at, int index, size_t depth) const;
    static int cascadeColumns(const MenuNode &level);
    void openCascade(CascadeKind kind, int source, int slot, Rect from);
    void pickCascade(int level, int index);
    // The keyboard in the target menu's search: Up and Down pick a match, Enter takes it, Escape empties the search,
    // or then closes the menu.
    void cascadeKey(Key key);
    // The MODS view's filter: in its title row, at the right, a magnifier until it is clicked or holds something.
    // While it holds something, only the rows whose source and target names hold every word of it show (and the
    // empty modulations, which have yet to get theirs). Escape empties it, or then lets the keyboard go, as Enter and
    // a click elsewhere do.
    Rect modsFilterRect() const;
    bool modsFilterOpen() const { return modsFiltering_ || !modsFilter_.text.empty(); }
    std::string modsFilterCount() const; // "3 of 13": the modulations it shows, of all
    void modsFilterKey(Key key);
    void paintCascade(Canvas &c);

    PanelHost &host_;
    // The patch's models, each telling the editor why it refused an edit (see PanelHost::flash()).
    ModulationModel &mods_;
    RoutingModel &routing_;
    FxGraphModel &fx_;
    const ParamTable &table_;
    float modsScroll_ = 0.0f;        // rows the MODS view is scrolled to, before clamping
    float modsGlide_ = 0.0f;         // and where it shows, gliding after it
    float modsThumbFrom_ = 0.0f;     // its thumb dragged: where the rows were scrolled to as it was grabbed
    std::optional<Cascade> cascade_; // a row's cascading menu, while it is open
    TextField modsFilter_;           // the filter
    bool modsFiltering_ = false;     // and whether it has the keyboard
    TextField cascadeSearch_;        // the search atop the open target menu
    ScopeCache<std::vector<Assignment>> rows_;
    ScopeCache<std::shared_ptr<const MenuNode>> cascadeTree_;
};

} // namespace fmvibe::gui
