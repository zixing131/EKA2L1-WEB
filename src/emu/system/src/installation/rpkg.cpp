/*
 * Copyright (c) 2018 EKA2L1 Team.
 * 
 * This file is part of EKA2L1 project 
 * (see bentokun.github.com/EKA2L1).
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

#include <system/devices.h>
#include <system/installation/rpkg.h>
#include <system/software.h>

#include <loader/rom.h>

#include <vfs/vfs.h>

#include <common/algorithm.h>
#include <common/cvt.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <common/path.h>
#include <common/platform.h>
#include <common/pystr.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <vector>

namespace eka2l1::loader {
    static device_installation_error finalize_extracted_rpkg(device_manager *dvcmngr, const std::string &devices_rom_path,
        const std::uint32_t machine_uid, std::string &firmware_code_ret);

    bool should_install_requires_additional_rpkg(const std::string &path) {
        common::ro_std_file_stream rom_file_stream(path, true);

        if (!rom_file_stream.valid()) {
            return false;
        }

        auto rom_parse = load_rom(reinterpret_cast<common::ro_stream *>(&rom_file_stream));
        if (!rom_parse.has_value()) {
            return false;
        }

        if (rom_parse->header.rom_base != loader::EKA1_ROM_BASE) {
            return true;
        }

        // Device information usually resides in ROFS. If it's in ROM likely there's no ROFS
        std::optional<rom_entry> rentry = rom_parse->burn_tree_find_entry("z:\\system\\versions\\sw.txt");
        if (rentry.has_value()) {
            return false;
        }

        return true;
    }

    static bool extract_file(const std::string &devices_rom_path, FILE *parent, rpkg_entry &ent, const std::size_t total, progress_changed_callback progress_cb, cancel_requested_callback cancel_cb) {
        std::string file_full_relative = common::ucs2_to_utf8(ent.path.substr(3));
        std::transform(file_full_relative.begin(), file_full_relative.end(), file_full_relative.begin(),
            ::tolower);

        std::string real_path = add_path(add_path(devices_rom_path, "/temp/"), file_full_relative);

        std::string dir = eka2l1::file_directory(real_path);
        common::create_directories(dir);

        common::wo_std_file_stream wf(real_path, true);

        if (!wf.valid()) {
            LOG_INFO(SYSTEM, "Skipping with real path: {}, dir: {}", real_path, dir);
            return false;
        }

        int64_t left = ent.data_size;
        int64_t take_def = 0x10000;

        std::array<char, 0x10000> temp;
        bool failed = false;

        while (left) {
            if (progress_cb) {
                progress_cb(ftell(parent), total);
            }

            if (cancel_cb && cancel_cb()) {
                failed = true;
                break;
            }

            int64_t take = left < take_def ? left : take_def;

            if (fread(temp.data(), 1, take, parent) != take) {
                failed = true;
                break;
            }

            if (wf.write(temp.data(), take) != take) {
                failed = true;
                break;
            }

            left -= take;
        }

        return !failed;
    }

    device_installation_error install_rom(device_manager *dvcmngr, const std::string &path, const std::string &rom_resident_path, const std::string &drives_z_resident_path, progress_changed_callback progress_cb, cancel_requested_callback cancel_cb) {
        const std::string temp_z_path = eka2l1::add_path(drives_z_resident_path, "temp\\");
        common::ro_std_file_stream rom_file_stream(path, true);
        progress_changed_callback wrapped_cb_1 = nullptr;

        if (progress_cb) {
            wrapped_cb_1 = [progress_cb](const std::size_t taked, const std::size_t total) {
                progress_cb(taked / 3, total);
            };
        }

        const bool err = loader::dump_rom_files(reinterpret_cast<common::ro_stream *>(&rom_file_stream), temp_z_path, wrapped_cb_1, cancel_cb);

        if (!err) {
            common::delete_folder(temp_z_path);
            return device_installation_rom_file_corrupt;
        }

        if (progress_cb) {
            progress_cb(1, 3);
        }

        epocver ver = determine_rpkg_symbian_version(temp_z_path);

        std::string manufacturer;
        std::string firmcode;
        std::string model;

        if (!determine_rpkg_product_info(temp_z_path, manufacturer, firmcode, model)) {
            LOG_ERROR(SYSTEM, "Revert all changes");
            eka2l1::common::delete_folder(temp_z_path);

            return device_installation_determine_product_failure;
        }

        if (dvcmngr->get(firmcode)) {
            LOG_ERROR(SYSTEM, "The device already exists, revert all changes");
            eka2l1::common::delete_folder(temp_z_path);

            return device_installation_already_exist;
        }

        auto firmcode_low = common::lowercase_string(firmcode);

        // Rename temp folder to its product code
        eka2l1::common::move_file(temp_z_path, add_path(drives_z_resident_path, firmcode_low + "\\"));
        const add_device_error err_adddvc = dvcmngr->add_new_device(firmcode, model, manufacturer, ver, 0);

        if (err_adddvc != add_device_none) {
            LOG_ERROR(SYSTEM, "This device ({}) failed to be install, revert all changes", firmcode);
            common::delete_folder(add_path(drives_z_resident_path, firmcode_low + "\\"));

            return device_installation_general_failure;
        }

        if (progress_cb) {
            progress_cb(2, 3);
        }

        const std::string rom_path = add_path(rom_resident_path, firmcode_low + "\\");
        common::create_directories(rom_path);

        eka2l1::common::copy_file(path, eka2l1::add_path(rom_path, "SYM.ROM"), true);

        if (progress_cb) {
            progress_cb(3, 3);
        }

        return device_installation_none;
    }

    device_installation_error install_rpkg(device_manager *dvcmngr, const std::string &path, const std::string &devices_rom_path,
        std::string &firmware_code_ret, progress_changed_callback progress_cb, cancel_requested_callback cancel_cb) {
        FILE *f = common::open_c_file(path.data(), "rb");

        if (!f) {
            return device_installation_not_exist;
        }

        fseek(f, 0, SEEK_END);

        // Total steps is 10. For progress reporting.
        const std::size_t total_size = ftell(f) * 10 / 9;

        fseek(f, 0, SEEK_SET);

        rpkg_header header;

        std::size_t total_read_size = 0;

        if (fread(&header.magic, 4, 4, f) != 4) {
            return device_installation_insufficent;
        }

        total_read_size = 16;
        std::uint8_t is_ver_one = 1;

        if ((header.magic[0] == 'R') && (header.magic[1] == 'P') && (header.magic[2] == 'K')) {
            switch (header.magic[3]) {
            case 'G':
                is_ver_one = 1;
                break;

            case '2':
                is_ver_one = 0;
                break;

            default:
                fclose(f);
                return device_installation_rpkg_corrupt;
            }
        }

        total_read_size += fread(&header.major_rom, 1, 1, f);
        total_read_size += fread(&header.minor_rom, 1, 1, f);
        total_read_size += fread(&header.build_rom, 1, 2, f);
        total_read_size += fread(&header.count, 1, 4, f);

        header.machine_uid = 0;
        header.header_size = 0;

        if (!is_ver_one) {
            total_read_size += fread(&header.header_size, 1, 4, f);
            total_read_size += fread(&header.machine_uid, 1, 4, f);
        } else {
            header.header_size = 24;
        }

        if (total_read_size != header.header_size) {
            fclose(f);
            return device_installation_rpkg_corrupt;
        }

        while (!feof(f)) {
            total_read_size = 0;

            rpkg_entry entry;

            total_read_size += fread(&entry.attrib, 1, 8, f);
            total_read_size += fread(&entry.time, 1, 8, f);
            total_read_size += fread(&entry.path_len, 1, 8, f);

            if (total_read_size != 24) {
                break;
            }

            entry.path.resize(entry.path_len);

            total_read_size = 0;

            total_read_size += fread(entry.path.data(), 1, entry.path_len * 2, f);
            total_read_size += fread(&entry.data_size, 1, 8, f);

            if (total_read_size != entry.path_len * 2 + 8) {
                break;
            }

            LOG_INFO(SYSTEM, "Extracting: {}", common::ucs2_to_utf8(entry.path));

            if (!extract_file(devices_rom_path, f, entry, total_size, progress_cb, cancel_cb)) {
                break;
            }

            if (progress_cb) {
                progress_cb(ftell(f), total_size);
            }
        }

        fclose(f);

        const std::string folder_extracted = add_path(devices_rom_path, "temp\\");

        if (cancel_cb && cancel_cb()) {
            common::delete_folder(folder_extracted);
            return device_installation_general_failure;
        }

        if (progress_cb) {
            progress_cb(9, 10);
        }

        const device_installation_error fin_err = finalize_extracted_rpkg(dvcmngr, devices_rom_path,
            header.machine_uid, firmware_code_ret);

        if (fin_err != device_installation_none) {
            return fin_err;
        }

        if (progress_cb) {
            progress_cb(10, 10);
        }

        return device_installation_none;
    }

    // The post-extraction half of an RPKG install: detect the product from the
    // extracted temp folder, register the device and move the folder to its
    // firmware-code name. Shared between install_rpkg() and the streaming
    // installer below.
    static device_installation_error finalize_extracted_rpkg(device_manager *dvcmngr, const std::string &devices_rom_path,
        const std::uint32_t machine_uid, std::string &firmware_code_ret) {
        const std::string folder_extracted = add_path(devices_rom_path, "temp\\");

        epocver ver = determine_rpkg_symbian_version(folder_extracted);

        std::string manufacturer;
        std::string firmcode;
        std::string model;

        if (!determine_rpkg_product_info(folder_extracted, manufacturer, firmcode, model)) {
            LOG_ERROR(SYSTEM, "Revert all changes");
            common::delete_folder(folder_extracted);

            return device_installation_determine_product_failure;
        }

        if (dvcmngr->get(firmcode)) {
            LOG_ERROR(SYSTEM, "The device already exists, revert all changes");
            common::delete_folder(folder_extracted);

            return device_installation_already_exist;
        }

        auto firmcode_low = common::lowercase_string(firmcode);
        firmware_code_ret = firmcode_low;

        // Rename temp folder to its product code
        eka2l1::common::move_file(folder_extracted, add_path(devices_rom_path, firmcode_low + "\\"));
        const add_device_error err_adddvc = dvcmngr->add_new_device(firmcode, model, manufacturer, ver, machine_uid);

        if (err_adddvc != add_device_none) {
            LOG_ERROR(SYSTEM, "This device ({}) failed to be install, revert all changes", firmcode);
            common::delete_folder(add_path(devices_rom_path, firmcode_low + "\\"));

            return device_installation_general_failure;
        }

        return device_installation_none;
    }

    // ---- streaming (push-mode) installer ----------------------------------

    rpkg_stream_installer::rpkg_stream_installer(device_manager *dvcmngr, const std::string &devices_rom_path)
        : dvcmngr_(dvcmngr)
        , devices_rom_path_(devices_rom_path)
        , state_(parse_state::header_fixed)
        , need_(24)
        , header_{}
        , entry_{}
        , data_left_(0)
        , discard_rest_(false)
        , finished_(false) {
        pending_.reserve(64);
    }

    rpkg_stream_installer::~rpkg_stream_installer() {
        if (!finished_) {
            abort();
        }
    }

    void rpkg_stream_installer::open_entry_output() {
        // Byte-exact mirror of extract_file()'s path derivation.
        std::string file_full_relative = common::ucs2_to_utf8(entry_.path.substr(3));
        std::transform(file_full_relative.begin(), file_full_relative.end(), file_full_relative.begin(),
            ::tolower);

        std::string real_path = add_path(add_path(devices_rom_path_, "/temp/"), file_full_relative);

        std::string dir = eka2l1::file_directory(real_path);
        common::create_directories(dir);

        out_ = std::make_unique<common::wo_std_file_stream>(real_path, true);

        if (!out_->valid()) {
            // install_rpkg() breaks out of the extraction loop here and still
            // finalizes; mirror that by discarding everything that follows.
            LOG_INFO(SYSTEM, "Skipping with real path: {}, dir: {}", real_path, dir);
            out_.reset();
            discard_rest_ = true;
        }
    }

    bool rpkg_stream_installer::feed(const std::uint8_t *data, std::size_t size) {
        while (size > 0) {
            if (state_ == parse_state::broken) {
                return false;
            }

            if (discard_rest_) {
                return true;
            }

            if (state_ == parse_state::entry_data) {
                const std::size_t take = static_cast<std::size_t>(
                    std::min<std::uint64_t>(data_left_, static_cast<std::uint64_t>(size)));

                if (out_) {
                    if (out_->write(data, static_cast<std::uint64_t>(take)) != take) {
                        LOG_ERROR(SYSTEM, "Write failure while extracting {}", common::ucs2_to_utf8(entry_.path));
                        out_.reset();
                        discard_rest_ = true;
                        return true;
                    }
                }

                data += take;
                size -= take;
                data_left_ -= take;

                if (data_left_ == 0) {
                    out_.reset();
                    state_ = parse_state::entry_meta;
                    need_ = 24;
                    pending_.clear();
                }

                continue;
            }

            // Fixed-size piece: accumulate until `need_` bytes are buffered.
            const std::size_t take = std::min<std::size_t>(need_ - pending_.size(), size);
            pending_.insert(pending_.end(), data, data + take);
            data += take;
            size -= take;

            if (pending_.size() < need_) {
                return true;
            }

            const std::uint8_t *p = pending_.data();

            switch (state_) {
            case parse_state::header_fixed: {
                // fread(&header.magic, 4, 4): the magic block is 16 bytes.
                std::memcpy(header_.magic, p, 16);
                std::memcpy(&header_.major_rom, p + 16, 1);
                std::memcpy(&header_.minor_rom, p + 17, 1);
                std::memcpy(&header_.build_rom, p + 18, 2);
                std::memcpy(&header_.count, p + 20, 4);

                header_.machine_uid = 0;
                header_.header_size = 0;

                std::uint8_t is_ver_one = 1;

                if ((header_.magic[0] == 'R') && (header_.magic[1] == 'P') && (header_.magic[2] == 'K')) {
                    switch (header_.magic[3]) {
                    case 'G':
                        is_ver_one = 1;
                        break;

                    case '2':
                        is_ver_one = 0;
                        break;

                    default:
                        state_ = parse_state::broken;
                        return false;
                    }
                }

                if (is_ver_one) {
                    header_.header_size = 24;
                    state_ = parse_state::entry_meta;
                    need_ = 24;
                } else {
                    state_ = parse_state::header_v2_extra;
                    need_ = 8;
                }

                pending_.clear();
                break;
            }

            case parse_state::header_v2_extra: {
                std::memcpy(&header_.header_size, p, 4);
                std::memcpy(&header_.machine_uid, p + 4, 4);

                // install_rpkg(): total consumed (32) must equal header_size.
                if (header_.header_size != 32) {
                    state_ = parse_state::broken;
                    return false;
                }

                state_ = parse_state::entry_meta;
                need_ = 24;
                pending_.clear();
                break;
            }

            case parse_state::entry_meta: {
                std::memcpy(&entry_.attrib, p, 8);
                std::memcpy(&entry_.time, p + 8, 8);
                std::memcpy(&entry_.path_len, p + 16, 8);

                // install_rpkg() has no sanity check here, but a corrupt
                // length would make us buffer gigabytes — refuse instead.
                if ((entry_.path_len < 4) || (entry_.path_len > 0x8000)) {
                    LOG_ERROR(SYSTEM, "Suspicious RPKG entry path length {}", entry_.path_len);
                    state_ = parse_state::broken;
                    return false;
                }

                state_ = parse_state::entry_path_and_size;
                need_ = static_cast<std::size_t>(entry_.path_len) * 2 + 8;
                pending_.clear();
                break;
            }

            case parse_state::entry_path_and_size: {
                entry_.path.resize(entry_.path_len);
                std::memcpy(entry_.path.data(), p, entry_.path_len * 2);
                std::memcpy(&entry_.data_size, p + entry_.path_len * 2, 8);

                LOG_INFO(SYSTEM, "Extracting: {}", common::ucs2_to_utf8(entry_.path));

                open_entry_output();

                data_left_ = entry_.data_size;
                state_ = parse_state::entry_data;
                pending_.clear();

                // Zero-byte files: no data follows, complete immediately.
                if (!discard_rest_ && (data_left_ == 0)) {
                    out_.reset();
                    state_ = parse_state::entry_meta;
                    need_ = 24;
                }
                break;
            }

            default:
                state_ = parse_state::broken;
                return false;
            }
        }

        return true;
    }

    device_installation_error rpkg_stream_installer::finish(std::string &firmware_code_ret) {
        out_.reset();
        finished_ = true;

        if (state_ == parse_state::broken) {
            common::delete_folder(add_path(devices_rom_path_, "temp\\"));
            return device_installation_rpkg_corrupt;
        }

        // A trailing partial entry matches install_rpkg()'s lenient break-and-
        // finalize behaviour; only data cut off mid-payload is worth a warning.
        if (state_ == parse_state::entry_data && data_left_ > 0) {
            LOG_WARN(SYSTEM, "RPKG ended mid-file ({} bytes missing), finalizing anyway",
                data_left_);
        }

        return finalize_extracted_rpkg(dvcmngr_, devices_rom_path_, header_.machine_uid, firmware_code_ret);
    }

    void rpkg_stream_installer::abort() {
        out_.reset();
        finished_ = true;
        common::delete_folder(add_path(devices_rom_path_, "temp\\"));
    }
}
