#include <common/algorithm.h>
#include <common/buffer.h>
#include <common/path.h>
#include <loader/rsc.h>
#include <services/cdl/cdl.h>
#include <services/cdl/ops.h>
#include <system/epoc.h>
#include <utils/err.h>
#include <vfs/vfs.h>

#include <algorithm>
#include <cstring>

namespace eka2l1 {
    namespace {
        static void serialize_refs(const std::vector<cdl_ref> &refs, std::vector<std::uint8_t> &out) {
            std::vector<std::u16string> names;
            for (const cdl_ref &ref : refs) {
                if (std::find(names.begin(), names.end(), ref.name) == names.end()) {
                    names.push_back(ref.name);
                }
            }
            std::sort(names.begin(), names.end());

            std::size_t size = 4;
            for (const auto &name : names) size += 4 + name.size() * 2;
            size += 4 + refs.size() * 12;
            out.assign(size, 0);
            std::uint8_t *p = out.data();
            auto put32 = [&p](std::uint32_t value) {
                std::memcpy(p, &value, sizeof(value));
                p += sizeof(value);
            };
            put32(static_cast<std::uint32_t>(names.size()));
            for (const auto &name : names) {
                put32(static_cast<std::uint32_t>(name.size() * 2));
                if (!name.empty()) {
                    std::memcpy(p, name.data(), name.size() * 2);
                    p += name.size() * 2;
                }
            }
            put32(static_cast<std::uint32_t>(refs.size()));
            for (const cdl_ref &ref : refs) {
                put32(ref.id);
                put32(ref.uid);
                const auto it = std::lower_bound(names.begin(), names.end(), ref.name);
                put32(static_cast<std::uint32_t>(std::distance(names.begin(), it)));
            }
        }
    }

    cdl_server_session::cdl_server_session(service::typical_server *server, kernel::uid session_uid,
        epoc::version client_version)
        : service::typical_session(server, session_uid, client_version) {
    }

    void cdl_server_session::get_refs_size(service::ipc_context *ctx) {
        std::vector<cdl_ref> filtered;
        const auto by_name = ctx->get_argument_value<std::int32_t>(1);
        if (!by_name) {
            ctx->complete(epoc::error_argument);
            return;
        }

        if (*by_name) {
            const auto name = ctx->get_argument_value<std::u16string>(2);
            if (!name) {
                ctx->complete(epoc::error_argument);
                return;
            }
            for (const cdl_ref &ref : server<cdl_server>()->refs_) {
                if (common::compare_ignore_case(ref.name, *name) == 0) filtered.push_back(ref);
            }
        } else {
            const auto uid = ctx->get_argument_value<std::uint32_t>(3);
            if (!uid) {
                ctx->complete(epoc::error_argument);
                return;
            }
            for (const cdl_ref &ref : server<cdl_server>()->refs_) {
                if (ref.uid == *uid) filtered.push_back(ref);
            }
        }

        serialize_refs(filtered, temp_buf_);
        const std::uint32_t size = static_cast<std::uint32_t>(temp_buf_.size());
        if (!ctx->write_data_to_descriptor_argument(0, size)) {
            ctx->complete(epoc::error_argument);
            return;
        }
        ctx->complete(epoc::error_none);
    }

    void cdl_server_session::get_all_refs_size(service::ipc_context *ctx) {
        serialize_refs(server<cdl_server>()->refs_, temp_buf_);
        const std::uint32_t size = static_cast<std::uint32_t>(temp_buf_.size());
        if (!ctx->write_data_to_descriptor_argument(0, size)) {
            ctx->complete(epoc::error_argument);
            return;
        }
        ctx->complete(epoc::error_none);
    }

    void cdl_server_session::get_temp_buf(service::ipc_context *ctx) {
        if (!ctx->write_data_to_descriptor_argument(0, temp_buf_.data(),
                static_cast<std::uint32_t>(temp_buf_.size()))) {
            ctx->complete(epoc::error_argument);
            return;
        }
        ctx->complete(epoc::error_none);
    }

    void cdl_server_session::get_plugin_drive(service::ipc_context *ctx) {
        const auto name = ctx->get_argument_value<std::u16string>(0);
        if (!name) {
            ctx->complete(epoc::error_argument);
            return;
        }
        const std::u16string lower = common::lowercase_ucs2_string(*name);
        for (const cdl_ref &ref : server<cdl_server>()->refs_) {
            if (common::lowercase_ucs2_string(ref.name) == lower
                || common::lowercase_ucs2_string(ref.name + u".dll") == lower) {
                ctx->complete(char16_to_drive(ref.name[0]));
                return;
            }
        }
        ctx->complete(epoc::error_not_found);
    }

    void cdl_server_session::fetch(service::ipc_context *ctx) {
        switch (ctx->msg->function) {
        case epoc::cdl_server_cmd_request_get_cust:
        case epoc::cdl_server_cmd_get_cust:
        case epoc::cdl_server_cmd_set_uids_to_notify:
        case epoc::cdl_server_cmd_set_cust:
        case epoc::cdl_server_cmd_get_name_size:
        case epoc::cdl_server_cmd_is_plugin_in_rom:
            // These legacy customization calls are optional on S60 ROMs. A
            // successful empty response keeps cdlengine from panicking while
            // preserving the normal no-customization state.
            ctx->complete(epoc::error_none);
            break;
        case epoc::cdl_server_cmd_get_refs_size: get_refs_size(ctx); break;
        case epoc::cdl_server_cmd_get_all_refs_size: get_all_refs_size(ctx); break;
        case epoc::cdl_server_cmd_get_temp_buf: get_temp_buf(ctx); break;
        case epoc::cdl_server_cmd_plugin_drive: get_plugin_drive(ctx); break;
        case epoc::cdl_server_cmd_notify_change:
            notifier_.requester = ctx->msg->own_thr;
            notifier_.sts = ctx->msg->request_sts;
            break;
        case epoc::cdl_server_cmd_cancel_notify_change:
            ctx->complete(epoc::error_none);
            break;
        default:
            ctx->complete(epoc::error_not_supported);
            break;
        }
    }

    cdl_server::cdl_server(system *sys)
        : service::typical_server(sys, "CdlServer") {
        load_refs();
    }

    void cdl_server::load_refs() {
        io_system *io = sys->get_io_system();
        if (!io) return;
        auto dir = io->open_dir(u"z:\\resource\\cdl\\*", {}, io_attrib_include_file);
        if (!dir) return;
        while (auto entry = dir->get_next_entry()) {
            const std::u16string name = common::utf8_to_ucs2(entry->name);
            if (name.size() < 17 || common::lowercase_ucs2_string(name).find(u"_cdl_detail.rsc") == std::u16string::npos) continue;
            auto file = io->open_file(u"z:\\resource\\cdl\\" + name, READ_MODE | BIN_MODE);
            if (!file) continue;
            eka2l1::ro_file_stream file_stream(file.get());
            loader::rsc_file rsc(&file_stream);
            std::vector<std::uint8_t> data = rsc.read(1);
            common::ro_buf_stream data_stream(data.data(), data.size());
            std::uint16_t count = 0;
            if (data_stream.read(&count, sizeof(count)) != sizeof(count)) continue;
            static constexpr std::u16string_view suffix = u"_cdl_detail.rsc";
            const std::u16string uid_name = u"z:\\" + name.substr(0, name.size() - suffix.size()) + u".dll";
            for (std::uint16_t i = 0; i < count; ++i) {
                cdl_ref ref;
                ref.name = uid_name;
                if (data_stream.read(&ref.uid, sizeof(ref.uid)) != sizeof(ref.uid)
                    || data_stream.read(&ref.id, sizeof(ref.id)) != sizeof(ref.id)) {
                    break;
                }
                refs_.push_back(std::move(ref));
            }
        }
    }

    void cdl_server::connect(service::ipc_context &ctx) {
        create_session<cdl_server_session>(&ctx);
        ctx.complete(epoc::error_none);
    }
}
