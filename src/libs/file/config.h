#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <toml++/toml.h>
#include <vector>

namespace fs = std::filesystem;

struct Config;
// Platform overrides applied after config.toml is read, and extra keys written when saving
// (implemented per platform; no-ops on most).
void platform_load_config(Config& config, const toml::table& file);
void platform_save_config(const Config& config, toml::table& out);

struct GeneralConfig {
    bool fps_counter = false;
    int audio_offset = 0;
    int visual_offset = 0;
    std::string language;
    // Z-DON: the language follows the system's (detect_system_language) on every start; `language` keeps
    // what it resolved to, so upstream builds still read a language they know
    bool zdon_language_auto = false;
    // Z-DON: the settings screen's choice, "auto" or a language (apply_language_choice)
    std::string zdon_language_choice;
    bool song_timer = false;
    bool judge_counter = false;
    bool show_timing_offset = false;
    bool zdon_hit_timing = true;    // Z-DON: per-hit early / late data for the skin (marks, result graph)
    bool zdon_song_progress = true; // Z-DON: song position / GOGO sections for the skin's progress bar
    // Z-DON: which hits the skin marks FAST / SLOW: "half" (off by more than half the good window),
    // "ok" (outside the good window), "all" (every hit)
    std::string zdon_hit_timing_show = "half";
    // Z-DON: the difficulty folder's "not cleared / FC / DFC" orders list only those songs (off: those first)
    bool zdon_sort_filter = true;
    bool zdon_practice_loop = true; // Z-DON: practice resumed at a jump mark loops to the next mark
    int zdon_ai_level = 5;          // Z-DON AI battle (zdon_ai.h): the AI's skill 1..10, +1 per match won, -1 per loss
    // Z-DON coin mode (default off): the skin shows the cabinet's credit text (COIN(S) n) instead of the free-play
    // text. There is no coin input: zdon_coin_credits is a plain counter the player edits in config.toml.
    bool zdon_coin_mode = false;
    int zdon_coin_credits = 0;
    int zdon_ai_wins = 0; // Z-DON AI battle: matches won so far
    std::string log_level;
    int practice_mode_bar_delay = 0;
    std::string score_method;
    bool display_bpm = false;
    int webcam_number = -1;
    bool touch_input = false;
    float touch_drum_size = 1.0f;
    bool skin_updater = true;
    bool fast_transitions = false;
    bool pause_menu = true; // Z-DON: Escape / Space open a pause menu during a song (false: Escape leaves)
};

struct LimitsConfig {
    bool timer_frozen = false;
    int song_limit = 0;
    int practice_time_limit = 0; // seconds, 0 = unlimited
};

struct CardReaderConfig {
    bool enabled = true;
    std::string port = "/dev/ttyUSB0";
    int baudrate = 38400;
    int poll_interval_ms = 1000;
};

struct NetworkConfig {
    std::string access_code_1;
    std::string access_code_2;
    bool online_play = false;
    bool sync_scores = false;
    bool auto_login = false;
};

struct PathsConfig {
    std::vector<fs::path> tja_path;
    fs::path skin;
};

struct KeysConfig {
    int exit_key = 0;
    int fullscreen_key = 0;
    int borderless_key = 0;
    int pause_key = 0;
    int back_key = 0;
    int restart_key = 0;
    // Menus use a fixed gamepad layout instead of the drum bindings (see input.cpp)
    bool gamepad_menu_layout = false;
};

// Per-player drum bindings: raylib key codes for keys, button numbers for gamepads.
struct DrumBindings {
    std::vector<int> left_kat;
    std::vector<int> left_don;
    std::vector<int> right_don;
    std::vector<int> right_kat;
};

using Keys1PConfig = DrumBindings;
using Keys2PConfig = DrumBindings;
using GamepadConfig = DrumBindings;

struct MidiConfig {
    // Case-sensitive substring of the MIDI port name; empty disables MIDI.
    std::string device;
    // MIDI channels are exposed to config as 1-16; 0 accepts any channel.
    int channel = 0;
    // Parallel arrays: MIDI note -> gamepad button number.
    std::vector<int> notes;
    std::vector<int> buttons;
};

struct AudioConfig {
    int device_type = 0;
    std::string device; // Output device name (empty = system default)
    int sample_rate = 44100;
    int buffer_size = 512;
    std::vector<int> asio_channel;
};

struct VolumeConfig {
    float global = 1.0f;
    float sound = 1.0f;
    float music = 1.0f;
    float voice = 1.0f;
    float hitsound = 1.0f;
    float attract_mode = 1.0f;
};

struct VideoConfig {
    bool fullscreen = false;
    bool borderless = false;
    int target_fps = 60;
    bool vsync = true;
    bool zdon_texture_compression = true; // Z-DON: skin textures as BC7 (texture_bc7.h)
};

struct Config {
    GeneralConfig general;
    LimitsConfig limits;
    CardReaderConfig card_reader;
    NetworkConfig network;
    PathsConfig paths;
    KeysConfig keys;
    Keys1PConfig keys_1p;
    Keys2PConfig keys_2p;
    GamepadConfig gamepad_1p;
    GamepadConfig gamepad_2p;
    MidiConfig midi;
    AudioConfig audio;
    VolumeConfig volume;
    VideoConfig video;
};

std::string getKeyString(int key_code);

std::vector<int> parseIntArray(const toml::array& arr);

Config get_config();

// Z-DON: the first of the system's preferred languages the game has (en, ja, zh, ko; any Chinese is zh),
// else ja
std::string detect_system_language();

// Z-DON: the settings screen's language choice into the config ("auto": detect it now)
void apply_language_choice(Config& config);

// Z-DON: true while the config in memory differs from the player's on purpose (a recording, zdon_record.h):
// save_config writes nothing
inline bool zdon_config_read_only = false;

void save_config(const Config& config);

// Writes a default config.toml (or dev-config.toml) when none exists yet, so
// the game ships without a bundled config and generates one on first boot.
void ensure_config_file(const Config& config);
