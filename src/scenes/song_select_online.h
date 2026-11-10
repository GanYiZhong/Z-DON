#pragma once
#include <string>
#include <vector>

#include "../libs/network/network.h"
#include "song_select.h"

// Z-DON online battle: the mode board "Online battle" on the entry screen opens this song select (screen name
// ONLINE_SELECT, the song select's skin). Deciding a course lists the server's plays of that chart that kept their
// hits (each player's best); picking one plays the chart against it, the record in the 2P lane (zdon_online.h,
// Game2PScreen), the higher score winning. Drawn by the engine (zdon_hud), the same in every skin.
class OnlineSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Loading,  // the list is being fetched
        List,     // pick an opponent
        Empty,    // nobody to challenge on this chart
        Offline,  // the server didn't answer
        Fetching, // the picked play is being fetched (SongSelectScreen::take_replay_jump starts the battle)
        Failed    // it could not be fetched or its chart is not here
    };
    Modal modal = Modal::None;
    std::vector<ChallengeEntry> entries;
    int cursor = 0;
    std::string title;
    int difficulty = 0;
    double fetch_started_ms = 0.0;

    void close_modal();
    void update_modal();
    void draw_modal();

protected:
    void select_song(SongBox* box) override;
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }

public:
    OnlineSongSelectScreen() : SongSelectScreen("song_select") {}
    void on_screen_start() override;
    std::optional<Screens> update() override;
    void draw() override;
};
