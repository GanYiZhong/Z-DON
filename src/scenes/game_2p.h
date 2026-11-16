#pragma once

#include <memory>
#include <vector>

#include "../libs/network/zdon_p2p.h"
#include "../libs/network/zdon_ws.h"
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
    // the WebSocket (pushed both ways the moment a hit is played); /match/sync polling without one
    std::unique_ptr<ZdonWs> live_ws;
    bool live_ws_off = false;     // none this song (turned off, refused, or dropped): polling
    bool live_ws_used = false;    // it was open this song (for the log)
    size_t live_ws_next = 0;      // my next hit to send on it
    double live_ws_last = -1e9;   // local ms of the last message on it
    std::vector<double> live_lag; // how late each of the opponent's hits came (ms), for the log
    // the start: the song waits until both games have it loaded (the server's "go"), so both start together
    bool live_wait_go = false;
    bool live_go_hold = false;     // the clock is held for it
    double live_wait_since = 0.0;  // local ms the wait began
    bool live_loaded_sent = false; // "loaded" went over the WebSocket
    // starting at the same instant: the server's "go" names a time on its clock; the clock differences come from
    // pings (the server's over the WebSocket; the opponent's over the direct connection, where the host then gives
    // the guest the start time on the guest's own clock)
    double live_go_at = -1.0;       // the server's start time (its wall ms), -1 = none
    double live_start_local = -1.0; // the start on my wall clock, once known exactly (-1 = not)
    double live_srv_off = 0.0;      // server clock - mine
    double live_srv_rtt = 1e9;      // the ping that measured it (the shortest)
    double live_peer_off = 0.0;     // opponent's clock - mine (the host measures it)
    double live_peer_rtt = 1e9;
    int live_pings = 0, live_peer_pings = 0, live_start_sent = 0;
    double live_ping_last = -1e9, live_peer_ping_last = -1e9;
    std::string live_start_how; // for the log
    // a direct UDP connection to the opponent (zdon_p2p.h), next to the WebSocket: hits go both ways, the first in
    // is taken
    std::unique_ptr<ZdonP2P> live_p2p;
    bool live_cand_sent = false; // my addresses went to the server
    size_t live_p2p_ack = 0;     // my hits the opponent has said it holds (direct)
    double live_p2p_last = -1e9; // local ms of the last direct message
    size_t live_p2p_sent = 0;    // my hits sent directly so far
    int live_first_direct = 0;   // the opponent's hits that came first directly / over the server
    int live_first_server = 0;
    void live_take(const std::string& body, bool direct = false);
    void live_log_lag();
    void live_update(double current_ms);
    void live_send_finish();
    void draw_live_overlay();
    // surrender (the pause key in a live battle): the song ends for both and counts as lost
    bool live_surrender_prompt = false;
    void live_end_now(); // the song ends here (a surrender either way)

protected:
    // merge 0ed42ec: upstream's virtual with Z-DON's rule (a Z-DON AI battle goes back to the AI song select);
    // upstream's AIGameScreen overrides it
    Screens select_screen() const override;
    Screens result_screen() const override { return Screens::RESULT_2P; }

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
