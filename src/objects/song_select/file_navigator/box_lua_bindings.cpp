#include "box_lua_bindings.h"

#include <algorithm>
#include <iterator>

#include "../../../libs/file/script.h"
#include "../../game/exam_caption.h"
#include "../diff_sort.h"
#include "../player.h"
#include "../song_select_script.h"
#include "box_back.h"
#include "box_dan.h"
#include "box_folder.h"
#include "box_song.h"
#include "genre_bg.h"
#include "navigator.h"
#include "text_layout.h"

namespace {
    template<class Info>
    sol::table course_table(const Info& info, bool has_score) {
        sol::table t = script_manager.lua->create_table(0, 6);
        t["has_course"] = info.has_course;
        t["level"] = info.level;
        t["is_branching"] = info.is_branching;
        t["crown"] = info.crown;
        t["rank"] = info.rank;
        t["has_score"] = has_score;
        return t;
    }
} // namespace

// split into several files: each new_usertype instantiates a lot of sol2, and one file of them all was
// by far the slowest to compile
void register_song_select_lua_bindings(sol::state& lua) {
    register_box_lua_bindings(lua);
    register_dan_box_lua_bindings(lua);
    register_player_lua_bindings(lua);
}

void register_box_lua_bindings(sol::state& lua) {
    lua.new_usertype<BaseBox>(
        "BaseBox", "box_x", &BaseBox::box_x, "box_y", &BaseBox::box_y, "left_bound", &BaseBox::left_bound,
        "right_bound", &BaseBox::right_bound,
        // read-only properties: box.fade / box.open_fade / box.open_anim stay the animation
        // objects Lua skins index (box.fade.attribute) now that the box owns them by unique_ptr
        "fade", sol::property([](BaseBox& self) { return self.fade.get(); }), "open_fade",
        sol::property([](BaseBox& self) { return self.open_fade.get(); }), "open_anim",
        sol::property([](BaseBox& self) { return self.open_anim.get(); }), "draw_state", &BaseBox::draw_state,
        "lua_kind", &BaseBox::lua_kind, "text_name", &BaseBox::text_name, "path",
        [](BaseBox& self) { return self.path.string(); }, "is_new", &BaseBox::is_new, "genre_frame",
        [](BaseBox& self) { return genre_to_ref_frame(self.genre_index); }, "fore_color",
        [](BaseBox& self) -> sol::object {
            sol::table t = script_manager.lua->create_table(0, 4);
            const Color& c = self.fore_color;
            t["r"] = c.r;
            t["g"] = c.g;
            t["b"] = c.b;
            t["a"] = c.a;
            return t;
        },
        "name", &BaseBox::horizontal_name, "collection", &BaseBox::collection, "genre_index",
        [](BaseBox& self) { return static_cast<int>(self.genre_index); }, "texture_index",
        [](BaseBox& self) { return static_cast<int>(self.texture_index); }, "has_recolor", &BaseBox::has_recolor,
        "begin_recolor", &BaseBox::begin_recolor, "end_recolor", &BaseBox::end_recolor);

    lua.new_usertype<SongBox>(
        "SongBox", sol::base_classes, sol::bases<BaseBox>(), "text_subtitle", &SongBox::text_subtitle, "subtitle",
        &SongBox::horizontal_subtitle, "subtitle_large", &SongBox::horizontal_subtitle_large, "bpm_text",
        [](SongBox& self) { return self.bpm_text.get(); }, "is_ura", &SongBox::is_ura, "is_favorite",
        &SongBox::is_favorite, "diff_fade_in", &SongBox::diff_fade_in, "has_ura", &SongBox::has_ura, "ex_data_flag",
        &SongBox::ex_data_flag, "course_info",
        [](SongBox& self, double diff) {
            auto info = self.course_info(static_cast<int>(diff));
            return course_table(info, diff >= 0 && diff < (int)self.scores.size() && self.scores[diff].has_value());
        },
        "course_info_p2",
        [](SongBox& self, double diff) {
            auto info = self.course_info(static_cast<int>(diff));
            info.crown = 0; // Crown::NONE
            info.rank = 0;  // Rank::_NONE
            if (diff >= 0 && diff < std::ssize(self.scores_p2) && self.scores_p2[diff].has_value()) {
                info.crown = static_cast<int>(self.scores_p2[diff]->crown);
                info.rank = static_cast<int>(self.scores_p2[diff]->rank);
            }
            return course_table(info,
                                diff >= 0 && diff < (int)self.scores_p2.size() && self.scores_p2[diff].has_value());
        },
        // Z-DON: box:best_score(diff) -> the player's best score on that course (0 = none)
        "best_score",
        [](SongBox& self, double diff) {
            const int d = static_cast<int>(diff);
            return (d >= 0 && d < std::ssize(self.scores) && self.scores[d].has_value()) ? self.scores[d]->score : 0;
        },
        // Z-DON: box:ai_section_crowns(diff) -> { crown per section, 0 none .. 3 donderful } of the player's AI
        // battles on that course (scores.db zdon_ai_sections)
        "ai_section_crowns",
        [](SongBox& self, double diff, sol::this_state s) {
            sol::state_view lua(s);
            const int d = static_cast<int>(diff);
            const std::vector<int> crowns =
                scores_manager.get_zdon_ai_section_crowns(get_player_id(PlayerNum::P1), self.hash_for(d), d);
            sol::table t = lua.create_table((int)crowns.size(), 0);
            for (size_t i = 0; i < crowns.size(); ++i) {
                t[i + 1] = crowns[i];
            }
            return t;
        },
        // Z-DON: box:chart_stats(diff) -> { notes, length_ms, bpm_min, bpm_max } (box_song_stats.cpp)
        "chart_stats",
        [](SongBox& self, double diff, sol::this_state s) {
            const SongBox::ChartStats& st = self.chart_stats(static_cast<int>(diff));
            sol::state_view lua(s);
            return lua.create_table_with("notes", st.notes, "length_ms", st.length_ms, "bpm_min", st.bpm_min, "bpm_max",
                                         st.bpm_max);
        },
        // Z-DON: box:recent_plays(diff) -> { {score, crown, good, ok, bad, played_at}, ... } oldest first (max 10)
        "recent_plays",
        [](SongBox& self, double diff, sol::this_state s) {
            sol::state_view lua(s);
            const std::vector<PlayRecord>& plays = self.recent_plays(static_cast<int>(diff));
            sol::table t = lua.create_table((int)plays.size(), 0);
            for (size_t i = 0; i < plays.size(); i++) {
                const PlayRecord& p = plays[i];
                t[i + 1] = lua.create_table_with("score", p.score, "crown", p.crown, "good", p.good, "ok", p.ok, "bad",
                                                 p.bad, "played_at", (double)p.played_at);
            }
            return t;
        },
        "has_preimage", [](SongBox& self) { return self.preimage.has_value(); },
        // the song's own image scaled into the rectangle (x, y, width, height)
        "draw_preimage",
        [](SongBox& self, float x, float y, float width, float height, float fade) {
            if (!self.preimage.has_value()) {
                return;
            }
            Rect src{0, 0, static_cast<float>(self.preimage->width), static_cast<float>(self.preimage->height)};
            Rect dest{x, y, width, height};
            draw_texture_pro(self.preimage.value(), src, dest, Vec2{0, 0}, 0, ::fade(Color::WHITE, fade));
        });

    lua.new_usertype<FolderBox>(
        "FolderBox", sol::base_classes, sol::bases<BaseBox>(), "tja_count", sol::readonly(&FolderBox::tja_count),
        "entered", sol::readonly(&FolderBox::entered), "enter_fade",
        [](FolderBox& self) { return self.enter_fade.get(); }, "highest_crown",
        [](FolderBox& self) -> sol::object {
            if (self.crown.empty()) {
                return sol::lua_nil;
            }
            auto it = self.crown.rbegin(); // std::map is sorted -> highest diff
            sol::table t = script_manager.lua->create_table(0, 2);
            t["crown"] = static_cast<int>(it->second);
            t["frame"] = std::min(static_cast<int>(Difficulty::URA), it->first);
            return t;
        },
        "highest_crown_p2",
        [](FolderBox& self) -> sol::object {
            if (self.crown_p2.empty()) {
                return sol::lua_nil;
            }
            auto it = self.crown_p2.rbegin();
            sol::table t = script_manager.lua->create_table(0, 2);
            t["crown"] = static_cast<int>(it->second);
            t["frame"] = std::min(static_cast<int>(Difficulty::URA), it->first);
            return t;
        },
        "explanation",
        [](FolderBox& self) {
            sol::table t = script_manager.lua->create_table(3, 0);
            for (int i = 0; i < 3; i++) {
                t[i + 1] = self.explanation[i];
            }
            return t;
        },
        "kind",
        [](FolderBox& self) -> std::string {
            if (self.genre_index == GenreIndex::DAN) {
                return "dan";
            }
            if (!self.collection.empty()) {
                return "sort";
            }
            return "genre";
        },
        "has_box_texture", [](FolderBox& self) { return self.box_texture.has_value(); }, "draw_box_texture",
        [](FolderBox& self, sol::table params) {
            if (!self.box_texture.has_value()) {
                return;
            }
            float s = tex.screen_scale;
            float scale = params.get_or("scale", 1.0f);
            float x = params.get_or("x", 0.0f);
            float y = params.get_or("y", 0.0f);
            float fade = params.get_or("fade", 1.0f);
            float w = self.box_texture->width;
            float h = self.box_texture->height;
            float dw = w * s * scale;
            float dh = h * s * scale;

            float max_w = params.get_or("max_w", 0.0f);
            float max_h = params.get_or("max_h", 0.0f);
            if (dw > 0.0f && dh > 0.0f && (max_w > 0.0f || max_h > 0.0f)) {
                float fw = (max_w > 0.0f) ? max_w / dw : 1.0f;
                float fh = (max_h > 0.0f) ? max_h / dh : 1.0f;
                float f = std::min(fw, fh);
                if (f < 1.0f) {
                    dw *= f;
                    dh *= f;
                } // shrink only, aspect kept
            }
            sol::optional<float> cx = params["cx"];
            sol::optional<float> cy = params["cy"];
            if (cx) {
                x = *cx - dw / 2.0f;
            }
            if (cy) {
                y = *cy - dh / 2.0f;
            }

            Rect src{0, 0, w, h};
            Rect dest{x, y, dw, dh};
            draw_texture_pro(self.box_texture.value(), src, dest, Vec2{0, 0}, 0, ::fade(Color::WHITE, fade));
        });

    lua.new_usertype<BackBox>("BackBox", sol::base_classes, sol::bases<BaseBox>());
}
