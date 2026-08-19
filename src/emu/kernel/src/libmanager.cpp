/*
 * Copyright (c) 2018 EKA2L1 Team.
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

#include <common/algorithm.h>
#include <common/armemitter.h>
#include <common/cvt.h>
#include <common/fileutils.h>
#include <common/ini.h>
#include <common/log.h>
#include <common/path.h>
#include <common/random.h>

#include <kernel/common.h>
#include <kernel/libmanager.h>
#include <kernel/reg.h>

#include <common/configure.h>
#include <config/config.h>

#include <loader/e32img.h>
#include <loader/romimage.h>
#include <mem/mem.h>
#include <mem/page.h>
#include <utils/dll.h>
#include <utils/err.h>
#include <utils/sec.h>
#include <vfs/vfs.h>

#include <kernel/chunk.h>
#include <kernel/codeseg.h>
#include <kernel/j9_jni_table.h>
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/thread.h>

#include <cpu/arm_interface.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <string>

// Defined in svc.cpp; debug probe toggled by the frontend.
extern bool eka2l1_leave_probe;

namespace eka2l1::hle {
    static std::array<std::u16string, 2> LDD_SKIP_LOAD_LIST = {
        u"VideoDriver.LDD",
        u"EKeyb.LDD"
    };

    // Given relocation entries, relocate the code and data
    static bool build_relocation_list(const std::vector<loader::e32_reloc_entry> &entries, std::vector<std::uint64_t> &relocation_list, const loader::relocate_section sect) {
        for (std::uint32_t i = 0; i < entries.size(); i++) {
            const loader::e32_reloc_entry &entry = entries[i];

            for (const auto &rel_info : entry.rels_info) {
                // Get the lower 12 bit for virtual_address
                const std::uint32_t virtual_addr = entry.base + (rel_info & 0x0FFF);
                loader::relocation_type rel_type = static_cast<loader::relocation_type>(rel_info & 0xF000);

                relocation_list.push_back((virtual_addr) | (static_cast<std::uint64_t>(rel_type) << 32) | (static_cast<std::uint64_t>(sect) << 48));
            }
        }

        return true;
    }

    static bool build_relocation_list(std::vector<std::uint64_t> &relocation_list, loader::e32img *img) {
        if (!build_relocation_list(img->code_reloc_section.entries, relocation_list, loader::relocate_section::relocate_section_text)) {
            return false;
        }

        if ((img->header.bss_size) || (img->header.data_size)) {
            return build_relocation_list(img->data_reloc_section.entries, relocation_list, loader::relocate_section::relocate_section_data);
        }

        return true;
    }

    static std::string get_real_dll_name(std::string dll_name) {
        const std::string ext = eka2l1::path_extension(dll_name);
        size_t dll_name_end_pos = dll_name.find_first_of("{");

        if (FOUND_STR(dll_name_end_pos)) {
            dll_name = dll_name.substr(0, dll_name_end_pos);
        } else {
            dll_name_end_pos = dll_name.find_last_of("[");

            if (FOUND_STR(dll_name_end_pos)) {
                dll_name = dll_name.substr(0, dll_name_end_pos);
            } else {
                return dll_name;
            }
        }

        return dll_name + (ext.empty() ? ".dll" : ext);
    }

    static bool pe_fix_up_iat(memory_system *mem, hle::lib_manager &mngr, const std::uint32_t iat_offset_from_codebase,
        loader::e32img_import_block &import_block, uint32_t &crr_idx, codeseg_ptr &parent_codeseg) {
        const std::string dll_name8 = get_real_dll_name(import_block.dll_name);
        const std::u16string dll_name = common::utf8_to_ucs2(dll_name8);

        codeseg_ptr cs = mngr.load(dll_name);

        if (!cs) {
            // Skip these ordinals
            LOG_TRACE(KERNEL, "Can't find {}", dll_name8);
            crr_idx += static_cast<std::uint32_t>(import_block.ordinals.size());

            return false;
        }

        kernel::codeseg_dependency_info dependency_info;
        dependency_info.dep_ = cs;

        uint32_t *imdir = &(import_block.ordinals[0]);

        for (std::uint32_t j = 0; j < import_block.ordinals.size(); crr_idx++, j++) {
            dependency_info.import_info_.push_back(kernel::make_import_info(iat_offset_from_codebase + sizeof(address) * crr_idx, import_block.ordinals[j]));
        }

        // Add dependency for the codeseg
        if (!parent_codeseg->add_dependency(dependency_info)) {
            LOG_ERROR(KERNEL, "Fail to add a codeseg as dependency!");
            return false;
        }

        return true;
    }

    static bool elf_fix_up_import_dir(memory_system *mem, hle::lib_manager &mngr, std::uint8_t *code_addr, loader::e32img_import_block &import_block,
        codeseg_ptr &parent_cs) {
        // LOG_INFO(KERNEL, "Fixup for: {}", import_block.dll_name);

        const std::string dll_name8 = get_real_dll_name(import_block.dll_name);
        const std::u16string dll_name = common::utf8_to_ucs2(dll_name8);

        // Use parent drive first
        const std::u16string dll_name_with_drive = eka2l1::root_name(parent_cs->get_full_path(), true) + dll_name;

        codeseg_ptr cs = mngr.load(dll_name_with_drive);

        if (!cs) {
            // Freestyle with the path this time.
            cs = mngr.load(dll_name);

            if (!cs) {
                LOG_TRACE(KERNEL, "Can't find {}", dll_name8);
                return false;
            }
        }

        kernel::codeseg_dependency_info dependency_info;
        dependency_info.dep_ = cs;

        std::uint32_t *imdir = &(import_block.ordinals[0]);

        for (uint32_t i = 0; i < import_block.ordinals.size(); i++) {
            const std::uint32_t off = imdir[i];
            const std::uint32_t *code_ptr = reinterpret_cast<std::uint32_t *>(code_addr + off);

            const std::uint32_t import_inf = *code_ptr;
            const std::uint16_t ord = import_inf & 0xFFFF;
            const std::uint16_t adj = static_cast<std::uint16_t>(import_inf >> 16);

            dependency_info.import_info_.push_back(kernel::make_import_info(off, ord, adj));
        }

        // Add that codeseg as our dependency
        parent_cs->add_dependency(dependency_info);
        return true;
    }

    void apply_j2me_compat_patches(codeseg_ptr cs, const std::string &name_hint);

    static void buildup_import_fixup_table(loader::e32img *img, memory_system *mem, hle::lib_manager &mngr, codeseg_ptr cs) {
        if (img->epoc_ver < epocver::eka2) {
            std::uint32_t track = 0;

            for (auto &ib : img->import_section.imports) {
                pe_fix_up_iat(mem, mngr, img->header.text_size, ib, track, cs);
            }
        } else {
            for (auto &ib : img->import_section.imports) {
                elf_fix_up_import_dir(mem, mngr, reinterpret_cast<std::uint8_t *>(&img->data[img->header.code_offset]), ib, cs);
            }
        }
    }

    static std::string get_e32_codeseg_name_from_path(const std::u16string &path) {
        std::string res = common::lowercase_string(common::ucs2_to_utf8(eka2l1::filename(path)));
        if (!res.empty() && res.back() == '\0') {
            res.pop_back();
        }

        return res;
    }

    static codeseg_ptr import_e32img(loader::e32img *img, memory_system *mem, kernel_system *kern, hle::lib_manager &mngr,
        const std::u16string &path = u"", const address force_code_addr = 0) {
        std::uint32_t data_seg_size = img->header.data_size + img->header.bss_size;
        kernel::codeseg_create_info info;

        info.full_path = path;
        info.uids[0] = static_cast<std::uint32_t>(img->header.uid1);
        info.uids[1] = img->header.uid2;
        info.uids[2] = img->header.uid3;
        info.code_base = img->header.code_base;
        info.data_base = img->header.data_base;
        info.code_size = img->header.code_size;
        info.data_size = img->header.data_size;
        info.text_size = img->header.text_size;
        info.bss_size = img->header.bss_size;
        info.entry_point = img->header.entry_point;
        info.export_table = img->ed.syms;

        if (img->epoc_ver <= epocver::eka2) {
            // The offset of exports have base code at address 0. Add in the code base
            // to each export. EKA2L1's emulated kernel works with export which has an actual base.
            for (auto &exp : info.export_table) {
                exp += info.code_base;
            }
        }

        if (img->has_extended_header) {
            info.sinfo.caps_u[0] = img->header_extended.info.cap1;
            info.sinfo.caps_u[1] = img->header_extended.info.cap2;
            info.sinfo.vendor_id = img->header_extended.info.vendor_id;
            info.sinfo.secure_id = img->header_extended.info.secure_id;

            if (img->has_extended_header && (img->header_extended.exception_des & 1)) {
                info.exception_descriptor = img->header_extended.exception_des - 1;
            } else {
                info.exception_descriptor = 0;
            }
        }

        info.constant_data = reinterpret_cast<std::uint8_t *>(&img->data[img->header.data_offset]);
        info.code_data = reinterpret_cast<std::uint8_t *>(&img->data[img->header.code_offset]);

        // Add relocation info in
        build_relocation_list(info.relocation_list, img);

        if (force_code_addr != 0) {
            info.code_load_addr = force_code_addr;
        }

        codeseg_ptr cs = kern->create<kernel::codeseg>(get_e32_codeseg_name_from_path(path), info);

        if (!cs) {
            LOG_ERROR(KERNEL, "E32 image loading failed!");
            return nullptr;
        }

        apply_j2me_compat_patches(cs, common::ucs2_to_utf8(eka2l1::filename(path)));

        // Build import table so that it can patch later
        buildup_import_fixup_table(img, mem, mngr, cs);
        mngr.try_apply_patch(cs);

        return cs;
    }

    static void patch_rom_export(std::map<address, address> &trampoline_map, memory_system *mem, codeseg_ptr source_seg, codeseg_ptr dest_seg, const std::uint32_t source_export, const std::uint32_t dest_export) {
        if (dest_export == 0) {
            LOG_ERROR(KERNEL, "Export should not have the value of 0!");
            return;
        }

        const address source_ptr = source_seg->lookup(nullptr, source_export);
        const address dest_ptr = dest_seg->lookup(nullptr, dest_export);

        std::uint8_t *dest_ptr_host = reinterpret_cast<std::uint8_t *>(mem->get_real_pointer(dest_ptr & ~1));

        if (!dest_ptr_host) {
            LOG_WARN(KERNEL, "Unable to patch export {} of {} due to export not exist", source_export, source_seg->name());
            return;
        }

        // For some functions that is too small (bx lr etc...) That can't fit the trampoline, use an SVC call and then lookup
        // the PC
        bool resolve_to_trampoline_map = false;

        // Quick check if the size is overlapped
        std::vector<std::uint32_t> export_tables = dest_seg->get_export_table_raw();
        auto will_current_export_resolve_to_trampoline_map = [&](const std::size_t index) -> bool {
            if (export_tables[index] > dest_ptr) {
                if (((dest_ptr & 1) && ((export_tables[index] & ~1) - (dest_ptr & ~1) < sizeof(THUMB_TRAMPOLINE_ASM))) ||
                    (((dest_ptr & 1) == 0) && ((export_tables[index] & ~1) - (dest_ptr & ~1) < sizeof(ARM_TRAMPOLINE_ASM)))) {
                    resolve_to_trampoline_map = true;
                    trampoline_map.emplace(dest_ptr, source_ptr);

                    return true;
                }
            }

            return false;
        };

        // Resolve to find later entries, they likely are neighbor. Of course the end entry will reiterate
        // from the first to find neighbor, but we did try to shorten our searches on others
        for (std::size_t i = dest_export; i < export_tables.size(); i++) {
            if (will_current_export_resolve_to_trampoline_map(i)) {
                break;
            }
        }

        for (std::size_t i = 0; i < dest_export - 1; i++) {
            if (will_current_export_resolve_to_trampoline_map(i)) {
                break;
            }
        }

        if (resolve_to_trampoline_map) {
            if (dest_ptr & 1) {
                *reinterpret_cast<std::uint16_t *>(dest_ptr_host) = 0xDFFF;            // SVC #0xFF
            } else {
                *reinterpret_cast<std::uint32_t *>(dest_ptr_host) = 0xEF0000FF;        // SVC #0xFF
            }
        } else {
            if (dest_ptr & 1) {
                std::memcpy(dest_ptr_host, THUMB_TRAMPOLINE_ASM, sizeof(THUMB_TRAMPOLINE_ASM));

                // It's thumb
                // Hope it's big enough
                if (((dest_ptr & ~1) & 3) == 0) {
                    dest_ptr_host -= 2;
                }

                *reinterpret_cast<std::uint32_t *>(dest_ptr_host + sizeof(THUMB_TRAMPOLINE_ASM)) = source_ptr;
            } else {
                // ARM!!!!!!!!!
                std::memcpy(dest_ptr_host, ARM_TRAMPOLINE_ASM, sizeof(ARM_TRAMPOLINE_ASM));
                *reinterpret_cast<std::uint32_t *>(dest_ptr_host + sizeof(ARM_TRAMPOLINE_ASM)) = source_ptr;
            }
        }
    }

    static void patch_original_codeseg(std::map<address, address> &trampoline_map, std::vector<patch_route_info> &infos, memory_system *mem, codeseg_ptr source_seg,
        codeseg_ptr dest_seg) {
        if (dest_seg->is_rom()) {
            for (auto &rinfo : infos) {
                patch_rom_export(trampoline_map, mem, source_seg, dest_seg, rinfo.first, rinfo.second);
            }
        }

        // Can't set upper since export table in upper loop needs to be in shape and unmodified
        for (auto &rinfo: infos) {
            dest_seg->set_export(rinfo.second, source_seg->lookup(nullptr, rinfo.first));
        }

        dest_seg->set_patched();
        dest_seg->set_entry_point_disabled();
    }

    static void get_route_from_ini_section(common::ini_section &section, std::vector<patch_route_info> &infos) {
        for (auto &pair_node : section) {
            common::ini_pair *pair = pair_node->get_as<common::ini_pair>();
            const std::uint32_t source_export = pair->key_as<std::uint32_t>();

            std::uint32_t dest_export = 0;
            pair->get(&dest_export, 1, 0);

            infos.push_back({ source_export, dest_export });
        }
    }

    static std::string epocver_to_plat_suffix(const epocver ver) {
        switch (ver) {
        case epocver::epoc6:
            return "v6";

        case epocver::epoc81b:
            return "v81b";

        case epocver::epoc81a:
            return "v81a";

        case epocver::epoc80:
            return "v80";

        case epocver::epoc93fp1:
            return "v93fp1";
            
        case epocver::epoc93fp2:
            return "v93fp2";

        case epocver::epoc94:
            return "v94";

        case epocver::epoc95:
            return "v95";

        case epocver::epoc10:
            return "v100";

        default:
            break;
        }

        return "";
    }

    void lib_manager::load_patch_libraries(const std::string &patch_folder) {
        auto iterator = common::make_directory_iterator(patch_folder, "*.map");
        if (!iterator) {
            return;
        }

        common::dir_entry entry;

        patches_.clear();

        std::vector<std::string> patch_image_paths;

        while (iterator->next_entry(entry) == 0) {
            const std::string original_map_name = eka2l1::replace_extension(eka2l1::filename(entry.name), "");
            const std::string patch_map_path = eka2l1::add_path(patch_folder, entry.name);

            epocver start_ver = kern_->get_epoc_version();

            std::string patch_dll_map;

            // Look for map file. These describes the export maps.
            // This function will replace original ROM subroutines with route to these functions.
            common::ini_file map_file_parser;
            map_file_parser.load(patch_map_path.c_str());

            std::string source_dll_name_from_patch = eka2l1::replace_extension(original_map_name, "_");
            common::ini_node_ptr pair_source_node = map_file_parser.find("source");
            if (pair_source_node != nullptr) {
                common::ini_pair *pair = pair_source_node->get_as<common::ini_pair>();
                if (pair != nullptr) {
                    if (pair->get_value_count() >= 1) {
                        std::vector<std::string> sources_dll_list(1);
                        pair->get(sources_dll_list);

                        if (sources_dll_list.size() >= 1) {
                            source_dll_name_from_patch = sources_dll_list[0] + "_";
                        }
                    }
                }
            }

            while (true) {
                if (start_ver >= epocver::epocverend) {
                    break;
                }

                const std::string source_dll_name = source_dll_name_from_patch + epocver_to_plat_suffix(start_ver) + ".dll";
                patch_dll_map = eka2l1::add_path(patch_folder, source_dll_name);

                if (!common::exists(patch_dll_map)) {
                    patch_dll_map.clear();
                    start_ver++;

                    continue;
                }

                LOG_TRACE(KERNEL, "Using dll {} as patch dll for map file {}", source_dll_name, original_map_name);
                break;
            }

            if (patch_dll_map.empty()) {
                const std::string source_dll_name = source_dll_name_from_patch + "general.dll";
                patch_dll_map = eka2l1::add_path(patch_folder, source_dll_name);

                if (!common::exists(patch_dll_map)) {
                    LOG_ERROR(KERNEL, "Can't find suitable patch DLL for map {}", original_map_name);
                    continue;
                }

                LOG_TRACE(KERNEL, "Using general DLL {} as patch DLL for map file {}", source_dll_name, original_map_name);
            }

            patch_image_paths.push_back(patch_dll_map);
            patch_info the_patch;

            the_patch.name_ = original_map_name;
            the_patch.patch_ = nullptr;
            the_patch.req_uid2_ = 0;
            the_patch.req_uid3_ = 0;

            // Get the requirements
            common::ini_section *req_section = map_file_parser.find("requirements")->get_as<common::ini_section>();
            if (req_section) {
                common::ini_pair *u2 = req_section->find("uid2")->get_as<common::ini_pair>();
                if (u2) {
                    u2->get(&the_patch.req_uid2_, 1, 0);
                }

                common::ini_pair *u3 = req_section->find("uid3")->get_as<common::ini_pair>();
                if (u3) {
                    u3->get(&the_patch.req_uid3_, 1, 0);
                }

                if (req_section->find("inrom") != nullptr) {
                    the_patch.need_dest_rom_ = true;
                } else {
                    the_patch.need_dest_rom_ = false;
                }
            } else {
                LOG_TRACE(KERNEL, "Patch {} has no hard requirements", entry.name);
            }

            // Patch out the shared segment first
            common::ini_section *shared_section = map_file_parser.find("shared")->get_as<common::ini_section>();

            if (shared_section) {
                get_route_from_ini_section(*shared_section, the_patch.routes_);
            } else {
                LOG_TRACE(KERNEL, "Shared section not found for patch DLL {}", entry.name);
            }

            const char *alone_section_name = epocver_to_string(kern_->get_epoc_version());

            if (alone_section_name) {
                common::ini_section *indi_section = map_file_parser.find(alone_section_name)->get_as<common::ini_section>();

                if (indi_section) {
                    get_route_from_ini_section(*indi_section, the_patch.routes_);
                } else {
                    LOG_TRACE(KERNEL, "Seperate section not found for epoc version {} of patch DLL {}", static_cast<int>(kern_->get_epoc_version()),
                        entry.name);
                }
            }

            patches_.push_back(the_patch);
        }

        const std::uint32_t last_add_mode = additional_mode_;
        additional_mode_ = 0;

        std::unordered_map<std::string, codeseg_ptr> patch_segs_lookup;

        for (std::size_t i = 0; i < patch_image_paths.size(); i++) {
            codeseg_ptr patch_seg = nullptr;
            auto lookup_result = patch_segs_lookup.find(patch_image_paths[i]);

            if (lookup_result != patch_segs_lookup.end()) {
                patch_seg = lookup_result->second;
            } else {
                // We want to patch ROM image though. Do it.
                auto e32imgfile = eka2l1::physical_file_proxy(patch_image_paths[i], READ_MODE | BIN_MODE);
                eka2l1::ro_file_stream image_data_stream(e32imgfile.get());

                // Try to load them to ROM section
                auto e32img = loader::parse_e32img(reinterpret_cast<common::ro_stream *>(&image_data_stream));

                if (!e32img) {
                    // Ignore.
                    continue;
                }

                // Create the code chunk in ROM
                kernel::chunk *code_chunk = kern_->create<kernel::chunk>(kern_->get_memory_system(), nullptr, "",
                    0, static_cast<eka2l1::address>(e32img->header.code_size), e32img->header.code_size, prot_read_write_exec,
                    kernel::chunk_type::normal, kernel::chunk_access::rom, kernel::chunk_attrib::anonymous);

                if (!code_chunk) {
                    continue;
                }

                // Relocate imports (yes we want to make codeseg object think this is a ROM image)
                const std::uint32_t code_delta = code_chunk->base(nullptr).ptr_address() - e32img->header.code_base;

                for (auto &export_entry : e32img->ed.syms) {
                    export_entry += code_delta;
                }

                memory_system *mem = kern_->get_memory_system();

                // Relocate! Import
                std::memcpy(code_chunk->host_base(), e32img->data.data() + e32img->header.code_offset, e32img->header.code_size);
                patch_seg = import_e32img(&e32img.value(), mem, kern_, *this, common::utf8_to_ucs2(patch_image_paths[i]),
                    code_chunk->base(nullptr).ptr_address());

                patch_segs_lookup.emplace(patch_image_paths[i], patch_seg);

                if (patch_seg) {
                    patch_seg->attach(nullptr, true);
                    patch_seg->unmark();
                }
            }

            if (!patch_seg) {
                continue;
            }

            patches_[i].patch_ = patch_seg;
        }

        additional_mode_ = last_add_mode;

        apply_pending_patches();
        apply_trick_or_treat_algo();
    }

    // Fine-grained instruction byte patches for ROM binaries, applied directly on the
    // live code region right after load (works for ROM/XIP images too - the Z: VFS
    // pseudo-file exposed for such binaries only carries header metadata and is never
    // re-read by the loader, so file-level patches would be silently ineffective).
    //
    // midp2installerplugin.dll (S60v3 MIDP2 installer):
    // 1) Consent/trust helper opens an ifeui confirmation for unsigned MIDlets.
    //    In headless installs nobody can press softkeys, the dialog Leaves, and
    //    SWInst rolls back with KSWInstErrUserCancel (-30471). Stub that helper
    //    to return the affirmative softkey value immediately so ifeui is never
    //    shown. This return is a boolean/button value, not a Symbian error code.
    // 2) Even after a successful (0) consent return the same helper still wrote
    //    leave-info {category=902, code=6}, which the caller also surfaces as
    //    UserCancel. Force the gating `bne` to always branch past that store.
    //
    // ifeui.dll: other installer paths still construct a confirmation dialog and call
    // its RunLD-equivalent, which blocks (then Leave(-25) / UserCancel) with no
    // softkeys. Stub that Run entry to return OK immediately (NOP'ing only the
    // call site after CreateLD left the dialog half-initialized and hung).
    static void apply_installer_compat_patch(codeseg_ptr cs) {
        static const std::uint8_t leave_sig[] = { 0x00, 0x2d, 0xe0, 0x64, 0x03, 0xd1 };
        static const std::uint8_t leave_patch[] = { 0x00, 0x2d, 0xe0, 0x64, 0x03, 0xe0 };

        // push {r4,r5,r6,lr}; movs r4, r0; movs r0, r1; movs r1, #1
        static const std::uint8_t consent_sig[] = { 0x70, 0xb5, 0x04, 0x00, 0x08, 0x00, 0x01, 0x21 };
        // This helper returns the selected query button, not a Symbian error
        // code: zero is cancel and records leave-info {902, 6}; non-zero is
        // consent.  Return 1 to emulate the affirmative softkey.
        // push {r4,r5,r6,lr}; movs r0, #1; pop {r4,r5,r6,pc}
        static const std::uint8_t consent_patch[] = { 0x70, 0xb5, 0x01, 0x20, 0x70, 0xbd };

        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        int applied = 0;

        for (std::uint32_t i = 0; (i + sizeof(consent_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, consent_sig, sizeof(consent_sig)) == 0) {
                std::memcpy(base + i, consent_patch, sizeof(consent_patch));
                LOG_WARN(KERNEL, "{} consent-stub patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                applied++;
                break;
            }
        }

        for (std::uint32_t i = 0; (i + sizeof(leave_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, leave_sig, sizeof(leave_sig)) == 0) {
                std::memcpy(base + i, leave_patch, sizeof(leave_patch));
                LOG_WARN(KERNEL, "{} leave-info patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                applied++;
                break;
            }
        }

        // OTA 901 / SWInst -30472 (KSWInstErrInsufficientMemory).
        // CMidp2InstallerEngine compares (UserHal-style 128KB reserve + extra)
        // against TVolumeInfo.iFree via `iFree - needed; bge ok`. On this ROM the
        // iFree slot the plugin reads is 0 (v1 vs v2 TDriveInfo / name overlay),
        // so every unsigned JAR fails before JavaReg/AppArc commit.
        //   adds r0, r0, r5; ldr r2,[sp,#0x28]; ldr r3,[sp,#0x2c]; asrs r1,r0,#31
        //   blx  __aeabi_lasr-sub; bge ok
        // Force the success branch.
        static const std::uint8_t space_sig[] = {
            0x40, 0x19, 0x0a, 0x9a, 0x0b, 0x9b, 0xc1, 0x17
        };
        for (std::uint32_t i = 0; (i + sizeof(space_sig) + 6) <= code_size; i++) {
            if (std::memcmp(base + i, space_sig, sizeof(space_sig)) != 0) {
                continue;
            }
            // space_sig is followed by a 32-bit blx (4 bytes) then `bge`.
            if ((base[i + 12] == 0x06) && (base[i + 13] == 0xda)) {
                base[i + 13] = 0xe0; // bge -> b
                LOG_WARN(KERNEL, "{} OTA-901 disk-space patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i + 12, run_addr + i + 12);
                applied++;
            }
            // Disk-space fail then `ReportOTA(901); User::Leave`. Swallowing
            // Leave in this DLL lets the engine fall through to the success
            // epilogue instead of surfacing -30472 to SWInst opcode 516.
            if ((i + 0x1C) <= code_size) {
                const std::uint16_t hw1 = static_cast<std::uint16_t>(base[i + 0x18] | (base[i + 0x19] << 8));
                const std::uint16_t hw2 = static_cast<std::uint16_t>(base[i + 0x1A] | (base[i + 0x1B] << 8));
                if (((hw1 & 0xF800) == 0xF000) && ((hw2 & 0xD000) == 0xC000)) {
                    const std::uint32_t s = (hw1 >> 10) & 1;
                    const std::uint32_t j1 = (hw2 >> 13) & 1;
                    const std::uint32_t j2 = (hw2 >> 11) & 1;
                    const std::uint32_t imm10 = hw1 & 0x3FF;
                    const std::uint32_t imm11 = hw2 & 0x7FF;
                    const std::uint32_t i1 = 1u - (j1 ^ s);
                    const std::uint32_t i2 = 1u - (j2 ^ s);
                    std::int32_t imm32 = static_cast<std::int32_t>(
                        (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1));
                    if (s) {
                        imm32 -= (1 << 25);
                    }
                    const std::uint32_t pc = (run_addr + i + 0x18) & ~1u;
                    // Thumb BLX targets ARM; the immediate is 4-byte aligned.
                    const std::uint32_t dest = (pc + 4 + static_cast<std::uint32_t>(imm32)) & ~3u;
                    if ((dest >= run_addr) && ((dest + 4) <= (run_addr + code_size))) {
                        std::uint8_t *stub = base + (dest - run_addr);
                        static const std::uint8_t arm_bx_lr[] = { 0x1E, 0xFF, 0x2F, 0xE1 };
                        if (std::memcmp(stub, arm_bx_lr, sizeof(arm_bx_lr)) != 0) {
                            std::memcpy(stub, arm_bx_lr, sizeof(arm_bx_lr));
                            LOG_WARN(KERNEL, "{} User::Leave stub swallowed at codeseg offset 0x{:X} (run addr 0x{:X})",
                                cs->name(), dest - run_addr, dest);
                            applied++;
                        }
                    }
                }
            }
            break;
        }

        // HandleInstallResult: ldr r1,[r0,#4]; cmp r1,#0; beq ok else ReportOTA+Leave.
        // Force r1=0 so a leftover OTA status cannot abort after the engine
        // has already copied the suite into AMS/JavaReg.
        static const std::uint8_t handle_sig[] = { 0x10, 0xb5, 0x41, 0x68, 0x00, 0x29, 0x10, 0xd0 };
        static const std::uint8_t handle_patch[] = { 0x10, 0xb5, 0x00, 0x21, 0x00, 0x29, 0x10, 0xd0 };
        for (std::uint32_t i = 0; (i + sizeof(handle_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, handle_sig, sizeof(handle_sig)) == 0) {
                std::memcpy(base + i, handle_patch, sizeof(handle_patch));
                LOG_WARN(KERNEL, "{} OTA HandleResult patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                applied++;
                break;
            }
        }

        // MapError switch: cmp r2,#0xa / bne default / ldr r5,=901.
        // Never take the naked-901 branch (integrity miss, missing .drv, etc.).
        static const std::uint8_t ota901_switch_sig[] = { 0x0a, 0x2a, 0x55, 0xd1 };
        for (std::uint32_t i = 0; (i + sizeof(ota901_switch_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, ota901_switch_sig, sizeof(ota901_switch_sig)) == 0) {
                base[i + 3] = 0xe0; // bne -> b (always skip 901)
                LOG_WARN(KERNEL, "{} OTA-901 switch patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                applied++;
                break;
            }
        }

        // End of CMidp2InstallerEngine::Install: after the long work, a status
        // on the stack is compared to two allowed values; anything else
        // ReportOTA(901)+Leave. That is what SWInst opcode 516 surfaces as
        // -30472 even when the suite files were already copied. Fall through
        // to the success epilogue instead.
        //   movs r7, #1; cmp r1, r0; bne fail; add r0, sp, #0x238
        static const std::uint8_t install_fail_sig[] = {
            0x01, 0x27, 0x81, 0x42, 0x1f, 0xd1, 0x8e, 0xa8
        };
        for (std::uint32_t i = 0; (i + sizeof(install_fail_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, install_fail_sig, sizeof(install_fail_sig)) == 0) {
                base[i + 4] = 0x00; // bne fail -> nop
                base[i + 5] = 0xbf;
                LOG_WARN(KERNEL, "{} OTA-901 install-epilogue patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i + 4, run_addr + i + 4);
                applied++;
                break;
            }
        }

        if (!applied) {
            LOG_WARN(KERNEL, "{}: no J2ME installer compat patches matched", cs->name());
        }
    }

    static void apply_integrity_compat_patch(codeseg_ptr cs) {
        // integrityserver opcode 2 looks up <hash>.drv under
        // C:\Private\1028247A\. A first-time unsigned JAR is not in that
        // store, so the TRAP wrapper Completes(-1 / KErrNotFound) and the
        // MIDP2 plugin maps that into OTA 901 / SWInst -30472.
        //   movs r1, r4; movs r0, r6; blx RMessage::Complete
        // Force Complete(0) so "not yet hashed" is treated as a clean miss.
        static const std::uint8_t complete_sig[] = { 0x21, 0x00, 0x30, 0x00, 0x01, 0xf0 };
        static const std::uint8_t complete_patch[] = { 0x00, 0x21, 0x30, 0x00, 0x01, 0xf0 };

        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        for (std::uint32_t i = 0; (i + sizeof(complete_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, complete_sig, sizeof(complete_sig)) == 0) {
                std::memcpy(base + i, complete_patch, sizeof(complete_patch));
                LOG_WARN(KERNEL, "{} integrity Complete(0) patch applied at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                return;
            }
        }
        static const std::uint8_t already[] = { 0x00, 0x21, 0x30, 0x00, 0x01, 0xf0 };
        for (std::uint32_t i = 0; (i + sizeof(already)) <= code_size; i++) {
            if (std::memcmp(base + i, already, sizeof(already)) == 0) {
                LOG_WARN(KERNEL, "{} integrity Complete(0) already applied at codeseg offset 0x{:X}",
                    cs->name(), i);
                return;
            }
        }
        LOG_WARN(KERNEL, "{}: no integrity Complete(0) patch matched", cs->name());
    }

    static void write_thumb_branch(std::uint8_t *at, const address src, const address dest, const bool link) {
        const std::int32_t offset = static_cast<std::int32_t>(dest - (src + 4));
        const std::uint32_t imm32 = static_cast<std::uint32_t>(offset);
        const std::uint32_t S = (imm32 >> 24) & 1u;
        const std::uint32_t I1 = (imm32 >> 23) & 1u;
        const std::uint32_t I2 = (imm32 >> 22) & 1u;
        const std::uint32_t imm10 = (imm32 >> 12) & 0x3FFu;
        const std::uint32_t imm11 = (imm32 >> 1) & 0x7FFu;
        const std::uint32_t J1 = S ^ (1u - I1);
        const std::uint32_t J2 = S ^ (1u - I2);
        const std::uint16_t hw1 = static_cast<std::uint16_t>(0xF000u | (S << 10) | imm10);
        const std::uint16_t hw2 = static_cast<std::uint16_t>(
            ((link ? 0b11u : 0b10u) << 14) | (J1 << 13) | (1u << 12) | (J2 << 11) | imm11);
        at[0] = static_cast<std::uint8_t>(hw1);
        at[1] = static_cast<std::uint8_t>(hw1 >> 8);
        at[2] = static_cast<std::uint8_t>(hw2);
        at[3] = static_cast<std::uint8_t>(hw2 >> 8);
    }

    // Thumb BLX immediate: like BL but bit 12 is clear and the target is ARM
    // (word-aligned). Offset is from Align(PC, 4), not PC.
    static void write_thumb_blx(std::uint8_t *at, const address src, const address dest) {
        const address pc = (src + 4) & ~3u;
        const std::int32_t offset = static_cast<std::int32_t>((dest & ~3u) - pc);
        const std::uint32_t imm32 = static_cast<std::uint32_t>(offset);
        const std::uint32_t S = (imm32 >> 24) & 1u;
        const std::uint32_t I1 = (imm32 >> 23) & 1u;
        const std::uint32_t I2 = (imm32 >> 22) & 1u;
        const std::uint32_t imm10 = (imm32 >> 12) & 0x3FFu;
        const std::uint32_t imm11 = (imm32 >> 1) & 0x7FFu;
        const std::uint32_t J1 = S ^ (1u - I1);
        const std::uint32_t J2 = S ^ (1u - I2);
        const std::uint16_t hw1 = static_cast<std::uint16_t>(0xF000u | (S << 10) | imm10);
        const std::uint16_t hw2 = static_cast<std::uint16_t>(
            (0b11u << 14) | (J1 << 13) | (J2 << 11) | imm11);
        at[0] = static_cast<std::uint8_t>(hw1);
        at[1] = static_cast<std::uint8_t>(hw1 >> 8);
        at[2] = static_cast<std::uint8_t>(hw2);
        at[3] = static_cast<std::uint8_t>(hw2 >> 8);
    }

    static address decode_thumb_bl(const address src, const std::uint8_t *hw) {
        const std::uint16_t hw1 = static_cast<std::uint16_t>(hw[0] | (hw[1] << 8));
        const std::uint16_t hw2 = static_cast<std::uint16_t>(hw[2] | (hw[3] << 8));
        if (((hw1 & 0xF800) != 0xF000) || ((hw2 & 0xD000) != 0xD000)) {
            return 0;
        }
        const std::uint32_t s = (hw1 >> 10) & 1u;
        const std::uint32_t j1 = (hw2 >> 13) & 1u;
        const std::uint32_t j2 = (hw2 >> 11) & 1u;
        const std::uint32_t imm10 = hw1 & 0x3FFu;
        const std::uint32_t imm11 = hw2 & 0x7FFu;
        const std::uint32_t i1 = 1u - (j1 ^ s);
        const std::uint32_t i2 = 1u - (j2 ^ s);
        std::int32_t imm32 = static_cast<std::int32_t>(
            (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1));
        if (s) {
            imm32 -= (1 << 25);
        }
        return (src + 4 + static_cast<address>(imm32)) & ~1u;
    }

    static address decode_thumb_blx(const address src, const std::uint8_t *hw) {
        const std::uint16_t hw1 = static_cast<std::uint16_t>(hw[0] | (hw[1] << 8));
        const std::uint16_t hw2 = static_cast<std::uint16_t>(hw[2] | (hw[3] << 8));
        if (((hw1 & 0xF800) != 0xF000) || ((hw2 & 0xD000) != 0xC000)) {
            return 0;
        }
        const std::uint32_t s = (hw1 >> 10) & 1u;
        const std::uint32_t j1 = (hw2 >> 13) & 1u;
        const std::uint32_t j2 = (hw2 >> 11) & 1u;
        const std::uint32_t imm10 = hw1 & 0x3FFu;
        const std::uint32_t imm11 = hw2 & 0x7FFu;
        const std::uint32_t i1 = 1u - (j1 ^ s);
        const std::uint32_t i2 = 1u - (j2 ^ s);
        std::int32_t imm32 = static_cast<std::int32_t>(
            (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1));
        if (s) {
            imm32 -= (1 << 25);
        }
        return ((src + 4) & ~3u) + static_cast<address>(imm32);
    }

    // Thumb STR/LDR [Rn, #imm] : 0110x imm5 Rn Rt  (Rt is the stored/loaded reg).
    static constexpr std::uint16_t t_str_imm(const unsigned rt, const unsigned rn, const unsigned imm) {
        return static_cast<std::uint16_t>(0x6000u | ((imm >> 2) << 6) | (rn << 3) | rt);
    }
    static constexpr std::uint16_t t_ldr_imm(const unsigned rt, const unsigned rn, const unsigned imm) {
        return static_cast<std::uint16_t>(0x6800u | ((imm >> 2) << 6) | (rn << 3) | rt);
    }
    static constexpr std::uint16_t t_mov_lo(const unsigned rd, const unsigned rm) {
        return static_cast<std::uint16_t>((rm << 3) | rd); // lsls rd, rm, #0
    }
    static constexpr std::uint16_t t_ldrh_imm(const unsigned rt, const unsigned rn, const unsigned imm) {
        return static_cast<std::uint16_t>(0x8800u | ((imm >> 1) << 6) | (rn << 3) | rt);
    }
    static constexpr std::uint16_t t_strh_imm(const unsigned rt, const unsigned rn, const unsigned imm) {
        return static_cast<std::uint16_t>(0x8000u | ((imm >> 1) << 6) | (rn << 3) | rt);
    }
    static constexpr std::uint16_t t_str_reg(const unsigned rt, const unsigned rn, const unsigned rm) {
        return static_cast<std::uint16_t>(0x5000u | (rm << 6) | (rn << 3) | rt);
    }
    static_assert(t_str_imm(1, 0, 4) == 0x6041, "str r1,[r0,#4]");
    static_assert(t_str_imm(0, 5, 0x1c) == 0x61E8, "str r0,[r5,#0x1c]");
    static_assert(t_ldr_imm(0, 0, 0x3c) == 0x6BC0, "ldr r0,[r0,#0x3c]");
    static_assert(t_ldr_imm(0, 4, 0x3c) == 0x6BE0, "ldr r0,[r4,#0x3c]");
    static_assert(t_str_imm(5, 6, 0x64) == 0x6675, "str r5,[r6,#0x64]");
    static_assert(t_str_imm(0, 6, 0x44) == 0x6470, "str r0,[r6,#0x44]");
    static_assert(t_str_imm(0, 6, 0x10) == 0x6130, "str r0,[r6,#0x10]");
    static_assert(t_str_imm(1, 0, 8) == 0x6081, "str r1,[r0,#8]");
    static_assert(t_str_reg(2, 7, 1) == 0x507A, "str r2,[r7,r1]");
    static_assert(t_str_imm(6, 7, 8) == 0x60BE, "str r6,[r7,#8]");
    static_assert(t_str_imm(0, 7, 0x28) == 0x62B8, "str r0,[r7,#0x28]");
    static_assert(t_str_imm(3, 7, 0) == 0x603B, "str r3,[r7]");
    static_assert(t_str_imm(3, 7, 4) == 0x607B, "str r3,[r7,#4]");
    static_assert(t_str_imm(3, 7, 0x24) == 0x627B, "str r3,[r7,#0x24]");
    static_assert(t_ldr_imm(0, 6, 0x44) == 0x6C70, "ldr r0,[r6,#0x44]");
    static_assert(t_str_imm(7, 0, 0x10) == 0x6107, "str r7,[r0,#0x10]");
    static_assert(t_str_imm(7, 0, 0x14) == 0x6147, "str r7,[r0,#0x14]");
    static_assert(t_str_imm(0, 7, 0x38) == 0x63B8, "str r0,[r7,#0x38]");
    static_assert(t_str_imm(0, 7, 0x3c) == 0x63F8, "str r0,[r7,#0x3c]");
    static_assert(t_str_imm(0, 6, 0x70) == 0x6730, "str r0,[r6,#0x70]");
    static_assert(t_str_imm(0, 6, 0x74) == 0x6770, "str r0,[r6,#0x74]");
    static_assert(t_str_imm(0, 6, 0x78) == 0x67B0, "str r0,[r6,#0x78]");
    static_assert(t_str_imm(0, 6, 0x7c) == 0x67F0, "str r0,[r6,#0x7c]");
    // Thumb1 STR/LDR imm5 only covers 0..0x7C. Offset 0xA0 sets bit 11 and
    // becomes LDR — never encode it with t_str_imm.
    static_assert(t_ldrh_imm(4, 2, 0) == 0x8814, "ldrh r4,[r2]");
    static_assert(t_ldrh_imm(5, 2, 0) == 0x8815, "ldrh r5,[r2]");
    static_assert(t_strh_imm(4, 3, 0) == 0x801C, "strh r4,[r3]");
    static_assert(t_strh_imm(5, 3, 0) == 0x801D, "strh r5,[r3]");
    static_assert(t_strh_imm(1, 3, 0) == 0x8019, "strh r1,[r3]");

    [[maybe_unused]] static bool find_zero_cave_from_end(const std::uint8_t *base, const std::uint32_t code_size,
        const std::uint32_t need, const std::uint32_t avoid_off, const std::uint32_t avoid_len,
        std::uint32_t &out_off) {
        if (code_size < need) {
            return false;
        }
        const std::uint32_t avoid_end = avoid_off + avoid_len;
        for (std::int32_t i = static_cast<std::int32_t>((code_size - need) & ~1u); i >= 0; i -= 2) {
            const auto off = static_cast<std::uint32_t>(i);
            if ((off < avoid_end) && ((off + need) > avoid_off)) {
                continue;
            }
            bool ok = true;
            for (std::uint32_t j = 0; j < need; j++) {
                if (base[off + j] != 0) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                out_off = off;
                return true;
            }
        }
        return false;
    }

    static void apply_ams_find_compat_patch(codeseg_ptr cs) {
        // MIDP2 opcode 1 Find walks factory+0x1c's linked list (uid at +0x64).
        // Silent install never commits, so the list stays empty and opcode 1
        // Completes(-1). On miss, NewL(suiteMgr) / SetUid / state=4 / Append /
        // collection+midlet, then Launch (secondary vtable+0x24) can run.
        static const std::uint8_t find_sig[] = {
            0x40, 0x68, 0x04, 0xe0, 0x42, 0x6e, 0x8a, 0x42, 0x03, 0xd0, 0xc0, 0x30
        };
        static const std::uint8_t newl_sig[] = { 0x10, 0xb5, 0x04, 0x00, 0xcc, 0x20 };
        static const std::uint8_t append_sig[] = { 0x42, 0x68, 0x00, 0x2a, 0x03, 0xd0, 0x82, 0x68 };
        static const std::uint8_t wrap_prefix[] = { 0x10, 0xb5, 0xc0, 0x69 };
        static const std::uint8_t load_all_sig[] = {
            0x70, 0xb5, 0x0c, 0x00, 0x05, 0x00, 0x0c, 0x20, 0x98, 0xb0
        };
        static const std::uint8_t coll_ctor_sig[] = { 0x6f, 0x4a, 0x41, 0x60, 0x02, 0x60, 0x70, 0x47 };
        static const std::uint8_t super_ctor_sig[] = { 0x14, 0x4b, 0x03, 0x60, 0x81, 0x60, 0x4c, 0x33 };
        // CMidlet full ctor: push {r3-r7,lr}; movs r5,r2; movs r7,r1; movs r6,#0; movs r2,#2
        static const std::uint8_t mid_ctor_sig[] = { 0xf8, 0xb5, 0x15, 0x00, 0x0f, 0x00, 0x00, 0x26, 0x02, 0x22 };
        // CMidlet collection append: mov r3, r0; push {r4,lr}; mov r4, r1; bl link
        static const std::uint8_t mid_append_sig[] = { 0x03, 0x00, 0x10, 0xb5, 0x0c, 0x00 };
        // CSuite HBufC setters: push {r4,lr}; mov r4, r0; mov r0, r1; blx HBufC::NewL
        static const std::uint8_t hbuf_set_sig[] = { 0x10, 0xb5, 0x04, 0x00, 0x08, 0x00 };

        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        std::uint32_t find_off = ~0u;
        std::uint32_t newl_off = ~0u;
        std::uint32_t append_off = ~0u;
        std::uint32_t wrap_off = ~0u;

        for (std::uint32_t i = 0; (i + sizeof(find_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, find_sig, sizeof(find_sig)) == 0) {
                find_off = i;
                LOG_WARN(KERNEL, "{} AMS Find located at codeseg offset 0x{:X} (run addr 0x{:X})",
                    cs->name(), i, run_addr + i);
                break;
            }
        }
        for (std::uint32_t i = 0; (i + sizeof(newl_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, newl_sig, sizeof(newl_sig)) == 0) {
                newl_off = i;
                break;
            }
        }
        for (std::uint32_t i = 0; (i + sizeof(append_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, append_sig, sizeof(append_sig)) == 0) {
                append_off = i;
                break;
            }
        }

        if (find_off != ~0u) {
            const address find_va = (run_addr + find_off) & ~1u;
            for (std::uint32_t i = 0; (i + 10) <= code_size; i++) {
                if (std::memcmp(base + i, wrap_prefix, sizeof(wrap_prefix)) != 0) {
                    continue;
                }
                if (decode_thumb_bl((run_addr + i + 4) & ~1u, base + i + 4) == find_va) {
                    wrap_off = i;
                    break;
                }
            }
        }

        // Opcode 1 Find walks factory+0x1c. Silent install never commits, so the
        // list is empty and Complete(-1) makes JS retry forever. A bare NewL'd
        // CSuite panics Launch (KERN-EXEC 3) because +0x44 collection is NULL
        // and FindAt ldrs from it. On miss, synthesize a suite with:
        //   NewL(suiteMgr), uid, state=4, Append, collection, midlet index 1
        // so Launch can queue the J9 spawn task. Cave is the unused iterator
        // loop of the boot load-all helper (empty at first boot).
        if ((find_off == ~0u) || (newl_off == ~0u) || (wrap_off == ~0u) || (append_off == ~0u)) {
            LOG_WARN(KERNEL, "{}: AMS Find-on-miss trampoline skipped (find={} newl={} wrap={} append={})",
                cs->name(), find_off, newl_off, wrap_off, append_off);
        } else if ((base[wrap_off] != 0x10) || (base[wrap_off + 1] != 0xb5)) {
            LOG_WARN(KERNEL, "{} AMS Find-on-miss trampoline already applied at codeseg offset 0x{:X}",
                cs->name(), wrap_off);
        } else {
            const address wrap_va = (run_addr + wrap_off) & ~1u;
            const address newl_va = (run_addr + newl_off) & ~1u;
            const address append_va = (run_addr + append_off) & ~1u;
            std::uint32_t thunk_off = ~0u;
            for (std::uint32_t i = 0; (i + 10) <= code_size; i++) {
                if ((base[i] != 0x10) || (base[i + 1] != 0xb5)
                    || (base[i + 2] != 0xc0) || (base[i + 3] != 0x6b)) {
                    continue;
                }
                if (decode_thumb_bl((run_addr + i + 4) & ~1u, base + i + 4) == wrap_va) {
                    thunk_off = i;
                    break;
                }
            }

            const address alloc_va = decode_thumb_blx((run_addr + newl_off + 6) & ~1u, base + newl_off + 6);

            std::uint32_t coll_ctor_off = ~0u;
            std::uint32_t super_ctor_off = ~0u;
            std::uint32_t mid_ctor_off = ~0u;
            std::uint32_t mid_append_off = ~0u;
            std::uint32_t jad_set_off = ~0u;
            std::uint32_t jar_set_off = ~0u;
            std::uint32_t hbuf_newl_int_off = ~0u;
            std::uint32_t load_off = ~0u;
            for (std::uint32_t i = 0; (i + 8) <= code_size; i += 4) {
                const std::uint32_t w = static_cast<std::uint32_t>(base[i] | (base[i + 1] << 8)
                    | (base[i + 2] << 16) | (base[i + 3] << 24));
                const std::uint32_t t = static_cast<std::uint32_t>(base[i + 4] | (base[i + 5] << 8)
                    | (base[i + 6] << 16) | (base[i + 7] << 24));
                if ((w == 0xE51FF004u) && (t == 0x801B0357u)) {
                    hbuf_newl_int_off = i; // HBufC::NewL(TInt) ARM veneer
                    break;
                }
            }
            for (std::uint32_t i = 0; (i + sizeof(coll_ctor_sig)) <= code_size; i++) {
                if (std::memcmp(base + i, coll_ctor_sig, sizeof(coll_ctor_sig)) == 0) {
                    coll_ctor_off = i;
                    break;
                }
            }
            for (std::uint32_t i = 0; (i + sizeof(super_ctor_sig)) <= code_size; i++) {
                if (std::memcmp(base + i, super_ctor_sig, sizeof(super_ctor_sig)) == 0) {
                    super_ctor_off = i;
                    break;
                }
            }
            for (std::uint32_t i = 0; (i + sizeof(mid_ctor_sig)) <= code_size; i++) {
                if (std::memcmp(base + i, mid_ctor_sig, sizeof(mid_ctor_sig)) == 0) {
                    mid_ctor_off = i;
                    break;
                }
            }
            for (std::uint32_t i = 0; (i + sizeof(mid_append_sig) + 4) <= code_size; i++) {
                if (std::memcmp(base + i, mid_append_sig, sizeof(mid_append_sig)) == 0) {
                    mid_append_off = i;
                    break;
                }
            }
            for (std::uint32_t i = 0; (i + 12) <= code_size; i++) {
                if (std::memcmp(base + i, hbuf_set_sig, sizeof(hbuf_set_sig)) != 0) {
                    continue;
                }
                if ((base[i + 10] == 0x20) && (base[i + 11] == 0x67)) {
                    jad_set_off = i; // str r0, [r4, #0x70]
                } else if ((base[i + 10] == 0x60) && (base[i + 11] == 0x67)) {
                    jar_set_off = i; // str r0, [r4, #0x74]
                }
            }
            for (std::uint32_t i = 0; (i + sizeof(load_all_sig)) <= code_size; i++) {
                if (std::memcmp(base + i, load_all_sig, sizeof(load_all_sig)) == 0) {
                    load_off = i;
                    break;
                }
            }

            std::uint32_t cave_off = ~0u;
            std::uint32_t cave_cap = 0;
            std::uint32_t epi_off = ~0u;
            std::uint32_t skip_off = ~0u;
            if (load_off != ~0u) {
                const std::uint32_t search_hi = std::min(code_size, load_off + 0x40);
                for (std::uint32_t i = load_off; (i + 2) <= search_hi; i += 2) {
                    if ((base[i] == 0xe8) && (base[i + 1] == 0x61)) {
                        // str r0, [r5, #0x1c] at i. List ctor only writes the vptr,
                        // so +4/+8 (head/tail) are leftover heap (often the JAD
                        // HBufC). Find walks that garbage and AMS KERN-EXEC 3.
                        skip_off = i + 2;
                        break;
                    }
                }
                if (skip_off != ~0u) {
                    const std::uint32_t epi_hi = std::min(code_size, load_off + 0x200);
                    for (std::uint32_t i = skip_off + 4; (i + 4) <= epi_hi; i += 2) {
                        if ((base[i] == 0x18) && (base[i + 1] == 0xb0)
                            && (base[i + 2] == 0x70) && (base[i + 3] == 0xbd)) {
                            epi_off = i;
                            break;
                        }
                    }
                    if (epi_off != ~0u) {
                        // movs r1,#0 / str r1,[r0,#4] / str r1,[r0,#8] / b epi
                        cave_off = skip_off + 8;
                        if (cave_off < epi_off) {
                            cave_cap = epi_off - cave_off;
                        }
                    }
                }
            }

            const bool have_helpers = (thunk_off != ~0u) && alloc_va
                && (coll_ctor_off != ~0u) && (cave_cap >= 0x80);
            (void)hbuf_newl_int_off;

            std::uint16_t skip16 = 0;
            if ((epi_off != ~0u) && (skip_off != ~0u)) {
                const address skip_b_va = (run_addr + skip_off + 6) & ~1u;
                const address epi_va = (run_addr + epi_off) & ~1u;
                const std::int32_t imm11 = static_cast<std::int32_t>(epi_va - (skip_b_va + 4)) / 2;
                if ((imm11 >= -1024) && (imm11 <= 1023)) {
                    skip16 = static_cast<std::uint16_t>(0xE000u | (imm11 & 0x7FF));
                }
            }
            const bool skip_already = (skip_off != ~0u)
                && (base[skip_off] == 0x00) && (base[skip_off + 1] == 0x21)
                && (base[skip_off + 2] == 0x41) && (base[skip_off + 3] == 0x60);

            if (skip_already) {
                LOG_WARN(KERNEL, "{} AMS Find-on-miss trampoline already applied at 0x{:X}",
                    cs->name(), (run_addr + cave_off) & ~1u);
            } else if (!have_helpers || !skip16) {
                LOG_WARN(KERNEL, "{}: AMS Find-on-miss trampoline skipped (thunk={} alloc={} "
                    "coll={} cave={}/{} skip16={:04X})",
                    cs->name(), thunk_off, alloc_va, coll_ctor_off, cave_off, cave_cap, skip16);
            } else {
                const address cave_va = (run_addr + cave_off) & ~1u;
                const address thunk_va = (run_addr + thunk_off) & ~1u;
                const address coll_ctor_va = (run_addr + coll_ctor_off) & ~1u;
                const address epi_va = (run_addr + epi_off) & ~1u;
                (void)hbuf_newl_int_off;
                (void)super_ctor_off;
                (void)mid_ctor_off;
                (void)mid_append_off;
                (void)jad_set_off;
                (void)jar_set_off;

                std::uint8_t tmp[0x100];
                struct thumb_emit {
                    std::uint8_t *p;
                    address va;
                    std::uint32_t used;
                    std::uint32_t cap;
                    void t16(const std::uint16_t hw) {
                        p[used] = static_cast<std::uint8_t>(hw);
                        p[used + 1] = static_cast<std::uint8_t>(hw >> 8);
                        used += 2;
                        va += 2;
                    }
                    void bl(const address dest) {
                        write_thumb_branch(p + used, va, dest, true);
                        used += 4;
                        va += 4;
                    }
                    void blx(const address dest) {
                        write_thumb_blx(p + used, va, dest);
                        used += 4;
                        va += 4;
                    }
                } em { tmp, cave_va, 0, static_cast<std::uint32_t>(sizeof(tmp)) };

                em.t16(0xB5F0);                          // push {r4,r5,r6,r7,lr}
                em.t16(t_mov_lo(4, 0));                  // movs r4, r0  suiteMgr
                em.t16(t_mov_lo(5, 1));                  // movs r5, r1  uid
                em.t16(t_ldr_imm(0, 4, 0x3c));           // ldr r0, [r4, #0x3c] factory
                em.bl(wrap_va);                          // original Find
                em.t16(0x2800);                          // cmp r0, #0
                const std::uint32_t bne_at = em.used;
                em.t16(0xD100);                          // bne done (patched below)
                em.t16(t_mov_lo(0, 4));                  // movs r0, r4
                em.bl(newl_va);                          // CSuite::NewL(suiteMgr)
                em.t16(t_mov_lo(6, 0));                  // movs r6, r0
                em.t16(t_str_imm(5, 6, 0x64));           // str r5, [r6, #0x64] uid
                em.t16(0x2004);                          // movs r0, #4
                em.t16(t_str_imm(0, 6, 0x10));           // str r0, [r6, #0x10] state=installed
                em.t16(0x2000);                          // movs r0, #0
                em.t16(t_str_imm(0, 6, 0x14));           // str r0, [r6, #0x14]
                em.t16(t_str_imm(0, 6, 0x18));           // str r0, [r6, #0x18]
                em.t16(t_str_imm(0, 6, 0x48));           // str r0, [r6, #0x48]
                em.t16(t_mov_lo(1, 6));                  // movs r1, r6
                em.t16(0x31C0);                          // adds r1, #0xc0
                em.t16(t_str_imm(0, 1, 8));              // str r0, [r1, #8] next=NULL at +0xc8
                em.t16(t_ldr_imm(0, 4, 0x3c));           // ldr r0, [r4, #0x3c]
                em.t16(t_ldr_imm(0, 0, 0x1c));           // ldr r0, [r0, #0x1c] list
                em.t16(t_mov_lo(1, 6));                  // movs r1, r6
                em.bl(append_va);
                em.t16(0x2018);                          // movs r0, #0x18
                em.blx(alloc_va);
                em.t16(t_mov_lo(7, 0));                  // movs r7, r0
                em.t16(t_mov_lo(0, 7));                  // movs r0, r7
                em.t16(t_mov_lo(1, 6));                  // movs r1, r6
                em.bl(coll_ctor_va);
                em.t16(t_str_imm(0, 6, 0x44));           // str r0, [r6, #0x44]
                em.t16(0x208C);                          // movs r0, #0x8c  CMidlet
                em.blx(alloc_va);
                em.t16(t_mov_lo(7, 0));                  // movs r7, r0
                const std::uint32_t vptr_ldr_at = em.used;
                em.t16(0x4B00);                          // ldr r3, [pc, #lit] CMidlet vptr
                em.t16(t_str_imm(3, 7, 0));              // str r3, [r7]
                em.t16(0x3358);                          // adds r3, #0x58
                em.t16(t_str_imm(3, 7, 4));              // str r3, [r7, #4]
                em.t16(0x3314);                          // adds r3, #0x14
                em.t16(t_str_imm(3, 7, 0x24));           // str r3, [r7, #0x24]
                em.t16(t_str_imm(6, 7, 8));              // str r6, [r7, #8] parent
                em.t16(0x2002);                          // movs r0, #2
                em.t16(t_str_imm(0, 7, 0x10));           // str r0, [r7, #0x10] flags
                em.t16(0x2001);                          // movs r0, #1
                em.t16(t_str_imm(0, 7, 0x28));           // str r0, [r7, #0x28] index
                em.t16(0x2003);                          // movs r0, #3  TBuf type EBuf
                em.t16(0x0700);                          // lsls r0, r0, #28
                em.t16(t_str_imm(0, 7, 0x38));           // str r0, [r7, #0x38]
                em.t16(0x2020);                          // movs r0, #0x20
                em.t16(t_str_imm(0, 7, 0x3c));           // str r0, [r7, #0x3c] maxLength
                // JAD/JAR HBufC fill is not needed while Launch is stubbed
                // (opcode 1 Completes(0) without reading suite paths).
                em.t16(t_ldr_imm(0, 6, 0x44));           // ldr r0, [r6, #0x44] collection
                em.t16(t_str_imm(7, 0, 0x10));           // str r7, [r0, #0x10] head
                em.t16(t_str_imm(7, 0, 0x14));           // str r7, [r0, #0x14] tail
                em.t16(t_mov_lo(1, 7));                  // movs r1, r7
                em.t16(0x3180);                          // adds r1, #0x80
                em.t16(0x2000);                          // movs r0, #0
                em.t16(t_str_imm(0, 1, 8));              // str r0, [r1, #8] next=NULL
                em.t16(t_mov_lo(0, 6));                  // movs r0, r6
                const std::uint32_t done_at = em.used;
                em.t16(0xBDF0);                          // pop {r4,r5,r6,r7,pc}

                if (em.used & 2u) {
                    em.t16(0xBF00);
                }
                const std::uint32_t lit_vptr_at = em.used;
                em.t16(0x86B0);
                em.t16(0x81D5);                          // 0x81D586B0 CMidlet vtable (even)

                auto patch_ldr_pc = [&](const std::uint32_t ldr_off, const std::uint32_t lit_off, const unsigned rt) {
                    const address ldr_va = cave_va + ldr_off;
                    const address lit_va = cave_va + lit_off;
                    const address pc_base = (ldr_va + 4) & ~3u;
                    const std::int32_t delta = static_cast<std::int32_t>(lit_va - pc_base);
                    if ((delta < 0) || (delta > (255 * 4)) || (delta & 3)) {
                        LOG_ERROR(KERNEL, "{} AMS ldr pc literal misaligned ldr=0x{:X} lit=0x{:X}",
                            cs->name(), ldr_va, lit_va);
                        return;
                    }
                    const std::uint16_t hw = static_cast<std::uint16_t>(0x4800u | (rt << 8) | static_cast<unsigned>(delta / 4));
                    tmp[ldr_off] = static_cast<std::uint8_t>(hw);
                    tmp[ldr_off + 1] = static_cast<std::uint8_t>(hw >> 8);
                };
                patch_ldr_pc(vptr_ldr_at, lit_vptr_at, 3);

                const std::int32_t bne_imm = static_cast<std::int32_t>(done_at - (bne_at + 4)) / 2;
                tmp[bne_at] = static_cast<std::uint8_t>(bne_imm & 0xFF);
                tmp[bne_at + 1] = 0xD1;

                if (em.used > cave_cap) {
                    LOG_ERROR(KERNEL, "{} AMS trampoline overflow {} > {}", cs->name(), em.used, cave_cap);
                } else {
                    // Zero list head/tail, then 16-bit B to epilogue. Do not use
                    // B.W: this DLL has almost none, and a 32-bit skip fell
                    // through into the cave. load_all's `bne body` also has to
                    // be retargeted off the iterator (it lands in the cave).
                    base[skip_off + 0] = 0x00;
                    base[skip_off + 1] = 0x21; // movs r1, #0
                    const std::uint16_t z4 = t_str_imm(1, 0, 4);
                    const std::uint16_t z8 = t_str_imm(1, 0, 8);
                    base[skip_off + 2] = static_cast<std::uint8_t>(z4);
                    base[skip_off + 3] = static_cast<std::uint8_t>(z4 >> 8);
                    base[skip_off + 4] = static_cast<std::uint8_t>(z8);
                    base[skip_off + 5] = static_cast<std::uint8_t>(z8 >> 8);
                    base[skip_off + 6] = static_cast<std::uint8_t>(skip16);
                    base[skip_off + 7] = static_cast<std::uint8_t>(skip16 >> 8);
                    if ((epi_off >= 6) && (base[epi_off - 6] == 0x87) && (base[epi_off - 5] == 0xd1)) {
                        const address loop_va = (run_addr + epi_off - 6) & ~1u;
                        const std::int32_t loop_imm = static_cast<std::int32_t>(epi_va - (loop_va + 4)) / 2;
                        const std::uint16_t loop_b = static_cast<std::uint16_t>(0xE000u | (loop_imm & 0x7FF));
                        base[epi_off - 6] = static_cast<std::uint8_t>(loop_b);
                        base[epi_off - 5] = static_cast<std::uint8_t>(loop_b >> 8);
                    }
                    std::memcpy(base + cave_off, tmp, em.used);
                    write_thumb_branch(base + thunk_off + 2, thunk_va + 2, cave_va, true);
                    base[thunk_off + 6] = 0x00;
                    base[thunk_off + 7] = 0xbf;
                    LOG_WARN(KERNEL, "{} AMS Find-on-miss trampoline at 0x{:X} ({} bytes, thunk 0x{:X}, mid+launchstub)",
                        cs->name(), cave_va, em.used, thunk_va);
                }
            }

            // Opcode 1 Find/Launch still KERN-EXEC 3s inside FillZ/memset
            // (lr=0x801B18C3, pc=heap/message). After `str r4,[sp,#0x10]`
            // skip to Complete(0) at +0x22 so AMS stays up; host spawns J9.
            // Distinguisher vs opcode 2/3: Find miss is `beq +0x14` (0ad0).
            static const std::uint8_t op1_sig[] = {
                0x04, 0x94, 0xc0, 0x6e, 0x01, 0x68, 0x49, 0x68,
                0x88, 0x47, 0x02, 0x68, 0x04, 0x99, 0xd2, 0x69,
                0x89, 0x68, 0x90, 0x47, 0x00, 0x28, 0x0a, 0xd0
            };
            bool op1_skipped = false;
            for (std::uint32_t i = 0; (i + sizeof(op1_sig)) <= code_size; i += 2) {
                if ((base[i] == 0x04) && (base[i + 1] == 0x94)
                    && (base[i + 2] == 0x0e) && (base[i + 3] == 0xe0)
                    && (std::memcmp(base + i + 4, op1_sig + 4, sizeof(op1_sig) - 4) == 0)) {
                    LOG_WARN(KERNEL, "{} AMS opcode 1 already Complete(0) at 0x{:X}",
                        cs->name(), (run_addr + i) & ~1u);
                    op1_skipped = true;
                    break;
                }
                if (std::memcmp(base + i, op1_sig, sizeof(op1_sig)) != 0) {
                    continue;
                }
                // b Complete(0): (0x22 - 6)/2 = 14 = 0xE00E
                base[i + 2] = 0x0e;
                base[i + 3] = 0xe0;
                LOG_WARN(KERNEL, "{} AMS opcode 1 Complete(0) skip at 0x{:X} (keep AMS alive for J9)",
                    cs->name(), (run_addr + i) & ~1u);
                op1_skipped = true;
                break;
            }
            if (!op1_skipped) {
                LOG_WARN(KERNEL, "{}: AMS opcode 1 Complete(0) skip not found", cs->name());
            }

            // Launch (vtable+0x24) NewL/Copy still KERN-EXEC 3s on the
            // synthesized suite. Stub it so opcode 1 Completes(0) and AMS
            // stays up; host then spawns j9midps60 as an AMS child.
            // 12-byte sig is unique: the other push/{r4,r5,r6,lr}; movs r4,r0;
            // movs r5,r1; movs r0,#0x14 sites BLX a different veneer.
            static const std::uint8_t launch_sig[] = {
                0x70, 0xb5, 0x04, 0x00, 0x0d, 0x00, 0x14, 0x20, 0x12, 0xf0, 0xe2, 0xeb
            };
            static const std::uint8_t launch_stubbed_sig[] = {
                0x00, 0x20, 0x70, 0x47, 0x0d, 0x00, 0x14, 0x20, 0x12, 0xf0, 0xe2, 0xeb
            };
            bool launch_stubbed = false;
            for (std::uint32_t i = 0; (i + sizeof(launch_sig)) <= code_size; i += 2) {
                if (std::memcmp(base + i, launch_stubbed_sig, sizeof(launch_stubbed_sig)) == 0) {
                    LOG_WARN(KERNEL, "{} AMS Launch already stubbed at 0x{:X}",
                        cs->name(), (run_addr + i) & ~1u);
                    launch_stubbed = true;
                    break;
                }
                if (std::memcmp(base + i, launch_sig, sizeof(launch_sig)) != 0) {
                    continue;
                }
                base[i + 0] = 0x00;
                base[i + 1] = 0x20; // movs r0, #0
                base[i + 2] = 0x70;
                base[i + 3] = 0x47; // bx lr
                LOG_WARN(KERNEL, "{} AMS Launch stubbed at 0x{:X} (keep AMS alive for J9)",
                    cs->name(), (run_addr + i) & ~1u);
                launch_stubbed = true;
                break;
            }
            if (!launch_stubbed) {
                LOG_WARN(KERNEL, "{}: AMS Launch stub not found (code_size=0x{:X})",
                    cs->name(), code_size);
            }
        }

        if (find_off == ~0u) {
            LOG_WARN(KERNEL, "{}: no AMS Find signature matched", cs->name());
        }
    }

    static void apply_ifeui_compat_patch(codeseg_ptr cs) {
        // Confirmation Run-equivalent entry (unique in this ROM image):
        //   push {r0,r4,r5,r6,r7,lr}; movs r1, #0; mvns r1, r1; movs r7, r0
        // Stub to: movs r0, #1; bx lr  (pretend softkey OK, no UI wait).
        // Returning immediately avoids both UserCancel (-30471) and the hang that
        // happens if we only NOP the call site after CreateLD.
        static const std::uint8_t run_sig[] = { 0xf1, 0xb5, 0x00, 0x21, 0xc9, 0x43, 0x07, 0x00 };
        static const std::uint8_t run_stub[] = { 0x01, 0x20, 0x70, 0x47 };

        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        int applied = 0;

        for (std::uint32_t i = 0; (i + sizeof(run_sig)) <= code_size; i++) {
            if (std::memcmp(base + i, run_sig, sizeof(run_sig)) != 0) {
                continue;
            }
            std::memcpy(base + i, run_stub, sizeof(run_stub));
            LOG_WARN(KERNEL, "{} auto-accept patch applied at codeseg offset 0x{:X} (Run stubbed OK, run addr 0x{:X})",
                cs->name(), i, run_addr + i);
            applied++;
            break;
        }

        if (!applied) {
            LOG_WARN(KERNEL, "{}: no J2ME ifeui auto-accept patch matched", cs->name());
        }
    }

    static void apply_silent_midlet_installer_compat_patch(codeseg_ptr cs) {
        // The S60v3 release build of MIDP2SilentMIDletInstall hard-codes
        // TInstallOptions::iUntrusted to EPolicyNotAllowed.  Consequently every
        // unsigned MIDlet is rejected by SWInst before the Java/AppArc
        // registration phase, even though this executable is specifically the
        // unattended preinstaller.  The corresponding debug build selects
        // EPolicyAllowed instead.
        //
        //   cmp  r0, #0
        //   bne  use_allowed
        //   movs r1, #1       ; EPolicyNotAllowed
        //   b    store
        // use_allowed:
        //   movs r1, #0       ; EPolicyAllowed
        //
        // Make the release-build arm select EPolicyAllowed as well.  Match the
        // complete conditional sequence so the patch cannot hit an unrelated
        // `movs r1, #1` instruction.
        // Match the cmp/bne/movs-r1-#1 prefix. The following `b` encoding is
        // not stable across reloc/XIP copies; only the policy immediate is.
        static const std::uint8_t untrusted_policy_prefix[] = {
            0x00, 0x28, 0x01, 0xd1, 0x01, 0x21
        };

        std::uint8_t *base = nullptr;
        address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (run_addr) {
            run_addr &= ~1u;
        }
        if ((!base || !run_addr) && cs->get_code_base()) {
            kernel_system *kern = cs->get_kernel_object_owner();
            if (kern && kern->get_memory_system()) {
                run_addr = cs->get_code_base() & ~1u;
                base = reinterpret_cast<std::uint8_t *>(kern->get_memory_system()->get_real_pointer(run_addr));
            }
        }
        if (!run_addr || !base) {
            LOG_WARN(KERNEL, "{}: unsigned-MIDlet policy patch skipped (no mapped code)", cs->name());
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        std::uint32_t search_size = code_size;
        if (cs->is_rom() && (search_size < 0x800)) {
            search_size = 0x2000;
        }

        auto apply_at = [&](const std::uint32_t off) {
            base[off + 4] = 0x00; // movs r1, #0  (EPolicyAllowed)
            LOG_WARN(KERNEL, "{} unsigned-MIDlet policy patch applied at codeseg offset 0x{:X} (run addr 0x{:X} size={})",
                cs->name(), off, run_addr + off, code_size);
        };

        for (std::uint32_t i = 0; (i + sizeof(untrusted_policy_prefix)) <= search_size; i++) {
            if (std::memcmp(base + i, untrusted_policy_prefix, sizeof(untrusted_policy_prefix)) == 0) {
                apply_at(i);
                return;
            }
        }

        // 5320 MIDP2SilentMIDletInstall.exe: sequence is at code+0x3C2.
        if ((search_size > 0x3C8)
            && (base[0x3C2] == 0x00) && (base[0x3C3] == 0x28)
            && (base[0x3C4] == 0x01) && (base[0x3C5] == 0xd1)
            && (base[0x3C6] == 0x01) && (base[0x3C7] == 0x21)) {
            apply_at(0x3C2);
            return;
        }

        // Live mapping may already be EPolicyAllowed (00 21) after the first apply.
        if ((search_size > 0x3C8)
            && (base[0x3C2] == 0x00) && (base[0x3C3] == 0x28)
            && (base[0x3C4] == 0x01) && (base[0x3C5] == 0xd1)
            && (base[0x3C6] == 0x00) && (base[0x3C7] == 0x21)) {
            LOG_WARN(KERNEL, "{} unsigned-MIDlet policy already Allowed at codeseg offset 0x3C2",
                cs->name());
            return;
        }

        LOG_WARN(KERNEL, "{}: no unsigned-MIDlet policy patch matched (run=0x{:X} size={} [0x3C2]={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X})",
            cs->name(), run_addr, code_size,
            (search_size > 0x3CB) ? base[0x3C2] : 0,
            (search_size > 0x3CB) ? base[0x3C3] : 0,
            (search_size > 0x3CB) ? base[0x3C4] : 0,
            (search_size > 0x3CB) ? base[0x3C5] : 0,
            (search_size > 0x3CB) ? base[0x3C6] : 0,
            (search_size > 0x3CB) ? base[0x3C7] : 0,
            (search_size > 0x3CB) ? base[0x3C8] : 0,
            (search_size > 0x3CB) ? base[0x3C9] : 0,
            (search_size > 0x3CB) ? base[0x3CA] : 0,
            (search_size > 0x3CB) ? base[0x3CB] : 0);
    }

    // j9midps60.exe ships without AllFiles. Host-spawned J9 then gets
    // KErrPermissionDenied (-46) as soon as it touches Z:\sys\ or AMS's
    // private dir (C:\private\10203636\). Real AMS launches the VM as a
    // child; we still grant AllFiles so either path can open those files.
    static void apply_j9_allfiles_patch(codeseg_ptr cs) {
        epoc::security_info &info = cs->get_sec_info();
        if (info.caps.get(epoc::cap_all_files)) {
            return;
        }
        info.caps.set(epoc::cap_all_files);
        LOG_INFO(KERNEL, "{} AllFiles capability granted for host J9 launch", cs->name());
    }

    // j9_23_midp2ams.dll keeps a TDesC* at .data+0x28 (RAM 0x3FFF0028 on
    // the 5320 image). ROM .data is all-zero, so the pointer stays NULL
    // until option parsing allocates an HBufC. JvmNativePort registers a
    // Compare callback against that slot first, then later asks whether a
    // native name equals it — TDesC::Compare(NULL) is KERN-EXEC 3
    // (euser.dll+0x5170, lr in midp2ams).
    //
    // Point the slot at an existing in-image TLitC (do NOT write into the
    // 12 zero bytes after the ARM dtor walker — that is its literal pool:
    // start/end of an empty static-destructor list. Overwriting it made
    // DllMain(ProcessDetach) ldr from 0x20 and KERN-EXEC 3 at
    // j9_23_midp2ams.dll+0x4C).
    static void apply_j9_midp2ams_desc_patch(codeseg_ptr cs) {
        std::uint8_t *data = cs->get_constant_data();
        const std::uint32_t data_size = cs->get_data_size();
        if (!data || data_size < 0x2C) {
            return;
        }

        std::uint32_t current = 0;
        std::memcpy(&current, data + 0x28, sizeof(current));
        if (current != 0) {
            return;
        }

        std::uint8_t *code = nullptr;
        const address code_addr = cs->get_code_run_addr(nullptr, &code);
        if (!code_addr || !code) {
            return;
        }

        // Prefer a length-1 TLitC so euser Compare does not take the
        // length-0 ldrle path. L"\\" and L":" live in the UTF-16 string
        // table, not in an ARM literal pool.
        static const std::uint8_t litc_backslash[] = {
            0x01, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00
        };
        static const std::uint8_t litc_colon[] = {
            0x01, 0x00, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x00
        };

        const std::uint32_t code_size = cs->get_code_size();
        address litc_addr = 0;
        const std::uint8_t *patterns[] = { litc_backslash, litc_colon };
        for (const std::uint8_t *pat : patterns) {
            for (std::uint32_t i = 0; i + 8 <= code_size; i += 2) {
                if (std::memcmp(code + i, pat, 8) == 0) {
                    litc_addr = code_addr + i;
                    break;
                }
            }
            if (litc_addr) {
                break;
            }
        }

        if (!litc_addr) {
            LOG_WARN(KERNEL, "{}: no length-1 TLitC to seed charset Compare pointer", cs->name());
            return;
        }

        std::memcpy(data + 0x28, &litc_addr, sizeof(litc_addr));
        LOG_INFO(KERNEL, "{} .data+0x28 TDesC* seeded with existing TLitC at 0x{:X}", cs->name(), litc_addr);
    }

    // NativeFile._open builds a TPtrC16 at sp+8, then calls
    // (*(0x3fff0018))->vtable+0x20(TPtrC, mode=0). A BKPT after the
    // TPtrC ctor dumps the exact UTF-16 path Java handed over — this is
    // the only authoritative answer to "which path" / "is it Chinese".
    static address g_j9_nf_path_bkpt = 0;
    static address g_j9_nf_result_bkpt = 0;

    // Last NativeFile._open seen by the hook; printed in the j9midps60 exit
    // hint (process.cpp) so a truncated console log still shows whether the
    // suite JAR open succeeded before the VM died.
    std::string j9_nf_last_path;
    std::int32_t j9_nf_last_result = 0;
    bool j9_nf_open_seen = false;
    bool j9_nf_hook_installed = false;
    std::string j9_loaded_libs;
    address j9_jcl_vm_dllmain = 0;
    address j9_jcl_onload = 0;
    address j9_jcl_jvm_onload = 0;
    address j9_jcl_jni_onunload = 0;

    static std::u16string read_guest_utf16(kernel::process *pr, const address ptr, std::uint32_t len) {
        if (!pr || !ptr || (len == 0)) {
            return {};
        }
        if (len > 512) {
            len = 512;
        }
        const auto *chars = reinterpret_cast<const char16_t *>(pr->get_ptr_on_addr_space(ptr));
        if (!chars) {
            return {};
        }
        return std::u16string(chars, chars + len);
    }

    // Keep the BKPT installed — the hook is NOT one-shot any more: J9 opens
    // several files through NativeFile._open during startup (JAD, JAR, jxe
    // cache, ...) and every call has to stay observable/rewritable. Both
    // patched sites are 16-bit instructions (`movs r5, #0` / `movs r5, r0`),
    // so emulate the displaced instruction and step over the BKPT.
    static void j9_step_over_bkpt(arm::core *core, const address pc, const std::uint32_t r5) {
        core->set_reg(5, r5);
        core->set_pc(pc + 2);
    }

    static void j9_nativefile_open_bkpt(arm::core *core, kernel::thread *thr, const std::uint32_t addr) {
        if (!core || !thr) {
            return;
        }
        const address pc = addr & ~1u;
        const bool path_hit = (pc == (g_j9_nf_path_bkpt & ~1u))
            || ((pc >= 0x81A5D4C8) && (pc <= 0x81A5D4CC));
        const bool result_hit = (pc == (g_j9_nf_result_bkpt & ~1u))
            || ((pc >= 0x81A5D4DE) && (pc <= 0x81A5D4E2));
        if (!path_hit && !result_hit) {
            return;
        }

        kernel::process *pr = thr->owning_process();
        if (result_hit) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] Open result r0={} (0x{:X}) from {}",
                static_cast<std::int32_t>(core->get_reg(0)), core->get_reg(0),
                pr ? pr->name() : "?");
            j9_nf_last_result = static_cast<std::int32_t>(core->get_reg(0));
            j9_nf_open_seen = true;
            j9_step_over_bkpt(core, pc, core->get_reg(0));
            return;
        }

        const address sp = core->get_reg(13);
        const auto *tdesc = reinterpret_cast<const std::uint32_t *>(pr ? pr->get_ptr_on_addr_space(sp + 8) : nullptr);
        const std::uint32_t type_len = tdesc ? tdesc[0] : 0;
        const address str_ptr = tdesc ? tdesc[1] : 0;
        const std::uint32_t len = type_len & 0x0FFFFFFFu;
        std::u16string path = read_guest_utf16(pr, str_ptr, len);
        if (path.empty()) {
            path = read_guest_utf16(pr, core->get_reg(6), len);
        }

        std::string hex;
        hex.reserve(path.size() * 5);
        bool non_ascii = false;
        std::uint32_t slash_fix = 0;
        for (char16_t &ch : path) {
            hex += fmt::format("{:04X} ", static_cast<std::uint16_t>(ch));
            if (static_cast<std::uint16_t>(ch) > 0x7F) {
                non_ascii = true;
            }
            if (ch == u'/') {
                ch = u'\\';
                ++slash_fix;
            }
        }
        if ((slash_fix > 0) && str_ptr && pr) {
            if (auto *writable = reinterpret_cast<char16_t *>(pr->get_ptr_on_addr_space(str_ptr))) {
                std::memcpy(writable, path.data(), path.size() * sizeof(char16_t));
            }
        }

        std::uint32_t file_obj = 0;
        std::uint32_t file_vt = 0;
        if (pr) {
            if (const auto *slot = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(0x3FFF0018))) {
                file_obj = slot[0];
                if (const auto *obj = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(file_obj))) {
                    file_vt = obj[0];
                }
            }
        }

        LOG_WARN(EMULATED_STDOUT, "[j9-nf] path='{}' len={} non_ascii={} slash_fix={} hex=[{}] "
            "tdesc=0x{:X}/0x{:X} r6=0x{:X} file_obj=0x{:X} vtable=0x{:X} from {}",
            common::ucs2_to_utf8(path), len, non_ascii ? 1 : 0, slash_fix, hex,
            type_len, str_ptr, core->get_reg(6), file_obj, file_vt,
            pr ? pr->name() : "?");

        // J9's Java layer can hand NativeFile the suite JAR path cut
        // mid-way (observed on 5320: the 25-char `C:/private/102033E6/j.jar`
        // arrives as `C:/private/102` — 14 chars; also the session-relative
        // `private/102`, and TBuf<40> copies ending in `...m.ja`). The alias
        // files staged by j2me::launch cover some of those names on disk,
        // but reads that follow (size/seek) still describe the wrong file.
        // Rewrite the just-built TPtrC16 in place to the always-staged short
        // copy `C:\j.jar` (8 chars fit into every truncated buffer) and fix
        // the descriptor length, keeping the type nibble.
        {
            const std::string lower = common::lowercase_string(common::ucs2_to_utf8(path));
            static const std::string SUITE_ROOT = "c:\\private\\102033e6";
            static const std::string SUITE_ROOT_REL = "private\\102033e6";
            const bool abs_cut = (lower.size() >= 14) && (lower.size() < SUITE_ROOT.size())
                && (SUITE_ROOT.compare(0, lower.size(), lower) == 0);
            const bool rel_cut = (lower.size() >= 11) && (lower.size() < SUITE_ROOT_REL.size())
                && (SUITE_ROOT_REL.compare(0, lower.size(), lower) == 0);
            bool ja_cut = false;
            if (lower.find("102033e6") != std::string::npos) {
                if (lower.size() >= 3) {
                    ja_cut = (lower.compare(lower.size() - 3, 3, ".ja") == 0);
                }
                if (!ja_cut && (lower.size() >= 2)) {
                    ja_cut = (lower.compare(lower.size() - 2, 2, ".j") == 0);
                }
            }

            if (((abs_cut) || (rel_cut) || (ja_cut)) && pr && tdesc && str_ptr && (len >= 8)) {
                static constexpr char16_t SHORT_JAR[] = u"C:\\j.jar";
                if (auto *writable = reinterpret_cast<char16_t *>(pr->get_ptr_on_addr_space(str_ptr))) {
                    std::memcpy(writable, SHORT_JAR, sizeof(SHORT_JAR) - sizeof(char16_t));
                    if (auto *len_word = reinterpret_cast<std::uint32_t *>(
                            pr->get_ptr_on_addr_space(sp + 8))) {
                        *len_word = (type_len & 0xF0000000u) | 8;
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] rewrote truncated suite path '{}' -> 'C:\\j.jar' from {}",
                        common::ucs2_to_utf8(path), pr->name());
                    path.assign(SHORT_JAR, SHORT_JAR + 8);
                }
            }
        }

        j9_nf_last_path = common::ucs2_to_utf8(path);

        j9_step_over_bkpt(core, pc, 0);
    }

    static void apply_j9_nativefile_open_hook(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        // NativeFile._open: push {r0-r2,r4-r7,lr}; sub sp,#0x10; movs r2,#0;
        // movs r4,r0; ldr r0,[r0]; movs r7,#5
        static const std::uint8_t open_sig[] = {
            0xf7, 0xb5, 0x84, 0xb0, 0x00, 0x22, 0x04, 0x00, 0x00, 0x68, 0x05, 0x27
        };
        static constexpr std::uint32_t k_path_off = 0x38; // movs r5, #0 after TPtrC
        static constexpr std::uint32_t k_result_off = 0x4E; // movs r5, r0 after Open

        for (std::uint32_t i = 0; (i + k_result_off + 2) <= code_size; i += 2) {
            if (std::memcmp(base + i, open_sig, sizeof(open_sig)) != 0) {
                continue;
            }
            const bool path_ok = ((base[i + k_path_off] == 0x00)
                && ((base[i + k_path_off + 1] == 0x25) || (base[i + k_path_off + 1] == 0xBE)));
            // `movs r5, r0` is 0x0005, stored LE as 05 00 (not 00 05).
            const bool result_ok = ((base[i + k_result_off] == 0x00)
                    && ((base[i + k_result_off + 1] == 0x05) || (base[i + k_result_off + 1] == 0xBE)))
                || ((base[i + k_result_off] == 0x05) && (base[i + k_result_off + 1] == 0x00));
            if (!path_ok || !result_ok) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] signature matched @0x{:X} but offsets differ (path=0x{:02X}{:02X} result=0x{:02X}{:02X}); hook NOT installed",
                    run_addr + i,
                    base[i + k_path_off], base[i + k_path_off + 1],
                    base[i + k_result_off], base[i + k_result_off + 1]);
                return;
            }

            base[i + k_path_off] = 0x00;
            base[i + k_path_off + 1] = 0xBE;
            base[i + k_result_off] = 0x00;
            base[i + k_result_off + 1] = 0xBE;
            g_j9_nf_path_bkpt = run_addr + i + k_path_off;
            g_j9_nf_result_bkpt = run_addr + i + k_result_off;
            j9_nf_hook_installed = true;

            kernel_system *kern = cs->get_kernel_object_owner();
            static bool hooked = false;
            if (kern && !hooked) {
                kern->register_breakpoint_hit_callback(j9_nativefile_open_bkpt);
                hooked = true;
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] hooked {} path@0x{:X} result@0x{:X}",
                cs->name(), g_j9_nf_path_bkpt, g_j9_nf_result_bkpt);
            return;
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] NativeFile._open signature not found in {} ({} bytes)",
            cs->name(), code_size);
    }

    // export 2 factory V2/V1 both NULL used to User::Exit(1), same as
    // Java Args.usage(). Remap that native failure to 11 so it is not
    // confused with parse/startApp Exit(1).
    //
    // Do NOT inject a bare "-app" into the CDesCArray. Args.parse treats
    // -app as iAppClassName (next token) and -event as the AMS event
    // name. A flag with no value calls usage() → "Bad command line".
    // Host CommandLine now passes `-app <MIDlet-1 class>` without `-event`.
    static void apply_j9_midp2ams_app_arg_patch(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        static const std::uint8_t factory_sig[] = { 0x20, 0x60, 0x01, 0xd1, 0x01, 0x20, 0xde, 0xe7 };
        for (std::uint32_t i = 0; (i + sizeof(factory_sig)) <= code_size; i += 2) {
            if (std::memcmp(base + i, factory_sig, sizeof(factory_sig)) == 0) {
                base[i + 4] = 11;
                LOG_WARN(KERNEL, "{} export2 factory-fail status remapped 1→11 at 0x{:X}",
                    cs->name(), (run_addr + i + 4) & ~1u);
                break;
            }
        }

        // midp2ams passes L"-jcl:cldc11:nokiaextcldc" to j9.dll.
        // -jcl:<config>[:options] — the :nokiaextcldc option makes dynload
        // treat nokiaextcldc as an exploded tree under jclCldc11\ and skip
        // the JXESL. Truncate to -jcl:cldc11 (null the tail in the TLitC).
        static const char16_t JCL_FULL[] = u"-jcl:cldc11:nokiaextcldc";
        static constexpr std::size_t JCL_KEEP = 11; // "-jcl:cldc11"
        static constexpr std::size_t JCL_CHARS = 24;
        const std::uint32_t jcl_bytes = static_cast<std::uint32_t>(JCL_CHARS * sizeof(char16_t));
        for (std::uint32_t i = 0; (i + jcl_bytes) <= code_size; i += 2) {
            if (std::memcmp(base + i, JCL_FULL, jcl_bytes) != 0) {
                continue;
            }
            std::memset(base + i + JCL_KEEP * sizeof(char16_t), 0,
                (JCL_CHARS - JCL_KEEP) * sizeof(char16_t));
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] patched -jcl:cldc11:nokiaextcldc -> -jcl:cldc11 @0x{:X}",
                run_addr + i);
            break;
        }
    }

    // JXESL odc does LoadLibrary + sl_lookup_name("J9GetJXE").
    // EPOC RLibrary::Lookup is ordinal-only; export 1 is usually a JNI
    // helper, not the no-arg getter. Point ordinal 1 at the ROM
    // `ldr r0, =jxe; bx lr` (or a planted copy) and aim that literal at
    // the embedded rom.classes (J99J), NOT the wrapping ZIP (PK).
    // iveLoadJxe treats a PK pointer as a JXE image and jumps through
    // garbage (unimplemented SVC + write to 0).
    static void apply_j9_getjxe_patch(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        const std::uint32_t code_size = cs->get_code_size();
        static const std::uint8_t pk[] = { 0x50, 0x4B, 0x03, 0x04, 0x14, 0x00 };
        static const std::uint8_t j99j[] = { 0x4A, 0x39, 0x39, 0x4A };
        std::uint32_t pk_off = code_size;
        std::uint32_t j99_off = code_size;
        for (std::uint32_t i = 0; (i + 6) <= code_size; i++) {
            if ((pk_off >= code_size) && (std::memcmp(base + i, pk, 6) == 0)) {
                // jar2jxe local header: 30 + "rom.classes"(11) + extra(7) = 48
                if ((i + 48 + 4) <= code_size && (std::memcmp(base + i + 48, j99j, 4) == 0)) {
                    pk_off = i;
                    j99_off = i + 48;
                    break;
                }
                if (pk_off >= code_size) {
                    pk_off = i;
                }
            }
            if ((j99_off >= code_size) && (std::memcmp(base + i, j99j, 4) == 0)) {
                j99_off = i;
            }
        }
        if ((j99_off >= code_size) && (pk_off >= code_size)) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] no embedded JXE/J99J in {}", cs->name());
            return;
        }
        // ROM J9GetJXE returns the wrapping ZIP (PK). iveLoadJxe then
        // fopen("jxe=<ptr>") / hyzip-extracts rom.classes. Returning the
        // inner J99J makes dynload treat header word 0xFFFFFFC0 as a SVC.
        const std::uint32_t image_off = (pk_off < code_size) ? pk_off : j99_off;
        address jxe = run_addr + image_off;
        kernel_system *kern = cs->get_kernel_object_owner();
        kernel::process *pr = kern ? kern->crr_process() : nullptr;
        if (kern) {
            if (const auto *at = reinterpret_cast<const std::uint8_t *>(
                    kern->get_memory_system()->get_real_pointer(jxe))) {
                if (std::memcmp(at, pk, 4) != 0) {
                    if (pk_off < code_size) {
                        jxe = run_addr + pk_off + 0x78;
                    }
                }
            }
        }

        address getter = 0;
        // Exact ROM J9GetJXE: `ldr r0, [pc, #0x74]; bx lr` (1d 48 70 47).
        // Scanning for any xx48 7047 hits a mid-instruction JNI gadget at
        // 0x81A6243D; calling that returns garbage and dynload jumps to 0xE00.
        for (std::uint32_t g = 0; (g + 4) <= code_size; g += 2) {
            if ((base[g] != 0x1D) || (base[g + 1] != 0x48)
                || (base[g + 2] != 0x70) || (base[g + 3] != 0x47)) {
                continue;
            }
            const std::uint32_t pc_align = ((run_addr + g + 4) & ~3u) - run_addr;
            const std::uint32_t lit = pc_align + 0x74;
            if ((lit + 4) > code_size) {
                continue;
            }
            std::memcpy(base + lit, &jxe, 4);
            getter = (run_addr + g) | 1u;
            break;
        }

        if (!getter) {
            // Fallback: plant `ldr r0, [pc, #0]; bx lr; .word jxe` in a
            // zero gap past the first 8KB (never the ROM header).
            std::uint32_t thunk_off = code_size;
            const std::uint32_t floor = (code_size > 0x2000) ? 0x2000 : 16;
            for (std::uint32_t i = (code_size - 8) & ~3u; i >= floor; i -= 4) {
                if ((base[i] | base[i + 1] | base[i + 2] | base[i + 3]
                        | base[i + 4] | base[i + 5] | base[i + 6] | base[i + 7]) == 0) {
                    thunk_off = i;
                    break;
                }
            }
            if (thunk_off >= code_size) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] no gap for J9GetJXE thunk in {}", cs->name());
                return;
            }
            base[thunk_off + 0] = 0x00;
            base[thunk_off + 1] = 0x48;
            base[thunk_off + 2] = 0x70;
            base[thunk_off + 3] = 0x47;
            std::memcpy(base + thunk_off + 4, &jxe, 4);
            getter = (run_addr + thunk_off) | 1u;
        }

        // iveLoadJxe writes the image. ROM XIP is RX and used to write-to-0.
        // Copy the ZIP into a RW chunk and return that pointer instead.
        if (kern) {
            const std::uint32_t zip_bytes = (pk_off < code_size)
                ? ((code_size - pk_off > 0x80000) ? 0x80000 : (code_size - pk_off))
                : 0;
            if (zip_bytes >= 64) {
                static kernel::chunk *jxe_rw = nullptr;
                if (!jxe_rw) {
                    jxe_rw = kern->create<kernel::chunk>(kern->get_memory_system(), nullptr, "J9JxeRW",
                        0, zip_bytes, zip_bytes, prot_read_write, kernel::chunk_type::normal,
                        kernel::chunk_access::rom, kernel::chunk_attrib::none);
                }
                if (jxe_rw) {
                    if (auto *dst = reinterpret_cast<std::uint8_t *>(jxe_rw->host_base())) {
                        const auto *src = reinterpret_cast<const std::uint8_t *>(
                            pr ? pr->get_ptr_on_addr_space(jxe) : (base + (jxe - run_addr)));
                        if (src) {
                            std::memcpy(dst, src, zip_bytes);
                            const address rw = jxe_rw->base(pr).ptr_address();
                            if (rw) {
                                jxe = rw;
                                if (getter) {
                                    const std::uint32_t g_off = (getter & ~1u) - run_addr;
                                    const std::uint32_t pc_align = ((run_addr + g_off + 4) & ~3u) - run_addr;
                                    const std::uint32_t lit = pc_align + 0x74;
                                    if (lit + 4 <= code_size) {
                                        std::memcpy(base + lit, &jxe, 4);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        cs->set_export(1, eka2l1::ptr<void>(getter));
        const auto exports = cs->get_export_table_raw();
        const address ord1 = exports.empty() ? 0 : exports[0];
        const auto *img = (j99_off < code_size) ? (base + j99_off) : (base + pk_off);
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] J9GetJXE export1 set=0x{:X} now=0x{:X} jxe=0x{:X} j99=+0x{:X} zip=+0x{:X} magic={:02X}{:02X}{:02X}{:02X} in {}",
            getter, ord1, jxe, j99_off, pk_off, img[0], img[1], img[2], img[3],
            cs->name());
    }

    // iveLoadJxeFromFile (j9vmall export 9, ivejar.c) fread's 0x60 bytes
    // and requires file[0]==PK\x03\x04 AND file[0x30]==J99J — the jar2jxe
    // ZIP local-header + rom.classes layout. Our staged C:\jcl.jxe is the
    // inner rom.classes (naked J99J). Both compares fail → return 3, the
    // JCL image is never registered, dynload reports JVMJ9VM019E
    // (java/lang/Object). A real ZIP passes the check but is then used as
    // the ROM base; ZIP+0x1C is the extra/filename field (nonzero), so
    // the AOT walk at 0x818FAF34 KERN-EXEC 3's.
    //
    // Accept a raw J99J file: compare magic at offset 0, and read the
    // in-place / AOT flags from the J99J header (+4 / +0x1C) so the
    // loader takes the "read whole file" path instead of hyzip.
    static constexpr address k_j9_loadjxe_magic = 0x818CFF46u;
    static constexpr address k_j9_loadjxe_result = 0x818CFFD6u;
    static constexpr address k_j9_dynload_jxe_parse = 0x818D529Au;
    static constexpr address k_j9_dynload_findjar = 0x818D52BAu;
    static constexpr address k_j9_rom_aot_load = 0x818FAF18u;
    static constexpr address k_j9_method_run_ld = 0x818FD668u;
    static constexpr address k_j9_verify_sig_ld = 0x81903DBAu;
    static constexpr address k_j9_monitorenter = 0x819109F0u;
    static constexpr address k_j9_monitorexit = 0x81910AD8u;

    static void apply_j9_vmall_raw_j99j_file_patch(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        const std::uint32_t code_size = cs->get_code_size();
        auto patch_at = [&](const address va, const std::uint32_t n, auto &&fn) {
            if (va < run_addr) {
                return false;
            }
            const std::uint32_t off = va - run_addr;
            if ((off + n) > code_size) {
                return false;
            }
            fn(base + off);
            return true;
        };
        bool magic_ok = false;
        patch_at(k_j9_loadjxe_magic, 40, [&](std::uint8_t *p) {
            // Original: add r0, sp, #0x34 / already patched: add r0, sp, #4
            if ((p[4] == 0x0D || p[4] == 0x01) && (p[5] == 0xA8)) {
                p[4] = 0x01;
                if ((p[16] == 0x33) && (p[17] == 0xA1)) {
                    p[16] = 0x31;
                }
                if ((p[32] == 0x14) && (p[33] == 0x98)) {
                    p[32] = 0x08;
                }
                if ((p[38] == 0x0E) && (p[39] == 0x98)) {
                    p[38] = 0x02;
                }
                magic_ok = true;
            }
        });
        bool bkpt_ok = false;
        patch_at(k_j9_loadjxe_result, 2, [&](std::uint8_t *p) {
            // cmp r0, #0 (00 28) or already BKPT (00 BE)
            if (((p[0] == 0x00) && (p[1] == 0x28)) || ((p[0] == 0x00) && (p[1] == 0xBE))) {
                p[0] = 0x00;
                p[1] = 0xBE;
                bkpt_ok = true;
            }
        });
        bool dyn_parse = false;
        bool dyn_find = false;
        patch_at(k_j9_dynload_jxe_parse, 2, [&](std::uint8_t *p) {
            if (((p[0] == 0x00) && (p[1] == 0x28)) || ((p[0] == 0x00) && (p[1] == 0xBE))) {
                p[0] = 0x00;
                p[1] = 0xBE;
                dyn_parse = true;
            }
        });
        patch_at(k_j9_dynload_findjar, 2, [&](std::uint8_t *p) {
            if (((p[0] == 0x00) && (p[1] == 0x28)) || ((p[0] == 0x00) && (p[1] == 0xBE))) {
                p[0] = 0x00;
                p[1] = 0xBE;
                dyn_find = true;
            }
        });
        // 0x818FAF18: ldr r8, [lr, #0x1c] (AOT pointer). This JCL J99J has
        // aot=0. The same walker is also entered with a ROMClass (Object
        // +0x1C = method count 0x0B), then [class+0x20]+(class+0x20) becomes
        // 0 on the RW copy and ldr [r7,#8] KERN-EXEC 3's at 0x8. Skip AOT.
        bool verify_sig = false;
        patch_at(k_j9_verify_sig_ld, 2, [&](std::uint8_t *p) {
            // ldrb r7, [r6, r3] — signature walk. A bad CP SRP makes r6
            // unmapped (0xD10D7BF6) while throwing StackOverflowError.
            if (((p[0] == 0xF7) && (p[1] == 0x5C)) || ((p[0] == 0x00) && (p[1] == 0xBE))) {
                p[0] = 0x00;
                p[1] = 0xBE;
                verify_sig = true;
            }
        });
        bool monenter = false;
        bool monexit = false;
        patch_at(k_j9_monitorenter, 4, [&](std::uint8_t *p) {
            // ldr lr, [r7] — thin-lock monitorenter. Class+4 bit31 forces
            // the slow helper, which throws IMSE and aborts class init.
            if (((p[0] == 0x00) && (p[1] == 0xE0) && (p[2] == 0x97) && (p[3] == 0xE5))
                || ((p[0] == 0x70) && (p[1] == 0x00) && (p[2] == 0x20) && (p[3] == 0xE1))) {
                p[0] = 0x70;
                p[1] = 0x00;
                p[2] = 0x20;
                p[3] = 0xE1;
                monenter = true;
            }
        });
        patch_at(k_j9_monitorexit, 4, [&](std::uint8_t *p) {
            // ldr ip, [r7], #4 — same bit31 slow path as monitorenter.
            if (((p[0] == 0x04) && (p[1] == 0xC0) && (p[2] == 0x97) && (p[3] == 0xE4))
                || ((p[0] == 0x70) && (p[1] == 0x00) && (p[2] == 0x20) && (p[3] == 0xE1))) {
                p[0] = 0x70;
                p[1] = 0x00;
                p[2] = 0x20;
                p[3] = 0xE1;
                monexit = true;
            }
        });
        bool method_run = false;
        patch_at(k_j9_method_run_ld, 2, [&](std::uint8_t *p) {
            // ldr r5, [r1] (0D 68) — J9Method.bytecodes. nextROMMethod was
            // returning 0 for XIP (j9_mapped32 rejects >=0x80000000), so
            // later methods get bytecodes=0x14 and [0+8] KERN-EXEC 3's.
            if (((p[0] == 0x0D) && (p[1] == 0x68)) || ((p[0] == 0x00) && (p[1] == 0xBE))) {
                p[0] = 0x00;
                p[1] = 0xBE;
                method_run = true;
            }
        });
        bool aot_skip = false;
        patch_at(k_j9_rom_aot_load, 4, [&](std::uint8_t *p) {
            if ((p[0] == 0x1C) && (p[1] == 0x80) && (p[2] == 0x9E) && (p[3] == 0xE5)) {
                p[0] = 0x00;
                p[1] = 0x80;
                p[2] = 0xA0;
                p[3] = 0xE3; // mov r8, #0
                aot_skip = true;
            } else if ((p[0] == 0x00) && (p[1] == 0x80) && (p[2] == 0xA0) && (p[3] == 0xE3)) {
                aot_skip = true;
            }
        });
        if (kernel_system *kern = cs->get_kernel_object_owner()) {
            if (arm::core *cpu = kern->get_cpu()) {
                cpu->imb_range(run_addr, code_size);
            }
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] iveLoadJxeFromFile raw-J99J magic={} bkpt={} dynparse={} dynfind={} aotskip={} methodrun={} verifysig={} monenter={} monexit={} run=0x{:X} in {}",
            magic_ok ? 1 : 0, bkpt_ok ? 1 : 0, dyn_parse ? 1 : 0, dyn_find ? 1 : 0, aot_skip ? 1 : 0,
            method_run ? 1 : 0, verify_sig ? 1 : 0, monenter ? 1 : 0, monexit ? 1 : 0, run_addr, cs->name());
    }

    // iveLoadJxe uses the JCL literal 0x8194FF98 (ZIP PK) as the in-memory
    // image base. fopen("jxe=<ptr>") is only a probe — the file never
    // replaces that pointer. Walking PK as J9ROMClass makes romMethods
    // resolve to 0 (KERN-EXEC 3 at j9vmall+0x390FC).
    //
    // J99J+0x10 is SRP 0xFFFFFFC0 (-64) → field+SRP = J99J-48 = ZIP local
    // header. Copying only rom.classes leaves that SRP before the chunk, so
    // iveLoadJxe never registers java/lang/Object (JVMJ9VM019E). Keep the
    // original XIP layout: [ZIP local][J99J][CD/EOCD], and retarget the
    // literal at the J99J (not PK). Do not smash XIP / JCL export 1.
    static kernel::chunk *g_j9_jcl_jxe_rw = nullptr;
    static address g_j9_jcl_jxe_va = 0;

    static void apply_j9_jcl_inmem_jxe_patch(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        const std::uint32_t code_size = cs->get_code_size();
        static const std::uint8_t pk[] = { 0x50, 0x4B, 0x03, 0x04 };
        static const std::uint8_t j99j[] = { 0x4A, 0x39, 0x39, 0x4A };
        std::uint32_t pk_off = code_size;
        std::uint32_t j99_off = code_size;
        std::uint32_t j99_size = 0;
        for (std::uint32_t i = 0; (i + 52) <= code_size; ++i) {
            if (std::memcmp(base + i, pk, 4) != 0) {
                continue;
            }
            const std::uint16_t fnlen = static_cast<std::uint16_t>(base[i + 26] | (base[i + 27] << 8));
            const std::uint16_t extra = static_cast<std::uint16_t>(base[i + 28] | (base[i + 29] << 8));
            const std::uint32_t usize = static_cast<std::uint32_t>(base[i + 22] | (base[i + 23] << 8)
                | (base[i + 24] << 16) | (base[i + 25] << 24));
            const std::uint32_t data = i + 30u + fnlen + extra;
            if ((fnlen != 11) || (extra > 32) || (usize < 64) || (usize > 0x80000)
                || (data + 4 > code_size) || (std::memcmp(base + data, j99j, 4) != 0)) {
                continue;
            }
            pk_off = i;
            j99_off = data;
            j99_size = usize;
            break;
        }
        if ((j99_off >= code_size) || (j99_size < 64) || (j99_off < pk_off)) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] no in-memory rom.classes/J99J in {}", cs->name());
            return;
        }
        const std::uint32_t j99_avail = code_size - j99_off;
        if (j99_size > j99_avail) {
            j99_size = j99_avail;
        }
        const std::uint32_t prefix = j99_off - pk_off;
        std::uint32_t zip_size = prefix + j99_size;
        const std::uint32_t zip_scan = j99_off + j99_size;
        for (std::uint32_t i = zip_scan; (i + 22) <= code_size; ++i) {
            if ((base[i] != 0x50) || (base[i + 1] != 0x4B) || (base[i + 2] != 5) || (base[i + 3] != 6)) {
                continue;
            }
            const std::uint16_t nent = static_cast<std::uint16_t>(base[i + 8] | (base[i + 9] << 8));
            const std::uint16_t tnent = static_cast<std::uint16_t>(base[i + 10] | (base[i + 11] << 8));
            const std::uint32_t csize = static_cast<std::uint32_t>(base[i + 12] | (base[i + 13] << 8)
                | (base[i + 14] << 16) | (base[i + 15] << 24));
            const std::uint32_t coff = static_cast<std::uint32_t>(base[i + 16] | (base[i + 17] << 8)
                | (base[i + 18] << 16) | (base[i + 19] << 24));
            const std::uint16_t comment = static_cast<std::uint16_t>(base[i + 20] | (base[i + 21] << 8));
            if ((nent != tnent) || (nent == 0) || (nent > 64) || (comment > 256)
                || (csize < 46) || (i < csize)) {
                continue;
            }
            const std::uint32_t cdir = i - csize;
            if ((cdir < coff) || (std::memcmp(base + cdir, "PK\x01\x02", 4) != 0)) {
                continue;
            }
            if ((cdir - coff) != pk_off) {
                continue;
            }
            zip_size = (i + 22u + comment) - pk_off;
            break;
        }
        if ((pk_off + zip_size) > code_size) {
            zip_size = code_size - pk_off;
        }
        kernel_system *kern = cs->get_kernel_object_owner();
        kernel::process *pr = kern ? kern->crr_process() : nullptr;
        if (!kern) {
            return;
        }
        const std::uint32_t chunk_sz = (zip_size + 0xFFFu) & ~0xFFFu;
        if (g_j9_jcl_jxe_rw && (g_j9_jcl_jxe_rw->max_size() < chunk_sz)) {
            g_j9_jcl_jxe_rw = nullptr;
        }
        if (!g_j9_jcl_jxe_rw) {
            g_j9_jcl_jxe_rw = kern->create<kernel::chunk>(kern->get_memory_system(), pr, "J9JclJxeRW",
                0, chunk_sz, chunk_sz, prot_read_write, kernel::chunk_type::normal,
                kernel::chunk_access::code, kernel::chunk_attrib::none);
            if (!g_j9_jcl_jxe_rw) {
                g_j9_jcl_jxe_rw = kern->create<kernel::chunk>(kern->get_memory_system(), nullptr, "J9JclJxeRW",
                    0, chunk_sz, chunk_sz, prot_read_write, kernel::chunk_type::normal,
                    kernel::chunk_access::rom, kernel::chunk_attrib::none);
            }
        }
        if (!g_j9_jcl_jxe_rw || !g_j9_jcl_jxe_rw->host_base()) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] J99J RW chunk failed in {}", cs->name());
            return;
        }
        auto *dst = reinterpret_cast<std::uint8_t *>(g_j9_jcl_jxe_rw->host_base());
        std::memcpy(dst, base + pk_off, zip_size);
        if (chunk_sz > zip_size) {
            std::memset(dst + zip_size, 0, chunk_sz - zip_size);
        }
        const address rw = g_j9_jcl_jxe_rw->base(pr).ptr_address();
        if (!rw) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] J99J RW base is 0 in {}", cs->name());
            return;
        }
        const address rw_j99 = rw + prefix;
        // dynload.c: path "jxe=<ptr>" → hex-parse → iveFindFileInJar(ptr,
        // "rom.classes"). ptr must be the ZIP PK, not the inner J99J.
        // Overwriting "jxe=%p" with a real filename skips this path and
        // the VM looks for java/lang/Object.class inside a class JAR.
        g_j9_jcl_jxe_va = rw;
        const address pk_va = run_addr + pk_off;
        const address j99_va = run_addr + j99_off;
        // Keep the stock XIP PK literal. dynload iveFindFileInJar needs the
        // original addresses so ROM SRPs/fn-ptrs stay inside mapped JCL.
        // Retargeting to RW made Object+0x20 / callback slots resolve to 0x8.
        int patched = 0;
        if (arm::core *cpu = kern->get_cpu()) {
            cpu->imb_range(run_addr, code_size);
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] inmem-j99j pk=0x{:X} j99=0x{:X} -> rw=0x{:X} j99=0x{:X} zip={} prefix={} patched={} in {}",
            pk_va, j99_va, rw, rw_j99, zip_size, prefix, patched, cs->name());
        const address cur_thr = 0x819409BCu;
        if ((cur_thr >= run_addr) && ((cur_thr - run_addr + 4u) <= code_size)) {
            auto *slot = reinterpret_cast<std::uint32_t *>(base + (cur_thr - run_addr));
            if (slot && ((*slot == 0xE5F5C003u) || (*slot == 0xE1200070u))) {
                *slot = 0xE1200070u;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(cur_thr, 4);
                }
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] currentThread helper hooked");
            }
        }
        auto hook_arm = [&](const address va, const std::uint32_t orig, const char *tag) {
            if ((va < run_addr) || ((va - run_addr + 4u) > code_size)) {
                return;
            }
            auto *slot = reinterpret_cast<std::uint32_t *>(base + (va - run_addr));
            if (slot && ((*slot == orig) || (*slot == 0xE1200070u))) {
                *slot = 0xE1200070u;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(va, 4);
                }
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] {} hooked", tag);
            }
        };
        hook_arm(0x8193EA38u, 0xE59DE02Cu, "init-loop");
        hook_arm(0x8193EB60u, 0xE12FFF1Eu, "init-ret");
    }

    // j9mjit23 hook at 0x819329A1 is registered as a dynload class-walk
    // callback (j9vmall 0x818D0532 blx r6). It treats [r2+4] as a J9Class*
    // and does ldr [([r5])-0x14 + 8]. After in-memory J99J load the class
    // object is still a ROM stub ([r5]=0x14) so that becomes a read of 0x8.
    // Return value is ignored; JCL is interpretable. Force interpreter.
    static constexpr address k_j9_mjit_class_hook = 0x819329A0u;

    static void apply_j9_mjit_noop_hook(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        const std::uint32_t code_size = cs->get_code_size();
        static const std::uint8_t prologue[] = { 0xF0, 0xB5, 0x85, 0xB0, 0x10, 0x68, 0x04, 0x90, 0x55, 0x68, 0x91, 0x68 };
        std::uint32_t off = code_size;
        if ((k_j9_mjit_class_hook >= run_addr) && ((k_j9_mjit_class_hook - run_addr + 12) <= code_size)
            && (std::memcmp(base + (k_j9_mjit_class_hook - run_addr), prologue, sizeof(prologue)) == 0)) {
            off = k_j9_mjit_class_hook - run_addr;
        } else {
            for (std::uint32_t i = 0; (i + sizeof(prologue)) <= code_size; ++i) {
                if (std::memcmp(base + i, prologue, sizeof(prologue)) == 0) {
                    off = i;
                    break;
                }
            }
        }
        if (off >= code_size) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] mjit-noop miss run=0x{:X} size={} in {}", run_addr, code_size, cs->name());
            return;
        }
        base[off + 0] = 0x00; // movs r0, #0
        base[off + 1] = 0x20;
        base[off + 2] = 0x70; // bx lr
        base[off + 3] = 0x47;
        if (kernel_system *kern = cs->get_kernel_object_owner()) {
            if (arm::core *cpu = kern->get_cpu()) {
                cpu->imb_range(run_addr + off, 4);
            }
        }
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] mjit-noop @0x{:X} (+0x{:X}) in {}", run_addr + off, off, cs->name());
    }

    struct j9_jni_export {
        std::string name;
        address fn = 0;
        address name_ga = 0;
    };
    static std::vector<j9_jni_export> j9_jni_exports;
    static address g_j9_midp2ams_jxe = 0;
    static address g_j9_midp2ams_run = 0;
    static address g_j9_midp2ams_size = 0;
    static address g_j9_jxesl_bkpt = 0;
    static address g_j9_sl_bkpt = 0;
    static address g_j9_sl_call_bkpt = 0;
    static address g_j9_mangle_bkpt = 0;
    static address g_j9_bind_fail_bkpt = 0;
    static address g_j9_jcl_sl_bkpt = 0;
    static address g_j9_walk_miss_bkpt = 0;
    static address g_j9_walk_hit_bkpt = 0;
    static address g_j9_invoke_sites[128] = {};
    static int g_j9_invoke_n = 0;
    static address g_j9_bx_sb_sites[32] = {};
    static int g_j9_bx_sb_n = 0;
    static address g_j9_lookup_fn = 0;
    static address g_j9_jni_table_va = 0;
    static std::uint32_t g_j9_jni_table_n = 0;
    static kernel::chunk *g_j9_walk_ch = nullptr;
    static address g_j9_walk_va = 0;
    static address g_j9_walk_pairs = 0;
    static address g_j9_jnienv_table = 0;
    static address g_j9_jni_fixer = 0;
    static address g_j9_fake_env = 0;
    static address g_j9_newstr_bkpt = 0;
    static address g_j9_getstrutf_bkpt = 0;
    static address g_j9_findclass_bkpt = 0;
    static address g_j9_findclass_ret_bkpt = 0;
    static address g_j9_findclass_saved_lr = 0;
    static address g_j9_getmethod_bkpt = 0;
    static address g_j9_newobjarr_bkpt = 0;
    static address g_j9_newglobal_bkpt = 0;
    static address g_j9_callstatic_bkpt = 0;
    static address g_j9_lcdui_chain_bkpt = 0;
    static address g_j9_cp_stub_bkpt = 0;
    static address g_j9_cp_stub_meth = 0;
    static address g_j9_main_cp = 0;
    static address g_j9_dummy_fw = 0;
    static address g_j9_dummy_args = 0;
    static address g_j9_dummy_rt = 0;
    static address g_j9_dummy_cms = 0;
    static address g_j9_main_clazz = 0;
    static address g_j9_main_method = 0;
    static address g_j9_utf_stash = 0;
    static address g_j9_proplist_bkpt = 0;
    static address g_j9_throw_bkpt = 0;
    static std::uint16_t g_j9_throw_orig = 0;
    static address g_j9_valid_cp = 0;
    static codeseg_ptr g_j9_vmall_cs = nullptr;

    static constexpr std::uint32_t k_j9_fixer_off = 0x200;
    static constexpr std::uint32_t k_j9_stub_off = 0x1000;
    static constexpr std::uint32_t k_j9_stub_size = 24;
    static constexpr std::uint32_t k_j9_adapt_off = 0x5000;
    static constexpr std::uint32_t k_j9_adapt_size = 96;
    static address g_j9_last_adapt = 0;
    static address g_j9_last_thumb = 0;
    static address g_j9_toolkit_obj = 0;
    static address g_j9_canvas_obj = 0;
    static address g_j9_graphics_obj = 0;
    static int g_j9_peer_n = 0;
    static address g_j9_vmthread = 0;
    static address g_j9_vt10_c = 0;
    static address g_j9_vt14_c = 0;
    static address g_j9_vt18_c = 0;
    static address g_j9_bytecode_pc = 0;
    static address g_j9_java_sp = 0;
    static address g_j9_tramp_method = 0;
    static address g_j9_saved_r2 = 0;
    static address g_j9_saved_r4 = 0;
    static address g_j9_saved_r5 = 0;
    static address g_j9_saved_r6 = 0;
    static address g_j9_tramp_va = 0;
    static constexpr int k_j9_page_max = 12;
    static address g_j9_page_key[k_j9_page_max] = {};
    static address g_j9_page_r4[k_j9_page_max] = {};
    static address g_j9_page_r6[k_j9_page_max] = {};
    static address g_j9_page_r5[k_j9_page_max] = {};
    static int g_j9_page_n = 0;
    static address g_j9_last_jxe_key = 0;
    static address g_j9_last_jxe_r4 = 0;
    static address g_j9_last_jxe_r5 = 0;
    static address g_j9_last_jxe_r6 = 0;
    static address g_j9_last_jxe_r7 = 0;
    static address g_j9_prev_jxe_r4 = 0;
    static address g_j9_prev_jxe_r5 = 0;
    static address g_j9_prev_jxe_r6 = 0;
    static address g_j9_caller_key[k_j9_page_max] = {};
    static address g_j9_caller_r4[k_j9_page_max] = {};
    static address g_j9_caller_r5[k_j9_page_max] = {};
    static address g_j9_caller_r6[k_j9_page_max] = {};
    static int g_j9_caller_n = 0;
    static constexpr int k_j9_snap_max = 16;
    static address g_j9_snap_pc[k_j9_snap_max] = {};
    static address g_j9_snap_r4[k_j9_snap_max] = {};
    static address g_j9_snap_r6[k_j9_snap_max] = {};
    static int g_j9_snap_i = 0;
    static int g_j9_snap_n = 0;
    static bool g_j9_resume_no_ac = false;
    static int g_j9_jcl_inited = 0;
    static kernel::chunk *g_j9_java_heap_ch = nullptr;
    static kernel::chunk *g_j9_heap_grow_ch = nullptr;
    static address g_j9_java_heap_va = 0;
    static std::uint32_t g_j9_java_heap_off = 0;
    static address g_j9_official_heap = 0;
    static address g_j9_thread_class = 0;
    static address g_j9_thread_obj = 0;
    static constexpr std::uint32_t k_j9_java_heap_size = 0x100000u;
    static constexpr address k_j9_alloc_object = 0x818EF8A4u;
    static constexpr address k_j9_alloc_indexable = 0x818EF9C0u;
    // Raw bump-pointer grab used by both allocateObject and
    // allocateIndexableObject. Hook this so the official wrappers still
    // write clazz/flags/lockword.
    static constexpr address k_j9_alloc_memory = 0x81912A4Cu;
    static constexpr address k_j9_thread_obj_off = 0x68u;
    static constexpr address k_j9_current_thread = 0x819409BCu;
    static constexpr address k_j9_init_loop = 0x8193EA38u;
    static constexpr address k_j9_init_ret = 0x8193EB60u;
    static address g_j9_resume_at = 0;
    static int g_j9_unbound_retry = 0;
    static address g_j9_good_r4 = 0;
    static address g_j9_good_r6 = 0;
    static address g_j9_method_start = 0;
    static address g_j9_force_caller = 0;
    static address g_j9_jcl_r4 = 0;
    static address g_j9_jcl_r5 = 0;
    static address g_j9_jcl_r6 = 0;
    static address g_j9_pending_ac_r6 = 0;
    static address g_j9_last_java_obj = 0;
    static address g_j9_string_clazz = 0;
    static address g_j9_char_array_clazz = 0;
    static address g_j9_string_array_clazz = 0;
    static address g_j9_encoding_str = 0;
    static address g_j9_proplist = 0;
    static address g_j9_system_clazz = 0;
    static address g_j9_system_r4 = 0;
    static address g_j9_system_r5 = 0;
    static address g_j9_system_r6 = 0;
    static address g_j9_system_r7 = 0;
    static address g_j9_system_method = 0;
    static int g_j9_encoding_n = 0;
    static bool g_j9_ht_filled = false;
    static address g_j9_system_ht = 0;
    static char g_j9_ht_keys[40][48];
    static address g_j9_ht_vals[40];
    static int g_j9_ht_n = 0;
    static int g_j9_throw_skips = 0;
    static address g_j9_last_skip_pc = 0;
    static address g_j9_system_frame = 0;
    static address g_j9_init_return = 0;
    static bool g_j9_init_tail = false;
    static address g_j9_init_caller_r4 = 0;
    static address g_j9_init_caller_r6 = 0;
    static address g_j9_init_caller_r7 = 0;
    static address g_j9_ac_fp = 0;
    static address g_j9_ac_w[6] = {};
    static address g_j9_init_glue = 0;
    static address g_j9_init_sp = 0;
    static address g_j9_caller_hdr[16] = {};
    static address g_j9_cframe[64] = {};
    static bool g_j9_cframe_ok = false;
    static bool g_j9_init_returned = false;
    static address g_j9_sys_sp = 0;
    static address g_j9_sys_frame[96] = {};
    static bool g_j9_sys_ok = false;
    static address g_j9_wrap_java_fp = 0;
    static address g_j9_wrap_sp34 = 0;
    static address g_j9_wrap_sp44 = 0;
    static address g_j9_wrap_r4 = 0;
    static address g_j9_wrap_r5 = 0;
    static address g_j9_wrap_r6 = 0;
    static address g_j9_wrap_r7 = 0;
    static address g_j9_wrap_t0 = 0;
    static address g_j9_wrap_t1 = 0;
    static address g_j9_wrap_t2 = 0;
    static address g_j9_wrap_clazz = 0;
    static address g_j9_last_jcl_clazz = 0;
    static address g_j9_converter_clazz = 0;
    static address g_j9_converter_dummy = 0;
    static bool g_j9_util_conv_done = false;
    static bool g_j9_string_astore_done = false;
    static bool g_j9_string_filled = false;
    static bool g_j9_init_c_returned = false;
    static address g_j9_cc_last_pc = 0;
    static int g_j9_cc_same_n = 0;
    static address g_j9_str_ret_r4 = 0;
    static address g_j9_str_ret_pc = 0;
    static address g_j9_str_ret_fp = 0;
    static bool g_j9_str_ret_ok = false;
    static address g_j9_conv_caller_r4 = 0;
    static address g_j9_conv_caller_r5 = 0;
    static address g_j9_conv_caller_r6 = 0;
    static address g_j9_conv_caller_r7 = 0;
    static address g_j9_live_r4 = 0;
    static address g_j9_live_r5 = 0;
    static address g_j9_live_r6 = 0;
    static address g_j9_live_r7 = 0;
    static address g_j9_caller_live_r4 = 0;
    static address g_j9_caller_live_r5 = 0;
    static address g_j9_caller_live_r6 = 0;
    static address g_j9_caller_live_r7 = 0;
    static address g_j9_boot_t0 = 0;
    static address g_j9_boot_t1 = 0;
    static address g_j9_boot_t2 = 0;
    static address g_j9_boot_fp = 0;
    static address g_j9_boot_csp = 0;
    static address g_j9_boot_cframe[16] = {};
    static bool g_j9_boot_returned = false;
    static address g_j9_live_csp = 0;
    static address g_j9_live_cframe[16] = {};
    static bool g_j9_live_cframe_ok = false;
    static address g_j9_consumed_csp = 0;
    static address g_j9_inl_r4 = 0;
    static address g_j9_inl_r6 = 0;
    static address g_j9_inl_r7 = 0;
    struct j9_init_frame {
        address clazz;
        address r4;
        address r6;
        address r7;
        address csp;
        address cframe[16];
    };
    static j9_init_frame g_j9_init_stack[8] = {};
    static int g_j9_init_depth = 0;
    static j9_init_frame g_j9_pending_ret = {};
    static bool g_j9_pending_clinit_ret = false;
    static address g_j9_outer_r4 = 0;
    static address g_j9_outer_r5 = 0;
    static address g_j9_outer_r6 = 0;
    static address g_j9_outer_r7 = 0;
    static address g_j9_skip_init_clazz = 0;
    static address g_j9_last_interp_pc = 0;
    static bool g_j9_wrap_fp_ok = false;
    static address g_j9_last_wrap_t1 = 0;
    static address g_j9_last_wrap_fp = 0;
    static address g_j9_jcl_this = 0;
    static address g_j9_dummy_array = 0;
    static address g_j9_jcl_outer_r4 = 0;
    static address g_j9_jcl_outer_r5 = 0;
    static address g_j9_jcl_outer_r6 = 0;
    static bool g_j9_jcl_returned = false;
    static address g_j9_midlet_this = 0;
    static address g_j9_display_obj = 0;
    static bool g_j9_alps_started = false;
    static int g_j9_alps_phase = 0;
    static address g_j9_alps_clazz = 0;
    static address g_j9_main_ret_sp = 0;
    static address g_j9_main_ret_lr = 0;
    static address g_j9_park_pc = 0;
    static address g_j9_meth_ptr[64] = {};
    static address g_j9_meth_ad[64] = {};
    static int g_j9_meth_n = 0;

    static void j9_remember_adapter(const address method, const address ad) {
        if (!method || !ad) {
            return;
        }
        for (int i = 0; i < g_j9_meth_n; ++i) {
            if (g_j9_meth_ptr[i] == method) {
                g_j9_meth_ad[i] = ad;
                return;
            }
        }
        if (g_j9_meth_n < 64) {
            g_j9_meth_ptr[g_j9_meth_n] = method;
            g_j9_meth_ad[g_j9_meth_n] = ad;
            ++g_j9_meth_n;
        }
    }

    static address j9_adapter_for_method(const address method) {
        for (int i = 0; i < g_j9_meth_n; ++i) {
            if (g_j9_meth_ptr[i] == method) {
                return g_j9_meth_ad[i];
            }
        }
        return 0;
    }

    static void sync_j9_guest_jni_table() {
        if (!g_j9_walk_ch || !g_j9_walk_ch->host_base() || !g_j9_walk_pairs || !g_j9_walk_va) {
            return;
        }
        auto *base = reinterpret_cast<std::uint8_t *>(g_j9_walk_ch->host_base());
        const std::uint32_t pairs_off = g_j9_walk_pairs - g_j9_walk_va;
        if (pairs_off < 4) {
            return;
        }
        auto *p = reinterpret_cast<std::uint32_t *>(base + pairs_off);
        const std::uint32_t maxn = (0x8000u - pairs_off) / 8u;
        std::uint32_t n = 0;
        for (const auto &ent : j9_jni_exports) {
            if (n >= maxn) {
                break;
            }
            const auto &nm = ent.name;
            // type=JXE is used for JCL as well as midp2ams, so every
            // collected Java_* must be visible to sl_lookup. Walker match
            // is exact / Java_+5, so short JCL names cannot collide.
            const bool midp_native = (nm.compare(0, 5, "Java_") == 0)
                && ((nm.find("symbian") != std::string::npos)
                    || (nm.find("nokia") != std::string::npos) || (nm.find("lcdui") != std::string::npos)
                    || (nm.find("midp") != std::string::npos));
            address fn = ent.fn;
            if (midp_native && g_j9_jni_fixer && g_j9_walk_va) {
                const std::uint32_t stub_off = k_j9_stub_off + n * k_j9_stub_size;
                if (stub_off + k_j9_stub_size <= 0x8000) {
                    auto *stub = base + stub_off;
                    const address stub_va = g_j9_walk_va + stub_off;
                    const address fixer = g_j9_jni_fixer;
                    const address real = ent.fn;
                    // Thumb PC is (instr+4)&~3. At +2 that is stub+4, so
                    // ldr r1,[pc,#16] loads fixer at +20. ldr.w ip,[pc,#4]
                    // at +8 loads real at +16.
                    stub[0] = 0x1E;
                    stub[1] = 0xB5; // push {r1-r3,lr}
                    stub[2] = 0x04;
                    stub[3] = 0x49; // ldr r1, [pc, #16] -> fixer
                    stub[4] = 0x88;
                    stub[5] = 0x47; // blx r1
                    stub[6] = 0x1E;
                    stub[7] = 0xBD; // pop {r1-r3,lr}
                    stub[8] = 0xDF;
                    stub[9] = 0xF8;
                    stub[10] = 0x04;
                    stub[11] = 0xC0; // ldr.w ip, [pc, #4] -> real
                    stub[12] = 0x60;
                    stub[13] = 0x47; // bx ip
                    stub[14] = 0x00;
                    stub[15] = 0x00;
                    std::memcpy(stub + 16, &real, 4);
                    std::memcpy(stub + 20, &fixer, 4);
                    // ARM adapter: interpreter `mov lr,pc; bx send` and
                    // `ldr pc,[send]` both land here with r0=J9Method* and
                    // r8=J9VMThread. Load JNIEnv, fix the table, call the
                    // Thumb native, return via lr (0x818F6D78 path).
                    const std::uint32_t ad_off = k_j9_adapt_off + n * k_j9_adapt_size;
                    if (ad_off + k_j9_adapt_size <= 0x8000) {
                        auto *ad = reinterpret_cast<std::uint32_t *>(base + ad_off);
                        // markTime's C++ JNI thunk corrupts the stacked lr
                        // without a full JNI frame. Return 0 for it only.
                        address call = real;
                        if (nm.find("markTime") != std::string::npos) {
                            call = (g_j9_walk_va + 0x27C) | 1u;
                        }
                        // r0=J9Method*. Build JNIEnv* (functions + vmthread
                        // in the reserved slots midp C++ reads at +8).
                        const address fake = g_j9_fake_env ? g_j9_fake_env : (g_j9_walk_va + 0x2B0);
                        ad[0] = 0xE92D4001u; // push {r0, lr}
                        ad[1] = 0xE59F0040u; // ldr r0, fake-env @0x4C
                        ad[2] = 0xE5808004u; // str r8, [r0, #4]
                        ad[3] = 0xE5808008u; // str r8, [r0, #8]
                        ad[4] = 0xE580800Cu; // str r8, [r0, #0xc]
                        ad[5] = 0xE1A00000u; // nop
                        ad[6] = 0xE1A00000u; // nop
                        ad[7] = 0xE1A00000u; // nop
                        ad[8] = 0xE59FC020u; // ldr ip, real @0x48
                        ad[9] = 0xE12FFF3Cu; // blx ip
                        ad[10] = 0xE8BD4001u; // pop {r0, lr}
                        ad[11] = 0xE1A0242Eu; // lsr r2, lr, #16
                        ad[12] = 0xE59F3018u; // ldr r3, =0x818F @0x50
                        ad[13] = 0xE1520003u; // cmp r2, r3
                        ad[14] = 0x1A000000u; // bne interp
                        ad[15] = 0xE12FFF1Eu; // bx lr
                        ad[16] = 0xE51FF004u; // ldr pc, [pc, #-4]
                        ad[17] = 0x8193A204u;
                        ad[18] = call;
                        ad[19] = fake;
                        ad[20] = 0x818Fu;
                        fn = g_j9_walk_va + ad_off;
                    } else {
                        fn = stub_va | 1u;
                    }
                    (void)stub_va;
                }
            }
            p[n * 2] = ent.name_ga;
            p[n * 2 + 1] = fn;
            ++n;
        }
        p[-1] = n;
        static std::uint32_t last_n = 0;
        if (n != last_n) {
            last_n = n;
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI guest table synced n={}", n);
        }
    }

    static void collect_j9_jni_exports(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        kernel_system *kern = cs->get_kernel_object_owner();
        kernel::process *pr = kern ? kern->crr_process() : nullptr;
        const std::uint32_t code_size = cs->get_code_size();
        if (cs->name().find("midp2ams") != std::string::npos) {
            g_j9_midp2ams_run = run_addr;
            g_j9_midp2ams_size = code_size;
        }
        const address lo = run_addr;
        const address hi = run_addr + ((code_size > 0x80000) ? 0x80000 : code_size);
        int added = 0;
        address run_start = 0;
        address run_prev = 0;
        int run_len = 0;
        address best_start = 0;
        int best_len = 0;
        for (address a = lo; (a + 8) <= hi; a += 4) {
            const auto *pair = reinterpret_cast<const std::uint32_t *>(
                pr ? pr->get_ptr_on_addr_space(a)
                   : ((a - run_addr + 8 <= 0x10000) ? (base + (a - run_addr)) : nullptr));
            if (!pair) {
                continue;
            }
            const address name_ga = pair[0];
            const address fn_ga = pair[1];
            if ((name_ga < lo) || (name_ga + 8 >= hi) || (fn_ga < lo) || (fn_ga >= hi)) {
                continue;
            }
            const char *s = reinterpret_cast<const char *>(
                pr ? pr->get_ptr_on_addr_space(name_ga)
                   : (base + (name_ga - run_addr)));
            if (!s || (std::memcmp(s, "Java_", 5) != 0)) {
                continue;
            }
            std::size_t n = 0;
            while ((s[n] != 0) && (n < 200)) {
                ++n;
            }
            if (n < 8) {
                continue;
            }
            j9_jni_export ent;
            ent.name.assign(s, n);
            ent.fn = fn_ga | 1u;
            ent.name_ga = name_ga;
            j9_jni_exports.push_back(std::move(ent));
            ++added;
            if (run_prev && (a == run_prev + 8)) {
                ++run_len;
            } else {
                run_start = a;
                run_len = 1;
            }
            run_prev = a;
            if (run_len > best_len) {
                best_len = run_len;
                best_start = run_start;
            }
        }
        if ((best_len >= 50) && (best_len > static_cast<int>(g_j9_jni_table_n))) {
            g_j9_jni_table_va = best_start;
            g_j9_jni_table_n = static_cast<std::uint32_t>(best_len);
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI pair table 0x{:X} count={} in {}",
                g_j9_jni_table_va, g_j9_jni_table_n, cs->name());
        }
        if (!g_j9_jnienv_table && (code_size > 0x200)) {
            // JNINativeInterface: 4 reserved zeros then many J9 code ptrs.
            // Slots may point into j9.dll or j9vmall, so accept the whole
            // 0x81800000–0x81C00000 ROM window, not just this codeseg.
            const std::uint32_t scan_hi = (code_size > 0x80000) ? 0x80000 : code_size;
            for (std::uint32_t i = 0; i + 4 * 180 <= scan_hi; i += 4) {
                const auto *w = reinterpret_cast<const std::uint32_t *>(
                    pr ? pr->get_ptr_on_addr_space(run_addr + i) : (base + i));
                if (!w || w[0] || w[1] || w[2] || w[3]) {
                    continue;
                }
                int ok = 0;
                for (int k = 4; k < 180; ++k) {
                    const address fn = w[k];
                    if ((fn < 0x81800000u) || (fn >= 0x81C00000u)) {
                        ok = -1;
                        break;
                    }
                    ++ok;
                }
                if (ok >= 160) {
                    g_j9_jnienv_table = run_addr + i;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNIEnv function table 0x{:X} in {}",
                        g_j9_jnienv_table, cs->name());
                    break;
                }
            }
            if (!g_j9_jnienv_table && (run_addr <= 0x8192C86Cu) && (0x8192C86Cu < hi)) {
                g_j9_jnienv_table = 0x8192C86Cu;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNIEnv function table fallback 0x{:X} in {}",
                    g_j9_jnienv_table, cs->name());
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] collect {} run=0x{:X} size=0x{:X} jnienv=0x{:X}",
                cs->name(), run_addr, code_size, g_j9_jnienv_table);
            if (cs->name().find("midp2ams") != std::string::npos) {
                g_j9_midp2ams_run = run_addr;
                g_j9_midp2ams_size = code_size;
            }
        }
        static const std::uint8_t pk[] = { 0x50, 0x4B, 0x03, 0x04 };
        static const std::uint8_t j99j[] = { 0x4A, 0x39, 0x39, 0x4A };
        for (std::uint32_t i = 0; (i + 52) <= code_size; ++i) {
            const std::uint8_t *p = pr
                ? reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(run_addr + i))
                : ((i + 52 < 0x20000) ? (base + i) : nullptr);
            if (!p || (std::memcmp(p, pk, 4) != 0) || (std::memcmp(p + 48, j99j, 4) != 0)) {
                continue;
            }
            g_j9_midp2ams_jxe = run_addr + i;
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] midp2ams embedded ZIP at 0x{:X} in {}",
                g_j9_midp2ams_jxe, cs->name());
            break;
        }
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] collected {} JNI exports from {}", added, cs->name());
        sync_j9_guest_jni_table();
    }

    static void collect_j9_jni_rom_table(kernel::process *pr, const address table_va,
        const int max_pairs, const char *tag) {
        if (!pr || !table_va) {
            return;
        }
        int added = 0;
        for (int i = 0; i < max_pairs; ++i) {
            const auto *pair = reinterpret_cast<const std::uint32_t *>(
                pr->get_ptr_on_addr_space(table_va + static_cast<address>(i) * 8u));
            if (!pair) {
                break;
            }
            const address name_ga = pair[0];
            const address fn_ga = pair[1];
            if ((name_ga < 0x81800000u) || (name_ga >= 0x81C00000u) || (fn_ga < 0x81800000u)
                || (fn_ga >= 0x81C00000u)) {
                break;
            }
            const char *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(name_ga));
            if (!s || (std::memcmp(s, "Java_", 5) != 0)) {
                break;
            }
            std::size_t n = 0;
            while ((s[n] != 0) && (n < 200)) {
                ++n;
            }
            if (n < 8) {
                break;
            }
            bool seen = false;
            for (const auto &ent : j9_jni_exports) {
                if (ent.name_ga == name_ga) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            j9_jni_export ent;
            ent.name.assign(s, n);
            ent.fn = fn_ga | 1u;
            ent.name_ga = name_ga;
            j9_jni_exports.push_back(std::move(ent));
            ++added;
        }
        if (added) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] collected {} JNI exports from ROM {} @0x{:X}",
                added, tag, table_va);
        }
    }

    static void collect_j9_jni_name_fn_lists(kernel::process *pr, const address names_va,
        const address fns_va, const int count, const char *tag) {
        if (!pr || !names_va || !fns_va || (count <= 0)) {
            return;
        }
        int added = 0;
        for (int i = 0; i < count; ++i) {
            const auto *np = reinterpret_cast<const std::uint32_t *>(
                pr->get_ptr_on_addr_space(names_va + static_cast<address>(i) * 4u));
            const auto *fp = reinterpret_cast<const std::uint32_t *>(
                pr->get_ptr_on_addr_space(fns_va + static_cast<address>(i) * 4u));
            if (!np || !fp) {
                break;
            }
            const address name_ga = *np;
            const address fn_ga = *fp;
            if ((name_ga < 0x81800000u) || (name_ga >= 0x81C00000u) || (fn_ga < 0x81800000u)
                || (fn_ga >= 0x81C00000u)) {
                continue;
            }
            const char *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(name_ga));
            if (!s || (s[0] < 'A') || (s[0] > 'z')) {
                continue;
            }
            std::size_t n = 0;
            while ((s[n] != 0) && (n < 200)) {
                ++n;
            }
            if (n < 6) {
                continue;
            }
            if ((n == 11) && (std::memcmp(s, "J9VMDllMain", 11) == 0)) {
                j9_jcl_vm_dllmain = fn_ga | 1u;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JCL J9VMDllMain 0x{:X}", j9_jcl_vm_dllmain);
                continue;
            }
            if ((n == 10) && (std::memcmp(s, "JCL_OnLoad", 10) == 0)) {
                j9_jcl_onload = fn_ga | 1u;
                continue;
            }
            if ((n == 10) && (std::memcmp(s, "JVM_OnLoad", 10) == 0)) {
                j9_jcl_jvm_onload = fn_ga | 1u;
                continue;
            }
            if ((n == 12) && (std::memcmp(s, "JNI_OnUnload", 12) == 0)) {
                j9_jcl_jni_onunload = fn_ga | 1u;
                continue;
            }
            if ((n == 12) && (std::memcmp(s, "JVM_OnUnload", 12) == 0)) {
                continue;
            }
            // Unprefixed names (java_lang_Object_getClass) are ARM
            // interpreter INL handlers. bindNative copies extra→send and
            // the interpreter bx's send with r5/r7, so these even ARM
            // addresses must be visible to sl_lookup. Do not Thumb-tag
            // them and do not wrap them as JNI.
            if (std::memcmp(s, "Java_", 5) != 0) {
                bool seen = false;
                for (const auto &ent : j9_jni_exports) {
                    if ((ent.name_ga == name_ga)
                        || ((ent.name.size() == n) && (ent.name.compare(0, n, s) == 0))) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    j9_jni_export ent;
                    ent.name.assign(s, n);
                    ent.fn = fn_ga;
                    ent.name_ga = name_ga;
                    j9_jni_exports.push_back(std::move(ent));
                    ++added;
                }
                continue;
            }
            bool seen = false;
            for (const auto &ent : j9_jni_exports) {
                if ((ent.name_ga == name_ga) || (ent.name.size() == n && ent.name.compare(0, n, s) == 0)) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            j9_jni_export ent;
            ent.name.assign(s, n);
            // JCL unprefixed natives are ARM (even); Java_* midp entries are Thumb.
            ent.fn = fn_ga;
            ent.name_ga = name_ga;
            j9_jni_exports.push_back(std::move(ent));
            ++added;
        }
        if (added) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] collected {} JNI exports from {} lists n/fn=0x{:X}/0x{:X}",
                added, tag, names_va, fns_va);
            for (const auto &ent : j9_jni_exports) {
                if ((ent.name.find("System") != std::string::npos)
                    || (ent.name.find("getProperty") != std::string::npos)
                    || (ent.name.find("Converter") != std::string::npos)
                    || (ent.name.find("PrintStream") != std::string::npos)
                    || (ent.name.find("Console") != std::string::npos)) {
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] jcl-sym '{}' -> 0x{:X}", ent.name, ent.fn);
                }
            }
        }
    }

    static address j9_lookup_jni_export(const char *name) {
        if (!name || !name[0]) {
            return 0;
        }
        for (const auto &ent : j9_jni_exports) {
            if (ent.name == name) {
                return ent.fn;
            }
        }
        const std::size_t nlen = std::strlen(name);
        if ((nlen >= 3) && (nlen < 80) && ((nlen < 5) || (std::memcmp(name, "Java_", 5) != 0))) {
            for (const auto &ent : j9_jni_exports) {
                if ((ent.name.size() > nlen) && (ent.name.compare(ent.name.size() - nlen, nlen, name) == 0)
                    && (ent.name[ent.name.size() - nlen - 1] == '_')) {
                    return ent.fn;
                }
            }
            // JNI escapes leading '_' in a method as "_1", so `_markTime`
            // lives in the table as `...Main__1markTime`.
            if (name[0] == '_') {
                const std::string escaped = std::string("__1") + (name + 1);
                for (const auto &ent : j9_jni_exports) {
                    if ((ent.name.size() > escaped.size())
                        && (ent.name.compare(ent.name.size() - escaped.size(), escaped.size(), escaped) == 0)) {
                        return ent.fn;
                    }
                }
            } else {
                // ROM method name `markTime` vs JNI `...Main__1markTime`.
                const std::string escaped = std::string("__1") + name;
                for (const auto &ent : j9_jni_exports) {
                    if ((ent.name.size() > escaped.size())
                        && (ent.name.compare(ent.name.size() - escaped.size(), escaped.size(), escaped) == 0)) {
                        return ent.fn;
                    }
                }
            }
        }
        return 0;
    }

    // sl_lookup often passes the method name without the JNI signature
    // (`convertImpl` vs `convertImpl___3BI_3CII`). Match query + "___" + sig.
    static void j9_bindnatv_bkpt(arm::core *core, kernel::thread *thr, const std::uint32_t addr);

    static void j9_set_pc(arm::core *core, const address fn) {
        if (!core) {
            return;
        }
        if (fn & 1u) {
            core->set_cpsr(core->get_cpsr() | 0x20u);
            core->set_pc(fn & ~1u);
        } else {
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(fn);
        }
    }

    static constexpr address k_j9_inl_dispatch = 0x81939FD8u;
    // Official bytecode table in j9vmall23. C2/C3 are monitorenter/exit.
    static constexpr address k_j9_opcode_table = 0x818F5F70u;
    // JCL INL snippets (ARM). They expect r2=opcode table and then
    // `ldr pc, [r2, op, lsl #2]`. tramp-fwd jumps here with r2 still
    // midp2ams .data (0x3FFF8E28), which re-enters initialize/verify
    // until StackOverflowError. Host-emulate the status word at Class+0x28.
    static constexpr address k_j9_inl_get_init_status = 0x8193DCE0u;
    static constexpr address k_j9_inl_get_init_thread = 0x8193DD04u;
    static constexpr address k_j9_inl_set_init_status = 0x8193DEE0u;
    static constexpr address k_j9_inl_set_init_thread = 0x8193DEFCu;
    static constexpr address k_j9_inl_verify_impl = 0x8193DF1Cu;
    static constexpr address k_j9_inl_init_impl = 0x8193DD2Cu;
    static constexpr address k_j9_inl_prepare_event = 0x8193DE1Cu;

    static bool j9_fn_is_code(const address fn) {
        const address bare = fn & ~1u;
        if (bare < 0x10000u) {
            return false;
        }
        // walk+0x304..+0x1000 is the JNI name/fn pair table, not code.
        if (g_j9_walk_va && (bare >= (g_j9_walk_va + 0x300u)) && (bare < (g_j9_walk_va + 0x1000u))) {
            return false;
        }
        // midp2ams JNI name pool (Java_com_...), not executable.
        if ((bare >= 0x81AB2000u) && (bare < 0x81AB8000u)) {
            return false;
        }
        return true;
    }

    // bx-sb must not treat RAM data (0x03D00030, java heap, midp .data) as
    // a helper. Only ROM XIP and the host trampoline are executable.
    static bool j9_fn_is_exec(const address fn) {
        const address bare = fn & ~1u;
        if ((bare >= 0x80000000u) && (bare < 0x82000000u)) {
            return (bare < 0x81AB2000u) || (bare >= 0x81AB8000u);
        }
        if (g_j9_walk_va && (bare >= g_j9_walk_va) && (bare < (g_j9_walk_va + 0x8000u))) {
            return (bare < (g_j9_walk_va + 0x300u)) || (bare >= (g_j9_walk_va + 0x1000u));
        }
        return false;
    }

    static address j9_read32(kernel::process *pr, const address p);
    static bool j9_mapped32(kernel::process *pr, const address p);
    static void j9_write32(kernel::process *pr, const address p, const address v);

    static address j9_official_opcode_table(kernel::process *pr, const address vt) {
        address tab = k_j9_opcode_table;
        if (pr && vt && j9_mapped32(pr, vt + 4u)) {
            const address vm = j9_read32(pr, vt + 4u);
            if (vm && pr->get_ptr_on_addr_space(vm + 0xa18u)) {
                const address t = j9_read32(pr, vm + 0xa18u);
                if (t && (t != 0x3FFF8E28u) && (t >= 0x80000000u) && (t < 0x82000000u)
                    && pr->get_ptr_on_addr_space(t)) {
                    tab = t;
                }
            }
        }
        return tab;
    }

    static void j9_restore_interp_table(arm::core *core, kernel::process *pr, const address vt,
        const bool set_r2) {
        const address tab = j9_official_opcode_table(pr, vt);
        if (set_r2 && core) {
            core->set_reg(2, tab);
        }
        if (pr && vt && j9_mapped32(pr, vt + 0xcu)) {
            j9_write32(pr, vt + 0xcu, tab);
        }
    }

    // extra==1 is the J9 interpreter's "call JNI" marker. extra==odd-and-large
    // is an INL CAS id, not a function pointer. Thumb JNI plants extra=1 and
    // send=an ARM adapter (or the official JNI entry 0x8193A0AC as fallback).
    static constexpr address k_j9_jni_entry = 0x8193A0ACu;

    static address j9_read32(kernel::process *pr, const address p) {
        if (!pr || (p < 0x1000u)) {
            return 0;
        }
        const auto *w = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(p));
        return w ? *w : 0;
    }

    static bool j9_mapped32(kernel::process *pr, const address p) {
        return pr && (p >= 0x10000u) && (p < 0x80000000u)
            && (pr->get_ptr_on_addr_space(p) != nullptr);
    }

    static bool j9_readable(kernel::process *pr, const address p) {
        return pr && p && (pr->get_ptr_on_addr_space(p) != nullptr);
    }

    // LCDUI natives do `lsls r0, r2, #2` then treat r0 as a Java object
    // (ldr [r0, #8]). J9 ARM stores refs as (ptr >> 2) on the interpreter
    // stack; full heap pointers need the same shift before the native.
    static bool j9_looks_heap(const address p) {
        return (p >= 0x01000000u) && (p < 0x08000000u);
    }

    static address j9_pack_obj(kernel::process *pr, const address raw) {
        if (!pr || (raw < 4u)) {
            return 0;
        }
        const address shifted = raw << 2;
        if (j9_looks_heap(shifted) && j9_mapped32(pr, shifted) && j9_mapped32(pr, shifted + 8u)) {
            return raw;
        }
        if (j9_looks_heap(raw) && ((raw & 3u) == 0) && j9_mapped32(pr, raw)
            && j9_mapped32(pr, raw + 8u)) {
            return raw >> 2;
        }
        // jobject is a pointer to a stack slot holding the object.
        if (((raw & 3u) == 0) && j9_mapped32(pr, raw) && (raw < 0x01000000u)) {
            const address v = j9_read32(pr, raw);
            if (j9_looks_heap(v) && j9_mapped32(pr, v) && j9_mapped32(pr, v + 8u)) {
                return v >> 2;
            }
        }
        return 0;
    }

    static bool j9_obj_has_peer(kernel::process *pr, const address packed) {
        if (!packed) {
            return false;
        }
        const address obj = packed << 2;
        const address peer = j9_read32(pr, obj + 8u);
        return peer && (peer >= 0x400000u) && j9_mapped32(pr, peer)
            && j9_mapped32(pr, peer + 8u);
    }

    static void j9_write32(kernel::process *pr, const address p, const address v) {
        if (auto *w = reinterpret_cast<std::uint32_t *>(
                pr ? pr->get_ptr_on_addr_space(p) : nullptr)) {
            *w = v;
        }
    }

    static address j9_alloc_java_obj(kernel::process *pr, const address clazz);
    static address j9_host_alloc_obj(kernel::process *pr, const address clazz, unsigned nbytes);
    static void j9_mark_jcl_supers(kernel::process *pr, const address clazz);
    static void j9_plant_main_thread(kernel::process *pr, const address clazz);
    static address j9_find_valid_cp(kernel::process *pr, const address ptr) {
        if (!pr || (ptr < 0x10000u) || !j9_mapped32(pr, ptr) || !j9_mapped32(pr, ptr + 8u)) {
            return 0;
        }
        const address p = ptr & ~7u;
        const address rom4 = j9_read32(pr, p + 4u);
        if (rom4 >= 0x80000000u) {
            return p;
        }
        for (int i = 0; i < 48; ++i) {
            const address cand = j9_read32(pr, p + static_cast<address>(i) * 4u);
            if ((cand > 0x10000u) && j9_mapped32(pr, cand) && j9_mapped32(pr, cand + 8u)) {
                const address c_rom4 = j9_read32(pr, (cand & ~7u) + 4u);
                if (c_rom4 >= 0x80000000u) {
                    return cand & ~7u;
                }
            }
        }
        return 0;
    }

    static void j9_ensure_method_cp(arm::core *core, kernel::process *pr) {
        if (!core || !pr) return;
        const address r4 = core->get_reg(4);
        if (r4 && (r4 > 0x10000u) && j9_mapped32(pr, r4) && j9_mapped32(pr, r4 + 12u)) {
            const address raw_cp = j9_read32(pr, r4 + 4u);
            address valid = j9_find_valid_cp(pr, raw_cp);
            if (!valid && g_j9_valid_cp) {
                valid = g_j9_valid_cp;
            }
            if (valid) {
                g_j9_valid_cp = valid;
                if ((raw_cp & ~7u) != valid) {
                    j9_write32(pr, r4 + 4u, valid);
                }
            }
        }
    }

    // Dummy C++ peer for Nokia LCDUI objects. Real peers are created by
    // Toolkit._create via CJavaEventSource; that path never ran before
    // Graphics._create, so [obj+8] is 0 and 0x81A61CC4 does
    // ldr [factory+8]; +8 → User path AV at address 8.
    static constexpr std::uint32_t k_j9_vt_off = 0x6000;
    static constexpr std::uint32_t k_j9_peer_off = 0x6080;
    static constexpr std::uint32_t k_j9_peer_size = 80;
    static constexpr int k_j9_peer_max = 24;

    static address j9_alloc_dummy_peer(kernel::process *pr, kernel::thread *thr) {
        if (!g_j9_walk_va || !g_j9_walk_ch || !g_j9_walk_ch->host_base()) {
            return 0;
        }
        auto *base = reinterpret_cast<std::uint8_t *>(g_j9_walk_ch->host_base());
        auto *vt = reinterpret_cast<std::uint32_t *>(base + k_j9_vt_off);
        if (vt[0] == 0) {
            for (int i = 0; i < 32; ++i) {
                vt[i] = 0xE12FFF1Eu; // ARM bx lr
            }
        }
        const int slot = g_j9_peer_n % k_j9_peer_max;
        ++g_j9_peer_n;
        const std::uint32_t off = k_j9_peer_off + static_cast<std::uint32_t>(slot) * k_j9_peer_size;
        auto *p = reinterpret_cast<std::uint32_t *>(base + off);
        std::memset(p, 0, k_j9_peer_size);
        const address peer = g_j9_walk_va + off;
        address heap = 0;
        if (thr) {
            if (kernel::thread_local_data *ld = thr->get_local_data()) {
                heap = ld->heap.ptr_address();
            }
        }
        p[0] = g_j9_walk_va + k_j9_vt_off;
        p[2] = heap ? heap : peer;
        return peer;
    }

    static void j9_seed_midp_bss_types(kernel::process *pr, kernel::thread *thr) {
        if (!pr) {
            return;
        }
        const address peer = j9_alloc_dummy_peer(pr, thr);
        if (!peer) {
            return;
        }
        // midp2ams .data slots: `ldr r0,=0x3fff00xx; sub #4; ldr r0,[r0]`.
        // 0x3FFF0030 is an RThread* used by the SetPriority wrappers at
        // 0x81A61F5C / 0x81A61F6E (r1 = ±10). Other slots are C++
        // factories (vtable dispatch). Dummy peer has a bx-lr vtable.
        static const address k_slots[] = {
            0x3FFF0000u, 0x3FFF0004u, 0x3FFF0014u, 0x3FFF0018u,
            0x3FFF001Cu, 0x3FFF0020u, 0x3FFF0024u, 0x3FFF0028u,
            0x3FFF002Cu, 0x3FFF0030u, 0x3FFF0034u, 0x3FFF0038u,
            0x3FFF003Cu,
        };
        int n = 0;
        address seen[4] = {};
        int ns = 0;
        for (address s : k_slots) {
            if (!j9_mapped32(pr, s)) {
                continue;
            }
            const address cur = j9_read32(pr, s);
            if (ns < 4) {
                seen[ns++] = cur;
            }
            if (cur == 0) {
                j9_write32(pr, s, peer);
                ++n;
            }
        }
        static int seed_logs = 0;
        if ((n || seed_logs < 2) && (seed_logs < 6)) {
            ++seed_logs;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] seeded {} midp BSS type slots peer=0x{:X} mapped={} [0]=0x{:X} [1]=0x{:X} [2]=0x{:X} [3]=0x{:X}",
                n, peer, ns, seen[0], seen[1], seen[2], seen[3]);
        }
    }

    static address j9_attach_dummy_peer(kernel::process *pr, kernel::thread *thr, const address packed) {
        if (!pr || !packed) {
            return 0;
        }
        const address obj = packed << 2;
        if (j9_obj_has_peer(pr, packed)) {
            return j9_read32(pr, obj + 8u);
        }
        const address peer = j9_alloc_dummy_peer(pr, thr);
        if (peer) {
            j9_write32(pr, obj + 8u, peer);
        }
        return peer;
    }

    static std::uint16_t j9_lcdui_orig_push(const address pc) {
        switch (pc) {
        case 0x81AF1616u:
            return 0xB51C;
        case 0x81AF2AACu:
        case 0x81AF17F0u:
        case 0x81AF106Au:
            return 0xB5FF;
        case 0x81AEE7F8u:
            return 0xB5F0;
        case 0x81A61CC4u:
            return 0xB40F;
        case 0x81AF2C12u:
        case 0x819171F0u:
            return 0xB570;
        default:
            return 0xB51C;
        }
    }

    // Replay the Thumb push we replaced with BKPT, then continue the
    // real LCDUI native. Used when the hit is not a packable _create.
    static void j9_thumb_continue_real(arm::core *core, kernel::process *pr,
        const address pc) {
        if (!core || !pr) {
            return;
        }
        const std::uint16_t insn = j9_lcdui_orig_push(pc);
        const unsigned mask = insn & 0x1FFu;
        int n = 0;
        for (int i = 0; i < 9; ++i) {
            if (mask & (1u << i)) {
                ++n;
            }
        }
        address sp = core->get_reg(13) - static_cast<address>(n) * 4u;
        address slot = sp;
        for (int i = 0; i < 8; ++i) {
            if (mask & (1u << i)) {
                j9_write32(pr, slot, core->get_reg(i));
                slot += 4;
            }
        }
        if (mask & 0x100u) {
            j9_write32(pr, slot, core->get_lr());
        }
        core->set_reg(13, sp);
        core->set_cpsr(core->get_cpsr() | 0x20u);
        core->set_pc(pc + 2u);
    }

    static unsigned j9_method_argc(kernel::process *pr, const address method) {
        unsigned argc = 1;
        if (!pr || (method < 0x10000u)) {
            return argc;
        }
        const auto *mw = reinterpret_cast<const std::uint32_t *>(
            pr->get_ptr_on_addr_space(method));
        if (!mw || !mw[0]) {
            return argc;
        }
        if (const auto *hb = reinterpret_cast<const std::uint8_t *>(
                pr->get_ptr_on_addr_space(mw[0] - 3u))) {
            if ((hb[0] >= 1u) && (hb[0] <= 8u)) {
                argc = hb[0];
            }
        }
        return argc;
    }

    static bool j9_r8_looks_vmthread(kernel::process *pr, const address r8,
        const address method) {
        if (!pr || !r8 || (r8 == method) || !j9_mapped32(pr, r8 + 4u)
            || !j9_mapped32(pr, r8 + 0x10u)) {
            return false;
        }
        const address w8 = j9_read32(pr, r8 + 8u);
        if ((w8 == k_j9_jni_entry) || (w8 == 8u) || (w8 == k_j9_inl_dispatch)) {
            return false;
        }
        if (g_j9_walk_va && (w8 >= (g_j9_walk_va + k_j9_adapt_off))
            && (w8 < (g_j9_walk_va + 0x8000u))) {
            return false;
        }
        const address vm = j9_read32(pr, r8 + 4u);
        return vm && j9_mapped32(pr, vm + 0xa18u);
    }

    static std::uint8_t j9_read8(kernel::process *pr, const address p) {
        if (!pr || (p < 0x1000u)) {
            return 0;
        }
        const auto *b = reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(p));
        return b ? *b : 0;
    }

    static bool j9_is_invoke_op(const std::uint8_t op) {
        return (op == 0xB6u) || (op == 0xB7u) || (op == 0xB8u) || (op == 0xB9u)
            || (op == 0xD0u) || (op == 0xD1u);
    }

    static bool j9_is_cp_op(const std::uint8_t op) {
        return j9_is_invoke_op(op) || (op == 0xB2u) || (op == 0xB3u)
            || (op == 0xB4u) || (op == 0xB5u);
    }

    static bool j9_is_wrap_caller_op(const std::uint8_t op) {
        return j9_is_invoke_op(op) || (op == 0xBBu) || (op == 0xBCu)
            || (op == 0xBDu);
    }

    static address j9_jxe_page(const address pc) {
        return pc & ~0x1FFu;
    }

    static void j9_save_jxe_page(const address pc, const address r4, const address r6) {
        if ((pc < 0x00770000u) || (pc >= 0x00780000u)) {
            return;
        }
        const address key = j9_jxe_page(pc);
        for (int i = 0; i < g_j9_page_n; ++i) {
            if (g_j9_page_key[i] == key) {
                // Keep the first class/locals seen on this page. A later
                // unbound invoke (method 0x60001) would otherwise overwrite
                // them with the callee J9Method*.
                g_j9_page_r5[i] = pc;
                return;
            }
        }
        if (g_j9_page_n < k_j9_page_max) {
            const int i = g_j9_page_n++;
            g_j9_page_key[i] = key;
            g_j9_page_r4[i] = r4;
            g_j9_page_r6[i] = r6;
            g_j9_page_r5[i] = pc;
        }
    }

    static bool j9_find_jxe_page(const address pc, address *r4, address *r6) {
        const address key = j9_jxe_page(pc);
        for (int i = 0; i < g_j9_page_n; ++i) {
            if (g_j9_page_key[i] == key) {
                if (r4) {
                    *r4 = g_j9_page_r4[i];
                }
                if (r6) {
                    *r6 = g_j9_page_r6[i];
                }
                return true;
            }
        }
        return false;
    }

    static void j9_save_snap(const address pc, const address r4, const address r6) {
        if ((pc < 0x00770000u) || (pc >= 0x00780000u)) {
            return;
        }
        for (int i = 0; i < g_j9_snap_n; ++i) {
            if (g_j9_snap_pc[i] == pc) {
                g_j9_snap_r4[i] = r4;
                g_j9_snap_r6[i] = r6;
                return;
            }
        }
        const int i = (g_j9_snap_n < k_j9_snap_max) ? g_j9_snap_n++
            : (g_j9_snap_i++ % k_j9_snap_max);
        g_j9_snap_pc[i] = pc;
        g_j9_snap_r4[i] = r4;
        g_j9_snap_r6[i] = r6;
    }

    static bool j9_find_jxe_caller(const address pc, address *r4, address *r5,
        address *r6);

    static bool j9_find_snap(const address pc, address *r4, address *r6) {
        int best = -1;
        int best_d = 0x7fffffff;
        for (int i = 0; i < g_j9_snap_n; ++i) {
            if (j9_jxe_page(g_j9_snap_pc[i]) != j9_jxe_page(pc)) {
                continue;
            }
            const int d = std::abs(static_cast<int>(g_j9_snap_pc[i])
                - static_cast<int>(pc));
            if (d < best_d) {
                best = i;
                best_d = d;
            }
        }
        if ((best >= 0) && (best_d <= 0x80)) {
            if (r4) {
                *r4 = g_j9_snap_r4[best];
            }
            if (r6) {
                *r6 = g_j9_snap_r6[best];
            }
            return true;
        }
        return j9_find_jxe_page(pc, r4, r6);
    }

    static bool j9_pick_caller_regs(const address pc, address *r4, address *r6) {
        address cr4 = 0;
        address cr5 = 0;
        address cr6 = 0;
        if (j9_find_jxe_caller(pc, &cr4, &cr5, &cr6) && cr4) {
            if (r4) {
                *r4 = cr4;
            }
            if (r6) {
                *r6 = cr6;
            }
            return true;
        }
        if (j9_find_jxe_page(pc, r4, r6)) {
            return true;
        }
        if (g_j9_good_r4) {
            if (r4) {
                *r4 = g_j9_good_r4;
            }
            if (r6) {
                *r6 = g_j9_good_r6;
            }
            return true;
        }
        return false;
    }

    static void j9_save_jxe_caller(const address callee_pc, const address r4,
        const address r5, const address r6) {
        if ((callee_pc < 0x00770000u) || (callee_pc >= 0x00780000u)
            || (r5 < 0x00770000u) || (r5 >= 0x00780000u)) {
            return;
        }
        const address key = j9_jxe_page(callee_pc);
        for (int i = 0; i < g_j9_caller_n; ++i) {
            if (g_j9_caller_key[i] == key) {
                return;
            }
        }
        if (g_j9_caller_n < k_j9_page_max) {
            const int i = g_j9_caller_n++;
            g_j9_caller_key[i] = key;
            g_j9_caller_r4[i] = r4;
            g_j9_caller_r5[i] = r5;
            g_j9_caller_r6[i] = r6;
        }
    }

    static bool j9_find_jxe_caller(const address pc, address *r4, address *r5,
        address *r6) {
        const address key = j9_jxe_page(pc);
        for (int i = 0; i < g_j9_caller_n; ++i) {
            if (g_j9_caller_key[i] == key) {
                if (r4) {
                    *r4 = g_j9_caller_r4[i];
                }
                if (r5) {
                    *r5 = g_j9_caller_r5[i];
                }
                if (r6) {
                    *r6 = g_j9_caller_r6[i];
                }
                return true;
            }
        }
        return false;
    }

    static bool j9_looks_jcl_pc(const address p) {
        return (p >= 0x81940000u) && (p < 0x81A50000u);
    }

    static bool j9_looks_java_obj(kernel::process *pr, const address p) {
        if (!pr || !j9_looks_heap(p) || ((p & 3u) != 0) || !j9_mapped32(pr, p + 12u)) {
            return false;
        }
        const address clazz = j9_read32(pr, p);
        return clazz && j9_mapped32(pr, clazz)
            && ((((clazz >= 0x00700000u) && (clazz < 0x00800000u))
                    || ((clazz >= 0x02000000u) && (clazz < 0x03000000u))
                    || ((clazz >= 0x81800000u) && (clazz < 0x82000000u))));
    }

    static address j9_dummy_jarray(kernel::process *pr, const unsigned len) {
        if (!pr || !g_j9_walk_va || !j9_mapped32(pr, g_j9_walk_va + 0x6980u)) {
            return g_j9_dummy_array;
        }
        const address arr = g_j9_walk_va + 0x6980u;
        const address clazz = (g_j9_jcl_r4 && j9_mapped32(pr, g_j9_jcl_r4))
            ? g_j9_jcl_r4 : 0x726780u;
        j9_write32(pr, arr, clazz);
        j9_write32(pr, arr + 4u, 0);
        j9_write32(pr, arr + 8u, len ? len : (240u * 320u));
        for (unsigned i = 0; i < 8; ++i) {
            j9_write32(pr, arr + 12u + i * 4u, 0);
        }
        g_j9_dummy_array = arr;
        return arr;
    }

    static address j9_ensure_jcl_this(kernel::process *pr) {
        if (!pr) {
            return g_j9_jcl_this;
        }
        address obj = 0;
        if (g_j9_jcl_r6 && j9_mapped32(pr, g_j9_jcl_r6)) {
            const address loc0 = j9_read32(pr, g_j9_jcl_r6);
            if (j9_looks_java_obj(pr, loc0)) {
                obj = loc0;
            }
        }
        if (!obj && j9_looks_java_obj(pr, g_j9_jcl_this)) {
            obj = g_j9_jcl_this;
        }
        if (!obj && j9_looks_heap(g_j9_jcl_this) && ((g_j9_jcl_this & 3u) == 0)
            && j9_mapped32(pr, g_j9_jcl_this + 0x20u)) {
            obj = g_j9_jcl_this;
        }
        if (!obj && g_j9_last_java_obj && j9_mapped32(pr, g_j9_last_java_obj + 0x20u)) {
            obj = g_j9_last_java_obj;
        }
        if (!obj && g_j9_walk_va && j9_mapped32(pr, g_j9_walk_va + 0x6800u)) {
            obj = g_j9_walk_va + 0x6800u;
        }
        if (!obj) {
            return 0;
        }
        address clazz = g_j9_jcl_r4;
        if (!clazz || !j9_mapped32(pr, clazz)) {
            const address cur = j9_read32(pr, obj);
            if (cur && j9_mapped32(pr, cur)) {
                clazz = cur;
            }
        }
        if (clazz) {
            j9_write32(pr, obj, clazz);
        }
        const address arr = j9_dummy_jarray(pr, 240u * 320u);
        if (j9_read32(pr, obj + 0xcu) < 0x40u) {
            j9_write32(pr, obj + 0xcu, 0x80u);
        }
        // Any unresolved instance slot becomes the dummy int/byte array so
        // JCL `getfield; arraylength` after Graphics._create does not NPE.
        for (int i = 2; i < 48; ++i) {
            const address at = obj + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, at)) {
                break;
            }
            const address w = j9_read32(pr, at);
            if (!w || !j9_mapped32(pr, w)) {
                j9_write32(pr, at, arr);
            }
        }
        if (g_j9_jcl_r6 && j9_mapped32(pr, g_j9_jcl_r6)) {
            j9_write32(pr, g_j9_jcl_r6, obj);
        }
        g_j9_jcl_this = obj;
        return obj;
    }

    static bool j9_looks_caller_pc(kernel::process *pr, const address pc) {
        if (!pr || !pc || (pc == g_j9_jcl_r5)) {
            return false;
        }
        const std::uint8_t op = j9_read8(pr, pc);
        if (!j9_is_invoke_op(op)) {
            return false;
        }
        if ((pc >= 0x00770000u) && (pc < 0x00780000u)) {
            return true;
        }
        // LCDUI / MIDP JCL, not J9VMInternals at 0x81980xxx.
        if ((pc >= 0x81940000u) && (pc < 0x81980000u)) {
            return true;
        }
        return false;
    }

    static void j9_plant_one_bkpt(kernel::process *pr, const address site,
        const std::uint32_t expect) {
        if (!pr || (g_j9_invoke_n >= 48)) {
            return;
        }
        for (int i = 0; i < g_j9_invoke_n; ++i) {
            if ((g_j9_invoke_sites[i] & ~1u) == site) {
                return;
            }
        }
        if (auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(site))) {
            if (*p == expect || *p == 0xE1200070u) {
                *p = 0xE1200070u;
                if (kernel_system *kern = pr->get_kernel_object_owner()) {
                    if (arm::core *cpu = kern->get_cpu()) {
                        cpu->imb_range(site, 4);
                    }
                }
                g_j9_invoke_sites[g_j9_invoke_n++] = site;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] planted post-jcl bkpt @0x{:X}", site);
            }
        }
    }

    static void j9_plant_post_jcl_r4fix(kernel::process *pr) {
        if (!pr) {
            return;
        }
        j9_plant_one_bkpt(pr, 0x81910F2Cu, 0xE593C004u);
        j9_plant_one_bkpt(pr, 0x81910F04u, 0xE797C10Au);
        j9_plant_one_bkpt(pr, 0x81910F48u, 0xE3E03007u);
        j9_plant_one_bkpt(pr, 0x81910F84u, 0x159AA000u);
        j9_plant_one_bkpt(pr, 0x81910F88u, 0x179AC00Cu);
        j9_plant_one_bkpt(pr, 0x81910F8Cu, 0x1590F008u);
        j9_plant_one_bkpt(pr, 0x818F6450u, 0xE79CA009u);
        j9_plant_one_bkpt(pr, 0x818F5A84u, 0xE5919A18u);
        j9_plant_one_bkpt(pr, 0x818F9410u, 0xE24DD048u);
        j9_plant_one_bkpt(pr, 0x818F85C0u, 0xE24DD040u);
        j9_plant_one_bkpt(pr, 0x818F87F0u, 0xE24DD050u);
        j9_plant_one_bkpt(pr, 0x818F8A80u, 0xE24DD048u);
        j9_plant_one_bkpt(pr, 0x818F8C14u, 0xE24DD048u);
        j9_plant_one_bkpt(pr, 0x818F8E00u, 0xE24DD048u);
        j9_plant_one_bkpt(pr, 0x818F910Cu, 0xE24DD040u);
        j9_plant_one_bkpt(pr, 0x818F9B5Cu, 0xE24DD040u);
        j9_plant_one_bkpt(pr, 0x818F9FA8u, 0xE24DD030u);
        j9_plant_one_bkpt(pr, 0x818ED590u, 0xE24DD028u);
    }

    static bool j9_scan_java_caller(kernel::process *pr, const address hint) {
        if (!pr) {
            return g_j9_jcl_outer_r5 != 0;
        }
        if (g_j9_jcl_outer_r5 && j9_looks_caller_pc(pr, g_j9_jcl_outer_r5)) {
            return true;
        }
        address best = 0;
        address br4 = 0;
        address br6 = 0;
        int best_score = -1;
        const auto consider_triple = [&](const address or4, const address opc,
            const address or6) {
            if (!j9_looks_caller_pc(pr, opc)) {
                return;
            }
            if ((or6 < 0x00710000u) || (or6 >= 0x00720000u)) {
                return;
            }
            int score = 10;
            if ((opc >= 0x81950000u) && (opc < 0x81970000u)) {
                score += 50;
            }
            if ((opc >= 0x00770000u) && (opc < 0x00780000u)) {
                score += 30;
            }
            if ((or4 >= 0x00720000u) && (or4 < 0x00730000u)) {
                score += 5;
            }
            if ((or4 >= 0x00726000u) && (or4 < 0x00728000u)) {
                score += 5;
            }
            if (score > best_score) {
                best_score = score;
                best = opc;
                br4 = or4;
                br6 = or6;
            }
        };
        const auto consider = [&](const address ot) {
            if (!j9_mapped32(pr, ot + 8u)) {
                return;
            }
            const address w0 = j9_read32(pr, ot);
            const address w1 = j9_read32(pr, ot + 4u);
            const address w2 = j9_read32(pr, ot + 8u);
            consider_triple(w0, w1, w2);
            consider_triple(w1, w0, w2);
            consider_triple(w0, w2, w1);
        };
        for (int k = -16; k < 80; ++k) {
            if (hint) {
                consider(hint + static_cast<address>(k) * 4u);
            }
            if (g_j9_jcl_r6) {
                consider(g_j9_jcl_r6 + static_cast<address>(k) * 4u);
            }
        }
        if (best) {
            g_j9_jcl_outer_r4 = br4;
            g_j9_jcl_outer_r5 = best;
            g_j9_jcl_outer_r6 = br6;
            return true;
        }
        return false;
    }

    static void j9_note_jcl_caller(kernel::process *pr, const address r6,
        const address r7) {
        if (g_j9_jcl_r5 || !pr) {
            return;
        }
        for (int i = -6; i < 20; ++i) {
            const address base = r7 + static_cast<address>(i) * 4u;
            const address alt = r6 + static_cast<address>(i) * 4u;
            for (address at : { base, alt }) {
                if (!j9_mapped32(pr, at + 8u)) {
                    continue;
                }
                const address w1 = j9_read32(pr, at + 4u);
                if (!j9_looks_jcl_pc(w1)) {
                    continue;
                }
                g_j9_jcl_r4 = j9_read32(pr, at);
                g_j9_jcl_r5 = w1;
                g_j9_jcl_r6 = j9_read32(pr, at + 8u);
                if (g_j9_jcl_r6 && j9_mapped32(pr, g_j9_jcl_r6)) {
                    const address loc0 = j9_read32(pr, g_j9_jcl_r6);
                    if (j9_looks_heap(loc0) && ((loc0 & 3u) == 0)
                        && j9_mapped32(pr, loc0)) {
                        g_j9_jcl_this = loc0;
                    }
                }
                if (!g_j9_jcl_this) {
                    for (int j = -2; j < 10; ++j) {
                        const address w = j9_read32(pr,
                            at + static_cast<address>(j) * 4u);
                        if ((w >= 0x02000000u) && (w < 0x03000000u)
                            && ((w & 3u) == 0)) {
                            g_j9_jcl_this = w;
                            break;
                        }
                    }
                }
                j9_scan_java_caller(pr, at);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] jcl-caller r4=0x{:X} pc=0x{:X} r6=0x{:X} at=0x{:X} this=0x{:X} loc0=0x{:X} outer=0x{:X}/0x{:X}/0x{:X} stk=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                    g_j9_jcl_r4, g_j9_jcl_r5, g_j9_jcl_r6, at, g_j9_jcl_this,
                    (g_j9_jcl_r6 && j9_mapped32(pr, g_j9_jcl_r6))
                        ? j9_read32(pr, g_j9_jcl_r6) : 0,
                    g_j9_jcl_outer_r4, g_j9_jcl_outer_r5, g_j9_jcl_outer_r6,
                    j9_read32(pr, at + 12u), j9_read32(pr, at + 16u),
                    j9_read32(pr, at + 20u), j9_read32(pr, at + 24u),
                    j9_read32(pr, at + 28u), j9_read32(pr, at + 32u),
                    j9_read32(pr, at + 36u), j9_read32(pr, at + 40u));
                return;
            }
        }
    }

    static bool j9_looks_bytecode_pc(kernel::process *pr, const address p) {
        const address bare = p & ~3u;
        if (!bare || !pr) {
            return false;
        }
        // type=JXE rom.classes live in this RAM window. Java locals / the
        // operand stack sit at 0x71xxxx and must not be treated as PC.
        if ((bare >= 0x00770000u) && (bare < 0x00780000u)) {
            return j9_mapped32(pr, bare);
        }
        // JCL ROM class bytecode is XIP. j9_mapped32 rejects >=0x80000000,
        // so only test that the page is readable.
        // Class ROM bytecode lives after jclcldc11 .text (bootstrap/j9ext).
        // 0x8194C2xx is ARM and must not be treated as a Java PC.
        if ((bare >= 0x81950000u) && (bare < 0x81B00000u)) {
            return pr->get_ptr_on_addr_space(bare) != nullptr;
        }
        if (g_j9_midp2ams_run && (bare >= g_j9_midp2ams_run)
            && (bare < (g_j9_midp2ams_run + g_j9_midp2ams_size))) {
            return pr->get_ptr_on_addr_space(bare) != nullptr;
        }
        // Official classfile→ROM lands in the Java heap / host chunk.
        if ((bare >= 0x02D00000u) && (bare < 0x03000000u)) {
            return pr->get_ptr_on_addr_space(bare) != nullptr;
        }
        if (g_j9_java_heap_va && (bare >= g_j9_java_heap_va)
            && (bare < (g_j9_java_heap_va + k_j9_java_heap_size))) {
            return pr->get_ptr_on_addr_space(bare) != nullptr;
        }
        return false;
    }

    static bool j9_is_initialize_pc(const address p) {
        return (p >= 0x81980500u) && (p < 0x81980800u);
    }

    // interpret() tail around putstatic/ireturn. Re-entering 0x819630A4
    // after g_j9_boot_returned already consumed the C frame → pc=0.
    static bool j9_is_interpret_tail_pc(const address p) {
        return (p >= 0x81963000u) && (p < 0x81963200u);
    }

    static bool j9_is_stale_interp_caller(const address r4, const address pc,
        const address cfp) {
        if ((pc >= 0x81962D00u) && (pc < 0x81962F00u)) {
            return true;
        }
        if (g_j9_boot_t0 && (r4 == g_j9_boot_t0)) {
            return true;
        }
        if (g_j9_boot_t1 && ((pc == g_j9_boot_t1) || (pc == (g_j9_boot_t1 + 3u)))) {
            return true;
        }
        if (g_j9_boot_t2 && cfp && ((cfp == g_j9_boot_t2) || (cfp == (g_j9_boot_t2 & ~3u)))) {
            return true;
        }
        if ((r4 == 0x726D60u) || (r4 == 0x726E50u) || (r4 == 0x7286A8u)) {
            return true;
        }
        if ((cfp == 0x71E5ECu) || (cfp == 0x71E5EEu) || (cfp == 0x71E5B4u)) {
            return true;
        }
        return false;
    }

    static void j9_note_outer_jcl(kernel::process *pr, const address r4,
        const address r5, const address r6, const address r7) {
        if (!pr || j9_is_initialize_pc(r5) || j9_is_interpret_tail_pc(r5)
            || !j9_looks_bytecode_pc(pr, r5)) {
            return;
        }
        if ((r6 < 0x0071E000u) || (r6 >= 0x00720000u)) {
            return;
        }
        g_j9_outer_r4 = r4;
        g_j9_outer_r5 = r5;
        g_j9_outer_r6 = r6;
        g_j9_outer_r7 = r7 ? r7 : r6;
    }

    // Snapshot interpreter regs at tramp entry, before jni_entry / adapter
    // clobber r8 (J9VMThread) and r6 (bytecode PC) to the J9Method*.
    static void j9_note_interp_frame(arm::core *core, kernel::process *pr,
        const address method) {
        if (!core || !pr) {
            return;
        }
        const address r8 = core->get_reg(8);
        const address r6 = core->get_reg(6);
        const address r7 = core->get_reg(7);
        if (!j9_r8_looks_vmthread(pr, r8, method)) {
            return;
        }
        g_j9_vmthread = r8;
        if (!g_j9_vt10_c && j9_mapped32(pr, r8 + 0x18u)) {
            const address w10 = j9_read32(pr, r8 + 0x10u);
            const address w18 = j9_read32(pr, r8 + 0x18u);
            if (w10 && ((w10 < 0x0071E000u) || (w10 >= 0x00720000u))) {
                g_j9_vt10_c = w10;
                g_j9_vt18_c = w18;
            }
        }
        if (method && (method >= 0x70000u) && (method != 0x60001u)
            && j9_mapped32(pr, method + 8u)) {
            g_j9_tramp_method = method;
            g_j9_unbound_retry = 0;
        }
        g_j9_saved_r2 = core->get_reg(2);
        g_j9_saved_r4 = core->get_reg(4);
        g_j9_saved_r5 = core->get_reg(5);
        g_j9_saved_r6 = r6;
        if ((g_j9_saved_r5 >= 0x00770000u) && (g_j9_saved_r5 < 0x00780000u)) {
            j9_note_jcl_caller(pr, r6, r7);
            const address key = j9_jxe_page(g_j9_saved_r5);
            if (g_j9_last_jxe_r5 && (j9_jxe_page(g_j9_last_jxe_r5) != key)) {
                j9_save_jxe_caller(g_j9_saved_r5, g_j9_last_jxe_r4,
                    g_j9_last_jxe_r5, g_j9_last_jxe_r6);
            }
            if (method && (method >= 0x70000u) && (method != 0x60001u)) {
                j9_save_jxe_page(g_j9_saved_r5, g_j9_saved_r4, r6);
                j9_save_snap(g_j9_saved_r5, g_j9_saved_r4, r6);
            }
            static int jxe_notes = 0;
            if (jxe_notes < 24) {
                ++jxe_notes;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] jxe-frame r4=0x{:X} r5=0x{:X} r6=0x{:X} r7=0x{:X} meth=0x{:X}",
                    g_j9_saved_r4, g_j9_saved_r5, r6, r7, method);
            }
            if (g_j9_last_jxe_r5 && (g_j9_last_jxe_r5 != g_j9_saved_r5)) {
                g_j9_prev_jxe_r4 = g_j9_last_jxe_r4;
                g_j9_prev_jxe_r5 = g_j9_last_jxe_r5;
                g_j9_prev_jxe_r6 = g_j9_last_jxe_r6;
            }
            g_j9_last_jxe_key = key;
            g_j9_last_jxe_r4 = g_j9_saved_r4;
            g_j9_last_jxe_r5 = g_j9_saved_r5;
            g_j9_last_jxe_r6 = r6;
            g_j9_last_jxe_r7 = r7;
            g_j9_bytecode_pc = g_j9_saved_r5;
        } else if (j9_looks_bytecode_pc(pr, g_j9_saved_r5)) {
            g_j9_bytecode_pc = g_j9_saved_r5;
        }
        if (j9_mapped32(pr, r7) && (r7 < 0x01000000u)) {
            g_j9_java_sp = r7;
        }
        static int notes = 0;
        if (notes < 40) {
            ++notes;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] interp-frame vt=0x{:X} r2=0x{:X} r4=0x{:X} r5=0x{:X} r6=0x{:X} r7=0x{:X} meth=0x{:X} vm8=0x{:X}",
                r8, g_j9_saved_r2, g_j9_saved_r4, g_j9_saved_r5, r6, r7, method,
                j9_read32(pr, r8 + 8u));
        }
        if (g_j9_thread_class && (g_j9_saved_r5 == 0x81922C1Du)) {
            static int native_ret = 0;
            if (native_ret < 6) {
                ++native_ret;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] native-ret vt=0x{:X} slot=0x{:X} ex=0x{:X} obj=0x{:X} "
                    "r4=0x{:X} r6=0x{:X} r7=0x{:X} meth=0x{:X}",
                    r8,
                    r8 ? j9_read32(pr, r8 + k_j9_thread_obj_off) : 0,
                    r8 ? j9_read32(pr, r8 + 0x64u) : 0,
                    g_j9_thread_obj, g_j9_saved_r4, r6, r7, method);
            }
        }
    }

    static void j9_run_main_cp_stub(arm::core *core, kernel::process *pr);
    static bool j9_host_cms_run(arm::core *core, kernel::process *pr);
    static bool j9_host_kick_lcdui(arm::core *core, kernel::process *pr);
    static bool j9_host_start_alps_init(arm::core *core, kernel::process *pr,
        const address clazz);
    static void j9_prepare_findclass_vt(kernel::process *pr);

    // type=JXE has no J9ROMMethod header, so official JNI teardown
    // (0x81911DFC) AVs. Pop the invoke args (and a jni_entry frame if
    // present) and dispatch the next bytecode via vm+0xa18.
    static void j9_jxe_resume_interp(arm::core *core, kernel::process *pr,
        const address method, const unsigned argc_in) {
        if (!core || !pr) {
            return;
        }
        const address meth = (method && (method != g_j9_vmthread))
            ? method
            : (g_j9_tramp_method ? g_j9_tramp_method : method);
        // 0 is a real value (leave the Java stack alone after a void
        // _create). Do not treat it as "unspecified".
        const unsigned argc = argc_in;
        address r8 = core->get_reg(8);
        const auto r8_is_method = [&](const address p) {
            if (!p || (p == meth)) {
                return true;
            }
            const address send = j9_read32(pr, p + 8u);
            return (send == k_j9_jni_entry) || (send == 8u) || (send == k_j9_inl_dispatch)
                || (g_j9_walk_va && (send >= (g_j9_walk_va + k_j9_adapt_off))
                    && (send < (g_j9_walk_va + 0x8000u)));
        };
        // Prefer the tramp-entry snapshot. jni_entry / adapter overwrite
        // r8 with the J9Method* and env+4 with that same clobbered value.
        if (g_j9_vmthread && j9_r8_looks_vmthread(pr, g_j9_vmthread, meth)) {
            r8 = g_j9_vmthread;
        } else if (g_j9_fake_env) {
            const address er8 = j9_read32(pr, g_j9_fake_env + 4u);
            if (er8 && !r8_is_method(er8) && j9_mapped32(pr, er8 + 0x10u)) {
                r8 = er8;
            }
        } else if (r8_is_method(r8) && g_j9_vmthread && !r8_is_method(g_j9_vmthread)) {
            r8 = g_j9_vmthread;
        }
        core->set_reg(8, r8);
        {
            const address vm = j9_read32(pr, r8 + 4u);
            address tab = vm ? j9_read32(pr, vm + 0xa18u) : 0;
            // tramp snapshots r2 as midp2ams .data (0x3FFF8E28). The
            // interpreter later does `ldr pc, [r2, op, lsl #2]`. Always
            // put the official bytecode table in r2, vmthread+0xc, vm+0xa18.
            if (!tab || (tab == 0x3FFF8E28u) || (tab < 0x81800000u)
                || (tab >= 0x82000000u) || !pr->get_ptr_on_addr_space(tab)) {
                tab = k_j9_opcode_table;
            }
            core->set_reg(2, tab);
            j9_write32(pr, r8 + 0xcu, tab);
            if (vm && j9_mapped32(pr, vm + 0xa18u)) {
                j9_write32(pr, vm + 0xa18u, tab);
            }
            g_j9_saved_r2 = tab;
        }
        if (g_j9_saved_r4) {
            core->set_reg(4, g_j9_saved_r4);
        } else if (g_j9_jcl_outer_r4) {
            core->set_reg(4, g_j9_jcl_outer_r4);
        } else if (g_j9_good_r4) {
            core->set_reg(4, g_j9_good_r4);
        }
        // r5 is overwritten below with the next-instruction PC when we
        // have a tramp snapshot. Only restore the tramp r5 as fallback.
        if (!g_j9_bytecode_pc) {
            core->set_reg(5, g_j9_saved_r5);
        }
        address r6 = g_j9_saved_r6 ? g_j9_saved_r6 : core->get_reg(6);
        address r7 = core->get_reg(7);
        // r5 is the bytecode PC (JCL: ROM, JXE: RAM). r6 is the local
        // frame pointer — the bytes at tramp r6 are stack slots (e.g.
        // the Graphics object 0x02D127D0), not opcodes.
        if (g_j9_saved_r5 && j9_looks_bytecode_pc(pr, g_j9_saved_r5)) {
            // r5 points at the invoke opcode; skip opcode + u2 index
            // unless the caller asked to re-dispatch the same invoke.
            if (!g_j9_resume_at) {
                core->set_reg(5, g_j9_saved_r5 + 3u);
            }
        }
        if (g_j9_java_sp && j9_mapped32(pr, g_j9_java_sp)) {
            r7 = g_j9_java_sp;
        }
        const address slot0 = j9_read32(pr, r7);
        const address slot1 = j9_read32(pr, r7 + 4u);
        const address saved_r4 = j9_read32(pr, r7 + 8u);
        const address saved_r5 = j9_read32(pr, r7 + 12u);
        const address saved_pc = j9_read32(pr, r7 + 16u) & ~3u;
        // Only a real jni_entry frame starts with the J9Method*. The
        // Java arg stack also has [1]==0 and a mapped word at [16].
        const bool jni_frame = method && (slot0 == method) && (slot1 == 0)
            && saved_pc && j9_mapped32(pr, saved_pc)
            && ((saved_pc & ~0xFFFu) != (r7 & ~0xFFFu));
        const auto looks_bc = [&](const address p) {
            const address bare = p & ~3u;
            if (!bare || !j9_mapped32(pr, bare)) {
                return false;
            }
            if ((bare & ~0xFFFu) == (r7 & ~0xFFFu)) {
                return false;
            }
            if ((bare >= 0x81800000u) && (bare < 0x82000000u)) {
                return false;
            }
            if ((bare >= g_j9_walk_va) && (bare < g_j9_walk_va + 0x8000u)) {
                return false;
            }
            return true;
        };
        const bool have_bc = g_j9_saved_r5 && j9_looks_bytecode_pc(pr, g_j9_saved_r5);
        if (jni_frame && !have_bc) {
            core->set_reg(4, saved_r4);
            core->set_reg(5, saved_r5);
            r6 = saved_pc;
            r7 = r7 + 20u + argc * 4u;
        } else {
            r7 = r7 + argc * 4u;
        }
        address fetch_pc = have_bc ? (g_j9_saved_r5 + 3u) : core->get_reg(5);
        if (g_j9_resume_at) {
            fetch_pc = g_j9_resume_at;
        }
        // 0xAC: `r7=r6+4; ldmda [old_r7+8] {r4,r5,r6}; ldrb ip,[r5,#3]!`.
        // After a stubbed native, old_r7 may still be that invoke's
        // frame. Point r7 at a real caller frame {class, invoke-pc, locals}
        // so the official handler returns out of this method.
        if (have_bc && !g_j9_resume_no_ac) {
            const auto *opb = reinterpret_cast<const std::uint8_t *>(
                pr->get_ptr_on_addr_space(fetch_pc));
            if (opb && ((*opb == 0xACu) || (*opb == 0xADu) || (*opb == 0xB0u)
                    || (*opb == 0xB1u))) {
                bool planted_outer = false;
                if (g_j9_force_caller) {
                    address cr4 = 0;
                    address cr5 = 0;
                    address cr6 = 0;
                    if (j9_find_jxe_caller(g_j9_force_caller, &cr4, &cr5, &cr6)
                        && cr5 && j9_is_invoke_op(j9_read8(pr, cr5))) {
                        if (!j9_mapped32(pr, r7 + 8u)) {
                            r7 = g_j9_java_sp ? g_j9_java_sp : r7;
                        }
                        if (cr4) {
                            j9_write32(pr, r7, cr4);
                            core->set_reg(4, cr4);
                        }
                        j9_write32(pr, r7 + 4u, cr5);
                        if (cr6) {
                            j9_write32(pr, r7 + 8u, cr6);
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] ac-outer r7=0x{:X} r4=0x{:X} pc=0x{:X} r6=0x{:X} from=0x{:X}",
                            r7, cr4, cr5, cr6, g_j9_saved_r5);
                        planted_outer = true;
                    }
                    g_j9_force_caller = 0;
                }
                address best = 0;
                int best_d = 0x7fffffff;
                if (planted_outer) {
                    best_d = 0;
                } else {
                const auto consider = [&](const address base) {
                    if (!j9_mapped32(pr, base + 8u)) {
                        return;
                    }
                    const address w0 = j9_read32(pr, base);
                    const address w1 = j9_read32(pr, base + 4u);
                    const address w2 = j9_read32(pr, base + 8u);
                    if ((w1 < 0x00770000u) || (w1 >= 0x00780000u)
                        || !j9_is_invoke_op(j9_read8(pr, w1))) {
                        return;
                    }
                    const int d = std::abs(static_cast<int>(w1)
                        - static_cast<int>(g_j9_saved_r5));
                    if ((d <= 16) || (d > 0x800) || (d >= best_d)) {
                        return;
                    }
                    if ((w0 < 0x00700000u) || (w0 >= 0x00800000u)) {
                        return;
                    }
                    (void)w2;
                    best = base;
                    best_d = d;
                };
                for (int i = -2; i < 16; ++i) {
                    consider(r7 + static_cast<address>(i) * 4u);
                    consider(r6 + static_cast<address>(i) * 4u);
                }
                if (!best) {
                    for (int i = 1; i < 16; ++i) {
                        const address at = r7 + static_cast<address>(i) * 4u;
                        const address pcw = j9_read32(pr, at);
                        if ((pcw >= 0x00770000u) && (pcw < 0x00780000u)
                            && j9_is_invoke_op(j9_read8(pr, pcw))) {
                            const int d = std::abs(static_cast<int>(pcw)
                                - static_cast<int>(g_j9_saved_r5));
                            if (d > 16) {
                                best = at - 4u;
                                best_d = d;
                                break;
                            }
                        }
                    }
                }
                }
                if (!best && !planted_outer) {
                    static int miss = 0;
                    if (miss < 4) {
                        ++miss;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] ac-miss from=0x{:X} r6=0x{:X} r7=0x{:X} [0]=0x{:X} [4]=0x{:X} [8]=0x{:X} [c]=0x{:X} [10]=0x{:X} [14]=0x{:X} b68=0x{:02X}",
                            g_j9_saved_r5, r6, r7,
                            j9_read32(pr, r7), j9_read32(pr, r7 + 4u),
                            j9_read32(pr, r7 + 8u), j9_read32(pr, r7 + 12u),
                            j9_read32(pr, r7 + 16u), j9_read32(pr, r7 + 20u),
                            j9_read8(pr, 0x770A68u));
                    }
                }
                if (best && !planted_outer) {
                    r7 = best;
                    const address raw_pc = j9_read32(pr, r7 + 4u);
                    address store = raw_pc;
                    if (j9_is_invoke_op(j9_read8(pr, raw_pc - 4u))
                        && j9_is_invoke_op(j9_read8(pr, raw_pc))) {
                        store = raw_pc - 4u;
                    } else if (j9_is_invoke_op(j9_read8(pr, raw_pc - 3u))) {
                        store = raw_pc - 3u;
                    }
                    if (store == raw_pc) {
                        const address w0 = j9_read32(pr, r7);
                        const address w2 = j9_read32(pr, r7 + 8u);
                        if ((w0 >= 0x00720000u) && (w0 < 0x00730000u)) {
                            g_j9_good_r4 = w0;
                            if ((w2 >= 0x00710000u) && (w2 < 0x00720000u)) {
                                g_j9_good_r6 = w2;
                            }
                            if (!g_j9_method_start) {
                                g_j9_method_start = store;
                            }
                        }
                    } else {
                        j9_write32(pr, r7 + 4u, store);
                        // Rewind landed on the caller invoke, but w0/w2
                        // still belong to the next slot (often a J9Method*
                        // leftover). Prefer a real {class, store, locals}
                        // triple if one is still on the stack; otherwise
                        // restore class/locals from the caller page / last
                        // good AC frame.
                        address found = 0;
                        for (int i = -4; i < 20; ++i) {
                            const address base = r7 + static_cast<address>(i) * 4u;
                            if (!j9_mapped32(pr, base + 8u)) {
                                continue;
                            }
                            const address w1 = j9_read32(pr, base + 4u);
                            if ((w1 == store) || (w1 == (store + 3u))) {
                                found = base;
                                break;
                            }
                        }
                        if (found) {
                            r7 = found;
                            if (j9_read32(pr, r7 + 4u) != store) {
                                j9_write32(pr, r7 + 4u, store);
                            }
                        }
                        address pr4 = 0;
                        address pr6 = 0;
                        if (j9_pick_caller_regs(store, &pr4, &pr6)
                            || j9_pick_caller_regs(g_j9_saved_r5, &pr4, &pr6)) {
                            if (pr4) {
                                j9_write32(pr, r7, pr4);
                                core->set_reg(4, pr4);
                            }
                            if (pr6) {
                                j9_write32(pr, r7 + 8u, pr6);
                            }
                        }
                    }
                    static int acfix = 0;
                    if (acfix < 8) {
                        ++acfix;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] ac-frame r7=0x{:X} r4=0x{:X} pc=0x{:X} store=0x{:X} r6=0x{:X} from=0x{:X} d={} good=0x{:X}",
                            r7, j9_read32(pr, r7), raw_pc, store,
                            j9_read32(pr, r7 + 8u), g_j9_saved_r5, best_d,
                            g_j9_good_r4);
                    }
                }
            }
        }
        std::uint8_t op = 0;
        if (const auto *p = reinterpret_cast<const std::uint8_t *>(
                pr->get_ptr_on_addr_space(fetch_pc))) {
            op = *p;
        }
        if (have_bc || g_j9_resume_at) {
            core->set_reg(5, fetch_pc);
        }
        if (j9_looks_bytecode_pc(pr, fetch_pc)) {
            g_j9_last_interp_pc = fetch_pc;
        }
        core->set_reg(0, op);
        core->set_reg(6, r6);
        core->set_reg(7, r7);
        j9_write32(pr, r8 + 8u, r6);
        {
            address tab = core->get_reg(2);
            if (!tab || (tab == 0x3FFF8E28u) || (tab < 0x80000000u) || (tab >= 0x82000000u)) {
                tab = k_j9_opcode_table;
                core->set_reg(2, tab);
            }
            j9_write32(pr, r8 + 0xcu, tab);
        }
        j9_write32(pr, r8 + 0x10u, r7);
        j9_write32(pr, r8 + 0x14u, core->get_reg(5));
        j9_write32(pr, r8 + 0x18u, core->get_reg(4));
        static int logs = 0;
        if ((logs < 12) || (g_j9_resume_at == 0x81980745u)
            || (g_j9_resume_at == 0x8198074Du)
            || ((g_j9_resume_at >= 0x8195BB00u) && (g_j9_resume_at < 0x8195BC00u))
            || ((g_j9_resume_at >= 0x81963000u) && (g_j9_resume_at < 0x81963200u))
            || ((g_j9_resume_at >= 0x8195AC00u) && (g_j9_resume_at < 0x8195AE00u))
            || ((g_j9_resume_at >= 0x8195A700u) && (g_j9_resume_at < 0x8195A900u))
            || ((g_j9_resume_at >= 0x81961A80u) && (g_j9_resume_at < 0x81961E00u))
            || (g_j9_resume_at == 0x81961C74u)
            || ((g_j9_resume_at >= 0x81962300u) && (g_j9_resume_at < 0x81962340u))
            || (g_j9_resume_at == 0x81962D43u)
            || ((g_j9_resume_at >= 0x81AA8000u) && (g_j9_resume_at < 0x81AA9000u))) {
            ++logs;
            const address vm = j9_read32(pr, r8 + 4u);
            const address tab = j9_read32(pr, vm + 0xa18u);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] jxe-resume meth=0x{:X} argc={} jni={} op=0x{:02X} r5=0x{:X} r6=0x{:X} r7=0x{:X} r8=0x{:X} stk=0x{:X}/0x{:X}/0x{:X}/0x{:X} hdl=0x{:X} bc={:02X}{:02X}{:02X}{:02X}{:02X}{:02X} sv5=0x{:X}",
                meth, argc, jni_frame ? 1 : 0, op, fetch_pc, r6, r7, r8,
                j9_read32(pr, r7), j9_read32(pr, r7 + 4u), j9_read32(pr, r7 + 8u),
                j9_read32(pr, r7 + 12u),
                j9_read32(pr, tab + static_cast<address>(op) * 4u),
                j9_read32(pr, fetch_pc) & 0xFFu,
                (j9_read32(pr, fetch_pc) >> 8) & 0xFFu,
                (j9_read32(pr, fetch_pc) >> 16) & 0xFFu,
                (j9_read32(pr, fetch_pc) >> 24) & 0xFFu,
                j9_read32(pr, fetch_pc + 4u) & 0xFFu,
                (j9_read32(pr, fetch_pc + 4u) >> 8) & 0xFFu,
                g_j9_saved_r5);
        }
        if (g_j9_main_clazz && (fetch_pc >= 0x81AA8138u) && (fetch_pc < 0x81AA81A0u)
            && g_j9_walk_va) {
            if ((fetch_pc == 0x81AA814Du) && (op == 0x4Eu)) {
                address val = 0;
                if (r7 && j9_mapped32(pr, r7)) {
                    val = j9_read32(pr, r7);
                    r7 += 4u;
                }
                if (r6 && j9_mapped32(pr, r6 + 12u)) {
                    j9_write32(pr, r6 + 12u, val);
                }
                core->set_reg(6, r6);
                core->set_reg(7, r7);
                g_j9_saved_r4 = g_j9_main_clazz;
                g_j9_saved_r6 = r6;
                g_j9_java_sp = r7;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] main-host astore_3 val=0x{:X} -> cms-run r6=0x{:X} r7=0x{:X}",
                    val, r6, r7);
                if (j9_host_cms_run(core, pr)) {
                    return;
                }
                g_j9_resume_at = 0x81AA8196u;
                g_j9_resume_no_ac = true;
                j9_jxe_resume_interp(core, pr, meth, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            if ((fetch_pc == 0x81AA819Au) && (op == 0xACu)) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] main-host ireturn park=0x{:X} ret=0x{:X}/0x{:X} r6=0x{:X}",
                    g_j9_park_pc, g_j9_main_ret_sp, g_j9_main_ret_lr, r6);
                if (g_j9_park_pc) {
                    j9_set_pc(core, g_j9_park_pc);
                    return;
                }
                if (g_j9_main_ret_sp && g_j9_main_ret_lr) {
                    core->set_reg(0, 0);
                    core->set_sp(g_j9_main_ret_sp);
                    j9_set_pc(core, g_j9_main_ret_lr);
                    return;
                }
            }
            const unsigned midx = static_cast<unsigned>(j9_read8(pr, fetch_pc + 1u))
                | (static_cast<unsigned>(j9_read8(pr, fetch_pc + 2u)) << 8);
            const bool host_op = ((op == 0xB2u) && (midx == 17u))
                || ((op == 0xB4u) && ((midx == 33u) || (midx == 36u)))
                || ((op == 0xB6u) && ((midx == 34u) || (midx == 38u)))
                || ((op == 0xB7u) && (midx == 43u))
                || ((op == 0xBBu) && (midx == 46u));
            if (host_op) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] main-host op=0x{:02X} #{} pc=0x{:X}",
                    op, midx, fetch_pc);
                j9_run_main_cp_stub(core, pr);
                return;
            }
        }
        j9_set_pc(core, 0x818F5A7Cu);
    }

    static bool j9_ptr_is_jcl_rom(const address p) {
        return (p >= 0x81940000u) && (p < 0x81A00000u);
    }

    static bool j9_ptr_is_rom_class(const address p) {
        return j9_ptr_is_jcl_rom(p)
            || ((p >= 0x81A00000u) && (p < 0x82000000u))
            || ((p >= 0x70300000u) && (p < 0x70500000u))
            || ((p >= 0x02D00000u) && (p < 0x03000000u))
            || (g_j9_java_heap_va && (p >= g_j9_java_heap_va)
                && (p < (g_j9_java_heap_va + k_j9_java_heap_size)));
    }

    static bool j9_class_is_jcl(kernel::process *pr, const address clazz) {
        if (!pr || !clazz) {
            return false;
        }
        for (int i = 0; i < 12; ++i) {
            const address w = j9_read32(pr, clazz + static_cast<address>(i) * 4u);
            if (j9_ptr_is_jcl_rom(w)) {
                return true;
            }
            if (w && pr->get_ptr_on_addr_space(w)) {
                const address inner = j9_read32(pr, w);
                if (j9_ptr_is_jcl_rom(inner)) {
                    return true;
                }
            }
        }
        return false;
    }

    static bool j9_wrap_triple_ok(kernel::process *pr, const address wfp,
        const address t0, const address t1, const address t2) {
        if (!pr) {
            return false;
        }
        if ((wfp < 0x0071E000u) || (wfp >= 0x00720000u)) {
            return false;
        }
        if ((t0 < 0x00720000u) || (t0 >= 0x00730000u)) {
            return false;
        }
        if ((t2 < 0x0071E000u) || (t2 >= 0x00720000u)) {
            return false;
        }
        if (!j9_looks_bytecode_pc(pr, t1) || !j9_is_wrap_caller_op(j9_read8(pr, t1))) {
            return false;
        }
        if ((t1 >= 0x81980500u) && (t1 < 0x81980800u)) {
            return false;
        }
        if (g_j9_last_wrap_t1 && (t1 == g_j9_last_wrap_t1)) {
            return false;
        }
        if (g_j9_last_wrap_fp && (wfp == g_j9_last_wrap_fp)) {
            return false;
        }
        return true;
    }

    static bool j9_try_save_iframe(kernel::process *pr, arm::core *core,
        const address clazz) {
        if (g_j9_sys_ok || !pr || !core) {
            return false;
        }
        const address sp = core->get_sp();
        const address slr = (sp && j9_mapped32(pr, sp + 0x20u))
            ? j9_read32(pr, sp + 0x20u) : 0;
        if (!sp || !j9_mapped32(pr, sp + 0x17Cu)
            || (slr < 0x81800000u) || (slr >= 0x82000000u)
            || !pr->get_ptr_on_addr_space(slr)) {
            return false;
        }
        g_j9_sys_sp = sp;
        for (int i = 0; i < 96; ++i) {
            g_j9_sys_frame[i] = j9_read32(pr, sp + static_cast<address>(i) * 4u);
        }
        g_j9_sys_ok = true;
        if (!g_j9_wrap_clazz) {
            g_j9_wrap_clazz = clazz;
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] system-iframe sp=0x{:X} lr=0x{:X} r4=0x{:X} "
            "op=0x{:X} bpc=0x{:X} wfp=0x{:X} clazz=0x{:X}",
            sp, slr, g_j9_sys_frame[0], g_j9_sys_frame[67],
            g_j9_sys_frame[71], g_j9_wrap_java_fp, clazz);
        return true;
    }

    static bool j9_try_wrap_scan(kernel::process *pr, arm::core *core,
        const address clazz) {
        if (g_j9_wrap_fp_ok || !pr || !core) {
            return false;
        }
        const address r7 = core->get_reg(7);
        const address r6 = core->get_reg(6);
        const address live = r6 ? r6 : (r7 ? (r7 + 0x1Cu) : 0);
        if (!live) {
            return false;
        }
        address found = 0;
        for (int i = -8; i < 24; ++i) {
            const address at = live + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, at + 8u)) {
                continue;
            }
            const address t0 = j9_read32(pr, at);
            const address t1 = j9_read32(pr, at + 4u);
            const address t2 = j9_read32(pr, at + 8u);
            if (j9_wrap_triple_ok(pr, at + 8u, t0, t1, t2)) {
                found = at;
                break;
            }
        }
        if (!found) {
            return false;
        }
        g_j9_wrap_t0 = j9_read32(pr, found);
        g_j9_wrap_t1 = j9_read32(pr, found + 4u);
        g_j9_wrap_t2 = j9_read32(pr, found + 8u);
        g_j9_wrap_java_fp = found + 8u;
        g_j9_wrap_clazz = clazz;
        g_j9_wrap_fp_ok = true;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] wrap-scan clazz=0x{:X} fp=0x{:X} t-8=0x{:X} "
            "t-4=0x{:X} t0=0x{:X}",
            clazz, g_j9_wrap_java_fp, g_j9_wrap_t0,
            g_j9_wrap_t1, g_j9_wrap_t2);
        return true;
    }

    static bool j9_copy_utf8(kernel::process *pr, const address p, char *out, const size_t n) {
        if (!pr || !p || !out || (n < 2) || !pr->get_ptr_on_addr_space(p + 2u)) {
            return false;
        }
        const auto *b = reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(p));
        if (!b) {
            return false;
        }
        const unsigned ln = static_cast<unsigned>(b[0]) | (static_cast<unsigned>(b[1]) << 8);
        if ((ln < 1u) || (ln >= n) || (ln > 180u) || !pr->get_ptr_on_addr_space(p + 1u + ln)) {
            return false;
        }
        for (unsigned i = 0; i < ln; ++i) {
            const std::uint8_t ch = b[2u + i];
            if ((ch < 0x20u) || (ch > 0x7Eu)) {
                return false;
            }
            out[i] = static_cast<char>(ch);
        }
        out[ln] = 0;
        return (std::strchr(out, '/') != nullptr) || (std::strncmp(out, "java", 4) == 0)
            || ((ln == 1u) && (out[0] >= 0x20) && (out[0] <= 0x7E))
            || ((out[0] >= 'A') && (out[0] <= 'z') && (ln >= 2u) && (ln < 80u));
    }

    static void j9_class_name(kernel::process *pr, const address clazz, char *out, const size_t n) {
        if (out && n) {
            out[0] = 0;
        }
        if (!pr || !clazz || !out || (n < 8)) {
            return;
        }
        const auto try_srp = [&](const address field) {
            if (!field || !pr->get_ptr_on_addr_space(field)) {
                return false;
            }
            const auto rel = static_cast<std::int32_t>(j9_read32(pr, field));
            const address tgt = field + static_cast<address>(rel);
            return j9_copy_utf8(pr, tgt, out, n) || j9_copy_utf8(pr, field, out, n);
        };
        for (int i = 0; i < 16; ++i) {
            const address w = j9_read32(pr, clazz + static_cast<address>(i) * 4u);
            address rom = 0;
            if (j9_ptr_is_rom_class(w)) {
                rom = w;
            } else if (w && pr->get_ptr_on_addr_space(w)) {
                const address inner = j9_read32(pr, w);
                if (j9_ptr_is_rom_class(inner)) {
                    rom = inner;
                }
            }
            if (!rom) {
                continue;
            }
            for (int off = 0; off <= 0x20; off += 4) {
                if (try_srp(rom + static_cast<address>(off))) {
                    return;
                }
            }
        }
    }

    static address j9_inl_class_from_slot(kernel::process *pr, const address slot) {
        if (!pr || !slot) {
            return 0;
        }
        if (pr->get_ptr_on_addr_space(slot + 0x28u)) {
            return slot;
        }
        const address unpacked = slot << 2;
        if ((slot < 0x04000000u) && pr->get_ptr_on_addr_space(unpacked + 0x28u)) {
            return unpacked;
        }
        const address hdr = j9_read32(pr, slot);
        if (hdr && pr->get_ptr_on_addr_space(hdr + 0x28u)) {
            return hdr;
        }
        const address vmref = j9_read32(pr, slot + 8u);
        if (vmref && pr->get_ptr_on_addr_space(vmref + 0x28u)) {
            return vmref;
        }
        return 0;
    }

    static bool j9_try_emu_inl(arm::core *core, kernel::process *pr, const address send,
        const address method) {
        if (!core || !pr) {
            return false;
        }
        const address r7 = core->get_reg(7);
        const address r8 = core->get_reg(8);
        unsigned argc = 0;
        const char *what = nullptr;
        address clazz = 0;
        address status = 0;
        if (send == k_j9_inl_get_init_status) {
            what = "getInitStatus";
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            status = clazz ? j9_read32(pr, clazz + 0x28u) : 0;
            if (clazz) {
                char tbuf[96];
                tbuf[0] = 0;
                j9_class_name(pr, clazz, tbuf, sizeof(tbuf));
                if (std::strcmp(tbuf, "java/lang/Thread") == 0) {
                    g_j9_thread_class = clazz;
                }
            }
            // Fresh J9Class objects have 0x3 at +0x28 (other flags). The
            // Java tableswitch treats 3 as a special path and verify()
            // does `if ((status&3)==3) setInitStatus(0)`, which clears
            // in-progress and re-enters initialize until SOE.
            if (clazz && ((status & ~3u) == 0) && ((status & 3u) == 3u)) {
                char nbuf[96];
                nbuf[0] = 0;
                j9_class_name(pr, clazz, nbuf, sizeof(nbuf));
                const bool exc = nbuf[0]
                    && (std::strstr(nbuf, "Error") || std::strstr(nbuf, "Exception")
                        || std::strstr(nbuf, "Throwable")
                        || std::strstr(nbuf, "StackTraceElement"));
                // Only short-circuit throwable unwind. System/Thread/etc.
                // must run real <clinit> or createMainThread NPEs.
                if (exc && j9_class_is_jcl(pr, clazz) && (g_j9_jcl_inited >= 4)) {
                    status = 1;
                    j9_write32(pr, clazz + 0x28u, 1);
                    what = "getInitStatus-jcl1";
                } else {
                    status = 0;
                    j9_write32(pr, clazz + 0x28u, 0);
                    what = "getInitStatus-norm0";
                }
            }
            address result = status & 3u;
            if (status & ~3u) {
                result |= 4u;
            }
            j9_write32(pr, r7, result);
            argc = 0;
        } else if (send == k_j9_inl_get_init_thread) {
            what = "getInitThread";
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            status = clazz ? j9_read32(pr, clazz + 0x28u) : 0;
            const address result = ((status & ~3u) == r8) ? 1u : 0u;
            j9_write32(pr, r7, result);
            argc = 0;
        } else if (send == k_j9_inl_set_init_status) {
            what = "setInitStatus";
            status = j9_read32(pr, r7);
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7 + 4u));
            if (clazz) {
                j9_write32(pr, clazz + 0x28u, status);
                if ((status == 1u) && j9_class_is_jcl(pr, clazz)) {
                    ++g_j9_jcl_inited;
                    char tname[96];
                    j9_class_name(pr, clazz, tname, sizeof(tname));
                    if (std::strcmp(tname, "java/lang/Thread") == 0) {
                        g_j9_thread_class = clazz;
                        const address vm = r8 ? j9_read32(pr, r8 + 4u) : 0;
                        const address heap = r8 ? j9_read32(pr, r8 + 0x24u) : 0;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] thread-status clazz=0x{:X} inst=0x{:X} obj=0x{:X} slot=0x{:X} "
                            "ex=0x{:X} ex2=0x{:X} heap=0x{:X} h18=0x{:X} h1c=0x{:X} h80=0x{:X} "
                            "seg8=0x{:X} seg14=0x{:X} vm2c8=0x{:X}",
                            clazz,
                            j9_read32(pr, clazz + 0x38u),
                            g_j9_thread_obj,
                            r8 ? j9_read32(pr, r8 + k_j9_thread_obj_off) : 0,
                            r8 ? j9_read32(pr, r8 + 0x64u) : 0,
                            r8 ? j9_read32(pr, r8 + 0x6cu) : 0,
                            heap,
                            heap ? j9_read32(pr, heap + 0x18u) : 0,
                            heap ? j9_read32(pr, heap + 0x1cu) : 0,
                            heap ? j9_read32(pr, heap + 0x80u) : 0,
                            j9_read32(pr, 0x02D10008u),
                            j9_read32(pr, 0x02D10014u),
                            vm ? j9_read32(pr, vm + 0x2c8u) : 0);
                    }
                }
            }
            argc = 2;
        } else if (send == k_j9_inl_set_init_thread) {
            what = "setInitThread";
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            status = clazz ? j9_read32(pr, clazz + 0x28u) : 0;
            if (clazz) {
                j9_write32(pr, clazz + 0x28u, status | r8);
            }
            argc = 1;
        } else if (send == k_j9_inl_verify_impl) {
            what = "verifyImpl-skip";
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            argc = 1;
        } else if (send == k_j9_inl_init_impl) {
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            // Class.<clinit> pulls in VM; VM.<clinit> then throws VME
            // (ICCE/SOE unwind). Skip those two; String/Thread/etc. run.
            char iname[96];
            j9_class_name(pr, clazz, iname, sizeof(iname));
            if ((std::strcmp(iname, "java/lang/Class") == 0)
                || (std::strncmp(iname, "com/ibm/oti/", 12) == 0)) {
                what = "initializeImpl-skip";
                argc = 1;
                g_j9_skip_init_clazz = clazz;
                if (std::strcmp(iname, "com/ibm/oti/io/CharacterConverter") == 0) {
                    g_j9_converter_clazz = clazz;
                }
            } else {
                return false;
            }
        } else if (send == k_j9_inl_prepare_event) {
            what = "prepareEvent-skip";
            clazz = j9_inl_class_from_slot(pr, j9_read32(pr, r7));
            argc = 1;
        } else {
            return false;
        }
        static int inl_logs = 0;
        if (inl_logs < 64) {
            ++inl_logs;
            char cname[96];
            j9_class_name(pr, clazz, cname, sizeof(cname));
            if (cname[0] && (std::strcmp(cname, "java/lang/String") == 0)) {
                g_j9_string_clazz = clazz;
            }
            if (cname[0] && (std::strcmp(cname, "java/lang/System") == 0)) {
                g_j9_system_clazz = clazz;
                g_j9_system_r4 = core->get_reg(4);
                g_j9_system_r5 = g_j9_saved_r5;
                g_j9_system_r6 = core->get_reg(6);
                g_j9_system_r7 = r7;
                g_j9_system_method = method;
                if (!g_j9_init_caller_r7) {
                    g_j9_init_caller_r4 = core->get_reg(4);
                    g_j9_init_caller_r6 = core->get_reg(6);
                    g_j9_init_caller_r7 = r7;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] init-caller r4=0x{:X} r6=0x{:X} r7=0x{:X} pc=0x{:X}",
                        g_j9_init_caller_r4, g_j9_init_caller_r6,
                        g_j9_init_caller_r7, g_j9_saved_r5);
                }
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] inl-emu {} clazz=0x{:X} name='{}' st=0x{:X} w0=0x{:X} w1=0x{:X} w2=0x{:X} w28=0x{:X} r7=0x{:X} pc=0x{:X} jcl={}",
                what, clazz, cname[0] ? cname : "?", status,
                clazz ? j9_read32(pr, clazz) : 0,
                clazz ? j9_read32(pr, clazz + 4u) : 0,
                clazz ? j9_read32(pr, clazz + 8u) : 0,
                clazz ? j9_read32(pr, clazz + 0x28u) : 0,
                r7, g_j9_saved_r5, j9_class_is_jcl(pr, clazz) ? 1 : 0);
        }
        if (g_j9_init_returned && clazz && j9_class_is_jcl(pr, clazz)) {
            if ((send == k_j9_inl_set_init_thread) && g_j9_boot_returned
                && r7 && (r7 >= 0x0071E000u) && (r7 < 0x00720000u)) {
                bool already = false;
                for (int i = 0; i < g_j9_init_depth; ++i) {
                    if (g_j9_init_stack[i].clazz == clazz) {
                        already = true;
                        break;
                    }
                }
                if (!already && (g_j9_init_depth < 8)) {
                    j9_init_frame &fr = g_j9_init_stack[g_j9_init_depth];
                    fr.clazz = clazz;
                    fr.r4 = core->get_reg(4);
                    fr.r6 = core->get_reg(6) ? core->get_reg(6) : r7;
                    fr.r7 = r7;
                    fr.csp = core->get_sp();
                    for (int i = 0; i < 16; ++i) {
                        fr.cframe[i] = 0;
                    }
                    if (fr.csp && j9_mapped32(pr, fr.csp + 32u)) {
                        for (int i = 0; i < 16; ++i) {
                            fr.cframe[i] = j9_read32(pr,
                                fr.csp + static_cast<address>(i) * 4u);
                        }
                    }
                    ++g_j9_init_depth;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] init-push clazz=0x{:X} r4=0x{:X} r6=0x{:X} r7=0x{:X} "
                        "csp=0x{:X} clr=0x{:X} depth={}",
                        clazz, fr.r4, fr.r6, fr.r7, fr.csp, fr.cframe[8],
                        g_j9_init_depth);
                }
            }
            if ((send == k_j9_inl_set_init_status) && (status == 1u)
                && (g_j9_init_depth > 0)) {
                for (int i = g_j9_init_depth - 1; i >= 0; --i) {
                    if (g_j9_init_stack[i].clazz == clazz) {
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] init-pop clazz=0x{:X} depth {}->{}",
                            clazz, g_j9_init_depth, i);
                        g_j9_init_depth = i;
                        break;
                    }
                }
            }
            if (g_j9_init_depth > 0) {
                const j9_init_frame &top = g_j9_init_stack[g_j9_init_depth - 1];
                g_j9_last_jcl_clazz = top.clazz;
                g_j9_inl_r4 = top.r4;
                g_j9_inl_r6 = top.r6;
                g_j9_inl_r7 = top.r7;
            } else {
                g_j9_last_jcl_clazz = clazz;
            }
            if (!g_j9_boot_returned && !g_j9_wrap_fp_ok) {
                j9_try_wrap_scan(pr, core, clazz);
            }
            if (!g_j9_boot_returned && !g_j9_sys_ok) {
                j9_try_save_iframe(pr, core, clazz);
            }
        }
        if (false && (send == k_j9_inl_set_init_status) && (status == 1u) && clazz
            && g_j9_init_returned && !g_j9_wrap_fp_ok && j9_class_is_jcl(pr, clazz)) {
            const address live = core->get_reg(6) ? core->get_reg(6) : (r7 + 0x1Cu);
            const address sp = core->get_sp();
            const address slr = (sp && j9_mapped32(pr, sp + 0x20u))
                ? j9_read32(pr, sp + 0x20u) : 0;
            address found = 0;
            for (int i = -8; i < 20; ++i) {
                const address at = live + static_cast<address>(i) * 4u;
                if (!j9_mapped32(pr, at + 8u)) {
                    continue;
                }
                const address pcw = j9_read32(pr, at + 4u);
                if (!j9_looks_bytecode_pc(pr, pcw)
                    || ((pcw >= 0x81980500u) && (pcw < 0x81980800u))
                    || !j9_is_invoke_op(j9_read8(pr, pcw))) {
                    continue;
                }
                found = at;
                break;
            }
            if (found) {
                g_j9_wrap_t0 = j9_read32(pr, found);
                g_j9_wrap_t1 = j9_read32(pr, found + 4u);
                g_j9_wrap_t2 = j9_read32(pr, found + 8u);
                g_j9_wrap_java_fp = found + 8u;
                g_j9_wrap_clazz = clazz;
                g_j9_wrap_fp_ok = true;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] wrap-scan clazz=0x{:X} fp=0x{:X} t-8=0x{:X} "
                    "t-4=0x{:X} t0=0x{:X}",
                    clazz, g_j9_wrap_java_fp, g_j9_wrap_t0,
                    g_j9_wrap_t1, g_j9_wrap_t2);
            }
            if (!g_j9_sys_ok && sp && j9_mapped32(pr, sp + 0x17Cu)
                && (slr >= 0x81800000u) && (slr < 0x82000000u)) {
                g_j9_sys_sp = sp;
                for (int i = 0; i < 96; ++i) {
                    g_j9_sys_frame[i] = j9_read32(pr,
                        sp + static_cast<address>(i) * 4u);
                }
                g_j9_sys_ok = true;
                if (!g_j9_wrap_clazz) {
                    g_j9_wrap_clazz = clazz;
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] system-iframe-late sp=0x{:X} lr=0x{:X} clazz=0x{:X}",
                    sp, slr, clazz);
            }
        }
        if ((send == k_j9_inl_set_init_status) && (status == 1u) && clazz) {
            const address fp = g_j9_saved_r6 ? g_j9_saved_r6 : core->get_reg(6);
            char cnm[96];
            j9_class_name(pr, clazz, cnm, sizeof(cnm));
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] init-fp name='{}' fp=0x{:X} live=0x{:X} [0]=0x{:X} [4]=0x{:X} "
                "[8]=0x{:X} [c]=0x{:X} [10]=0x{:X} [14]=0x{:X} [18]=0x{:X} r7=0x{:X}",
                cnm, fp, core->get_reg(6),
                j9_read32(pr, fp), j9_read32(pr, fp + 4u),
                j9_read32(pr, fp + 8u), j9_read32(pr, fp + 12u),
                j9_read32(pr, fp + 16u), j9_read32(pr, fp + 20u),
                j9_read32(pr, fp + 24u), r7);
            if (!g_j9_ac_fp && (std::strcmp(cnm, "java/lang/String") == 0) && fp) {
                g_j9_ac_fp = fp;
                for (int i = 0; i < 6; ++i) {
                    g_j9_ac_w[i] = j9_read32(pr, fp + static_cast<address>(i) * 4u);
                }
            }
            // J9VMInternals caller frame at 0x71E600. Opcode 0xFF does
            // `ldr pc, [r6, #-16]`, so [0x71E5F0] is the ARM glue.
            if (j9_mapped32(pr, 0x71E610u)) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] caller-hdr name='{}' e0=0x{:X} e4=0x{:X} e8=0x{:X} "
                    "ec=0x{:X} f0=0x{:X} f4=0x{:X} f8=0x{:X} fc=0x{:X} "
                    "00=0x{:X} 04=0x{:X} sp=0x{:X}",
                    cnm,
                    j9_read32(pr, 0x71E5E0u), j9_read32(pr, 0x71E5E4u),
                    j9_read32(pr, 0x71E5E8u), j9_read32(pr, 0x71E5ECu),
                    j9_read32(pr, 0x71E5F0u), j9_read32(pr, 0x71E5F4u),
                    j9_read32(pr, 0x71E5F8u), j9_read32(pr, 0x71E5FCu),
                    j9_read32(pr, 0x71E600u), j9_read32(pr, 0x71E604u),
                    core->get_sp());
                if (!g_j9_init_glue) {
                    const address glue = j9_read32(pr, 0x71E5F0u);
                    if (glue && (glue >= 0x81800000u) && (glue < 0x82000000u)) {
                        g_j9_init_glue = glue;
                        g_j9_init_sp = core->get_sp();
                        for (int i = 0; i < 16; ++i) {
                            g_j9_caller_hdr[i] = j9_read32(pr,
                                0x71E5E0u + static_cast<address>(i) * 4u);
                        }
                        if (g_j9_init_sp && j9_mapped32(pr, g_j9_init_sp + 0xFCu)) {
                            for (int i = 0; i < 64; ++i) {
                                g_j9_cframe[i] = j9_read32(pr,
                                    g_j9_init_sp + static_cast<address>(i) * 4u);
                            }
                            g_j9_cframe_ok = true;
                        }
                    }
                }
            }
            if ((clazz == g_j9_system_clazz) || (clazz == 0x727770u)) {
                // Interpreter r6 at setInitStatus is live (0x71E584), not
                // the getInitStatus snapshot (0x71E5A4). local 2 must be
                // the Class that initializeImpl synchronized on.
                const address live = core->get_reg(6) ? core->get_reg(6)
                    : (r7 + 0x1Cu);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] system-live-dump r7=0x{:X} live=0x{:X} "
                    "m20=0x{:X} m1c=0x{:X} m18=0x{:X} m14=0x{:X} "
                    "m10=0x{:X} m0c=0x{:X} m08=0x{:X} m04=0x{:X} "
                    "p00=0x{:X} p04=0x{:X} p08=0x{:X} p0c=0x{:X} "
                    "p10=0x{:X} p14=0x{:X} ret={}",
                    r7, live,
                    j9_read32(pr, live - 0x20u), j9_read32(pr, live - 0x1cu),
                    j9_read32(pr, live - 0x18u), j9_read32(pr, live - 0x14u),
                    j9_read32(pr, live - 0x10u), j9_read32(pr, live - 0x0cu),
                    j9_read32(pr, live - 0x08u), j9_read32(pr, live - 0x04u),
                    j9_read32(pr, live), j9_read32(pr, live + 4u),
                    j9_read32(pr, live + 8u), j9_read32(pr, live + 12u),
                    j9_read32(pr, live + 16u), j9_read32(pr, live + 20u),
                    g_j9_init_returned ? 1 : 0);
                if (j9_mapped32(pr, live + 24u)) {
                    // Second AC reads live+4 / +8 / +c. Official System
                    // still has [8]=0 (2A/B7 not run); plant the String
                    // shape so AC2 hits 0x81922C1D → FF → glue.
                    j9_write32(pr, live, clazz);
                    j9_write32(pr, live + 4u, 0);
                    j9_write32(pr, live + 8u, 0x81922C1Du);
                    j9_write32(pr, live + 12u, 0x71E600u);
                    if (!g_j9_init_returned) {
                        j9_write32(pr, live + 16u, clazz);
                        j9_write32(pr, live + 20u, 0);
                        j9_write32(pr, live + 24u, 0);
                    }
                }
                // Official System inner at live-0x14 is
                // {0x7257A8, 0x81980615, 0x71E5A4}. The C-loop String
                // path used outer=0x71E5EC; after init-ret the outer fp
                // is 0x71E5A4 (initializeImpl caller).
                const address inner = live - 0x14u;
                const address outer = g_j9_init_returned ? 0x71E5A4u : 0x71E5ECu;
                if (j9_mapped32(pr, inner + 8u)) {
                    j9_write32(pr, inner, g_j9_init_caller_r4
                        ? g_j9_init_caller_r4 : 0x7257A8u);
                    j9_write32(pr, inner + 4u, 0x81980615u);
                    j9_write32(pr, inner + 8u, outer);
                }
                if (g_j9_init_glue && j9_mapped32(pr, 0x71E5F0u)) {
                    j9_write32(pr, 0x71E5F0u, g_j9_init_glue);
                }
                if (!g_j9_init_returned && g_j9_caller_hdr[4]
                    && j9_mapped32(pr, 0x71E5F0u)) {
                    const address cur0 = j9_read32(pr, 0x71E5E0u);
                    if ((cur0 < 0x10000u) || (cur0 == 9u)) {
                        for (int i = 0; i < 16; ++i) {
                            j9_write32(pr, 0x71E5E0u
                                + static_cast<address>(i) * 4u, g_j9_caller_hdr[i]);
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] caller-hdr restore e0=0x{:X} f0=0x{:X}",
                            g_j9_caller_hdr[0], g_j9_caller_hdr[4]);
                    }
                }
                g_j9_saved_r4 = g_j9_init_caller_r4
                    ? g_j9_init_caller_r4 : 0x7257A8u;
                g_j9_saved_r6 = live;
                // argc=2 from incoming r7=0x71E568 → 0x71E570 = live-0x14.
                g_j9_java_sp = 0;
                core->set_reg(4, g_j9_saved_r4);
                core->set_reg(6, live);
                g_j9_resume_at = 0x81980745u;
                argc = 2;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] system-clinit-done fix live=0x{:X} r7=0x{:X} "
                    "inner=0x{:X} glue=0x{:X} "
                    "[0]=0x{:X} [4]=0x{:X} [8]=0x{:X} [c]=0x{:X} "
                    "[10]=0x{:X} i0=0x{:X} i4=0x{:X} i8=0x{:X}",
                    live, r7, inner, g_j9_init_glue,
                    j9_read32(pr, live), j9_read32(pr, live + 4u),
                    j9_read32(pr, live + 8u), j9_read32(pr, live + 12u),
                    j9_read32(pr, live + 16u),
                    j9_read32(pr, inner), j9_read32(pr, inner + 4u),
                    j9_read32(pr, inner + 8u));
            }
        }
        g_j9_resume_no_ac = true;
        j9_jxe_resume_interp(core, pr, method, argc);
        g_j9_resume_no_ac = false;
        g_j9_resume_at = 0;
        return true;
    }

    // After the JCL Graphics helper returns, the remaining caller tail is
    // `pop; aload N; return`. Official 0xAD then ldmda a missing invoke
    // frame and leaves r4=0, so the next resolve AVs at 0x81910F2C.
    // Jump back to the JXE invoke that entered JCL (0x770B08) instead.
    static bool j9_scan_jxe_parent(kernel::process *pr) {
        if (!pr) {
            return false;
        }
        if (g_j9_last_jxe_r5 && j9_is_invoke_op(j9_read8(pr, g_j9_last_jxe_r5))
            && (g_j9_last_jxe_r4 >= 0x00720000u) && (g_j9_last_jxe_r4 < 0x00730000u)
            && (g_j9_last_jxe_r6 >= 0x00710000u) && (g_j9_last_jxe_r6 < 0x00720000u)) {
            return true;
        }
        address best = 0;
        address br4 = 0;
        address br6 = 0;
        int best_score = -1;
        const auto consider = [&](const address or4, const address opc,
            const address or6) {
            if ((opc < 0x00770000u) || (opc >= 0x00780000u)
                || !j9_is_invoke_op(j9_read8(pr, opc))) {
                return;
            }
            if ((or6 < 0x00710000u) || (or6 >= 0x00720000u)) {
                return;
            }
            if ((or4 < 0x00720000u) || (or4 >= 0x00730000u)) {
                return;
            }
            int score = 20;
            if (opc == g_j9_last_jxe_r5) {
                score += 20;
            }
            if (or4 == g_j9_last_jxe_r4) {
                score += 10;
            }
            if (score > best_score) {
                best_score = score;
                best = opc;
                br4 = or4;
                br6 = or6;
            }
        };
        const auto consider_at = [&](const address ot) {
            if (!j9_mapped32(pr, ot + 8u)) {
                return;
            }
            const address w0 = j9_read32(pr, ot);
            const address w1 = j9_read32(pr, ot + 4u);
            const address w2 = j9_read32(pr, ot + 8u);
            consider(w0, w1, w2);
            consider(w1, w0, w2);
            consider(w0, w2, w1);
        };
        const address hints[3] = { g_j9_last_jxe_r6, g_j9_jcl_outer_r6, g_j9_jcl_r6 };
        for (const address hint : hints) {
            if (!hint) {
                continue;
            }
            for (int k = -32; k < 96; ++k) {
                consider_at(hint + static_cast<address>(k) * 4u);
            }
        }
        if (best) {
            g_j9_last_jxe_r4 = br4;
            g_j9_last_jxe_r5 = best;
            g_j9_last_jxe_r6 = br6;
            return true;
        }
        return false;
    }

    static address j9_obj_clazz(kernel::process *pr, const address obj) {
        if (!pr || !j9_looks_heap(obj) || ((obj & 3u) != 0)
            || !j9_mapped32(pr, obj + 4u)) {
            return 0;
        }
        const address c0 = j9_read32(pr, obj);
        const address c4 = j9_read32(pr, obj + 4u);
        if ((c0 >= 0x00720000u) && (c0 < 0x00730000u)) {
            return c0;
        }
        if ((c4 >= 0x00720000u) && (c4 < 0x00730000u)) {
            return c4;
        }
        return 0;
    }

    static bool j9_is_return_op(const std::uint8_t op) {
        return (op == 0xACu) || (op == 0xADu) || (op == 0xB0u) || (op == 0xB1u);
    }

    // AlpsFarm.<init>: `aload_0; aload_0; invokestatic getDisplay; putfield`.
    // Canvas class is not loaded yet, so do not jump into Canvas.<init>.
    static address j9_scan_canvas_marker(kernel::process *pr) {
        if (!pr) {
            return 0;
        }
        for (address mark = 0x00700000u; mark + 4u < 0x007F0000u; mark += 1u) {
            if ((j9_read8(pr, mark) == 0x2Au) && (j9_read8(pr, mark + 1u) == 0x10u)
                && (j9_read8(pr, mark + 2u) == 0x0Au)
                && (j9_read8(pr, mark + 3u) == 0xBCu)) {
                return mark;
            }
        }
        return 0;
    }

    static address j9_find_alps_putdisplay(kernel::process *pr) {
        if (!pr) {
            return 0;
        }
        int n22 = 0;
        address hits[4] = {};
        for (address p = 0x00770000u; p + 8u < 0x007A0000u; ++p) {
            if (j9_read8(pr, p) != 0x2Au || j9_read8(pr, p + 1u) != 0x2Au) {
                continue;
            }
            ++n22;
            const std::uint8_t op = j9_read8(pr, p + 2u);
            if (!j9_is_invoke_op(op)) {
                continue;
            }
            if (j9_read8(pr, p + 5u) == 0xB5u) {
                if (n22 < 4) {
                    hits[0] = p;
                }
                return p + 5u; // putfield display
            }
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ac-jcl-22 n={} hit=0x{:X}", n22, hits[0]);
        return 0;
    }

    static bool j9_ensure_java_heap(kernel::process *pr) {
        if (g_j9_java_heap_va && g_j9_java_heap_ch && g_j9_java_heap_ch->host_base()) {
            return true;
        }
        if (!pr) {
            return false;
        }
        kernel_system *kern = pr->get_kernel_object_owner();
        if (!kern) {
            return false;
        }
        g_j9_java_heap_ch = kern->create<kernel::chunk>(kern->get_memory_system(), pr, "J9JavaHeap",
            0, k_j9_java_heap_size, k_j9_java_heap_size, prot_read_write, kernel::chunk_type::normal,
            kernel::chunk_access::local, kernel::chunk_attrib::none);
        if (!g_j9_java_heap_ch) {
            g_j9_java_heap_ch = kern->create<kernel::chunk>(kern->get_memory_system(), pr, "J9JavaHeap",
                0, k_j9_java_heap_size, k_j9_java_heap_size, prot_read_write, kernel::chunk_type::normal,
                kernel::chunk_access::code, kernel::chunk_attrib::none);
        }
        if (!g_j9_java_heap_ch || !g_j9_java_heap_ch->host_base()) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] java-heap chunk failed");
            return false;
        }
        g_j9_java_heap_va = g_j9_java_heap_ch->base(pr).ptr_address();
        g_j9_java_heap_off = 0x10u;
        std::memset(g_j9_java_heap_ch->host_base(), 0, k_j9_java_heap_size);
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] java-heap va=0x{:X} size=0x{:X}",
            g_j9_java_heap_va, k_j9_java_heap_size);
        return g_j9_java_heap_va != 0;
    }

    static address j9_probe_mapped_span(kernel::process *pr, address start, address limit) {
        if (!pr || !j9_mapped32(pr, start)) {
            return 0;
        }
        address top = start;
        while ((top + 0x1000u) < limit && j9_mapped32(pr, top + 0xFFCu)) {
            top += 0x1000u;
        }
        return top + 0x1000u;
    }

    static void j9_wire_official_heap(kernel::process *pr, const address heap, const address vm) {
        if (!pr || !heap || !j9_mapped32(pr, heap + 0x1cu)) {
            return;
        }
        const address cur0 = j9_read32(pr, heap + 0x1cu);
        const address end0 = j9_read32(pr, heap + 0x18u);
        if ((end0 > cur0) && ((end0 - cur0) >= 0x80u) && j9_mapped32(pr, cur0)) {
            return;
        }
        const address mm = vm ? j9_read32(pr, vm + 4u) : 0;
        static int dumps = 0;
        if (dumps < 2) {
            ++dumps;
            const address span2d = j9_probe_mapped_span(pr, 0x02D00000u, 0x02F00000u);
            const address alt = mm ? j9_read32(pr, mm + 0x24u) : 0;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-wire heap=0x{:X} was end=0x{:X} cur=0x{:X} "
                "w0=0x{:X} w1=0x{:X} w2=0x{:X} w3=0x{:X} w4=0x{:X} w5=0x{:X} "
                "w6=0x{:X} w7=0x{:X} w8=0x{:X} w9=0x{:X} w10=0x{:X} w21=0x{:X} w31=0x{:X}",
                heap, end0, cur0,
                j9_read32(pr, heap), j9_read32(pr, heap + 4u),
                j9_read32(pr, heap + 8u), j9_read32(pr, heap + 0xcu),
                j9_read32(pr, heap + 0x10u), j9_read32(pr, heap + 0x14u),
                j9_read32(pr, heap + 0x18u), j9_read32(pr, heap + 0x1cu),
                j9_read32(pr, heap + 0x20u), j9_read32(pr, heap + 0x24u),
                j9_read32(pr, heap + 0x28u), j9_read32(pr, heap + 0x54u),
                j9_read32(pr, heap + 0x7cu));
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-meta vm=0x{:X} mm=0x{:X} mm20=0x{:X} mm24=0x{:X} mm28=0x{:X} "
                "alt=0x{:X} alt18=0x{:X} alt1c=0x{:X} span2d=0x{:X} "
                "h2d0=0x{:X} h2d4=0x{:X} h2d8=0x{:X} h2dc=0x{:X} h2d18=0x{:X} h2d1c=0x{:X}",
                vm, mm,
                mm ? j9_read32(pr, mm + 0x20u) : 0,
                mm ? j9_read32(pr, mm + 0x24u) : 0,
                mm ? j9_read32(pr, mm + 0x28u) : 0,
                alt,
                (alt && j9_mapped32(pr, alt + 0x1cu)) ? j9_read32(pr, alt + 0x18u) : 0,
                (alt && j9_mapped32(pr, alt + 0x1cu)) ? j9_read32(pr, alt + 0x1cu) : 0,
                span2d,
                j9_read32(pr, 0x02D10000u), j9_read32(pr, 0x02D10004u),
                j9_read32(pr, 0x02D10008u), j9_read32(pr, 0x02D1000cu),
                j9_read32(pr, 0x02D10018u), j9_read32(pr, 0x02D1001cu));
        }
        address base = 0;
        address top = 0;
        const address live = 0x02D10000u;
        const address live_top = j9_probe_mapped_span(pr, live, 0x02F00000u);
        const address seg_end = j9_mapped32(pr, live + 0x1cu) ? j9_read32(pr, live + 0x18u) : 0;
        const address seg_cur = j9_mapped32(pr, live + 0x1cu) ? j9_read32(pr, live + 0x1cu) : 0;
        if (seg_end > seg_cur && ((seg_end - seg_cur) >= 0x1000u)
            && j9_mapped32(pr, seg_cur) && j9_mapped32(pr, seg_end - 4u)) {
            base = seg_cur;
            top = seg_end;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-reuse seg cur=0x{:X} end=0x{:X}", base, top);
        } else if (j9_mapped32(pr, live + 4u) && (j9_read32(pr, live + 4u) >= 0x1000u)
            && (j9_read32(pr, live + 4u) <= 0x200000u)
            && j9_mapped32(pr, live + 0x20u)) {
            const address seg_sz = j9_read32(pr, live + 4u);
            base = live + 0x20u;
            top = live + seg_sz;
            if (!j9_mapped32(pr, top - 4u) && (live_top > (live + 0x2000u))) {
                top = live_top;
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-reuse segsize=0x{:X} base=0x{:X} top=0x{:X}",
                seg_sz, base, top);
        } else if (live_top > (live + 0x2000u)) {
            base = live + 0x20u;
            top = live_top;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-reuse live=0x{:X} top=0x{:X}", live, top);
        } else {
            if (!j9_ensure_java_heap(pr) || !g_j9_java_heap_va) {
                return;
            }
            base = g_j9_java_heap_va + 0x10u;
            top = g_j9_java_heap_va + k_j9_java_heap_size;
        }
        if (!j9_read32(pr, heap)) {
            j9_write32(pr, heap, 0x8u);
        }
        if (!j9_read32(pr, heap + 4u)) {
            j9_write32(pr, heap + 4u, top - base);
        }
        if (!j9_read32(pr, heap + 8u)) {
            j9_write32(pr, heap + 8u, base);
        }
        if (!j9_read32(pr, heap + 0xcu)) {
            j9_write32(pr, heap + 0xcu, base);
        }
        if (!j9_read32(pr, heap + 0x10u)) {
            j9_write32(pr, heap + 0x10u, base);
        }
        if (!j9_read32(pr, heap + 0x14u)) {
            j9_write32(pr, heap + 0x14u, top);
        }
        j9_write32(pr, heap + 0x18u, top);
        j9_write32(pr, heap + 0x1cu, base);
        const address ms = g_j9_java_heap_va ? g_j9_java_heap_va : heap;
        if (ms && j9_mapped32(pr, ms + 0xcu) && !j9_read32(pr, ms)) {
            j9_write32(pr, ms + 0x00u, heap);
            j9_write32(pr, ms + 0x04u, base);
            j9_write32(pr, ms + 0x08u, base);
            j9_write32(pr, ms + 0x0cu, top);
        }
        if (!j9_read32(pr, heap + 0x54u)) {
            j9_write32(pr, heap + 0x54u, ms);
        }
        if (!j9_read32(pr, heap + 0x7cu)) {
            j9_write32(pr, heap + 0x7cu, ms);
        }
        // [heap+0x80] is the TLH refresh slot. A stale non-zero value
        // makes 0x8191DACC copy it over heap.cur and rewind the bump.
        j9_write32(pr, heap + 0x80u, 0);
        // Official J9MemorySegment at 0x2D10000 is type+size only.
        // Native allocators that walk the segment (not the TLH) see
        // base/top/alloc == 0 and throw OOME without hitting 0x81912A4C.
        if (j9_mapped32(pr, live + 0x14u)) {
            if (!j9_read32(pr, live + 0x8u)) {
                j9_write32(pr, live + 0x8u, base);
            }
            if (!j9_read32(pr, live + 0xcu)) {
                j9_write32(pr, live + 0xcu, base);
            }
            if (!j9_read32(pr, live + 0x10u)) {
                j9_write32(pr, live + 0x10u, top);
            }
            const address seg_alloc = j9_read32(pr, live + 0x14u);
            if (!seg_alloc || (seg_alloc < base) || (seg_alloc > top)) {
                j9_write32(pr, live + 0x14u, base);
            }
            if (!j9_read32(pr, live + 0x18u)) {
                j9_write32(pr, live + 0x18u, top);
            }
            if (!j9_read32(pr, live + 0x1cu)) {
                j9_write32(pr, live + 0x1cu, j9_read32(pr, live + 0x14u));
            }
        }
        const address mm20 = (mm && j9_mapped32(pr, mm + 0x20u)) ? j9_read32(pr, mm + 0x20u) : 0;
        if (mm20 && j9_mapped32(pr, mm20 + 0x1cu)) {
            static int ms_dumps = 0;
            if (ms_dumps < 2) {
                ++ms_dumps;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] memspace=0x{:X} w0=0x{:X} w1=0x{:X} w2=0x{:X} w3=0x{:X} "
                    "w4=0x{:X} w5=0x{:X} w6=0x{:X} w7=0x{:X} w8=0x{:X} w9=0x{:X}",
                    mm20,
                    j9_read32(pr, mm20), j9_read32(pr, mm20 + 4u),
                    j9_read32(pr, mm20 + 8u), j9_read32(pr, mm20 + 0xcu),
                    j9_read32(pr, mm20 + 0x10u), j9_read32(pr, mm20 + 0x14u),
                    j9_read32(pr, mm20 + 0x18u), j9_read32(pr, mm20 + 0x1cu),
                    j9_read32(pr, mm20 + 0x20u), j9_read32(pr, mm20 + 0x24u));
            }
        }
        if (vm && j9_mapped32(pr, vm + 0x2c8u)) {
            static int vm_dumps = 0;
            if (vm_dumps < 2) {
                ++vm_dumps;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] vm-mm vm2c0=0x{:X} vm2c4=0x{:X} vm2c8=0x{:X} vm2cc=0x{:X} "
                    "seg=0x{:X} s8=0x{:X} sC=0x{:X} s10=0x{:X} s14=0x{:X} s18=0x{:X} s1c=0x{:X}",
                    j9_read32(pr, vm + 0x2c0u), j9_read32(pr, vm + 0x2c4u),
                    j9_read32(pr, vm + 0x2c8u), j9_read32(pr, vm + 0x2ccu),
                    live, j9_read32(pr, live + 8u), j9_read32(pr, live + 0xcu),
                    j9_read32(pr, live + 0x10u), j9_read32(pr, live + 0x14u),
                    j9_read32(pr, live + 0x18u), j9_read32(pr, live + 0x1cu));
            }
        }
        g_j9_official_heap = heap;
        g_j9_java_heap_off = 0x10u;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] heap-wired base=0x{:X} top=0x{:X} ms=0x{:X}", base, top, ms);
    }

    static void j9_commit_official_heap_end(kernel::process *pr, const address heap,
        const address new_top) {
        if (!pr || !heap || !new_top) {
            return;
        }
        j9_write32(pr, heap + 0x14u, new_top);
        j9_write32(pr, heap + 0x18u, new_top);
        if (j9_mapped32(pr, heap + 4u)) {
            const address base = j9_read32(pr, heap + 8u);
            if (base && (new_top > base)) {
                j9_write32(pr, heap + 4u, new_top - base);
            }
        }
        const address live = 0x02D10000u;
        if (j9_mapped32(pr, live + 0x18u)) {
            j9_write32(pr, live + 0x10u, new_top);
            j9_write32(pr, live + 0x18u, new_top);
            if (j9_mapped32(pr, live + 4u)) {
                const address b = j9_read32(pr, live + 8u);
                if (b && (new_top > b)) {
                    j9_write32(pr, live + 4u, new_top - live);
                }
            }
        }
    }

    static bool j9_grow_official_heap(kernel::process *pr, const unsigned need) {
        if (!pr || !g_j9_official_heap || !j9_mapped32(pr, g_j9_official_heap + 0x1cu)) {
            return false;
        }
        const address heap = g_j9_official_heap;
        const address cur = j9_read32(pr, heap + 0x1cu);
        const address end = j9_read32(pr, heap + 0x18u);
        const address base = j9_mapped32(pr, heap + 8u) ? j9_read32(pr, heap + 8u) : 0;
        const unsigned room = (end > cur) ? static_cast<unsigned>(end - cur) : 0;
        if ((room >= (need + 0x2000u)) && end && j9_mapped32(pr, end - 4u)) {
            return true;
        }
        const address span_from = cur ? cur : (end ? end : 0x02D10000u);
        const address mapped_top = j9_probe_mapped_span(pr, span_from, 0x03400000u);
        if (mapped_top && cur && (mapped_top > cur)
            && ((mapped_top - cur) >= (need + 0x1000u))) {
            j9_commit_official_heap_end(pr, heap, mapped_top);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-grow mapped-span top=0x{:X} cur=0x{:X} need=0x{:X}",
                mapped_top, cur, need);
            return true;
        }
        if (g_j9_heap_grow_ch && g_j9_heap_grow_ch->host_base()) {
            const address va = g_j9_heap_grow_ch->base(pr).ptr_address();
            if (va && end && (va == end) && j9_mapped32(pr, va)) {
                j9_commit_official_heap_end(pr, heap, va + 0x200000u);
                return true;
            }
            return room >= need;
        }
        kernel_system *kern = pr->get_kernel_object_owner();
        if (!kern) {
            return false;
        }
        constexpr unsigned k_grow = 0x200000u;
        const address tries[5] = {
            (end && !j9_mapped32(pr, end)) ? end : 0,
            0x02D20000u,
            0x02E00000u,
            0x03000000u,
            0x03200000u,
        };
        for (address attach : tries) {
            if (!attach || j9_mapped32(pr, attach)) {
                continue;
            }
            g_j9_heap_grow_ch = kern->create<kernel::chunk>(kern->get_memory_system(), pr, "J9HeapGrow",
                0, k_grow, k_grow, prot_read_write, kernel::chunk_type::normal,
                kernel::chunk_access::local, kernel::chunk_attrib::none, 0x00, false, attach);
            if (!g_j9_heap_grow_ch || !g_j9_heap_grow_ch->host_base()) {
                g_j9_heap_grow_ch = kern->create<kernel::chunk>(kern->get_memory_system(), pr, "J9HeapGrow",
                    0, k_grow, k_grow, prot_read_write, kernel::chunk_type::normal,
                    kernel::chunk_access::code, kernel::chunk_attrib::none, 0x00, false, attach);
            }
            if (!g_j9_heap_grow_ch || !g_j9_heap_grow_ch->host_base()) {
                g_j9_heap_grow_ch = nullptr;
                continue;
            }
            const address va = g_j9_heap_grow_ch->base(pr).ptr_address();
            const bool overlap = base && va && (va < end) && ((va + k_grow) > base);
            if (va != attach) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] heap-grow noncontig want=0x{:X} got=0x{:X} overlap={}",
                    attach, va, overlap ? 1 : 0);
                if (overlap) {
                    g_j9_heap_grow_ch = nullptr;
                    continue;
                }
                std::memset(g_j9_heap_grow_ch->host_base(), 0, k_grow);
                return room >= need;
            }
            std::memset(g_j9_heap_grow_ch->host_base(), 0, k_grow);
            j9_commit_official_heap_end(pr, heap, va + k_grow);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] heap-grow va=0x{:X} attach=0x{:X} top=0x{:X} cur=0x{:X} need=0x{:X}",
                va, attach, va + k_grow, j9_read32(pr, heap + 0x1cu), need);
            return j9_mapped32(pr, j9_read32(pr, heap + 0x1cu));
        }
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] heap-grow failed need=0x{:X} room=0x{:X}",
            need, room);
        return room >= need;
    }

    static address j9_host_alloc_obj(kernel::process *pr, const address clazz, unsigned nbytes) {
        if (!pr) {
            return 0;
        }
        if (nbytes < 0x20u) {
            nbytes = 0x20u;
        }
        nbytes = (nbytes + 7u) & ~7u;
        address obj = 0;
        if (g_j9_official_heap && j9_mapped32(pr, g_j9_official_heap + 0x1cu)) {
            const address cur = j9_read32(pr, g_j9_official_heap + 0x1cu);
            const address end = j9_read32(pr, g_j9_official_heap + 0x18u);
            if (cur && end && (end > cur) && ((end - cur) >= nbytes)
                && j9_mapped32(pr, cur) && j9_mapped32(pr, cur + nbytes - 4u)) {
                obj = cur;
                j9_write32(pr, g_j9_official_heap + 0x1cu, cur + nbytes);
            }
        }
        if (!obj && j9_ensure_java_heap(pr)
            && (g_j9_java_heap_off + nbytes <= k_j9_java_heap_size)) {
            obj = g_j9_java_heap_va + g_j9_java_heap_off;
            g_j9_java_heap_off += nbytes;
        }
        if (!obj) {
            static int n = 0;
            const address cands[5] = { 0x02D07200u, 0x02D07800u, 0x02D09000u,
                0x02D0A000u, 0x02D0B000u };
            for (address c : cands) {
                const address at = c + static_cast<address>((n % 8) * 0x80);
                if (j9_mapped32(pr, at + 0x40u)) {
                    obj = at;
                    break;
                }
            }
            ++n;
        }
        if (!obj) {
            return 0;
        }
        const address cz = (clazz && j9_mapped32(pr, clazz)) ? clazz : 0;
        const unsigned words = nbytes / 4u;
        for (unsigned i = 0; i < words; ++i) {
            j9_write32(pr, obj + i * 4u, 0);
        }
        if (cz) {
            j9_write32(pr, obj, cz);
            address flags = 0;
            const address p10 = j9_read32(pr, cz + 0x10u);
            if (p10 && j9_mapped32(pr, p10 + 0x44u)) {
                flags = j9_read32(pr, p10 + 0x44u);
            }
            flags |= (obj << 14) & 0x7FFF0000u;
            j9_write32(pr, obj + 4u, flags);
        }
        g_j9_last_java_obj = obj;
        return obj;
    }

    static void j9_write16(kernel::process *pr, const address p, const std::uint16_t v) {
        if (!pr || !p) {
            return;
        }
        if (auto *w = reinterpret_cast<std::uint16_t *>(pr->get_ptr_on_addr_space(p))) {
            *w = v;
        }
    }

    static void j9_write8(kernel::process *pr, const address p, const std::uint8_t v) {
        if (auto *w = reinterpret_cast<std::uint8_t *>(
                pr ? pr->get_ptr_on_addr_space(p) : nullptr)) {
            *w = v;
        }
    }

    static address j9_steal_char_array_clazz(kernel::process *pr, const address str_clazz) {
        if (!pr || !str_clazz) {
            return 0;
        }
        for (address p = 0x02D10020u; p < 0x02D10800u; p += 4u) {
            if (!j9_mapped32(pr, p + 8u)) {
                break;
            }
            if (j9_read32(pr, p) != str_clazz) {
                continue;
            }
            const address val = j9_read32(pr, p + 8u);
            if (val && j9_looks_heap(val) && j9_mapped32(pr, val)) {
                const address ac = j9_read32(pr, val);
                if (ac && j9_mapped32(pr, ac + 0x38u)) {
                    return ac;
                }
            }
        }
        return 0;
    }

    static void j9_store_string_fields(kernel::process *pr, const address str,
        const address arr, const unsigned n) {
        if (!pr || !str || !j9_mapped32(pr, str + 20u)) {
            return;
        }
        // CLDC: value@+8, offset@+12, count@+16.
        // J9 native GetStringUTF*: value@+0xC, offset@+0x10, count@+0x14.
        // +16 is CLDC count and J9 offset, so keep it 0 and put count at +20
        // (J9 +0x14). Java getfield count may see 0; JNI walks the char[].
        j9_write32(pr, str + 8u, arr);
        j9_write32(pr, str + 12u, arr);
        j9_write32(pr, str + 16u, 0);
        j9_write32(pr, str + 20u, n);
    }

    static address j9_new_string_utf(kernel::process *pr, const char *utf) {
        if (!pr || !utf || !g_j9_string_clazz || !j9_mapped32(pr, g_j9_string_clazz)) {
            return 0;
        }
        address ac = g_j9_char_array_clazz;
        if (!ac || !j9_mapped32(pr, ac)) {
            ac = j9_steal_char_array_clazz(pr, g_j9_string_clazz);
            g_j9_char_array_clazz = ac;
        }
        if (!ac) {
            return 0;
        }
        const unsigned n = static_cast<unsigned>(std::strlen(utf));
        // Official 32-bit J9 contiguous arrays use a 16-byte header
        // (clazz, flags, size, pad) then elements. char[] data starts at +16.
        const address arr = j9_host_alloc_obj(pr, ac, 16u + n * 2u);
        if (!arr) {
            return 0;
        }
        j9_write32(pr, arr + 8u, n);
        j9_write32(pr, arr + 12u, n);
        for (unsigned i = 0; i < n; ++i) {
            j9_write16(pr, arr + 16u + i * 2u,
                static_cast<std::uint16_t>(static_cast<unsigned char>(utf[i])));
        }
        const address str = j9_host_alloc_obj(pr, g_j9_string_clazz, 0x20u);
        if (!str) {
            return 0;
        }
        j9_store_string_fields(pr, str, arr, n);
        return str;
    }

    static address j9_rom_class_from_utf(kernel::process *pr, const address utf,
        const address lo, const address hi) {
        if (!pr || !utf || (hi <= lo)) {
            return 0;
        }
        const address start = (utf + 4u > lo) ? (utf + 4u) : lo;
        const address end = (hi < utf + 0x90000u) ? hi : (utf + 0x90000u);
        for (address field = start; field + 8u < end; field += 4u) {
            if (!j9_readable(pr, field) || !j9_readable(pr, field + 4u)) {
                continue;
            }
            const auto rel = static_cast<std::int32_t>(j9_read32(pr, field));
            if ((field + static_cast<address>(rel)) != utf) {
                continue;
            }
            const address clazz = field - 8u;
            if (!j9_readable(pr, clazz) || !j9_ptr_is_rom_class(clazz)) {
                continue;
            }
            const address sz = j9_read32(pr, clazz);
            if ((sz >= 0x40u) && (sz <= 0x100000u)) {
                return clazz;
            }
        }
        return 0;
    }

    static address j9_find_utf8_in_range(kernel::process *pr, const char *want,
        const address lo, const address hi) {
        if (!pr || !want || (hi <= lo + 8u)) {
            return 0;
        }
        const unsigned nlen = static_cast<unsigned>(std::strlen(want));
        if ((nlen < 3u) || (nlen > 160u)) {
            return 0;
        }
        for (address p = lo; p + nlen + 4u < hi; p += 2u) {
            const auto *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(p));
            if (!s || (std::memcmp(s, want, nlen) != 0)) {
                continue;
            }
            const auto *lenp = reinterpret_cast<const std::uint8_t *>(
                pr->get_ptr_on_addr_space(p - 2u));
            if (!lenp || (lenp[0] != static_cast<std::uint8_t>(nlen)) || (lenp[1] != 0)) {
                continue;
            }
            return p - 2u;
        }
        return 0;
    }

    static address j9_find_rom_class_by_name(kernel::process *pr, const char *want) {
        if (!pr || !want || !want[0]) {
            return 0;
        }
        const unsigned nlen = static_cast<unsigned>(std::strlen(want));
        if ((nlen < 3u) || (nlen > 160u)) {
            return 0;
        }
        // midp2ams XIP: file UTF8 0xA956 / ROMClass 0x4C728, E32 hdr 0x78.
        if (g_j9_midp2ams_run && (std::strcmp(want,
                "com/symbian/j2me/midp/runtimeV2/Main") == 0)) {
            const address utf = g_j9_midp2ams_run + 0xA8DCu;
            const address rom = g_j9_midp2ams_run + 0x4C6B0u;
            const auto *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(utf + 2u));
            const address sz = j9_readable(pr, rom) ? j9_read32(pr, rom) : 0;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] rom-main-peek utf=0x{:X} rom=0x{:X} sz=0x{:X} name='{}'",
                utf, rom, sz, s ? s : "");
            if (s && (std::memcmp(s, want, nlen) == 0) && (sz >= 0x40u) && (sz <= 0x8000u)) {
                return rom;
            }
        }
        const address ranges[][2] = {
            { g_j9_midp2ams_run,
                g_j9_midp2ams_run ? (g_j9_midp2ams_run + g_j9_midp2ams_size) : 0 },
            { 0x70300000u, 0x70500000u },
            { 0x81940000u, 0x81B80000u },
            { 0x81AE0000u, 0x81B20000u },
            { 0x02D00000u, 0x03000000u },
        };
        for (const auto &rg : ranges) {
            if (!rg[0] || (rg[1] <= rg[0])) {
                continue;
            }
            const address utf = j9_find_utf8_in_range(pr, want, rg[0], rg[1]);
            if (!utf) {
                continue;
            }
            if (const address clazz = j9_rom_class_from_utf(pr, utf, rg[0], rg[1])) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] rom-class '{}' utf=0x{:X} rom=0x{:X}", want, utf, clazz);
                return clazz;
            }
        }
        return 0;
    }

    static address j9_find_class_by_name(kernel::process *pr, const char *want);

    static address j9_dummy_jcl_obj(kernel::process *pr, const address prefer, unsigned nbytes) {
        const address oc = (prefer && j9_mapped32(pr, prefer + 0x38u))
            ? prefer
            : (j9_mapped32(pr, 0x725538u + 0x38u) ? 0x725538u : g_j9_string_clazz);
        return j9_host_alloc_obj(pr, oc, nbytes);
    }

    static address j9_next_rom_method(kernel::process *pr, const address rom) {
        if (!pr || !rom || !pr->get_ptr_on_addr_space(rom + 8u)) {
            return 0;
        }
        const std::uint32_t modifiers = j9_read32(pr, rom + 8u);
        std::uint32_t extra = 0;
        if (const auto *h = reinterpret_cast<const std::uint16_t *>(
                pr->get_ptr_on_addr_space(rom + 0xEu))) {
            extra = *h;
        }
        if (modifiers & 0x8000u) {
            if (const auto *b = reinterpret_cast<const std::uint8_t *>(
                    pr->get_ptr_on_addr_space(rom + 0x10u))) {
                extra += static_cast<std::uint32_t>(*b) << 16;
            }
        }
        address next = rom + 0x14u + (extra << 2);
        if (modifiers & 0x2000000u) {
            next += 4u;
        }
        if (modifiers & 0x20000u) {
            const address sl = next;
            next += 4u;
            if (const auto *h = reinterpret_cast<const std::uint16_t *>(
                    pr->get_ptr_on_addr_space(sl))) {
                next += (static_cast<std::uint32_t>(h[0]) << 4)
                    + (static_cast<std::uint32_t>(h[1]) << 2);
            }
        }
        return next;
    }

    static address j9_rom_method_named(kernel::process *pr, const address rom,
        const char *want, const char *sig) {
        if (!pr || !rom || !want) {
            return 0;
        }
        const address first = rom + 0x20u + j9_read32(pr, rom + 0x20u);
        address romm = first;
        for (int i = 0; i < 48 && romm; ++i) {
            if (!pr->get_ptr_on_addr_space(romm + 8u)) {
                break;
            }
            char nbuf[48];
            char sbuf[48];
            nbuf[0] = 0;
            sbuf[0] = 0;
            const auto reln = static_cast<std::int32_t>(j9_read32(pr, romm));
            const auto rels = static_cast<std::int32_t>(j9_read32(pr, romm + 4u));
            j9_copy_utf8(pr, romm + static_cast<address>(reln), nbuf, sizeof(nbuf));
            j9_copy_utf8(pr, romm + 4u + static_cast<address>(rels), sbuf, sizeof(sbuf));
            if (nbuf[0] && (std::strcmp(nbuf, want) == 0)
                && (!sig || (std::strcmp(sbuf, sig) == 0))) {
                return romm;
            }
            romm = j9_next_rom_method(pr, romm);
            if (!romm || (romm <= rom) || (romm > (rom + 0x800000u))) {
                break;
            }
        }
        return 0;
    }

    static address j9_ram_method_for_romm(kernel::process *pr, const address clazz,
        const address romm) {
        if (!pr || !clazz || !romm) {
            return 0;
        }
        for (unsigned off = 0x20u; off <= 0x50u; off += 4u) {
            const address tab = j9_read32(pr, clazz + off);
            if (!tab || !j9_mapped32(pr, tab + 8u)) {
                continue;
            }
            for (unsigned i = 0; i < 16u; ++i) {
                const address m = tab + i * 16u;
                if (!j9_mapped32(pr, m + 8u)) {
                    break;
                }
                const address bc = j9_read32(pr, m);
                if (bc && ((bc == (romm + 0x14u)) || (bc == romm))) {
                    return m;
                }
                const address send = j9_read32(pr, m + 8u);
                if (send == 0x81911CACu) {
                    const address cp = j9_read32(pr, m + 4u) & ~7u;
                    if (cp && j9_mapped32(pr, cp) && (j9_read32(pr, cp) == clazz)) {
                        return m;
                    }
                }
            }
        }
        return 0;
    }

    static address j9_make_interp_method(kernel::process *pr, const address clazz,
        const address romm) {
        if (!pr || !romm) {
            return 0;
        }
        if (const address hit = j9_ram_method_for_romm(pr, clazz, romm)) {
            return hit;
        }
        address tmpl = 0;
        if (j9_mapped32(pr, 0x726720u + 0x38u)) {
            tmpl = 0x726720u;
        } else if (j9_mapped32(pr, 0x7295F0u + 0x38u)) {
            tmpl = 0x7295F0u;
        }
        if (!tmpl) {
            return 0;
        }
        const address meth = j9_host_alloc_obj(pr, 0, 0x40u);
        if (!meth) {
            return 0;
        }
        for (unsigned i = 0; i < 0x40u; i += 4u) {
            j9_write32(pr, meth + i, j9_read32(pr, tmpl + i));
        }
        j9_write32(pr, meth, romm + 0x14u);
        j9_write32(pr, meth + 8u, 0x81911CACu);
        address cp = 0;
        if (clazz && j9_mapped32(pr, clazz + 4u)) {
            cp = j9_read32(pr, clazz + 4u) & ~7u;
        }
        if (cp && j9_mapped32(pr, cp)) {
            j9_write32(pr, meth + 4u, cp);
        }
        return meth;
    }

    static bool j9_host_start_alps_init(arm::core *core, kernel::process *pr,
        const address clazz) {
        if (!core || !pr || !clazz) {
            return false;
        }
        address rom = 0;
        if (j9_mapped32(pr, clazz + 0x10u)) {
            const address w = j9_read32(pr, clazz + 0x10u);
            if (w && (j9_ptr_is_rom_class(w) || j9_readable(pr, w))) {
                const address sz = j9_read32(pr, w);
                if ((sz >= 0x40u) && (sz <= 0x800000u)) {
                    rom = w;
                }
            }
        }
        const address romm = rom ? j9_rom_method_named(pr, rom, "<init>", "()V") : 0;
        const address meth = romm ? j9_make_interp_method(pr, clazz, romm) : 0;
        address mid = j9_host_alloc_obj(pr, clazz, 0x80u);
        if (!mid) {
            mid = j9_dummy_jcl_obj(pr, clazz, 0x80u);
        }
        if (mid) {
            g_j9_midlet_this = mid;
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] alps-init clazz=0x{:X} rom=0x{:X} romm=0x{:X} meth=0x{:X} mid=0x{:X} "
            "w=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            clazz, rom, romm, meth, mid,
            j9_read32(pr, clazz), j9_read32(pr, clazz + 4u),
            j9_read32(pr, clazz + 8u), j9_read32(pr, clazz + 0x10u));
        if (!meth || !mid) {
            return false;
        }
        address fp = g_j9_saved_r6 ? g_j9_saved_r6
            : (g_j9_walk_va ? (g_j9_walk_va + 0x4100u) : 0);
        if (fp && j9_mapped32(pr, fp + 16u)) {
            j9_write32(pr, fp, mid);
            g_j9_saved_r6 = fp;
            g_j9_java_sp = fp;
            if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x10u)) {
                j9_write32(pr, g_j9_vmthread + 0x10u, fp);
            }
        }
        g_j9_saved_r4 = clazz;
        g_j9_saved_r5 = j9_read32(pr, meth);
        g_j9_resume_at = g_j9_saved_r5;
        g_j9_resume_no_ac = true;
        g_j9_alps_phase = 2;
        j9_jxe_resume_interp(core, pr, meth, 1u);
        g_j9_resume_no_ac = false;
        g_j9_resume_at = 0;
        return true;
    }

    static j9_ws_bind_fn g_j9_ws_bind = nullptr;

    void j9_register_ws_bind(j9_ws_bind_fn fn) {
        g_j9_ws_bind = fn;
    }

    bool j9_bind_windowserver(kernel::process *pr, kernel::thread *thr) {
        return g_j9_ws_bind && pr && thr && g_j9_ws_bind(pr, thr);
    }

    static bool j9_host_kick_lcdui(arm::core *core, kernel::process *pr) {
        if (!core || !pr) {
            return false;
        }
        j9_ensure_java_heap(pr);
        const address disp_c = j9_find_class_by_name(pr, "javax/microedition/lcdui/Display");
        const address canvas_c = j9_find_class_by_name(pr, "javax/microedition/lcdui/Canvas");
        address tk_c = j9_find_class_by_name(pr, "javax/microedition/lcdui/Toolkit");
        if (!tk_c) {
            tk_c = j9_find_class_by_name(pr, "com/nokia/mid/ui/Toolkit");
        }
        if (!tk_c) {
            tk_c = disp_c;
        }
        address mid = g_j9_midlet_this;
        if (!mid) {
            mid = j9_dummy_jcl_obj(pr, 0x725538u, 0x80u);
            g_j9_midlet_this = mid;
        }
        const address canvas = canvas_c ? j9_host_alloc_obj(pr, canvas_c, 0x80u)
            : j9_dummy_jcl_obj(pr, 0x725538u, 0x80u);
        const address display = disp_c ? j9_host_alloc_obj(pr, disp_c, 0x80u)
            : j9_dummy_jcl_obj(pr, 0x725538u, 0x80u);
        const address toolkit = tk_c ? j9_host_alloc_obj(pr, tk_c, 0x80u)
            : j9_dummy_jcl_obj(pr, 0x725538u, 0x80u);
        if (display) {
            g_j9_display_obj = display;
        }
        if (canvas) {
            g_j9_canvas_obj = canvas;
        }
        if (toolkit) {
            g_j9_toolkit_obj = toolkit;
        }
        kernel_system *kern = pr->get_kernel_object_owner();
        kernel::thread *thr = kern ? kern->crr_thread() : nullptr;
        j9_seed_midp_bss_types(pr, thr);
        if (canvas) {
            j9_attach_dummy_peer(pr, thr, canvas >> 2);
        }
        if (display) {
            j9_attach_dummy_peer(pr, thr, display >> 2);
        }
        if (toolkit) {
            j9_attach_dummy_peer(pr, thr, toolkit >> 2);
        }
        const address env = g_j9_fake_env ? g_j9_fake_env
            : (g_j9_vmthread ? g_j9_vmthread : 0x714E00u);
        address jsp = g_j9_java_sp;
        if ((!jsp || !j9_mapped32(pr, jsp + 8u)) && g_j9_walk_va) {
            jsp = g_j9_walk_va + 0x4100u;
        }
        if (jsp && j9_mapped32(pr, jsp + 8u)) {
            j9_write32(pr, jsp, toolkit);
            j9_write32(pr, jsp + 4u, display);
            j9_write32(pr, jsp + 8u, canvas);
            g_j9_java_sp = jsp;
            if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x10u)) {
                j9_write32(pr, g_j9_vmthread + 0x10u, jsp);
            }
            core->set_reg(7, jsp);
        }
        const address ret = g_j9_park_pc ? g_j9_park_pc
            : (g_j9_lcdui_chain_bkpt ? g_j9_lcdui_chain_bkpt : 0);
        g_j9_alps_phase = 4;
        g_j9_alps_started = true;
        // Toolkit._create (0x81AF2AAC) and CJavaEventSource::Execute
        // (0x81A61CC4) both wait on the Java event thread / VM lock.
        // Main.main still holds that lock here, so continue_real deadlocks
        // in euser (pc=0x8019D8E4). Park the interpreter and attach a
        // Windowserver client on this j9 thread instead.
        core->set_reg(0, env);
        core->set_reg(1, toolkit);
        core->set_reg(2, 0);
        core->set_reg(3, 0);
        if (ret) {
            core->set_lr(ret);
        }
        const bool ws = j9_bind_windowserver(pr, thr);
        const bool game = j9_host_run_midlet(pr, "AlpsFarm");
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] alps-lcdui-kick env=0x{:X} tk=0x{:X} disp=0x{:X} cv=0x{:X} "
            "mid=0x{:X} dc=0x{:X} cc=0x{:X} tc=0x{:X} ret=0x{:X} jsp=0x{:X} ws={} game={}",
            env, toolkit, display, canvas, mid, disp_c, canvas_c, tk_c, ret, jsp,
            ws ? 1 : 0, game ? 1 : 0);
        if (game && thr && thr->get_scheduler()) {
            // Park trampoline at walk+0x3C0 sits inside the JNI name/fn pair
            // table and AVs (pc=0x702003C8). Host MIDP owns drawing now, so
            // take Main off the run queue instead of jumping there.
            thr->get_scheduler()->sleep(thr, 1800000000u, true);
            return true;
        }
        if (ret) {
            j9_set_pc(core, ret);
            if (kern) {
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(ret & ~1u, 16);
                }
            }
        }
        return true;
    }

    static bool j9_host_cms_run(arm::core *core, kernel::process *pr) {
        if (!pr) {
            return false;
        }
        address clazz = j9_find_class_by_name(pr, "AlpsFarm");
        address mid = 0;
        if (clazz) {
            mid = j9_host_alloc_obj(pr, clazz, 0x40u);
        }
        if (!mid) {
            mid = j9_dummy_jcl_obj(pr, 0x725538u, 0x40u);
        }
        if (mid) {
            g_j9_midlet_this = mid;
        }
        g_j9_alps_started = true;
        j9_ensure_java_heap(pr);
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] host-cms-run clazz=0x{:X} mid=0x{:X} fw=0x{:X} args=0x{:X} phase={}",
            clazz, mid, g_j9_dummy_fw, g_j9_dummy_args, g_j9_alps_phase);
        if (clazz && (g_j9_alps_phase < 2) && j9_host_start_alps_init(core, pr, clazz)) {
            return true;
        }
        if (!core || (g_j9_alps_phase >= 4)) {
            return false;
        }
        return j9_host_kick_lcdui(core, pr);
    }

    static void j9_run_main_cp_stub(arm::core *core, kernel::process *pr) {
        if (!core || !pr) {
            return;
        }
        const address bpc = core->get_reg(5);
        const std::uint8_t op = bpc ? j9_read8(pr, bpc) : 0;
        const unsigned idx = bpc ? (static_cast<unsigned>(j9_read8(pr, bpc + 1u))
            | (static_cast<unsigned>(j9_read8(pr, bpc + 2u)) << 8)) : 0;
        address r7 = core->get_reg(7);
        address next = bpc + ((op == 0x12u) ? 2u : 3u);
        address push = 0;
        unsigned popn = 0;
        const bool main_bc = (bpc >= 0x81AA8138u) && (bpc < 0x81AA81A0u);
        if (main_bc && (op == 0xB2u) && (idx == 17u)) {
            if (!g_j9_dummy_rt) {
                g_j9_dummy_rt = j9_dummy_jcl_obj(pr, 0x727B48u, 0x20u);
            }
            push = g_j9_dummy_rt;
        } else if (main_bc && (op == 0xB4u) && (idx == 33u)) {
            popn = 1;
            push = (g_j9_dummy_args && j9_mapped32(pr, g_j9_dummy_args + 8u))
                ? j9_read32(pr, g_j9_dummy_args + 8u) : 0;
        } else if (main_bc && (op == 0xB4u) && (idx == 36u)) {
            popn = 1;
            push = (g_j9_dummy_args && j9_mapped32(pr, g_j9_dummy_args + 12u))
                ? j9_read32(pr, g_j9_dummy_args + 12u) : 0;
        } else if (main_bc && (op == 0xB6u) && (idx == 34u)) {
            popn = 2;
            next = 0x81AA8176u;
        } else if (main_bc && (op == 0xBBu) && (idx == 46u)) {
            if (!g_j9_dummy_cms) {
                g_j9_dummy_cms = j9_dummy_jcl_obj(pr, 0x725538u, 0x30u);
            }
            push = g_j9_dummy_cms;
        } else if (main_bc && (op == 0xB7u) && (idx == 43u)) {
            popn = 3;
        } else if (main_bc && (op == 0xB6u) && (idx == 38u)) {
            popn = 2;
            next = 0x81AA8196u;
            if (j9_host_cms_run(core, pr)) {
                return;
            }
        } else if (idx == 10u) {
            next = bpc + 6u;
        } else if (idx == 11u) {
            popn = 1;
        } else if (idx == 13u) {
            if (!g_j9_dummy_fw) {
                g_j9_dummy_fw = j9_dummy_jcl_obj(pr, 0x725538u, 0x20u);
            }
            push = g_j9_dummy_fw;
        } else if (idx == 15u) {
            popn = 1;
            if (!g_j9_dummy_args) {
                g_j9_dummy_args = j9_dummy_jcl_obj(pr, 0x725538u, 0x30u);
                const address name = j9_new_string_utf(pr, "AlpsFarm");
                const address ht = j9_mapped32(pr, 0x727CD0u + 0x38u)
                    ? j9_host_alloc_obj(pr, 0x727CD0u, 0x20u) : 0;
                if (g_j9_dummy_args) {
                    j9_write32(pr, g_j9_dummy_args + 8u, ht);
                    j9_write32(pr, g_j9_dummy_args + 12u, name);
                }
            }
            push = g_j9_dummy_args;
        } else if (idx == 20u) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] startEventProcessing host");
        } else if (idx == 23u) {
            popn = 1;
        }
        r7 += popn * 4u;
        if (push) {
            r7 -= 4u;
            if (j9_mapped32(pr, r7)) {
                j9_write32(pr, r7, push);
            }
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] cp-stub op=0x{:02X} #{} next=0x{:X} push=0x{:X} pop={} "
            "r4=0x{:X} r6=0x{:X} r7=0x{:X}",
            op, idx, next, push, popn, core->get_reg(4), core->get_reg(6), r7);
        g_j9_saved_r4 = g_j9_main_clazz ? g_j9_main_clazz : core->get_reg(4);
        g_j9_saved_r5 = next;
        g_j9_saved_r6 = core->get_reg(6);
        g_j9_java_sp = r7;
        g_j9_resume_at = next;
        g_j9_resume_no_ac = true;
        address inner = g_j9_main_method;
        if (inner && j9_mapped32(pr, inner)) {
            const address p = j9_read32(pr, inner);
            if (p && j9_mapped32(pr, p + 8u)) {
                inner = p;
            }
        }
        j9_jxe_resume_interp(core, pr, inner, 0u);
        g_j9_resume_no_ac = false;
        g_j9_resume_at = 0;
    }

    static address j9_make_ram_class(kernel::process *pr, const address rom) {
        if (!pr || !rom) {
            return 0;
        }
        address tmpl = 0;
        if (j9_mapped32(pr, 0x729540u + 0x38u)) {
            tmpl = 0x729540u;
        } else if (g_j9_string_clazz && j9_mapped32(pr, g_j9_string_clazz + 0x38u)) {
            tmpl = g_j9_string_clazz;
        }
        if (!tmpl) {
            return 0;
        }
        const unsigned nbytes = 0xB0u;
        address ram = 0;
        const bool is_main = g_j9_midp2ams_run
            && (rom == (g_j9_midp2ams_run + 0x4C6B0u));
        if (is_main && g_j9_walk_va && j9_mapped32(pr, g_j9_walk_va + 0x3C00u + nbytes)) {
            ram = g_j9_walk_va + 0x3C00u;
        } else {
            ram = j9_host_alloc_obj(pr, 0, nbytes);
        }
        if (!ram) {
            return 0;
        }
        for (unsigned i = 0; i < nbytes; i += 4u) {
            j9_write32(pr, ram + i, j9_read32(pr, tmpl + i));
        }
        // Live JCL J9Class: +04 flags, +10 romClass, +18 init, +1C loader.
        // Do not smash +04; String/CP use +10 as romClass.
        j9_write32(pr, ram + 0x10u, rom);
        if (j9_mapped32(pr, ram + 0x18u)) {
            j9_write32(pr, ram + 0x18u, 1);
        }
        if (j9_mapped32(pr, ram + 0x28u)) {
            j9_write32(pr, ram + 0x28u, 1);
        }
        const address loader = j9_read32(pr, tmpl + 0x1Cu);
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ram-class-synth rom=0x{:X} tmpl=0x{:X} -> 0x{:X} loader=0x{:X} "
            "w=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            rom, tmpl, ram, loader,
            j9_read32(pr, ram), j9_read32(pr, ram + 4u),
            j9_read32(pr, ram + 8u), j9_read32(pr, ram + 0x0Cu),
            j9_read32(pr, ram + 0x10u), j9_read32(pr, ram + 0x14u),
            j9_read32(pr, ram + 0x18u), j9_read32(pr, ram + 0x1Cu));
        return ram;
    }

    static address j9_make_main_method(kernel::process *pr) {
        if (!pr || !g_j9_midp2ams_run) {
            return g_j9_main_method;
        }
        if (g_j9_main_method && j9_mapped32(pr, g_j9_main_method + 8u)) {
            return g_j9_main_method;
        }
        const address romm = g_j9_midp2ams_run + 0x4C9DCu;
        address tmpl = 0;
        if (j9_mapped32(pr, 0x726720u + 0x38u)) {
            tmpl = 0x726720u;
        } else if (j9_mapped32(pr, 0x7295F0u + 0x38u)) {
            tmpl = 0x7295F0u;
        }
        if (!tmpl) {
            return 0;
        }
        address meth = g_j9_walk_va ? (g_j9_walk_va + 0x3D00u)
            : j9_host_alloc_obj(pr, 0, 0x40u);
        if (!meth) {
            return 0;
        }
        for (unsigned i = 0; i < 0x40u; i += 4u) {
            j9_write32(pr, meth + i, j9_read32(pr, tmpl + i));
        }
        // J9Method[0] is bytecodes (first opcode). ROMMethod header is
        // 0x14 bytes; bytecodes[-3] is argCount=1 for main(String[]).
        // Keep [+8] (methodRunAddress) from the live JCL template.
        j9_write32(pr, meth, romm + 0x14u);
        if (g_j9_main_clazz && g_j9_walk_va) {
            const address romc = g_j9_midp2ams_run + 0x4C6B0u;
            const address romcp = romc + 0x68u;
            const address cp = g_j9_walk_va + 0x3E00u;
            g_j9_main_cp = cp;
            j9_write32(pr, cp, g_j9_main_clazz);
            j9_write32(pr, cp + 4u, romcp);
            for (unsigned i = 8; i < 0x200u; i += 4u) {
                j9_write32(pr, cp + i, 0);
            }
            address stub = g_j9_walk_va + 0x3D80u;
            for (unsigned i = 0; i < 0x40u; i += 4u) {
                j9_write32(pr, stub + i, j9_read32(pr, tmpl + i));
            }
            const address stub_send = g_j9_cp_stub_bkpt
                ? g_j9_cp_stub_bkpt : (g_j9_walk_va + 0x320u);
            g_j9_cp_stub_bkpt = stub_send;
            j9_write32(pr, stub + 8u, stub_send);
            j9_write32(pr, stub, 0x81AA8138u);
            g_j9_cp_stub_meth = stub;
            unsigned nstr = 0;
            unsigned nstat = 0;
            unsigned ncls = 0;
            for (unsigned i = 1; i < 48u; ++i) {
                const address it = romcp + i * 8u;
                if (!j9_readable(pr, it + 4u)) {
                    break;
                }
                const address slot1 = j9_read32(pr, it);
                const address type = j9_read32(pr, it + 4u);
                const address ram = cp + i * 8u;
                if (type == 1u) {
                    const auto rel = static_cast<std::int32_t>(slot1);
                    const address utf = it + static_cast<address>(rel);
                    char ubuf[96];
                    ubuf[0] = 0;
                    if (j9_copy_utf8(pr, utf, ubuf, sizeof(ubuf))) {
                        const address s = j9_new_string_utf(pr, ubuf);
                        j9_write32(pr, ram, s);
                        ++nstr;
                    }
                } else if (type == 2u) {
                    const auto rel = static_cast<std::int32_t>(slot1);
                    const address utf = it + static_cast<address>(rel);
                    char ubuf[96];
                    ubuf[0] = 0;
                    j9_copy_utf8(pr, utf, ubuf, sizeof(ubuf));
                    address rc = 0;
                    if (std::strcmp(ubuf, "java/lang/StringBuffer") == 0) {
                        rc = 0x7285B8u;
                    } else if (std::strcmp(ubuf, "java/lang/Object") == 0) {
                        rc = 0x725538u;
                    } else if (std::strcmp(ubuf, "java/lang/Runtime") == 0) {
                        rc = 0x727B48u;
                    } else if (std::strcmp(ubuf, "java/util/Hashtable") == 0) {
                        rc = 0x727CD0u;
                    }
                    if (rc && j9_mapped32(pr, rc + 0x38u)) {
                        j9_write32(pr, ram, rc);
                        ++ncls;
                    }
                } else if ((type == 0x158u) || (type == 0x160u)
                    || (type == 0x168u) || (type == 0x170u)) {
                    j9_write32(pr, ram, stub);
                    ++nstat;
                } else if (type == 0x150u) {
                    // static field Runtime.RUNTIME: cell at walk+0x3FC0
                    const address cell = g_j9_walk_va + 0x3FE0u;
                    const address obj = j9_host_alloc_obj(pr, 0x727B48u, 0x20u);
                    j9_write32(pr, cell, obj);
                    j9_write32(pr, ram, cell);
                    j9_write32(pr, ram + 4u, 0);
                }
            }
            j9_write32(pr, meth + 4u, cp);
            j9_write32(pr, g_j9_main_clazz + 0x10u, romc);
            const address tag = j9_read32(pr, g_j9_main_clazz + 4u) & 7u;
            j9_write32(pr, g_j9_main_clazz + 4u, (cp & ~7u) | tag);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] main-cp fill str={} stat={} cls={} stub=0x{:X} send=0x{:X} "
                "tag=0x{:X} c4=0x{:X}",
                nstr, nstat, ncls, stub, stub_send, tag,
                j9_read32(pr, g_j9_main_clazz + 4u));
        }
        // j9.dll treats jmethodID as J9JNIMethodID* { J9Method*, vTableIndex }.
        address jid = g_j9_walk_va ? (g_j9_walk_va + 0x3D40u)
            : j9_host_alloc_obj(pr, 0, 0x10u);
        if (jid) {
            j9_write32(pr, jid, meth);
            j9_write32(pr, jid + 4u, 0);
        }
        g_j9_main_method = jid ? jid : meth;
        const address tcp = j9_read32(pr, tmpl + 4u);
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] make-main-method jid=0x{:X} meth=0x{:X} tmpl=0x{:X} "
            "romm=0x{:X} bc=0x{:X} send=0x{:X} cp=0x{:X} "
            "tcp=0x{:X}/0x{:X}/0x{:X}/0x{:X} tw=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            jid, meth, tmpl, romm, romm + 0x14u,
            j9_read32(pr, meth + 8u), j9_read32(pr, meth + 4u),
            tcp,
            tcp ? j9_read32(pr, tcp) : 0, tcp ? j9_read32(pr, tcp + 4u) : 0,
            tcp ? j9_read32(pr, tcp + 8u) : 0,
            j9_read32(pr, tmpl), j9_read32(pr, tmpl + 4u),
            j9_read32(pr, tmpl + 8u), j9_read32(pr, tmpl + 12u));
        return g_j9_main_method;
    }

    static address j9_ram_class_for_rom(kernel::process *pr, const address rom) {
        if (!pr || !rom) {
            return 0;
        }
        const address ranges[][2] = {
            { 0x00720000u, 0x00740000u },
            { 0x00700000u, 0x00780000u },
            { 0x004100000u, 0x004200000u },
            { 0x02D10000u, 0x02D20000u },
        };
        for (const auto &rg : ranges) {
            for (address p = rg[0]; p + 0x38u < rg[1]; p += 8u) {
                if (!j9_mapped32(pr, p + 0x38u)) {
                    continue;
                }
                for (unsigned off = 0; off <= 0x10u; off += 4u) {
                    const address w = j9_read32(pr, p + off);
                    if ((w == rom) || ((w & ~7u) == rom)) {
                        return p;
                    }
                    if (w && j9_mapped32(pr, w) && (j9_read32(pr, w) == rom)) {
                        return p;
                    }
                }
            }
        }
        return 0;
    }

    static address j9_walk_loader_classes(kernel::process *pr, const address loader,
        const char *want) {
        if (!pr || !loader || !want || !j9_mapped32(pr, loader + 0x20u)) {
            return 0;
        }
        char nbuf[96];
        for (unsigned off = 0x10u; off <= 0x30u; off += 4u) {
            const address tab = j9_read32(pr, loader + off);
            if (!tab || !j9_mapped32(pr, tab + 0x10u)) {
                continue;
            }
            const address n = j9_read32(pr, tab);
            const address nodes = j9_read32(pr, tab + 8u);
            if ((n == 0) || (n > 0x4000u) || !nodes || !j9_mapped32(pr, nodes)) {
                continue;
            }
            const unsigned lim = (n > 0x200u) ? 0x200u : static_cast<unsigned>(n);
            for (unsigned i = 0; i < lim; ++i) {
                address node = j9_read32(pr, nodes + i * 4u);
                for (int hop = 0; node && (hop < 24); ++hop) {
                    if (!j9_mapped32(pr, node + 8u)) {
                        break;
                    }
                    const address clazz = j9_read32(pr, node + 4u);
                    if (clazz && j9_mapped32(pr, clazz + 0x38u)) {
                        nbuf[0] = 0;
                        j9_class_name(pr, clazz, nbuf, sizeof(nbuf));
                        if (nbuf[0] && (std::strcmp(nbuf, want) == 0)) {
                            return clazz;
                        }
                    }
                    node = j9_read32(pr, node);
                }
            }
        }
        return 0;
    }

    static address j9_find_class_by_name(kernel::process *pr, const char *want) {
        if (!pr || !want || !want[0]) {
            return 0;
        }
        char nbuf[96];
        const address ranges[][2] = {
            { 0x00700000u, 0x00780000u },
            { 0x004100000u, 0x004200000u },
            { 0x02D10000u, 0x02D20000u },
        };
        for (const auto &rg : ranges) {
            for (address p = rg[0]; p < rg[1]; p += 8u) {
                if (!j9_mapped32(pr, p + 0x38u)) {
                    continue;
                }
                nbuf[0] = 0;
                j9_class_name(pr, p, nbuf, sizeof(nbuf));
                if (nbuf[0] && (std::strcmp(nbuf, want) == 0)) {
                    return p;
                }
            }
        }
        if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 4u)) {
            const address vm = j9_read32(pr, g_j9_vmthread + 4u);
            if (vm && j9_mapped32(pr, vm + 0x250u)) {
                if (const address hit = j9_walk_loader_classes(pr,
                        j9_read32(pr, vm + 0x250u), want)) {
                    return hit;
                }
                if (const address hit = j9_walk_loader_classes(pr,
                        j9_read32(pr, vm + 0x58u), want)) {
                    return hit;
                }
            }
        }
        const address rom = j9_find_rom_class_by_name(pr, want);
        if (rom) {
            if (const address ram = j9_ram_class_for_rom(pr, rom)) {
                return ram;
            }
            return j9_make_ram_class(pr, rom);
        }
        return 0;
    }

    static address j9_string_array_clazz(kernel::process *pr) {
        if (g_j9_string_array_clazz && j9_mapped32(pr, g_j9_string_array_clazz + 0x38u)) {
            return g_j9_string_array_clazz;
        }
        if (g_j9_string_clazz && j9_mapped32(pr, g_j9_string_clazz + 0x30u)) {
            for (unsigned off = 0x14u; off <= 0x30u; off += 4u) {
                const address ac = j9_read32(pr, g_j9_string_clazz + off);
                if (!ac || !j9_mapped32(pr, ac + 0x38u)) {
                    continue;
                }
                char nbuf[96];
                nbuf[0] = 0;
                j9_class_name(pr, ac, nbuf, sizeof(nbuf));
                if ((nbuf[0] == '[') && std::strstr(nbuf, "String")) {
                    g_j9_string_array_clazz = ac;
                    return ac;
                }
            }
        }
        g_j9_string_array_clazz = j9_find_class_by_name(pr, "[Ljava/lang/String;");
        if (!g_j9_string_array_clazz) {
            g_j9_string_array_clazz = j9_find_class_by_name(pr, "[Ljava/lang/String");
        }
        // System.initialize has already created Hashtable.Entry[]. That is
        // an object-array class; aaload of our Strings is enough for the
        // property loop (we skip the invoke, so no checkcast).
        if (!g_j9_string_array_clazz && j9_mapped32(pr, 0x7281C8u + 0x38u)) {
            g_j9_string_array_clazz = 0x7281C8u;
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] proplist-ac-fallback 0x7281C8");
        }
        return g_j9_string_array_clazz;
    }

    static address j9_new_string_array(kernel::process *pr, const char *const *kv, unsigned n) {
        if (!pr) {
            return 0;
        }
        const address ac = j9_string_array_clazz(pr);
        if (!ac) {
            return 0;
        }
        const address arr = j9_host_alloc_obj(pr, ac, 16u + n * 4u);
        if (!arr) {
            return 0;
        }
        j9_write32(pr, arr + 8u, n);
        j9_write32(pr, arr + 12u, n);
        for (unsigned i = 0; i < n; ++i) {
            const address s = (kv[i] && kv[i][0]) ? j9_new_string_utf(pr, kv[i]) : 0;
            j9_write32(pr, arr + 16u + i * 4u, s);
        }
        return arr;
    }

    static const char *j9_jcl_name_for_fn(const address walk) {
        for (const auto &ent : j9_jni_exports) {
            if ((ent.fn & ~1u) == (walk & ~1u)) {
                return ent.name.c_str();
            }
        }
        return nullptr;
    }

    static bool j9_is_char_array(kernel::process *pr, const address p) {
        if (!pr || !p || !j9_mapped32(pr, p + 16u)) {
            return false;
        }
        const address clazz = j9_read32(pr, p);
        if (g_j9_char_array_clazz && (clazz == g_j9_char_array_clazz)) {
            return true;
        }
        return clazz == 0x725F58u;
    }

    static bool j9_string_fields(kernel::process *pr, const address str,
        address *arr_out, unsigned *off_out, unsigned *n_out) {
        if (!pr || !str || !j9_mapped32(pr, str + 20u)) {
            return false;
        }
        const address a8 = j9_read32(pr, str + 8u);
        const address a12 = j9_read32(pr, str + 12u);
        const unsigned a16 = j9_read32(pr, str + 16u);
        const unsigned a20 = j9_read32(pr, str + 20u);
        address arr = 0;
        unsigned off = 0;
        unsigned n = 0;
        if (j9_is_char_array(pr, a8)) {
            arr = a8;
            off = a12;
            n = a16;
        } else if (j9_is_char_array(pr, a12)) {
            arr = a12;
            off = a16;
            n = a20;
        } else {
            return false;
        }
        if (!arr || (n > 256u) || (off > 256u)) {
            return false;
        }
        if (arr_out) {
            *arr_out = arr;
        }
        if (off_out) {
            *off_out = off;
        }
        if (n_out) {
            *n_out = n;
        }
        return true;
    }

    static unsigned j9_string_hash(kernel::process *pr, const address str) {
        address arr = 0;
        unsigned off = 0;
        unsigned n = 0;
        if (!j9_string_fields(pr, str, &arr, &off, &n)) {
            return 0;
        }
        const auto *p = reinterpret_cast<const std::uint8_t *>(
            pr->get_ptr_on_addr_space(arr + 16u + off * 2u));
        if (!p) {
            return 0;
        }
        unsigned h = 0;
        for (unsigned i = 0; i < n; ++i) {
            const unsigned ch = static_cast<unsigned>(p[i * 2u])
                | (static_cast<unsigned>(p[i * 2u + 1u]) << 8);
            h = h * 31u + ch;
        }
        return h;
    }

    static bool j9_hashtable_put(kernel::process *pr, const address ht,
        const address key, const address val) {
        if (!pr || !ht || !key || !j9_mapped32(pr, ht + 20u)) {
            return false;
        }
        const address table = j9_read32(pr, ht + 12u);
        if (!table || !j9_mapped32(pr, table + 16u)) {
            return false;
        }
        unsigned tlen = j9_read32(pr, table + 12u);
        if ((tlen < 1u) || (tlen > 1024u)) {
            const unsigned t8 = j9_read32(pr, table + 8u);
            if ((t8 >= 1u) && (t8 <= 1024u)) {
                tlen = t8;
            } else {
                tlen = 101u;
            }
        }
        const unsigned hash = j9_string_hash(pr, key);
        const unsigned idx = (hash & 0x7FFFFFFFu) % tlen;
        address eclazz = 0x728058u;
        if (!j9_mapped32(pr, eclazz)) {
            const address ac = j9_read32(pr, table);
            const address comp = (ac && j9_mapped32(pr, ac + 0x38u))
                ? j9_read32(pr, ac + 0x38u) : 0;
            eclazz = (comp && j9_mapped32(pr, comp)) ? comp : 0;
        }
        if (!eclazz) {
            eclazz = 0x725538u;
        }
        if (!eclazz || !j9_mapped32(pr, eclazz)) {
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] ht-put-fail no-entry tab=0x{:X} tlen={} ac=0x{:X}",
                table, tlen, j9_read32(pr, table));
            return false;
        }
        const address slot = table + 16u + idx * 4u;
        const address e = j9_host_alloc_obj(pr, eclazz, 0x20u);
        if (!e) {
            return false;
        }
        j9_write32(pr, e + 8u, hash);
        j9_write32(pr, e + 12u, key);
        j9_write32(pr, e + 16u, val);
        j9_write32(pr, e + 20u, j9_mapped32(pr, slot) ? j9_read32(pr, slot) : 0);
        j9_write32(pr, slot, e);
        j9_write32(pr, ht + 8u, j9_read32(pr, ht + 8u) + 1u);
        return true;
    }

    static void j9_string_text(kernel::process *pr, const address str, char *out, unsigned cap) {
        if (!out || !cap) {
            return;
        }
        out[0] = 0;
        address arr = 0;
        unsigned off = 0;
        unsigned n0 = 0;
        if (!j9_string_fields(pr, str, &arr, &off, &n0)) {
            return;
        }
        const auto *p = reinterpret_cast<const std::uint8_t *>(
            pr->get_ptr_on_addr_space(arr + 16u + off * 2u));
        if (!p) {
            return;
        }
        unsigned n = n0;
        if (n + 1u > cap) {
            n = cap - 1u;
        }
        for (unsigned i = 0; i < n; ++i) {
            const unsigned ch = static_cast<unsigned>(p[i * 2u]);
            out[i] = (ch >= 32u && ch < 127u) ? static_cast<char>(ch) : '?';
        }
        out[n] = 0;
    }

    static bool j9_string_eq(kernel::process *pr, const address a, const address b) {
        if (!a || !b) {
            return false;
        }
        if (a == b) {
            return true;
        }
        address aa = 0;
        address ba = 0;
        unsigned oa = 0;
        unsigned ob = 0;
        unsigned na = 0;
        unsigned nb = 0;
        if (!j9_string_fields(pr, a, &aa, &oa, &na)
            || !j9_string_fields(pr, b, &ba, &ob, &nb) || (na != nb)) {
            return false;
        }
        const auto *pa = reinterpret_cast<const std::uint8_t *>(
            pr->get_ptr_on_addr_space(aa + 16u + oa * 2u));
        const auto *pb = reinterpret_cast<const std::uint8_t *>(
            pr->get_ptr_on_addr_space(ba + 16u + ob * 2u));
        if (!pa || !pb) {
            return false;
        }
        for (unsigned i = 0; i < na; ++i) {
            if ((pa[i * 2u] != pb[i * 2u]) || (pa[i * 2u + 1u] != pb[i * 2u + 1u])) {
                return false;
            }
        }
        return true;
    }

    static address j9_host_prop_get(kernel::process *pr, const address key) {
        char kbuf[48];
        j9_string_text(pr, key, kbuf, sizeof(kbuf));
        if (!kbuf[0]) {
            return 0;
        }
        for (int i = 0; i < g_j9_ht_n; ++i) {
            if (std::strcmp(g_j9_ht_keys[i], kbuf) == 0) {
                return g_j9_ht_vals[i];
            }
        }
        return 0;
    }

    static address j9_hashtable_get(kernel::process *pr, const address ht,
        const address key) {
        if (!pr || !key) {
            return 0;
        }
        if (const address hostv = j9_host_prop_get(pr, key)) {
            return hostv;
        }
        if (!ht || !j9_mapped32(pr, ht + 12u)) {
            return 0;
        }
        const address table = j9_read32(pr, ht + 12u);
        if (!table || !j9_mapped32(pr, table + 16u)) {
            return 0;
        }
        unsigned tlen = j9_read32(pr, table + 12u);
        if ((tlen < 1u) || (tlen > 1024u)) {
            tlen = 101u;
        }
        const unsigned hash = j9_string_hash(pr, key);
        const unsigned idx = (hash & 0x7FFFFFFFu) % tlen;
        address e = j9_read32(pr, table + 16u + idx * 4u);
        for (int n = 0; e && (n < 32) && j9_mapped32(pr, e + 20u); ++n) {
            const address ek = j9_read32(pr, e + 12u);
            if (j9_string_eq(pr, ek, key)) {
                return j9_read32(pr, e + 16u);
            }
            e = j9_read32(pr, e + 20u);
        }
        for (unsigned i = 0; i < tlen; ++i) {
            e = j9_read32(pr, table + 16u + i * 4u);
            for (int n = 0; e && (n < 32) && j9_mapped32(pr, e + 20u); ++n) {
                const address ek = j9_read32(pr, e + 12u);
                if (j9_string_eq(pr, ek, key)) {
                    return j9_read32(pr, e + 16u);
                }
                e = j9_read32(pr, e + 20u);
            }
        }
        char kbuf[48];
        j9_string_text(pr, key, kbuf, sizeof(kbuf));
        char seen[160];
        seen[0] = 0;
        unsigned sl = 0;
        for (unsigned i = 0; (i < tlen) && (sl + 20u < sizeof(seen)); ++i) {
            e = j9_read32(pr, table + 16u + i * 4u);
            if (!e || !j9_mapped32(pr, e + 12u)) {
                continue;
            }
            char one[24];
            j9_string_text(pr, j9_read32(pr, e + 12u), one, sizeof(one));
            if (!one[0]) {
                continue;
            }
            sl += static_cast<unsigned>(std::snprintf(seen + sl, sizeof(seen) - sl, "%s%s",
                sl ? "," : "", one));
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ht-miss key=0x{:X} '{}' have='{}'", key, kbuf, seen);
        return 0;
    }

    static void j9_fill_system_properties(kernel::process *pr) {
        if (g_j9_ht_filled || !pr) {
            return;
        }
        address ht = 0x2D103B4u;
        if (!j9_mapped32(pr, ht + 12u) || (j9_read32(pr, ht) != 0x727CD0u)) {
            ht = 0;
        }
        if (!ht) {
            return;
        }
        static const char *kv[] = {
            "file.encoding", "ISO-8859-1",
            "os.encoding", "ISO-8859-1",
            "ibm.system.encoding", "ISO-8859-1",
            "console.encoding", "ISO-8859-1",
            "microedition.encoding", "ISO-8859-1",
            "microedition.platform", "Nokia5320/05.00",
            "microedition.configuration", "CLDC-1.1",
            "microedition.profiles", "MIDP-2.0",
            "os.name", "Symbian OS",
            "os.arch", "arm",
            "os.version", "9.2",
            "line.separator", "\n",
            "file.separator", "\\",
            "path.separator", ";",
            "com.ibm.oti.configuration", "cldc",
            "com.ibm.oti.jcl.build", "next",
            "java.version", "1.4.2",
            "java.vendor", "IBM",
            "java.vendor.url", "http://www.ibm.com",
            "java.home", "C:\\",
            "java.class.path", "",
            "java.vm.version", "2.3",
            "java.vm.vendor", "IBM",
            "java.vm.name", "J9",
            "java.specification.version", "1.4",
            "java.specification.name", "J2ME",
            "user.dir", "C:\\",
            "user.home", "C:\\",
            "user.name", "user",
            "user.language", "en",
            "user.region", "US",
        };
        int n = 0;
        const address tab = j9_read32(pr, ht + 12u);
        const address k0 = j9_new_string_utf(pr, kv[0]);
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ht-fill-try ht=0x{:X} sclazz=0x{:X} aclazz=0x{:X} k0=0x{:X} "
            "tab=0x{:X} tw=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            ht, g_j9_string_clazz, g_j9_char_array_clazz, k0, tab,
            tab ? j9_read32(pr, tab) : 0, tab ? j9_read32(pr, tab + 4u) : 0,
            tab ? j9_read32(pr, tab + 8u) : 0, tab ? j9_read32(pr, tab + 12u) : 0,
            tab ? j9_read32(pr, tab + 16u) : 0, tab ? j9_read32(pr, tab + 20u) : 0);
        g_j9_ht_n = 0;
        for (unsigned i = 0; i + 1u < sizeof(kv) / sizeof(kv[0]); i += 2u) {
            const address k = (i == 0) ? k0 : j9_new_string_utf(pr, kv[i]);
            const address v = j9_new_string_utf(pr, kv[i + 1u]);
            if (k && v) {
                if (j9_hashtable_put(pr, ht, k, v)) {
                    ++n;
                }
                if (g_j9_ht_n < 40) {
                    std::strncpy(g_j9_ht_keys[g_j9_ht_n], kv[i], 47);
                    g_j9_ht_keys[g_j9_ht_n][47] = 0;
                    g_j9_ht_vals[g_j9_ht_n] = v;
                    ++g_j9_ht_n;
                }
            }
        }
        if (n) {
            g_j9_ht_filled = true;
            g_j9_system_ht = ht;
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ht-fill ht=0x{:X} n={} count=0x{:X} tab=0x{:X} hostn={}",
            ht, n, j9_read32(pr, ht + 8u), j9_read32(pr, ht + 12u), g_j9_ht_n);
    }

    static address j9_ensure_proplist(kernel::process *pr) {
        if (g_j9_proplist && j9_mapped32(pr, g_j9_proplist)) {
            return g_j9_proplist;
        }
        static const char *kv[] = {
            "file.encoding", "ISO-8859-1",
            "os.encoding", "ISO-8859-1",
            "ibm.system.encoding", "ISO-8859-1",
            "microedition.encoding", "ISO-8859-1",
            "microedition.platform", "Nokia5320/05.00",
            "microedition.configuration", "CLDC-1.1",
            "microedition.profiles", "MIDP-2.0",
            "os.name", "Symbian OS",
            "os.arch", "arm",
            "os.version", "9.2",
            "line.separator", "\n",
            "file.separator", "\\",
            "path.separator", ";",
            "com.ibm.oti.configuration", "cldc",
            "com.ibm.oti.jcl.build", "next",
            "java.version", "1.4.2",
            "java.vendor", "IBM",
            "java.home", "C:\\",
            "java.class.path", "",
            "java.vm.version", "2.3",
            "java.vm.vendor", "IBM",
            "java.vm.name", "J9",
            "user.dir", "C:\\",
            "user.home", "C:\\",
            "user.name", "user",
        };
        g_j9_proplist = j9_new_string_array(pr, kv,
            static_cast<unsigned>(sizeof(kv) / sizeof(kv[0])));
        if (g_j9_proplist) {
            return g_j9_proplist;
        }
        // String[] may not be loaded yet. arraylength only needs size at +8,
        // so an empty array with any array clazz lets the for-loop skip.
        address ac = g_j9_char_array_clazz;
        if (!ac || !j9_mapped32(pr, ac)) {
            ac = 0x7281C8u;
        }
        const address arr = j9_host_alloc_obj(pr, ac, 16u);
        if (arr) {
            j9_write32(pr, arr + 8u, 0);
            j9_write32(pr, arr + 12u, 0);
            g_j9_proplist = arr;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] proplist-empty arr=0x{:X} ac=0x{:X}", arr, ac);
        }
        return g_j9_proplist;
    }

    static bool j9_rom_method_name(kernel::process *pr, const address rom, char *out, size_t n) {
        if (out && n) {
            out[0] = 0;
        }
        if (!pr || !rom || !out || (n < 4)) {
            return false;
        }
        for (unsigned off = 0; off <= 0x10u; off += 4u) {
            const auto rel = static_cast<std::int32_t>(j9_read32(pr, rom + off));
            const address tgt = rom + off + static_cast<address>(rel);
            if (j9_copy_utf8(pr, tgt, out, n) || j9_copy_utf8(pr, rom + off, out, n)) {
                return true;
            }
        }
        return false;
    }

    static bool j9_in_system_init(const address p) {
        return (p >= 0x8195D600u) && (p < 0x8195D900u);
    }

    static bool j9_is_invoke_byte(const std::uint8_t op) {
        return (op == 0xB6u) || (op == 0xB7u) || (op == 0xB8u) || (op == 0xB9u);
    }

    static address j9_scan_invoke_pc(kernel::process *pr, const address jsp) {
        if (!pr || !jsp) {
            return 0;
        }
        address sys = 0;
        address any = 0;
        for (int i = 0; i < 16; ++i) {
            const address at = jsp + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, at)) {
                continue;
            }
            const address w = j9_read32(pr, at);
            if (!j9_looks_bytecode_pc(pr, w) || !j9_is_invoke_byte(j9_read8(pr, w))) {
                continue;
            }
            if (j9_in_system_init(w) && !g_j9_boot_returned) {
                return w;
            }
            if (!any) {
                any = w;
            }
        }
        for (int i = -20; i < 24; ++i) {
            const address at = jsp + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, at)) {
                continue;
            }
            const address w = j9_read32(pr, at);
            if (!j9_looks_bytecode_pc(pr, w) || !j9_is_invoke_byte(j9_read8(pr, w))) {
                continue;
            }
            if (j9_in_system_init(w) && !g_j9_boot_returned) {
                sys = w;
                break;
            }
            if (!any) {
                any = w;
            }
        }
        if (g_j9_boot_returned) {
            return any;
        }
        return sys ? sys : 0;
    }

    static void j9_rebind_teardown_jni(kernel::process *pr) {
        if (!pr || !g_j9_proplist_bkpt) {
            return;
        }
        int n = 0;
        for (address p = 0x00725000u; p < 0x0072C000u; p += 16u) {
            if (!j9_mapped32(pr, p + 12u)) {
                continue;
            }
            const address w0 = j9_read32(pr, p);
            const address send = j9_read32(pr, p + 8u);
            const address extra = j9_read32(pr, p + 12u);
            if ((send != 0x81911DFCu) || (extra != 1u) || !j9_ptr_is_jcl_rom(w0)) {
                continue;
            }
            char nbuf[96];
            nbuf[0] = 0;
            j9_rom_method_name(pr, w0, nbuf, sizeof(nbuf));
            const bool want = nbuf[0]
                && (std::strstr(nbuf, "getPropertyList")
                    || std::strstr(nbuf, "getEncoding")
                    || std::strstr(nbuf, "getHostname"));
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] rebind-jni meth=0x{:X} rom=0x{:X} name='{}' want={}",
                p, w0, nbuf[0] ? nbuf : "?", want ? 1 : 0);
            if (!want) {
                continue;
            }
            j9_write32(pr, p + 8u, g_j9_proplist_bkpt);
            ++n;
        }
        if (n) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] rebind-jni n={}", n);
        }
    }

    static address j9_alloc_java_obj(kernel::process *pr, const address clazz) {
        unsigned nbytes = 0x80u;
        if (clazz && j9_mapped32(pr, clazz + 0x38u)) {
            const unsigned inst = j9_read32(pr, clazz + 0x38u);
            if ((inst >= 4u) && (inst < 0x1000u)) {
                nbytes = inst + 0x10u;
            }
        }
        return j9_host_alloc_obj(pr, clazz, nbytes);
    }

    static address j9_ensure_converter_dummy(kernel::process *pr) {
        const address clazz = g_j9_converter_clazz
            ? g_j9_converter_clazz
            : g_j9_last_jcl_clazz;
        if (!pr || !clazz || !j9_mapped32(pr, clazz)) {
            return 0;
        }
        if (g_j9_converter_dummy && j9_mapped32(pr, g_j9_converter_dummy)
            && (j9_read32(pr, g_j9_converter_dummy) == clazz)) {
            return g_j9_converter_dummy;
        }
        g_j9_converter_dummy = j9_alloc_java_obj(pr, clazz);
        return g_j9_converter_dummy;
    }

    static address j9_stack_ref(const address obj) {
        if (!obj || (obj < 0x01000000u) || (obj >= 0x08000000u)) {
            return obj;
        }
        if ((obj & 3u) == 0) {
            return obj >> 2;
        }
        return obj;
    }

    static address j9_obj_from_slot(kernel::process *pr, const address v) {
        if (!pr || !v) {
            return 0;
        }
        if (j9_looks_heap(v) && ((v & 3u) == 0) && j9_mapped32(pr, v)
            && j9_mapped32(pr, v + 8u)) {
            return v;
        }
        const address sh = v << 2;
        if (j9_looks_heap(sh) && j9_mapped32(pr, sh) && j9_mapped32(pr, sh + 8u)) {
            return sh;
        }
        return 0;
    }

    static void j9_pack_frame_locals(kernel::process *pr, const address fp,
        const int nlocals) {
        if (!pr || !fp) {
            return;
        }
        for (int i = 0; i < nlocals; ++i) {
            const address sl = fp + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, sl)) {
                break;
            }
            const address v = j9_read32(pr, sl);
            if (!j9_looks_heap(v) || (v & 3u)) {
                continue;
            }
            if (j9_obj_from_slot(pr, v) == v) {
                j9_write32(pr, sl, v >> 2);
            }
        }
    }

    static void j9_seed_string_if_needed(kernel::process *pr, const address fp) {
        if (!pr || !fp || !g_j9_string_clazz || g_j9_string_filled) {
            return;
        }
        const address thiz = j9_obj_from_slot(pr, j9_read32(pr, fp));
        if (!thiz || (j9_read32(pr, thiz) != g_j9_string_clazz)) {
            return;
        }
        address val = j9_obj_from_slot(pr, j9_read32(pr, thiz + 8u));
        if (!val || !j9_is_char_array(pr, val)) {
            val = j9_obj_from_slot(pr, j9_read32(pr, thiz + 12u));
        }
        if (!val || !j9_is_char_array(pr, val)) {
            const address empty = j9_new_string_utf(pr, "");
            if (empty) {
                val = j9_obj_from_slot(pr, j9_read32(pr, empty + 8u));
                if (!val) {
                    val = j9_read32(pr, empty + 8u);
                }
                j9_write32(pr, thiz + 8u, val);
            }
        }
        j9_store_string_fields(pr, thiz, val, 0);
    }

    static unsigned j9_array_len(kernel::process *pr, const address arr) {
        if (!pr || !arr || !j9_mapped32(pr, arr + 12u)) {
            return 0;
        }
        unsigned n = j9_read32(pr, arr + 12u);
        if (n && (n <= 0x10000u)) {
            return n;
        }
        n = j9_read32(pr, arr + 8u);
        return (n <= 0x10000u) ? n : 0;
    }

    static bool j9_looks_jcl_interp_lr(const address lr) {
        const address bare = lr & ~1u;
        return (bare >= 0x81930000u) && (bare < 0x8194C000u);
    }

    static bool j9_looks_byte_array(kernel::process *pr, const address p) {
        if (!pr || !p || !j9_mapped32(pr, p + 16u)) {
            return false;
        }
        char nbuf[96];
        nbuf[0] = 0;
        j9_class_name(pr, j9_read32(pr, p), nbuf, sizeof(nbuf));
        return (nbuf[0] == '[') && (nbuf[1] == 'B') && (nbuf[2] == 0);
    }

    static void j9_save_string_ret(kernel::process *pr, const address fp) {
        if (!pr || (fp < 0x0071E000u) || (fp >= 0x00720000u) || (fp & 3u)
            || !j9_mapped32(pr, fp - 12u)) {
            return;
        }
        g_j9_str_ret_r4 = j9_read32(pr, fp - 12u);
        g_j9_str_ret_pc = j9_read32(pr, fp - 8u);
        g_j9_str_ret_fp = j9_read32(pr, fp - 4u);
        g_j9_str_ret_ok = true;
    }

    static void j9_dump_string_frame(kernel::process *pr, arm::core *core,
        const address fp, const char *tag) {
        if (!pr || !fp || !j9_mapped32(pr, fp + 20u)) {
            return;
        }
        static int dumps = 0;
        if (dumps >= 8) {
            return;
        }
        ++dumps;
        const address thiz = j9_obj_from_slot(pr, j9_read32(pr, fp));
        char l1n[48];
        l1n[0] = 0;
        const address l1 = j9_obj_from_slot(pr, j9_read32(pr, fp + 4u));
        if (l1) {
            j9_class_name(pr, j9_read32(pr, l1), l1n, sizeof(l1n));
        }
        address clr0 = 0;
        address clr1 = 0;
        address csp0 = core ? core->get_sp() : 0;
        if (core && pr && (csp0 >= 0x00400000u) && (csp0 < 0x00500000u)) {
            for (unsigned off = 0; off < 0x400u; off += 4u) {
                const address w = j9_read32(pr, csp0 + off);
                const address b = w & ~1u;
                if ((((b >= 0x81930000u) && (b < 0x8194C000u))
                        || ((b >= 0x818D8000u) && (b < 0x818EA000u)))
                    && (pr->get_ptr_on_addr_space(b) != nullptr)) {
                    if (!clr0) {
                        clr0 = w;
                    } else if (w != clr0) {
                        clr1 = w;
                        break;
                    }
                }
            }
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] string-frame {} fp=0x{:X} m10=0x{:X}/0x{:X}/0x{:X} "
            "loc=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} thiz=0x{:X} "
            "val=0x{:X}/0x{:X}/0x{:X} l1=0x{:X} l1n='{}' l1n8=0x{:X}/0x{:X} "
            "sp=0x{:X} jcllr=0x{:X}/0x{:X} ret=0x{:X}/0x{:X}/0x{:X}",
            tag ? tag : "?", fp,
            j9_read32(pr, fp - 12u), j9_read32(pr, fp - 8u),
            j9_read32(pr, fp - 4u),
            j9_read32(pr, fp), j9_read32(pr, fp + 4u),
            j9_read32(pr, fp + 8u), j9_read32(pr, fp + 12u),
            j9_read32(pr, fp + 16u), thiz,
            thiz ? j9_read32(pr, thiz + 8u) : 0,
            thiz ? j9_read32(pr, thiz + 12u) : 0,
            thiz ? j9_read32(pr, thiz + 16u) : 0,
            l1, l1n,
            l1 ? j9_read32(pr, l1 + 8u) : 0,
            l1 ? j9_read32(pr, l1 + 12u) : 0,
            csp0, clr0, clr1,
            g_j9_str_ret_r4, g_j9_str_ret_pc, g_j9_str_ret_fp);
    }

    static bool j9_lr_follows_blx(kernel::process *pr, const address lr) {
        if (!pr || !lr || !pr->get_ptr_on_addr_space(lr & ~1u)) {
            return false;
        }
        const address bare = lr & ~1u;
        if (bare == 0x81922C1Cu) {
            return false;
        }
        if ((bare >= 0x818ED000u) && (bare < 0x818F0000u)) {
            return false;
        }
        if (lr & 1u) {
            if (bare < 2u) {
                return false;
            }
            const address ins = bare - 2u;
            const unsigned h = static_cast<unsigned>(j9_read8(pr, ins))
                | (static_cast<unsigned>(j9_read8(pr, ins + 1u)) << 8);
            // Thumb BLX Rm: 0100 0111 1mmm 000
            return (h & 0xFF87u) == 0x4780u;
        }
        if (bare < 4u) {
            return false;
        }
        const address ins = bare - 4u;
        const address w = j9_read32(pr, ins);
        // ARM BLX Rm
        if ((w & 0x0FFFFFF0u) == 0x012FFF30u) {
            return true;
        }
        // ARM BL interpret() @ 0x818DE324 or run-method helper @ 0x818DFD00
        if ((w & 0x0F000000u) == 0x0B000000u) {
            std::int32_t imm24 = static_cast<std::int32_t>(w & 0x00FFFFFFu);
            if (imm24 & 0x00800000) {
                imm24 -= 0x01000000;
            }
            const address dest = (ins + 8u)
                + static_cast<address>(imm24 << 2);
            return (dest == 0x818DE324u) || (dest == 0x818DFD00u);
        }
        return false;
    }

    static bool j9_looks_interp_clr(kernel::process *pr, const address lr) {
        if (!pr) {
            return false;
        }
        const address bare = lr & ~1u;
        if (bare == 0x81922C1Cu) {
            return false;
        }
        if (j9_looks_bytecode_pc(pr, bare) || j9_is_initialize_pc(bare)
            || j9_is_interpret_tail_pc(bare)) {
            return false;
        }
        if ((bare >= 0x81930000u) && (bare < 0x81950000u)
            && (pr->get_ptr_on_addr_space(bare) != nullptr)) {
            return j9_lr_follows_blx(pr, lr);
        }
        if ((bare >= 0x818D8000u) && (bare < 0x818EA000u)
            && (pr->get_ptr_on_addr_space(bare) != nullptr)) {
            return j9_lr_follows_blx(pr, lr);
        }
        // j9.dll JNI Call*Method (`blx r5` @ 0x818BD18E → 0x818BD191)
        if ((bare >= 0x818BB000u) && (bare < 0x818C2000u)
            && (pr->get_ptr_on_addr_space(bare) != nullptr)) {
            return j9_lr_follows_blx(pr, lr);
        }
        return false;
    }

    static bool j9_looks_interp_cframe(kernel::process *pr, const address at) {
        if (!pr || (at < 0x00400000u) || (at >= 0x00500000u)
            || !j9_mapped32(pr, at + 32u)) {
            return false;
        }
        const address lr = j9_read32(pr, at + 32u);
        if (!j9_looks_interp_clr(pr, lr)) {
            return false;
        }
        const address r4 = j9_read32(pr, at);
        const address r8 = j9_read32(pr, at + 16u);
        const auto in_text = [](const address p) {
            const address b = p & ~1u;
            return (b >= 0x81800000u) && (b < 0x81A00000u);
        };
        // Jump tables of Thumb thunks sit on the C stack and match a
        // code-looking word at +32. interpret()'s STMFD {r4-r11,lr}
        // keeps data / vmthread in r4/r8.
        if (in_text(r4) && in_text(r8)) {
            return false;
        }
        if ((r8 == g_j9_vmthread) || (r8 == 0)
            || ((r8 >= 0x00700000u) && (r8 < 0x00720000u))) {
            return true;
        }
        if (!in_text(r4) && ((r4 >= 0x00400000u) && (r4 < 0x00800000u))) {
            return true;
        }
        return false;
    }

    static void j9_try_capture_live_cframe(kernel::process *pr, arm::core *core) {
        if (!g_j9_boot_returned || !pr || !core) {
            return;
        }
        const address sp0 = core->get_sp();
        if ((sp0 < 0x00400000u) || (sp0 >= 0x00500000u)) {
            return;
        }
        address best = 0;
        address best_jcl = 0;
        for (unsigned off = 0; off < 0x400u; off += 4u) {
            const address at = sp0 + off;
            if (!j9_mapped32(pr, at + 32u)) {
                break;
            }
            const address lr = j9_read32(pr, at + 32u);
            if (g_j9_boot_returned && g_j9_boot_csp && (at >= g_j9_boot_csp)
                && (lr == g_j9_boot_cframe[8])) {
                continue;
            }
            if (j9_looks_interp_cframe(pr, at) && j9_lr_follows_blx(pr, lr)) {
                if (!best) {
                    best = at;
                }
                if (!best_jcl && j9_looks_jcl_interp_lr(lr)) {
                    best_jcl = at;
                    break;
                }
            }
        }
        if (best_jcl) {
            best = best_jcl;
        } else if (!best) {
            return;
        }
        if (g_j9_live_cframe_ok && j9_looks_jcl_interp_lr(g_j9_live_cframe[8])
            && !best_jcl) {
            return;
        }
        if (g_j9_live_cframe_ok && (best >= g_j9_live_csp)) {
            return;
        }
        g_j9_live_csp = best;
        for (int i = 0; i < 16; ++i) {
            const address at = best + static_cast<address>(i) * 4u;
            g_j9_live_cframe[i] = j9_mapped32(pr, at) ? j9_read32(pr, at) : 0;
        }
        g_j9_live_cframe_ok = true;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] live-cframe csp=0x{:X} clr=0x{:X} "
            "w0=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            best, g_j9_live_cframe[8], g_j9_live_cframe[0],
            g_j9_live_cframe[4], g_j9_live_cframe[7], g_j9_live_cframe[8]);
    }

    static void j9_fill_string_from_locals(kernel::process *pr, const address fp) {
        if (!pr || !fp || !j9_mapped32(pr, fp + 16u) || !g_j9_string_clazz) {
            return;
        }
        const address thiz = j9_obj_from_slot(pr, j9_read32(pr, fp));
        if (!thiz || (j9_read32(pr, thiz) != g_j9_string_clazz)) {
            return;
        }
        address src = 0;
        address off = 0;
        address len = 0;
        for (int i = 1; i <= 4; ++i) {
            const address sl = fp + static_cast<address>(i) * 4u;
            if (!j9_mapped32(pr, sl + 8u)) {
                break;
            }
            const address cand = j9_obj_from_slot(pr, j9_read32(pr, sl));
            if (!cand) {
                continue;
            }
            if (j9_is_char_array(pr, cand)) {
                const unsigned n = j9_array_len(pr, cand);
                j9_store_string_fields(pr, thiz, cand, n);
                g_j9_string_filled = true;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] string-fill-reuse this=0x{:X} arr=0x{:X} n={}",
                    thiz, cand, n);
                return;
            }
            if (j9_looks_byte_array(pr, cand)) {
                src = cand;
                off = j9_read32(pr, sl + 4u);
                len = j9_read32(pr, sl + 8u);
                break;
            }
            if (!src && j9_mapped32(pr, cand + 16u)
                && j9_array_len(pr, cand)) {
                src = cand;
                off = j9_read32(pr, sl + 4u);
                len = j9_read32(pr, sl + 8u);
            }
        }
        if (!src && g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x6cu)) {
            const address pinned = j9_obj_from_slot(pr,
                j9_read32(pr, g_j9_vmthread + 0x6cu));
            if (pinned && (j9_looks_byte_array(pr, pinned)
                    || j9_array_len(pr, pinned))) {
                src = pinned;
                off = 0;
                len = j9_array_len(pr, pinned);
            }
        }
        if (!src) {
            const address extras[4] = { 0x2D137B8u, 0x2D10078u, 0x2D13768u,
                0x2D10474u };
            for (const address raw : extras) {
                const address cand = j9_obj_from_slot(pr, raw);
                if (cand && j9_looks_byte_array(pr, cand)) {
                    src = cand;
                    off = 0;
                    len = j9_array_len(pr, cand);
                    break;
                }
            }
        }
        if (!src || !j9_mapped32(pr, src + 16u)) {
            address val = j9_obj_from_slot(pr, j9_read32(pr, thiz + 8u));
            if (!val || !j9_is_char_array(pr, val)) {
                val = j9_obj_from_slot(pr, j9_read32(pr, thiz + 12u));
            }
            if (val && j9_is_char_array(pr, val)) {
                j9_store_string_fields(pr, thiz, val, j9_array_len(pr, val));
                g_j9_string_filled = true;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] string-fill-empty this=0x{:X} arr=0x{:X} n={}",
                    thiz, val, j9_array_len(pr, val));
                return;
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] string-fill-miss this=0x{:X} fp=0x{:X} "
                "l1=0x{:X} l2=0x{:X} l3=0x{:X} l4=0x{:X}",
                thiz, fp, j9_read32(pr, fp + 4u), j9_read32(pr, fp + 8u),
                j9_read32(pr, fp + 12u), j9_read32(pr, fp + 16u));
            return;
        }
        const unsigned nsrc = j9_array_len(pr, src);
        if (!nsrc || (off > nsrc)) {
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] string-fill-bad src=0x{:X} nsrc={} off={} "
                "s8=0x{:X} sC=0x{:X}",
                src, nsrc, static_cast<unsigned>(off),
                j9_read32(pr, src + 8u), j9_read32(pr, src + 12u));
            return;
        }
        unsigned n = static_cast<unsigned>(len);
        if (!n || ((off + n) > nsrc)) {
            n = static_cast<unsigned>(nsrc - off);
        }
        if (n > 0x10000u) {
            return;
        }
        address ac = g_j9_char_array_clazz;
        if (!ac) {
            ac = j9_steal_char_array_clazz(pr, g_j9_string_clazz);
            g_j9_char_array_clazz = ac;
        }
        if (!ac) {
            return;
        }
        const address arr = j9_host_alloc_obj(pr, ac, 16u + n * 2u);
        if (!arr) {
            return;
        }
        j9_write32(pr, arr + 8u, n);
        j9_write32(pr, arr + 12u, n);
        unsigned o = 0;
        unsigned i = 0;
        while ((i < n) && (o < n)) {
            const unsigned c = j9_read8(pr, src + 16u + off + i);
            ++i;
            std::uint16_t ch = static_cast<std::uint16_t>(c);
            if ((c >= 0xC0u) && (c < 0xE0u) && (i < n)) {
                const unsigned c2 = j9_read8(pr, src + 16u + off + i);
                ++i;
                ch = static_cast<std::uint16_t>(((c & 0x1Fu) << 6) | (c2 & 0x3Fu));
            } else if ((c >= 0xE0u) && (c < 0xF0u) && ((i + 1u) < n)) {
                const unsigned c2 = j9_read8(pr, src + 16u + off + i);
                const unsigned c3 = j9_read8(pr, src + 16u + off + i + 1u);
                i += 2u;
                ch = static_cast<std::uint16_t>(((c & 0x0Fu) << 12)
                    | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu));
            }
            j9_write16(pr, arr + 16u + o * 2u, ch);
            ++o;
        }
        j9_write32(pr, arr + 8u, o);
        j9_write32(pr, arr + 12u, o);
        j9_store_string_fields(pr, thiz, arr, o);
        g_j9_string_filled = true;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] string-fill this=0x{:X} src=0x{:X} off={} n={} out={}",
            thiz, src, static_cast<unsigned>(off),
            static_cast<unsigned>(len), o);
    }

    static bool j9_interp_csp_from_vt(kernel::process *pr, const address vt,
        address *csp, address *lr) {
        if (!pr || !vt || !csp || !lr || !j9_mapped32(pr, vt + 0x114u)) {
            return false;
        }
        const address link = j9_read32(pr, vt + 0x114u);
        if ((link < 0x00400080u) || (link >= 0x00500000u)
            || !j9_mapped32(pr, link)) {
            return false;
        }
        const address at = link - 0x80u;
        if ((at < 0x00400000u) || !j9_mapped32(pr, at + 32u)) {
            return false;
        }
        *csp = at;
        *lr = j9_read32(pr, at + 32u);
        const address bare = (*lr) & ~1u;
        if ((bare < 0x81800000u) || (bare >= 0x81A00000u)
            || !pr->get_ptr_on_addr_space(bare)
            || !j9_lr_follows_blx(pr, *lr)) {
            return false;
        }
        if (g_j9_consumed_csp && (at == g_j9_consumed_csp)) {
            return false;
        }
        return true;
    }

    static bool j9_return_to_interp_c(arm::core *core, kernel::process *pr,
        const address ret0, const address vt, const bool keep_saved_r8) {
        if (!core || !pr) {
            return false;
        }
        if (g_j9_vmthread && g_j9_vt10_c && j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
            j9_write32(pr, g_j9_vmthread + 0x10u, g_j9_vt10_c);
            if (g_j9_vt14_c) {
                j9_write32(pr, g_j9_vmthread + 0x14u, g_j9_vt14_c);
            }
            if (g_j9_vt18_c) {
                j9_write32(pr, g_j9_vmthread + 0x18u, g_j9_vt18_c);
            }
        }
        address csp = 0;
        address frame[16] = {};
        const auto lr_ok = [&](const address lr) {
            return j9_lr_follows_blx(pr, lr);
        };
        address vt_csp = 0;
        address vt_lr = 0;
        if (j9_interp_csp_from_vt(pr, vt ? vt : g_j9_vmthread, &vt_csp, &vt_lr)
            && ((vt_csp != g_j9_boot_csp) || (vt_lr != g_j9_boot_cframe[8]))) {
            csp = vt_csp;
            for (int i = 0; i < 16; ++i) {
                const address w = csp + static_cast<address>(i) * 4u;
                frame[i] = j9_mapped32(pr, w) ? j9_read32(pr, w) : 0;
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] interp-vt-frame csp=0x{:X} lr=0x{:X} r8=0x{:X}",
                csp, vt_lr, frame[4]);
        }
        if (!csp && g_j9_live_cframe_ok && (g_j9_live_csp >= 0x00400000u)
            && (g_j9_live_csp < 0x00500000u)
            && !(g_j9_boot_returned && g_j9_boot_csp && (g_j9_live_csp >= g_j9_boot_csp))
            && lr_ok(g_j9_live_cframe[8])) {
            const address sr4 = g_j9_live_cframe[0];
            const address sr8 = g_j9_live_cframe[4];
            const bool r4_code = ((sr4 & ~1u) >= 0x81800000u)
                && ((sr4 & ~1u) < 0x81A00000u);
            const bool r8_code = ((sr8 & ~1u) >= 0x81800000u)
                && ((sr8 & ~1u) < 0x81A00000u);
            if (!r4_code || !r8_code) {
                csp = g_j9_live_csp;
                for (int i = 0; i < 16; ++i) {
                    frame[i] = g_j9_live_cframe[i];
                }
            }
        }
        if (!csp) {
            const address sp0 = core->get_sp();
            address best = 0;
            for (unsigned off = 0; off < 0x400u; off += 4u) {
                const address at = sp0 + off;
                if (!j9_mapped32(pr, at + 32u)) {
                    break;
                }
                const address lr = j9_read32(pr, at + 32u);
                if (g_j9_boot_returned && g_j9_boot_csp && (at >= g_j9_boot_csp)
                    && (lr == g_j9_boot_cframe[8])) {
                    continue;
                }
                if (j9_looks_interp_cframe(pr, at) && lr_ok(lr)) {
                    best = at;
                    break;
                }
            }
            if (!best || (best < 0x00400000u) || (best >= 0x00500000u)) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] interp-c-miss sp=0x{:X} live={} csp=0x{:X} clr=0x{:X}",
                    core->get_sp(), g_j9_live_cframe_ok ? 1 : 0, g_j9_live_csp,
                    g_j9_live_cframe_ok ? g_j9_live_cframe[8] : 0);
                return false;
            }
            csp = best;
            for (int i = 0; i < 16; ++i) {
                const address w = best + static_cast<address>(i) * 4u;
                frame[i] = j9_mapped32(pr, w) ? j9_read32(pr, w) : 0;
            }
        }
        for (int i = 0; i < 9; ++i) {
            j9_write32(pr, csp + static_cast<address>(i) * 4u, frame[i]);
        }
        if (vt && j9_mapped32(pr, vt + 0x6cu)) {
            j9_write32(pr, vt + 0x64u, 0);
            j9_write32(pr, vt + 0x6cu, 0);
            if (ret0 && j9_mapped32(pr, vt + 0x68u)) {
                j9_write32(pr, vt + 0x68u, ret0);
            }
            const address link = csp + 0x80u;
            if (j9_mapped32(pr, link) && j9_mapped32(pr, vt + 0x114u)) {
                j9_write32(pr, vt + 0x114u, j9_read32(pr, link));
            }
        }
        // interpret() returns the Java result in r0. <init> is void (0);
        // a String factory returns the object. r8 stays the vmthread.
        core->set_reg(0, ret0);
        if (vt) {
            core->set_reg(8, vt);
        }
        // 0x818DFD00's caller (Class.initialize send) saves r8=0xFFFFFFF8
        // (`mvn r8, #7`) as an alignment mask. Overwriting it with the
        // vmthread makes send treat dummy/String as a class/PC.
        if (vt && !keep_saved_r8) {
            frame[4] = vt;
            j9_write32(pr, csp + 16u, vt);
        }
        core->set_sp(csp);
        g_j9_consumed_csp = csp;
        g_j9_live_cframe_ok = false;
        g_j9_resume_at = 0;
        g_j9_resume_no_ac = false;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] interp-ret-c this=0x{:X} csp=0x{:X} lr=0x{:X} "
            "vt=0x{:X} w0=0x{:X}",
            ret0, csp, frame[8], vt, frame[0]);
        ++g_j9_throw_skips;
        j9_set_pc(core, 0x818DE50Cu);
        return true;
    }

    static address j9_find_jni_call_frame(kernel::process *pr, arm::core *core) {
        if (!pr || !core) {
            return 0;
        }
        const address sp0 = core->get_sp();
        if ((sp0 < 0x00400000u) || (sp0 >= 0x00500000u)) {
            return 0;
        }
        for (unsigned off = 0; off < 0x400u; off += 4u) {
            const address at = sp0 + off;
            if (!j9_mapped32(pr, at + 32u)) {
                break;
            }
            if (j9_read32(pr, at + 32u) == 0x818BD191u) {
                return at;
            }
        }
        return 0;
    }

    static unsigned j9_read_cstr(kernel::process *pr, const address p, char *buf,
        const unsigned cap) {
        if (!buf || !cap) {
            return 0;
        }
        buf[0] = 0;
        if (!pr || !p) {
            return 0;
        }
        const char *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(p));
        if (!s) {
            return 0;
        }
        unsigned n = 0;
        while ((n + 1u < cap) && s[n] && (static_cast<unsigned char>(s[n]) >= 0x09)) {
            buf[n] = s[n];
            ++n;
        }
        buf[n] = 0;
        return n;
    }

    static bool j9_looks_string_obj(kernel::process *pr, const address obj) {
        if (!pr || !obj || !j9_mapped32(pr, obj + 20u)) {
            return false;
        }
        if (g_j9_string_clazz && (j9_read32(pr, obj) == g_j9_string_clazz)) {
            return true;
        }
        char nbuf[96];
        nbuf[0] = 0;
        const address clazz = j9_read32(pr, obj);
        if (clazz && j9_mapped32(pr, clazz + 0x38u)) {
            j9_class_name(pr, clazz, nbuf, sizeof(nbuf));
        }
        return nbuf[0] && (std::strcmp(nbuf, "java/lang/String") == 0);
    }

    static void j9_fill_string_from_cstr(kernel::process *pr, const address str,
        const char *utf) {
        if (!pr || !str || !utf || !j9_mapped32(pr, str + 20u)) {
            return;
        }
        const unsigned n = static_cast<unsigned>(std::strlen(utf));
        address ac = g_j9_char_array_clazz;
        if (!ac || !j9_mapped32(pr, ac)) {
            const address clazz = g_j9_string_clazz ? g_j9_string_clazz
                : j9_read32(pr, str);
            ac = j9_steal_char_array_clazz(pr, clazz);
            g_j9_char_array_clazz = ac;
        }
        if (!ac) {
            return;
        }
        const address arr = j9_host_alloc_obj(pr, ac, 16u + (n ? n : 1u) * 2u);
        if (!arr) {
            return;
        }
        j9_write32(pr, arr + 8u, n);
        j9_write32(pr, arr + 12u, n);
        for (unsigned i = 0; i < n; ++i) {
            j9_write16(pr, arr + 16u + i * 2u,
                static_cast<std::uint16_t>(static_cast<unsigned char>(utf[i])));
        }
        j9_store_string_fields(pr, str, arr, n);
    }

    static address j9_copy_cstr_buf(kernel::process *pr, const char *utf) {
        if (!pr || !utf) {
            return 0;
        }
        const unsigned n = static_cast<unsigned>(std::strlen(utf));
        const address buf = j9_host_alloc_obj(pr, 0, n + 8u);
        if (!buf) {
            return 0;
        }
        for (unsigned i = 0; i < n; ++i) {
            j9_write8(pr, buf + i, static_cast<std::uint8_t>(utf[i]));
        }
        j9_write8(pr, buf + n, 0);
        return buf;
    }

    static address j9_string_to_utf8_buf(kernel::process *pr, address str) {
        if (!pr || !str || !j9_mapped32(pr, str)) {
            return 0;
        }
        if (!j9_looks_string_obj(pr, str)) {
            const address inner = j9_read32(pr, str);
            if (j9_looks_string_obj(pr, inner)) {
                str = inner;
            } else {
                return 0;
            }
        }
        address arr = j9_obj_from_slot(pr, j9_read32(pr, str + 12u));
        unsigned off = j9_read32(pr, str + 16u);
        unsigned n = j9_read32(pr, str + 20u);
        if (!arr || !j9_is_char_array(pr, arr)) {
            arr = j9_obj_from_slot(pr, j9_read32(pr, str + 8u));
            off = j9_read32(pr, str + 12u);
            n = j9_read32(pr, str + 16u);
        }
        if (!arr || !j9_mapped32(pr, arr + 16u)) {
            return 0;
        }
        const unsigned alen = j9_array_len(pr, arr);
        if (off > alen) {
            off = 0;
        }
        if (!n || ((off + n) > alen)) {
            n = alen - off;
        }
        if (n > 0x1000u) {
            n = 0x1000u;
        }
        const address buf = j9_host_alloc_obj(pr, 0, n + 8u);
        if (!buf) {
            return 0;
        }
        for (unsigned i = 0; i < n; ++i) {
            const std::uint8_t lo = j9_read8(pr, arr + 16u + (off + i) * 2u);
            j9_write8(pr, buf + i, lo);
        }
        j9_write8(pr, buf + n, 0);
        return buf;
    }

    static void j9_save_interp_vt(kernel::process *pr) {
        if (!pr || !g_j9_vmthread || !j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
            return;
        }
        const address w10 = j9_read32(pr, g_j9_vmthread + 0x10u);
        const address w14 = j9_read32(pr, g_j9_vmthread + 0x14u);
        const address w18 = j9_read32(pr, g_j9_vmthread + 0x18u);
        // interpret() stores a class/method pointer at +0x18. ROM FindClass
        // then overwrites that with a small stack depth — keep the pointer.
        if ((w18 > 0x1000u) || !g_j9_vt18_c) {
            g_j9_vt10_c = w10;
            g_j9_vt14_c = w14;
            g_j9_vt18_c = w18;
        }
    }

    static void j9_restore_interp_vt(kernel::process *pr) {
        if (!pr || !g_j9_vmthread || !j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
            return;
        }
        if (g_j9_vt10_c) {
            j9_write32(pr, g_j9_vmthread + 0x10u, g_j9_vt10_c);
        }
        if (g_j9_vt14_c) {
            j9_write32(pr, g_j9_vmthread + 0x14u, g_j9_vt14_c);
        }
        if (g_j9_vt18_c) {
            j9_write32(pr, g_j9_vmthread + 0x18u, g_j9_vt18_c);
        }
    }

    // JNI NewLocalRef does r5 = vt[0x10] + vt[0x18]. interpret() parks a
    // class pointer at +0x18; keep the live Java SP and a zero depth.
    static void j9_prepare_jni_vt(kernel::process *pr) {
        if (!pr || !g_j9_vmthread || !j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
            return;
        }
        j9_save_interp_vt(pr);
        if (g_j9_vt10_c) {
            j9_write32(pr, g_j9_vmthread + 0x10u, g_j9_vt10_c);
        }
        j9_write32(pr, g_j9_vmthread + 0x18u, 0);
    }

    // ROM FindClass adds vt+0x18 onto vt+0x10 as a Java-stack depth.
    // interpret() stores the current class/method pointer at +0x18, so
    // zero the depth and keep a real Java SP (or 0 → javaVM+0x250).
    static void j9_prepare_findclass_vt(kernel::process *pr) {
        if (!pr || !g_j9_vmthread || !j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
            return;
        }
        j9_save_interp_vt(pr);
        address scratch = 0;
        if (g_j9_official_heap && j9_mapped32(pr, g_j9_official_heap + 0x80u)) {
            scratch = g_j9_official_heap + 0x80u;
        } else if (g_j9_walk_va) {
            scratch = g_j9_walk_va + 0x2F0u;
        }
        if (scratch) {
            j9_write32(pr, scratch, 0);
        }
        // [vt+0x10]+[vt+0x18] must be a 0 word so ROM uses javaVM+0x250.
        // The live Java SP often holds a String/method pointer; using it
        // makes FindClass treat that object as the current J9Class.
        j9_write32(pr, g_j9_vmthread + 0x10u, scratch);
        j9_write32(pr, g_j9_vmthread + 0x18u, 0);
    }

    static bool j9_return_to_jni_call(arm::core *core, kernel::process *pr,
        const address ret0) {
        if (!core || !pr) {
            return false;
        }
        const address at = j9_find_jni_call_frame(pr, core);
        if (!at) {
            return false;
        }
        const address saved0 = j9_read32(pr, at);
        const address saved4 = j9_read32(pr, at + 16u);
        const address link = j9_mapped32(pr, at + 0x80u)
            ? j9_read32(pr, at + 0x80u) : 0;
        const address vtl = (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x114u))
            ? j9_read32(pr, g_j9_vmthread + 0x114u) : 0;
        const bool interp_like = (vtl == (at + 0x80u))
            || ((link >= 0x00400000u) && (link < 0x00500000u)
                && (link == vtl));
        const auto jni_lr = [](const address lr) {
            const address b = lr & ~1u;
            return (b == 0x818BD2A6u) || (b == 0x818BD446u)
                || (b == 0x818BD2A4u) || (b == 0x818BD444u);
        };
        address jni_sp = 0;
        for (const address inner : { 0x40u, 0x90u }) {
            const address cand = at + inner;
            if (!j9_mapped32(pr, cand + 0x2Cu)) {
                continue;
            }
            if (jni_lr(j9_read32(pr, cand + 0x2Cu))) {
                jni_sp = cand;
                break;
            }
        }
        if (!jni_sp) {
            const address sp0 = core->get_sp();
            for (unsigned off = 0; off < 0x400u; off += 4u) {
                const address p = sp0 + off;
                if (!j9_mapped32(pr, p + 0x2Cu)) {
                    break;
                }
                if (jni_lr(j9_read32(pr, p + 0x2Cu))) {
                    jni_sp = p;
                    break;
                }
            }
        }
        const address outp = jni_sp && j9_mapped32(pr, jni_sp + 0x34u)
            ? j9_read32(pr, jni_sp + 0x34u) : 0;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] jni-leave at=0x{:X} ret=0x{:X} w0=0x{:X} w4=0x{:X} "
            "link=0x{:X} vt114=0x{:X} interp={} jsp=0x{:X} jlr=0x{:X} "
            "out=0x{:X} w=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            at, ret0, saved0, saved4, link, vtl, interp_like ? 1 : 0,
            jni_sp, jni_sp ? j9_read32(pr, jni_sp + 0x2Cu) : 0, outp,
            j9_read32(pr, at), j9_read32(pr, at + 4u),
            j9_read32(pr, at + 8u), j9_read32(pr, at + 12u),
            j9_read32(pr, at + 16u), j9_read32(pr, at + 20u),
            j9_read32(pr, at + 24u), j9_read32(pr, at + 28u));
        if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x64u)) {
            j9_write32(pr, g_j9_vmthread + 0x64u, 0);
            j9_write32(pr, g_j9_vmthread + 0x6cu, 0);
            if (ret0 && j9_mapped32(pr, g_j9_vmthread + 0x68u)) {
                j9_write32(pr, g_j9_vmthread + 0x68u, ret0);
            }
        }
        if (jni_sp) {
            const address outer_sp = jni_sp + 0x30u;
            char path[160];
            path[0] = 0;
            const address pathp = j9_mapped32(pr, outer_sp + 0x14u)
                ? j9_read32(pr, outer_sp + 0x14u) : 0;
            const address rawp = j9_mapped32(pr, outer_sp + 0x38u)
                ? j9_read32(pr, outer_sp + 0x38u) : 0;
            unsigned pn = j9_read_cstr(pr, pathp, path, sizeof(path));
            if (!pn) {
                pn = j9_read_cstr(pr, rawp, path, sizeof(path));
            }
            if (ret0 && j9_mapped32(pr, ret0 + 20u)) {
                if (pn) {
                    j9_fill_string_from_cstr(pr, ret0, path);
                    g_j9_utf_stash = j9_copy_cstr_buf(pr, path);
                } else {
                    address arr = j9_obj_from_slot(pr, j9_read32(pr, ret0 + 8u));
                    if (!arr || !j9_is_char_array(pr, arr)) {
                        arr = j9_obj_from_slot(pr, j9_read32(pr, ret0 + 12u));
                    }
                    j9_store_string_fields(pr, ret0, arr, 0);
                }
            }
            if (outp && j9_mapped32(pr, outp)) {
                j9_write32(pr, outp, ret0);
            }
            if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x18u)) {
                j9_prepare_jni_vt(pr);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] vt-saved 0x{:X}/0x{:X}/0x{:X}",
                    g_j9_vt10_c, g_j9_vt14_c, g_j9_vt18_c);
            }
            const address env = j9_mapped32(pr, jni_sp + 0xCu)
                ? j9_read32(pr, jni_sp + 0xCu) : 0;
            const address real_table = (env && j9_mapped32(pr, env))
                ? j9_read32(pr, env) : 0;
            address used_table = real_table;
            if (real_table && j9_mapped32(pr, real_table + 169u * 4u)
                && g_j9_getstrutf_bkpt) {
                if (g_j9_jnienv_table && (g_j9_jnienv_table != real_table)
                    && j9_mapped32(pr, g_j9_jnienv_table + 169u * 4u)) {
                    for (unsigned i = 0; i < 247u; ++i) {
                        j9_write32(pr, g_j9_jnienv_table + i * 4u,
                            j9_read32(pr, real_table + i * 4u));
                    }
                    used_table = g_j9_jnienv_table;
                }
                const address walk = g_j9_getstrutf_bkpt
                    ? (g_j9_getstrutf_bkpt - 0x2D8u) : 0;
                const address zero_fn = walk ? ((walk + 0x280u) | 1u) : 0;
                const address rel_fn = walk ? ((walk + 0x284u) | 1u) : 0;
                if (zero_fn) {
                    j9_write32(pr, used_table + 15u * 4u, zero_fn);
                    j9_write32(pr, used_table + 164u * 4u, zero_fn);
                    j9_write32(pr, used_table + 168u * 4u, zero_fn);
                }
                if (rel_fn) {
                    j9_write32(pr, used_table + 17u * 4u, rel_fn);
                    j9_write32(pr, used_table + 166u * 4u, rel_fn);
                    j9_write32(pr, used_table + 170u * 4u, rel_fn);
                }
                if (zero_fn) {
                    j9_write32(pr, used_table + 23u * 4u, zero_fn);
                }
                if (g_j9_newstr_bkpt) {
                    j9_write32(pr, used_table + 167u * 4u, g_j9_newstr_bkpt);
                }
                j9_write32(pr, used_table + 169u * 4u, g_j9_getstrutf_bkpt);
                if (g_j9_findclass_bkpt) {
                    j9_write32(pr, used_table + 6u * 4u, g_j9_findclass_bkpt);
                }
                if (g_j9_getmethod_bkpt) {
                    j9_write32(pr, used_table + 33u * 4u, g_j9_getmethod_bkpt);
                    j9_write32(pr, used_table + 113u * 4u, g_j9_getmethod_bkpt);
                }
                if (g_j9_newobjarr_bkpt) {
                    j9_write32(pr, used_table + 172u * 4u, g_j9_newobjarr_bkpt);
                }
                if (g_j9_newglobal_bkpt) {
                    j9_write32(pr, used_table + 21u * 4u, g_j9_newglobal_bkpt);
                }
                if (g_j9_callstatic_bkpt) {
                    j9_write32(pr, used_table + 141u * 4u, g_j9_callstatic_bkpt);
                    j9_write32(pr, used_table + 116u * 4u, g_j9_callstatic_bkpt);
                }
                if (used_table != real_table) {
                    j9_write32(pr, env, used_table);
                }
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] jni-fill path='{}' @0x{:X}/0x{:X} str=0x{:X} stash=0x{:X} "
                "env=0x{:X} tbl=0x{:X}/0x{:X} fc=0x{:X} utf=0x{:X}",
                path, pathp, rawp, ret0, g_j9_utf_stash, env, real_table, used_table,
                (used_table && j9_mapped32(pr, used_table + 24u))
                    ? j9_read32(pr, used_table + 24u) : 0,
                (used_table && j9_mapped32(pr, used_table + 169u * 4u))
                    ? j9_read32(pr, used_table + 169u * 4u) : 0);
            // Resume the inner j9.dll Call*Method wrapper. Do not rewrite
            // [sp,#0x14]: that slot is the malloc'd path the wrapper frees
            // at 0x818BD2B0 (overwriting it caused USER 42).
            core->set_reg(0, 0);
            core->set_sp(jni_sp);
            g_j9_consumed_csp = jni_sp;
            g_j9_live_cframe_ok = false;
            g_j9_resume_at = 0;
            g_j9_resume_no_ac = false;
            ++g_j9_throw_skips;
            j9_set_pc(core, 0x818BCFC5u);
            return true;
        }
        return false;
    }

    static bool j9_finish_class_init_c(arm::core *core, kernel::process *pr) {
        if (!core || !pr || g_j9_init_c_returned) {
            return false;
        }
        const address vt = g_j9_vmthread;
        address csp = 0;
        address lr = 0;
        if (!j9_interp_csp_from_vt(pr, vt, &csp, &lr) || (lr != 0x818F92C0u)) {
            return false;
        }
        const auto mark = [&](const address clazz) {
            if (clazz && j9_mapped32(pr, clazz + 0x28u)) {
                j9_write32(pr, clazz + 0x28u, 1);
            }
        };
        mark(g_j9_converter_clazz);
        mark(0x729540u);
        g_j9_init_c_returned = true;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] skip-lazy-init-ret clazz=0x{:X} csp=0x{:X} lr=0x{:X}",
            g_j9_converter_clazz, csp, lr);
        return j9_return_to_interp_c(core, pr, 0, vt, true);
    }

    static address j9_skip_invoke_putfield(kernel::process *pr, address pc) {
        if (!pr || !pc) {
            return pc;
        }
        if ((j9_read8(pr, pc) == 0x3Au) && (j9_read8(pr, pc + 1u) == 0x04u)) {
            pc += 2u;
        }
        for (int n = 0; n < 10; ++n) {
            const std::uint8_t op = j9_read8(pr, pc);
            if ((op == 0x2Au) || (op == 0x2Bu) || (op == 0x2Cu) || (op == 0x2Du)
                || ((op >= 0x1Au) && (op <= 0x1Du))) {
                pc += 1u;
                continue;
            }
            if ((op == 0x19u) || (op == 0x15u)) {
                pc += 2u;
                continue;
            }
            if ((op == 0xB6u) || (op == 0xB5u)) {
                pc += 3u;
                if (op == 0xB5u) {
                    break;
                }
                continue;
            }
            break;
        }
        // String ctor tail: aload_0; [D7]; getfield value; arraylength;
        // putfield count; goto. Official getfield resolve AVs/NPEs.
        if (j9_read8(pr, pc) == 0x2Au) {
            pc += 1u;
        }
        if (j9_read8(pr, pc) == 0xD7u) {
            pc += 1u;
        }
        if (j9_read8(pr, pc) == 0xB4u) {
            pc += 3u;
        }
        if (j9_read8(pr, pc) == 0xBEu) {
            pc += 1u;
        }
        if (j9_read8(pr, pc) == 0xB5u) {
            pc += 3u;
        }
        if (j9_read8(pr, pc) == 0xA7u) {
            const std::int16_t off = static_cast<std::int16_t>(
                j9_read8(pr, pc + 1u)
                | (static_cast<unsigned>(j9_read8(pr, pc + 2u)) << 8));
            pc = static_cast<address>(static_cast<std::int32_t>(pc) + off);
        }
        return pc;
    }

    static bool j9_is_java_fp(const address p) {
        return (p >= 0x0071E000u) && (p < 0x00720000u) && ((p & 3u) == 0);
    }

    // Walk the operand stack for {class, invoke-pc, locals} belonging to
    // the method that called `fp`. Skip the frame that returns *into*
    // `fp` (Util → A7F5) and interpret/clinit sentinels.
    static bool j9_find_caller_frame(kernel::process *pr, const address fp,
        const address skip_pc, address *or4, address *opc, address *ofp) {
        if (!pr || !j9_is_java_fp(fp) || !or4 || !opc || !ofp) {
            return false;
        }
        // Official invoke pushes {r4,r5,r6} immediately below locals.
        // Do not scan sideways: leftover JCL bytes + heap words produce
        // false B7/AD hits that panic at 0x81911090.
        const address r4 = j9_read32(pr, fp - 12u);
        const address pc = j9_read32(pr, fp - 8u);
        const address cfp = j9_read32(pr, fp - 4u);
        if (!j9_looks_bytecode_pc(pr, pc) || !j9_is_invoke_op(j9_read8(pr, pc))) {
            return false;
        }
        if ((pc == skip_pc) || (pc == 0x8195A7F5u) || (pc == 0x8195ACE0u)
            || (pc == 0x81961C71u) || j9_is_initialize_pc(pc)
            || j9_is_interpret_tail_pc(pc)
            || j9_is_stale_interp_caller(r4, pc, cfp)) {
            return false;
        }
        if (!j9_is_java_fp(cfp) || (cfp <= fp)) {
            return false;
        }
        if ((r4 < 0x00720000u) || (r4 >= 0x00730000u)) {
            return false;
        }
        *or4 = r4;
        *opc = pc;
        *ofp = cfp;
        return true;
    }

    static bool j9_scan_java_caller_above(kernel::process *pr, const address fp,
        const address skip_pc, address *or4, address *opc, address *ofp) {
        if (!pr || !j9_is_java_fp(fp) || !or4 || !opc || !ofp) {
            return false;
        }
        for (address at = fp + 4u; at + 8u < 0x0071E5F0u; at += 4u) {
            if (!j9_mapped32(pr, at + 8u)) {
                break;
            }
            const address r4 = j9_read32(pr, at);
            const address pc = j9_read32(pr, at + 4u);
            const address cfp = j9_read32(pr, at + 8u);
            if (!j9_looks_bytecode_pc(pr, pc) || !j9_is_invoke_op(j9_read8(pr, pc))) {
                continue;
            }
            if ((pc == skip_pc) || (pc == 0x8195A7F5u) || (pc == 0x8195ACE0u)
                || (pc == 0x81961C71u) || j9_is_initialize_pc(pc)
                || j9_is_interpret_tail_pc(pc)
                || j9_is_stale_interp_caller(r4, pc, cfp)) {
                continue;
            }
            if (!j9_is_java_fp(cfp) || (cfp <= fp)) {
                continue;
            }
            if ((r4 < 0x00720000u) || (r4 >= 0x00730000u)) {
                continue;
            }
            *or4 = r4;
            *opc = pc;
            *ofp = cfp;
            return true;
        }
        return false;
    }

    static void j9_dump_cstack_frames(kernel::process *pr, arm::core *core,
        const char *tag) {
        if (!pr || !core) {
            return;
        }
        static int dumps = 0;
        if (dumps >= 4) {
            return;
        }
        ++dumps;
        const address sp0 = core->get_sp();
        address hit0 = 0;
        address hit1 = 0;
        address hit2 = 0;
        address vt0 = 0;
        address vt1 = 0;
        if ((sp0 >= 0x00400000u) && (sp0 < 0x00500000u)) {
            for (unsigned off = 0; off < 0x280u; off += 4u) {
                const address at = sp0 + off;
                if (!j9_mapped32(pr, at + 32u)) {
                    break;
                }
                const address lr = j9_read32(pr, at + 32u);
                const address r8 = j9_read32(pr, at + 16u);
                if ((r8 == g_j9_vmthread) && !vt0) {
                    vt0 = at;
                    vt1 = lr;
                }
                if (!j9_lr_follows_blx(pr, lr)) {
                    continue;
                }
                if (!hit0) {
                    hit0 = at;
                } else if (!hit1) {
                    hit1 = at;
                } else {
                    hit2 = at;
                    break;
                }
            }
        }
        const auto lr_at = [&](const address at) {
            return (at && j9_mapped32(pr, at + 32u)) ? j9_read32(pr, at + 32u) : 0;
        };
        const address vtl = (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x114u))
            ? j9_read32(pr, g_j9_vmthread + 0x114u) : 0;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] cstack {} sp=0x{:X} bootcsp=0x{:X} "
            "sp20=0x{:X} boot20=0x{:X} a4b0=0x{:X} a618=0x{:X} "
            "vt114=0x{:X} vt=0x{:X}/0x{:X} "
            "h0=0x{:X}/0x{:X} h1=0x{:X}/0x{:X} h2=0x{:X}/0x{:X}",
            tag ? tag : "?", sp0, g_j9_boot_csp,
            lr_at(sp0), lr_at(g_j9_boot_csp), lr_at(0x43F4B0u), lr_at(0x43F618u),
            vtl, vt0, vt1,
            hit0, hit0 ? j9_read32(pr, hit0 + 32u) : 0,
            hit1, hit1 ? j9_read32(pr, hit1 + 32u) : 0,
            hit2, hit2 ? j9_read32(pr, hit2 + 32u) : 0);
    }

    static bool j9_leave_string_ctor(arm::core *core, kernel::process *pr,
        const address fp, const address thiz, const address meth) {
        if (!core || !pr) {
            return false;
        }
        address r4 = g_j9_str_ret_ok ? g_j9_str_ret_r4
            : (fp ? j9_read32(pr, fp - 12u) : 0);
        address pc = g_j9_str_ret_ok ? g_j9_str_ret_pc
            : (fp ? j9_read32(pr, fp - 8u) : 0);
        address cfp = g_j9_str_ret_ok ? g_j9_str_ret_fp
            : (fp ? j9_read32(pr, fp - 4u) : 0);
        if (!j9_looks_bytecode_pc(pr, pc) || !j9_is_invoke_op(j9_read8(pr, pc))
            || !j9_is_java_fp(cfp) || (fp && (cfp <= fp))) {
            address sr4 = 0;
            address spc = 0;
            address sfp = 0;
            if (j9_scan_java_caller_above(pr, fp, 0, &sr4, &spc, &sfp)) {
                r4 = sr4;
                pc = spc;
                cfp = sfp;
            }
        }
        if (j9_looks_bytecode_pc(pr, pc) && j9_is_invoke_op(j9_read8(pr, pc))
            && !j9_is_initialize_pc(pc) && !j9_is_interpret_tail_pc(pc)
            && !j9_is_stale_interp_caller(r4, pc, cfp)
            && j9_is_java_fp(cfp) && (!fp || (cfp > fp))) {
            const std::uint8_t op = j9_read8(pr, pc);
            address osp = (cfp >= 4u) ? (cfp - 4u) : cfp;
            if ((op != 0xB7u) && thiz && osp && j9_mapped32(pr, osp)) {
                j9_write32(pr, osp, thiz);
            }
            g_j9_saved_r4 = r4;
            g_j9_saved_r5 = pc;
            g_j9_saved_r6 = cfp;
            g_j9_java_sp = osp;
            g_j9_resume_at = pc + 3u;
            g_j9_resume_no_ac = true;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] str-leave-java -> 0x{:X} r4=0x{:X} r6=0x{:X} "
                "inv=0x{:02X} this=0x{:X}",
                g_j9_resume_at, r4, cfp, op, thiz);
            ++g_j9_throw_skips;
            j9_jxe_resume_interp(core, pr, meth, 0u);
            g_j9_resume_no_ac = false;
            g_j9_resume_at = 0;
            return true;
        }
        if (((pc & ~3u) == 0x81922C1Cu) && j9_is_java_fp(cfp)) {
            address osp = (cfp >= 4u) ? (cfp - 4u) : cfp;
            if (thiz && osp && j9_mapped32(pr, osp)) {
                j9_write32(pr, osp, thiz);
            }
            g_j9_saved_r4 = r4;
            g_j9_saved_r5 = 0x81922C1Du;
            g_j9_saved_r6 = cfp;
            g_j9_java_sp = osp;
            g_j9_resume_at = 0x81922C1Du;
            g_j9_resume_no_ac = true;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] str-leave-jni r4=0x{:X} r6=0x{:X} this=0x{:X}",
                r4, cfp, thiz);
            ++g_j9_throw_skips;
            j9_jxe_resume_interp(core, pr, meth, 0u);
            g_j9_resume_no_ac = false;
            g_j9_resume_at = 0;
            return true;
        }
        const address vmth = g_j9_vmthread ? g_j9_vmthread : 0;
        j9_dump_cstack_frames(pr, core, "leave");
        j9_try_capture_live_cframe(pr, core);
        address icsp = 0;
        address ilr = 0;
        if (j9_interp_csp_from_vt(pr, vmth, &icsp, &ilr) && (ilr == 0x818F92C0u)) {
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] str-leave-skip-init this=0x{:X} csp=0x{:X}",
                thiz, icsp);
            return false;
        }
        if (j9_return_to_interp_c(core, pr, thiz, vmth, false)) {
            return true;
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] str-leave-miss this=0x{:X} fp=0x{:X} ret=0x{:X}/0x{:X}/0x{:X}",
            thiz, fp, r4, pc, cfp);
        return false;
    }

    static address j9_skip_simple_ops(kernel::process *pr, address pc) {
        if (!pr || !pc) {
            return 0;
        }
        for (int n = 0; n < 8; ++n) {
            const std::uint8_t op = j9_read8(pr, pc);
            if (j9_is_cp_op(op) || ((op >= 0xACu) && (op <= 0xB1u))) {
                return pc;
            }
            if ((op == 0x00u) || (op == 0x57u) || (op == 0x58u) || (op == 0x59u)
                || ((op >= 0x1Au) && (op <= 0x2Du))
                || ((op >= 0x4Bu) && (op <= 0x4Eu))) {
                pc += 1u;
                continue;
            }
            if ((op == 0xC6u) || (op == 0xC7u) || ((op >= 0x99u) && (op <= 0xA4u))) {
                pc += 3u;
                continue;
            }
            break;
        }
        return pc;
    }

    static void j9_mark_jcl_supers(kernel::process *pr, const address clazz) {
        (void)pr;
        (void)clazz;
    }

    static void j9_plant_main_thread(kernel::process *pr, const address clazz) {
        if (!pr || !clazz) {
            return;
        }
        const address vt = g_j9_vmthread;
        address obj = g_j9_thread_obj;
        if (!obj || (j9_read32(pr, obj) != clazz)) {
            obj = j9_alloc_java_obj(pr, clazz);
            g_j9_thread_obj = obj;
        }
        if (!obj) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] thread-plant alloc failed clazz=0x{:X} vt=0x{:X}",
                clazz, vt);
            return;
        }
        if (vt && j9_mapped32(pr, vt + k_j9_thread_obj_off)) {
            j9_write32(pr, vt + k_j9_thread_obj_off, obj);
            const unsigned inst = j9_read32(pr, clazz + 0x38u);
            const unsigned nbytes = (inst < 0x1000u) ? (inst + 0xcu) : 0x10u;
            if (nbytes >= 0x10u) {
                j9_write32(pr, obj + 0xcu, vt);
            }
            if (nbytes >= 0x14u) {
                j9_write32(pr, obj + 0x10u, vt);
            }
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] thread-plant obj=0x{:X} clazz=0x{:X} vt=0x{:X} slot=0x{:X}",
            obj, clazz, vt, vt ? j9_read32(pr, vt + k_j9_thread_obj_off) : 0);
    }

    static bool j9_find_invoke_frame(kernel::process *pr, const address *exclude,
        const int nexcl, address *or4, address *opc, address *or6) {
        if (!pr) {
            return false;
        }
        address best = 0;
        address br4 = 0;
        address br6 = 0;
        int best_score = -1;
        const auto excluded = [&](const address pc) {
            for (int i = 0; i < nexcl; ++i) {
                if (exclude[i] && (pc == exclude[i])) {
                    return true;
                }
            }
            return (pc == g_j9_jcl_r5) || (pc == g_j9_jcl_outer_r5)
                || (pc == g_j9_last_jxe_r5);
        };
        const auto consider = [&](const address fr4, const address fpc,
            const address fr6) {
            if (!fpc || excluded(fpc) || !j9_is_invoke_op(j9_read8(pr, fpc))) {
                return;
            }
            const bool jxe = (fpc >= 0x00770000u) && (fpc < 0x00780000u);
            const bool jcl = (fpc >= 0x81940000u) && (fpc < 0x81980000u);
            if (!jxe && !jcl) {
                return;
            }
            if ((fr6 < 0x00710000u) || (fr6 >= 0x00720000u)) {
                return;
            }
            const std::uint8_t after = j9_read8(pr, fpc + 3u);
            // `invoke; putfield/putstatic` is a callee site, not the caller.
            if ((after == 0xB3u) || (after == 0xB5u)) {
                return;
            }
            int score = jxe ? 55 : 30;
            if ((fr4 >= 0x00720000u) && (fr4 < 0x00730000u)) {
                score += 12;
            } else if (fr4) {
                score -= 8;
            }
            if (j9_is_return_op(after)) {
                score += 24;
            }
            if (after == 0xC0u) {
                score += 16;
            }
            if (g_j9_last_jxe_r6) {
                const int d = std::abs(static_cast<int>(fr6)
                    - static_cast<int>(g_j9_last_jxe_r6));
                if (d < 0x80) {
                    score += 20 - (d / 8);
                }
            }
            if ((fpc >= 0x81968000u) && (fpc < 0x8196A000u)) {
                score += 6;
            }
            if (score > best_score) {
                best_score = score;
                best = fpc;
                br4 = fr4;
                br6 = fr6;
            }
        };
        const auto consider_at = [&](const address ot) {
            if (!j9_mapped32(pr, ot + 8u)) {
                return;
            }
            const address w0 = j9_read32(pr, ot);
            const address w1 = j9_read32(pr, ot + 4u);
            const address w2 = j9_read32(pr, ot + 8u);
            consider(w0, w1, w2);
            consider(w1, w0, w2);
            consider(w0, w2, w1);
            if (j9_is_invoke_op(j9_read8(pr, w1))
                && (w2 >= 0x00710000u) && (w2 < 0x00720000u)) {
                consider(0, w1, w2);
            }
        };
        const address hints[5] = {
            g_j9_last_jxe_r7, g_j9_last_jxe_r6, g_j9_jcl_outer_r6, g_j9_jcl_r6,
            g_j9_last_jxe_r6 ? (g_j9_last_jxe_r6 - 32u) : 0
        };
        for (const address hint : hints) {
            if (!hint) {
                continue;
            }
            for (int k = -40; k < 96; ++k) {
                consider_at(hint + static_cast<address>(k) * 4u);
            }
        }
        if (!best) {
            return false;
        }
        if ((!br4 || (br4 < 0x00720000u) || (br4 >= 0x00730000u))) {
            const address self = (g_j9_last_jxe_r6 && j9_mapped32(pr, g_j9_last_jxe_r6))
                ? j9_read32(pr, g_j9_last_jxe_r6) : 0;
            br4 = j9_obj_clazz(pr, self);
            if (!br4) {
                br4 = g_j9_jcl_outer_r4 ? g_j9_jcl_outer_r4
                    : (g_j9_jcl_r4 ? g_j9_jcl_r4 : g_j9_last_jxe_r4);
            }
        }
        if (or4) {
            *or4 = br4;
        }
        if (opc) {
            *opc = best;
        }
        if (or6) {
            *or6 = br6;
        }
        return true;
    }

    static bool j9_finish_jcl_to_jxe(arm::core *core, kernel::process *pr) {
        if (!core || !pr || g_j9_alps_started || !j9_scan_jxe_parent(pr)) {
            return false;
        }
        address pr4 = g_j9_last_jxe_r4;
        address pr5 = g_j9_last_jxe_r5;
        address pr6 = g_j9_last_jxe_r6;
        address retval = 0;
        const address tail = g_j9_jcl_outer_r5 ? (g_j9_jcl_outer_r5 + 3u) : 0;
        if (tail && (j9_read8(pr, tail) == 0x57u)
            && (j9_read8(pr, tail + 1u) == 0x19u) && g_j9_jcl_outer_r6) {
            const std::uint8_t idx = j9_read8(pr, tail + 2u);
            retval = j9_read32(pr, g_j9_jcl_outer_r6 + static_cast<address>(idx) * 4u);
        }
        if (!retval && g_j9_jcl_outer_r6) {
            retval = j9_read32(pr, g_j9_jcl_outer_r6 + 20u);
        }
        if (!retval) {
            retval = g_j9_last_java_obj ? g_j9_last_java_obj
                : (g_j9_jcl_this ? g_j9_jcl_this : g_j9_graphics_obj);
        }
        address resume = pr5 + 3u;
        auto pick_heap = [&](const address p) {
            return (j9_looks_heap(p) && ((p & 3u) == 0)) ? p : 0;
        };
        // JXE tail after the JCL factory helper is `putstatic; return`.
        // Official putstatic treats a leftover JCL PC as a class and AVs
        // at 0x8190D1B8. Walk out of the ctor / Display.getDisplay tail.
        if ((j9_read8(pr, resume) == 0xB3u)
            && j9_is_return_op(j9_read8(pr, resume + 3u))) {
            address self = pick_heap(pr6 ? j9_read32(pr, pr6) : 0);
            if (!self) {
                self = pick_heap(g_j9_jcl_this);
            }
            if (!self) {
                self = pick_heap(g_j9_graphics_obj);
            }
            address excl[6] = { pr5, g_j9_jcl_r5, g_j9_jcl_outer_r5, 0, 0, 0 };
            int nexcl = 3;
            address cr4 = 0;
            address cr5 = 0;
            address cr6 = 0;
            for (int hop = 0; hop < 3; ++hop) {
                if (!j9_find_invoke_frame(pr, excl, nexcl, &cr4, &cr5, &cr6)
                    || !cr5) {
                    break;
                }
                pr4 = cr4 ? cr4 : pr4;
                pr5 = cr5;
                pr6 = cr6 ? cr6 : pr6;
                resume = cr5 + 3u;
                if (nexcl < 6) {
                    excl[nexcl++] = cr5;
                }
                const std::uint8_t nop = j9_read8(pr, resume);
                // Factory `invokespecial; return` or getDisplay
                // `checkcast; dup; astore_1; return`.
                const bool factory_ret = j9_is_return_op(nop);
                const bool getdisplay_ret = (nop == 0xC0u)
                    && (j9_read8(pr, resume + 3u) == 0x59u)
                    && j9_is_return_op(j9_read8(pr, resume + 5u));
                if (!factory_ret && !getdisplay_ret) {
                    break;
                }
                // Parent of a one-invoke wrapper sits in its locals.
                for (int k = 0; k < 8; ++k) {
                    const address at = pr6 + static_cast<address>(k) * 4u;
                    const address w = j9_read32(pr, at);
                    bool seen = false;
                    for (int e = 0; e < nexcl; ++e) {
                        if (excl[e] == w) {
                            seen = true;
                            break;
                        }
                    }
                    if (!w || seen || (w == g_j9_last_jxe_r5)
                        || !j9_is_invoke_op(j9_read8(pr, w))) {
                        continue;
                    }
                    const std::uint8_t after = j9_read8(pr, w + 3u);
                    if ((after == 0xB3u) || (after == 0xB5u)) {
                        continue;
                    }
                    const address loc = j9_read32(pr, at + 4u);
                    if ((loc < 0x00710000u) || (loc >= 0x00720000u)) {
                        continue;
                    }
                    if (nexcl < 6) {
                        excl[nexcl++] = w;
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-hop pc=0x{:X} r6=0x{:X} from=0x{:X}",
                        w, loc, pr5);
                    pr5 = w;
                    pr6 = loc;
                    resume = w + 3u;
                    break;
                }
            }
            if (self) {
                retval = self;
            }
            // Never dispatch official areturn / checkcast — frames are a
            // slot off and JCL CP resolve AVs on type=JXE.
            auto is_getdisplay_tail = [&](const address pc) {
                return (j9_read8(pr, pc) == 0xC0u)
                    && (j9_read8(pr, pc + 3u) == 0x59u)
                    && j9_is_return_op(j9_read8(pr, pc + 5u));
            };
            address visited[8] = { pr5, g_j9_last_jxe_r5, g_j9_jcl_r5,
                g_j9_jcl_outer_r5, 0, 0, 0, 0 };
            int nvis = 4;
            int hops_left = 3;
            while (hops_left-- > 0 && pr6
                && (j9_is_return_op(j9_read8(pr, resume))
                    || is_getdisplay_tail(resume))) {
                const bool want_jxe = is_getdisplay_tail(resume);
                address hop_pc = 0;
                address hop_r6 = 0;
                int hop_score = -1;
                for (int k = -12; k < 24; ++k) {
                    const address w = j9_read32(pr, pr6 + static_cast<address>(k) * 4u);
                    if (!w || !j9_is_invoke_op(j9_read8(pr, w))) {
                        continue;
                    }
                    bool seen = false;
                    for (int e = 0; e < nvis; ++e) {
                        if (visited[e] == w) {
                            seen = true;
                            break;
                        }
                    }
                    if (seen) {
                        continue;
                    }
                    const std::uint8_t after = j9_read8(pr, w + 3u);
                    if ((after == 0xB3u) || (after == 0xB5u)) {
                        continue;
                    }
                    const address loc = j9_read32(pr,
                        pr6 + static_cast<address>(k + 1) * 4u);
                    if ((loc < 0x00710000u) || (loc >= 0x00720000u)) {
                        continue;
                    }
                    const bool jxe = (w >= 0x00770000u) && (w < 0x00780000u);
                    if (want_jxe && !jxe) {
                        continue;
                    }
                    int sc = jxe ? 50 : 20;
                    if (sc > hop_score) {
                        hop_score = sc;
                        hop_pc = w;
                        hop_r6 = loc;
                    }
                }
                if (!hop_pc && want_jxe) {
                    address cr4 = 0;
                    address cr5 = 0;
                    address cr6 = 0;
                    if (g_j9_prev_jxe_r5 && (g_j9_prev_jxe_r5 != g_j9_last_jxe_r5)
                        && j9_is_invoke_op(j9_read8(pr, g_j9_prev_jxe_r5))) {
                        hop_pc = g_j9_prev_jxe_r5;
                        hop_r6 = g_j9_prev_jxe_r6;
                        pr4 = g_j9_prev_jxe_r4 ? g_j9_prev_jxe_r4 : pr4;
                    } else if (j9_find_jxe_caller(g_j9_last_jxe_r5, &cr4, &cr5, &cr6)
                        && cr5 && (cr5 != g_j9_last_jxe_r5)
                        && j9_is_invoke_op(j9_read8(pr, cr5))) {
                        hop_pc = cr5;
                        hop_r6 = cr6;
                        pr4 = cr4 ? cr4 : pr4;
                    }
                }
                if (!hop_pc && want_jxe) {
                    address hits[8] = {};
                    int nh = 0;
                    for (int k = -80; k < 80 && nh < 8; ++k) {
                        const address w = j9_read32(pr,
                            pr6 + static_cast<address>(k) * 4u);
                        if ((w >= 0x00770000u) && (w < 0x00780000u)) {
                            hits[nh++] = w;
                        }
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-scan r6=0x{:X} w=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} jxe=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                        pr6,
                        j9_read32(pr, pr6 - 16u), j9_read32(pr, pr6 - 12u),
                        j9_read32(pr, pr6 - 8u), j9_read32(pr, pr6 - 4u),
                        j9_read32(pr, pr6), j9_read32(pr, pr6 + 4u),
                        j9_read32(pr, pr6 + 8u), j9_read32(pr, pr6 + 12u),
                        hits[0], hits[1], hits[2], hits[3]);
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-jxe bc7709=0x{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                        j9_read32(pr, 0x007709A0u), j9_read32(pr, 0x007709A8u),
                        j9_read32(pr, 0x007709B0u), j9_read32(pr, 0x00770B00u),
                        j9_read32(pr, 0x00770B08u), j9_read32(pr, 0x00770B0Cu));
                    address wide[8] = {};
                    int nw = 0;
                    for (address a = 0x0071E000u; a < 0x0071E800u && nw < 8; a += 4u) {
                        const address w = j9_read32(pr, a);
                        if ((w >= 0x00770000u) && (w < 0x00778000u)
                            && (w != 0x00770B08u)) {
                            wide[nw++] = w;
                        }
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-wide jxe=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} vt=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                        wide[0], wide[1], wide[2], wide[3], wide[4], wide[5],
                        j9_read32(pr, 0x00714E00u + 0x14u),
                        j9_read32(pr, 0x00714E00u + 0x18u),
                        j9_read32(pr, 0x00714E00u + 0x1Cu),
                        j9_read32(pr, 0x00714E00u + 0x20u));
                    const address bases[3] = { pr6, g_j9_last_jxe_r6, g_j9_last_jxe_r7 };
                    for (const address base : bases) {
                        if (!base) {
                            continue;
                        }
                        for (int k = -48; k < 64; ++k) {
                            const address w = j9_read32(pr,
                                base + static_cast<address>(k) * 4u);
                            if ((w < 0x00770000u) || (w >= 0x00780000u)
                                || (w == g_j9_last_jxe_r5)
                                || !j9_is_invoke_op(j9_read8(pr, w))) {
                                continue;
                            }
                            bool seen = false;
                            for (int e = 0; e < nvis; ++e) {
                                if (visited[e] == w) {
                                    seen = true;
                                    break;
                                }
                            }
                            if (seen) {
                                continue;
                            }
                            const address loc = j9_read32(pr,
                                base + static_cast<address>(k + 1) * 4u);
                            address use_loc = loc;
                            if ((use_loc < 0x00710000u) || (use_loc >= 0x00720000u)) {
                                use_loc = g_j9_last_jxe_r6 ? g_j9_last_jxe_r6 : pr6;
                            }
                            hop_pc = w;
                            hop_r6 = use_loc;
                            break;
                        }
                        if (hop_pc) {
                            break;
                        }
                    }
                }
                if (!hop_pc) {
                    if (is_getdisplay_tail(resume)) {
                        const address midlet = pick_heap(j9_read32(pr, pr6))
                            ? j9_read32(pr, pr6)
                            : (pick_heap(g_j9_midlet_this) ? g_j9_midlet_this
                                : pick_heap(j9_read32(pr, 0x0071E534u)));
                        const address display = pick_heap(retval)
                            ? retval
                            : (pick_heap(g_j9_jcl_this) ? g_j9_jcl_this
                                : pick_heap(g_j9_graphics_obj));
                        if (pick_heap(midlet)) {
                            g_j9_midlet_this = midlet;
                        }
                        if (pick_heap(display)) {
                            g_j9_display_obj = display;
                        }
                        if (pick_heap(midlet) && pick_heap(display)
                            && j9_mapped32(pr, midlet + 0x20u)) {
                            for (int i = 2; i < 10; ++i) {
                                const address at = midlet
                                    + static_cast<address>(i) * 4u;
                                if (j9_mapped32(pr, at)
                                    && !j9_read32(pr, at)) {
                                    j9_write32(pr, at, display);
                                }
                            }
                        }
                        const address putd = j9_find_alps_putdisplay(pr);
                        if (putd && pick_heap(midlet) && pick_heap(display)) {
                            pr4 = g_j9_last_jxe_r4 ? g_j9_last_jxe_r4 : pr4;
                            // Locals: this. Stack: this, display for putfield.
                            pr6 = g_j9_last_jxe_r6 ? g_j9_last_jxe_r6 : pr6;
                            if (pr6 && j9_mapped32(pr, pr6 + 8u)) {
                                j9_write32(pr, pr6, midlet);
                                j9_write32(pr, pr6 + 4u, midlet);
                                j9_write32(pr, pr6 + 8u, display);
                            }
                            pr5 = putd;
                            resume = putd;
                            retval = display;
                            g_j9_java_sp = pr6 + 8u;
                            g_j9_alps_started = true;
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] ac-jcl-alps putd=0x{:X} mid=0x{:X} disp=0x{:X} r4=0x{:X} r6=0x{:X}",
                                putd, midlet, display, pr4, pr6);
                            break;
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] ac-jcl-alps miss mid=0x{:X} disp=0x{:X} putd=0x{:X}",
                            midlet, display, putd);
                        resume += 6u;
                    }
                    break;
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] ac-jcl-hop2 pc=0x{:X} r6=0x{:X} from=0x{:X}",
                    hop_pc, hop_r6, pr5);
                if (nvis < 8) {
                    visited[nvis++] = hop_pc;
                }
                pr5 = hop_pc;
                pr6 = hop_r6;
                resume = hop_pc + 3u;
            }
        }
        if (!pick_heap(retval)) {
            retval = pick_heap(g_j9_jcl_this);
        }
        if (!pick_heap(retval)) {
            retval = pick_heap(g_j9_graphics_obj);
        }
        if (!pick_heap(retval)) {
            retval = pick_heap(g_j9_last_java_obj);
        }
        address sp = g_j9_java_sp ? g_j9_java_sp : (pr6 + 4u);
        if (!g_j9_alps_started) {
            sp = pr6 + 4u;
            if (sp && j9_mapped32(pr, sp)) {
                j9_write32(pr, sp, retval);
            }
        }
        g_j9_jcl_returned = true;
        j9_plant_post_jcl_r4fix(pr);
        g_j9_saved_r4 = pr4;
        g_j9_saved_r5 = pr5;
        g_j9_saved_r6 = pr6;
        g_j9_java_sp = sp;
        g_j9_resume_no_ac = true;
        g_j9_resume_at = resume;
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] ac-jcl-to-jxe r4=0x{:X} from=0x{:X} -> 0x{:X} r6=0x{:X} ret=0x{:X} loc0=0x{:X} prev=0x{:X}/0x{:X} stk=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
            pr4, g_j9_jcl_outer_r5, resume, pr6, retval,
            (g_j9_last_jxe_r6 && j9_mapped32(pr, g_j9_last_jxe_r6))
                ? j9_read32(pr, g_j9_last_jxe_r6) : 0,
            g_j9_prev_jxe_r4, g_j9_prev_jxe_r5,
            j9_read32(pr, pr6), j9_read32(pr, pr6 + 4u),
            j9_read32(pr, pr6 + 8u), j9_read32(pr, pr6 + 12u));
        j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
        g_j9_resume_no_ac = false;
        g_j9_resume_at = 0;
        return true;
    }

    static void j9_marshal_java_args(arm::core *core, kernel::process *pr, std::uint32_t *mw) {
        if (!core || !pr) {
            return;
        }
        address sp = core->get_reg(7);
        const address r8 = core->get_reg(8);
        const address sp_vm = j9_read32(pr, r8 + 0x10);
        if (!j9_mapped32(pr, sp) && j9_mapped32(pr, sp_vm)) {
            sp = sp_vm;
        }
        address raws[8] = {};
        for (int i = 0; i < 8; ++i) {
            raws[i] = j9_read32(pr, sp + static_cast<address>(i) * 4u);
        }
        unsigned argc = 1;
        if (mw && mw[0]) {
            if (const auto *hb = reinterpret_cast<const std::uint8_t *>(
                    pr->get_ptr_on_addr_space(mw[0] - 3u))) {
                if ((hb[0] >= 1u) && (hb[0] <= 3u)) {
                    argc = hb[0];
                }
            }
        }
        address this_slot = sp + (argc - 1u) * 4u;
        address arg_slot = (argc >= 2u) ? (sp + (argc - 2u) * 4u) : 0;
        address cands[10] = {};
        int nc = 0;
        const auto push_cand = [&](const address raw) {
            const address p = j9_pack_obj(pr, raw);
            if (!p || (nc >= 10)) {
                return;
            }
            for (int i = 0; i < nc; ++i) {
                if (cands[i] == p) {
                    return;
                }
            }
            cands[nc++] = p;
        };
        push_cand(j9_read32(pr, this_slot));
        if (arg_slot) {
            push_cand(j9_read32(pr, arg_slot));
        }
        push_cand(core->get_reg(1));
        push_cand(core->get_reg(2));
        push_cand(core->get_reg(3));
        push_cand(core->get_reg(5));
        for (int i = 0; i < 8; ++i) {
            push_cand(raws[i]);
        }
        if (g_j9_toolkit_obj) {
            push_cand(g_j9_toolkit_obj);
        }
        if (g_j9_canvas_obj) {
            push_cand(g_j9_canvas_obj);
        }
        address r2 = 0;
        address r3 = 0;
        for (int i = 0; i < nc; ++i) {
            if (j9_obj_has_peer(pr, cands[i])) {
                r2 = cands[i];
                break;
            }
        }
        if (!r2 && nc) {
            r2 = cands[0];
        }
        for (int i = 0; i < nc; ++i) {
            if (cands[i] != r2) {
                r3 = cands[i];
                break;
            }
        }
        // Leave r0 as J9Method* — jni_entry's epilogue at 0x8193A140
        // does ldr extra, [r0, #0xc]. The adapter pushes r0 then loads
        // the fake JNIEnv itself.
        core->set_reg(1, this_slot);
        core->set_reg(2, r2);
        core->set_reg(3, r3);
        static int logs = 0;
        if (logs < 20) {
            ++logs;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] jni-args sp=0x{:X} argc={} [0]=0x{:X} [4]=0x{:X} [8]=0x{:X} [c]=0x{:X} [10]=0x{:X} [14]=0x{:X} this=0x{:X} -> r2=0x{:X} r3=0x{:X}",
                sp, argc, raws[0], raws[1], raws[2], raws[3], raws[4], raws[5],
                this_slot, r2, r3);
        }
    }

    static address j9_adapter_for(const address fn) {
        if (!g_j9_walk_va || !g_j9_walk_ch || !g_j9_walk_ch->host_base()) {
            return 0;
        }
        const address bare = fn & ~1u;
        const address ad0 = g_j9_walk_va + k_j9_adapt_off;
        if ((bare >= ad0) && (bare < ad0 + 0x3000u) && ((bare & 3u) == 0)) {
            return bare;
        }
        const address stub0 = g_j9_walk_va + k_j9_stub_off;
        if ((bare >= stub0) && (bare < stub0 + 0x4000u)) {
            const unsigned n = static_cast<unsigned>((bare - stub0) / k_j9_stub_size);
            const address ad = g_j9_walk_va + k_j9_adapt_off + n * k_j9_adapt_size;
            if (ad + k_j9_adapt_size <= g_j9_walk_va + 0x8000u) {
                return ad;
            }
            return 0;
        }
        if (!g_j9_walk_pairs) {
            return 0;
        }
        auto *base = reinterpret_cast<std::uint8_t *>(g_j9_walk_ch->host_base());
        const std::uint32_t pairs_off = g_j9_walk_pairs - g_j9_walk_va;
        if (pairs_off < 4) {
            return 0;
        }
        auto *p = reinterpret_cast<std::uint32_t *>(base + pairs_off);
        const std::uint32_t n = p[-1];
        for (std::uint32_t i = 0; (i < n) && (i < 256u); ++i) {
            const address slot = p[i * 2 + 1];
            if ((slot == fn) || ((slot & ~1u) == bare)) {
                const address ad = g_j9_walk_va + k_j9_adapt_off + i * k_j9_adapt_size;
                if (ad + k_j9_adapt_size <= g_j9_walk_va + 0x8000u) {
                    return ad;
                }
            }
            const auto *realp = reinterpret_cast<const std::uint32_t *>(
                base + k_j9_stub_off + i * k_j9_stub_size + 16);
            if (realp && ((*realp == fn) || (*realp == bare) || (*realp == (bare | 1u)))) {
                return g_j9_walk_va + k_j9_adapt_off + i * k_j9_adapt_size;
            }
        }
        return 0;
    }

    static address j9_generic_jni_send(kernel::process *pr, arm::core *core) {
        if (!pr || !core) {
            return 0;
        }
        const address r8 = core->get_reg(8);
        const address vm = j9_read32(pr, r8 + 4);
        const address env = j9_read32(pr, vm + 0x278);
        const address inner = j9_read32(pr, env + 4);
        const address sl = j9_read32(pr, inner + 0x14);
        const address send = j9_read32(pr, sl + 0x30);
        if (j9_fn_is_code(send) && (send != k_j9_jni_entry) && (send != k_j9_inl_dispatch)
            && (send >= 0x10000u)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] generic interp JNI send=0x{:X}", send);
            }
            return send;
        }
        return 0;
    }

    static void j9_plant_method(std::uint32_t *mw, const address fn, const address method = 0) {
        if (!mw || !j9_fn_is_code(fn)) {
            return;
        }
        if (const address ad = j9_adapter_for(fn)) {
            g_j9_last_adapt = ad;
            g_j9_last_thumb = fn;
            address real = fn;
            if (g_j9_walk_ch && g_j9_walk_ch->host_base()
                && (ad >= (g_j9_walk_va + k_j9_adapt_off))) {
                auto *base = reinterpret_cast<std::uint8_t *>(g_j9_walk_ch->host_base());
                const auto *aw = reinterpret_cast<const std::uint32_t *>(
                    base + (ad - g_j9_walk_va));
                if (aw[18] && ((aw[18] & ~1u) >= 0x81A00000u)) {
                    real = aw[18];
                }
            }
            const address bare = real & ~1u;
            // Only LCDUI/nokialcdui need packed-object marshalling.
            // Other midp natives (markTime) keep official jni_entry.
            if ((bare >= 0x81AE0000u) && (bare < 0x81B20000u)) {
                j9_remember_adapter(method, ad);
            }
            mw[2] = k_j9_jni_entry;
            mw[3] = 1u;
            g_j9_last_thumb = fn;
        } else {
            mw[2] = fn;
            mw[3] = fn;
        }
    }

    static void plant_j9_invoke_tramp(kernel::process *pr, kernel_system *kern) {
        if (!pr || !kern) {
            return;
        }
        constexpr address k_invoke[] = {
            0x818DE498u, 0x818DE7A8u, 0x818DEB14u, 0x818DED30u, 0x818DEF60u,
            0x818DF0A0u, 0x818DF2B4u, 0x818DF420u, 0x818DF628u, 0x818DF7E8u,
            0x818DF958u, 0x818DFAD4u, 0x818DFCFCu, 0x818DFE38u, 0x818DFFA4u,
            0x818F5A78u, 0x818F74ECu,
            0x81910C6Cu, 0x81910DDCu, 0x81910F0Cu, 0x81910F44u, 0x81910F8Cu,
            0x8193A0A8u, 0x8193A13Cu, 0x8193A20Cu, 0x8193A2A0u,
            0x8193A370u, 0x8193A404u, 0x8193A4D8u, 0x8193A56Cu, 0x8193E460u
        };
        address tramp_va = 0;
        for (address a = 0x81920000u; a + 16u < 0x81928000u; a += 4) {
            auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(a));
            if (!p) {
                continue;
            }
            const bool old_tramp = (p[0] == 0xE590C008u)
                && ((p[1] == 0xE35C0102u) || (p[1] == 0xE1200070u));
            const bool new_tramp = (p[0] == 0xE1200070u) && (p[1] == 0xE590C008u);
            if (old_tramp || new_tramp
                || ((p[0] == 0) && (p[1] == 0) && (p[2] == 0) && (p[3] == 0))) {
                // BKPT first so every invoke snapshots J9VMThread / bytecode
                // PC before jni_entry clobbers r8. C++ then forwards
                // send>=0x80000000 (INL / jni_entry / LCDUI) the same way
                // the old `cmp; movhs pc,r12` path did.
                p[0] = 0xE1200070u;
                p[1] = 0xE590C008u;
                p[2] = 0xE35C0102u;
                p[3] = 0x21A0F00Cu;
                tramp_va = a;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(a, 16);
                }
                break;
            }
        }
        g_j9_invoke_n = 0;
        int n = 0;
        for (address site : k_invoke) {
            auto *slot = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(site));
            if (!slot) {
                continue;
            }
            const std::uint32_t orig = *slot;
            const bool ldr = (orig == 0xE590F008u) || (orig == 0x1590F008u);
            const bool already_b = ((orig & 0xFF000000u) == 0xEA000000u);
            const bool already_bk = (orig == 0xE1200070u);
            if (!ldr && !already_b && !already_bk) {
                continue;
            }
            const bool jni_post = (site >= 0x8193A000u) && (site < 0x8193A800u);
            if (jni_post) {
                *slot = 0xE1200070u;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(site, 4);
                }
                if (g_j9_invoke_n < 48) {
                    g_j9_invoke_sites[g_j9_invoke_n++] = site;
                }
            } else if (tramp_va && (ldr || already_b)) {
                const std::int32_t disp = static_cast<std::int32_t>(tramp_va - (site + 8));
                *slot = 0xEA000000u
                    | ((static_cast<std::uint32_t>(disp) >> 2) & 0x00FFFFFFu);
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(site, 4);
                }
            } else if (ldr) {
                *slot = 0xE1200070u;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(site, 4);
                }
            }
            ++n;
        }
        g_j9_tramp_va = tramp_va;
        if (tramp_va && (g_j9_invoke_n < 120)) {
            g_j9_invoke_sites[g_j9_invoke_n++] = tramp_va;
        }
        // LCDUI Thumb entries are often invoked with send=ROM (tramp
        // forwards). Plant BKPT on the first insn of each _create and
        // the shared New/Execute helper at 0x81A61CC4.
        for (address site : { 0x81AF1616u, 0x81AF2AACu, 0x81AEE7F8u,
                 0x81A61CC4u, 0x81AF17F0u, 0x81AF106Au,
                 0x81A61F5Cu, 0x81A61F6Eu, 0x81AF2C12u,
                 0x819171F0u, 0x818FCA82u, k_j9_alloc_memory, k_j9_alloc_object, k_j9_alloc_indexable }) {
            if (auto *th = reinterpret_cast<std::uint16_t *>(pr->get_ptr_on_addr_space(site))) {
                const std::uint16_t orig = *th;
                if ((orig == 0xB51Cu) || (orig == 0xB5FFu) || (orig == 0xB5F0u)
                    || (orig == 0xB5F8u) || (orig == 0xB40Fu) || (orig == 0xB530u)
                    || (orig == 0xB570u) || (orig == 0xBE00u)
                    || (orig == 0x4800u + 0x41u) || (orig == 0x4800u + 0x3Du)
                    || ((orig & 0xF800u) == 0x4800u)
                    || (site == k_j9_alloc_memory)
                    || (site == k_j9_alloc_object)
                    || (site == k_j9_alloc_indexable)
                    || (site == 0x818FCA82u)) {
                if (site == 0x818FCA82u) {
                    g_j9_throw_orig = orig;
                    g_j9_throw_bkpt = site;
                }
                *th = 0xBE00u;
                if (arm::core *cpu = kern->get_cpu()) {
                    cpu->imb_range(site, 2);
                }
                if (g_j9_invoke_n < 120) {
                    g_j9_invoke_sites[g_j9_invoke_n++] = site;
                }
            }
        }
        }
        // Interpreter native call: `mov lr,pc; bx sb`. sb is the compiled
        // wrapper or send. JXE compile fails and leaves sb=0 (Main pc=0,
        // lr=0x818F6D78). Trap those sites and recover to the adapter.
        g_j9_bx_sb_n = 0;
        for (address a = 0x818F2000u; (a + 8u < 0x81910000u) && (g_j9_bx_sb_n < 32); a += 4) {
            auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(a));
            if (!p) {
                continue;
            }
            if ((p[0] == 0xE1A0E00Fu)
                && ((p[1] == 0xE12FFF19u) || (p[1] == 0xE1200070u))) {
                if (p[1] == 0xE12FFF19u) {
                    p[1] = 0xE1200070u;
                    if (arm::core *cpu = kern->get_cpu()) {
                        cpu->imb_range(a + 4, 4);
                    }
                }
                g_j9_bx_sb_sites[g_j9_bx_sb_n++] = a + 4;
            }
        }
        // Prepared JNI invoke / bytecode invoke: restore r4-r7 from the VM thread, then bx send / ldr pc, [r0, #8].
        for (address site : { 0x81911DFCu, 0x81911E60u, 0x81910B94u, 0x81910BB0u,
                 0x8190F0A0u, 0x818F6608u, 0x818F67A0u, 0x8190E940u, 0x819106F4u,
                 0x81910C6Cu, 0x81910DDCu, 0x81910F04u, 0x81910F44u, 0x818F5A78u, 0x81910F84u,
                 0x81910F88u, 0x81910F8Cu, 0x818F5A84u, 0x818F9410u, 0x818F85C0u,
                 0x818F87F0u, 0x818F8A80u, 0x818F8C14u, 0x818F8E00u, 0x818F910Cu,
                 0x818F9B5Cu, 0x818F9FA8u, 0x818ED590u }) {
            if (g_j9_invoke_n >= 120) {
                break;
            }
            if (auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(site))) {
                if ((*p == 0xE3E0E003u) || (*p == 0xE5991000u) || (*p == 0xE1200070u)
                    || (*p == 0xE287A008u) || (*p == 0xE497C004u) || (*p == 0xE1A00004u)
                    || (*p == 0xE1A03004u) || (*p == 0xE1A01004u)
                    || (*p == 0xE3E09007u) || (*p == 0xE3E01007u)
                    || (*p == 0xE797C10Au) || (*p == 0x159AA000u)
                    || (*p == 0x179AC00Cu) || (*p == 0x1590F008u)
                    || (*p == 0xE5919A18u) || ((*p & 0xFFFFF000u) == 0xE24DD000u)
                    || ((*p & 0xFFF00000u) == 0xE5900000u) || ((*p & 0xFFF00000u) == 0xE5990000u)
                    || ((*p & 0xFFF00000u) == 0xE4900000u)) {
                    *p = 0xE1200070u;
                    if (arm::core *cpu = kern->get_cpu()) {
                        cpu->imb_range(site, 4);
                    }
                    g_j9_invoke_sites[g_j9_invoke_n++] = site;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] planted invoke site 0x{:X}", site);
                }
            }
        }
        if (auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(0x81910F04u))) {
            *p = 0xE1200070u;
            if (arm::core *cpu = kern->get_cpu()) {
                cpu->imb_range(0x81910F04u, 4);
            }
            bool have = false;
            for (int i = 0; i < g_j9_invoke_n; ++i) {
                if ((g_j9_invoke_sites[i] & ~1u) == 0x81910F04u) {
                    have = true;
                    break;
                }
            }
            if (!have && (g_j9_invoke_n < 120)) {
                g_j9_invoke_sites[g_j9_invoke_n++] = 0x81910F04u;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] planted invoke site 0x{:X}", 0x81910F04u);
            }
        }
        for (address site : { 0x81910D1Cu, 0x81910E90u }) {
            if (g_j9_bx_sb_n >= 32) {
                break;
            }
            if (auto *p = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(site))) {
                if (*p == 0xE12FFF19u || *p == 0xE1200070u) {
                    *p = 0xE1200070u;
                    if (arm::core *cpu = kern->get_cpu()) {
                        cpu->imb_range(site, 4);
                    }
                    g_j9_bx_sb_sites[g_j9_bx_sb_n++] = site;
                }
            }
        }
        j9_seed_midp_bss_types(pr, kern->crr_thread());
        LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI walker invoke tramp@0x{:X} sites={} bxsb={}",
            tramp_va, n, g_j9_bx_sb_n);
        static bool hooked = false;
        if (!hooked) {
            kern->register_breakpoint_hit_callback(j9_bindnatv_bkpt);
            hooked = true;
        }
    }

    static address j9_lookup_jni_prefix(const char *name, std::string *matched) {
        if (!name || !name[0]) {
            return 0;
        }
        if (const address fn = j9_lookup_jni_export(name)) {
            if (matched) {
                *matched = name;
            }
            return fn;
        }
        const std::size_t nlen = std::strlen(name);
        struct Pref {
            address fn = 0;
            const char *nm = nullptr;
        };
        Pref hits[4];
        int nhit = 0;
        for (const auto &ent : j9_jni_exports) {
            if ((ent.name.size() <= nlen) || (ent.name.compare(0, nlen, name) != 0)
                || (ent.name.compare(nlen, 3, "___") != 0)) {
                continue;
            }
            if (nhit < 4) {
                hits[nhit].fn = ent.fn;
                hits[nhit].nm = ent.name.c_str();
                ++nhit;
            }
        }
        if (nhit == 0) {
            return 0;
        }
        static std::string last_q;
        static int q_idx = 0;
        if (last_q != name) {
            last_q = name;
            q_idx = 0;
        }
        const int pick = (q_idx < nhit) ? q_idx : (nhit - 1);
        ++q_idx;
        if (matched) {
            *matched = hits[pick].nm ? hits[pick].nm : name;
        }
        return hits[pick].fn;
    }

    static const char *j9_guest_cstr(kernel::process *pr, const address p) {
        if (!pr || (p < 0x10000) || (p > 0xFFFF0000u)) {
            return nullptr;
        }
        const char *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(p));
        if (!s || (s[0] < 0x20) || (s[0] > 0x7E)) {
            return nullptr;
        }
        return s;
    }

    static std::string g_j9_last_java_name;

    static address j9_try_name_ptr(kernel::process *pr, const address p) {
        const char *s = j9_guest_cstr(pr, p);
        if (!s) {
            return 0;
        }
        if (const address fn = j9_lookup_jni_export(s)) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] bound '{}' -> 0x{:X}", s, fn);
            return fn;
        }
        if ((std::memcmp(s, "Java_", 5) == 0) || (std::memcmp(s, "com_", 4) == 0)
            || (std::memcmp(s, "java_", 5) == 0) || (std::memcmp(s, "org_", 4) == 0)
            || (std::memcmp(s, "javax_", 6) == 0)) {
            g_j9_last_java_name = s;
        }
        return 0;
    }

    static address j9_bind_from_method(kernel::process *pr, const address method) {
        if (!pr || (method < 0x10000)) {
            return 0;
        }
        const auto *mw = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(method));
        if (!mw) {
            return 0;
        }
        const address desc = mw[0];
        const auto *b = reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(desc));
        if (!b) {
            return 0;
        }
        if (const address fn = j9_try_name_ptr(pr, desc)) {
            return fn;
        }
        if (const address fn = j9_try_name_ptr(pr, desc + 2)) {
            return fn;
        }
        const address named = desc + 2u + static_cast<address>(b[0]);
        return j9_try_name_ptr(pr, named);
    }

    static address j9_bind_from_regs(arm::core *core, kernel::process *pr) {
        const std::uint32_t cands[12] = {
            core->get_reg(0), core->get_reg(1), core->get_reg(2),
            core->get_reg(3), core->get_reg(4), core->get_reg(5),
            core->get_reg(6), core->get_reg(7), core->get_reg(8),
            core->get_reg(9), core->get_reg(10), core->get_reg(11)
        };
        for (std::uint32_t p : cands) {
            if (const address fn = j9_try_name_ptr(pr, p)) {
                return fn;
            }
        }
        if (!g_j9_last_java_name.empty()) {
            const std::string saved_name = std::move(g_j9_last_java_name);
            g_j9_last_java_name.clear();
            const address fn = j9_lookup_jni_export(saved_name.c_str());
            if (fn) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] bound saved '{}' -> 0x{:X}",
                    saved_name, fn);
                return fn;
            }
        }
        return 0;
    }

    static void j9_bindnatv_bkpt(arm::core *core, kernel::thread *thr, const std::uint32_t addr) {
        if (!core) {
            return;
        }
        const address pc = addr & ~1u;
        kernel::process *pr = thr ? thr->owning_process() : nullptr;
        if (pr && g_j9_proplist_bkpt && (pc == g_j9_proplist_bkpt)) {
            const address arr = j9_ensure_proplist(pr);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] proplist-stub arr=0x{:X} ac=0x{:X} lr=0x{:X} r5=0x{:X} r7=0x{:X}",
                arr, g_j9_string_array_clazz, core->get_lr(),
                core->get_reg(5), core->get_reg(7));
            core->set_reg(0, arr);
            address vt = core->get_reg(8);
            if (!j9_r8_looks_vmthread(pr, vt, 0)) {
                vt = g_j9_vmthread ? g_j9_vmthread : core->get_reg(0);
            }
            j9_restore_interp_table(core, pr, vt, false);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && g_j9_throw_bkpt && (pc == g_j9_throw_bkpt)) {
            const address meth = core->get_reg(7);
            const address send = (meth && j9_mapped32(pr, meth + 12u))
                ? j9_read32(pr, meth + 8u) : 0;
            const address extra = (meth && j9_mapped32(pr, meth + 12u))
                ? j9_read32(pr, meth + 12u) : 0;
            const address vt = g_j9_vmthread ? g_j9_vmthread : core->get_reg(8);
            const address bpc = vt ? j9_read32(pr, vt + 0x14u) : 0;
            const address jsp = vt ? j9_read32(pr, vt + 0x10u) : 0;
            address invoke_pc = j9_scan_invoke_pc(pr, jsp);
            if (!g_j9_boot_returned && !j9_in_system_init(invoke_pc) && jsp) {
                for (int i = 0; i < 12; ++i) {
                    const address w = j9_read32(pr, jsp + static_cast<address>(i) * 4u);
                    if (j9_in_system_init(w)) {
                        invoke_pc = w;
                        break;
                    }
                }
            }
            const auto usable_cp_pc = [&](const address w) {
                if (!w || (w == g_j9_last_skip_pc) || (w == g_j9_wrap_t1)
                    || !j9_looks_bytecode_pc(pr, w)) {
                    return false;
                }
                return j9_is_cp_op(j9_read8(pr, w));
            };
            const auto boot_jcl_pc = [&](const address w) {
                return usable_cp_pc(w) && !j9_in_system_init(w)
                    && !j9_is_initialize_pc(w)
                    && (w >= 0x81950000u) && (w < 0x81A00000u);
            };
            if (g_j9_boot_returned) {
                if (boot_jcl_pc(g_j9_saved_r5)) {
                    invoke_pc = g_j9_saved_r5;
                } else if (boot_jcl_pc(g_j9_live_r5)) {
                    invoke_pc = g_j9_live_r5;
                } else if (j9_looks_bytecode_pc(pr, g_j9_live_r5)
                    && !j9_is_initialize_pc(g_j9_live_r5)
                    && !j9_is_interpret_tail_pc(g_j9_live_r5)) {
                    const address fwd = j9_skip_simple_ops(pr, g_j9_live_r5);
                    if (boot_jcl_pc(fwd)) {
                        invoke_pc = fwd;
                    }
                } else if (j9_in_system_init(invoke_pc) && jsp) {
                    invoke_pc = 0;
                    for (int i = 0; i < 16; ++i) {
                        const address w = j9_read32(pr,
                            jsp + static_cast<address>(i) * 4u);
                        if (boot_jcl_pc(w)) {
                            invoke_pc = w;
                            break;
                        }
                    }
                }
            }
            if (!invoke_pc && usable_cp_pc(g_j9_saved_r5)) {
                invoke_pc = g_j9_saved_r5;
            }
            if (!invoke_pc && usable_cp_pc(g_j9_live_r5)) {
                invoke_pc = g_j9_live_r5;
            }
            if (!invoke_pc && jsp) {
                for (int i = 0; i < 12; ++i) {
                    const address w = j9_read32(pr, jsp + static_cast<address>(i) * 4u);
                    if (usable_cp_pc(w)) {
                        invoke_pc = w;
                        break;
                    }
                    if ((w >= 0x81950003u) && usable_cp_pc(w - 3u)) {
                        invoke_pc = w - 3u;
                        break;
                    }
                }
            }
            static int sys_bc_dump = 0;
            if (sys_bc_dump < 2) {
                ++sys_bc_dump;
                char hex[400];
                static const char *khex = "0123456789ABCDEF";
                unsigned n = 0;
                for (unsigned i = 0; (i < 192u) && (n + 2u < sizeof(hex)); ++i) {
                    const std::uint8_t b = j9_read8(pr, 0x8195D6E0u + i);
                    hex[n++] = khex[b >> 4];
                    hex[n++] = khex[b & 0xFu];
                }
                hex[n] = 0;
                const address ht = 0x2D103B4u;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] sys-init-bc {} ht=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} "
                    "stk=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                    hex,
                    j9_read32(pr, ht), j9_read32(pr, ht + 4u), j9_read32(pr, ht + 8u),
                    j9_read32(pr, ht + 12u), j9_read32(pr, ht + 16u), j9_read32(pr, ht + 20u),
                    jsp ? j9_read32(pr, jsp) : 0, jsp ? j9_read32(pr, jsp + 4u) : 0,
                    jsp ? j9_read32(pr, jsp + 8u) : 0, jsp ? j9_read32(pr, jsp + 12u) : 0,
                    jsp ? j9_read32(pr, jsp + 16u) : 0, jsp ? j9_read32(pr, jsp + 20u) : 0,
                    jsp ? j9_read32(pr, jsp + 24u) : 0, jsp ? j9_read32(pr, jsp + 28u) : 0);
            }
            static int throw_logs = 0;
            if ((throw_logs < 16) || (g_j9_boot_returned && (throw_logs < 40))) {
                ++throw_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] throw-hit meth=0x{:X} send=0x{:X} extra=0x{:X} "
                    "bpc=0x{:X} jsp=0x{:X} inv=0x{:X} enc={} lr=0x{:X} "
                    "s0=0x{:X} s1=0x{:X} s2=0x{:X} s3=0x{:X}",
                    meth, send, extra, bpc, jsp, invoke_pc, g_j9_encoding_n, core->get_lr(),
                    jsp ? j9_read32(pr, jsp) : 0, jsp ? j9_read32(pr, jsp + 4u) : 0,
                    jsp ? j9_read32(pr, jsp + 8u) : 0, jsp ? j9_read32(pr, jsp + 12u) : 0);
            }
            if (!meth && g_j9_vt10_c) {
                j9_restore_interp_vt(pr);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] throw-vt-restore 0x{:X}/0x{:X}/0x{:X}",
                    g_j9_vt10_c, g_j9_vt14_c, g_j9_vt18_c);
            }
            j9_try_capture_live_cframe(pr, core);
            const bool sys_inv = j9_in_system_init(invoke_pc);
            const bool jcl_inv = (invoke_pc >= 0x81950000u) && (invoke_pc < 0x81A00000u);
            const bool unresolved_send = (extra == 1u)
                && ((send == 0x81911BB0u) || (send == 0x819119C0u)
                    || (send == 0x81911C88u)
                    || ((send >= 0x81911800u) && (send < 0x81912200u)));
            const bool sb_init = (invoke_pc >= 0x8195BB80u)
                && (invoke_pc < 0x8195BBA8u);
            const bool caller_cp = (invoke_pc >= 0x81963090u)
                && (invoke_pc < 0x81963100u);
            const bool conv_npe = g_j9_boot_returned
                && (((invoke_pc >= 0x8195A7F0u) && (invoke_pc <= 0x8195A830u))
                    || (g_j9_util_conv_done
                        && ((invoke_pc == 0x8195ACE0u)
                            || (invoke_pc == 0x81961C71u))));
            if (g_j9_util_conv_done && !g_j9_string_astore_done && g_j9_boot_returned
                && ((invoke_pc == 0x8195ACE0u) || (invoke_pc == 0x81961C71u)
                    || ((invoke_pc >= 0x8195ACE0u) && (invoke_pc < 0x8195AD00u)))) {
                const address dummy = j9_ensure_converter_dummy(pr);
                address sr4 = 0x726620u;
                address sfp = 0x71E548u;
                if (g_j9_conv_caller_r7 && j9_mapped32(pr, g_j9_conv_caller_r7 + 12u)
                    && (j9_read32(pr, g_j9_conv_caller_r7 + 8u) == 0x8195A7F5u)) {
                    const address fr = j9_read32(pr, g_j9_conv_caller_r7 + 12u);
                    const address mr = j9_read32(pr, g_j9_conv_caller_r7 + 4u);
                    if (j9_is_java_fp(fr)) {
                        sfp = fr;
                        sr4 = mr;
                    }
                }
                j9_save_string_ret(pr, sfp);
                address sosp = (sfp >= 4u) ? (sfp - 4u) : sfp;
                if (dummy && sosp && j9_mapped32(pr, sosp)) {
                    j9_write32(pr, sosp, dummy);
                }
                if (dummy && sfp && j9_mapped32(pr, sfp + 16u)) {
                    j9_write32(pr, sfp + 16u, dummy);
                }
                j9_seed_string_if_needed(pr, sfp);
                g_j9_saved_r4 = sr4;
                g_j9_saved_r5 = 0x8195A7F5u;
                g_j9_saved_r6 = sfp;
                g_j9_java_sp = sosp;
                g_j9_resume_at = 0x8195A7F8u;
                g_j9_resume_no_ac = true;
                g_j9_string_astore_done = true;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] skip-util-done pc=0x{:X} -> 0x8195A7F8 r4=0x{:X} "
                    "r6=0x{:X} dummy=0x{:X}",
                    invoke_pc, sr4, sfp, dummy);
                ++g_j9_throw_skips;
                j9_jxe_resume_interp(core, pr, meth, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            const bool cc_after_init = g_j9_init_c_returned
                && (invoke_pc >= 0x81961800u) && (invoke_pc < 0x81963000u)
                && (invoke_pc != 0x81961C71u);
            if (cc_after_init && unresolved_send) {
                const std::uint8_t iop = j9_read8(pr, invoke_pc);
                if (invoke_pc == g_j9_cc_last_pc) {
                    if (++g_j9_cc_same_n >= 3) {
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] skip-cc-int-stop pc=0x{:X} op=0x{:02X}",
                            invoke_pc, iop);
                        j9_set_pc(core, (g_j9_throw_bkpt + 2u) | 1u);
                        return;
                    }
                } else {
                    g_j9_cc_last_pc = invoke_pc;
                    g_j9_cc_same_n = 1;
                }
                const std::uint8_t next = j9_read8(pr, invoke_pc + 3u);
                address fp = (g_j9_live_r6 && j9_is_java_fp(g_j9_live_r6))
                    ? g_j9_live_r6 : jsp;
                address osp = g_j9_live_r7 ? g_j9_live_r7
                    : (fp ? (fp - 4u) : 0);
                address r4 = g_j9_live_r4 ? g_j9_live_r4
                    : (g_j9_saved_r4 ? g_j9_saved_r4 : 0x729650u);
                address resume = invoke_pc + 3u;
                const char *why = "invoke";
                const bool cc_create_body = (invoke_pc == 0x81961AA7u)
                    || (invoke_pc == 0x81961AB6u) || (invoke_pc == 0x81961AC6u)
                    || (invoke_pc == 0x81961ACBu) || (invoke_pc == 0x81961AD1u)
                    || (invoke_pc == 0x81961AD5u);
                if (cc_create_body) {
                    // create() send leftover after skip-lazy-init-ret.
                    // Official AD here AVs; hand the dummy back at String
                    // A7F8 the same way skip-lazy-cache already did.
                    const address dummy = j9_ensure_converter_dummy(pr);
                    r4 = 0x726620u;
                    fp = 0x71E548u;
                    osp = 0x71E544u;
                    if (g_j9_conv_caller_r7 && j9_mapped32(pr, g_j9_conv_caller_r7 + 12u)
                        && (j9_read32(pr, g_j9_conv_caller_r7 + 8u) == 0x8195A7F5u)) {
                        const address fr = j9_read32(pr, g_j9_conv_caller_r7 + 12u);
                        const address mr = j9_read32(pr, g_j9_conv_caller_r7 + 4u);
                        if (j9_is_java_fp(fr)) {
                            fp = fr;
                            r4 = mr ? mr : r4;
                            osp = (fr >= 4u) ? (fr - 4u) : fr;
                        }
                    }
                    j9_save_string_ret(pr, fp);
                    if (dummy && osp && j9_mapped32(pr, osp)) {
                        j9_write32(pr, osp, dummy);
                    }
                    if (dummy && fp && j9_mapped32(pr, fp + 16u)) {
                        j9_write32(pr, fp + 16u, dummy);
                    }
                    j9_seed_string_if_needed(pr, fp);
                    resume = 0x8195A7F8u;
                    why = "cc-create-a7f8";
                    g_j9_string_astore_done = true;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-cc-create dummy=0x{:X} -> 0x8195A7F8 "
                        "r4=0x{:X} r6=0x{:X} r7=0x{:X}",
                        dummy, r4, fp, osp);
                } else if (iop == 0xB3u) {
                    // Official putstatic resolve ICCEs. Discard the value.
                    if (osp) {
                        osp += 4u;
                    }
                    why = "putstatic";
                } else if (iop == 0xB2u) {
                    if (osp >= 4u) {
                        osp -= 4u;
                    }
                    if (osp && j9_mapped32(pr, osp)) {
                        j9_write32(pr, osp, 0);
                    }
                    why = "getstatic";
                } else if (invoke_pc == 0x81961AB6u) {
                    // Skip the char[] fill loop; jump to `new String(buf)`.
                    resume = 0x81961AC6u;
                    osp += 16u;
                    why = "cc-fill";
                } else if (j9_is_invoke_op(iop) && (next == 0xB3u)
                    && (invoke_pc >= 0x81962314u) && (invoke_pc < 0x81962334u)) {
                    // Character.<clinit>: ldc; intern; putstatic. Drop the
                    // string and skip both CP ops — field resolve ICCEs.
                    if (osp) {
                        osp += 4u;
                    }
                    resume = invoke_pc + 6u;
                    why = "clinit-intern";
                } else if (j9_is_invoke_op(iop)
                    && ((next == 0xBCu) || (next == 0xBDu))) {
                    if (osp >= 4u) {
                        osp -= 4u;
                    }
                    if (osp && j9_mapped32(pr, osp)) {
                        j9_write32(pr, osp, 8);
                    }
                    why = "newarray";
                } else if (j9_is_invoke_op(iop) && (next == 0xB3u)) {
                    if (osp) {
                        osp += 4u;
                    }
                    resume = invoke_pc + 6u;
                    why = "invoke-putstatic";
                } else if (!j9_is_invoke_op(iop)) {
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-cc-int-off pc=0x{:X} op=0x{:02X}",
                        invoke_pc, iop);
                    j9_set_pc(core, (g_j9_throw_bkpt + 2u) | 1u);
                    return;
                }
                g_j9_saved_r4 = r4;
                g_j9_saved_r5 = resume;
                g_j9_saved_r6 = fp;
                g_j9_java_sp = osp;
                g_j9_resume_at = resume;
                g_j9_resume_no_ac = true;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] skip-cc-int pc=0x{:X} -> 0x{:X} why={} "
                    "op=0x{:02X} next=0x{:02X} r4=0x{:X} r6=0x{:X} r7=0x{:X}",
                    invoke_pc, resume, why, iop, next, r4, fp, osp);
                ++g_j9_throw_skips;
                j9_jxe_resume_interp(core, pr, meth, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            if ((g_j9_encoding_n >= 3) && !sys_inv && jcl_inv
                && !j9_is_initialize_pc(invoke_pc)
                && !cc_after_init
                && (g_j9_throw_skips < 40)
                && (unresolved_send || conv_npe)
                && (sb_init || caller_cp || g_j9_boot_returned)) {
                address fp = jsp;
                address osp = 0;
                address r4 = 0;
                if (g_j9_live_r6 && (g_j9_live_r6 >= 0x0071E000u)
                    && (g_j9_live_r6 < 0x00720000u)
                    && g_j9_live_r5
                    && ((g_j9_live_r5 & ~0xFFu) == (invoke_pc & ~0xFFu))) {
                    fp = g_j9_live_r6;
                    osp = g_j9_live_r7 ? g_j9_live_r7 : (fp - 8u);
                    r4 = g_j9_live_r4;
                    // Snapshot at the invoke itself still has the outgoing
                    // `this` on the stack; pop it. invokestatic has no
                    // receiver — keep the argument as an identity return.
                    if ((g_j9_live_r5 >= invoke_pc) && osp && (osp + 4u <= fp)
                        && (j9_read8(pr, invoke_pc) != 0xB8u)) {
                        osp += 4u;
                    }
                }
                if (caller_cp && g_j9_caller_live_r6
                    && (g_j9_caller_live_r6 >= 0x0071E000u)
                    && (g_j9_caller_live_r6 < 0x00720000u)) {
                    fp = g_j9_caller_live_r6 & ~3u;
                    if (g_j9_caller_live_r7) {
                        osp = g_j9_caller_live_r7;
                    }
                    if (g_j9_caller_live_r4) {
                        r4 = g_j9_caller_live_r4;
                    }
                }
                if ((fp < 0x0071E000u) || (fp >= 0x00720000u)) {
                    fp = g_j9_wrap_java_fp;
                }
                if ((fp < 0x0071E000u) || (fp >= 0x00720000u)) {
                    fp = g_j9_wrap_r6;
                }
                const auto cp_ok = [&](const address c) {
                    if ((c < 0x00720000u) || (c >= 0x00730000u)
                        || !j9_mapped32(pr, c + 4u)) {
                        return false;
                    }
                    const address slot = j9_read32(pr, c + 4u) & ~7u;
                    if (!slot) {
                        return false;
                    }
                    if ((slot >= 0x81940000u) && (slot < 0x81A00000u)) {
                        return pr->get_ptr_on_addr_space(slot) != nullptr;
                    }
                    return j9_mapped32(pr, slot);
                };
                const address s2 = fp ? j9_read32(pr, fp + 8u) : 0;
                const address s_m4 = fp ? j9_read32(pr, fp - 4u) : 0;
                if (!cp_ok(r4)) {
                    if (cp_ok(s2)) {
                        r4 = s2;
                    } else if (cp_ok(g_j9_wrap_r4)) {
                        r4 = g_j9_wrap_r4;
                    } else if (cp_ok(g_j9_saved_r4)) {
                        r4 = g_j9_saved_r4;
                    } else {
                        r4 = s2 ? s2 : (g_j9_last_jcl_clazz ? g_j9_last_jcl_clazz
                            : 0x728648u);
                    }
                }
                if (!osp) {
                    osp = (fp >= 8u) ? (fp - 8u) : fp;
                }
                address resume_pc = invoke_pc + 3u;
                if (conv_npe) {
                    if (g_j9_live_r6 && j9_is_java_fp(g_j9_live_r6)
                        && g_j9_live_r5
                        && (g_j9_live_r5 >= 0x8195A700u)
                        && (g_j9_live_r5 <= 0x8195A830u)) {
                        fp = g_j9_live_r6;
                        if (g_j9_live_r4) {
                            r4 = g_j9_live_r4;
                        }
                        osp = g_j9_live_r7 ? g_j9_live_r7 : ((fp >= 4u) ? (fp - 4u) : fp);
                    } else if (g_j9_saved_r6 && j9_is_java_fp(g_j9_saved_r6)) {
                        fp = g_j9_saved_r6;
                        if (g_j9_saved_r4) {
                            r4 = g_j9_saved_r4;
                        }
                        osp = (fp >= 4u) ? (fp - 4u) : fp;
                    }
                    j9_seed_string_if_needed(pr, fp);
                    address skip_from = invoke_pc;
                    if (g_j9_util_conv_done
                        && ((invoke_pc == 0x8195A7F5u)
                            || (invoke_pc == 0x8195ACE0u)
                            || (invoke_pc == 0x81961C71u))) {
                        skip_from = 0x8195A7F8u;
                        fp = 0x71E548u;
                        r4 = 0x726620u;
                        if (g_j9_conv_caller_r7
                            && j9_mapped32(pr, g_j9_conv_caller_r7 + 12u)
                            && (j9_read32(pr, g_j9_conv_caller_r7 + 8u)
                                == 0x8195A7F5u)) {
                            const address fr = j9_read32(pr,
                                g_j9_conv_caller_r7 + 12u);
                            if (j9_is_java_fp(fr)) {
                                fp = fr;
                                r4 = j9_read32(pr, g_j9_conv_caller_r7 + 4u);
                            }
                        }
                        osp = (fp >= 4u) ? (fp - 4u) : fp;
                    }
                    resume_pc = j9_skip_invoke_putfield(pr, skip_from);
                    if (j9_read8(pr, resume_pc) == 0xACu) {
                        address or4 = 0;
                        address opc = 0;
                        address ofp = 0;
                        const bool have_caller =
                            j9_find_caller_frame(pr, fp, invoke_pc, &or4, &opc, &ofp)
                            || j9_scan_java_caller_above(pr, fp, invoke_pc,
                                &or4, &opc, &ofp);
                        if (have_caller) {
                            const std::uint8_t cop = j9_read8(pr, opc);
                            address cosp = (ofp >= 4u) ? (ofp - 4u) : ofp;
                            const address thiz = fp
                                ? j9_obj_from_slot(pr, j9_read32(pr, fp)) : 0;
                            if ((cop != 0xB7u) && cosp && j9_mapped32(pr, cosp)) {
                                j9_write32(pr, cosp, 0);
                            } else if (thiz && cosp && j9_mapped32(pr, cosp)) {
                                if (!j9_obj_from_slot(pr, j9_read32(pr, cosp))) {
                                    j9_write32(pr, cosp, thiz);
                                }
                            }
                            r4 = or4;
                            fp = ofp;
                            osp = cosp;
                            resume_pc = opc + 3u;
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] conv-ac-unwind -> 0x{:X} r4=0x{:X} "
                                "r6=0x{:X} r7=0x{:X} inv=0x{:02X} thiz=0x{:X}",
                                resume_pc, r4, fp, osp, cop, thiz);
                        } else if (fp && j9_mapped32(pr, fp + 12u)) {
                            const address thiz = j9_obj_from_slot(pr,
                                j9_read32(pr, fp));
                            if (!g_j9_str_ret_ok) {
                                j9_save_string_ret(pr, fp);
                            }
                            j9_fill_string_from_locals(pr, fp);
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] conv-ac-defer this=0x{:X} -> 0x{:X}",
                                thiz, resume_pc);
                        }
                    }
                }
                const std::uint8_t iop = j9_read8(pr, invoke_pc);
                if ((iop == 0xB2u) && osp && (osp >= 4u) && j9_mapped32(pr, osp - 4u)) {
                    osp -= 4u;
                    j9_write32(pr, osp, 0);
                } else if ((iop == 0xB3u) && osp) {
                    osp += 4u;
                } else if ((iop == 0xB8u) && g_j9_boot_returned && osp
                    && j9_mapped32(pr, osp)) {
                    const address top = j9_read32(pr, osp);
                    if (!j9_looks_heap(top)) {
                        j9_write32(pr, osp, 0);
                    }
                } else if ((iop == 0xB6u) && (j9_read8(pr, invoke_pc + 3u) == 0xB5u)
                    && !conv_npe && osp && (osp >= 4u)
                    && j9_mapped32(pr, osp - 4u)) {
                    j9_write32(pr, osp, 0);
                }
                g_j9_resume_at = resume_pc;
                g_j9_java_sp = osp;
                g_j9_saved_r4 = r4;
                g_j9_saved_r5 = invoke_pc;
                g_j9_saved_r6 = fp;
                g_j9_saved_r2 = k_j9_opcode_table;
                g_j9_last_skip_pc = invoke_pc;
                std::uint8_t next_op = j9_read8(pr, g_j9_resume_at);
                const std::uint8_t n1 = j9_read8(pr, g_j9_resume_at + 1u);
                const std::uint8_t n2 = j9_read8(pr, g_j9_resume_at + 2u);
                const auto dump12 = [&](const address at, char *out, const size_t n) {
                    static const char *khex = "0123456789ABCDEF";
                    unsigned w = 0;
                    for (unsigned i = 0; (i < 12u) && (w + 2u < n); ++i) {
                        const std::uint8_t b = j9_read8(pr, at + i);
                        out[w++] = khex[b >> 4];
                        out[w++] = khex[b & 0xFu];
                    }
                    out[w] = 0;
                };
                char bc12[32];
                dump12(g_j9_resume_at, bc12, sizeof(bc12));
                if (g_j9_boot_returned && (next_op >= 0x4Bu) && (next_op <= 0x4Eu)
                    && (n1 == 0xB2u) && fp && (fp >= 0x0071E000u)
                    && (fp < 0x00720000u)) {
                    g_j9_conv_caller_r4 = r4;
                    g_j9_conv_caller_r5 = g_j9_resume_at;
                    g_j9_conv_caller_r6 = fp;
                    g_j9_conv_caller_r7 = osp;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] conv-caller pc=0x{:X} r4=0x{:X} r6=0x{:X} "
                        "r7=0x{:X} bc={}",
                        g_j9_resume_at, r4, fp, osp, bc12);
                }
                // getDefaultConverter lazy cache: invoke; dup; astore_n;
                // ifnonnull. Do not resume the official create/putstatic
                // tail (field resolve AVs). Return the dummy to Util.
                const bool lazy_cache = g_j9_boot_returned && (next_op == 0x59u)
                    && (n1 >= 0x4Bu) && (n1 <= 0x4Eu) && (n2 == 0xC7u);
                if (lazy_cache) {
                    const address dummy = j9_ensure_converter_dummy(pr);
                    const address packed = j9_stack_ref(dummy);
                    const address ifpc = g_j9_resume_at + 2u;
                    const std::int16_t off = static_cast<std::int16_t>(
                        j9_read8(pr, ifpc + 1u)
                        | (static_cast<unsigned>(j9_read8(pr, ifpc + 2u)) << 8));
                    address slot = g_j9_conv_caller_r7 ? g_j9_conv_caller_r7 : osp;
                    const address s1 = slot ? j9_read32(pr, slot + 4u) : 0;
                    const address s2 = slot ? j9_read32(pr, slot + 8u) : 0;
                    const address s3 = slot ? j9_read32(pr, slot + 12u) : 0;
                    char caller_bc[32];
                    dump12(s2, caller_bc, sizeof(caller_bc));
                    if (j9_looks_bytecode_pc(pr, s2) && j9_is_cp_op(j9_read8(pr, s2))
                        && (s3 >= 0x0071E000u) && (s3 < 0x00720000u)) {
                        // Hand the dummy back at String A7F8 (astore 4).
                        // Official Util areturn is BKPT'd and leaves r5 at
                        // ACE0, which rewinds into getDefaultConverter.
                        j9_save_string_ret(pr, s3);
                        address csp = (s3 >= 4u) ? (s3 - 4u) : s3;
                        if (dummy && csp && j9_mapped32(pr, csp)
                            && (j9_read32(pr, csp) != 0x81922C1Du)) {
                            j9_write32(pr, csp, dummy);
                        }
                        if (dummy && s3 && j9_mapped32(pr, s3 + 16u)
                            && (j9_read32(pr, s3 + 16u) != 0x81922C1Du)) {
                            j9_write32(pr, s3 + 16u, dummy);
                        }
                        j9_seed_string_if_needed(pr, s3);
                        r4 = s1;
                        fp = s3;
                        osp = csp;
                        g_j9_saved_r4 = r4;
                        g_j9_saved_r5 = s2;
                        g_j9_saved_r6 = fp;
                        g_j9_java_sp = osp;
                        g_j9_resume_at = s2 + 3u;
                        g_j9_util_conv_done = true;
                        g_j9_string_astore_done = true;
                    } else if (g_j9_conv_caller_r5 && g_j9_conv_caller_r4) {
                        address cr5 = 0x8195ACF8u;
                        address cr7 = g_j9_conv_caller_r7;
                        address cr6 = g_j9_conv_caller_r6;
                        if (packed && cr6 && j9_mapped32(pr, cr6)) {
                            j9_write32(pr, cr6, packed);
                        }
                        if (packed && cr7 && j9_mapped32(pr, cr7)) {
                            j9_write32(pr, cr7, packed);
                        }
                        r4 = g_j9_conv_caller_r4;
                        fp = cr6;
                        osp = cr7;
                        g_j9_saved_r4 = r4;
                        g_j9_saved_r5 = cr5;
                        g_j9_saved_r6 = fp;
                        g_j9_java_sp = osp;
                        g_j9_resume_at = cr5;
                    } else if (packed && osp && j9_mapped32(pr, osp)) {
                        j9_write32(pr, osp, packed);
                        g_j9_java_sp = osp;
                    }
                    char hit12[32];
                    dump12(g_j9_resume_at, hit12, sizeof(hit12));
                    char tname[96];
                    tname[0] = 0;
                    const address thiz = fp ? j9_obj_from_slot(pr, j9_read32(pr, fp)) : 0;
                    if (thiz) {
                        j9_class_name(pr, j9_read32(pr, thiz), tname, sizeof(tname));
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-lazy-cache pc=0x{:X} -> 0x{:X} "
                        "dummy=0x{:X} pk=0x{:X} clazz=0x{:X} "
                        "r4=0x{:X} r6=0x{:X} r7=0x{:X} loc0=0x{:X} "
                        "thiz=0x{:X} tn='{}' val=0x{:X}/0x{:X}/0x{:X} "
                        "if=0x{:X} off={} call=0x{:X}/0x{:X}/0x{:X} "
                        "cbc={} bc={} hit={}",
                        invoke_pc, g_j9_resume_at, dummy, packed,
                        g_j9_converter_clazz, g_j9_saved_r4, g_j9_saved_r6,
                        g_j9_java_sp, fp ? j9_read32(pr, fp) : 0,
                        thiz, tname,
                        thiz ? j9_read32(pr, thiz + 8u) : 0,
                        thiz ? j9_read32(pr, thiz + 12u) : 0,
                        thiz ? j9_read32(pr, thiz + 16u) : 0,
                        ifpc, static_cast<int>(off),
                        s1, s2, s3, caller_bc, bc12, hit12);
                    next_op = j9_read8(pr, g_j9_resume_at);
                    if (fp) {
                        j9_dump_string_frame(pr, core, fp, "lazy");
                    }
                    if (thiz) {
                        j9_fill_string_from_locals(pr, fp);
                    }
                    j9_dump_cstack_frames(pr, core, "lazy");
                    j9_try_capture_live_cframe(pr, core);
                    if (j9_finish_class_init_c(core, pr)) {
                        g_j9_resume_no_ac = false;
                        g_j9_resume_at = 0;
                        return;
                    }
                }
                // Skipped invoke is often `create; putstatic; getstatic`.
                // Official get/putstatic resolve at 0x8191163C AVs
                // (r0=-8 → write FFFFFFF4) while the CP entry is a stub.
                int skipped_fs = 0;
                while (!lazy_cache && g_j9_boot_returned && (skipped_fs < 4) && osp
                    && j9_mapped32(pr, osp)) {
                    next_op = j9_read8(pr, g_j9_resume_at);
                    if (next_op == 0xB3u) {
                        osp += 4u;
                    } else if ((next_op == 0xB2u) && (osp >= 4u)) {
                        osp -= 4u;
                        j9_write32(pr, osp, 0);
                    } else {
                        break;
                    }
                    g_j9_java_sp = osp;
                    g_j9_resume_at += 3u;
                    ++skipped_fs;
                }
                if (skipped_fs) {
                    next_op = j9_read8(pr, g_j9_resume_at);
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-fieldstatic after 0x{:X} -> 0x{:X} "
                        "n={} op=0x{:02X} r7=0x{:X}",
                        invoke_pc, g_j9_resume_at, skipped_fs, next_op, osp);
                }
                next_op = j9_read8(pr, g_j9_resume_at);
                // Stale stack PC landed on a return. Hand the dummy back to
                // Util (astore; getstatic) instead of executing lreturn.
                if (!lazy_cache && g_j9_boot_returned && skipped_fs
                    && (next_op >= 0xACu) && (next_op <= 0xB1u)
                    && g_j9_conv_caller_r7) {
                    const address dummy = j9_ensure_converter_dummy(pr);
                    const address packed = j9_stack_ref(dummy);
                    const address slot = g_j9_conv_caller_r7;
                    const address s1 = j9_read32(pr, slot + 4u);
                    const address s2 = j9_read32(pr, slot + 8u);
                    const address s3 = j9_read32(pr, slot + 12u);
                    if (j9_looks_bytecode_pc(pr, s2)
                        && (s3 >= 0x0071E000u) && (s3 < 0x00720000u)) {
                        address csp = g_j9_conv_caller_r6 ? g_j9_conv_caller_r6 : fp;
                        if (csp >= 4u) {
                            csp -= 4u;
                        }
                        if (packed && csp && j9_mapped32(pr, csp)) {
                            j9_write32(pr, csp, packed);
                        }
                        r4 = s1;
                        fp = s3;
                        osp = csp;
                        g_j9_saved_r4 = r4;
                        g_j9_saved_r5 = s2;
                        g_j9_saved_r6 = fp;
                        g_j9_java_sp = osp;
                        g_j9_resume_at = s2 + 3u;
                        next_op = j9_read8(pr, g_j9_resume_at);
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] skip-ret-live from=0x{:X} -> 0x{:X} "
                            "dummy=0x{:X} pk=0x{:X} r4=0x{:X} r6=0x{:X} "
                            "r7=0x{:X} op=0x{:02X} call=0x{:X}",
                            invoke_pc, g_j9_resume_at, dummy, packed,
                            g_j9_saved_r4, g_j9_saved_r6, osp, next_op, s2);
                    }
                }
                g_j9_resume_no_ac = (next_op != 0xACu) && (next_op != 0xB1u)
                    && (next_op != 0xADu) && (next_op != 0xB0u);
                if (g_j9_boot_returned && !lazy_cache && !conv_npe
                    && !g_j9_util_conv_done) {
                    bool iret_soon = (next_op == 0xACu) || (next_op == 0xB1u);
                    for (int i = 0; !iret_soon && (i < 16); ++i) {
                        if (j9_read8(pr, g_j9_resume_at + static_cast<address>(i))
                            == 0xACu) {
                            iret_soon = true;
                        }
                    }
                    if (iret_soon) {
                        const j9_init_frame *top = (g_j9_init_depth > 0)
                            ? &g_j9_init_stack[g_j9_init_depth - 1] : nullptr;
                        const address clazz = top ? top->clazz : g_j9_last_jcl_clazz;
                        if (clazz && j9_mapped32(pr, clazz + 0x28u)) {
                            j9_write32(pr, clazz + 0x28u, 1);
                        }
                        if (top && top->csp) {
                            g_j9_pending_ret = *top;
                            g_j9_pending_clinit_ret = true;
                        }
                        if (g_j9_inl_r7 && (g_j9_inl_r7 >= 0x0071E000u)
                            && (g_j9_inl_r7 < 0x00720000u)) {
                            if (g_j9_inl_r4) {
                                g_j9_saved_r4 = g_j9_inl_r4;
                            }
                            g_j9_saved_r6 = g_j9_inl_r6 ? g_j9_inl_r6 : g_j9_inl_r7;
                            g_j9_java_sp = g_j9_inl_r7;
                        }
                        g_j9_resume_at = 0x819805F4u;
                        g_j9_saved_r5 = 0x819805F1u;
                        g_j9_resume_no_ac = true;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] throw-finish-clinit pc=0x{:X} clazz=0x{:X} "
                            "-> 0x819805F4 r4=0x{:X} r6=0x{:X} r7=0x{:X} "
                            "csp=0x{:X} clr=0x{:X}",
                            invoke_pc, clazz, g_j9_saved_r4, g_j9_saved_r6,
                            g_j9_java_sp, g_j9_pending_ret.csp,
                            g_j9_pending_ret.cframe[8]);
                    }
                }
                if (((invoke_pc == 0x819630A4u) || (invoke_pc == 0x819630A7u)
                        || (next_op == 0xB0u))
                    && g_j9_boot_t1 && !g_j9_boot_returned) {
                    // 0x726D60 is interpret()'s method. After the nested
                    // StringBuffer helper, do not execute its ireturn with
                    // a fabricated AC frame (that AV'd at 0x81911090).
                    // Return through interpret's official epilogue.
                    const address vmth = g_j9_vmthread ? g_j9_vmthread : vt;
                    if (vmth && j9_mapped32(pr, vmth + 0x6cu)) {
                        j9_write32(pr, vmth + 0x64u, 0);
                        j9_write32(pr, vmth + 0x6cu, 0);
                    }
                    if (g_j9_caller_hdr[4] && j9_mapped32(pr, 0x71E5F0u)) {
                        for (int i = 0; i < 16; ++i) {
                            j9_write32(pr, 0x71E5E0u
                                + static_cast<address>(i) * 4u, g_j9_caller_hdr[i]);
                        }
                    }
                    const address ilr = g_j9_boot_cframe[8];
                    const bool cframe_ok = g_j9_boot_csp
                        && (g_j9_boot_csp >= 0x00400000u)
                        && (g_j9_boot_csp < 0x00500000u)
                        && j9_mapped32(pr, g_j9_boot_csp + 32u)
                        && (ilr >= 0x81800000u) && (ilr < 0x82000000u)
                        && pr->get_ptr_on_addr_space(ilr);
                    if (cframe_ok) {
                        for (int i = 0; i < 9; ++i) {
                            j9_write32(pr, g_j9_boot_csp
                                + static_cast<address>(i) * 4u, g_j9_boot_cframe[i]);
                        }
                        g_j9_resume_at = 0;
                        g_j9_resume_no_ac = false;
                        g_j9_saved_r5 = 0;
                        core->set_reg(0, 0);
                        if (vmth) {
                            core->set_reg(8, vmth);
                        }
                        core->set_sp(g_j9_boot_csp);
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] throw-return-native pc=0x{:X} sp=0x{:X} "
                            "lr=0x{:X} r8=0x{:X} boot=0x{:X}/0x{:X}/0x{:X} "
                            "cf=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                            invoke_pc, g_j9_boot_csp, ilr,
                            vmth, g_j9_boot_t0, g_j9_boot_t1, g_j9_boot_t2,
                            g_j9_boot_cframe[0], g_j9_boot_cframe[4],
                            g_j9_boot_cframe[7], g_j9_boot_cframe[8]);
                        ++g_j9_throw_skips;
                        g_j9_boot_returned = true;
                        j9_set_pc(core, 0x818DE50Cu);
                        return;
                    }
                    address slot = g_j9_boot_fp ? g_j9_boot_fp : 0x71E5C4u;
                    if (!j9_mapped32(pr, slot + 8u)) {
                        slot = 0x71E5C4u;
                    }
                    j9_write32(pr, slot, 0);
                    j9_write32(pr, slot + 4u, 0x81922C1Du);
                    j9_write32(pr, slot + 8u, 0x71E600u);
                    g_j9_saved_r4 = 0;
                    g_j9_saved_r5 = 0x81962D40u;
                    g_j9_saved_r6 = g_j9_boot_t2 ? (g_j9_boot_t2 & ~3u) : 0x71E5ECu;
                    g_j9_java_sp = slot;
                    g_j9_resume_at = 0x81962D43u;
                    g_j9_resume_no_ac = true;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] throw-return-boot pc=0x{:X} -> 0x{:X} "
                        "r4=0x{:X} r6=0x{:X} csp=0x{:X} slot=0x{:X} "
                        "boot=0x{:X}/0x{:X}/0x{:X} bcsp=0x{:X} ilr=0x{:X}",
                        invoke_pc, g_j9_resume_at, g_j9_saved_r4,
                        g_j9_saved_r6, g_j9_java_sp, slot,
                        g_j9_boot_t0, g_j9_boot_t1, g_j9_boot_t2,
                        g_j9_boot_csp, ilr);
                } else if (!lazy_cache && !conv_npe && !g_j9_util_conv_done
                    && (next_op == 0xACu) && g_j9_wrap_t1
                    && j9_is_invoke_op(j9_read8(pr, g_j9_wrap_t1))) {
                    const address inst = fp ? j9_read32(pr, fp) : 0;
                    address cr4 = 0;
                    address cr6 = 0;
                    address csp = 0;
                    if (cp_ok(g_j9_caller_live_r4)) {
                        cr4 = g_j9_caller_live_r4;
                    }
                    if (g_j9_caller_live_r6 && (g_j9_caller_live_r6 >= 0x0071E000u)
                        && (g_j9_caller_live_r6 < 0x00720000u)) {
                        cr6 = g_j9_caller_live_r6 & ~3u;
                        csp = g_j9_caller_live_r7 ? g_j9_caller_live_r7 : (cr6 - 4u);
                    }
                    if (!cr4 && g_j9_wrap_t0 && j9_mapped32(pr, g_j9_wrap_t0 + 4u)) {
                        const address cp = j9_read32(pr, g_j9_wrap_t0 + 4u);
                        if (cp_ok(cp)) {
                            cr4 = cp;
                        }
                    }
                    if (!cr6) {
                        cr6 = g_j9_wrap_r6 ? (g_j9_wrap_r6 & ~3u)
                            : (g_j9_wrap_t2 ? (g_j9_wrap_t2 & ~3u) : 0);
                    }
                    if (!csp) {
                        csp = g_j9_wrap_r7 ? g_j9_wrap_r7
                            : (cr6 ? (cr6 - 4u) : osp);
                    }
                    if (cr4) {
                        g_j9_saved_r4 = cr4;
                    }
                    g_j9_saved_r5 = g_j9_wrap_t1;
                    if (cr6) {
                        g_j9_saved_r6 = cr6;
                    }
                    if (inst && (inst >= 0x02D10000u) && (inst < 0x02D20000u)
                        && csp && j9_mapped32(pr, csp)) {
                        j9_write32(pr, csp, inst);
                        g_j9_java_sp = csp;
                    }
                    g_j9_resume_at = g_j9_wrap_t1 + 3u;
                    g_j9_resume_no_ac = true;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] throw-return-caller pc=0x{:X} -> 0x{:X} "
                        "r4=0x{:X} r6=0x{:X} inst=0x{:X} csp=0x{:X} "
                        "cr=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                        invoke_pc, g_j9_resume_at, g_j9_saved_r4,
                        g_j9_saved_r6, inst, g_j9_java_sp,
                        g_j9_caller_live_r4, g_j9_caller_live_r5,
                        g_j9_caller_live_r6, g_j9_caller_live_r7);
                }
                ++g_j9_throw_skips;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] throw-resume-jcl pc=0x{:X} -> 0x{:X} jsp=0x{:X} "
                    "osp=0x{:X} meth=0x{:X} send=0x{:X} r4=0x{:X} cp4=0x{:X} "
                    "lm4=0x{:X} l0=0x{:X} live=0x{:X}/0x{:X}/0x{:X}/0x{:X} "
                    "next={:02X}{:02X}{:02X}{:02X}",
                    invoke_pc, g_j9_resume_at, fp, osp, meth, send, r4,
                    r4 ? j9_read32(pr, r4 + 4u) : 0, s_m4,
                    fp ? j9_read32(pr, fp) : 0,
                    g_j9_live_r4, g_j9_live_r5, g_j9_live_r6, g_j9_live_r7,
                    j9_read8(pr, g_j9_resume_at),
                    j9_read8(pr, g_j9_resume_at + 1u),
                    j9_read8(pr, g_j9_resume_at + 2u),
                    j9_read8(pr, g_j9_resume_at + 3u));
                j9_note_outer_jcl(pr, r4, g_j9_resume_at, fp, osp);
                j9_jxe_resume_interp(core, pr, meth, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            if ((g_j9_encoding_n >= 3) && sys_inv && !g_j9_boot_returned
                && (g_j9_throw_skips < 40)
                && ((send == 0x81911DFCu) || (send == g_j9_proplist_bkpt)
                    || (extra == 1u) || (meth == 0x727E20u)
                    || !j9_mapped32(pr, meth + 12u)
                    || ((meth < 0x00725000u) || (meth >= 0x0072C000u)))) {
                const std::uint8_t iop = j9_read8(pr, invoke_pc);
                const std::uint8_t i1 = j9_read8(pr, invoke_pc + 1u);
                const bool is_put = (iop == 0xB6u) && (i1 == 0x36u);
                const bool is_get = (iop == 0xB6u) && (i1 == 0x2Du);
                const bool next_pop = (j9_read8(pr, invoke_pc + 3u) == 0x57u);
                if (is_put) {
                    j9_fill_system_properties(pr);
                }
                const address arr = (iop == 0xB8u) ? j9_ensure_proplist(pr) : 0;
                core->set_reg(0, arr);
                address frame = g_j9_system_frame;
                if (!frame && jsp && j9_in_system_init(j9_read32(pr, jsp + 12u))) {
                    frame = jsp + 8u;
                    g_j9_system_frame = frame;
                } else if (!frame && jsp && g_j9_system_r4
                    && (j9_read32(pr, jsp + 8u) == g_j9_system_r4)) {
                    frame = jsp + 8u;
                    g_j9_system_frame = frame;
                }
                address sp = jsp;
                if (is_put && next_pop && frame) {
                    g_j9_resume_at = invoke_pc + 4u;
                    g_j9_java_sp = frame;
                    sp = frame;
                    if (invoke_pc >= 0x8195D7F0u) {
                        g_j9_init_tail = true;
                        char hex[96];
                        static const char *khex = "0123456789ABCDEF";
                        unsigned n = 0;
                        for (unsigned i = 0; (i < 40u) && (n + 2u < sizeof(hex)); ++i) {
                            const std::uint8_t b = j9_read8(pr, invoke_pc + 4u + i);
                            hex[n++] = khex[b >> 4];
                            hex[n++] = khex[b & 0xFu];
                            if (!g_j9_init_return
                                && ((b == 0xB1u) || (b == 0xB0u) || (b == 0xACu))) {
                                g_j9_init_return = invoke_pc + 4u + i;
                            }
                        }
                        hex[n] = 0;
                        if (g_j9_init_caller_r7) {
                            g_j9_resume_at = 0x819805F4u;
                            g_j9_saved_r4 = g_j9_init_caller_r4;
                            g_j9_saved_r6 = g_j9_init_caller_r6;
                            g_j9_java_sp = g_j9_init_caller_r7;
                            sp = g_j9_init_caller_r7;
                            core->set_reg(4, g_j9_init_caller_r4);
                            core->set_reg(6, g_j9_init_caller_r6);
                        } else if (g_j9_init_return) {
                            g_j9_resume_at = g_j9_init_return;
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] init-tail pc=0x{:X} next={} ret=0x{:X} "
                            "cr4=0x{:X} cr6=0x{:X} cr7=0x{:X}",
                            invoke_pc, hex, g_j9_resume_at,
                            g_j9_init_caller_r4, g_j9_init_caller_r6,
                            g_j9_init_caller_r7);
                    }
                } else if (g_j9_init_tail && g_j9_init_return
                    && (invoke_pc < 0x8195D7F0u) && frame) {
                    g_j9_resume_at = g_j9_init_return;
                    g_j9_java_sp = frame;
                    sp = frame;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] init-reenter skip pc=0x{:X} -> ret=0x{:X}",
                        invoke_pc, g_j9_init_return);
                } else if (is_get && frame) {
                    const address f4 = j9_mapped32(pr, frame - 4u)
                        ? j9_read32(pr, frame - 4u) : 0;
                    const address f8 = j9_mapped32(pr, frame - 8u)
                        ? j9_read32(pr, frame - 8u) : 0;
                    const address f12 = j9_mapped32(pr, frame - 12u)
                        ? j9_read32(pr, frame - 12u) : 0;
                    const auto is_ht = [&](const address p) {
                        return p && j9_mapped32(pr, p)
                            && ((p == 0x2D103B4u) || (j9_read32(pr, p) == 0x727CD0u));
                    };
                    const auto is_str = [&](const address p) {
                        return p && g_j9_string_clazz && j9_mapped32(pr, p)
                            && (j9_read32(pr, p) == g_j9_string_clazz);
                    };
                    address key = 0;
                    if (is_ht(f4) && is_str(f8)) {
                        key = f8;
                    } else if (is_str(f8)) {
                        key = f8;
                    } else if (is_str(f4)) {
                        key = f4;
                    } else if (is_str(f12)) {
                        key = f12;
                    }
                    const address val = j9_hashtable_get(pr, 0x2D103B4u, key);
                    sp = frame - 4u;
                    j9_write32(pr, sp, val);
                    g_j9_java_sp = sp;
                    char kbuf[80];
                    char vbuf[80];
                    j9_string_text(pr, key, kbuf, sizeof(kbuf));
                    j9_string_text(pr, val, vbuf, sizeof(vbuf));
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ht-get key=0x{:X} '{}' val=0x{:X} '{}' pc=0x{:X} "
                        "f-4=0x{:X}/0x{:X} f-8=0x{:X}/0x{:X} f-12=0x{:X} "
                        "kw=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                        key, kbuf, val, vbuf, invoke_pc,
                        f4, f4 && j9_mapped32(pr, f4) ? j9_read32(pr, f4) : 0,
                        f8, f8 && j9_mapped32(pr, f8) ? j9_read32(pr, f8) : 0,
                        f12,
                        key ? j9_read32(pr, key) : 0,
                        key ? j9_read32(pr, key + 4u) : 0,
                        key ? j9_read32(pr, key + 8u) : 0,
                        key ? j9_read32(pr, key + 12u) : 0,
                        key ? j9_read32(pr, key + 16u) : 0,
                        key ? j9_read32(pr, key + 20u) : 0);
                } else if (iop == 0xB8u) {
                    if (sp && j9_mapped32(pr, sp - 4u)) {
                        sp -= 4u;
                        j9_write32(pr, sp, arr);
                        g_j9_java_sp = sp;
                    }
                } else if (frame) {
                    sp = frame - 4u;
                    j9_write32(pr, sp, 0);
                    g_j9_java_sp = sp;
                }
                ++g_j9_throw_skips;
                g_j9_last_skip_pc = invoke_pc;
                const bool leave_init = g_j9_init_tail && g_j9_init_caller_r7
                    && (g_j9_resume_at == 0x819805F4u);
                if (!leave_init) {
                    if (g_j9_system_r4) {
                        g_j9_saved_r4 = g_j9_system_r4;
                        core->set_reg(4, g_j9_system_r4);
                    } else if (g_j9_system_clazz) {
                        g_j9_saved_r4 = g_j9_system_clazz;
                        core->set_reg(4, g_j9_system_clazz);
                    }
                    if (g_j9_system_r6) {
                        g_j9_saved_r6 = g_j9_system_r6;
                        core->set_reg(6, g_j9_system_r6);
                    }
                    g_j9_saved_r5 = invoke_pc;
                } else {
                    g_j9_saved_r5 = 0x819805F1u;
                }
                g_j9_saved_r2 = k_j9_opcode_table;
                g_j9_resume_no_ac = true;
                const address rom = (meth && j9_mapped32(pr, meth))
                    ? j9_read32(pr, meth) : 0;
                if (g_j9_throw_skips <= 16) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] throw-resume arr=0x{:X} ac=0x{:X} pc=0x{:X} jsp=0x{:X} "
                    "op={:02X}{:02X}{:02X}{:02X} r4=0x{:X} r6=0x{:X} "
                    "bc={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X} "
                    "rom=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} "
                    "strc=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                    arr, g_j9_string_array_clazz, invoke_pc, sp,
                    j9_read8(pr, invoke_pc), j9_read8(pr, invoke_pc + 1u),
                    j9_read8(pr, invoke_pc + 2u), j9_read8(pr, invoke_pc + 3u),
                    g_j9_saved_r4, g_j9_saved_r6,
                    j9_read8(pr, invoke_pc - 8u), j9_read8(pr, invoke_pc - 7u),
                    j9_read8(pr, invoke_pc - 6u), j9_read8(pr, invoke_pc - 5u),
                    j9_read8(pr, invoke_pc - 4u), j9_read8(pr, invoke_pc - 3u),
                    j9_read8(pr, invoke_pc - 2u), j9_read8(pr, invoke_pc - 1u),
                    rom ? j9_read32(pr, rom) : 0, rom ? j9_read32(pr, rom + 4u) : 0,
                    rom ? j9_read32(pr, rom + 8u) : 0, rom ? j9_read32(pr, rom + 12u) : 0,
                    rom ? j9_read32(pr, rom + 16u) : 0, rom ? j9_read32(pr, rom + 20u) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x14u) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x18u) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x1Cu) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x20u) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x24u) : 0,
                    g_j9_string_clazz ? j9_read32(pr, g_j9_string_clazz + 0x28u) : 0);
                }
                j9_jxe_resume_interp(core, pr,
                    g_j9_system_method ? g_j9_system_method : meth, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            // Re-entering the BKPT address loops forever (hook is on the VA).
            if (invoke_pc && j9_looks_bytecode_pc(pr, invoke_pc)) {
                g_j9_saved_r5 = invoke_pc;
                g_j9_saved_r2 = k_j9_opcode_table;
                g_j9_resume_no_ac = true;
                j9_jxe_resume_interp(core, pr,
                    g_j9_system_method ? g_j9_system_method : meth, 0u);
                g_j9_resume_no_ac = false;
                return;
            }
            j9_set_pc(core, (g_j9_throw_bkpt + 2u) | 1u);
            return;
        }
        if (pr && g_j9_newstr_bkpt && (pc == g_j9_newstr_bkpt)) {
            const address utf = core->get_reg(1);
            char buf[128];
            buf[0] = 0;
            if (const char *s = reinterpret_cast<const char *>(pr->get_ptr_on_addr_space(utf))) {
                std::size_t n = 0;
                while ((n < 127u) && s[n]) {
                    buf[n] = s[n];
                    ++n;
                }
                buf[n] = 0;
            }
            const address str = j9_new_string_utf(pr, buf[0] ? buf : "ISO-8859-1");
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] NewStringUTF utf='{}' @0x{:X} -> 0x{:X} sclazz=0x{:X} aclazz=0x{:X}",
                buf, utf, str, g_j9_string_clazz, g_j9_char_array_clazz);
            core->set_reg(0, str);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && g_j9_getstrutf_bkpt && (pc == g_j9_getstrutf_bkpt)) {
            const address jstr = core->get_reg(1);
            const address iscopy = core->get_reg(2);
            address utf = j9_string_to_utf8_buf(pr, jstr);
            if (!utf) {
                utf = g_j9_utf_stash;
            }
            if (!utf) {
                utf = j9_copy_cstr_buf(pr, "");
            }
            if (iscopy && j9_mapped32(pr, iscopy)) {
                j9_write32(pr, iscopy, 1);
            }
            char peek[80];
            peek[0] = 0;
            j9_read_cstr(pr, utf, peek, sizeof(peek));
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] GetStringUTFChars jstr=0x{:X} -> 0x{:X} '{}'",
                jstr, utf, peek);
            core->set_reg(0, utf);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && ((g_j9_cp_stub_bkpt && (pc == g_j9_cp_stub_bkpt))
                || (g_j9_walk_va && (pc == (g_j9_walk_va + 0x320u))))) {
            j9_run_main_cp_stub(core, pr);
            return;
        }
        if (pr && g_j9_lcdui_chain_bkpt && (pc == g_j9_lcdui_chain_bkpt)) {
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] lcdui-chain phase={} disp=0x{:X} cv=0x{:X} tk=0x{:X}",
                g_j9_alps_phase, g_j9_display_obj, g_j9_canvas_obj, g_j9_toolkit_obj);
            if (g_j9_alps_phase == 4) {
                g_j9_alps_phase = 5;
                const address env = g_j9_fake_env ? g_j9_fake_env
                    : (g_j9_vmthread ? g_j9_vmthread : 0x714E00u);
                core->set_reg(0, env);
                core->set_reg(1, g_j9_display_obj);
                core->set_reg(2, g_j9_canvas_obj);
                core->set_reg(3, 0);
                core->set_lr(g_j9_park_pc);
                j9_set_pc(core, 0x81AF2C12u | 1u);
                return;
            }
            if (g_j9_park_pc) {
                j9_set_pc(core, g_j9_park_pc);
            }
            return;
        }
        if (pr && g_j9_callstatic_bkpt && (pc == g_j9_callstatic_bkpt)) {
            const address clazz = core->get_reg(1);
            const address jid = core->get_reg(2);
            const address args = core->get_reg(3);
            address meth = jid;
            if (jid && j9_mapped32(pr, jid)) {
                const address inner = j9_read32(pr, jid);
                if (inner && j9_mapped32(pr, inner + 8u)) {
                    meth = inner;
                }
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] CallStatic clazz=0x{:X} jid=0x{:X} meth=0x{:X} args=0x{:X} "
                "bc=0x{:X} send=0x{:X}",
                clazz, jid, meth, args,
                meth ? j9_read32(pr, meth) : 0,
                meth ? j9_read32(pr, meth + 8u) : 0);
            j9_prepare_jni_vt(pr);
            if (g_j9_vmthread && args && j9_mapped32(pr, g_j9_vmthread + 0x10u)) {
                const address jsp = j9_read32(pr, g_j9_vmthread + 0x10u);
                if (jsp && j9_mapped32(pr, jsp)) {
                    j9_write32(pr, jsp, args);
                    g_j9_java_sp = jsp;
                }
            }
            g_j9_saved_r4 = clazz ? clazz : g_j9_saved_r4;
            g_j9_resume_at = meth ? j9_read32(pr, meth) : 0;
            g_j9_resume_no_ac = true;
            j9_jxe_resume_interp(core, pr, meth, 1u);
            g_j9_resume_no_ac = false;
            g_j9_resume_at = 0;
            return;
        }
        if (pr && g_j9_newglobal_bkpt && (pc == g_j9_newglobal_bkpt)) {
            const address obj = core->get_reg(1);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] NewGlobalRef obj=0x{:X}", obj);
            j9_prepare_jni_vt(pr);
            core->set_reg(0, obj);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && g_j9_newobjarr_bkpt && (pc == g_j9_newobjarr_bkpt)) {
            const unsigned n = core->get_reg(1);
            const address elem = core->get_reg(2);
            const unsigned use = (n > 16u) ? 0u : n;
            const address arr = j9_new_string_array(pr, nullptr, use);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] NewObjectArray n={} elem=0x{:X} -> 0x{:X}",
                n, elem, arr);
            j9_prepare_jni_vt(pr);
            core->set_reg(0, arr);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && g_j9_getmethod_bkpt && (pc == g_j9_getmethod_bkpt)) {
            const address clazz = core->get_reg(1);
            const address namep = core->get_reg(2);
            const address sigp = core->get_reg(3);
            char nbuf[80];
            char sbuf[80];
            nbuf[0] = 0;
            sbuf[0] = 0;
            j9_read_cstr(pr, namep, nbuf, sizeof(nbuf));
            j9_read_cstr(pr, sigp, sbuf, sizeof(sbuf));
            address meth = 0;
            if (std::strcmp(nbuf, "main") == 0) {
                meth = j9_make_main_method(pr);
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] GetMethodID clazz=0x{:X} '{}' '{}' -> 0x{:X} mainc=0x{:X}",
                clazz, nbuf, sbuf, meth, g_j9_main_clazz);
            j9_prepare_jni_vt(pr);
            if ((std::strcmp(nbuf, "main") == 0) && meth && g_j9_main_clazz) {
                const address sp = core->get_reg(13);
                const address args = (sp && j9_mapped32(pr, sp + 0x20u))
                    ? j9_read32(pr, sp + 0x20u) : 0;
                address inner = meth;
                if (j9_mapped32(pr, meth)) {
                    const address p = j9_read32(pr, meth);
                    if (p && j9_mapped32(pr, p + 8u)) {
                        inner = p;
                    }
                }
                g_j9_main_ret_sp = core->get_reg(13);
                g_j9_main_ret_lr = core->get_lr();
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] CallStatic-direct clazz=0x{:X} jid=0x{:X} meth=0x{:X} "
                    "args=0x{:X} bc=0x{:X} send=0x{:X} ret=0x{:X}/0x{:X}",
                    g_j9_main_clazz, meth, inner, args,
                    inner ? j9_read32(pr, inner) : 0,
                    inner ? j9_read32(pr, inner + 8u) : 0,
                    g_j9_main_ret_sp, g_j9_main_ret_lr);
                address fp = g_j9_walk_va ? (g_j9_walk_va + 0x4100u) : 0;
                if (fp && j9_mapped32(pr, fp + 24u)) {
                    j9_write32(pr, fp, args);
                    j9_write32(pr, fp + 4u, g_j9_main_clazz);
                    j9_write32(pr, fp + 8u, 0x818DE518u);
                    j9_write32(pr, fp + 12u, 0x71E548u);
                    g_j9_saved_r6 = fp;
                    g_j9_java_sp = fp;
                    if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 0x10u)) {
                        j9_write32(pr, g_j9_vmthread + 0x10u, fp);
                    }
                } else if (g_j9_vmthread && args
                    && j9_mapped32(pr, g_j9_vmthread + 0x10u)) {
                    const address jsp = j9_read32(pr, g_j9_vmthread + 0x10u);
                    if (jsp && j9_mapped32(pr, jsp)) {
                        j9_write32(pr, jsp, args);
                        g_j9_java_sp = jsp;
                    }
                }
                g_j9_saved_r4 = g_j9_main_clazz;
                g_j9_saved_r5 = inner ? j9_read32(pr, inner) : 0;
                g_j9_resume_at = g_j9_saved_r5;
                g_j9_resume_no_ac = true;
                j9_jxe_resume_interp(core, pr, inner, 1u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            core->set_reg(0, meth);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pr && g_j9_findclass_ret_bkpt && (pc == g_j9_findclass_ret_bkpt)) {
            const address ret = core->get_reg(0);
            char nbuf[96];
            nbuf[0] = 0;
            if (ret && j9_mapped32(pr, ret + 0x38u)) {
                j9_class_name(pr, ret, nbuf, sizeof(nbuf));
            }
            j9_prepare_jni_vt(pr);
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] FindClass-rom-ret 0x{:X} '{}' vt=0x{:X}/0x{:X}/0x{:X} lr=0x{:X} phase={}",
                ret, nbuf, g_j9_vt10_c, g_j9_vt14_c, g_j9_vt18_c,
                g_j9_findclass_saved_lr, g_j9_alps_phase);
            if (g_j9_alps_phase == 1) {
                g_j9_alps_clazz = ret;
                if (ret && j9_host_start_alps_init(core, pr, ret)) {
                    g_j9_findclass_saved_lr = 0;
                    return;
                }
                g_j9_alps_phase = 3;
                g_j9_saved_r4 = g_j9_main_clazz;
                g_j9_resume_at = 0x81AA8196u;
                g_j9_resume_no_ac = true;
                address inner = g_j9_main_method;
                if (inner && j9_mapped32(pr, inner)) {
                    const address p = j9_read32(pr, inner);
                    if (p && j9_mapped32(pr, p + 8u)) {
                        inner = p;
                    }
                }
                j9_jxe_resume_interp(core, pr, inner, 0u);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                g_j9_findclass_saved_lr = 0;
                return;
            }
            core->set_reg(0, ret);
            j9_set_pc(core, g_j9_findclass_saved_lr ? g_j9_findclass_saved_lr
                : core->get_lr());
            g_j9_findclass_saved_lr = 0;
            return;
        }
        if (pr && g_j9_findclass_bkpt && (pc == g_j9_findclass_bkpt)) {
            const address namep = core->get_reg(1);
            char nbuf[160];
            nbuf[0] = 0;
            j9_read_cstr(pr, namep, nbuf, sizeof(nbuf));
            address clazz = nbuf[0] ? j9_find_class_by_name(pr, nbuf) : 0;
            const address rom = (!clazz && nbuf[0]) ? j9_find_rom_class_by_name(pr, nbuf) : 0;
            if (!clazz && rom) {
                clazz = j9_ram_class_for_rom(pr, rom);
                if (!clazz) {
                    clazz = j9_make_ram_class(pr, rom);
                }
            }
            address vm = 0;
            address cl250 = 0;
            address cl58 = 0;
            if (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread + 4u)) {
                vm = j9_read32(pr, g_j9_vmthread + 4u);
                if (vm && j9_mapped32(pr, vm + 0x250u)) {
                    cl250 = j9_read32(pr, vm + 0x250u);
                    cl58 = j9_read32(pr, vm + 0x58u);
                }
            }
            address inst = 0;
            if (clazz && j9_mapped32(pr, clazz + 0x38u)) {
                inst = j9_read32(pr, clazz + 0x38u);
            }
            const address ret = clazz;
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] FindClass '{}' -> clazz=0x{:X} rom=0x{:X} inst=0x{:X} ret=0x{:X} "
                "vm=0x{:X} cl=0x{:X}/0x{:X} vt10=0x{:X} vt18=0x{:X}",
                nbuf, clazz, rom, inst, ret, vm, cl250, cl58,
                g_j9_vmthread ? j9_read32(pr, g_j9_vmthread + 0x10u) : 0,
                g_j9_vmthread ? j9_read32(pr, g_j9_vmthread + 0x18u) : 0);
            if (ret) {
                if (std::strstr(nbuf, "runtimeV2/Main")) {
                    g_j9_main_clazz = ret;
                    j9_make_main_method(pr);
                    const address sp = core->get_reg(13);
                    const address strc = g_j9_string_clazz
                        ? g_j9_string_clazz : 0x726570u;
                    const address args = j9_new_string_array(pr, nullptr, 0);
                    if (sp && j9_mapped32(pr, sp + 0x40u)) {
                        j9_write32(pr, sp + 0x18u, strc);
                        j9_write32(pr, sp + 0x20u, args);
                        j9_write32(pr, sp + 0x28u, ret);
                        j9_write32(pr, sp + 0x40u, 0);
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] plant-main-launch clazz=0x{:X} meth=0x{:X} "
                        "args=0x{:X} strc=0x{:X} sp=0x{:X} r5=0x{:X}",
                        ret, g_j9_main_method, args, strc, sp,
                        core->get_reg(5));
                    j9_prepare_jni_vt(pr);
                    core->set_reg(0, ret);
                    // Skip NewObjectArray (tbl[172] is junk on this frame).
                    // 0x818BD4DE = GetStaticMethodID(clazz, "main", "([L...;)V").
                    j9_set_pc(core, 0x818BD4DFu);
                    return;
                }
                j9_prepare_jni_vt(pr);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] FindClass-ret lr=0x{:X} r4=0x{:X} r5=0x{:X} sp=0x{:X}",
                    core->get_lr(), core->get_reg(4), core->get_reg(5),
                    core->get_reg(13));
                core->set_reg(0, ret);
                j9_set_pc(core, core->get_lr());
                return;
            }
            // Exploded suite .class files (AlpsFarm*) must not go through
            // official classfile→ROM: a bad heap grow previously jumped to
            // heap-end and AVd. Host LCDUI kick owns that path.
            if (nbuf[0] && std::strstr(nbuf, "AlpsFarm")) {
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] FindClass-skip-classfile '{}'", nbuf);
                j9_prepare_jni_vt(pr);
                core->set_reg(0, g_j9_alps_clazz);
                j9_set_pc(core, core->get_lr());
                return;
            }
            // Host miss: let ROM FindClass walk the loaded JXE tables.
            // Restore interpret() SP/class after it returns.
            j9_prepare_findclass_vt(pr);
            g_j9_findclass_saved_lr = core->get_lr();
            if (g_j9_findclass_ret_bkpt) {
                core->set_lr(g_j9_findclass_ret_bkpt);
            }
            j9_set_pc(core, 0x818D93C8u);
            return;
        }
        if (pr && ((pc == k_j9_alloc_object) || (pc == k_j9_alloc_indexable))) {
            const address vm = core->get_reg(0);
            const address clazz = core->get_reg(1);
            const address a2 = core->get_reg(2);
            const address a3 = core->get_reg(3);
            char nbuf[96];
            nbuf[0] = 0;
            if (clazz && j9_mapped32(pr, clazz + 0x38u)) {
                j9_class_name(pr, clazz, nbuf, sizeof(nbuf));
            }
            const address ir5 = core->get_reg(5);
            if ((ir5 >= 0x8195D600u) && (ir5 < 0x8195D800u)) {
                g_j9_system_r4 = core->get_reg(4);
                g_j9_system_r5 = ir5;
                g_j9_system_r6 = core->get_reg(6);
                g_j9_system_r7 = core->get_reg(7);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] system-init-regs r4=0x{:X} r5=0x{:X} r6=0x{:X} r7=0x{:X}",
                    g_j9_system_r4, g_j9_system_r5, g_j9_system_r6, g_j9_system_r7);
            }
            if (nbuf[0] && (std::strcmp(nbuf, "java/lang/String") == 0)) {
                g_j9_string_clazz = clazz;
            } else if ((nbuf[0] == '[') && (nbuf[1] == 'C')) {
                g_j9_char_array_clazz = clazz;
            } else if (!nbuf[0] && (pc == k_j9_alloc_indexable) && g_j9_string_clazz
                && !g_j9_char_array_clazz && (a2 > 0u) && (a2 < 0x200u)) {
                g_j9_char_array_clazz = clazz;
            }
            if (nbuf[0] && std::strstr(nbuf, "IncompatibleClassChangeError")) {
                const address r5 = core->get_reg(5);
                const address r6 = core->get_reg(6);
                const address r7 = core->get_reg(7);
                const address send = j9_mapped32(pr, r7 + 8u) ? j9_read32(pr, r7 + 8u) : 0;
                const char *mn = j9_jcl_name_for_fn(send);
                const address s0 = j9_read32(pr, r7);
                char raw[48];
                raw[0] = 0;
                if (const char *s = reinterpret_cast<const char *>(
                        pr->get_ptr_on_addr_space(s0))) {
                    std::size_t i = 0;
                    while ((i < 47u) && s[i] && (static_cast<unsigned char>(s[i]) >= 0x20)
                        && (static_cast<unsigned char>(s[i]) < 0x7F)) {
                        raw[i] = s[i];
                        ++i;
                    }
                    raw[i] = 0;
                }
                const address vt = g_j9_vmthread ? g_j9_vmthread : core->get_reg(8);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] icce-dump r4=0x{:X} r5=0x{:X} r6=0x{:X} r7=0x{:X} r8=0x{:X} "
                    "lr=0x{:X} send=0x{:X} name='{}' utf='{}' sv5=0x{:X} "
                    "vt10=0x{:X} vt14=0x{:X} vt18=0x{:X} "
                    "s0=0x{:X} s1=0x{:X} s2=0x{:X} s3=0x{:X}",
                    core->get_reg(4), r5, r6, r7, core->get_reg(8), core->get_lr(),
                    send, mn ? mn : "?", raw, g_j9_saved_r5,
                    vt ? j9_read32(pr, vt + 0x10u) : 0,
                    vt ? j9_read32(pr, vt + 0x14u) : 0,
                    vt ? j9_read32(pr, vt + 0x18u) : 0,
                    s0, j9_read32(pr, r7 + 4u),
                    j9_read32(pr, r7 + 8u), j9_read32(pr, r7 + 12u));
            }
            static int ao_logs = 0;
            if ((ao_logs < 24) || (g_j9_encoding_n > 0 && ao_logs < 48)) {
                ++ao_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] {} vm=0x{:X} clazz=0x{:X} name='{}' a2=0x{:X} a3=0x{:X} "
                    "inst=0x{:X} lr=0x{:X} r5=0x{:X} sv5=0x{:X}",
                    (pc == k_j9_alloc_object) ? "alloc-obj" : "alloc-idx",
                    vm, clazz, nbuf[0] ? nbuf : "?", a2, a3,
                    (clazz && j9_mapped32(pr, clazz + 0x38u)) ? j9_read32(pr, clazz + 0x38u) : 0,
                    core->get_lr(), core->get_reg(5), g_j9_saved_r5);
            }
            // Original insn is `push {r0-r7,lr}` (0xB5FF). Replay it and
            // continue at the following `sub sp`.
            const address sp = core->get_reg(13) - 36u;
            for (int i = 0; i < 8; ++i) {
                j9_write32(pr, sp + static_cast<address>(i) * 4u, core->get_reg(i));
            }
            j9_write32(pr, sp + 32u, core->get_lr());
            core->set_reg(13, sp);
            j9_set_pc(core, (pc + 2u) | 1u);
            return;
        }
        if (pr && (pc == k_j9_alloc_memory)) {
            const address desc = core->get_reg(0);
            const address vm = core->get_reg(1);
            unsigned nbytes = desc ? j9_read32(pr, desc) : 0x80u;
            if (nbytes < 0x10u) {
                nbytes = 0x10u;
            }
            if (nbytes > 0x200000u) {
                nbytes = 0x200000u;
            }
            const address mm = vm ? j9_read32(pr, vm + 4u) : 0;
            const address heap = mm ? j9_read32(pr, mm + 0x24u) : 0;
            j9_wire_official_heap(pr, heap, vm);
            j9_grow_official_heap(pr, nbytes + 0x1000u);
            const address hend = heap ? j9_read32(pr, heap + 0x18u) : 0;
            const address hcur = heap ? j9_read32(pr, heap + 0x1cu) : 0;
            const bool official_ok = heap && (hend > hcur) && ((hend - hcur) >= nbytes)
                && j9_mapped32(pr, hcur) && j9_mapped32(pr, hend - 4u);
            static int alloc_logs = 0;
            if (alloc_logs < 24) {
                ++alloc_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] host-mem n=0x{:X} desc=0x{:X} vm=0x{:X} heap=0x{:X} end=0x{:X} cur=0x{:X} official={} lr=0x{:X}",
                    nbytes, desc, vm, heap, hend, hcur, official_ok ? 1 : 0,
                    core->get_lr());
            }
            if (official_ok) {
                const address sp = core->get_reg(13) - 24u;
                j9_write32(pr, sp + 0u, core->get_reg(3));
                j9_write32(pr, sp + 4u, core->get_reg(4));
                j9_write32(pr, sp + 8u, core->get_reg(5));
                j9_write32(pr, sp + 12u, core->get_reg(6));
                j9_write32(pr, sp + 16u, core->get_reg(7));
                j9_write32(pr, sp + 20u, core->get_lr());
                core->set_reg(13, sp);
                j9_set_pc(core, 0x81912A4Fu);
                return;
            }
            const address obj = j9_host_alloc_obj(pr, 0, nbytes);
            core->set_reg(0, obj);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (pc == (k_j9_loadjxe_result & ~1u)) {
            address img = core->get_reg(0);
            const address fallback = g_j9_jcl_jxe_va;
            std::uint32_t magic = 0;
            if (pr && img) {
                if (const auto *m = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(img))) {
                    magic = *m;
                }
            }
            if (!img && fallback) {
                img = fallback;
                core->set_reg(0, img);
                if (pr) {
                    if (const auto *m = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(img))) {
                        magic = *m;
                    }
                }
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] iveLoadJxeFromFile image=0x{:X} fallback=0x{:X} magic=0x{:08X} from {}",
                img, fallback, magic, pr ? pr->name() : "?");
            std::uint32_t cpsr = core->get_cpsr();
            if (img) {
                cpsr &= ~(1u << 30); // Z=0 → bne success
            } else {
                cpsr |= (1u << 30);
            }
            core->set_cpsr(cpsr);
            core->set_cpsr(core->get_cpsr() | 0x20u);
            core->set_pc(pc + 2);
            return;
        }
        if ((pc == (k_j9_dynload_jxe_parse & ~1u)) || (pc == (k_j9_dynload_findjar & ~1u))) {
            const address r0 = core->get_reg(0);
            const address sp = core->get_reg(13);
            address out = 0;
            std::uint32_t magic = 0;
            if (pr && (pc == (k_j9_dynload_findjar & ~1u))) {
                if (const auto *slot = reinterpret_cast<const address *>(pr->get_ptr_on_addr_space(sp + 0x1c))) {
                    out = *slot;
                }
            }
            const address peek = (pc == (k_j9_dynload_jxe_parse & ~1u)) ? r0 : out;
            if (pr && peek) {
                if (const auto *m = reinterpret_cast<const std::uint32_t *>(pr->get_ptr_on_addr_space(peek))) {
                    magic = *m;
                }
            }
            LOG_WARN(EMULATED_STDOUT,
                "[j9-nf] dynload-{} r0=0x{:X} out=0x{:X} magic=0x{:08X} from {}",
                (pc == (k_j9_dynload_jxe_parse & ~1u)) ? "jxe" : "findjar",
                r0, out, magic, pr ? pr->name() : "?");
            std::uint32_t cpsr = core->get_cpsr();
            if (r0) {
                cpsr &= ~(1u << 30);
            } else {
                cpsr |= (1u << 30);
            }
            core->set_cpsr(cpsr);
            core->set_cpsr(core->get_cpsr() | 0x20u);
            core->set_pc(pc + 2);
            return;
        }
        if (pc == (k_j9_method_run_ld & ~1u)) {
            const address method = core->get_reg(1);
            const address bc = j9_read32(pr, method);
            static int method_run_logs = 0;
            if (method_run_logs < 12) {
                ++method_run_logs;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] method-run method=0x{:X} bytecodes=0x{:X} from {}",
                    method, bc, pr ? pr->name() : "?");
            }
            if (bc < 0x1000u) {
                j9_write32(pr, method + 8u, 0);
                // push {r3-r7,lr} already ran; pop it via the function epilogue
                j9_set_pc(core, 0x818FD71Du);
                return;
            }
            core->set_reg(5, bc);
            core->set_cpsr(core->get_cpsr() | 0x20u);
            core->set_pc(pc + 2);
            return;
        }
        if (pc == (k_j9_verify_sig_ld & ~1u)) {
            const address r6 = core->get_reg(6);
            const address r3 = core->get_reg(3);
            const address at = r6 + r3;
            std::uint8_t ch = 0;
            bool ok = false;
            if (pr && at) {
                if (const auto *b = reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(at))) {
                    ch = *b;
                    ok = true;
                }
            }
            if (!ok) {
                static int sig_logs = 0;
                if (sig_logs < 8) {
                    ++sig_logs;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] verify-sig unmapped r6=0x{:X} r3=0x{:X} — skip opcode",
                        r6, r3);
                }
                // Join point after this opcode's signature walk. Treating
                // the byte as ')' still reads [r6+r3+1] at 0x81903DC2.
                j9_set_pc(core, 0x81903E37u);
                return;
            }
            core->set_reg(7, ch);
            core->set_cpsr(core->get_cpsr() | 0x20u);
            core->set_pc(pc + 2);
            return;
        }
        if (pc == k_j9_init_loop) {
            const address sp = core->get_sp();
            const address vm = (sp && j9_mapped32(pr, sp + 0x28u))
                ? j9_read32(pr, sp + 0x28u) : 0;
            const address vmth = (sp && j9_mapped32(pr, sp + 0x2cu))
                ? j9_read32(pr, sp + 0x2cu) : 0;
            const address r4 = core->get_reg(4);
            const address r10 = core->get_reg(10);
            const address st4 = (r4 && j9_mapped32(pr, r4 + 0x28u))
                ? j9_read32(pr, r4 + 0x28u) : 0;
            const address st10 = (r10 && j9_mapped32(pr, r10 + 0x28u))
                ? j9_read32(pr, r10 + 0x28u) : 0;
            const address ex = (vmth && j9_mapped32(pr, vmth + 0x64u))
                ? j9_read32(pr, vmth + 0x64u) : 0;
            static int init_loop_logs = 0;
            if (init_loop_logs < 16) {
                ++init_loop_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] init-loop sp=0x{:X} vm=0x{:X} vmth=0x{:X} "
                    "r4=0x{:X} st4=0x{:X} r10=0x{:X} st10=0x{:X} ex=0x{:X} "
                    "clr=0x{:X}",
                    sp, vm, vmth, r4, st4, r10, st10, ex,
                    (sp && j9_mapped32(pr, sp + 0x20u))
                        ? j9_read32(pr, sp + 0x20u) : 0);
            }
            core->set_lr(vmth);
            j9_set_pc(core, pc + 4u);
            return;
        }
        if (pc == k_j9_init_ret) {
            address lr = core->get_lr();
            static int init_ret_logs = 0;
            g_j9_init_returned = true;
            if (init_ret_logs < 8) {
                ++init_ret_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] init-ret lr=0x{:X} r0=0x{:X} sp=0x{:X} r4=0x{:X} r10=0x{:X}",
                    lr, core->get_reg(0), core->get_sp(),
                    core->get_reg(4), core->get_reg(10));
            }
            const bool lr_ok = lr && (lr >= 0x80000000u) && pr
                && pr->get_ptr_on_addr_space(lr & ~1u);
            if (!lr_ok) {
                static int init_ret_retry = 0;
                if (init_ret_retry < 1) {
                    ++init_ret_retry;
                    core->set_sp(core->get_sp() - 0x30u);
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] init-ret bad lr=0x{:X} — retry loop sp=0x{:X}",
                        lr, core->get_sp());
                    j9_set_pc(core, k_j9_init_loop);
                    return;
                }
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] init-ret bad lr=0x{:X} — stop", lr);
            }
            j9_set_pc(core, lr_ok ? lr : (pc + 4u));
            return;
        }
        if (pc == k_j9_current_thread) {
            const address r5 = core->get_reg(5) + 3u;
            const address r8 = core->get_reg(8);
            const address r2 = core->get_reg(2);
            address r7 = core->get_reg(7);
            const std::uint8_t ip = j9_read8(pr, r5);
            address obj = r8 ? j9_read32(pr, r8 + k_j9_thread_obj_off) : 0;
            if (!obj || !j9_mapped32(pr, obj)) {
                if (g_j9_thread_class) {
                    j9_plant_main_thread(pr, g_j9_thread_class);
                    obj = g_j9_thread_obj;
                }
            }
            r7 -= 4u;
            if (j9_mapped32(pr, r7)) {
                j9_write32(pr, r7, obj);
            }
            core->set_reg(5, r5);
            core->set_reg(7, r7);
            core->set_reg(11, obj);
            const address next = (r2 && j9_mapped32(pr, r2 + static_cast<address>(ip) * 4u))
                ? j9_read32(pr, r2 + static_cast<address>(ip) * 4u) : 0;
            static int ct_logs = 0;
            if (ct_logs < 8) {
                ++ct_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] currentThread obj=0x{:X} vt=0x{:X} next=0x{:X} op=0x{:02X} r5=0x{:X}",
                    obj, r8, next, ip, r5);
            }
            if (next) {
                j9_set_pc(core, next);
            } else {
                j9_set_pc(core, 0x818F5A7Cu);
            }
            return;
        }
        if ((pc == k_j9_monitorenter) || (pc == k_j9_monitorexit)) {
            const address r5 = core->get_reg(5);
            const address r7 = core->get_reg(7);
            const address r8 = core->get_reg(8);
            const bool is_enter = (pc == k_j9_monitorenter);
            const address obj = j9_read32(pr, r7);
            const address lock = obj ? j9_read32(pr, obj + 8u) : 0;
            const address flags = obj ? j9_read32(pr, obj + 4u) : 0;
            static int mon_logs = 0;
            if ((mon_logs < 24) || (r5 == 0x81980746u)) {
                ++mon_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] mon-{} obj=0x{:X} lock=0x{:X} flags=0x{:X} r8=0x{:X} r5=0x{:X} r7=0x{:X}",
                    is_enter ? "enter" : "exit", obj, lock, flags, r8, r5, r7);
            }
            if (!obj) {
                core->set_reg(0, 6);
                core->set_reg(1, 0);
                j9_set_pc(core, 0x818F5E78u);
                return;
            }
            if (is_enter) {
                if (!lock) {
                    j9_write32(pr, obj + 8u, r8);
                }
                core->set_reg(7, r7 + 4u);
            } else {
                // Fast-path unlock. Official also requires Class+4 bit31
                // clear; every Class object has it set, so the slow helper
                // runs and throws IMSE (JVMJ9VM022E). Owner match is enough.
                if (lock && (lock != r8)) {
                    static int mon_mismatch = 0;
                    if (mon_mismatch < 8) {
                        ++mon_mismatch;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] mon-exit mismatch obj=0x{:X} lock=0x{:X} r8=0x{:X} — force unlock",
                            obj, lock, r8);
                    }
                }
                j9_write32(pr, obj + 8u, 0);
                core->set_reg(7, r7 + 4u);
                if ((r5 == 0x81980746u) && g_j9_boot_returned
                    && g_j9_skip_init_clazz && (obj == g_j9_skip_init_clazz)) {
                    const address ifp = core->get_reg(6);
                    const address retpc = (ifp && j9_mapped32(pr, ifp + 24u))
                        ? j9_read32(pr, ifp + 24u) : 0;
                    const bool parent_bad = !j9_looks_bytecode_pc(pr, retpc);
                    const bool outer_ok = g_j9_outer_r5
                        && !j9_is_initialize_pc(g_j9_outer_r5)
                        && !j9_is_interpret_tail_pc(g_j9_outer_r5)
                        && (g_j9_outer_r6 >= 0x0071E000u)
                        && (g_j9_outer_r6 < 0x00720000u);
                    if (parent_bad && outer_ok) {
                        const address vmth = g_j9_vmthread ? g_j9_vmthread : r8;
                        if (vmth && j9_mapped32(pr, vmth + 0x6cu)) {
                            j9_write32(pr, vmth + 0x64u, 0);
                            j9_write32(pr, vmth + 0x6cu, 0);
                        }
                        g_j9_saved_r4 = g_j9_outer_r4;
                        g_j9_saved_r5 = g_j9_outer_r5;
                        g_j9_saved_r6 = g_j9_outer_r6;
                        g_j9_java_sp = g_j9_outer_r7;
                        g_j9_saved_r2 = k_j9_opcode_table;
                        g_j9_resume_at = g_j9_outer_r5;
                        g_j9_resume_no_ac = true;
                        g_j9_skip_init_clazz = 0;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] skip-init-ret-outer obj=0x{:X} -> 0x{:X} "
                            "r4=0x{:X} r6=0x{:X} r7=0x{:X} parent=0x{:X}",
                            obj, g_j9_resume_at, g_j9_saved_r4,
                            g_j9_saved_r6, g_j9_java_sp, retpc);
                        j9_jxe_resume_interp(core, pr, 0, 0u);
                        g_j9_resume_no_ac = false;
                        g_j9_resume_at = 0;
                        return;
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-init-keep obj=0x{:X} parent=0x{:X} "
                        "outer=0x{:X} bad={} ok={}",
                        obj, retpc, g_j9_outer_r5,
                        parent_bad ? 1 : 0, outer_ok ? 1 : 0);
                    g_j9_skip_init_clazz = 0;
                }
                if ((r5 == 0x81980746u) && g_j9_thread_class && (obj == g_j9_thread_class)
                    && !g_j9_thread_obj) {
                    j9_plant_main_thread(pr, g_j9_thread_class);
                }
                // initializeImpl tail: skip 2A/B7/AC/FF and return through
                // the wrapper that called interpret.
                if ((r5 == 0x81980746u) && g_j9_init_returned
                    && !g_j9_boot_returned
                    && obj && j9_class_is_jcl(pr, obj)) {
                    if (!g_j9_wrap_fp_ok) {
                        j9_try_wrap_scan(pr, core, obj);
                    }
                    if (!g_j9_sys_ok) {
                        j9_try_save_iframe(pr, core, obj);
                    }
                    if (!g_j9_wrap_clazz) {
                        g_j9_wrap_clazz = obj;
                    }
                }
                if ((r5 == 0x81980746u)
                    && !g_j9_boot_returned
                    && g_j9_sys_ok && g_j9_sys_sp && g_j9_init_returned
                    && g_j9_wrap_fp_ok && g_j9_wrap_clazz
                    && (obj == g_j9_wrap_clazz)) {
                    for (int i = 0; i < 96; ++i) {
                        j9_write32(pr, g_j9_sys_sp
                            + static_cast<address>(i) * 4u, g_j9_sys_frame[i]);
                    }
                    const address vmth = g_j9_vmthread ? g_j9_vmthread : r8;
                    if (vmth && j9_mapped32(pr, vmth + 0x6cu)) {
                        j9_write32(pr, vmth + 0x64u, 0);
                        j9_write32(pr, vmth + 0x6cu, 0);
                    }
                    const address iframe_op = g_j9_sys_frame[67];
                    const std::uint8_t wrap_op = g_j9_wrap_t1
                        ? j9_read8(pr, g_j9_wrap_t1) : 0;
                    // `new` (0xBB) interpret must resume at the allocator
                    // (0x8190E988), not the wrapper. Wrapper return skips
                    // allocation and leaves r0=Class for <init>.
                    if ((iframe_op == 0xBBu) || (g_j9_wrap_sp34 == 0xBBu)
                        || (wrap_op == 0xBBu)) {
                        if (g_j9_wrap_java_fp && vmth
                            && j9_mapped32(pr, vmth + 8u)) {
                            j9_write32(pr, vmth + 8u, g_j9_wrap_java_fp);
                        }
                        if (g_j9_wrap_java_fp
                            && j9_mapped32(pr, g_j9_wrap_java_fp)) {
                            j9_write32(pr, g_j9_wrap_java_fp - 8u, g_j9_wrap_t0);
                            j9_write32(pr, g_j9_wrap_java_fp - 4u, g_j9_wrap_t1);
                            j9_write32(pr, g_j9_wrap_java_fp, g_j9_wrap_t2);
                        }
                        address jsp_save = g_j9_sys_frame[3];
                        if ((jsp_save < 0x0071E000u) || (jsp_save >= 0x00720000u)) {
                            jsp_save = g_j9_wrap_r7;
                        }
                        if ((jsp_save < 0x0071E000u) || (jsp_save >= 0x00720000u)) {
                            jsp_save = g_j9_wrap_java_fp;
                        }
                        if (vmth && j9_mapped32(pr, vmth + 0x18u)) {
                            j9_write32(pr, vmth + 0x8u, g_j9_wrap_java_fp);
                            j9_write32(pr, vmth + 0x10u, jsp_save);
                            j9_write32(pr, vmth + 0x14u, g_j9_wrap_t1);
                            j9_write32(pr, vmth + 0x18u, g_j9_wrap_t0);
                        }
                        j9_write32(pr, g_j9_sys_sp + 0x30u, obj);
                        address jsp = g_j9_sys_frame[3];
                        if ((jsp < 0x0071E000u) || (jsp >= 0x00720000u)) {
                            jsp = g_j9_wrap_r7;
                        }
                        if ((jsp < 0x0071E000u) || (jsp >= 0x00720000u)) {
                            jsp = g_j9_wrap_java_fp;
                        }
                        const address vmt = (g_j9_vmthread && j9_mapped32(pr, g_j9_vmthread))
                            ? g_j9_vmthread : vmth;
                        core->set_reg(0, obj);
                        core->set_reg(1, obj);
                        core->set_reg(2, 0x818F5F70u);
                        core->set_reg(3, 1);
                        core->set_reg(4, g_j9_wrap_t0 ? g_j9_wrap_t0
                            : g_j9_sys_frame[0]);
                        core->set_reg(5, g_j9_wrap_t1 ? g_j9_wrap_t1
                            : g_j9_sys_frame[1]);
                        core->set_reg(6, g_j9_wrap_java_fp
                            ? g_j9_wrap_java_fp : g_j9_sys_frame[2]);
                        core->set_reg(7, jsp);
                        core->set_reg(8, vmt);
                        core->set_reg(9, g_j9_sys_frame[5]);
                        core->set_reg(10, g_j9_sys_frame[6]);
                        core->set_reg(11, g_j9_sys_frame[7]);
                        core->set_lr(obj);
                        core->set_sp(g_j9_sys_sp);
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] new-continue r0=0x{:X} sp=0x{:X} "
                            "r5=0x{:X} r6=0x{:X} r7=0x{:X} wfp=0x{:X} "
                            "t-4=0x{:X}",
                            obj, g_j9_sys_sp, core->get_reg(5),
                            core->get_reg(6), core->get_reg(7),
                            g_j9_wrap_java_fp, g_j9_wrap_t1);
                        g_j9_last_wrap_t1 = g_j9_wrap_t1;
                        g_j9_last_wrap_fp = g_j9_wrap_java_fp;
                        g_j9_wrap_fp_ok = false;
                        g_j9_wrap_clazz = 0;
                        g_j9_sys_ok = false;
                        g_j9_sys_sp = 0;
                        j9_set_pc(core, 0x8190E988u);
                        return;
                    }
                    // Return into whichever wrapper called interpret
                    // (0x818F92C0 / 0x818F87E4 / ...). Both do add sp,#8
                    // then read the result from the 0x40/0x50 frame.
                    address retpc = g_j9_sys_frame[8];
                    if ((retpc < 0x81800000u) || (retpc >= 0x82000000u)
                        || !pr->get_ptr_on_addr_space(retpc)) {
                        retpc = 0x818F92C0u;
                    }
                    j9_write32(pr, g_j9_sys_sp + 0xD0u, obj);
                    j9_write32(pr, g_j9_sys_sp + 0xD4u, obj);
                    if (g_j9_wrap_fp_ok && g_j9_wrap_java_fp && vmth
                        && j9_mapped32(pr, vmth + 8u)) {
                        j9_write32(pr, vmth + 8u, g_j9_wrap_java_fp);
                    }
                    if (g_j9_wrap_fp_ok && g_j9_wrap_java_fp
                        && j9_mapped32(pr, g_j9_wrap_java_fp)) {
                        j9_write32(pr, g_j9_wrap_java_fp - 8u, g_j9_wrap_t0);
                        j9_write32(pr, g_j9_wrap_java_fp - 4u, g_j9_wrap_t1);
                        j9_write32(pr, g_j9_wrap_java_fp, g_j9_wrap_t2);
                    }
                    if (g_j9_wrap_fp_ok && g_j9_wrap_sp44
                        && j9_looks_bytecode_pc(pr, g_j9_wrap_sp44)) {
                        j9_write32(pr, g_j9_sys_sp + 0x11Cu, g_j9_wrap_sp44);
                    }
                    core->set_reg(0, obj);
                    core->set_reg(4, g_j9_sys_frame[0]);
                    core->set_reg(5, g_j9_sys_frame[1]);
                    core->set_reg(6, g_j9_sys_frame[2]);
                    core->set_reg(7, g_j9_sys_frame[3]);
                    core->set_reg(8, g_j9_sys_frame[4]);
                    core->set_reg(9, g_j9_sys_frame[5]);
                    core->set_reg(10, g_j9_sys_frame[6]);
                    core->set_reg(11, g_j9_sys_frame[7]);
                    core->set_lr(retpc);
                    core->set_sp(g_j9_sys_sp + 0x90u);
                    const address wfp = g_j9_wrap_java_fp;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] system-ret-wrap r0=0x{:X} sp=0x{:X} "
                        "r4=0x{:X} ret=0x{:X} ip=0x{:X} bpc=0x{:X} "
                        "wfp=0x{:X} t-8=0x{:X} t-4=0x{:X} t0=0x{:X}",
                        obj, g_j9_sys_sp + 0x90u, g_j9_sys_frame[0],
                        retpc, g_j9_sys_frame[53],
                        g_j9_sys_frame[71], wfp,
                        (wfp && j9_mapped32(pr, wfp - 8u))
                            ? j9_read32(pr, wfp - 8u) : 0,
                        (wfp && j9_mapped32(pr, wfp - 4u))
                            ? j9_read32(pr, wfp - 4u) : 0,
                        (wfp && j9_mapped32(pr, wfp))
                            ? j9_read32(pr, wfp) : 0);
                    g_j9_last_wrap_t1 = g_j9_wrap_t1;
                    g_j9_last_wrap_fp = g_j9_wrap_java_fp;
                    g_j9_wrap_fp_ok = false;
                    g_j9_wrap_clazz = 0;
                    g_j9_sys_ok = false;
                    g_j9_sys_sp = 0;
                    j9_set_pc(core, retpc);
                    return;
                }
                if ((r5 == 0x81980746u)
                    && ((obj == g_j9_system_clazz) || (obj == 0x727770u))
                    && g_j9_cframe_ok && g_j9_init_sp && !g_j9_init_returned) {
                    const address vmth = g_j9_vmthread ? g_j9_vmthread : r8;
                    // Pending ICCE from System.initialize must not abort
                    // the C initialize loop at 0x8193EA38.
                    if (vmth && j9_mapped32(pr, vmth + 0x6cu)) {
                        j9_write32(pr, vmth + 0x64u, 0);
                        j9_write32(pr, vmth + 0x6cu, 0);
                    }
                    for (int i = 0; i < 64; ++i) {
                        j9_write32(pr, g_j9_init_sp
                            + static_cast<address>(i) * 4u, g_j9_cframe[i]);
                    }
                    core->set_reg(0, 0);
                    core->set_reg(4, g_j9_cframe[0]);
                    core->set_reg(5, g_j9_cframe[1]);
                    core->set_reg(6, g_j9_cframe[2]);
                    core->set_reg(7, g_j9_cframe[3]);
                    core->set_reg(8, g_j9_cframe[4]);
                    core->set_reg(9, g_j9_cframe[5]);
                    core->set_reg(10, g_j9_cframe[6]);
                    core->set_reg(11, g_j9_cframe[7]);
                    core->set_lr(g_j9_cframe[8]);
                    core->set_sp(g_j9_init_sp + 0x90u);
                    const address ret = g_j9_cframe[8] ? g_j9_cframe[8] : 0x8193EBB4u;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] system-return-c ret=0x{:X} sp=0x{:X} "
                        "r4=0x{:X} r8=0x{:X} r10=0x{:X} r11=0x{:X} ex=0x{:X} "
                        "clr=0x{:X}",
                        ret, g_j9_init_sp + 0x90u,
                        g_j9_cframe[0], g_j9_cframe[4], g_j9_cframe[6],
                        g_j9_cframe[7],
                        vmth ? j9_read32(pr, vmth + 0x64u) : 0,
                        g_j9_cframe[46]);
                    j9_set_pc(core, ret);
                    return;
                }
                if (false && (r5 == 0x81980746u) && g_j9_boot_returned
                    && g_j9_pending_clinit_ret && g_j9_pending_ret.csp
                    && (obj == g_j9_pending_ret.clazz)) {
                    const address ilr = g_j9_pending_ret.cframe[8];
                    const address csp = g_j9_pending_ret.csp;
                    const address vmth = g_j9_vmthread ? g_j9_vmthread : r8;
                    if (vmth && j9_mapped32(pr, vmth + 0x6cu)) {
                        j9_write32(pr, vmth + 0x64u, 0);
                        j9_write32(pr, vmth + 0x6cu, 0);
                    }
                    address frame[16];
                    for (int i = 0; i < 16; ++i) {
                        frame[i] = g_j9_pending_ret.cframe[i];
                    }
                    if (vmth) {
                        frame[4] = vmth;
                    }
                    for (int i = 0; i < 9; ++i) {
                        j9_write32(pr, csp + static_cast<address>(i) * 4u, frame[i]);
                    }
                    core->set_reg(0, 0);
                    if (vmth) {
                        core->set_reg(8, vmth);
                    }
                    core->set_sp(csp);
                    g_j9_pending_clinit_ret = false;
                    g_j9_pending_ret = {};
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] clinit-ret-c obj=0x{:X} -> 0x818DE50C "
                        "csp=0x{:X} lr=0x{:X} r8=0x{:X}",
                        obj, csp, ilr, vmth);
                    j9_set_pc(core, 0x818DE50Cu);
                    return;
                }
            }
            address next = r5 + 1u;
            std::uint8_t op = j9_read8(pr, next);
            // Official goto also consults vmthread+0x1c; if that word is
            // -1 it enters 0x818F5118 and corrupts the frame. Fold it.
            if (op == 0xA7u) {
                const std::int16_t off = static_cast<std::int16_t>(
                    j9_read8(pr, next + 1u)
                    | (static_cast<std::uint16_t>(static_cast<std::int8_t>(
                        j9_read8(pr, next + 2u))) << 8));
                next = next + static_cast<address>(off);
                op = j9_read8(pr, next);
            }
            // initialize() calls verify() after the first unlock. The ROM
            // image is preverified; running it throws ICCE (bad CP) and
            // then SOE. Skip to the initializeImpl path.
            if ((next == 0x819805D4u) && (op == 0x2Au)) {
                next = 0x819805ECu;
                op = j9_read8(pr, next);
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] mon-skip-verify -> 0x{:X} op=0x{:02X}",
                    next, op);
            }
            core->set_reg(5, next);
            core->set_reg(0, op);
            const address vm = j9_read32(pr, r8 + 4u);
            address tab = vm ? j9_read32(pr, vm + 0xa18u) : 0;
            if (!tab || (tab == 0x3FFF8E28u) || (tab < 0x81800000u)
                || (tab >= 0x82000000u) || !pr->get_ptr_on_addr_space(tab)) {
                tab = k_j9_opcode_table;
            }
            core->set_reg(2, tab);
            if (r8) {
                j9_write32(pr, r8 + 0xcu, tab);
            }
            if (vm && j9_mapped32(pr, vm + 0xa18u)) {
                j9_write32(pr, vm + 0xa18u, tab);
            }
            j9_set_pc(core, 0x818F5A7Cu);
            return;
        }

        bool is_bx_sb = false;
        for (int i = 0; i < g_j9_bx_sb_n; ++i) {
            if (pc == (g_j9_bx_sb_sites[i] & ~1u)) {
                is_bx_sb = true;
                break;
            }
        }
        if (is_bx_sb) {
            if ((pc == 0x81910D1Cu) || (pc == 0x81910E90u)) {
                static int prep = 0;
                if (prep < 12) {
                    ++prep;
                    const address r6 = core->get_reg(6);
                    const address r7 = core->get_reg(7);
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] prep-bx pc=0x{:X} sb=0x{:X} r6=0x{:X} [0]=0x{:X} [4]=0x{:X} [-4]=0x{:X} [-8]=0x{:X} r7=0x{:X} [0]=0x{:X} [4]=0x{:X}",
                        pc, core->get_reg(9), r6,
                        j9_read32(pr, r6), j9_read32(pr, r6 + 4),
                        j9_read32(pr, r6 - 4), j9_read32(pr, r6 - 8),
                        r7, j9_read32(pr, r7), j9_read32(pr, r7 + 4));
                }
            }
            address target = core->get_reg(9);
            const address method = core->get_reg(0);
            std::uint32_t *mw = nullptr;
            address send = 0;
            address extra = 0;
            if (pr && (method > 0x10000u)) {
                mw = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(method));
                if (mw) {
                    send = mw[2];
                    extra = mw[3];
                }
            }
            // Raw midp2ams/LCDUI Thumb JNI. 0x81910D1C/0x81910E90 already
            // restored r7 from the VM thread — go to the adapter, not
            // jni_entry (that would double-advance the bytecode PC).
            if (((target & ~1u) >= 0x81A5B000u) && ((target & ~1u) < 0x81B20000u)
                && (target & 1u)) {
                address ad = j9_adapter_for(target);
                if (!ad) {
                    ad = g_j9_last_adapt;
                }
                if (mw && target) {
                    j9_plant_method(mw, target);
                    if (mw[2] && j9_fn_is_code(mw[2]) && (mw[2] != k_j9_jni_entry)) {
                        ad = mw[2];
                    }
                }
                static int midp_bx = 0;
                if (midp_bx < 16) {
                    ++midp_bx;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] bx-sb midp pc=0x{:X} sb=0x{:X} -> ad=0x{:X} r7=0x{:X} [4]=0x{:X} [8]=0x{:X}",
                        pc, core->get_reg(9), ad, core->get_reg(7),
                        j9_read32(pr, core->get_reg(7) + 4),
                        j9_read32(pr, core->get_reg(7) + 8));
                }
                // Prepared invoke sites already have interpreter regs.
                if ((pc == 0x81910D1Cu) || (pc == 0x81910E90u)) {
                    j9_marshal_java_args(core, pr, mw);
                    j9_set_pc(core, ad ? ad : k_j9_jni_entry);
                } else {
                    j9_set_pc(core, k_j9_jni_entry);
                }
                return;
            }
            if (!j9_fn_is_code(target) || (target < 0x10000u) || (target == k_j9_jni_entry)) {
                address fn = 0;
                if (g_j9_walk_va && (send >= (g_j9_walk_va + k_j9_adapt_off))
                    && (send < (g_j9_walk_va + 0x8000u))) {
                    fn = send;
                } else if ((extra == 1u) && g_j9_last_adapt) {
                    fn = g_j9_last_adapt;
                }
                if (const address ad = j9_adapter_for(fn)) {
                    target = ad;
                } else if (fn) {
                    target = fn;
                } else if ((extra > 1u) && (extra & 1u)) {
                    core->set_pc(pc + 4);
                    return;
                }
            }
            if (!j9_fn_is_exec(target)) {
                address walk = target;
                address slot = 0;
                address slot2 = 0;
                for (int hop = 0; hop < 4; ++hop) {
                    if (j9_fn_is_exec(walk)) {
                        break;
                    }
                    if (!walk || !j9_mapped32(pr, walk & ~1u)) {
                        break;
                    }
                    const address w0 = j9_read32(pr, walk & ~1u);
                    const address w1 = j9_read32(pr, (walk & ~1u) + 4u);
                    if (!slot) {
                        slot = w0;
                        slot2 = w1;
                    }
                    // RAM veneer: `ldr pc, [pc, #-4]` then the real target.
                    if (w0 == 0xE51FF004u) {
                        walk = w1;
                        continue;
                    }
                    break;
                }
                if (j9_fn_is_exec(walk)) {
                    static int deref = 0;
                    if (deref < 8) {
                        ++deref;
                        const char *jcln = nullptr;
                        for (const auto &ent : j9_jni_exports) {
                            if ((ent.fn & ~1u) == (walk & ~1u)) {
                                jcln = ent.name.c_str();
                                break;
                            }
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] bx-sb veneer pc=0x{:X} sb=0x{:X} -> 0x{:X} slot=0x{:X}/0x{:X} name='{}'",
                            pc, target, walk, slot, slot2, jcln ? jcln : "?");
                    }
                    // JCL Java_* Thumb bodies expect a JNIEnv* in r0.
                    // The interpreter calls them with r0=vmthread, so the
                    // first JNI slot blx is null. Host-return through LR.
                    const char *jcln = j9_jcl_name_for_fn(walk);
                    const bool is_getenc = ((walk & ~1u) == 0x819477FAu)
                        || (jcln && std::strstr(jcln, "System_getEncoding"));
                    const bool is_proplist = jcln
                        && std::strstr(jcln, "System_getPropertyList");
                    if (is_getenc || is_proplist
                        || (jcln && std::strstr(jcln, "Java_java_lang_System_"))) {
                        address vt = core->get_reg(8);
                        if (!j9_r8_looks_vmthread(pr, vt, 0)) {
                            vt = core->get_reg(0);
                        }
                        if (is_getenc) {
                            if (!g_j9_encoding_str) {
                                g_j9_encoding_str = j9_new_string_utf(pr, "ISO-8859-1");
                                if (g_j9_encoding_str) {
                                    const address val = j9_read32(pr, g_j9_encoding_str + 8u);
                                    LOG_WARN(EMULATED_STDOUT,
                                        "[j9-nf] host-str shape str=0x{:X} "
                                        "s=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X} "
                                        "arr=0x{:X} a=0x{:X}/0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                                        g_j9_encoding_str,
                                        j9_read32(pr, g_j9_encoding_str),
                                        j9_read32(pr, g_j9_encoding_str + 4u),
                                        j9_read32(pr, g_j9_encoding_str + 8u),
                                        j9_read32(pr, g_j9_encoding_str + 12u),
                                        j9_read32(pr, g_j9_encoding_str + 16u),
                                        j9_read32(pr, g_j9_encoding_str + 20u),
                                        val,
                                        val ? j9_read32(pr, val) : 0,
                                        val ? j9_read32(pr, val + 4u) : 0,
                                        val ? j9_read32(pr, val + 8u) : 0,
                                        val ? j9_read32(pr, val + 12u) : 0,
                                        val ? j9_read32(pr, val + 16u) : 0);
                                }
                            }
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] bx-sb getEncoding pc=0x{:X} type=0x{:X} str=0x{:X} "
                                "lr=0x{:X} r5=0x{:X} r7=0x{:X} name='{}'",
                                pc, core->get_reg(2), g_j9_encoding_str, core->get_lr(),
                                core->get_reg(5), core->get_reg(7), jcln ? jcln : "?");
                            core->set_reg(0, g_j9_encoding_str);
                            ++g_j9_encoding_n;
                            if (g_j9_encoding_n >= 3) {
                                j9_rebind_teardown_jni(pr);
                            }
                            j9_restore_interp_table(core, pr, vt, false);
                            j9_set_pc(core, core->get_lr());
                            return;
                        }
                        if (is_proplist) {
                            const address arr = j9_ensure_proplist(pr);
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] bx-sb getPropertyList pc=0x{:X} arr=0x{:X} "
                                "ac=0x{:X} lr=0x{:X} r5=0x{:X} r7=0x{:X}",
                                pc, arr, g_j9_string_array_clazz,
                                core->get_lr(), core->get_reg(5), core->get_reg(7));
                            core->set_reg(0, arr);
                            j9_restore_interp_table(core, pr, vt, false);
                            j9_set_pc(core, core->get_lr());
                            return;
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] bx-sb system-native pc=0x{:X} walk=0x{:X} name='{}' "
                            "r0=0x{:X} r1=0x{:X} r2=0x{:X} r5=0x{:X} r7=0x{:X} lr=0x{:X}",
                            pc, walk, jcln ? jcln : "?",
                            core->get_reg(0), core->get_reg(1), core->get_reg(2),
                            core->get_reg(5), core->get_reg(7), core->get_lr());
                    }
                    target = walk;
                } else if (slot == 0xE51FF004u) {
                    // Execute the veneer itself; it is RAM code, not a data ptr.
                    static int veneer = 0;
                    if (veneer < 8) {
                        ++veneer;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] bx-sb run-veneer pc=0x{:X} sb=0x{:X} tgt=0x{:X}",
                            pc, target, slot2);
                    }
                    address vt = core->get_reg(8);
                    if (!j9_r8_looks_vmthread(pr, vt, 0)) {
                        vt = core->get_reg(0);
                    }
                    j9_restore_interp_table(core, pr, vt, false);
                    j9_set_pc(core, target);
                    return;
                } else {
                    address vt = core->get_reg(8);
                    if (!j9_r8_looks_vmthread(pr, vt, 0)) {
                        vt = core->get_reg(0);
                    }
                    static int bad = 0;
                    if (bad < 8) {
                        ++bad;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] bx-sb skip pc=0x{:X} sb=0x{:X} slot=0x{:X}/0x{:X} "
                            "r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} r4=0x{:X} r5=0x{:X} "
                            "r6=0x{:X} r7=0x{:X} r8=0x{:X} lr=0x{:X} "
                            "pre=0x{:X}/0x{:X}/0x{:X}/0x{:X} vt0c=0x{:X}",
                            pc, target, slot, slot2,
                            core->get_reg(0), core->get_reg(1), core->get_reg(2),
                            core->get_reg(3), core->get_reg(4), core->get_reg(5),
                            core->get_reg(6), core->get_reg(7), core->get_reg(8),
                            core->get_lr(),
                            j9_read32(pr, pc - 16u), j9_read32(pr, pc - 12u),
                            j9_read32(pr, pc - 8u), j9_read32(pr, pc - 4u),
                            vt ? j9_read32(pr, vt + 0xcu) : 0);
                    }
                    j9_restore_interp_table(core, pr, vt, true);
                    j9_set_pc(core, core->get_lr());
                    return;
                }
            }
            if ((target & ~1u) == 0x8190AF14u) {
                const address r0 = core->get_reg(0);
                const address r1 = core->get_reg(1);
                const address r2 = core->get_reg(2);
                static int seg_logs = 0;
                if (seg_logs < 8) {
                    ++seg_logs;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] skip-memseg pc=0x{:X} r0=0x{:X} r1=0x{:X} r2=0x{:X} lr=0x{:X}",
                        pc, r0, r1, r2, core->get_lr());
                }
                // C convention (vmThread, object, segment) uses the 0x02D
                // Java heap. Run the official helper there.
                if ((r1 >= 0x02000000u) && (r1 < 0x03000000u)) {
                    j9_set_pc(core, target);
                    return;
                }
                // Interpreter convention is (vmThread, clazz, obj). Link a
                // freshly allocated Thread instance as currentThread so
                // native createMainThread does not throw OOME/NPE.
                if (r1 && r2 && j9_mapped32(pr, r2) && (j9_read32(pr, r2) == r1)
                    && (g_j9_thread_class ? (r1 == g_j9_thread_class) : true)) {
                    char nbuf[96];
                    nbuf[0] = 0;
                    j9_class_name(pr, r1, nbuf, sizeof(nbuf));
                    if (nbuf[0] && (std::strcmp(nbuf, "java/lang/Thread") == 0)) {
                        g_j9_thread_class = r1;
                        g_j9_thread_obj = r2;
                        if (r0 && j9_mapped32(pr, r0 + k_j9_thread_obj_off)) {
                            j9_write32(pr, r0 + k_j9_thread_obj_off, r2);
                            const unsigned inst = j9_read32(pr, r1 + 0x38u);
                            const unsigned nbytes = (inst < 0x1000u) ? (inst + 0xcu) : 0x10u;
                            if (nbytes >= 0x10u) {
                                j9_write32(pr, r2 + 0xcu, r0);
                            }
                            if (nbytes >= 0x14u) {
                                j9_write32(pr, r2 + 0x10u, r0);
                            }
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] thread-link obj=0x{:X} clazz=0x{:X} vt=0x{:X} inst=0x{:X} slot=0x{:X}",
                            r2, r1, r0, j9_read32(pr, r1 + 0x38u),
                            r0 ? j9_read32(pr, r0 + k_j9_thread_obj_off) : 0);
                    }
                }
                j9_set_pc(core, core->get_lr());
                return;
            }
            static int bx_logs = 0;
            if (bx_logs < 24) {
                ++bx_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] bx-sb pc=0x{:X} sb=0x{:X} -> 0x{:X} send=0x{:X} extra=0x{:X} meth=0x{:X}",
                    pc, core->get_reg(9), target, send, extra, method);
            }
            {
                address vt = core->get_reg(8);
                if (!j9_r8_looks_vmthread(pr, vt, 0)) {
                    vt = core->get_reg(0);
                }
                j9_restore_interp_table(core, pr, vt, false);
            }
            j9_set_pc(core, target);
            return;
        }

        bool is_invoke = false;
        for (int i = 0; i < g_j9_invoke_n; ++i) {
            if (pc == (g_j9_invoke_sites[i] & ~1u)) {
                is_invoke = true;
                break;
            }
        }
        const bool is_tramp = g_j9_tramp_va && (pc == (g_j9_tramp_va & ~1u));
        if (is_tramp) {
            const address method = core->get_reg(0);
            j9_note_interp_frame(core, pr, method);
            const address send = j9_read32(pr, method + 8u);
            const address extra = j9_read32(pr, method + 0xcu);
            address thumb = 0;
            if (const address ad = j9_adapter_for_method(method)) {
                if (g_j9_walk_ch && g_j9_walk_ch->host_base()
                    && (ad >= (g_j9_walk_va + k_j9_adapt_off))) {
                    auto *base = reinterpret_cast<std::uint8_t *>(g_j9_walk_ch->host_base());
                    const auto *aw = reinterpret_cast<const std::uint32_t *>(
                        base + (ad - g_j9_walk_va));
                    if (aw[18] && ((aw[18] & ~1u) >= 0x81A00000u)) {
                        thumb = aw[18];
                    }
                }
                if (!thumb) {
                    thumb = g_j9_last_thumb;
                }
            }
            if (!thumb && (send == k_j9_jni_entry) && (extra == 1u)) {
                thumb = g_j9_last_thumb;
            }
            const address bare = thumb & ~1u;
            const bool lcdui_create = (bare == 0x81AF1616u) || (bare == 0x81AF2AACu)
                || (bare == 0x81AEE7F8u) || (bare == 0x81A61CC4u)
                || (bare == 0x81AF17F0u) || (bare == 0x81AF106Au)
                || (bare == 0x81AF2C12u);
            if (lcdui_create) {
                const address r7 = core->get_reg(7);
                address packed = j9_pack_obj(pr, j9_read32(pr, r7));
                if (!packed) {
                    packed = j9_pack_obj(pr, j9_read32(pr, r7 + 4u));
                }
                if (!packed) {
                    // Not a Java _create(this, ...) — fall through to
                    // jni_entry / the real Thumb native.
                    static int skip = 0;
                    if (skip < 8) {
                        ++skip;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] lcdui-real tramp thumb=0x{:X} meth=0x{:X} r7=0x{:X} [0]=0x{:X} [4]=0x{:X}",
                            thumb, method, r7, j9_read32(pr, r7),
                            j9_read32(pr, r7 + 4u));
                    }
                } else {
                const address peer = j9_attach_dummy_peer(pr, thr, packed);
                if (bare == 0x81AF2AACu) {
                    g_j9_toolkit_obj = packed ? (packed << 2) : g_j9_toolkit_obj;
                } else if (bare == 0x81AEE7F8u) {
                    g_j9_canvas_obj = packed ? (packed << 2) : g_j9_canvas_obj;
                } else if ((bare == 0x81AF1616u) && packed) {
                    g_j9_graphics_obj = packed << 2;
                }
                static int create_at_tramp = 0;
                if (create_at_tramp < 16) {
                    ++create_at_tramp;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] lcdui-tramp-stub thumb=0x{:X} meth=0x{:X} packed=0x{:X} peer=0x{:X} r6=0x{:X} r7=0x{:X}",
                        thumb, method, packed, peer, core->get_reg(6), r7);
                }
                // _create is void. Leave a full-pointer `this` at the
                // Java stack top so the following invokevirtual sees
                // the object we just attached a peer to.
                // Next opcode after Graphics._create is 0xAC (return).
                // Pop the native's args so the return handler sees the
                // caller's interpreter frame, not the invoke operands.
                if (g_j9_alps_started
                    && ((bare == 0x81AF1616u) || (bare == 0x81AEE7F8u)
                        || (bare == 0x81AF2AACu) || (bare == 0x81AF2C12u)
                        || (bare == 0x81A61CC4u))) {
                    // Event-source Execute / JNI _create wait on the VM
                    // lock held by this interpreter frame. Stub and resume.
                }
                const address tm = g_j9_tramp_method ? g_j9_tramp_method : method;
                j9_jxe_resume_interp(core, pr, tm, j9_method_argc(pr, tm));
                return;
                }
            }
            // Same as the old `cmp r12,#0x80000000; movhs pc,r12`: ROM
            // INL / jni_entry resume after the snapshot. Set r12 too —
            // some INL handlers expect send there.
            if (send >= 0x80000000u) {
                if (j9_try_emu_inl(core, pr, send, method)) {
                    return;
                }
                // J9VMInternals.verify() — ROM classes are preverified.
                if (j9_read32(pr, method) == 0x8198030Cu) {
                    j9_write32(pr, core->get_reg(7), 0);
                    g_j9_resume_no_ac = true;
                    j9_jxe_resume_interp(core, pr, method, 0);
                    g_j9_resume_no_ac = false;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] verify-skip meth=0x{:X}", method);
                    return;
                }
                static int fwd = 0;
                if (fwd < 8) {
                    ++fwd;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] tramp-fwd send=0x{:X} meth=0x{:X} r6=0x{:X} r7=0x{:X} r8=0x{:X}",
                        send, method, core->get_reg(6), core->get_reg(7),
                        core->get_reg(8));
                }
                // initializeImpl / sendClassPrepareEvent save r2 to
                // vmthread+0xc. tramp r2 is midp2ams .data; that poisons
                // later bytecode dispatch (VME / ICCE / SOE).
                {
                    const address r8f = core->get_reg(8);
                    const address vm = j9_read32(pr, r8f + 4u);
                    const address tab = vm ? j9_read32(pr, vm + 0xa18u) : 0;
                    if (tab && pr->get_ptr_on_addr_space(tab)) {
                        core->set_reg(2, tab);
                        j9_write32(pr, r8f + 0xcu, tab);
                    }
                }
                core->set_reg(12, send);
                j9_set_pc(core, send);
                return;
            }
        }
        if (is_invoke && (pc == 0x8190F0A0u)) {
            // Shared snippet: `ldr r12,[r7],#4`. Only rewrite when the
            // bytecode PC is the JCL Graphics helper (aload_0/getfield).
            // JXE opcodes (e.g. 0x2F at 0x770B4A) use the same code.
            const address r5 = core->get_reg(5);
            address r7 = core->get_reg(7);
            if (!j9_looks_jcl_pc(r5)) {
                core->set_reg(12, j9_read32(pr, r7));
                core->set_reg(7, r7 + 4u);
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x8190F0A4u);
                return;
            }
            address r12 = j9_read32(pr, r7);
            address r11 = j9_read32(pr, r7 + 4u);
            const address jthis = j9_ensure_jcl_this(pr);
            if (!r11 || (r11 < 0x10000u) || !j9_mapped32(pr, r11 + 0xcu)) {
                if (j9_looks_heap(r12) && j9_mapped32(pr, r12 + 0xcu)) {
                    r11 = r12;
                    r12 = 8u;
                } else {
                    r11 = jthis;
                }
            }
            if (!r12 || (r12 > 0x10u)) {
                r12 = 8u;
            }
            r7 += 8u;
            core->set_reg(12, r12);
            core->set_reg(11, r11);
            core->set_reg(7, r7);
            static int gf = 0;
            if (gf < 8) {
                ++gf;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] ac-jcl-getf r11=0x{:X} r12=0x{:X} this=0x{:X} r5=0x{:X} op=0x{:02X}",
                    r11, r12, jthis, r5, j9_read8(pr, r5));
            }
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x8190F0A8u);
            return;
        }
        if (is_invoke && (pc == 0x81910F2Cu)) {
            // Post-Graphics-return invoke: `ldr r12,[r3,#4]` with r3 from r4.
            address r3 = core->get_reg(3);
            if (!r3 || (r3 < 0x10000u) || !j9_mapped32(pr, r3 + 4u)) {
                address r4 = core->get_reg(4);
                if (!r4 || !j9_mapped32(pr, r4 + 4u)) {
                    r4 = g_j9_jcl_outer_r4 ? g_j9_jcl_outer_r4
                        : (g_j9_good_r4 ? g_j9_good_r4 : g_j9_jcl_r4);
                }
                r3 = r4;
                if (r4) {
                    core->set_reg(4, r4);
                    core->set_reg(3, r4);
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] invoke-r4fix r3=0x{:X} r4=0x{:X} r5=0x{:X}",
                    r3, r4, core->get_reg(5));
            }
            if (r3 && j9_mapped32(pr, r3 + 4u)) {
                core->set_reg(12, j9_read32(pr, r3 + 4u));
            }
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x81910F30u);
            return;
        }
        if (is_invoke && (pc == 0x81910F84u)) {
            const address sl = core->get_reg(10);
            if (!sl || !j9_mapped32(pr, sl)) {
                core->set_reg(10, 0);
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910FACu);
                return;
            }
            const address vt = j9_read32(pr, sl);
            const address ip = core->get_reg(12);
            if (!vt || !j9_mapped32(pr, vt + ip)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            const address method = j9_read32(pr, vt + ip);
            if (!method || (method < 0x10000u) || !j9_mapped32(pr, method) || !j9_mapped32(pr, method + 8u)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            core->set_reg(10, vt);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x81910F88u);
            return;
        }
        if (is_invoke && (pc == 0x81910F88u)) {
            const address vt = core->get_reg(10);
            const address ip = core->get_reg(12);
            if (!vt || !j9_mapped32(pr, vt + ip)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            const address method = j9_read32(pr, vt + ip);
            if (!method || (method < 0x10000u) || !j9_mapped32(pr, method) || !j9_mapped32(pr, method + 8u)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            core->set_reg(0, method);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x81910F8Cu);
            return;
        }
        if (is_invoke && (pc == 0x81910F8Cu)) {
            const address method = core->get_reg(0);
            if (!method || (method < 0x10000u) || !j9_mapped32(pr, method) || !j9_mapped32(pr, method + 8u)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            const address send = j9_read32(pr, method + 8u);
            if (!send || !j9_fn_is_code(send)) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x81910F98u);
                return;
            }
            j9_set_pc(core, send);
            return;
        }
        if (is_invoke && ((pc == 0x81910F04u) || (pc == 0x81910F48u))) {
            if (pc == 0x81910F48u) {
                core->set_reg(3, ~7u);
            }
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x818F6450u)) {
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x818F5A84u)) {
            const address op = core->get_reg(0);
            if (op >= 256u) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                core->set_pc(0x818F5118u);
                return;
            }
            // Official table lives in ROM (0x818F5F70). j9_mapped32 rejects
            // >=0x80000000, so the old path fell back to midp2ams .data
            // (0x3FFF8E28) and every opcode jumped to the wrong handler.
            const address r1_val = core->get_reg(1);
            address sb = k_j9_opcode_table;
            if (r1_val && pr->get_ptr_on_addr_space(r1_val + 0xA18u)) {
                const address tab = j9_read32(pr, r1_val + 0xA18u);
                if (tab && pr->get_ptr_on_addr_space(tab)
                    && pr->get_ptr_on_addr_space(tab + 0xC2u * 4u)
                    && (j9_read32(pr, tab + 0xC2u * 4u) == k_j9_monitorenter)) {
                    sb = tab;
                }
            }
            core->set_reg(9, sb);
            if (sb && pr->get_ptr_on_addr_space(sb + op * 4u)) {
                const address target = j9_read32(pr, sb + op * 4u);
                if (target && j9_fn_is_code(target)) {
                    static int disp_logs = 0;
                    if (disp_logs < 12) {
                        ++disp_logs;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] dispatch op=0x{:02X} tab=0x{:X} -> 0x{:X} r5=0x{:X}",
                            op, sb, target, core->get_reg(5));
                    }
                    if (g_j9_init_returned) {
                        const address ir5 = core->get_reg(5);
                        if ((ir5 >= 0x81950000u) && (ir5 < 0x81A00000u)
                            && ((ir5 < 0x81980500u) || (ir5 >= 0x81980800u))) {
                            g_j9_live_r4 = core->get_reg(4);
                            g_j9_live_r5 = ir5;
                            g_j9_live_r6 = core->get_reg(6);
                            g_j9_live_r7 = core->get_reg(7);
                            j9_note_outer_jcl(pr, g_j9_live_r4, ir5,
                                g_j9_live_r6, g_j9_live_r7);
                            if ((ir5 >= 0x81962000u) && (ir5 < 0x81964000u)) {
                                g_j9_caller_live_r4 = g_j9_live_r4;
                                g_j9_caller_live_r5 = ir5;
                                g_j9_caller_live_r6 = g_j9_live_r6;
                                g_j9_caller_live_r7 = g_j9_live_r7;
                            }
                            static int live_logs = 0;
                            if ((live_logs < 8)
                                && (ir5 >= 0x8195BB00u) && (ir5 < 0x8195BC00u)) {
                                ++live_logs;
                                LOG_WARN(EMULATED_STDOUT,
                                    "[j9-nf] live-snap r4=0x{:X} r5=0x{:X} "
                                    "r6=0x{:X} r7=0x{:X} l0=0x{:X} lm4=0x{:X}",
                                    g_j9_live_r4, g_j9_live_r5, g_j9_live_r6,
                                    g_j9_live_r7,
                                    g_j9_live_r6 ? j9_read32(pr, g_j9_live_r6) : 0,
                                    g_j9_live_r6 ? j9_read32(pr, g_j9_live_r6 - 4u) : 0);
                            }
                        }
                    }
                    j9_set_pc(core, target);
                    return;
                }
            }
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x818F5118u);
            return;
        }
        if (is_invoke && (pc == 0x818F9FA8u)) {
            // UTF8/name helper (vm, J9Method*). Not an invoke — do not
            // rewrite r1 via j9_find_valid_cp (that walks 48 words past a
            // 16-byte J9Method and replaces r1 with a random heap slot).
            core->set_reg(13, core->get_reg(13) - 0x30u);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && ((pc == 0x818F85C0u) || (pc == 0x818F87F0u) || (pc == 0x818F8A80u)
                || (pc == 0x818F8C14u) || (pc == 0x818F8E00u) || (pc == 0x818F910Cu)
                || (pc == 0x818F9410u) || (pc == 0x818F9B5Cu))) {
            if ((pc == 0x818F85C0u) && pr) {
                const address base = core->get_reg(1);
                const address idx = core->get_reg(2);
                const address slot = base + (idx << 3);
                if ((idx >= 0x00100000u) || !base || !j9_mapped32(pr, slot)) {
                    static int um = 0;
                    if (um < 12) {
                        ++um;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] wrap-unresolved pc=0x818F85C0 r1=0x{:X} "
                            "r2=0x{:X} slot=0x{:X} lr=0x{:X} boot={}",
                            base, idx, slot, core->get_lr(),
                            g_j9_boot_returned ? 1 : 0);
                    }
                    core->set_reg(0, 0);
                    j9_set_pc(core, core->get_lr());
                    return;
                }
            }
            if (g_j9_init_returned) {
                j9_try_capture_live_cframe(pr, core);
                const address wsp = core->get_sp();
                const address wr8 = core->get_reg(8);
                const address wfp = (wr8 && j9_mapped32(pr, wr8 + 8u))
                    ? j9_read32(pr, wr8 + 8u) : 0;
                const address t0 = (wfp && j9_mapped32(pr, wfp - 8u))
                    ? j9_read32(pr, wfp - 8u) : 0;
                const address t1 = (wfp && j9_mapped32(pr, wfp - 4u))
                    ? j9_read32(pr, wfp - 4u) : 0;
                const address t2 = (wfp && j9_mapped32(pr, wfp))
                    ? j9_read32(pr, wfp) : 0;
                static int wrap_see = 0;
                if (!g_j9_wrap_fp_ok && (wrap_see < 24)) {
                    ++wrap_see;
                    const std::uint8_t top = t1 ? j9_read8(pr, t1) : 0;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] wrap-see pc=0x{:X} fp=0x{:X} t-8=0x{:X} "
                        "t-4=0x{:X} op=0x{:02X} t0=0x{:X} ok={} have={} lr=0x{:X}",
                        pc, wfp, t0, t1, top, t2,
                        j9_wrap_triple_ok(pr, wfp, t0, t1, t2) ? 1 : 0,
                        g_j9_wrap_fp_ok ? 1 : 0, core->get_lr());
                }
                // First valid snapshot only. Nested enters after the
                // initializeImpl wrapper must not overwrite it.
                if (!g_j9_wrap_fp_ok && !g_j9_sys_ok
                    && j9_wrap_triple_ok(pr, wfp, t0, t1, t2)) {
                    g_j9_wrap_java_fp = wfp;
                    g_j9_wrap_r4 = core->get_reg(4);
                    g_j9_wrap_r5 = core->get_reg(5);
                    g_j9_wrap_r6 = core->get_reg(6);
                    g_j9_wrap_r7 = core->get_reg(7);
                    g_j9_wrap_sp34 = (wsp && j9_mapped32(pr, wsp + 0x34u))
                        ? j9_read32(pr, wsp + 0x34u) : 0;
                    g_j9_wrap_sp44 = (wsp && j9_mapped32(pr, wsp + 0x44u))
                        ? j9_read32(pr, wsp + 0x44u) : 0;
                    g_j9_wrap_t0 = t0;
                    g_j9_wrap_t1 = t1;
                    g_j9_wrap_t2 = t2;
                    g_j9_wrap_fp_ok = true;
                    if (t1 == 0x81962D17u) {
                        g_j9_boot_t0 = t0;
                        g_j9_boot_t1 = t1;
                        g_j9_boot_t2 = t2;
                        g_j9_boot_fp = wfp;
                        g_j9_boot_csp = wsp;
                        for (int i = 0; i < 16; ++i) {
                            g_j9_boot_cframe[i] = (wsp && j9_mapped32(pr,
                                wsp + static_cast<address>(i) * 4u))
                                ? j9_read32(pr, wsp + static_cast<address>(i) * 4u)
                                : 0;
                        }
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] wrap-enter pc=0x{:X} fp=0x{:X} r5=0x{:X} "
                        "sp34=0x{:X} sp44=0x{:X} t-8=0x{:X} t-4=0x{:X} "
                        "t0=0x{:X} lr=0x{:X} csp=0x{:X} clr=0x{:X}",
                        pc, wfp, g_j9_wrap_r5, g_j9_wrap_sp34, g_j9_wrap_sp44,
                        t0, t1, t2, core->get_lr(), g_j9_boot_csp,
                        g_j9_boot_cframe[8]);
                }
                if (g_j9_boot_returned) {
                    j9_try_capture_live_cframe(pr, core);
                }
            }
            if (!g_j9_boot_returned) {
                const address r1_val = core->get_reg(1);
                const address valid_cp = j9_find_valid_cp(pr, r1_val);
                if (valid_cp && (valid_cp != (r1_val & ~7u))) {
                    core->set_reg(1, valid_cp);
                    g_j9_valid_cp = valid_cp;
                }
            }
            address sub_sp = 0x48;
            if ((pc == 0x818F85C0u) || (pc == 0x818F910Cu) || (pc == 0x818F9B5Cu)) {
                sub_sp = 0x40;
            } else if (pc == 0x818F87F0u) {
                sub_sp = 0x50;
            }
            core->set_reg(13, core->get_reg(13) - sub_sp);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x818ED590u)) {
            // nextROMMethod. The old gate required both >=0x80000000 (XIP)
            // and j9_mapped32 (which rejects XIP), so every call returned 0
            // and createRAMClass stored bytecodes=NULL+0x14.
            const address rom = core->get_reg(0);
            address next = 0;
            if (rom && pr && pr->get_ptr_on_addr_space(rom + 8u)) {
                const std::uint32_t modifiers = j9_read32(pr, rom + 8u);
                std::uint32_t extra = 0;
                if (const auto *h = reinterpret_cast<const std::uint16_t *>(pr->get_ptr_on_addr_space(rom + 0xEu))) {
                    extra = *h;
                }
                if (modifiers & 0x8000u) {
                    if (const auto *b = reinterpret_cast<const std::uint8_t *>(pr->get_ptr_on_addr_space(rom + 0x10u))) {
                        extra += static_cast<std::uint32_t>(*b) << 16;
                    }
                }
                next = rom + 0x14u + (extra << 2);
                if (modifiers & 0x2000000u) {
                    next += 4u;
                }
                if (modifiers & 0x20000u) {
                    const address sl = next;
                    next += 4u;
                    if (const auto *h = reinterpret_cast<const std::uint16_t *>(pr->get_ptr_on_addr_space(sl))) {
                        next += (static_cast<std::uint32_t>(h[0]) << 4) + (static_cast<std::uint32_t>(h[1]) << 2);
                    }
                }
            }
            static int next_rom_logs = 0;
            if (next_rom_logs < 16) {
                ++next_rom_logs;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] nextROMMethod in=0x{:X} out=0x{:X}", rom, next);
            }
            core->set_reg(0, next);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (is_invoke && (pc == 0x819106F4u)) {
            core->set_reg(1, ~7u);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x8190E940u)) {
            core->set_reg(1, core->get_reg(4));
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x818F67A0u)) {
            core->set_reg(3, core->get_reg(4));
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x818F6608u)) {
            // JCL putfield resolve AVs on type=JXE fieldrefs. Skip the
            // 3-byte opcode when we are in the Graphics helper.
            const address r5 = core->get_reg(5);
            if (g_j9_jcl_returned
                && (j9_looks_jcl_pc(r5)
                    || ((r5 >= 0x00770000u) && (r5 < 0x00780000u)))
                && (j9_read8(pr, r5) == 0xB5u)) {
                const address next = r5 + 3u;
                static int npl = 0;
                if (npl < 6) {
                    ++npl;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-nopf-late r5=0x{:X} next=0x{:02X}",
                        r5, j9_read8(pr, next));
                }
                if (j9_is_return_op(j9_read8(pr, next))) {
                    if (j9_finish_jcl_to_jxe(core, pr)) {
                        return;
                    }
                }
                const address r6pf = core->get_reg(6);
                const address r7pf = core->get_reg(7);
                const address obj = r6pf ? j9_read32(pr, r6pf) : 0;
                const address val = r7pf ? j9_read32(pr, r7pf) : 0;
                if (j9_looks_heap(obj) && j9_looks_heap(val)
                    && j9_mapped32(pr, obj + 0x20u)) {
                    for (int i = 2; i < 12; ++i) {
                        const address at = obj + static_cast<address>(i) * 4u;
                        if (j9_mapped32(pr, at) && !j9_read32(pr, at)) {
                            j9_write32(pr, at, val);
                            break;
                        }
                    }
                }
                g_j9_saved_r5 = r5;
                g_j9_saved_r4 = core->get_reg(4);
                g_j9_saved_r6 = core->get_reg(6);
                g_j9_java_sp = core->get_reg(7);
                g_j9_resume_no_ac = true;
                g_j9_resume_at = next;
                j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            if (g_j9_jcl_r5 && (r5 >= g_j9_jcl_r5) && (r5 < (g_j9_jcl_r5 + 0x40u))) {
                const address next = r5 + 3u;
                const std::uint8_t nop = next ? j9_read8(pr, next) : 0;
                j9_scan_java_caller(pr, g_j9_jcl_r6);
                address resume = next;
                address rr4 = g_j9_jcl_r4;
                address rr5 = g_j9_jcl_r5;
                address rr6 = g_j9_jcl_r6;
                // Last store in the helper; return to the constructor's
                // caller instead of dispatching goto/areturn with no frame.
                if ((nop == 0xA7u) || (nop == 0xACu) || (nop == 0xB1u)) {
                    if (g_j9_jcl_outer_r5 && j9_looks_caller_pc(pr, g_j9_jcl_outer_r5)) {
                        g_j9_jcl_returned = true;
                        j9_plant_post_jcl_r4fix(pr);
                        if (j9_finish_jcl_to_jxe(core, pr)) {
                            return;
                        }
                        resume = g_j9_jcl_outer_r5 + 3u;
                        rr4 = g_j9_jcl_outer_r4;
                        rr5 = g_j9_jcl_outer_r5;
                        rr6 = g_j9_jcl_outer_r6;
                    }
                }
                const address jthis = j9_ensure_jcl_this(pr);
                if (rr6 && j9_mapped32(pr, rr6 + 4u)) {
                    j9_write32(pr, rr6 + 4u, jthis);
                }
                // Official lreturn restores {r4,r5,r6} from the parent
                // frame. Keep a valid class in nearby slots so the next
                // invoke does not ldr [r4+4] with r4=0.
                if (rr4) {
                    for (int k = 0; k < 24; ++k) {
                        const address ot = rr6 + static_cast<address>(k) * 4u;
                        if (!j9_mapped32(pr, ot + 8u)) {
                            continue;
                        }
                        const address w0 = j9_read32(pr, ot);
                        const address w1 = j9_read32(pr, ot + 4u);
                        if (j9_looks_caller_pc(pr, w1) && !w0) {
                            j9_write32(pr, ot, rr4);
                        }
                    }
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] ac-jcl-nopf r5=0x{:X} next=0x{:02X} -> 0x{:X} outer=0x{:X} this=0x{:X}",
                    r5, nop, resume, g_j9_jcl_outer_r5, jthis);
                g_j9_saved_r4 = rr4;
                g_j9_saved_r5 = rr5;
                g_j9_saved_r6 = rr6;
                g_j9_java_sp = rr6 ? (rr6 + 4u) : core->get_reg(7);
                g_j9_resume_no_ac = true;
                g_j9_resume_at = resume;
                j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            core->set_reg(0, core->get_reg(4));
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(0x818F660Cu);
            return;
        }
        if (is_invoke && (pc == 0x81910BB0u)) {
            const address r5 = core->get_reg(5);
            const address r6 = core->get_reg(6);
            const address r7 = core->get_reg(7);
            if (g_j9_util_conv_done && (r5 == 0x8195ACF7u)
                && j9_is_java_fp(r6) && r7 && j9_mapped32(pr, r7 + 12u)
                && (j9_read32(pr, r7 + 8u) == 0x8195A7F5u)) {
                const address dummy = j9_ensure_converter_dummy(pr);
                const address sr4 = j9_read32(pr, r7 + 4u);
                const address sfp = j9_read32(pr, r7 + 12u);
                j9_save_string_ret(pr, sfp);
                address sosp = (sfp >= 4u) ? (sfp - 4u) : sfp;
                if (dummy && sosp && j9_mapped32(pr, sosp)) {
                    j9_write32(pr, sosp, dummy);
                }
                if (dummy && sfp && j9_mapped32(pr, sfp + 16u)) {
                    j9_write32(pr, sfp + 16u, dummy);
                }
                j9_seed_string_if_needed(pr, sfp);
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] ad-util-to-string r4=0x{:X} r6=0x{:X} dummy=0x{:X}",
                    sr4, sfp, dummy);
                g_j9_saved_r4 = sr4;
                g_j9_saved_r5 = 0x8195A7F5u;
                g_j9_saved_r6 = sfp;
                g_j9_java_sp = sosp;
                g_j9_resume_no_ac = true;
                g_j9_resume_at = 0x8195A7F8u;
                j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                return;
            }
            if (g_j9_jcl_returned && j9_is_return_op(j9_read8(pr, r5))) {
                address val = j9_read32(pr, r7);
                if (!j9_looks_heap(val)) {
                    val = g_j9_jcl_this ? g_j9_jcl_this : g_j9_graphics_obj;
                }
                address parent = 0;
                address ploc = 0;
                for (int i = 0; i < 10; ++i) {
                    const address w = j9_read32(pr, r7 + static_cast<address>(i) * 4u);
                    if (!w || (w == g_j9_last_jxe_r5) || (w == g_j9_jcl_outer_r5)
                        || (w == g_j9_jcl_r5) || (w < 0x00770000u) || (w >= 0x00780000u)
                        || !j9_is_invoke_op(j9_read8(pr, w))) {
                        continue;
                    }
                    const address loc = j9_read32(pr,
                        r7 + static_cast<address>(i + 1) * 4u);
                    if ((loc >= 0x00710000u) && (loc < 0x00720000u)) {
                        parent = w;
                        ploc = loc;
                        break;
                    }
                }
                if (parent) {
                    address next = parent + 3u;
                    if ((j9_read8(pr, next) == 0xC0u)
                        && (j9_read8(pr, next + 3u) == 0x59u)
                        && j9_is_return_op(j9_read8(pr, next + 5u))) {
                        for (int k = -8; k < 24; ++k) {
                            const address at = ploc + static_cast<address>(k) * 4u;
                            const address w = j9_read32(pr, at);
                            if ((w >= 0x00770000u) && (w < 0x00780000u)
                                && j9_is_invoke_op(j9_read8(pr, w))) {
                                const address loc = j9_read32(pr, at + 4u);
                                parent = w;
                                if ((loc >= 0x00710000u) && (loc < 0x00720000u)) {
                                    ploc = loc;
                                }
                                next = w + 3u;
                                break;
                            }
                        }
                    }
                    const address sp = ploc + 4u;
                    if (sp && j9_mapped32(pr, sp)) {
                        j9_write32(pr, sp, val);
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-ad r5=0x{:X} -> 0x{:X} r6=0x{:X} val=0x{:X}",
                        r5, next, ploc, val);
                    g_j9_saved_r4 = g_j9_saved_r4 ? g_j9_saved_r4 : g_j9_last_jxe_r4;
                    g_j9_saved_r5 = parent;
                    g_j9_saved_r6 = ploc;
                    g_j9_java_sp = sp;
                    g_j9_resume_no_ac = true;
                    g_j9_resume_at = next;
                    j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                    g_j9_resume_no_ac = false;
                    g_j9_resume_at = 0;
                    return;
                }
            }
            core->set_reg(12, j9_read32(pr, r7));
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && (pc == 0x81910B94u)) {
            const address ac_r5 = core->get_reg(5);
            const address ac_r6 = core->get_reg(6);
            if ((g_j9_string_filled || g_j9_string_astore_done) && ac_r6
                && j9_is_java_fp(ac_r6)
                && ((ac_r5 >= 0x8195A7F0u) && (ac_r5 <= 0x8195A830u))) {
                const address thiz = j9_obj_from_slot(pr, j9_read32(pr, ac_r6));
                j9_dump_string_frame(pr, core, ac_r6, "ac-op");
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] conv-ac-keep this=0x{:X} r5=0x{:X} r6=0x{:X}",
                    thiz, ac_r5, ac_r6);
                // Official AC ldmda uses [r7]/[r7+4]/[r7+8] as
                // {class, invoke-pc, caller-fp}. r7 is fp-4, so planting
                // there would smash String locals. Leave into the JNI
                // interpret sentinel instead: 0xFF at 0x81922C1D does
                // `ldr pc, [r6, #-0x10]` with r6=0x71E600.
                const address head = 0x71E600u;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] conv-ac-leave this=0x{:X} head=0x{:X} "
                    "link=0x{:X} h-10=0x{:X}/0x{:X}/0x{:X}/0x{:X}",
                    thiz, head, j9_read32(pr, head - 0x10u),
                    j9_read32(pr, head - 0x10u), j9_read32(pr, head - 0x0Cu),
                    j9_read32(pr, head - 0x08u), j9_read32(pr, head - 0x04u));
                j9_dump_cstack_frames(pr, core, "ac-leave");
                j9_try_capture_live_cframe(pr, core);
                if (g_j9_main_clazz) {
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] conv-ac-leave-main-skip clazz=0x{:X}", g_j9_main_clazz);
                    return;
                }
                if (j9_return_to_jni_call(core, pr, thiz)) {
                    return;
                }
                if (j9_return_to_interp_c(core, pr, thiz, g_j9_vmthread, true)) {
                    return;
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] conv-ac-leave-cmiss this=0x{:X}", thiz);
                return;
            }
            // Official 0xAC. Only rewrite the frame when we just
            // stubbed a JXE invokevirtual and the next ireturn is the
            // caller's (r6 matches the pending locals).
            const address r5 = ac_r5;
            const address r6 = core->get_reg(6);
            if (g_j9_jcl_returned && g_j9_jcl_outer_r5
                && (r5 >= g_j9_jcl_outer_r5) && (r5 < (g_j9_jcl_outer_r5 + 16u))) {
                const std::uint8_t op = j9_read8(pr, r5);
                if ((op == 0xADu) || (op == 0xACu) || (op == 0xB0u) || (op == 0xB1u)) {
                    if (j9_finish_jcl_to_jxe(core, pr)) {
                        return;
                    }
                }
            }
            if (j9_looks_jcl_pc(r5) && (j9_read8(pr, r5) == 0xACu)
                && g_j9_jcl_r5 && !g_j9_jcl_returned
                && (r5 >= g_j9_jcl_r5) && (r5 < (g_j9_jcl_r5 + 0x40u))) {
                j9_scan_java_caller(pr, g_j9_jcl_r6);
                const address jthis = j9_ensure_jcl_this(pr);
                if (g_j9_jcl_outer_r5 && j9_looks_caller_pc(pr, g_j9_jcl_outer_r5)) {
                    const address next = g_j9_jcl_outer_r5 + 3u;
                    address sp = g_j9_jcl_outer_r6 ? (g_j9_jcl_outer_r6 + 4u) : 0;
                    if (sp && j9_mapped32(pr, sp)) {
                        j9_write32(pr, sp, jthis);
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-aret r4=0x{:X} from=0x{:X} -> 0x{:X} this=0x{:X}",
                        g_j9_jcl_outer_r4, r5, next, jthis);
                    g_j9_jcl_returned = true;
                    j9_plant_post_jcl_r4fix(pr);
                    g_j9_saved_r4 = g_j9_jcl_outer_r4;
                    g_j9_saved_r5 = g_j9_jcl_outer_r5;
                    g_j9_saved_r6 = g_j9_jcl_outer_r6;
                    g_j9_java_sp = sp;
                    g_j9_resume_no_ac = true;
                    g_j9_resume_at = next;
                    j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                    g_j9_resume_no_ac = false;
                    g_j9_resume_at = 0;
                    return;
                }
            }
            const bool jxe_ac = (r5 >= 0x00770000u) && (r5 < 0x00780000u)
                && (j9_read8(pr, r5) == 0xACu);
            if (jxe_ac && g_j9_pending_ac_r6 && (r6 == g_j9_pending_ac_r6)
                && g_j9_jcl_r5) {
                const address after_pf = g_j9_jcl_r5 + 6u;
                const address pf = g_j9_jcl_r5 + 3u;
                if (j9_read8(pr, pf) == 0xB5u) {
                    // JCL is `invokevirtual; putfield`. Restore the
                    // instance (local0 was wiped by the JXE helper) and
                    // let the interpreter putfield run with a real this
                    // plus the helper's return value.
                    const address jthis = j9_ensure_jcl_this(pr);
                    const address val = g_j9_last_java_obj
                        ? g_j9_last_java_obj
                        : (g_j9_graphics_obj ? g_j9_graphics_obj
                            : (jthis ? jthis : 240u));
                    // Official putfield resolve AVs (J9RAMFieldRef is a
                    // leftover UTF8). Store the helper result on the
                    // instance ourselves and continue at aload_0.
                    if (jthis && val && j9_mapped32(pr, jthis + 0x20u)) {
                        for (int i = 2; i < 8; ++i) {
                            const address at = jthis + static_cast<address>(i) * 4u;
                            if (j9_mapped32(pr, at)) {
                                j9_write32(pr, at, val);
                            }
                        }
                    }
                    // Skip putfield/getfield/arraylength and return into
                    // the Java caller of this Graphics constructor.
                    j9_scan_java_caller(pr, g_j9_jcl_r6);
                    address ret_r4 = g_j9_jcl_outer_r4 ? g_j9_jcl_outer_r4 : g_j9_jcl_r4;
                    address ret_r5 = g_j9_jcl_outer_r5;
                    address ret_r6 = g_j9_jcl_outer_r6 ? g_j9_jcl_outer_r6 : g_j9_jcl_r6;
                    address resume = 0;
                    if (ret_r5 && j9_looks_caller_pc(pr, ret_r5)) {
                        resume = ret_r5 + 3u;
                    } else {
                        // No caller frame: do not dispatch the helper's
                        // own 0xAC (official handler AVs). Stay in the
                        // constructor's aload_0/getfield tail with this
                        // restored so later LCDUI _create can run.
                        resume = g_j9_jcl_r5 ? (g_j9_jcl_r5 + 6u) : 0x8195A86Au;
                        ret_r4 = g_j9_jcl_r4;
                        ret_r5 = g_j9_jcl_r5;
                        ret_r6 = g_j9_jcl_r6;
                    }
                    address sp = ret_r6 ? (ret_r6 + 4u) : 0;
                    if (sp && j9_mapped32(pr, sp)) {
                        j9_write32(pr, sp, jthis ? jthis : val);
                    }
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] ac-jcl-putf r4=0x{:X} from=0x{:X} ret->0x{:X} this=0x{:X} val=0x{:X} outer=0x{:X}",
                        ret_r4, r5, resume, jthis, val, g_j9_jcl_outer_r5);
                    g_j9_saved_r4 = ret_r4;
                    g_j9_saved_r5 = ret_r5;
                    g_j9_saved_r6 = ret_r6;
                    g_j9_java_sp = sp;
                    g_j9_pending_ac_r6 = 0;
                    g_j9_resume_no_ac = true;
                    g_j9_resume_at = resume;
                    (void)pf;
                    (void)after_pf;
                    j9_jxe_resume_interp(core, pr, g_j9_tramp_method, 0);
                    g_j9_resume_no_ac = false;
                    g_j9_resume_at = 0;
                    return;
                }
                const address r7 = core->get_reg(7);
                j9_write32(pr, r7, g_j9_jcl_r4);
                j9_write32(pr, r7 + 4u, g_j9_jcl_r5);
                j9_write32(pr, r7 + 8u, g_j9_jcl_r6);
                // After this handler, r7 becomes pending_r6+4 and JCL
                // does putfield. Leave {value, this} where that SP
                // will pop them (stack grows down).
                const address slot = g_j9_pending_ac_r6 + 4u;
                address jthis = j9_read32(pr, g_j9_jcl_r6);
                if (!jthis || (jthis < 0x10000u)) {
                    for (int i = -4; i < 8; ++i) {
                        const address c = j9_read32(pr,
                            g_j9_jcl_r6 + static_cast<address>(i) * 4u);
                        if ((c >= 0x02000000u) && (c < 0x03000000u)
                            && ((c & 3u) == 0)) {
                            jthis = c;
                            break;
                        }
                    }
                }
                if (!jthis || (jthis < 0x10000u)) {
                    jthis = g_j9_jcl_this ? g_j9_jcl_this
                        : (g_j9_last_java_obj ? g_j9_last_java_obj
                            : g_j9_graphics_obj);
                }
                const address val = g_j9_last_java_obj ? g_j9_last_java_obj
                    : (g_j9_graphics_obj ? g_j9_graphics_obj : 240u);
                if (j9_mapped32(pr, slot + 4u)) {
                    j9_write32(pr, slot, val);
                    if (jthis) {
                        j9_write32(pr, slot + 4u, jthis);
                    }
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] ac-jcl r7=0x{:X} r4=0x{:X} pc=0x{:X} r6=0x{:X} from=0x{:X} this=0x{:X} val=0x{:X}",
                    r7, g_j9_jcl_r4, g_j9_jcl_r5, g_j9_jcl_r6, r5, jthis, val);
                g_j9_pending_ac_r6 = 0;
            }
            core->set_reg(10, core->get_reg(7) + 8u);
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && ((pc == 0x81911DFCu) || (pc == 0x81911E60u))) {
            // Official JNI teardown. JCL ROM methods have a header at
            // [method]-4; type=JXE does not. Only steal the return when
            // the tramp snapshot is JXE RAM bytecode.
            const bool jxe = (g_j9_saved_r5 >= 0x00770000u)
                && (g_j9_saved_r5 < 0x00780000u);
            static int td_n = 0;
            if (td_n < 8) {
                ++td_n;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] jni-td pc=0x{:X} r0=0x{:X} r5=0x{:X} sv5=0x{:X} jxe={} lr=0x{:X}",
                    pc, core->get_reg(0), core->get_reg(5), g_j9_saved_r5,
                    jxe ? 1 : 0, core->get_lr());
            }
            if (jxe) {
                const address tm = g_j9_tramp_method ? g_j9_tramp_method : core->get_reg(0);
                j9_jxe_resume_interp(core, pr, tm, j9_method_argc(pr, tm));
                return;
            }
            core->set_cpsr(core->get_cpsr() & ~0x20u);
            if (pc == 0x81911DFCu) {
                core->set_lr(0xFFFFFFFCu);
            } else {
                core->set_reg(1, j9_read32(pr, core->get_reg(9)));
            }
            core->set_pc(pc + 4u);
            return;
        }
        if (is_invoke && ((pc == 0x81A61F5Cu) || (pc == 0x81A61F6Eu))) {
            // RThread::SetPriority wrappers, also used as a midp JNI
            // send. type=JXE must resume the interpreter, not official
            // JNI teardown.
            const address ret = core->get_lr() & ~1u;
            const bool from_adapt = g_j9_walk_va
                && (ret >= g_j9_walk_va) && (ret < (g_j9_walk_va + 0x8000u));
            static int pri_n = 0;
            if (pri_n < 12) {
                ++pri_n;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] pri-stub pc=0x{:X} r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} slot=0x{:X} lr=0x{:X} adapt={}",
                    pc, core->get_reg(0), core->get_reg(1), core->get_reg(2),
                    core->get_reg(3), j9_read32(pr, 0x3FFF0030u), core->get_lr(),
                    from_adapt ? 1 : 0);
            }
            j9_seed_midp_bss_types(pr, thr);
            if (from_adapt) {
                const address sp = core->get_reg(13);
                address method = j9_read32(pr, sp);
                if (!method || (method < 0x10000u) || (method == g_j9_fake_env)
                    || !j9_mapped32(pr, method) || (method == 0x60001u)) {
                    method = g_j9_tramp_method;
                }
                core->set_reg(13, sp + 8u);
                const address tm = (method && (method >= 0x10000u))
                    ? method : g_j9_tramp_method;
                j9_jxe_resume_interp(core, pr, tm, j9_method_argc(pr, tm));
                return;
            }
            core->set_reg(0, 0);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (is_invoke && (pc == 0x819171F0u)) {
            const address method = core->get_reg(0);
            if (method && (method > 0x10000u) && j9_mapped32(pr, method) && j9_mapped32(pr, method + 12u)) {
                const address raw_cp = j9_read32(pr, method + 4u);
                address valid = j9_find_valid_cp(pr, raw_cp);
                if (!valid && g_j9_valid_cp) valid = g_j9_valid_cp;
                if (valid) {
                    if ((raw_cp & ~7u) != valid) {
                        j9_write32(pr, method + 4u, valid);
                    }
                    g_j9_valid_cp = valid;
                }
                const address real_cp = j9_read32(pr, method + 4u) & ~7u;
                if (real_cp && j9_mapped32(pr, real_cp)) {
                    const address clazz = j9_read32(pr, real_cp);
                    if (clazz && j9_mapped32(pr, clazz + 0x10u)) {
                        const address rom_class = j9_read32(pr, clazz + 0x10u);
                        if (rom_class >= 0x80000000u && j9_mapped32(pr, rom_class + 0x24u)) {
                            const address rom_m_off = j9_read32(pr, rom_class + 0x20u);
                            const address first_rom_m = rom_class + 0x20u + rom_m_off;
                            if (first_rom_m >= 0x80000000u && j9_mapped32(pr, first_rom_m + 16u)) {
                                j9_thumb_continue_real(core, pr, pc);
                                return;
                            }
                        }
                    }
                }
            }
            core->set_reg(0, 0x81961A90u);
            j9_set_pc(core, core->get_lr());
            return;
        }
        if (is_invoke && (pc == 0x81AF2C12u)) {
            static int sc = 0;
            if (sc < 8) {
                ++sc;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] lcdui-setcurrent r0=0x{:X} r1=0x{:X} r2=0x{:X} r7=0x{:X}",
                    core->get_reg(0), core->get_reg(1), core->get_reg(2),
                    core->get_reg(7));
            }
            address packed = j9_pack_obj(pr, core->get_reg(1));
            if (!packed) {
                packed = j9_pack_obj(pr, core->get_reg(2));
            }
            if (!packed) {
                packed = j9_pack_obj(pr, j9_read32(pr, core->get_reg(7)));
            }
            if (packed) {
                j9_attach_dummy_peer(pr, thr, packed);
                g_j9_canvas_obj = packed << 2;
            }
            j9_seed_midp_bss_types(pr, thr);
            j9_thumb_continue_real(core, pr, pc);
            return;
        }
        if (is_invoke && ((pc == 0x81AF1616u) || (pc == 0x81AF2AACu) || (pc == 0x81AEE7F8u)
                || (pc == 0x81A61CC4u) || (pc == 0x81AF17F0u) || (pc == 0x81AF106Au))) {
            const address r0_in = core->get_reg(0);
            const address r1_in = core->get_reg(1);
            const address r2_in = core->get_reg(2);
            const address r3_in = core->get_reg(3);
            address packed = j9_pack_obj(pr, r1_in);
            if (!packed) {
                packed = j9_pack_obj(pr, r2_in);
            }
            if (!packed) {
                packed = j9_pack_obj(pr, j9_read32(pr, core->get_reg(7)));
            }
            if ((pc == 0x81A61CC4u) && r0_in && ((r0_in & 3u) == 0) && j9_looks_heap(r0_in)) {
                packed = r0_in >> 2;
            }
            if (!packed && (pc == 0x81AF1616u) && g_j9_graphics_obj) {
                packed = g_j9_graphics_obj >> 2;
            }
            if (!packed && (pc != 0x81A61CC4u)) {
                // Official JNI teardown AVs on type=JXE. Still resume
                // the interpreter; just skip attaching a peer.
                static int real_n = 0;
                if (real_n < 16) {
                    ++real_n;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] lcdui-nopc pc=0x{:X} r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} lr=0x{:X}",
                        pc, r0_in, r1_in, r2_in, r3_in, core->get_lr());
                }
            }
            j9_seed_midp_bss_types(pr, thr);
            const address peer = packed ? j9_attach_dummy_peer(pr, thr, packed) : 0;
            if (pc == 0x81AF2AACu) {
                g_j9_toolkit_obj = packed ? (packed << 2) : g_j9_toolkit_obj;
            } else if (pc == 0x81AEE7F8u) {
                g_j9_canvas_obj = packed ? (packed << 2) : g_j9_canvas_obj;
            } else if ((pc == 0x81AF1616u) && packed) {
                g_j9_graphics_obj = packed << 2;
            }
            if (pc == 0x81A61CC4u) {
                if (g_j9_alps_started) {
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] lcdui-create-stub r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} lr=0x{:X}",
                        r0_in, r1_in, r2_in, r3_in, core->get_lr());
                }
                // Called from Thumb _create, not the interpreter.
                if (r3_in && j9_mapped32(pr, r3_in)) {
                    j9_write32(pr, r3_in, peer ? peer : j9_alloc_dummy_peer(pr, thr));
                }
                core->set_reg(0, 0);
                j9_set_pc(core, core->get_lr());
                return;
            }
            static int create_logs = 0;
            if (create_logs < 16) {
                ++create_logs;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] lcdui-stub pc=0x{:X} r0=0x{:X} r1=0x{:X} r2=0x{:X} r3=0x{:X} packed=0x{:X} peer=0x{:X} tk=0x{:X} cv=0x{:X}",
                    pc, r0_in, r1_in, r2_in, r3_in, packed, peer,
                    g_j9_toolkit_obj, g_j9_canvas_obj);
            }
            if (g_j9_alps_started
                && ((pc == 0x81AF1616u) || (pc == 0x81AEE7F8u)
                    || (pc == 0x81AF2AACu))) {
                j9_set_pc(core, core->get_lr());
                return;
            }
            const address ret = core->get_lr() & ~1u;
            const bool from_adapt = g_j9_walk_va
                && (ret >= (g_j9_walk_va + k_j9_adapt_off))
                && (ret < (g_j9_walk_va + 0x8000u));
            address method = 0;
            if (from_adapt) {
                const address sp = core->get_reg(13);
                method = j9_read32(pr, sp);
                core->set_reg(13, sp + 8u);
            } else if (r0_in && (r0_in != g_j9_fake_env) && j9_mapped32(pr, r0_in)) {
                method = r0_in;
            }
            const address tm = g_j9_tramp_method ? g_j9_tramp_method : method;
            j9_jxe_resume_interp(core, pr, tm, j9_method_argc(pr, tm));
            return;
        }
        if (is_invoke) {
            const address hook_m = core->get_reg(0);
            if (pr && g_j9_cp_stub_meth && hook_m
                && ((hook_m == g_j9_cp_stub_meth)
                    || (g_j9_walk_va && (hook_m == (g_j9_walk_va + 0x3D80u))))) {
                j9_run_main_cp_stub(core, pr);
                return;
            }
            j9_ensure_method_cp(core, pr);
            // Interpreter `ldr pc, [r0, #8]` — r0 is J9Method*. type=JXE
            // leaves send=8 (unbound). Bound methods just resume send.
            // After official JNI compile (0x8193A13C) the same tramp is hit
            // with lr=0x8193A124; then send must be the adapter, not jni_entry.
            const address method = core->get_reg(0);
            const address lr = core->get_lr() & ~1u;
            const bool post_compile = (lr == 0x8193A124u) || (lr == 0x8193A288u);
            std::uint32_t *mw = nullptr;
            address send = 0;
            address extra = 0;
            if (pr && (method > 0x10000u) && j9_mapped32(pr, method) && j9_mapped32(pr, method + 12u)) {
                mw = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(method));
                if (mw) {
                    send = mw[2];
                    extra = mw[3];
                    if (mw[1] && (mw[1] > 0x10000u) && j9_mapped32(pr, mw[1] & ~7u)) {
                        g_j9_valid_cp = mw[1];
                    }
                }
            }
            if (!mw) {
                core->set_cpsr(core->get_cpsr() & ~0x20u);
                if (pc == 0x818F5A78u) {
                    core->set_pc(0x818F5118u);
                } else if (!method) {
                    core->set_pc(0x81910FACu);
                } else {
                    core->set_pc(0x81910F98u);
                }
                return;
            }
            const auto is_direct_send = [](const address s) {
                return j9_fn_is_code(s) && (s != k_j9_jni_entry) && (s != k_j9_inl_dispatch)
                    && (s >= 0x10000u);
            };
            const auto is_walk_adapter = [&](const address s) {
                return g_j9_walk_va && is_direct_send(s) && ((s & 1u) == 0)
                    && (s >= (g_j9_walk_va + k_j9_adapt_off))
                    && (s < (g_j9_walk_va + 0x8000u));
            };
            const bool jni_post_site = (pc >= 0x8193A000u) && (pc < 0x8193A800u);
            const address thumb = ((send & 1u) && ((send & ~1u) >= 0x81AE0000u)
                && ((send & ~1u) < 0x81B20000u))
                ? send
                : 0;
            address ad = is_walk_adapter(send) ? send : j9_adapter_for_method(method);
            if (!ad && thumb) {
                ad = j9_adapter_for(thumb);
            }
            if (ad && is_walk_adapter(ad) && (post_compile || jni_post_site)) {
                j9_marshal_java_args(core, pr, mw);
                j9_set_pc(core, ad);
                return;
            }
            if (jni_post_site) {
                // markTime and other non-LCDUI: resume whatever send compile left.
                if (is_direct_send(send) && (send != k_j9_jni_entry)) {
                    j9_set_pc(core, send);
                } else {
                    core->set_pc(pc + 4);
                }
                return;
            }
            if (ad && is_walk_adapter(ad)) {
                if (mw) {
                    mw[3] = 1u;
                }
                j9_set_pc(core, k_j9_jni_entry);
                return;
            }
            // bindNative may overwrite extra/send to the Thumb fixer stub.
            if (((send & 1u) && j9_fn_is_code(send) && (send < 0x80000000u))
                || ((extra > 1u) && (extra & 1u) && j9_fn_is_code(extra) && (extra < 0x80000000u))) {
                const address thumb = ((send & 1u) && j9_fn_is_code(send)) ? send : extra;
                if (mw) {
                    j9_plant_method(mw, thumb);
                    send = mw[2];
                    extra = mw[3];
                }
            }
            if ((extra == 1u) || (send == k_j9_jni_entry)) {
                if (post_compile) {
                    address target = send;
                    if (!is_direct_send(target) || (target == k_j9_jni_entry)) {
                        if (const address gen = j9_generic_jni_send(pr, core)) {
                            target = gen;
                        } else if (g_j9_last_adapt) {
                            target = g_j9_last_adapt;
                        }
                    }
                    if (is_direct_send(target) && (target != k_j9_jni_entry)) {
                        if (mw) {
                            mw[2] = target;
                        }
                        static int post_logs = 0;
                        if (post_logs < 24) {
                            ++post_logs;
                            const address r7 = core->get_reg(7);
                            const address r8v = core->get_reg(8);
                            LOG_WARN(EMULATED_STDOUT,
                                "[j9-nf] jni-post send=0x{:X} -> 0x{:X} lr=0x{:X} r7=0x{:X} [0]=0x{:X} [4]=0x{:X} [8]=0x{:X} tsp=0x{:X}",
                                send, target, lr, r7,
                                j9_read32(pr, r7), j9_read32(pr, r7 + 4), j9_read32(pr, r7 + 8),
                                j9_read32(pr, r8v + 8));
                        }
                        j9_set_pc(core, target);
                        return;
                    }
                    if (g_j9_last_adapt) {
                        j9_set_pc(core, g_j9_last_adapt);
                        return;
                    }
                }
                j9_set_pc(core, k_j9_jni_entry);
                return;
            }
            if (j9_fn_is_code(send) && (send >= 0x80000000u) && (send != k_j9_inl_dispatch)) {
                if (((send & ~1u) >= 0x81A5B000u) && ((send & ~1u) < 0x81B20000u)
                    && (send & 1u)) {
                    if (mw) {
                        j9_plant_method(mw, send);
                    }
                    j9_set_pc(core, k_j9_jni_entry);
                    return;
                }
                j9_set_pc(core, send);
                return;
            }
            // extra even: non-JNI method (interpreted bytecode or ARM INL handler)
            if ((extra & 1u) == 0) {
                if ((extra >= 0x10000u) && j9_fn_is_code(extra)) {
                    if (mw) {
                        mw[2] = extra;
                    }
                    j9_set_pc(core, extra);
                    return;
                }
                if (j9_fn_is_code(send) && (send != k_j9_jni_entry) && (send != k_j9_inl_dispatch)) {
                    j9_set_pc(core, send);
                    return;
                }
                // Bind uncompiled/interpreted method and run via J9's helper at 0x818F73B4
                // (which loads [vm + 0x6c8], stores to [r0 + 8], and branches to it)
                j9_set_pc(core, 0x818F73B4u);
                return;
            }
            if ((method < 0x70000u) || (method == 0x60001u) || !mw) {
                address pr4 = 0;
                address pr6 = 0;
                const bool got = g_j9_saved_r5
                    && (j9_pick_caller_regs(g_j9_saved_r5, &pr4, &pr6)
                        || (g_j9_good_r4 && ((pr4 = g_j9_good_r4),
                            (pr6 = g_j9_good_r6), true)))
                    && pr4;
                if (got && (g_j9_unbound_retry < 2) && (pr4 != g_j9_saved_r4)) {
                    ++g_j9_unbound_retry;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] invoke-unbound r4fix meth=0x{:X} r4=0x{:X}->0x{:X} r6=0x{:X} pc=0x{:X}",
                        method, g_j9_saved_r4, pr4, pr6, g_j9_saved_r5);
                    g_j9_saved_r4 = pr4;
                    if (pr6) {
                        g_j9_saved_r6 = pr6;
                    }
                    g_j9_resume_no_ac = true;
                    g_j9_resume_at = g_j9_saved_r5;
                    const address tm = g_j9_tramp_method;
                    j9_jxe_resume_interp(core, pr, tm, 0);
                    g_j9_resume_no_ac = false;
                    g_j9_resume_at = 0;
                    return;
                }
                const address tm = g_j9_tramp_method;
                if (g_j9_saved_r5 && g_j9_saved_r6 && (g_j9_unbound_retry < 1)
                    && (j9_read8(pr, g_j9_saved_r5) == 0xB6u)) {
                    address recv = g_j9_last_java_obj;
                    if (!recv) {
                        for (int i = 0; i < 6; ++i) {
                            const address cand = j9_read32(pr,
                                g_j9_saved_r6 + static_cast<address>(i) * 4u);
                            if ((cand >= 0x02000000u) && (cand < 0x03000000u)
                                && ((cand & 3u) == 0) && j9_mapped32(pr, cand)) {
                                recv = cand;
                                break;
                            }
                        }
                    }
                    if (recv) {
                        ++g_j9_unbound_retry;
                        j9_write32(pr, g_j9_saved_r6 + 8u, recv);
                        if (g_j9_java_sp && j9_mapped32(pr, g_j9_java_sp)) {
                            j9_write32(pr, g_j9_java_sp, recv);
                        }
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] invoke-b6-recv meth=0x{:X} recv=0x{:X} pc=0x{:X}",
                            method, recv, g_j9_saved_r5);
                        g_j9_resume_no_ac = true;
                        g_j9_resume_at = g_j9_saved_r5;
                        j9_jxe_resume_interp(core, pr, tm, 0);
                        g_j9_resume_no_ac = false;
                        g_j9_resume_at = 0;
                        return;
                    }
                }
                const address next_pc = g_j9_saved_r5 ? (g_j9_saved_r5 + 3u) : 0;
                const std::uint8_t next_op = next_pc ? j9_read8(pr, next_pc) : 0;
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] invoke-unbound skip meth=0x{:X} send=0x{:X} extra=0x{:X} lr=0x{:X} r4=0x{:X} sv5=0x{:X} next=0x{:02X} start=0x{:X}",
                    method, send, extra, lr, g_j9_saved_r4, g_j9_saved_r5,
                    next_op, g_j9_method_start);
                if (next_op == 0xACu) {
                    address cr4 = 0;
                    address cr5 = 0;
                    address cr6 = 0;
                    const address hint = g_j9_method_start
                        ? g_j9_method_start : g_j9_saved_r5;
                    if (j9_find_jxe_caller(hint, &cr4, &cr5, &cr6)
                        && cr5 && j9_is_invoke_op(j9_read8(pr, cr5))) {
                        const address next = cr5 + 3u;
                        std::uint8_t cop = j9_read8(pr, next);
                        // getGraphics after Image/Graphics._create, else
                        // getWidth-style int. Leave the value in the
                        // caller's return slot for the following putstatic.
                        for (int i = 0; i < 6; ++i) {
                            const address cand = j9_read32(pr,
                                g_j9_saved_r6 + static_cast<address>(i) * 4u);
                            if ((cand >= 0x02000000u) && (cand < 0x03000000u)
                                && ((cand & 3u) == 0) && j9_mapped32(pr, cand)) {
                                const address clazz = j9_read32(pr, cand);
                                if (clazz && j9_mapped32(pr, clazz)
                                    && (((clazz >= 0x00700000u) && (clazz < 0x00800000u))
                                        || ((clazz >= 0x02000000u) && (clazz < 0x03000000u))
                                        || ((clazz >= 0x81800000u) && (clazz < 0x82000000u)))) {
                                    g_j9_last_java_obj = cand;
                                    break;
                                }
                            }
                        }
                        const address retv = g_j9_last_java_obj
                            ? g_j9_last_java_obj
                            : (g_j9_graphics_obj ? g_j9_graphics_obj : 240u);
                        if (cr6 && j9_mapped32(pr, cr6 + 4u)) {
                            j9_write32(pr, cr6 + 4u, retv);
                        }
                        g_j9_pending_ac_r6 = cr6;
                        LOG_WARN(EMULATED_STDOUT,
                            "[j9-nf] ac-outer-ret r4=0x{:X} pc=0x{:X} next=0x{:X} op=0x{:02X} r6=0x{:X} from=0x{:X} ret=0x{:X} bc={:02X}{:02X}{:02X}{:02X} loc=0x{:X}/0x{:X}/0x{:X}",
                            cr4, cr5, next, cop, cr6, g_j9_saved_r5, retv,
                            j9_read8(pr, g_j9_saved_r5),
                            j9_read8(pr, g_j9_saved_r5 + 1u),
                            j9_read8(pr, g_j9_saved_r5 + 2u),
                            j9_read8(pr, g_j9_saved_r5 + 3u),
                            j9_read32(pr, g_j9_saved_r6),
                            j9_read32(pr, g_j9_saved_r6 + 4u),
                            j9_read32(pr, g_j9_saved_r6 + 8u));
                        if (cr4) {
                            g_j9_saved_r4 = cr4;
                            core->set_reg(4, cr4);
                        }
                        g_j9_saved_r5 = cr5;
                        g_j9_saved_r6 = cr6;
                        if (cr6) {
                            g_j9_java_sp = cr6 + 4u;
                        }
                        g_j9_resume_no_ac = true;
                        g_j9_resume_at = next;
                        j9_jxe_resume_interp(core, pr, tm, 0);
                        g_j9_resume_no_ac = false;
                        g_j9_resume_at = 0;
                        return;
                    }
                    g_j9_force_caller = hint;
                    j9_jxe_resume_interp(core, pr, tm, 0);
                    g_j9_force_caller = 0;
                } else {
                    g_j9_resume_no_ac = true;
                    j9_jxe_resume_interp(core, pr, tm, 0);
                    g_j9_resume_no_ac = false;
                }
                return;
            }
            address fn = j9_bind_from_method(pr, method);
            if (!fn) {
                fn = j9_bind_from_regs(core, pr);
            }
            if (!fn && g_j9_last_thumb) {
                fn = g_j9_last_thumb;
            }
            if (!fn && g_j9_walk_va) {
                fn = (g_j9_walk_va + 0x27C) | 1u;
            }
            if (fn && mw && j9_fn_is_code(fn)) {
                j9_plant_method(mw, fn);
            }
            static int inv_logs = 0;
            if (inv_logs < 48) {
                ++inv_logs;
                const char *nm = j9_guest_cstr(pr, mw ? mw[0] : 0);
                if (!nm && mw) {
                    nm = j9_guest_cstr(pr, mw[0] + 2);
                }
                LOG_WARN(EMULATED_STDOUT,
                    "[j9-nf] invoke-unbound meth=0x{:X} send=0x{:X} extra=0x{:X} -> 0x{:X} name='{}' w0=0x{:X} lr=0x{:X}",
                    method, send, extra, fn, nm ? nm : "?", mw ? mw[0] : 0, lr);
            }
            // Never resume a type=JXE leftover (send=8) or an INL id.
            if (mw) {
                j9_plant_method(mw, j9_fn_is_code(fn) ? fn : ((g_j9_walk_va + 0x27C) | 1u));
            }
            if (post_compile && g_j9_last_adapt) {
                j9_set_pc(core, g_j9_last_adapt);
                return;
            }
            j9_set_pc(core, (fn & 1u) || !j9_fn_is_code(fn) ? k_j9_jni_entry : fn);
            return;
        }

        if (g_j9_walk_hit_bkpt && (pc == (g_j9_walk_hit_bkpt & ~1u))) {
            const char *s = j9_guest_cstr(pr, core->get_reg(2));
            const address pair = core->get_reg(4);
            const address func_out = core->get_reg(8);
            address fn = 0;
            if (pr && pair) {
                if (const auto *pw = reinterpret_cast<const std::uint32_t *>(
                        pr->get_ptr_on_addr_space(pair))) {
                    fn = pw[1];
                    if (!j9_fn_is_code(fn) && j9_fn_is_code(pw[0])) {
                        fn = pw[0];
                    }
                }
            }
            if (fn && j9_fn_is_code(fn) && pr && (func_out > 0x400000u) && (func_out < 0x800000u)) {
                const address method = func_out - 0xCu;
                if (auto *mw = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(method))) {
                    j9_plant_method(mw, fn, method);
                    if (mw[0]) {
                        if (auto *fl = reinterpret_cast<std::uint32_t *>(
                                pr->get_ptr_on_addr_space(mw[0] - 0xCu))) {
                            *fl |= 0x100u;
                        }
                    }
                }
            }
            static int hit_logs = 0;
            if (hit_logs < 80) {
                ++hit_logs;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] walker-hit '{}' fn=0x{:X} out=0x{:X}",
                    s ? s : "?", fn, func_out);
            }
            core->set_pc(pc + 4);
            return;
        }

        if (g_j9_walk_miss_bkpt && (pc == (g_j9_walk_miss_bkpt & ~1u))) {
            // Walker miss: r2 is the sl_lookup name. Frame is the walker's
            // push {r0-r8,lr} then the Thumb-saved port then bindNative's
            // {func_out, sl}. Prefix-match unsigned JNI names and plant
            // extra/send on the J9Method so invoke does not jump to 0x8.
            const char *s = j9_guest_cstr(pr, core->get_reg(2));
            std::string matched;
            const address fn = s ? j9_lookup_jni_prefix(s, &matched) : 0;
            const address func_out = core->get_reg(8);
            const address sp = core->get_reg(13);
            auto *st = reinterpret_cast<std::uint32_t *>(
                pr ? pr->get_ptr_on_addr_space(sp) : nullptr);
            bool planted = false;
            address method = 0;
            if (fn && j9_fn_is_code(fn) && pr && (func_out > 0x1000u)) {
                method = func_out - 0xCu;
                if (auto *mw = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(method))) {
                    j9_plant_method(mw, fn, method);
                    planted = true;
                }
                if (auto *slot = reinterpret_cast<address *>(pr->get_ptr_on_addr_space(func_out))) {
                    *slot = fn;
                    planted = true;
                }
            }
            if (planted) {
                static int bind_logs = 0;
                if (bind_logs < 48) {
                    ++bind_logs;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] walker-prefix '{}' -> {} @0x{:X} out=0x{:X} meth=0x{:X}",
                        s ? s : "?", matched, fn, func_out, method);
                }
                // extra/send already planted. Let orig_wrap miss so
                // bindNative sees extra!=0 and uses send (do not fake r0=0).
                core->set_pc(pc + 4);
                return;
            }
            static int miss_logs = 0;
            if (miss_logs < 48) {
                ++miss_logs;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] walker-miss '{}' r2=0x{:X} out=0x{:X}",
                    s ? s : "?", core->get_reg(2), func_out);
            }
            core->set_pc(pc + 4);
            return;
        }

        if (pc == (g_j9_mangle_bkpt & ~1u)) {
            // Displaced `movs r0, r6` at the Java_* mangler return.
            const address name_ptr = core->get_reg(6);
            const char *s = j9_guest_cstr(pr, name_ptr);
            core->set_reg(0, name_ptr);
            if (s && (std::memcmp(s, "Java_", 5) == 0)) {
                g_j9_last_java_name = s;
            }
            core->set_pc(pc + 2);
            return;
        }

        if (pc == (g_j9_bind_fail_bkpt & ~1u)) {
            // JXE-only sl_lookup (fp==0). r2 is the constructed native name.
            // Hit: fill *func_out and return 0 via `bx lr` (lr = ARM after the
            // patched BLX). Miss: run the original Thumb sl_lookup wrapper.
            const char *s = j9_guest_cstr(pr, core->get_reg(2));
            const bool ours = s && ((std::strstr(s, "symbian") != nullptr)
                || (std::strstr(s, "nokia") != nullptr)
                || (std::strstr(s, "markTime") != nullptr)
                || (std::strncmp(s, "Java_com_", 9) == 0));
            address fn = 0;
            if (ours) {
                fn = j9_lookup_jni_export(s);
            }
            if (fn && pr) {
                const address sp = core->get_reg(13);
                if (const auto *slotp = reinterpret_cast<const std::uint32_t *>(
                        pr->get_ptr_on_addr_space(sp))) {
                    if (auto *out = reinterpret_cast<address *>(pr->get_ptr_on_addr_space(*slotp))) {
                        *out = fn;
                    }
                }
                // lookup() saved the J9Method* at [sp,#0x2c] before two pushes.
                if (const auto *mp = reinterpret_cast<const std::uint32_t *>(
                        pr->get_ptr_on_addr_space(sp + 0x34))) {
                    const address method = *mp;
                    if (auto *m = reinterpret_cast<std::uint32_t *>(pr->get_ptr_on_addr_space(method))) {
                        m[2] = fn;           // extra / send
                        m[3] = fn | 1u;      // native | bound
                    }
                }
                core->set_reg(0, 0);
                static int bind_logs = 0;
                if (bind_logs < 32) {
                    ++bind_logs;
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] jxe-sl bound '{}' -> 0x{:X}",
                        s ? s : g_j9_last_java_name.c_str(), fn);
                }
                g_j9_last_java_name.clear();
                core->set_pc(pc + 2); // cmp r0,#0; beq; bx lr
                return;
            }
            static int miss_logs = 0;
            if ((miss_logs < 2) && s) {
                ++miss_logs;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] jxe-sl miss '{}' r2=0x{:X}", s, core->get_reg(2));
            }
            g_j9_last_java_name.clear();
            // Tail-call the original Thumb wrapper via `bx ip` (ip = orig|1).
            // r0-r3 stay as the sl_lookup arguments; lr is still the ARM
            // return (call+4) from the patched BLX.
            if (g_j9_lookup_fn) {
                core->set_reg(12, g_j9_lookup_fn | 1u);
            }
            core->set_pc(pc + 2);
            return;
        }

        if (pc == (g_j9_sl_bkpt & ~1u)) {
            // Displaced `add sp, #0x10c` after sl_lookup_name. Only fill a
            // failed lookup when the name argument is a Java_* symbol from
            // midp2ams — never hijack J9GetJXE / iveLoadJxe.
            if (core->get_reg(0) == 0) {
                const std::uint32_t name_cands[2] = { core->get_reg(5), core->get_reg(6) };
                for (std::uint32_t p : name_cands) {
                    const char *s = j9_guest_cstr(pr, p);
                    if (!s || (std::memcmp(s, "Java_", 5) != 0)) {
                        continue;
                    }
                    if (const address fn = j9_lookup_jni_export(s)) {
                        core->set_reg(0, fn);
                        LOG_WARN(EMULATED_STDOUT, "[j9-nf] sl-bound '{}' -> 0x{:X}", s, fn);
                    }
                    break;
                }
            }
            core->set_reg(13, core->get_reg(13) + 0x10c);
            core->set_pc(pc + 2);
            return;
        }

        if (pc == (g_j9_sl_call_bkpt & ~1u)) {
            // Walker reached the Java_com_s*/n* table walk. r4=name, r5=func_out.
            const char *s = j9_guest_cstr(pr, core->get_reg(4));
            const address func_out = core->get_reg(5);
            const address walk_base = pc & ~0xFFu;
            if (s) {
                if (const address fn = j9_lookup_jni_export(s)) {
                    if (pr && func_out) {
                        if (auto *slot = reinterpret_cast<address *>(pr->get_ptr_on_addr_space(func_out))) {
                            *slot = fn;
                        }
                    }
                    core->set_reg(0, 0);
                    if (auto *saved = reinterpret_cast<address *>(pr ? pr->get_ptr_on_addr_space(core->get_reg(13)) : nullptr)) {
                        *saved = 0;
                    }
                    core->set_pc((g_j9_sl_call_bkpt & ~1u) + 2); // success movs r0,#0
                    LOG_WARN(EMULATED_STDOUT, "[j9-nf] walker-bound '{}' -> 0x{:X}", s, fn);
                    return;
                }
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] walker-miss '{}'", s);
            } else {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] walker-hit r4=0x{:X} not a cstr", core->get_reg(4));
            }
            (void)walk_base;
            core->set_pc((g_j9_sl_call_bkpt & ~1u) + 8); // fallback sl_lookup
            return;
        }
    }

    static std::uint8_t *j9_guest_write(kernel::process *pr, std::uint8_t *base,
        const address run_addr, const std::uint32_t off) {
        if (pr) {
            if (auto *p = reinterpret_cast<std::uint8_t *>(pr->get_ptr_on_addr_space(run_addr + off))) {
                return p;
            }
        }
        return base ? (base + off) : nullptr;
    }

    static void apply_j9_bindnatv_hook(codeseg_ptr cs) {
        std::uint8_t *base = nullptr;
        const address run_addr = cs->get_code_run_addr(nullptr, &base);
        if (!run_addr || !base) {
            return;
        }
        kernel_system *kern = cs->get_kernel_object_owner();
        kernel::process *pr = kern ? kern->crr_process() : nullptr;
        if (!kern || (j9_jni_exports.size() < 50)) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI guest walker skipped (table={}) in {}",
                j9_jni_exports.size(), cs->name());
            return;
        }

        const std::uint32_t code_size = cs->get_code_size();
        const std::uint32_t scan = (code_size > 0x80000) ? 0x80000 : code_size;

        // fp==0 (type=JXE) sl_lookup only — not the DLL-export hot path.
        static const std::uint8_t call_sig[] = {
            0x38, 0x10, 0x9D, 0xE5, 0x06, 0x11, 0x00, 0xFB, 0x00, 0x00, 0x50, 0xE3
        };
        std::uint32_t call_off = scan;
        for (std::uint32_t i = 0; i + sizeof(call_sig) <= scan; i += 4) {
            if (std::memcmp(base + i, call_sig, sizeof(call_sig)) == 0) {
                call_off = i + 4;
                break;
            }
        }
        if (call_off >= scan) {
            if (g_j9_walk_va) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI guest walker already planted in {}", cs->name());
                plant_j9_invoke_tramp(pr, kern);
            } else {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JXE sl_lookup call not found in {}", cs->name());
            }
            return;
        }

        std::uint32_t orig_blx = 0;
        std::memcpy(&orig_blx, base + call_off, 4);
        const address call_va = run_addr + call_off;
        const std::int32_t imm24 = static_cast<std::int32_t>(orig_blx << 8) >> 8;
        const address decoded = call_va + 8 + (static_cast<std::uint32_t>(imm24) << 2)
            + ((orig_blx >> 24) & 2u);
        if (const auto *at = reinterpret_cast<const std::uint8_t *>(
                pr ? pr->get_ptr_on_addr_space(decoded & ~1u) : nullptr)) {
            if ((at[0] == 0x01) && (at[1] == 0xB4) && (at[2] == 0x02) && (at[3] == 0x48)) {
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI guest walker already planted in {}", cs->name());
                plant_j9_invoke_tramp(pr, kern);
                return;
            }
        }
        const address orig_wrap = decoded;
        g_j9_lookup_fn = orig_wrap;

        kernel::chunk *walk_ch = kern->create<kernel::chunk>(kern->get_memory_system(), pr,
            "J9JniWalk", 0, 0x8000, 0x8000, prot_read_write_exec,
            kernel::chunk_type::normal, kernel::chunk_access::code, kernel::chunk_attrib::none);
        if (!walk_ch) {
            walk_ch = kern->create<kernel::chunk>(kern->get_memory_system(), nullptr,
                "J9JniWalk", 0, 0x8000, 0x8000, prot_read_write_exec,
                kernel::chunk_type::normal, kernel::chunk_access::rom, kernel::chunk_attrib::none);
        }
        if (!walk_ch || !walk_ch->host_base()) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI walker chunk failed in {}", cs->name());
            return;
        }
        const address walk_va = walk_ch->base(pr).ptr_address();
        auto *walk = reinterpret_cast<std::uint8_t *>(walk_ch->host_base());

        // Host-synced {name,fn} list lives at +0x300 so later LCDUI/jcl
        // collects can grow it. Count is the word immediately before the pairs.
        const address pairs_va = walk_va + 0x304;
        const auto walker = hle::j9_emit_jni_walker(pairs_va, orig_wrap);
        if (walker.empty() || (walker.size() * 4u + 8u > 0x300u)) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNI walker emit failed ({} words) in {}",
                walker.size(), cs->name());
            return;
        }
        std::memcpy(walk, walker.data(), walker.size() * 4u);
        std::memset(walk + 0x200, 0, 0x8000 - 0x200);
        g_j9_walk_ch = walk_ch;
        g_j9_walk_va = walk_va;
        g_j9_walk_pairs = pairs_va;
        g_j9_walk_miss_bkpt = 0;
        g_j9_walk_hit_bkpt = 0;
        for (std::size_t i = 0; i + 1 < walker.size(); ++i) {
            if (walker[i] == 0xE1200070u) {
                const address at = walk_va + static_cast<address>(i * 4u);
                if (!g_j9_walk_miss_bkpt) {
                    g_j9_walk_miss_bkpt = at;
                } else {
                    g_j9_walk_hit_bkpt = at;
                }
            }
        }
        (void)orig_wrap;

        // The ROM JNINativeInterface lives in j9.dll at 0x8192C86C. Scan can
        // miss it when that VA is outside the codeseg currently being
        // collected (j9vmall ends before the table).
        if (!g_j9_jnienv_table) {
            g_j9_jnienv_table = 0x8192C86Cu;
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNIEnv function table forced 0x{:X}",
                g_j9_jnienv_table);
        }
        if (pr) {
            collect_j9_jni_rom_table(pr, 0x81B1A300u, 180, "nokialcdui");
            // jclcldc11 sl_lookup: 169 names @ 0x8194F9E0, fns @ 0x8194FC84.
            collect_j9_jni_name_fn_lists(pr, 0x8194F9E0u, 0x8194FC84u, 169, "jcl-sl");
        }
        // Shared Thumb stub for Main._markTime: return 0, do not leak JNIEnv.
        walk[0x27C] = 0x00;
        walk[0x27D] = 0x20; // movs r0, #0
        walk[0x27E] = 0x70;
        walk[0x27F] = 0x47; // bx lr

        if (g_j9_jnienv_table) {
            // ROM GetStringChars walks J9 JNIEnv internals at +0xB08.
            // type=JXE leaves env->functions garbage (0xFFFC53A8) and those
            // fields unset, so copy the ROM table into this chunk and replace
            // the string ops with stubs that do not touch env internals.
            constexpr std::uint32_t k_jni_tbl_off = 0x3400;
            constexpr std::uint32_t k_jni_tbl_words = 247;
            constexpr std::uint32_t k_str_stub_off = 0x280;
            address usable_table = g_j9_jnienv_table;
            if (pr) {
                if (const auto *src = reinterpret_cast<const std::uint32_t *>(
                        pr->get_ptr_on_addr_space(g_j9_jnienv_table))) {
                    std::memcpy(walk + k_jni_tbl_off, src, k_jni_tbl_words * 4u);
                    const address empty_va = walk_va + 0x2A0;
                    walk[0x2A0] = 0;
                    walk[0x2A1] = 0;
                    walk[0x2A2] = 0;
                    walk[0x2A3] = 0;
                    // 0x280 GetStringLength / ExceptionOccurred: return 0
                    walk[0x280] = 0x00;
                    walk[0x281] = 0x20; // movs r0, #0
                    walk[0x282] = 0x70;
                    walk[0x283] = 0x47; // bx lr
                    // 0x284 ReleaseString*: nop
                    walk[0x284] = 0x70;
                    walk[0x285] = 0x47; // bx lr
                    walk[0x286] = 0x00;
                    walk[0x287] = 0xBF;
                    // 0x288 GetStringChars / GetStringUTFChars:
                    // if (isCopy) *isCopy = 0; return empty
                    walk[0x288] = 0x00;
                    walk[0x289] = 0x2A; // cmp r2, #0
                    walk[0x28A] = 0x01;
                    walk[0x28B] = 0xD0; // beq +4
                    walk[0x28C] = 0x00;
                    walk[0x28D] = 0x23; // movs r3, #0
                    walk[0x28E] = 0x13;
                    walk[0x28F] = 0x60; // str r3, [r2]
                    walk[0x290] = 0x01;
                    walk[0x291] = 0x48; // ldr r0, [pc, #4] -> +0x298
                    walk[0x292] = 0x70;
                    walk[0x293] = 0x47; // bx lr
                    walk[0x294] = 0x00;
                    walk[0x295] = 0x00;
                    walk[0x296] = 0x00;
                    walk[0x297] = 0x00;
                    std::memcpy(walk + 0x298, &empty_va, 4);
                    auto *tbl = reinterpret_cast<std::uint32_t *>(walk + k_jni_tbl_off);
                    const address zero_fn = (walk_va + 0x280) | 1u;
                    const address rel_fn = (walk_va + 0x284) | 1u;
                    const address chars_fn = (walk_va + 0x288) | 1u;
                    tbl[15] = zero_fn; // ExceptionOccurred
                    tbl[17] = rel_fn; // ExceptionClear
                    tbl[164] = zero_fn; // GetStringLength
                    tbl[165] = chars_fn; // GetStringChars
                    tbl[166] = rel_fn; // ReleaseStringChars
                    // NewStringUTF: ARM BKPT into host so JCL natives can
                    // build real java.lang.String objects.
                    walk[0x2C8] = 0x70;
                    walk[0x2C9] = 0x00;
                    walk[0x2CA] = 0x20;
                    walk[0x2CB] = 0xE1; // bkpt
                    walk[0x2CC] = 0x1E;
                    walk[0x2CD] = 0xFF;
                    walk[0x2CE] = 0x2F;
                    walk[0x2CF] = 0xE1; // bx lr
                    g_j9_newstr_bkpt = walk_va + 0x2C8u;
                    tbl[167] = g_j9_newstr_bkpt; // NewStringUTF
                    walk[0x2D0] = 0x70;
                    walk[0x2D1] = 0x00;
                    walk[0x2D2] = 0x20;
                    walk[0x2D3] = 0xE1; // bkpt
                    walk[0x2D4] = 0x1E;
                    walk[0x2D5] = 0xFF;
                    walk[0x2D6] = 0x2F;
                    walk[0x2D7] = 0xE1; // bx lr
                    g_j9_proplist_bkpt = walk_va + 0x2D0u;
                    walk[0x2D8] = 0x70;
                    walk[0x2D9] = 0x00;
                    walk[0x2DA] = 0x20;
                    walk[0x2DB] = 0xE1; // bkpt
                    walk[0x2DC] = 0x1E;
                    walk[0x2DD] = 0xFF;
                    walk[0x2DE] = 0x2F;
                    walk[0x2DF] = 0xE1; // bx lr
                    g_j9_getstrutf_bkpt = walk_va + 0x2D8u;
                    walk[0x2E0] = 0x70;
                    walk[0x2E1] = 0x00;
                    walk[0x2E2] = 0x20;
                    walk[0x2E3] = 0xE1; // bkpt
                    walk[0x2E4] = 0x1E;
                    walk[0x2E5] = 0xFF;
                    walk[0x2E6] = 0x2F;
                    walk[0x2E7] = 0xE1; // bx lr
                    g_j9_findclass_bkpt = walk_va + 0x2E0u;
                    walk[0x2E8] = 0x70;
                    walk[0x2E9] = 0x00;
                    walk[0x2EA] = 0x20;
                    walk[0x2EB] = 0xE1; // bkpt
                    walk[0x2EC] = 0x1E;
                    walk[0x2ED] = 0xFF;
                    walk[0x2EE] = 0x2F;
                    walk[0x2EF] = 0xE1; // bx lr
                    g_j9_findclass_ret_bkpt = walk_va + 0x2E8u;
                    walk[0x2F8] = 0x70;
                    walk[0x2F9] = 0x00;
                    walk[0x2FA] = 0x20;
                    walk[0x2FB] = 0xE1; // bkpt
                    walk[0x2FC] = 0x1E;
                    walk[0x2FD] = 0xFF;
                    walk[0x2FE] = 0x2F;
                    walk[0x2FF] = 0xE1; // bx lr
                    g_j9_getmethod_bkpt = walk_va + 0x2F8u;
                    walk[0x300] = 0x70;
                    walk[0x301] = 0x00;
                    walk[0x302] = 0x20;
                    walk[0x303] = 0xE1; // bkpt
                    walk[0x304] = 0x1E;
                    walk[0x305] = 0xFF;
                    walk[0x306] = 0x2F;
                    walk[0x307] = 0xE1; // bx lr
                    g_j9_newobjarr_bkpt = walk_va + 0x300u;
                    walk[0x310] = 0x70;
                    walk[0x311] = 0x00;
                    walk[0x312] = 0x20;
                    walk[0x313] = 0xE1; // bkpt
                    walk[0x314] = 0x1E;
                    walk[0x315] = 0xFF;
                    walk[0x316] = 0x2F;
                    walk[0x317] = 0xE1; // bx lr
                    g_j9_newglobal_bkpt = walk_va + 0x310u;
                    walk[0x318] = 0x70;
                    walk[0x319] = 0x00;
                    walk[0x31A] = 0x20;
                    walk[0x31B] = 0xE1; // bkpt
                    walk[0x31C] = 0x1E;
                    walk[0x31D] = 0xFF;
                    walk[0x31E] = 0x2F;
                    walk[0x31F] = 0xE1; // bx lr
                    g_j9_callstatic_bkpt = walk_va + 0x318u;
                    walk[0x320] = 0x70;
                    walk[0x321] = 0x00;
                    walk[0x322] = 0x20;
                    walk[0x323] = 0xE1; // bkpt
                    walk[0x324] = 0x1E;
                    walk[0x325] = 0xFF;
                    walk[0x326] = 0x2F;
                    walk[0x327] = 0xE1; // bx lr
                    g_j9_cp_stub_bkpt = walk_va + 0x320u;
                    // ARM `b .` pad: kick parks here after host WS bind.
                    // A single word used to sit next to uninitialised
                    // bytes; Thumb fall-through then wrote 0x70643568.
                    for (unsigned po = 0x328u; po < 0x380u; po += 4u) {
                        walk[po] = 0xFE;
                        walk[po + 1u] = 0xFF;
                        walk[po + 2u] = 0xFF;
                        walk[po + 3u] = 0xEA;
                    }
                    // Thumb `b .` (fe e7). Kick enters with |1 so a leftover
                    // T bit cannot fall through the ARM pad into 0x36C.
                    for (unsigned po = 0x3C0u; po < 0x3D0u; po += 2u) {
                        walk[po] = 0xFE;
                        walk[po + 1u] = 0xE7;
                    }
                    g_j9_park_pc = (walk_va + 0x3C0u) | 1u;
                    walk[0x340] = 0x70;
                    walk[0x341] = 0x00;
                    walk[0x342] = 0x20;
                    walk[0x343] = 0xE1; // bkpt
                    walk[0x344] = 0x1E;
                    walk[0x345] = 0xFF;
                    walk[0x346] = 0x2F;
                    walk[0x347] = 0xE1; // bx lr
                    g_j9_lcdui_chain_bkpt = walk_va + 0x340u;
                    tbl[6] = g_j9_findclass_bkpt; // FindClass
                    tbl[21] = g_j9_newglobal_bkpt; // NewGlobalRef
                    tbl[23] = zero_fn; // DeleteLocalRef / ExceptionOccurred
                    tbl[33] = g_j9_getmethod_bkpt; // GetMethodID
                    tbl[113] = g_j9_getmethod_bkpt; // GetStaticMethodID
                    tbl[168] = zero_fn; // GetStringUTFLength
                    tbl[169] = g_j9_getstrutf_bkpt; // GetStringUTFChars
                    tbl[170] = rel_fn; // ReleaseStringUTFChars
                    tbl[141] = g_j9_callstatic_bkpt; // CallStaticVoidMethodA
                    tbl[172] = g_j9_newobjarr_bkpt; // NewObjectArray
                    usable_table = walk_va + k_jni_tbl_off;
                    g_j9_jnienv_table = usable_table;
                    // Stand-alone JNIEnv: functions + J9 reserved/thread slots.
                    auto *fe = reinterpret_cast<std::uint32_t *>(walk + 0x2B0);
                    fe[0] = usable_table;
                    fe[1] = 0;
                    fe[2] = 0;
                    fe[3] = 0;
                    fe[4] = 0;
                    g_j9_fake_env = walk_va + 0x2B0;
                    LOG_WARN(EMULATED_STDOUT,
                        "[j9-nf] JNIEnv guest table 0x{:X} fake-env=0x{:X} stubs @0x{:X}",
                        usable_table, g_j9_fake_env, walk_va + k_str_stub_off);
                }
            }
            // Always install the guest JNINativeInterface. ROM GetStringChars
            // walks J9 env internals that type=JXE never fills; keeping the
            // ROM table made markTime blx a garbage slot (pc=0x157BEB0).
            const address fixer_va = walk_va + k_j9_fixer_off;
            std::uint32_t fixw[] = {
                0xE59F1004, // 0 ldr r1, table
                0xE5801000, // 1 str r1, [r0]
                0xE12FFF1E, // 2 bx lr
                g_j9_jnienv_table
            };
            std::memcpy(walk + k_j9_fixer_off, fixw, sizeof(fixw));
            g_j9_jni_fixer = fixer_va;
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] JNIEnv fixer @0x{:X} table=0x{:X}",
                fixer_va, g_j9_jnienv_table);
        }
        sync_j9_guest_jni_table();

        std::uint32_t gap_off = scan;
        const std::uint32_t lo = (scan > 0x50000) ? 0x40000 : 0x2000;
        const std::uint32_t hi = (scan > 0x1000) ? (scan - 0x800) : scan;
        for (std::uint32_t i = lo; i + 16 <= hi; i += 4) {
            bool z = true;
            for (int b = 0; b < 16; ++b) {
                if (base[i + b] != 0) {
                    z = false;
                    break;
                }
            }
            if (z) {
                gap_off = i;
                break;
            }
        }
        if (gap_off >= scan) {
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] no gap for JNI trampoline in {}", cs->name());
            return;
        }

        std::uint8_t *stub = j9_guest_write(pr, base, run_addr, gap_off);
        std::uint8_t *blx_slot = j9_guest_write(pr, base, run_addr, call_off);
        if (!stub || !blx_slot) {
            return;
        }
        // Thumb: push {r0}; ldr r0, [pc, #8]; bx r0; nop; .word walk_va
        stub[0] = 0x01;
        stub[1] = 0xB4;
        stub[2] = 0x02;
        stub[3] = 0x48;
        stub[4] = 0x00;
        stub[5] = 0x47;
        stub[6] = 0x00;
        stub[7] = 0xBF;
        stub[8] = 0x00;
        stub[9] = 0x00;
        stub[10] = 0x00;
        stub[11] = 0x00;
        std::memcpy(stub + 12, &walk_va, 4);

        const address stub_va = run_addr + gap_off;
        const std::int32_t blx_disp = static_cast<std::int32_t>(stub_va - (call_va + 8));
        const std::uint32_t blx = 0xFA000000u
            | ((static_cast<std::uint32_t>(blx_disp) >> 2) & 0x00FFFFFFu);
        std::memcpy(blx_slot, &blx, 4);

        if (arm::core *cpu = kern->get_cpu()) {
            cpu->imb_range(call_va, 4);
            cpu->imb_range(stub_va, 16);
            cpu->imb_range(walk_va, 0x8000);
        }
        LOG_WARN(EMULATED_STDOUT,
            "[j9-nf] JNI guest walker @0x{:X} tramp@0x{:X} blx@0x{:X} orig=0x{:X} pairs=0x{:X} n={} missbk=0x{:X} in {}",
            walk_va, stub_va, call_va, orig_wrap, pairs_va, g_j9_jni_table_n, g_j9_walk_miss_bkpt, cs->name());

        plant_j9_invoke_tramp(pr, kern);
    }

    void apply_j2me_compat_patches(codeseg_ptr cs, const std::string &name_hint) {
        const std::string name = common::lowercase_string(name_hint.empty() ? cs->name() : name_hint);

        if ((name.find("j9") != std::string::npos) || (name.find("jcl") != std::string::npos)) {
            if (name.find("j9midps60") != std::string::npos) {
                j9_loaded_libs.clear();
                j9_jni_exports.clear();
                g_j9_last_java_name.clear();
                g_j9_mangle_bkpt = 0;
                g_j9_bind_fail_bkpt = 0;
                g_j9_jcl_sl_bkpt = 0;
                g_j9_walk_miss_bkpt = 0;
                g_j9_walk_hit_bkpt = 0;
                g_j9_invoke_n = 0;
                g_j9_bx_sb_n = 0;
                g_j9_jxesl_bkpt = 0;
                g_j9_sl_bkpt = 0;
                g_j9_sl_call_bkpt = 0;
                g_j9_lookup_fn = 0;
                g_j9_midp2ams_jxe = 0;
                g_j9_midp2ams_run = 0;
                g_j9_midp2ams_size = 0;
                g_j9_jni_table_va = 0;
                g_j9_jni_table_n = 0;
                g_j9_walk_ch = nullptr;
                g_j9_walk_va = 0;
                g_j9_walk_pairs = 0;
                g_j9_jnienv_table = 0;
                g_j9_jni_fixer = 0;
                g_j9_fake_env = 0;
                g_j9_newstr_bkpt = 0;
                g_j9_getstrutf_bkpt = 0;
                g_j9_findclass_bkpt = 0;
                g_j9_findclass_ret_bkpt = 0;
                g_j9_findclass_saved_lr = 0;
                g_j9_getmethod_bkpt = 0;
                g_j9_newobjarr_bkpt = 0;
                g_j9_newglobal_bkpt = 0;
                g_j9_callstatic_bkpt = 0;
                g_j9_lcdui_chain_bkpt = 0;
                g_j9_cp_stub_bkpt = 0;
                g_j9_cp_stub_meth = 0;
                g_j9_main_cp = 0;
                g_j9_dummy_fw = 0;
                g_j9_dummy_args = 0;
                g_j9_dummy_rt = 0;
                g_j9_dummy_cms = 0;
                g_j9_main_clazz = 0;
                g_j9_main_method = 0;
                g_j9_utf_stash = 0;
                g_j9_proplist_bkpt = 0;
                g_j9_throw_bkpt = 0;
                g_j9_throw_orig = 0;
                g_j9_last_adapt = 0;
                g_j9_last_thumb = 0;
                g_j9_meth_n = 0;
                g_j9_toolkit_obj = 0;
                g_j9_canvas_obj = 0;
                g_j9_graphics_obj = 0;
                g_j9_peer_n = 0;
                g_j9_vmthread = 0;
                g_j9_vt10_c = 0;
                g_j9_vt14_c = 0;
                g_j9_vt18_c = 0;
                g_j9_bytecode_pc = 0;
                g_j9_java_sp = 0;
                g_j9_tramp_method = 0;
                g_j9_jcl_inited = 0;
                g_j9_saved_r2 = 0;
                g_j9_saved_r4 = 0;
                g_j9_saved_r5 = 0;
                g_j9_saved_r6 = 0;
                g_j9_tramp_va = 0;
                g_j9_page_n = 0;
                g_j9_caller_n = 0;
                g_j9_snap_n = 0;
                g_j9_snap_i = 0;
                g_j9_resume_no_ac = false;
                g_j9_resume_at = 0;
                g_j9_unbound_retry = 0;
                g_j9_good_r4 = 0;
                g_j9_good_r6 = 0;
                g_j9_method_start = 0;
                g_j9_force_caller = 0;
                g_j9_jcl_r4 = 0;
                g_j9_jcl_r5 = 0;
                g_j9_jcl_r6 = 0;
                g_j9_pending_ac_r6 = 0;
                g_j9_last_java_obj = 0;
                g_j9_string_clazz = 0;
                g_j9_char_array_clazz = 0;
                g_j9_string_array_clazz = 0;
                g_j9_encoding_str = 0;
                g_j9_proplist = 0;
                g_j9_system_clazz = 0;
                g_j9_system_r4 = 0;
                g_j9_system_r5 = 0;
                g_j9_system_r6 = 0;
                g_j9_system_r7 = 0;
                g_j9_system_method = 0;
                g_j9_encoding_n = 0;
                g_j9_ht_filled = false;
                g_j9_system_ht = 0;
                g_j9_ht_n = 0;
                g_j9_throw_skips = 0;
                g_j9_last_skip_pc = 0;
                g_j9_system_frame = 0;
                g_j9_init_return = 0;
                g_j9_init_tail = false;
                g_j9_init_caller_r4 = 0;
                g_j9_init_caller_r6 = 0;
                g_j9_init_caller_r7 = 0;
                g_j9_ac_fp = 0;
                g_j9_init_glue = 0;
                g_j9_init_sp = 0;
                g_j9_cframe_ok = false;
                g_j9_init_returned = false;
                g_j9_sys_ok = false;
                g_j9_sys_sp = 0;
                g_j9_wrap_java_fp = 0;
                g_j9_wrap_sp34 = 0;
                g_j9_wrap_sp44 = 0;
                g_j9_wrap_r5 = 0;
                g_j9_wrap_t0 = 0;
                g_j9_wrap_t1 = 0;
                g_j9_wrap_t2 = 0;
                g_j9_wrap_clazz = 0;
                g_j9_last_jcl_clazz = 0;
                g_j9_converter_clazz = 0;
                g_j9_converter_dummy = 0;
                g_j9_util_conv_done = false;
                g_j9_string_astore_done = false;
                g_j9_string_filled = false;
                g_j9_init_c_returned = false;
                g_j9_cc_last_pc = 0;
                g_j9_cc_same_n = 0;
                g_j9_str_ret_r4 = 0;
                g_j9_str_ret_pc = 0;
                g_j9_str_ret_fp = 0;
                g_j9_str_ret_ok = false;
                g_j9_conv_caller_r4 = 0;
                g_j9_conv_caller_r5 = 0;
                g_j9_conv_caller_r6 = 0;
                g_j9_conv_caller_r7 = 0;
                g_j9_live_r4 = 0;
                g_j9_live_r5 = 0;
                g_j9_live_r6 = 0;
                g_j9_live_r7 = 0;
                g_j9_caller_live_r4 = 0;
                g_j9_caller_live_r5 = 0;
                g_j9_caller_live_r6 = 0;
                g_j9_caller_live_r7 = 0;
                g_j9_boot_t0 = 0;
                g_j9_boot_t1 = 0;
                g_j9_boot_t2 = 0;
                g_j9_boot_fp = 0;
                g_j9_boot_csp = 0;
                g_j9_boot_returned = false;
                g_j9_live_csp = 0;
                g_j9_consumed_csp = 0;
                g_j9_live_cframe_ok = false;
                for (int i = 0; i < 16; ++i) {
                    g_j9_live_cframe[i] = 0;
                }
                g_j9_inl_r4 = 0;
                g_j9_inl_r6 = 0;
                g_j9_inl_r7 = 0;
                g_j9_init_depth = 0;
                g_j9_pending_clinit_ret = false;
                g_j9_pending_ret = {};
                g_j9_last_interp_pc = 0;
                g_j9_outer_r4 = 0;
                g_j9_outer_r5 = 0;
                g_j9_outer_r6 = 0;
                g_j9_outer_r7 = 0;
                g_j9_skip_init_clazz = 0;
                for (int i = 0; i < 8; ++i) {
                    g_j9_init_stack[i] = {};
                }
                for (int i = 0; i < 16; ++i) {
                    g_j9_boot_cframe[i] = 0;
                }
                g_j9_wrap_fp_ok = false;
                for (int i = 0; i < 96; ++i) {
                    g_j9_sys_frame[i] = 0;
                }
                for (int i = 0; i < 6; ++i) {
                    g_j9_ac_w[i] = 0;
                }
                for (int i = 0; i < 16; ++i) {
                    g_j9_caller_hdr[i] = 0;
                }
                for (int i = 0; i < 64; ++i) {
                    g_j9_cframe[i] = 0;
                }
                g_j9_jcl_this = 0;
                g_j9_dummy_array = 0;
                g_j9_jcl_outer_r4 = 0;
                g_j9_jcl_outer_r5 = 0;
                g_j9_jcl_outer_r6 = 0;
                g_j9_jcl_returned = false;
                g_j9_midlet_this = 0;
                g_j9_display_obj = 0;
                g_j9_alps_started = false;
                g_j9_alps_phase = 0;
                g_j9_alps_clazz = 0;
                j9_host_midp_reset();
                g_j9_main_ret_sp = 0;
                g_j9_main_ret_lr = 0;
                g_j9_park_pc = 0;
                g_j9_lcdui_chain_bkpt = 0;
                g_j9_last_jxe_key = 0;
                g_j9_last_jxe_r4 = 0;
                g_j9_last_jxe_r5 = 0;
                g_j9_last_jxe_r6 = 0;
                g_j9_last_jxe_r7 = 0;
                g_j9_prev_jxe_r4 = 0;
                g_j9_prev_jxe_r5 = 0;
                g_j9_prev_jxe_r6 = 0;
                g_j9_valid_cp = 0;
                g_j9_vmall_cs = nullptr;
                j9_jcl_vm_dllmain = 0;
                j9_jcl_onload = 0;
                j9_jcl_jvm_onload = 0;
                j9_jcl_jni_onunload = 0;
                g_j9_jcl_jxe_rw = nullptr;
                g_j9_jcl_jxe_va = 0;
                g_j9_java_heap_ch = nullptr;
                g_j9_heap_grow_ch = nullptr;
                g_j9_java_heap_va = 0;
                g_j9_java_heap_off = 0;
                g_j9_official_heap = 0;
                g_j9_thread_class = 0;
                g_j9_thread_obj = 0;
            }
            if (j9_loaded_libs.find(name) == std::string::npos) {
                if (!j9_loaded_libs.empty()) {
                    j9_loaded_libs += ',';
                }
                j9_loaded_libs += name;
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] codeseg attached '{}'", name);
            }
        }

        if (name.find("midp2silentmidletinstall") != std::string::npos) {
            apply_silent_midlet_installer_compat_patch(cs);
        } else if (name == "midp2installerplugin.dll") {
            apply_installer_compat_patch(cs);
        } else if (name == "ifeui.dll") {
            apply_ifeui_compat_patch(cs);
        } else if (name.find("j9midps60") != std::string::npos) {
            apply_j9_allfiles_patch(cs);
        } else if (name.find("j9_23_midp2ams") != std::string::npos) {
            apply_j9_midp2ams_desc_patch(cs);
            apply_j9_midp2ams_app_arg_patch(cs);
            apply_j9_nativefile_open_hook(cs);
            collect_j9_jni_exports(cs);
            if (g_j9_vmall_cs) {
                apply_j9_bindnatv_hook(g_j9_vmall_cs);
            }
        } else if (name.find("j9vmall") != std::string::npos) {
            collect_j9_jni_exports(cs);
            g_j9_vmall_cs = cs;
            apply_j9_vmall_raw_j99j_file_patch(cs);
            apply_j9_bindnatv_hook(cs);
        } else if ((name.find("jclcldc") != std::string::npos) || (name.find("jclcdc") != std::string::npos)) {
            collect_j9_jni_exports(cs);
            apply_j9_jcl_inmem_jxe_patch(cs);
        } else if (name.find("j9mjit") != std::string::npos) {
            apply_j9_mjit_noop_hook(cs);
            collect_j9_jni_exports(cs);
        } else if (((name.find("j9") != std::string::npos) || (name.find("jcl") != std::string::npos)
                       || (name.find("lcdui") != std::string::npos))
            && (name.find("j9midps60") == std::string::npos)) {
            collect_j9_jni_exports(cs);
        } else if (name.find("integrityserver") != std::string::npos) {
            apply_integrity_compat_patch(cs);
        } else if (name.find("midp2systemams") != std::string::npos) {
            apply_ams_find_compat_patch(cs);
        }
    }

    static bool does_condition_meet_for_patch(codeseg_ptr original, patch_info &patch, const bool check_name) {
        if (check_name) {
            const std::string org_name = original->name();

            if (common::compare_ignore_case(org_name.c_str(), patch.name_.c_str()) != 0) {
                return false;
            }
        }

        if (patch.need_dest_rom_ && !original->is_rom()) {
            return false;
        }

        const auto the_uids = original->get_uids();
        const bool uid2_sas = (!patch.req_uid2_ || (patch.req_uid2_ == std::get<1>(the_uids)));
        const bool uid3_sas = (!patch.req_uid3_ || (patch.req_uid3_ == std::get<2>(the_uids)));

        if (uid2_sas && uid3_sas) {
            return true;
        }

        return false;
    }

    void lib_manager::apply_trick_or_treat_algo() {
        for (auto &patch : patches_) {
            // Auto apply
            codeseg_ptr cc = load(common::utf8_to_ucs2(patch.name_));
        }
    }

    bool lib_manager::try_apply_patch(codeseg_ptr original) {
        if (!original) {
            return false;
        }

        for (std::size_t i = 0; i < patches_.size(); i++) {
            if (does_condition_meet_for_patch(original, patches_[i], true)) {
                if (!patches_[i].patch_) {
                    patch_pending_entry entry;
                    entry.info_index_ = i;
                    entry.dest_ = original;

                    patch_pendings_.push_back(entry);
                } else {
                    patch_original_codeseg(trampoline_lookup_, patches_[i].routes_, kern_->get_memory_system(), patches_[i].patch_,
                        original);
                }

                return true;
            }
        }

        return false;
    }

    void lib_manager::apply_pending_patches() {
        for (auto &pending_entry : patch_pendings_) {
            if (pending_entry.info_index_ >= patches_.size()) {
                continue;
            }

            patch_info &info = patches_[pending_entry.info_index_];
            patch_original_codeseg(trampoline_lookup_, info.routes_, kern_->get_memory_system(), info.patch_,
                pending_entry.dest_);
        }
    }

    drive_number lib_manager::get_drive_rom() {
        if (rom_drv_ == drive_invalid) {
            for (drive_number drv = drive_z; drv >= drive_a; drv = static_cast<drive_number>(static_cast<int>(drv) - 1)) {
                if (auto ent = io_->get_drive_entry(drv)) {
                    if (ent->media_type == drive_media::rom) {
                        rom_drv_ = drv;
                        break;
                    }
                }
            }
        }

        return rom_drv_;
    }

    codeseg_ptr lib_manager::load_as_e32img(loader::e32img &img, const std::u16string &path) {
        if (auto seg = kern_->get_by_name<kernel::codeseg>(get_e32_codeseg_name_from_path(path))) {
            apply_j2me_compat_patches(seg, common::ucs2_to_utf8(eka2l1::filename(path)));
            return seg;
        }

        return import_e32img(&img, mem_, kern_, *this, path);
    }

    codeseg_ptr lib_manager::load_as_romimg(loader::romimg &romimg, const std::u16string &path, const bool only_shell) {
        if (auto seg = kern_->pull_codeseg_by_ep(romimg.header.entry_point)) {
            // Executable code segments can be materialized while the ROM image
            // is indexed, before their process is launched. Apply targeted
            // runtime compatibility patches here as well as on first import.
            apply_j2me_compat_patches(seg, common::ucs2_to_utf8(eka2l1::filename(path)));
            return seg;
        }

        kernel::codeseg_create_info info;

        info.full_path = path;
        info.uids[0] = romimg.header.uid1;
        info.uids[1] = romimg.header.uid2;
        info.uids[2] = romimg.header.uid3;
        info.code_base = romimg.header.code_address;
        info.data_base = romimg.header.data_bss_linear_base_address;
        info.code_load_addr = romimg.header.code_address;
        info.data_load_addr = romimg.header.data_address;
        info.code_size = romimg.header.code_size;
        info.data_size = romimg.header.data_size;
        info.text_size = romimg.header.text_size;
        info.entry_point = romimg.header.entry_point;
        info.bss_size = romimg.header.bss_size;
        info.export_table = romimg.exports;
        info.sinfo.caps_u[0] = romimg.header.sec_info.cap1;
        info.sinfo.caps_u[1] = romimg.header.sec_info.cap2;
        info.sinfo.vendor_id = romimg.header.sec_info.vendor_id;
        info.sinfo.secure_id = romimg.header.sec_info.secure_id;
        info.exception_descriptor = romimg.header.exception_des;
        info.constant_data = reinterpret_cast<std::uint8_t *>(mem_->get_real_pointer(romimg.header.data_address));

        const std::string seg_name = (path.empty()) ? "codeseg" :
            common::lowercase_string(common::ucs2_to_utf8(eka2l1::filename(path)));

        auto cs = kern_->create<kernel::codeseg>(seg_name, info);
        apply_j2me_compat_patches(cs, seg_name);

        if (only_shell) {
            return cs;
        }

        struct dll_ref_table {
            std::uint16_t flags;
            std::uint16_t num_entries;
            std::uint32_t rom_img_headers_ref[25];
        };

        // Find dependencies
        std::function<void(loader::rom_image_header *, codeseg_ptr)> dig_dependencies;
        dig_dependencies = [&](loader::rom_image_header *header, codeseg_ptr acs) {
            if (header->dll_ref_table_address != 0) {
                dll_ref_table *ref_table = eka2l1::ptr<dll_ref_table>(header->dll_ref_table_address).get(mem_);

                for (std::uint16_t i = 0; i < ref_table->num_entries; i++) {
                    loader::rom_image_header *ref_header = nullptr;
                    address entry_point = 0;

                    if (kern_->is_eka1()) {
                        // See sf.os.buildtools, release.txt of elf2e32.
                        // A mentioned by Morgan tell an dll ref entry is made up of entry point and
                        // another address to that entry point's DLL ref table. Each is 4 bytes
                        entry_point = ref_table->rom_img_headers_ref[i * 2];
                    } else {
                        ref_header = eka2l1::ptr<loader::rom_image_header>(ref_table->rom_img_headers_ref[i])
                                         .get(mem_);

                        entry_point = ref_header->entry_point;
                    }

                    if (auto ref_seg = kern_->pull_codeseg_by_ep(entry_point)) {
                        // Add ref
                        kernel::codeseg_dependency_info dep_info;
                        dep_info.dep_ = ref_seg;

                        acs->add_dependency(dep_info);
                    } else {
                        address romimg_addr = 0;
                        address ep_org = entry_point;

                        if (kern_->is_eka1()) {
                            // Look for entry point word, from there trace back to the ROM image header
                            // Align down entry point address to word size
                            entry_point = common::align(entry_point, 4, 0);
                            address *the_word = eka2l1::ptr<address>(entry_point).get(mem_);

                            static constexpr std::uint32_t MAX_BACK_TRACE = 100;
                            std::uint32_t traced = 0;

                            while (traced < MAX_BACK_TRACE) {
                                if (*(the_word - traced) == ep_org) {
                                    break;
                                }

                                traced++;
                            }

                            if (*(the_word - traced) != ep_org) {
                                LOG_ERROR(KERNEL, "Unable to find DLL image for entry point address: 0x{:X}", ep_org);
                            } else {
                                entry_point -= traced * sizeof(address);
                                the_word -= traced;

                                traced = 0;

                                // The header has two variables: entry point and code address. They may be same, so looks on the furthest
                                while ((traced < MAX_BACK_TRACE) && (*(the_word - traced - 1) == ep_org)) {
                                    traced++;
                                }

                                romimg_addr = entry_point - traced * sizeof(address) - offsetof(loader::rom_image_header, entry_point);
                            }
                        } else {
                            romimg_addr = ref_table->rom_img_headers_ref[i];
                        }

                        if (romimg_addr != 0) {
                            // TODO: Supply right size. The loader doesn't care about size right now
                            common::ro_buf_stream buf_stream(eka2l1::ptr<std::uint8_t>(romimg_addr).get(mem_), 0xFFFF);

                            // Load new romimage and add dependency
                            loader::romimg rimg = *loader::parse_romimg(reinterpret_cast<common::ro_stream *>(&buf_stream), mem_, kern_->get_epoc_version());
                            std::u16string path_to_dll;

                            for (std::size_t i = 0; i < search_paths.size(); i++) {
                                if (!eka2l1::has_root_name(search_paths[i])) {
                                    path_to_dll = drive_to_char16(get_drive_rom());
                                    path_to_dll += u':';
                                }

                                path_to_dll += search_paths[i];

                                std::optional<std::u16string> dll_name = io_->find_entry_with_address(path_to_dll, romimg_addr);

                                if (dll_name) {
                                    path_to_dll += dll_name.value();
                                    break;
                                }
                            }

                            kernel::codeseg_dependency_info dep_info;
                            dep_info.dep_ = load_as_romimg(rimg, path_to_dll);

                            acs->add_dependency(dep_info);
                        }
                    }
                }
            }
        };

        dig_dependencies(&romimg.header, cs);
        try_apply_patch(cs);

        return cs;
    }

    std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>> lib_manager::try_search_and_parse(const std::u16string &path, std::u16string *full_path) {
        std::u16string lib_path = path;

        // Try opening e32img, if fail, try open as romimg
        auto open_and_get = [&](const std::u16string &path) -> std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>> {
            std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>>
                result{ std::nullopt, std::nullopt };

            if (io_->exist(path)) {
                symfile f = io_->open_file(path, READ_MODE | BIN_MODE | additional_mode_);
                if (!f) {
                    return result;
                }

                eka2l1::ro_file_stream image_data_stream(f.get());

                auto parse_result = loader::parse_e32img(reinterpret_cast<common::ro_stream *>(&image_data_stream));
                if (parse_result != std::nullopt) {
                    f->close();
                    result.first = std::move(parse_result);

                    return result;
                }

                image_data_stream.seek(0, common::seek_where::beg);
                auto parse_result_2 = loader::parse_romimg(reinterpret_cast<common::ro_stream *>(&image_data_stream), mem_, kern_->get_epoc_version());
                if (parse_result_2 != std::nullopt) {
                    f->close();
                    result.second = std::move(parse_result_2);

                    return result;
                }

                f->close();
            }

            return std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>>{};
        };

        if (!eka2l1::has_root_dir(lib_path)) {
            auto org_root_name = eka2l1::root_name(lib_path, true);
            auto fname = eka2l1::filename(lib_path, true);

            // Nope ? We need to cycle through all possibilities
            for (std::size_t i = 0; i < search_paths.size(); i++) {
                bool only_once = eka2l1::has_root_name(search_paths[i], true);

                if (only_once && !org_root_name.empty()) {
                    continue;
                }

                for (drive_number drv = drive_a; drv <= drive_z; drv = static_cast<drive_number>(static_cast<int>(drv) + 1)) {
                    const char16_t drvc = drive_to_char16(drv);

                    if (!org_root_name.empty()) {
                        lib_path = org_root_name;
                    } else if (!only_once) {
                        lib_path = drvc;
                        lib_path += u':';
                    } else {
                        lib_path = u"";
                    }

                    lib_path += search_paths[i];
                    lib_path += fname;

                    auto result = open_and_get(lib_path);
                    if (result.first != std::nullopt || result.second != std::nullopt) {
                        if (full_path)
                            *full_path = lib_path;

                        return result;
                    }

                    if (only_once) {
                        break;
                    }
                }
            }

            return std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>>{};
        }

        if (!eka2l1::has_root_name(lib_path, true)) {
            // A path like \sys\bin\foo.exe names a directory but no drive. Symbian's
            // loader searches every drive for it; opening it verbatim finds nothing.
            for (drive_number drv = drive_a; drv <= drive_z; drv = static_cast<drive_number>(static_cast<int>(drv) + 1)) {
                std::u16string candidate(1, drive_to_char16(drv));
                candidate += u':';
                candidate += lib_path;

                auto result = open_and_get(candidate);
                if (result.first != std::nullopt || result.second != std::nullopt) {
                    if (full_path)
                        *full_path = candidate;

                    return result;
                }
            }

            return std::pair<std::optional<loader::e32img>, std::optional<loader::romimg>>{};
        }

        if (full_path) {
            *full_path = lib_path;
        }

        return open_and_get(lib_path);
    }

    codeseg_ptr lib_manager::load(const std::u16string &name) {
        // j9vmall's default JCL is the literal "jclcdc11_23"; 5320 ships
        // jclcldc11_23.dll. Rewrite before the drive search so ROM/XIP
        // lookup and RFs-backed loads both hit the real image.
        {
            const std::string lower = common::lowercase_string(common::ucs2_to_utf8(name));
            if ((lower.find("jclcdc") != std::string::npos)
                && (lower.find("jclcldc") == std::string::npos)) {
                std::u16string alias = name;
                const std::string alower = common::lowercase_string(common::ucs2_to_utf8(alias));
                const std::size_t pos = alower.find("jclcdc");
                alias.insert(alias.begin() + static_cast<std::ptrdiff_t>(pos) + 4, u'l');
                LOG_WARN(EMULATED_STDOUT, "[j9-nf] codeseg load '{}' -> '{}'",
                    common::ucs2_to_utf8(name), common::ucs2_to_utf8(alias));
                return load(alias);
            }
        }

        bool is_driver_lib = false;

        if (kern_->is_eka1()) {
            const std::u16string filename = eka2l1::filename(name);
            for (std::size_t i = 0; i < LDD_SKIP_LOAD_LIST.size(); i++) {
                if (common::compare_ignore_case(filename, LDD_SKIP_LOAD_LIST[i]) == 0) {
                    is_driver_lib = true;
                    break;
                }
            }
        }
    
        auto load_depend_on_drive = [&](const std::u16string &lib_path, const bool is_driver_lib = false) -> codeseg_ptr {
            symfile f = io_->open_file(lib_path, READ_MODE | BIN_MODE | additional_mode_);
            if (!f) {
                LOG_ERROR(KERNEL, "Can't open {}", common::ucs2_to_utf8(lib_path));
                return nullptr;
            }

            eka2l1::ro_file_stream image_data_stream(f.get());

            if (f->is_in_rom() && !loader::is_e32img(reinterpret_cast<common::ro_stream *>(&image_data_stream))) {
                auto romimg = loader::parse_romimg(reinterpret_cast<common::ro_stream *>(&image_data_stream), mem_, kern_->get_epoc_version(), is_driver_lib);
                if (!romimg) {
                    return nullptr;
                }

                return load_as_romimg(*romimg, lib_path, is_driver_lib);
            } else {
                auto e32img = loader::parse_e32img(reinterpret_cast<common::ro_stream *>(&image_data_stream));
                if (!e32img) {
                    return nullptr;
                }

                return load_as_e32img(*e32img, lib_path);
            }

            return nullptr;
        };

        std::u16string lib_path = name;

        // A Symbian absolute path can be rooted without naming a drive (for
        // example, "\\sys\\bin\\foo.dll"). Resolve that form against each
        // mounted drive instead of treating it as a complete VFS path.
        if (eka2l1::has_root_dir(lib_path) && eka2l1::root_name(lib_path, true).empty()) {
            for (drive_number drv = drive_a; drv <= drive_z; drv = static_cast<drive_number>(static_cast<int>(drv) + 1)) {
                std::u16string candidate(1, drive_to_char16(drv));
                candidate += u':';
                candidate += lib_path;

                if (io_->exist(candidate)) {
                    if (codeseg_ptr result = load_depend_on_drive(candidate, is_driver_lib)) {
                        result->set_full_path(candidate);
                        return result;
                    }
                }
            }

            return nullptr;
        }

        // Create a new codeseg, we should try search these files
        // Absolute yet ?
        if (!eka2l1::has_root_dir(lib_path)) {
            auto org_root_name = eka2l1::root_name(lib_path, true);
            auto fname = eka2l1::filename(lib_path, true);

            // Nope ? We need to cycle through all possibilities
            for (std::size_t i = 0; i < search_paths.size(); i++) {
                bool only_once = eka2l1::has_root_name(search_paths[i]);

                if (only_once && !org_root_name.empty()) {
                    continue;
                }

                for (drive_number drv = drive_a; drv <= drive_z; drv = static_cast<drive_number>(static_cast<int>(drv) + 1)) {
                    const char16_t drvc = drive_to_char16(drv);

                    if (!org_root_name.empty()) {
                        lib_path = org_root_name;
                    } else {
                        if (!only_once) {
                            lib_path = drvc;
                            lib_path += u':';
                        } else {
                            lib_path = u"";
                        }
                    }

                    lib_path += search_paths[i];
                    lib_path += fname;

                    if (io_->exist(lib_path)) {
                        auto result = load_depend_on_drive(lib_path, is_driver_lib);
                        if (result != nullptr) {
                            result->set_full_path(lib_path);
                            return result;
                        }
                    }

                    if (only_once || !org_root_name.empty())
                        break;
                }
            }

            return nullptr;
        }

        if (!io_->exist(lib_path)) {
            return nullptr;
        }

        // Add the codeseg that trying to be loaded path to search path, for dependencies search.
        search_paths.insert(search_paths.begin(), eka2l1::file_directory(lib_path, true));

        if (auto cs = load_depend_on_drive(lib_path, is_driver_lib)) {
            cs->set_full_path(lib_path);
            search_paths.erase(search_paths.begin());
            return cs;
        }

        search_paths.erase(search_paths.begin());
        return nullptr;
    }

    void lib_manager::jump_trampoline_through_svc() {
        arm::core *cpu = kern_->get_cpu();
        const bool thumb = (cpu->get_cpsr() & 0x20) != 0;

        // CPU backends advance PC before invoking the SVC handler. The
        // trampoline map is keyed by the address at which the SVC was
        // installed, so recover that instruction address before lookup.
        const address lookup_addr = (cpu->get_pc() - (thumb ? 2 : 4)) | (thumb ? 1 : 0);

        auto branch_to = [cpu](const address target) {
            std::uint32_t cpsr = cpu->get_cpsr() & ~0x20;
            if (target & 1) {
                cpsr |= 0x20;
            }
            cpu->set_cpsr(cpsr);
            cpu->set_pc(target & ~1);
        };

        auto ite = trampoline_lookup_.find(lookup_addr);
        if (ite != trampoline_lookup_.end()) {
            branch_to(ite->second);
        } else {
            LOG_ERROR(KERNEL, "Unable to find jump for patched address 0x{:X} (impossible)", lookup_addr);
            branch_to(cpu->get_lr());
        }
    }

    bool lib_manager::call_svc(sid svcnum) {
        // Lock the kernel so SVC call can operate in safety
        kern_->lock();
        
        // Trampoline lookup here
        if (svcnum == 0xFF) {
            jump_trampoline_through_svc();
            kern_->unlock();

            return true;
        }

        auto res = svc_funcs_.find(svcnum);

        if (res == svc_funcs_.end()) {
            LOG_ERROR(KERNEL, "Unimplement system call: 0x{:X}!", svcnum);

            if (eka2l1_leave_probe && kern_->crr_thread()) {
                kern_->crr_thread()->dump_panic_context();
            }

            kern_->unlock();
            return false;
        }

        epoc_import_func func = res->second;

        if (kern_->get_config()->log_svc) {
            kernel::thread *caller = kern_->crr_thread();
            LOG_TRACE(KERNEL, "[{}] Calling SVC 0x{:x} {}",
                caller ? caller->name() : "?", svcnum, func.name);
        }

        func.func(kern_, kern_->crr_process(), kern_->get_cpu());

        kern_->unlock();
        return true;
    }

    bool lib_manager::build_eka1_thread_bootstrap_code() {
        static constexpr const char *BOOTSTRAP_CHUNK_NAME = "EKA1ThreadBootstrapCodeChunk";
        bootstrap_chunk_ = kern_->create<kernel::chunk>(kern_->get_memory_system(), nullptr, BOOTSTRAP_CHUNK_NAME,
            0, 0x1000, 0x1000, prot_read_write_exec, kernel::chunk_type::normal, kernel::chunk_access::rom,
            kernel::chunk_attrib::none);

        if (!bootstrap_chunk_) {
            LOG_ERROR(KERNEL, "Failed to create bootstrap chunk for EKA1 thread!");
            return false;
        }

        codeseg_ptr userlib = load(u"euser.dll");

        if (!userlib) {
            LOG_ERROR(KERNEL, "Unable to load euser.dll to build EKA1 bootstrap code!");
            return false;
        }

        static constexpr std::uint32_t CHUNK_HEAP_CREATE_FUNC_ORDINAL = 166;
        static constexpr std::uint32_t THREAD_EXIT_FUNC_ORDINAL = 397;
        static constexpr std::uint32_t HEAP_SWITCH_FUNC_ORDINAL = 1127;

        const address chunk_heap_create_func_addr = userlib->lookup(nullptr, CHUNK_HEAP_CREATE_FUNC_ORDINAL);
        const address heap_switch_func_addr = userlib->lookup(nullptr, HEAP_SWITCH_FUNC_ORDINAL);
        const address thread_exit_func_addr = userlib->lookup(nullptr, THREAD_EXIT_FUNC_ORDINAL);

        common::cpu_info info;
        info.bARMv7 = false;
        info.bASIMD = false;

        common::armgen::armx_emitter emitter(reinterpret_cast<std::uint8_t *>(bootstrap_chunk_->host_base()), info);

        // SP should points to the thread info struct
        entry_points_call_routine_ = emitter.get_code_pointer();

        const std::uint32_t STATIC_CALL_LIST_SVC_FAKE = 0xFE;
        const std::uint32_t TOTAL_EP_TO_ALLOC = 100;

        /* CODE FOR DLL's ENTRY POINTS INVOKE */
        emitter.PUSH(4, common::armgen::R3, common::armgen::R4, common::armgen::R5, common::armgen::R_LR);

        // Allocate entry point addresses on stack, plus also allocate the total entry point count
        emitter.MOVI2R(common::armgen::R3, TOTAL_EP_TO_ALLOC * sizeof(address) + sizeof(std::uint32_t));
        emitter.SUB(common::armgen::R_SP, common::armgen::R_SP, common::armgen::R3);
        emitter.MOVI2R(common::armgen::R3, TOTAL_EP_TO_ALLOC); // Assign allocated entry point count
        emitter.STR(common::armgen::R3, common::armgen::R_SP); // Store it to the count variable.
        emitter.MOV(common::armgen::R5, common::armgen::R0); // Store dll entry point invoke reason in R5

        emitter.MOV(common::armgen::R0, common::armgen::R_SP); // First arugment is pointer to total number of entry point
        emitter.ADD(common::armgen::R1, common::armgen::R_SP, sizeof(std::uint32_t)); // Second argument is pointer to EP array

        // The SVC call convention on EKA1 assigns PC after SVC with LR, which is horrible, we gotta follow it
        emitter.MOV(common::armgen::R_LR, common::armgen::R_PC); // This PC will skip forwards to the POP, no need to add or sub more thing.
        emitter.SVC(STATIC_CALL_LIST_SVC_FAKE); // Call SVC StaticCallList, our own SVC :D

        emitter.MOV(common::armgen::R3, 0); // R3 is iterator
        emitter.LDR(common::armgen::R4, common::armgen::R_SP); // R4 is total of entry point to iterate.
        emitter.SUB(common::armgen::R4, common::armgen::R4, 1);
        emitter.ADD(common::armgen::R_SP, common::armgen::R_SP, sizeof(std::uint32_t)); // Free our count variable

        std::uint8_t *loop_continue_ptr = emitter.get_writeable_code_ptr();
        emitter.CMP(common::armgen::R3, common::armgen::R4);

        common::armgen::fixup_branch entry_point_call_loop_done = emitter.B_CC(common::cc_flags::CC_GE);

        emitter.MOV(common::armgen::R0, common::armgen::R5); // Move in the entry point invoke reason, in case other function trashed this out.
        emitter.LSL(common::armgen::R12, common::armgen::R3, 2); // Calculate the offset of this entry point, 4 bytes
        emitter.ADD(common::armgen::R12, common::armgen::R12, common::armgen::R_SP);
        emitter.LDR(common::armgen::R12, common::armgen::R12); // Jump
        emitter.BL(common::armgen::R12);
        emitter.ADD(common::armgen::R3, common::armgen::R3, 1);
        emitter.B(loop_continue_ptr);

        emitter.set_jump_target(entry_point_call_loop_done);

        emitter.MOVI2R(common::armgen::R3, TOTAL_EP_TO_ALLOC * sizeof(address));
        emitter.ADD(common::armgen::R_SP, common::armgen::R_SP, common::armgen::R3);
        emitter.POP(4, common::armgen::R3, common::armgen::R4, common::armgen::R5, common::armgen::R_PC);

        emitter.flush_lit_pool();

        /* CODE FOR THREAD INITIALIZATION */
        thread_entry_routine_ = emitter.get_code_pointer();

        emitter.MOV(common::armgen::R0, kernel::dll_reason_thread_attach); // Set the first argument the DLL reason
        emitter.MOV(common::armgen::R4, common::armgen::R1); // Info struct in R1 to R4
        emitter.BL(entry_points_call_routine_);

        // Check the allocator in the thread create info
        emitter.LDR(common::armgen::R0, common::armgen::R4, offsetof(kernel::epoc9_std_epoc_thread_create_info, allocator));
        emitter.CMP(common::armgen::R0, 0);
        common::armgen::fixup_branch allocator_unavail_block = emitter.B_CC(common::CC_EQ);

        // Block: allocator available, switch it
        emitter.MOVI2R(common::armgen::R12, heap_switch_func_addr);
        emitter.BL(common::armgen::R12);
        common::armgen::fixup_branch allocator_setup_done = emitter.B();

        emitter.set_jump_target(allocator_unavail_block);
        emitter.MOV(common::armgen::R0, 0);
        emitter.LDR(common::armgen::R1, common::armgen::R4, offsetof(kernel::epoc9_std_epoc_thread_create_info, heap_min));
        emitter.LDR(common::armgen::R2, common::armgen::R4, offsetof(kernel::epoc9_std_epoc_thread_create_info, heap_max));
        emitter.MOVI2R(common::armgen::R3, 0x1000); // Grow by

        emitter.MOVI2R(common::armgen::R12, chunk_heap_create_func_addr);
        emitter.BL(common::armgen::R12);

        // Switch the heap
        emitter.MOVI2R(common::armgen::R12, heap_switch_func_addr);
        emitter.BL(common::armgen::R12);

        emitter.set_jump_target(allocator_setup_done);

        // Jump to our friend
        // Load userdata to first argument.
        emitter.LDR(common::armgen::R0, common::armgen::R4, offsetof(kernel::epoc9_std_epoc_thread_create_info, ptr));
        emitter.LDR(common::armgen::R12, common::armgen::R4, offsetof(kernel::epoc9_std_epoc_thread_create_info, func_ptr));

        // Pop our struct friend from the stack
        emitter.ADD(common::armgen::R_SP, common::armgen::R_SP, sizeof(kernel::epoc9_std_epoc_thread_create_info));
        emitter.BL(common::armgen::R12);

        // Exit the thread, reason is already in R0 after calling our friend.
        emitter.MOVI2R(common::armgen::R12, thread_exit_func_addr);
        emitter.BL(common::armgen::R12);

        emitter.flush_lit_pool();

        return true;
    }

    lib_manager::lib_manager(kernel_system *kerns, io_system *ios, memory_system *mems)
        : kern_(kerns)
        , io_(ios)
        , mem_(mems)
        , bootstrap_chunk_(nullptr)
        , rom_drv_(drive_invalid)
        , additional_mode_(0)
        , entry_points_call_routine_(nullptr)
        , thread_entry_routine_(nullptr) {
        hle::symbols sb;
        std::string lib_name;

#define LIB(x) lib_name = #x;
#define EXPORT(x, y) \
    sb.push_back(x);
#define ENDLIB()                       \
    lib_symbols.emplace(lib_name, sb); \
    sb.clear();

        if (kern_->get_epoc_version() == epocver::epoc6) {
            //  #include <bridge/epoc6_n.def>
        } else {
            // #include <bridge/epoc9_n.def>
        }

#undef LIB
#undef EXPORT
#undef ENLIB

        switch (kern_->get_epoc_version()) {
        case epocver::epoc6:
            epoc::register_epocv6(*this);
            break;

        case epocver::epoc7:
            // For now seems to match exactly
            epoc::register_epocv80(*this);
            break;

        case epocver::epoc80:
            epoc::register_epocv80(*this);
            break;

        case epocver::epoc81a:
            epoc::register_epocv81a(*this);
            break;

        case epocver::epoc94:
            epoc::register_epocv94(*this);
            break;

        case epocver::epoc93fp1:
        case epocver::epoc93fp2:
            epoc::register_epocv93(*this);
            break;

        case epocver::epoc10:
            epoc::register_epocv10(*this);
            break;

        default:
            break;
        }

        if (kern_->is_eka1()) {
            search_paths.push_back(u"\\System\\Libs\\");
            search_paths.push_back(u"\\System\\Programs\\");
            search_paths.push_back(u"\\System\\Fep\\");
        } else {
            search_paths.push_back(u"\\Sys\\Bin\\");

            // Circumvent ROM vs ROFS issue at the moment.
            additional_mode_ = PREFER_PHYSICAL;
        }
    }

    lib_manager::~lib_manager() {
        svc_funcs_.clear();
    }

    system *lib_manager::get_sys() {
        return kern_->get_system();
    }

    address lib_manager::get_entry_point_call_routine_address() const {
        if (!entry_points_call_routine_) {
            return 0;
        }

        return static_cast<address>(entry_points_call_routine_ - reinterpret_cast<std::uint8_t *>(bootstrap_chunk_->host_base())) + bootstrap_chunk_->base(nullptr).ptr_address();
    }

    address lib_manager::get_thread_entry_routine_address() const {
        if (!thread_entry_routine_) {
            return 0;
        }

        return static_cast<address>(thread_entry_routine_ - reinterpret_cast<std::uint8_t *>(bootstrap_chunk_->host_base())) + bootstrap_chunk_->base(nullptr).ptr_address();
    }
}

namespace eka2l1::epoc {
    bool get_image_info(hle::lib_manager *mngr, const std::u16string &name, epoc::lib_info &linfo) {
        auto imgs = mngr->try_search_and_parse(name);
        LOG_TRACE(KERNEL, "Get Info of {}", common::ucs2_to_utf8(name));

        if (!imgs.first && !imgs.second) {
            return false;
        }

        if (!imgs.first && imgs.second) {
            auto &rimg = imgs.second;

            linfo.uid1 = rimg->header.uid1;
            linfo.uid2 = rimg->header.uid2;
            linfo.uid3 = rimg->header.uid3;
            linfo.secure_id = rimg->header.sec_info.secure_id;
            linfo.caps[0] = rimg->header.sec_info.cap1;
            linfo.caps[1] = rimg->header.sec_info.cap2;
            linfo.vendor_id = rimg->header.sec_info.vendor_id;
            linfo.major = rimg->header.major;
            linfo.minor = rimg->header.minor;

            return true;
        }

        auto &eimg = imgs.first;
        memcpy(&linfo.uid1, &eimg->header.uid1, 12);

        linfo.secure_id = eimg->header_extended.info.secure_id;
        linfo.caps[0] = eimg->header_extended.info.cap1;
        linfo.caps[1] = eimg->header_extended.info.cap2;
        linfo.vendor_id = eimg->header_extended.info.vendor_id;
        linfo.major = eimg->header.major;
        linfo.minor = eimg->header.minor;

        return true;
    }

    std::int32_t get_image_info_from_stream(common::ro_stream *stream, epoc::lib_info &linfo) {
        loader::e32img_header header;
        loader::e32img_header_extended header_extended;
        [[maybe_unused]] epocver ver_use = epocver::epoc94;
        [[maybe_unused]] std::uint32_t uncomp_size = 0;

        const std::int32_t error = loader::parse_e32img_header(stream, header, header_extended, uncomp_size, ver_use);
        if (error != epoc::error_none) {
            return error;
        }

        memcpy(&linfo.uid1, &header.uid1, 12);

        linfo.secure_id = header_extended.info.secure_id;
        linfo.caps[0] = header_extended.info.cap1;
        linfo.caps[1] = header_extended.info.cap2;
        linfo.vendor_id = header_extended.info.vendor_id;
        linfo.major = header.major;
        linfo.minor = header.minor;

        return epoc::error_none;
    }
}
