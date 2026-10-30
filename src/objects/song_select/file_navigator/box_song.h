#pragma once

#include <atomic>
#include <cmath>
#include <thread>

#include "../../../libs/audio.h"
#include "../../../libs/song_parser.h"
#include "box_base.h"
#include "score_history.h"

class SongBox : public BaseBox {
    void preregister_text() override;

public:
    std::array<std::string, 5> hashes;
    std::array<std::optional<Score>, 5> scores;
    std::array<std::optional<Score>, 5> scores_p2;
    SongParser parser;
    bool is_favorite;
    std::string text_subtitle;
    std::unique_ptr<OutlinedText> bpm_text;
    std::optional<Texture> preimage;
    bool music_playing = false;
    struct PreviewLoad {
        std::atomic<bool> done{false};
        bool ok = false;
        AudioEngine::PreparedPCM pcm;
    };
    std::shared_ptr<PreviewLoad> preview_load;
    std::thread preview_thread;
    bool preview_attempted = false;
    // the preview audio file, checked once on the first update()
    enum class WaveKind {
        UNKNOWN,
        MISSING,
        BANK,
        STREAM
    } wave_kind = WaveKind::UNKNOWN;
    std::unique_ptr<ScoreHistory> score_history;
    double box_opened_at = 0.0;
    FadeAnimation* diff_fade_in;
    bool is_ura = false;
    GenreIndex song_genre_index = GenreIndex::DEFAULT;
    std::string song_genre_label;

    // Z-DON: one course's chart numbers for the skin's difficulty select (box_song_stats.cpp): parsed on a
    // worker the first time they are asked for (empty until then), then kept on the box
    struct ChartStats {
        int notes = 0;          // don / ka notes (the master branch on branching charts)
        double length_ms = 0.0; // the last note's time
        double bpm_min = 0.0;
        double bpm_max = 0.0;
    };
    struct ChartStatsJob {
        std::atomic<bool> done{false};
        ChartStats stats;
    };
    std::array<std::optional<ChartStats>, 5> chart_stats_cache;
    std::array<std::shared_ptr<ChartStatsJob>, 5> chart_stats_jobs;
    const ChartStats& chart_stats(int difficulty);
    // Z-DON: the last plays of one course (scores_manager.recent_plays), read once per box until
    // refresh_scores() (box_song_stats.cpp)
    std::array<std::optional<std::vector<PlayRecord>>, 5> recent_plays_cache;
    const std::vector<PlayRecord>& recent_plays(int difficulty);

    SongBox(const fs::path& path, const BoxDef& box_def, SongParser parser);
    ~SongBox() override {
        stop_preview_load();
        if (preimage.has_value()) {
            unload_texture(preimage.value());
        }
    }

    static void service_bgm_resume(double current_ms);
    static void reset_bgm_slot();
    void release_preview_slot();
    void stop_preview_load();
    void play_preview(float seek_position);
    bool holds_preview_slot = false;

    void reset() override;

    void load_text() override;
    void update(double current_time) override;
    void draw_score_history() override;
    void expand_box() override;
    void enter_box() override;
    void close_box() override;
    std::vector<Difficulty> get_diffs();

    void refresh_scores();
    std::string hash_for(int difficulty);

    const char* lua_kind() const override { return "song"; }
    float subtitle_font_size() const {
        float font_size = tex.get_skin_config(SC::SONG_SELECT_BOX_SUBTITLE)->font_size;
        if (utf8_char_count(text_subtitle) >= 30) {
            font_size -= (int)(10 * tex.screen_scale);
        }
        return font_size;
    }
    OutlinedText* horizontal_subtitle() {
        if (!horizontal_subtitle_cache) {
            horizontal_subtitle_cache =
                std::make_unique<OutlinedText>(text_subtitle, subtitle_font_size(), text_color, fore_color, false);
        }
        return horizontal_subtitle_cache.get();
    }
    OutlinedText* horizontal_subtitle_large() {
        if (!horizontal_subtitle_large_cache) {
            horizontal_subtitle_large_cache = std::make_unique<OutlinedText>(
                text_subtitle, (int)(subtitle_font_size() * 1.3f), text_color, fore_color, false);
        }
        return horizontal_subtitle_large_cache.get();
    }
    bool has_ura() const { return parser.metadata.course_data.contains((int)Difficulty::URA); }
    int ex_data_flag() const {
        if (parser.ex_data.new_audio) {
            return 1;
        }
        if (parser.ex_data.old_audio) {
            return 2;
        }
        if (parser.ex_data.limited_time) {
            return 3;
        }
        if (is_new) {
            return 4;
        }
        return 0;
    }
    struct CourseInfo {
        bool has_course;
        int level;
        bool is_branching;
        int crown;
        int rank;
    };
    CourseInfo course_info(int diff) const {
        auto it = parser.metadata.course_data.find(diff);
        bool has_course = it != parser.metadata.course_data.end();
        CourseInfo info{has_course, 0, false, (int)Crown::NONE, (int)Rank::_NONE};
        if (has_course) {
            info.level = (int)std::round(it->second.level);
            info.is_branching = it->second.is_branching;
        }
        if (diff >= 0 && diff < (int)scores.size() && scores[diff].has_value()) {
            info.crown = (int)scores[diff]->crown;
            info.rank = (int)scores[diff]->rank;
        }
        return info;
    }

protected:
    std::unique_ptr<OutlinedText> horizontal_subtitle_cache;
    std::unique_ptr<OutlinedText> horizontal_subtitle_large_cache;
};
