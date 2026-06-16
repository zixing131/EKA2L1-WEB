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

#pragma once

#include "context_egl.h"

namespace eka2l1::drivers::graphics {
    // HarmonyOS / OpenHarmony EGL context. The render window is the
    // OHNativeWindow* handed out by the XComponent surface callback. It is a
    // valid EGLNativeWindowType, so surface creation reuses the EGL base path.
    class gl_context_egl_ohos final : public gl_context_egl {
    public:
        explicit gl_context_egl_ohos() = default;
        explicit gl_context_egl_ohos(const window_system_info& wsi, bool stereo, bool core);

    protected:
        virtual void create_surface() override;
    };
}
