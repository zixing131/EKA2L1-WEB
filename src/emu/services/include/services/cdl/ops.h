#pragma once

namespace eka2l1::epoc {
    enum cdl_server_cmd {
        cdl_server_cmd_request_get_cust,
        cdl_server_cmd_get_cust,
        cdl_server_cmd_set_uids_to_notify,
        cdl_server_cmd_notify_change,
        cdl_server_cmd_cancel_notify_change,
        cdl_server_cmd_set_cust,
        cdl_server_cmd_get_refs_size,
        cdl_server_cmd_get_name_size,
        cdl_server_cmd_get_temp_buf,
        cdl_server_cmd_get_all_refs_size,
        cdl_server_cmd_is_plugin_in_rom,
        cdl_server_cmd_plugin_drive,
    };
}
