#pragma once

#include <cstddef>
#include <cstdint>

#include "gfx.h" // IWYU pragma: keep
#include "raylib_ns.h"

// Z-DON: a PNG decoder for the common case of skin art (8-bit gray / gray+alpha / RGB / RGBA /
// palette, not interlaced), inflating with zlib instead of stb_image's own inflate. The output is
// byte-for-byte what raylib's LoadImageFromMemory(".png") gives (same channel count and pixel
// format, same malloc'd buffer that UnloadImage frees). Anything else -- 16-bit or sub-byte
// depths, interlacing, unknown critical chunks, any decode error -- returns false and the caller
// uses raylib, so odd files decode exactly as before.
// YATAIDON_FASTPNG=0 turns it off (enabled() is false, and builds without zlib never use it).
namespace png_fast {

    bool enabled();

    // true and image set when decoded; false (image untouched) to fall back to raylib
    bool decode(const uint8_t* data, size_t size, Image& image);

} // namespace png_fast
