#pragma once

#include <future>
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
    std::future<bool> song_download;
    std::shared_ptr<const ReplayData> pending_replay;

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
    void start_song_download(const fs::path& song);
    bool song_download_pending() const;
    virtual void on_select_timeout();
    void on_diff_select_timeout();
    virtual SongSelectPlayer* second_player() { return nullptr; }
    virtual void release_players() { player.reset(); }

    virtual void handle_input(double current_ms);
    void handle_touch();
    // Steps the wheel once per box pitch in `travel`, leaving the remainder in it.
    void step_wheel(float& travel);
    void fling_wheel(double now);
    std::optional<float> drag_start_x; // browsing drag: canvas position the wheel last stepped at
    float drag_last = 0.0f;            // last finger position along the wheel, for its speed
    double drag_last_ms = 0.0;
    float drag_velocity = 0.0f;  // px per ms
    float fling_velocity = 0.0f; // wheel momentum after a flick, px per ms
    float fling_travel = 0.0f;
    double fling_last_ms = 0.0;

    virtual void handle_input_browsing(double current_ms);
    virtual void handle_input_selecting();
    virtual void handle_input_diff_sorting();
    virtual void handle_input_search();

    // Polls /poll_jump; song jumps are applied here, replay jumps are left for take_replay_jump()
    void poll_song_jump(double current_ms);
    double last_jump_poll_ms = -1e9;

    std::optional<Screens> take_replay_jump();
    std::optional<Screens> start_replay(const ReplayData& replay);
    // Z-DON online challenge (zdon_online.h): play this record's chart against it, the record in the 2P lane
    std::optional<Screens> start_challenge(const ReplayData& replay);
    bool replay_is_challenge = false;      // the replay being fetched is for a challenge (the website's button)
    bool pending_replay_challenge = false; // merge 4917fdc8: pending_replay (waiting for its song download) is one

    virtual bool allows_second_player_join() { return true; }
    double join_request_ms = -1.0;
    PlayerNum join_existing_seat = PlayerNum::P1;
    bool awaiting_join_don_release = false;
    std::optional<Screens> poll_second_player_join(double current_ms);

    virtual void draw_overlays();
    virtual void draw_extra_players() {} // Z-DON: after the player (ZdonAiSongSelectScreen: the AI on the 2P side)

    // Z-DON: the course select's timer (seconds), the cue when a timer runs out, backing out of the course select
    virtual int diff_select_seconds() { return 60; }
    virtual void on_select_time_up() {}
    virtual void on_diff_select_cancelled(double /*current_ms*/) {}
    // a time-up on the course select with the option / neiro panel open: close it and give this many seconds more
    // (once; 0 = pick the course right away)
    virtual int options_time_up_extra_seconds() { return 0; }
    // runs right after the select timers (a state set here is seen by this frame's state change)
    virtual void after_select_timers(double /*current_ms*/) {}
    void arm_diff_select_timer(int seconds, double current_ms);
    bool diff_timer_extend_pending = false;
    bool diff_timer_extended = false;
    virtual bool popup_open() { return player->neiro_selector.has_value() || player->modifier_selector.has_value(); }

    virtual bool hides_dan() { return tex.options[SCO::DISABLE_DAN] || tex.get_skin_config(SC::ENTRY_DAN) != nullptr; }
    virtual bool shows_ai_assigned() { return false; }
    virtual bool is_2p_screen() { return false; }
    virtual bool mutes_enter_voice() { return false; } // the AI select (skin zdon_ai_no_enter_voice)
    virtual bool is_waiwai_screen() { return false; }
    virtual fs::path bgm_override() { return {}; }
    fs::path bgm_dir;
    bool bgm_is_box = false;
    void update_box_bgm();
    virtual Screens get_game_screen_target() { return Screens::GAME; }

public:
    TouchInput touch_input() const override { return TouchInput::SCREEN; }
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
