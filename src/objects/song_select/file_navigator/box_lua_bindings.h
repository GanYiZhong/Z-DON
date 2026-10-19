#pragma once

#include <sol/sol.hpp>

void register_song_select_lua_bindings(sol::state& lua);

// the parts of it, in box_lua_bindings*.cpp
void register_box_lua_bindings(sol::state& lua);
void register_dan_box_lua_bindings(sol::state& lua);
void register_player_lua_bindings(sol::state& lua);
