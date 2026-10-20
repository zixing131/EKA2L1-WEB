/* Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later.
 * Bounded, incremental MP3 decoding for platforms without FFmpeg.
 */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MINIMP3_SCAN_LIMIT (256 * 1024)
#include <minimp3_ex.h>
#include <drivers/audio/backend/minimp3/player_minimp3.h>
#include <drivers/audio/audio.h>
#include <drivers/audio/stream.h>
#include <common/buffer.h>
#include <common/log.h>
#include <algorithm>
#include <cstring>

namespace eka2l1::drivers {
    namespace {
        struct decoder_deleter {
            void operator()(mp3dec_ex_t *decoder) const { mp3dec_ex_close(decoder); delete decoder; }
        };
        using decoder_ptr = std::unique_ptr<mp3dec_ex_t, decoder_deleter>;

        class minimp3_player : public player {
            audio_driver *driver_;
            std::vector<std::uint8_t> source_;
            decoder_ptr decoder_;
            std::unique_ptr<audio_output_stream> output_;
            bool playing_ = false;
            bool draining_ = false;
            bool finished_ = false;
            std::uint64_t submitted_ = 0;
            std::uint64_t origin_us_ = 0;
            std::int32_t repeats_ = 0;
            std::int32_t repeat_left_ = 0;
            std::uint64_t silence_frames_ = 0;
            std::uint64_t silence_left_ = 0;

            bool make_output() {
                output_ = driver_->new_output_stream(decoder_->info.hz, decoder_->info.channels,
                    [this](std::int16_t *data, std::size_t frames) { return render(data, frames); });
                if (!output_) return false;
                output_->set_volume(volume_ / 10.0f);
                submitted_ = 0;
                return true;
            }
            std::size_t render(std::int16_t *data, std::size_t frames) {
                if (!playing_ || !decoder_) return 0;
                const auto channels = decoder_->info.channels;
                if (draining_) {
                    std::uint64_t played = 0;
                    if (output_->current_frame_position(&played) && played + 1 < submitted_) return 0;
                    playing_ = false;
                    output_->pause();
                    finished_ = true;
                    // Complete only after queued PCM has reached the playhead.
                    // The callback may clear itself; don't hold player::lock_.
                    auto callback = callback_;
                    if (callback) callback(userdata_.data());
                    return 0;
                }
                if (silence_left_) {
                    const auto count = std::min<std::uint64_t>(silence_left_, frames);
                    std::memset(data, 0, count * channels * sizeof(std::int16_t));
                    silence_left_ -= count;
                    submitted_ += count;
                    return count;
                }
                const auto samples = mp3dec_ex_read(decoder_.get(), data, frames * channels);
                const auto count = samples / channels;
                if (balance_ && channels == 2) {
                    for (std::size_t i = 0; i < samples; i += 2) {
                        if (balance_ > 0) data[i] = data[i] * (100 - balance_) / 100;
                        else data[i + 1] = data[i + 1] * (100 + balance_) / 100;
                    }
                }
                submitted_ += count;
                if (count < frames) {
                    if (repeat_left_ != 0 && !decoder_->last_error) {
                        if (repeat_left_ > 0) --repeat_left_;
                        mp3dec_ex_seek(decoder_.get(), 0);
                        silence_left_ = silence_frames_;
                    } else {
                        draining_ = true;
                    }
                }
                return count;
            }
            bool open(common::ro_stream &stream) {
                constexpr std::size_t max_file_size = 32 * 1024 * 1024;
                const auto length = stream.size();
                if (!stream.valid() || !length || length > max_file_size) return false;
                std::vector<std::uint8_t> bytes(length);
                stream.seek(0, common::seek_where::beg);
                if (stream.read(bytes.data(), length) != length) return false;
                decoder_ptr decoder(new mp3dec_ex_t{});
                if (mp3dec_ex_open_buf(decoder.get(), bytes.data(), bytes.size(), MP3D_SEEK_TO_SAMPLE)
                    || !decoder->samples || decoder->info.hz <= 0 || decoder->info.channels < 1 || decoder->info.channels > 2)
                    return false;
                playing_ = false;
                output_.reset();
                decoder_.reset();
                source_ = std::move(bytes);
                decoder_ = std::move(decoder);
                draining_ = finished_ = false;
                origin_us_ = 0;
                silence_left_ = 0;
                repeat_left_ = repeats_;
                LOG_INFO(DRIVER_AUD, "MP3 decoder: {} Hz, {} channels, {} us", decoder_->info.hz, decoder_->info.channels, duration());
                return make_output();
            }
        public:
            explicit minimp3_player(audio_driver *driver) : driver_(driver) {}
            ~minimp3_player() override { playing_ = false; output_.reset(); }
            bool open_url(const std::string &url) override {
                common::ro_std_file_stream stream(url, true);
                return stream.valid() && open(stream);
            }
            bool open_custom(common::rw_stream *stream) override { return stream && open(*stream); }
            bool play() override {
                if (!decoder_ || !output_) return false;
                if (finished_) set_position(0);
                playing_ = true;
                return output_->start();
            }
            bool stop() override {
                playing_ = false;
                if (decoder_) set_position(0);
                repeat_left_ = repeats_;
                return true;
            }
            void pause() override {
                playing_ = false;
                if (output_) output_->pause();
            }
            bool record() override { return false; }
            bool crop() override { return false; }
            bool is_playing() const override { return playing_; }
            player_type get_player_type() const override { return player_type_minimp3; }
            bool set_volume(std::uint32_t volume) override {
                if (!player::set_volume(volume)) return false;
                return !output_ || output_->set_volume(volume / 10.0f);
            }
            void set_position(std::uint64_t microseconds) override {
                if (!decoder_) return;
                const auto target = std::min(microseconds, duration());
                const auto sample = (target * decoder_->info.hz / 1000000) * decoder_->info.channels;
                if (mp3dec_ex_seek(decoder_.get(), sample)) return;
                // Recreate the output to discard old queued audio on seek/stop.
                output_.reset();
                origin_us_ = target;
                draining_ = finished_ = false;
                silence_left_ = 0;
                if (make_output() && playing_) output_->start();
            }
            std::uint64_t position() const override {
                if (!decoder_) return 0;
                if (finished_) return duration();
                std::uint64_t played = 0;
                if (output_) output_->current_frame_position(&played);
                return std::min(duration(), origin_us_ + frames_to_microseconds(played, decoder_->info.hz));
            }
            std::uint64_t duration() const override {
                return decoder_ ? decoder_->samples / decoder_->info.channels * 1000000ULL / decoder_->info.hz : 0;
            }
            void set_repeat(std::int32_t count, std::int64_t silence) override {
                repeats_ = repeat_left_ = std::max(-1, count);
                silence_frames_ = decoder_ ? std::max<std::int64_t>(0, silence) * decoder_->info.hz / 1000000 : 0;
            }
            bool set_dest_freq(std::uint32_t) override { return false; }
            bool set_dest_channel_count(std::uint32_t) override { return false; }
            bool set_dest_encoding(std::uint32_t) override { return false; }
            void set_dest_container_format(std::uint32_t) override {}
            std::uint32_t get_dest_freq() override { return decoder_ ? decoder_->info.hz : 0; }
            std::uint32_t get_dest_channel_count() override { return decoder_ ? decoder_->info.channels : 0; }
            std::uint32_t get_dest_encoding() override { return player_audio_encoding_pcm16; }
        };
    }
    std::unique_ptr<player> new_minimp3_player(audio_driver *driver) { return std::make_unique<minimp3_player>(driver); }
}
