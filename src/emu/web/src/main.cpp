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

#include <algorithm>
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

#include "web_audio.h"

#include <system/epoc.h>
#include <system/devices.h>
#include <system/installation/rpkg.h>
#include <cpu/dyncom/arm_dyncom_jit.h>
#include <system/installation/common.h>

#include <services/applist/applist.h>
#include <services/fbs/fbs.h>
#include <utils/apacmd.h>
#include <common/crypt.h>
#include <common/cvt.h>
#include <loader/mbm.h>
#include <loader/mif.h>
#include <loader/nvg.h>
#include <loader/svgb.h>
#include <vfs/vfs.h>

#include <kernel/codeseg.h>
#include <kernel/kernel.h>
#include <kernel/thread.h>

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

// SDL button index (1/2/3) -> drivers::mouse_button (left=0, right=1, middle=2).
// The window server only treats mouse_button_left as the touch pointer
// (button1down); passing the raw SDL index made every click a right-click.
static int sdl_mouse_button_to_driver(const Uint8 sdl_button) {
    switch (sdl_button) {
    case SDL_BUTTON_RIGHT:
        return eka2l1::drivers::mouse_button_right;
    case SDL_BUTTON_MIDDLE:
        return eka2l1::drivers::mouse_button_middle;
    case SDL_BUTTON_LEFT:
    default:
        return eka2l1::drivers::mouse_button_left;
    }
}

static int sdl_mouse_state_to_driver(const Uint32 state) {
    if (state & SDL_BUTTON_LMASK)
        return eka2l1::drivers::mouse_button_left;
    if (state & SDL_BUTTON_RMASK)
        return eka2l1::drivers::mouse_button_right;
    if (state & SDL_BUTTON_MMASK)
        return eka2l1::drivers::mouse_button_middle;
    return eka2l1::drivers::mouse_button_none;
}

class sdl_web_window : public eka2l1::drivers::emu_window {
private:
    SDL_Window *window_;
    void *userdata_;
    bool should_quit_;
    eka2l1::vec2 window_size_;
    std::array<std::uint32_t, eka2l1::MAX_SYMBIAN_SUPPORTED_POINTERS> active_pointers_;

    // Map an SDL finger id to a small stable pointer slot (Symbian pointer
    // numbers must stay < MAX_SYMBIAN_SUPPORTED_POINTERS; browser touch
    // identifiers grow unbounded). Returns -1 when no slot matches/frees.
    int finger_slot_acquire(const SDL_FingerID id) {
        const std::uint32_t key = static_cast<std::uint32_t>(id) + 1;
        for (std::size_t i = 0; i < active_pointers_.size(); i++) {
            if (active_pointers_[i] == key)
                return static_cast<int>(i);
        }
        for (std::size_t i = 0; i < active_pointers_.size(); i++) {
            if (active_pointers_[i] == 0) {
                active_pointers_[i] = key;
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int finger_slot_find(const SDL_FingerID id) {
        const std::uint32_t key = static_cast<std::uint32_t>(id) + 1;
        for (std::size_t i = 0; i < active_pointers_.size(); i++) {
            if (active_pointers_[i] == key)
                return static_cast<int>(i);
        }
        return -1;
    }

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
                // Touch taps also synthesize mouse events (which == SDL_TOUCH_MOUSEID);
                // skip those — the SDL_FINGER* path below delivers them once.
                if (raw_mouse_event && (event.motion.which != SDL_TOUCH_MOUSEID)) {
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.motion.x, event.motion.y, 0),
                        sdl_mouse_state_to_driver(event.motion.state), 1, 0);
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                if (raw_mouse_event && (event.button.which != SDL_TOUCH_MOUSEID)) {
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.button.x, event.button.y, 0),
                        sdl_mouse_button_to_driver(event.button.button), 0, 0);
                }
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (raw_mouse_event && (event.button.which != SDL_TOUCH_MOUSEID)) {
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(event.button.x, event.button.y, 0),
                        sdl_mouse_button_to_driver(event.button.button), 2, 0);
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
                    const int slot = finger_slot_acquire(event.tfinger.fingerId);
                    if (slot < 0) {
                        break;
                    }

                    int screen_w = 0, screen_h = 0;
                    SDL_GetWindowSize(window_, &screen_w, &screen_h);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(
                            static_cast<int>(event.tfinger.x * screen_w),
                            static_cast<int>(event.tfinger.y * screen_h),
                            static_cast<int>(event.tfinger.pressure * eka2l1::PRESSURE_MAX_NUM)),
                        eka2l1::drivers::mouse_button_left, 0, slot);
                }
                break;
            }

            case SDL_FINGERMOTION: {
                if (raw_mouse_event) {
                    const int slot = finger_slot_find(event.tfinger.fingerId);
                    if (slot < 0) {
                        break;
                    }

                    int screen_w = 0, screen_h = 0;
                    SDL_GetWindowSize(window_, &screen_w, &screen_h);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(
                            static_cast<int>(event.tfinger.x * screen_w),
                            static_cast<int>(event.tfinger.y * screen_h),
                            static_cast<int>(event.tfinger.pressure * eka2l1::PRESSURE_MAX_NUM)),
                        eka2l1::drivers::mouse_button_left, 1, slot);
                }
                break;
            }

            case SDL_FINGERUP: {
                if (raw_mouse_event) {
                    const int slot = finger_slot_find(event.tfinger.fingerId);
                    if (slot < 0) {
                        break;
                    }
                    active_pointers_[slot] = 0;

                    // Release must carry the lift position: apps cancel a tap
                    // when the pointer-up lands outside the pressed control,
                    // which is what the old (0,0) release did to every tap.
                    int screen_w = 0, screen_h = 0;
                    SDL_GetWindowSize(window_, &screen_w, &screen_h);
                    raw_mouse_event(userdata_,
                        eka2l1::vec3(
                            static_cast<int>(event.tfinger.x * screen_w),
                            static_cast<int>(event.tfinger.y * screen_h),
                            0),
                        eka2l1::drivers::mouse_button_left, 2, slot);
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

        // Fit the *rotated* footprint into the window (mirrors the Qt
        // frontend): with 90/270 the screen occupies height x width.
        eka2l1::vec2 size = crr_mode.size;
        if ((scr->ui_rotation % 180) != 0) {
            std::swap(size.x, size.y);
        }

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

        src.size = crr_mode.size;

        eka2l1::drivers::advance_draw_pos_around_origin(dest, scr->ui_rotation);

        if (scr->ui_rotation % 180 != 0) {
            std::swap(dest.size.x, dest.size.y);
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

#ifdef __EMSCRIPTEN__
    // Attach SDL's keyboard listeners to the canvas instead of the whole
    // window (the default). The default handler preventDefault()s every
    // keystroke on the page, which swallows plain (non-IME) typing into HTML
    // inputs — e.g. English text in the library page's search box. With the
    // canvas as the target, the emulator only captures keys while the canvas
    // has focus (the player page focuses it when gameplay starts).
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif

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

    // Web Audio backend: PCM is pulled on the main thread each frame and
    // scheduled as AudioBuffer chunks (see web_audio.cpp for why SDL audio
    // is unusable here).
    g_state.audio_driver = std::make_unique<eka2l1::drivers::web_audio_driver>(
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

    // Cap at 60fps. The loop runs on requestAnimationFrame, which fires at
    // display refresh — 120Hz on ProMotion phones/laptops would double the
    // per-second CPU burn for no visible benefit. Skipped ticks still polled
    // events above and keep the audio queue fed below.
    static double s_last_frame_ms = 0.0;
    const double now_ms = emscripten_get_now();
    if ((now_ms - s_last_frame_ms) < 15.5) {
        if (g_state.audio_driver) {
            static_cast<eka2l1::drivers::web_audio_driver *>(g_state.audio_driver.get())->pump();
        }
        return;
    }
    const double raf_interval_ms = (s_last_frame_ms > 0.0) ? (now_ms - s_last_frame_ms) : 16.6;
    s_last_frame_ms = now_ms;

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
    // Until the first frame is presented (boot/app startup, e.g. ECom plugin
    // discovery takes hundreds of millions of guest instructions) spend most
    // of the frame on the guest.
    //
    // Post-boot the budget is a hard speed cap for CPU-hungry 3D games: at
    // the old 8ms a game wanting more than 8ms of guest CPU per 16.6ms frame
    // ran at ~50% no matter how fast the device was. 11ms leaves ~5ms for GL
    // submission/audio/compositor at 60Hz and lifts that cap by ~37%.
    // (A feedback controller keyed on RAF intervals was tried and reverted:
    // on slow/loaded machines long intervals made it starve the guest — the
    // opposite of the intent.)
    const bool boot_phase = (s_redraw_cb_count.load() == 0);
    const double FRAME_CPU_BUDGET_MS = boot_phase ? 16.0 : 11.0;
    const int MAX_SLICES_PER_FRAME = boot_phase ? 96 : 32;

    (void)raf_interval_ms;

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

    // Feed Web Audio: pull PCM from playing guest streams and schedule it.
    if (g_state.audio_driver) {
        static_cast<eka2l1::drivers::web_audio_driver *>(g_state.audio_driver.get())->pump();
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
/*
 * Streaming (push-mode) RPKG install.
 *
 * The classic path (wasm_init_with_rom) needs the whole RPKG staged in MEMFS
 * first — a full extra copy in the JS heap, which on iOS Safari is the
 * difference between installing and the tab getting jetsam-killed
 * ("立即刷新"). Here JS feeds the picked File in small chunks; the container
 * is parsed and extracted as the bytes arrive and is never resident.
 *
 * JS protocol:
 *   wasm_rpkg_stream_begin() -> 0 | negative error
 *   loop: ptr = wasm_rpkg_stream_buffer(n); HEAPU8.set(chunk, ptr);
 *         wasm_rpkg_stream_feed(n) -> 0 | negative error
 *   wasm_rpkg_stream_finish() -> 0 | -(1000+e)   (registers device, saves devices.yml)
 *   write the ROM to wasm_rpkg_stream_rom_target(), then wasm_init_with_rom("", "").
 */
static std::unique_ptr<eka2l1::loader::rpkg_stream_installer> g_rpkg_stream;
static std::vector<std::uint8_t> g_rpkg_stream_buf;
static std::string g_rpkg_stream_rom_target;

EMSCRIPTEN_KEEPALIVE
std::uint8_t *wasm_rpkg_stream_buffer(int size) {
    if (size <= 0) {
        return nullptr;
    }
    if (g_rpkg_stream_buf.size() < static_cast<std::size_t>(size)) {
        g_rpkg_stream_buf.resize(static_cast<std::size_t>(size));
    }
    return g_rpkg_stream_buf.data();
}

EMSCRIPTEN_KEEPALIVE
int wasm_rpkg_stream_begin() {
    if (!eka2l1::log::spd_logger) {
        eka2l1::log::setup_log(nullptr);
        eka2l1::log::toggle_console();
    }

    if (!g_state.initialized) {
        if (!init_emulator()) {
            return -2;
        }
    }

    eka2l1::device_manager *dvcmngr = g_state.symsys->get_device_manager();
    if (dvcmngr->total() > 0) {
        // Same encoding wasm_init_with_rom uses for "device already exists".
        return -(1000 + static_cast<int>(eka2l1::device_installation_already_exist));
    }

    const std::string &storage = g_state.conf.storage;
    eka2l1::common::create_directories(eka2l1::add_path(storage, "roms/"));
    const std::string drives_z_path = eka2l1::add_path(storage, "drives/z/");
    eka2l1::common::create_directories(drives_z_path);

    g_rpkg_stream = std::make_unique<eka2l1::loader::rpkg_stream_installer>(dvcmngr, drives_z_path);
    g_rpkg_stream_rom_target.clear();

    LOG_INFO(FRONTEND_CMDLINE, "Streaming RPKG install started");
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int wasm_rpkg_stream_feed(int size) {
    if (!g_rpkg_stream || (size < 0) || (static_cast<std::size_t>(size) > g_rpkg_stream_buf.size())) {
        return -1;
    }
    if (!g_rpkg_stream->feed(g_rpkg_stream_buf.data(), static_cast<std::size_t>(size))) {
        g_rpkg_stream.reset();
        return -(1000 + static_cast<int>(eka2l1::device_installation_rpkg_corrupt));
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int wasm_rpkg_stream_finish() {
    if (!g_rpkg_stream) {
        return -1;
    }

    std::string firmware_code;
    const eka2l1::device_installation_error err = g_rpkg_stream->finish(firmware_code);
    g_rpkg_stream.reset();
    g_rpkg_stream_buf.clear();
    g_rpkg_stream_buf.shrink_to_fit();

    if (err != eka2l1::device_installation_none) {
        LOG_ERROR(FRONTEND_CMDLINE, "Streaming RPKG install failed, error={}", static_cast<int>(err));
        return -(1000 + static_cast<int>(err));
    }

    const std::string rom_dir = eka2l1::add_path(
        eka2l1::add_path(g_state.conf.storage, "roms/"), firmware_code + "/");
    eka2l1::common::create_directories(rom_dir);
    g_rpkg_stream_rom_target = eka2l1::add_path(rom_dir, "SYM.ROM");

    // install_rpkg's callers rely on this to persist devices.yml.
    g_state.symsys->get_device_manager()->save_devices();

    LOG_INFO(FRONTEND_CMDLINE, "Streaming RPKG install done: firmcode={}", firmware_code);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
const char *wasm_rpkg_stream_rom_target() {
    return g_rpkg_stream_rom_target.c_str();
}

EMSCRIPTEN_KEEPALIVE
void wasm_rpkg_stream_abort() {
    if (g_rpkg_stream) {
        g_rpkg_stream->abort();
        g_rpkg_stream.reset();
    }
    g_rpkg_stream_buf.clear();
    g_rpkg_stream_buf.shrink_to_fit();
}

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

        // The applist server only scans registries at boot / drive changes, so
        // a fresh install stays invisible to wasm_get_app_list until reload.
        // Mirror the Qt frontend (force_refresh_applist): rescan right away —
        // on WASM the rescan path is synchronous, safe on this thread.
        eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
        if (kern) {
            auto *alserv = reinterpret_cast<eka2l1::applist_server *>(
                kern->get_by_name<eka2l1::service::server>(
                    eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));
            if (alserv) {
                alserv->rescan_registries(g_state.symsys->get_io_system());
            }
        }
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
 * Decode an app's icon into a JSON payload the page can render directly:
 *   {"type":"svg","data":"<base64 svg>"}            MIF vector icons
 *   {"type":"rgba","w":W,"h":H,"data":"<base64>"}   MBM / AIF bitmaps (mask
 *                                                   merged into alpha)
 *   null                                            no icon / decode failure
 * Mirrors the extraction logic of the Qt/Android frontends. The returned
 * pointer is valid until the next call.
 */
EMSCRIPTEN_KEEPALIVE
const char *wasm_get_app_icon(int uid) {
    static std::string s_icon_json;
    s_icon_json = "null";

    if (!g_state.symsys) {
        return s_icon_json.c_str();
    }

    eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
    if (!kern) {
        return s_icon_json.c_str();
    }

    auto *alserv = reinterpret_cast<eka2l1::applist_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::get_app_list_server_name_by_epocver(kern->get_epoc_version())));
    if (!alserv) {
        return s_icon_json.c_str();
    }

    eka2l1::apa_app_registry *reg = alserv->get_registration(static_cast<std::uint32_t>(uid));
    if (!reg) {
        return s_icon_json.c_str();
    }

    eka2l1::io_system *io = g_state.symsys->get_io_system();
    const std::u16string path_ext = eka2l1::common::lowercase_ucs2_string(
        eka2l1::path_extension(reg->icon_file_path));

    auto make_rgba_json = [](const std::uint8_t *data, const int w, const int h) {
        return "{\"type\":\"rgba\",\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h)
            + ",\"data\":\"" + eka2l1::crypt::base64_encode(data, static_cast<std::size_t>(w) * h * 4)
            + "\"}";
    };

    if (path_ext == u".mif") {
        // Vector icon: debinarize SVGB/NVG to plain SVG text — the browser
        // renders SVG natively, so no rasterizer is needed on this side.
        eka2l1::symfile file_route = io->open_file(reg->icon_file_path, READ_MODE | BIN_MODE);
        if (!file_route) {
            return s_icon_json.c_str();
        }

        eka2l1::ro_file_stream file_route_stream(file_route.get());
        eka2l1::loader::mif_file mif_parser(reinterpret_cast<eka2l1::common::ro_stream *>(&file_route_stream));
        if (!mif_parser.do_parse()) {
            return s_icon_json.c_str();
        }

        int dest_size = 0;
        if (!mif_parser.read_mif_entry(0, nullptr, dest_size) || (dest_size <= 0)) {
            return s_icon_json.c_str();
        }

        std::vector<std::uint8_t> data(dest_size);
        mif_parser.read_mif_entry(0, data.data(), dest_size);

        eka2l1::common::ro_buf_stream inside_stream(data.data(), data.size());
        eka2l1::loader::mif_icon_header header;
        inside_stream.read(&header, sizeof(header));

        // MEMFS path outside /eka2l1 so it never gets persisted to IndexedDB.
        // (wo_growable_buf_stream is ostringstream-based, which traps on WASM
        // due to the missing locale database — file round-trip instead.)
        static const char *TMP_SVG = "/icon_tmp.svg";
        bool svg_ok = false;
        {
            eka2l1::common::wo_std_file_stream out_stream(TMP_SVG, true);
            if (header.type == eka2l1::loader::mif_icon_type_svg) {
                std::vector<eka2l1::loader::svgb_convert_error_description> errors;
                if (eka2l1::loader::convert_svgb_to_svg(inside_stream, out_stream, errors)) {
                    svg_ok = true;
                } else if (!errors.empty()
                    && (errors[0].reason_ == eka2l1::loader::svgb_convert_error_invalid_file)) {
                    // Entry is already plain SVG text, pass it through.
                    out_stream.write(data.data() + sizeof(header), data.size() - sizeof(header));
                    svg_ok = true;
                }
            } else {
                eka2l1::common::ro_buf_stream nvg_stream(data.data() + sizeof(header),
                    data.size() - sizeof(header));
                std::vector<eka2l1::loader::nvg_convert_error_description> errors_nvg;
                svg_ok = eka2l1::loader::convert_nvg_to_svg(nvg_stream, out_stream, errors_nvg);
            }
        }

        if (svg_ok) {
            eka2l1::common::ro_std_file_stream svg_in(TMP_SVG, true);
            if (svg_in.valid() && svg_in.size() > 0) {
                std::vector<std::uint8_t> svg_data(svg_in.size());
                svg_in.read(svg_data.data(), svg_data.size());
                s_icon_json = "{\"type\":\"svg\",\"data\":\""
                    + eka2l1::crypt::base64_encode(svg_data.data(), svg_data.size()) + "\"}";
            }
        }

        return s_icon_json.c_str();
    }

    eka2l1::fbs_server *fbss = reinterpret_cast<eka2l1::fbs_server *>(
        kern->get_by_name<eka2l1::service::server>(
            eka2l1::epoc::get_fbs_server_name_by_epocver(kern->get_epoc_version())));
    if (!fbss) {
        return s_icon_json.c_str();
    }

    if (path_ext == u".mbm") {
        eka2l1::symfile file_route = io->open_file(reg->icon_file_path, READ_MODE | BIN_MODE);
        if (!file_route) {
            return s_icon_json.c_str();
        }

        eka2l1::ro_file_stream file_route_stream(file_route.get());
        eka2l1::loader::mbm_file mbm_parser(reinterpret_cast<eka2l1::common::ro_stream *>(&file_route_stream));
        if (!mbm_parser.do_read_headers() || mbm_parser.sbm_headers.empty()) {
            return s_icon_json.c_str();
        }

        eka2l1::loader::sbm_header *icon_header = &mbm_parser.sbm_headers[0];
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(icon_header->size_pixels.x)
            * icon_header->size_pixels.y * 4);
        eka2l1::common::wo_buf_stream rgba_stream(rgba.data(), rgba.size());

        if (eka2l1::epoc::convert_to_rgba8888(fbss, mbm_parser, 0, rgba_stream)) {
            s_icon_json = make_rgba_json(rgba.data(), icon_header->size_pixels.x,
                icon_header->size_pixels.y);
        }

        return s_icon_json.c_str();
    }

    // AIF & friends: icons were loaded into the registry during the scan.
    std::optional<eka2l1::apa_app_masked_icon_bitmap> icon_pair = alserv->get_icon(*reg, 0);
    if (!icon_pair.has_value() || !icon_pair->first) {
        return s_icon_json.c_str();
    }

    eka2l1::epoc::bitwise_bitmap *main_bitmap = icon_pair->first;
    const int w = main_bitmap->header_.size_pixels.x;
    const int h = main_bitmap->header_.size_pixels.y;

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    eka2l1::common::wo_buf_stream rgba_stream(rgba.data(), rgba.size());
    if (!eka2l1::epoc::convert_to_rgba8888(fbss, main_bitmap, rgba_stream)) {
        return s_icon_json.c_str();
    }

    if (icon_pair->second) {
        eka2l1::epoc::bitwise_bitmap *mask_bitmap = icon_pair->second;
        if ((mask_bitmap->header_.size_pixels.x == w) && (mask_bitmap->header_.size_pixels.y == h)) {
            std::vector<std::uint8_t> mask_rgba(rgba.size());
            eka2l1::common::wo_buf_stream mask_stream(mask_rgba.data(), mask_rgba.size());

            // make_standard_mask: alpha=255 where the mask pixel is pure white
            // (= icon visible); copy that straight into the icon's alpha.
            if (eka2l1::epoc::convert_to_rgba8888(fbss, mask_bitmap, mask_stream, true)) {
                for (std::size_t i = 3; i < rgba.size(); i += 4) {
                    rgba[i] = mask_rgba[i];
                }
            }
        }
    }

    s_icon_json = make_rgba_json(rgba.data(), w, h);
    return s_icon_json.c_str();
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

                // WARN so it survives the web build's warn-level filter: a
                // dying app with no visible message reads as a "hang".
                LOG_WARN(FRONTEND_CMDLINE,
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
 * Inject a key event using a raw Symbian scancode (epoc::std_scan_code).
 * Used by the on-screen keypad in the web UI; bypasses SDL entirely.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_send_key(int scancode, int pressed) {
    if (!g_state.winserv || !scancode) {
        return;
    }

    auto evt = make_key_event_driver(scancode,
        pressed ? eka2l1::drivers::key_state::pressed : eka2l1::drivers::key_state::released);
    g_state.winserv->queue_input_from_driver(evt);
}

/**
 * Number of screen redraw callbacks fired so far. Zero means no emulated
 * frame has been composed yet — the runner page polls this to know when to
 * hide its loading overlay.
 */
EMSCRIPTEN_KEEPALIVE
double wasm_get_redraw_count() {
    return static_cast<double>(s_redraw_cb_count.load());
}

/**
 * Rotate the presented screen (0/90/180/270 degrees clockwise). Landscape
 * games run sideways on the emulated portrait device; this turns the picture
 * without touching the guest. The window server's pointer transform already
 * honours ui_rotation, so touch input keeps landing on the right spot.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_screen_rotation(int rotation) {
    rotation = ((rotation % 360) + 360) % 360;
    if ((rotation % 90) != 0) {
        return;
    }

    if (!g_state.winserv) {
        return;
    }

    epoc::screen *scr = g_state.winserv->get_current_focus_screen();
    if (!scr) {
        scr = g_state.winserv->get_screens();
    }

    if (!scr) {
        return;
    }

    scr->ui_rotation = rotation;

    // Reshape the canvas so the rotated image fills it edge-to-edge. SDL
    // updates the canvas element; the resize event refreshes window_width/
    // height for draw_emulated_screen's fit.
    const eka2l1::vec2 base = scr->current_mode().size;
    eka2l1::vec2 canvas_size = base;
    if ((rotation % 180) != 0) {
        std::swap(canvas_size.x, canvas_size.y);
    }

    // Keep the emulator's original upscale factor (the SDL window is created
    // bigger than the guest screen; preserve that ratio).
    if ((base.x > 0) && (base.y > 0) && g_window) {
        const float upscale = static_cast<float>(std::max(g_state.window_width, g_state.window_height))
            / static_cast<float>(std::max(base.x, base.y));
        canvas_size.x = static_cast<int>(canvas_size.x * upscale);
        canvas_size.y = static_cast<int>(canvas_size.y * upscale);
        SDL_SetWindowSize(g_window, canvas_size.x, canvas_size.y);

        g_state.window_width = canvas_size.x;
        g_state.window_height = canvas_size.y;
    }

    LOG_INFO(FRONTEND_CMDLINE, "Screen rotation set to {} degrees", rotation);
}

static const char *thread_state_to_str(const eka2l1::kernel::thread_state st) {
    switch (st) {
    case eka2l1::kernel::thread_state::create: return "create";
    case eka2l1::kernel::thread_state::run: return "run";
    case eka2l1::kernel::thread_state::wait: return "wait";
    case eka2l1::kernel::thread_state::ready: return "ready";
    case eka2l1::kernel::thread_state::stop: return "stop";
    case eka2l1::kernel::thread_state::wait_fast_sema: return "wait_fast_sema";
    case eka2l1::kernel::thread_state::wait_mutex: return "wait_mutex";
    case eka2l1::kernel::thread_state::wait_condvar: return "wait_condvar";
    case eka2l1::kernel::thread_state::wait_mutex_suspend: return "wait_mutex_suspend";
    case eka2l1::kernel::thread_state::wait_fast_sema_suspend: return "wait_fast_sema_suspend";
    case eka2l1::kernel::thread_state::wait_condvar_suspend: return "wait_condvar_suspend";
    case eka2l1::kernel::thread_state::hold_mutex_pending: return "hold_mutex_pending";
    case eka2l1::kernel::thread_state::wait_dfc: return "wait_dfc";
    case eka2l1::kernel::thread_state::wait_hle: return "wait_hle";
    default: return "?";
    }
}

// dyncom progress counters (defined in the CPU module, WASM builds only).
extern std::atomic<std::uint64_t> eka2l1_wasm_guest_instrs_total;
extern std::atomic<std::uint64_t> eka2l1_wasm_guest_blocks_translated;

// Debug probe (defined in kernel/svc.cpp): when enabled, every guest Leave /
// thread kill / process kill / unimplemented SVC logs a guest backtrace at
// WARN level. For chasing apps that die silently during init.
extern bool eka2l1_leave_probe;

EMSCRIPTEN_KEEPALIVE
void wasm_set_leave_probe(int enabled) {
    eka2l1_leave_probe = (enabled != 0);
    std::printf("[probe] leave probe %s\n", enabled ? "ON" : "OFF");
}

/**
 * Enable/disable the dyncom hot-block wasm JIT. Call before the device is
 * activated (the flag is latched when the CPU core is created); used by the
 * frontend's ?jit=0 escape hatch.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_jit(int enabled) {
    eka2l1::arm::dyncom_jit::enabled_default = (enabled != 0);
    std::printf("[jit] dyncom wasm JIT default %s\n", enabled ? "ON" : "OFF");
}

// Bisect aid: compile at most `limit` blocks (?jitlimit=N).
EMSCRIPTEN_KEEPALIVE
void wasm_set_jit_limit(int limit) {
    eka2l1::arm::dyncom_jit::compile_limit = limit;
    std::printf("[jit] compile limit = %d\n", limit);
}

/**
 * Dump every guest thread (state, PC/LR/SP, wait object, owning module) plus
 * emulator progress counters to the console. Call it twice a few seconds
 * apart when something hangs:
 *   - instrs delta == 0  -> the guest is fully blocked (look at wait objects)
 *   - instrs delta != 0 with the same PCs -> a guest thread is spinning there
 * Runs lock-free on purpose: it must work even when the system is wedged.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_debug_dump() {
    std::printf("[dump] ================= EKA2L1 state dump =================\n");
    std::printf("[dump] guest_instrs=%llu blocks_translated=%llu redraws=%llu paused=%d\n",
        static_cast<unsigned long long>(eka2l1_wasm_guest_instrs_total.load()),
        static_cast<unsigned long long>(eka2l1_wasm_guest_blocks_translated.load()),
        static_cast<unsigned long long>(s_redraw_cb_count.load()),
        g_state.paused ? 1 : 0);
    {
        const std::uint64_t total = eka2l1_wasm_guest_instrs_total.load();
        const std::uint64_t jit = eka2l1::arm::dyncom_jit::stat_jit_instrs;
        std::printf("[dump] jit: default=%d compiled=%u rejected=%u jit_instrs=%llu (%.1f%% of guest)\n",
            eka2l1::arm::dyncom_jit::enabled_default,
            eka2l1::arm::dyncom_jit::stat_compiled,
            eka2l1::arm::dyncom_jit::stat_rejected,
            static_cast<unsigned long long>(jit),
            total ? (100.0 * static_cast<double>(jit) / static_cast<double>(total)) : 0.0);

        // Top compile blockers (first unsupported instruction kind seen).
        std::printf("[dump] jit blockers:");
        for (int rank = 0; rank < 10; rank++) {
            std::uint32_t best = 0;
            int best_idx = -1;
            for (int i = 0; i < 224; i++) {
                if (eka2l1::arm::dyncom_jit::stat_blocker_hist[i] > best) {
                    best = eka2l1::arm::dyncom_jit::stat_blocker_hist[i];
                    best_idx = i;
                }
            }
            if (best_idx < 0 || best == 0) break;
            std::printf(" idx%d=%u", best_idx, best);
            eka2l1::arm::dyncom_jit::stat_blocker_hist[best_idx] = 0; // consume for ranking
        }
        std::printf("\n");
    }

    if (!g_state.symsys) {
        std::printf("[dump] system not created\n");
        return;
    }

    eka2l1::kernel_system *kern = g_state.symsys->get_kernel_system();
    if (!kern) {
        std::printf("[dump] kernel not created\n");
        return;
    }

    eka2l1::arm::core *cpu = kern->get_cpu();
    eka2l1::kernel::thread *crr = kern->crr_thread();

    for (auto &obj : kern->get_thread_list()) {
        eka2l1::kernel::thread *thr = reinterpret_cast<eka2l1::kernel::thread *>(obj.get());
        if (!thr) {
            continue;
        }

        eka2l1::kernel::process *pr = thr->owning_process();

        std::uint32_t pc = 0, lr = 0, sp = 0;
        if (thr == crr && cpu) {
            pc = cpu->get_pc();
            lr = cpu->get_lr();
            sp = cpu->get_sp();
        } else {
            const eka2l1::arm::core::thread_context &ctx = thr->get_thread_context();
            pc = ctx.cpu_registers[15];
            lr = ctx.cpu_registers[14];
            sp = ctx.cpu_registers[13];
        }

        // Resolve PC to the codeseg (module) containing it.
        std::string pc_module = "?";
        std::uint32_t pc_offset = 0;
        if (pr) {
            for (auto &seg_obj : kern->get_codeseg_list()) {
                eka2l1::codeseg_ptr seg = reinterpret_cast<eka2l1::codeseg_ptr>(seg_obj.get());
                if (!seg) {
                    continue;
                }
                const eka2l1::address beg = seg->get_code_run_addr(pr);
                if (beg && (pc >= beg) && (pc < beg + seg->get_text_size())) {
                    pc_module = seg->name();
                    pc_offset = pc - beg;
                    break;
                }
            }
        }

        std::printf("[dump] %s thread='%s' proc='%s' state=%s pc=0x%08X (%s+0x%X) lr=0x%08X sp=0x%08X wait_obj='%s'\n",
            (thr == crr) ? "*" : " ",
            thr->name().c_str(),
            pr ? pr->name().c_str() : "?",
            thread_state_to_str(thr->current_state()),
            pc, pc_module.c_str(), pc_offset, lr, sp,
            thr->wait_obj ? thr->wait_obj->name().c_str() : "");
    }

    std::printf("[dump] =====================================================\n");
    std::fflush(stdout);
}

/**
 * Get the current FPS.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_get_fps() {
    return g_state.current_fps;
}

/**
 * Debug: dump the focus screen's guest framebuffer (the DSA chunk) to the
 * console as base64, with the display-mode metadata needed to decode it
 * offline. For diagnosing garbled-screen reports: the chunk shows what the
 * guest actually drew, before any host-side interpretation.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_dump_screen() {
    if (!g_state.winserv) {
        std::printf("[screendump] no winserv\n");
        return;
    }

    epoc::screen *scr = g_state.winserv->get_current_focus_screen();
    if (!scr || !scr->screen_buffer_chunk) {
        std::printf("[screendump] no screen/chunk\n");
        return;
    }

    const eka2l1::vec2 size = scr->current_mode().size;
    const int bpp = epoc::get_bpp_from_display_mode(scr->disp_mode);
    const std::uint8_t *data = scr->screen_buffer_ptr();
    const std::size_t total = static_cast<std::size_t>(size.x) * size.y * 4;

    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((total + 2) / 3) * 4);
    for (std::size_t i = 0; i < total; i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = (i + 1 < total) ? data[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < total) ? data[i + 2] : 0;
        const std::uint32_t n = (b0 << 16) | (b1 << 8) | b2;
        out += B64[(n >> 18) & 63];
        out += B64[(n >> 12) & 63];
        out += B64[(n >> 6) & 63];
        out += B64[n & 63];
    }

    std::printf("[screendump] w=%d h=%d mode=%d bpp=%d bytes=%zu\n",
        size.x, size.y, static_cast<int>(scr->disp_mode), bpp, total);
    std::printf("[screendump-data] %s\n", out.c_str());
    std::fflush(stdout);
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
