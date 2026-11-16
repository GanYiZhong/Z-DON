#pragma once
#include "../../libs/file/script.h"

sol::protected_function load_skin_module_fn(const char* module, const char* fn_name);
sol::protected_function cached_skin_module_fn(const char* module, const char* fn_name, const char* global_name);

class MoveAnimation;
float option_panel_offset(const MoveAnimation* move, bool confirmed);

class BaseBox;
class SongSelectPlayer;
class Navigator;
class DiffSortSelect;
class GenreBG;

class SongSelectScript : public LuaScript {
private:
    double clock_offset_ms = 0.0;
    std::optional<double> fast_open_started_ms;

    double animation_ms();

    // Pins the script clock to animation_ms() for the duration of a hook call.
    struct ClockScope {
        std::optional<double> previous = script_manager.current_ms_override;

        explicit ClockScope(double ms) { script_manager.current_ms_override = ms; }
        ~ClockScope() { script_manager.current_ms_override = previous; }
    };

    template<typename... Args>
    void call(sol::protected_function& fn, std::string_view context, Args&&... args) {
        ClockScope scope(animation_ms());

        LuaScript::call(fn, context, std::forward<Args>(args)...);
    }

    // Like call(), for a hook that returns true/false/nothing. Only an explicit `false` declines.
    template<typename... Args>
    bool call_handled(sol::protected_function& fn, std::string_view context, Args&&... args) {
        ClockScope scope(animation_ms());

        auto r = call_r<sol::object>(fn, context, std::forward<Args>(args)...);
        if (!r) {
            return true; // a Lua error already logged: do not draw a second time
        }
        return !(r->template is<bool>() && !r->template as<bool>());
    }

    // A hook the skin may override; when it declines, the default from the skin chain draws instead.
    template<typename... Args>
    bool draw_with_default(sol::protected_function& fn, std::string_view context, sol::protected_function& default_fn,
                           std::string_view default_context, Args&&... args) {
        if (fn.valid() && call_handled(fn, context, args...)) {
            return true;
        }
        call(default_fn, default_context, std::forward<Args>(args)...);
        return false;
    }

    sol::protected_function fn_update;
    sol::protected_function fn_restart_text_fade;
    sol::protected_function fn_draw_footer;
    sol::protected_function fn_draw_overlays;
    sol::protected_function fn_draw_top;
    sol::protected_function fn_draw_song_num;
    sol::protected_function fn_draw_box;
    sol::protected_function fn_draw_box_bg;
    sol::protected_function fn_draw_background;
    sol::protected_function fn_draw_genre_bg;
    sol::protected_function fn_draw_selector;
    sol::protected_function fn_draw_option_panel;
    sol::protected_function fn_draw_sort_window;
    sol::protected_function fn_default_option_panel;
    sol::protected_function fn_default_sort_window;
    sol::protected_function fn_default_selector;
    sol::protected_function fn_default_background_diffs;
    sol::protected_function fn_draw_search_box;
    sol::protected_function fn_default_search_box;
    sol::protected_function fn_draw_background_diffs;

    sol::object box_to_lua(BaseBox* box);

public:
    SongSelectScript();
    void start_folder_open();
    void finish_folder_open();
    void update(double current_ms);
    void restart_text_fade();
    void draw_footer();
    void draw_overlays(int state);
    void draw_top(float dan_progress);
    // true when the skin drew the "N song" chip text itself (the engine text is skipped); false/absent = engine draws
    bool draw_song_num();

    bool draw_box(BaseBox* box);
    bool draw_box_bg(BaseBox* box);
    bool has_box_bg() const { return fn_draw_box_bg.valid(); }
    bool draw_background(Navigator* nav);
    bool draw_genre_bg(GenreBG* genre_bg, float start_pos, float end_pos, BaseBox* folder);
    // true when the skin's own hook drew it (the default player_ui module draws it otherwise)
    bool draw_selector(SongSelectPlayer* player, bool is_half, float fade_in, int pass);
    bool draw_background_diffs(SongSelectPlayer* player);
    // the search overlay: search text and the open animation (bg_resize, fade_in)
    bool draw_search_box(const std::string& search, double bg_resize, double fade_in);
    // true when the skin's own hook drew the panel (the default module draws it otherwise)
    bool draw_option_panel(SongSelectPlayer* player, int kind);

    bool has_sort_window() const { return fn_draw_sort_window.valid(); }
    bool draw_sort_window(DiffSortSelect* window);
};
