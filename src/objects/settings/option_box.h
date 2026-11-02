#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../libs/gfx/text.h"
#include "../../libs/screen.h"
#include "config_ref.h"

class BaseOptionBox {
protected:
    std::string description_text;
    // Z-DON: the description wrapped to the box (option_box.cpp draw_base), rebuilt when text / size / width change
    mutable std::vector<std::string> desc_lines;
    mutable std::string desc_lines_for;
    mutable float desc_lines_size = 0.0f;
    mutable float desc_lines_w = 0.0f;
    static std::vector<std::string> wrap_description(const Font& font, const std::string& text, float size,
                                                     float max_w);
    ConfigRef config_ref;
    std::unique_ptr<OutlinedText> name_text;

    TextureObject* t_overlay = nullptr;
    TextureObject* t_title = nullptr;
    TextureObject* t_title_highlight = nullptr;
    TextureObject* t_button_on = nullptr;
    TextureObject* t_button_off = nullptr;

    void draw_base() const;
    void draw_centered(OutlinedText& text, int index) const;
    void draw_value_box(OutlinedText& text, const FadeAnimation& flicker) const;

public:
    virtual ~BaseOptionBox() = default;

    bool is_highlighted{false};
    // Z-DON: set by a key/button capture when it ends (bound or refused), so
    // the settings screen can drop the captured press instead of acting on it.
    bool capture_done{false};
    // Z-DON: set when this option rewrote other options' config values (reset to defaults).
    bool reload_siblings{false};

    BaseOptionBox(const std::string& name, std::string description, const std::string& path);

    virtual void confirm() {}
    virtual bool needs_text_input() const { return false; }
    // Z-DON: a held kat repeats move_left/move_right while highlighted (numeric values).
    virtual bool repeats_on_hold() const { return false; }
    // Z-DON: re-read the value from config (after a reset of sibling options).
    virtual void reload() {}
    // Z-DON: a requested screen transition; consumed by the call.
    virtual std::optional<Screens> take_screen_change() { return std::nullopt; }
    virtual std::string text_input_value() const { return {}; }

    virtual void update(double /*current_time*/) {}
    virtual void move_left() {}
    virtual void move_right() {}
    virtual void draw() { draw_base(); }
};

class BoolOptionBox : public BaseOptionBox {
    bool value;
    std::unique_ptr<OutlinedText> on_text;
    std::unique_ptr<OutlinedText> off_text;

public:
    BoolOptionBox(const std::string& name, const std::string& description, const std::string& path,
                  const std::string& true_label, const std::string& false_label);

    void confirm() override;
    void move_left() override;  // set false
    void move_right() override; // set true
    void draw() override;
};

class IntOptionBox : public BaseOptionBox {
    int value;
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
    std::vector<std::pair<int, std::string>> value_list;
    int value_index{0};

    void rebuild_text();

public:
    IntOptionBox(const std::string& name, const std::string& description, const std::string& path,
                 const std::map<std::string, std::string>& values);

    void confirm() override;
    bool repeats_on_hold() const override { return value_list.empty(); }
    void update(double current_time) override;
    void move_left() override;
    void move_right() override;
    void draw() override;
};

class StrOptionBox : public BaseOptionBox {
    std::string value;
    std::string input_string; // used for free-form editing
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
    std::vector<std::pair<std::string, std::string>> value_list;
    int value_index{0};

    void rebuild_text();

public:
    StrOptionBox(const std::string& name, const std::string& description, const std::string& path,
                 const std::map<std::string, std::string>& values);

    void confirm() override;
    bool needs_text_input() const override;
    std::string text_input_value() const override { return input_string; }
    void update(double current_time) override;
    void move_left() override;
    void move_right() override;
    void draw() override;
};

class KeybindOptionBox : public BaseOptionBox {
    std::string path;
    std::vector<int> value;
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
    // Capture starts one frame after highlighting (see KeyBindControllerOptionBox).
    bool capture_armed = false;
    double message_until = 0.0; // "already used" shown in the value box until then

    void rebuild_text();

public:
    KeybindOptionBox(const std::string& name, const std::string& description, const std::string& path);

    void confirm() override;
    void reload() override;
    void update(double current_time) override;
    void draw() override;
};

class KeyBindControllerOptionBox : public BaseOptionBox {
    std::string path;
    std::vector<int> value;
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
    // Capture starts one frame after highlighting, so the press that opened
    // the box isn't immediately bound to it.
    bool capture_armed = false;
    double message_until = 0.0;

    void rebuild_text();

public:
    KeyBindControllerOptionBox(const std::string& name, const std::string& description, const std::string& path);

    void confirm() override;
    void reload() override;
    void update(double current_time) override;
    void draw() override;
};

class FloatOptionBox : public BaseOptionBox {
    float value;
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;

    void rebuild_text();

public:
    FloatOptionBox(const std::string& name, const std::string& description, const std::string& path);

    void confirm() override;
    bool repeats_on_hold() const override { return true; }
    void update(double current_time) override;
    void move_left() override;
    void move_right() override;
    void draw() override;
};

// Z-DON: kat left/right adjusts the offset both ways; Calibrate is its own
// row (CalibrateOptionBox) right after it, so it stays a visible item.
class AudioOffsetOptionBox : public BaseOptionBox {
public:
    AudioOffsetOptionBox(const std::string& name, const std::string& description, const std::string& path);

    void confirm() override;
    bool repeats_on_hold() const override { return true; }
    void move_left() override;
    void move_right() override;
    void draw() override;
    void update(double current_time) override;

private:
    int value;
    std::unique_ptr<OutlinedText> value_text;
    std::unique_ptr<FadeAnimation> flicker_fade;

    void rebuild_text();
};

// Z-DON: a row that opens the audio offset calibration screen on don.
class CalibrateOptionBox : public BaseOptionBox {
public:
    CalibrateOptionBox(const std::string& name, const std::string& description, const std::string& path,
                       Screens calibrate_screen);

    void confirm() override;
    std::optional<Screens> take_screen_change() override;
    void update(double current_time) override;
    void draw() override;

private:
    Screens calibrate_screen;
    bool wants_screen_change = false;
    std::unique_ptr<OutlinedText> button_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
};

// Z-DON: "Reset to defaults" row appended to every box with key/controller
// bind rows. Don opens a No/Yes choice (No first), kat picks, don applies.
class ResetBindingsOptionBox : public BaseOptionBox {
public:
    ResetBindingsOptionBox(const std::string& name, const std::string& description, std::vector<std::string> paths);

    void confirm() override;
    void move_left() override;
    void move_right() override;
    void update(double current_time) override;
    void draw() override;

private:
    std::vector<std::string> paths;
    bool yes = false;
    std::unique_ptr<OutlinedText> no_text;
    std::unique_ptr<OutlinedText> yes_text;
    std::unique_ptr<FadeAnimation> flicker_fade;
};

// Z-DON: the config.cpp default for a key / controller binding path, if it has one.
std::optional<std::vector<int>> default_binding(const std::string& path);

// Z-DON: localized built-in label for the rows above ("en" fallback).
std::string settings_builtin_text(const std::string& key);
