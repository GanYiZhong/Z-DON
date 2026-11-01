#pragma once
#include <filesystem>
#include <string>

#include "song_select.h"

// Z-DON recording mode: the mode board "Recording" on the entry screen opens this song select (screen name
// RECORD_SELECT, the song select's skin). Deciding a course does not play it: a window asks auto play or no
// hits, then the game records it to a video in a hidden child process (zdon_record.h export_*) while a
// progress window waits, and the song select comes back with the saved file's place. Drawn by the engine
// (zdon_hud), the same in every skin.
class RecordSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Choose,
        Running,
        Done,
        Failed
    };
    Modal modal = Modal::None;
    int choice = 0; // 0 auto play, 1 no hits
    std::filesystem::path song;
    int difficulty = 0;
    std::string title;
    std::filesystem::path out;
    double progress = 0.0;

    void close_modal();
    void update_modal();
    void draw_modal();

protected:
    void select_song(SongBox* song) override;
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }

public:
    RecordSongSelectScreen() : SongSelectScreen("song_select") {}
    Screens on_screen_end(Screens next_screen) override;
    std::optional<Screens> update() override;
    void draw() override;
};
