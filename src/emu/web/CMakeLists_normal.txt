# ============================================================================
# EKA2L1 WebAssembly Frontend Build
# ============================================================================

# Test build (build_wasm_test) defines this to skip all copyright/integrity/
# domain protection. Release (build_wasm_release) leaves it OFF for full
# protection. See src/protection.cpp and gen_integrity.py.
option(EKA2L1_DEBUG_BUILD "Skip web copyright/integrity protection (debug web build)" OFF)

# The web frontend executable
add_executable(eka2l1_wasm
    src/main.cpp
    src/platform_stubs.cpp
    src/web_audio.cpp
    src/protection.cpp
)

target_include_directories(eka2l1_wasm PUBLIC include)

# Build timestamp baked into the watermark / build info. Stamped at configure
# time; protection.cpp falls back to __DATE__/__TIME__ if this is absent.
string(TIMESTAMP EKA2L1_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build_stamp.h"
    "#pragma once\n#define BUILD_TIMESTAMP \"${EKA2L1_BUILD_TIMESTAMP}\"\n")
target_include_directories(eka2l1_wasm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

if (EKA2L1_DEBUG_BUILD)
    target_compile_definitions(eka2l1_wasm PRIVATE EKA2L1_DEBUG_BUILD)
endif()

# Link against all core emulator libraries
target_link_libraries(eka2l1_wasm PRIVATE
    common
    config
    cpu
    disasm
    epocdispatch
    drivers
    epoc
    epockern
    epocservs
    epocpkg
    epocloader
    epocio
    epocldd
    gdbstub
    bridge
    miniz
    FAT
    pugixml
    stb
    yaml-cpp
)

# Set output directory
set_target_properties(eka2l1_wasm PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if (EMSCRIPTEN)
    set(WASM_STAGE_DIR "${CMAKE_BINARY_DIR}/bin")

    # Stage resources before linking so Emscripten file_packager can find them
    add_custom_command(TARGET eka2l1_wasm PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_STAGE_DIR}/resources/upscale"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_STAGE_DIR}/miscs"
        # WebGL2 == GLES3: must ship the "#version 300 es" shader set, the
        # desktop "gl/" set (#version 140) fails to compile in the browser.
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/gles/" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/upscale/" "${WASM_STAGE_DIR}/resources/upscale/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.hsb" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.sf2" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/miscs/" "${WASM_STAGE_DIR}/miscs/"
        # HLE patch libraries (prebuilt Symbian DLLs + .map files). The desktop
        # build stages these into bin/patch; without them the guest runs the
        # ROM's original scdv/mediaclient/etc., whose hardware paths the
        # emulator only partially implements (e.g. DSA games died at startup).
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_STAGE_DIR}/patch"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/avkonfep/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/audiooutputrouting/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/scdv/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/ecam/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/vibractrl/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/backlightctrl/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/mediaclientaudio/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/mediaclientaudiostream/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/mediaclientvideo/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/postingsurfacefactory/group" "${WASM_STAGE_DIR}/patch/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/patch/goommonitor/group" "${WASM_STAGE_DIR}/patch/"
        # Glyph-fallback fonts (CJK): fbs loads host-side "fonts/" after the ROM
        # scan, so Chinese text renders even on firmware without CJK glyphs.
        # Wipe first so swapping the fallback font doesn't leave a stale copy in
        # the packaged data (copy_directory merges, it doesn't mirror).
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${WASM_STAGE_DIR}/fonts"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WASM_STAGE_DIR}/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" "${WASM_STAGE_DIR}/fonts/"
        COMMENT "Staging resources for Emscripten file packaging"
    )

    # Emscripten linker flags for WASM output
    target_link_options(eka2l1_wasm PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        # 160MB start, grow on demand (in-place for shared memory, no copy).
        # iOS Safari kills the tab well under ~1.2GB commit, and a ROM install
        # peaks at baseline + RPKG in MEMFS + extracted Z drive + IndexedDB
        # sync copies. 256MB was sized for the old 131MB dyncom translation
        # arena; with that trimmed to 32MB on WASM the same headroom fits in
        # 160MB, leaving ~100MB more for the install spike.
        "SHELL:-s INITIAL_MEMORY=268435456"
        # 1GB cap: heavy runs peak well under 400MB, and a smaller reservation
        # is friendlier to iOS Safari's shared-memory accounting.
        "SHELL:-s MAXIMUM_MEMORY=1073741824"
        "SHELL:-s STACK_SIZE=4194304"
        "SHELL:-s USE_SDL=2"
        "SHELL:-s FULL_ES3=0"
        "SHELL:-s USE_FREETYPE=1"
        "SHELL:-s USE_PTHREADS=1"
        # Pooled workers cost real memory on iOS (JS worker + wasm stack each).
        # Census after pruning the idle applist pool: ntimer + applist loader +
        # spdlog + slack = well under 8, and runtime creation still works for
        # stragglers since nothing blocks on spawn.
        "SHELL:-s PTHREAD_POOL_SIZE=1"
        # 1MB per worker stack (allocated from the wasm heap). The old 64KB
        # default overflowed; 1MB keeps 16x headroom over that crash without
        # 32MB+ of standing stacks.s
        "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1048576"
        # HEAPU8: the streaming RPKG installer copies picked-file chunks
        # directly into the wasm heap from JS.
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[\"FS\",\"ccall\",\"cwrap\",\"getValue\",\"setValue\",\"UTF8ToString\",\"stringToUTF8\",\"callMain\",\"HEAPU8\"]"
        # The dyncom wasm JIT installs runtime-compiled block functions into
        # the shared indirect table (wasmTable.grow in EM_JS).
        "SHELL:-s ALLOW_TABLE_GROWTH=1"
        "SHELL:-s EXPORT_NAME=createEKA2L1Module"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "SHELL:-lidbfs.js"
        "SHELL:-s ENVIRONMENT=web"
        "SHELL:-s WASM=1"
        "SHELL:-s GL_ASSERTIONS=0"
        "SHELL:-s GL_DEBUG=0"
        "SHELL:-s ALLOW_TABLE_GROWTH=1"
        "SHELL:-s DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=[\"$autoResumeAudioContext\"]"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:--preload-file ${WASM_STAGE_DIR}/resources@resources"
        "SHELL:--preload-file ${WASM_STAGE_DIR}/miscs@miscs"
        "SHELL:--preload-file ${WASM_STAGE_DIR}/patch@patch"
        "SHELL:--preload-file ${WASM_STAGE_DIR}/fonts@fonts"
        # Performance build: max link-time optimization.
        -O3
        "--closure=0"
    )

    # Profiling vs release link mode.
    #   -DEKA2L1_WASM_PROFILING=ON  keeps function names in the wasm so the
    #   browser profiler / DevTools flame chart shows real symbols (needed to
    #   find the next dyncom hotspot). Optimization stays at -O3 so the profile
    #   reflects the shipped code; only the symbol table is retained.
    #
    #   OFF (default) strips debug info and runtime assertions for the smallest,
    #   fastest shipping build. Trade-off: crash backtraces are bare address
    #   offsets.
    option(EKA2L1_WASM_PROFILING "Build the WASM frontend with profiling symbols" OFF)
    if (EKA2L1_WASM_PROFILING)
        target_link_options(eka2l1_wasm PRIVATE
            "SHELL:--profiling-funcs"
            "SHELL:-s ASSERTIONS=1"
        )
    else()
        target_link_options(eka2l1_wasm PRIVATE
            -g0
            "SHELL:-s ASSERTIONS=0"
        )
    endif()

    # Emit plain eka2l1.js (+ .wasm/.data); the UI is served from the static
    # two-page frontend in pages/ (index.html = library/installer,
    # run.html = player), which loads the module via <script src="eka2l1.js">.
    set_target_properties(eka2l1_wasm PROPERTIES
        OUTPUT_NAME "eka2l1"
        SUFFIX ".js"
    )

    # Copy the static frontend next to the build output and stamp asset
    # references with a content hash (?v=...) so browser/CDN caches refresh on
    # deploy. Runs on every build so page/script edits land without
    # reconfiguring.
    add_custom_command(TARGET eka2l1_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DPAGES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/pages
            -DSTAGE_DIR=${WASM_STAGE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/stamp_pages.cmake
        COMMENT "Copying web frontend pages"
    )

    # Release only: seal the file hashes into the wasm + write integrity.js.
    # Runs AFTER page stamping so it hashes the final (?v=-stamped) HTML.
    if (NOT EKA2L1_DEBUG_BUILD)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if (Python3_Interpreter_FOUND)
            set(EKA2L1_PYTHON ${Python3_EXECUTABLE})
        else()
            set(EKA2L1_PYTHON python3)
        endif()
        add_custom_command(TARGET eka2l1_wasm POST_BUILD
            COMMAND ${EKA2L1_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/gen_integrity.py
                --stage ${WASM_STAGE_DIR}
            COMMENT "Sealing integrity hashes into eka2l1.wasm (Release)"
        )
    endif()
endif()
