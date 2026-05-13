/*
 * Copyright (c) 2024 EKA2L1 Team.
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

namespace eka2l1::drivers {
    struct null_audio_driver : public audio_driver {
        explicit null_audio_driver(const std::uint32_t initial_master_volume = 100,
            const player_type preferred_midi_backend = player_type_tsf)
            : audio_driver(initial_master_volume, preferred_midi_backend) {}

        std::unique_ptr<audio_output_stream> new_output_stream(const std::uint32_t,
            const std::uint8_t, data_callback) override {
            return nullptr;
        }

        std::unique_ptr<audio_input_stream> new_input_stream(const std::uint32_t,
            const std::uint8_t, data_callback) override {
            return nullptr;
        }

        std::uint32_t native_sample_rate() override {
            return 44100;
        }
    };
}
