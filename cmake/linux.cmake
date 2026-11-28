# The Linux build: the engine and the editor over Cairo, the CLAP and VST3 plugins (the editor embedded in the host's
# X11 window, the dialogs the desktop's: zenity, else kdialog), the audio quality test, the UI snapshots and a small
# test host. The plugins' Windows halves are CMakeLists.txt's (Win32, Direct2D); this builds what runs without them.
# See "Linux development build" in AGENTS.md, and `./build.sh`.

# The VST3 interfaces (MIT licensed), pinned by tag and hash as CMakeLists.txt has them: the plugin is written straight
# against them. They come from VST3_PLUGINTERFACES_DIR where it is given (build.sh clones the tag there, as it does
# CLAP, since a cloud session's proxy refuses GitHub's archives), else the archive is downloaded here.
set(VST3_PLUGINTERFACES_DIR "" CACHE PATH "The vst3_pluginterfaces checkout (holding base/, gui/, vst/)")
if (NOT VST3_PLUGINTERFACES_DIR OR NOT EXISTS "${VST3_PLUGINTERFACES_DIR}/base/funknown.h")
    set(VST3_PLUGINTERFACES_DIR ${FETCHCONTENT_BASE_DIR}/vst3sdk/pluginterfaces)
    if (NOT EXISTS "${VST3_PLUGINTERFACES_DIR}/base/funknown.h")
        set(archive ${CMAKE_BINARY_DIR}/vst3_pluginterfaces.tar.gz)
        message(STATUS "Fetching the VST3 interfaces")
        file(DOWNLOAD
            https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v3.8.1_build_84.tar.gz
            ${archive}
            EXPECTED_HASH SHA256=19e88e225b6ecb4a3c429d3ff2fd511be9c6c8b8b1c7e86b05949b45c0553e2d)
        file(ARCHIVE_EXTRACT INPUT ${archive} DESTINATION ${FETCHCONTENT_BASE_DIR}/vst3_extract)
        file(RENAME ${FETCHCONTENT_BASE_DIR}/vst3_extract/vst3_pluginterfaces-3.8.1_build_84
             ${VST3_PLUGINTERFACES_DIR})
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-xlib)
find_package(X11 REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The plugins carry the C++ runtime they were built with, as the Windows ones carry the CRT: a distribution with an
# older GCC's libstdc++ than the one that built them still loads them, and the host's own copy is never bound (the
# version scripts keep it local).
set(FMVIBE_STATIC_RUNTIME -static-libstdc++ -static-libgcc)

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
    src/gui/rateshape.h
    src/gui/canvas_cairo.cpp
    src/gui/canvas.h
    src/gui/controller.cpp
    src/gui/controller.h
    src/gui/dialogs.cpp
    src/gui/dx7.cpp
    src/gui/dx7.h
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
    src/gui/patchedits.cpp
    src/gui/patchedits.h
    src/gui/tuningedits.cpp
    src/gui/tuningedits.h
    src/gui/presetbrowser.cpp
    src/gui/presetbrowser.h
    src/gui/routing.h
    src/gui/routingmodel.cpp
    src/gui/routingmodel.h
    src/gui/routingpanel.cpp
    src/gui/routingpanel.h
    src/gui/shapeeditor.cpp
    src/gui/shapeeditor.h
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
)
set_target_properties(fmvibe-core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(fmvibe-core PUBLIC src)
target_compile_definitions(fmvibe-core PUBLIC FMVIBE_CAIRO)
target_link_libraries(fmvibe-core PUBLIC clap PkgConfig::CAIRO)
fmvibe_options(fmvibe-core)

# What the CLAP and VST3 plugins share beyond the engine and the view: the plugin core (params, state, the editor's
# Controller), the preset library, the user's settings and the editor's window (an X11 child here).
add_library(fmvibe-plugin STATIC
    src/plugincore.cpp
    src/plugincore.h
    src/presets.cpp
    src/presets.h
    src/usersettings.cpp
    src/usersettings.h
    src/gui/editor.h
    src/gui/editor_x11.cpp
    src/mtstuning.cpp
    src/mtstuning.h
    third_party/mts-esp/libMTSClient.cpp
    third_party/mts-esp/libMTSClient.h
)
set_target_properties(fmvibe-plugin PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
# CMAKE_DL_LIBS: the MTS-ESP client opens libMTS with dlopen() (see third_party/mts-esp).
target_link_libraries(fmvibe-plugin PUBLIC fmvibe-core X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
fmvibe_options(fmvibe-plugin)
# ODDSound's MTS-ESP client, as it comes: its warnings are its own.
set_source_files_properties(third_party/mts-esp/libMTSClient.cpp PROPERTIES COMPILE_OPTIONS "-w")

# The CLAP plugin: a shared object, FMVibe.clap, that hosts load from a CLAP folder (~/.clap).
add_library(FMVibe MODULE
    src/entry.cpp
    src/plugin.cpp
    src/plugin.h
)
target_link_libraries(FMVibe PRIVATE fmvibe-plugin)
set_target_properties(FMVibe PROPERTIES PREFIX "" SUFFIX ".clap" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(FMVibe PRIVATE -Wl,--version-script=${PROJECT_SOURCE_DIR}/cmake/symbols-clap.map
    ${FMVIBE_STATIC_RUNTIME})
fmvibe_options(FMVibe)

# The VST3 interfaces' ids and FUID, which the plugin and its test host both need.
add_library(vst3-interfaces STATIC
    ${VST3_PLUGINTERFACES_DIR}/base/coreiids.cpp
    ${VST3_PLUGINTERFACES_DIR}/base/funknown.cpp
    src/vst3iids.cpp
)
target_include_directories(vst3-interfaces PUBLIC ${VST3_PLUGINTERFACES_DIR}/..)
target_compile_features(vst3-interfaces PUBLIC cxx_std_20)
target_link_libraries(vst3-interfaces PUBLIC Threads::Threads)

# The VST3 plugin: a bundle, FMVibe.vst3, holding the shared object (named like the bundle) for 64-bit Linux.
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
    SUFFIX ".so"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${FMVIBE_VST3_BUNDLE}/Contents/x86_64-linux
)
target_link_options(FMVibe-vst3 PRIVATE -Wl,--version-script=${PROJECT_SOURCE_DIR}/cmake/symbols-vst3.map
    ${FMVIBE_STATIC_RUNTIME})
fmvibe_options(FMVibe-vst3)

# The bundle's moduleinfo.json (VST SDK 3.7.8 on, in Contents/Resources): what a host scans without loading the module.
set(FMVIBE_MODULEINFO ${FMVIBE_VST3_BUNDLE}/Contents/Resources/moduleinfo.json)
file(WRITE ${FMVIBE_MODULEINFO} "{\n"
    "  \"Name\": \"FMVibe\",\n"
    "  \"Version\": \"${FMVIBE_VERSION_TEXT}\",\n"
    "  \"Factory Info\": { \"Vendor\": \"pcdlin\", \"URL\": \"https://github.com/pcdlin/FMVibe\", \"E-Mail\": \"\", "
    "\"Flags\": { \"Unicode\": true } },\n"
    "  \"Classes\": [\n"
    "    {\n"
    "      \"CID\": \"9BCBBCB9166D49F6A9C2271D5009BDD5\",\n"
    "      \"Category\": \"Audio Module Class\",\n"
    "      \"Name\": \"FMVibe\",\n"
    "      \"Vendor\": \"pcdlin\",\n"
    "      \"Version\": \"${FMVIBE_VERSION_TEXT}\",\n"
    "      \"SDKVersion\": \"VST 3.8.1\",\n"
    "      \"Sub Categories\": [\"Instrument\", \"Synth\"],\n"
    "      \"Class Flags\": 0,\n"
    "      \"Cardinality\": 2147483647\n"
    "    }\n"
    "  ]\n"
    "}\n")

# `cmake --install` puts them where a user's hosts look for them by hand: ~/.clap and ~/.vst3. `build.sh --install`
# does it, with the prefix set to $HOME.
install(TARGETS FMVibe LIBRARY DESTINATION .clap COMPONENT clap)
install(TARGETS FMVibe-vst3 LIBRARY DESTINATION .vst3/FMVibe.vst3/Contents/x86_64-linux COMPONENT vst3)
install(FILES ${FMVIBE_MODULEINFO} DESTINATION .vst3/FMVibe.vst3/Contents/Resources COMPONENT vst3)

enable_testing()
add_custom_target(validate) # `--target validate`: every conformance check this build has

add_executable(fmvibe-quality tests/quality.cpp)
target_link_libraries(fmvibe-quality PRIVATE fmvibe-core)
fmvibe_options(fmvibe-quality)
# Its checks keep dozens of param sets (and engines holding them) on the stack at once (see the MSVC build's
# /STACK in CMakeLists.txt), past Linux's usual 8 MB: run with the 64 MB the Windows build links in.
add_test(NAME quality COMMAND sh -c "ulimit -s 65536 2>/dev/null; exec \"$0\"" $<TARGET_FILE:fmvibe-quality>)

add_executable(fmvibe-uishot tools/uishot.cpp)
target_link_libraries(fmvibe-uishot PRIVATE fmvibe-core)
fmvibe_options(fmvibe-uishot)
# Likewise its editors and param sets: run with the 16 MB the Windows build links in, past Linux's usual 8 MB.
add_test(NAME ui COMMAND sh -c "ulimit -s 16384 2>/dev/null; exec \"$0\" \"$1\"" $<TARGET_FILE:fmvibe-uishot>
                         ${CMAKE_CURRENT_BINARY_DIR}/ui-shots)
include(cmake/manual.cmake)

# Steinberg's validator, on the VST3 bundle: `ctest -R vst3-validator`, or `--target validate`.
option(FMVIBE_LINUX_VST3_VALIDATOR "Build Steinberg's VST3 validator and run it as a test" ON)
if (FMVIBE_LINUX_VST3_VALIDATOR)
    include(cmake/vst3-validator.cmake)
endif()

add_executable(fmvibe-presettool tools/presettool.cpp)
target_link_libraries(fmvibe-presettool PRIVATE fmvibe-core)
fmvibe_options(fmvibe-presettool)

# A small headless host: loads the built .clap and the .vst3 bundle's object, plays a note through each, round-trips
# its state, and (where a display is there) opens the editor in a window of its own and drives it.
# A small MTS-ESP master for trying FMVibe's microtuning by hand (see tools/mtsmaster.cpp): ODDSound's master library,
# which needs libMTS in /usr/local/lib. Not a test: while it runs, it retunes every MTS-ESP client on the machine.
add_executable(fmvibe-mtsmaster tools/mtsmaster.cpp third_party/mts-esp/libMTSMaster.cpp third_party/mts-esp/libMTSMaster.h)
target_link_libraries(fmvibe-mtsmaster PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
fmvibe_options(fmvibe-mtsmaster)
set_source_files_properties(third_party/mts-esp/libMTSMaster.cpp PROPERTIES COMPILE_OPTIONS "-w")

add_executable(fmvibe-linux-host tests/linux_host.cpp)
# fmvibe-plugin for MtsTuning, which it checks on its own as well as through the plugins.
target_link_libraries(fmvibe-linux-host PRIVATE clap fmvibe-plugin vst3-interfaces X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
fmvibe_options(fmvibe-linux-host)
add_dependencies(fmvibe-linux-host FMVibe FMVibe-vst3)
add_test(NAME linux-host COMMAND fmvibe-linux-host $<TARGET_FILE:FMVibe>
         ${FMVIBE_VST3_BUNDLE}/Contents/x86_64-linux/FMVibe.so)
