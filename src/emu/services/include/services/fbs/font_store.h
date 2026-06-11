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

#include <unordered_map>
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

        // Per-glyph fallback source. When this font's own adapter has no glyph
        // for a codepoint, both the server text atlas and the glyph rasterizer
        // borrow it from this wide-coverage font instead of drawing notdef. Set
        // at startup for glyph-poor fonts; null when no fallback is needed.
        // Kept separate from adapter/idx so the font's own identity (name, UID,
        // bitmap data, native metrics) stays intact for by-UID / native lookups.
        epoc::adapter::font_file_adapter_base *fallback_adapter = nullptr;
        std::size_t fallback_idx = 0;
    };

    // A set of fonts
    class font_store {
        std::vector<open_font_info> open_font_store;
        std::vector<epoc::adapter::font_file_adapter_instance> font_adapters;
        std::vector<epoc::typeface_support> typefaces;

        // codepoint -> index into open_font_store (or -1 when nothing covers it)
        std::unordered_map<std::uint32_t, std::int32_t> glyph_fallback_cache;

        eka2l1::io_system *io;

    protected:
        void folder_change_callback(const std::u16string &path, int action);

    public:
        explicit font_store(eka2l1::io_system *io)
            : io(io) {
        }

        void add_fonts(std::vector<std::uint8_t> &buf, const epoc::adapter::font_file_adapter_kind adapter_kind);

        open_font_info *seek_the_open_font(epoc::font_spec_base &spec);
        open_font_info *seek_the_font_by_uid(const epoc::uid the_uid, epoc::open_font_metrics &target_metric, std::uint32_t *metric_identifier = nullptr);
        open_font_info *seek_the_open_font_with_character(const std::uint32_t codepoint, epoc::adapter::font_file_adapter_base *exclude_adapter);
        epoc::typeface_support *get_typeface_support(const std::uint32_t index);

        const std::size_t number_of_fonts() const {
            return open_font_store.size();
        }

        open_font_info *get_open_font_info(const std::size_t index) {
            return (index < open_font_store.size()) ? &open_font_store[index] : nullptr;
        }

        /**
         * @brief Give glyph-poor fonts a wide-coverage fallback, non-destructively.
         *
         * Every non-symbol entry that lacks any of the probe codepoints records
         * the source font as its per-glyph fallback (open_font_info::fallback_*),
         * without touching its own adapter/face/UID/metrics. The text atlas and
         * the glyph rasterizer then borrow individual glyphs the font cannot draw
         * (typically CJK on western firmware) while every other path - by-name,
         * by-UID, spec scoring, native bitmap rendering - keeps using the real
         * font. This replaces the earlier in-place rebind, which destroyed the
         * original font identity and broke by-UID system-font lookups.
         */
        void assign_fallback_for_glyphless_fonts(const std::size_t source_index, const std::uint32_t *probe_codepoints,
            const std::size_t probe_count);

        const std::size_t number_of_typefaces() const {
            return typefaces.size();
        }
    };
}