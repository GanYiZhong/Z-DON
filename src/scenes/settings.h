#pragma once

#include "../libs/screen.h"
#include "../objects/global/allnet_indicator.h"
#include "../objects/global/coin_overlay.h"
#include "../objects/global/indicator.h"
#include "../objects/settings/settings_box_manager.h"

class SettingsScreen : public Screen {
private:
    std::unique_ptr<SettingsBoxManager> box_manager;
    std::optional<Indicator> indicator;
    std::optional<CoinOverlay> coin_overlay;
    std::optional<AllNetIcon> allnet_indicator;
    std::string username_on_entry;

    TextureObject* t_background = nullptr;
    TextureObject* t_footer = nullptr;

    // Z-DON: input after a key/controller capture is dropped until then, so the
    // captured press does not also move the cursor or re-open the capture.
    double input_guard_until = 0.0;
    // Z-DON: hold-repeat of kat left/right on numeric values (-1 / +1, 0 = none).
    int hold_dir = 0;
    double hold_start = 0.0;
    double next_repeat = 0.0;

    std::optional<Screens> handle_input();

public:
    SettingsScreen() : Screen("settings") {}

    void on_screen_start() override;
    Screens on_screen_end(Screens next_screen) override;
    std::optional<Screens> update() override;
    void draw() override;
};
