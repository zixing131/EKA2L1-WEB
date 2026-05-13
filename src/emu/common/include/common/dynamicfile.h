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

#pragma once

#include <cstdio>
#include <string>

namespace eka2l1::common {
    /**
     * \brief A file reader that supports UTF-8 and UCS-2 encoded text files.
     *
     * Reimplemented on top of C-style FILE* to avoid std::ifstream locale
     * issues in environments like Emscripten/WASM where std::basic_filebuf
     * internals may call unavailable locale facets and abort.
     */
    class dynamic_ifile {
        FILE *fp_;
        int   ucs2_;  // -1 = UTF-8, 0 = UCS-2 LE, 1 = UCS-2 BE
        bool  eof_;
        bool  fail_;

    public:
        explicit dynamic_ifile(const std::string &name);

        // Allow move-assignment (used when retrying with another filename).
        dynamic_ifile(dynamic_ifile &&o) noexcept
            : fp_(o.fp_), ucs2_(o.ucs2_), eof_(o.eof_), fail_(o.fail_) {
            o.fp_ = nullptr;
        }
        dynamic_ifile &operator=(dynamic_ifile &&o) noexcept {
            if (this != &o) {
                if (fp_) fclose(fp_);
                fp_   = o.fp_;
                ucs2_ = o.ucs2_;
                eof_  = o.eof_;
                fail_ = o.fail_;
                o.fp_ = nullptr;
            }
            return *this;
        }

        dynamic_ifile(const dynamic_ifile &) = delete;
        dynamic_ifile &operator=(const dynamic_ifile &) = delete;

        ~dynamic_ifile() {
            if (fp_) { fclose(fp_); fp_ = nullptr; }
        }

        bool getline(std::string &line);
        bool getline(std::u16string &line);

        void read(std::string &line, const std::size_t len);
        void read(std::u16string &line, const std::size_t len);

        void seek(int mode, const std::size_t offset);

        bool eof()  const { return eof_  || (fp_ && feof(fp_)); }
        bool fail() const { return fail_ || !fp_; }

        void set_ucs2(int ucs2) { ucs2_ = ucs2; }
        bool is_ucs2() const    { return ucs2_ != -1; }
    };
}
