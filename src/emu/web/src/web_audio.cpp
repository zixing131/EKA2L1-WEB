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
// JS glue. One AudioContext shared by all streams.
//
// Preferred path: an AudioWorklet whose processor reads interleaved S16 PCM
// straight out of a ring buffer living in the (shared) WASM heap. The audio
// thread drains the ring in real time, so sound keeps playing even when the
// browser main thread is busy running the emulator — the main thread only
// has to top the ring up once per frame (or slower, up to the ring size).
//
// Fallback path (worklet unavailable / not ready yet): schedule AudioBuffer
// chunks back-to-back, which needs main-thread service every ~150ms and
// stutters under load.
// ============================================================================

// clang-format off
EM_JS(void, ek_webaudio_init, (), {
    if (window.__ekAudio) return;
    var A = { ctx: null, streams: {}, nodes: {}, workletReady: 0, workletFailed: 0 };
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

    A.ensureCtx = function () {
        if (!A.ctx) {
            try {
                A.ctx = new (window.AudioContext || window.webkitAudioContext)();
            } catch (e) {
                return null;
            }
            A.loadWorklet();
        }
        return A.ctx;
    };

    // The processor source is inlined and loaded via a blob URL so deployment
    // stays a single-file affair. Control block (Int32Array, 4 words):
    //   [0] write position in frames (monotonic, uint32 wrap)
    //   [1] read position in frames (monotonic, uint32 wrap)
    //   [2] volume, 16.16 fixed point
    //   [3] flags: bit0 = stream dead, processor should self-destruct
    var PROC_SRC = ''
        + 'class EkRing extends AudioWorkletProcessor {\n'
        + '  constructor(o) {\n'
        + '    super();\n'
        + '    var d = o.processorOptions;\n'
        + '    this.c = new Int32Array(d.sab, d.ctrl, 4);\n'
        + '    this.r = new Int16Array(d.sab, d.ring, d.ringLen);\n'
        + '    this.ch = d.ch;\n'
        + '    this.rf = (d.ringLen / d.ch) | 0;\n'
        + '    this.step = d.rate / sampleRate;\n'
        + '    this.pos = 0;\n'
        + '    this.g = 1;\n'                 /* declick: ramp-in gain after starvation */
        + '    this.tail = new Float32Array(8);\n' /* declick: decaying tail per channel */
        + '  }\n'
        + '  process(inputs, outputs) {\n'
        + '    if (Atomics.load(this.c, 3) & 1) return false;\n'
        + '    var out = outputs[0];\n'
        + '    var n = out[0].length;\n'
        + '    var rd = Atomics.load(this.c, 1) >>> 0;\n'
        + '    var wr = Atomics.load(this.c, 0) >>> 0;\n'
        + '    var vol = (Atomics.load(this.c, 2) >>> 0) / (65536 * 32768);\n'
        + '    var avail = (wr - rd) >>> 0;\n'
        + '    if (avail > this.rf) avail = this.rf;\n'
        + '    var pos = this.pos, step = this.step;\n'
        + '    var produced = 0;\n'
        + '    for (var f = 0; f < n; f++) {\n'
        + '      var ip = Math.floor(pos);\n'
        + '      if (ip + 1 >= avail) break;\n'
        + '      if (this.g < 1) this.g = Math.min(1, this.g + 0.005);\n'
        + '      var fr = pos - ip;\n'
        + '      var i0 = ((rd + ip) % this.rf) * this.ch;\n'
        + '      var i1 = ((rd + ip + 1) % this.rf) * this.ch;\n'
        + '      for (var c = 0; c < out.length; c++) {\n'
        + '        var sc = c < this.ch ? c : (this.ch - 1);\n'
        + '        var s = this.r[i0 + sc] + (this.r[i1 + sc] - this.r[i0 + sc]) * fr;\n'
        + '        out[c][f] = s * vol * this.g;\n'
        + '        this.tail[c] = out[c][f];\n'
        + '      }\n'
        + '      pos += step;\n'
        + '      produced = f + 1;\n'
        + '    }\n'
        + '    if (produced < n) {\n'
        + '      for (var f2 = produced; f2 < n; f2++) {\n'
        + '        for (var c2 = 0; c2 < out.length; c2++) {\n'
        + '          this.tail[c2] *= 0.94;\n'
        + '          out[c2][f2] = this.tail[c2];\n'
        + '        }\n'
        + '      }\n'
        + '      this.g = 0;\n'
        + '    }\n'
        + '    var consumed = Math.floor(pos);\n'
        + '    if (consumed > 0) {\n'
        + '      Atomics.store(this.c, 1, (rd + consumed) | 0);\n'
        + '      pos -= consumed;\n'
        + '    }\n'
        + '    this.pos = pos;\n'
        + '    return true;\n'
        + '  }\n'
        + '}\n'
        + 'registerProcessor(\'ek-ring\', EkRing);\n';

    A.loadWorklet = function () {
        if (!A.ctx || !A.ctx.audioWorklet || A.workletReady || A.workletFailed) {
            if (!A.ctx || !A.ctx.audioWorklet) A.workletFailed = 1;
            return;
        }
        var url = URL.createObjectURL(new Blob([PROC_SRC], { type: 'application/javascript' }));
        A.ctx.audioWorklet.addModule(url).then(function () {
            A.workletReady = 1;
            URL.revokeObjectURL(url);
        }, function (e) {
            console.warn('[EKA2L1] AudioWorklet unavailable, falling back to buffer scheduling:', e);
            A.workletFailed = 1;
            URL.revokeObjectURL(url);
        });
    };
});

// 1 = the worklet module is loaded and nodes can be created.
EM_JS(int, ek_webaudio_worklet_state, (), {
    var A = window.__ekAudio;
    if (!A) return 0;
    A.ensureCtx();
    if (A.workletReady) return 1;
    return A.workletFailed ? 2 : 0;
});

// Create the worklet node for one stream, wired to the ring in the WASM heap.
EM_JS(int, ek_webaudio_make_node, (int id, const void *ctrl_ptr, const void *ring_ptr,
    int ring_len_elems, int channels, int rate), {
    var A = window.__ekAudio;
    if (!A || !A.workletReady) return 0;
    var ctx = A.ensureCtx();
    if (!ctx) return 0;
    try {
        var node = new AudioWorkletNode(ctx, 'ek-ring', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [channels],
            processorOptions: {
                sab: HEAP16.buffer,
                ctrl: ctrl_ptr,
                ring: ring_ptr,
                ringLen: ring_len_elems,
                ch: channels,
                rate: rate
            }
        });
        node.connect(ctx.destination);
        A.nodes[id] = node;
        return 1;
    } catch (e) {
        console.warn('[EKA2L1] AudioWorkletNode creation failed:', e);
        A.workletFailed = 1;
        A.workletReady = 0;
        return 0;
    }
});

EM_JS(void, ek_webaudio_node_drop, (int id), {
    var A = window.__ekAudio;
    if (!A) return;
    var node = A.nodes[id];
    if (node) {
        try { node.disconnect(); } catch (e) {}
        delete A.nodes[id];
    }
});

// ---- Legacy buffer-scheduling path (fallback) ----

// Queue one chunk of interleaved S16 PCM for a stream. Returns the seconds of
// audio buffered ahead of the playhead after queuing (negative ctx = huge so
// the caller stops pulling).
EM_JS(double, ek_webaudio_queue, (int id, const short *ptr, int frames, int channels, int rate, double vol), {
    var A = window.__ekAudio;
    if (!A) return 1e9;

    if (!A.ensureCtx()) {
        return 1e9;
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
    // Fallback path: keep roughly this much audio scheduled per stream.
    static constexpr double TARGET_BUFFERED_SECS = 0.15;

    // Worklet path: ring capacity. The audio thread drains in real time, so
    // this is pure headroom against main-thread stalls, not added latency —
    // playback latency is one worklet quantum.
    static constexpr unsigned RING_SECONDS_DIV = 2; // sample_rate / 2 = 0.5s

    class web_audio_output_stream : public audio_output_stream {
        friend class web_audio_driver;

        // Shared with the worklet processor (Int32Array view over the heap).
        struct ring_control {
            std::atomic<std::uint32_t> write_frames;
            std::atomic<std::uint32_t> read_frames;
            std::atomic<std::uint32_t> volume_q16;
            std::atomic<std::uint32_t> flags; // bit0 = dead
        };

        static_assert(sizeof(ring_control) == 16, "worklet expects 4 i32 control words");

        web_audio_driver *web_driver_;
        data_callback callback_;
        std::vector<std::int16_t> chunk_buf_;

        std::unique_ptr<ring_control> ctrl_;
        std::vector<std::int16_t> ring_buf_;
        std::uint32_t ring_frames_;
        bool node_made_;

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
            , ctrl_(std::make_unique<ring_control>())
            , ring_frames_(sample_rate / RING_SECONDS_DIV)
            , node_made_(false)
            , id_(next_id_++)
            , volume_(1.0f)
            , playing_(false)
            , frames_pulled_(0) {
            // 50ms pull chunks for both paths.
            chunk_buf_.resize((sample_rate / 20) * channels);
            ring_buf_.resize(ring_frames_ * channels);

            ctrl_->write_frames.store(0, std::memory_order_relaxed);
            ctrl_->read_frames.store(0, std::memory_order_relaxed);
            ctrl_->volume_q16.store(1 << 16, std::memory_order_relaxed);
            ctrl_->flags.store(0, std::memory_order_relaxed);
        }

        ~web_audio_output_stream() override {
            playing_ = false;
            if (ctrl_) {
                ctrl_->flags.store(1, std::memory_order_release); // processor self-destructs
            }
            web_driver_->unregister_stream(this);
            ek_webaudio_node_drop(id_);
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

        // Top up the ring (worklet path) or the schedule (fallback path) from
        // the emulated side. Runs on the browser main thread once per frame.
        void pump(const float master_volume_factor) {
            if (!playing_) {
                return;
            }

            const std::size_t chunk_frames = chunk_buf_.size() / channels;

            if (!node_made_ && (ek_webaudio_worklet_state() == 1)) {
                node_made_ = (ek_webaudio_make_node(id_, ctrl_.get(), ring_buf_.data(),
                                  static_cast<int>(ring_buf_.size()), channels,
                                  static_cast<int>(sample_rate))
                    != 0);
            }

            if (node_made_) {
                const float vol = std::clamp(volume_ * master_volume_factor, 0.0f, 4.0f);
                ctrl_->volume_q16.store(static_cast<std::uint32_t>(vol * 65536.0f),
                    std::memory_order_relaxed);

                for (int guard = 0; guard < 12; guard++) {
                    const std::uint32_t wr = ctrl_->write_frames.load(std::memory_order_relaxed);
                    const std::uint32_t rd = ctrl_->read_frames.load(std::memory_order_acquire);
                    const std::uint32_t used = wr - rd;

                    if (ring_frames_ - used < chunk_frames) {
                        break; // ring is full
                    }

                    const std::size_t produced = callback_(chunk_buf_.data(), chunk_frames);
                    if (produced == 0) {
                        break;
                    }

                    // Commit only the frames the source really produced. We pull
                    // ahead of real time, so a short read just means "queue ran
                    // dry for now" — padding it with silence would bake an
                    // audible gap (crackle) into the stream.
                    const std::uint32_t start = wr % ring_frames_;
                    const std::uint32_t first = std::min<std::uint32_t>(
                        static_cast<std::uint32_t>(produced), ring_frames_ - start);
                    std::memcpy(ring_buf_.data() + static_cast<std::size_t>(start) * channels,
                        chunk_buf_.data(), static_cast<std::size_t>(first) * channels * sizeof(std::int16_t));
                    if (first < produced) {
                        std::memcpy(ring_buf_.data(), chunk_buf_.data() + static_cast<std::size_t>(first) * channels,
                            (produced - first) * channels * sizeof(std::int16_t));
                    }

                    ctrl_->write_frames.store(wr + static_cast<std::uint32_t>(produced),
                        std::memory_order_release);
                    frames_pulled_ += produced;

                    if (produced < chunk_frames) {
                        break;
                    }
                }
                return;
            }

            // Fallback: schedule AudioBuffer chunks.
            double ahead = ek_webaudio_ahead(id_);

            for (int guard = 0; (ahead < TARGET_BUFFERED_SECS) && (guard < 8); guard++) {
                const std::size_t produced = callback_(chunk_buf_.data(), chunk_frames);
                if (produced == 0) {
                    break;
                }

                frames_pulled_ += produced;
                ahead = ek_webaudio_queue(id_, chunk_buf_.data(), static_cast<int>(produced),
                    channels, static_cast<int>(sample_rate),
                    static_cast<double>(volume_) * master_volume_factor);

                if (produced < chunk_frames) {
                    break;
                }
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
        // Both paths accept any stream rate (the worklet resamples linearly,
        // the fallback lets the browser resample AudioBuffers).
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
