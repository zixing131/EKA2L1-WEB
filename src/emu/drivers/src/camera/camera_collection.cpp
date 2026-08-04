/*
 * Copyright (c) 2022 EKA2L1 Team.
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

#include <drivers/camera/camera_collection.h>
#include <drivers/camera/backend/null/camera_collection_null.h>
#include <drivers/camera/backend/pattern/camera_pattern.h>

#include <common/platform.h>

#if EKA2L1_PLATFORM(ANDROID)
#include <drivers/camera/backend/android/camera_collection_android.h>
#endif

namespace eka2l1::drivers::camera {
    std::unique_ptr<collection> collection_detail = nullptr;

    collection *get_collection() {
        if (collection_detail == nullptr) {
#if EKA2L1_PLATFORM(ANDROID)
            collection_detail = std::make_unique<collection_android>();

            // Emulators / locked-down devices can report zero CameraX devices.
            // Fall back to the portable test-pattern pair so the guest ECam
            // path stays reachable (same idea as yeatse's iOS simulator fixture).
            if (collection_detail->count() == 0) {
                collection_detail = std::make_unique<collection_pattern>();
            }
#elif EKA2L1_PLATFORM(WASM)
            collection_detail = std::make_unique<collection_pattern>();
#else
            collection_detail = std::make_unique<collection_null>();
#endif
        }

        return collection_detail.get();
    }
}
