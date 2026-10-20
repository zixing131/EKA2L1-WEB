/* Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later.
 * MMF DevSound PCM/MP3 output without FFmpeg. Guest writes can split frames.
 */
#include <drivers/audio/backend/minimp3/dsp_minimp3.h>
#include <drivers/audio/backend/dsp_shared.h>
#include <common/log.h>
#include <minimp3.h>
#include <algorithm>
#include <cstring>

namespace eka2l1::drivers {
    namespace {
        class minimp3_dsp_output : public dsp_output_stream_shared {
            mp3dec_t decoder_{};
            std::vector<std::uint8_t> pending_;
            bool need_input_ = true;
            bool first_frame_ = true;
            bool failed_ = false;
            std::size_t skip_tag_ = 0;
        public:
            explicit minimp3_dsp_output(audio_driver *driver) : dsp_output_stream_shared(driver) {
                format_ = PCM16_FOUR_CC_CODE;
                mp3dec_init(&decoder_);
            }
            ~minimp3_dsp_output() override { shutdown_stream(); }
            bool format(four_cc value) override {
                if (value != PCM16_FOUR_CC_CODE && value != PCM8_FOUR_CC_CODE && value != MP3_FOUR_CC_CODE) return false;
                format_ = value;
                pending_.clear();
                skip_tag_ = 0;
                need_input_ = first_frame_ = true;
                failed_ = false;
                mp3dec_init(&decoder_);
                return true;
            }
            void get_supported_formats(std::vector<four_cc> &formats) override {
                formats = { PCM16_FOUR_CC_CODE, PCM8_FOUR_CC_CODE, MP3_FOUR_CC_CODE };
            }
            bool write(const std::uint8_t *data, std::uint32_t size) override {
                if (failed_ || size > 512 * 1024 || pending_.size() + size > 512 * 1024) return false;
                return dsp_output_stream_shared::write(data, size);
            }
            void queue_data_decode(const std::uint8_t *data, std::size_t size) override {
                if (size) pending_.insert(pending_.end(), data, data + size);
                need_input_ = false;
            }
            bool decode_data(std::vector<std::uint8_t> &dest) override {
                dest.clear();
                if (format_ == PCM8_FOUR_CC_CODE) {
                    dest.resize(pending_.size() * 2);
                    for (std::size_t i = 0; i < pending_.size(); ++i) {
                        const std::int16_t sample = static_cast<std::int8_t>(pending_[i]) * 256;
                        std::memcpy(dest.data() + i * 2, &sample, 2);
                    }
                    pending_.clear(); need_input_ = true;
                    return !dest.empty();
                }
                if (!skip_tag_ && pending_.size() >= 10 && !std::memcmp(pending_.data(), "ID3", 3)) {
                    skip_tag_ = 10 + ((pending_[6] & 127) << 21) + ((pending_[7] & 127) << 14)
                        + ((pending_[8] & 127) << 7) + (pending_[9] & 127);
                }
                if (skip_tag_) {
                    const auto count = std::min(skip_tag_, pending_.size());
                    pending_.erase(pending_.begin(), pending_.begin() + count);
                    skip_tag_ -= count;
                }
                std::int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
                // Decode a bounded group, keeping the shared PCM ring small.
                for (int frame = 0; frame < 8 && !pending_.empty(); ++frame) {
                    mp3dec_frame_info_t info{};
                    auto next_decoder = decoder_;
                    const int samples = mp3dec_decode_frame(&next_decoder, pending_.data(), pending_.size(), pcm, &info);
                    if (!info.hz || !info.frame_bytes) {
                        // A trailing partial frame is not corrupt data. Retain it
                        // and the bit reservoir until the next guest write.
                        // A complete Layer III frame is much smaller than 16 KiB.
                        // Bound resynchronization work on garbage while retaining
                        // ample tail bytes for split/free-format frame headers.
                        if (pending_.size() > 16384)
                            pending_.erase(pending_.begin(), pending_.end() - 16384);
                        need_input_ = true;
                        break;
                    }
                    decoder_ = next_decoder;
                    pending_.erase(pending_.begin(), pending_.begin() + info.frame_bytes);
                    if (first_frame_) {
                        LOG_INFO(DRIVER_AUD, "MMF MP3: {} Hz/{} channels, output {} Hz/{} channels", info.hz, info.channels, freq_, channels_);
                        first_frame_ = false;
                    }
                    if (info.hz != freq_ || info.channels != channels_) {
                        // MMF's controller configures the output from the MP3
                        // header before PlayData. Do not reinterpret other PCM.
                        LOG_ERROR(DRIVER_AUD, "MMF MP3 header disagrees with configured output");
                        failed_ = true;
                        need_input_ = true;
                        break;
                    }
                    const auto bytes = samples * info.channels * sizeof(std::int16_t);
                    const auto at = dest.size();
                    dest.resize(at + bytes);
                    std::memcpy(dest.data() + at, pcm, bytes);
                }
                if (pending_.empty()) need_input_ = true;
                return !dest.empty();
            }
            bool internal_decode_running_out() override {
                if (format_ == PCM16_FOUR_CC_CODE) return dsp_output_stream_shared::internal_decode_running_out();
                return need_input_ && buffer_.size() <= low_water_mark_samples();
            }
            bool stop() override {
                if (stream_) stream_->stop();
                pending_.clear(); skip_tag_ = 0;
                need_input_ = first_frame_ = true;
                failed_ = false;
                mp3dec_init(&decoder_);
                return dsp_output_stream_shared::stop();
            }
        };
    }
    std::unique_ptr<dsp_output_stream> new_minimp3_dsp_output(audio_driver *driver) {
        return std::make_unique<minimp3_dsp_output>(driver);
    }
}
