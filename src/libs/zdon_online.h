#pragma once
// Z-DON online battle. CHALLENGE: the 2P lane plays someone's recorded play of the chart (its hits from the server),
// the higher score wins. LIVE: a room on the server (server/zdon_match.py); the 2P lane plays the opponent's hits
// as they arrive, following the opponent's own song position a moment behind (the two games start on their own).
// Set by the song select that starts the battle; the song select resets it on its next start.
#include <cstdint>
#include <optional>
#include <string>

#include "file/scores.h"
#include "judge.h"

struct ZdonOnline {
    enum class Mode {
        NONE,
        CHALLENGE,
        LIVE
    };
    Mode mode = Mode::NONE;
    InputLog opponent_log; // chart ms, drum
    PlayerData opponent;   // name, title, colours
    int opponent_score = 0;
    // the record's chart options: its hits only fit the notes it saw
    bool inverse = false;
    int random = 0;
    uint32_t seed = 0;
    int subdiff = 0;

    // LIVE
    std::string room;         // the room's code; kept after a round for the next one (the lobby leaves it)
    bool host = false;        // the host picks the songs
    int round = 0;            // the server's round number
    size_t sent = 0;          // my hits the server has
    size_t received = 0;      // the opponent's hits taken so far
    InputLog incoming;        // received, not yet given to the 2P lane
    double opp_pos = -1e18;   // the opponent's chart ms when last heard
    double opp_pos_at = 0.0;  // local ms of that
    double delay_ms = 1000.0; // how far the 2P lane follows behind the opponent's position
    bool opp_left = false;
    struct Final {
        int score = 0, good = 0, ok = 0, bad = 0, drumroll = 0, max_combo = 0;
    };
    std::optional<Final> opp_final;
    // the 2P lane's chart ms now (local ms)
    double opp_clock(double now_ms) const { return opp_pos - delay_ms + (now_ms - opp_pos_at); }

    bool active() const { return mode != Mode::NONE; }
    bool live() const { return mode == Mode::LIVE; }
    void reset() { *this = ZdonOnline{}; }
    // a new LIVE round in the same room
    void start_round(int new_round);
    // reads inverse / random / seed / subdiff from a play's modifiers JSON (network.cpp modifiers_to_json)
    void read_modifiers(const std::string& json);
};

// the opponent's profile from the server's JSON (zdon_server.py Api._profile)
PlayerData zdon_online_profile(const std::string& json_object);

extern ZdonOnline zdon_online;
