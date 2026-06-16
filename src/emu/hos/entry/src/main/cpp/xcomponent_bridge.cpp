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

// IMPORTANT: this TU must NOT include any EKA2L1 header. The XComponent SDK
// header defines a global KEY_* enum that collides with EKA2L1's keycode.inc.
#include "xcomponent_bridge.h"

#include <ace/xcomponent/native_interface_xcomponent.h>

namespace eka2l1::hos::xcbridge {
    namespace {
        surface_handlers g_handlers;

        void on_surface_created(OH_NativeXComponent *component, void *window) {
            uint64_t width = 0;
            uint64_t height = 0;
            OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);
            if (g_handlers.on_created) {
                g_handlers.on_created(window, static_cast<int>(width), static_cast<int>(height));
            }
        }

        void on_surface_changed(OH_NativeXComponent *component, void *window) {
            uint64_t width = 0;
            uint64_t height = 0;
            OH_NativeXComponent_GetXComponentSize(component, window, &width, &height);
            if (g_handlers.on_changed) {
                g_handlers.on_changed(window, static_cast<int>(width), static_cast<int>(height));
            }
        }

        void on_surface_destroyed(OH_NativeXComponent *component, void *window) {
            if (g_handlers.on_destroyed) {
                g_handlers.on_destroyed(window);
            }
        }

        void on_dispatch_touch_event(OH_NativeXComponent *component, void *window) {
            OH_NativeXComponent_TouchEvent touch_event;
            if (OH_NativeXComponent_GetTouchEvent(component, window, &touch_event) != 0) {
                return;
            }

            // Map XComponent touch type to the Android-style action the emulator
            // input layer expects: 0 = press, 1 = move, 2 = release.
            int action = -1;
            switch (touch_event.type) {
            case OH_NATIVEXCOMPONENT_DOWN:
                action = 0;
                break;
            case OH_NATIVEXCOMPONENT_MOVE:
                action = 1;
                break;
            case OH_NATIVEXCOMPONENT_UP:
            case OH_NATIVEXCOMPONENT_CANCEL:
                action = 2;
                break;
            default:
                return;
            }

            if (!g_handlers.on_touch) {
                return;
            }

            for (uint32_t i = 0; i < touch_event.numPoints; i++) {
                const OH_NativeXComponent_TouchPoint &pt = touch_event.touchPoints[i];
                g_handlers.on_touch(static_cast<int>(pt.x), static_cast<int>(pt.y), action, pt.id);
            }
        }

        OH_NativeXComponent_Callback g_callback = {
            .OnSurfaceCreated = on_surface_created,
            .OnSurfaceChanged = on_surface_changed,
            .OnSurfaceDestroyed = on_surface_destroyed,
            .DispatchTouchEvent = on_dispatch_touch_event,
        };
    }

    void register_xcomponent(napi_env env, napi_value exports, const surface_handlers &handlers) {
        g_handlers = handlers;

        napi_value export_instance = nullptr;
        if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &export_instance) != napi_ok) {
            return;
        }

        OH_NativeXComponent *native_xcomponent = nullptr;
        if (napi_unwrap(env, export_instance, reinterpret_cast<void **>(&native_xcomponent)) != napi_ok) {
            return;
        }

        if (native_xcomponent) {
            OH_NativeXComponent_RegisterCallback(native_xcomponent, &g_callback);
        }
    }
}
