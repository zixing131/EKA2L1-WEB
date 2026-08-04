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
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <drivers/camera/backend/web/camera_web.h>
#include <drivers/camera/backend/pattern/camera_pattern.h>

#include <common/log.h>

#include <emscripten.h>
#include <emscripten/threading.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace eka2l1::drivers::camera {
    namespace web_cam {
        constexpr std::uint32_t CAMERA_COUNT = 2;
        constexpr int VIEWFINDER_FPS = 15;

        const eka2l1::vec2 BACK_MAX_SIZE(1280, 720);
        const eka2l1::vec2 FRONT_MAX_SIZE(640, 480);

        std::vector<eka2l1::vec2> image_size_ladder(const bool front_facing) {
            static const eka2l1::vec2 CANDIDATES[] = {
                eka2l1::vec2(1280, 720), eka2l1::vec2(1024, 768), eka2l1::vec2(640, 480),
                eka2l1::vec2(320, 240), eka2l1::vec2(160, 120)
            };

            const eka2l1::vec2 max_size = front_facing ? FRONT_MAX_SIZE : BACK_MAX_SIZE;
            std::vector<eka2l1::vec2> result;
            for (const eka2l1::vec2 &candidate : CANDIDATES) {
                if ((candidate.x <= max_size.x) && (candidate.y <= max_size.y)) {
                    result.push_back(candidate);
                }
            }
            if (result.empty()) {
                result.push_back(max_size);
            }
            return result;
        }

        void rgba_to_bgra(const std::uint8_t *rgba, const int width, const int height,
            std::vector<std::uint8_t> &bgra) {
            bgra.resize(static_cast<std::size_t>(width) * 4 * height);
            for (int i = 0; i < width * height; i++) {
                bgra[i * 4 + 0] = rgba[i * 4 + 2];
                bgra[i * 4 + 1] = rgba[i * 4 + 1];
                bgra[i * 4 + 2] = rgba[i * 4 + 0];
                bgra[i * 4 + 3] = 0xFF;
            }
        }

        bool encode_rgba(const std::uint8_t *rgba, const int width, const int height,
            const frame_format format, std::vector<std::uint8_t> &out) {
            std::vector<std::uint8_t> bgra;
            rgba_to_bgra(rgba, width, height, bgra);
            if ((format == FRAME_FORMAT_JPEG) || (format == FRAME_FORMAT_EXIF)) {
                return pattern_encode_bgra_to_jpeg(bgra.data(), width, height, out);
            }
            return pattern_convert_bgra_to_guest(bgra.data(), static_cast<std::size_t>(width) * 4,
                width, height, format, out);
        }

        // clang-format off
        EM_JS(void, ek_cam_js_init, (), {
            if (window.__ekCam) return;
            var C = { cams: {}, perm: 0, warmStream: null };
            window.__ekCam = C;

            C.ensureVideo = function (id) {
                var cam = C.cams[id];
                if (!cam) return null;
                if (cam.video) return cam.video;
                var v = document.createElement('video');
                v.playsInline = true;
                v.autoplay = true;
                v.muted = true;
                v.style.display = 'none';
                document.body.appendChild(v);
                cam.video = v;
                cam.canvas = document.createElement('canvas');
                cam.ctx = null;
                return v;
            };

            C.stopTracks = function (stream) {
                if (!stream) return;
                try { stream.getTracks().forEach(function (t) { t.stop(); }); } catch (e) {}
            };

            C.open = function (id, facing, wantW, wantH) {
                if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
                    console.warn('[ekCam] getUserMedia unavailable');
                    return;
                }
                var cam = C.cams[id];
                if (!cam) {
                    cam = { id: id, facing: facing, wantW: wantW|0, wantH: wantH|0,
                            stream: null, feeding: 0, bufPtr: 0, bufCap: 0,
                            frameW: 0, frameH: 0, ready: 0, failed: 0, raf: 0 };
                    C.cams[id] = cam;
                } else {
                    cam.facing = facing;
                    cam.wantW = wantW|0;
                    cam.wantH = wantH|0;
                }
                if (cam.stream) {
                    cam.ready = 1;
                    if (cam.feeding) C.startRaf(id);
                    return;
                }

                C.stopTracks(C.warmStream);
                C.warmStream = null;

                var constraints = {
                    audio: false,
                    video: {
                        facingMode: { ideal: facing === 1 ? 'user' : 'environment' },
                        width: { ideal: Math.max(160, wantW|0) },
                        height: { ideal: Math.max(120, wantH|0) }
                    }
                };

                navigator.mediaDevices.getUserMedia(constraints).then(function (stream) {
                    C.perm = 1;
                    cam.stream = stream;
                    cam.failed = 0;
                    var v = C.ensureVideo(id);
                    v.srcObject = stream;
                    var play = v.play();
                    if (play && play.catch) play.catch(function () {});
                    cam.ready = 1;
                    if (cam.feeding) C.startRaf(id);
                }).catch(function (err) {
                    console.warn('[ekCam] getUserMedia failed:', err && err.name, err && err.message);
                    C.perm = -1;
                    cam.failed = 1;
                    cam.ready = 0;
                });
            };

            C.setBuffer = function (id, ptr, cap) {
                var cam = C.cams[id];
                if (!cam) {
                    cam = { id: id, stream: null, feeding: 0, bufPtr: 0, bufCap: 0,
                            frameW: 0, frameH: 0, ready: 0, failed: 0, raf: 0 };
                    C.cams[id] = cam;
                }
                cam.bufPtr = ptr|0;
                cam.bufCap = cap|0;
            };

            C.sampleOnce = function (id) {
                var cam = C.cams[id];
                if (!cam || !cam.ready || !cam.video || cam.bufPtr <= 0) return 0;
                var v = cam.video;
                var vw = v.videoWidth|0;
                var vh = v.videoHeight|0;
                if (vw <= 0 || vh <= 0) return 0;

                var tw = cam.wantW > 0 ? cam.wantW : vw;
                var th = cam.wantH > 0 ? cam.wantH : vh;
                if (!cam.canvas) C.ensureVideo(id);
                if (cam.canvas.width !== tw || cam.canvas.height !== th) {
                    cam.canvas.width = tw;
                    cam.canvas.height = th;
                    cam.ctx = cam.canvas.getContext('2d', { willReadFrequently: true });
                }
                var ctx = cam.ctx;
                if (!ctx) return 0;

                var srcAspect = vw / vh;
                var dstAspect = tw / th;
                var sx = 0, sy = 0, sw = vw, sh = vh;
                if (srcAspect > dstAspect) {
                    sw = (vh * dstAspect)|0;
                    sx = ((vw - sw) / 2)|0;
                } else if (srcAspect < dstAspect) {
                    sh = (vw / dstAspect)|0;
                    sy = ((vh - sh) / 2)|0;
                }
                ctx.drawImage(v, sx, sy, sw, sh, 0, 0, tw, th);

                var need = tw * th * 4;
                if (need > cam.bufCap) return 0;
                var img = ctx.getImageData(0, 0, tw, th);
                HEAPU8.set(img.data, cam.bufPtr);
                cam.frameW = tw;
                cam.frameH = th;
                return 1;
            };

            C.startRaf = function (id) {
                var cam = C.cams[id];
                if (!cam || cam.raf) return;
                cam.feeding = 1;
                var tick = function () {
                    cam.raf = 0;
                    if (!cam.feeding) return;
                    if (C.sampleOnce(id)) {
                        try { Module['_ek_cam_js_frame_ready'](id, cam.frameW, cam.frameH); } catch (e) {}
                    }
                    cam.raf = requestAnimationFrame(tick);
                };
                cam.raf = requestAnimationFrame(tick);
            };

            C.stopRaf = function (id) {
                var cam = C.cams[id];
                if (!cam) return;
                cam.feeding = 0;
                if (cam.raf) { cancelAnimationFrame(cam.raf); cam.raf = 0; }
            };

            C.close = function (id) {
                var cam = C.cams[id];
                if (!cam) return;
                C.stopRaf(id);
                C.stopTracks(cam.stream);
                cam.stream = null;
                cam.ready = 0;
                if (cam.video) {
                    try { cam.video.srcObject = null; cam.video.remove(); } catch (e) {}
                    cam.video = null;
                }
                cam.canvas = null;
                cam.ctx = null;
                delete C.cams[id];
            };

            C.requestPermission = function () {
                if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) return 0;
                if (C.perm === 1) return 1;
                navigator.mediaDevices.getUserMedia({ audio: false, video: true }).then(function (stream) {
                    C.perm = 1;
                    C.stopTracks(C.warmStream);
                    C.warmStream = stream;
                }).catch(function (err) {
                    console.warn('[ekCam] permission request failed:', err && err.name);
                    C.perm = -1;
                });
                return 1;
            };
        });

        EM_JS(void, ek_cam_js_open, (int id, int facing, int want_w, int want_h), {
            if (!window.__ekCam) ek_cam_js_init();
            window.__ekCam.open(id, facing, want_w, want_h);
        });

        EM_JS(void, ek_cam_js_set_buffer, (int id, uint8_t *ptr, int cap), {
            if (!window.__ekCam) ek_cam_js_init();
            window.__ekCam.setBuffer(id, ptr, cap);
        });

        EM_JS(void, ek_cam_js_start_feed, (int id), {
            if (!window.__ekCam) return;
            window.__ekCam.startRaf(id);
        });

        EM_JS(void, ek_cam_js_stop_feed, (int id), {
            if (!window.__ekCam) return;
            window.__ekCam.stopRaf(id);
        });

        EM_JS(void, ek_cam_js_close, (int id), {
            if (!window.__ekCam) return;
            window.__ekCam.close(id);
        });

        EM_JS(int, ek_cam_js_request_permission, (), {
            if (!window.__ekCam) ek_cam_js_init();
            return window.__ekCam.requestPermission();
        });

        EM_JS(int, ek_cam_js_sample_once, (int id), {
            if (!window.__ekCam) return 0;
            return window.__ekCam.sampleOnce(id)|0;
        });
        // clang-format on

        void main_open(int id, int facing, int w, int h) {
            ek_cam_js_init();
            ek_cam_js_open(id, facing, w, h);
        }

        void main_set_buffer(int id, int ptr, int cap, int) {
            ek_cam_js_set_buffer(id, reinterpret_cast<std::uint8_t *>(static_cast<intptr_t>(ptr)), cap);
        }

        void main_start_feed(int id, int, int, int) {
            ek_cam_js_start_feed(id);
        }

        void main_stop_feed(int id, int, int, int) {
            ek_cam_js_stop_feed(id);
        }

        void main_close(int id, int, int, int) {
            ek_cam_js_close(id);
        }

        void main_request_permission() {
            ek_cam_js_init();
            ek_cam_js_request_permission();
        }

        void run_on_main(void (*fn)(int, int, int, int), int a, int b, int c, int d) {
            if (emscripten_is_main_runtime_thread()) {
                fn(a, b, c, d);
            } else {
                emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_VIIII, fn, a, b, c, d);
            }
        }

        class instance_web;

        std::mutex g_instances_lock;
        std::unordered_map<int, instance_web *> g_instances;
        std::atomic<int> g_next_handle{ 1 };

        class instance_web : public instance {
        private:
            int handle_;
            bool front_facing_;

            std::mutex callback_lock_;
            camera_capture_image_done_callback frame_callback_;
            camera_wants_new_frame_callback wants_frame_callback_;
            eka2l1::vec2 viewfinder_size_{ 0, 0 };
            frame_format viewfinder_format_ = FRAME_FORMAT_ARGB8888;

            std::mutex frame_lock_;
            std::vector<std::uint8_t> latest_rgba_;
            std::vector<std::uint8_t> rgba_scratch_;
            int latest_w_ = 0;
            int latest_h_ = 0;
            bool latest_valid_ = false;

            std::atomic<bool> feed_running_{ false };
            std::thread feed_thread_;

            std::uint32_t optical_zoom_ = 0;
            std::uint32_t digital_zoom_ = 1;
            std::uint32_t contrast_ = 0;
            std::uint32_t brightness_ = 0;
            std::uint32_t white_balance_ = 0;
            std::uint32_t exposure_ = EXPOSURE_MODE_AUTO;
            std::uint32_t flash_mode_ = FLASH_MODE_OFF;
            bool reserved_ = false;

            void feed_loop() {
                using clock = std::chrono::steady_clock;
                const auto interval = std::chrono::milliseconds(1000 / VIEWFINDER_FPS);

                while (feed_running_.load(std::memory_order_acquire)) {
                    const auto start = clock::now();

                    camera_capture_image_done_callback frame_callback;
                    camera_wants_new_frame_callback wants_frame_callback;
                    frame_format format;
                    {
                        const std::lock_guard<std::mutex> guard(callback_lock_);
                        frame_callback = frame_callback_;
                        wants_frame_callback = wants_frame_callback_;
                        format = viewfinder_format_;
                    }

                    if (frame_callback && wants_frame_callback && wants_frame_callback()) {
                        std::vector<std::uint8_t> rgba;
                        int w = 0;
                        int h = 0;
                        {
                            const std::lock_guard<std::mutex> guard(frame_lock_);
                            if (latest_valid_) {
                                rgba = latest_rgba_;
                                w = latest_w_;
                                h = latest_h_;
                            }
                        }
                        if (!rgba.empty()) {
                            std::vector<std::uint8_t> encoded;
                            if (encode_rgba(rgba.data(), w, h, format, encoded)) {
                                frame_callback(encoded.data(), encoded.size(), 0);
                            }
                        }
                    }

                    const auto elapsed = clock::now() - start;
                    if (elapsed < interval) {
                        std::this_thread::sleep_for(interval - elapsed);
                    }
                }
            }

            void ensure_scratch_capacity() {
                // Fixed max so the pointer JS writes into never moves while a
                // rAF sampler is live (capture and viewfinder share this buffer).
                constexpr int kMaxBytes = 1280 * 720 * 4;
                if (static_cast<int>(rgba_scratch_.size()) < kMaxBytes) {
                    rgba_scratch_.assign(static_cast<std::size_t>(kMaxBytes), 0);
                }
            }

            void bind_scratch_buffer(const int width, const int height) {
                const int bytes = width * height * 4;
                std::uint8_t *ptr = nullptr;
                int cap = 0;
                {
                    const std::lock_guard<std::mutex> guard(frame_lock_);
                    ensure_scratch_capacity();
                    ptr = rgba_scratch_.data();
                    cap = static_cast<int>(rgba_scratch_.size());
                }
                (void)bytes;
                run_on_main(main_set_buffer, handle_,
                    static_cast<int>(reinterpret_cast<intptr_t>(ptr)), cap, 0);
            }

        public:
            explicit instance_web(const int index)
                : handle_(g_next_handle.fetch_add(1))
                , front_facing_(index == 1) {
                {
                    const std::lock_guard<std::mutex> guard(g_instances_lock);
                    g_instances[handle_] = this;
                }
                const eka2l1::vec2 open_size = front_facing_ ? FRONT_MAX_SIZE : BACK_MAX_SIZE;
                run_on_main(main_open, handle_, front_facing_ ? 1 : 0, open_size.x, open_size.y);
            }

            ~instance_web() override {
                release();
                run_on_main(main_close, handle_, 0, 0, 0);
                const std::lock_guard<std::mutex> guard(g_instances_lock);
                g_instances.erase(handle_);
            }

            void accept_js_frame(const int width, const int height) {
                const std::size_t bytes = static_cast<std::size_t>(width) * 4 * height;
                const std::lock_guard<std::mutex> guard(frame_lock_);
                if (rgba_scratch_.size() < bytes) {
                    return;
                }
                latest_rgba_.resize(bytes);
                std::memcpy(latest_rgba_.data(), rgba_scratch_.data(), bytes);
                latest_w_ = width;
                latest_h_ = height;
                latest_valid_ = true;
            }

            bool set_parameter(const parameter_key key, const std::uint32_t value) override {
                switch (key) {
                case PARAMETER_KEY_OPTICAL_ZOOM: optical_zoom_ = value; break;
                case PARAMETER_KEY_DIGITAL_ZOOM: digital_zoom_ = value; break;
                case PARAMETER_KEY_CONTRAST: contrast_ = value; break;
                case PARAMETER_KEY_BRIGHTNESS: brightness_ = value; break;
                case PARAMETER_KEY_FLASH: flash_mode_ = value; break;
                case PARAMETER_KEY_EXPOSURE: exposure_ = value; break;
                case PARAMETER_KEY_WHITE_BALANCE: white_balance_ = value; break;
                default:
                    LOG_WARN(DRIVER_CAM, "Unsupported parameter key {} to set value", static_cast<int>(key));
                    return false;
                }
                return true;
            }

            bool get_parameter(const parameter_key key, std::uint32_t &value) override {
                switch (key) {
                case PARAMETER_KEY_OPTICAL_ZOOM: value = optical_zoom_; break;
                case PARAMETER_KEY_DIGITAL_ZOOM: value = digital_zoom_; break;
                case PARAMETER_KEY_CONTRAST: value = contrast_; break;
                case PARAMETER_KEY_BRIGHTNESS: value = brightness_; break;
                case PARAMETER_KEY_FLASH: value = flash_mode_; break;
                case PARAMETER_KEY_EXPOSURE: value = exposure_; break;
                case PARAMETER_KEY_WHITE_BALANCE: value = white_balance_; break;
                default:
                    LOG_WARN(DRIVER_CAM, "Unsupported parameter key {} to get value", static_cast<int>(key));
                    return false;
                }
                return true;
            }

            std::vector<frame_format> supported_frame_formats() override {
                return std::vector<frame_format>(std::begin(PATTERN_SUPPORTED_FORMATS),
                    std::end(PATTERN_SUPPORTED_FORMATS));
            }

            std::vector<eka2l1::vec2> supported_output_image_sizes(const frame_format) override {
                return image_size_ladder(front_facing_);
            }

            bool reserve() override {
                if (reserved_) {
                    LOG_ERROR(DRIVER_CAM, "Another camera instance is currently reserved!");
                    return false;
                }
                reserved_ = true;
                return true;
            }

            void release() override {
                stop_viewfinder_feed();
                reserved_ = false;
            }

            info get_info() override {
                info result;
                result.camera_direction_ = front_facing_ ? DIRECTION_FRONT : DIRECTION_BACK;
                result.num_image_sizes_supported_ =
                    static_cast<std::int32_t>(image_size_ladder(front_facing_).size());
                result.flash_modes_supported_ =
                    FLASH_MODE_OFF | FLASH_MODE_AUTO | FLASH_MODE_FORCED | FLASH_MODE_VIDEO_LIGHT;
                result.options_supported_ = CAPTURE_OPTION_ALL;
                result.supported_image_formats_ = 0;
                for (const frame_format format : PATTERN_SUPPORTED_FORMATS) {
                    result.supported_image_formats_ |= static_cast<std::uint32_t>(format);
                }
                return result;
            }

            void capture_image(const std::uint32_t resolution_index, const frame_format format,
                camera_capture_image_done_callback callback) override {
                if (!callback) {
                    return;
                }
                if (!pattern_is_supported_format(format)) {
                    callback(nullptr, 0, -1);
                    return;
                }
                const std::vector<eka2l1::vec2> sizes = image_size_ladder(front_facing_);
                if (resolution_index >= sizes.size()) {
                    callback(nullptr, 0, -1);
                    return;
                }

                const eka2l1::vec2 size = sizes[resolution_index];
                const int handle = handle_;
                const bool front = front_facing_;

                std::thread([this, callback, size, format, handle, front]() {
                    eka2l1::vec2 restore(0, 0);
                    {
                        const std::lock_guard<std::mutex> guard(callback_lock_);
                        restore = viewfinder_size_;
                    }

                    {
                        const std::lock_guard<std::mutex> guard(frame_lock_);
                        latest_valid_ = false;
                    }

                    bind_scratch_buffer(size.x, size.y);
                    run_on_main(main_open, handle, front ? 1 : 0, size.x, size.y);
                    // Ensure sampling runs even without an active viewfinder feed.
                    run_on_main(main_start_feed, handle, 0, 0, 0);

                    bool got = false;
                    for (int i = 0; i < 90; i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(33));
                        const std::lock_guard<std::mutex> guard(frame_lock_);
                        if (latest_valid_ && latest_w_ > 0 && latest_h_ > 0) {
                            got = true;
                            break;
                        }
                    }

                    std::vector<std::uint8_t> rgba;
                    int w = 0;
                    int h = 0;
                    {
                        const std::lock_guard<std::mutex> guard(frame_lock_);
                        if (latest_valid_) {
                            rgba = latest_rgba_;
                            w = latest_w_;
                            h = latest_h_;
                        }
                    }

                    if (restore.x > 0 && restore.y > 0) {
                        bind_scratch_buffer(restore.x, restore.y);
                        run_on_main(main_open, handle, front ? 1 : 0, restore.x, restore.y);
                    } else if (!feed_running_.load(std::memory_order_acquire)) {
                        run_on_main(main_stop_feed, handle, 0, 0, 0);
                    }

                    if (!got || rgba.empty()) {
                        LOG_ERROR(DRIVER_CAM, "Web camera capture: no frame available");
                        callback(nullptr, 0, -1);
                        return;
                    }

                    std::vector<std::uint8_t> encoded;
                    if (encode_rgba(rgba.data(), w, h, format, encoded)) {
                        callback(encoded.data(), encoded.size(), 0);
                    } else {
                        callback(nullptr, 0, -1);
                    }
                }).detach();
            }

            void receive_viewfinder_feed(const eka2l1::vec2 &size, const frame_format format,
                camera_wants_new_frame_callback new_frame_needed_callback,
                camera_capture_image_done_callback new_frame_come_callback) override {
                if (!reserved_) {
                    LOG_ERROR(DRIVER_CAM, "Camera is not yet reserved to receive viewfinder feed!");
                    return;
                }
                if (!new_frame_come_callback || !new_frame_needed_callback) {
                    return;
                }
                if ((size.x <= 0) || (size.y <= 0) || !pattern_is_supported_format(format)) {
                    return;
                }

                {
                    const std::lock_guard<std::mutex> guard(callback_lock_);
                    viewfinder_size_ = size;
                    viewfinder_format_ = format;
                    frame_callback_ = new_frame_come_callback;
                    wants_frame_callback_ = new_frame_needed_callback;
                }

                bind_scratch_buffer(size.x, size.y);
                run_on_main(main_open, handle_, front_facing_ ? 1 : 0, size.x, size.y);
                run_on_main(main_start_feed, handle_, 0, 0, 0);

                if (feed_running_.load(std::memory_order_acquire)) {
                    return;
                }
                feed_running_.store(true, std::memory_order_release);
                feed_thread_ = std::thread([this]() { feed_loop(); });
            }

            void stop_viewfinder_feed() override {
                {
                    const std::lock_guard<std::mutex> guard(callback_lock_);
                    frame_callback_ = nullptr;
                    wants_frame_callback_ = nullptr;
                }
                run_on_main(main_stop_feed, handle_, 0, 0, 0);
                if (feed_running_.exchange(false, std::memory_order_acq_rel)) {
                    if (feed_thread_.joinable()) {
                        feed_thread_.join();
                    }
                }
            }
        };

        void deliver_frame(int id, int width, int height) {
            instance_web *inst = nullptr;
            {
                const std::lock_guard<std::mutex> guard(g_instances_lock);
                const auto it = g_instances.find(id);
                if (it != g_instances.end()) {
                    inst = it->second;
                }
            }
            if (inst) {
                inst->accept_js_frame(width, height);
            }
        }
    } // namespace web_cam

    std::uint32_t collection_web::count() const {
        return web_cam::CAMERA_COUNT;
    }

    std::unique_ptr<instance> collection_web::make_camera(const std::uint32_t camera_index) {
        if (camera_index >= web_cam::CAMERA_COUNT) {
            LOG_ERROR(DRIVER_CAM, "Web camera index {} out of range!", camera_index);
            return nullptr;
        }
        return std::make_unique<web_cam::instance_web>(static_cast<int>(camera_index));
    }

    int web_request_camera_permission() {
        if (emscripten_is_main_runtime_thread()) {
            web_cam::main_request_permission();
            return 1;
        }
        emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, web_cam::main_request_permission);
        return 1;
    }
}

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void ek_cam_js_frame_ready(int id, int width, int height) {
        eka2l1::drivers::camera::web_cam::deliver_frame(id, width, height);
    }

    EMSCRIPTEN_KEEPALIVE
    int wasm_request_camera(void) {
        return eka2l1::drivers::camera::web_request_camera_permission();
    }
}
