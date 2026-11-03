#pragma once
#include <algorithm>

#include "../libs/audio/audio.h"
#include "../libs/file/scores.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/global/chara_3d.h"
#include "../objects/global/nameplate.h"
#include "song_select.h"

// Z-DON AI battle (objects/game/zdon_ai.h): the mode board "AI battle" on the entry screen opens this song
// select (screen name AI_SELECT, the song select's skin); the chosen song is played in GAME_2P against the AI.
// The AI stands on the 2P side: its nameplate ("AI", "Lv.N") and character in the song_select_2p slot.
class AiSongSelectScreen : public SongSelectScreen {
    Nameplate ai_plate;
    std::unique_ptr<Chara3D> ai_chara;

protected:
    Screens get_game_screen_target() override {
        global_data.zdon_ai.requested = true;
        return Screens::GAME_2P;
    }
    bool hides_dan() override { return true; }

    // ai_song_select_all.lua: one timer, 100 s on the song list; the course board keeps what is left, at least
    // 30 s (30 s after a time-up on the list); backing out tops it up to 30 s; each time-up sounds don_big
    int diff_select_seconds() override { return std::max(30, select_timer ? select_timer->time() : 0); }
    void on_select_time_up() override {
        if (audio.has_sound("ai_don_big")) {
            audio.play_sound("ai_don_big", VolumePreset::SOUND);
        }
    }
    void on_diff_select_cancelled(double current_ms) override {
        if (!select_timer || select_timer->time() <= 30) {
            select_timer = std::make_unique<Timer>(30, current_ms, [this] {
                on_select_time_up();
                player->select_song();
            });
        }
    }
    bool allows_second_player_join() override { return false; }

    void draw_extra_players() override {
        if (zdon_ai_skin_arena()) {
            return; // the skin draws the AI (global/ai_battle.lua)
        }
        auto [nx, ny] = tex.nameplate_xy("song_select_2p");
        ai_plate.draw(nx, ny);
        if (ai_chara) {
            auto [cx, cy] = tex.chara_xy("song_select_2p");
            ai_chara->draw(cx, cy, 1.0f);
        }
    }

public:
    AiSongSelectScreen() : SongSelectScreen("song_select") {}

    void on_screen_start() override {
        SongSelectScreen::on_screen_start();
        zdon_ai_condition(); // rolls the session's condition (its tension) on the first visit
        select_timer = std::make_unique<Timer>(100, get_current_ms(), [this] {
            on_select_time_up();
            player->select_song();
        });
        // the AI select's own bgm (bgm_aisenkyoku) when the skin has it (Sounds/song_select/ai/bgm)
        if (audio.has_sound("ai_bgm")) {
            audio.stop_sound("bgm");
            SongBox::bgm_sound = "ai_bgm";
            audio.set_sound_loop("ai_bgm", true);
            audio.play_sound("ai_bgm", VolumePreset::MUSIC);
        }
        const PlayerData pd = zdon_ai_player_data(global_data.config->general.zdon_ai_level);
        ai_plate = Nameplate(pd.username, pd.title, PlayerNum::AI, pd.dan, pd.gold, pd.rainbow, pd.title_bg);
        ai_chara = make_chara_from_player_data(&pd, true);
        ai_chara->set_don_colors(pd.chara_color_1, pd.chara_color_2, pd.chara_color_3);
        ai_chara->set_anim(AnimIndex::DON_SELECT_LOOP);
    }

    std::optional<Screens> update() override {
        auto next = SongSelectScreen::update();
        const double now = get_frame_ms();
        ai_plate.update(now);
        if (ai_chara) {
            ai_chara->update(now);
        }
        return next;
    }
};
