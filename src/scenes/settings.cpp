#include "settings.h"

#include "../libs/audio.h"
#include "../libs/filesystem.h"
#include "../libs/global_data.h"
#include "../libs/input.h"
#include "../libs/network.h"
#include "../libs/scores.h"
#include "../objects/song_select/file_navigator/navigator.h"

void save_config(const Config& config);

void SettingsScreen::on_screen_start() {
    Screen::on_screen_start();

    if (!global_data.config) {
        spdlog::error("SettingsScreen entered without initialized config");
        screen_init = false;
        throw std::runtime_error("SettingsScreen: global_data.config is null");
    }

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
        box_manager = std::make_unique<SettingsBoxManager>(read_json_file(tmpl_path));
    } catch (const std::exception& e) {
        spdlog::error("Failed to load settings template: {}", e.what());
        screen_init = false;
        throw;
    }
    indicator = Indicator(Indicator::State::SELECT);
    coin_overlay = CoinOverlay();
    allnet_indicator = AllNetIcon();
    username_on_entry = scores_manager.player_1_data.username;

    t_background = tex.get_texture("background/background");
    t_footer = tex.get_texture("background/footer");

    audio.play_sound("bgm", VolumePreset::MUSIC);
    screen_init = true;
}

Screens SettingsScreen::on_screen_end(Screens next_screen) {
    save_config(*global_data.config);
    scores_manager.save_player_data(scores_manager.player_1_data);
    scores_manager.save_player_data(scores_manager.player_2_data);
    spdlog::info("Settings saved");

    if (!global_data.config->network.access_code_1.empty() &&
        scores_manager.player_1_data.username != username_on_entry) {
        network.update_username(global_data.config->network.access_code_1, scores_manager.player_1_data.username);
    }
    if (!global_data.config->network.access_code_2.empty() &&
        scores_manager.player_2_data.username != username_on_entry) {
        network.update_username(global_data.config->network.access_code_2, scores_manager.player_2_data.username);
    }
    username_on_entry.clear();

    box_manager.reset();

    indicator.reset();
    coin_overlay.reset();
    allnet_indicator.reset();
    t_background = nullptr;
    t_footer = nullptr;
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
        audio.play_sound("kat", VolumePreset::SOUND);
        box_manager->move_left();
        if (numeric) {
            hold_dir = -1;
            hold_start = now;
            next_repeat = now + 400.0;
        }
    } else if (is_r_kat_pressed() || (!typing && menu_right_pressed())) {
        audio.play_sound("kat", VolumePreset::SOUND);
        box_manager->move_right();
        if (numeric) {
            hold_dir = 1;
            hold_start = now;
            next_repeat = now + 400.0;
        }
    } else if (is_l_don_pressed() || is_r_don_pressed() || (!typing && menu_confirm_pressed())) {
        audio.play_sound("don", VolumePreset::SOUND);
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
    box_manager->update(current_time);
    if (auto screen = box_manager->pending_screen_change()) {
        return on_screen_end(*screen);
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
