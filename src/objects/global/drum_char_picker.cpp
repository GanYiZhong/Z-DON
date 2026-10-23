#include "drum_char_picker.h"

#include <algorithm>
#include <map>

#include "../../libs/audio.h"
#include "../../libs/ime_ui.h"
#include "../../libs/input.h"
#include "../../libs/text.h"
#include "../../libs/texture.h"

namespace {

    enum Page {
        LATIN = 0,
        HIRAGANA = 1,
        KATAKANA = 2,
        ZHUYIN = 3
    };

    const std::vector<const char*>& page_rows(int page) {
        static const std::vector<const char*> latin = {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "0123456789",
                                                       "?&:/()+_~#"};
        static const std::vector<const char*> kana = {
            "あいうえおかきくけこ", "さしすせそたちつてと", "なにぬねのはひふへほ",
            "まみむめもやゆよわを", "らりるれろんゃゅょっ", "がぎぐげござじずぜぞ",
            "だぢづでどばびぶべぼ", "ぱぴぷぺぽぁぃぅぇぉ", "ー、。・「」！？"};
        static const std::vector<const char*> zhuyin = {"ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎ", "ㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘ",
                                                        "ㄙㄚㄛㄜㄝㄞㄟㄠㄡㄢ", "ㄣㄤㄥㄦㄧㄨㄩ", "ˊˇˋ˙"};
        static const std::vector<const char*> digits = {"0123456789"};
        switch (page) {
        case LATIN:
            return latin;
        case HIRAGANA:
        case KATAKANA:
            return kana;
        case ZHUYIN:
            return zhuyin;
        default:
            return digits;
        }
    }

    int decode(const std::string& s, size_t& i) {
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        int len = p[i] < 0x80 ? 1 : p[i] < 0xE0 ? 2 : p[i] < 0xF0 ? 3 : 4;
        int cp = len == 1 ? p[i] : p[i] & (0xFF >> (len + 1));
        for (int k = 1; k < len && i + k < s.size(); k++) {
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        i += len;
        return cp;
    }

    std::string encode(int cp) {
        int n = 0;
        const char* u = codepoint_to_utf8(cp, &n);
        return {u, (size_t)n};
    }

    size_t codepoint_count(const std::string& s) {
        size_t n = 0;
        for (size_t i = 0; i < s.size();) {
            decode(s, i);
            n++;
        }
        return n;
    }

    // The character a key with this scancode types (US layout), 0 if none.
    int scancode_char(int sc) {
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
            return 'a' + (sc - SDL_SCANCODE_A);
        }
        if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) {
            return '1' + (sc - SDL_SCANCODE_1);
        }
        if (sc == SDL_SCANCODE_0) {
            return '0';
        }
        if (sc == SDL_SCANCODE_SPACE) {
            return ' ';
        }
        return 0;
    }

    std::string hint_text() {
        static const std::map<std::string, std::string> t = {{"en", "Kat: move    Don: choose"},
                                                             {"ja", "カッ：移動　　ドン：決定"},
                                                             {"zh", "咔：移动　　咚：选择"},
                                                             {"ko", "딱: 이동    쿵: 선택"}};
        auto it = t.find(global_data.config->general.language);
        return it != t.end() ? it->second : t.at("en");
    }

} // namespace

DrumCharPicker::DrumCharPicker(CharSet set, size_t max_codepoints) : set(set), max_codepoints(max_codepoints) {
    build_rows();
}

void DrumCharPicker::build_rows() {
    rows.clear();
    std::vector<Cell> control = {{Kind::UP, ""}, {Kind::OK, "OK"}, {Kind::DEL, "DEL"}};
    if (set == CharSet::FULL) {
        control.push_back({Kind::SPACE, "SPACE"});
    }
    control.push_back({Kind::CANCEL, "CANCEL"});
    if (set == CharSet::FULL) {
        control.push_back({Kind::PAGE, "abc", LATIN});
        control.push_back({Kind::PAGE, "あ", HIRAGANA});
        control.push_back({Kind::PAGE, "ア", KATAKANA});
        control.push_back({Kind::PAGE, "ㄅ", ZHUYIN});
        if (page == LATIN) {
            control.push_back({Kind::SHIFT, "Aa"});
        }
    }
    control.push_back({Kind::DOWN, ""});
    rows.push_back(std::move(control));

    for (const char* r : page_rows(set == CharSet::DIGITS ? -1 : page)) {
        std::vector<Cell> cells = {{Kind::UP, ""}};
        std::string s = r;
        for (size_t i = 0; i < s.size();) {
            int cp = decode(s, i);
            if (page == KATAKANA && cp >= 0x3041 && cp <= 0x3096) {
                cp += 0x60; // hiragana -> katakana
            }
            cells.push_back({Kind::CHAR, encode(cp)});
        }
        cells.push_back({Kind::DOWN, ""});
        rows.push_back(std::move(cells));
    }
    row = std::clamp(row, 0, (int)rows.size() - 1);
    col = std::clamp(col, 0, (int)rows[row].size() - 1);
}

std::string DrumCharPicker::cell_label(const Cell& c) const {
    switch (c.kind) {
    case Kind::UP:
        return "▲";
    case Kind::DOWN:
        return "▼";
    case Kind::CHAR:
        if (shift && c.text.size() == 1 && c.text[0] >= 'a' && c.text[0] <= 'z') {
            return std::string(1, (char)(c.text[0] - 'a' + 'A'));
        }
        return c.text;
    default:
        return c.text;
    }
}

bool DrumCharPicker::is_drum_char(int codepoint, PlayerNum player) {
    const auto& c = *global_data.config;
    auto hits = [codepoint](const DrumBindings& b) {
        for (const auto* keys : {&b.left_kat, &b.left_don, &b.right_don, &b.right_kat}) {
            for (int sc : *keys) {
                int ch = scancode_char(sc);
                if (ch != 0 && (codepoint == ch || (ch >= 'a' && ch <= 'z' && codepoint == ch - 'a' + 'A'))) {
                    return true;
                }
            }
        }
        return false;
    };
    return (player != PlayerNum::P2 && hits(c.keys_1p)) || (player != PlayerNum::P1 && hits(c.keys_2p));
}

DrumCharPicker::Result DrumCharPicker::handle_input(PlayerNum player, std::string& text) {
    // Read every drum input so none of them is left for the screen behind.
    const bool l_kat = is_l_kat_pressed(player);
    const bool r_kat = is_r_kat_pressed(player);
    const bool l_don = is_l_don_pressed(player);
    const bool r_don = is_r_don_pressed(player);

    const int n = (int)rows[row].size();
    if (l_kat != r_kat) {
        audio.play_sound("kat", VolumePreset::SOUND);
        col = (col + (l_kat ? n - 1 : 1)) % n;
    }
    if (!l_don && !r_don) {
        return Result::NONE;
    }
    audio.play_sound("don", VolumePreset::SOUND);
    const Cell cell = rows[row][col];
    const int r_count = (int)rows.size();
    switch (cell.kind) {
    case Kind::CHAR:
        if (max_codepoints == 0 || codepoint_count(text) < max_codepoints) {
            text += cell_label(cell);
        }
        break;
    case Kind::SPACE:
        if (max_codepoints == 0 || codepoint_count(text) < max_codepoints) {
            text += ' ';
        }
        break;
    case Kind::DEL:
        while (!text.empty() && ((unsigned char)text.back() & 0xC0) == 0x80) {
            text.pop_back();
        }
        if (!text.empty()) {
            text.pop_back();
        }
        break;
    case Kind::UP:
        row = (row + r_count - 1) % r_count;
        col = 0;
        break;
    case Kind::DOWN:
        row = (row + 1) % r_count;
        col = (int)rows[row].size() - 1;
        break;
    case Kind::PAGE:
        page = cell.page;
        shift = false;
        row = 1;
        col = 1;
        build_rows();
        break;
    case Kind::SHIFT:
        shift = !shift;
        break;
    case Kind::OK:
        return Result::CONFIRM;
    case Kind::CANCEL:
        return Result::CANCEL;
    }
    return Result::NONE;
}

void DrumCharPicker::draw(Rect field) const {
    const float S = tex.screen_scale;
    const auto sw = (float)tex.screen_width;
    const auto sh = (float)tex.screen_height;
    const int fs = std::max(8, (int)(26 * S));
    const float hint_fs = 20 * S;
    const float spacing = 1.0f;
    const float cell_h = 40 * S;
    const float min_w = 40 * S;
    const float gap = 4 * S;
    const float pad = 10 * S;
    const float margin = 8 * S;

    // the cursor's row in the middle of at most 5 rows
    const int r_count = (int)rows.size();
    std::vector<int> shown;
    if (r_count <= 5) {
        for (int i = 0; i < r_count; i++) {
            shown.push_back(i);
        }
    } else {
        for (int d = -2; d <= 2; d++) {
            shown.push_back(((row + d) % r_count + r_count) % r_count);
        }
    }

    const std::string hint = hint_text();
    std::string all = hint;
    for (int r : shown) {
        for (const Cell& c : rows[r]) {
            all += cell_label(c);
        }
    }
    // one get_font per draw: the font aliases the manager's atlas until the next call
    ray::Font font = global_data.skinless ? ray::GetFontDefault() : font_manager.get_font(all, fs);
    auto measure = [&](const std::string& s, float size) { return measure_text_ex(font, s.c_str(), size, spacing).x; };

    std::vector<std::vector<float>> widths;
    float panel_w = measure(hint, hint_fs);
    for (int r : shown) {
        std::vector<float> w;
        float total = 0;
        for (const Cell& c : rows[r]) {
            float cw = std::max(min_w, measure(cell_label(c), (float)fs) + 16 * S);
            w.push_back(cw);
            total += cw + gap;
        }
        panel_w = std::max(panel_w, total - gap);
        widths.push_back(std::move(w));
    }
    panel_w += 2 * pad;
    const float panel_h = pad + (float)shown.size() * (cell_h + gap) + hint_fs + pad;

    float x = std::clamp(field.x + field.width / 2 - panel_w / 2, margin, std::max(margin, sw - panel_w - margin));
    float y = field.y + field.height + margin;
    if (y + panel_h > sh - margin) {
        y = std::max(margin, field.y - margin - panel_h);
    }

    const Color border{119, 26, 45, 255}; // the default search box's outline
    const Rect panel{x, y, panel_w, panel_h};
    const float round = std::min(24 * S / std::min(panel_w, panel_h), 1.0f);
    draw_rectangle_rounded(panel, round, 16, Color{255, 255, 255, 245});
    draw_rectangle_rounded_lines_ex(panel, round, 16, 3 * S, border);

    float cy = y + pad;
    for (size_t i = 0; i < shown.size(); i++) {
        const int r = shown[i];
        const bool cur_row = r == row;
        float row_w = 0;
        for (float w : widths[i]) {
            row_w += w + gap;
        }
        row_w -= gap;
        float cx = x + panel_w / 2 - row_w / 2;
        if (cur_row) {
            draw_rectangle_rounded(Rect{x + pad / 2, cy - gap / 2, panel_w - pad, cell_h + gap}, 0.3f, 8,
                                   Color{255, 236, 200, 255});
        }
        for (size_t k = 0; k < rows[r].size(); k++) {
            const Cell& c = rows[r][k];
            const Rect cell{cx, cy, widths[i][k], cell_h};
            const bool sel = cur_row && (int)k == col;
            const bool on = (c.kind == Kind::PAGE && c.page == page) || (c.kind == Kind::SHIFT && shift);
            Color bg = c.kind == Kind::CHAR ? Color{235, 235, 235, 255} : Color{205, 222, 245, 255};
            if (on) {
                bg = Color{255, 196, 110, 255};
            }
            draw_rectangle_rounded(cell, 0.3f, 8, sel ? border : bg);
            if (!cur_row) {
                draw_rectangle_rounded(cell, 0.3f, 8, Color{255, 255, 255, 110}); // dim the other rows
            }
            const std::string label = cell_label(c);
            const float tw = measure(label, (float)fs);
            draw_text_ex(font, label.c_str(), {cx + (widths[i][k] - tw) / 2, cy + (cell_h - fs) / 2}, (float)fs,
                         spacing, sel ? Color::WHITE : Color::BLACK);
            cx += widths[i][k] + gap;
        }
        cy += cell_h + gap;
    }
    const float hw = measure(hint, hint_fs);
    draw_text_ex(font, hint.c_str(), {x + panel_w / 2 - hw / 2, cy + 2 * S}, hint_fs, spacing, Color{90, 90, 90, 255});

    // the IME panel goes clear of the field and the picker
    const float ux = std::min(field.x, x);
    const float uy = std::min(field.y, y);
    ime_ui_set_field(Rect{ux, uy, std::max(field.x + field.width, x + panel_w) - ux,
                          std::max(field.y + field.height, y + panel_h) - uy});
}
