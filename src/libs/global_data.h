#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "file/config.h"
#include "gfx.h" // IWYU pragma: keep
#include "gfx/color_utils.h"

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

struct Exam {
    std::string type; // "gauge","combo","judgebad","judgegood","judgeperfect","hit","score"
    int red = 0;
    int gold = 0;
    std::string range; // "less" or "more"
    bool gothrough = true;
    std::vector<int> song_red;
    std::vector<int> song_gold;
    static constexpr int GOLD_FULL = -1;
    bool per_song() const { return !song_red.empty(); }
    // The exam as it applies to song i: red/gold swapped for that song's pair.
    Exam for_song(int i) const {
        assert(!per_song() || (i >= 0 && i < std::ssize(song_red)));
        Exam ex = *this;
        if (per_song() && i >= 0 && i < std::ssize(song_red)) {
            ex.red = song_red[i];
            ex.gold = i < std::ssize(song_gold) ? song_gold[i] : song_red[i];
        }
        // the view of one song is a plain pair again (captions print a single number)
        ex.song_red.clear();
        ex.song_gold.clear();
        return ex;
    }
};

inline std::string_view dan_bar_state(const Exam& exam, int value, bool live = false, bool near_end = false,
                                      bool just_before_end = false) {
    const bool down = (exam.range == "less");
    // CheckMax
    if (exam.gold > 0 && ((down && value < exam.gold) || (!down && value >= exam.gold))) {
        if (!live || !down) {
            return "max";
        }
        if (just_before_end) {
            return "max_soon2";
        }
        if (near_end) {
            return "max_soon";
        }
        // fall through to the flat palette -- no `max` during play
    }
    int gauge = (exam.red > 0) ? static_cast<int>(std::floor(100.0 * value / exam.red)) : 100;
    gauge = std::clamp(gauge, 0, 100);
    if (down) {
        gauge = 100 - gauge;
    }
    if (gauge <= 0) {
        return "empty";
    }
    if (!down) {
        if (gauge <= 49) {
            return "up_50";
        }
        if (gauge <= 99) {
            return "up_80";
        }
        return "up_100";
    }
    const int good =
        (exam.red > 0 && exam.gold > 0) ? 100 - static_cast<int>(std::floor(100.0 * exam.gold / exam.red)) : 100;
    if (gauge < 30) {
        return "down_80";
    }
    if (gauge <= good) {
        return "up_80";
    }
    return "down_100";
}

struct DanSongEntry {
    fs::path song_path;
    int genre_index = 0;
    int difficulty = 0;
    int level = 0;
    bool hidden = false;
};

struct DanResultSong {
    int selected_difficulty = 0;
    int diff_level = 0;
    std::string song_title = "default_title";
    int genre_index = 0;
    int good = 0;
    int ok = 0;
    int bad = 0;
    int drumroll = 0;
    bool hidden = false;
    bool unreached = false;
    int64_t score_id = -1;
};

struct DanResultExam {
    float progress = 0.0f;
    int counter_value = 0;             // the counter as shown (for "less" exams: what is left)
    int value = -1;                    // the measured value (count, percent, ...), -1 = not measured
    int song_actual[3] = {-1, -1, -1}; // per-song exams: each song's measured value
    std::string bar_texture = "exam_red";
    bool failed = false;
    int song_value[3] = {0, 0, 0};
    float song_progress[3] = {0.0f, 0.0f, 0.0f};
    int song_count = 0;
    int tier = 0;
    std::string bar_state = "empty";
    std::string song_state[3] = {"empty", "empty", "empty"};
};

struct DanResultData {
    int dan_color = 0;
    int dan_rank = -1;
    int dan_index = -1;
    int dan_index_max = -1;
    bool is_gaiden = false;
    bool is_modern = true; // older versions are recorded but never rank up the profile or grant the costume
    std::string dan_title = "default_title";
    int score = 0;
    float gauge_length = 0.0f;
    int max_combo = 0;
    std::vector<DanResultSong> songs;
    std::vector<Exam> exams;
    std::vector<DanResultExam> exam_data;
    int odai_result = -1;
    bool skipped = false;  // the player skipped out: nothing is recorded
    bool unranked = false; // auto play or a replay: shown, but not recorded (as for single songs)
};

struct ResultData {
    int score = 0;
    int good = 0;
    int ok = 0;
    int bad = 0;
    int max_combo = 0;
    int total_drumroll = 0;
    float gauge_length = 0.0f;
    bool cleared = true;
    int prev_score = 0;
    bool submitted = false;
    double hit_offset_sum_ms = 0.0;
    int hit_offset_count = 0;
    std::vector<float> hit_offsets; // Z-DON: every judged hit's offset in ms (+ = late), in hit order
};

// Z-DON: live play data for skin Lua (src/libs/zdon_lua.cpp), written by Player / GameScreen
struct ZdonLiveData {
    double last_hit_offset_ms = 0.0;                    // the latest judged hit (+ = late)
    double last_hit_at_ms = -1.0;                       // get_current_ms() of that hit; -1 = none yet
    double song_end_ms = 0.0;                           // the last playable note's time on the chart clock
    std::vector<std::pair<double, double>> gogo_ranges; // GOGO sections, chart ms (start, end)
};

struct AISectionTally {
    int good = 0;
    int ok = 0;
    int bad = 0;
    int drumroll = 0;
    bool won = false;
};

struct AIMatchResult {
    std::vector<AISectionTally> sections;
    bool won = false;
    bool saves = false;
    int stars = 1;
};

// one battle mode song, for its result screen
struct BattleTally {
    bool played = false;
    int stage = 1;
    int difficulty = 0;
    bool boss = false;
    int boss_hp = 0; // before the song
    int boss_hp_max = 0;
    int kills = 0;
    int damage = 0;
    int power = 0;
    int chests = 0;
    int orb_chests = 0;
};

struct SessionData {
    fs::path selected_song;
    std::string selected_collection; // the song select folder it was picked from, sent with the score
    std::string song_hash;
    fs::path selected_dan_folder;
    fs::path selected_tutorial;
    std::vector<DanSongEntry> selected_dan;
    std::vector<Exam> selected_dan_exam;
    int dan_color = 0;
    int dan_rank = -1;
    int dan_index = -1;
    int dan_index_max = -1;
    bool dan_gaiden = false;
    bool dan_modern = true;
    bool dan_gen3 = false;
    int selected_difficulty = 0;
    int subdiff = 0;
    int ai_sections_won = 0;
    int ai_sections_total = 0;
    bool ai_assigned = false;
    std::string song_title = "default_title";
    std::string song_subtitle = "default_subtitle";
    bool song_subtitle_full_display = false;
    int genre_index = 0;
    std::string genre_label;
    ResultData result_data;
    ZdonLiveData zdon_live;
    // Z-DON: why the last song's score was not saved (GameScreen::zdon_unsaved_reasons: "auto", "replay",
    // "equal_speed"); empty = saved. The result screens show it.
    std::vector<std::string> zdon_unsaved;
    DanResultData dan_result_data;
    std::optional<std::vector<std::pair<double, int>>> replay_input_log;
    std::string replay_username;
    std::string replay_title;
    int replay_title_bg = 0;
    Color replay_chara_color_1 = Color{0x68, 0xBF, 0xC0, 0xFF};
    Color replay_chara_color_2 = Color{0xF9, 0x47, 0x28, 0xFF};
    Color replay_chara_color_3 = Color{0xF9, 0xF0, 0xE1, 0xFF};
    int replay_chara_head_index = 0;
    int replay_chara_body_index = 0;
    int replay_chara_cos_index = 0;
    bool replay_chara_is_costume = true;
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

struct GlobalData {
    std::shared_ptr<PracticeTimer> practice_timer; // session countdown, spans practice select + game
    int songs_played = 0;
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
    double ai_condition = 0.95;
    std::string current_screen = "LOADING";
    std::string previous_screen;
    bool in_transition = false;
    std::string title_state;
    double title_state_start_ms = 0.0;
    int live_skip_count = -1;
    bool live_skip_used = false;
    bool force_auto_play = false;
    fs::path cli_dan; // `--dan` (test): the course dan select starts as soon as its scan is done
    bool skinless = false;
    bool skin_reload_pending = false;
    bool needs_onboarding = false;
    bool returned_from_result = false;
    bool entry_join_pending = false;
    PlayerNum entry_joined_seat = PlayerNum::P1;
    std::string campaign;
    int bncoin = 99999;
    int campaign_next = 0;
    bool entry_campaign_resume = false;
    PlayerNum entry_resume_seats[2] = {PlayerNum::ALL, PlayerNum::ALL};
    int kinotake_team[2] = {-1, -1};
    int kinotake_next = 0;
    AIMatchResult ai_match;
    bool battle_tracked = false;
    int battle_bond = 1;
    std::string waiwai_collab = "taiko"; // the waiwai song's collab art set (Graphics/waiwai_game/tips/<collab>.png)
    int waiwai_rare_total = 0;           // rare notes per player this song
    unsigned waiwai_rare_hits = 0;       // bit n: either player hit their n-th rare note
    int battle_best_power = 0;
    int battle_stage = 1;
    int battle_boss_hp = 0; // the stage's boss, if it has one
    int battle_boss_hp_max = 0;
    int battle_moves = 1;           // special moves unlocked
    bool battle_hacchake = false;   // the Maou event's move, bought with orbs
    bool battle_orbs = false;       // white chests can drop
    std::vector<int> battle_skills; // the skill picker's slots; empty until the first battle select
    BattleTally battle_song;
    bool hold_genre_voice = false;             // a song select story/intro is running: folder voices wait for it
    std::vector<int> credit_gauges[2];         // soul gauge percent per scored song this credit, per seat
    std::vector<std::string> credit_titles[2]; // same songs
    bool reward_shop_from_entry = false;       // mode select opened the shop: no reward time, back to entry
    bool credit_seats[2] = {false, false};     // seats that played the credit that just ended
    bool guest_credit = false;                 // every seat is a guest (set leaving entry): game over shows the ad
    // Card reader
    std::string card_reader_card_id_hex; // 16-char hex card ID
    bool card_reader_card_valid = false;
    std::string card_prev_code[2];
    bool card_override[2] = {false, false};
    CameraConfig camera;
    Config* config = nullptr; // Using pointer, initialize appropriately
    int total_songs = 0;
    PlayerNum player_num = PlayerNum::P1;
    PlayerNum first_login_player = PlayerNum::P1;
    std::vector<SessionData> session_data = std::vector<SessionData>(3);
    std::vector<int> last_difficulty = std::vector<int>(3, -1);
    std::vector<fs::path> dan_folder = std::vector<fs::path>(3);
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
