#include "pause_menu.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "../../libs/audio/audio.h"
#include "../../libs/gfx/texture.h"
#include "../../libs/global_data.h"
#include "../../libs/input/input.h"

namespace {

    // built-in texts, for skins without pause_menu_* (or practice menu) keys
    const std::map<std::string, std::map<std::string, std::string>>& builtin_texts() {
        static const std::map<std::string, std::map<std::string, std::string>> t = {
            {"pause_menu_title", {{"en", "Paused"}, {"ja", "一時停止中"}, {"zh", "暂停中"}, {"ko", "일시 정지"}}},
            {"pause_menu_resume", {{"en", "Resume"}, {"ja", "演奏にもどる"}, {"zh", "继续演奏"}, {"ko", "계속 연주"}}},
            {"pause_menu_restart",
             {{"en", "From the Top"}, {"ja", "最初から演奏"}, {"zh", "从头演奏"}, {"ko", "처음부터 연주"}}},
            {"pause_menu_back",
             {{"en", "Another Song"}, {"ja", "ほかの曲を演奏"}, {"zh", "选其他歌曲"}, {"ko", "다른 곡 연주"}}},
        };
        return t;
    }

    // the skin's own text for key, else the practice menu's text for the same action, else built in
    std::string menu_text(const std::string& key, const char* practice_key, const std::string& lang) {
        std::string s = tex.skin_text(key, lang);
        if (s.empty() && practice_key) {
            s = tex.skin_text(practice_key, lang);
        }
        if (!s.empty()) {
            return s;
        }
        const auto& t = builtin_texts().at(key);
        auto it = t.find(lang);
        return it != t.end() ? it->second : t.at("en");
    }

    TextureObject* sprite(const char* name) {
        return tex.has_texture(name) ? tex.get_texture(name) : nullptr;
    }

    // draws t with its top left at (x, y) (texture.json positions are offsets the draw adds)
    void draw_at(TextureObject* t, float x, float y) {
        tex.draw_texture(t, {.x = x - static_cast<float>(t->x[0]), .y = y - static_cast<float>(t->y[0])});
    }

} // namespace

void PauseMenu::open_menu(double now_ms) {
    if (!pause_start) {
        pause_start = now_ms;
    }
    open = true;
    countdown_start.reset();
    index = ROW_RESUME;
}

void PauseMenu::reset() {
    open = false;
    countdown_start.reset();
    pause_start.reset();
    index = ROW_RESUME;
}

PauseMenu::Action PauseMenu::update(double now_ms) {
    const auto& keys = global_data.config->keys;
    if (counting()) {
        if (check_key_pressed(keys.back_key) || check_key_pressed(keys.pause_key)) {
            open_menu(now_ms); // pause again
            return Action::NONE;
        }
        if (now_ms - *countdown_start >= kCountdownMs) {
            countdown_start.reset(); // the caller reads paused_ms, then resets
            return Action::RESUME;
        }
        return Action::NONE;
    }
    if (!open) {
        return Action::NONE;
    }
    if (check_key_pressed(keys.back_key) || check_key_pressed(keys.pause_key)) {
        open = false;
        countdown_start = now_ms;
        return Action::NONE;
    }
    const bool left = menu_left_pressed();
    const bool right = menu_right_pressed();
    if (left != right) {
        audio.play_sound("kat", VolumePreset::SOUND);
        index = (index + (right ? 1 : ROW_COUNT - 1)) % ROW_COUNT;
    }
    if (menu_confirm_pressed()) {
        audio.play_sound("don", VolumePreset::SOUND);
        switch (index) {
        case ROW_RESTART:
            reset();
            return Action::RESTART;
        case ROW_BACK:
            reset();
            return Action::BACK;
        default:
            open = false;
            countdown_start = now_ms;
            break;
        }
    }
    return Action::NONE;
}

void PauseMenu::make_texts() {
    const std::string& l = global_data.config->general.language;
    if (title && lang == l) {
        return;
    }
    lang = l;
    const float s = tex.screen_scale;
    title = std::make_unique<OutlinedText>(menu_text("pause_menu_title", nullptr, lang), static_cast<int>(44 * s),
                                           Color::WHITE, Color::BLACK, false);
    const std::pair<const char*, const char*> keys[ROW_COUNT] = {{"pause_menu_resume", nullptr},
                                                                 {"pause_menu_restart", "practice_menu_restart"},
                                                                 {"pause_menu_back", "practice_menu_another_song"}};
    TextureObject* button = sprite("practice/menu_button");
    const float max_w = (button ? static_cast<float>(button->x2[0]) : 212 * s) - 16 * s;
    for (int i = 0; i < ROW_COUNT; ++i) {
        const std::string label = menu_text(keys[i].first, keys[i].second, lang);
        int fs = static_cast<int>(32 * s);
        rows[i] = std::make_unique<OutlinedText>(label, fs, Color::WHITE, Color::BLACK, false);
        if (rows[i]->width > max_w) { // shrink a long label to fit the button
            fs = std::max(1, static_cast<int>(std::floor(fs * max_w / rows[i]->width)));
            rows[i] = std::make_unique<OutlinedText>(label, fs, Color::WHITE, Color::BLACK, false);
        }
    }
    for (int i = 0; i < 3; ++i) {
        digits[i] = std::make_unique<OutlinedText>(std::to_string(i + 1), static_cast<int>(160 * s), Color::WHITE,
                                                   Color::BLACK, false, 8.0f);
    }
}

void PauseMenu::draw(double now_ms) {
    if (!active()) {
        return;
    }
    make_texts();
    const float w = static_cast<float>(tex.screen_width);
    const float h = static_cast<float>(tex.screen_height);
    const float s = tex.screen_scale;
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::BLACK, open ? 0.55f : 0.35f));

    if (counting()) {
        const double left = kCountdownMs - (now_ms - *countdown_start);
        const int n = std::clamp(static_cast<int>(std::ceil(left / (kCountdownMs / 3.0))), 1, 3);
        // each number pops in and fades over its third of the countdown
        const double phase = 1.0 - (left - (n - 1) * (kCountdownMs / 3.0)) / (kCountdownMs / 3.0);
        const float scale = 1.0f + 0.25f * static_cast<float>(std::max(0.0, 1.0 - phase * 4.0));
        const OutlinedText& d = *digits[n - 1];
        digits[n - 1]->draw({.scale = scale,
                             .x = (w - d.width * scale) / 2.0f,
                             .y = (h - d.height * scale) / 2.0f,
                             .fade = 1.0 - 0.6 * phase});
        return;
    }

    TextureObject* panel = sprite("practice/menu_panel");
    TextureObject* button = sprite("practice/menu_button");
    TextureObject* button_sel = sprite("practice/menu_button_selected");
    if (!button || !button_sel) {
        button = button_sel = nullptr;
    }
    const float pw = panel ? static_cast<float>(panel->x2[0]) : 940 * s;
    const float ph = panel ? static_cast<float>(panel->y2[0]) : 460 * s;
    const float px = (w - pw) / 2.0f;
    const float py = (h - ph) / 2.0f;
    if (panel) {
        draw_at(panel, px, py);
    } else {
        const Rect r{px, py, pw, ph};
        draw_rectangle(r, Color{24, 24, 32, 235});
        draw_rectangle_lines_ex(r, 4 * s, Color::WHITE);
    }
    title->draw({.x = px + (pw - title->width) / 2.0f, .y = py + ph * 0.2f});

    const float bw = button ? static_cast<float>(button->x2[0]) : 212 * s;
    const float bh = button ? static_cast<float>(button->y2[0]) : 102 * s;
    const float gap = 36 * s;
    const float bx0 = px + (pw - (static_cast<float>(ROW_COUNT) * bw + static_cast<float>(ROW_COUNT - 1) * gap)) / 2.0f;
    const float by = py + ph * 0.55f;
    for (int i = 0; i < ROW_COUNT; ++i) {
        const float bx = bx0 + static_cast<float>(i) * (bw + gap);
        const bool sel = i == index;
        if (button) {
            draw_at(sel ? button_sel : button, bx, by);
        } else {
            const Rect r{bx, by, bw, bh};
            draw_rectangle(r, sel ? Color{232, 96, 40, 255} : Color{72, 72, 84, 255});
            draw_rectangle_lines_ex(r, 3 * s, sel ? Color::WHITE : Color::GRAY);
        }
        rows[i]->draw({.x = bx + (bw - rows[i]->width) / 2.0f, .y = by + (bh - rows[i]->height) / 2.0f});
    }
}
