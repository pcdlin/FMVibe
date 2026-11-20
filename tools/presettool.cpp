// fmvibe-presettool: keeps the factory presets' files (presets/Factory/*.fmv) sparse, and their routing graphs clear.
//
//   fmvibe-presettool [--arrange] [--author=<name>] [--category=<name>] [--description=<line>] <file.fmv>...
//
// Rewrites each preset file holding only the params that differ from their defaults (which is what loading it gives
// the others) and its editor layout, then its DATA chunk (its own tables and samples) as it was, as the factory presets
// are stored. A preset edited in the plugin and saved over
// its file holds every param (about 128 KB); this makes it sparse again before it is committed. With --arrange, it
// first lays the preset's routing graph out again (see src/gui/arrange.h) where its nodes, wires, lines to OUT or
// labels run over each other, and says how many such clashes are left. --author, --category and --description set
// what the preset says about itself (see gui::PresetInfo), each in every file given. A file it can't read is left as
// it is.

#include "gui/arrange.h"
#include "gui/controller.h"
#include "state.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace fmvibe;

namespace {

// The file at `path`, opened in `mode` ("rb" or "wb"), or null.
FILE *openFile(const std::filesystem::path &path, const char *mode) {
#ifdef _WIN32
    FILE *file = nullptr;
    const wchar_t *wide = mode[0] == 'r' ? L"rb" : L"wb";
    return _wfopen_s(&file, path.c_str(), wide) == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

// Rewrites the preset at `path` sparse, laid out again first with `arrange`; what is said of it, or empty where it
// can't be read or written.
// The info to set: each part only where it was given.
struct InfoChange {
    std::optional<std::string> author, category, description;
};

std::string normalize(const std::filesystem::path &path, bool arrange, const InfoChange &info) {
    FILE *file = openFile(path, "rb");
    if (!file)
        return {};
    ParamValues values;
    clap_istream_t in = fileIn(file);
    const bool read = readParams(&in, values);
    gui::UiLayout layout = read ? readLayout(&in) : gui::UiLayout{};
    // What follows the layout (the DATA chunk of the tables and samples the preset holds) stays as it is.
    std::vector<unsigned char> rest;
    for (unsigned char chunk[4096]; read;) {
        const size_t n = std::fread(chunk, 1, sizeof(chunk), file);
        rest.insert(rest.end(), chunk, chunk + n);
        if (n < sizeof(chunk))
            break;
    }
    std::fclose(file);
    if (!read)
        return {};
    std::string said = "sparse";
    if (arrange) {
        const int before = gui::detail::arrangeScore(values, layout).clashes;
        if (before > 0)
            gui::detail::arrangeRouting(values, layout);
        const int after = gui::detail::arrangeScore(values, layout).clashes;
        said += before == 0 ? ", clear" : ", " + std::to_string(before) + " clashes laid out, " +
                                              std::to_string(after) + " left";
    }
    if (info.author)
        layout.presetInfo.author = *info.author;
    if (info.category)
        layout.presetInfo.category = *info.category;
    if (info.description)
        layout.presetInfo.description = *info.description;
    if (info.author || info.category || info.description)
        said += ", info set";
    std::vector<unsigned char> bytes;
    clap_ostream_t out = memoryOut(bytes);
    if (!writePatch(&out, values, layout, true))
        return {};
    bytes.insert(bytes.end(), rest.begin(), rest.end());
    file = openFile(path, "wb");
    if (!file)
        return {};
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && written ? said : std::string{};
}

template <typename Char> int run(int argc, Char **argv) {
    bool arrange = false;
    InfoChange info;
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path arg(argv[i]);
        const std::u8string u8 = arg.u8string();
        const std::string text(u8.begin(), u8.end());
        auto option = [&](const char *name, std::optional<std::string> &to) {
            const std::string prefix = std::string("--") + name + "=";
            if (!text.starts_with(prefix))
                return false;
            to = text.substr(prefix.size());
            return true;
        };
        if (text == "--arrange")
            arrange = true;
        else if (!option("author", info.author) && !option("category", info.category) &&
                 !option("description", info.description))
            files.push_back(arg);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: fmvibe-presettool [--arrange] [--author=<name>] [--category=<name>] "
                             "[--description=<line>] <file.fmv>...\n");
        return 2;
    }
    int failed = 0;
    for (const std::filesystem::path &path : files) {
        const std::string said = normalize(path, arrange, info);
        std::fprintf(said.empty() ? stderr : stdout, "%s: %s\n", path.filename().string().c_str(),
                     said.empty() ? "can't read or write it" : said.c_str());
        failed += said.empty() ? 1 : 0;
    }
    return failed ? 1 : 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    return run(argc, argv);
}
#else
int main(int argc, char **argv) {
    return run(argc, argv);
}
#endif
