#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../libs/gfx/text.h"
#include "option_box.h"

class SettingsBox {
private:
    std::unique_ptr<OutlinedText> label;
    float x;
    float y;
    float start_position;
    float wrap_bottom;
    float target_position;
    int direction{1};
    BaseAnimation* move_anim{nullptr};
    FadeAnimation* blue_arrow_fade{nullptr};
    MoveAnimation* blue_arrow_move{nullptr};
    bool is_selected{false};
    int option_index{0};
    std::vector<std::unique_ptr<BaseOptionBox>> options;

    TextureObject* t_box = nullptr;
    TextureObject* t_box_highlight = nullptr;
    TextureObject* t_blue_arrow = nullptr;

    void draw_text() const;

    static std::unique_ptr<BaseOptionBox> make_option_box(const rapidjson::Value& opt);

public:
    std::string box_name;
    bool in_box{false};

    SettingsBox(std::string name, const std::string& label_text, const rapidjson::Value& options_json);
    ~SettingsBox();

    void set_y(float new_y);
    float center_y() const { return t_box ? y + t_box->y[0] + t_box->y2[0] / 2.0f : y; }
    void set_box_count(int box_count);

    bool can_move() const;

    // Returns false while the carousel is still animating from a previous move
    bool move_left();
    // Returns false while the carousel is still animating from a previous move
    bool move_right();

    // Returns false when the box should become un-selected (exit to outer navigation)
    bool move_option_left();
    void move_option_right();

    void select_option();
    bool editing_text() const;
    // Z-DON: true once after a key/controller capture ended (consumes the flag).
    bool take_capture_done();
    // Z-DON: the highlighted option is a number that a held kat repeats.
    bool adjusting_number() const;
    bool editing() const;

    void select();

    // Returns non-null if an option requested a screen transition. Consumes
    // (clears) the pending transition, so it is not a pure query.
    std::optional<Screens> pending_screen_change();

    void update(double current_time_ms, bool selected);
    void draw();
    void draw_overlay();
    // Z-DON (zdon_mouse.h): in the list, the box is a click target that many steps from the selected box (found from
    // the y of the selected box and the list's pitch); open, its option's blue arrows are kat left / right
    void add_click_areas(float selected_y, float pitch, int id) const;
    float list_y() const { return y; }
};
