#pragma once
#include <future>
#include <memory>
#include <string>
#include <unordered_map>

#include "../libs/gfx/text.h"
#include "../libs/network/zdon_forge_songs.h"
#include "../libs/screen.h"

// Z-DON: the TaikoForge login (server/zdon_game_login.py) between the loading screen and the title. The server's
// /health says whether it wants one ("forge_login": "required" or "optional"); without that, or offline, the title
// comes at once. A token kept in config.toml ([network] forge_token, never the password) logs in without the form;
// the form asks for the TaikoForge email and password, and the game then plays (1P) as the member's game account.
// Drawn by the engine (zdon_hud), the same in every skin.
// Logged in, it then brings the member's TaikoForge charts into the song folder (zdon_forge_songs), once a launch;
// when that changed anything the loading screen comes again (the song list is walked anew), else the title.
class ZdonLoginScreen : public Screen {
public:
    ZdonLoginScreen() : Screen("zdon_login") {}
    TouchInput touch_input() const override { return TouchInput::SCREEN; }

    void on_screen_start() override;
    std::optional<Screens> update() override;
    void draw() override;
    Screens on_screen_end(Screens next_screen) override;

private:
    enum class Stage {
        Health,  // asking /health whether this server wants a login
        Session, // checking the kept token
        Form,    // the email / password form
        Sending, // the form was sent
        Forge    // the member's TaikoForge charts are brought in
    };
    // the form's rows: the two fields, then the buttons
    enum Row {
        EMAIL = 0,
        PASSWORD,
        LOGIN,
        REGISTER,
        FORGOT,
        SKIP_OR_QUIT,
        ROW_COUNT
    };
    Stage stage = Stage::Health;
    bool required = false;
    int ticket = -1;
    int focus = EMAIL;
    std::string email;
    std::string password;
    std::string message; // the last error, in the player's language
    bool keyboard_shown = false;
    bool skipped = false;                            // "optional": the player chose to play without logging in
    std::string url_base = "https://forge.taiko.ac"; // TaikoForge (/health's forge_url): register / forgot links
    double started_ms = 0;
    Rect row_rect[ROW_COUNT]{};
    // the screen's texts (thin or no outline, unlike zhud::label), built on first use; cleared when it grows
    std::unordered_map<std::string, std::unique_ptr<OutlinedText>> texts;
    OutlinedText& text(const std::string& s, int size, Color color, float outline = 0.0f);

    std::future<bool> forge_job; // zdon_forge_songs::sync: true when the song folder changed
    std::shared_ptr<zdon_forge_songs::Progress> forge_progress;

    std::optional<Screens> done(); // the title (or the TaikoForge charts first), the login settled
    void send_login();
    void use_account(const std::string& code);
    void set_focus(int row);
    void activate(int row);
};
