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
        const auto& r = global_data.session_data[static_cast<int>(PlayerNum::P1)].result_data;
        GlobalData::ZdonAiSong song;
        song.title = global_data.session_data[static_cast<int>(PlayerNum::P1)].song_title;
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
    // skin_config zdon_ai_bgm_after_announce x: the result BGM starts x ms after the announce (footage 0vjiSUv61i4
    // 4 battles and 0wD5kkcN5WA: Nijiiro 1810 ms with the announce at the screen start; Tl7g8XXXvAM: Green 4700 ms
    // with the announce at the fade, see zdon_ai_announce_at_fade)
    zdon_bgm_pending = false;
    zdon_total_cue = 0;
    if (global_data.zdon_ai.active && audio.has_sound("ai_bgm")) {
        audio.stop_sound("bgm");
        audio.set_sound_loop("ai_bgm", true);
        double after = 0.0;
        if (const auto it = tex.skin_config_by_name.find("zdon_ai_bgm_after_announce");
            it != tex.skin_config_by_name.end()) {
            after = it->second.x;
        }
        double announced = global_data.zdon_ai.announce_ms;
        global_data.zdon_ai.announce_ms = -1.0;
        if (announced < 0.0 && audio.has_sound("aivoice_announce")) {
            audio.play_sound("aivoice_announce", VolumePreset::VOICE);
            announced = get_current_ms();
        }
        if (after > 0.0 && announced >= 0.0 && get_current_ms() < announced + after) {
            zdon_bgm_pending = true;
            zdon_bgm_at_ms = announced + after;
        } else {
            audio.play_sound("ai_bgm", VolumePreset::MUSIC);
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
    if (zdon_bgm_pending && current_time >= zdon_bgm_at_ms) {
        zdon_bgm_pending = false;
        audio.play_sound("ai_bgm", VolumePreset::MUSIC);
    }
    // the credit's play-all page: announce_total, then window_total 1.85 s later (footage 7ziYe-6YQ3U: 0.982 / 2.836)
    if (global_data.zdon_ai.play_all && zdon_play_all_ms > 0) {
        if (zdon_total_cue == 0) {
            zdon_total_cue = 1;
            if (audio.has_sound("aivoice_announce_total")) {
                audio.play_sound("aivoice_announce_total", VolumePreset::VOICE);
            }
        }
        if (zdon_total_cue == 1 && current_time >= zdon_play_all_ms + 1854.0) {
            zdon_total_cue = 2;
            if (audio.has_sound("ai_window_total")) {
                audio.play_sound("ai_window_total", VolumePreset::SOUND);
            }
        }
    }
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

    if (zdon_freeze_exit == 2) { // zdon_end_over_result: the held summary frame goes under the ending curtain
        zdon_freeze_exit = 0;
        global_data.zdon_ai.play_all = false;
        global_data.zdon_ai.active = false;
        global_data.songs_played = 0;
        return on_screen_end(Screens::GAME_OVER);
    }
    if (zdon_freeze_exit == 1) {
        zdon_freeze_exit = 2;
    }

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
            if (quitting) {
                return on_screen_end(credit_end_screen());
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
