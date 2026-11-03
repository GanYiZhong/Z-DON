#include "density.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <spdlog/spdlog.h>

#include "gfx/texture.h"
#include "raylib_ns.h"
#include "zdon_record.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

namespace density {
    namespace {
        float g_desat_request = 0.0f; // set by the skin during a frame
        float g_desat_apply = 0.0f;   // this frame's (the previous frame's request)
        std::optional<Shader> g_gray;
        bool g_freeze_request = false;
        bool g_freeze_apply = false;
        bool g_freeze_handoff = false;
        RenderTarget g_freeze{}; // the held frame
    } // namespace

    void request_freeze() {
        g_freeze_request = true;
    }

    void freeze_handoff() {
        g_freeze_handoff = g_freeze.id != 0;
    }

    bool freeze_handoff_active() {
        return g_freeze_handoff;
    }

    void freeze_release() {
        g_freeze_handoff = false;
        g_freeze_request = false;
        if (g_freeze.id != 0) {
            unload_render_texture(g_freeze);
            g_freeze = {};
        }
    }

    bool draw_frozen(float w, float h) {
        if (g_freeze.id == 0) {
            return false;
        }
        const Texture& t = g_freeze.texture;
        draw_texture_pro(t, {0.0f, 0.0f, static_cast<float>(t.width), -static_cast<float>(t.height)},
                         {0.0f, 0.0f, w, h}, {0.0f, 0.0f}, 0.0f, Color::WHITE);
        return true;
    }

    void request_desaturate(float amount) {
        g_desat_request = std::clamp(amount, 0.0f, 1.0f);
    }

    int factor() {
        // a recording draws straight into its window (the video's size): the skin's high-density textures are
        // drawn scaled down there, not into a target 4x the video's pixels (a GPU-bound parallel recording)
        static const bool record_full = [] {
            const char* v = std::getenv("ZDON_RECORD_FULL_DENSITY");
            return v && *v == '1';
        }();
        if (zdon_record::active() && !record_full) {
            return 1;
        }
        return std::max(tex.texture_density, 1);
    }

    bool begin_frame(Color clear) {
        g_desat_apply = g_desat_request;
        g_desat_request = 0.0f;
        g_freeze_apply = g_freeze_request;
        g_freeze_request = false;
        const bool special = g_desat_apply > 0.0f || g_freeze_apply;
        const int d = std::max(factor(), special ? 1 : 0); // desaturating or freezing: a screen-size target (d = 1)
        if (d <= 1 && !special) {
            if (zdon_density::target.id != 0) {
                unload_render_texture(zdon_density::target);
                zdon_density::target = {};
            }
            zdon_density::on = false;
            zdon_density::bound = false;
            return false;
        }
        const int w = std::max(tex.screen_width, 1) * d;
        const int h = std::max(tex.screen_height, 1) * d;
        if (zdon_density::target.id == 0 || zdon_density::target.texture.width != w ||
            zdon_density::target.texture.height != h) {
            if (zdon_density::target.id != 0) {
                unload_render_texture(zdon_density::target);
            }
            zdon_density::target = load_render_texture(w, h);
            if (zdon_density::target.id == 0) {
                spdlog::error("texture_density {}: no {}x{} render target, drawing to the window", d, w, h);
                zdon_density::on = false;
                zdon_density::bound = false;
                return false;
            }
            set_texture_filter(zdon_density::target.texture, texture_filter::BILINEAR);
            spdlog::info("texture_density {}: rendering into {}x{}", d, w, h);
        }
        // the target's letterboxed place in the window
        const auto ww = static_cast<float>(std::max(ray::GetRenderWidth(), 1));
        const auto wh = static_cast<float>(std::max(ray::GetRenderHeight(), 1));
        zdon_density::scale = std::min(ww / static_cast<float>(w), wh / static_cast<float>(h));
        zdon_density::off_x = (ww - static_cast<float>(w) * zdon_density::scale) * 0.5f;
        zdon_density::off_y = (wh - static_cast<float>(h) * zdon_density::scale) * 0.5f;

        zdon_density::on = true;
        ray::BeginTextureMode(zdon_density::target);
        zdon_density::bound = true;
        clear_background(clear);
        return true;
    }

    void end_frame(Color border) {
        if (!zdon_density::bound) {
            return;
        }
        ray::EndTextureMode();
        zdon_density::bound = false;
        const Texture& t = zdon_density::target.texture;
        const auto w = static_cast<float>(t.width);
        const auto h = static_cast<float>(t.height);
        if (g_freeze_apply) { // keep this frame: a plain copy into the held target
            if (g_freeze.id == 0 || g_freeze.texture.width != t.width || g_freeze.texture.height != t.height) {
                if (g_freeze.id != 0) {
                    unload_render_texture(g_freeze);
                }
                g_freeze = load_render_texture(t.width, t.height);
                if (g_freeze.id != 0) {
                    set_texture_filter(g_freeze.texture, texture_filter::BILINEAR);
                }
            }
            if (g_freeze.id != 0) {
                ray::BeginTextureMode(g_freeze);
                ray::rlDisableColorBlend();
                draw_texture_pro(t, {0.0f, 0.0f, w, -h}, {0.0f, 0.0f, w, h}, {0.0f, 0.0f}, 0.0f, Color::WHITE);
                ray::rlDrawRenderBatchActive();
                ray::rlEnableColorBlend();
                ray::EndTextureMode();
            }
        }
        clear_background(border);
        // a plain copy: the target's alpha is not blended into the window
        ray::rlDrawRenderBatchActive();
        ray::rlDisableColorBlend();
        const bool gray = g_desat_apply > 0.0f;
        if (gray) {
            if (!g_gray) {
                g_gray = load_shader("shader/dummy.vs", "shader/grayscale.fs");
            }
            const float amount = g_desat_apply;
            set_shader_value(*g_gray, get_shader_location(*g_gray, "amount"), &amount, shader_uniform::FLOAT);
            begin_shader_mode(*g_gray);
        }
        draw_texture_pro(t, {0.0f, 0.0f, w, -h},
                         {zdon_density::off_x, zdon_density::off_y, w * zdon_density::scale, h * zdon_density::scale},
                         {0.0f, 0.0f}, 0.0f, Color::WHITE);
        ray::rlDrawRenderBatchActive();
        if (gray) {
            end_shader_mode();
        }
        ray::rlEnableColorBlend();
    }

    Image capture() {
        if (!zdon_density::on || zdon_density::target.id == 0) {
            return load_image_from_screen();
        }
        Image img = ray::LoadImageFromTexture(zdon_density::target.texture);
        ray::ImageFlipVertical(reinterpret_cast<ray::Image*>(&img));
        ray::ImageFormat(reinterpret_cast<ray::Image*>(&img),
                         ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8); // opaque, like a screen read
        if (g_desat_apply > 0.0f) {                             // the window shows it grayscale: so does the capture
            auto* px = static_cast<unsigned char*>(img.data);
            const float amt = g_desat_apply;
            for (long i = 0, n = static_cast<long>(img.width) * img.height; i < n; ++i, px += 3) {
                const float y = 0.299f * px[0] + 0.587f * px[1] + 0.114f * px[2];
                for (int c = 0; c < 3; ++c) {
                    px[c] = static_cast<unsigned char>(px[c] + (y - px[c]) * amt + 0.5f);
                }
            }
        }
        return img;
    }

    Vec2 window_to_render(Vec2 p) {
        if (!zdon_density::on) {
            return p;
        }
        return {(p.x - zdon_density::off_x) / zdon_density::scale, (p.y - zdon_density::off_y) / zdon_density::scale};
    }

    Camera2D to_window(const Camera2D& cam) {
        if (!zdon_density::on) {
            return cam;
        }
        Camera2D c = cam;
        c.zoom = cam.zoom * zdon_density::scale;
        c.offset = {cam.offset.x * zdon_density::scale + zdon_density::off_x,
                    cam.offset.y * zdon_density::scale + zdon_density::off_y};
        return c;
    }
} // namespace density
