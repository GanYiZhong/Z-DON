#include "input_test.h"

#include <format>

#include "../libs/file/config.h"
#include "../libs/input/input.h"
#include "../libs/input/zdon_usio.h"

void InputTestScreen::on_screen_start() {
    // Loads the screen's sounds (don/kat come from the skin root), which the
    // hit handlers below play. Without it the screen was silent - the
    // previous screen's on_screen_end had already unloaded everything.
    Screen::on_screen_start();
    tex.load_animations("game");
    tex.load_folder("game", "practice");
    tex.load_folder("settings", "background");

    t_background = tex.get_texture("background/background");
    t_large_drum = tex.get_texture("practice/large_drum");

    // Centre the drum by its drawn size (x2/y2: a skin may give it a size other than the PNG's).
    if (!t_large_drum) {
        return;
    }
    auto& drum = *t_large_drum;
    drum_x_offset = (tex.screen_width - drum.x2[0]) / 2.0f - drum.x[0];
    drum_y_offset = (tex.screen_height - drum.y2[0]) / 2.0f - drum.y[0];
}

std::optional<Screens> InputTestScreen::update() {
    if (auto result = Screen::update()) {
        return result;
    }

    if (check_key_pressed(global_data.config->keys.back_key)) {
        return on_screen_end(Screens::SETTINGS);
    }

    double current_ms = get_current_ms();
    for (auto it = hit_effects.begin(); it != hit_effects.end();) {
        (*it)->update(current_ms);
        if ((*it)->is_finished()) {
            it = hit_effects.erase(it);
        } else {
            ++it;
        }
    }

    float strength = 1.0f;
    if (is_l_don_pressed(PlayerNum::ALL, &strength)) {
        hit_effects.push_back(
            std::make_unique<InputTestDrumEffect>(DrumType::DON, Side::LEFT, drum_x_offset, drum_y_offset));
        audio.play_sound("don", VolumePreset::HITSOUND, strength);
    }

    if (is_r_don_pressed(PlayerNum::ALL, &strength)) {
        hit_effects.push_back(
            std::make_unique<InputTestDrumEffect>(DrumType::DON, Side::RIGHT, drum_x_offset, drum_y_offset));
        audio.play_sound("don", VolumePreset::HITSOUND, strength);
    }

    if (is_l_kat_pressed(PlayerNum::ALL, &strength)) {
        hit_effects.push_back(
            std::make_unique<InputTestDrumEffect>(DrumType::KAT, Side::LEFT, drum_x_offset, drum_y_offset));
        audio.play_sound("kat", VolumePreset::HITSOUND, strength);
    }

    if (is_r_kat_pressed(PlayerNum::ALL, &strength)) {
        hit_effects.push_back(
            std::make_unique<InputTestDrumEffect>(DrumType::KAT, Side::RIGHT, drum_x_offset, drum_y_offset));
        audio.play_sound("kat", VolumePreset::HITSOUND, strength);
    }

    return std::nullopt;
}

void InputTestScreen::draw() {
    tex.draw_texture(t_background);
    tex.draw_texture(t_large_drum, {.x = drum_x_offset, .y = drum_y_offset});
    for (auto& effect : hit_effects) {
        effect->draw(0);
    }
    if (global_data.config && global_data.config->usio.enabled) {
        draw_usio(get_current_ms());
    }
}

// Z-DON: the USIO test: every drum sensor's reading against the threshold, the cabinet switches and the coin
// counters, laid out on the 1280x720 base canvas
void InputTestScreen::draw_usio(double now) {
    const float sc = tex.screen_scale;
    auto px = [sc](float v) { return static_cast<int>(v * sc); };
    auto text = [&](const std::string& s, float x, float y, float size, Color c) {
        draw_text(s.c_str(), px(x), px(y), px(size), c);
    };
    const Color panel{0, 0, 0, 190};
    const Color dim{150, 150, 150, 255};
    const Color lit{90, 230, 120, 255};

    const zdon_usio::Snapshot u = zdon_usio::snapshot();
    draw_rectangle(px(0), px(0), px(1280), px(214), panel);
    draw_rectangle(px(0), px(606), px(1280), px(114), panel);
    if (!u.active) {
        text("USIO is not running: check [usio] dll in config.toml and latest.log", 20, 20, 22,
             Color{255, 120, 100, 255});
        return;
    }
    const bool one_threshold = std::ranges::all_of(u.threshold, [&](int t) { return t == u.threshold[0]; });
    text(std::format("USIO  {}  fw {}  {}  {:.0f} reads/s  threshold {}", u.board.empty() ? "(no name)" : u.board,
                     u.firmware, u.connected ? "connected" : "NOT CONNECTED", u.reads_per_sec,
                     one_threshold ? std::to_string(u.threshold[0]) : std::string("per sensor")),
         20, 8, 20, Color::WHITE);

    // per player: left kat, left don, right don, right kat; bars over 0..32768 (a louder hit runs off the end)
    static constexpr std::array<const char*, 4> names{"L KA", "L DON", "R DON", "R KA"};
    constexpr float full = 32768.0f;
    constexpr float bar_w = 250.0f;
    for (int p = 0; p < 2; p++) {
        const float x0 = p == 0 ? 20.0f : 660.0f;
        text(p == 0 ? "1P" : "2P", x0, 36, 20, Color{255, 220, 120, 255});
        // columns after the bar: the reading now, the held peak, hits counted, the last hit's value
        static constexpr std::array<std::pair<const char*, float>, 4> columns{
            std::pair{"now", 0.0f}, std::pair{"peak", 70.0f}, std::pair{"hits", 140.0f}, std::pair{"last", 200.0f}};
        for (const auto& [head, cx] : columns) {
            text(head, x0 + 60.0f + bar_w + 12.0f + cx, 40, 15, dim);
        }
        for (int i = 0; i < 4; i++) {
            const size_t ch = static_cast<size_t>((p * 4) + i);
            if (u.max_since[ch] >= usio_hold[ch] || now > usio_hold_until[ch]) {
                usio_hold[ch] = u.max_since[ch];
                usio_hold_until[ch] = now + 1500.0;
            }
            const float y = 62.0f + (static_cast<float>(i) * 36.0f);
            const bool kat = i == 0 || i == 3;
            const Color fill = kat ? Color{80, 170, 255, 255} : Color{255, 90, 70, 255};
            const float bx = x0 + 60.0f;
            text(names[static_cast<size_t>(i)], x0, y + 3, 18, Color::WHITE);
            draw_rectangle(px(bx), px(y), px(bar_w), px(24), Color{50, 50, 50, 255});
            const float hold_w = std::min(1.0f, static_cast<float>(usio_hold[ch]) / full) * bar_w;
            draw_rectangle(px(bx), px(y), px(hold_w), px(24), Color{fill.r, fill.g, fill.b, 110});
            const float now_w = std::min(1.0f, static_cast<float>(u.value[ch]) / full) * bar_w;
            draw_rectangle(px(bx), px(y), px(now_w), px(24), fill);
            const float th_x = bx + (std::min(1.0f, static_cast<float>(u.threshold[ch]) / full) * bar_w);
            draw_rectangle(px(th_x), px(y - 3), std::max(1, px(2)), px(30), Color{255, 230, 0, 255});
            const float tx = bx + bar_w + 12.0f;
            text(std::to_string(u.value[ch]), tx, y + 5, 15, Color::WHITE);
            text(std::to_string(usio_hold[ch]), tx + 70.0f, y + 5, 15, Color::WHITE);
            text(std::to_string(u.hits[ch]), tx + 140.0f, y + 5, 15, Color::WHITE);
            text(std::to_string(u.last_hit[ch]), tx + 200.0f, y + 5, 15, Color::WHITE);
        }
    }

    // switches, lit while held (TaikoArcadeLoader's bit layout)
    static constexpr std::array<std::pair<const char*, uint32_t>, 5> switches{
        std::pair{"TEST", zdon_usio::SW_TEST}, std::pair{"SERVICE", zdon_usio::SW_SERVICE},
        std::pair{"UP", zdon_usio::SW_UP}, std::pair{"DOWN", zdon_usio::SW_DOWN},
        std::pair{"ENTER", zdon_usio::SW_ENTER}};
    if (u.has_switches) {
        float x = 20.0f;
        for (const auto& [label, bit] : switches) {
            const bool on = (u.switches & bit) != 0;
            draw_rectangle(px(x), px(618), px(120), px(36), on ? lit : Color{50, 50, 50, 255});
            text(label, x + 10.0f, 627, 18, on ? Color::BLACK : dim);
            x += 130.0f;
        }
        text(std::format("raw {:#010x}", u.switches), 680, 627, 18, Color::WHITE);
    } else {
        text("no bnusio_GetSwIn: switches unavailable", 20, 627, 18, dim);
    }
    const int credits = global_data.config->general.zdon_coin_credits;
    text(std::format("coin counter {}   service counter {}   credits {}{}",
                     u.coin_total < 0 ? std::string("-") : std::to_string(u.coin_total),
                     u.service_total < 0 ? std::string("-") : std::to_string(u.service_total), credits,
                     global_data.config->general.zdon_coin_mode ? "" : " (coin mode off)"),
         20, 668, 18, Color::WHITE);
    text("yellow line: threshold   TEST or back key: return", 860, 690, 14, dim);
}
