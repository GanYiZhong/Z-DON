#pragma once

#include "game.h"

class Game2PScreen : public GameScreen {
private:
    std::shared_ptr<SongParser> parser_2p;
    bool ai_card_hold = false; // screen option ai_wait_loading_card: the chart clock waits for the loading card
    double ai_black_ms = -1.0; // screen option ai_result_short_black: when the cover went fully black

    void release_chart_resources() override;

    PlayerNum restart_player_num() const override { return PlayerNum::P1; }

    // Z-DON online battle (live): the opponent's hits and position come from the server (zdon_online.h)
    int live_ticket = -1;          // the sync request in flight
    int live_finish_ticket = -1;   // my final result, sent once
    double live_last_sync = -1e9;  // local ms of the last sync sent
    double live_now = 0.0;         // this frame's local ms
    double live_hold_since = -1.0; // local ms the song's end started waiting for the opponent
    void live_update(double current_ms);
    void live_send_finish();
    void draw_live_overlay();

protected:
    // merge 0ed42ec: upstream's virtual with Z-DON's rule as the base (a Z-DON AI battle goes back to the AI
    // song select); upstream's AIGameScreen overrides it
    virtual Screens select_screen() const;
    virtual Screens result_screen() const { return Screens::RESULT_2P; }

public:
    bool two_player_audio() const override { return true; }

    Game2PScreen() : GameScreen("game") {}

    PlayerNum scene_player_num() const override { return PlayerNum::TWO_PLAYER; }

    void initialize_chart(const fs::path& song) override;
    std::optional<Screens> update() override;
    void draw() override;
    Screens on_screen_end(Screens next_screen) override;
    double lane_position(size_t index) const override;
    bool hold_song_end() override;
    bool pause_menu_allowed() const override;
};
