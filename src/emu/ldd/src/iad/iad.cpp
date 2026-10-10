/*
 * Copyright (c) 2026 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <ldd/iad/iad.h>

#include <common/log.h>
#include <kernel/process.h>
#include <kernel/thread.h>
#include <utils/des.h>
#include <utils/err.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace eka2l1::ldd {
    namespace {
        host_location g_fix;
        int g_log_left = 48;

        int nmea_checksum(const std::string &body) {
            unsigned char sum = 0;
            for (unsigned char c : body) {
                sum ^= c;
            }
            return sum;
        }

        std::string nmea_gga(const host_location &fix) {
            const double lat = std::fabs(fix.latitude);
            const int lat_deg = static_cast<int>(lat);
            const double lat_min = (lat - lat_deg) * 60.0;
            const double lon = std::fabs(fix.longitude);
            const int lon_deg = static_cast<int>(lon);
            const double lon_min = (lon - lon_deg) * 60.0;

            char body[160];
            std::snprintf(body, sizeof(body),
                "GPGGA,120000.00,%02d%07.4f,%c,%03d%07.4f,%c,1,08,0.9,%.1f,M,0.0,M,,",
                lat_deg, lat_min, fix.latitude >= 0 ? 'N' : 'S',
                lon_deg, lon_min, fix.longitude >= 0 ? 'E' : 'W',
                fix.altitude_m);
            char sentence[180];
            std::snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", body, nmea_checksum(body));
            return sentence;
        }

        void log_arg(kernel::process *pr, const char *which, eka2l1::ptr<void> arg) {
            if (!arg || g_log_left <= 0 || !pr) {
                return;
            }
            const std::uint32_t addr = arg.ptr_address();
            if (addr < 0x1000) {
                LOG_WARN(KERNEL, "IAD {} imm {}", which, addr);
                return;
            }
            epoc::desc_base *des = arg.cast<epoc::desc_base>().get(pr);
            if (des && des->is_valid_descriptor()) {
                const std::uint32_t max_len = des->get_max_length(pr);
                if (max_len > 0 && max_len <= 65536) {
                    LOG_WARN(KERNEL, "IAD {} des type {} len {} max {}", which,
                        static_cast<int>(des->get_descriptor_type()), des->get_length(), max_len);
                    return;
                }
            }
            auto *words = arg.cast<std::uint32_t>().get(pr);
            if (!words) {
                LOG_WARN(KERNEL, "IAD {} ptr {:#x} unmapped", which, addr);
                return;
            }
            LOG_WARN(KERNEL, "IAD {} ptr {:#x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                which, addr, words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        }

        bool write_nmea(kernel::process *pr, eka2l1::ptr<void> arg) {
            if (!g_fix.valid || !arg || arg.ptr_address() < 0x1000 || !pr) {
                return false;
            }
            epoc::des8 *des = arg.cast<epoc::des8>().get(pr);
            if (!des || !des->is_valid_descriptor()) {
                return false;
            }
            const epoc::des_type kind = des->get_descriptor_type();
            if (kind != epoc::ptr && kind != epoc::buf) {
                return false;
            }
            const std::uint32_t max_len = des->get_max_length(pr);
            if (max_len < 64 || max_len > 4096 || des->get_length() != 0) {
                return false;
            }
            if (!des->get_pointer(pr)) {
                return false;
            }
            const std::string sentence = nmea_gga(g_fix);
            if (sentence.size() > max_len) {
                return false;
            }
            return des->assign(pr, sentence) == 0;
        }

        void inspect_words(kernel::thread *thread, kernel::process *pr, eka2l1::ptr<void> arg, const char *which) {
            (void)thread;
            if (!arg || arg.ptr_address() < 0x1000 || !pr) {
                return;
            }
            auto *words = arg.cast<std::uint32_t>().get(pr);
            if (!words) {
                return;
            }
            for (int i = 0; i < 8; ++i) {
                const std::uint32_t child_addr = words[i];
                if (child_addr < 0x1000) {
                    continue;
                }
                eka2l1::ptr<void> child(child_addr);
                if (write_nmea(pr, child)) {
                    LOG_WARN(KERNEL, "IAD wrote nmea into {}[{}] {:#x}", which, i, child_addr);
                }
                auto *nested = child.cast<std::uint32_t>().get(pr);
                if (!nested) {
                    continue;
                }
                if (nested[0] == 0x80000001) {
                    LOG_WARN(KERNEL, "IAD pending status at {:#x} buf {:#x}", child_addr, nested[2]);
                    auto *buf = eka2l1::ptr<std::uint8_t>(nested[2]).get(pr);
                    if (buf) {
                        LOG_WARN(KERNEL, "IAD buf {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15]);
                    }
                }
                if (g_log_left > 0) {
                    --g_log_left;
                    LOG_WARN(KERNEL, "IAD {}[{}] {:#x} {:08x} {:08x} {:08x} {:08x}",
                        which, i, child_addr, nested[0], nested[1], nested[2], nested[3]);
                }
            }
        }
    }

    host_location &host_location_fix() {
        return g_fix;
    }

    void set_host_location(double latitude, double longitude, double accuracy_m,
        double altitude_m, double speed_mps, double heading_deg) {
        if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
            return;
        }
        const bool first = !g_fix.valid;
        const bool moved = std::fabs(g_fix.latitude - latitude) > 0.0001
            || std::fabs(g_fix.longitude - longitude) > 0.0001;
        g_fix.valid = true;
        g_fix.latitude = latitude;
        g_fix.longitude = longitude;
        g_fix.accuracy_m = accuracy_m;
        g_fix.altitude_m = altitude_m;
        g_fix.speed_mps = speed_mps;
        g_fix.heading_deg = heading_deg;
        if (first || moved) {
            LOG_WARN(KERNEL, "IAD host gps {:.6f},{:.6f} acc {:.0f}m", latitude, longitude, accuracy_m);
        }
    }

    iad_factory::iad_factory(kernel_system *kern, system *sys)
        : factory(kern, sys) {
    }

    void iad_factory::install() {
        obj_name = IAD_DRIVER_FACTORY_NAME;
    }

    std::unique_ptr<channel> iad_factory::make_channel(epoc::version ver) {
        return std::make_unique<iad_channel>(kern, sys_, ver);
    }

    iad_channel::iad_channel(kernel_system *kern, system *sys, epoc::version ver)
        : channel(kern, sys, ver) {
    }

    std::int32_t iad_channel::do_control(kernel::thread *thread, const std::uint32_t opcode,
        const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2) {
        if (g_log_left > 0) {
            --g_log_left;
            LOG_WARN(KERNEL, "IAD control opcode {}", opcode);
            kernel::process *pr = thread ? thread->owning_process() : nullptr;
            if (pr) {
                log_arg(pr, "control.a1", arg1);
                log_arg(pr, "control.a2", arg2);
                inspect_words(thread, pr, arg1, "control.a1");
                inspect_words(thread, pr, arg2, "control.a2");
            }
        }
        return epoc::error_none;
    }

    std::int32_t iad_channel::do_request(epoc::notify_info info, const std::uint32_t opcode,
        const eka2l1::ptr<void> arg1, const eka2l1::ptr<void> arg2, const bool) {
        kernel::process *pr = info.requester ? info.requester->owning_process() : nullptr;
        if (g_log_left > 0) {
            --g_log_left;
            LOG_WARN(KERNEL, "IAD request opcode {}", opcode);
            if (pr) {
                log_arg(pr, "request.a1", arg1);
                log_arg(pr, "request.a2", arg2);
            }
        }
        if (pr && !write_nmea(pr, arg1)) {
            write_nmea(pr, arg2);
        }
        info.complete(epoc::error_none);
        return epoc::error_none;
    }
}

#ifdef __EMSCRIPTEN__
extern "C" {
    // Coordinates are scaled integers so the JS ccall number ABI stays exact.
    // lat/lon: degrees * 1e7. accuracy: millimetres. altitude: centimetres.
    // speed: millimetres per second. heading: millidegrees.
    EMSCRIPTEN_KEEPALIVE
    void wasm_set_gps(int lat_e7, int lon_e7, int accuracy_mm, int altitude_cm,
        int speed_mms, int heading_mdeg) {
        eka2l1::ldd::set_host_location(
            static_cast<double>(lat_e7) / 1e7,
            static_cast<double>(lon_e7) / 1e7,
            static_cast<double>(accuracy_mm) / 1000.0,
            static_cast<double>(altitude_cm) / 100.0,
            static_cast<double>(speed_mms) / 1000.0,
            static_cast<double>(heading_mdeg) / 1000.0);
    }
}
#endif
