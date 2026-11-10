#pragma once
// Z-DON online battle. CHALLENGE: the 2P lane plays someone's recorded play of the chart (its hits from the server),
// the higher score wins. LIVE (later): the 2P lane plays the opponent's hits as they arrive.
// Set by the song select that starts the battle; the song select resets it on its next start.
#include <cstdint>
#include <string>

#include "file/scores.h"
#include "judge.h"

struct ZdonOnline {
    enum class Mode {
        NONE,
        CHALLENGE
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

    bool active() const { return mode != Mode::NONE; }
    void reset() { *this = ZdonOnline{}; }
    // reads inverse / random / seed / subdiff from a play's modifiers JSON (network.cpp modifiers_to_json)
    void read_modifiers(const std::string& json);
};

extern ZdonOnline zdon_online;
