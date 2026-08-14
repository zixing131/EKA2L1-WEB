/*
 * Copyright (c) 2018 EKA2L1 Team
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

#include <services/applist/applist.h>
#include <services/applist/op.h>
#include <services/fs/fs.h>
#include <services/context.h>
#include <services/fbs/fbs.h>

#include <common/benchmark.h>
#include <common/cvt.h>
#include <common/log.h>
#include <common/path.h>
#include <common/pystr.h>
#include <common/types.h>

#include <common/common.h>
#include <common/crypt.h>
#include <kernel/kernel.h>
#include <loader/rsc.h>
#include <system/epoc.h>
#include <utils/apacmd.h>
#include <utils/bafl.h>
#include <utils/des.h>
#include <vfs/vfs.h>

#include <functional>
#include <algorithm>
#include <cstring>
#include <utils/err.h>

#include <config/config.h>

namespace eka2l1 {
    static constexpr epoc::uid JAVA_MIDLET_APPLICATION_TYPE_UID = 0x10210E26;
    static constexpr epoc::uid JAVA_MIDLET_LEGACY_TYPE_ID = 0xB031C52A;
    static constexpr epoc::uid PREFIXED_NON_NATIVE_REGISTRATION_UID = 0x1027289D;
    static constexpr char16_t NON_NATIVE_RESOURCE_DIRECTORY[] = u"\\Private\\10003a3f\\import\\apps\\NonNative\\Resource\\";

    static const std::array<std::u16string, 9> RECOG_MIME_TYPES = {
        u"image/png",
        u"image/jpeg",
        u"image/bmp",
        u"audio/mpeg",
        u"video/mp4",
        u"text/html",
        u"text/xml",
        u"application/x-shockwave-flash",
        u"application/octet-stream"
    };

    static void serialize_mime_arrays(common::chunkyseri &seri) {
        utils::cardinality card(static_cast<std::uint32_t>(RECOG_MIME_TYPES.size()));
        card.serialize(seri);

        std::uint32_t uid = 0;

        // Make copy
        for (auto mime: RECOG_MIME_TYPES) {
            epoc::absorb_des_string(mime, seri, true);
            seri.absorb(uid);
        }
    } 

    static void populate_icon_sizes(common::chunkyseri &seri, apa_app_registry *reg) {
        std::uint32_t size = reg->app_icons.size();
        seri.absorb(size);
        for (const auto &icon : reg->app_icons) {
            seri.absorb(icon.bmp_->bitmap_->header_.size_pixels.x);
            seri.absorb(icon.bmp_->bitmap_->header_.size_pixels.y);
        }
    }

    const std::string get_app_list_server_name_by_epocver(const epocver ver) {
        if (ver < epocver::epoc81a) {
            return "AppListServer";
        }

        return "!AppListServer";
    }

    static const char16_t *APA_APP_RUNNER = u"apprun.exe";

    applist_server::applist_server(system *sys)
        : service::typical_server(sys, get_app_list_server_name_by_epocver(sys->get_symbian_version_use()))
        , drive_change_handle_(0)
        , fbsserv(nullptr)
        , fsserv(nullptr)
#ifdef __EMSCRIPTEN__
        // The web build loads registries synchronously (the pool's wait()
        // deadlocks the browser main thread), so don't spawn a worker per
        // hardware thread just to sit idle — each one costs a Web Worker plus
        // a wasm stack, which counts against iOS Safari's per-tab memory cap.
        , loading_thread_pool_(1) {
#else
        , loading_thread_pool_(std::thread::hardware_concurrency() <= 0 ? 4 : std::thread::hardware_concurrency()) {
#endif
    }

    applist_server::~applist_server() {
        io_system *io = sys->get_io_system();
        io->remove_drive_change_notify(drive_change_handle_);
    }

    bool applist_server::load_registry_oldarch(eka2l1::io_system *io, const std::u16string &path, drive_number land_drive,
        const language ideal_lang) {
        symfile f = io->open_file(path, READ_MODE | BIN_MODE);

        if (!f) {
            return false;
        }

        std::uint64_t last_mof = 0;

        {
            const std::lock_guard<std::mutex> guard(list_access_mut_);

            auto existed = std::find_if(regs.begin(), regs.end(), [=](const apa_app_registry &reg) {
                return (common::compare_ignore_case(reg.rsc_path, path) == 0);
            });

            last_mof = f->last_modify_since_0ad();

            if (existed != regs.end()) {
                if (existed->last_rsc_modified != last_mof) {
                    regs.erase(existed);
                } else {
                    return false;
                }
            }
        }

        eka2l1::ro_file_stream std_rsc_raw(f.get());
        if (!std_rsc_raw.valid()) {
            LOG_ERROR(SERVICE_APPLIST, "Registry file {} is invalid!", common::ucs2_to_utf8(path));
            return false;
        }

        apa_app_registry reg;

        reg.land_drive = land_drive;
        reg.rsc_path = path;
        reg.last_rsc_modified = last_mof;
        reg.mandatory_info.app_path = eka2l1::replace_extension(path, u".app"); // It seems so.

        if (!read_registeration_info_aif(reinterpret_cast<common::ro_stream *>(&std_rsc_raw), reg, land_drive,
                ideal_lang)) {
            return false;
        }

        if (reg.mandatory_info.short_caption.get_length() == 0) {
            // Use the filename of the register file as the caption
            const std::u16string caption_to_use = eka2l1::replace_extension(eka2l1::filename(path, true),
                u"");

            reg.mandatory_info.short_caption.assign(nullptr, caption_to_use);
            reg.mandatory_info.long_caption.assign(nullptr, caption_to_use);
        }

        address romaddr = static_cast<address>(f->seek(0, file_seek_mode::address));

        if (romaddr == 0xFFFFFFFF) {
            romaddr = 0;
        }

        f->seek(0, file_seek_mode::beg);

        {
            // Icon creation modifies kernel and services (creating bitmap)
            // So we use a mutex to safeguard it
            const std::lock_guard<std::mutex> guard(list_access_mut_);

            if (!read_icon_data_aif(reinterpret_cast<common::ro_stream *>(&std_rsc_raw), fbsserv, reg.app_icons,
                                    romaddr)) {
                return false;
            }
        }

        std::u16string caption_file_path = eka2l1::replace_extension(path, u"") + u"_caption.rsc";
        caption_file_path = utils::get_nearest_lang_file(io, caption_file_path, ideal_lang, land_drive);

        f = io->open_file(caption_file_path, READ_MODE | BIN_MODE);

        if (f) {
            eka2l1::ro_file_stream caption_file_stream(f.get());
            if (!caption_file_stream.valid()) {
                LOG_INFO(SERVICE_APPLIST, "Caption file for {} is corrupted!", common::ucs2_to_utf8(reg.mandatory_info.short_caption.to_std_string(nullptr)));
            } else {
                if (!read_caption_data_oldarch(reinterpret_cast<common::ro_stream *>(&caption_file_stream), reg)) {
                    LOG_INFO(SERVICE_APPLIST, "Failed to read caption file for {}", common::ucs2_to_utf8(reg.mandatory_info.short_caption.to_std_string(nullptr)));
                }
            }
        }

        if (reg.mandatory_info.short_caption.get_length() == 0) {
            reg.caps.is_hidden = true;
        }

        const std::lock_guard<std::mutex> guard(list_access_mut_);
        regs.push_back(std::move(reg));

        return true;
    }

    bool applist_server::load_registry(eka2l1::io_system *io, const std::u16string &path, drive_number land_drive,
        const language ideal_lang) {
        // common::benchmarker marker(__FUNCTION__);
        const std::u16string nearest_path = utils::get_nearest_lang_file(io, path, ideal_lang, land_drive);
        symfile f = io->open_file(nearest_path, READ_MODE | BIN_MODE);

        if (!f) {
            return false;
        }

        std::uint64_t last_modified = 0;

        {
            const std::lock_guard<std::mutex> guard(list_access_mut_);

            auto find_result = std::find_if(regs.begin(), regs.end(), [=](const apa_app_registry &reg) {
                return (common::compare_ignore_case(reg.rsc_path, nearest_path) == 0);
            });

            last_modified = f->last_modify_since_0ad();

            if (find_result != regs.end()) {
                if (find_result->last_rsc_modified != last_modified) {
                    regs.erase(find_result);
                } else {
                    return false;
                }
            }
        }

        apa_app_registry reg;

        reg.land_drive = land_drive;
        reg.rsc_path = nearest_path;
        reg.last_rsc_modified = last_modified;

        auto read_rsc_from_file = [](symfile &f, const int id, const bool confirm_sig,
                                     std::uint32_t *uid1, std::uint32_t *uid2,
                                     std::uint32_t *uid3, const std::size_t file_offset) -> std::vector<std::uint8_t> {
            if (!f || (file_offset >= f->size())) {
                return {};
            }

            std::vector<std::uint8_t> file_data(static_cast<std::size_t>(f->size()) - file_offset);
            f->seek(file_offset, file_seek_mode::beg);
            if (f->read_file(file_data.data(), 1, static_cast<std::uint32_t>(file_data.size())) != file_data.size()) {
                return {};
            }

            common::ro_buf_stream std_rsc_raw(file_data.data(), file_data.size());
            if (!std_rsc_raw.valid()) {
                return {};
            }

            loader::rsc_file std_rsc(reinterpret_cast<common::ro_stream *>(&std_rsc_raw));

            if (confirm_sig) {
                std_rsc.confirm_signature();
            }

            if (uid1) *uid1 = std_rsc.get_uid(1);
            if (uid2) *uid2 = std_rsc.get_uid(2);
            if (uid3) *uid3 = std_rsc.get_uid(3);

            return std_rsc.read(id);
        };

        // Open the file
        std::uint32_t outer_uid1 = 0;
        std::uint32_t outer_uid2 = 0;
        std::uint32_t checked_uids[4] = {};
        f->seek(0, file_seek_mode::beg);
        const bool has_checked_uid = f->read_file(checked_uids, 1, sizeof(checked_uids)) == sizeof(checked_uids);
        const bool is_prefixed_non_native = has_checked_uid
            && (checked_uids[0] == PREFIXED_NON_NATIVE_REGISTRATION_UID)
            && (crypt::calculate_checked_uid_checksum(checked_uids) == checked_uids[3]);
        const std::size_t registration_file_offset = is_prefixed_non_native ? sizeof(checked_uids) : 0;

        auto dat = read_rsc_from_file(f, 1, false,
            is_prefixed_non_native ? nullptr : &outer_uid1,
            is_prefixed_non_native ? nullptr : &outer_uid2,
            is_prefixed_non_native ? nullptr : &reg.mandatory_info.uid,
            registration_file_offset);

        if (is_prefixed_non_native) {
            outer_uid1 = checked_uids[0];
            outer_uid2 = checked_uids[1];
            reg.mandatory_info.uid = checked_uids[2];
        }

        if (dat.empty()) {
            return false;
        }

        common::ro_buf_stream app_info_resource_stream(&dat[0], dat.size());
        bool result = read_registeration_info(reinterpret_cast<common::ro_stream *>(&app_info_resource_stream),
            reg, land_drive, kern->get_epoc_version() < epocver::epoc95);

        if (!result) {
            return false;
        }

        if ((outer_uid1 == PREFIXED_NON_NATIVE_REGISTRATION_UID)
            || (reg.caps.flags & apa_capability::non_native)) {
            reg.caps.flags |= apa_capability::non_native;
            reg.non_native_application_type = outer_uid2;
            reg.non_native_opaque_data = read_rsc_from_file(f, 2, false, nullptr, nullptr, nullptr,
                registration_file_offset);
        }

        // Getting our localised resource info
        if (reg.localised_info_rsc_path.empty()) {
            // Assume default path is used
            reg.localised_info_rsc_path.append(1, drive_to_char16(land_drive));
            reg.localised_info_rsc_path += u":\\resource\\apps\\";

            std::u16string name_rsc = eka2l1::replace_extension(eka2l1::filename(reg.rsc_path), u"");

            if (common::lowercase_ucs2_string(name_rsc.substr(name_rsc.length() - 4, 4)) == u"_reg") {
                name_rsc.erase(name_rsc.length() - 4, 4);
            }

            reg.localised_info_rsc_path += name_rsc + u".rsc";
        }

        // Absolute the localised info path
        reg.localised_info_rsc_path = eka2l1::absolute_path(reg.localised_info_rsc_path,
            eka2l1::root_dir(path), true);

        const auto localised_path = utils::get_nearest_lang_file(io, reg.localised_info_rsc_path,
            ideal_lang, land_drive);

        if (localised_path.empty()) {
            const std::lock_guard<std::mutex> guard(list_access_mut_);
            regs.push_back(std::move(reg));
            return true;
        }

        f = io->open_file(localised_path, READ_MODE | BIN_MODE);

        dat = read_rsc_from_file(f, reg.localised_info_rsc_id, true, nullptr, nullptr, nullptr, 0);

        if (dat.empty()) {
            const std::lock_guard<std::mutex> guard(list_access_mut_);
            regs.push_back(std::move(reg));
            return true;
        }

        common::ro_buf_stream localised_app_info_resource_stream(dat.data(), dat.size());

        // Read localised info
        // Ignore result
        if (localised_app_info_resource_stream.valid()) {
            read_localised_registration_info(reinterpret_cast<common::ro_stream *>(&localised_app_info_resource_stream),
                reg, land_drive);
        }

        LOG_INFO(SERVICE_APPLIST, "Found app: {}, uid: 0x{:X}",
            common::ucs2_to_utf8(reg.mandatory_info.long_caption.to_std_string(nullptr)),
            reg.mandatory_info.uid);

        if (!eka2l1::is_absolute(reg.icon_file_path, std::u16string(u"c:\\"), true)) {
            // Try to absolute icon path
            // Search the registration file drive, and than the localizable registration file
            std::u16string try_1 = eka2l1::absolute_path(reg.icon_file_path,
                std::u16string(1, drive_to_char16(land_drive)) + u":\\", true);

            if (io->exist(try_1)) {
                reg.icon_file_path = try_1;
            } else {
                try_1[0] = localised_path[0];

                if (io->exist(try_1)) {
                    reg.icon_file_path = try_1;
                } else {
                    reg.icon_file_path = u"";
                }
            }
        }

        const std::lock_guard<std::mutex> guard(list_access_mut_);
        regs.push_back(std::move(reg));
        return true;
    }

    bool applist_server::delete_registry(const std::u16string &rsc_path) {
        const std::lock_guard<std::mutex> guard(list_access_mut_);

        auto result = std::find_if(regs.begin(), regs.end(), [rsc_path](const apa_app_registry &reg) {
            return common::compare_ignore_case(reg.rsc_path, rsc_path) == 0;
        });

        if (result == regs.end()) {
            return false;
        }

        regs.erase(result);
        return true;
    }

    void applist_server::sort_registry_list() {
        std::sort(regs.begin(), regs.end(), [](const apa_app_registry &lhs, const apa_app_registry &rhs) {
            return lhs.mandatory_info.uid < rhs.mandatory_info.uid;
        });
    }

    void applist_server::remove_registries_on_drive(const drive_number drv) {
        common::erase_elements(regs, [drv](const apa_app_registry &reg) {
            return reg.land_drive == drv;
        });
    }

    static const char *OLDARCH_REG_FILE_EXT = ".aif";
    static const char *NEWARCH_REG_FILE_EXT = ".r??";
    static const char16_t *NEWARCH_REG_FILE_SEARCH_WILDCARD16 = u"*.r??";

    void applist_server::on_drive_change(void *userdata, drive_number drv, drive_action act) {
        io_system *io = reinterpret_cast<io_system *>(userdata);
        std::atomic_bool modified = false;

        switch (act) {
        case drive_action_mount: {
            avail_drives_ |= 1 << (drv - drive_a);
            std::vector<std::u16string> register_file_paths;

            if (kern->is_eka1()) {
                rescan_registries_on_drive_oldarch(io, drv, register_file_paths);
            } else {
                rescan_registries_on_drive_newarch(io, drv, register_file_paths);
            }

#ifdef __EMSCRIPTEN__
            for (std::size_t idx = 0; idx < register_file_paths.size(); idx++) {
                bool entry_modified = kern->is_eka1()
                    ? load_registry_oldarch(io, register_file_paths[idx], drive_number(idx % drive_count), language::en)
                    : load_registry(io, register_file_paths[idx], drive_number(idx % drive_count), language::en);
                if (entry_modified) modified = true;
            }
#else
            auto load_registry_task = loading_thread_pool_.submit_loop<std::size_t>(0, register_file_paths.size(),
                [this, &register_file_paths, &modified, io](std::size_t idx) {
                    bool entry_modified = false;

                    if (kern->is_eka1()) {
                        entry_modified = load_registry_oldarch(io, register_file_paths[idx], drive_number(idx % drive_count), language::en);
                    } else {
                        entry_modified = load_registry(io, register_file_paths[idx], drive_number(idx % drive_count), language::en);
                    }

                    if (entry_modified) {
                        modified = true;
                    }
                });

            load_registry_task.wait();
#endif
            break;
        }

        case drive_action_unmount:
            avail_drives_ &= ~(1 << (drv - drive_a));
            remove_registries_on_drive(drv);
            modified = true;

            break;

        default:
            break;
        }

        if (modified) {
            sort_registry_list();
        }
    }

    void applist_server::rescan_registries_on_drive_oldarch(eka2l1::io_system *io, const drive_number drv,
        std::vector<std::u16string> &register_file_paths) {
        const std::u16string base_dir = std::u16string(1, drive_to_char16(drv)) + u":\\System\\Apps\\";
        auto reg_dir = io->open_dir(base_dir, {}, io_attrib_include_dir);

        if (reg_dir) {
            while (auto ent = reg_dir->get_next_entry()) {
                if ((ent->type == io_component_type::dir) && (ent->name != ".") && (ent->name != "..")) {
                    const std::u16string aif_reg_file = common::utf8_to_ucs2(eka2l1::add_path(
                        ent->full_path, ent->name + OLDARCH_REG_FILE_EXT, true));

                    register_file_paths.push_back(aif_reg_file);
                }
            }
        }
    }

    void applist_server::rescan_registries_on_drive_newarch(eka2l1::io_system *io, const drive_number drv,
        std::vector<std::u16string> &register_file_paths) {
        const std::u16string import_rsc_path = std::u16string(1, drive_to_char16(drv)) + u":\\Private\\10003a3f\\import\\apps\\" + NEWARCH_REG_FILE_SEARCH_WILDCARD16;
        const std::u16string non_native_rsc_path = std::u16string(1, drive_to_char16(drv)) + NON_NATIVE_RESOURCE_DIRECTORY + NEWARCH_REG_FILE_SEARCH_WILDCARD16;
        const std::u16string rom_rscs_path = std::u16string(1, drive_to_char16(drv)) + u":\\Private\\10003a3f\\apps\\" + NEWARCH_REG_FILE_SEARCH_WILDCARD16;

        // Supposedly to only scan in ROM, but it's not really that strict on the emulator ;)
        rescan_registries_on_drive_newarch_with_path(io, drv, rom_rscs_path, register_file_paths);
        rescan_registries_on_drive_newarch_with_path(io, drv, import_rsc_path, register_file_paths);
        rescan_registries_on_drive_newarch_with_path(io, drv, non_native_rsc_path, register_file_paths);
    }

    void applist_server::rescan_registries_on_drive_newarch_with_path(eka2l1::io_system *io, const drive_number drv, const std::u16string &path,
        std::vector<std::u16string> &results) {
        auto reg_dir = io->open_dir(path, {}, io_attrib_include_file);
        bool modded = false;

        if (reg_dir) {
            while (auto ent = reg_dir->get_next_entry()) {
                if (ent->type == io_component_type::file) {
                    results.push_back(common::utf8_to_ucs2(ent->full_path));
                }
            }
        }
    }

    bool applist_server::rescan_registries(eka2l1::io_system *io) {
        LOG_INFO(SERVICE_APPLIST, "Loading app registries");

        std::atomic_bool global_modified = false;

        if (avail_drives_ == 0) {
            for (drive_number drv = drive_z; drv >= drive_a; drv--) {
                if (io->get_drive_entry(drv)) {
                    avail_drives_ |= 1 << (drv - drive_a);
                }
            }
        }

        // Delete entries that no longer exist...
        std::size_t prev = regs.size();

        common::erase_elements(regs, [io](const apa_app_registry &reg) {
            return !io->exist(reg.rsc_path);
        });

        if (prev != regs.size()) {
            global_modified = true;
        }

        std::vector<std::u16string> register_file_paths;

        for (std::uint8_t i = 0; i < drive_count; i++) {
            if (avail_drives_ & (1 << i)) {
                drive_number drv = static_cast<drive_number>(static_cast<int>(drive_a) + i);

                if (kern->is_eka1()) {
                    rescan_registries_on_drive_oldarch(io, drv, register_file_paths);
                } else {
                    rescan_registries_on_drive_newarch(io, drv, register_file_paths);
                }
            }
        }

        auto current_lang = kern->get_current_language();

#ifdef __EMSCRIPTEN__
        for (std::size_t idx = 0; idx < register_file_paths.size(); idx++) {
            auto path = register_file_paths[idx];
            drive_number drv = char16_to_drive(path[0]);
            bool modified = kern->is_eka1()
                ? load_registry_oldarch(io, register_file_paths[idx], drv, current_lang)
                : load_registry(io, register_file_paths[idx], drv, current_lang);
            if (modified) global_modified = true;
        }
#else
        auto load_registry_task = loading_thread_pool_.submit_loop<std::size_t>(0, register_file_paths.size(),
            [this, &register_file_paths, &global_modified, io, current_lang](std::size_t idx) {
                bool modified = false;

                auto path = register_file_paths[idx];
                drive_number drv = char16_to_drive(path[0]);

                if (kern->is_eka1()) {
                    modified = load_registry_oldarch(io, register_file_paths[idx], drv, current_lang);
                } else {
                    modified = load_registry(io, register_file_paths[idx], drv, current_lang);
                }

                if (modified) {
                    global_modified = true;
                }
            });

        load_registry_task.wait();
#endif

        if (global_modified) {
            sort_registry_list();
        }

        // Register drive change callback
        if (!drive_change_handle_) {
            drive_change_handle_ = io->register_drive_change_notify([this](void *userdata, drive_number drv, drive_action act) {
                return on_drive_change(userdata, drv, act);
            }, io);
        }

        LOG_INFO(SERVICE_APPLIST, "Done loading!");
        return global_modified.load();
    }

    int applist_server::legacy_level() {
        if (kern->get_epoc_version() < epocver::epoc7) {
            return APA_LEGACY_LEVEL_OLD;
        } else if (kern->get_epoc_version() < epocver::epoc81a) {
            return APA_LEGACY_LEVEL_S60V2;
        } else if (kern->get_epoc_version() < epocver::eka2) {
            return APA_LEGACY_LEVEL_TRANSITION;
        }

        return APA_LEGACY_LEVEL_MORDEN;
    }

    void applist_server::connect(service::ipc_context &ctx) {
        create_session<applist_session>(&ctx);
        server::connect(ctx);
    }

    void applist_server::init() {
        fbsserv = reinterpret_cast<fbs_server *>(kern->get_by_name<service::server>(
            epoc::get_fbs_server_name_by_epocver(kern->get_epoc_version())));

        fsserv = kern->get_by_name<eka2l1::fs_server>(epoc::fs::get_server_name_through_epocver(
            kern->get_epoc_version()));

        rescan_registries(sys->get_io_system());

        flags |= AL_INITED;
    }

    std::vector<apa_app_registry> &applist_server::get_registerations() {
        if (!(flags & AL_INITED)) {
            // Initialize
            init();
        }

        const std::lock_guard<std::mutex> guard(list_access_mut_);
        return regs;
    }

    apa_app_registry *applist_server::get_registration(const std::uint32_t uid) {
        std::unique_lock<std::mutex> guard(list_access_mut_);

        if (!(flags & AL_INITED)) {
            guard.unlock();

            // Initialize
            init();

            guard.lock();
        }

        auto result = std::lower_bound(regs.begin(), regs.end(), uid, [](const apa_app_registry &lhs, const std::uint32_t rhs) {
            return lhs.mandatory_info.uid < rhs;
        });

        if (result != regs.end() && result->mandatory_info.uid == uid) {
            return &(*result);
        }

        return nullptr;
    }

    void applist_server::is_accepted_to_run(service::ipc_context &ctx) {
        auto exe_name = ctx.get_argument_value<std::u16string>(0);

        if (!exe_name) {
            ctx.complete(false);
            return;
        }

        LOG_TRACE(SERVICE_APPLIST, "Asking permission to launch: {}, accepted", common::ucs2_to_utf8(*exe_name));
        ctx.complete(true);
    }

    void applist_server::default_screen_number(service::ipc_context &ctx) {
        const epoc::uid app_uid = *ctx.get_argument_value<epoc::uid>(0);
        apa_app_registry *reg = get_registration(app_uid);

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        ctx.complete(reg->default_screen_number);
    }

    void applist_server::app_language(service::ipc_context &ctx) {
        LOG_TRACE(SERVICE_APPLIST, "AppList::AppLanguage stubbed to returns ELangEnglish");

        language default_lang = language::en;

        ctx.write_data_to_descriptor_argument<language>(1, default_lang);
        ctx.complete(0);
    }

    void applist_server::get_app_info(service::ipc_context &ctx) {
        const epoc::uid app_uid = *ctx.get_argument_value<epoc::uid>(0);
        apa_app_registry *reg = get_registration(app_uid);

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }
        
        apa_app_info info_copy = reg->mandatory_info;
        auto map_to_host_ite = uids_app_to_executable.find(app_uid);

        if (map_to_host_ite != uids_app_to_executable.end()) {
            info_copy.app_path = std::u16string(MAPPED_EXECUTABLE_HEAD_STRING) + u"_" + map_to_host_ite->second +
                UNIQUE_MAPPED_EXTENSION_STRING;
        }

        ctx.write_data_to_descriptor_argument<apa_app_info>(1, info_copy);
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_app_icon_file_name(service::ipc_context &ctx) {
        const epoc::uid app_uid = *ctx.get_argument_value<epoc::uid>(0);
        apa_app_registry *reg = get_registration(app_uid);

        // Either the registeration doesn't exist, or the icon file doesn't exist
        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        if (reg->icon_file_path.empty()) {
            ctx.complete(epoc::error_not_supported);
            return;
        }

        epoc::filename fname_des(reg->icon_file_path);

        ctx.write_data_to_descriptor_argument<epoc::filename>(1, fname_des);
        ctx.complete(epoc::error_none);
    }

    struct app_icon_handles {
        std::uint32_t bmp_handle;
        std::uint32_t mask_bmp_handle;
    };

    void applist_server::get_app_icon(service::ipc_context &ctx) {
        std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(0);
        std::optional<std::int32_t> icon_size_width = std::nullopt;
        std::optional<std::int32_t> icon_size_height = std::nullopt;

        if (legacy_level() == APA_LEGACY_LEVEL_OLD) {
            std::optional<eka2l1::vec2> size_vec = ctx.get_argument_data_from_descriptor<eka2l1::vec2>(1);
            if (size_vec.has_value()) {
                icon_size_width = size_vec->x;
                icon_size_height = size_vec->y;
            }
        } else {
            icon_size_width = ctx.get_argument_value<std::int32_t>(1);
            icon_size_height = ctx.get_argument_value<std::int32_t>(2);
        }

        if (!app_uid || !icon_size_width || !icon_size_height) {
            ctx.complete(epoc::error_argument);
            return;
        }

        apa_app_registry *reg = get_registration(app_uid.value());

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        if (reg->app_icons.size() == 0) {
            // Usually it must have 1
            ctx.complete(epoc::error_not_supported);
            return;
        }

        app_icon_handles handle_result;

        // TODO: Iterate and choose right size. But have to do many code....
        if (reg->app_icons[0].bmp_rom_addr_) {
            handle_result.bmp_handle = reg->app_icons[0].bmp_rom_addr_;
        } else {
            handle_result.bmp_handle = reg->app_icons[0].bmp_->id;
        }

        if (reg->app_icons[1].bmp_rom_addr_) {
            handle_result.mask_bmp_handle = reg->app_icons[1].bmp_rom_addr_;
        } else {
            handle_result.mask_bmp_handle = reg->app_icons[1].bmp_->id;
        }

        if (legacy_level() == APA_LEGACY_LEVEL_OLD) {
            ctx.write_data_to_descriptor_argument<std::uint32_t>(2, handle_result.bmp_handle);
            ctx.write_data_to_descriptor_argument<std::uint32_t>(3, handle_result.mask_bmp_handle);
        } else {
            ctx.write_data_to_descriptor_argument<app_icon_handles>(3, handle_result);
        }

        ctx.complete(epoc::error_none);
    }

    void applist_server::get_app_icon_sizes(service::ipc_context &ctx) {
        std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(0);

        if (!app_uid) {
            ctx.complete(epoc::error_argument);
            return;
        }

        apa_app_registry *reg = get_registration(app_uid.value());

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        if (reg->app_icons.size() == 0) {
            ctx.complete(epoc::error_not_supported);
            return;
        }

        std::vector<std::uint8_t> buf;
        common::chunkyseri seri(nullptr, 0, common::SERI_MODE_MEASURE);
        populate_icon_sizes(seri, reg);

        buf.resize(seri.size());
        seri = common::chunkyseri(buf.data(), buf.size(), common::SERI_MODE_WRITE);
        populate_icon_sizes(seri, reg);

        ctx.write_data_to_descriptor_argument(2, buf.data(), static_cast<std::uint32_t>(buf.size()));
        ctx.complete(epoc::error_none);
    }

    void applist_server::launch_app(service::ipc_context &ctx) {
        std::optional<std::u16string> cmd_line = ctx.get_argument_value<std::u16string>(0);
        if (!cmd_line) {
            LOG_ERROR(SERVICE_APPLIST, "Failed to launch a new app! Command line is not available.");
            ctx.complete(epoc::error_argument);

            return;
        }

        common::pystr16 cmd_line_py(cmd_line.value());
        auto arguments = cmd_line_py.split();

        if (arguments.empty()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        // Simply load only
        codeseg_ptr seg = kern->get_lib_manager()->load(arguments[0].std_str());
        std::u16string app_launch = APA_APP_RUNNER;

        if (std::get<0>(seg->get_uids()) == epoc::EXECUTABLE_UID) {
            app_launch = arguments[0].std_str();
        }

        kernel::uid thread_id = 0;
        if (!launch_app(app_launch, cmd_line.value(), &thread_id, ctx.msg->own_thr->owning_process())) {
            LOG_ERROR(SERVICE_APPLIST, "Failed to create new app process (command line: {})", common::ucs2_to_utf8(cmd_line.value()));
            ctx.complete(epoc::error_no_memory);

            return;
        }

        if (kern->is_eka1()) {
            ctx.write_data_to_descriptor_argument<kernel::uid_eka1>(1, static_cast<kernel::uid_eka1>(thread_id));
        } else {
            ctx.write_data_to_descriptor_argument<kernel::uid>(1, thread_id);
        }

        ctx.complete(epoc::error_none);
    }

    void applist_server::is_program(service::ipc_context &ctx) {
        std::optional<std::u16string> path = ctx.get_argument_value<std::u16string>(0);

        if (!path.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        hle::lib_manager *lmngr = kern->get_lib_manager();
        std::int32_t is_program = false;

        if (lmngr->load(path.value())) {
            is_program = true;
        }

        ctx.write_data_to_descriptor_argument<std::int32_t>(1, is_program);
        ctx.complete(epoc::error_none);
    }

    std::u16string applist_server::native_executable_for_non_native_type(const epoc::uid type) const {
        const auto registered = non_native_app_types_.find(type);
        if (registered != non_native_app_types_.end()) {
            return registered->second;
        }

        // Java 1.x on S60 3rd Edition normally registers this during boot.
        // Keep the ROM-compatible fallback because AppArc's real type table is
        // persistent, while EKA2L1 currently reconstructs the server at boot.
        if ((type == JAVA_MIDLET_APPLICATION_TYPE_UID) || (type == JAVA_MIDLET_LEGACY_TYPE_ID)) {
            return u"StubMIDP2RecogExe.exe";
        }

        return {};
    }

    apa_app_registry *applist_server::inject_non_native_java_midlet(epoc::uid app_uid,
        const std::u16string &caption, const std::u16string &logical_jad_path) {
        if (!app_uid || logical_jad_path.empty()) {
            return nullptr;
        }

        const std::lock_guard<std::mutex> guard(list_access_mut_);

        apa_app_registry *existing = nullptr;
        for (auto &reg : regs) {
            if (reg.mandatory_info.uid == app_uid) {
                existing = &reg;
                break;
            }
        }

        if (!existing) {
            regs.emplace_back();
            existing = &regs.back();
        }

        existing->mandatory_info.uid = app_uid;
        existing->mandatory_info.app_path.assign(nullptr, logical_jad_path);
        existing->mandatory_info.short_caption.assign(nullptr, caption);
        existing->mandatory_info.long_caption.assign(nullptr, caption);
        existing->caps.flags |= apa_capability::non_native;
        existing->caps.is_hidden = 0;
        existing->caps.launch_in_background = 0;
        existing->land_drive = drive_e;
        existing->non_native_application_type = JAVA_MIDLET_APPLICATION_TYPE_UID;

        // StubMIDP2RecogExe reads two TInt32s from OpaqueData (suite UID + MIDlet
        // index) and sends them as !MIDP.SystemAMS.MIDP2 opcode 1. A UTF-16 path
        // here is the wrong payload; empty opaque is KErrEof (-25).
        existing->non_native_opaque_data.resize(8);
        const std::uint32_t uid_le = app_uid;
        const std::uint32_t midlet_le = 1;
        std::memcpy(existing->non_native_opaque_data.data(), &uid_le, 4);
        std::memcpy(existing->non_native_opaque_data.data() + 4, &midlet_le, 4);

        sort_registry_list();

        auto found = std::lower_bound(regs.begin(), regs.end(), app_uid,
            [](const apa_app_registry &lhs, const std::uint32_t rhs) {
                return lhs.mandatory_info.uid < rhs;
            });
        if (found == regs.end() || found->mandatory_info.uid != app_uid) {
            return nullptr;
        }

        LOG_INFO(SERVICE_APPLIST,
            "Injected host-side Java MIDlet uid=0x{:08X} caption='{}' jad='{}'",
            app_uid, common::ucs2_to_utf8(caption), common::ucs2_to_utf8(logical_jad_path));
        return &(*found);
    }

    void applist_session::get_native_executable_name_if_non_native(service::ipc_context &ctx) {
        std::optional<std::u16string> path = ctx.get_argument_value<std::u16string>(1);

        if (!path.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        if ((path->find(MAPPED_EXECUTABLE_HEAD_STRING) == 0) && (path->find(UNIQUE_MAPPED_EXTENSION_STRING) != std::u16string::npos)) {
            ctx.write_arg(0, kernel::BRIDAGED_EXECUTABLE_NAME);
            pending_opaque_data_.clear();
            ctx.complete(0);

            return;
        }

        applist_server *serv = server<applist_server>();
        auto &registries = serv->regs;
        const auto found = std::find_if(registries.begin(), registries.end(), [&path](apa_app_registry &reg) {
            return common::compare_ignore_case(reg.mandatory_info.app_path.to_std_string(nullptr), *path) == 0;
        });

        if ((found != registries.end()) && (found->caps.flags & apa_capability::non_native)) {
            const std::u16string launcher = serv->native_executable_for_non_native_type(found->non_native_application_type);
            if (launcher.empty()) {
                LOG_ERROR(SERVICE_APPLIST, "No native launcher registered for non-native application type 0x{:08X}",
                    found->non_native_application_type);
                ctx.complete(epoc::error_not_found);
                return;
            }

            ctx.write_arg(0, launcher);
            pending_opaque_data_ = found->non_native_opaque_data;
            ctx.complete(static_cast<int>(pending_opaque_data_.size()));
            return;
        }

        ctx.set_descriptor_argument_length(0, 0);
        pending_opaque_data_.clear();
        ctx.complete(0);
    }

    void applist_session::get_opaque_data(service::ipc_context &ctx) {
        if (pending_opaque_data_.empty()) {
            ctx.set_descriptor_argument_length(0, 0);
            ctx.complete(epoc::error_none);
            return;
        }

        if (ctx.get_argument_max_data_size(0) < pending_opaque_data_.size()) {
            ctx.complete(epoc::error_overflow);
            return;
        }

        ctx.write_data_to_descriptor_argument(0, pending_opaque_data_.data(),
            static_cast<std::uint32_t>(pending_opaque_data_.size()));
        pending_opaque_data_.clear();
        ctx.complete(epoc::error_none);
    }

    void applist_server::app_info_provided_by_reg_file(service::ipc_context &ctx) {
        std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(0);
        if (!app_uid.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        apa_app_registry *reg = get_registration(app_uid.value());
        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        const bool reg_file_available = !reg->rsc_path.empty();

        ctx.write_data_to_descriptor_argument(1, reg_file_available);
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_preferred_buf_size(service::ipc_context &ctx) {
        LOG_TRACE(SERVICE_APPLIST, "AppList::GetPreferredBufSize stubbed to 0");

        std::uint32_t buf_size = 0;
        ctx.complete(buf_size);
    }

    void applist_server::get_app_for_document_impl(service::ipc_context &ctx, const std::u16string &path) {
        applist_app_for_document app;
        app.uid = 0;
        app.data_type.uid = 0;

        config::state *conf = kern->get_config();

        if (conf && conf->mime_detection) {
            LOG_TRACE(SERVICE_APPLIST, "AppList::AppForDocument datatype stubbed with file extension");

            // Types that must be reported exactly: the guest picks an ECom
            // handler by MIME string, so the uppercased-extension placeholder
            // below resolves to nothing. SWInst for instance answers
            // KSWInstErrPackageNotSupported for a MIDlet it cannot type.
            static const struct {
                const char16_t *extension;
                const char *mime;
            } KNOWN_MIME_TYPES[] = {
                { u".swf", "application/x-shockwave-flash" },
                { u".jad", "text/vnd.sun.j2me.app-descriptor" },
                { u".jar", "application/java-archive" }
            };

            const std::u16string ext = eka2l1::path_extension(path);
            if (!ext.empty()) {
                const char *mime = nullptr;
                for (const auto &known : KNOWN_MIME_TYPES) {
                    if (common::compare_ignore_case(ext, known.extension) == 0) {
                        mime = known.mime;
                        break;
                    }
                }

                if (mime) {
                    app.data_type.data_type.assign(nullptr, mime);
                } else {
                    app.data_type.data_type.assign(nullptr, common::uppercase_string(common::ucs2_to_utf8(ext.substr(1))));
                }
            } else {
                app.data_type.data_type.assign(nullptr, "UNK");
            }
        } else {
            LOG_TRACE(SERVICE_APPLIST, "AppList::AppForDocument datatype left empty!");
        }

        ctx.write_data_to_descriptor_argument<applist_app_for_document>(0, app);
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_app_for_document_by_file_handle(service::ipc_context &ctx) {
        session_ptr fs_target_session = ctx.sys->get_kernel_system()->get<service::session>(*(ctx.get_argument_value<std::int32_t>(2)));
        const std::uint32_t fs_file_handle = *(ctx.get_argument_value<std::uint32_t>(3));
        file *source_file = fsserv->get_file(fs_target_session->unique_id(), fs_file_handle);

        if (!source_file) {
            ctx.complete(epoc::error_argument);
            return;
        }

        get_app_for_document_impl(ctx, source_file->file_name());
    }

    void applist_server::get_app_for_document(service::ipc_context &ctx) {
        std::optional<std::u16string> path = ctx.get_argument_value<std::u16string>(2);
        if (!path.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        get_app_for_document_impl(ctx, path.value());
    }

    void applist_server::get_app_for_data_type(service::ipc_context &ctx) {
        // RApaLsSession::AppForDataType(const TDataType &, TUid &). No MIME→app
        // mapping table yet, so report "no handler" the way the real server does:
        // KErrNone with a null UID. Leaving this opcode uncompleted wedges the
        // client forever (QQ2010 hangs on the splash after AppForDataType).
        LOG_TRACE(SERVICE_APPLIST, "AppList::AppForDataType stubbed (null UID)");

        const epoc::uid null_uid = 0;
        ctx.write_data_to_descriptor_argument<epoc::uid>(1, null_uid);
        ctx.complete(epoc::error_none);
    }

    data_recog_result applist_server::recognize_data_impl(common::ro_stream &stream, const std::u16string &name) {
        data_recog_result result{};
        const std::u16string extension = eka2l1::path_extension(name);

        // Match the S60 web recognizer. It identifies XHTML as text/html; it
        // does not advertise a separate application/xhtml+xml data type.
        if ((common::compare_ignore_case(extension, u".html") == 0)
            || (common::compare_ignore_case(extension, u".htm") == 0)
            || (common::compare_ignore_case(extension, u".shtml") == 0)
            || (common::compare_ignore_case(extension, u".shtm") == 0)
            || (common::compare_ignore_case(extension, u".xhtml") == 0)) {
            result.type_.type_name_.assign(nullptr, "text/html");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        if (common::compare_ignore_case(extension, u".xml") == 0) {
            result.type_.type_name_.assign(nullptr, "text/xml");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        std::uint8_t magic4[4] = { 0, 0, 0, 0 };

        stream.seek(0, common::seek_where::beg);
        stream.read(magic4, 4);

        if ((common::compare_ignore_case(extension, u".swf") == 0)
            || (((magic4[0] == 'F') || (magic4[0] == 'C') || (magic4[0] == 'Z'))
                && (magic4[1] == 'W') && (magic4[2] == 'S'))) {
            result.type_.type_name_.assign(nullptr, "application/x-shockwave-flash");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        // MP3
        if ((magic4[0] == 0xFF) && ((magic4[1] == 0xFB) || (magic4[1] == 0xF3) || (magic4[1] == 0xF2))) {
            result.type_.type_name_.assign(nullptr, "audio/mpeg");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        // MP3 ver2
        if ((magic4[0] == 0x49) && (magic4[1] == 0x44) && (magic4[2] == 0x33)) {
            result.type_.type_name_.assign(nullptr, "audio/mpeg");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        std::uint8_t magic8[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        stream.read(magic8, 8);

        if (memcmp(magic8, "ftypmp42", 8) == 0) {
            result.type_.type_name_.assign(nullptr, "video/mp4");
            result.confidence_rating_ = data_recognition_confidence_probable;
            return result;
        }

        result.type_.type_name_.assign(nullptr, "application/octet-stream");
        result.confidence_rating_ = data_recognition_confidence_probable;
        return result;
    }

    void applist_server::recognize_data(service::ipc_context &ctx) {
        std::uint8_t *data = ctx.get_descriptor_argument_ptr(2);
        const std::size_t data_size = ctx.get_argument_data_size(2);
        const std::optional<std::u16string> name = ctx.get_argument_value<std::u16string>(1);

        if (!data && data_size != 0) {
            ctx.complete(epoc::error_argument);
            return;
        }

        std::uint8_t empty_data = 0;
        common::ro_buf_stream stream(data ? data : &empty_data, data_size);
        const data_recog_result result = recognize_data_impl(stream, name.value_or(std::u16string()));

        ctx.write_data_to_descriptor_argument<data_recog_result>(0, result);
        ctx.complete(epoc::error_none);
    }

    void applist_server::recognize_data_by_file_handle(service::ipc_context &ctx) {
        session_ptr fs_target_session = ctx.sys->get_kernel_system()->get<service::session>(*(ctx.get_argument_value<std::int32_t>(1)));
        const std::uint32_t fs_file_handle = *(ctx.get_argument_value<std::uint32_t>(2));
        file *source_file = fsserv->get_file(fs_target_session->unique_id(), fs_file_handle);

        if (!source_file) {
            ctx.complete(epoc::error_argument);
            return;
        }

        const std::uint64_t current_pos = source_file->tell();
        ro_file_stream stream_read(source_file);

        const data_recog_result result = recognize_data_impl(stream_read, source_file->file_name());
        source_file->seek(current_pos, file_seek_mode::beg);

        if (result.type_.type_name_.get_length() == 0) {
            LOG_WARN(SERVICE_APPLIST, "File MIME data is not recognizable (filename: {})!", common::ucs2_to_utf8(source_file->file_name()));
            ctx.complete(epoc::error_not_supported);
            return;
        }

        ctx.write_data_to_descriptor_argument<data_recog_result>(0, result);
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_supported_data_types_phase1(service::ipc_context &ctx) {
        common::chunkyseri seri(nullptr, 0, common::SERI_MODE_MEASURE);
        serialize_mime_arrays(seri);

        ctx.complete(static_cast<int>(seri.size()));
    }

    void applist_server::get_supported_data_types_phase2(service::ipc_context &ctx) {
        std::uint8_t *data_ptr = ctx.get_descriptor_argument_ptr(0);
        std::size_t data_max_size = ctx.get_argument_max_data_size(0);

        if (!data_ptr || !data_max_size) {
            ctx.complete(epoc::error_argument);
            return;
        }

        common::chunkyseri seri(nullptr, data_max_size, common::SERI_MODE_MEASURE);
        serialize_mime_arrays(seri);

        if (seri.size() > data_max_size) {
            ctx.complete(epoc::error_overflow);
            return;
        }

        seri = common::chunkyseri(data_ptr, data_max_size, common::SERI_MODE_WRITE);
        serialize_mime_arrays(seri);

        ctx.set_descriptor_argument_length(0, static_cast<std::uint32_t>(seri.size()));
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_capability(service::ipc_context &ctx) {
        const epoc::uid app_uid = *ctx.get_argument_value<epoc::uid>(1);
        apa_app_registry *reg = get_registration(app_uid);

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        ctx.write_data_to_descriptor_argument<apa_capability>(0, reg->caps, nullptr, true);
        ctx.complete(epoc::error_none);
    }

    void applist_session::get_app_executable_name_given_app_uid(service::ipc_context &ctx) {
        std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(2);

        if (!app_uid.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        applist_server *serv = server<applist_server>();
        auto executable_map_result = serv->uids_app_to_executable.find(app_uid.value());
        if (executable_map_result != serv->uids_app_to_executable.end()) {
            ctx.write_arg(0, kernel::BRIDAGED_EXECUTABLE_NAME);
            ctx.write_arg(1, executable_map_result->second);
            pending_opaque_data_.clear();
            ctx.complete(0);
            
            return;
        }

        apa_app_registry *reg = serv->get_registration(app_uid.value());

        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        const std::u16string logical_path = reg->mandatory_info.app_path.to_std_string(nullptr);
        if (reg->caps.flags & apa_capability::non_native) {
            const std::u16string launcher = serv->native_executable_for_non_native_type(reg->non_native_application_type);
            if (launcher.empty()) {
                ctx.complete(epoc::error_not_found);
                return;
            }

            ctx.write_arg(0, launcher);
            ctx.write_arg(1, logical_path);
            pending_opaque_data_ = reg->non_native_opaque_data;
            ctx.complete(static_cast<int>(pending_opaque_data_.size()));
            return;
        }

        ctx.write_arg(0, logical_path);
        pending_opaque_data_.clear();
        ctx.complete(0);
    }

    void applist_server::register_non_native_app_type(service::ipc_context &ctx) {
        const std::optional<epoc::uid> type = ctx.get_argument_value<epoc::uid>(0);
        const std::optional<std::u16string> executable = ctx.get_argument_value<std::u16string>(1);
        if (!type || !*type || !executable || executable->empty()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        if (non_native_app_types_.find(*type) != non_native_app_types_.end()) {
            ctx.complete(epoc::error_already_exists);
            return;
        }

        non_native_app_types_.emplace(*type, *executable);
        LOG_INFO(SERVICE_APPLIST, "Registered non-native application type 0x{:08X} -> {}",
            *type, common::ucs2_to_utf8(*executable));
        ctx.complete(epoc::error_none);
    }

    void applist_server::deregister_non_native_app_type(service::ipc_context &ctx) {
        const std::optional<epoc::uid> type = ctx.get_argument_value<epoc::uid>(0);
        if (!type || !*type) {
            ctx.complete(epoc::error_argument);
            return;
        }

        const auto found = non_native_app_types_.find(*type);
        if (found == non_native_app_types_.end()) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        non_native_app_types_.erase(found);
        ctx.complete(epoc::error_none);
    }

    void applist_server::prepare_non_native_app_updates(service::ipc_context &ctx) {
        if (non_native_update_prepared_) {
            ctx.complete(epoc::error_in_use);
            return;
        }

        non_native_update_prepared_ = true;
        pending_non_native_updates_.clear();
        ctx.complete(epoc::error_none);
    }

    void applist_server::register_non_native_app(service::ipc_context &ctx) {
        if (!non_native_update_prepared_) {
            ctx.complete(epoc::error_not_ready);
            return;
        }

        const std::uint8_t *raw = ctx.get_descriptor_argument_ptr(0);
        const std::size_t raw_size = ctx.get_argument_data_size(0);
        static constexpr std::size_t NON_NATIVE_INFO_SIZE = sizeof(std::uint32_t) * 2;
        static constexpr std::size_t CHECKED_UID_SIZE = sizeof(std::uint32_t) * 4;

        if (!raw || (raw_size <= NON_NATIVE_INFO_SIZE + CHECKED_UID_SIZE)) {
            ctx.complete(epoc::error_argument);
            return;
        }

        std::uint32_t application_type = 0;
        std::int32_t drive = 0;
        std::uint32_t inner_checked_uids[4] = {};
        std::memcpy(&application_type, raw, sizeof(application_type));
        std::memcpy(&drive, raw + sizeof(application_type), sizeof(drive));
        std::memcpy(inner_checked_uids, raw + NON_NATIVE_INFO_SIZE, sizeof(inner_checked_uids));

        if (!application_type || !inner_checked_uids[2]
            || (drive < drive_a) || (drive >= drive_count)
            || (crypt::calculate_checked_uid_checksum(inner_checked_uids) != inner_checked_uids[3])) {
            ctx.complete(epoc::error_argument);
            return;
        }

        pending_non_native_update update;
        update.kind_ = pending_non_native_update::kind::register_app;
        update.application_uid_ = inner_checked_uids[2];
        update.application_type_ = application_type;
        update.drive_ = static_cast<drive_number>(drive);

        const std::uint32_t outer_uids[3] = {
            PREFIXED_NON_NATIVE_REGISTRATION_UID,
            application_type,
            update.application_uid_
        };
        const std::uint32_t outer_checksum = crypt::calculate_checked_uid_checksum(outer_uids);

        update.registration_resource_.resize(CHECKED_UID_SIZE + raw_size - NON_NATIVE_INFO_SIZE);
        std::memcpy(update.registration_resource_.data(), outer_uids, sizeof(outer_uids));
        std::memcpy(update.registration_resource_.data() + sizeof(outer_uids), &outer_checksum, sizeof(outer_checksum));
        std::memcpy(update.registration_resource_.data() + CHECKED_UID_SIZE,
            raw + NON_NATIVE_INFO_SIZE, raw_size - NON_NATIVE_INFO_SIZE);

        const std::uint8_t *localisable = ctx.get_descriptor_argument_ptr(1);
        const std::size_t localisable_size = ctx.get_argument_data_size(1);
        if (localisable && localisable_size && (localisable_size != static_cast<std::size_t>(-1))) {
            update.localisable_resource_.assign(localisable, localisable + localisable_size);
        }

        pending_non_native_updates_.push_back(std::move(update));
        ctx.complete(epoc::error_none);
    }

    void applist_server::deregister_non_native_app(service::ipc_context &ctx) {
        if (!non_native_update_prepared_) {
            ctx.complete(epoc::error_not_ready);
            return;
        }

        const std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(0);
        if (!app_uid || !*app_uid) {
            ctx.complete(epoc::error_argument);
            return;
        }

        apa_app_registry *reg = get_registration(*app_uid);
        if (!reg || !(reg->caps.flags & apa_capability::non_native)) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        pending_non_native_update update;
        update.kind_ = pending_non_native_update::kind::deregister_app;
        update.application_uid_ = *app_uid;
        update.application_type_ = reg->non_native_application_type;
        update.drive_ = reg->land_drive;
        pending_non_native_updates_.push_back(std::move(update));
        ctx.complete(epoc::error_none);
    }

    void applist_server::commit_non_native_app_updates(service::ipc_context &ctx) {
        if (!non_native_update_prepared_) {
            ctx.complete(epoc::error_not_ready);
            return;
        }

        io_system *io = sys->get_io_system();
        bool succeeded = true;
        for (const pending_non_native_update &update : pending_non_native_updates_) {
            const std::u16string base = std::u16string(1, drive_to_char16(update.drive_)) + u":" + NON_NATIVE_RESOURCE_DIRECTORY;
            const std::u16string uid_name = common::utf8_to_ucs2(fmt::format("{:08x}", update.application_uid_));
            const std::u16string registration_path = base + uid_name + u"_reg.rsc";
            const std::u16string localisable_path = base + uid_name + u"_loc.rsc";

            if (update.kind_ == pending_non_native_update::kind::deregister_app) {
                io->delete_entry(registration_path);
                io->delete_entry(localisable_path);
                continue;
            }

            if (!io->create_directories(base)) {
                succeeded = false;
                break;
            }

            symfile registration_file = io->open_file(registration_path, WRITE_MODE | BIN_MODE);
            if (!registration_file
                || (registration_file->write_file(update.registration_resource_.data(), 1,
                    static_cast<std::uint32_t>(update.registration_resource_.size())) != update.registration_resource_.size())) {
                succeeded = false;
                break;
            }
            registration_file->close();

            if (!update.localisable_resource_.empty()) {
                symfile localisable_file = io->open_file(localisable_path, WRITE_MODE | BIN_MODE);
                if (!localisable_file
                    || (localisable_file->write_file(update.localisable_resource_.data(), 1,
                        static_cast<std::uint32_t>(update.localisable_resource_.size())) != update.localisable_resource_.size())) {
                    succeeded = false;
                    break;
                }
                localisable_file->close();
            }
        }

        pending_non_native_updates_.clear();
        non_native_update_prepared_ = false;

        if (!succeeded) {
            ctx.complete(epoc::error_general);
            return;
        }

        rescan_registries(io);
        ctx.complete(epoc::error_none);
    }

    void applist_server::rollback_non_native_app_updates(service::ipc_context &ctx) {
        pending_non_native_updates_.clear();
        non_native_update_prepared_ = false;
        ctx.complete(epoc::error_none);
    }

    void applist_server::get_app_type(service::ipc_context &ctx) {
        const std::optional<epoc::uid> app_uid = ctx.get_argument_value<epoc::uid>(0);
        if (!app_uid) {
            ctx.complete(epoc::error_argument);
            return;
        }

        apa_app_registry *reg = get_registration(*app_uid);
        if (!reg) {
            ctx.complete(epoc::error_not_found);
            return;
        }

        epoc::uid type = reg->non_native_application_type;
        if (type == JAVA_MIDLET_LEGACY_TYPE_ID) {
            type = JAVA_MIDLET_APPLICATION_TYPE_UID;
        }
        ctx.write_data_to_descriptor_argument<epoc::uid>(1, type);
        ctx.complete(epoc::error_none);
    }

    applist_session::applist_session(service::typical_server *svr, kernel::uid client_ss_uid, epoc::version client_ver)
        : typical_session(svr, client_ss_uid, client_ver)
        , filter_method_(APP_FILTER_NONE) {
    }

    void applist_session::get_filtered_apps_by_flags(service::ipc_context &ctx) {
        std::optional<std::uint32_t> screen_mode = ctx.get_argument_value<std::uint32_t>(0);
        std::optional<std::uint32_t> flags_mask = ctx.get_argument_value<std::uint32_t>(1);
        std::optional<std::uint32_t> flags_value = ctx.get_argument_value<std::uint32_t>(2);
    
        if (!screen_mode.has_value() || !flags_mask.has_value() || !flags_value.has_value()) {
            ctx.complete(epoc::error_argument);
            return;
        }

        filter_method_ = APP_FILTER_BY_FLAGS;
        flags_mask_ = flags_mask.value();
        flags_value = flags_value.value();
        requested_screen_mode_ = screen_mode.value();
        current_index_ = 0;

        ctx.complete(epoc::error_none);
    }

    void applist_session::get_next_app(service::ipc_context &ctx) {
        if (filter_method_ == APP_FILTER_NONE) {
            ctx.complete(epoc::error_not_ready);
            return;
        }

        auto &registries = server<applist_server>()->regs;
        for (; current_index_ < registries.size(); current_index_++) {
            if (!registries[current_index_].supports_screen_mode(requested_screen_mode_)) {
                continue;
            }
            
            if (filter_method_ == APP_FILTER_BY_FLAGS) {
                if ((registries[current_index_].caps.flags & flags_mask_) == flags_match_value_) {
                    ctx.write_data_to_descriptor_argument<apa_app_info>(1, registries[current_index_].mandatory_info);
                    ctx.complete(epoc::error_none);

                    current_index_++;

                    return;
                }
            }
        }

        ctx.complete(epoc::error_not_found);
    }

    void applist_session::fetch(service::ipc_context *ctx) {
        const int llevel = server<applist_server>()->legacy_level();
        if (llevel == APA_LEGACY_LEVEL_OLD) {
            switch (ctx->msg->function) {
            case applist_request_oldarch_app_info:
                server<applist_server>()->get_app_info(*ctx);
                break;

            case applist_request_oldarch_start_app:
                server<applist_server>()->launch_app(*ctx);
                break;

            case applist_request_oldarch_is_program:
                server<applist_server>()->is_program(*ctx);
                break;

            case applist_request_oldarch_app_icon_by_uid_and_size:
                server<applist_server>()->get_app_icon(*ctx);
                break;

            case applist_request_oldarch_get_app_icon_sizes:
                server<applist_server>()->get_app_icon_sizes(*ctx);
                break;

            default:
                LOG_ERROR(SERVICE_APPLIST, "Unimplemented applist opcode {}", ctx->msg->function);
                ctx->complete(epoc::error_not_supported);
                break;
            }
        } else if (llevel == APA_LEGACY_LEVEL_S60V2) {
            switch (ctx->msg->function) {
            case applist_request_s60v2_app_info:
                server<applist_server>()->get_app_info(*ctx);
                break;

            case applist_request_s60v2_app_icon_by_uid_and_size:
                server<applist_server>()->get_app_icon(*ctx);
                break;

            default:
                LOG_ERROR(SERVICE_APPLIST, "Unimplemented applist opcode {}", ctx->msg->function);
                ctx->complete(epoc::error_not_supported);
                break;
            }
        } else if (llevel == APA_LEGACY_LEVEL_TRANSITION) {
            switch (ctx->msg->function) {
            case applist_request_trans_app_info:
                server<applist_server>()->get_app_info(*ctx);
                break;

            case applist_request_trans_app_capability:
                server<applist_server>()->get_capability(*ctx);
                break;

            case applist_request_trans_app_icon_filename:
                server<applist_server>()->get_app_icon_file_name(*ctx);
                break;

            case applist_request_trans_app_info_provide_by_reg_file:
                server<applist_server>()->app_info_provided_by_reg_file(*ctx);
                break;

            case applist_request_trans_app_icon_by_uid_and_size:
                server<applist_server>()->get_app_icon(*ctx);
                break;

            case applist_request_trans_start_app_without_returning_thread_id:
                server<applist_server>()->launch_app(*ctx);
                break;

            default:
                LOG_ERROR(SERVICE_APPLIST, "Unimplemented applist opcode 0x{:X}", ctx->msg->function);
                ctx->complete(epoc::error_not_supported);
                break;
            }
        } else {
            switch (ctx->msg->function) {
            case applist_request_get_default_screen_number:
                server<applist_server>()->default_screen_number(*ctx);
                break;

            case applist_request_app_language:
                server<applist_server>()->app_language(*ctx);
                break;

            case applist_request_rule_based_launching:
                server<applist_server>()->is_accepted_to_run(*ctx);
                break;

            case applist_request_app_info:
                server<applist_server>()->get_app_info(*ctx);
                break;

            case applist_request_app_capability:
                server<applist_server>()->get_capability(*ctx);
                break;

            case applist_request_app_icon_filename:
                server<applist_server>()->get_app_icon_file_name(*ctx);
                break;

            case applist_request_get_executable_name_if_non_native:
                get_native_executable_name_if_non_native(*ctx);
                break;

            case applist_request_get_opaque_data:
                get_opaque_data(*ctx);
                break;

            case applist_request_app_info_provide_by_reg_file:
                server<applist_server>()->app_info_provided_by_reg_file(*ctx);
                break;

            case applist_request_app_icon_by_uid_and_size:
                server<applist_server>()->get_app_icon(*ctx);
                break;

            case applist_request_preferred_buf_size:
                server<applist_server>()->get_preferred_buf_size(*ctx);
                break;

            case applist_request_app_for_document:
                server<applist_server>()->get_app_for_document(*ctx);
                break;

            case applist_request_app_for_document_passed_by_file_handle:
                server<applist_server>()->get_app_for_document_by_file_handle(*ctx);
                break;

            case applist_request_app_for_data_type:
                server<applist_server>()->get_app_for_data_type(*ctx);
                break;

            case applist_request_recognize_data:
                server<applist_server>()->recognize_data(*ctx);
                break;

            case applist_request_recognize_data_passed_by_file_handle:
                server<applist_server>()->recognize_data_by_file_handle(*ctx);
                break;

            case applist_request_get_executable_name_given_app_uid:
                get_app_executable_name_given_app_uid(*ctx);
                break;

            case applist_request_get_data_types_phase1:
                server<applist_server>()->get_supported_data_types_phase1(*ctx);
                break;
                
            case applist_request_get_data_types_phase2:
                server<applist_server>()->get_supported_data_types_phase2(*ctx);
                break;

            case applsit_request_init_attr_filtered_list:
                get_filtered_apps_by_flags(*ctx);
                break;

            case applist_request_get_next_app:
                get_next_app(*ctx);
                break;

            case applist_request_register_non_native_app_type:
                server<applist_server>()->register_non_native_app_type(*ctx);
                break;

            case applist_request_deregister_non_native_app_type:
                server<applist_server>()->deregister_non_native_app_type(*ctx);
                break;

            case applist_request_prepare_non_native_app_updates:
                server<applist_server>()->prepare_non_native_app_updates(*ctx);
                break;

            case applist_request_register_non_native_app:
                server<applist_server>()->register_non_native_app(*ctx);
                break;

            case applist_request_deregister_non_native_app:
                server<applist_server>()->deregister_non_native_app(*ctx);
                break;

            case applist_request_commit_non_native_application:
                server<applist_server>()->commit_non_native_app_updates(*ctx);
                break;

            case applist_request_rollback_non_native_application:
                server<applist_server>()->rollback_non_native_app_updates(*ctx);
                break;

            case applist_request_get_app_type:
                server<applist_server>()->get_app_type(*ctx);
                break;

            default:
                LOG_ERROR(SERVICE_APPLIST, "Unimplemented applist opcode 0x{:X}", ctx->msg->function);
                ctx->complete(epoc::error_not_supported);
                break;
            }
        }
    }

    bool apa_app_registry::supports_screen_mode(const int mode_num) {
        if (mode_num < 0) {
            return true;
        }

        if (view_datas.size() == 0) {
            return (mode_num == 0);
        }

        for (std::size_t i = 0; i < view_datas.size(); i++) {
            if (view_datas[i].screen_mode_ == mode_num) {
                return true;
            }
        }

        return false;
    }

    void apa_app_registry::get_launch_parameter(std::u16string &native_executable_path, epoc::apa::command_line &args) {
        const std::u16string app_path = mandatory_info.app_path.to_std_string(nullptr);

        if (caps.flags & apa_capability::built_as_dll) {
            native_executable_path = APA_APP_RUNNER;
        } else {
            native_executable_path = mandatory_info.app_path.to_std_string(nullptr);
        }

        // Native apps historically receive just the filename as the document.
        // Non-native Java MIDlets are launched through StubMIDP2RecogExe, which
        // treats DocumentName / ExecutableName as the suite/JAD to open — pass
        // the full logical path so a host-injected registration can start.
        if (caps.flags & apa_capability::non_native) {
            args.document_name_ = app_path;
        } else {
            args.document_name_ = eka2l1::replace_extension(eka2l1::filename(app_path), u"");
        }
        args.executable_path_ = app_path;
        args.default_screen_number_ = default_screen_number;
    }

    static constexpr std::uint8_t ENVIRONMENT_SLOT_MAIN = 1;

    bool applist_server::launch_app(const std::u16string &exe_path, const std::u16string &cmd, kernel::uid *thread_id,
                                    kernel::process *requester, const epoc::uid known_uid, std::function<void(kernel::process*)> app_exit_callback,
                                    const bool pass_command_line_in_env_slot, const std::vector<std::uint8_t> *guest_env_slot) {
        static constexpr std::size_t MINIMAL_LAUNCH_STACK_SIZE = 0x10000;
        static constexpr std::size_t MINIMAL_LAUNCH_STACK_SIZE_S3 = 0x80000;

        // Some S^3 and other OS apps has very low stack size for some reason. So we replace the stack size if it's too low
        // Note that on the actual source code, app is launched with possible stack override
        // For reference: see variable KMinApplicationStackSize and this line at file:
        // https://github.com/SymbianSource/oss.FCL.sf.mw.appsupport/blob/master/appfw/apparchitecture/apgrfx/apgstart.cpp#L164
        // Value of S^3 is based on the proud Doodle Farm (for some reason using 0.3mb of stack in a single function when in EXE it ask for 0.1mb)
        process_ptr pr = kern->spawn_new_process(exe_path, (legacy_level() < APA_LEGACY_LEVEL_MORDEN) ? cmd : u"",
            0, (kern->get_epoc_version() >= epocver::epoc95) ? MINIMAL_LAUNCH_STACK_SIZE_S3 : MINIMAL_LAUNCH_STACK_SIZE);

        if (!pr) {
            return false;
        }

        // Only non-native apps need this: their runtime reads the MIDlet's
        // opaque data out of the command line. Native apps must not get one,
        // because the serialised CApaCommandLine carries parent_process_id_ = 0
        // and Avkon then opens process 0 and leaves during startup.
        if (pass_command_line_in_env_slot && (legacy_level() >= APA_LEGACY_LEVEL_MORDEN)) {
            if (guest_env_slot && !guest_env_slot->empty()) {
                pr->set_arg_slot(ENVIRONMENT_SLOT_MAIN,
                    const_cast<std::uint8_t *>(guest_env_slot->data()), guest_env_slot->size());
            } else if (!cmd.empty()) {
                pr->set_arg_slot(ENVIRONMENT_SLOT_MAIN,
                    reinterpret_cast<std::uint8_t *>(const_cast<char16_t *>(cmd.data())),
                    cmd.size() * sizeof(char16_t));
            }
        }

        if (thread_id)
            *thread_id = pr->get_primary_thread()->unique_id();

        if (requester) {
            requester->add_child_process(pr);
        }

        if (app_exit_callback) {
            pr->logon([app_exit_callback, this](kernel::process *pr) {
                auto child_processes = pr->get_child_processes();

                // Handle case where a subsequence UI process is spawned
                if (!child_processes.empty()) {
                    bool deferred_exit = false;

                    for (auto &child : child_processes) {
                        if (child->get_exit_type() == kernel::entity_exit_type::pending) {
                            auto pid = child->get_uid();

                            if (get_registration(pid)) {
                                child->logon([app_exit_callback](kernel::process *pr) {
                                    app_exit_callback(pr);
                                });

                                deferred_exit = true;
                            }
                        }
                    }

                    if (!deferred_exit) {
                        app_exit_callback(pr);
                    }
                } else {
                    app_exit_callback(pr);
                }
            });
        }

        if ((kern->get_epoc_version() < epocver::eka2) && known_uid) {
            kernel::process_uid_type current_uid_type = pr->get_uid_type();
            std::get<2>(current_uid_type) = known_uid;

            pr->set_uid_type(current_uid_type);
        }

        // Add it into our app running list
        return pr->run();
    }

    bool applist_server::launch_app(apa_app_registry &registry, epoc::apa::command_line &parameter, kernel::uid *thread_id,
                                    std::function<void(kernel::process*)> app_exit_callback) {
        // OPL game check
        io_system *io = sys->get_io_system();
        symfile app_file = io->open_file(registry.mandatory_info.app_path.to_std_string(nullptr), READ_MODE | BIN_MODE);

        if (app_file != nullptr) {
            static constexpr std::uint32_t OPL_APP_UID2 = 0x100055C1;
            static constexpr std::uint32_t OPL_LAUNCHER_UID = 0x10005D2E;

            std::uint32_t uid = 0;
            if (app_file->read_file(4, &uid, 1, 4) == 4) {
                if (uid == OPL_APP_UID2) {
                    std::u16string processed_app_path = registry.mandatory_info.app_path.to_std_string(nullptr);
                    if (processed_app_path.find_first_of(' ') != std::string::npos) {
                        processed_app_path.insert(processed_app_path.begin(), u'\"');
                        processed_app_path += u'\"';
                    }

                    // Put our app path in tail end
                    epoc::apa::command_line new_parameter = parameter;
                    new_parameter.tail_end_.resize((processed_app_path.length() + 1) * 2);

                    // OPL command: run no IPC
                    static constexpr std::uint8_t OPL_COMMAND_RUN_NO_IPC = 82;

                    new_parameter.tail_end_[0] = 0x00;
                    new_parameter.tail_end_[1] = OPL_COMMAND_RUN_NO_IPC;

                    std::memcpy(new_parameter.tail_end_.data() + 2, processed_app_path.data(), new_parameter.tail_end_.length());

                    apa_app_registry *launcher_reg = get_registration(OPL_LAUNCHER_UID);
                    if (!launcher_reg) {
                        LOG_ERROR(SERVICE_APPLIST, "OPL is not installed! Please install it!");
                        return false;
                    }

                    // Launch the OPL launcher
                    return launch_app(*launcher_reg, new_parameter, thread_id, app_exit_callback);
                }
            }
        }

        std::u16string executable_to_run;
        registry.get_launch_parameter(executable_to_run, parameter);

        if (registry.caps.flags & apa_capability::non_native) {
            executable_to_run = native_executable_for_non_native_type(registry.non_native_application_type);
            if (executable_to_run.empty()) {
                LOG_ERROR(SERVICE_APPLIST, "Unable to launch non-native app 0x{:08X}: no runtime for type 0x{:08X}",
                    registry.mandatory_info.uid, registry.non_native_application_type);
                return false;
            }

            parameter.opaque_data_.assign(
                reinterpret_cast<const char *>(registry.non_native_opaque_data.data()),
                registry.non_native_opaque_data.size());
        }

        const bool is_non_native = (registry.caps.flags & apa_capability::non_native) != 0;
        std::vector<std::uint8_t> guest_slot;
        std::u16string apacmddat;
        if (is_non_native && (legacy_level() >= APA_LEGACY_LEVEL_MORDEN)) {
            guest_slot = parameter.to_guest_env_slot();
        } else {
            apacmddat = parameter.to_string(legacy_level() < APA_LEGACY_LEVEL_MORDEN);
        }

        return launch_app(executable_to_run, apacmddat, thread_id, nullptr, registry.mandatory_info.uid,
            app_exit_callback, is_non_native, guest_slot.empty() ? nullptr : &guest_slot);
    }

    std::optional<apa_app_masked_icon_bitmap> applist_server::get_icon(apa_app_registry &registry, const std::int8_t index) {
        epoc::bitwise_bitmap *real_bmp = nullptr;
        epoc::bitwise_bitmap *real_mask_bmp = nullptr;

        if (index * 2 >= registry.app_icons.size()) {
            return std::nullopt;
        }

        if (registry.app_icons[index * 2].bmp_)
            real_bmp = registry.app_icons[index * 2].bmp_->bitmap_;
        else
            real_bmp = eka2l1::ptr<epoc::bitwise_bitmap>(registry.app_icons[index * 2].bmp_rom_addr_).get(sys->get_memory_system());

        if (!real_bmp) {
            return std::nullopt;
        }

        if (registry.app_icons[index * 2 + 1].bmp_)
            real_mask_bmp = registry.app_icons[index * 2 + 1].bmp_->bitmap_;
        else
            real_mask_bmp = eka2l1::ptr<epoc::bitwise_bitmap>(registry.app_icons[index * 2 + 1].bmp_rom_addr_).get(sys->get_memory_system());

        return std::make_optional(std::make_pair(real_bmp, real_mask_bmp));
    }

    void applist_server::add_app_uid_to_host_launch_name(const epoc::uid app_uid, const std::u16string &host_launch_name) {
        uids_app_to_executable.emplace(app_uid, host_launch_name);
    }
}
