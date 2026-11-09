#include "player.h"

#include <algorithm>
#include <iterator>
#include <string_view>

#include "../../libs/audio/audio.h"
#include "../../libs/file/scores.h"
#include "../../libs/gfx/text.h"
#include "../../libs/input/input.h"
#include "../../libs/time.h"
#include "../game/zdon_ai_board.h"
#include "song_select_script.h"

void SongSelectPlayer::try_lua_selector(bool is_half, float fade_in, int pass) {
    if (script) {
        script->draw_selector(this, is_half, fade_in, pass);
    }
}

SongSelectPlayer::SongSelectPlayer(PlayerNum player_num) : player_num(player_num) {
    PlayerSoundScope audio_owner(player_num);
    std::string player_id = (player_num == global_data.first_login_player) ? global_data.config->network.access_code_1
                                                                           : global_data.config->network.access_code_2;
    if (auto p = scores_manager.get_player_data(player_id)) {
        player_data = *p;
    }

    nameplate = Nameplate(player_data.username, player_data.title, player_num, player_data.dan, player_data.gold,
                          player_data.rainbow, player_data.title_bg);

    selected_difficulty = Difficulty::BACK;
    prev_diff = Difficulty::BACK;
    selected_song = false;
    is_ready = false;
    is_ura = false;
    voice_played = false;
    ura_toggle = 0;
    diff_select_move_right = false;
    last_moved = 0;

    chara = make_chara_from_player_data(&player_data, player_num == PlayerNum::P2);
    if (!player_data.player_id.empty()) {
        chara->set_don_colors(player_data.chara_color_1, player_data.chara_color_2, player_data.chara_color_3);
        chara->apply_face(player_data.chara_face_index);
    } else {
        chara->set_don_colors(chara_default_color_1(player_num), chara_default_color_2(player_num),
                              {249, 240, 225, 255});
    }
    chara->set_anim(AnimIndex::DON_SELECT_LOOP);

    diff_selector_move_1 = dynamic_cast<MoveAnimation*>(tex.get_animation(26, true));
    diff_selector_move_2 = dynamic_cast<MoveAnimation*>(tex.get_animation(27, true));
    selected_diff_bounce = dynamic_cast<MoveAnimation*>(tex.get_animation(33, true));
    selected_diff_fadein = dynamic_cast<FadeAnimation*>(tex.get_animation(34, true));
    selected_diff_highlight_fade = dynamic_cast<FadeAnimation*>(tex.get_animation(35, true));
    selected_diff_text_resize = dynamic_cast<TextureResizeAnimation*>(tex.get_animation(36, true));
    selected_diff_text_fadein = dynamic_cast<FadeAnimation*>(tex.get_animation(37, true));
}

void SongSelectPlayer::update(double current_time) {
    PlayerSoundScope audio_owner(player_num);
    if (board_voice_at_ms >= 0.0 && get_current_ms() >= board_voice_at_ms) {
        board_voice_at_ms = -1.0;
        audio.play_sound("voice_select_diff", VolumePreset::VOICE);
    }
    if (card_enter_at_ms >= 0.0 && get_current_ms() >= card_enter_at_ms) {
        card_enter_at_ms = -1.0;
        navigator.enter_current_box();
    }
    selected_diff_bounce->update(current_time);
    selected_diff_fadein->update(current_time);
    selected_diff_highlight_fade->update(current_time);
    selected_diff_text_resize->update(current_time);
    selected_diff_text_fadein->update(current_time);
    diff_selector_move_1->update(current_time);
    diff_selector_move_2->update(current_time);
    nameplate.update(current_time);
    chara->update(current_time);

    if (neiro_selector.has_value()) {
        neiro_selector->update(current_time);
        if (neiro_selector->is_finished) {
            neiro_selector.reset();
            scores_manager.save_player_data(player_data);
            chara->set_anim(AnimIndex::DON_SELECT_PANELDOWN);
        }
    }

    if (modifier_selector.has_value()) {
        modifier_selector->update(current_time);
        if (modifier_selector->is_finished) {
            modifier_selector.reset();
            scores_manager.save_player_data(player_data);
            chara->set_anim(AnimIndex::DON_SELECT_PANELDOWN);
        }
    }
    if (ura_switch.has_value()) {
        ura_switch->update(current_time);
    }
    if (voice_played && (global_data.config->general.fast_transitions || !is_voice_playing())) {
        is_ready = true;
    }
    sync_level_panel();
}

bool SongSelectPlayer::is_voice_playing() {
    // merge 0ed42ec: Z-DON's silent AI decide wait plus upstream's shared start voice
    return get_current_ms() < voice_silent_until_ms ||
           audio.is_sound_playing(!shared_start_voice.empty()
                                      ? shared_start_voice
                                      : "voice_start_song_" + std::to_string(static_cast<int>(player_num)) + "p");
}

bool SongSelectPlayer::has_course(Difficulty d) const {
    return std::ranges::find(curr_diffs, d) != curr_diffs.end();
}

static constexpr int ONI_COMMAND_COUNT = 10;
static constexpr double ONI_OPEN_MS = 2000.0;

void SongSelectPlayer::hide_oni() {
    oni_hidden = false;
    oni_command = 0;
    oni_open_ms = -1.0;
    SongBox* song = dynamic_cast<SongBox*>(navigator.get_current_item());
    if (!song) {
        return;
    }
    song->oni_locked = false;
    // the slot opens from the course left of it, so the chart needs one
    if (!tex.options[SCO::HIDDEN_ONI] || !std::ranges::contains(curr_diffs, Difficulty::ONI) ||
        curr_diffs.front() >= Difficulty::ONI) {
        return;
    }
    std::erase_if(curr_diffs, [](Difficulty d) { return d >= Difficulty::ONI; });
    song->oni_locked = true;
    oni_hidden = true;
}

void SongSelectPlayer::open_oni() {
    oni_open_ms = get_current_ms();
    audio.play_player_sound("oni_open", player_num, VolumePreset::SOUND);
}

void SongSelectPlayer::init_diff_cursor() {
    hide_oni();
    // Ura mode follows the song: a chart with only an ura course opens in ura mode (there is
    // no oni to toggle from), and a chart without one leaves it.
    const bool has_ura = has_course(Difficulty::URA);
    const bool has_oni = has_course(Difficulty::ONI);
    if (has_ura && !has_oni) {
        is_ura = true;
    } else if (!has_ura && is_ura) {
        is_ura = false;
    }
    ura_toggle = 0;
    if (SongBox* song = dynamic_cast<SongBox*>(navigator.get_current_item())) {
        song->is_ura = is_ura;
    }

    int last = global_data.last_difficulty[static_cast<int>(player_num)];
    if (last < static_cast<int>(Difficulty::EASY)) {
        return;
    }

    Difficulty desired = static_cast<Difficulty>(std::min(last, static_cast<int>(Difficulty::ONI)));

    // the oni slot holds ura in ura mode
    auto slot = [&](Difficulty d) -> Difficulty {
        if (d == Difficulty::URA) {
            return is_ura ? Difficulty::ONI : Difficulty::BACK;
        }
        if (d == Difficulty::ONI) {
            return is_ura ? Difficulty::BACK : Difficulty::ONI;
        }
        return d;
    };
    Difficulty pick = Difficulty::BACK;
    for (Difficulty raw : curr_diffs) {
        Difficulty d = slot(raw);
        if (d < Difficulty::EASY || d > Difficulty::ONI) {
            continue;
        }
        if (d <= desired && (pick == Difficulty::BACK || d > pick)) {
            pick = d;
        }
    }
    if (pick == Difficulty::BACK) {
        // nothing at or below the remembered difficulty - take the lowest
        for (Difficulty raw : curr_diffs) {
            Difficulty d = slot(raw);
            if (d < Difficulty::EASY || d > Difficulty::ONI) {
                continue;
            }
            if (pick == Difficulty::BACK || d < pick) {
                pick = d;
            }
        }
    }
    if (pick != Difficulty::BACK) {
        if (pick == Difficulty::ONI && is_ura) {
            pick = Difficulty::URA;
        }
        selected_difficulty = pick;
        prev_diff = pick;
    }
}

void SongSelectPlayer::reset_selection() {
    is_ready = false;
    selected_song = false;
    voice_played = false;
    level_select = false;
    level_done = false;
    if (level_panel_on) {
        level_panel_on = false;
        zdon_ai_level_panel_close();
    }
    level_course = -1;
}

void SongSelectPlayer::start_background_diffs() {
    selected_diff_text_resize->start();
    selected_diff_text_fadein->start();
    selected_diff_highlight_fade->start();
}

void SongSelectPlayer::sync_ura(bool ura) {
    if (voice_played || is_ready) {
        return;
    }
    if (is_ura == ura) {
        return;
    }
    is_ura = ura;
    ura_toggle = 0;
    if (selected_difficulty == Difficulty::ONI || selected_difficulty == Difficulty::URA) {
        selected_difficulty = Difficulty(7 - static_cast<int>(selected_difficulty));
    }
}

SongSelectState SongSelectPlayer::select_song() {
    audio.play_player_sound("don", player_num, VolumePreset::SOUND);
    BaseBox* item = navigator.get_current_item();
    if (navigator.is_directory(item) && item->genre_index == GenreIndex::DAN) {
        const auto player_index = static_cast<int>(player_num);
        global_data.session_data[player_index].selected_dan_folder = item->path;
        if (player_index >= 0 && player_index < std::ssize(global_data.dan_folder)) {
            global_data.dan_folder[player_index] = item->path;
        }
        return SongSelectState::DAN_SELECTED;
    }
    if (navigator.is_directory(item) && item->genre_index == GenreIndex::TUTORIAL) {
        global_data.session_data[static_cast<int>(global_data.player_num)].selected_tutorial =
            item->path / TUTORIAL_CHART;
        return SongSelectState::TUTORIAL_SELECTED;
    }

    if (navigator.is_directory(item) && item->collection == WAIWAI_NORMAL_COLLECTION) {
        return SongSelectState::NORMAL_MODE_SELECTED;
    }
    if (navigator.is_directory(item) && item->collection == WAIWAI_TUTORIAL_COLLECTION) {
        return SongSelectState::WAIWAI_TUTORIAL_SELECTED;
    }
    if (navigator.is_directory(item) && item->collection == WAIWAI_MODE_COLLECTION) {
        return SongSelectState::WAIWAI_MODE_SELECTED;
    }

    if (navigator.is_song(item)) {
        SongBox* song_item = dynamic_cast<SongBox*>(item);
        curr_diffs = song_item->get_diffs();
        if (curr_diffs.empty()) {
            return SongSelectState::BROWSING;
        }
        card_enter_at_ms = -1.0;
        level_panel_at_ms = 0.0;
        if (zdon_ai_arcade_sort()) {
            const auto hold = tex.skin_config_by_name.find("zdon_ai_card_hold_ms");
            const auto lp = tex.skin_config_by_name.find("zdon_ai_level_panel_ms");
            if (hold != tex.skin_config_by_name.end() && hold->second.x > 0.0f) {
                card_enter_at_ms = get_current_ms() + hold->second.x;
            }
            if (lp != tex.skin_config_by_name.end() && lp->second.x > 0.0f) {
                level_panel_at_ms = get_current_ms() + lp->second.x;
            }
        }
        // the AI select's list stays where it is and fades, the decided board with it (arcade capture MBAZD6xA1D8)
        const bool fade_in_place = silent_start_voice && tex.options[SCO::AI_LIST_FADE_IN_PLACE];
        if (fade_in_place) {
            card_enter_at_ms = get_current_ms() + 300.0;
        }
        navigator.enter_diff_select(card_enter_at_ms >= 0.0, fade_in_place);
        selected_song = true;
        // the course board opens with voice_select_diff (footage YKjsle7VBCI 61.73 s, 0vjiSUv61i4 45.13 s,
        // Tl7g8XXXvAM 126.80 / 332.89 / 520.11 s)
        // Green AI select (skin_config zdon_ai_board_voice_ms): the voice sounds when the board shows, 0.99 s after the
        // don on the card (Tl7g8XXXvAM don 125.81, voice 126.80); keys absent / 0 keep the voice at the don
        board_voice_at_ms = -1.0;
        if (audio.has_sound("voice_select_diff")) {
            const auto bv = tex.skin_config_by_name.find("zdon_ai_board_voice_ms");
            if (zdon_ai_arcade_sort() && bv != tex.skin_config_by_name.end() && bv->second.x > 0.0f) {
                board_voice_at_ms = get_current_ms() + bv->second.x;
            } else {
                audio.play_sound("voice_select_diff", VolumePreset::VOICE);
            }
        }
        init_diff_cursor();
        selected_diff_bounce->start();
        selected_diff_fadein->start();
        return SongSelectState::SONG_SELECTED;
    }

    if (navigator.is_directory(item)) {
        navigator.load_current_directory(item->path);
    }
    return SongSelectState::BROWSING;
}

SongSelectState SongSelectPlayer::handle_input_browsing(double current_ms) {

    bool l_kat = is_l_kat_pressed(player_num) || key_pressed(SDL_SCANCODE_LEFT);
    bool r_kat = is_r_kat_pressed(player_num) || key_pressed(SDL_SCANCODE_RIGHT);
    bool l_don = is_l_don_pressed(player_num) || key_pressed(SDL_SCANCODE_RETURN);
    bool r_don = is_r_don_pressed(player_num) || key_pressed(SDL_SCANCODE_RETURN);
    float wheel = mouse_wheel();

    if (key_pressed(SDL_SCANCODE_F5)) {
        navigator.load_current_directory(navigator.current_path);
    }
    // Z-DON: F2 opens the song search from anywhere on the wheel
    if (key_pressed(SDL_SCANCODE_F2)) {
        search_guard = true;
        search_guard_until_ms = current_ms + 150.0;
        return SongSelectState::SEARCHING;
    }

    bool navigated = false;
    if (key_pressed(SDL_SCANCODE_LCTRL) || (l_kat && current_ms <= last_moved + 50)) {
        audio.play_player_sound("skip", player_num, VolumePreset::SOUND);
        navigator.skip_left();
        last_moved = current_ms;
        navigated = true;
    } else if (l_kat || wheel > 0) {
        audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        navigator.move_left();
        last_moved = current_ms;
        navigated = true;
    }

    if (key_pressed(SDL_SCANCODE_RCTRL) || (r_kat && current_ms <= last_moved + 50)) {
        audio.play_player_sound("skip", player_num, VolumePreset::SOUND);
        navigator.skip_right();
        last_moved = current_ms;
        navigated = true;
    } else if (r_kat || wheel < 0) {
        audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        navigator.move_right();
        last_moved = current_ms;
        navigated = true;
    }

    int dir = is_kat_held(true, player_num) ? -1 : is_kat_held(false, player_num) ? 1 : 0;
    if (l_kat || r_kat) {
        hold_dir = dir;
        next_repeat = current_ms + 350;
    } else if (dir != hold_dir) {
        hold_dir = 0;
    }
    if (hold_dir && !navigated && current_ms >= next_repeat) {
        audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        if (hold_dir < 0) {
            navigator.move_left();
        } else {
            navigator.move_right();
        }
        last_moved = current_ms;
        next_repeat = current_ms + 70;
    }

    if (key_pressed(SDL_SCANCODE_SPACE)) {
        BaseBox* item = navigator.get_current_item();
        if (navigator.is_song(item)) {
            navigator.toggle_favorite(static_cast<SongBox*>(item));
            audio.play_player_sound("add_favorite", player_num, VolumePreset::SOUND);
        }
    }

    // Z-DON: a don pressed with (or just after) a move opened the new box while the wheel still
    // scrolled; the skin then played the decide on the box it last drew open (the old one), which
    // stayed open over the new one. The don now waits until the box the cursor moved to has stopped
    // and been drawn open for a frame (at most 500 ms).
    if (current_ms - last_browse_ms > 100.0) {
        pending_select_ms = -1; // pressed before another state took the input
        open_frames = 0;
    }
    last_browse_ms = current_ms;
    BaseBox* current = navigator.get_current_item();
    open_frames = (current && std::string_view(current->draw_state()) != "closed") ? open_frames + 1 : 0;
    if (l_don || r_don) {
        pending_select_ms = current_ms;
    }
    const bool settled = open_frames >= 2 || current_ms - pending_select_ms >= 500.0;
    if (pending_select_ms >= 0 && !navigated && settled) {
        pending_select_ms = -1;
        BaseBox* item = navigator.get_current_item();
        if (navigator.is_directory(item) && item->collection == COLLECTIONS[5]) {
            search_guard = true;
            search_guard_until_ms = get_current_ms() + 150.0;
            return SongSelectState::SEARCHING;
        }
        return select_song();
    }
    return SongSelectState::BROWSING;
}

std::optional<std::pair<int, int>> SongSelectPlayer::handle_input_diff_sort(DiffSortSelect* diff_sort_selector) {
    if (is_l_kat_pressed(player_num)) {
        diff_sort_selector->input_left();
        if (!tex.options[SCO::ONE_MENU_SORT]) {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        }
    }
    if (is_r_kat_pressed(player_num)) {
        diff_sort_selector->input_right();
        if (!tex.options[SCO::ONE_MENU_SORT]) {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        }
    }
    if (is_l_don_pressed(player_num) || is_r_don_pressed(player_num)) {
        if (!tex.options[SCO::ONE_MENU_SORT]) {
            audio.play_player_sound("don", player_num, VolumePreset::SOUND);
        }
        return diff_sort_selector->input_select();
    }
    return std::nullopt;
}

// Strip leading / trailing blanks: ASCII whitespace and the ideographic space U+3000.
static std::string trim_search(std::string s) {
    auto blank_at = [&](size_t i, size_t& len) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b == ' ' || b == '\t' || b == '\r' || b == '\n') {
            len = 1;
            return true;
        }
        if (b == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            static_cast<unsigned char>(s[i + 2]) == 0x80) {
            len = 3;
            return true;
        }
        return false;
    };
    size_t len = 0;
    while (!s.empty() && blank_at(0, len)) {
        s.erase(0, len);
    }
    for (;;) {
        if (s.empty()) {
            break;
        }
        if (s.size() >= 3 && blank_at(s.size() - 3, len) && len == 3) {
            s.resize(s.size() - 3);
            continue;
        }
        if (blank_at(s.size() - 1, len) && len == 1) {
            s.pop_back();
            continue;
        }
        break;
    }
    return s;
}

static bool any_don_key_down(PlayerNum player_num) {
    const auto& c = *global_data.config;
    auto down = [](const std::vector<int>& keys) {
        return std::ranges::any_of(keys, [](int k) { return key_down(k); });
    };
    if (player_num != PlayerNum::P2 && (down(c.keys_1p.left_don) || down(c.keys_1p.right_don))) {
        return true;
    }
    if (player_num != PlayerNum::P1 && (down(c.keys_2p.left_don) || down(c.keys_2p.right_don))) {
        return true;
    }
    return false;
}

std::optional<std::string> SongSelectPlayer::handle_input_search() {
    // The guard is for physical keyboards. An on-screen keyboard (a console's native dialog)
    // returns a completed query, never the opening drum key: don't discard a fast result.
    if (SDL_HasScreenKeyboardSupport()) {
        search_guard = false;
    }
    if (search_guard) {
        // The don press is seen by the input thread a few ms before raylib's own key state
        // and the typed character catch up, so the opening keystroke (F/J) would land in the
        // query. Swallow typed input for a short grace period and until every don key is up.
        while (char_pressed() > 0) {
        }
        if (get_current_ms() >= search_guard_until_ms && !any_don_key_down(player_num)) {
            search_guard = false;
        }
        return std::nullopt;
    }

    TextEditAction action = poll_text_edit(search_string);
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
    if (action == TextEditAction::None && (is_l_don_pressed(player_num) || is_r_don_pressed(player_num))) {
        action = TextEditAction::Confirm;
    }
#endif
    if (action == TextEditAction::Confirm || action == TextEditAction::Cancel) {
        std::string result = action == TextEditAction::Confirm ? trim_search(search_string) : "";
        search_string = "";
        clear_input_buffers();
        return result;
    }
    return std::nullopt;
}

static bool neiro_in_options() {
    return tex.options[SCO::OPTION_NEIRO_ROW];
}

void SongSelectPlayer::decide_course() {
    voice_played = true;
    start_background_diffs();
    if (silent_start_voice) {
        // the AI battle's decide has no start voice (0vjiSUv61i4, 0wD5kkcN5WA, Tl7g8XXXvAM: none of the
        // battles has it, the normal play footage does); the transition still waits the voice's 2 s
        voice_silent_until_ms = get_current_ms() + 2000.0;
        if (tex.options[SCO::AI_START_VOICE] && audio.has_sound("ai_select_vo_begin")) {
            audio.play_sound("ai_select_vo_begin", VolumePreset::VOICE);
        }
    } else if (shared_start_voice.empty()) { // upstream: a shared voice is played by the screen
        audio.play_player_sound("voice_start_song_" + std::to_string(static_cast<int>(player_num)) + "p", player_num,
                                VolumePreset::VOICE);
    }
}

// zdon_ai_arcade_flow: the level panel of the cabinet's course board. It lists the levels of the highlighted course
// (the window ends at the course's top level); the cursor starts on the song's own level, the other pillars are levels
// the song does not have (a don there refuses)
void SongSelectPlayer::sync_level_panel() {
    if (!zdon_ai_arcade_sort()) {
        return;
    }
    auto* song = dynamic_cast<SongBox*>(navigator.get_current_item());
    const bool want = selected_song && get_current_ms() >= level_panel_at_ms && !voice_played &&
                      !neiro_selector.has_value() && !modifier_selector.has_value() &&
                      selected_difficulty >= Difficulty::EASY && song != nullptr &&
                      song->course_info(static_cast<int>(selected_difficulty)).level > 0;
    if (!want) {
        if (level_panel_on) {
            level_panel_on = false;
            level_select = false;
            level_course = -1;
            zdon_ai_level_panel_close();
        }
        return;
    }
    const int course = static_cast<int>(selected_difficulty);
    if (!level_panel_on || course != level_course) {
        level_song = song->course_info(course).level;
        level_top = std::max(zdon_ai_board_top(course), level_song);
        level_sel = level_song;
        level_course = course;
        if (!level_panel_on) {
            zdon_ai_level_panel_open(course, level_sel, level_top);
        } else {
            zdon_ai_level_panel_course(course, level_sel, level_top);
        }
        level_panel_on = true;
    }
    zdon_ai_level_panel_phase(!level_select);
}

bool SongSelectPlayer::begin_level_select() {
    sync_level_panel();
    if (!level_panel_on) {
        return false;
    }
    level_select = true;
    zdon_ai_level_panel_phase(false);
    return true;
}

void SongSelectPlayer::handle_level_select(bool l_kat, bool r_kat, bool l_don, bool r_don) {
    if (l_kat || r_kat) {
        audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
        if (l_kat && level_sel <= 1) { // past the lowest level: back to the course board
            level_select = false;
            zdon_ai_level_panel_phase(true);
            return;
        }
        level_sel = l_kat ? level_sel - 1 : std::min(level_top, level_sel + 1);
        zdon_ai_level_panel_set(level_sel);
    } else if (l_don || r_don) {
        if (level_sel != level_song) {
            audio.play_player_sound("cancel", player_num, VolumePreset::SOUND);
            return;
        }
        audio.play_player_sound("don", player_num, VolumePreset::SOUND);
        level_select = false;
        level_done = true;
        level_panel_on = false;
        zdon_ai_level_panel_close();
        decide_course();
    }
}

SongSelectState SongSelectPlayer::handle_input_selecting() {
    bool l_kat = is_l_kat_pressed(player_num);
    bool r_kat = is_r_kat_pressed(player_num);
    bool l_don = is_l_don_pressed(player_num);
    bool r_don = is_r_don_pressed(player_num);

    if (voice_played) {
        return SongSelectState::SONG_SELECTED;
    }
    sync_level_panel();
    if (level_select) {
        handle_level_select(l_kat, r_kat, l_don, r_don);
        return SongSelectState::SONG_SELECTED;
    }
    if (oni_open_ms >= 0.0 && get_current_ms() - oni_open_ms < ONI_OPEN_MS) {
        return SongSelectState::SONG_SELECTED;
    }
    // the slot appears once the open animation ends (here or on the other player's side)
    if (oni_hidden) {
        SongBox* song = dynamic_cast<SongBox*>(navigator.get_current_item());
        if (song && (oni_open_ms >= 0.0 || !song->oni_locked)) {
            song->oni_locked = false;
            curr_diffs = song->get_diffs();
            oni_hidden = false;
        }
    }

    if (l_kat) {
        if (modifier_selector.has_value()) {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
            modifier_selector->left();
        } else if (neiro_selector.has_value()) {
            neiro_selector->left();
        } else {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
            navigate_difficulty_left();
            if (selected_difficulty >= Difficulty::EASY) {
                selected_diff_bounce->start();
                selected_diff_fadein->start();
            }
        }
    } else if (r_kat) {
        if (modifier_selector.has_value()) {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
            modifier_selector->right();
        } else if (neiro_selector.has_value()) {
            neiro_selector->right();
        } else {
            audio.play_player_sound("kat", player_num, VolumePreset::SOUND);
            Difficulty prev_difficulty = selected_difficulty;
            navigate_difficulty_right();
            if (selected_difficulty >= Difficulty::EASY) {
                selected_diff_bounce->start();
                if (prev_difficulty != selected_difficulty) {
                    selected_diff_fadein->start();
                }
            }
        }
    } else if (l_don || r_don) {
        audio.play_player_sound("don", player_num, VolumePreset::SOUND);
        if (modifier_selector.has_value()) {
            modifier_selector->confirm();
        } else if (neiro_selector.has_value()) {
            neiro_selector->confirm();
        } else {
            if (selected_difficulty == Difficulty::MODIFIER) {
                modifier_selector = ModifierSelector(player_num, &player_data);
                chara->set_anim(AnimIndex::DON_SELECT_PANELUP);
            } else if (selected_difficulty == Difficulty::NEIRO && !neiro_in_options()) {
                neiro_selector = NeiroSelector(player_num, &player_data);
                chara->set_anim(AnimIndex::DON_SELECT_PANELUP);
            } else if (selected_difficulty >= Difficulty::EASY) {
                if (!(zdon_ai_arcade_sort() && !level_done && begin_level_select())) {
                    decide_course();
                }
            } else if (selected_difficulty == Difficulty::BACK) {
                is_ready = true;
            }
        }
    }
    return SongSelectState::SONG_SELECTED;
}

void SongSelectPlayer::navigate_difficulty_left() {
    diff_select_move_right = false;

    if (is_ura && selected_difficulty == Difficulty::URA) {
        diff_selector_move_1->start();
        prev_diff = selected_difficulty;
        Difficulty below = Difficulty::BACK;
        for (Difficulty d : curr_diffs) {
            if (d >= Difficulty::EASY && d <= Difficulty::HARD && (below == Difficulty::BACK || d > below)) {
                below = d;
            }
        }
        selected_difficulty =
            (below != Difficulty::BACK) ? below : (neiro_in_options() ? Difficulty::MODIFIER : Difficulty::NEIRO);
    } else if (selected_difficulty == Difficulty::NEIRO || selected_difficulty == Difficulty::MODIFIER) {
        diff_selector_move_2->start();
        prev_diff = selected_difficulty;
        selected_difficulty = Difficulty(static_cast<int>(selected_difficulty) - 1);
    } else if (selected_difficulty == Difficulty::BACK) {
        // no-op
    } else if (!std::ranges::contains(curr_diffs, selected_difficulty)) {
        prev_diff = selected_difficulty;
        diff_selector_move_1->start();
        selected_difficulty = curr_diffs.front();
    } else if (selected_difficulty == curr_diffs.front()) {
        diff_selector_move_2->start();
        prev_diff = selected_difficulty;
        selected_difficulty = neiro_in_options() ? Difficulty::MODIFIER : Difficulty::NEIRO;
    } else {
        diff_selector_move_1->start();
        prev_diff = selected_difficulty;
        auto it = std::ranges::find(curr_diffs, selected_difficulty);
        selected_difficulty = *std::prev(it);
    }
}

void SongSelectPlayer::navigate_difficulty_right() {
    diff_select_move_right = true;

    if (oni_hidden && selected_difficulty == curr_diffs.back()) {
        if (oni_open_ms < 0.0 && ++oni_command >= ONI_COMMAND_COUNT) {
            open_oni();
        }
        return;
    }

    bool has_ura = has_course(Difficulty::URA);
    bool has_oni = has_course(Difficulty::ONI);

    if (is_ura && has_ura && selected_difficulty == Difficulty::HARD) {
        prev_diff = selected_difficulty;
        selected_difficulty = Difficulty::URA;
        diff_selector_move_1->start();
    }

    if ((selected_difficulty == Difficulty::ONI || selected_difficulty == Difficulty::URA) && has_ura && has_oni) {
        ura_toggle = (ura_toggle + 1) % 10;
        if (ura_toggle == 0) {
            toggle_ura_mode();
        }
    } else if (selected_difficulty == Difficulty::NEIRO ||
               (selected_difficulty == Difficulty::MODIFIER && neiro_in_options())) {
        prev_diff = selected_difficulty;
        selected_difficulty = curr_diffs.front();
        diff_selector_move_2->start();
        diff_selector_move_1->start();
    } else if (selected_difficulty == Difficulty::MODIFIER || selected_difficulty == Difficulty::BACK) {
        prev_diff = selected_difficulty;
        selected_difficulty = Difficulty(static_cast<int>(selected_difficulty) + 1);
        diff_selector_move_2->start();
    } else if (selected_difficulty < curr_diffs.back()) {
        prev_diff = selected_difficulty;
        auto it = std::ranges::find(curr_diffs, selected_difficulty);
        selected_difficulty = *std::next(it);
        diff_selector_move_1->start();
    }
}

void SongSelectPlayer::toggle_ura_mode() {
    ura_toggle = 0;
    is_ura = !is_ura;
    audio.play_player_sound("ura_switch", player_num, VolumePreset::SOUND);
    selected_difficulty = Difficulty(7 - static_cast<int>(selected_difficulty));
    ura_switch.emplace();
    ura_switch->start(is_ura);
    SongBox* song = dynamic_cast<SongBox*>(navigator.get_current_item());
    if (song) {
        song->is_ura = is_ura;
    }
}

void SongSelectPlayer::draw_background_diffs(SongSelectState state) {
    if (!selected_song || state != SongSelectState::SONG_SELECTED || selected_difficulty < Difficulty::EASY) {
        return;
    }
    if (script) {
        script->draw_background_diffs(this);
    }
}

void SongSelectPlayer::draw(SongSelectState state, bool is_half, float diff_fade_in) {
    PlayerSoundScope audio_owner(player_num);
    if (selected_song && state == SongSelectState::SONG_SELECTED) {
        try_lua_selector(is_half, diff_fade_in, 1);
    }

    float offset = 0.0f;
    if (neiro_selector.has_value()) {
        offset = option_panel_offset(neiro_selector->move, neiro_selector->is_confirmed);
    }
    if (modifier_selector.has_value()) {
        offset = option_panel_offset(modifier_selector->move, modifier_selector->is_confirmed);
    }

    // a skin that draws the option panel itself keeps the character where it is
    if (option_panel_by_lua) {
        offset = 0.0f;
    }

    const char* slot = player_num == PlayerNum::P1 ? "song_select_1p" : "song_select_2p";
    auto [nx, ny] = tex.nameplate_xy(slot);
    nameplate.draw(nx, ny);
    auto [cx, cy] = tex.chara_xy(slot);
    // the AI select (Green, skin_config zdon_ai_don {dx, dy} + zdon_ai_don_scale {percent}): the arcade draws the Don
    // larger and a little to the right; keys absent or 0 (Nijiiro, normal modes) leave the Don where it is
    float chara_mul = 1.0f;
    if (zdon_ai_arcade_sort()) { // Green AI select only (zdon_ai_arcade_flow)
        const auto sc = tex.skin_config_by_name.find("zdon_ai_don_scale");
        if (sc != tex.skin_config_by_name.end() && sc->second.x > 0.0f) {
            chara_mul = sc->second.x / 100.0f;
            const auto po = tex.skin_config_by_name.find("zdon_ai_don");
            if (po != tex.skin_config_by_name.end()) {
                cx += po->second.x;
                cy += po->second.y;
            }
        }
    }
    chara->draw(cx, cy + (offset * 0.6f), chara_mul);

    bool panel_by_lua = false;
    if (neiro_selector.has_value() && script) {
        panel_by_lua = script->draw_option_panel(this, 2) || panel_by_lua;
    }
    if (modifier_selector.has_value() && script) {
        panel_by_lua = script->draw_option_panel(this, 1) || panel_by_lua;
    }
    option_panel_by_lua = panel_by_lua;
    if (ura_switch.has_value()) {
        ura_switch->draw();
    }
}
