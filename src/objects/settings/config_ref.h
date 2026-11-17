#pragma once

#include <variant>

#include "../../libs/file/scores.h"
#include "../../libs/global_data.h"

struct ConfigRef {
    std::variant<bool*, int*, float*, std::string*, std::vector<int>*, fs::path*> ptr;

    ConfigRef(bool* p) : ptr(p) {}
    ConfigRef(int* p) : ptr(p) {}
    ConfigRef(float* p) : ptr(p) {}
    ConfigRef(std::string* p) : ptr(p) {}
    ConfigRef(std::vector<int>* p) : ptr(p) {}
    ConfigRef(fs::path* p) : ptr(p) {}

    bool get_bool() const {
        if (auto* p = std::get_if<bool*>(&ptr)) {
            return **p;
        }
        throw std::runtime_error("ConfigRef: not a bool");
    }
    int get_int() const {
        if (auto* p = std::get_if<int*>(&ptr)) {
            return **p;
        }
        throw std::runtime_error("ConfigRef: not an int");
    }
    float get_float() const {
        if (auto* p = std::get_if<float*>(&ptr)) {
            return **p;
        }
        throw std::runtime_error("ConfigRef: not a float");
    }
    std::vector<int> get_vec() const {
        if (auto* p = std::get_if<std::vector<int>*>(&ptr)) {
            return **p;
        }
        throw std::runtime_error("ConfigRef: not a vector<int>");
    }
    std::string get_str() const {
        if (auto* p = std::get_if<fs::path*>(&ptr)) {
            return (*p)->string();
        }
        if (auto* p = std::get_if<std::string*>(&ptr)) {
            return **p;
        }
        throw std::runtime_error("ConfigRef: not a string/path");
    }

    void set_bool(bool v) {
        if (auto* p = std::get_if<bool*>(&ptr)) {
            **p = v;
        } else {
            throw std::runtime_error("ConfigRef: not a bool");
        }
    }
    void set_int(int v) {
        if (auto* p = std::get_if<int*>(&ptr)) {
            **p = v;
        } else {
            throw std::runtime_error("ConfigRef: not an int");
        }
    }
    void set_float(float v) {
        if (auto* p = std::get_if<float*>(&ptr)) {
            **p = v;
        } else {
            throw std::runtime_error("ConfigRef: not a float");
        }
    }
    void set_vec(const std::vector<int>& v) {
        if (auto* p = std::get_if<std::vector<int>*>(&ptr)) {
            **p = v;
        } else {
            throw std::runtime_error("ConfigRef: not a vector<int>");
        }
    }
    void set_str(const std::string& v) {
        if (auto* p = std::get_if<fs::path*>(&ptr)) {
            **p = v;
            return;
        }
        if (auto* p = std::get_if<std::string*>(&ptr)) {
            **p = v;
            return;
        }
        throw std::runtime_error("ConfigRef: not a string/path");
    }

    bool is_int() const { return std::holds_alternative<int*>(ptr); }
};

inline ConfigRef get_config_ref(const std::string& path) {
    Config* c = global_data.config;
    if (!c) {
        throw std::runtime_error("Config not initialized");
    }
    // general
    if (path == "general/fps_counter") {
        return &c->general.fps_counter;
    }
    if (path == "general/audio_offset") {
        return &c->general.audio_offset;
    }
    if (path == "general/visual_offset") {
        return &c->general.visual_offset;
    }
    if (path == "general/language") {
        return &c->general.zdon_language_choice; // Z-DON: "auto" too, applied when the settings close
    }
    if (path == "general/song_timer") {
        return &c->general.song_timer;
    }
    if (path == "general/judge_counter") {
        return &c->general.judge_counter;
    }
    if (path == "general/show_timing_offset") {
        return &c->general.show_timing_offset;
    }
    if (path == "general/zdon_hit_timing") {
        return &c->general.zdon_hit_timing;
    }
    if (path == "general/zdon_song_progress") {
        return &c->general.zdon_song_progress;
    }
    if (path == "general/zdon_hit_timing_show") {
        return &c->general.zdon_hit_timing_show;
    }
    if (path == "general/zdon_sort_filter") {
        return &c->general.zdon_sort_filter;
    }
    if (path == "general/zdon_practice_loop") {
        return &c->general.zdon_practice_loop;
    }
    if (path == "general/zdon_ai_level") {
        return &c->general.zdon_ai_level;
    }
    if (path == "general/practice_mode_bar_delay") {
        return &c->general.practice_mode_bar_delay;
    }
    if (path == "general/log_level") {
        return &c->general.log_level;
    }
    if (path == "general/score_method") {
        return &c->general.score_method;
    }
    if (path == "general/display_bpm") {
        return &c->general.display_bpm;
    }
    if (path == "general/touch_input") {
        return &c->general.touch_input;
    }
    if (path == "general/touch_drum_size") {
        return &c->general.touch_drum_size;
    }
    // limits
    if (path == "limits/timer_frozen") {
        return &c->limits.timer_frozen;
    }
    if (path == "limits/song_limit") {
        return &c->limits.song_limit;
    }
    if (path == "limits/practice_time_limit") {
        return &c->limits.practice_time_limit;
    }
    // paths
    if (path == "paths/skin") {
        return &c->paths.skin;
    }
    // network
    if (path == "network/online_play") {
        return &c->network.online_play;
    }
    if (path == "network/access_code_1") {
        return &c->network.access_code_1;
    }
    if (path == "network/access_code_2") {
        return &c->network.access_code_2;
    }
    if (path == "network/sync_scores") {
        return &c->network.sync_scores;
    }
    if (path == "network/guest_mode") {
        return &c->network.guest_mode;
    }
    // nameplate_1p → player 1 data cache
    if (path.starts_with("nameplate_") && !_scores_manager_ptr) {
        throw std::runtime_error("Scores manager not initialized");
    }
    if (path == "nameplate_1p/name") {
        return &scores_manager.player_1_data.username;
    }
    if (path == "nameplate_1p/title") {
        return &scores_manager.player_1_data.title;
    }
    if (path == "nameplate_1p/title_bg") {
        return &scores_manager.player_1_data.title_bg;
    }
    if (path == "nameplate_1p/dan") {
        return &scores_manager.player_1_data.dan;
    }
    if (path == "nameplate_1p/gold") {
        return &scores_manager.player_1_data.gold;
    }
    if (path == "nameplate_1p/rainbow") {
        return &scores_manager.player_1_data.rainbow;
    }
    // nameplate_2p → player 2 data cache
    if (path == "nameplate_2p/name") {
        return &scores_manager.player_2_data.username;
    }
    if (path == "nameplate_2p/title") {
        return &scores_manager.player_2_data.title;
    }
    if (path == "nameplate_2p/title_bg") {
        return &scores_manager.player_2_data.title_bg;
    }
    if (path == "nameplate_2p/dan") {
        return &scores_manager.player_2_data.dan;
    }
    if (path == "nameplate_2p/gold") {
        return &scores_manager.player_2_data.gold;
    }
    if (path == "nameplate_2p/rainbow") {
        return &scores_manager.player_2_data.rainbow;
    }
    // keys (single ints stored in config)
    if (path == "keys/exit_key") {
        return &c->keys.exit_key;
    }
    if (path == "keys/fullscreen_key") {
        return &c->keys.fullscreen_key;
    }
    if (path == "keys/borderless_key") {
        return &c->keys.borderless_key;
    }
    if (path == "keys/pause_key") {
        return &c->keys.pause_key;
    }
    if (path == "keys/back_key") {
        return &c->keys.back_key;
    }
    if (path == "keys/restart_key") {
        return &c->keys.restart_key;
    }
    if (path == "keys/uncleared_song_key") { // Z-DON
        return &c->keys.uncleared_song_key;
    }
    if (path == "keys/random_song_key") { // Z-DON
        return &c->keys.random_song_key;
    }
    // keys_1p/2p / gamepad (vector<int>)
    if (path == "keys_1p/left_kat") {
        return &c->keys_1p.left_kat;
    }
    if (path == "keys_1p/left_don") {
        return &c->keys_1p.left_don;
    }
    if (path == "keys_1p/right_don") {
        return &c->keys_1p.right_don;
    }
    if (path == "keys_1p/right_kat") {
        return &c->keys_1p.right_kat;
    }
    if (path == "keys_2p/left_kat") {
        return &c->keys_2p.left_kat;
    }
    if (path == "keys_2p/left_don") {
        return &c->keys_2p.left_don;
    }
    if (path == "keys_2p/right_don") {
        return &c->keys_2p.right_don;
    }
    if (path == "keys_2p/right_kat") {
        return &c->keys_2p.right_kat;
    }
    if (path == "gamepad_1p/left_kat") {
        return &c->gamepad_1p.left_kat;
    }
    if (path == "gamepad_1p/left_don") {
        return &c->gamepad_1p.left_don;
    }
    if (path == "gamepad_1p/right_don") {
        return &c->gamepad_1p.right_don;
    }
    if (path == "gamepad_1p/right_kat") {
        return &c->gamepad_1p.right_kat;
    }
    if (path == "gamepad_2p/left_kat") {
        return &c->gamepad_2p.left_kat;
    }
    if (path == "gamepad_2p/left_don") {
        return &c->gamepad_2p.left_don;
    }
    if (path == "gamepad_2p/right_don") {
        return &c->gamepad_2p.right_don;
    }
    if (path == "gamepad_2p/right_kat") {
        return &c->gamepad_2p.right_kat;
    }
    // Z-DON: MIDI (settings' MIDI box)
    if (path == "midi/device") {
        return &c->midi.device;
    }
    if (path == "midi/notes") {
        return &c->midi.notes;
    }
    // Z-DON: the 3D view (settings' 3D box)
    if (path == "general/zdon_lane3d") {
        return &c->general.zdon_lane3d;
    }
    if (path == "general/zdon_remember_difficulty") {
        return &c->general.zdon_remember_difficulty;
    }
    if (path == "general/zdon_practice_metronome") {
        return &c->general.zdon_practice_metronome;
    }
    if (path == "general/zdon_hist_auto_open") {
        return &c->general.zdon_hist_auto_open;
    }
    if (path == "general/zdon_target_score") {
        return &c->general.zdon_target_score;
    }
    if (path == "general/zdon_ka_mark") {
        return &c->general.zdon_ka_mark;
    }
    if (path == "general/zdon_new_best_chip") {
        return &c->general.zdon_new_best_chip;
    }
    if (path == "general/zdon_hit_error_bar") {
        return &c->general.zdon_hit_error_bar;
    }
    if (path == "general/zdon_lane3d_preset") {
        return &c->general.zdon_lane3d_preset;
    }
    // Z-DON: USIO (settings' USIO box)
    if (path == "usio/enabled") {
        return &c->usio.enabled;
    }
    // audio
    if (path == "audio/device_type") {
        return &c->audio.device_type;
    }
    if (path == "audio/device") {
        return &c->audio.device;
    }
    if (path == "audio/sample_rate") {
        return &c->audio.sample_rate;
    }
    if (path == "audio/buffer_size") {
        return &c->audio.buffer_size;
    }
    // volume
    if (path == "volume/sound") {
        return &c->volume.sound;
    }
    if (path == "volume/music") {
        return &c->volume.music;
    }
    if (path == "volume/voice") {
        return &c->volume.voice;
    }
    if (path == "volume/hitsound") {
        return &c->volume.hitsound;
    }
    if (path == "volume/attract_mode") {
        return &c->volume.attract_mode;
    }
    // video
    if (path == "video/fullscreen") {
        return &c->video.fullscreen;
    }
    if (path == "video/borderless") {
        return &c->video.borderless;
    }
    if (path == "video/target_fps") {
        return &c->video.target_fps;
    }
    if (path == "video/vsync") {
        return &c->video.vsync;
    }

    throw std::runtime_error("Unknown config path: " + path);
}
