#pragma once

#include <stop_token>

#include "../libs/gfx/color_utils.h"
#include "../libs/screen.h"
#include "../objects/game/song_info.h"
#include "../objects/game/transition.h"
#include "../objects/global/allnet_indicator.h"
#include "../objects/global/coin_overlay.h"
#include "../objects/global/indicator.h"
#include "../objects/global/timer.h"
#include "../objects/song_select/dan_transition.h"
#include "../objects/song_select/player.h"
#include "../objects/song_select/search_box.h"
#include "../objects/song_select/song_select_script.h"

struct ReplayData;

class SongSelectScreen : public Screen {
protected:
    FadeAnimation* diff_fade_out = nullptr;
    std::unique_ptr<SongSelectScript> script;
    TextureObject* t_song_num_bg = nullptr;

    SongSelectState state = SongSelectState::BROWSING;

    std::optional<Transition> game_transition;
    std::optional<DanTransition> dan_transition;
    CoinOverlay coin_overlay;
    AllNetIcon allnet_indicator;
    std::unique_ptr<Timer> select_timer;
    std::unique_ptr<Timer> diff_select_timer;
    std::unique_ptr<Indicator> indicator;
    Statistics cached_stats;
    std::stop_source stats_stop;
    std::future<Statistics> stats_future;

    Shader shader;
    Color color;
    std::unique_ptr<SongNum> song_num;

    std::unique_ptr<SongSelectPlayer> player;

    std::optional<DiffSortSelect> diff_sort_selector;
    std::pair<int, int> last_diff_sort = {-1, -1};
    int last_diff_order = 1;
    void apply_sort_window_result();
    void update_selection_ui(double current_time, bool advance_timers);
    void update_diff_sort(double current_time);
    void start_search();

    std::optional<SearchBox> search_box;

    // Z-DON song search panel (song_select_zdon_search.cpp): live results under the search box
    struct ZdonSearch {
        std::string key; // the query + filters the results are for
        int genre = -1, course = -1, level = 0, sel = -1;
        bool asked = false;
        std::vector<std::string> genres;
        std::vector<fs::path> hits;
        std::vector<std::pair<std::string, std::string>> lines; // title, genre + course
        size_t total = 0;
        // the mouse: where the last draw put the rows and the genre / course / level chips (virtual px)
        std::vector<Rect> row_rects;
        Rect chips[3] = {};
        Vec2 last_mouse = {-1, -1};
    } zdon_search;
    void zdon_search_step_filter(int which, int dir);
    void zdon_search_refresh();
    bool zdon_search_input();
    void zdon_draw_search();

    virtual void select_song(SongBox* song);
    // Z-DON: when the difficulty timer runs out on back / options / neiro, the player gets the song's lowest course;
    // false if the song has none
    static bool pick_course_on_timeout(SongSelectPlayer& p);

    virtual void handle_input(double current_ms);

    virtual void handle_input_browsing(double current_ms);
    virtual void handle_input_selecting();
    virtual void handle_input_diff_sorting();
    virtual void handle_input_search();

    void poll_song_jump(double current_ms);
    double last_song_jump_poll_ms = -1e9;

    std::optional<Screens> poll_replay_jump(double current_ms);
    double last_replay_jump_poll_ms = -1e9;
    std::optional<Screens> start_replay(const ReplayData& replay);

    virtual bool allows_second_player_join() { return true; }
    double join_request_ms = -1.0;
    PlayerNum join_existing_seat = PlayerNum::P1;
    bool awaiting_join_don_release = false;
    std::optional<Screens> poll_second_player_join(double current_ms);

    virtual void draw_overlays();
    virtual void draw_extra_players() {} // Z-DON: after the player (AiSongSelectScreen: the AI on the 2P side)

    virtual bool hides_dan() { return tex.get_skin_config(SC::ENTRY_DAN) != nullptr; }
    virtual bool is_2p_screen() { return false; }
    virtual Screens get_game_screen_target() { return Screens::GAME; }

public:
    SongSelectScreen() : Screen("song_select") {}

    explicit SongSelectScreen(const std::string& name) : Screen(name) {}

    ~SongSelectScreen() override { stats_stop.request_stop(); }

    void on_screen_start() override;
    Screens on_screen_end(Screens next_screen) override;
    // Practice replaces the select countdowns with its session timer
    virtual bool uses_session_timer() const { return false; }
    std::optional<Screens> update() override;
    void draw() override;
};
