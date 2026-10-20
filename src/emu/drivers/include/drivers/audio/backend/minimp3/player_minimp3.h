/* Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later. */
#pragma once
#include <drivers/audio/player.h>
namespace eka2l1::drivers {
    std::unique_ptr<player> new_minimp3_player(audio_driver *driver);
}
