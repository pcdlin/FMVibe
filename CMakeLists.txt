cmake_minimum_required(VERSION 3.25)
project(FMVibe VERSION 0.1 LANGUAGES CXX)

# The version hosts are told and the settings dialog shows, for every target (the browser demo's too). Change it
# in project() above.
add_compile_definitions(FMVIBE_VERSION="${PROJECT_VERSION}")

include(FetchContent)

# CLAP SDK (headers only), pinned by tag and hash.
FetchContent_Declare(clap
    URL https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz
    URL_HASH SHA256=58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(clap)

# The sound engine, independent of CLAP and Windows: the plugin and the browser demo share it.
set(FMVIBE_ENGINE_SOURCES
    src/analyzer.cpp
    src/analyzer.h
    src/effects.cpp
    src/effects.h
    src/factory.cpp
    src/factory.h
    src/envelope.h
    src/filter.h
    src/noise.h
    src/params.cpp
    src/params.h
    src/uistrings.cpp
    src/uistrings.h
    src/uistrings.inc
    src/synth.cpp
    src/synth.h
    src/wavetable.cpp
    src/wavetable.h
)

# The browser demo (web/), built with Emscripten: `.\build-web.ps1`.
if (EMSCRIPTEN)
    add_subdirectory(web)
    return()
endif()

if (NOT WIN32)
    message(FATAL_ERROR "FMVibe's editor is built on Win32 and Direct2D; only Windows is supported for now.")
endif()

# A .clap is a plain DLL with another extension. Link the CRT statically so
# the plugin loads on machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(fmvibe_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# Engine and editor view, shared by the plugin and the tools.
add_library(fmvibe-core STATIC
    ${FMVIBE_ENGINE_SOURCES}
    src/gui/canvas.cpp
    src/gui/canvas.h
    src/gui/controller.cpp
    src/gui/controller.h
    src/gui/dialogs.cpp
    src/gui/icons.cpp
    src/gui/icons.h
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
target_link_libraries(fmvibe-core PUBLIC clap d2d1 dwrite)
fmvibe_options(fmvibe-core)

add_library(FMVibe MODULE
    src/entry.cpp
    src/plugin.cpp
    src/plugin.h
    src/presets.cpp
    src/presets.h
    src/usersettings.cpp
    src/usersettings.h
    src/gui/editor.cpp
    src/gui/editor.h
)
target_link_libraries(FMVibe PRIVATE fmvibe-core)
set_target_properties(FMVibe PROPERTIES PREFIX "" SUFFIX ".clap")
fmvibe_options(FMVibe)

# `cmake --install` puts the plugin into <prefix>/CLAP. The presets set the
# prefix to %LOCALAPPDATA%/Programs/Common, the per-user CLAP folder.
install(TARGETS FMVibe LIBRARY DESTINATION CLAP)

enable_testing()

# Headless host that loads the built plugin, plays notes and opens the editor.
add_executable(fmvibe-smoke tests/smoke_host.cpp src/uistrings.cpp)
target_link_libraries(fmvibe-smoke PRIVATE clap)
# For the param id helpers in params.h and the wordings in uistrings.h; the test still talks to the plugin only
# through CLAP.
target_include_directories(fmvibe-smoke PRIVATE src)
fmvibe_options(fmvibe-smoke)
add_dependencies(fmvibe-smoke FMVibe)
add_test(NAME smoke COMMAND fmvibe-smoke $<TARGET_FILE:FMVibe>)

# Measures aliasing, zipper noise, clicks and the cost of oversampling on the engine.
add_executable(fmvibe-quality tests/quality.cpp)
target_link_libraries(fmvibe-quality PRIVATE fmvibe-core)
fmvibe_options(fmvibe-quality)
add_test(NAME quality COMMAND fmvibe-quality)

# Renders the editor offscreen into PNGs and replays a few mouse interactions.
add_executable(fmvibe-uishot tools/uishot.cpp)
target_link_libraries(fmvibe-uishot PRIVATE fmvibe-core windowscodecs ole32)
fmvibe_options(fmvibe-uishot)

# Both keep whole engines or editors (models and views) on the stack, a few at a time, which an unoptimized
# build doesn't overlap: room for them beyond Windows' default megabyte.
if (MSVC)
    target_link_options(fmvibe-quality PRIVATE /STACK:8388608)
    target_link_options(fmvibe-uishot PRIVATE /STACK:8388608)
endif()
add_test(NAME ui COMMAND fmvibe-uishot ${CMAKE_CURRENT_BINARY_DIR}/ui-shots)

# clap-validator (free-audio) runs the plugin through the spec conformance
# tests: `cmake --build --preset release --target validate`.
option(FMVIBE_FETCH_VALIDATOR "Download a prebuilt clap-validator" ON)
if (FMVIBE_FETCH_VALIDATOR)
    FetchContent_Declare(clap_validator
        URL https://github.com/free-audio/clap-validator/releases/download/0.4.1/clap-validator-0.4.1-127-g152b982-windows.zip
        URL_HASH SHA256=d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10
    )
    FetchContent_MakeAvailable(clap_validator)
endif()
find_program(CLAP_VALIDATOR clap-validator HINTS ${clap_validator_SOURCE_DIR})
if (CLAP_VALIDATOR)
    add_custom_target(validate
        COMMAND ${CLAP_VALIDATOR} validate $<TARGET_FILE:FMVibe>
        DEPENDS FMVibe
        USES_TERMINAL
    )
endif()
