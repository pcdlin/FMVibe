#pragma once

// The table browser (see tablebrowser.cpp): over the routing panel, as the partial editor is, while an operator's TABLE
// or SAMPLE picker has it open, it lists the tables (or samples) the operator can take: along its top its title, its
// search, Import file... and Done; under them the sources' column at its left (for a table Factory, then In this patch,
// then the folders of the library of what it picks, each with its menu, and + Add folder...), the list of the source shown in the middle (a folder
// being scanned shows how far the scan is instead; a search lists what it finds in every source, each source's under
// its name) with its scrollbar, and the preview of the row under the mouse at its right. A click on a row loads it into
// the operator.
//
// It also holds what the inspector's pages draw of a table or a sample (the pickers' glyphs, and a table's frames as a
// stack), which its preview shares.

#include "../opdata.h"
#include "../tables/library.h"
#include "../warp.h"
#include "panel.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fmvibe::gui {

class ShapeEditor;

// A table's glyph (three waves, one behind another) and a sample's (a burst that dies away), in `r`.
void tableGlyph(Canvas &c, Rect r, Color color);
void sampleGlyph(Canvas &c, Rect r, Color color);
// A chevron pointing left (-1), right (1) or down (0), about `at`.
void chevron(Canvas &c, Point at, int dir, Color color);
// A table's frames as stored aren't scaled yet (a factory table's least of all): its frames' peak, to draw them at the
// gain the synth plays them at, the loudest at full scale. Found once a table, in the curve cache.
float tablePeak(const OpData &table);
// A table's frames as a stack seen from a little above and to the side, the front one first, up to `most` of them,
// the frame at `position` (0..1; none below 0) lit, read through `warp` at `amount`.
void paintTableStack(Canvas &c, Rect r, const OpData &table, int most, float position, Color color,
                     OpWarp warp = OpWarp::Off, double amount = 0.0);

class TableBrowser : public Panel {
public:
    // Opening it closes the full-size shape editors (`shapes`), whose place it takes.
    TableBrowser(PanelHost &host, ShapeEditor &shapes);

    // The operator whose table or sample it picks, or -1 (also while a full-size shape editor is open over it), and
    // what it picks.
    int browsing() const;
    DataKind kind() const { return kind_; }
    // Opens it for operator `op`'s table or sample, on the source what it plays came from; a second click on the
    // picker that opened it closes it.
    void open(int op, DataKind kind);
    void close();
    // Shows the library's source `source` (a folder just added).
    void showSource(int source);

    void forgetScoped() override;
    bool step() override; // takes in what the library's scans found
    void widgets(std::vector<Widget> &list) const override;
    void paintBack(Canvas &c) override;
    bool paintWidget(Canvas &c, const Widget &w, bool hovered) override;
    std::string tooltip(const Widget &w) const override;
    std::optional<CursorKind> cursor(const Widget &w) const override;
    void pressing(const Widget *hit) override;
    bool press(const Widget &w, Point p, Modifiers mods) override;
    bool doublePress(const Widget &w) const override;
    void drag(const DragState &d) override;
    void release() override;
    bool dragsOnTouch(const Widget &w) const override;
    bool wheel(Point p, float steps) override;
    bool contextMenu(const std::optional<Widget> &hit, Point p, std::vector<MenuItem> &items) override;
    bool menuCommand(int command) override;
    TextField *field(Field f) override;
    bool fieldFocus(std::optional<Field> &focus) const override;
    void fieldChanged(Field f) override;
    void fieldKey(Field f, Key key) override;

private:
    using Kind = WidgetKind;

    // A row of the list: a source's name over what a search found in it, or a table (or sample).
    struct Row {
        bool header = false;
        int source = -2;  // -2 Factory, -1 In this patch, else the library's
        int entry = -1;   // the library source's entry
        uint32_t key = 0; // a factory table's or the patch's
        std::string name, path, tag; // its file's name in a library source
        int frames = 0;
        bool sample = false, guessed = false;
        ReadError error = ReadError::None;
    };

    // The list, shared rather than copied: a folder's can run to thousands of rows, and each row painted asks for it.
    using RowList = std::shared_ptr<const std::vector<Row>>;

    Controller &controller() const { return host_.controller(); }
    double value(int index) const;
    TableLibrary *library() const;
    RowList rows() const;              // kept while the host's scope goes on
    std::vector<Row> makeRows() const;
    void pick(const Row &row);         // loads it into the operator
    float end() const;                 // how many rows the list scrolls past at most
    Rect thumb() const;                // the list's scrollbar's thumb, at least 24 px tall however long the list
    // The data a row holds (read from its file for a library's, kept for a while): null where it can't be read.
    std::shared_ptr<const OpData> data(const Row &row) const;
    // The keys the search doesn't take itself: Enter loads the row picked and lets the keyboard go, Escape empties it
    // and then lets it go, Up and Down pick a row.
    void searchKey(Key key);
    // A folder's new name, typed into its row: kept (`commit`), or let go of; emptied, it keeps the old one.
    void endRename(bool commit);
    // The rect of the source `source` in the sources' column, which only a table's browser starts with Factory in.
    Rect sourceRow(int source) const;

    PanelHost &host_;
    ShapeEditor &shapes_;
    int op_ = -1; // the operator it picks for, or -1 while it is shut
    DataKind kind_ = DataKind::Table;
    int source_ = -2;
    float scroll_ = 0.0f;
    int pick_ = -1; // the row picked with the keys
    // Whether its search has the keyboard: once clicked, until a click elsewhere, Enter or Escape lets it go; till then
    // the keys play notes, as the host has them.
    bool searching_ = false;
    TextField search_;
    int renaming_ = -1; // the library's source whose name is typed into its row, or -1
    TextField rename_;
    int menuSource_ = -1; // the library's source whose menu is open
    float scrollFrom_ = 0.0f; // where the list was as its scrollbar's thumb was grabbed
    bool scrolling_ = false;
    // The data it read from the library's files: the last few, by source and path.
    mutable std::vector<std::pair<std::string, std::shared_ptr<const OpData>>> read_;
    ScopeCache<RowList> rows_;
};

} // namespace fmvibe::gui
