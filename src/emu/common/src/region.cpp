/*
 * Copyright (c) 2020 EKA2L1 Team
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
#include <common/region.h>

#include <climits>

namespace eka2l1::common {
    /**
     * NOTE: CODE REVIEWED FROM SYMBIAN OPEN SOURCE.
     */

    eka2l1::rect region::bounding_rect() const {
        if (rects_.empty()) return eka2l1::rect({0, 0}, {0, 0});
        eka2l1::vec2 tl{ INT_MAX, INT_MAX };
        eka2l1::vec2 br{ INT_MIN, INT_MIN };

        for (std::size_t i = 0; i < rects_.size(); i++) {
            tl.x = common::min(tl.x, rects_[i].top.x);
            tl.y = common::min(tl.y, rects_[i].top.y);
            br.x = common::max(br.x, rects_[i].top.x + rects_[i].size.x);
            br.y = common::max(br.y, rects_[i].top.y + rects_[i].size.y);
        }

        return eka2l1::rect{ tl, br - tl };
    }

    bool region::add_rect(const eka2l1::rect &rect) {
        if (rect.size.x <= 0 || rect.size.y <= 0) return false;
        if (!rects_.empty() && rect.contains(bounding_rect())) {
            if (rects_.size() == 1 && rects_.front() == rect) return false;
            const auto copy = rect;
            rects_ = {copy};
            return true;
        }
        // Add only the uncovered pieces. The previous implementation removed
        // the existing rectangle and kept pieces of the new one, losing their
        // intersection and sometimes introducing overlapping rectangles.
        region remaining;
        remaining.rects_.push_back(rect);
        for (const auto &existing : rects_) remaining.eliminate(existing);
        if (remaining.empty()) return false;
        rects_.insert(rects_.end(), remaining.rects_.begin(), remaining.rects_.end());
        return true;
    }

    bool region::add_region(const region &rg) {
        if (this == &rg) return false;
        bool modded = false;
        for (std::size_t i = 0; i < rg.rects_.size(); i++) {
            if (add_rect(rg.rects_[i])) {
                modded = true;
            }
        }

        return modded;
    }

    void region::eliminate(const eka2l1::rect &rect) {
        if (rect.size.x <= 0 || rect.size.y <= 0) return;
        std::vector<eka2l1::rect> remaining;
        for (const auto &original : rects_) {
            const auto intersection = original.intersect(rect);
            if (intersection.empty()) {
                remaining.push_back(original);
                continue;
            }
            const auto end = original.bottom_right();
            const auto cut_end = intersection.bottom_right();
            auto piece = [&](vec2 top, vec2 bottom) {
                const eka2l1::rect part(top, bottom - top);
                if (part.size.x > 0 && part.size.y > 0) remaining.push_back(part);
            };
            piece(original.top, {end.x, intersection.top.y});
            piece({original.top.x, cut_end.y}, end);
            piece({original.top.x, intersection.top.y}, {intersection.top.x, cut_end.y});
            piece({cut_end.x, intersection.top.y}, {end.x, cut_end.y});
        }
        rects_ = std::move(remaining);
    }

    void region::eliminate(const region &reg) {
        if (this == &reg) { make_empty(); return; }
        for (std::size_t i = 0; i < reg.rects_.size(); i++) {
            eliminate(reg.rects_[i]);
        }
    }

    region region::intersect(const region &target) const {
        region intersection;

        for (std::size_t i = 0; i < rects_.size(); i++) {
            for (std::size_t j = 0; j < target.rects_.size(); j++) {
                eka2l1::rect the_intersect = target.rects_[j].intersect(rects_[i]);

                if (the_intersect.size.x > 0 && the_intersect.size.y > 0) {
                    intersection.rects_.push_back(the_intersect);
                }
            }
        }

        return intersection;
    }

    bool region::identical(const region &rhs) const {
        if (rects_.size() != rhs.rects_.size()) {
            return false;
        }

        for (std::size_t i = 0; i < rects_.size(); i++) {
            if ((rects_[i].size != rhs.rects_[i].size) || (rects_[i].top != rhs.rects_[i].top)) {
                return false;
            }
        }

        return true;
    }

    void region::advance(const eka2l1::vec2 &amount) {
        for (std::size_t i = 0; i < rects_.size(); i++) {
            rects_[i].top += amount;
        }
    }

    void region::clip(const eka2l1::rect &bounding) {
        if ((bounding.size.x <= 0) || (bounding.size.y <= 0)) {
            rects_.clear();
            return;
        }

        for (std::size_t i = 0; i < rects_.size();) {
            rects_[i] = rects_[i].intersect(bounding);

            if ((rects_[i].size.x <= 0) || (rects_[i].size.y <= 0)) {
                rects_.erase(rects_.begin() + i);
            } else {
                i++;
            }
        }
    }

    bool region::contains(const eka2l1::point &p) {
        for (std::size_t i = 0; i < rects_.size(); i++) {
            if (rects_[i].contains(p)) {
                return true;
            }
        }

        return false;
    }
}
