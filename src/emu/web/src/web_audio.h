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

#pragma once

#include <drivers/audio/audio.h>

#include <memory>
#include <mutex>
#include <vector>

namespace eka2l1::drivers {
    class web_audio_output_stream;

    /**
     * Audio driver for the Emscripten/browser build.
     *
     * SDL2's Emscripten audio backend resamples on the browser main thread
     * inside a ScriptProcessorNode callback and froze the page, so this driver
     * goes to Web Audio directly: the frontend's main loop calls pump() each
     * frame, which pulls PCM from every playing stream (on the same thread the
     * kernel runs on — no cross-thread races) and schedules it as AudioBuffer
     * chunks. The browser does all resampling and mixing.
     */
    class web_audio_driver : public audio_driver {
        std::mutex streams_lock_;
        std::vector<web_audio_output_stream *> streams_;

    public:
        explicit web_audio_driver(const std::uint32_t initial_master_vol = 100,
            const player_type preferred_midi_backend = player_type_tsf);

        std::unique_ptr<audio_output_stream> new_output_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback callback) override;

        std::unique_ptr<audio_input_stream> new_input_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback callback) override;

        std::uint32_t native_sample_rate() override;

        /// Called by the frontend main loop (browser main thread) every frame.
        void pump();

        void unregister_stream(web_audio_output_stream *stream);
    };
}
