#include "zdon_lua_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace zdon_lua_profile {
    namespace {
        std::unordered_map<std::string, long> lines;   // "source:line" of the running Lua code, per sample
        std::unordered_map<std::string, long> c_calls; // C function name -> calls
        std::unordered_map<std::string, long> l_calls; // Lua function "source:linedefined" -> calls
        long samples = 0;
        auto since = std::chrono::steady_clock::now();

        template<class M>
        std::string top(const M& m, size_t n, double scale, const char* unit) {
            std::vector<std::pair<std::string, long>> v(m.begin(), m.end());
            std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string out;
            for (size_t i = 0; i < v.size() && i < n; ++i) {
                out += std::format("\n    {:9.1f} {}  {}", static_cast<double>(v[i].second) * scale, unit, v[i].first);
            }
            return out;
        }

        void dump_if_due(lua_State* L) {
            const auto now = std::chrono::steady_clock::now();
            const double s = std::chrono::duration<double>(now - since).count();
            if (s < 10.0) {
                return;
            }
            long cc = 0;
            for (const auto& [k, v] : c_calls) {
                cc += v;
            }
            long lc = 0;
            for (const auto& [k, v] : l_calls) {
                lc += v;
            }
            spdlog::info("[luaprof] {:.1f} s: {} samples, {:.0f} C calls/s, {:.0f} Lua calls/s, Lua heap {} KB\n  "
                         "lines (samples):{}\n"
                         "  C functions (calls/s):{}\n  Lua functions (calls/s):{}",
                         s, samples, cc / s, lc / s, lua_gc(L, LUA_GCCOUNT, 0), top(lines, 25, 1.0, "smp"),
                         top(c_calls, 25, 1.0 / s, "/s"), top(l_calls, 25, 1.0 / s, "/s"));
            lines.clear();
            c_calls.clear();
            l_calls.clear();
            samples = 0;
            since = now;
        }

        void hook(lua_State* L, lua_Debug* ar) {
            if (ar->event == LUA_HOOKCOUNT) {
                lua_Debug d;
                if (lua_getstack(L, 0, &d) && lua_getinfo(L, "Sl", &d)) {
                    ++lines[std::string(d.short_src) + ":" + std::to_string(d.currentline)];
                    ++samples;
                }
                dump_if_due(L);
                return;
            }
            if (ar->event == LUA_HOOKCALL || ar->event == LUA_HOOKTAILCALL) {
                if (!lua_getinfo(L, "Sn", ar)) {
                    return;
                }
                if (ar->what && ar->what[0] == 'C') {
                    ++c_calls[ar->name ? ar->name : "?"];
                } else {
                    ++l_calls[std::string(ar->short_src) + ":" + std::to_string(ar->linedefined) + " " +
                              (ar->name ? ar->name : "?")];
                }
            }
        }
    } // namespace

    void install(lua_State* L) {
        const char* v = std::getenv("ZDON_LUA_PROFILE");
        if (!v || *v == '\0' || *v == '0') {
            return;
        }
        lua_sethook(L, hook, LUA_MASKCALL | LUA_MASKCOUNT, 1000);
        spdlog::info("[luaprof] Lua profiling on (every 1000 instructions, all calls)");
    }
} // namespace zdon_lua_profile
