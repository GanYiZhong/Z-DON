#pragma once

#include <chrono>

#include "../platform/platform.h"
#include "fixed_step.h"

inline double get_current_ms() {
    if (fixed_step::on()) {
        return fixed_step::clock_ms.load(std::memory_order_relaxed); // YATAIDON_FIXED_STEP_MS
    }
    return platform_game_time_ms();
}

extern double g_frame_ms;

inline double get_frame_ms() {
    return (g_frame_ms > 0.0) ? g_frame_ms : get_current_ms();
}
