/*
* Copyright (c) 2019 EKA2L1 Team.
*
* This file is part of EKA2L1 project
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
#include <common/cvt.h>
#include <common/log.h>

#include <services/fbs/adapter/freetype_font_adapter.h>
#include <climits>
#include <memory>

#include <freetype/tttables.h>

namespace eka2l1::epoc::adapter {
    struct freetype_lib_raii {
        FT_Library lib_{};

        explicit freetype_lib_raii() {
            auto err = FT_Init_FreeType(&lib_);
            if (err) {
                LOG_ERROR(SERVICE_FBS, "Failed to initialize FreeType library, error: {}", FT_Error_String(err));
            }
        }

        ~freetype_lib_raii() {
            FT_Done_FreeType(lib_);
        }
    };

    std::unique_ptr<freetype_lib_raii> ft_lib_raii_;

    // Thank you very much Anson!
    // Source: https://lists.gnu.org/archive/html/freetype/2005-06/msg00033.html
    // Don't know if Symbian dev made this code, or they copy it
    // Symbian Source: https://github.com/SymbianSource/oss.FCL.sf.os.textandloc/blob/59666d6704fee305b0fdd74974f7b4f42659c6a6/fontservices/freetypefontrasteriser/src/FTRAST2.CPP#L934
    static int derive_design_height_from_max_height(const FT_Face& aFace, int aMaxHeightInPixel)
    {
        const int boundingBoxHeightInFontUnit = aFace->bbox.yMax - aFace->bbox.yMin;
        int designHeightInPixels = ( ( aMaxHeightInPixel *
                                        aFace->units_per_EM ) / boundingBoxHeightInFontUnit );

        const int maxHeightInFontUnit = aMaxHeightInPixel << 6;
        FT_Set_Pixel_Sizes( aFace, designHeightInPixels, designHeightInPixels );
        int currentMaxHeightInFontUnit = FT_MulFix(
            boundingBoxHeightInFontUnit, aFace->size->metrics.y_scale );
        while ( currentMaxHeightInFontUnit < maxHeightInFontUnit )
        {
            designHeightInPixels++;
            FT_Set_Pixel_Sizes( aFace, designHeightInPixels, designHeightInPixels );
            currentMaxHeightInFontUnit = FT_MulFix(
                boundingBoxHeightInFontUnit, aFace->size->metrics.y_scale );
        }
        while ( currentMaxHeightInFontUnit > maxHeightInFontUnit )
        {
            designHeightInPixels--;
            FT_Set_Pixel_Sizes( aFace, designHeightInPixels, designHeightInPixels );
            currentMaxHeightInFontUnit = FT_MulFix(
                boundingBoxHeightInFontUnit, aFace->size->metrics.y_scale );
        }
        return designHeightInPixels;
    }

    inline float ft_convention_to_float(FT_Pos val) {
        if (0 > val) {
            val = static_cast<FT_Pos>(-val);
        }

        return static_cast<float>(val) / 64.0f;
    }

    inline short ft_convention_to_int_pixel(FT_Pos val) {
        return static_cast<short>((val + 32) >> 6);
    }

    FT_Library get_ft_lib() {
        return ft_lib_raii_->lib_;
    }

    // At or below this pixel size, scalable fonts (e.g. the bundled CJK fallback
    // Droid Sans Fallback) are rendered with FreeType's monochrome hinter rather
    // than antialiased / LCD output. Two reasons, both seen as fragmented "tofu"
    // on small softkey / dialog-title text:
    //   1) The guest blitter silently drops 8bpp AA glyphs when the destination
    //      is a low display-depth offscreen bitmap (custom-drawn Avkon chrome).
    //   2) Dense CJK outlines below ~16ppem antialias into an illegible smear;
    //      the b/w hinter snaps stems to the pixel grid. This is exactly what
    //      S60's embedded bitmap strikes (12-20ppem) used to provide before the
    //      fallback font was swapped to a pure-outline one.
    // The resulting 1bpp glyph flows through the existing MONO->RLE path, which
    // the guest accepts at any display depth.
    static constexpr std::uint32_t SMALL_GLYPH_MONO_PPEM_THRESHOLD = 15;

    freetype_font_adapter::freetype_font_adapter(std::vector<std::uint8_t> &data)
        : data_(data)
        , is_valid_(false) {
        if (!ft_lib_raii_) {
            ft_lib_raii_ = std::make_unique<freetype_lib_raii>();
        }

        FT_Face query_face;
        auto err = FT_New_Memory_Face(get_ft_lib(), data_.data(), static_cast<FT_Long>(data_.size()), -1, &query_face);

        if (err) {
            LOG_ERROR(SERVICE_FBS, "FreeType failed to query face count from memory, error: {}", FT_Error_String(err));
            return;
        }

        for (auto i = 0; i < query_face->num_faces; i++) {
            FT_Face face;
            err = FT_New_Memory_Face(get_ft_lib(), data_.data(), static_cast<FT_Long>(data_.size()), i, &face);
            if (err) {
                LOG_ERROR(SERVICE_FBS, "FreeType failed to read face from memory, error: {}", FT_Error_String(err));
                continue;
            }
            faces_.push_back(face);
        }

        if (!faces_.empty()) {
            is_valid_ = true;

            current_font_sizes_.resize(faces_.size());
            std::fill(current_font_sizes_.begin(), current_font_sizes_.end(), 0);
        }
    }

    bool freetype_font_adapter::set_font_size(const std::size_t index, const std::uint32_t size) {
        if (index >= faces_.size()) {
            return false;
        }

        if (current_font_sizes_[index] != 0 && current_font_sizes_[index] == size) {
            return true;
        }

        auto face = faces_[index];
        auto err = FT_Set_Pixel_Sizes(face, 0, size);

        if (err) {
            LOG_ERROR(SERVICE_FBS, "Failed to set character size for face, error: {}", FT_Error_String(err));
            return false;
        }

        current_font_sizes_[index] = size;
        return true;
    }

    freetype_font_adapter::~freetype_font_adapter() {
        for (auto face : faces_) {
            FT_Done_Face(face);
        }
    }

    std::uint32_t freetype_font_adapter::line_gap(const std::size_t idx, const std::uint32_t metric_identifier) {
        if (idx >= faces_.size()) {
            return 0;
        }

        if (!set_font_size(idx, metric_identifier)) {
            return 0;
        }

        return faces_[idx]->size->metrics.height - faces_[idx]->size->metrics.ascender + faces_[idx]->size->metrics.descender;
    }

    bool freetype_font_adapter::get_face_attrib(const std::size_t idx, open_font_face_attrib &face_attrib) {
        if (idx >= faces_.size()) {
            return false;
        }

        auto face = faces_[idx];

        auto fam_name = common::utf8_to_ucs2(face->family_name);
        auto name = fam_name + u" " + common::utf8_to_ucs2(face->style_name);

        face_attrib.fam_name.assign(nullptr, fam_name);
        face_attrib.name.assign(nullptr, name);
        face_attrib.local_full_fam_name.assign(nullptr, fam_name);
        face_attrib.local_full_name.assign(nullptr, name);
        face_attrib.style = 0;

        if (face->style_flags & FT_STYLE_FLAG_BOLD) {
            face_attrib.style |= open_font_face_attrib::bold;
        }

        if (face->style_flags & FT_STYLE_FLAG_ITALIC) {
            face_attrib.style |= open_font_face_attrib::italic;
        }

        if (face->face_flags & FT_FACE_FLAG_FIXED_WIDTH) {
            face_attrib.style |= open_font_face_attrib::mono_width;
        }

        auto header = reinterpret_cast<TT_Header*>(FT_Get_Sfnt_Table(face, FT_SFNT_HEAD));

        if (header) {
            face_attrib.min_size_in_pixels = header->Lowest_Rec_PPEM;
        }

        auto os2 = reinterpret_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));

        if (os2) {
            face_attrib.coverage[0] = os2->ulUnicodeRange1;
            face_attrib.coverage[1] = os2->ulUnicodeRange2;
            face_attrib.coverage[2] = os2->ulUnicodeRange3;
            face_attrib.coverage[3] = os2->ulUnicodeRange4;

            if (((os2->panose[1] >= 2) && (os2->panose[1] <= 10)) || (os2->panose[1] >= 14)) {
                face_attrib.style |= open_font_face_attrib::serif;
            }
        }

        return true;
    }

    std::uint8_t *freetype_font_adapter::get_glyph_bitmap(const std::size_t idx, std::uint32_t code, const std::uint32_t metric_identifier,
        int *rasterized_width, int *rasterized_height, std::uint32_t &total_size, epoc::glyph_bitmap_type *bmp_type,
        open_font_character_metric &character_metric) {
        if (idx >= faces_.size()) {
            return nullptr;
        }

        auto face = faces_[idx];
        auto glyph_index = code;

        if (glyph_index & 0x80000000) {
            glyph_index &= ~0x80000000;
        } else {
            glyph_index = FT_Get_Char_Index(face, code);
        }

        if (!set_font_size(idx, metric_identifier)) {
            return nullptr;
        }

        // Small scalable glyphs: render through the monochrome hinter so they
        // survive low display-depth blits and stay legible (see threshold note).
        // Fonts with embedded strikes already return MONO and are unaffected.
        FT_Int32 load_flags = FT_LOAD_RENDER;
        if ((face->size->metrics.y_ppem != 0) && (static_cast<std::uint32_t>(face->size->metrics.y_ppem) <= SMALL_GLYPH_MONO_PPEM_THRESHOLD)) {
            load_flags |= FT_LOAD_TARGET_MONO;
        }

        auto err = FT_Load_Glyph(face, glyph_index, load_flags);
        if (err) {
            LOG_ERROR(SERVICE_FBS, "Failed to load glyph for face to get glyph bitmap, error: {}", FT_Error_String(err));
            return nullptr;
        }

        auto glyph = face->glyph;
        auto bitmap = glyph->bitmap;

        if (rasterized_width) {
            *rasterized_width = static_cast<int>(bitmap.width);
        }

        if (rasterized_height) {
            *rasterized_height = static_cast<int>(bitmap.rows);
        }

        total_size = bitmap.width * bitmap.rows;

        if (bmp_type) {
            *bmp_type = glyph_bitmap_type::antialised_glyph_bitmap;
        }

        // Fonts with embedded bitmap strikes (EBDT/EBLC, e.g. the S60 CJK fallback
        // S60SC.ttf) make FreeType return a 1bpp packed bitmap when a strike matches
        // the requested ppem. Re-encode it as Symbian's run-length monochrome glyph
        // stream (same as the GDR adapter): the guest blitter accepts that at any
        // display depth, while 8bpp antialiased glyphs are dropped on low-depth
        // offscreen bitmaps (custom-drawn UIs) and raw mono would be misread.
        if (bitmap.buffer && (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)) {
            const int mono_width = static_cast<int>(bitmap.width);
            const int mono_height = static_cast<int>(bitmap.rows);

            // Worst case: every line is its own non-repeat section (5 bits header
            // per line) + payload. Round up generously.
            const std::size_t worst_bits = static_cast<std::size_t>(mono_height) * (5 + mono_width) + 32;
            mono_expand_scratch_.assign((worst_bits + 7) / 8 + 4, 0);

            std::uint32_t total_bit_write = 0;
            auto write_bit = [&](const int bit) {
                mono_expand_scratch_[total_bit_write >> 3] |= static_cast<std::uint8_t>((bit & 1) << (total_bit_write & 7));
                total_bit_write++;
            };

            auto src_bit = [&](const int row, const int col) -> int {
                const std::uint8_t byte = bitmap.buffer[static_cast<std::ptrdiff_t>(row) * bitmap.pitch + (col >> 3)];
                return (byte >> (7 - (col & 7))) & 1;
            };

            auto lines_equal = [&](const int row1, const int row2) {
                for (int col = 0; col < mono_width; col++) {
                    if (src_bit(row1, col) != src_bit(row2, col)) {
                        return false;
                    }
                }
                return true;
            };

            int line = 0;

            while (line < mono_height) {
                bool repeat = false;
                int count = 1;

                if (line + 1 < mono_height) {
                    repeat = lines_equal(line, line + 1);
                    count = 2;

                    while ((count < 15) && (line + count < mono_height)
                        && (lines_equal(repeat ? line : (line + count - 1), line + count) == repeat)) {
                        count++;
                    }

                    if (!repeat) {
                        count--;
                    }
                }

                write_bit(repeat ? 0 : 1);
                write_bit(count & 1);
                write_bit((count >> 1) & 1);
                write_bit((count >> 2) & 1);
                write_bit((count >> 3) & 1);

                for (int j = 0; j < (repeat ? 1 : count); j++) {
                    for (int col = 0; col < mono_width; col++) {
                        write_bit(src_bit(line + j, col));
                    }
                }

                line += count;
            }

            total_size = ((total_bit_write + 31) >> 5) * 4;

            if (bmp_type) {
                *bmp_type = glyph_bitmap_type::monochrome_glyph_bitmap;
            }

            character_metric.width = ft_convention_to_int_pixel(glyph->metrics.width);
            character_metric.height = ft_convention_to_int_pixel(glyph->metrics.height);
            character_metric.horizontal_bearing_x = ft_convention_to_int_pixel(glyph->metrics.horiBearingX);
            character_metric.horizontal_bearing_y = ft_convention_to_int_pixel(glyph->metrics.horiBearingY);
            character_metric.horizontal_advance = ft_convention_to_int_pixel(glyph->metrics.horiAdvance);
            character_metric.vertical_bearing_x = ft_convention_to_int_pixel(glyph->metrics.vertBearingX);
            character_metric.vertical_bearing_y = ft_convention_to_int_pixel(glyph->metrics.vertBearingY);
            character_metric.vertical_advance = ft_convention_to_int_pixel(glyph->metrics.vertAdvance);
            character_metric.bitmap_type = glyph_bitmap_type::monochrome_glyph_bitmap;

            return mono_expand_scratch_.data();
        }

        character_metric.width = ft_convention_to_int_pixel(glyph->metrics.width);
        character_metric.height = ft_convention_to_int_pixel(glyph->metrics.height);
        character_metric.horizontal_bearing_x = ft_convention_to_int_pixel(glyph->metrics.horiBearingX);
        character_metric.horizontal_bearing_y = ft_convention_to_int_pixel(glyph->metrics.horiBearingY);
        character_metric.horizontal_advance = ft_convention_to_int_pixel(glyph->metrics.horiAdvance);
        character_metric.vertical_bearing_x = ft_convention_to_int_pixel(glyph->metrics.vertBearingX);
        character_metric.vertical_bearing_y = ft_convention_to_int_pixel(glyph->metrics.vertBearingY);
        character_metric.vertical_advance = ft_convention_to_int_pixel(glyph->metrics.vertAdvance);
        character_metric.bitmap_type = glyph_bitmap_type::antialised_glyph_bitmap;

        return bitmap.buffer;
    }

    void freetype_font_adapter::free_glyph_bitmap(std::uint8_t *data) {
    }

    std::int32_t freetype_font_adapter::begin_get_atlas(std::uint8_t *atlas_ptr, const eka2l1::vec2 atlas_size) {
        auto pack_state = std::make_unique<atlas_pack_state>();

        pack_state->atlas_base_ = atlas_ptr;
        pack_state->atlas_size_ = atlas_size;

        pack_state->atlas_node_.resize(atlas_size.x);

        stbrp_init_target(&pack_state->atlas_context_, atlas_size.x, atlas_size.y, pack_state->atlas_node_.data(),
            static_cast<int>(pack_state->atlas_node_.size()));
        std::memset(pack_state->atlas_base_, 0, atlas_size.x * atlas_size.y);

        return static_cast<std::int32_t>(pack_states_.add(pack_state));
    }

    bool freetype_font_adapter::get_glyph_atlas(const std::int32_t handle, const std::size_t idx, const char16_t start_code, int *unicode_point, const char16_t num_code, const std::uint32_t metric_identifier, character_info *info) {
        auto pack_state = pack_states_.get(handle);

        if (!pack_state) {
            return false;
        }

        auto pack_state_ptr = pack_state->get();
        auto face = faces_[idx];

        if (!face) {
            return false;
        }

        if (!set_font_size(idx, metric_identifier)) {
            return false;
        }

        // Mirror the offscreen path: render small scalable glyphs as monochrome
        // (b/w hinter) instead of LCD. The atlas upload already has a MONO branch;
        // this keeps tiny CJK fallback text crisp instead of an aliased LCD smear.
        const bool want_mono = (face->size->metrics.y_ppem != 0)
            && (static_cast<std::uint32_t>(face->size->metrics.y_ppem) <= SMALL_GLYPH_MONO_PPEM_THRESHOLD);

        const FT_Int32 load_flags = want_mono ? (FT_LOAD_DEFAULT | FT_LOAD_TARGET_MONO) : FT_LOAD_DEFAULT;
        const FT_Render_Mode render_mode = want_mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_LCD;

        std::vector<stbrp_rect> pack_rects(num_code);

        // Measure pass: render each glyph exactly as the render pass below will.
        // FT_LOAD_BITMAP_METRICS_ONLY only yields dimensions for fonts with
        // embedded bitmap strikes; for outline fonts it leaves the slot bitmap
        // empty (0x0), so every rect packed to the same spot and the rendered
        // glyphs overwrote each other (fragmented "broken" text).
        for (auto i = 0; i < num_code; i++) {
            const char16_t char_code = unicode_point ? unicode_point[i] : static_cast<char16_t>(start_code + i);
            auto err = FT_Load_Char(face, char_code, load_flags);

            if (err) {
                LOG_WARN(SERVICE_FBS, "Failed to load character code 0x{:X} for face to get glyph atlas, error: {}",
                    static_cast<int>(char_code), FT_Error_String(err));
            } else {
                FT_Render_Glyph(face->glyph, render_mode);
            }

            const auto &measured = face->glyph->bitmap;
            const std::uint32_t measured_pixel_width = (measured.pixel_mode == FT_PIXEL_MODE_LCD)
                ? (measured.width / 3) : measured.width;

            pack_rects[i].x = 0;
            pack_rects[i].y = 0;
            pack_rects[i].w = measured_pixel_width + 10;
            pack_rects[i].h = measured.rows + 10;
        }

        if (!stbrp_pack_rects(&pack_state_ptr->atlas_context_, pack_rects.data(), static_cast<int>(pack_rects.size()))) {
            LOG_ERROR(SERVICE_FBS, "Failed to pack rects for glyph atlas");
            return false;
        }

        // Render and put bitmap to atlas
        for (auto i = 0; i < num_code; i++) {
            const char16_t char_code = unicode_point ? unicode_point[i] : static_cast<char16_t>(start_code + i);
            auto err = FT_Load_Char(face, char_code, load_flags);

            if (err) {
                LOG_WARN(SERVICE_FBS, "Failed to load character code 0x{:X} for face to get glyph atlas, error: {}",
                    static_cast<int>(char_code), FT_Error_String(err));
            }

            err = FT_Render_Glyph(face->glyph, render_mode);
            if (err) {
                LOG_WARN(SERVICE_FBS, "Failed to render character code 0x{:X} for face to get glyph atlas, error: {}",
                static_cast<int>(char_code), FT_Error_String(err));
            }

            auto glyph = face->glyph;
            auto bitmap = glyph->bitmap;

            auto &rect = pack_rects[i];
            auto dest = pack_state_ptr->atlas_base_ + (rect.x + 5) * 4 + (rect.y + 5) * pack_state_ptr->atlas_size_.x * 4;

            // Fonts with embedded bitmap strikes (e.g. CJK fonts like S60SC.ttf)
            // come out of FT_Render_Glyph still as packed 1bpp MONO — FreeType
            // does not convert bitmap glyphs to the requested LCD mode. Reading
            // them as 3-byte LCD triplets paints colored noise into the atlas.
            const bool is_mono = (bitmap.pixel_mode == FT_PIXEL_MODE_MONO);
            const std::uint32_t pixel_width = (bitmap.pixel_mode == FT_PIXEL_MODE_LCD) ? (bitmap.width / 3) : bitmap.width;

            for (auto y = 0; y < bitmap.rows; y++) {
                for (auto x = 0u; x < pixel_width; x++) {
                    eka2l1::vec4 color;

                    if (is_mono) {
                        const bool on = bitmap.buffer[y * bitmap.pitch + (x >> 3)] & (0x80 >> (x & 7));
                        const std::uint8_t lum = on ? 255 : 0;
                        color = eka2l1::vec4(lum, lum, lum, lum);
                    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                        const std::uint8_t lum = bitmap.buffer[y * bitmap.pitch + x];
                        color = eka2l1::vec4(lum, lum, lum, lum);
                    } else {
                        auto average = static_cast<float>(bitmap.buffer[x * 3 + y * bitmap.pitch] +
                            bitmap.buffer[x * 3 + y * bitmap.pitch + 1] +
                            bitmap.buffer[x * 3 + y * bitmap.pitch + 2]) / 3.0f;

                        color = eka2l1::vec4(bitmap.buffer[x * 3 + y * bitmap.pitch], bitmap.buffer[x * 3 + y * bitmap.pitch + 1],
                           bitmap.buffer[x * 3 + y * bitmap.pitch +2], static_cast<std::uint8_t>(average));

                        float max = (static_cast<float>(std::max({ color.x, color.y, color.z })) / 255.0f);
                        int min = std::min({ color.x, color.y, color.z });

                        color = color * max + eka2l1::vec4(color.x, color.y, color.z, min) * (1.0f - max);
                    }

                    dest[x * 4 + y * pack_state_ptr->atlas_size_.x * 4 + 0] = color.x;
                    dest[x * 4 + y * pack_state_ptr->atlas_size_.x * 4 + 1] = color.y;
                    dest[x * 4 + y * pack_state_ptr->atlas_size_.x * 4 + 2] = color.z;
                    dest[x * 4 + y * pack_state_ptr->atlas_size_.x * 4 + 3] = color.w;

                }
            }

            info[i].x0 = rect.x + 5;
            info[i].y0 = rect.y + 5;
            info[i].x1 = rect.x + 5 + pixel_width;
            info[i].y1 = rect.y + 5 + bitmap.rows;
            info[i].xadv = ft_convention_to_float(glyph->metrics.horiAdvance);
            info[i].xoff = static_cast<float>(glyph->bitmap_left);
            info[i].yoff = static_cast<float>(-glyph->bitmap_top);
            info[i].xoff2 = info[i].xoff + info[i].xadv;
            info[i].yoff2 = info[i].yoff + static_cast<float>(bitmap.rows);
        }

        return true;
    }

    void freetype_font_adapter::end_get_atlas(const std::int32_t handle) {
        pack_states_.remove(handle);
    }

    bool freetype_font_adapter::does_glyph_exist(std::size_t idx, std::uint32_t code, const std::uint32_t metric_identifier) {
        if (idx >= faces_.size()) {
            return false;
        }

        auto face = faces_[idx];

        if (code & 0x80000000) {
            return ((code & ~0x80000000) < face->num_glyphs);
        }

        return FT_Get_Char_Index(face, code) != 0;
    }

    std::size_t freetype_font_adapter::count() {
        return faces_.size();
    }

    bool freetype_font_adapter::has_character(const std::size_t face_index, const std::int32_t codepoint, const std::uint32_t metric_identifier) {
        if (face_index >= faces_.size()) {
            return false;
        }

        auto face = faces_[face_index];
        auto glyph_index = FT_Get_Char_Index(face, codepoint);

        return glyph_index != 0;
    }

    bool freetype_font_adapter::get_table_content(const std::size_t face_index, const std::uint32_t tag4, std::uint8_t *dest, uint32_t &dest_size) {
        auto face = faces_[face_index];

        FT_ULong dest_size_temp = !dest ? 0 : dest_size;
        auto table = FT_Load_Sfnt_Table(face, tag4, 0, dest, &dest_size_temp);

        if (!table) {
            return false;
        }

        dest_size = static_cast<std::uint32_t>(dest_size_temp);
        return true;
    }

    static constexpr float DESIGN_SIZE_SCALE = 9.0f / 10.0f;

    std::optional<open_font_metrics> freetype_font_adapter::get_nearest_supported_metric(const std::size_t face_index, const std::uint16_t targeted_font_size, std::uint32_t *metric_identifier,
        bool is_design_font_size) {
        if (face_index >= faces_.size()) {
            return std::nullopt;
        }

        auto face = faces_[face_index];
        auto adjusted_font_size = is_design_font_size ? (static_cast<int>(static_cast<float>(targeted_font_size) * DESIGN_SIZE_SCALE)) :
            derive_design_height_from_max_height(face, targeted_font_size);

        auto fake_design_height = is_design_font_size ? targeted_font_size : adjusted_font_size;

        // Fonts with embedded bitmap strikes (EBDT/EBLC, e.g. the S60 CJK system
        // font) are bitmap designs first: their TrueType outlines are low-quality
        // auto-traced placeholders that render as solid blobs. Snap the pixel size
        // to the nearest strike so FreeType always serves the hand-made bitmaps.
        // Outline-only fonts (num_fixed_sizes == 0) are unaffected.
        if (face->num_fixed_sizes > 0) {
            int best_strike = 0;
            int best_delta = INT_MAX;

            for (int i = 0; i < face->num_fixed_sizes; i++) {
                const int strike_ppem = static_cast<int>(face->available_sizes[i].y_ppem >> 6);
                const int delta = common::abs(strike_ppem - adjusted_font_size);

                if (delta < best_delta) {
                    best_delta = delta;
                    best_strike = strike_ppem;
                }
            }

            if (best_strike > 0) {
                adjusted_font_size = best_strike;
            }
        }

        if (!set_font_size(face_index, adjusted_font_size)) {
            return std::nullopt;
        }

        // TODO: Filling max depth?
        open_font_metrics metrics{};
        metrics.ascent = ft_convention_to_int_pixel(face->size->metrics.ascender);
        metrics.descent = ft_convention_to_int_pixel(face->size->metrics.descender);
        metrics.max_height = ft_convention_to_int_pixel(face->size->metrics.height);
        metrics.max_width = ft_convention_to_int_pixel(face->size->metrics.max_advance);
        metrics.max_depth = 0;
        metrics.baseline_correction = 0;
        metrics.design_height = static_cast<std::int16_t>(fake_design_height);

        if (metric_identifier) {
            *metric_identifier = adjusted_font_size;
        }

        return metrics;
    }

    std::uint32_t freetype_font_adapter::get_glyph_advance(const std::size_t face_index, const std::uint32_t codepoint, const std::uint32_t metric_identifier, const bool vertical) {
        auto face = faces_[face_index];
        if (!face) {
            return 0;
        }

        auto glyph_index = codepoint;
        if (glyph_index & 0x80000000) {
            glyph_index &= ~0x80000000;
        } else {
            glyph_index = FT_Get_Char_Index(face, codepoint);
        }

        if (!set_font_size(face_index, metric_identifier)) {
            return 0;
        }

        auto err = FT_Load_Glyph(face, glyph_index, FT_LOAD_ADVANCE_ONLY);
        if (err) {
            LOG_ERROR(SERVICE_FBS, "Failed to load glyph for face to get glyph advance, error: {}", FT_Error_String(err));
            return 0;
        }

        auto glyph = face->glyph;

        return vertical ? ft_convention_to_int_pixel(glyph->metrics.vertAdvance) :
                          ft_convention_to_int_pixel(glyph->metrics.horiAdvance);
    }
}
