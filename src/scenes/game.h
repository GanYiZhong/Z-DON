#pragma once

#include <future>
#include <span>

#include "../libs/playback_clock.h"
#include "../libs/screen.h"
#include "../libs/video/video.h"
#include "../objects/game/pause_menu.h"
#include "../objects/game/player.h"
#include "../objects/game/result_transition.h"
#include "../objects/game/song_info.h"
#include "../objects/game/transition.h"
#include "../objects/global/allnet_indicator.h"

struct RemoteScoreSubmission;
struct PlayerData;

class GameScreen : public Screen {
protected:
    GameScreen(const std::string& name) : Screen(name) {}

    PlaybackClock clock;

    void update_gameplay(double current_ms);
    virtual void create_players();
    void apply_personal_best(Player& player);
    virtual float song_playback_rate() const { return 1.0f; }
    enum class AudioLoadMode {
        Sync,
        Async
    };

    enum class BackgroundNotification {
        Enabled,
        Disabled
    };

    void end_song(double end_time, double transition_delay, BackgroundNotification notification);
    void initialize_song_movie();
    void set_session_song_info(PlayerNum player_num);
    void load_song_audio(AudioLoadMode mode);
    double audio_start_ms() const;
    void reset_playback();
    void reset_song_clock(double opening_delay_ms = 0.0);
    void stop_song_audio();
    void unload_song();
    virtual void release_chart_resources() {}
    void pause_playback();
    void resume_playback(std::optional<double> chart_ms = std::nullopt);
    virtual PlayerNum restart_player_num() const { return global_data.player_num; }
    virtual Screens select_screen() const { return Screens::SONG_SELECT; }
    virtual Screens result_screen() const { return Screens::RESULT; }

    // Z-DON: the pause menu (pause_menu.h); screens that handle Escape themselves turn it off
    PauseMenu pause_menu;
    bool pause_used = false; // upstream 5f369cc: a song paused on the way is not saved
    bool window_was_focused = true;
    virtual bool pause_menu_allowed() const { return true; }
    bool can_pause() const;
    void open_pause_menu();
    // a frame while the menu or its countdown is up (everything else is frozen); back: where Back goes
    std::optional<Screens> update_pause_menu(Screens back);
    // the focus left the window since the last call (not in test runs, whose window is hidden)
    bool focus_lost();

public:
    TouchInput touch_input() const override { return TouchInput::DRUM; }
    GameScreen() : Screen("game") {}

    std::span<const std::string> skipped_texture_subsets() const override;

    double start_delay = 1000.0;
    bool song_started = false;
    bool score_saved = false;
    bool ai_music_cut = false;       // Z-DON AI battle: the song audio was cut when the battle ended
    double ai_cover_start_ms = -1.0; // Z-DON Green AI: when the result cover started (skin key zdon_ai_result_hold)
    bool ai_ending_pending = false;  // Z-DON AI battle: the ending anim waits for the skin's battle cut-in
    float bpm = 0.0f;

    std::unique_ptr<VideoPlayer> movie;
    // Z-DON: a background video opens on a worker (container probe + the whole audio track read);
    // movie is set when it is done (adopt_movie), at the latest when the song starts
    std::future<std::unique_ptr<VideoPlayer>> pending_movie;
    void open_movie(const fs::path& path);
    void adopt_movie(bool wait);
    bool has_movie() const { return movie || pending_movie.valid(); }
    std::optional<std::string> song_music;
    std::shared_ptr<SongParser> parser; // shared with the players, not copied
    std::string scene_preset;
    std::vector<std::unique_ptr<Player>> players;
    SongInfo song_info;
    std::optional<Transition> transition;
    ResultTransition result_transition;
    static constexpr double kFastResultTransitionDelayMs = 3000.0;
    static constexpr double kZdonAiCutinMs = 2600.0; // the AI battle cut-in (skin ai_battle.lua END) before the ending
    AllNetIcon allnet_indicator;
    std::optional<Background> background;

    void on_screen_start() override;

    virtual PlayerNum scene_player_num() const { return global_data.player_num; }
    virtual std::string background_scene_preset() const { return scene_preset; }

    virtual Modifiers get_player_modifiers(PlayerNum pn);
    static std::vector<std::string> zdon_unsaved_reasons(const Player& player);
    // Z-DON PB pace: this play's score over the song (saved when it is a new best) and the best play's
    struct ZdonGhost {
        bool loaded = false;
        std::vector<std::pair<float, int>> best, rec;
        int last = -1;
    };
    std::array<ZdonGhost, 4> zdon_ghost; // by PlayerNum
    void zdon_ghost_update();
    void zdon_draw_ghost() const;
    void zdon_draw_hit_error_bar() const; // Z-DON: [general] zdon_hit_error_bar
    void zdon_draw_target() const;        // Z-DON: [general] zdon_target_score

    Screens on_screen_end(Screens next_screen) override;

    void load_hitsounds();

    virtual void initialize_chart(const fs::path& song);

    void start_song(double chart_ms);

    void poll_pending_song();
    void set_players_audio_end();

    void restart_song();

    void toggle_pause(); // Z-DON: [general] pause_menu = false keeps the plain pause on Space

    void resync_song(double current_ms);

    static constexpr int SKIP_HITS = 10;
    PlayerNum skip_lane = PlayerNum::ALL;
    int skip_count = 0;
    int skip_last = -1;
    size_t skip_play_hits = 0;
    bool skipped = false;
    std::optional<std::pair<int, bool>> skip_pushed; // (count, skipped) last given to handle_skip
    std::unique_ptr<OutlinedText> skip_text;
    void init_skip();
    void update_skip();
    void poll_skip();
    void push_skip_state();
    void do_skip();
    void draw_skip();
    // Z-DON sudden death: a player out under the option has their chart cut there (the rest count as 不可); once
    // every player is out the song stops, as a skip does but without the skip banner
    bool zdon_sudden_over = false;
    void zdon_sudden_death_check();

    std::optional<Screens> global_keys();

    void update_background(double current_ms);

    void save_score(const std::string& player_id, PlayerNum player_num);

    std::optional<Screens> update() override;

    virtual void draw_players();
    // Z-DON online battle (live): the chart ms lane i plays at (the opponent's lane follows the opponent), and
    // whether the song's end waits (for the opponent's lane to finish)
    virtual double lane_position(size_t /*index*/) const { return clock.position(); }
    virtual bool hold_song_end() { return false; }

    void draw_overlay(bool with_song_info = true);
    void draw_overlay_hud(bool with_song_info); // Z-DON: draw_overlay's two parts (the 3D view turns the first)
    void draw_overlay_flat();
    void draw_back();             // the movie or the background's back
    void zdon_set_lane3d_pivot(); // Z-DON 3D view: about the judge circle

    void draw() override;

private:
    void reward_score(const Player& player, const RemoteScoreSubmission& submission, const PlayerData& profile);

    void play_song_audio(double position_seconds);

    float paused_audio_time = 0.0f;
    std::future<std::string> pending_song_load;
};
