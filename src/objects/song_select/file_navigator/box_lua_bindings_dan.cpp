#include <algorithm>

#include "../../../libs/script.h"
#include "../../game/exam_caption.h"
#include "../diff_sort.h"
#include "../player.h"
#include "../song_select_script.h"
#include "box_back.h"
#include "box_dan.h"
#include "box_folder.h"
#include "box_lua_bindings.h"
#include "box_song.h"
#include "genre_bg.h"
#include "navigator.h"
#include "text_layout.h"

// DanBox, the sort window
void register_dan_box_lua_bindings(sol::state& lua) {
    lua.new_usertype<DanBox>(
        "DanBox", sol::base_classes, sol::bases<BaseBox>(), "dan_title", sol::readonly(&DanBox::dan_title), "dan_color",
        sol::readonly(&DanBox::dan_color), "dan_rank", sol::readonly(&DanBox::dan_rank), "clear_mark",
        sol::readonly(&DanBox::clear_mark), "record_rank", sol::readonly(&DanBox::record_rank),
        // the recorded best for exam i (course-wide) / for song j of a per-song exam;
        // nil when there is none, or when it was recorded for a different exam type
        "exam_best",
        [](DanBox& self, double i) -> sol::optional<int> {
            auto k = static_cast<size_t>(i);
            if (k >= self.exam_bests.size() || k >= self.exams.size()) {
                return sol::nullopt;
            }
            const DanExamBest& b = self.exam_bests[k];
            if (b.type != self.exams[k].type || b.value < 0) {
                return sol::nullopt;
            }
            return b.value;
        },
        "exam_song_best",
        [](DanBox& self, double i, double j) -> sol::optional<int> {
            auto k = static_cast<size_t>(i);
            int s = static_cast<int>(j);
            if (k >= self.exam_bests.size() || k >= self.exams.size() || s < 0 || s > 2) {
                return sol::nullopt;
            }
            const DanExamBest& b = self.exam_bests[k];
            if (b.type != self.exams[k].type || b.song[s] < 0) {
                return sol::nullopt;
            }
            return b.song[s];
        },
        "dan_index", sol::readonly(&DanBox::dan_index), "gaiden", sol::readonly(&DanBox::gaiden), "is_back",
        sol::readonly(&DanBox::is_back), "total_notes", sol::readonly(&DanBox::total_notes), "song_masked",
        [](DanBox& self, double i) {
            auto k = static_cast<size_t>(i);
            return k < self.song_masked.size() && self.song_masked[k];
        },
        "song_count", [](DanBox& self) { return (int)self.songs.size(); }, "song_genre",
        [](DanBox& self, double i) { return self.songs[static_cast<int>(i)].genre_index; }, "song_difficulty",
        [](DanBox& self, double i) { return self.songs[static_cast<int>(i)].difficulty; }, "song_level",
        [](DanBox& self, double i) { return self.songs[static_cast<int>(i)].level; }, "chip_name",
        [](DanBox& self) { return self.name_text(); }, "hori_name", [](DanBox& self) { return self.hori_name.get(); },
        "song_title_text",
        [](DanBox& self, double i) {
            int idx = static_cast<int>(i);
            return (idx >= 0 && idx < (int)self.song_texts.size()) ? self.song_texts[idx].first.get() : nullptr;
        },
        "song_subtitle_text",
        [](DanBox& self, double i) {
            int idx = static_cast<int>(i);
            return (idx >= 0 && idx < (int)self.song_texts.size()) ? self.song_texts[idx].second.get() : nullptr;
        },
        "exam_count", [](DanBox& self) { return (int)self.exams.size(); }, "exam_type",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].type; }, "exam_red",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].red; }, "exam_gold",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].gold; }, "exam_range",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].range; }, "exam_gothrough",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].gothrough; }, "exam_per_song",
        [](DanBox& self, double i) { return self.exams[static_cast<int>(i)].per_song(); }, "exam_song_count",
        [](DanBox& self, double i) { return (int)self.exams[static_cast<int>(i)].song_red.size(); }, "exam_song_red",
        [](DanBox& self, double i, double j) { return self.exams[static_cast<int>(i)].song_red[static_cast<int>(j)]; },
        "exam_song_gold",
        [](DanBox& self, double i, double j) {
            const Exam& e = self.exams[static_cast<int>(i)];
            int idx = static_cast<int>(j);
            return idx < (int)e.song_gold.size() ? e.song_gold[idx] : e.song_red[idx];
        },
        "exam_caption",
        [](DanBox& self, double i) {
            return exam_border_text(tex, self.exams[static_cast<int>(i)], global_data.config->general.language);
        },
        "exam_song_caption",
        [](DanBox& self, double i, double j) {
            return exam_border_text(tex, self.exams[static_cast<int>(i)].for_song(static_cast<int>(j)),
                                    global_data.config->general.language);
        });

    lua.new_usertype<DiffSortSelect>(
        "SortWindow", "session", [](DiffSortSelect& s) { return s.lua_session(); }, "diff",
        [](DiffSortSelect& s) { return s.lua_diff(); }, "star", [](DiffSortSelect& s) { return s.lua_star(); }, "order",
        [](DiffSortSelect& s) { return s.lua_order(); }, "song_num", [](DiffSortSelect& s) { return s.lua_song_num(); },
        "phase", [](DiffSortSelect& s) { return s.lua_phase(); }, "alpha",
        [](DiffSortSelect& s) { return s.lua_alpha(); }, "arrow_offset",
        [](DiffSortSelect& s, double row) { return s.lua_arrow_offset(static_cast<int>(row)); },
        // classic (non one-menu) window
        "one_menu", [](DiffSortSelect& s) { return s.lua_one_menu(); }, "box",
        [](DiffSortSelect& s) { return s.lua_box(); }, "level", [](DiffSortSelect& s) { return s.lua_level(); },
        "in_level", [](DiffSortSelect& s) { return s.lua_in_level(); }, "confirmation",
        [](DiffSortSelect& s) { return s.lua_confirmation(); }, "confirm_index",
        [](DiffSortSelect& s) { return s.lua_confirm_index(); }, "num_boxes",
        [](DiffSortSelect& s) { return s.lua_num_boxes(); }, "prev_diff",
        [](DiffSortSelect& s) { return s.lua_prev_diff(); }, "prev_level",
        [](DiffSortSelect& s) { return s.lua_prev_level(); }, "limit",
        [](DiffSortSelect& s, double box) { return s.lua_limit(static_cast<int>(box)); },
        // total, clears, full combos of a course at one star level
        "stats",
        [](DiffSortSelect& s, double diff, double level) {
            CourseStats c = s.lua_stats(static_cast<int>(diff), static_cast<int>(level));
            sol::table t = script_manager.lua->create_table();
            t["total"] = c.total;
            t["clears"] = c.clears;
            t["full_combos"] = c.full_combos;
            return t;
        },
        // the same, summed over every level of a course
        "sums",
        [](DiffSortSelect& s, double diff) {
            auto c = s.lua_sums(static_cast<int>(diff));
            sol::table t = script_manager.lua->create_table();
            t["total"] = c[0];
            t["clears"] = c[1];
            t["full_combos"] = c[2];
            return t;
        },
        "anim",
        [](DiffSortSelect& s) {
            auto a = s.lua_anim();
            sol::table t = script_manager.lua->create_table();
            t["bg_resize"] = a[0];
            t["fade_in"] = a[1];
            t["flicker"] = a[2];
            t["bounce"] = a[3];
            t["arrow_fade"] = a[4];
            t["arrow_move"] = a[5];
            return t;
        });
}
