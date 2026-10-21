#include "config.h"

#include <algorithm>
#include <spdlog/spdlog.h>

#include "global_data.h"
#include "ray.h"

// Raylib key codes and their config names (uppercase; getKeyString emits the lowercase form).
static const std::pair<int, const char*> kKeyNames[] = {
    {ray::KEY_SPACE, "SPACE"},
    {ray::KEY_ESCAPE, "ESCAPE"},
    {ray::KEY_ENTER, "ENTER"},
    {ray::KEY_TAB, "TAB"},
    {ray::KEY_BACKSPACE, "BACKSPACE"},
    {ray::KEY_INSERT, "INSERT"},
    {ray::KEY_DELETE, "DELETE"},
    {ray::KEY_RIGHT, "RIGHT"},
    {ray::KEY_LEFT, "LEFT"},
    {ray::KEY_DOWN, "DOWN"},
    {ray::KEY_UP, "UP"},
    {ray::KEY_PAGE_UP, "PAGE_UP"},
    {ray::KEY_PAGE_DOWN, "PAGE_DOWN"},
    {ray::KEY_HOME, "HOME"},
    {ray::KEY_END, "END"},
    {ray::KEY_CAPS_LOCK, "CAPS_LOCK"},
    {ray::KEY_SCROLL_LOCK, "SCROLL_LOCK"},
    {ray::KEY_NUM_LOCK, "NUM_LOCK"},
    {ray::KEY_PRINT_SCREEN, "PRINT_SCREEN"},
    {ray::KEY_PAUSE, "PAUSE"},
    {ray::KEY_F1, "F1"},
    {ray::KEY_F2, "F2"},
    {ray::KEY_F3, "F3"},
    {ray::KEY_F4, "F4"},
    {ray::KEY_F5, "F5"},
    {ray::KEY_F6, "F6"},
    {ray::KEY_F7, "F7"},
    {ray::KEY_F8, "F8"},
    {ray::KEY_F9, "F9"},
    {ray::KEY_F10, "F10"},
    {ray::KEY_F11, "F11"},
    {ray::KEY_F12, "F12"},
    {ray::KEY_LEFT_SHIFT, "LEFT_SHIFT"},
    {ray::KEY_LEFT_CONTROL, "LEFT_CONTROL"},
    {ray::KEY_LEFT_ALT, "LEFT_ALT"},
    {ray::KEY_LEFT_SUPER, "LEFT_SUPER"},
    {ray::KEY_RIGHT_SHIFT, "RIGHT_SHIFT"},
    {ray::KEY_RIGHT_CONTROL, "RIGHT_CONTROL"},
    {ray::KEY_RIGHT_ALT, "RIGHT_ALT"},
    {ray::KEY_RIGHT_SUPER, "RIGHT_SUPER"},
    {ray::KEY_KB_MENU, "KB_MENU"},
    {ray::KEY_KP_0, "KP_0"},
    {ray::KEY_KP_1, "KP_1"},
    {ray::KEY_KP_2, "KP_2"},
    {ray::KEY_KP_3, "KP_3"},
    {ray::KEY_KP_4, "KP_4"},
    {ray::KEY_KP_5, "KP_5"},
    {ray::KEY_KP_6, "KP_6"},
    {ray::KEY_KP_7, "KP_7"},
    {ray::KEY_KP_8, "KP_8"},
    {ray::KEY_KP_9, "KP_9"},
    {ray::KEY_KP_DECIMAL, "KP_DECIMAL"},
    {ray::KEY_KP_DIVIDE, "KP_DIVIDE"},
    {ray::KEY_KP_MULTIPLY, "KP_MULTIPLY"},
    {ray::KEY_KP_SUBTRACT, "KP_SUBTRACT"},
    {ray::KEY_KP_ADD, "KP_ADD"},
    {ray::KEY_KP_ENTER, "KP_ENTER"},
    {ray::KEY_KP_EQUAL, "KP_EQUAL"},
    {ray::KEY_APOSTROPHE, "APOSTROPHE"},
    {ray::KEY_COMMA, "COMMA"},
    {ray::KEY_MINUS, "MINUS"},
    {ray::KEY_PERIOD, "PERIOD"},
    {ray::KEY_SLASH, "SLASH"},
    {ray::KEY_SEMICOLON, "SEMICOLON"},
    {ray::KEY_EQUAL, "EQUAL"},
    {ray::KEY_LEFT_BRACKET, "LEFT_BRACKET"},
    {ray::KEY_BACKSLASH, "BACKSLASH"},
    {ray::KEY_RIGHT_BRACKET, "RIGHT_BRACKET"},
    {ray::KEY_GRAVE, "GRAVE"},
};

std::string getKeyString(int key_code) {
    // Handle alphanumeric keys
    if (key_code >= 65 && key_code <= 90) {
        return std::string(1, static_cast<char>(key_code));
    }
    if (key_code >= 48 && key_code <= 57) {
        return std::string(1, static_cast<char>(key_code));
    }

    // Map raylib key codes to strings
    static const std::map<int, std::string> key_map = [] {
        std::map<int, std::string> m;
        for (const auto& [code, name] : kKeyNames) {
            std::string lower = name;
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            m.emplace(code, std::move(lower));
        }
        return m;
    }();

    auto it = key_map.find(key_code);
    if (it != key_map.end()) {
        return it->second;
    }

    throw std::runtime_error("Unknown key code: " + std::to_string(key_code));
}

static int getKeyCode(const std::string& key) {
    // Unambiguous escaped form for round-tripping key codes that have no
    // known string representation (see getKeyStringSafe).
    if (key.starts_with("code:")) {
        std::string num = key.substr(5);
        try {
            std::size_t pos = 0;
            int code = std::stoi(num, &pos);
            if (pos != num.size()) {
                throw std::invalid_argument("trailing characters");
            }
            return code;
        } catch (...) {
            throw std::runtime_error("Invalid key: " + key);
        }
    }

    // Handle single alphanumeric characters
    if (key.length() == 1 && std::isalnum(static_cast<unsigned char>(key[0]))) {
        return std::toupper(static_cast<unsigned char>(key[0]));
    }

    // Convert to uppercase for comparison
    std::string upper_key = key;
    std::ranges::transform(upper_key, upper_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    // Map strings to raylib key codes
    for (const auto& [code, name] : kKeyNames) {
        if (upper_key == name) {
            return code;
        }
    }

    throw std::runtime_error("Invalid key: " + key);
}

static int getKeyCodeOrDefault(const std::string& v, const char* def) {
    try {
        return getKeyCode(v);
    } catch (const std::runtime_error& e) {
        spdlog::warn("{} -- using {}", e.what(), def);
        try {
            return getKeyCode(def);
        } catch (const std::runtime_error& e2) {
            spdlog::error("Invalid default key '{}': {} -- using escape", def, e2.what());
            return ray::KEY_ESCAPE;
        }
    }
}

static std::vector<int> parseKeyArray(const toml::array& arr) {
    std::vector<int> result;
    for (const auto& elem : arr) {
        if (elem.is_string()) {
            try {
                result.push_back(getKeyCode(elem.as_string()->get()));
            } catch (const std::runtime_error& e) {
                spdlog::warn("Skipping invalid key binding: {}", e.what());
            }
        } else {
            spdlog::warn("Skipping non-string key binding entry");
        }
    }
    return result;
}

std::vector<int> parseIntArray(const toml::array& arr) {
    std::vector<int> result;
    for (const auto& elem : arr) {
        if (auto val = elem.as_integer()) {
            result.push_back(static_cast<int>(val->get()));
        } else {
            spdlog::warn("Skipping non-integer gamepad binding element");
        }
    }
    return result;
}

static std::vector<fs::path> parsePathArray(const toml::array& arr) {
    std::vector<fs::path> result;
    for (const auto& elem : arr) {
        if (elem.is_string()) {
            result.emplace_back(elem.as_string()->get());
        }
    }
    return result;
}

static fs::path config_file_path() {
    return fs::path("config.toml");
}

// Touch input is enabled by default on the mobile platforms (where the touch
// drums are the primary control) and disabled on desktop.
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
static constexpr bool kDefaultTouchInput = true;
#else
static constexpr bool kDefaultTouchInput = false;
#endif

// iOS drives the display from UIKit's animation callback, so vsync must stay on.
#ifdef YATAIDON_PLATFORM_IOS
static constexpr bool kDefaultVsync = true;
#else
static constexpr bool kDefaultVsync = false;
#endif

template<class T, class Parse>
static std::vector<T> parseArrayOrDefault(const toml::table& t, const char* section, const char* key,
                                          std::vector<T> def, Parse parse) {
    if (auto arr = t[section][key].as_array()) {
        return parse(*arr);
    }
    return def;
}

static std::vector<int> parseKeyArrayOrDefault(const toml::table& t, const char* section, const char* key,
                                               std::vector<int> def) {
    return parseArrayOrDefault(t, section, key, std::move(def), parseKeyArray);
}

static std::vector<fs::path> parsePathArrayOrDefault(const toml::table& t, const char* section, const char* key,
                                                     std::vector<fs::path> def) {
    return parseArrayOrDefault(t, section, key, std::move(def), parsePathArray);
}

static std::vector<int> parseIntArrayOrDefault(const toml::table* section, const char* key, std::vector<int> def) {
    if (section) {
        if (auto arr = (*section)[key].as_array()) {
            return parseIntArray(*arr);
        }
    }
    return def;
}

Config get_config() {
    fs::path config_path = config_file_path();

    toml::table config_file;
    try {
        config_file = toml::parse_file(config_path.string());
    } catch (const toml::parse_error& err) {
        std::error_code ec;
        if (!fs::exists(config_path, ec)) {
            spdlog::info("{} not found -- using defaults", config_path.string());
        } else {
            spdlog::error("Failed to parse {}: {} -- using defaults", config_path.string(), err.what());
            // Back up the unparsable file before defaults get written over it by
            // the next save_config(), so the user's real settings aren't lost.
            fs::path backup_path = config_path;
            backup_path += ".bak";
            fs::copy_file(config_path, backup_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                spdlog::error("Failed to back up {}: {}", config_path.string(), ec.message());
                fs::path aside_path = config_path;
                aside_path += ".unparsable";
                fs::rename(config_path, aside_path, ec);
                if (ec) {
                    spdlog::error("Failed to preserve unparsable {}: {}", config_path.string(), ec.message());
                }
            } else {
                spdlog::warn("Backed up unparsable config to {}", backup_path.string());
            }
        }
    }

    Config config{};

    config.general.fps_counter = config_file["general"]["fps_counter"].value_or(false);
    config.general.audio_offset = config_file["general"]["audio_offset"].value_or(0);
    config.general.visual_offset = config_file["general"]["visual_offset"].value_or(0);
    config.general.language = config_file["general"]["language"].value_or("en");
    config.general.song_timer = config_file["general"]["song_timer"].value_or(false);
    config.general.judge_counter = config_file["general"]["judge_counter"].value_or(false);
    config.general.show_timing_offset = config_file["general"]["show_timing_offset"].value_or(false);
    config.general.log_level = config_file["general"]["log_level"].value_or("info");
    config.general.practice_mode_bar_delay = config_file["general"]["practice_mode_bar_delay"].value_or(1);
    config.general.score_method = config_file["general"]["score_method"].value_or("shinuchi");
    config.general.display_bpm = config_file["general"]["display_bpm"].value_or(false);
    config.general.webcam_number = config_file["general"]["webcam_number"].value_or(-1);
    config.general.touch_input = config_file["general"]["touch_input"].value_or(kDefaultTouchInput);
    config.general.skin_updater = config_file["general"]["skin_updater"].value_or(true);
    config.general.fast_transitions = config_file["general"]["fast_transitions"].value_or(false);
    config.general.pause_menu = config_file["general"]["pause_menu"].value_or(true);

    // limits (timer_frozen / song_limit used to live under [general])
    config.limits.timer_frozen =
        config_file["limits"]["timer_frozen"].value_or(config_file["general"]["timer_frozen"].value_or(true));
    config.limits.song_limit =
        config_file["limits"]["song_limit"].value_or(config_file["general"]["song_limit"].value_or(0));
    config.limits.practice_time_limit = config_file["limits"]["practice_time_limit"].value_or(120);

    // Migrate old config: access_code -> access_code_1, player_1_id -> access_code_1, player_2_id -> access_code_2
    std::string old_access_code =
        config_file["network"]["access_code"].value_or(config_file["general"]["access_code"].value_or(""));
    int old_player_1_id = config_file["general"]["player_1_id"].value_or(-1);
    int old_player_2_id = config_file["general"]["player_2_id"].value_or(-1);

    // access_code_1: prefer new field, then old access_code, then convert old player_1_id, else default "0"
    if (auto ac1 = config_file["network"]["access_code_1"].value<std::string>()) {
        config.network.access_code_1 = *ac1;
    } else if (!old_access_code.empty()) {
        config.network.access_code_1 = old_access_code;
    } else if (old_player_1_id > 0) {
        // Map old integer IDs to new string access codes (matching DB migration: 1->"0", 2->"1")
        if (old_player_1_id == 1) {
            config.network.access_code_1 = "0";
        } else if (old_player_1_id == 2) {
            config.network.access_code_1 = "1";
        } else {
            config.network.access_code_1 = std::to_string(old_player_1_id);
        }
    } else {
        // Completely new config: use "0" as default (matches default player "0" in DB)
        config.network.access_code_1 = "0";
    }

    // access_code_2: prefer new field, then convert old player_2_id, else default "1"
    if (auto ac2 = config_file["network"]["access_code_2"].value<std::string>()) {
        config.network.access_code_2 = *ac2;
    } else if (old_player_2_id > 0) {
        // Map old player_2_id to new string access code (old default was 2, maps to "1")
        if (old_player_2_id == 2) {
            config.network.access_code_2 = "1";
        } else {
            config.network.access_code_2 = std::to_string(old_player_2_id);
        }
    } else {
        // Completely new config: use "1" as default (matches default player "1" in DB)
        config.network.access_code_2 = "1";
    }

    // Access codes were 24 digits; the server now uses the last 20.
    for (std::string* code : {&config.network.access_code_1, &config.network.access_code_2}) {
        if (code->size() == 24 && std::ranges::all_of(*code, ::isdigit)) {
            code->erase(0, 4);
        }
    }

    config.network.online_play =
        config_file["network"]["online_play"].value_or(config_file["general"]["online_play"].value_or(false));
    config.network.sync_scores =
        config_file["network"]["sync_scores"].value_or(config_file["general"]["sync_scores_on_launch"].value_or(false));
    config.network.auto_login = config_file["network"]["auto_login"].value_or(true);

    config.card_reader.enabled = config_file["card_reader"]["enabled"].value_or(false);

    // Parse paths
    config.paths.tja_path = parsePathArrayOrDefault(config_file, "paths", "tja_path", {fs::path("Songs")});
    config.paths.skin = fs::path(config_file["paths"]["skin"].value_or(""));

    // Parse keys (converting from strings to key codes)
    config.keys.exit_key = getKeyCodeOrDefault(config_file["keys"]["exit_key"].value_or("Q"), "Q");
    config.keys.fullscreen_key = getKeyCodeOrDefault(config_file["keys"]["fullscreen_key"].value_or("f11"), "f11");
    config.keys.borderless_key = getKeyCodeOrDefault(config_file["keys"]["borderless_key"].value_or("f10"), "f10");
    config.keys.pause_key = getKeyCodeOrDefault(config_file["keys"]["pause_key"].value_or("space"), "space");
    config.keys.back_key = getKeyCodeOrDefault(config_file["keys"]["back_key"].value_or("escape"), "escape");
    config.keys.restart_key = getKeyCodeOrDefault(config_file["keys"]["restart_key"].value_or("f1"), "f1");

    // Parse keys_1p
    config.keys_1p.left_kat = parseKeyArrayOrDefault(config_file, "keys_1p", "left_kat", {ray::KEY_D});
    config.keys_1p.left_don = parseKeyArrayOrDefault(config_file, "keys_1p", "left_don", {ray::KEY_F});
    config.keys_1p.right_don = parseKeyArrayOrDefault(config_file, "keys_1p", "right_don", {ray::KEY_J});
    config.keys_1p.right_kat = parseKeyArrayOrDefault(config_file, "keys_1p", "right_kat", {ray::KEY_K});

    // Parse keys_2p
    config.keys_2p.left_kat = parseKeyArrayOrDefault(config_file, "keys_2p", "left_kat", {ray::KEY_Z});
    config.keys_2p.left_don = parseKeyArrayOrDefault(config_file, "keys_2p", "left_don", {ray::KEY_X});
    config.keys_2p.right_don = parseKeyArrayOrDefault(config_file, "keys_2p", "right_don", {ray::KEY_C});
    config.keys_2p.right_kat = parseKeyArrayOrDefault(config_file, "keys_2p", "right_kat", {ray::KEY_V});

    // Parse gamepad_1p (fallback to legacy [gamepad] if missing)
    const toml::table* gamepad_1p_node = config_file["gamepad_1p"].as_table();
    if (!gamepad_1p_node) {
        gamepad_1p_node = config_file["gamepad"].as_table();
    }
    config.gamepad_1p.left_kat = parseIntArrayOrDefault(gamepad_1p_node, "left_kat", {10});
    config.gamepad_1p.left_don = parseIntArrayOrDefault(gamepad_1p_node, "left_don", {16});
    config.gamepad_1p.right_don = parseIntArrayOrDefault(gamepad_1p_node, "right_don", {17});
    config.gamepad_1p.right_kat = parseIntArrayOrDefault(gamepad_1p_node, "right_kat", {12});

    // Parse gamepad_2p
    const toml::table* gamepad_2p_node = config_file["gamepad_2p"].as_table();
    config.gamepad_2p.left_kat = parseIntArrayOrDefault(gamepad_2p_node, "left_kat", {});
    config.gamepad_2p.left_don = parseIntArrayOrDefault(gamepad_2p_node, "left_don", {});
    config.gamepad_2p.right_don = parseIntArrayOrDefault(gamepad_2p_node, "right_don", {});
    config.gamepad_2p.right_kat = parseIntArrayOrDefault(gamepad_2p_node, "right_kat", {});

    const toml::table* midi_node = config_file["midi"].as_table();
    config.midi.device = midi_node ? (*midi_node)["device"].value_or("") : "";
    config.midi.channel = midi_node ? (*midi_node)["channel"].value_or(0) : 0;
    config.midi.notes = parseIntArrayOrDefault(midi_node, "notes", {});
    config.midi.buttons = parseIntArrayOrDefault(midi_node, "buttons", {});
    if (config.midi.channel < 0 || config.midi.channel > 16) {
        spdlog::warn("MIDI channel must be 0 (any) or 1-16; using any channel");
        config.midi.channel = 0;
    }

    // Parse audio
    config.audio.device_type = config_file["audio"]["device_type"].value_or(0);
    config.audio.device = config_file["audio"]["device"].value_or("");
    config.audio.sample_rate = config_file["audio"]["sample_rate"].value_or(44100);
    config.audio.buffer_size = config_file["audio"]["buffer_size"].value_or(128);
    if (auto asio_channel = config_file["audio"]["asio_channel"].as_array()) {
        config.audio.asio_channel = parseIntArray(*asio_channel);
    }
    if (config.audio.asio_channel.empty()) {
        config.audio.asio_channel.push_back(0);
    }

    // Parse volume
    config.volume.sound = config_file["volume"]["sound"].value_or(1.0);
    config.volume.music = config_file["volume"]["music"].value_or(0.8);
    config.volume.voice = config_file["volume"]["voice"].value_or(1.0);
    config.volume.hitsound = config_file["volume"]["hitsound"].value_or(1.0);
    config.volume.attract_mode = config_file["volume"]["attract_mode"].value_or(1.0);

    // Parse video
    config.video.fullscreen = config_file["video"]["fullscreen"].value_or(false);
    config.video.borderless = config_file["video"]["borderless"].value_or(false);
    config.video.target_fps = config_file["video"]["target_fps"].value_or(-1);
    config.video.vsync = config_file["video"]["vsync"].value_or(kDefaultVsync);

    return config;
}

static std::string getKeyStringSafe(int key_code) {
    try {
        return getKeyString(key_code);
    } catch (const std::runtime_error& e) {
        spdlog::warn("{} -- saving escaped key code {}", e.what(), key_code);
        return "code:" + std::to_string(key_code);
    }
}

static toml::array key_names_array(const std::vector<int>& keys) {
    toml::array arr;
    for (int key : keys) {
        arr.push_back(getKeyStringSafe(key));
    }
    return arr;
}

static toml::array int_array(const std::vector<int>& values) {
    toml::array arr;
    for (int v : values) {
        arr.push_back(v);
    }
    return arr;
}

void save_config(const Config& config) {
    fs::path config_path = config_file_path();

    toml::table config_table;

    // General
    config_table.insert("general", toml::table{{"fps_counter", config.general.fps_counter},
                                               {"audio_offset", config.general.audio_offset},
                                               {"visual_offset", config.general.visual_offset},
                                               {"language", config.general.language},
                                               {"song_timer", config.general.song_timer},
                                               {"judge_counter", config.general.judge_counter},
                                               {"show_timing_offset", config.general.show_timing_offset},
                                               {"log_level", config.general.log_level},
                                               {"practice_mode_bar_delay", config.general.practice_mode_bar_delay},
                                               {"score_method", config.general.score_method},
                                               {"display_bpm", config.general.display_bpm},
                                               {"webcam_number", config.general.webcam_number},
                                               {"touch_input", config.general.touch_input},
                                               {"skin_updater", config.general.skin_updater},
                                               {"fast_transitions", config.general.fast_transitions},
                                               {"pause_menu", config.general.pause_menu}});

    config_table.insert("limits", toml::table{{"timer_frozen", config.limits.timer_frozen},
                                              {"song_limit", config.limits.song_limit},
                                              {"practice_time_limit", config.limits.practice_time_limit}});

    // Network
    config_table.insert("network",
                        toml::table{{"access_code_1", global_data.card_override[0] ? global_data.card_prev_code[0]
                                                                                   : config.network.access_code_1},
                                    {"access_code_2", global_data.card_override[1] ? global_data.card_prev_code[1]
                                                                                   : config.network.access_code_2},
                                    {"online_play", config.network.online_play},
                                    {"sync_scores", config.network.sync_scores},
                                    {"auto_login", config.network.auto_login}});

    config_table.insert("card_reader", toml::table{{"enabled", config.card_reader.enabled}});

    // Paths
    toml::array tja_path_array;
    for (const auto& path : config.paths.tja_path) {
        tja_path_array.push_back(path.string());
    }
    config_table.insert("paths", toml::table{{"tja_path", tja_path_array}, {"skin", config.paths.skin.string()}});

    // Keys
    config_table.insert("keys", toml::table{{"exit_key", getKeyStringSafe(config.keys.exit_key)},
                                            {"fullscreen_key", getKeyStringSafe(config.keys.fullscreen_key)},
                                            {"borderless_key", getKeyStringSafe(config.keys.borderless_key)},
                                            {"pause_key", getKeyStringSafe(config.keys.pause_key)},
                                            {"back_key", getKeyStringSafe(config.keys.back_key)},
                                            {"restart_key", getKeyStringSafe(config.keys.restart_key)}});

    // Keys 1P / 2P
    config_table.insert("keys_1p", toml::table{{"left_kat", key_names_array(config.keys_1p.left_kat)},
                                               {"left_don", key_names_array(config.keys_1p.left_don)},
                                               {"right_don", key_names_array(config.keys_1p.right_don)},
                                               {"right_kat", key_names_array(config.keys_1p.right_kat)}});
    config_table.insert("keys_2p", toml::table{{"left_kat", key_names_array(config.keys_2p.left_kat)},
                                               {"left_don", key_names_array(config.keys_2p.left_don)},
                                               {"right_don", key_names_array(config.keys_2p.right_don)},
                                               {"right_kat", key_names_array(config.keys_2p.right_kat)}});

    config_table.insert("gamepad_1p", toml::table{{"left_kat", int_array(config.gamepad_1p.left_kat)},
                                                  {"left_don", int_array(config.gamepad_1p.left_don)},
                                                  {"right_don", int_array(config.gamepad_1p.right_don)},
                                                  {"right_kat", int_array(config.gamepad_1p.right_kat)}});
    config_table.insert("gamepad_2p", toml::table{{"left_kat", int_array(config.gamepad_2p.left_kat)},
                                                  {"left_don", int_array(config.gamepad_2p.left_don)},
                                                  {"right_don", int_array(config.gamepad_2p.right_don)},
                                                  {"right_kat", int_array(config.gamepad_2p.right_kat)}});

    config_table.insert("midi", toml::table{{"device", config.midi.device},
                                            {"channel", config.midi.channel},
                                            {"notes", int_array(config.midi.notes)},
                                            {"buttons", int_array(config.midi.buttons)}});

    // Audio
    config_table.insert("audio", toml::table{{"device_type", config.audio.device_type},
                                             {"device", config.audio.device},
                                             {"sample_rate", config.audio.sample_rate},
                                             {"buffer_size", config.audio.buffer_size},
                                             {"asio_channel", int_array(config.audio.asio_channel)}});

    // Volume
    config_table.insert("volume", toml::table{{"sound", config.volume.sound},
                                              {"music", config.volume.music},
                                              {"voice", config.volume.voice},
                                              {"hitsound", config.volume.hitsound},
                                              {"attract_mode", config.volume.attract_mode}});

    // Video
    config_table.insert("video", toml::table{{"fullscreen", config.video.fullscreen},
                                             {"borderless", config.video.borderless},
                                             {"target_fps", config.video.target_fps},
                                             {"vsync", config.video.vsync}});

    // main saves at every start (for migrated fields): a file that already holds the same text is
    // left alone (the same text mode read as it is written)
    {
        std::ostringstream rendered;
        rendered << config_table;
        std::ifstream current(config_path);
        if (current) {
            std::ostringstream existing;
            existing << current.rdbuf();
            if (current && existing.str() == rendered.str()) {
                return;
            }
        }
    }

    fs::path tmp_path = config_path;
    tmp_path += ".tmp";
    {
        std::error_code rm_ec;
        std::ofstream ofs(tmp_path, std::ios::trunc);
        if (!ofs.is_open()) {
            spdlog::error("Failed to open {} for writing", tmp_path.string());
            return;
        }
        ofs << config_table;
        ofs.close();
        if (!ofs) {
            spdlog::error("Failed to write {}", tmp_path.string());
            fs::remove(tmp_path, rm_ec);
            return;
        }
    }

    std::error_code ec;
    // std::filesystem::rename replacing an existing destination is only
    // guaranteed on POSIX; on Windows it commonly fails when config_path
    // already exists, so remove it first (best-effort).
    std::error_code pre_rm_ec;
    fs::remove(config_path, pre_rm_ec);
    fs::rename(tmp_path, config_path, ec);
    if (ec) {
        spdlog::error("Failed to save config.toml: {}", ec.message());
        std::error_code rm_ec;
        fs::remove(tmp_path, rm_ec);
    }
}

void ensure_config_file(const Config& config) {
    std::error_code ec;
    if (fs::exists("config.toml", ec)) {
        return;
    }
    spdlog::info("No config file found -- writing defaults to {}", config_file_path().string());
    save_config(config);
}
