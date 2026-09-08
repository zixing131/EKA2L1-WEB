#pragma once

#include <algorithm>
#include <cmath>

namespace eka2l1::web {
    // Keep deadlines on the requested cadence instead of restarting the period
    // at each RAF callback. Otherwise a 60 Hz limit becomes 48 Hz on 144 Hz RAF.
    class frame_pacer {
        double interval_ms_ = 1000.0 / 60.0;
        double next_frame_ms_ = -1.0;

    public:
        void set_max_fps(int fps) {
            interval_ms_ = 1000.0 / std::clamp(fps, 15, 120);
            next_frame_ms_ = -1.0;
        }

        bool due(double now_ms) {
            if (next_frame_ms_ < 0.0) {
                next_frame_ms_ = now_ms;
            }
            // Small RAF jitter at 60 Hz should not turn one frame into two.
            if (now_ms + 0.5 < next_frame_ms_) {
                return false;
            }
            next_frame_ms_ += interval_ms_;
            if (next_frame_ms_ <= now_ms + 0.5) {
                // A background tab or long task can miss many deadlines. Skip
                // those frames; never accumulate guest work to catch up later.
                next_frame_ms_ += (std::floor((now_ms + 0.5 - next_frame_ms_) / interval_ms_) + 1.0) * interval_ms_;
            }
            return true;
        }
    };
}
