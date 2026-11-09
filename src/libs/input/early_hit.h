#pragma once

#include <optional>

#include "../audio/audio.h"
#include "../global_data.h"

// Z-DON: the hit sound and press time of a drum hit, taken on the input thread (general.zdon_early_hitsound,
// general.zdon_press_time_judge). Without this a hit waits for the next frame's Player::handle_input, up to a whole
// frame after the key went down. A lane calls early_hit_tick every frame while a person plays it; the input
// thread only acts for lanes that ticked recently, so pausing or leaving the song stops it with no cleanup.
// Keyboard and gamepad presses only: touch, MIDI and injected keys take the normal path.

enum EarlyHitPad : int {
    EARLY_HIT_L_DON,
    EARLY_HIT_R_DON,
    EARLY_HIT_L_KAT,
    EARLY_HIT_R_KAT,
    EARLY_HIT_PAD_COUNT
};

struct EarlyHitTick {
    int owner = 0; // the lane: static_cast<int>(player_num)
    PlayerNum input_player;
    PlayerNum sound_player;
    SoundId don = kNoSound;
    SoundId kat = kNoSound;
    bool play_sound = false;
};

void early_hit_tick(const EarlyHitTick& tick);

// The next unclaimed press of this pad: its get_current_ms() time. Empty when the input thread did not see it.
std::optional<double> early_hit_take(int owner, int pad);
