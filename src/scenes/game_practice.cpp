#include "game_practice.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <ranges>

#include "../libs/input/input.h"
#include "../libs/math/animation.h"
#include "../objects/game/zdon_lane3d.h"
#include "../objects/global/practice_timer.h"
#include "../objects/global/zdon_hud.h"

void PracticeGameScreen::init_practice_textures() {
    for (int t = 0; t <= 9; ++t) {
        std::string name = "notes/" + std::to_string(t);
        t_notes[t] = tex.has_texture(name) ? tex.get_texture(name) : nullptr;
    }
    t_notes_0 = tex.get_texture("notes/0");
    t_notes_8 = tex.get_texture("notes/8");
    t_notes_9 = tex.get_texture("notes/9");
    t_notes_10 = tex.get_texture("notes/10");
    t_drumroll_big_tail = tex.get_texture("notes/drumroll_big_tail");
    t_drumroll_tail = tex.get_texture("notes/drumroll_tail");
    t_moji = tex.get_texture("notes/moji_" + global_data.config->general.language);
    t_moji_drumroll_mid = tex.get_texture("notes/moji_drumroll_mid");

    t_large_drum = tex.get_texture("practice/large_drum");
    t_pause_don = tex.get_texture("practice/pause_don");
    t_pause_kat = tex.get_texture("practice/pause_kat");
    t_resume_don = tex.get_texture("practice/resume_don");
    t_skip_l_kat = tex.get_texture("practice/skip_l_kat");
    t_skip_r_kat = tex.get_texture("practice/skip_r_kat");
    t_menu_don = tex.get_texture("practice/menu_don");
    t_speed_r_kat = tex.get_texture("practice/speed_r_kat");
    t_speed_l_kat = tex.get_texture("practice/speed_l_kat");
    t_confirm = tex.get_texture("practice/confirm");
    t_delete = tex.get_texture("practice/delete");
    t_finish = tex.get_texture("practice/finish");
    t_jump_point_editing = tex.get_texture("practice/jump_point_editing");
    t_jump_point_arrow = tex.get_texture("practice/jump_point_arrow");
    t_playing = tex.get_texture("practice/playing");
    t_progress_bar_bg = tex.get_texture("practice/progress_bar_bg");
    t_progress_bar = tex.get_texture("practice/progress_bar");
    t_gogo_marker = tex.get_texture("practice/gogo_marker");
    t_jump_point_progress = tex.get_texture("practice/jump_point_progress");
    t_bar_count = tex.get_texture("practice/bar_count");
    t_bar_divider = tex.get_texture("practice/bar_divider");
    t_bar_count_bar = tex.get_texture("practice/bar_count_bar");
    t_song_tempo = tex.get_texture("practice/song_tempo");
    t_dot = tex.get_texture("practice/dot");
    t_multiplier = tex.get_texture("practice/multiplier");
    t_bar_label = tex.get_texture("practice/bar_label");
    t_paused = tex.get_texture("practice/paused");
    t_tempo_rate = tex.get_texture("practice/tempo_rate");
    t_tempo_dot = tex.get_texture("practice/tempo_dot");
    t_tempo_dot_on = tex.get_texture("practice/tempo_dot_on");
    t_skip_l_kat_off = tex.get_texture("practice/skip_l_kat_off");
    t_skip_r_kat_off = tex.get_texture("practice/skip_r_kat_off");
    t_menu_arrow = tex.get_texture("practice/menu_arrow");
    t_menu_decide = tex.get_texture("practice/menu_decide");
    t_menu_panel = tex.get_texture("practice/menu_panel");
    t_gauge_max = tex.get_texture("practice/gauge_max");
    t_jump_count = tex.get_texture("practice/jump_count");
    t_jump_count_digits = tex.get_texture("practice/jump_count_digits");
    t_seek_limit = tex.get_texture("practice/seek_limit");
    t_review_panel = tex.get_texture("practice/review_panel");
    t_renda_slash = tex.get_texture("practice/renda_slash");
    t_renda_digits = tex.get_texture("practice/renda_digits");
    t_renda_digits_small = tex.get_texture("practice/renda_digits_small");
    for (int i = 0; i < 3; i++) {
        t_jump_buttons[i] = tex.get_texture("practice/jump_" + std::to_string(i + 1));
        t_review_cards[i] = tex.get_texture("practice/review_card_" + std::to_string(i + 1));
        t_renda_bubbles[i] = tex.get_texture("practice/renda_bubble_" + std::to_string(i + 1));
    }
}

void PracticeGameScreen::on_screen_start() {
    GameScreen::on_screen_start();
    global_data.zdon_loop = {}; // Z-DON: no section loop carried over from the last practice
    init_practice_textures();
    menu.init_textures();
    pause_don_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    pause_kat_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    resume_don_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    skip_l_kat_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    skip_r_kat_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    menu_don_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    speed_l_kat_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    speed_r_kat_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    mark_action_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    mark_finish_anim = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(67, true));
    PracticeTimer::begin_session(get_frame_ms());
    hide_cursor();                // Hide cursor when entering practice mode
    set_touch_drum_enabled(true); // Enable touch drum (input + drawing) when entering practice mode
    initialize_practice_chart();
}

Screens PracticeGameScreen::on_screen_end(Screens next_screen) {
    if (next_screen != Screens::GAME_PRACTICE && next_screen != Screens::PRACTICE_SELECT) {
        PracticeTimer::end_session();
    }
    scrobble_index = 0;
    scrobble_time = 0;
    phase = Phase::PRACTICE;
    tempo_change_ms = seek_limit_ms = review_start_ms = -1e9;
    max_progress = 0;
    jump_bars.fill(-1);
    menu = PracticeMenu();
    scrobble_move = std::make_unique<MoveAnimation>(200.0, 0);
    bars.clear();
    scrobble_note_list.clear();
    markers.clear();
    zdon_branch_force = -1;
    zdon_bar_input_open = false;
    zdon_bar_input.clear();
    zdon_fade_start = -1.0;
    hide_cursor();                // Hide cursor when leaving practice mode
    set_touch_drum_enabled(true); // Re-enable touch drum (input + drawing) when leaving practice mode
    return GameScreen::on_screen_end(next_screen);
}

void PracticeGameScreen::release_chart_resources() {
    practice_player = nullptr;
}

void PracticeGameScreen::create_players() {
    const auto player_index = static_cast<int>(global_data.player_num);
    players.push_back(std::make_unique<PracticePlayer>(parser, global_data.player_num,
                                                       global_data.session_data[player_index].selected_difficulty,
                                                       false, get_player_modifiers(global_data.player_num)));

    players.back()->allow_papamama();
    practice_player = static_cast<PracticePlayer*>(players.back().get());
}

void PracticeGameScreen::initialize_practice_chart() {
    int difficulty = global_data.session_data[static_cast<int>(global_data.player_num)].selected_difficulty;
    auto [notes, bm, be, bn] = parser->notes_to_position(difficulty);
    // Z-DON: the chart's own tempo per note, before constant speed rewrites note.bpm (the info strip)
    zdon_tempo.clear();
    for (const Note& n : notes.notes) {
        zdon_tempo.emplace_back(n.hit_ms, n.bpm);
    }
    for (const auto* branch : {&bm, &be, &bn}) {
        for (const NoteList& section : *branch) {
            for (const Note& n : section.notes) {
                zdon_tempo.emplace_back(n.hit_ms, n.bpm);
            }
        }
    }
    std::ranges::sort(zdon_tempo);
    // This used to set the TJA parser's scroll_disabled here, after the parse. The screen's parser is never
    // parsed again (initialize_chart makes a new one), and the player had its own copy, so it changed nothing. The
    // player now shares this parser, and setting it would change the player's later re-parses.
    const Modifiers& practice_modifiers = get_player_modifiers(global_data.player_num);
    apply_modifiers(notes, practice_modifiers);

    base_chart = std::move(notes);
    branch_m_all.assign(bm.begin(), bm.end());
    branch_e_all.assign(be.begin(), be.end());
    branch_n_all.assign(bn.begin(), bn.end());
    for (auto* branch : {&branch_m_all, &branch_e_all, &branch_n_all}) {
        for (NoteList& section : *branch) {
            apply_modifiers(section, practice_modifiers);
        }
    }
    branch_display_synced = static_cast<size_t>(-1); // force a full rebuild below

    sync_branch_display();

    if (!bars.empty()) {
        scrobble_index = 0;
        scrobble_time = bars[0].hit_ms;
    }
    scrobble_move = std::make_unique<MoveAnimation>(200.0, 0);
}

void PracticeGameScreen::sync_branch_display() {
    size_t resolved = practice_player ? practice_player->branch_history.size() : 0;
    if (branch_display_synced == resolved) {
        return;
    }

    auto ms_of = [&](int index) -> double {
        return (index >= 0 && index < std::ssize(bars)) ? bars[index].hit_ms : -1.0;
    };
    double cursor_ms = ms_of(scrobble_index);
    double jump_arrow_ms = ms_of(jump_arrow_bar);
    std::array<double, PracticeMenu::MARK_SLOTS> mark_ms;
    for (size_t i = 0; i < jump_bars.size(); ++i) {
        mark_ms[i] = ms_of(jump_bars[i]);
    }

    scrobble_note_list.assign(base_chart.notes.begin(), base_chart.notes.end());
    bars.clear();
    markers.clear();

    size_t checkpoints = std::max({branch_m_all.size(), branch_e_all.size(), branch_n_all.size()});
    for (size_t i = 0; i < checkpoints; ++i) {
        BranchDifficulty chosen = (practice_player && i < practice_player->branch_history.size())
                                      ? practice_player->branch_history[i]
                                      : BranchDifficulty::MASTER; // not decided yet: preview the master branch
        const NoteList* section = nullptr;
        if (chosen == BranchDifficulty::EXPERT && i < branch_e_all.size()) {
            section = &branch_e_all[i];
        } else if (chosen == BranchDifficulty::MASTER && i < branch_m_all.size()) {
            section = &branch_m_all[i];
        } else if (chosen == BranchDifficulty::NORMAL && i < branch_n_all.size()) {
            section = &branch_n_all[i];
        }

        if (!section) {
            if (i < branch_m_all.size()) {
                section = &branch_m_all[i];
            } else if (i < branch_n_all.size()) {
                section = &branch_n_all[i];
            } else if (i < branch_e_all.size()) {
                section = &branch_e_all[i];
            }
        }
        if (!section) {
            continue;
        }

        scrobble_note_list.insert(scrobble_note_list.end(), section->notes.begin(), section->notes.end());
        for (const auto& tl : section->timeline) {
            if (tl.gogo_time.has_value() && tl.gogo_time.value()) {
                markers.push_back(tl.start_time);
            }
        }
    }

    std::ranges::sort(scrobble_note_list, {}, &Note::hit_ms);
    std::ranges::sort(markers);
    for (const Note& note : scrobble_note_list) {
        if (note.type == NoteType::BARLINE) {
            bars.push_back(note);
        }
    }

    auto reindex = [&](double ms) {
        if (ms < 0) {
            return -1;
        }
        for (int i = 0; i < std::ssize(bars); ++i) {
            if (bars[i].hit_ms == ms) {
                return i;
            }
        }
        return -1;
    };
    if (cursor_ms >= 0) {
        scrobble_index = std::max(0, reindex(cursor_ms));
    }
    jump_arrow_bar = reindex(jump_arrow_ms);
    for (size_t i = 0; i < jump_bars.size(); ++i) {
        jump_bars[i] = reindex(mark_ms[i]);
    }

    branch_display_synced = resolved;
}

void PracticeGameScreen::pause_practice() {
    if (clock.is_paused()) {
        return;
    }

    pause_playback();
    global_data.zdon_loop.active = false; // Z-DON: a pause ends the section loop (the last pass stays shown)

    if (practice_player) {
        practice_player->paused = true;
    }

    set_touch_drum_enabled(false);
    show_cursor();

    if (bars.empty()) {
        return;
    }

    double first_bar_time = bars[0].hit_ms;
    int nearest_bar_index = 0;
    double min_distance = std::numeric_limits<double>::infinity();

    for (std::size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const double bar_relative_time = bar.hit_ms - first_bar_time;
        const double distance = std::abs(bar_relative_time - clock.position());

        if (distance < min_distance) {
            min_distance = distance;
            nearest_bar_index = static_cast<int>(i);
        }
    }

    scrobble_index = std::max(0, nearest_bar_index - 1);
    scrobble_time = bars[scrobble_index].hit_ms;
}

void PracticeGameScreen::resume_practice(Side don_side) {
    if (!clock.is_paused()) {
        return;
    }
    zdon_don_wait_ms = -1.0;

    if (bars.empty()) {
        global_data.zdon_loop = {};
        resume_playback();
    } else {
        int resume_bar_index = std::clamp(scrobble_index, 0, static_cast<int>(bars.size()) - 1);
        zdon_loop_start(resume_bar_index);
        seek_and_play_from_bar(resume_bar_index);
    }

    if (practice_player) {
        practice_player->paused = false;
        practice_player->spawn_scrobble_effect(DrumType::DON, don_side, static_cast<int>(global_data.player_num) - 1);
    }

    set_touch_drum_enabled(true);
    hide_cursor();
    resume_don_anim->start();
}

void PracticeGameScreen::seek_and_play_from_bar(int resume_bar_index) {
    int previous_bar_index = std::max(0, resume_bar_index - global_data.config->general.practice_mode_bar_delay);
    // Z-DON: at least 2 s of run-up before the bar (fast songs: more bars)
    while (previous_bar_index > 0 && bars[resume_bar_index].hit_ms - bars[previous_bar_index].hit_ms < 2000.0) {
        previous_bar_index--;
    }

    double first_bar_time = bars[0].hit_ms;
    double resume_time = bars[resume_bar_index].hit_ms - first_bar_time + start_delay;
    double start_time = bars[previous_bar_index].hit_ms - first_bar_time + start_delay;

    if (practice_player) {
        practice_player->seek_to(resume_time);
        practice_player->reset_performance();
    }

    resume_playback(start_time);
    // Z-DON: the music fades in over the run-up instead of starting mid-note at full volume
    zdon_fade_start = get_current_ms();
    zdon_update_fade(zdon_fade_start);
}

// Z-DON: resumed exactly on a jump mark with a later mark set: loop A..B
void PracticeGameScreen::zdon_loop_start(int resume_bar_index) {
    auto& loop = global_data.zdon_loop;
    loop = {};
    if (!global_data.config->general.zdon_practice_loop) {
        return;
    }
    if (std::ranges::find(jump_bars, resume_bar_index) == jump_bars.end()) {
        return;
    }
    int next = -1;
    for (int b : jump_bars) {
        if (b > resume_bar_index && (next < 0 || b < next)) {
            next = b;
        }
    }
    loop.active = true;
    loop.a_bar = resume_bar_index;
    if (next < 0 || next >= static_cast<int>(bars.size())) {
        // Z-DON: only A: loop from A to the end of the song
        loop.b_bar = static_cast<int>(bars.size()) - 1;
        loop.to_end = true;
        zhud::toast(zhud::text("loop_to_end", std::to_string(resume_bar_index + 1)));
        return;
    }
    loop.b_bar = next;
}

// Z-DON: at the B mark: keep the pass's judgements, go back to A (as a resume at A would)
void PracticeGameScreen::zdon_loop_update() {
    auto& loop = global_data.zdon_loop;
    if (!loop.active || clock.is_paused() || bars.empty() || loop.b_bar >= static_cast<int>(bars.size())) {
        return;
    }
    const double b_time = loop.to_end ? (players.empty() ? 0.0 : players[0]->end_time) + 1500.0
                                      : bars[loop.b_bar].hit_ms - bars[0].hit_ms + start_delay;
    if (clock.position() < b_time) {
        return;
    }
    if (practice_player) {
        ResultData r = practice_player->get_result_score();
        loop.last_good = r.good;
        loop.last_ok = r.ok;
        loop.last_bad = r.bad;
        loop.has_last = true;
        // the pass history: the last few passes and the best one (good 2, ok 1), formatted once here
        using Loop = std::remove_reference_t<decltype(loop)>;
        const int pass = loop.passes + 1;
        loop.history[static_cast<size_t>(loop.history_head)] = zhud::text(
            "loop_pass", std::to_string(pass), std::to_string(r.good), std::to_string(r.ok), std::to_string(r.bad));
        loop.history_head = (loop.history_head + 1) % Loop::HISTORY;
        loop.history_count = std::min(loop.history_count + 1, Loop::HISTORY);
        if (const int score = (r.good * 2) + r.ok; score > loop.best_score) {
            loop.best_score = score;
            loop.best_line = zhud::text("loop_best", std::to_string(pass), std::to_string(r.good), std::to_string(r.ok),
                                        std::to_string(r.bad));
        }
    }
    loop.passes++;
    pause_playback();
    seek_and_play_from_bar(loop.a_bar);
}

void PracticeGameScreen::open_practice_menu() {
    zdon_don_wait_ms = -1.0;
    menu_don_anim->start();
    menu.open_menu();
}

void PracticeGameScreen::change_practice_speed(bool down, bool up) {
    if (down) {
        song_speed = std::max(MIN_SONG_SPEED, song_speed - 1);
        speed_l_kat_anim->start();
    }

    if (up) {
        song_speed = std::min(MAX_SONG_SPEED, song_speed + 1);
        speed_r_kat_anim->start();
    }
    tempo_change_ms = now_ms;

    if (song_music) {
        audio.set_sound_pitch(*song_music, song_speed / 10.0f);
    }
}

void PracticeGameScreen::restart_practice() {
    restart_song();
    initialize_practice_chart();
    if (zdon_branch_force >= 0) {
        zdon_apply_branch_force();
    }
    max_progress = 0;
    hide_cursor();                // Hide cursor on restart
    set_touch_drum_enabled(true); // Re-enable touch drum (input + drawing) on restart
    menu.close();
}

std::optional<Screens> PracticeGameScreen::handle_menu_action(PracticeMenu::Action action) {
    switch (action) {
    case PracticeMenu::Action::END_GAME:
        if (auto* session = PracticeTimer::active(); session && phase == Phase::PRACTICE && !session->is_up()) {
            session->expire();
            menu.close();
            return std::nullopt;
        }
        return on_screen_end(Screens::ENTRY);
    case PracticeMenu::Action::ANOTHER_SONG:
        return on_screen_end(Screens::PRACTICE_SELECT);
    case PracticeMenu::Action::RESTART:
        restart_practice();
        return std::nullopt;
    case PracticeMenu::Action::AUTO_ON:
        if (practice_player) {
            practice_player->set_auto_play(true);
        }
        return std::nullopt;
    case PracticeMenu::Action::AUTO_OFF:
        if (practice_player) {
            practice_player->set_auto_play(false);
        }
        return std::nullopt;
    case PracticeMenu::Action::JUMP_TO_MARK: {
        bool any_mark = false;
        for (int b : jump_bars) {
            if (b >= 0 && b < std::ssize(bars)) {
                any_mark = true;
                break;
            }
        }
        if (any_mark) {
            menu.open_jump_mode();
        }
        return std::nullopt;
    }
    case PracticeMenu::Action::SET_MARK:
        menu.open_mark_edit();
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void PracticeGameScreen::animate_scrobble_to(int new_index) {
    if (bars.empty()) {
        return;
    }
    if (scrobble_move->is_started && !scrobble_move->is_finished) {
        scrobble_time = bars[scrobble_index].hit_ms;
    }
    double time_difference = bars[new_index].hit_ms - bars[scrobble_index].hit_ms;
    scrobble_index = new_index;
    scrobble_move = std::make_unique<MoveAnimation>(400.0, static_cast<int>(time_difference), false, false, 0, 0.0,
                                                    std::nullopt, std::nullopt, EaseType::Quadratic);
    scrobble_move->start();
}

void PracticeGameScreen::scrobble_step_bar(bool right, int count) {
    if (bars.empty()) {
        return;
    }
    audio.play_sound("kat", VolumePreset::HITSOUND);
    if (right) {
        skip_r_kat_anim->start();
    } else {
        skip_l_kat_anim->start();
    }
    if (practice_player) {
        int player_idx = static_cast<int>(global_data.player_num) - 1;
        if (right) {
            practice_player->spawn_scrobble_effect(DrumType::KAT, Side::RIGHT, player_idx);
        } else {
            practice_player->spawn_scrobble_effect(DrumType::KAT, Side::LEFT, player_idx);
        }
    }
    if (!right && at_first_bar()) {
        animate_scrobble_to(static_cast<int>(bars.size()) - 1);
        return;
    }
    if (right && at_last_bar()) {
        seek_limit_ms = now_ms;
        return;
    }
    const auto n = static_cast<int>(bars.size());
    animate_scrobble_to(std::clamp(scrobble_index + (right ? count : -count), 0, n - 1));
}

void PracticeGameScreen::review_jump(int card) {
    if (bars.empty()) {
        return;
    }
    int target = 0;
    if (card == 1) {
        double middle = (bars.front().hit_ms + bars.back().hit_ms) / 2.0;
        auto it = std::ranges::min_element(bars, {}, [&](const Note& b) { return std::abs(b.hit_ms - middle); });
        target = static_cast<int>(std::distance(bars.begin(), it));
    } else if (card == 2) {
        target = static_cast<int>(bars.size()) - 1;
    }
    TextureResizeAnimation* anims[3] = {speed_l_kat_anim, menu_don_anim, speed_r_kat_anim};
    anims[card]->start();
    audio.play_sound(card == 1 ? "don" : "kat", VolumePreset::HITSOUND);
    if (target != scrobble_index) {
        animate_scrobble_to(target);
    }
}

void PracticeGameScreen::start_last_play() {
    phase = Phase::LAST_PLAY;
    if (auto* session = PracticeTimer::active()) {
        session->hidden = true;
    }
    restart_practice();
}

void PracticeGameScreen::start_review() {
    phase = Phase::REVIEW;
    pause_practice();
    if (!bars.empty()) {
        scrobble_index = 0;
        scrobble_time = bars[0].hit_ms;
        scrobble_move = std::make_unique<MoveAnimation>(200.0, 0);
    }
    review_start_ms = now_ms;
    if (auto* session = PracticeTimer::active()) {
        session->hidden = false;
        session->start_review(REVIEW_SECONDS, now_ms);
    }
}

static constexpr double REVIEW_PANEL_OPEN_MS = 83, REVIEW_PANEL_CLOSE_AT_MS = 1150, REVIEW_PANEL_CLOSE_MS = 67;

bool PracticeGameScreen::review_panel_open() const {
    return phase == Phase::REVIEW && now_ms - review_start_ms < REVIEW_PANEL_CLOSE_AT_MS + REVIEW_PANEL_CLOSE_MS;
}

// Check if a point is inside a rectangle
static bool point_in_rect(float x, float y, float rect_x, float rect_y, float rect_w, float rect_h) {
    return x >= rect_x && x <= rect_x + rect_w && y >= rect_y && y <= rect_y + rect_h;
}

// Get the screen-space rectangle for a practice mode UI element
static Rect get_practice_button_rect(TextureObject* tex_obj, int index, float scale = 1.0f) {
    if (!tex_obj || index < 0 || static_cast<size_t>(index) >= tex_obj->x.size()) {
        return {0, 0, 0, 0};
    }

    float x = tex_obj->x[index];
    float y = tex_obj->y[index];
    float w = tex_obj->x2[index] * scale;
    float h = tex_obj->y2[index] * scale;

    // Convert from virtual coordinates to screen coordinates
    int screen_x = virtual_to_screen_x(x);
    int screen_y = virtual_to_screen_y(y);
    const float screen_scale = std::min(static_cast<float>(get_screen_width()) / tex.screen_width,
                                        static_cast<float>(get_screen_height()) / tex.screen_height);
    int screen_w = static_cast<int>(w * screen_scale);
    int screen_h = static_cast<int>(h * screen_scale);

    return {static_cast<float>(screen_x), static_cast<float>(screen_y), static_cast<float>(screen_w),
            static_cast<float>(screen_h)};
}

// Handle mouse/touch input for practice mode UI buttons
std::optional<Screens> PracticeGameScreen::handle_mouse_input() {
    if (!clock.is_paused() || (phase != Phase::PRACTICE && !menu.open)) {
        return std::nullopt;
    }

    // Get mouse/touch position
    Vec2 mouse_pos = mouse_position();
    bool clicked = mouse_pressed(SDL_BUTTON_LEFT);
    Camera2D camera;

    int player_idx = (global_data.player_num == PlayerNum::P1) ? 0 : 1;
    int other_idx = (global_data.player_num == PlayerNum::P1) ? 1 : 0;

    // If menu is open, let the menu handle mouse input
    if (menu.open) {
        auto action =
            menu.handle_mouse_input(camera, mouse_pos, clicked, practice_player && practice_player->is_auto_play());
        return handle_menu_action(action);
    }

    auto button_hit = [&](TextureObject* tex_obj, int index, const TextureResizeAnimation* anim) {
        auto rect = get_practice_button_rect(tex_obj, index, anim ? static_cast<float>(anim->attribute) : 1.0f);
        return point_in_rect(mouse_pos.x, mouse_pos.y, rect.x, rect.y, rect.width, rect.height);
    };

    // Also check for touch input (raylib handles touch as mouse on mobile)
    // Note: touch input is already converted to virtual keys in input.cpp,
    // but we also want to allow direct clicking on the practice drums when paused

    // Check resume_don (index = player_idx)
    if (button_hit(t_resume_don, player_idx, resume_don_anim)) {
        if (clicked) {
            resume_practice();
            audio.play_sound("don", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    // Check skip_l_kat (index = player_idx * 2)
    if (button_hit(t_skip_l_kat, player_idx * 2, skip_l_kat_anim)) {
        if (clicked && !bars.empty()) {
            scrobble_step_bar(false);
        }
        return std::nullopt;
    }

    // Check skip_r_kat (index = player_idx * 2 + 1)
    if (button_hit(t_skip_r_kat, player_idx * 2 + 1, skip_r_kat_anim)) {
        if (clicked && !bars.empty()) {
            scrobble_step_bar(true);
        }
        return std::nullopt;
    }

    // Check menu_don (index = other_idx)
    if (button_hit(t_menu_don, other_idx, menu_don_anim)) {
        if (clicked) {
            open_practice_menu();
            audio.play_sound("don", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    // Check speed_l_kat (index = other_idx * 2 + 1)
    if (button_hit(t_speed_l_kat, other_idx * 2 + 1, speed_l_kat_anim)) {
        if (clicked) {
            change_practice_speed(true, false);
            audio.play_sound("kat", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    // Check speed_r_kat (index = other_idx * 2)
    if (button_hit(t_speed_r_kat, other_idx * 2, speed_r_kat_anim)) {
        if (clicked) {
            change_practice_speed(false, true);
            audio.play_sound("kat", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    // Also allow clicking on the large drums (index 0 and 1 for player)
    // Left drum (index = player_idx * 2) - DON
    if (button_hit(t_large_drum, player_idx * 2, nullptr)) {
        if (clicked && practice_player) {
            practice_player->spawn_hit_effects(DrumType::DON, Side::LEFT);
            practice_player->spawn_scrobble_effect(DrumType::DON, Side::LEFT, player_idx);
            audio.play_sound("don", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    // Right drum (index = player_idx * 2 + 1) - DON
    if (button_hit(t_large_drum, player_idx * 2 + 1, nullptr)) {
        if (clicked && practice_player) {
            practice_player->spawn_hit_effects(DrumType::DON, Side::RIGHT);
            practice_player->spawn_scrobble_effect(DrumType::DON, Side::RIGHT, player_idx);
            audio.play_sound("don", VolumePreset::HITSOUND);
        }
        return std::nullopt;
    }

    return std::nullopt;
}

// "Other player" practice controls (pause / menu / speed / mark-edit exit) must
// not fire from the playing player's own drum. The shipped config gives
// [gamepad_2p] the same buttons as [gamepad_1p] (and keyboard 2P keys can
// coincide too), so a binding shared with the playing player for the same
// role is ignored. With no overlap this is the plain per-player check.
enum class PracticeDrumRole {
    L_DON,
    R_DON,
    L_KAT,
    R_KAT
};

static const std::vector<int>& practice_role_binding(const DrumBindings& b, PracticeDrumRole role) {
    switch (role) {
    case PracticeDrumRole::L_DON:
        return b.left_don;
    case PracticeDrumRole::R_DON:
        return b.right_don;
    case PracticeDrumRole::L_KAT:
        return b.left_kat;
    case PracticeDrumRole::R_KAT:
        break;
    }
    return b.right_kat;
}

static std::vector<int> practice_distinct_binding(const std::vector<int>& other, const std::vector<int>& own) {
    std::vector<int> out;
    for (int k : other) {
        if (std::ranges::find(own, k) == own.end()) {
            out.push_back(k);
        }
    }
    return out;
}

static bool other_drum_pressed(PlayerNum self, PracticeDrumRole role) {
    const auto& c = *global_data.config;
    bool self_p1 = self != PlayerNum::P2;
    const DrumBindings& own_keys = self_p1 ? c.keys_1p : c.keys_2p;
    const DrumBindings& own_pad = self_p1 ? c.gamepad_1p : c.gamepad_2p;
    const DrumBindings& other_keys = self_p1 ? c.keys_2p : c.keys_1p;
    const DrumBindings& other_pad = self_p1 ? c.gamepad_2p : c.gamepad_1p;

    const std::vector<int>& ok = practice_role_binding(other_keys, role);
    const std::vector<int>& op = practice_role_binding(other_pad, role);
    std::vector<int> keys = practice_distinct_binding(ok, practice_role_binding(own_keys, role));
    std::vector<int> pads = practice_distinct_binding(op, practice_role_binding(own_pad, role));

    if (keys.size() == ok.size() && pads.size() == op.size()) {
        PlayerNum other = self_p1 ? PlayerNum::P2 : PlayerNum::P1;
        switch (role) {
        case PracticeDrumRole::L_DON:
            return is_l_don_pressed(other);
        case PracticeDrumRole::R_DON:
            return is_r_don_pressed(other);
        case PracticeDrumRole::L_KAT:
            return is_l_kat_pressed(other);
        case PracticeDrumRole::R_KAT:
            break;
        }
        return is_r_kat_pressed(other);
    }
    return is_input_key_pressed(keys, pads);
}

static bool other_don_pressed(PlayerNum self) {
    return other_drum_pressed(self, PracticeDrumRole::L_DON) || other_drum_pressed(self, PracticeDrumRole::R_DON);
}

std::optional<Screens> PracticeGameScreen::global_keys_practice() {
    if (zdon_practice_keys()) {
        return std::nullopt;
    }
    if (phase == Phase::PRACTICE && check_key_pressed(global_data.config->keys.restart_key)) {
        restart_practice();
        return std::nullopt;
    }

    if (check_key_pressed(global_data.config->keys.back_key)) {
        return on_screen_end(Screens::PRACTICE_SELECT);
    }

    PlayerNum other_player = (global_data.player_num == PlayerNum::P1) ? PlayerNum::P2 : PlayerNum::P1;
    int other_idx = static_cast<int>(other_player) - 1;
    int player_idx = static_cast<int>(global_data.player_num) - 1;

    if (phase == Phase::LAST_PLAY || review_panel_open()) {
        return std::nullopt;
    }
    if (phase == Phase::REVIEW && !menu.open) {
        bool step_l = is_l_kat_pressed(global_data.player_num);
        bool step_r = is_r_kat_pressed(global_data.player_num);
        if (step_l || step_r) {
            scrobble_step_bar(!step_l && step_r);
        }
        if (is_l_don_pressed(global_data.player_num) || is_r_don_pressed(global_data.player_num)) {
            audio.play_sound("don", VolumePreset::HITSOUND);
            mark_finish_anim->start();
            menu.open_review_end();
        } else if (is_l_kat_pressed(other_player)) {
            review_jump(0);
        } else if (is_l_don_pressed(other_player) || is_r_don_pressed(other_player)) {
            review_jump(1);
        } else if (is_r_kat_pressed(other_player)) {
            review_jump(2);
        }
        return std::nullopt;
    }

    if (!clock.is_paused()) {
        bool other_don = other_don_pressed(global_data.player_num);
        bool other_l_kat = other_drum_pressed(global_data.player_num, PracticeDrumRole::L_KAT);
        bool other_r_kat = other_drum_pressed(global_data.player_num, PracticeDrumRole::R_KAT);
        bool other_kat = other_l_kat || other_r_kat;
        if (other_don || other_kat) {
            pause_practice();
            if (other_don) {
                pause_don_anim->start();
            }
            if (other_kat) {
                pause_kat_anim->start();
            }
            if (practice_player) {
                practice_player->spawn_scrobble_effect(DrumType::DON, Side::LEFT, other_idx);
            }
        }
    } else {
        if (menu.open) {
            if (menu.jumping_marks) {
                // Free-roam jump-point navigation: 1P cycles between marks
                // and confirms with don, returning to paused practice.
                bool step_l = is_l_kat_pressed(global_data.player_num);
                bool step_r = is_r_kat_pressed(global_data.player_num);
                bool p1_don = is_l_don_pressed(global_data.player_num) || is_r_don_pressed(global_data.player_num);

                if (!bars.empty() && (step_l || step_r)) {
                    std::vector<int> marks;
                    for (int b : jump_bars) {
                        if (b >= 0 && b < std::ssize(bars)) {
                            marks.push_back(b);
                        }
                    }
                    std::ranges::sort(marks);

                    if (!marks.empty()) {
                        bool right = !step_l && step_r;
                        int new_index;
                        if (right) {
                            auto it = std::ranges::upper_bound(marks, scrobble_index);
                            new_index = (it != marks.end()) ? *it : marks.front();
                        } else {
                            auto it = std::ranges::lower_bound(marks, scrobble_index);
                            new_index = (it == marks.begin()) ? marks.back() : *(--it);
                        }
                        audio.play_sound("kat", VolumePreset::HITSOUND);
                        if (right) {
                            skip_r_kat_anim->start();
                        } else {
                            skip_l_kat_anim->start();
                        }
                        if (practice_player) {
                            if (right) {
                                practice_player->spawn_scrobble_effect(DrumType::KAT, Side::RIGHT, player_idx);
                            } else {
                                practice_player->spawn_scrobble_effect(DrumType::KAT, Side::LEFT, player_idx);
                            }
                        }
                        animate_scrobble_to(new_index);
                    }
                }
                if (p1_don) {
                    audio.play_sound("don", VolumePreset::HITSOUND);
                    mark_finish_anim->start();
                    menu.close();
                }
                return std::nullopt;
            }

            if (menu.editing_marks) {
                // Jump-point editor: 1P moves the bar cursor and toggles a mark
                // at the current bar, 2P confirm exits.
                bool step_l = is_l_kat_pressed(global_data.player_num);
                bool step_r = is_r_kat_pressed(global_data.player_num);
                bool p1_don = is_l_don_pressed(global_data.player_num) || is_r_don_pressed(global_data.player_num);
                bool p2_don = other_don_pressed(global_data.player_num);

                if (step_l || step_r) {
                    scrobble_step_bar(!step_l && step_r);
                }
                if (p1_don) {
                    audio.play_sound("don", VolumePreset::HITSOUND);
                    mark_action_anim->start();
                    auto marked = std::ranges::find(jump_bars, scrobble_index);
                    if (marked != jump_bars.end()) {
                        *marked = -1; // already marked here: remove it
                    } else {
                        auto free_slot = std::ranges::find(jump_bars, -1);
                        if (free_slot != jump_bars.end()) {
                            *free_slot = scrobble_index; // append
                            jump_arrow_bar = scrobble_index;
                            jump_arrow_anim = std::make_unique<MoveAnimation>(
                                350.0, static_cast<int>(184 * tex.screen_scale), false, false, 0, 0.0, std::nullopt,
                                std::nullopt, EaseType::Quadratic);
                            jump_arrow_anim->start();
                        }
                    }
                }
                if (p2_don) {
                    audio.play_sound("don", VolumePreset::HITSOUND);
                    mark_finish_anim->start();
                    menu.close_mark_edit();
                }
                return std::nullopt;
            }

            // The menu swallows the drums: kat steps, don confirms.
            bool step_l = is_l_kat_pressed(global_data.player_num) || is_l_kat_pressed(other_player);
            bool step_r = is_r_kat_pressed(global_data.player_num) || is_r_kat_pressed(other_player);
            bool don = is_l_don_pressed(global_data.player_num) || is_r_don_pressed(global_data.player_num) ||
                       is_l_don_pressed(other_player) || is_r_don_pressed(other_player);

            if (step_l || step_r) {
                audio.play_sound("kat", VolumePreset::HITSOUND);
                menu.step(step_r);
            }
            if (don) {
                audio.play_sound("don", VolumePreset::HITSOUND);
                bool in_dialog = menu.dialog != PracticeMenu::Dialog::NONE;
                auto action =
                    in_dialog ? menu.confirm() : menu.activate(practice_player && practice_player->is_auto_play());
                return handle_menu_action(action);
            }
            return std::nullopt;
        }

        if (other_don_pressed(global_data.player_num)) {
            audio.play_sound("don", VolumePreset::HITSOUND);
            open_practice_menu();
            return std::nullopt;
        }

        const bool l_don = is_l_don_pressed(global_data.player_num);
        const bool r_don = is_r_don_pressed(global_data.player_num);
        if (zdon_don_gesture(l_don, r_don)) {
            // a don held for the branch gesture's window resumes later, with neither key pressed this frame
            resume_practice((r_don && !l_don) || (!l_don && !r_don && zdon_don_right) ? Side::RIGHT : Side::LEFT);
        }

        bool speed_down = other_drum_pressed(global_data.player_num, PracticeDrumRole::L_KAT);
        bool speed_up = other_drum_pressed(global_data.player_num, PracticeDrumRole::R_KAT);
        if (clock.is_paused() && (speed_down || speed_up)) {
            audio.play_sound("kat", VolumePreset::HITSOUND);
            change_practice_speed(speed_down, speed_up);
        }

        bool scrobble_left = is_l_kat_pressed(global_data.player_num);
        bool scrobble_right = is_r_kat_pressed(global_data.player_num);

        if (!bars.empty() && (scrobble_left || scrobble_right)) {
            scrobble_step_bar(!scrobble_left && scrobble_right); // one bar a kat, however quick
        }
    }

    return std::nullopt;
}

std::optional<Screens> PracticeGameScreen::update() {
    if (auto init = Screen::update()) {
        return init;
    }

    double current_ms = get_frame_ms();
    now_ms = current_ms;
    transition->update(current_ms);
    clock.update(current_ms);

    if (song_started && song_music && audio.is_sound_playing(*song_music)) {
        double audio_ms = audio.get_sound_time_played(*song_music) * 1000.0;
        clock.follow_audio(current_ms, audio_ms + audio_start_ms());
    }

    poll_pending_song();
    if (transition->is_finished()) {
        start_song(clock.position());
        reset_input_lock();
    }

    update_background(current_ms);

    // Process practice input before player update so events aren't consumed by handle_input
    auto next_screen = global_keys_practice();
    if (next_screen.has_value()) {
        return next_screen;
    }

    // Handle mouse/touch input for practice mode UI buttons
    next_screen = handle_mouse_input();
    if (next_screen.has_value()) {
        return next_screen;
    }
    if (!clock.is_paused()) {
        zdon_lane3d::update_mouse(); // Z-DON 3D view: turned while playing (paused, the mouse is the panel's)
    }

    for (auto& player : players) {
        player->update(clock.position(), current_ms, background);
    }
    zdon_loop_update();
    sync_branch_display();
    zdon_update_fade(current_ms);
    if (global_data.config->general.zdon_practice_metronome && !clock.is_paused() && !menu.open &&
        phase == Phase::PRACTICE && !bars.empty()) {
        zdon_metronome_update();
    } else {
        metronome_primed = false; // paused or off: no tick for the time it skipped
    }
    if (practice_player) {
        const auto& loop = global_data.zdon_loop;
        if (loop.active && loop.a_bar >= 0 && loop.a_bar < static_cast<int>(bars.size()) &&
            loop.b_bar < static_cast<int>(bars.size())) {
            practice_player->zdon_dim_range =
                std::make_pair(bars[loop.a_bar].hit_ms,
                               loop.to_end ? std::numeric_limits<double>::infinity() : bars[loop.b_bar].hit_ms);
        } else {
            practice_player->zdon_dim_range.reset();
        }
    }

    if (!clock.is_paused() && !players.empty() && players[0]->end_time > 0) {
        max_progress = std::max(max_progress, std::min(clock.position() / players[0]->end_time, 1.0));
    }
    if (phase == Phase::LAST_PLAY && !players.empty() && clock.position() >= players[0]->end_time + 1000) {
        start_review();
    }

    scrobble_move->update(current_ms);
    if (scrobble_move->is_started && scrobble_move->is_finished) {
        if (scrobble_index >= 0 && scrobble_index < std::ssize(bars)) {
            scrobble_time = bars[scrobble_index].hit_ms;
        }
        scrobble_move = std::make_unique<MoveAnimation>(200.0, 0);
    }

    for (auto* anim : {pause_don_anim, pause_kat_anim, resume_don_anim, skip_l_kat_anim, skip_r_kat_anim, menu_don_anim,
                       speed_l_kat_anim, speed_r_kat_anim, mark_action_anim, mark_finish_anim}) {
        if (anim) {
            anim->update(current_ms);
        }
    }
    if (jump_arrow_anim) {
        jump_arrow_anim->update(current_ms);
    }

    if (auto* session = PracticeTimer::active()) {
        bool running = false;
        if (phase == Phase::PRACTICE) {
            running = !clock.is_paused() || session->is_up();
        } else if (phase == Phase::REVIEW) {
            running = !review_panel_open() && !menu.open;
        }
        session->update(current_ms, running);

        if (phase == Phase::PRACTICE && session->is_finished()) {
            start_last_play();
        } else if (phase == Phase::REVIEW && session->is_up()) {
            return on_screen_end(Screens::ENTRY);
        }
    }

    return std::nullopt;
}

float PracticeGameScreen::get_scrobble_position_x(const Note& note, double current_ms) const {
    float speedx = note.bpm / 240000.0f * note.scroll_x * scroll_distance();
    float offset_px = 0.0f;
    if (!bars.empty() && scrobble_index < std::ssize(bars)) {
        float bar_speedx = bars[scrobble_index].bpm / 240000.0f * bars[scrobble_index].scroll_x * scroll_distance();
        offset_px = scrobble_move->attribute * bar_speedx;
    }
    return JudgePos::X + static_cast<float>((note.hit_ms - current_ms) * speedx) - offset_px;
}

Color PracticeGameScreen::moji_judgment_color(int note_index) const {
    if (!practice_player) {
        return Color::WHITE;
    }
    auto judgment = practice_player->get_note_judgment(note_index);
    if (!judgment.has_value()) {
        return Color::WHITE;
    }
    switch (judgment.value()) {
    case Judgments::GOOD:
        return Color::YELLOW;
    case Judgments::OK:
        return Color::WHITE;
    case Judgments::BAD:
        return Color::SKYBLUE;
    }
    return Color::WHITE;
}

void PracticeGameScreen::draw_bar_scrobble(const Note& bar, double current_ms) const {
    if (!bar.display) {
        return;
    }
    float x = get_scrobble_position_x(bar, current_ms);
    float y_off =
        static_cast<float>((bar.hit_ms - current_ms) * (bar.bpm / 240000.0f * bar.scroll_y * scroll_distance()));
    float angle = (y_off != 0) ? std::atan2(bar.scroll_y, bar.scroll_x) * 180.0f / std::numbers::pi_v<float> : 0.0f;
    tex.draw_texture(t_notes_0, {.frame = bar.is_branch_start,
                                 .x = x + tex.get_skin_config(SC::GAME_MOJI_DRUMROLL)->x - t_notes_9->width / 2.0f,
                                 .y = y_off + tex.get_skin_config(SC::GAME_MOJI_DRUMROLL)->y + (184 * tex.screen_scale),
                                 .rotation = angle});
}

void PracticeGameScreen::draw_drumroll_scrobble(const Note& head, double current_ms) const {
    float start_pos = get_scrobble_position_x(head, current_ms);
    const Note* tail = find_tail(head);
    if (!tail) {
        return;
    }
    float end_pos = get_scrobble_position_x(*tail, current_ms);
    float length = end_pos - start_pos;
    bool is_big = (head.type == NoteType::ROLL_HEAD_L);
    int color_val = head.color.value_or(255);
    const auto color_channel = static_cast<unsigned char>(color_val);
    Color color = {255, color_channel, color_channel, 255};
    float y_pos = tex.get_skin_config(SC::GAME_NOTES)->y + (184 * tex.screen_scale);
    float moji_y = tex.get_skin_config(SC::GAME_MOJI)->y + (184 * tex.screen_scale);
    if (head.display) {
        tex.draw_texture(t_notes_8, {.color = color,
                                     .frame = is_big,
                                     .x = start_pos,
                                     .y = y_pos,
                                     .x2 = length + tex.get_skin_config(SC::GAME_DRUMROLL_WIDTH_OFFSET)->width});
        if (is_big) {
            tex.draw_texture(t_drumroll_big_tail, {.color = color, .x = end_pos, .y = y_pos});
        } else {
            tex.draw_texture(t_drumroll_tail, {.color = color, .x = end_pos, .y = y_pos});
        }
        TextureObject* head_tex = t_notes[static_cast<int>(head.type)];
        tex.draw_texture(head_tex, {.color = color, .x = start_pos - t_notes_9->width / 2.0f, .y = y_pos});
    }
    tex.draw_texture(t_moji_drumroll_mid, {.x = start_pos, .y = moji_y, .x2 = length});
    tex.draw_texture(t_moji, {.frame = head.moji, .x = start_pos - t_moji->width / 2.0f, .y = moji_y});
    tex.draw_texture(t_moji, {.frame = tail->moji, .x = end_pos - t_moji->width / 2.0f, .y = moji_y});
}

void PracticeGameScreen::draw_balloon_scrobble(const Note& head, double current_ms) const {
    float offset = tex.get_skin_config(SC::GAME_BALLOON_OFFSET)->x;
    float start_pos = get_scrobble_position_x(head, current_ms);
    const Note* tail = find_tail(head);
    if (!tail) {
        return;
    }
    float end_pos = get_scrobble_position_x(*tail, current_ms);
    float pause_pos = JudgePos::X;
    float y_pos = tex.get_skin_config(SC::GAME_NOTES)->y + (184 * tex.screen_scale);
    float position;
    if (current_ms >= tail->hit_ms) {
        position = end_pos;
    } else if (current_ms >= head.hit_ms) {
        position = pause_pos;
    } else {
        position = start_pos;
    }
    if (head.display) {
        TextureObject* head_tex = t_notes[static_cast<int>(head.type)];
        tex.draw_texture(head_tex, {.x = position - offset - t_notes_9->width / 2.0f, .y = y_pos});
        tex.draw_texture(t_notes_10,
                         {.x = position - offset + t_notes_10->width - t_notes_9->width / 2.0f, .y = y_pos});
    }
    float moji_y = tex.get_skin_config(SC::GAME_MOJI)->y + (184 * tex.screen_scale);
    tex.draw_texture(t_moji, {.frame = head.moji, .x = position - t_moji->width / 2.0f, .y = moji_y});
}

void PracticeGameScreen::draw_notes_scrobble(double current_ms) const {
    int scissor_x = players[0]->get_scissor_x();
    int win_w = get_render_width();
    begin_scissor_mode(scissor_x, 0, win_w - scissor_x, get_render_height());

    // Draw bars
    for (const auto& bar : std::views::reverse(bars)) {
        draw_bar_scrobble(bar, current_ms);
    }
    // Draw notes in reverse order
    float y = 184 * tex.screen_scale;
    for (const auto& note : std::views::reverse(scrobble_note_list)) {
        if (note.type == NoteType::TAIL || note.type == NoteType::BARLINE) {
            continue;
        }

        if (note.color.has_value()) {
            draw_drumroll_scrobble(note, current_ms);
        } else if (note.type == NoteType::BALLOON_HEAD || note.type == NoteType::KUSUDAMA) {
            draw_balloon_scrobble(note, current_ms);
        } else {
            if (note.display) {
                float x = get_scrobble_position_x(note, current_ms);
                TextureObject* note_tex = t_notes[static_cast<int>(note.type)];
                tex.draw_texture(note_tex, {.center = true,
                                            .x = x - t_notes_9->width / 2.0f,
                                            .y = tex.get_skin_config(SC::GAME_NOTES)->y + y});
            }
            float moji_x = get_scrobble_position_x(note, current_ms) - t_moji->width / 2.0f;
            tex.draw_texture(t_moji, {.color = moji_judgment_color(note.index),
                                      .frame = note.moji,
                                      .x = moji_x,
                                      .y = tex.get_skin_config(SC::GAME_MOJI)->y + y});
            zdon_draw_offset(note, get_scrobble_position_x(note, current_ms));
        }
    }

    if (practice_player) {
        float note_cy = tex.get_skin_config(SC::GAME_NOTES)->y + y + t_notes_9->height / 2.0f;
        for (const auto& note : scrobble_note_list) {
            bool roll = note.type == NoteType::ROLL_HEAD || note.type == NoteType::ROLL_HEAD_L;
            bool balloon = note.type == NoteType::BALLOON_HEAD;
            if (!roll && !balloon) {
                continue;
            }
            auto hits = practice_player->get_roll_hits(note.index);
            if (!hits) {
                continue;
            }
            float x = get_scrobble_position_x(note, current_ms);
            if (balloon) { // follows the balloon: parked at the judge circle while it lasts
                const Note* tail = find_tail(note);
                if (tail && current_ms >= tail->hit_ms) {
                    x = get_scrobble_position_x(*tail, current_ms);
                } else if (current_ms >= note.hit_ms) {
                    x = JudgePos::X;
                }
                x -= tex.get_skin_config(SC::GAME_BALLOON_OFFSET)->x;
            }
            draw_renda_marker(x, note_cy, *hits, balloon ? note.count.value_or(0) : 0);
        }
    }

    end_scissor_mode();

    if (!bars.empty() && scrobble_index < std::ssize(bars)) {
        std::string bar_str = std::to_string(scrobble_index + 1);
        float dw = t_bar_count_bar->x2[0];
        float x = get_scrobble_position_x(bars[scrobble_index], current_ms) + 4 * tex.screen_scale;
        float y = tex.get_skin_config(SC::GAME_NOTES)->y + 184 * tex.screen_scale + t_bar_count_bar->y2[0] +
                  2 * tex.screen_scale;
        for (int i = 0; i < std::ssize(bar_str); i++) {
            tex.draw_texture(t_bar_count_bar, {.frame = bar_str[i] - '0', .x = x + i * dw, .y = y});
        }
    }
}

void PracticeGameScreen::draw() {
    clear_background(Color::BLACK);
    // Z-DON 3D view: while playing, the background and the lane in layers (zdon_lane3d.h); paused, flat (its
    // buttons take clicks). The drums and the practice panel below stay flat either way.
    zdon_set_lane3d_pivot();
    if (!clock.is_paused() && zdon_lane3d::begin_frame()) {
        using zdon_lane3d::Layer;
        zdon_lane3d::begin_layer(Layer::BACKGROUND);
        if (movie) {
            movie->draw();
        } else if (!has_movie() && background.has_value()) {
            background->draw_back();
        }
        zdon_lane3d::end_layer();
        for (const Layer layer :
             {Layer::LANE, Layer::TARGET, Layer::NOTES, Layer::JUDGMENTS, Layer::HUD, Layer::EFFECTS}) {
            zdon_lane3d::begin_layer(layer);
            players[0]->draw_practice(clock.position(), 0, 184 * tex.screen_scale, true);
            zdon_lane3d::end_layer();
        }
        if (background.has_value()) {
            zdon_lane3d::begin_layer(Layer::FOREGROUND);
            background->draw_fore();
            zdon_lane3d::end_layer();
        }
        zdon_lane3d::end_frame();
        if (players[0]->judge_counter.has_value()) {
            players[0]->judge_counter->draw(); // flat, by the practice panel (Player::draw_overlays skips it)
        }
    } else {
        if (movie) {
            movie->draw();
        } else if (!has_movie() && background.has_value()) {
            background->draw_back();
        }

        players[0]->draw_practice(clock.position(), 0, 184 * tex.screen_scale, !clock.is_paused());

        if (background.has_value()) {
            background->draw_fore();
        }
    }

    // Scrobble note overlay when paused
    if (clock.is_paused()) {
        draw_notes_scrobble(scrobble_time);
        if (menu.editing_marks) {
            tex.draw_texture(t_jump_point_editing, {});
        }
        if (jump_arrow_anim && jump_arrow_anim->is_started && jump_arrow_bar >= 0 &&
            jump_arrow_bar < std::ssize(bars)) {
            float ax = get_scrobble_position_x(bars[jump_arrow_bar], scrobble_time) - t_jump_point_arrow->width / 2.0f;
            float ay = static_cast<float>(jump_arrow_anim->attribute) - t_jump_point_arrow->height;
            tex.draw_texture(t_jump_point_arrow, {.x = ax, .y = ay});
        }
    }

    // Player::draw_practice already drew the overlays (drum, hit effects, combo) once, before
    // the background's draw_fore, as Player::draw does in normal play.

    tex.draw_texture(t_large_drum, {.index = 0});
    tex.draw_texture(t_large_drum, {.index = 1});
    if (practice_player) {
        practice_player->draw_large_drum_hits(); // Z-DON: the hit colours over the drums, not under them
    }

    int other_idx = (global_data.player_num == PlayerNum::P1) ? 1 : 0;
    int player_idx = (global_data.player_num == PlayerNum::P1) ? 0 : 1;
    auto anim_scale = [](TextureResizeAnimation* anim) { return anim ? static_cast<float>(anim->attribute) : 1.0f; };
    if (!clock.is_paused()) {
        if (phase == Phase::PRACTICE) {
            tex.draw_texture(t_pause_don, {.scale = anim_scale(pause_don_anim), .center = true, .index = other_idx});
            tex.draw_texture(t_pause_kat,
                             {.scale = anim_scale(pause_kat_anim), .center = true, .index = other_idx * 2});
            tex.draw_texture(t_pause_kat,
                             {.scale = anim_scale(pause_kat_anim), .center = true, .index = other_idx * 2 + 1});
        }
    } else if (phase == Phase::REVIEW) {
        draw_skip_buttons(player_idx, true);
        tex.draw_texture(t_finish, {.scale = anim_scale(mark_finish_anim), .center = true, .index = player_idx});
        tex.draw_texture(t_jump_buttons[0],
                         {.scale = anim_scale(speed_l_kat_anim), .center = true, .index = other_idx * 2});
        tex.draw_texture(t_jump_buttons[1], {.scale = anim_scale(menu_don_anim), .center = true, .index = other_idx});
        tex.draw_texture(t_jump_buttons[2],
                         {.scale = anim_scale(speed_r_kat_anim), .center = true, .index = other_idx * 2 + 1});
    } else if (menu.editing_marks || menu.jumping_marks) {

        draw_skip_buttons(player_idx, menu.editing_marks);
        if (menu.editing_marks) {
            tex.draw_texture(t_finish, {.scale = anim_scale(mark_finish_anim), .center = true, .index = other_idx});
            bool marked = std::ranges::contains(jump_bars, scrobble_index);
            tex.draw_texture(marked ? t_delete : t_confirm,
                             {.scale = anim_scale(mark_action_anim), .center = true, .index = player_idx});

            auto left = static_cast<int>(std::ranges::count(jump_bars, -1));
            tex.draw_texture(t_jump_count, {.index = player_idx});
            tex.draw_texture(t_jump_count_digits, {.frame = left, .index = player_idx});
        } else {
            tex.draw_texture(t_finish, {.scale = anim_scale(mark_finish_anim), .center = true, .index = player_idx});
        }
    } else if (menu.open) {
        draw_menu_arrows();
    } else {
        tex.draw_texture(t_resume_don, {.scale = anim_scale(resume_don_anim), .center = true, .index = player_idx});
        draw_skip_buttons(player_idx, true);
        tex.draw_texture(t_menu_don, {.scale = anim_scale(menu_don_anim), .center = true, .index = other_idx});
        tex.draw_texture(t_speed_r_kat,
                         {.scale = anim_scale(speed_r_kat_anim), .center = true, .index = other_idx * 2});
        tex.draw_texture(t_speed_l_kat,
                         {.scale = anim_scale(speed_l_kat_anim), .center = true, .index = other_idx * 2 + 1});
    }
    draw_tempo_feedback(other_idx, clock.is_paused() && phase == Phase::PRACTICE && !menu.open);

    if (!clock.is_paused()) {
        tex.draw_texture(t_playing, {.fade = 0.5, .index = static_cast<int>(global_data.player_num) - 1});
    }
    // lyric stays at its normal position but on top of the large drums
    if (!players.empty()) {
        players[0]->draw_lyric(184 * tex.screen_scale);
    }

    // Progress bar
    tex.draw_texture(t_progress_bar_bg, {});
    float bar_width = tex.get_skin_config(SC::GAME_PRACTICE_PROGRESS_BAR_WIDTH)->width;
    if (max_progress > 0) {
        tex.draw_texture(t_gauge_max, {.x2 = static_cast<float>(max_progress) * bar_width});
    }
    float progress;
    if (clock.is_paused() && !bars.empty()) {
        double raw = scrobble_time + scrobble_move->attribute - bars[0].hit_ms;
        progress = (players.empty() || players[0]->end_time <= 0) ? 0.0f : std::min(raw / players[0]->end_time, 1.0);
    } else {
        progress = players.empty() ? 0.0f : std::min(clock.position() / players[0]->end_time, 1.0);
    }
    tex.draw_texture(t_progress_bar, {.x2 = progress * bar_width});
    if (phase == Phase::REVIEW) {
        for (TextureObject* card : t_review_cards) {
            tex.draw_texture(card, {});
        }
    }

    if (!bars.empty()) {
        double first_bar_time = bars[0].hit_ms;
        double total_time = players.empty() ? 1.0 : players[0]->end_time;
        for (double marker : markers) {
            float mx = static_cast<float>((marker - first_bar_time) / total_time) * bar_width;
            tex.draw_texture(t_gogo_marker, {.x = mx});
        }
        for (int b : jump_bars) {
            if (b < 0 || b >= std::ssize(bars)) {
                continue;
            }
            float jx = static_cast<float>((bars[b].hit_ms - first_bar_time) / total_time) * bar_width;
            tex.draw_texture(t_jump_point_progress, {.x = jx});
        }
    }

    if (!bars.empty()) {
        int current_bar;
        if (clock.is_paused()) {
            current_bar = scrobble_index + 1;
        } else {
            current_bar = 0;
            double first_bar_time = bars[0].hit_ms;
            for (int i = 0; i < std::ssize(bars); i++) {
                if (bars[i].hit_ms - first_bar_time <= clock.position()) {
                    current_bar = i + 1;
                } else {
                    break;
                }
            }
        }

        float dw = t_bar_count->x2[0];
        float divw = t_bar_divider->x2[0];
        float dh = t_bar_count->y2[0];
        float pb_y = t_progress_bar_bg->y[0];
        float pb_h = t_progress_bar_bg->y2[0];
        float digit_y = pb_y + (pb_h - dh) * 0.5f;

        std::string cur_str = std::to_string(current_bar);
        std::string tot_str = std::to_string(bars.size());
        int d1 = song_speed / 10;
        int d2 = song_speed % 10;

        tex.draw_texture(t_song_tempo, {});
        tex.draw_texture(t_dot, {});
        tex.draw_texture(t_multiplier, {});
        tex.draw_texture(t_bar_label, {});

        float st_right = t_song_tempo->x[0] + t_song_tempo->x2[0];
        float dot_right = t_dot->x[0] + t_dot->x2[0];
        float lbl_right = t_bar_label->x[0] + t_bar_label->x2[0];

        tex.draw_texture(t_bar_count, {.frame = d1, .x = st_right - 4 * tex.screen_scale, .y = digit_y});
        tex.draw_texture(t_bar_count, {.frame = d2, .x = dot_right - 4 * tex.screen_scale, .y = digit_y});

        for (int i = 0; i < std::ssize(cur_str); i++) {
            tex.draw_texture(t_bar_count, {.frame = cur_str[i] - '0', .x = lbl_right + i * dw, .y = digit_y});
        }
        float div_x = lbl_right + cur_str.size() * dw;
        tex.draw_texture(t_bar_divider, {.x = div_x, .y = digit_y});
        for (int i = 0; i < std::ssize(tot_str); i++) {
            tex.draw_texture(t_bar_count, {.frame = tot_str[i] - '0', .x = div_x + divw + i * dw, .y = digit_y});
        }
    }

    if (clock.is_paused()) {
        tex.draw_texture(t_paused, {.fade = 0.5});
        if (menu.open && !menu.editing_marks && !menu.jumping_marks) {
            if (menu.dialog != PracticeMenu::Dialog::NONE) {
                menu.draw_dialog();
            } else {
                menu.draw();
            }
        }
    }

    if (double t = now_ms - seek_limit_ms; t < 1133) {
        double fade = t < 900 ? 1.0 : 1.0 - std::pow((t - 900) / 233.0, 2);
        tex.draw_texture(t_seek_limit, {.fade = fade});
    }
    if (review_panel_open()) {
        draw_review_panel();
    }

    if (auto* session = PracticeTimer::active()) {
        session->draw();
    }

    zdon_draw_info();
    if (zdon_bar_input_open) {
        zdon_draw_bar_input();
    }
    draw_overlay(false);
}

// ---------------------------------------------------------------- Z-DON practice extras

bool PracticeGameScreen::zdon_has_branches() const {
    return !branch_m_all.empty() || !branch_e_all.empty() || !branch_n_all.empty();
}

// the forced branch for every checkpoint (or none: the play decides again from the next resume on)
void PracticeGameScreen::zdon_apply_branch_force() {
    if (!practice_player) {
        return;
    }
    const size_t checkpoints = std::max({branch_m_all.size(), branch_e_all.size(), branch_n_all.size()});
    if (zdon_branch_force < 0) {
        practice_player->branch_history.clear();
    } else {
        practice_player->branch_history.assign(checkpoints, static_cast<BranchDifficulty>(zdon_branch_force));
    }
    branch_display_synced = static_cast<size_t>(-1);
    sync_branch_display();
}

static int zdon_digit_pressed() {
    static constexpr SDL_Scancode row[10] = {SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3,
                                             SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
                                             SDL_SCANCODE_8, SDL_SCANCODE_9};
    static constexpr SDL_Scancode pad[10] = {SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
                                             SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
                                             SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9};
    for (int d = 0; d < 10; ++d) {
        if (key_pressed(row[d]) || key_pressed(pad[d])) {
            return d;
        }
    }
    return -1;
}

static const char* zdon_branch_key(int force) {
    static constexpr const char* names[] = {"branch_normal", "branch_expert", "branch_master"};
    return force < 0 ? "branch_auto" : names[std::clamp(force, 0, 2)];
}

bool PracticeGameScreen::zdon_practice_keys() {
    if (!clock.is_paused() || menu.open) {
        return false;
    }
    if (zdon_bar_input_open) {
        // the typed bar number: Enter jumps, Backspace erases, Esc closes; the drums wait meanwhile
        const int d = zdon_digit_pressed();
        if (d >= 0 && zdon_bar_input.size() < 4) {
            zdon_bar_input += static_cast<char>('0' + d);
            audio.play_sound("kat", VolumePreset::HITSOUND);
        }
        if (key_pressed(SDL_SCANCODE_BACKSPACE) && !zdon_bar_input.empty()) {
            zdon_bar_input.pop_back();
        }
        if (key_pressed(SDL_SCANCODE_ESCAPE)) {
            zdon_bar_input_open = false;
        }
        if (key_pressed(SDL_SCANCODE_RETURN) || key_pressed(SDL_SCANCODE_KP_ENTER)) {
            const int n = zdon_bar_input.empty() ? 0 : std::stoi(zdon_bar_input);
            if (n >= 1 && n <= static_cast<int>(bars.size())) {
                audio.play_sound("don", VolumePreset::HITSOUND);
                animate_scrobble_to(n - 1);
                zdon_bar_input_open = false;
            } else {
                audio.play_sound("kat", VolumePreset::HITSOUND);
                zhud::toast(zhud::text("bar_missing", zdon_bar_input));
                zdon_bar_input.clear();
            }
        }
        clear_input_buffers(); // nothing typed here reaches the drums
        return true;
    }
    if (const int d = zdon_digit_pressed(); d >= 0 && !bars.empty()) {
        zdon_bar_input_open = true;
        zdon_bar_input = d > 0 ? std::string(1, static_cast<char>('0' + d)) : std::string();
        audio.play_sound("kat", VolumePreset::HITSOUND);
        return true;
    }
    // F6: the branch for the whole song: decided by the play -> normal -> advanced -> master
    if (key_pressed(SDL_SCANCODE_F6) && zdon_has_branches()) {
        zdon_cycle_branch();
        return true;
    }
    // M: to the bar of the latest miss in this pass (a passed, unhit note is judged BAD too)
    if (key_pressed(SDL_SCANCODE_M) && practice_player && !bars.empty()) {
        const Note* last = nullptr;
        for (const Note& note : scrobble_note_list) {
            const auto j = practice_player->get_note_judgment(note.index);
            if (j && *j == Judgments::BAD && (!last || note.hit_ms > last->hit_ms)) {
                last = &note;
            }
        }
        if (last) {
            auto it = std::ranges::upper_bound(bars, last->hit_ms, {}, &Note::hit_ms);
            const int bar = std::max(0, static_cast<int>(std::distance(bars.begin(), it)) - 1);
            audio.play_sound("don", VolumePreset::HITSOUND);
            animate_scrobble_to(bar);
            zhud::toast(zhud::text("miss_bar", std::to_string(bar + 1)));
        } else {
            zhud::toast(zhud::text("no_miss"));
        }
        return true;
    }
    return false;
}

void PracticeGameScreen::zdon_cycle_branch() {
    zdon_branch_force = zdon_branch_force >= static_cast<int>(BranchDifficulty::MASTER) ? -1 : zdon_branch_force + 1;
    zdon_apply_branch_force();
    audio.play_sound("don", VolumePreset::HITSOUND);
    zhud::toast(zhud::text("branch", zhud::text(zdon_branch_key(zdon_branch_force))));
}

// a paused 1P don: on a chart with branches, both dons within 80 ms switch the branch and one don resumes
// once the 80 ms are over (true = resume now)
bool PracticeGameScreen::zdon_don_gesture(bool l, bool r) {
    if (!zdon_has_branches()) {
        return l || r;
    }
    constexpr double WINDOW_MS = 80.0;
    const double now = get_current_ms();
    if (zdon_don_wait_ms >= 0) {
        if (zdon_don_right ? l : r) {
            zdon_don_wait_ms = -1.0;
            zdon_cycle_branch();
            return false;
        }
        if (now - zdon_don_wait_ms >= WINDOW_MS) {
            zdon_don_wait_ms = -1.0;
            return true;
        }
        return false;
    }
    if (l && r) {
        zdon_cycle_branch();
        return false;
    }
    if (l || r) {
        zdon_don_wait_ms = now;
        zdon_don_right = r;
    }
    return false;
}

void PracticeGameScreen::zdon_update_fade(double current_ms) {
    if (zdon_fade_start < 0 || !song_music) {
        return;
    }
    static constexpr double FADE_MS = 900.0;
    const double t = std::clamp((current_ms - zdon_fade_start) / FADE_MS, 0.0, 1.0);
    audio.set_sound_volume(*song_music, audio.preset_level(VolumePreset::MUSIC) * static_cast<float>(t));
    if (t >= 1.0) {
        zdon_fade_start = -1.0;
    }
}

void PracticeGameScreen::zdon_metronome_update() {
    // chart time, as the info strip's; the tempo map counts beats in it (a #DELAY holds the beat)
    const double now = clock.position() + bars[0].hit_ms - start_delay;
    const double beat = std::floor(base_chart.tempo_map.beat_at(now));
    const bool jumped = !metronome_primed || std::abs(now - metronome_last_ms) > 250.0; // a seek or the A-B loop
    if (!jumped && beat > metronome_last_beat) {
        audio.play_sound("kat", VolumePreset::SOUND, 0.35f);
    }
    metronome_last_beat = beat;
    metronome_last_ms = now;
    metronome_primed = true;
}

double PracticeGameScreen::zdon_bpm_at(double hit_ms) const {
    if (zdon_tempo.empty()) {
        return players.empty() ? 0.0 : players[0]->bpm;
    }
    auto it = std::ranges::upper_bound(zdon_tempo, hit_ms + 0.5, {}, &std::pair<double, double>::first);
    return it == zdon_tempo.begin() ? zdon_tempo.front().second : std::prev(it)->second;
}

static std::string zdon_fmt_tempo(double v) {
    const long long tenths = std::llround(v * 10.0);
    std::string t = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        t += "." + std::to_string(std::llabs(tenths % 10));
    }
    return t;
}

// "BPM 158  x0.8 -> 126", the branch (charts with branches), and the keys while paused: bottom left, or
// with its top left at a skin's  "zdon_practice_info": { "x": .., "y": .. }
void PracticeGameScreen::zdon_draw_info() const {
    if (bars.empty()) {
        return;
    }
    const float s = tex.screen_scale;
    const double at = clock.is_paused() ? bars[std::clamp(scrobble_index, 0, static_cast<int>(bars.size()) - 1)].hit_ms
                                        : clock.position() + bars[0].hit_ms - start_delay;
    const double bpm = zdon_bpm_at(at);
    std::string tempo = zhud::text("bpm", zdon_fmt_tempo(bpm));
    if (song_speed != 10) {
        tempo += "  x" + zdon_fmt_tempo(song_speed / 10.0) + " → " + zdon_fmt_tempo(bpm * song_speed / 10.0);
    }
    std::vector<OutlinedText*> lines = {&zhud::label(tempo, 22)};
    if (clock.is_paused()) {
        lines.push_back(&zhud::label(
            zhud::text("bar_pos", std::to_string(std::clamp(scrobble_index, 0, static_cast<int>(bars.size()) - 1) + 1),
                       std::to_string(bars.size())),
            22));
    }
    if (zdon_has_branches()) {
        lines.push_back(&zhud::label(zhud::text("branch", zhud::text(zdon_branch_key(zdon_branch_force))), 22,
                                     zdon_branch_force < 0 ? Color::WHITE : Color{255, 226, 120, 255}));
    }
    // the A-B loop's passes, newest first, and the best (while paused: the notes stay clear in play)
    if (const auto& loop = global_data.zdon_loop; clock.is_paused() && !menu.open && loop.history_count > 0) {
        using Loop = std::remove_cvref_t<decltype(loop)>;
        for (int i = 0; i < loop.history_count; i++) {
            const auto& line =
                loop.history[static_cast<size_t>((loop.history_head - 1 - i + Loop::HISTORY) % Loop::HISTORY)];
            lines.push_back(&zhud::label(line, 18, Color{200, 206, 220, 255}));
        }
        if (!loop.best_line.empty()) {
            lines.push_back(&zhud::label(loop.best_line, 18, Color{255, 226, 120, 255}));
        }
    }
    if (clock.is_paused() && !menu.open) {
        lines.push_back(&zhud::label(zhud::text("practice_keys"), 18, Color{200, 206, 220, 255}));
    }
    float w = 0;
    float h = 8 * s;
    for (auto* t : lines) {
        w = std::max(w, t->width);
        h += t->height + 2 * s;
    }
    w += 24 * s;
    float x = 10 * s;
    float y = tex.screen_height - h - 10 * s;
    if (auto it = tex.skin_config_by_name.find("zdon_practice_info"); it != tex.skin_config_by_name.end()) {
        x = it->second.x;
        y = it->second.y;
    }
    zhud::panel(x, y, w, h, 0.72f);
    y += 4 * s;
    for (auto* t : lines) {
        t->draw({.x = x + 12 * s, .y = y});
        y += t->height + 2 * s;
    }
}

void PracticeGameScreen::zdon_draw_bar_input() const {
    const float s = tex.screen_scale;
    const bool blink = static_cast<long long>(get_current_ms() / 400) % 2 == 0;
    OutlinedText& t = zhud::label(zhud::text("bar_input", zdon_bar_input + (blink ? "_" : " ")), 44);
    OutlinedText& hint =
        zhud::label(zhud::text("bar_input_hint", std::to_string(bars.size())), 20, Color{200, 206, 220, 255});
    const float w = std::max({t.width, hint.width, 360 * s}) + 48 * s;
    const float h = t.height + hint.height + 30 * s;
    const float x = (tex.screen_width - w) / 2.0f;
    const float y = tex.screen_height * 0.45f;
    zhud::panel(x, y, w, h, 0.9f, Color{255, 226, 120, 220});
    t.draw({.x = x + (w - t.width) / 2.0f, .y = y + 10 * s});
    hint.draw({.x = x + (w - hint.width) / 2.0f, .y = y + 18 * s + t.height});
}

// the hit's offset over a note in the pause view: blue = early (-), orange = late (+); two rows so close notes
// do not cover each other
void PracticeGameScreen::zdon_draw_offset(const Note& note, float x) const {
    if (!practice_player) {
        return;
    }
    const std::optional<float> off = practice_player->get_note_offset(note.index);
    if (!off) {
        return;
    }
    const int ms = static_cast<int>(std::lround(*off));
    const Color c = ms <= -1 ? Color{120, 196, 255, 255} : ms >= 1 ? Color{255, 150, 90, 255} : Color::WHITE;
    OutlinedText& t = zhud::label((ms > 0 ? "+" : "") + std::to_string(ms), 18, c);
    const float s = tex.screen_scale;
    const float top = tex.get_skin_config(SC::GAME_NOTES)->y + 184 * s;
    const float row = (note.index % 2) ? t.height : 0.0f;
    t.draw({.x = x - t.width / 2.0f, .y = top - t.height - row + 4 * s});
}

static float ease_out(double p) {
    p = std::clamp(p, 0.0, 1.0);
    return static_cast<float>(1.0 - (1.0 - p) * (1.0 - p));
}

void PracticeGameScreen::draw_skip_buttons(int player_idx, bool clamp) const {
    bool l_off = false;
    bool r_off = clamp && at_last_bar();
    tex.draw_texture(l_off ? t_skip_l_kat_off : t_skip_l_kat,
                     {.scale = skip_l_kat_anim ? static_cast<float>(skip_l_kat_anim->attribute) : 1.0f,
                      .center = true,
                      .index = player_idx * 2});
    tex.draw_texture(r_off ? t_skip_r_kat_off : t_skip_r_kat,
                     {.scale = skip_r_kat_anim ? static_cast<float>(skip_r_kat_anim->attribute) : 1.0f,
                      .center = true,
                      .index = player_idx * 2 + 1});
}

void PracticeGameScreen::draw_tempo_feedback(int other_idx, bool dots) const {
    double t = now_ms - tempo_change_ms;
    int level = 10 - song_speed;
    bool has_level = level >= 0;
    if (dots) {
        for (int i = 0; i < 6; i++) {
            tex.draw_texture(t_tempo_dot, {.index = other_idx * 6 + i});
        }
        if (has_level && t < 450) {
            double fade = t < 50 ? 1.0 : 0.93 * (1.0 - ease_out((t - 50) / 400.0));
            tex.draw_texture(t_tempo_dot_on, {.fade = fade, .index = other_idx * 6 + level});
        }
    }
    if (!has_level || t >= 750) {
        return;
    }
    double scale, fade = 1.0;
    if (t < 33) {
        scale = 0.294 + 0.53 * t / 33.0;
        fade = 0.75 * t / 33.0;
    } else if (t < 50) {
        scale = 0.956 + 0.044 * (t - 33) / 17.0;
    } else if (t < 317) {
        double p = (t - 50) / 267.0;
        scale = 1.0 - 0.076 * p * p;
    } else if (t < 433) {
        scale = 0.915 - 0.078 * (t - 317) / 116.0;
    } else {
        double p = (t - 433) / 317.0;
        scale = 0.83 - 0.124 * p;
        fade = 0.949 * (1.0 - p);
    }
    tex.draw_texture(
        t_tempo_rate,
        {.frame = level, .scale = static_cast<float>(scale), .center = true, .fade = fade, .index = other_idx});
}

void PracticeGameScreen::draw_menu_arrows() const {
    double p = std::fmod(now_ms, 1000.0);
    float offset = 20.0f;
    double fade = 0.0;
    if (p < 467) {
        offset = 14.85f * ease_out(p / 467.0);
        fade = 1.0;
    } else if (p < 933) {
        double q = (p - 467) / 466.0;
        offset = 14.85f + 5.15f * ease_out(q);
        fade = 1.0 - q;
    }
    offset *= tex.screen_scale;
    for (int drum = 0; drum < 2; drum++) {
        tex.draw_texture(t_menu_arrow, {.x = -offset, .fade = fade, .index = drum * 2});
        tex.draw_texture(t_menu_arrow,
                         {.mirror = Mirror::HORIZONTAL, .x = offset, .fade = fade, .index = drum * 2 + 1});
        tex.draw_texture(t_menu_decide, {.index = drum});
    }
}

void PracticeGameScreen::draw_review_panel() const {
    double t = now_ms - review_start_ms;
    double s = 1.0;
    if (t < REVIEW_PANEL_OPEN_MS) {
        s = t / REVIEW_PANEL_OPEN_MS;
    } else if (t > REVIEW_PANEL_CLOSE_AT_MS) {
        s = 1.0 - (t - REVIEW_PANEL_CLOSE_AT_MS) / REVIEW_PANEL_CLOSE_MS;
    }
    if (s <= 0.01) {
        return;
    }
    auto sx = static_cast<float>(s);
    float cx = tex.screen_width / 2.0f;
    float panel_w = t_menu_panel->width * sx;
    tex.draw_texture(t_menu_panel,
                     {.x = cx - panel_w / 2.0f, .y = t_review_panel->y[0] - 75 * tex.screen_scale, .x2 = panel_w});
    float rx = t_review_panel->x[0];
    tex.draw_texture(t_review_panel, {.x = (cx + (rx - cx) * sx) - rx, .x2 = t_review_panel->width * sx});
}

const Note* PracticeGameScreen::find_tail(const Note& head) const {

    for (const auto& n : scrobble_note_list) {
        if (n.type == NoteType::TAIL && n.hit_ms > head.hit_ms) {
            return &n;
        }
    }
    return nullptr;
}

void PracticeGameScreen::draw_renda_marker(float cx, float cy, int count, int required) const {
    const float sc = tex.screen_scale;
    const float by = cy - 88 * sc; // bubble centre
    auto draw_centred = [&](TextureObject* t, int frame, float x, float y) {
        tex.draw_texture(t, {.frame = frame, .x = cx + x * sc - t->width / 2.0f, .y = by + y * sc - t->height / 2.0f});
    };
    auto draw_number = [&](TextureObject* t, int value, int slots, const float* xs, float y) {
        std::string str = std::to_string(value);
        auto pad = slots - static_cast<int>(str.size());
        for (int i = 0; i < std::ssize(str); i++) {
            draw_centred(t, str[i] - '0', xs[pad + i], y);
        }
    };
    if (required <= 0) {
        static constexpr float XS[4][4] = {{0}, {-11, 11}, {-22, 0, 22}, {-31, -10.35f, 10.3f, 31}};
        count = std::min(count, 9999);
        auto slots = static_cast<int>(std::to_string(count).size());
        draw_centred(t_renda_bubbles[slots < 4 ? 0 : 1], 0, 0, 0);
        draw_number(t_renda_digits, count, slots, XS[slots - 1], -13);
        return;
    }
    static constexpr float COUNT_XS[3][3] = {{-18}, {-32, -11}, {-47.25f, -26.9f, -6.5f}};
    static constexpr float SLASH_X[3] = {3, 8, 9.25f};
    static constexpr float MAX_XS[3][3] = {{20}, {22, 38}, {21.25f, 36.25f, 51.25f}};
    count = std::min(count, 999);
    required = std::min(required, 999);
    auto slots = static_cast<int>(std::to_string(std::max(count, required)).size());
    draw_centred(t_renda_bubbles[slots - 1], 0, 0, 0);
    draw_number(t_renda_digits, count, slots, COUNT_XS[slots - 1], -13);
    draw_centred(t_renda_slash, 0, SLASH_X[slots - 1], -13);
    draw_number(t_renda_digits_small, required, slots, MAX_XS[slots - 1], -10);
}
