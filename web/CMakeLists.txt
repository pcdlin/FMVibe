# The browser demo: the engine and the editor as two WebAssembly modules, and the page around them, all in
# build/web/site. The audio side runs in an AudioWorklet, the editor on the page's main thread; they only send
# each other messages, so the page needs no cross-origin isolation and any static host can serve it.

set(site ${CMAKE_BINARY_DIR}/site)
list(TRANSFORM FMVIBE_ENGINE_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/ OUTPUT_VARIABLE engine)

function(fmvibe_web_options target)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${target} PRIVATE clap)
    target_compile_options(${target} PRIVATE -msimd128 -fno-exceptions -Wall)
    # The plugin's threads have the MSVC default of 1 MB of stack; Emscripten's is far smaller.
    target_link_options(${target} PRIVATE -msimd128 -fno-exceptions -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=1048576)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${site})
endfunction()

# The audio side, loaded by site/worklet.js without Emscripten's JS: a standalone module with C exports.
add_executable(fmvibe-dsp dsp.cpp ${engine})
fmvibe_web_options(fmvibe-dsp)
target_link_options(fmvibe-dsp PRIVATE -sSTANDALONE_WASM --no-entry)
set_target_properties(fmvibe-dsp PROPERTIES SUFFIX ".wasm")

# The editor: the plugin's View, drawing on the page's canvas through src/gui/canvas_web.cpp.
add_executable(fmvibe-ui
    ui.cpp
    ${engine}
    ${PROJECT_SOURCE_DIR}/src/gui/canvas_web.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/controller.cpp
    ${PROJECT_SOURCE_DIR}/src/gui/view.cpp
)
fmvibe_web_options(fmvibe-ui)
target_link_options(fmvibe-ui PRIVATE
    -sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createFMVibeUI -sENVIRONMENT=web
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF64,UTF8ToString
)
set_target_properties(fmvibe-ui PROPERTIES SUFFIX ".js")

# The page, next to the modules.
set(site_files index.html style.css main.js canvas2d.js worklet.js)
list(TRANSFORM site_files PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/site/ OUTPUT_VARIABLE site_sources)
add_custom_target(fmvibe-site ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${site}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${site_sources} ${site}
    DEPENDS ${site_sources}
    COMMENT "Copying the demo page"
)
