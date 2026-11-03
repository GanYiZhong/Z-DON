#include "result_2p.h"

#include <algorithm>

void Result2PScreen::on_screen_start() {
    ResultScreen::on_screen_start();
    allow_retry = false;
    if (loading_graphic.has_value()) {
        unload_texture(loading_graphic.value());
        loading_graphic.reset();
    }
    background.emplace(PlayerNum::TWO_PLAYER, tex.screen_width);
    fade_in.emplace(PlayerNum::TWO_PLAYER);
    player_1.emplace(PlayerNum::P1, true, false);
    player_2.emplace(PlayerNum::P2, true, true);
    // Z-DON AI battle: this song joins the credit's (the play-all summary)
    if (global_data.zdon_ai.active && global_data.zdon_ai.finished) {
        const auto& b = global_data.zdon_ai;
        const auto& r = global_data.session_data[(int)PlayerNum::P1].result_data;
        GlobalData::ZdonAiSong song;
        song.title = global_data.session_data[(int)PlayerNum::P1].song_title;
        song.score = r.score;
        song.good = r.good;
        song.ok = r.ok;
        song.bad = r.bad;
        song.cleared = r.cleared;
        song.won = b.won;
        song.section_score = b.section_score;
        song.section_crown = b.crowns;
        global_data.zdon_ai_credit.push_back(std::move(song));
    }
    // Z-DON AI battle (AiResultMain.lua): bgm_airesult and the announce, when the skin has them
    if (global_data.zdon_ai.active && audio.has_sound("ai_bgm")) {
        audio.stop_sound("bgm");
        audio.set_sound_loop("ai_bgm", true);
        audio.play_sound("ai_bgm", VolumePreset::MUSIC);
        if (audio.has_sound("aivoice_announce")) {
            audio.play_sound("aivoice_announce", VolumePreset::VOICE);
        }
    }
}

double Result2PScreen::reveal_end_ms() {
    double a = player_1.has_value() ? player_1->reveal_end_ms() : 0.0;
    double b = player_2.has_value() ? player_2->reveal_end_ms() : 0.0;
    if (a == 0 || b == 0) {
        return 0.0;
    }
    return std::max(a, b);
}

std::optional<Screens> Result2PScreen::update() {
    Screen::update();
    double current_time = get_current_ms();
    if (fade_in.has_value()) {
        fade_in->update(current_time);
    }
    if (player_1.has_value()) {
        player_1->update(current_time, fade_in.has_value() && fade_in->is_finished(), skipped_time > 0);
    }
    if (player_2.has_value()) {
        player_2->update(current_time, fade_in.has_value() && fade_in->is_finished(), skipped_time > 0);
    }

    update_input_and_timeout(current_time);

    if (fade_out) {
        fade_out->update(current_time);
        if (fade_out->is_finished) {
            // Z-DON AI battle: the credit's last song (limits.song_limit) ends the credit after play-all
            if (global_data.zdon_ai.play_all) {
                global_data.zdon_ai.play_all = false;
                global_data.zdon_ai.active = false;
                global_data.songs_played = 0;
                return on_screen_end(Screens::GAME_OVER);
            }
            global_data.returned_from_result = true;
            // Z-DON AI battle: back to the AI song select
            return on_screen_end(global_data.zdon_ai.active ? Screens::AI_SELECT : Screens::SONG_SELECT_2P);
        }
    }
    return std::nullopt;
}

void Result2PScreen::draw() {
    if (background.has_value()) {
        background->draw();
    }
    draw_song_info();
    if (player_1.has_value()) {
        player_1->draw();
    }
    if (player_2.has_value()) {
        player_2->draw();
    }
    draw_overlay();
}
