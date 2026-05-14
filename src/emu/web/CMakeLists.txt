# ============================================================================
# EKA2L1 WebAssembly Frontend Build
# ============================================================================

# The web frontend executable
add_executable(eka2l1_wasm
    src/main.cpp
    src/platform_stubs.cpp
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
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/gl/" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/upscale/" "${WASM_STAGE_DIR}/resources/upscale/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.hsb" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.sf2" "${WASM_STAGE_DIR}/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/miscs/" "${WASM_STAGE_DIR}/miscs/"
        COMMENT "Staging resources for Emscripten file packaging"
    )

    # Emscripten linker flags for WASM output
    target_link_options(eka2l1_wasm PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=536870912"
        "SHELL:-s MAXIMUM_MEMORY=2147483648"
        "SHELL:-s STACK_SIZE=8388608"
        "SHELL:-s USE_SDL=2"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s USE_FREETYPE=1"
        "SHELL:-s USE_PTHREADS=1"
        "SHELL:-s PTHREAD_POOL_SIZE=32"
        "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=2097152"
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
        -O2
        -g2
        "--closure=0"
        "SHELL:-s ASSERTIONS=1"
    )

    set_target_properties(eka2l1_wasm PROPERTIES
        OUTPUT_NAME "eka2l1"
        SUFFIX ".html"
    )

    # Emscripten processes {{{ SCRIPT }}} in the shell template to produce the final HTML
    target_link_options(eka2l1_wasm PRIVATE
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
    )
endif()
