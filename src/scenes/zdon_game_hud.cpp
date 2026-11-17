// Z-DON: GameScreen's own additions, kept out of game.cpp (an upstream file) so merges stay small

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../libs/judge.h"
#include "../objects/game/zdon_lane3d.h"
#include "game.h"

// Z-DON 3D view: it turns about the judge circle (the lanes' middle with two)
void GameScreen::zdon_set_lane3d_pivot() {
    if (!zdon_lane3d::enabled()) {
        return;
    }
    const float s = tex.screen_scale;
    const float half_lane = 88.0f * s;
    zdon_lane3d::set_pivot(JudgePos::X, // the judge circle's middle, as the hit error bar
                           players.size() == 2
                               ? (((184.0f * s) + tex.get_skin_config(SC::GAME_P2_Y)->y) * 0.5f) + half_lane
                               : (184.0f * s) + half_lane);
}

// "PB +1,230" at the right end of each lane (a skin's "zdon_ghost": {x, y} = the 1P chip's top left)
// Z-DON hit error bar ([general] zdon_hit_error_bar): under each lane's judge circle, the good and ok windows as
// bands and a tick for each of the last hits, left early, right late, fading over two seconds
void GameScreen::zdon_draw_hit_error_bar() const {
    // practice shows each note's own offset, and its progress bar sits where this bar would
    if (!global_data.config->general.zdon_hit_error_bar || global_data.current_screen == "GAME_PRACTICE") {
        return;
    }
    const float s = tex.screen_scale;
    const double now = get_current_ms();
    auto rect = [](float x, float y, float w, float h, Color c) {
        draw_rectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h), c);
    };
    for (size_t i = 0; i < players.size() && i < 2; i++) {
        const Player& p = *players[i];
        // auto play records no hits (unless ZDON_RECORD_AUTO_OFFSETS, for tests): an empty bar says nothing
        static const bool record_auto = std::getenv("ZDON_RECORD_AUTO_OFFSETS") != nullptr;
        if (p.is_zdon_ai() || (p.get_modifiers().auto_play && !record_auto)) {
            continue;
        }
        const int pn = static_cast<int>(p.player_num);
        if (pn < 0 || pn >= static_cast<int>(global_data.session_data.size())) {
            continue;
        }
        const auto& session = global_data.session_data[static_cast<size_t>(pn)];
        const bool easy = session.selected_difficulty <= static_cast<int>(Difficulty::NORMAL);
        const float good = easy ? Timing::GOOD_EASY : Timing::GOOD;
        const float ok = easy ? Timing::OK_EASY : Timing::OK;
        const float lane_y = i == 0 ? 184.0f * s : tex.get_skin_config(SC::GAME_P2_Y)->y;
        const float cx = JudgePos::X; // the judge circle's middle
        // under the captions; with two lanes, 1P's goes above its lane (2P's lane starts right under it), as the
        // ghost chip does
        const bool above_lane = i == 0 && players.size() == 2;
        const float cy = above_lane ? lane_y - (19.0f * s) : lane_y + (198.0f * s);
        const float half = 92.0f * s; // the ok window's half width
        const float px_per_ms = half / ok;
        const float bh = 14.0f * s;
        rect(cx - half - (3 * s), cy - (bh / 2) - (3 * s), (half * 2) + (6 * s), bh + (6 * s), Color{0, 0, 0, 150});
        rect(cx - half, cy - (bh / 2), half * 2, bh, Color{255, 200, 80, 200});
        rect(cx - (good * px_per_ms), cy - (bh / 2), good * px_per_ms * 2, bh, Color{90, 200, 255, 220});
        rect(cx - s, cy - (bh / 2) - (4 * s), 2 * s, bh + (8 * s), Color{255, 255, 255, 220});
        const auto& live = session.zdon_live;
        double sum = 0.0;
        int count = 0;
        for (int k = 0; k < live.recent_hits_count; k++) {
            const auto& [offset, at] = live.recent_hits[static_cast<size_t>(k)];
            const double age = now - at;
            if (age < 0.0 || age > 2000.0) {
                continue;
            }
            sum += offset;
            count++;
            const float a = 1.0f - static_cast<float>(age / 2000.0);
            const float o = std::clamp(offset, -ok, ok);
            const Color c = std::abs(offset) <= good ? Color{120, 220, 255, 255}
                            : std::abs(offset) <= ok ? Color{255, 220, 110, 255}
                                                     : Color{255, 90, 80, 255};
            rect(cx + (o * px_per_ms) - (1.5f * s), cy - (bh / 2) - (6 * s), 3 * s, bh + (12 * s),
                 Color{c.r, c.g, c.b, static_cast<unsigned char>(255.0f * a)});
        }
        if (count > 0) { // the recent hits' mean, as a marker over the bar
            const float mx = cx + (std::clamp(static_cast<float>(sum / count), -ok, ok) * px_per_ms);
            rect(mx - (3 * s), cy - (bh / 2) - (14 * s), 6 * s, 6 * s, Color{255, 255, 255, 230});
        }
    }
}
