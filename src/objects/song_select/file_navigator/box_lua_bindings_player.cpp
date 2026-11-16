#include <algorithm>

#include "../../../libs/file/script.h"
#include "../../../libs/raylib_ns.h"
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

namespace {
    sol::table modifier_rows_table(ModifierSelector& m) {
        sol::table out = script_manager.lua->create_table();
        auto rows = m.lua_rows();
        for (int i = 0; i < static_cast<int>(rows.size()); i++) {
            sol::table r = script_manager.lua->create_table();
            r["name"] = rows[i].name;
            r["value"] = rows[i].value;
            r["label"] = rows[i].label;
            r["state"] = rows[i].state;
            r["changed"] = rows[i].changed;
            r["enabled"] = rows[i].enabled;
            r["greyed"] = rows[i].greyed;
            out[i + 1] = r;
        }
        return out;
    }

    // the same sideways-slide state is exposed by the modifier and the neiro panels
    template<class Selector>
    sol::table change_table(const Selector& sel) {
        sol::table t = script_manager.lua->create_table();
        t["dir"] = sel.lua_change_dir();
        t["fade"] = sel.lua_change_fade();
        t["active"] = sel.lua_change_active();
        t["move"] = sel.lua_change_move();
        t["seq"] = sel.lua_change_seq();
        t["id"] = sel.lua_id();
        t["arrow_fade"] = sel.lua_arrow_fade();
        t["arrow_move"] = sel.lua_arrow_move();
        return t;
    }

    sol::table string_list(const std::vector<std::string>& names) {
        sol::table out = script_manager.lua->create_table();
        for (int i = 0; i < static_cast<int>(names.size()); i++) {
            out[i + 1] = names[i];
        }
        return out;
    }
} // namespace

// the player, its option panels, the navigator, the genre background, text helpers
void register_player_lua_bindings(sol::state& lua) {
    lua.new_usertype<SongSelectPlayer>(
        "SongSelectPlayer", "selected_difficulty",
        [](SongSelectPlayer& self) { return static_cast<int>(self.selected_difficulty); }, "player_num",
        [](SongSelectPlayer& self) { return static_cast<int>(self.player_num); }, "difficulty_decided",
        [](SongSelectPlayer& self) { return self.difficulty_decided(); },
        // state for the cursor drawn under the wheel
        "selector_state",
        [](SongSelectPlayer& self) {
            sol::table t = script_manager.lua->create_table();
            t["selected"] = static_cast<int>(self.selected_difficulty);
            t["prev"] = static_cast<int>(self.prev_diff);
            t["move_right"] = self.diff_select_move_right;
            t["move1"] = self.diff_selector_move_1->attribute;
            t["move1_done"] = self.diff_selector_move_1->is_finished;
            t["move2"] = self.diff_selector_move_2->attribute;
            t["move2_done"] = self.diff_selector_move_2->is_finished;
            t["panel"] = self.neiro_selector.has_value() || self.modifier_selector.has_value();
            auto* song = dynamic_cast<SongBox*>(navigator.get_current_item());
            t["papamama"] = self.solo && song && self.selected_difficulty >= Difficulty::EASY &&
                            song->course_info(static_cast<int>(self.selected_difficulty)).is_papamama;
            return t;
        },
        // state for the difficulty banner behind the wheel
        "bg_diff_state",
        [](SongSelectPlayer& self) {
            sol::table t = script_manager.lua->create_table();
            t["selected"] = static_cast<int>(self.selected_difficulty);
            t["bounce"] = self.selected_diff_bounce->attribute;
            t["fade_in"] = self.selected_diff_fadein->attribute;
            t["highlight_fade"] = self.selected_diff_highlight_fade->attribute;
            t["highlight_on"] =
                self.selected_diff_highlight_fade->is_reversing || self.selected_diff_highlight_fade->is_finished;
            t["text_resize"] = self.selected_diff_text_resize->attribute;
            t["text_fade_in"] = self.selected_diff_text_fadein->attribute;
            t["oni_open_ms"] = self.oni_open_ms; // when this player opened a hidden oni, -1 = never
            return t;
        },
        "neiro_active", [](SongSelectPlayer& self) { return self.neiro_selector.has_value(); }, "modifier_active",
        [](SongSelectPlayer& self) { return self.modifier_selector.has_value(); }, "modifier_offset",
        [](SongSelectPlayer& self) -> sol::optional<float> {
            if (!self.modifier_selector.has_value()) {
                return sol::nullopt;
            }
            const auto& m = self.modifier_selector.value();
            return option_panel_offset(m.move, m.is_confirmed);
        },
        "modifier_rows",
        [](SongSelectPlayer& self) -> sol::object {
            if (!self.modifier_selector.has_value()) {
                return sol::lua_nil;
            }
            return modifier_rows_table(self.modifier_selector.value());
        },
        "modifier_index",
        [](SongSelectPlayer& self) -> sol::optional<int> {
            if (!self.modifier_selector.has_value()) {
                return sol::nullopt;
            }
            return self.modifier_selector.value().lua_index() + 1;
        },
        "modifier_confirmed",
        [](SongSelectPlayer& self) -> sol::optional<bool> {
            if (!self.modifier_selector.has_value()) {
                return sol::nullopt;
            }
            return self.modifier_selector.value().is_confirmed;
        },
        "modifier_change",
        [](SongSelectPlayer& self) -> sol::object {
            if (!self.modifier_selector.has_value()) {
                return sol::lua_nil;
            }
            return change_table(self.modifier_selector.value());
        },
        "neiro_offset",
        [](SongSelectPlayer& self) -> sol::optional<float> {
            if (!self.neiro_selector.has_value()) {
                return sol::nullopt;
            }
            const auto& n = self.neiro_selector.value();
            return option_panel_offset(n.move, n.is_confirmed);
        },
        "neiro_names",
        [](SongSelectPlayer& self) -> sol::object {
            if (!self.neiro_selector.has_value()) {
                return sol::lua_nil;
            }
            return string_list(self.neiro_selector.value().lua_names());
        },
        "neiro_index",
        [](SongSelectPlayer& self) -> sol::optional<int> {
            if (!self.neiro_selector.has_value()) {
                return sol::nullopt;
            }
            return self.neiro_selector.value().lua_index() + 1;
        },
        "neiro_change",
        [](SongSelectPlayer& self) -> sol::object {
            if (!self.neiro_selector.has_value()) {
                return sol::lua_nil;
            }
            return change_table(self.neiro_selector.value());
        },
        // the selectors themselves, for the default option panel (see ModifierPanel / NeiroPanel)
        "modifier_panel",
        [](SongSelectPlayer& self) -> ModifierSelector* {
            return self.modifier_selector.has_value() ? &self.modifier_selector.value() : nullptr;
        },
        "neiro_panel",
        [](SongSelectPlayer& self) -> NeiroSelector* {
            return self.neiro_selector.has_value() ? &self.neiro_selector.value() : nullptr;
        });

    lua.new_usertype<ModifierSelector>(
        "ModifierPanel", "player_num", [](ModifierSelector& m) { return static_cast<int>(m.lua_player()); }, "offset",
        [](ModifierSelector& m) { return option_panel_offset(m.move, m.is_confirmed); }, "rows",
        [](ModifierSelector& m) { return modifier_rows_table(m); }, "index",
        [](ModifierSelector& m) { return m.lua_index() + 1; }, "confirmed",
        [](ModifierSelector& m) { return m.is_confirmed; }, "change",
        [](ModifierSelector& m) { return change_table(m); });

    lua.new_usertype<NeiroSelector>(
        "NeiroPanel", "player_num", [](NeiroSelector& n) { return static_cast<int>(n.lua_player()); }, "offset",
        [](NeiroSelector& n) { return option_panel_offset(n.move, n.is_confirmed); }, "names",
        [](NeiroSelector& n) { return string_list(n.lua_names()); }, "index",
        [](NeiroSelector& n) { return n.lua_index() + 1; }, "change", [](NeiroSelector& n) { return change_table(n); });

    lua.new_usertype<Navigator>(
        "Navigator", "background_move", &Navigator::background_move_anim, "background_fade_change",
        &Navigator::background_fade_anim, "bg_genre_frame", &Navigator::bg_genre_frame, "last_bg_genre_frame",
        &Navigator::last_bg_genre_frame, "current_folder", &Navigator::lua_current_folder,
        // reading either marks the skin as one that animates the folder legs (see swap_is_early)
        "wheel_event", sol::property([](Navigator& n) {
            n.wheel_events_read = true;
            return n.wheel_event;
        }),
        "wheel_event_seq", sol::property([](Navigator& n) {
            n.wheel_events_read = true;
            return n.wheel_event_seq;
        }));

    lua.new_usertype<GenreBG>("GenreBG", "texture_frame", &GenreBG::texture_frame, "name", &GenreBG::name_text,
                              "has_recolor", &GenreBG::has_recolor, "has_gradient", &GenreBG::has_gradient,
                              "gradient_texture", &GenreBG::gradient_texture, "begin_recolor", &GenreBG::begin_recolor,
                              "end_recolor", &GenreBG::end_recolor, "stretch",
                              sol::property([](GenreBG& self) { return self.stretch.get(); }), "scale",
                              sol::property([](GenreBG& self) { return self.scale.get(); }), "move",
                              sol::property([](GenreBG& self) { return self.move.get(); }), "fade",
                              sol::property([](GenreBG& self) { return self.fade.get(); }), "move_left",
                              sol::property([](GenreBG& self) { return self.move_left.get(); }), "move_right",
                              sol::property([](GenreBG& self) { return self.move_right.get(); }), "is_finished",
                              &GenreBG::is_finished, "is_complete", &GenreBG::is_complete);

    // Text-measurement helpers the Lua port needs to lay out folder explanations the way
    // the C++ FolderBox does (word_wrap needs ray::MeasureTextEx, unavailable in Lua).
    lua["text"]["word_wrap"] = [](const std::string& s, double font_size, float spacing, float max_width) {
        return word_wrap(s, static_cast<int>(font_size), spacing, max_width);
    };
    lua["text"]["language_is_cjk"] = [](const std::string& lang) { return language_is_cjk(lang); };
}
