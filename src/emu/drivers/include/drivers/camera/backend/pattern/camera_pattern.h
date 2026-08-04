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
#include <vector>

namespace eka2l1::drivers::camera {
    // Portable test-pattern camera used by WASM (no getUserMedia required to
    // exercise ECam) and as a fallback when the Android backend reports zero
    // devices. Mirrors yeatse's iOS simulator fixture: colour bars, corner
    // marker, grid, grey ramp, sweeping bar.

    extern const frame_format PATTERN_SUPPORTED_FORMATS[7];

    bool pattern_is_supported_format(const frame_format format);

    // Repack top-down BGRA into the guest-facing layout (same rules as the
    // Android / iOS backends).
    bool pattern_convert_bgra_to_guest(const std::uint8_t *src, const std::size_t src_stride,
        const int width, const int height, const frame_format format,
        std::vector<std::uint8_t> &dest);

    // JPEG-encode tightly packed top-down BGRA via stb_image_write.
    bool pattern_encode_bgra_to_jpeg(const std::uint8_t *bgra, const int width, const int height,
        std::vector<std::uint8_t> &out);

    class collection_pattern : public collection {
    public:
        std::uint32_t count() const override;
        std::unique_ptr<instance> make_camera(const std::uint32_t camera_index) override;
    };
}
