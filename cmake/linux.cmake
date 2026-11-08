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
    src/gui/canvas_cairo.cpp
    src/gui/canvas.h
    src/gui/controller.cpp
    src/gui/controller.h
    src/gui/dialogs.cpp
    src/gui/fxgraph.cpp
    src/gui/fxgraph.h
    src/gui/icons.cpp
    src/gui/icons.h
    src/gui/mods.cpp
    src/gui/layout.h
    src/gui/routing.cpp
    src/gui/routing.h
    src/gui/shapes.cpp
    src/gui/theme.cpp
    src/gui/theme.h
    src/gui/view.cpp
    src/gui/view.h
    src/remotepages.cpp
    src/remotepages.h
    src/sharedtelemetry.h
    src/state.cpp
    src/state.h
)
target_include_directories(fmvibe-core PUBLIC src)
target_compile_definitions(fmvibe-core PUBLIC FMVIBE_CAIRO)
target_link_libraries(fmvibe-core PUBLIC clap PkgConfig::CAIRO)
fmvibe_options(fmvibe-core)

enable_testing()

add_executable(fmvibe-quality tests/quality.cpp)
target_link_libraries(fmvibe-quality PRIVATE fmvibe-core)
fmvibe_options(fmvibe-quality)
add_test(NAME quality COMMAND fmvibe-quality)

add_executable(fmvibe-uishot tools/uishot.cpp)
target_link_libraries(fmvibe-uishot PRIVATE fmvibe-core)
fmvibe_options(fmvibe-uishot)
add_test(NAME ui COMMAND fmvibe-uishot ${CMAKE_CURRENT_BINARY_DIR}/ui-shots)

add_executable(fmvibe-presettool tools/presettool.cpp)
target_link_libraries(fmvibe-presettool PRIVATE fmvibe-core)
fmvibe_options(fmvibe-presettool)
