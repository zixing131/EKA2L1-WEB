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

#include "napi/native_api.h"

// The XComponent SDK header (<ace/xcomponent/...>) defines a global key-code
// enum (KEY_TAB, KEY_HOME, ...) that collides with EKA2L1's keycode.inc. To keep
// them apart, ALL XComponent usage lives in xcomponent_bridge.cpp, which never
// includes EKA2L1 headers. This header exposes only plain callbacks so napi_init
// (which does include EKA2L1 headers) can stay XComponent-free.

namespace eka2l1::hos::xcbridge {
    // Surface lifecycle, forwarded to the emulator on the main module side.
    // `window` is the OHNativeWindow* (passed back as void*).
    struct surface_handlers {
        void (*on_created)(void *window, int width, int height) = nullptr;
        void (*on_changed)(void *window, int width, int height) = nullptr;
        void (*on_destroyed)(void *window) = nullptr;
        // A single touch point: action is 0=press, 1=move, 2=release.
        void (*on_touch)(int x, int y, int action, int pointer_id) = nullptr;
    };

    // Register the surface handlers and hook the XComponent from module exports.
    void register_xcomponent(napi_env env, napi_value exports, const surface_handlers &handlers);
}
