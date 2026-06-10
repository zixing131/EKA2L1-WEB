#pragma once

#include <common/types.h>
#include <system/installation/common.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eka2l1 {
    class io_system;
    class device_manager;

    namespace common {
        class wo_std_file_stream;
    }

    namespace loader {
        struct rpkg_header {
            std::uint32_t magic[4];
            std::uint8_t major_rom;
            std::uint8_t minor_rom;
            std::uint16_t build_rom;
            std::uint32_t count;
            std::uint32_t header_size;
            std::uint32_t machine_uid;
        };

        struct rpkg_entry {
            std::uint64_t attrib;
            std::uint64_t time;
            std::uint64_t path_len;

            std::u16string path;

            std::uint64_t data_size;
        };

        bool should_install_requires_additional_rpkg(const std::string &path);

        device_installation_error install_rom(device_manager *dvc, const std::string &path, const std::string &rom_resident_path, const std::string &drives_z_resident_path, progress_changed_callback progress_cb, cancel_requested_callback cancel_cb);
        device_installation_error install_rpkg(device_manager *dvc, const std::string &path, const std::string &devices_rom_path, std::string &firmware_code, progress_changed_callback progress_cb, cancel_requested_callback cancel_cb);

        /**
         * Incremental (push-mode) RPKG installer.
         *
         * install_rpkg() needs the whole package on a filesystem, which on the
         * web frontend means a full extra copy in MEMFS (= JS heap). iOS
         * Safari kills the tab long before desktop memory limits, so the
         * browser feeds the picked File straight through here in small chunks
         * instead: the RPKG container is parsed and extracted as the bytes
         * arrive and the package itself is never resident.
         *
         * The parser mirrors install_rpkg()'s byte-exact behaviour, including
         * its lenient handling of truncated containers (stop extracting,
         * still try to finalize).
         *
         * Usage: construct → feed() until EOF → finish(). abort() (or the
         * destructor before finish) removes the partially extracted temp dir.
         */
        class rpkg_stream_installer {
        public:
            explicit rpkg_stream_installer(device_manager *dvcmngr, const std::string &devices_rom_path);
            ~rpkg_stream_installer();

            rpkg_stream_installer(const rpkg_stream_installer &) = delete;
            rpkg_stream_installer &operator=(const rpkg_stream_installer &) = delete;

            /// Consume the next chunk. Returns false when the container is
            /// unusable (bad magic/header) — feeding more is pointless.
            bool feed(const std::uint8_t *data, std::size_t size);

            /// Run the post-extraction steps (product detection, device
            /// registration). Call once, after the last feed().
            device_installation_error finish(std::string &firmware_code_ret);

            /// Delete the partially extracted temp folder.
            void abort();

        private:
            enum class parse_state {
                header_fixed, ///< first 24 bytes (16B magic block + ver/build/count)
                header_v2_extra, ///< RPK2 only: header_size + machine_uid
                entry_meta, ///< attrib + time + path_len
                entry_path_and_size, ///< UTF-16 path + data_size
                entry_data, ///< file payload, streamed through
                broken ///< bad container, ignore the rest
            };

            void open_entry_output();

            device_manager *dvcmngr_;
            std::string devices_rom_path_;

            parse_state state_;
            std::vector<std::uint8_t> pending_; ///< buffer for fixed-size pieces
            std::size_t need_;

            rpkg_header header_;
            rpkg_entry entry_;
            std::uint64_t data_left_;
            bool discard_rest_; ///< output failure: mirror install_rpkg's "break"
            bool finished_;

            std::unique_ptr<common::wo_std_file_stream> out_;
        };
    }
}
