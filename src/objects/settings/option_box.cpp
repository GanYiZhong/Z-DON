#include "option_box.h"

#include <algorithm>
#include <cmath>

#include "../../libs/animation.h"
#include "../../libs/audio.h"
#include "../../libs/ime_ui.h"
#include "../../libs/input.h"

std::string getKeyString(int key_code);

static constexpr int OPTION_FONT_SIZE_BASE = 30;
static constexpr float DESC_X_BASE = 450.0f;
static constexpr float DESC_Y_BASE = 270.0f;
static constexpr float DESC_FONT_SIZE_BASE = 25.0f;

// These are laid out for the 1280x720 base canvas; scale with the skin so
// higher-resolution skins don't drift out of alignment with the box art.
static int option_font_size() {
    return (int)(OPTION_FONT_SIZE_BASE * tex.screen_scale);
}

static std::unique_ptr<FadeAnimation> make_flicker() {
    auto fa = std::make_unique<FadeAnimation>(400.0, 0.0, true, false, 1.0, 0.0, std::nullopt, std::nullopt, 0.0);
    fa->start();
    return fa;
}

BaseOptionBox::BaseOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : description_text(description), config_ref(get_config_ref(path)),
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

    float desc_font_size = DESC_FONT_SIZE_BASE * tex.screen_scale;
    ray::Font font = font_manager.get_font(description_text, (int)desc_font_size);
    draw_text_ex(font, description_text.c_str(), {DESC_X_BASE * tex.screen_scale, DESC_Y_BASE * tex.screen_scale},
                 desc_font_size, 1, Color::BLACK);
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
            for (int i = 0; i < (int)value_list.size(); i++) {
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
        value_index = std::min((int)value_list.size() - 1, value_index + 1);
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
        for (int i = 0; i < (int)value_list.size(); i++) {
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
        value_index = std::min((int)value_list.size() - 1, value_index + 1);
        value = value_list[value_index].first;
        rebuild_text();
    }
}

void StrOptionBox::draw() {
    draw_base();
    if (is_highlighted && value_list.empty()) {
        ime_ui_set_field(
            {(float)t_button_on->x[2], (float)t_button_on->y[2], (float)t_button_on->x2[2], (float)t_button_on->y2[2]});
    }

    draw_value_box(*value_text, *flicker_fade);
}

KeybindOptionBox::KeybindOptionBox(const std::string& name, const std::string& description, const std::string& path)
    : BaseOptionBox(name, description, path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeybindOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < (int)value.size(); i++) {
        try {
            label += getKeyString(value[i]);
        } catch (...) {
            label += std::to_string(value[i]);
        }
        if (i + 1 < (int)value.size()) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeybindOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeybindOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
    if (is_highlighted) {
        int key = key_pressed_next();
        if (key > 0) {
            value = {key};
            confirm();
            audio.play_sound("don", VolumePreset::SOUND);
            rebuild_text();
            is_highlighted = false;
        }
    }
}

void KeybindOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

KeyBindControllerOptionBox::KeyBindControllerOptionBox(const std::string& name, const std::string& description,
                                                       const std::string& path)
    : BaseOptionBox(name, description, path), value_text(nullptr), flicker_fade(make_flicker()) {
    value = load_key_binding(config_ref);
    rebuild_text();
}

void KeyBindControllerOptionBox::rebuild_text() {
    std::string label;
    for (int i = 0; i < (int)value.size(); i++) {
        label += std::to_string(value[i]);
        if (i + 1 < (int)value.size()) {
            label += ", ";
        }
    }
    value_text = std::make_unique<OutlinedText>(label.empty() ? "none" : label, option_font_size(), Color::WHITE,
                                                Color::BLACK, false, 4, -4);
}

void KeyBindControllerOptionBox::confirm() {
    save_key_binding(config_ref, value);
}

void KeyBindControllerOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
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
            value = {btn};
            confirm();
            audio.play_sound("don", VolumePreset::SOUND);
            rebuild_text();
            is_highlighted = false;
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
    std::string label = std::to_string((int)(value * 100)) + "%";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void FloatOptionBox::confirm() {
    config_ref.set_float(value);
    audio.set_volume_presets(global_data.config->volume); // Z-DON: what is playing follows at once
}

void FloatOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void FloatOptionBox::move_left() {
    value = std::round((value * 100.0f) - 1.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::move_right() {
    value = std::round((value * 100.0f) + 1.0f) / 100.0f;
    rebuild_text();
}

void FloatOptionBox::draw() {
    draw_base();

    draw_value_box(*value_text, *flicker_fade);
}

AudioOffsetOptionBox::AudioOffsetOptionBox(const std::string& name, const std::string& description,
                                           const std::string& path, Screens calibrate_screen)
    : BaseOptionBox(name, description, path), value(config_ref.get_int()), calibrate_screen(calibrate_screen),
      value_text(nullptr), calibrate_text(std::make_unique<OutlinedText>("Calibrate", option_font_size(), Color::WHITE,
                                                                         Color::BLACK, false, 4, -4)),
      flicker_fade(make_flicker()) {
    rebuild_text();
}

void AudioOffsetOptionBox::rebuild_text() {
    std::string label = (value >= 0 ? "+" : "") + std::to_string(value) + " ms";
    value_text = std::make_unique<OutlinedText>(label, option_font_size(), Color::WHITE, Color::BLACK, false, 4, -4);
}

void AudioOffsetOptionBox::confirm() {
    if (is_highlighted) {
        return; // entering — act on exit only
    }
    if (offset_highlighted) {
        config_ref.set_int(value);
    } else {
        wants_screen_change = true;
        pending_screen = calibrate_screen;
    }
}

void AudioOffsetOptionBox::move_left() {
    if (offset_highlighted) {
        value -= 1;
        rebuild_text();
    } else {
        offset_highlighted = true;
    }
}

void AudioOffsetOptionBox::move_right() {
    if (offset_highlighted) {
        offset_highlighted = false;
    }
}

void AudioOffsetOptionBox::update(double current_time) {
    flicker_fade->update(current_time);
}

void AudioOffsetOptionBox::draw() {
    draw_base();

    // Offset value button (index 0)
    if (offset_highlighted && is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker_fade->attribute, .index = 0});
    } else {
        tex.draw_texture(t_button_off, {.index = 0});
    }
    draw_centered(*value_text, 0);

    // Calibrate button (index 1)
    if (!offset_highlighted && is_highlighted) {
        tex.draw_texture(t_button_on, {.fade = flicker_fade->attribute, .index = 1});
    } else {
        tex.draw_texture(t_button_off, {.index = 1});
    }
    draw_centered(*calibrate_text, 1);
}
