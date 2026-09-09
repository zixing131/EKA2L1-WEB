/*
 * Copyright (c) 2026 EKA2L1 Team.
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

// A skin may define the same item twice, and the later definition is meant to
// replace the earlier one. These build a skin descriptor holding one class
// chunk with two definitions of the same item, which is the shape a Symbian^3
// skin has for QsnFrPopup.

#include <services/ui/skin/skn.h>

#include <catch2/catch.hpp>

#include <common/buffer.h>
#include <config/config.h>
#include <cpu/arm_factory.h>
#include <kernel/chunk.h>
#include <kernel/kernel.h>
#include <kernel/timing.h>
#include <mem/mem.h>
#include <services/ui/skin/chunk_maintainer.h>

#include <cstdint>
#include <vector>

using namespace eka2l1;

namespace {
    // Chunk header fields shared by every descriptor chunk.
    constexpr std::size_t OFF_LEN = 0;
    constexpr std::size_t OFF_TYPE = 4;

    constexpr std::size_t MASTER_CHUNK_COUNT = 38;
    constexpr std::size_t MASTER_CONTENT = 42;

    constexpr std::size_t CLASS_CHUNK_N = 9;
    constexpr std::size_t CLASS_CONTENT = 13;

    constexpr std::size_t ITEM_HASH = 8;
    constexpr std::size_t TABLE_COUNT = 16;
    constexpr std::size_t TABLE_ENTRY0 = 17;
    constexpr std::size_t TABLE_ENTRY_SIZE = 8;

    // process_attrib reads up to offset 18 of the attribute block.
    constexpr std::size_t ATTRIB_SIZE = 20;

    void put(std::vector<std::uint8_t> &buf, const std::size_t at, const std::uint64_t value, const std::size_t width) {
        if (buf.size() < at + width) {
            buf.resize(at + width, 0);
        }

        for (std::size_t i = 0; i < width; i++) {
            buf[at + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF);
        }
    }

    // One image table definition: a header, the item's hash, its entries, and
    // the attribute block the parser expects to find after them.
    std::vector<std::uint8_t> make_image_table(const std::uint64_t id_hash, const std::vector<std::uint64_t> &images) {
        std::vector<std::uint8_t> chunk;

        put(chunk, OFF_TYPE, 6 /* as_desc_skin_desc_img_tbl_item_def */, 2);
        put(chunk, ITEM_HASH, id_hash, 8);
        put(chunk, TABLE_COUNT, images.size(), 2);

        for (std::size_t i = 0; i < images.size(); i++) {
            put(chunk, TABLE_ENTRY0 + i * TABLE_ENTRY_SIZE, images[i], TABLE_ENTRY_SIZE);
        }

        chunk.resize(TABLE_ENTRY0 + images.size() * TABLE_ENTRY_SIZE + ATTRIB_SIZE, 0);
        put(chunk, OFF_LEN, chunk.size(), 4);

        return chunk;
    }

    // A skin descriptor whose single class chunk holds the given definitions.
    std::vector<std::uint8_t> make_skin(const std::vector<std::vector<std::uint8_t>> &defs) {
        std::vector<std::uint8_t> class_chunk;

        put(class_chunk, OFF_TYPE, 3 /* as_desc_skin_desc_class */, 2);
        put(class_chunk, CLASS_CHUNK_N, defs.size(), 4);
        class_chunk.resize(CLASS_CONTENT, 0);

        for (const std::vector<std::uint8_t> &def : defs) {
            class_chunk.insert(class_chunk.end(), def.begin(), def.end());
        }

        put(class_chunk, OFF_LEN, class_chunk.size(), 4);

        std::vector<std::uint8_t> skin;

        put(skin, OFF_TYPE, 0 /* as_desc_skin_desc */, 2);
        put(skin, MASTER_CHUNK_COUNT, 1, 4);
        skin.resize(MASTER_CONTENT, 0);

        skin.insert(skin.end(), class_chunk.begin(), class_chunk.end());
        put(skin, OFF_LEN, skin.size(), 4);

        return skin;
    }

    std::vector<std::uint64_t> nine(const std::uint64_t base) {
        std::vector<std::uint64_t> images;

        for (std::uint64_t i = 0; i < 9; i++) {
            images.push_back(base + i);
        }

        return images;
    }
}

TEST_CASE("skn_image_table_redefinition_replaces_the_entries", "skn_file") {
    // Avkon's frame drawing wants exactly nine elements and silently drops the
    // whole frame otherwise, so a redefinition that appends instead of replacing
    // leaves eighteen and the frame never appears.
    std::vector<std::uint8_t> data = make_skin({
        make_image_table(0x1234, nine(0x100)),
        make_image_table(0x1234, nine(0x200)),
    });

    common::ro_buf_stream stream(data.data(), data.size());
    epoc::skn_file skn(reinterpret_cast<common::ro_stream *>(&stream));

    REQUIRE(skn.img_tabs_.size() == 1);

    const epoc::skn_image_table &table = skn.img_tabs_.begin()->second;

    REQUIRE(table.images.size() == 9);
    REQUIRE(table.images.front() == 0x200);
}

TEST_CASE("skn_image_table_keeps_distinct_items_apart", "skn_file") {
    // Two different items must both survive -- the override rule is per hash.
    std::vector<std::uint8_t> data = make_skin({
        make_image_table(0x1234, nine(0x100)),
        make_image_table(0x5678, nine(0x200)),
    });

    common::ro_buf_stream stream(data.data(), data.size());
    epoc::skn_file skn(reinterpret_cast<common::ro_stream *>(&stream));

    REQUIRE(skn.img_tabs_.size() == 2);
    REQUIRE(skn.img_tabs_[0x1234].images.size() == 9);
    REQUIRE(skn.img_tabs_[0x5678].images.size() == 9);
    REQUIRE(skn.img_tabs_[0x1234].images.front() == 0x100);
    REQUIRE(skn.img_tabs_[0x5678].images.front() == 0x200);
}

TEST_CASE("skin theme overrides preserve base items and filename references", "[skin-merge]") {
    const auto flags = GENERATE(0u, static_cast<unsigned>(epoc::akn_skin_chunk_maintainer_lookup_use_linked_list));
    config::state conf;
    ntimer timing(1000000);
    auto monitor = arm::create_exclusive_monitor(arm_emulator_type::dyncom, 1);
    auto cpu = arm::create_core(monitor.get(), arm_emulator_type::dyncom);
    memory_system memory(monitor.get(), &conf, mem::mem_model_type::multiple, false);
    kernel_system kern(nullptr, &timing, nullptr, &conf, nullptr, nullptr, cpu.get(), nullptr);
    kernel::chunk shared(&kern, &memory, nullptr, "skin-test", 0, 384 * 1024, 384 * 1024,
        prot_read_write, kernel::chunk_type::normal, kernel::chunk_access::global, kernel::chunk_attrib::none);
    epoc::akn_skin_chunk_maintainer maintainer(&shared, 4096, flags);
    auto data = make_skin({});
    common::ro_buf_stream stream(data.data(), data.size());
    epoc::skn_file base(&stream);
    base.filenames_[0] = u"base.mbm";
    // These IDs collide in the linked-list table. Overriding the middle
    // entry must preserve the rest of the chain as well as the v1 stride.
    for (unsigned id : {1u, 129u, 257u}) {
        epoc::skn_bitmap_info bitmap{};
        bitmap.id_hash = id;
        bitmap.filename_id = 0;
        bitmap.bmp_idx = id;
        bitmap.mask_bitmap_idx = 0xFFFFFFFF;
        base.bitmaps_[id] = bitmap;
    }
    REQUIRE(maintainer.import(base, u"z:\\base\\"));
    epoc::skn_file theme(&stream);
    theme.filenames_[0] = u"theme.mbm";
    theme.bitmaps_[129] = base.bitmaps_[129];
    theme.bitmaps_[129].bmp_idx = 999;
    REQUIRE(maintainer.import(theme, u"z:\\theme\\"));
    for (unsigned id : {1u, 129u, 257u}) {
        auto *def = maintainer.get_item_definition({id, 0});
        REQUIRE(def);
        CHECK(def->id_ == epoc::pid(id, 0));
        auto *payload = def->data_.get_relative<std::uint32_t>(
            maintainer.get_area_base(epoc::akn_skin_chunk_area_base_offset::data_area_base));
        REQUIRE(payload);
        CHECK(payload[2] == (id == 129 ? 999 : id));
        auto *filenames = static_cast<std::uint8_t *>(
            maintainer.get_area_base(epoc::akn_skin_chunk_area_base_offset::filename_area_base));
        const std::u16string filename(reinterpret_cast<char16_t *>(filenames + payload[1]));
        CHECK(filename == (id == 129 ? u"z:\\theme\\theme.mbm" : u"z:\\base\\base.mbm"));
    }
}
