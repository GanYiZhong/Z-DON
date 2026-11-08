#include "game_2p.h"

#include <algorithm>
#include <iterator>
#include <set>

#include "../libs/input/input.h"
#include "../libs/math/animation.h"
#include "../objects/game/zdon_ai.h"

void Game2PScreen::release_chart_resources() {
    parser_2p.reset();
}

static std::set<long long> kusudama_times(SongParser& parser, int difficulty) {
    std::set<long long> times;
    auto add = [&](const NoteList& list) {
        for (const Note& note : list.notes) {
            if (note.type == NoteType::KUSUDAMA) {
                times.insert(std::llround(note.hit_ms));
            }
        }
    };
    auto [notes, branch_m, branch_e, branch_n] = parser.notes_to_position(difficulty);
    add(notes);
    for (const auto* branch : {&branch_m, &branch_e, &branch_n}) {
        for (const NoteList& section : *branch) {
            add(section);
        }
    }
    return times;
}

void Game2PScreen::initialize_chart(const fs::path& song) {
    start_delay = (song.extension() == ".osu") ? 0 : start_delay;
    // Z-DON AI battle (also on a restart): 2P plays 1P's chart
    const bool ai_battle = global_data.zdon_ai.requested || global_data.zdon_ai.active;
    // skin_config zdon_ai_end_delay (Green): the cabinet's song starts 3 s after the battle screen appears
    if (ai_battle && start_delay > 0 && zdon_ai_end_delay_on()) {
        start_delay = 3000.0;
        // skin_config zdon_ai_start_delay_ms (Green): the song starts 3.37 s after the arena reveal (footage
        // Tl7g8XXXvAM: reveal 136.63 s, song 140.0 s)
        if (const auto it = tex.skin_config_by_name.find("zdon_ai_start_delay_ms");
            it != tex.skin_config_by_name.end() && it->second.x > 0.0f) {
            start_delay = it->second.x;
        }
    }
    int delay = static_cast<int>(start_delay);
    global_data.zdon_ai.requested = false;
    ai_music_cut = false;
    ai_ending_pending = false;
    if (ai_battle) {
        auto& s1 = global_data.session_data[static_cast<int>(PlayerNum::P1)];
        auto& s2 = global_data.session_data[static_cast<int>(PlayerNum::P2)];
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

    // a kusudama is shared, so one only the other lane lacks plays as a balloon
    const int diff_1 = global_data.session_data[p1_index].selected_difficulty;
    const int diff_2 = global_data.session_data[p2_index].selected_difficulty;
    std::set<long long> shared;
    std::ranges::set_intersection(kusudama_times(*parser, diff_1), kusudama_times(*parser_2p, diff_2),
                                  std::inserter(shared, shared.begin()));
    Modifiers mods_1 = get_player_modifiers(PlayerNum::P1);
    Modifiers mods_2 = get_player_modifiers(PlayerNum::P2);
    if (ai_battle) {
        mods_2 = mods_1; // the same speed / options as the player
        mods_2.auto_play = false;
    }
    mods_1.shared_kusudamas = shared;
    mods_2.shared_kusudamas = shared;

    players.push_back(std::make_unique<Player>(parser, PlayerNum::P1, diff_1, false, mods_1));
    players.push_back(std::make_unique<Player>(parser_2p, PlayerNum::P2, diff_2, true, mods_2));
    if (ai_battle) {
        // the arcade tunes its AI by the course's star level (ai_tuninginfo levels 1..11)
        const auto course = parser->metadata.course_data.find(diff_1);
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
        return update_pause_menu(select_screen()); // Z-DON: pause menu (pause_menu.h)
    }
    clock.update(current_time);
    transition->update(current_time);
    update_gameplay(current_time);
    if (global_data.zdon_ai.active && players.size() == 2) {
        zdon_ai_battle_update(clock.position(), *players[0], *players[1]);
        if (global_data.zdon_ai.finished && get_current_ms() >= global_data.zdon_ai.finished_at_ms && !ai_music_cut &&
            song_music && (tex.skin_config_by_name.contains("zdon_ai_cutin_first") || zdon_ai_end_delay_on())) {
            audio.stop_sound(
                *song_music); // the cabinet cuts the song when the battle ends (the emblem and voice follow)
            ai_music_cut = true;
        }
    }
    result_transition.update(current_time);

    const double end_time = std::max(players[0]->end_time, players[1]->end_time);

    // skin_config zdon_ai_result_hold (Green AI): the black cover stays until this long after it started (direct feed
    // Tl7g8XXXvAM: the fade starts 274.1 s, the result comes up out of black 277.3 s)
    bool ai_cover_held = false;
    if (global_data.zdon_ai.active && ai_cover_start_ms >= 0.0) {
        if (const auto it = tex.skin_config_by_name.find("zdon_ai_result_hold"); it != tex.skin_config_by_name.end()) {
            ai_cover_held = get_current_ms() < ai_cover_start_ms + it->second.x;
        }
    }
    if (result_transition.is_finished && !audio.is_sound_playing("result_transition") && !ai_cover_held) {
        return on_screen_end(result_screen());
    }

    if (clock.position() >= end_time) {
        double transition_delay = global_data.config->general.fast_transitions ? kFastResultTransitionDelayMs : 8533.34;
        // Z-DON AI battle, skin_config zdon_ai_end_delay (Green): the cabinet holds the battle emblem ~16.5 s after the
        // last note (footage Tl7g8XXXvAM: emblem 265.0, black 277, result bgm + announce 278.6, last note 260.5), so
        // the result comes 5.25 s later than a normal play's
        if (global_data.zdon_ai.active && !global_data.config->general.fast_transitions && zdon_ai_end_delay_on()) {
            transition_delay += 5250.0;
            // skin_config zdon_ai_result_delay x (Green): ms the cover waits after the song end, replacing the above
            // (the laurel holds 9.1 s before the black fade: direct feed Tl7g8XXXvAM 265.0 -> 274.1)
            if (const auto it = tex.skin_config_by_name.find("zdon_ai_result_delay");
                it != tex.skin_config_by_name.end()) {
                transition_delay = it->second.x;
            }
        }

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
        return on_screen_end(select_screen());
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
