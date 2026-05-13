/*
 * Copyright (c) 2018 EKA2L1 Team.
 *
 * This file is part of EKA2L1 project
 * (see bentokun.github.com/EKA2L1).
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

#include <common/cvt.h>
#include <common/dynamicfile.h>
#include <common/fileutils.h>   // open_c_file

#include <cstring>

namespace eka2l1::common {

    dynamic_ifile::dynamic_ifile(const std::string &name)
        : fp_(nullptr), ucs2_(-1), eof_(false), fail_(false) {
        fp_ = common::open_c_file(name, "rb");
        if (!fp_) {
            fail_ = true;
            return;
        }

        // Detect BOM to determine encoding.
        std::uint16_t bom = 0;
        if (fread(&bom, 1, 2, fp_) != 2) {
            // Very short file – treat as UTF-8 and rewind.
            fseek(fp_, 0, SEEK_SET);
            return;
        }

        if (bom == 0xFEFF) {
            ucs2_ = 0; // UCS-2 LE
        } else if (bom == 0xFFFE) {
            ucs2_ = 1; // UCS-2 BE
        } else {
            ucs2_ = -1; // UTF-8 / plain ASCII
            fseek(fp_, 0, SEEK_SET);
        }
    }

    // ---- Internal helpers ----

    static int file_read_byte(FILE *fp, bool &eof_out) {
        int c = fgetc(fp);
        if (c == EOF) { eof_out = true; return -1; }
        return c;
    }

    // Read one UCS-2 code unit (2 bytes) according to endianness.
    static bool read_u16(FILE *fp, int ucs2, char16_t &ch) {
        int lo = fgetc(fp);
        int hi = fgetc(fp);
        if (lo == EOF || hi == EOF) return false;
        if (ucs2 == 1) // BE
            ch = static_cast<char16_t>((lo << 8) | hi);
        else            // LE
            ch = static_cast<char16_t>((hi << 8) | lo);
        return true;
    }

    // ---- getline (UTF-8) ----

    bool dynamic_ifile::getline(std::string &line) {
        if (fail_ || !fp_) return false;

        if (is_ucs2()) {
            std::u16string u16;
            bool ok = getline(u16);
            line = common::ucs2_to_utf8(u16);
            return ok;
        }

        line.clear();
        int c;
        bool got_any = false;
        while ((c = fgetc(fp_)) != EOF) {
            got_any = true;
            if (c == '\n') break;
            if (c != '\r') line += static_cast<char>(c);
        }
        if (c == EOF) eof_ = true;
        return got_any || !eof_;
    }

    // ---- getline (UCS-2) ----

    bool dynamic_ifile::getline(std::u16string &line) {
        if (fail_ || !fp_) return false;

        if (ucs2_ == -1) {
            std::string utf8;
            bool ok = getline(utf8);
            line = common::utf8_to_ucs2(utf8);
            return ok;
        }

        line.clear();
        char16_t ch;
        bool got_any = false;
        while (read_u16(fp_, ucs2_, ch)) {
            got_any = true;
            if (ch == u'\n') break;
            if (ch != u'\r') line.push_back(ch);
        }
        if (feof(fp_)) eof_ = true;
        return got_any || !eof_;
    }

    // ---- read(string, len) ----

    void dynamic_ifile::read(std::string &line, const std::size_t len) {
        if (fail_ || !fp_) return;

        if (ucs2_ < 0) {
            line.resize(len);
            std::size_t n = fread(&line[0], 1, len, fp_);
            line.resize(n);
            if (feof(fp_)) eof_ = true;
            return;
        }

        std::u16string u16;
        read(u16, len);
        line = common::ucs2_to_utf8(u16);
    }

    // ---- read(u16string, len) ----

    void dynamic_ifile::read(std::u16string &line, const std::size_t len) {
        if (fail_ || !fp_) return;

        if (ucs2_ < 0) {
            std::string tmp(len, '\0');
            std::size_t n = fread(&tmp[0], 1, len, fp_);
            tmp.resize(n);
            line = common::utf8_to_ucs2(tmp);
            if (feof(fp_)) eof_ = true;
            return;
        }

        line.clear();
        for (std::size_t i = 0; i < len; ++i) {
            char16_t ch;
            if (!read_u16(fp_, ucs2_, ch)) { eof_ = true; break; }
            if (ch == u'\n') break;
            line.push_back(ch);
        }
    }

    // ---- seek ----

    void dynamic_ifile::seek(int mode, const std::size_t offset) {
        if (!fp_) return;
        int whence = SEEK_SET;
        if (mode == 1) whence = SEEK_CUR;
        if (mode == 2) whence = SEEK_END;

        long off = static_cast<long>(offset);
        if (ucs2_ >= 0) {
            // Skip 2-byte BOM, each character is 2 bytes.
            fseek(fp_, 2 + off * 2, whence);
        } else {
            fseek(fp_, off, whence);
        }
        eof_ = false;
    }

} // namespace eka2l1::common
