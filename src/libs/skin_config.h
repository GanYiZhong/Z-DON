#pragma once

#include <string>
#include <unordered_map>

enum class SC : unsigned int {
    DAN_SELECT_EXAM_INFO,
    DAN_SELECT_SUBTITLE,
    DAN_SELECT_TITLE,
    ENTRY_BOX_SPACING,
    ENTRY_BOX_TEXT,
    GAME_BALLOON_OFFSET,
    GAME_BRANCH_INDICATOR_Y_OFFSET,
    GAME_COMBO_ANNOUNCE_P2_Y_OFFSET,
    GAME_DRUMROLL_COUNTER_MARGIN,
    GAME_DRUMROLL_WIDTH_OFFSET,
    GAME_GAUGE_2P_OFFSET,
    GAME_GAUGE_HIT_EFFECT_NOTE,
    GAME_JUDGE_POS,
    GAME_MOJI,
    GAME_MOJI_DRUMROLL,
    GAME_NOTES,
    GAME_P2_Y,
    GAME_PRACTICE_PROGRESS_BAR_WIDTH,
    GAME_SCORE_COUNTER_2P_Y_OFFSET,
    GAME_SONG_TIMER,
    SONG_SELECT_BOX_BG,
    SONG_SELECT_BOX_BPM,
    SONG_SELECT_BOX_NAME,
    SONG_SELECT_BOX_SUBTITLE,
    SONG_SELECT_GENRE_BG_BOUNDARY,
    TRANSITION_OFFSET,
    TRANSITION_SUBTITLE,
    TRANSITION_TITLE,
    // keys below are optional: absent from tex.get_skin_config() unless the skin defines them
    OPTIONAL_KEYS,
    SCREEN_FADE_MS,
    GAME_DAN_EXAM_INFO,
    SKIP_COUNTER,
    SKIP_USED_TEXT,
    GAME_BALLOON_COUNTER_2P_OFFSET,
    GAME_BALLOON_COUNTER_MARGIN,
    GAME_BALLOON_POP_SHAKE,
    GAME_GOGO_EXPLOSION_TILT,
    GAME_GOGO_EXPLOSION_SLOTS,
    GAME_GOGO_FIRE_ANIM,
    GAME_GOGO_FIRE_FADE,
    ENTRY_DAN,
    LOADING_PROGRESS_BAR,
    LOADING_FADE_MS,
    DAN_LOADING_RANK,
    DAN_SELECT_CHIP_NAME,
    LYRIC,
    MOD_BADGE_GRID,
    SONG_SELECT_ROW_PITCH,
    SONG_SELECT_ROW_CURVE,
    // merge 09db50f: upstream moved these to its song_info.lua; Z-DON keeps the C++ SongInfo
    GAME_SONG_INFO,
    GAME_SONG_INFO_SUBTITLE,
    SONG_NUM_GAME,
    SONG_NUM_MAX_GAME,
    GAME_SONG_INFO_MAX_WIDTH,
    GAME_SONG_INFO_CENTER,
    AI_GAME_SONG_INFO,
    GAME_AI_DRUMROLL_COUNTER_SCALE,
};

namespace std {
    template<>
    struct hash<SC> {
        size_t operator()(SC v) const noexcept { return hash<unsigned int>{}(static_cast<unsigned int>(v)); }
    };
} // namespace std

const std::unordered_map<std::string, SC> skin_config_map{
    {"screen_fade_ms", SC::SCREEN_FADE_MS},
    {"game_ai_drumroll_counter_scale", SC::GAME_AI_DRUMROLL_COUNTER_SCALE},
    {"game_dan_exam_info", SC::GAME_DAN_EXAM_INFO},
    {"skip_counter", SC::SKIP_COUNTER},
    {"skip_used_text", SC::SKIP_USED_TEXT},
    {"game_balloon_counter_2p_offset", SC::GAME_BALLOON_COUNTER_2P_OFFSET},
    {"game_balloon_counter_margin", SC::GAME_BALLOON_COUNTER_MARGIN},
    {"game_balloon_pop_shake", SC::GAME_BALLOON_POP_SHAKE},
    {"game_gogo_explosion_tilt", SC::GAME_GOGO_EXPLOSION_TILT},
    {"game_gogo_explosion_slots", SC::GAME_GOGO_EXPLOSION_SLOTS},
    {"game_gogo_fire_anim", SC::GAME_GOGO_FIRE_ANIM},
    {"game_gogo_fire_fade", SC::GAME_GOGO_FIRE_FADE},
    {"entry_dan", SC::ENTRY_DAN},
    {"loading_progress_bar", SC::LOADING_PROGRESS_BAR},
    {"loading_fade_ms", SC::LOADING_FADE_MS},
    {"dan_loading_rank", SC::DAN_LOADING_RANK},
    {"dan_select_chip_name", SC::DAN_SELECT_CHIP_NAME},
    {"lyric", SC::LYRIC},
    {"mod_badge_grid", SC::MOD_BADGE_GRID},
    {"song_select_row_pitch", SC::SONG_SELECT_ROW_PITCH},
    {"song_select_row_curve", SC::SONG_SELECT_ROW_CURVE},
    {"song_num_game", SC::SONG_NUM_GAME},
    {"song_num_max_game", SC::SONG_NUM_MAX_GAME},
    {"game_song_info_max_width", SC::GAME_SONG_INFO_MAX_WIDTH},
    {"game_song_info_center", SC::GAME_SONG_INFO_CENTER},
    {"game_song_info", SC::GAME_SONG_INFO},
    {"game_song_info_subtitle", SC::GAME_SONG_INFO_SUBTITLE},
    {"ai_game_song_info", SC::AI_GAME_SONG_INFO},
    {"dan_select_exam_info", SC::DAN_SELECT_EXAM_INFO},
    {"dan_select_subtitle", SC::DAN_SELECT_SUBTITLE},
    {"dan_select_title", SC::DAN_SELECT_TITLE},
    {"entry_box_spacing", SC::ENTRY_BOX_SPACING},
    {"entry_box_text", SC::ENTRY_BOX_TEXT},
    {"game_balloon_offset", SC::GAME_BALLOON_OFFSET},
    {"game_branch_indicator_y_offset", SC::GAME_BRANCH_INDICATOR_Y_OFFSET},
    {"game_combo_announce_p2_y_offset", SC::GAME_COMBO_ANNOUNCE_P2_Y_OFFSET},
    {"game_drumroll_counter_margin", SC::GAME_DRUMROLL_COUNTER_MARGIN},
    {"game_drumroll_width_offset", SC::GAME_DRUMROLL_WIDTH_OFFSET},
    {"game_gauge_2p_offset", SC::GAME_GAUGE_2P_OFFSET},
    {"game_gauge_hit_effect_note", SC::GAME_GAUGE_HIT_EFFECT_NOTE},
    {"game_judge_pos", SC::GAME_JUDGE_POS},
    {"game_moji", SC::GAME_MOJI},
    {"game_moji_drumroll", SC::GAME_MOJI_DRUMROLL},
    {"game_notes", SC::GAME_NOTES},
    {"game_p2_y", SC::GAME_P2_Y},
    {"game_practice_progress_bar_width", SC::GAME_PRACTICE_PROGRESS_BAR_WIDTH},
    {"game_score_counter_2p_y_offset", SC::GAME_SCORE_COUNTER_2P_Y_OFFSET},
    {"game_song_timer", SC::GAME_SONG_TIMER},
    {"song_select_box_bg", SC::SONG_SELECT_BOX_BG},
    {"song_select_box_bpm", SC::SONG_SELECT_BOX_BPM},
    {"song_select_box_name", SC::SONG_SELECT_BOX_NAME},
    {"song_select_box_subtitle", SC::SONG_SELECT_BOX_SUBTITLE},
    {"song_select_genre_bg_boundary", SC::SONG_SELECT_GENRE_BG_BOUNDARY},
    {"transition_offset", SC::TRANSITION_OFFSET},
    {"transition_subtitle", SC::TRANSITION_SUBTITLE},
    {"transition_title", SC::TRANSITION_TITLE},
};

enum class SCO : unsigned int {
    HORIZONTAL_SONG_SELECT,
    ONE_MENU_SORT,
    OPTION_SKIP_ROW,
    OPTION_NEIRO_ROW,
    DAN_TITLE_HORIZONTAL,
    DAN_CHIP_NAME_BLACK,
    DISABLE_CHARA_3D,
    CROP_RESULT_SCREENSHOT,
    LOADING_COUNTDOWN,
    DISABLE_RETRY_GAME,
    LANGUAGE_VARIANTS, // Z-DON: C++ fixed-name textures prefer <name>_<lang> when the skin ships one
    HIDDEN_ONI,
    DISABLE_CAMPAIGNS,
    DISABLE_REWARD_SHOP,
    AI_WAIT_LOADING_CARD, // the AI battle's chart waits for the loading card
    AI_NO_SUPPORT_LEVELS, // the AI select plays the chosen chart as it is: no easier support levels

    DISABLE_TOTAL_RESULT,
    DISABLE_WAIWAI,
    DISABLE_AI_COMBO_ANNOUNCE, // Z-DON: no combo plate or voice in an AI battle
    AI_SELECT_CHARA_3D,        // Z-DON: the AI select shows the 3D AI Don
    AI_KEEP_PREVIEW,           // Z-DON: the preview plays on after the course is decided
    AI_START_VOICE,            // Z-DON: deciding the course plays the AI battle's start voice
    AI_LIST_FADE_IN_PLACE,     // Z-DON: on the AI select the song list fades where it is when a song is decided
    AI_RESULT_REVEAL_AT_ONCE,  // Z-DON: the AI result lands score, judges, rank and crown together
    AI_RESULT_SHORT_BLACK,     // Z-DON: the AI result follows the black cover without waiting for its sound
    AI_DON_UNDER_FLAMES,       // Z-DON: the arena's Don is drawn when the skin asks, so the skin's flames lie over it
    AI_GAME_CHARA_3D,          // Z-DON: the arena's AI is the 3D AI Don (tex.ai_draw_rival)
    AI_LOADING_CHARA_3D,       // Z-DON: the loading VS window shows both 3D Dons (tex.ai_vs_chara_draw)
};

namespace std {
    template<>
    struct hash<SCO> {
        size_t operator()(SCO v) const noexcept { return hash<unsigned int>{}(static_cast<unsigned int>(v)); }
    };
} // namespace std

const std::unordered_map<std::string, SCO> screen_options_map{
    {"horizontal_song_select", SCO::HORIZONTAL_SONG_SELECT},
    {"one_menu_sort", SCO::ONE_MENU_SORT},
    {"option_skip_row", SCO::OPTION_SKIP_ROW},
    {"option_neiro_row", SCO::OPTION_NEIRO_ROW},
    {"dan_title_horizontal", SCO::DAN_TITLE_HORIZONTAL},
    {"dan_chip_name_black", SCO::DAN_CHIP_NAME_BLACK},
    {"disable_chara_3d", SCO::DISABLE_CHARA_3D},
    {"crop_result_screenshot", SCO::CROP_RESULT_SCREENSHOT},
    {"loading_countdown", SCO::LOADING_COUNTDOWN},
    {"disable_retry_game", SCO::DISABLE_RETRY_GAME},
    {"language_variants", SCO::LANGUAGE_VARIANTS},
    {"hidden_oni", SCO::HIDDEN_ONI},
    {"disable_campaigns", SCO::DISABLE_CAMPAIGNS},
    {"disable_reward_shop", SCO::DISABLE_REWARD_SHOP},
    {"disable_total_result", SCO::DISABLE_TOTAL_RESULT},
    {"disable_waiwai", SCO::DISABLE_WAIWAI},
    {"disable_ai_combo_announce", SCO::DISABLE_AI_COMBO_ANNOUNCE},
    {"ai_select_chara_3d", SCO::AI_SELECT_CHARA_3D},
    {"ai_keep_preview", SCO::AI_KEEP_PREVIEW},
    {"ai_wait_loading_card", SCO::AI_WAIT_LOADING_CARD},
    {"ai_start_voice", SCO::AI_START_VOICE},
    {"ai_list_fade_in_place", SCO::AI_LIST_FADE_IN_PLACE},
    {"ai_result_reveal_at_once", SCO::AI_RESULT_REVEAL_AT_ONCE},
    {"ai_result_short_black", SCO::AI_RESULT_SHORT_BLACK},
    {"ai_don_under_flames", SCO::AI_DON_UNDER_FLAMES},
    {"ai_game_chara_3d", SCO::AI_GAME_CHARA_3D},
    {"ai_loading_chara_3d", SCO::AI_LOADING_CHARA_3D},
    {"ai_no_support_levels", SCO::AI_NO_SUPPORT_LEVELS},
};
