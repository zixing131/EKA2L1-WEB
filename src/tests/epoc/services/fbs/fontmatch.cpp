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

#include <catch2/catch.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <services/fbs/font_store.h>

using namespace eka2l1;

namespace {
    epoc::font_spec_v1 make_spec(const std::u16string &name, const std::int32_t height, const std::uint32_t style_flags,
        const std::uint32_t typeface_flags = epoc::typeface_info::tf_propotional) {
        epoc::font_spec_v1 spec{};

        spec.tf.name.assign(nullptr, name);
        spec.tf.flags = typeface_flags;
        spec.height = height;
        spec.style.flags = style_flags;

        return spec;
    }

    epoc::open_font_face_attrib make_face(const std::u16string &name, const std::int32_t style) {
        epoc::open_font_face_attrib attrib{};

        attrib.name.assign(nullptr, name);
        attrib.fam_name.assign(nullptr, name);
        attrib.style = style;

        return attrib;
    }
}

TEST_CASE("font_match_prefers_name_over_every_style_attribute", "fbs") {
    // MatchFontSpecsInPixels pays 10 for the name and 2 for the weight, so a
    // font asked for by name wins even when another one matches the weight.
    epoc::font_spec_v1 spec = make_spec(u"MHeiM-C-GB18030-S60", 20, epoc::font_style_base::bold);

    const epoc::open_font_face_attrib named = make_face(u"MHeiM-C-GB18030-S60", 0);
    const epoc::open_font_face_attrib bold = make_face(u"Nokia Sans S60 SemiBold", epoc::open_font_face_attrib::bold);

    REQUIRE(epoc::match_font_spec(named, u"MHeiM-C-GB18030-S60", spec, 20)
        > epoc::match_font_spec(bold, u"Nokia Sans S60 SemiBold", spec, 20));
}

TEST_CASE("font_match_costs_more_for_height_than_for_weight", "fbs") {
    // Height is worth 10 - |diff|, so being two pixels off already outweighs
    // the 2 points a weight match is worth. The old scoring had bold override
    // up to fifty pixels of difference.
    epoc::font_spec_v1 spec = make_spec(u"", 20, epoc::font_style_base::bold);

    const epoc::open_font_face_attrib bold = make_face(u"Bitmap Bold", epoc::open_font_face_attrib::bold);
    const epoc::open_font_face_attrib regular = make_face(u"Bitmap Regular", 0);

    REQUIRE(epoc::match_font_spec(regular, u"Bitmap Regular", spec, 20)
        > epoc::match_font_spec(bold, u"Bitmap Bold", spec, 17));

    // At the same height the weight decides after all.
    REQUIRE(epoc::match_font_spec(bold, u"Bitmap Bold", spec, 20)
        > epoc::match_font_spec(regular, u"Bitmap Regular", spec, 20));
}

TEST_CASE("font_match_scores_italic_mono_and_serif", "fbs") {
    epoc::font_spec_v1 upright = make_spec(u"", 20, 0);
    epoc::font_spec_v1 italic = make_spec(u"", 20, epoc::font_style_base::italic);

    const epoc::open_font_face_attrib slanted = make_face(u"Face Italic", epoc::open_font_face_attrib::italic);
    const epoc::open_font_face_attrib plain = make_face(u"Face", 0);

    REQUIRE(epoc::match_font_spec(slanted, u"Face Italic", italic, 20) > epoc::match_font_spec(plain, u"Face", italic, 20));
    REQUIRE(epoc::match_font_spec(plain, u"Face", upright, 20) > epoc::match_font_spec(slanted, u"Face Italic", upright, 20));

    // Monospace is rated above serif, as it changes the layout rather than
    // just the look.
    epoc::font_spec_v1 mono_wanted = make_spec(u"", 20, 0, 0);
    const epoc::open_font_face_attrib mono = make_face(u"Mono", epoc::open_font_face_attrib::mono_width);
    const epoc::open_font_face_attrib serif = make_face(u"Serif", epoc::open_font_face_attrib::serif);

    REQUIRE(epoc::match_font_spec(mono, u"Mono", mono_wanted, 20) > epoc::match_font_spec(serif, u"Serif", mono_wanted, 20));
}

TEST_CASE("font_match_ignores_name_when_none_was_asked_for", "fbs") {
    // An empty typeface name means "whatever fits", and must not hand the 10
    // points to a font whose own name happens to be empty too.
    epoc::font_spec_v1 spec = make_spec(u"", 20, 0);

    const epoc::open_font_face_attrib unnamed = make_face(u"", 0);
    const epoc::open_font_face_attrib named = make_face(u"Nokia Sans S60", 0);

    REQUIRE(epoc::match_font_spec(unnamed, u"", spec, 20) == epoc::match_font_spec(named, u"Nokia Sans S60", spec, 20));
}

namespace {
    // Synthetic fixtures model the ROM's CJK font: valid Chinese, cmap entries
    // for blank Latin glyphs, and a bogus Latin advance. No ROM data is needed.
    std::vector<std::uint8_t> make_ui_font(const std::string &family, const bool latin, const bool cjk) {
        std::string bdf = "STARTFONT 2.1\nFONT -test-" + family
            + "-medium-r-normal--14-140-72-72-c-80-iso10646-1\n"
              "SIZE 14 72 72\nFONTBOUNDINGBOX 8 14 0 0\nSTARTPROPERTIES 4\n"
              "FONT_ASCENT 14\nFONT_DESCENT 0\nFAMILY_NAME \"" + family
            + "\"\nWEIGHT_NAME \"Medium\"\nENDPROPERTIES\nCHARS 6\n";
        for (const int code : { 0x20, 0x30, 0x41, 0x61, 0x2026, 0x4E2D }) {
            const bool ink = code != 0x20 && ((code == 0x4E2D) ? cjk : latin);
            const int advance = (code == 0x4E2D) ? 14 : (latin ? 8 : 87);
            bdf += "STARTCHAR glyph" + std::to_string(code) + "\nENCODING " + std::to_string(code)
                + "\nSWIDTH 1000 0\nDWIDTH " + std::to_string(advance) + " 0\nBBX 8 14 0 0\nBITMAP\n";
            for (int row = 0; row < 14; row++) {
                bdf += ink ? "7E\n" : "00\n";
            }
            bdf += "ENDCHAR\n";
        }
        bdf += "ENDFONT\n";
        return { bdf.begin(), bdf.end() };
    }
}

TEST_CASE("ROM fonts with blank Latin glyphs use a drawable ROM fallback", "[fbs][web]") {
    for (const bool cjk_first : { false, true }) {
        epoc::font_store store(nullptr);
        auto chinese = make_ui_font("ChineseUI", false, true);
        auto latin = make_ui_font("LatinUI", true, false);
        const auto kind = epoc::adapter::font_file_adapter_kind::freetype;
        REQUIRE(store.add_fonts(cjk_first ? chinese : latin, kind));
        REQUIRE(store.add_fonts(cjk_first ? latin : chinese, kind));
        auto *chinese_face = store.get_open_font_info(cjk_first ? 0 : 1);
        REQUIRE(chinese_face);
        auto *original = chinese_face->adapter;
        REQUIRE(store.get_open_font_info(cjk_first ? 1 : 0)->adapter->has_character(0, 'A', 0));
        // cmap existence is insufficient; visible ASCII/ellipsis need ink.
        REQUIRE_FALSE(original->has_character(0, 'A', 14));
        REQUIRE_FALSE(original->does_glyph_exist(0, 'A', 14));
        REQUIRE_FALSE(original->has_character(0, 0x2026, 14));
        REQUIRE(original->has_character(0, ' ', 14));
        REQUIRE(original->has_character(0, 0x4E2D, 14));
        // Shaped glyph indices are face-specific and are not reinterpreted.
        REQUIRE(original->does_glyph_exist(0, 0x80000001, 14));

        const auto name = chinese_face->face_attrib.name.to_std_string(nullptr);
        // No host fonts are installed: fallback must use the ROM Latin face.
        store.attach_user_font_fallbacks();
        auto spec = make_spec(name, 16, 0);
        auto *selected = store.seek_the_open_font(spec);
        REQUIRE(selected == chinese_face);
        REQUIRE(selected->adapter != original);
        auto *linked = selected->adapter;
        REQUIRE(linked->has_character(0, 'A', 14));
        REQUIRE(linked->get_glyph_advance(0, 'A', 14) == 8);
        REQUIRE(linked->get_glyph_advance(0, 0x2026, 14) == 8);
        REQUIRE(linked->get_glyph_advance(0, 0x4E2D, 14) == 14);
        REQUIRE(linked->get_glyph_advance(0, ' ', 14) == 87);

        const int code = 'A';
        eka2l1::vec2 size;
        REQUIRE(linked->measure_atlas_glyphs(0, &code, 1, 14, &size));
        REQUIRE(size.x > 0);
        REQUIRE(size.y > 0);
        std::vector<std::uint8_t> pixels(size.x * size.y * 4, 0);
        const eka2l1::vec2 position(0, 0);
        epoc::adapter::character_info glyph{};
        REQUIRE(linked->render_atlas_glyphs(0, &code, 1, 14, pixels.data(), size, &position, &glyph));
        REQUIRE(std::any_of(pixels.begin(), pixels.end(), [](std::uint8_t pixel) { return pixel != 0; }));
        REQUIRE(glyph.xadv == 8);
        epoc::glyph_bitmap_type type = epoc::antialised_glyph_bitmap;
        epoc::open_font_character_metric metric{};
        std::uint32_t bytes = 0;
        auto *bitmap = linked->get_glyph_bitmap(0, 'A', 14, nullptr, nullptr, bytes, &type, metric);
        REQUIRE(bitmap);
        REQUIRE(metric.horizontal_advance == 8);
        linked->free_glyph_bitmap(bitmap);
    }
}
