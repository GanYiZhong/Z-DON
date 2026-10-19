#pragma once

#include <chrono>

#include "fixed_step.h"

#ifdef YATAIDON_PLATFORM_IOS
#include "../platform/platform_ios.h"
#endif

inline double get_current_ms() {
    if (fixed_step::on()) {
        return fixed_step::clock_ms.load(std::memory_order_relaxed); // YATAIDON_FIXED_STEP_MS
    }
#ifdef YATAIDON_PLATFORM_IOS
    return ios_game_time_ms();
#else
    using namespace std::chrono;
    auto now = steady_clock::now();
    return duration<double, std::milli>(now.time_since_epoch()).count();
#endif
}

extern double g_frame_ms;

inline double get_frame_ms() {
    return (g_frame_ms > 0.0) ? g_frame_ms : get_current_ms();
}
