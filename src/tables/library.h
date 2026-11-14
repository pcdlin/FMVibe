#pragma once

// The wavetable library the table browser lists (see View::paintTableBrowser()): the user's folders, which they add by
// hand (FMVibe never searches the disk for other plugins' tables), each a source with every subfolder in it at any
// depth. A worker thread the library owns scans them, reading each file's headers and chunks only (see
// probeTableFile()), and keeps what it read in a cache file keyed by path, size and time, so a scan only reads what
// changed; the main thread takes what it found in with poll(), so the browser fills in as it goes. The browser demo,
// which has no threads and no folders to keep, lists the files a user picks on the page for the session instead (see
// addFiles()). Independent of Windows: tests/quality.cpp covers it.

#include "readers.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fmvibe {

// A folder the user added, as the settings file keeps it.
struct TableFolder {
    std::string name;  // its folder's name to begin with
    std::string path;  // UTF-8
    int frameSize = 0; // for its files that no chunk gives one: 0 Auto, else 256, 512, 1024 or 2048
};

// A file a source lists.
struct LibraryEntry {
    std::string path;  // within its source, "/" between folders
    std::string group; // its folder within the source, "" at its top
    std::string name;  // its file's name without the extension
    TableInfo info;
};

struct LibrarySource {
    TableFolder folder;
    bool missing = false;  // its folder isn't there (moved, or a drive not plugged in)
    bool scanning = false; // a scan is under way
    bool cut = false;      // it holds more files than a scan lists (kMaxFiles)
    std::vector<LibraryEntry> entries; // by path
    // Files picked on the page (the browser demo), held for the session: by entry.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;
};

class TableLibrary {
public:
    static constexpr int kMaxFiles = 20000; // a folder's files a scan lists, at most

    // `cache`: the cache file's path (UTF-8), "" for none.
    explicit TableLibrary(std::string cache = {});
    ~TableLibrary();
    TableLibrary(const TableLibrary &) = delete;
    TableLibrary &operator=(const TableLibrary &) = delete;

    // Main thread. The folders as the settings file has them, scanned anew.
    void setFolders(const std::vector<TableFolder> &folders);
    std::vector<TableFolder> folders() const;
    // Adds a folder as a source and scans it: the index of its source. A folder that is a source already, or lies
    // inside one, is that source; a folder holding sources takes them into it.
    int addFolder(const std::string &path);
    void removeFolder(int source);
    void renameFolder(int source, const std::string &name);
    void setFrameSize(int source, int frameSize);
    void rescan(int source);
    // The browser demo: files picked on the page (path within the folder picked, and bytes), as a source for the session.
    int addFiles(const std::string &name, std::vector<std::pair<std::string, std::vector<uint8_t>>> files);

    // Main thread: takes in what the scans found since the last call; true if anything changed.
    bool poll();
    const std::vector<LibrarySource> &sources() const { return sources_; }
    // Whether a scan is under way.
    bool scanning() const;
    // An entry's file, whole: false if it can't be read now.
    bool readEntry(int source, int entry, std::vector<uint8_t> &bytes) const;
    // Waits for the scans under way to finish, then takes in what they found (tests).
    void finish();
    // How many files the scans read since the library was made, past the cache (tests).
    int filesRead() const { return filesRead_.load(); }

private:
    struct Job {
        uint64_t id;
        TableFolder folder;
    };
    struct Result {
        uint64_t id;
        bool done = false;
        bool missing = false;
        bool cut = false;
        std::vector<LibraryEntry> entries;
    };
    struct CacheEntry {
        uint64_t size = 0;
        int64_t time = 0;
        int frameSize = 0; // the folder's setting it was read with
        TableInfo info;
    };
    void start(int source);
    void work();
    void loadCache();
    void saveCache();
    void scan(const Job &job);

    std::string cachePath_;
    std::vector<LibrarySource> sources_; // main thread
    std::vector<uint64_t> jobOf_;         // the job each source waits for, by source
    uint64_t nextJob_ = 1;

    mutable std::mutex mutex_; // what the worker and the main thread share, below
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    std::vector<std::pair<std::string, CacheEntry>> cache_; // by path, sorted
    bool cacheLoaded_ = false;
    bool cacheDirty_ = false;
    bool busy_ = false;
    bool stopping_ = false;
    std::atomic<uint64_t> cancelBelow_{0}; // jobs before this are called off
    std::atomic<int> filesRead_{0};
    std::thread worker_;
};

} // namespace fmvibe
