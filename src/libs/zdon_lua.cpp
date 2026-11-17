#include "zdon_lua.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../objects/entry/zdon_outfit_menu.h"
#include "../objects/enums.h"
#include "../objects/game/player.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/game/zdon_ai_board.h"
#include "../objects/game/zdon_lane3d.h"
#include "density.h"
#include "file/scores.h"
#include "file/script.h"
#include "global_data.h"

namespace {
    bool valid_player(int player_num) {
        return player_num >= 0 && player_num < static_cast<int>(global_data.session_data.size());
    }

    // the good / ok windows the player's course judges with (Player::check_note)
    std::pair<float, float> judge_windows(int player_num) {
        if (global_data.session_data[player_num].selected_difficulty <= static_cast<int>(Difficulty::NORMAL)) {
            return {Timing::GOOD_EASY, Timing::OK_EASY};
        }
        return {Timing::GOOD, Timing::OK};
    }
} // namespace

void register_zdon_lua_bindings(sol::table& tex) {
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
        r.push_back({s, sol::in_place, static_cast<double>(good)});
        r.push_back({s, sol::in_place, static_cast<double>(ok)});
        const std::string& show = global_data.config->general.zdon_hit_timing_show;
        const double mark = show == "all" ? -1.0 : show == "ok" ? static_cast<double>(good) : good * 0.5;
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
        sol::table t = lua.create_table(static_cast<int>(offsets.size()), 0);
        for (size_t i = 0; i < offsets.size(); ++i) {
            t[i + 1] = offsets[i];
        }
        auto [good, ok] = judge_windows(player_num);
        r.push_back({s, sol::in_place, t});
        r.push_back({s, sol::in_place, static_cast<double>(good)});
        r.push_back({s, sol::in_place, static_cast<double>(ok)});
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
        sol::table gogo = lua.create_table(static_cast<int>(live.gogo_ranges.size()), 0);
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
        r.push_back({s, sol::in_place, loop.a_bar});
        r.push_back({s, sol::in_place, loop.b_bar});
        r.push_back({s, sol::in_place, loop.passes});
        if (loop.has_last) {
            r.push_back({s, sol::in_place, loop.last_good});
            r.push_back({s, sol::in_place, loop.last_ok});
            r.push_back({s, sol::in_place, loop.last_bad});
        }
        return r;
    });

    // tex.ai_battle() -> nil outside an AI battle (objects/game/zdon_ai.h), else a table:
    //   level (tuning level 1..11), condition (1..5), state (0 neutral, 1 the player leads, 2 the player trails),
    //   tug (clamp(player - AI + 9, 1, 17) of the open section), section (1-based, count+1 when done), count,
    //   live_p, live_ai (the open section's points), wins_p, wins_ai, finished, result (finished: "win" / "lose"),
    //   total_p, total_ai, judge_p, judge_ai = { good, ok, bad, drumroll }
    //   sections = { {p=, ai=, winner= -1 open / 0 player / 1 AI, crown= 0..3, end_ms=,
    //                 good=, ok=, bad=, roll= (the player's judges in it), score= (the player's score in it),
    //                 best = {score=, good=, ok=, bad=, roll=} (the best play's, recorded before this play) or nil},
    //                 ...}
    //   new_best (finished: this play is the chart's new best AI battle play), detail (the result's detail view)
    tex.set_function("ai_battle", [](sol::this_state s) -> sol::object {
        const auto& b = global_data.zdon_ai;
        if (!b.active || b.bounds.empty()) {
            return sol::lua_nil;
        }
        sol::state_view lua(s);
        sol::table t = lua.create_table();
        int wins_p = 0, wins_ai = 0, total_p = 0, total_ai = 0;
        sol::table secs = lua.create_table(static_cast<int>(b.bounds.size()), 0);
        for (size_t i = 0; i < b.bounds.size(); ++i) {
            const bool open = static_cast<int>(i) == b.section;
            const int p = open ? b.live_p : b.points_p[i];
            const int a = open ? b.live_ai : b.points_ai[i];
            wins_p += b.winners[i] == 0;
            wins_ai += b.winners[i] == 1;
            total_p += p;
            total_ai += a;
            std::array<int, 4> j{};
            if (i < b.section_judge.size()) {
                j = b.section_judge[i];
            }
            if (open) {
                for (int k = 0; k < 4; ++k) {
                    j[k] = b.judge_p[k] - b.base_judge[k];
                }
            }
            sol::table sec = lua.create_table_with("p", p, "ai", a, "winner", b.winners[i], "crown",
                                                   i < b.crowns.size() ? b.crowns[i] : 0, "end_ms", b.bounds[i]);
            sec["good"] = j[0];
            sec["ok"] = j[1];
            sec["bad"] = j[2];
            sec["roll"] = j[3];
            sec["score"] = open ? 0 : (i < b.section_score.size() ? b.section_score[i] : 0);
            if (i < b.section_best.size()) {
                const auto& r = b.section_best[i];
                sec["best"] = lua.create_table_with("score", r[0], "good", r[1], "ok", r[2], "bad", r[3], "roll", r[4]);
            }
            secs[i + 1] = sec;
        }
        t["level"] = b.level;
        // the song's genre (session genre_index: 0 J-POP .. 7 namco original, 8 none) and the title's visibility
        const SessionData& sd = global_data.session_data[static_cast<int>(global_data.player_num)];
        t["genre"] = sd.genre_index;
        t["title_alpha"] = zdon_ai_title_alpha();
        t["revealed"] = zdon_ai_revealed();
        t["emotion_low"] = zdon_ai_tuning(b.level).emotion_low;
        t["emotion_high"] = zdon_ai_tuning(b.level).emotion_high;
        t["condition"] = b.condition;
        t["state"] = b.state;
        t["tug"] = b.tug;
        t["section"] = b.section + 1;
        t["count"] = static_cast<int>(b.bounds.size());
        t["live_p"] = b.live_p;
        t["live_ai"] = b.live_ai;
        t["judge_p"] = lua.create_table_with(1, b.judge_p[0], 2, b.judge_p[1], 3, b.judge_p[2], 4, b.judge_p[3]);
        t["judge_ai"] = lua.create_table_with(1, b.judge_ai[0], 2, b.judge_ai[1], 3, b.judge_ai[2], 4, b.judge_ai[3]);
        t["wins_p"] = wins_p;
        t["wins_ai"] = wins_ai;
        t["total_p"] = total_p;
        t["total_ai"] = total_ai;
        t["finished"] = b.finished;
        t["skipped"] = b.skipped;
        if (b.finished) {
            t["result"] = b.won ? "win" : "lose";
            t["new_best"] = b.new_best;
            t["detail"] = b.detail;
            t["rank_before"] = b.rank_before;
            t["rank"] = b.rank;
            t["points_before"] = b.points_before;
            t["points"] = b.points;
            t["rank_gain"] = b.rank_gain;
            t["first_win"] = zdon_ai_last_first_win();
            t["bonus_high"] = zdon_ai_last_bonus_high();
            t["play_all"] = b.play_all;
        }
        t["sections"] = secs;
        return t;
    });

    // tex.ai_credit() -> the credit's AI battles so far (the play-all summary), oldest first:
    //   { {title=, score=, good=, ok=, bad=, cleared=, won=, sections = { {score=, crown=}, ... }}, ... }
    tex.set_function("ai_credit", [](sol::this_state s) {
        sol::state_view lua(s);
        sol::table list = lua.create_table();
        int i = 1;
        for (const auto& song : global_data.zdon_ai_credit) {
            sol::table t = lua.create_table();
            t["title"] = song.title;
            t["score"] = song.score;
            t["good"] = song.good;
            t["ok"] = song.ok;
            t["bad"] = song.bad;
            t["cleared"] = song.cleared;
            t["won"] = song.won;
            sol::table secs = lua.create_table();
            for (size_t k = 0; k < song.section_score.size(); ++k) {
                secs[k + 1] = lua.create_table_with("score", song.section_score[k], "crown",
                                                    k < song.section_crown.size() ? song.section_crown[k] : 0);
            }
            t["sections"] = secs;
            list[i++] = t;
        }
        return list;
    });

    // tex.ai_active() -> true in an AI battle's GAME_2P / RESULT_2P from the first frame (tex.ai_battle() stays
    // nil until the song's first frame cuts the sections): skins swap the 2P character for the AI's
    tex.set_function("ai_active", [] -> bool { return global_data.zdon_ai.active; });

    // tex.zdon_ai_select() -> true on Z-DON's AI select only: Screens::AI_SELECT while the skin does not use
    // upstream's AI battle (zdon_ai_upstream). Green's Z-DON AI hooks test this instead of current_screen() ==
    // "AI_SELECT", so upstream's AI select in Green draws without them; Nijiiro / YataiDON14 inherit the hooks
    tex.set_function("zdon_ai_select",
                     [] -> bool { return global_data.current_screen == "AI_SELECT" && !zdon_ai_upstream(); });

    // tex.ai_diff_stage() -> nil, or the cursor (0 back, 1..4 easy..oni, 5 recommended, 6 last) while the AI select
    // shows its course columns (skin_config zdon_ai_diff_first)
    tex.set_function("ai_diff_stage", [](sol::this_state ts) -> sol::object {
        int cursor = 0;
        if (!zdon_ai_diff_stage(&cursor)) {
            return sol::make_object(ts, sol::lua_nil);
        }
        return sol::make_object(ts, cursor);
    });

    zdon_ai_board_bind(tex); // tex.ai_board(), tex.ai_diff_info()

    // tex.upai_song() -> genre, title_alpha on upstream's AI game in a "zdon_upai_arena" skin (else nil): the session's
    // genre_index (0 J-POP .. 7 namco original, 8 none) and the song title's visibility (SongInfo), for the skin's
    // genre pill under the title (Green draws it, zdon_ai_song_info width 1)
    tex.set_function("upai_song", [](sol::this_state ts) -> std::tuple<sol::object, sol::object> {
        if (!zdon_upai_arena()) {
            return {sol::make_object(ts, sol::lua_nil), sol::make_object(ts, sol::lua_nil)};
        }
        const SessionData& sd = global_data.session_data[static_cast<int>(global_data.player_num)];
        return {sol::make_object(ts, sd.genre_index), sol::make_object(ts, zdon_ai_title_alpha())};
    });

    // tex.ai_transition() -> true while an AI battle is being started: from the song select's decide (the VS window and
    // loading card of its game transition) until the battle ends. Unlike ai_active it is already true there
    tex.set_function("ai_transition",
                     [] -> bool { return global_data.zdon_ai.requested || global_data.zdon_ai.active; });

    // tex.ai_draw_don(x, y, scale, rot_y): the arena (skin_config zdon_ai_arena) shows the player's 3D Don this frame
    // at x, y (the character's feet, 1080p px), scaled, turned rot_y degrees (the cabinet's AI battle stands it in
    // the arena facing the AI). Drawn by the player's lane after the background; call it every frame it shows
    tex.set_function("ai_draw_don", [](float x, float y, sol::optional<float> scale, sol::optional<float> rot) {
        auto& b = global_data.zdon_ai;
        b.don_show = b.active;
        b.don_x = x;
        b.don_y = y;
        b.don_scale = scale.value_or(1.0f);
        b.don_rot = rot.value_or(0.0f);
        if (b.don_show && b.don_draw_now && script_manager.tex.options[SCO::AI_DON_UNDER_FLAMES]) {
            b.don_draw_now(); // at the call, so what the skin draws next (the clash flames) lies over it
        }
    });

    // tex.ai_draw_rival(x, y, scale, rot_y) -> bool: screen option ai_game_chara_3d, the arena's AI as the 3D AI Don at
    // the call (feet at x, y, 1080p px); false = nothing drawn (the skin keeps its 2D AI)
    tex.set_function("ai_draw_rival",
                     [](float x, float y, sol::optional<float> scale, sol::optional<float> rot) -> bool {
                         auto& b = global_data.zdon_ai;
                         if (!b.active || !b.ai_draw_now || !script_manager.tex.options[SCO::AI_GAME_CHARA_3D]) {
                             return false;
                         }
                         b.ai_draw_now(x, y, scale.value_or(1.0f), rot.value_or(0.0f));
                         return true;
                     });

    // tex.ai_vs_chara_load() / ai_vs_chara_draw(side, x, y, scale, rot_y) / ai_vs_chara_release(): the loading VS
    // window's 3D Dons (side 0 = the player, 1 = the AI; screen option ai_loading_chara_3d)
    tex.set_function("ai_vs_chara_load", [] { zdon_ai_vs_chara_load(); });
    tex.set_function("ai_vs_chara_draw",
                     [](int side, float x, float y, sol::optional<float> scale, sol::optional<float> rot) {
                         zdon_ai_vs_chara_draw(side, x, y, scale.value_or(1.0f), rot.value_or(0.0f));
                     });
    tex.set_function("ai_vs_chara_release", [] { zdon_ai_vs_chara_release(); });

    // tex.set_screen_desaturate(amount): 0..1, call it every frame it should apply. The next frame renders through a
    // render target and a grayscale shader (libs/density.cpp); never called = no target, no shader, no cost
    tex.set_function("set_screen_desaturate", [](float amount) { density::request_desaturate(amount); });

    // tex.draw_frozen_screen() -> bool: draws the held frame (density::request_freeze; the result screen keeps its
    // last frame for a skin_config zdon_end_over_result skin) over the whole screen; false = none held
    tex.set_function("draw_frozen_screen", [] {
        return density::draw_frozen(static_cast<float>(script_manager.tex.screen_width),
                                    static_cast<float>(script_manager.tex.screen_height));
    });

    // tex.ai_pages(open) -> steps, finished: a skin's result cut-in pages (Green's rank up / reward). open = the
    // pages show this frame (the result screen then turns a kat into the next page and a don into the end, and holds
    // its timeout); steps = kat presses since they opened, finished = a don ended them
    tex.set_function("ai_pages", [](bool open) -> std::tuple<int, bool> {
        zdon_ai_pages_set(open);
        return {zdon_ai_page_steps(), zdon_ai_pages_finished()};
    });

    // tex.ai_level() / tex.ai_condition() -> the AI's condition this session, 1 worst .. 5 best (its tension; the
    // arcade's aiCondition, rolled once per session). ai_level is the old name.
    tex.set_function("ai_level", [] -> int { return zdon_ai_condition(); });
    tex.set_function("ai_condition", [] -> int { return zdon_ai_condition(); });

    // tex.zdon_lane3d(yaw, pitch[, distance[, ms]]) -> the play screen's 3D view ([general] zdon_lane3d; nothing when
    // off): degrees (yaw -55..55, + turns the right side away; pitch -25..65, + tips the top away), distance 1 = as
    // flat (0.4..3), reached over ms (default 0: at once). The mouse's turn adds to it. tex.zdon_lane3d_on() -> whether
    // the view is enabled, so a script can leave it alone otherwise.
    tex.set_function("zdon_lane3d",
                     [](float yaw, float pitch, sol::optional<float> distance, sol::optional<double> ms) {
                         zdon_lane3d::set_script_view(yaw, pitch, distance.value_or(1.0f), ms.value_or(0.0));
                     });
    tex.set_function("zdon_lane3d_on", [] -> bool { return zdon_lane3d::enabled(); });

    // tex.coin_mode() -> [general] zdon_coin_mode (default off): the skin shows the cabinet credit text instead of
    // the free-play text; tex.coin_credits() -> [general] zdon_coin_credits (0..99), a plain counter, no coin input
    tex.set_function("coin_mode", [] -> bool { return global_data.config->general.zdon_coin_mode; });
    tex.set_function("coin_credits",
                     [] -> int { return std::clamp(global_data.config->general.zdon_coin_credits, 0, 99); });

    // tex.ai_skill() -> the AI's skill level 1..10 ([general] zdon_ai_level, +1 per match won, -1 per loss); the
    // course board's "pick the difficulty level" pillars show it
    tex.set_function("ai_skill", [] -> int { return std::clamp(global_data.config->general.zdon_ai_level, 1, 10); });

    // tex.ai_rank() -> the player's AI battle rank (the Green ghost rank, objects/game/zdon_ai.h):
    //   {rank = 0..11, points =, next = points still needed for the next rank (0 at the top),
    //    least = {e, n, h, m, x} (the lowest star level that counts toward the next rank: the next row; 0 = not
    //    counted, 99 = only the task folder's songs), bonus = {...}}
    tex.set_function("ai_rank", [](sol::this_state s) {
        sol::state_view lua(s);
        auto [points, rank] = scores_manager.get_zdon_ai_rank(get_player_id(PlayerNum::P1));
        const ZdonAiRank& r = zdon_ai_rank_info(std::min(rank + 1, kZdonAiRanks - 1));
        sol::table t = lua.create_table();
        t["rank"] = rank;
        t["points"] = points;
        t["next"] = rank + 1 < kZdonAiRanks ? std::max(0, zdon_ai_rank_info(rank + 1).acquired - points) : 0;
        t["least"] = lua.create_table_with(1, r.least[0], 2, r.least[1], 3, r.least[2], 4, r.least[3], 5, r.least[4]);
        t["bonus"] = lua.create_table_with(1, r.bonus[0], 2, r.bonus[1], 3, r.bonus[2], 4, r.bonus[3], 5, r.bonus[4]);
        return t;
    });

    // tex.ai_next_reward() -> wins still needed for the next AI battle reward (0: past the last one)
    tex.set_function("ai_next_reward",
                     [] -> int { return zdon_ai_next_reward(global_data.config->general.zdon_ai_wins); });

    // tex.outfit(player_num) -> the player's 2D outfit in this skin (0 = plain; skin_config "zdon_outfits",
    // objects/entry/zdon_outfit_menu.h)
    tex.set_function("outfit", [](int player_num) -> int {
        if (player_num != 1 && player_num != 2) {
            return 0;
        }
        return zdon_player_outfit(static_cast<PlayerNum>(player_num));
    });

    // tex.ai_wins() -> AI battles won so far ([general] zdon_ai_wins)
    tex.set_function("ai_wins", [] -> int { return global_data.config->general.zdon_ai_wins; });

    // tex.best_score(player_num) -> the saved best score of this song and course before this play (0 = none);
    // set when the result is saved, so it is meant for the result screen
    tex.set_function("best_score", [](int player_num) -> int {
        if (!valid_player(player_num)) {
            return 0;
        }
        return global_data.session_data[player_num].result_data.prev_score;
    });

    // tex.record_counts(player_num, min_difficulty) -> { ranks = {white..rainbow}, crowns = {clear, fc, dfc} }: the
    // player's best rank / crown per chart of that difficulty or higher, counted (the AI battle's player board)
    tex.set_function("record_counts", [](int player_num, int min_difficulty, sol::this_state s) -> sol::object {
        if (player_num != 1 && player_num != 2) {
            return sol::lua_nil;
        }
        const std::array<int, 10> c =
            scores_manager.get_zdon_record_counts(get_player_id(static_cast<PlayerNum>(player_num)), min_difficulty);
        sol::state_view lua(s);
        sol::table ranks = lua.create_table_with(1, c[0], 2, c[1], 3, c[2], 4, c[3], 5, c[4], 6, c[5], 7, c[6]);
        sol::table crowns = lua.create_table_with(1, c[7], 2, c[8], 3, c[9]);
        return lua.create_table_with("ranks", ranks, "crowns", crowns);
    });
}
