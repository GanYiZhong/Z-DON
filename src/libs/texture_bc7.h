#pragma once

#include <cstdint>
#include <filesystem>

#include "gfx.h" // IWYU pragma: keep
#include "raylib_ns.h"

// Z-DON: skin textures as BC7 (GL_COMPRESSED_RGBA_BPTC_UNORM): a quarter of the video memory of RGBA8, and of the
// copy the GL driver keeps in system memory. An image is compressed when every visible pixel comes back within
// kMaxError of its decoded value, or the whole image within kMaxMse (PSNR 45 dB, for painted gradients); the rest
// stay RGBA8.
//
// Encoding is slow (a 4K skin is tens of CPU seconds), so it never runs on a load: a decoded image the cache does not
// know is uploaded as it is and encoded by a Background job, and the next load takes the cached blocks.
// cache/bc7/textures.bin: the blocks (deflated) by the 64-bit hash of the decoded pixels, so any skin, child skin or
// edited file finds its own entry; read at start, rewritten (.tmp + rename) when a batch of encodes is done.
// [video] zdon_texture_compression (default true) or ZDON_BC7=0/1 turns it off / on; it is off without GL 4.2 or
// GL_ARB_texture_compression_bptc.
namespace texture_bc7 {
    // main thread, once the GL context exists
    void init(bool configured);
    bool enabled();

    // A skin file's size and last write, taken before it is read (zero when it could not be)
    struct FileStamp {
        uint64_t size = 0;
        int64_t mtime = 0;
    };
    FileStamp stamp(const std::filesystem::path& path);

    // The file's BC7 image without decoding it: a load compressed this file (same path, size and last write)
    // from the cache before. False: decode it and hand it to compress. cache/bc7/files.bin keeps the files.
    bool cached_image(const std::filesystem::path& path, const FileStamp& st, Image& out);

    // An image just decoded from a skin file (any thread): RGBA8 / RGB8 becomes BC7 when the cache has its blocks;
    // anything else stays as it is. With the file's path and stamp, the next load of it finds it by cached_image.
    void compress(Image& image, const std::filesystem::path& path = {}, const FileStamp& st = {});

    // GetImageAlphaBorder(image, 0) of a BC7 image: the border of its pixels before compression
    Rect alpha_border(const Image& image);

    // saves what was encoded; queued encodes are dropped
    void shutdown();
} // namespace texture_bc7
