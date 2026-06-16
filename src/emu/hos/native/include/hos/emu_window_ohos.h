/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project
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
#pragma once

#include <common/vecx.h>
#include <drivers/graphics/emu_window.h>

#include <functional>

namespace eka2l1 {
    namespace drivers {
        // HarmonyOS render window. The native surface is the OHNativeWindow* that
        // the ArkUI XComponent hands to the NAPI layer (OnSurfaceCreated). It is a
        // valid EGLNativeWindowType, so the EGL context just uses it directly.
        //
        // This mirrors emu_window_android: the class itself is a thin holder; the
        // real GL context lives in the graphics driver thread (see thread.cpp).
        class emu_window_ohos : public emu_window {
            eka2l1::vec2 fb_size;
            void *userdata;

        public:
            explicit emu_window_ohos();

            // surf is an OHNativeWindow* (kept as void* to avoid leaking the OHOS
            // window header into the shared drivers layer).
            void surface_changed(void *surf, int width, int height);

            bool get_mouse_button_hold(const int mouse_btt) override;

            void change_title(std::string new_title) override;

            void init(std::string title, vec2 size, const std::uint32_t flags) override;
            void make_current() override;
            void done_current() override;
            void swap_buffer() override;
            void poll_events() override;
            void set_userdata(void *userdata) override;
            void *get_userdata() override;
            void set_fullscreen(const bool is_fullscreen) override;

            bool should_quit() override;

            void shutdown() override;

            vec2 window_size() override;
            vec2 window_fb_size() override;
            vec2d get_mouse_pos() override;
            bool set_cursor(cursor *cur) override;
            void cursor_visiblity(const bool visi) override;
            bool cursor_visiblity() override;
        };
    }
}
