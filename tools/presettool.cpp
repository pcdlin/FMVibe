// fmvibe-presettool: keeps the factory presets' files (presets/Factory/*.fmv) sparse.
//
//   fmvibe-presettool <file.fmv>...
//
// Rewrites each preset file holding only the params that differ from their defaults (which is what loading it gives
// the others) and its editor layout, as the factory presets are stored. A preset edited in the plugin and saved over
// its file holds every param (about 128 KB); this makes it sparse again before it is committed. A file it can't read
// is left as it is.

#include "gui/controller.h"
#include "state.h"

#include <cstdio>
#include <filesystem>
#include <vector>

using namespace fmvibe;

namespace {

bool normalize(const std::filesystem::path &path) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    ParamValues values;
    clap_istream_t in = fileIn(file);
    const bool read = readParams(&in, values);
    const gui::UiLayout layout = read ? readLayout(&in) : gui::UiLayout{};
    std::fclose(file);
    if (!read)
        return false;
    std::vector<unsigned char> bytes;
    clap_ostream_t out = memoryOut(bytes);
    if (!writePatch(&out, values, layout, true))
        return false;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
        return false;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && written;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fmvibe-presettool <file.fmv>...\n");
        return 2;
    }
    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        const bool done = normalize(argv[i]);
        std::fwprintf(done ? stdout : stderr, L"%ls: %ls\n", argv[i], done ? L"sparse" : L"can't read or write it");
        failed += done ? 0 : 1;
    }
    return failed ? 1 : 0;
}
