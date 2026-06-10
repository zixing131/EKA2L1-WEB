/*
 * Copyright (c) 2020 EKA2L1 Team.
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

#include <common/platform.h>
#include <drivers/audio/dsp.h>
#if !EKA2L1_PLATFORM(WASM)
#include <drivers/audio/backend/ffmpeg/dsp_ffmpeg.h>
#endif

#include <common/log.h>

namespace eka2l1::drivers {
    dsp_stream::dsp_stream()
        : samples_played_(0)
        , samples_copied_(0)
        , freq_(0)
        , channels_(0)
        , complete_callback_(nullptr)
        , more_buffer_callback_(nullptr)
        , complete_userdata_(nullptr)
        , more_buffer_userdata_(nullptr) {
    }

    dsp_output_stream::dsp_output_stream()
        : dsp_stream()
        , volume_(10) {
    }

    void dsp_stream::reset_stat() {
        samples_played_ = 0;
        samples_copied_ = 0;
    }

    void dsp_stream::register_callback(dsp_stream_notification_type nof_type, dsp_stream_notification_callback callback,
        void *userdata) {
        const std::lock_guard<std::mutex> guard(callback_lock_);

        switch (nof_type) {
        case dsp_stream_notification_done:
            complete_callback_ = callback;
            complete_userdata_ = userdata;
            break;

        case dsp_stream_notification_more_buffer:
            more_buffer_userdata_ = userdata;
            more_buffer_callback_ = callback;
            break;

        default:
            LOG_ERROR(DRIVER_AUD, "Unsupport notification type!");
            break;
        }
    }

    void *dsp_stream::get_userdata(dsp_stream_notification_type nof_type) {
        switch (nof_type) {
        case dsp_stream_notification_done:
            return complete_userdata_;

        case dsp_stream_notification_more_buffer:
            return more_buffer_userdata_;

        default:
            LOG_ERROR(DRIVER_AUD, "Unsupport notification type!");
            break;
        }

        return nullptr;
    }


#if EKA2L1_PLATFORM(WASM)
    // WASM has no audio backend (ffmpeg is compiled out), so the factories used
    // to return nullptr. The MMF device server dereferences the returned stream
    // unconditionally, which traps. These no-op streams accept all data and
    // report success without producing sound, keeping audio-using apps alive.
    namespace {
        struct dsp_output_stream_null : public dsp_output_stream {
            bool playing_ = false;

            std::uint64_t position() override { return 0; }
            std::uint64_t real_time_position() override { return 0; }

            bool set_properties(const std::uint32_t freq, const std::uint8_t channels) override {
                freq_ = freq;
                channels_ = channels;
                return true;
            }

            void get_supported_formats(std::vector<four_cc> &cc_list) override {
                cc_list.push_back(PCM16_FOUR_CC_CODE);
            }

            bool start() override {
                playing_ = true;
                return true;
            }

            bool stop() override {
                playing_ = false;
                return true;
            }

            bool is_playing() const override { return playing_; }

            bool write(const std::uint8_t *data, const std::uint32_t data_size) override {
                const std::uint32_t frame_size = channels_ ? (channels_ * static_cast<std::uint32_t>(sizeof(std::uint16_t))) : 1;
                samples_copied_ += data_size / frame_size;
                samples_played_ = samples_copied_;
                return true;
            }
        };

        struct dsp_input_stream_null : public dsp_input_stream {
            bool recording_ = false;

            std::uint64_t position() override { return 0; }
            std::uint64_t real_time_position() override { return 0; }

            bool set_properties(const std::uint32_t freq, const std::uint8_t channels) override {
                freq_ = freq;
                channels_ = channels;
                return true;
            }

            void get_supported_formats(std::vector<four_cc> &cc_list) override {
                cc_list.push_back(PCM16_FOUR_CC_CODE);
            }

            bool start() override {
                recording_ = true;
                return true;
            }

            bool stop() override {
                recording_ = false;
                return true;
            }

            bool is_recording() const override { return recording_; }

            bool read(std::uint8_t *data, const std::uint32_t max_data_size) override {
                return false;
            }
        };
    }
#endif

    std::unique_ptr<dsp_stream> new_dsp_out_stream(drivers::audio_driver *aud, const dsp_stream_backend dsp_backend) {
        switch (dsp_backend) {
        case dsp_stream_backend_ffmpeg:
#if EKA2L1_PLATFORM(WASM)
            (void)aud;
            return std::make_unique<dsp_output_stream_null>();
#else
            return std::make_unique<dsp_output_stream_ffmpeg>(aud);
#endif
        default:
            break;
        }

        return nullptr;
    }

    std::unique_ptr<dsp_stream> new_dsp_in_stream(drivers::audio_driver *aud, const dsp_stream_backend dsp_backend) {
        switch (dsp_backend) {
        case dsp_stream_backend_ffmpeg:
#if EKA2L1_PLATFORM(WASM)
            (void)aud;
            return std::make_unique<dsp_input_stream_null>();
#else
            return std::make_unique<dsp_input_stream_shared>(aud);
#endif
        default:
            break;
        }

        return nullptr;
    }
}
