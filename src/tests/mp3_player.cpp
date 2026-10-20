#include <catch2/catch.hpp>
#include <drivers/audio/audio.h>
#include <drivers/audio/backend/minimp3/player_minimp3.h>
#include <drivers/audio/backend/minimp3/dsp_minimp3.h>
#include <drivers/audio/stream.h>
#include <common/buffer.h>
#include <algorithm>
#include <cmath>

namespace {
    using namespace eka2l1::drivers;
    struct memory_stream : eka2l1::common::rw_stream {
        eka2l1::common::ro_buf_stream reader;
        memory_stream(std::uint8_t *data, std::size_t length) : reader(data, length) {}
        bool valid() override { return reader.valid(); }
        std::uint64_t size() override { return reader.size(); }
        std::uint64_t left() override { return reader.left(); }
        std::uint64_t tell() override { return reader.tell(); }
        void seek(std::int64_t offset, eka2l1::common::seek_where where) override { reader.seek(offset, where); }
        std::uint64_t read(void *data, std::uint64_t length) override { return reader.read(data, length); }
    };
    struct test_stream : audio_output_stream {
        data_callback callback;
        bool running = false;
        std::uint64_t played = 0;
        float volume = 1;
        test_stream(audio_driver *driver, unsigned rate, std::uint8_t channels, data_callback cb)
            : audio_output_stream(driver, rate, channels), callback(std::move(cb)) {}
        bool start() override { running = true; return true; }
        bool stop() override { running = false; return true; }
        void pause() override { running = false; }
        bool is_playing() override { return running; }
        bool is_pausing() override { return !running; }
        bool set_volume(float v) override { volume = v; return true; }
        float get_volume() const override { return volume; }
        bool current_frame_position(std::uint64_t *p) override { *p = played; return true; }
        std::vector<std::int16_t> pull(unsigned frames) {
            if (!running) return {};
            std::vector<std::int16_t> pcm(frames * channels);
            const auto count = callback(pcm.data(), frames);
            played += count;
            pcm.resize(count * channels);
            return pcm;
        }
    };
    struct test_driver : audio_driver {
        test_stream *stream = nullptr;
        test_driver() : audio_driver(100, player_type_tsf) {}
        std::unique_ptr<audio_output_stream> new_output_stream(unsigned rate, std::uint8_t channels, data_callback callback) override {
            auto output = std::make_unique<test_stream>(this, rate, channels, std::move(callback));
            stream = output.get(); return output;
        }
        std::unique_ptr<audio_input_stream> new_input_stream(unsigned, std::uint8_t, data_callback) override { return nullptr; }
        unsigned native_sample_rate() override { return 44100; }
    };
}

TEST_CASE("MP3 decoder produces real PCM and preserves pause, seek and completion", "[audio][mp3]") {
    test_driver driver;
    auto player = eka2l1::drivers::new_minimp3_player(&driver);
    REQUIRE(player->open_url("commonassets/audio/tone.mp3"));
    CHECK(player->get_dest_freq() == 44100);
    CHECK(player->get_dest_channel_count() == 2);
    CHECK(player->duration() == Approx(400000).margin(30000));
    REQUIRE(player->play());
    const auto pcm = driver.stream->pull(4096);
    REQUIRE(pcm.size() == 8192);
    CHECK(std::any_of(pcm.begin(), pcm.end(), [](auto sample) { return std::abs(sample) > 100; }));
    const auto paused = player->position();
    player->pause();
    CHECK(driver.stream->pull(1024).empty());
    CHECK(player->position() == paused);
    REQUIRE(player->play());
    player->set_position(200000);
    CHECK(player->position() == 200000);
    CHECK_FALSE(driver.stream->pull(1024).empty());
    int done = 0;
    std::uint8_t token = 0;
    REQUIRE(player->notify_any_done([&](std::uint8_t *) { ++done; }, &token, 1));
    for (int i = 0; i < 100; ++i) driver.stream->pull(1024);
    CHECK(done == 1);
    CHECK_FALSE(player->is_playing());
    CHECK(player->position() == player->duration());
    REQUIRE(player->stop());
    CHECK(player->position() == 0);
    REQUIRE(player->play());
    CHECK_FALSE(driver.stream->pull(1024).empty());
}

TEST_CASE("MP3 decoder rejects non-audio data", "[audio][mp3]") {
    test_driver driver;
    auto player = eka2l1::drivers::new_minimp3_player(&driver);
    std::uint8_t bytes[32]{};
    memory_stream stream(bytes, sizeof(bytes));
    CHECK_FALSE(player->open_custom(&stream));
    CHECK_FALSE(player->play());
}

TEST_CASE("MMF MP3 accepts split compressed frames and requests subsequent guest buffers", "[audio][mp3]") {
    using namespace eka2l1;
    test_driver driver;
    auto dsp = drivers::new_minimp3_dsp_output(&driver);
    REQUIRE(dsp->format(drivers::MP3_FOUR_CC_CODE));
    REQUIRE(dsp->set_properties(44100, 2));
    common::ro_std_file_stream file("commonassets/audio/tone.mp3", true);
    REQUIRE(file.valid());
    std::vector<std::uint8_t> bytes(file.size());
    REQUIRE(file.read(bytes.data(), bytes.size()) == bytes.size());
    int requested = 0;
    dsp->register_callback(drivers::dsp_stream_notification_more_buffer, [&](void *) { ++requested; return true; }, nullptr);
    REQUIRE(dsp->start());
    int peak = 0;
    std::size_t offset = 0;
    int handled = -1; // initial buffer, then one guest write per notification
    for (int i = 0; i < 2000; ++i) {
        if (offset < bytes.size() && requested > handled) {
            handled = requested;
            const auto size = std::min<std::size_t>(137, bytes.size() - offset);
            REQUIRE(dsp->write(bytes.data() + offset, size));
            offset += size;
        }
        for (const auto sample : driver.stream->pull(128)) peak = std::max(peak, std::abs(int(sample)));
    }
    CHECK(offset == bytes.size());
    CHECK(peak > 100);
    CHECK(requested > 3);
    CHECK(dsp->samples_copied() > 30000);
    CHECK_FALSE(dsp->format(drivers::AMR_FOUR_CC_CODE));
    REQUIRE(dsp->stop());
}

TEST_CASE("MMF MP3 resynchronizes after garbage and rejects incompatible output", "[audio][mp3]") {
    using namespace eka2l1;
    test_driver driver;
    auto dsp = drivers::new_minimp3_dsp_output(&driver);
    REQUIRE(dsp->format(drivers::MP3_FOUR_CC_CODE));
    REQUIRE(dsp->set_properties(44100, 2));
    REQUIRE(dsp->start());
    std::vector<std::uint8_t> junk(32768, 0);
    for (int i = 0; i < 20; ++i) {
        REQUIRE(dsp->write(junk.data(), junk.size()));
        driver.stream->pull(128);
    }
    common::ro_std_file_stream file("commonassets/audio/tone.mp3", true);
    std::vector<std::uint8_t> bytes(file.size());
    REQUIRE(file.read(bytes.data(), bytes.size()) == bytes.size());
    REQUIRE(dsp->write(bytes.data(), bytes.size()));
    int peak = 0;
    for (int i = 0; i < 100; ++i)
        for (auto sample : driver.stream->pull(128)) peak = std::max(peak, std::abs(int(sample)));
    CHECK(peak > 100);
    REQUIRE(dsp->stop());
    REQUIRE(dsp->set_properties(8000, 1));
    REQUIRE(dsp->start());
    REQUIRE(dsp->write(bytes.data(), bytes.size()));
    driver.stream->pull(128);
    CHECK_FALSE(dsp->write(bytes.data(), bytes.size()));
}

TEST_CASE("MP3 custom input owns compressed bytes after opening", "[audio][mp3]") {
    using namespace eka2l1;
    test_driver driver;
    auto player = drivers::new_minimp3_player(&driver);
    common::ro_std_file_stream file("commonassets/audio/tone.mp3", true);
    std::vector<std::uint8_t> bytes(file.size());
    REQUIRE(file.read(bytes.data(), bytes.size()) == bytes.size());
    memory_stream stream(bytes.data(), bytes.size());
    REQUIRE(player->open_custom(&stream));
    std::fill(bytes.begin(), bytes.end(), 0);
    REQUIRE(player->play());
    const auto pcm = driver.stream->pull(4096);
    CHECK(std::any_of(pcm.begin(), pcm.end(), [](auto sample) { return std::abs(sample) > 100; }));
}
