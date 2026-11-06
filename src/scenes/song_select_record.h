#pragma once
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "song_select.h"

// Z-DON recording mode: the mode board "Recording" on the entry screen opens this song select (screen name
// RECORD_SELECT, the song select's skin). Deciding a course does not play it: a window asks auto play or no
// hits (and which other courses of the song to record with it, one lane each), then the game records it to a video in a
// hidden child process (zdon_record.h export_*) while a progress window waits, and the song select comes back with the
// saved file's place. Drawn by the engine (zdon_hud), the same in every skin.
// The progress window can go to the background (Kat): the song select works while the recording runs (a corner panel
// shows its progress, Back brings the window back), and songs decided meanwhile are queued and recorded one after
// another. Leaving the screen stops them all.
class RecordSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Choose,
        Running,
        Done,
        Failed
    };
    Modal modal = Modal::None;
    // the Choose window's cursor: 0 auto play, 1 no hits (Don records), 2.. the song's courses (all ticked at first)
    int cursor = 0;
    std::filesystem::path song;
    int difficulty = 0;             // the decided course: always ticked first, the top lane (1P of two)
    std::vector<int> available;     // the song's courses (0 Easy .. 4 Ura)
    std::array<bool, 5> ticked{};   // the courses recorded together (1 to 5)
    std::vector<int> lanes() const; // the ticked courses: the decided one, then the rest hardest first
    std::string title;

    // a decided recording, waiting for the one before it
    struct Job {
        std::filesystem::path song;
        std::vector<int> courses;
        bool autoplay = false;
        std::string title;
    };
    std::deque<Job> queue;
    bool running = false; // a recording runs (in the window or in the background)
    Job current;
    std::filesystem::path out; // the running (or last) recording's file
    double progress = 0.0;
    std::chrono::steady_clock::time_point run_started;
    int batch_saved = 0, batch_failed = 0; // since the queue was last empty
    std::string toast;                     // a short note over the song select (queued, all done)
    std::chrono::steady_clock::time_point toast_until;

    bool start(const Job& job); // false: the child could not start
    void poll();                // every frame while running: the next queued one when one ends
    std::string eta() const;    // the time left, "m:ss" ("" while it cannot tell yet)
    void show_toast(const std::string& s, double seconds);
    void close_modal();
    void update_modal();
    void draw_modal();
    void draw_background_panel();

protected:
    void select_song(SongBox* box) override;
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }

public:
    RecordSongSelectScreen() : SongSelectScreen("song_select") {}
    Screens on_screen_end(Screens next_screen) override;
    std::optional<Screens> update() override;
    void draw() override;
};
