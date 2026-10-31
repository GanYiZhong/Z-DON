#pragma once
#include "song_select.h"

// Z-DON AI battle (objects/game/zdon_ai.h): the mode board "AI battle" on the entry screen opens this song
// select (screen name AI_SELECT, the song select's skin); the chosen song is played in GAME_2P against the AI
class AiSongSelectScreen : public SongSelectScreen {
protected:
    Screens get_game_screen_target() override {
        global_data.zdon_ai.requested = true;
        return Screens::GAME_2P;
    }
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }

public:
    AiSongSelectScreen() : SongSelectScreen("song_select") {}
};
