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
#include <config/config.h>
#include <kernel/kernel.h>
#include <vfs/vfs.h>

#include <cctype>
#include <miniz.h>
#include <memory>

namespace eka2l1::j2me {
    // Forward declarations for the S60v3 MIDP2 launch / uninstall paths
    // (defined later in this file).
    bool launch_through_midp2(system *sys, const app_entry &entry, std::function<void(kernel::process*)> exit_cb);
    void uninstall_for_midp2(system *sys, app_list *applist, const app_entry &entry);

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

        return launch_through_midp2(sys, entry.value(), exit_cb);
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

    // Build a per-MIDlet storage path on drive E: so each MIDlet keeps its own
    // JAR/JAD copy independent of the shared short-name preinstall staging.
    static std::u16string build_midp2_storage_dir(const app_entry &entry) {
        const std::string safe_vendor = make_safe_preinstall_name(entry.author_);
        const std::string safe_name = make_safe_preinstall_name(entry.name_);
        const std::string safe_ver = make_safe_preinstall_name(entry.version_);
        return u"E:\\system\\midp\\" + common::utf8_to_ucs2(safe_vendor)
            + u"\\" + common::utf8_to_ucs2(safe_name)
            + u"\\" + common::utf8_to_ucs2(safe_ver) + u"\\";
    }

    // Copy JAR/JAD from the per-MIDlet storage to the shared preinstall dir
    // (short name required by midp2downloader's TBuf<16> scheme parser).
    static bool restage_to_preinstall(io_system *io, const std::u16string &storage_dir,
        const std::u16string &storage_name) {
        static constexpr char16_t PREINSTALL_DIR[] = u"E:\\system\\data\\midp2\\preinstall\\";
        if (!io->create_directories(PREINSTALL_DIR)) {
            return false;
        }

        const std::u16string src_jar = storage_dir + storage_name + u".jar";
        const std::u16string src_jad = storage_dir + storage_name + u".jad";
        const std::u16string dst_jar = std::u16string(PREINSTALL_DIR) + u"m.jar";
        const std::u16string dst_jad = std::u16string(PREINSTALL_DIR) + u"m.jad";

        io->delete_entry(dst_jar);
        io->delete_entry(dst_jad);

        auto copy_guest = [&](const std::u16string &src, const std::u16string &dst) -> bool {
            symfile sf = io->open_file(src, READ_MODE | BIN_MODE);
            if (!sf) return false;
            std::vector<char> buf(sf->size());
            sf->read_file(buf.data(), 1, static_cast<std::uint32_t>(buf.size()));
            sf->close();
            symfile df = io->open_file(dst, WRITE_MODE | BIN_MODE);
            if (!df) return false;
            bool ok = (df->write_file(buf.data(), 1, static_cast<std::uint32_t>(buf.size())) == buf.size());
            df->close();
            return ok;
        };

        if (!copy_guest(src_jar, dst_jar)) return false;
        copy_guest(src_jad, dst_jad); // JAD optional
        return true;
    }

    bool launch_through_midp2(system *sys, const app_entry &entry, std::function<void(kernel::process*)> exit_cb) {
        io_system *io = sys->get_io_system();
        const std::u16string storage_dir = build_midp2_storage_dir(entry);
        const std::string safe_name = make_safe_preinstall_name(entry.name_);
        const std::u16string storage_name = common::utf8_to_ucs2(safe_name);

        const std::u16string storage_jar = storage_dir + storage_name + u".jar";
        if (!io->exist(storage_jar)) {
            LOG_ERROR(J2ME, "MIDP2 launch: JAR not found at {}", common::ucs2_to_utf8(storage_jar));
            return false;
        }

        prepare_midp2_environment(sys);
        if (!restage_to_preinstall(io, storage_dir, storage_name)) {
            return false;
        }

        // Try known S60v3 standalone MIDlet launcher executables.
        // NOTE: StubMIDP2RecogExe.exe is deliberately excluded — it is a file
        // recognizer invoked by AppArc with opaque data, not a standalone launcher.
        // Spawning it directly with a JAD argument does nothing useful.
        static const char16_t *LAUNCHER_CANDIDATES[] = {
            u"Z:\\sys\\bin\\midp2midletlauncher.exe",
            u"Z:\\sys\\bin\\midletlauncher.exe"
        };

        const char16_t *launcher_path = nullptr;
        for (const char16_t *candidate : LAUNCHER_CANDIDATES) {
            if (io->exist(candidate)) {
                launcher_path = candidate;
                break;
            }
        }

        if (!launcher_path) {
            LOG_ERROR(J2ME, "MIDP2 launch: no Java MIDlet launcher found in ROM");
            return false;
        }

        static constexpr char16_t PREINSTALL_DIR[] = u"E:\\system\\data\\midp2\\preinstall\\";
        const std::u16string jad_arg = std::u16string(PREINSTALL_DIR) + u"m.jad";

        kernel_system *kern = sys->get_kernel_system();

        // SystemAMSCore must be running for the launcher to resolve the Java
        // runtime services (lifecycle, security, registry).
        kernel::process *ams = kern->spawn_new_process(u"Z:\\sys\\bin\\systemamscore.exe", u"");
        if (ams) {
            ams->run();
        }

        kernel::process *pr = kern->spawn_new_process(launcher_path, jad_arg);
        if (!pr) {
            LOG_ERROR(J2ME, "Can't spawn MIDP2 launcher {}", common::ucs2_to_utf8(launcher_path));
            if (ams) ams->kill(kernel::entity_exit_type::kill, u"HostMIDPLaunch", 0);
            return false;
        }

        pr->logon([ams, exit_cb = std::move(exit_cb)](kernel::process *finished) {
            if (ams) ams->kill(kernel::entity_exit_type::kill, u"HostMIDPLaunch", 0);
            if (exit_cb) exit_cb(finished);
        });

        if (!pr->run()) {
            LOG_ERROR(J2ME, "Can't run MIDP2 launcher");
            if (ams) ams->kill(kernel::entity_exit_type::kill, u"HostMIDPLaunch", 0);
            return false;
        }

        LOG_INFO(J2ME, "MIDP2 launcher started: {} (jad={})", common::ucs2_to_utf8(launcher_path),
            common::ucs2_to_utf8(jad_arg));
        return true;
    }

    // Re-run the ROM silent installer to register an already-installed MIDlet
    // in AppArc. Needed because AppArc registrations are in-memory and lost on
    // emulator restart; the per-MIDlet JAR/JAD persist on E: and can be
    // re-staged. Returns true if the installer was spawned (exit_cb fires async).
    bool reregister_midlet(system *sys, const std::uint32_t app_id,
        std::function<void(kernel::process*)> exit_cb) {
        app_list *applist = sys->get_j2me_applist();
        if (!applist) return false;

        std::optional<app_entry> entry_opt = applist->get_entry(app_id);
        if (!entry_opt.has_value()) {
            LOG_ERROR(J2ME, "reregister: app_id {} not found", app_id);
            return false;
        }
        app_entry &entry = entry_opt.value();

        io_system *io = sys->get_io_system();
        static constexpr char16_t SILENT_INSTALLER[] = u"Z:\\sys\\bin\\midp2silentmidletinstall.exe";
        static constexpr char16_t SYSTEM_AMS_CORE[] = u"Z:\\sys\\bin\\systemamscore.exe";
        if (!io->exist(SILENT_INSTALLER) || !io->exist(SYSTEM_AMS_CORE)) {
            LOG_ERROR(J2ME, "reregister: ROM lacks silent installer or AMS core");
            return false;
        }

        const std::u16string storage_dir = build_midp2_storage_dir(entry);
        const std::string safe_name = make_safe_preinstall_name(entry.name_);
        const std::u16string storage_name = common::utf8_to_ucs2(safe_name);
        const std::u16string storage_jar = storage_dir + storage_name + u".jar";

        if (!io->exist(storage_jar)) {
            LOG_ERROR(J2ME, "reregister: stored JAR not found at {}", common::ucs2_to_utf8(storage_jar));
            return false;
        }

        prepare_midp2_environment(sys);

        if (!restage_to_preinstall(io, storage_dir, storage_name)) {
            LOG_ERROR(J2ME, "reregister: failed to restage JAR to preinstall dir");
            return false;
        }

        // Fix the MIDlet-Jar-URL in the preinstall JAD (midp2downloader reads it).
        static constexpr char16_t PREINSTALL_DIR[] = u"E:\\system\\data\\midp2\\preinstall\\";
        const std::u16string guest_jad_path = std::u16string(PREINSTALL_DIR) + u"m.jad";
        {
            symfile jad_file = io->open_file(guest_jad_path, READ_MODE | BIN_MODE);
            if (jad_file) {
                std::string jad_content;
                jad_content.resize(jad_file->size());
                jad_file->read_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size()));
                jad_file->close();

                const std::string fake_url = "MIDlet-Jar-URL: https://12z1.com/jar/fake.jar";
                const std::string local_url = "MIDlet-Jar-URL: m.jar";
                const std::size_t pos = jad_content.find(fake_url);
                if (pos != std::string::npos) {
                    jad_content.replace(pos, fake_url.size(), local_url);
                }

                symfile out = io->open_file(guest_jad_path, WRITE_MODE | BIN_MODE);
                if (out) {
                    out->write_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size()));
                    out->close();
                }
            }
        }

        kernel_system *kern = sys->get_kernel_system();

        kernel::process *creator = kern->spawn_new_process(SYSTEM_AMS_CORE, u"");
        if (!creator) {
            LOG_ERROR(J2ME, "reregister: can't spawn SystemAMSCore");
            return false;
        }
        creator->run();

        kernel::process *installer = kern->spawn_new_process(SILENT_INSTALLER, guest_jad_path);
        if (!installer) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPReregister", 0);
            LOG_ERROR(J2ME, "reregister: can't spawn silent installer");
            return false;
        }

        creator->add_child_process(installer);
        installer->logon([creator, exit_cb = std::move(exit_cb)](kernel::process *finished) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPReregister", 0);
            if (exit_cb) exit_cb(finished);
        });

        if (!installer->run()) {
            creator->kill(kernel::entity_exit_type::kill, u"HostMIDPReregister", 0);
            LOG_ERROR(J2ME, "reregister: can't run silent installer");
            return false;
        }

        LOG_INFO(J2ME, "MIDlet '{}' reregister: JAR restaged and silent installer started",
            entry.name_);
        return true;
    }

    void uninstall_for_midp2(system *sys, app_list *applist, const app_entry &entry) {
        applist->remove_entry(entry.id_);

        io_system *io = sys->get_io_system();
        const std::u16string storage_dir = build_midp2_storage_dir(entry);
        const std::string safe_name = make_safe_preinstall_name(entry.name_);
        const std::u16string storage_name = common::utf8_to_ucs2(safe_name);

        io->delete_entry(storage_dir + storage_name + u".jar");
        io->delete_entry(storage_dir + storage_name + u".jad");

        if (!entry.icon_path_.empty()) {
            const std::string icon_real_path = eka2l1::add_path(sys->get_config()->storage, entry.icon_path_);
            common::remove(icon_real_path);
        }
    }

    static install_error install_for_midp2(system *sys, app_list *applist, const std::string &path,
        app_entry &entry_info, std::function<void(kernel::process*)> install_exit_cb) {
        FILE *jar_file = common::open_c_file(path, "rb");
        if (!jar_file) {
            return INSTALL_ERROR_JAR_NOT_FOUND;
        }

        std::string jad_content;
        int midp_version = 0;
        const install_error parse_result = get_app_entry(jar_file, entry_info, jad_content, midp_version);
        if (parse_result != INSTALL_ERROR_JAR_SUCCESS) {
            fclose(jar_file);
            return parse_result;
        }

        // Extract the MIDlet icon from the JAR to host storage (same logic as
        // install_for_kmidrun). Without this, icon_path_ is empty and the web
        // frontend can't render an icon for the MIDlet.
        {
            fseek(jar_file, 0, SEEK_SET);
            std::string real_icon_path;
            if (extract_icon_to_store(jar_file, *sys->get_config(), entry_info, real_icon_path) == INSTALL_ERROR_JAR_SUCCESS) {
                entry_info.icon_path_ = real_icon_path;
            }
        }
        fclose(jar_file);

        io_system *io = sys->get_io_system();
        static constexpr char16_t SILENT_INSTALLER[] = u"Z:\\sys\\bin\\midp2silentmidletinstall.exe";
        static constexpr char16_t SYSTEM_AMS_CORE[] = u"Z:\\sys\\bin\\systemamscore.exe";

        // --- Host-side registration (always runs, before any guest install) ---
        // Copy the JAR/JAD to a per-MIDlet storage directory so the MIDlet
        // survives across emulator restarts and can be restaged on launch.
        {
            const std::u16string storage_dir = build_midp2_storage_dir(entry_info);
            const std::string safe_name = make_safe_preinstall_name(entry_info.name_);
            const std::u16string storage_name = common::utf8_to_ucs2(safe_name);
            if (io->create_directories(storage_dir)) {
                const std::optional<std::u16string> raw_storage_jar = io->get_raw_path(storage_dir + storage_name + u".jar");
                if (raw_storage_jar) {
                    if (!common::copy_file(path, common::ucs2_to_utf8(*raw_storage_jar), true)) {
                        LOG_WARN(J2ME, "Failed to copy JAR to per-MIDlet storage: {} -> {}",
                            path, common::ucs2_to_utf8(*raw_storage_jar));
                    } else {
                        LOG_INFO(J2ME, "JAR copied to per-MIDlet storage: {}", common::ucs2_to_utf8(*raw_storage_jar));
                    }
                } else {
                    LOG_WARN(J2ME, "Can't resolve host path for per-MIDlet storage JAR at {}", common::ucs2_to_utf8(storage_dir));
                }
                symfile storage_jad = io->open_file(storage_dir + storage_name + u".jad", WRITE_MODE | BIN_MODE);
                if (storage_jad) {
                    storage_jad->write_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size()));
                    storage_jad->close();
                }
            } else {
                LOG_WARN(J2ME, "Can't create MIDP2 per-MIDlet storage dir {}", common::ucs2_to_utf8(storage_dir));
            }
        }

        // Register the MIDlet in the host-side j2me app list so it always
        // shows up in the UI, even if the ROM lacks a Java runtime.
        if (applist) {
            entry_info.original_title_ = entry_info.title_;
            const std::uint32_t added_id = applist->add_entry(entry_info, true);
            if (added_id == static_cast<std::uint32_t>(-1)) {
                LOG_WARN(J2ME, "Failed to add MIDlet to the j2me app list database");
            } else {
                entry_info.id_ = added_id;
            }
        }

        // If the ROM does not ship a MIDP2 silent installer / AMS core, the
        // MIDlet is still registered in the host list. Complete the install
        // synchronously (no guest process to wait for) and return success —
        // the user sees the MIDlet and can attempt to launch it later once a
        // Java runtime is available.
        if (!io->exist(SILENT_INSTALLER) || !io->exist(SYSTEM_AMS_CORE)) {
            LOG_WARN(J2ME,
                "ROM does not ship MIDP2 silent installer ({} / {}); MIDlet "
                "registered in host list but launching requires a Java-capable ROM",
                common::ucs2_to_utf8(SILENT_INSTALLER),
                common::ucs2_to_utf8(SYSTEM_AMS_CORE));
            if (install_exit_cb) {
                install_exit_cb(nullptr);
            }
            return INSTALL_ERROR_JAR_SUCCESS;
        }

        static constexpr char16_t PREINSTALL_DIRECTORY[] = u"E:\\system\\data\\midp2\\preinstall\\";

        if (!io->create_directories(PREINSTALL_DIRECTORY)) {
            return INSTALL_ERROR_JAR_CANT_COPY;
        }

        prepare_midp2_environment(sys);

        // midp2downloader copies the URI scheme into a TBuf<16>. A long bare
        // relative name is misread as the scheme and rejected (KErrArgument).
        // Keep the preinstall basename short so a relative MIDlet-Jar-URL stays
        // within that limit when the downloader treats it as a local file name.
        static constexpr char16_t SHORT_BASENAME[] = u"m";
        const std::u16string guest_jar_path = std::u16string(PREINSTALL_DIRECTORY)
            + SHORT_BASENAME + u".jar";
        std::u16string guest_jad_path = guest_jar_path;
        guest_jad_path.back() = u'd';

        // Remove any previous staging leftovers so a rename cannot collide.
        io->delete_entry(guest_jar_path);
        io->delete_entry(guest_jad_path);

        const std::optional<std::u16string> raw_jar_path = io->get_raw_path(guest_jar_path);
        if (!raw_jar_path
            || !common::copy_file(path, common::ucs2_to_utf8(*raw_jar_path), true)) {
            return INSTALL_ERROR_JAR_CANT_COPY;
        }

        const std::string fake_url = "MIDlet-Jar-URL: https://12z1.com/jar/fake.jar";
        const std::string local_url = "MIDlet-Jar-URL: m.jar";
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

        // Pass the staged JAD path as the command-line argument. The silent
        // installer reads this to know which MIDlet to install; without it the
        // process boots but has nothing to act on and stalls in its async
        // scheduler waiting for a download/trigger that never arrives.
        kernel::process *installer = kern->spawn_new_process(SILENT_INSTALLER, guest_jad_path);
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
        return install_for_midp2(sys, applist, path, entry_info, std::move(install_exit_cb));
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
        uninstall_for_midp2(sys, applist, entry.value());
        return true;
    }

    bool check_launch_capability(system *sys) {
        // A ROM can launch MIDlets via either:
        //   1. A standalone launcher exe (midp2midletlauncher.exe / midletlauncher.exe)
        //   2. The AppArc non-native path: SystemAMSCore + midp2runtimev2.dll,
        //      where the MIDlet is registered in AppArc and launched through
        //      StubMIDP2RecogExe.exe with the proper opaque data payload.
        //
        // StubMIDP2RecogExe.exe is a file recognizer, NOT a launcher — it is
        // invoked by AppArc with opaque data, not spawned directly. Counting it
        // as a launcher candidate causes a false-positive "available" result.
        io_system *io = sys->get_io_system();

        static const char16_t *STANDALONE_LAUNCHERS[] = {
            u"Z:\\sys\\bin\\midp2midletlauncher.exe",
            u"Z:\\sys\\bin\\midletlauncher.exe"
        };

        for (const char16_t *candidate : STANDALONE_LAUNCHERS) {
            if (io->exist(candidate)) {
                return true;
            }
        }

        // AppArc path: requires the AMS core and the MIDP2 runtime DLL.
        if (io->exist(u"Z:\\sys\\bin\\systemamscore.exe")
            && io->exist(u"Z:\\sys\\bin\\midp2runtimev2.dll")) {
            return true;
        }

        return false;
    }

    bool extract_midlet_icon_png(system *sys, const std::uint32_t app_id, std::vector<std::uint8_t> &out) {
        app_list *applist = sys->get_j2me_applist();
        if (!applist) {
            return false;
        }

        std::optional<app_entry> entry = applist->get_entry(app_id);
        if (!entry.has_value()) {
            return false;
        }

        io_system *io = sys->get_io_system();
        const std::u16string storage_dir = build_midp2_storage_dir(entry.value());
        const std::string safe_name = make_safe_preinstall_name(entry.value().name_);
        const std::u16string storage_name = common::utf8_to_ucs2(safe_name);
        const std::u16string storage_jar = storage_dir + storage_name + u".jar";

        const std::optional<std::u16string> raw_path = io->get_raw_path(storage_jar);
        if (!raw_path.has_value()) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: no host path for JAR at {}", common::ucs2_to_utf8(storage_jar));
            return false;
        }

        FILE *jar_file = common::open_c_file(common::ucs2_to_utf8(*raw_path), "rb");
        if (!jar_file) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: can't open JAR at {}", common::ucs2_to_utf8(*raw_path));
            return false;
        }

        // Parse the JAR manifest to find the icon file name.
        app_entry parsed;
        std::string jad_content;
        int midp_ver = 0;
        if (get_app_entry(jar_file, parsed, jad_content, midp_ver) != INSTALL_ERROR_JAR_SUCCESS) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: can't parse manifest");
            fclose(jar_file);
            return false;
        }

        if (parsed.icon_path_.empty()) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: manifest has no icon path (MIDlet-1 entry)");
            fclose(jar_file);
            return false;
        }

        // get_app_entry leaves the FILE* seeked to the end; rewind before
        // re-initialising the zip reader on the same handle.
        fseek(jar_file, 0, SEEK_SET);
        std::unique_ptr<mz_zip_archive> archive = std::make_unique<mz_zip_archive>();
        if (!mz_zip_reader_init_cfile(archive.get(), jar_file, 0, 0)) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: can't re-init zip reader");
            fclose(jar_file);
            return false;
        }

        bool found = false;
        mz_uint icon_index = 0;
        mz_zip_archive_file_stat stat;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(archive.get()); i++) {
            if (mz_zip_reader_file_stat(archive.get(), i, &stat)) {
                if (common::compare_ignore_case(parsed.icon_path_.c_str(), stat.m_filename) == 0) {
                    icon_index = i;
                    found = true;
                    break;
                }
            }
        }

        if (!found) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: icon entry '{}' not found in JAR", parsed.icon_path_);
            mz_zip_reader_end(archive.get());
            fclose(jar_file);
            return false;
        }

        out.resize(static_cast<std::size_t>(stat.m_uncomp_size));
        const bool ok = mz_zip_reader_extract_to_mem(archive.get(), icon_index, out.data(), out.size(), 0);

        mz_zip_reader_end(archive.get());
        fclose(jar_file);

        if (!ok) {
            LOG_WARN(J2ME, "extract_midlet_icon_png: extract_to_mem failed for '{}'", parsed.icon_path_);
            out.clear();
            return false;
        }
        return true;
    }
}
