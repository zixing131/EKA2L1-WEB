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

#include "web_audio.h"

#include <common/log.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include <emscripten.h>

// ============================================================================
// JS glue. One AudioContext shared by all streams; each stream schedules
// AudioBuffer chunks back-to-back on its own timeline so the browser mixes
// and resamples everything. All calls happen on the browser main thread.
// ============================================================================

// clang-format off
EM_JS(void, ek_webaudio_init, (), {
    if (window.__ekAudio) return;
    var A = { ctx: null, streams: {} };
    window.__ekAudio = A;

    // Autoplay policy: contexts start suspended until a user gesture. Resume
    // on the first interaction of any kind.
    var resume = function () {
        if (A.ctx && A.ctx.state === 'suspended') {
            A.ctx.resume();
        }
    };
    ['pointerdown', 'touchstart', 'keydown'].forEach(function (evt) {
        document.addEventListener(evt, resume, { capture: true, passive: true });
    });
});

// Queue one chunk of interleaved S16 PCM for a stream. Returns the seconds of
// audio buffered ahead of the playhead after queuing (negative ctx = huge so
// the caller stops pulling).
EM_JS(double, ek_webaudio_queue, (int id, const short *ptr, int frames, int channels, int rate, double vol), {
    var A = window.__ekAudio;
    if (!A) return 1e9;

    if (!A.ctx) {
        try {
            A.ctx = new (window.AudioContext || window.webkitAudioContext)();
        } catch (e) {
            return 1e9;
        }
    }
    if (A.ctx.state === 'suspended') {
        A.ctx.resume();
    }

    var st = A.streams[id];
    if (!st) {
        st = A.streams[id] = { next: 0 };
    }

    var buf = A.ctx.createBuffer(channels, frames, rate);
    var base = ptr >> 1;
    var scale = vol / 32768.0;
    for (var c = 0; c < channels; c++) {
        var ch = buf.getChannelData(c);
        for (var i = 0; i < frames; i++) {
            ch[i] = HEAP16[base + i * channels + c] * scale;
        }
    }

    var src = A.ctx.createBufferSource();
    src.buffer = buf;
    src.connect(A.ctx.destination);

    // Small lead-in guards against scheduling in the past after a stall.
    var t = Math.max(A.ctx.currentTime + 0.03, st.next);
    src.start(t);
    st.next = t + frames / rate;

    return st.next - A.ctx.currentTime;
});

EM_JS(double, ek_webaudio_ahead, (int id), {
    var A = window.__ekAudio;
    if (!A || !A.ctx) return 0;
    var st = A.streams[id];
    if (!st) return 0;
    var ahead = st.next - A.ctx.currentTime;
    return (ahead > 0) ? ahead : 0;
});

EM_JS(void, ek_webaudio_drop, (int id), {
    var A = window.__ekAudio;
    if (A) delete A.streams[id];
});
// clang-format on

namespace eka2l1::drivers {
    // Keep roughly this much audio scheduled per stream. Big enough to ride
    // out a slow frame, small enough to keep latency tolerable for games.
    static constexpr double TARGET_BUFFERED_SECS = 0.15;

    class web_audio_output_stream : public audio_output_stream {
        friend class web_audio_driver;

        web_audio_driver *web_driver_;
        data_callback callback_;
        std::vector<std::int16_t> chunk_buf_;

        int id_;
        float volume_;
        std::atomic<bool> playing_;
        std::uint64_t frames_pulled_;

        static std::atomic<int> next_id_;

    public:
        explicit web_audio_output_stream(web_audio_driver *driver, const std::uint32_t sample_rate,
            const std::uint8_t channels, data_callback callback)
            : audio_output_stream(driver, sample_rate, channels)
            , web_driver_(driver)
            , callback_(callback)
            , id_(next_id_++)
            , volume_(1.0f)
            , playing_(false)
            , frames_pulled_(0) {
            // 50ms chunks: 2-4 pulls fill the schedule, then 1 pull per frame.
            chunk_buf_.resize((sample_rate / 20) * channels);
        }

        ~web_audio_output_stream() override {
            playing_ = false;
            web_driver_->unregister_stream(this);
            ek_webaudio_drop(id_);
        }

        bool start() override {
            playing_ = true;
            return true;
        }

        bool stop() override {
            playing_ = false;
            return true;
        }

        void pause() override {
            playing_ = false;
        }

        bool is_playing() override {
            return playing_;
        }

        bool is_pausing() override {
            return !playing_;
        }

        bool set_volume(const float volume) override {
            volume_ = volume;
            return true;
        }

        float get_volume() const override {
            return volume_;
        }

        bool current_frame_position(std::uint64_t *pos) override {
            *pos = frames_pulled_;
            return true;
        }

        // Pull PCM from the emulated side and hand it to Web Audio until the
        // stream has TARGET_BUFFERED_SECS scheduled. Runs on the main thread.
        void pump(const float master_volume_factor) {
            if (!playing_) {
                return;
            }

            double ahead = ek_webaudio_ahead(id_);
            const std::size_t chunk_frames = chunk_buf_.size() / channels;

            // Bound iterations: never spend more than a few chunks per frame.
            for (int guard = 0; (ahead < TARGET_BUFFERED_SECS) && (guard < 8); guard++) {
                const std::size_t produced = callback_(chunk_buf_.data(), chunk_frames);
                if (produced == 0) {
                    break;
                }

                if (produced < chunk_frames) {
                    std::memset(chunk_buf_.data() + produced * channels, 0,
                        (chunk_frames - produced) * channels * sizeof(std::int16_t));
                }

                frames_pulled_ += chunk_frames;
                ahead = ek_webaudio_queue(id_, chunk_buf_.data(), static_cast<int>(chunk_frames),
                    channels, static_cast<int>(sample_rate),
                    static_cast<double>(volume_) * master_volume_factor);
            }
        }
    };

    std::atomic<int> web_audio_output_stream::next_id_{ 1 };

    web_audio_driver::web_audio_driver(const std::uint32_t initial_master_vol,
        const player_type preferred_midi_backend)
        : audio_driver(initial_master_vol, preferred_midi_backend) {
        ek_webaudio_init();
    }

    std::unique_ptr<audio_output_stream> web_audio_driver::new_output_stream(const std::uint32_t sample_rate,
        const std::uint8_t channels, data_callback callback) {
        auto stream = std::make_unique<web_audio_output_stream>(this, sample_rate, channels, callback);

        const std::lock_guard<std::mutex> guard(streams_lock_);
        streams_.push_back(stream.get());

        return stream;
    }

    std::unique_ptr<audio_input_stream> web_audio_driver::new_input_stream(const std::uint32_t sample_rate,
        const std::uint8_t channels, data_callback callback) {
        // No microphone support on the web build.
        return nullptr;
    }

    std::uint32_t web_audio_driver::native_sample_rate() {
        // Web Audio resamples AudioBuffers of any rate; reporting a fixed rate
        // avoids creating the AudioContext before the first user gesture.
        return 44100;
    }

    void web_audio_driver::unregister_stream(web_audio_output_stream *stream) {
        const std::lock_guard<std::mutex> guard(streams_lock_);
        streams_.erase(std::remove(streams_.begin(), streams_.end(), stream), streams_.end());
    }

    void web_audio_driver::pump() {
        const float master_factor = suspending() ? 0.0f : (master_volume() / 100.0f);

        const std::lock_guard<std::mutex> guard(streams_lock_);
        for (web_audio_output_stream *stream : streams_) {
            stream->pump(master_factor);
        }
    }
}
