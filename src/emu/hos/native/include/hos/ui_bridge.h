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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace eka2l1::hos {
    // A decoded app icon as straight RGBA8888 (premultiplied), ready for the NAPI
    // layer to wrap into an ArkUI PixelMap. Replaces the JNI Bitmap[] the Android
    // launcher returned. `mask` is optional (empty when the icon has its own alpha).
    struct icon_bitmap {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba;     // size = width*height*4

        std::uint32_t mask_width = 0;
        std::uint32_t mask_height = 0;
        std::vector<std::uint8_t> mask_rgba;

        bool valid() const {
            return (width != 0) && (height != 0) && !rgba.empty();
        }
    };

    // Host UI callbacks. The Android frontend reached into Java via JNI for these;
    // on OHOS the NAPI bridge registers ArkTS-backed implementations. All are
    // optional - if unset the launcher silently no-ops (so the OS thread never
    // blocks waiting on a UI that isn't wired yet).
    struct ui_callbacks {
        // The active Symbian app process exited (normal close or guest panic).
        std::function<void()> on_app_exit;

        // Show a text input dialog (initial text + max length). The frontend must
        // eventually call launcher::on_finished_text_input with the result.
        std::function<void(const std::string & /*initial*/, int /*max_len*/)> show_input_dialog;

        // Dismiss the text input dialog if shown.
        std::function<void()> close_input_dialog;

        // Show a yes/no question dialog. The frontend must eventually call
        // launcher::on_question_dialog_finished with the chosen button index.
        std::function<void(const std::string & /*text*/, const std::string & /*yes*/,
            const std::string & /*no*/)> show_question_dialog;
    };

    // Process-wide UI callbacks, set by the NAPI bridge during init.
    ui_callbacks &get_ui_callbacks();
}
