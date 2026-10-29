#include "zdon_lua.h"

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

void register_zdon_lua_bindings(sol::table& tex) {
    // tex.last_hit(player_num) -> offset_ms (+ = late), at_ms (get_current_ms of the hit), good_ms, ok_ms;
    // nil before the first judged hit or with [general] zdon_hit_timing off
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

    // tex.best_score(player_num) -> the saved best score of this song and course before this play (0 = none);
    // set when the result is saved, so it is meant for the result screen
    tex.set_function("best_score", [](int player_num) -> int {
        if (!valid_player(player_num)) {
            return 0;
        }
        return global_data.session_data[player_num].result_data.prev_score;
    });
}
