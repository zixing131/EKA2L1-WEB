/*
 * Copyright (c) 2020 EKA2L1 Team.
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

#include <kernel/ldd.h>

namespace eka2l1::ldd {
    enum video_driver_control_op {
        video_driver_control_op_get_screen_display_constrast = 3,
        video_driver_control_op_get_screen_display_constrast_max = 43,
        video_driver_control_op_get_screen_display_state = 13,
        video_driver_control_op_get_screen_num_of_colors = 14,
        video_driver_control_op_get_screen_basic_info = 15,
        video_driver_control_op_get_screen_extra_info = 16      // Same as 15, just fill other fields
    };

    class video_driver_channel : public channel {
    private:
        std::int32_t get_screen_number_of_colors(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
            const eka2l1::ptr<void> arg2);
        std::int32_t get_screen_info(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
            const eka2l1::ptr<void> arg2);
    public:
        explicit video_driver_channel(kernel_system *kern, system *sys, epoc::version ver);
        ~video_driver_channel() override {}

        std::int32_t do_control(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
            const eka2l1::ptr<void> arg2) override;

        std::int32_t do_request(epoc::notify_info info, const std::uint32_t n,
            const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2,
            const bool is_supervisor) override;
    };

    /**
     * @brief MMC interface channel factory.
     */
    class video_driver_factory : public factory {
    public:
        explicit video_driver_factory(kernel_system *kern, system *sys);

        ~video_driver_factory() override {
        }

        void install() override;
        std::unique_ptr<channel> make_channel(epoc::version ver) override;
    };

    // Nokia S60 3rd-edition "DisplayDriver" LDD (d_display.ldd). The user side is the
    // RDisplay wrapper used by scdv's CFbsDrawDevice screen devices (the direct-screen-
    // access path). Function numbers and the info-struct layout were recovered from the
    // N5320 ROM disassembly of that wrapper and its scdv callers.
    enum display_driver_control_op {
        display_driver_control_op_update_region = 9,        // packed TRect halfwords (x, y, w, h)
        display_driver_control_op_set_screen = 10,          // a1 = screen number
        display_driver_control_op_get_info = 11,            // a1 = 0x64-byte info struct out
        display_driver_control_op_get_stride = 31,          // a1 = TInt out, scanline length in bytes
    };

    // 0x64-byte info block returned by display_driver_control_op_get_info. Only the
    // fields the ROM's scdv/RDisplay actually read are decoded; the rest stay zero.
    struct display_driver_info {
        std::uint32_t update_needed_;        // +0x00 gates RDisplay::UpdateRegion calls
        std::uint32_t unk04_;
        std::uint32_t flipped_;              // +0x08 scdv stores the negation
        std::uint32_t unk0c_;
        std::uint32_t unk10_;                // caller seeds 1
        std::uint32_t unk14_;                // caller seeds 1
        std::uint32_t unk18_[6];
        std::uint32_t color4k_supported_;    // +0x30 EColor4K
        std::uint32_t color64k_supported_;   // +0x34 EColor64K
        std::uint32_t color16m_supported_;   // +0x38 EColor16M
        std::uint32_t color16mu_supported_;  // +0x3C EColor16MU
        std::uint32_t color16ma_supported_;  // +0x40 EColor16MA
        std::uint32_t unk44_[8];
    };

    static_assert(sizeof(display_driver_info) == 0x64);

    class display_driver_channel : public channel {
    private:
        std::int32_t screen_number_{ 0 };

    public:
        explicit display_driver_channel(kernel_system *kern, system *sys, epoc::version ver);
        ~display_driver_channel() override {}

        std::int32_t do_control(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
            const eka2l1::ptr<void> arg2) override;

        std::int32_t do_request(epoc::notify_info info, const std::uint32_t n,
            const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2,
            const bool is_supervisor) override;
    };

    class display_driver_factory : public factory {
    public:
        explicit display_driver_factory(kernel_system *kern, system *sys);

        ~display_driver_factory() override {
        }

        void install() override;
        std::unique_ptr<channel> make_channel(epoc::version ver) override;
    };
}