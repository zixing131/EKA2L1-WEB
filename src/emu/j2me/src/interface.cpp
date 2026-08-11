/*
 * Copyright (c) 2022 EKA2L1 Team.
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

#include <j2me/interface.h>
#include <j2me/kmidrun.h>
#include <j2me/applist.h>

#include <system/epoc.h>
#include <common/algorithm.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <common/path.h>
#include <kernel/kernel.h>
#include <vfs/vfs.h>

#include <cctype>

namespace eka2l1::j2me {
    bool launch(system *sys, const std::uint32_t app_id, std::function<void(kernel::process*)> exit_cb) {
        app_list *applist = sys->get_j2me_applist();
        std::optional<app_entry> entry = applist->get_entry(app_id);
        if (!entry.has_value()) {
            return false;
        }
        const epocver sysver = sys->get_symbian_version_use();
        if (sysver <= epocver::epoc6) {
            launch_through_kmidrun(sys, entry.value(), exit_cb);
            return true;
        }

        return false;
    }

    /**
     * 把一个来宾文件从 src 复制到 dst（不存在时才复制）。
     * 返回 false 仅当源存在但复制失败。
     */
    static bool copy_guest_file_if_missing(io_system *io, const std::u16string &src, const std::u16string &dst) {
        if (io->exist(dst) || !io->exist(src)) {
            return true;
        }
        symfile src_file = io->open_file(src, READ_MODE | BIN_MODE);
        if (!src_file) {
            return false;
        }
        std::vector<char> buf(src_file->size());
        src_file->read_file(buf.data(), 1, static_cast<std::uint32_t>(buf.size()));
        src_file->close();

        symfile dst_file = io->open_file(dst, WRITE_MODE | BIN_MODE);
        if (!dst_file) {
            return false;
        }
        const bool ok = (dst_file->write_file(buf.data(), 1, static_cast<std::uint32_t>(buf.size())) == buf.size());
        dst_file->close();
        return ok;
    }

    bool prepare_midp2_environment(system *sys) {
        io_system *io = sys->get_io_system();

        // 出厂 C: 目录树: 缺目录会让策略/AMS 组件收到 KErrPathNotFound 而非
        // KErrNotFound, 其回退逻辑不识别前者, 直接 Leave 崩溃。
        static const char16_t *REQUIRED_DIRS[] = {
            u"C:\\system\\data\\midp2\\security\\policy\\",
            u"C:\\system\\data\\midp2\\preinstall\\",
            u"C:\\private\\101F9F6C\\security\\",
        };
        bool ok = true;
        for (const char16_t *dir : REQUIRED_DIRS) {
            if (!io->exist(dir) && !io->create_directories(dir)) {
                LOG_ERROR(J2ME, "Can't create MIDP2 directory {}", common::ucs2_to_utf8(dir));
                ok = false;
            }
        }

        // 安全策略文件: 真机首启从 ROM 预置到 C:。
        static constexpr char16_t POLICY_DIR_ROM[] = u"Z:\\system\\data\\midp2\\security\\policy\\";
        static constexpr char16_t POLICY_DIR_RAM[] = u"C:\\system\\data\\midp2\\security\\policy\\";
        static const char16_t *POLICY_FILES[] = { u"midp2_rp.xpf", u"jtwi_r1.xpf" };
        for (const char16_t *file : POLICY_FILES) {
            if (!copy_guest_file_if_missing(io, std::u16string(POLICY_DIR_ROM) + file,
                    std::u16string(POLICY_DIR_RAM) + file)) {
                LOG_ERROR(J2ME, "Can't provision MIDP2 policy file {}", common::ucs2_to_utf8(file));
                ok = false;
            }
        }
        return ok;
    }

    static std::string make_safe_preinstall_name(const std::string &path) {
        std::string name = eka2l1::replace_extension(eka2l1::filename(path), "");
        std::string safe;
        safe.reserve(name.size());
        for (const unsigned char c : name) {
            if (std::isalnum(c) || (c == '-') || (c == '_')) {
                safe.push_back(static_cast<char>(std::tolower(c)));
            } else {
                safe.push_back('_');
            }
        }

        if (safe.empty()) {
            safe = "midlet";
        }
        return safe;
    }

    static install_error install_for_midp2(system *sys, const std::string &path, app_entry &entry_info,
        std::function<void(kernel::process*)> install_exit_cb) {
        FILE *jar_file = common::open_c_file(path, "rb");
        if (!jar_file) {
            return INSTALL_ERROR_JAR_NOT_FOUND;
        }

        std::string jad_content;
        int midp_version = 0;
        const install_error parse_result = get_app_entry(jar_file, entry_info, jad_content, midp_version);
        fclose(jar_file);
        if (parse_result != INSTALL_ERROR_JAR_SUCCESS) {
            return parse_result;
        }

        io_system *io = sys->get_io_system();
        static constexpr char16_t PREINSTALL_DIRECTORY[] = u"E:\\system\\data\\midp2\\preinstall\\";
        static constexpr char16_t SILENT_INSTALLER[] = u"Z:\\sys\\bin\\midp2silentmidletinstall.exe";
        static constexpr char16_t SYSTEM_AMS_CORE[] = u"Z:\\sys\\bin\\systemamscore.exe";
        if (!io->exist(SILENT_INSTALLER) || !io->exist(SYSTEM_AMS_CORE)) {
            return INSTALL_ERROR_JAVA_RUNTIME_NOT_FOUND;
        }

        if (!io->create_directories(PREINSTALL_DIRECTORY)) {
            return INSTALL_ERROR_JAR_CANT_COPY;
        }

        prepare_midp2_environment(sys);

        const std::string base_name = make_safe_preinstall_name(path);
        const std::u16string guest_jar_path = std::u16string(PREINSTALL_DIRECTORY)
            + common::utf8_to_ucs2(base_name) + u".jar";
        std::u16string guest_jad_path = guest_jar_path;
        guest_jad_path.back() = u'd';

        const std::optional<std::u16string> raw_jar_path = io->get_raw_path(guest_jar_path);
        if (!raw_jar_path
            || !common::copy_file(path, common::ucs2_to_utf8(*raw_jar_path), true)) {
            return INSTALL_ERROR_JAR_CANT_COPY;
        }

        const std::string fake_url = "MIDlet-Jar-URL: https://12z1.com/jar/fake.jar";
        const std::string local_url = "MIDlet-Jar-URL: " + base_name + ".jar";
        const std::size_t fake_url_pos = jad_content.find(fake_url);
        if (fake_url_pos != std::string::npos) {
            jad_content.replace(fake_url_pos, fake_url.size(), local_url);
        }

        symfile jad_file = io->open_file(guest_jad_path, WRITE_MODE | BIN_MODE);
        if (!jad_file
            || (jad_file->write_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size())) != jad_content.size())) {
            io->delete_entry(guest_jar_path);
            return INSTALL_ERROR_JAR_CANT_COPY;
        }
        jad_file->close();

        kernel_system *kern = sys->get_kernel_system();

        // MIDP2SilentMIDletInstall deliberately checks User::CreatorSecureId()
        // and rejects callers other than SystemAMSCore (SID 0x10203636) with
        // KErrPermissionDenied. Spawn the ROM AMS core as the creator and let
        // it actually run: the installer talks to the live AMS (lifecycle /
        // security / registry threads) to perform the real installation.
        kernel::process *creator = kern->spawn_new_process(SYSTEM_AMS_CORE, u"");
        if (!creator) {
            io->delete_entry(guest_jar_path);
            io->delete_entry(guest_jad_path);
            return INSTALL_ERROR_JAVA_INSTALLER_CANT_START;
        }

        if (!creator->run()) {
            LOG_WARN(J2ME, "SystemAMSCore could not be started; the silent installer may stall");
        }

        kernel::process *installer = kern->spawn_new_process(SILENT_INSTALLER, u"");
        if (!installer) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPInstall", 0);
            io->delete_entry(guest_jar_path);
            io->delete_entry(guest_jad_path);
            return INSTALL_ERROR_JAVA_INSTALLER_CANT_START;
        }

        creator->add_child_process(installer);
        installer->logon([creator, install_exit_cb = std::move(install_exit_cb)](kernel::process *finished) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPInstall", 0);
            if (install_exit_cb) {
                install_exit_cb(finished);
            }
        });

        if (!installer->run()) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPInstall", 0);
            io->delete_entry(guest_jar_path);
            io->delete_entry(guest_jad_path);
            return INSTALL_ERROR_JAVA_INSTALLER_CANT_START;
        }

        LOG_INFO(J2ME, "MIDP {} JAR staged at {} and the guest Java installer was started",
            midp_version, common::ucs2_to_utf8(guest_jar_path));
        return INSTALL_ERROR_JAR_SUCCESS;
    }

    install_error install(system *sys, const std::string &path, app_entry &entry_info,
        std::function<void(kernel::process*)> install_exit_cb) {
        app_list *applist = sys->get_j2me_applist();
        const epocver sysver = sys->get_symbian_version_use();
        if (sysver <= epocver::epoc6) {
            return install_for_kmidrun(sys, applist, path, entry_info);
        }
        return install_for_midp2(sys, path, entry_info, std::move(install_exit_cb));
    }

    bool uninstall(system *sys, const std::uint32_t app_id) {
        app_list *applist = sys->get_j2me_applist();
        std::optional<app_entry> entry = applist->get_entry(app_id);
        if (!entry.has_value()) {
            return false;
        }
        const epocver sysver = sys->get_symbian_version_use();
        if (sysver <= epocver::epoc6) {
            uninstall_for_kmidrun(sys, applist, entry.value());
            return true;
        }
        return false;
    }
}
