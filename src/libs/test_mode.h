#pragma once

#include <cstdlib>

// Z-DON dev hook: YATAIDON_TEST=1 marks an automated test run. It plays at 1% volume, starts
// minimized (unless the window is hidden), ignores real input (keyboard, controllers, MIDI;
// only YATAIDON_FAKE_KEYS presses get through) and never writes the player's song lists
// (Recently Played, favorites), so a test can't touch the player's data.
inline bool zdon_test_mode() {
    static const bool on = [] {
        const char* v = std::getenv("YATAIDON_TEST");
        return v && *v == '1';
    }();
    return on;
}
