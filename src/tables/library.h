#pragma once

// The wavetable library the table browser lists (see TableBrowser): the user's folders, which they add by hand (FMVibe
// never searches the disk for other plugins' tables), each a source of the tables directly in it, its subfolders left
// out (a subfolder is added as a folder of its own). A worker thread the library owns scans them, reading each file's
// headers and chunks only (see probeTableFile()), several files at a time, and keeps what it read in a cache file keyed
// by path, size and time, so a scan only reads what changed; the main thread takes how far it got, and in the end what
// it found, in with poll(). The browser demo, which has no threads and no folders to keep, lists the files a user picks
// on the page for the session instead (see addFiles()). The table browser keeps two, each with folders and a cache of
// its own: one of wavetables, and one of samples, which lists WAV and FLAC files, each as a sample. Independent of
// Windows: tests/quality.cpp covers it.

#include "readers.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fmvibe {

// A folder the user added, as the settings file keeps it.
struct TableFolder {
    std::string name;  // its folder's name to begin with
    std::string path;  // UTF-8
    int frameSize = 0; // for its files that no chunk gives one: 0 Auto, else 256, 512, 1024 or 2048
};

// What the settings file holds once a library's changes are in it: `saved`, as the file has it now (another instance may
// have changed it since), with what the library made of `before` to get to `now`, and nothing else.
std::vector<TableFolder> mergedFolders(const std::vector<TableFolder> &saved, const std::vector<TableFolder> &before,
                                       const std::vector<TableFolder> &now);

// A file a source lists.
struct LibraryEntry {
    std::string path;  // its file's name in its source
    std::string name;  // its file's name without the extension
    std::string order; // the name in lower case, which the list is sorted by
    TableInfo info;
};

struct LibrarySource {
    TableFolder folder;
    bool missing = false;  // its folder isn't there (moved, or a drive not plugged in)
    bool scanning = false; // a scan is under way
    bool cut = false;      // it holds more files than a scan lists (kMaxFiles)
    int scanned = 0, total = 0; // how many of its files the scan under way is through, and how many it found
    std::vector<LibraryEntry> entries; // by name, the last scan's until the one under way is done
    // Files picked on the page (the browser demo), held for the session: by entry.
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> held;
};

class TableLibrary {
public:
    static constexpr int kMaxFiles = 20000; // a folder's files a scan lists, at most

    // `cache`: the cache file's path (UTF-8), "" for none. `samples`: a library of samples, not of wavetables.
    explicit TableLibrary(std::string cache = {}, bool samples = false);
    ~TableLibrary();
    TableLibrary(const TableLibrary &) = delete;
    TableLibrary &operator=(const TableLibrary &) = delete;

    // Main thread. The folders as the settings file has them, scanned anew.
    void setFolders(const std::vector<TableFolder> &folders);
    // The folders as another instance may have left them: those gone taken out, those new added and scanned, the rest
    // kept with what they have found.
    void syncFolders(const std::vector<TableFolder> &folders);
    std::vector<TableFolder> folders() const;
    // Adds a folder as a source and scans it: the index of its source. A folder that is a source already is that
    // source; one inside another source, or holding one, is a source of its own.
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
    // Shows source `source` as a scan part-way through its files (the editor's checks).
    void showScan(int source, int scanned, int total);

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
        int scanned = 0, total = 0;
        std::vector<LibraryEntry> entries; // once it is done
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
    bool samples_ = false;
    std::vector<LibrarySource> sources_; // main thread
    std::vector<uint64_t> jobOf_;         // the job each source waits for, by source
    uint64_t nextJob_ = 1;

    mutable std::mutex mutex_; // what the worker and the main thread share, below
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    std::unordered_map<std::string, CacheEntry> cache_; // by path
    bool cacheLoaded_ = false;
    bool cacheDirty_ = false;
    bool busy_ = false;
    std::atomic<bool> stopping_{false};    // the library is going: a scan under way stops
    std::atomic<uint64_t> cancelBelow_{0}; // jobs before this are called off
    std::atomic<int> filesRead_{0};
    std::thread worker_;
};

} // namespace fmvibe
