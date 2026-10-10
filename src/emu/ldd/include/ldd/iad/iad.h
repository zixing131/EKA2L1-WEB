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

#pragma once

#include <kernel/ldd.h>

namespace eka2l1::ldd {
    // Nokia ISA access (isaaccessldd.ldd) opens this logical device as
    // "IADDriver". The kernel lowercases that name before the factory lookup.
    constexpr const char *IAD_DRIVER_FACTORY_NAME = "iaddriver";

    struct host_location {
        bool valid = false;
        double latitude = 0;
        double longitude = 0;
        double accuracy_m = 0;
        double altitude_m = 0;
        double speed_mps = 0;
        double heading_deg = 0;
    };

    host_location &host_location_fix();
    void set_host_location(double latitude, double longitude, double accuracy_m,
        double altitude_m, double speed_mps, double heading_deg);

    class iad_channel : public channel {
    public:
        explicit iad_channel(kernel_system *kern, system *sys, epoc::version ver);

        std::int32_t do_control(kernel::thread *thread, std::uint32_t opcode,
            eka2l1::ptr<void> arg1, eka2l1::ptr<void> arg2) override;
        std::int32_t do_request(epoc::notify_info info, std::uint32_t opcode,
            eka2l1::ptr<void> arg1, eka2l1::ptr<void> arg2, bool is_supervisor) override;
    };

    class iad_factory : public factory {
    public:
        explicit iad_factory(kernel_system *kern, system *sys);

        void install() override;
        std::unique_ptr<channel> make_channel(epoc::version ver) override;
    };
}
