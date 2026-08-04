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

#include <drivers/camera/backend/pattern/camera_pattern.h>

// Keep the writer static to this TU so we don't collide with frontend
// translation units that also pull in stb_image_write.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

#include <vector>

namespace eka2l1::drivers::camera {
    const frame_format PATTERN_SUPPORTED_FORMATS[7] = {
        FRAME_FORMAT_ARGB8888, FRAME_FORMAT_JPEG, FRAME_FORMAT_RGB565,
        FRAME_FORMAT_FBSBMP_COLOR64K, FRAME_FORMAT_FBSBMP_COLOR16M,
        FRAME_FORMAT_FBSBMP_COLOR16MU, FRAME_FORMAT_EXIF
    };

    bool pattern_is_supported_format(const frame_format format) {
        for (const frame_format supported : PATTERN_SUPPORTED_FORMATS) {
            if (format == supported) {
                return true;
            }
        }

        return false;
    }

    bool pattern_convert_bgra_to_guest(const std::uint8_t *src, const std::size_t src_stride,
        const int width, const int height, const frame_format format, std::vector<std::uint8_t> &dest) {
        switch (format) {
        case FRAME_FORMAT_FBSBMP_COLOR16MU: {
            const std::size_t dest_stride = static_cast<std::size_t>(width) * 4;
            dest.resize(dest_stride * height);

            for (int y = 0; y < height; y++) {
                const std::uint8_t *src_row = src + y * src_stride;
                std::uint8_t *dest_row = dest.data() + y * dest_stride;

                for (int x = 0; x < width; x++) {
                    dest_row[x * 4 + 0] = src_row[x * 4 + 0];
                    dest_row[x * 4 + 1] = src_row[x * 4 + 1];
                    dest_row[x * 4 + 2] = src_row[x * 4 + 2];
                    dest_row[x * 4 + 3] = 0xFF;
                }
            }

            return true;
        }

        case FRAME_FORMAT_ARGB8888: {
            const std::size_t dest_stride = static_cast<std::size_t>(width) * 4;
            dest.resize(dest_stride * height);

            for (int y = 0; y < height; y++) {
                const std::uint8_t *src_row = src + y * src_stride;
                std::uint8_t *dest_row = dest.data() + y * dest_stride;

                for (int x = 0; x < width; x++) {
                    dest_row[x * 4 + 0] = src_row[x * 4 + 2];
                    dest_row[x * 4 + 1] = src_row[x * 4 + 1];
                    dest_row[x * 4 + 2] = src_row[x * 4 + 0];
                    dest_row[x * 4 + 3] = 0xFF;
                }
            }

            return true;
        }

        case FRAME_FORMAT_FBSBMP_COLOR16M: {
            const std::size_t dest_stride = (static_cast<std::size_t>(width) * 3 + 3) / 4 * 4;
            dest.resize(dest_stride * height);

            for (int y = 0; y < height; y++) {
                const std::uint8_t *src_row = src + y * src_stride;
                std::uint8_t *dest_row = dest.data() + y * dest_stride;

                for (int x = 0; x < width; x++) {
                    dest_row[x * 3 + 0] = src_row[x * 4 + 0];
                    dest_row[x * 3 + 1] = src_row[x * 4 + 1];
                    dest_row[x * 3 + 2] = src_row[x * 4 + 2];
                }
            }

            return true;
        }

        case FRAME_FORMAT_FBSBMP_COLOR64K:
        case FRAME_FORMAT_RGB565: {
            const std::size_t dest_stride = (format == FRAME_FORMAT_FBSBMP_COLOR64K)
                ? (static_cast<std::size_t>(width) * 2 + 3) / 4 * 4
                : static_cast<std::size_t>(width) * 2;
            dest.resize(dest_stride * height);

            for (int y = 0; y < height; y++) {
                const std::uint8_t *src_row = src + y * src_stride;
                std::uint8_t *dest_row = dest.data() + y * dest_stride;

                for (int x = 0; x < width; x++) {
                    const std::uint16_t pixel = static_cast<std::uint16_t>(
                        ((src_row[x * 4 + 0] & 0xF8) >> 3) |
                        ((src_row[x * 4 + 1] & 0xFC) << 3) |
                        ((src_row[x * 4 + 2] & 0xF8) << 8));

                    dest_row[x * 2 + 0] = static_cast<std::uint8_t>(pixel & 0xFF);
                    dest_row[x * 2 + 1] = static_cast<std::uint8_t>(pixel >> 8);
                }
            }

            return true;
        }

        default:
            break;
        }

        return false;
    }

    bool pattern_encode_bgra_to_jpeg(const std::uint8_t *bgra, const int width, const int height,
        std::vector<std::uint8_t> &out) {
        // stb wants RGB(A) with R first; our buffer is BGRA.
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * 4 * height);
        for (int i = 0; i < width * height; i++) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = 0xFF;
        }

        out.clear();
        auto append = [](void *context, void *data, int size) {
            auto *dest = static_cast<std::vector<std::uint8_t> *>(context);
            const auto *bytes = static_cast<const std::uint8_t *>(data);
            dest->insert(dest->end(), bytes, bytes + size);
        };

        return stbi_write_jpg_to_func(append, &out, width, height, 4, rgba.data(), 50) != 0;
    }
}
