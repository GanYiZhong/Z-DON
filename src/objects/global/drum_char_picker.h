#pragma once

#include <string>
#include <vector>

#include "../../libs/global_data.h"
#include "../../libs/math_types.h"

// Z-DON: on-screen character picker for text fields, driven by the drum alone
// (kat left/right moves, don picks). The physical keyboard / IME keep working
// next to it through poll_text_edit; the caller filters the characters that
// keyboard drum keys would type (is_drum_char).
//
// Layout: one page is a short list of rows. Only the cursor's row is
// navigated: kat moves through its cells and wraps inside the row. Every row
// starts with "up" and ends with "down" (wrapping makes them neighbours), so
// a row change is one or two kat away plus a don, and the cursor keeps its
// column. Row 0 holds OK / DEL / SPACE / CANCEL and the page cells (abc,
// hiragana, katakana, zhuyin, Aa = shift). Rows hold at most 10 characters,
// so any cell of a row is at most 6 kat from any other.
class DrumCharPicker {
public:
    enum class CharSet {
        FULL,
        DIGITS
    };
    enum class Result {
        NONE,
        CONFIRM,
        CANCEL
    };

    explicit DrumCharPicker(CharSet set, size_t max_codepoints = 0);

    // Reads this frame's drum presses of `player` and edits `text`.
    Result handle_input(PlayerNum player, std::string& text);

    // Draws the picker under (or above) `field`, in screen space, and points
    // the IME overlay at field + picker so its panel goes clear of both.
    void draw(Rect field) const;

    // true for a code point a keyboard drum key of `player` would type (the
    // drum keys are ordinary letters): such characters drive the picker and
    // must not land in the text.
    static bool is_drum_char(int codepoint, PlayerNum player);

private:
    enum class Kind {
        CHAR,
        UP,
        DOWN,
        OK,
        DEL,
        SPACE,
        CANCEL,
        PAGE,
        SHIFT
    };
    struct Cell {
        Kind kind;
        std::string text; // CHAR: the character; PAGE: the label
        int page = 0;     // PAGE: target page
    };

    CharSet set;
    size_t max_codepoints;
    int page = 0;
    bool shift = false;
    int row = 1;
    int col = 1;
    std::vector<std::vector<Cell>> rows;

    void build_rows();
    std::string cell_label(const Cell& c) const;
};
