#pragma once

#include <services/framework.h>
#include <utils/reqsts.h>

#include <cstdint>
#include <string>
#include <vector>

namespace eka2l1 {
    class cdl_server;

    struct cdl_ref {
        std::u16string name;
        std::uint32_t uid = 0;
        std::uint32_t id = 0;
    };

    class cdl_server_session final : public service::typical_session {
        std::vector<std::uint8_t> temp_buf_;
        epoc::notify_info notifier_;

        void get_refs_size(service::ipc_context *ctx);
        void get_all_refs_size(service::ipc_context *ctx);
        void get_temp_buf(service::ipc_context *ctx);
        void get_plugin_drive(service::ipc_context *ctx);

    public:
        cdl_server_session(service::typical_server *server, kernel::uid session_uid,
            epoc::version client_version);
        void fetch(service::ipc_context *ctx) override;
    };

    /** CDL compatibility service used by the native S60 shell. */
    class cdl_server final : public service::typical_server {
        friend class cdl_server_session;

        std::vector<cdl_ref> refs_;

        void load_refs();

    public:
        explicit cdl_server(system *sys);
        void connect(service::ipc_context &ctx) override;
        const std::vector<cdl_ref> &refs() const { return refs_; }
    };
}
