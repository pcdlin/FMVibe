#!/usr/bin/env bash
# The Linux development build (see "Linux development build" in AGENTS.md): the engine and the editor over Cairo, the
# audio quality test and the UI snapshots, for cloud sessions and anyone on Linux. The plugins themselves need
# Windows: build.ps1 builds them.
#
#   ./build.sh                  # build into build/linux and run the tests (quality, ui)
#   ./build.sh --skip-tests     # build only
#   ./build.sh --debug          # a debug build, into build/linux-debug
#   ./build.sh --shots [scale]  # build, then render the editor's snapshots into build/linux/ui-shots (at 2 for 2x)
#
# The snapshots are PNGs: open them to look at the editor. The text is drawn in Inter (FMVIBE_FONT picks another),
# not Windows' Segoe UI, so it comes out a little wider than in the plugin.
set -euo pipefail
cd "$(dirname "$0")"

config=Release
dir=build/linux
tests=1
shots=0
scale=1
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-tests) tests=0 ;;
        --debug) config=Debug; dir=build/linux-debug ;;
        --shots) shots=1; tests=0
                 if [ $# -gt 1 ] && [[ "$2" =~ ^[0-9.]+$ ]]; then scale=$2; shift; fi ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

for tool in cmake ninja c++ pkg-config git; do
    command -v "$tool" >/dev/null || { echo "$tool is missing" >&2; exit 1; }
done
if ! pkg-config --exists cairo; then
    echo "Cairo's headers are missing: sudo apt-get install -y libcairo2-dev" >&2
    exit 1
fi

# The CLAP SDK, at the tag CMakeLists.txt pins. CMake would download GitHub's archive of it, which the cloud
# sessions' proxy refuses; a git clone of the same tag goes through.
tag=$(sed -n 's|.*github.com/free-audio/clap/archive/refs/tags/\([^/]*\)\.tar\.gz.*|\1|p' CMakeLists.txt)
[ -n "$tag" ] || { echo "no CLAP tag found in CMakeLists.txt" >&2; exit 1; }
clap=build/deps/clap-$tag
if [ ! -f "$clap/include/clap/clap.h" ]; then
    echo "== fetch CLAP $tag"
    rm -rf "$clap"
    git clone --quiet --depth 1 --branch "$tag" -c advice.detachedHead=false https://github.com/free-audio/clap "$clap"
fi

echo "== configure ($config)"
cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DFETCHCONTENT_SOURCE_DIR_CLAP="$PWD/$clap" >/dev/null
echo "== build"
cmake --build "$dir"

if [ "$tests" = 1 ]; then
    echo "== test"
    ctest --test-dir "$dir" --output-on-failure
    echo "The editor's snapshots are in $dir/ui-shots."
fi
if [ "$shots" = 1 ]; then
    echo "== snapshots"
    "$dir/fmvibe-uishot" "$dir/ui-shots" "$scale" | grep -v '^ok' || true
fi
