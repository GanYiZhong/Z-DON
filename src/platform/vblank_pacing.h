#pragma once

// Z-DON: vblank-locked frame pacing for the uncapped, no-vsync mode (Windows).
//
// Uncapped without vsync, the frame the compositor picks at each refresh started at a different
// phase every time, so the content step between shown frames jittered by up to a frame (measured:
// std 1.17 ms on a 120 Hz screen). With pacing, frames start on a grid locked to the display's
// vblank (read from DWM): one frame starts a fixed lead before every vblank (the one that is
// shown; the lead follows the measured frame time), and before it extra frames about 2 ms apart,
// as many as are expected to finish in time. Input is still handled at frame times, so fewer
// frames mean coarser input steps (at 165 Hz with ~2.5 ms frames: about one frame per refresh).
// The frame clock of a frame that woke on time is its grid time, so the shown frames advance by
// exactly one refresh period.
//
// Only with vsync off and no target_fps; on by default. ZDON_VBLANK_PACING=0 turns it off,
// =<n> allows at most n frames per refresh (unset or =auto: automatic).
// ZDON_PACING_LOG=<file> writes a per-frame CSV (frame clock, wake, present end, vblank grid).

#ifdef _WIN32
namespace vblank_pacing {
    void init(bool vsync, double target_fps);
    bool active();
    // the frame clock for a frame that starts at `now_ms` (get_current_ms timebase)
    double frame_ms(double now_ms);
    // after present: sleeps until the next frame's grid time
    void wait_next();
} // namespace vblank_pacing
#else
namespace vblank_pacing {
    inline void init(bool, double) {}
    inline bool active() {
        return false;
    }
    inline double frame_ms(double now_ms) {
        return now_ms;
    }
    inline void wait_next() {}
} // namespace vblank_pacing
#endif
