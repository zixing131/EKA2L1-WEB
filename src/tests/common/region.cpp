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
#include <common/region.h>

using namespace eka2l1;

TEST_CASE("clip_region_to_bounds", "region") {
    common::region region;
    region.add_rect(eka2l1::rect({ -10, -20 }, { 40, 50 }));

    region.clip(eka2l1::rect({ 0, 0 }, { 20, 15 }));

    REQUIRE(region.rects_.size() == 1);
    REQUIRE(region.rects_[0] == eka2l1::rect({ 0, 0 }, { 20, 15 }));
}

TEST_CASE("clip_region_removes_empty_rectangles", "region") {
    common::region region;
    region.add_rect(eka2l1::rect({ 0, 0 }, { 240, 236 }));

    region.clip(eka2l1::rect({ 0, 0 }, { 0, 0 }));

    REQUIRE(region.empty());
}

TEST_CASE("region union and subtraction preserve covered pixels", "[region]") {
    common::region region;
    bool pixels[24][24]{};
    std::uint32_t seed = 9247;
    auto next = [&]() { seed = seed * 1664525 + 1013904223; return seed; };
    for (int step = 0; step < 160; ++step) {
        const rect area({int(next() % 18), int(next() % 18)},
                        {int(next() % 6 + 1), int(next() % 6 + 1)});
        const bool add = (step % 3) != 2;
        if (add) region.add_rect(area);
        else region.eliminate(area);
        for (int y = area.top.y; y < area.top.y + area.size.y; ++y)
            for (int x = area.top.x; x < area.top.x + area.size.x; ++x)
                pixels[y][x] = add;
        bool matches = true, overlaps = false;
        for (int y = 0; y < 24; ++y) for (int x = 0; x < 24; ++x) {
            int count = 0;
            for (const auto &part : region.rects_)
                count += x >= part.top.x && x < part.bottom_right().x
                    && y >= part.top.y && y < part.bottom_right().y;
            matches &= (count != 0) == pixels[y][x];
            overlaps |= count > 1;
        }
        REQUIRE(matches);
        REQUIRE_FALSE(overlaps);
        for (const auto &part : region.rects_) {
            REQUIRE(part.size.x > 0);
            REQUIRE(part.size.y > 0);
        }
    }
    CHECK_FALSE(region.add_region(region));
    region.eliminate(region);
    CHECK(region.empty());
}

TEST_CASE("region subtraction visits consecutive rectangles", "[region]") {
    common::region region;
    region.add_rect(rect({0, 0}, {10, 10}));
    region.add_rect(rect({12, 0}, {10, 10}));
    region.eliminate(rect({0, 0}, {24, 8}));
    CHECK_FALSE(region.contains({15, 5}));
    CHECK(region.contains({15, 9}));
}
