#include "zdon_ai_board.h"

#include <algorithm>
#include <map>

#include "../../libs/file/scores.h"
#include "../../libs/global_data.h"
#include "../song_select/file_navigator/navigator.h"

namespace {
    struct Entry {
        std::string title, sub;
        int crown = 0;
    };
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

bool zdon_ai_board_on() {
    return board_on;
}

void zdon_ai_board_info_set(bool on, int /*column*/, const AiBoardCourse& c) {
    info_on = on;
    info_course = c;
}

void zdon_ai_board_bind(sol::table& tex) {
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
            items[d] =
                lua.create_table_with("title", entries[i].title, "sub", entries[i].sub, "crown", entries[i].crown);
        }
        t["items"] = items;
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
