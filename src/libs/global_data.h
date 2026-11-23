#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "file/config.h"
#include "gfx.h" // IWYU pragma: keep
#include "gfx/color_utils.h"
#include "session_types.h" // IWYU pragma: export

namespace fs = std::filesystem;

namespace ScoreMethod {
    const std::string GEN3 = "gen3";
    const std::string SHINUCHI = "shinuchi";
} // namespace ScoreMethod

enum class PlayerNum {
    ALL = 0,
    P1 = 1,
    P2 = 2,
    TWO_PLAYER = 3,
    DAN = 4,
    AI = 5
};

enum class Difficulty {
    BACK = -3,
    MODIFIER = -2,
    NEIRO = -1,
    EASY = 0,
    NORMAL = 1,
    HARD = 2,
    ONI = 3,
    URA = 4,
    TOWER = 5,
    DAN = 6
};

enum class Crown {
    NONE = 0,
    CLEAR = 1,
    FC = 2,
    DFC = 3
};

// underscore to avoid conflict with raylib colors
enum class Rank {
    _NONE = 0,
    _WHITE = 1,
    _BRONZE = 2,
    _SILVER = 3,
    _GOLD = 4,
    _PINK = 5,
    _PURPLE = 6,
    _RAINBOW = 7
};

struct ReplayData;

// Z-DON: live play data for skin Lua (src/libs/zdon_lua.cpp), written by Player / GameScreen
struct ZdonLiveData {
    double last_hit_offset_ms = 0.0;                    // the latest judged hit (+ = late)
    double last_hit_at_ms = -1.0;                       // get_current_ms() of that hit; -1 = none yet
    double song_end_ms = 0.0;                           // the last playable note's time on the chart clock
    std::vector<std::pair<double, double>> gogo_ranges; // GOGO sections, chart ms (start, end)
    // the last judged hits, oldest overwritten: (offset ms, get_current_ms() of the hit), for the hit error bar
    std::array<std::pair<float, double>, 24> recent_hits{};
    int recent_hits_next = 0;
    int recent_hits_count = 0;
};

struct SessionData {
    fs::path selected_song;
    std::string selected_collection; // the song select folder it was picked from, sent with the score
    std::string song_hash;
    int selected_difficulty = 0;
    int subdiff = 0;
    std::string song_title = "default_title";
    std::string song_subtitle = "default_subtitle";
    bool song_subtitle_full_display = false;
    int genre_index = 0;
    std::string genre_label;
    fs::path selected_dan_folder;
    fs::path selected_tutorial;
    DanCourse dan;
    bool ai_assigned = false;
    std::shared_ptr<const ReplayData> replay; // set when song select starts a downloaded replay
    ResultData result_data;
    ZdonLiveData zdon_live;
    // Z-DON: why the last song's score was not saved (GameScreen::zdon_unsaved_reasons: "auto", "replay",
    // "equal_speed"); empty = saved. The result screens show it.
    std::vector<std::string> zdon_unsaved;
    DanResultData dan_result_data;
};

struct CameraConfig {
    Vec2 offset = {0.0f, 0.0f};
    float zoom = 1.0f;
    float h_scale = 1.0f;
    float v_scale = 1.0f;
    float rotation = 0.0f;
    Color border_color = Color::BLACK;
};

class PracticeTimer;

struct CardSession {
    std::string reader_hex; // 16-char hex card ID
    bool reader_valid = false;
    std::string prev_code[2];
    bool overridden[2] = {false, false};
};

struct EntryResume {
    bool join_pending = false;
    PlayerNum joined_seat = PlayerNum::P1;
    bool campaign_resume = false;
    PlayerNum resume_seats[2] = {PlayerNum::ALL, PlayerNum::ALL};
};

struct CampaignSession {
    std::string name;
    int next = 0;
    int kinotake_team[2] = {-1, -1};
    int kinotake_next = 0;
};

struct CreditSession {
    int songs_played = 0;
    int songs[2] = {0, 0};               // scored songs this credit, per seat
    bool seats[2] = {false, false};      // seats that played the credit that just ended
    bool guest = false;                  // every seat is a guest (set leaving entry): game over shows the ad
    bool reward_shop_from_entry = false; // mode select opened the shop: no reward time, back to entry
    int bncoin = 99999;
};

struct GlobalData {
    Config* config = nullptr;
    PlayerNum player_num = PlayerNum::P1;
    PlayerNum first_login_player = PlayerNum::P1;
    std::array<SessionData, 3> session_data;
    std::array<int, 3> last_difficulty = {-1, -1, -1};
    std::array<fs::path, 3> dan_folder;
    CameraConfig camera;

    std::string current_screen = "LOADING";
    std::string previous_screen;
    bool in_transition = false;
    // Z-DON
    double zdon_song_pos_ms = -1.0; // Z-DON: the playing song's chart clock (GameScreen::update); -1 = none
    // Z-DON: practice section loop (game_practice.cpp): resumed at a jump mark with a later mark, the play
    // jumps back from the later mark to the first; `last_*` are the judgements of the last finished pass
    struct ZdonPracticeLoop {
        bool active = false;
        int a_bar = -1, b_bar = -1; // bar indices (bars[]), A <= play < B
        bool to_end = false;        // no later mark: A .. the song's end (b_bar = the last bar)
        int passes = 0;
        bool has_last = false;
        int last_good = 0, last_ok = 0, last_bad = 0;
        // the last passes' lines (a ring, formatted at each pass's end) and the best pass (good 2, ok 1)
        static constexpr int HISTORY = 5;
        std::array<std::string, HISTORY> history{};
        int history_head = 0;
        int history_count = 0;
        int best_score = -1;
        std::string best_line;
    } zdon_loop;
    // Z-DON AI battle (objects/game/zdon_ai.h): the 2P lane is played by an AI; the song is cut into sections,
    // each won by the higher section score (good 2, ok 1), the match by the most sections
    struct ZdonAiBattle {
        bool requested = false;     // the next GAME_2P starts as an AI battle (song select / --ai)
        bool active = false;        // GAME_2P / RESULT_2P of an AI battle; cleared by the song select
        double announce_ms = -1.0;  // skin_config zdon_ai_announce_at_fade: when the result announce already played
        int level = 5;              // tuning level 1..11 (the course's star level)
        int condition = 3;          // 1 worst .. 5 best (the session's, zdon_ai_condition)
        int state = 0;              // 0 neutral, 1 "superior" (the player leads), 2 "inferior" (the player trails)
        int tug = 9;                // clamp(player - AI + 9, 1, 17) of the open section
        std::vector<double> bounds; // each section's end, chart ms
        std::vector<int> points_p, points_ai, winners; // per section; winners: -1 open, 0 player, 1 AI
        std::vector<int> crowns;                       // per section: the player's crown 0 none .. 3 donderful
        std::vector<std::array<int, 4>> section_judge; // per section: the player's good, ok, bad, drumroll
        std::vector<int> section_score;                // per section: the player's score in it
        // the best play's record per section (score, good, ok, bad, drumroll) from before this play, empty: none;
        // new_best: this play beat that play's score (it is the record now)
        std::vector<std::array<int, 5>> section_best;
        bool new_best = false;
        bool detail = false;   // the result shows the per-section detail view (kat toggles it)
        bool play_all = false; // the credit's last result shows the play-all summary (AiResultPlayAllMain)
        double star = 0;       // the course's star level (the Green rank's per-course minimums)
        // the Green ghost rank (zdon_ai_rank_*): before and after this play, the points it gave
        int rank_before = 0, rank = 0, points_before = 0, points = 0, rank_gain = 0;
        int base_score = 0; // the player's score at the section's start
        int section = 0;
        int base_p = 0, base_ai = 0;              // the score totals at the current section's start
        std::array<int, 4> base_judge{};          // the player's good / ok / bad / drumroll at the section's start
        int live_p = 0, live_ai = 0;              // the current section's points so far
        std::array<int, 4> judge_p{}, judge_ai{}; // good, ok, bad, drumroll hits so far (score boards)
        bool finished = false;
        bool skipped = false; // the player used the skip: no end cut-in, straight to the result
        double finished_at_ms =
            0;            // wall clock (get_current_ms) when finished turned true: the skin's cut-in starts then
        bool won = false; // finished: the player won the match
        std::vector<float> player_offsets; // the player's recent hit offsets (the AI's "trace")
        // the skin's arena asks for the player's Don this frame (tex.ai_draw_don): where, how big, turned how far
        // (degrees about the vertical axis). The player's lane draws it, then clears don_show
        bool don_show = false;
        float don_x = 0, don_y = 0, don_scale = 1, don_rot = 0;
        std::function<void()> don_draw_now; // screen option ai_don_under_flames: draws it at the skin's call
        std::function<void(float, float, float, float)> ai_draw_now; // ai_game_chara_3d: x, y, scale, rot_y of the AI
        bool hold_ready_cue = false; // screen option ai_wait_loading_card: the loading card is still up
    } zdon_ai;
    int zdon_ai_condition = 0; // the session's AI condition 1..5; 0 = roll it at the next AI song select
    // the AI battles of this credit (the play-all summary): title, the player's score / judges / cleared, won,
    // per section score and crown
    struct ZdonAiSong {
        std::string title;
        int score = 0, good = 0, ok = 0, bad = 0;
        bool cleared = false, won = false;
        std::vector<int> section_score, section_crown;
    };
    std::vector<ZdonAiSong> zdon_ai_credit;
    fs::path cli_dan; // `--dan` (test): the course dan select starts as soon as its scan is done
    bool skinless = false;
    bool skin_reload_pending = false;
    bool needs_onboarding = false;
    bool returned_from_result = false;
    bool force_auto_play = false;
    bool hold_genre_voice = false; // a song select story/intro is running: folder voices wait for it
    int live_skip_count = -1;

    std::shared_ptr<PracticeTimer> practice_timer; // session countdown, spans practice select + game
    std::vector<int> battle_skills;                // the skill picker's slots; empty until the first battle select

    CardSession card;
    EntryResume entry;
    CampaignSession campaign;
    CreditSession credit;
};

void reset_session();
std::string get_player_id(PlayerNum player_num);
// Guest seats (network.guest_mode, no card): a temporary id per login slot that scores.db never stores
std::string guest_id(int slot);
bool is_guest_id(const std::string& player_id);
// every seated player is a guest: slot 1, and slot 2 too when two players are in
bool guests_only(bool two_player);
void load_skin();
void unload_skin();

void set_user_appearance(const std::string& skin, const std::string& language);
void clear_user_appearance();
void apply_user_appearance();
void revert_user_appearance();
std::string instance_skin();
std::string instance_language();

void queue_window_resize(int width, int height);
bool apply_queued_window_resize();

void drop_other_screens_for_skin_reload();
void reload_skin_screens();
// Rebuilds the current screen against a fresh Lua state, so script edits on disk take effect.
void reload_current_screen_scripts();

extern GlobalData global_data;
