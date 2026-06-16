/*
 * Copyright (c) 2024 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project.
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

#include "context_egl_ohos.h"
#include <common/log.h>

namespace eka2l1::drivers::graphics {
    gl_context_egl_ohos::gl_context_egl_ohos(const window_system_info& wsi, bool stereo, bool core)
        : gl_context_egl(wsi, stereo, core, true) {
    }

    void gl_context_egl_ohos::create_surface() {
        if (!render_window) {
            return;
        }

        // The XComponent already owns the OHNativeWindow and has configured its
        // buffer geometry/format. Unlike Android we do not call
        // ANativeWindow_setBuffersGeometry here; eglCreateWindowSurface in the
        // base class accepts the OHNativeWindow* directly.
        gl_context_egl::create_surface();
    }
}
