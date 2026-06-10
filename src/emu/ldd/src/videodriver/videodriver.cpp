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

#include <ldd/videodriver/videodriver.h>
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/thread.h>
#include <kernel/svc.h>

#include <common/log.h>
#include <utils/err.h>
#include <services/window/window.h>

#include <cstring>
#include <string>

// Debug probe defined in kernel/svc.cpp.
extern bool eka2l1_leave_probe;

namespace eka2l1::ldd {
    static const std::string VIDEO_DRIVER_FACTORY_NAME = "VideoDriver";

    video_driver_factory::video_driver_factory(kernel_system *kern, system *sys)
        : factory(kern, sys) {
    }

    void video_driver_factory::install() {
        obj_name = VIDEO_DRIVER_FACTORY_NAME;
    }

    std::unique_ptr<channel> video_driver_factory::make_channel(epoc::version ver) {
        return std::make_unique<video_driver_channel>(kern, sys_, ver);
    }

    video_driver_channel::video_driver_channel(kernel_system *kern, system *sys, epoc::version ver)
        : channel(kern, sys, ver) {
    }

    std::int32_t video_driver_channel::get_screen_number_of_colors(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
        const eka2l1::ptr<void> arg2) {
        void *num_ptr = arg1.get(r->owning_process());
        if (!num_ptr) {
            return epoc::error_argument;
        }

        return epoc::do_hal_by_data_num(sys_, kernel::hal_data_eka1_screen_num_of_colors, num_ptr);
    }

    std::int32_t video_driver_channel::get_screen_info(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
        const eka2l1::ptr<void> arg2) {
        void *data = arg1.get(r->owning_process());
        if (!data) {
            return epoc::error_argument;
        }

        return epoc::do_hal_by_data_num(sys_, kernel::hal_data_eka1_video_info, data);
    }

    std::int32_t video_driver_channel::do_control(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
        const eka2l1::ptr<void> arg2) {
        switch (n) {
        case video_driver_control_op_get_screen_num_of_colors:
            return get_screen_number_of_colors(r, n, arg1, arg2);

        case video_driver_control_op_get_screen_basic_info:
            return get_screen_info(r, n, arg1, arg2);
        
        default:
            break;
        }
        LOG_TRACE(LDD_MMCIF, "Unimplemented video driver control opcode {}", n);
        return 0;
    }

    std::int32_t video_driver_channel::do_request(epoc::notify_info info, const std::uint32_t n,
        const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2,
        const bool is_supervisor) {
        LOG_TRACE(LDD_MMCIF, "Unimplemented video driver request opcode {}", n);
        return 0;
    }

    display_driver_factory::display_driver_factory(kernel_system *kern, system *sys)
        : factory(kern, sys) {
    }

    void display_driver_factory::install() {
        // Lowercase: the EKA2 ChannelCreate/DeviceLoad execs look factories up by
        // the lowercased device name.
        obj_name = "displaydriver";
    }

    std::unique_ptr<channel> display_driver_factory::make_channel(epoc::version ver) {
        return std::make_unique<display_driver_channel>(kern, sys_, ver);
    }

    display_driver_channel::display_driver_channel(kernel_system *kern, system *sys, epoc::version ver)
        : channel(kern, sys, ver) {
    }

    std::int32_t display_driver_channel::do_control(kernel::thread *r, const std::uint32_t n, const eka2l1::ptr<void> arg1,
        const eka2l1::ptr<void> arg2) {
        if (eka2l1_leave_probe) {
            LOG_WARN(LDD_MMCIF, "[probe] DisplayDriver do_control func={} a1=0x{:X} a2=0x{:X}",
                n, arg1.ptr_address(), arg2.ptr_address());
        }

        switch (n) {
        case display_driver_control_op_get_info: {
            display_driver_info *info = arg1.cast<display_driver_info>().get(r->owning_process());

            if (!info) {
                return epoc::error_argument;
            }

            std::memset(info, 0, sizeof(display_driver_info));

            window_server *winserv = reinterpret_cast<window_server *>(
                kern->get_by_name<service::server>(
                    eka2l1::get_winserv_name_by_epocver(kern->get_epoc_version())));
            epoc::screen *scr = winserv ? winserv->get_screen(screen_number_) : nullptr;

            // The HLE window server composes from a plain memory chunk, so the
            // direct-access layouts scdv may ask for can be claimed. The 16MA
            // flag doubles as the discriminator for driver mode index 1 (the
            // RDisplay wrapper resolves index 1 to 16MA when set, 16MU when
            // clear), so it must mirror the actual screen format.
            info->update_needed_ = 1;
            info->unk10_ = 1;
            info->unk14_ = 1;
            info->color4k_supported_ = 1;
            info->color64k_supported_ = 1;
            info->color16mu_supported_ = 1;
            info->color16ma_supported_ = (scr && (scr->disp_mode == epoc::display_mode::color16ma)) ? 1 : 0;
            return epoc::error_none;
        }

        case display_driver_control_op_get_stride: {
            std::int32_t *stride_out = arg1.cast<std::int32_t>().get(r->owning_process());
            if (!stride_out) {
                return epoc::error_argument;
            }

            window_server *winserv = reinterpret_cast<window_server *>(
                kern->get_by_name<service::server>(
                    eka2l1::get_winserv_name_by_epocver(kern->get_epoc_version())));

            epoc::screen *scr = winserv ? winserv->get_screen(screen_number_) : nullptr;
            if (!scr) {
                return epoc::error_not_found;
            }

            // Must match the layout of the window server's screen buffer chunk:
            // scdv turns this back into pixels-per-line for direct access.
            *stride_out = epoc::get_byte_width(scr->size().x,
                epoc::get_bpp_from_display_mode(scr->disp_mode));
            return epoc::error_none;
        }

        case display_driver_control_op_set_screen:
            screen_number_ = static_cast<std::int32_t>(arg1.ptr_address());
            return epoc::error_none;

        case display_driver_control_op_update_region:
            // The HLE window server already presents the screen chunk each frame,
            // so update notifications need no extra work here.
            return epoc::error_none;

        default:
            break;
        }

        LOG_TRACE(LDD_MMCIF, "Unimplemented display driver control opcode {}", n);
        return 0;
    }

    std::int32_t display_driver_channel::do_request(epoc::notify_info info, const std::uint32_t n,
        const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2,
        const bool is_supervisor) {
        LOG_TRACE(LDD_MMCIF, "Unimplemented display driver request opcode {}", n);
        return 0;
    }
}