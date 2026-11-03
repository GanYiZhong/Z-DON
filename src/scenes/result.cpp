#include "result.h"

#include "../libs/input/input.h"
#include "../libs/zdon_record.h"
#include "../objects/game/player.h"
#include "../objects/game/zdon_ai.h"
#include "../objects/global/zdon_hud.h"

void ResultScreen::on_screen_start() {
    Screen::on_screen_start();
    SessionData& session_data = global_data.session_data[static_cast<int>(global_data.player_num)];
    song_title = session_data.song_title;
    song_subtitle = (session_data.song_subtitle_full_display && !session_data.song_subtitle.empty())
                        ? session_data.song_subtitle
                        : std::string();
    song_number = global_data.songs_played + 1;
    audio.play_sound("bgm", VolumePreset::MUSIC);
    fade_out = dynamic_cast<FadeAnimation*>(tex.get_animation(0));
    retry_message.reset();
    retrying = false;
    fade_in.emplace(global_data.player_num);
    skipped_time = 0;
    skip_enabled_ms = 0;
    fs::path loading_graphic_path = session_data.selected_song.parent_path() / "Loading.png";
    background.reset();
    if (exists(loading_graphic_path)) {
        Texture loaded = load_texture(loading_graphic_path.string().c_str());
        if (loaded.id != 0) {
            loading_graphic.emplace(loaded);
        } else {
            spdlog::error("Failed to load result loading graphic: {}", loading_graphic_path.string());
            background.emplace(global_data.player_num, tex.screen_width);
        }
    } else {
        background.emplace(global_data.player_num, tex.screen_width);
    }
    player_1.emplace(global_data.player_num, false, false);
}

Screens ResultScreen::on_screen_end(Screens next_screen) {
    if (loading_graphic.has_value()) {
        unload_texture(loading_graphic.value());
        loading_graphic.reset();
    }
    // the retry mini-game restarts the song from this session
    if (next_screen != Screens::RETRY_GAME) {
        reset_session();
    }
    player_1.reset(); // Z-DON: its Don's model goes back to the part pool for the next screen
    return Screen::on_screen_end(next_screen);
}

double ResultScreen::reveal_end_ms() {
    return player_1.has_value() ? player_1->reveal_end_ms() : 0.0;
}

bool ResultScreen::retry_pending() const {
    const SessionData& session_data = global_data.session_data[static_cast<int>(global_data.player_num)];
    const bool limit_reached =
        global_data.config->limits.song_limit > 0 && global_data.config->limits.song_limit == global_data.songs_played;
    return allow_retry && !tex.options[SCO::DISABLE_RETRY_GAME] && !session_data.result_data.cleared &&
           !limit_reached && script_manager.has_lua_script("retry_game");
}

bool ResultScreen::exiting() const {
    return retrying || (fade_out && fade_out->is_started);
}

// Z-DON AI battle: the credit's last song (limits.song_limit reached) shows the credit's AI songs before it ends
bool ResultScreen::zdon_play_all_pending() const {
    const auto& b = global_data.zdon_ai;
    return b.active && !b.play_all && global_data.config->limits.song_limit > 0 &&
           global_data.songs_played >= global_data.config->limits.song_limit && !global_data.zdon_ai_credit.empty();
}

void ResultScreen::begin_exit() {
    if (zdon_play_all_pending()) {
        global_data.zdon_ai.play_all = true;
        zdon_play_all_ms = get_current_ms();
        return;
    }
    if (global_data.zdon_ai.play_all && get_current_ms() - zdon_play_all_ms < 1000) {
        return; // the press that opened it
    }
    retrying = retry_pending() && fade_in.has_value();
    if (retrying) {
        audio.play_sound("retry_transition", VolumePreset::SOUND);
        fade_in->start_reverse();
    } else {
        fade_out->start();
    }
}

void ResultScreen::handle_input(double current_ms) {
    if (!fade_out) {
        return;
    }
    if (!menu_confirm_pressed()) {
        return;
    }

    double reveal_end = reveal_end_ms();
    if (reveal_end == 0) {
        if (skipped_time == 0) {
            skipped_time = current_ms;
            audio.play_sound("don", VolumePreset::SOUND);
        }
        return;
    }
    const double advance_delay =
        global_data.config->general.fast_transitions ? kFastInputDelayMs : kWaitEffectEndMs + kWaitNextSceneMs;
    if (current_ms >= reveal_end + advance_delay && !exiting()) {
        begin_exit();
        audio.play_sound("don", VolumePreset::SOUND);
    }
}

void ResultScreen::update_input_and_timeout(double current_ms) {
    if (!fade_out || exiting()) {
        return;
    }
    {
        const double r = reveal_end_ms();
        zdon_ai_set_revealed(r > 0 && current_ms >= r);
    }
    if (zdon_hist_input()) {
        return; // the skin's AI battle cut-in pages take the input and hold the timeout
    }

    if (skip_enabled_ms == 0 && fade_in.has_value() && fade_in->is_finished()) {
        skip_enabled_ms =
            current_ms + (global_data.config->general.fast_transitions ? kFastInputDelayMs : kEnableSkipMs);
    }

    if (skip_enabled_ms > 0 && current_ms >= skip_enabled_ms) {
        handle_input(current_ms);
    }

    double reveal_end = reveal_end_ms();
    const bool hist_open = zdon_hist_open[1] || zdon_hist_open[2]; // Z-DON: no timeout while it is read
    const double idle_from = std::max(reveal_end + kWaitEffectEndMs, zdon_play_all_ms); // Z-DON: play-all restarts it
    if (reveal_end > 0 && !exiting() && !hist_open && current_ms >= idle_from + kAutoNextSceneMs) {
        begin_exit();
    }
}

std::optional<Screens> ResultScreen::update() {
    Screen::update();
    double current_time = get_current_ms();
    allnet_indicator.update(current_time);
    if (fade_in.has_value()) {
        fade_in->update(current_time);
    }
    if (player_1.has_value()) {
        player_1->update(current_time, fade_in.has_value() && fade_in->is_finished(), skipped_time > 0);
    }

    update_input_and_timeout(current_time);

    if (retrying) {
        if (fade_in->is_finished()) {
            if (!retry_message) {
                retry_message.emplace();
            }
            retry_message->update(current_time);
            if (retry_message->is_finished()) {
                return on_screen_end(Screens::RETRY_GAME);
            }
        }
    } else if (fade_out) {
        fade_out->update(current_time);
        if (fade_out->is_finished) {
            if (global_data.config->limits.song_limit > 0 &&
                global_data.config->limits.song_limit == global_data.songs_played) {
                global_data.songs_played = 0;
                return on_screen_end(Screens::GAME_OVER);
            }
            global_data.returned_from_result = true;
            return on_screen_end(Screens::SONG_SELECT);
        }
    }
    return std::nullopt;
}

// Z-DON: "this score is not saved (auto, constant speed)" under each player's board (GameScreen::save_score)
void ResultScreen::draw_unsaved_note() const {
    if (zdon_record::active()) {
        return; // not in the video
    }
    const bool two = zdon_two_boards();
    for (int pn = 1; pn <= 2; ++pn) {
        if (!two && pn != (global_data.player_num == PlayerNum::P2 ? 2 : 1)) {
            continue;
        }
        const auto& why = global_data.session_data[pn].zdon_unsaved;
        if (why.empty()) {
            continue;
        }
        std::string list;
        for (const std::string& r : why) {
            list += (list.empty() ? "" : zhud::text("list_sep")) + zhud::text("reason_" + r);
        }
        OutlinedText& t = zhud::label(zhud::text("unsaved", list), 24, Color{255, 226, 120, 255});
        const float s = tex.screen_scale;
        const float cx = two ? tex.screen_width * (pn == 1 ? 0.25f : 0.75f) : tex.screen_width / 2.0f;
        const float w = t.width + 32 * s;
        const float h = t.height + 12 * s;
        const float y = tex.screen_height - h - 64 * s;
        zhud::panel(cx - w / 2, y, w, h, 0.8f, fade(Color{255, 226, 120, 255}, 0.7f));
        t.draw({.x = cx - t.width / 2, .y = y + 6 * s});
    }
}

// Z-DON: a kat opens / closes the player's hit timing panel (only with recorded hits)
bool ResultScreen::zdon_hist_input() {
    if (global_data.zdon_ai.active && global_data.zdon_ai.finished) {
        // the skin's cut-in pages (Green's rank up / reward: a kat turns the page, a don ends them)
        if (zdon_ai_pages_open()) {
            if (is_l_kat_pressed(PlayerNum::P1) || is_r_kat_pressed(PlayerNum::P1)) {
                zdon_ai_page_next();
                audio.play_sound("kat", VolumePreset::SOUND);
            } else if (is_l_don_pressed(PlayerNum::P1) || is_r_don_pressed(PlayerNum::P1)) {
                zdon_ai_pages_finish();
                audio.play_sound("don", VolumePreset::SOUND);
            }
            return true;
        }
        // AI battle (AiResultSectionMain SwitchSection): kat switches the VS panel's simple / detail view
        if (is_l_kat_pressed(PlayerNum::P1) || is_r_kat_pressed(PlayerNum::P1)) {
            global_data.zdon_ai.detail = !global_data.zdon_ai.detail;
            audio.play_sound("kat", VolumePreset::SOUND);
        }
        return false;
    }
    for (int pn = 1; pn <= 2; ++pn) {
        if (global_data.session_data[pn].result_data.hit_offsets.empty()) {
            continue;
        }
        const PlayerNum who = zdon_two_boards() ? static_cast<PlayerNum>(pn) : global_data.player_num;
        if (!zdon_two_boards() && pn != (global_data.player_num == PlayerNum::P2 ? 2 : 1)) {
            continue;
        }
        if (is_l_kat_pressed(who) || is_r_kat_pressed(who)) {
            zdon_hist_open[pn] = !zdon_hist_open[pn];
            audio.play_sound("kat", VolumePreset::SOUND);
        }
    }
    return false;
}

// the hits' offsets in bins across the OK window (gold inside GOOD), mean, SD, early / late; or a small hint
void ResultScreen::zdon_draw_hist() const {
    if (global_data.zdon_ai.active && global_data.zdon_ai.finished) {
        return; // AI battle: kat switches the VS panel's view instead (zdon_hist_input)
    }
    const bool two = zdon_two_boards();
    const float s = tex.screen_scale;
    bool hint = false;
    for (int pn = 1; pn <= 2; ++pn) {
        if (!two && pn != (global_data.player_num == PlayerNum::P2 ? 2 : 1)) {
            continue;
        }
        const SessionData& sd = global_data.session_data[pn];
        const std::vector<float>& off = sd.result_data.hit_offsets;
        if (off.empty()) {
            continue;
        }
        if (!zdon_hist_open[pn]) {
            hint = true;
            continue;
        }
        const bool easy = sd.selected_difficulty <= (int)Difficulty::NORMAL;
        const float good = easy ? Timing::GOOD_EASY : Timing::GOOD;
        const float ok = easy ? Timing::OK_EASY : Timing::OK;
        const float bin_ms = std::max(2.0f, std::round(ok / 25.0f));
        const int half = static_cast<int>(std::ceil(ok / bin_ms));
        std::vector<int> bins(2 * half + 1, 0);
        double sum = 0, sum2 = 0;
        int early = 0, late = 0;
        for (float o : off) {
            sum += o;
            sum2 += (double)o * o;
            early += o < 0;
            late += o > 0;
            const int b = std::clamp(static_cast<int>(std::lround(o / bin_ms)), -half, half);
            bins[b + half]++;
        }
        const double mean = sum / off.size();
        const double sd_ms = std::sqrt(std::max(0.0, sum2 / off.size() - mean * mean));
        const int peak = std::max(1, *std::ranges::max_element(bins));

        const float w = (two ? 600 : 760) * s;
        const float graph_h = 170 * s;
        OutlinedText& title = zhud::label(zhud::text("hist_title") + "（" + std::to_string(off.size()) + "）", 24,
                                          Color{255, 226, 120, 255});
        auto fmt1 = [](double v) {
            const long long t = std::llround(v * 10);
            return std::string(t < 0   ? "-"
                               : t > 0 ? "+"
                                       : "") +
                   std::to_string(std::llabs(t) / 10) + "." + std::to_string(std::llabs(t) % 10);
        };
        OutlinedText& stats =
            zhud::label(zhud::text("hist_stats", fmt1(mean), fmt1(sd_ms).substr(fmt1(sd_ms)[0] == '+' ? 1 : 0)), 20);
        OutlinedText& counts = zhud::label(zhud::text("hist_counts", std::to_string(early), std::to_string(late)), 20,
                                           Color{200, 206, 220, 255});
        const float h = 14 * s + title.height + 6 * s + graph_h + 26 * s + stats.height + 4 * s + 12 * s;
        const float cx = two ? tex.screen_width * (pn == 1 ? 0.25f : 0.75f) : tex.screen_width / 2.0f;
        float x = cx - w / 2;
        float y = (tex.screen_height - h) / 2;
        if (auto cfg = tex.skin_config_by_name.find("zdon_hit_histogram");
            !two && cfg != tex.skin_config_by_name.end()) {
            x = cfg->second.x;
            y = cfg->second.y;
        }
        zhud::panel(x, y, w, h, 0.9f, fade(Color{255, 226, 120, 255}, 0.6f));
        title.draw({.x = x + 20 * s, .y = y + 12 * s});
        counts.draw({.x = x + w - 20 * s - counts.width, .y = y + 14 * s});

        const float gx = x + 24 * s;
        const float gw = w - 48 * s;
        const float gy = y + 14 * s + title.height + 6 * s;
        const float bw = gw / bins.size();
        const float ms_to_x = gw / (2 * half * bin_ms + bin_ms);
        const float zero_x = gx + gw / 2;
        draw_rectangle(Rect{zero_x - good * ms_to_x, gy, 2 * good * ms_to_x, graph_h}, Color{255, 226, 120, 28});
        for (size_t b = 0; b < bins.size(); ++b) {
            if (bins[b] == 0) {
                continue;
            }
            const float center = (static_cast<int>(b) - half) * bin_ms;
            const float bh = graph_h * bins[b] / peak;
            const Color c = std::abs(center) <= good ? Color{255, 196, 64, 255} : Color{190, 196, 214, 255};
            draw_rectangle(Rect{gx + b * bw + 1 * s, gy + graph_h - bh, std::max(1.0f, bw - 2 * s), bh}, c);
        }
        draw_rectangle(Rect{zero_x - 1 * s, gy, 2 * s, graph_h}, fade(Color::WHITE, 0.8f));
        const float mean_x = zero_x + static_cast<float>(mean) * ms_to_x;
        draw_rectangle(Rect{mean_x - 1 * s, gy, 2 * s, graph_h}, Color{120, 196, 255, 220});
        draw_rectangle(Rect{gx, gy + graph_h, gw, 1 * s}, fade(Color::WHITE, 0.5f));
        const int ok_i = static_cast<int>(std::lround(ok));
        const int good_i = static_cast<int>(std::lround(good));
        const std::pair<float, std::string> ticks[] = {{-ok, "-" + std::to_string(ok_i)},
                                                       {-good, "-" + std::to_string(good_i)},
                                                       {0.0f, "0"},
                                                       {good, "+" + std::to_string(good_i)},
                                                       {ok, "+" + std::to_string(ok_i)}};
        for (const auto& [ms, label] : ticks) {
            OutlinedText& tk = zhud::label(label, 16, Color{200, 206, 220, 255});
            const float tx = std::clamp(zero_x + ms * ms_to_x - tk.width / 2, gx, gx + gw - tk.width);
            tk.draw({.x = tx, .y = gy + graph_h + 4 * s});
        }
        stats.draw({.x = x + (w - stats.width) / 2, .y = gy + graph_h + 26 * s});
    }
    if (hint) {
        OutlinedText& t = zhud::label(zhud::text("hist_hint"), 18, Color{200, 206, 220, 255});
        const float w = t.width + 20 * s;
        const float h = t.height + 6 * s;
        const float x = tex.screen_width - w - 12 * s;
        const float y = tex.screen_height - h - 12 * s;
        zhud::panel(x, y, w, h, 0.6f);
        t.draw({.x = x + 10 * s, .y = y + 3 * s});
    }
}

void ResultScreen::draw_overlay() {
    draw_unsaved_note();
    zdon_draw_hist();
    if (fade_in.has_value()) {
        fade_in->draw();
    }
    if (fade_out) {
        draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::BLACK, fade_out->attribute));
    }
    if (retry_message) {
        retry_message->draw();
    }
    coin_overlay.draw();
    allnet_indicator.draw();
}

void ResultScreen::draw_song_info() {
    if (player_1.has_value()) {
        player_1->draw_song_info(song_title, song_subtitle, song_number);
    }
}

void ResultScreen::draw() {
    if (background.has_value()) {
        background->draw();
    }
    if (loading_graphic.has_value()) {
        Rect src = {0, 0, static_cast<float>(loading_graphic.value().width),
                    static_cast<float>(loading_graphic.value().height)};
        Rect dst = {0, 0, static_cast<float>(tex.screen_width), static_cast<float>(tex.screen_height)};
        draw_texture_pro(loading_graphic.value(), src, dst, {0, 0}, 0, Color::WHITE);
    }
    draw_song_info();
    if (player_1.has_value()) {
        player_1->draw();
    }
    draw_overlay();
}
