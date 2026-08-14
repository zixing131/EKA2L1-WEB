/*
 * Copyright (c) 2020 EKA2L1 Team
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

#include <services/notifier/queries.h>
#include <services/ui/plugins/keylocknof.h>
#include <services/ui/plugins/notenof.h>
#include <kernel/kernel.h>
#include <utils/err.h>

namespace eka2l1::epoc::notifier {
    void memory_card_dialog_plugin::handle(epoc::desc8 *, epoc::des8 *response,
        epoc::notify_info &complete_info) {
        // KAknMemoryCardDialogUid is obsolete in this S60 generation, but
        // legacy installer UI code still uses it as a one-byte media-ready
        // query. The browser-mounted E: drive is already accessible, so answer
        // affirmatively and complete the asynchronous request.
        if (response) {
            kernel::process *process = complete_info.requester->owning_process();
            auto *value = reinterpret_cast<std::uint8_t *>(response->get_pointer(process));
            if (value && response->get_max_length(process)) {
                *value = 1;
                response->set_length(process, 1);
            }
        }

        complete_info.complete(epoc::error_none);
    }

    void memory_card_dialog_plugin::cancel() {
    }

    void add_builtin_plugins(kernel_system *kern, std::vector<plugin_instance> &plugins) {
#define ADD_PLUGIN(name) plugins.push_back(std::make_unique<name>(kern))
        ADD_PLUGIN(note_display_plugin);
        ADD_PLUGIN(keylock_plugin);
        ADD_PLUGIN(memory_card_dialog_plugin);
#undef ADD_PLUGIN

        std::sort(plugins.begin(), plugins.end(), [](const plugin_instance &lhs, const plugin_instance &rhs) {
            return lhs->unique_id() < rhs->unique_id();
        });
    }
};
