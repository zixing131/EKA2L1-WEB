cmake_minimum_required(VERSION 3.13)

# ============================================================================
# EKA2L1 WebAssembly Port - Emscripten Build Configuration
# ============================================================================
# Usage:
#   emcmake cmake -C CMakeLists_wasm.cmake -B build_wasm .
#   emcmake cmake --build build_wasm
# ============================================================================

project(EKA2L1_WASM)

set(CMAKE_CXX_STANDARD 20)

# ---- Emscripten-specific settings ----
set(EMSCRIPTEN ON)
set(ANDROID OFF)

# Output to single HTML file with embedded WASM
set(CMAKE_EXECUTABLE_SUFFIX ".js")

# Tell Emscripten to generate HTML
if(NOT EMSCRIPTEN_GENERATE_HTML)
    set(EMSCRIPTEN_GENERATE_HTML ON)
endif()

# ---- WASM Platform Definition ----
add_definitions(-DEKA2L1_PLATFORM_WASM=1)

# Disable platform-specific features that won't work in browser
set(EKA2L1_BUILD_TOOLS OFF CACHE BOOL "Disable tools for WASM" FORCE)
set(EKA2L1_BUILD_TESTS OFF CACHE BOOL "Disable tests for WASM" FORCE)
set(EKA2L1_ENABLE_SCRIPTING_ABILITY OFF CACHE BOOL "Disable scripting for WASM" FORCE)
set(EKA2L1_ENABLE_UNEXPECTED_EXCEPTION_HANDLER OFF CACHE BOOL "" FORCE)
set(EKA2L1_BUILD_VULKAN_BACKEND OFF CACHE BOOL "" FORCE)
set(EKA2L1_DEPLOY_DMG OFF CACHE BOOL "" FORCE)
set(EKA2L1_BUILD_PATCH OFF CACHE BOOL "" FORCE)
set(EKA2L1_ENABLE_DISCORD_RICH_PRESENCE OFF CACHE BOOL "" FORCE)

# Force the dyncom interpreter (dynarmic JIT requires x86 host)
set(ARCHITECTURE_ARM32 OFF CACHE BOOL "" FORCE)
set(ARCHITECTURE_AARCH64 OFF CACHE BOOL "" FORCE)

# ---- Emscripten Linker Flags ----
# -s ALLOW_MEMORY_GROWTH=1: Allow dynamic memory growth
# -s MAXIMUM_MEMORY=1GB: Maximum WASM memory
# -s USE_SDL=2: Use Emscripten's SDL2 port
# -s USE_OPENGL=3: Enable WebGL2 (OpenGL ES 3.0)  
# -s FULL_ES3=1: Full OpenGL ES 3.0 emulation
# -s USE_FREETYPE=1: Use Emscripten's freetype port
# -s USE_PTHREADS=1: Enable threading (SharedArrayBuffer required)
# -sPTHREAD_POOL_SIZE=4: Thread pool size
# -s EXPORTED_RUNTIME_METHODS: Export useful runtime methods
# -s EXPORT_NAME: Module name
# -s MODULARIZE: Generate modularized output
# -s ASYNCIFY: Enable async emulation for main loop
# --shell-file: Custom HTML shell
# -s MINIMAL_RUNTIME=0: Use standard runtime for more features
# -s WASM=1: Generate WASM output
# -s GL_ASSERTIONS=0: Disable GL assertions for performance
# -s GL_DEBUG=0: Disable GL debug for performance
# -s FORCE_FILESYSTEM=1: Force Emscripten filesystem
# -s NODERAWFS=0: Don't use Node.js raw filesystem
# -s ENVIRONMENT=web: Target web environment
# -s ALLOW_TABLE_GROWTH: Allow table growth for dynamic dispatch

set(EMSCRIPTEN_LINKER_FLAGS
    "-s ALLOW_MEMORY_GROWTH=1"
    "-s MAXIMUM_MEMORY=1073741824"
    "-s USE_SDL=2"
    "-s USE_OPENGL=3"
    "-s FULL_ES3=1"
    "-s USE_FREETYPE=1"
    "-s USE_PTHREADS=1"
    "-s PTHREAD_POOL_SIZE=4"
    "-s EXPORTED_RUNTIME_METHODS=['FS','ccall','cwrap','getValue','setValue','UTF8ToString','stringToUTF8','callMain']"
    "-s EXPORT_NAME='createEKA2L1Module'"
    "-s MODULARIZE=1"
    "-s FORCE_FILESYSTEM=1"
    "-s ENVIRONMENT='web'"
    "-s WASM=1"
    "-s GL_ASSERTIONS=0"
    "-s GL_DEBUG=0"
    "-s ALLOW_TABLE_GROWTH=1"
    "-s DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=['$autoResumeAudioContext']"
    "-s MIN_WEBGL_VERSION=2"
    "-s MAX_WEBGL_VERSION=2"
    "--preload-file resources@resources"
    "--preload-file miscs@miscs"
    "--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/src/emu/web/shell.html"
    "-O2"
    "-g2"
    "--closure=0"
    "-s ASYNCIFY=1"
    "-s ASYNCIFY_IMPORTS=['asyncify_sleep','asyncify_main_loop']"
)

# ---- Compiler flags ----
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-warn-absolute-paths")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-warn-absolute-paths -Wno-invalid-offsetof")