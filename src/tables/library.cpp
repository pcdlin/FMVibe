#include "library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

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

// Whether folder `inner` is `outer` or lies inside it.
bool within(const fs::path &inner, const fs::path &outer) {
    const std::string a = lower(utf8Of(inner.lexically_normal())), b = lower(utf8Of(outer.lexically_normal()));
    auto trimmed = [](std::string s) {
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    };
    const std::string i = trimmed(a), o = trimmed(b);
    return i == o || (i.size() > o.size() && i.compare(0, o.size(), o) == 0 && i[o.size()] == '/');
}

LibraryEntry entryOf(const std::string &relative) {
    LibraryEntry e;
    e.path = relative;
    const size_t slash = relative.rfind('/');
    e.group = slash == std::string::npos ? std::string() : relative.substr(0, slash);
    const std::string file = slash == std::string::npos ? relative : relative.substr(slash + 1);
    const size_t dot = file.rfind('.');
    e.name = dot == std::string::npos ? file : file.substr(0, dot);
    return e;
}

void sortEntries(std::vector<LibraryEntry> &entries) {
    std::sort(entries.begin(), entries.end(), [](const LibraryEntry &a, const LibraryEntry &b) {
        const std::string x = lower(a.group), y = lower(b.group);
        return x != y ? x < y : lower(a.name) < lower(b.name);
    });
}

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
        if (sources_[i].held.empty() && within(folder, pathOf(sources_[i].folder.path)))
            return static_cast<int>(i);
    // The sources inside it go into it: it takes the first one's place.
    int at = -1;
    for (size_t i = sources_.size(); i-- > 0;)
        if (sources_[i].held.empty() && within(pathOf(sources_[i].folder.path), folder)) {
            removeFolder(static_cast<int>(i));
            at = static_cast<int>(i);
        }
    LibrarySource s;
    s.folder.path = utf8Of(folder.lexically_normal());
    while (s.folder.path.size() > 3 && s.folder.path.back() == '/')
        s.folder.path.pop_back();
    s.folder.name = utf8Of(pathOf(s.folder.path).filename());
    if (s.folder.name.empty())
        s.folder.name = s.folder.path;
    const int index = at >= 0 ? at : static_cast<int>(sources_.size());
    sources_.insert(sources_.begin() + index, std::move(s));
    jobOf_.insert(jobOf_.begin() + index, 0);
    start(index);
    return index;
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
            // A scan reports its files as it goes, then once more with all of them when it is done.
            if (r.done) {
                s.entries = std::move(r.entries);
                s.scanning = false;
                s.missing = r.missing;
                s.cut = r.cut;
            } else {
                s.entries.insert(s.entries.end(), r.entries.begin(), r.entries.end());
                sortEntries(s.entries);
            }
            changed = true;
        }
    return changed;
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
    std::vector<LibraryEntry> batch;
    auto flush = [&] {
        std::lock_guard lock(mutex_);
        results_.push_back({job.id, false, false, false, batch});
        batch.clear();
    };
    int seen = 0;
    auto lastFlush = std::chrono::steady_clock::now();
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (job.id < cancelBelow_.load())
            return; // its source was removed
        std::error_code e;
        if (!it->is_regular_file(e) || !tableExtension(it->path()))
            continue;
        if (++seen > kMaxFiles) {
            done.cut = true;
            break;
        }
        const fs::path &file = it->path();
        LibraryEntry entry = entryOf(utf8Of(file.lexically_relative(root)));
        const uint64_t size = it->file_size(e);
        const auto time = static_cast<int64_t>(it->last_write_time(e).time_since_epoch().count());
        const std::string key = utf8Of(file);
        bool cached = false;
        {
            std::lock_guard lock(mutex_);
            const auto found = std::lower_bound(cache_.begin(), cache_.end(), key,
                                                [](const auto &a, const std::string &k) { return a.first < k; });
            if (found != cache_.end() && found->first == key && found->second.size == size &&
                found->second.time == time && found->second.frameSize == job.folder.frameSize) {
                entry.info = found->second.info;
                cached = true;
            }
        }
        if (!cached) {
            std::ifstream in(file, std::ios::binary);
            const ReadAt read = [&in](uint64_t at, void *to, size_t n) -> size_t {
                in.clear();
                in.seekg(static_cast<std::streamoff>(at));
                in.read(static_cast<char *>(to), static_cast<std::streamsize>(n));
                return static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0));
            };
            entry.info = in ? probeTableFile(read, size, utf8Of(file.filename()), job.folder.frameSize)
                            : TableInfo{ReadError::Broken};
            filesRead_.fetch_add(1);
            std::lock_guard lock(mutex_);
            const auto found = std::lower_bound(cache_.begin(), cache_.end(), key,
                                                [](const auto &a, const std::string &k) { return a.first < k; });
            const CacheEntry c{size, time, job.folder.frameSize, entry.info};
            if (found != cache_.end() && found->first == key)
                found->second = c;
            else
                cache_.insert(found, {key, c});
            cacheDirty_ = true;
        }
        done.entries.push_back(entry);
        batch.push_back(std::move(entry));
        if (const auto now = std::chrono::steady_clock::now(); now - lastFlush > std::chrono::milliseconds(150)) {
            flush();
            lastFlush = now;
        }
    }
    sortEntries(done.entries);
    std::lock_guard lock(mutex_);
    if (job.id >= cancelBelow_.load())
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
            cache_.push_back({std::move(path), c});
    }
    std::sort(cache_.begin(), cache_.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
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
