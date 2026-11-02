#pragma once
// Z-DON: small engine-drawn UI that every skin shows the same way (practice info strip and bar-number
// input, toasts, the unsaved-score note, the song search results): dark panels and cached OutlinedText.
// Strings are built in (ja / en / zh / ko); a skin's text "zdon_<key>" replaces one.
#include <string>

#include "../../libs/gfx/text.h"

namespace zhud {

    // the string for key in the player's language ({0}, {1}: arguments)
    std::string text(const std::string& key);
    std::string text(const std::string& key, const std::string& a0, const std::string& a1 = "");

    // a cached text (size in 1080p px, scaled to the screen); built on first use
    OutlinedText& label(const std::string& s, int size, Color color = Color::WHITE);

    // a dark panel with a thin border; x, y, w, h in screen px
    void panel(float x, float y, float w, float h, float alpha = 0.82f, Color border = Color{255, 255, 255, 140});

    // a one-line message centred near the top, for 1.6 s
    void toast(const std::string& s);
    void draw_toast();

    // the course name for 0..4 (Easy .. Ura)
    std::string course_name(int course);

} // namespace zhud
