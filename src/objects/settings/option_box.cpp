#include "option_box.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <utility>

#include "../../libs/input/ime_ui.h"
#include "../../libs/input/input.h"
#include "../../libs/math/animation.h"

std::string getKeyString(int key_code);

static constexpr int OPTION_FONT_SIZE_BASE = 30;
static constexpr float DESC_X_BASE = 450.0f;
static constexpr float DESC_Y_BASE = 270.0f;
static constexpr float DESC_FONT_SIZE_BASE = 25.0f;

// These are laid out for the 1280x720 base canvas; scale with the skin so
// higher-resolution skins don't drift out of alignment with the box art.
static int option_font_size() {
    return static_cast<int>(OPTION_FONT_SIZE_BASE * tex.screen_scale);
}

static std::unique_ptr<FadeAnimation> make_flicker() {
    auto fa = std::make_unique<FadeAnimation>(400.0, 0.0, true, false, 1.0, 0.0, std::nullopt, std::nullopt, 0.0);
    fa->start();
    return fa;
}

BaseOptionBox::BaseOptionBox(const std::string& name, std::string description, const std::string& path)
    : description_text(std::move(description)), config_ref(get_config_ref(path)),
      name_text(std::make_unique<OutlinedText>(name, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)),
      t_overlay(tex.get_texture("background/overlay")), t_title(tex.get_texture("background/title")),
      t_title_highlight(tex.get_texture("background/title_highlight")),
      t_button_on(tex.get_texture("option/button_on")), t_button_off(tex.get_texture("option/button_off")) {}

void BaseOptionBox::draw_base() const {
    tex.draw_texture(t_overlay, {.scale = 0.70f});
    if (is_highlighted) {
        tex.draw_texture(t_title_highlight);
    } else {
        tex.draw_texture(t_title);
    }
    float text_x = t_title->x[0] + (t_title->x2[0] / 2.0f) - (name_text->width / 2.0f);
    float text_y = t_title->y[0] + (t_title->y2[0] / 8.0f);
    name_text->draw({.x = text_x, .y = text_y});

    // Z-DON: skin_config "settings_description" (x, y, font_size, width) overrides the hard-coded
    // position / size (a pixel font needs its own grid size); the text wraps at the width (default: to
    // the screen's right edge less the left margin's 1/10), between words or between CJK characters
    float desc_font_size = DESC_FONT_SIZE_BASE * tex.screen_scale;
    float desc_x = DESC_X_BASE * tex.screen_scale;
    float desc_y = DESC_Y_BASE * tex.screen_scale;
    float max_w = 0.0f;
    if (const SkinInfo* info = font_manager.get_text_info("settings_description")) {
        if (info->font_size > 0) {
            desc_font_size = (float)info->font_size;
        }
        if (info->x > 0) {
            desc_x = info->x;
        }
        if (info->y > 0) {
            desc_y = info->y;
        }
        max_w = info->width;
    }
    if (max_w <= 0) {
        max_w = (float)tex.screen_width - desc_x * 1.1f;
    }
    Font font = font_manager.get_font(description_text, (int)desc_font_size);
    if (desc_lines_for != description_text || desc_lines_size != desc_font_size || desc_lines_w != max_w) {
        desc_lines = wrap_description(font, description_text, desc_font_size, max_w);
        desc_lines_for = description_text;
        desc_lines_size = desc_font_size;
        desc_lines_w = max_w;
    }
    const float line_h = std::round(desc_font_size * 1.25f);
    for (size_t i = 0; i < desc_lines.size(); i++) {
        draw_text_ex(font, desc_lines[i].c_str(), {desc_x, desc_y + line_h * (float)i}, desc_font_size, 1,
                     Color::BLACK);
    }
}

// Z-DON: greedy wrap to max_w: break at spaces, or before any character that starts a CJK run
std::vector<std::string> BaseOptionBox::wrap_description(const Font& font, const std::string& text, float size,
                                                         float max_w) {
    std::vector<std::string> lines;
    auto width = [&](const std::string& s) { return measure_text_ex(font, s.c_str(), size, 1).x; };
    if (width(text) <= max_w) {
        lines.push_back(text);
        return lines;
    }
    auto is_cjk = [](uint32_t cp) {
        return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFF00 && cp <= 0xFFEF);
    };
    // split into units: a word with its trailing space, or one CJK character
    std::vector<std::string> units;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
        uint32_t cp = c;
        if (len == 2) {
            cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        } else if (len == 3) {
            cp = ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        } else if (len == 4) {
            cp = ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
        }
        const std::string ch = text.substr(i, len);
        i += len;
        if (is_cjk(cp)) {
            if (!cur.empty()) {
                units.push_back(cur);
                cur.clear();
            }
            units.push_back(ch);
        } else {
            cur += ch;
            if (ch == " ") {
                units.push_back(cur);
                cur.clear();
            }
        }
    }
    if (!cur.empty()) {
        units.push_back(cur);
    }
    std::string line;
    for (const std::string& u : units) {
        if (!line.empty() && width(line + u) > max_w) {
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            lines.push_back(line);
            line.clear();
            if (u == " ") {
                continue;
            }
        }
        line += u;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

void BaseOptionBox::draw_centered(OutlinedText& text, int index) const {
    float tx = t_button_on->x[index] + (t_button_on->x2[index] / 2.0f) - (text.width / 2.0f);
    float ty = t_button_on->y[index] + (t_button_on->y2[index] / 2.0f) - (text.height / 2.0f);
    text.draw({.x = tx, .y = ty});
}

void BaseOptionBox::draw_value_box(OutlinedText& text, const FadeAnimation& flicker) const {
    tex.draw_texture(t_button_off, {.index = 2});
    if (is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker.attribute, .index = 2});
    }
    draw_centered(text, 2);
}

// Z-DON built-in labels for the rows the code adds (Calibrate, Reset to defaults)
// and for the refused-keybind message. zh is Simplified like the skin templates.
std::string settings_builtin_text(const std::string& key) {
    static const std::map<std::string, std::map<std::string, std::string>> t = {
        {"calibrate_name",
         {{"en", "Audio Offset Calibration"},
          {"ja", "音ズレ調整"},
          {"zh", "音频延迟校准"},
          {"ko", "오디오 오프셋 보정"}}},
        {"calibrate_desc",
         {{"en", "Hit don to open the calibration screen."},
          {"ja", "ドンで調整画面を開きます。"},
          {"zh", "敲咚打开校准画面。"},
          {"ko", "쿵으로 보정 화면을 엽니다."}}},
        {"calibrate_button", {{"en", "Calibrate"}, {"ja", "調整"}, {"zh", "校准"}, {"ko", "보정"}}},
        {"reset_name",
         {{"en", "Reset to Defaults"}, {"ja", "初期設定に戻す"}, {"zh", "恢复默认"}, {"ko", "기본값 복원"}}},
        {"reset_desc",
         {{"en", "Restores every binding in this box. Don, pick Yes with kat, don."},
          {"ja", "この項目のキー設定を初期値に戻します。ドン、カッで「はい」、ドン。"},
          {"zh", "将此项的所有按键恢复默认。咚，用咔选“是”，咚。"},
          {"ko", "이 항목의 키 설정을 기본값으로 되돌립니다. 쿵, 딱으로 예, 쿵."}}},
        {"no", {{"en", "No"}, {"ja", "いいえ"}, {"zh", "否"}, {"ko", "아니요"}}},
        {"yes", {{"en", "Yes"}, {"ja", "はい"}, {"zh", "是"}, {"ko", "예"}}},
        {"key_in_use", {{"en", "Already used"}, {"ja", "使用中"}, {"zh", "已被占用"}, {"ko", "이미 사용 중"}}},
    };
    const auto& m = t.at(key);
    auto it = m.find(global_data.config->general.language);
    return it != m.end() ? it->second : m.at("en");
}

std::optional<std::vector<int>> default_binding(const std::string& path) {
    // Mirrors the defaults in get_config() (libs/config.cpp).
    static const std::map<std::string, std::vector<int>> d = {
        {"keys/exit_key", {SDL_SCANCODE_Q}},
        {"keys/fullscreen_key", {SDL_SCANCODE_F11}},
        {"keys/borderless_key", {SDL_SCANCODE_F10}},
        {"keys/pause_key", {SDL_SCANCODE_SPACE}},
        {"keys/back_key", {SDL_SCANCODE_ESCAPE}},
        {"keys/restart_key", {SDL_SCANCODE_F1}},
        {"keys_1p/left_kat", {SDL_SCANCODE_D}},
        {"keys_1p/left_don", {SDL_SCANCODE_F}},
        {"keys_1p/right_don", {SDL_SCANCODE_J}},
        {"keys_1p/right_kat", {SDL_SCANCODE_K}},
        {"keys_2p/left_kat", {SDL_SCANCODE_Z}},
        {"keys_2p/left_don", {SDL_SCANCODE_X}},
        {"keys_2p/right_don", {SDL_SCANCODE_C}},
        {"keys_2p/right_kat", {SDL_SCANCODE_V}},
        {"gamepad_1p/left_kat", {10}},
        {"gamepad_1p/left_don", {16}},
        {"gamepad_1p/right_don", {17}},
        {"gamepad_1p/right_kat", {12}},
        {"gamepad_2p/left_kat", {}},
        {"gamepad_2p/left_don", {}},
        {"gamepad_2p/right_don", {}},
        {"gamepad_2p/right_kat", {}},
    };
    auto it = d.find(path);
    if (it == d.end()) {
        return std::nullopt;
    }
    return it->second;
}

// Lock-out guard: a P1 drum role (keyboard or controller) may not take a key
// another P1 drum role already uses, or the menus (kat moves, don confirms)
// could become unreachable. Rebinding a role to its own key is fine.
static bool p1_drum_key_taken(const std::string& path, int key) {
    for (const char* prefix : {"keys_1p/", "gamepad_1p/"}) {
        if (!path.starts_with(prefix)) {
            continue;
        }
        for (const char* role : {"left_kat", "left_don", "right_don", "right_kat"}) {
            std::string other = std::string(prefix) + role;
            if (other == path) {
                continue;
            }
            std::vector<int> keys = get_config_ref(other).get_vec();
            if (std::ranges::find(keys, key) != keys.end()) {
                return true;
            }
        }
    }
    return false;
}

static std::vector<int> load_key_binding(const ConfigRef& ref) {
    if (ref.is_int()) {
        return {ref.get_int()};
    }
    return ref.get_vec();
}

static void save_key_binding(ConfigRef& ref, const std::vector<int>& value) {
    if (ref.is_int()) {
        ref.set_int(value.empty() ? 0 : value[0]);
    } else {
        ref.set_vec(value);
    }
}

BoolOptionBox::BoolOptionBox(const std::string& name, const std::string& description, const std::string& path,
                             const std::string& true_label, const std::string& false_label)
    : BaseOptionBox(name, description, path), value(config_ref.get_bool()),
      on_text(std::make_unique<OutlinedText>(true_label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)),
      off_text(
          std::make_unique<OutlinedText>(false_label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4)) {}

void BoolOptionBox::confirm() {
    config_ref.set_bool(value);
}

void BoolOptionBox::move_left() {
    value = false;
}
void BoolOptionBox::move_right() {
    value = true;
}

void BoolOptionBox::draw() {
    draw_base();

    if (!value) {
        tex.draw_texture(t_button_on, {.index = 0});
    } else {
        tex.draw_texture(t_button_off, {.index = 0});
    }
    draw_centered(*off_text, 0);

    if (value) {
        tex.draw_texture(t_button_on, {.index = 1});
    } else {
        tex.draw_texture(t_button_off, {.index = 1});
    }
    draw_centered(*on_text, 1);
}

static std::string int_display(int v) {
    return std::to_string(v);
}

IntOptionBox::IntOptionBox(const std::string& name, const std::string& description, const std::string& path,
                           const std::map<std::string, std::string>& values)
    : BaseOptionBox(name, description, path), value(config_ref.get_int()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    if (!values.empty()) {
        for (auto& [k, v] : values) {
            try {
                value_list.emplace_back(std::stoi(k), v);
            } catch (const std::exception&) {
                continue;
            }
        }

        std::ranges::sort(value_list, [](const auto& a, const auto& b) { return a.first < b.first; });

        if (!value_list.empty()) {
            bool matched = false;
            for (int i = 0; i < std::ssize(value_list); i++) {
                if (value_list[i].first == value) {
                    value_index = i;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                value_index = 0;
                value = value_list[0].first;
            }
        }
    }
    rebuild_text();
}

void IntOptionBox::rebuild_text() {
    std::string label = value_list.empty() ? int_display(value) : value_list[value_index].second;
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void IntOptionBox::confirm() {
    config_ref.set_int(value);
}

void IntOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    // The engine can adjust a value after it is set (e.g. a display rejecting a refresh rate).
    if (!is_highlighted && config_ref.get_int() != value) {
        value = config_ref.get_int();
        for (int i = 0; i < std::ssize(value_list); i++) {
            if (value_list[i].first == value) {
                value_index = i;
            }
        }
        rebuild_text();
    }
}

void IntOptionBox::move_left() {
    if (!value_list.empty()) {
        value_index = std::max(0, value_index - 1);
        value = value_list[value_index].first;
    } else {
        value--;
    }
    rebuild_text();
}

void IntOptionBox::move_right() {
    if (!value_list.empty()) {
        value_index = std::min(static_cast<int>(value_list.size()) - 1, value_index + 1);
        value = value_list[value_index].first;
    } else {
        value++;
    }
    rebuild_text();
}

void IntOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

StrOptionBox::StrOptionBox(const std::string& name, const std::string& description, const std::string& path,
                           const std::map<std::string, std::string>& values)
    : BaseOptionBox(name, description, path), value(config_ref.get_str()), input_string(value), value_text(nullptr),
      flicker_fade(make_flicker()) {
    if (!values.empty()) {
        for (auto& [k, v] : values) {
            value_list.emplace_back(k, v);
        }
        bool matched = false;
        for (int i = 0; i < std::ssize(value_list); i++) {
            if (value_list[i].first == value) {
                value_index = i;
                matched = true;
                break;
            }
        }
        if (!matched) {
            value_index = 0;
            value = value_list[0].first;
        }
    }
    rebuild_text();
}

void StrOptionBox::rebuild_text() {
    std::string label = value_list.empty() ? input_string : value_list[value_index].second;
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void StrOptionBox::confirm() {
    config_ref.set_str(value);
}

bool StrOptionBox::needs_text_input() const {
    return value_list.empty();
}

void StrOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (is_highlighted && value_list.empty()) {
        std::string before = input_string;
        TextEditAction action = poll_text_edit(input_string);
#if defined(PLATFORM_ANDROID) || defined(YATAIDON_PLATFORM_IOS)
        if (action == TextEditAction::None && (is_l_don_pressed() || is_r_don_pressed())) {
            action = TextEditAction::Confirm;
        }
#endif
        if (action == TextEditAction::Confirm) {
            value = input_string;
            confirm();
            is_highlighted = false;
            set_keyboard_visible(false);
        }
        if (action == TextEditAction::Cancel) {
            input_string = value;
            is_highlighted = false;
            set_keyboard_visible(false);
        }
        if (input_string != before) {
            rebuild_text();
        }
    }
}

void StrOptionBox::move_left() {
    if (!value_list.empty()) {
        value_index = std::max(0, value_index - 1);
        value = value_list[value_index].first;
        rebuild_text();
    }
}

void StrOptionBox::move_right() {
    if (!value_list.empty()) {
        value_index = std::min(static_cast<int>(value_list.size()) - 1, value_index + 1);
        value = value_list[value_index].first;
        rebuild_text();
    }
}

void StrOptionBox::draw() {
    draw_base();
    if (is_highlighted && value_list.empty()) {
        ime_ui_set_field({static_cast<float>(t_button_on->x[2]), static_cast<float>(t_button_on->y[2]),
                          static_cast<float>(t_button_on->x2[2]), static_cast<float>(t_button_on->y2[2])});
    }

    draw_value_box(*value_text, *flicker_fade);
}

KeybindOptionBox::KeybindOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), path(path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeybindOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < std::ssize(value); i++) {
        try {
            label += getKeyString(value[i]);
        } catch (...) {
            label += std::to_string(value[i]);
        }
        if (i + 1 < std::ssize(value)) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeybindOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeybindOptionBox::reload() {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeybindOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (message_until > 0.0 && current_time >= message_until) {
        message_until = 0.0;
        rebuild_text();
    }
    if (!is_highlighted) {
        capture_armed = false;
        return;
    }
    if (!capture_armed) {
        // Drop keys queued before the press that opened this box.
        while (key_pressed_next() > 0) {
        }
        capture_armed = true;
        return;
    }
    int key = key_pressed_next();
    if (key <= 0) {
        return;
    }
    is_highlighted = false;
    capture_done = true;
    if (p1_drum_key_taken(path, key)) {
        audio.play_sound("kat", VolumePreset::SOUND);
        value_text = std::make_unique<OutlinedText>(settings_builtin_text("key_in_use"), option_font_size(),
                                                    Color::WHITE, Color::BLACK, false, 4, -4);
        message_until = current_time + 1500.0;
        return;
    }
    value = {key};
    confirm();
    audio.play_sound("don", VolumePreset::SOUND);
    rebuild_text();
}

void KeybindOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

KeyBindControllerOptionBox::KeyBindControllerOptionBox(const std::string& name, const std::string& description,
                                                       const std::string& path)
    : BaseOptionBox(name, description, path), path(path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeyBindControllerOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < std::ssize(value); i++) {
        label += std::to_string(value[i]);
        if (i + 1 < std::ssize(value)) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeyBindControllerOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeyBindControllerOptionBox::reload() {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeyBindControllerOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (message_until > 0.0 && current_time >= message_until) {
        message_until = 0.0;
        rebuild_text();
    }
    if (!is_highlighted) {
        capture_armed = false;
        return;
    }
    if (!capture_armed) {
        // Drop the press that opened this box, plus anything buffered
        // before it, so capture starts clean.
        take_gamepad_button_pressed();
        capture_armed = true;
        return;
    }
    {
        int btn = take_gamepad_button_pressed();
        if (btn >= 0) {
            is_highlighted = false;
            capture_done = true;
            if (p1_drum_key_taken(path, btn)) {
                audio.play_sound("kat", VolumePreset::SOUND);
                value_text = std::make_unique<OutlinedText>(settings_builtin_text("key_in_use"), option_font_size(),
                                                            Color::WHITE, Color::BLACK, false, 4, -4);
                message_until = current_time + 1500.0;
                return;
            }
            value = {btn};
            confirm();
            audio.play_sound("don", VolumePreset::SOUND);
            rebuild_text();
        }
    }
}

void KeyBindControllerOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

FloatOptionBox::FloatOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), value(config_ref.get_float()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    rebuild_text();
}

void FloatOptionBox::rebuild_text() {
    std::string label = std::to_string(static_cast<int>(value * 100)) + "%";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void FloatOptionBox::confirm() {
    config_ref.set_float(value);
    audio.set_volume_presets(global_data.config->volume); // Z-DON: what is playing follows at once
}

void FloatOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

// Volumes: 0-100 %.
void FloatOptionBox::move_left() {
    value = std::clamp(std::round((value * 100.0f) - 1.0f), 0.0f, 100.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::move_right() {
    value = std::clamp(std::round((value * 100.0f) + 1.0f), 0.0f, 100.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

AudioOffsetOptionBox::AudioOffsetOptionBox(const std::string& name, const std::string& description,
                                           const std::string& path)
    : BaseOptionBox(name, description, path), value(config_ref.get_int()), value_text(nullptr),
      flicker_fade(make_flicker()) {
    rebuild_text();
}

void AudioOffsetOptionBox::rebuild_text() {
    std::string label = (value >= 0 ? "+" : "") + std::to_string(value) + " ms";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void AudioOffsetOptionBox::confirm() {
    if (is_highlighted) {
        return; // entering -- act on exit only
    }
    config_ref.set_int(value);
}

void AudioOffsetOptionBox::move_left() {
    value -= 1;
    rebuild_text();
}

void AudioOffsetOptionBox::move_right() {
    value += 1;
    rebuild_text();
}

void AudioOffsetOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void AudioOffsetOptionBox::draw() {
    draw_base();
    draw_value_box(*value_text, *flicker_fade);
}

CalibrateOptionBox::CalibrateOptionBox(const std::string& name, const std::string& description, const std::string& path,
                                       Screens calibrate_screen)
    : BaseOptionBox(name, description, path), calibrate_screen(calibrate_screen),
      button_text(std::make_unique<OutlinedText>(settings_builtin_text("calibrate_button"), option_font_size(),
                                                 Color::WHITE, Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {}

void CalibrateOptionBox::confirm() {
    // select_option() highlighted this row: go straight to calibration.
    if (is_highlighted) {
        is_highlighted = false;
        wants_screen_change = true;
    }
}

std::optional<Screens> CalibrateOptionBox::take_screen_change() {
    if (!wants_screen_change) {
        return std::nullopt;
    }
    wants_screen_change = false;
    return calibrate_screen;
}

void CalibrateOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void CalibrateOptionBox::draw() {
    draw_base();
    draw_value_box(*button_text, *flicker_fade);
}

ResetBindingsOptionBox::ResetBindingsOptionBox(const std::string& name, const std::string& description,
                                               std::vector<std::string> paths)
    : BaseOptionBox(name, description, paths.front()), paths(std::move(paths)),
      no_text(std::make_unique<OutlinedText>(settings_builtin_text("no"), option_font_size(), Color::WHITE,
                                             Color::BLACK, false, 4, -4)),
      yes_text(std::make_unique<OutlinedText>(settings_builtin_text("yes"), option_font_size(), Color::WHITE,
                                              Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {}

void ResetBindingsOptionBox::confirm() {
    if (is_highlighted) {
        yes = false; // entering: the choice starts on No
        return;
    }
    if (!yes) {
        return;
    }
    yes = false;
    for (const std::string& p : paths) {
        if (auto def = default_binding(p)) {
            ConfigRef ref = get_config_ref(p);
            save_key_binding(ref, *def);
        }
    }
    reload_siblings = true;
}

void ResetBindingsOptionBox::move_left() {
    yes = false;
}

void ResetBindingsOptionBox::move_right() {
    yes = true;
}

void ResetBindingsOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void ResetBindingsOptionBox::draw() {
    draw_base();
    tex.draw_texture(t_button_off, {.index = 0});
    tex.draw_texture(t_button_off, {.index = 1});
    if (is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker_fade->attribute, .index = yes ? 1 : 0});
    }
    draw_centered(*no_text, 0);
    draw_centered(*yes_text, 1);
}
