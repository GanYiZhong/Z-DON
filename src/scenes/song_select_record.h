#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "song_select.h"

// Z-DON recording mode: the mode board "Recording" on the entry screen opens this song select (screen name
// RECORD_SELECT, the song select's skin). Deciding a course does not play it: a window asks auto play or no
// hits (and which other courses of the song to record with it, one lane each), then the game records it to a video in a
// hidden child process (zdon_record.h export_*) while a progress window waits, and the song select comes back with the
// saved file's place. Drawn by the engine (zdon_hud), the same in every skin.
class RecordSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Choose,
        Running,
        Done,
        Failed
    };
    Modal modal = Modal::None;
    // the Choose window's cursor: 0 auto play, 1 no hits (Don records), 2.. the song's courses (Don ticks one)
    int cursor = 0;
    std::filesystem::path song;
    int difficulty = 0;             // the decided course: always ticked first, the top lane (1P of two)
    std::vector<int> available;     // the song's courses (0 Easy .. 4 Ura)
    std::array<bool, 5> ticked{};   // the courses recorded together (1 to 5)
    std::vector<int> lanes() const; // the ticked courses: the decided one, then the rest hardest first
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
