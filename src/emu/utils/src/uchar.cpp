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

#include <cctype>
#include <cwctype>
#include <locale>
#include <utils/uchar.h>

namespace eka2l1::epoc {

    // C-locale helpers — used when ln is nullptr (e.g. Emscripten/WASM where
    // std::locale construction is unavailable).
    static std::uint32_t get_uchar_category_c(const uchar c) {
        const wchar_t wc = static_cast<wchar_t>(c);
        if (std::iswcntrl(wc)) return uchar_category::UCHAR_CATEGORY_OTHER_CONTROL;
        if (std::iswpunct(wc)) return uchar_category::UCHAR_CATEGORY_PUNCTUATION_GROUP;
        if (std::iswalpha(wc)) {
            return std::iswlower(wc)
                ? uchar_category::UCHAR_CATEGORY_LETTER_LOWERCASE
                : uchar_category::UCHAR_CATEGORY_LETTER_UPPERCASE;
        }
        if (std::iswspace(wc)) return uchar_category::UCHAR_CATEGORY_SEPARATOR_SPACE;
        if (std::iswdigit(wc)) return uchar_category::UCHAR_CATEGORY_NUMBER_DECIMAL_DIGIT;
        return uchar_category::UCHAR_CATEGORY_OTHER_NOT_ASSIGNED;
    }

    std::uint32_t get_uchar_category(const uchar c, std::locale *ln) {
        if (!ln) return get_uchar_category_c(c);
        const wchar_t wc = static_cast<wchar_t>(c);
        if (std::iscntrl(wc, *ln)) return uchar_category::UCHAR_CATEGORY_OTHER_CONTROL;
        if (std::ispunct(wc, *ln)) return uchar_category::UCHAR_CATEGORY_PUNCTUATION_GROUP;
        if (std::isalpha(wc, *ln)) {
            return std::islower(wc, *ln)
                ? uchar_category::UCHAR_CATEGORY_LETTER_LOWERCASE
                : uchar_category::UCHAR_CATEGORY_LETTER_UPPERCASE;
        }
        if (std::isspace(wc, *ln)) return uchar_category::UCHAR_CATEGORY_SEPARATOR_SPACE;
        if (std::isdigit(wc, *ln)) return uchar_category::UCHAR_CATEGORY_NUMBER_DECIMAL_DIGIT;
        return uchar_category::UCHAR_CATEGORY_OTHER_NOT_ASSIGNED;
    }

    const uchar uppercase_uchar(const uchar c, std::locale *ln) {
        if (!ln) return static_cast<uchar>(std::towupper(static_cast<wchar_t>(c)));
        return std::toupper(static_cast<wchar_t>(c), *ln);
    }

    const uchar lowercase_uchar(const uchar c, std::locale *ln) {
        if (!ln) return static_cast<uchar>(std::towlower(static_cast<wchar_t>(c)));
        return std::tolower(static_cast<wchar_t>(c), *ln);
    }

    const uchar fold_uchar(const uchar c, std::locale *ln) {
        return uppercase_uchar(c, ln);
    }

} // namespace eka2l1::epoc
