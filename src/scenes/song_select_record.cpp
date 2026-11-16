#include "song_select_record.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "../libs/audio/audio.h"
#include "../libs/input/input.h"
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
    difficulty = static_cast<int>(player->selected_difficulty);
    title = box->text_name;
    cursor = 0;
    available.clear();
    ticked = {};
    for (int c = 0; c < static_cast<int>(ticked.size()); c++) { // every course of the song starts ticked
        if (box->course_info(c).has_course) {
            available.push_back(c);
            ticked[c] = true;
        }
    }
    if (difficulty >= 0 && difficulty < static_cast<int>(ticked.size())) {
        ticked[difficulty] = true;
    }
    modal = Modal::Choose;
}

std::vector<int> RecordSongSelectScreen::lanes() const {
    std::vector<int> v = {difficulty};
    for (int c = static_cast<int>(ticked.size()) - 1; c >= 0; c--) {
        if (ticked[c] && c != difficulty) {
            v.push_back(c);
        }
    }
    return v;
}

void RecordSongSelectScreen::close_modal() {
    modal = Modal::None;
    zdon_record::export_forget();
    navigator.exit_diff_select();
    state = SongSelectState::BROWSING;
    player->reset_selection();
}

bool RecordSongSelectScreen::start(const Job& job) {
    current = job;
    out = zdon_record::export_path(job.title, job.courses, job.autoplay);
    progress = 0.0;
    run_started = std::chrono::steady_clock::now();
    running = zdon_record::export_start(job.song, job.courses, job.autoplay, out);
    return running;
}

void RecordSongSelectScreen::poll() {
    if (!running) {
        return;
    }
    const zdon_record::ExportStatus st = zdon_record::export_poll();
    progress = st.progress;
    if (st.state != zdon_record::ExportState::Done && st.state != zdon_record::ExportState::Failed) {
        return;
    }
    const bool saved = st.state == zdon_record::ExportState::Done;
    (saved ? batch_saved : batch_failed)++;
    running = false;
    zdon_record::export_forget();
    while (!queue.empty()) { // the next queued recording
        const Job next = queue.front();
        queue.pop_front();
        if (start(next)) {
            return;
        }
        batch_failed++;
    }
    if (modal == Modal::Running) { // the window waited for it: the last one's file (or the failure)
        modal = saved ? Modal::Done : Modal::Failed;
        audio.dev_set_master_volume(normal_volume());
    } else {
        show_toast(zhud::text("rec_batch_done", std::to_string(batch_saved), std::to_string(batch_failed)), 6.0);
    }
}

std::string RecordSongSelectScreen::eta() const {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started).count();
    if (progress < 0.02 || elapsed < 2.0) {
        return "";
    }
    const int left = static_cast<int>(std::lround(elapsed * (1.0 - progress) / progress));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", left / 60, left % 60);
    return zhud::text("rec_eta", buf);
}

void RecordSongSelectScreen::show_toast(const std::string& s, double seconds) {
    toast = s;
    toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
}

void RecordSongSelectScreen::update_modal() {
    const int back = global_data.config->keys.back_key;
    const bool l_kat = is_l_kat_pressed();
    const bool r_kat = is_r_kat_pressed();
    const bool kat = l_kat || r_kat;
    const bool don = is_l_don_pressed() || is_r_don_pressed();
    const bool cancel = check_key_pressed(back);
    switch (modal) {
    case Modal::Choose:
        if (cancel) {
            close_modal();
        } else if (kat) { // left / right through auto, no hits and the courses
            const int n = 2 + static_cast<int>(available.size());
            cursor = (cursor + (r_kat ? 1 : n - 1)) % n;
            audio.play_sound("kat", VolumePreset::HITSOUND);
        } else if (don && cursor >= 2) { // tick a course to record with the decided one (that one stays)
            const int c = available[cursor - 2];
            if (c != difficulty) {
                ticked[c] = !ticked[c];
            }
            audio.play_sound("don", VolumePreset::HITSOUND);
        } else if (don) {
            audio.play_sound("don", VolumePreset::HITSOUND);
            const Job job{song, lanes(), cursor == 0, title};
            if (running) { // after the ones before it
                queue.push_back(job);
                show_toast(zhud::text("rec_queued", std::to_string(queue.size())), 2.5);
                close_modal();
            } else if (start(job)) {
                batch_saved = batch_failed = 0;
                modal = Modal::Running;
                audio.dev_set_master_volume(0.0f); // the preview, while the window waits
            } else {
                modal = Modal::Failed;
            }
        }
        break;
    case Modal::Running:
        if (cancel) { // this one and the queued ones
            queue.clear();
            zdon_record::export_cancel();
            running = false;
            audio.dev_set_master_volume(normal_volume());
            close_modal();
        } else if (kat) { // the song select comes back; the recording goes on (and more can be queued)
            audio.dev_set_master_volume(normal_volume());
            close_modal();
        }
        break;
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
    if (running) {
        queue.clear();
        zdon_record::export_cancel();
        running = false;
    }
    audio.dev_set_master_volume(normal_volume());
    modal = Modal::None;
    return SongSelectScreen::on_screen_end(next_screen);
}

std::optional<Screens> RecordSongSelectScreen::update() {
    poll();
    if (modal != Modal::None) {
        update_modal();
        clear_input_buffers(); // the song select behind the window gets none of it
    } else if (running && state == SongSelectState::BROWSING && check_key_pressed(global_data.config->keys.back_key)) {
        modal = Modal::Running; // Back shows the recording's window again instead of leaving (which stops it)
        audio.dev_set_master_volume(0.0f);
        clear_input_buffers();
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
    if (running && modal != Modal::Running) {
        draw_background_panel();
    }
    if (modal != Modal::None) {
        draw_modal();
    }
    if (!toast.empty() && std::chrono::steady_clock::now() < toast_until) {
        OutlinedText& t = zhud::label(toast, 26, Color{255, 226, 120, 255});
        const float pw = t.width + 48 * u, ph = t.height + 24 * u;
        const float px = tex.screen_width / 2.0f - pw / 2, py = tex.screen_height * 0.8f;
        zhud::panel(px, py, pw, ph, 0.9f, Color{255, 226, 120, 200});
        t.draw({.x = px + 24 * u, .y = py + 12 * u});
    }
}

// the recording running behind the song select: a panel in the top left corner (skins keep the timer and the
// online state on the right)
void RecordSongSelectScreen::draw_background_panel() {
    const float u = tex.screen_width / 1920.0f;
    const Color gold{255, 226, 120, 255};
    std::string head = zhud::text("rec_bg", std::to_string(static_cast<int>(progress * 100.0 + 0.5)), eta());
    while (!head.empty() && head.back() == ' ') { // no time left shown yet: a trailing space drew no text at all
        head.pop_back();
    }
    const std::string sub =
        queue.empty() ? zhud::text("rec_bg_keys")
                      : zhud::text("rec_queue_n", std::to_string(queue.size())) + "   " + zhud::text("rec_bg_keys");
    OutlinedText& a = zhud::label(head, 24, gold);
    OutlinedText& b = zhud::label(sub, 18, Color{200, 200, 200, 255});
    const float pad = 14 * u, bar = 8 * u;
    const float pw = std::max({a.width, b.width, 360 * u}) + 2 * pad;
    const float ph = a.height + b.height + bar + 4 * pad;
    const float px = 16 * u, py = 16 * u;
    zhud::panel(px, py, pw, ph, 0.85f, Color{255, 226, 120, 160});
    a.draw({.x = px + pad, .y = py + pad});
    const float by = py + 2 * pad + a.height;
    draw_rectangle(static_cast<int>(px + pad), static_cast<int>(by), static_cast<int>(pw - 2 * pad),
                   static_cast<int>(bar), Color{60, 60, 60, 255});
    draw_rectangle(static_cast<int>(px + pad), static_cast<int>(by), static_cast<int>((pw - 2 * pad) * progress),
                   static_cast<int>(bar), gold);
    b.draw({.x = px + pad, .y = by + bar + pad});
}

void RecordSongSelectScreen::draw_modal() {
    const float u = tex.screen_width / 1920.0f;
    const float cx = tex.screen_width / 2.0f;
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::BLACK, 0.55f));
    const float w = 900 * u, h = (modal == Modal::Choose ? 470 : 330) * u;
    const float x = cx - w / 2, y = tex.screen_height / 2.0f - h / 2;
    zhud::panel(x, y, w, h, 0.92f, Color{255, 226, 120, 200});

    auto centered = [&](const std::string& s, int size, float ty, Color c = Color::WHITE) {
        OutlinedText& t = zhud::label(s, size, c);
        t.draw({.x = cx - t.width / 2, .y = ty});
        return t.height;
    };
    // the window's song: the one being chosen, or the one recording (or last recorded)
    const bool choosing = modal == Modal::Choose;
    std::string courses;
    for (const int c : choosing ? lanes() : current.courses) {
        courses += (courses.empty() ? "" : " + ") + zhud::course_name(c);
    }
    centered((choosing ? title : current.title) + "  " + courses, 30, y + 24 * u);

    const Color gold{255, 226, 120, 255};
    switch (modal) {
    case Modal::Choose: {
        centered(zhud::text("rec_choose"), 26, y + 80 * u, Color{200, 200, 200, 255});
        const char* keys[2] = {"rec_auto", "rec_nohit"};
        const float bw = 300 * u, bh = 70 * u, gap = 40 * u;
        for (int i = 0; i < 2; i++) {
            const float bx = cx - bw - gap / 2 + i * (bw + gap);
            const float by = y + 130 * u;
            const bool sel = cursor == i;
            zhud::panel(bx, by, bw, bh, sel ? 0.95f : 0.6f, sel ? gold : Color{255, 255, 255, 90});
            OutlinedText& t = zhud::label(zhud::text(keys[i]), 32, sel ? gold : Color{170, 170, 170, 255});
            t.draw({.x = bx + bw / 2 - t.width / 2, .y = by + bh / 2 - t.height / 2});
        }
        // the courses to record together, one lane each (the decided one always)
        centered(zhud::text("rec_courses"), 22, y + 225 * u, Color{200, 200, 200, 255});
        const float cw = 150 * u, ch = 56 * u, cgap = 16 * u;
        const float row = available.size() * cw + (available.size() - 1) * cgap;
        for (size_t i = 0; i < available.size(); i++) {
            const int c = available[i];
            const float bx = cx - row / 2 + i * (cw + cgap);
            const float by = y + 265 * u;
            const bool sel = cursor == 2 + static_cast<int>(i);
            const bool on = ticked[c];
            // ticked: a green fill (no tick glyph: not every skin's font has one)
            const Color edge = sel ? gold : on ? Color{140, 220, 140, 220} : Color{255, 255, 255, 90};
            zhud::panel(bx, by, cw, ch, sel ? 0.95f : 0.6f, edge);
            if (on) {
                draw_rectangle(static_cast<int>(bx + 4 * u), static_cast<int>(by + 4 * u), static_cast<int>(cw - 8 * u),
                               static_cast<int>(ch - 8 * u), Color{70, 150, 80, 200});
            }
            OutlinedText& t = zhud::label(zhud::course_name(c), 24, on ? Color::WHITE : Color{150, 150, 150, 255});
            t.draw({.x = bx + cw / 2 - t.width / 2, .y = by + ch / 2 - t.height / 2});
        }
        centered(zhud::text(running ? "rec_keys_queue" : "rec_keys_choose"), 22, y + h - 50 * u,
                 Color{200, 200, 200, 255});
        break;
    }
    case Modal::Running: {
        const int pct = static_cast<int>(progress * 100.0 + 0.5);
        centered(zhud::text("rec_running", std::to_string(pct)), 32, y + 90 * u, gold);
        const float bw = 700 * u, bh = 24 * u;
        const float bx = cx - bw / 2, by = y + 150 * u;
        draw_rectangle(static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bw), static_cast<int>(bh),
                       Color{60, 60, 60, 255});
        draw_rectangle(static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bw * progress),
                       static_cast<int>(bh), gold);
        draw_rectangle_lines(static_cast<int>(bx), static_cast<int>(by), static_cast<int>(bw), static_cast<int>(bh),
                             Color{255, 255, 255, 160});
        std::string info = eta();
        if (!queue.empty()) {
            info += (info.empty() ? "" : "   ") + zhud::text("rec_queue_n", std::to_string(queue.size()));
        }
        if (!info.empty()) {
            centered(info, 22, y + 192 * u, Color{200, 200, 200, 255});
        }
        centered(zhud::text("rec_running_keys"), 20, y + h - 50 * u, Color{200, 200, 200, 255});
        break;
    }
    case Modal::Done: {
        const int batch = batch_saved + batch_failed;
        centered(batch > 1 ? zhud::text("rec_batch_done", std::to_string(batch_saved), std::to_string(batch_failed))
                           : zhud::text("rec_done"),
                 32, y + 90 * u, gold);
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
