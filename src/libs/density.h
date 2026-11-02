#pragma once

// Z-DON texture density: skin_config `screen.texture_density` = d (integer 1-4, default 1).
// With d > 1:
// - the skin's own PNGs are d x their logical size, and their texture.json numbers (x, y, x2, y2, crop) are
//   in those physical pixels: they load scaled by 1/d (TextureWrapper::load_folder), so every position and
//   size the engine and Lua see is logical, while the source rects sample the full image. Ancestor skins'
//   textures keep their usual scaling. skin_config and animation.json numbers stay logical.
// - each frame renders into a (screen_width * d) x (screen_height * d) target (zdon_density in ray.h), with
//   the usual Camera2D (compute_camera2d then gives zoom d), so get_render_width/height, scissors, the
//   render textures sized off them and Chara3D are physical, and outlined text is rasterised at
//   font_size * d. The target is drawn letterboxed into the window; screenshots and YATAIDON_DUMP_* read
//   the target.
// With d == 1 nothing here runs and the frame draws straight to the window as before.

#include "gfx.h"
#include "math/math_types.h"
#include "raylib_ns.h"

namespace density {
    // the active skin's texture density (1: off)
    int factor();

    // Start of the frame, before its camera is computed: with a factor > 1 (re)allocates the target and
    // binds it (cleared to `clear`); with 1 frees a target left from a previous skin. Returns whether the
    // frame renders into the target.
    bool begin_frame(Color clear);

    // End of the frame's drawing (outside Mode2D): draws the target letterboxed into the window (cleared to
    // `border`). No-op when the frame draws to the window.
    void end_frame(Color border);

    // the finished frame as an image: the target's (physical size) or the window's
    Image capture();

    // a window position (mouse) -> render (target) pixels
    Vec2 window_to_render(Vec2 p);

    // a camera of the target -> the same mapping onto the window
    Camera2D to_window(const Camera2D& cam);
} // namespace density
