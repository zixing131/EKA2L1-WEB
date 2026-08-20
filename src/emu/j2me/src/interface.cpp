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
#include <common/cvt.h>
#include <common/fileutils.h>
#include <common/log.h>
#include <common/path.h>
#include <config/config.h>
#include <kernel/kernel.h>
#include <kernel/libmanager.h>
#include <kernel/server.h>
#include <utils/apacmd.h>
#include <utils/err.h>
#include <vfs/vfs.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <miniz.h>
#include <memory>
#include <vector>

namespace eka2l1::j2me {
    static bool g_launch_should_retry = false;
    // StubMIDP2 is a bounce process: it SendReceive()s opcode 1 to
    // !MIDP.SystemAMS.MIDP2 and User::Exit()s with the IPC result. AMS then
    // spawns j9midps60 asynchronously. A successful stub exit must not be
    // treated as "J9 failed to start".
    static bool g_ams_handoff_pending = false;
    static int g_ams_handoff_waits = 0;
    static int g_last_stub_reason = 0;
    static const char *g_last_stub_how = "none";
    static int g_stub_cycle = 0;

    bool launch_should_retry() {
        return g_launch_should_retry;
    }

    // Forward declarations for the S60v3 MIDP2 launch / uninstall paths
    // (defined later in this file).
    bool launch_through_midp2(system *sys, const app_entry &entry, std::function<void(kernel::process*)> exit_cb);
    void uninstall_for_midp2(system *sys, app_list *applist, const app_entry &entry);
    bool stage_midlet_for_launch(system *sys, const std::uint32_t app_id, std::u16string &jad_path_out);
    static bool rewrite_staged_jad_jar_url(io_system *io, const std::u16string &guest_jad_path,
        const char *jar_url = "m.jar");
    static bool write_jad_from_guest_jar(io_system *io, const std::u16string &jar_path,
        const std::u16string &jad_path);

    bool launch(system *sys, const std::uint32_t app_id, std::function<void(kernel::process*)> exit_cb) {
        g_launch_should_retry = false;
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
     * Copy a guest file from src to dst.
     * Returns false only when src exists but the copy fails.
     * Missing src is treated as success when required=false.
     */
    static bool copy_guest_file(io_system *io, const std::u16string &src, const std::u16string &dst,
        const bool overwrite, const bool required) {
        if (!io->exist(src)) {
            return !required;
        }
        if (!overwrite && io->exist(dst)) {
            return true;
        }
        if (overwrite) {
            io->delete_entry(dst);
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

    static bool copy_guest_file_if_missing(io_system *io, const std::u16string &src, const std::u16string &dst) {
        return copy_guest_file(io, src, dst, false, false);
    }

    static bool write_guest_bytes(io_system *io, const std::u16string &dst, const void *data,
        const std::size_t size) {
        if (!io || !data) {
            return false;
        }
        const std::u16string dir = eka2l1::file_directory(dst);
        if (!dir.empty()) {
            io->create_directories(dir);
        }
        io->delete_entry(dst);
        symfile out = io->open_file(dst, WRITE_MODE | BIN_MODE);
        if (!out) {
            return false;
        }
        const bool ok = (out->write_file(data, 1, static_cast<std::uint32_t>(size)) == size);
        out->close();
        return ok;
    }

    static int explode_jar_to_home(io_system *io, const std::u16string &jar_path,
        const std::u16string &home) {
        if (!io || jar_path.empty() || home.empty()) {
            return 0;
        }
        symfile jar = io->open_file(jar_path, READ_MODE | BIN_MODE);
        if (!jar) {
            return 0;
        }
        std::vector<char> buf(jar->size());
        const auto nread = jar->read_file(buf.data(), 1, static_cast<std::uint32_t>(buf.size()));
        jar->close();
        if (nread != buf.size()) {
            return 0;
        }
        mz_zip_archive archive{};
        if (!mz_zip_reader_init_mem(&archive, buf.data(), buf.size(), 0)) {
            return 0;
        }
        int n = 0;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive, i, &stat) || stat.m_is_directory) {
                continue;
            }
            const std::string name = stat.m_filename;
            if (name.rfind("META-INF/", 0) == 0) {
                continue;
            }
            std::u16string rel;
            rel.reserve(name.size());
            for (unsigned char c : name) {
                rel.push_back((c == static_cast<unsigned char>('/'))
                    ? u'\\' : static_cast<char16_t>(c));
            }
            std::vector<char> out(static_cast<std::size_t>(stat.m_uncomp_size));
            if (stat.m_uncomp_size
                && !mz_zip_reader_extract_to_mem(&archive, i, out.data(), out.size(), 0)) {
                continue;
            }
            if (write_guest_bytes(io, home + rel, out.empty() ? "" : out.data(), out.size())) {
                ++n;
            }
        }
        mz_zip_reader_end(&archive);
        return n;
    }

    // jar2jxe embeds a ZIP (rom.classes + META-INF/JXE.MF) in the ROM XIP
    // image. iveLoadJxeFromFile wants that ZIP, not the surrounding DLL.
    // The first PK\x03\x04 in a DLL is often a false hit inside a string
    // (jclcldc11_23 has "PK\x03\x04zipsup.c:..."); only accept an EOCD
    // whose central directory and local header actually line up.
    static bool extract_embedded_jxe_zip(const std::vector<char> &blob, std::vector<char> &zip_out) {
        if (blob.size() < 22) {
            return false;
        }
        std::size_t best_start = static_cast<std::size_t>(-1);
        std::size_t best_end = 0;
        bool best_has_j99 = false;
        for (std::size_t i = blob.size() - 22; i > 0; --i) {
            if ((blob[i] != 'P') || (blob[i + 1] != 'K') || (blob[i + 2] != 5) || (blob[i + 3] != 6)) {
                continue;
            }
            const std::uint16_t nent = static_cast<std::uint16_t>(
                static_cast<unsigned char>(blob[i + 8])
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 9])) << 8));
            const std::uint16_t tnent = static_cast<std::uint16_t>(
                static_cast<unsigned char>(blob[i + 10])
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 11])) << 8));
            const std::uint32_t csize = static_cast<std::uint32_t>(
                static_cast<unsigned char>(blob[i + 12])
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 13])) << 8)
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 14])) << 16)
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 15])) << 24));
            const std::uint32_t coff = static_cast<std::uint32_t>(
                static_cast<unsigned char>(blob[i + 16])
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 17])) << 8)
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 18])) << 16)
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 19])) << 24));
            const std::uint16_t comment = static_cast<std::uint16_t>(
                static_cast<unsigned char>(blob[i + 20])
                | (static_cast<unsigned int>(static_cast<unsigned char>(blob[i + 21])) << 8));
            if ((nent != tnent) || (nent == 0) || (nent > 4096) || (comment > 256)
                || (csize < 46) || (csize > 0x100000) || (i < csize)) {
                continue;
            }
            const std::size_t cdir = i - csize;
            if ((cdir + 4) > blob.size()
                || (blob[cdir] != 'P') || (blob[cdir + 1] != 'K')
                || (blob[cdir + 2] != 1) || (blob[cdir + 3] != 2)
                || (cdir < coff)) {
                continue;
            }
            const std::size_t start = cdir - coff;
            if ((start + 4) > blob.size()
                || (blob[start] != 'P') || (blob[start + 1] != 'K')
                || (blob[start + 2] != 3) || (blob[start + 3] != 4)) {
                continue;
            }
            const std::size_t zip_end = i + 22 + comment;
            if (zip_end > blob.size()) {
                continue;
            }
            bool has_j99 = false;
            const std::size_t scan_lim = (start + 64 < zip_end) ? (start + 64) : zip_end;
            for (std::size_t j = start; (j + 4) <= scan_lim; ++j) {
                if ((blob[j] == 'J') && (blob[j + 1] == '9')
                    && (blob[j + 2] == '9') && (blob[j + 3] == 'J')) {
                    has_j99 = true;
                    break;
                }
            }
            if ((best_start == static_cast<std::size_t>(-1)) || (has_j99 && !best_has_j99)
                || (has_j99 == best_has_j99 && (zip_end - start) > (best_end - best_start))) {
                best_start = start;
                best_end = zip_end;
                best_has_j99 = has_j99;
            }
        }
        if (best_start == static_cast<std::size_t>(-1)) {
            return false;
        }
        zip_out.assign(blob.begin() + static_cast<std::ptrdiff_t>(best_start),
            blob.begin() + static_cast<std::ptrdiff_t>(best_end));
        return !zip_out.empty();
    }

    // jar2jxe ZIP is STORE. type=JXE / iveLoadJxe fopen("jxe=<ptr>") maps the
    // file as a raw J9 image; feeding it the wrapping PK makes romMethods
    // resolve to address 0 (KERN-EXEC 3 at j9vmall ldr [r7,#8]).
    static bool extract_stored_zip_member(const std::vector<char> &zip, const char *name,
        std::vector<char> &out) {
        if (!name || zip.size() < 30) {
            return false;
        }
        const std::size_t nlen = std::strlen(name);
        std::size_t i = 0;
        while (i + 30 <= zip.size()) {
            if ((zip[i] != 'P') || (zip[i + 1] != 'K') || (zip[i + 2] != 3) || (zip[i + 3] != 4)) {
                const auto *hit = reinterpret_cast<const char *>(
                    std::memchr(zip.data() + i + 1, 'P', zip.size() - i - 1));
                if (!hit) {
                    return false;
                }
                i = static_cast<std::size_t>(hit - zip.data());
                continue;
            }
            const std::uint16_t method = static_cast<std::uint16_t>(
                static_cast<unsigned char>(zip[i + 8])
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 9])) << 8));
            const std::uint32_t csize = static_cast<std::uint32_t>(
                static_cast<unsigned char>(zip[i + 18])
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 19])) << 8)
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 20])) << 16)
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 21])) << 24));
            const std::uint32_t usize = static_cast<std::uint32_t>(
                static_cast<unsigned char>(zip[i + 22])
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 23])) << 8)
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 24])) << 16)
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 25])) << 24));
            const std::uint16_t fnlen = static_cast<std::uint16_t>(
                static_cast<unsigned char>(zip[i + 26])
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 27])) << 8));
            const std::uint16_t extra = static_cast<std::uint16_t>(
                static_cast<unsigned char>(zip[i + 28])
                | (static_cast<unsigned int>(static_cast<unsigned char>(zip[i + 29])) << 8));
            const std::size_t data = i + 30 + fnlen + extra;
            if ((data + csize) > zip.size()) {
                return false;
            }
            if ((fnlen == nlen) && (std::memcmp(zip.data() + i + 30, name, nlen) == 0)) {
                if ((method != 0) || (csize != usize) || (usize < 16)) {
                    return false;
                }
                out.assign(zip.begin() + static_cast<std::ptrdiff_t>(data),
                    zip.begin() + static_cast<std::ptrdiff_t>(data + usize));
                return (out[0] == 'J') && (out[1] == '9') && (out[2] == '9') && (out[3] == 'J');
            }
            i = data + csize;
        }
        return false;
    }

    static bool extract_jxe_from_guest_dll(io_system *io, const std::u16string &dll,
        const std::u16string &dst) {
        if (!io->exist(dll)) {
            return false;
        }
        symfile src = io->open_file(dll, READ_MODE | BIN_MODE);
        if (!src) {
            return false;
        }
        std::vector<char> blob(src->size());
        src->read_file(blob.data(), 1, static_cast<std::uint32_t>(blob.size()));
        src->close();
        std::vector<char> zip;
        if (!extract_embedded_jxe_zip(blob, zip)) {
            return false;
        }
        // Bootclasspath is now C:\jcl.jxe (ends in .jxe), so j9ext.c takes
        // the JXE/hyzip file path instead of treating "jxe=%p" as a JAR.
        // That path wants the wrapping ZIP (PK + rom.classes at +48), not
        // the inner naked J99J — hyzip looks up "rom.classes" by name.
        return write_guest_bytes(io, dst, zip.data(), zip.size());
    }

    // iveLoadJxe of an in-memory JXESL pointer (J9GetJXE) crashes in
    // j9vmall dynload on this port (null+0x28). iveLoadJxeFromFile of the
    // same ZIP works — JCL already takes that path. Rewrite every staged
    // type=JXESL odc to type=JXE and drop the embedded ZIP next to it.
    static void rewrite_jxsl_odcs_to_file_jxe(io_system *io) {
        static constexpr char16_t C_EXT[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\";
        static constexpr char16_t C_EXT_ODC[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\odc\\";
        auto listing = io->open_dir(std::u16string(C_EXT) + u"*", {}, io_attrib_include_file);
        if (!listing) {
            return;
        }
        int converted = 0;
        while (auto ent = listing->get_next_entry()) {
            const std::string fname = ent->name;
            const std::string lower_fn = common::lowercase_string(fname);
            if ((lower_fn.size() < 4) || (lower_fn.compare(lower_fn.size() - 4, 4, ".odc") != 0)) {
                continue;
            }
            const std::u16string odc_path = std::u16string(C_EXT) + common::utf8_to_ucs2(fname);
            symfile f = io->open_file(odc_path, READ_MODE | BIN_MODE);
            if (!f) {
                continue;
            }
            std::string text(static_cast<std::size_t>(f->size()), '\0');
            f->read_file(text.data(), 1, static_cast<std::uint32_t>(text.size()));
            f->close();
            const std::string lower_text = common::lowercase_string(text);
            if (lower_text.find("type=jxesl") == std::string::npos) {
                continue;
            }
            std::string container;
            const std::size_t npos = lower_text.find("name=");
            if (npos != std::string::npos) {
                const std::size_t start = npos + 5;
                const std::size_t end = text.find_first_of("\r\n", start);
                container = text.substr(start, ((end == std::string::npos) ? text.size() : end) - start);
                while (!container.empty() && ((container.back() == ' ') || (container.back() == '\t'))) {
                    container.pop_back();
                }
            }
            if (container.empty()) {
                continue;
            }
            std::string stem = container;
            const std::string stem_l = common::lowercase_string(stem);
            if ((stem_l.size() >= 4) && (stem_l.compare(stem_l.size() - 4, 4, ".jxe") == 0)) {
                stem.resize(stem.size() - 4);
            }
            const std::u16string dll = u"Z:\\sys\\bin\\" + common::utf8_to_ucs2(stem) + u".dll";
            const std::string jxe_name = stem + ".jxe";
            const std::u16string jxe_path = std::u16string(C_EXT) + common::utf8_to_ucs2(jxe_name);
            if (!extract_jxe_from_guest_dll(io, dll, jxe_path)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JXESL {} extract failed from {}", fname,
                    common::ucs2_to_utf8(dll));
                continue;
            }
            copy_guest_file(io, jxe_path, std::u16string(C_EXT_ODC) + common::utf8_to_ucs2(jxe_name), true, false);
            std::string out = text;
            auto replace_key = [&](const char *key, const std::string &val) {
                const std::string lkey = common::lowercase_string(key);
                const std::size_t p = common::lowercase_string(out).find(lkey);
                if (p == std::string::npos) {
                    return;
                }
                const std::size_t e = out.find_first_of("\r\n", p);
                const std::size_t n = (e == std::string::npos) ? (out.size() - p) : (e - p);
                out.replace(p, n, std::string(key) + val);
            };
            replace_key("name=", jxe_name);
            replace_key("type=", "JXE");
            write_guest_bytes(io, odc_path, out.data(), out.size());
            ++converted;
        }
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] converted {} JXESL odc(s) to type=JXE file load", converted);
    }

    static void copy_guest_dir_files(io_system *io, const std::u16string &src_dir,
        const std::u16string &dst_dir) {
        io->create_directories(dst_dir);
        auto listing = io->open_dir(src_dir + u"*", {}, io_attrib_include_file);
        if (!listing) {
            return;
        }
        while (auto ent = listing->get_next_entry()) {
            if (ent->name.empty() || (ent->name[0] == '.')) {
                continue;
            }
            copy_guest_file(io, src_dir + common::utf8_to_ucs2(ent->name),
                dst_dir + common::utf8_to_ucs2(ent->name), true, false);
        }
    }

    // J9 dynload scans %c:\resource\ive\lib\jclCldc11\ext\*.odc.
    // %c is the system drive (C:), so an empty C: ext hides the ROM
    // type=JXESL descriptors. Copy the ROM odc tree onto C: (keep
    // type=JXESL so J9GetJXE + in-DLL natives stay together) and also
    // extract the embedded JXE ZIPs as files for iveLoadJxeFromFile.
    static void stage_j9_jxe_runtime(io_system *io) {
        static constexpr char16_t Z_EXT[] = u"Z:\\resource\\ive\\lib\\jclCldc11\\ext\\";
        static constexpr char16_t C_EXT[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\";
        static constexpr char16_t C_EXT_ODC[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\odc\\";
        static constexpr char16_t Z_MIDP[] = u"Z:\\sys\\bin\\j9_23_midp2ams.dll";
        static constexpr char16_t Z_JCL[] = u"Z:\\sys\\bin\\jclcldc11_23.dll";
        static constexpr char16_t C_MIDP_JXE[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\midp2ams.jxe";
        static constexpr char16_t C_JCL_JXE[] = u"C:\\resource\\ive\\lib\\jclCldc11\\ext\\jcl.jxe";

        copy_guest_dir_files(io, Z_EXT, C_EXT);
        copy_guest_dir_files(io, std::u16string(Z_EXT) + u"odc\\", C_EXT_ODC);

        const bool midp_ok = extract_jxe_from_guest_dll(io, Z_MIDP, C_MIDP_JXE);
        const bool jcl_ok = extract_jxe_from_guest_dll(io, Z_JCL, C_JCL_JXE);
        if (midp_ok) {
            copy_guest_file(io, C_MIDP_JXE, std::u16string(C_EXT_ODC) + u"midp2ams.jxe", true, false);
            copy_guest_file(io, C_MIDP_JXE, u"C:\\midp2ams.jxe", true, false);
        }
        if (jcl_ok) {
            copy_guest_file(io, C_JCL_JXE, std::u16string(C_EXT_ODC) + u"jcl.jxe", true, false);
            copy_guest_file(io, C_JCL_JXE, u"C:\\jcl.jxe", true, false);
            copy_guest_file(io, C_JCL_JXE, u"C:\\jcl.zip", true, false);
            copy_guest_file(io, C_JCL_JXE, u"C:\\resource\\ive\\lib\\jclCldc11\\jclcldc11_23.jxe", true, false);
            copy_guest_file(io, C_JCL_JXE, u"C:\\resource\\ive\\lib\\jclCldc11\\jclcdc11_23.jxe", true, false);
            copy_guest_file(io, C_JCL_JXE, u"C:\\resource\\ive\\lib\\jclCldc11\\romclass_cln.jxe", true, false);
        }

        // JCL is a plain JXE file. midp2ams stays ROM type=JXESL so
        // J9GetJXE + J9VMDllMain still bind JNI natives.
        static const char JCL_ODC[] =
            "[container]\n"
            "name=jcl.jxe\n"
            "type=JXE\n"
            "\n"
            "[packages]\n"
            "com/ibm/oti/io\n"
            "com/ibm/oti/lang\n"
            "com/ibm/oti/util\n"
            "com/ibm/oti/vm\n"
            "java/io\n"
            "java/lang\n"
            "java/lang/ref\n"
            "java/security\n"
            "java/util\n"
            "javax/microedition/io\n"
            "\n"
            "[properties]\n";
        write_guest_bytes(io, std::u16string(C_EXT) + u"0_jcl_jxe.odc",
            JCL_ODC, sizeof(JCL_ODC) - 1);

        rewrite_jxsl_odcs_to_file_jxe(io);

        LOG_WARN(EMULATED_STDOUT, "[j9-nf] staged JXE midp={} ({} bytes) jcl={} ({} bytes) odc-on-C",
            midp_ok ? 1 : 0,
            io->exist(C_MIDP_JXE) ? 1 : 0,
            jcl_ok ? 1 : 0,
            io->exist(C_JCL_JXE) ? 1 : 0);
        if (midp_ok) {
            if (symfile jf = io->open_file(C_MIDP_JXE, READ_MODE | BIN_MODE)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] midp2ams.jxe size={}", jf->size());
                jf->close();
            }
        }
        if (jcl_ok) {
            if (symfile jf = io->open_file(C_JCL_JXE, READ_MODE | BIN_MODE)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] jcl.jxe size={}", jf->size());
                jf->close();
            }
        }
    }

    static kernel::process *find_running_process_with_args(kernel_system *kern, const char *needle,
        const char *args_needle) {
        if (!kern || !needle || !needle[0]) {
            return nullptr;
        }
        const std::string lower_needle = common::lowercase_string(needle);
        const std::string lower_args = args_needle ? common::lowercase_string(args_needle) : std::string();
        for (auto &obj : kern->get_process_list()) {
            auto *pr = reinterpret_cast<kernel::process *>(obj.get());
            if (!pr || (pr->get_exit_type() != kernel::entity_exit_type::pending)) {
                continue;
            }
            if (common::lowercase_string(pr->raw_name()).find(lower_needle) == std::string::npos) {
                continue;
            }
            if (!lower_args.empty()) {
                const std::string cmd = common::lowercase_string(common::ucs2_to_utf8(pr->get_cmd_args()));
                if (cmd.find(lower_args) == std::string::npos) {
                    continue;
                }
            }
            return pr;
        }
        return nullptr;
    }

    static kernel::process *find_running_process(kernel_system *kern, const char *needle) {
        return find_running_process_with_args(kern, needle, nullptr);
    }

    std::u16string build_j9midps60_args(std::uint32_t suite_uid, const std::u16string &midlet_class) {
        // Do NOT pass -jcl/-Xjcl: midp2ams Args.parse does not know those
        // flags (Unrecognized argument → usage → Exit(1)). j9vmall loads
        // the JCL itself as jclcdc11_23.dll; the loader aliases that to
        // the ROM's jclcldc11_23.dll.
        std::u16string args = u"-jad C:/j.jad -jar C:/j.jar -msid "
            + common::utf8_to_ucs2(fmt::format("{}", suite_uid)) + u" -msin 1";
        if (!midlet_class.empty()) {
            args += u" -app " + midlet_class;
        }
        return args;
    }

    // SystemAMSCore -boot scans ?:\system\data\midp2\preinstall\ (and the later
    // OMJ resource\java\preinstall path) for *.jad/*.jar and spawns
    // MIDP2SilentMIDletInstall. Direct J9 launch already restages the suite
    // under C:\private\102033E6\MIDlets\<uid>\; leaving bait in those scan
    // dirs makes the installer race the VM (ifeui / javareg / SWInst -30471)
    // and J9 User::Exit(1).
    static void clear_preinstall_scan_dirs(io_system *io) {
        static const char16_t *DIRS[] = {
            u"E:\\system\\data\\midp2\\preinstall\\",
            u"C:\\system\\data\\midp2\\preinstall\\",
            u"E:\\resource\\java\\preinstall\\",
            u"C:\\resource\\java\\preinstall\\",
        };
        for (const char16_t *dir : DIRS) {
            auto listing = io->open_dir(std::u16string(dir) + u"*", {}, io_attrib_include_file);
            if (!listing) {
                continue;
            }
            while (auto ent = listing->get_next_entry()) {
                const std::string lower = common::lowercase_string(ent->name);
                if ((lower.size() < 4)
                    || ((lower.compare(lower.size() - 4, 4, ".jad") != 0)
                        && (lower.compare(lower.size() - 4, 4, ".jar") != 0))) {
                    continue;
                }
                const std::u16string path = std::u16string(dir) + common::utf8_to_ucs2(ent->name);
                if (io->delete_entry(path)) {
                    LOG_INFO(J2ME, "Cleared AMS auto-install file {}", common::ucs2_to_utf8(path));
                }
            }
        }
    }

    static bool java_silent_installer_running(kernel_system *kern) {
        return find_running_process(kern, "midp2silent") != nullptr;
    }

    static void stop_java_auto_installer(kernel_system *kern) {
        if (!kern) {
            return;
        }
        std::vector<kernel::process *> victims;
        for (auto &obj : kern->get_process_list()) {
            auto *pr = reinterpret_cast<kernel::process *>(obj.get());
            if (!pr || (pr->get_exit_type() != kernel::entity_exit_type::pending)) {
                continue;
            }
            const std::string lower = common::lowercase_string(pr->raw_name());
            if ((lower.find("midp2silent") != std::string::npos)
                || (lower.find("101f875a") != std::string::npos)) {
                victims.push_back(pr);
            }
        }
        for (kernel::process *pr : victims) {
            LOG_WARN(J2ME, "Stopping leftover Java installer {} so it cannot race J9", pr->name());
            pr->kill(kernel::entity_exit_type::kill, u"J2ME", 0);
        }
    }

    void stop_silent_installer(system *sys) {
        if (sys) {
            stop_java_auto_installer(sys->get_kernel_system());
        }
    }

    // Silent install must commit the suite into AMS/JavaReg before StubMIDP2
    // opcode 1. Killing it at the first launch retry left AMS with KErrNotFound.
    static void wait_for_silent_installer(system *sys, const int max_steps) {
        kernel_system *kern = sys->get_kernel_system();
        if (!java_silent_installer_running(kern)) {
            return;
        }
        LOG_WARN(J2ME, "Silent installer still running; pumping so it can commit before StubMIDP2");
        for (int i = 0; i < max_steps; i++) {
            sys->loop();
            if (!java_silent_installer_running(kern)) {
                LOG_WARN(J2ME, "Silent installer exited after {} extra steps", i + 1);
                return;
            }
        }
        LOG_WARN(J2ME, "Silent installer still running after {} steps; leaving it to finish", max_steps);
    }

    static kernel::process *ensure_guest_process(kernel_system *kern, io_system *io,
        const char16_t *path, const char16_t *args, const char *needle) {
        if (kernel::process *existing = find_running_process(kern, needle)) {
            return existing;
        }
        if (!io->exist(path)) {
            return nullptr;
        }
        kernel::process *pr = kern->spawn_new_process(path, args ? args : u"");
        if (!pr) {
            LOG_WARN(J2ME, "Can't spawn {}", common::ucs2_to_utf8(path));
            return nullptr;
        }
        if (!pr->run()) {
            LOG_WARN(J2ME, "Can't run {}", common::ucs2_to_utf8(path));
        } else {
            LOG_INFO(J2ME, "Started Java helper {} args='{}'", common::ucs2_to_utf8(path),
                common::ucs2_to_utf8(args ? args : u""));
        }
        return pr;
    }

    static kernel::process *spawn_systemams_core(kernel_system *kern, io_system *io,
        const char16_t *args) {
        static constexpr char16_t AMS_CORE[] = u"Z:\\sys\\bin\\systemamscore.exe";
        if (!io->exist(AMS_CORE)) {
            return nullptr;
        }
        kernel::process *pr = kern->spawn_new_process(AMS_CORE, args ? args : u"");
        if (!pr) {
            LOG_WARN(J2ME, "Can't spawn SystemAMSCore.exe args='{}'",
                common::ucs2_to_utf8(args ? args : u""));
            return nullptr;
        }
        if (!pr->run()) {
            LOG_WARN(J2ME, "Can't run SystemAMSCore.exe args='{}'",
                common::ucs2_to_utf8(args ? args : u""));
        } else {
            LOG_WARN(J2ME, "Started SystemAMSCore.exe args='{}'",
                common::ucs2_to_utf8(args ? args : u""));
        }
        return pr;
    }

    // SystemAMS.EXE (ROM 0x81d1e720, SID 0x200159D7) hosts the trader:
    // E32Main StartServer()s !SystemAMSTrader.Private then .Public, then
    // RProcess::Create()s SystemAMSCore.exe with " -boot" / " -proxy".
    // StubMIDP2 pings Public first; spawning only SystemAMSCore never
    // publishes those names (both cores fall through to AMS boot).
    static kernel::process *spawn_system_ams(kernel_system *kern, io_system *io) {
        static constexpr char16_t SYSTEM_AMS[] = u"Z:\\sys\\bin\\SystemAMS.exe";
        if (!io->exist(SYSTEM_AMS)) {
            return nullptr;
        }
        kernel::process *pr = kern->spawn_new_process(SYSTEM_AMS, u"");
        if (!pr) {
            LOG_WARN(J2ME, "Can't spawn SystemAMS.exe");
            return nullptr;
        }
        if (!pr->run()) {
            LOG_WARN(J2ME, "Can't run SystemAMS.exe");
        } else {
            LOG_WARN(J2ME, "Started SystemAMS.exe (trader + SystemAMSCore launcher)");
        }
        return pr;
    }

    static kernel::process *boot_java_runtime(system *sys) {
        io_system *io = sys->get_io_system();
        kernel_system *kern = sys->get_kernel_system();
        prepare_midp2_environment(sys);

        ensure_guest_process(kern, io, u"Z:\\sys\\bin\\AMSDbServer.exe", u"", "amsdbserver");
        ensure_guest_process(kern, io, u"Z:\\sys\\bin\\javaregistry.exe", u"", "javaregistry");
        ensure_guest_process(kern, io, u"Z:\\sys\\bin\\JavaRedirServer.exe", u"", "javaredir");

        // "systemams[" matches SystemAMS[uid], not systemamscore[uid].
        const bool trader_up = kern->get_by_name<service::server>("!SystemAMSTrader.Public");
        if (!trader_up && !find_running_process(kern, "systemams[")) {
            spawn_system_ams(kern, io);
        }

        kernel::process *ams = find_running_process_with_args(kern, "systemamscore", "-boot");
        if (!ams) {
            for (int i = 0; i < 48; i++) {
                sys->loop();
                ams = find_running_process_with_args(kern, "systemamscore", "-boot");
                if (ams) {
                    LOG_WARN(J2ME, "SystemAMSCore.exe -boot appeared after {} steps (via SystemAMS.exe)",
                        i + 1);
                    break;
                }
            }
        }
        if (!ams) {
            // ROM without SystemAMS.exe, or the trampoline did not spawn cores.
            if (!find_running_process_with_args(kern, "systemamscore", "-proxy")
                && !kern->get_by_name<service::server>("!SystemAMSTrader.Public")) {
                spawn_systemams_core(kern, io, u" -proxy");
            }
            ams = spawn_systemams_core(kern, io, u" -boot");
        }
        if (!ams) {
            ams = find_running_process(kern, "systemamscore");
        }
        if (!ams) {
            LOG_WARN(J2ME, "SystemAMSCore.exe -boot could not be started");
        }
        return ams;
    }

    static bool guest_server_exists(kernel_system *kern, const char *name) {
        return kern && name && kern->get_by_name<service::server>(name);
    }

    static bool guest_server_prefix(kernel_system *kern, const char *prefix) {
        if (!kern || !prefix || !prefix[0]) {
            return false;
        }
        int start = 0;
        for (;;) {
            auto found = kern->find_object("*", start, kernel::object_type::server, false);
            if (!found) {
                return false;
            }
            start = static_cast<int>(found->index);
            if (found->obj->name().find(prefix) != std::string::npos) {
                return true;
            }
        }
    }

    static bool java_ams_server_ready(kernel_system *kern) {
        // J9 talks to !MIDP.SystemAMS.SystemAMS. StubMIDP2 first
        // CreateSession()s !SystemAMSTrader.Public, then !MIDP.SystemAMS.MIDP2.
        return guest_server_exists(kern, "!MIDP.SystemAMS.SystemAMS")
            || guest_server_exists(kern, "!MIDP.SystemAMS.MIDP2")
            || guest_server_exists(kern, "!SystemAMSTrader.Public");
    }

    static bool java_midp2_server_ready(kernel_system *kern) {
        return guest_server_exists(kern, "!MIDP.SystemAMS.MIDP2");
    }

    static bool java_ams_trader_ready(kernel_system *kern) {
        return guest_server_exists(kern, "!SystemAMSTrader.Public");
    }

    // StubMIDP2RecogExe Connect() at ROM 0x81d78ee2:
    //   1. CreateSession("!SystemAMSTrader.Public") + opcode 1 ping
    //   2. only then CreateSession("!MIDP.SystemAMS.MIDP2")
    // Missing trader → E32Main returns -1 without ever touching MIDP2.
    static bool stubmidp2_connect_ready(kernel_system *kern) {
        return java_ams_trader_ready(kern) && java_midp2_server_ready(kern);
    }

    static void log_guest_servers(kernel_system *kern, const char *why) {
        LOG_WARN(J2ME, "Guest servers ({}):", why);
        int start = 0;
        int n = 0;
        for (;;) {
            auto found = kern->find_object("*", start, kernel::object_type::server, false);
            if (!found) {
                break;
            }
            start = static_cast<int>(found->index);
            std::string full;
            found->obj->full_name(full);
            LOG_WARN(J2ME, "  [{}] name='{}' full='{}'", n, found->obj->name(), full);
            n++;
            if (n >= 64) {
                break;
            }
        }
        if (n == 0) {
            LOG_WARN(J2ME, "  (none)");
        }
    }

    // j9midps60.exe is an 828-byte stub that LoadLibrary's j9_23_midp2ams.dll
    // and calls export 2. That export Connect()s to !MIDP.SystemAMS.SystemAMS.
    // StubMIDP2RecogExe instead CreateSession()s !MIDP.SystemAMS.MIDP2.
    // Spawning J9 in the same C++ call as AMS means AMS has not executed yet,
    // so the session handle is null and the stub data-aborts (KERN-EXEC 3).
    // Nested loop() here is only a short pump; if AMS is still down the
    // caller must return -101 so the browser main loop can run it.
    static bool wait_for_java_ams_server(system *sys, const int max_steps) {
        kernel_system *kern = sys->get_kernel_system();
        if (java_ams_server_ready(kern)) {
            return true;
        }
        for (int i = 0; i < max_steps; i++) {
            sys->loop();
            if (java_ams_server_ready(kern)) {
                LOG_WARN(J2ME, "Java AMS server ready after {} emulator steps", i + 1);
                return true;
            }
        }
        LOG_WARN(J2ME, "Java AMS server not ready after {} emulator steps", max_steps);
        log_guest_servers(kern, "AMS wait timed out");
        return false;
    }

    static std::uint32_t java_midlet_uid(const std::uint32_t app_id) {
        return 0x20000000u | (app_id & 0x00FFFFFFu);
    }

    static std::string read_guest_text_prefix(io_system *io, const std::u16string &path, const std::size_t max_bytes) {
        symfile f = io->open_file(path, READ_MODE | BIN_MODE);
        if (!f) {
            return {};
        }
        std::string out(max_bytes, '\0');
        const std::size_t n = f->read_file(out.data(), 1, static_cast<std::uint32_t>(max_bytes));
        f->close();
        out.resize(n);
        return out;
    }

    // StubMIDP2 opcode 1 looks the suite up by AMS uid, not by the JAD path.
    // Host inject uses 0x20000000|app_id; a successful silent install creates a
    // different folder under C:\private\102033E6\MIDlets\. Prefer that uid.
    static std::uint32_t pick_ams_suite_uid(io_system *io, const app_entry &entry) {
        const std::uint32_t host_uid = java_midlet_uid(entry.id_);
        static constexpr char16_t MIDLETS[] = u"C:\\private\\102033E6\\MIDlets\\";
        auto dir = io->open_dir(MIDLETS, {}, io_attrib_include_dir);
        std::uint32_t ams_uid = 0;
        if (!dir) {
            return host_uid;
        }
        while (auto ent = dir->get_next_entry()) {
            if ((ent->type != io_component_type::dir) || (ent->name == ".") || (ent->name == "..")) {
                continue;
            }
            if (ent->name.size() != 8) {
                continue;
            }
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(ent->name.c_str(), &end, 16);
            if (!end || (*end != '\0') || (parsed > 0xFFFFFFFFul)) {
                continue;
            }
            const auto uid = static_cast<std::uint32_t>(parsed);
            const std::u16string jad = std::u16string(MIDLETS) + common::utf8_to_ucs2(ent->name) + u"\\m.jad";
            const std::string jad_text = read_guest_text_prefix(io, jad, 1024);
            const bool name_hit = !jad_text.empty()
                && ((jad_text.find(entry.name_) != std::string::npos)
                    || (!entry.title_.empty() && (jad_text.find(entry.title_) != std::string::npos)));
            LOG_WARN(J2ME, "AMS MIDlets suite uid=0x{:08X} jad={} name_hit={}",
                uid, !jad_text.empty(), name_hit);
            if (name_hit && (uid != host_uid)) {
                ams_uid = uid;
            }
        }
        if (ams_uid) {
            LOG_WARN(J2ME, "Using AMS-registered suite uid 0x{:08X} instead of host inject 0x{:08X}",
                ams_uid, host_uid);
            return ams_uid;
        }
        return host_uid;
    }

    bool prepare_midp2_environment(system *sys) {
        io_system *io = sys->get_io_system();

        // 出厂 C: 目录树: 缺目录会让策略/AMS 组件收到 KErrPathNotFound 而非
        // KErrNotFound, 其回退逻辑不识别前者, 直接 Leave 崩溃。
        static const char16_t *REQUIRED_DIRS[] = {
            u"C:\\system\\data\\midp2\\security\\policy\\",
            u"C:\\system\\data\\midp2\\preinstall\\",
            u"C:\\system\\data\\midp2\\systemams\\",
            u"C:\\private\\101F9F6C\\security\\",
            u"C:\\private\\102033E6\\MIDlets\\",
            // SystemAMSCore UID3 is 0x101F9F6C but its PlatSec SID is
            // 0x10203636 — FS private paths use the SID, not UID3.
            u"C:\\private\\10203636\\systemams\\connection\\",
            // SWInst UI / journal and Java integrity hash store. Missing
            // directories become KErrPathNotFound and abort the silent
            // installer before AMS can register the suite.
            u"C:\\private\\101F875A\\",
            u"C:\\private\\1028247A\\",
            // J9 locale lookup: Z:\resource\ive\bin\java_<lang>.properties
            // (ROM has none). Empty C: copies stop KErrPathNotFound.
            u"C:\\resource\\ive\\bin\\",
        };
        bool ok = true;
        for (const char16_t *dir : REQUIRED_DIRS) {
            if (!io->exist(dir) && !io->create_directories(dir)) {
                LOG_ERROR(J2ME, "Can't create MIDP2 directory {}", common::ucs2_to_utf8(dir));
                ok = false;
            }
        }

        static constexpr char16_t JOURNAL_DB[] = u"C:\\private\\101F875A\\journal.db";
        if (!io->exist(JOURNAL_DB)) {
            if (symfile jf = io->open_file(JOURNAL_DB, WRITE_MODE | BIN_MODE)) {
                jf->close();
                LOG_WARN(J2ME, "Created empty SWInst journal.db");
            } else {
                LOG_WARN(J2ME, "Can't create SWInst journal.db");
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

        // RPKG has the real 35KB java.properties. An empty C: stub from an
        // earlier bring-up pass must be replaced, or J9 boots with no
        // java.home / bootstrap library path.
        static constexpr char16_t Z_JAVA_PROPS[] = u"Z:\\resource\\ive\\bin\\java.properties";
        static constexpr char16_t C_JAVA_PROPS[] = u"C:\\resource\\ive\\bin\\java.properties";
        bool c_props_empty = true;
        if (symfile cf = io->open_file(C_JAVA_PROPS, READ_MODE | BIN_MODE)) {
            c_props_empty = (cf->size() == 0);
            cf->close();
        }
        if (c_props_empty) {
            if (copy_guest_file(io, Z_JAVA_PROPS, C_JAVA_PROPS, true, false)) {
                LOG_WARN(J2ME, "Provisioned java.properties from ROM");
            }
        }

        // Previous DirOpen fallback created an empty nokiaextcldc that
        // hid the JXESL in j9_23_midp2ams.dll. Remove it if still there.
        static constexpr char16_t PHANTOM_NOKIAEXT[] =
            u"C:\\resource\\ive\\lib\\jclCldc11\\nokiaextcldc\\";
        if (io->exist(PHANTOM_NOKIAEXT)) {
            io->delete_entry(PHANTOM_NOKIAEXT);
            LOG_WARN(J2ME, "Removed empty phantom nokiaextcldc");
        }

        static const char16_t *J9_LOCALE_PROPS[] = {
            u"C:\\resource\\ive\\bin\\java_en.properties",
            u"C:\\resource\\ive\\bin\\java_en_GB.properties",
            u"C:\\resource\\ive\\bin\\java_en_US.properties",
        };
        for (const char16_t *prop : J9_LOCALE_PROPS) {
            if (io->exist(prop)) {
                continue;
            }
            if (symfile pf = io->open_file(prop, WRITE_MODE | BIN_MODE)) {
                pf->close();
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
        } else {
            bool any_alnum = false;
            for (const unsigned char c : safe) {
                if (std::isalnum(c)) {
                    any_alnum = true;
                    break;
                }
            }
            if (!any_alnum) {
                safe = "midlet";
            }
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

        if (!copy_guest_file(io, src_jar, dst_jar, true, true)) {
            return false;
        }
        copy_guest_file(io, src_jad, dst_jad, true, false); // JAD optional
        if (io->exist(dst_jad)) {
            rewrite_staged_jad_jar_url(io, dst_jad);
        }

        // Some S60 Java preinstallers scan :\resource\java\preinstall\ instead
        // of the older MIDP2 directory. Mirror the staged pair there too.
        static constexpr char16_t OMJ_PREINSTALL_DIR[] = u"E:\\resource\\java\\preinstall\\";
        if (io->create_directories(OMJ_PREINSTALL_DIR)) {
            const std::u16string omj_jar = std::u16string(OMJ_PREINSTALL_DIR) + u"m.jar";
            const std::u16string omj_jad = std::u16string(OMJ_PREINSTALL_DIR) + u"m.jad";
            copy_guest_file(io, src_jar, omj_jar, true, false);
            copy_guest_file(io, src_jad, omj_jad, true, false);
        }

        return true;
    }

    // StubMIDP2 / AMS want a suite JAD in env slot 1. A JAR-only install never
    // wrote m.jad, so launch_via_stubmidp2 used to return false with no log.
    // Build a descriptor from META-INF/MANIFEST.MF and point MIDlet-Jar-URL
    // at the sibling m.jar.
    static bool write_jad_from_guest_jar(io_system *io, const std::u16string &jar_path,
        const std::u16string &jad_path) {
        if (!io || jar_path.empty() || jad_path.empty()) {
            return false;
        }
        if (io->exist(jad_path)) {
            return true;
        }

        symfile jar = io->open_file(jar_path, READ_MODE | BIN_MODE);
        if (!jar) {
            LOG_ERROR(J2ME, "Can't open JAR to synthesize JAD: {}", common::ucs2_to_utf8(jar_path));
            return false;
        }

        std::vector<char> buf(jar->size());
        const std::uint32_t jar_size = static_cast<std::uint32_t>(buf.size());
        if (jar->read_file(buf.data(), 1, jar_size) != jar_size) {
            jar->close();
            return false;
        }
        jar->close();

        mz_zip_archive archive{};
        if (!mz_zip_reader_init_mem(&archive, buf.data(), buf.size(), 0)) {
            LOG_ERROR(J2ME, "JAR is not a zip; can't synthesize JAD");
            return false;
        }

        std::int32_t index = -1;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); i++) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
                continue;
            }
            if (common::compare_ignore_case(stat.m_filename, "META-INF/MANIFEST.MF") == 0) {
                index = static_cast<std::int32_t>(i);
                break;
            }
        }
        if (index < 0) {
            mz_zip_reader_end(&archive);
            LOG_ERROR(J2ME, "JAR has no META-INF/MANIFEST.MF; can't synthesize JAD");
            return false;
        }

        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat)
            || (stat.m_uncomp_size >= common::MB(1))) {
            mz_zip_reader_end(&archive);
            return false;
        }

        std::string manifest(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&archive, static_cast<mz_uint>(index),
                manifest.data(), manifest.size(), 0)) {
            mz_zip_reader_end(&archive);
            return false;
        }
        mz_zip_reader_end(&archive);

        while (!manifest.empty() && ((manifest.back() == '\n') || (manifest.back() == '\r'))) {
            manifest.pop_back();
        }
        manifest += "\r\nMIDlet-Jar-URL: m.jar\r\n";
        manifest += fmt::format("MIDlet-Jar-Size: {}\r\n", jar_size);

        io->delete_entry(jad_path);
        symfile out = io->open_file(jad_path, WRITE_MODE | BIN_MODE);
        if (!out) {
            LOG_ERROR(J2ME, "Can't write synthesized JAD {}", common::ucs2_to_utf8(jad_path));
            return false;
        }
        const bool ok = (out->write_file(manifest.data(), 1,
            static_cast<std::uint32_t>(manifest.size())) == manifest.size());
        out->close();
        if (ok) {
            LOG_INFO(J2ME, "Synthesized suite JAD from JAR manifest at {}",
                common::ucs2_to_utf8(jad_path));
        }
        return ok;
    }

    static std::string jad_attr_line(const std::string &jad, const char *key) {
        const std::size_t pos = jad.find(key);
        if (pos == std::string::npos) {
            return {};
        }
        std::size_t end = jad.find_first_of("\r\n", pos);
        if (end == std::string::npos) {
            end = jad.size();
        }
        std::string val = jad.substr(pos + std::strlen(key), end - (pos + std::strlen(key)));
        while (!val.empty() && ((val.front() == ' ') || (val.front() == '\t'))) {
            val.erase(val.begin());
        }
        while (!val.empty() && ((val.back() == ' ') || (val.back() == '\t'))) {
            val.pop_back();
        }
        return val;
    }

    static std::string read_guest_text(io_system *io, const std::u16string &path) {
        if (!io || !io->exist(path)) {
            return {};
        }
        symfile f = io->open_file(path, READ_MODE | BIN_MODE);
        if (!f) {
            return {};
        }
        std::string text(f->size(), '\0');
        f->read_file(text.data(), 1, static_cast<std::uint32_t>(text.size()));
        f->close();
        return text;
    }

    static bool write_ascii_runtime_jad(io_system *io, const std::u16string &jad_path,
        const std::u16string &jar_path, const std::string &midlet_class) {
        std::uint64_t jar_size = 0;
        if (symfile rf = io->open_file(jar_path, READ_MODE | BIN_MODE)) {
            jar_size = rf->size();
            rf->close();
        }
        std::string cls = midlet_class.empty() ? "MIDlet" : midlet_class;
        std::string name = cls;
        const std::size_t dot = name.rfind('.');
        if ((dot != std::string::npos) && (dot + 1 < name.size())) {
            name = name.substr(dot + 1);
        }
        auto ascii_only = [](std::string &s) {
            for (char &c : s) {
                if (static_cast<unsigned char>(c) > 0x7F) {
                    c = '_';
                }
            }
        };
        ascii_only(cls);
        ascii_only(name);
        const std::string body = fmt::format(
            "MIDlet-Name: {}\r\n"
            "MIDlet-Version: 1.0\r\n"
            "MIDlet-Vendor: Vendor\r\n"
            "MIDlet-Jar-URL: j.jar\r\n"
            "MIDlet-Jar-Size: {}\r\n"
            "MIDlet-1: {}, /icon.png, {}\r\n"
            "MicroEdition-Profile: MIDP-2.0\r\n"
            "MicroEdition-Configuration: CLDC-1.1\r\n",
            name, jar_size, name, cls);
        io->delete_entry(jad_path);
        symfile out = io->open_file(jad_path, WRITE_MODE | BIN_MODE);
        if (!out) {
            return false;
        }
        const bool ok = (out->write_file(body.data(), 1,
            static_cast<std::uint32_t>(body.size())) == body.size());
        out->close();
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] wrote ASCII JAD {} ({} bytes) class='{}'",
            common::ucs2_to_utf8(jad_path), body.size(), cls);
        return ok;
    }

    static bool restage_to_ams_suite(io_system *io, const app_entry &entry,
        const std::u16string &storage_dir, const std::u16string &storage_name,
        std::u16string &jad_out, std::u16string &jar_out, std::u16string &dir_out) {
        const std::uint32_t uid = java_midlet_uid(entry.id_);
        dir_out = u"C:\\private\\102033E6\\MIDlets\\"
            + common::utf8_to_ucs2(fmt::format("{:08X}", uid)) + u"\\";
        if (!io->create_directories(dir_out)) {
            LOG_ERROR(J2ME, "Can't create AMS suite dir {}", common::ucs2_to_utf8(dir_out));
            return false;
        }

        jar_out = dir_out + u"m.jar";
        jad_out = dir_out + u"m.jad";
        // Copy from per-MIDlet storage, never from the AMS auto-scan preinstall
        // directory. J9 launch clears that folder so AMS -boot will not spawn
        // MIDP2SilentMIDletInstall; a -101 AMS-wait retry must still be able
        // to restage the suite.
        const std::u16string src_jar = storage_dir + storage_name + u".jar";
        const std::u16string src_jad = storage_dir + storage_name + u".jad";
        if (!copy_guest_file(io, src_jar, jar_out, true, true)) {
            LOG_ERROR(J2ME, "Can't copy JAR into AMS suite dir from {}",
                common::ucs2_to_utf8(src_jar));
            return false;
        }
        copy_guest_file(io, src_jad, jad_out, true, false);
        if (!io->exist(jad_out) && !write_jad_from_guest_jar(io, jar_out, jad_out)) {
            LOG_ERROR(J2ME, "No suite JAD and can't synthesize one from {}",
                common::ucs2_to_utf8(jar_out));
            return false;
        }
        rewrite_staged_jad_jar_url(io, jad_out);
        // Persist the descriptor next to the per-MIDlet JAR so a later restage
        // does not have to unzip the manifest again.
        if (!io->exist(src_jad)) {
            copy_guest_file(io, jad_out, src_jad, true, false);
        }
        // J9 also probes C:\private\102033E6\MIDlet (no suite UID) for the
        // AMS-side suite descriptor. Mirror the JAD there so the open succeeds.
        copy_guest_file(io, jad_out, u"C:\\private\\102033E6\\MIDlet", true, false);
        return true;
    }

    static std::u16string midlet_class_from_guest_jad(io_system *io, const std::u16string &jad_path) {
        if (!io || !io->exist(jad_path)) {
            return {};
        }
        symfile jad = io->open_file(jad_path, READ_MODE | BIN_MODE);
        if (!jad) {
            return {};
        }
        std::string content(jad->size(), '\0');
        jad->read_file(content.data(), 1, static_cast<std::uint32_t>(content.size()));
        jad->close();

        const std::string key = "MIDlet-1:";
        const std::size_t pos = content.find(key);
        if (pos == std::string::npos) {
            return {};
        }
        std::size_t line_end = content.find_first_of("\r\n", pos);
        if (line_end == std::string::npos) {
            line_end = content.size();
        }
        std::string val = content.substr(pos + key.size(), line_end - (pos + key.size()));
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        // MIDlet-1: name, icon, class
        const std::size_t c1 = val.find(',');
        if (c1 == std::string::npos) {
            return {};
        }
        const std::size_t c2 = val.find(',', c1 + 1);
        if (c2 == std::string::npos) {
            return {};
        }
        std::string cls = val.substr(c2 + 1);
        while (!cls.empty() && (cls.front() == ' ' || cls.front() == '\t')) {
            cls.erase(cls.begin());
        }
        while (!cls.empty() && (cls.back() == ' ' || cls.back() == '\t')) {
            cls.pop_back();
        }
        if (cls.empty()) {
            return {};
        }
        LOG_WARN(J2ME, "MIDlet-1='{}' class='{}'", val, cls);
        return common::utf8_to_ucs2(cls);
    }

    // j9_23_midp2ams export 2 User::Exit(1) means both
    // RLibrary::Load("MIDP2RuntimeV2.dll") and the V1 fallback failed, or
    // their ordinal-1 factory returned null. Materialize those ROM codesegs
    // so the guest Loader::LoadLibrary path is a cache hit.
    static void preload_j9_runtime_dlls(system *sys) {
        hle::lib_manager *mngr = sys->get_lib_manager();
        if (!mngr) {
            return;
        }
        static const char16_t *DLLS[] = {
            u"Z:\\sys\\bin\\MIDP2RuntimeV2.dll",
            u"Z:\\sys\\bin\\MIDP2JTWIrOneSecurity.dll",
            u"Z:\\sys\\bin\\MIDP2SecurityPolicyV2.dll",
            u"Z:\\sys\\bin\\MIDP2ADT.dll",
            u"Z:\\sys\\bin\\MIDP2DBV2.dll",
            u"Z:\\sys\\bin\\MIDP2Client.dll",
        };
        for (const char16_t *path : DLLS) {
            if (!mngr->load(path)) {
                LOG_WARN(J2ME, "J9 runtime preload failed: {}", common::ucs2_to_utf8(path));
            }
        }
    }

    static void set_java_launch_opaque(epoc::apa::command_line &cmd, const std::uint32_t suite_uid) {
        // StubMIDP2RecogExe TRAP-reads two TInt32s from OpaqueData and passes
        // them to !MIDP.SystemAMS.MIDP2 opcode 1. Empty opaque → KErrEof (-25)
        // and the trampoline exits without sending the launch IPC.
        std::uint32_t fields[2] = { suite_uid, 1 };
        cmd.opaque_data_.assign(reinterpret_cast<const char *>(fields),
            reinterpret_cast<const char *>(fields) + sizeof(fields));
    }

    static std::string bytes_hex_prefix(const std::vector<std::uint8_t> &data, const std::size_t n = 32) {
        std::string hex;
        const std::size_t lim = std::min(data.size(), n);
        hex.reserve(lim * 2);
        for (std::size_t i = 0; i < lim; i++) {
            hex += fmt::format("{:02X}", data[i]);
        }
        return hex;
    }

    static kernel::process *spawn_stubmidp2(system *sys, kernel::process *ams,
        const std::u16string &suite_jad, const std::uint32_t suite_uid) {
        kernel_system *kern = sys->get_kernel_system();
        io_system *io = sys->get_io_system();
        static constexpr char16_t STUB[] = u"Z:\\sys\\bin\\StubMIDP2RecogExe.exe";
        static constexpr std::uint32_t STUB_STACK = 0x10000;

        epoc::apa::command_line cmd;
        cmd.launch_cmd_ = epoc::apa::command_open;
        cmd.executable_path_ = suite_jad;
        cmd.document_name_ = suite_jad;
        set_java_launch_opaque(cmd, suite_uid);

        std::uint64_t jad_size = 0;
        if (symfile jad = io->open_file(suite_jad, READ_MODE | BIN_MODE)) {
            jad_size = jad->size();
            jad->close();
        }

        // Always attach the text cmdline as well. If slot 1 is unused
        // (DataParameterLength == -1) CApaCommandLine falls back to
        // User::CommandLine(); without a document path that path is
        // indistinguishable from "empty opaque → Exit(-25)". Text still
        // cannot carry the two OpaqueData ints, so slot 1 must succeed
        // for the trampoline to send IPC.
        const std::u16string text_cmd = cmd.to_string(true);
        kernel::process *stub = kern->spawn_new_process(STUB, text_cmd, 0, STUB_STACK);
        if (!stub) {
            LOG_WARN(J2ME, "Can't spawn StubMIDP2RecogExe");
            return nullptr;
        }

        std::vector<std::uint8_t> packed = cmd.to_guest_env_slot();
        if (!stub->set_arg_slot(epoc::apa::PROCESS_ENVIRONMENT_ARG_SLOT_MAIN,
                packed.data(), packed.size())) {
            LOG_WARN(J2ME, "Can't put CApaCommandLine in StubMIDP2 env slot 1");
            return nullptr;
        }
        auto slot = stub->get_arg_slot(epoc::apa::PROCESS_ENVIRONMENT_ARG_SLOT_MAIN);
        LOG_WARN(J2ME,
            "StubMIDP2 spawn slot1 used={} bytes={} jad='{}' jad_size={} uid=0x{:08X} hex={} trader={} midp2={}",
            slot && slot->used, packed.size(), common::ucs2_to_utf8(suite_jad),
            jad_size, suite_uid, bytes_hex_prefix(packed),
            java_ams_trader_ready(kern), java_midp2_server_ready(kern));

        if (ams) {
            ams->add_child_process(stub);
        }
        if (!stub->run()) {
            LOG_WARN(J2ME, "Can't run StubMIDP2RecogExe");
            return nullptr;
        }
        return stub;
    }

    // StubMIDP2RecogExe E32Main (ROM 0x81d8ee18):
    //   CApaCommandLine::GetCommandLineFromProcessEnvironment(slot 1)
    //     slot missing → parse RProcess::CommandLine() text ("exe" O "doc")
    //   TRAP-read two TInt32s from OpaqueData (suite uid, midlet index)
    //   RSessionBase::CreateSession("!MIDP.SystemAMS.MIDP2")
    //   if Command()==background: opcode 4, else opcode 1 (those two ints)
    //   User::Exit(IPC result)
    static bool launch_via_stubmidp2(system *sys, kernel::process *ams,
        const std::u16string &suite_jad, const std::uint32_t suite_uid,
        std::function<void(kernel::process *)> exit_cb) {
        io_system *io = sys->get_io_system();
        kernel_system *kern = sys->get_kernel_system();
        static constexpr char16_t STUB[] = u"Z:\\sys\\bin\\StubMIDP2RecogExe.exe";
        g_last_stub_how = "none";
        g_last_stub_reason = 0;

        if (!io->exist(STUB)) {
            g_last_stub_how = "no-exe";
            LOG_WARN(J2ME, "StubMIDP2RecogExe.exe is not in this ROM");
            return false;
        }
        if (suite_jad.empty() || !io->exist(suite_jad)) {
            g_last_stub_how = "no-jad";
            LOG_WARN(J2ME, "StubMIDP2 skipped: suite JAD missing ({})",
                common::ucs2_to_utf8(suite_jad));
            return false;
        }

        auto attach_exit_cb = [&](kernel::process *tracked) {
            if (exit_cb && tracked) {
                tracked->logon([exit_cb](kernel::process *finished) {
                    exit_cb(finished);
                });
            }
        };

        auto wait_for_j9 = [&](kernel::process *stub, const int steps) -> kernel::process * {
            bool logged_stub_exit = false;
            for (int i = 0; i < steps; i++) {
                sys->loop();
                if (kernel::process *vm = find_running_process(kern, "j9midps60")) {
                    LOG_WARN(J2ME, "j9midps60 appeared after {} emulator steps (AMS launch)", i + 1);
                    return vm;
                }
                if (stub && !logged_stub_exit
                    && (stub->get_exit_type() != kernel::entity_exit_type::pending)) {
                    logged_stub_exit = true;
                    g_last_stub_reason = stub->get_exit_reason();
                    auto leftover = stub->get_arg_slot(epoc::apa::PROCESS_ENVIRONMENT_ARG_SLOT_MAIN);
                    LOG_WARN(J2ME,
                        "StubMIDP2 exited type={} reason={} after {} steps; slot1 leftover used={} size={}; continuing to wait for AMS to spawn J9",
                        static_cast<int>(stub->get_exit_type()), g_last_stub_reason, i + 1,
                        leftover && leftover->used,
                        leftover ? leftover->data.size() : 0);
                }
            }
            return nullptr;
        };

        auto finish_without_j9 = [&](kernel::process *stub) -> bool {
            const bool stub_pending = stub
                && (stub->get_exit_type() == kernel::entity_exit_type::pending);
            std::int32_t stub_reason = stub ? stub->get_exit_reason() : g_last_stub_reason;
            // wait_for_j9 records the IPC result, then keeps pumping. By the
            // time we get here the process object may already have been
            // reused and get_exit_reason() reads 0.
            if ((stub_reason == 0) && (g_last_stub_reason != 0)) {
                stub_reason = g_last_stub_reason;
            }
            g_last_stub_reason = stub_reason;
            if (stub_pending) {
                g_launch_should_retry = true;
                g_last_stub_how = "stub-pending";
                LOG_WARN(J2ME, "StubMIDP2 still running but J9 has not appeared; returning retry");
                return false;
            }
            if (stub_reason == 0) {
                g_ams_handoff_pending = true;
                g_ams_handoff_waits = 0;
                g_launch_should_retry = true;
                g_last_stub_how = "handoff";
                LOG_WARN(J2ME,
                    "StubMIDP2 handed off to AMS (exit 0); waiting for j9midps60 instead of direct J9");
                return false;
            }
            // -1 is Connect() to !SystemAMSTrader.Public failing, or AMS
            // returning KErrNotFound. Direct j9midps60 cannot recover from
            // that (export 2 Exit(1)). Keep pumping AMS.
            g_launch_should_retry = true;
            LOG_WARN(J2ME, "StubMIDP2 exited without starting J9: type={} reason={} trader={} midp2={} systemams={}",
                stub ? static_cast<int>(stub->get_exit_type()) : -1, stub_reason,
                java_ams_trader_ready(kern), java_midp2_server_ready(kern),
                guest_server_exists(kern, "!MIDP.SystemAMS.SystemAMS"));
            if (stub_reason == -1 && g_stub_cycle == 0) {
                log_guest_servers(kern, "StubMIDP2 exit -1");
            }
            return false;
        };

        if (kernel::process *existing_vm = find_running_process(kern, "j9midps60")) {
            g_ams_handoff_pending = false;
            g_ams_handoff_waits = 0;
            g_stub_cycle = 0;
            attach_exit_cb(existing_vm);
            return true;
        }

        if (g_ams_handoff_pending) {
            g_ams_handoff_waits++;
            g_last_stub_how = "handoff-wait";
            LOG_WARN(J2ME, "AMS handoff pending; waiting for j9midps60 (attempt {})",
                g_ams_handoff_waits);
            kernel::process *vm = wait_for_j9(find_running_process(kern, "StubMIDP2"), 240);
            if (vm) {
                g_ams_handoff_pending = false;
                g_ams_handoff_waits = 0;
                attach_exit_cb(vm);
                return true;
            }
            if (g_ams_handoff_waits < 8) {
                g_launch_should_retry = true;
                return false;
            }
            g_ams_handoff_pending = false;
            g_ams_handoff_waits = 0;
            LOG_WARN(J2ME, "AMS handoff timed out; J9 never appeared");
            return false;
        }

        if (!stubmidp2_connect_ready(kern)) {
            LOG_WARN(J2ME,
                "StubMIDP2 needs trader+MIDP2 (trader={} midp2={} SystemAMS={}); pumping before spawn",
                java_ams_trader_ready(kern), java_midp2_server_ready(kern),
                guest_server_exists(kern, "!MIDP.SystemAMS.SystemAMS"));
            for (int i = 0; i < 80; i++) {
                sys->loop();
                if (stubmidp2_connect_ready(kern)) {
                    LOG_WARN(J2ME, "StubMIDP2 connect servers ready after {} extra steps", i + 1);
                    break;
                }
            }
            if (!stubmidp2_connect_ready(kern)) {
                g_last_stub_how = java_ams_trader_ready(kern) ? "no-midp2-server" : "no-trader";
                g_launch_should_retry = true;
                if (g_stub_cycle == 0) {
                    log_guest_servers(kern, "StubMIDP2 connect servers missing");
                }
                LOG_WARN(J2ME,
                    "!SystemAMSTrader.Public / !MIDP.SystemAMS.MIDP2 not up yet; returning retry");
                return false;
            }
        }

        if (kernel::process *existing_stub = find_running_process(kern, "StubMIDP2")) {
            g_last_stub_how = "existing-stub";
            LOG_WARN(J2ME, "StubMIDP2 still running; waiting for AMS to spawn J9");
            kernel::process *vm = wait_for_j9(existing_stub, 240);
            if (vm) {
                attach_exit_cb(vm);
                return true;
            }
            return finish_without_j9(existing_stub);
        }

        auto try_stub = [&]() -> bool {
            g_last_stub_how = "slot1";
            kernel::process *stub = spawn_stubmidp2(sys, ams, suite_jad, suite_uid);
            if (!stub) {
                g_last_stub_how = "spawn-slot1-fail";
                return false;
            }
            kernel::process *vm = wait_for_j9(stub, 240);
            if (vm) {
                attach_exit_cb(vm);
                return true;
            }
            finish_without_j9(stub);
            return false;
        };

        // Text command line cannot carry OpaqueData. Host-side opcode 1 from a
        // random guest thread can tear down LLE AMS. Only the authentic
        // StubMIDP2 trampoline (trader ping, then MIDP2 opcode 1) is used.
        if (try_stub()) {
            g_stub_cycle = 0;
            return true;
        }
        if (std::strcmp(g_last_stub_how, "handoff") != 0) {
            g_launch_should_retry = true;
            g_stub_cycle++;
            LOG_WARN(J2ME,
                "StubMIDP2 did not start J9 (how={} reason={}); retry {} (no direct J9)",
                g_last_stub_how, g_last_stub_reason, g_stub_cycle);
        }
        return false;
    }

    static constexpr std::uint32_t J9_STACK_SIZE = 0x40000;

    static kernel::process *spawn_j9_with_ams(system *sys, kernel::process *ams,
        const std::u16string &suite_jad, const std::u16string &suite_jar,
        const std::u16string &suite_dir, const std::u16string &suite_name,
        const std::uint32_t suite_uid) {
        kernel_system *kern = sys->get_kernel_system();
        io_system *io = sys->get_io_system();
        static constexpr char16_t J9[] = u"Z:\\sys\\bin\\j9midps60.exe";
        if (kernel::process *existing = find_running_process(kern, "j9midps60")) {
            LOG_INFO(J2ME, "j9midps60 already running; not spawning a second VM");
            return existing;
        }
        if (!io->exist(J9)) {
            LOG_ERROR(J2ME, "j9midps60.exe is not in this ROM");
            return nullptr;
        }
        // J9 parses -jad/-jar into a TBuf<40>. AMS suite paths are 42 chars
        // (`...\MIDlets\<uid>\m.jar`) and the open becomes `m.ja`.
        //
        // FileServer now shows a second cut: the 25-char
        // `C:/private/102033E6/j.jar` arrives as `C:/private/102` (14 chars,
        // stops at the first non-digit in `102033E6`). The JAD token of the
        // same length opens in full, so this is the JAR-side copy, not Args.
        // Command line therefore uses `C:/j.jad` / `C:/j.jar` (8 chars).
        // `C:\j.jad` is still forbidden: Args.parse sees `\j` after `C:`.
        // Do NOT pass -path.
        static constexpr char16_t PRIV_DIR[] = u"C:\\private\\102033E6\\";
        static constexpr char16_t ROOT_JAD[] = u"C:\\j.jad";
        static constexpr char16_t ROOT_JAR[] = u"C:\\j.jar";
        static constexpr char16_t ROOT_M_JAR[] = u"C:\\m.jar";
        static constexpr char16_t PRIV_JAD[] = u"C:\\private\\102033E6\\j.jad";
        static constexpr char16_t PRIV_JAR[] = u"C:\\private\\102033E6\\j.jar";
        static constexpr char16_t PRIV_M_JAR[] = u"C:\\private\\102033E6\\m.jar";
        static constexpr char16_t TRUNC_JAR[] = u"C:\\private\\102";
        static constexpr char16_t TRUNC_REL_DIR[] = u"C:\\private\\102033E6\\private\\";
        static constexpr char16_t TRUNC_REL_JAR[] = u"C:\\private\\102033E6\\private\\102";
        static constexpr char16_t CMD_JAD[] = u"C:/j.jad";
        static constexpr char16_t CMD_JAR[] = u"C:/j.jar";
        io->create_directories(PRIV_DIR);
        io->create_directories(u"C:\\sys\\bin\\");
        io->create_directories(u"C:\\logs\\java\\");
        stage_j9_jxe_runtime(io);
        // j9vmall fopen/Load "jclcdc11_23.dll"; ROM name is jclcldc11_23.dll.
        static constexpr char16_t ROM_JCL[] = u"Z:\\sys\\bin\\jclcldc11_23.dll";
        copy_guest_file(io, ROM_JCL, u"C:\\sys\\bin\\jclcdc11_23.dll", false, false);
        copy_guest_file(io, ROM_JCL, std::u16string(PRIV_DIR) + u"jclcdc11_23.dll", false, false);
        if (!copy_guest_file(io, suite_jar, PRIV_JAR, true, true) || !io->exist(PRIV_JAR)) {
            LOG_ERROR(J2ME, "Can't copy suite JAR to {}", common::ucs2_to_utf8(PRIV_JAR));
            return nullptr;
        }
        copy_guest_file(io, suite_jar, PRIV_M_JAR, true, false);
        copy_guest_file(io, suite_jar, ROOT_JAR, true, false);
        copy_guest_file(io, suite_jar, ROOT_M_JAR, true, false);
        copy_guest_file(io, suite_jar, TRUNC_JAR, true, false);
        io->create_directories(TRUNC_REL_DIR);
        copy_guest_file(io, suite_jar, TRUNC_REL_JAR, true, false);
        const std::string src_jad_text = read_guest_text(io, suite_jad);
        std::u16string midlet_class = midlet_class_from_guest_jad(io, suite_jad);
        const std::string chinese_name = jad_attr_line(src_jad_text, "MIDlet-Name:");
        if (!write_ascii_runtime_jad(io, PRIV_JAD, PRIV_JAR, common::ucs2_to_utf8(midlet_class))) {
            LOG_ERROR(J2ME, "Can't write ASCII JAD at {}", common::ucs2_to_utf8(PRIV_JAD));
            return nullptr;
        }
        copy_guest_file(io, PRIV_JAD, ROOT_JAD, true, false);

        // findSuiteHome does RFs::Entry on C:\Private\102033E6\<msid>.
        // Args.parse feeds -msid to TLex::Val (decimal): "0x20000004" stops
        // at 'x' and the suite id becomes 0. Stage that home (and the
        // decimal / hex spellings) so Entry succeeds even if the next
        // command line still carries a 0x prefix.
        auto stage_suite_home = [&](const std::u16string &home, const bool explode) {
            io->create_directories(home);
            copy_guest_file(io, PRIV_JAR, home + u"j.jar", true, false);
            copy_guest_file(io, PRIV_JAR, home + u"m.jar", true, false);
            copy_guest_file(io, PRIV_JAD, home + u"j.jad", true, false);
            copy_guest_file(io, PRIV_JAD, home + u"m.jad", true, false);
            if (explode) {
                const int n = explode_jar_to_home(io, PRIV_JAR, home);
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] exploded JAR n={} into '{}'",
                    n, common::ucs2_to_utf8(home));
            }
        };
        const std::u16string msid_dec = common::utf8_to_ucs2(fmt::format("{}", suite_uid));
        const std::u16string msid_hex = common::utf8_to_ucs2(fmt::format("{:x}", suite_uid));
        const std::u16string msid_hex8 = common::utf8_to_ucs2(fmt::format("{:08X}", suite_uid));
        stage_suite_home(std::u16string(PRIV_DIR) + u"0\\", true);
        stage_suite_home(std::u16string(PRIV_DIR) + msid_dec + u"\\", false);
        stage_suite_home(std::u16string(PRIV_DIR) + msid_hex + u"\\", false);
        stage_suite_home(std::u16string(PRIV_DIR) + msid_hex8 + u"\\", false);
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] staged suite homes 0/{}/{}", 
            common::ucs2_to_utf8(msid_dec), common::ucs2_to_utf8(msid_hex8));

        if (!midlet_class.empty()) {
            copy_guest_file(io, PRIV_JAR, std::u16string(PRIV_DIR) + midlet_class + u".jar", true, false);
            copy_guest_file(io, PRIV_JAR, u"C:\\" + midlet_class + u".jar", true, false);
        }
        if (!chinese_name.empty()) {
            bool name_ok = true;
            for (unsigned char c : chinese_name) {
                if ((c == '/') || (c == '\\') || (c == ':')) {
                    name_ok = false;
                    break;
                }
            }
            if (name_ok) {
                const std::u16string cn = common::utf8_to_ucs2(chinese_name);
                copy_guest_file(io, PRIV_JAR, std::u16string(PRIV_DIR) + cn + u".jar", true, false);
                copy_guest_file(io, PRIV_JAR, u"C:\\" + cn + u".jar", true, false);
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] aliased JAR as '{}'.jar", chinese_name);
            }
        }
        const std::u16string jad = PRIV_JAD;
        const std::u16string jar = PRIV_JAR;
        {
            std::uint64_t jad_sz = 0;
            std::uint64_t jar_sz = 0;
            if (symfile jf = io->open_file(jad, READ_MODE | BIN_MODE)) {
                jad_sz = jf->size();
                jf->close();
            }
            if (symfile rf = io->open_file(jar, READ_MODE | BIN_MODE)) {
                jar_sz = rf->size();
                rf->close();
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] short suite jad={} ({} bytes) jar={} ({} bytes) root_jar={} trunc_jar={} rel_trunc={}",
                common::ucs2_to_utf8(jad), jad_sz, common::ucs2_to_utf8(jar), jar_sz,
                io->exist(ROOT_JAR) ? "yes" : "no", io->exist(TRUNC_JAR) ? "yes" : "no",
                io->exist(TRUNC_REL_JAR) ? "yes" : "no");
        }
        // Args.parse: -app takes iAppClassName (next token). A bare "-app"
        // calls usage() → Exit(1). Do NOT pass -event AMS.StartApp here:
        // startApplication(Args) then takes the AMS-event path, which needs
        // a suite already in iApplicationTable (opcode 1 skip leaves it empty)
        // and CMS.run throws → main catch → exit(1).
        (void)suite_name;
        (void)suite_dir;
        // Decimal: TLex::Val on "0x20000004" yields 0 (stops at 'x').
        const std::u16string msid = msid_dec;
        if (midlet_class.empty()) {
            midlet_class = midlet_class_from_guest_jad(io, jad);
        }
        std::u16string args = build_j9midps60_args(suite_uid, midlet_class);
        if (midlet_class.empty()) {
            LOG_WARN(J2ME, "JAD has no MIDlet-1 class; J9 will not startApp");
        } else {
            hle::j9_set_pending_midlet_class(common::ucs2_to_utf8(midlet_class).c_str());
        }
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] args='{}'", common::ucs2_to_utf8(args));

        kernel::process *pr = kern->spawn_new_process(J9, args, 0, J9_STACK_SIZE);
        if (!pr) {
            LOG_ERROR(J2ME, "Can't spawn j9midps60");
            return nullptr;
        }
        if (ams) {
            ams->add_child_process(pr);
        }
        if (!pr->run()) {
            LOG_ERROR(J2ME, "Can't run j9midps60");
            return nullptr;
        }
        LOG_WARN(J2ME, "j9midps60 started as AMS child chars={} args='{}'",
            args.size(), common::ucs2_to_utf8(args));

        bool saw_suite_ams = false;
        for (int i = 0; i < 64; i++) {
            sys->loop();
            if (pr->get_exit_type() != kernel::entity_exit_type::pending) {
                LOG_WARN(J2ME, "j9midps60 exited immediately type={} reason={} after {} steps",
                    static_cast<int>(pr->get_exit_type()), pr->get_exit_reason(), i + 1);
                return pr;
            }
            if (!saw_suite_ams && guest_server_prefix(kern, "MIDletSuiteAMS")) {
                saw_suite_ams = true;
                LOG_WARN(J2ME, "j9midps60 published !MIDletSuiteAMS after {} steps", i + 1);
            }
        }
        LOG_WARN(J2ME, "j9midps60 still running after spawn pump suite_ams={}", saw_suite_ams);
        return pr;
    }

    bool launch_through_midp2(system *sys, const app_entry &entry, std::function<void(kernel::process*)> exit_cb) {
        io_system *io = sys->get_io_system();
        std::u16string storage_dir = build_midp2_storage_dir(entry);
        std::string safe_name = make_safe_preinstall_name(entry.name_);
        std::u16string storage_name = common::utf8_to_ucs2(safe_name);

        std::u16string storage_jar = storage_dir + storage_name + u".jar";
        if (!io->exist(storage_jar)) {
            const std::uint32_t uid = java_midlet_uid(entry.id_);
            const std::u16string uid_hex = common::utf8_to_ucs2(fmt::format("{:08X}", uid));
            const std::u16string fallbacks[] = {
                u"E:\\system\\data\\midp2\\preinstall\\m.jar",
                u"E:\\resource\\java\\preinstall\\m.jar",
                u"C:\\j.jar",
                u"C:\\private\\102033E6\\j.jar",
                u"C:\\private\\102033E6\\MIDlets\\" + uid_hex + u"\\m.jar",
                u"C:\\private\\102033E6\\MIDlets\\" + uid_hex + u"\\j.jar",
            };
            bool found = false;
            for (const std::u16string &cand : fallbacks) {
                if (io->exist(cand)) {
                    storage_dir = eka2l1::file_directory(cand);
                    storage_name = common::utf8_to_ucs2(eka2l1::replace_extension(
                        eka2l1::filename(common::ucs2_to_utf8(cand)), ""));
                    storage_jar = cand;
                    found = true;
                    LOG_WARN(J2ME, "MIDP2 launch: using fallback JAR {}",
                        common::ucs2_to_utf8(cand));
                    break;
                }
            }
            if (!found) {
                LOG_ERROR(J2ME, "MIDP2 launch: JAR not found at {}", common::ucs2_to_utf8(storage_jar));
                return false;
            }
        }

        prepare_midp2_environment(sys);

        std::u16string suite_jad;
        std::u16string suite_jar;
        std::u16string suite_dir;
        if (!restage_to_ams_suite(io, entry, storage_dir, storage_name, suite_jad, suite_jar, suite_dir)) {
            return false;
        }
        {
            const std::u16string cls = midlet_class_from_guest_jad(io, suite_jad);
            if (!cls.empty()) {
                hle::j9_set_pending_midlet_class(common::ucs2_to_utf8(cls).c_str());
            }
        }

        static constexpr char16_t J9_LAUNCHER[] = u"Z:\\sys\\bin\\j9midps60.exe";
        const bool use_j9 = io->exist(J9_LAUNCHER);
        // 5320 J9 launch needs AMS to own the suite. Host-inject into
        // C:\private\102033E6\MIDlets\<uid>\ is not enough: MIDP2 opcode 1
        // looks the uid up in the in-memory AMS list populated by silent
        // install. Stage preinstall bait so SystemAMSCore -boot can register
        // it; only clear that bait after AMS has assigned a real suite uid.
        if (!restage_to_preinstall(io, storage_dir, storage_name)) {
            if (!use_j9) {
                return false;
            }
            LOG_WARN(J2ME, "MIDP2 launch: preinstall restage failed; continuing with AMS suite dir only");
        }

        kernel_system *kern = sys->get_kernel_system();
        kernel::process *ams = boot_java_runtime(sys);
        // A handful of nested slices so a warm AMS can publish its server
        // without a JS round-trip. Cold boot needs the browser main loop.
        if (!wait_for_java_ams_server(sys, 64)) {
            g_launch_should_retry = true;
            LOG_WARN(J2ME, "SystemAMSCore is running but !MIDP.SystemAMS.SystemAMS "
                "is not up yet; returning retry so the emulator can pump AMS");
            return false;
        }

        // 5320 / IBM J9: the real VM is j9midps60.exe. StubMIDP2RecogExe is only
        // an AMS trampoline (CreateSession !MIDP.SystemAMS.MIDP2, opcode 1)
        // and must not be treated as the launched app.
        static const char16_t *LEGACY_LAUNCHERS[] = {
            u"Z:\\sys\\bin\\midp2midletlauncher.exe",
            u"Z:\\sys\\bin\\midletlauncher.exe"
        };

        const char16_t *launcher_path = nullptr;
        std::u16string launcher_args;
        if (use_j9) {
            wait_for_silent_installer(sys, 64);
            preload_j9_runtime_dlls(sys);

            if (kernel::process *existing_vm = find_running_process(kern, "j9midps60")) {
                LOG_INFO(J2ME, "j9midps60 already running; not spawning a second VM");
                stop_java_auto_installer(kern);
                if (exit_cb) {
                    existing_vm->logon([exit_cb = std::move(exit_cb)](kernel::process *finished) {
                        exit_cb(finished);
                    });
                }
                return true;
            }

            // Authentic 5320 path: StubMIDP2 delivers CApaCommandLine to AMS,
            // which then spawns J9. Direct J9 with only -jad/-jar hits
            // User::Exit(1) inside j9_23_midp2ams export 2 (runtime factory).
            const std::uint32_t suite_uid = pick_ams_suite_uid(io, entry);
            if (suite_uid != java_midlet_uid(entry.id_)) {
                const std::u16string ams_dir = u"C:\\private\\102033E6\\MIDlets\\"
                    + common::utf8_to_ucs2(fmt::format("{:08X}", suite_uid)) + u"\\";
                const std::u16string ams_jad = ams_dir + u"m.jad";
                if (io->exist(ams_jad)) {
                    suite_jad = ams_jad;
                }
                // AMS has committed a real uid. Drop preinstall bait so -boot
                // does not spawn another silent installer next retry.
                clear_preinstall_scan_dirs(io);
            }
            if (launch_via_stubmidp2(sys, ams, suite_jad, suite_uid, exit_cb)) {
                stop_java_auto_installer(kern);
                return true;
            }
            // Launch is stubbed so opcode 1 Completes(0) without KERN-EXEC.
            // StubMIDP2 then User::Exit(0). If AMS is still up, spawn J9 as
            // its child (Creator SID check).
            if ((std::strcmp(g_last_stub_how, "handoff") == 0)
                && (java_midp2_server_ready(kern) || java_ams_server_ready(kern))) {
                const std::u16string suite_name = common::utf8_to_ucs2(
                    entry.name_.empty() ? entry.title_ : entry.name_);
                kernel::process *vm = spawn_j9_with_ams(sys, ams, suite_jad, suite_jar,
                    suite_dir, suite_name, suite_uid);
                if (vm) {
                    g_ams_handoff_pending = false;
                    g_ams_handoff_waits = 0;
                    g_launch_should_retry = false;
                    stop_java_auto_installer(kern);
                    if (exit_cb) {
                        vm->logon([exit_cb = std::move(exit_cb)](kernel::process *finished) {
                            exit_cb(finished);
                        });
                    }
                    return true;
                }
            }
            if (!g_launch_should_retry) {
                LOG_WARN(J2ME,
                    "AMS StubMIDP2 launch did not start J9 (how={} reason={}); AMS down, not spawning J9",
                    g_last_stub_how, g_last_stub_reason);
                g_launch_should_retry = true;
            }
            return false;
        } else {
            for (const char16_t *candidate : LEGACY_LAUNCHERS) {
                if (io->exist(candidate)) {
                    launcher_path = candidate;
                    launcher_args = suite_jad.empty()
                        ? (std::u16string(u"E:\\system\\data\\midp2\\preinstall\\") + u"m.jad")
                        : suite_jad;
                    break;
                }
            }
        }

        if (!launcher_path) {
            LOG_ERROR(J2ME, "MIDP2 launch: no Java MIDlet launcher found in ROM");
            return false;
        }

        if (kernel::process *existing_vm = find_running_process(kern, "j9midps60")) {
            LOG_INFO(J2ME, "j9midps60 already running; not spawning a second VM");
            if (exit_cb) {
                existing_vm->logon([exit_cb = std::move(exit_cb)](kernel::process *finished) {
                    exit_cb(finished);
                });
            }
            return true;
        }

        // ROM header gives j9midps60.exe a 16KB stack. The stub immediately
        // loads j9_23_midp2ams.dll (365KB) whose export 2 allocates large
        // frames and brings up the VM — 16KB overflows into KERN-EXEC 3.
        // AppArc uses 64KB minimum; give the JVM 256KB.
        static constexpr std::uint32_t J9_STACK_SIZE = 0x40000;
        kernel::process *pr = kern->spawn_new_process(launcher_path, launcher_args, 0, J9_STACK_SIZE);
        if (!pr) {
            LOG_ERROR(J2ME, "Can't spawn MIDP2 launcher {}", common::ucs2_to_utf8(launcher_path));
            return false;
        }

        // j9midps60 checks User::CreatorSecureId() against SystemAMSCore
        // (SID 0x10203636), same as the silent installer. Without a parent
        // it User::Exit(KErrPermissionDenied / -46).
        if (ams) {
            ams->add_child_process(pr);
        }

        // Keep AMS alive across MIDlet runs; only the VM process is tracked.
        if (exit_cb) {
            pr->logon([exit_cb = std::move(exit_cb)](kernel::process *finished) {
                exit_cb(finished);
            });
        }

        if (!pr->run()) {
            LOG_ERROR(J2ME, "Can't run MIDP2 launcher");
            return false;
        }

        LOG_INFO(J2ME, "MIDP2 launcher started: {} args='{}'", common::ucs2_to_utf8(launcher_path),
            common::ucs2_to_utf8(launcher_args));
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

        std::u16string guest_jad_path;
        if (!stage_midlet_for_launch(sys, app_id, guest_jad_path)) {
            return false;
        }

        kernel_system *kern = sys->get_kernel_system();

        kernel::process *creator = boot_java_runtime(sys);
        if (!creator) {
            LOG_ERROR(J2ME, "reregister: can't spawn SystemAMSCore");
            return false;
        }

        kernel::process *installer = kern->spawn_new_process(SILENT_INSTALLER, guest_jad_path);
        if (!installer) {
            LOG_ERROR(J2ME, "reregister: can't spawn silent installer");
            return false;
        }

        creator->add_child_process(installer);
        installer->logon([exit_cb = std::move(exit_cb)](kernel::process *finished) {
            if (exit_cb) exit_cb(finished);
        });

        if (!installer->run()) {
            LOG_ERROR(J2ME, "reregister: can't run silent installer");
            return false;
        }

        LOG_INFO(J2ME, "MIDlet '{}' reregister: JAR restaged and silent installer started",
            entry.name_);
        return true;
    }

    static bool rewrite_staged_jad_jar_url(io_system *io, const std::u16string &guest_jad_path,
        const char *jar_url) {
        if (!jar_url || !jar_url[0]) {
            jar_url = "m.jar";
        }
        symfile jad_file = io->open_file(guest_jad_path, READ_MODE | BIN_MODE);
        if (!jad_file) {
            return false;
        }

        std::string jad_content;
        jad_content.resize(jad_file->size());
        jad_file->read_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size()));
        jad_file->close();

        const std::string fake_url = "MIDlet-Jar-URL: https://12z1.com/jar/fake.jar";
        const std::string local_url = std::string("MIDlet-Jar-URL: ") + jar_url;
        const std::size_t pos = jad_content.find(fake_url);
        if (pos != std::string::npos) {
            jad_content.replace(pos, fake_url.size(), local_url);
        }

        // Keep a relative jar URL even if the original JAD used a different
        // absolute URL, so the silent installer never tries to download.
        const std::string url_key = "MIDlet-Jar-URL:";
        const std::size_t key_pos = jad_content.find(url_key);
        if (key_pos != std::string::npos && pos == std::string::npos) {
            std::size_t line_end = jad_content.find_first_of("\r\n", key_pos);
            if (line_end == std::string::npos) {
                line_end = jad_content.size();
            }
            jad_content.replace(key_pos, line_end - key_pos, local_url);
        } else if (key_pos == std::string::npos) {
            if (!jad_content.empty() && jad_content.back() != '\n') {
                jad_content += "\r\n";
            }
            jad_content += local_url;
            jad_content += "\r\n";
        }

        symfile out = io->open_file(guest_jad_path, WRITE_MODE | BIN_MODE);
        if (!out) {
            return false;
        }
        out->write_file(jad_content.data(), 1, static_cast<std::uint32_t>(jad_content.size()));
        out->close();
        LOG_WARN(J2ME, "JAD {} MIDlet-Jar-URL -> {}", common::ucs2_to_utf8(guest_jad_path), jar_url);
        return true;
    }

    bool stage_midlet_for_launch(system *sys, const std::uint32_t app_id, std::u16string &jad_path_out) {
        app_list *applist = sys->get_j2me_applist();
        if (!applist) {
            return false;
        }

        std::optional<app_entry> entry_opt = applist->get_entry(app_id);
        if (!entry_opt.has_value()) {
            LOG_ERROR(J2ME, "stage: app_id {} not found", app_id);
            return false;
        }
        const app_entry &entry = entry_opt.value();

        io_system *io = sys->get_io_system();
        const std::u16string storage_dir = build_midp2_storage_dir(entry);
        const std::string safe_name = make_safe_preinstall_name(entry.name_);
        const std::u16string storage_name = common::utf8_to_ucs2(safe_name);
        const std::u16string storage_jar = storage_dir + storage_name + u".jar";

        if (!io->exist(storage_jar)) {
            LOG_ERROR(J2ME, "stage: stored JAR not found at {}", common::ucs2_to_utf8(storage_jar));
            return false;
        }

        prepare_midp2_environment(sys);
        if (!restage_to_preinstall(io, storage_dir, storage_name)) {
            LOG_ERROR(J2ME, "stage: failed to restage JAR to preinstall dir");
            return false;
        }

        static constexpr char16_t PREINSTALL_DIR[] = u"E:\\system\\data\\midp2\\preinstall\\";
        jad_path_out = std::u16string(PREINSTALL_DIR) + u"m.jad";
        rewrite_staged_jad_jar_url(io, jad_path_out);
        rewrite_staged_jad_jar_url(io, u"E:\\resource\\java\\preinstall\\m.jad");
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
                if (!io->exist(storage_dir + storage_name + u".jar")) {
                    symfile src = io->open_file(common::utf8_to_ucs2(path), READ_MODE | BIN_MODE);
                    if (!src) {
                        // path is a host path; copy via raw bytes from fopen.
                        if (FILE *hf = fopen(path.c_str(), "rb")) {
                            fseek(hf, 0, SEEK_END);
                            const long sz = ftell(hf);
                            fseek(hf, 0, SEEK_SET);
                            if (sz > 0) {
                                std::vector<char> buf(static_cast<std::size_t>(sz));
                                if (fread(buf.data(), 1, buf.size(), hf) == buf.size()) {
                                    write_guest_bytes(io, storage_dir + storage_name + u".jar",
                                        buf.data(), buf.size());
                                }
                            }
                            fclose(hf);
                        }
                    } else {
                        src->close();
                    }
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
        // KErrPermissionDenied. Boot the ROM AMS core as the creator and keep
        // it alive for the later J9 launch (do not kill it when the installer
        // exits).
        kernel::process *creator = boot_java_runtime(sys);
        if (!creator) {
            io->delete_entry(guest_jar_path);
            io->delete_entry(guest_jad_path);
            return INSTALL_ERROR_JAVA_INSTALLER_CANT_START;
        }

        // Pass the staged JAD path as the command-line argument. The silent
        // installer reads this to know which MIDlet to install; without it the
        // process boots but has nothing to act on and stalls in its async
        // scheduler waiting for a download/trigger that never arrives.
        kernel::process *installer = kern->spawn_new_process(SILENT_INSTALLER, guest_jad_path);
        if (!installer) {
            io->delete_entry(guest_jar_path);
            io->delete_entry(guest_jad_path);
            return INSTALL_ERROR_JAVA_INSTALLER_CANT_START;
        }

        creator->add_child_process(installer);
        installer->logon([install_exit_cb = std::move(install_exit_cb)](kernel::process *finished) {
            if (install_exit_cb) {
                install_exit_cb(finished);
            }
        });

        if (!installer->run()) {
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
        // A ROM can launch MIDlets via:
        //   1. IBM J9 (j9midps60.exe) — the 5320 path
        //   2. A standalone launcher exe (midp2midletlauncher.exe / midletlauncher.exe)
        //   3. The AppArc trampoline: SystemAMSCore + midp2runtimev2.dll
        //
        // StubMIDP2RecogExe.exe is a file recognizer, NOT a launcher.
        io_system *io = sys->get_io_system();

        static const char16_t *STANDALONE_LAUNCHERS[] = {
            u"Z:\\sys\\bin\\j9midps60.exe",
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
