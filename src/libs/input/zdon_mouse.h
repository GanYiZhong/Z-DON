#pragma once

// Z-DON: the mouse in menus. A click works everywhere outside a play (and the tutorial and input test screens):
//   left button  = don (confirm), right button = back, wheel = kat left (up) / kat right (down).
// A screen or a skin's script can also mark what it drew as clickable (add_area, Lua tex.click_area): a click there
// walks the selection that many steps (one kat press at a time, so the screen animates and sounds as with a drum)
// and a click on the selected item (0 steps) confirms it. Areas live one frame: whatever is drawn registers again.
// Touch is not the mouse here: the mouse events a touch screen makes are left out (the touch drum has its own).

#include <optional>
#include <string_view>

#include "../gfx.h"

namespace zdon_mouse {

    inline constexpr int kSwallow =
        1 << 20; // an area that takes the click and does nothing (a panel with its own mouse)
    inline constexpr int kBack = kSwallow + 1; // an area that goes back
    // an area whose click is a command for the screen (kCommand + n): take_command() gives n in that frame's update
    inline constexpr int kCommand = kSwallow + 1000;

    // x, y, w, h in the canvas's coordinates (tex.screen_width x tex.screen_height, as textures are drawn);
    // steps: 0 confirm, -n / +n that many kat presses left / right, kSwallow, kBack. A later area wins where they
    // overlap. id (>= 0): the item, the same each frame; a walk to it then looks its steps up again each frame
    // until it is selected (a press the menu drops while it still moves is sent again).
    void add_area(float x, float y, float w, float h, int steps, int id = -1);

    // ZDON_SHOW_CLICK_AREAS=1: this frame's areas tinted, drawn over the screen
    void draw_debug();

    // once a frame, after poll_input_events and before the screen's update: the click against the areas the last frame
    // drew, the wheel, the steps still to send, and the cursor (shown while the mouse moves, hidden after it rests)
    void frame(const Camera2D& camera, std::string_view screen);

    // the screen changed: steps queued for the old one are dropped
    void reset();

    // this frame's clicked command (an area kCommand + n), once; nothing when none
    std::optional<int> take_command();

} // namespace zdon_mouse
