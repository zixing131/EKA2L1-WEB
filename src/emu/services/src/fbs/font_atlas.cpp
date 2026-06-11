/*
 * Copyright (c) 2019 EKA2L1 Team
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

#include <drivers/graphics/graphics.h>
#include <services/fbs/font_atlas.h>
#include <services/fbs/font_store.h>

#include <common/algorithm.h>
#include <common/log.h>
#include <common/time.h>

#include <unordered_map>

namespace eka2l1::epoc {
    font_atlas::font_atlas()
        : atlas_handle_(0)
        , atlas_data_(nullptr)
        , pack_handle_(0)
        , store_(nullptr) {
    }

    font_atlas::font_atlas(adapter::font_file_adapter_base *adapter, const std::size_t typeface_idx, const char16_t initial_start,
        const char16_t initial_char_count, const int font_size, const std::uint32_t metric_identifier,
        font_store *store)
        : atlas_handle_(0)
        , adapter_(adapter)
        , metric_identifier_(metric_identifier)
        , size_(font_size)
        , initial_range_(initial_start, initial_char_count)
        , typeface_idx_(typeface_idx)
        , pack_handle_(0)
        , store_(store) {
        atlas_data_ = nullptr;
    }

    void font_atlas::init(adapter::font_file_adapter_base *adapter, const std::size_t typeface_idx, const char16_t initial_start,
        const char16_t initial_char_count, const int font_size, const std::uint32_t metric_identifier,
        font_store *store) {
        adapter_ = adapter;
        atlas_handle_ = 0;
        metric_identifier_ = metric_identifier;
        size_ = font_size;
        initial_range_ = { initial_start, initial_char_count };
        typeface_idx_ = typeface_idx;
        pack_handle_ = 0;

        store_ = store;
        fallback_sources_.clear();

        atlas_data_.reset();
    }

    void font_atlas::destroy(drivers::graphics_driver *driver) {
        for (auto &source : fallback_sources_) {
            if (source.atlas_) {
                source.atlas_->destroy(driver);
            }
        }

        fallback_sources_.clear();

        if (atlas_handle_) {
            drivers::graphics_command_builder builder;
            builder.destroy_bitmap(atlas_handle_);

            drivers::command_list retrieved = builder.retrieve_command_list();
            driver->submit_command_list(retrieved);

            atlas_handle_ = 0;
            atlas_data_.reset();
        }

        if (pack_handle_) {
            adapter_->end_get_atlas(pack_handle_);
        }

        last_use_.clear();
        characters_.clear();
    }

    int font_atlas::get_atlas_width() const {
        return common::align(ESTIMATE_MAX_CHAR_IN_ATLAS_WIDTH * size_, 1024);
    }

    bool font_atlas::prepare_glyphs(const std::u16string &chars, drivers::graphics_driver *driver,
        drivers::graphics_command_builder &upload_builder) {
        const int width = get_atlas_width();
        const int atlas_byte_size = width * width * adapter_->get_atlas_bitmap_bits_per_pixel() / 8;

        if (!atlas_data_) {
            atlas_data_ = std::make_unique<std::uint8_t[]>(atlas_byte_size);
            auto cinfos = std::make_unique<adapter::character_info[]>(initial_range_.second);

            pack_handle_ = adapter_->begin_get_atlas(atlas_data_.get(), { width, width });

            if (pack_handle_ == -1) {
                return false;
            }

            if (initial_range_.second > 0) {
                if (!adapter_->get_glyph_atlas(pack_handle_, typeface_idx_, initial_range_.first, nullptr, initial_range_.second,
                        metric_identifier_, cinfos.get())) {
                    return false;
                }

                // initialize the last used list and character map
                for (char16_t i = 0; i < initial_range_.second; i++) {
                    last_use_.push_back(initial_range_.first + i);
                    characters_.emplace(initial_range_.first + i, cinfos[i]);
                }
            }

            // Submit the bitmap through another queue, in case the command list above never got submitted
            atlas_handle_ = drivers::create_bitmap(driver, { width, width },  adapter_->get_atlas_bitmap_bits_per_pixel());

            upload_builder.update_bitmap(atlas_handle_, reinterpret_cast<const char *>(atlas_data_.get()),
                atlas_byte_size, { 0, 0 }, { width, width });
            upload_builder.set_texture_filter(atlas_handle_, false, drivers::filter_option::nearest);
        }

        std::vector<int> to_rast;
        std::vector<char16_t> unique_char;

        // Iterate through characters, and filter out characters which is not available in the atlas.
        // Add character to last used, too.
        for (auto &chr : chars) {
            if (characters_.find(chr) == characters_.end()) {
                if (!std::binary_search(to_rast.begin(), to_rast.end(), chr)) {
                    to_rast.push_back(chr);
                    std::sort(to_rast.begin(), to_rast.end());
                }
            }

            if (std::find(unique_char.begin(), unique_char.end(), chr) == unique_char.end()) {
                // Move-to-front MRU: each atlas resident appears exactly once,
                // so the rebuild below can treat the list as "who is in the
                // atlas, most recent first". The old insert-then-chop-tail kept
                // duplicates and silently dropped residents that were still in
                // characters_ — after a rebuild those drew stale rects from
                // repacked atlas regions (fragmented glyph debris).
                auto existing = std::find(last_use_.begin(), last_use_.end(), static_cast<int>(chr));
                if (existing != last_use_.end()) {
                    last_use_.erase(existing);
                }

                last_use_.insert(last_use_.begin(), chr);
                unique_char.push_back(chr);
            }
        }

        if (!to_rast.empty()) {
            // Try to rasterize these
            auto cinfos = std::make_unique<adapter::character_info[]>(to_rast.size());

            if (!adapter_->get_glyph_atlas(pack_handle_, typeface_idx_, 0, to_rast.data(), static_cast<char16_t>(to_rast.size()), metric_identifier_,
                    cinfos.get())) {
                // The atlas is full. Rebuild it from the most recently used
                // glyphs (the requested ones are at the front of the MRU).
                // Halve the resident set until a rebuild fits: a failed pack
                // attempt mutates the packer state, so restart the atlas for
                // every retry.
                std::size_t take = std::min(characters_.size() + to_rast.size(), last_use_.size());
                bool rebuilt = false;

                auto evict_cinfos = std::make_unique<adapter::character_info[]>(take);

                while (take > 0) {
                    adapter_->end_get_atlas(pack_handle_);
                    pack_handle_ = adapter_->begin_get_atlas(atlas_data_.get(), { width, width });

                    if (pack_handle_ == -1) {
                        return false;
                    }

                    if (adapter_->get_glyph_atlas(pack_handle_, typeface_idx_, 0, &last_use_[0], static_cast<char16_t>(take),
                            metric_identifier_, evict_cinfos.get())) {
                        rebuilt = true;
                        break;
                    }

                    take /= 2;
                }

                if (!rebuilt) {
                    return false;
                }

                // The atlas now holds exactly the first `take` MRU glyphs:
                // anything else would point at repacked regions, so drop it.
                // Evicted characters get re-rasterized on their next use.
                characters_.clear();
                last_use_.resize(take);

                for (std::size_t i = 0; i < take; i++) {
                    characters_[last_use_[i]] = evict_cinfos[i];
                }

                upload_builder.update_bitmap(atlas_handle_, reinterpret_cast<const char *>(atlas_data_.get()),
                    atlas_byte_size, { 0, 0 }, { width, width });
            } else {
                // Update the characters
                for (char16_t i = 0; i < static_cast<char16_t>(to_rast.size()); i++) {
                    characters_.emplace(to_rast[i], cinfos[i]);
                }

                upload_builder.update_bitmap(atlas_handle_, reinterpret_cast<const char *>(atlas_data_.get()),
                    atlas_byte_size, { 0, 0 }, { width, width });
            }
        }

        return true;
    }

    bool font_atlas::draw_text(const std::u16string &text, const eka2l1::rect &text_box, const epoc::text_alignment alignment, drivers::graphics_driver *driver, drivers::graphics_command_builder &builder, const eka2l1::vec2f scale_vector) {
        drivers::graphics_command_builder upload_builder;

        auto is_control_char = [](const char16_t chr) {
            return (chr >= 0x200c && chr <= 0x200f) || (chr >= 0x202a && chr <= 0x202e) || (chr >= 0xfffe && chr <= 0xffff);
        };

        // Split the text: glyphs the bound font can't draw but another loaded font
        // can are routed to a secondary atlas. Everything else (including the
        // bound font's own notdef for truly missing glyphs) stays on the primary.
        // Resolved per character: different characters may borrow from different
        // fonts (e.g. a ROM font with partial CJK plus the bundled wide-coverage
        // host font), each with its own atlas.
        std::u16string primary_text;
        std::vector<std::u16string> fallback_texts;
        std::unordered_map<char16_t, std::size_t> fallback_chars;

        for (auto &chr : text) {
            bool to_fallback = false;

            if (store_ && !is_control_char(chr) && !store_->can_really_draw(adapter_, typeface_idx_, chr, metric_identifier_)) {
                auto known = fallback_chars.find(chr);

                if (known != fallback_chars.end()) {
                    to_fallback = true;
                } else if (epoc::open_font_info *fb = store_->seek_the_open_font_with_character(chr, adapter_)) {
                    std::size_t source_index = 0;

                    for (; source_index < fallback_sources_.size(); source_index++) {
                        if ((fallback_sources_[source_index].adapter_ == fb->adapter)
                            && (fallback_sources_[source_index].idx_ == fb->idx)) {
                            break;
                        }
                    }

                    if (source_index == fallback_sources_.size()) {
                        std::uint32_t fb_metric_identifier = 0;

                        if (fb->adapter->get_nearest_supported_metric(fb->idx, static_cast<std::uint16_t>(size_),
                                &fb_metric_identifier, true).has_value()) {
                            fallback_sources_.push_back({ fb->adapter, fb->idx, fb_metric_identifier, nullptr });
                        } else {
                            source_index = static_cast<std::size_t>(-1);
                        }
                    }

                    if (source_index != static_cast<std::size_t>(-1)) {
                        fallback_chars.emplace(chr, source_index);

                        if (fallback_texts.size() < fallback_sources_.size()) {
                            fallback_texts.resize(fallback_sources_.size());
                        }

                        fallback_texts[source_index].push_back(chr);
                        to_fallback = true;
                    }
                }
            }

            if (!to_fallback) {
                primary_text.push_back(chr);
            }
        }

        if (!prepare_glyphs(primary_text, driver, upload_builder)) {
            return false;
        }

        for (std::size_t i = 0; i < fallback_texts.size(); i++) {
            if (fallback_texts[i].empty()) {
                continue;
            }

            fallback_source &source = fallback_sources_[i];

            if (!source.atlas_) {
                source.atlas_ = std::make_unique<font_atlas>();
                // No seed range: only the borrowed glyphs live here.
                source.atlas_->init(source.adapter_, source.idx_, 0, 0, size_, source.metric_identifier_);
            }

            source.atlas_->prepare_glyphs(fallback_texts[i], driver, upload_builder);
        }

        // Pick the atlas that actually holds a character's glyph.
        auto resolve_glyph = [&](const char16_t chr) -> std::pair<font_atlas *, adapter::character_info *> {
            auto fb_it = fallback_chars.find(chr);

            if (fb_it != fallback_chars.end()) {
                font_atlas *fb_atlas = fallback_sources_[fb_it->second].atlas_.get();

                if (fb_atlas) {
                    auto it = fb_atlas->characters_.find(chr);
                    if (it != fb_atlas->characters_.end()) {
                        return { fb_atlas, &it->second };
                    }
                }
            }

            auto it = characters_.find(chr);
            if (it != characters_.end()) {
                return { this, &it->second };
            }

            return { nullptr, nullptr };
        };

        eka2l1::vec2 cur_pos = text_box.top;

        // Calculate size of the text to know where to put them
        // If other alignment then left is on
        if (alignment != epoc::text_alignment::left) {
            float size_length = 0;

            for (auto &chr : text) {
                auto resolved = resolve_glyph(chr);
                if (resolved.second) {
                    size_length += static_cast<int>(resolved.second->xadv * scale_vector[0]);
                }
            }

            if (alignment == epoc::text_alignment::right) {
                cur_pos.x = text_box.size.x + text_box.top.x - static_cast<int>(size_length);
            } else {
                cur_pos.x += static_cast<int>((text_box.size.x - size_length) / 2);
            }
        }

        builder.set_feature(drivers::graphics_feature::blend, true);
        builder.blend_formula(drivers::blend_equation::add, drivers::blend_equation::add,
            drivers::blend_factor::frag_out_alpha, drivers::blend_factor::one_minus_frag_out_alpha,
            drivers::blend_factor::one, drivers::blend_factor::one);

        // Start to render these texts.
        for (auto &chr : text) {
            if (is_control_char(chr)) {
                // Skip control characters
                // TODO: Handle them properly
                continue;
            }

            auto resolved = resolve_glyph(chr);
            if (!resolved.second) {
                continue;
            }

            adapter::character_info &info = *resolved.second;
            const drivers::handle glyph_atlas_handle = resolved.first->atlas_handle_;

            eka2l1::rect source_rect;
            source_rect.top = { info.x0, info.y0 };
            source_rect.size = eka2l1::object_size(info.x1 - info.x0, info.y1 - info.y0);

            eka2l1::rect dest_rect;
            dest_rect.top.x = cur_pos.x + static_cast<int>(info.xoff * scale_vector[0]);
            dest_rect.top.y = cur_pos.y + static_cast<int>(info.yoff * scale_vector[1]);

            dest_rect.size.x = static_cast<int>((info.xoff2 - info.xoff) * scale_vector[0]);
            dest_rect.size.y = static_cast<int>((info.yoff2 - info.yoff) * scale_vector[1]);

            if ((dest_rect.size.x != 0) && (dest_rect.size.y != 0) && (source_rect.size.x != 0) && (source_rect.size.y != 0)) {
                builder.draw_bitmap(glyph_atlas_handle, 0, dest_rect, source_rect, eka2l1::vec2(0, 0), 0.0f,
                    drivers::bitmap_draw_flag_use_brush);
            }

            // TODO: Newline
            cur_pos.x += static_cast<int>(std::round(info.xadv * scale_vector[0]));
        }

        builder.set_feature(drivers::graphics_feature::blend, false);

        drivers::command_list retrieved = upload_builder.retrieve_command_list();
        driver->submit_command_list(retrieved);

        return true;
    }
}