#include "song_select_online.h"

#include <algorithm>

#include "../libs/audio/audio.h"
#include "../libs/input/input.h"
#include "../objects/global/zdon_hud.h"

void OnlineSongSelectScreen::on_screen_start() {
    SongSelectScreen::on_screen_start();
    modal = Modal::None;
    entries.clear();
}

void OnlineSongSelectScreen::select_song(SongBox* box) {
    if (modal != Modal::None) {
        return; // the course stays decided behind the window
    }
    // the options are read from scores.db when the game starts (SongSelectScreen::select_song saves them too)
    scores_manager.save_player_data(player->player_data);
    difficulty = static_cast<int>(player->selected_difficulty);
    title = box->text_name;
    entries.clear();
    cursor = 0;
    network.request_challenges(box->hash_for(difficulty), difficulty);
    modal = Modal::Loading;
}

void OnlineSongSelectScreen::close_modal() {
    modal = Modal::None;
    replay_is_challenge = false;
    navigator.exit_diff_select();
    state = SongSelectState::BROWSING;
    player->reset_selection();
}

void OnlineSongSelectScreen::update_modal() {
    const bool l_kat = is_l_kat_pressed();
    const bool r_kat = is_r_kat_pressed();
    const bool don = is_l_don_pressed() || is_r_don_pressed();
    const bool cancel = check_key_pressed(global_data.config->keys.back_key);
    switch (modal) {
    case Modal::Loading:
        if (auto list = network.take_challenges()) {
            entries = std::move(list->entries);
            modal = !list->ok ? Modal::Offline : entries.empty() ? Modal::Empty : Modal::List;
        } else if (cancel) {
            close_modal();
        }
        break;
    case Modal::List:
        if (cancel) {
            close_modal();
        } else if (l_kat || r_kat) {
            const int n = static_cast<int>(entries.size());
            cursor = (cursor + (r_kat ? 1 : n - 1)) % n;
            audio.play_sound("kat", VolumePreset::SOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::SOUND);
            replay_is_challenge = true; // take_replay_jump starts the battle when the play arrives
            network.request_replay(entries[cursor].score_id);
            fetch_started_ms = get_current_ms();
            modal = Modal::Fetching;
        }
        break;
    case Modal::Fetching:
        // still here long after the request: it failed (no answer, or the chart is not in this library)
        if (cancel || get_current_ms() - fetch_started_ms > 12000.0) {
            replay_is_challenge = false;
            modal = cancel ? Modal::List : Modal::Failed;
        }
        break;
    case Modal::Empty:
    case Modal::Offline:
    case Modal::Failed:
        if (don || l_kat || r_kat || cancel) {
            close_modal();
        }
        break;
    case Modal::None:
        break;
    }
}

std::optional<Screens> OnlineSongSelectScreen::update() {
    if (modal != Modal::None) {
        update_modal();
        clear_input_buffers(); // the song select behind the window gets none of it
    }
    return SongSelectScreen::update();
}

void OnlineSongSelectScreen::draw() {
    SongSelectScreen::draw();
    const float u = tex.screen_width / 1920.0f;
    const Color tag_color{140, 220, 255, 255};
    OutlinedText& tag = zhud::label(zhud::text("online_mode"), 26, tag_color);
    const float tw = tag.width + 28 * u;
    const float th = tag.height + 10 * u;
    zhud::panel(tex.screen_width / 2.0f - tw / 2, 10 * u, tw, th, 0.8f, fade(tag_color, 0.8f));
    tag.draw({.x = tex.screen_width / 2.0f - tag.width / 2, .y = 15 * u});
    if (modal != Modal::None) {
        draw_modal();
    }
}

void OnlineSongSelectScreen::draw_modal() {
    const float u = tex.screen_width / 1920.0f;
    const Color gold{255, 226, 120, 255};
    const float w = 900 * u;
    const float x = (tex.screen_width - w) / 2.0f;
    auto centred = [&](OutlinedText& t, float y) { t.draw({.x = (tex.screen_width - t.width) / 2.0f, .y = y}); };

    if (modal != Modal::List) {
        const char* key = modal == Modal::Loading    ? "online_loading"
                          : modal == Modal::Empty    ? "online_none"
                          : modal == Modal::Offline  ? "online_offline"
                          : modal == Modal::Fetching ? "online_fetching"
                                                     : "online_failed";
        OutlinedText& t = zhud::label(zhud::text(key), 30, modal == Modal::Fetching ? gold : Color::WHITE);
        const float h = t.height + 60 * u;
        const float y = (tex.screen_height - h) / 2.0f;
        zhud::panel(x, y, w, h, 0.92f, fade(gold, 0.7f));
        centred(t, y + 30 * u);
        return;
    }

    // the list: up to 8 rows around the cursor
    constexpr int kRows = 8;
    const int n = static_cast<int>(entries.size());
    const int first = std::clamp(cursor - kRows / 2, 0, std::max(0, n - kRows));
    const int shown = std::min(kRows, n);
    const float row_h = 48 * u;
    OutlinedText& head = zhud::label(zhud::text("online_list_title", title, zhud::course_name(difficulty)), 30, gold);
    OutlinedText& hint = zhud::label(zhud::text("online_list_hint"), 22, Color{200, 200, 200, 255});
    const float h = 30 * u + head.height + 16 * u + shown * row_h + 16 * u + hint.height + 24 * u;
    const float y0 = (tex.screen_height - h) / 2.0f;
    zhud::panel(x, y0, w, h, 0.94f, fade(gold, 0.7f));
    centred(head, y0 + 22 * u);
    float y = y0 + 30 * u + head.height + 16 * u;
    for (int i = first; i < first + shown; ++i) {
        const ChallengeEntry& e = entries[i];
        const bool on = i == cursor;
        if (on) {
            zhud::panel(x + 20 * u, y, w - 40 * u, row_h - 6 * u, 0.6f, gold);
        }
        const Color c = on ? gold : Color::WHITE;
        OutlinedText& rank = zhud::label(std::to_string(i + 1), 26, c);
        OutlinedText& name = zhud::label(e.username, 26, c);
        std::string score = std::to_string(e.score);
        for (int p = static_cast<int>(score.size()) - 3; p > 0; p -= 3) {
            score.insert(static_cast<size_t>(p), ",");
        }
        OutlinedText& sc = zhud::label(score, 26, c);
        const float ty = y + (row_h - 6 * u - name.height) / 2.0f;
        rank.draw({.x = x + 70 * u - rank.width, .y = ty});
        name.draw({.x = x + 100 * u, .y = ty});
        sc.draw({.x = x + w - 60 * u - sc.width, .y = ty});
        y += row_h;
    }
    centred(hint, y + 16 * u);
}
