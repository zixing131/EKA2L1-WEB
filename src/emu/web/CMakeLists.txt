# ============================================================================
# EKA2L1 WebAssembly Frontend Build
# ============================================================================

# The web frontend executable
add_executable(eka2l1_wasm
    src/main.cpp
)

target_include_directories(eka2l1_wasm PUBLIC include)

# Link against all core emulator libraries
target_link_libraries(eka2l1_wasm PRIVATE
    common
    config
    cpu
    disasm
    dispatch
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
    # Emscripten linker flags for WASM output
    target_link_options(eka2l1_wasm PRIVATE
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MAXIMUM_MEMORY=1073741824"
        "SHELL:-s USE_SDL=2"
        "SHELL:-s USE_OPENGL=3"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s USE_FREETYPE=1"
        "SHELL:-s USE_PTHREADS=1"
        "SHELL:-s PTHREAD_POOL_SIZE=4"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=[\"FS\",\"ccall\",\"cwrap\",\"getValue\",\"setValue\",\"UTF8ToString\",\"stringToUTF8\",\"callMain\"]"
        "SHELL:-s EXPORT_NAME=createEKA2L1Module"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "SHELL:-s ENVIRONMENT=web"
        "SHELL:-s WASM=1"
        "SHELL:-s GL_ASSERTIONS=0"
        "SHELL:-s GL_DEBUG=0"
        "SHELL:-s ALLOW_TABLE_GROWTH=1"
        "SHELL:-s DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=[\"$autoResumeAudioContext\"]"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:-s ASYNCIFY=1"
        "SHELL:-s ASYNCIFY_IMPORTS=[\"asyncify_sleep\",\"asyncify_main_loop\"]"
        "SHELL:--preload-file resources@resources"
        "SHELL:--preload-file miscs@miscs"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html"
        -O2
        -g2
        "--closure=0"
    )

    set_target_properties(eka2l1_wasm PROPERTIES
        OUTPUT_NAME "eka2l1"
        SUFFIX ".js"
    )

    # Copy resources to build directory for Emscripten's file packaging
    add_custom_command(TARGET eka2l1_wasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:eka2l1_wasm>/resources"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:eka2l1_wasm>/resources/upscale"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:eka2l1_wasm>/miscs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/gl/" "$<TARGET_FILE_DIR:eka2l1_wasm>/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/upscale/" "$<TARGET_FILE_DIR:eka2l1_wasm>/resources/upscale/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.hsb" "$<TARGET_FILE_DIR:eka2l1_wasm>/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../drivers/resources/defaultbank.sf2" "$<TARGET_FILE_DIR:eka2l1_wasm>/resources/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/miscs/" "$<TARGET_FILE_DIR:eka2l1_wasm>/miscs/"
    )
endif()
