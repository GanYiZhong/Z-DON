#include "zdon_lua.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../objects/enums.h"
#include "../objects/game/player.h"
#include "global_data.h"
#include "script.h"

namespace {
    bool valid_player(int player_num) {
        return player_num >= 0 && player_num < (int)global_data.session_data.size();
    }

    // the good / ok windows the player's course judges with (Player::check_note)
    std::pair<float, float> judge_windows(int player_num) {
        if (global_data.session_data[player_num].selected_difficulty <= (int)Difficulty::NORMAL) {
            return {Timing::GOOD_EASY, Timing::OK_EASY};
        }
        return {Timing::GOOD, Timing::OK};
    }
} // namespace

std::optional<int> zdon_offset_suggestion(int player_num) {
    if (!global_data.config->general.zdon_hit_timing || !valid_player(player_num)) {
        return std::nullopt;
    }
    std::vector<float> offs = global_data.session_data[player_num].result_data.hit_offsets;
    if (offs.size() < 50) {
        return std::nullopt;
    }
    auto mid = offs.begin() + (std::ptrdiff_t)(offs.size() / 2);
    std::nth_element(offs.begin(), mid, offs.end());
    const int delta = (int)std::lround(*mid);
    if (std::abs(delta) < 3) {
        return std::nullopt;
    }
    return delta;
}

int zdon_apply_offset_suggestion(int player_num) {
    if (!valid_player(player_num)) {
        return 0;
    }
    ZdonLiveData& live = global_data.session_data[player_num].zdon_live;
    if (live.offset_applied != 0) {
        return 0;
    }
    std::optional<int> delta = zdon_offset_suggestion(player_num);
    if (!delta) {
        return 0;
    }
    global_data.config->general.audio_offset += *delta;
    save_config(*global_data.config);
    live.offset_applied = *delta;
    return *delta;
}

void register_zdon_lua_bindings(sol::table& tex) {
    // tex.offset_suggestion(player_num) -> change_ms (nil: none), audio_offset now, applied change (0: not yet)
    tex.set_function("offset_suggestion", [](int player_num, sol::this_state s) -> sol::variadic_results {
        sol::variadic_results r;
        if (!valid_player(player_num)) {
            return r;
        }
        const int applied = global_data.session_data[player_num].zdon_live.offset_applied;
        std::optional<int> delta = zdon_offset_suggestion(player_num);
        if (!delta && applied == 0) {
            return r;
        }
        r.push_back({s, sol::in_place, (double)(delta ? *delta : applied)});
        r.push_back({s, sol::in_place, (double)global_data.config->general.audio_offset});
        r.push_back({s, sol::in_place, (double)applied});
        return r;
    });

    // tex.last_hit(player_num) -> offset_ms (+ = late), at_ms (get_current_ms of the hit), good_ms, ok_ms,
    // mark_ms (mark FAST / SLOW when |offset| > mark_ms: [general] zdon_hit_timing_show half = good / 2,
    // ok = good, all = -1); nil before the first judged hit or with [general] zdon_hit_timing off
    tex.set_function("last_hit", [](int player_num, sol::this_state s) -> sol::variadic_results {
        sol::variadic_results r;
        if (!global_data.config->general.zdon_hit_timing || !valid_player(player_num)) {
            return r;
        }
        const ZdonLiveData& live = global_data.session_data[player_num].zdon_live;
        if (live.last_hit_at_ms < 0) {
            return r;
        }
        auto [good, ok] = judge_windows(player_num);
        r.push_back({s, sol::in_place, live.last_hit_offset_ms});
        r.push_back({s, sol::in_place, live.last_hit_at_ms});
        r.push_back({s, sol::in_place, (double)good});
        r.push_back({s, sol::in_place, (double)ok});
        const std::string& show = global_data.config->general.zdon_hit_timing_show;
        const double mark = show == "all" ? -1.0 : show == "ok" ? (double)good : good * 0.5;
        r.push_back({s, sol::in_place, mark});
        return r;
    });

    // tex.hit_offsets(player_num) -> { offset_ms, ... } (the finished song's judged hits, in order),
    // good_ms, ok_ms; nil with zdon_hit_timing off
    tex.set_function("hit_offsets", [](int player_num, sol::this_state s) -> sol::variadic_results {
        sol::variadic_results r;
        if (!global_data.config->general.zdon_hit_timing || !valid_player(player_num)) {
            return r;
        }
        sol::state_view lua(s);
        const std::vector<float>& offsets = global_data.session_data[player_num].result_data.hit_offsets;
        sol::table t = lua.create_table((int)offsets.size(), 0);
        for (size_t i = 0; i < offsets.size(); ++i) {
            t[i + 1] = offsets[i];
        }
        auto [good, ok] = judge_windows(player_num);
        r.push_back({s, sol::in_place, t});
        r.push_back({s, sol::in_place, (double)good});
        r.push_back({s, sol::in_place, (double)ok});
        return r;
    });

    // tex.song_progress(player_num) -> pos_ms, end_ms, { {start_ms, end_ms}, ... } (GOGO sections), all on
    // the chart clock; nil outside a song or with [general] zdon_song_progress off
    tex.set_function("song_progress", [](int player_num, sol::this_state s) -> sol::variadic_results {
        sol::variadic_results r;
        if (!global_data.config->general.zdon_song_progress || !valid_player(player_num) ||
            global_data.zdon_song_pos_ms < 0) {
            return r;
        }
        const ZdonLiveData& live = global_data.session_data[player_num].zdon_live;
        if (live.song_end_ms <= 0) {
            return r;
        }
        sol::state_view lua(s);
        sol::table gogo = lua.create_table((int)live.gogo_ranges.size(), 0);
        for (size_t i = 0; i < live.gogo_ranges.size(); ++i) {
            gogo[i + 1] = lua.create_table_with(1, live.gogo_ranges[i].first, 2, live.gogo_ranges[i].second);
        }
        r.push_back({s, sol::in_place, global_data.zdon_song_pos_ms});
        r.push_back({s, sol::in_place, live.song_end_ms});
        r.push_back({s, sol::in_place, gogo});
        return r;
    });

    // tex.practice_loop() -> active, a_bar, b_bar, passes, last_good, last_ok, last_bad (nil: no loop and no
    // finished pass); bars are 0-based indices of the chart's bar lines
    tex.set_function("practice_loop", [](sol::this_state s) -> sol::variadic_results {
        sol::variadic_results r;
        const auto& loop = global_data.zdon_loop;
        if (!loop.active && !loop.has_last) {
            return r;
        }
        r.push_back({s, sol::in_place, loop.active});
        r.push_back({s, sol::in_place, (int)loop.a_bar});
        r.push_back({s, sol::in_place, (int)loop.b_bar});
        r.push_back({s, sol::in_place, (int)loop.passes});
        if (loop.has_last) {
            r.push_back({s, sol::in_place, (int)loop.last_good});
            r.push_back({s, sol::in_place, (int)loop.last_ok});
            r.push_back({s, sol::in_place, (int)loop.last_bad});
        }
        return r;
    });

    // tex.ai_battle() -> nil outside an AI battle (objects/game/zdon_ai.h), else a table:
    //   level, state (0 even, 1 AI leads the section, 2 AI trails), section (1-based, count+1 when done), count,
    //   live_p, live_ai (the open section's points), wins_p, wins_ai, finished, result (finished: "win" / "lose" /
    //   "draw"; a tie in sections goes to the higher total), total_p, total_ai,
    //   sections = { {p=, ai=, winner= -1 open / 0 player / 1 AI / 2 draw, end_ms=}, ... }
    tex.set_function("ai_battle", [](sol::this_state s) -> sol::object {
        const auto& b = global_data.zdon_ai;
        if (!b.active || b.bounds.empty()) {
            return sol::lua_nil;
        }
        sol::state_view lua(s);
        sol::table t = lua.create_table();
        int wins_p = 0, wins_ai = 0, total_p = 0, total_ai = 0;
        sol::table secs = lua.create_table((int)b.bounds.size(), 0);
        for (size_t i = 0; i < b.bounds.size(); ++i) {
            const bool open = (int)i == b.section;
            const int p = open ? b.live_p : b.points_p[i];
            const int a = open ? b.live_ai : b.points_ai[i];
            wins_p += b.winners[i] == 0;
            wins_ai += b.winners[i] == 1;
            total_p += p;
            total_ai += a;
            secs[i + 1] = lua.create_table_with("p", p, "ai", a, "winner", b.winners[i], "end_ms", b.bounds[i]);
        }
        t["level"] = b.level;
        t["state"] = b.state;
        t["section"] = b.section + 1;
        t["count"] = (int)b.bounds.size();
        t["live_p"] = b.live_p;
        t["live_ai"] = b.live_ai;
        t["wins_p"] = wins_p;
        t["wins_ai"] = wins_ai;
        t["total_p"] = total_p;
        t["total_ai"] = total_ai;
        t["finished"] = b.finished;
        if (b.finished) {
            const int d = wins_p != wins_ai ? wins_p - wins_ai : total_p - total_ai;
            t["result"] = d > 0 ? "win" : (d < 0 ? "lose" : "draw");
        }
        t["sections"] = secs;
        return t;
    });

    // tex.ai_level() -> the next AI battle's level (1..10; [general] zdon_ai_level, moved by each match's result)
    tex.set_function("ai_level", []() -> int { return global_data.config->general.zdon_ai_level; });

    // tex.best_score(player_num) -> the saved best score of this song and course before this play (0 = none);
    // set when the result is saved, so it is meant for the result screen
    tex.set_function("best_score", [](int player_num) -> int {
        if (!valid_player(player_num)) {
            return 0;
        }
        return global_data.session_data[player_num].result_data.prev_score;
    });
}
