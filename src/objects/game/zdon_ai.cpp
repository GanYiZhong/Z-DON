#include "zdon_ai.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <spdlog/spdlog.h>

#include "../../libs/audio.h"
#include "../../libs/global_data.h"
#include "../../libs/scores.h"
#include "player.h"

namespace {
    constexpr size_t kLearnSamples = 64; // the player's offsets kept (global_data.zdon_ai.player_offsets)
    constexpr double kLearnWeight = 0.7; // how far the AI moves toward the player's timing at a full buffer
    constexpr int kLeadPoints = 6;       // section lead (points) that switches the AI's state
    constexpr int kEvenPoints = 1;       // ... and back to even

    int section_points(const Player& p) {
        return p.get_good() * 2 + p.get_ok();
    }
} // namespace

double ZdonAiDriver::draw_offset(double bad_window_ms) {
    const auto& battle = global_data.zdon_ai;
    double sigma = 34.0 - 2.0 * (level - 1);
    double slip = 0.06 - 0.006 * (level - 1);
    double mean = 0.0;

    const auto& offs = battle.player_offsets;
    if (offs.size() >= 2) {
        const double n = (double)offs.size();
        const double m = std::accumulate(offs.begin(), offs.end(), 0.0) / n;
        double var = 0.0;
        for (float o : offs) {
            var += (o - m) * (o - m);
        }
        const double sd = std::sqrt(var / (n - 1));
        const double w = kLearnWeight * std::min(1.0, n / (double)kLearnSamples);
        mean = m * w;
        sigma += (sd - sigma) * w;
    }
    if (battle.state == 1) { // leading: loosen
        sigma *= 1.5;
        slip *= 2.0;
    } else if (battle.state == 2) { // trailing: tighten
        sigma *= 0.7;
        slip *= 0.5;
    }
    sigma = std::clamp(sigma, 6.0, 60.0);

    if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < slip) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double off = std::normal_distribution<double>(mean, sigma)(rng);
    if (std::abs(off) >= bad_window_ms) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return off;
}

PlayerData zdon_ai_player_data(int level) {
    PlayerData pd;
    pd.username = "AI";
    pd.title = "Lv." + std::to_string(level);
    pd.chara_color_1 = Color{0x5A, 0x6E, 0x8C, 0xFF};
    pd.chara_color_2 = Color{0x68, 0xBF, 0xC0, 0xFF};
    pd.chara_color_3 = Color{0xF9, 0xF0, 0xE1, 0xFF};
    return pd;
}

void Player::enable_zdon_ai(int level) {
    // ZDON_AI_SEED=<n>: a fixed seed (reproducible hidden test runs)
    const char* seed_env = std::getenv("ZDON_AI_SEED");
    const unsigned seed = seed_env ? (unsigned)std::strtoul(seed_env, nullptr, 10) : std::random_device{}();
    zdon_ai = std::make_shared<ZdonAiDriver>(std::clamp(level, 1, 10), seed);
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

void Player::zdon_ai_manager(double ms_from_start, double current_ms, std::optional<Background>& background) {
    ZdonAiDriver& ai = *zdon_ai;
    if (is_drumroll || is_balloon) {
        const double interval = 1000.0 / (10.0 + ai.level);
        if (ms_from_start >= ai.next_roll_ms) {
            ai.next_roll_ms = std::max(ai.next_roll_ms + interval, ms_from_start - interval);
            autoplay_hit_side = autoplay_hit_side == Side::LEFT ? Side::RIGHT : Side::LEFT;
            spawn_hit_effects(DrumType::DON, autoplay_hit_side);
            audio.play_sound(don_hitsound_id, VolumePreset::HITSOUND);
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
            audio.play_sound(k == 0 ? don_hitsound_id : kat_hitsound_id, VolumePreset::HITSOUND);
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
    std::vector<float> learned = std::move(b.player_offsets); // the player's timing carries over between songs
    b = {};
    b.active = true;
    b.level = level;
    b.player_offsets = std::move(learned);
}

static void make_sections(const std::vector<double>& note_times) {
    auto& b = global_data.zdon_ai;
    const size_t n = note_times.size();
    const size_t count = n >= 200 ? 5 : 3;
    for (size_t s = 1; s < count; ++s) {
        const size_t split = n * s / count;
        if (split == 0 || split >= n) {
            continue;
        }
        b.bounds.push_back((note_times[split - 1] + note_times[split]) / 2.0);
    }
    b.bounds.push_back(n ? note_times.back() + 500.0 : 0.0);
    b.points_p.assign(b.bounds.size(), 0);
    b.points_ai.assign(b.bounds.size(), 0);
    b.winners.assign(b.bounds.size(), -1);
    spdlog::info("[zdon_ai] level {}: {} notes, {} sections", b.level, n, b.bounds.size());
}

void zdon_ai_battle_update(double chart_ms, const Player& player, const Player& ai) {
    auto& b = global_data.zdon_ai;
    if (b.active && b.bounds.empty()) {
        const std::vector<double> times = ai.zdon_note_times();
        if (times.empty()) {
            return; // the song has not started
        }
        make_sections(times);
    }
    if (!b.active || b.finished || b.bounds.empty()) {
        return;
    }
    const int tp = section_points(player);
    const int ta = section_points(ai);
    b.live_p = tp - b.base_p;
    b.live_ai = ta - b.base_ai;

    const int lead = b.live_ai - b.live_p;
    if (b.state == 0) {
        if (lead >= kLeadPoints) {
            b.state = 1;
        } else if (lead <= -kLeadPoints) {
            b.state = 2;
        }
    } else if (std::abs(lead) <= kEvenPoints) {
        b.state = 0;
    }

    while (b.section < (int)b.bounds.size() && chart_ms >= b.bounds[b.section]) {
        const int s = b.section;
        b.points_p[s] = b.live_p;
        b.points_ai[s] = b.live_ai;
        b.winners[s] = b.live_p > b.live_ai ? 0 : (b.live_ai > b.live_p ? 1 : 2);
        b.base_p = tp;
        b.base_ai = ta;
        b.live_p = b.live_ai = 0;
        b.state = 0;
        b.section++;
    }
    if (!b.finished && b.section >= (int)b.bounds.size()) {
        b.finished = true;
        // the next match's level follows this one: +1 after a win, -1 after a loss (the arcade raises its AI
        // with the player's record the same way)
        int wins_p = 0, wins_ai = 0, total = 0;
        for (size_t i = 0; i < b.winners.size(); ++i) {
            wins_p += b.winners[i] == 0;
            wins_ai += b.winners[i] == 1;
            total += b.points_p[i] - b.points_ai[i];
        }
        const int d = wins_p != wins_ai ? wins_p - wins_ai : total;
        int& level = global_data.config->general.zdon_ai_level;
        const int next = std::clamp(level + (d > 0 ? 1 : (d < 0 ? -1 : 0)), 1, 10);
        if (next != level && !global_data.force_auto_play) { // never from auto play (tests)
            level = next;
            save_config(*global_data.config);
        }
    }
}
