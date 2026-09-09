#include <catch2/catch.hpp>
#include <loader/e32img.h>
#include <services/featmgr/featmgr.h>
#include <cstring>

TEST_CASE("ROM static feature tables are bounded by the loaded code image", "[featmgr]") {
    eka2l1::loader::e32img image{};
    image.header.code_base = 0x8000;
    image.header.code_offset = 0x20;
    image.header.code_size = 0x60;
    image.data.resize(0x80);
    const unsigned char lookup[] = {
        0x00,0x20,0x01,0x23,0x10,0xB5,0x08,0x4C,0xDB,0x07,0x04,0xE0,
        0x91,0x42,0x01,0xD1,0x01,0x20,0x10,0xBD,0x40,0x1C,0x82,0x00,
        0xA2,0x58,0x9A,0x42,0xF6,0xD1,0x00,0x20,0x10,0xBD
    };
    std::memcpy(image.data.data() + 0x20, lookup, sizeof(lookup));
    auto put = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(image.data.data() + 0x20 + offset, &value, 4);
    };
    put(0x28, 0x8040);
    put(0x40, 12);
    put(0x44, 1715);
    put(0x48, 0x80000000);
    CHECK(eka2l1::read_static_features(image) == std::vector<eka2l1::epoc::uid>{12, 1715});
    SECTION("a pointer outside the image is rejected") {
        put(0x28, 0x7FFC);
        CHECK(eka2l1::read_static_features(image).empty());
        put(0x28, 0xFFFFFFFC);
        CHECK(eka2l1::read_static_features(image).empty());
    }
    SECTION("a truncated list is rejected as a whole") {
        image.header.code_size = 0x48;
        CHECK(eka2l1::read_static_features(image).empty());
    }
    SECTION("malformed image extents are rejected") {
        image.header.code_size = 0xFFFFFFFF;
        CHECK(eka2l1::read_static_features(image).empty());
    }
    SECTION("similar unrelated code is not treated as a feature provider") {
        image.data[0x20 + 28] = 0;
        CHECK(eka2l1::read_static_features(image).empty());
    }
}
