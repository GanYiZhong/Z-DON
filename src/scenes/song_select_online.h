#pragma once
#include <optional>
#include <string>
#include <vector>

#include "../libs/network/network.h"
#include "song_select.h"

// Z-DON online battle: the mode board "Online battle" on the entry screen opens this song select (screen name
// ONLINE_SELECT, the song select's skin), starting at a menu:
//   Challenge   deciding a course lists the server's plays of that chart that kept their hits (each player's best);
//               picking one plays the chart against it, the record in the 2P lane
//   Quick match / Create room / Join room (a 4-digit code)   a live battle (server/zdon_match.py): the host picks
//               the songs here, the guest picks its own course of each; both play at once, the opponent in the 2P
//               lane (Game2PScreen live_*). The room stays for the next song until one leaves.
// The higher score wins. Drawn by the engine (zdon_hud), the same in every skin.
class OnlineSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Menu,       // challenge / quick match / create / join
        JoinInput,  // typing a room code
        Connecting, // create / quick / join in flight
        Room,       // in a room, waiting (for an opponent, for the host's song, for the guest)
        GuestPick,  // the guest: the host's song, pick a course
        Notice,     // a message, then back to the menu or the room
        // challenge
        Loading,
        List,
        Empty,
        Offline,
        Fetching,
        Failed
    };
    Modal modal = Modal::Menu;
    int menu_cursor = 0;
    // challenge
    std::vector<ChallengeEntry> entries;
    int cursor = 0;
    std::string title;
    int difficulty = 0;
    double fetch_started_ms = 0.0;

    // live room
    std::string room; // the room's code ("" = not in one)
    bool host = false;
    std::string join_code;  // being typed
    int request = -1;       // create / quick / join / song / ready in flight
    int state_request = -1; // the room state poll in flight
    double last_state_ms = -1e9;
    std::string room_state; // waiting / choosing / song / playing / finished
    int room_round = 0;
    int started_round = 0; // the last round this side started playing
    std::string opp_json;  // the opponent's profile (JSON object) as the server sent it
    std::string opp_name;
    std::optional<bool> opp_has_song;
    int opp_difficulty = -1;
    std::string opp_mods;
    std::string song_hash, song_title; // the host's pick
    int song_difficulty = 3;
    std::vector<int> courses; // the guest: the courses of the host's song this library has
    int course_cursor = 0;
    std::string notice;
    Modal notice_back = Modal::Menu;
    bool host_waiting_guest = false; // the host picked and waits for the guest

    static std::string my_code();
    std::string my_mods_json() const;
    void close_modal();
    void show_notice(const std::string& text, Modal back);
    void leave_room();
    void poll_room(double now);
    void on_room_state(const std::string& body);
    std::optional<Screens> start_live();
    std::optional<Screens> update_modal();
    void draw_modal();
    void draw_live_tag(float u);

protected:
    void select_song(SongBox* box) override;
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }

public:
    OnlineSongSelectScreen() : SongSelectScreen("song_select") {}
    void on_screen_start() override;
    Screens on_screen_end(Screens next_screen) override;
    std::optional<Screens> update() override;
    void draw() override;
};
