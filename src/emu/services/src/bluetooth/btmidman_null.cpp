/*
 * Copyright (c) 2024 EKA2L1 Team
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

// WASM stub: Bluetooth middleman not available in browser sandbox
#include <services/bluetooth/btmidman.h>

namespace eka2l1::epoc::bt {
    midman::midman()
        : local_name_(u"eka2l1")
        , native_handle_(nullptr) {
    }

    class midman_null : public midman {
    public:
        midman_null() : midman() {}
        midman_type type() const override { return MIDMAN_INET_BT; }
    };

    std::unique_ptr<midman> make_bluetooth_midman(const eka2l1::config::state &conf, const std::uint32_t reserved_stack_type) {
        return std::make_unique<midman_null>();
    }
}
