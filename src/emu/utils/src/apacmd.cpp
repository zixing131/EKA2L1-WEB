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

#include <common/chunkyseri.h>
#include <utils/apacmd.h>
#include <utils/des.h>

#include <vector>

namespace eka2l1::epoc::apa {
    static void append_cardinality(std::vector<std::uint8_t> &out, const std::uint32_t val) {
        // Match ROM TCardinality::ExternalizeL (euser 0x8054ece2): packed
        // encoding, little-endian. Do not go through chunkyseri — a 4-byte
        // absorb of uint16_t is fine, but writing via size() on a stack
        // buffer is easy to get wrong and guest InternalizeL is unforgiving
        // (KErrEof / -25).
        if (val <= 0x7F) {
            out.push_back(static_cast<std::uint8_t>(val << 1));
            return;
        }
        if (val <= 0x3FFF) {
            const std::uint16_t packed = static_cast<std::uint16_t>((val << 2) + 1);
            out.push_back(static_cast<std::uint8_t>(packed));
            out.push_back(static_cast<std::uint8_t>(packed >> 8));
            return;
        }
        const std::uint32_t packed = (val << 3) + 3;
        out.push_back(static_cast<std::uint8_t>(packed));
        out.push_back(static_cast<std::uint8_t>(packed >> 8));
        out.push_back(static_cast<std::uint8_t>(packed >> 16));
        out.push_back(static_cast<std::uint8_t>(packed >> 24));
    }

    static void append_des16_uncompressed(std::vector<std::uint8_t> &out, const std::u16string &str) {
        // Odd TCardinality => guest HBufC::NewL reads one byte per character
        // and zero-extends to UTF-16. Raw UTF-16 would leave unread bytes and
        // make the next descriptor hit KErrEof (-25).
        append_cardinality(out, static_cast<std::uint32_t>(str.size() * 2 + 1));
        for (const char16_t ch : str) {
            out.push_back(static_cast<std::uint8_t>(ch));
        }
    }

    static void append_des8_uncompressed(std::vector<std::uint8_t> &out, const std::string &str) {
        append_cardinality(out, static_cast<std::uint32_t>(str.size() * 2 + 1));
        out.insert(out.end(), str.begin(), str.end());
    }

    static void append_i32(std::vector<std::uint8_t> &out, const std::int32_t value) {
        const auto u = static_cast<std::uint32_t>(value);
        out.push_back(static_cast<std::uint8_t>(u));
        out.push_back(static_cast<std::uint8_t>(u >> 8));
        out.push_back(static_cast<std::uint8_t>(u >> 16));
        out.push_back(static_cast<std::uint8_t>(u >> 24));
    }

    command_line::command_line()
        : launch_cmd_(command_run)
        , server_differentiator_(0)
        , default_screen_number_(-1)
        , parent_window_group_id_(0)
        , debug_mem_fail_(0)
        , app_startup_instrumentation_event_id_base_(0)
        , parent_process_id_(0) {
    }

    void command_line::do_it_newarch(common::chunkyseri &seri) {
        epoc::absorb_des_string(document_name_, seri, true);
        epoc::absorb_des_string(executable_path_, seri, true);
        epoc::absorb_des_string(opaque_data_, seri, false);
        epoc::absorb_des_string(tail_end_, seri, false);

        seri.absorb(launch_cmd_);
        seri.absorb(server_differentiator_);
        seri.absorb(default_screen_number_);
        seri.absorb(parent_window_group_id_);
        seri.absorb(debug_mem_fail_);
        seri.absorb(app_startup_instrumentation_event_id_base_);
        seri.absorb(parent_process_id_);
    }

    static char16_t get_char_correspond_to_command(const command cmd) {
        switch (cmd) {
        case command_open:
            return 'O';

        case command_create:
            return 'C';

        case command_run:
            return 'R';

        case command_background:
            return 'B';

        case command_view_activate:
            return 'V';

        case command_run_without_views:
            return 'W';

        default:
            break;
        }

        return 'R';
    }

    std::u16string command_line::to_string(const bool oldarch) {
        if (oldarch) {
            // From RE. The order: .app path -> command (O, C, R, B, V, W) -> document name -> tail end
            // .app path and document name are in quote (not neccessary if they don't have spaces in their string).
            // Each components are separated by space.
            std::u16string result;

            // Add executable path
            result += '"';
            result += executable_path_;
            result += u"\" ";

            // Add command. Components are space-separated: "exe" O "document"
            result += get_char_correspond_to_command(launch_cmd_);
            result += ' ';

            // Add document name
            result += '"';
            result += document_name_;
            result += u"\" ";

            // Add tail end
            result.append(reinterpret_cast<char16_t *>(tail_end_.data()), (tail_end_.length() + 1) >> 1);

            // Other parameters are unused.
            return result;
        }

        common::chunkyseri seri(nullptr, 0, common::chunkyseri_mode::SERI_MODE_MEASURE);
        do_it_newarch(seri);

        std::u16string data;
        data.resize((seri.size() + 1) >> 1);

        seri = common::chunkyseri(reinterpret_cast<std::uint8_t *>(data.data()), data.length() * 2,
            common::chunkyseri_mode::SERI_MODE_WRITE);

        do_it_newarch(seri);

        return data;
    }

    std::vector<std::uint8_t> command_line::to_guest_env_slot() const {
        std::vector<std::uint8_t> out;
        out.reserve(64 + (document_name_.size() + executable_path_.size()) * 2
            + opaque_data_.size() + tail_end_.size());

        append_des16_uncompressed(out, document_name_);
        append_des16_uncompressed(out, executable_path_);
        append_des8_uncompressed(out, opaque_data_);
        append_des8_uncompressed(out, tail_end_);

        append_i32(out, static_cast<std::int32_t>(launch_cmd_));
        append_i32(out, static_cast<std::int32_t>(server_differentiator_));
        append_i32(out, default_screen_number_);
        append_i32(out, static_cast<std::int32_t>(parent_window_group_id_));
        append_i32(out, debug_mem_fail_);
        append_i32(out, static_cast<std::int32_t>(app_startup_instrumentation_event_id_base_));
        append_i32(out, parent_process_id_);
        return out;
    }
}