#include "zdon_login.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <rapidjson/document.h>

#include "../libs/file/scores.h"
#include "../libs/gfx/camera_utils.h"
#include "../libs/input/input.h"
#include "../libs/network/network.h"
#include "../objects/global/zdon_hud.h"

namespace {
    const Color kGold{255, 226, 120, 255};
    const Color kBlue{140, 220, 255, 255};
    const Color kGrey{190, 190, 190, 255};
    const Color kRed{255, 150, 150, 255};
    constexpr size_t kMaxField = 254;

    // a reply's JSON object (an empty one when it is not JSON)
    rapidjson::Document parse(const std::string& body) {
        rapidjson::Document doc;
        if (doc.Parse(body.c_str()).HasParseError() || !doc.IsObject()) {
            doc.SetObject();
        }
        return doc;
    }

    std::string str(const rapidjson::Document& doc, const char* key) {
        return doc.HasMember(key) && doc[key].IsString() ? doc[key].GetString() : "";
    }

    bool placeholder(const std::string& code) {
        return code.empty() || code == "0" || code == "1";
    }
} // namespace

void ZdonLoginScreen::on_screen_start() {
    Screen::on_screen_start();
    stage = Stage::Health;
    required = false;
    skipped = false;
    url_base = "https://forge.taiko.ac";
    message.clear();
    password.clear();
    email = global_data.config->network.forge_email;
    focus = email.empty() ? EMAIL : PASSWORD;
    started_ms = get_current_ms();
    ticket = network.signed_request("GET", "/health", {}, 6000);
}

Screens ZdonLoginScreen::on_screen_end(Screens next_screen) {
    if (ticket >= 0) {
        network.forget_signed(ticket);
        ticket = -1;
    }
    if (keyboard_shown) {
        keyboard_shown = false;
        set_keyboard_visible(false);
    }
    password.clear();
    password.shrink_to_fit();
    return Screen::on_screen_end(next_screen);
}

std::optional<Screens> ZdonLoginScreen::done() {
    return on_screen_end(Screens::TITLE);
}

// 1P plays as this game account from now on (the local scores of a placeholder id go with it)
void ZdonLoginScreen::use_account(const std::string& code) {
    auto& net = global_data.config->network;
    if (code.empty() || code == net.access_code_1) {
        return;
    }
    if (placeholder(net.access_code_1)) {
        scores_manager.migrate_player_id(net.access_code_1, code);
    }
    net.access_code_1 = code;
}

void ZdonLoginScreen::send_login() {
    if (email.empty() || password.empty()) {
        message = zhud::text("login_empty");
        set_focus(email.empty() ? EMAIL : PASSWORD);
        return;
    }
    const auto& net = global_data.config->network;
    const auto pd = scores_manager.get_player_data(net.access_code_1);
    ticket = network.signed_request("POST", "/forge_login",
                                    {{"email", email},
                                     {"password", password},
                                     {"access_code", net.access_code_1},
                                     {"username", pd && !pd->username.empty() ? pd->username : "Player"}},
                                    20000);
    if (ticket < 0) {
        message = zhud::text("login_unreachable");
        return;
    }
    message.clear();
    stage = Stage::Sending;
}

void ZdonLoginScreen::set_focus(int row) {
    focus = std::clamp(row, 0, ROW_COUNT - 1);
}

void ZdonLoginScreen::activate(int row) {
    switch (row) {
    case EMAIL:
        set_focus(PASSWORD);
        break;
    case PASSWORD:
    case LOGIN:
        send_login();
        break;
    case REGISTER:
        SDL_OpenURL((url_base + "/register").c_str());
        break;
    case FORGOT:
        SDL_OpenURL((url_base + "/forgot").c_str());
        break;
    case SKIP_OR_QUIT:
        if (required) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        } else {
            skipped = true;
        }
        break;
    default:
        break;
    }
}

std::optional<Screens> ZdonLoginScreen::update() {
    if (auto r = Screen::update()) {
        return r;
    }
    auto& net = global_data.config->network;

    if (stage == Stage::Health || stage == Stage::Session || stage == Stage::Sending) {
        if (ticket < 0) {
            return done(); // offline: nothing to ask
        }
        auto reply = network.take_signed(ticket);
        if (!reply) {
            return std::nullopt;
        }
        ticket = -1;
        const auto& [status, body] = *reply;
        const rapidjson::Document doc = parse(body);

        if (stage == Stage::Health) {
            const std::string mode = status == 200 ? str(doc, "forge_login") : "";
            if (mode != "required" && mode != "optional") {
                return done(); // this server wants no login, or it did not answer
            }
            required = mode == "required";
            if (const std::string u = str(doc, "forge_url"); !u.empty()) {
                url_base = u;
            }
            if (net.forge_token.empty()) {
                stage = Stage::Form;
                return std::nullopt;
            }
            ticket = network.signed_request("POST", "/forge_session", {{"token", net.forge_token}}, 10000);
            stage = Stage::Session;
            return std::nullopt;
        }

        if (stage == Stage::Session) {
            if (status == 200) {
                // still a member: keep the account if it is one of theirs, else their first one
                bool mine = false;
                std::string first;
                if (doc.HasMember("accounts") && doc["accounts"].IsArray()) {
                    for (const auto& a : doc["accounts"].GetArray()) {
                        if (a.IsObject() && a.HasMember("access_code") && a["access_code"].IsString()) {
                            const std::string code = a["access_code"].GetString();
                            mine = mine || code == net.access_code_1;
                            if (first.empty()) {
                                first = code;
                            }
                        }
                    }
                }
                if (!mine && !first.empty()) {
                    use_account(first);
                    save_config(*global_data.config);
                }
                return done();
            }
            if (status == 401 || status == 403) {
                net.forge_token.clear();
                save_config(*global_data.config);
                message = zhud::text(str(doc, "code") == "blocked" ? "login_blocked" : "login_expired");
                stage = Stage::Form;
                return std::nullopt;
            }
            return done(); // the server could not check it now: play with the kept login
        }

        // Stage::Sending
        stage = Stage::Form;
        if (status == 200 && !str(doc, "token").empty()) {
            net.forge_token = str(doc, "token");
            net.forge_email = str(doc, "email").empty() ? email : str(doc, "email");
            use_account(str(doc, "access_code"));
            save_config(*global_data.config);
            password.clear();
            zhud::toast(zhud::text("login_done", net.forge_email));
            return done();
        }
        const std::string code = str(doc, "code");
        if (status == 401) {
            message = zhud::text("login_wrong");
            password.clear();
            set_focus(PASSWORD);
        } else if (code == "unverified") {
            message = zhud::text("login_unverified");
        } else if (code == "blocked") {
            message = zhud::text("login_blocked");
        } else if (status == 429) {
            message = zhud::text("login_too_many");
        } else {
            message = zhud::text("login_unreachable");
        }
        return std::nullopt;
    }

    // the form
    if (skipped) {
        return done();
    }
    const bool typing = focus == EMAIL || focus == PASSWORD;
    if (typing != keyboard_shown) {
        keyboard_shown = typing;
        set_keyboard_visible(typing, focus == EMAIL ? email : std::string());
    }
    const bool shift = key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
    if (key_pressed(SDL_SCANCODE_TAB)) {
        set_focus((focus + (shift ? ROW_COUNT - 1 : 1)) % ROW_COUNT);
        return std::nullopt;
    }
    if (key_pressed(SDL_SCANCODE_UP)) {
        set_focus(focus - 1);
        return std::nullopt;
    }
    if (key_pressed(SDL_SCANCODE_DOWN)) {
        set_focus(focus + 1);
        return std::nullopt;
    }

    // a click or tap on a row
    if (mouse_pressed(SDL_BUTTON_LEFT)) {
        const Vec2 m = get_screen_to_world_2d(mouse_position(), compute_camera2d(tex.screen_width, tex.screen_height));
        for (int i = 0; i < ROW_COUNT; ++i) {
            const Rect& r = row_rect[i];
            if (m.x >= r.x && m.x <= r.x + r.width && m.y >= r.y && m.y <= r.y + r.height) {
                set_focus(i);
                if (i >= LOGIN) {
                    activate(i);
                }
                return std::nullopt;
            }
        }
    }

    if (typing) {
        std::string& field = focus == EMAIL ? email : password;
        const bool is_email = focus == EMAIL;
        const TextEditAction a = poll_text_edit(field, [&field, is_email](int cp) {
            return field.size() < kMaxField && cp > 0x20 && (!is_email || cp < 0x80);
        });
        if (a == TextEditAction::Confirm) {
            activate(focus);
        } else if (a == TextEditAction::Cancel && !required) {
            skipped = true;
        }
        return std::nullopt;
    }

    // the buttons: drum or keyboard
    if (menu_left_pressed()) {
        set_focus(focus == LOGIN ? PASSWORD : focus - 1);
    } else if (menu_right_pressed()) {
        set_focus(focus + 1);
    } else if (menu_confirm_pressed() || key_pressed(SDL_SCANCODE_RETURN)) {
        activate(focus);
    } else if (key_pressed(SDL_SCANCODE_ESCAPE) && !required) {
        skipped = true;
    }
    return std::nullopt;
}

void ZdonLoginScreen::draw() {
    const float u = tex.screen_width / 1920.0f;
    const float sw = static_cast<float>(tex.screen_width);
    const float sh = static_cast<float>(tex.screen_height);
    const bool waiting = stage == Stage::Health || stage == Stage::Session;
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, waiting ? Color::BLACK : Color{24, 18, 34, 255});
    auto centred = [&](OutlinedText& t, float y) { t.draw({.x = (sw - t.width) / 2.0f, .y = y}); };

    if (stage == Stage::Health || stage == Stage::Session) {
        // a moment on most starts: only say something when it takes a while
        if (get_current_ms() - started_ms > 400) {
            centred(zhud::label(zhud::text("login_connecting"), 30, kGrey), sh / 2.0f - 20 * u);
        }
        return;
    }

    const float w = 940 * u;
    const float x = (sw - w) / 2.0f;
    const float field_h = 64 * u;
    const float button_h = 58 * u;
    const float gap = 16 * u;
    const float h = 814 * u;
    const float top = (sh - h) / 2.0f;
    zhud::panel(x, top, w, h, 0.9f, fade(kBlue, 0.6f));

    float y = top + 36 * u;
    centred(zhud::label(zhud::text("login_title"), 40, kGold), y);
    y += 70 * u;
    OutlinedText& sub = zhud::label(zhud::text(required ? "login_required" : "login_optional"), 22, kGrey);
    centred(sub, y);
    y += sub.height + 30 * u;

    const bool sending = stage == Stage::Sending;
    const bool caret_on = static_cast<long long>(get_current_ms() / 500) % 2 == 0;
    auto field = [&](int row, const char* label_key, const std::string& shown) {
        OutlinedText& l = zhud::label(zhud::text(label_key), 22, kGrey);
        l.draw({.x = x + 60 * u, .y = y});
        y += l.height + 6 * u;
        const Rect r{x + 60 * u, y, w - 120 * u, field_h};
        row_rect[row] = r;
        const bool on = focus == row && !sending;
        draw_rectangle(r, Color{10, 8, 16, 230});
        draw_rectangle_lines(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                             static_cast<int>(r.height), on ? kGold : Color{255, 255, 255, 90});
        const std::string text = shown + (on && caret_on ? "_" : "");
        if (!text.empty()) {
            OutlinedText& t = zhud::label(text, 28, Color::WHITE);
            t.draw({.x = r.x + 18 * u, .y = r.y + (field_h - t.height) / 2.0f});
        }
        y += field_h + gap;
    };
    field(EMAIL, "login_email", email);
    std::string dots;
    for (size_t i = 0; i < std::min<size_t>(password.size(), 40); ++i) {
        dots += "●";
    }
    field(PASSWORD, "login_password", dots);

    y += 6 * u;
    OutlinedText* msg = message.empty() ? nullptr : &zhud::label(message, 24, kRed);
    if (sending) {
        msg = &zhud::label(zhud::text("login_sending"), 24, kGrey);
    }
    if (msg) {
        centred(*msg, y);
    }
    y += 58 * u;

    auto draw_button = [&](int row, const std::string& text, float bx, float bw, bool primary) {
        const Rect r{bx, y, bw, button_h};
        row_rect[row] = r;
        const bool on = focus == row && !sending;
        const Color fill = primary ? Color{200, 60, 40, static_cast<unsigned char>(on ? 255 : 200)}
                                   : Color{60, 50, 80, static_cast<unsigned char>(on ? 255 : 190)};
        draw_rectangle(r, fill);
        if (on) {
            draw_rectangle_lines(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                                 static_cast<int>(r.height), kGold);
        }
        OutlinedText& t = zhud::label(text, primary ? 30 : 24, Color::WHITE);
        t.draw({.x = r.x + (bw - t.width) / 2.0f, .y = r.y + (button_h - t.height) / 2.0f});
    };
    const float inner_x = x + 60 * u;
    const float inner_w = w - 120 * u;
    draw_button(LOGIN, zhud::text("login_button"), inner_x, inner_w, true);
    y += button_h + gap;
    const float half = (inner_w - gap) / 2.0f;
    draw_button(REGISTER, zhud::text("login_register"), inner_x, half, false);
    draw_button(FORGOT, zhud::text("login_forgot"), inner_x + half + gap, half, false);
    y += button_h + gap;
    draw_button(SKIP_OR_QUIT, zhud::text(required ? "login_quit" : "login_skip"), inner_x, inner_w, false);
    y += button_h + 24 * u;

    centred(zhud::label(zhud::text("login_hint"), 20, kGrey), y);
}
