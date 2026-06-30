# EKA2L1 WebAssembly Port - Build Guide

This document describes how to build and run EKA2L1 as a WebAssembly application that runs in modern web browsers.

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                 Browser (Chrome/Firefox/Safari)  │
│  ┌───────────────────────────────────────────┐  │
│  │         HTML5 / JavaScript Shell          │  │
│  │    (shell.html - UI, file dialogs, etc.)  │  │
│  └───────────────┬───────────────────────────┘  │
│                  │                               │
│  ┌───────────────▼───────────────────────────┐  │
│  │       Emscripten Runtime (Module)          │  │
│  │   - Virtual Filesystem (MEMFS/IDBFS)      │  │
│  │   - WebGL2 Context                        │  │
│  │   - SDL2 Audio                            │  │
│  │   - SDL2 Events                           │  │
│  └───────────────┬───────────────────────────┘  │
│                  │                               │
│  ┌───────────────▼───────────────────────────┐  │
│  │          eka2l1.wasm                       │  │
│  │  ┌─────────────────────────────────────┐  │  │
│  │  │  Web Frontend (main.cpp)            │  │  │
│  │  │  - SDL2 Window/Input               │  │  │
│  │  │  - WebGL2 Graphics Driver          │  │  │
│  │  │  - SDL2 Audio Callback             │  │  │
│  │  ├─────────────────────────────────────┤  │  │
│  │  │  EKA2L1 Core                        │  │  │
│  │  │  - ARM Interpreter (dyncom)         │  │  │
│  │  │  - Symbian OS Services              │  │  │
│  │  │  - Graphics/Audio Drivers           │  │  │
│  │  │  - Loader (SIS, ROM, RPKG)          │  │  │
│  │  └─────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

## Prerequisites

### 1. Install Emscripten SDK

```bash
# Clone the Emscripten SDK
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Install the latest version
./emsdk install latest

# Activate the latest version
./emsdk activate latest

# Source environment variables
source ./emsdk_env.sh   # Linux/macOS
emsdk_env.bat           # Windows
```

### 2. Build Dependencies

Emscripten provides these ports automatically:
- **SDL2** (`-s USE_SDL=2`) - For windowing, input, and audio
- **FreeType** (`-s USE_FREETYPE=1`) - For font rendering
- **OpenGL ES 3.0** (`-s USE_OPENGL=3`) - WebGL2 graphics backend

## Building

### Quick Build (Linux/macOS)

```bash
# Make sure emscripten is activated
source /path/to/emsdk/emsdk_env.sh

# Configure with emcmake wrapper
emcmake cmake -B build_wasm \
    -DCMAKE_BUILD_TYPE=Release \
    -DEKA2L1_BUILD_TOOLS=OFF \
    -DEKA2L1_BUILD_TESTS=OFF \
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF \
    -DEKA2L1_BUILD_VULKAN_BACKEND=OFF \
    -DEKA2L1_ENABLE_DISCORD_RICH_PRESENCE=OFF

# Build
emcmake cmake --build build_wasm -j$(nproc)
```

### Quick Build (Windows)

```cmd
:: Activate Emscripten
call C:\path\to\emsdk\emsdk_env.bat

:: Configure
emcmake cmake -B build_wasm ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DEKA2L1_BUILD_TOOLS=OFF ^
    -DEKA2L1_BUILD_TESTS=OFF ^
    -DEKA2L1_ENABLE_SCRIPTING_ABILITY=OFF ^
    -DEKA2L1_BUILD_VULKAN_BACKEND=OFF ^
    -DEKA2L1_ENABLE_DISCORD_RICH_PRESENCE=OFF

:: Build
emcmake cmake --build build_wasm --config Release
```

### Build Output

After a successful build, you'll find in `build_wasm/bin/`:
- `eka2l1.js` - JavaScript loader/glue code
- `eka2l1.wasm` - WebAssembly binary (the compiled emulator)
- `eka2l1.html` - Full HTML page with embedded UI
- `eka2l1.data` - Preloaded filesystem data (resources, miscs)
- `eka2l1.worker.js` - Web Worker script (for threading)

## Running

### Local HTTP Server

WebAssembly requires serving from an HTTP server (not `file://`). Use any local server:

```bash
# Python 3
cd build_wasm/bin
python -m http.server 8080

# Node.js (npx)
npx serve build_wasm/bin

# PHP
php -S localhost:8080 -t build_wasm/bin
```

Then open `http://localhost:8080/eka2l1.html` in your browser.

### Browser Requirements

- **Chrome 89+** / **Edge 89+** (recommended)
- **Firefox 89+**
- **Safari 15.2+**

Required web APIs:
- WebGL 2.0
- SharedArrayBuffer (for threading)
- WebAssembly
- Fetch API

> **Note:** SharedArrayBuffer requires these HTTP headers:
> ```
> Cross-Origin-Opener-Policy: same-origin
> Cross-Origin-Embedder-Policy: require-corp
> ```

## Usage

1. **Open the page** - The WASM module will load automatically
2. **Load a ROM** - Click "📦 ROM" to load a Symbian ROM (.rom) or RPKG (.rpkg) file
3. **Install apps** - Click "📲 Install" to install SIS/SISX packages
4. **Run** - Click "▶ Run" to start emulation
5. **Settings** - Use the ⚙ button to adjust display scale, volume, etc.

## Key Technical Decisions

### CPU Backend: Dyncom (Interpreter)
The dynarmic JIT compiler requires x86/x64 host architecture, so the WASM port uses the `dyncom` interpreter backend. This is slower but fully portable.

### Graphics: WebGL 2.0 (OpenGL ES 3.0)
EKA2L1's existing OpenGL backend maps well to WebGL 2.0. The GLSL shaders are compatible with GLSL ES 3.00.

### Audio: SDL2 Audio
Emscripten's SDL2 audio implementation uses Web Audio API under the hood. Audio is handled through SDL2 callback mechanism.

### Filesystem: Emscripten MEMFS + JavaScript File API
- ROM, RPKG, and SIS files are loaded via browser file picker
- Written to Emscripten's in-memory filesystem (MEMFS)
- Optionally persisted to IndexedDB (IDBFS) for save data

### Threading: Web Workers + SharedArrayBuffer
EKA2L1 uses threads for the graphics driver, audio driver, and CPU emulation. These are mapped to Web Workers via Emscripten's pthreads implementation.

## Limitations

1. **Performance**: Interpreter-only CPU emulation is 2-5x slower than native JIT. Modern browsers with fast WebAssembly engines partially compensate.

2. **Threading**: Requires SharedArrayBuffer, which needs specific HTTP headers. Some hosting environments may not support this.

3. **File Access**: Files must be loaded through the browser's file picker API. No direct filesystem access.

4. **Memory**: WASM has a 4GB address space limit. Large ROMs or apps may exceed this.

5. **Bluetooth/Network**: No access to Bluetooth hardware. Network features are not available in the browser sandbox.

6. **GDB Stub**: Remote debugging via GDB is not available in the web version.

## Project Structure

```
src/emu/web/
├── CMakeLists.txt          # WASM build configuration
├── shell.html              # HTML5/JS frontend UI
├── src/
│   └── main.cpp            # Main entry point with SDL2/OpenGL
└── BUILDING_WASM.md        # This file
```

## Modified Files for WASM Support

| File | Change |
|------|--------|
| `src/emu/CMakeLists.txt` | Added `EMSCRIPTEN` condition to build web target |
| `src/emu/drivers/include/drivers/graphics/common.h` | Added `web` to `window_system_type` enum |
| `src/emu/common/include/common/platform.h` | Added `EKA2L1_PLATFORM_WASM` detection |
| `CMakeLists_wasm.cmake` | Emscripten-specific build cache file |

## Troubleshooting

### "SharedArrayBuffer is not defined"
Your server must send these headers:
```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

### "WebGL2 is not supported"
Use a modern browser. WebGL 2 is supported in all major browsers since 2017.

### Build fails with "dynarmic" errors
Make sure dynarmic is not being built. The WASM port should use dyncom interpreter. Set `-DARCHITECTURE_ARM32=OFF`.

### Out of memory errors
Increase `MAXIMUM_MEMORY` in `CMakeLists_wasm.cmake` or `src/emu/web/CMakeLists.txt` linker flags. The default is 1GB.

### Slow performance
- Use `Release` build type
- Ensure browser hardware acceleration is enabled
- Close other tabs to free memory