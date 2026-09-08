#include <catch2/catch.hpp>
#include "../emu/web/src/frame_pacer.h"

using eka2l1::web::frame_pacer;

TEST_CASE("Web frame limits retain their rate across display refresh rates", "[web][pacing]") {
    for (int refresh : {30, 60, 90, 120, 144, 165, 240}) {
        for (int limit : {15, 30, 60, 120}) {
            INFO("display=" << refresh << " limit=" << limit);
            frame_pacer pacer;
            pacer.set_max_fps(limit);
            int frames = 0;
            for (int tick = 0; tick < refresh * 10; ++tick) {
                frames += pacer.due(tick * 1000.0 / refresh);
            }
            REQUIRE(frames == Approx(std::min(refresh, limit) * 10).margin(1));
        }
    }
}

TEST_CASE("Web pacing tolerates RAF jitter without halving the frame rate", "[web][pacing]") {
    frame_pacer pacer;
    int frames = 0;
    for (int tick = 0; tick < 600; ++tick) {
        const double jitter = tick % 2 ? -0.2 : 0.2;
        frames += pacer.due(tick * 1000.0 / 60.0 + jitter);
    }
    REQUIRE(frames == 600);
}

TEST_CASE("Web pacing skips missed frames and applies runtime limits immediately", "[web][pacing]") {
    frame_pacer pacer;
    REQUIRE(pacer.due(0));
    REQUIRE_FALSE(pacer.due(1));
    REQUIRE(pacer.due(60000));
    REQUIRE_FALSE(pacer.due(60000));
    REQUIRE_FALSE(pacer.due(60001));
    REQUIRE(pacer.due(60000 + 1000.0 / 60.0));

    pacer.set_max_fps(30);
    REQUIRE(pacer.due(61000));
    REQUIRE_FALSE(pacer.due(61000 + 1000.0 / 60.0));
    REQUIRE(pacer.due(61000 + 1000.0 / 30.0));

    pacer.set_max_fps(120);
    REQUIRE(pacer.due(62000));
    REQUIRE(pacer.due(62000 + 1000.0 / 120.0));
}
