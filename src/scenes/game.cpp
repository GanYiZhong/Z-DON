#include "game.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

#include "../libs/file/filesystem.h"
#include "../libs/file/scores.h"
#include "../libs/file/script.h"
#include "../libs/fixed_step.h"
#include "../libs/gfx/text.h"
#include "../libs/input/input.h"
#include "../libs/math/animation.h"
#include "../libs/network/network.h"
#include "../libs/score_submission.h"
#include "../libs/test_mode.h"
#include "../libs/zdon_record.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/game/zdon_lane3d.h"
#include "../objects/global/zdon_hud.h"
#include "../platform/vblank_pacing.h"

std::span<const std::string> GameScreen::skipped_texture_subsets() const {
    static const std::vector<std::string> skipped = {"ai_battle", "enso_battle", "battle_stage", "practice",
                                                     "dan_info",  "gauge_ai",    "gauge_dan",    "tutorial"};
    return skipped;
}

void GameScreen::on_screen_start() {
    Screen::on_screen_start();
    reset_playback();
    pause_menu.init_textures();
    zdon_lane3d::reset();
    zdon_lane3d::show_hint_once();
    start_delay = 1000.0f;
    JudgePos::X = tex.get_skin_config(SC::GAME_JUDGE_POS)->x;
    JudgePos::Y = tex.get_skin_config(SC::GAME_JUDGE_POS)->y;
    TJAParser::jpos_field_width = (tex.screen_width - JudgePos::X) / tex.screen_scale;
    SessionData& session_data = global_data.session_data[static_cast<int>(global_data.player_num)];
    if (session_data.selected_song.empty() || !exists(session_data.selected_song)) {
        auto songs = get_song_files(global_data.config->paths.tja_path);
        if (songs.empty()) {
            throw std::runtime_error("No songs found in tja_path, cannot load fallback song");
        }
        session_data.selected_song = songs.front();
        session_data.selected_difficulty = static_cast<int>(Difficulty::EASY);
        try {
            SongParser probe(session_data.selected_song, 0);
            auto& course_data = probe.metadata.course_data;
            if (!course_data.empty()) {
                session_data.selected_difficulty = course_data.rbegin()->first;
            }
        } catch (const std::exception& e) {
            spdlog::warn("Failed to probe difficulty for fallback song {}: {}", session_data.selected_song.string(),
                         e.what());
        }
        spdlog::info("GameScreen auto-loaded fallback song: {}", session_data.selected_song.string());
    }
    initialize_chart(session_data.selected_song);
    spdlog::info("Chart initialized for song: {}", session_data.selected_song.string());
    load_hitsounds();
    song_info = SongInfo(session_data.song_title, session_data.song_subtitle, parser->metadata.subtitle_full_display,
                         session_data.genre_index, global_data.credit.songs_played + 1, 0, session_data.genre_label);
    result_transition = ResultTransition(scene_player_num());
    ai_cover_start_ms = -1.0;
    bpm = parser->metadata.bpm;
    scene_preset = parser->metadata.scene_preset;
    if (!has_movie()) {
        background.emplace(scene_player_num(), bpm, background_scene_preset());
        spdlog::info("Background initialized");
    } else {
        spdlog::info("Movie initialized");
    }
    transition.emplace(session_data.song_title, session_data.song_subtitle, true);
    if (exists(session_data.selected_song.parent_path() / "Loading.png")) {
        transition->add_loading_graphic((session_data.selected_song.parent_path() / "Loading.png").string());
    }
    transition->start();

    reset_song_clock();

    init_skip();

    std::optional<Note> first_note = players.back()->get_first_note();
    if (first_note.has_value()) {
        double travel_time = first_note->hit_ms - first_note->load_ms;
        double initial_ms = parser->metadata.offset * 1000 + global_data.config->general.audio_offset;
        double extra_delay = initial_ms - start_delay + travel_time;
        if (extra_delay > 0.0) {
            clock.seek(get_current_ms(), clock.position() - extra_delay);
        }
    }

    platform_trim_heap();
}

Screens GameScreen::on_screen_end(Screens next_screen) {
    zdon_lane3d::release();                     // Z-DON: the 3D view's targets go with the screen
    global_data.zdon_ai.don_draw_now = nullptr; // it points at this screen's player
    global_data.zdon_ai.ai_draw_now = nullptr;
    unload_song();
    pause_menu = PauseMenu();
    background.reset();
    transition.reset();
    global_data.camera = CameraConfig(); // TJA camera commands must not leak out of the song

    return Screen::on_screen_end(next_screen);
}

Modifiers GameScreen::get_player_modifiers(PlayerNum pn) {
    Modifiers m = player_data_to_modifiers(scores_manager.get_player_data(get_player_id(pn)).value_or(PlayerData{}));
    if (global_data.force_auto_play) {
        m.auto_play = true; // `--auto`
    }
    if (zdon_record::active()) { // Z-DON: a lane's support level (--record-tracks <course>@<level>[y])
        const size_t lane = pn == PlayerNum::P2 ? 1 : 0;
        if (const int level = zdon_record::track_subdiff(lane)) {
            m.subdiff = level;
            m.zdon_subdiff_yono = zdon_record::track_subdiff_yono(lane);
        }
    }
    if (m.equal_speed) {
        m.equal_speed_bpm = (parser && parser->metadata.bpm > 0) ? parser->metadata.bpm : 120.0;
    }
    if (m.random > 0) {
        m.seed = std::random_device{}();
    }
    return m;
}

void GameScreen::load_hitsounds() {
    int neiro_p1 = scores_manager.get_player_data(get_player_id(PlayerNum::P1)).value_or(PlayerData{}).neiro_index;
    int neiro_p2 = scores_manager.get_player_data(get_player_id(PlayerNum::P2)).value_or(PlayerData{}).neiro_index;
    // Z-DON: the four decodes run at once (audio.load_sound is thread-safe; the song loads the same way)
    const std::pair<fs::path, const char*> sounds[] = {
        {resolve_skin_path(fs::path("Sounds/hit_sounds") / std::to_string(neiro_p1) / "don.ogg"), "hitsound_don_1p"},
        {resolve_skin_path(fs::path("Sounds/hit_sounds") / std::to_string(neiro_p1) / "ka.ogg"), "hitsound_kat_1p"},
        {resolve_skin_path(fs::path("Sounds/hit_sounds") / std::to_string(neiro_p2) / "don.ogg"), "hitsound_don_2p"},
        {resolve_skin_path(fs::path("Sounds/hit_sounds") / std::to_string(neiro_p2) / "ka.ogg"), "hitsound_kat_2p"},
    };
    // Z-DON: resident across screens; a later game screen with the same neiros reuses them undecoded
    bool reused[std::size(sounds)] = {};
    std::vector<std::future<std::string>> loads;
    for (size_t i = 1; i < std::size(sounds); i++) {
        loads.push_back(std::async(std::launch::async, [&s = sounds[i], r = &reused[i]] {
            return audio.load_resident_sound(s.first, s.second, r);
        }));
    }
    audio.load_resident_sound(sounds[0].first, sounds[0].second, &reused[0]);
    for (auto& f : loads) {
        f.get();
    }
    spdlog::info("Loaded ogg hit sounds for 1P and 2P ({} of {} resident, not decoded again)",
                 std::count(std::begin(reused), std::end(reused), true), std::size(sounds));
}

void GameScreen::open_movie(const fs::path& path) {
    adopt_movie(true);
    movie.reset();
    const std::string ext = path.extension().string();
    if (ext == ".png" || ext == ".jpg") {
        movie = std::make_unique<VideoPlayer>(path); // a still image: a GL texture, main thread
        return;
    }
    pending_movie = std::async(std::launch::async, [path] { return std::make_unique<VideoPlayer>(path); });
    fixed_step::wait(pending_movie);
}

void GameScreen::adopt_movie(bool wait) {
    if (!pending_movie.valid()) {
        return;
    }
    if (!wait && pending_movie.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }
    try {
        movie = pending_movie.get();
    } catch (const std::exception& e) {
        spdlog::error("Failed to open the background video: {}", e.what());
    }
}

void GameScreen::initialize_chart(const fs::path& song) {
    start_delay = (song.extension() == ".osu") ? 0 : start_delay;
    const auto t0 = std::chrono::steady_clock::now();
    parser = std::make_shared<SongParser>(song, start_delay);
    const auto t1 = std::chrono::steady_clock::now();
    initialize_song_movie();
    set_session_song_info(global_data.player_num);

    load_song_audio(AudioLoadMode::Async);

    const auto t2 = std::chrono::steady_clock::now();
    create_players();
    using ms = std::chrono::duration<double, std::milli>;
    spdlog::info("Chart read in {:.1f} ms, players made in {:.1f} ms", ms(t1 - t0).count(),
                 ms(std::chrono::steady_clock::now() - t2).count());
}

void GameScreen::initialize_song_movie() {
    // merge 0ed42ec: upstream's empty / not-a-file guard, Z-DON's async open_movie
    if (!parser->metadata.bgmovie.empty() && fs::is_regular_file(parser->metadata.bgmovie)) {
        open_movie(parser->metadata.bgmovie);
    }
}

void GameScreen::set_session_song_info(PlayerNum player_num) {
    auto& session = global_data.session_data[static_cast<int>(player_num)];
    const auto& metadata = parser->metadata;
    const std::string& lang = global_data.config->general.language;

    session.song_title = localized_text(metadata.title, lang, LocalizedTextFallback::ENGLISH_OR_FIRST);
    session.song_subtitle = localized_text(metadata.subtitle, lang, LocalizedTextFallback::NONE);
    session.song_subtitle_full_display = metadata.subtitle_full_display;
}

void GameScreen::apply_personal_best(Player& player) {
    if (player.is_auto_play() || player.is_replay() || player.get_modifiers().subdiff != 0) {
        return;
    }
    const SessionData& sd = global_data.session_data[static_cast<int>(player.player_num)];
    try {
        const auto previous =
            scores_manager.get_score(sd.song_hash, sd.selected_difficulty, get_player_id(player.player_num));
        if (previous && previous->score > 0) {
            player.set_personal_best(previous->score);
        }
    } catch (const std::exception& e) {
        spdlog::error("GameScreen::apply_personal_best failed: {}", e.what());
    }
}

void GameScreen::create_players() {
    const auto player_index = static_cast<int>(global_data.player_num);
    players.push_back(std::make_unique<Player>(parser, global_data.player_num,
                                               global_data.session_data[player_index].selected_difficulty, false,
                                               get_player_modifiers(global_data.player_num)));
    players.back()->allow_papamama();

    if (const auto& replay = global_data.session_data[player_index].replay) {
        InputLog typed_log;
        for (const auto& [ms, type] : replay->input_log) {
            typed_log.emplace_back(ms, static_cast<InputLogType>(type));
        }
        players.back()->load_replay(typed_log);
        players.back()->apply_replay_appearance(replay->player_data);
    }
    apply_personal_best(*players.back());
}

void GameScreen::load_song_audio(AudioLoadMode mode) {
    if (!fs::exists(parser->metadata.wave) || song_music || pending_song_load.valid()) {
        return;
    }

    fs::path wave = parser->metadata.wave;
    if (mode == AudioLoadMode::Async) {
        pending_song_load = std::async(std::launch::async, [wave] { return audio.load_sound(wave, "song"); });
        fixed_step::wait(pending_song_load);
    } else {
        song_music = audio.load_sound(wave, "song");
    }
}

void GameScreen::set_players_audio_end() {
    if (!song_music || song_music->empty()) {
        return;
    }
    double audio_end = audio.get_sound_time_length(*song_music) * 1000.0 + audio_start_ms();
    for (auto& player : players) {
        player->set_audio_end(audio_end);
    }
}

void GameScreen::poll_pending_song() {
    if (!pending_song_load.valid() ||
        pending_song_load.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    std::string name = pending_song_load.get();
    if (name.empty()) {
        return;
    }
    song_music = name;

    if (!song_started) {
        set_players_audio_end();
    }

    if (song_started && !clock.is_paused()) {
        play_song_audio((clock.position() - audio_start_ms()) / 1000.0);
    }
}

void GameScreen::start_song(double chart_ms) {
    if (chart_ms >= audio_start_ms() && !song_started && !clock.is_paused()) {
        if (song_music) {
            play_song_audio(0.0);
            spdlog::info("Song started at {}", chart_ms);
        }
        adopt_movie(true);
        if (movie) {
            movie->start(get_current_ms());
            movie->set_volume(0.0);
        }
        song_started = true;
    }
}

double GameScreen::audio_start_ms() const {
    return parser->metadata.offset * 1000.0 + start_delay - global_data.config->general.audio_offset;
}

void GameScreen::reset_playback() {
    clock.reset(get_current_ms());
    song_started = false;
    score_saved = false;
    pause_used = false;
    paused_audio_time = 0.0f;
}

void GameScreen::reset_song_clock(double opening_delay_ms) {
    clock.reset(get_current_ms(),
                parser->metadata.offset * 1000.0 + global_data.config->general.audio_offset - opening_delay_ms);
}

void GameScreen::stop_song_audio() {
    if (pending_song_load.valid()) {
        try {
            song_music = pending_song_load.get();
        } catch (const std::exception& e) {
            spdlog::warn("Failed to finish song audio load: {}", e.what());
        }
    }

    if (song_music) {
        audio.stop_sound(*song_music);
    }

    song_music.reset();
    song_started = false;
}

void GameScreen::unload_song() {
    stop_song_audio();

    adopt_movie(true);
    if (movie) {
        movie->stop();
    }

    movie.reset();
    players.clear();
    release_chart_resources();
    parser.reset();
    reset_playback();
}

void GameScreen::play_song_audio(double position_seconds) {
    if (!song_music) {
        return;
    }

    audio.set_sound_pitch(*song_music, song_playback_rate());
    audio.play_sound(*song_music, VolumePreset::MUSIC);
    audio.seek_sound(*song_music, static_cast<float>(std::max(0.0, position_seconds)));
}

void GameScreen::pause_playback() {
    if (clock.is_paused()) {
        return;
    }

    clock.pause(get_current_ms());
    paused_audio_time = static_cast<float>(std::max(0.0, (clock.position() - audio_start_ms()) / 1000.0));

    if (song_music && song_started) {
        paused_audio_time = audio.get_sound_time_played(*song_music);
        audio.stop_sound(*song_music);
    }
}

void GameScreen::resume_playback(std::optional<double> chart_ms) {
    if (!clock.is_paused()) {
        return;
    }

    double current_ms = get_current_ms();

    if (chart_ms) {
        clock.seek(current_ms, *chart_ms);
        paused_audio_time = static_cast<float>(std::max(0.0, (*chart_ms - audio_start_ms()) / 1000.0));
        song_started = true;
    }

    clock.resume(current_ms);

    if (song_started) {
        play_song_audio(paused_audio_time);
    }
}

void GameScreen::toggle_pause() {
    if (clock.is_paused()) {
        resume_playback();
    } else {
        pause_playback();
    }
}

void GameScreen::restart_song() {
    pause_menu.reset();
    tex.cancel_predecode("result");
    audio.cancel_predecoded_sounds("result");
    unload_song();
    initialize_chart(global_data.session_data[static_cast<int>(restart_player_num())].selected_song);

    if (background.has_value()) {
        for (const auto& player : players) {
            background->handle_miss_streak(player->player_num, false);
        }
    }

    result_transition = ResultTransition(scene_player_num());
    ai_cover_start_ms = -1.0;
    audio.play_sound("restart", VolumePreset::SOUND);
    init_skip();
    reset_song_clock();
}

std::optional<Screens> GameScreen::global_keys() {
    if (check_key_pressed(global_data.config->keys.restart_key)) {
        restart_song();
    }

    if (check_key_pressed(global_data.config->keys.back_key)) {
        if (can_pause()) {
            open_pause_menu(); // Z-DON: Escape pauses (pause_menu = false: leaves at once, as before)
            return std::nullopt;
        }
        return on_screen_end(select_screen());
    }

    // if (check_key_pressed(global_data.config->keys.pause_key)) {
    //     toggle_pause();
    // }
    // Z-DON: Space, or the window losing the focus, opens the pause menu
    const bool lost_focus = focus_lost();
    if (can_pause() && (check_key_pressed(global_data.config->keys.pause_key) || lost_focus)) {
        open_pause_menu();
    }

    return std::nullopt;
}

bool GameScreen::can_pause() const {
    if (!global_data.config->general.pause_menu || !pause_menu_allowed() || !transition || !transition->is_finished() ||
        result_transition.is_started || score_saved || skipped || players.empty()) {
        return false;
    }
    double end_time = 0.0;
    for (const auto& player : players) {
        end_time = std::max(end_time, player->end_time);
    }
    return clock.position() < end_time;
}

void GameScreen::open_pause_menu() {
    pause_playback();
    pause_used = true;
    pause_menu.open_menu(get_current_ms());
    spdlog::info("Pause menu opened at {:.1f} ms", clock.position());
}

bool GameScreen::focus_lost() {
    if (zdon_test_mode()) {
        return false;
    }
    const bool focused = is_window_focused();
    const bool lost = window_was_focused && !focused;
    window_was_focused = focused;
    return lost;
}

std::optional<Screens> GameScreen::update_pause_menu(Screens back) {
    const double now = get_current_ms();
    window_was_focused = zdon_test_mode() || is_window_focused();
    PauseMenu::Action action = pause_menu.update(now);
    if (check_key_pressed(global_data.config->keys.restart_key)) {
        action = PauseMenu::Action::RESTART;
    }
    switch (action) {
    case PauseMenu::Action::RESUME: {
        const double held = pause_menu.paused_ms(now);
        pause_menu.reset();
        clear_input_buffers(); // nothing pressed while paused or counting down reaches the notes
        resume_playback();
        if (movie) {
            movie->delay(held);
        }
        spdlog::info("Pause menu: resumed at {:.1f} ms after {:.0f} ms", clock.position(), held);
        break;
    }
    case PauseMenu::Action::RESTART:
        pause_menu.reset();
        clear_input_buffers();
        restart_song();
        spdlog::info("Pause menu: restart");
        break;
    case PauseMenu::Action::BACK:
        pause_menu.reset();
        spdlog::info("Pause menu: back");
        return on_screen_end(back);
    case PauseMenu::Action::NONE:
        break;
    }
    return std::nullopt;
}

void GameScreen::update_background(double current_ms) {
    if (has_movie()) {
        adopt_movie(false);
        if (movie) {
            movie->update(current_ms);
        }
    } else {
        bpm = players[0]->bpm;
        if (background.has_value()) {
            background->update(current_ms, bpm);
        }
    }
}

// Z-DON: every reason a play's score is not saved, in one place (keys of zdon_hud's "reason_<key>")
std::vector<std::string> GameScreen::zdon_unsaved_reasons(const Player& player) {
    std::vector<std::string> r;
    if (player.zdon_auto_used()) {
        r.emplace_back("auto");
    }
    if (player.is_replay()) {
        r.emplace_back("replay");
    }
    if (player.get_modifiers().equal_speed_bpm > 0) {
        r.emplace_back("equal_speed");
    }
    if (zdon_record::active()) {
        r.emplace_back("record"); // a recording without hits is not a play
    }
    if (player.zdon_sudden_failed()) {
        r.emplace_back("sudden_death"); // a run cut short by a miss
    }
    return r;
}

void GameScreen::save_score(const std::string& player_id, PlayerNum player_num) {
    if (pause_used) {
        return;
    }

    Player* target = nullptr;

    for (const auto& player : players) {
        if (player && player->player_num == player_num) {
            target = player.get();
        }
    }

    if (!target || target->is_auto_play() || target->is_replay()) {
        return;
    }
    std::vector<std::string>& unsaved = global_data.session_data[static_cast<int>(player_num)].zdon_unsaved;
    unsaved = zdon_unsaved_reasons(*target);
    if (!unsaved.empty()) {
        std::string why;
        for (const std::string& r : unsaved) {
            why += (why.empty() ? "" : ", ") + r;
        }
        spdlog::info("Score of player {} not saved: {}", static_cast<int>(player_num), why);
        return;
    }

    SessionData& sd = global_data.session_data[static_cast<int>(player_num)];
    auto& result = sd.result_data;
    if (target->get_modifiers().subdiff != 0) {
        result.prev_score = result.score;
        return;
    }

    try {
        const auto previous = scores_manager.get_score(sd.song_hash, sd.selected_difficulty, player_id);

        if (previous) {
            result.prev_score = previous->score;
            result.prev_known = true;
        }

        ScoreSubmissionRequest request{.player_id = player_id,
                                       .hash = sd.song_hash,
                                       .difficulty = sd.selected_difficulty,
                                       .score = completed_play_score({.score = result.score,
                                                                      .good = result.good,
                                                                      .ok = result.ok,
                                                                      .bad = result.bad,
                                                                      .drumroll = result.total_drumroll,
                                                                      .max_combo = result.max_combo},
                                                                     skipped, target->gauge->get_is_clear()),
                                       .modifiers = target->get_modifiers(),
                                       .input_log = target->input_log,
                                       .judge_params = target->judge_params()};
        const Score& score = request.score;
        // Z-DON: whether it replaces a stored best, ranked as the scores table ranks (crown first, then score)
        result.new_best =
            !skipped && previous.has_value() &&
            (score.crown > previous->crown || (score.crown == previous->crown && score.score > previous->score));

        const auto submission =
            score_submission_for(player_id, scores_manager, *global_data.config, sd.selected_collection,
                                 static_cast<int>(target->gauge->get_length()));

        const auto profile = ScoreSubmitter(scores_manager, network).submit(request, submission);
        // Z-DON PB pace: a new best keeps its score curve
        if (auto& g = zdon_ghost[std::clamp(static_cast<int>(player_num), 0, 3)];
            !skipped && !g.rec.empty() && (!previous || score.score > previous->score)) {
            g.rec.emplace_back(static_cast<float>(std::max<double>(clock.position(), g.rec.back().first)), score.score);
            scores_manager.set_zdon_ghost(sd.song_hash, sd.selected_difficulty, player_id, g.rec);
        }
        if (profile) {
            result.submitted = true;
            reward_score(*target, *submission, *profile);
        }
    } catch (const std::exception& e) {
        spdlog::error("GameScreen::save_score failed for player {}: {}", player_id, e.what());
    } catch (...) {
        spdlog::error("GameScreen::save_score failed for player {}: unknown exception", player_id);
    }
}

void GameScreen::reward_score(const Player& player, const RemoteScoreSubmission& submission,
                              const PlayerData& profile) {
    const int seat = static_cast<int>(player.player_num) - 1;

    if (skipped || seat < 0 || seat > 1) {
        return;
    }

    const auto& sd = global_data.session_data[static_cast<int>(player.player_num)];
    global_data.credit.songs[seat]++;
    network.submit_don_points(seat, submission.access_code, submission.gauge, sd.song_title);
    network.submit_medals(seat, submission.access_code);

    if (global_data.campaign.kinotake_team[seat] >= 0) {
        const bool duet = std::ranges::count_if(
                              players, [](const auto& p) { return p && !p->is_auto_play() && !p->is_replay(); }) == 2;

        const bool costume = profile.chara_is_costume && kinotake_costume_team(profile.chara_cos_index) >= 0;

        network.submit_kinotake(seat, submission.access_code, global_data.campaign.kinotake_team[seat],
                                sd.result_data.score, duet, costume);
    }
}

void GameScreen::resync_song(double current_ms) {
    if (fixed_step::on()) {
        return; // YATAIDON_FIXED_STEP_MS: the chart follows the frame clock
    }
    if (!song_started || clock.is_paused()) {
        return;
    }
    if (!song_music) {
        return;
    }
    if (!audio.is_sound_playing(*song_music)) {
        return;
    }

    double audio_ms = audio.get_sound_time_played(*song_music) * 1000.0f;
    double audio_ms_adjusted = audio_ms + audio_start_ms();

    // Z-DON: with vblank pacing, the refresh rate DWM measures (e.g. 119.88 Hz where the mode says 120); the
    // shown frames advance by exactly that period
    clock.synchronize(current_ms, audio_ms_adjusted, [] {
        const double period_ms = vblank_pacing::refresh_period_ms();
        return period_ms > 0.0 ? 1000.0 / period_ms : static_cast<double>(get_monitor_refresh_rate());
    });
}

void GameScreen::init_skip() {
    skip_lane = PlayerNum::ALL;
    skip_count = 0;
    skip_last = -1;
    skip_play_hits = 0;
    skipped = false;
    zdon_sudden_over = false;
    skip_pushed.reset();
    skip_text.reset();
    global_data.live_skip_count = -1;

    if (!tex.options[SCO::OPTION_SKIP_ROW]) {
        return;
    }

    // Z-DON: an AI battle has one human: the skip is read from the AI's (unplayed) lane, as in a solo play
    const bool ai_battle = global_data.zdon_ai.active && players.size() == 2;
    if (!ai_battle && (global_data.player_num == PlayerNum::TWO_PLAYER || players.size() > 1)) {
        return;
    }

    bool wanted = false;
    bool lane_used[3] = {false, false, false};
    const auto p1_index = static_cast<int>(PlayerNum::P1);
    const auto p2_index = static_cast<int>(PlayerNum::P2);
    for (const auto& player : players) {
        if (!player) {
            continue;
        }
        if (player->is_skip_enabled()) {
            wanted = true;
        }
        const auto pn = static_cast<int>(player->player_num);
        if (pn == p1_index || pn == p2_index) {
            lane_used[pn] = true;
        }
    }
    if (!wanted) {
        return;
    }

    if (ai_battle || !lane_used[p2_index]) {
        skip_lane = PlayerNum::P2;
    } else if (!lane_used[p1_index]) {
        skip_lane = PlayerNum::P1;
    }
    if (skip_lane != PlayerNum::ALL) {
        global_data.live_skip_count = 0;
    }
}

void GameScreen::update_skip() {
    if (skip_lane == PlayerNum::ALL) {
        return;
    }
    if (!skipped && !clock.is_paused()) {
        poll_skip();
    }
    push_skip_state();
}

void GameScreen::push_skip_state() {
    global_data.live_skip_count = skip_count;
    if (!background.has_value() || !background->wants_skip()) {
        return;
    }
    // only on a change: the skins' handle_skip keeps the last state (and every field of it
    // follows from count and skipped), so the same table every frame was only garbage
    if (skip_pushed == std::pair{skip_count, skipped}) {
        return;
    }
    skip_pushed = std::pair{skip_count, skipped};

    sol::state& lua = *script_manager.lua;
    sol::table st = lua.create_table();
    st["count"] = skip_count;
    st["total"] = SKIP_HITS;
    st["remaining"] = SKIP_HITS - skip_count;
    st["skipped"] = skipped;
    background->handle_skip(global_data.player_num, st);
}

void GameScreen::poll_skip() {

    size_t play_hits = 0;
    for (const auto& player : players) {
        if (player && (!global_data.zdon_ai.active || player == players.front())) { // not the AI's own hits
            play_hits += player->input_log.size();
        }
    }
    if (play_hits != skip_play_hits) {
        skip_play_hits = play_hits;
        if (skip_count > 0 && skip_count < SKIP_HITS) {
            skip_count = 0;
            skip_last = -1;
            skip_text.reset();
        }
    }

    int value = -1;
    if (is_l_kat_pressed(skip_lane)) {
        value = 1;
    } else if (is_r_kat_pressed(skip_lane)) {
        value = 0;
    }
    if (value < 0) {
        return;
    }

    if (skip_count == 0) {
        skip_count = 1;
        skip_last = value;
    } else if (value != skip_last) {
        skip_count++;
        skip_last = value;
    } else {
        return;
    }

    const bool skin_draws = background.has_value() && background->wants_skip();
    const SkinInfo* pos = skin_draws ? nullptr : tex.get_skin_config(SC::SKIP_COUNTER);
    if (pos) {
        skip_text = std::make_unique<OutlinedText>(std::to_string(skip_count) + "/" + std::to_string(SKIP_HITS),
                                                   (pos->font_size > 0 ? pos->font_size : 40), Color::WHITE,
                                                   Color::BLACK, false, 4);
    }

    if (skip_count >= SKIP_HITS) {
        do_skip();
    }
}

void GameScreen::do_skip() {
    skipped = true;
    const bool skin_draws = background.has_value() && background->wants_skip();
    const SkinInfo* used = skin_draws ? nullptr : tex.get_skin_config(SC::SKIP_USED_TEXT);
    if (used) {
        skip_text = std::make_unique<OutlinedText>(tex.skin_text("skip_used", global_data.config->general.language, ""),
                                                   (used->font_size > 0 ? used->font_size : 40), Color::WHITE,
                                                   Color::BLACK, false, 4);
    } else {
        skip_text.reset();
    }
    spdlog::info("skipped at {} ms", clock.position());
    if (song_music) {
        audio.stop_sound(*song_music);
    }
    if (movie) {
        movie->stop();
    }
    for (auto& player : players) {
        if (player) {
            player->cut_to_end(clock.position());
        }
    }
}

void GameScreen::zdon_sudden_death_check() {
    if (skipped || zdon_sudden_over || score_saved) {
        return;
    }
    int humans = 0;
    int out = 0;
    for (auto& player : players) {
        if (!player || player->is_zdon_ai()) {
            continue;
        }
        humans++;
        if (!player->zdon_sudden_failed()) {
            continue;
        }
        out++;
        if (!player->was_skipped()) { // just went out: its chart stops here
            player->cut_to_end(clock.position());
            zhud::toast(zhud::text("sudden_death_out"));
        }
    }
    if (humans == 0 || out < humans) {
        return;
    }
    zdon_sudden_over = true;
    spdlog::info("Sudden death: the song ends at {} ms", clock.position());
    if (song_music) {
        audio.stop_sound(*song_music);
    }
    if (movie) {
        movie->stop();
    }
    for (auto& player : players) { // the AI lane too (an AI battle is lost there)
        if (player && !player->was_skipped()) {
            player->cut_to_end(clock.position());
        }
    }
}

void GameScreen::draw_skip() {
    if (background.has_value() && background->wants_skip()) {
        return;
    }
    if (!skip_text || skip_count <= 0) {
        return;
    }
    const SkinInfo* pos = tex.get_skin_config(skipped ? SC::SKIP_USED_TEXT : SC::SKIP_COUNTER);
    if (!pos) {
        return;
    }
    skip_text->draw({.x = pos->x - skip_text->width / 2.0f, .y = pos->y});
}

void GameScreen::end_song(double end_time, double transition_delay, BackgroundNotification notification) {
    if (clock.position() >= end_time + 1000 && !score_saved) {
        for (const auto& player : players) {
            const ResultData& r = global_data.session_data[static_cast<int>(player->player_num)].result_data =
                player->get_result_score();
            // Z-DON: the final counts in the log (a pause must not change them)
            spdlog::info("Song result for player {}: score {} good {} ok {} bad {} max combo {} drumroll {}",
                         static_cast<int>(player->player_num), r.score, r.good, r.ok, r.bad, r.max_combo,
                         r.total_drumroll);
        }

        for (const auto& player : players) {
            if (player->is_zdon_ai()) {
                // Z-DON AI battle: the AI's play is never saved to the 2P profile (and is no "new best")
                global_data.session_data[static_cast<int>(player->player_num)].zdon_unsaved.clear();
                auto& rd = global_data.session_data[static_cast<int>(player->player_num)].result_data;
                rd.prev_score = rd.score;
                continue;
            }
            save_score(get_player_id(player->player_num), player->player_num);
        }

        for (const auto& player : players) {
            // Z-DON AI battle (skin_config zdon_ai_cutin_first, Nijiiro): the battle cut-in first, then the full combo
            // one
            if (global_data.zdon_ai.active && global_data.zdon_ai.finished &&
                tex.skin_config_by_name.contains("zdon_ai_cutin_first")) {
                // zdon_ai_banner: the skin draws the cabinet's own full combo banner after the cut-in (no Nijiiro
                // ending anim at all)
                ai_ending_pending = !tex.skin_config_by_name.contains("zdon_ai_banner");
            } else {
                player->spawn_ending_anim(background ? &*background : nullptr);
            }

            if (notification == BackgroundNotification::Enabled && background) {
                int g = player->get_good(), o = player->get_ok(), b = player->get_bad();
                background->handle_song_end(player->player_num, g, o, b, g + o + b);
            }
        }

        global_data.credit.songs_played += 1;
        score_saved = true;
        // Z-DON: a second after the last note the result screen's textures decode in the background, while the
        // ending plays (never while notes are still to be hit)
        tex.start_predecode("result");
        audio.predecode_screen_sounds("result");
    }

    if (ai_ending_pending && get_current_ms() >= global_data.zdon_ai.finished_at_ms + kZdonAiCutinMs) {
        ai_ending_pending = false;
        // the cabinet's own cut-in after the battle one is the full combo / donderful banner only
        for (const auto& player : players) {
            if (!player->is_zdon_ai() && player->get_bad() == 0) {
                player->spawn_ending_anim(background ? &*background : nullptr);
            }
        }
    }

    // zdon_ai_banner (Nijiiro): after a full combo win the result follows the skin's banner, 6.2 s after the battle
    // ended for a donderful (footage MBAZD6xA1D8: the emblem 156.2, black 162.4) and 5.0 s for a full combo with oks
    // (7ifN-P3qKOE: the result BGM 1.2 s earlier than after a donderful)
    const double ai_banner_ms = players[0]->get_ok() == 0 ? 6200.0 : 5000.0;
    const bool ai_banner_done = global_data.zdon_ai.active && global_data.zdon_ai.finished && global_data.zdon_ai.won &&
                                tex.skin_config_by_name.contains("zdon_ai_banner") && players[0]->get_bad() == 0 &&
                                get_current_ms() >= global_data.zdon_ai.finished_at_ms + ai_banner_ms;
    if ((clock.position() >= end_time + transition_delay || ai_banner_done) && !result_transition.is_started) {
        result_transition.start();
        ai_cover_start_ms = get_current_ms();
        audio.play_sound("result_transition", VolumePreset::VOICE);
        // skin_config zdon_ai_announce_at_fade (Green): the AI result announce sounds as the screen fades out, 4.7 s
        // before the result BGM (footage Tl7g8XXXvAM 273.94 / 492.73: announce at the fade, BGM +4.70)
        global_data.zdon_ai.announce_ms = -1.0;
        const auto at_fade = tex.skin_config_by_name.find("zdon_ai_announce_at_fade");
        if (global_data.zdon_ai.active && at_fade != tex.skin_config_by_name.end() && at_fade->second.x != 0.0f) {
            // the announce belongs to the result screen's sounds: load it here (the result screen reloads it)
            const fs::path announce = audio.sounds_path / "result" / "aivoice" / "announce.ogg";
            if (!audio.has_sound("aivoice_announce") && fs::exists(announce)) {
                audio.load_sound(announce, "aivoice_announce");
            }
        }
        if (global_data.zdon_ai.active && at_fade != tex.skin_config_by_name.end() && at_fade->second.x != 0.0f &&
            audio.has_sound("aivoice_announce")) {
            audio.play_sound("aivoice_announce", VolumePreset::VOICE);
            global_data.zdon_ai.announce_ms = get_current_ms();
        }
        spdlog::info("Result transition started and voice played");
    }
}

void GameScreen::update_gameplay(double current_ms) {
    poll_pending_song();
    if (transition->is_finished()) {
        start_song(clock.position());
        reset_input_lock();
    }
    if (zdon_record::active() && !players.empty()) {
        // the recording's progress (a course of a stacked recording ends with the longest course)
        zdon_record::note_song(clock.position(), std::max(players[0]->end_time, zdon_record::sync_end_ms()));
    }

    resync_song(current_ms);
    update_background(current_ms);

    // Z-DON test hook: ZDON_TEST_STALL="<song ms>:<ms>,..." blocks the loop once there, as dragging the window
    // does (the auto play catch-up in Player::update)
    static std::vector<std::pair<double, int>> stalls = [] {
        std::vector<std::pair<double, int>> v;
        if (const char* e = std::getenv("ZDON_TEST_STALL")) {
            std::string spec = e;
            for (size_t a = 0; a < spec.size();) {
                size_t b = spec.find(',', a);
                b = b == std::string::npos ? spec.size() : b;
                const std::string item = spec.substr(a, b - a);
                if (const size_t c = item.find(':'); c != std::string::npos) {
                    v.emplace_back(std::atof(item.substr(0, c).c_str()), std::atoi(item.substr(c + 1).c_str()));
                }
                a = b + 1;
            }
        }
        return v;
    }();
    if (!stalls.empty() && clock.position() >= stalls.front().first) {
        std::this_thread::sleep_for(std::chrono::milliseconds(stalls.front().second));
        stalls.erase(stalls.begin());
    }

    for (size_t i = 0; i < players.size(); ++i) {
        players[i]->update(lane_position(i), current_ms, background);
    }
    global_data.zdon_song_pos_ms = clock.position(); // Z-DON: the skin's song progress (zdon_lua.cpp)
    zdon_ghost_update();

    song_info.update(current_ms);
}

std::optional<Screens> GameScreen::update() {
    if (auto init = Screen::update()) {
        return init;
    }

    double current_ms = get_frame_ms();
    allnet_indicator.update(current_ms);
    if (pause_menu.active()) {
        return update_pause_menu(select_screen()); // Z-DON: the song, its video and the notes wait
    }
    clock.update(current_ms);
    zdon_lane3d::update_mouse();

    transition->update(current_ms);
    if (!pause_menu.active()) {
        update_gameplay(current_ms);
    }
    update_skip();
    zdon_sudden_death_check();
    result_transition.update(current_ms);

    if (result_transition.is_finished && !audio.is_sound_playing("result_transition")) {
        return on_screen_end(result_screen());
    }

    // Z-DON: a course of a stacked recording ends with the longest course (zdon_record.h --record-tracks)
    const double song_end = std::max(players[0]->end_time, zdon_record::sync_end_ms());
    if (clock.position() >= song_end && !hold_song_end()) {
        // Z-DON: skin_config "zdon_result_delay" {x = ms} replaces the 8533 ms song end -> result fade delay of a
        // normal (non-AI) play; Nijiiro: direct feeds YKjsle7VBCI (clear 545.28 s, FC 186.46 s) show the fade 7.05 s
        // after the banner's SE, 0.5 s earlier than the default's 7.53 s
        double normal_delay = 8533.34;
        if (!global_data.zdon_ai.active) {
            if (auto it = tex.skin_config_by_name.find("zdon_result_delay"); it != tex.skin_config_by_name.end()) {
                normal_delay = it->second.x;
            }
        }
        const double transition_delay = global_data.config->general.fast_transitions
                                            ? kFastResultTransitionDelayMs
                                            : (players[0]->was_skipped() ? 2800.0 : normal_delay);

        end_song(song_end, transition_delay, BackgroundNotification::Enabled);
    }
    return global_keys();
}

void GameScreen::draw_overlay(bool with_song_info) {
    draw_overlay_hud(with_song_info);
    draw_overlay_flat();
}

// the overlay's part that sits on the screen like the lanes (the 3D view's front layer)
void GameScreen::draw_overlay_hud(bool with_song_info) {
    if (with_song_info) {
        song_info.draw();
    }
    // Z-DON: a "constant speed" chip while it is on, and the toggle's message
    if (!players.empty() && players[0]->get_modifiers().equal_speed_bpm > 0) {
        const float s = tex.screen_scale;
        OutlinedText& t = zhud::label(zhud::text("equal_speed"), 22, Color{255, 226, 120, 255});
        const float w = t.width + 24 * s;
        const float h = t.height + 8 * s;
        const float x = tex.screen_width - w - 12 * s;
        const float y = tex.screen_height - h - 12 * s;
        zhud::panel(x, y, w, h, 0.75f, fade(Color{255, 226, 120, 255}, 0.7f));
        t.draw({.x = x + 12 * s, .y = y + 4 * s});
    }
    zdon_draw_ghost();
    if (!pause_menu.active()) {
        zdon_draw_target();
        zdon_draw_hit_error_bar();
    }
    zhud::draw_toast();
    draw_skip();
}

// what stays flat over the 3D view: transitions and the pause menu
void GameScreen::draw_overlay_flat() {
    bool over_transition = false;
    if (!transition->is_finished()) {
        transition->draw();
        over_transition = true;
    }
    if (over_transition) {
        global_data.in_transition = true;
    }
    allnet_indicator.draw();
    if (over_transition) {
        global_data.in_transition = false;
    }
    if (result_transition.is_started) {
        result_transition.draw();
    }
    pause_menu.draw(get_current_ms()); // Z-DON: the pause clock is wall time (pause_menu.h)
}

// Z-DON PB pace: record this play's score whenever it changes; the best play's curve is read on the first frame
void GameScreen::zdon_ghost_update() {
    const double t = clock.position();
    for (const auto& player : players) {
        if (!player || player->is_zdon_ai()) {
            continue;
        }
        ZdonGhost& g = zdon_ghost[std::clamp(static_cast<int>(player->player_num), 0, 3)];
        if (!g.loaded) {
            g.loaded = true;
            if (player->get_modifiers().zdon_ghost && !player->get_modifiers().auto_play) {
                const SessionData& sd = global_data.session_data[static_cast<int>(player->player_num)];
                g.best = scores_manager.get_zdon_ghost(sd.song_hash, sd.selected_difficulty,
                                                       get_player_id(player->player_num));
            }
        }
        if (!g.rec.empty() && t < g.rec.back().first - 500.0) {
            g.rec.clear(); // restarted
            g.last = -1;
        }
        const int score = player->get_score(); // Z-DON: not get_result_score (copies every hit offset)
        if (score != g.last) {
            g.last = score;
            g.rec.emplace_back(static_cast<float>(t), score);
        }
    }
}

static std::string zdon_signed_thousands(int v) {
    std::string d = std::to_string(std::abs(v));
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3) {
        d.insert(static_cast<size_t>(i), ",");
    }
    return (v > 0 ? "+" : v < 0 ? "-" : "±") + d;
}

void GameScreen::zdon_draw_ghost() const {
    const double t = clock.position();
    const auto cfg = tex.skin_config_by_name.find("zdon_ghost");
    for (size_t i = 0; i < players.size(); ++i) {
        const auto& player = players[i];
        if (!player) {
            continue;
        }
        const ZdonGhost& g = zdon_ghost[std::clamp(static_cast<int>(player->player_num), 0, 3)];
        if (g.best.empty()) {
            continue;
        }
        auto it = std::ranges::upper_bound(g.best, static_cast<float>(t), {}, &std::pair<float, int>::first);
        const int best = it == g.best.begin() ? 0 : std::prev(it)->second;
        const int diff = player->get_score() - best;
        const Color c = diff > 0 ? Color{120, 236, 140, 255} : diff < 0 ? Color{255, 120, 110, 255} : Color::WHITE;
        OutlinedText& txt = zhud::label(zhud::text("ghost_diff", zdon_signed_thousands(diff)), 24, c);
        const float s = tex.screen_scale;
        const float w = txt.width + 24 * s;
        const float h = txt.height + 8 * s;
        const float lane_y = i == 0 ? 184 * s : tex.get_skin_config(SC::GAME_P2_Y)->y;
        // under the lane (169 px tall at 720p in both arcade skins); 2P play: 1P's over its lane, 2P's lane stays clear
        float x = tex.screen_width - w - 12 * s;
        float y = players.size() == 2 && i == 0 ? lane_y - h - 6 * s : lane_y + 174 * s;
        if (cfg != tex.skin_config_by_name.end()) {
            x = cfg->second.x;
            y = cfg->second.y + (lane_y - 184 * s);
        }
        zhud::panel(x, y, w, h, 0.72f, fade(c, 0.7f));
        txt.draw({.x = x + 12 * s, .y = y + 4 * s});
    }
}

void GameScreen::draw_players() {
    if (players.size() == 1) {
        players[0]->draw(clock.position(), 0, 184 * tex.screen_scale);
    } else if (players.size() == 2) {
        players[0]->draw(lane_position(0), 0, 184 * tex.screen_scale);
        players[1]->draw(lane_position(1), 0, tex.get_skin_config(SC::GAME_P2_Y)->y);
        // the shared kusudama covers both lanes, so it goes over the 2P lane too
        if (zdon_lane3d::draws(zdon_lane3d::Layer::EFFECTS)) {
            for (const auto& player : players) {
                player->draw_shared_kusudama();
            }
        }
    } else {
        float gap = (tex.screen_height - (players.size() * 176 * tex.screen_scale)) / (players.size() + 1);
        for (int i = 0; i < players.size(); i++) {
            float position = gap + i * ((176 * tex.screen_scale) + gap);
            players[i]->draw(clock.position(), 0, position);
        }
    }
}

void GameScreen::draw_back() {
    if (movie) {
        movie->draw();
    } else if (has_movie()) {
        clear_background(Color::BLACK); // what the movie draws before its first frame
    } else if (background.has_value()) {
        background->draw_back();
    } else {
        clear_background(Color::BLACK);
    }
}

void GameScreen::draw() {
    zdon_set_lane3d_pivot();
    if (zdon_lane3d::begin_frame()) { // each layer on its own plane (zdon_lane3d.h)
        using zdon_lane3d::Layer;
        clear_background(Color::BLACK); // what a turned background leaves uncovered
        zdon_lane3d::begin_layer(Layer::BACKGROUND);
        draw_back();
        zdon_lane3d::end_layer();
        for (const Layer layer :
             {Layer::LANE, Layer::TARGET, Layer::NOTES, Layer::JUDGMENTS, Layer::HUD, Layer::EFFECTS}) {
            zdon_lane3d::begin_layer(layer);
            draw_players();
            zdon_lane3d::end_layer();
        }
        if (background.has_value()) {
            zdon_lane3d::begin_layer(Layer::FOREGROUND);
            background->draw_fore();
            zdon_lane3d::end_layer();
        }
        zdon_lane3d::begin_layer(Layer::OVERLAY);
        draw_overlay_hud(true);
        zdon_lane3d::end_layer();
        zdon_lane3d::end_frame();
        draw_overlay_flat();
        return;
    }
    draw_back();
    draw_players();
    if (background.has_value()) {
        background->draw_fore();
    }
    draw_overlay();
}
