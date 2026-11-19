# The user manual: tools/manual.py turns docs/manual.md into one self-contained FMVibe-Manual.html (the release puts
# it into both zips and onto GitHub Pages). As a test it checks the manual too, failing on a link to a heading that
# isn't there, an image that isn't there or Markdown it can't convert, so the build catches what would otherwise
# stop a release. Without Python 3 the test is left out.

find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME manual
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/manual.py ${PROJECT_SOURCE_DIR}/docs/manual.md
                ${CMAKE_CURRENT_BINARY_DIR}/FMVibe-Manual.html)
else()
    message(STATUS "Python 3 not found: the user manual's check (the manual test) is left out")
endif()
