#pragma once

#include <cstdint>
#include <vector>

#include "ray.h" // IWYU pragma: keep

struct AVFrame;

// Z-DON: video frames go to the GPU as their decoded YUV planes (three R8 textures: Y full size, U and V
// at the chroma size) and a fragment shader converts them while drawing. Before, the decode thread
// converted every frame to RGB24 with swscale (a new context per frame) and the main thread uploaded
// 3 bytes per pixel; now it is 1.5 bytes per pixel for 4:2:0 and no CPU conversion.
//
// 8-bit planar YUV only (yuv420p/422p/444p/440p/411p/410p and their yuvj forms); anything else (10-bit,
// NV12, RGB, palette, alpha) keeps the RGB24 path. Colour matrix from the stream: BT.709 and BT.2020 when
// tagged, BT.601 otherwise (what swscale used for every stream before); range: full for yuvj formats or a
// stream tagged full, limited otherwise. The shader reproduces what was drawn before: nearest chroma per
// video pixel (swscale's unscaled conversion), then bilinear filtering of the converted pixels.
// Nothing outside VideoPlayer sees the video texture (no Lua API, screenshot or readback uses it), so no
// RGB copy is made.
//
// ZDON_VIDEO_YUV=0 returns to the RGB24 path. ZDON_VIDEO_YUV_CHECK=1 (dev): every 30th frame the decode
// thread also converts with swscale (the old path, told the stream's matrix and range) and the main
// thread renders the shader's conversion at the video's size, reads it back and logs the per-channel
// difference ("[video yuv check]").
namespace video_yuv {

    struct Format {
        int width = 0;
        int height = 0;
        int chroma_width = 0;
        int chroma_height = 0;
        int chroma_shift_x = 0; // log2 of the chroma subsampling
        int chroma_shift_y = 0;
        int matrix = 601; // 601, 709 or 2020
        bool full_range = false;
    };

    bool enabled();
    bool check_enabled();

    // Decode thread: copies the frame's three planes, without row padding, into `bytes` (Y, then U, then
    // V) and describes them in `fmt`. False when the frame is not 8-bit planar YUV (bytes untouched).
    bool copy_planes(const AVFrame* frame, std::vector<uint8_t>& bytes, Format& fmt);

    // Decode thread, ZDON_VIDEO_YUV_CHECK: the frame as swscale converts it to RGB24 (tight rows).
    void reference_rgb(const AVFrame* frame, std::vector<uint8_t>& rgb);

    // The three plane textures and the conversion shader (main thread).
    class Textures {
    public:
        Textures() = default;
        Textures(const Textures&) = delete;
        Textures& operator=(const Textures&) = delete;
        ~Textures() { unload(); }
        // false when the shader failed to compile (from then on enabled() is false)
        bool upload(const Format& fmt, const uint8_t* bytes);
        bool valid() const { return y.id != 0; }
        int width() const { return y.width; }
        int height() const { return y.height; }
        void draw(ray::Rectangle source, ray::Rectangle dest);
        // ZDON_VIDEO_YUV_CHECK: compares the shader's output for the uploaded frame with `rgb`
        void check(const std::vector<uint8_t>& rgb, int frame_index);
        void unload();

    private:
        ray::Texture2D y{}, u{}, v{};
        Format fmt{};
        ray::Shader shader{};
        int loc_u = -1, loc_v = -1, loc_off = -1, loc_r = -1, loc_g = -1, loc_b = -1, loc_size = -1, loc_shift = -1;
    };

} // namespace video_yuv
