// The table browser (TableBrowser, see tablebrowser.h) over the routing panel, and the tables' and samples' drawings
// the inspector's pages share with it.

#include "tablebrowser.h"

#include "controller.h"
#include "icons.h"
#include "layout.h"
#include "shapeeditor.h"
#include "theme.h"
#include "../tables.h"
#include "../tables/readers.h"
#include "../uistrings.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>

namespace fmvibe::gui {

using namespace detail;

namespace {

// The list's scrollbar, along its right edge, and its thumb's least height (as the editor's other scrollbars have
// one), so a list of thousands of files still has a thumb to grab.
constexpr Rect kTbScrollTrack{kTbList.x + kTbList.w - 5.0f, kTbList.y, 4.0f, kTbList.h};
constexpr float kTbThumbMin = 24.0f;

// A folder's menu's commands, taken only while its own menu is open.
constexpr int kTbRenameCommand = 60000;
constexpr int kTbFrameSizeCommand = 60001; // + 0 Auto, 1 256, 2 512, 3 1024, 4 2048
constexpr int kTbRescanCommand = 60010;
constexpr int kTbShowCommand = 60011;
constexpr int kTbRemoveCommand = 60012;
constexpr int kTbFrameSizes[] = {0, 256, 512, 1024, 2048};

// The longest name a folder takes.
constexpr size_t kMaxFolderName = 64;

} // namespace

// --- The drawings the inspector shares.

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

void sampleGlyph(Canvas &c, Rect r, Color color) {
    const float mid = r.center().y;
    for (int i = 0; i < 7; ++i) {
        const float x = r.x + 1.0f + i * (r.w - 2.0f) / 6.0f;
        const float h = (0.5f * r.h - 1.0f) * (1.0f - i / 8.0f) * (i % 2 == 0 ? 1.0f : 0.6f);
        c.line({x, mid - h}, {x, mid + h}, color, 1.4f);
    }
}

float tablePeak(const OpData &table) {
    const size_t samples = static_cast<size_t>(std::max(table.frames, 0)) * FrameTable::kFrameSize;
    if (samples == 0 || table.pcm.size() < samples)
        return 1.0f;
    const std::vector<double> key = {static_cast<double>(Curve::TableFrame), static_cast<double>(table.key),
                                     static_cast<double>(table.frames), static_cast<double>(table.pcm[0]),
                                     static_cast<double>(table.pcm[samples / 2]), -1.0};
    const float peak = curveCache().get(key, [&] {
        float most = 0.0f;
        for (size_t i = 0; i < samples; ++i)
            most = std::max(most, std::abs(table.pcm[i]));
        return std::vector<Point>{{most, 0.0f}};
    })[0].x;
    return peak > 0.0f ? peak : 1.0f;
}

void paintTableStack(Canvas &c, Rect r, const OpData &table, int most, float position, Color color, OpWarp warp,
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
        // By the table's key and frames (a file previewed, under the key 0, apart from Basic Shapes by its frames'
        // count).
        const std::vector<double> key = {static_cast<double>(Curve::TableFrame), static_cast<double>(table.key),
                                         static_cast<double>(frames), static_cast<double>(table.pcm[0]),
                                         static_cast<double>(table.pcm[table.pcm.size() / 2]), f, r.x, r.y, r.w, r.h,
                                         static_cast<double>(coefs.mode), coefs.a, coefs.b, coefs.c, scale};
        const std::vector<Point> points = curveCache().get(key, [&] {
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
        frameAt(f, t, color.alpha(0.05f + 0.15f * (1.0f - t)), 1.0f, WarpCoefs{});
    }
    if (position >= 0.0f) { // the frame played, lit at its depth
        const double f = std::clamp(position, 0.0f, 1.0f) * (frames - 1);
        frameAt(f, frames > 1 ? static_cast<float>(f / (frames - 1)) : 0.0f, color, 2.0f, warpCoefs(warp, amount));
    }
    c.popClip();
}

// --- The browser.

TableBrowser::TableBrowser(PanelHost &host, ShapeEditor &shapes) : host_(host), shapes_(shapes) {}

double TableBrowser::value(int index) const {
    return controller().value(index);
}

TableLibrary *TableBrowser::library() const {
    return controller().tableLibrary(kind_);
}

int TableBrowser::browsing() const {
    if (op_ < 0 || host_.shapeEditorOpen())
        return -1;
    // Only while the operator still plays what it picks: switched to another source, it shuts.
    const OpWave wave = opWave(value(paramIndex(op_, OpParam::Wave)));
    return wave == (kind_ == DataKind::Table ? OpWave::Wavetable : OpWave::Sample) ? op_ : -1;
}

Rect TableBrowser::sourceRow(int source) const {
    return tbSourceRow(source + (kind_ == DataKind::Table ? 2 : 1));
}

void TableBrowser::open(int op, DataKind kind) {
    if (op_ == op && kind_ == kind) { // a second click closes it
        close();
        return;
    }
    endRename(true);
    if (kind != kind_)
        read_.clear(); // by source number, which the other library's are numbered apart from
    op_ = op;
    kind_ = kind;
    pick_ = -1;
    searching_ = false; // the keys play notes until the search is clicked
    scroll_ = 0.0f;
    shapes_.close(); // its place over the panels
    host_.clearField(Field::Tables);
    // The source the operator's data came from, else Factory for a table and the patch for a sample.
    source_ = kind == DataKind::Table ? -2 : -1;
    const double playing = value(paramIndex(op, kind == DataKind::Table ? OpParam::Table : OpParam::Sample));
    if (const OpData *d = controller().opData(dataKey(playing)); d) {
        if (d->key >= kUserDataBase)
            source_ = -1;
        if (TableLibrary *lib = library(); lib && !d->origin.empty())
            for (size_t s = 0; s < lib->sources().size(); ++s)
                if (d->origin.starts_with(lib->sources()[s].folder.name + "/"))
                    source_ = static_cast<int>(s);
    }
    if (TableLibrary *lib = library())
        lib->poll();
    host_.resetHover();
}

void TableBrowser::close() {
    if (op_ < 0)
        return;
    endRename(true);
    op_ = -1;
    searching_ = false;
    host_.resetHover();
}

void TableBrowser::showSource(int source) {
    if (source >= 0) {
        source_ = source;
        scroll_ = 0.0f;
        pick_ = -1;
        host_.clearField(Field::Tables);
    }
}

void TableBrowser::forgetScoped() {
    rows_.forget();
}

bool TableBrowser::step() {
    TableLibrary *lib = op_ >= 0 ? library() : nullptr;
    if (!lib || !lib->poll())
        return false;
    pick_ = -1; // a scan done may have changed the list under it
    return true;
}

TableBrowser::RowList TableBrowser::rows() const {
    return rows_.get(host_, [&] { return std::make_shared<const std::vector<Row>>(makeRows()); });
}

std::vector<TableBrowser::Row> TableBrowser::makeRows() const {
    std::vector<Row> rows;
    const std::vector<std::string> words = search_.words();
    const bool searching = !words.empty();
    TableLibrary *lib = library();
    const DataStore &store = controller().data();
    auto listSource = [&](int source, const std::string &heading) {
        std::vector<Row> found;
        if (source == -2) {
            for (uint32_t t = 0; t < kFactoryTables; ++t) {
                Row row;
                row.source = -2;
                row.key = t;
                row.name = factoryTableName(static_cast<int>(t));
                row.frames = 64;
                found.push_back(row);
            }
        } else if (source == -1) {
            for (uint32_t key : store.keys()) {
                const OpData *d = key >= kUserDataBase ? store.find(key) : nullptr;
                if (!d || d->kind != kind_)
                    continue;
                Row row;
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
            if (s.scanning && !searching)
                return; // it shows how far the scan is (see paintBack()); a search finds what it listed last
            for (size_t e = 0; e < s.entries.size(); ++e) {
                const LibraryEntry &entry = s.entries[e];
                Row row;
                row.source = source;
                row.entry = static_cast<int>(e);
                row.name = entry.name;
                row.path = entry.path;
                row.frames = entry.info.frames;
                row.sample = entry.info.sample;
                row.guessed = entry.info.guessed;
                row.error = entry.info.error;
                row.tag = entry.info.format.empty() ? "" : entry.info.format + (entry.info.guessed && !entry.info.sample ? "?" : "");
                found.push_back(row);
            }
        }
        bool headed = false;
        for (Row &row : found) {
            if (searching && !TextField::holds(heading + " " + row.name, words))
                continue;
            if (searching && !headed) { // each source's matches under its name
                Row head;
                head.header = true;
                head.source = source;
                head.name = heading;
                rows.push_back(head);
                headed = true;
            }
            rows.push_back(std::move(row));
        }
    };
    if (!searching) {
        listSource(source_, "");
        return rows;
    }
    if (kind_ == DataKind::Table)
        listSource(-2, tr(Str::TablesFactory));
    listSource(-1, tr(Str::TablesInPatch));
    for (size_t s = 0; lib && s < lib->sources().size(); ++s)
        listSource(static_cast<int>(s), lib->sources()[s].folder.name);
    return rows;
}

float TableBrowser::end() const {
    return std::max(static_cast<float>(rows()->size()) - kTbList.h / kTbRowH, 0.0f);
}

std::shared_ptr<const OpData> TableBrowser::data(const Row &row) const {
    if (row.header)
        return nullptr;
    if (row.source < 0)
        return controller().data().get(row.key);
    const std::string id = std::to_string(row.source) + "|" + row.path;
    for (const auto &[have, d] : read_)
        if (have == id)
            return d;
    TableLibrary *lib = library();
    std::vector<uint8_t> bytes;
    if (!lib || row.error != ReadError::None || !lib->readEntry(row.source, row.entry, bytes))
        return nullptr;
    const LibrarySource &s = lib->sources()[row.source];
    const TableFile file = readTableFile(bytes.data(), bytes.size(), s.entries[row.entry].path,
                                         row.sample ? ReadAs::Sample : ReadAs::Table, s.folder.frameSize);
    std::shared_ptr<OpData> made;
    if (file.error == ReadError::None) {
        made = std::make_shared<OpData>();
        made->kind = file.sample ? DataKind::Sample : DataKind::Table;
        made->name = row.name;
        made->format = file.format;
        made->frames = file.frames;
        made->rate = file.rate;
        made->pcm = file.data;
        if (file.sample)
            buildPeaks(*made);
    }
    read_.insert(read_.begin(), {id, made});
    if (read_.size() > 48)
        read_.pop_back();
    return made;
}

void TableBrowser::widgets(std::vector<Widget> &list) const {
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
    if (!search_.text.empty())
        add(Kind::SearchClear, fieldClear(kTbSearch), static_cast<int>(Field::Tables));
    // The sources: for a table Factory, then the patch's own, then the library's folders, then + Add folder... after
    // them. A folder being renamed is its name's field.
    if (kind_ == DataKind::Table)
        add(Kind::TbSource, sourceRow(-2), -2);
    add(Kind::TbSource, sourceRow(-1), -1);
    int row = kind_ == DataKind::Table ? 2 : 1;
    if (TableLibrary *lib = library()) {
        for (size_t s = 0; s < lib->sources().size() && tbSourceRow(row + 1).bottom() <= kTbSources.bottom(); ++s) {
            if (renaming_ == static_cast<int>(s))
                add(Kind::SearchField, tbSourceRow(row), static_cast<int>(Field::TableRename));
            else
                add(Kind::TbSource, tbSourceRow(row), static_cast<int>(s));
            ++row;
        }
        add(Kind::TbAddFolder, tbSourceRow(row));
    }
    // The list, cut off at its ends as it scrolls.
    add(Kind::TbScroll, kTbList);
    const RowList held = rows();
    const std::vector<Row> &all = *held;
    const float first = std::clamp(scroll_, 0.0f, end());
    for (int r = static_cast<int>(first); r < static_cast<int>(all.size()); ++r) {
        const float at = static_cast<float>(r) - first;
        if (at * kTbRowH > kTbList.h)
            break;
        add(all[r].header ? Kind::TbGroup : Kind::TbRow, tbListRow(at), r);
        list.back().clip = kTbList;
    }
    if (end() > 0.0f) // over the rows' right ends, a little wider than it is drawn
        add(Kind::TbScrollBar, kTbScrollTrack.inset(-3.0f));
}

Rect TableBrowser::thumb() const {
    const float last = end(), shown = kTbList.h / kTbRowH;
    const float h = std::clamp(kTbScrollTrack.h * shown / (shown + last), std::min(kTbThumbMin, kTbScrollTrack.h),
                               kTbScrollTrack.h);
    const float at = last > 0.0f ? std::clamp(scroll_, 0.0f, last) / last : 0.0f;
    return {kTbScrollTrack.x, kTbScrollTrack.y + (kTbScrollTrack.h - h) * at, kTbScrollTrack.w, h};
}

// --- Painting.

void TableBrowser::paintBack(Canvas &c) {
    const Rect panel = kTableBrowser.inset(-6.0f);
    c.fillRect(panel, kPanel, kRadiusCard);
    const bool samples = kind_ == DataKind::Sample;
    c.text({kTableBrowser.x, kTableBrowser.y + 4.0f, 160.0f, 26.0f}, tr(samples ? Str::SamplesTitle : Str::TablesTitle),
           kTextBody, kText, Align::Left, true);
    host_.paintField(c, Field::Tables, kTbSearch, fieldTyped(kTbSearch), tr(Str::TablesSearchHint));
    if (renaming_ >= 0) { // a folder's name being typed, in its row
        const Rect box = sourceRow(renaming_);
        host_.paintField(c, Field::TableRename, box, {box.x + 8.0f, box.y, box.w - 16.0f, box.h}, "", {}, false);
    }
    c.line({kTbSources.right() + 6.0f, kTbTop}, {kTbSources.right() + 6.0f, kTbBottom}, kBorder, 1.0f);
    c.line({kTbList.right() + 6.0f, kTbTop}, {kTbList.right() + 6.0f, kTbBottom}, kBorder, 1.0f);
    // A list with nothing in it says so; a folder being scanned, how far the scan is, along a bar under the words.
    const RowList held = rows();
    const std::vector<Row> &all = *held;
    TableLibrary *lib = library();
    if (all.empty()) {
        std::string say = tr(samples ? Str::SamplesEmpty : Str::TablesEmpty);
        const LibrarySource *scanning = nullptr;
        if (!search_.text.empty()) {
            say = trf(Str::TablesNoMatch, {search_.text});
        } else if (lib && source_ >= 0 && source_ < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[source_];
            if (s.missing) {
                say = trf(Str::TablesMissing, {s.folder.path});
            } else if (s.scanning) {
                say = tr(Str::TablesScanning);
                scanning = &s;
            }
        }
        c.text({kTbList.x, kTbList.y + 20.0f, kTbList.w, 20.0f}, say, kTextBody, kDim);
        if (scanning) {
            const Rect bar = tbScanBar();
            c.fillRect(bar, kTrack, kRadiusMark);
            if (scanning->total > 0) {
                const float share = std::clamp(static_cast<float>(scanning->scanned) / scanning->total, 0.0f, 1.0f);
                if (share > 0.0f)
                    c.fillRect({bar.x, bar.y, std::max(bar.w * share, bar.h), bar.h}, kAccent, kRadiusMark);
                c.text({kTbList.x, bar.bottom() + 6.0f, kTbList.w, 16.0f},
                       trf(Str::TablesScanCount, {std::to_string(scanning->scanned), std::to_string(scanning->total)}),
                       kTextSmall, kFaint);
            }
        }
    }
    // The preview: the row under the mouse, else the one picked with the keys, else what the operator plays.
    std::optional<Row> shown;
    const std::optional<Widget> &hover = host_.hovered();
    if (hover && hover->kind == Kind::TbRow && !host_.dragging() && hover->a >= 0 &&
        hover->a < static_cast<int>(all.size()))
        shown = all[hover->a];
    else if (pick_ >= 0 && pick_ < static_cast<int>(all.size()))
        shown = all[pick_];
    const Rect view = kTbPreviewView;
    c.fillRect(view, kDisplayColor, kRadiusCard);
    std::shared_ptr<const OpData> d;
    if (shown)
        d = data(*shown);
    else if (const int op = browsing(); op >= 0) {
        // What the operator plays: a sample's key of 0 is no sample, where a table's is the first factory table.
        const uint32_t key = dataKey(value(paramIndex(op, samples ? OpParam::Sample : OpParam::Table)));
        if (!samples || key != 0)
            d = controller().data().get(key);
    }
    if (d && d->kind == DataKind::Table) {
        paintTableStack(c, view.inset(4.0f), *d, 24, 0.0f, kAccent); // its first frame lit, as if played
    } else if (d && d->kind == DataKind::Sample && !d->peakMin.empty()) {
        const Rect a = view.inset(8.0f);
        std::vector<Point> top, bottom;
        for (int i = 0; i < 128; ++i) {
            // The lowest and highest of the peaks this column of the drawing covers.
            const int from = i * OpData::kPeakColumns / 128;
            const int to = std::max(from + 1, (i + 1) * OpData::kPeakColumns / 128);
            float low = 0.0f, high = 0.0f;
            for (int k = from; k < to && k < OpData::kPeakColumns; ++k) {
                low = std::min(low, d->peakMin[k]);
                high = std::max(high, d->peakMax[k]);
            }
            const float px = a.x + (i + 0.5f) * a.w / 128.0f;
            top.push_back({px, a.center().y - high * a.h * 0.48f});
            bottom.push_back({px, a.center().y - low * a.h * 0.48f});
        }
        top.insert(top.end(), bottom.rbegin(), bottom.rend());
        c.fillPolygon(top.data(), top.size(), kAccent.alpha(0.55f));
    } else {
        c.text(view, tr(samples ? Str::SamplesNoPreview : Str::TablesNoPreview), kTextLabel, kFaint);
    }
    // What it is: its path, its format, its frame size (read or guessed) and frames.
    if (shown || d) {
        std::vector<std::string> lines;
        if (shown && shown->source >= 0 && lib && shown->source < static_cast<int>(lib->sources().size())) {
            const LibrarySource &s = lib->sources()[shown->source];
            lines.push_back(s.folder.name + " \xE2\x80\xBA " + shown->name);
            if (shown->error != ReadError::None)
                lines.push_back(tr(readErrorText(shown->error)));
            const LibraryEntry &e = s.entries[shown->entry];
            if (!e.info.format.empty())
                lines.push_back(trf(Str::TablesFormat, {e.info.format}));
            if (!samples && e.info.frameSize > 0)
                lines.push_back(trf(e.info.guessed ? Str::TablesFrameSizeGuessed : Str::TablesFrameSize,
                                    {std::to_string(e.info.frameSize)}));
        } else if (d) {
            lines.push_back(d->origin.empty() ? d->name : d->origin);
            if (!d->format.empty())
                lines.push_back(trf(Str::TablesFormat, {d->format}));
        }
        if (d && d->kind == DataKind::Table)
            lines.push_back(d->frames == 1 ? std::string(tr(Str::TablesOneFrame))
                                           : trf(Str::TablesFrames, {std::to_string(d->frames)}));
        float y = view.bottom() + 8.0f;
        for (const std::string &line : lines) {
            c.pushClip({kTbPreview.x, y, kTbPreview.w, 16.0f});
            c.text({kTbPreview.x, y, kTbPreview.w, 16.0f}, line, kTextLabel, kDim, Align::Left);
            c.popClip();
            y += 17.0f;
        }
    }
}

bool TableBrowser::paintWidget(Canvas &c, const Widget &w, bool hovered) {
    const bool active = host_.isActive(w);
    switch (w.kind) {
    case Kind::TbSource: {
        TableLibrary *lib = library();
        const bool selected = source_ == w.a && search_.text.empty();
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
            for (uint32_t key : controller().data().keys()) {
                const OpData *const d = key >= kUserDataBase ? controller().data().find(key) : nullptr;
                n += d && d->kind == kind_ ? 1 : 0;
            }
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
    case Kind::TbGroup: { // a source's name over what a search found in it
        const RowList held = rows();
        if (w.a >= 0 && w.a < static_cast<int>(held->size()))
            c.text({w.rect.x + 8.0f, w.rect.y, w.rect.w - 8.0f, w.rect.h}, (*held)[w.a].name, kTextLabel, kFaint,
                   Align::Left, true);
        return true;
    }
    case Kind::TbRow: {
        const RowList held = rows();
        const std::vector<Row> &all = *held;
        if (w.a < 0 || w.a >= static_cast<int>(all.size()))
            return true;
        const Row &row = all[w.a];
        const int op = browsing();
        TableLibrary *lib = library();
        // The one the operator plays, the one picked with the keys, and the one under the mouse.
        bool current = false;
        if (op >= 0) {
            const int p = paramIndex(op, kind_ == DataKind::Table ? OpParam::Table : OpParam::Sample);
            if (row.source < 0)
                current = dataKey(value(p)) == row.key;
            else if (const OpData *d = controller().opData(dataKey(value(p)));
                     d && !d->origin.empty() && lib && row.source < static_cast<int>(lib->sources().size()))
                current = d->origin == lib->sources()[row.source].folder.name + "/" +
                                           lib->sources()[row.source].entries[row.entry].path;
        }
        const bool unreadable = row.error != ReadError::None;
        if (current || pick_ == w.a || hovered)
            c.fillRect(w.rect, current ? kAccent.alpha(0.2f) : pick_ == w.a ? kAccent.alpha(0.1f) : kTrack,
                       kRadiusControl);
        const Color ink = unreadable ? kFaint : current ? kText : kDim;
        // A thumbnail: the last, middle and first frames over one another, the first bold, or a sample's glyph.
        const Rect thumbnail{w.rect.x + 6.0f, w.rect.y + 3.0f, 40.0f, w.rect.h - 6.0f};
        c.fillRect(thumbnail, kDisplayColor, kRadiusMark);
        std::shared_ptr<const OpData> d;
        if (row.source < 0)
            d = controller().data().get(row.key);
        else
            for (const auto &[id, read] : read_)
                if (id == std::to_string(row.source) + "|" + row.path)
                    d = read;
        if (d && d->kind == DataKind::Table && d->frames > 0) {
            const float height = thumbnail.h * 0.4f / tablePeak(*d);
            for (int k = 2; k >= 0; --k) {
                const int f = k * (d->frames - 1) / 2;
                const float *x = &d->pcm[static_cast<size_t>(f) * FrameTable::kFrameSize];
                Point p[33];
                for (int i = 0; i <= 32; ++i)
                    p[i] = {thumbnail.x + 2.0f + i * (thumbnail.w - 4.0f) / 32.0f,
                            thumbnail.center().y -
                                x[i * (FrameTable::kFrameSize / 32) % FrameTable::kFrameSize] * height};
                c.polyline(p, 33, (unreadable ? kFaint : kAccent).alpha(k == 0 ? 1.0f : 0.2f), k == 0 ? 1.6f : 1.0f);
            }
        } else if (row.sample || (d && d->kind == DataKind::Sample)) {
            sampleGlyph(c, thumbnail.inset(4.0f), unreadable ? kFaint : kAccent);
        } else {
            tableGlyph(c, thumbnail.inset(3.0f), unreadable ? kFaint : kDim);
        }
        const float tagW = row.tag.empty() ? 0.0f : 36.0f;
        const Rect name{thumbnail.right() + 8.0f, w.rect.y, w.rect.w - 46.0f - 8.0f - 72.0f - tagW, w.rect.h};
        c.pushClip(name);
        c.text(name, row.name, kTextBody, ink, Align::Left, current);
        c.popClip();
        const std::string frames = row.sample || (d && d->kind == DataKind::Sample)
                                       ? std::string(tr(Str::TablesSampleRow))
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
    case Kind::TbImport: {
        const bool done = w.kind == Kind::TbDone;
        host_.paintButtonFace(c, w, done, hovered, kAccent, 0.22f, kRadiusControl);
        c.text(w.rect, tr(done ? Str::TablesDone : Str::TablesImport), kTextLabel,
               done ? litInk(kText) : hovered ? kText : kDim, Align::Center, done);
        return true;
    }
    case Kind::TbScroll:
        return true;
    case Kind::TbScrollBar: // as the MODS view's
        c.fillRect(kTbScrollTrack, kTrack.alpha(0.5f), kRadiusMark);
        c.fillRect(thumb(), hovered || active ? kDim : kFaint, kRadiusMark);
        return true;
    default:
        return false;
    }
}

std::string TableBrowser::tooltip(const Widget &w) const {
    switch (w.kind) {
    case Kind::TbRow: {
        const RowList held = rows();
        const std::vector<Row> &all = *held;
        if (w.a >= 0 && w.a < static_cast<int>(all.size()) && all[w.a].error != ReadError::None)
            return tr(readErrorText(all[w.a].error));
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

std::optional<CursorKind> TableBrowser::cursor(const Widget &w) const {
    switch (w.kind) {
    case Kind::TbScroll:
    case Kind::TbScrollBar:
        return CursorKind::Arrow;
    case Kind::TbSource:
    case Kind::TbAddFolder:
    case Kind::TbRow:
    case Kind::TbDone:
    case Kind::TbImport:
        return CursorKind::Hand;
    default:
        return std::nullopt;
    }
}

// --- Input.

void TableBrowser::pick(const Row &row) {
    const int op = browsing();
    if (op < 0 || row.header || row.error != ReadError::None)
        return;
    if (row.source == -2) {
        if (kind_ == DataKind::Table)
            controller().pickTable(op, row.key);
        return;
    }
    if (row.source == -1) {
        const OpData *d = controller().data().find(row.key);
        if (!d)
            return;
        if (d->kind == DataKind::Table && kind_ == DataKind::Table) {
            controller().pickTable(op, row.key);
        } else if (d->kind == DataKind::Sample && kind_ == DataKind::Sample) {
            const UndoScope step(controller());
            controller().setValue(paramIndex(op, OpParam::Sample), static_cast<double>(row.key));
            controller().setValue(paramIndex(op, OpParam::Wave), static_cast<double>(OpWave::Sample));
        }
        return;
    }
    TableLibrary *lib = library();
    std::vector<uint8_t> bytes;
    if (!lib || row.source >= static_cast<int>(lib->sources().size()) || !lib->readEntry(row.source, row.entry, bytes)) {
        host_.flash(trf(Str::MsgImportFailed, {row.name, tr(Str::ReadBroken)}));
        return;
    }
    const LibrarySource &s = lib->sources()[row.source];
    const std::string &path = s.entries[row.entry].path;
    const size_t slash = path.rfind('/');
    const std::string said = controller().importOperatorData(
        op, kind_, bytes, slash == std::string::npos ? path : path.substr(slash + 1), s.folder.name + "/" + path,
        s.folder.frameSize);
    if (!said.empty())
        host_.flash(said);
}

// The search and a folder's name being typed let the keyboard go at a click anywhere else: the browser's keys then play
// notes, as the host has them; the name is kept.
void TableBrowser::pressing(const Widget *hit) {
    auto on = [&](Field f) {
        return hit && (hit->kind == Kind::SearchField || hit->kind == Kind::SearchClear) &&
               hit->a == static_cast<int>(f);
    };
    if (!on(Field::Tables))
        searching_ = false;
    if (!on(Field::TableRename))
        endRename(true);
}

bool TableBrowser::press(const Widget &w, Point p, Modifiers) {
    switch (w.kind) {
    case Kind::SearchField: // the search takes the keyboard; the caret goes where it is clicked, as in any field
        if (w.a == static_cast<int>(Field::Tables))
            searching_ = true;
        return false;
    case Kind::SearchClear: // the search keeps the keyboard, for what is typed next
        if (w.a != static_cast<int>(Field::Tables))
            return false;
        host_.clearField(Field::Tables);
        searching_ = true;
        host_.resetHover();
        return true;
    case Kind::TbSource:
        source_ = w.a;
        scroll_ = 0.0f;
        pick_ = -1;
        host_.clearField(Field::Tables);
        return true;
    case Kind::TbAddFolder:
        controller().chooseTableFolder(kind_);
        return true;
    case Kind::TbGroup:
        return true;
    case Kind::TbRow: {
        const RowList held = rows();
        const std::vector<Row> &all = *held;
        if (w.a >= 0 && w.a < static_cast<int>(all.size())) {
            pick_ = w.a;
            pick(all[w.a]);
        }
        return true;
    }
    case Kind::TbDone:
        close();
        return true;
    case Kind::TbImport:
        if (browsing() >= 0)
            controller().chooseOperatorFile(browsing(), kind_);
        return true;
    case Kind::TbScroll:
        return true;
    case Kind::TbScrollBar: { // as the MODS view's: off the thumb, it jumps to center on the click, then drags on
        const Rect bar = thumb();
        if (p.y < bar.y || p.y >= bar.bottom())
            scroll_ = std::clamp((p.y - bar.h * 0.5f - kTbScrollTrack.y) / std::max(kTbScrollTrack.h - bar.h, 1.0f) *
                                     end(),
                                 0.0f, end());
        scrollFrom_ = std::clamp(scroll_, 0.0f, end());
        scrolling_ = true;
        host_.startDrag(*this, w, p, false);
        return true;
    }
    default:
        return false;
    }
}

bool TableBrowser::doublePress(const Widget &w) const {
    return w.kind == Kind::TbRow; // quick clicks down the list load each
}

void TableBrowser::drag(const DragState &d) { // the list follows its scrollbar's thumb
    if (!scrolling_)
        return;
    const float room = kTbScrollTrack.h - thumb().h;
    if (room > 0.0f)
        scroll_ = std::clamp(scrollFrom_ + (d.now.y - d.start.y) / room * end(), 0.0f, end());
}

void TableBrowser::release() {
    scrolling_ = false;
}

bool TableBrowser::dragsOnTouch(const Widget &w) const {
    return w.kind == Kind::TbScrollBar;
}

bool TableBrowser::wheel(Point p, float steps) {
    if (browsing() < 0 || !kTbList.contains(p))
        return false;
    scroll_ = std::clamp(scroll_ - steps * 3.0f, 0.0f, end());
    return true;
}

void TableBrowser::searchKey(Key key) {
    const RowList held = rows();
    const std::vector<Row> &all = *held;
    switch (key) {
    case Key::Enter: // loads the row picked, and lets the keyboard go, so its keys play it
        if (pick_ >= 0 && pick_ < static_cast<int>(all.size()))
            pick(all[pick_]);
        searching_ = false;
        break;
    case Key::Escape: // empties the search, then lets the keyboard go
        if (!search_.text.empty())
            host_.clearField(Field::Tables);
        else
            searching_ = false;
        break;
    case Key::Up:
    case Key::Down: { // to the next row that is a table, that way
        const int step = key == Key::Down ? 1 : -1;
        int at = pick_;
        for (int tries = 0; tries < static_cast<int>(all.size()); ++tries) {
            at = at < 0 ? (step > 0 ? 0 : static_cast<int>(all.size()) - 1) : at + step;
            if (at < 0 || at >= static_cast<int>(all.size()))
                return;
            if (!all[at].header)
                break;
        }
        if (at < 0 || at >= static_cast<int>(all.size()) || all[at].header)
            return;
        pick_ = at;
        const float shown = kTbList.h / kTbRowH;
        if (at < scroll_)
            scroll_ = static_cast<float>(at);
        else if (at + 1 > scroll_ + shown)
            scroll_ = at + 1 - shown;
        scroll_ = std::clamp(scroll_, 0.0f, end());
        break;
    }
    default:
        break;
    }
}

void TableBrowser::endRename(bool commit) {
    if (renaming_ < 0)
        return;
    const int source = renaming_;
    renaming_ = -1;
    const std::string &text = rename_.text;
    const size_t start = text.find_first_not_of(' ');
    TableLibrary *lib = library();
    if (commit && start != std::string::npos && lib && source < static_cast<int>(lib->sources().size())) {
        const std::string name = text.substr(start, text.find_last_not_of(' ') - start + 1);
        lib->renameFolder(source, name.substr(0, kMaxFolderName));
        controller().tableFoldersChanged(kind_);
    }
    host_.resetHover();
}

// A library folder's menu: renaming it, the frame size for its files that name none (a folder of wavetables'), rescanning
// it, showing it in the system's file browser and removing it from the library (its files left as they are).
bool TableBrowser::contextMenu(const std::optional<Widget> &hit, Point, std::vector<MenuItem> &items) {
    menuSource_ = -1;
    TableLibrary *lib = library();
    if (!hit || hit->kind != Kind::TbSource || !lib || hit->a < 0 || hit->a >= static_cast<int>(lib->sources().size()))
        return false;
    endRename(true);
    menuSource_ = hit->a;
    const int size = lib->sources()[hit->a].folder.frameSize;
    items.push_back({kTbRenameCommand, tr(Str::MenuRename)});
    items.push_back({0, ""});
    if (kind_ == DataKind::Table) {
        for (int k = 0; k < static_cast<int>(std::size(kTbFrameSizes)); ++k)
            items.push_back({kTbFrameSizeCommand + k,
                             trf(Str::MenuFrameSize, {k == 0 ? std::string(tr(Str::TablesFolderAuto))
                                                             : std::to_string(kTbFrameSizes[k])}),
                             size == kTbFrameSizes[k]});
        items.push_back({0, ""});
    }
    items.push_back({kTbRescanCommand, tr(Str::MenuRescan)});
    items.push_back({kTbShowCommand, tr(Str::MenuShowFolder)});
    items.push_back({0, ""});
    items.push_back({kTbRemoveCommand, tr(Str::MenuRemove)});
    return true;
}

bool TableBrowser::menuCommand(int command) {
    if (menuSource_ < 0 || command < kTbRenameCommand || command > kTbRemoveCommand)
        return false;
    TableLibrary *lib = library();
    const int source = menuSource_;
    menuSource_ = -1;
    if (!lib || source >= static_cast<int>(lib->sources().size()))
        return true;
    if (command == kTbRenameCommand) { // its name typed into its row, all of it selected to type over
        renaming_ = source;
        rename_.most = kMaxFolderName;
        rename_.set(lib->sources()[source].folder.name);
        rename_.anchor = 0;
        searching_ = false;
        host_.resetHover();
        return true;
    }
    if (command >= kTbFrameSizeCommand && command < kTbFrameSizeCommand + static_cast<int>(std::size(kTbFrameSizes)))
        lib->setFrameSize(source, kTbFrameSizes[command - kTbFrameSizeCommand]);
    else if (command == kTbRescanCommand)
        lib->rescan(source);
    else if (command == kTbShowCommand)
        controller().showTableFolder(kind_, source);
    else if (command == kTbRemoveCommand) {
        lib->removeFolder(source);
        source_ = source_ == source ? (kind_ == DataKind::Table ? -2 : -1) : source_ > source ? source_ - 1 : source_;
    }
    controller().tableFoldersChanged(kind_);
    return true;
}

TextField *TableBrowser::field(Field f) {
    if (f == Field::Tables)
        return &search_;
    if (f == Field::TableRename)
        return &rename_;
    return nullptr;
}

// A folder's name being typed has the keyboard, else the search once clicked; while it shows neither has it, the keys
// go on to the host.
bool TableBrowser::fieldFocus(std::optional<Field> &focus) const {
    if (browsing() < 0)
        return false;
    if (renaming_ >= 0)
        focus = Field::TableRename;
    else if (searching_)
        focus = Field::Tables;
    else
        return false;
    return true;
}

// What is typed in the search changes: the list from its top, nothing picked.
void TableBrowser::fieldChanged(Field f) {
    if (f != Field::Tables)
        return;
    scroll_ = 0.0f;
    pick_ = -1;
}

void TableBrowser::fieldKey(Field f, Key key) {
    if (f == Field::Tables)
        searchKey(key);
    else if (f == Field::TableRename && (key == Key::Enter || key == Key::Escape))
        endRename(key == Key::Enter);
}

} // namespace fmvibe::gui
