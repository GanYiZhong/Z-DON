// Z-DON song search panel: while the search box is open, the songs that match the typed text, a genre, a
// course and a level, eight at a time, under the box. Up / Down pick one and Enter goes straight to it; with
// nothing picked, Enter keeps the old behaviour (the search folder). Tab: genre, F3: course, PgUp / PgDn: level.
// The mouse: a row under it is picked, a click goes to it, the wheel moves the pick, a filter chip clicked
// (right click: back) steps it.
#include "../libs/file/scores.h"
#include "../libs/input/input.h"
#include "../objects/global/zdon_hud.h"
#include "song_select.h"

namespace {
    constexpr size_t kShown = 8;

    bool mouse_in(const Rect& r, Vec2 m) {
        if (r.width <= 0 || r.height <= 0) {
            return false;
        }
        const float x0 = (float)virtual_to_screen_x(r.x), y0 = (float)virtual_to_screen_y(r.y);
        const float x1 = (float)virtual_to_screen_x(r.x + r.width), y1 = (float)virtual_to_screen_y(r.y + r.height);
        return m.x >= x0 && m.x < x1 && m.y >= y0 && m.y < y1;
    }

    // the song's title in the player's language (else English, else the first)
    std::string display_title(const fs::path& path) {
        auto u8 = path.u8string();
        auto meta = scores_manager.get_song_meta(std::string(u8.begin(), u8.end()));
        if (!meta) {
            return path.stem().string();
        }
        const std::string& lang = global_data.config->general.language;
        std::string en;
        for (const auto& [l, t] : meta->titles) {
            if (l == lang && !t.empty()) {
                return t;
            }
            if (l == "en") {
                en = t;
            }
        }
        if (!en.empty()) {
            return en;
        }
        return meta->title.empty() ? path.stem().string() : meta->title;
    }
} // namespace

void SongSelectScreen::zdon_search_refresh() {
    const std::string q = player ? player->search_string : std::string();
    const std::string key = q + '\x1f' + std::to_string(zdon_search.genre) + '\x1f' +
                            std::to_string(zdon_search.course) + '\x1f' + std::to_string(zdon_search.level);
    if (key == zdon_search.key) {
        return;
    }
    zdon_search.key = key;
    zdon_search.sel = -1;
    zdon_search.hits.clear();
    zdon_search.total = 0;
    const bool filtered = zdon_search.genre >= 0 || zdon_search.course >= 0 || zdon_search.level > 0;
    zdon_search.asked = q.find_first_not_of(" \t") != std::string::npos || filtered;
    if (!zdon_search.asked) {
        zdon_search.lines.clear();
        return; // nothing asked yet
    }
    const std::string genre = zdon_search.genre >= 0 && zdon_search.genre < (int)zdon_search.genres.size()
                                  ? zdon_search.genres[zdon_search.genre]
                                  : std::string();
    zdon_search.hits =
        navigator.zdon_search(q, genre, zdon_search.course, zdon_search.level, kShown, zdon_search.total);
    zdon_search.lines.clear();
    for (const fs::path& p : zdon_search.hits) {
        const auto lv = navigator.zdon_levels_of(p);
        int c = zdon_search.course;
        if (c < 0) { // the hardest course the song has
            for (int k = 4; k >= 0 && c < 0; --k) {
                if (lv[k] >= 0 && (zdon_search.level <= 0 || lv[k] == zdon_search.level)) {
                    c = k;
                }
            }
        }
        std::string right = c >= 0 ? zhud::course_name(c) + " ★" + std::to_string(lv[c]) : std::string();
        const std::string g = navigator.zdon_genre_of(p).second;
        zdon_search.lines.push_back({display_title(p), g.empty() ? right : g + "　" + right});
    }
}

// 0 genre, 1 course, 2 level; dir +1 / -1
void SongSelectScreen::zdon_search_step_filter(int which, int dir) {
    if (which == 0) {
        if (zdon_search.genres.empty()) {
            zdon_search.genres = navigator.zdon_genres();
        }
        const int n = (int)zdon_search.genres.size() + 1; // -1 = all
        zdon_search.genre = ((zdon_search.genre + 1 + dir) % n + n) % n - 1;
    } else if (which == 1) {
        zdon_search.course = ((zdon_search.course + 1 + dir) % 6 + 6) % 6 - 1;
    } else {
        zdon_search.level = ((zdon_search.level + dir) % 11 + 11) % 11;
    }
}

// true: the keys of this frame are used up
bool SongSelectScreen::zdon_search_input() {
    if (!search_box || !player || player->search_guard) {
        return false;
    }
    bool changed = false;
    if (key_pressed(SDL_SCANCODE_TAB)) {
        zdon_search_step_filter(0, +1);
        changed = true;
    }
    if (key_pressed(SDL_SCANCODE_F3)) {
        zdon_search_step_filter(1, +1);
        changed = true;
    }
    if (key_pressed(SDL_SCANCODE_PAGEUP)) {
        zdon_search_step_filter(2, +1);
        changed = true;
    }
    if (key_pressed(SDL_SCANCODE_PAGEDOWN)) {
        zdon_search_step_filter(2, -1);
        changed = true;
    }
    const Vec2 m = mouse_position();
    const bool click_l = mouse_pressed(SDL_BUTTON_LEFT);
    const bool click_r = mouse_pressed(SDL_BUTTON_RIGHT);
    for (int c = 0; c < 3 && (click_l || click_r); ++c) {
        if (mouse_in(zdon_search.chips[c], m)) {
            zdon_search_step_filter(c, click_r ? -1 : +1);
            changed = true;
        }
    }
    if (changed) {
        audio.play_sound("kat", VolumePreset::SOUND);
        zdon_search_refresh();
        return true;
    }
    zdon_search_refresh();
    const int n = (int)zdon_search.hits.size();
    // the mouse: hovering picks (only when it moved, so the keys keep their pick), the wheel steps
    int clicked_row = -1;
    for (int i = 0; i < n && i < (int)zdon_search.row_rects.size(); ++i) {
        if (mouse_in(zdon_search.row_rects[i], m)) {
            if (m.x != zdon_search.last_mouse.x || m.y != zdon_search.last_mouse.y) {
                zdon_search.sel = i;
            }
            if (click_l) {
                clicked_row = i;
            }
        }
    }
    zdon_search.last_mouse = m;
    if (const float wheel = mouse_wheel(); wheel != 0 && n > 0) {
        zdon_search.sel = std::clamp(zdon_search.sel + (wheel < 0 ? 1 : -1), -1, n - 1);
        audio.play_sound("kat", VolumePreset::SOUND);
        return true;
    }
    if (key_pressed(SDL_SCANCODE_DOWN) && n > 0) {
        zdon_search.sel = std::min(zdon_search.sel + 1, n - 1);
        audio.play_sound("kat", VolumePreset::SOUND);
        return true;
    }
    if (key_pressed(SDL_SCANCODE_UP) && zdon_search.sel >= 0) {
        zdon_search.sel--;
        audio.play_sound("kat", VolumePreset::SOUND);
        return true;
    }
    const bool blank = player->search_string.find_first_not_of(" \t") == std::string::npos;
    const int pick = zdon_search.sel >= 0 ? zdon_search.sel : (blank && n > 0 ? 0 : -1);
    const bool enter = (key_pressed(SDL_SCANCODE_RETURN) || key_pressed(SDL_SCANCODE_KP_ENTER)) && pick >= 0;
    if (enter || clicked_row >= 0) {
        const fs::path target = zdon_search.hits[clicked_row >= 0 ? clicked_row : pick];
        audio.play_sound("don", VolumePreset::SOUND);
        search_box.reset();
        player->search_string.clear();
        set_keyboard_visible(false);
        clear_input_buffers();
        state = SongSelectState::BROWSING;
        zdon_search = {};
        spdlog::info("Song search: jump to {}", target.string());
        navigator.jump_to_song_path(target);
        return true;
    }
    return false;
}

void SongSelectScreen::zdon_draw_search() {
    if (!search_box) {
        return;
    }
    zdon_search_refresh();
    const float s = tex.screen_scale;
    const float w = 1000 * s;
    const float x = (tex.screen_width - w) / 2.0f;
    const float row_h = 40 * s;
    const float top = tex.screen_height * 0.5f;
    const std::string chip_text[3] = {
        zhud::text("search_genre", zdon_search.genre >= 0 && zdon_search.genre < (int)zdon_search.genres.size()
                                       ? zdon_search.genres[zdon_search.genre]
                                       : zhud::text("search_all")),
        zhud::text("search_course",
                   zdon_search.course >= 0 ? zhud::course_name(zdon_search.course) : zhud::text("search_all")),
        zdon_search.level > 0 ? zhud::text("search_level", std::to_string(zdon_search.level))
                              : "★" + zhud::text("search_all")};
    OutlinedText* chip_label[3];
    for (int c = 0; c < 3; ++c) {
        chip_label[c] = &zhud::label(chip_text[c], 22, Color{255, 226, 120, 255});
    }
    OutlinedText& head = *chip_label[0];
    OutlinedText& keys = zhud::label(zhud::text("search_keys"), 18, Color{200, 206, 220, 255});
    const size_t rows = std::max<size_t>(zdon_search.lines.size(), 1);
    const float h = 16 * s + head.height + 8 * s + rows * row_h + 8 * s + keys.height + 12 * s;
    zhud::panel(x, top, w, h, 0.86f);
    float y = top + 12 * s;
    float cx = x + 20 * s;
    const Vec2 m = mouse_position();
    for (int c = 0; c < 3; ++c) {
        OutlinedText& t = *chip_label[c];
        const Rect r{cx - 8 * s, y - 3 * s, t.width + 16 * s, t.height + 6 * s};
        zdon_search.chips[c] = r;
        draw_rectangle_rounded(r, 0.4f, 6, mouse_in(r, m) ? Color{255, 226, 120, 70} : Color{255, 255, 255, 26});
        t.draw({.x = cx, .y = y});
        cx += t.width + 28 * s;
    }
    if (zdon_search.asked) {
        OutlinedText& count =
            zhud::label(zhud::text("search_count", std::to_string(zdon_search.total)), 22, Color{200, 206, 220, 255});
        count.draw({.x = cx, .y = y});
    }
    y += head.height + 8 * s;
    zdon_search.row_rects.clear();
    if (zdon_search.lines.empty()) {
        if (zdon_search.asked) {
            OutlinedText& none = zhud::label(zhud::text("search_none"), 24, Color{200, 206, 220, 255});
            none.draw({.x = x + 24 * s, .y = y + (row_h - none.height) / 2});
        }
        y += row_h;
    }
    for (size_t i = 0; i < zdon_search.lines.size(); ++i) {
        const bool sel = (int)i == zdon_search.sel;
        zdon_search.row_rects.push_back(Rect{x + 12 * s, y, w - 24 * s, row_h});
        if (sel) {
            draw_rectangle_rounded(Rect{x + 12 * s, y + 2 * s, w - 24 * s, row_h - 4 * s}, 0.3f, 6,
                                   Color{232, 96, 40, 230});
        }
        OutlinedText& t = zhud::label(zdon_search.lines[i].first, 24);
        OutlinedText& r = zhud::label(zdon_search.lines[i].second, 20, Color{210, 214, 226, 255});
        const float max_title = w - r.width - 72 * s;
        const float scale = t.width > max_title ? max_title / t.width : 1.0f;
        t.draw({.scale = scale, .x = x + 24 * s, .y = y + (row_h - t.height * scale) / 2});
        r.draw({.x = x + w - 24 * s - r.width, .y = y + (row_h - r.height) / 2});
        y += row_h;
    }
    y += 8 * s;
    keys.draw({.x = x + (w - keys.width) / 2, .y = y});
}
