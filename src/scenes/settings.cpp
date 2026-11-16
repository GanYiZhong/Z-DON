#include "settings.h"

#include "../libs/audio/audio.h"
#include "../libs/file/filesystem.h"
#include "../libs/file/scores.h"
#include "../libs/gfx/camera_utils.h"
#include "../libs/global_data.h"
#include "../libs/input/input.h"
#include "../libs/network/network.h"
#include "../objects/settings/option_box.h"
#include "../objects/song_select/file_navigator/navigator.h"

#if defined(YATAIDON_HAS_MIDI)
// Z-DON: the MIDI box (device, learn the drum) is the engine's: skins' templates do not know it. Added before "exit"
// unless the template already has a "midi" box.
static void zdon_add_midi_box(rapidjson::Document& tmpl) {
    if (!tmpl.IsObject() || tmpl.HasMember("midi")) {
        return;
    }
    auto& a = tmpl.GetAllocator();
    auto text = [&](const std::string& key) {
        rapidjson::Value v(rapidjson::kObjectType);
        v.AddMember("en", rapidjson::Value(settings_builtin_text(key).c_str(), a), a);
        return v;
    };
    auto option = [&](const char* path, const char* type, const std::string& name, const std::string& desc) {
        rapidjson::Value o(rapidjson::kObjectType);
        o.AddMember("path", rapidjson::Value(path, a), a);
        o.AddMember("type", rapidjson::Value(type, a), a);
        o.AddMember("name", text(name), a);
        o.AddMember("description", text(desc), a);
        return o;
    };
    rapidjson::Value options(rapidjson::kObjectType);
    options.AddMember("device", option("midi/device", "zdon_mididevice", "midi_device", "midi_device_desc"), a);
    options.AddMember("learn", option("midi/notes", "zdon_midilearn", "midi_learn", "midi_learn_desc"), a);
    rapidjson::Value box(rapidjson::kObjectType);
    box.AddMember("name", text("midi_box"), a);
    box.AddMember("options", options, a);

    rapidjson::Value rebuilt(rapidjson::kObjectType);
    bool added = false;
    for (auto it = tmpl.MemberBegin(); it != tmpl.MemberEnd(); ++it) {
        if (!added && std::string_view(it->name.GetString()) == "exit") {
            rebuilt.AddMember("midi", box, a);
            added = true;
        }
        rebuilt.AddMember(rapidjson::Value(it->name, a), rapidjson::Value(it->value, a), a);
    }
    if (!added) {
        rebuilt.AddMember("midi", box, a);
    }
    tmpl.SetObject();
    for (auto it = rebuilt.MemberBegin(); it != rebuilt.MemberEnd(); ++it) {
        tmpl.AddMember(rapidjson::Value(it->name, a), rapidjson::Value(it->value, a), a);
    }
}
#endif

void SettingsScreen::on_screen_start() {
    Screen::on_screen_start();

    if (!global_data.config) {
        spdlog::error("SettingsScreen entered without initialized config");
        screen_init = false;
        throw std::runtime_error("SettingsScreen: global_data.config is null");
    }

    // Z-DON: the language row shows "auto" or the language (config_ref general/language)
    global_data.config->general.zdon_language_choice =
        global_data.config->general.zdon_language_auto ? "auto" : global_data.config->general.language;

    fs::path skin_dir = fs::path("Skins") / global_data.config->paths.skin / "Graphics";
    fs::path tmpl_path = skin_dir / "settings_template.json";
    if (!fs::exists(tmpl_path)) {
        for (const fs::path& root : tex.all_graphics_roots()) {
            if (fs::exists(root / "settings_template.json")) {
                tmpl_path = root / "settings_template.json";
                break;
            }
        }
    }

    try {
        rapidjson::Document tmpl = read_json_file(tmpl_path);
#if defined(YATAIDON_HAS_MIDI)
        zdon_add_midi_box(tmpl); // Z-DON: a MIDI box before Exit, whatever the skin's template has
#endif
        box_manager = std::make_unique<SettingsBoxManager>(tmpl);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load settings template: {}", e.what());
        screen_init = false;
        throw;
    }
    indicator = Indicator(Indicator::State::SELECT);
    coin_overlay = CoinOverlay();
    allnet_indicator = AllNetIcon();
    accounts_on_entry = {
        AccountOnEntry{global_data.config->network.access_code_1, scores_manager.player_1_data.username},
        AccountOnEntry{global_data.config->network.access_code_2, scores_manager.player_2_data.username}};
    set_touch_drum_enabled(false);
    take_touch_tap();
    show_cursor(); // the access code boxes reveal on mouse hover
    assets_on_entry.emplace(*global_data.config);

    t_background = tex.get_texture("background/background");
    t_footer = tex.get_texture("background/footer");

    audio.play_sound("bgm", VolumePreset::MUSIC);
    screen_init = true;
}

Screens SettingsScreen::on_screen_end(Screens next_screen) {
    apply_language_choice(*global_data.config); // Z-DON
    set_touch_drum_enabled(true);
    save_config(*global_data.config);
    scores_manager.save_player_data(scores_manager.player_1_data);
    scores_manager.save_player_data(scores_manager.player_2_data);
    spdlog::info("Settings saved");

    hide_cursor();

    // A switched account's name is not an edit; pushing it could overwrite the server's newer one.
    const std::array<std::pair<const std::string*, const PlayerData*>, 2> accounts{
        std::pair{&global_data.config->network.access_code_1, &scores_manager.player_1_data},
        std::pair{&global_data.config->network.access_code_2, &scores_manager.player_2_data}};
    for (size_t i = 0; i < accounts.size(); i++) {
        const auto& [code, data] = accounts[i];
        if (!code->empty() && *code == accounts_on_entry[i].code && data->username != accounts_on_entry[i].username) {
            network.update_username(*code, data->username);
        }
    }

    box_manager.reset();

    indicator.reset();
    coin_overlay.reset();
    allnet_indicator.reset();
    t_background = nullptr;
    t_footer = nullptr;
    if (assets_on_entry && !assets_on_entry->needs_reload(*global_data.config)) {
        return Screen::on_screen_end(next_screen);
    }
    drop_other_screens_for_skin_reload();
    // navigator is a global, not a Screen -- the rebuild above never
    // touches it, so it needs its own reset.
    navigator.reset_for_skin_reload();

    unload_skin();
    load_skin();

    reload_skin_screens();

    return Screen::on_screen_end(next_screen);
}

std::optional<Screens> SettingsScreen::handle_input() {
    if (!box_manager->editing() && check_key_pressed(global_data.config->keys.back_key)) {
        return on_screen_end(Screens::ENTRY);
    }
    if (key_pressed(SDL_SCANCODE_F1)) {
        return on_screen_end(Screens::INPUT_CALI);
    }
    if (key_pressed(SDL_SCANCODE_F3)) {
        return on_screen_end(Screens::INPUT_TEST);
    }
    const double now = get_current_ms();
    if (box_manager->take_capture_done()) {
        // The press was bound by the option's update() this frame; the drum
        // path may report it now or a frame later, so flush for a moment.
        input_guard_until = now + 200.0;
    }
    if (now < input_guard_until) {
        clear_input_buffers();
        hold_dir = 0;
        return std::nullopt;
    }
    const bool typing = box_manager->editing_text();
    if (const auto tap = take_touch_tap(); tap && !typing) {
        const Vec2 pixel{tap->x * get_screen_width(), tap->y * get_screen_height()};
        const Vec2 world = get_screen_to_world_2d(pixel, compute_camera2d(tex.screen_width, tex.screen_height));
        audio.play_sound("don", VolumePreset::HITSOUND);
        if (box_manager->tap(world)) {
            return on_screen_end(Screens::ENTRY);
        }
        return std::nullopt;
    }
    const bool numeric = box_manager->adjusting_number();
    if (numeric && hold_dir != 0) {
        // Single-kat hold repeats the step: 400 ms delay, then 100 ms,
        // 50 ms after 1.5 s and 20 ms after 3 s of holding.
        bool held = hold_dir < 0 ? (is_kat_held(true) || key_down(SDL_SCANCODE_LEFT))
                                 : (is_kat_held(false) || key_down(SDL_SCANCODE_RIGHT));
        if (!held) {
            hold_dir = 0;
        } else if (now >= next_repeat) {
            double held_ms = now - hold_start;
            next_repeat = now + (held_ms < 1500.0 ? 100.0 : held_ms < 3000.0 ? 50.0 : 20.0);
            if (hold_dir < 0) {
                box_manager->move_left();
            } else {
                box_manager->move_right();
            }
        }
    } else {
        hold_dir = 0;
    }
    if (is_l_kat_pressed() || (!typing && menu_left_pressed())) {
        audio.play_sound("kat", VolumePreset::HITSOUND);
        box_manager->move_left();
        if (numeric) {
            hold_dir = -1;
            hold_start = now;
            next_repeat = now + 400.0;
        }
    } else if (is_r_kat_pressed() || (!typing && menu_right_pressed())) {
        audio.play_sound("kat", VolumePreset::HITSOUND);
        box_manager->move_right();
        if (numeric) {
            hold_dir = 1;
            hold_start = now;
            next_repeat = now + 400.0;
        }
    } else if (is_l_don_pressed() || is_r_don_pressed() || (!typing && menu_confirm_pressed())) {
        audio.play_sound("don", VolumePreset::HITSOUND);
        bool result = box_manager->select_box();
        if (result) {
            return on_screen_end(Screens::ENTRY);
        }
    }
    return std::nullopt;
}

std::optional<Screens> SettingsScreen::update() {
    if (auto result = Screen::update()) {
        return result;
    }
    if (!screen_init) {
        return std::nullopt;
    }
    double current_time = get_current_ms();
    allnet_indicator->update(current_time);
    indicator->update(current_time);
    const bool was_editing_text = box_manager->editing_text();
    box_manager->update(current_time);
    if (auto screen = box_manager->pending_screen_change()) {
        return on_screen_end(*screen);
    }
    // The field consumes Return/Escape during update(). Keep that entire frame
    // in text-edit mode so the same key cannot reopen it or leave settings.
    if (was_editing_text || box_manager->editing_text()) {
        clear_input_buffers();
        return std::nullopt;
    }
    return handle_input();
}

void SettingsScreen::draw() {
    tex.draw_texture(t_background);
    box_manager->draw();
    tex.draw_texture(t_footer);
    indicator->draw_on("song_select");
    coin_overlay->draw();
    allnet_indicator->draw();
}
