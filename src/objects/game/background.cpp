#include "background.h"

#include "../../libs/audio/audio.h"
#include "../../libs/file/script.h"

Background::Background(PlayerNum player_num, float bpm, const std::string& scene_preset) {
    if (!script_manager.lua) {
        spdlog::error("Background: Lua state is not available");
        return;
    }
    sol::state& lua = *script_manager.lua;

    if (!lua["Background"].valid()) {
        if (!script_manager.has_lua_script("background")) {
            spdlog::error("No background script in this skin or its parent");
            return;
        }
        std::string script_path = script_manager.get_lua_script_path("background");
        auto result = lua.script_file(script_path);
        if (!result.valid()) {
            sol::error err = result;
            spdlog::error("Error loading background.lua: {}", err.what());
            return;
        }
    }

    sol::object bg_obj = lua["Background"];
    if (!bg_obj.is<sol::table>()) {
        spdlog::error("background.lua did not define a Background table");
        return;
    }
    sol::table background_class = bg_obj;
    sol::protected_function new_func = background_class["new"];

    auto call_result = new_func(static_cast<int>(player_num), bpm, scene_preset);
    if (!call_result.valid()) {
        sol::error err = call_result;
        spdlog::error("Error calling Background.new: {}", err.what());
    } else if (call_result.get_type() != sol::type::table) {
        spdlog::error("Background.new did not return a table");
    } else {
        lua_object = call_result;
        fn_update = lua_object["update"];
        fn_handle_good = lua_object["handle_good"];
        fn_handle_ok = lua_object["handle_ok"];
        fn_handle_bad = lua_object["handle_bad"];
        fn_handle_drumroll = lua_object["handle_drumroll"];
        fn_handle_balloon = lua_object["handle_balloon"];
        fn_handle_kusudama = lua_object["handle_kusudama"];
        fn_handle_gauge = lua_object["handle_gauge"];
        fn_handle_song_end = lua_object["handle_song_end"];
        fn_handle_dan = lua_object["handle_dan"];
        fn_handle_skip = lua_object["handle_skip"];
        fn_handle_score = lua_object["handle_score"];
        fn_handle_ending = lua_object["handle_ending"];
        fn_draw_ending = lua_object["draw_ending"];
        fn_draw_back = lua_object["draw_back"];
        fn_draw_fore = lua_object["draw_fore"];
        fn_draw_gauge = lua_object["draw_gauge"];
        fn_handle_balloon_end = lua_object["handle_balloon_end"];
        fn_handle_gogo = lua_object["handle_gogo"];
        fn_handle_miss_streak = lua_object["handle_miss_streak"];
    }
}

Background::~Background() {
    if (!lua_object.valid() || !script_manager.lua) {
        return;
    }
    sol::optional<sol::protected_function> destroy = lua_object["destroy"];
    if (destroy) {
        auto result = destroy.value()(lua_object);
        if (!result.valid()) {
            sol::error err = result;
            spdlog::error("Error calling destroy: {}", err.what());
        }
    }
}

void Background::update(double current_ms, float bpm) {
    invoke(fn_update, "update", current_ms, bpm);
}

void Background::handle_good(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_good, "handle_good", static_cast<int>(player_num));
}

void Background::handle_ok(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_ok, "handle_ok", static_cast<int>(player_num));
}

void Background::handle_bad(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_bad, "handle_bad", static_cast<int>(player_num));
}

void Background::handle_drumroll(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_drumroll, "handle_drumroll", static_cast<int>(player_num));
}

void Background::handle_balloon(PlayerNum player_num, int remaining) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_balloon, "handle_balloon", static_cast<int>(player_num), remaining);
}

// A kusudama hit: skins without handle_kusudama get handle_balloon, as before.
void Background::handle_kusudama(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    if (!fn_handle_kusudama.valid()) {
        handle_balloon(player_num);
        return;
    }
    auto result = fn_handle_kusudama(lua_object, static_cast<int>(player_num));
    if (!result.valid()) {
        sol::error err = result;
        spdlog::error("Error calling handle_kusudama: {}", err.what());
    }
}

void Background::handle_gauge(PlayerNum player_num, float progress, bool is_clear, bool is_rainbow,
                              float clear_progress, float flash) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_gauge, "handle_gauge", static_cast<int>(player_num), progress, is_clear, is_rainbow,
           clear_progress, flash);
}

void Background::handle_song_end(PlayerNum player_num, int good, int ok, int bad, int total_notes) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_song_end, "handle_song_end", static_cast<int>(player_num), good, ok, bad, total_notes);
}

void Background::handle_dan(PlayerNum player_num, const sol::table& state) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_dan, "handle_dan", static_cast<int>(player_num), state);
}

void Background::handle_score(PlayerNum player_num, int score) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_score, "handle_score", static_cast<int>(player_num), score);
}

void Background::handle_ending(PlayerNum player_num, const std::string& kind) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_ending, "handle_ending", static_cast<int>(player_num), kind);
}

void Background::draw_ending(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_draw_ending, "draw_ending", static_cast<int>(player_num));
}

void Background::handle_balloon_end(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_balloon_end, "handle_balloon_end", static_cast<int>(player_num));
}

void Background::handle_skip(PlayerNum player_num, const sol::table& state) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_skip, "handle_skip", static_cast<int>(player_num), state);
}

void Background::draw_back() {
    invoke(fn_draw_back, "draw_back");
}

void Background::draw_fore() {
    invoke(fn_draw_fore, "draw_fore");
}

void Background::draw_gauge(PlayerNum player_num) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_draw_gauge, "draw_gauge", static_cast<int>(player_num));
}

void Background::handle_miss_streak(PlayerNum player_num, bool on) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_miss_streak, "handle_miss_streak", static_cast<int>(player_num), on);
}

void Background::handle_gogo(PlayerNum player_num, bool on) {
    PlayerSoundScope audio_owner(player_num);
    invoke(fn_handle_gogo, "handle_gogo", static_cast<int>(player_num), on);
}
