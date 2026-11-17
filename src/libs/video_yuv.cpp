#include "video_yuv.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <spdlog/spdlog.h>
#include <string>

#include "raylib_ns.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

#ifndef __EMSCRIPTEN__
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace video_yuv {
    namespace {

#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
        constexpr const char* kHeader = "#version 300 es\nprecision highp float;\n";
#else
        constexpr const char* kHeader = "#version 330\n";
#endif
        // raylib's default vertex shader feeds fragTexCoord / fragColor; texture0 is the Y plane.
        // What the RGB path drew, computed here: each video pixel converted with the chroma sample that
        // covers it (swscale's unscaled conversion: nearest chroma) and clamped, then the four around the
        // fragment blended bilinearly with the wrap-around the RGB texture had (GL_REPEAT).
        constexpr const char* kFragment = R"(
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform sampler2D texU;
uniform sampler2D texV;
uniform vec3 yuvOffset;
uniform vec3 rowR;
uniform vec3 rowG;
uniform vec3 rowB;
uniform ivec2 lumaSize;
uniform ivec2 chromaShift;
vec3 rgbAt(ivec2 p) {
    p = ivec2(mod(vec2(p), vec2(lumaSize)));
    ivec2 c = ivec2(p.x >> chromaShift.x, p.y >> chromaShift.y);
    vec3 yuv = vec3(texelFetch(texture0, p, 0).r, texelFetch(texU, c, 0).r, texelFetch(texV, c, 0).r) - yuvOffset;
    return clamp(vec3(dot(rowR, yuv), dot(rowG, yuv), dot(rowB, yuv)), 0.0, 1.0);
}
void main() {
    vec2 t = fragTexCoord * vec2(lumaSize) - 0.5;
    vec2 b = floor(t);
    vec2 f = t - b;
    ivec2 p = ivec2(b);
    vec3 top = mix(rgbAt(p), rgbAt(p + ivec2(1, 0)), f.x);
    vec3 bottom = mix(rgbAt(p + ivec2(0, 1)), rgbAt(p + ivec2(1, 1)), f.x);
    finalColor = vec4(mix(top, bottom, f.y), 1.0) * fragColor;
}
)";

        std::atomic<bool> shader_failed{false}; // then every video takes the RGB24 path

        bool env_on(const char* name, bool def) {
            const char* v = std::getenv(name);
            return v && *v ? *v != '0' : def;
        }

        // one plane as an R8 texture; the shader reads it with texelFetch, so filter and wrap don't matter
        Texture plane_texture(const uint8_t* data, int w, int h) {
            Image img{};
            img.data = const_cast<uint8_t*>(data); // raylib copies on load
            img.width = w;
            img.height = h;
            img.mipmaps = 1;
            img.format = ray::PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
            return ray::LoadTextureFromImage(img);
        }

    } // namespace

    bool enabled() {
        static const bool on = env_on("ZDON_VIDEO_YUV", true);
        return on && !shader_failed.load(std::memory_order_relaxed);
    }

    bool check_enabled() {
        static const bool on = env_on("ZDON_VIDEO_YUV_CHECK", false);
        return on;
    }

#ifndef __EMSCRIPTEN__
    bool copy_planes(const AVFrame* frame, std::vector<uint8_t>& bytes, Format& fmt) {
        const auto pix = static_cast<AVPixelFormat>(frame->format);
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pix);
        constexpr uint64_t kReject = AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL |
                                     AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_BITSTREAM;
        if (!d || (d->flags & kReject) || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || d->nb_components != 3) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            const AVComponentDescriptor& c = d->comp[i];
            if (c.plane != i || c.depth != 8 || c.step != 1 || c.shift != 0 || c.offset != 0) {
                return false;
            }
        }
        const int w = frame->width, h = frame->height;
        const int cw = AV_CEIL_RSHIFT(w, d->log2_chroma_w), ch = AV_CEIL_RSHIFT(h, d->log2_chroma_h);
        if (w <= 0 || h <= 0) {
            return false;
        }
        bytes.resize(static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch);
        uint8_t* out = bytes.data();
        const int widths[3] = {w, cw, cw}, heights[3] = {h, ch, ch};
        for (int p = 0; p < 3; ++p) {
            const uint8_t* src = frame->data[p];
            for (int row = 0; row < heights[p]; ++row) {
                std::memcpy(out, src + static_cast<ptrdiff_t>(row) * frame->linesize[p], widths[p]);
                out += widths[p];
            }
        }
        fmt.width = w;
        fmt.height = h;
        fmt.chroma_width = cw;
        fmt.chroma_height = ch;
        fmt.chroma_shift_x = d->log2_chroma_w;
        fmt.chroma_shift_y = d->log2_chroma_h;
        switch (frame->colorspace) {
        case AVCOL_SPC_BT709:
            fmt.matrix = 709;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            fmt.matrix = 2020;
            break;
        default:
            fmt.matrix = 601;
            break;
        }
        fmt.full_range = frame->color_range == AVCOL_RANGE_JPEG || pix == AV_PIX_FMT_YUVJ420P ||
                         pix == AV_PIX_FMT_YUVJ422P || pix == AV_PIX_FMT_YUVJ444P || pix == AV_PIX_FMT_YUVJ440P ||
                         pix == AV_PIX_FMT_YUVJ411P;
        return true;
    }

    void reference_rgb(const AVFrame* frame, std::vector<uint8_t>& rgb) {
        const int w = frame->width, h = frame->height;
        SwsContext* sws = sws_getContext(w, h, static_cast<AVPixelFormat>(frame->format), w, h, AV_PIX_FMT_RGB24,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            rgb.clear();
            return;
        }
        // as the old path, but with the stream's matrix and range (the old path used BT.601 limited for all)
        int* inv = nullptr;
        int* table = nullptr;
        int src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
        sws_getColorspaceDetails(sws, &inv, &src_range, &table, &dst_range, &brightness, &contrast, &saturation);
        Format fmt;
        std::vector<uint8_t> scratch;
        if (copy_planes(frame, scratch, fmt)) {
            const int cs = fmt.matrix == 709 ? SWS_CS_ITU709 : fmt.matrix == 2020 ? SWS_CS_BT2020 : SWS_CS_ITU601;
            sws_setColorspaceDetails(sws, sws_getCoefficients(cs), fmt.full_range ? 1 : 0, table, dst_range, brightness,
                                     contrast, saturation);
        }
        rgb.resize(static_cast<size_t>(w) * h * 3);
        uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {w * 3, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst, dst_stride);
        sws_freeContext(sws);
    }
#else
    bool copy_planes(const AVFrame*, std::vector<uint8_t>&, Format&) {
        return false;
    }
    void reference_rgb(const AVFrame*, std::vector<uint8_t>& rgb) {
        rgb.clear();
    }
#endif

    bool Textures::upload(const Format& f, const uint8_t* bytes) {
        const uint8_t* pu = bytes + static_cast<size_t>(f.width) * f.height;
        const uint8_t* pv = pu + static_cast<size_t>(f.chroma_width) * f.chroma_height;
        if (valid() && (f.width != fmt.width || f.height != fmt.height || f.chroma_width != fmt.chroma_width ||
                        f.chroma_height != fmt.chroma_height)) {
            unload();
        }
        if (!shader.id) {
            const std::string fs = std::string(kHeader) + kFragment;
            shader = ray::LoadShaderFromMemory(nullptr, fs.c_str());
            if (shader.id == 0 || shader.id == ray::rlGetShaderIdDefault()) {
                spdlog::error("Video: the YUV shader did not compile, videos use the RGB path");
                shader = ray::Shader{};
                shader_failed = true;
                return false;
            }
            loc_u = ray::GetShaderLocation(shader, "texU");
            loc_v = ray::GetShaderLocation(shader, "texV");
            loc_off = ray::GetShaderLocation(shader, "yuvOffset");
            loc_r = ray::GetShaderLocation(shader, "rowR");
            loc_g = ray::GetShaderLocation(shader, "rowG");
            loc_b = ray::GetShaderLocation(shader, "rowB");
            loc_size = ray::GetShaderLocation(shader, "lumaSize");
            loc_shift = ray::GetShaderLocation(shader, "chromaShift");
        }
        if (!valid()) {
            y = plane_texture(bytes, f.width, f.height);
            u = plane_texture(pu, f.chroma_width, f.chroma_height);
            v = plane_texture(pv, f.chroma_width, f.chroma_height);
        } else {
            ray::UpdateTexture(y, bytes);
            ray::UpdateTexture(u, pu);
            ray::UpdateTexture(v, pv);
        }
        fmt = f;
        return true;
    }

    void Textures::draw(Rect source, Rect dest) {
        if (!valid() || !shader.id) {
            return;
        }
        // Y' = (Y - 16/255) * 255/219, C' = (C - 128/255) * 255/224 limited; Y' = Y, C' = C - 128/255 full
        const double kr = fmt.matrix == 709 ? 0.2126 : fmt.matrix == 2020 ? 0.2627 : 0.299;
        const double kb = fmt.matrix == 709 ? 0.0722 : fmt.matrix == 2020 ? 0.0593 : 0.114;
        const double kg = 1.0 - kr - kb;
        const double ys = fmt.full_range ? 1.0 : 255.0 / 219.0;
        const double cs = fmt.full_range ? 1.0 : 255.0 / 224.0;
        const float off[3] = {fmt.full_range ? 0.0f : 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
        const float row_r[3] = {static_cast<float>(ys), 0.0f, static_cast<float>(cs * 2.0 * (1.0 - kr))};
        const float row_g[3] = {static_cast<float>(ys), static_cast<float>(-cs * 2.0 * kb * (1.0 - kb) / kg),
                                static_cast<float>(-cs * 2.0 * kr * (1.0 - kr) / kg)};
        const float row_b[3] = {static_cast<float>(ys), static_cast<float>(cs * 2.0 * (1.0 - kb)), 0.0f};
        ray::BeginShaderMode(shader);
        ray::SetShaderValueTexture(shader, loc_u, u);
        ray::SetShaderValueTexture(shader, loc_v, v);
        ray::SetShaderValue(shader, loc_off, off, ray::SHADER_UNIFORM_VEC3);
        ray::SetShaderValue(shader, loc_r, row_r, ray::SHADER_UNIFORM_VEC3);
        ray::SetShaderValue(shader, loc_g, row_g, ray::SHADER_UNIFORM_VEC3);
        ray::SetShaderValue(shader, loc_b, row_b, ray::SHADER_UNIFORM_VEC3);
        const int size[2] = {fmt.width, fmt.height};
        const int shift[2] = {fmt.chroma_shift_x, fmt.chroma_shift_y};
        ray::SetShaderValue(shader, loc_size, size, ray::SHADER_UNIFORM_IVEC2);
        ray::SetShaderValue(shader, loc_shift, shift, ray::SHADER_UNIFORM_IVEC2);
        ray::DrawTexturePro(y, source, dest, {0.0f, 0.0f}, 0.0f, Color::WHITE);
        ray::EndShaderMode();
    }

    void Textures::check(const std::vector<uint8_t>& rgb, int frame_index) {
        const int w = fmt.width, h = fmt.height;
        if (!valid() || rgb.size() != static_cast<size_t>(w) * h * 3) {
            return;
        }
        // called mid-frame (VideoPlayer::update runs inside BeginMode2D): EndTextureMode resets both matrices
        const Matrix proj = ray::rlGetMatrixProjection();
        const Matrix view = ray::rlGetMatrixModelview();
        RenderTarget rt = ray::LoadRenderTexture(w, h);
        begin_texture_mode(rt); // paired with end_texture_mode below (the target stack)
        ray::ClearBackground(Color::BLACK);
        draw({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)},
             {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        end_texture_mode(); // back to the texture-density target, if the frame draws into one
        ray::rlSetMatrixProjection(proj);
        ray::rlSetMatrixModelview(view);
        Image img = ray::LoadImageFromTexture(rt.texture);
        ray::UnloadRenderTexture(rt);
        ray::ImageFlipVertical(reinterpret_cast<ray::Image*>(&img));
        ray::ImageFormat(reinterpret_cast<ray::Image*>(&img), ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8);
        const auto* got = static_cast<const uint8_t*>(img.data);
        int max_d[3] = {0, 0, 0};
        double sum[3] = {0, 0, 0};
        size_t over2 = 0;
        const size_t n = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < n; ++i) {
            int m = 0;
            for (int c = 0; c < 3; ++c) {
                const int dd = std::abs(static_cast<int>(got[i * 3 + c]) - static_cast<int>(rgb[i * 3 + c]));
                max_d[c] = std::max(max_d[c], dd);
                sum[c] += dd;
                m = std::max(m, dd);
            }
            over2 += m > 2;
        }
        ray::UnloadImage(img);
        spdlog::info("[video yuv check] frame {} {}x{} BT.{} {}: max |d| R {} G {} B {}, mean {:.3f} {:.3f} {:.3f}, "
                     "pixels off by >2: {:.3f}%",
                     frame_index, w, h, fmt.matrix, fmt.full_range ? "full" : "limited", max_d[0], max_d[1], max_d[2],
                     sum[0] / n, sum[1] / n, sum[2] / n, 100.0 * over2 / n);
    }

    void Textures::unload() {
        for (Texture* t : {&y, &u, &v}) {
            if (t->id != 0) {
                ray::UnloadTexture(*t);
            }
            *t = Texture{};
        }
        if (shader.id != 0) {
            ray::UnloadShader(shader);
            shader = ray::Shader{};
        }
        fmt = Format{};
    }

} // namespace video_yuv
