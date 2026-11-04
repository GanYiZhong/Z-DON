#pragma once
#include <algorithm>

#include "../libs/audio/audio.h"
#include "../libs/file/scores.h"
#include "../libs/input/input.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/game/zdon_ai_board.h"
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
    bool music_cut = false;
    int seek_steps = 0;
    int seek_moved = 0;
    double seek_next_ms = 0;
    void list_time_up() {
        page0 = false;
        set_diff_stage(false);
        close_board();
        on_select_time_up();
        if (state != SongSelectState::BROWSING) {
            return;
        }
        seek_steps = 600;
        seek_moved = 0;
        seek_next_ms = 0;
    }
    void after_select_timers(double now) override {
        // the cabinet cuts the select music the moment the course is decided (footage Tl7g8XXXvAM 2:12 and
        // 0wD5kkcN5WA 1:44: silence until the loading card's cues); the decide voice goes on alone
        if (player && player->difficulty_decided()) {
            if (!music_cut) {
                music_cut = true;
                if (audio.is_music_stream_valid("preview")) {
                    audio.stop_music_stream("preview");
                }
                for (const char* bgm : {"bgm", "ai_bgm"}) {
                    if (audio.has_sound(bgm)) {
                        audio.stop_sound(bgm);
                    }
                }
            }
        } else {
            music_cut = false;
        }
        if (board_jump && state == SongSelectState::BROWSING && !navigator.is_processing &&
            !navigator.inline_streaming) {
            BaseBox* item = navigator.get_current_item();
            if (navigator.is_song(item) && now >= board_jump_since + 300) {
                if (p23) {
                    if (board_land < 0) {
                        board_land = now;
                    }
                    zdon_ai_board_jump_set(now - board_land);
                    if (now < board_land + P23_HOLD_MS) {
                        return;
                    }
                }
                board_jump = false;
                close_board();
                state = player->select_song();
                return;
            }
            if (now > board_jump_since + 8000) { // the jump never landed: leave it to the wheel
                board_jump = false;
                close_board();
            }
        }
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

    // skin_config zdon_ai_native_sort (Green): the AI select uses the normal song select's own Diff Sort box instead of
    // the skin-built course columns / song board: the cursor starts on the "Difficulty Sort" box of the genre shelf
    // (the cabinet's page 0 has the box in the first slot) and a don opens the engine's sort window
    bool focus_sort = false;
    int focus_sort_steps = 0;
    void focus_sort_tick() {
        if (!focus_sort || state != SongSelectState::BROWSING || navigator.is_processing ||
            navigator.inline_streaming) {
            return;
        }
        BaseBox* item = navigator.get_current_item();
        if (item == nullptr) {
            return;
        }
        if ((navigator.is_directory(item) && item->genre_index == GenreIndex::DIFFICULTY) || focus_sort_steps >= 60) {
            focus_sort = false;
            return;
        }
        navigator.move_right();
        ++focus_sort_steps;
    }

    // skin_config zdon_ai_diff_first (Green): the cabinet asks for the course first (columns: back, easy .. oni, the
    // recommended one, the last one; footage Tl7g8XXXvAM 1:50), then the song list; the chosen course is the one
    // the course board opens on
    bool diff_stage = false;
    int diff_cursor = 5;
    bool diff_leave = false;
    // skin_config zdon_ai_page0 (Green): the cabinet opens on the genre shelf with a "choose by difficulty" box (page
    // 0, tex.ai_diff_stage() = -1); a don opens it into the course columns, "back" there returns to the shelf
    bool page0 = false, page0_enabled = false;
    int page0_off = 0;
    bool board_enabled = false; // skin_config zdon_ai_song_board (Green)
    void set_diff_stage(bool on) {
        diff_stage = on;
        if (!on) {
            zdon_ai_board_info_set(false, 0, {});
        }
        zdon_ai_set_diff_stage(on, on && page0 ? -1 : diff_cursor);
    }
    // the course-filtered song board (tex.ai_board(); footage Tl7g8XXXvAM 1:55-2:10): kats walk the songs, a don goes
    // to the song's course board; the first strip is "back"
    bool board_stage = false, board_jump = false;
    double board_jump_since = 0;
    // skin_config zdon_ai_p23 (Green): the chosen card is held for the skin page 2 -> 3 transition (lanes, band into
    // the arena, card widening; footage Tl7g8XXXvAM t=124.4-127) before the course board opens
    bool p23 = false;
    double board_land = -1;
    static constexpr double P23_HOLD_MS = 2300;
    int board_cursor = 0;
    AiBoardCourse board_course;
    std::vector<fs::path> board_songs;
    std::array<AiBoardCourse, 7> col_info;
    std::array<bool, 7> col_known{};
    void close_board() {
        if (board_stage) {
            board_stage = false;
            board_land = -1;
            zdon_ai_board_jump_set(-1);
            zdon_ai_board_set(false, board_course, {}, 0);
        }
    }
    bool open_board(int column) {
        const int last = global_data.last_difficulty[0];
        AiBoardCourse c = zdon_ai_board_course(column, last >= 0 ? last : -1, last_diff_sort.second);
        auto songs = zdon_ai_board_songs(c);
        if (songs.empty()) {
            return false;
        }
        board_course = c;
        board_songs = std::move(songs);
        board_cursor = 0;
        board_stage = true;
        zdon_ai_board_set(true, c, board_songs, board_cursor);
        return true;
    }
    void handle_board_input(double current_ms) {
        if (board_jump) {
            return;
        }
        const int n = static_cast<int>(board_songs.size());
        int next = board_cursor;
        if (is_l_kat_pressed(PlayerNum::P1)) {
            next = std::max(-1, board_cursor - 1);
        } else if (is_r_kat_pressed(PlayerNum::P1)) {
            next = std::min(n - 1, board_cursor + 1);
        }
        if (next != board_cursor) {
            board_cursor = next;
            audio.play_player_sound("kat", PlayerNum::P1, VolumePreset::SOUND);
        } else if (is_l_don_pressed(PlayerNum::P1) || is_r_don_pressed(PlayerNum::P1)) {
            audio.play_player_sound("don", PlayerNum::P1, VolumePreset::SOUND);
            if (board_cursor < 0) {
                close_board();
                set_diff_stage(true);
                return;
            }
            global_data.last_difficulty[0] = board_course.course;
            if (navigator.jump_to_song_path(board_songs[board_cursor])) {
                board_jump = true;
                board_jump_since = current_ms;
            } else {
                close_board();
            }
            return;
        }
        zdon_ai_board_set(true, board_course, board_songs, board_cursor);
    }
    void handle_input_browsing(double current_ms) override {
        if (board_stage) {
            handle_board_input(current_ms);
            return;
        }
        if (!diff_stage) {
            SongSelectScreen::handle_input_browsing(current_ms);
            return;
        }
        if (page0) {
            // the shelf scrolls on kats (the engine's genre boxes beside the box); the box is slot 0, one slot per kat
            const bool strict = tex.skin_config_by_name.contains("zdon_green_strict");
            const bool kl = is_l_kat_pressed(PlayerNum::P1), kr = is_r_kat_pressed(PlayerNum::P1);
            if (kl || kr) {
                if (strict && (kr || (kl && page0_off > 0))) {
                    page0_off += kr ? 1 : -1;
                    audio.play_player_sound("kat", PlayerNum::P1, VolumePreset::SOUND);
                    if (kr) {
                        navigator.move_right(); // the input was read above, so the shelf is moved here
                    } else {
                        navigator.move_left();
                    }
                    zdon_ai_set_diff_stage(true, page0_off == 0 ? -1 : -2);
                } else {
                    audio.play_player_sound("kat", PlayerNum::P1, VolumePreset::SOUND);
                }
            } else if (strict && page0_off > 0) {
                // another genre of the shelf is focused: the normal song select reads the don (the shelf is its own)
                SongSelectScreen::handle_input_browsing(current_ms);
            } else if (is_l_don_pressed(PlayerNum::P1) || is_r_don_pressed(PlayerNum::P1)) {
                audio.play_player_sound("don", PlayerNum::P1, VolumePreset::SOUND);
                page0 = false;
                zdon_ai_set_diff_stage(true, diff_cursor);
            }
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
                if (page0_enabled) {
                    page0 = true;
                    zdon_ai_set_diff_stage(true, -1);
                    return;
                }
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
            if (board_enabled) {
                open_board(diff_cursor);
            }
            return;
        }
        zdon_ai_set_diff_stage(true, diff_cursor);
        if (board_enabled && diff_cursor >= 1) {
            if (!col_known[diff_cursor]) {
                const int last = global_data.last_difficulty[0];
                col_info[diff_cursor] = zdon_ai_board_course(diff_cursor, last >= 0 ? last : -1, last_diff_sort.second);
                col_known[diff_cursor] = true;
            }
            zdon_ai_board_info_set(true, diff_cursor, col_info[diff_cursor]);
        } else {
            zdon_ai_board_info_set(false, diff_cursor, {});
        }
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
        board_stage = board_jump = false;
        col_known.fill(false);
        board_enabled = tex.skin_config_by_name.contains("zdon_ai_song_board");
        p23 = tex.skin_config_by_name.contains("zdon_ai_p23");
        board_land = -1;
        page0_enabled =
            tex.skin_config_by_name.contains("zdon_ai_page0") && tex.skin_config_by_name.contains("zdon_ai_diff_first");
        page0 = page0_enabled;
        page0_off = 0;
        {
            const auto ns =
                tex.skin_config_by_name.find("zdon_ai_native_sort"); // a child skin switches it off with x = 0
            focus_sort = ns != tex.skin_config_by_name.end() && ns->second.x != 0.0f;
        }
        focus_sort_steps = 0;
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
        focus_sort_tick();
        const double now = get_frame_ms();
        ai_plate.update(now);
        if (ai_chara) {
            ai_chara->update(now);
        }
        return next;
    }
};
