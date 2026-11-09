#pragma once

#include "game.h"

class Game2PScreen : public GameScreen {
private:
    std::shared_ptr<SongParser> parser_2p;
    bool ai_card_hold = false; // screen option ai_wait_loading_card: the chart clock waits for the loading card
    double ai_black_ms = -1.0; // screen option ai_result_short_black: when the cover went fully black

    void release_chart_resources() override;

    PlayerNum restart_player_num() const override { return PlayerNum::P1; }

protected:
    // merge 0ed42ec: upstream's virtual with Z-DON's rule as the base (a Z-DON AI battle goes back to the AI
    // song select); upstream's AIGameScreen overrides it
    virtual Screens select_screen() const {
        return global_data.zdon_ai.active ? Screens::AI_SELECT : Screens::SONG_SELECT_2P;
    }
    virtual Screens result_screen() const { return Screens::RESULT_2P; }

public:
    bool two_player_audio() const override { return true; }

    Game2PScreen() : GameScreen("game") {}

    PlayerNum scene_player_num() const override { return PlayerNum::TWO_PLAYER; }

    void initialize_chart(const fs::path& song) override;
    std::optional<Screens> update() override;
};
