# The Linux development build: what runs without Windows, so cloud sessions (and anyone on Linux) can build the
# engine and the editor, run the audio quality test and render the UI snapshots. The plugins themselves, their
# smoke tests and the validators need Windows (Win32, Direct2D, the plugin hosts); this builds none of them.
# The editor draws through Cairo here (src/gui/canvas_cairo.cpp). See "Linux development build" in AGENTS.md, and
# `./build.sh`.

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)

function(fmvibe_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_options(${target} PRIVATE -Wall)
endfunction()

fmvibe_factory_presets(factory_presets)
add_library(fmvibe-core STATIC
    ${FMVIBE_ENGINE_SOURCES}
    ${factory_presets}
    src/factory.cpp
    src/factory.h
    src/gui/arrange.h
    src/gui/canvas_cairo.cpp
    src/gui/canvas.h
    src/gui/controller.cpp
    src/gui/controller.h
    src/gui/dialogs.cpp
    src/gui/filterdisplay.cpp
    src/gui/filterdisplay.h
    src/gui/fxgraph.h
    src/gui/fxgraphmodel.cpp
    src/gui/fxgraphmodel.h
    src/gui/fxpanel.cpp
    src/gui/fxpanel.h
    src/gui/icons.cpp
    src/gui/icons.h
    src/gui/inspectorpanel.cpp
    src/gui/inspectorpanel.h
    src/gui/lists.h
    src/gui/modulationmodel.cpp
    src/gui/modulationmodel.h
    src/gui/modspanel.cpp
    src/gui/modspanel.h
    src/gui/panel.h
    src/gui/seqpanel.cpp
    src/gui/seqpanel.h
    src/gui/layout.h
    src/gui/opsource.cpp
    src/gui/patchedits.cpp
    src/gui/patchedits.h
    src/gui/presetbrowser.cpp
    src/gui/presetbrowser.h
    src/gui/routing.h
    src/gui/routingmodel.cpp
    src/gui/routingmodel.h
    src/gui/routingpanel.cpp
    src/gui/routingpanel.h
    src/gui/shapeeditor.cpp
    src/gui/shapeeditor.h
    src/gui/tablebrowser.cpp
    src/gui/tablebrowser.h
    src/gui/theme.cpp
    src/gui/theme.h
    src/gui/view.cpp
    src/gui/view.h
    src/gui/widget.h
    src/remotepages.cpp
    src/remotepages.h
    src/sharedtelemetry.h
    src/state.cpp
    src/state.h
    src/tables/flac.cpp
    src/tables/library.cpp
    src/tables/library.h
    src/tables/readers.cpp
    src/tables/vital.cpp
    src/tables/readers.h
)
target_include_directories(fmvibe-core PUBLIC src)
target_compile_definitions(fmvibe-core PUBLIC FMVIBE_CAIRO)
find_package(Threads REQUIRED)
target_link_libraries(fmvibe-core PUBLIC clap PkgConfig::CAIRO Threads::Threads)
fmvibe_options(fmvibe-core)

enable_testing()

add_executable(fmvibe-quality tests/quality.cpp)
target_link_libraries(fmvibe-quality PRIVATE fmvibe-core)
fmvibe_options(fmvibe-quality)
# The stack the Windows build links it with (/STACK in CMakeLists.txt): it keeps whole engines on the stack, more
# at once than a shell's usual 8 MB holds. A main thread's stack on Linux is the shell's limit, so the test raises it.
add_test(NAME quality COMMAND sh -c "ulimit -s 65536 && exec \"$1\"" sh $<TARGET_FILE:fmvibe-quality>)

add_executable(fmvibe-uishot tools/uishot.cpp)
target_link_libraries(fmvibe-uishot PRIVATE fmvibe-core)
fmvibe_options(fmvibe-uishot)
add_test(NAME ui COMMAND fmvibe-uishot ${CMAKE_CURRENT_BINARY_DIR}/ui-shots)

add_executable(fmvibe-presettool tools/presettool.cpp)
target_link_libraries(fmvibe-presettool PRIVATE fmvibe-core)
fmvibe_options(fmvibe-presettool)
