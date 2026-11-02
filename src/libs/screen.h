#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

#include "audio/audio.h"
#include "gfx/texture.h"
#include "global_data.h"
#include "readahead.h"
#include "time.h"

// One axis of the virtual -> window mapping: dim/win_dim are the skin's and the window's size along the axis,
// stretch is the camera's h_scale or v_scale, cam_offset the camera offset along it
inline int virtual_to_screen_axis(float v, float dim, float win_dim, float stretch, float cam_offset) {
    int win_w = get_render_width();
    int win_h = get_render_height();
    float scale = std::min((float)win_w / tex.screen_width, (float)win_h / tex.screen_height);
    float effective_zoom = scale * global_data.camera.zoom;
    float zoom_off = (dim * scale * (global_data.camera.zoom - 1.0f)) * 0.5f;
    float stretch_off = (dim * scale * (stretch - 1.0f)) * 0.5f;
    float offset = (win_dim - dim * scale) * 0.5f - zoom_off - stretch_off + cam_offset * scale;
    return static_cast<int>(v * effective_zoom + offset);
}

inline int virtual_to_screen_x(float virtual_x) {
    if (tex.screen_width <= 0 || tex.screen_height <= 0) {
        return 0;
    }
    return virtual_to_screen_axis(virtual_x, tex.screen_width, (float)get_render_width(), global_data.camera.h_scale,
                                  global_data.camera.offset.x);
}

inline int virtual_to_screen_y(float virtual_y) {
    if (tex.screen_width <= 0 || tex.screen_height <= 0) {
        return 0;
    }
    return virtual_to_screen_axis(virtual_y, tex.screen_height, (float)get_render_height(), global_data.camera.v_scale,
                                  global_data.camera.offset.y);
}

enum class Screens {
    TITLE,
    ENTRY,
    SONG_SELECT,
    GAME,
    GAME_2P,
    RESULT,
    RESULT_2P,
    SONG_SELECT_2P,
    DAN_SELECT,
    GAME_DAN,
    DAN_RESULT,
    PRACTICE_SELECT,
    GAME_PRACTICE,
    AI_SELECT,
    AI_GAME,
    SETTINGS,
    LOADING,
    INPUT_CALI,
    GAME_OVER,
    INPUT_TEST,
    RETRY_GAME,
    ONBOARDING,
    TUTORIAL,
    RECORD_SELECT // Z-DON: the recording mode's song select (song_select_record.h)
};

inline std::string screens_to_string(Screens screen) {
    static const std::array<std::string, 24> names = {
        "TITLE",         "ENTRY",          "SONG_SELECT", "GAME",       "GAME_2P",    "RESULT",
        "RESULT_2P",     "SONG_SELECT_2P", "DAN_SELECT",  "GAME_DAN",   "DAN_RESULT", "PRACTICE_SELECT",
        "GAME_PRACTICE", "AI_SELECT",      "AI_GAME",     "SETTINGS",   "LOADING",    "INPUT_CALI",
        "GAME_OVER",     "INPUT_TEST",     "RETRY_GAME",  "ONBOARDING", "TUTORIAL",   "RECORD_SELECT"};
    int idx = static_cast<int>(screen);
    if (idx < 0 || static_cast<size_t>(idx) >= names.size()) {
        return "UNKNOWN";
    }
    return names[idx];
}

template<>
struct fmt::formatter<Screens> : fmt::formatter<std::string> {
    auto format(Screens screen, fmt::format_context& ctx) const {
        return fmt::formatter<std::string>::format(screens_to_string(screen), ctx);
    }
};

class Screen {

public:
    bool screen_init{false};
    std::string screen_name;
    Screen(std::string name) : screen_name(std::move(name)) {}

    virtual ~Screen() = default;

    // Scene identity is independent of its shared skin name and login seat.
    virtual bool two_player_audio() const { return false; }

    virtual void on_screen_start() {
        audio.set_two_player_mode(two_player_audio());
        tex.load_screen_textures(screen_name);
        audio.load_screen_sounds(screen_name);
    }

    virtual Screens on_screen_end(Screens next_screen) {
        screen_init = false;
        spdlog::info("{} ended, transitioning to {} screen", screen_name, next_screen);

        unload_screen_resources();

        return next_screen;
    }

    virtual std::optional<Screens> update() { return _do_screen_start(); }

    virtual void draw() {
        // Base implementation does nothing, override in derived classes
    }

    void _do_draw() {
        if (screen_init) {
            draw();
        }
    }

protected:
    // Z-DON predecode: what the screen most likely shown next needs, decoded while this one idles (the textures
    // after the result screens come from the prefill, texture_lazy.h; a song's screens start theirs when a song
    // is chosen and after its last note)
    static void predecode_next(const std::string& screen) {
        if (screen == "title") {
            tex.start_predecode("song_select", {"entry"}); // the entry screen comes in between
        } else if (screen == "entry") {
            audio.predecode_screen_sounds("song_select");
        } else if (screen == "result") {
            audio.predecode_screen_sounds("song_select");
        } else if (screen == "dan_result") {
            audio.predecode_screen_sounds("dan_select");
        }
    }

    static void unload_screen_resources() {
        audio.unload_all_sounds();
        audio.unload_all_music();
        tex.unload_textures();
    }

    std::optional<Screens> _do_screen_start() {
        if (!screen_init) {
            screen_init = true;
            try {
                readahead::cancel(); // the screen reads its own files now: don't compete with it for the disk
                on_screen_start();
                tex.end_predecode(); // Z-DON: the predecoded images the screen did not need
                g_frame_ms = get_current_ms();
                spdlog::info("{} initialized", screen_name);
                predecode_next(screen_name);
                readahead::after_screen(screen_name);
            } catch (const std::exception& e) {
                spdlog::critical("{} failed to initialize: {}", screen_name, e.what());
                tex.end_predecode();
                screen_init = false;
                unload_screen_resources();
                return Screens::SONG_SELECT;
            } catch (...) {
                spdlog::critical("{} failed to initialize: unknown exception", screen_name);
                tex.end_predecode();
                screen_init = false;
                unload_screen_resources();
                return Screens::SONG_SELECT;
            }
        }
        return std::nullopt;
    }
};
