#include "zdon_ai.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <spdlog/spdlog.h>

#include "../../libs/audio/audio.h"
#include "../../libs/file/scores.h"
#include "../../libs/global_data.h"
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

    // the arcade's AI battle win-count rewards (reward.bin, releaseType 42)
    constexpr int kRewardWins[] = {1,   2,   10,  25,  50,  75,  100,  125,  150,  175,  200,  250,  300,
                                   350, 400, 500, 600, 765, 876, 1000, 2000, 3000, 4000, 5000, 10000};

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
        return (float)(good + ok) / (float)all > 0.7f ? 1 : 0;
    }
} // namespace

const ZdonAiTuning& zdon_ai_tuning(int level) {
    return kTuning[std::clamp(level, 1, 11) - 1];
}

int zdon_ai_tuning_level(double star_level) {
    const int s = (int)star_level;
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

bool zdon_ai_arena_game() {
    return global_data.zdon_ai.active && zdon_ai_skin_arena();
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
    const unsigned seed = seed_env ? (unsigned)std::strtoul(seed_env, nullptr, 10) : std::random_device{}();
    zdon_ai = std::make_shared<ZdonAiDriver>(std::clamp(level, 1, 11), seed);
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
    std::ranges::sort(times);
    return times;
}

std::vector<double> Player::zdon_bar_times() const {
    std::vector<double> times;
    times.reserve(barlines.size());
    for (const Note& n : barlines) {
        times.push_back(n.hit_ms);
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

    const double bad_window = (difficulty <= (int)Difficulty::NORMAL) ? Timing::BAD_EASY : Timing::BAD;
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

void zdon_ai_battle_begin(int level) {
    auto& b = global_data.zdon_ai;
    std::vector<float> traced = std::move(b.player_offsets); // the player's timing carries over between songs
    b = {};
    b.active = true;
    b.level = std::clamp(level, 1, 11);
    b.condition = zdon_ai_condition();
    b.player_offsets = std::move(traced);
}

static void make_sections(const std::vector<double>& note_times, const std::vector<double>& bar_times) {
    auto& b = global_data.zdon_ai;
    const size_t n = note_times.size();
    const size_t count = n >= 200 ? 5 : 3;
    // AiEnsoGameManager: the cuts are the bar lines that start a new measure after equal parts of the song, the
    // last section ends 500 ms after the last note
    const double first = note_times.front();
    const double last = note_times.back();
    for (size_t s = 1; s < count; ++s) {
        const double at = first + (last - first) * (double)s / (double)count;
        double cut = at;
        for (double bar : bar_times) {
            if (bar >= at) {
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
    spdlog::info("[zdon_ai] tuning level {}, condition {}: {} notes, {} sections", b.level, b.condition, n,
                 b.bounds.size());
}

void zdon_ai_battle_update(double chart_ms, const Player& player, const Player& ai) {
    auto& b = global_data.zdon_ai;
    if (b.active && b.bounds.empty()) {
        const std::vector<double> times = ai.zdon_note_times();
        if (times.empty()) {
            return; // the song has not started
        }
        make_sections(times, ai.zdon_bar_times());
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

    while (b.section < (int)b.bounds.size() && chart_ms >= b.bounds[b.section]) {
        const int s = b.section;
        b.points_p[s] = b.live_p;
        b.points_ai[s] = b.live_ai;
        b.winners[s] = b.tug < 9 ? 1 : 0;
        b.crowns[s] = section_crown(b.judge_p[0] - b.base_judge[0], b.judge_p[1] - b.base_judge[1],
                                    b.judge_p[2] - b.base_judge[2]);
        b.base_p = tp;
        b.base_ai = ta;
        b.base_judge = {b.judge_p[0], b.judge_p[1], b.judge_p[2]};
        b.live_p = b.live_ai = 0;
        b.tug = 9;
        b.state = 0;
        b.section++;
    }
    if (!b.finished && b.section >= (int)b.bounds.size()) {
        b.finished = true;
        const int wins_p = (int)std::ranges::count(b.winners, 0);
        b.won = wins_p >= (int)b.winners.size() - wins_p;
        // the arcade only records a played match: never auto play (ZDON_AI_RECORD_AUTO=1: tests record anyway)
        const bool record =
            (!global_data.force_auto_play && !player.is_auto_play()) || std::getenv("ZDON_AI_RECORD_AUTO");
        if (record) {
            global_data.config->general.zdon_ai_wins += b.won ? 1 : 0;
            save_config(*global_data.config);
            const auto& sd = global_data.session_data[(int)PlayerNum::P1];
            scores_manager.set_zdon_ai_section_crowns(get_player_id(PlayerNum::P1), sd.song_hash,
                                                      sd.selected_difficulty, b.crowns);
        }
    }
}
