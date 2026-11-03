#include "drumroll_counter.h"

#include <algorithm>
#include <stdexcept>

#include "zdon_ai.h"

static constexpr int DRUMROLL_COUNTER_FADE_ANIM_ID = 8;
static constexpr int DRUMROLL_COUNTER_STRETCH_ANIM_ID = 9;

DrumrollCounter::DrumrollCounter() {
    drumroll_count = 0;
    fade = dynamic_cast<FadeAnimation*>(tex.get_animation(DRUMROLL_COUNTER_FADE_ANIM_ID));
    stretch = dynamic_cast<TextStretchAnimation*>(tex.get_animation(DRUMROLL_COUNTER_STRETCH_ANIM_ID));
    if (!fade || !stretch) {
        throw std::runtime_error("DrumrollCounter: animation 8/9 has an unexpected type");
    }
    t_bubble = tex.get_texture("drumroll_counter/bubble");
    t_counter = tex.get_texture("drumroll_counter/counter");
}

void DrumrollCounter::update_count(int count) {
    count = std::max(count, 0);
    if (drumroll_count != count) {
        drumroll_count = count;
        fade->start();
        stretch->start();
    }
}

void DrumrollCounter::update(double current_ms, int count) {
    update_count(count);
    update_animations(current_ms);
}

void DrumrollCounter::update_animations(double current_ms) {
    fade->update(current_ms);
    stretch->update(current_ms);
}

void DrumrollCounter::draw(float y) {
    // Z-DON: skin_config "zdon_ai_drumroll" {x, y} moves the bubble on the skin's AI stage (Green's AI battle shows
    // it over the arena, next to the clash)
    float x = 0.0f;
    if (zdon_ai_arena_game()) {
        if (auto it = tex.skin_config_by_name.find("zdon_ai_drumroll"); it != tex.skin_config_by_name.end()) {
            x = it->second.x;
            y += it->second.y;
        }
    }
    tex.draw_texture(t_bubble, {.x = x, .y = y, .fade = fade->attribute});
    std::string counter = std::to_string(drumroll_count);
    const float margin = tex.get_skin_config(SC::GAME_DRUMROLL_COUNTER_MARGIN)->x;
    float total_width = static_cast<float>(counter.length()) * margin;
    for (size_t i = 0; i < counter.size(); i++) {
        char digit = counter[i];
        if (digit < '0' || digit > '9') {
            continue;
        }
        tex.draw_texture(t_counter, {.frame = digit - '0',
                                     .x = x - (total_width / 2.0f) + (i * margin),
                                     .y = y - static_cast<float>(stretch->attribute),
                                     .y2 = static_cast<float>(stretch->attribute),
                                     .fade = fade->attribute});
    }
}

bool DrumrollCounter::is_finished() const {
    return fade->is_finished;
}
