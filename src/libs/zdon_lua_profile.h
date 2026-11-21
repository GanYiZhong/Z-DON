#pragma once

struct lua_State;

// Z-DON dev hook: ZDON_LUA_PROFILE=1 samples the skin's Lua (every 1000 VM instructions: the running line) and counts
// calls into C functions (engine bindings) by name; every 10 s it logs the top lines, the top C functions and the
// calls per second. Off (no hook installed) unless the variable is set.
namespace zdon_lua_profile {
    void install(lua_State* L);
}
