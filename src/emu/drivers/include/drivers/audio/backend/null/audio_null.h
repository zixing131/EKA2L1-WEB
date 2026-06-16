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
#include <drivers/audio/stream.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace eka2l1::drivers {
    // A silent, self-pacing output stream. It owns a tiny pump thread that calls
    // the data callback on a ~20ms cadence and discards the produced samples.
    // This is what makes a backend-less platform (OHOS) progress the guest MMF
    // pipeline: the higher dsp_output_stream_shared layer relies on its data
    // callback being pulled to mark buffers "played". Without a paced pull, apps
    // that start audio stall forever (black screen).
    struct null_audio_output_stream : public audio_output_stream {
        struct pump_state {
            std::atomic_bool stop{ false };
            std::atomic_bool playing{ false };
            data_callback cb;
            std::uint32_t sample_rate = 8000;
            std::uint8_t channels = 1;
            std::atomic<std::uint64_t> frames{ 0 };
        };

        std::shared_ptr<pump_state> pump_;
        float volume_ = 1.0f;

        explicit null_audio_output_stream(audio_driver *driver, const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback callback)
            : audio_output_stream(driver, sample_rate, channels)
            , pump_(std::make_shared<pump_state>()) {
            pump_->cb = callback;
            pump_->sample_rate = (sample_rate == 0) ? 8000 : sample_rate;
            pump_->channels = (channels == 0) ? 1 : channels;

            std::shared_ptr<pump_state> st = pump_;
            std::thread([st]() {
                // ~20ms worth of frames per tick.
                const std::size_t frames_per_tick = static_cast<std::size_t>(st->sample_rate) / 50;
                std::vector<std::int16_t> scratch(
                    (frames_per_tick == 0 ? 1 : frames_per_tick) * st->channels, 0);

                while (!st->stop.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    if (!st->playing.load(std::memory_order_acquire)) {
                        continue;
                    }
                    if (st->cb) {
                        const std::size_t produced = st->cb(scratch.data(),
                            frames_per_tick == 0 ? 1 : frames_per_tick);
                        st->frames.fetch_add(produced, std::memory_order_acq_rel);
                    }
                }
            }).detach();
        }

        ~null_audio_output_stream() override {
            pump_->stop.store(true, std::memory_order_release);
        }

        bool start() override {
            pump_->playing.store(true, std::memory_order_release);
            return true;
        }

        bool stop() override {
            pump_->playing.store(false, std::memory_order_release);
            return true;
        }

        void pause() override {
            pump_->playing.store(false, std::memory_order_release);
        }

        bool is_playing() override {
            return pump_->playing.load(std::memory_order_acquire);
        }

        bool is_pausing() override {
            return !pump_->playing.load(std::memory_order_acquire);
        }

        bool set_volume(const float volume) override {
            volume_ = volume;
            return true;
        }

        float get_volume() const override {
            return volume_;
        }

        bool current_frame_position(std::uint64_t *pos) override {
            if (pos) {
                *pos = pump_->frames.load(std::memory_order_acquire);
            }
            return true;
        }
    };

    struct null_audio_input_stream : public audio_input_stream {
        std::atomic_bool recording_{ false };

        explicit null_audio_input_stream(audio_driver *driver, const std::uint32_t sample_rate,
            const std::uint8_t channels)
            : audio_input_stream(driver, sample_rate, channels) {}

        bool start() override {
            recording_.store(true);
            return true;
        }

        bool stop() override {
            recording_.store(false);
            return true;
        }

        bool is_recording() override {
            return recording_.load();
        }

        bool current_frame_position(std::uint64_t *pos) override {
            if (pos) {
                *pos = 0;
            }
            return true;
        }
    };

    struct null_audio_driver : public audio_driver {
        explicit null_audio_driver(const std::uint32_t initial_master_volume = 100,
            const player_type preferred_midi_backend = player_type_tsf)
            : audio_driver(initial_master_volume, preferred_midi_backend) {}

        std::unique_ptr<audio_output_stream> new_output_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback callback) override {
            return std::make_unique<null_audio_output_stream>(this, sample_rate, channels, callback);
        }

        std::unique_ptr<audio_input_stream> new_input_stream(const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback) override {
            return std::make_unique<null_audio_input_stream>(this, sample_rate, channels);
        }

        std::uint32_t native_sample_rate() override {
            return 44100;
        }
    };
}
