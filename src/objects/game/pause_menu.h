#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>

#include "../../libs/gfx/text.h"

// Z-DON: the pause menu of a normal song (GameScreen, Game2PScreen). Escape (back_key) or Space
// (pause_key) opens it; the screen freezes the song clock, the audio, the video and the notes while it
// is up. The rims move between Resume / Restart / Back to song select, a centre hit confirms, Escape or
// Space again resumes. Resume counts down 3-2-1 (kCountdownMs) before the song goes on; whatever was
// pressed while paused or counting down is dropped then (clear_input_buffers), so a pause never judges
// a note. [general] pause_menu = false keeps the old behaviour (Escape leaves the song at once).
//
// The look reuses the practice menu's skin sprites when the skin has them (practice/menu_panel,
// practice/menu_button, practice/menu_button_selected) and draws a plain panel
// otherwise; the texts come from the skin's text_config when it has pause_menu_* (or the practice
// menu's) keys, else from a small built-in table. No skin file is needed or changed.
class PauseMenu {
public:
    enum class Action {
        NONE,
        RESUME, // the countdown has finished: play on
        RESTART,
        BACK,
    };

    static constexpr double kCountdownMs = 1800.0; // 3, 2, 1: 600 ms each

    bool active() const { return open || counting(); }
    bool menu_open() const { return open; }
    bool counting() const { return countdown_start.has_value(); }
    // wall time the menu (and countdown) has held the song so far
    double paused_ms(double now_ms) const { return pause_start ? now_ms - *pause_start : 0.0; }

    void open_menu(double now_ms);
    // one frame while active(): reads the drums / keys, runs the countdown
    Action update(double now_ms);
    void draw(double now_ms);
    void reset();

private:
    enum Row {
        ROW_RESUME,
        ROW_RESTART,
        ROW_BACK,
        ROW_COUNT
    };

    bool open = false;
    int index = ROW_RESUME;
    std::optional<double> pause_start;
    std::optional<double> countdown_start;
    std::string lang;
    std::unique_ptr<OutlinedText> title;
    std::array<std::unique_ptr<OutlinedText>, ROW_COUNT> rows;
    std::array<std::unique_ptr<OutlinedText>, 3> digits;

    void make_texts();
};
