#include "density.h"

#include <algorithm>
#include <cstdlib>
#include <spdlog/spdlog.h>

#include "texture.h"
#include "zdon_record.h"

// raylib.h (inside namespace ray) already defined RL_MATRIX_TYPE, so rlgl.h declares its functions with this
using ray::Matrix;
#include <rlgl.h>

namespace density {
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
        const int d = factor();
        if (d <= 1) {
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
        clear_background(border);
        const ray::Texture2D& t = zdon_density::target.texture;
        const auto w = static_cast<float>(t.width);
        const auto h = static_cast<float>(t.height);
        // a plain copy: the target's alpha is not blended into the window
        rlDrawRenderBatchActive();
        rlDisableColorBlend();
        draw_texture_pro(t, {0.0f, 0.0f, w, -h},
                         {zdon_density::off_x, zdon_density::off_y, w * zdon_density::scale, h * zdon_density::scale},
                         {0.0f, 0.0f}, 0.0f, Color::WHITE);
        rlDrawRenderBatchActive();
        rlEnableColorBlend();
    }

    ray::Image capture() {
        if (!zdon_density::on || zdon_density::target.id == 0) {
            return load_image_from_screen();
        }
        ray::Image img = ray::LoadImageFromTexture(zdon_density::target.texture);
        ray::ImageFlipVertical(&img);
        ray::ImageFormat(&img, ray::PIXELFORMAT_UNCOMPRESSED_R8G8B8); // opaque, like a screen read
        return img;
    }

    Vec2 window_to_render(Vec2 p) {
        if (!zdon_density::on) {
            return p;
        }
        return {(p.x - zdon_density::off_x) / zdon_density::scale, (p.y - zdon_density::off_y) / zdon_density::scale};
    }

    ray::Camera2D to_window(const ray::Camera2D& cam) {
        if (!zdon_density::on) {
            return cam;
        }
        ray::Camera2D c = cam;
        c.zoom = cam.zoom * zdon_density::scale;
        c.offset = {cam.offset.x * zdon_density::scale + zdon_density::off_x,
                    cam.offset.y * zdon_density::scale + zdon_density::off_y};
        return c;
    }
} // namespace density
