#pragma once

#include "../libs/gfx/text.h"
#include "../libs/screen.h"
#include "../objects/game/song_info.h"
#include "../objects/global/allnet_indicator.h"
#include "../objects/global/coin_overlay.h"
#include "../objects/result/background.h"
#include "../objects/result/fade_in.h"
#include "../objects/result/player.h"
#include "../objects/result/retry_message.h"

class ResultScreen : public Screen {
protected:
    std::string song_title;
    std::string song_subtitle; // empty when the chart hides it
    int song_number = 0;
    FadeAnimation* fade_out = nullptr;
    // after the fade out of a failed run: the revive message, then the retry mini-game
    std::optional<ResultRetryMessage> retry_message;
    bool retrying = false;
    bool allow_retry = true;
    AllNetIcon allnet_indicator;
    CoinOverlay coin_overlay;

    std::optional<ResultBackground> background;
    std::optional<Texture> loading_graphic;
    double skipped_time = 0;
    static constexpr double kFrameMs = 1000.0 / 120.0;
    static constexpr double kEnableSkipMs = 100 * kFrameMs;
    static constexpr double kWaitEffectEndMs = 500 * kFrameMs;
    static constexpr double kWaitNextSceneMs = 500 * kFrameMs;
    static constexpr double kFastInputDelayMs = 250.0;
    static constexpr double kAutoNextSceneMs = 3600 * kFrameMs;
    double skip_enabled_ms = 0;
    std::optional<ResultPlayer> player_1;
    std::optional<FadeIn> fade_in;

    void handle_input(double current_ms);

    bool retry_pending() const;
    bool exiting() const;
    void begin_exit();

    virtual double reveal_end_ms();
    void update_input_and_timeout(double current_ms);

    void draw_overlay();
    void draw_unsaved_note() const; // Z-DON
    bool zdon_hist_open[3] = {};    // Z-DON: the hit timing panel, by player
    double zdon_play_all_ms = 0;    // Z-DON AI battle: when the play-all summary came up (0 = not yet)
    bool zdon_play_all_pending() const;
    bool zdon_hist_input(); // true: the input went to an AI battle cut-in page
    void zdon_draw_hist() const;
    virtual bool zdon_two_boards() const { return false; } // Z-DON: Result2PScreen

    void draw_song_info();

public:
    ResultScreen() : Screen("result") {}

    void on_screen_start() override;

    std::optional<Screens> update() override;

    Screens on_screen_end(Screens next_screen) override;

    void draw() override;
};
