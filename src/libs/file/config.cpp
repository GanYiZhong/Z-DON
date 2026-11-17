#include "config.h"

#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>
#include <string_view>

#include "../../platform/platform.h"
#include "../gfx.h"
#include "../gfx/sdl_compat.h"
#include "../global_data.h"

// Key codes are SDL_Scancode values. Letters and digits are handled separately in both directions.
static const std::vector<std::pair<std::string_view, int>> key_names = {{"space", SDL_SCANCODE_SPACE},
                                                                        {"escape", SDL_SCANCODE_ESCAPE},
                                                                        {"enter", SDL_SCANCODE_RETURN},
                                                                        {"tab", SDL_SCANCODE_TAB},
                                                                        {"backspace", SDL_SCANCODE_BACKSPACE},
                                                                        {"insert", SDL_SCANCODE_INSERT},
                                                                        {"delete", SDL_SCANCODE_DELETE},
                                                                        {"right", SDL_SCANCODE_RIGHT},
                                                                        {"left", SDL_SCANCODE_LEFT},
                                                                        {"down", SDL_SCANCODE_DOWN},
                                                                        {"up", SDL_SCANCODE_UP},
                                                                        {"page_up", SDL_SCANCODE_PAGEUP},
                                                                        {"page_down", SDL_SCANCODE_PAGEDOWN},
                                                                        {"home", SDL_SCANCODE_HOME},
                                                                        {"end", SDL_SCANCODE_END},
                                                                        {"caps_lock", SDL_SCANCODE_CAPSLOCK},
                                                                        {"scroll_lock", SDL_SCANCODE_SCROLLLOCK},
                                                                        {"num_lock", SDL_SCANCODE_NUMLOCKCLEAR},
                                                                        {"print_screen", SDL_SCANCODE_PRINTSCREEN},
                                                                        {"pause", SDL_SCANCODE_PAUSE},
                                                                        {"f1", SDL_SCANCODE_F1},
                                                                        {"f2", SDL_SCANCODE_F2},
                                                                        {"f3", SDL_SCANCODE_F3},
                                                                        {"f4", SDL_SCANCODE_F4},
                                                                        {"f5", SDL_SCANCODE_F5},
                                                                        {"f6", SDL_SCANCODE_F6},
                                                                        {"f7", SDL_SCANCODE_F7},
                                                                        {"f8", SDL_SCANCODE_F8},
                                                                        {"f9", SDL_SCANCODE_F9},
                                                                        {"f10", SDL_SCANCODE_F10},
                                                                        {"f11", SDL_SCANCODE_F11},
                                                                        {"f12", SDL_SCANCODE_F12},
                                                                        {"left_shift", SDL_SCANCODE_LSHIFT},
                                                                        {"left_control", SDL_SCANCODE_LCTRL},
                                                                        {"left_alt", SDL_SCANCODE_LALT},
                                                                        {"left_super", SDL_SCANCODE_LGUI},
                                                                        {"right_shift", SDL_SCANCODE_RSHIFT},
                                                                        {"right_control", SDL_SCANCODE_RCTRL},
                                                                        {"right_alt", SDL_SCANCODE_RALT},
                                                                        {"right_super", SDL_SCANCODE_RGUI},
                                                                        {"kb_menu", SDL_SCANCODE_APPLICATION},
                                                                        {"kp_0", SDL_SCANCODE_KP_0},
                                                                        {"kp_1", SDL_SCANCODE_KP_1},
                                                                        {"kp_2", SDL_SCANCODE_KP_2},
                                                                        {"kp_3", SDL_SCANCODE_KP_3},
                                                                        {"kp_4", SDL_SCANCODE_KP_4},
                                                                        {"kp_5", SDL_SCANCODE_KP_5},
                                                                        {"kp_6", SDL_SCANCODE_KP_6},
                                                                        {"kp_7", SDL_SCANCODE_KP_7},
                                                                        {"kp_8", SDL_SCANCODE_KP_8},
                                                                        {"kp_9", SDL_SCANCODE_KP_9},
                                                                        {"kp_decimal", SDL_SCANCODE_KP_PERIOD},
                                                                        {"kp_divide", SDL_SCANCODE_KP_DIVIDE},
                                                                        {"kp_multiply", SDL_SCANCODE_KP_MULTIPLY},
                                                                        {"kp_subtract", SDL_SCANCODE_KP_MINUS},
                                                                        {"kp_add", SDL_SCANCODE_KP_PLUS},
                                                                        {"kp_enter", SDL_SCANCODE_KP_ENTER},
                                                                        {"kp_equal", SDL_SCANCODE_KP_EQUALS},
                                                                        {"apostrophe", SDL_SCANCODE_APOSTROPHE},
                                                                        {"comma", SDL_SCANCODE_COMMA},
                                                                        {"minus", SDL_SCANCODE_MINUS},
                                                                        {"period", SDL_SCANCODE_PERIOD},
                                                                        {"slash", SDL_SCANCODE_SLASH},
                                                                        {"semicolon", SDL_SCANCODE_SEMICOLON},
                                                                        {"equal", SDL_SCANCODE_EQUALS},
                                                                        {"left_bracket", SDL_SCANCODE_LEFTBRACKET},
                                                                        {"backslash", SDL_SCANCODE_BACKSLASH},
                                                                        {"right_bracket", SDL_SCANCODE_RIGHTBRACKET},
                                                                        {"grave", SDL_SCANCODE_GRAVE}};

std::string getKeyString(int key_code) {
    if (key_code >= SDL_SCANCODE_A && key_code <= SDL_SCANCODE_Z) {
        return std::string(1, static_cast<char>('A' + key_code - SDL_SCANCODE_A));
    }
    if (key_code >= SDL_SCANCODE_1 && key_code <= SDL_SCANCODE_9) {
        return std::string(1, static_cast<char>('1' + key_code - SDL_SCANCODE_1));
    }
    if (key_code == SDL_SCANCODE_0) {
        return "0";
    }

    for (const auto& [name, code] : key_names) {
        if (code == key_code) {
            return std::string(name);
        }
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
        const int c = std::toupper(static_cast<unsigned char>(key[0]));
        if (c >= 'A' && c <= 'Z') {
            return SDL_SCANCODE_A + (c - 'A');
        }
        return c == '0' ? SDL_SCANCODE_0 : SDL_SCANCODE_1 + (c - '1');
    }

    std::string lower_key = key;
    std::ranges::transform(lower_key, lower_key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& [name, code] : key_names) {
        if (name == lower_key) {
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
            return SDL_SCANCODE_ESCAPE;
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

#ifdef PLATFORM_ANDROID
static constexpr int kDefaultAudioBufferSize = 256;
#else
static constexpr int kDefaultAudioBufferSize = 128;
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

std::string detect_system_language() {
    static constexpr std::string_view kLanguages[] = {"en", "ja", "zh", "ko"};
    std::vector<std::string> preferred;
#ifdef YATAIDON_USE_SDL2
    if (SDL_Locale* locales = SDL_GetPreferredLocales()) {
        for (SDL_Locale* l = locales; l->language; ++l) {
            preferred.emplace_back(l->language);
        }
        SDL_free(locales);
    }
#else
    int count = 0;
    if (SDL_Locale** locales = SDL_GetPreferredLocales(&count)) {
        for (int i = 0; i < count; ++i) {
            if (locales[i] && locales[i]->language) {
                preferred.emplace_back(locales[i]->language);
            }
        }
        SDL_free(locales);
    }
#endif
    for (std::string lang : preferred) {
        std::ranges::transform(lang, lang.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::ranges::find(kLanguages, lang) != std::end(kLanguages)) {
            spdlog::info("Language: {} (the system's {})", lang, preferred.front());
            return lang;
        }
    }
    spdlog::info("Language: ja (none of the system's {} languages is one the game has)", preferred.size());
    return "ja";
}

void apply_language_choice(Config& config) {
    auto& g = config.general;
    if (g.zdon_language_choice == "auto") {
        g.zdon_language_auto = true;
        g.language = detect_system_language();
    } else if (!g.zdon_language_choice.empty()) {
        g.zdon_language_auto = false;
        g.language = g.zdon_language_choice;
    }
}

Config get_config() {
    fs::path config_path = config_file_path();

    toml::table config_file;
    try {
        std::vector<unsigned char> bytes;
        if (platform_read_file(config_path.string().c_str(), bytes)) {
            config_file = toml::parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                                      config_path.string());
        } else {
            config_file = toml::parse_file(config_path.string());
        }
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
    // Z-DON: a file without a language (a new install) follows the system's
    const auto file_language = config_file["general"]["language"].value<std::string>();
    config.general.zdon_language_auto =
        config_file["general"]["zdon_language_auto"].value_or(!file_language.has_value());
    config.general.language =
        config.general.zdon_language_auto ? detect_system_language() : file_language.value_or("en");
    config.general.zdon_language_choice = config.general.zdon_language_auto ? "auto" : config.general.language;
    config.general.song_timer = config_file["general"]["song_timer"].value_or(false);
    config.general.judge_counter = config_file["general"]["judge_counter"].value_or(false);
    config.general.show_timing_offset = config_file["general"]["show_timing_offset"].value_or(false);
    config.general.zdon_hit_timing = config_file["general"]["zdon_hit_timing"].value_or(true);
    config.general.zdon_song_progress = config_file["general"]["zdon_song_progress"].value_or(true);
    config.general.zdon_hit_timing_show = config_file["general"]["zdon_hit_timing_show"].value_or("half");
    config.general.zdon_sort_filter = config_file["general"]["zdon_sort_filter"].value_or(true);
    config.general.zdon_practice_loop = config_file["general"]["zdon_practice_loop"].value_or(true);
    config.general.zdon_early_hitsound = config_file["general"]["zdon_early_hitsound"].value_or(false);
    config.general.zdon_press_time_judge = config_file["general"]["zdon_press_time_judge"].value_or(false);
    config.general.zdon_branch_by_measure = config_file["general"]["zdon_branch_by_measure"].value_or(false);
    config.general.zdon_lane3d = config_file["general"]["zdon_lane3d"].value_or(false);
    config.general.zdon_hit_error_bar = config_file["general"]["zdon_hit_error_bar"].value_or(false);
    config.general.zdon_new_best_chip = config_file["general"]["zdon_new_best_chip"].value_or(false);
    config.general.zdon_ka_mark = config_file["general"]["zdon_ka_mark"].value_or(false);
    config.general.zdon_hist_auto_open = config_file["general"]["zdon_hist_auto_open"].value_or(false);
    config.general.zdon_target_score = std::clamp(config_file["general"]["zdon_target_score"].value_or(0), 0, 9999999);
    config.general.zdon_remember_difficulty = config_file["general"]["zdon_remember_difficulty"].value_or(false);
    config.general.zdon_lane3d_preset = std::clamp(config_file["general"]["zdon_lane3d_preset"].value_or(0), 0, 3);
    config.general.zdon_lane3d_hint_shown = config_file["general"]["zdon_lane3d_hint_shown"].value_or(false);
    config.general.zdon_coin_mode = config_file["general"]["zdon_coin_mode"].value_or(false);
    config.general.zdon_coin_credits = std::clamp(config_file["general"]["zdon_coin_credits"].value_or(0), 0, 99);
    config.general.zdon_ai_level = std::clamp(config_file["general"]["zdon_ai_level"].value_or(5), 1, 10);
    config.general.zdon_ai_wins = std::max(0, config_file["general"]["zdon_ai_wins"].value_or(0));
    config.general.log_level = config_file["general"]["log_level"].value_or("info");
    config.general.practice_mode_bar_delay = config_file["general"]["practice_mode_bar_delay"].value_or(1);
    config.general.score_method = config_file["general"]["score_method"].value_or("shinuchi");
    config.general.display_bpm = config_file["general"]["display_bpm"].value_or(false);
    config.general.webcam_number = config_file["general"]["webcam_number"].value_or(-1);
    config.general.touch_input = config_file["general"]["touch_input"].value_or(kDefaultTouchInput);
    config.general.touch_drum_size = config_file["general"]["touch_drum_size"].value_or(1.0f);
    config.general.skin_updater = config_file["general"]["skin_updater"].value_or(true);
    config.general.fast_transitions = config_file["general"]["fast_transitions"].value_or(false);
    config.general.pause_menu = config_file["general"]["pause_menu"].value_or(true);
    config.general.ignore_account_appearance = config_file["general"]["ignore_account_appearance"].value_or(true);

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

    // Z-DON: a build with a compiled-in server (NETWORK_URL) plays online out of the box
#if defined(NETWORK_URL)
    constexpr bool kCompiledServer = sizeof(NETWORK_URL) > 1;
#else
    constexpr bool kCompiledServer = false;
#endif
    config.network.online_play =
        config_file["network"]["online_play"].value_or(config_file["general"]["online_play"].value_or(kCompiledServer));
    config.network.sync_scores =
        config_file["network"]["sync_scores"].value_or(config_file["general"]["sync_scores_on_launch"].value_or(false));
    config.network.auto_login = config_file["network"]["auto_login"].value_or(true);
    config.network.guest_mode = config_file["network"]["guest_mode"].value_or(false);
    config.network.zdon_server_url = config_file["network"]["zdon_server_url"].value_or("");
    config.network.zdon_server_key = config_file["network"]["zdon_server_key"].value_or("");
    config.network.zdon_update_url = config_file["network"]["zdon_update_url"].value_or("");
    config.network.zdon_update_check = config_file["network"]["zdon_update_check"].value_or(true);

    config.card_reader.enabled = config_file["card_reader"]["enabled"].value_or(false);

    config.card_reader.backend = config_file["card_reader"]["backend"].value_or("serial");
    config.card_reader.device = config_file["card_reader"]["device"].value_or("");
    config.card_reader.port = config_file["card_reader"]["port"].value_or("/dev/ttyUSB0");
    const int baudrate = config_file["card_reader"]["baudrate"].value_or(38400);
    config.card_reader.baudrate = (baudrate == 38400 || baudrate == 115200) ? baudrate : 38400;

    // Parse paths
    config.paths.tja_path = parsePathArrayOrDefault(config_file, "paths", "tja_path", {fs::path("Songs")});
    config.paths.start_path = fs::path(config_file["paths"]["start_path"].value_or(""));
    config.paths.skin = fs::path(config_file["paths"]["skin"].value_or(""));

    // Parse keys (converting from strings to key codes)
    config.keys.exit_key = getKeyCodeOrDefault(config_file["keys"]["exit_key"].value_or("Q"), "Q");
    config.keys.fullscreen_key = getKeyCodeOrDefault(config_file["keys"]["fullscreen_key"].value_or("f11"), "f11");
    config.keys.borderless_key = getKeyCodeOrDefault(config_file["keys"]["borderless_key"].value_or("f10"), "f10");
    config.keys.pause_key = getKeyCodeOrDefault(config_file["keys"]["pause_key"].value_or("space"), "space");
    config.keys.back_key = getKeyCodeOrDefault(config_file["keys"]["back_key"].value_or("escape"), "escape");
    config.keys.random_song_key = 0;
    if (auto v = config_file["keys"]["random_song_key"].value<std::string>(); v && !v->empty()) {
        try {
            config.keys.random_song_key = getKeyCode(*v);
        } catch (const std::runtime_error& e) {
            spdlog::warn("[keys] random_song_key: {}", e.what());
        }
    }
    config.keys.restart_key = getKeyCodeOrDefault(config_file["keys"]["restart_key"].value_or("f1"), "f1");
    config.keys.gamepad_menu_layout = config_file["keys"]["gamepad_menu_layout"].value_or(false);

    // Parse keys_1p
    config.keys_1p.left_kat = parseKeyArrayOrDefault(config_file, "keys_1p", "left_kat", {SDL_SCANCODE_D});
    config.keys_1p.left_don = parseKeyArrayOrDefault(config_file, "keys_1p", "left_don", {SDL_SCANCODE_F});
    config.keys_1p.right_don = parseKeyArrayOrDefault(config_file, "keys_1p", "right_don", {SDL_SCANCODE_J});
    config.keys_1p.right_kat = parseKeyArrayOrDefault(config_file, "keys_1p", "right_kat", {SDL_SCANCODE_K});

    // Parse keys_2p
    config.keys_2p.left_kat = parseKeyArrayOrDefault(config_file, "keys_2p", "left_kat", {});
    config.keys_2p.left_don = parseKeyArrayOrDefault(config_file, "keys_2p", "left_don", {});
    config.keys_2p.right_don = parseKeyArrayOrDefault(config_file, "keys_2p", "right_don", {});
    config.keys_2p.right_kat = parseKeyArrayOrDefault(config_file, "keys_2p", "right_kat", {});

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

    config.usio.enabled = config_file["usio"]["enabled"].value_or(false);
    config.usio.dll = config_file["usio"]["dll"].value_or(std::string("bnusio.dll"));
    config.usio.threshold = std::clamp(config_file["usio"]["threshold"].value_or(2000), 1, 65535);
    config.usio.holdoff_ms = std::clamp(config_file["usio"]["holdoff_ms"].value_or(25), 0, 500);
    config.usio.poll_us = std::clamp(config_file["usio"]["poll_us"].value_or(1000), 0, 20000);
    config.usio.thresholds = parseIntArrayOrDefault(config_file["usio"].as_table(), "thresholds", {});
    if (!config.usio.thresholds.empty() && config.usio.thresholds.size() != 8) {
        spdlog::warn("[usio] thresholds needs 8 values (P1 left kat, left don, right don, right kat, then P2); "
                     "using threshold for every sensor");
        config.usio.thresholds.clear();
    }
    for (int& t : config.usio.thresholds) {
        t = std::clamp(t, 1, 65535);
    }

    // Parse audio
    config.audio.device_type = config_file["audio"]["device_type"].value_or(0);
    config.audio.device = config_file["audio"]["device"].value_or("");
    config.audio.sample_rate = config_file["audio"]["sample_rate"].value_or(44100);
    config.audio.buffer_size = config_file["audio"]["buffer_size"].value_or(kDefaultAudioBufferSize);
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
    config.video.zdon_texture_compression = config_file["video"]["zdon_texture_compression"].value_or(true);
    config.video.zdon_composited_fullscreen = config_file["video"]["zdon_composited_fullscreen"].value_or(false);
    platform_load_config(config, config_file);

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
    if (zdon_config_read_only) {
        return;
    }
    fs::path config_path = config_file_path();

    toml::table config_table;

    // General
    config_table.insert("general",
                        toml::table{{"fps_counter", config.general.fps_counter},
                                    {"audio_offset", config.general.audio_offset},
                                    {"visual_offset", config.general.visual_offset},
                                    {"language", instance_language()},
                                    {"zdon_language_auto", config.general.zdon_language_auto},
                                    {"song_timer", config.general.song_timer},
                                    {"judge_counter", config.general.judge_counter},
                                    {"show_timing_offset", config.general.show_timing_offset},
                                    {"zdon_hit_timing", config.general.zdon_hit_timing},
                                    {"zdon_song_progress", config.general.zdon_song_progress},
                                    {"zdon_hit_timing_show", config.general.zdon_hit_timing_show},
                                    {"zdon_sort_filter", config.general.zdon_sort_filter},
                                    {"zdon_practice_loop", config.general.zdon_practice_loop},
                                    {"zdon_early_hitsound", config.general.zdon_early_hitsound},
                                    {"zdon_press_time_judge", config.general.zdon_press_time_judge},
                                    {"zdon_branch_by_measure", config.general.zdon_branch_by_measure},
                                    {"zdon_lane3d", config.general.zdon_lane3d},
                                    {"zdon_lane3d_preset", config.general.zdon_lane3d_preset},
                                    {"zdon_hit_error_bar", config.general.zdon_hit_error_bar},
                                    {"zdon_new_best_chip", config.general.zdon_new_best_chip},
                                    {"zdon_ka_mark", config.general.zdon_ka_mark},
                                    {"zdon_hist_auto_open", config.general.zdon_hist_auto_open},
                                    {"zdon_target_score", config.general.zdon_target_score},
                                    {"zdon_remember_difficulty", config.general.zdon_remember_difficulty},
                                    {"zdon_lane3d_hint_shown", config.general.zdon_lane3d_hint_shown},
                                    {"zdon_coin_mode", config.general.zdon_coin_mode},
                                    {"zdon_coin_credits", config.general.zdon_coin_credits},
                                    {"zdon_ai_level", config.general.zdon_ai_level},
                                    {"zdon_ai_wins", config.general.zdon_ai_wins},
                                    {"log_level", config.general.log_level},
                                    {"practice_mode_bar_delay", config.general.practice_mode_bar_delay},
                                    {"score_method", config.general.score_method},
                                    {"display_bpm", config.general.display_bpm},
                                    {"webcam_number", config.general.webcam_number},
                                    {"touch_input", config.general.touch_input},
                                    {"touch_drum_size", config.general.touch_drum_size},
                                    {"skin_updater", config.general.skin_updater},
                                    {"fast_transitions", config.general.fast_transitions},
                                    {"pause_menu", config.general.pause_menu},
                                    {"ignore_account_appearance", config.general.ignore_account_appearance}});

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
                                    {"auto_login", config.network.auto_login},
                                    {"guest_mode", config.network.guest_mode},
                                    {"zdon_server_url", config.network.zdon_server_url},
                                    {"zdon_server_key", config.network.zdon_server_key},
                                    {"zdon_update_url", config.network.zdon_update_url},
                                    {"zdon_update_check", config.network.zdon_update_check}});

    config_table.insert("card_reader", toml::table{{"enabled", config.card_reader.enabled},
                                                   {"backend", config.card_reader.backend},
                                                   {"device", config.card_reader.device},
                                                   {"port", config.card_reader.port},
                                                   {"baudrate", config.card_reader.baudrate}});

    // Paths
    toml::array tja_path_array;
    for (const auto& path : config.paths.tja_path) {
        tja_path_array.push_back(path.string());
    }
    config_table.insert("paths", toml::table{{"tja_path", tja_path_array},
                                             {"start_path", config.paths.start_path.string()},
                                             {"skin", instance_skin()}});

    // Keys
    config_table.insert("keys", toml::table{{"exit_key", getKeyStringSafe(config.keys.exit_key)},
                                            {"fullscreen_key", getKeyStringSafe(config.keys.fullscreen_key)},
                                            {"borderless_key", getKeyStringSafe(config.keys.borderless_key)},
                                            {"pause_key", getKeyStringSafe(config.keys.pause_key)},
                                            {"back_key", getKeyStringSafe(config.keys.back_key)},
                                            {"random_song_key", config.keys.random_song_key != 0
                                                                    ? getKeyStringSafe(config.keys.random_song_key)
                                                                    : std::string()},
                                            {"restart_key", getKeyStringSafe(config.keys.restart_key)},
                                            {"gamepad_menu_layout", config.keys.gamepad_menu_layout}});

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
    toml::table usio_table{{"enabled", config.usio.enabled},
                           {"dll", config.usio.dll},
                           {"threshold", config.usio.threshold},
                           {"holdoff_ms", config.usio.holdoff_ms},
                           {"poll_us", config.usio.poll_us}};
    if (!config.usio.thresholds.empty()) { // only when set: unset, every sensor follows `threshold`
        usio_table.insert("thresholds", int_array(config.usio.thresholds));
    }
    config_table.insert("usio", std::move(usio_table));

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
                                             {"vsync", config.video.vsync},
                                             {"zdon_texture_compression", config.video.zdon_texture_compression},
                                             {"zdon_composited_fullscreen", config.video.zdon_composited_fullscreen}});

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

    platform_save_config(config, config_table);
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
