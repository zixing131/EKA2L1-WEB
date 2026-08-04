/*
 * Copyright (c) 2026 EKA2L1 Team.
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

#include <drivers/camera/camera.h>
#include <drivers/camera/camera_collection.h>

#include <cstdint>
#include <memory>

namespace eka2l1::drivers::camera {
    // Browser getUserMedia camera for the WASM frontend. Samples the live
    // MediaStream on the main thread (DOM APIs are main-only under pthreads)
    // into a shared RGBA buffer; a C++ feed thread converts to guest formats
    // with the same helpers as the pattern backend.

    class collection_web : public collection {
    public:
        std::uint32_t count() const override;
        std::unique_ptr<instance> make_camera(const std::uint32_t camera_index) override;
    };

    // Ask the browser for camera permission / warm the default stream.
    // Returns 1 if a request was kicked off (or already granted), 0 if
    // MediaDevices is unavailable. Safe to call from JS on a user gesture.
    int web_request_camera_permission();
}
