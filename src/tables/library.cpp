#include "library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

namespace fmvibe {

namespace fs = std::filesystem;

namespace {

fs::path pathOf(const std::string &utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string utf8Of(const fs::path &path) {
    const std::u8string s = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool tableExtension(const fs::path &path) {
    const std::string ext = lower(utf8Of(path.extension()));
    return ext == ".wav" || ext == ".flac" || ext == ".wt" || ext == ".vitaltable";
}

// Whether two paths name one folder, whatever their case and a slash at their end.
bool sameFolder(const fs::path &a, const fs::path &b) {
    auto plain = [](const fs::path &path) {
        std::string s = lower(utf8Of(path.lexically_normal()));
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    };
    return plain(a) == plain(b);
}

// A file read a block at a time: a scan asks for a few bytes here and there, nearly all of them near the file's start
// or past its audio, and each block saves the system calls of a seek and a read per chunk.
class BlockFile {
public:
    explicit BlockFile(const fs::path &path) : in_(path, std::ios::binary) {}
    explicit operator bool() const { return static_cast<bool>(in_); }

    size_t read(uint64_t at, void *to, size_t size) {
        auto *out = static_cast<uint8_t *>(to);
        size_t got = 0;
        while (got < size) {
            if (at < from_ || at >= from_ + have_) {
                if (size - got >= kBlock) // a whole file (a Vital table's), straight to where it goes
                    return got + fetch(at, out + got, size - got);
                from_ = at;
                have_ = fetch(at, block_, kBlock);
                if (have_ == 0)
                    break;
            }
            const size_t offset = static_cast<size_t>(at - from_);
            const size_t n = std::min(size - got, have_ - offset);
            std::memcpy(out + got, block_ + offset, n);
            got += n;
            at += n;
        }
        return got;
    }

private:
    static constexpr size_t kBlock = 16384;
    size_t fetch(uint64_t at, void *to, size_t size) {
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(at));
        in_.read(static_cast<char *>(to), static_cast<std::streamsize>(size));
        return static_cast<size_t>(std::max<std::streamsize>(in_.gcount(), 0));
    }
    std::ifstream in_;
    uint64_t from_ = 0;
    size_t have_ = 0;
    uint8_t block_[kBlock];
};

LibraryEntry entryOf(const std::string &file) {
    LibraryEntry e;
    e.path = file;
    const size_t dot = file.rfind('.');
    e.name = dot == std::string::npos ? file : file.substr(0, dot);
    e.order = lower(e.name);
    return e;
}

void sortEntries(std::vector<LibraryEntry> &entries) {
    std::sort(entries.begin(), entries.end(),
              [](const LibraryEntry &a, const LibraryEntry &b) { return a.order < b.order; });
}

// How many files a scan reads at once: opening a file is most of what reading its headers costs, more so with a virus
// scanner looking at each one, and it waits on the disk rather than the processor.
constexpr unsigned kScanThreads = 8;
// A scan's progress goes to the main thread this often, and what it read so far to the cache file this often, so a
// long first scan broken off doesn't start again from nothing.
constexpr auto kReportEvery = std::chrono::milliseconds(100);
constexpr auto kSaveEvery = std::chrono::seconds(5);

constexpr uint32_t kCacheMagic = 0x43424C54; // "TLBC"
constexpr uint32_t kCacheVersion = 1;

} // namespace

TableLibrary::TableLibrary(std::string cache) : cachePath_(std::move(cache)) {
#ifndef __EMSCRIPTEN__
    worker_ = std::thread([this] { work(); });
#endif
}

TableLibrary::~TableLibrary() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void TableLibrary::setFolders(const std::vector<TableFolder> &folders) {
    std::vector<LibrarySource> kept;
    for (LibrarySource &s : sources_) // the demo's picked files stay for the session
        if (!s.held.empty())
            kept.push_back(std::move(s));
    sources_.clear();
    jobOf_.clear();
    for (const TableFolder &f : folders) {
        LibrarySource s;
        s.folder = f;
        sources_.push_back(std::move(s));
        jobOf_.push_back(0);
        start(static_cast<int>(sources_.size()) - 1);
    }
    for (LibrarySource &s : kept) {
        sources_.push_back(std::move(s));
        jobOf_.push_back(0);
    }
}

std::vector<TableFolder> TableLibrary::folders() const {
    std::vector<TableFolder> out;
    for (const LibrarySource &s : sources_)
        if (s.held.empty())
            out.push_back(s.folder);
    return out;
}

int TableLibrary::addFolder(const std::string &path) {
    const fs::path folder = pathOf(path);
    for (size_t i = 0; i < sources_.size(); ++i)
        if (sources_[i].held.empty() && sameFolder(folder, pathOf(sources_[i].folder.path)))
            return static_cast<int>(i);
    LibrarySource s;
    s.folder.path = utf8Of(folder.lexically_normal());
    while (s.folder.path.size() > 3 && s.folder.path.back() == '/')
        s.folder.path.pop_back();
    s.folder.name = utf8Of(pathOf(s.folder.path).filename());
    if (s.folder.name.empty())
        s.folder.name = s.folder.path;
    sources_.push_back(std::move(s));
    jobOf_.push_back(0);
    start(static_cast<int>(sources_.size()) - 1);
    return static_cast<int>(sources_.size()) - 1;
}

void TableLibrary::removeFolder(int source) {
    if (source < 0 || source >= static_cast<int>(sources_.size()))
        return;
    {
        std::lock_guard lock(mutex_);
        const uint64_t job = jobOf_[source];
        std::erase_if(jobs_, [job](const Job &j) { return j.id == job; });
        if (job != 0)
            cancelBelow_.store(std::max(cancelBelow_.load(), job + 1)); // a scan of it under way stops
    }
    sources_.erase(sources_.begin() + source);
    jobOf_.erase(jobOf_.begin() + source);
    // Scans called off with it: the others start again.
    for (size_t i = 0; i < sources_.size(); ++i)
        if (sources_[i].scanning && jobOf_[i] < cancelBelow_.load())
            start(static_cast<int>(i));
}

void TableLibrary::renameFolder(int source, const std::string &name) {
    if (source >= 0 && source < static_cast<int>(sources_.size()) && !name.empty())
        sources_[source].folder.name = name;
}

void TableLibrary::setFrameSize(int source, int frameSize) {
    if (source < 0 || source >= static_cast<int>(sources_.size()) || sources_[source].folder.frameSize == frameSize)
        return;
    sources_[source].folder.frameSize = frameSize;
    rescan(source);
}

void TableLibrary::rescan(int source) {
    if (source >= 0 && source < static_cast<int>(sources_.size()) && sources_[source].held.empty())
        start(source);
}

int TableLibrary::addFiles(const std::string &name, std::vector<std::pair<std::string, std::vector<uint8_t>>> files) {
    LibrarySource s;
    s.folder.name = name;
    for (auto &[path, bytes] : files) {
        LibraryEntry e = entryOf(path);
        const auto held = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        const ReadAt read = [&held](uint64_t at, void *to, size_t n) -> size_t {
            if (at >= held->size())
                return 0;
            const size_t got = static_cast<size_t>(std::min<uint64_t>(n, held->size() - at));
            std::memcpy(to, held->data() + at, got);
            return got;
        };
        e.info = probeTableFile(read, held->size(), path);
        s.entries.push_back(std::move(e));
        s.held.push_back(held);
    }
    // Sorted with their bytes alongside.
    std::vector<size_t> order(s.entries.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::vector<LibraryEntry> sorted = s.entries;
    sortEntries(sorted);
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held(sorted.size());
    for (size_t i = 0; i < sorted.size(); ++i)
        for (size_t j = 0; j < s.entries.size(); ++j)
            if (s.entries[j].path == sorted[i].path)
                held[i] = s.held[j];
    s.entries = std::move(sorted);
    s.held = std::move(held);
    sources_.push_back(std::move(s));
    jobOf_.push_back(0);
    return static_cast<int>(sources_.size()) - 1;
}

void TableLibrary::start(int source) {
    LibrarySource &s = sources_[source];
    s.scanning = true;
    s.scanned = s.total = 0;
    const uint64_t id = nextJob_++;
    jobOf_[source] = id;
#ifdef __EMSCRIPTEN__
    Job job{id, s.folder};
    scan(job); // no threads in the demo: at once
#else
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({id, s.folder});
    }
    wake_.notify_all();
#endif
}

bool TableLibrary::scanning() const {
    for (const LibrarySource &s : sources_)
        if (s.scanning)
            return true;
    return false;
}

bool TableLibrary::poll() {
    std::vector<Result> results;
    {
        std::lock_guard lock(mutex_);
        results.swap(results_);
    }
    bool changed = false;
    for (Result &r : results)
        for (size_t i = 0; i < sources_.size(); ++i) {
            if (jobOf_[i] != r.id)
                continue;
            LibrarySource &s = sources_[i];
            // A scan reports how far it got as it goes, then its files when it is done.
            s.scanned = r.scanned;
            s.total = r.total;
            if (r.done) {
                s.entries = std::move(r.entries);
                s.scanning = false;
                s.missing = r.missing;
                s.cut = r.cut;
            }
            changed = true;
        }
    return changed;
}

void TableLibrary::showScan(int source, int scanned, int total) {
    LibrarySource &s = sources_[source];
    s.scanning = true;
    s.scanned = scanned;
    s.total = total;
}

void TableLibrary::finish() {
#ifndef __EMSCRIPTEN__
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
    lock.unlock();
#endif
    poll();
}

bool TableLibrary::readEntry(int source, int entry, std::vector<uint8_t> &bytes) const {
    if (source < 0 || source >= static_cast<int>(sources_.size()))
        return false;
    const LibrarySource &s = sources_[source];
    if (entry < 0 || entry >= static_cast<int>(s.entries.size()))
        return false;
    if (!s.held.empty()) {
        bytes = *s.held[entry];
        return true;
    }
    std::ifstream in(pathOf(s.folder.path) / pathOf(s.entries[entry].path), std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || size > (std::streamoff{256} << 20))
        return false;
    in.seekg(0);
    bytes.resize(static_cast<size_t>(size));
    in.read(reinterpret_cast<char *>(bytes.data()), size);
    return static_cast<bool>(in);
}

void TableLibrary::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_)
                return;
            job = jobs_.front();
            jobs_.pop_front();
            busy_ = true;
        }
        scan(job);
        {
            std::lock_guard lock(mutex_);
            busy_ = false;
            if (jobs_.empty() && cacheDirty_)
                saveCache();
        }
        idle_.notify_all();
    }
}

void TableLibrary::scan(const Job &job) {
    {
        std::lock_guard lock(mutex_);
        if (!cacheLoaded_)
            loadCache();
    }
    const fs::path root = pathOf(job.folder.path);
    Result done{job.id, true};
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        done.missing = true;
        std::lock_guard lock(mutex_);
        results_.push_back(std::move(done));
        return;
    }
    auto calledOff = [&] { return stopping_.load() || job.id < cancelBelow_.load(); };

    // The tables directly in the folder, its subfolders left out.
    struct Found {
        fs::path file;
        std::string key; // its path, in the cache
        uint64_t size = 0;
        int64_t time = 0;
        LibraryEntry entry;
    };
    std::vector<Found> found;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (calledOff())
            return;
        std::error_code e;
        if (!it->is_regular_file(e) || !tableExtension(it->path()))
            continue;
        if (found.size() == static_cast<size_t>(kMaxFiles)) {
            done.cut = true;
            break;
        }
        Found f;
        f.file = it->path();
        f.key = utf8Of(f.file);
        f.size = it->file_size(e);
        f.time = static_cast<int64_t>(it->last_write_time(e).time_since_epoch().count());
        f.entry = entryOf(utf8Of(f.file.filename()));
        found.push_back(std::move(f));
    }

    // What the cache knows of them; the rest is read.
    std::vector<size_t> unread;
    {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < found.size(); ++i) {
            const auto cached = cache_.find(found[i].key);
            if (cached != cache_.end() && cached->second.size == found[i].size &&
                cached->second.time == found[i].time && cached->second.frameSize == job.folder.frameSize)
                found[i].entry.info = cached->second.info;
            else
                unread.push_back(i);
        }
    }
    const int total = static_cast<int>(found.size());
    int scanned = total - static_cast<int>(unread.size()); // under the mutex
    auto report = [&] { // under the mutex: the newest in place of one the main thread hasn't taken yet
        if (!results_.empty() && results_.back().id == job.id && !results_.back().done)
            results_.back().scanned = scanned;
        else
            results_.push_back({job.id, false, false, false, scanned, total, {}});
    };
    {
        std::lock_guard lock(mutex_);
        report();
    }

    std::atomic<size_t> next{0};
    auto lastReport = std::chrono::steady_clock::now(), lastSave = lastReport; // under the mutex
    auto readFiles = [&] {
        for (;;) {
            const size_t at = next.fetch_add(1);
            if (at >= unread.size() || calledOff())
                return;
            Found &f = found[unread[at]];
            BlockFile in(f.file);
            const ReadAt read = [&in](uint64_t from, void *to, size_t n) { return in.read(from, to, n); };
            f.entry.info = in ? probeTableFile(read, f.size, f.entry.path, job.folder.frameSize)
                              : TableInfo{ReadError::Broken};
            filesRead_.fetch_add(1);
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard lock(mutex_);
            cache_[f.key] = CacheEntry{f.size, f.time, job.folder.frameSize, f.entry.info};
            cacheDirty_ = true;
            ++scanned;
            if (now - lastReport >= kReportEvery) {
                lastReport = now;
                report();
            }
            if (now - lastSave >= kSaveEvery) {
                lastSave = now;
                saveCache();
            }
        }
    };
#ifdef __EMSCRIPTEN__
    readFiles(); // no threads in the demo
#else
    std::vector<std::thread> helpers;
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 2u, kScanThreads);
    for (unsigned t = 1; t < threads && t < unread.size(); ++t)
        helpers.emplace_back(readFiles);
    readFiles();
    for (std::thread &helper : helpers)
        helper.join();
#endif
    if (calledOff())
        return;

    done.scanned = done.total = total;
    done.entries.reserve(found.size());
    for (Found &f : found)
        done.entries.push_back(std::move(f.entry));
    sortEntries(done.entries);
    std::lock_guard lock(mutex_);
    results_.push_back(std::move(done));
}

// The cache file: magic, version, count, then each file's path, size, time, the folder's frame size and what was read.
void TableLibrary::loadCache() {
    cacheLoaded_ = true;
    if (cachePath_.empty())
        return;
    std::ifstream in(pathOf(cachePath_), std::ios::binary);
    auto word = [&in]() {
        uint32_t w = 0;
        in.read(reinterpret_cast<char *>(&w), 4);
        return w;
    };
    auto text = [&]() {
        const uint32_t n = word();
        std::string s(std::min<uint32_t>(n, 4096), '\0');
        in.read(s.data(), static_cast<std::streamsize>(s.size()));
        return s;
    };
    if (!in || word() != kCacheMagic || word() != kCacheVersion)
        return;
    const uint32_t count = std::min<uint32_t>(word(), 1u << 20);
    cache_.reserve(count);
    for (uint32_t i = 0; i < count && in; ++i) {
        std::string path = text();
        CacheEntry c;
        c.size = word();
        c.size |= uint64_t{word()} << 32;
        c.time = static_cast<int64_t>(word());
        c.time |= static_cast<int64_t>(uint64_t{word()} << 32);
        c.frameSize = static_cast<int>(word());
        c.info.error = static_cast<ReadError>(std::min<uint32_t>(word(), static_cast<uint32_t>(ReadError::NotTable)));
        c.info.sample = word() != 0;
        c.info.format = text();
        c.info.frameSize = static_cast<int>(word());
        c.info.guessed = word() != 0;
        c.info.frames = static_cast<int>(word());
        if (in)
            cache_.emplace(std::move(path), c);
    }
}

void TableLibrary::saveCache() {
    cacheDirty_ = false;
    if (cachePath_.empty())
        return;
    std::error_code error;
    fs::create_directories(pathOf(cachePath_).parent_path(), error);
    const fs::path part = pathOf(cachePath_ + ".part");
    {
        std::ofstream out(part, std::ios::binary);
        auto word = [&out](uint32_t w) { out.write(reinterpret_cast<const char *>(&w), 4); };
        auto text = [&](const std::string &s) {
            word(static_cast<uint32_t>(s.size()));
            out.write(s.data(), static_cast<std::streamsize>(s.size()));
        };
        word(kCacheMagic);
        word(kCacheVersion);
        word(static_cast<uint32_t>(cache_.size()));
        for (const auto &[path, c] : cache_) {
            text(path);
            word(static_cast<uint32_t>(c.size));
            word(static_cast<uint32_t>(c.size >> 32));
            word(static_cast<uint32_t>(static_cast<uint64_t>(c.time)));
            word(static_cast<uint32_t>(static_cast<uint64_t>(c.time) >> 32));
            word(static_cast<uint32_t>(c.frameSize));
            word(static_cast<uint32_t>(c.info.error));
            word(c.info.sample ? 1u : 0u);
            text(c.info.format);
            word(static_cast<uint32_t>(c.info.frameSize));
            word(c.info.guessed ? 1u : 0u);
            word(static_cast<uint32_t>(c.info.frames));
        }
        if (!out)
            return;
    }
    fs::rename(part, pathOf(cachePath_), error);
}

} // namespace fmvibe
