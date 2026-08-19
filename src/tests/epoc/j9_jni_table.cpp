#include <catch2/catch.hpp>
#include <common/cvt.h>
#include <j2me/interface.h>
#include <kernel/j9_jni_table.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using eka2l1::hle::j9_jni_symbol;
using eka2l1::hle::j9_lookup_jni_symbol;
using eka2l1::hle::j9_scan_jni_export_table;

TEST_CASE("j9_scan_jni_export_table finds Java_ name/fn pairs", "[j9][jni]") {
    // Synthetic XIP-like image: code at VA 0x81A5B000, a name string and a
    // tiny Thumb fn, then a {name, fn} pair. Same pairing rules the emulator
    // uses when a midp2ams codeseg attaches.
    constexpr std::uint32_t run = 0x81A5B000;
    std::vector<std::uint8_t> img(0x200, 0);

    const char kName[] = "Java_com_symbian_j2me_midp_runtimeV2_Main__1markTime";
    const std::uint32_t name_off = 0x40;
    const std::uint32_t fn_off = 0x20;
    const std::uint32_t pair_off = 0x100;
    std::memcpy(img.data() + name_off, kName, sizeof(kName));
    img[fn_off] = 0x70;
    img[fn_off + 1] = 0x47; // bx lr

    const std::uint32_t name_ga = run + name_off;
    const std::uint32_t fn_ga = run + fn_off;
    std::memcpy(img.data() + pair_off, &name_ga, 4);
    std::memcpy(img.data() + pair_off + 4, &fn_ga, 4);

    std::vector<j9_jni_symbol> tab;
    const std::size_t n = j9_scan_jni_export_table(img.data(), run,
        static_cast<std::uint32_t>(img.size()), tab);
    REQUIRE(n >= 1);
    const std::uint32_t fn = j9_lookup_jni_symbol(tab, kName);
    REQUIRE(fn != 0);
    REQUIRE((fn & ~1u) == fn_ga);
}

TEST_CASE("j9_lookup_jni_symbol misses unknown natives", "[j9][jni]") {
    std::vector<j9_jni_symbol> tab;
    tab.push_back({ "Java_com_symbian_j2me_midp_runtimeV2_Main__1markTime", 0x81A5CBA9 });
    REQUIRE(j9_lookup_jni_symbol(tab, "Java_no_such_method") == 0);
    REQUIRE(j9_lookup_jni_symbol(tab, "Java_com_symbian_j2me_midp_runtimeV2_Main__1markTime") == 0x81A5CBA9);
}

TEST_CASE("j9_lookup_jni_symbol resolves Main._markTime suffix", "[j9][jni]") {
    std::vector<j9_jni_symbol> tab;
    tab.push_back({ "Java_com_symbian_j2me_midp_runtimeV2_Main__1markTime", 0x81A5CBAB });
    REQUIRE(j9_lookup_jni_symbol(tab, "Java_com_symbian_j2me_midp_runtimeV2_Main__1markTime") == 0x81A5CBAB);
    REQUIRE(j9_lookup_jni_symbol(tab, "_markTime") == 0x81A5CBAB);
    REQUIRE(j9_lookup_jni_symbol(tab, "markTime") == 0x81A5CBAB);
}

TEST_CASE("j9_emit_jni_walker patches PC-rel literal and miss-return", "[j9][jni]") {
    constexpr std::uint32_t pairs = 0x70200304;
    constexpr std::uint32_t orig = 0x818FC5D6;
    const auto w = eka2l1::hle::j9_emit_jni_walker(pairs, orig);
    REQUIRE(w.size() > 16);
    REQUIRE(w.size() * 4u < 0x300u);
    REQUIRE(w.front() == 0xE92D41FFu); // push {r0-r8,lr}
    REQUIRE(w.back() == (orig | 1u));
    REQUIRE(w[w.size() - 2] == pairs);

    REQUIRE((w[1] & 0xFFFFF000u) == 0xE59F4000u);
    const std::uint32_t imm12 = w[1] & 0xFFFu;
    const std::uint32_t lit_off = 4u + 8u + imm12;
    REQUIRE(lit_off / 4u == w.size() - 2u);

    bool saw_hit_ret = false;
    bool saw_tail = false;
    bool saw_miss_bkpt = false;
    int bkpts = 0;
    for (std::uint32_t insn : w) {
        if (insn == 0xE3A00000u) {
            saw_hit_ret = true;
        }
        if (insn == 0xE49D0004u) {
            saw_tail = true;
        }
        if (insn == 0xE1200070u) {
            saw_miss_bkpt = true;
            ++bkpts;
        }
    }
    REQUIRE(saw_hit_ret);
    REQUIRE(saw_tail);
    REQUIRE(saw_miss_bkpt);
    REQUIRE(bkpts >= 2);

    for (std::size_t i = 0; i + 1 < w.size(); ++i) {
        const std::uint32_t insn = w[i];
        const std::uint32_t top = insn >> 24;
        if ((top == 0xEA) || (top == 0x1A) || (top == 0x0A)) {
            const std::int32_t imm24 = static_cast<std::int32_t>(insn << 8) >> 8;
            const std::int32_t dest = static_cast<std::int32_t>(i) + 2 + imm24;
            REQUIRE(dest >= 0);
            REQUIRE(dest < static_cast<std::int32_t>(w.size()));
        }
    }
}

TEST_CASE("build_j9midps60_args includes decimal msid and MIDlet class", "[j9][j2me]") {
    const auto args = eka2l1::j2me::build_j9midps60_args(536870913u, u"AlpsFarm");
    const std::string utf8 = eka2l1::common::ucs2_to_utf8(args);
    REQUIRE(utf8.find("-msid 536870913") != std::string::npos);
    REQUIRE(utf8.find("-app AlpsFarm") != std::string::npos);
    REQUIRE(utf8.find("-jcl") == std::string::npos);
}
