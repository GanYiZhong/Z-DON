#pragma once
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
    bool allows_second_player_join() override { return false; }

    void draw_extra_players() override {
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
