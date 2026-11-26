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
    texts.clear();
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

    // the mouse over a button highlights it
    if (mouse_moved()) {
        const Vec2 m = get_screen_to_world_2d(mouse_position(), compute_camera2d(tex.screen_width, tex.screen_height));
        for (int i = LOGIN; i < ROW_COUNT; ++i) {
            const Rect& r = row_rect[i];
            if (m.x >= r.x && m.x <= r.x + r.width && m.y >= r.y && m.y <= r.y + r.height) {
                set_focus(i);
            }
        }
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
            // a password may have spaces; an email has none (and only ASCII)
            return field.size() < kMaxField && (is_email ? cp > 0x20 && cp < 0x80 : cp >= 0x20);
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

OutlinedText& ZdonLoginScreen::text(const std::string& s, int size, Color color, float outline) {
    const std::string key = s + '\x1f' + std::to_string(size) + '\x1f' + std::to_string(color.r) + ',' +
                            std::to_string(color.g) + ',' + std::to_string(color.b) + '\x1f' + std::to_string(outline);
    auto it = texts.find(key);
    if (it == texts.end()) {
        const int px = std::max(1, static_cast<int>(size * tex.screen_scale));
        it = texts.emplace(key, std::make_unique<OutlinedText>(s, px, color, Color{20, 14, 26, 255}, false, outline))
                 .first;
    }
    return *it->second;
}

// The look follows zdon.taiko.ac: the night background, red buttons, the rainbow stripe.
void ZdonLoginScreen::draw() {
    if (texts.size() > 300) {
        texts.clear(); // typing makes a text per length: start over (nothing drawn yet this frame)
    }
    const float u = tex.screen_width / 1920.0f;
    const float sw = static_cast<float>(tex.screen_width);
    const float sh = static_cast<float>(tex.screen_height);
    const Color night{31, 24, 40, 255};
    const Color card{43, 33, 54, 250};
    const Color red{232, 68, 58, 255};
    const Color red_hi{246, 96, 84, 255};
    const Color gold{240, 190, 60, 255};
    const Color ink{246, 240, 248, 255};
    const Color muted{170, 156, 182, 255};
    const Color field_bg{22, 16, 30, 255};

    const bool waiting = stage == Stage::Health || stage == Stage::Session;
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, waiting ? Color::BLACK : night);
    if (waiting) {
        // a moment on most starts: only say something when it takes a while
        if (get_current_ms() - started_ms > 400) {
            OutlinedText& t = text(zhud::text("login_connecting"), 28, muted);
            t.draw({.x = (sw - t.width) / 2.0f, .y = sh / 2.0f - t.height / 2.0f});
        }
        return;
    }

    // drum heads in the corners, the rainbow stripe on top
    auto circle = [&](float cx, float cy, float r, Color c) {
        draw_rectangle_rounded(Rect{(cx - r) * u, (cy - r) * u, 2 * r * u, 2 * r * u}, 1.0f, 64, c);
    };
    circle(150, 960, 430, Color{232, 68, 58, 22});
    circle(150, 960, 330, night);
    circle(150, 960, 250, Color{232, 68, 58, 16});
    circle(1790, 110, 330, Color{59, 142, 222, 20});
    circle(1790, 110, 250, night);
    circle(1790, 110, 185, Color{59, 142, 222, 14});
    const Color rainbow[] = {
        {232, 68, 58, 255}, {240, 160, 32, 255}, {63, 174, 90, 255}, {59, 142, 222, 255}, {155, 89, 208, 255}};
    for (int i = 0; i < 5; ++i) {
        draw_rectangle(Rect{sw * static_cast<float>(i) / 5.0f, 0, sw / 5.0f + 1, 6 * u}, rainbow[i]);
    }

    const bool sending = stage == Stage::Sending;
    const float w = 780 * u;
    const float x = (sw - w) / 2.0f;
    const float h = 800 * u;
    const float top = (sh - h) / 2.0f - 10 * u;
    draw_rectangle_rounded(Rect{x + 6 * u, top + 10 * u, w, h}, 0.06f, 12, Color{0, 0, 0, 90}); // shadow
    draw_rectangle_rounded(Rect{x, top, w, h}, 0.06f, 12, card);
    draw_rectangle_rounded_lines_ex(Rect{x, top, w, h}, 0.06f, 12, 1.5f * u, Color{255, 255, 255, 28});

    const float pad = 64 * u;
    const float inner_x = x + pad;
    const float inner_w = w - 2 * pad;
    float y = top + 44 * u;

    // Z-DON, then what this is
    OutlinedText& z = text("Z-", 64, ink, 2.0f);
    OutlinedText& don = text("DON", 64, red, 2.0f);
    const float logo_w = z.width + don.width;
    z.draw({.x = (sw - logo_w) / 2.0f, .y = y});
    don.draw({.x = (sw - logo_w) / 2.0f + z.width, .y = y});
    y += z.height + 6 * u;
    OutlinedText& title = text(zhud::text("login_title"), 26, ink);
    title.draw({.x = (sw - title.width) / 2.0f, .y = y});
    y += title.height + 6 * u;
    OutlinedText& sub = text(zhud::text(required ? "login_required" : "login_optional"), 18, muted);
    sub.draw({.x = (sw - sub.width) / 2.0f, .y = y});
    y += sub.height + 34 * u;

    const bool caret_on = static_cast<long long>(get_current_ms() / 530) % 2 == 0;
    const float field_h = 60 * u;
    auto field = [&](int row, const char* label_key, const std::string& shown, const std::string& placeholder) {
        OutlinedText& l = text(zhud::text(label_key), 19, muted);
        l.draw({.x = inner_x + 4 * u, .y = y});
        y += l.height + 8 * u;
        const Rect r{inner_x, y, inner_w, field_h};
        row_rect[row] = r;
        const bool on = focus == row && !sending;
        draw_rectangle_rounded(r, 0.3f, 10, field_bg);
        draw_rectangle_rounded_lines_ex(r, 0.3f, 10, (on ? 2.5f : 1.2f) * u, on ? red : Color{255, 255, 255, 40});
        const float tx = r.x + 20 * u;
        float caret_x = tx;
        if (!shown.empty()) {
            OutlinedText& t = text(shown, 26, ink);
            t.draw({.x = tx, .y = r.y + (field_h - t.height) / 2.0f});
            caret_x += t.width + 2 * u;
        } else if (!on && !placeholder.empty()) {
            OutlinedText& t = text(placeholder, 24, Color{110, 98, 122, 255});
            t.draw({.x = tx, .y = r.y + (field_h - t.height) / 2.0f});
        }
        if (on && caret_on) {
            draw_rectangle(Rect{caret_x, r.y + 14 * u, 2.5f * u, field_h - 28 * u}, ink);
        }
        y += field_h + 22 * u;
    };
    field(EMAIL, "login_email", email, "name@example.com");
    std::string dots;
    for (size_t i = 0; i < std::min<size_t>(password.size(), 32); ++i) {
        dots += "•";
    }
    field(PASSWORD, "login_password", dots, "");

    // the last error (a red note) or room for it
    const float note_h = 44 * u;
    if (!message.empty() && !sending) {
        const Rect r{inner_x, y - 8 * u, inner_w, note_h};
        draw_rectangle_rounded(r, 0.4f, 8, Color{232, 68, 58, 40});
        OutlinedText& t = text(message, 20, Color{255, 178, 170, 255});
        t.draw({.x = (sw - t.width) / 2.0f, .y = r.y + (note_h - t.height) / 2.0f});
    }
    y += note_h + 6 * u;

    // log in: the red pill
    const float button_h = 64 * u;
    {
        const Rect r{inner_x, y, inner_w, button_h};
        row_rect[LOGIN] = r;
        const bool on = focus == LOGIN && !sending;
        draw_rectangle_rounded(r, 1.0f, 24, sending ? Color{150, 60, 56, 255} : on ? red_hi : red);
        if (on) {
            draw_rectangle_rounded_lines_ex(Rect{r.x - 4 * u, r.y - 4 * u, r.width + 8 * u, r.height + 8 * u}, 1.0f, 24,
                                            2.5f * u, gold);
        }
        OutlinedText& t = text(zhud::text(sending ? "login_sending" : "login_button"), 28, Color::WHITE, 1.0f);
        t.draw({.x = r.x + (inner_w - t.width) / 2.0f, .y = r.y + (button_h - t.height) / 2.0f});
    }
    y += button_h + 20 * u;

    // sign up / forgot password: outlined pills
    const float small_h = 50 * u;
    const float gap = 16 * u;
    const float half = (inner_w - gap) / 2.0f;
    auto pill = [&](int row, const std::string& label, float bx) {
        const Rect r{bx, y, half, small_h};
        row_rect[row] = r;
        const bool on = focus == row && !sending;
        if (on) {
            draw_rectangle_rounded(r, 1.0f, 20, Color{255, 255, 255, 18});
        }
        draw_rectangle_rounded_lines_ex(r, 1.0f, 20, (on ? 2.0f : 1.2f) * u, on ? gold : Color{255, 255, 255, 70});
        OutlinedText& t = text(label, 21, on ? gold : ink);
        t.draw({.x = r.x + (half - t.width) / 2.0f, .y = r.y + (small_h - t.height) / 2.0f});
    };
    pill(REGISTER, zhud::text("login_register"), inner_x);
    pill(FORGOT, zhud::text("login_forgot"), inner_x + half + gap);
    y += small_h + 22 * u;

    // quit / skip: a quiet text button
    {
        const bool on = focus == SKIP_OR_QUIT && !sending;
        OutlinedText& t = text(zhud::text(required ? "login_quit" : "login_skip"), 20, on ? gold : muted);
        const Rect r{(sw - t.width) / 2.0f - 16 * u, y, t.width + 32 * u, t.height + 12 * u};
        row_rect[SKIP_OR_QUIT] = r;
        t.draw({.x = r.x + 16 * u, .y = r.y + 6 * u});
        if (on) {
            draw_rectangle(Rect{r.x + 16 * u, r.y + r.height - 4 * u, t.width, 2 * u}, gold);
        }
    }

    OutlinedText& hint = text(zhud::text("login_hint"), 18, Color{130, 118, 142, 255});
    hint.draw({.x = (sw - hint.width) / 2.0f, .y = sh - 64 * u});
}
