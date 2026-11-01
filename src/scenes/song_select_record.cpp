#include "song_select_record.h"

#include <chrono>
#include <thread>

#include "../libs/audio.h"
#include "../libs/input.h"
#include "../libs/test_mode.h"
#include "../libs/zdon_record.h"
#include "../objects/global/zdon_hud.h"

namespace {
    // the master volume outside a recording (YATAIDON_TEST runs play at 1 %)
    float normal_volume() {
        return zdon_test_mode() ? 0.01f : 1.0f;
    }
} // namespace

void RecordSongSelectScreen::select_song(SongBox* box) {
    if (modal != Modal::None) {
        return; // the course stays decided behind the window
    }
    song = box->path;
    difficulty = (int)player->selected_difficulty;
    title = box->text_name;
    choice = 0;
    modal = Modal::Choose;
}

void RecordSongSelectScreen::close_modal() {
    modal = Modal::None;
    zdon_record::export_forget();
    navigator.exit_diff_select();
    state = SongSelectState::BROWSING;
    player->reset_selection();
}

void RecordSongSelectScreen::update_modal() {
    const int back = global_data.config->keys.back_key;
    const bool kat = is_l_kat_pressed() || is_r_kat_pressed();
    const bool don = is_l_don_pressed() || is_r_don_pressed();
    const bool cancel = check_key_pressed(back);
    switch (modal) {
    case Modal::Choose:
        if (cancel) {
            close_modal();
        } else if (kat) {
            choice ^= 1;
            audio.play_sound("kat", VolumePreset::SOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::SOUND);
            out = zdon_record::export_path(title, difficulty, choice == 0);
            progress = 0.0;
            if (zdon_record::export_start(song, difficulty, choice == 0, out)) {
                modal = Modal::Running;
                audio.dev_set_master_volume(0.0f); // the preview, while the window waits
            } else {
                modal = Modal::Failed;
            }
        }
        break;
    case Modal::Running: {
        const zdon_record::ExportStatus st = zdon_record::export_poll();
        progress = st.progress;
        if (st.state == zdon_record::ExportState::Done || st.state == zdon_record::ExportState::Failed) {
            modal = st.state == zdon_record::ExportState::Done ? Modal::Done : Modal::Failed;
            audio.dev_set_master_volume(normal_volume());
        } else if (cancel) {
            zdon_record::export_cancel();
            audio.dev_set_master_volume(normal_volume());
            close_modal();
        }
        break;
    }
    case Modal::Done:
        if (don) {
            zdon_record::show_in_folder(out);
            close_modal();
        } else if (kat || cancel) {
            close_modal();
        }
        break;
    case Modal::Failed:
        if (don || kat || cancel) {
            close_modal();
        }
        break;
    case Modal::None:
        break;
    }
}

Screens RecordSongSelectScreen::on_screen_end(Screens next_screen) {
    if (modal == Modal::Running) {
        zdon_record::export_cancel();
        audio.dev_set_master_volume(normal_volume());
    }
    modal = Modal::None;
    return SongSelectScreen::on_screen_end(next_screen);
}

std::optional<Screens> RecordSongSelectScreen::update() {
    if (modal != Modal::None) {
        update_modal();
        clear_input_buffers(); // the song select behind the window gets none of it
    }
    auto next = SongSelectScreen::update();
    if (modal == Modal::Running) {
        // the recording renders as fast as it can: this window needs a few frames a second, not hundreds
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return next;
}

void RecordSongSelectScreen::draw() {
    SongSelectScreen::draw();
    const float u = tex.screen_width / 1920.0f;
    OutlinedText& tag = zhud::label(zhud::text("rec_mode"), 26, Color{255, 140, 140, 255});
    const float tw = tag.width + 28 * u;
    const float th = tag.height + 10 * u;
    zhud::panel(tex.screen_width / 2.0f - tw / 2, 10 * u, tw, th, 0.8f, Color{255, 140, 140, 200});
    tag.draw({.x = tex.screen_width / 2.0f - tag.width / 2, .y = 10 * u + 5 * u});
    if (modal != Modal::None) {
        draw_modal();
    }
}

void RecordSongSelectScreen::draw_modal() {
    const float u = tex.screen_width / 1920.0f;
    const float cx = tex.screen_width / 2.0f;
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::BLACK, 0.55f));
    const float w = 900 * u, h = 330 * u;
    const float x = cx - w / 2, y = tex.screen_height / 2.0f - h / 2;
    zhud::panel(x, y, w, h, 0.92f, Color{255, 226, 120, 200});

    auto centered = [&](const std::string& s, int size, float ty, Color c = Color::WHITE) {
        OutlinedText& t = zhud::label(s, size, c);
        t.draw({.x = cx - t.width / 2, .y = ty});
        return t.height;
    };
    centered(title + "  " + zhud::course_name(difficulty), 30, y + 24 * u);

    const Color gold{255, 226, 120, 255};
    switch (modal) {
    case Modal::Choose: {
        centered(zhud::text("rec_choose"), 26, y + 80 * u, Color{200, 200, 200, 255});
        const char* keys[2] = {"rec_auto", "rec_nohit"};
        const float bw = 300 * u, bh = 70 * u, gap = 40 * u;
        for (int i = 0; i < 2; i++) {
            const float bx = cx - bw - gap / 2 + i * (bw + gap);
            const float by = y + 130 * u;
            const bool sel = choice == i;
            zhud::panel(bx, by, bw, bh, sel ? 0.95f : 0.6f, sel ? gold : Color{255, 255, 255, 90});
            OutlinedText& t = zhud::label(zhud::text(keys[i]), 32, sel ? gold : Color{170, 170, 170, 255});
            t.draw({.x = bx + bw / 2 - t.width / 2, .y = by + bh / 2 - t.height / 2});
        }
        centered(zhud::text("rec_keys_choose"), 22, y + h - 50 * u, Color{200, 200, 200, 255});
        break;
    }
    case Modal::Running: {
        const int pct = (int)(progress * 100.0 + 0.5);
        centered(zhud::text("rec_running", std::to_string(pct)), 32, y + 100 * u, gold);
        const float bw = 700 * u, bh = 24 * u;
        const float bx = cx - bw / 2, by = y + 170 * u;
        draw_rectangle((int)bx, (int)by, (int)bw, (int)bh, Color{60, 60, 60, 255});
        draw_rectangle((int)bx, (int)by, (int)(bw * progress), (int)bh, gold);
        draw_rectangle_lines((int)bx, (int)by, (int)bw, (int)bh, Color{255, 255, 255, 160});
        centered(zhud::text("rec_running_keys"), 22, y + h - 50 * u, Color{200, 200, 200, 255});
        break;
    }
    case Modal::Done: {
        centered(zhud::text("rec_done"), 32, y + 90 * u, gold);
        const std::u8string name = out.filename().u8string();
        centered(std::string(name.begin(), name.end()), 24, y + 150 * u);
        const std::u8string dir = out.parent_path().u8string();
        centered(std::string(dir.begin(), dir.end()), 20, y + 190 * u, Color{170, 170, 170, 255});
        centered(zhud::text("rec_done_keys"), 22, y + h - 50 * u, Color{200, 200, 200, 255});
        break;
    }
    case Modal::Failed:
        centered(zhud::text("rec_failed"), 30, y + 120 * u, Color{255, 140, 140, 255});
        centered(zhud::text("rec_failed_keys"), 22, y + h - 50 * u, Color{200, 200, 200, 255});
        break;
    case Modal::None:
        break;
    }
}
