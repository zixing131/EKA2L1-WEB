/*
 * Copyright (c) 2024 EKA2L1 Team.
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

#pragma once

#include "napi/native_api.h"

#include <string>
#include <vector>

namespace eka2l1::hos::napiutil {
    // Read a JS string argument into a std::string (UTF-8).
    inline std::string get_string(napi_env env, napi_value value) {
        size_t len = 0;
        napi_get_value_string_utf8(env, value, nullptr, 0, &len);
        std::string out(len, '\0');
        size_t written = 0;
        napi_get_value_string_utf8(env, value, out.data(), len + 1, &written);
        out.resize(written);
        return out;
    }

    inline int32_t get_int(napi_env env, napi_value value) {
        int32_t out = 0;
        napi_get_value_int32(env, value, &out);
        return out;
    }

    inline int64_t get_int64(napi_env env, napi_value value) {
        int64_t out = 0;
        napi_get_value_int64(env, value, &out);
        return out;
    }

    inline double get_double(napi_env env, napi_value value) {
        double out = 0.0;
        napi_get_value_double(env, value, &out);
        return out;
    }

    inline bool get_bool(napi_env env, napi_value value) {
        bool out = false;
        napi_get_value_bool(env, value, &out);
        return out;
    }

    inline napi_value make_string(napi_env env, const std::string &str) {
        napi_value out = nullptr;
        napi_create_string_utf8(env, str.c_str(), str.size(), &out);
        return out;
    }

    inline napi_value make_int(napi_env env, int32_t value) {
        napi_value out = nullptr;
        napi_create_int32(env, value, &out);
        return out;
    }

    inline napi_value make_double(napi_env env, double value) {
        napi_value out = nullptr;
        napi_create_double(env, value, &out);
        return out;
    }

    inline napi_value make_bool(napi_env env, bool value) {
        napi_value out = nullptr;
        napi_get_boolean(env, value, &out);
        return out;
    }

    inline napi_value make_undefined(napi_env env) {
        napi_value out = nullptr;
        napi_get_undefined(env, &out);
        return out;
    }

    // Convert a vector of UTF-8 strings into a JS string[].
    inline napi_value make_string_array(napi_env env, const std::vector<std::string> &strings) {
        napi_value arr = nullptr;
        napi_create_array_with_length(env, strings.size(), &arr);
        for (size_t i = 0; i < strings.size(); i++) {
            napi_set_element(env, arr, static_cast<uint32_t>(i), make_string(env, strings[i]));
        }
        return arr;
    }
}
