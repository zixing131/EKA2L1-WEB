/*
 * Copyright (c) 2026 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project - WebAssembly port.
 *
 * EKA2L1-WEB copyright / version-tracking / tamper-detection. The whole point
 * of this file is that the decision logic is compiled into the wasm binary, so
 * editing the HTML/JS shell cannot disable the copyright notice, the domain
 * whitelist, or the file-integrity checks.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "protection.h"

#include <common/crypt.h>
#include <common/version.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef EMSCRIPTEN_KEEPALIVE
#define EMSCRIPTEN_KEEPALIVE
#endif

// Build timestamp: configure_file generates build_stamp.h fresh each build so
// __DATE__/__TIME__ staleness (a TU only re-stamps when it recompiles) can't
// freeze the watermark. Fall back to the compiler macros if it isn't present.
#if defined(__has_include)
#if __has_include("build_stamp.h")
#include "build_stamp.h"
#endif
#endif
#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP (__DATE__ " " __TIME__)
#endif

#ifdef EKA2L1_DEBUG_BUILD
#define EKA2L1_BUILD_CHANNEL "Test"
#else
#define EKA2L1_BUILD_CHANNEL "Release"
#endif

namespace eka2l1::web::protection {
    // ------------------------------------------------------------------------
    // Copyright string, baked into the wasm and kept from being stripped.
    // ------------------------------------------------------------------------
    __attribute__((used)) const char g_eka2l1_copyright[] =
        "EKA2L1-WEB powered by zixing (QQ:1311817771)\n"
        "Symbian \xE6\x98\xAF\xE8\xAF\xBA\xE5\x9F\xBA\xE4\xBA\x9A\xE5\xA1\x9E"
        "\xE7\x8F\xAD\xE7\xB3\xBB\xE7\xBB\x9F\xE7\x9A\x84\xE6\xB3\xA8\xE5\x86\x8C"
        "\xE5\x95\x86\xE6\xA0\x87\xEF\xBC\x8C\xE5\x85\xB6\xE7\x9B\xB8\xE5\x85\xB3"
        "\xE5\x95\x86\xE6\xA0\x87\xE6\x9D\x83\xE5\xBD\x92 Nokia \xE6\x89\x80\xE6\x9C\x89\xE3\x80\x82";

    // ------------------------------------------------------------------------
    // Embedded integrity table. Filled in post-build by gen_integrity.py, which
    // scans the .wasm for the magic and overwrites the bytes that follow. The
    // layout is all byte-arrays + 4-byte ints at 4-aligned offsets, so it is
    // padding-free and the Python side can mirror it exactly.
    // ------------------------------------------------------------------------
    struct integ_entry {
        char name[24];
        std::uint8_t sha256[32];
    };
    struct integ_table {
        char magic[16];
        std::uint32_t version;
        std::uint32_t count;
        integ_entry entries[8];
    };

    static_assert(sizeof(integ_entry) == 56, "integ_entry must be padding-free");
    static_assert(sizeof(integ_table) == 24 + 8 * 56, "integ_table layout drift");

    // CRITICAL: every byte of the table must be non-zero in the initializer.
    // wasm-ld trims a data segment's trailing zeros (and splits large zero
    // gaps) out of the binary, so a zero-initialized entries area is NOT
    // materialized — gen_integrity.py would then patch bytes that belong to the
    // next data segment, producing a module that fails to parse
    // ("unknown init_expr opcode 0"). The 0xCC filler keeps the whole 472-byte
    // blob contiguous and patchable; gen_integrity overwrites version/count and
    // the used entries, unused bytes keep the filler.
#define EKA2L1_PC8 '\xCC', '\xCC', '\xCC', '\xCC', '\xCC', '\xCC', '\xCC', '\xCC'
#define EKA2L1_PC24 EKA2L1_PC8, EKA2L1_PC8, EKA2L1_PC8
#define EKA2L1_PB8 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC
#define EKA2L1_PB32 EKA2L1_PB8, EKA2L1_PB8, EKA2L1_PB8, EKA2L1_PB8
#define EKA2L1_PENTRY { { EKA2L1_PC24 }, { EKA2L1_PB32 } }

    __attribute__((used)) volatile integ_table g_integ = {
        // 14 chars + NUL; gen_integrity.py searches for "EKA2L1INTEGTBL\0".
        { 'E', 'K', 'A', '2', 'L', '1', 'I', 'N', 'T', 'E', 'G', 'T', 'B', 'L', 0, 0 },
        1,
        0xCCCCCCCCu, // count placeholder (gen_integrity sets the real count)
        { EKA2L1_PENTRY, EKA2L1_PENTRY, EKA2L1_PENTRY, EKA2L1_PENTRY,
          EKA2L1_PENTRY, EKA2L1_PENTRY, EKA2L1_PENTRY, EKA2L1_PENTRY }
    };

#undef EKA2L1_PENTRY
#undef EKA2L1_PB32
#undef EKA2L1_PB8
#undef EKA2L1_PC24
#undef EKA2L1_PC8

    // ------------------------------------------------------------------------
    // Runtime protection state (release only).
    // ------------------------------------------------------------------------
    enum status_bits {
        prot_ok = 0,
        prot_domain = 1,
        prot_asset = 2,
        prot_wasm = 4,
        prot_incomplete = 8
    };

    static bool g_domain_ok = false;
    static bool g_asset_failed = false;
    static bool g_wasm_failed = false;
    static std::uint32_t g_asset_pass_mask = 0;

    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static bool host_in_whitelist(const std::string &host_in) {
        const std::string host = to_lower(host_in);
        static const char *roots[] = { "zixing.fun", "iniche.cn" };
        for (const char *root : roots) {
            const std::string r(root);
            if (host == r) {
                return true;
            }
            // "*.root": any subdomain, e.g. test.zixing.fun, www.zixing.fun.
            if (host.size() > r.size() + 1) {
                const std::string suffix = "." + r;
                if (host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    return true;
                }
            }
        }
        return false;
    }

    std::string copyright_text() {
        return std::string(const_cast<const char *>(g_eka2l1_copyright));
    }

    std::string build_info() {
        return std::string("EKA2L1-WEB\nBuild: ") + BUILD_TIMESTAMP +
            "\nCommit: " + GIT_COMMIT_HASH +
            "\nChannel: " + EKA2L1_BUILD_CHANNEL;
    }

    std::string watermark_text() {
        // Three short lines (Build / Commit / Channel) so it fits the corner on
        // narrow screens instead of overflowing as one clipped line.
        return std::string("Build: ") + BUILD_TIMESTAMP +
            "\nCommit: " + GIT_COMMIT_HASH +
            "\nChannel: " + EKA2L1_BUILD_CHANNEL;
    }

    bool is_blocked() {
#ifdef EKA2L1_DEBUG_BUILD
        return false;
#else
        if (!g_domain_ok || g_asset_failed || g_wasm_failed) {
            return true;
        }
        const std::uint32_t count = g_integ.count;
        const std::uint32_t required = (count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u);
        return (g_asset_pass_mask & required) != required;
#endif
    }

    // ------------------------------------------------------------------------
    // 8x8 bitmap font (public-domain font8x8_basic), ASCII 0x20..0x7F. Each
    // glyph is 8 rows; bit 0 (value 1) is the leftmost column.
    // ------------------------------------------------------------------------
    static const std::uint8_t k_font8x8[96][8] = {
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x20 ' '
        { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00 }, // '!'
        { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '"'
        { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00 }, // '#'
        { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00 }, // '$'
        { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00 }, // '%'
        { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00 }, // '&'
        { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '\''
        { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00 }, // '('
        { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00 }, // ')'
        { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 }, // '*'
        { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00 }, // '+'
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // ','
        { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00 }, // '-'
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // '.'
        { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00 }, // '/'
        { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00 }, // '0'
        { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00 }, // '1'
        { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00 }, // '2'
        { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00 }, // '3'
        { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00 }, // '4'
        { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00 }, // '5'
        { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00 }, // '6'
        { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00 }, // '7'
        { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00 }, // '8'
        { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00 }, // '9'
        { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00 }, // ':'
        { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06 }, // ';'
        { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00 }, // '<'
        { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00 }, // '='
        { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00 }, // '>'
        { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00 }, // '?'
        { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00 }, // '@'
        { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00 }, // 'A'
        { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00 }, // 'B'
        { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00 }, // 'C'
        { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00 }, // 'D'
        { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00 }, // 'E'
        { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00 }, // 'F'
        { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00 }, // 'G'
        { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00 }, // 'H'
        { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 'I'
        { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00 }, // 'J'
        { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00 }, // 'K'
        { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00 }, // 'L'
        { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00 }, // 'M'
        { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00 }, // 'N'
        { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00 }, // 'O'
        { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00 }, // 'P'
        { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00 }, // 'Q'
        { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00 }, // 'R'
        { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00 }, // 'S'
        { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 'T'
        { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00 }, // 'U'
        { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // 'V'
        { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 }, // 'W'
        { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00 }, // 'X'
        { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00 }, // 'Y'
        { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00 }, // 'Z'
        { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00 }, // '['
        { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00 }, // '\\'
        { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00 }, // ']'
        { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00 }, // '^'
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF }, // '_'
        { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '`'
        { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00 }, // 'a'
        { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00 }, // 'b'
        { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00 }, // 'c'
        { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00 }, // 'd'
        { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00 }, // 'e'
        { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00 }, // 'f'
        { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // 'g'
        { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00 }, // 'h'
        { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 'i'
        { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E }, // 'j'
        { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00 }, // 'k'
        { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00 }, // 'l'
        { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00 }, // 'm'
        { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00 }, // 'n'
        { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00 }, // 'o'
        { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F }, // 'p'
        { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78 }, // 'q'
        { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00 }, // 'r'
        { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00 }, // 's'
        { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00 }, // 't'
        { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00 }, // 'u'
        { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00 }, // 'v'
        { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00 }, // 'w'
        { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00 }, // 'x'
        { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F }, // 'y'
        { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00 }, // 'z'
        { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00 }, // '{'
        { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00 }, // '|'
        { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00 }, // '}'
        { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, // '~'
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }  // 0x7F
    };

    bool render_text_rgba(const std::string &text, int scale,
        std::vector<std::uint8_t> &out, int &width, int &height) {
        if (text.empty() || scale < 1) {
            return false;
        }

        // Split on '\n' into lines; the texture is sized to the widest line and
        // the line count so multi-line watermarks render in a tidy block.
        std::vector<std::string> lines;
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        lines.push_back(cur);

        std::size_t max_len = 0;
        for (const std::string &l : lines) {
            max_len = std::max(max_len, l.size());
        }
        if (max_len == 0) {
            return false;
        }

        width = static_cast<int>(max_len) * 8 * scale;
        height = static_cast<int>(lines.size()) * 8 * scale;
        out.assign(static_cast<std::size_t>(width) * height * 4, 0);

        // Translucent white text; the caller blits this over the screen.
        const std::uint8_t alpha = 170;

        for (std::size_t li = 0; li < lines.size(); li++) {
            const std::string &line = lines[li];
            const int line_y = static_cast<int>(li) * 8 * scale;

            for (std::size_t ci = 0; ci < line.size(); ci++) {
                unsigned char ch = static_cast<unsigned char>(line[ci]);
                if (ch < 0x20 || ch > 0x7F) {
                    ch = '?';
                }
                const std::uint8_t *glyph = k_font8x8[ch - 0x20];
                const int char_x = static_cast<int>(ci) * 8 * scale;

                for (int row = 0; row < 8; row++) {
                    for (int col = 0; col < 8; col++) {
                        if (!((glyph[row] >> col) & 1)) {
                            continue;
                        }
                        // Expand the 1x1 source pixel to scale x scale.
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                const int px = char_x + col * scale + sx;
                                const int py = line_y + row * scale + sy;
                                const std::size_t idx =
                                    (static_cast<std::size_t>(py) * width + px) * 4;
                                out[idx + 0] = 255;
                                out[idx + 1] = 255;
                                out[idx + 2] = 255;
                                out[idx + 3] = alpha;
                            }
                        }
                    }
                }
            }
        }
        return true;
    }
}

// ============================================================================
// C API exported to JavaScript. JS only fetches bytes / reads location.hostname;
// every comparison happens here in the wasm.
// ============================================================================
extern "C" {

// Reusable scratch buffer the JS side fills (HEAPU8.set) before calling
// wasm_verify_asset / wasm_verify_wasm, so no malloc export is needed and big
// files never round-trip through ccall's stack-allocated 'array' marshalling.
static std::vector<std::uint8_t> g_verify_buf;

EMSCRIPTEN_KEEPALIVE
std::uint8_t *wasm_protect_buffer(int size) {
    if (size <= 0) {
        return nullptr;
    }
    if (g_verify_buf.size() < static_cast<std::size_t>(size)) {
        g_verify_buf.resize(static_cast<std::size_t>(size));
    }
    return g_verify_buf.data();
}

EMSCRIPTEN_KEEPALIVE
void wasm_protect_buffer_free() {
    g_verify_buf.clear();
    g_verify_buf.shrink_to_fit();
}

EMSCRIPTEN_KEEPALIVE
const char *wasm_get_copyright() {
    static std::string s = eka2l1::web::protection::copyright_text();
    return s.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char *wasm_get_build_info() {
    static std::string s = eka2l1::web::protection::build_info();
    return s.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char *wasm_watermark_text() {
    static std::string s = eka2l1::web::protection::watermark_text();
    return s.c_str();
}

EMSCRIPTEN_KEEPALIVE
int wasm_check_domain(const char *host) {
#ifdef EKA2L1_DEBUG_BUILD
    (void)host;
    return 0;
#else
    using namespace eka2l1::web::protection;
    if (host && host_in_whitelist(host)) {
        g_domain_ok = true;
        return 0;
    }
    g_domain_ok = false;
    return -1;
#endif
}

EMSCRIPTEN_KEEPALIVE
int wasm_verify_asset(int name_id, const std::uint8_t *ptr, int len) {
#ifdef EKA2L1_DEBUG_BUILD
    (void)name_id;
    (void)ptr;
    (void)len;
    return 0;
#else
    using namespace eka2l1::web::protection;
    if (!ptr || len < 0 || name_id < 0 ||
        name_id >= static_cast<int>(g_integ.count) || name_id >= 8) {
        g_asset_failed = true;
        return -1;
    }

    const std::array<std::uint8_t, 32> digest =
        eka2l1::crypt::sha256(ptr, static_cast<std::size_t>(len));

    // g_integ is volatile (post-build patched); copy the expected bytes out.
    std::array<std::uint8_t, 32> expected{};
    for (int i = 0; i < 32; i++) {
        expected[i] = g_integ.entries[name_id].sha256[i];
    }

    if (digest == expected) {
        g_asset_pass_mask |= (1u << name_id);
        return 0;
    }
    g_asset_failed = true;
    return -1;
#endif
}

EMSCRIPTEN_KEEPALIVE
int wasm_verify_wasm(const std::uint8_t *ptr, int len, const char *expected_hex) {
#ifdef EKA2L1_DEBUG_BUILD
    (void)ptr;
    (void)len;
    (void)expected_hex;
    return 0;
#else
    using namespace eka2l1::web::protection;
    if (!ptr || len < 0 || !expected_hex) {
        g_wasm_failed = true;
        return -1;
    }
    const std::string got = eka2l1::crypt::sha256_hex(ptr, static_cast<std::size_t>(len));
    std::string want = expected_hex;
    std::transform(want.begin(), want.end(), want.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (got == want && !want.empty()) {
        return 0;
    }
    g_wasm_failed = true;
    return -1;
#endif
}

EMSCRIPTEN_KEEPALIVE
int wasm_protection_status() {
#ifdef EKA2L1_DEBUG_BUILD
    return 0;
#else
    using namespace eka2l1::web::protection;
    int s = prot_ok;
    if (!g_domain_ok) {
        s |= prot_domain;
    }
    if (g_asset_failed) {
        s |= prot_asset;
    }
    if (g_wasm_failed) {
        s |= prot_wasm;
    }
    const std::uint32_t count = g_integ.count;
    const std::uint32_t required = (count >= 32) ? 0xFFFFFFFFu : ((1u << count) - 1u);
    if ((g_asset_pass_mask & required) != required) {
        s |= prot_incomplete;
    }
    return s;
#endif
}

EMSCRIPTEN_KEEPALIVE
int wasm_is_blocked() {
    return eka2l1::web::protection::is_blocked() ? 1 : 0;
}

} // extern "C"
