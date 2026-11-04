#include "zdon_ai_board.h"

#include <algorithm>
#include <array>
#include <map>

#include "../../libs/file/scores.h"
#include "../../libs/global_data.h"
#include "../../libs/time.h"
#include "../song_select/file_navigator/color_utils.h"
#include "../song_select/file_navigator/navigator.h"

namespace {
    struct Entry {
        std::string title, sub;
        int crown = 0;
        std::array<int, 5> levels = {-1, -1, -1, -1, -1};
        int outline[3] = {101, 0, 82}; // the genre title outline (the shelf colours)
    };
    double jump_ms = -1.0; // >= 0: the chosen card is held (zdon_ai_p23), ms since the cursor landed on it
    bool board_on = false, info_on = false;
    AiBoardCourse board_course, info_course;
    std::vector<Entry> entries;
    int board_cursor = 0;

    // the song's title / subtitle in the player's language (else English, else the fallback)
    std::string pick(const std::vector<std::pair<std::string, std::string>>& by_lang, const std::string& fallback) {
        const std::string& lang = global_data.config->general.language;
        std::string en;
        for (const auto& [l, t] : by_lang) {
            if (l == lang && !t.empty()) {
                return t;
            }
            if (l == "en") {
                en = t;
            }
        }
        return en.empty() ? fallback : en;
    }

    int crown_of(const fs::path& p, int course) {
        auto hashes = scores_manager.get_hashes(p);
        if (course < 0 || course > 4 || hashes[course].empty()) {
            return 0;
        }
        auto s = scores_manager.get_score(hashes[course], course, global_data.config->network.access_code_1);
        return s ? static_cast<int>(s->crown) : 0;
    }

    std::map<std::pair<int, int>, AiBoardCourse> cache; // (course, level) -> numbers
} // namespace

std::vector<fs::path> zdon_ai_board_songs(const AiBoardCourse& c) {
    size_t total = 0;
    return navigator.zdon_search("", "", c.course, c.level, 100000, total);
}

AiBoardCourse zdon_ai_board_course(int column, int last_course, int last_level) {
    AiBoardCourse c;
    c.course = column >= 1 && column <= 4 ? column - 1 : 3;
    if (column == 6 && last_course >= 0) {
        c.course = std::min(last_course, 3);
    }
    // the level most songs of the course have (the footage's recommended column: oni, 112 songs at 10)
    int best = 0;
    for (int lv = 1; lv <= 10; ++lv) {
        size_t total = 0;
        navigator.zdon_search("", "", c.course, lv, 0, total);
        if (static_cast<int>(total) > best) {
            best = static_cast<int>(total);
            c.level = lv;
        }
    }
    if (column == 6 && last_level > 0) {
        c.level = last_level;
    }
    if (c.level == 0) {
        return c;
    }
    auto it = cache.find({c.course, c.level});
    if (it != cache.end()) {
        return it->second;
    }
    for (const auto& p : zdon_ai_board_songs(c)) {
        ++c.count;
        const int cr = crown_of(p, c.course);
        c.clear += cr >= static_cast<int>(Crown::CLEAR);
        c.fc += cr >= static_cast<int>(Crown::FC);
    }
    cache[{c.course, c.level}] = c;
    return c;
}

void zdon_ai_board_set(bool on, const AiBoardCourse& c, const std::vector<fs::path>& songs, int cursor) {
    if (on && !board_on) {
        entries.clear();
        for (const auto& p : songs) {
            Entry e;
            auto u8 = p.u8string();
            auto meta = scores_manager.get_song_meta(std::string(u8.begin(), u8.end()));
            e.title = meta ? pick(meta->titles, meta->title) : p.stem().string();
            if (e.title.empty()) {
                e.title = p.stem().string();
            }
            if (meta) {
                e.sub = pick(meta->subtitles, meta->subtitle);
            }
            e.crown = crown_of(p, c.course);
            e.levels = navigator.zdon_levels_of(p);
            const auto genre = static_cast<GenreIndex>(navigator.zdon_genre_of(p).first);
            const auto col = DEFAULT_COLORS.find(genre);
            if (col != DEFAULT_COLORS.end() && col->second[1].has_value()) {
                e.outline[0] = col->second[1]->r;
                e.outline[1] = col->second[1]->g;
                e.outline[2] = col->second[1]->b;
            }
            entries.push_back(std::move(e));
        }
    } else if (!on) {
        entries.clear();
        cache.clear(); // the next visit may follow a play
    }
    board_on = on;
    board_course = c;
    board_cursor = cursor;
}

void zdon_ai_board_jump_set(double ms) {
    jump_ms = ms;
}

bool zdon_ai_board_on() {
    return board_on;
}

void zdon_ai_board_info_set(bool on, int /*column*/, const AiBoardCourse& c) {
    info_on = on;
    info_course = c;
}

namespace {
    bool arcade_sort_on = false, sort_view_on = false;
    AiBoardCourse sort_view_course;
} // namespace

void zdon_ai_arcade_sort_enable(bool on) {
    arcade_sort_on = on;
    if (!on) {
        sort_view_on = false;
    }
}

bool zdon_ai_arcade_sort() {
    return arcade_sort_on;
}

void zdon_ai_sort_view_set(bool on, const AiBoardCourse& c) {
    sort_view_on = on;
    sort_view_course = c;
}

bool zdon_ai_sort_view_on() {
    return sort_view_on;
}

namespace {
    struct LevelPanel {
        bool on = false, closing = false, lit = true;
        int course = 3, level = 1, top = 4;
        double t0 = 0.0, out0 = 0.0;
    } level_panel;
} // namespace

AiBoardCourse zdon_ai_board_level(int course, int level) {
    AiBoardCourse c;
    c.course = course;
    c.level = level;
    auto it = cache.find({course, level});
    if (it != cache.end()) {
        return it->second;
    }
    for (const auto& p : zdon_ai_board_songs(c)) {
        ++c.count;
        const int cr = crown_of(p, c.course);
        c.clear += cr >= static_cast<int>(Crown::CLEAR);
        c.fc += cr >= static_cast<int>(Crown::FC);
    }
    cache[{course, level}] = c;
    return c;
}

int zdon_ai_board_top(int course) {
    if (course >= 3) {
        return 13;
    }
    int top = 4;
    for (int lv = 1; lv <= 13; ++lv) {
        size_t total = 0;
        navigator.zdon_search("", "", course, lv, 0, total);
        if (total > 0) {
            top = std::max(top, lv);
        }
    }
    return top;
}

void zdon_ai_level_panel_open(int course, int level, int top) {
    level_panel = {true, false, true, course, level, top, get_current_ms(), 0.0};
}

void zdon_ai_level_panel_phase(bool lit) {
    level_panel.lit = lit;
}

void zdon_ai_level_panel_set(int level) {
    level_panel.level = level;
}

void zdon_ai_level_panel_course(int course, int level, int top) {
    level_panel.course = course;
    level_panel.level = level;
    level_panel.top = top;
}

void zdon_ai_level_panel_close() {
    if (level_panel.on && !level_panel.closing) {
        level_panel.closing = true;
        level_panel.out0 = get_current_ms();
    }
}

void zdon_ai_level_panel_reset() {
    level_panel = {};
}

void zdon_ai_board_bind(sol::table& tex) {
    tex.set_function("ai_level_panel", [](sol::this_state ts) -> sol::object {
        if (!level_panel.on) {
            return sol::make_object(ts, sol::lua_nil);
        }
        sol::state_view lua(ts);
        const double now = get_current_ms();
        sol::table t = lua.create_table_with("course", level_panel.course, "level", level_panel.level, "top",
                                             level_panel.top, "ms", now - level_panel.t0);
        t["lit"] = level_panel.lit;
        if (level_panel.closing) {
            t["out"] = now - level_panel.out0;
        }
        return t;
    });
    tex.set_function("ai_sort_view", [](sol::this_state ts) -> sol::object {
        if (!sort_view_on) {
            return sol::make_object(ts, sol::lua_nil);
        }
        sol::state_view lua(ts);
        return sol::make_object(ts, lua.create_table_with("course", sort_view_course.course, "level",
                                                          sort_view_course.level, "count", sort_view_course.count));
    });
    tex.set_function("ai_board", [](sol::this_state ts) -> sol::object {
        if (!board_on) {
            return sol::make_object(ts, sol::lua_nil);
        }
        sol::state_view lua(ts);
        sol::table t = lua.create_table_with("course", board_course.course, "level", board_course.level, "count",
                                             board_course.count, "clear", board_course.clear, "fc", board_course.fc,
                                             "cursor", board_cursor, "total", static_cast<int>(entries.size()));
        sol::table items = lua.create_table();
        const int n = static_cast<int>(entries.size());
        for (int d = -7; d <= 7; ++d) {
            const int i = board_cursor + d;
            if (i < -1 || i >= n) {
                continue;
            }
            if (i == -1) { // the back strip before the first song
                items[d] = lua.create_table_with("back", true, "title", "", "sub", "", "crown", 0);
                continue;
            }
            sol::table it =
                lua.create_table_with("title", entries[i].title, "sub", entries[i].sub, "crown", entries[i].crown);
            it["outline"] =
                lua.create_table_with(1, entries[i].outline[0], 2, entries[i].outline[1], 3, entries[i].outline[2]);
            sol::table lv = lua.create_table();
            for (int c = 0; c < 5; ++c) {
                lv[c + 1] = entries[i].levels[c];
            }
            it["levels"] = lv; // per course (1 easy .. 5 ura), -1 = none
            items[d] = it;
        }
        t["items"] = items;
        if (jump_ms >= 0.0) {
            t["jump"] = jump_ms;
        }
        return sol::make_object(ts, t);
    });
    tex.set_function("ai_diff_info", [](sol::this_state ts) -> sol::object {
        if (!info_on || info_course.level == 0) {
            return sol::make_object(ts, sol::lua_nil);
        }
        sol::state_view lua(ts);
        return sol::make_object(ts, lua.create_table_with("course", info_course.course, "level", info_course.level,
                                                          "count", info_course.count, "clear", info_course.clear, "fc",
                                                          info_course.fc));
    });
}
