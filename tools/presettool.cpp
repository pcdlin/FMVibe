// fmvibe-presettool: keeps the factory presets' files (presets/Factory/*.fmv) sparse, and their routing graphs clear.
//
//   fmvibe-presettool [--arrange] [--author=<name>] [--category=<name>] [--description=<line>] <file.fmv>...
//   fmvibe-presettool --dx7=<folder> <file.syx>...
//
// Rewrites each preset file holding only the params that differ from their defaults (which is what loading it gives
// the others) and its editor layout, as the factory presets are stored. A preset edited in the plugin and saved over
// its file holds every param (about 128 KB); this makes it sparse again before it is committed. With --arrange, it
// first lays the preset's routing graph out again (see src/gui/arrange.h) where its nodes, wires, lines to OUT or
// labels run over each other, and says how many such clashes are left. --author, --category and --description set
// what the preset says about itself (see gui::PresetInfo), each in every file given. A file it can't read is left as
// it is.
//
// With --dx7, it imports DX7 sysex files instead, as the plugin's Load does (see src/gui/dx7.h): each voice of each
// file a preset in <folder>/<the file's name>, named by its place in the file and its name.

#include "gui/arrange.h"
#include "gui/dx7.h"
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
    file = openFile(path, "wb");
    if (!file)
        return {};
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && written ? said : std::string{};
}

// Imports the DX7 voices in the sysex file at `path` into `into`/<its name>: how many, or -1 where it holds none or a
// preset couldn't be written.
int importDx7(const std::filesystem::path &path, const std::filesystem::path &into) {
    FILE *file = openFile(path, "rb");
    if (!file)
        return -1;
    std::vector<unsigned char> bytes;
    unsigned char chunk[4096];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(file);
    const std::u8string stem = path.stem().u8string();
    const std::vector<gui::dx7::Preset> presets =
        gui::dx7::presetsOf(bytes.data(), bytes.size(), std::string(stem.begin(), stem.end()));
    if (presets.empty())
        return -1;
    const std::filesystem::path folder = into / path.stem();
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    for (const gui::dx7::Preset &preset : presets) {
        const std::u8string name(preset.name.begin(), preset.name.end());
        FILE *out = openFile(folder / std::filesystem::path(name + u8".fmv"), "wb");
        if (!out)
            return -1;
        const bool written = std::fwrite(preset.bytes.data(), 1, preset.bytes.size(), out) == preset.bytes.size();
        if (std::fclose(out) != 0 || !written)
            return -1;
    }
    return static_cast<int>(presets.size());
}

template <typename Char> int run(int argc, Char **argv) {
    bool arrange = false;
    InfoChange info;
    std::optional<std::string> dx7;
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
                 !option("description", info.description) && !option("dx7", dx7))
            files.push_back(arg);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: fmvibe-presettool [--arrange] [--author=<name>] [--category=<name>] "
                             "[--description=<line>] <file.fmv>...\n"
                             "       fmvibe-presettool --dx7=<folder> <file.syx>...\n");
        return 2;
    }
    int failed = 0;
    if (dx7) {
        const std::filesystem::path into(std::u8string(dx7->begin(), dx7->end()));
        for (const std::filesystem::path &path : files) {
            const int voices = importDx7(path, into);
            if (voices < 0)
                std::fprintf(stderr, "%s: no DX7 voices, or a preset couldn't be written\n",
                             path.filename().string().c_str());
            else
                std::fprintf(stdout, "%s: %d voices\n", path.filename().string().c_str(), voices);
            failed += voices < 0 ? 1 : 0;
        }
        return failed ? 1 : 0;
    }
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
