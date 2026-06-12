# Post-build page stamping: copy pages/ into the stage dir, then rewrite
# script/asset references with a content-derived version (?v=<hash>) so static
# hosting + browser caches pick up new builds without a manual hard refresh.
# The version is the truncated SHA256 of eka2l1.wasm + eka2l1.data + every
# file in pages/, so JS/CSS-only changes also get a fresh tag (binaries-only
# hashing once shipped a JS fix that returning visitors never received).
#
# Usage: cmake -DPAGES_DIR=... -DSTAGE_DIR=... -P stamp_pages.cmake

if(NOT PAGES_DIR OR NOT STAGE_DIR)
    message(FATAL_ERROR "stamp_pages.cmake needs -DPAGES_DIR and -DSTAGE_DIR")
endif()

file(COPY "${PAGES_DIR}/" DESTINATION "${STAGE_DIR}")

set(BUILD_ID "dev")
if(EXISTS "${STAGE_DIR}/eka2l1.wasm")
    file(SHA256 "${STAGE_DIR}/eka2l1.wasm" WASM_HASH)
    set(DATA_HASH "")
    if(EXISTS "${STAGE_DIR}/eka2l1.data")
        file(SHA256 "${STAGE_DIR}/eka2l1.data" DATA_HASH)
    endif()
    set(PAGES_HASH "")
    file(GLOB_RECURSE PAGE_FILES LIST_DIRECTORIES false "${PAGES_DIR}/*")
    list(SORT PAGE_FILES)
    foreach(pf ${PAGE_FILES})
        file(SHA256 "${pf}" PF_HASH)
        string(APPEND PAGES_HASH "${PF_HASH}")
    endforeach()
    string(SHA256 COMBINED_HASH "${WASM_HASH}${DATA_HASH}${PAGES_HASH}")
    string(SUBSTRING "${COMBINED_HASH}" 0 12 BUILD_ID)
endif()

# Expose the id to boot.js (locateFile appends it to .wasm/.data requests).
file(WRITE "${STAGE_DIR}/js/build_id.js" "window.EKA2L1_BUILD_ID='${BUILD_ID}';\n")

# Stamp every page that loads the emulator scripts.
file(GLOB STAMP_PAGES "${STAGE_DIR}/*.html")
foreach(page ${STAMP_PAGES})
    file(READ "${page}" content)
    string(REPLACE "src=\"eka2l1.js\"" "src=\"eka2l1.js?v=${BUILD_ID}\"" content "${content}")
    string(REPLACE "src=\"js/boot.js\"" "src=\"js/boot.js?v=${BUILD_ID}\"" content "${content}")
    string(REPLACE "src=\"js/index.js\"" "src=\"js/index.js?v=${BUILD_ID}\"" content "${content}")
    string(REPLACE "src=\"js/run.js\"" "src=\"js/run.js?v=${BUILD_ID}\"" content "${content}")
    string(REPLACE "src=\"js/build_id.js\"" "src=\"js/build_id.js?v=${BUILD_ID}\"" content "${content}")
    string(REPLACE "href=\"css/app.css\"" "href=\"css/app.css?v=${BUILD_ID}\"" content "${content}")
    file(WRITE "${page}" "${content}")
endforeach()

message(STATUS "Stamped web pages with build id ${BUILD_ID}")
