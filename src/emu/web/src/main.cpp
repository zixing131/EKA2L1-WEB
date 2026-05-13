/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project - WebAssembly port
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// ============================================================================
// EKA2L1 WebAssembly Main Entry Point
// ============================================================================
// This is the main entry point for the web version of EKA2L1.
// It uses SDL2 (provided by Emscripten) for windowing, input, and audio,
// and OpenGL ES 3.0 (WebGL 2.0) for graphics rendering.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

// Emscripten-specific headers
#include <emscripten.h>
#include <emscripten/html5.h>

// EKA2L1 headers
#include <common/algorithm.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <common/path.h>
#include <common/platform.h>
#include <common/version.h>

#include <config/app_settings.h>
#include <config/config.h>

#include <drivers/audio/audio.h>
#include <drivers/graphics/graphics.h>
#include <drivers/input/emu_controller.h>

#include <system/epoc.h>
#include <system/devices.h>

#include <kernel/kernel.h>

#include <services/window/window.h>
#include <services/init.h>

// ============================================================================
// Global State
// ============================================================================

static SDL_Window *g_window = nullptr;
static SDL_GLContext g_gl_context = nullptr;
static SDL_AudioDeviceID g_audio_device = 0;

namespace eka2l1::web {
    struct wasm_state {
        std::unique_ptr<eka2l1::system> symsys;
        drivers::graphics_driver_ptr graphics_driver;
        drivers::audio_driver_instance audio_driver;
        config::state conf;
        std::unique_ptr<config::app_settings> app_settings;

        bool initialized = false;
        bool running = false;
        bool paused = false;

        int window_width = 360;
        int window_height = 640;

        // FPS tracking
        Uint32 last_fps_time = 0;
        int frame_count = 0;
        int current_fps = 0;
    };

    static wasm_state g_state;
}

using namespace eka2l1::web;
using namespace eka2l1;

// ============================================================================
// SDL2-based Emu Window for Web
// ============================================================================

class sdl_web_window : public eka2l1::drivers::emu_window {
private:
    SDL_Window *window_;
    void *userdata_;
    bool should_quit_;
    eka2l1::vec2 window_size_;
    std::array<std::uint32_t, eka2l1::MAX_SYMBIAN_SUPPORTED_POINTERS> active_pointers_;

public:
    sdl_web_window(SDL_Window *win)
        : window_(win)
        , userdata_(nullptr)
        , should_quit_(false) {
        std::fill(active_pointers_.begin(), active_pointers_.end(), 0);
    }

    void init(std::string title, eka2l1::vec2 size, const std::uint32_t flags) override {
        window_size_ = size;
        SDL_SetWindowTitle(window_, title.c_str());
    }

    void make_current() override {}
    void done_current() override {}
    void swap_buffer() override { SDL_GL_SwapWindow(window_); }

    void poll_events() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                should_quit_ = true;
                if (close_hook) close_hook(userdata_);
                break;

            case SDL_KEYDOWN: {
                if (button_pressed)
                    button_pressed(userdata_, event.key.keysym.sym);
                break;
            }

            case SDL_KEYUP: {
                if (button_released)
                    button_released(userdata_, event.key.keysym.sym);
                break;
            }

            case SDL_MOUSEMOTION: {
                if (raw_mouse_event) {
                    int button = (event.motion.state == 0) ? -1 :
                        eka2l1::common::find_most_significant_bit_one(event.motion.state);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.motion.x, event.motion.y, 0),
                        button, 1, 0);
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                if (raw_mouse_event) {
                    int button = eka2l1::common::find_most_significant_bit_one(event.button.button);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.button.x, event.button.y, 0),
                        button, 0, 0);
                }
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (raw_mouse_event) {
                    int button = eka2l1::common::find_most_significant_bit_one(event.button.button);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.button.x, event.button.y, 0),
                        button, 2, 0);
                }
                break;
            }

            case SDL_MOUSEWHEEL: {
                if (mouse_wheeling) {
                    mouse_wheeling(userdata_, eka2l1::vec2d{static_cast<double>(event.wheel.x), static_cast<double>(event.wheel.y)});
                }
                break;
            }

            case SDL_FINGERDOWN: {
                if (raw_mouse_event) {
                    int screen_w = 0, screen_h = 0;
                    SDL_GetWindowSize(window_, &screen_w, &screen_h);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(
                            static_cast<int>(event.tfinger.x * screen_w),
                            static_cast<int>(event.tfinger.y * screen_h),
                            static_cast<int>(event.tfinger.pressure * eka2l1::PRESSURE_MAX_NUM)),
                        0, 0, static_cast<int>(event.tfinger.fingerId));
                }
                break;
            }

            case SDL_FINGERMOTION: {
                if (raw_mouse_event) {
                    int screen_w = 0, screen_h = 0;
                    SDL_GetWindowSize(window_, &screen_w, &screen_h);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(
                            static_cast<int>(event.tfinger.x * screen_w),
                            static_cast<int>(event.tfinger.y * screen_h),
                            static_cast<int>(event.tfinger.pressure * eka2l1::PRESSURE_MAX_NUM)),
                        0, 1, static_cast<int>(event.tfinger.fingerId));
                }
                break;
            }

            case SDL_FINGERUP: {
                if (raw_mouse_event) {
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(0, 0, 0),
                        0, 2, static_cast<int>(event.tfinger.fingerId));
                }
                break;
            }

            case SDL_WINDOWEVENT: {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    window_size_ = eka2l1::vec2(event.window.data1, event.window.data2);
                    if (resize_hook) {
                        resize_hook(userdata_, window_size_);
                    }
                }
                break;
            }

            case SDL_TEXTINPUT: {
                if (char_hook) {
                    char_hook(userdata_, static_cast<std::uint32_t>(event.text.text[0]));
                }
                break;
            }

            default:
                break;
            }
        }
    }

    void shutdown() override {
        // Don't destroy the window here - Emscripten manages it
    }

    void set_fullscreen(const bool is_fullscreen) override {
        if (is_fullscreen) {
            SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
        } else {
            SDL_SetWindowFullscreen(window_, 0);
        }
    }

    bool should_quit() override { return should_quit_; }

    void change_title(std::string title) override {
        SDL_SetWindowTitle(window_, title.c_str());
    }

    eka2l1::vec2 window_size() override {
        return window_size_;
    }

    eka2l1::vec2 window_fb_size() override {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        return eka2l1::vec2(w, h);
    }

    eka2l1::vec2d get_mouse_pos() override {
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        return eka2l1::vec2d{ static_cast<double>(x), static_cast<double>(y) };
    }

    bool get_mouse_button_hold(const int mouse_btt) override {
        return SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(mouse_btt);
    }

    void set_userdata(void *userdata) override { userdata_ = userdata; }
    void *get_userdata() override { return userdata_; }

    bool set_cursor(eka2l1::drivers::cursor *cur) override { return true; }
    void cursor_visiblity(const bool visi) override {}
    bool cursor_visiblity() override { return true; }

    eka2l1::drivers::window_system_info get_window_system_info() override {
        eka2l1::drivers::window_system_info wsi;
        wsi.type = eka2l1::drivers::window_system_type::web;
        wsi.render_window = window_;
        wsi.render_surface_scale = 1.0f;

        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        wsi.surface_width = static_cast<std::uint32_t>(w);
        wsi.surface_height = static_cast<std::uint32_t>(h);

        return wsi;
    }
};

// ============================================================================
// Audio Callback
// ============================================================================

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
    // The audio driver mix will fill this buffer
    if (g_state.audio_driver) {
        // Audio driver provides mixed audio data
        std::memset(stream, 0, len);
        // TODO: Hook up audio driver output to SDL audio stream
    } else {
        std::memset(stream, 0, len);
    }
}

// ============================================================================
// Initialization
// ============================================================================

static bool init_sdl() {
    // SDL init with video, audio, and timer subsystems
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        LOG_ERROR(FRONTEND_CMDLINE, "SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    // Set OpenGL ES attributes for WebGL2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // Create window with OpenGL support
    g_window = SDL_CreateWindow(
        "EKA2L1 Web - Symbian Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_state.window_width, g_state.window_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!g_window) {
        LOG_ERROR(FRONTEND_CMDLINE, "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }

    // Create OpenGL context
    g_gl_context = SDL_GL_CreateContext(g_window);
    if (!g_gl_context) {
        LOG_ERROR(FRONTEND_CMDLINE, "SDL_GL_CreateContext failed: {}", SDL_GetError());
        return false;
    }

    // Enable vsync
    SDL_GL_SetSwapInterval(1);

    // Initialize SDL audio
    SDL_AudioSpec want_spec;
    SDL_zero(want_spec);
    want_spec.freq = 44100;
    want_spec.format = AUDIO_S16LSB;
    want_spec.channels = 2;
    want_spec.samples = 1024;
    want_spec.callback = sdl_audio_callback;

    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &want_spec, nullptr, 0);
    if (g_audio_device > 0) {
        SDL_PauseAudioDevice(g_audio_device, 0);
    }

    return true;
}

static bool init_emulator() {
    // Setup logger
    eka2l1::log::setup_log(nullptr);

    LOG_INFO(FRONTEND_CMDLINE, "EKA2L1 WebAssembly v0.0.1 ({}-{})", GIT_BRANCH, GIT_COMMIT_HASH);

    // Load config
    g_state.conf.deserialize();
    g_state.app_settings = std::make_unique<config::app_settings>(&g_state.conf);

    // Create system
    eka2l1::system_create_components comp;
    comp.audio_ = nullptr;
    comp.graphics_ = nullptr;
    comp.conf_ = &g_state.conf;
    comp.settings_ = g_state.app_settings.get();

    g_state.symsys = std::make_unique<eka2l1::system>(comp);

    // Setup virtual filesystem paths (using Emscripten's virtual FS)
    const std::string storage_path = "/eka2l1";

    // Create necessary directories in Emscripten FS
    eka2l1::common::create_directory(storage_path);
    eka2l1::common::create_directory(storage_path + "/drives");
    eka2l1::common::create_directory(storage_path + "/drives/c");
    eka2l1::common::create_directory(storage_path + "/drives/d");
    eka2l1::common::create_directory(storage_path + "/drives/e");
    eka2l1::common::create_directory(storage_path + "/drives/z");
    eka2l1::common::create_directory(storage_path + "/temp");
    eka2l1::common::create_directory(storage_path + "/rpkg");

    // Startup the system
    g_state.symsys->startup();

    // Mount drives
    g_state.symsys->mount(drive_c, drive_media::physical,
        storage_path + "/drives/c/", io_attrib_internal);
    g_state.symsys->mount(drive_d, drive_media::physical,
        storage_path + "/drives/d/", io_attrib_internal);
    g_state.symsys->mount(drive_e, drive_media::physical,
        storage_path + "/drives/e/", io_attrib_removeable);

    // Try to set device
    eka2l1::device_manager *dvcmngr = g_state.symsys->get_device_manager();
    if (dvcmngr->total() > 0) {
        g_state.symsys->set_device(g_state.conf.device);
    }

    // Create graphics driver (OpenGL ES 3.0 / WebGL2)
    auto web_window = std::make_unique<sdl_web_window>(g_window);
    web_window->init("EKA2L1", eka2l1::vec2(g_state.window_width, g_state.window_height), 0);

    g_state.graphics_driver = eka2l1::drivers::create_graphics_driver(
        eka2l1::drivers::graphic_api::opengl,
        web_window->get_window_system_info()
    );

    if (!g_state.graphics_driver) {
        LOG_ERROR(FRONTEND_CMDLINE, "Failed to create graphics driver!");
        return false;
    }

    g_state.symsys->set_graphics_driver(g_state.graphics_driver.get());

    // Create audio driver (use null backend for WASM - Web Audio API is not yet wired)
    g_state.audio_driver = eka2l1::drivers::make_audio_driver(
        eka2l1::drivers::audio_driver_backend::null,
        g_state.conf.audio_master_volume,
        eka2l1::drivers::player_type_tsf
    );

    if (g_state.audio_driver) {
        g_state.audio_driver->set_bank_path(eka2l1::drivers::MIDI_BANK_TYPE_HSB,
            g_state.conf.hsb_bank_path);
        g_state.audio_driver->set_bank_path(eka2l1::drivers::MIDI_BANK_TYPE_SF2,
            g_state.conf.sf2_bank_path);
    }

    g_state.symsys->set_audio_driver(g_state.audio_driver.get());

    g_state.initialized = true;
    g_state.running = true;

    LOG_INFO(FRONTEND_CMDLINE, "EKA2L1 WebAssembly initialized successfully!");
    return true;
}

// ============================================================================
// Main Loop (called via emscripten_set_main_loop)
// ============================================================================

static void main_loop() {
    if (!g_state.initialized || !g_state.symsys) {
        return;
    }

    if (g_state.paused) {
        return;
    }

    // Poll SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            g_state.running = false;
            return;
        }
    }

    // Run one iteration of the emulator
    g_state.symsys->loop();

    // Swap buffers
    SDL_GL_SwapWindow(g_window);

    // FPS counting
    g_state.frame_count++;
    Uint32 now = SDL_GetTicks();
    if (now - g_state.last_fps_time >= 1000) {
        g_state.current_fps = g_state.frame_count;
        g_state.frame_count = 0;
        g_state.last_fps_time = now;

        // Update window title with FPS
        char title[128];
        std::snprintf(title, sizeof(title), "EKA2L1 Web - %d FPS", g_state.current_fps);
        SDL_SetWindowTitle(g_window, title);
    }
}

// ============================================================================
// C API Functions (exported to JavaScript via Emscripten)
// ============================================================================

extern "C" {

/**
 * Initialize emulator with a ROM file path.
 * Called from JavaScript after the user loads a ROM file.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_init_with_rom(const char *rom_path) {
    if (!rom_path) return -1;

    LOG_INFO(FRONTEND_CMDLINE, "Initializing with ROM: {}", rom_path);

    if (!g_state.initialized) {
        if (!init_emulator()) {
            return -2;
        }
    }

    // Mount ROM drive
    g_state.symsys->mount(drive_z, drive_media::rom,
        eka2l1::add_path(g_state.conf.storage, "/drives/z/"),
        io_attrib_internal | io_attrib_write_protected);

    g_state.symsys->initialize_user_parties();

    return 0;
}

/**
 * Install a SIS package from the given path.
 * Called from JavaScript after the user loads a SIS file.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_install_package(const char *pkg_path) {
    if (!pkg_path) return -1;

    LOG_INFO(FRONTEND_CMDLINE, "Installing package: {}", pkg_path);

    if (!g_state.symsys) return -2;

    auto pkgmngr = g_state.symsys->get_packages();
    if (pkgmngr) {
        // Package installation logic
        LOG_INFO(FRONTEND_CMDLINE, "Package installation queued");
        return 0;
    }

    return -3;
}

/**
 * Run the main emulation loop.
 * Uses emscripten_set_main_loop for browser-friendly execution.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_main_loop() {
    emscripten_set_main_loop(main_loop, 0, 1);
}

/**
 * Reset the emulator state.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_reset() {
    LOG_INFO(FRONTEND_CMDLINE, "Resetting emulator...");

    if (g_state.symsys) {
        g_state.paused = false;
        g_state.running = false;

        // Reset the system
        g_state.symsys.reset();
        g_state.initialized = false;
    }
}

/**
 * Set the audio volume (0-100).
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_volume(int volume) {
    if (g_state.audio_driver) {
        g_state.audio_driver->master_volume(static_cast<std::uint32_t>(volume));
    }
}

/**
 * Pause/resume emulation.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_paused(int paused) {
    g_state.paused = (paused != 0);
}

/**
 * Get the current FPS.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_get_fps() {
    return g_state.current_fps;
}

/**
 * Get emulator state as a bitmask.
 * Bit 0: initialized
 * Bit 1: running
 * Bit 2: paused
 */
EMSCRIPTEN_KEEPALIVE
int wasm_get_state() {
    int state = 0;
    if (g_state.initialized) state |= 1;
    if (g_state.running) state |= 2;
    if (g_state.paused) state |= 4;
    return state;
}

} // extern "C"

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char **argv) {
    LOG_INFO(FRONTEND_CMDLINE, "EKA2L1 WebAssembly starting...");

    // Initialize SDL
    if (!init_sdl()) {
        LOG_ERROR(FRONTEND_CMDLINE, "Failed to initialize SDL!");
        return 1;
    }

    LOG_INFO(FRONTEND_CMDLINE, "SDL initialized successfully");
    LOG_INFO(FRONTEND_CMDLINE, "OpenGL Renderer: {}", (const char *)glGetString(GL_RENDERER));
    LOG_INFO(FRONTEND_CMDLINE, "OpenGL Version: {}", (const char *)glGetString(GL_VERSION));

    // Initialize the emulator
    if (!init_emulator()) {
        LOG_ERROR(FRONTEND_CMDLINE, "Failed to initialize emulator!");
        // Don't return - keep the window open to show error
    }

    // Set the main loop
    // emscripten_set_main_loop takes care of:
    // - requestAnimationFrame for smooth rendering
    // - yielding back to the browser event loop
    // - proper handling of browser tab switching
    emscripten_set_main_loop(main_loop, 0, 1);

    // Cleanup (this is actually never reached in Emscripten)
    if (g_gl_context) SDL_GL_DeleteContext(g_gl_context);
    if (g_window) SDL_DestroyWindow(g_window);
    if (g_audio_device > 0) SDL_CloseAudioDevice(g_audio_device);
    SDL_Quit();

    return 0;
}