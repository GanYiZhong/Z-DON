#pragma once

#include "../../libs/gfx/texture.h"
#include "../global/chara_3d.h"
#include "../global/nameplate.h"
#include "diff_sort.h"
#include "modifier.h"
#include "neiro.h"
#include "ura_switch.h"

class SongSelectScript;

class SongSelectPlayer {
public:
    PlayerNum player_num;
    PlayerData player_data;
    Difficulty selected_difficulty;
    Difficulty prev_diff;
    std::vector<Difficulty> curr_diffs;
    double last_moved;
    double next_repeat = 0;
    int hold_dir = 0;
    double pending_select_ms = -1; // a don pressed while the wheel still scrolls, taken once it settles
    double last_browse_ms = 0;
    int open_frames = 0; // frames the current box has been open (drawn open by the skin)
    bool selected_song;
    bool is_ready;
    bool is_ura;
    int ura_toggle;
    bool diff_select_move_right;
    std::string search_string;

    bool search_guard = false;
    double search_guard_until_ms = 0.0;

    std::optional<NeiroSelector> neiro_selector;
    std::optional<ModifierSelector> modifier_selector;
    std::optional<UraSwitchAnimation> ura_switch;

    MoveAnimation* diff_selector_move_1;
    MoveAnimation* diff_selector_move_2;
    MoveAnimation* selected_diff_bounce;
    FadeAnimation* selected_diff_fadein;
    FadeAnimation* selected_diff_highlight_fade;
    TextureResizeAnimation* selected_diff_text_resize;
    FadeAnimation* selected_diff_text_fadein;

    std::unique_ptr<Chara3D> chara;
    Nameplate nameplate;

    SongSelectScript* script = nullptr;

    bool option_panel_by_lua = false;
    bool silent_start_voice = false; // the AI select: the decide plays no start voice (the wait stays)

    bool solo = true;
    std::string shared_start_voice;
    std::string start_voice;
    double oni_open_ms = -1.0;

    SongSelectPlayer(PlayerNum player_num);

    void update(double current_time);
    bool is_voice_playing();
    std::string own_start_voice() const;

    // zdon_ai_arcade_flow (Green AI select): the course board carries the cabinet's level panel (footage Tl7g8XXXvAM
    // t=127-131): it slides in with the board and follows the highlighted course on the lit board (decide_course); a
    // don on a course dims the board and moves to select_lv, where kats choose the level and a don decides
    bool level_select = false;
    bool in_level_step() const { return level_select; }
    void sync_level_panel();

    bool difficulty_decided() const { return selected_difficulty >= Difficulty::EASY && (voice_played || is_ready); }

    SongSelectState select_song();
    void sync_ura(bool ura);
    void init_diff_cursor();
    void reset_selection();
    SongSelectState handle_input_browsing(double current_ms);
    SongSelectState handle_input_selecting();
    std::optional<std::pair<int, int>> handle_input_diff_sort(DiffSortSelect* diff_sort_selector);
    std::optional<std::string> handle_input_search();

    void try_lua_selector(bool is_half, float fade_in, int pass);
    void draw_background_diffs(SongSelectState state);
    void draw(SongSelectState state, bool is_half, float diff_fade_in);

private:
    bool voice_played;
    double board_voice_at_ms =
        -1.0; // skin_config zdon_ai_board_voice_ms: voice_select_diff when the board shows (Green AI)
    double voice_silent_until_ms = 0.0; // the AI battle's decide: no voice, the same wait
    // Green AI select (skin keys zdon_ai_card_hold_ms / zdon_ai_level_panel_ms): the cabinet holds the card 29 frames
    // after the don before it widens and 64 frames before the level panel rises (Tl7g8XXXvAM 125.81 s)
    double card_enter_at_ms = -1.0;
    double level_panel_at_ms = 0.0;
    bool oni_hidden = false;
    int oni_command = 0;
    void hide_oni();
    void open_oni();
    void navigate_difficulty_left();
    void navigate_difficulty_right();
    bool has_course(Difficulty d) const;
    void toggle_ura_mode();
    void start_background_diffs();
    void decide_course();
    bool begin_level_select();
    void handle_level_select(bool l_kat, bool r_kat, bool l_don, bool r_don);
    bool level_done = false, level_panel_on = false;
    int level_sel = 1, level_top = 4, level_song = 0, level_course = -1;
    // The panel just closed: keep the account's server-side copy in step
    void push_modifiers();
};
