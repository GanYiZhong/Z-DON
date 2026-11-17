#include "zdon_lane3d.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../../libs/file/config.h"
#include "../../libs/fixed_step.h"
#include "../../libs/gfx.h"
#include "../../libs/gfx/camera_utils.h"
#include "../../libs/gfx/texture.h"
#include "../../libs/global_data.h"
#include "../../libs/input/input.h"
#include "../../libs/time.h"
#include "../../libs/zdon_record.h"
#include "../global/zdon_hud.h"
#include "../settings/option_box.h"

namespace zdon_lane3d {

    namespace {
        struct View {
            float yaw = 0.0f;   // degrees, + turns the right side away
            float pitch = 0.0f; // degrees, + tips the top away (a highway)
            float distance = 1.0f;
        };

        View user_target; // where the mouse sent the view
        View user;        // eased towards it
        double last_ease_ms = -1.0;
        View script_from; // a script's move: from, to, over
        View script_to;
        double script_start_ms = 0.0;
        double script_len_ms = 0.0;
        float pivot_x = -1.0f; // virtual; < 0: the screen's middle
        float pivot_y = -1.0f;

        bool pressing = false;
        bool dragging = false; // past the dead zone
        Vec2 press_at{};
        Vec2 last_mouse{};
        int preset = 0;

        constexpr size_t LAYER_COUNT = static_cast<size_t>(Layer::COUNT);
        std::array<RenderTarget, LAYER_COUNT> targets{};
        std::array<bool, LAYER_COUNT> drawn_this_frame{};
        bool in_frame = false;
        Layer current_layer = Layer::ALL;
        Camera2D frame_cam{};
        View frame_view{};
        double flat_since_ms = -1.0;

        // each layer's depth in half screen heights, + towards the viewer (the notes' plane is the pivot's)
        // (the hit circle's layer shares the notes' plane, so a note meets its circle at any angle)
        constexpr std::array<float, LAYER_COUNT> DEPTH{-0.55f, 0.0f, 0.09f, 0.09f, 0.17f, 0.24f, 0.31f, 0.38f, 0.46f};
        // how lit each layer is (255 = as drawn): the far ones recede
        constexpr std::array<unsigned char, LAYER_COUNT> LIGHT{185, 235, 255, 255, 255, 255, 255, 245, 255};
        // the shadow each layer casts on what is behind it (alpha); its offset grows with the gap behind it
        constexpr std::array<unsigned char, LAYER_COUNT> SHADOW{0, 110, 0, 0, 70, 130, 80, 0, 0};
        constexpr float SHADOW_DX = 0.15f;    // x the gap
        constexpr float SHADOW_DY = -0.22f;   // x the gap (light from the top left)
        constexpr float SHADOW_SOFT = 0.012f; // the widest of its rings
        // the notes are coins: their face on the notes' plane, their body reaching back towards the lane, drawn as
        // slices of the notes' layer, darker towards the back
        constexpr int NOTE_SLICES = 12;
        constexpr float NOTE_DEPTH = 1.0f;        // of the gap between the lane and the notes
        constexpr float NOTE_SIDE_FRONT = 175.0f; // without the silhouette shader: the slices darkened
        constexpr float NOTE_SIDE_BACK = 105.0f;
        // with it: a solid side, lighter just behind the face (a rim) and darker towards the back
        constexpr Color NOTE_RIM{232, 222, 205, 255};
        constexpr Color NOTE_SIDE_NEAR{96, 52, 44, 255}; // darker than the lane's edge, so a side reads apart
        constexpr Color NOTE_SIDE_FAR{52, 30, 28, 255};
        // the lane is a board: a side wall behind it, its thickness a share of the gap behind the lane
        constexpr int LANE_SLICES = 6;
        constexpr float LANE_THICK = 0.14f;
        constexpr Color LANE_EDGE{128, 84, 62, 255};
        constexpr Color LANE_SIDE_NEAR{120, 70, 56, 255};
        constexpr Color LANE_SIDE_FAR{70, 40, 36, 255};

        // a vignette over the turned view: transparent in the middle, darker towards the edges and corners (it also
        // hides the backdrop at the frame's edges). Made once.
        Texture vignette{};
        const Texture& vignette_texture() {
            if (vignette.id != 0) {
                return vignette;
            }
            constexpr int VW = 256;
            constexpr int VH = 144;
            Image img = gen_image_color(VW, VH, Color{0, 0, 0, 0});
            auto* px = static_cast<unsigned char*>(img.data);
            for (int y = 0; y < VH; y++) {
                for (int x = 0; x < VW; x++) {
                    const float dx = ((static_cast<float>(x) + 0.5f) / VW * 2.0f) - 1.0f;
                    const float dy = ((static_cast<float>(y) + 0.5f) / VH * 2.0f) - 1.0f;
                    const float r = std::sqrt((dx * dx * 0.8f) + (dy * dy * 1.1f));
                    const float t = std::clamp((r - 0.55f) / 0.75f, 0.0f, 1.0f);
                    const float a = t * t * (3.0f - (2.0f * t)) * 0.25f;
                    px[(((y * VW) + x) * 4) + 3] = static_cast<unsigned char>(a * 255.0f);
                }
            }
            vignette = load_texture_from_image(img);
            unload_image(img);
            set_texture_filter(vignette, texture_filter::BILINEAR);
            return vignette;
        }

        Color mix(Color a, Color b, float t, float shade) {
            auto ch = [&](unsigned char x, unsigned char y) {
                const float v = (static_cast<float>(x) + ((static_cast<float>(y) - static_cast<float>(x)) * t)) * shade;
                return static_cast<unsigned char>(std::clamp(v, 0.0f, 255.0f));
            };
            return Color{ch(a.r, b.r), ch(a.g, b.g), ch(a.b, b.b), 255};
        }
        // how much a layer turns with the view: the HUD and the overlay turn less, so their text stays readable
        constexpr std::array<float, LAYER_COUNT> TURN{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.4f, 0.8f, 1.0f, 0.5f};

        constexpr float MIN_YAW = -55.0f;
        constexpr float MAX_YAW = 55.0f;
        constexpr float MIN_PITCH = -25.0f;
        constexpr float MAX_PITCH = 65.0f;
        constexpr float DEG_PER_PX = 0.22f;
        constexpr float DEAD_ZONE_PX = 4.0f;
        constexpr float MIN_DIST = 0.4f;
        constexpr float MAX_DIST = 3.0f;
        constexpr float FOVY = 30.0f;
        constexpr double EASE_MS = 70.0; // the mouse's view follows with this time constant

        constexpr std::array<View, 4> PRESETS{View{0.0f, 0.0f, 1.0f}, View{0.0f, 18.0f, 1.0f}, View{0.0f, 26.0f, 1.12f},
                                              View{-18.0f, 14.0f, 1.15f}};
        constexpr std::array<const char*, 4> PRESET_NAMES{"3D: flat", "3D: tilt", "3D: highway", "3D: angle"};

        View clamp_view(View v) {
            return {std::clamp(v.yaw, MIN_YAW, MAX_YAW), std::clamp(v.pitch, MIN_PITCH, MAX_PITCH),
                    std::clamp(v.distance, MIN_DIST, MAX_DIST)};
        }

        View script_now() {
            if (script_len_ms <= 0.0) {
                return script_to;
            }
            const double t = std::clamp((get_current_ms() - script_start_ms) / script_len_ms, 0.0, 1.0);
            const auto e = static_cast<float>(t * t * (3.0 - (2.0 * t))); // ease in and out
            return {script_from.yaw + ((script_to.yaw - script_from.yaw) * e),
                    script_from.pitch + ((script_to.pitch - script_from.pitch) * e),
                    script_from.distance + ((script_to.distance - script_from.distance) * e)};
        }

        View current() {
            const View s = script_now();
            return clamp_view({s.yaw + user.yaw, s.pitch + user.pitch, s.distance * user.distance});
        }

        bool is_flat(const View& v) {
            return std::abs(v.yaw) < 0.01f && std::abs(v.pitch) < 0.01f && std::abs(v.distance - 1.0f) < 0.0001f;
        }

        // the mouse's view eases to its target, frame rate independent; a settled view snaps (flat is exact)
        void ease_user() {
            const double now = get_current_ms();
            const double dt = last_ease_ms < 0.0 ? 1000.0 : std::clamp(now - last_ease_ms, 0.0, 250.0);
            last_ease_ms = now;
            const auto k = static_cast<float>(1.0 - std::exp(-dt / EASE_MS));
            user.yaw += (user_target.yaw - user.yaw) * k;
            user.pitch += (user_target.pitch - user.pitch) * k;
            user.distance += (user_target.distance - user.distance) * k;
            if (std::abs(user.yaw - user_target.yaw) < 0.005f && std::abs(user.pitch - user_target.pitch) < 0.005f &&
                std::abs(user.distance - user_target.distance) < 0.00005f) {
                user = user_target;
            }
        }

        void release_targets() {
            for (RenderTarget& t : targets) {
                if (t.id != 0) {
                    unload_render_texture(t);
                    t = {};
                }
            }
        }

        bool ensure_target(RenderTarget& t, int w, int h) {
            if (t.id != 0 && t.texture.width == w && t.texture.height == h) {
                return true;
            }
            if (t.id != 0) {
                unload_render_texture(t);
            }
            t = load_render_texture(w, h);
            if (t.id == 0) {
                return false;
            }
            set_texture_filter(t.texture, texture_filter::BILINEAR);
            return true;
        }
    } // namespace

    bool enabled() {
        return global_data.config && global_data.config->general.zdon_lane3d;
    }

    bool layered() {
        return in_frame;
    }

    void reset() {
        user = user_target = {};
        last_ease_ms = get_current_ms() - 16.0; // the starting view eases in, not snaps
        script_from = script_to = {};
        script_len_ms = 0.0;
        pressing = dragging = false;
        pivot_x = pivot_y = -1.0f;
        // the view each song starts in ([general] zdon_lane3d_preset), eased from flat
        preset = global_data.config ? std::clamp(global_data.config->general.zdon_lane3d_preset, 0, 3) : 0;
        user_target = PRESETS[static_cast<size_t>(preset)];
        release_targets(); // allocated again by the first turned frame
        flat_since_ms = -1.0;
        // dev hook ZDON_LANE3D_VIEW="yaw,pitch[,distance]": a fixed view for screenshots
        if (const char* view = std::getenv("ZDON_LANE3D_VIEW"); view && *view) {
            float yaw = 0.0f;
            float pitch = 0.0f;
            float distance = 1.0f;
            if (std::sscanf(view, "%f,%f,%f", &yaw, &pitch, &distance) >= 2) {
                set_script_view(yaw, pitch, distance, 0.0);
            }
        }
    }

    void release() {
        release_targets();
    }

    void show_hint_once() {
        if (!enabled() || global_data.config->general.zdon_lane3d_hint_shown || zdon_record::active() ||
            fixed_step::on()) {
            return;
        }
        global_data.config->general.zdon_lane3d_hint_shown = true;
        save_config(*global_data.config);
        zhud::toast(settings_builtin_text("lane3d_hint"));
    }

    void set_pivot(float virtual_x, float virtual_y) {
        pivot_x = virtual_x;
        pivot_y = virtual_y;
    }

    void set_script_view(float yaw_deg, float pitch_deg, float distance, double over_ms) {
        script_from = script_now();
        script_to = clamp_view({yaw_deg, pitch_deg, distance});
        script_start_ms = get_current_ms();
        script_len_ms = std::max(over_ms, 0.0);
    }

    void update_mouse() {
        if (!enabled()) {
            return;
        }
        ease_user();
        // a recording or a fixed-step run frames only what scripts and ZDON_LANE3D_VIEW say, never the mouse
        if (zdon_record::active() || fixed_step::on()) {
            return;
        }
        const Vec2 pos = mouse_position();
        if (mouse_down(SDL_BUTTON_LEFT)) {
            if (!pressing) {
                pressing = true;
                press_at = pos;
                last_mouse = pos;
            }
            if (!dragging && std::hypot(pos.x - press_at.x, pos.y - press_at.y) > DEAD_ZONE_PX) {
                dragging = true; // a click is not a turn
            }
            if (dragging) {
                user_target.yaw += (pos.x - last_mouse.x) * DEG_PER_PX;
                user_target.pitch -= (pos.y - last_mouse.y) * DEG_PER_PX;
                user_target = clamp_view(user_target);
            }
            last_mouse = pos;
        } else {
            pressing = dragging = false;
        }
        if (const float wheel = mouse_wheel(); wheel != 0.0f) {
            user_target.distance = std::clamp(user_target.distance * std::pow(0.9f, wheel), MIN_DIST, MAX_DIST);
        }
        if (mouse_pressed(SDL_BUTTON_MIDDLE)) {
            preset = (preset + 1) % static_cast<int>(PRESETS.size());
            user_target = PRESETS[static_cast<size_t>(preset)];
            zhud::toast(PRESET_NAMES[static_cast<size_t>(preset)]);
        }
        if (mouse_pressed(SDL_BUTTON_RIGHT)) {
            user_target = {}; // eases back to flat
            preset = 0;
        }
    }

    bool draws(Layer layer) {
        return current_layer == Layer::ALL || current_layer == layer;
    }

    bool begin_frame() {
        if (!enabled()) {
            return false;
        }
        frame_view = current();
        if (is_flat(frame_view)) {
            // flat for a while: give the targets' memory back
            const double now = get_current_ms();
            if (flat_since_ms < 0.0) {
                flat_since_ms = now;
            } else if (now - flat_since_ms > 5000.0 && targets[0].id != 0) {
                release_targets();
            }
            return false;
        }
        flat_since_ms = -1.0;
        const int w = std::max(get_render_width(), 1);
        const int h = std::max(get_render_height(), 1);
        for (RenderTarget& t : targets) {
            if (!ensure_target(t, w, h)) {
                release_targets();
                return false;
            }
        }
        // the frame's own camera into targets of the frame's size: each layer lands on the frame's pixels
        frame_cam = compute_camera2d(tex.screen_width, tex.screen_height);
        drawn_this_frame.fill(false);
        in_frame = true;
        return true;
    }

    void begin_layer(Layer layer) {
        if (!in_frame || layer >= Layer::COUNT) {
            return;
        }
        const auto i = static_cast<size_t>(layer);
        end_mode_2d();
        end_blend_mode();
        begin_texture_mode(targets[i]);
        clear_background(Color::BLANK);
        set_additive_keeps_alpha(true); // a glow over empty pixels stays a glow once composited
        set_separate_alpha_blend();
        begin_blend_mode(blend_mode::CUSTOM_SEPARATE);
        begin_mode_2d(frame_cam);
        current_layer = layer;
        drawn_this_frame[i] = true;
    }

    void end_layer() {
        if (current_layer == Layer::ALL) {
            return;
        }
        current_layer = Layer::ALL;
        end_mode_2d();
        end_blend_mode();
        set_additive_keeps_alpha(false);
        end_texture_mode();
        begin_blend_mode(blend_mode::CUSTOM_SEPARATE);
        begin_mode_2d(frame_cam);
    }

    void end_frame() {
        if (!in_frame) {
            return;
        }
        end_layer();
        in_frame = false;
        end_mode_2d();
        end_blend_mode();

        // In half screen heights, y up, the frame's middle at the origin. The camera looks straight down -z from
        // where the notes' plane exactly fills the view; a plane d nearer is drawn (D - d) / D as large, so the flat
        // view puts every layer's pixels back where they were. The view turns about the judge circle on the notes'
        // plane, and the camera moves along the line through it, so that point stays put on the screen.
        const auto w = static_cast<float>(targets[0].texture.width);
        const auto h = static_cast<float>(targets[0].texture.height);
        const float unit = h * 0.5f;
        const float half_w = (w * 0.5f) / unit;
        const float rest_dist = 1.0f / std::tan(FOVY * 0.5f * deg2rad);
        const View v = frame_view;
        const float notes_z = DEPTH[static_cast<size_t>(Layer::NOTES)];

        auto to_px_x = [&](float vx) { return ((vx - frame_cam.target.x) * frame_cam.zoom) + frame_cam.offset.x; };
        auto to_px_y = [&](float vy) { return ((vy - frame_cam.target.y) * frame_cam.zoom) + frame_cam.offset.y; };
        const float px_virtual = pivot_x >= 0.0f ? pivot_x : static_cast<float>(tex.screen_width) * 0.5f;
        const float py_virtual = pivot_y >= 0.0f ? pivot_y : static_cast<float>(tex.screen_height) * 0.5f;
        // the pivot where the notes' plane shows it at rest: (x, y) on screen scaled back to that plane's depth
        const float notes_s = (rest_dist - notes_z) / rest_dist;
        const Vec3 pivot{((to_px_x(px_virtual) - (w * 0.5f)) / unit) * notes_s,
                         (((h * 0.5f) - to_px_y(py_virtual)) / unit) * notes_s, notes_z};

        // how turned the view is, 0..1: the layers lie close and fully lit when barely turned and spread apart,
        // with the far ones dimmed and the shadows deepening, as it turns
        const float turned_raw = std::clamp(
            (std::abs(v.yaw) / 25.0f) + (std::abs(v.pitch) / 30.0f) + (std::abs(v.distance - 1.0f) * 2.0f), 0.0f, 1.0f);
        const float turned = turned_raw * turned_raw * (3.0f - (2.0f * turned_raw));
        const float spread = 0.35f + (0.85f * turned);
        std::array<float, LAYER_COUNT> depth{};
        for (size_t i = 0; i < LAYER_COUNT; i++) {
            depth[i] = notes_z + ((DEPTH[i] - notes_z) * spread);
        }

        struct Turn {
            float cy, sy, cp, sp;
        };
        auto turn_of = [&](float f) {
            return Turn{std::cos(v.yaw * f * deg2rad), std::sin(v.yaw * f * deg2rad), std::cos(v.pitch * f * deg2rad),
                        std::sin(v.pitch * f * deg2rad)};
        };
        auto place = [&](const Turn& t, float x, float y, float z) {
            // about the pivot: pitch (x axis), then yaw (y axis)
            const float vx = x - pivot.x;
            const float vy = y - pivot.y;
            const float vz = z - pivot.z;
            const float y1 = (vy * t.cp) + (vz * t.sp);
            const float z1 = (-vy * t.sp) + (vz * t.cp);
            const float x2 = (vx * t.cy) + (z1 * t.sy);
            const float z2 = (-vx * t.sy) + (z1 * t.cy);
            return Vec3{x2 + pivot.x, y1 + pivot.y, z2 + pivot.z};
        };
        // a plane lit by a light from the top left front: turned towards it a little brighter, away from it darker
        auto shade_of = [&](size_t i) {
            const Turn t = turn_of(TURN[i]);
            const float nx = t.cp * t.sy;
            const float ny = t.sp;
            const float nz = t.cp * t.cy;
            constexpr float lx = -0.30f;
            constexpr float ly = 0.45f;
            constexpr float lz = 0.84f; // about unit length
            const float facing = (nx * lx) + (ny * ly) + (nz * lz);
            return std::clamp(1.0f + (0.9f * (facing - lz)), 0.78f, 1.0f);
        };
        auto quad = [&](size_t i, float dx, float dy, float dz, float grow, Color tint) {
            const Turn t = turn_of(TURN[i]);
            const float z = depth[i] + dz;
            const float s = (rest_dist - depth[i]) / rest_dist;
            const float hw = (half_w * s) + grow;
            const float hh = s + grow;
            const Vec3 corners[4] = {place(t, -hw + dx, hh + dy, z), place(t, hw + dx, hh + dy, z),
                                     place(t, hw + dx, -hh + dy, z), place(t, -hw + dx, -hh + dy, z)};
            static constexpr Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; // targets are stored upside down
            return draw_texture_quad_3d(targets[i].texture, corners, uvs, tint);
        };

        // zooming moves the camera along the line from the rest camera through the pivot
        const Vec3 rest_cam{0.0f, 0.0f, rest_dist};
        Camera3D cam{};
        cam.position = {pivot.x + ((rest_cam.x - pivot.x) * v.distance),
                        pivot.y + ((rest_cam.y - pivot.y) * v.distance),
                        pivot.z + ((rest_cam.z - pivot.z) * v.distance)};
        cam.target = {cam.position.x, cam.position.y, cam.position.z - 1.0f};
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.fovy = FOVY;
        cam.projection = camera_projection::PERSPECTIVE;

        // tilted planes are minified: mipmaps keep thin lines and text from shimmering
        // (barely turned or zoomed in, nothing is minified: no mipmaps, and the targets sample as drawn)
        const bool minified = std::abs(v.yaw) > 12.0f || std::abs(v.pitch) > 15.0f || v.distance > 1.08f;
        for (size_t i = 0; i < LAYER_COUNT; i++) {
            if (drawn_this_frame[i] && minified) {
                gen_render_texture_mipmaps(targets[i]);
            } else if (drawn_this_frame[i]) {
                set_texture_filter(targets[i].texture, texture_filter::BILINEAR);
            }
        }
        begin_blend_mode(blend_mode::ALPHA_PREMULTIPLY); // the targets hold premultiplied colour
        // what the turned layers leave uncovered: the background, flat and dark, as a backdrop
        if (const auto bg = static_cast<size_t>(Layer::BACKGROUND); drawn_this_frame[bg]) {
            draw_texture_pro(targets[bg].texture, {0, 0, w, -h}, {0, 0, w, h}, {0, 0}, 0.0f, Color{92, 58, 52, 255});
        }
        begin_mode_3d(cam);
        begin_quads_3d();
        bool flat_fallback = false;
        float behind = depth[0]; // the depth of the nearest layer drawn so far: what a shadow falls on
        for (size_t i = 0; i < LAYER_COUNT && !flat_fallback; i++) {
            if (!drawn_this_frame[i]) {
                continue;
            }
            // its shadow first, on the layer behind: its own shape in a dark warm tone, offset by the gap between
            // them (a light from the top left), in three widening rings for a soft edge
            if (SHADOW[i] > 0 && i > 0 && turned > 0.0f) {
                const float gap = std::max(depth[i] - behind, 0.0f);
                const auto a = static_cast<unsigned char>(static_cast<float>(SHADOW[i]) * turned / 3.0f);
                for (int ring = 0; ring < 3; ring++) {
                    const float r = static_cast<float>(ring) / 2.0f;
                    quad(i, gap * SHADOW_DX, gap * SHADOW_DY, (behind - depth[i]) + 0.002f, SHADOW_SOFT * r,
                         Color{20, 8, 16, a});
                }
            }
            // the lane's side wall, behind the board
            if (i == static_cast<size_t>(Layer::LANE) && turned > 0.0f) {
                const float thick = LANE_THICK * turned;
                const float sh = shade_of(i);
                const bool solid = begin_silhouette_mode();
                for (int slice = LANE_SLICES; slice >= 1; slice--) {
                    const float f = static_cast<float>(slice) / static_cast<float>(LANE_SLICES);
                    const Color c = !solid       ? Color{60, 40, 36, 255}
                                    : slice == 1 ? mix(LANE_EDGE, LANE_EDGE, 0.0f, sh)
                                                 : mix(LANE_SIDE_NEAR, LANE_SIDE_FAR, f, sh);
                    quad(i, 0.0f, 0.0f, -thick * f, 0.0f, c);
                }
                if (solid) {
                    end_silhouette_mode();
                }
            }
            // the notes' bodies go behind the hit circle's layer, their faces after it: a solid side with a light rim
            // just behind each face
            if (const auto notes = static_cast<size_t>(Layer::NOTES);
                i == static_cast<size_t>(Layer::TARGET) && drawn_this_frame[notes] && turned > 0.0f) {
                const float body = (depth[notes] - depth[static_cast<size_t>(Layer::LANE)]) * NOTE_DEPTH * turned;
                const float sh = shade_of(notes);
                const bool solid = begin_silhouette_mode();
                // about one slice per two pixels of body on screen, 2 to NOTE_SLICES
                const int slices = std::clamp(static_cast<int>(std::ceil(body * unit / 2.0f)), 2, NOTE_SLICES);
                for (int slice = slices; slice >= 1; slice--) {
                    const float f = static_cast<float>(slice) / static_cast<float>(slices);
                    Color c;
                    if (!solid) {
                        const auto l =
                            static_cast<unsigned char>(NOTE_SIDE_FRONT + ((NOTE_SIDE_BACK - NOTE_SIDE_FRONT) * f));
                        c = Color{l, l, l, 255};
                    } else if (slice == 1) {
                        c = mix(NOTE_RIM, NOTE_RIM, 0.0f, sh);
                    } else {
                        c = mix(NOTE_SIDE_NEAR, NOTE_SIDE_FAR, f, sh);
                    }
                    quad(notes, 0.0f, 0.0f, -body * f, 0.0f, c);
                }
                if (solid) {
                    end_silhouette_mode();
                }
            }
            // the background grows as the view turns, so it still fills the frame
            const float grow = i == 0 ? 0.7f * turned : 0.0f;
            const float lit = (255.0f - ((255.0f - static_cast<float>(LIGHT[i])) * turned)) * shade_of(i);
            const auto l = static_cast<unsigned char>(std::clamp(lit, 0.0f, 255.0f));
            if (!quad(i, 0.0f, 0.0f, 0.0f, grow, Color{l, l, l, 255})) {
                flat_fallback = true;
            }
            behind = depth[i];
        }
        end_quads_3d();
        end_mode_3d();
        if (turned > 0.0f) { // black with alpha: premultiplied, it darkens what is under it
            const Texture& vg = vignette_texture();
            const auto va = static_cast<unsigned char>(255.0f * turned);
            draw_texture_pro(vg, {0, 0, static_cast<float>(vg.width), static_cast<float>(vg.height)}, {0, 0, w, h},
                             {0, 0}, 0.0f, Color{255, 255, 255, va});
        }
        if (flat_fallback) { // no 3D quad on this backend: the layers as they were
            for (size_t i = 0; i < LAYER_COUNT; i++) {
                if (drawn_this_frame[i]) {
                    draw_texture_pro(targets[i].texture, {0, 0, w, -h}, {0, 0, w, h}, {0, 0}, 0.0f, Color::WHITE);
                }
            }
        }
        end_blend_mode();
        begin_blend_mode(blend_mode::CUSTOM_SEPARATE);
        begin_mode_2d(frame_cam);
    }

} // namespace zdon_lane3d
