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

#include <common/log.h>
#include <services/fbs/font_store.h>

namespace eka2l1::epoc {
    void font_store::add_fonts(std::vector<std::uint8_t> &buf, const epoc::adapter::font_file_adapter_kind adapter_kind,
        const bool prefer_for_glyph_fallback) {
        auto adapter = epoc::adapter::make_font_file_adapter(adapter_kind, buf);

        if (!adapter->is_valid()) {
            return;
        }

        for (std::size_t i = 0; i < adapter->count(); i++) {
            epoc::open_font_face_attrib attrib;
            epoc::open_font_metrics metrics;

            if (!adapter->get_face_attrib(i, attrib)) {
                continue;
            }

            const std::u16string fam_name = attrib.fam_name.to_std_string(nullptr);
            const std::u16string name = attrib.name.to_std_string(nullptr);

            bool found = false;

            // Are we facing duplicate fonts ?
            for (std::size_t i = 0; i < open_font_store.size(); i++) {
                if (open_font_store[i].face_attrib.name.to_std_string(nullptr) == name) {
                    found = true;
                    break;
                }
            }

            // No duplicate font
            if (!found) {
                // Get the metrics and make new open font
                open_font_info info;

                info.family = fam_name;
                info.idx = static_cast<std::int32_t>(i);
                info.face_attrib = attrib;
                info.adapter = adapter.get();
                info.prefer_for_glyph_fallback = prefer_for_glyph_fallback;

                open_font_store.push_back(std::move(info));
            }

            bool found_typeface = false;

            for (std::size_t i = 0; i < typefaces.size(); i++) {
                if (common::compare_ignore_case(typefaces[i].info_.name.to_std_string(nullptr), fam_name) == 0) {
                    // NOTE: Stored in here is actually pixels, they will just scale to twips later when retrieved through API
                    typefaces[i].is_scalable_ = adapter->vectorizable();
                    typefaces[i].num_heights_++;
                    typefaces[i].max_height_in_twips_ = common::max<std::int32_t>(typefaces[i].max_height_in_twips_, metrics.max_height); 
                    typefaces[i].min_height_in_twips_ = common::min<std::int32_t>(typefaces[i].min_height_in_twips_, attrib.min_size_in_pixels);

                    if (attrib.style & epoc::open_font_face_attrib::serif) {
                        typefaces[i].info_.flags |= epoc::typeface_info::tf_serif;
                    }

                    if (!(attrib.style & epoc::open_font_face_attrib::mono_width)) {
                        typefaces[i].info_.flags |= epoc::typeface_info::tf_propotional;
                    }

                    if (attrib.style & epoc::open_font_face_attrib::symbol) {
                        typefaces[i].info_.flags |= epoc::typeface_info::tf_symbol;
                    }

                    found_typeface = true;
                    break;
                }
            }

            if (!found_typeface) {
                epoc::typeface_support support;
                support.info_.name.assign(nullptr, fam_name);

                if (attrib.style & epoc::open_font_face_attrib::serif) {
                    support.info_.flags |= epoc::typeface_info::tf_serif;
                }

                if (!(attrib.style & epoc::open_font_face_attrib::mono_width)) {
                    support.info_.flags |= epoc::typeface_info::tf_propotional;
                }

                if (attrib.style & epoc::open_font_face_attrib::symbol) {
                    support.info_.flags |= epoc::typeface_info::tf_symbol;
                }

                support.num_heights_ = 1;
                support.is_scalable_ = adapter->vectorizable();
                support.max_height_in_twips_ = metrics.max_height;
                support.min_height_in_twips_ = attrib.min_size_in_pixels;

                typefaces.push_back(std::move(support));
            }
        }

        font_adapters.push_back(std::move(adapter));

        // New file may cover codepoints previously cached as not-found
        glyph_fallback_cache.clear();
    }

    bool font_store::is_cjk_placeholder_font(epoc::adapter::font_file_adapter_base *adapter, const std::size_t face_idx) {
        const auto key = std::make_pair(adapter, face_idx);
        auto cached = cjk_placeholder_cache.find(key);

        if (cached != cjk_placeholder_cache.end()) {
            return cached->second;
        }

        // Unrelated han characters shared by the JIS and GB repertoires (stub
        // fonts on western firmware keep a full CJK cmap but render the same
        // box for everything). A genuine CJK font renders them all distinct;
        // when every claimed probe comes out byte-identical, it's a stub.
        static constexpr std::uint32_t probe_codepoints[] = {
            0x4E2D, // 中
            0x4F53, // 体
            0x56FD, // 国
            0x65E5, // 日
            0x84DD, // 蓝 (GB-only: also catches partial JIS fonts when present)
        };

        std::uint32_t probe_metric = 0;
        bool placeholder = false;
        std::size_t rendered = 0;
        bool all_identical = true;
        std::vector<std::uint8_t> first_render;

        if (adapter->get_nearest_supported_metric(face_idx, 16, &probe_metric, true).has_value()) {
            auto render_probe = [&](const std::uint32_t codepoint, std::vector<std::uint8_t> &out) -> bool {
                int width = 0;
                int height = 0;
                std::uint32_t data_size = 0;
                epoc::glyph_bitmap_type type = epoc::glyph_bitmap_type::default_glyph_bitmap;
                epoc::open_font_character_metric metric;

                std::uint8_t *data = adapter->get_glyph_bitmap(face_idx, codepoint, probe_metric, &width, &height,
                    data_size, &type, metric);

                if (!data || !data_size) {
                    return false;
                }

                out.assign(data, data + data_size);
                out.push_back(static_cast<std::uint8_t>(width));
                out.push_back(static_cast<std::uint8_t>(height));

                adapter->free_glyph_bitmap(data);
                return true;
            };

            for (const std::uint32_t codepoint : probe_codepoints) {
                if (!adapter->has_character(face_idx, static_cast<std::int32_t>(codepoint), 0)) {
                    continue;
                }

                std::vector<std::uint8_t> render;
                if (!render_probe(codepoint, render)) {
                    continue;
                }

                if (rendered == 0) {
                    first_render = std::move(render);
                } else if (render != first_render) {
                    all_identical = false;
                }

                rendered++;
            }

            placeholder = (rendered >= 2) && all_identical;
        }

        if (placeholder) {
            LOG_INFO(SERVICE_FBS, "Font face {} of adapter 0x{:X} claims CJK coverage but renders placeholder boxes "
                                  "({} probes identical), rerouting its han glyphs to the fallback",
                face_idx, reinterpret_cast<std::uintptr_t>(adapter), rendered);
        }

        cjk_placeholder_cache.emplace(key, placeholder);
        return placeholder;
    }

    static inline bool codepoint_is_cjk_text(const std::uint32_t codepoint) {
        // CJK punctuation/kana/han + hangul + fullwidth forms: everything a
        // placeholder-outline UI font fakes with the same box glyph.
        return (codepoint >= 0x2E00) && (codepoint < 0xFFF0);
    }

    bool font_store::can_really_draw(epoc::adapter::font_file_adapter_base *adapter, const std::size_t face_idx,
        const std::uint32_t codepoint, const std::uint32_t metric_identifier) {
        if (!adapter->does_glyph_exist(face_idx, codepoint, metric_identifier)) {
            return false;
        }

        if (codepoint_is_cjk_text(codepoint) && is_cjk_placeholder_font(adapter, face_idx)) {
            return false;
        }

        return true;
    }

    open_font_info *font_store::seek_the_open_font_with_character(const std::uint32_t codepoint, epoc::adapter::font_file_adapter_base *exclude_adapter) {
        // Two passes: wide-coverage host fallback fonts first, then everything
        // else. Symbol fonts never serve text fallback (their cmaps claim text
        // codepoints but map them to dingbats).
        auto search = [this, codepoint](epoc::adapter::font_file_adapter_base *exclude) -> std::int32_t {
            for (int prefer_pass = 1; prefer_pass >= 0; prefer_pass--) {
                for (std::size_t i = 0; i < open_font_store.size(); i++) {
                    open_font_info &info = open_font_store[i];

                    if (info.prefer_for_glyph_fallback != static_cast<bool>(prefer_pass)) {
                        continue;
                    }

                    if (info.adapter == exclude) {
                        continue;
                    }

                    if (info.face_attrib.style & epoc::open_font_face_attrib::symbol) {
                        continue;
                    }

                    if (info.adapter->has_character(info.idx, static_cast<std::int32_t>(codepoint), 0)) {
                        if (codepoint_is_cjk_text(codepoint) && is_cjk_placeholder_font(info.adapter, info.idx)) {
                            continue; // its "coverage" is the same box for every han char
                        }

                        return static_cast<std::int32_t>(i);
                    }
                }
            }

            return -1;
        };

        // The cache stores the unrestricted best match: results that depend on the
        // caller's excluded adapter must not be served to (or recorded for) callers
        // with a different exclusion.
        std::int32_t found_index = -1;
        auto cached = glyph_fallback_cache.find(codepoint);

        if (cached != glyph_fallback_cache.end()) {
            found_index = cached->second;
        } else {
            found_index = search(nullptr);
            glyph_fallback_cache.emplace(codepoint, found_index);
        }

        if (found_index < 0) {
            return nullptr;
        }

        if (open_font_store[found_index].adapter == exclude_adapter) {
            found_index = search(exclude_adapter);

            if (found_index < 0) {
                return nullptr;
            }
        }

        return &open_font_store[found_index];
    }

    open_font_info *font_store::seek_the_font_by_uid(const epoc::uid the_uid, open_font_metrics &target_metric, std::uint32_t *metric_identifier) {
        for (auto &info : open_font_store) {
            if (std::optional<open_font_metrics> result = info.adapter->get_metric_with_uid(info.idx, the_uid, metric_identifier)) {
                target_metric = std::move(result.value());
                return &info;
            }
        }

        return nullptr;
    }

    open_font_info *font_store::seek_the_open_font(epoc::font_spec_base &spec) {
        open_font_info *best = nullptr;
        int best_score = -99999999;

        const std::u16string my_name = spec.tf.name.to_std_string(nullptr);
        std::int32_t spec_height = spec.height;

        for (auto &info : open_font_store) {
            bool maybe_same_family = (my_name.find(info.family) != std::string::npos);
            int score = 0;

            // Better returns me!
            if (info.face_attrib.name.to_std_string(nullptr) == my_name) {
                return &info;
            }

            if (maybe_same_family) {
                score += 10000;
            }

            std::optional<open_font_metrics> target_metric = info.adapter->get_nearest_supported_metric(info.idx, static_cast<std::uint16_t>(spec_height));

            if (target_metric.has_value()) {
                // Font that is not vectorizable, according to test, can't resize
                std::uint32_t subtract_score = (common::abs(spec.height - target_metric->design_height)) * 100;
                if (info.face_attrib.coverage[1] & 0x8000000) {
                    // NOTE: Some older games that use special character like Chinese seems to prefer smaller font
                    // more despite the twips size (since their character is larger in display). Here CJK flag of
                    // coverage is checked
                    // But I don't want to make it choose too big fonts like 1000 or too small like 1.
                    // So here we have the unit divided by 8, reducing penalty. Should help some fonts like
                    // 11x12 or 15x16 to choose 11x12... 
                    // I think the twip divider for epocv6 is close enough.
                    // Take N-Gage for example, with 130ppi, it should be around 11 or 12 if you do the math
                    subtract_score /= 8;
                }
                score -= subtract_score;
            }

            // Match the flags. This is also an important factor.
            if (info.face_attrib.style & epoc::open_font_face_attrib::italic) {
                // Extra flags are not welcome
                if ((static_cast<epoc::font_spec_v1 &>(spec).style.flags & epoc::font_style_base::italic)) {
                    score += 5000;
                } else {
                    score -= 3000;
                }
            }

            if (info.face_attrib.style & epoc::open_font_face_attrib::bold) {
                if (static_cast<epoc::font_spec_v1 &>(spec).style.flags & epoc::font_style_base::bold) {
                    score += 5000;
                } else {
                    score -= 3000;
                }
            }

            static constexpr std::uint32_t COVERAGE_WORD_COUNT = sizeof(info.face_attrib.coverage) / sizeof(info.face_attrib.coverage[0]);

            // The more coverage, the more varieties of the font.
            for (std::size_t i = 0; i < COVERAGE_WORD_COUNT; i++) {
                std::uint32_t coverage_patched = info.face_attrib.coverage[i];

                // Symbols take multiple bits, patch it out, give less scores
                if ((i == 1) && (coverage_patched & 0xFFFE)) {
                    score += 40;
                    coverage_patched &= ~0xFFFE;
                }

                score += 100 * common::count_bit_set(info.face_attrib.coverage[i]);
            }

            if (score > best_score) {
                best = &info;
                best_score = score;
            }
        }

        return best;
    }

    epoc::typeface_support *font_store::get_typeface_support(const std::uint32_t index) {
        if (index >= open_font_store.size()) {
            return nullptr;
        }
        return &typefaces[index];
    }
}