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
#include <common/platform.h>
#if !EKA2L1_PLATFORM(WASM) && !EKA2L1_PLATFORM(OHOS)
#include <services/notifier/bluetooth.h>
#endif
#include <services/ui/plugins/keylocknof.h>
#include <services/ui/plugins/notenof.h>
#include <kernel/kernel.h>
#include <utils/err.h>
#include <common/buffer.h>
#include <common/unicode.h>
#include <common/log.h>
#include <drivers/ui/input_dialog.h>
#include <loader/rsc.h>
#include <utils/bafl.h>
#include <vfs/vfs.h>

namespace eka2l1::epoc::notifier {
    namespace {
        bool read_query_text(common::ro_buf_stream &stream, std::u16string &text) {
            std::uint16_t expected = 0;
            std::uint8_t first = 0;
            if (stream.read(&expected, 2) != 2 || expected > 4096 || stream.read(&first, 1) != 1) {
                return false;
            }
            // A streamed UTF-16 descriptor has a cardinality of length << 1.
            std::uint32_t cardinality = first;
            if (!(first & 1)) {
                cardinality >>= 1;
            } else if (!(first & 2)) {
                std::uint8_t high = 0;
                if (stream.read(&high, 1) != 1) return false;
                cardinality = (cardinality | (high << 8)) >> 2;
            } else if (!(first & 4)) {
                std::uint8_t high[3];
                if (stream.read(high, 3) != 3) return false;
                cardinality = (cardinality | (std::uint32_t(high[0]) << 8)
                    | (std::uint32_t(high[1]) << 16) | (std::uint32_t(high[2]) << 24)) >> 3;
            } else {
                return false;
            }
            if (cardinality != std::uint32_t(expected) * 2) return false;
            text.resize(expected);
            if (!expected) return true;
            const auto offset = stream.tell();
            std::vector<std::uint8_t> encoded(stream.size() - offset);
            stream.read(encoded.data(), encoded.size());
            int consumed = static_cast<int>(encoded.size());
            common::unicode_expander expander;
            if (expander.expand(encoded.data(), consumed, reinterpret_cast<std::uint8_t *>(text.data()),
                    expected * 2) != expected * 2) return false;
            stream.seek(offset + consumed, common::seek_where::beg);
            return true;
        }

        std::vector<query_button> load_query_buttons(kernel_system *kern, std::uint32_t resource_id) {
            // Global queries use the system Avkon resource file. Do not guess
            // command IDs for an unknown application's private resource.
            if ((resource_id & 0xFFFFF000) != 0x08CC0000) return {};
            auto *io = kern->get_io_system();
            const auto path = utils::get_nearest_lang_file(io, u"z:\\resource\\avkon.rsc",
                kern->get_current_language(), drive_z);
            auto file = io->open_file(path, READ_MODE | BIN_MODE);
            if (!file || file->size() > 16 * 1024 * 1024) return {};
            std::vector<std::uint8_t> bytes(file->size());
            if (file->read_file(bytes.data(), 1, bytes.size()) != bytes.size()) return {};
            common::ro_buf_stream stream(bytes.data(), bytes.size());
            loader::rsc_file resources(&stream);
            resources.confirm_signature();
            std::vector<query_button> buttons;
            if (!parse_query_buttons(resources.read(resource_id), buttons)) return {};
            return buttons;
        }
    }

    bool parse_global_message_query(const std::uint8_t *data, std::size_t size, global_message_query &query) {
        if (!data || size < 18 || size > 65536) return false;
        common::ro_buf_stream stream(const_cast<std::uint8_t *>(data), size);
        std::uint32_t signature = 0;
        if (stream.read(&signature, 4) != 4 || signature != 0x00116BBA
            || stream.read(&query.softkeys, 4) != 4) return false;
        stream.seek(6, common::seek_where::cur); // image, mask and tone
        // The remaining image/delay/skin data is presentation only.
        return read_query_text(stream, query.text) && read_query_text(stream, query.header);
    }

    bool parse_query_buttons(const std::vector<std::uint8_t> &resource, std::vector<query_button> &buttons) {
        buttons.clear();
        if (resource.size() < 12) return false;
        common::ro_buf_stream stream(const_cast<std::uint8_t *>(resource.data()), resource.size());
        stream.seek(10, common::seek_where::beg); // LONG flags, WORD breadth, LLINK related_buttons
        std::uint16_t count = 0;
        if (stream.read(&count, 2) != 2 || count < 1 || count > 3) return false;
        for (std::uint16_t i = 0; i < count; i++) {
            std::uint8_t version = 0;
            query_button button{};
            if (stream.read(&version, 1) != 1 || (version != 0 && version != 10)
                || stream.read(&button.command, 2) != 2) return false;
            if (version == 10) { // KAknCbaVersion adds a long-press command
                std::int16_t long_command;
                if (stream.read(&long_command, 2) != 2) return false;
            }
            std::u16string bitmap;
            std::uint32_t bitmap_ids;
            if (!loader::read_resource_string(stream, button.text)
                || !loader::read_resource_string(stream, bitmap)
                || stream.read(&bitmap_ids, 4) != 4) return false;
            // A third CBA entry describes the middle softkey (usually repeats
            // the affirmative command). The host dialog has two softkeys.
            if (i < 2) buttons.push_back(std::move(button));
        }
        return !buttons.empty();
    }

    struct global_message_query_plugin::request_state {
        kernel_system *kern;
        kernel::uid thread = 0;
        eka2l1::ptr<epoc::request_status> status = 0;
        std::uint64_t generation = 0;

        // handle/cancel are called by IPC dispatch with the kernel locked;
        // the host callback acquires that same lock before inspecting state.
        bool pending() const {
            auto *requester = kern->get_by_id<kernel::thread>(thread);
            if (!status || !requester || requester->get_exit_type() != kernel::entity_exit_type::pending)
                return false;
            const auto *request = status.get(requester->owning_process());
            return request && request->status == epoc::request_status::pending_status;
        }

        void finish(int command) {
            auto *requester = kern->get_by_id<kernel::thread>(thread);
            const bool still_pending = this->pending();
            auto pending = status;
            status = 0;
            ++generation;
            if (still_pending) {
                epoc::notify_info info(pending, requester);
                info.complete(command);
            }
        }
    };

    global_message_query_plugin::global_message_query_plugin(kernel_system *kern)
        : plugin_base(kern), state_(std::make_shared<request_state>()) {
        state_->kern = kern;
    }

    void global_message_query_plugin::handle(epoc::desc8 *request, epoc::des8 *, epoc::notify_info &info) {
        if (state_->status) {
            if (state_->pending()) {
                info.complete(epoc::error_in_use);
                return;
            }
            state_->finish(epoc::error_cancel);
        }
        global_message_query query;
        if (!request || !parse_global_message_query(reinterpret_cast<const std::uint8_t *>(
                request->get_pointer_raw(info.requester->owning_process())), request->get_length(), query)) {
            info.complete(epoc::error_argument);
            return;
        }
        auto buttons = load_query_buttons(kern_, query.softkeys);
        if (buttons.empty()) {
            LOG_ERROR(SERVICE_NOTIFIER, "Unsupported global message query softkeys 0x{:X}", query.softkeys);
            info.complete(epoc::error_not_supported);
            return;
        }
        if (buttons.size() == 1) buttons.push_back({ -1, u"Cancel" });
        state_->thread = info.requester->unique_id();
        state_->status = info.sts;
        const auto generation = ++state_->generation;
        std::weak_ptr<request_state> weak = state_;
        drivers::ui::show_yes_no_dialog(query.header + u"\n\n" + query.text, buttons[0].text, buttons[1].text,
            [weak, generation, buttons](int choice) {
                const auto state = weak.lock();
                if (!state) return;
                const std::lock_guard<kernel_system> guard(*state->kern);
                if (generation != state->generation || !state->status) return;
                // Avkon completes TRequestStatus with the command ID; the
                // one-byte response descriptor is deliberately unused.
                state->finish(choice >= 0 && choice < 2 ? buttons[choice].command : -1);
            });
    }

    void global_message_query_plugin::cancel() {
        state_->finish(epoc::error_cancel);
    }

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
        ADD_PLUGIN(global_message_query_plugin);
#if !EKA2L1_PLATFORM(WASM) && !EKA2L1_PLATFORM(OHOS)
        ADD_PLUGIN(bluetooth_device_selection_plugin);
#endif
#undef ADD_PLUGIN

        std::sort(plugins.begin(), plugins.end(), [](const plugin_instance &lhs, const plugin_instance &rhs) {
            return lhs->unique_id() < rhs->unique_id();
        });
    }
};
