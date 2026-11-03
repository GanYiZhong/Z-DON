#pragma once
// Z-DON AI battle: the 2P lane is played by an AI opponent; the song is split into sections and each
// section goes to whoever scores more in it (good 2, ok 1). State lives in global_data.zdon_ai.
//
// The opponent's model (its own numbers, after the arcade AI battle's design):
//   - every note gets a hit time drawn from a normal distribution around the note; a draw outside the bad
//     window, or a per-note slip, is a miss
//   - the spread starts from the level (1: 34 ms .. 10: 16 ms) and moves toward the player's own recent
//     timing as the player hits notes (the AI plays "like you", up to 70% of the way)
//   - inside a section the AI loosens up while it leads and tightens while it trails, so sections stay close
//   - rolls and balloons are hit at a steady 11..20 hits/s by level
#include <random>
#include <vector>

class Player;
struct PlayerData;

struct ZdonAiDriver {
    int level = 5;
    std::mt19937 rng;
    int planned_index[2] = {-1, -1};   // the note (don, ka lane front) whose hit time is drawn
    double planned_offset[2] = {0, 0}; // ms from the note; NaN = this note is missed
    double next_roll_ms = -1e18;

    explicit ZdonAiDriver(int level_, unsigned seed) : level(level_), rng(seed) {}
    double draw_offset(double bad_window_ms); // NaN = miss
};

// the AI's nameplate / character ("AI", "Lv.N", a metal Don's colors); its nameplate is built with
// PlayerNum::AI (skins draw their AI plate: global/nameplate/ai)
PlayerData zdon_ai_player_data(int level);

// the skin draws the AI battle's own stage (skin_config "zdon_ai_arena"): the AI song select leaves out the AI's
// nameplate and character, and an AI battle's game leaves out both players' characters and the AI lane's nameplate
// and course mark (the skin draws the arena, the AI and its plate)
bool zdon_ai_skin_arena();
bool zdon_ai_arena_game();

// a new battle (the player's learned timing is kept)
void zdon_ai_battle_begin(int level);
// per frame, after the players updated: on the song's first frame cuts the sections (equal note counts;
// 3 sections, 5 from 200 notes), then closes sections and updates the lead state
void zdon_ai_battle_update(double chart_ms, const Player& player, const Player& ai);
