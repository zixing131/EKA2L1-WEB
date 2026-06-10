# ============================================================================
# EKA2L1 WebAssembly Frontend Build
# ============================================================================

# The web frontend executable
add_executable(eka2l1_wasm
    src/main.cpp
    src/platform_stubs.cpp
    src/web_audio.cpp
)

target_include_directories(eka2l1_wasm PUBLIC include)

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
        COMMENT "Staging resources for Emscripten file packaging"
    )

    # Emscripten linker flags for WASM output
    target_link_options(eka2l1_wasm PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        # 256MB start, grow on demand. iOS Safari kills the tab around ~1.2GB
        # commit; a 512MB upfront heap + a ROM install (whole-file buffers +
        # extraction + IndexedDB sync) blew past that and the page "refreshed"
        # mid-install, leaving a half-persisted device.
        "SHELL:-s INITIAL_MEMORY=268435456"
        "SHELL:-s MAXIMUM_MEMORY=2147483648"
        "SHELL:-s STACK_SIZE=8388608"
        "SHELL:-s USE_SDL=2"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s USE_FREETYPE=1"
        "SHELL:-s USE_PTHREADS=1"
        # Pooled workers cost real memory on iOS (JS worker + wasm stack each);
        # 16 covers the emulator's steady-state thread count, and runtime
        # creation still works for stragglers since nothing blocks on spawn.
        "SHELL:-s PTHREAD_POOL_SIZE=16"
        # 1MB per worker stack (allocated from the wasm heap). The old 64KB
        # default overflowed; 1MB keeps 16x headroom over that crash without
        # 32MB+ of standing stacks.
        "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1048576"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[\"FS\",\"ccall\",\"cwrap\",\"getValue\",\"setValue\",\"UTF8ToString\",\"stringToUTF8\",\"callMain\"]"
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

    # Copy the static frontend next to the build output. Runs on every build
    # so page/script edits land without reconfiguring.
    add_custom_command(TARGET eka2l1_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/pages" "${WASM_STAGE_DIR}"
        COMMENT "Copying web frontend pages"
    )
endif()
