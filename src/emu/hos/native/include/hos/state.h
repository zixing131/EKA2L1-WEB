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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <common/log.h>
#include <common/queue.h>
#include <common/sync.h>
#include <config/app_settings.h>
#include <config/config.h>
#include <package/manager.h>
#include <system/epoc.h>

#include <hos/emu_window_ohos.h>
#include <hos/launcher.h>
#include <drivers/graphics/emu_window.h>
#include <drivers/input/emu_controller.h>
#include <services/window/window.h>
#include <drivers/sensor/sensor.h>

namespace eka2l1 {
    namespace drivers {
        class graphics_driver;
        class audio_driver;
    }

    namespace hos {
        class launcher;
    }
}

namespace eka2l1::hos {
    /**
     * \brief State of the emulator on HarmonyOS.
     *
     * Mirrors eka2l1::android::emulator. The OS, graphics and UI run on their own
     * threads; the ArkUI side only talks to this through the NAPI bridge.
     */
    struct emulator {
        std::unique_ptr<system> symsys;
        std::unique_ptr<drivers::graphics_driver> graphics_driver;
        std::unique_ptr<drivers::audio_driver> audio_driver;
        std::unique_ptr<drivers::sensor_driver> sensor_driver;
        std::unique_ptr<launcher> launcher;

        std::shared_ptr<base_logger> logger;
        std::unique_ptr<config::app_settings> app_settings;
        std::unique_ptr<drivers::emu_window_ohos> window;
        drivers::emu_controller_ptr joystick_controller;

        std::atomic<bool> should_emu_quit;
        std::atomic<bool> should_emu_pause;
        std::atomic<bool> should_graphics_pause;
        std::atomic<bool> surface_inited;
        std::atomic<bool> should_ui_quit;
        std::atomic<bool> stage_two_inited;
        std::vector<std::size_t> screen_change_handles;
        std::size_t system_reset_cbh;

        bool first_time;

        common::semaphore graphics_sema;
        common::semaphore pause_sema;
        common::semaphore pause_graphics_sema;
        common::event graphics_init_done;

        config::state conf;
        window_server *winserv;
        int present_status;

        // Real present FPS measurement (updated from the draw callback, read via NAPI).
        std::atomic<float> present_fps{ 0.0f };
        std::uint64_t fps_window_start_us{ 0 };
        std::uint32_t fps_frame_count{ 0 };

        std::mutex input_mutex;

        explicit emulator();

        void stage_one();
        bool stage_two();

        // First-boot bring-up for a device installed while the emulator was
        // already running with no device. Mirrors the device-setup block of
        // stage_one() (startup + set_device + mount C/D/E). Returns false when
        // there is still no installed device. After this succeeds the caller
        // must run stage_two() and start the OS thread.
        bool bring_up_after_install();

        void on_system_reset(system *the_sys);
        void register_draw_callback();
    };
}
