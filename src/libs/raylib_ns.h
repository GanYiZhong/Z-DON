#pragma once

// Z-DON: raylib (and rlgl) inside namespace ray, for Z-DON code that talks to raylib directly: lazy
// texture upload, BC7, the recording read-back, the YUV video path, ... Raylib backend only; the
// project's own types (Texture, Image, RenderTarget, ...) share raylib's layout and convert to them.

#include "gfx.h"
#include "gfx/sdl_compat.h" // before raylib.h: raylib.h must not be the first to pull in <stdarg.h>

namespace ray {
    extern "C" {
#include <raylib.h>
// raylib color macros clash with Color::WHITE etc.
#undef WHITE
#undef BLACK
#undef BLANK
#undef BLUE
#undef GRAY
#undef GREEN
#undef LIGHTGRAY
#undef LIME
#undef ORANGE
#undef RED
#undef SKYBLUE
#undef YELLOW
#include <rlgl.h>
    }
} // namespace ray
