#include "zdon_mouse.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "../global_data.h"
#include "../time.h"
#include "input.h"

namespace zdon_mouse {
    namespace {
        struct Area {
            float x, y, w, h;
            int steps;
            int id;
        };
        std::vector<Area> drawing; // registered by this frame's draw
        std::vector<Area> shown;   // what the last frame drew: this frame's clicks land on these

        // a walk to a clicked item: by its id, looked up again each frame (a press the menu drops while it still
        // moves is sent again), or for an area without an id a count of presses
        int walk_id = -1;
        int walk_count = 0;
        double walk_until_ms = 0.0;
        double next_step_ms = 0.0;
        constexpr double kStepMs = 90.0;      // between the presses of a walk
        constexpr double kWalkMaxMs = 4000.0; // a walk that has not arrived by then is given up
        float wheel_rest = 0.0f;              // a touchpad's part-notches, until they make one
        std::optional<int> command;           // clicked this frame (kCommand + n)

        double cursor_moved_ms = -1e18;
        bool cursor_shown = false;
        constexpr double kCursorRestMs = 3000.0;

        // a play (and the screens that read the drum as a drum): the mouse stays out
        bool drum_screen(std::string_view s) {
            return s == "GAME" || s == "GAME_2P" || s == "WAIWAI_GAME" || s == "GAME_DAN" || s == "GAME_PRACTICE" ||
                   s == "AI_GAME" || s == "INPUT_TEST" || s == "INPUT_CALI" || s == "TUTORIAL" || s == "LOADING";
        }

        bool env_on(const char* name) {
            const char* e = std::getenv(name);
            return e && *e == '1';
        }

        void stop_walk() {
            walk_id = -1;
            walk_count = 0;
        }
    } // namespace

    void add_area(float x, float y, float w, float h, int steps, int id) {
        if (w > 0 && h > 0) {
            drawing.push_back({x, y, w, h, steps, id});
        }
    }

    void draw_debug() {
        // ZDON_SHOW_CLICK_AREAS=1: each area tinted where it is (green: confirm, blue: a walk, grey: swallow / back)
        static const bool show = env_on("ZDON_SHOW_CLICK_AREAS");
        if (!show) {
            return;
        }
        for (const Area& a : drawing) {
            const Color c = a.steps >= kCommand   ? Color{220, 120, 0, 70}
                            : a.steps == 0        ? Color{0, 200, 0, 80}
                            : a.steps >= kSwallow ? Color{90, 90, 90, 60}
                                                  : Color{0, 80, 255, 60};
            draw_rectangle(Rect{a.x, a.y, a.w, a.h}, c);
        }
    }

    std::optional<int> take_command() {
        return std::exchange(command, std::nullopt);
    }

    void reset() {
        stop_walk();
        wheel_rest = 0.0f;
        shown.clear();
        drawing.clear();
    }

    void frame(const Camera2D& camera, std::string_view screen) {
        shown.swap(drawing);
        command.reset();
        drawing.clear();
        static std::string last_screen;
        if (screen != last_screen) {
            last_screen = screen;
            stop_walk();
            wheel_rest = 0.0f;
            shown.clear(); // drawn by the screen before
        }
        static const bool off = env_on("ZDON_NO_MOUSE_MENUS");
        if (off) {
            return;
        }
        const double now = get_current_ms();

        // the cursor: shown while the mouse moves, hidden once it rests (a drum player never sees it)
        if (mouse_moved()) {
            cursor_moved_ms = now;
            if (!cursor_shown) {
                show_cursor();
                cursor_shown = true;
            }
        } else if (cursor_shown && now - cursor_moved_ms > kCursorRestMs && screen != "SETTINGS" &&
                   screen != "GAME_PRACTICE") {
            hide_cursor();
            cursor_shown = false;
        }

        if (drum_screen(screen) || global_data.in_transition) {
            stop_walk();
            wheel_rest = 0.0f;
            return;
        }

        const bool from_touch = mouse_click_from_touch();
        const bool left = !from_touch && mouse_pressed(SDL_BUTTON_LEFT);
        const bool right = !from_touch && mouse_pressed(SDL_BUTTON_RIGHT);
        const float wheel = mouse_wheel();
        const Area* hit = nullptr;
        if (left || right || wheel != 0.0f) {
            const Vec2 m = get_screen_to_world_2d(mouse_position(), camera);
            for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
                if (m.x >= it->x && m.x < it->x + it->w && m.y >= it->y && m.y < it->y + it->h) {
                    hit = &*it;
                    break;
                }
            }
        }
        if (hit && hit->steps == kSwallow) {
            // a panel with its own mouse (or a capture): neither button reaches the menu
        } else if (left) {
            stop_walk();
            if (!hit || hit->steps == 0) {
                inject_menu_press(0); // the selected item, or nowhere in particular: confirm, as a don would
            } else if (hit->steps >= kCommand) {
                command = hit->steps - kCommand;
            } else if (hit->steps == kBack) {
                inject_menu_press(2);
            } else if (hit->steps != kSwallow) {
                walk_id = hit->id;
                walk_count = hit->id >= 0 ? 0 : hit->steps;
                walk_until_ms = now + kWalkMaxMs;
                next_step_ms = now;
            }
        } else if (right) {
            stop_walk();
            inject_menu_press(2);
        }

        // the wheel: up = left, down = right, a press a notch
        if (hit && hit->steps == kSwallow) {
            wheel_rest = 0.0f;
        } else {
            wheel_rest += wheel;
        }
        while (std::abs(wheel_rest) >= 1.0f) {
            stop_walk();
            inject_menu_press(wheel_rest > 0 ? -1 : 1);
            wheel_rest += wheel_rest > 0 ? -1.0f : 1.0f;
        }

        if ((walk_id < 0 && walk_count == 0) || now < next_step_ms) {
            return;
        }
        if (now > walk_until_ms) {
            stop_walk();
            return;
        }
        int steps = walk_count;
        if (walk_id >= 0) {
            steps = 0;
            bool found = false;
            for (const Area& a : shown) {
                if (a.id == walk_id && a.steps < kSwallow) {
                    steps = a.steps;
                    found = true;
                    break;
                }
            }
            if (!found || steps == 0) { // gone from the screen, or selected: the walk is over
                stop_walk();
                return;
            }
        } else {
            walk_count += walk_count < 0 ? 1 : -1;
        }
        inject_menu_press(steps < 0 ? -1 : 1);
        next_step_ms = now + kStepMs;
    }

} // namespace zdon_mouse
