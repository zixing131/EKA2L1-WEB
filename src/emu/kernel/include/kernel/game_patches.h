/* Copyright (c) 2026 EKA2L1 Team. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eka2l1::kernel {
    // Super Miners' Nokia renderer assumes a 2048-pixel hardware scanline.
    // Select its existing CFbsBitmap renderer instead of writing directly to
    // that hardware layout. Match the known ARM constructor and UID before
    // changing any bytes; unknown builds must remain untouched.
    inline bool apply_super_miners_bitmap_fallback(const std::uint32_t uid3,
        std::uint8_t *code, const std::size_t size) {
        constexpr std::size_t constructor = 0x8654;
        constexpr std::size_t select_renderer = 0x86A0;
        static constexpr std::uint8_t prologue[] = {
            0xF8, 0x40, 0x2D, 0xE9, 0x00, 0x40, 0xA0, 0xE1,
            0x00, 0x60, 0xA0, 0xE3, 0x20, 0x60, 0x80, 0xE5
        };
        static constexpr std::uint8_t selection[] = {
            0x02, 0x00, 0x50, 0xE3, // CMP r0,#2 (Nokia manufacturer)
            0x38, 0x60, 0x84, 0x15, // STRNE r6,[r4,#0x38] (bitmap renderer)
            0x5A, 0x0F, 0xA0, 0x13, // MOVNE r0,#360 (bitmap pitch)
            0x02, 0x0B, 0xA0, 0x03, // MOVEQ r0,#2048 (hardware pitch)
            0x38, 0x70, 0x84, 0x05, // STREQ r7,[r4,#0x38] (direct renderer)
            0x04, 0x00, 0x85, 0xE5  // STR r0,[r5,#4]
        };
        if (uid3 != 0x2002517C || !code || size != 0x764D0
            || std::memcmp(code + constructor, prologue, sizeof(prologue)) != 0
            || std::memcmp(code + select_renderer, selection, sizeof(selection)) != 0) {
            return false;
        }
        // CMP r6,#1: r6 is initialized to zero above and preserved across the
        // intervening calls. NE selects the bitmap path without clobbering a
        // register or changing any instruction/import/relocation offsets.
        static constexpr std::uint8_t bitmap_compare[] = {0x01, 0x00, 0x56, 0xE3};
        std::memcpy(code + select_renderer, bitmap_compare, sizeof(bitmap_compare));
        return true;
    }
}
