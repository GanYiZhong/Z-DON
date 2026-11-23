#include "box_base.h"

#include <string_view>
#include <utility>

#include "../../../libs/color_shader.h"
#include "../../../libs/gfx/text.h"
#include "color_utils.h"

BaseBox::BaseBox(fs::path path, const BoxDef& box_def)
    : path(std::move(path)), texture_index(box_def.texture_index), genre_index(box_def.genre_index),
      genre_label(box_def.genre_label), collection(box_def.collection), collab(box_def.collab),
      explanation(box_def.explanation) {
    BoxColors colors = resolve_box_colors(box_def.box_color, box_def.back_color, box_def.fore_color);
    this->back_color = colors.box;
    this->gradients = GradientTextures(this->back_color);
    this->fore_color = colors.outline;
    this->text_color = colors.text;

    position = std::numeric_limits<float>::infinity();
    target_position = std::numeric_limits<float>::infinity();

    open_anim = std::make_unique<MoveAnimation>(233, 150.0f * tex.screen_scale, false, false, 0, 133);
    open_fade = std::make_unique<FadeAnimation>(200, 0.0f, false, false, 1.0f, 133);
    move = std::make_unique<MoveAnimation>(133, 0, false, false, 0, 0.0, std::nullopt, std::nullopt, EaseType::Cubic);
    move->start();

    yellow_right_width = tex.get_texture("yellow_box/yellow_box_right")->width;
    folder_texture_left_width = tex.get_texture("box/folder_texture_left")->width;
    folder_texture_right_width = tex.get_texture("box/folder_texture_right")->width;

    fade_in(100);
}

BaseBox::~BaseBox() = default;

void BaseBox::preregister_text() {
    font_manager.register_text(text_name, static_cast<int>(box_name_font_size()));
}

void BaseBox::load_text() {
    name = make_unique<OutlinedText>(text_name, box_name_font_size(), text_color, fore_color, true);

    if (!back_color.empty()) {
        shader_owner = load_color_shader(back_color.front());
        shader = *shader_owner;
        shader_loaded = is_shader_valid(shader);
    }

    text_loaded = true;
}

void BaseBox::set_box_state(bool active) {
    yellow_box_active = active;
    yellow_box_opened = false;
    is_diff_select = false;
}

void BaseBox::reset() {
    set_box_state(false);
    open_anim->reset();
    open_fade->reset();
}

void BaseBox::expand_box() {
    set_box_state(true);
    open_anim->start();
    bar_open_started_at = get_current_ms();
}

void BaseBox::close_box() {
    set_box_state(false);
}

void BaseBox::enter_box() {
    if (!yellow_box_active) {
        return;
    }
    is_diff_select = true;
}

void BaseBox::exit_box() {
    set_box_state(true);
    open_fade->start();
}

void BaseBox::set_position(float target_position) {
    position = target_position;
    this->target_position = position;
}

void BaseBox::move_box(float target_position, float duration, float delay, std::optional<EaseType> ease) {
    this->target_position = target_position;
    float delta = target_position - position;
    move_delta = delta;
    move = std::make_unique<MoveAnimation>(duration, delta, false, false, 0, delay, std::nullopt, std::nullopt, ease);
    move->start();
}

void BaseBox::fade_in(float delay, float duration) {
    fade = std::make_unique<FadeAnimation>(duration, 0.0f, false, false, 1.0f, delay);
    fade->start();
}

void BaseBox::fade_out(float duration) {
    fade = std::make_unique<FadeAnimation>(duration);
    fade->start();
}

void BaseBox::update(double current_time) {
    // Z-DON (merge 0ed42ec): kept the per-animation settled() checks instead of upstream's early return; same
    // effect for boxes at rest, and it also skips a delayed-start animation's idle frames
    // every box of the open folder comes here every frame, and almost all of them are at rest:
    // an animation is only updated when that can change it
    if (!open_anim->settled(current_time)) {
        open_anim->update(current_time);
    }
    if (!open_fade->settled(current_time)) {
        open_fade->update(current_time);
    }
    if (!fade->settled(current_time)) {
        fade->update(current_time);
    }
    if (snap_open_pending) {
        move->fast_forward();
    }
    float prev_position = move->attribute;
    if (!move->settled(current_time)) {
        move->update(current_time);
    }
    if (!move->is_finished) {
        position += move->attribute - prev_position;
        if (move_delta != 0.0f && cross_lead != 0.0f) {
            float p = move->attribute / move_delta;
            if (p < 0.0f) {
                p = 0.0f;
            } else if (p > 1.0f) {
                p = 1.0f;
            }
            cross_pos = cross_target + cross_lead * (1.0f - p);
        }
    } else {
        position = target_position;
        cross_pos = cross_target;
        cross_lead = 0.0f;
        if (yellow_box_active && !yellow_box_opened) {
            yellow_box_opened = true;
            open_fade->start();
            if (snap_open_pending) {
                open_fade->fast_forward();
                snap_open_pending = false;
            }
        }
    }
    if (yellow_box_active) {
        // The Lua skin advances the shared yellow-box animations; read their current
        // attributes to derive the box bounds the navigator uses for the inline-list bg.
        auto yb = [](int id) { return dynamic_cast<MoveAnimation*>(tex.get_animation(id)); };
        if (is_diff_select) {
            left_bound = position + (yb(13)->attribute - yb(13)->start_position);
            right_bound = yb(14)->attribute - yb(14)->start_position;
        } else {
            left_bound = position + (yb(9)->attribute - yb(9)->start_position);
            right_bound = yb(10)->attribute + yellow_right_width;
        }
    } else {
        left_bound = position;
        right_bound = position + folder_texture_left_width + folder_texture_right_width +
                      (tex.get_skin_config(SC::SONG_SELECT_BOX_BG)->width);
    }
}
