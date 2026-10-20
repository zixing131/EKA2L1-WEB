/* Copyright (c) 2026 EKA2L1 Team. GPL-3.0-or-later. */
#pragma once
#include <drivers/audio/dsp.h>
namespace eka2l1::drivers {
    std::unique_ptr<dsp_output_stream> new_minimp3_dsp_output(audio_driver *driver);
}
