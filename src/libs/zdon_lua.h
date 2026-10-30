#pragma once

#include <optional>
#include <sol/sol.hpp>

// Z-DON: extra `tex.*` functions for skin Lua (play timing, song progress, best score). Skins must
// check that a function exists before calling it: upstream YataiDON does not have them.
void register_zdon_lua_bindings(sol::table& tex);

// Z-DON: the audio_offset change the finished song's hits suggest (the median offset, + = the player was
// late, and a higher audio_offset counts every hit earlier): set when >= 50 hits were recorded and the median
// is >= 3 ms off; nullopt otherwise or with [general] zdon_hit_timing off
std::optional<int> zdon_offset_suggestion(int player_num);
// applies it to [general] audio_offset and saves config.toml once per song; returns the change applied
int zdon_apply_offset_suggestion(int player_num);
