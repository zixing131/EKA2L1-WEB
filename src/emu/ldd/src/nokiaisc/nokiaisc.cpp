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

#include <ldd/nokiaisc/nokiaisc.h>

#include <common/log.h>
#include <kernel/thread.h>
#include <utils/err.h>

namespace eka2l1::ldd {
    nokiaisc_factory::nokiaisc_factory(kernel_system *kern, system *sys)
        : factory(kern, sys) {
    }

    void nokiaisc_factory::install() {
        obj_name = NOKIA_ISC_FACTORY_NAME;
    }

    std::unique_ptr<channel> nokiaisc_factory::make_channel(epoc::version ver) {
        return std::make_unique<nokiaisc_channel>(kern, sys_, ver);
    }

    nokiaisc_channel::nokiaisc_channel(kernel_system *kern, system *sys, epoc::version ver)
        : channel(kern, sys, ver) {
    }

    std::int32_t nokiaisc_channel::do_control(kernel::thread *, const std::uint32_t opcode,
        const eka2l1::ptr<void>, const eka2l1::ptr<void>) {
        // ISC is Nokia's secure-element interface. The phone shell only needs
        // the device to initialise during boot; no emulated application can
        // access a physical secure element, so report a present, inert device.
        LOG_TRACE(LDD_MMCIF, "NokiaIscDriver control opcode {}", opcode);
        return epoc::error_none;
    }

    std::int32_t nokiaisc_channel::do_request(epoc::notify_info info, const std::uint32_t opcode,
        const eka2l1::ptr<void>, const eka2l1::ptr<void>, const bool) {
        LOG_TRACE(LDD_MMCIF, "NokiaIscDriver request opcode {}", opcode);
        info.complete(epoc::error_none);
        return epoc::error_none;
    }
}
