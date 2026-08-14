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

#include <kernel/codeseg.h>
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/thread.h>

#include <cpu/arm_interface.h>

#include <cctype>
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

    static void j9_restore_thumb_and_advance(arm::core *core, kernel::process *pr,
        const address pc, const std::uint16_t original, const std::uint32_t r5) {
        if (pr) {
            if (auto *half = reinterpret_cast<std::uint16_t *>(pr->get_ptr_on_addr_space(pc))) {
                *half = original;
            }
        }
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
            j9_restore_thumb_and_advance(core, pr, pc, 0x0005, core->get_reg(0));
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

        j9_restore_thumb_and_advance(core, pr, pc, 0x2500, 0);
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
            if (!(((base[i + k_path_off] == 0x00) && (base[i + k_path_off + 1] == 0x25))
                    || ((base[i + k_path_off] == 0x00) && (base[i + k_path_off + 1] == 0xBE)))) {
                break;
            }
            if (!(((base[i + k_result_off] == 0x00) && (base[i + k_result_off + 1] == 0x05))
                    || ((base[i + k_result_off] == 0x00) && (base[i + k_result_off + 1] == 0xBE)))) {
                break;
            }

            base[i + k_path_off] = 0x00;
            base[i + k_path_off + 1] = 0xBE;
            base[i + k_result_off] = 0x00;
            base[i + k_result_off + 1] = 0xBE;
            g_j9_nf_path_bkpt = run_addr + i + k_path_off;
            g_j9_nf_result_bkpt = run_addr + i + k_result_off;

            kernel_system *kern = cs->get_kernel_object_owner();
            static bool hooked = false;
            if (kern && !hooked) {
                kern->register_breakpoint_hit_callback(j9_nativefile_open_bkpt);
                hooked = true;
            }
            LOG_WARN(EMULATED_STDOUT, "[j9-nf] hooked {} path@0x{:X} result@0x{:X}",
                cs->name(), g_j9_nf_path_bkpt, g_j9_nf_result_bkpt);
            break;
        }
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
    }

    void apply_j2me_compat_patches(codeseg_ptr cs, const std::string &name_hint) {
        const std::string name = common::lowercase_string(name_hint.empty() ? cs->name() : name_hint);

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
