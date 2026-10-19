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

#pragma once

#include <services/notifier/plugin.h>
#include <vector>

namespace eka2l1 {
    class kernel_system;

    namespace epoc::notifier {
        struct global_message_query {
            std::uint32_t softkeys;
            std::u16string text;
            std::u16string header;
        };

        struct query_button {
            std::int16_t command;
            std::u16string text;
        };

        bool parse_global_message_query(const std::uint8_t *data, std::size_t size, global_message_query &query);
        bool parse_query_buttons(const std::vector<std::uint8_t> &resource, std::vector<query_button> &buttons);

        class global_message_query_plugin : public plugin_base {
            struct request_state;
            std::shared_ptr<request_state> state_;

        public:
            explicit global_message_query_plugin(kernel_system *kern);
            epoc::uid unique_id() const override { return 0x100059CB; }
            void handle(epoc::desc8 *request, epoc::des8 *response, epoc::notify_info &complete_info) override;
            void cancel() override;
        };

        class memory_card_dialog_plugin : public plugin_base {
        public:
            explicit memory_card_dialog_plugin(kernel_system *kern)
                : plugin_base(kern) {
            }

            epoc::uid unique_id() const override {
                return 0x101F467A;
            }

            void handle(epoc::desc8 *request, epoc::des8 *response,
                epoc::notify_info &complete_info) override;
            void cancel() override;
        };

        void add_builtin_plugins(kernel_system *kern, std::vector<plugin_instance> &plugins);
    }
};
