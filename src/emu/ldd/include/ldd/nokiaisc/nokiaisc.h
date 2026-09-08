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
    constexpr const char *NOKIA_ISC_FACTORY_NAME = "nokiaiscdriver";

    class nokiaisc_channel : public channel {
    public:
        nokiaisc_channel(kernel_system *kern, system *sys, epoc::version ver);

        std::int32_t do_control(kernel::thread *thread, std::uint32_t opcode,
            eka2l1::ptr<void> arg1, eka2l1::ptr<void> arg2) override;
        std::int32_t do_request(epoc::notify_info info, std::uint32_t opcode,
            eka2l1::ptr<void> arg1, eka2l1::ptr<void> arg2, bool is_supervisor) override;
    };

    class nokiaisc_factory : public factory {
    public:
        nokiaisc_factory(kernel_system *kern, system *sys);

        void install() override;
        std::unique_ptr<channel> make_channel(epoc::version ver) override;
    };
}
