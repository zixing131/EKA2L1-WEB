#include <catch2/catch.hpp>
#include <kernel/game_patches.h>
#include <algorithm>
#include <vector>

namespace {
    std::vector<std::uint8_t> super_miners_image() {
        std::vector<std::uint8_t> code(0x764D0, 0xCC);
        const std::uint8_t prologue[] = {
            0xF8, 0x40, 0x2D, 0xE9, 0x00, 0x40, 0xA0, 0xE1,
            0x00, 0x60, 0xA0, 0xE3, 0x20, 0x60, 0x80, 0xE5
        };
        const std::uint8_t selection[] = {
            0x02, 0x00, 0x50, 0xE3, 0x38, 0x60, 0x84, 0x15,
            0x5A, 0x0F, 0xA0, 0x13, 0x02, 0x0B, 0xA0, 0x03,
            0x38, 0x70, 0x84, 0x05, 0x04, 0x00, 0x85, 0xE5
        };
        std::copy(std::begin(prologue), std::end(prologue), code.begin() + 0x8654);
        std::copy(std::begin(selection), std::end(selection), code.begin() + 0x86A0);
        return code;
    }
}

TEST_CASE("Super Miners renderer patch preserves the rest of the image", "[kernel][compat]") {
    auto code = super_miners_image();
    auto expected = code;
    const std::uint8_t cmp_r6_one[] = {1, 0, 0x56, 0xE3};
    std::copy(std::begin(cmp_r6_one), std::end(cmp_r6_one), expected.begin() + 0x86A0);
    REQUIRE(eka2l1::kernel::apply_super_miners_bitmap_fallback(0x2002517C, code.data(), code.size()));
    REQUIRE(code == expected);
    REQUIRE_FALSE(eka2l1::kernel::apply_super_miners_bitmap_fallback(0x2002517C, code.data(), code.size()));
    REQUIRE(code == expected);
}

TEST_CASE("Super Miners renderer patch rejects unknown and incomplete images", "[kernel][compat]") {
    auto code = super_miners_image();
    auto uid = 0x2002517Cu;
    SECTION("other application") { uid ^= 1; }
    SECTION("changed register initialization") { code[0x865C] = 1; }
    SECTION("changed renderer selection") { code[0x86AC] ^= 1; }
    SECTION("truncated image") { code.resize(0x86A1); }
    SECTION("different build size") { code.push_back(0); }
    const auto before = code;
    REQUIRE_FALSE(eka2l1::kernel::apply_super_miners_bitmap_fallback(uid, code.data(), code.size()));
    REQUIRE(code == before);
    REQUIRE_FALSE(eka2l1::kernel::apply_super_miners_bitmap_fallback(0x2002517C, nullptr, 0x764D0));
}
