/*
 * Copyright (c) 2019 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project 
 * (see bentokun.github.com/EKA2L1).
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

#include <drivers/graphics/backend/ogl/buffer_ogl.h>
#include <drivers/graphics/backend/ogl/common_ogl.h>

namespace eka2l1::drivers {
    static GLenum get_usage_hint(const buffer_upload_hint usage_hint) {
        if (usage_hint & buffer_upload_static) {
            if (usage_hint & buffer_upload_draw) {
                return GL_STATIC_DRAW;
            }

            if (usage_hint & buffer_upload_copy) {
                return GL_STATIC_COPY;
            }

            if (usage_hint & buffer_upload_read) {
                return GL_STATIC_READ;
            }

            return GL_INVALID_ENUM;
        }

        if (usage_hint & buffer_upload_dynamic) {
            if (usage_hint & buffer_upload_draw) {
                return GL_DYNAMIC_DRAW;
            }

            if (usage_hint & buffer_upload_copy) {
                return GL_DYNAMIC_COPY;
            }

            if (usage_hint & buffer_upload_read) {
                return GL_DYNAMIC_READ;
            }

            return GL_INVALID_ENUM;
        }

        if (usage_hint & buffer_upload_stream) {
            if (usage_hint & buffer_upload_draw) {
                return GL_STREAM_DRAW;
            }

            if (usage_hint & buffer_upload_copy) {
                return GL_STREAM_COPY;
            }

            if (usage_hint & buffer_upload_read) {
                return GL_STREAM_READ;
            }

            return GL_INVALID_ENUM;
        }

        return GL_INVALID_ENUM;
    }

    ogl_buffer::ogl_buffer()
        : buffer_(0)
        , last_buffer_(0)
        , last_vao_(0)
        , is_index_(false)
        , size_(0) {
    }

    ogl_buffer::~ogl_buffer() {
        if (buffer_ != 0) {
            glDeleteBuffers(1, &buffer_);
        }
    }

    // All data operations go through GL_COPY_WRITE_BUFFER (core since
    // GL 3.1 / GLES 3.0). Two reasons:
    //  - WebGL type-locks a buffer to ARRAY vs ELEMENT_ARRAY on its first
    //    such binding. Uploading через GL_ARRAY_BUFFER permanently locked
    //    every buffer as a vertex buffer, so binding one as an index buffer
    //    later silently failed and indexed draws vanished (Jelly Chase's
    //    in-game scene). COPY_WRITE is type-neutral: the deciding bind now
    //    happens at first USE (ARRAY in the input descriptors, ELEMENT in
    //    draw_indexed).
    //  - GL_ELEMENT_ARRAY_BUFFER binding is VAO state; touching it during
    //    uploads would corrupt whichever VAO happens to be bound.
    // WebGL type-locks every buffer to "element" vs "non-element" on its
    // first binding to any target (COPY_WRITE included). Index buffers must
    // therefore only ever be bound as GL_ELEMENT_ARRAY_BUFFER — and since
    // that binding lives inside the current VAO, uploads temporarily switch
    // to the default VAO so no input-descriptor VAO gets its element binding
    // clobbered. Restoring the previous VAO restores its element state.
    void ogl_buffer::bind(graphics_driver *driver) {
        if (buffer_) {
            if (is_index_) {
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, reinterpret_cast<GLint *>(&last_vao_));
                glBindVertexArray(0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer_);
            } else {
                glGetIntegerv(GL_ARRAY_BUFFER_BINDING, reinterpret_cast<GLint *>(&last_buffer_));
                glBindBuffer(GL_ARRAY_BUFFER, buffer_);
            }
        }
    }

    void ogl_buffer::unbind(graphics_driver *driver) {
        if (buffer_) {
            if (is_index_) {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glBindVertexArray(last_vao_);
            } else {
                glBindBuffer(GL_ARRAY_BUFFER, last_buffer_);
            }
        }
    }

    bool ogl_buffer::create(graphics_driver *driver, const void *data, const std::size_t initial_size, const buffer_upload_hint use_hint) {
        usage_hint_gl_ = get_usage_hint(use_hint);
        const bool want_index = (use_hint & buffer_upload_index) != 0;

        // A recycled handle switching roles needs a fresh GL name: the old
        // one is permanently type-locked by WebGL.
        if (buffer_ && (want_index != is_index_)) {
            glDeleteBuffers(1, &buffer_);
            buffer_ = 0;
        }

        is_index_ = want_index;

        if (!buffer_) {
            glGenBuffers(1, &buffer_);
        }

        const GLenum target = is_index_ ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;

        // Prealloc data first
        bind(driver);
        glBufferData(target, initial_size, data, usage_hint_gl_);
        unbind(driver);

        size_ = initial_size;

        return true;
    }

    void ogl_buffer::update_data(graphics_driver *driver, const void *data, const std::size_t offset, std::size_t size) {
        const GLenum target = is_index_ ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;

        bind(driver);

        if (offset + size > size_) {
            if (offset == 0) {
                // Orphan the buffer
                glBufferData(target, offset + size, nullptr, usage_hint_gl_);
            }

            size_ = offset + size;
        }

        // Use subdata to update
        glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLintptr>(size), data);

        unbind(driver);
    }
}
