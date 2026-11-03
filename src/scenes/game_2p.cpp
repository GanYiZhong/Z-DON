#include "game_2p.h"

#include <algorithm>

#include "../libs/input/input.h"
#include "../libs/math/animation.h"
#include "../objects/game/zdon_ai.h"

// Z-DON AI battle: leaving goes back to the AI song select
static Screens zdon_song_select() {
    return global_data.zdon_ai.active ? Screens::AI_SELECT : Screens::SONG_SELECT_2P;
}

void Game2PScreen::release_chart_resources() {
    parser_2p.reset();
}

void Game2PScreen::initialize_chart(const fs::path& song) {
    start_delay = (song.extension() == ".osu") ? 0 : start_delay;
    int delay = static_cast<int>(start_delay);
    // Z-DON AI battle (also on a restart): 2P plays 1P's chart
    const bool ai_battle = global_data.zdon_ai.requested || global_data.zdon_ai.active;
    global_data.zdon_ai.requested = false;
    ai_music_cut = false;
    ai_ending_pending = false;
    if (ai_battle) {
        auto& s1 = global_data.session_data[(int)PlayerNum::P1];
        auto& s2 = global_data.session_data[(int)PlayerNum::P2];
        s2.selected_song = s1.selected_song;
        s2.selected_difficulty = s1.selected_difficulty;
        s2.song_hash = s1.song_hash;
    }
    parser = std::make_shared<SongParser>(song, delay, PlayerNum::P1);
    parser_2p = std::make_shared<SongParser>(song, delay, PlayerNum::P2);

    initialize_song_movie();
    set_session_song_info(PlayerNum::P1);
    set_session_song_info(PlayerNum::P2);

    load_song_audio(AudioLoadMode::Sync);

    const auto p1_index = static_cast<int>(PlayerNum::P1);
    const auto p2_index = static_cast<int>(PlayerNum::P2);

    players.push_back(std::make_unique<Player>(parser, PlayerNum::P1,
                                               global_data.session_data[p1_index].selected_difficulty, false,
                                               get_player_modifiers(PlayerNum::P1)));
    Modifiers mods_2p = get_player_modifiers(PlayerNum::P2);
    if (ai_battle) {
        mods_2p = get_player_modifiers(PlayerNum::P1); // the same speed / options as the player
        mods_2p.auto_play = false;
    }
    players.push_back(std::make_unique<Player>(parser_2p, PlayerNum::P2,
                                               global_data.session_data[p2_index].selected_difficulty, true, mods_2p));
    if (ai_battle) {
        // the arcade tunes its AI by the course's star level (ai_tuninginfo levels 1..11)
        const int diff = global_data.session_data[p1_index].selected_difficulty;
        const auto course = parser->metadata.course_data.find(diff);
        const int level =
            zdon_ai_tuning_level(course != parser->metadata.course_data.end() ? course->second.level : 5.0);
        players[1]->enable_zdon_ai(level);
        players[1]->apply_replay_appearance(zdon_ai_player_data(level));
        zdon_ai_battle_begin(level);
        global_data.zdon_ai.star = course != parser->metadata.course_data.end() ? course->second.level : 0.0;
    }

    players[0]->kusudama_partner = players[1].get();
    players[1]->kusudama_partner = players[0].get();
    set_players_audio_end();

    reset_song_clock();
}

std::optional<Screens> Game2PScreen::update() {
    if (auto init = Screen::update()) {
        return init;
    }

    double current_time = get_current_ms();
    if (pause_menu.active()) {
        return update_pause_menu(zdon_song_select()); // Z-DON: pause menu (pause_menu.h)
    }
    clock.update(current_time);
    transition->update(current_time);
    update_gameplay(current_time);
    if (global_data.zdon_ai.active && players.size() == 2) {
        zdon_ai_battle_update(clock.position(), *players[0], *players[1]);
        if (global_data.zdon_ai.finished && !ai_music_cut && song_music &&
            tex.skin_config_by_name.contains("zdon_ai_cutin_first")) {
            audio.stop_sound(
                *song_music); // the cabinet cuts the song when the battle ends (the emblem and voice follow)
            ai_music_cut = true;
        }
    }
    result_transition.update(current_time);

    const double end_time = std::max(players[0]->end_time, players[1]->end_time);

    if (result_transition.is_finished && !audio.is_sound_playing("result_transition")) {
        return on_screen_end(Screens::RESULT_2P);
    }

    if (clock.position() >= end_time) {
        const double transition_delay =
            global_data.config->general.fast_transitions ? kFastResultTransitionDelayMs : 8533.34;

        end_song(end_time, transition_delay, BackgroundNotification::Disabled);
    }

    if (check_key_pressed(global_data.config->keys.restart_key)) {
        restart_song();
    }
    if (check_key_pressed(global_data.config->keys.back_key)) {
        if (can_pause()) {
            open_pause_menu();
            return std::nullopt;
        }
        return on_screen_end(zdon_song_select());
    }
    const bool lost_focus = focus_lost();
    if (can_pause()) {
        if (check_key_pressed(global_data.config->keys.pause_key) || lost_focus) {
            open_pause_menu();
        }
    } else if (!global_data.config->general.pause_menu && check_key_pressed(global_data.config->keys.pause_key)) {
        toggle_pause(); // pause_menu = false: the old plain pause
    }

    return std::nullopt;
}
