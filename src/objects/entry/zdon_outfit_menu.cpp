#include "zdon_outfit_menu.h"

#include <algorithm>

#include "../../libs/audio/audio.h"
#include "../../libs/file/scores.h"
#include "../../libs/gfx/texture.h"
#include "../../libs/input/input.h"

int zdon_outfit_count() {
    auto it = tex.skin_config_by_name.find("zdon_outfits");
    if (it == tex.skin_config_by_name.end()) {
        return 0;
    }
    return std::max(0, static_cast<int>(it->second.width));
}

std::string zdon_skin_key() {
    return global_data.config->paths.skin.filename().string();
}

int zdon_player_outfit(PlayerNum pn) {
    const int count = zdon_outfit_count();
    if (count <= 0) {
        return 0;
    }
    const auto& cfg = tex.skin_config_by_name.at("zdon_outfits");
    const int fallback = std::clamp(static_cast<int>(pn == PlayerNum::P2 ? cfg.y : cfg.x), 0, count - 1);
    const std::string id = get_player_id(pn);
    if (id.empty()) {
        return fallback;
    }
    const int outfit = scores_manager.get_outfit(id, zdon_skin_key());
    return (outfit >= 0 && outfit < count) ? outfit : fallback;
}

ZdonOutfitMenu::ZdonOutfitMenu(PlayerNum player_num_) : player_num(player_num_) {
    count = zdon_outfit_count();
    index = zdon_player_outfit(player_num);
    if (!load("OutfitMenu", "outfit_menu", player_num == PlayerNum::P2, count, index)) {
        return;
    }
    fn_update = lua_object["update"];
    fn_draw = lua_object["draw"];
}

void ZdonOutfitMenu::update(double current_ms) {
    call(fn_update, "OutfitMenu:update", current_ms, index);
}

void ZdonOutfitMenu::handle_input() {
    if (count <= 0) {
        confirmed = true;
        return;
    }
    if (menu_left_pressed(player_num)) {
        index = (index - 1 + count) % count;
        audio.play_sound("kat", VolumePreset::SOUND);
    }
    if (menu_right_pressed(player_num)) {
        index = (index + 1) % count;
        audio.play_sound("kat", VolumePreset::SOUND);
    }
    if (menu_confirm_pressed(player_num)) {
        audio.play_sound("don", VolumePreset::SOUND);
        const std::string id = get_player_id(player_num);
        if (!id.empty()) {
            scores_manager.set_outfit(id, zdon_skin_key(), index);
        }
        confirmed = true;
    }
}

void ZdonOutfitMenu::draw() {
    call(fn_draw, "OutfitMenu:draw", index);
}
