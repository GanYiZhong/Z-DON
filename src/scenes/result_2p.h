#pragma once

#include "result.h"

class Result2PScreen : public ResultScreen {
private:
    std::optional<ResultPlayer> player_2;
    bool zdon_bgm_pending = false; // the AI result BGM waits for skin_config zdon_ai_bgm_after_announce
    double zdon_bgm_at_ms = 0.0;
    int zdon_total_cue = 0; // the play-all page: 1 = announce played, 2 = window sound played

protected:
    double reveal_end_ms() override;
    bool zdon_two_boards() const override { return true; }

public:
    bool two_player_audio() const override { return true; }

    Result2PScreen() = default;

    void on_screen_start() override;
    std::optional<Screens> update() override;
    void draw() override;
};
