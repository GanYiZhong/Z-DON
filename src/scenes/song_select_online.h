#pragma once
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "../libs/network/network.h"
#include "../libs/zdon_song_share.h"
#include "song_select.h"

// Z-DON online battle: the mode board "Online battle" on the entry screen opens this song select (screen name
// ONLINE_SELECT, the song select's skin), starting at a menu:
//   Challenge   deciding a course lists the server's plays of that chart that kept their hits (each player's best);
//               picking one plays the chart against it, the record in the 2P lane
//   Quick match / Create room / Join room (a 4-digit code)   a live battle (server/zdon_match.py): the host picks
//               the songs here; the guest's song select goes to that song, where it picks its own course and
//               options; both play at once, the opponent in the 2P lane (Game2PScreen live_*). A guest without
//               the chart gets it from the host's game (zdon_song_share, checked by its hash). The room stays for
//               the next song until one leaves.
//   A match is best of three: the first song's picker is drawn at random, then the loser of a song picks (at 1:1
//   drawn again); the first to two wins takes it and the room closes.
//   The guest is put straight into the host's song's course window and can't leave it (course and options only).
//   Chat: in a room, Y opens a line to type (the system's IME works), Enter sends it, Esc closes it; the room's
//   messages show bottom left (server/zdon_match.py chat).
// The higher score wins. Drawn by the engine (zdon_hud), the same in every skin.
class OnlineSongSelectScreen : public SongSelectScreen {
    enum class Modal {
        None,
        Menu,       // challenge / quick match / create / join
        JoinInput,  // typing a room code
        Connecting, // create / quick / join in flight
        Room,       // in a room, waiting (for an opponent, for the host's song, for the guest)
        GuestPick,  // the guest: the host's song, pick a course (when the song select can't go to the song)
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
    bool guest_choosing = false;     // the guest: browsing at the host's song, picking a course and options
    bool guest_rejump = false;       // the guest picked another song: back to the host's after the notice
    int guest_difficulty = -1;       // the guest's course for this round

    // the best-of-three match (server/zdon_match.py): songs won, and its end
    int match_you = 0, match_opp = 0, match_songs = 0;

    // chat
    struct ChatLine {
        std::string from, text;
        double at_ms = 0.0; // local ms it came
    };
    std::vector<ChatLine> chat_lines;
    int chat_last_id = 0;
    bool chat_typing = false;
    std::string chat_text;
    int chat_request = -1;
    void update_chat(double now);
    void draw_chat(float u);

    // sending a song to a guest that hasn't it (the host) / getting it (the guest)
    std::future<ZdonSongPack> pack_job;
    std::future<ZdonSongInstall> install_job;
    int transfer_request = -1;     // upload / download in flight
    bool transfer_started = false; // this round's song is being sent / fetched
    bool transfer_ready = false;   // the guest: the server has the host's song

    static std::string my_code();
    std::string my_mods_json() const;
    void close_modal();
    void show_notice(const std::string& text, Modal back);
    void leave_room();
    void poll_room(double now);
    void on_room_state(const std::string& body);
    std::optional<Screens> start_live();
    void guest_go_to_song();
    void guest_ready(int difficulty);
    void update_transfer();
    std::optional<Screens> update_modal();
    void draw_modal();
    void draw_live_tag(float u);

protected:
    void select_song(SongBox* box) override;
    bool hides_dan() override { return true; }
    bool allows_second_player_join() override { return false; }
    // the guest stays in the host's song: backing out of the course window opens it again
    void on_diff_select_cancelled(double current_ms) override;

public:
    OnlineSongSelectScreen() : SongSelectScreen("song_select") {}
    void on_screen_start() override;
    Screens on_screen_end(Screens next_screen) override;
    std::optional<Screens> update() override;
    void draw() override;
};
