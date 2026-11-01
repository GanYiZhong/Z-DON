#pragma once

#include <optional>
#include <sol/sol.hpp>

// Z-DON: extra `tex.*` functions for skin Lua (play timing, song progress, best score). Skins must
// check that a function exists before calling it: upstream YataiDON does not have them.
void register_zdon_lua_bindings(sol::table& tex);
