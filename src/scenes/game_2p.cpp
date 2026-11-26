#include "game_2p.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iterator>
#include <rapidjson/document.h>
#include <set>

#include "../libs/input/input.h"
#include "../libs/math/animation.h"
#include "../libs/network/network.h"
#include "../libs/zdon_online.h"
#include "../objects/game/ai_match.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/game/zdon_lane3d.h"
#include "../objects/global/zdon_hud.h"

static double wall_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
}

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
    ai_card_hold = false;
    ai_black_ms = -1.0;
    const bool online = zdon_online.active(); // Z-DON online battle: 2P plays the opponent's hits
    if (ai_battle || online) {
        auto& s1 = global_data.session_data[static_cast<int>(PlayerNum::P1)];
        auto& s2 = global_data.session_data[static_cast<int>(PlayerNum::P2)];
        s2.selected_song = s1.selected_song;
        if (!zdon_online.live()) { // a live opponent picked its own course (the lobby set s2's)
            s2.selected_difficulty = s1.selected_difficulty;
            s2.song_hash = s1.song_hash;
        }
    }
    live_ticket = live_finish_ticket = -1;
    live_last_sync = -1e9;
    live_hold_since = -1.0;
    live_ws.reset();
    live_wait_go = zdon_online.live();
    live_go_hold = live_loaded_sent = false;
    live_go_at = live_start_local = -1.0;
    live_srv_off = live_peer_off = 0.0;
    live_srv_rtt = live_peer_rtt = 1e9;
    live_pings = live_peer_pings = live_start_sent = 0;
    live_ping_last = live_peer_ping_last = -1e9;
    live_start_how.clear();
    live_ws_off = live_ws_used = false;
    live_ws_next = 0;
    live_ws_last = -1e9;
    live_lag.clear();
    live_p2p.reset();
    live_cand_sent = false;
    live_p2p_ack = live_p2p_sent = 0;
    live_p2p_last = -1e9;
    live_first_direct = live_first_server = 0;
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
        mods_1.subdiff = global_data.session_data[static_cast<int>(global_data.player_num)].subdiff;
        mods_2 = mods_1; // the same speed / options as the player
        mods_2.auto_play = false;
    }
    if (online) {
        // no skipping the song and no sudden death in an online battle (the user's rule 2026-11-11): both play it out
        mods_1.skip = false;
        mods_1.zdon_sudden_death = 0;
        // the player's options on both lanes (as an AI battle), but the opponent's chart options: its hits only
        // fit the notes it saw
        mods_2 = mods_1;
        mods_2.auto_play = false;
        mods_2.zdon_sudden_death = 0;
        mods_2.inverse = zdon_online.inverse;
        mods_2.random = zdon_online.random;
        mods_2.seed = zdon_online.seed;
        mods_2.subdiff = zdon_online.subdiff;
        mods_2.zdon_flashlight = false; // my own handicaps stay on my lane
        mods_2.zdon_fade = 0;
    }
    mods_1.shared_kusudamas = shared;
    mods_2.shared_kusudamas = shared;

    players.push_back(std::make_unique<Player>(parser, PlayerNum::P1, diff_1, false, mods_1));
    players.push_back(std::make_unique<Player>(parser_2p, PlayerNum::P2, diff_2, true, mods_2));
    if (online) {
        players[1]->apply_replay_appearance(zdon_online.opponent);
        players[1]->load_replay(zdon_online.opponent_log); // live: empty, the hits come in as they are played
        players[0]->log_auto_hits = zdon_online.live() && mods_1.auto_play;
        players[1]->zdon_silent_replay = true; // the opponent's hits make no sound here
        if (zdon_online.live()) {              // scrolls with the music, judged as the hits come in
            players[1]->zdon_stream = true;
            players[1]->zdon_judge_limit_ms = -1e18;
        }
    }
    if (ai_battle) {
        // the arcade tunes its AI by the course's star level (ai_tuninginfo levels 1..11)
        const auto course = parser->metadata.course_data.find(diff_1);
        const int level =
            zdon_ai_tuning_level(course != parser->metadata.course_data.end() ? course->second.level : 5.0);
        players[1]->enable_zdon_ai(level);
        players[1]->apply_replay_appearance(zdon_ai_player_data(level));
        if (tex.options[SCO::AI_GAME_CHARA_3D]) { // the AI Don's own colours are in its model
            players[1]->use_costume(AI_COSTUME);
        }
        zdon_ai_battle_begin(level);
        global_data.zdon_ai.star = course != parser->metadata.course_data.end() ? course->second.level : 0.0;
    }
    apply_personal_best(*players[0]);
    if (!online && !ai_battle) { // an online opponent's or the AI's best is not this player's
        apply_personal_best(*players[1]);
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
    zdon_lane3d::update_mouse(); // Z-DON: the 3D view (zdon_lane3d.h)
    // Z-DON live battle: the pause key asks to surrender this song (the song goes on meanwhile)
    if (zdon_online.live() && !live_wait_go && zdon_online.surrender == 0 && !zdon_sudden_over) {
        const bool pause_key = check_key_pressed(global_data.config->keys.pause_key);
        if (live_surrender_prompt) {
            const bool don = is_l_don_pressed() || is_r_don_pressed();
            const bool kat = is_l_kat_pressed() || is_r_kat_pressed();
            if (don) {
                live_surrender_prompt = false;
                zdon_online.surrender = 1;
                if (live_ws && live_ws->state() == ZdonWs::State::Open) {
                    live_ws->send(R"({"t":"surrender"})");
                } else {
                    const int t = network.signed_request(
                        "POST", "/match/surrender",
                        {{"access_code", global_data.config->network.access_code_1}, {"code", zdon_online.room}});
                    (void)t;
                }
                spdlog::info("Online battle: surrendered the song at {:.0f} ms", clock.position());
                live_end_now();
            } else if (kat || pause_key || check_key_pressed(global_data.config->keys.back_key)) {
                live_surrender_prompt = false;
            }
            clear_input_buffers(); // the prompt's keys play no notes
        } else if (pause_key) {
            live_surrender_prompt = true;
            clear_input_buffers();
        }
    }
    // screen option ai_wait_loading_card (Nijiiro): the battle starts when the loading card is gone, not behind it:
    // the chart clock stands still and nothing typed meanwhile reaches the drums
    const bool card_up = global_data.zdon_ai.active && tex.options[SCO::AI_WAIT_LOADING_CARD] && transition &&
                         !transition->is_finished();
    global_data.zdon_ai.hold_ready_cue = card_up;
    if (card_up) {
        if (!ai_card_hold) {
            ai_card_hold = true;
            clock.pause(current_time);
        }
        clear_input_buffers();
    } else if (ai_card_hold) {
        ai_card_hold = false;
        clear_input_buffers();
        clock.resume(current_time);
    }
    // Z-DON live battle: the song starts when both games have loaded it (at most 20 s for an opponent that doesn't)
    if (live_wait_go) {
        if (!live_go_hold) {
            live_go_hold = true;
            live_wait_since = current_time;
            clock.pause(current_time);
        }
        clear_input_buffers();
        // the start: the exact time when known (the host's, on my clock), else the server's on its clock, else at
        // once (polling, or no clock difference measured)
        const double now_w = wall_ms();
        bool due = false;
        if (live_start_local >= 0.0) {
            due = now_w >= live_start_local;
        } else if (live_go_at >= 0.0) {
            due = live_srv_rtt < 1e8 ? now_w + live_srv_off >= live_go_at : true;
            if (live_start_how.empty()) {
                live_start_how = live_srv_rtt < 1e8 ? std::format("the server's clock, ping {:.0f} ms", live_srv_rtt)
                                                    : "at once (no clock difference)";
            }
        } else if (zdon_online.go) {
            due = true;
            live_start_how = "at once (polling)";
        }
        if (due || zdon_online.opp_left || current_time - live_wait_since > 20000.0) {
            spdlog::info("Online battle: start ({}, after {:.0f} ms)", due ? live_start_how : std::string("no answer"),
                         current_time - live_wait_since);
            live_wait_go = live_go_hold = false;
            clock.resume(current_time);
        }
    }
    clock.update(current_time);
    transition->update(current_time);
    live_update(current_time);
    update_gameplay(current_time);
    zdon_sudden_death_check();
    if (global_data.zdon_ai.active && players.size() == 2) {
        update_skip();
        const bool cut_short = skipped || zdon_sudden_over;
        if (cut_short) { // what was left of the battle goes to the AI, at once
            zdon_ai_battle_concede();
        }
        zdon_ai_battle_update(cut_short ? 1e12 : clock.position(), *players[0], *players[1]);
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
    // the arcade's AI result comes 0.67 s after the screen is black (capture MBAZD6xA1D8: 162.83 -> 163.50)
    bool cover_waits = audio.is_sound_playing("result_transition");
    if (global_data.zdon_ai.active && tex.options[SCO::AI_RESULT_SHORT_BLACK] && result_transition.is_finished) {
        if (ai_black_ms < 0.0) {
            ai_black_ms = current_time;
        }
        cover_waits = current_time < ai_black_ms + 670.0;
    }
    if (result_transition.is_finished && !cover_waits && !ai_cover_held) {
        return on_screen_end(result_screen());
    }

    if (clock.position() >= end_time && !hold_song_end()) {
        live_send_finish();
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

        if ((skipped || zdon_sudden_over) && !global_data.config->general.fast_transitions) {
            transition_delay = 2800.0; // the skip banner (or the sudden death message), then the result
        }
        end_song(end_time, transition_delay, BackgroundNotification::Disabled);
    }

    if (check_key_pressed(global_data.config->keys.restart_key) && !zdon_online.live()) {
        restart_song(); // not in a live battle: the opponent's song goes on
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

// ---------------------------------------------------------------------------------------------------------------------
// Z-DON online battle (live)

Screens Game2PScreen::select_screen() const {
    if (zdon_online.active()) {
        return Screens::ONLINE_SELECT;
    }
    return global_data.zdon_ai.active ? Screens::AI_SELECT : Screens::SONG_SELECT_2P;
}

bool Game2PScreen::pause_menu_allowed() const {
    return !zdon_online.live(); // the opponent can't wait
}

// both lanes scroll with the music; the opponent's is judged as its hits come in (Player::zdon_stream)
double Game2PScreen::lane_position(size_t /*index*/) const {
    return clock.position();
}

// the song ends now for both lanes, as sudden death ends it (a surrender, mine or the opponent's)
void Game2PScreen::live_end_now() {
    if (zdon_sudden_over) {
        return;
    }
    zdon_sudden_over = true;
    if (song_music) {
        audio.stop_sound(*song_music);
    }
    if (movie) {
        movie->stop();
    }
    for (auto& player : players) {
        if (player && !player->was_skipped()) {
            player->cut_to_end(clock.position());
        }
    }
}

bool Game2PScreen::hold_song_end() {
    if (!zdon_online.live() || zdon_online.opp_left || zdon_online.surrender != 0 || players.size() < 2) {
        return false;
    }
    if (zdon_online.opp_final) {
        return false; // the opponent finished: every hit it played has come (the limit is lifted)
    }
    if (live_hold_since < 0.0) {
        live_hold_since = live_now;
    }
    return live_now - live_hold_since < 20000.0; // never longer than this
}

static std::string live_code() {
    return global_data.config->network.access_code_1;
}

// a /match/sync answer, or one WebSocket message (zdon_match.py): the opponent's hits, position and end
void Game2PScreen::live_take(const std::string& body, bool direct) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
        return;
    }
    auto num = [&doc](const char* k) -> std::optional<double> {
        return doc.HasMember(k) && doc[k].IsNumber() ? std::optional(doc[k].GetDouble()) : std::nullopt;
    };
    const std::string t = doc.HasMember("t") && doc["t"].IsString() ? doc["t"].GetString() : "";
    if (t == "hello" && doc.HasMember("go") && doc["go"].IsBool() && doc["go"].GetBool()) {
        zdon_online.go = true;
    }
    if (t == "hello" || t == "a") { // the WebSocket: how many of my hits the server holds
        if (auto have = num("have")) {
            zdon_online.sent = static_cast<size_t>(*have);
            const bool took = !doc.HasMember("took") || !doc["took"].IsBool() || doc["took"].GetBool();
            if (t == "hello" || !took) {
                live_ws_next = zdon_online.sent; // send again from what it has
            }
        }
        // the round's key for a direct connection: try one
        const char* env = std::getenv("ZDON_ONLINE_P2P");
        if (t == "hello" && !live_p2p && doc.HasMember("p2p") && doc["p2p"].IsString() &&
            !(env && std::string(env) == "0")) {
            live_p2p = std::make_unique<ZdonP2P>();
            live_p2p->start(doc["p2p"].GetString());
        }
        return;
    }
    if (t == "cand") { // the opponent's addresses
        if (live_p2p && doc.HasMember("c") && doc["c"].IsArray()) {
            std::vector<std::string> list;
            for (const auto& c : doc["c"].GetArray()) {
                if (c.IsString()) {
                    list.emplace_back(c.GetString());
                }
            }
            live_p2p->add_remote(list);
        }
        return;
    }
    if (auto ack = num("ack"); ack && direct) {
        live_p2p_ack = std::max(live_p2p_ack, static_cast<size_t>(*ack));
    }
    if (t == "l") {
        zdon_online.opp_left = true;
        return;
    }
    if (t == "surrender" || (doc.HasMember("surrender") && doc["surrender"].IsBool() && doc["surrender"].GetBool())) {
        if (zdon_online.surrender == 0) {
            zdon_online.surrender = 2;
            spdlog::info("Online battle: the opponent surrendered the song");
            zhud::toast(zhud::text("online_opp_surrendered"));
            live_end_now();
        }
        if (t == "surrender") {
            return;
        }
    }
    if (t == "pong") { // the server's clock (the shortest round trip measures it best)
        if (auto c = num("c"), sv = num("s"); c && sv) {
            const double now = wall_ms();
            if (now - *c < live_srv_rtt) {
                live_srv_rtt = now - *c;
                live_srv_off = *sv - (*c + now) / 2.0;
            }
        }
        return;
    }
    if (direct && t == "ping") { // the host measuring my clock
        if (auto c = num("c"); c && live_p2p) {
            live_p2p->send(std::format(R"({{"t":"ppong","c":{:.3f},"p":{:.3f}}})", *c, wall_ms()));
        }
        return;
    }
    if (direct && t == "ppong") {
        if (auto c = num("c"), pv = num("p"); c && pv) {
            const double now = wall_ms();
            if (now - *c < live_peer_rtt) {
                live_peer_rtt = now - *c;
                live_peer_off = *pv - (*c + now) / 2.0;
            }
        }
        return;
    }
    if (direct && t == "start") { // the host's start time, on my clock
        if (auto at = num("at"); at && !zdon_online.host && live_wait_go) {
            live_start_local = *at;
            live_start_how =
                std::format("the host's time over the direct connection, ping {:.1f} ms", num("rtt").value_or(0.0));
        }
        return;
    }
    if (t == "go" || (doc.HasMember("go") && doc["go"].IsBool() && doc["go"].GetBool())) {
        zdon_online.go = true;
        if (auto at = num("at"); at && t == "go") {
            live_go_at = *at;
        }
        if (t == "go") {
            return;
        }
    }
    if (t.empty()) { // polling: my hits it holds, and whether the opponent is still here
        if (auto have = num("have")) {
            zdon_online.sent = static_cast<size_t>(*have);
        }
        zdon_online.opp_left = doc.HasMember("left") && doc["left"].IsBool() && doc["left"].GetBool();
    }
    if (auto t0 = num("t0")) {
        zdon_online.opp_t0 = *t0;
    }
    bool complete = true; // every hit up to the message's position is here: its position counts
    if (doc.HasMember("hits") && doc["hits"].IsString()) {
        // the hits from `at` on (polling: from the `since` this side sent, i.e. what it had taken)
        size_t at = num("at") ? static_cast<size_t>(*num("at")) : zdon_online.received;
        const std::string hits = doc["hits"].GetString();
        const double now = wall_ms();
        size_t pos = 0;
        while (pos < hits.size()) {
            size_t comma = hits.find(',', pos);
            if (comma == std::string::npos) {
                comma = hits.size();
            }
            const std::string hit = hits.substr(pos, comma - pos);
            pos = comma + 1;
            const size_t colon = hit.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            if (at++ != zdon_online.received) {
                continue; // already taken (a resend), or after a gap
            }
            const double ms = std::atof(hit.c_str());
            const int type = std::clamp(std::atoi(hit.c_str() + colon + 1), 0, 3);
            players[1]->append_replay(ms, static_cast<InputLogType>(type));
            ++zdon_online.received;
            ++(direct ? live_first_direct : live_first_server);
            if (zdon_online.opp_t0 > 0.0) {
                live_lag.push_back(now - (zdon_online.opp_t0 + ms));
            }
        }
        complete = at <= zdon_online.received;
    } else if (auto at = num("at")) {
        complete = static_cast<size_t>(*at) <= zdon_online.received;
    }
    // the two paths can arrive out of order: the position only moves on
    if (auto p = num("pos"); p && complete && *p > zdon_online.opp_pos) {
        zdon_online.opp_pos = *p;
        zdon_online.opp_pos_at = live_now;
    }
    if (doc.HasMember("final") && doc["final"].IsObject()) {
        const auto& f = doc["final"];
        auto get = [&f](const char* k) { return f.HasMember(k) && f[k].IsInt() ? f[k].GetInt() : 0; };
        zdon_online.opp_final =
            ZdonOnline::Final{get("score"), get("good"), get("ok"), get("bad"), get("drumroll"), get("max_combo")};
    }
}

// how late the opponent's hits came, from its wall clock at chart ms 0 to mine when they arrived (exact on one
// machine; across two it includes their clocks' difference)
void Game2PScreen::live_log_lag() {
    if (live_lag.empty()) {
        return;
    }
    std::vector<double> v = live_lag;
    std::ranges::sort(v);
    double sum = 0.0;
    for (double x : v) {
        sum += x;
    }
    spdlog::info("Online battle: {} opponent hits came {:.0f} ms late on average (median {:.0f}, 95% {:.0f}, max "
                 "{:.0f}); first directly {}, over the server ({}) {}",
                 v.size(), sum / static_cast<double>(v.size()), v[v.size() / 2], v[v.size() * 95 / 100], v.back(),
                 live_first_direct, live_ws_used ? "WebSocket" : "polling", live_first_server);
    live_lag.clear();
}

void Game2PScreen::live_update(double current_ms) {
    live_now = current_ms;
    if (!zdon_online.live() || players.size() < 2) {
        return;
    }
    // the WebSocket: opened once at the start of the song; polling if there is none
    if (!live_ws && !live_ws_off && live_ticket < 0) {
        const char* env = std::getenv("ZDON_ONLINE_WS");
        std::optional<std::pair<std::string, std::vector<std::string>>> target;
        if (!env || std::string(env) != "0") {
            target = network.signed_ws_target("/match/ws", {{"access_code", live_code()},
                                                            {"code", zdon_online.room},
                                                            {"since", std::to_string(zdon_online.received)}});
        }
        if (target) {
            live_ws = std::make_unique<ZdonWs>();
            live_ws->open(target->first, target->second);
            live_ws_next = zdon_online.sent;
        } else {
            live_ws_off = true;
        }
    }
    if (live_ws && (live_ws->state() == ZdonWs::State::Failed || live_ws->state() == ZdonWs::State::Closed)) {
        while (auto m = live_ws->poll()) {
            live_take(*m);
        }
        live_ws.reset();
        live_ws_off = true; // polling from here on
    }
    const bool ws_open = live_ws && live_ws->state() == ZdonWs::State::Open;
    if (ws_open) {
        live_ws_used = true;
        while (auto m = live_ws->poll()) {
            live_take(*m);
        }
    }
    if (ws_open && live_wait_go && live_pings < 8 && current_ms - live_ping_last >= 25.0) {
        live_ws->send(std::format(R"({{"t":"ping","c":{:.3f}}})", wall_ms()));
        ++live_pings;
        live_ping_last = current_ms;
    }
    if (ws_open && !live_loaded_sent && live_pings >= 4) { // the clock is being measured first
        live_ws->send(R"({"t":"loaded"})");
        live_loaded_sent = true;
    }
    const bool direct_up = live_p2p && live_p2p->connected();
    if (direct_up && zdon_online.host && live_wait_go && live_peer_pings < 12 &&
        current_ms - live_peer_ping_last >= 20.0) {
        live_p2p->send(std::format(R"({{"t":"ping","c":{:.3f}}})", wall_ms()));
        ++live_peer_pings;
        live_peer_ping_last = current_ms;
    }
    // the host: once the server's start time is known, it is mine on my clock and the guest's on its own
    if (zdon_online.host && live_wait_go && live_go_at >= 0.0 && live_srv_rtt < 1e8) {
        if (live_start_local < 0.0) {
            live_start_local = live_go_at - live_srv_off;
            live_start_how = std::format("the server's clock, ping {:.0f} ms", live_srv_rtt);
        }
        if (direct_up && live_peer_rtt < 1e8 && live_start_sent < 3) {
            live_p2p->send(std::format(R"({{"t":"start","at":{:.3f},"rtt":{:.2f}}})", live_start_local + live_peer_off,
                                       live_peer_rtt));
            ++live_start_sent;
        }
    }
    if (live_p2p) {
        while (auto m = live_p2p->poll()) {
            live_take(*m, true);
        }
        if (!live_cand_sent && ws_open) {
            if (auto cand = live_p2p->candidates()) {
                std::string list;
                for (const std::string& c : *cand) {
                    list += std::format("{}\"{}\"", list.empty() ? "" : ",", c);
                }
                live_ws->send(std::format(R"({{"t":"cand","c":[{}]}})", list));
                live_cand_sent = true;
            }
        }
    }
    if (live_ticket >= 0) {
        if (auto r = network.take_signed(live_ticket)) {
            live_ticket = -1;
            if (r->first == 200) {
                live_take(r->second);
            }
        }
    }
    // the opponent's lane misses notes only as far as its reported position (all its hits before it are here);
    // finished or gone: everything left is decided
    players[1]->zdon_judge_limit_ms = (zdon_online.opp_final || zdon_online.opp_left) ? 1e18
                                      : zdon_online.opp_pos < -1e17                   ? -1e18
                                                                                      : zdon_online.opp_pos;
    const InputLog& log = players[0]->input_log;
    const double pos = clock.position();
    const double t0 = wall_ms() - pos;
    auto hits_from = [&log](size_t from) {
        std::string hits;
        for (size_t i = from; i < log.size(); ++i) {
            hits += std::format("{}{:.1f}:{}", hits.empty() ? "" : ",", log[i].first, static_cast<int>(log[i].second));
        }
        return hits;
    };
    if (live_p2p && live_p2p->connected() && (log.size() > live_p2p_sent || current_ms - live_p2p_last >= 50.0)) {
        // everything the opponent hasn't confirmed (a lost datagram is covered by the next one), up to 60 hits
        const size_t from = std::min(live_p2p_ack, log.size());
        const size_t to = std::min(log.size(), from + 60);
        std::string hits;
        for (size_t i = from; i < to; ++i) {
            hits += std::format("{}{:.1f}:{}", hits.empty() ? "" : ",", log[i].first, static_cast<int>(log[i].second));
        }
        const std::string tail = to == log.size() ? std::format(R"(,"pos":{:.1f})", pos) : "";
        live_p2p->send(std::format(R"({{"t":"o","at":{},"hits":"{}","ack":{},"score":{},"combo":{},"t0":{:.1f}{}}})",
                                   from, hits, zdon_online.received, players[0]->get_score(), players[0]->get_combo(),
                                   t0, tail));
        live_p2p_sent = log.size();
        live_p2p_last = current_ms;
    }
    if (ws_open) {
        // a new hit goes at once; the position at least every 50 ms
        if (log.size() > live_ws_next || current_ms - live_ws_last >= 50.0) {
            live_ws->send(std::format(
                R"({{"t":"s","from":{},"hits":"{}","pos":{:.1f},"score":{},"combo":{},"t0":{:.1f}}})", live_ws_next,
                hits_from(live_ws_next), pos, players[0]->get_score(), players[0]->get_combo(), t0));
            live_ws_next = log.size();
            live_ws_last = current_ms;
        }
        return;
    }
    if (live_ws) {
        return; // still connecting (at the start of the song, before the notes)
    }
    if (live_ticket < 0 && current_ms - live_last_sync >= 100.0) {
        live_ticket = network.signed_request("POST", "/match/sync",
                                             {{"access_code", live_code()},
                                              {"code", zdon_online.room},
                                              {"from", std::to_string(zdon_online.sent)},
                                              {"hits", hits_from(zdon_online.sent)},
                                              {"pos", std::format("{:.1f}", pos)},
                                              {"score", std::to_string(players[0]->get_score())},
                                              {"combo", std::to_string(players[0]->get_combo())},
                                              {"t0", std::format("{:.1f}", t0)},
                                              {"loaded", "1"},
                                              {"since", std::to_string(zdon_online.received)}});
        live_last_sync = current_ms;
    }
}

void Game2PScreen::live_send_finish() {
    if (!zdon_online.live() || live_finish_ticket >= 0 || players.empty()) {
        return;
    }
    const ResultData r = players[0]->get_result_score();
    live_finish_ticket = network.signed_request("POST", "/match/finish",
                                                {{"access_code", live_code()},
                                                 {"code", zdon_online.room},
                                                 {"score", std::to_string(r.score)},
                                                 {"good", std::to_string(r.good)},
                                                 {"ok", std::to_string(r.ok)},
                                                 {"bad", std::to_string(r.bad)},
                                                 {"drumroll", std::to_string(r.total_drumroll)},
                                                 {"max_combo", std::to_string(r.max_combo)}});
}

Screens Game2PScreen::on_screen_end(Screens next_screen) {
    if (zdon_online.live()) {
        live_send_finish();
        if (live_ticket >= 0) {
            network.forget_signed(live_ticket);
            live_ticket = -1;
        }
        live_log_lag();
        live_ws.reset(); // closes it
        live_p2p.reset();
        // the opponent's own count is the one that stands (its lane here may have missed a late hit)
        if (zdon_online.opp_final) {
            auto& rd = global_data.session_data[static_cast<int>(PlayerNum::P2)].result_data;
            rd.score = zdon_online.opp_final->score;
            rd.good = zdon_online.opp_final->good;
            rd.ok = zdon_online.opp_final->ok;
            rd.bad = zdon_online.opp_final->bad;
            rd.total_drumroll = zdon_online.opp_final->drumroll;
            rd.max_combo = zdon_online.opp_final->max_combo;
        }
    }
    return GameScreen::on_screen_end(next_screen);
}

void Game2PScreen::draw() {
    GameScreen::draw();
    draw_live_overlay();
}

// mid-screen: the opponent hasn't started yet, has left, or is still finishing at the song's end
void Game2PScreen::draw_live_overlay() {
    if (!zdon_online.live()) {
        return;
    }
    const float u = tex.screen_width / 1920.0f;
    std::string line;
    if (live_surrender_prompt) {
        line = zhud::text("online_surrender_prompt");
    } else if (zdon_online.surrender == 1) {
        line = zhud::text("online_you_surrendered");
    } else if (zdon_online.surrender == 2) {
        line = zhud::text("online_opp_surrendered");
    } else if (zdon_online.opp_left) {
        line = zhud::text("online_opp_left");
    } else if (live_wait_go || zdon_online.opp_pos < -1e17) {
        line = zhud::text("online_opp_waiting");
    } else if (live_hold_since >= 0.0) {
        line = zhud::text("online_wait_finish");
    }
    if (line.empty()) {
        return;
    }
    OutlinedText& t = zhud::label(line, 26, Color{140, 220, 255, 255});
    const float w = t.width + 40 * u, h = t.height + 16 * u;
    const float x = (tex.screen_width - w) / 2.0f, y = tex.screen_height * 0.5f - h / 2.0f;
    zhud::panel(x, y, w, h, 0.85f, Color{140, 220, 255, 200});
    t.draw({.x = x + 20 * u, .y = y + 8 * u});
}
