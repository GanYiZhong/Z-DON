#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <queue>
#include <stop_token>
#include <unordered_map>

#include "../../../libs/file/filesystem.h"
#include "box_song.h"
#include "genre_bg.h"

class SongSelectScript;

struct CourseStats {
    int total = 0;
    int full_combos = 0;
    int clears = 0;
    int full_combos_2p = 0;
    int clears_2p = 0;
};

struct InlineState {
    std::unique_ptr<FolderBox> saved_folder_box;
    int folder_index;
    int first_song_index;
    int songs_count;
    bool fading_out = false;
};

using Statistics = std::map<int, std::map<int, CourseStats>>;

class Navigator {
private:
    std::vector<fs::path> root_paths;
    std::vector<std::unique_ptr<BaseBox>> items;
    TextureObject* t_background = nullptr;
    std::map<std::pair<std::string, std::string>, fs::path> song_files;
    // find_song_by_title's lookup tables over song_files, rebuilt when its size changes: the dojo
    // looks up every chart of every course (1000+), and each linear pass re-normalized the library
    std::mutex title_index_mutex;
    size_t title_index_size = SIZE_MAX;
    std::unordered_map<std::string, fs::path> title_subtitle_index;  // normalized "title\x1fsubtitle"
    std::unordered_map<std::string, fs::path> title_only_index;      // normalized title
    std::vector<std::pair<std::string, fs::path>> normalized_titles; // song_files order
    // song path -> every title and subtitle in every language, ASCII-folded, for Song Search
    std::unordered_map<std::string, std::string> song_search_text;
    // song path -> level per course 0..4 (-1 = course absent), filled by the same scan; the
    // difficulty filter reads this instead of re-parsing every chart on disk
    std::unordered_map<std::string, std::array<int, 5>> song_levels;
    std::unordered_map<std::string, std::pair<int, std::string>> zdon_genre_cache; // zdon_genre_of
    int open_index;
    bool is_init = false;
    bool is_preloaded = false;
    bool reloading_roots = false;
    bool genre_box_defs_built = false;
    std::map<GenreIndex, BoxDef> genre_box_defs;
    bool built_hide_dan = false;
    bool built_hide_collections = false;
    bool built_waiwai = false;
    bool built_is_2p = false;
    bool built_ai_assigned = false;

    std::optional<InlineState> inline_state;
    std::optional<fs::path> pending_inline_path;
    FolderBox* pending_inline_folder = nullptr;
    BoxDef inline_back_def; // genre/colours for the repeated back boxes of the open folder
    BoxDef pending_inline_box_def;
    bool is_inline = false;

    std::optional<GenreBG> genre_bg;
    int genre_bg_start;
    int genre_bg_end;
    std::optional<float> genre_bg_end_pos;
    GenreIndex bg_genre_index;
    GenreIndex last_bg_genre_index;
    bool bg_genre_pending = true;

    FadeAnimation* background_fade_change;
    MoveAnimation* background_move;

    std::thread loader_thread;

    struct InlinePrefetch {
        fs::path path;
        std::vector<fs::path> song_paths;
        std::unordered_map<std::string, std::vector<std::pair<bool, fs::path>>> plan;
        std::unordered_map<std::string, std::unique_ptr<SongParser>> preparsed;
        bool new_songs = false; // song_paths are the NEW collection's hits (no plan)
        bool complete = false;  // set by the prefetch thread when it was not aborted
    };
    std::thread prefetch_thread;
    std::unique_ptr<InlinePrefetch> prefetch;
    void start_inline_prefetch(const fs::path& path);
    void start_new_songs_prefetch(const fs::path& path);
    std::vector<fs::path> find_new_songs(const fs::path& path) const;
    void join_prefetch();
    void scan_song_tree(const fs::path& path, std::vector<fs::path>& song_paths,
                        std::unordered_map<std::string, std::vector<std::pair<bool, fs::path>>>& plan);
    std::thread song_files_thread;
    std::mutex pending_mutex;
    std::queue<std::unique_ptr<BaseBox>> pending_boxes;
    std::queue<std::unique_ptr<BaseBox>> pending_inline_boxes;
    std::atomic<bool> loading_complete{false};
    std::atomic<bool> abort_loading{false};

    std::optional<fs::path> reopen_folder_path;
    std::optional<fs::path> reopen_song_path;
    std::optional<fs::path> restore_cursor_path;
    std::optional<fs::path> open_start_folder;
    fs::path start_folder; // start_path listing: top of navigation, so it has no back box

    std::optional<fs::path> recent_folder_path;
    std::optional<fs::path> favorite_folder_path;
    std::set<std::string> favorite_songs;
    std::unordered_map<std::string, bool> def_file_cache;
    std::unordered_map<std::string, bool> child_folders_cache; // has_child_folders; cleared with def_file_cache
    std::mutex child_folders_mutex; // def_file_cache + child_folders_cache: the loader thread fills them too
    std::unordered_map<std::string, BoxDef> box_def_cache;

    bool awaiting_diff_sort = false;
    std::optional<std::array<int, 3>> diff_sort_filter;
    std::optional<std::pair<int, int>> last_diff_sort_result;
    std::optional<std::set<fs::path>> diff_sort_scope; // nullopt: the box sits at a root

    bool vertical_gallery = false;

    void navigate(int delta, bool snap);
    void set_positions(bool init, float duration);
    bool is_song_file(const fs::path& path);
    bool is_osu_song_folder(const fs::path& path);
    bool is_gen4_song_folder(const fs::path& path);
    bool is_gen4_root(const fs::path& path);
    bool is_gen3_root(const fs::path& path);
    fs::path gen3_root_at(const fs::path& path);
    bool is_gen3_song_folder(const fs::path& path);
    void load_gen3_genres(const fs::path& data_root);
    bool load_gen3_genre_songs(const fs::path& genre_path, const BoxDef& box_def);
    bool only_gen4_songs();
    const BoxDef* box_def_for_genre(GenreIndex genre);
    void load_gen4_genres(const fs::path& data_root);
    bool load_gen4_genre_songs(const fs::path& genre_path, const BoxDef& box_def);
    bool has_def_file(const std::filesystem::path& path);
    fs::path find_box_def_folder(const fs::path& song_path);

    void setup_back_box(const fs::path& path, bool has_children, const BaseBox* from = nullptr);
    void prefetch_child_folders(const fs::path& parent);
    bool has_child_folders(const fs::path& path);
    bool scan_child_folders(const fs::path& path);

    void wait_for_song_files();
    void enqueue_box(std::unique_ptr<BaseBox> box);
    void enqueue_inline_box(std::unique_ptr<BaseBox> box);
    void parse_song_list(const fs::path& path, const BoxDef& box_def, bool inline_mode);
    void load_current_directory_async(const fs::path path);
    void clear_directory_state(); // merge 0ed42ec: upstream's split of load_all_roots
    void load_all_roots();
    void load_collection_difficulty(const fs::path& path, const BoxDef& box_def, int course, int level, int order = 1);
    void load_from_song_list(const fs::path& path, const BoxDef& box_def, bool mark_favorite);
    void load_collection_new(const fs::path& path, const BoxDef& box_def);
    void load_collection_recommended(const fs::path& path, const BoxDef& box_def);
    void load_collection_search(const fs::path& path, const BoxDef& box_def);
    void load_collection_play_count(const fs::path& path, const BoxDef& box_def);
    void load_songs_inline_async(const fs::path path, BoxDef box_def);
    fs::path resolve_start_path(); // upstream: config start_path
    void load_start_folder();
    void promote_recent_box(const SongBox* song);
    void reload_favorites();
    void collapse_inline_now();
    void flush_pending_boxes();
    void exit_inline();
    void begin_inline_load();
    void dump_library_if_requested();
    void selftest_box_records_if_requested();

public:
    Navigator();
    ~Navigator();

    std::atomic<bool> is_processing{false};
    bool hide_dan = false;
    bool hide_collections = false; // guests only: no collection folders, favorites or recent
    bool ai_assigned = false;      // AI battle rank 10: show the assigned songs folder
    bool waiwai = false;
    bool is_2p = false;
    bool inline_streaming = false;
    fs::path current_path;
    fs::path start_path;
    std::string current_search;

    SongSelectScript* script = nullptr;

    void join_loader();
    void cancel_loading() { join_loader(); } // upstream's name for join_loader
    void clear_folder_caches();
    void apply_genre_of(const fs::path& song_path, SongBox* song);
    void enqueue_inline_back_every_ten(int songs_added, const fs::path& parent);
    // `files`: the chart files under songs_paths when the caller already listed them (the loading screen),
    // `stamps` what their listing said about each (get_song_files)
    void preload(std::vector<fs::path> songs_paths, std::optional<std::vector<fs::path>> listed = std::nullopt,
                 std::vector<SongFileStamp> stamps = {});
    void init(std::vector<fs::path> songs_paths);

    void reset_for_skin_reload();
    void add_to_recent(const SongBox* song);
    void toggle_favorite(SongBox* song);
    void refresh_scores();
    BoxDef parse_box_def(const fs::path& path);
    static BoxDef parse_box_def_uncached(const fs::path& path);
    std::atomic<bool> song_files_ready{true};
    bool diff_sort_ready() { return awaiting_diff_sort; }
    bool in_inline() const { return is_inline; }
    const std::optional<std::set<fs::path>>& get_diff_sort_scope() const { return diff_sort_scope; }
    void apply_diff_sort(int course, int level, int order = 1);
    void cancel_diff_sort();
    void load_current_directory(const fs::path path);
    bool jump_to_song(const std::string& hash);
    bool jump_to_song_path(const fs::path& song_path);
    // defer_box (Green AI select, skin key zdon_ai_card_hold_ms): the other boxes leave now, the card widens later
    // through enter_current_box()
    void enter_diff_select(bool defer_box = false, bool fade_in_place = false);
    void enter_current_box();
    void exit_diff_select();
    float get_diff_fade_in();
    bool is_directory(BaseBox* item);
    bool is_song(BaseBox* item);
    BaseBox* get_current_item();
    Statistics get_statistics(const std::vector<fs::path>& paths, const std::stop_token& stop,
                              const std::set<fs::path>* scope = nullptr);

    std::optional<fs::path> find_song_by_title(const std::string& title, const std::string& subtitle);

    // Z-DON song search panel (scenes/song_select_zdon_search.cpp)
    std::vector<fs::path> zdon_search(const std::string& query, const std::string& genre, int course, int level,
                                      size_t limit, size_t& total);
    std::vector<std::string> zdon_genres();
    std::pair<int, std::string> zdon_genre_of(const fs::path& song_path);
    std::array<int, 5> zdon_levels_of(const fs::path& song_path) const;
    size_t song_file_count() const { return song_files.size(); }

    // Z-DON AI select (skin key zdon_ai_instant_page0): the cursor goes to the first folder of `genre` at once, the
    // shelf and the box opening animation at rest (the cabinet page 0 has no scroll to the box)
    bool snap_to_genre(GenreIndex genre);
    void move_left();
    void move_right();
    void skip_left();
    void skip_right();

    void update(double current_ms);
    void draw_background();
    void draw();
    void draw_diff_select_bg();
    void draw_score_history();

    MoveAnimation* background_move_anim() const { return background_move; }
    FadeAnimation* background_fade_anim() const { return background_fade_change; }
    FolderBox* lua_current_folder() const {
        if (!inline_state.has_value()) {
            return nullptr;
        }
        return inline_state->saved_folder_box.get();
    }
    int bg_genre_frame() const { return genre_to_ref_frame(bg_genre_index); }
    int last_bg_genre_frame() const { return genre_to_ref_frame(last_bg_genre_index); }

    enum WheelEvent {
        WHEEL_EVENT_NONE = 0,
        WHEEL_EVENT_SCENE_ENTRY = 1,
        WHEEL_EVENT_CURSOR_MOVE = 2,
        WHEEL_EVENT_OPEN_BEGIN = 3,
        WHEEL_EVENT_OPEN_SWAP = 4,
        WHEEL_EVENT_CLOSE_BEGIN = 5,
        WHEEL_EVENT_CLOSE_SWAP = 6,
        WHEEL_EVENT_COURSE_BACK = 7,
    };
    int wheel_event = WHEEL_EVENT_NONE;
    int wheel_event_seq = 0;
    double wheel_leg_ms = 0.0;
    bool wheel_events_read = false; // the skin script reads the wheel events
    void emit_wheel_event(int id);

    static constexpr double kSwapDelayMs = (114.0 - 45.0) / 60.0 * 1000.0;
    bool swap_is_early(int begin_id) const;
};

extern Navigator navigator;
