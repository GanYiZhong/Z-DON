#pragma once

#include <cassert>
#include <chrono>
#include <mutex>
#include <sqlite3.h>

#include "color_utils.h"
#include "global_data.h"
#include "parsers/tja.h"

inline int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct PlayerData {
    std::string player_id;
    std::string username = "";
    std::string title = "";
    int title_bg = 0;
    int dan = -1; // -1 = no 段位, matches the column default
    bool gold = false;
    bool rainbow = false;
    bool modifier_auto = false;
    int modifier_speed = 10; // 1.0x, in tenths
    bool modifier_display = false;
    bool modifier_inverse = false;
    int modifier_random = 0;
    bool modifier_skip = false;
    int neiro_index = 0;
    Color chara_color_1 = Color{0x68, 0xBF, 0xC0, 0xFF};
    Color chara_color_2 = Color{0xF9, 0x47, 0x28, 0xFF};
    Color chara_color_3 = Color{0xF9, 0xF0, 0xE1, 0xFF};
    int chara_head_index = 0;
    int chara_body_index = 0;
    int chara_cos_index = 0;
    bool chara_is_costume = true;
    int chara_paint_index = 0;
    int chara_face_index = 0;
    int chara_acce_index = 0;
};

// A course exam's best result: the course-wide value and, for per-song exams, each song's.
// -1 = nothing recorded yet.
struct DanExamBest {
    std::string type;
    int value = -1;
    int song[3] = {-1, -1, -1};
};

struct DanRecord {
    int dan_index = -1;
    int rank = 0;
    int score = 0;
    int arrival = 0;
    std::vector<DanExamBest> exam_bests; // by exam index, from the newest attempt
};

inline Rank rank_for_score(int score) {
    if (score >= 1000000) {
        return Rank::_RAINBOW;
    }
    if (score >= 950000) {
        return Rank::_PURPLE;
    }
    if (score >= 900000) {
        return Rank::_PINK;
    }
    if (score >= 800000) {
        return Rank::_GOLD;
    }
    if (score >= 700000) {
        return Rank::_SILVER;
    }
    if (score >= 600000) {
        return Rank::_BRONZE;
    }
    return Rank::_WHITE;
}

struct Score {
    Crown crown;
    Rank rank;
    int score;
    int good;
    int ok;
    int bad;
    int drumroll;
    int max_combo;
};

// What the loading screen needs from a chart file, cached in song_cache so unchanged files
// are not parsed again at the next start (see LoadingScreen::load_song_hashes).
struct SongCacheEntry {
    int64_t mtime = 0;
    int64_t size = 0;
    // FILETIME of the last write as the directory listing gave it; 0 = not recorded (rows written by
    // an older build or upstream, which leave the column out): those are checked against mtime once
    int64_t ft_write = 0;
    std::array<std::string, 5> hashes;
    std::string title;
    std::string subtitle;
    // what Navigator::preload needs for search and the level filters: every language's title and
    // subtitle (metadata order) and the level of courses 0..4 (-1 = none)
    std::vector<std::pair<std::string, std::string>> titles;
    std::vector<std::pair<std::string, std::string>> subtitles;
    std::array<int, 5> levels = {-1, -1, -1, -1, -1};
};

// Bump when get_diff_hash / the parsers produce different hashes or metadata for the same file,
// so every chart is parsed again.
constexpr int SONG_CACHE_VERSION = 3; // 3: Shift-JIS charts read as UTF-8

struct RemoteScore;

// Z-DON: a dan course song's note count (DanNavigator::song_note_count) for one version of its chart
// file (the song_cache mtime and size of the session that counted it)
struct DanNoteCount {
    int64_t mtime = 0;
    int64_t size = 0;
    int notes = 0;
};

class ScoresManager {
private:
    sqlite3* db_fsd;
    sqlite3_stmt* add_song_stmt = nullptr;
    sqlite3_stmt* store_song_cache_stmt = nullptr; // with the song_cache table made once
    sqlite3_stmt* remove_song_cache_stmt = nullptr;
    std::mutex box_record_mutex; // the song select's threads read and write box records
    sqlite3_stmt* get_box_record_stmt = nullptr;
    sqlite3_stmt* store_box_record_stmt = nullptr;
    std::mutex song_meta_mutex;
    std::unordered_map<std::string, SongCacheEntry> song_meta;
    mutable std::mutex maps_mutex;
    std::unordered_map<fs::path, std::array<std::string, 5>> path_to_hashes;
    std::unordered_map<std::string, fs::path> single_hash_to_path;
    std::unordered_map<std::string, fs::path> diff_hash_to_path;
    std::map<std::tuple<std::string, int, std::string>, Score> score_cache;
    void load_score_cache();

public:
    std::string player_1;
    std::string player_2;
    PlayerData player_1_data;
    PlayerData player_2_data;
    ScoresManager(const fs::path& db_path);
    ~ScoresManager();
    ScoresManager(const ScoresManager&) = delete;
    ScoresManager& operator=(const ScoresManager&) = delete;
    void py_taiko_import(const fs::path& old_db_path);
    void export_to_hiroba(const std::string& access_code, const std::string& player_id);

    int apply_remote_scores(const std::vector<RemoteScore>& scores, const std::string& player_id);
    std::optional<Score> get_score(const std::string& hash, int difficulty, const std::string& player_id);
    // out_score_id (optional) receives the new scores row id, or -1 if the insert failed.
    Score save_score(const std::string& hash, int difficulty, const std::string& player_id, Score score,
                     int64_t played_at, const std::string& modifiers_json, int64_t* out_score_id = nullptr);
    void add_path_binding(const fs::path& path, const std::array<std::string, 5>& hashes);
    std::array<std::string, 5> get_hashes(const fs::path& path);
    std::string get_single_hash(const fs::path& path);
    std::optional<fs::path> get_path_by_hash(const std::string& single_hash);
    std::optional<fs::path> get_path_by_diff_hash(const std::string& diff_hash);
    void add_song(const std::array<std::string, 5>& hash, const std::string& title, const std::string& subtitle);
    // song_cache: path -> entry, current SONG_CACHE_VERSION only
    std::unordered_map<std::string, SongCacheEntry> load_song_cache();
    void store_song_cache(const std::string& path, const SongCacheEntry& entry);
    // fills in ft_write of rows that matched by mtime and size
    void set_song_cache_ft_write(const std::vector<std::pair<std::string, int64_t>>& rows);
    void remove_song_cache(const std::string& path);
    // the loading screen's scan result per chart (key: UTF-8 path), for Navigator::preload
    void set_song_meta(const std::string& path, const SongCacheEntry& entry);
    std::optional<SongCacheEntry> get_song_meta(const std::string& path);
    // Z-DON table zdon_dan_note_counts: (UTF-8 path, difficulty) -> count, current SONG_CACHE_VERSION only
    std::map<std::pair<std::string, int>, DanNoteCount> load_dan_note_counts();
    void store_dan_note_counts(const std::vector<std::pair<std::pair<std::string, int>, DanNoteCount>>& rows);
    // Z-DON table zdon_song_box_records: (UTF-8 path) -> SongParser::box_record. A record is only returned
    // for the version of the chart this session's loading screen saw (its get_song_meta mtime, size and
    // listing write time), else "" (and for a path the loading screen did not see).
    std::vector<std::string> get_song_box_records(const std::vector<std::string>& paths);
    // stored for the version get_song_meta has (a path without one is skipped)
    void store_song_box_records(const std::vector<std::pair<std::string, std::string>>& rows);
    // Best-of view over dan_scores for one course (course_id from dan_course_id).
    std::optional<DanRecord> get_dan_record(const std::string& player_id, const std::string& course_id);
    // Appends one attempt to dan_scores; each song links to its normal scores row (DanResultSong::score_id).
    // exam_bests: the course's exam bests including this attempt (stored on its row)
    void save_dan_score(const std::string& player_id, const std::string& course_id, const DanResultData& rd,
                        int arrival, const std::vector<DanExamBest>& exam_bests = {});
    std::optional<PlayerData> get_player_data(const std::string& player_id);
    void save_player_data(const PlayerData& player);
    void migrate_player_id(const std::string& old_id, const std::string& new_id);
    bool begin_transaction();
    bool commit();
};

// Chart hash of one dan song (path binding, else parsed from the chart).
std::string dan_song_hash(const DanSongEntry& song);

// Identity of a dan course: sha256 over its songs' chart hashes + difficulties and its exams.
std::string dan_course_id(const std::vector<DanSongEntry>& songs, const std::vector<Exam>& exams);

extern ScoresManager* _scores_manager_ptr;
inline ScoresManager& get_scores_manager() {
    assert(_scores_manager_ptr && "scores_manager used before init_scores_manager()");
    return *_scores_manager_ptr;
}
#define scores_manager get_scores_manager()
void init_scores_manager(bool gen3);

inline Color chara_default_color_1(PlayerNum player_num) {
    return (player_num == PlayerNum::P1) ? Color{104, 191, 192, 255} : Color{249, 71, 40, 255};
}
inline Color chara_default_color_2(PlayerNum player_num) {
    return (player_num == PlayerNum::P1) ? Color{249, 71, 40, 255} : Color{104, 191, 192, 255};
}

inline Modifiers player_data_to_modifiers(const PlayerData& pd) {
    Modifiers m;
    m.auto_play = pd.modifier_auto;
    m.speed = pd.modifier_speed;
    m.display = pd.modifier_display;
    m.inverse = pd.modifier_inverse;
    m.random = pd.modifier_random;
    m.skip = pd.modifier_skip;
    return m;
}
