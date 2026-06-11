/*
 * Copyright (c) 2019 EKA2L1 Team
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

#pragma once

#include <services/fbs/adapter/font_adapter.h>
#include <services/fbs/font.h>

#include <utils/consts.h>

#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eka2l1 {
    class io_system;
}

namespace eka2l1::epoc {
    struct open_font_info {
        std::u16string family;
        std::size_t idx;

        epoc::adapter::font_file_adapter_base *adapter;
        epoc::open_font_face_attrib face_attrib;
        epoc::open_font_metrics metrics;

        std::uint32_t metric_identifier;

        // True for host-side fallback fonts (wide-coverage, e.g. the bundled CJK
        // font). seek_the_open_font_with_character prefers these over ROM fonts
        // so borrowed glyphs come from one consistent, fully-covering font instead
        // of whichever partial ROM font happens to sit first in the store.
        bool prefer_for_glyph_fallback = false;
    };

    // A set of fonts
    class font_store {
        std::vector<open_font_info> open_font_store;
        std::vector<epoc::adapter::font_file_adapter_instance> font_adapters;
        std::vector<epoc::typeface_support> typefaces;

        // codepoint -> index into open_font_store (or -1 when nothing covers it)
        std::unordered_map<std::uint32_t, std::int32_t> glyph_fallback_cache;

        // (adapter, face) -> "claims CJK in its cmap but renders the same
        // placeholder box for every han character" (see is_cjk_placeholder_font)
        std::map<std::pair<epoc::adapter::font_file_adapter_base *, std::size_t>, bool> cjk_placeholder_cache;

        eka2l1::io_system *io;

    protected:
        void folder_change_callback(const std::u16string &path, int action);

    public:
        explicit font_store(eka2l1::io_system *io)
            : io(io) {
        }

        void add_fonts(std::vector<std::uint8_t> &buf, const epoc::adapter::font_file_adapter_kind adapter_kind,
            const bool prefer_for_glyph_fallback = false);

        open_font_info *seek_the_open_font(epoc::font_spec_base &spec);
        open_font_info *seek_the_font_by_uid(const epoc::uid the_uid, epoc::open_font_metrics &target_metric, std::uint32_t *metric_identifier = nullptr);
        open_font_info *seek_the_open_font_with_character(const std::uint32_t codepoint, epoc::adapter::font_file_adapter_base *exclude_adapter);
        epoc::typeface_support *get_typeface_support(const std::uint32_t index);

        /**
         * \brief Detect fonts whose cmap claims CJK coverage but whose outlines are
         *        a single placeholder box (e.g. ROM UI fonts on western firmware).
         *
         * Such fonts defeat the does_glyph_exist gate of the per-character glyph
         * fallback: the glyph "exists", then rasterizes as an identical filled box
         * for every han codepoint. Render two unrelated probe characters and call
         * the font a placeholder when the bitmaps come out byte-identical.
         * Results are cached per (adapter, face).
         */
        bool is_cjk_placeholder_font(epoc::adapter::font_file_adapter_base *adapter, const std::size_t face_idx);

        /**
         * \brief True when this font can really draw the given character: the cmap
         *        has it and, for han/fullwidth text, the font is not a placeholder.
         */
        bool can_really_draw(epoc::adapter::font_file_adapter_base *adapter, const std::size_t face_idx,
            const std::uint32_t codepoint, const std::uint32_t metric_identifier);

        const std::size_t number_of_fonts() const {
            return open_font_store.size();
        }

        open_font_info *get_open_font_info(const std::size_t index) {
            return (index < open_font_store.size()) ? &open_font_store[index] : nullptr;
        }

        const std::size_t number_of_typefaces() const {
            return typefaces.size();
        }
    };
}