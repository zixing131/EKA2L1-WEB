/*
 * Copyright (c) 2024 EKA2L1 Team.
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

#include "context_emscripten.h"
#include <common/log.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

namespace eka2l1::drivers::graphics {
    gl_context_emscripten::gl_context_emscripten(const drivers::window_system_info &system_info, bool stereo, bool core)
        : window_(static_cast<SDL_Window*>(system_info.render_window))
        , context_(nullptr) {
        // Use OpenGL ES 3.0 (maps to WebGL 2.0)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

        // If a window was provided, create the GL context from it
        if (window_) {
            context_ = SDL_GL_CreateContext(window_);
            if (!context_) {
                LOG_ERROR(DRIVER_GRAPHICS, "Failed to create SDL GL context: {}", SDL_GetError());
                return;
            }

            // Make context current
            SDL_GL_MakeCurrent(window_, context_);

            // Get dimensions
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window_, &w, &h);
            m_backbuffer_width = static_cast<std::uint32_t>(w);
            m_backbuffer_height = static_cast<std::uint32_t>(h);
        }

        m_opengl_mode = mode::opengl_es;
    }

    gl_context_emscripten::~gl_context_emscripten() {
        if (context_) {
            SDL_GL_DeleteContext(context_);
            context_ = nullptr;
        }
    }

    bool gl_context_emscripten::make_current() {
        if (!window_ || !context_) {
            return false;
        }

        return SDL_GL_MakeCurrent(window_, context_) == 0;
    }

    bool gl_context_emscripten::clear_current() {
        if (!window_) {
            return false;
        }

        return SDL_GL_MakeCurrent(window_, nullptr) == 0;
    }

    void gl_context_emscripten::swap_buffers() {
        if (window_) {
            SDL_GL_SwapWindow(window_);
        }
    }

    void gl_context_emscripten::update(const std::uint32_t new_width, const std::uint32_t new_height) {
        m_backbuffer_width = new_width;
        m_backbuffer_height = new_height;
    }

    void gl_context_emscripten::set_swap_interval(const std::int32_t interval) {
        SDL_GL_SetSwapInterval(interval);
    }

    bool gl_context_emscripten::is_headless() const {
        return window_ == nullptr;
    }

    void gl_context_emscripten::update_surface(void *new_surface) {
        window_ = static_cast<SDL_Window*>(new_surface);
    }

    std::unique_ptr<gl_context> gl_context_emscripten::create_shared_context() {
        // Shared contexts are not well supported in WebGL
        // Return nullptr for now - the emulator can still work without it
        return nullptr;
    }
}