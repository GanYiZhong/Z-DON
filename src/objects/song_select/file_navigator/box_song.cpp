#include "box_song.h"

#include <chrono>
#include <cstdlib>
#include <thread>

#include "../../../libs/audio/audio.h"
#include "../../../libs/fixed_step.h"
#include "../../../libs/gfx/text.h"
#include "../../../libs/latest_worker.h"
#include "../../../libs/network/network.h"
#include "../../../platform/platform.h"
#include "color_utils.h"
#include "navigator.h"

namespace {
    double bgm_resume_at = 0.0; // 0 = nothing pending
    int preview_holders = 0;    // focused song boxes that own the bgm slot

    // Z-DON dev hook ZDON_PREVIEW_LOG=1: logs how long a preview took to open and to start playing
    struct PreviewTiming {
        double started_ms = 0; // when the stream was opened (0 = not timing)
        double open_ms = 0;
        float position = 0;
    } preview_timing;
    bool preview_log() {
        static const bool on = std::getenv("ZDON_PREVIEW_LOG") != nullptr;
        return on;
    }
} // namespace

std::string SongBox::bgm_sound = "bgm";

void SongBox::reset_bgm_slot() {
    bgm_resume_at = 0.0;
    preview_holders = 0;
}

void SongBox::service_bgm_resume(double current_ms) {
    if (bgm_resume_at <= 0.0) {
        return;
    }
    if (preview_holders > 0) {
        bgm_resume_at = 0.0;
        return;
    } // a song took it
    if (current_ms < bgm_resume_at) {
        return;
    }
    bgm_resume_at = 0.0;
    audio.play_sound(bgm_sound, VolumePreset::MUSIC);
}

SongBox::SongBox(const fs::path& path, const BoxDef& box_def, SongParser parser) : BaseBox(path, box_def) {
    song_genre_index = genre_index;
    song_genre_label = box_def.genre_label;

    parser.get_metadata();
    if (box_def.collab) {
        auto colors = DEFAULT_COLORS.find(get_genre_index(parser.metadata.genre));
        if (colors != DEFAULT_COLORS.end() && colors->second[1].has_value()) {
            fore_color = colors->second[1].value();
            text_color = Color::WHITE;
        }
    }
    auto& titles = parser.metadata.title;
    const std::string& lang = global_data.config->general.language;
    text_name = localized_text(titles, lang, LocalizedTextFallback::ENGLISH_OR_FIRST);

    auto& subtitles = parser.metadata.subtitle;
    text_subtitle = localized_text(subtitles, lang, LocalizedTextFallback::ENGLISH_OR_FIRST);

    audio.queue_song_loudness(parser.metadata.wave);
    this->parser = std::move(parser);

    is_favorite = false;
    diff_fade_in = static_cast<FadeAnimation*>(tex.get_animation(12));
    refresh_scores();
}

void SongBox::refresh_scores() {
    for (auto& r : recent_plays_cache) {
        r.reset();
    }
    hashes = scores_manager.get_hashes(path);
#ifdef SUPPORT_FUMEN
    bool cheap_hash = !std::holds_alternative<FumenParser>(parser.impl);
#else
    bool cheap_hash = true;
#endif
    for (const auto& [course, course_data] : parser.metadata.course_data) {
        if (course < 0 || course >= static_cast<int>(hashes.size())) {
            continue;
        }
        if (hashes[course].empty() && cheap_hash) {
            hashes[course] = parser.get_diff_hash(course);
        }
    }
    for (int i = 0; i < 5; i++) {
        const std::string& p1_id = global_data.config->network.access_code_1;
        const std::string& p2_id = global_data.config->network.access_code_2;
        scores[i] = scores_manager.get_score(hashes[i], i, p1_id);
        scores_p2[i] = navigator.is_2p ? scores_manager.get_score(hashes[i], i, p2_id) : std::nullopt;
    }
    score_history.reset();
}

std::string SongBox::hash_for(int difficulty) {
    if (difficulty < 0 || difficulty >= static_cast<int>(hashes.size())) {
        return "";
    }
    if (hashes[difficulty].empty()) {
        hashes[difficulty] = parser.get_diff_hash(difficulty);
        if (!hashes[difficulty].empty()) {
            scores_manager.add_path_binding(path, hashes);
        }
    }
    return hashes[difficulty];
}

void SongBox::reset() {
    BaseBox::reset();
    diff_fade_in = static_cast<FadeAnimation*>(tex.get_animation(12));
    if (audio.is_music_stream_valid("preview")) {
        audio.unload_music_stream("preview");
    }
    music_playing = false;
    stop_preview_load();
    score_history.reset();
    box_opened_at = 0.0;
}

std::vector<Difficulty> SongBox::get_diffs() {
    std::vector<Difficulty> diffs;
    diffs.reserve(parser.metadata.course_data.size());
    for (const auto& [diff, level] : parser.metadata.course_data) {
        diffs.push_back(Difficulty(diff));
    }
    return diffs;
}

void SongBox::preregister_text() {
    BaseBox::preregister_text();
    float base_sub_font = static_cast<float>(tex.get_skin_config(SC::SONG_SELECT_BOX_SUBTITLE)->font_size);
    float sub_font = utf8_char_count(text_subtitle) >= 30 ? base_sub_font - 10.0f * tex.screen_scale : base_sub_font;
    font_manager.register_text(text_subtitle, static_cast<int>(sub_font));
    font_manager.register_text("BPM\n0123456789", tex.get_skin_config(SC::SONG_SELECT_BOX_BPM)->font_size);
}

void SongBox::load_text() {
    BaseBox::load_text();
    bpm_text = make_unique<OutlinedText>("BPM\n" + std::to_string(static_cast<int>(parser.metadata.bpm)),
                                         tex.get_skin_config(SC::SONG_SELECT_BOX_BPM)->font_size, Color::WHITE,
                                         Color::BLACK, false);
    if (exists(parser.metadata.preimage)) {
        if (preimage.has_value()) {
            unload_texture(preimage.value());
        }
        preimage = load_texture_mipmapped(parser.metadata.preimage.string().c_str());
    }
    text_loaded = true;
}

void SongBox::unload_text() {
    BaseBox::unload_text();
    bpm_text.reset();
    horizontal_subtitle_cache.reset();
    horizontal_subtitle_large_cache.reset();
    if (preimage.has_value()) {
        unload_texture(preimage.value());
        preimage.reset();
    }
    score_history.reset();
}

void SongBox::update(double current_time) {
    BaseBox::update(current_time);
    if (popular_rank < 0) {
        int best = 0;
        for (const auto& hash : hashes) {
            int rank = hash.empty() ? 0 : network.popular_rank(hash);
            if (rank < 0) {
                best = -1;
                break;
            }
            if (rank > 0 && (best == 0 || rank < best)) {
                best = rank;
            }
        }
        popular_rank = best;
    }
    // Z-DON (merge 0ed42ec): no upstream early return for closed boxes; the score_history load below also runs
    // for closed boxes here
    if (!diff_fade_in->settled(current_time)) { // one animation shared by every song box
        diff_fade_in->update(current_time);
    }

    // update() runs every frame for every box of the list: the audio file is looked at once,
    // when this box is first opened, instead of building paths / stat-ing it every frame
    if (yellow_box_active && wave_kind == WaveKind::UNKNOWN) {
        audio.queue_song_loudness(parser.metadata.wave, true);
        const auto wave_ext = parser.metadata.wave.extension();
        const bool bank = wave_ext == ".nus3bank" || wave_ext == ".nub";
        std::error_code ec;
        const bool exists = !parser.metadata.wave.empty() && fs::exists(parser.metadata.wave, ec);
        wave_kind = !exists ? WaveKind::MISSING : bank ? WaveKind::BANK : WaveKind::STREAM;
    }
    const bool is_bank = wave_kind == WaveKind::BANK;
    const bool is_stream = wave_kind == WaveKind::STREAM;
    // The yellow-box open animation (slide-in ~133ms + left_out ~217ms) finishes in ~350ms;
    // wait that long rather than polling the animation, which the Lua skin now drives.
    // (only an open box reads the clock: this runs for every box of the folder)
    bool box_opened = yellow_box_active && get_current_ms() - bar_open_started_at > 350;

    if (is_bank && yellow_box_active && !music_playing && !preview_load && !preview_attempted &&
        get_current_ms() - bar_open_started_at > 250) {
        preview_attempted = true;
        preview_load = std::make_shared<PreviewLoad>();
        if (preview_thread.joinable()) {
            preview_thread.join();
        }
        preview_thread = std::thread([state = preview_load, wave = parser.metadata.wave] {
            state->ok = audio.prepare_nus3bank_pcm(wave, state->pcm, true);
            state->done.store(true, std::memory_order_release);
        });
        fixed_step::join(preview_thread);
    }

    if (is_stream && yellow_box_active && box_opened && !music_playing) {
        const double t0 = get_current_ms();
        const auto open_start = std::chrono::steady_clock::now();
        audio.load_music_stream(parser.metadata.wave, "preview");
        const double open_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - open_start).count();
        play_preview(parser.metadata.demostart);
        if (preview_log() && music_playing) {
            preview_timing = {t0, open_ms, audio.get_music_time_played("preview")};
        }
    }
    if (preview_timing.started_ms > 0 && yellow_box_active && music_playing &&
        audio.get_music_time_played("preview") > preview_timing.position) {
        spdlog::info("Preview of {} playing {:.1f} ms after it was opened (open {:.2f} ms on the main thread), "
                     "{:.0f} ms after the box opened",
                     parser.metadata.wave.filename().string(), get_current_ms() - preview_timing.started_ms,
                     preview_timing.open_ms, get_current_ms() - bar_open_started_at);
        preview_timing.started_ms = 0;
    }

    if (preview_load && preview_load->done.load(std::memory_order_acquire) && yellow_box_active && box_opened &&
        !music_playing) {
        auto state = std::move(preview_load);
        if (state->ok) {
            float demo_start = state->pcm.preview_ms > 0 ? state->pcm.preview_ms / 1000.0f : parser.metadata.demostart;
            audio.load_music_stream_prepared(std::move(state->pcm), "preview");
            play_preview(demo_start);
        }
    }

    if (!score_history) {
        for (Difficulty d : get_diffs()) {
            hash_for(static_cast<int>(d));
        }
        score_history = std::make_unique<ScoreHistory>(scores, hashes, current_time);
    }

    if (score_history) {
        score_history->update(current_time);
    }
}

void SongBox::expand_box() {
    BaseBox::expand_box();
    box_opened_at = get_current_ms();
    if (!holds_preview_slot && fs::exists(parser.metadata.wave)) {
        holds_preview_slot = true;
        preview_holders++;
    }
}

// Starts the freshly loaded "preview" stream in place of the bgm, if the load succeeded.
void SongBox::play_preview(float seek_position) {
    if (!audio.is_music_stream_valid("preview")) {
        return;
    }
    music_playing = true;
    audio.stop_sound(bgm_sound);
    audio.play_music_stream("preview", VolumePreset::MUSIC);
    audio.seek_music_stream("preview", seek_position);
}

void SongBox::stop_preview_load() {
    if (preview_thread.joinable()) {
        preview_thread.join();
    }
    preview_load.reset();
    preview_attempted = false;
    release_preview_slot();
}

void SongBox::release_preview_slot() {
    if (!holds_preview_slot) {
        return;
    }
    holds_preview_slot = false;
    if (preview_holders > 0) {
        preview_holders--;
    }
}

void SongBox::close_box() {
    BaseBox::close_box();
    box_opened_at = 0.0;
    stop_preview_load();
    if (music_playing) {
        if (audio.is_music_stream_valid("preview")) {
            audio.stop_music_stream("preview");
            audio.unload_music_stream("preview");
        }
        bgm_resume_at = get_current_ms() + 330.0;
        music_playing = false;
    }
}

void SongBox::draw_score_history() {
    if (!score_history) {
        return;
    }
    if (!yellow_box_opened) {
        return;
    }
    score_history->draw(get_current_ms() - box_opened_at);
}

void SongBox::enter_box() {
    is_diff_select = true;
    if (!diff_fade_prestarted) {
        diff_fade_in->start();
    }
    diff_fade_prestarted = false;
}

void SongBox::prestart_diff_fade() {
    diff_fade_in->start();
    diff_fade_prestarted = true;
}
