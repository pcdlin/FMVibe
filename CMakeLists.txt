cmake_minimum_required(VERSION 3.25)
project(FMVibe VERSION 0.4 LANGUAGES CXX)

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

# The sound engine, independent of the plugin formats and Windows: the plugins and the browser demo share it.
set(FMVIBE_ENGINE_SOURCES
    src/analyzer.cpp
    src/analyzer.h
    src/effects.cpp
    src/effects.h
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
    src/zerobuffer.cpp
    src/zerobuffer.h
    src/wavetable.cpp
    src/wavetable.h
)

# The factory presets, compiled in: fmvibe_factory_presets(out) sets `out` to a .cpp in the current binary folder that
# defines factoryPresets() (src/factory.h) from presets/factory.txt and the files it names, rebuilt when one changes.
# Each directory whose targets compile it calls this once, since a generated source's rule belongs to its directory.
set(FMVIBE_PRESET_LIST ${PROJECT_SOURCE_DIR}/presets/factory.txt)
file(GLOB FMVIBE_PRESET_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/presets/Factory/*.fmv)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FMVIBE_PRESET_LIST})
function(fmvibe_factory_presets out)
    set(generated ${CMAKE_CURRENT_BINARY_DIR}/factory_presets.cpp)
    add_custom_command(
        OUTPUT ${generated}
        COMMAND ${CMAKE_COMMAND} -DLIST=${FMVIBE_PRESET_LIST} -DDIR=${PROJECT_SOURCE_DIR}/presets/Factory
                -DOUTPUT=${generated} -P ${PROJECT_SOURCE_DIR}/cmake/embed_presets.cmake
        DEPENDS ${FMVIBE_PRESET_LIST} ${FMVIBE_PRESET_FILES} ${PROJECT_SOURCE_DIR}/cmake/embed_presets.cmake
        COMMENT "Building the factory presets in"
        VERBATIM
    )
    set(${out} ${generated} PARENT_SCOPE)
endfunction()

# The browser demo (web/), built with Emscripten: `.\build-web.ps1`.
if (EMSCRIPTEN)
    add_subdirectory(web)
    return()
endif()

if (NOT WIN32)
    message(FATAL_ERROR "FMVibe's editor is built on Win32 and Direct2D; only Windows is supported for now.")
endif()

# A .clap is a plain DLL with another extension, and so is the DLL in a .vst3 bundle. Link the CRT statically so
# the plugins load on machines without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The VST3 SDK's interfaces (MIT licensed), pinned by tag and hash: the VST3 plugin is written straight against them.
# Their headers include each other as pluginterfaces/..., hence the folder they land in.
FetchContent_Declare(vst3_pluginterfaces
    URL https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v3.8.1_build_84.tar.gz
    URL_HASH SHA256=19e88e225b6ecb4a3c429d3ff2fd511be9c6c8b8b1c7e86b05949b45c0553e2d
    SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vst3sdk/pluginterfaces
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(vst3_pluginterfaces)

function(fmvibe_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endif()
endfunction()

# Engine and editor view, shared by the plugin and the tools.
fmvibe_factory_presets(factory_presets)
add_library(fmvibe-core STATIC
    ${FMVIBE_ENGINE_SOURCES}
    ${factory_presets}
    src/factory.cpp
    src/factory.h
    src/gui/canvas.cpp
    src/gui/canvas.h
    src/gui/controller.cpp
    src/gui/controller.h
    src/gui/dialogs.cpp
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
target_link_libraries(fmvibe-core PUBLIC clap d2d1 dwrite)
fmvibe_options(fmvibe-core)

# What the CLAP and the VST3 plugin share beyond the engine and the view: the plugin core (params, state, the
# editor's Controller), the preset library, the user's settings and the editor's window. Each plugin links its own
# copy, so the editor's window class belongs to that plugin's DLL.
add_library(fmvibe-plugin STATIC
    src/plugincore.cpp
    src/plugincore.h
    src/presets.cpp
    src/presets.h
    src/usersettings.cpp
    src/usersettings.h
    src/gui/editor.cpp
    src/gui/editor.h
)
target_link_libraries(fmvibe-plugin PUBLIC fmvibe-core)
fmvibe_options(fmvibe-plugin)

add_library(FMVibe MODULE
    src/entry.cpp
    src/plugin.cpp
    src/plugin.h
)
target_link_libraries(FMVibe PRIVATE fmvibe-plugin)
set_target_properties(FMVibe PROPERTIES PREFIX "" SUFFIX ".clap")
fmvibe_options(FMVibe)

# The VST3 interfaces' ids and FUID, which the plugin and its test host both need.
add_library(vst3-interfaces STATIC
    ${vst3_pluginterfaces_SOURCE_DIR}/base/coreiids.cpp
    ${vst3_pluginterfaces_SOURCE_DIR}/base/funknown.cpp
    src/vst3iids.cpp
)
target_include_directories(vst3-interfaces PUBLIC ${vst3_pluginterfaces_SOURCE_DIR}/..)
target_compile_features(vst3-interfaces PUBLIC cxx_std_20)
target_compile_definitions(vst3-interfaces PUBLIC NOMINMAX UNICODE _UNICODE)
target_link_libraries(vst3-interfaces PUBLIC ole32)

# The VST3 plugin: a bundle, FMVibe.vst3, holding the DLL (named like the bundle) for 64-bit Windows.
set(FMVIBE_VST3_BUNDLE ${CMAKE_CURRENT_BINARY_DIR}/FMVibe.vst3)
add_library(FMVibe-vst3 MODULE
    src/vst3entry.cpp
    src/vst3plugin.cpp
    src/vst3plugin.h
)
target_link_libraries(FMVibe-vst3 PRIVATE fmvibe-plugin vst3-interfaces)
set_target_properties(FMVibe-vst3 PROPERTIES
    OUTPUT_NAME FMVibe
    PREFIX ""
    SUFFIX ".vst3"
    LIBRARY_OUTPUT_DIRECTORY ${FMVIBE_VST3_BUNDLE}/Contents/x86_64-win
    # Its import library stays out of the bundle, and clear of the CLAP plugin's of the same name.
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/vst3-implib
)
fmvibe_options(FMVibe-vst3)

# `cmake --install` puts the plugins into <prefix>/CLAP and <prefix>/VST3. The presets set the prefix to
# %LOCALAPPDATA%/Programs/Common, which holds the per-user CLAP and VST3 folders. Each is a component of its own, so
# one can go alone: `cmake --install build\release --component clap` (or vst3).
install(TARGETS FMVibe LIBRARY DESTINATION CLAP COMPONENT clap)
install(TARGETS FMVibe-vst3 LIBRARY DESTINATION VST3/FMVibe.vst3/Contents/x86_64-win COMPONENT vst3)

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

# Headless VST3 host: loads the built FMVibe.vst3, checks what the VST3 side adds (factory, buses, params, MIDI
# mapping, note expressions, state, editor) and plays notes through it.
add_executable(fmvibe-vst3-smoke tests/vst3_host.cpp)
# The core for param ids and to write a state of its own; everything else goes through VST3.
target_link_libraries(fmvibe-vst3-smoke PRIVATE fmvibe-core vst3-interfaces)
fmvibe_options(fmvibe-vst3-smoke)
add_dependencies(fmvibe-vst3-smoke FMVibe-vst3)
add_test(NAME vst3 COMMAND fmvibe-vst3-smoke $<TARGET_FILE:FMVibe-vst3>)

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

# A small CLAP host for trying the editor by hand: loads the built plugin, opens its editor in a window and plays it
# through the default output device, with notes from MIDI inputs. Not a test: it waits for the window to close.
add_executable(fmvibe-uihost tools/uihost.cpp)
target_link_libraries(fmvibe-uihost PRIVATE clap ole32 winmm avrt)
fmvibe_options(fmvibe-uihost)
add_dependencies(fmvibe-uihost FMVibe)

# Makes edited preset files sparse again before they are committed (see presets/factory.txt).
add_executable(fmvibe-presettool tools/presettool.cpp)
target_link_libraries(fmvibe-presettool PRIVATE fmvibe-core)
fmvibe_options(fmvibe-presettool)

# The validators run the plugins through their formats' conformance tests:
# `cmake --build --preset release --target validate` runs both.
add_custom_target(validate)

# clap-validator (free-audio), for the CLAP plugin: `--target validate-clap`.
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
    add_custom_target(validate-clap
        COMMAND ${CLAP_VALIDATOR} validate $<TARGET_FILE:FMVibe>
        DEPENDS FMVibe
        USES_TERMINAL
    )
    add_dependencies(validate validate-clap)
endif()

# Steinberg's validator, for the VST3 plugin: `--target validate-vst3`. It comes as source only, in the VST3 SDK's
# base and public parts (MIT licensed too), built here with the SDK's own list of the validator's sources.
option(FMVIBE_FETCH_VST3_VALIDATOR "Download the VST3 SDK's sources and build its validator" ON)
if (FMVIBE_FETCH_VST3_VALIDATOR)
    FetchContent_Declare(vst3_base
        URL https://github.com/steinbergmedia/vst3_base/archive/refs/tags/v3.8.1_build_84.tar.gz
        URL_HASH SHA256=3ac62b2e271b87863ce50399b40549c204fc21065134247f87d8b84381692ced
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vst3sdk/base
        EXCLUDE_FROM_ALL
    )
    FetchContent_Declare(vst3_public_sdk
        URL https://github.com/steinbergmedia/vst3_public_sdk/archive/refs/tags/v3.8.1_build_84.tar.gz
        URL_HASH SHA256=f683db0c831dd6603a1bb8c7747a6e1dd223ac3423e288b08c5706cf30b76ae8
        SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/vst3sdk/public.sdk
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(vst3_base vst3_public_sdk)

    # What the validator's CMakeLists.txt expects of the SDK's build: its root, the hosting library and a few
    # helpers, which do nothing here.
    set(SDK_ROOT ${FETCHCONTENT_BASE_DIR}/vst3sdk)
    set(SMTG_WIN ON)
    function(smtg_target_codesign)
    endfunction()
    function(smtg_target_setup_universal_binary)
    endfunction()
    add_library(sdk_hosting STATIC EXCLUDE_FROM_ALL
        ${SDK_ROOT}/base/source/baseiids.cpp
        ${SDK_ROOT}/base/source/fbuffer.cpp
        ${SDK_ROOT}/base/source/fdebug.cpp
        ${SDK_ROOT}/base/source/fdynlib.cpp
        ${SDK_ROOT}/base/source/fobject.cpp
        ${SDK_ROOT}/base/source/fstreamer.cpp
        ${SDK_ROOT}/base/source/fstring.cpp
        ${SDK_ROOT}/base/source/timer.cpp
        ${SDK_ROOT}/base/source/updatehandler.cpp
        ${SDK_ROOT}/base/thread/source/fcondition.cpp
        ${SDK_ROOT}/base/thread/source/flock.cpp
        ${SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
        ${SDK_ROOT}/pluginterfaces/base/coreiids.cpp
        ${SDK_ROOT}/pluginterfaces/base/funknown.cpp
        ${SDK_ROOT}/pluginterfaces/base/ustring.cpp
        ${SDK_ROOT}/public.sdk/source/common/commoniids.cpp
        ${SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
        ${SDK_ROOT}/public.sdk/source/common/threadchecker_win32.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
        ${SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp
        ${SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
        ${SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
        ${SDK_ROOT}/public.sdk/source/vst/vstpresetfile.cpp
    )
    target_include_directories(sdk_hosting PUBLIC ${SDK_ROOT})
    target_compile_features(sdk_hosting PUBLIC cxx_std_17)
    target_compile_definitions(sdk_hosting PUBLIC $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1> UNICODE _UNICODE)
    target_link_libraries(sdk_hosting PUBLIC ole32 shell32)
    add_subdirectory(${SDK_ROOT}/public.sdk/samples/vst-hosting/validator vst3-validator EXCLUDE_FROM_ALL)

    add_custom_target(validate-vst3
        COMMAND validator $<TARGET_FILE:FMVibe-vst3>
        DEPENDS validator FMVibe-vst3
        USES_TERMINAL
    )
    add_dependencies(validate validate-vst3)
endif()
