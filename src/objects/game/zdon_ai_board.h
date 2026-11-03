#pragma once

#include <filesystem>
#include <sol/sol.hpp>
#include <string>
#include <vector>

// Z-DON AI select, Green's course-filtered song board (footage Tl7g8XXXvAM 1:52-2:12): after the course columns the
// cabinet shows the songs that have the chosen course at one star level (header "oni x10"). The screen builds the
// list from the navigator's level index; the skin draws it (tex.ai_board(), tex.ai_diff_info()).
struct AiBoardCourse {
    int course = 3; // 0 easy .. 3 oni, 4 ura
    int level = 0;  // the star level
    int count = 0;  // songs with this course at this level
    int clear = 0;  // of them: cleared by player 1
    int fc = 0;     // of them: full combo or better
};

// the course a column (1..4 easy..oni, 5 recommended, 6 last played) stands for, its star level (the one most songs
// have), the song count and player 1's crowns; `last_course` / `last_level` feed column 6 (-1 = none)
AiBoardCourse zdon_ai_board_course(int column, int last_course, int last_level);
// the songs of that course and level, in the index's order
std::vector<std::filesystem::path> zdon_ai_board_songs(const AiBoardCourse& c);

void zdon_ai_board_set(bool on, const AiBoardCourse& c, const std::vector<std::filesystem::path>& songs, int cursor);
bool zdon_ai_board_on();
// zdon_ai_p23: the chosen card is held for the skin transition; ms since it landed, < 0 = off
void zdon_ai_board_jump_set(double ms);
void zdon_ai_board_info_set(bool on, int column, const AiBoardCourse& c);

// tex.ai_board() -> nil | { course, level, count, clear, fc, cursor (1-based), total, items = { [-7..7] = {title, sub,
// crown, outline = {r, g, b} of the song genre, levels = {easy .. ura}} }, jump = ms (only while held) }
// tex.ai_diff_info() -> nil | { course, level, count, clear, fc } (the highlighted course column)
void zdon_ai_board_bind(sol::table& tex);
