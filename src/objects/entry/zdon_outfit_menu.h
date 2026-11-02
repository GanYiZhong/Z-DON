#pragma once
// Z-DON 2D outfits: a skin without 3D characters can still offer the entry screen's costume board. It declares
// how many outfits it draws with a skin_config entry  "zdon_outfits": { "width": N, "x": a, "y": b }  (x / y: the
// outfit of a 1P / 2P player who never picked one; what a number means is the skin's business, e.g. which
// character wears what), and draws the picker with its own Lua class OutfitMenu (entry/outfit_menu.lua):
//   OutfitMenu.new(is_2p, count, current)   :update(ms, index)   :draw(index)
// Each player's pick is stored per skin in scores.db (table zdon_outfits) and read back with tex.outfit(pn).
#include <string>

#include "../../libs/file/script.h"
#include "../../libs/global_data.h" // IWYU pragma: keep

int zdon_outfit_count();              // 0: the skin has no 2D outfits
std::string zdon_skin_key();          // the current skin's folder name
int zdon_player_outfit(PlayerNum pn); // the player's outfit in this skin (never picked: the skin's x / y default)

class ZdonOutfitMenu : public LuaScript {
public:
    PlayerNum player_num;
    int count = 0;
    int index = 0;
    bool confirmed = false;

    explicit ZdonOutfitMenu(PlayerNum player_num);
    void update(double current_ms);
    void handle_input();
    void draw();

private:
    sol::protected_function fn_update;
    sol::protected_function fn_draw;
};
