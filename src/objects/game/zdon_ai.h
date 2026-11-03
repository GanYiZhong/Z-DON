#pragma once
// Z-DON AI battle: the 2P lane is played by an AI opponent; the song is split into sections and each section goes
// to whoever scores more in it. State lives in global_data.zdon_ai. The rules follow the arcade's AI battle
// (CHN05 AiEnsoGameManager / AiEnsoInput and its ai_tuninginfo table):
//   - the AI's strength comes from the course's star level (tuning levels 1..10, 11 above 10) and its condition
//     (1 worst .. 5 best, rolled once per session and shown as its tension): each note's hit time is drawn from a
//     normal distribution whose variance goes from the level's "worst" to its "best" with the condition; a draw
//     outside the bad window is a miss
//   - a state machine on the open section's point difference (AI - player) moves the AI between neutral,
//     "superior" (the player leads: the AI tightens) and "inferior" (the player trails: the AI loosens); each state
//     scales the variance, and at some levels the AI copies the player's own recent timing ("trace")
//   - section points: good / ok / bad score the level's ryo / ka / fuka (5/3/0 .. 2/1/0); rolls score nothing
//   - the tug value clamp(player - AI + 9, 1, 17) decides a section when it closes: below 9 the AI wins it, else
//     the player (a tie goes to the player); the player wins the match with at least as many sections as the AI
//   - each section also gets a crown from the player's judges in it (donderful / full combo / clear at > 70%)
//   - rolls and balloons are hit at a steady 11..21 hits/s by level
#include <array>
#include <random>
#include <vector>

class Player;
struct PlayerData;

struct ZdonAiDriver {
    int level = 5; // tuning level 1..11
    std::mt19937 rng;
    int planned_index[2] = {-1, -1};   // the note (don, ka lane front) whose hit time is drawn
    double planned_offset[2] = {0, 0}; // ms from the note; NaN = this note is missed
    double next_roll_ms = -1e18;

    explicit ZdonAiDriver(int level_, unsigned seed) : level(level_), rng(seed) {}
    double draw_offset(double bad_window_ms); // NaN = miss
};

// the arcade's AI tuning row (ai_tuninginfo) for a tuning level 1..11
struct ZdonAiTuning {
    int variance_worst, variance_best;            // ms^2 of the hit spread at condition 1 / 5
    std::array<int, 3> trace_prob, variance_coef; // per state (neutral, superior, inferior): %, % of the variance
    int state_n_s, state_n_i, state_s_n, state_i_n;
    int samples;                           // the player's recent offsets kept for tracing
    int ryo, ka, fuka;                     // section points of good / ok / bad
    int emotion_low = 2, emotion_high = 6; // the AI calms down / gets angry at this lead of the player's (Green)
};
const ZdonAiTuning& zdon_ai_tuning(int level);
// the tuning level of a course's star level (1..10; 11 above 10)
int zdon_ai_tuning_level(double star_level);

// the AI's condition for this session (1..5), rolled on the first call after zdon_ai_new_session()
int zdon_ai_condition();
void zdon_ai_new_session();

// the rank bonuses the last finished battle got: its first win on the chart (winpoint_bonus_firstclear), the course
// at or above the rank's bonus stars (winpoint_bonus_highlevel); the result's "この曲で初バトル勝利" /
// "条件の難易度をこえて勝利" notes
bool zdon_ai_last_first_win();
// a skin's result cut-in pages: tex.ai_pages(open) holds them open each frame; the result screen turns a kat into
// the next page and a don into the end (and holds its own input / timeout meanwhile)
// the result's players have counted up (Result*Screen::reveal_end_ms passed); the AI cut-ins wait for it
void zdon_ai_set_revealed(bool revealed);
bool zdon_ai_revealed();
// the game's song title visibility this frame (SongInfo: the title / song number swap), for skins that draw next
// to the title (Green's genre pill)
void zdon_ai_set_title_alpha(float a);
float zdon_ai_title_alpha();
bool zdon_ai_pages_open();
void zdon_ai_pages_set(bool open);
int zdon_ai_page_steps();
bool zdon_ai_pages_finished();
void zdon_ai_page_next();
void zdon_ai_pages_finish();
bool zdon_ai_last_bonus_high();

// wins still needed for the next AI battle reward (the arcade's win-count rewards); 0 past the last one
int zdon_ai_next_reward(int wins);

// the Green (System 357) ghost rank, ghostinfo.xml: rank k needs `acquired` points; at rank k a win on course c
// (easy, normal, hard, oni, ura) counts when its star level is at least row k+1's least[c] (0: the course no
// longer counts; ai_rankup_todo shows that row), +1 point (winpoint_bonus_highlevel) from its bonus[c] stars,
// +1 on a chart's first win
struct ZdonAiRank {
    int acquired;
    std::array<int, 5> least, bonus;
};
inline constexpr int kZdonAiRanks = 12;
const ZdonAiRank& zdon_ai_rank_info(int rank);
int zdon_ai_rank_of(int points);

// the AI's nameplate / character ("AI" and a metal Don's colors); its nameplate is built with
// PlayerNum::AI (skins draw their AI plate: global/nameplate/ai)
PlayerData zdon_ai_player_data(int level);

// the skin draws the AI battle's own stage (skin_config "zdon_ai_arena"): the AI song select leaves out the AI's
// nameplate and character, and an AI battle's game leaves out both players' characters and the AI lane's nameplate
// and course mark (the skin draws the arena, the AI and its plate)
bool zdon_ai_skin_arena();
bool zdon_ai_arena_game();
// GAME_2P of an AI battle in a skin with "zdon_ai_lane" (the AI lane's own cover / background, no badges)
bool zdon_ai_lane_game();

// a new battle at a tuning level (the player's recent timing is kept)
void zdon_ai_battle_begin(int level);
// per frame, after the players updated: on the song's first frame cuts the sections (3, 5 from 200 notes; the cuts
// fall on bar lines), then closes sections, keeps the tug and the AI's state, and at the end records the result
void zdon_ai_battle_update(double chart_ms, const Player& player, const Player& ai);
