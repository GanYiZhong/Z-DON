#pragma once

#include <future>

#include "../libs/playback_clock.h"
#include "../libs/screen.h"
#include "../libs/video.h"
#include "../objects/game/pause_menu.h"
#include "../objects/game/player.h"
#include "../objects/game/result_transition.h"
#include "../objects/game/song_info.h"
#include "../objects/game/transition.h"
#include "../objects/global/allnet_indicator.h"

class GameScreen : public Screen {
protected:
    GameScreen(const std::string& name) : Screen(name) {}

    PlaybackClock clock;

    void update_gameplay(double current_ms);
    virtual void create_players();
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

    // Z-DON: the pause menu (pause_menu.h); screens that handle Escape themselves turn it off
    PauseMenu pause_menu;
    bool window_was_focused = true;
    virtual bool pause_menu_allowed() const { return true; }
    bool can_pause() const;
    void open_pause_menu();
    // a frame while the menu or its countdown is up (everything else is frozen); back: where Back goes
    std::optional<Screens> update_pause_menu(Screens back);
    // the focus left the window since the last call (not in test runs, whose window is hidden)
    bool focus_lost();

public:
    GameScreen() : Screen("game") {}

    double start_delay = 1000.0;
    bool song_started = false;
    bool score_saved = false;
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

    Screens on_screen_end(Screens next_screen) override;

    void load_hitsounds();

    virtual void initialize_chart(const fs::path& song);

    void start_song(double chart_ms);

    void poll_pending_song();
    void set_players_audio_end();

    void restart_song();

    void toggle_pause();

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

    std::optional<Screens> global_keys();

    void update_background(double current_ms);

    void save_score(const std::string& player_id, PlayerNum player_num);

    std::optional<Screens> update() override;

    void draw_players();

    void draw_overlay(bool with_song_info = true);

    void draw() override;

private:
    void play_song_audio(double position_seconds);

    float paused_audio_time = 0.0f;
    std::future<std::string> pending_song_load;
};
