# Writes version.txt into the demo's site: a hash of every file beside it. index.html fetches it past the
# browser's cache and then asks for each file with it (style.css?v=..., main.js?v=... and so on), so a new build
# is new URLs throughout and a browser never mixes files from two builds, whatever it kept.
#
#   cmake -DSITE=<site dir> -P version.cmake    (once the modules and the page are in place)

file(GLOB files RELATIVE ${SITE} ${SITE}/*)
list(REMOVE_ITEM files version.txt)
list(SORT files)
set(hashes "")
foreach(name IN LISTS files)
    if(NOT IS_DIRECTORY ${SITE}/${name})
        file(SHA256 ${SITE}/${name} hash)
        string(APPEND hashes "${name} ${hash}\n")
    endif()
endforeach()
string(SHA256 version "${hashes}")
string(SUBSTRING ${version} 0 16 version)

set(out ${SITE}/version.txt)
if(EXISTS ${out})
    file(READ ${out} old)
endif()
if(NOT old STREQUAL "${version}\n")
    file(WRITE ${out} "${version}\n")
endif()
