#include "song_select_online.h"

#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "../libs/audio/audio.h"
#include "../libs/input/input.h"
#include "../libs/math/sha256.h"
#include "../libs/zdon_online.h"
#include "../objects/global/zdon_hud.h"

namespace {
    constexpr const char* kMenuKeys[] = {"online_menu_challenge", "online_menu_quick", "online_menu_create",
                                         "online_menu_join"};
    constexpr int kMenuCount = 4;
    const Color kGold{255, 226, 120, 255};
    const Color kBlue{140, 220, 255, 255};
} // namespace

std::string OnlineSongSelectScreen::my_code() {
    return global_data.config->network.access_code_1;
}

// the chart options the opponent's lane needs to rebuild my notes
std::string OnlineSongSelectScreen::my_mods_json() const {
    rapidjson::Document doc;
    doc.SetObject();
    auto& a = doc.GetAllocator();
    doc.AddMember("inverse", player->player_data.modifier_inverse, a);
    doc.AddMember("random", player->player_data.modifier_random, a);
    doc.AddMember("seed", 0, a);
    doc.AddMember("subdiff", global_data.session_data[static_cast<int>(PlayerNum::P1)].subdiff, a);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    doc.Accept(w);
    return buf.GetString();
}

void OnlineSongSelectScreen::on_screen_start() {
    SongSelectScreen::on_screen_start(); // resets zdon_online; the room is kept here
    entries.clear();
    request = state_request = -1;
    last_state_ms = -1e9;
    host_waiting_guest = false;
    modal = room.empty() ? Modal::Menu : Modal::Room; // back from a live battle: the room goes on
}

Screens OnlineSongSelectScreen::on_screen_end(Screens next_screen) {
    if (next_screen != Screens::GAME_2P && !room.empty()) {
        leave_room();
    }
    return SongSelectScreen::on_screen_end(next_screen);
}

void OnlineSongSelectScreen::leave_room() {
    guest_choosing = false;
    transfer_started = false;
    if (!room.empty()) {
        const int t = network.signed_request("POST", "/match/leave", {{"access_code", my_code()}, {"code", room}});
        (void)t; // fire and forget: an answer nobody takes is dropped with the next ones
    }
    room.clear();
    room_state.clear();
    opp_name.clear();
    opp_json.clear();
    host = false;
    host_waiting_guest = false;
}

void OnlineSongSelectScreen::show_notice(const std::string& text, Modal back) {
    notice = text;
    notice_back = back;
    modal = Modal::Notice;
}

void OnlineSongSelectScreen::select_song(SongBox* box) {
    if (modal != Modal::None) {
        return; // the course stays decided behind the window
    }
    // the options are read from scores.db when the game starts (SongSelectScreen::select_song saves them too)
    scores_manager.save_player_data(player->player_data);
    difficulty = static_cast<int>(player->selected_difficulty);
    title = box->text_name;
    if (!room.empty() && !host) { // the guest: its course (and options) of the host's song
        const auto want = scores_manager.get_path_by_diff_hash(song_hash);
        if (!guest_choosing || !want || *want != box->path) {
            guest_rejump = guest_choosing;
            show_notice(zhud::text("online_guest_wrong_song", song_title), Modal::None);
            return;
        }
        guest_ready(difficulty);
        return;
    }
    if (!room.empty()) { // the host's pick for the room
        song_hash = box->hash_for(difficulty);
        song_title = title;
        song_difficulty = difficulty;
        global_data.session_data[static_cast<int>(PlayerNum::P1)].selected_song = box->path;
        request = network.signed_request("POST", "/match/song",
                                         {{"access_code", my_code()},
                                          {"code", room},
                                          {"hash", song_hash},
                                          {"title", song_title},
                                          {"difficulty", std::to_string(difficulty)},
                                          {"mods", my_mods_json()}});
        host_waiting_guest = true;
        modal = Modal::Room;
        return;
    }
    entries.clear();
    cursor = 0;
    network.request_challenges(box->hash_for(difficulty), difficulty);
    modal = Modal::Loading;
}

void OnlineSongSelectScreen::close_modal() {
    modal = Modal::None;
    replay_is_challenge = false;
    navigator.exit_diff_select();
    state = SongSelectState::BROWSING;
    player->reset_selection();
}

void OnlineSongSelectScreen::poll_room(double now) {
    if (room.empty()) {
        return;
    }
    if (state_request >= 0) {
        if (auto r = network.take_signed(state_request)) {
            state_request = -1;
            if (r->first == 404) {
                room.clear();
                show_notice(zhud::text("online_room_gone"), Modal::Menu);
                return;
            }
            if (r->first == 200) {
                on_room_state(r->second);
            }
        }
    }
    if (state_request < 0 && now - last_state_ms >= 500.0) {
        state_request = network.signed_request("GET", "/match/state", {{"access_code", my_code()}, {"code", room}});
        last_state_ms = now;
    }
}

void OnlineSongSelectScreen::on_room_state(const std::string& body) {
    rapidjson::Document doc;
    if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
        return;
    }
    auto str = [](const rapidjson::Value& v, const char* k) -> std::string {
        return v.HasMember(k) && v[k].IsString() ? v[k].GetString() : "";
    };
    const std::string prev_opp = opp_name;
    room_state = str(doc, "state");
    host = str(doc, "role") == "host";
    room_round = doc.HasMember("round") && doc["round"].IsInt() ? doc["round"].GetInt() : 0;
    opp_name.clear();
    opp_has_song.reset();
    opp_difficulty = -1;
    if (doc.HasMember("opponent") && doc["opponent"].IsObject()) {
        const auto& o = doc["opponent"];
        rapidjson::StringBuffer buf;
        rapidjson::Writer<rapidjson::StringBuffer> w(buf);
        o.Accept(w);
        opp_json = buf.GetString();
        opp_name = str(o, "username");
        if (o.HasMember("has_song") && o["has_song"].IsBool()) {
            opp_has_song = o["has_song"].GetBool();
        }
        opp_difficulty = o.HasMember("difficulty") && o["difficulty"].IsInt() ? o["difficulty"].GetInt() : -1;
        opp_mods = str(o, "mods");
    }
    transfer_ready = doc.HasMember("transfer") && doc["transfer"].IsObject() && doc["transfer"].HasMember("ready") &&
                     doc["transfer"]["ready"].IsBool() && doc["transfer"]["ready"].GetBool();
    if (doc.HasMember("song") && doc["song"].IsObject()) {
        song_hash = str(doc["song"], "hash");
        song_title = str(doc["song"], "title");
        song_difficulty = doc["song"].HasMember("difficulty") && doc["song"]["difficulty"].IsInt()
                              ? doc["song"]["difficulty"].GetInt()
                              : 3;
    }
    if (room_state != "transfer" && transfer_started && !pack_job.valid() && !install_job.valid() &&
        transfer_request < 0) {
        transfer_started = false; // done (or given up): a later round may send again
        if (!host && room_state == "choosing") {
            show_notice(zhud::text("online_transfer_failed"), Modal::Room);
            return;
        }
    }
    if (!prev_opp.empty() && opp_name.empty()) { // the opponent left: wait for the next one
        host_waiting_guest = false;
        show_notice(zhud::text("online_opp_gone", prev_opp), Modal::Room);
        return;
    }
    // the host: an opponent is here and no song is waiting: pick one (the song select itself)
    if (host && !opp_name.empty() && (room_state == "choosing" || room_state == "finished")) {
        if (host_waiting_guest && opp_has_song.has_value() && !*opp_has_song) {
            host_waiting_guest = false;
            show_notice(zhud::text("online_no_song", opp_name), Modal::None);
            return;
        }
        if (modal == Modal::Room && !host_waiting_guest) {
            close_modal();
        }
    }
    // the guest: the host picked a song (go to it), or the host is sending it
    if (!host && room_state == "song" && modal == Modal::Room && !transfer_started) {
        if (scores_manager.get_path_by_diff_hash(song_hash)) {
            guest_go_to_song();
        } else { // not in this library: the host's game sends it
            request = network.signed_request(
                "POST", "/match/ready",
                {{"access_code", my_code()}, {"code", room}, {"has_song", "false"}, {"want", "1"}});
            transfer_started = true;
            transfer_ready = false;
        }
    }
    // the host: the guest asked for the song
    if (host && room_state == "transfer" && !transfer_started) {
        transfer_started = true;
        if (auto path = scores_manager.get_path_by_diff_hash(song_hash)) {
            pack_job = std::async(std::launch::async, [p = *path] { return zdon_pack_song(p); });
        } else {
            transfer_request =
                network.signed_request("POST", "/match/upload_fail", {{"access_code", my_code()}, {"code", room}});
        }
    }
}

void OnlineSongSelectScreen::guest_go_to_song() {
    close_modal();
    if (navigator.jump_to_song(song_hash)) {
        guest_choosing = true;
        return;
    }
    // the song select can't show it (a collection folder, ...): the course window
    courses.clear();
    if (auto path = scores_manager.get_path_by_diff_hash(song_hash)) {
        const auto hashes = scores_manager.get_hashes(*path);
        for (int d = 0; d < static_cast<int>(hashes.size()); ++d) {
            if (!hashes[d].empty()) {
                courses.push_back(d);
            }
        }
    }
    if (courses.empty()) {
        courses.push_back(std::clamp(song_difficulty, 0, 4));
    }
    auto it = std::ranges::find(courses, song_difficulty);
    course_cursor = it != courses.end() ? static_cast<int>(it - courses.begin()) : static_cast<int>(courses.size()) - 1;
    modal = Modal::GuestPick;
}

void OnlineSongSelectScreen::guest_ready(int course) {
    guest_choosing = false;
    guest_difficulty = course;
    request = network.signed_request("POST", "/match/ready",
                                     {{"access_code", my_code()},
                                      {"code", room},
                                      {"has_song", "true"},
                                      {"difficulty", std::to_string(course)},
                                      {"mods", my_mods_json()}});
    modal = Modal::Room;
}

// the song going from the host's game to the guest's (through the server)
void OnlineSongSelectScreen::update_transfer() {
    auto ready = [](auto& job) {
        return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    };
    if (ready(pack_job)) { // the host: zipped, send it
        ZdonSongPack pack = pack_job.get();
        if (pack.zip.empty() || room.empty()) {
            transfer_request =
                network.signed_request("POST", "/match/upload_fail", {{"access_code", my_code()}, {"code", room}});
        } else {
            const std::string sha = crypto::to_hex(crypto::sha256(pack.zip));
            transfer_request = network.signed_upload(
                "/match/upload",
                {{"access_code", my_code()}, {"code", room}, {"hash", song_hash}, {"name", pack.name}, {"sha256", sha}},
                std::move(pack.zip), 300000);
        }
    }
    if (transfer_request >= 0) {
        if (auto r = network.take_signed(transfer_request)) {
            transfer_request = -1;
            if (host && r->first != 200 && room_state == "transfer") { // the upload failed: the host picks again
                spdlog::warn("Online battle: sending the song failed ({})", r->first);
                transfer_request =
                    network.signed_request("POST", "/match/upload_fail", {{"access_code", my_code()}, {"code", room}});
            } else if (!host && r->first == 200 && !r->second.empty()) { // the guest: unpack and check it
                install_job =
                    std::async(std::launch::async, [zip = std::move(r->second), h = song_hash,
                                                    t = song_title]() mutable { return zdon_install_song(zip, t, h); });
            } else if (!host) {
                transfer_started = false; // try the download again with the next state
            }
        }
    }
    // the guest: the server has it: fetch it
    if (!host && transfer_started && transfer_ready && transfer_request < 0 && !install_job.valid() &&
        room_state == "transfer") {
        transfer_request =
            network.signed_request("GET", "/match/download", {{"access_code", my_code()}, {"code", room}}, 300000);
        transfer_ready = false;
    }
    if (ready(install_job)) {
        const ZdonSongInstall got = install_job.get();
        transfer_started = false;
        if (got.chart && room_state == "transfer" && modal == Modal::Room) {
            guest_go_to_song();
        } else if (!got.chart) { // not the host's chart after all: the host picks again
            request = network.signed_request("POST", "/match/ready",
                                             {{"access_code", my_code()}, {"code", room}, {"has_song", "false"}});
            show_notice(zhud::text("online_transfer_bad", song_title), Modal::Room);
        }
    }
}

// both sides ready: set the song up and start (each game starts on its own; Game2PScreen follows the opponent)
std::optional<Screens> OnlineSongSelectScreen::start_live() {
    auto path = scores_manager.get_path_by_diff_hash(song_hash);
    if (!path) {
        return std::nullopt;
    }
    const auto hashes = scores_manager.get_hashes(*path);
    const int mine = host ? song_difficulty : guest_difficulty >= 0 ? guest_difficulty : song_difficulty;
    const int theirs = std::clamp(opp_difficulty >= 0 ? opp_difficulty : song_difficulty, 0, 4);
    auto& s1 = global_data.session_data[static_cast<int>(PlayerNum::P1)];
    auto& s2 = global_data.session_data[static_cast<int>(PlayerNum::P2)];
    s1.selected_song = *path;
    s1.selected_difficulty = mine;
    s1.song_hash = hashes[std::clamp(mine, 0, 4)];
    s2.selected_song = *path;
    s2.selected_difficulty = theirs;
    s2.song_hash = hashes[theirs];
    zdon_online.reset();
    zdon_online.start_round(room_round);
    zdon_online.room = room;
    zdon_online.host = host;
    zdon_online.opponent = zdon_online_profile(opp_json);
    zdon_online.read_modifiers(opp_mods);
    started_round = room_round;
    host_waiting_guest = false;
    spdlog::info("Online battle: room {} round {} vs {} ({}), my course {}", room, room_round, opp_name, theirs, mine);
    return on_screen_end(Screens::GAME_2P);
}

std::optional<Screens> OnlineSongSelectScreen::update_modal() {
    const bool l_kat = is_l_kat_pressed();
    const bool r_kat = is_r_kat_pressed();
    const bool don = is_l_don_pressed() || is_r_don_pressed();
    const bool cancel = check_key_pressed(global_data.config->keys.back_key);
    const bool account = !my_code().empty() && my_code() != "0" && my_code() != "1" && !is_guest_id(my_code());
    switch (modal) {
    case Modal::Menu:
        if (cancel) {
            return on_screen_end(Screens::ENTRY);
        }
        if (l_kat || r_kat) {
            menu_cursor = (menu_cursor + (r_kat ? 1 : kMenuCount - 1)) % kMenuCount;
            audio.play_sound("kat", VolumePreset::SOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::SOUND);
            if (menu_cursor == 0) {
                close_modal(); // browse: deciding a course lists the challenges
            } else if (!account) {
                show_notice(zhud::text("online_need_account"), Modal::Menu);
            } else if (menu_cursor == 3) {
                join_code.clear();
                modal = Modal::JoinInput;
            } else {
                // tried even when the last connection check failed (a slow first check, or one slow answer, must
                // not keep online play away): a real failure shows when this one fails
                request = network.signed_request("POST", menu_cursor == 1 ? "/match/quick" : "/match/create",
                                                 {{"access_code", my_code()}}, 10000);
                modal = request >= 0 ? Modal::Connecting : Modal::Menu;
                if (request < 0) {
                    show_notice(zhud::text("online_offline"), Modal::Menu);
                }
            }
        }
        break;
    case Modal::JoinInput: {
        for (int d = 0; d < 10; ++d) {
            const int sc = d == 0 ? SDL_SCANCODE_0 : SDL_SCANCODE_1 + (d - 1);
            const int kp = d == 0 ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (d - 1);
            if ((key_pressed(sc) || key_pressed(kp)) && join_code.size() < 4) {
                join_code += static_cast<char>('0' + d);
            }
        }
        if (key_pressed(SDL_SCANCODE_BACKSPACE) && !join_code.empty()) {
            join_code.pop_back();
        }
        if (cancel) {
            modal = Modal::Menu;
        } else if ((don || key_pressed(SDL_SCANCODE_RETURN)) && join_code.size() == 4) {
            request =
                network.signed_request("POST", "/match/join", {{"access_code", my_code()}, {"code", join_code}}, 10000);
            modal = Modal::Connecting;
            if (request < 0) {
                show_notice(zhud::text("online_offline"), Modal::Menu);
            }
        }
        break;
    }
    case Modal::Connecting:
        if (auto r = network.take_signed(request)) {
            request = -1;
            rapidjson::Document doc;
            if (r->first == 200 && !doc.Parse(r->second.c_str()).HasParseError() && doc.IsObject() &&
                doc.HasMember("code") && doc["code"].IsString()) {
                room = doc["code"].GetString();
                room_state = "waiting";
                last_state_ms = -1e9;
                modal = Modal::Room;
            } else {
                show_notice(zhud::text(r->first == 404   ? "online_no_room"
                                       : r->first == 409 ? "online_room_full"
                                                         : "online_offline"),
                            Modal::Menu);
            }
        } else if (cancel) {
            modal = Modal::Menu;
        }
        break;
    case Modal::Room:
        if (cancel) {
            leave_room();
            modal = Modal::Menu;
        }
        break;
    case Modal::GuestPick:
        if (cancel) {
            leave_room();
            modal = Modal::Menu;
        } else if (l_kat || r_kat) {
            const int n = static_cast<int>(courses.size());
            course_cursor = (course_cursor + (r_kat ? 1 : n - 1)) % n;
            audio.play_sound("kat", VolumePreset::SOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::SOUND);
            guest_ready(courses[course_cursor]);
        }
        break;
    case Modal::Notice:
        if (don || l_kat || r_kat || cancel) {
            if (std::exchange(guest_rejump, false) && !room.empty() && room_state != "choosing") {
                guest_go_to_song(); // back at the host's song
            } else if (notice_back == Modal::None) {
                close_modal();
            } else {
                modal = notice_back;
            }
        }
        break;
    case Modal::Loading:
        if (auto list = network.take_challenges()) {
            entries = std::move(list->entries);
            modal = !list->ok ? Modal::Offline : entries.empty() ? Modal::Empty : Modal::List;
        } else if (cancel) {
            close_modal();
        }
        break;
    case Modal::List:
        if (cancel) {
            close_modal();
        } else if (l_kat || r_kat) {
            const int n = static_cast<int>(entries.size());
            cursor = (cursor + (r_kat ? 1 : n - 1)) % n;
            audio.play_sound("kat", VolumePreset::SOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::SOUND);
            replay_is_challenge = true; // take_replay_jump starts the battle when the play arrives
            network.request_replay(entries[cursor].score_id);
            fetch_started_ms = get_current_ms();
            modal = Modal::Fetching;
        }
        break;
    case Modal::Fetching:
        // still here long after the request: it failed (no answer, or the chart is not in this library)
        if (cancel || get_current_ms() - fetch_started_ms > 12000.0) {
            replay_is_challenge = false;
            modal = cancel ? Modal::List : Modal::Failed;
        }
        break;
    case Modal::Empty:
    case Modal::Offline:
    case Modal::Failed:
        if (don || l_kat || r_kat || cancel) {
            close_modal();
        }
        break;
    case Modal::None:
        break;
    }
    return std::nullopt;
}

std::optional<Screens> OnlineSongSelectScreen::update() {
    const double now = get_current_ms();
    if (request >= 0 && modal != Modal::Connecting && network.take_signed(request)) {
        request = -1; // song / ready: the room state tells what came of it
    }
    poll_room(now);
    update_transfer();
    // both ready: go (the host after its pick, the guest after its course)
    if (!room.empty() && room_state == "playing" && room_round != started_round && !opp_name.empty() &&
        (modal == Modal::Room || modal == Modal::None)) {
        if (auto next = start_live()) {
            return next;
        }
    }
    // the host browsing with nobody (left) or no pick allowed: back to the room window
    if (!room.empty() && host && modal == Modal::None &&
        (opp_name.empty() || (room_state != "choosing" && room_state != "finished"))) {
        modal = Modal::Room;
    }
    // the guest choosing at the host's song while the host left or picks again: back to the room window
    if (!room.empty() && !host && modal == Modal::None &&
        (opp_name.empty() || (room_state != "song" && room_state != "transfer"))) {
        close_modal();
        guest_choosing = false;
        modal = Modal::Room;
    }
    if (modal != Modal::None) {
        auto next = update_modal();
        clear_input_buffers(); // the song select behind the window gets none of it
        if (next) {
            return next;
        }
    } else if (!room.empty() && state == SongSelectState::BROWSING &&
               check_key_pressed(global_data.config->keys.back_key)) {
        leave_room(); // leaves the room instead of the screen (in the course window it closes that)
        modal = Modal::Menu;
        clear_input_buffers();
    }
    return SongSelectScreen::update();
}

void OnlineSongSelectScreen::draw_live_tag(float u) {
    std::string text = zhud::text("online_mode");
    if (!room.empty()) {
        text = zhud::text("online_room_tag", room, opp_name.empty() ? "-" : opp_name);
    }
    if (guest_choosing && modal == Modal::None) {
        text = zhud::text("online_guest_choose", opp_name, song_title);
    }
    OutlinedText& tag = zhud::label(text, 26, kBlue);
    const float tw = tag.width + 28 * u;
    const float th = tag.height + 10 * u;
    zhud::panel(tex.screen_width / 2.0f - tw / 2, 10 * u, tw, th, 0.8f, fade(kBlue, 0.8f));
    tag.draw({.x = tex.screen_width / 2.0f - tag.width / 2, .y = 15 * u});
}

void OnlineSongSelectScreen::draw() {
    SongSelectScreen::draw();
    const float u = tex.screen_width / 1920.0f;
    draw_live_tag(u);
    if (modal != Modal::None) {
        draw_modal();
    }
}

void OnlineSongSelectScreen::draw_modal() {
    const float u = tex.screen_width / 1920.0f;
    const float w = 900 * u;
    const float x = (tex.screen_width - w) / 2.0f;
    auto centred = [&](OutlinedText& t, float y) { t.draw({.x = (tex.screen_width - t.width) / 2.0f, .y = y}); };
    // a window of text lines (the first one gold)
    auto lines_window = [&](const std::vector<std::string>& lines, const std::string& hint) {
        std::vector<OutlinedText*> ts;
        float h = 40 * u;
        for (size_t i = 0; i < lines.size(); ++i) {
            ts.push_back(&zhud::label(lines[i], i == 0 ? 30 : 26, i == 0 ? kGold : Color::WHITE));
            h += ts.back()->height + 14 * u;
        }
        OutlinedText* hint_t = hint.empty() ? nullptr : &zhud::label(hint, 22, Color{200, 200, 200, 255});
        if (hint_t) {
            h += hint_t->height + 10 * u;
        }
        float y = (tex.screen_height - h) / 2.0f;
        zhud::panel(x, y, w, h, 0.94f, fade(kGold, 0.7f));
        y += 20 * u;
        for (OutlinedText* t : ts) {
            centred(*t, y);
            y += t->height + 14 * u;
        }
        if (hint_t) {
            centred(*hint_t, y + 4 * u);
        }
    };

    switch (modal) {
    case Modal::Menu: {
        const float row_h = 64 * u;
        OutlinedText& head = zhud::label(zhud::text("online_mode"), 32, kGold);
        OutlinedText& hint = zhud::label(zhud::text("online_menu_hint"), 22, Color{200, 200, 200, 255});
        const float h = 30 * u + head.height + 20 * u + kMenuCount * row_h + 10 * u + hint.height + 24 * u;
        const float y0 = (tex.screen_height - h) / 2.0f;
        zhud::panel(x, y0, w, h, 0.94f, fade(kGold, 0.7f));
        centred(head, y0 + 22 * u);
        float y = y0 + 30 * u + head.height + 20 * u;
        for (int i = 0; i < kMenuCount; ++i) {
            const bool on = i == menu_cursor;
            if (on) {
                zhud::panel(x + 120 * u, y, w - 240 * u, row_h - 10 * u, 0.6f, kGold);
            }
            OutlinedText& t = zhud::label(zhud::text(kMenuKeys[i]), 28, on ? kGold : Color::WHITE);
            centred(t, y + (row_h - 10 * u - t.height) / 2.0f);
            y += row_h;
        }
        centred(hint, y + 10 * u);
        return;
    }
    case Modal::JoinInput: {
        std::string shown = join_code;
        while (shown.size() < 4) {
            shown += "_";
        }
        lines_window({zhud::text("online_join_title"), shown}, zhud::text("online_join_hint"));
        return;
    }
    case Modal::Connecting:
        lines_window({zhud::text("online_connecting")}, "");
        return;
    case Modal::Room: {
        std::vector<std::string> lines{zhud::text("online_room_title", room)};
        if (opp_name.empty()) {
            lines.push_back(zhud::text("online_wait_opp"));
        } else if (room_state == "transfer") {
            lines.push_back(zhud::text("online_vs", opp_name));
            lines.push_back(song_title);
            lines.push_back(host ? zhud::text("online_transfer_send", opp_name)
                                 : zhud::text("online_transfer_wait", opp_name));
        } else if (room_state == "song" || host_waiting_guest) {
            lines.push_back(zhud::text("online_vs", opp_name));
            lines.push_back(host ? zhud::text("online_wait_guest", opp_name) : zhud::text("online_wait_start"));
        } else {
            lines.push_back(zhud::text("online_vs", opp_name));
            lines.push_back(host ? zhud::text("online_pick_song") : zhud::text("online_wait_host", opp_name));
        }
        lines_window(lines, zhud::text("online_room_hint"));
        return;
    }
    case Modal::GuestPick: {
        const int d = courses.empty() ? 0 : courses[course_cursor];
        lines_window({zhud::text("online_guest_title", opp_name), song_title, "< " + zhud::course_name(d) + " >"},
                     zhud::text("online_guest_hint"));
        return;
    }
    case Modal::Notice:
        lines_window({notice}, zhud::text("online_notice_hint"));
        return;
    case Modal::List:
        break;
    case Modal::None:
        return;
    default: {
        const char* key = modal == Modal::Loading    ? "online_loading"
                          : modal == Modal::Empty    ? "online_none"
                          : modal == Modal::Offline  ? "online_offline"
                          : modal == Modal::Fetching ? "online_fetching"
                                                     : "online_failed";
        lines_window({zhud::text(key)}, "");
        return;
    }
    }

    // the challenge list: up to 8 rows around the cursor
    constexpr int kRows = 8;
    const int n = static_cast<int>(entries.size());
    const int first = std::clamp(cursor - kRows / 2, 0, std::max(0, n - kRows));
    const int shown = std::min(kRows, n);
    const float row_h = 48 * u;
    OutlinedText& head = zhud::label(zhud::text("online_list_title", title, zhud::course_name(difficulty)), 30, kGold);
    OutlinedText& hint = zhud::label(zhud::text("online_list_hint"), 22, Color{200, 200, 200, 255});
    const float h = 30 * u + head.height + 16 * u + shown * row_h + 16 * u + hint.height + 24 * u;
    const float y0 = (tex.screen_height - h) / 2.0f;
    zhud::panel(x, y0, w, h, 0.94f, fade(kGold, 0.7f));
    centred(head, y0 + 22 * u);
    float y = y0 + 30 * u + head.height + 16 * u;
    for (int i = first; i < first + shown; ++i) {
        const ChallengeEntry& e = entries[i];
        const bool on = i == cursor;
        if (on) {
            zhud::panel(x + 20 * u, y, w - 40 * u, row_h - 6 * u, 0.6f, kGold);
        }
        const Color c = on ? kGold : Color::WHITE;
        OutlinedText& rank = zhud::label(std::to_string(i + 1), 26, c);
        OutlinedText& name = zhud::label(e.username, 26, c);
        std::string score = std::to_string(e.score);
        for (int p = static_cast<int>(score.size()) - 3; p > 0; p -= 3) {
            score.insert(static_cast<size_t>(p), ",");
        }
        OutlinedText& sc = zhud::label(score, 26, c);
        const float ty = y + (row_h - 6 * u - name.height) / 2.0f;
        rank.draw({.x = x + 70 * u - rank.width, .y = ty});
        name.draw({.x = x + 100 * u, .y = ty});
        sc.draw({.x = x + w - 60 * u - sc.width, .y = ty});
        y += row_h;
    }
    centred(hint, y + 16 * u);
}
