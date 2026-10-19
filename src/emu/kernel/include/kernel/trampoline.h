#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eka2l1::hle {
    // Tail-branch from a ROM export to its replacement. Thumb BX PC enters
    // aligned ARM code, whose LDR PC also restores the target's instruction set.
    // No register or stack slot may be borrowed: callers can pass arguments on
    // the stack, and the replacement must return directly to the original LR.
    constexpr std::size_t export_trampoline_arm_offset(std::uint32_t entry) {
        return (entry & 1) ? (4 + (entry & 2)) : 0;
    }

    constexpr std::size_t export_trampoline_size(std::uint32_t entry) {
        return export_trampoline_arm_offset(entry) + 8;
    }

    inline void write_export_trampoline(void *buffer, std::uint32_t entry, std::uint32_t target) {
        auto *bytes = static_cast<std::uint8_t *>(buffer);
        const auto arm_offset = export_trampoline_arm_offset(entry);
        if (entry & 1) {
            const std::uint16_t bx_pc = 0x4778;
            const std::uint16_t nop = 0x46C0;
            const auto bx_offset = entry & 2;
            // BX to ARM requires a word-aligned PC. Put BX itself at a word
            // boundary even for a Thumb export starting in the upper halfword.
            if (bx_offset) {
                std::memcpy(bytes, &nop, sizeof(nop));
            }
            std::memcpy(bytes + bx_offset, &bx_pc, sizeof(bx_pc));
            std::memcpy(bytes + bx_offset + 2, &nop, sizeof(nop));
        }
        const std::uint32_t ldr_pc = 0xE51FF004;
        std::memcpy(bytes + arm_offset, &ldr_pc, sizeof(ldr_pc));
        std::memcpy(bytes + arm_offset + 4, &target, sizeof(target));
    }
}
