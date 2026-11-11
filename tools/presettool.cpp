// fmvibe-presettool: keeps the factory presets' files (presets/Factory/*.fmv) sparse, and their routing graphs clear.
//
//   fmvibe-presettool [--arrange] <file.fmv>...
//
// Rewrites each preset file holding only the params that differ from their defaults (which is what loading it gives
// the others) and its editor layout, as the factory presets are stored. A preset edited in the plugin and saved over
// its file holds every param (about 128 KB); this makes it sparse again before it is committed. With --arrange, it
// first lays the preset's routing graph out again (see src/gui/arrange.h) where its nodes, wires, lines to OUT or
// labels run over each other, and says how many such clashes are left. A file it can't read is left as it is.

#include "gui/arrange.h"
#include "gui/controller.h"
#include "state.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
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
std::string normalize(const std::filesystem::path &path, bool arrange) {
    FILE *file = openFile(path, "rb");
    if (!file)
        return {};
    ParamValues values;
    clap_istream_t in = fileIn(file);
    const bool read = readParams(&in, values);
    gui::UiLayout layout = read ? readLayout(&in) : gui::UiLayout{};
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
    std::vector<unsigned char> bytes;
    clap_ostream_t out = memoryOut(bytes);
    if (!writePatch(&out, values, layout, true))
        return {};
    file = openFile(path, "wb");
    if (!file)
        return {};
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && written ? said : std::string{};
}

template <typename Char> int run(int argc, Char **argv) {
    int first = 1;
    bool arrange = false;
    if (argc > 1 && std::filesystem::path(argv[1]) == "--arrange") {
        arrange = true;
        first = 2;
    }
    if (argc <= first) {
        std::fprintf(stderr, "usage: fmvibe-presettool [--arrange] <file.fmv>...\n");
        return 2;
    }
    int failed = 0;
    for (int i = first; i < argc; ++i) {
        const std::filesystem::path path(argv[i]);
        const std::string said = normalize(path, arrange);
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
