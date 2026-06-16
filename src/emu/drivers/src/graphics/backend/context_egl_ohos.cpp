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

#include <native_window/external_window.h>

namespace eka2l1::drivers::graphics {
    gl_context_egl_ohos::gl_context_egl_ohos(const window_system_info& wsi, bool stereo, bool core)
        : gl_context_egl(wsi, stereo, core, true) {
    }

    void gl_context_egl_ohos::create_surface() {
        if (!render_window) {
            return;
        }

        // The XComponent's OHNativeWindow defaults to an RGBA8888 buffer, but the
        // EGLConfig we picked may be RGB(X)888 (no alpha). eglCreateWindowSurface
        // then fails with EGL_BAD_MATCH (pixel format) and every later draw hits
        // "EGLSurface is invalid" (EGL_BAD_SURFACE 0x300D) -> the screen stays
        // black even though the render loop and FPS keep running.
        //
        // Force the native window's buffer format to match the chosen config's
        // native visual id before creating the surface - the OHOS equivalent of
        // Android's ANativeWindow_setBuffersGeometry(format) in
        // context_egl_android.cpp.
        EGLint format = 0;
        if (eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &format) == EGL_TRUE && format != 0) {
            OHNativeWindow *window = reinterpret_cast<OHNativeWindow *>(render_window);
            const int32_t ret = OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, format);
            if (ret != 0) {
                LOG_WARN(DRIVER_GRAPHICS, "OHOS native window SET_FORMAT {} failed: {}", format, ret);
            }
        } else {
            LOG_WARN(DRIVER_GRAPHICS, "Could not read EGL_NATIVE_VISUAL_ID; native window format left as-is");
        }

        gl_context_egl::create_surface();
    }
}
