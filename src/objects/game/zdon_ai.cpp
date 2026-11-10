#include "zdon_ai.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <spdlog/spdlog.h>

#include "../../libs/audio/audio.h"
#include "../../libs/file/scores.h"
#include "../../libs/file/script.h"
#include "../../libs/gfx/texture.h"
#include "../../libs/global_data.h"
#include "ai_match.h"
#include "player.h"

namespace {
    // the arcade's ai_tuninginfo (CHN05 datatable), tuning levels 1..11:
    // variance worst / best (ms^2), trace probability and variance coefficient per state (neutral, superior,
    // inferior), the state thresholds n->s, n->i, s->n, i->n, the trace sample size, section points ryo / ka / fuka
    constexpr ZdonAiTuning kTuning[11] = {
        {1444, 361, {0, 0, 0}, {0, -50, 40}, -7, 5, -1, -1, 45, 5, 3, 0},
        {1444, 361, {0, 0, 0}, {0, -50, 40}, -7, 5, -1, -1, 80, 5, 3, 0},
        {1024, 256, {0, 0, 0}, {0, -50, 40}, -7, 5, -1, -1, 120, 5, 3, 0},
        {1024, 256, {0, 0, 0}, {0, -40, 40}, -7, 5, -1, -1, 150, 5, 3, 0},
        {784, 196, {0, 0, 0}, {0, -40, 40}, -7, 5, -1, -1, 170, 5, 3, 0},
        {676, 169, {0, 0, 0}, {0, -40, 40}, -7, 5, -1, -1, 230, 3, 2, 0},
        {576, 144, {0, 0, 0}, {0, -40, 40}, -7, 4, -1, -1, 290, 3, 2, 0},
        {400, 100, {0, 0, 0}, {0, -30, 50}, -7, 4, -1, -1, 350, 3, 2, 0},
        {256, 64, {0, 0, 0}, {0, -30, 50}, -6, 4, 0, 0, 430, 3, 2, 0},
        {196, 49, {0, 0, 0}, {0, -30, 50}, -6, 4, 0, 0, 540, 2, 1, 0},
        {100, 25, {0, 50, 0}, {5, -20, 60}, -6, 6, 0, 0, 540, 3, 1, 0},
    };

    // Green (System 357) ghosttuning.xml tuning_lv1..lv11, the same fields (patterns neutral / superior /
    // inferior; patternnode 0 inferior / superior = n->s / n->i, node 1 inferior = s->n, node 2 superior = i->n),
    // its emotion thresholds emotionofs_threslow / threshigh; tuning_lv0 (variance 1024 / 512, sample 100,
    // 4 / 2 / 0, emotion 2 / 6, 120-note sections) is not a star level
    constexpr ZdonAiTuning kTuningGreen[11] = {
        {1024, 256, {0, 0, 0}, {5, -50, 40}, -7, 5, -1, -1, 45, 5, 3, 0, 5, 6},
        {1024, 256, {0, 0, 0}, {5, -50, 40}, -7, 5, -1, -1, 80, 5, 3, 0, 5, 6},
        {1024, 256, {0, 0, 0}, {5, -50, 40}, -7, 5, -1, -1, 120, 5, 3, 0, 5, 6},
        {1024, 256, {0, 0, 0}, {5, -40, 40}, -7, 5, -1, -1, 150, 5, 3, 0, 5, 6},
        {576, 144, {0, 0, 0}, {5, -40, 40}, -7, 5, -1, -1, 170, 5, 3, 0, 5, 6},
        {576, 144, {0, 0, 0}, {5, -40, 40}, -7, 5, -1, -1, 230, 3, 2, 0, 5, 6},
        {400, 100, {0, 0, 0}, {5, -40, 40}, -7, 5, -1, -1, 290, 3, 2, 0, 5, 6},
        {256, 64, {0, 0, 0}, {5, -30, 50}, -7, 5, -1, -1, 350, 3, 2, 0, 5, 6},
        {144, 36, {0, 0, 0}, {5, -30, 50}, -6, 6, 0, 0, 430, 3, 2, 0, 5, 6},
        {144, 36, {0, 50, 0}, {5, -20, 50}, -6, 6, 0, 0, 900, 3, 1, 0, 5, 6},
        {100, 25, {0, 50, 0}, {5, -20, 60}, -6, 6, 0, 0, 540, 3, 1, 0, 5, 6},
    };

    // which rank bonuses the last finished battle got (the result's notes)
    bool last_first_win = false;
    bool last_bonus_high = false;
    // the result's cut-in pages (zdon_ai_pages_*): open this frame, kat presses so far, ended by a don
    bool pages_open = false;
    int page_steps = 0;
    bool pages_finished = false;
    bool result_revealed = false;
    float title_alpha = 1.0f;

    // the arcade's AI battle win-count rewards (reward.bin, releaseType 42)
    constexpr int kRewardWins[] = {1,   2,   10,  25,  50,  75,  100,  125,  150,  175,  200,  250,  300,
                                   350, 400, 500, 600, 765, 876, 1000, 2000, 3000, 4000, 5000, 10000};

    // the arcade's cues (se_aienso_v12g, voice_aienso_v12g) when the skin has them: Sounds/game/ai/<name>,
    // Sounds/game/aivoice/<name>[_<n>] (a random one of the numbered takes)
    void cue(const std::string& se, const std::string& voice, int takes) {
        spdlog::info("AI cue se='{}' voice='{}' at {:.0f} ms", se, voice, get_current_ms());
        if (!se.empty() && audio.has_sound("ai_" + se)) {
            audio.play_sound("ai_" + se, VolumePreset::SOUND);
        }
        if (voice.empty()) {
            return;
        }
        static std::mt19937 rng{std::random_device{}()};
        const std::string name =
            "aivoice_" + voice +
            (takes > 1 ? "_" + std::to_string(std::uniform_int_distribution<int>(1, takes)(rng)) : "");
        if (audio.has_sound(name)) {
            audio.play_sound(name, VolumePreset::VOICE);
        }
    }

    // skin_config zdon_ai_end_delay (Green): the cabinet's song-end cue (SE + voice) comes after the emblem, ~5.1 s
    // after the last note (footage Tl7g8XXXvAM 4:21-4:26: the emblem at +4.5 s, the voice 0.6 s later)
    constexpr double kEndCueDelayMs = 5100.0;
    bool end_cue_pending = false;
    bool end_cue_won = false;
    double end_cue_at_ms = 0.0;
    // the last section's own cue (SE + voice) comes 1.6 s after the last note, before the emblem's cue (footage
    // Tl7g8XXXvAM: the last note 260.5 s, the section SE 262.1 s, the song cue 265.3 s)
    constexpr double kLastSectionCueDelayMs = 1600.0;
    bool last_cue_pending = false;
    // the match / AI-reach voice follows the section's own voice by 1.5 s (footage 0vjiSUv61i4 396.59 section_lose_2
    // -> 398.08 section_match; n44 UoUjVSZw34U 27.75 section_lose_2 -> 29.24 section_ai_reach)
    constexpr double kMatchVoiceDelayMs = 1500.0;
    bool match_cue_pending = false;
    double match_cue_at_ms = 0.0;
    const char* match_cue_voice = "";
    // Green: the start voice comes 3.3 s before the song, the ready SE 1.65 s before it (footage Tl7g8XXXvAM: voice
    // 136.7 s, SE 138.4 s, song 140.1 s); the game gives the lead (Game2PScreen start_delay 3 s)
    constexpr double kReadySeDelayMs = 1350.0;
    bool ready_se_pending = false;
    // skin_config zdon_ai_ready_voice_ms (Green): the start voice waits for the arena reveal (footage Tl7g8XXXvAM: the
    // voice 136.70 s while the loading wipe ends, the wipe starts 136.3 s; ours starts the wipe 0.4 s after the chart
    // loads)
    bool ready_voice_pending = false;
    double ready_voice_at_ms = 0.0;
    bool ready_start_pending = false; // Nijiiro: the start voice and SE are due, once the loading card is gone
    void ready_voice_cue() {
        const auto rv = tex.skin_config_by_name.find("zdon_ai_ready_voice_ms");
        if (rv != tex.skin_config_by_name.end() && rv->second.x > 0.0f) {
            ready_voice_pending = true;
            ready_voice_at_ms = get_current_ms() + rv->second.x;
        } else {
            cue("", "ready", 3);
        }
    }
    double ready_se_at_ms = 0.0;
    bool last_cue_won = false;
    double last_cue_at_ms = 0.0;

    // skin_config zdon_ai_end_hold x (Nijiiro): ms from the last section's end to the emblem; 0 = off
    double end_hold_ms() {
        const auto it = tex.skin_config_by_name.find("zdon_ai_end_hold");
        if (it == tex.skin_config_by_name.end() || it->second.x <= 0.0f) {
            return 0.0;
        }
        const auto ed = tex.skin_config_by_name.find("zdon_ai_end_delay");
        return (ed != tex.skin_config_by_name.end() && ed->second.x != 0.0f) ? 0.0 : static_cast<double>(it->second.x);
    }
    // Nijiiro: the ready SE follows the start voice by 0.37 s (footage 1Lg6EXE2mo8: voice 1.54 s, SE 1.91 s)
    constexpr double kReadySeLagMs = 370.0;

    int section_points(const Player& p, const ZdonAiTuning& t) {
        return p.get_good() * t.ryo + p.get_ok() * t.ka + p.get_bad() * t.fuka;
    }

    // a section's crown from the player's judges in it (AiEnsoGameManager: no ok and no bad = donderful, no bad =
    // full combo, else a clear above 70% good + ok)
    int section_crown(int good, int ok, int bad) {
        if (bad == 0) {
            return ok == 0 ? 3 : 2;
        }
        const int all = good + ok + bad;
        return static_cast<float>(good + ok) / static_cast<float>(all) > 0.7f ? 1 : 0;
    }
} // namespace

const ZdonAiTuning& zdon_ai_tuning(int level) {
    // skin_config "zdon_ai_tuning" (PyTaikoGreen): Green's ghosttuning instead of the CHN05 table
    // read by value: a child skin that inherits it switches it off with x = 0
    const auto tune = tex.skin_config_by_name.find("zdon_ai_tuning");
    const auto* table = tune != tex.skin_config_by_name.end() && tune->second.x != 0.0f ? kTuningGreen : kTuning;
    return table[std::clamp(level, 1, 11) - 1];
}

bool zdon_ai_end_delay_on() {
    const auto it = tex.skin_config_by_name.find("zdon_ai_end_delay");
    return it != tex.skin_config_by_name.end() && it->second.x != 0.0f;
}

int zdon_ai_tuning_level(double star_level) {
    const int s = static_cast<int>(star_level);
    return s > 10 ? 11 : std::clamp(s, 1, 10);
}

int zdon_ai_condition() {
    if (global_data.zdon_ai_condition < 1 || global_data.zdon_ai_condition > 5) {
        // SceneAISongSelect: a stored condition, else a uniform 1..5 for the session
        static std::mt19937 rng{std::random_device{}()};
        global_data.zdon_ai_condition = std::uniform_int_distribution<int>(1, 5)(rng);
    }
    return global_data.zdon_ai_condition;
}

void zdon_ai_new_session() {
    global_data.zdon_ai_condition = 0;
    global_data.zdon_ai_credit.clear();
}

namespace {
    constexpr ZdonAiRank kRanks[kZdonAiRanks] = {
        {0, {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}},     {1, {1, 1, 1, 1, 1}, {2, 2, 1, 1, 1}},
        {8, {3, 2, 1, 1, 1}, {4, 3, 1, 1, 1}},     {15, {5, 4, 1, 1, 1}, {0, 5, 2, 1, 1}},
        {30, {0, 6, 3, 2, 2}, {0, 7, 4, 3, 3}},    {45, {0, 7, 4, 3, 3}, {0, 0, 5, 4, 4}},
        {60, {0, 0, 6, 5, 5}, {0, 0, 7, 6, 6}},    {75, {0, 0, 8, 7, 7}, {0, 0, 0, 8, 8}},
        {90, {0, 0, 0, 8, 8}, {0, 0, 0, 9, 9}},    {105, {0, 0, 0, 9, 9}, {0, 0, 0, 10, 10}},
        {120, {0, 0, 0, 10, 10}, {0, 0, 0, 0, 0}}, {150, {0, 0, 0, 99, 99}, {0, 0, 0, 0, 0}},
    };
} // namespace

const ZdonAiRank& zdon_ai_rank_info(int rank) {
    return kRanks[std::clamp(rank, 0, kZdonAiRanks - 1)];
}

int zdon_ai_rank_of(int points) {
    int r = 0;
    for (int k = 0; k < kZdonAiRanks; ++k) {
        if (points >= kRanks[k].acquired) {
            r = k;
        }
    }
    return r;
}

void zdon_ai_set_revealed(bool revealed) {
    result_revealed = revealed;
}

bool zdon_ai_revealed() {
    return result_revealed;
}

namespace {
    bool diff_stage_on = false;
    int diff_stage_cursor = 0;
} // namespace

void zdon_ai_set_diff_stage(bool on, int cursor) {
    diff_stage_on = on;
    diff_stage_cursor = cursor;
}

bool zdon_ai_diff_stage(int* cursor) {
    if (cursor) {
        *cursor = diff_stage_cursor;
    }
    return diff_stage_on;
}

void zdon_ai_set_title_alpha(float a) {
    title_alpha = a;
}

float zdon_ai_title_alpha() {
    return title_alpha;
}

bool zdon_ai_pages_open() {
    return pages_open && !pages_finished;
}

void zdon_ai_pages_set(bool open) {
    if (open && !pages_open) {
        page_steps = 0;
        pages_finished = false;
    }
    pages_open = open;
}

int zdon_ai_page_steps() {
    return page_steps;
}

bool zdon_ai_pages_finished() {
    return pages_finished;
}

void zdon_ai_page_next() {
    ++page_steps;
}

void zdon_ai_pages_finish() {
    pages_finished = true;
}

bool zdon_ai_last_first_win() {
    return last_first_win;
}

bool zdon_ai_last_bonus_high() {
    return last_bonus_high;
}

int zdon_ai_next_reward(int wins) {
    for (int need : kRewardWins) {
        if (wins < need) {
            return need - wins;
        }
    }
    return 0;
}

double ZdonAiDriver::draw_offset(double bad_window_ms) {
    const auto& battle = global_data.zdon_ai;
    const ZdonAiTuning& t = zdon_ai_tuning(level);
    const int state = std::clamp(battle.state, 0, 2);
    // the player's own recent timing, at the state's trace probability
    const auto& offs = battle.player_offsets;
    if (t.trace_prob[state] > 0 && !offs.empty() &&
        std::uniform_int_distribution<int>(0, 99)(rng) < t.trace_prob[state]) {
        const double off = offs[std::uniform_int_distribution<size_t>(0, offs.size() - 1)(rng)];
        return std::abs(off) >= bad_window_ms ? std::numeric_limits<double>::quiet_NaN() : off;
    }
    const double c = (std::clamp(battle.condition, 1, 5) - 1) / 4.0;
    double variance = t.variance_worst + (t.variance_best - t.variance_worst) * c;
    variance *= 1.0 + t.variance_coef[state] / 100.0;
    const double off = std::normal_distribution<double>(0.0, std::sqrt(std::max(1.0, variance)))(rng);
    if (std::abs(off) >= bad_window_ms) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return off;
}

bool zdon_ai_skin_arena() {
    return tex.skin_config_by_name.contains("zdon_ai_arena");
}

// "zdon_ai_lane" is on by presence (Green sets x = 0); a child skin that inherits it switches it off with x < 0
static bool zdon_ai_lane_on() {
    const auto it = tex.skin_config_by_name.find("zdon_ai_lane");
    return it != tex.skin_config_by_name.end() && it->second.x >= 0.0f;
}

bool zdon_ai_lane_game() {
    return zdon_ai_arena_game() && zdon_ai_lane_on();
}

bool zdon_ai_arena_game() {
    return global_data.zdon_ai.active && zdon_ai_skin_arena();
}

// skin_config zdon_ai_keep_cabinet (Nijiiro): its own ai_support_menu script is only for YataiDON's AI screens
bool zdon_ai_upstream() {
    if (!script_manager.has_lua_script("ai_support_menu") || tex.skin_config_by_name.contains("zdon_ai_keep_cabinet")) {
        return false;
    }
    // the script index keeps the nearest skin's file, so its path tells whose it is; cached per (script, skin)
    static std::string cached_script, cached_root;
    static bool cached = false;
    const std::string script = script_manager.get_lua_script_path("ai_support_menu");
    const std::string root = tex.skin_root().string();
    if (script != cached_script || root != cached_root) {
        const fs::path own = fs::absolute(tex.skin_root() / "Scripts").lexically_normal();
        const fs::path file = fs::absolute(fs::path(script)).lexically_normal();
        const auto [o, f] = std::ranges::mismatch(own, file);
        cached = o == own.end() && f != file.end();
        cached_script = script;
        cached_root = root;
    }
    return cached;
}

bool zdon_upai_arena() {
    return global_data.current_screen == "AI_GAME" && tex.skin_config_by_name.contains("zdon_upai_arena") &&
           zdon_ai_upstream();
}

bool zdon_ai_stage() {
    return zdon_ai_arena_game() || zdon_upai_arena();
}

void Player::use_upai_lane() {
    upai_ai = true;
    if (tex.has_texture("lane/ai_lane_cover")) {
        lane_cover_tex_id = tex.get_texture("lane/ai_lane_cover");
    }
}

PlayerData zdon_ai_player_data(int level) {
    PlayerData pd;
    pd.username = "AI";
    pd.title = "Lv." + std::to_string(level);
    pd.chara_color_1 = Color{0x3C, 0x46, 0x56, 0xFF}; // body: dark steel
    pd.chara_color_2 = Color{0xB9, 0xC6, 0xD4, 0xFF}; // face: silver
    pd.chara_color_3 = Color{0x7C, 0xE8, 0xF5, 0xFF}; // rim: cyan glow
    return pd;
}

void Player::enable_zdon_ai(int level) {
    // ZDON_AI_SEED=<n>: a fixed seed (reproducible hidden test runs)
    const char* seed_env = std::getenv("ZDON_AI_SEED");
    const unsigned seed =
        seed_env ? static_cast<unsigned>(std::strtoul(seed_env, nullptr, 10)) : std::random_device{}();
    zdon_ai = std::make_shared<ZdonAiDriver>(std::clamp(level, 1, 11), seed);
    // skin_config "zdon_ai_lane": the AI's lane is the AI's own (Green ghost: lane/ai_lane_cover, the circuit obi,
    // lane/ai_lane_background, no lane badges)
    if (zdon_ai_skin_arena() && zdon_ai_lane_on() && tex.has_texture("lane/ai_lane_cover")) {
        lane_cover_tex_id = tex.get_texture("lane/ai_lane_cover");
    }
}

std::vector<double> Player::zdon_note_times() const {
    // the queues fill when the song starts (reset_chart); read before any note is judged
    std::vector<double> times;
    times.reserve(don_notes.size() + kat_notes.size());
    for (const Note& n : don_notes) {
        times.push_back(n.hit_ms);
    }
    for (const Note& n : kat_notes) {
        times.push_back(n.hit_ms);
    }
    // a branched chart: the queues hold the common notes only (each branch section is merged in when the song
    // reaches it), so the note count and the last note come from the master branch as well (as upstream's AI
    // battle counts them); a section already merged is in the queues and gone from branch_m
    for (const NoteList& section : branch_m) {
        for (const Note& n : section.notes) {
            if (n.type >= NoteType::DON && n.type <= NoteType::KAT_L) {
                times.push_back(n.hit_ms);
            }
        }
    }
    std::ranges::sort(times);
    return times;
}

std::vector<double> Player::zdon_bar_times() const {
    std::vector<double> times;
    times.reserve(barlines.size());
    for (const Note& n : barlines) {
        times.push_back(n.hit_ms);
    }
    for (const NoteList& section : branch_m) { // the bar lines inside branch sections (master's)
        for (const Note& n : section.notes) {
            if (n.type == NoteType::BARLINE) {
                times.push_back(n.hit_ms);
            }
        }
    }
    std::ranges::sort(times);
    return times;
}

void Player::zdon_ai_manager(double ms_from_start, double current_ms, std::optional<Background>& background) {
    ZdonAiDriver& ai = *zdon_ai;
    if (is_drumroll || is_balloon) {
        const double interval = 1000.0 / (10.0 + ai.level);
        if (ms_from_start >= ai.next_roll_ms) {
            ai.next_roll_ms = std::max(ai.next_roll_ms + interval, ms_from_start - interval);
            autoplay_hit_side = autoplay_hit_side == Side::LEFT ? Side::RIGHT : Side::LEFT;
            spawn_hit_effects(DrumType::DON, autoplay_hit_side);
            if (!zdon_auto_catchup) {
                audio.play_sound(don_hitsound_id, VolumePreset::HITSOUND);
            }
            check_note(ms_from_start, DrumType::DON, current_ms, background);
        }
        return;
    }

    const double bad_window = (difficulty <= static_cast<int>(Difficulty::NORMAL)) ? Timing::BAD_EASY : Timing::BAD;
    for (int k = 0; k < 2; ++k) {
        auto& lane = k == 0 ? don_notes : kat_notes;
        const DrumType type = k == 0 ? DrumType::DON : DrumType::KAT;
        // a few notes can fall in one frame
        for (int guard = 0; guard < 8 && !lane.empty(); ++guard) {
            const Note& note = lane.front();
            if (ai.planned_index[k] != note.index) {
                ai.planned_index[k] = note.index;
                ai.planned_offset[k] = ai.draw_offset(bad_window);
            }
            const double off = ai.planned_offset[k];
            if (std::isnan(off) || ms_from_start < note.hit_ms + off) {
                break; // a planned miss: the note manager lets it pass
            }
            const bool big = note.type == NoteType::DON_L || note.type == NoteType::KAT_L;
            if (big) {
                spawn_hit_effects(type, Side::LEFT);
                spawn_hit_effects(type, Side::RIGHT);
            } else {
                autoplay_hit_side = autoplay_hit_side == Side::LEFT ? Side::RIGHT : Side::LEFT;
                spawn_hit_effects(type, autoplay_hit_side);
            }
            if (!zdon_auto_catchup) {
                audio.play_sound(k == 0 ? don_hitsound_id : kat_hitsound_id, VolumePreset::HITSOUND);
            }
            const size_t before = lane.size();
            check_note(note.hit_ms + off, type, current_ms, background);
            if (lane.size() == before) {
                break;
            }
        }
    }
}

static bool battle_conceded = false;

void zdon_ai_battle_concede() {
    battle_conceded = true;
    global_data.zdon_ai.skipped = true;
}

namespace {
    std::array<std::unique_ptr<Chara3D>, 2> vs_charas;
}

void zdon_ai_vs_chara_load() {
    if (!vs_charas[0]) {
        const auto pd = scores_manager.get_player_data(get_player_id(PlayerNum::P1));
        auto don = make_chara_from_player_data(pd ? &*pd : nullptr, false, true);
        if (pd) {
            don->set_don_colors(pd->chara_color_1, pd->chara_color_2, pd->chara_color_3);
            don->apply_face(pd->chara_face_index);
        } else {
            don->set_don_colors(chara_default_color_1(PlayerNum::P1), chara_default_color_2(PlayerNum::P1),
                                {249, 240, 225, 255});
        }
        don->set_anim(AnimIndex::DON_NORMAL);
        vs_charas[0] = std::move(don);
    }
    if (!vs_charas[1]) {
        PlayerData ai = zdon_ai_player_data(global_data.config->general.zdon_ai_level);
        ai.chara_is_costume = true;
        ai.chara_cos_index = AI_COSTUME;
        vs_charas[1] = make_chara_from_player_data(&ai, false, true);
        vs_charas[1]->set_anim(AnimIndex::DON_NORMAL);
    }
}

void zdon_ai_vs_chara_draw(int side, float x, float y, float scale, float rot_y) {
    zdon_ai_vs_chara_load();
    Chara3D& c = *vs_charas[side == 0 ? 0 : 1];
    const Chara3DConfig& cfg = tex.chara_3d_config;
    c.update(get_current_ms());
    c.set_rotation(cfg.rot_x, rot_y, cfg.rot_z);
    c.set_draw_scale(scale);
    c.set_puchi_hidden(true);
    c.draw(x, y);
    c.set_puchi_hidden(false);
}

void zdon_ai_vs_chara_release() {
    vs_charas[0].reset();
    vs_charas[1].reset();
}

void zdon_ai_battle_begin(int level) {
    auto& b = global_data.zdon_ai;
    battle_conceded = false;
    std::vector<float> traced = std::move(b.player_offsets); // the player's timing carries over between songs
    b = {};
    end_cue_pending = false;
    last_cue_pending = false;
    match_cue_pending = false;
    ready_se_pending = false;
    ready_voice_pending = false;
    ready_start_pending = false;
    b.active = true;
    b.level = std::clamp(level, 1, 11);
    b.condition = zdon_ai_condition();
    b.player_offsets = std::move(traced);
}

static void make_sections(const std::vector<double>& note_times, const std::vector<double>& bar_times) {
    auto& b = global_data.zdon_ai;
    const size_t n = note_times.size();
    // 5 sections from 200 notes (music_ai_section's usual split); a skin's "zdon_ai_rules" x overrides it
    // (Green ghosttuning.xml thres5section = 120)
    const auto rules = tex.skin_config_by_name.find("zdon_ai_rules");
    const size_t five_from =
        rules != tex.skin_config_by_name.end() && rules->second.x > 0 ? static_cast<size_t>(rules->second.x) : 200;
    const size_t count = n >= five_from ? 5 : 3;
    // AiEnsoGameManager (disassembly of its section setup): step = (LastOnpuDurationTime + 500) / count from the
    // chart's start; section i ends at SyusetsuJustTime(NumberOfSyousetsuByTime((i + 1) * step) + 1), the bar line
    // that starts the measure after the one holding that time; the last one ends 500 ms after the last note
    const double first = note_times.front();
    const double last = note_times.back();
    const double step = (last + 500.0) / static_cast<double>(count);
    for (size_t s = 1; s < count; ++s) {
        const double at = step * static_cast<double>(s);
        double cut = at;
        for (double bar : bar_times) {
            if (bar > at) {
                cut = bar;
                break;
            }
        }
        if (cut > first && cut < last && (b.bounds.empty() || cut > b.bounds.back())) {
            b.bounds.push_back(cut);
        }
    }
    b.bounds.push_back(last + 500.0);
    b.points_p.assign(b.bounds.size(), 0);
    b.points_ai.assign(b.bounds.size(), 0);
    b.winners.assign(b.bounds.size(), -1);
    b.crowns.assign(b.bounds.size(), 0);
    b.section_judge.assign(b.bounds.size(), {0, 0, 0, 0});
    b.section_score.assign(b.bounds.size(), 0);
    spdlog::info("[zdon_ai] tuning level {}, condition {}: {} notes, {} sections", b.level, b.condition, n,
                 b.bounds.size());
}

void zdon_ai_battle_update(double chart_ms, const Player& player, const Player& ai) {
    auto& b = global_data.zdon_ai;
    if (ready_start_pending && !b.hold_ready_cue) {
        ready_start_pending = false;
        ready_voice_cue();
        ready_se_pending = true;
        ready_se_at_ms = get_current_ms() + kReadySeLagMs;
    }
    if (ready_voice_pending && get_current_ms() >= ready_voice_at_ms) {
        ready_voice_pending = false;
        cue("", "ready", 3);
    }
    if (ready_se_pending && get_current_ms() >= ready_se_at_ms) {
        ready_se_pending = false;
        cue("ready", "", 1);
    }
    if (match_cue_pending && get_current_ms() >= match_cue_at_ms) {
        match_cue_pending = false;
        cue("", match_cue_voice, 1);
    }
    if (last_cue_pending && get_current_ms() >= last_cue_at_ms) {
        last_cue_pending = false;
        cue(last_cue_won ? "section_win" : "section_lose", last_cue_won ? "section_win" : "section_lose", 2);
    }
    if (end_cue_pending && get_current_ms() >= end_cue_at_ms) {
        end_cue_pending = false;
        cue(end_cue_won ? "song_win" : "song_lose", end_cue_won ? "song_win" : "song_lose", 1);
    }
    if (b.active && b.bounds.empty()) {
        const std::vector<double> times = ai.zdon_note_times();
        if (times.empty()) {
            return; // the song has not started
        }
        make_sections(times, ai.zdon_bar_times());
        if (zdon_ai_end_delay_on()) {
            ready_voice_cue();
            ready_se_pending = true;
            // skin_config zdon_ai_ready_se_ms (Green): the ready SE 1.62 s before the song (footage Tl7g8XXXvAM: SE
            // 138.385 s, song 140.0 s); default kReadySeDelayMs
            double se_ms = kReadySeDelayMs;
            if (const auto it = tex.skin_config_by_name.find("zdon_ai_ready_se_ms");
                it != tex.skin_config_by_name.end() && it->second.x > 0.0f) {
                se_ms = it->second.x;
            }
            ready_se_at_ms = get_current_ms() + se_ms;
        } else if (end_hold_ms() > 0) {
            ready_start_pending = true; // the sections are cut behind the loading card, the cues wait for the screen
        } else {
            cue("ready", "ready", 3);
        }
    }
    if (!b.active || b.finished || b.bounds.empty()) {
        return;
    }
    const ZdonAiTuning& t = zdon_ai_tuning(b.level);
    const int tp = section_points(player, t);
    const int ta = section_points(ai, t);
    b.judge_p = {player.get_good(), player.get_ok(), player.get_bad(), player.get_total_drumroll()};
    b.judge_ai = {ai.get_good(), ai.get_ok(), ai.get_bad(), ai.get_total_drumroll()};
    b.live_p = tp - b.base_p;
    b.live_ai = ta - b.base_ai;
    b.tug = std::clamp(b.live_p - b.live_ai + 9, 1, 17);

    // the AI's state on the section's difference (AI - player)
    const int diff = b.live_ai - b.live_p;
    if (b.state == 0) {
        if (diff <= t.state_n_s) {
            b.state = 1;
        } else if (diff >= t.state_n_i) {
            b.state = 2;
        }
    } else if (b.state == 1 && diff >= t.state_s_n) {
        b.state = 0;
    } else if (b.state == 2 && diff <= t.state_i_n) {
        b.state = 0;
    }

    while (b.section < static_cast<int>(b.bounds.size()) && chart_ms >= b.bounds[b.section]) {
        const int s = b.section;
        b.points_p[s] = b.live_p;
        b.points_ai[s] = b.live_ai;
        b.winners[s] = (battle_conceded || b.tug < 9) ? 1 : 0;
        b.crowns[s] = section_crown(b.judge_p[0] - b.base_judge[0], b.judge_p[1] - b.base_judge[1],
                                    b.judge_p[2] - b.base_judge[2]);
        b.base_p = tp;
        b.base_ai = ta;
        for (int k = 0; k < 4; ++k) {
            b.section_judge[s][k] = b.judge_p[k] - b.base_judge[k];
        }
        b.base_judge = b.judge_p;
        b.section_score[s] = player.get_score() - b.base_score;
        b.base_score = player.get_score();
        b.live_p = b.live_ai = 0;
        b.tug = 9;
        b.state = 0;
        b.section++;
        const int n = static_cast<int>(b.winners.size());
        if (b.section < n && !battle_conceded) {
            cue(b.winners[s] == 0 ? "section_win" : "section_lose", b.winners[s] == 0 ? "section_win" : "section_lose",
                2);
            // the next section decides the match / gives the AI the match (voice_aienso section_match / ai_reach)
            const int wins_p = static_cast<int>(std::ranges::count(b.winners, 0));
            const int wins_ai = static_cast<int>(std::ranges::count(b.winners, 1));
            const int left = n - b.section;
            if (wins_ai + 1 > n / 2 && wins_ai <= n / 2 && wins_p + left > wins_ai) {
                match_cue_pending = true;
                match_cue_voice = "section_ai_reach";
                match_cue_at_ms = get_current_ms() + kMatchVoiceDelayMs;
            } else if (left == 1 && wins_p == wins_ai) {
                match_cue_pending = true;
                match_cue_voice = "section_match";
                match_cue_at_ms = get_current_ms() + kMatchVoiceDelayMs;
            }
        }
    }
    if (!b.finished && b.section >= static_cast<int>(b.bounds.size())) {
        // skin_config zdon_ai_end_hold (Nijiiro): the last section's own SE comes at the section end and the
        // emblem (cut-in, song cue) follows 3.25 s later (footage MBAZD6xA1D8: last note 152.5 s, section flash 153.0,
        // emblem 156.25; n6a4oDaaJ4w: the last section's SE alone 0.43 s after the last note)
        const double hold_ms = end_hold_ms();
        b.finished = true;
        b.finished_at_ms = get_current_ms() + std::max(0.0, hold_ms);
        const int wins_p = static_cast<int>(std::ranges::count(b.winners, 0));
        b.won = wins_p >= static_cast<int>(b.winners.size()) - wins_p;
        if (battle_conceded) {
            // no end cut-in and no cue: the skip banner, then the result
        } else if (zdon_ai_end_delay_on()) {
            end_cue_pending = true;
            end_cue_won = b.won;
            end_cue_at_ms = get_current_ms() + kEndCueDelayMs;
            last_cue_pending = !b.winners.empty();
            last_cue_won = !b.winners.empty() && b.winners.back() == 0;
            last_cue_at_ms = get_current_ms() + kLastSectionCueDelayMs;
        } else if (hold_ms > 0 && !b.winners.empty()) {
            cue(b.winners.back() == 0 ? "section_win" : "section_lose", "", 1);
            end_cue_pending = true;
            end_cue_won = b.won;
            end_cue_at_ms = b.finished_at_ms;
        } else {
            cue(b.won ? "song_win" : "song_lose", b.won ? "song_win" : "song_lose", 1);
        }
        // the arcade only records a played match: never auto play (ZDON_AI_RECORD_AUTO=1: tests record anyway)
        // nor a simplified chart (zdon_ai_support_levels), as upstream's AI battle
        const bool record =
            !battle_conceded && player.get_modifiers().subdiff == 0 &&
            ((!global_data.force_auto_play && !player.is_auto_play()) || std::getenv("ZDON_AI_RECORD_AUTO"));
        if (record) {
            global_data.config->general.zdon_ai_wins += b.won ? 1 : 0;
            save_config(*global_data.config);
            const auto& sd = global_data.session_data[static_cast<int>(PlayerNum::P1)];
            scores_manager.set_zdon_ai_section_crowns(get_player_id(PlayerNum::P1), sd.song_hash,
                                                      sd.selected_difficulty, b.crowns);
            // AiResultSectionMain: the detail view compares each section with the best play's
            b.section_best = scores_manager.get_zdon_ai_section_best(get_player_id(PlayerNum::P1), sd.song_hash,
                                                                     sd.selected_difficulty);
            std::vector<std::array<int, 5>> rec(b.bounds.size());
            for (size_t i = 0; i < rec.size(); ++i) {
                rec[i] = {b.section_score[i], b.section_judge[i][0], b.section_judge[i][1], b.section_judge[i][2],
                          b.section_judge[i][3]};
            }
            b.new_best = scores_manager.set_zdon_ai_section_best(get_player_id(PlayerNum::P1), sd.song_hash,
                                                                 sd.selected_difficulty, player.get_score(), rec);
            // the Green ghost rank (ghostsetting.xml: a win 1 point, high level +1, first win +1; EBOOT sub_B05D4)
            auto [points, rank] = scores_manager.get_zdon_ai_rank(get_player_id(PlayerNum::P1));
            b.points_before = b.points = points;
            b.rank_before = b.rank = rank;
            last_first_win = last_bonus_high = false;
            pages_open = pages_finished = false; // a new result: its cut-in pages start closed
            page_steps = 0;
            if (b.won) {
                const int c = std::clamp(sd.selected_difficulty, 0, 4);
                const int star = static_cast<int>(b.star);
                // the rank-up conditions are the next rank's row (ai_rankup_todo rank_r shows row r + 1's
                // minimums; at rank 10 only the task folder's songs count = least 99, not reachable here)
                const ZdonAiRank& r = zdon_ai_rank_info(std::min(rank + 1, kZdonAiRanks - 1));
                const bool first = scores_manager.add_zdon_ai_first_win(get_player_id(PlayerNum::P1), sd.song_hash, c);
                int least = r.least[c];
                // test hook: ZDON_AI_TASK_STAR=N lets a course of N stars or more count at rank 10 (the cabinet's task
                // folder songs are not in Z-DON), so the rank 10 -> 10+ cut-ins can be reproduced
                if (rank + 1 == kZdonAiRanks - 1) {
                    if (const char* env = std::getenv("ZDON_AI_TASK_STAR")) {
                        least = std::atoi(env);
                    }
                }
                if (rank + 1 < kZdonAiRanks && least > 0 && star >= least) {
                    last_bonus_high = r.bonus[c] > 0 && star >= r.bonus[c];
                    last_first_win = first;
                    b.rank_gain = 1 + (last_bonus_high ? 1 : 0) + (first ? 1 : 0);
                }
                // the points count within the current rank: reaching the next row's `acquired` ranks up and the
                // counter restarts at 0 (footage Tl7g8XXXvAM 4:56-5:06: a first win right after rank 0 -> rank 1
                // shows "8 points" to rank 2 = ghostinfo row 2's acquired, not 8 minus this win's points)
                b.points = points + b.rank_gain;
                b.rank = rank;
                if (rank + 1 < kZdonAiRanks && b.points >= zdon_ai_rank_info(rank + 1).acquired) {
                    b.rank = rank + 1;
                    b.points = 0;
                }
                scores_manager.set_zdon_ai_rank(get_player_id(PlayerNum::P1), b.points, b.rank);
            }
        }
    }
}
