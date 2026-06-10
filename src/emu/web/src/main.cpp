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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

// Emscripten-specific headers
#include <emscripten.h>
#include <emscripten/html5.h>
#include <unistd.h>

// EKA2L1 headers
#include <common/algorithm.h>
#include <common/buffer.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <common/path.h>
#include <common/platform.h>
#include <common/version.h>

#include <config/app_settings.h>
#include <config/config.h>

#include <drivers/audio/audio.h>
#include <drivers/graphics/graphics.h>
#include <drivers/input/common.h>
#include <drivers/input/emu_controller.h>
#include <drivers/itc.h>

#include <system/epoc.h>
#include <system/devices.h>
#include <system/installation/rpkg.h>
#include <system/installation/common.h>

#include <services/applist/applist.h>
#include <utils/apacmd.h>
#include <common/cvt.h>

#include <kernel/codeseg.h>
#include <kernel/kernel.h>

#include <services/window/window.h>
#include <services/window/screen.h>
#include <services/window/keys.h>
#include <services/init.h>

// ============================================================================
// Global State
// ============================================================================

static SDL_Window *g_window = nullptr;
static SDL_GLContext g_gl_context = nullptr;
static SDL_AudioDeviceID g_audio_device = 0;

class sdl_web_window;

namespace eka2l1::web {
    struct wasm_state {
        std::unique_ptr<eka2l1::system> symsys;
        drivers::graphics_driver_ptr graphics_driver;
        drivers::audio_driver_instance audio_driver;
        config::state conf;
        std::unique_ptr<config::app_settings> app_settings;

        // Lives for the whole page lifetime; owns the SDL input hook plumbing.
        sdl_web_window *window = nullptr;
        eka2l1::window_server *winserv = nullptr;

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
// Input plumbing: SDL events -> window server
// ============================================================================

static eka2l1::drivers::input_event make_mouse_event_driver(const float x, const float y, const float z,
    const int button, const int action, const int mouse_id) {
    eka2l1::drivers::input_event evt;
    evt.type_ = eka2l1::drivers::input_event_type::touch;
    evt.mouse_.raw_screen_pos_ = false;
    evt.mouse_.pos_x_ = static_cast<int>(x);
    evt.mouse_.pos_y_ = static_cast<int>(y);
    evt.mouse_.pos_z_ = static_cast<int>(z);
    evt.mouse_.mouse_id = static_cast<std::uint32_t>(mouse_id);
    evt.mouse_.button_ = static_cast<eka2l1::drivers::mouse_button>(button);
    evt.mouse_.action_ = static_cast<eka2l1::drivers::mouse_action>(action);

    return evt;
}

// Map host (SDL) keycodes to Symbian scancodes for a classic non-touch S60
// phone. Sent as key_raw events so no keybind configuration is needed.
//   Arrows = D-pad, Enter = OK/middle key, F1/F2 = soft keys,
//   F3/F4 (or Esc) = call/end-call, Backspace = C (clear),
//   digits / letters map straight through, [ ] = * #.
static std::uint32_t sdl_key_to_symbian_scancode(const int sdl_key) {
    switch (sdl_key) {
    case SDLK_UP:
        return epoc::std_key_up_arrow;
    case SDLK_DOWN:
        return epoc::std_key_down_arrow;
    case SDLK_LEFT:
        return epoc::std_key_left_arrow;
    case SDLK_RIGHT:
        return epoc::std_key_right_arrow;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return epoc::std_key_device_3; // middle / OK
    case SDLK_F1:
        return epoc::std_key_device_0; // left soft key
    case SDLK_F2:
        return epoc::std_key_device_1; // right soft key
    case SDLK_F3:
        return epoc::std_key_yes; // call (green)
    case SDLK_F4:
    case SDLK_ESCAPE:
        return epoc::std_key_no; // end call (red)
    case SDLK_BACKSPACE:
        return epoc::std_key_backspace; // C key
    case SDLK_LEFTBRACKET:
    case SDLK_KP_MULTIPLY:
        return epoc::std_key_nkp_asterisk; // *
    case SDLK_RIGHTBRACKET:
        return epoc::std_key_hash; // #
    default:
        break;
    }

    if ((sdl_key >= SDLK_0) && (sdl_key <= SDLK_9)) {
        return static_cast<std::uint32_t>('0' + (sdl_key - SDLK_0));
    }

    if ((sdl_key >= SDLK_KP_1) && (sdl_key <= SDLK_KP_9)) {
        return static_cast<std::uint32_t>('1' + (sdl_key - SDLK_KP_1));
    }

    if (sdl_key == SDLK_KP_0) {
        return static_cast<std::uint32_t>('0');
    }

    if ((sdl_key >= SDLK_a) && (sdl_key <= SDLK_z)) {
        return static_cast<std::uint32_t>('A' + (sdl_key - SDLK_a));
    }

    return 0;
}

static eka2l1::drivers::input_event make_key_event_driver(const int key, const eka2l1::drivers::key_state key_state) {
    eka2l1::drivers::input_event evt;
    // key_raw: the code is used directly as the Symbian scancode (no keybind
    // table lookup, which would be empty on a fresh web config).
    evt.type_ = eka2l1::drivers::input_event_type::key_raw;
    evt.key_.state_ = key_state;
    evt.key_.code_ = key;

    return evt;
}

static void on_web_window_mouse_evt(void *userdata, eka2l1::vec3 mouse_pos, int button, int action, int mouse_id) {
    if (!g_state.winserv) {
        return;
    }

    const float scale = g_state.conf.ui_scale;
    auto evt = make_mouse_event_driver(mouse_pos.x / scale, mouse_pos.y / scale, mouse_pos.z / scale,
        button, action, mouse_id);
    g_state.winserv->queue_input_from_driver(evt);
}

static void on_web_window_key_press(void *userdata, const int key) {
    const std::uint32_t scancode = sdl_key_to_symbian_scancode(key);

    // TEMP DIAGNOSTIC: confirms SDL actually delivers keydown to the C++ side.
    // If this never logs when you press keys, the browser/SDL isn't routing
    // keyboard events here (focus issue). Remove once input is confirmed.
    LOG_INFO(FRONTEND_CMDLINE, "[key] down sdl={} -> scancode=0x{:X} winserv={}",
        key, scancode, g_state.winserv ? "ok" : "null");

    if (!g_state.winserv) {
        return;
    }

    if (!scancode) {
        return;
    }

    auto evt = make_key_event_driver(static_cast<int>(scancode), eka2l1::drivers::key_state::pressed);
    g_state.winserv->queue_input_from_driver(evt);
}

static void on_web_window_key_release(void *userdata, const int key) {
    if (!g_state.winserv) {
        return;
    }

    const std::uint32_t scancode = sdl_key_to_symbian_scancode(key);
    if (!scancode) {
        return;
    }

    auto evt = make_key_event_driver(static_cast<int>(scancode), eka2l1::drivers::key_state::released);
    g_state.winserv->queue_input_from_driver(evt);
}

// ============================================================================
// Screen composition: emulated screen texture -> backbuffer
// ============================================================================

// Number of screen redraw callbacks fired. main_loop's boot_phase CPU budget
// keys off whether the first frame has been presented yet (see boot_phase).
static std::atomic<std::uint64_t> s_redraw_cb_count{ 0 };

// Simplified port of the Android launcher::draw(): clear the backbuffer and
// blit the emulated screen texture scaled to fit the window, centered.
static void draw_emulated_screen(eka2l1::drivers::graphics_command_builder &builder, epoc::screen *scr,
    const std::uint32_t window_width, const std::uint32_t window_height) {
    eka2l1::rect viewport;
    eka2l1::rect src;
    eka2l1::rect dest;

    eka2l1::vec2 swapchain_size(static_cast<int>(window_width), static_cast<int>(window_height));
    viewport.size = swapchain_size;

    builder.set_swapchain_size(swapchain_size);
    builder.backup_state();
    builder.bind_bitmap(0);

    builder.set_feature(eka2l1::drivers::graphics_feature::cull, false);
    builder.set_feature(eka2l1::drivers::graphics_feature::depth_test, false);
    builder.set_feature(eka2l1::drivers::graphics_feature::blend, false);
    builder.set_feature(eka2l1::drivers::graphics_feature::clipping, false);
    builder.set_feature(eka2l1::drivers::graphics_feature::stencil_test, false);
    builder.set_viewport(viewport);

    builder.clear({ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }, eka2l1::drivers::draw_buffer_bit_color_buffer);

    if (scr && scr->screen_texture) {
        auto &crr_mode = scr->current_mode();

        eka2l1::vec2 size = crr_mode.size;
        src.size = size;

        // Fit in window, keep aspect ratio
        float width = static_cast<float>(swapchain_size.x);
        float height = size.y * width / size.x;

        if (height > swapchain_size.y) {
            height = static_cast<float>(swapchain_size.y);
            width = size.x * height / size.y;
        }

        const int x = static_cast<int>((swapchain_size.x - width) / 2);
        const int y = static_cast<int>((swapchain_size.y - height) / 2);

        const float scale_x = width / static_cast<float>(size.x);
        const float scale_y = height / static_cast<float>(size.y);

        scr->set_native_scale_factor(g_state.graphics_driver.get(), scale_x, scale_y);
        scr->absolute_pos.x = x;
        scr->absolute_pos.y = y;

        dest.top = eka2l1::vec2(x, y);
        dest.size = eka2l1::vec2(static_cast<int>(width), static_cast<int>(height));

        eka2l1::drivers::advance_draw_pos_around_origin(dest, scr->ui_rotation);

        if (scr->ui_rotation % 180 != 0) {
            std::swap(dest.size.x, dest.size.y);
            std::swap(src.size.x, src.size.y);
        }

        src.size *= scr->display_scale_factor;

        builder.set_texture_filter(scr->screen_texture, true, eka2l1::drivers::filter_option::linear);
        builder.set_texture_filter(scr->screen_texture, false, eka2l1::drivers::filter_option::linear);
        builder.draw_bitmap(scr->screen_texture, 0, dest, src, eka2l1::vec2(0, 0),
            static_cast<float>(scr->ui_rotation), 0);
    }

    builder.load_backup_state();
}

// Counterpart of the Android frontend's register_draw_callback(): whenever the
// window server finishes composing a screen, queue a command list that presents
// it to the backbuffer. The callback may fire on the timer thread; it only
// builds commands (no GL) — the actual GL work happens in main_loop's pump().
static void register_screen_draw_callbacks() {
    if (!g_state.winserv) {
        return;
    }

    epoc::screen *screens = g_state.winserv->get_screens();
    int total_screens = 0;

    while (screens) {
        screens->add_screen_redraw_callback(nullptr, [](void *userdata, epoc::screen *scr, const bool is_dsa) {
            if (!g_state.graphics_driver) {
                return;
            }

            // Counts presented frames; main_loop's boot_phase budget keys off
            // the first redraw (see FRAME_CPU_BUDGET_MS).
            ++s_redraw_cb_count;

            eka2l1::drivers::graphics_command_builder builder;
            draw_emulated_screen(builder, scr,
                static_cast<std::uint32_t>(g_state.window_width),
                static_cast<std::uint32_t>(g_state.window_height));

            eka2l1::drivers::command_list draw_list = builder.retrieve_command_list();
            g_state.graphics_driver->submit_command_list(draw_list);

            // Present as a separate list. No status tracking: this callback may
            // run on the timer thread and must never block.
            eka2l1::drivers::graphics_command_builder present_builder;
            present_builder.present(nullptr);
            eka2l1::drivers::command_list present_list = present_builder.retrieve_command_list();
            g_state.graphics_driver->submit_command_list(present_list);
        });

        screens = screens->next;
        total_screens++;
    }

    // WARN so it is visible at the default WASM log level.
    LOG_WARN(FRONTEND_CMDLINE, "Screen redraw callbacks registered on {} screen(s)", total_screens);
}

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
    // SDL init with video, audio, and timer subsystems.
    // On WASM we deliberately skip SDL_INIT_AUDIO: SDL2's Emscripten audio
    // backend uses ScriptProcessorNode whose onaudioprocess callback runs on
    // the browser main thread and performs SDL_ResampleAudio synchronously.
    // Profiling showed it monopolized the main thread, starving
    // requestAnimationFrame and freezing the page.
#ifdef __EMSCRIPTEN__
    constexpr Uint32 SDL_INIT_FLAGS = SDL_INIT_VIDEO | SDL_INIT_TIMER;
#else
    constexpr Uint32 SDL_INIT_FLAGS = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
#endif
    if (SDL_Init(SDL_INIT_FLAGS) != 0) {
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

    // Create window with OpenGL support.
    // No RESIZABLE/HIGHDPI: on Emscripten those make SDL sync the canvas
    // bitmap size to the CSS/client size on every browser resize — while the
    // page is still showing the splash the container is 0x0, which collapses
    // the WebGL drawing buffer to 1x1. The canvas stays at the fixed emulator
    // resolution; the page scales it visually with CSS only.
    g_window = SDL_CreateWindow(
        "EKA2L1 Web - Symbian Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_state.window_width, g_state.window_height,
        SDL_WINDOW_OPENGL
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

    // Enable vsync (skip on WASM: emscripten_set_main_loop_timing not available yet)
#ifndef __EMSCRIPTEN__
    SDL_GL_SetSwapInterval(1);
#endif

#ifndef __EMSCRIPTEN__
    // Initialize SDL audio (skipped on WASM — SDL2 Emscripten audio runs
    // SDL_ResampleAudio on the main thread via ScriptProcessorNode and
    // starves the RAF loop; profiling traced page freeze to this path).
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
#endif

    return true;
}

static bool init_emulator() {
    // Setup logger
    eka2l1::log::setup_log(nullptr);

    LOG_INFO(FRONTEND_CMDLINE, "EKA2L1 WebAssembly v0.0.1 ({}-{})", GIT_BRANCH, GIT_COMMIT_HASH);

    // All persistent data lives under the IDBFS mount point.
    // Override the storage path BEFORE creating the system so that device_manager,
    // package manager, etc. all use the same persisted location.
    const std::string storage_path = "/eka2l1";
    g_state.conf.storage = storage_path;

    // Load config (may return early if config.yml doesn't exist yet – that's fine)
    g_state.conf.deserialize();
    // Re-enforce storage path in case deserialize() loaded a stale relative path.
    g_state.conf.storage = storage_path;

    g_state.app_settings = std::make_unique<config::app_settings>(&g_state.conf);

    // Create necessary directories in Emscripten FS
    eka2l1::common::create_directory(storage_path);
    eka2l1::common::create_directory(storage_path + "/drives");
    eka2l1::common::create_directory(storage_path + "/drives/c");
    eka2l1::common::create_directory(storage_path + "/drives/d");
    eka2l1::common::create_directory(storage_path + "/drives/e");
    eka2l1::common::create_directory(storage_path + "/drives/z");
    eka2l1::common::create_directory(storage_path + "/roms");
    eka2l1::common::create_directory(storage_path + "/temp");
    eka2l1::common::create_directory(storage_path + "/rpkg");

    // Create system
    eka2l1::system_create_components comp;
    comp.audio_ = nullptr;
    comp.graphics_ = nullptr;
    comp.conf_ = &g_state.conf;
    comp.settings_ = g_state.app_settings.get();

    g_state.symsys = std::make_unique<eka2l1::system>(comp);

    // Startup the system
    g_state.symsys->startup();

    // Mount drives
    g_state.symsys->mount(drive_c, drive_media::physical,
        storage_path + "/drives/c/", io_attrib_internal);
    g_state.symsys->mount(drive_d, drive_media::physical,
        storage_path + "/drives/d/", io_attrib_internal);
    g_state.symsys->mount(drive_e, drive_media::physical,
        storage_path + "/drives/e/", io_attrib_removeable);

    // On WASM, do NOT call set_device() here: wasm_init_with_rom() will call it once.
    // Calling it here causes a second set_device() call later which triggers
    // ntimer::wipeout() -> timer_thread_->join(), blocking the main browser thread.

    // Create graphics driver (OpenGL ES 3.0 / WebGL2).
    // The window object must outlive this function: it carries the input hooks
    // that poll_events() dispatches every frame (page lifetime, never freed).
    if (!g_state.window) {
        g_state.window = new sdl_web_window(g_window);
    }
    g_state.window->init("EKA2L1", eka2l1::vec2(g_state.window_width, g_state.window_height), 0);
    g_state.window->raw_mouse_event = on_web_window_mouse_evt;
    g_state.window->button_pressed = on_web_window_key_press;
    g_state.window->button_released = on_web_window_key_release;

    g_state.graphics_driver = eka2l1::drivers::create_graphics_driver(
        eka2l1::drivers::graphic_api::opengl,
        g_state.window->get_window_system_info()
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

    // Poll SDL events through the emu window so the input hooks fire and
    // mouse/touch/key events reach the window server. Poll even while paused
    // so the canvas stays responsive.
    if (g_state.window) {
        g_state.window->poll_events();

        const eka2l1::vec2 fb_size = g_state.window->window_fb_size();
        if ((fb_size.x > 0) && (fb_size.y > 0)) {
            g_state.window_width = fb_size.x;
            g_state.window_height = fb_size.y;
        }
    }

    // Execute screen redraws deferred by the animation scheduler. They must
    // run here on the main thread: redraw performs synchronous GPU calls
    // (inline-dispatched on this thread), which would deadlock on the ntimer
    // thread while it holds the kernel lock.
    if (g_state.winserv) {
        g_state.winserv->get_anim_scheduler()->flush_pending_redraws();
    }

    if (g_state.paused) {
        // Still flush pending GPU work (e.g. texture uploads queued before pausing).
        if (g_state.graphics_driver) {
            g_state.graphics_driver->pump();
        }
        return;
    }

    // Run several short guest slices per browser frame. A single full guest
    // timeslice can block the browser, but only one tiny slice per RAF makes
    // app startup crawl. Use a wall-clock budget to balance progress/yielding.
    //
    // Adaptive: until the first frame is presented (boot/app startup, e.g.
    // ECom plugin discovery takes hundreds of millions of guest instructions)
    // spend most of the frame on the guest. Once content is on screen, back
    // off so DOM events stay responsive.
    const bool boot_phase = (s_redraw_cb_count.load() == 0);
    const double FRAME_CPU_BUDGET_MS = boot_phase ? 16.0 : 8.0;
    const int MAX_SLICES_PER_FRAME = boot_phase ? 96 : 32;

    const double frame_start = emscripten_get_now();
    int slices = 0;
    while (slices < MAX_SLICES_PER_FRAME) {
        const int loop_result = g_state.symsys->loop();
        ++slices;

        if (loop_result == 0) {
            break;
        }

        if ((emscripten_get_now() - frame_start) >= FRAME_CPU_BUDGET_MS) {
            break;
        }
    }

    // Drain the graphics command queue on the browser main thread (the only
    // thread allowed to touch WebGL). This executes window-server composition
    // and the present command lists queued by the screen redraw callbacks.
    if (g_state.graphics_driver) {
        g_state.graphics_driver->pump();
    }

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
 * Initialize emulator with a ROM (and optionally RPKG) file.
 * Called from JavaScript after the user loads device files into the VFS.
 *
 * EKA1 mode  (rpkg_path == NULL or ""):
 *   install_rom(rom_path) – self-contained old-style ROM.
 *
 * EKA2 mode (rpkg_path provided):
 *   install_rpkg(rpkg_path) – extracts Z-drive contents and registers device.
 *   Then copies rom_path to roms/<firmcode>/SYM.ROM.
 *
 * After installation:
 *   set_device(0) → triggers full reset() (memory, dispatcher, services).
 *   mount drive_z from the extracted directory.
 *   initialize_user_parties() → HLE libraries + boot.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_init_with_rom(const char *rom_path, const char *rpkg_path) {
    if (!rom_path) return -1;

    // Defensive: if JS calls us before main() runs, spd_logger is null and the
    // first LOG_INFO traps with an out-of-bounds access. Ensure setup_log() has
    // run before any logging happens here.
    if (!eka2l1::log::spd_logger) {
        eka2l1::log::setup_log(nullptr);
        eka2l1::log::toggle_console();
    }

    const bool has_rpkg = (rpkg_path && rpkg_path[0] != '\0');
    LOG_INFO(FRONTEND_CMDLINE, "Initializing with ROM: {}  RPKG: {}",
        rom_path, has_rpkg ? rpkg_path : "(none)");

    if (!g_state.initialized) {
        if (!init_emulator()) {
            return -2;
        }
    }

    eka2l1::device_manager *dvcmngr = g_state.symsys->get_device_manager();

    // Diagnostic: how many devices did device_manager find at startup?
    // After a page reload this comes from /eka2l1/devices.yml restored by IDBFS.
    LOG_INFO(FRONTEND_CMDLINE, "device_manager loaded {} device(s) from {}",
        dvcmngr->total(), g_state.conf.storage);
    {
        common::ro_std_file_stream yml_check(
            eka2l1::add_path(g_state.conf.storage, "devices.yml"), true);
        LOG_INFO(FRONTEND_CMDLINE, "devices.yml exists/valid: {} size={}",
            yml_check.valid() ? "yes" : "no",
            yml_check.valid() ? static_cast<int>(yml_check.size()) : -1);
    }

    if (dvcmngr->total() == 0) {
        // Validate ROM file exists and is readable.
        {
            common::ro_std_file_stream check_stream(std::string(rom_path), true);
            if (!check_stream.valid()) {
                LOG_ERROR(FRONTEND_CMDLINE, "ROM file not found or not readable: {}", rom_path);
                return -3;
            }
        }

        const std::string &storage = g_state.conf.storage;
        const std::string rom_resident  = eka2l1::add_path(storage, "roms/");
        const std::string drives_z_path = eka2l1::add_path(storage, "drives/z/");

        eka2l1::common::create_directories(rom_resident);
        eka2l1::common::create_directories(drives_z_path);

        eka2l1::device_installation_error install_err = eka2l1::device_installation_none;

        if (has_rpkg) {
            // ---- EKA2 path: RPKG contains Z-drive content, ROM is just the kernel image ----
            {
                common::ro_std_file_stream rpkg_check(std::string(rpkg_path), true);
                if (!rpkg_check.valid()) {
                    LOG_ERROR(FRONTEND_CMDLINE, "RPKG file not found or not readable: {}", rpkg_path);
                    return -3;
                }
            }

            std::string firmware_code;
            LOG_INFO(FRONTEND_CMDLINE, "EKA2 install: RPKG={} ROM={}", rpkg_path, rom_path);

            install_err = eka2l1::loader::install_rpkg(
                dvcmngr, std::string(rpkg_path), drives_z_path, firmware_code, nullptr, nullptr);

            if (install_err != eka2l1::device_installation_none) {
                LOG_ERROR(FRONTEND_CMDLINE, "RPKG installation failed, error={}",
                    static_cast<int>(install_err));
                return -(1000 + static_cast<int>(install_err));
            }

            // Copy ROM image to roms/<firmcode>/SYM.ROM
            const std::string rom_dir = eka2l1::add_path(rom_resident, firmware_code + "/");
            eka2l1::common::create_directories(rom_dir);
            if (!eka2l1::common::copy_file(std::string(rom_path),
                    eka2l1::add_path(rom_dir, "SYM.ROM"), true)) {
                LOG_ERROR(FRONTEND_CMDLINE, "Failed to copy ROM to {}", rom_dir);
                return -5;
            }

            LOG_INFO(FRONTEND_CMDLINE, "EKA2 device installed: firmcode={}", firmware_code);
        } else {
            // ---- EKA1 path: single self-contained ROM file ----
            LOG_INFO(FRONTEND_CMDLINE, "EKA1 install: ROM={}", rom_path);

            install_err = eka2l1::loader::install_rom(
                dvcmngr, std::string(rom_path), rom_resident, drives_z_path, nullptr, nullptr);

            if (install_err != eka2l1::device_installation_none) {
                LOG_ERROR(FRONTEND_CMDLINE, "ROM installation failed, error={}",
                    static_cast<int>(install_err));
                return -(1000 + static_cast<int>(install_err));
            }

            LOG_INFO(FRONTEND_CMDLINE, "EKA1 device installed");
        }

        // install_rpkg/install_rom call add_new_device() but never save_devices().
        // Without this call devices.yml is never written to disk, so the device
        // list is lost on every page reload.
        dvcmngr->save_devices();
        LOG_INFO(FRONTEND_CMDLINE, "{} device(s) now registered, devices.yml saved", dvcmngr->total());
    }

    // Triggers reset(): memory model init, ROM load, dispatcher + service setup.
    if (!g_state.symsys->set_device(0)) {
        LOG_ERROR(FRONTEND_CMDLINE, "set_device(0) failed");
        return -4;
    }

    // Mount Z drive from the extracted ROM filesystem contents.
    g_state.symsys->mount(drive_z, drive_media::rom,
        eka2l1::add_path(g_state.conf.storage, "/drives/z/"),
        io_attrib_internal | io_attrib_write_protected);

    // Register HLE dispatch libraries, init services, start the bootload.
    g_state.symsys->initialize_user_parties();

    // Hook the window server: screen redraw callbacks (composition + present)
    // and the input event queue target. Must re-run after every set_device()
    // since reset() recreates the kernel and all services.
    eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
    if (kern) {
        g_state.winserv = reinterpret_cast<eka2l1::window_server *>(
            kern->get_by_name<eka2l1::service::server>(
                eka2l1::get_winserv_name_by_epocver(g_state.symsys->get_symbian_version_use())));

        if (g_state.winserv) {
            register_screen_draw_callbacks();
        } else {
            LOG_ERROR(FRONTEND_CMDLINE, "Window server not found; no display output will be presented");
        }
    }

    return 0;
}

/**
 * Install a SIS package from the given VFS path.
 * Returns 0 on success, negative on error, or package::installation_result value.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_install_package(const char *pkg_path) {
    if (!pkg_path) return -1;

    LOG_INFO(FRONTEND_CMDLINE, "Installing package: {}", pkg_path);

    if (!g_state.symsys) return -2;

    const std::u16string path_u16 = eka2l1::common::utf8_to_ucs2(std::string(pkg_path));
    const int result = g_state.symsys->install_package(path_u16, drive_e);

    if (result == 0) {
        LOG_INFO(FRONTEND_CMDLINE, "Package installed successfully");
    } else {
        LOG_ERROR(FRONTEND_CMDLINE, "Package installation failed: {}", result);
    }

    return result;
}

/**
 * Return a JSON array of installed (visible) apps from the APA registry.
 * Format: [{"uid":<number>,"name":"<string>"},...]
 * The returned pointer is valid until the next call to this function.
 */
EMSCRIPTEN_KEEPALIVE
const char *wasm_get_app_list() {
    static std::string s_json;

    if (!g_state.symsys) {
        s_json = "[]";
        return s_json.c_str();
    }

    eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
    if (!kern) {
        s_json = "[]";
        return s_json.c_str();
    }

    auto *alserv = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

    if (!alserv) {
        s_json = "[]";
        return s_json.c_str();
    }

    std::string json = "[";
    bool first = true;

    for (auto &reg : alserv->get_registerations()) {
        if (reg.caps.is_hidden) continue;

        std::string name = eka2l1::common::ucs2_to_utf8(
            reg.mandatory_info.long_caption.to_std_string(nullptr));

        // Minimal JSON escaping: replace backslash then double-quote
        std::string escaped;
        escaped.reserve(name.size());
        for (char c : name) {
            if (c == '\\') { escaped += "\\\\"; }
            else if (c == '"') { escaped += "\\\""; }
            else { escaped += c; }
        }

        if (!first) json += ',';
        first = false;
        json += "{\"uid\":";
        json += std::to_string(reg.mandatory_info.uid);
        json += ",\"name\":\"";
        json += escaped;
        json += "\"}";
    }

    json += ']';
    s_json = std::move(json);
    return s_json.c_str();
}

/**
 * Launch an installed app by its UID.
 * Returns 0 on success, negative on error.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_launch_app(int uid) {
    if (!g_state.symsys) return -1;

    eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
    if (!kern) return -2;

    auto *alserv = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));

    if (!alserv) return -3;

    eka2l1::apa_app_registry *reg = alserv->get_registration(
        static_cast<std::uint32_t>(uid));
    if (!reg) return -4;

    const std::u16string app_path = reg->mandatory_info.app_path.to_std_string(nullptr);
    LOG_INFO(FRONTEND_CMDLINE, "Launching app uid=0x{:08X} path={}",
        static_cast<std::uint32_t>(uid), eka2l1::common::ucs2_to_utf8(app_path));

    epoc::apa::command_line cmdline;
    // Match the desktop app-list launcher. command_create is used by the
    // command-line path, but UI-launched apps expect the normal open command.
    cmdline.launch_cmd_ = epoc::apa::command_open;

    bool ok = false;
    try {
        kern->lock();
        ok = alserv->launch_app(*reg, cmdline, nullptr,
            [](kernel::process *pr) {
                if (!pr) {
                    LOG_ERROR(FRONTEND_CMDLINE, "Launched app exited with null process handle");
                    return;
                }

                LOG_INFO(FRONTEND_CMDLINE,
                    "App process exited: name={} uid=0x{:08X} exit_type={} category={} reason={}",
                    pr->name(), pr->get_uid(), static_cast<int>(pr->get_exit_type()),
                    eka2l1::common::ucs2_to_utf8(pr->get_exit_category()), pr->get_exit_reason());
            });
        kern->unlock();
    } catch (const std::exception &exception) {
        kern->unlock();
        LOG_ERROR(FRONTEND_CMDLINE, "Exception while launching app uid=0x{:08X}: {}",
            static_cast<std::uint32_t>(uid), exception.what());
        return -6;
    } catch (...) {
        kern->unlock();
        LOG_ERROR(FRONTEND_CMDLINE, "Unknown exception while launching app uid=0x{:08X}",
            static_cast<std::uint32_t>(uid));
        return -7;
    }

    if (!ok) {
        LOG_ERROR(FRONTEND_CMDLINE, "launch_app returned false for uid=0x{:08X}",
            static_cast<std::uint32_t>(uid));
    }

    if (ok) {
        g_state.paused = false;
    }

    return ok ? 0 : -5;
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
    // Must initialize logging before any LOG_* calls, otherwise spd_logger
    // and filterings are null, which causes a null-function trap in WASM.
    eka2l1::log::setup_log(nullptr);
    // By default spdlog only writes to EKA2L1.log; toggle_console() adds the
    // stdout sink so logs appear in the browser devtools console.
    eka2l1::log::toggle_console();

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
