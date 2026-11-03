#pragma once

#include "../../libs/gfx/texture.h"
#include "../../libs/math/animation.h"

class DrumrollCounter {
private:
    int drumroll_count;
    FadeAnimation* fade = nullptr;
    TextStretchAnimation* stretch = nullptr;
    TextureObject* t_bubble = nullptr;
    TextureObject* t_counter = nullptr;
    // Z-DON: optional skin frames of the bubble opening (the arcade folding fan); null = skin has none
    TextureObject* t_unfold = nullptr;
    // Z-DON: the AI battle's own (teal) fan, used on the AI stage when the skin has bubble_ai / bubble_unfold_ai
    TextureObject* t_bubble_ai = nullptr;
    TextureObject* t_unfold_ai = nullptr;
    double start_ms = -1.0;
    double now_ms = 0.0;

public:
    DrumrollCounter();

    void update_count(int count);

    void update(double current_ms, int count);

    void update_animations(double current_ms);

    // ai: this is the AI player's counter (skin_config zdon_ai_drumroll_ai places it)
    void draw(float y, bool ai = false);

    bool is_finished() const;
};
