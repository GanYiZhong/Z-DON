#include "ime_ui.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "global_data.h"
#include "spdlog/spdlog.h"
#include "text.h"
#include "texture.h"

#if defined(PLATFORM_DESKTOP_SDL)
#include "sdl_compat.h"
#endif

#if defined(PLATFORM_DESKTOP_SDL) && !defined(YATAIDON_USE_SDL2)
#define ZDON_IME_UI 1
#endif

namespace {
    struct ImeState {
        std::string composition;
        int start = 0;  // cursor / selected clause, in code points
        int length = 0; // selected clause length, 0 = plain cursor
        std::vector<std::string> candidates;
        int selected = -1;
        bool horizontal = false;
        int page_size = 0; // largest page seen since the list opened
    };

    std::mutex ime_mutex; // the watch runs where SDL events are pumped / pushed
    ImeState ime;
    bool ime_active = false;
    std::string commit_tail; // what raylib dropped of each committed string (all but its 1st code point)
    bool field_set = false;
    ray::Rectangle field{};
#ifdef ZDON_IME_UI
    SDL_Rect last_area{-1, -1, -1, -1};
#endif

    // Byte offset of code point `cp` in `s` (clamped to its end).
    size_t utf8_offset(const std::string& s, int cp) {
        size_t i = 0;
        for (int n = 0; n < cp && i < s.size(); ++n) {
            ++i;
            while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) {
                ++i;
            }
        }
        return i;
    }

    void clear_locked() {
        ime = ImeState{};
    }
} // namespace

#ifdef ZDON_IME_UI
static bool SDLCALL ime_event_watch(void* /*userdata*/, SDL_Event* event) {
    switch (event->type) {
    case SDL_EVENT_TEXT_EDITING: {
        std::scoped_lock lock(ime_mutex);
        ime.composition = event->edit.text ? event->edit.text : "";
        ime.start = std::max(0, (int)event->edit.start);
        ime.length = std::max(0, (int)event->edit.length);
        break;
    }
    case SDL_EVENT_TEXT_EDITING_CANDIDATES: {
        const auto& c = event->edit_candidates;
        std::scoped_lock lock(ime_mutex);
        ime.candidates.clear();
        for (int i = 0; c.candidates && i < c.num_candidates; ++i) {
            ime.candidates.emplace_back(c.candidates[i] ? c.candidates[i] : "");
        }
        ime.selected = c.selected_candidate;
        ime.horizontal = c.horizontal;
        ime.page_size = ime.candidates.empty() ? 0 : std::max(ime.page_size, (int)ime.candidates.size());
        break;
    }
    case SDL_EVENT_TEXT_INPUT: {
        std::scoped_lock lock(ime_mutex);
        ime.composition.clear(); // committed
        ime.start = ime.length = 0;
        if (ime_active && event->text.text && *event->text.text) {
            commit_tail += event->text.text + utf8_offset(event->text.text, 1);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

static SDL_Window* main_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* win = (windows && count > 0) ? windows[0] : nullptr;
    SDL_free(windows);
    return win;
}
#endif

void ime_ui_set_hint() {
#ifdef ZDON_IME_UI
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition,candidates");
#endif
}

void ime_ui_install() {
#ifdef ZDON_IME_UI
    SDL_AddEventWatch(ime_event_watch, nullptr);
#endif
}

void ime_ui_text_input_changed(bool active) {
    std::scoped_lock lock(ime_mutex);
    ime_active = active;
    clear_locked();
    commit_tail.clear();
#ifdef ZDON_IME_UI
    last_area = {-1, -1, -1, -1};
#endif
}

std::string ime_ui_take_commit_tail() {
    std::scoped_lock lock(ime_mutex);
    return std::exchange(commit_tail, std::string());
}

void ime_ui_set_field(ray::Rectangle rect) {
    field = rect;
    field_set = true;
}

void ime_ui_draw(const ray::Camera2D& camera) {
    const bool had_field = field_set;
    field_set = false;
    if (!ime_active) {
        return;
    }
    const float S = tex.screen_scale;
    const float sw = (float)tex.screen_width;
    const float sh = (float)tex.screen_height;
    const ray::Rectangle r = had_field ? field : ray::Rectangle{sw / 4, sh * 0.3f, sw / 2, 60 * S};

#ifdef ZDON_IME_UI
    // Where any OS IME UI that still shows should go: the field, in window coordinates.
    if (SDL_Window* win = main_window()) {
        ray::Vector2 a = ray::GetWorldToScreen2D({r.x, r.y}, camera);
        ray::Vector2 b = ray::GetWorldToScreen2D({r.x + r.width, r.y + r.height}, camera);
        int ww = 0, wh = 0, pw = 0, ph = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        const float kx = pw > 0 ? (float)ww / pw : 1.0f;
        const float ky = ph > 0 ? (float)wh / ph : 1.0f;
        SDL_Rect area{(int)(a.x * kx), (int)(a.y * ky), (int)((b.x - a.x) * kx), (int)((b.y - a.y) * ky)};
        if (area.x != last_area.x || area.y != last_area.y || area.w != last_area.w || area.h != last_area.h) {
            SDL_SetTextInputArea(win, &area, 0);
            last_area = area;
        }
    }
#else
    (void)camera;
#endif

    ImeState st;
    {
        std::scoped_lock lock(ime_mutex);
        st = ime;
    }
    if (st.composition.empty() && st.candidates.empty()) {
        return;
    }

    // Windows SDL numbers the candidates itself ("1 xx"); elsewhere they come bare.
    std::vector<std::string> labels;
    for (size_t i = 0; i < st.candidates.size(); ++i) {
        const std::string& c = st.candidates[i];
        const bool numbered = c.size() >= 2 && c[0] >= '0' && c[0] <= '9' && c[1] == ' ';
        labels.push_back(numbered ? c : std::to_string((i + 1) % 10) + " " + c);
    }
    // SDL's Windows backend reports the selection over the whole list, the strings for one page.
    int sel = st.selected;
    int page = 0;
    if (sel >= (int)labels.size() && st.page_size > 0) {
        page = sel / st.page_size;
        sel -= page * st.page_size;
    }
    const std::string page_label = page > 0 ? "p." + std::to_string(page + 1) : "";

    const int fs = std::max(8, (int)(30 * S));
    const float spacing = 1.0f;
    std::string all = st.composition + page_label;
    for (const auto& l : labels) {
        all += l;
    }
    ray::Font font = global_data.skinless ? ray::GetFontDefault() : font_manager.get_font(all, fs);
    auto measure = [&](const std::string& s) { return ray::MeasureTextEx(font, s.c_str(), (float)fs, spacing).x; };

    const float pad = 12 * S;
    const float row_h = fs * 1.25f;
    const float gap = 22 * S;
    const float margin = 8 * S;
    const float comp_w = st.composition.empty() ? 0 : measure(st.composition);
    const float comp_h = st.composition.empty() ? 0 : row_h;
    std::vector<float> label_w;
    for (const auto& l : labels) {
        label_w.push_back(measure(l));
    }
    bool horizontal = st.horizontal;
    float cand_h = 0;
    float sep = 0;
    float w = 0;
    float h = 0;
    auto size_panel = [&] {
        float cand_w = 0;
        cand_h = 0;
        if (!labels.empty()) {
            for (float lw : label_w) {
                cand_w = horizontal ? cand_w + lw + gap : std::max(cand_w, lw);
            }
            cand_w -= horizontal ? gap : 0;
            cand_h = horizontal ? row_h : row_h * labels.size();
            if (!page_label.empty()) {
                cand_h += row_h * 0.8f;
            }
        }
        sep = (comp_h > 0 && cand_h > 0) ? 10 * S : 0;
        w = std::max({comp_w, cand_w + 2 * pad, 160 * S}) + 2 * pad;
        h = comp_h + sep + cand_h + 2 * pad;
    };
    size_panel();
    const float below = sh - margin - (r.y + r.height + 10 * S);
    const float above = r.y - 10 * S - margin;
    if (!horizontal && h > below && h > above) {
        horizontal = true; // a vertical list fits on neither side of the field: lay it out in a row
        size_panel();
        if (w > sw - 2 * margin) {
            horizontal = false;
            size_panel();
        }
    }
    float x = r.x + r.width / 2 - w / 2;
    x = std::clamp(x, margin, std::max(margin, sw - w - margin));
    float y = r.y + r.height + 10 * S;
    if (h > below && h <= above) {
        y = r.y - h - 10 * S;
    } else if (h > below) {
        y = std::max(margin, sh - margin - h);
    }

    const ray::Color border{119, 26, 45, 255}; // the search box's outline
    const ray::Rectangle panel{x, y, w, h};
    const float round = std::min(24 * S / std::min(w, h), 1.0f);
    ray::DrawRectangleRounded(panel, round, 16, ray::Color{255, 255, 255, 245});
    ray::DrawRectangleRoundedLinesEx(panel, round, 16, 3 * S, border);

    float cy = y + pad;
    if (comp_h > 0) {
        const float tx = x + pad;
        const float ty = cy + (row_h - fs) / 2;
        ray::DrawTextEx(font, st.composition.c_str(), {tx, ty}, (float)fs, spacing, ray::BLACK);
        const float uy = ty + fs + 2 * S;
        ray::DrawRectangleRec({tx, uy, comp_w, 2 * S}, ray::Color{90, 90, 90, 255});
        const size_t b0 = utf8_offset(st.composition, st.start);
        const float x0 = tx + measure(st.composition.substr(0, b0));
        if (st.length > 0) {
            const size_t b1 = utf8_offset(st.composition, st.start + st.length);
            const float x1 = tx + measure(st.composition.substr(0, b1));
            ray::DrawRectangleRec({x0, uy - 1 * S, x1 - x0, 4 * S}, border);
        } else {
            ray::DrawRectangleRec({x0, ty - 2 * S, 2 * S, fs + 4 * S}, ray::BLACK); // caret
        }
        cy += comp_h + sep;
        if (cand_h > 0) {
            ray::DrawRectangleRec({x + pad, cy - sep / 2 - 1 * S, w - 2 * pad, 1 * S}, ray::Color{200, 200, 200, 255});
        }
    }
    float cx = x + pad + pad;
    for (size_t i = 0; i < labels.size(); ++i) {
        const bool is_sel = (int)i == sel;
        const ray::Rectangle cell = horizontal ? ray::Rectangle{cx - pad / 2, cy, label_w[i] + pad, row_h}
                                               : ray::Rectangle{x + pad, cy, w - 2 * pad, row_h};
        if (is_sel) {
            ray::DrawRectangleRounded(cell, 0.3f, 8, border);
        }
        const float tx = horizontal ? cx : x + 2 * pad;
        ray::DrawTextEx(font, labels[i].c_str(), {tx, cy + (row_h - fs) / 2}, (float)fs, spacing,
                        is_sel ? ray::WHITE : ray::BLACK);
        if (horizontal) {
            cx += label_w[i] + gap;
        } else {
            cy += row_h;
        }
    }
    if (horizontal && !labels.empty()) {
        cy += row_h;
    }
    if (!page_label.empty()) {
        const float pw = measure(page_label);
        ray::DrawTextEx(font, page_label.c_str(), {x + w - pad - pw * 0.7f, cy + 2 * S}, fs * 0.7f, spacing,
                        ray::Color{110, 110, 110, 255});
    }
}

void ime_ui_dev_tick(double since) {
#ifdef ZDON_IME_UI
    static const char* spec = std::getenv("YATAIDON_FAKE_IME");
    if (!spec || !*spec) {
        return;
    }
    static const double t_compose = std::atof(spec);
    static const double t_commit = [] {
        const char* comma = std::strchr(spec, ',');
        return comma ? std::atof(comma + 1) : -1.0;
    }();
    static int stage = 0;
    SDL_Window* win = main_window();
    if (!win) {
        return;
    }
    if (stage == 0 && since >= t_compose) {
        stage = 1;
        static const char* const cands[] = {"1 中", "2 鐘", "3 終", "4 忠", "5 鍾", "6 衷", "7 盅", "8 塚", "9 腫"};
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_EDITING;
        e.edit.windowID = SDL_GetWindowID(win);
        e.edit.text = "太鼓ㄓㄨㄥ";
        e.edit.start = 2;
        e.edit.length = 1;
        SDL_PushEvent(&e);
        SDL_Event c{};
        c.type = SDL_EVENT_TEXT_EDITING_CANDIDATES;
        c.edit_candidates.windowID = SDL_GetWindowID(win);
        c.edit_candidates.candidates = cands;
        c.edit_candidates.num_candidates = 9;
        c.edit_candidates.selected_candidate = 15; // SDL on Windows: index over the whole list (page 2)
        c.edit_candidates.horizontal = false;
        SDL_PushEvent(&c);
        spdlog::info("[fakeime] composition + candidates pushed at {:.0f}", since);
    } else if (stage == 1 && t_commit >= 0 && since >= t_commit) {
        stage = 2;
        SDL_Event c{};
        c.type = SDL_EVENT_TEXT_EDITING_CANDIDATES;
        c.edit_candidates.windowID = SDL_GetWindowID(win);
        c.edit_candidates.selected_candidate = -1;
        SDL_PushEvent(&c);
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_EDITING;
        e.edit.windowID = SDL_GetWindowID(win);
        e.edit.text = "";
        SDL_PushEvent(&e);
        SDL_Event t{};
        t.type = SDL_EVENT_TEXT_INPUT;
        t.text.windowID = SDL_GetWindowID(win);
        t.text.text = "太鼓盅";
        SDL_PushEvent(&t);
        spdlog::info("[fakeime] commit pushed at {:.0f}", since);
    }
#else
    (void)since;
#endif
}
