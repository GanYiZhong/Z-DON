#pragma once
#include <algorithm>

#include "../libs/audio/audio.h"
#include "../libs/file/scores.h"
#include "../libs/input/input.h"
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
    // the game transition (VS window, loading card) is an AI one: the skin asks tex.ai_transition()
    void select_song(SongBox* song) override {
        global_data.zdon_ai.requested = true;
        SongSelectScreen::select_song(song);
    }

    // ai_song_select_all.lua: one timer, 100 s on the song list; the course board keeps what is left, at least
    // 30 s (30 s after a time-up on the list); backing out tops it up to 30 s; each time-up sounds don_big
    int diff_select_seconds() override { return std::max(30, select_timer ? select_timer->time() : 0); }
    int options_time_up_extra_seconds() override { return 10; }
    void on_select_time_up() override {
        if (audio.has_sound("ai_don_big")) {
            audio.play_sound("ai_don_big", VolumePreset::SOUND);
        }
    }
    void on_diff_select_cancelled(double current_ms) override {
        if (!select_timer || select_timer->time() <= 30) {
            select_timer = std::make_unique<Timer>(30, current_ms, [this] { list_time_up(); });
        }
    }

    // a time-up on the song list: the song under the cursor, else the cabinet's TimeUpSequence (open the genre,
    // take a song): step through the list to a song; a list without one opens the next genre folder
    int seek_steps = 0;
    int seek_moved = 0;
    double seek_next_ms = 0;
    void list_time_up() {
        set_diff_stage(false);
        on_select_time_up();
        if (state != SongSelectState::BROWSING) {
            return;
        }
        seek_steps = 600;
        seek_moved = 0;
        seek_next_ms = 0;
    }
    void after_select_timers(double now) override {
        if (seek_steps <= 0 || state != SongSelectState::BROWSING || navigator.is_processing ||
            navigator.inline_streaming || now < seek_next_ms) {
            return;
        }
        --seek_steps;
        BaseBox* item = navigator.get_current_item();
        if (navigator.is_song(item)) {
            seek_steps = 0;
            state = player->select_song();
            return;
        }
        const bool genre = navigator.is_directory(item) && item->genre_index >= GenreIndex::JPOP &&
                           item->genre_index <= GenreIndex::NAMCO;
        if (genre && seek_moved >= 60) { // no song in this list: open a genre
            player->select_song();
            seek_moved = 0;
            seek_next_ms = now + 500;
            return;
        }
        navigator.move_right();
        ++seek_moved;
        seek_next_ms = now + 50;
    }
    bool allows_second_player_join() override { return false; }

    // skin_config zdon_ai_diff_first (Green): the cabinet asks for the course first (columns: back, easy .. oni, the
    // recommended one, the last one; footage Tl7g8XXXvAM 1:50), then the song list; the chosen course is the one
    // the course board opens on
    bool diff_stage = false;
    int diff_cursor = 5;
    bool diff_leave = false;
    void set_diff_stage(bool on) {
        diff_stage = on;
        zdon_ai_set_diff_stage(on, diff_cursor);
    }
    void handle_input_browsing(double current_ms) override {
        if (!diff_stage) {
            SongSelectScreen::handle_input_browsing(current_ms);
            return;
        }
        if (is_l_kat_pressed(PlayerNum::P1)) {
            diff_cursor = std::max(0, diff_cursor - 1);
            audio.play_player_sound("kat", PlayerNum::P1, VolumePreset::SOUND);
        } else if (is_r_kat_pressed(PlayerNum::P1)) {
            diff_cursor = std::min(6, diff_cursor + 1);
            audio.play_player_sound("kat", PlayerNum::P1, VolumePreset::SOUND);
        } else if (is_l_don_pressed(PlayerNum::P1) || is_r_don_pressed(PlayerNum::P1)) {
            audio.play_player_sound("don", PlayerNum::P1, VolumePreset::SOUND);
            if (diff_cursor == 0) {
                diff_leave = true;
                return;
            }
            const int last = global_data.last_difficulty[0];
            int course = diff_cursor <= 4 ? diff_cursor - 1 : static_cast<int>(Difficulty::ONI);
            if (diff_cursor == 6 && last >= static_cast<int>(Difficulty::EASY)) {
                course = std::min(last, static_cast<int>(Difficulty::ONI));
            }
            global_data.last_difficulty[0] = course;
            set_diff_stage(false);
            return;
        }
        zdon_ai_set_diff_stage(true, diff_cursor);
    }

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
        select_timer = std::make_unique<Timer>(100, get_current_ms(), [this] { list_time_up(); });
        seek_steps = 0;
        diff_leave = false;
        diff_cursor = 5;
        set_diff_stage(tex.skin_config_by_name.contains("zdon_ai_diff_first"));
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
        if (diff_leave && !next) {
            diff_leave = false;
            set_diff_stage(false);
            return on_screen_end(Screens::ENTRY);
        }
        const double now = get_frame_ms();
        ai_plate.update(now);
        if (ai_chara) {
            ai_chara->update(now);
        }
        return next;
    }
};
