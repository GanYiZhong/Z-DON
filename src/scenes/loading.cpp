#include "loading.h"

#include <future>
#include <optional>
#include <unordered_set>

#include "../libs/file/filesystem.h"
#include "../libs/file/scores.h"
#include "../libs/file/script.h"
#include "../libs/fixed_step.h"
#include "../libs/global_data.h"
#include "../libs/input/input.h"
#include "../libs/jobs.h"
#include "../libs/library_index.h"
#include "../libs/network/online_songs.h"
#include "../libs/network/skin_updater.h"
#include "../libs/parsers/song_parser.h"
#include "../libs/song_library.h"
#include "../libs/song_library_loader.h"
#include "../libs/zdon_os.h"
#include "../objects/song_select/file_navigator/navigator.h"
#include "../objects/song_select/song_select_script.h"
#include "../platform/platform.h"

namespace {
    struct LibraryWalk {
        std::vector<fs::path> songs;
        std::vector<SongFileStamp> stamps;
        std::shared_ptr<const LibraryIndex> index;
    };
    // Started by main, taken by the first load_song_hashes. Never freed if no loading screen runs: the
    // process exits with it.
    std::future<LibraryWalk>* g_early_walk = nullptr;

    LibraryWalk walk_library(const std::vector<fs::path>& roots) {
        LibraryWalk w;
        w.songs = get_song_files(roots, &w.stamps, &w.index);
        return w;
    }
} // namespace

void LoadingScreen::start_library_walk_early() {
#ifndef __EMSCRIPTEN__
    if (!g_early_walk) {
        g_early_walk = new std::future<LibraryWalk>(
            std::async(std::launch::async, walk_library, global_data.config->paths.tja_path));
    }
#endif
}

void LoadingScreen::on_screen_start() {
    init_visuals();

    start_ms = get_current_ms();
#ifdef __EMSCRIPTEN__
    load_song_hashes();
#else
    loading_thread = std::thread(&LoadingScreen::load_song_hashes, this);
    fixed_step::join(loading_thread);
#endif
}

// Loads this screen's textures/sounds and (re)grabs the texture pointers and
// geometry from the current skin. Split out of on_screen_start() so it can be
// run again after a skin update forces a skin reload.
void LoadingScreen::init_visuals() {
    tex.load_screen_textures("loading");
    audio.load_screen_sounds("loading");

    progress_bar.width = tex.screen_width * 0.43;
    progress_bar.height = 50 * tex.screen_scale;
    progress_bar.x = (tex.screen_width - progress_bar.width) / 2;
    progress_bar.y = tex.screen_height * 0.85;

    if (const SkinInfo* bar = tex.get_skin_config(SC::LOADING_PROGRESS_BAR)) {
        if (bar->width > 0 && bar->height > 0) {
            progress_bar = {bar->x, bar->y, bar->width, bar->height};
        }
    }

    double fade_ms = 1000;
    if (const SkinInfo* f = tex.get_skin_config(SC::LOADING_FADE_MS); f && f->x > 0) {
        fade_ms = f->x;
    }
    fade_in = std::make_unique<FadeAnimation>(fade_ms, 0.0, false, false, 1.0);
    allnet_indicator.emplace();
    t_warning = tex.get_texture("kidou/warning");

    countdown_ms = 0.0;
    if (tex.options[SCO::LOADING_COUNTDOWN] && script_manager.lua) {
        sol::protected_function require = (*script_manager.lua)["require"];
        auto r = require("loading/loading_countdown");
        if (r.valid()) {
            sol::table module = r;
            countdown_ms = module.get_or("seconds", 0.0) * 1000.0;
        }
    }
    has_countdown = countdown_ms > 0.0;
}

void LoadingScreen::load_song_hashes() {
    // merge 0ed42ec: upstream fills the shared SongLibrary here (title index for its navigation_load / dan
    // navigator); false when it is already loaded or loading (a later loading screen)
    const bool populate_library = get_song_library().begin_loading();
    try {
        // merge 4917fdc8 (upstream fd2e4c08): the online song roots are brought up to date first
        online_songs::sync(global_data.config->paths.song_manifests, progress);
        progress = 0.0f;
        // Listed here, not in on_screen_start: the screen keeps drawing while a large library is walked
        std::shared_ptr<const LibraryIndex> index;
        if (g_early_walk) { // walked while main loaded the skin
            std::unique_ptr<std::future<LibraryWalk>> early(std::exchange(g_early_walk, nullptr));
            LibraryWalk w = early->get();
            songs = std::move(w.songs);
            song_stamps = std::move(w.stamps);
            index = std::move(w.index);
        } else {
            songs = get_song_files(global_data.config->paths.tja_path, &song_stamps, &index);
        }
        // the song select answers its folder questions from this walk (nullptr: from the disk)
        publish_library_index(std::move(index));
        std::atomic<int> songs_loaded = 0;
        const int thread_count = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        std::mutex scores_mutex;

        // Files whose size and modification time match the cache are not parsed again; the
        // workers only read `cache`, new or changed entries are written after they finish.
        const std::unordered_map<std::string, SongCacheEntry> cache = scores_manager.load_song_cache();
        std::vector<std::pair<std::string, SongCacheEntry>> fresh;
        std::vector<std::pair<fs::path, SongMetadata>> library_entries; // added in the background, see below
        std::vector<std::pair<std::string, int64_t>> ft_backfill;       // rows that only had mtime
        // bound to their paths once the workers are done (see below)
        std::vector<std::optional<std::array<std::string, 5>>> bindings(songs.size());
        std::atomic<int> cache_hits = 0;

        // every worker takes the next chart (a new song pack is a run of uncached charts: in fixed ranges
        // one worker parsed all of it); `ranges` below only orders the path bindings
        std::atomic<int> cursor{0};
        auto worker = [&] {
            // Z-DON: below the main thread (it draws the loading screen); CPU only -- the loading screen
            // waits for these reads, so not background I/O
            zdon_os::job_priority_background(true);
            for (int i; (i = cursor.fetch_add(1)) < static_cast<int>(songs.size());) {
                auto u8 = songs[i].u8string();
                const std::string path(u8.begin(), u8.end());

                // A row with the listing's write time and size is current without opening the file. Only
                // a row without it (older build), a changed file (its mtime is stored for builds that
                // compare that) and a link or folder (the listing describes the link) stat the file.
                const SongFileStamp& listed = song_stamps[i];
                const bool listing_usable = listed.listed && !listed.is_dir && !listed.is_reparse;
                auto cached = cache.find(path);
                SongCacheEntry stamp;
                bool from_cache = false;
                if (listing_usable && cached != cache.end() && cached->second.ft_write != 0) {
                    from_cache = cached->second.ft_write == listed.ft_write && cached->second.size == listed.size;
                    stamp.mtime = cached->second.mtime;
                    stamp.size = cached->second.size;
                }
                if (!from_cache) {
                    // upstream: a platform's own stamp first (Switch); Windows / Linux stat the file
                    if (!platform_file_stamp(songs[i], stamp.mtime, stamp.size)) {
                        std::error_code ec;
                        const auto mtime = std::filesystem::last_write_time(songs[i], ec);
                        if (ec) {
                            spdlog::error("Could not stat {}: {}", path, ec.message());
                            continue;
                        }
                        const auto size = std::filesystem::file_size(songs[i], ec);
                        stamp.mtime = static_cast<int64_t>(mtime.time_since_epoch().count());
                        stamp.size = ec ? -1 : static_cast<int64_t>(size);
                    }
                    if (cached != cache.end() && cached->second.ft_write == 0) {
                        from_cache = cached->second.mtime == stamp.mtime && cached->second.size == stamp.size;
                        if (from_cache && listing_usable) {
                            std::scoped_lock lock(scores_mutex);
                            ft_backfill.emplace_back(path, listed.ft_write);
                        }
                    } else if (cached != cache.end() && !listing_usable) {
                        from_cache = cached->second.mtime == stamp.mtime && cached->second.size == stamp.size;
                    }
                }
                stamp.ft_write = listing_usable ? listed.ft_write : 0;

                std::array<std::string, 5> hashes;
                std::string title, subtitle;
                SongCacheEntry meta; // handed to Navigator::preload
                if (from_cache) {
                    hashes = cached->second.hashes;
                    title = cached->second.title;
                    subtitle = cached->second.subtitle;
                    meta = cached->second;
                    cache_hits++;
                } else {
                    try {
                        SongParser parser(songs[i]);
                        spdlog::debug("Parsing song: {}", path);
                        if (auto mirrored = online_songs::chart_hashes(songs[i])) {
                            hashes = *mirrored; // merge 4917fdc8: an online song's hashes come from its manifest
                        } else if (songs[i].extension() == ".osu") {
                            hashes[0] = parser.get_diff_hash(0);
                        } else {
                            for (const auto& [course, course_data] : parser.metadata.course_data) {
                                if (course < 0 || course >= static_cast<int>(hashes.size())) {
                                    continue;
                                }
                                hashes[course] = parser.get_diff_hash(course);
                            }
                        }
                        auto metadata = song_metadata_from(parser.metadata); // upstream's shared helper
                        title = metadata.title;
                        subtitle = metadata.subtitle;
                        stamp.hashes = hashes;
                        stamp.title = title;
                        stamp.subtitle = subtitle;
                        stamp.titles = std::move(metadata.titles);
                        stamp.subtitles = std::move(metadata.subtitles);
                        stamp.levels = metadata.levels;
                        meta = stamp;
                        std::scoped_lock lock(scores_mutex);
                        fresh.emplace_back(path, std::move(stamp));
                    } catch (const std::exception& e) {
                        spdlog::error("Failed to parse song {}: {}", path, e.what());
                        continue;
                    }
                }

                // upstream's is_library_song_file stats the file; the listing already says it is a file (a stat per
                // chart under scores_mutex cost ~450 ms on 3008 cached charts), so only a link / unlisted one stats
                const fs::path ext = songs[i].extension();
                const bool library_file = populate_library && (listing_usable ? (ext == ".tja" || ext == ".osu")
                                                                              : is_library_song_file(songs[i]));
                try {
                    // Z-DON: built outside scores_mutex (set_song_meta has its own lock)
                    std::optional<SongMetadata> library_meta;
                    if (library_file) {
                        library_meta.emplace(
                            SongMetadata{meta.title, meta.subtitle, meta.titles, meta.subtitles, meta.levels});
                    }
                    scores_manager.set_song_meta(path, std::move(meta));
                    std::scoped_lock lock(scores_mutex);
                    // a cached file's songs row was written when it was parsed
                    if (!from_cache) {
                        scores_manager.add_song(hashes, title, subtitle);
                    }
                    bindings[i] = hashes;
                    if (library_meta) {
                        library_entries.emplace_back(songs[i], std::move(*library_meta));
                    }

                    progress = static_cast<float>(++songs_loaded) / songs.size();
                } catch (const std::exception& e) {
                    spdlog::error("Failed to record song {}: {}", path, e.what());
                }
            }
        };

        std::vector<std::pair<int, int>> ranges; // each worker's [start, end) of songs
#ifdef __EMSCRIPTEN__
        ranges.emplace_back(0, (int)songs.size());
#else
        int chunk = songs.size() / thread_count;
        for (int i = 0; i < thread_count; i++) {
            int start = i * chunk;
            int end = (i == thread_count - 1) ? songs.size() : start + chunk;
            ranges.emplace_back(start, end);
        }
#endif
        scores_manager.begin_transaction();
#ifdef __EMSCRIPTEN__
        worker();
#else
        threads.reserve(ranges.size());
        for (size_t t = 0; t < ranges.size(); t++) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }
#endif
        // Charts sharing a hash map to the one bound last. The workers used to bind as they went, so
        // that was the one its worker reached last: with every chart about as quick (from the cache),
        // the later position in its worker's range. Bound in that order here -- position in the range,
        // then range -- for the same result without the race between workers.
        for (int offset = 0;; offset++) {
            bool any = false;
            for (const auto& [start, end] : ranges) {
                const int i = start + offset;
                if (i < end) {
                    any = true;
                    if (bindings[i]) {
                        scores_manager.add_path_binding(songs[i], *bindings[i]);
                    }
                }
            }
            if (!any) {
                break;
            }
        }
        for (const auto& [path, entry] : fresh) {
            scores_manager.store_song_cache(path, entry);
        }
        scores_manager.set_song_cache_ft_write(ft_backfill);
        // forget files that are gone, so the table does not grow with deleted songs
        if (!cache.empty()) {
            std::unordered_set<std::string> present;
            present.reserve(songs.size());
            for (const auto& song : songs) {
                auto u8 = song.u8string();
                present.emplace(u8.begin(), u8.end());
            }
            for (const auto& [path, entry] : cache) {
                if (!present.contains(path)) {
                    scores_manager.remove_song_cache(path);
                }
            }
        }
        scores_manager.commit();
        spdlog::info("Song scan: {} files, {} from cache, {} parsed", songs.size(), cache_hits.load(), fresh.size());

        if (fs::exists(fs::path("scores_pytaiko.db"))) {
            scores_manager.py_taiko_import(fs::path("scores_pytaiko.db"));
            fs::remove(fs::path("scores_pytaiko.db"));
        }

        if (populate_library) {
            // Z-DON: filled by a background job, not by the workers: SongLibrary::add folds every title under the
            // library's lock, which (inside scores_mutex) serialized the workers and made the warm scan of 3008
            // charts ~500 ms slower. Its readers (upstream's navigation loader / dan navigator) wait for it.
            jobs::submit(jobs::Priority::Background, [entries = std::move(library_entries)] {
                for (const auto& [path, metadata] : entries) {
                    get_song_library().add(path, metadata);
                }
                get_song_library().finish_loading();
            });
        }

        load_navigator();
    } catch (const std::exception& e) {
        if (populate_library) {
            get_song_library().fail_loading();
        }

        spdlog::error("Loading failed: {}", e.what());
    }
    loading_complete = true;
}

void LoadingScreen::load_navigator() {
    navigator.preload(global_data.config->paths.tja_path, songs, song_stamps);
}

Screens LoadingScreen::on_screen_end(Screens next_screen) {
    if (loading_thread.joinable()) {
        loading_thread.join();
    }
    return Screen::on_screen_end(next_screen);
}

std::optional<Screens> LoadingScreen::update() {
    Screen::update();
    if (allnet_indicator) {
        allnet_indicator->update(get_current_ms());
    }

    if (is_l_don_pressed() || is_r_don_pressed()) {
        skip_requested = true;
    }

    if (!skin_reloaded && loading_complete && skin_updater.finished() && skin_updater.updated_this_pass()) {
        skin_reloaded = true;
        spdlog::info("Skin update applied; reloading skin before leaving the loading screen");
        try {
            allnet_indicator.reset();
            t_warning = nullptr;
            has_countdown = false;
            drop_other_screens_for_skin_reload();
            navigator.reset_for_skin_reload();
            unload_skin();
            load_skin();
            reload_skin_screens();
        } catch (const std::exception& e) {
            spdlog::error("Skin reload after update failed: {}", e.what());
        }
        init_visuals();
    }

    const bool skin_updater_blocking = skin_updater.applying_updates();

    if (loading_complete && !fade_in->isStarted() && !skin_updater_blocking &&
        (skip_requested || get_current_ms() - start_ms >= countdown_ms)) {
        fade_in->start();
    }

    fade_in->update(get_current_ms());
    if (fade_in->is_finished) {
        if (!global_data.cli_dan.empty()) {
            return on_screen_end(Screens::DAN_SELECT); // `--dan`
        }
        if (global_data.needs_onboarding) {
            return on_screen_end(Screens::ONBOARDING);
        }
#ifdef __EMSCRIPTEN__
        return on_screen_end(Screens::ENTRY);
#else
        return on_screen_end(Screens::TITLE);
#endif
    }

    return std::nullopt;
}

void LoadingScreen::draw() {
    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, Color::BLACK);

    draw_rectangle(progress_bar, Color(101, 0, 0, 255));

    float clamped_progress = std::max(0.0f, std::min(1.0f, progress.load()));
    Rect fill = progress_bar;
    fill.width *= clamped_progress;
    if (fill.width > 0) {
        draw_rectangle(fill, Color::RED);
    }
    tex.draw_texture(t_warning);

    if (has_countdown) {
        sol::protected_function draw_countdown =
            cached_skin_module_fn("loading/loading_countdown", "draw", "__loading_countdown_draw");
        if (draw_countdown.valid()) {
            auto r = draw_countdown(std::max(0.0, countdown_ms - (get_current_ms() - start_ms)));
            if (!r.valid()) {
                sol::error err = r;
                spdlog::error("Lua error in loading_countdown.draw: {}", err.what());
            }
        }
    }

    draw_rectangle(0, 0, tex.screen_width, tex.screen_height, fade(Color::WHITE, fade_in->attribute));
    if (allnet_indicator) {
        allnet_indicator->draw();
    }
}
